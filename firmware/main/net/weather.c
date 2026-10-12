// Weather forecast from Open-Meteo (free, no API key) for where the van is: the GPS module's fix (io/gps.c), else
// the Starlink dish's when it reports one, otherwise the last fix or a location entered by hand. Kept in g_cfg.wx_*
// across reboots.
// The nearest town comes from OpenStreetMap's Nominatim, looked up only after a move (its policy: an identifying
// User-Agent and at most one request a second).
#include "weather.h"
#include "app/model.h"
#include "app/settings.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "weather";
#define EVERY_US     (30 * 60 * 1000000LL)
#define RETRY_US     (2 * 60 * 1000000LL)
#define SAVE_MOVE_KM 1.0f      // store a new GPS/Starlink fix once the van has moved this far (spares the flash)
#define GPS_HOLD_US  (60 * 1000000LL)   // keep using the GPS through a short loss of fix before falling back
#define REFETCH_KM   10.0f     // and fetch again straight away once it is this far from the forecast's location
#define BUF_CAP      (16 * 1024)
#define PLACE_KM       2.0f                      // look the town up again after moving this far
#define PLACE_RETRY_US (10 * 60 * 1000000LL)
#define PLACE_URL_FMT  "https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=13&accept-language=en" \
                       "&lat=%.3f&lon=%.3f"
#define URL_FMT "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f" \
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,is_day,wind_speed_10m" \
    "&hourly=temperature_2m,weather_code,precipitation_probability,is_day" \
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max," \
    "shortwave_radiation_sum,sunrise,sunset" \
    "&timezone=auto&timeformat=unixtime&forecast_days=" WX_DAYS_S "&forecast_hours=24"
#define STR_(x) #x
#define STR(x) STR_(x)
#define WX_DAYS_S STR(WX_DAYS)

static volatile bool s_kick;
static int64_t s_place_next;
static char s_ua[72];

static float dist_km(float la1, float lo1, float la2, float lo2)   // equirectangular: plenty at these distances
{
    float x = (lo2 - lo1) * cosf((la1 + la2) * 0.5f * (float)M_PI / 180), y = la2 - la1;
    return sqrtf(x * x + y * y) * 111.2f;
}

const char *weather_text(int c)
{
    switch (c) {
    case 0: return "Clear";            case 1: return "Mostly clear";     case 2: return "Partly cloudy";
    case 3: return "Overcast";         case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";     case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";      case 63: return "Rain";            case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";      case 73: return "Snow";            case 75: return "Heavy snow";
    case 77: return "Snow grains";     case 80: case 81: return "Showers"; case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";    case 96: case 99: return "Thunderstorm, hail";
    default: return "--";
    }
}

// Flat-mounted roof panels: irradiation (kWh/m2 on the horizontal) x array peak watts x the system efficiency for
// heat, wiring, MPPT and the charger cutting back once the battery is full (both set in Settings > Solar).
float weather_expected_kwh(float rad_kwh_m2)
{
    return rad_kwh_m2 * g_cfg.pv_w * g_cfg.pv_eff / 100.0f / 1000.0f;
}

bool weather_set_location(float lat, float lon)
{
    if (!(fabsf(lat) <= 90 && fabsf(lon) <= 180)) return false;
    model_lock();
    g_cfg.wx_lat = lat; g_cfg.wx_lon = lon; g_cfg.wx_src = WX_SRC_MANUAL;
    model_commit();
    settings_save_later();
    weather_refresh();
    return true;
}

void weather_refresh(void) { s_kick = true; }

bool weather_place(const char **name, const char **country)
{
    if (!g_cfg.wx_place[0] || g_cfg.wx_src == WX_SRC_NONE ||
        dist_km(g_cfg.wx_place_lat, g_cfg.wx_place_lon, g_cfg.wx_lat, g_cfg.wx_lon) > 2 * PLACE_KM) return false;
    *name = g_cfg.wx_place;
    *country = g_cfg.wx_country;
    return true;
}

static int http_get(const char *url, char *buf, int cap)
{
    esp_http_client_config_t c = { .url = url, .timeout_ms = 10000, .crt_bundle_attach = esp_crt_bundle_attach,
                                   .user_agent = s_ua };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    int n = -1;
    if (h && esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        int st = esp_http_client_get_status_code(h), r;
        n = 0;
        while (n < cap - 1 && (r = esp_http_client_read(h, buf + n, cap - 1 - n)) > 0) n += r;
        buf[n] = 0;
        if (st != 200) { ESP_LOGW(TAG, "HTTP %d: %.160s", st, buf); n = -1; }
        else if (n >= cap - 1) { ESP_LOGW(TAG, "reply too large"); n = -1; }
    }
    if (h) { esp_http_client_close(h); esp_http_client_cleanup(h); }
    return n;
}

static float num(const cJSON *o, const char *k) { const cJSON *x = cJSON_GetObjectItem(o, k); return cJSON_IsNumber(x) ? (float)x->valuedouble : NAN; }
static float at(const cJSON *o, const char *k, int i) { const cJSON *x = cJSON_GetArrayItem(cJSON_GetObjectItem(o, k), i); return cJSON_IsNumber(x) ? (float)x->valuedouble : NAN; }
static int64_t at_t(const cJSON *o, const char *k, int i) { const cJSON *x = cJSON_GetArrayItem(cJSON_GetObjectItem(o, k), i); return cJSON_IsNumber(x) ? (int64_t)x->valuedouble : 0; }
static uint8_t u8(float v) { return isnan(v) || v < 0 ? 0 : v > 255 ? 255 : (uint8_t)lroundf(v); }

static bool parse(const char *js, vd_weather_t *w)
{
    cJSON *root = cJSON_Parse(js);
    if (!root) return false;
    const cJSON *cur = cJSON_GetObjectItem(root, "current"), *hr = cJSON_GetObjectItem(root, "hourly"),
                *dl = cJSON_GetObjectItem(root, "daily");
    bool ok = cur && hr && dl;
    if (ok) {
        w->utc_off = (int32_t)num(root, "utc_offset_seconds");
        int64_t now = (int64_t)num(cur, "time");
        w->temp = num(cur, "temperature_2m");
        w->feels = num(cur, "apparent_temperature");
        w->wind_kmh = num(cur, "wind_speed_10m");
        w->humidity = u8(num(cur, "relative_humidity_2m"));
        w->code = u8(num(cur, "weather_code"));
        w->day = num(cur, "is_day") > 0;

        // Every third hour from the current one.
        int nh = cJSON_GetArraySize(cJSON_GetObjectItem(hr, "time")), i0 = 0;
        while (i0 < nh - 1 && at_t(hr, "time", i0) + 3600 <= now) i0++;
        w->n_hours = 0;
        for (int i = i0; i < nh && w->n_hours < WX_HOURS; i += 3) {
            vd_wx_hour_t *h = &w->hour[w->n_hours++];
            h->t = at_t(hr, "time", i);
            h->temp = at(hr, "temperature_2m", i);
            h->code = u8(at(hr, "weather_code", i));
            h->pop = u8(at(hr, "precipitation_probability", i));
            h->day = at(hr, "is_day", i) > 0;
        }
        int nd = cJSON_GetArraySize(cJSON_GetObjectItem(dl, "time"));
        w->n_days = 0;
        for (int i = 0; i < nd && w->n_days < WX_DAYS; i++) {
            vd_wx_day_t *d = &w->daily[w->n_days++];
            d->t = at_t(dl, "time", i);
            d->sunrise = at_t(dl, "sunrise", i);
            d->sunset = at_t(dl, "sunset", i);
            d->tmax = at(dl, "temperature_2m_max", i);
            d->tmin = at(dl, "temperature_2m_min", i);
            d->rad_kwh_m2 = at(dl, "shortwave_radiation_sum", i) / 3.6f;   // MJ/m2 -> kWh/m2
            d->code = u8(at(dl, "weather_code", i));
            d->pop = u8(at(dl, "precipitation_probability_max", i));
        }
        ok = !isnan(w->temp) && w->n_days > 0;
    }
    cJSON_Delete(root);
    return ok;
}

// The panel's fonts are ASCII only: fold Latin accents ("Kraków" -> "Krakow"). False if anything else (another
// script) had to be dropped, so the caller can try a different name.
static bool ascii_fold(const char *s, char *o, size_t n)
{
    static const char L1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";   // U+00C0..U+00FF
    static const char LA[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOo"
                             "RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";                      // U+0100..U+017F
    const uint8_t *p = (const uint8_t *)s;
    size_t k = 0;
    bool all = true;
    while (*p) {
        uint32_t c = *p++;
        if (c >= 0x80) {
            int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            if (!extra) continue;                      // stray continuation byte
            c &= 0x3F >> extra;
            for (int i = 0; i < extra && (*p & 0xC0) == 0x80; i++) c = c << 6 | (*p++ & 0x3F);
        }
        char one[2] = { 0 };
        const char *r = one;
        if (c < 0x80) one[0] = (char)c;
        else if (c == 0xC6) r = "AE"; else if (c == 0xE6) r = "ae"; else if (c == 0xDF) r = "ss";
        else if (c == 0x152) r = "OE"; else if (c == 0x153) r = "oe";
        else if (c >= 0xC0 && c < 0x100) one[0] = L1[c - 0xC0];
        else if (c >= 0x100 && c < 0x180) one[0] = LA[c - 0x100];
        else if (c == 0x2018 || c == 0x2019) one[0] = '\'';
        else if (c == 0x2013 || c == 0x2014) one[0] = '-';
        else all = false;
        while (*r && k + 1 < n) o[k++] = *r++;
    }
    o[k] = 0;
    return all;
}

// Town from a Nominatim reply: the most specific populated place it names. An "error" reply (open sea, nothing
// near) is a valid answer with no town.
static bool parse_place(const char *js, char *place, size_t pn, char *country, size_t cn)
{
    static const char *const KEYS[] = { "city", "town", "village", "hamlet", "municipality", "suburb", "county", "state" };
    cJSON *root = cJSON_Parse(js);
    if (!root) return false;
    const cJSON *a = cJSON_GetObjectItem(root, "address");
    bool ok = a || cJSON_GetObjectItem(root, "error");
    place[0] = country[0] = 0;
    for (size_t i = 0; a && i < sizeof KEYS / sizeof *KEYS && !place[0]; i++) {
        const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(a, KEYS[i]));
        if (s && !ascii_fold(s, place, pn)) place[0] = 0;
    }
    const char *c = a ? cJSON_GetStringValue(cJSON_GetObjectItem(a, "country")) : NULL;
    if (c && !ascii_fold(c, country, cn)) country[0] = 0;
    cJSON_Delete(root);
    return ok;
}

static void lookup_place(float lat, float lon, char *buf)
{
    char url[192], place[sizeof g_cfg.wx_place], country[sizeof g_cfg.wx_country];
    snprintf(url, sizeof url, PLACE_URL_FMT, lat, lon);
    bool ok = http_get(url, buf, BUF_CAP) > 0 && parse_place(buf, place, sizeof place, country, sizeof country);
    s_place_next = now_us() + (ok ? 60 * 1000000LL : PLACE_RETRY_US);
    if (!ok) { ESP_LOGW(TAG, "place lookup failed, retrying in 10 min"); return; }
    model_lock();
    strlcpy(g_cfg.wx_place, place, sizeof g_cfg.wx_place);
    strlcpy(g_cfg.wx_country, country, sizeof g_cfg.wx_country);
    g_cfg.wx_place_lat = lat; g_cfg.wx_place_lon = lon;
    model_commit();
    settings_save_later();
    ESP_LOGI(TAG, "place: %s, %s", place[0] ? place : "(none nearby)", country);
}

static void set_state(vd_wx_state_t s)
{
    model_lock();
    if (M.wx.state != s) { M.wx.state = s; model_commit(); } else model_unlock();
}

static void task(void *arg)
{
    char *buf = heap_caps_malloc(BUF_CAP, MALLOC_CAP_SPIRAM);
    vd_weather_t *w = heap_caps_malloc(sizeof *w, MALLOC_CAP_SPIRAM);
    char url[512];
    int64_t next = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        // Take the GPS fix, else the dish's, whenever there is one; store it only after a real move (switching
        // between the two live sources alone isn't worth a flash write).
        bool save = false;
        model_lock();
        bool gps = !M.gps.demo && M.gps.fix_at_us && now_us() - M.gps.fix_at_us < GPS_HOLD_US;
        if (gps || M.sl.loc_ok) {
            float la = gps ? (float)M.gps.lat : M.sl.lat, lo = gps ? (float)M.gps.lon : M.sl.lon;
            bool live = g_cfg.wx_src == WX_SRC_GPS || g_cfg.wx_src == WX_SRC_STARLINK;
            g_cfg.wx_src = gps ? WX_SRC_GPS : WX_SRC_STARLINK;
            if (!live || dist_km(g_cfg.wx_lat, g_cfg.wx_lon, la, lo) > SAVE_MOVE_KM) {
                g_cfg.wx_lat = la; g_cfg.wx_lon = lo;
                save = true;
            }
        }
        bool have = g_cfg.wx_src != WX_SRC_NONE, net = M.wifi.connected;
        float lat = g_cfg.wx_lat, lon = g_cfg.wx_lon;
        bool valid = M.wx.valid, moved = valid && dist_km(M.wx.lat, M.wx.lon, lat, lon) > REFETCH_KM;
        vd_wx_state_t st = M.wx.state;
        bool place_due = (!g_cfg.wx_place_lat && !g_cfg.wx_place_lon) ||
                         dist_km(g_cfg.wx_place_lat, g_cfg.wx_place_lon, lat, lon) > PLACE_KM;
        model_unlock();
        if (save) settings_save_later();

        if (!have) { set_state(WX_NO_LOC); continue; }
        if (!net) { set_state(WX_NO_NET); continue; }
        if (buf && valid && place_due && now_us() >= s_place_next) lookup_place(lat, lon, buf);   // forecast first
        if (!(s_kick || moved || now_us() >= next)) {
            if (st == WX_NO_LOC || st == WX_NO_NET) set_state(valid ? WX_OK : WX_ERROR);
            continue;
        }
        s_kick = false;
        set_state(WX_FETCHING);
        snprintf(url, sizeof url, URL_FMT, lat, lon);
        model_lock(); *w = M.wx; model_unlock();
        bool ok = buf && w && http_get(url, buf, BUF_CAP) > 0 && parse(buf, w);
        model_lock();
        if (ok) {
            M.wx = *w;
            M.wx.valid = true; M.wx.lat = lat; M.wx.lon = lon; M.wx.fetched = time(NULL);
        }
        M.wx.state = ok ? WX_OK : WX_ERROR;
        model_commit();
        next = now_us() + (ok ? EVERY_US : RETRY_US);
        if (ok) ESP_LOGI(TAG, "%.4f, %.4f: %.1f C, %s", lat, lon, w->temp, weather_text(w->code));
        else ESP_LOGW(TAG, "fetch failed, retrying in %lld s", RETRY_US / 1000000);
    }
}

void weather_init(void)
{
    snprintf(s_ua, sizeof s_ua, "VanDeck/%s (ESP32 van panel)", esp_app_get_description()->version);
    xTaskCreate(task, "weather", 10240, NULL, 3, NULL);
}

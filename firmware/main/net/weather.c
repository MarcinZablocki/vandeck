// Weather forecast from Open-Meteo (free, no API key) for where the van is: the Starlink dish's GPS fix when it
// reports one, otherwise the last fix or a location entered by hand. Both are kept in g_cfg.wx_* across reboots.
#include "weather.h"
#include "app/model.h"
#include "app/settings.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "weather";
#define EVERY_US     (30 * 60 * 1000000LL)
#define RETRY_US     (2 * 60 * 1000000LL)
#define SAVE_MOVE_KM 1.0f      // store a new Starlink fix once the van has moved this far (spares the flash)
#define REFETCH_KM   10.0f     // and fetch again straight away once it is this far from the forecast's location
#define BUF_CAP      (16 * 1024)
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

// Flat-mounted roof panels: irradiation (kWh/m2 on the horizontal) x array peak watts x a performance ratio for
// heat, wiring, MPPT and the charger cutting back once the battery is full.
float weather_expected_kwh(float rad_kwh_m2)
{
    return rad_kwh_m2 * CONFIG_VD_PV_ARRAY_W * CONFIG_VD_PV_PERF_PCT / 100.0f / 1000.0f;
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

static int http_get(const char *url, char *buf, int cap)
{
    esp_http_client_config_t c = { .url = url, .timeout_ms = 10000, .crt_bundle_attach = esp_crt_bundle_attach };
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
        // Take the dish's fix whenever it has one; store it only after a real move.
        bool save = false;
        model_lock();
        if (M.sl.loc_ok && (g_cfg.wx_src != WX_SRC_STARLINK ||
                            dist_km(g_cfg.wx_lat, g_cfg.wx_lon, M.sl.lat, M.sl.lon) > SAVE_MOVE_KM)) {
            g_cfg.wx_lat = M.sl.lat; g_cfg.wx_lon = M.sl.lon; g_cfg.wx_src = WX_SRC_STARLINK;
            save = true;
        }
        bool have = g_cfg.wx_src != WX_SRC_NONE, net = M.wifi.connected;
        float lat = g_cfg.wx_lat, lon = g_cfg.wx_lon;
        bool valid = M.wx.valid, moved = valid && dist_km(M.wx.lat, M.wx.lon, lat, lon) > REFETCH_KM;
        vd_wx_state_t st = M.wx.state;
        model_unlock();
        if (save) settings_save_later();

        if (!have) { set_state(WX_NO_LOC); continue; }
        if (!net) { set_state(WX_NO_NET); continue; }
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

void weather_init(void) { xTaskCreate(task, "weather", 10240, NULL, 3, NULL); }

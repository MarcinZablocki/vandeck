// Desktop stand-ins for the services the UI calls: settings without NVS, no radios, and canned data
// for the parts of the model app/sim.c doesn't fill (heater, tanks, weather, Wi-Fi).
#include "app/settings.h"
#include "app/model.h"
#include "board/board.h"
#include "ble/ble_svc.h"
#include "net/ota_svc.h"
#include "net/starlink.h"
#include "net/time_svc.h"
#include "net/update_svc.h"
#include "net/matter_svc.h"
#include "net/weather.h"
#include "net/wifi_svc.h"
#include "io/tanks.h"
#include "io/truma_lin.h"
#include <string.h>
#include <time.h>

// ---- ESP-IDF bits ----------------------------------------------------------------------------------------------
const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t d = { "sim", "vandeck", __TIME__, __DATE__ };
    return &d;
}
const esp_partition_t *esp_ota_get_running_partition(void) { static const esp_partition_t p = { "sim" }; return &p; }
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *k) { return (esp_netif_t *)1; }
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ip) { ip->ip.addr = 0x2A00A8C0; return ESP_OK; }   // 192.168.0.42

// ---- settings (in memory) --------------------------------------------------------------------------------------
vd_settings_t g_cfg;

void settings_load(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.magic = VD_CFG_MAGIC; g_cfg.ver = VD_CFG_VER;
    g_cfg.wifi_on = true; g_cfg.auto_time = true; g_cfg.tz = 5;
    g_cfg.heater_on = true; g_cfg.setpoint_c = 21.0f; g_cfg.water = WATER_ECO; g_cfg.fan = FAN_ECO;
    g_cfg.tank_show_sec = 30;
    g_cfg.screen_timeout = SCREEN_TO_NEVER; g_cfg.brightness = 80;
    g_cfg.pv_w = CONFIG_VD_PV_ARRAY_W; g_cfg.pv_eff = CONFIG_VD_PV_PERF_PCT;
    g_cfg.upd_mode = UPD_MODE_NOTIFY;
    g_cfg.wx_lat = 46.56f; g_cfg.wx_lon = 7.98f; g_cfg.wx_src = WX_SRC_MANUAL;
    strcpy(g_cfg.wx_place, "Lauterbrunnen"); strcpy(g_cfg.wx_country, "Switzerland");
    g_cfg.wx_place_lat = g_cfg.wx_lat; g_cfg.wx_place_lon = g_cfg.wx_lon;
}
void settings_save_later(void) {}
int settings_screen_timeout_ms(void)
{
    static const int MS[] = { 30000, 60000, 300000, 0 };
    return g_cfg.screen_timeout <= SCREEN_TO_NEVER ? MS[g_cfg.screen_timeout] : 0;
}

// ---- canned data -----------------------------------------------------------------------------------------------
void sim_fill_static(void)
{
    model_lock();
    M.time_synced = true;
    M.heater = (vd_heater_t){ .link = true, .registered = true, .heating = true, .room_c = 19.4f };
    M.tank[0] = (vd_tank_t){ .level = 2 };
    M.tank[1] = (vd_tank_t){ .level = 1 };
    M.wifi.connected = true;
    strcpy(M.wifi.nets[0].ssid, "Starlink"); M.wifi.nets[0].rssi = -48; M.wifi.nets[0].secure = true; M.wifi.n_nets = 1;
    M.sl.power = true; M.sl.power_since_us = now_us() - 60LL * 1000000;

    vd_weather_t *w = &M.wx;
    time_t now = time(NULL);
    w->state = WX_OK; w->valid = true; w->lat = g_cfg.wx_lat; w->lon = g_cfg.wx_lon;
    w->utc_off = 7200; w->fetched = now - 600;
    w->temp = 14.2f; w->feels = 12.8f; w->wind_kmh = 11; w->code = 2; w->humidity = 64; w->day = true;
    static const uint8_t hc[] = { 2, 2, 3, 61, 61, 3, 1, 0 };
    int64_t h0 = now - now % 10800;
    for (int i = 0; i < WX_HOURS; i++)
        w->hour[i] = (vd_wx_hour_t){ h0 + i * 10800, 14 - i * 1.3f + (i > 4 ? (i - 4) * 2.5f : 0), hc[i], i == 3 || i == 4 ? 70 : 10, i < 3 || i > 6 };
    w->n_hours = WX_HOURS;
    static const uint8_t dc[] = { 2, 61, 3, 0, 1 };
    int64_t d0 = now - (now + w->utc_off) % 86400;
    for (int i = 0; i < WX_DAYS; i++)
        w->daily[i] = (vd_wx_day_t){ d0 + i * 86400, d0 + i * 86400 + 7 * 3600, d0 + i * 86400 + 19 * 3600,
                                     16 + i, 6 + i * 0.5f, 4.5f - (dc[i] == 61) * 2.5f, dc[i], dc[i] == 61 ? 80 : 5 };
    w->n_days = WX_DAYS;
    model_commit();
}

// ---- services: no-ops ------------------------------------------------------------------------------------------
lv_display_t *board_init(void) { return NULL; }
void board_set_backlight(uint8_t pct) {}

void ble_svc_init(void) {}
void ble_pair_begin(vd_role_t role) {}
void ble_pair_cancel(void) {}
void ble_pair_select(int idx, const uint8_t key[16], const char *pin) {}
void ble_forget(vd_role_t role) {}
void ble_svc_scan_pause(void) {}
void ble_svc_scan_resume(void) {}
void ble_svc_dump(char *buf, size_t n) { snprintf(buf, n, "sim"); }
void ble_svc_stats_reset(void) {}

ota_install_t ota_svc_last_install(void) { return (ota_install_t){ .at = 0, .via_ota = false, .known = true }; }

void starlink_set_power(bool on) { model_lock(); M.sl.power = on; M.sl.power_since_us = now_us(); model_commit(); }
void starlink_reboot(void) {}
void starlink_set_snow(uint8_t mode) { model_lock(); g_cfg.sl_snow = mode; model_commit(); }

// Matter: unpaired, with CHIP's documentation example code, so Settings > Apple Home shows its QR layout.
vd_mt_state_t matter_svc_state(void) { return MT_UNPAIRED; }
const char *matter_svc_state_text(vd_mt_state_t st)
{
    static const char *T[] = { "Off", "Starting", "Ready to pair", "Paired", "Error" };
    return st <= MT_ERROR ? T[st] : "?";
}
uint8_t matter_svc_fabrics(void) { return 0; }
bool matter_svc_qr(char *buf, size_t n) { snprintf(buf, n, "MT:Y.K9042C00KA0648G00"); return true; }
bool matter_svc_manual_code(char *buf, size_t n) { snprintf(buf, n, "3497-011-2332"); return true; }
void matter_svc_reset(void) {}

const vd_zone_t VD_ZONES[] = {
    { "Pacific", "" }, { "Mountain", "" }, { "Central", "" }, { "Eastern", "" },
    { "London", "" }, { "Central Europe", "" }, { "Eastern Europe", "" }, { "Sydney", "" },
};
const int VD_ZONE_COUNT = sizeof VD_ZONES / sizeof VD_ZONES[0];
void time_svc_apply_tz(void) {}
void time_svc_set_auto(bool on) { g_cfg.auto_time = on; }
void time_svc_shift(int field, int delta) {}
bool time_valid(void) { return true; }
int time_utc_offset_min(void)
{
    time_t t = time(NULL);
    return (int)(localtime(&t)->tm_gmtoff / 60);
}

bool update_svc_enabled(void) { return true; }
void update_svc_check(void) {}
void update_svc_install(void) {}

void weather_refresh(void) {}
bool weather_set_location(float lat, float lon) { g_cfg.wx_lat = lat; g_cfg.wx_lon = lon; return true; }
const char *weather_text(int c)
{
    switch (c) {
    case 0: return "Clear";            case 1: return "Mostly clear";     case 2: return "Partly cloudy";
    case 3: return "Overcast";         case 45: case 48: return "Fog";
    case 61: return "Light rain";      case 63: return "Rain";            case 65: return "Heavy rain";
    case 71: return "Light snow";      case 73: return "Snow";            case 95: return "Thunderstorm";
    default: return "--";
    }
}
float weather_expected_kwh(float rad_kwh_m2) { return rad_kwh_m2 * g_cfg.pv_w * g_cfg.pv_eff / 100.0f / 1000.0f; }
bool weather_place(const char **name, const char **country)
{
    if (!g_cfg.wx_place[0]) return false;
    *name = g_cfg.wx_place; *country = g_cfg.wx_country;
    return true;
}

void wifi_svc_set_enabled(bool on) { model_lock(); g_cfg.wifi_on = on; M.wifi.connected = on; model_commit(); }
void wifi_svc_scan(void) {}
void wifi_svc_connect(const char *ssid, const char *pass) {}

void tanks_read(int tank) {}

void truma_set_power(bool on) { model_lock(); g_cfg.heater_on = on; M.heater.heating = on; model_commit(); }
void truma_set_setpoint(float c) { model_lock(); g_cfg.setpoint_c = c; model_commit(); }
void truma_set_water(uint8_t mode) { model_lock(); g_cfg.water = mode; model_commit(); }
void truma_set_fan(uint8_t mode) { model_lock(); g_cfg.fan = mode; model_commit(); }

// ---- solar log: two weeks of made-up days ----------------------------------------------------------------------
#include "app/solar_log.h"
int32_t solar_log_today(void) { time_t t = time(NULL); return (int32_t)((t + localtime(&t)->tm_gmtoff) / 86400); }
int32_t solar_log_first_day(void) { return solar_log_today() - 13; }
bool solar_log_get(int32_t day, vd_day_t *out)
{
    int k = solar_log_today() - day;
    if (k < 0 || k > 13) return false;
    static const uint16_t Y[] = { 640, 910, 1180, 1020, 450, 300, 870, 1240, 1310, 1150, 980, 720, 1090, 1200 };
    *out = (vd_day_t){ .day = (uint32_t)day, .yield_wh = Y[k], .max_w = Y[k] / 5, .max_v_x10 = 412,
                       .min_bulk = 300, .min_abs = 120, .min_float = 200 };
    return true;
}
void civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)yoe + era * 400 + (*m <= 2);
}

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <math.h>

typedef enum { ROLE_SOLAR, ROLE_BATT1, ROLE_BATT2, ROLE_SHELLY, ROLE_COUNT } vd_role_t;
typedef enum { DEVT_VICTRON, DEVT_LITIME, DEVT_SHELLY } vd_devtype_t;
typedef enum { DEV_NONE, DEV_PAIRING, DEV_ON, DEV_LOST } vd_devstate_t;
typedef enum { STAGE_OFF, STAGE_BULK, STAGE_ABS, STAGE_FLOAT, STAGE_OTHER } vd_stage_t;
enum { WATER_OFF, WATER_ECO, WATER_HOT };
enum { FAN_ECO, FAN_HIGH, FAN_VENT };
enum { SNOW_AUTO, SNOW_ON, SNOW_OFF };
enum { SL_DOWN, SL_UP, SL_LAT, SL_PWR, SL_PING, SL_OBS, SL_NHIST };

#define SL_HIST_LEN    24
#define WIFI_MAX_NETS  8
#define PAIR_MAX_CANDS 6

// mode = Victron operation mode as broadcast (0 off, 3 bulk, 4 absorption, 5 float, 245 starting up, ...);
// load_a = load output current, NAN when the charger has no load output.
typedef struct { bool valid; int64_t ts_us; float pv_w, pv_v, batt_v, batt_a, load_a; uint16_t yield_wh; vd_stage_t stage; uint8_t mode, err; } vd_solar_t;
typedef struct { bool valid, stale; int64_t ts_us; float soc, volts, amps, remain_ah, full_ah, temp_c; } vd_batt_t;   // full_ah = BMS full-charge capacity
// Requested values live in g_cfg (mirrored from the CP Plus in iNet box mode). registered = the CP Plus talks to us.
typedef struct { bool link, registered; bool heating; float room_c; uint16_t err; } vd_heater_t;
typedef struct { int8_t level; bool reading; int64_t read_at_us; } vd_tank_t;           // level -1 unknown, 0..3 = E..F
typedef struct {
    bool power; int64_t power_since_us;
    bool reachable; int64_t online_since_us; bool heating;
    float v[SL_NHIST];                       // NAN = not available
    float hist[SL_NHIST][SL_HIST_LEN]; uint8_t hist_n;
    // Dish GPS position (get_location). loc_denied = the dish refused: "allow local access" is off in the Starlink app.
    bool loc_ok, loc_denied; float lat, lon; int64_t loc_at_us;
} vd_starlink_t;
// Open-Meteo forecast. Times are unix seconds; utc_off is the forecast location's UTC offset, used for day/hour labels.
#define WX_HOURS 8             // every 3 h over the next 24 h
#define WX_DAYS  5
typedef enum { WX_NO_LOC, WX_NO_NET, WX_FETCHING, WX_OK, WX_ERROR } vd_wx_state_t;
typedef struct { int64_t t; float temp; uint8_t code, pop; bool day; } vd_wx_hour_t;
typedef struct { int64_t t, sunrise, sunset; float tmax, tmin, rad_kwh_m2; uint8_t code, pop; } vd_wx_day_t;
typedef struct {
    vd_wx_state_t state; bool valid;          // valid = a forecast is held (it may be old when state is an error)
    float lat, lon; int32_t utc_off; int64_t fetched;
    float temp, feels, wind_kmh; uint8_t code, humidity; bool day;
    vd_wx_hour_t hour[WX_HOURS]; vd_wx_day_t daily[WX_DAYS]; uint8_t n_hours, n_days;
} vd_weather_t;
typedef struct { char ssid[33]; int8_t rssi; bool secure; } vd_net_t;
typedef struct { bool connected, scanning, failed; char joining[33]; vd_net_t nets[WIFI_MAX_NETS]; uint8_t n_nets; } vd_wifi_t;
typedef struct { char name[24]; uint8_t addr[6]; uint8_t addr_type; int8_t rssi; } vd_cand_t;
typedef enum { OTA_IDLE, OTA_RECEIVING, OTA_DONE, OTA_FAILED } vd_ota_state_t;
typedef struct { vd_ota_state_t state; uint8_t pct; char version[32]; char msg[48]; } vd_ota_t;
typedef struct { bool active, scanning; uint8_t role; vd_cand_t c[PAIR_MAX_CANDS]; uint8_t n; } vd_pair_t;
// Tailnet (Tailscale) link. ip is the panel's 100.x address in host byte order, 0 until registered.
typedef enum { TN_OFF, TN_NEED_KEY, TN_WAIT_WIFI, TN_CONNECTING, TN_CONNECTED, TN_AUTH_FAILED, TN_ERROR } vd_tn_state_t;
typedef struct { vd_tn_state_t state; bool has_key, has_creds; uint32_t ip; uint8_t peers; } vd_tailnet_t;

typedef struct {
    vd_solar_t solar;
    vd_batt_t batt[2];
    vd_heater_t heater;
    vd_tank_t tank[2];
    vd_starlink_t sl;
    vd_weather_t wx;
    vd_wifi_t wifi;
    uint8_t dev_state[ROLE_COUNT];
    vd_pair_t pair;
    vd_ota_t ota;
    vd_tailnet_t tn;
    bool time_synced;
    bool demo;
    uint32_t version;          // bumped on every change; the UI redraws when it moves
} vd_model_t;

extern vd_model_t M;           // shared — only touch between model_lock() and model_unlock()/model_commit()

void model_init(void);
void model_lock(void);
void model_unlock(void);       // release without signalling a change
void model_commit(void);       // bump version and release
void model_snapshot(vd_model_t *out);
int64_t now_us(void);
vd_devtype_t role_type(vd_role_t r);
const char *role_name(vd_role_t r);

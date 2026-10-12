#pragma once
#include "model.h"

#define VD_CFG_MAGIC 0x56444B31
#define VD_CFG_VER   6   // 2: screen_timeout, brightness; 3: weather location; 4: solar array; 5: place name; 6: updates
                         // (appended; older blobs still load)

typedef struct {
    bool paired;
    uint8_t addr[6]; uint8_t addr_type;
    char name[24];
    uint8_t key[16];           // Victron instant-readout key
    char pin[33];              // Victron BLE password (for a future GATT/history link)
} vd_dev_cfg_t;

typedef struct {
    uint32_t magic; uint16_t ver;
    bool wifi_on; char wifi_ssid[33];
    bool auto_time; uint8_t tz; bool h12; bool unit_f;
    int8_t room_offset_x10;
    bool heater_on; float setpoint_c; uint8_t water, fan;
    uint16_t sl_auto_min; uint8_t sl_snow;
    uint16_t tank_show_sec;
    vd_dev_cfg_t dev[ROLE_COUNT];
    // v2 (new fields go at the end so older blobs keep loading)
    uint8_t screen_timeout;    // SCREEN_TO_*
    uint8_t brightness;        // backlight %, 10..100
    // v3
    float wx_lat, wx_lon;      // weather location: the last GPS or Starlink fix, or one entered by hand
    uint8_t wx_src;            // WX_SRC_*
    // v4
    uint16_t pv_w;             // solar array peak power, for the forecast's expected yield
    uint8_t pv_eff;            // % of that rating that reaches the battery
    // v5: nearest place to the weather location (OpenStreetMap Nominatim), ASCII; "" = none found / not looked up
    char wx_place[40], wx_country[32];
    float wx_place_lat, wx_place_lon;   // where it was looked up for
    // v6
    uint8_t upd_mode;          // UPD_MODE_*: firmware updates from GitHub releases
} vd_settings_t;

enum { UPD_MODE_OFF, UPD_MODE_NOTIFY, UPD_MODE_AUTO };

enum { WX_SRC_NONE, WX_SRC_STARLINK, WX_SRC_MANUAL, WX_SRC_GPS };   // stored: append only

enum { SCREEN_TO_30S, SCREEN_TO_1M, SCREEN_TO_5M, SCREEN_TO_NEVER };
int settings_screen_timeout_ms(void);   // 0 = never

extern vd_settings_t g_cfg;    // write under model_lock(), then settings_save_later()

void settings_load(void);
void settings_save_later(void);   // debounced NVS write (2 s)

#pragma once
#include "lvgl.h"
#include "app/model.h"

lv_obj_t *ui_energy_create(lv_obj_t *parent);
void ui_energy_refresh(const vd_model_t *m);
void ui_energy_track(const vd_model_t *m);       // every UI tick, any page: keeps the bank current average
lv_obj_t *ui_solar_create(lv_obj_t *parent);
void ui_solar_refresh(const vd_model_t *m);
lv_obj_t *ui_tanks_create(lv_obj_t *parent);
void ui_tanks_refresh(const vd_model_t *m);
lv_obj_t *ui_starlink_create(lv_obj_t *parent);
void ui_starlink_refresh(const vd_model_t *m);
lv_obj_t *ui_weather_create(lv_obj_t *parent);
void ui_weather_refresh(const vd_model_t *m);

enum { V_ENERGY, V_SOLAR, V_TANKS, V_STARLINK, V_WEATHER, V_COUNT };
void ui_show_view(int v);
const char *ui_wx_icon(int code, bool day);      // WMO code -> glyph in F_ICON32 / F_ICON80
uint32_t ui_wx_color(int code, bool day);
void ui_fmt_deg(char *b, size_t n, float c);     // "14°" in the chosen unit

void ui_settings_create(void);
void ui_settings_open(void);
void ui_settings_refresh(const vd_model_t *m);

void ui_entry_create(void);
void ui_entry_open_wifi(const char *ssid);
void ui_entry_open_victron(int cand_idx);
void ui_entry_open_tailnet_key(void);
void ui_entry_open_location(void);

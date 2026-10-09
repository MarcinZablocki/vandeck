#pragma once
#include "lvgl.h"
#include "app/model.h"

lv_obj_t *ui_energy_create(lv_obj_t *parent);
void ui_energy_refresh(const vd_model_t *m);
lv_obj_t *ui_solar_create(lv_obj_t *parent);
void ui_solar_refresh(const vd_model_t *m);
lv_obj_t *ui_tanks_create(lv_obj_t *parent);
void ui_tanks_refresh(const vd_model_t *m);
lv_obj_t *ui_starlink_create(lv_obj_t *parent);
void ui_starlink_refresh(const vd_model_t *m);

void ui_settings_create(void);
void ui_settings_open(void);
void ui_settings_refresh(const vd_model_t *m);

void ui_entry_create(void);
void ui_entry_open_wifi(const char *ssid);
void ui_entry_open_victron(int cand_idx);
void ui_entry_open_tailnet_key(void);

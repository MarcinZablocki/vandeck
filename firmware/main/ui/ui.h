#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "app/model.h"

void ui_init(void);                      // call with the LVGL port lock held
void ui_splash_show(void);               // full-screen logo on the top layer (LVGL lock held)
void ui_splash_hide(void);               // fades it out once it has been up long enough (LVGL lock held)
bool ui_screen_is_off(void);
void ui_screen_sleep(void);              // backlight off now, as the screen saver would (LVGL lock held)
uint32_t ui_idle_ms(void);               // time since the panel was last touched
void ui_screen_wake(void);               // wake from the screen saver and restart its idle timer (LVGL lock held)
const vd_model_t *ui_model(void);        // latest snapshot (LVGL task only)
void ui_request_refresh(void);
void ui_fmt_temp(char *buf, size_t n, float c);   // "21.0" or "70" depending on unit
const char *ui_temp_unit(void);          // "°C" / "°F"

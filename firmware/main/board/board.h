#pragma once
#include <stdint.h>
#include "lvgl.h"

lv_display_t *board_init(void);          // display + touch + LVGL port
void board_set_backlight(uint8_t pct);

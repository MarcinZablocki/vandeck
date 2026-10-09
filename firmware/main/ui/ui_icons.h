#pragma once
// Material Design Icons, remapped to U+E000.. by tools/gen_icons.sh (keep the order in step with it).
// vd_icons_32 has every glyph; vd_icons_80 only the weather ones.
#include "lvgl.h"

LV_FONT_DECLARE(vd_icons_32)
LV_FONT_DECLARE(vd_icons_80)
#define F_ICON32 (&vd_icons_32)
#define F_ICON80 (&vd_icons_80)

#define IC_ENERGY        "\xEE\x80\x80"
#define IC_SOLAR         "\xEE\x80\x81"
#define IC_TANKS         "\xEE\x80\x82"
#define IC_STARLINK      "\xEE\x80\x83"
#define IC_SETTINGS      "\xEE\x80\x84"
#define IC_WX_SUN        "\xEE\x80\x85"
#define IC_WX_MOON       "\xEE\x80\x86"
#define IC_WX_PARTLY     "\xEE\x80\x87"
#define IC_WX_PARTLY_N   "\xEE\x80\x88"
#define IC_WX_CLOUD      "\xEE\x80\x89"
#define IC_WX_FOG        "\xEE\x80\x8A"
#define IC_WX_RAIN       "\xEE\x80\x8B"
#define IC_WX_POUR       "\xEE\x80\x8C"
#define IC_WX_SNOW       "\xEE\x80\x8D"
#define IC_WX_SNOW_HEAVY "\xEE\x80\x8E"
#define IC_WX_SLEET      "\xEE\x80\x8F"
#define IC_WX_STORM      "\xEE\x80\x90"
#define IC_WX_HAIL       "\xEE\x80\x91"
#define IC_PIN           "\xEE\x80\x92"
#define IC_UMBRELLA      "\xEE\x80\x93"
#define IC_WIND          "\xEE\x80\x94"
#define IC_HUMIDITY      "\xEE\x80\x95"
#define IC_SUNRISE       "\xEE\x80\x96"
#define IC_SUNSET        "\xEE\x80\x97"

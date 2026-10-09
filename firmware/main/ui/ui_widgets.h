#pragma once
#include "lvgl.h"
#include <stdbool.h>

// Palette (matches the Van Panel design)
#define C_BG        0x121314
#define C_CARD      0x1C1D1F
#define C_INSET     0x26272A
#define C_BTN       0x323337
#define C_BTN2      0x3D3E42
#define C_LINE      0x2E2F33
#define C_TXT       0xECEDEE
#define C_TXT2      0xB4B7BB
#define C_MUTED     0x8B8E93
#define C_FAINT     0x5A5C60
#define C_AMBER     0xEBAF4F
#define C_AMBER_DIM 0x7A6232
#define C_GREEN     0x74D08C
#define C_ORANGE    0xF39F6B
#define C_RED       0xEC7C74
#define C_BLUE      0x72C2E6
#define HEX(c) lv_color_hex(c)

// Compact layout for small panels (800x480 class): chosen once from the display size in ui_init(), before any
// page is built. SZ(full, compact) picks a size or font; at 1024x600 everything uses the full value.
extern bool g_compact;
#define SZ(full, compact) (g_compact ? (compact) : (full))

LV_IMAGE_DECLARE(img_logo_640);          // VanDeck wordmark, alpha-only: tinted with w_tinted_image()
LV_IMAGE_DECLARE(img_logo_160);
lv_obj_t *w_tinted_image(lv_obj_t *p, const lv_image_dsc_t *img, uint32_t color);

#define F_14 (&lv_font_montserrat_14)
#define F_16 (&lv_font_montserrat_16)
#define F_18 (&lv_font_montserrat_18)
#define F_20 (&lv_font_montserrat_20)
#define F_22 (&lv_font_montserrat_22)
#define F_24 (&lv_font_montserrat_24)
#define F_26 (&lv_font_montserrat_26)
#define F_28 (&lv_font_montserrat_28)
#define F_30 (&lv_font_montserrat_30)
#define F_36 (&lv_font_montserrat_36)
#define F_40 (&lv_font_montserrat_40)
#define F_48 (&lv_font_montserrat_48)
#if CONFIG_VD_BIG_FONTS
LV_FONT_DECLARE(vd_num_64)
LV_FONT_DECLARE(vd_num_92)
LV_FONT_DECLARE(vd_num_110)
#define F_NUM64  (&vd_num_64)
#define F_NUM92  (&vd_num_92)
#define F_NUM110 (&vd_num_110)
#else
#define F_NUM64  F_48
#define F_NUM92  F_48
#define F_NUM110 F_48
#endif

lv_obj_t *w_box(lv_obj_t *p);                              // bare container, no style/scroll
lv_obj_t *w_rect(lv_obj_t *p, uint32_t bg, int radius);
lv_obj_t *w_row(lv_obj_t *p, int gap);
lv_obj_t *w_col(lv_obj_t *p, int gap);
lv_obj_t *w_card(lv_obj_t *p);
lv_obj_t *w_spacer(lv_obj_t *p);
lv_obj_t *w_label(lv_obj_t *p, const lv_font_t *f, uint32_t color, const char *txt);
lv_obj_t *w_dot(lv_obj_t *p, int size, uint32_t color);
lv_obj_t *w_header(lv_obj_t *p, const char *title, const char *sub, lv_obj_t **sub_out);
lv_obj_t *w_kv(lv_obj_t *p, const char *key, const char *unit);   // returns value label
lv_obj_t *w_btn(lv_obj_t *p, const char *txt, const lv_font_t *f, int h, bool primary, lv_event_cb_t cb, void *ud);
void w_btn_state(lv_obj_t *b, bool primary, bool enabled);
void w_btn_text(lv_obj_t *b, const char *txt);
lv_obj_t *w_seg(lv_obj_t *p, const char *const *map, int h, const lv_font_t *f, lv_event_cb_t cb, void *ud);
void w_seg_set(lv_obj_t *seg, int idx);
int w_seg_get(lv_event_t *e);
lv_obj_t *w_switch(lv_obj_t *p, lv_event_cb_t cb, void *ud);
void w_switch_set(lv_obj_t *sw, bool on, uint32_t on_color);
void w_fmt(lv_obj_t *lbl, const char *fmt, ...);
void w_color(lv_obj_t *o, uint32_t c);
void w_bg(lv_obj_t *o, uint32_t c);
void w_show(lv_obj_t *o, bool show);
void w_enable(lv_obj_t *o, bool en);

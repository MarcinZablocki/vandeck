#include "ui_widgets.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Content-sized boxes must keep START main-axis flex alignment: LVGL measures LV_SIZE_CONTENT from the box's
// left/top edge, so END/CENTER-packed children overhang that edge, aren't counted, and get clipped.
// Position the box from its parent instead.
lv_obj_t *w_box(lv_obj_t *p)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return o;
}

lv_obj_t *w_rect(lv_obj_t *p, uint32_t bg, int radius)
{
    lv_obj_t *o = w_box(p);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, HEX(bg), 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

lv_obj_t *w_row(lv_obj_t *p, int gap)
{
    lv_obj_t *o = w_box(p);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    return o;
}

lv_obj_t *w_col(lv_obj_t *p, int gap)
{
    lv_obj_t *o = w_box(p);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

lv_obj_t *w_card(lv_obj_t *p)
{
    lv_obj_t *o = w_rect(p, C_CARD, 24);
    lv_obj_set_style_pad_all(o, 24, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, 0, 0);
    return o;
}

lv_obj_t *w_spacer(lv_obj_t *p) { lv_obj_t *o = w_box(p); lv_obj_set_flex_grow(o, 1); return o; }

lv_obj_t *w_tinted_image(lv_obj_t *p, const lv_image_dsc_t *img, uint32_t color)
{
    lv_obj_t *o = lv_image_create(p);
    lv_image_set_src(o, img);
    lv_obj_set_style_image_recolor(o, HEX(color), 0);   // A8 images take their colour from the recolor style
    lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
    return o;
}

lv_obj_t *w_label(lv_obj_t *p, const lv_font_t *f, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, HEX(color), 0);
    lv_label_set_text(l, txt ? txt : "");
    return l;
}

lv_obj_t *w_dot(lv_obj_t *p, int size, uint32_t color)
{
    lv_obj_t *o = w_rect(p, color, LV_RADIUS_CIRCLE);
    lv_obj_set_size(o, size, size);
    return o;
}

lv_obj_t *w_header(lv_obj_t *p, const char *title, const char *sub, lv_obj_t **sub_out)
{
    lv_obj_t *r = w_row(p, 12);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *c = w_col(r, 2);
    w_label(c, F_20, C_TXT, title);
    lv_obj_t *s = w_label(c, F_14, C_MUTED, sub);
    if (sub_out) *sub_out = s;
    return r;
}

lv_obj_t *w_kv(lv_obj_t *p, const char *key, const char *unit)
{
    lv_obj_t *r = w_row(p, 8);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    w_label(r, F_16, C_MUTED, key);
    lv_obj_t *v = w_row(r, 6);
    lv_obj_set_flex_align(v, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_t *val = w_label(v, F_26, C_TXT, "--");
    w_label(v, F_16, C_MUTED, unit);
    return val;
}

lv_obj_t *w_btn(lv_obj_t *p, const char *txt, const lv_font_t *f, int h, bool primary, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_pad_hor(b, 20, 0);
    lv_obj_set_style_bg_color(b, HEX(C_BTN2), LV_STATE_PRESSED);
    lv_obj_set_size(b, LV_SIZE_CONTENT, h);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, f, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    w_btn_state(b, primary, true);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void w_btn_state(lv_obj_t *b, bool primary, bool enabled)
{
    w_bg(b, enabled && primary ? C_TXT : C_INSET);
    w_color(lv_obj_get_child(b, 0), !enabled ? C_FAINT : primary ? C_BG : C_TXT);
    w_enable(b, enabled);
}

void w_btn_text(lv_obj_t *b, const char *txt) { lv_label_set_text(lv_obj_get_child(b, 0), txt); }

lv_obj_t *w_seg(lv_obj_t *p, const char *const *map, int h, const lv_font_t *f, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *bm = lv_buttonmatrix_create(p);
    lv_obj_remove_style_all(bm);
    lv_buttonmatrix_set_map(bm, map);
    lv_buttonmatrix_set_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(bm, true);
    lv_obj_set_size(bm, LV_PCT(100), h);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bm, HEX(C_INSET), 0);
    lv_obj_set_style_radius(bm, 18, 0);
    lv_obj_set_style_pad_all(bm, 6, 0);
    lv_obj_set_style_pad_gap(bm, 6, 0);
    lv_obj_set_style_bg_opa(bm, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, HEX(C_INSET), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, HEX(C_BTN), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(bm, HEX(C_TXT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bm, HEX(C_TXT2), LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, HEX(C_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(bm, f, LV_PART_ITEMS);
    lv_obj_set_style_radius(bm, 13, LV_PART_ITEMS);
    if (cb) lv_obj_add_event_cb(bm, cb, LV_EVENT_VALUE_CHANGED, ud);
    return bm;
}

void w_seg_set(lv_obj_t *bm, int idx)
{
    if ((intptr_t)lv_obj_get_user_data(bm) == idx + 1) return;
    lv_obj_set_user_data(bm, (void *)(intptr_t)(idx + 1));
    lv_buttonmatrix_clear_button_ctrl_all(bm, LV_BUTTONMATRIX_CTRL_CHECKED);
    if (idx >= 0) lv_buttonmatrix_set_button_ctrl(bm, idx, LV_BUTTONMATRIX_CTRL_CHECKED);
}

int w_seg_get(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    int idx = (int)lv_buttonmatrix_get_selected_button(bm);
    lv_obj_set_user_data(bm, (void *)(intptr_t)(idx + 1));
    return idx;
}

lv_obj_t *w_switch(lv_obj_t *p, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *sw = lv_switch_create(p);
    lv_obj_set_size(sw, 104, 56);
    lv_obj_set_style_bg_color(sw, HEX(C_BTN2), 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sw, HEX(C_TXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -6, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_outline_width(sw, 0, LV_STATE_FOCUS_KEY);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, ud);
    return sw;
}

void w_switch_set(lv_obj_t *sw, bool on, uint32_t on_color)
{
    lv_obj_set_state(sw, LV_STATE_CHECKED, on);
    lv_obj_set_style_bg_color(sw, HEX(on_color), LV_PART_INDICATOR | LV_STATE_CHECKED);
}

void w_fmt(lv_obj_t *l, const char *fmt, ...)
{
    char b[128];
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (strcmp(lv_label_get_text(l), b)) lv_label_set_text(l, b);
}

void w_color(lv_obj_t *o, uint32_t c) { lv_obj_set_style_text_color(o, HEX(c), 0); }
void w_bg(lv_obj_t *o, uint32_t c) { lv_obj_set_style_bg_color(o, HEX(c), 0); }
void w_show(lv_obj_t *o, bool s) { if (s) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
void w_enable(lv_obj_t *o, bool en) { lv_obj_set_state(o, LV_STATE_DISABLED, !en); }

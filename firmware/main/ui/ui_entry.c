// Full-screen text entry: Wi-Fi password, Victron BLE password (step 1), Victron encryption key (step 2), Tailscale auth key,
// weather location.
#include "ui_views.h"
#include "ui_widgets.h"
#include "ui.h"
#include "net/wifi_svc.h"
#include "net/tailnet_svc.h"
#include "ble/ble_svc.h"
#include "net/weather.h"
#include "app/settings.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

typedef enum { M_WIFI, M_VPIN, M_VKEY, M_TSKEY, M_LOC } mode_t_;
static lv_obj_t *s_root, *l_title, *l_step, *b_ok, *ta, *b_show, *l_count, *l_hint, *kb;
static mode_t_ s_mode;
static char s_ssid[33], s_pin[33];
static int s_cand;

static const char *HEX_MAP[] = { "0", "1", "2", "3", "4", "5", "6", "7", "\n",
                                 "8", "9", "A", "B", "C", "D", "E", "F", "\n",
                                 "Clear", LV_SYMBOL_BACKSPACE, "" };
static const lv_buttonmatrix_ctrl_t HEX_CTRL[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                                   4 | LV_BUTTONMATRIX_CTRL_CHECKED, 4 | LV_BUTTONMATRIX_CTRL_CHECKED };

static const char *LOC_MAP[] = { "1", "2", "3", "4", "5", "-", "\n",
                                 "6", "7", "8", "9", "0", ".", "\n",
                                 ", ", "Clear", LV_SYMBOL_BACKSPACE, "" };
static const lv_buttonmatrix_ctrl_t LOC_CTRL[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                                   2, 2 | LV_BUTTONMATRIX_CTRL_CHECKED, 2 | LV_BUTTONMATRIX_CTRL_CHECKED };

// "52.2297, 21.0122": latitude then longitude in decimal degrees, separated by a comma and/or spaces.
static bool parse_loc(const char *t, float *la, float *lo)
{
    char *e, *e2;
    *la = strtof(t, &e);
    if (e == t || (*e != ',' && *e != ' ')) return false;
    while (*e == ',' || *e == ' ') e++;
    *lo = strtof(e, &e2);
    if (e2 == e) return false;
    while (*e2 == ' ') e2++;
    return !*e2 && fabsf(*la) <= 90 && fabsf(*lo) <= 180;
}

static bool valid(void)
{
    size_t n = strlen(lv_textarea_get_text(ta));
    if (s_mode == M_LOC) { float a, b; return parse_loc(lv_textarea_get_text(ta), &a, &b); }
    if (s_mode == M_TSKEY) return n >= 20 && !strncmp(lv_textarea_get_text(ta), "tskey-", 6);
    return s_mode == M_WIFI ? n >= 8 : s_mode == M_VPIN ? n >= 1 : n == 32;
}

static void update(void)
{
    const char *t = lv_textarea_get_text(ta);
    bool ok = valid();
    w_btn_state(b_ok, true, ok);
    if (s_mode == M_VKEY) w_fmt(l_count, "%u/32", (unsigned)strlen(t)); else lv_label_set_text(l_count, "");
    if (s_mode == M_WIFI) { lv_label_set_text(l_hint, ok ? "Ready to connect" : "At least 8 characters"); w_color(l_hint, ok ? C_GREEN : C_MUTED); }
    if (s_mode == M_TSKEY) { lv_label_set_text(l_hint, ok ? "Ready to save" : "Starts with tskey-"); w_color(l_hint, ok ? C_GREEN : C_MUTED); }
    if (s_mode == M_LOC) {
        lv_label_set_text(l_hint, ok ? "Ready to save. Starlink's position replaces it once the dish shares one"
                                     : "Latitude, longitude in decimal degrees. South and west are negative");
        w_color(l_hint, ok ? C_GREEN : C_MUTED);
    }
}

static void close_entry(void) { w_show(s_root, false); ui_request_refresh(); }

static void open(mode_t_ m, const char *title, const char *step, const char *hint)
{
    s_mode = m;
    lv_label_set_text(l_title, title);
    lv_label_set_text(l_step, step);
    lv_label_set_text(l_hint, hint);
    w_color(l_hint, C_MUTED);
    lv_textarea_set_text(ta, "");
    bool keypad = m == M_VKEY || m == M_LOC;
    lv_textarea_set_password_mode(ta, !keypad);
    lv_textarea_set_max_length(ta, m == M_TSKEY ? 127 : m == M_WIFI ? 63 : 32);
    lv_textarea_set_accepted_chars(ta, m == M_VKEY ? "0123456789ABCDEFabcdef" : m == M_LOC ? "0123456789.,- " : NULL);
    lv_textarea_set_placeholder_text(ta, m == M_VKEY ? "32 hex characters" : m == M_TSKEY ? "tskey-auth-..."
                                         : m == M_LOC ? "52.2297, 21.0122" : "Password");
    w_show(b_show, !keypad);
    w_btn_text(b_show, "Show");
    w_btn_text(b_ok, m == M_WIFI ? "Connect" : m == M_VPIN ? "Next" : m == M_TSKEY || m == M_LOC ? "Save" : "Pair");
    lv_keyboard_set_mode(kb, m == M_VKEY ? LV_KEYBOARD_MODE_USER_1 : m == M_LOC ? LV_KEYBOARD_MODE_USER_2 : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_width(kb, keypad ? SZ(760, LV_PCT(100)) : LV_PCT(100));
    lv_obj_set_height(kb, keypad ? SZ(220, 200) : SZ(300, 250));
    update();
    w_show(s_root, true);
}

void ui_entry_open_wifi(const char *ssid)
{
    strlcpy(s_ssid, ssid, sizeof s_ssid);
    open(M_WIFI, ssid, "Wi-Fi password", "At least 8 characters");
}

void ui_entry_open_tailnet_key(void)
{
    open(M_TSKEY, "Tailscale auth key", "Tailnet", "From the Tailscale admin console: Settings > Keys. A reusable key lets the panel re-register on its own");
}

void ui_entry_open_location(void)
{
    open(M_LOC, "Weather location", "Weather", "");
    if (g_cfg.wx_src != WX_SRC_NONE) {
        char b[40];
        snprintf(b, sizeof b, "%.4f, %.4f", g_cfg.wx_lat, g_cfg.wx_lon);
        lv_textarea_set_text(ta, b);
    }
    update();
}

void ui_entry_open_victron(int cand)
{
    s_cand = cand;
    open(M_VPIN, "Bluetooth password", "SmartSolar - step 1 of 2", "The PIN/password set in VictronConnect (default 000000)");
}

static void confirm(void)
{
    if (!valid()) return;
    const char *t = lv_textarea_get_text(ta);
    if (s_mode == M_WIFI) { wifi_svc_connect(s_ssid, t); close_entry(); }
    else if (s_mode == M_TSKEY) {
        if (tailnet_svc_set_auth_key(t) == ESP_OK) tailnet_svc_set_enabled(true);
        close_entry();
    }
    else if (s_mode == M_LOC) {
        float la, lo;
        parse_loc(t, &la, &lo);
        weather_set_location(la, lo);
        close_entry();
    }
    else if (s_mode == M_VPIN) {
        strlcpy(s_pin, t, sizeof s_pin);
        open(M_VKEY, "Encryption key", "SmartSolar - step 2 of 2", "VictronConnect > Settings > Product info > Instant readout details");
    } else {
        uint8_t key[16];
        for (int i = 0; i < 16; i++) { unsigned v; sscanf(t + i * 2, "%2x", &v); key[i] = v; }
        ble_pair_select(s_cand, key, s_pin);
        close_entry();
    }
}

static void on_ok(lv_event_t *e) { confirm(); }
static void on_cancel(lv_event_t *e) { if (s_mode == M_VPIN || s_mode == M_VKEY) ble_pair_cancel(); close_entry(); }
static void on_show(lv_event_t *e)
{
    bool pw = lv_textarea_get_password_mode(ta);
    lv_textarea_set_password_mode(ta, !pw);
    w_btn_text(b_show, pw ? "Hide" : "Show");
}
static void on_ta(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_VALUE_CHANGED) update();
    else if (c == LV_EVENT_READY) confirm();
    else if (c == LV_EVENT_CANCEL) on_cancel(e);
}
static void on_kb(lv_event_t *e)
{
    lv_obj_t *k = lv_event_get_target(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(k);
    const char *t = id == LV_BUTTONMATRIX_BUTTON_NONE ? NULL : lv_buttonmatrix_get_button_text(k, id);
    if (t && !strcmp(t, "Clear")) { lv_textarea_set_text(ta, ""); return; }
    lv_keyboard_def_event_cb(e);
}

void ui_entry_create(void)
{
    s_root = w_rect(lv_layer_top(), C_BG, 0);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_hor(s_root, SZ(16, 10), 0);
    lv_obj_set_style_pad_bottom(s_root, SZ(16, 10), 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *h = w_row(s_root, 12);
    lv_obj_set_size(h, LV_PCT(100), SZ(60, 52));
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *cb = w_btn(h, "Cancel", F_16, 44, false, on_cancel, NULL);
    lv_obj_set_style_radius(cb, 22, 0);
    lv_obj_t *tc = w_col(h, 2);
    lv_obj_set_flex_align(tc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    l_title = w_label(tc, F_20, C_TXT, "");
    l_step = w_label(tc, F_14, C_MUTED, "");
    b_ok = w_btn(h, "Connect", F_16, 44, true, on_ok, NULL);
    lv_obj_set_style_radius(b_ok, 22, 0);

    lv_obj_t *mid = w_col(s_root, SZ(14, 8));
    lv_obj_set_width(mid, LV_PCT(100));
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *field = w_rect(mid, C_CARD, 16);
    lv_obj_set_size(field, SZ(720, 640), SZ(68, 56));
    lv_obj_set_style_border_width(field, 2, 0);
    lv_obj_set_style_border_color(field, HEX(C_BTN2), 0);
    lv_obj_set_style_pad_left(field, 12, 0);
    lv_obj_set_style_pad_right(field, 10, 0);
    lv_obj_set_flex_flow(field, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(field, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(field, 12, 0);
    ta = lv_textarea_create(field);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_text_font(ta, SZ(F_24, F_22), 0);
    lv_obj_set_style_text_color(ta, HEX(C_TXT), 0);
    lv_obj_set_style_text_color(ta, HEX(C_FAINT), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(ta, HEX(C_TXT), LV_PART_CURSOR);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(ta, on_ta, LV_EVENT_ALL, NULL);
    l_count = w_label(field, F_14, C_MUTED, "");
    b_show = w_btn(field, "Show", F_16, SZ(48, 40), false, on_show, NULL);
    lv_obj_set_style_radius(b_show, 12, 0);
    l_hint = w_label(mid, SZ(F_16, F_14), C_MUTED, "");
    lv_obj_set_style_max_width(l_hint, SZ(720, 640), 0);

    kb = lv_keyboard_create(s_root);
    lv_keyboard_set_textarea(kb, ta);
    lv_keyboard_set_popovers(kb, false);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, HEX_MAP, HEX_CTRL);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_2, LOC_MAP, LOC_CTRL);
    lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb, on_kb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_obj_set_style_pad_gap(kb, SZ(6, 4), 0);
    lv_obj_set_style_bg_color(kb, HEX(C_INSET), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, HEX(C_BTN), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(kb, HEX(C_BTN2), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, HEX(C_TXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, SZ(F_22, F_20), LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 12, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);

    w_show(s_root, false);
}

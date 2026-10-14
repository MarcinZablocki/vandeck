#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "net/ota_svc.h"
#include <time.h>
#include "ui_views.h"
#include "board/board.h"
#include "ui_widgets.h"
#include "ui.h"
#include "app/settings.h"
#include "net/wifi_svc.h"
#include "net/time_svc.h"
#include "ble/ble_svc.h"
#include "net/weather.h"
#include "net/update_svc.h"
#include "net/matter_svc.h"
#include "io/gps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { TAB_WIFI, TAB_HOME, TAB_DEV, TAB_HEATER, TAB_SOLAR, TAB_TIME, TAB_UNITS, TAB_ABOUT, TAB_N };
static lv_obj_t *s_root, *tabs[TAB_N], *tab_meta[TAB_N], *panels[TAB_N];
static int s_tab;
static int64_t s_last_scan;

static lv_obj_t *wf_sum, *wf_sw, *wf_list, *wf_row[WIFI_MAX_NETS], *wf_ssid[WIFI_MAX_NETS], *wf_meta[WIFI_MAX_NETS], *wf_stat[WIFI_MAX_NETS];
static lv_obj_t *dv_main, *dv_sum, *dv_dot[ROLE_COUNT], *dv_role[ROLE_COUNT], *dv_name[ROLE_COUNT], *dv_meta[ROLE_COUNT], *dv_b1[ROLE_COUNT], *dv_b2[ROLE_COUNT];
static lv_obj_t *dv_pick, *pk_title, *pk_sub, *pk_row[PAIR_MAX_CANDS], *pk_name[PAIR_MAX_CANDS], *pk_meta[PAIR_MAX_CANDS], *pk_empty, *pk_empty_l;
static lv_obj_t *ht_val;
static lv_obj_t *tm_sub, *tm_sw, *tm_tz, *tm_off, *tm_fmt, *tm_manual, *tm_mlabel, *tm_val[5], *tm_flbl[5];
static lv_obj_t *un_seg;
static uint8_t s_cand_order[PAIR_MAX_CANDS];

static const char *TAB_NAMES[] = { "Wi-Fi", "Home", "Devices", "Heater", "Solar", "Time", "Display", "About" };
static const char *FMT_MAP[] = { "24h", "12h", "" };
static const char *UNIT_MAP[] = { "Celsius °C", "Fahrenheit °F", "" };

static void cfg_done(void) { model_commit(); settings_save_later(); ui_request_refresh(); }

static void show_tab(int t)
{
    s_tab = t;
    for (int i = 0; i < TAB_N; i++) {
        w_show(panels[i], i == t);
        lv_obj_set_style_bg_opa(tabs[i], i == t ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        w_color(lv_obj_get_child(tabs[i], 0), i == t ? C_TXT : C_TXT2);
    }
    if (t == TAB_WIFI) { wifi_svc_scan(); s_last_scan = now_us(); }
    ui_request_refresh();
}

static void on_tab(lv_event_t *e) { show_tab((int)(intptr_t)lv_event_get_user_data(e)); }
static void on_done(lv_event_t *e) { ble_pair_cancel(); w_show(s_root, false); ui_request_refresh(); }
void ui_settings_open(void) { w_show(s_root, true); show_tab(TAB_WIFI); }

// --- Wi-Fi
static void on_wifi_sw(lv_event_t *e) { wifi_svc_set_enabled(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)); }
static void on_net(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const vd_net_t *n = &ui_model()->wifi.nets[i];
    if (!strcmp(n->ssid, g_cfg.wifi_ssid) && ui_model()->wifi.connected) return;
    if (wifi_svc_connect_saved(n->ssid)) return;   // remembered: no password needed
    if (n->secure) ui_entry_open_wifi(n->ssid); else wifi_svc_connect(n->ssid, "");
}

// Hold a remembered network to forget it (the one in use stays connected until it drops).
static void on_net_hold(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const vd_net_t *n = &ui_model()->wifi.nets[i];
    if (wifi_svc_forget(n->ssid)) ui_request_refresh();
}

static void build_wifi(lv_obj_t *p)
{
    lv_obj_t *h = w_header(p, "Wi-Fi", "", &wf_sum);
    wf_sw = w_switch(h, on_wifi_sw, NULL);
    wf_list = w_col(p, 6);
    lv_obj_set_width(wf_list, LV_PCT(100));
    lv_obj_set_flex_grow(wf_list, 1);
    lv_obj_add_flag(wf_list, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < WIFI_MAX_NETS; i++) {
        lv_obj_t *r = wf_row[i] = w_rect(wf_list, C_INSET, 16);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, HEX(C_BTN), LV_STATE_PRESSED);
        lv_obj_set_size(r, LV_PCT(100), 64);
        lv_obj_set_style_pad_hor(r, 20, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(r, on_net, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, on_net_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        lv_obj_t *c = w_col(r, 2);
        wf_ssid[i] = w_label(c, F_18, C_TXT, "");
        wf_meta[i] = w_label(c, F_14, C_MUTED, "");
        wf_stat[i] = w_label(r, F_16, C_TXT2, "");
    }
    w_label(p, F_14, C_MUTED, "Remembered networks join by themselves, the strongest in range first. Hold one to forget it.");
}

static void refresh_wifi(const vd_model_t *m)
{
    const vd_wifi_t *w = &m->wifi;
    w_switch_set(wf_sw, g_cfg.wifi_on, C_GREEN);
    if (!g_cfg.wifi_on) lv_label_set_text(wf_sum, "Off");
    else if (w->joining[0]) w_fmt(wf_sum, "Connecting to %s...", w->joining);
    else if (w->connected) w_fmt(wf_sum, "Connected to %s", g_cfg.wifi_ssid);
    else if (w->failed) lv_label_set_text(wf_sum, "Couldn't connect - check the password");
    else lv_label_set_text(wf_sum, w->scanning ? "Scanning..." : "Not connected");
    lv_obj_set_style_opa(wf_list, g_cfg.wifi_on ? LV_OPA_COVER : LV_OPA_40, 0);
    for (int i = 0; i < WIFI_MAX_NETS; i++) {
        bool vis = g_cfg.wifi_on && i < w->n_nets;
        w_show(wf_row[i], vis);
        if (!vis) continue;
        const vd_net_t *n = &w->nets[i];
        bool cur = w->connected && !strcmp(n->ssid, g_cfg.wifi_ssid), j = !strcmp(n->ssid, w->joining);
        lv_label_set_text(wf_ssid[i], n->ssid);
        w_fmt(wf_meta[i], "%s%s - %s", n->saved ? "Saved - " : "", n->secure ? "Secured" : "Open",
              n->rssi > -60 ? "strong" : n->rssi > -72 ? "good" : "weak");
        lv_label_set_text(wf_stat[i], cur ? "Connected" : j ? "Connecting..." : "Join");
        w_color(wf_stat[i], cur ? C_GREEN : j ? C_AMBER : C_TXT2);
    }
    if (s_tab == TAB_WIFI && g_cfg.wifi_on && now_us() - s_last_scan > 15LL * 1000000) { s_last_scan = now_us(); wifi_svc_scan(); }
}

// --- Apple Home (Matter)
static lv_obj_t *mh_sum, *mh_st, *mh_n, *mh_code, *mh_qrbox, *mh_qr, *mh_hint, *mh_reset;
static char s_qr_shown[64];
static int64_t s_reset_armed;

// Two taps: unpairing removes the panel from every Home and restarts it.
static void on_mh_reset(lv_event_t *e)
{
    if (now_us() - s_reset_armed < 5000000) { matter_svc_reset(); return; }
    s_reset_armed = now_us();
    ui_request_refresh();
}

static void build_home(lv_obj_t *p)
{
    w_header(p, "Apple Home", "", &mh_sum);
    lv_obj_t *cols = w_row(p, 28);
    lv_obj_set_width(cols, LV_PCT(100));
    lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // Dark modules on white with a quiet zone: what phone cameras read best, whatever the theme.
    mh_qrbox = w_rect(cols, 0xFFFFFF, 12);
    lv_obj_set_style_pad_all(mh_qrbox, 14, 0);
    lv_obj_set_size(mh_qrbox, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    mh_qr = lv_qrcode_create(mh_qrbox);
    lv_qrcode_set_size(mh_qr, 196);
    lv_qrcode_set_dark_color(mh_qr, lv_color_black());
    lv_qrcode_set_light_color(mh_qr, lv_color_white());
    lv_obj_t *c = w_col(cols, 14);
    lv_obj_set_flex_grow(c, 1);
    mh_st = w_kv(c, "Status", "");
    mh_n = w_kv(c, "Paired with", "");
    mh_code = w_kv(c, "Setup code", "");
    for (lv_obj_t **l = (lv_obj_t *[]){ mh_st, mh_n, mh_code, NULL }; *l; l++) lv_obj_set_style_text_font(*l, F_18, 0);
    mh_hint = w_label(c, F_14, C_MUTED, "");
    lv_obj_set_width(mh_hint, LV_PCT(100));
    lv_label_set_long_mode(mh_hint, LV_LABEL_LONG_WRAP);
    mh_reset = w_btn(c, "Unpair", F_16, 48, false, on_mh_reset, NULL);
}

static void refresh_home(void)
{
    vd_mt_state_t st = matter_svc_state();
    uint8_t n = matter_svc_fabrics();
    lv_label_set_text(mh_sum, st == MT_PAIRED ? "Batteries and bank power in the Home app" : "Matter accessory, paired over Wi-Fi");
    lv_label_set_text(mh_st, matter_svc_state_text(st));
    w_color(mh_st, st == MT_PAIRED ? C_GREEN : st == MT_ERROR ? C_RED : C_TXT);
    if (st == MT_PAIRED) w_fmt(mh_n, n == 1 ? "1 home" : "%u homes", n); else lv_label_set_text(mh_n, "--");
    char code[20];
    if (matter_svc_manual_code(code, sizeof code)) lv_label_set_text(mh_code, code); else lv_label_set_text(mh_code, "--");
    // The QR only pairs while the panel is unpaired; afterwards a home adds others through its own pairing mode.
    char qr[64];
    bool show = st == MT_UNPAIRED && matter_svc_qr(qr, sizeof qr);
    w_show(mh_qrbox, show);
    if (show && strcmp(qr, s_qr_shown)) { strlcpy(s_qr_shown, qr, sizeof s_qr_shown); lv_qrcode_update(mh_qr, qr, strlen(qr)); }
    lv_label_set_text(mh_hint,
        st == MT_UNPAIRED ? "In the Home app: + > Add Accessory, then scan the code, or choose More options and type the setup code. "
                            "Your phone must be on the same Wi-Fi as the panel. Home warns that the accessory is uncertified: add it anyway."
        : st == MT_PAIRED ? "Each battery shows its temperature; open it for charge level and charging. Bank power is in the Energy view (iOS 27). "
                            "To add the panel to another app as well, turn on pairing mode for it in the Home app."
        : st == MT_OFF ? "Matter is off in this firmware (VanDeck > Matter)."
        : st == MT_ERROR && matter_svc_skipped() ? "Matter was skipped: its last start brought the panel down. The console log (tools/console.sh) "
                                                   "shows how that run ended; 'matter retry' there tries again."
        : st == MT_ERROR ? "Matter didn't start. The console log (tools/console.sh) has the reason."
        : "Starting...");
    bool armed = now_us() - s_reset_armed < 5000000;
    w_show(mh_reset, st == MT_PAIRED);
    w_btn_text(mh_reset, armed ? "Tap again to unpair" : "Unpair");
    w_btn_state(mh_reset, armed, true);
}

// --- Devices
static void on_pair(lv_event_t *e) { ble_pair_begin((vd_role_t)(intptr_t)lv_event_get_user_data(e)); }
static void on_forget(lv_event_t *e) { ble_forget((vd_role_t)(intptr_t)lv_event_get_user_data(e)); }
static void on_pick_cancel(lv_event_t *e) { ble_pair_cancel(); }
static void on_pick(lv_event_t *e)
{
    int idx = s_cand_order[(int)(intptr_t)lv_event_get_user_data(e)];
    if (ui_model()->pair.role == ROLE_SOLAR) ui_entry_open_victron(idx);
    else ble_pair_select(idx, NULL, NULL);
}

static lv_obj_t *list_row(lv_obj_t *p, int h)
{
    lv_obj_t *r = w_rect(p, C_INSET, 16);
    lv_obj_set_size(r, LV_PCT(100), h);
    lv_obj_set_style_pad_left(r, 20, 0);
    lv_obj_set_style_pad_right(r, 12, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    return r;
}

static void build_devices(lv_obj_t *p)
{
    dv_main = w_col(p, 16);
    lv_obj_set_size(dv_main, LV_PCT(100), LV_PCT(100));
    lv_obj_t *hc = w_col(dv_main, 2);
    w_label(hc, F_20, C_TXT, "Bluetooth devices");
    dv_sum = w_label(hc, F_14, C_MUTED, "");
    lv_obj_t *list = w_col(dv_main, 6);
    lv_obj_set_width(list, LV_PCT(100));
    for (int r = 0; r < ROLE_COUNT; r++) {
        lv_obj_t *row = list_row(list, 80);
        lv_obj_t *l = w_row(row, 14);
        dv_dot[r] = w_dot(l, 10, C_FAINT);
        lv_obj_t *c = w_col(l, 2);
        dv_role[r] = w_label(c, F_14, C_MUTED, role_name(r));
        dv_name[r] = w_label(c, F_18, C_TXT, "");
        dv_meta[r] = w_label(c, F_14, C_MUTED, "");
        lv_obj_t *bt = w_row(row, 6);
        dv_b1[r] = w_btn(bt, "Pair", F_16, 48, true, on_pair, (void *)(intptr_t)r);
        dv_b2[r] = w_btn(bt, "Forget", F_16, 48, false, on_forget, (void *)(intptr_t)r);
    }

    dv_pick = w_col(p, 16);
    lv_obj_set_size(dv_pick, LV_PCT(100), LV_PCT(100));
    lv_obj_t *ph = w_row(dv_pick, 12);
    lv_obj_set_width(ph, LV_PCT(100));
    lv_obj_set_flex_align(ph, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *pc = w_col(ph, 2);
    pk_title = w_label(pc, F_20, C_TXT, "");
    pk_sub = w_label(pc, F_14, C_MUTED, "");
    lv_obj_t *cb = w_btn(ph, "Cancel", F_16, 48, false, on_pick_cancel, NULL);
    lv_obj_set_style_radius(cb, 24, 0);
    lv_obj_t *pl = w_col(dv_pick, 6);
    lv_obj_set_width(pl, LV_PCT(100));
    lv_obj_set_flex_grow(pl, 1);
    lv_obj_add_flag(pl, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < PAIR_MAX_CANDS; i++) {
        lv_obj_t *row = pk_row[i] = list_row(pl, 72);
        lv_obj_t *c = w_col(row, 2);
        pk_name[i] = w_label(c, F_18, C_TXT, "");
        pk_meta[i] = w_label(c, F_14, C_MUTED, "");
        w_btn(row, "Select", F_16, 48, true, on_pick, (void *)(intptr_t)i);
    }
    pk_empty = w_rect(pl, C_CARD, 16);
    lv_obj_set_size(pk_empty, LV_PCT(100), 72);
    lv_obj_set_style_border_width(pk_empty, 2, 0);
    lv_obj_set_style_border_color(pk_empty, HEX(C_INSET), 0);
    pk_empty_l = w_label(pk_empty, F_16, C_MUTED, "");
    lv_obj_center(pk_empty_l);
}

static void refresh_devices(const vd_model_t *m)
{
    static const char *TYPE[] = { "Victron", "LiTime battery", "Starlink relay" };
    const vd_pair_t *pr = &m->pair;
    w_show(dv_main, !pr->active); w_show(dv_pick, pr->active);
    int on = 0;
    for (int r = 0; r < ROLE_COUNT; r++) {
        const vd_dev_cfg_t *c = &g_cfg.dev[r];
        uint8_t st = m->dev_state[r];
        if (st == DEV_ON) on++;
        lv_label_set_text(dv_name[r], c->paired ? c->name : "Not paired");
        w_color(dv_name[r], c->paired ? C_TXT : C_MUTED);
        if (!c->paired) w_fmt(dv_meta[r], "%s expected", TYPE[role_type(r)]);
        else lv_label_set_text(dv_meta[r], st == DEV_PAIRING ? "Pairing..." : st == DEV_ON ? "Connected" : "Searching...");
        w_bg(dv_dot[r], st == DEV_ON ? C_GREEN : st == DEV_NONE ? C_FAINT : C_AMBER);
        w_btn_text(dv_b1[r], c->paired ? "Change" : "Pair");
        w_btn_state(dv_b1[r], !c->paired, true);
        w_show(dv_b2[r], c->paired);
        w_btn_state(dv_b2[r], false, true);
    }
    w_fmt(dv_sum, "%d of %d connected - each role takes one device", on, ROLE_COUNT);
    if (!pr->active) return;

    const char *rn = role_name(pr->role);
    char t[32]; snprintf(t, sizeof t, "Choose %s", rn);
    for (char *q = t + 8; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;
    lv_label_set_text(pk_title, t);
    w_fmt(pk_sub, pr->scanning ? "Scanning for %s devices..." : "Only %s devices are shown", TYPE[role_type(pr->role)]);
    int n = pr->scanning ? 0 : pr->n;
    for (int i = 0; i < n; i++) s_cand_order[i] = i;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++)
        if (pr->c[s_cand_order[j]].rssi > pr->c[s_cand_order[i]].rssi) { uint8_t x = s_cand_order[i]; s_cand_order[i] = s_cand_order[j]; s_cand_order[j] = x; }
    for (int i = 0; i < PAIR_MAX_CANDS; i++) {
        w_show(pk_row[i], i < n);
        if (i >= n) continue;
        const vd_cand_t *c = &pr->c[s_cand_order[i]];
        lv_label_set_text(pk_name[i], c->name);
        w_fmt(pk_meta[i], "%02X:%02X:%02X - signal %s (%d dBm)", c->addr[2], c->addr[1], c->addr[0],
              c->rssi > -65 ? "strong" : c->rssi > -80 ? "good" : "weak", c->rssi);
    }
    w_show(pk_empty, n == 0);
    lv_label_set_text(pk_empty_l, pr->scanning ? "Scanning..." : "No other devices found");
}

// --- Heater
static void on_offset(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    model_lock();
    int v = g_cfg.room_offset_x10 + d * 5;
    g_cfg.room_offset_x10 = v < -50 ? -50 : v > 50 ? 50 : v;
    cfg_done();
}

static void build_heater(lv_obj_t *p)
{
    w_header(p, "Room sensor offset", "Corrects the Truma room temperature reading", NULL);
    lv_obj_t *r = w_rect(p, C_INSET, 20);
    lv_obj_set_size(r, 420, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(r, 12, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *m = w_btn(r, "-", F_40, 76, false, on_offset, (void *)-1);
    lv_obj_set_width(m, 76); w_bg(m, C_BTN);
    ht_val = w_label(r, F_36, C_TXT, "0.0");
    lv_obj_t *pl = w_btn(r, "+", F_40, 76, false, on_offset, (void *)1);
    lv_obj_set_width(pl, 76); w_bg(pl, C_BTN);
}

// --- Time
static void on_auto(lv_event_t *e) { time_svc_set_auto(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)); }
static void on_tz(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    model_lock(); g_cfg.tz = (g_cfg.tz + VD_ZONE_COUNT + d) % VD_ZONE_COUNT; cfg_done();
    time_svc_apply_tz();
}
static void on_fmt(lv_event_t *e) { int i = w_seg_get(e); model_lock(); g_cfg.h12 = i == 1; cfg_done(); }
static void on_shift(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);   // field * 2 + (0 = minus, 1 = plus)
    time_svc_shift(v / 2, v % 2 ? 1 : -1);
    ui_request_refresh();
}

static void build_time(lv_obj_t *p)
{
    lv_obj_t *h = w_header(p, "Set automatically", "", &tm_sub);
    tm_sw = w_switch(h, on_auto, NULL);
    lv_obj_t *r = w_row(p, 12);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *zc = w_col(r, 10);
    lv_obj_set_flex_grow(zc, 3);
    w_label(zc, F_16, C_MUTED, "Time zone");
    lv_obj_t *zr = w_rect(zc, C_INSET, 18);
    lv_obj_set_size(zr, LV_PCT(100), 68);
    lv_obj_set_style_pad_hor(zr, 6, 0);
    lv_obj_set_flex_flow(zr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(zr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *bl = w_btn(zr, LV_SYMBOL_LEFT, F_20, 56, false, on_tz, (void *)-1);
    lv_obj_set_width(bl, 56); w_bg(bl, C_BTN);
    lv_obj_t *zm = w_col(zr, 2);
    lv_obj_set_flex_align(zm, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    tm_tz = w_label(zm, F_18, C_TXT, "");
    tm_off = w_label(zm, F_14, C_MUTED, "");
    lv_obj_t *br = w_btn(zr, LV_SYMBOL_RIGHT, F_20, 56, false, on_tz, (void *)1);
    lv_obj_set_width(br, 56); w_bg(br, C_BTN);
    lv_obj_t *fc = w_col(r, 10);
    lv_obj_set_flex_grow(fc, 2);
    w_label(fc, F_16, C_MUTED, "Format");
    tm_fmt = w_seg(fc, FMT_MAP, 68, F_18, on_fmt, NULL);

    tm_manual = w_col(p, 10);
    lv_obj_set_width(tm_manual, LV_PCT(100));
    tm_mlabel = w_label(tm_manual, F_16, C_MUTED, "Manual time");
    lv_obj_t *g = w_row(tm_manual, 8);
    lv_obj_set_width(g, LV_PCT(100));
    static const char *FL[] = { "Hour", "Minute", "Day", "Month", "Year" };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *c = w_rect(g, C_INSET, 16);
        lv_obj_set_flex_grow(c, 1);
        lv_obj_set_height(c, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, 6, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(c, 4, 0);
        lv_obj_t *p1 = w_btn(c, "+", F_24, 44, false, on_shift, (void *)(intptr_t)(i * 2 + 1));
        lv_obj_set_width(p1, LV_PCT(100)); w_bg(p1, C_BTN);
        tm_val[i] = w_label(c, F_22, C_TXT, "--");
        tm_flbl[i] = w_label(c, F_14, C_MUTED, FL[i]);
        lv_obj_t *m1 = w_btn(c, "-", F_24, 44, false, on_shift, (void *)(intptr_t)(i * 2));
        lv_obj_set_width(m1, LV_PCT(100)); w_bg(m1, C_BTN);
    }
}

static void refresh_time(const vd_model_t *m)
{
    static const char *MO[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    w_switch_set(tm_sw, g_cfg.auto_time, C_GREEN);
    lv_label_set_text(tm_sub, !g_cfg.auto_time ? "Manual - set below" : m->wifi.connected ? "Synced from internet via Wi-Fi" : "No Wi-Fi - keeping last synced time");
    lv_label_set_text(tm_tz, VD_ZONES[g_cfg.tz % VD_ZONE_COUNT].name);
    int off = time_utc_offset_min();
    if (off % 60) w_fmt(tm_off, "UTC%+d:%02d", off / 60, abs(off % 60)); else w_fmt(tm_off, "UTC%+d", off / 60);
    w_seg_set(tm_fmt, g_cfg.h12 ? 1 : 0);
    lv_obj_set_style_opa(tm_manual, g_cfg.auto_time ? LV_OPA_40 : LV_OPA_COVER, 0);
    if (g_cfg.auto_time) lv_obj_add_state(tm_manual, LV_STATE_DISABLED); else lv_obj_remove_state(tm_manual, LV_STATE_DISABLED);
    for (uint32_t i = 0; i < lv_obj_get_child_count(lv_obj_get_child(tm_manual, 1)); i++) {
        lv_obj_t *col = lv_obj_get_child(lv_obj_get_child(tm_manual, 1), i);
        w_enable(lv_obj_get_child(col, 0), !g_cfg.auto_time);
        w_enable(lv_obj_get_child(col, 3), !g_cfg.auto_time);
    }
    lv_label_set_text(tm_mlabel, g_cfg.auto_time ? "Manual time (turn off Set automatically)" : "Manual time");
    time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
    if (g_cfg.h12) { w_fmt(tm_val[0], "%d", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12); w_fmt(tm_flbl[0], "Hour - %s", lt.tm_hour < 12 ? "AM" : "PM"); }
    else { w_fmt(tm_val[0], "%02d", lt.tm_hour); lv_label_set_text(tm_flbl[0], "Hour"); }
    w_fmt(tm_val[1], "%02d", lt.tm_min);
    w_fmt(tm_val[2], "%d", lt.tm_mday);
    lv_label_set_text(tm_val[3], MO[lt.tm_mon]);
    w_fmt(tm_val[4], "%d", lt.tm_year + 1900);
}

// --- Units
static void on_unit(lv_event_t *e) { int i = w_seg_get(e); model_lock(); g_cfg.unit_f = i == 1; cfg_done(); }

// --- Display (Units tab): temperature unit, screen saver timeout, brightness
static lv_obj_t *dp_to, *dp_bri, *dp_bri_val;
static const char *TO_MAP[] = { "30 s", "1 min", "5 min", "Never", "" };

static void on_timeout(lv_event_t *e) { int i = w_seg_get(e); model_lock(); g_cfg.screen_timeout = i; cfg_done(); }

static void on_bright(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    model_lock(); g_cfg.brightness = v; model_commit();
    board_set_backlight(v);
    w_fmt(dp_bri_val, "%d %%", v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save_later();
}

static void build_display(lv_obj_t *p)
{
    w_label(p, F_16, C_MUTED, "Turn screen off after");
    dp_to = w_seg(p, TO_MAP, 68, F_18, on_timeout, NULL);
    lv_obj_set_width(dp_to, 520);
    w_label(p, F_14, C_FAINT, "A touch turns it back on (that touch doesn't press anything).");

    lv_obj_t *r = w_row(p, 16);
    lv_obj_set_width(r, 520);
    lv_obj_set_style_pad_top(r, 12, 0);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    w_label(r, F_16, C_MUTED, "Brightness");
    dp_bri_val = w_label(r, F_18, C_TXT, "");
    dp_bri = lv_slider_create(p);
    lv_obj_set_size(dp_bri, 500, 14);
    lv_obj_set_style_margin_hor(dp_bri, 10, 0);
    lv_slider_set_range(dp_bri, 10, 100);
    lv_obj_set_style_bg_color(dp_bri, HEX(C_INSET), 0);
    lv_obj_set_style_bg_color(dp_bri, HEX(C_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(dp_bri, HEX(C_TXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(dp_bri, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(dp_bri, on_bright, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(dp_bri, on_bright, LV_EVENT_RELEASED, NULL);

    lv_obj_t *u = w_label(p, F_16, C_MUTED, "Temperature");
    lv_obj_set_style_pad_top(u, 12, 0);
}

static void refresh_display(void)
{
    w_seg_set(dp_to, g_cfg.screen_timeout);
    if (!lv_obj_has_state(dp_bri, LV_STATE_PRESSED)) lv_slider_set_value(dp_bri, g_cfg.brightness, LV_ANIM_OFF);
    w_fmt(dp_bri_val, "%u %%", g_cfg.brightness);
    w_seg_set(un_seg, g_cfg.unit_f ? 1 : 0);
}

// --- About
static lv_obj_t *ab_ver, *ab_built, *ab_slot, *ab_upd, *ab_ip, *ab_up;
static lv_obj_t *up_seg, *up_st, *up_hint, *up_check, *up_inst;
static lv_obj_t *gp_sub, *gp_txt;
static const char *UPD_MAP[] = { "Off", "Notify", "Automatic", "" };

static void on_upd_mode(lv_event_t *e) { int i = w_seg_get(e); model_lock(); g_cfg.upd_mode = i; cfg_done(); }
static void on_upd_check(lv_event_t *e) { update_svc_check(); }
static void on_upd_install(lv_event_t *e) { update_svc_install(); }

static void build_about(lv_obj_t *p)
{
    w_tinted_image(p, &img_logo_160, C_TXT2);
    lv_obj_t *cols = w_row(p, 24);
    lv_obj_set_width(cols, LV_PCT(100));
    lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *kv = w_col(cols, 14);
    lv_obj_set_width(kv, 372);
    lv_obj_set_style_pad_top(kv, 8, 0);
    ab_ver = w_kv(kv, "Firmware version", "");
    ab_built = w_kv(kv, "Built", "");
    ab_upd = w_kv(kv, "Last update", "");
    ab_slot = w_kv(kv, "Running from", "");
    ab_ip = w_kv(kv, "IP address", "");
    ab_up = w_kv(kv, "Uptime", "");

    // Updates from GitHub releases
    lv_obj_t *u = w_col(cols, 12);
    lv_obj_set_flex_grow(u, 1);
    w_header(u, "Updates", update_svc_enabled() ? "From GitHub releases" : "No repository set (menuconfig)", NULL);
    up_seg = w_seg(u, UPD_MAP, 56, F_16, on_upd_mode, NULL);
    up_st = w_label(u, F_16, C_TXT2, "");
    up_hint = w_label(u, F_14, C_MUTED, "");
    for (lv_obj_t **l = (lv_obj_t *[]){ up_st, up_hint, NULL }; *l; l++) {
        lv_obj_set_width(*l, LV_PCT(100));
        lv_label_set_long_mode(*l, LV_LABEL_LONG_WRAP);
    }
    lv_obj_t *br = w_row(u, 10);
    up_check = w_btn(br, "Check now", F_16, 48, false, on_upd_check, NULL);
    up_inst = w_btn(br, "Install", F_16, 48, true, on_upd_install, NULL);
    if (!update_svc_enabled()) { w_enable(up_seg, false); w_show(br, false); }

    lv_obj_t *fn = w_label(p, F_14, C_MUTED, "Update over Wi-Fi: tools/ota.sh  -  console: tools/console.sh (vandeck.local:23)");
    lv_obj_set_style_pad_top(fn, 8, 0);

    // GPS diagnostics: what the 'gps' console command says about the fix and the aiding, in short
    lv_obj_t *gc = w_col(p, 4);
    lv_obj_set_width(gc, LV_PCT(100));
    gp_sub = w_label(gc, F_16, C_TXT, "");
    gp_txt = w_label(gc, F_14, C_TXT2, "");
    lv_obj_set_width(gp_txt, LV_PCT(100));
    lv_label_set_long_mode(gp_txt, LV_LABEL_LONG_WRAP);
    for (lv_obj_t **l = (lv_obj_t *[]){ ab_ver, ab_built, ab_upd, ab_slot, ab_ip, ab_up, NULL }; *l; l++)
        lv_obj_set_style_text_font(*l, F_18, 0);
}

static void fmt_when(char *b, size_t n, time_t t)
{
    struct tm tm; localtime_r(&t, &tm);
    strftime(b, n, g_cfg.h12 ? "%d %b %Y, %I:%M %p" : "%d %b %Y, %H:%M", &tm);
}

static void refresh_updates(const vd_model_t *m)
{
    const vd_update_t *u = &m->upd;
    w_seg_set(up_seg, g_cfg.upd_mode);
    char ago[32] = "";
    if (u->checked) {
        int64_t s = time(NULL) - u->checked;
        if (s < 120) snprintf(ago, sizeof ago, " (checked just now)");
        else if (s < 7200) snprintf(ago, sizeof ago, " (checked %d min ago)", (int)(s / 60));
        else snprintf(ago, sizeof ago, " (checked %d h ago)", (int)(s / 3600));
    }
    uint32_t c = C_TXT2;
    if (!update_svc_enabled()) lv_label_set_text(up_st, "Set VanDeck > GitHub repository for updates");
    else switch (u->state) {
    case UPD_CHECKING: lv_label_set_text(up_st, "Checking..."); break;
    case UPD_CURRENT: w_fmt(up_st, "Up to date%s", ago); c = C_GREEN; break;
    case UPD_AVAILABLE: w_fmt(up_st, "Version %s is available", u->latest); c = C_AMBER; break;
    case UPD_INSTALLING: w_fmt(up_st, "Installing %s - %u %%", u->latest, u->pct); c = C_AMBER; break;
    case UPD_FAILED: w_fmt(up_st, "%s", u->msg[0] ? u->msg : "Update check failed"); c = C_ORANGE; break;
    default: lv_label_set_text(up_st, g_cfg.upd_mode == UPD_MODE_OFF ? "Not checking" : "First check a few minutes after start"); break;
    }
    w_color(up_st, c);
    lv_label_set_text(up_hint, g_cfg.upd_mode == UPD_MODE_AUTO ? "Installs by itself at night (2 to 5 am) once the panel has been idle for 15 minutes"
                             : g_cfg.upd_mode == UPD_MODE_NOTIFY ? "Checks twice a day and shows a dot on Settings when there's an update"
                             : "No update checks. Check now still works");
    bool avail = u->state == UPD_AVAILABLE;
    w_show(up_inst, avail);
    if (avail) w_btn_text(up_inst, "Install");
    w_btn_state(up_check, false, u->state != UPD_CHECKING && u->state != UPD_INSTALLING);
}

static void refresh_gps(const vd_model_t *m)
{
    gps_info_t i;
    gps_info(&i);
    const vd_gps_t *g = &m->gps;
    if (!i.enabled && !g->demo) lv_label_set_text(gp_sub, "GPS: off (VanDeck > GPS in menuconfig)");
    else if (g->fix) w_fmt(gp_sub, "GPS: fix, %u satellites, HDOP %.1f%s", g->sats, g->hdop, g->demo ? " (demo)" : "");
    else if (g->present) w_fmt(gp_sub, "GPS: no fix yet, %u satellites in use", g->sats);
    else lv_label_set_text(gp_sub, "GPS: no data from the module");
    w_show(gp_txt, i.enabled);
    if (!i.enabled) return;

    char t[400], a[24], note[120];
    int k;
    if (g->fix) k = snprintf(t, sizeof t, "Last fix: now");
    else if (i.fix_unix) { gps_fmt_age(time(NULL) - i.fix_unix, a, sizeof a); k = snprintf(t, sizeof t, "Last fix: %s ago", a); }
    else k = snprintf(t, sizeof t, "Last fix: none since start-up");
    if (i.tx) {
        k += snprintf(t + k, sizeof t - k, "  -  Autonomous: %s", i.aop > 0 ? "on" : i.aop == 0 ? "refused by the module" : "not set yet");
        if (i.aop_sats >= 0) k += snprintf(t + k, sizeof t - k, ", orbits for %d satellites", i.aop_sats);
    }
    if (i.online) k += snprintf(t + k, sizeof t - k, "  -  Online: %s", i.online_token ? i.online_st : "no token");
    gps_aid_note(&i, note, sizeof note);
    if (i.aid_saved) k += snprintf(t + k, sizeof t - k, "\nSaved: %d frames (%d AOP), %s", i.aid_frames, i.aop_frames, note);
    else if (i.tx) k += snprintf(t + k, sizeof t - k, "\nSaved: none yet (every 30 min with a fix)");
    snprintf(t + k, sizeof t - k, "\nAt start-up: %s", i.aid);
    if (strcmp(lv_label_get_text(gp_txt), t)) lv_label_set_text(gp_txt, t);   // longer than w_fmt takes
}

static void refresh_about(const vd_model_t *m)
{
    refresh_updates(m);
    refresh_gps(m);
    const esp_app_desc_t *d = esp_app_get_description();
    lv_label_set_text(ab_ver, d->version);
    w_fmt(ab_built, "%s %.5s", d->date, d->time);
    const esp_partition_t *p = esp_ota_get_running_partition();
    lv_label_set_text(ab_slot, p ? p->label : "--");
    ota_install_t in = ota_svc_last_install();
    char when[40];
    if (!in.known) lv_label_set_text(ab_upd, "checking...");
    else if (!in.at) w_fmt(ab_upd, "via %s (time unknown)", in.via_ota ? "Wi-Fi" : "USB");
    else { fmt_when(when, sizeof when, in.at); w_fmt(ab_upd, "%s via %s", when, in.via_ota ? "Wi-Fi" : "USB"); }
    esp_netif_ip_info_t ip;
    esp_netif_t *nf = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nf && esp_netif_get_ip_info(nf, &ip) == ESP_OK && ip.ip.addr) w_fmt(ab_ip, IPSTR, IP2STR(&ip.ip));
    else lv_label_set_text(ab_ip, "not connected");
    int64_t s = esp_timer_get_time() / 1000000;
    if (s >= 86400) w_fmt(ab_up, "%lldd %lldh", s / 86400, s % 86400 / 3600);
    else w_fmt(ab_up, "%lldh %02lldm", s / 3600, s % 3600 / 60);
}

// --- Solar: the array the forecast's expected yield is worked out for
static lv_obj_t *sv_w, *sv_eff, *sv_ex;

static void on_pv_w(lv_event_t *e)
{
    int v = g_cfg.pv_w + (int)(intptr_t)lv_event_get_user_data(e) * 10;
    model_lock(); g_cfg.pv_w = v < 20 ? 20 : v > 2000 ? 2000 : v; cfg_done();
}

static void on_pv_eff(lv_event_t *e)
{
    int v = g_cfg.pv_eff + (int)(intptr_t)lv_event_get_user_data(e) * 5;
    model_lock(); g_cfg.pv_eff = v < 30 ? 30 : v > 100 ? 100 : v; cfg_done();
}

// - value + row; holding a button keeps stepping.
static lv_obj_t *stepper(lv_obj_t *p, lv_event_cb_t cb)
{
    lv_obj_t *r = w_rect(p, C_INSET, 20);
    lv_obj_set_size(r, 420, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(r, 12, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *m = w_btn(r, "-", F_40, 76, false, cb, (void *)-1);
    lv_obj_set_width(m, 76); w_bg(m, C_BTN);
    lv_obj_add_event_cb(m, cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)-1);
    lv_obj_t *v = w_label(r, F_36, C_TXT, "");
    lv_obj_t *pl = w_btn(r, "+", F_40, 76, false, cb, (void *)1);
    lv_obj_set_width(pl, 76); w_bg(pl, C_BTN);
    lv_obj_add_event_cb(pl, cb, LV_EVENT_LONG_PRESSED_REPEAT, (void *)1);
    return v;
}

static void build_solar(lv_obj_t *p)
{
    w_header(p, "Panel capacity", "Peak rating of all the panels together", NULL);
    sv_w = stepper(p, on_pv_w);
    lv_obj_t *h = w_header(p, "System efficiency", "Share of that rating that reaches the battery: heat, wiring and charger losses", NULL);
    lv_obj_set_style_pad_top(h, 8, 0);
    sv_eff = stepper(p, on_pv_eff);
    sv_ex = w_label(p, F_16, C_MUTED, "");
    lv_obj_set_style_pad_top(sv_ex, 8, 0);
    lv_obj_set_width(sv_ex, LV_PCT(100));
    lv_label_set_long_mode(sv_ex, LV_LABEL_LONG_WRAP);
}

static void refresh_solar(void)
{
    w_fmt(sv_w, "%u W", g_cfg.pv_w);
    w_fmt(sv_eff, "%u %%", g_cfg.pv_eff);
    w_fmt(sv_ex, "Expected yield: about %.1f kWh on a clear summer day, %.1f kWh when overcast",
          weather_expected_kwh(6), weather_expected_kwh(1));
}

void ui_settings_create(void)
{
    s_root = w_rect(lv_layer_top(), C_BG, 0);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_hor(s_root, 16, 0);
    lv_obj_set_style_pad_bottom(s_root, 16, 0);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *h = w_row(s_root, 12);
    lv_obj_set_size(h, LV_PCT(100), 60);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    w_label(h, F_26, C_TXT, "Settings");
    lv_obj_t *done = w_btn(h, "Done", F_16, 44, true, on_done, NULL);
    lv_obj_set_style_radius(done, 22, 0);

    static int32_t cols[] = { 240, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t rows[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_t *body = w_box(s_root);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_grid_dsc_array(body, cols, rows);
    lv_obj_set_style_pad_column(body, 12, 0);

    lv_obj_t *tl = w_rect(body, C_CARD, 24);
    lv_obj_set_grid_cell(tl, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_style_pad_all(tl, 12, 0);
    lv_obj_set_flex_flow(tl, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tl, 6, 0);
    for (int i = 0; i < TAB_N; i++) {
        lv_obj_t *b = tabs[i] = w_rect(tl, C_BTN, 16);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(b, LV_PCT(100), 54);   // eight tabs have to fit above the bottom edge
        lv_obj_set_style_pad_hor(b, 20, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(b, on_tab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        w_label(b, F_18, C_TXT2, TAB_NAMES[i]);
        tab_meta[i] = w_label(b, F_14, C_MUTED, "");
    }

    lv_obj_t *pane = w_rect(body, C_CARD, 24);
    lv_obj_set_grid_cell(pane, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_style_pad_all(pane, 24, 0);
    for (int i = 0; i < TAB_N; i++) {
        panels[i] = w_col(pane, 16);
        lv_obj_set_size(panels[i], LV_PCT(100), LV_PCT(100));
    }
    build_wifi(panels[TAB_WIFI]);
    build_home(panels[TAB_HOME]);
    build_devices(panels[TAB_DEV]);
    build_heater(panels[TAB_HEATER]);
    build_solar(panels[TAB_SOLAR]);
    build_time(panels[TAB_TIME]);
    build_display(panels[TAB_UNITS]);
    un_seg = w_seg(panels[TAB_UNITS], UNIT_MAP, 68, F_18, on_unit, NULL);
    lv_obj_set_width(un_seg, 420);
    build_about(panels[TAB_ABOUT]);

    w_show(s_root, false);
    show_tab(TAB_WIFI);
}

void ui_settings_refresh(const vd_model_t *m)
{
    if (lv_obj_has_flag(s_root, LV_OBJ_FLAG_HIDDEN)) return;
    int on = 0; for (int r = 0; r < ROLE_COUNT; r++) if (m->dev_state[r] == DEV_ON) on++;
    lv_label_set_text(tab_meta[TAB_WIFI], !g_cfg.wifi_on ? "Off" : m->wifi.connected ? g_cfg.wifi_ssid : "--");
    vd_mt_state_t mt = matter_svc_state();
    lv_label_set_text(tab_meta[TAB_HOME], mt == MT_PAIRED ? "Paired" : mt == MT_UNPAIRED ? "--" : mt == MT_OFF ? "Off" : "...");
    w_fmt(tab_meta[TAB_DEV], "%d/%d", on, ROLE_COUNT);
    w_fmt(tab_meta[TAB_HEATER], "%+.1f", g_cfg.room_offset_x10 / 10.0f);
    w_fmt(tab_meta[TAB_SOLAR], "%u W", g_cfg.pv_w);
    lv_label_set_text(tab_meta[TAB_TIME], g_cfg.h12 ? "12h" : "24h");
    static const char *TO_SHORT[] = { "30 s", "1 min", "5 min", "never" };
    w_fmt(tab_meta[TAB_UNITS], "%s, %s", ui_temp_unit(), TO_SHORT[g_cfg.screen_timeout & 3]);
    if (m->upd.state == UPD_AVAILABLE && g_cfg.upd_mode != UPD_MODE_OFF) w_fmt(tab_meta[TAB_ABOUT], "%s new", m->upd.latest);
    else lv_label_set_text(tab_meta[TAB_ABOUT], esp_app_get_description()->version);
    switch (s_tab) {
    case TAB_WIFI: refresh_wifi(m); break;
    case TAB_HOME: refresh_home(); break;
    case TAB_DEV: refresh_devices(m); break;
    case TAB_HEATER: {
        float o = g_cfg.room_offset_x10 / 10.0f;
        if (g_cfg.unit_f) w_fmt(ht_val, "%+.1f °F", o * 9 / 5); else w_fmt(ht_val, "%+.1f °C", o);
        break;
    }
    case TAB_SOLAR: refresh_solar(); break;
    case TAB_TIME: refresh_time(m); break;
    case TAB_ABOUT: refresh_about(m); break;
    default: refresh_display(); break;
    }
}

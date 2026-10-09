#include "ui.h"
#include "esp_log.h"
#include "ui_views.h"
#include "board/board.h"
#include "ui_widgets.h"
#include "app/settings.h"
#include "net/time_svc.h"
#include <time.h>
#include <stdio.h>
#include <string.h>

static vd_model_t s_m;
static uint32_t s_ver = UINT32_MAX;
static time_t s_sec;
static int s_view;
static lv_obj_t *s_views[4];
static lv_obj_t *l_time, *l_date, *l_demo, *d_wifi, *d_ble, *l_ble;
static const char *VIEW_MAP[] = { "Energy", "Solar", "Tanks", "Starlink", "" };

const vd_model_t *ui_model(void) { return &s_m; }
void ui_request_refresh(void) { s_ver = UINT32_MAX; }
const char *ui_temp_unit(void) { return g_cfg.unit_f ? "°F" : "°C"; }
void ui_fmt_temp(char *b, size_t n, float c)
{
    if (g_cfg.unit_f) snprintf(b, n, "%d", (int)lroundf(c * 9 / 5 + 32));
    else snprintf(b, n, "%.1f", c);
}

static void on_view(lv_event_t *e)
{
    s_view = w_seg_get(e);
    for (int i = 0; i < 4; i++) w_show(s_views[i], i == s_view);
    ui_request_refresh();
}
static void on_settings(lv_event_t *e) { ui_settings_open(); }

static void refresh_topbar(const vd_model_t *m)
{
    static const char *WD[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *MO[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (!time_valid()) { lv_label_set_text(l_time, "--:--"); lv_label_set_text(l_date, "Clock not set"); }
    else {
        time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
        if (g_cfg.h12) w_fmt(l_time, "%d:%02d %s", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
        else w_fmt(l_time, "%02d:%02d", lt.tm_hour, lt.tm_min);
        w_fmt(l_date, "%s %d %s", WD[lt.tm_wday], lt.tm_mday, MO[lt.tm_mon]);
    }
    w_show(l_demo, m->demo);
    w_bg(d_wifi, m->wifi.connected ? C_GREEN : g_cfg.wifi_on ? C_AMBER : C_FAINT);
    int paired = 0, on = 0;
    for (int r = 0; r < ROLE_COUNT; r++) { if (g_cfg.dev[r].paired) paired++; if (m->dev_state[r] == DEV_ON) on++; }
    w_fmt(l_ble, "BLE %d/%d", on, ROLE_COUNT);
    w_bg(d_ble, on == ROLE_COUNT ? C_GREEN : on ? C_AMBER : C_FAINT);
    (void)paired;
}

// Firmware update overlay on the top layer: progress while an OTA upload is written, the reason if it fails.
static lv_obj_t *s_ota, *s_ota_title, *s_ota_bar, *s_ota_msg;

static void ota_dismiss(lv_event_t *e)
{
    model_lock();
    if (M.ota.state == OTA_FAILED) M.ota.state = OTA_IDLE;
    model_commit();
}

static void refresh_ota(const vd_model_t *m)
{
    if (m->ota.state == OTA_IDLE) { if (s_ota) { lv_obj_delete(s_ota); s_ota = NULL; } return; }
    if (!s_ota) {
        s_ota = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_ota);
        lv_obj_set_size(s_ota, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_ota, HEX(C_BG), 0);
        lv_obj_set_style_bg_opa(s_ota, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_ota, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_ota, ota_dismiss, LV_EVENT_CLICKED, NULL);
        lv_obj_set_flex_flow(s_ota, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_ota, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(s_ota, 28, 0);
        w_tinted_image(s_ota, &img_logo_160, C_FAINT);
        s_ota_title = w_label(s_ota, F_28, C_TXT, "");
        s_ota_bar = lv_bar_create(s_ota);
        lv_obj_set_size(s_ota_bar, 520, 14);
        lv_obj_set_style_bg_color(s_ota_bar, HEX(C_INSET), 0);
        lv_obj_set_style_bg_color(s_ota_bar, HEX(C_AMBER), LV_PART_INDICATOR);
        lv_obj_set_style_radius(s_ota_bar, 7, 0);
        lv_obj_set_style_radius(s_ota_bar, 7, LV_PART_INDICATOR);
        s_ota_msg = w_label(s_ota, F_18, C_MUTED, "");
    }
    bool failed = m->ota.state == OTA_FAILED;
    if (m->ota.version[0]) w_fmt(s_ota_title, failed ? "Update to %s failed" : "Updating to %s", m->ota.version);
    else lv_label_set_text(s_ota_title, failed ? "Update failed" : "Updating firmware");
    lv_bar_set_value(s_ota_bar, m->ota.pct, LV_ANIM_OFF);
    w_show(s_ota_bar, !failed);
    if (failed) w_fmt(s_ota_msg, "%s - tap to close", m->ota.msg);
    else if (m->ota.state == OTA_DONE) lv_label_set_text(s_ota_msg, "Restarting...");
    else w_fmt(s_ota_msg, "%u %%  -  keep the panel powered", m->ota.pct);
    w_color(s_ota_msg, failed ? C_RED : C_MUTED);
}

static void saver_tick(lv_timer_t *t);
static void tick(lv_timer_t *t)
{
    model_snapshot(&s_m);
    time_t now = time(NULL);
    if (s_m.version == s_ver && now == s_sec) return;
    s_ver = s_m.version; s_sec = now;
    refresh_topbar(&s_m);
    refresh_ota(&s_m);
    switch (s_view) {
    case 0: ui_energy_refresh(&s_m); break;
    case 1: ui_solar_refresh(&s_m); break;
    case 2: ui_tanks_refresh(&s_m); break;
    default: ui_starlink_refresh(&s_m); break;
    }
    ui_settings_refresh(&s_m);
}

static lv_obj_t *status_item(lv_obj_t *p, const char *txt, lv_obj_t **dot, lv_obj_t **lbl)
{
    lv_obj_t *r = w_row(p, 8);
    *dot = w_dot(r, 10, C_FAINT);
    lv_obj_t *l = w_label(r, F_16, C_TXT2, txt);
    if (lbl) *lbl = l;
    return r;
}

void ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, HEX(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, HEX(C_TXT), 0);
    lv_obj_set_style_pad_hor(scr, 16, 0);
    lv_obj_set_style_pad_bottom(scr, 16, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Side blocks size to their content; the tabs centre in the space between them so they never overlap.
    static int32_t tcols[] = { LV_GRID_CONTENT, LV_GRID_FR(1), LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST };
    static int32_t trows[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_t *top = w_box(scr);
    lv_obj_set_size(top, LV_PCT(100), 60);
    lv_obj_set_grid_dsc_array(top, tcols, trows);
    lv_obj_set_style_pad_column(top, 16, 0);

    lv_obj_t *left = w_row(top, 14);
    lv_obj_set_grid_cell(left, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    l_time = w_label(left, F_26, C_TXT, "--:--");
    l_date = w_label(left, F_18, C_MUTED, "");
    l_demo = w_label(left, F_14, C_AMBER, "DEMO");

    lv_obj_t *seg = w_seg(top, VIEW_MAP, 48, F_16, on_view, NULL);
    lv_obj_set_width(seg, 460);
    lv_obj_set_style_bg_color(seg, HEX(C_CARD), 0);
    lv_obj_set_style_bg_color(seg, HEX(C_CARD), LV_PART_ITEMS);
    lv_obj_set_style_radius(seg, 22, 0);
    lv_obj_set_style_pad_all(seg, 4, 0);
    lv_obj_set_grid_cell(seg, LV_GRID_ALIGN_CENTER, 1, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    w_seg_set(seg, 0);

    lv_obj_t *right = w_row(top, 22);
    lv_obj_set_grid_cell(right, LV_GRID_ALIGN_END, 2, 1, LV_GRID_ALIGN_CENTER, 0, 1);
    status_item(right, "Wi-Fi", &d_wifi, NULL);
    status_item(right, "BLE 0/4", &d_ble, &l_ble);
    lv_obj_t *sb = w_btn(right, "Settings", F_16, 44, false, on_settings, NULL);
    lv_obj_set_style_radius(sb, 22, 0);

    lv_obj_t *content = w_box(scr);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    s_views[0] = ui_energy_create(content);
    s_views[1] = ui_solar_create(content);
    s_views[2] = ui_tanks_create(content);
    s_views[3] = ui_starlink_create(content);
    for (int i = 1; i < 4; i++) w_show(s_views[i], false);

    ui_settings_create();
    ui_entry_create();
    lv_timer_create(tick, 200, NULL);
    lv_timer_create(saver_tick, 500, NULL);
}

// Boot splash: full-screen wordmark on the top layer, so the dashboard can be built underneath it.
#define SPLASH_MIN_MS 1500
static lv_obj_t *s_splash;
static uint32_t s_splash_at;

void ui_splash_show(void)
{
    s_splash = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_splash);
    lv_obj_set_size(s_splash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_splash, HEX(C_BG), 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_splash, LV_OBJ_FLAG_CLICKABLE);   // swallow touches until the dashboard is ready
    lv_obj_center(w_tinted_image(s_splash, &img_logo_640, C_TXT));
    s_splash_at = lv_tick_get();
}

static void splash_fade(lv_timer_t *t)
{
    if (!s_splash) return;
    lv_obj_fade_out(s_splash, 400, 0);
    lv_obj_delete_delayed(s_splash, 450);
    s_splash = NULL;
}

void ui_splash_hide(void)
{
    if (!s_splash) return;
    uint32_t up = lv_tick_elaps(s_splash_at);
    lv_timer_t *t = lv_timer_create(splash_fade, up >= SPLASH_MIN_MS ? 1 : SPLASH_MIN_MS - up, NULL);
    lv_timer_set_repeat_count(t, 1);
}

// Screen saver: backlight off after the configured idle time; the first touch only wakes the screen. While dark, a
// transparent layer above everything takes that touch, so it can't also press whatever is underneath.
static lv_obj_t *s_blank;

static void wake(void)
{
    if (!s_blank) return;
    lv_obj_delete_async(s_blank);
    s_blank = NULL;
    board_set_backlight(g_cfg.brightness);
    ESP_LOGI("screen", "on");
    lv_display_trigger_activity(NULL);
}

static void on_blank_touch(lv_event_t *e) { wake(); }

static void saver_tick(lv_timer_t *t)
{
    int to = settings_screen_timeout_ms();
    bool busy = M.ota.state != OTA_IDLE;   // keep update progress visible
    if (s_blank) { if (!to || busy) wake(); return; }
    if (!to || busy || lv_display_get_inactive_time(NULL) < (uint32_t)to) return;
    s_blank = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_blank);
    lv_obj_set_size(s_blank, LV_PCT(100), LV_PCT(100));
    lv_obj_add_flag(s_blank, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_blank, on_blank_touch, LV_EVENT_PRESSED, NULL);
    board_set_backlight(0);
    ESP_LOGI("screen", "off after %d s idle", to / 1000);
}

void ui_screen_wake(void) { wake(); lv_display_trigger_activity(NULL); }
bool ui_screen_is_off(void) { return s_blank != NULL; }

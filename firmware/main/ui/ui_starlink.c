#include "ui_views.h"
#include "ui_widgets.h"
#include "app/settings.h"
#include "net/starlink.h"
#include <stdio.h>

static lv_obj_t *d_status, *l_status, *l_sub;
static lv_obj_t *l_alert, *d_alert, *seg_snow, *b_reboot;
typedef struct { lv_obj_t *val, *unit, *sub, *spark[SL_HIST_LEN]; } tile_t;
static tile_t TL[SL_NHIST];
static const char *SNOW_MAP[] = { "Auto", "On", "Off", "" };

static void on_snow(lv_event_t *e) { starlink_set_snow(w_seg_get(e)); }
static void on_reboot(lv_event_t *e) { starlink_reboot(); }

static void tile(lv_obj_t *g, int k, int col, int row, const char *label, const char *unit)
{
    lv_obj_t *t = w_rect(g, C_CARD, SZ(20, 16));
    lv_obj_set_grid_cell(t, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
    lv_obj_set_style_pad_ver(t, SZ(16, 12), 0);
    lv_obj_set_style_pad_hor(t, SZ(18, 12), 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 4, 0);
    w_label(t, F_14, C_MUTED, label);
    lv_obj_t *r = w_row(t, 6);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    TL[k].val = w_label(r, SZ(F_36, F_30), C_TXT, "--");
    TL[k].unit = w_label(r, SZ(F_16, F_14), C_MUTED, unit);
    TL[k].sub = w_label(t, F_14, C_MUTED, "");
    lv_obj_set_size(TL[k].sub, LV_PCT(100), lv_font_get_line_height(F_14));
    lv_label_set_long_mode(TL[k].sub, LV_LABEL_LONG_DOT);
    lv_obj_t *sp = w_row(t, 2);
    lv_obj_set_width(sp, LV_PCT(100));
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_flex_align(sp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int i = 0; i < SL_HIST_LEN; i++) {
        TL[k].spark[i] = w_rect(sp, C_INSET, 2);
        lv_obj_set_flex_grow(TL[k].spark[i], 1);
        lv_obj_set_height(TL[k].spark[i], 3);
    }
}

lv_obj_t *ui_starlink_create(lv_obj_t *p)
{
    static int32_t cols[] = { 240, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t rows[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    if (g_compact) cols[0] = 176;
    lv_obj_t *v = w_box(p);
    lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
    lv_obj_set_grid_dsc_array(v, cols, rows);
    lv_obj_set_style_pad_column(v, SZ(12, 10), 0);

    lv_obj_t *c = w_card(v);
    lv_obj_set_grid_cell(c, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    w_label(c, SZ(F_20, F_18), C_TXT, "Starlink");
    lv_obj_t *mid = w_col(c, 18);
    lv_obj_set_width(mid, LV_PCT(100));
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *sr = w_row(mid, 10);
    d_status = w_dot(sr, 12, C_MUTED);
    l_status = w_label(sr, SZ(F_26, F_22), C_MUTED, "Off");
    l_sub = w_label(mid, F_14, C_MUTED, "");
    lv_obj_set_width(l_sub, LV_PCT(100));
    lv_obj_set_style_text_align(l_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l_sub, LV_LABEL_LONG_WRAP);

    static int32_t gc[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t gr[] = { LV_GRID_FR(1), LV_GRID_FR(1), 68, LV_GRID_TEMPLATE_LAST };
    if (g_compact) gr[2] = 56;
    lv_obj_t *g = w_box(v);
    lv_obj_set_grid_cell(g, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_grid_dsc_array(g, gc, gr);
    lv_obj_set_style_pad_gap(g, SZ(12, 10), 0);
    tile(g, SL_DOWN, 0, 0, "Download", "Mbps");
    tile(g, SL_UP, 1, 0, "Upload", "Mbps");
    tile(g, SL_LAT, 2, 0, "Latency", "ms");
    tile(g, SL_PWR, 0, 1, "Dish power", "W");
    tile(g, SL_PING, 1, 1, "Ping success", "%");
    tile(g, SL_OBS, 2, 1, "Obstruction", "%");

    lv_obj_t *bb = w_rect(g, C_CARD, SZ(20, 16));
    lv_obj_set_grid_cell(bb, LV_GRID_ALIGN_STRETCH, 0, 3, LV_GRID_ALIGN_STRETCH, 2, 1);
    lv_obj_set_style_pad_left(bb, SZ(20, 14), 0);
    lv_obj_set_style_pad_right(bb, 8, 0);
    lv_obj_set_flex_flow(bb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bb, SZ(14, 10), 0);
    d_alert = w_dot(bb, 10, C_MUTED);
    l_alert = w_label(bb, SZ(F_16, F_14), C_MUTED, "");
    lv_obj_set_flex_grow(l_alert, 1);
    lv_label_set_long_mode(l_alert, LV_LABEL_LONG_DOT);
    w_label(bb, F_14, C_MUTED, "Snow melt");
    seg_snow = w_seg(bb, SNOW_MAP, SZ(52, 40), SZ(F_16, F_14), on_snow, NULL);
    lv_obj_set_width(seg_snow, SZ(210, 150));
    lv_obj_set_style_radius(seg_snow, SZ(14, 12), 0);
    lv_obj_set_style_pad_all(seg_snow, 4, 0);
    b_reboot = w_btn(bb, SZ("Reboot dish", "Reboot"), SZ(F_16, F_14), SZ(52, 40), false, on_reboot, NULL);
    return v;
}

static void dur(char *b, size_t n, int64_t us)
{
    int s = (int)(us / 1000000), m = s / 60;
    if (m >= 60) snprintf(b, n, "%dh %02dm", m / 60, m % 60); else snprintf(b, n, "%dm %02ds", m, s % 60);
}

void ui_starlink_refresh(const vd_model_t *m)
{
    const vd_starlink_t *s = &m->sl;
    int64_t now = now_us();
    int state = !s->power ? 0 : s->reachable ? 2 : 1;
    uint32_t col = state == 2 ? C_GREEN : state == 1 ? C_AMBER : C_MUTED;
    char b[48];

    w_bg(d_status, col); w_color(l_status, col);
    lv_label_set_text(l_status, state == 2 ? "Online" : state == 1 ? "No dish data" : "Offline");
    if (state == 2) { dur(b, sizeof b, now - s->online_since_us); w_fmt(l_sub, "Up %s", b); }
    else if (state == 1) lv_label_set_text(l_sub, "Dish not answering at 192.168.100.1");
    else lv_label_set_text(l_sub, "Not on the Starlink Wi-Fi");

    static const char *SUB_FMT[SL_NHIST] = { "peak %.0f", "peak %.0f", "PoP ping", "", "last 60 s", "of sky blocked" };
    for (int k = 0; k < SL_NHIST; k++) {
        tile_t *t = &TL[k];
        float v = s->v[k];
        bool ok = s->reachable && !isnan(v);
        float mx = 0, mn = 1e9f;
        for (int i = 0; i < s->hist_n; i++) if (!isnan(s->hist[k][i])) { if (s->hist[k][i] > mx) mx = s->hist[k][i]; if (s->hist[k][i] < mn) mn = s->hist[k][i]; }
        uint32_t vc = C_TXT;
        if (k == SL_LAT && v > 80) vc = C_ORANGE;
        if (k == SL_PWR && v > 55) vc = C_ORANGE;
        if (k == SL_PING && v < 98) vc = C_ORANGE;
        if (ok) {
            if (k == SL_PING || k == SL_OBS) w_fmt(t->val, "%.1f", v); else w_fmt(t->val, "%.0f", v);
            w_color(t->val, vc);
            if (k == SL_PWR) w_fmt(t->sub, "~%.1f A from battery", v / 13.3f);
            else w_fmt(t->sub, SUB_FMT[k], mx);
        } else {
            lv_label_set_text(t->val, "--");
            w_color(t->val, C_FAINT);
            lv_label_set_text(t->sub, state == 1 ? "Waiting for dish" : "Dish offline");
        }
        bool inv = k == SL_PING || k == SL_OBS;
        for (int i = 0; i < SL_HIST_LEN; i++) {
            int hi = i - (SL_HIST_LEN - s->hist_n);
            float x = hi >= 0 && ok ? s->hist[k][hi] : NAN;
            if (isnan(x)) { lv_obj_set_height(t->spark[i], 3); w_bg(t->spark[i], C_INSET); continue; }
            float lo = inv ? mn : 0, range = (mx - lo) > 0 ? mx - lo : 1;
            int pct = (int)((x - lo) / range * 100); if (pct < 8) pct = 8;
            lv_obj_set_height(t->spark[i], LV_PCT(pct));
            w_bg(t->spark[i], i == SL_HIST_LEN - 1 ? (k == SL_PWR ? C_AMBER : vc == C_TXT ? C_BLUE : vc) : C_BTN2);
        }
    }

    const char *alert = NULL; uint32_t ac = C_MUTED;
    if (s->reachable) {
        float pw = s->v[SL_PWR];
        if (g_cfg.sl_snow == SNOW_ON || s->heating || (g_cfg.sl_snow == SNOW_AUTO && pw > 55)) { alert = "Heating - snow melt active"; ac = C_ORANGE; }
        else if (s->v[SL_LAT] > 80) { alert = "High latency spike"; ac = C_ORANGE; }
        else if (s->v[SL_PING] < 98) { alert = "Packet loss in the last minute"; ac = C_ORANGE; }
        else { alert = "No alerts"; ac = C_GREEN; }
    }
    lv_label_set_text(l_alert, alert ? alert : state == 0 ? "Not on the Starlink Wi-Fi" : "No dish data");
    w_color(l_alert, ac); w_bg(d_alert, ac);
    w_seg_set(seg_snow, g_cfg.sl_snow);
    w_btn_state(b_reboot, false, s->reachable);
}


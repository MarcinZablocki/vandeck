#include "ui_views.h"
#include "ui_widgets.h"
#include "app/solar_log.h"
#include "app/settings.h"
#include "net/time_svc.h"
#include "net/weather.h"
#include "ui.h"
#include "ui_icons.h"
#include <time.h>
#include <stdio.h>
#include <math.h>

#define NDAYS 30
static int s_range = 14, s_sel = NDAYS - 1;
static lv_obj_t *l_sum, *l_since, *seg_range, *ticks[3], *bars[NDAYS], *fills[NDAYS], *lbls[NDAYS], *plot, *lrow;
static lv_obj_t *l_title, *l_note, *box_data, *box_none, *l_dy, *l_dmw, *l_dmv, *stk[3], *st_time[3];
static const char *RANGE_MAP[] = { "7d", "14d", "30d", "" };
static const char *MO[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *WD[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
#define FC_DAYS 4
static lv_obj_t *fc_none, *fc_days, *fc_col[FC_DAYS], *fc_ic[FC_DAYS], *fc_day[FC_DAYS], *fc_kwh[FC_DAYS];

static void on_forecast(lv_event_t *e) { ui_show_view(V_WEATHER); }

// Expected yield for the next days from the forecast's irradiation; tapping it opens the Weather page.
static void forecast_strip(lv_obj_t *v)
{
    lv_obj_t *c = w_rect(v, C_CARD, 24);
    lv_obj_set_grid_cell(c, LV_GRID_ALIGN_STRETCH, 0, 2, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_set_style_pad_hor(c, 24, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 16, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, on_forecast, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = w_col(c, 2);
    lv_obj_set_width(t, 150);
    w_label(t, F_18, C_TXT, "Forecast");
    w_label(t, F_14, C_MUTED, "Expected yield");
    fc_none = w_label(c, F_16, C_MUTED, "");
    fc_days = w_row(c, 8);
    lv_obj_set_flex_grow(fc_days, 1);
    for (int i = 0; i < FC_DAYS; i++) {
        lv_obj_t *d = fc_col[i] = w_row(fc_days, 10);
        lv_obj_set_flex_grow(d, 1);
        fc_ic[i] = w_label(d, F_ICON32, C_TXT2, "");
        lv_obj_t *dc = w_col(d, 0);
        fc_day[i] = w_label(dc, F_14, C_MUTED, "");
        fc_kwh[i] = w_label(dc, F_20, C_AMBER, "");
    }
}

static void forecast_refresh(const vd_model_t *m)
{
    const vd_weather_t *w = &m->wx;
    w_show(fc_none, !w->valid);
    w_show(fc_days, w->valid);
    if (!w->valid) {
        lv_label_set_text(fc_none, g_cfg.wx_src == WX_SRC_NONE ? "Needs a location - tap to open Weather" : "No forecast yet");
        return;
    }
    int64_t now = time_valid() ? time(NULL) : w->fetched, today = (now + w->utc_off) / 86400;
    int k = 0;
    for (int i = 0; i < w->n_days && k < FC_DAYS; i++) {
        const vd_wx_day_t *d = &w->daily[i];
        int64_t dn = (d->t + w->utc_off) / 86400;
        if (dn < today) continue;
        lv_label_set_text(fc_day[k], dn == today ? "Today" : dn == today + 1 ? "Tomorrow" : WD[(dn + 4) % 7]);
        lv_label_set_text(fc_ic[k], ui_wx_icon(d->code, true));
        w_color(fc_ic[k], ui_wx_color(d->code, true));
        if (isnan(d->rad_kwh_m2)) lv_label_set_text(fc_kwh[k], "--");
        else w_fmt(fc_kwh[k], "~%.1f kWh", weather_expected_kwh(d->rad_kwh_m2));
        w_show(fc_col[k++], true);
    }
    for (; k < FC_DAYS; k++) w_show(fc_col[k], false);
}

static void on_bar(lv_event_t *e) { s_sel = (int)(intptr_t)lv_event_get_user_data(e); ui_solar_refresh(NULL); }
static void on_range(lv_event_t *e)
{
    static const int R[] = { 7, 14, 30 };
    s_range = R[w_seg_get(e)];
    if (s_sel < NDAYS - s_range) s_sel = NDAYS - 1;
    ui_solar_refresh(NULL);
}

lv_obj_t *ui_solar_create(lv_obj_t *p)
{
    static int32_t cols[] = { LV_GRID_FR(1), 300, LV_GRID_TEMPLATE_LAST };
    static int32_t rows[] = { LV_GRID_FR(1), 92, LV_GRID_TEMPLATE_LAST };
    lv_obj_t *v = w_box(p);
    lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
    lv_obj_set_grid_dsc_array(v, cols, rows);
    lv_obj_set_style_pad_gap(v, 12, 0);

    lv_obj_t *c = w_card(v);
    lv_obj_set_grid_cell(c, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_style_pad_row(c, 20, 0);
    lv_obj_t *h = w_row(c, 12);
    lv_obj_set_width(h, LV_PCT(100));
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *hc = w_col(h, 2);
    lv_obj_set_flex_grow(hc, 1);   // the summary lines shorten with "..." before the range selector
    w_label(hc, F_20, C_TXT, "Daily yield");
    l_sum = w_label(hc, F_14, C_TXT2, "");
    l_since = w_label(hc, F_14, C_MUTED, "");
    lv_obj_set_size(l_sum, LV_PCT(100), lv_font_get_line_height(F_14)); lv_label_set_long_mode(l_sum, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l_since, LV_PCT(100), lv_font_get_line_height(F_14)); lv_label_set_long_mode(l_since, LV_LABEL_LONG_DOT);
    seg_range = w_seg(h, RANGE_MAP, 52, F_16, on_range, NULL);
    lv_obj_set_width(seg_range, 200);
    lv_obj_set_style_radius(seg_range, 16, 0);
    lv_obj_set_style_pad_all(seg_range, 4, 0);
    w_seg_set(seg_range, 1);

    lv_obj_t *chart = w_row(c, 10);
    lv_obj_set_width(chart, LV_PCT(100));
    lv_obj_set_flex_grow(chart, 1);
    lv_obj_set_flex_align(chart, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *tc = w_col(chart, 0);
    lv_obj_set_size(tc, 44, LV_PCT(100));
    lv_obj_set_style_pad_bottom(tc, 28, 0);
    lv_obj_set_flex_align(tc, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int i = 0; i < 3; i++) ticks[i] = w_label(tc, F_14, C_MUTED, "");
    lv_obj_t *pc = w_col(chart, 0);
    lv_obj_set_height(pc, LV_PCT(100));
    lv_obj_set_flex_grow(pc, 1);

    plot = w_row(pc, 8);
    lv_obj_set_width(plot, LV_PCT(100));
    lv_obj_set_flex_grow(plot, 1);
    lv_obj_set_flex_align(plot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int k = 0; k < 2; k++) {
        lv_obj_t *gl = w_rect(plot, C_LINE, 0);
        lv_obj_add_flag(gl, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_size(gl, LV_PCT(100), 1);
        lv_obj_align(gl, k ? LV_ALIGN_LEFT_MID : LV_ALIGN_TOP_LEFT, 0, k ? 0 : 6);
    }
    lrow = w_row(pc, 8);
    lv_obj_set_size(lrow, LV_PCT(100), 28);
    lv_obj_set_flex_align(lrow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    for (int i = 0; i < NDAYS; i++) {
        bars[i] = w_box(plot);
        lv_obj_add_flag(bars[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_height(bars[i], LV_PCT(100));
        lv_obj_set_flex_grow(bars[i], 1);
        lv_obj_add_event_cb(bars[i], on_bar, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        fills[i] = w_rect(bars[i], C_AMBER_DIM, 6);
        lv_obj_set_size(fills[i], LV_PCT(100), 4);
        lv_obj_align(fills[i], LV_ALIGN_BOTTOM_MID, 0, 0);
        lbls[i] = w_label(lrow, F_14, C_MUTED, "");
        lv_obj_set_flex_grow(lbls[i], 1);
        lv_obj_set_style_text_align(lbls[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(lbls[i], LV_LABEL_LONG_CLIP);
    }

    lv_obj_t *d = w_card(v);
    lv_obj_set_grid_cell(d, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    l_title = w_label(d, F_20, C_TXT, "Today");
    l_note = w_label(d, F_14, C_MUTED, "");
    box_data = w_col(d, 12);
    lv_obj_set_width(box_data, LV_PCT(100));
    lv_obj_set_style_pad_top(box_data, 18, 0);
    lv_obj_t *yr = w_row(box_data, 8);
    lv_obj_set_flex_align(yr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    l_dy = w_label(yr, F_NUM64, C_AMBER, "--");
    w_label(yr, F_24, C_TXT2, "kWh");
    lv_obj_t *kv = w_col(box_data, 12);
    lv_obj_set_width(kv, LV_PCT(100));
    lv_obj_set_style_pad_top(kv, 10, 0);
    l_dmw = w_kv(kv, "Max power", "W");
    l_dmv = w_kv(kv, "Max PV voltage", "V");
    box_none = w_rect(d, C_INSET, 16);
    lv_obj_set_size(box_none, LV_PCT(100), 120);
    lv_obj_center(w_label(box_none, F_22, C_MUTED, "No data"));
    w_spacer(d);
    lv_obj_t *ln = w_rect(d, C_INSET, 0);
    lv_obj_set_size(ln, LV_PCT(100), 2);
    lv_obj_t *sc = w_col(d, 12);
    lv_obj_set_width(sc, LV_PCT(100));
    lv_obj_set_style_pad_top(sc, 12, 0);
    w_label(sc, F_16, C_MUTED, "Time in charge stage");
    lv_obj_t *sb = w_row(sc, 3);
    lv_obj_set_size(sb, LV_PCT(100), 12);
    static const uint32_t SC[] = { C_AMBER, C_ORANGE, C_GREEN };
    static const char *SN[] = { "Bulk", "Absorption", "Float" };
    for (int i = 0; i < 3; i++) { stk[i] = w_rect(sb, SC[i], 4); lv_obj_set_height(stk[i], 12); lv_obj_set_flex_grow(stk[i], 1); }
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = w_row(sc, 10);
        lv_obj_set_width(r, LV_PCT(100));
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *l = w_row(r, 10);
        lv_obj_t *dot = w_rect(l, SC[i], 3); lv_obj_set_size(dot, 10, 10);
        w_label(l, F_16, C_TXT2, SN[i]);
        st_time[i] = w_label(r, F_18, C_TXT, "--");
    }
    forecast_strip(v);
    return v;
}

static void fmt_min(lv_obj_t *l, int m)
{
    if (m >= 60) w_fmt(l, "%dh %02dm", m / 60, m % 60); else w_fmt(l, "%dm", m);
}

void ui_solar_refresh(const vd_model_t *m)
{
    if (!m) m = ui_model();
    forecast_refresh(m);
    int32_t today = solar_log_today();
    if (today < 0) {
        lv_label_set_text(l_sum, "Set the clock to start logging");
        lv_label_set_text(l_since, "");
        for (int i = 0; i < NDAYS; i++) { w_show(bars[i], false); w_show(lbls[i], false); }
        return;
    }
    vd_day_t rec[NDAYS]; bool has[NDAYS];
    int first = NDAYS - s_range, logged = 0;
    uint32_t total = 0, mx = 0;
    for (int i = 0; i < NDAYS; i++) {
        has[i] = solar_log_get(today - (NDAYS - 1 - i), &rec[i]);
        if (i >= first && has[i]) { logged++; total += rec[i].yield_wh; if (rec[i].yield_wh > mx) mx = rec[i].yield_wh; }
    }
    uint32_t top = ((mx + 499) / 500) * 500; if (!top) top = 500;
    w_fmt(ticks[0], "%.1f", top / 1000.0f);
    w_fmt(ticks[1], "%.2f", top / 2000.0f);
    lv_label_set_text(ticks[2], "0");
    w_fmt(l_sum, "Total %.1f kWh - avg %.2f kWh/day", total / 1000.0f, logged ? total / 1000.0f / logged : 0);
    int32_t fd = solar_log_first_day();
    if (fd >= 0) { int y; unsigned mo, d; civil_from_days(fd, &y, &mo, &d); w_fmt(l_since, "Logged on this panel since %u %s", d, MO[mo - 1]); }

    int every = s_range == 30 ? 5 : s_range == 14 ? 2 : 1;
    lv_obj_set_style_pad_column(plot, s_range == 30 ? 4 : 8, 0);
    lv_obj_set_style_pad_column(lrow, s_range == 30 ? 4 : 8, 0);
    for (int i = 0; i < NDAYS; i++) {
        bool vis = i >= first, on = i == s_sel;
        w_show(bars[i], vis); w_show(lbls[i], vis);
        if (!vis) continue;
        if (has[i]) {
            int pct = (int)(rec[i].yield_wh * 100 / top); if (pct < 2) pct = 2;
            lv_obj_set_height(fills[i], LV_PCT(pct));
            w_bg(fills[i], on ? C_AMBER : C_AMBER_DIM);
        } else { lv_obj_set_height(fills[i], 4); w_bg(fills[i], on ? C_FAINT : C_LINE); }
        int32_t day = today - (NDAYS - 1 - i);
        bool show = i == NDAYS - 1 || (NDAYS - 1 - i) % every == 0 || (on && s_range != 30);
        if (!show) lv_label_set_text(lbls[i], "");
        else if (i == NDAYS - 1 && s_range == 7) lv_label_set_text(lbls[i], "Today");   // too narrow at 14/30 days
        else if (s_range == 7) lv_label_set_text(lbls[i], WD[(day + 4) % 7]);
        else { int y; unsigned mo, d; civil_from_days(day, &y, &mo, &d); w_fmt(lbls[i], "%u", d); }
        w_color(lbls[i], on ? C_TXT : C_MUTED);
    }

    int32_t sday = today - (NDAYS - 1 - s_sel);
    if (s_sel == NDAYS - 1) lv_label_set_text(l_title, "Today");
    else if (s_sel == NDAYS - 2) lv_label_set_text(l_title, "Yesterday");
    else { int y; unsigned mo, d; civil_from_days(sday, &y, &mo, &d); w_fmt(l_title, "%s %u %s", WD[(sday + 4) % 7], d, MO[mo - 1]); }
    bool h = has[s_sel];
    w_show(box_data, h); w_show(box_none, !h);
    lv_label_set_text(l_note, h ? "" : (fd < 0 || sday < fd) ? "Before logging started" : "No data - panel was off");
    const vd_day_t *r = &rec[s_sel];
    int mins[3] = { h ? r->min_bulk : 0, h ? r->min_abs : 0, h ? r->min_float : 0 };
    static const uint32_t SC[] = { C_AMBER, C_ORANGE, C_GREEN };
    int tot = mins[0] + mins[1] + mins[2];
    for (int i = 0; i < 3; i++) {
        // flex grow is a uint8_t: scale the minutes (up to 1440 a day) into 1..250
        lv_obj_set_flex_grow(stk[i], h && mins[i] ? (uint8_t)(mins[i] * 249 / tot + 1) : 1);
        w_bg(stk[i], h ? SC[i] : C_LINE);
        if (h) fmt_min(st_time[i], mins[i]); else lv_label_set_text(st_time[i], "--");
    }
    if (h) {
        w_fmt(l_dy, "%.2f", r->yield_wh / 1000.0f);
        w_fmt(l_dmw, "%u", r->max_w);
        if (r->max_v_x10) w_fmt(l_dmv, "%.1f", r->max_v_x10 / 10.0f); else lv_label_set_text(l_dmv, "--");
    }
}

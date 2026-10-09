// Weather page: conditions now, the next 24 hours and 5 days (Open-Meteo), and where the forecast is for.
#include "ui_views.h"
#include "ui_widgets.h"
#include "ui_icons.h"
#include "ui.h"
#include "app/settings.h"
#include "net/time_svc.h"
#include "net/weather.h"
#include <stdio.h>
#include <time.h>

static lv_obj_t *box_now, *box_msg, *l_msg, *l_msg_sub, *i_now, *l_temp, *l_unit, *l_cond;
static lv_obj_t *l_feels, *l_wind, *l_wind_u, *l_hum, *l_rise, *l_set, *l_upd, *l_src, *l_coord;
static struct { lv_obj_t *col, *t, *ic, *temp, *pop; } H[WX_HOURS];
static struct { lv_obj_t *col, *t, *ic, *hi, *lo, *pop; } D[WX_DAYS];
static const char *WD[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

const char *ui_wx_icon(int c, bool day)
{
    switch (c) {
    case 0: case 1: return day ? IC_WX_SUN : IC_WX_MOON;
    case 2: return day ? IC_WX_PARTLY : IC_WX_PARTLY_N;
    case 45: case 48: return IC_WX_FOG;
    case 51: case 53: case 55: case 56: case 57: case 61: case 63: case 80: case 81: return IC_WX_RAIN;
    case 65: case 82: return IC_WX_POUR;
    case 66: case 67: return IC_WX_SLEET;
    case 71: case 73: case 77: case 85: return IC_WX_SNOW;
    case 75: case 86: return IC_WX_SNOW_HEAVY;
    case 95: return IC_WX_STORM;
    case 96: case 99: return IC_WX_HAIL;
    default: return IC_WX_CLOUD;
    }
}

uint32_t ui_wx_color(int c, bool day)
{
    if (c <= 2) return day ? C_AMBER : C_TXT2;
    if (c >= 95) return C_ORANGE;
    if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return C_BLUE;
    if (c >= 71 && c <= 86) return C_TXT;
    return C_TXT2;
}

static void on_set_loc(lv_event_t *e) { ui_entry_open_location(); }

static lv_obj_t *unit_of(lv_obj_t *kv_val) { return lv_obj_get_child(lv_obj_get_parent(kv_val), 1); }

static lv_obj_t *fcol(lv_obj_t *p)
{
    lv_obj_t *c = w_col(p, 6);
    lv_obj_set_flex_grow(c, 1);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return c;
}

static lv_obj_t *frow(lv_obj_t *card, const char *title)
{
    w_label(card, F_20, C_TXT, title);
    lv_obj_t *r = w_row(card, 4);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_flex_grow(r, 1);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static void now_card(lv_obj_t *v)
{
    lv_obj_t *c = w_card(v);
    lv_obj_set_grid_cell(c, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 2);
    w_label(c, F_20, C_TXT, "Weather");
    l_upd = w_label(c, F_14, C_MUTED, "");

    box_now = w_col(c, 10);
    lv_obj_set_width(box_now, LV_PCT(100));
    lv_obj_set_style_pad_top(box_now, 14, 0);
    lv_obj_t *r = w_row(box_now, 14);
    i_now = w_label(r, F_ICON80, C_AMBER, IC_WX_SUN);
    lv_obj_t *tc = w_col(r, 2);
    lv_obj_t *tr = w_row(tc, 2);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    l_temp = w_label(tr, F_48, C_TXT, "--");
    l_unit = w_label(tr, F_24, C_TXT2, "°C");
    l_cond = w_label(tc, F_16, C_TXT2, "");
    lv_obj_t *kv = w_col(box_now, 10);
    lv_obj_set_width(kv, LV_PCT(100));
    lv_obj_set_style_pad_top(kv, 8, 0);
    l_feels = w_kv(kv, "Feels like", "");
    l_wind = w_kv(kv, "Wind", "km/h");
    l_wind_u = unit_of(l_wind);
    l_hum = w_kv(kv, "Humidity", "%");
    l_rise = w_kv(kv, "Sunrise", "");
    l_set = w_kv(kv, "Sunset", "");

    box_msg = w_col(c, 8);
    lv_obj_set_width(box_msg, LV_PCT(100));
    lv_obj_set_style_pad_top(box_msg, 24, 0);
    l_msg = w_label(box_msg, F_22, C_TXT2, "");
    l_msg_sub = w_label(box_msg, F_16, C_MUTED, "");
    lv_obj_set_width(l_msg_sub, LV_PCT(100));
    lv_label_set_long_mode(l_msg_sub, LV_LABEL_LONG_WRAP);

    w_spacer(c);
    lv_obj_t *ln = w_rect(c, C_INSET, 0);
    lv_obj_set_size(ln, LV_PCT(100), 2);
    lv_obj_t *f = w_row(c, 10);
    lv_obj_set_width(f, LV_PCT(100));
    lv_obj_set_style_pad_top(f, 14, 0);
    w_label(f, F_ICON32, C_MUTED, IC_PIN);
    lv_obj_t *fc = w_col(f, 2);
    lv_obj_set_flex_grow(fc, 1);
    l_src = w_label(fc, F_16, C_TXT2, "");
    l_coord = w_label(fc, F_14, C_MUTED, "");
    lv_obj_t *b = w_btn(f, "Set", F_16, 48, false, on_set_loc, NULL);
    lv_obj_set_style_radius(b, 16, 0);
}

lv_obj_t *ui_weather_create(lv_obj_t *p)
{
    static int32_t cols[] = { 300, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t rows[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_t *v = w_box(p);
    lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
    lv_obj_set_grid_dsc_array(v, cols, rows);
    lv_obj_set_style_pad_gap(v, 12, 0);
    now_card(v);

    lv_obj_t *hc = w_card(v);
    lv_obj_set_grid_cell(hc, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_t *hr = frow(hc, "Next 24 hours");
    for (int i = 0; i < WX_HOURS; i++) {
        lv_obj_t *c = H[i].col = fcol(hr);
        H[i].t = w_label(c, F_14, C_MUTED, "");
        H[i].ic = w_label(c, F_ICON32, C_TXT2, "");
        H[i].temp = w_label(c, F_20, C_TXT, "");
        H[i].pop = w_label(c, F_14, C_MUTED, "");
    }

    lv_obj_t *dc = w_card(v);
    lv_obj_set_grid_cell(dc, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 1, 1);
    lv_obj_t *dr = frow(dc, "Next 5 days");
    for (int i = 0; i < WX_DAYS; i++) {
        lv_obj_t *c = D[i].col = fcol(dr);
        D[i].t = w_label(c, F_14, C_MUTED, "");
        D[i].ic = w_label(c, F_ICON32, C_TXT2, "");
        lv_obj_t *t = w_row(c, 6);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        D[i].hi = w_label(t, F_20, C_TXT, "");
        D[i].lo = w_label(t, F_16, C_MUTED, "");
        D[i].pop = w_label(c, F_14, C_MUTED, "");
    }
    return v;
}

// Clock times at the forecast's location (its UTC offset), so they are right even with the panel's zone unset.
static void fmt_hm(char *b, size_t n, int64_t t, int32_t off)
{
    int64_t s = (t + off) % 86400; if (s < 0) s += 86400;
    int h = (int)(s / 3600), m = (int)(s / 60 % 60);
    if (g_cfg.h12) snprintf(b, n, "%d:%02d %s", h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
    else snprintf(b, n, "%02d:%02d", h, m);
}

static void fmt_age(char *b, size_t n, int64_t s)
{
    if (s < 90) snprintf(b, n, "just now");
    else if (s < 5400) snprintf(b, n, "%d min ago", (int)(s / 60));
    else if (s < 2 * 86400) snprintf(b, n, "%d h ago", (int)(s / 3600));
    else snprintf(b, n, "%d days ago", (int)(s / 86400));
}

static void pop_label(lv_obj_t *l, uint8_t pop)
{
    w_fmt(l, "%u%%", pop);
    w_color(l, pop >= 30 ? C_BLUE : C_MUTED);
}

void ui_weather_refresh(const vd_model_t *m)
{
    const vd_weather_t *w = &m->wx;
    char b[32], age[24];
    int64_t now = time_valid() ? time(NULL) : w->fetched;

    // Where the forecast is for
    if (g_cfg.wx_src == WX_SRC_NONE) { lv_label_set_text(l_src, "No location"); lv_label_set_text(l_coord, ""); }
    else {
        lv_label_set_text(l_src, g_cfg.wx_src == WX_SRC_MANUAL ? "Set by hand" : m->sl.loc_ok ? "Starlink GPS" : "Last Starlink fix");
        w_fmt(l_coord, "%.3f, %.3f", g_cfg.wx_lat, g_cfg.wx_lon);
    }

    // Freshness line
    fmt_age(age, sizeof age, now - w->fetched);
    uint32_t uc = C_MUTED;
    if (!w->valid) lv_label_set_text(l_upd, "Open-Meteo");
    else if (w->state == WX_FETCHING) lv_label_set_text(l_upd, "Updating...");
    else if (w->state == WX_NO_NET) { w_fmt(l_upd, "Offline - updated %s", age); uc = C_ORANGE; }
    else if (w->state == WX_ERROR) { w_fmt(l_upd, "Update failed - from %s", age); uc = C_ORANGE; }
    else w_fmt(l_upd, "Updated %s", age);
    w_color(l_upd, uc);

    w_show(box_now, w->valid);
    w_show(box_msg, !w->valid);
    if (!w->valid) {
        switch (w->state) {
        case WX_NO_LOC:
            lv_label_set_text(l_msg, "No location yet");
            lv_label_set_text(l_msg_sub, m->sl.reachable
                ? "The dish isn't sharing its position. In the Starlink app, allow local network access "
                  "(Advanced > Debug data), or set a location below."
                : "The forecast follows the Starlink dish's position once it's online. You can also set a location below.");
            break;
        case WX_NO_NET: lv_label_set_text(l_msg, "Waiting for internet"); lv_label_set_text(l_msg_sub, "The forecast loads once the panel is online."); break;
        case WX_FETCHING: lv_label_set_text(l_msg, "Getting the forecast..."); lv_label_set_text(l_msg_sub, ""); break;
        default: lv_label_set_text(l_msg, "No forecast"); lv_label_set_text(l_msg_sub, "Couldn't reach Open-Meteo. Trying again every 2 minutes."); break;
        }
        for (int i = 0; i < WX_HOURS; i++) w_show(H[i].col, false);
        for (int i = 0; i < WX_DAYS; i++) w_show(D[i].col, false);
        return;
    }

    // Now
    lv_label_set_text(i_now, ui_wx_icon(w->code, w->day));
    w_color(i_now, ui_wx_color(w->code, w->day));
    w_fmt(l_temp, "%d", (int)lroundf(g_cfg.unit_f ? w->temp * 9 / 5 + 32 : w->temp));
    lv_label_set_text(l_unit, ui_temp_unit());
    lv_label_set_text(l_cond, weather_text(w->code));
    ui_fmt_deg(b, sizeof b, w->feels); lv_label_set_text(l_feels, b);
    if (g_cfg.unit_f) { w_fmt(l_wind, "%.0f", w->wind_kmh * 0.6214f); lv_label_set_text(l_wind_u, "mph"); }
    else { w_fmt(l_wind, "%.0f", w->wind_kmh); lv_label_set_text(l_wind_u, "km/h"); }
    w_fmt(l_hum, "%u", w->humidity);

    // Days, skipping any that have already ended at the location (a forecast held while offline)
    int64_t today = (now + w->utc_off) / 86400;
    int k = 0;
    const vd_wx_day_t *td = NULL;
    for (int i = 0; i < w->n_days && k < WX_DAYS; i++) {
        const vd_wx_day_t *d = &w->daily[i];
        int64_t dn = (d->t + w->utc_off) / 86400;
        if (dn < today) continue;
        if (dn == today) td = d;
        if (dn == today) lv_label_set_text(D[k].t, "Today");
        else if (dn == today + 1) lv_label_set_text(D[k].t, "Tomorrow");
        else lv_label_set_text(D[k].t, WD[(dn + 4) % 7]);
        lv_label_set_text(D[k].ic, ui_wx_icon(d->code, true));
        w_color(D[k].ic, ui_wx_color(d->code, true));
        ui_fmt_deg(b, sizeof b, d->tmax); lv_label_set_text(D[k].hi, b);
        ui_fmt_deg(b, sizeof b, d->tmin); lv_label_set_text(D[k].lo, b);
        pop_label(D[k].pop, d->pop);
        w_show(D[k++].col, true);
    }
    for (; k < WX_DAYS; k++) w_show(D[k].col, false);
    if (td) {
        fmt_hm(b, sizeof b, td->sunrise, w->utc_off); lv_label_set_text(l_rise, b);
        fmt_hm(b, sizeof b, td->sunset, w->utc_off); lv_label_set_text(l_set, b);
    } else { lv_label_set_text(l_rise, "--"); lv_label_set_text(l_set, "--"); }

    // Hours
    k = 0;
    for (int i = 0; i < w->n_hours; i++) {
        const vd_wx_hour_t *h = &w->hour[i];
        if (h->t + 3 * 3600 <= now) continue;
        if (h->t <= now) lv_label_set_text(H[k].t, "Now");
        else {
            int64_t s = (h->t + w->utc_off) % 86400; int hh = (int)(s / 3600);
            if (g_cfg.h12) w_fmt(H[k].t, "%d %s", hh % 12 ? hh % 12 : 12, hh < 12 ? "AM" : "PM");
            else w_fmt(H[k].t, "%02d:00", hh);
        }
        lv_label_set_text(H[k].ic, ui_wx_icon(h->code, h->day));
        w_color(H[k].ic, ui_wx_color(h->code, h->day));
        ui_fmt_deg(b, sizeof b, h->temp); lv_label_set_text(H[k].temp, b);
        pop_label(H[k].pop, h->pop);
        w_show(H[k++].col, true);
    }
    for (; k < WX_HOURS; k++) w_show(H[k].col, false);
}

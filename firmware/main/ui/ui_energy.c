#include "ui_views.h"
#include "ui_widgets.h"
#include "ui.h"
#include "app/settings.h"
#include "io/truma_lin.h"
#include <stdio.h>
#include <math.h>

static lv_obj_t *l_pv, *l_pvcap, *st_bar[3], *st_lbl[3], *l_serr, *l_sbv, *l_sba, *l_scp, *l_sload, *l_yield;
static lv_obj_t *l_tot, *l_tot_ah, *flow_dot, *l_flow, *l_flow_w;
static struct { lv_obj_t *fill, *soc, *amps, *ah, *err; } B[2];
static lv_obj_t *l_roomnow, *sp_dim[4];   // setpoint controls dim with the heating switch; the room reading doesn't
static lv_obj_t *l_hsub, *sw_heat, *ctrls, *b_minus, *b_plus, *l_sp, *l_unit, *seg_water, *seg_fan;
static bool s_unit_f = false;

static const char *WATER_C[] = { "Off", "Eco 40°", "Hot 60°", "" };
static const char *WATER_F[] = { "Off", "Eco 104°", "Hot 140°", "" };
static const char *FAN_MAP[] = { "Eco", "High", "Vent", "" };

static void on_heat(lv_event_t *e) { truma_set_power(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED)); }
static void on_step(lv_event_t *e) { truma_set_setpoint(g_cfg.setpoint_c + (int)(intptr_t)lv_event_get_user_data(e) * 0.5f); }
static void on_water(lv_event_t *e) { truma_set_water(w_seg_get(e)); }
static void on_fan(lv_event_t *e) { truma_set_fan(w_seg_get(e)); }

// Victron operation modes and charger errors (VE.Direct protocol / victron-ble).
static const char *solar_mode_text(uint8_t m)
{
    switch (m) {
    case 0: return "Off";            case 1: return "Low power";       case 2: return "Fault";
    case 3: return "Bulk";           case 4: return "Absorption";      case 5: return "Float";
    case 6: return "Storage";        case 7: return "Equalize";        case 11: return "Power supply";
    case 245: return "Starting up";  case 246: return "Repeated absorption"; case 247: return "Recondition";
    case 248: return "BatterySafe";  case 252: return "External control";
    default: return NULL;
    }
}

static const char *solar_err_text(uint8_t e)
{
    switch (e) {
    case 1: return "Battery temperature too high";     case 2: return "Battery voltage too high";
    case 3: case 4: case 5: return "Remote temperature sensor failure";
    case 6: case 7: case 8: return "Remote battery voltage sense failure";
    case 11: return "Battery high ripple voltage";      case 14: return "Battery temperature too low";
    case 17: return "Charger temperature too high";     case 18: return "Charger over current";
    case 20: return "Bulk time limit exceeded";         case 21: return "Current sensor issue";
    case 22: case 23: return "Internal temperature sensor failure";
    case 24: return "Fan failure";                      case 26: return "Terminals overheated";
    case 27: return "Charger short circuit";            case 28: return "Power stage issue";
    case 29: return "Over-charge protection";           case 33: return "PV voltage too high";
    case 34: return "PV current too high";              case 35: return "PV over-power";
    case 38: return "Input shutdown (battery voltage too high)";
    case 39: return "Input shutdown (current during off mode)";
    case 40: return "PV input failed to shut down";
    case 65: return "Lost communication with a device"; case 66: return "Synchronised charging config issue";
    case 67: return "BMS connection lost";
    case 68: case 69: case 70: case 71: return "Network misconfigured";
    case 80: case 81: case 82: case 83: case 84: case 85: case 86: case 87: return "PV input shutdown";
    case 114: return "CPU temperature too high";        case 116: return "Factory calibration data lost";
    case 117: return "Incompatible firmware";           case 119: return "Settings data lost";
    default: return NULL;
    }
}

static void solar_card(lv_obj_t *v)
{
    lv_obj_t *c = w_card(v);
    lv_obj_set_grid_cell(c, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    w_header(c, "Solar", "SmartSolar 100/30", NULL);
    lv_obj_t *r = w_row(c, 8);
    lv_obj_set_style_pad_top(r, 24, 0);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    l_pv = w_label(r, F_NUM92, C_AMBER, "--");
    w_label(r, F_30, C_TXT2, "W");
    l_pvcap = w_label(c, F_16, C_MUTED, "PV power");
    lv_obj_set_style_pad_top(l_pvcap, 8, 0);

    lv_obj_t *g = w_row(c, 6);
    lv_obj_set_width(g, LV_PCT(100));
    lv_obj_set_style_pad_top(g, 28, 0);
    static const char *names[] = { "Bulk", "Absorb", "Float" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *col = w_col(g, 8);
        lv_obj_set_flex_grow(col, 1);
        st_bar[i] = w_rect(col, C_INSET, 4);
        lv_obj_set_size(st_bar[i], LV_PCT(100), 8);
        st_lbl[i] = w_label(col, F_14, C_MUTED, names[i]);
    }
    l_serr = w_label(c, F_14, C_RED, "");
    lv_obj_set_width(l_serr, LV_PCT(100));
    lv_label_set_long_mode(l_serr, LV_LABEL_LONG_WRAP);
    w_show(l_serr, false);
    w_spacer(c);
    lv_obj_t *ln = w_rect(c, C_INSET, 0);
    lv_obj_set_size(ln, LV_PCT(100), 2);
    lv_obj_t *kv = w_col(c, 14);
    lv_obj_set_width(kv, LV_PCT(100));
    lv_obj_set_style_pad_top(kv, 14, 0);
    // Everything the charger's instant readout carries (it has no PV voltage or temperature).
    l_sbv = w_kv(kv, "Battery", "V");
    l_sba = w_kv(kv, "Charge current", "A");
    l_scp = w_kv(kv, "Charge power", "W");
    l_sload = w_kv(kv, "Load output", "A");
    w_show(lv_obj_get_parent(lv_obj_get_parent(l_sload)), false);   // only chargers with a load output report it
    l_yield = w_kv(kv, "Yield today", "kWh");
}



static void battery_card(lv_obj_t *v)
{
    lv_obj_t *c = w_card(v);
    lv_obj_set_grid_cell(c, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_t *h = w_header(c, "Batteries", "LiTime 2x160Ah", NULL);
    lv_obj_t *rc = w_col(h, 2);
    lv_obj_set_flex_align(rc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_t *rr = w_row(rc, 2);
    lv_obj_set_flex_align(rr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    l_tot = w_label(rr, F_30, C_TXT, "--");
    w_label(rr, F_16, C_MUTED, "%");
    l_tot_ah = w_label(rc, F_14, C_MUTED, "");

    lv_obj_t *fl = w_rect(c, C_INSET, 16);
    lv_obj_set_size(fl, LV_PCT(100), 52);
    lv_obj_set_style_pad_hor(fl, 16, 0);
    lv_obj_set_flex_flow(fl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fl, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *lr = w_row(fl, 10);
    flow_dot = w_dot(lr, 10, C_MUTED);
    l_flow = w_label(lr, F_16, C_MUTED, "Idle");
    lv_obj_t *wr = w_row(fl, 4);
    lv_obj_set_flex_align(wr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    l_flow_w = w_label(wr, F_18, C_TXT, "0");
    w_label(wr, F_14, C_MUTED, "W");

    static int32_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t rows[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_t *g = w_box(c);
    lv_obj_set_width(g, LV_PCT(100));
    lv_obj_set_flex_grow(g, 1);
    lv_obj_set_grid_dsc_array(g, cols, rows);
    lv_obj_set_style_pad_column(g, 16, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *col = w_col(g, 12);
        lv_obj_set_grid_cell(col, LV_GRID_ALIGN_STRETCH, i, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *bar = w_rect(col, C_INSET, 20);
        lv_obj_set_width(bar, 84);
        lv_obj_set_flex_grow(bar, 1);
        B[i].fill = w_rect(bar, C_GREEN, 20);
        lv_obj_set_size(B[i].fill, LV_PCT(100), LV_PCT(0));
        lv_obj_align(B[i].fill, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_t *sr = w_row(col, 2);
        lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        B[i].soc = w_label(sr, F_40, C_TXT, "--");
        w_label(sr, F_20, C_MUTED, "%");
        lv_obj_t *ic = w_col(col, 2);
        lv_obj_set_flex_align(ic, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        B[i].amps = w_label(ic, F_16, C_MUTED, "");
        B[i].ah = w_label(ic, F_16, C_TXT2, "");
        lv_obj_t *nr = w_row(ic, 6);
        w_label(nr, F_14, C_MUTED, i ? "Battery 2" : "Battery 1");
        B[i].err = w_label(nr, F_14, C_RED, LV_SYMBOL_WARNING);
        w_show(B[i].err, false);
    }
}

static void heater_card(lv_obj_t *v)
{
    lv_obj_t *c = w_card(v);
    lv_obj_set_grid_cell(c, LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_style_pad_row(c, 20, 0);
    lv_obj_t *h = w_header(c, "Heater", "Truma Combi", &l_hsub);
    lv_obj_set_flex_grow(lv_obj_get_parent(l_hsub), 1);   // the status line shortens with "..." before the switch
    lv_obj_set_size(l_hsub, LV_PCT(100), lv_font_get_line_height(F_14));
    lv_label_set_long_mode(l_hsub, LV_LABEL_LONG_DOT);
    sw_heat = w_switch(h, on_heat, NULL);

    ctrls = w_col(c, 20);
    lv_obj_set_width(ctrls, LV_PCT(100));
    lv_obj_t *sp = w_rect(ctrls, C_INSET, 20);
    lv_obj_set_size(sp, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sp, 12, 0);
    lv_obj_set_flex_flow(sp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sp, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    b_minus = w_btn(sp, "-", F_40, 76, false, on_step, (void *)-1);
    lv_obj_set_width(b_minus, 68); w_bg(b_minus, C_BTN); lv_obj_set_style_radius(b_minus, 16, 0);
    lv_obj_t *mid = w_col(sp, 4);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *r = w_row(mid, 2);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    l_sp = w_label(r, F_48, C_ORANGE, "--");
    l_unit = w_label(r, F_28, C_TXT2, "°C");
    lv_obj_t *cap = w_label(mid, F_14, C_MUTED, "Room setpoint");
    l_roomnow = w_label(mid, F_16, C_TXT2, "");
    b_plus = w_btn(sp, "+", F_40, 76, false, on_step, (void *)1);
    lv_obj_set_width(b_plus, 68); w_bg(b_plus, C_BTN); lv_obj_set_style_radius(b_plus, 16, 0);
    sp_dim[0] = b_minus; sp_dim[1] = r; sp_dim[2] = cap; sp_dim[3] = b_plus;

    lv_obj_t *wg = w_col(ctrls, 10); lv_obj_set_width(wg, LV_PCT(100));
    w_label(wg, F_16, C_MUTED, "Hot water");
    seg_water = w_seg(wg, WATER_C, 68, F_18, on_water, NULL);
    lv_obj_t *fg = w_col(ctrls, 10); lv_obj_set_width(fg, LV_PCT(100));
    w_label(fg, F_16, C_MUTED, "Fan");
    seg_fan = w_seg(fg, FAN_MAP, 68, F_18, on_fan, NULL);

    w_spacer(c);
    lv_obj_t *logo = w_row(c, 10);
    lv_obj_set_width(logo, LV_PCT(100));
    lv_obj_set_flex_align(logo, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    w_tinted_image(logo, &img_logo_160, C_FAINT);
}



lv_obj_t *ui_energy_create(lv_obj_t *p)
{
    static int32_t cols[] = { 256, 248, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t rows[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_t *v = w_box(p);
    lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
    lv_obj_set_grid_dsc_array(v, cols, rows);
    lv_obj_set_style_pad_column(v, 12, 0);
    solar_card(v);
    battery_card(v);
    heater_card(v);
    return v;
}

void ui_energy_refresh(const vd_model_t *m)
{
    const vd_solar_t *s = &m->solar;
    if (s->valid) w_fmt(l_pv, "%d", (int)lroundf(s->pv_w)); else lv_label_set_text(l_pv, "--");
    int si = !s->valid ? -1 : s->stage == STAGE_BULK ? 0 : s->stage == STAGE_ABS ? 1 : s->stage == STAGE_FLOAT ? 2 : -1;
    for (int i = 0; i < 3; i++) { w_bg(st_bar[i], i <= si ? C_AMBER : C_INSET); w_color(st_lbl[i], i == si ? C_TXT : C_MUTED); }
    const char *smode = s->valid ? solar_mode_text(s->mode) : NULL;
    if (smode && si < 0) w_fmt(l_pvcap, "PV power - %s", smode); else lv_label_set_text(l_pvcap, "PV power");
    const char *et = s->valid && s->err ? solar_err_text(s->err) : NULL;
    if (s->valid && s->err) w_fmt(l_serr, "Err %u - %s", s->err, et ? et : "see Victron error codes");
    w_show(l_serr, s->valid && s->err);
    if (s->valid) {
        w_fmt(l_sbv, "%.2f", s->batt_v);
        w_fmt(l_sba, "%.1f", s->batt_a);
        w_fmt(l_scp, "%d", (int)lroundf(s->batt_v * s->batt_a));
    } else { lv_label_set_text(l_sbv, "--"); lv_label_set_text(l_sba, "--"); lv_label_set_text(l_scp, "--"); }
    bool load = s->valid && !isnan(s->load_a);
    if (load) w_fmt(l_sload, "%.1f", s->load_a);
    w_show(lv_obj_get_parent(lv_obj_get_parent(l_sload)), load);
    if (s->valid) w_fmt(l_yield, "%.2f", s->yield_wh / 1000.0f); else lv_label_set_text(l_yield, "--");

    float soc_sum = 0, ah = 0, full = 0, net = 0; int nv = 0;
    for (int i = 0; i < 2; i++) {
        const vd_batt_t *b = &m->batt[i];
        if (!b->valid) {
            lv_label_set_text(B[i].soc, "--"); lv_label_set_text(B[i].amps, ""); lv_label_set_text(B[i].ah, "");
            lv_obj_set_height(B[i].fill, 0);
            w_show(B[i].err, false);
            continue;
        }
        w_show(B[i].err, b->stale);
        nv++; soc_sum += b->soc; ah += b->remain_ah; full += b->full_ah; net += b->volts * b->amps;
        w_fmt(B[i].soc, "%d", (int)lroundf(b->soc));
        lv_obj_set_height(B[i].fill, LV_PCT((int)b->soc));
        w_bg(B[i].fill, b->soc < 20 ? C_RED : C_GREEN);
        w_fmt(B[i].amps, "%s%.1f A", b->amps > 0.05f ? "+" : b->amps < -0.05f ? "-" : "", fabsf(b->amps));
        w_fmt(B[i].ah, "%d Ah", (int)lroundf(b->remain_ah));
    }
    if (nv) {
        // Bank SOC = remaining / real full capacity, i.e. each battery's SOC weighted by its size.
        float avg = full > 0 ? ah / full * 100 : soc_sum / nv;
        if (avg > 100) avg = 100; else if (avg < 0) avg = 0;
        w_fmt(l_tot, "%d", (int)lroundf(avg));
        w_color(l_tot, avg < 20 ? C_RED : C_TXT);
        w_fmt(l_tot_ah, "%d / %d Ah", (int)lroundf(ah), (int)lroundf(full));
    } else { lv_label_set_text(l_tot, "--"); lv_label_set_text(l_tot_ah, ""); }
    int mode = !nv || fabsf(net) < 10 ? 0 : net > 0 ? 1 : 2;
    uint32_t fc = mode == 1 ? C_GREEN : mode == 2 ? C_ORANGE : C_MUTED;
    w_bg(flow_dot, fc); w_color(l_flow, fc);
    lv_label_set_text(l_flow, mode == 1 ? "Charging" : mode == 2 ? "Discharging" : "Idle");
    w_fmt(l_flow_w, "%s%d", net >= 10 ? "+" : net <= -10 ? "-" : "", (int)lroundf(fabsf(net)));

    bool on = g_cfg.heater_on;
    w_switch_set(sw_heat, on, C_ORANGE);
    const char *st = !m->heater.link ? "No LIN link"
#if CONFIG_VD_LIN_MODE_INETBOX
                   : !m->heater.registered ? "Run PR SET on the CP Plus"
#endif
                   : !on ? (g_cfg.water == WATER_OFF ? "Off" : "Water only")
                   : g_cfg.water == WATER_OFF ? "Heating" : "Heating + water";
    w_fmt(l_hsub, "Truma Combi - %s", st);
    for (int i = 0; i < 4; i++) lv_obj_set_style_opa(sp_dim[i], on ? LV_OPA_COVER : LV_OPA_40, 0);
    w_enable(b_minus, on); w_enable(b_plus, on); w_enable(seg_water, true); w_enable(seg_fan, true);
    char b[12]; ui_fmt_temp(b, sizeof b, g_cfg.setpoint_c);
    w_fmt(l_sp, "%s", b);
    lv_label_set_text(l_unit, ui_temp_unit());
    if (m->heater.link && !isnan(m->heater.room_c) && m->heater.room_c != 0) {
        char rb[12]; ui_fmt_temp(rb, sizeof rb, m->heater.room_c);
        w_fmt(l_roomnow, "Now %s%s", rb, ui_temp_unit());
        w_color(l_roomnow, m->heater.heating ? C_ORANGE : C_TXT2);
    } else lv_label_set_text(l_roomnow, "");
    if (s_unit_f != g_cfg.unit_f) {
        s_unit_f = g_cfg.unit_f;
        lv_buttonmatrix_set_map(seg_water, s_unit_f ? WATER_F : WATER_C);
        lv_buttonmatrix_set_button_ctrl_all(seg_water, LV_BUTTONMATRIX_CTRL_CHECKABLE);
        lv_obj_set_user_data(seg_water, NULL);
    }
    w_seg_set(seg_water, g_cfg.water);
    w_seg_set(seg_fan, g_cfg.fan);
}

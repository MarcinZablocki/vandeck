#include "ui_views.h"
#include "ui_widgets.h"
#include "app/settings.h"
#include "io/tanks.h"

typedef struct { lv_obj_t *seg[3], *seg_l[3], *e, *lvl, *status, *read_l, *btn; } tank_ui_t;
static tank_ui_t T[2];
static const char *NAMES[] = { "E", "1/3", "2/3", "F" };

static void on_read(lv_event_t *e) { tanks_read((int)(intptr_t)lv_event_get_user_data(e)); }

static void tank_card(lv_obj_t *v, int t, const char *title)
{
    tank_ui_t *u = &T[t];
    lv_obj_t *c = w_card(v);
    lv_obj_set_grid_cell(c, LV_GRID_ALIGN_STRETCH, t, 1, LV_GRID_ALIGN_STRETCH, 0, 1);
    lv_obj_set_style_pad_row(c, SZ(20, 12), 0);
    w_header(c, title, "KIB sensor", NULL);
    lv_obj_t *body = w_row(c, SZ(28, 14));
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *g = w_col(body, 6);
    lv_obj_set_size(g, SZ(150, 104), LV_PCT(100));
    for (int i = 0; i < 3; i++) {   // top to bottom: F, 2/3, 1/3
        u->seg[i] = w_rect(g, C_INSET, SZ(14, 10));
        lv_obj_set_width(u->seg[i], LV_PCT(100));
        lv_obj_set_flex_grow(u->seg[i], 1);
        u->seg_l[i] = w_label(u->seg[i], F_16, C_MUTED, NAMES[3 - i]);
        lv_obj_center(u->seg_l[i]);
    }
    lv_obj_t *eb = w_box(g);
    lv_obj_set_size(eb, LV_PCT(100), SZ(28, 22));
    u->e = w_label(eb, F_16, C_MUTED, "E");
    lv_obj_center(u->e);
    lv_obj_t *info = w_col(body, 10);
    lv_obj_set_height(info, LV_PCT(100));
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    u->lvl = w_label(info, SZ(F_NUM110, F_NUM64), C_FAINT, "--");
    u->status = w_label(info, SZ(F_22, F_18), C_MUTED, "Press Read now");
    lv_obj_set_width(u->status, LV_PCT(100));
    lv_label_set_long_mode(u->status, LV_LABEL_LONG_WRAP);

    lv_obj_t *rr = w_rect(c, C_INSET, SZ(16, 12));
    lv_obj_set_size(rr, LV_PCT(100), SZ(64, 52));
    lv_obj_set_style_pad_left(rr, 18, 0);
    lv_obj_set_style_pad_right(rr, 8, 0);
    lv_obj_set_flex_flow(rr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    u->read_l = w_label(rr, F_16, C_MUTED, "Not read");
    u->btn = w_btn(rr, "Read now", SZ(F_16, F_14), SZ(48, 40), true, on_read, (void *)(intptr_t)t);
}

lv_obj_t *ui_tanks_create(lv_obj_t *p)
{
    static int32_t cols[] = { LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    static int32_t rows[] = { LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST };
    lv_obj_t *v = w_box(p);
    lv_obj_set_size(v, LV_PCT(100), LV_PCT(100));
    lv_obj_set_grid_dsc_array(v, cols, rows);
    lv_obj_set_style_pad_column(v, SZ(12, 10), 0);
    tank_card(v, 0, "LPG");
    tank_card(v, 1, "Grey water");
    return v;
}

static uint32_t tank_color(int t, int l)
{
    if (t == 0) return l == 0 ? C_RED : l == 1 ? C_ORANGE : C_GREEN;
    return l == 3 ? C_RED : l == 2 ? C_ORANGE : C_BLUE;
}

void ui_tanks_refresh(const vd_model_t *m)
{
    static const char *ST[2][4] = {
        { "Empty - refill", "Low - refill soon", "OK", "Full" },
        { "Empty", "OK", "Dump soon", "Full - dump now" },
    };
    int64_t now = now_us();
    for (int t = 0; t < 2; t++) {
        tank_ui_t *u = &T[t];
        const vd_tank_t *k = &m->tank[t];
        bool shown = k->read_at_us && !k->reading &&
                     (g_cfg.tank_show_sec == 0 || now - k->read_at_us < (int64_t)g_cfg.tank_show_sec * 1000000);
        w_btn_text(u->btn, k->reading ? "Reading..." : "Read now");
        w_btn_state(u->btn, !k->reading, !k->reading);
        if (!shown || k->level < 0) {
            for (int i = 0; i < 3; i++) { w_bg(u->seg[i], C_INSET); w_color(u->seg_l[i], C_FAINT); }
            w_color(u->e, C_FAINT);
            lv_label_set_text(u->lvl, "--");
            w_color(u->lvl, C_FAINT);
            lv_label_set_text(u->status, k->reading ? "Reading sensor..." : shown ? "No sensor" : "Press Read now");
            w_color(u->status, C_MUTED);
            lv_label_set_text(u->read_l, k->reading ? "Measuring..." : "Not read");
            continue;
        }
        int l = k->level;
        uint32_t c = tank_color(t, l);
        for (int i = 0; i < 3; i++) {
            bool lit = l >= 3 - i;
            w_bg(u->seg[i], lit ? c : C_INSET);
            w_color(u->seg_l[i], lit ? C_BG : C_MUTED);
        }
        w_color(u->e, l == 0 ? c : C_MUTED);
        lv_label_set_text(u->lvl, NAMES[l]);
        w_color(u->lvl, l == 0 ? C_MUTED : c);
        lv_label_set_text(u->status, ST[t][l]);
        w_color(u->status, (c == C_GREEN || c == C_BLUE) ? C_TXT2 : c);
        int ago = (int)((now - k->read_at_us) / 1000000);
        w_fmt(u->read_l, "Read %ds ago", ago);
    }
}

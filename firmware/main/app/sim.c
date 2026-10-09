#include "sim.h"
#include "model.h"
#include "settings.h"
#include "esp_timer.h"
#include "esp_random.h"

static esp_timer_handle_t s_t;
static float rnd(void) { return (esp_random() & 0xFFFF) / 65535.0f; }

static void push(int k, float v)
{
    M.sl.v[k] = v;
    if (M.sl.hist_n < SL_HIST_LEN) M.sl.hist[k][M.sl.hist_n] = v;
    else { for (int i = 1; i < SL_HIST_LEN; i++) M.sl.hist[k][i - 1] = M.sl.hist[k][i]; M.sl.hist[k][SL_HIST_LEN - 1] = v; }
}

static void tick(void *arg)
{
    int64_t t = now_us();
    model_lock();
    bool demo = false;
    if (!g_cfg.dev[ROLE_SOLAR].paired) {
        demo = true;
        vd_solar_t *s = &M.solar;
        float pv = (s->valid ? s->pv_w : 380) + (rnd() - 0.5f) * 30;
        s->pv_w = pv < 0 ? 0 : pv > 440 ? 440 : pv;
        s->pv_v = 37.5f + rnd() * 2.4f;
        s->yield_wh += (uint16_t)(s->pv_w / 1800);
        s->batt_v = 13.6f; s->batt_a = s->pv_w * 0.96f / s->batt_v; s->load_a = NAN;
        s->stage = STAGE_BULK; s->mode = 3; s->err = 0; s->valid = true; s->ts_us = t;
    }
    for (int i = 0; i < 2; i++) {
        if (g_cfg.dev[ROLE_BATT1 + i].paired) continue;
        demo = true;
        vd_batt_t *b = &M.batt[i];
        if (!b->valid) b->soc = i ? 81 : 78;
        float net = M.solar.pv_w * 0.97f - 650;
        b->volts = 13.3f; b->amps = net / 13.3f / 2 * (i ? 1.04f : 0.96f);
        b->remain_ah = b->soc * CONFIG_VD_BATT_CAPACITY_AH / 100.0f;
        b->full_ah = CONFIG_VD_BATT_CAPACITY_AH;
        b->valid = true; b->ts_us = t;
    }
    if (!g_cfg.dev[ROLE_SHELLY].paired && M.sl.power) {
        demo = true;
        bool online = t - M.sl.power_since_us > 20LL * 1000000;
        if (online && !M.sl.reachable) M.sl.online_since_us = t;
        M.sl.reachable = online;
        if (online) {
            bool heat = g_cfg.sl_snow == SNOW_ON;
            push(SL_DOWN, 110 + rnd() * 110); push(SL_UP, 9 + rnd() * 14);
            push(SL_LAT, rnd() < 0.1f ? 70 + rnd() * 50 : 26 + rnd() * 18);
            push(SL_PWR, (heat ? 62 : 34) + rnd() * 8);
            push(SL_PING, rnd() < 0.12f ? 96 + rnd() * 3 : 99.5f + rnd() * 0.5f);
            push(SL_OBS, 0.3f + rnd() * 0.15f);
            if (M.sl.hist_n < SL_HIST_LEN) M.sl.hist_n++;
            M.sl.heating = heat;
        }
    } else if (!g_cfg.dev[ROLE_SHELLY].paired) { M.sl.reachable = false; M.sl.hist_n = 0; }
    M.demo = demo;
    model_commit();
}

void sim_start(void)
{
    esp_timer_create_args_t a = { .callback = tick, .name = "sim" };
    esp_timer_create(&a, &s_t);
    esp_timer_start_periodic(s_t, 2000000);
}

// Starlink dish status over grpc-web (HTTP/1.1, port 9201) — the same transport Dishylink uses.
// Field numbers below come from community reverse engineering; VERIFY them against the schema the
// dish serves (e.g. grpcurl -plaintext 192.168.100.1:9200 describe SpaceX.API.Device.DishGetStatusResponse).
// Do NOT send Referer/Origin headers: the dish answers with an empty body.
#include "starlink.h"
#include "app/model.h"
#include "app/settings.h"
#include "ble/relay.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "starlink";
#define SL_URL "http://192.168.100.1:9201/SpaceX.API.Device.Device/Handle"

// Request oneof fields
#define REQ_REBOOT       1001
#define REQ_GET_STATUS   1004
#define REQ_GET_LOCATION 1017
// Response oneof fields
#define RESP_DISH_STATUS 2004
#define RESP_LOCATION    1017   // GetLocationResponse: lla = 1 (LLAPosition: lat = 1, lon = 2, doubles) — VERIFY
#define LOC_EVERY_US     (5 * 60 * 1000000LL)
// DishGetStatusResponse fields — VERIFY
#define F_POP_DROP_RATE  1003   // float, 0..1
#define F_OBSTRUCTION    1004   // message; fraction_obstructed = field 1 (float)
#define F_DOWN_BPS       1007   // float
#define F_UP_BPS         1008   // float
#define F_POP_LATENCY_MS 1009   // float
#define F_ALERTS         1005   // message; is_heating = field 6 (bool) — VERIFY

static volatile bool s_reboot;

static int pb_put_empty(uint8_t *b, uint32_t field)
{
    uint32_t tag = (field << 3) | 2; int n = 0;
    while (tag >= 0x80) { b[n++] = (tag & 0x7F) | 0x80; tag >>= 7; }
    b[n++] = tag; b[n++] = 0;
    return n;
}

static bool pb_varint(const uint8_t **p, const uint8_t *e, uint64_t *v)
{
    *v = 0;
    for (int s = 0; *p < e && s < 64; s += 7) { uint8_t c = *(*p)++; *v |= (uint64_t)(c & 0x7F) << s; if (!(c & 0x80)) return true; }
    return false;
}

typedef void (*pb_cb)(uint32_t f, int wt, uint64_t v, const uint8_t *buf, uint32_t len, void *ctx);

static void pb_walk(const uint8_t *p, const uint8_t *e, pb_cb cb, void *ctx)
{
    while (p < e) {
        uint64_t tag, v = 0;
        if (!pb_varint(&p, e, &tag)) return;
        uint32_t f = tag >> 3; int wt = tag & 7;
        const uint8_t *buf = NULL; uint32_t len = 0;
        if (wt == 0) { if (!pb_varint(&p, e, &v)) return; }
        else if (wt == 1) { if (e - p < 8) return; memcpy(&v, p, 8); p += 8; }
        else if (wt == 5) { if (e - p < 4) return; uint32_t x; memcpy(&x, p, 4); v = x; p += 4; }
        else if (wt == 2) { uint64_t l; if (!pb_varint(&p, e, &l) || (uint64_t)(e - p) < l) return; buf = p; len = l; p += l; }
        else return;
        cb(f, wt, v, buf, len, ctx);
    }
}

static float as_f32(uint64_t v) { uint32_t x = (uint32_t)v; float f; memcpy(&f, &x, 4); return f; }

typedef struct { float down, up, lat, drop, obs; bool heating, got; } status_t;

static void obs_cb(uint32_t f, int wt, uint64_t v, const uint8_t *b, uint32_t l, void *ctx)
{ if (f == 1 && wt == 5) ((status_t *)ctx)->obs = as_f32(v); }
static void alert_cb(uint32_t f, int wt, uint64_t v, const uint8_t *b, uint32_t l, void *ctx)
{ if (f == 6 && wt == 0) ((status_t *)ctx)->heating = v != 0; }

static void dish_cb(uint32_t f, int wt, uint64_t v, const uint8_t *b, uint32_t l, void *ctx)
{
    status_t *s = ctx;
    if (wt == 5) {
        float x = as_f32(v);
        if (f == F_DOWN_BPS) s->down = x;
        else if (f == F_UP_BPS) s->up = x;
        else if (f == F_POP_LATENCY_MS) s->lat = x;
        else if (f == F_POP_DROP_RATE) s->drop = x;
    } else if (wt == 2 && f == F_OBSTRUCTION) pb_walk(b, b + l, obs_cb, s);
    else if (wt == 2 && f == F_ALERTS) pb_walk(b, b + l, alert_cb, s);
}

static void resp_cb(uint32_t f, int wt, uint64_t v, const uint8_t *b, uint32_t l, void *ctx)
{
    if (f == RESP_DISH_STATUS && wt == 2) { ((status_t *)ctx)->got = true; pb_walk(b, b + l, dish_cb, ctx); }
}

typedef struct { double lat, lon; bool lat_ok, lon_ok; } loc_t;

static void lla_cb(uint32_t f, int wt, uint64_t v, const uint8_t *b, uint32_t l, void *ctx)
{
    loc_t *p = ctx;
    if (wt != 1) return;
    double d; memcpy(&d, &v, 8);
    if (f == 1) { p->lat = d; p->lat_ok = true; } else if (f == 2) { p->lon = d; p->lon_ok = true; }
}
static void loc_cb(uint32_t f, int wt, uint64_t v, const uint8_t *b, uint32_t l, void *ctx)
{ if (f == 1 && wt == 2) pb_walk(b, b + l, lla_cb, ctx); }
static void loc_resp_cb(uint32_t f, int wt, uint64_t v, const uint8_t *b, uint32_t l, void *ctx)
{ if (f == RESP_LOCATION && wt == 2) pb_walk(b, b + l, loc_cb, ctx); }

static int grpc_call(uint32_t req_field, uint8_t *resp, int cap)
{
    uint8_t body[16]; uint8_t msg[8];
    int ml = pb_put_empty(msg, req_field);
    body[0] = 0; body[1] = body[2] = body[3] = 0; body[4] = ml;
    memcpy(body + 5, msg, ml);

    esp_http_client_config_t c = { .url = SL_URL, .method = HTTP_METHOD_POST, .timeout_ms = 1500 };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_http_client_set_header(h, "Content-Type", "application/grpc-web+proto");
    esp_http_client_set_header(h, "X-Grpc-Web", "1");
    int n = -1;
    if (esp_http_client_open(h, 5 + ml) == ESP_OK && esp_http_client_write(h, (char *)body, 5 + ml) == 5 + ml) {
        esp_http_client_fetch_headers(h);
        n = 0;
        int r;
        while (n < cap && (r = esp_http_client_read(h, (char *)resp + n, cap - n)) > 0) n += r;
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return n;
}

static void push(int k, float v)
{
    M.sl.v[k] = v;
    if (M.sl.hist_n < SL_HIST_LEN) M.sl.hist[k][M.sl.hist_n] = v;
    else { memmove(M.sl.hist[k], M.sl.hist[k] + 1, (SL_HIST_LEN - 1) * sizeof(float)); M.sl.hist[k][SL_HIST_LEN - 1] = v; }
}

// Ask the dish where it is. It only answers with "allow access on local network" switched on in the Starlink app
// (Settings > Advanced > Debug data); otherwise the reply carries no message, only a grpc-status in the trailers.
static void poll_location(uint8_t *buf, int cap)
{
    int n = grpc_call(REQ_GET_LOCATION, buf, cap);
    loc_t p = { 0 };
    if (n > 5 && buf[0] == 0) {
        uint32_t len = (uint32_t)buf[1] << 24 | buf[2] << 16 | buf[3] << 8 | buf[4];
        if (len <= (uint32_t)n - 5) pb_walk(buf + 5, buf + 5 + len, loc_resp_cb, &p);
    }
    bool ok = p.lat_ok && p.lon_ok && fabs(p.lat) <= 90 && fabs(p.lon) <= 180 && (p.lat != 0 || p.lon != 0);
    model_lock();
    M.sl.loc_ok = ok;
    M.sl.loc_denied = !ok && n >= 0;          // the dish answered, but without a position
    if (ok) { M.sl.lat = p.lat; M.sl.lon = p.lon; M.sl.loc_at_us = now_us(); }
    model_commit();
    if (ok) ESP_LOGI(TAG, "location %.4f, %.4f", p.lat, p.lon);
    else ESP_LOGI(TAG, "no location from the dish (enable local access in the Starlink app)");
}

static void task(void *arg)
{
    static uint8_t buf[4096];
    int64_t loc_next = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        model_lock();
        bool want = M.wifi.connected;                // the dish is on the Starlink Wi-Fi: no plug involved
        if (M.sl.power != want) { M.sl.power = want; M.sl.power_since_us = want ? now_us() : 0; model_commit(); }
        else model_unlock();
        if (!want) {
            model_lock();
            if (M.sl.reachable || M.sl.hist_n) { M.sl.reachable = false; M.sl.hist_n = 0; for (int i = 0; i < SL_NHIST; i++) M.sl.v[i] = NAN; model_commit(); }
            else model_unlock();
            continue;
        }
        if (s_reboot) { s_reboot = false; grpc_call(REQ_REBOOT, buf, sizeof buf); }

        int n = grpc_call(REQ_GET_STATUS, buf, sizeof buf);
        status_t s = { .down = NAN, .up = NAN, .lat = NAN, .drop = NAN, .obs = NAN };
        if (n > 5 && buf[0] == 0) {
            uint32_t len = (uint32_t)buf[1] << 24 | buf[2] << 16 | buf[3] << 8 | buf[4];
            if (len <= (uint32_t)n - 5) pb_walk(buf + 5, buf + 5 + len, resp_cb, &s);
        }
        model_lock();
        if (s.got && !M.sl.reachable) M.sl.online_since_us = now_us();
        M.sl.reachable = s.got;
        if (s.got) {
            push(SL_DOWN, s.down / 1e6f); push(SL_UP, s.up / 1e6f); push(SL_LAT, s.lat);
            push(SL_PWR, NAN);                       // not in get_status; meter it if you need it
            push(SL_PING, isnan(s.drop) ? NAN : (1 - s.drop) * 100);
            push(SL_OBS, isnan(s.obs) ? NAN : s.obs * 100);
            if (M.sl.hist_n < SL_HIST_LEN) M.sl.hist_n++;
            M.sl.heating = s.heating;
        }
        if (!s.got) { M.sl.loc_ok = false; M.sl.loc_denied = false; loc_next = 0; }
        model_commit();
        if (s.got && now_us() >= loc_next) { poll_location(buf, sizeof buf); loc_next = now_us() + LOC_EVERY_US; }
    }
}

void starlink_init(void) { xTaskCreate(task, "starlink", 6144, NULL, 3, NULL); }

void starlink_set_power(bool on)
{
    if (g_cfg.dev[ROLE_RELAY].paired) { relay_set_output(on); return; }
    model_lock();
    M.sl.power = on;
    M.sl.power_since_us = on ? now_us() : 0;
    model_commit();
}

void starlink_reboot(void) { s_reboot = true; }

void starlink_set_snow(uint8_t mode)
{
    model_lock(); g_cfg.sl_snow = mode; model_commit();
    settings_save_later();
    // TODO: send dish_set_config (snow-melt mode) once the field numbers are confirmed.
}

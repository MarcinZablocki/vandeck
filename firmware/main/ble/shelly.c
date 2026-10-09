// Shelly Gen2 RPC over BLE (Shelly Plus Uni). Connects on demand, sends one RPC, disconnects.
// Frame: write 4-byte big-endian length to TX_CTL, write JSON to DATA, read RX_CTL for the reply
// length, then read DATA until complete. Devices with auth enabled need digest auth (not handled).
#include "shelly.h"
#include "gatt_link.h"
#include "app/model.h"
#include "app/settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "shelly";
enum { C_DATA, C_TX, C_RX };
static const ble_uuid128_t U_DATA = BLE_UUID128_INIT(0x5f,0x5f,0x5f,0x61,0x74,0x61,0x64,0x5f,0x43,0x50,0x52,0x5f,0x53,0x4f,0x6d,0x5f);
static const ble_uuid128_t U_TX   = BLE_UUID128_INIT(0x5f,0x6c,0x74,0x63,0x5f,0x78,0x74,0x5f,0x43,0x50,0x52,0x5f,0x53,0x4f,0x6d,0x5f);
static const ble_uuid128_t U_RX   = BLE_UUID128_INIT(0x5f,0x6c,0x74,0x63,0x5f,0x78,0x72,0x5f,0x43,0x50,0x52,0x5f,0x53,0x4f,0x6d,0x5f);

static gatt_link_t L;
static char s_req[160];
static int s_req_len, s_sent, s_want = -1;      // s_want: requested output state for Switch.Set, -1 = status query
static char s_resp[512];
static uint32_t s_resp_len, s_resp_got;
static int s_rx_tries;
static bool s_busy;
static esp_timer_handle_t s_retry_t, s_timeout_t;

static void finish(void) { s_busy = false; esp_timer_stop(s_timeout_t); gl_disconnect(&L); }

static void handle_response(void)
{
    s_resp[s_resp_got < sizeof s_resp ? s_resp_got : sizeof s_resp - 1] = 0;
    ESP_LOGI(TAG, "rpc reply: %s", s_resp);
    bool ok = strstr(s_resp, "\"result\"") != NULL;
    int on = -1;
    if (s_want >= 0 && ok) on = s_want;
    else if (strstr(s_resp, "\"output\":true")) on = 1;
    else if (strstr(s_resp, "\"output\":false")) on = 0;
    model_lock();
    M.dev_state[ROLE_SHELLY] = ok ? DEV_ON : DEV_LOST;
    if (on >= 0) {
        if (on && !M.sl.power) M.sl.power_since_us = now_us();
        M.sl.power = on;
    }
    model_commit();
    finish();
}

static void read_data(gatt_link_t *l);
static void on_data(gatt_link_t *l, int chr, const uint8_t *d, uint16_t n)
{
    uint32_t room = sizeof s_resp - 1 - s_resp_got;
    memcpy(s_resp + s_resp_got, d, n < room ? n : room);
    s_resp_got += n;
    if (s_resp_got >= s_resp_len || n == 0) handle_response(); else read_data(l);
}
static void read_data(gatt_link_t *l) { if (gl_read(l, C_DATA, on_data)) finish(); }

static void read_rx_ctl(gatt_link_t *l);
static void retry_rx(void *a) { read_rx_ctl(&L); }
static void on_rx_ctl(gatt_link_t *l, int chr, const uint8_t *d, uint16_t n)
{
    s_resp_len = n >= 4 ? ((uint32_t)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3]) : 0;
    if (!s_resp_len) {
        if (++s_rx_tries > 20) { finish(); return; }
        esp_timer_start_once(s_retry_t, 100000);
        return;
    }
    s_resp_got = 0;
    read_data(l);
}
static void read_rx_ctl(gatt_link_t *l) { if (gl_read(l, C_RX, on_rx_ctl)) finish(); }

static void send_chunk(gatt_link_t *l)
{
    if (s_sent >= s_req_len) { s_rx_tries = 0; read_rx_ctl(l); return; }
    int chunk = gl_mtu(l) - 3, n = s_req_len - s_sent;
    if (n > chunk) n = chunk;
    const char *p = s_req + s_sent;
    s_sent += n;
    if (gl_write(l, C_DATA, p, n, send_chunk)) finish();
}

static void ready(gatt_link_t *l)
{
    uint8_t len[4] = { s_req_len >> 24, s_req_len >> 16, s_req_len >> 8, s_req_len };
    s_sent = 0;
    if (gl_write(l, C_TX, len, 4, send_chunk)) finish();
}

static void down(gatt_link_t *l) { if (s_busy) { s_busy = false; model_lock(); M.dev_state[ROLE_SHELLY] = DEV_LOST; model_commit(); } }
static void timeout(void *a) { ESP_LOGW(TAG, "rpc timeout"); finish(); }

static void rpc(int want, const char *method, const char *params)
{
    if (!g_cfg.dev[ROLE_SHELLY].paired || s_busy) return;
    s_want = want;
    s_req_len = snprintf(s_req, sizeof s_req, "{\"id\":1,\"src\":\"vandeck\",\"method\":\"%s\",\"params\":%s}", method, params);
    memcpy(L.addr.val, g_cfg.dev[ROLE_SHELLY].addr, 6);
    L.addr.type = g_cfg.dev[ROLE_SHELLY].addr_type;
    s_busy = true;
    esp_timer_start_once(s_timeout_t, 10LL * 1000000);
    if (gl_connect(&L)) finish();
}

void shelly_init(void)
{
    memset(&L, 0, sizeof L);
    gl_init(&L);
    L.uuid[C_DATA].u128 = U_DATA; L.uuid[C_TX].u128 = U_TX; L.uuid[C_RX].u128 = U_RX;
    L.n_chr = 3;
    L.on_ready = ready; L.on_down = down;
    esp_timer_create_args_t a = { .callback = retry_rx, .name = "shelly_rx" };
    esp_timer_create(&a, &s_retry_t);
    esp_timer_create_args_t b = { .callback = timeout, .name = "shelly_to" };
    esp_timer_create(&b, &s_timeout_t);
}

void shelly_on_seen(const ble_addr_t *addr) { (void)addr; }
void shelly_set_output(bool on) { rpc(on, "Switch.Set", on ? "{\"id\":0,\"on\":true}" : "{\"id\":0,\"on\":false}"); }
void shelly_refresh(void) { rpc(-1, "Switch.GetStatus", "{\"id\":0}"); }
void shelly_forget(void) { finish(); }

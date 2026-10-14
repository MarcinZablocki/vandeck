// Starlink relay node (see ../../relay): an ESP32 driving a 12 V relay, advertised as "VanRelay".
// One characteristic: read = 1 byte (0 off, 1 on); write = 1 byte state + the shared key. Connects on demand,
// does one read or write, disconnects.
#include "relay.h"
#include "gatt_link.h"
#include "app/model.h"
#include "app/settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "relay";
static const ble_uuid128_t U_STATE = BLE_UUID128_INIT(0x01,0xa0,0x72,0x65,0x6c,0x61,0x79,0x6e,0x6f,0x64,0x65,0x56,0x41,0x4e,0x00,0x02);

static gatt_link_t L;
static int s_want = -1;          // requested state, -1 = read only
static bool s_busy;
static esp_timer_handle_t s_timeout_t;

static void finish(void) { s_busy = false; esp_timer_stop(s_timeout_t); gl_disconnect(&L); }

static void set_state(int on)
{
    model_lock();
    M.dev_state[ROLE_RELAY] = DEV_ON;
    if (on >= 0) {
        if (on && !M.sl.power) M.sl.power_since_us = now_us();
        M.sl.power = on;
    }
    model_commit();
}

static void on_read(gatt_link_t *l, int chr, const uint8_t *d, uint16_t n)
{
    if (n >= 1) set_state(d[0] ? 1 : 0);
    finish();
}

static void wrote(gatt_link_t *l)
{
    set_state(s_want);   // the relay answers a write only after switching
    finish();
}

static void ready(gatt_link_t *l)
{
    int rc;
    if (s_want >= 0) {
        uint8_t buf[1 + sizeof CONFIG_VD_RELAY_KEY];
        size_t k = strlen(CONFIG_VD_RELAY_KEY);
        buf[0] = s_want;
        memcpy(buf + 1, CONFIG_VD_RELAY_KEY, k);
        rc = gl_write(l, 0, buf, 1 + k, wrote);
    } else rc = gl_read(l, 0, on_read);
    if (rc) finish();
}

static void down(gatt_link_t *l)
{
    if (!s_busy) return;
    s_busy = false;
    model_lock(); M.dev_state[ROLE_RELAY] = DEV_LOST; model_commit();
}
static void timeout(void *a) { ESP_LOGW(TAG, "timeout"); finish(); }

static void go(int want)
{
    if (!g_cfg.dev[ROLE_RELAY].paired || s_busy) return;
    s_want = want;
    memcpy(L.addr.val, g_cfg.dev[ROLE_RELAY].addr, 6);
    L.addr.type = g_cfg.dev[ROLE_RELAY].addr_type;
    s_busy = true;
    esp_timer_start_once(s_timeout_t, 8LL * 1000000);
    if (gl_connect(&L)) finish();
}

void relay_init(void)
{
    memset(&L, 0, sizeof L);
    gl_init(&L);
    L.uuid[0].u128 = U_STATE;
    L.n_chr = 1;
    L.on_ready = ready; L.on_down = down;
    esp_timer_create_args_t b = { .callback = timeout, .name = "relay_to" };
    esp_timer_create(&b, &s_timeout_t);
}

void relay_on_seen(const ble_addr_t *addr) { (void)addr; }
void relay_set_output(bool on) { go(on); }
void relay_refresh(void) { go(-1); }
void relay_forget(void) { finish(); }

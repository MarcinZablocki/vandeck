// VanRelay: BLE-controlled 12 V relay for the Starlink supply. The VanDeck panel connects, reads the state
// or writes {state, key}. Pinout and wiring: README.md.
#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "relay";
#define NAME "VanRelay"

// Same bytes as the panel's U_STATE in firmware/main/ble/relay.c.
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(0x00,0xa0,0x72,0x65,0x6c,0x61,0x79,0x6e,0x6f,0x64,0x65,0x56,0x41,0x4e,0x00,0x01);
static const ble_uuid128_t CHR_UUID = BLE_UUID128_INIT(0x01,0xa0,0x72,0x65,0x6c,0x61,0x79,0x6e,0x6f,0x64,0x65,0x56,0x41,0x4e,0x00,0x02);

static uint8_t s_own;
static bool s_on;
static esp_timer_handle_t s_loss_t;

// Active-low modules: open-drain, so "off" lets the module's own pull-up release the opto instead of fighting it.
static void set_pin(bool on)
{
#if CONFIG_RELAY_ACTIVE_LOW
    gpio_set_level(CONFIG_RELAY_GPIO, !on);
#else
    gpio_set_level(CONFIG_RELAY_GPIO, on);
#endif
}

static void apply(bool on, bool save)
{
    s_on = on;
    set_pin(on);
    ESP_LOGI(TAG, "relay %s", on ? "ON" : "OFF");
    if (save) {
        nvs_handle_t h;
        if (nvs_open("relay", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "on", on); nvs_commit(h); nvs_close(h); }
    }
}

static void arm_loss_timer(void)
{
#if CONFIG_RELAY_LINK_LOSS_OFF_MIN > 0
    esp_timer_stop(s_loss_t);
    esp_timer_start_once(s_loss_t, CONFIG_RELAY_LINK_LOSS_OFF_MIN * 60LL * 1000000);
#endif
}
static void loss_cb(void *a) { ESP_LOGW(TAG, "panel silent, switching off"); apply(false, false); }

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    arm_loss_timer();
    if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t v = s_on;
        return os_mbuf_append(ctx->om, &v, 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    uint8_t buf[32];
    uint16_t n = OS_MBUF_PKTLEN(ctx->om);
    if (n < 1 || n > sizeof buf) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    ble_hs_mbuf_to_flat(ctx->om, buf, sizeof buf, &n);
    size_t k = strlen(CONFIG_RELAY_KEY);
    if (n != 1 + k || memcmp(buf + 1, CONFIG_RELAY_KEY, k)) { ESP_LOGW(TAG, "bad key"); return BLE_ATT_ERR_INSUFFICIENT_AUTHOR; }
    apply(buf[0] != 0, true);
    return 0;
}

static const struct ble_gatt_svc_def GATT[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &SVC_UUID.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
        { .uuid = &CHR_UUID.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
        { 0 } } },
    { 0 }
};

static int gap_cb(struct ble_gap_event *ev, void *arg);
static void advertise(void)
{
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)NAME; f.name_len = strlen(NAME); f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_gap_adv_params p = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN };
    int rc = ble_gap_adv_start(s_own, NULL, BLE_HS_FOREVER, &p, gap_cb, NULL);
    if (rc) ESP_LOGW(TAG, "adv rc=%d", rc);
}
static int gap_cb(struct ble_gap_event *ev, void *arg)
{
    if (ev->type == BLE_GAP_EVENT_CONNECT && ev->connect.status != 0) advertise();
    if (ev->type == BLE_GAP_EVENT_DISCONNECT) advertise();
    return 0;
}
static void on_sync(void) { ble_hs_util_ensure_addr(0); ble_hs_id_infer_auto(0, &s_own); advertise(); }
static void host_task(void *p) { nimble_port_run(); nimble_port_freertos_deinit(); }

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }

    // Park the line in the "off" level before configuring it as an output, so the relay can't glitch on.
    set_pin(false);
    gpio_config_t io = { .pin_bit_mask = 1ULL << CONFIG_RELAY_GPIO,
#if CONFIG_RELAY_ACTIVE_LOW
        .mode = GPIO_MODE_OUTPUT_OD };
#else
        .mode = GPIO_MODE_OUTPUT };
#endif
    gpio_config(&io);
    set_pin(false);

    bool on = false;
#if CONFIG_RELAY_BOOT_ON
    on = true;
#elif CONFIG_RELAY_BOOT_LAST
    nvs_handle_t h; uint8_t v = 0;
    if (nvs_open("relay", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "on", &v); nvs_close(h); }
    on = v;
#endif
    apply(on, false);

    esp_timer_create_args_t a = { .callback = loss_cb, .name = "loss" };
    esp_timer_create(&a, &s_loss_t);
    arm_loss_timer();

    nimble_port_init();
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init(); ble_svc_gatt_init();
    ble_svc_gap_device_name_set(NAME);
    ble_gatts_count_cfg(GATT); ble_gatts_add_svcs(GATT);
    nimble_port_freertos_init(host_task);
}

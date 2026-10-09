// Tailscale node (microlink). One task owns the microlink handle: it starts it once the user has switched the
// tailnet on, a key (or stored node credentials) is available and Wi-Fi is up, rebinds it after a Wi-Fi
// reconnect, and mirrors its state into M.tn for the UI and shell. microlink_stop/destroy can take seconds,
// which is why none of this runs on the caller's task.
#include "tailnet_svc.h"
#include "app/model.h"
#include "app/settings.h"
#include "microlink.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

// TODO(telemetry): push battery/solar/tank readings out to a service on the tailnet (microlink_tcp_connect /
// microlink_udp_send once M.tn.state == TN_CONNECTED). Not built yet: for now the tailnet only serves the
// existing endpoints (OTA, /version, /status, console).

static const char *TAG = "tailnet";
#define NS        "vd_tail"          // NVS: "on" (u8), "key" (auth key string)
#define KEY_MAX   128
#define RETRY_US  (30LL * 1000000)   // after microlink_init/start fails (out of memory, ...)

static SemaphoreHandle_t s_lock;     // guards the fields below that the UI and shell set
static bool s_on;
static char s_key[KEY_MAX];          // auth key to register with ("" = none)
static bool s_restart_req, s_reset_req;

// Owned by the task. microlink keeps the pointers it is given, so these must outlive the handle.
static microlink_t *s_ml;
static char s_run_key[KEY_MAX];
static bool s_wifi_prev;
static int64_t s_retry_at;

static void nvs_put_on(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "on", on);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_put_key(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (key[0]) nvs_set_str(h, "key", key); else nvs_erase_key(h, "key");
    nvs_commit(h);
    nvs_close(h);
}

static void publish(vd_tn_state_t st, uint32_t ip, int peers, bool has_key, bool has_creds)
{
    if (peers > 255) peers = 255;
    model_lock();
    bool chg = M.tn.state != st || M.tn.ip != ip || M.tn.peers != peers || M.tn.has_key != has_key || M.tn.has_creds != has_creds;
    if (chg) {
        if (M.tn.state != st) ESP_LOGI(TAG, "%s", tailnet_svc_state_text(st));
        M.tn = (vd_tailnet_t){ .state = st, .has_key = has_key, .has_creds = has_creds, .ip = ip, .peers = peers };
        model_commit();
    } else model_unlock();
}

static void teardown(void)
{
    if (!s_ml) return;
    esp_err_t e = microlink_stop(s_ml);
    if (e != ESP_OK) ESP_LOGW(TAG, "stop: %s (a worker was still busy; it is released later)", esp_err_to_name(e));
    microlink_destroy(s_ml);
    s_ml = NULL;
}

static bool start(const char *key)
{
    strlcpy(s_run_key, key, sizeof s_run_key);
    microlink_config_t c = {
        .auth_key = s_run_key,     // "" is fine once the node has registered
        .device_name = CONFIG_VD_TAILNET_HOSTNAME,
        .enable_derp = true, .enable_stun = true, .enable_disco = true,
    };
    s_ml = microlink_init(&c);
    if (!s_ml) { ESP_LOGE(TAG, "microlink_init failed"); return false; }
    if (microlink_start(s_ml) != ESP_OK) {
        ESP_LOGE(TAG, "microlink_start failed");
        teardown();
        return false;
    }
    s_wifi_prev = true;
    ESP_LOGI(TAG, "starting as '%s' (this task's stack: %u bytes never used)", CONFIG_VD_TAILNET_HOSTNAME,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return true;
}

static vd_tn_state_t map_state(microlink_state_t st)
{
    switch (st) {
    case ML_STATE_CONNECTED: return TN_CONNECTED;
    case ML_STATE_AUTH_FAILED: return TN_AUTH_FAILED;
    case ML_STATE_ERROR: return TN_ERROR;
    case ML_STATE_IDLE: case ML_STATE_WIFI_WAIT: return TN_WAIT_WIFI;
    default: return TN_CONNECTING;   // CONNECTING, REGISTERING, RECONNECTING
    }
}

static void task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        char key[KEY_MAX];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool on = s_on, restart = s_restart_req, reset = s_reset_req;
        strlcpy(key, s_key, sizeof key);
        s_restart_req = s_reset_req = false;
        xSemaphoreGive(s_lock);

        model_lock();
        bool wifi = M.wifi.connected;
        bool wifi_enabled = g_cfg.wifi_on;
        model_unlock();

        if (restart || reset || !on || !wifi_enabled) teardown();   // a changed key or a switch-off starts clean
        if (reset) microlink_factory_reset();                        // erases the node keys; must not be running
        bool creds = microlink_has_stored_credentials();

        if (!on) { publish(TN_OFF, 0, 0, key[0], creds); continue; }
        if (!s_ml) {
            if (!key[0] && !creds) { publish(TN_NEED_KEY, 0, 0, false, false); continue; }
            if (!wifi || esp_timer_get_time() < s_retry_at) { publish(TN_WAIT_WIFI, 0, 0, key[0], creds); continue; }
            if (!start(key)) { s_retry_at = esp_timer_get_time() + RETRY_US; publish(TN_ERROR, 0, 0, key[0], creds); continue; }
        }
        if (wifi && !s_wifi_prev) {   // Wi-Fi came back, possibly with another address: move the sockets over
            ESP_LOGI(TAG, "Wi-Fi is back, rebinding");
            microlink_rebind(s_ml);
        }
        s_wifi_prev = wifi;
        publish(map_state(microlink_get_state(s_ml)), microlink_get_vpn_ip(s_ml), microlink_get_peer_count(s_ml), key[0], creds);
    }
}

void tailnet_svc_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t on = 0; size_t n = sizeof s_key;
        nvs_get_u8(h, "on", &on);
        if (nvs_get_str(h, "key", s_key, &n) != ESP_OK) s_key[0] = 0;
        s_on = on;
        nvs_close(h);
    }
    publish(s_on ? TN_WAIT_WIFI : TN_OFF, 0, 0, s_key[0], false);
    // The 8 KB covers what microlink_init/start run on the caller's stack (first-run key generation).
    xTaskCreate(task, "tailnet", 8192, NULL, 4, NULL);
}

bool tailnet_svc_enabled(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool on = s_on;
    xSemaphoreGive(s_lock);
    return on;
}

void tailnet_svc_set_enabled(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_on = on;
    xSemaphoreGive(s_lock);
    nvs_put_on(on);
}

esp_err_t tailnet_svc_set_auth_key(const char *key)
{
    size_t n = key ? strlen(key) : 0;
    if (n && (n >= KEY_MAX || n < 20 || strncmp(key, "tskey-", 6))) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_key, key ? key : "", sizeof s_key);
    s_restart_req = true;
    xSemaphoreGive(s_lock);
    nvs_put_key(key ? key : "");
    return ESP_OK;
}

void tailnet_svc_forget(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_on = false;
    s_key[0] = 0;
    s_reset_req = true;
    xSemaphoreGive(s_lock);
    nvs_put_on(false);
    nvs_put_key("");
}

const char *tailnet_svc_state_text(vd_tn_state_t st)
{
    static const char *T[] = { "Off", "Needs an auth key", "Waiting for Wi-Fi", "Connecting", "Connected",
                               "Auth key rejected", "Error" };
    return st <= TN_ERROR ? T[st] : "?";
}

const char *tailnet_svc_hostname(void) { return CONFIG_VD_TAILNET_HOSTNAME; }

void tailnet_svc_ip_str(uint32_t ip, char *buf, size_t n)
{
    char t[16];
    if (!ip) { strlcpy(buf, "--", n); return; }
    microlink_ip_to_str(ip, t);
    strlcpy(buf, t, n);
}

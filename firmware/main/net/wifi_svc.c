// Wi-Fi via the ESP32-C6 co-processor: esp_wifi_remote exposes the normal esp_wifi API on the P4.
#include "wifi_svc.h"
#include "time_svc.h"
#include "app/model.h"
#include "app/settings.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi";
static esp_timer_handle_t s_retry;
static bool s_started;

static void retry_cb(void *a) { if (g_cfg.wifi_on && g_cfg.wifi_ssid[0]) esp_wifi_connect(); }

static int by_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

static void on_scan_done(void)
{
    uint16_t n = 20;
    wifi_ap_record_t *r = calloc(n, sizeof *r);
    if (!r) return;
    esp_wifi_scan_get_ap_records(&n, r);
    qsort(r, n, sizeof *r, by_rssi);
    model_lock();
    M.wifi.n_nets = 0;
    for (int i = 0; i < n && M.wifi.n_nets < WIFI_MAX_NETS; i++) {
        if (!r[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < M.wifi.n_nets; j++) if (!strcmp(M.wifi.nets[j].ssid, (char *)r[i].ssid)) dup = true;
        if (dup) continue;
        vd_net_t *nn = &M.wifi.nets[M.wifi.n_nets++];
        strlcpy(nn->ssid, (char *)r[i].ssid, sizeof nn->ssid);
        nn->rssi = r[i].rssi;
        nn->secure = r[i].authmode != WIFI_AUTH_OPEN;
    }
    M.wifi.scanning = false;
    model_commit();
    free(r);
}

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_cfg.wifi_ssid[0]) esp_wifi_connect();
        wifi_svc_scan();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        model_lock();
        M.wifi.connected = false;
        if (M.wifi.joining[0] && d->reason != WIFI_REASON_ASSOC_LEAVE) { M.wifi.joining[0] = 0; M.wifi.failed = true; }
        model_commit();
        ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
        if (g_cfg.wifi_on) { esp_timer_stop(s_retry); esp_timer_start_once(s_retry, 10LL * 1000000); }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        on_scan_done();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_config_t c;
        esp_wifi_get_config(WIFI_IF_STA, &c);
        model_lock();
        M.wifi.connected = true; M.wifi.failed = false; M.wifi.joining[0] = 0;
        strlcpy(g_cfg.wifi_ssid, (char *)c.sta.ssid, sizeof g_cfg.wifi_ssid);
        model_commit();
        settings_save_later();
        time_svc_on_network();
    }
}

void wifi_svc_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    esp_timer_create(&ta, &s_retry);
    if (g_cfg.wifi_on) { esp_wifi_start(); s_started = true; }
}

void wifi_svc_set_enabled(bool on)
{
    model_lock();
    g_cfg.wifi_on = on;
    if (!on) { M.wifi.connected = false; M.wifi.n_nets = 0; M.wifi.joining[0] = 0; }
    model_commit();
    settings_save_later();
    if (on && !s_started) { esp_wifi_start(); s_started = true; }
    else if (!on && s_started) { esp_timer_stop(s_retry); esp_wifi_stop(); s_started = false; }
}

void wifi_svc_scan(void)
{
    if (!s_started) return;
    // A scan takes the radio off the network for seconds; never during a firmware upload.
    model_lock(); bool ota = M.ota.state == OTA_RECEIVING; model_unlock();
    if (ota) return;
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) { model_lock(); M.wifi.scanning = true; model_commit(); }
}

void wifi_svc_connect(const char *ssid, const char *pass)
{
    wifi_config_t c = { 0 };
    strlcpy((char *)c.sta.ssid, ssid, sizeof c.sta.ssid);
    strlcpy((char *)c.sta.password, pass ? pass : "", sizeof c.sta.password);
    c.sta.threshold.authmode = (pass && *pass) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    esp_wifi_disconnect();
    model_lock();
    strlcpy(M.wifi.joining, ssid, sizeof M.wifi.joining);
    M.wifi.failed = false;
    model_commit();
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_wifi_connect();
}

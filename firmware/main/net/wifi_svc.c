// Wi-Fi via the ESP32-C6 co-processor: esp_wifi_remote exposes the normal esp_wifi API on the P4.
//
// Remembered networks (up to WIFI_SAVED_MAX) live in the P4's own NVS (vd_wifi), newest first. The driver's own
// storage is RAM only: it used to be the one copy, kept on the C6, and a C6 that came up in a bad state forgot it.
// Whenever the panel isn't connected it scans and joins the strongest remembered network in range; one that fails
// three times in a row is passed over until the others have had a go. A network is remembered once it connects, so
// a mistyped password never is.
#include "wifi_svc.h"
#include "time_svc.h"
#include "app/model.h"
#include "app/settings.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi";
#define NS          "vd_wifi"
#define MAX_FAILS   3

typedef struct { char ssid[33]; char pass[65]; } saved_t;

static saved_t s_saved[WIFI_SAVED_MAX];
static uint8_t s_fails[WIFI_SAVED_MAX];
static int s_n_saved;
static char s_try_pass[65];                 // password of the join in progress, saved if it gets an address
static esp_timer_handle_t s_retry;
static bool s_started;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_lock;            // the saved list: Wi-Fi events, the UI and the console all use it
#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

// --- Saved list

static void saved_store(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "nets", s_saved, sizeof s_saved[0] * s_n_saved);
    nvs_commit(h);
    nvs_close(h);
}

static int saved_find(const char *ssid)
{
    for (int i = 0; i < s_n_saved; i++) if (!strcmp(s_saved[i].ssid, ssid)) return i;
    return -1;
}

// Newest first; a full list drops the oldest.
static void saved_put(const char *ssid, const char *pass)
{
    LOCK();
    int i = saved_find(ssid);
    if (i == 0 && !strcmp(s_saved[0].pass, pass)) { UNLOCK(); return; }
    saved_t n = { 0 };
    strlcpy(n.ssid, ssid, sizeof n.ssid);
    strlcpy(n.pass, pass, sizeof n.pass);
    if (i < 0) i = s_n_saved < WIFI_SAVED_MAX ? s_n_saved++ : WIFI_SAVED_MAX - 1;
    memmove(&s_saved[1], &s_saved[0], sizeof s_saved[0] * i);
    memmove(&s_fails[1], &s_fails[0], sizeof s_fails[0] * i);
    s_saved[0] = n;
    s_fails[0] = 0;
    saved_store();
    ESP_LOGI(TAG, "remembered %s (%d saved)", ssid, s_n_saved);
    UNLOCK();
}

static void saved_load(void)
{
    nvs_handle_t h;
    size_t sz = sizeof s_saved;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "nets", s_saved, &sz) == ESP_OK) s_n_saved = sz / sizeof s_saved[0];
        nvs_close(h);
    }
    if (s_n_saved) return;
    // First start with this list: take over the one network the driver kept (on the C6) until now.
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_STA, &c) == ESP_OK && c.sta.ssid[0]) saved_put((char *)c.sta.ssid, (char *)c.sta.password);
}

// --- Joining

static void join(const char *ssid, const char *pass)
{
    wifi_config_t c = { 0 };
    strlcpy((char *)c.sta.ssid, ssid, sizeof c.sta.ssid);
    strlcpy((char *)c.sta.password, pass, sizeof c.sta.password);
    c.sta.threshold.authmode = *pass ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    taskENTER_CRITICAL(&s_mux);
    strlcpy(s_try_pass, pass, sizeof s_try_pass);
    taskEXIT_CRITICAL(&s_mux);
    esp_wifi_disconnect();
    model_lock();
    strlcpy(M.wifi.joining, ssid, sizeof M.wifi.joining);
    M.wifi.failed = false;
    model_commit();
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_wifi_connect();
}

// The strongest remembered network in the scan, passing over ones that keep failing unless nothing else is there.
// Returns false when none is in range.
static bool pick(const wifi_ap_record_t *r, int n)
{
    saved_t c;
    LOCK();
    int best = -1, best_rssi = -127, fallback = -1;
    for (int k = 0; k < n; k++) {   // r is sorted strongest first
        int i = saved_find((const char *)r[k].ssid);
        if (i < 0) continue;
        if (fallback < 0) fallback = i;
        if (s_fails[i] < MAX_FAILS && r[k].rssi > best_rssi) { best = i; best_rssi = r[k].rssi; }
    }
    if (best < 0 && fallback >= 0) {   // everything in range has failed: start the counts over
        memset(s_fails, 0, sizeof s_fails);
        best = fallback;
    }
    if (best >= 0) c = s_saved[best];
    UNLOCK();
    if (best < 0) return false;
    ESP_LOGI(TAG, "joining %s", c.ssid);
    join(c.ssid, c.pass);
    return true;
}

static void retry_cb(void *a) { wifi_svc_scan(); }

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
        nn->saved = wifi_svc_is_saved(nn->ssid);
    }
    M.wifi.scanning = false;
    bool idle = g_cfg.wifi_on && !M.wifi.connected && !M.wifi.joining[0];
    model_commit();
    // Not connected and no remembered network in range: look again in 30 s (driving into range, an AP rebooting).
    if (idle && !pick(r, n)) { esp_timer_stop(s_retry); esp_timer_start_once(s_retry, 30LL * 1000000); }
    free(r);
}

static void on_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        wifi_svc_scan();                  // the scan picks a network
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        char ssid[33];
        model_lock();
        bool joining = M.wifi.joining[0];
        strlcpy(ssid, joining ? M.wifi.joining : g_cfg.wifi_ssid, sizeof ssid);
        M.wifi.connected = false;
        if (joining && d->reason != WIFI_REASON_ASSOC_LEAVE) { M.wifi.joining[0] = 0; M.wifi.failed = true; }
        model_commit();
        LOCK();
        int i = saved_find(ssid);
        if (joining && i >= 0 && s_fails[i] < 255) s_fails[i]++;
        UNLOCK();
        ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
        // Rescan rather than retrying blindly: the network may be gone, and another remembered one in range.
        if (g_cfg.wifi_on) { esp_timer_stop(s_retry); esp_timer_start_once(s_retry, 5LL * 1000000); }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        on_scan_done();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_config_t c;
        esp_wifi_get_config(WIFI_IF_STA, &c);
        char pass[65];
        taskENTER_CRITICAL(&s_mux);
        strlcpy(pass, s_try_pass, sizeof pass);
        taskEXIT_CRITICAL(&s_mux);
        model_lock();
        M.wifi.connected = true; M.wifi.failed = false; M.wifi.joining[0] = 0;
        strlcpy(g_cfg.wifi_ssid, (char *)c.sta.ssid, sizeof g_cfg.wifi_ssid);
        for (int k = 0; k < M.wifi.n_nets; k++) if (!strcmp(M.wifi.nets[k].ssid, g_cfg.wifi_ssid)) M.wifi.nets[k].saved = true;
        model_commit();
        settings_save_later();
        saved_put((char *)c.sta.ssid, pass);
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
    s_lock = xSemaphoreCreateRecursiveMutex();
    saved_load();                         // before switching the driver to RAM: it reads the old flash copy
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_evt, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_evt, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
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
    else if (g_cfg.wifi_on) { esp_timer_stop(s_retry); esp_timer_start_once(s_retry, 10LL * 1000000); }   // busy: later
}

void wifi_svc_connect(const char *ssid, const char *pass)
{
    LOCK();
    int i = saved_find(ssid);
    if (i >= 0) s_fails[i] = 0;
    UNLOCK();
    join(ssid, pass ? pass : "");
}

bool wifi_svc_connect_saved(const char *ssid)
{
    char pass[65];
    LOCK();
    int i = saved_find(ssid);
    if (i >= 0) strlcpy(pass, s_saved[i].pass, sizeof pass);
    UNLOCK();
    if (i < 0) return false;
    wifi_svc_connect(ssid, pass);
    return true;
}

bool wifi_svc_is_saved(const char *ssid)
{
    LOCK();
    bool r = saved_find(ssid) >= 0;
    UNLOCK();
    return r;
}

int wifi_svc_saved(char (*ssids)[33], int max)
{
    LOCK();
    int n = s_n_saved < max ? s_n_saved : max;
    for (int i = 0; i < n; i++) strlcpy(ssids[i], s_saved[i].ssid, 33);
    UNLOCK();
    return n;
}

bool wifi_svc_forget(const char *ssid)
{
    LOCK();
    int i = saved_find(ssid);
    if (i < 0) { UNLOCK(); return false; }
    memmove(&s_saved[i], &s_saved[i + 1], sizeof s_saved[0] * (s_n_saved - i - 1));
    memmove(&s_fails[i], &s_fails[i + 1], sizeof s_fails[0] * (s_n_saved - i - 1));
    s_n_saved--;
    memset(&s_saved[s_n_saved], 0, sizeof s_saved[0]);
    saved_store();
    UNLOCK();
    model_lock();
    for (int k = 0; k < M.wifi.n_nets; k++) if (!strcmp(M.wifi.nets[k].ssid, ssid)) M.wifi.nets[k].saved = false;
    model_commit();
    ESP_LOGI(TAG, "forgot %s", ssid);
    return true;
}

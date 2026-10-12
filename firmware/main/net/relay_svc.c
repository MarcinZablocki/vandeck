// Cloudflare relay link: one outbound WebSocket to the vandeck-link Worker (cloud/). See relay_svc.h.
//
// Panel -> Worker: {"type":"status","data":<GET /status JSON>} every 10 s and right after a command;
//                  {"type":"ack","id":"..","ok":true|false,"error":".."} for each command.
// Worker -> panel: {"type":"cmd","id":"..","set":{"heater_on":bool,"setpoint_c":num,"water":"off|eco|hot",
//                  "fan":"eco|high"}}; any subset of "set". It goes through the same truma_set_* calls as the
//                  touch screen and Matter, so the change shows everywhere.
// The key (the Worker's PANEL_KEY secret) is sent as a bearer token on the WebSocket upgrade, over TLS checked
// against the certificate bundle. URL, key and on/off are in NVS (vd_relay); the key is never logged.
#include "relay_svc.h"
#include "status_api.h"
#include "app/model.h"
#include "app/settings.h"
#include "io/truma_lin.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "relay";
#define NS        "vd_relay"
#define PERIOD_MS 10000

static char s_url[160], s_key[128];
static bool s_on;
static volatile bool s_restart;           // URL or key changed: the task reconnects
static volatile vd_relay_state_t s_state = RELAY_OFF;
static esp_websocket_client_handle_t s_ws;
static TaskHandle_t s_task;
static char *s_buf;                        // PSRAM: status message
static char s_ack[160];                    // reply to the last command, sent by the task
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void nvs_put(const char *k, const char *v, int u8)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (v) nvs_set_str(h, k, v); else nvs_set_u8(h, k, (uint8_t)u8);
    nvs_commit(h);
    nvs_close(h);
}

static void wake(void) { if (s_task) xTaskNotifyGive(s_task); }

// Runs on the WebSocket client's task: apply, then leave the reply and a fresh status to the relay task.
static void on_command(const char *data, int len)
{
    cJSON *j = cJSON_ParseWithLength(data, len);
    if (!j) return;
    const cJSON *type = cJSON_GetObjectItem(j, "type"), *id = cJSON_GetObjectItem(j, "id"), *set = cJSON_GetObjectItem(j, "set");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "cmd") || !cJSON_IsString(id) || !cJSON_IsObject(set)) { cJSON_Delete(j); return; }
    for (const char *p = id->valuestring; *p; p++)   // echoed back unescaped: ids are the Worker's UUIDs
        if (!(*p == '-' || (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) { cJSON_Delete(j); return; }

    const char *err = NULL;
    const cJSON *on = cJSON_GetObjectItem(set, "heater_on"), *sp = cJSON_GetObjectItem(set, "setpoint_c");
    const cJSON *water = cJSON_GetObjectItem(set, "water"), *fan = cJSON_GetObjectItem(set, "fan");
    int w = -1, f = -1;
    if (cJSON_IsString(water)) {
        w = !strcmp(water->valuestring, "off") ? WATER_OFF : !strcmp(water->valuestring, "eco") ? WATER_ECO
          : !strcmp(water->valuestring, "hot") ? WATER_HOT : -2;
        if (w == -2) err = "water must be off, eco or hot";
    }
    if (cJSON_IsString(fan)) {
        f = !strcmp(fan->valuestring, "eco") ? FAN_ECO : !strcmp(fan->valuestring, "high") ? FAN_HIGH : -2;
        if (f == -2) err = "fan must be eco or high";
    }
    if (sp && (!cJSON_IsNumber(sp) || sp->valuedouble < 5 || sp->valuedouble > 30)) err = "setpoint_c must be 5..30";
    if (on && !cJSON_IsBool(on)) err = "heater_on must be true or false";
    if (!err) {   // all checked first: a bad field changes nothing
        if (on) truma_set_power(cJSON_IsTrue(on));
        if (sp) truma_set_setpoint((float)sp->valuedouble);
        if (w >= 0) truma_set_water((uint8_t)w);
        if (f >= 0) truma_set_fan((uint8_t)f);
        ESP_LOGI(TAG, "command from the website applied");
    } else ESP_LOGW(TAG, "command refused: %s", err);

    char ack[sizeof s_ack];
    snprintf(ack, sizeof ack, "{\"type\":\"ack\",\"id\":\"%.40s\",\"ok\":%s,\"error\":%s%s%s}", id->valuestring,
             err ? "false" : "true", err ? "\"" : "", err ? err : "null", err ? "\"" : "");
    taskENTER_CRITICAL(&s_mux);
    strlcpy(s_ack, ack, sizeof s_ack);
    taskEXIT_CRITICAL(&s_mux);
    cJSON_Delete(j);
    wake();
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        s_state = RELAY_CONNECTED;
        wake();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_state == RELAY_CONNECTED) ESP_LOGW(TAG, "disconnected");
        s_state = RELAY_CONNECTING;
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "connection failed");
        break;
    case WEBSOCKET_EVENT_DATA:
        // Commands are small: take whole text frames only.
        if (d->op_code == 1 && d->payload_offset == 0 && d->data_len == d->payload_len) on_command(d->data_ptr, d->data_len);
        break;
    default:
        break;
    }
}

static void stop_client(void)
{
    if (!s_ws) return;
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
}

static void start_client(void)
{
    static char headers[160];
    snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\n", s_key);
    esp_websocket_client_config_t cfg = {
        .uri = s_url,
        .headers = headers,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 15000,
        .network_timeout_ms = 15000,
        .ping_interval_sec = 30,          // keeps NAT mappings (Starlink, campsite routers) open
        .buffer_size = 1024,
        .task_stack = 6144,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) { ESP_LOGE(TAG, "no memory for the client"); return; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws, NULL);
    s_state = RELAY_CONNECTING;
    esp_websocket_client_start(s_ws);
}

static void send_status(void)
{
    static const char HEAD[] = "{\"type\":\"status\",\"data\":";
    size_t n = sizeof HEAD - 1;
    memcpy(s_buf, HEAD, n);
    size_t j = status_api_json(s_buf + n, STATUS_JSON_MAX - 2);
    if (!j) return;
    n += j;
    s_buf[n++] = '}';
    esp_websocket_client_send_text(s_ws, s_buf, n, pdMS_TO_TICKS(5000));
}

static void relay_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PERIOD_MS));
        if (s_restart) { s_restart = false; stop_client(); }
        bool configured = s_url[0] && s_key[0];
        if (!s_on || !configured) {
            stop_client();
            s_state = s_on ? RELAY_NOT_SET : RELAY_OFF;
            continue;
        }
        model_lock();
        bool wifi = M.wifi.connected;
        model_unlock();
        if (!s_ws) {
            if (!wifi) { s_state = RELAY_WAIT_WIFI; continue; }
            start_client();   // from here the client reconnects by itself, Wi-Fi drops included
        }
        if (!s_ws || !esp_websocket_client_is_connected(s_ws)) continue;
        char ack[sizeof s_ack];
        taskENTER_CRITICAL(&s_mux);
        strlcpy(ack, s_ack, sizeof ack);
        s_ack[0] = 0;
        taskEXIT_CRITICAL(&s_mux);
        if (ack[0]) esp_websocket_client_send_text(s_ws, ack, strlen(ack), pdMS_TO_TICKS(5000));
        send_status();
    }
}

void relay_svc_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof s_url;
        if (nvs_get_str(h, "url", s_url, &n) != ESP_OK) s_url[0] = 0;
        n = sizeof s_key;
        if (nvs_get_str(h, "key", s_key, &n) != ESP_OK) s_key[0] = 0;
        uint8_t on = 0;
        nvs_get_u8(h, "on", &on);
        s_on = on;
        nvs_close(h);
    }
    s_buf = heap_caps_malloc(STATUS_JSON_MAX + 32, MALLOC_CAP_SPIRAM);
    if (!s_buf) { ESP_LOGE(TAG, "no memory"); return; }
    xTaskCreate(relay_task, "relay", 4096, NULL, 3, &s_task);
}

vd_relay_state_t relay_svc_state(void) { return s_state; }
const char *relay_svc_url(void) { return s_url; }
bool relay_svc_enabled(void) { return s_on; }

const char *relay_svc_state_text(vd_relay_state_t st)
{
    static const char *T[] = { "Off", "URL or key not set", "Waiting for Wi-Fi", "Connecting", "Connected" };
    return st <= RELAY_CONNECTED ? T[st] : "?";
}

void relay_svc_set_enabled(bool on)
{
    s_on = on;
    nvs_put("on", NULL, on);
    wake();
}

bool relay_svc_set_url(const char *url)
{
    if (strncasecmp(url, "wss://", 6) || strlen(url) >= sizeof s_url) return false;
    strlcpy(s_url, url, sizeof s_url);
    nvs_put("url", s_url, 0);
    s_restart = true;   // the relay task reconnects with the new one
    wake();
    return true;
}

bool relay_svc_set_key(const char *key)
{
    size_t n = strlen(key);
    if (n < 32 || n >= sizeof s_key) return false;
    for (const char *p = key; *p; p++) if (*p <= ' ' || *p == '"' || *p > '~') return false;
    strlcpy(s_key, key, sizeof s_key);
    nvs_put("key", s_key, 0);
    s_restart = true;
    wake();
    return true;
}

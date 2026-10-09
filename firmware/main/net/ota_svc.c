// OTA over the local network. The image streams straight into the inactive app slot; the bootloader only switches
// to it once it's complete and verified, and rolls back if the new image doesn't confirm itself (see main.c).
#include "ota_svc.h"
#include "status_api.h"
#include "app/model.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_log.h"
#include "mdns.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "ota";
static volatile bool s_busy;
static ota_install_t s_install;

// NVS "vd_ota": sha/at/via = the running image's ELF SHA-256 and when/how it was installed;
// pend_sha/pend_at = the image last written by OTA (becomes sha/at once it boots and confirms itself).
#define NS "vd_ota"

static bool clock_set(time_t t) { struct tm tm; localtime_r(&t, &tm); return tm.tm_year + 1900 >= 2024; }

static void set_state(vd_ota_state_t st, int pct, const char *msg)
{
    model_lock();
    M.ota.state = st;
    if (pct >= 0) M.ota.pct = pct;
    if (msg) strlcpy(M.ota.msg, msg, sizeof M.ota.msg);
    model_commit();
}

static esp_err_t fail(httpd_req_t *r, esp_ota_handle_t h, const char *status, const char *msg)
{
    if (h) esp_ota_abort(h);
    ESP_LOGE(TAG, "%s", msg);
    set_state(OTA_FAILED, -1, msg);
    s_busy = false;
    httpd_resp_set_status(r, status);
    httpd_resp_set_hdr(r, "Connection", "close");   // the client may still be sending; don't leave it waiting
    httpd_resp_sendstr(r, msg);
    return ESP_OK;
}

static void reboot_task(void *a)
{
    vTaskDelay(pdMS_TO_TICKS(1500));   // let the HTTP reply and the "restarting" screen get out
    esp_restart();
}

bool ota_svc_claim(void) { return !__atomic_exchange_n(&s_busy, true, __ATOMIC_ACQ_REL); }
void ota_svc_release(void) { s_busy = false; }
void ota_svc_set_state(vd_ota_state_t st, int pct, const char *msg) { set_state(st, pct, msg); }

esp_err_t ota_svc_commit(const esp_partition_t *dst, bool quiet)
{
    if (esp_ota_set_boot_partition(dst) != ESP_OK) return ESP_FAIL;
    esp_app_desc_t nd;
    nvs_handle_t nv;
    if (nvs_open(NS, NVS_READWRITE, &nv) == ESP_OK) {
        if (esp_ota_get_partition_description(dst, &nd) == ESP_OK) {
            time_t now = time(NULL);
            nvs_set_blob(nv, "pend_sha", nd.app_elf_sha256, sizeof nd.app_elf_sha256);
            nvs_set_i64(nv, "pend_at", clock_set(now) ? now : 0);
        }
        if (quiet) nvs_set_u8(nv, "quiet", 1);
        nvs_commit(nv);
        nvs_close(nv);
    }
    ESP_LOGI(TAG, "update written to %s, restarting", dst->label);
    if (!quiet) set_state(OTA_DONE, 100, "Restarting");
    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

bool ota_svc_take_quiet_boot(void)
{
    nvs_handle_t nv;
    uint8_t q = 0;
    if (nvs_open(NS, NVS_READWRITE, &nv) != ESP_OK) return false;
    if (nvs_get_u8(nv, "quiet", &q) == ESP_OK) { nvs_erase_key(nv, "quiet"); nvs_commit(nv); }
    nvs_close(nv);
    return q;
}

bool ota_svc_check_key(httpd_req_t *r)
{
    char key[64] = { 0 };
    return httpd_req_get_hdr_value_str(r, "X-OTA-Key", key, sizeof key) == ESP_OK && !strcmp(key, CONFIG_VD_OTA_KEY);
}

static esp_err_t on_ota(httpd_req_t *r)
{
    if (!ota_svc_check_key(r)) {
        httpd_resp_set_status(r, "403 Forbidden");
        return httpd_resp_sendstr(r, "bad or missing X-OTA-Key\n");
    }
    if (!ota_svc_claim()) { httpd_resp_set_status(r, "409 Conflict"); return httpd_resp_sendstr(r, "update already running\n"); }

    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    size_t total = r->content_len;
    if (!dst) return fail(r, 0, "500 Internal Server Error", "no OTA slot\n");
    if (total == 0 || total > dst->size) return fail(r, 0, "413 Payload Too Large", "image size doesn't fit the OTA slot\n");
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)total, dst->label);
    model_lock(); M.ota.version[0] = 0; model_unlock();
    set_state(OTA_RECEIVING, 0, "Receiving update");

    const size_t hdr_len = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    char *buf = malloc(4096);
    if (!buf) return fail(r, 0, "500 Internal Server Error", "out of memory\n");
    esp_ota_handle_t h = 0;
    size_t got = 0, fill = 0;
    int last_pct = -1;
    while (got < total) {
        // Until the image header is complete, keep appending to the buffer (a read can return less than asked).
        int n = httpd_req_recv(r, buf + fill, 4096 - fill);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { free(buf); return fail(r, h, "400 Bad Request", "upload interrupted\n"); }
        if (!h) {
            fill += n;
            if (fill < hdr_len && got + fill < total) continue;
            n = fill; fill = 0;
            // The header carries the app description: refuse anything that isn't a VanDeck image before touching
            // flash (e.g. another project built for the same board).
            if (n < hdr_len) { free(buf); return fail(r, 0, "400 Bad Request", "not a firmware image\n"); }
            const esp_app_desc_t *d = (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
            if (d->magic_word != ESP_APP_DESC_MAGIC_WORD || strcmp(d->project_name, esp_app_get_description()->project_name)) {
                free(buf);
                return fail(r, 0, "400 Bad Request", "not a VanDeck firmware image\n");
            }
            model_lock(); strlcpy(M.ota.version, d->version, sizeof M.ota.version); model_unlock();
            ESP_LOGI(TAG, "image: %s %s (running %s)", d->project_name, d->version, esp_app_get_description()->version);
            if (esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
                free(buf);
                return fail(r, 0, "500 Internal Server Error", "can't start OTA\n");
            }
        }
        if (esp_ota_write(h, buf, n) != ESP_OK) { free(buf); return fail(r, h, "500 Internal Server Error", "flash write failed\n"); }
        got += n;
        int pct = got * 100 / total;
        if (pct != last_pct) { last_pct = pct; set_state(OTA_RECEIVING, pct, NULL); }
    }
    free(buf);

    esp_err_t e = esp_ota_end(h);   // validates the complete image (format, chip, checksum/hash)
    if (e != ESP_OK) {
        s_busy = false;
        set_state(OTA_FAILED, -1, e == ESP_ERR_OTA_VALIDATE_FAILED ? "Image failed verification" : "OTA end failed");
        httpd_resp_set_status(r, "400 Bad Request");
        return httpd_resp_sendstr(r, "image failed verification\n");
    }
    if (ota_svc_commit(dst, false) != ESP_OK) return fail(r, 0, "500 Internal Server Error", "can't select new image\n");
    return httpd_resp_sendstr(r, "OK, restarting\n");
}

static esp_err_t on_version(httpd_req_t *r)
{
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *p = esp_ota_get_running_partition();
    char b[160];
    snprintf(b, sizeof b, "{\"project\":\"%s\",\"version\":\"%s\",\"built\":\"%s %s\",\"slot\":\"%s\"}\n",
             d->project_name, d->version, d->date, d->time, p ? p->label : "?");
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, b);
}

void ota_svc_init(void)
{
    model_lock(); M.ota.state = OTA_IDLE; model_unlock();

    // Same image as last boot: the install record is already known. A new one is recorded at confirmation time,
    // when the clock has usually been set by SNTP.
    nvs_handle_t nv;
    uint8_t sha[32]; size_t n = sizeof sha; int64_t at = 0; uint8_t via = 0;
    if (nvs_open(NS, NVS_READONLY, &nv) == ESP_OK) {
        if (nvs_get_blob(nv, "sha", sha, &n) == ESP_OK && n == 32 && !memcmp(sha, esp_app_get_description()->app_elf_sha256, 32)) {
            nvs_get_i64(nv, "at", &at);
            nvs_get_u8(nv, "via", &via);
            s_install = (ota_install_t){ .at = (time_t)at, .via_ota = via, .known = true };
        }
        nvs_close(nv);
    }

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("vandeck");
        mdns_instance_name_set("VanDeck");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    } else ESP_LOGW(TAG, "mDNS unavailable; use the panel's IP address");

    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 6144;
    c.max_open_sockets = 3;          // OTA + /version need few; leaves sockets for the console and the rest
    c.lru_purge_enable = true;
    // Wi-Fi shares the C6's radio with BLE (battery links, scanning), so reads can stall for seconds.
    c.recv_wait_timeout = 30;
    c.send_wait_timeout = 30;
    httpd_handle_t srv;
    if (httpd_start(&srv, &c) != ESP_OK) { ESP_LOGE(TAG, "HTTP server failed to start"); return; }
    httpd_uri_t ota = { .uri = "/ota", .method = HTTP_POST, .handler = on_ota };
    httpd_uri_t ver = { .uri = "/version", .method = HTTP_GET, .handler = on_version };
    httpd_register_uri_handler(srv, &ota);
    httpd_register_uri_handler(srv, &ver);
    status_api_register(srv);
    ESP_LOGI(TAG, "ready: POST http://vandeck.local/ota (running %s)", esp_app_get_description()->version);
}

ota_install_t ota_svc_last_install(void) { return s_install; }

static void record_install(void)
{
    const uint8_t *cur = esp_app_get_description()->app_elf_sha256;
    uint8_t sha[32], pend[32];
    size_t n = sizeof sha;
    int64_t at = 0;
    uint8_t via = 0;
    nvs_handle_t nv;
    if (nvs_open(NS, NVS_READWRITE, &nv) != ESP_OK) return;
    if (nvs_get_blob(nv, "sha", sha, &n) == ESP_OK && n == 32 && !memcmp(sha, cur, 32)) {
        nvs_get_i64(nv, "at", &at);
        nvs_get_u8(nv, "via", &via);
    } else {
        // A new image: it came over the air if it's the one OTA last wrote, otherwise it was flashed over USB.
        n = sizeof pend;
        bool ota = nvs_get_blob(nv, "pend_sha", pend, &n) == ESP_OK && n == 32 && !memcmp(pend, cur, 32);
        time_t now = time(NULL);
        if (ota) nvs_get_i64(nv, "pend_at", &at);
        if (!at && clock_set(now)) at = now;
        via = ota;
        nvs_set_blob(nv, "sha", cur, 32);
        nvs_set_i64(nv, "at", at);
        nvs_set_u8(nv, "via", via);
        nvs_erase_key(nv, "pend_sha");
        nvs_commit(nv);
        ESP_LOGI(TAG, "recorded install of %s via %s", esp_app_get_description()->version, via ? "OTA" : "USB");
    }
    nvs_close(nv);
    s_install = (ota_install_t){ .at = (time_t)at, .via_ota = via, .known = true };
}

void ota_svc_mark_valid(void)
{
    record_install();
    esp_ota_img_states_t st;
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed; rollback cancelled");
    }
}

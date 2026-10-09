// Firmware updates from GitHub releases. The latest release's version.json names the version, the download URL,
// its size and SHA-256; it is fetched through github.com/<repo>/releases/latest/download/, which isn't subject to
// the GitHub API's per-IP rate limit (Starlink shares addresses). The image goes through the same path as a
// Wi-Fi upload: written to the inactive slot, verified, and rolled back if it doesn't confirm itself after boot.
#include "update_svc.h"
#include "ota_svc.h"
#include "app/model.h"
#include "app/settings.h"
#include "net/time_svc.h"
#include "ui/ui.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *TAG = "update";
#define BASE           "https://github.com/" CONFIG_VD_UPDATE_REPO
#define FIRST_CHECK_US (2 * 60 * 1000000LL)        // after boot, once the network has settled
#define CHECK_EVERY_US (12 * 3600 * 1000000LL)
#define RETRY_US       (30 * 60 * 1000000LL)
#define NIGHT_FROM     2                           // automatic installs: local hours [2, 5)
#define NIGHT_TO       5
#define IDLE_MS        (15 * 60 * 1000)           // and nobody has touched the panel for this long

typedef struct { char version[32], url[256], sha[65]; uint32_t size; } rel_t;

static volatile bool s_check, s_install;
static rel_t s_rel;                                // the release on offer (update task only)
static char s_ua[72];
static char s_auto_failed[32];                     // an automatic install of this version failed: don't retry it

bool update_svc_enabled(void) { return CONFIG_VD_UPDATE_REPO[0] != 0; }
void update_svc_check(void) { s_check = true; }
void update_svc_install(void) { s_install = true; }

static void set_upd(vd_upd_state_t st, const char *msg)
{
    model_lock();
    M.upd.state = st;
    strlcpy(M.upd.msg, msg ? msg : "", sizeof M.upd.msg);
    model_commit();
}

// "0.5.6" > "0.5.10"? Compare numerically; a leading "v" is ignored. Unparseable versions never count as newer.
static bool newer(const char *a, const char *b)
{
    int x[3], y[3];
    if (*a == 'v') a++;
    if (*b == 'v') b++;
    if (sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]) != 3 || sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]) != 3) return false;
    for (int i = 0; i < 3; i++) if (x[i] != y[i]) return x[i] > y[i];
    return false;
}

// GET that follows GitHub's redirects (release asset -> its download host). Returns an open handle at status 200.
static esp_http_client_handle_t open_follow(const char *url)
{
    esp_http_client_config_t c = { .url = url, .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach,
                                   .buffer_size = 4096, .buffer_size_tx = 2048, .user_agent = s_ua };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return NULL;
    for (int hop = 0; hop < 5; hop++) {
        if (esp_http_client_open(h, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(h);
        int st = esp_http_client_get_status_code(h);
        if (st == 200) return h;
        if (st != 301 && st != 302 && st != 303 && st != 307 && st != 308) { ESP_LOGW(TAG, "HTTP %d", st); break; }
        esp_http_client_flush_response(h, NULL);
        if (esp_http_client_set_redirection(h) != ESP_OK) break;
        esp_http_client_close(h);
    }
    esp_http_client_cleanup(h);
    return NULL;
}

static void close_http(esp_http_client_handle_t h) { esp_http_client_close(h); esp_http_client_cleanup(h); }

static const char *jstr(const cJSON *j, const char *k) { return cJSON_GetStringValue(cJSON_GetObjectItem(j, k)); }

// version.json: {"project":"vandeck","version":"0.5.6","url":".../releases/download/v0.5.6/vandeck.bin",
//                "size":2551216,"sha256":"<64 hex>"}
static bool fetch_release(rel_t *r, char *err, size_t en)
{
    esp_http_client_handle_t h = open_follow(BASE "/releases/latest/download/version.json");
    if (!h) { snprintf(err, en, "No release found"); return false; }
    char *buf = calloc(1, 2048);
    int n = 0, k;
    while (buf && n < 2047 && (k = esp_http_client_read(h, buf + n, 2047 - n)) > 0) n += k;
    close_http(h);
    cJSON *j = buf ? cJSON_Parse(buf) : NULL;
    free(buf);
    const char *proj = jstr(j, "project"), *ver = jstr(j, "version"), *url = jstr(j, "url"), *sha = jstr(j, "sha256");
    const cJSON *sz = cJSON_GetObjectItem(j, "size");
    static const char PREFIX[] = BASE "/releases/download/";   // only this repository's own release assets
    bool ok = proj && ver && url && sha && cJSON_IsNumber(sz) && sz->valuedouble > 0 &&
              !strcmp(proj, esp_app_get_description()->project_name) && strlen(sha) == 64 &&
              !strncmp(url, PREFIX, sizeof PREFIX - 1) && strlen(ver) < sizeof r->version && strlen(url) < sizeof r->url;
    if (ok) {
        strlcpy(r->version, ver, sizeof r->version);
        strlcpy(r->url, url, sizeof r->url);
        strlcpy(r->sha, sha, sizeof r->sha);
        r->size = (uint32_t)sz->valuedouble;
    } else snprintf(err, en, "Release has no valid version.json");
    cJSON_Delete(j);
    return ok;
}

static void progress(bool quiet, int pct)
{
    model_lock();
    M.upd.pct = pct;
    model_commit();
    if (!quiet) ota_svc_set_state(OTA_RECEIVING, pct, NULL);
}

// Download into the inactive slot, checking project, version, size and SHA-256 before it can boot.
static bool install(const rel_t *r, bool quiet, char *err, size_t en)
{
    if (!ota_svc_claim()) { snprintf(err, en, "Another update is running"); return false; }
    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    esp_http_client_handle_t h = NULL;
    esp_ota_handle_t oh = 0;
    char *buf = malloc(4096);
    psa_hash_operation_t sh = PSA_HASH_OPERATION_INIT;
    bool ok = false;
    snprintf(err, en, "Download failed");

    set_upd(UPD_INSTALLING, NULL);
    if (!quiet) {
        model_lock(); strlcpy(M.ota.version, r->version, sizeof M.ota.version); model_unlock();
        ota_svc_set_state(OTA_RECEIVING, 0, "Downloading update");
    }
    progress(quiet, 0);
    if (!buf || !dst || r->size > dst->size || psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&sh, PSA_ALG_SHA_256) != PSA_SUCCESS) { snprintf(err, en, "Can't start the update"); goto out; }
    ESP_LOGI(TAG, "downloading %s (%u bytes) into %s", r->version, (unsigned)r->size, dst->label);
    if (!(h = open_follow(r->url))) goto out;

    const size_t hdr_len = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    size_t got = 0, fill = 0;
    int last = -1, n;
    while ((n = esp_http_client_read(h, buf + fill, 4096 - fill)) > 0) {
        if (!oh) {
            // Hold back until the app description is in: refuse anything but the announced VanDeck version.
            fill += n;
            if (fill < hdr_len) continue;
            n = fill; fill = 0;
            const esp_app_desc_t *d = (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
            if (d->magic_word != ESP_APP_DESC_MAGIC_WORD || strcmp(d->project_name, esp_app_get_description()->project_name) ||
                strcmp(d->version, r->version)) { snprintf(err, en, "Not the announced firmware"); goto out; }
            if (esp_ota_begin(dst, OTA_WITH_SEQUENTIAL_WRITES, &oh) != ESP_OK) { snprintf(err, en, "Can't start OTA"); goto out; }
        }
        if (got + n > r->size) { snprintf(err, en, "Image larger than announced"); goto out; }
        if (esp_ota_write(oh, buf, n) != ESP_OK) { snprintf(err, en, "Flash write failed"); goto out; }
        psa_hash_update(&sh, (const uint8_t *)buf, n);
        got += n;
        int pct = (int)((uint64_t)got * 100 / r->size);
        if (pct != last) { last = pct; progress(quiet, pct); }
    }
    if (got != r->size) { snprintf(err, en, "Download interrupted"); goto out; }

    uint8_t dig[32]; size_t dl = 0;
    char hex[65];
    if (psa_hash_finish(&sh, dig, sizeof dig, &dl) != PSA_SUCCESS || dl != 32) goto out;
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", dig[i]);
    if (strcasecmp(hex, r->sha)) { snprintf(err, en, "Checksum mismatch"); goto out; }

    esp_err_t e = esp_ota_end(oh);   // validates the image itself (format, chip, its own hash)
    oh = 0;
    if (e != ESP_OK) { snprintf(err, en, "Image failed verification"); goto out; }
    if (ota_svc_commit(dst, quiet) != ESP_OK) { snprintf(err, en, "Can't select new image"); goto out; }
    ok = true;                       // restarting: keep the OTA slot claimed

out:
    psa_hash_abort(&sh);
    if (oh) esp_ota_abort(oh);
    if (h) close_http(h);
    free(buf);
    if (!ok) {
        ESP_LOGW(TAG, "update to %s failed: %s", r->version, err);
        if (!quiet) ota_svc_set_state(OTA_FAILED, -1, err);
        ota_svc_release();
    }
    return ok;
}

static bool night_now(void)
{
    if (!time_valid()) return false;
    time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
    return lt.tm_hour >= NIGHT_FROM && lt.tm_hour < NIGHT_TO;
}

static void task(void *arg)
{
    int64_t next = now_us() + FIRST_CHECK_US;
    bool have = false;                             // s_rel holds a newer release
    char err[48];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        bool net = M.wifi.connected;
        uint8_t mode = g_cfg.upd_mode;
        if (mode == UPD_MODE_OFF && !s_check && !s_install) continue;

        if (net && (s_check || now_us() >= next)) {
            s_check = false;
            set_upd(UPD_CHECKING, NULL);
            rel_t r;
            if (fetch_release(&r, err, sizeof err)) {
                const char *cur = esp_app_get_description()->version;
                have = newer(r.version, cur);
                if (have) s_rel = r;
                model_lock();
                strlcpy(M.upd.latest, r.version, sizeof M.upd.latest);
                M.upd.checked = time(NULL);
                model_unlock();
                set_upd(have ? UPD_AVAILABLE : UPD_CURRENT, NULL);
                ESP_LOGI(TAG, "latest release %s, running %s%s", r.version, cur, have ? ": update available" : "");
                next = now_us() + CHECK_EVERY_US;
            } else {
                set_upd(UPD_FAILED, err);
                next = now_us() + RETRY_US;
            }
        }

        bool auto_go = mode == UPD_MODE_AUTO && have && net && night_now() && M.ota.state == OTA_IDLE &&
                       ui_idle_ms() >= IDLE_MS && strcmp(s_rel.version, s_auto_failed);
        if (s_install || auto_go) {
            bool quiet = !s_install;
            s_install = false;
            if (!have) { set_upd(UPD_FAILED, "No newer release"); continue; }
            if (!net) { set_upd(UPD_FAILED, "No network"); continue; }
            ESP_LOGI(TAG, "installing %s%s", s_rel.version, quiet ? " (automatic, night)" : "");
            if (install(&s_rel, quiet, err, sizeof err)) vTaskDelay(portMAX_DELAY);   // restarting
            set_upd(UPD_FAILED, err);
            if (quiet) strlcpy(s_auto_failed, s_rel.version, sizeof s_auto_failed);
            next = now_us() + RETRY_US;            // re-check before trying again
            have = false;
        }
    }
}

void update_svc_init(void)
{
    if (!update_svc_enabled()) { ESP_LOGI(TAG, "no update repository configured"); return; }
    snprintf(s_ua, sizeof s_ua, "VanDeck/%s (ESP32 van panel)", esp_app_get_description()->version);
    xTaskCreate(task, "update", 10240, NULL, 3, NULL);
}

// NimBLE host on the P4; the radio is the ESP32-C6 reached through esp_hosted.
#include "ble_svc.h"
#include "victron.h"
#include "litime.h"
#include "relay.h"
#include "gatt_link.h"
#include "app/settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "store/config/ble_store_config.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#endif

static const char *TAG = "ble";
static uint8_t s_own;
static int s_pause;
static bool s_synced;
static esp_timer_handle_t s_pair_t, s_housekeep_t;
static int8_t s_rssi[ROLE_COUNT];            // last advert RSSI per paired role (diagnostics)
static int64_t s_seen_us[ROLE_COUNT];
static uint32_t s_adv_n[ROLE_COUNT];

static int gap_cb(struct ble_gap_event *ev, void *arg);

// Scanning shares the C6's single radio with Wi-Fi, and readings only need refreshing every ~10 s, so outside
// pairing the scan runs in bursts: SCAN_BURST_MS listening (half duty), then SCAN_GAP_MS with the radio left to
// Wi-Fi. A burst reliably catches the Victron's instant readout and LiTime adverts (for reconnecting).
#define SCAN_BURST_MS 3000
#define SCAN_GAP_MS   7000
static esp_timer_handle_t s_scan_t;

static void scan_start(void)
{
    if (!s_synced || s_pause > 0 || ble_gap_disc_active()) return;
    model_lock(); bool pairing = M.pair.active; model_unlock();
    struct ble_gap_disc_params p = { .itvl = 0x60, .window = 0x30, .filter_duplicates = 0, .passive = 0 };
    int rc = ble_gap_disc(s_own, pairing ? BLE_HS_FOREVER : SCAN_BURST_MS, &p, gap_cb, NULL);
    if (rc) ESP_LOGW(TAG, "disc rc=%d", rc);
}
static void scan_timer_cb(void *a) { scan_start(); }

// Switch between continuous (pairing) and burst scanning right away.
static void scan_restart(void)
{
    esp_timer_stop(s_scan_t);
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    scan_start();
}
void ble_svc_scan_pause(void)  { s_pause++; if (ble_gap_disc_active()) ble_gap_disc_cancel(); }
void ble_svc_scan_resume(void) { if (s_pause > 0) s_pause--; scan_start(); }

static bool is_litime(const char *n, const struct ble_hs_adv_fields *f)
{
    for (int i = 0; i < f->num_uuids16; i++) if (ble_uuid_u16(&f->uuids16[i].u) == 0xFFE0) return true;
    return !strncmp(n, "L-12", 4) || !strncmp(n, "LT-", 3) || !strncasecmp(n, "litime", 6);
}

// Common view of a legacy (DISC) or extended (EXT_DISC) advertising report.
typedef struct { ble_addr_t addr; int8_t rssi; const uint8_t *data; uint8_t len; } adv_t;

static void pair_offer(const adv_t *d, const char *name, vd_devtype_t t)
{
    model_lock();
    if (!M.pair.active || role_type(M.pair.role) != t) { model_unlock(); return; }
    for (int r = 0; r < ROLE_COUNT; r++)
        if (r != M.pair.role && g_cfg.dev[r].paired && !memcmp(g_cfg.dev[r].addr, d->addr.val, 6)) { model_unlock(); return; }
    int i;
    for (i = 0; i < M.pair.n; i++) if (!memcmp(M.pair.c[i].addr, d->addr.val, 6)) break;
    if (i == M.pair.n) { if (M.pair.n >= PAIR_MAX_CANDS) { model_unlock(); return; } M.pair.n++; memset(&M.pair.c[i], 0, sizeof M.pair.c[i]); }
    vd_cand_t *c = &M.pair.c[i];
    memcpy(c->addr, d->addr.val, 6);
    c->addr_type = d->addr.type;
    c->rssi = d->rssi;
    if (name[0]) strlcpy(c->name, name, sizeof c->name);
    else if (!c->name[0]) snprintf(c->name, sizeof c->name, "%s %02X%02X", t == DEVT_VICTRON ? "Victron" : "Device", d->addr.val[1], d->addr.val[0]);
    model_commit();
}

static void on_adv(const adv_t *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->len) != 0) return;
    char name[24] = { 0 };
    if (f.name && f.name_len) memcpy(name, f.name, f.name_len < 23 ? f.name_len : 23);
    bool victron = f.mfg_data && f.mfg_data_len >= 2 && f.mfg_data[0] == 0xE1 && f.mfg_data[1] == 0x02;

    if (M.pair.active) {
        if (victron) pair_offer(d, name, DEVT_VICTRON);
        else if (!strcmp(name, "VanRelay")) pair_offer(d, name, DEVT_RELAY);
        else if (name[0] && is_litime(name, &f)) pair_offer(d, name, DEVT_LITIME);
    }
    for (int r = 0; r < ROLE_COUNT; r++) {
        const vd_dev_cfg_t *c = &g_cfg.dev[r];
        if (!c->paired || memcmp(c->addr, d->addr.val, 6)) continue;
        s_rssi[r] = d->rssi; s_seen_us[r] = now_us(); s_adv_n[r]++;
        if (r == ROLE_SOLAR && victron) victron_on_adv(f.mfg_data, f.mfg_data_len);
        else if (r == ROLE_BATT1 || r == ROLE_BATT2) litime_on_seen(r - ROLE_BATT1, &d->addr);
        else if (r == ROLE_RELAY) relay_on_seen(&d->addr);
    }
}

static int gap_cb(struct ble_gap_event *ev, void *arg)
{
    if (ev->type == BLE_GAP_EVENT_DISC) {
        adv_t a = { ev->disc.addr, ev->disc.rssi, ev->disc.data, ev->disc.length_data };
        on_adv(&a);
    }
#if MYNEWT_VAL(BLE_EXT_ADV)
    // With extended advertising compiled in, ble_gap_disc() runs an extended scan and every report
    // (legacy ones included) arrives here instead of as BLE_GAP_EVENT_DISC.
    else if (ev->type == BLE_GAP_EVENT_EXT_DISC) {
        if (ev->ext_disc.data_status != BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE) return 0;
        adv_t a = { ev->ext_disc.addr, ev->ext_disc.rssi, ev->ext_disc.data, ev->ext_disc.length_data };
        on_adv(&a);
    }
#endif
    else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        model_lock(); bool pairing = M.pair.active; model_unlock();
        if (pairing) scan_start();
        else { esp_timer_stop(s_scan_t); esp_timer_start_once(s_scan_t, SCAN_GAP_MS * 1000LL); }
    }
    return 0;
}

static void pair_scan_done(void *a) { model_lock(); M.pair.scanning = false; model_commit(); }

void ble_pair_begin(vd_role_t role)
{
    model_lock();
    M.pair.active = true; M.pair.scanning = true; M.pair.role = role; M.pair.n = 0;
    model_commit();
    esp_timer_stop(s_pair_t);
    esp_timer_start_once(s_pair_t, 4LL * 1000000);
    scan_restart();   // continuous while the pairing list is open
}

void ble_pair_cancel(void) { model_lock(); M.pair.active = false; model_commit(); scan_restart(); }

void ble_pair_select(int idx, const uint8_t key[16], const char *pin)
{
    model_lock();
    if (!M.pair.active || idx < 0 || idx >= M.pair.n) { model_unlock(); return; }
    vd_role_t r = M.pair.role;
    vd_cand_t c = M.pair.c[idx];
    vd_dev_cfg_t *d = &g_cfg.dev[r];
    memset(d, 0, sizeof *d);
    d->paired = true;
    memcpy(d->addr, c.addr, 6);
    d->addr_type = c.addr_type;
    strlcpy(d->name, c.name, sizeof d->name);
    if (key) memcpy(d->key, key, 16);
    if (pin) strlcpy(d->pin, pin, sizeof d->pin);
    M.dev_state[r] = DEV_PAIRING;
    M.pair.active = false;
    if (r == ROLE_SOLAR) M.solar.valid = false;
    else if (r != ROLE_RELAY) M.batt[r - ROLE_BATT1].valid = false;
    model_commit();
    settings_save_later();
    if (r == ROLE_RELAY) relay_refresh();
    scan_restart();
}

void ble_forget(vd_role_t r)
{
    if (r == ROLE_BATT1 || r == ROLE_BATT2) litime_forget(r - ROLE_BATT1);
    if (r == ROLE_RELAY) relay_forget();
    model_lock();
    memset(&g_cfg.dev[r], 0, sizeof g_cfg.dev[r]);
    M.dev_state[r] = DEV_NONE;
    model_commit();
    settings_save_later();
}

// Marks devices stale and drives periodic relay polling.
static void housekeep(void *a)
{
    gl_watchdog();
    int64_t t = now_us();
    static int n;
    model_lock();
    if (g_cfg.dev[ROLE_SOLAR].paired && M.solar.valid && t - M.solar.ts_us > 60LL * 1000000) {
        M.solar.valid = false;
        M.dev_state[ROLE_SOLAR] = DEV_LOST;
    }
    model_commit();
    if (++n % 12 == 0 && g_cfg.dev[ROLE_RELAY].paired) relay_refresh();   // every 60 s
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own);
    s_synced = true;
    scan_start();
}
static void on_reset(int reason) { s_synced = false; ESP_LOGW(TAG, "host reset %d", reason); }
static void host_task(void *p) { nimble_port_run(); nimble_port_freertos_deinit(); }

void ble_store_config_init(void);   // not in a public NimBLE header

void ble_svc_init(void)
{
#if CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    // The hosted transport is already up from wifi_svc_init(); start the C6's BT controller before the NimBLE host.
    if (esp_hosted_bt_controller_init() != ESP_OK || esp_hosted_bt_controller_enable() != ESP_OK) {
        ESP_LOGE(TAG, "C6 BT controller didn't start (does its slave firmware have BT enabled?)");
        return;
    }
#endif
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble init failed"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;   // Victron GATT passkey, later
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_store_config_init();

    model_lock();
    for (int r = 0; r < ROLE_COUNT; r++) M.dev_state[r] = g_cfg.dev[r].paired ? DEV_LOST : DEV_NONE;
    model_commit();

    litime_init();
    relay_init();
    esp_timer_create_args_t a = { .callback = pair_scan_done, .name = "pair" };
    esp_timer_create(&a, &s_pair_t);
    esp_timer_create_args_t sc = { .callback = scan_timer_cb, .name = "ble_scan" };
    esp_timer_create(&sc, &s_scan_t);
    esp_timer_create_args_t h = { .callback = housekeep, .name = "ble_hk" };
    esp_timer_create(&h, &s_housekeep_t);
    esp_timer_start_periodic(s_housekeep_t, 5LL * 1000000);
    nimble_port_freertos_init(host_task);
}

static int64_t s_stats_since;

void ble_svc_stats_reset(void)
{
    memset(s_adv_n, 0, sizeof s_adv_n);
    litime_stats_reset();
    s_stats_since = now_us();
}

void ble_svc_dump(char *b, size_t n)
{
    size_t k = 0;
    int64_t t = now_us();
    k += snprintf(b, n, "counters cover the last %llds\r\n", (t - s_stats_since) / 1000000);
    model_lock(); vd_model_t m = M; model_unlock();
    for (int r = 0; r < ROLE_COUNT && k < n; r++) {
        const vd_dev_cfg_t *c = &g_cfg.dev[r];
        if (!c->paired) { k += snprintf(b + k, n - k, "%-12s not paired\r\n", role_name(r)); continue; }
        int64_t age = -1;
        if (r == ROLE_SOLAR && m.solar.valid) age = (t - m.solar.ts_us) / 1000000;
        if ((r == ROLE_BATT1 || r == ROLE_BATT2) && m.batt[r - ROLE_BATT1].valid) age = (t - m.batt[r - ROLE_BATT1].ts_us) / 1000000;
        k += snprintf(b + k, n - k, "%-12s %02X:%02X:%02X:%02X:%02X:%02X  ", role_name(r),
                      c->addr[5], c->addr[4], c->addr[3], c->addr[2], c->addr[1], c->addr[0]);
        if (k >= n) break;
        if (s_seen_us[r]) k += snprintf(b + k, n - k, "rssi %4d dBm, seen %llds ago (%lu adverts)", s_rssi[r],
                                        (t - s_seen_us[r]) / 1000000, (unsigned long)s_adv_n[r]);
        else k += snprintf(b + k, n - k, "no adverts seen");
        if (k >= n) break;
        if (age >= 0) k += snprintf(b + k, n - k, ", reading %llds old\r\n", age);
        else k += snprintf(b + k, n - k, ", no current reading\r\n");
        if (k < n && (r == ROLE_BATT1 || r == ROLE_BATT2)) {
            litime_stats_t st = litime_stats(r - ROLE_BATT1);
            k += snprintf(b + k, n - k, "             queries %lu (acked %lu), replies %lu, unanswered %lu, notifications %lu, discarded %lu, failed connects %lu\r\n",
                          (unsigned long)st.attempts, (unsigned long)st.wr_ok, (unsigned long)st.ok, (unsigned long)st.no_reading,
                          (unsigned long)st.notif, (unsigned long)st.bad, (unsigned long)st.no_connect);
        }
    }
}

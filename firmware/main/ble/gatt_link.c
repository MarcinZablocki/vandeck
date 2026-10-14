#include "gatt_link.h"
#include "ble_svc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "gatt";
static gatt_link_t *s_links[4];

// Standing links (slow_itvl set) are held at a slow interval: peers like LiTime ask for ~12 ms, which would spend
// the C6's shared radio on idle connection events instead of Wi-Fi. The supervision timeout must exceed 2 x it.
static void slow_params(const gatt_link_t *l, struct ble_gap_upd_params *p)
{
    p->itvl_min = l->slow_itvl; p->itvl_max = l->slow_itvl + l->slow_itvl / 4;
    p->latency = 0; p->supervision_timeout = 800;   // 8 s
    p->min_ce_len = 0; p->max_ce_len = 0;
}

static void request_slow(gatt_link_t *l)
{
    struct ble_gap_upd_params p;
    slow_params(l, &p);
    int rc = ble_gap_update_params(l->conn, &p);
    if (rc && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "interval update rc=%d", rc);
}


// A pending connect holds the scan paused; whichever way it ends, release it exactly once.
static void connect_finished(gatt_link_t *l)
{
    if (!l->connecting) return;
    l->connecting = false;
    ble_svc_scan_resume();
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    gatt_link_t *l = arg;
    if (err->status == 0 && chr) {
        for (int i = 0; i < l->n_chr; i++)
            if (ble_uuid_cmp(&chr->uuid.u, &l->uuid[i].u) == 0) l->h[i] = chr->val_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "discovery done (notify handle %u)", l->notify_chr >= 0 ? l->h[l->notify_chr] : 0);
        if (l->notify_chr >= 0 && l->h[l->notify_chr]) {
            uint8_t v[2] = { 1, 0 };   // CCCD assumed directly after the value handle (true for LiTime)
            ble_gattc_write_flat(conn, l->h[l->notify_chr] + 1, v, 2, NULL, NULL);
        }
        l->up = true;
        if (l->slow_itvl) request_slow(l);   // discovery done: settle to the slow interval
        if (l->on_ready) l->on_ready(l);
    } else {
        ESP_LOGW(TAG, "discovery failed %d", err->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int gap_evt(struct ble_gap_event *ev, void *arg)
{
    gatt_link_t *l = arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect status %d", ev->connect.status);
        connect_finished(l);
        if (ev->connect.status == 0) {
            l->conn = ev->connect.conn_handle;
            memset(l->h, 0, sizeof l->h);
            ble_gattc_exchange_mtu(l->conn, NULL, NULL);
            ble_gattc_disc_all_chrs(l->conn, 1, 0xFFFF, chr_cb, l);
        } else if (l->on_down) l->on_down(l);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason 0x%x", ev->disconnect.reason);
        connect_finished(l);   // a connect that fails to establish (0x23e) ends here, without a CONNECT event
        l->up = false;
        l->conn = BLE_HS_CONN_HANDLE_NONE;
        if (l->on_down) l->on_down(l);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (l->on_notify) {
            uint8_t buf[256]; uint16_t n = OS_MBUF_PKTLEN(ev->notify_rx.om);
            if (n > sizeof buf) n = sizeof buf;
            ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, n, &n);
            int chr = -1;
            for (int i = 0; i < l->n_chr; i++) if (l->h[i] == ev->notify_rx.attr_handle) chr = i;
            l->on_notify(l, chr, buf, n);
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        // Link-layer request from the peer: answer with our interval.
        if (l->slow_itvl && l->up && ev->conn_update_req.self_params) slow_params(l, ev->conn_update_req.self_params);
        break;
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
        // L2CAP request (what LiTime sends): NimBLE applies peer_params once we accept, so rewrite them in place
        // (a writable local in ble_l2cap_sig.c despite the const). During discovery let the fast one through.
        if (l->slow_itvl && l->up) slow_params(l, (struct ble_gap_upd_params *)ev->conn_update_req.peer_params);
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ev->conn_update.status == 0) {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(ev->conn_update.conn_handle, &d) == 0) {
                ESP_LOGI(TAG, "connection interval now %u ms", d.conn_itvl * 5 / 4);
                if (l->slow_itvl && l->up && d.conn_itvl < l->slow_itvl) request_slow(l);   // pushed fast again
            }
        }
        break;
    default: break;
    }
    return 0;
}

void gl_init(gatt_link_t *l)
{
    l->conn = BLE_HS_CONN_HANDLE_NONE; l->notify_chr = -1;
    for (int i = 0; i < 4; i++) if (!s_links[i] || s_links[i] == l) { s_links[i] = l; break; }
}

void gl_watchdog(void)
{
    for (int i = 0; i < 4; i++) {
        gatt_link_t *l = s_links[i];
        int64_t limit = (l ? (l->connect_ms ? l->connect_ms : 3000) : 0) * 1000LL + 4000000;
        if (l && l->connecting && esp_timer_get_time() - l->connect_at_us > limit) {
            ESP_LOGW(TAG, "connect attempt never resolved, cancelling");
            ble_gap_conn_cancel();
            connect_finished(l);
        }
    }
}

int gl_connect(gatt_link_t *l)
{
    if (l->up || l->connecting) return 0;
    ble_svc_scan_pause();
    uint8_t own;
    ble_hs_id_infer_auto(0, &own);
    l->connecting = true;
    l->connect_at_us = esp_timer_get_time();
    // While connecting, the C6 listens for the peer's advert; NimBLE's default is continuous (100 % of the shared
    // radio). Listen half the time and give up after 3 s, so an absent or weak device can't starve Wi-Fi.
    static const struct ble_gap_conn_params cp = {
        .scan_itvl = 0x60, .scan_window = 0x30,              // 30 ms of every 60 ms
        .itvl_min = 24, .itvl_max = 40,                      // 30-50 ms while the short exchange runs
        .latency = 0, .supervision_timeout = 400,            // 4 s
        .min_ce_len = 0, .max_ce_len = 0,
    };
    int rc = ble_gap_connect(own, &l->addr, l->connect_ms ? l->connect_ms : 3000, &cp, gap_evt, l);
    if (rc) { connect_finished(l); ESP_LOGW(TAG, "connect rc=%d", rc); }
    return rc;
}

void gl_disconnect(gatt_link_t *l)
{
    if (l->connecting) { ble_gap_conn_cancel(); connect_finished(l); }
    if (l->conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(l->conn, BLE_ERR_REM_USER_CONN_TERM);
}

static int write_done(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    gatt_link_t *l = arg;
    gl_evt_cb cb = l->write_cb; l->write_cb = NULL;
    if (cb && err->status == 0) cb(l);
    return 0;
}

int gl_write(gatt_link_t *l, int chr, const void *data, uint16_t len, gl_evt_cb done)
{
    if (!l->up || !l->h[chr]) return -1;
    if (!done) return ble_gattc_write_no_rsp_flat(l->conn, l->h[chr], data, len);
    l->write_cb = done;
    return ble_gattc_write_flat(l->conn, l->h[chr], data, len, write_done, l);
}

static int read_done(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    gatt_link_t *l = arg;
    gl_data_cb cb = l->read_cb; l->read_cb = NULL;
    if (!cb || err->status != 0 || !attr) return 0;
    uint8_t buf[512]; uint16_t n = OS_MBUF_PKTLEN(attr->om);
    if (n > sizeof buf) n = sizeof buf;
    ble_hs_mbuf_to_flat(attr->om, buf, n, &n);
    cb(l, l->read_chr, buf, n);
    return 0;
}

int gl_read(gatt_link_t *l, int chr, gl_data_cb cb)
{
    if (!l->up || !l->h[chr]) return -1;
    l->read_cb = cb; l->read_chr = chr;
    return ble_gattc_read_long(l->conn, l->h[chr], 0, read_done, l);
}

uint16_t gl_mtu(gatt_link_t *l) { return l->up ? ble_att_mtu(l->conn) : 23; }

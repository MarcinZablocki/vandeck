// LiTime LiFePO4 BMS over BLE: service FFE0, notify FFE1, write FFE2, polled with a fixed query.
// Response layout (>= 104 bytes, LE, byte[2] == 0x65) per the three community implementations cross-checked in
// ~/Projects/ESP32-Truma (.claude/skills/litimeble). The first full frame from each battery is still logged as hex.
#include "litime.h"
#include "gatt_link.h"
#include "app/model.h"
#include "app/settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "litime";
static gatt_link_t L[2];
static esp_timer_handle_t s_poll;
static const uint8_t QUERY[] = { 0x00, 0x00, 0x04, 0x01, 0x13, 0x55, 0xAA, 0x17 };
#define RESP_LEN 104
static uint8_t s_rx[2][160];
static int s_rxn[2];

// Each battery keeps a standing link at the interval it asks for (~12 ms). Holding LiTime at a slower interval was
// tried (60-250 ms, to spare the radio Wi-Fi shares): the BMS keeps re-requesting 12 ms and stops answering
// queries within ~30 s, so its parameters are accepted as-is. Airtime is saved on scanning instead (ble_svc). A query goes out every POLL_S over the open link; a
// battery that rarely advertises only has to be caught once. Readings count as current for STALE_S.
#define POLL_S     10
#define STALE_S    120     // grace period: last reading stays on screen this long, then flagged stale (error icon)
#define SEEN_S     60      // only try to (re)connect to batteries heard advertising recently
// A LiTime BMS acknowledges every query but, after a few on one connection, stops answering until reconnected
// (seen on both batteries; ESP32-Truma reconnects for every read for the same reason). So one unanswered query
// means: drop the link and reconnect at once, while the BMS is still advertising fast.
#define MAX_UNANSWERED 1
// Both batteries answer exactly 4 queries per connection (the first on connect, then every POLL_S) and then go quiet.
// Refresh the link on the poll after the 4th reply, instead of waiting for a query to go unanswered.
#define REPLIES_PER_LINK 4
static uint8_t s_replies[2];
#define CONNECT_MS 6000
static int64_t s_seen[2], s_retry_at[2], s_next_poll;
static uint8_t s_fails[2];
static litime_stats_t s_st[2];
static bool s_await[2];    // query sent, reply not in yet
static bool s_was_up[2];   // the current link got as far as discovery (else a failed connect)

static int idx_of(gatt_link_t *l) { return l == &L[1]; }
static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return p[0] | p[1] << 8; }

static uint8_t s_miss[2];   // consecutive unanswered queries
static bool s_refresh[2];   // we dropped a link that went quiet: reconnect at once (the BMS advertises fast right after)
static uint32_t s_wr_ok[2], s_wr_fail[2];

static void write_done(gatt_link_t *l) { s_wr_ok[idx_of(l)]++; }

static void query(int i)
{
    if (!s_await[i] && s_replies[i] >= REPLIES_PER_LINK) {
        s_replies[i] = 0;
        s_refresh[i] = true;
        gl_disconnect(&L[i]);
        return;
    }
    if (s_await[i]) {
        s_st[i].no_reading++;   // the previous one was never answered
        if (++s_miss[i] >= MAX_UNANSWERED) {
            ESP_LOGW(TAG, "battery %d: %d queries unanswered, reconnecting", i + 1, MAX_UNANSWERED);
            s_miss[i] = 0;
            s_refresh[i] = true;
            gl_disconnect(&L[i]);
            return;
        }
    }
    s_rxn[i] = 0;
    s_await[i] = true;
    s_st[i].attempts++;
    // Write with response: the BMS acknowledges, so the counters tell lost queries from lost replies.
    if (gl_write(&L[i], 1, QUERY, sizeof QUERY, write_done)) s_wr_fail[i]++;
}

static void ready(gatt_link_t *l)
{
    int i = idx_of(l);
    ESP_LOGI(TAG, "battery %d connected", i + 1);
    s_was_up[i] = true;
    s_fails[i] = 0; s_retry_at[i] = 0;
    s_await[i] = false;
    s_replies[i] = 0;
    query(i);
}

static void down(gatt_link_t *l)
{
    int i = idx_of(l);
    if (!s_was_up[i]) s_st[i].no_connect++;
    s_was_up[i] = false;
    s_await[i] = false;
    if (s_refresh[i]) {   // our own drop of a quiet link: straight back in
        s_refresh[i] = false;
        s_retry_at[i] = 0;
        ESP_LOGI(TAG, "battery %d: link refreshed", i + 1);
        return;
    }
    // Never got as far as a reading on this link, or lost it: back off 10, 20, 40, then 60 s between tries.
    if (s_fails[i] < 3) s_fails[i]++;
    int wait = s_fails[i] >= 3 ? 60 : 10 << (s_fails[i] - 1);
    s_retry_at[i] = now_us() + wait * 1000000LL;
    ESP_LOGI(TAG, "battery %d: link down, next try in %d s", i + 1, wait);
}

static uint32_t s_notif[2], s_bad[2];

static void notify(gatt_link_t *l, int chr, const uint8_t *d, uint16_t n)
{
    int i = idx_of(l);
    s_notif[i]++;
    // A status frame starts 00 00 65: resynchronise there, so a stray or late notification can't misalign the buffer.
    if (n >= 3 && d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x65) s_rxn[i] = 0;
    // The reply can arrive split over several notifications: accumulate until a whole status frame is in.
    int room = (int)sizeof s_rx[i] - s_rxn[i];
    if (n > room) n = room;
    memcpy(s_rx[i] + s_rxn[i], d, n);
    s_rxn[i] += n;
    if (s_rxn[i] < RESP_LEN) return;
    d = s_rx[i]; n = s_rxn[i];
    s_rxn[i] = 0;
    if (d[2] != 0x65) {
        if (!s_bad[i]++) { ESP_LOGW(TAG, "battery %d: discarded frame (%u bytes):", i + 1, n); ESP_LOG_BUFFER_HEX(TAG, d, n > 32 ? 32 : n); }
        return;
    }
    static bool dumped[2];
    if (!dumped[i]) { ESP_LOGI(TAG, "battery %d frame (%u bytes):", i + 1, n); ESP_LOG_BUFFER_HEX(TAG, d, n); dumped[i] = true; }
    float volts = u32(d + 8) / 1000.0f;             // measured pack voltage (offset 12 is the cell sum)
    float amps = (int32_t)u32(d + 48) / 1000.0f;    // positive = charging
    float rem = u16(d + 62) / 100.0f;
    float full = u16(d + 64) / 100.0f;   // the BMS tracks the real capacity; LiFePO4 usually beats its rating
    float soc = u16(d + 90);
    float temp = (int16_t)u16(d + 52);
    s_await[i] = false;
    s_miss[i] = 0;
    s_replies[i]++;
    s_st[i].ok++;
    if (soc > 100 || volts < 8 || volts > 16) return;
    ESP_LOGI(TAG, "battery %d: %.2f V, %.1f A, %.0f %%", i + 1, volts, amps, soc);
    model_lock();
    M.dev_state[ROLE_BATT1 + i] = DEV_ON;
    vd_batt_t *b = &M.batt[i];
    b->valid = true; b->stale = false; b->ts_us = now_us();
    b->volts = volts; b->amps = amps; b->soc = soc; b->temp_c = temp;
    b->full_ah = full > 0 ? full : CONFIG_VD_BATT_CAPACITY_AH;
    b->remain_ah = rem > 0 ? rem : soc * b->full_ah / 100.0f;
    model_commit();
}

static void poll(void *a)
{
    int64_t t = now_us();
    // Readings age out rather than vanishing the moment a link drops.
    model_lock();
    bool ch = false;
    for (int i = 0; i < 2; i++) {
        vd_batt_t *b = &M.batt[i];
        if (b->valid && !b->stale && t - b->ts_us > STALE_S * 1000000LL) {
            b->stale = true; ch = true;
            if (g_cfg.dev[ROLE_BATT1 + i].paired) M.dev_state[ROLE_BATT1 + i] = DEV_LOST;
        }
    }
    if (ch) model_commit(); else model_unlock();

    if (t >= s_next_poll) {
        s_next_poll = t + POLL_S * 1000000LL;
        for (int i = 0; i < 2; i++) if (L[i].up) query(i);
    }

    // (Re)connect one battery at a time: only one connect can be pending in NimBLE.
    if (L[0].connecting || L[1].connecting) return;
    for (int i = 0; i < 2; i++) {
        const vd_dev_cfg_t *c = &g_cfg.dev[ROLE_BATT1 + i];
        gatt_link_t *l = &L[i];
        // conn != NONE: connected, still discovering
        if (!c->paired || l->up || l->conn != BLE_HS_CONN_HANDLE_NONE || t - s_seen[i] > SEEN_S * 1000000LL || t < s_retry_at[i]) continue;
        l->addr.type = c->addr_type;
        memcpy(l->addr.val, c->addr, 6);
        if (gl_connect(l) == 0) return;
    }
}

void litime_init(void)
{
    for (int i = 0; i < 2; i++) {
        gatt_link_t *l = &L[i];
        memset(l, 0, sizeof *l);
        gl_init(l);
        l->uuid[0].u16.u.type = BLE_UUID_TYPE_16; l->uuid[0].u16.value = 0xFFE1;
        l->uuid[1].u16.u.type = BLE_UUID_TYPE_16; l->uuid[1].u16.value = 0xFFE2;
        l->n_chr = 2; l->notify_chr = 0;
        l->connect_ms = CONNECT_MS;
        l->on_ready = ready; l->on_down = down; l->on_notify = notify;
    }
    esp_timer_create_args_t a = { .callback = poll, .name = "litime" };
    esp_timer_create(&a, &s_poll);
    esp_timer_start_periodic(s_poll, 1000000);   // 1 s tick: polls, reconnects, ages readings
}

// Adverts tell us a battery is in range; connects use its stored address.
void litime_on_seen(int i, const ble_addr_t *addr) { (void)addr; s_seen[i] = now_us(); }

void litime_forget(int i)
{
    gl_disconnect(&L[i]);
    model_lock(); M.batt[i].valid = false; M.batt[i].stale = false; model_commit();
}

litime_stats_t litime_stats(int i) { litime_stats_t st = s_st[i & 1]; st.wr_ok = s_wr_ok[i & 1]; st.wr_fail = s_wr_fail[i & 1]; st.notif = s_notif[i & 1]; st.bad = s_bad[i & 1]; return st; }
void litime_stats_reset(void) { memset(s_st, 0, sizeof s_st); memset(s_wr_ok, 0, sizeof s_wr_ok); memset(s_wr_fail, 0, sizeof s_wr_fail); memset(s_notif, 0, sizeof s_notif); memset(s_bad, 0, sizeof s_bad); }

// GPS: a u-blox 7 module (GT-U7) on the expansion header, ported from the GPS-A devkit sketch.
// NMEA GGA (position, satellites, HDOP, altitude) and RMC (UTC, speed, course) go into M.gps. The position comes
// first for the weather (net/weather.c, then Starlink, then a location set by hand); RMC time sets the clock while
// SNTP hasn't (net/time_svc.c).
// With the module's RX wired (VD_GPS_TX_GPIO) it also gets aiding for a faster first fix:
//   - AssistNow Autonomous (AOP) switched on at every start (UBX-CFG-NAVX5; the setting lives in the module's RAM):
//     from broadcast ephemeris the module works out orbits that stay usable for about 3 days;
//   - optional AssistNow Online (VD_GPS_ASSISTNOW, a legacy u-blox token in NVS): fresh AID frames over HTTPS;
//   - UBX-AID-INI at start-up: the clock (once SNTP has it) and the last known position (g_cfg.wx_*);
//   - ionosphere/UTC, almanac, ephemeris and AOP data read back from the module every 30 min while it has a fix
//     (AID-HUI/ALM/EPH/AOP), kept in GPS_DIR/aid.bin and replayed at the next start: ephemeris only under 4 h old,
//     AOP under 3 days. AOP needs HUI and the full date and time (u-blox 7 receiver description, 15.8), so it
//     goes after AID-INI and HUI, and only when the time is known.
// Track: GPS_TRACK_DIR/YYYYMMDD.csv gets a point only when the van has really moved; the oldest days go when the
// storage runs low. Without the 'storage' partition, aiding replay and the track are skipped.
#include "gps.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#if CONFIG_VD_GPS
#include "app/model.h"
#include "app/settings.h"
#include "app/solar_log.h"
#include "net/time_svc.h"
#include "driver/uart.h"
#if CONFIG_VD_GPS_ASSISTNOW
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/idf_additions.h"
#include "nvs.h"
#endif
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "gps";
#define PORT            CONFIG_VD_GPS_UART_NUM
#define RX_BUF          1024                    // ~1 s of 9600 baud
#define STALE_US        (3 * 1000000LL)         // no NMEA / GGA for this long = no module / no fix
#define AID_FILE        GPS_DIR "/aid.bin"
#define AID_TMP         GPS_DIR "/aid.tmp"
#define AID_MAGIC       0x31414456              // "VDA1", then u32 saved-at (unix), then frames: cls id len16 payload
#define AID_EVERY_US    (30 * 60 * 1000000LL)
#define EPH_MAX_AGE_S   (4 * 3600)              // GPS ephemeris is good for about 4 h
#define ALM_MAX_AGE_S   (30 * 86400)
#define AOP_MAX_AGE_S   (3 * 86400)             // AOP orbits: about 3 days at the firmware's default error limit
#define AOP_LEN         60                      // AID-AOP without the optional 3 x 48 bytes, which only save the module time
#define AOP_STATUS_EVERY_US (5 * 60 * 1000000LL)
#define AID_WAIT_SNTP_US (2 * 60 * 1000000LL)   // after the module shows up, wait this long for SNTP before aiding
#define TRACK_MIN_DIST_M  10.0
#define TRACK_HDOP_DIST_M 8.0                   // stationary jitter: need a move of max(10 m, 8 x HDOP)
#define TRACK_MIN_S       30                    // while moving, at most a point every 30 s
#define TRACK_MAX_HDOP    5.0f
#define TRACK_MIN_FREE    (256 * 1024)          // delete the oldest days below this

typedef struct { uint8_t cls, id; uint16_t len; uint8_t payload[512]; } ubx_t;

// Parser and navigation state: in PSRAM (internal RAM is the tight resource).
typedef struct {
    ubx_t f; int ust; uint16_t ui; uint8_t ca, cb;
    char line[100]; int li;
    int64_t nmea_us, gga_us, rmc_us;
    int quality, sats; float hdop; double lat, lon; float alt;
    bool rmc_ok; int64_t utc; float kmh, course;
    uint8_t wcls, wid; volatile bool wdone;      // ubx_poll: waiting for (cls, id)
    uint8_t ack_cls, ack_id; volatile int8_t ack; // ubx_cmd: waiting for ACK (1) / NAK (-1) of (cls, id)
    int cap_fd, cap_n, aop_n, aop_seen;          // AID frames being saved; AOP among them; AOP replies so far
    int64_t log_utc; double log_lat, log_lon;    // last track point
} st_t;

static st_t *S;
static bool s_fs;
static char s_sw[31], s_hw[11], s_aid[160] = "not yet";
static volatile uint32_t s_bytes, s_sentences, s_bad;
static int64_t s_aid_saved, s_fix_us;
static int s_aid_frames, s_aop_frames;
static int8_t s_aop = -1, s_aop_sats = -1;
#if CONFIG_VD_GPS_ASSISTNOW
static char s_token[64], s_online_st[48] = "not tried yet";
#endif
static TaskHandle_t s_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char *s_raw; static size_t s_raw_n, s_raw_cap;

// ---------------------------------------------------------------- UBX

static void ubx_send(uint8_t cls, uint8_t id, const uint8_t *p, uint16_t len)
{
    uint8_t h[6] = { 0xB5, 0x62, cls, id, len & 0xFF, len >> 8 }, a = 0, b = 0;
    for (int i = 2; i < 6; i++) { a += h[i]; b += a; }
    for (int i = 0; i < len; i++) { a += p[i]; b += a; }
    uint8_t ck[2] = { a, b };
    uart_write_bytes(PORT, h, 6);
    if (len) uart_write_bytes(PORT, p, len);
    uart_write_bytes(PORT, ck, 2);
}

static void on_ubx(const ubx_t *f)
{
    if (f->cls == 0x0A && f->id == 0x04 && f->len >= 40) {          // MON-VER
        memcpy(s_sw, f->payload, 30); s_sw[30] = 0;
        memcpy(s_hw, f->payload + 30, 10); s_hw[10] = 0;
    }
    if (f->cls == 0x05 && f->len >= 2 && f->payload[0] == S->ack_cls && f->payload[1] == S->ack_id)   // ACK-ACK / ACK-NAK
        S->ack = f->id == 0x01 ? 1 : -1;
    if (f->cls == 0x01 && f->id == 0x60 && f->len >= 12) {          // NAV-AOPSTATUS: availGPS, one bit per PRN
        uint32_t avail;
        memcpy(&avail, f->payload + 8, 4);
        s_aop_sats = __builtin_popcount(avail);
    }
    // AID-HUI always; AID-ALM/EPH with an 8-byte payload mean "nothing for this satellite". AID-AOP comes back
    // for each of the 32 PRNs, as the 1-byte poll when there's no data; kept without its optional part.
    if (S->cap_fd >= 0 && f->cls == 0x0B) {
        uint16_t len = f->len;
        bool keep = f->id == 0x02 || ((f->id == 0x30 || f->id == 0x31) && len > 8);
        if (f->id == 0x33) { S->aop_seen++; if (len >= AOP_LEN) { keep = true; len = AOP_LEN; } }
        uint8_t h[4] = { f->cls, f->id, len & 0xFF, len >> 8 };
        if (keep && write(S->cap_fd, h, 4) == 4 && write(S->cap_fd, f->payload, len) == len) {
            S->cap_n++;
            if (f->id == 0x33) S->aop_n++;
        }
    }
    if (f->cls == S->wcls && f->id == S->wid) S->wdone = true;
}

// ---------------------------------------------------------------- NMEA

static double coord(const char *v, const char *hemi)
{
    if (!*v) return 0;
    double raw = atof(v);
    int deg = (int)(raw / 100);
    double d = deg + (raw - deg * 100) / 60.0;
    return *hemi == 'S' || *hemi == 'W' ? -d : d;
}

static int two(const char *s) { return (s[0] - '0') * 10 + s[1] - '0'; }

static void raw_add(const char *line)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_raw) {
        size_t n = strlen(line);
        if (s_raw_n + n + 3 < s_raw_cap) {
            s_raw[s_raw_n++] = '$';
            memcpy(s_raw + s_raw_n, line, n); s_raw_n += n;
            s_raw[s_raw_n++] = '\n'; s_raw[s_raw_n] = 0;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void on_nmea(char *line)
{
    raw_add(line);
    char *star = strchr(line, '*');
    if (!star) { s_bad++; return; }
    uint8_t cs = 0;
    for (char *p = line; p < star; p++) cs ^= *p;
    if (strtol(star + 1, NULL, 16) != cs) { s_bad++; return; }
    *star = 0;
    s_sentences++;
    S->nmea_us = now_us();

    char *f[20];
    int n = 0;
    f[n++] = line;
    for (char *p = line; *p && n < 20; p++) if (*p == ',') { *p = 0; f[n++] = p + 1; }
    if (strlen(f[0]) != 5) return;
    if (!strcmp(f[0] + 2, "GGA") && n >= 10) {               // GPGGA, GNGGA
        S->quality = atoi(f[6]);
        S->sats = atoi(f[7]);
        S->hdop = *f[8] ? atof(f[8]) : 99.9f;
        if (S->quality > 0) { S->lat = coord(f[2], f[3]); S->lon = coord(f[4], f[5]); S->alt = atof(f[9]); }
        S->gga_us = S->nmea_us;
    } else if (!strcmp(f[0] + 2, "RMC") && n >= 10) {
        S->rmc_ok = *f[2] == 'A' && strlen(f[1]) >= 6 && strlen(f[9]) == 6;
        if (S->rmc_ok) {
            S->utc = (int64_t)days_from_civil(2000 + two(f[9] + 4), two(f[9] + 2), two(f[9])) * 86400 +
                     two(f[1]) * 3600 + two(f[1] + 2) * 60 + two(f[1] + 4);
            S->kmh = atof(f[7]) * 1.852f;
            S->course = atof(f[8]);
            S->rmc_us = S->nmea_us;
        }
    }
}

static inline void ck(uint8_t c) { S->ca += c; S->cb += S->ca; }

static void feed(uint8_t c)
{
    switch (S->ust) {
    case 0:
        if (c == 0xB5) { S->ust = 1; break; }
        if (c == '$') { S->li = 0; break; }
        if (S->li < 0) break;
        if (c == '\r' || c == '\n') {
            S->line[S->li] = 0;
            if (S->li > 6) on_nmea(S->line);
            S->li = -1;
        } else if (S->li < (int)sizeof S->line - 1) S->line[S->li++] = c;
        else S->li = -1;
        break;
    case 1: S->ust = c == 0x62 ? 2 : 0; break;
    case 2: S->f.cls = c; S->ca = S->cb = 0; ck(c); S->ust = 3; break;
    case 3: S->f.id = c; ck(c); S->ust = 4; break;
    case 4: S->f.len = c; ck(c); S->ust = 5; break;
    case 5:
        S->f.len |= c << 8; ck(c); S->ui = 0;
        S->ust = S->f.len > sizeof S->f.payload ? 0 : S->f.len ? 6 : 7;
        break;
    case 6: S->f.payload[S->ui++] = c; ck(c); if (S->ui >= S->f.len) S->ust = 7; break;
    case 7: S->ust = c == S->ca ? 8 : 0; break;
    case 8: if (c == S->cb) on_ubx(&S->f); S->ust = 0; break;
    }
}

// Read and parse whatever arrives within ms.
static void pump(int ms)
{
    uint8_t b[128];
    int n = uart_read_bytes(PORT, b, sizeof b, pdMS_TO_TICKS(ms));
    for (int i = 0; i < n; i++) feed(b[i]);
    if (n > 0) s_bytes += n;
}

static void pump_for(int ms)
{
    int64_t end = now_us() + ms * 1000LL;
    while (now_us() < end) pump(20);
}

// Send a poll and wait for the reply frame.
static bool ubx_poll(uint8_t cls, uint8_t id, int timeout_ms)
{
    S->wcls = cls; S->wid = id; S->wdone = false;
    ubx_send(cls, id, NULL, 0);
    int64_t end = now_us() + timeout_ms * 1000LL;
    while (!S->wdone && now_us() < end) pump(20);
    S->wcls = S->wid = 0xFF;
    return S->wdone;
}

// Send a CFG message and wait for its ACK: 1 acknowledged, -1 refused (NAK), 0 no answer.
static int ubx_cmd(uint8_t cls, uint8_t id, const uint8_t *p, uint16_t len, int timeout_ms)
{
    S->ack_cls = cls; S->ack_id = id; S->ack = 0;
    ubx_send(cls, id, p, len);
    int64_t end = now_us() + timeout_ms * 1000LL;
    while (!S->ack && now_us() < end) pump(20);
    S->ack_cls = S->ack_id = 0xFF;
    return S->ack;
}

// ---------------------------------------------------------------- model

static void publish(void)
{
    int64_t t = now_us();
    bool present = S->nmea_us && t - S->nmea_us < STALE_US;
    bool fix = present && S->quality > 0 && t - S->gga_us < STALE_US;
    model_lock();
    vd_gps_t *g = &M.gps;
    if (!present && (!g->present || g->demo)) { model_unlock(); return; }   // nothing of ours to clear
    // Commit (UI redraw) only on a state change; the position itself is read from snapshots.
    bool changed = g->present != present || g->fix != fix || g->sats != S->sats || g->demo;
    g->present = present; g->fix = fix; g->demo = false;
    g->sats = present ? S->sats : 0;
    g->hdop = S->hdop;
    if (fix) { g->lat = S->lat; g->lon = S->lon; g->alt_m = S->alt; g->fix_at_us = S->gga_us; }
    if (S->rmc_ok && t - S->rmc_us < STALE_US) { g->utc = S->utc; g->kmh = S->kmh; g->course = S->course; }
    else g->kmh = 0;
    if (changed) model_commit(); else model_unlock();
}

// ---------------------------------------------------------------- aiding

static void put32(uint8_t *b, uint32_t v) { memcpy(b, &v, 4); }

// UBX-AID-INI: rough time and position. Returns a description for 'gps'.
static void send_aid_ini(bool have_time, char *desc, size_t n)
{
    uint8_t p[48] = { 0 };
    uint32_t flags = 0;
    model_lock();
    uint8_t src = g_cfg.wx_src; float lat = g_cfg.wx_lat, lon = g_cfg.wx_lon;
    model_unlock();
    static const char *SRC[] = { "", "Starlink", "manual", "GPS" };
    if (src != WX_SRC_NONE) {
        put32(p + 0, (uint32_t)(int32_t)lroundf(lat * 1e7f));
        put32(p + 4, (uint32_t)(int32_t)lroundf(lon * 1e7f));
        put32(p + 12, (src == WX_SRC_MANUAL ? 100000 : 10000) * 100);   // accuracy, cm: 100 km / 10 km
        flags |= 0x01 | 0x20;                                           // position valid, lat/lon/alt
    }
    if (have_time) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t gps_s = (int64_t)tv.tv_sec - 315964800 + 18;            // GPS epoch 1980-01-06, 18 leap seconds
        p[18] = (gps_s / 604800) & 0xFF; p[19] = (gps_s / 604800) >> 8;  // week
        put32(p + 20, (uint32_t)((gps_s % 604800) * 1000 + tv.tv_usec / 1000));
        put32(p + 24, (uint32_t)((tv.tv_usec % 1000) * 1000));
        put32(p + 28, 500);                                             // time accuracy 0.5 s
        flags |= 0x02;
    }
    put32(p + 44, flags);
    ubx_send(0x0B, 0x01, p, sizeof p);
    snprintf(desc, n, "time %s, position %s", have_time ? "yes" : "no", src < 4 && src ? SRC[src] : "none");
}

// AssistNow Autonomous on: UBX-CFG-NAVX5 with only mask1's aop bit (14) applied, useAOP set, aopOrbMaxErr 0 = the
// firmware default (about 3 days of orbits). Kept in the module's RAM only, so it's sent at every start.
static void enable_aop(void)
{
    uint8_t p[40] = { 0 };
    p[3] = 0x40;                    // mask1, little-endian: bit 14
    p[27] = 0x01;                   // aopCfg: useAOP
    int r = ubx_cmd(0x06, 0x23, p, sizeof p, 1000);
    if (r) s_aop = r > 0;
    if (r > 0) ESP_LOGI(TAG, "AssistNow Autonomous on");
    else if (r < 0) ESP_LOGW(TAG, "module refused AssistNow Autonomous (CFG-NAVX5 NAK)");
}

static bool read_aid_header(int fd, uint32_t *saved)
{
    uint32_t h[2];
    if (read(fd, h, 8) != 8 || h[0] != AID_MAGIC) return false;
    *saved = h[1];
    return true;
}

// Replay the frames saved by an earlier session. After AssistNow Online only AOP: the rest is older than its data.
static void replay_aid(bool have_time, bool online, char *desc, size_t n)
{
    int fd = s_fs ? open(AID_FILE, O_RDONLY) : -1;
    uint32_t saved;
    if (fd < 0 || !read_aid_header(fd, &saved)) { if (fd >= 0) close(fd); snprintf(desc, n, "; nothing saved"); return; }
    int64_t age = have_time ? (int64_t)time(NULL) - saved : INT64_MAX;
    if (age < 0 || age > ALM_MAX_AGE_S) { close(fd); snprintf(desc, n, "; saved data too old%s", have_time ? "" : " (no time)"); return; }
    bool eph = age < EPH_MAX_AGE_S, aop = age < AOP_MAX_AGE_S;
    uint8_t *buf = heap_caps_malloc(512, MALLOC_CAP_SPIRAM), h[4];
    int sent = 0, sent_aop = 0, old_eph = 0, old_aop = 0;
    while (buf && read(fd, h, 4) == 4) {
        uint16_t len = h[2] | h[3] << 8;
        if (len > 512 || read(fd, buf, len) != len) break;
        if (online && h[1] != 0x33) continue;
        if (h[1] == 0x31 && !eph) { old_eph++; continue; }
        if (h[1] == 0x33 && !aop) { old_aop++; continue; }
        ubx_send(h[0], h[1], buf, len);
        pump(5);                    // keep reading NMEA while the frames go out (~110 ms each at 9600)
        sent++;
        if (h[1] == 0x33) sent_aop++;
    }
    free(buf);
    close(fd);
    char a[24];
    gps_fmt_age(age, a, sizeof a);
    int k = snprintf(desc, n, "; %d saved frames, %s old", sent, a);
    if (k < (int)n && old_eph) k += snprintf(desc + k, n - k, ", ephemeris too old");
    if (k < (int)n && sent_aop) k += snprintf(desc + k, n - k, ", AOP for %d satellites", sent_aop);
    else if (k < (int)n && old_aop) k += snprintf(desc + k, n - k, ", AOP too old");
    else if (k < (int)n) snprintf(desc + k, n - k, ", no AOP saved");
}

static void save_aid(void)
{
    if (!s_fs || !time_valid()) return;
    S->cap_fd = open(AID_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (S->cap_fd < 0) return;
    uint32_t h[2] = { AID_MAGIC, (uint32_t)time(NULL) };
    write(S->cap_fd, h, 8);
    S->cap_n = S->aop_n = S->aop_seen = 0;
    ubx_send(0x0B, 0x02, NULL, 0);  // AID-HUI
    ubx_send(0x0B, 0x30, NULL, 0);  // AID-ALM, every satellite
    ubx_send(0x0B, 0x31, NULL, 0);  // AID-EPH, every satellite
    pump_for(8000);
    // AID-AOP: a reply per PRN, up to 204 bytes each with the optional data: ~7 s at 9600 baud.
    ubx_send(0x0B, 0x33, NULL, 0);
    int64_t end = now_us() + 12 * 1000000LL;
    while (S->aop_seen < 32 && now_us() < end) pump(20);
    close(S->cap_fd);
    S->cap_fd = -1;
    if (S->cap_n > 0) {
        unlink(AID_FILE);
        rename(AID_TMP, AID_FILE);
        s_aid_saved = h[1]; s_aid_frames = S->cap_n; s_aop_frames = S->aop_n;
        ESP_LOGI(TAG, "saved %d aiding frames from the module (%d AOP)", S->cap_n, S->aop_n);
    } else {
        unlink(AID_TMP);
        ESP_LOGW(TAG, "module returned no aiding data");
    }
}

static void load_aid_info(void)
{
    int fd = s_fs ? open(AID_FILE, O_RDONLY) : -1;
    uint32_t saved;
    if (fd < 0) return;
    if (read_aid_header(fd, &saved)) {
        s_aid_saved = saved;
        uint8_t h[4];
        while (read(fd, h, 4) == 4 && lseek(fd, h[2] | h[3] << 8, SEEK_CUR) >= 0) {
            s_aid_frames++;
            if (h[1] == 0x33) s_aop_frames++;
        }
    }
    close(fd);
}

#if CONFIG_VD_GPS_ASSISTNOW
// AssistNow Online, the legacy MGA service in its u-blox 7 form (format=aid): one HTTPS GET at start-up returns
// AID-INI (time, and the position we sent), AID-HUI, AID-ALM and AID-EPH, ready to forward. "aidfull" in u-blox's
// migration table: every satellite's ephemeris, not only those in view. The token is in the URL, so the URL is
// never logged. TLS needs more stack than the GPS task has: the GET runs on a short-lived task with its stack in
// PSRAM (it doesn't touch flash).
#define AN_NS  "vd_gps"
#define AN_URL "https://online-live1.services.u-blox.com/GetOnlineData.ashx"
#define AN_MAX (16 * 1024)          // GPS eph + alm + aux is about 6 KB

typedef struct { char url[256]; uint8_t *buf; int n, status; volatile bool done; } an_job_t;

static void an_fetch(void *arg)
{
    an_job_t *j = arg;
    esp_http_client_config_t c = { .url = j->url, .timeout_ms = 10000, .crt_bundle_attach = esp_crt_bundle_attach };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    j->n = -1;
    if (h && esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        j->status = esp_http_client_get_status_code(h);
        int n = 0, r;
        while (n < AN_MAX && (r = esp_http_client_read(h, (char *)j->buf + n, AN_MAX - n)) > 0) n += r;
        j->n = n;
    }
    if (h) { esp_http_client_close(h); esp_http_client_cleanup(h); }
    j->done = true;
    vTaskSuspend(NULL);             // deleted by the GPS task
}

// Returns the number of frames forwarded; describes the result in desc ("" when there's no token).
static int online_aid(char *desc, size_t n)
{
    desc[0] = 0;
    if (!s_token[0]) return 0;
    model_lock();
    bool wifi = M.wifi.connected;
    uint8_t src = g_cfg.wx_src; float lat = g_cfg.wx_lat, lon = g_cfg.wx_lon;
    model_unlock();
    if (!wifi) { snprintf(s_online_st, sizeof s_online_st, "no Wi-Fi at start-up"); return 0; }
    an_job_t *j = heap_caps_calloc(1, sizeof *j, MALLOC_CAP_SPIRAM);
    if (j) j->buf = heap_caps_malloc(AN_MAX, MALLOC_CAP_SPIRAM);
    TaskHandle_t th = NULL;
    if (j && j->buf) {
        int k = snprintf(j->url, sizeof j->url, AN_URL "?token=%s;gnss=gps;datatype=pos,eph,alm,aux;format=aid", s_token);
        if (src != WX_SRC_NONE)   // pacc in metres, as for AID-INI: 100 km set by hand, 10 km otherwise
            snprintf(j->url + k, sizeof j->url - k, ";lat=%.3f;lon=%.3f;pacc=%d", lat, lon, src == WX_SRC_MANUAL ? 100000 : 10000);
    }
    if (!j || !j->buf || xTaskCreateWithCaps(an_fetch, "gps_an", 8192, j, 3, &th, MALLOC_CAP_SPIRAM) != pdPASS) {
        snprintf(s_online_st, sizeof s_online_st, "no memory");
        if (j) free(j->buf);
        free(j);
        return 0;
    }
    while (!j->done) pump(100);     // the HTTP timeouts bound this
    vTaskDeleteWithCaps(th);

    int frames = 0;
    if (j->n > 0 && j->status == 200 && j->buf[0] == 0xB5) {
        for (int i = 0; i + 8 <= j->n && j->buf[i] == 0xB5 && j->buf[i + 1] == 0x62; ) {
            int len = j->buf[i + 4] | j->buf[i + 5] << 8;
            if (i + 8 + len > j->n) break;
            uart_write_bytes(PORT, j->buf + i, 8 + len);   // complete frames, checksum included
            pump(5);
            frames++;
            i += 8 + len;
        }
        snprintf(s_online_st, sizeof s_online_st, "%d frames, %.1f KB at start-up", frames, j->n / 1024.0);
        ESP_LOGI(TAG, "AssistNow Online: %d frames, %d bytes", frames, j->n);
    } else if (j->n < 0) {
        snprintf(s_online_st, sizeof s_online_st, "no connection");
    } else {
        // An error comes back as text ("Unauthorized", ...): show its start, printable characters only.
        char t[24] = "";
        int m = 0;
        for (int i = 0; i < j->n && m < (int)sizeof t - 1; i++) if (j->buf[i] >= ' ' && j->buf[i] <= '~') t[m++] = j->buf[i];
        t[m] = 0;
        snprintf(s_online_st, sizeof s_online_st, "HTTP %d %s", j->status, t);
        ESP_LOGW(TAG, "AssistNow Online failed: %s", s_online_st);
    }
    free(j->buf);
    free(j);
    if (frames) snprintf(desc, n, "AssistNow Online %d frames; ", frames);
    else snprintf(desc, n, "AssistNow Online failed; ");
    return frames;
}

bool gps_set_token(const char *token)
{
    size_t n = strlen(token);
    if (n && (n < 16 || n >= sizeof s_token)) return false;
    for (const char *p = token; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-' || *p == '_')) return false;
    nvs_handle_t h;
    if (nvs_open(AN_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    if (n) nvs_set_str(h, "token", token); else nvs_erase_key(h, "token");
    nvs_commit(h);
    nvs_close(h);
    strlcpy(s_token, token, sizeof s_token);
    snprintf(s_online_st, sizeof s_online_st, n ? "token set, used at the next start" : "no token");
    return true;
}
#endif

// ---------------------------------------------------------------- track

static double dist_m(double la1, double lo1, double la2, double lo2)
{
    const double r = M_PI / 180;
    double a = sin((la2 - la1) * r / 2), b = sin((lo2 - lo1) * r / 2);
    double h = a * a + cos(la1 * r) * cos(la2 * r) * b * b;
    return 2 * 6371000.0 * asin(sqrt(h));
}

static void day_path(int64_t utc, char *p, size_t n)
{
    int y; unsigned m, d;
    civil_from_days((int32_t)(utc / 86400), &y, &m, &d);
    snprintf(p, n, GPS_TRACK_DIR "/%04d%02u%02u.csv", y, m, d);
}

// Delete the oldest days while free space is low.
static void track_space(void)
{
    size_t total = 0, used = 0;
    while (esp_littlefs_info("storage", &total, &used) == ESP_OK && total - used < TRACK_MIN_FREE) {
        char oldest[16] = "";
        DIR *dir = opendir(GPS_TRACK_DIR);
        struct dirent *e;
        while (dir && (e = readdir(dir)))
            if (strlen(e->d_name) < sizeof oldest && (!oldest[0] || strcmp(e->d_name, oldest) < 0)) strcpy(oldest, e->d_name);
        if (dir) closedir(dir);
        if (!oldest[0]) return;
        char p[48];
        snprintf(p, sizeof p, GPS_TRACK_DIR "/%s", oldest);
        if (unlink(p) != 0) return;
        ESP_LOGW(TAG, "storage low, deleted track %s", oldest);
    }
}

// After a restart, carry on from the day file's last point.
static void track_resume(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    char t[96];
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, sz > (off_t)sizeof t - 1 ? sz - (sizeof t - 1) : 0, SEEK_SET);
    int n = read(fd, t, sizeof t - 1);
    close(fd);
    if (n <= 0) return;
    t[n] = 0;
    while (n > 0 && (t[n - 1] == '\n' || t[n - 1] == '\r')) t[--n] = 0;
    char *last = strrchr(t, '\n');
    long long u; double la, lo;
    if (sscanf(last ? last + 1 : t, "%lld,%lf,%lf", &u, &la, &lo) == 3) { S->log_utc = u; S->log_lat = la; S->log_lon = lo; }
}

static void track_point(void)
{
    static int64_t seen;            // once per RMC, not on every pass of the task loop
    int64_t t = now_us();
    if (S->rmc_us == seen) return;
    seen = S->rmc_us;
    if (!s_fs || S->quality == 0 || !S->rmc_ok || t - S->gga_us > STALE_US || t - S->rmc_us > STALE_US ||
        S->hdop > TRACK_MAX_HDOP || S->utc < 1735689600) return;
    int64_t e = S->utc;
    char path[48];
    day_path(e, path, sizeof path);
    if (!S->log_utc) track_resume(path);
    bool new_day = !S->log_utc || e / 86400 != S->log_utc / 86400;
    if (!new_day) {
        if (e - S->log_utc < TRACK_MIN_S) return;
        double min_move = fmax(TRACK_MIN_DIST_M, TRACK_HDOP_DIST_M * S->hdop);
        if (dist_m(S->log_lat, S->log_lon, S->lat, S->lon) < min_move) return;
    }
    track_space();
    struct stat st;
    bool fresh = stat(path, &st) != 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char l[96];
    int n = fresh ? snprintf(l, sizeof l, "time,lat,lon,alt,sats,hdop,kmh\n") : 0;
    n += snprintf(l + n, sizeof l - n, "%lld,%.6f,%.6f,%.1f,%d,%.1f,%.1f\n", (long long)e, S->lat, S->lon, S->alt,
                  S->sats, S->hdop, S->kmh);
    write(fd, l, n);
    close(fd);
    S->log_utc = e; S->log_lat = S->lat; S->log_lon = S->lon;
}

// ---------------------------------------------------------------- task

static void task(void *arg)
{
    const bool tx = CONFIG_VD_GPS_TX_GPIO >= 0;
    struct stat st;
    s_fs = stat("/root", &st) == 0;
    if (s_fs) { mkdir(GPS_DIR, 0755); mkdir(GPS_TRACK_DIR, 0755); load_aid_info(); }
    if (!tx) snprintf(s_aid, sizeof s_aid, "none (listen only)");

    bool aided = !tx, had_fix = false;
    int64_t seen_at = 0, ver_next = 0, aid_next = 0, clock_next = 0, pub_next = 0, aop_next = 0, aops_next = 0;
    int ver_tries = 0, aop_tries = 0;
    for (;;) {
        pump(100);
        int64_t t = now_us();
        bool present = S->nmea_us && t - S->nmea_us < STALE_US;
        bool fix = present && S->quality > 0 && t - S->gga_us < STALE_US;
        if (t >= pub_next) { publish(); pub_next = t + 1000000; }
        if (!present) { seen_at = 0; continue; }
        if (!seen_at) seen_at = t;

        if (tx && !s_sw[0] && ver_tries < 5 && t >= ver_next) {       // module version: proves TX is wired too
            ver_tries++;
            ver_next = t + 10 * 1000000LL;
            if (ubx_poll(0x0A, 0x04, 1500)) ESP_LOGI(TAG, "u-blox SW %s, HW %s", s_sw, s_hw);
            else if (ver_tries == 5) ESP_LOGW(TAG, "no answer to UBX-MON-VER: is the module's RX wired to GPIO %d?", CONFIG_VD_GPS_TX_GPIO);
        }
        // AssistNow Autonomous: before the aiding below, once the module has answered.
        if (tx && s_sw[0] && s_aop < 0 && aop_tries < 3 && t >= aop_next) {
            aop_tries++;
            aop_next = t + 10 * 1000000LL;
            enable_aop();
        }
        if (tx && s_sw[0] && t >= aops_next) {                      // how many satellites it has AOP orbits for
            aops_next = t + AOP_STATUS_EVERY_US;
            ubx_poll(0x01, 0x60, 1500);
        }

        // Aiding once: when SNTP has the time, or after waiting a while for it. Pointless once there's a fix.
        if (!aided) {
            model_lock(); bool sntp = M.time_synced; model_unlock();
            if (fix) { aided = true; snprintf(s_aid, sizeof s_aid, "none (already had a fix)"); }
            else if (sntp || t - seen_at > AID_WAIT_SNTP_US) {
                aided = true;
                int online = 0;
                s_aid[0] = 0;
#if CONFIG_VD_GPS_ASSISTNOW
                if (sntp) online = online_aid(s_aid, sizeof s_aid);   // SNTP synced: there's internet
#endif
                size_t k = strlen(s_aid);
                send_aid_ini(sntp, s_aid + k, sizeof s_aid - k);       // after Online's: our clock is closer
                k = strlen(s_aid);
                replay_aid(sntp, online > 0, s_aid + k, sizeof s_aid - k);
                ESP_LOGI(TAG, "aiding: %s", s_aid);
                aops_next = now_us() + 30 * 1000000LL;
            }
        }

        if (fix) s_fix_us = t;
        if (fix && !had_fix) {
            had_fix = true;
            ESP_LOGI(TAG, "fix: %.5f, %.5f, %d satellites, HDOP %.1f", S->lat, S->lon, S->sats, S->hdop);
            if (!aid_next) aid_next = t + 60 * 1000000LL;   // ephemeris is fuller a minute after the first fix
        } else if (!fix && had_fix && t - S->gga_us > 60 * 1000000LL) {
            had_fix = false;
            ESP_LOGI(TAG, "fix lost");
        }
        if (fix && S->rmc_ok && t - S->rmc_us < STALE_US && t >= clock_next) {
            clock_next = t + 10 * 60 * 1000000LL;
            if (time_svc_set_from_gps(S->utc)) ESP_LOGI(TAG, "clock set from GPS");
        }
        if (fix) track_point();
        if (tx && fix && aid_next && t >= aid_next) { aid_next = t + AID_EVERY_US; save_aid(); aops_next = 0; }
    }
}

void gps_init(void)
{
    S = heap_caps_calloc(1, sizeof *S, MALLOC_CAP_SPIRAM);
    if (!S) { ESP_LOGE(TAG, "no memory"); return; }
    S->li = -1; S->cap_fd = -1; S->hdop = 99.9f; S->wcls = S->wid = S->ack_cls = S->ack_id = 0xFF;
#if CONFIG_VD_GPS_ASSISTNOW
    nvs_handle_t h;
    size_t n = sizeof s_token;
    if (nvs_open(AN_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "token", s_token, &n) != ESP_OK) s_token[0] = 0;
        nvs_close(h);
    }
    if (!s_token[0]) snprintf(s_online_st, sizeof s_online_st, "no token");
#endif
    uart_config_t u = { .baud_rate = CONFIG_VD_GPS_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    if (uart_driver_install(PORT, RX_BUF, 0, 0, NULL, 0) != ESP_OK || uart_param_config(PORT, &u) != ESP_OK ||
        uart_set_pin(PORT, CONFIG_VD_GPS_TX_GPIO, CONFIG_VD_GPS_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART%d setup failed", PORT);
        return;
    }
    gpio_pullup_en(CONFIG_VD_GPS_RX_GPIO);   // no module: idle high instead of picking up noise
    // 5 KB: LittleFS writes and float formatting run on it (see 'gps' for the high-water mark).
    xTaskCreate(task, "gps", 5120, NULL, 3, &s_task);
    ESP_LOGI(TAG, "UART%d RX GPIO %d, TX GPIO %d, %d baud", PORT, CONFIG_VD_GPS_RX_GPIO, CONFIG_VD_GPS_TX_GPIO, CONFIG_VD_GPS_BAUD);
}

void gps_info(gps_info_t *o)
{
    memset(o, 0, sizeof *o);
    o->enabled = true;
    o->tx = CONFIG_VD_GPS_TX_GPIO >= 0;
    o->fs = s_fs;
    o->rx_gpio = CONFIG_VD_GPS_RX_GPIO; o->tx_gpio = CONFIG_VD_GPS_TX_GPIO; o->baud = CONFIG_VD_GPS_BAUD;
    strlcpy(o->sw, s_sw, sizeof o->sw);
    strlcpy(o->hw, s_hw, sizeof o->hw);
    o->bytes = s_bytes; o->sentences = s_sentences; o->bad = s_bad;
    strlcpy(o->aid, s_aid, sizeof o->aid);
    o->aid_saved = s_aid_saved; o->aid_frames = s_aid_frames; o->aop_frames = s_aop_frames;
    o->aop = s_aop; o->aop_sats = s_aop_sats;
    if (time_valid()) {
        int64_t now = time(NULL), up_s = now_us() / 1000000;
        o->boot_unix = now - up_s;
        if (s_fix_us) o->fix_unix = now - (now_us() - s_fix_us) / 1000000;
    }
#if CONFIG_VD_GPS_ASSISTNOW
    o->online = true;
    o->online_token = s_token[0] != 0;
    strlcpy(o->online_st, s_online_st, sizeof o->online_st);
#endif
    o->stack_free = s_task ? uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t) : 0;
}

size_t gps_raw(char *buf, size_t cap, int ms)
{
    if (!S || cap < 2) return 0;
    buf[0] = 0;
    taskENTER_CRITICAL(&s_mux);
    bool busy = s_raw != NULL;
    if (!busy) { s_raw = buf; s_raw_n = 0; s_raw_cap = cap; }
    taskEXIT_CRITICAL(&s_mux);
    if (busy) return 0;
    vTaskDelay(pdMS_TO_TICKS(ms));
    taskENTER_CRITICAL(&s_mux);
    size_t n = s_raw_n;
    s_raw = NULL;
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

#else

void gps_init(void) {}
void gps_info(gps_info_t *o) { memset(o, 0, sizeof *o); o->aop = o->aop_sats = -1; }
size_t gps_raw(char *buf, size_t cap, int ms) { if (cap) buf[0] = 0; return 0; }

#endif

void gps_fmt_age(int64_t s, char *b, size_t n)
{
    if (s < 0) s = 0;
    if (s < 60) snprintf(b, n, "%d s", (int)s);
    else if (s < 3600) snprintf(b, n, "%d min", (int)(s / 60));
    else if (s < 86400) snprintf(b, n, "%d h %d min", (int)(s / 3600), (int)(s % 3600 / 60));
    else snprintf(b, n, "%d d %d h", (int)(s / 86400), (int)(s % 86400 / 3600));
}

// Saves happen only with a fix (every 30 min), so an old save means either the panel was off (saved before this
// start-up) or the module has had no fix since.
void gps_aid_note(const gps_info_t *i, char *b, size_t n)
{
    b[0] = 0;
    if (!i->aid_saved) return;
    int64_t now = time(NULL);
    char a[24], u[24];
    gps_fmt_age(now - i->aid_saved, a, sizeof a);
    if (i->boot_unix && i->aid_saved < i->boot_unix) {
        gps_fmt_age(now - i->boot_unix, u, sizeof u);
        snprintf(b, n, "saved %s ago, before this start-up %s ago%s", a, u, i->fix_unix ? "" : "; no fix since");
    } else if (now - i->aid_saved > 35 * 60)
        snprintf(b, n, "saved %s ago; %s", a, i->fix_unix > i->aid_saved + 120 ? "fixes since, but nothing new saved" : "no fix since");
    else snprintf(b, n, "saved %s ago", a);
}

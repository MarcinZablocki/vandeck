// Shell commands for reading what the panel knows (esp_console: USB serial and the Wi-Fi console).
#include "cli.h"
#include "model.h"
#include "settings.h"
#include "ble/ble_svc.h"
#include "net/weather.h"
#include "net/matter_svc.h"
#include "net/relay_svc.h"
#include "net/wifi_svc.h"
#include "io/truma_lin.h"
#include "io/gps.h"
#include "net/update_svc.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "ui/ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

static vd_model_t *snap(void)
{
    vd_model_t *m = heap_caps_malloc(sizeof *m, MALLOC_CAP_SPIRAM);
    if (m) model_snapshot(m);
    return m;
}

static long age_s(int64_t ts) { return (long)((now_us() - ts) / 1000000); }

static const char *stage_txt(uint8_t mode)
{
    switch (mode) {
    case 0: return "off"; case 2: return "fault"; case 3: return "bulk"; case 4: return "absorption";
    case 5: return "float"; case 6: return "storage"; case 7: return "equalize"; case 245: return "starting up";
    default: return "other";
    }
}

const char *cli_solar_stage(uint8_t mode) { return stage_txt(mode); }

static void print_batt(const vd_model_t *m)
{
    float ah = 0, full = 0; int nv = 0;
    for (int i = 0; i < 2; i++) {
        const vd_batt_t *b = &m->batt[i];
        if (!g_cfg.dev[ROLE_BATT1 + i].paired) { printf("battery %d: not paired\n", i + 1); continue; }
        if (!b->valid) { printf("battery %d: no current reading\n", i + 1); continue; }
        printf("battery %d: %5.2f V  %+6.1f A  %3.0f %%  %5.1f / %5.1f Ah  %2.0f C  (%lds ago)\n", i + 1, b->volts,
               b->amps, b->soc, b->remain_ah, b->full_ah, b->temp_c, age_s(b->ts_us));
        nv++; ah += b->remain_ah; full += b->full_ah;
    }
    if (nv && full > 0) printf("bank:      %3.0f %%  %.0f / %.0f Ah\n", fminf(100, ah / full * 100), ah, full);
}

static void print_solar(const vd_model_t *m)
{
    const vd_solar_t *s = &m->solar;
    if (!g_cfg.dev[ROLE_SOLAR].paired && !m->demo) { printf("solar: not paired\n"); return; }
    if (!s->valid) { printf("solar: no current reading\n"); return; }
    printf("solar: %.0f W PV, %s, battery %.2f V %+.1f A (%.0f W), yield today %.2f kWh", s->pv_w, stage_txt(s->mode),
           s->batt_v, s->batt_a, s->batt_v * s->batt_a, s->yield_wh / 1000.0f);
    if (!isnan(s->load_a)) printf(", load %.1f A", s->load_a);
    if (s->err) printf(", ERROR %u", s->err);
    printf("  (%lds ago)\n", age_s(s->ts_us));
}

static void print_heater(const vd_model_t *m)
{
    static const char *WATER[] = { "off", "eco 40", "hot 60" }, *FAN[] = { "eco", "high", "vent" };
    const vd_heater_t *h = &m->heater;
    printf("heater: LIN %s%s, %s, setpoint %.1f C", h->link ? "up" : "down",
           h->link && !h->registered ? " (not registered: run PR SET on the CP Plus)" : "",
           g_cfg.heater_on ? (h->heating ? "on, heating" : "on") : "off", g_cfg.setpoint_c);
    if (!isnan(h->room_c) && h->room_c != 0) printf(", room %.1f C", h->room_c);
    printf(", water %s, fan %s", WATER[g_cfg.water % 3], FAN[g_cfg.fan % 3]);
    if (h->err) {
        const char *fix, *t = truma_err_text(h->err, &fix);
        printf(", %s %c %u: %s. %s", h->warn ? "warning" : "fault", h->warn ? 'W' : 'E', h->err, t, fix);
    }
    printf("\n");
}

static void print_tanks(const vd_model_t *m)
{
    static const char *NAME[] = { "LPG", "grey water" }, *LVL[] = { "empty", "1/3", "2/3", "full" };
    for (int i = 0; i < 2; i++) {
        const vd_tank_t *t = &m->tank[i];
        if (t->reading) printf("%s: reading...\n", NAME[i]);
        else if (t->level < 0) printf("%s: unknown (tap the tank on the panel to read)\n", NAME[i]);
        else printf("%s: %s (%lds ago)\n", NAME[i], LVL[t->level & 3], age_s(t->read_at_us));
    }
}

static void print_starlink(const vd_model_t *m)
{
    const vd_starlink_t *s = &m->sl;
    printf("starlink: power %s, dish %s%s", s->power ? "on" : "off", s->reachable ? "reachable" : "not reachable",
           s->heating ? ", heating" : "");
    if (s->reachable) {
        static const char *N[] = { "down", "up", "latency", "power", "ping ok", "obstructed" };
        static const char *U[] = { "Mbps", "Mbps", "ms", "W", "%", "%" };
        for (int k = 0; k < SL_NHIST; k++) if (!isnan(s->v[k])) printf(", %s %.1f %s", N[k], s->v[k], U[k]);
    }
    printf("\n");
}

static void print_wifi(const vd_model_t *m)
{
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_t *nf = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nf) esp_netif_get_ip_info(nf, &ip);
    wifi_ap_record_t ap;
    bool up = m->wifi.connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (!g_cfg.wifi_on) printf("wifi: off\n");
    else if (!up) printf("wifi: not connected (saved network: %s)\n", g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : "none");
    else printf("wifi: %s, rssi %d dBm, ip " IPSTR "\n", (char *)ap.ssid, ap.rssi, IP2STR(&ip.ip));
}

static void print_matter(const vd_model_t *m)
{
    vd_mt_state_t st = matter_svc_state();
    char code[20];
    printf("matter: %s", matter_svc_state_text(st));
    if (st == MT_PAIRED) printf(", %u home%s", matter_svc_fabrics(), matter_svc_fabrics() == 1 ? "" : "s");
    if (matter_svc_skipped()) printf(" (skipped: the last start brought the panel down; 'matter retry')");
    if (matter_svc_manual_code(code, sizeof code)) printf(", setup code %s", code);
    printf("\n");
}

static void print_relay(const vd_model_t *m)
{
    printf("relay: %s", relay_svc_state_text(relay_svc_state()));
    if (relay_svc_url()[0]) printf(", %s", relay_svc_url());
    printf("\n");
}

static void print_weather(const vd_model_t *m)
{
    static const char *SRC[] = { "none", "Starlink", "manual", "GPS" };
    static const char *ST[] = { "no location", "no network", "fetching", "ok", "fetch failed" };
    const vd_weather_t *w = &m->wx;
    printf("weather: %s, location %s", ST[w->state], SRC[g_cfg.wx_src & 3]);
    if (g_cfg.wx_src) printf(" %.4f, %.4f", g_cfg.wx_lat, g_cfg.wx_lon);
    const char *place, *country;
    if (weather_place(&place, &country)) printf(" near %s%s%s", place[0] ? place : "(no town)", country[0] ? ", " : "", country);
    if (m->sl.reachable) printf(" (dish %s)", m->sl.loc_ok ? "reports its position" : m->sl.loc_denied ? "won't share its position" : "position pending");
    printf("\n");
    if (!w->valid) return;
    printf("  now %.1f C (feels %.1f), %s, wind %.0f km/h, humidity %u %%, fetched %llds ago\n", w->temp, w->feels,
           weather_text(w->code), w->wind_kmh, w->humidity, (long long)(time(NULL) - w->fetched));
    for (int i = 0; i < w->n_days; i++) {
        const vd_wx_day_t *d = &w->daily[i];
        printf("  day %d: %5.1f / %5.1f C, %-18s rain %3u %%, sun %.2f kWh/m2 -> ~%.2f kWh\n", i, d->tmin, d->tmax,
               weather_text(d->code), d->pop, d->rad_kwh_m2, weather_expected_kwh(d->rad_kwh_m2));
    }
}

static const char *compass(float deg)
{
    static const char *P[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    return P[(int)lroundf(deg / 45) & 7];
}

static void print_gps(const vd_model_t *m)
{
    gps_info_t i;
    gps_info(&i);
    const vd_gps_t *g = &m->gps;
    if (!i.enabled && !g->demo) { printf("gps: off (VanDeck > GPS in menuconfig)\n"); return; }
    if (g->fix) {
        printf("gps: %.6f, %.6f  alt %.0f m  %u satellites  HDOP %.1f", g->lat, g->lon, g->alt_m, g->sats, g->hdop);
        if (g->kmh >= 2) printf("  %.0f km/h %s", g->kmh, compass(g->course));
        if (g->demo) printf("  (demo)");
        printf("\n");
    } else if (g->present) printf("gps: no fix yet, %u satellites in use\n", g->sats);
    else printf("gps: no data from the module\n");
    if (!g->fix && g->fix_at_us) printf("  last fix %.6f, %.6f, %lds ago\n", g->lat, g->lon, age_s(g->fix_at_us));
    if (g->utc) {
        time_t u = (time_t)g->utc; struct tm tm; char b[24];
        gmtime_r(&u, &tm); strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
        printf("  GPS time %s UTC\n", b);
    }
    if (!i.enabled) return;
    printf("  UART RX GPIO %d, TX GPIO %d, %d baud: %lu bytes, %lu sentences, %lu bad checksums\n", i.rx_gpio, i.tx_gpio,
           i.baud, (unsigned long)i.bytes, (unsigned long)i.sentences, (unsigned long)i.bad);
    if (i.sw[0]) printf("  module: u-blox SW %s, HW %s\n", i.sw, i.hw);
    else if (i.tx) printf("  module: no answer to UBX-MON-VER yet%s\n", i.sentences ? " (check the wire to its RX)" : "");
    printf("  aiding at start-up: %s\n", i.aid);
    char b[120];
    if (g->fix) printf("  last fix: now\n");
    else if (i.fix_unix) { gps_fmt_age(time(NULL) - i.fix_unix, b, sizeof b); printf("  last fix: %s ago\n", b); }
    else if (i.boot_unix) { gps_fmt_age(time(NULL) - i.boot_unix, b, sizeof b); printf("  last fix: none since start-up %s ago\n", b); }
    else printf("  last fix: none since start-up\n");
    gps_aid_note(&i, b, sizeof b);
    if (i.aid_saved) printf("  saved aiding: %d frames (%d AOP), %s\n", i.aid_frames, i.aop_frames, b);
    else if (i.tx) printf("  saved aiding: none yet (saved every 30 min with a fix)\n");
    if (i.tx) {
        printf("  AssistNow Autonomous: %s", i.aop > 0 ? "on" : i.aop == 0 ? "refused by the module" : "not set yet");
        if (i.aop_sats >= 0) printf(", orbits for %d satellites", i.aop_sats);
        printf("\n");
    }
    if (i.online) printf("  AssistNow Online: %s\n", i.online_token ? i.online_st : "no token ('gps assistnow <token>')");
    if (!i.fs) printf("  no 'storage' partition: no track log or saved ephemeris\n");
    else {
        time_t now = time(NULL); struct tm tm; char p[48]; struct stat st;
        gmtime_r(&now, &tm);
        snprintf(p, sizeof p, GPS_TRACK_DIR "/%04d%02d%02d.csv", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        if (stat(p, &st) == 0) printf("  track today: %s, %ld bytes\n", p, (long)st.st_size);
        else printf("  track today: none yet (" GPS_TRACK_DIR "/YYYYMMDD.csv, UTC days)\n");
    }
    printf("  task stack free: %lu bytes\n", (unsigned long)i.stack_free);
}

static void print_update(const vd_model_t *m)
{
    static const char *MODE[] = { "off", "notify", "automatic" };
    static const char *ST[] = { "not checked yet", "checking", "up to date", "update available", "installing", "failed" };
    const vd_update_t *u = &m->upd;
    if (!update_svc_enabled()) { printf("updates: no repository configured (VanDeck > GitHub repository for updates)\n"); return; }
    printf("updates (%s, %s): running %s, %s", CONFIG_VD_UPDATE_REPO, MODE[g_cfg.upd_mode % 3],
           esp_app_get_description()->version, ST[u->state]);
    if (u->latest[0]) printf(", latest release %s", u->latest);
    if (u->checked) printf(", checked %llds ago", (long long)(time(NULL) - u->checked));
    if (u->state == UPD_FAILED && u->msg[0]) printf(": %s", u->msg);
    printf("\n");
}

typedef void (*printer_t)(const vd_model_t *);

static int run(printer_t p)
{
    vd_model_t *m = snap();
    if (!m) { printf("out of memory\n"); return 1; }
    if (m->demo) printf("(demo values shown for devices that aren't paired)\n");
    p(m);
    free(m);
    return 0;
}

static void print_all(const vd_model_t *m)
{
    print_batt(m); print_solar(m); print_heater(m); print_tanks(m); print_starlink(m); print_gps(m); print_weather(m); print_wifi(m); print_matter(m); print_relay(m);
}

static int c_status(int c, char **v) { return run(print_all); }
static int c_batt(int c, char **v) { return run(print_batt); }
static int c_solar(int c, char **v) { return run(print_solar); }
static int c_heater(int c, char **v) { return run(print_heater); }
static int c_tanks(int c, char **v) { return run(print_tanks); }
static int c_starlink(int c, char **v) { return run(print_starlink); }
static int c_gps(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "raw")) {
        int s = argc > 2 ? atoi(argv[2]) : 3;
        s = s < 1 ? 1 : s > 10 ? 10 : s;
        char *b = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        if (!b) return 1;
        size_t n = gps_raw(b, 4096, s * 1000);
        if (n) fwrite(b, 1, n, stdout); else printf("nothing received in %d s\n", s);
        free(b);
        return 0;
    }
#if CONFIG_VD_GPS_ASSISTNOW
    if (argc == 3 && !strcmp(argv[1], "assistnow")) {
        if (!gps_set_token(strcmp(argv[2], "off") ? argv[2] : "")) { printf("the token is 16..63 letters, digits, '-' or '_'\n"); return 1; }
        printf(strcmp(argv[2], "off") ? "token saved, used at the next start\n" : "token cleared\n");
        return 0;
    }
#endif
#if CONFIG_VD_GPS_ASSISTNOW
    if (argc > 1) { printf("usage: gps [raw [seconds] | assistnow <token>|off]\n"); return 1; }
#else
    if (argc > 1) { printf("usage: gps [raw [seconds]]\n"); return 1; }
#endif
    return run(print_gps);
}
static int c_wifi(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "forget")) {
        if (!wifi_svc_forget(argv[2])) { printf("%s isn't remembered\n", argv[2]); return 1; }
        printf("forgot %s\n", argv[2]);
        return 0;
    }
    if (argc > 1) { printf("usage: net [forget <ssid>]\n"); return 1; }
    int r = run(print_wifi);
    char ssids[WIFI_SAVED_MAX][33];
    int n = wifi_svc_saved(ssids, WIFI_SAVED_MAX);
    printf("remembered (newest first):");
    for (int i = 0; i < n; i++) printf("%s %s", i ? "," : "", ssids[i]);
    printf("%s\n", n ? "" : " none");
    return r;
}
static int c_update(int argc, char **argv)
{
    static const char *MODE[] = { "off", "notify", "auto" };
    if (argc > 1) {
        int i = 0;
        while (i < 3 && strcmp(argv[1], MODE[i])) i++;
        if (i < 3) { model_lock(); g_cfg.upd_mode = i; model_commit(); settings_save_later(); }
        else if (!strcmp(argv[1], "check")) { update_svc_check(); printf("checking (see 'update' in a few seconds)\n"); }
        else if (!strcmp(argv[1], "install")) { update_svc_install(); printf("installing if a newer release was found; the panel restarts when done\n"); }
        else { printf("usage: update [check|install|off|notify|auto]\n"); return 1; }
    }
    return run(print_update);
}
static int c_weather(int argc, char **argv)
{
    if (argc > 1) {
        if (!strcmp(argv[1], "refresh")) { weather_refresh(); printf("fetching\n"); }
        else if (!strcmp(argv[1], "loc") && argc > 3) {
            if (!weather_set_location(strtof(argv[2], NULL), strtof(argv[3], NULL))) { printf("latitude -90..90, longitude -180..180\n"); return 1; }
            printf("location set (a GPS fix or the Starlink dish's position replaces it)\n");
        } else { printf("usage: weather [refresh|loc <lat> <lon>]\n"); return 1; }
    }
    return run(print_weather);
}

static int c_matter(int argc, char **argv)
{
    if (argc > 1) {
        if (!strcmp(argv[1], "unpair")) { printf("forgetting every home and restarting\n"); matter_svc_reset(); return 0; }
        if (!strcmp(argv[1], "retry")) { printf("restarting to start Matter again\n"); matter_svc_retry(); return 0; }
        if (!strcmp(argv[1], "qr")) {
            char qr[64];
            if (!matter_svc_qr(qr, sizeof qr)) { printf("not started\n"); return 1; }
            printf("%s\n", qr);
            return 0;
        }
        printf("usage: matter [qr|unpair|retry]\n");
        return 1;
    }
    return run(print_matter);
}

// The key is the Worker's PANEL_KEY secret (cloud/README.md). It isn't printed back.
static int c_relay(int argc, char **argv)
{
    if (argc > 1) {
        if (!strcmp(argv[1], "on")) relay_svc_set_enabled(true);
        else if (!strcmp(argv[1], "off")) relay_svc_set_enabled(false);
        else if (!strcmp(argv[1], "url") && argc > 2) {
            if (!relay_svc_set_url(argv[2])) { printf("needs a wss:// URL\n"); return 1; }
        } else if (!strcmp(argv[1], "key") && argc > 2) {
            if (!relay_svc_set_key(argv[2])) { printf("the key is 32..127 printable characters, no spaces or quotes\n"); return 1; }
            printf("key saved\n");
        } else { printf("usage: relay [on|off|url <wss://...>|key <secret>]\n"); return 1; }
    }
    return run(print_relay);
}

static int c_ble(int c, char **v)
{
    if (c > 1 && !strcmp(v[1], "reset")) { ble_svc_stats_reset(); printf("counters reset\n"); return 0; }
    char *b = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!b) return 1;
    ble_svc_dump(b, 1024);
    for (char *p = b; *p; p++) if (*p != '\r') putchar(*p);
    free(b);
    return 0;
}

static int c_screen(int argc, char **argv)
{
    static const char *NAME[] = { "30s", "1m", "5m", "never" };
    if (argc > 1) {
        if (!strcmp(argv[1], "wake")) {
            lvgl_port_lock(0); ui_screen_wake(); lvgl_port_unlock();
        } else {
            int i = 0;
            while (i < 4 && strcmp(argv[1], NAME[i])) i++;
            if (i == 4) { printf("usage: screen [30s|1m|5m|never|wake]\n"); return 1; }
            model_lock(); g_cfg.screen_timeout = i; model_commit();
            settings_save_later();
        }
    }
    lvgl_port_lock(0); bool off = ui_screen_is_off(); lvgl_port_unlock();
    printf("screen %s, turns off after: %s, brightness %u %%\n", off ? "off (screen saver)" : "on",
           NAME[g_cfg.screen_timeout & 3], g_cfg.brightness);
    return 0;
}

// grep: BreezyBox has no line filter; this one reads a file or, in a pipeline, stdin.
static int c_grep(int argc, char **argv)
{
    bool icase = false, invert = false;
    int a = 1;
    for (; a < argc && argv[a][0] == '-' && argv[a][1]; a++) {
        for (const char *f = argv[a] + 1; *f; f++) {
            if (*f == 'i') icase = true;
            else if (*f == 'v') invert = true;
            else { printf("grep: unknown option -%c\n", *f); return 2; }
        }
    }
    if (a >= argc) { printf("usage: grep [-i] [-v] <pattern> [file]\n"); return 2; }
    const char *pat = argv[a++];
    FILE *in = a < argc ? fopen(argv[a], "r") : stdin;
    if (!in) { printf("grep: %s: can't open\n", argv[a]); return 2; }
    char line[256];
    int hits = 0;
    while (fgets(line, sizeof line, in)) {
        bool m = icase ? strcasestr(line, pat) != NULL : strstr(line, pat) != NULL;
        if (m != invert) { fputs(line, stdout); hits++; }
    }
    if (in != stdin) fclose(in);
    return hits ? 0 : 1;
}

static const esp_console_cmd_t CMDS[] = {
    { .command = "grep",     .help = "Print lines containing a pattern (-i ignore case, -v invert); reads a file or a pipe", .hint = "[-i] [-v] <pattern> [file]", .func = c_grep },
    { .command = "screen",   .help = "Screen saver: show state, set timeout (30s|1m|5m|never) or 'wake'", .hint = "[30s|1m|5m|never|wake]", .func = c_screen },
    { .command = "status",   .help = "Everything the panel knows: batteries, solar, heater, tanks, Starlink, GPS, weather, Wi-Fi, Matter, relay", .func = c_status },
    { .command = "batt",     .help = "Battery readings (voltage, current, SOC, capacity, temperature)", .func = c_batt },
    { .command = "solar",    .help = "Solar charger: PV power, charge stage, battery side, yield", .func = c_solar },
    { .command = "heater",   .help = "Truma heater: LIN link, settings, room temperature", .func = c_heater },
    { .command = "tanks",    .help = "Tank levels (LPG, grey water)", .func = c_tanks },
    { .command = "starlink", .help = "Starlink power and dish statistics", .func = c_starlink },
    { .command = "gps",      .help = "GPS fix, module link and aiding; 'raw [s]' shows the NMEA as it arrives (up to 10 s); 'assistnow <token>|off' with VD_GPS_ASSISTNOW", .hint = "[raw [seconds]|assistnow <token>|off]", .func = c_gps },
    { .command = "update",   .help = "Firmware updates from GitHub releases: show state; check, install, or set off|notify|auto", .hint = "[check|install|off|notify|auto]", .func = c_update },
    { .command = "weather",  .help = "Forecast and its location; 'loc <lat> <lon>' sets one by hand, 'refresh' fetches now", .hint = "[refresh|loc <lat> <lon>]", .func = c_weather },
    { .command = "net",      .help = "Wi-Fi in use and the remembered networks; 'forget <ssid>' drops one", .hint = "[forget <ssid>]", .func = c_wifi },
    { .command = "matter",   .help = "Apple Home (Matter): pairing state and setup code; 'qr' prints the QR payload, 'unpair' forgets every home, 'retry' after a skipped start", .hint = "[qr|unpair|retry]", .func = c_matter },
    { .command = "relay",    .help = "Cloudflare relay (remote website): state; on|off, 'url <wss://...>', 'key <secret>'", .hint = "[on|off|url <u>|key <k>]", .func = c_relay },
    { .command = "ble",      .help = "Paired BLE devices: signal, last seen, reading age, read success counters", .hint = "[reset]", .func = c_ble },
};

const esp_console_cmd_t *cli_cmds(size_t *n) { *n = sizeof CMDS / sizeof *CMDS; return CMDS; }

void cli_register(void)
{
    for (size_t i = 0; i < sizeof CMDS / sizeof *CMDS; i++) esp_console_cmd_register(&CMDS[i]);
}

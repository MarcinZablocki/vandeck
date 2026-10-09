// Shell commands for reading what the panel knows (esp_console: USB serial and the Wi-Fi console).
#include "cli.h"
#include "model.h"
#include "settings.h"
#include "ble/ble_svc.h"
#include "net/tailnet_svc.h"
#include "net/weather.h"
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
    if (h->err) printf(", error %u", h->err);
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

static void print_tailnet(const vd_model_t *m)
{
    const vd_tailnet_t *t = &m->tn;
    char ip[16];
    tailnet_svc_ip_str(t->ip, ip, sizeof ip);
    printf("tailnet: %s", tailnet_svc_state_text(t->state));
    if (t->state == TN_CONNECTED) printf(", %s as %s, %u peers", ip, tailnet_svc_hostname(), t->peers);
    else if (t->state == TN_NEED_KEY) printf(" (run 'tailnet key tskey-...')");
    else if (t->state == TN_AUTH_FAILED) printf(" (expired or revoked: set a new one with 'tailnet key tskey-...')");
    printf("\n");
}

static void print_weather(const vd_model_t *m)
{
    static const char *SRC[] = { "none", "Starlink", "manual" };
    static const char *ST[] = { "no location", "no network", "fetching", "ok", "fetch failed" };
    const vd_weather_t *w = &m->wx;
    printf("weather: %s, location %s", ST[w->state], SRC[g_cfg.wx_src]);
    if (g_cfg.wx_src) printf(" %.4f, %.4f", g_cfg.wx_lat, g_cfg.wx_lon);
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
    print_batt(m); print_solar(m); print_heater(m); print_tanks(m); print_starlink(m); print_weather(m); print_wifi(m); print_tailnet(m);
}

static int c_status(int c, char **v) { return run(print_all); }
static int c_batt(int c, char **v) { return run(print_batt); }
static int c_solar(int c, char **v) { return run(print_solar); }
static int c_heater(int c, char **v) { return run(print_heater); }
static int c_tanks(int c, char **v) { return run(print_tanks); }
static int c_starlink(int c, char **v) { return run(print_starlink); }
static int c_wifi(int c, char **v) { return run(print_wifi); }
static int c_weather(int argc, char **argv)
{
    if (argc > 1) {
        if (!strcmp(argv[1], "refresh")) { weather_refresh(); printf("fetching\n"); }
        else if (!strcmp(argv[1], "loc") && argc > 3) {
            if (!weather_set_location(strtof(argv[2], NULL), strtof(argv[3], NULL))) { printf("latitude -90..90, longitude -180..180\n"); return 1; }
            printf("location set (Starlink's position replaces it once the dish reports one)\n");
        } else { printf("usage: weather [refresh|loc <lat> <lon>]\n"); return 1; }
    }
    return run(print_weather);
}
static int c_tailnet(int argc, char **argv)
{
    if (argc > 1) {
        if (!strcmp(argv[1], "on")) { tailnet_svc_set_enabled(true); printf("tailnet switched on (state updates within a second)\n"); }
        else if (!strcmp(argv[1], "off")) { tailnet_svc_set_enabled(false); printf("tailnet switched off\n"); }
        else if (!strcmp(argv[1], "key") && argc > 2) {
            if (tailnet_svc_set_auth_key(argv[2]) != ESP_OK) { printf("that isn't a Tailscale auth key (tskey-auth-...)\n"); return 1; }
            printf("auth key saved\n");
        } else if (!strcmp(argv[1], "forget")) { tailnet_svc_forget(); printf("left the tailnet: node keys and auth key erased, switched off\n"); }
        else { printf("usage: tailnet [on|off|key <tskey-...>|forget]\n"); return 1; }
    }
    return run(print_tailnet);
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
    { .command = "status",   .help = "Everything the panel knows: batteries, solar, heater, tanks, Starlink, Wi-Fi, tailnet", .func = c_status },
    { .command = "batt",     .help = "Battery readings (voltage, current, SOC, capacity, temperature)", .func = c_batt },
    { .command = "solar",    .help = "Solar charger: PV power, charge stage, battery side, yield", .func = c_solar },
    { .command = "heater",   .help = "Truma heater: LIN link, settings, room temperature", .func = c_heater },
    { .command = "tanks",    .help = "Tank levels (LPG, grey water)", .func = c_tanks },
    { .command = "starlink", .help = "Starlink power and dish statistics", .func = c_starlink },
    { .command = "weather",  .help = "Forecast and its location; 'loc <lat> <lon>' sets one by hand, 'refresh' fetches now", .hint = "[refresh|loc <lat> <lon>]", .func = c_weather },
    { .command = "net",      .help = "Wi-Fi connection the panel is using (BreezyBox 'wifi' manages networks)", .func = c_wifi },
    { .command = "tailnet",  .help = "Tailscale link: show state; on|off, 'key <tskey-...>' to register, 'forget' to leave the tailnet", .hint = "[on|off|key <k>|forget]", .func = c_tailnet },
    { .command = "ble",      .help = "Paired BLE devices: signal, last seen, reading age, read success counters", .hint = "[reset]", .func = c_ble },
};

const esp_console_cmd_t *cli_cmds(size_t *n) { *n = sizeof CMDS / sizeof *CMDS; return CMDS; }

void cli_register(void)
{
    for (size_t i = 0; i < sizeof CMDS / sizeof *CMDS; i++) esp_console_cmd_register(&CMDS[i]);
}

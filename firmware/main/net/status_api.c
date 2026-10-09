#include "status_api.h"
#include "ota_svc.h"
#include "tailnet_svc.h"
#include "app/cli.h"
#include "app/model.h"
#include "app/settings.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUF_SZ 3072

typedef struct { char *b; size_t n; } jb_t;

static void add(jb_t *j, const char *fmt, ...)
{
    va_list a; va_start(a, fmt);
    int w = vsnprintf(j->b + j->n, BUF_SZ - j->n, fmt, a);
    va_end(a);
    if (w > 0) j->n = j->n + w < BUF_SZ ? j->n + w : BUF_SZ - 1;
}

// Objects are written with a trailing comma after every member; end() turns the last one into the closing brace.
static void end(jb_t *j, const char *tail)
{
    if (j->n && j->b[j->n - 1] == ',') j->n--;
    add(j, "%s", tail);
}

static void num(jb_t *j, const char *k, float v, int dec)
{
    if (isnan(v) || isinf(v)) add(j, "\"%s\":null,", k); else add(j, "\"%s\":%.*f,", k, dec, v);
}
static void integer(jb_t *j, const char *k, long v) { add(j, "\"%s\":%ld,", k, v); }
static void boolean(jb_t *j, const char *k, bool v) { add(j, "\"%s\":%s,", k, v ? "true" : "false"); }
static void age(jb_t *j, int64_t ts)
{
    if (ts) add(j, "\"age_s\":%ld,", (long)((now_us() - ts) / 1000000)); else add(j, "\"age_s\":null,");
}
static void str(jb_t *j, const char *k, const char *s)
{
    add(j, "\"%s\":\"", k);
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') add(j, "\\%c", c);
        else if (c < 0x20) add(j, "\\u%04x", c);
        else add(j, "%c", c);
    }
    add(j, "\",");
}

static esp_err_t on_status(httpd_req_t *r)
{
    if (!ota_svc_check_key(r)) {
        httpd_resp_set_status(r, "403 Forbidden");
        return httpd_resp_sendstr(r, "bad or missing X-OTA-Key\n");
    }
    vd_model_t *m = heap_caps_malloc(sizeof *m, MALLOC_CAP_SPIRAM);
    char *buf = heap_caps_malloc(BUF_SZ, MALLOC_CAP_SPIRAM);
    if (!m || !buf) {
        free(m); free(buf);
        httpd_resp_set_status(r, "500 Internal Server Error");
        return httpd_resp_sendstr(r, "out of memory\n");
    }
    model_snapshot(m);
    jb_t j = { buf, 0 };

    add(&j, "{");
    str(&j, "version", esp_app_get_description()->version);
    integer(&j, "uptime_s", (long)(esp_timer_get_time() / 1000000));
    boolean(&j, "demo", m->demo);

    add(&j, "\"battery\":[");
    for (int i = 0; i < 2; i++) {
        const vd_batt_t *b = &m->batt[i];
        add(&j, "{");
        boolean(&j, "paired", g_cfg.dev[ROLE_BATT1 + i].paired);
        boolean(&j, "valid", b->valid);
        if (b->valid) {
            num(&j, "soc", b->soc, 0); num(&j, "volts", b->volts, 2); num(&j, "amps", b->amps, 1);
            num(&j, "remain_ah", b->remain_ah, 1); num(&j, "full_ah", b->full_ah, 1); num(&j, "temp_c", b->temp_c, 0);
            age(&j, b->ts_us);
        }
        end(&j, "},");
    }
    end(&j, "],");

    const vd_solar_t *s = &m->solar;
    add(&j, "\"solar\":{");
    boolean(&j, "paired", g_cfg.dev[ROLE_SOLAR].paired);
    boolean(&j, "valid", s->valid);
    if (s->valid) {
        num(&j, "pv_w", s->pv_w, 0); num(&j, "batt_v", s->batt_v, 2); num(&j, "batt_a", s->batt_a, 1);
        num(&j, "load_a", s->load_a, 1); num(&j, "yield_kwh", s->yield_wh / 1000.0f, 2);
        integer(&j, "mode", s->mode); str(&j, "stage", cli_solar_stage(s->mode)); integer(&j, "error", s->err);
        age(&j, s->ts_us);
    }
    end(&j, "},");

    const vd_heater_t *h = &m->heater;
    add(&j, "\"heater\":{");
    boolean(&j, "link", h->link); boolean(&j, "registered", h->registered); boolean(&j, "heating", h->heating);
    boolean(&j, "on", g_cfg.heater_on); num(&j, "setpoint_c", g_cfg.setpoint_c, 1); num(&j, "room_c", h->room_c, 1);
    integer(&j, "water", g_cfg.water); integer(&j, "fan", g_cfg.fan); integer(&j, "error", h->err); integer(&j, "warning", h->warn);
    end(&j, "},");

    static const char *TANK[] = { "lpg", "grey" };
    add(&j, "\"tanks\":{");
    for (int i = 0; i < 2; i++) {
        const vd_tank_t *t = &m->tank[i];
        add(&j, "\"%s\":{", TANK[i]);
        if (t->level < 0) add(&j, "\"level\":null,"); else integer(&j, "level", t->level & 3);   // 0 empty .. 3 full
        age(&j, t->level < 0 ? 0 : t->read_at_us);
        end(&j, "},");
    }
    end(&j, "},");

    const vd_starlink_t *sl = &m->sl;
    add(&j, "\"starlink\":{");
    boolean(&j, "power", sl->power); boolean(&j, "reachable", sl->reachable); boolean(&j, "heating", sl->heating);
    end(&j, "},");

    add(&j, "\"wifi\":{");
    boolean(&j, "connected", m->wifi.connected);
    if (m->wifi.connected) str(&j, "ssid", g_cfg.wifi_ssid);
    end(&j, "},");

    char ip[16];
    tailnet_svc_ip_str(m->tn.ip, ip, sizeof ip);
    add(&j, "\"tailnet\":{");
    str(&j, "state", tailnet_svc_state_text(m->tn.state));
    if (m->tn.ip) str(&j, "ip", ip);
    integer(&j, "peers", m->tn.peers);
    end(&j, "}");
    end(&j, "}\n");

    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(r, buf, j.n);
    free(m); free(buf);
    return e;
}

void status_api_register(httpd_handle_t srv)
{
    httpd_uri_t u = { .uri = "/status", .method = HTTP_GET, .handler = on_status };
    httpd_register_uri_handler(srv, &u);
}

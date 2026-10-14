#include "time_svc.h"
#include "app/model.h"
#include "app/settings.h"
#include "app/solar_log.h"
#include "esp_netif_sntp.h"
#include <time.h>
#include <sys/time.h>
#include <stdlib.h>

const vd_zone_t VD_ZONES[] = {
    { "Pacific",        "PST8PDT,M3.2.0,M11.1.0" },
    { "Mountain",       "MST7MDT,M3.2.0,M11.1.0" },
    { "Central",        "CST6CDT,M3.2.0,M11.1.0" },
    { "Eastern",        "EST5EDT,M3.2.0,M11.1.0" },
    { "London",         "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Sydney",         "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};
const int VD_ZONE_COUNT = sizeof VD_ZONES / sizeof VD_ZONES[0];
static bool s_sntp;

static void synced(struct timeval *tv) { model_lock(); M.time_synced = true; model_commit(); }

void time_svc_apply_tz(void)
{
    setenv("TZ", VD_ZONES[g_cfg.tz % VD_ZONE_COUNT].posix, 1);
    tzset();
    model_lock(); model_commit();
}

void time_svc_init(void) { time_svc_apply_tz(); }

void time_svc_on_network(void)
{
    if (!g_cfg.auto_time || s_sntp) return;
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    c.sync_cb = synced;
    if (esp_netif_sntp_init(&c) == ESP_OK) s_sntp = true;
}

void time_svc_set_auto(bool on)
{
    model_lock(); g_cfg.auto_time = on; bool online = M.wifi.connected; model_commit();
    settings_save_later();
    if (!on && s_sntp) { esp_netif_sntp_deinit(); s_sntp = false; }
    else if (on && online) time_svc_on_network();
}

// GPS time (RMC) while SNTP hasn't synced: no internet, or not yet. Only with automatic time on, and only when the
// clock is off by more than 2 s (RMC arrives a few hundred ms into the second it names).
bool time_svc_set_from_gps(int64_t utc)
{
    model_lock(); bool sntp = M.time_synced; model_unlock();
    if (!g_cfg.auto_time || sntp || utc < 1735689600) return false;
    int64_t d = utc - (int64_t)time(NULL);
    if (d >= -2 && d <= 2) return false;
    struct timeval tv = { .tv_sec = (time_t)utc };
    settimeofday(&tv, NULL);
    model_lock(); model_commit();
    return true;
}

bool time_valid(void) { return time(NULL) > 1735689600; }   // after 2025-01-01

void time_svc_shift(int field, int delta)
{
    time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
    if (!time_valid()) { lt.tm_year = 126; lt.tm_mon = 0; lt.tm_mday = 1; lt.tm_hour = 12; lt.tm_min = 0; }
    switch (field) {
    case 0: lt.tm_hour += delta; break;
    case 1: lt.tm_min += delta; break;
    case 2: lt.tm_mday += delta; break;
    case 3: lt.tm_mon += delta; break;
    default: lt.tm_year += delta; break;
    }
    lt.tm_isdst = -1;
    struct timeval tv = { .tv_sec = mktime(&lt) };
    settimeofday(&tv, NULL);
    model_lock(); model_commit();
}

int time_utc_offset_min(void)
{
    time_t t = time(NULL); struct tm g, l;
    gmtime_r(&t, &g); localtime_r(&t, &l);
    int32_t dg = days_from_civil(g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    int32_t dl = days_from_civil(l.tm_year + 1900, l.tm_mon + 1, l.tm_mday);
    return (int)((dl - dg) * 1440 + (l.tm_hour - g.tm_hour) * 60 + (l.tm_min - g.tm_min));
}

#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct { const char *name; const char *posix; } vd_zone_t;
extern const vd_zone_t VD_ZONES[];
extern const int VD_ZONE_COUNT;

void time_svc_init(void);
void time_svc_apply_tz(void);         // after changing g_cfg.tz
void time_svc_on_network(void);       // called by wifi_svc once we have an IP
void time_svc_set_auto(bool on);
void time_svc_shift(int field, int delta);   // 0 hour, 1 minute, 2 day, 3 month, 4 year
bool time_svc_set_from_gps(int64_t utc);   // sets the clock if SNTP hasn't; true if it changed it
bool time_valid(void);
int time_utc_offset_min(void);

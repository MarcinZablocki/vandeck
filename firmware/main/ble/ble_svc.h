#pragma once
#include "app/model.h"

void ble_svc_init(void);
void ble_pair_begin(vd_role_t role);      // scan for devices of that role's type
void ble_pair_cancel(void);
void ble_pair_select(int idx, const uint8_t key[16], const char *pin);   // key/pin only for Victron
void ble_forget(vd_role_t role);

// used by gatt_link: NimBLE can't connect while discovery runs
void ble_svc_scan_pause(void);
void ble_svc_scan_resume(void);

// Diagnostics for the console: per paired device, advert RSSI and when it was last seen.
#include <stddef.h>
void ble_svc_dump(char *buf, size_t n);
void ble_svc_stats_reset(void);   // restart the counters (for comparing periods)

#pragma once
#include "host/ble_hs.h"

void litime_init(void);
void litime_on_seen(int idx, const ble_addr_t *addr);   // connects if paired and not connected
void litime_forget(int idx);

// Diagnostics: read attempts, readings obtained, attempts that never connected, connected but no reading.
typedef struct { uint32_t attempts, ok, no_connect, no_reading, wr_ok, wr_fail, notif, bad; } litime_stats_t;
litime_stats_t litime_stats(int idx);
void litime_stats_reset(void);

#pragma once
#include <stdint.h>
#include <stdbool.h>

// One record per local calendar day, logged by the panel from live SmartSolar readings
// (the controller's own history isn't available over instant readout).
typedef struct __attribute__((packed)) {
    uint32_t day;              // days since 1970-01-01 (local)
    uint16_t yield_wh, max_w, max_v_x10, min_bulk, min_abs, min_float;
} vd_day_t;

void solar_log_init(void);
bool solar_log_get(int32_t day, vd_day_t *out);
int32_t solar_log_today(void);        // -1 until the clock is set
int32_t solar_log_first_day(void);    // -1 when empty
int32_t days_from_civil(int y, unsigned m, unsigned d);
void civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d);

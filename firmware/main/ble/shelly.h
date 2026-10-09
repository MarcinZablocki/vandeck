#pragma once
#include <stdbool.h>
#include "host/ble_hs.h"

void shelly_init(void);
void shelly_on_seen(const ble_addr_t *addr);
void shelly_set_output(bool on);   // Switch.Set on output 0 (Starlink)
void shelly_refresh(void);         // Switch.GetStatus
void shelly_forget(void);

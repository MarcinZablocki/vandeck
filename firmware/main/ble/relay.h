#pragma once
#include <stdbool.h>
#include "host/ble_hs.h"

void relay_init(void);
void relay_on_seen(const ble_addr_t *addr);
void relay_set_output(bool on);   // switch the Starlink relay
void relay_refresh(void);         // read the relay state
void relay_forget(void);

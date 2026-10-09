#pragma once
#include <stdint.h>
// Victron "instant readout" advertisement (manufacturer data incl. the 0x02E1 company id).
// Replace with your own decoder if you already have one working.
void victron_on_adv(const uint8_t *mfg, int len);

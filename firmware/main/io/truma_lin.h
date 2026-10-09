#pragma once
#include <stdbool.h>
#include <stdint.h>

void truma_init(void);
void truma_set_power(bool on);
void truma_set_setpoint(float c);
void truma_set_water(uint8_t mode);   // WATER_*
void truma_set_fan(uint8_t mode);     // FAN_*

// Heater fault code (M.heater.err, as the CP Plus shows it) -> short cause; *fix gets what to do about it.
const char *truma_err_text(uint16_t code, const char **fix);

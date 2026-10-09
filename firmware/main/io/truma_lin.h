#pragma once
#include <stdbool.h>
#include <stdint.h>

void truma_init(void);
void truma_set_power(bool on);
void truma_set_setpoint(float c);
void truma_set_water(uint8_t mode);   // WATER_*
void truma_set_fan(uint8_t mode);     // FAN_*

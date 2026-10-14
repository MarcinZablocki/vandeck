#pragma once
#include <stdbool.h>
#include <stdint.h>

void starlink_init(void);
void starlink_set_power(bool on);     // via the relay node (demo mode when it isn't paired)
void starlink_reboot(void);
void starlink_set_snow(uint8_t mode); // SNOW_AUTO / SNOW_ON / SNOW_OFF

#pragma once
#include <stdbool.h>

void weather_init(void);
void weather_refresh(void);                    // fetch again now (if there is a location and a network)
bool weather_set_location(float lat, float lon);   // manual location; false if out of range
const char *weather_text(int code);            // WMO weather code -> "Partly cloudy"
float weather_expected_kwh(float rad_kwh_m2);  // daily irradiation -> expected PV yield for this array
bool weather_place(const char **name, const char **country);   // nearest town to the weather location, if known

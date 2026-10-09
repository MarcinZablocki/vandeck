// Truma Combi fault codes as the CP Plus shows them: "E" stops the heater until it is switched off and on again,
// "W" is a warning it keeps running through. Sources: Truma Combi troubleshooting guides and tech tips.
#include "truma_lin.h"
#include <stddef.h>

typedef struct { uint16_t code; const char *title, *fix; } truma_err_t;

static const truma_err_t ERRS[] = {
    { 17,  "Water tank empty",        "Switch off, let it cool down, then fill the water heater." },
    { 18,  "Air outlet blocked",      "Clear the warm-air outlets and the air intake." },
    { 21,  "Room sensor fault",       "Sensor or its cable is faulty: contact Truma service." },
    { 24,  "Battery voltage low",     "Below 10.4 V: charge the house battery." },
    { 41,  "Electronics overheated",  "Let it cool down; if it keeps happening, contact Truma service." },
    { 43,  "Voltage too high",        "Above 16.4 V: check the charger and shore power." },
    { 44,  "Battery voltage too low", "Below 10 V: charge the house battery." },
    { 45,  "No mains power",          "The electric heating elements have no AC supply: check shore power." },
    { 112, "No flame",                "Open the gas valve, check the LPG, clear the outside flue." },
    { 121, "No flame",                "Open the gas valve, check the LPG, clear the outside flue." },
    { 122, "No flame",                "Open the gas valve, check the LPG, clear the outside flue." },
    { 202, "No flame",                "Open the gas valve, check the LPG, clear the outside flue." },
    { 211, "No flame",                "Open the gas valve, check the LPG, clear the outside flue." },
    { 212, "No flame",                "Open the gas valve, check the LPG, clear the outside flue." },
    { 255, "No contact with heater",  "Check the heater's 12 V supply and its cable to the CP Plus." },
};

const char *truma_err_text(uint16_t code, const char **fix)
{
    for (size_t i = 0; i < sizeof ERRS / sizeof *ERRS; i++)
        if (ERRS[i].code == code) { if (fix) *fix = ERRS[i].fix; return ERRS[i].title; }
    if (fix) *fix = "See the Truma manual for this code.";
    return "Heater fault";
}

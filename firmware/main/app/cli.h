#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_console.h"

// The panel's shell commands (status, batt, solar, heater, tanks, starlink, net, ble). They print with printf,
// so they work on any stdout (USB serial, Wi-Fi console).
const char *cli_solar_stage(uint8_t victron_mode);   // "bulk", "float", ... (also used by GET /status)
const esp_console_cmd_t *cli_cmds(size_t *n);   // for BreezyBox, which registers them itself
void cli_register(void);                        // after esp_console_init(), when BreezyBox isn't running

// Starts the shell: BreezyBox on the USB serial port when the 'storage' (LittleFS) partition exists, otherwise
// plain esp_console. Returns true if BreezyBox is running. Either way, shell_exec() runs one line.
bool shell_start(void);
int shell_exec(const char *line);

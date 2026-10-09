#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

// Firmware updates over Wi-Fi: POST the app image to http://vandeck.local/ota (tools/ota.sh), GET /version.
void ota_svc_init(void);
bool ota_svc_check_key(httpd_req_t *r);   // true if the request carries the right X-OTA-Key header (also used by /status)
void ota_svc_mark_valid(void);   // confirm a freshly updated image so the bootloader won't roll it back,
                                 // and record when/how the running image was installed
#include <stdbool.h>
#include <time.h>
// When the running firmware was installed: at = 0 if unknown (not recorded yet, or the clock wasn't set).
typedef struct { time_t at; bool via_ota; bool known; } ota_install_t;
ota_install_t ota_svc_last_install(void);

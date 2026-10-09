#pragma once
#include <stdbool.h>
#include "esp_http_server.h"
#include "esp_partition.h"
#include "esp_err.h"
#include "app/model.h"

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

// Shared with the GitHub updater (update_svc): one image write at a time, the same finish and record keeping.
bool ota_svc_claim(void);                     // false if an update is already being written
void ota_svc_release(void);
bool ota_svc_busy(void);                      // an image is being written (until release or restart)
void ota_svc_set_state(vd_ota_state_t st, int pct, const char *msg);   // the full-screen progress overlay
// Boot the freshly written (and esp_ota_end-verified) image in dst: records it, restarts in ~1.5 s.
// quiet: no "Restarting" overlay, and the next boot keeps the screen dark (a night-time automatic update).
esp_err_t ota_svc_commit(const esp_partition_t *dst, bool quiet);
bool ota_svc_take_quiet_boot(void);           // true once after a quiet commit

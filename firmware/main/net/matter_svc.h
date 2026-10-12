#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Matter node for Apple Home (and any other Matter controller). Each battery is a temperature sensor with a Power
// Source cluster (charge %, charging state, low battery, voltage); the bank is an electrical sensor whose power shows
// in the iOS 27 Energy view. Pairing is over the network the panel is already on (no BLE: NimBLE stays VanDeck's),
// with a setup code generated once per panel and kept in NVS.
typedef enum { MT_OFF, MT_STARTING, MT_UNPAIRED, MT_PAIRED, MT_ERROR } vd_mt_state_t;

void matter_svc_init(void);                 // once the running image is confirmed (main.c)
vd_mt_state_t matter_svc_state(void);
const char *matter_svc_state_text(vd_mt_state_t st);
uint8_t matter_svc_fabrics(void);           // controllers (Apple Home, Google Home, ...) the panel is paired with
bool matter_svc_qr(char *buf, size_t n);    // "MT:..." payload for the pairing QR code
bool matter_svc_manual_code(char *buf, size_t n);   // 11-digit code to type in instead
void matter_svc_reset(void);                // forget every controller and reopen pairing (restarts the panel)
bool matter_svc_skipped(void);              // not started: the previous start brought the panel down
void matter_svc_retry(void);                // clear that and restart the panel to try again

#ifdef __cplusplus
}
#endif

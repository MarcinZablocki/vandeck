#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "app/model.h"

// Tailscale node on the panel (microlink: ts2021 control plane + WireGuard). While it is up the panel has a
// 100.x address and its HTTP server (OTA, /version, /status) and console (port 23) answer on it like on the LAN.
// State for the UI and shell is in M.tn. Off by default; it needs a Tailscale auth key once to register, after
// which the node keys it stored in NVS are enough to reconnect.
void tailnet_svc_init(void);                       // after Wi-Fi is initialised
bool tailnet_svc_enabled(void);
void tailnet_svc_set_enabled(bool on);
esp_err_t tailnet_svc_set_auth_key(const char *key);   // "tskey-..."; ESP_ERR_INVALID_ARG if it isn't one
void tailnet_svc_forget(void);                     // leave the tailnet: erase node keys and the stored auth key, switch off
const char *tailnet_svc_state_text(vd_tn_state_t st);
const char *tailnet_svc_hostname(void);            // the name the panel registers with
void tailnet_svc_ip_str(uint32_t ip, char *buf, size_t n);   // "100.x.y.z"; "--" for 0

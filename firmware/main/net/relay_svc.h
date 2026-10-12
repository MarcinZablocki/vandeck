#pragma once
#include <stdbool.h>

// Outbound link to the Cloudflare relay (cloud/ in this repo): the panel keeps one TLS WebSocket open to the
// vandeck-link Worker, sends its status JSON (as GET /status) every 10 s, and takes heater commands from the website.
// Nothing listens on the panel, so it works behind any NAT. Off until a URL and key are set ('relay' command).
typedef enum { RELAY_OFF, RELAY_NOT_SET, RELAY_WAIT_WIFI, RELAY_CONNECTING, RELAY_CONNECTED } vd_relay_state_t;

void relay_svc_init(void);                 // after Wi-Fi is initialised
vd_relay_state_t relay_svc_state(void);
const char *relay_svc_state_text(vd_relay_state_t st);
const char *relay_svc_url(void);           // "" when not set
bool relay_svc_enabled(void);
void relay_svc_set_enabled(bool on);
bool relay_svc_set_url(const char *url);   // wss://...; false if it isn't one
bool relay_svc_set_key(const char *key);   // the Worker's PANEL_KEY; 32..127 characters

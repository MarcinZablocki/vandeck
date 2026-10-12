#pragma once
#include "esp_http_server.h"

// GET /status: what the panel knows (batteries, solar, heater, tanks, Starlink, Wi-Fi, Matter) as JSON, read-only.
// Needs the same X-OTA-Key header as /ota.
void status_api_register(httpd_handle_t srv);

#define STATUS_JSON_MAX 3072
// The same JSON into buf (not terminated); returns its length, 0 if out of memory. Also used by relay_svc.
size_t status_api_json(char *buf, size_t cap);

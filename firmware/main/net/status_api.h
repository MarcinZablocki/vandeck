#pragma once
#include "esp_http_server.h"

// GET /status: what the panel knows (batteries, solar, heater, tanks, Starlink, Wi-Fi, Matter) as JSON, read-only.
// Needs the same X-OTA-Key header as /ota.
void status_api_register(httpd_handle_t srv);

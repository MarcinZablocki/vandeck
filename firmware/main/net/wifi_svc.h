#pragma once
#include <stdbool.h>

void wifi_svc_init(void);
void wifi_svc_set_enabled(bool on);
void wifi_svc_scan(void);
void wifi_svc_connect(const char *ssid, const char *pass);

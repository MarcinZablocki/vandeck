#pragma once
#include <stdbool.h>

#define WIFI_SAVED_MAX 8

// Wi-Fi with remembered networks (see wifi_svc.c): joins the strongest remembered one in range by itself.
void wifi_svc_init(void);
void wifi_svc_set_enabled(bool on);
void wifi_svc_scan(void);
void wifi_svc_connect(const char *ssid, const char *pass);   // remembered once it connects
bool wifi_svc_connect_saved(const char *ssid);               // false if it isn't remembered
bool wifi_svc_is_saved(const char *ssid);
int wifi_svc_saved(char (*ssids)[33], int max);              // remembered SSIDs, newest first
bool wifi_svc_forget(const char *ssid);

// Desktop stand-ins for the ESP-IDF / FreeRTOS APIs the UI and model code touch.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102

#define ESP_LOGE(tag, fmt, ...) printf("E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("I %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)

// esp_timer: backed by LVGL timers in sim_main.c
typedef void (*esp_timer_cb_t)(void *arg);
typedef struct { esp_timer_cb_t callback; void *arg; int dispatch_method; const char *name; bool skip_unhandled_events; } esp_timer_create_args_t;
typedef struct esp_timer *esp_timer_handle_t;
int64_t esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period_us);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);
static inline uint32_t esp_random(void) { return (uint32_t)rand(); }

// FreeRTOS mutex: single-threaded here
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFF
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return (void *)1; }
#define xSemaphoreTakeRecursive(m, t) (1)
#define xSemaphoreGiveRecursive(m) (1)

typedef struct { char version[32], project_name[32], time[16], date[16]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
typedef struct { char label[17]; } esp_partition_t;
const esp_partition_t *esp_ota_get_running_partition(void);

typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct esp_netif_obj esp_netif_t;
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr & 0xff), (int)(((a)->addr >> 8) & 0xff), (int)(((a)->addr >> 16) & 0xff), (int)(((a)->addr >> 24) & 0xff)
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *k);
esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ip);

typedef struct httpd_req httpd_req_t;

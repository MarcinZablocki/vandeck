#include "settings.h"
#include <string.h>
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"

vd_settings_t g_cfg;
static esp_timer_handle_t s_t;

static void defaults(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.magic = VD_CFG_MAGIC; g_cfg.ver = VD_CFG_VER;
    g_cfg.wifi_on = true; g_cfg.auto_time = true; g_cfg.tz = 5;
    g_cfg.setpoint_c = 21.0f; g_cfg.water = WATER_ECO; g_cfg.fan = FAN_ECO;
    g_cfg.tank_show_sec = 30;
    g_cfg.screen_timeout = SCREEN_TO_5M; g_cfg.brightness = 80;
}

int settings_screen_timeout_ms(void)
{
    static const int MS[] = { 30000, 60000, 300000, 0 };
    return g_cfg.screen_timeout <= SCREEN_TO_NEVER ? MS[g_cfg.screen_timeout] : 0;
}

static void save_now(void *arg)
{
    vd_settings_t copy;
    model_lock(); copy = g_cfg; model_unlock();
    nvs_handle_t h;
    if (nvs_open("vandeck", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &copy, sizeof copy);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI("cfg", "saved");
}

void settings_load(void)
{
    defaults();
    nvs_handle_t h;
    if (nvs_open("vandeck", NVS_READONLY, &h) == ESP_OK) {
        // A blob from an older version is shorter: keep what it has and let defaults() fill the newer fields.
        vd_settings_t tmp; size_t sz = 0;
        if (nvs_get_blob(h, "cfg", NULL, &sz) == ESP_OK && sz <= sizeof tmp &&
            nvs_get_blob(h, "cfg", &tmp, &sz) == ESP_OK && tmp.magic == VD_CFG_MAGIC && tmp.ver <= VD_CFG_VER) {
            uint16_t ver = tmp.ver;
            memcpy(&g_cfg, &tmp, sz);
            if (ver < 2) { g_cfg.screen_timeout = SCREEN_TO_5M; g_cfg.brightness = 80; }
            if (g_cfg.brightness < 10 || g_cfg.brightness > 100) g_cfg.brightness = 80;
            g_cfg.ver = VD_CFG_VER;
            ESP_LOGI("cfg", "loaded settings v%u", ver);
        }
        nvs_close(h);
    }
    esp_timer_create_args_t a = { .callback = save_now, .name = "cfg_save" };
    esp_timer_create(&a, &s_t);
}

void settings_save_later(void)
{
    esp_timer_stop(s_t);
    esp_timer_start_once(s_t, 2000000);
}

#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "board/board.h"
#include "app/model.h"
#include "app/settings.h"
#include "app/solar_log.h"
#include "app/sim.h"
#include "net/wifi_svc.h"
#include "net/time_svc.h"
#include "net/starlink.h"
#include "net/ota_svc.h"
#include "net/tailnet_svc.h"
#include "net/log_svc.h"
#include "app/cli.h"
#include "ble/ble_svc.h"
#include "io/truma_lin.h"
#include "io/tanks.h"
#include "ui/ui.h"

static void startup(void *arg)
{
    log_svc_capture();   // from here on, log output is also kept for the Wi-Fi console
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    model_init();
    settings_load();
    time_svc_init();
    solar_log_init();

    board_init();
    lvgl_port_lock(0);
    ui_splash_show();
    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(60));   // let the splash reach the panel before the backlight comes on
    board_set_backlight(g_cfg.brightness);
    lvgl_port_lock(0);
    ui_init();
    lvgl_port_unlock();

    wifi_svc_init();
    ota_svc_init();
    tailnet_svc_init();   // after Wi-Fi: waits for an IP, then joins the tailnet if it has been switched on
    shell_start();   // BreezyBox on USB serial (if the storage partition exists); shell commands for both consoles
    log_svc_start();
    ble_svc_init();
    truma_init();
    tanks_init();
    starlink_init();
#if CONFIG_VD_SIMULATE
    sim_start();
#endif
    lvgl_port_lock(0);
    ui_splash_hide();
    lvgl_port_unlock();

    // A freshly updated image boots "pending verify": confirm it only after it has run for a while, so one that
    // crashes or hangs during start-up gets rolled back to the previous firmware by the bootloader.
    vTaskDelay(pdMS_TO_TICKS(30000));
    ota_svc_mark_valid();
    vTaskDelete(NULL);
}

// Start-up runs in its own task rather than on the main task. Before app_main the big 384 KB SRAM block isn't on
// the heap yet and esp_hosted takes most of what is, so the main task's stack ends up in LP (RTC) RAM, which the
// HP CPU reaches ~100x slower. A task created here gets its stack from normal SRAM.
void app_main(void)
{
    xTaskCreatePinnedToCore(startup, "startup", 12288, NULL, 2, NULL, 0);
}

// The P4's 8 KB SPM (TCM) is part of the internal heap, but flash operations (NVS, BLE bond storage) assert if
// the calling task's stack is there (esp_task_stack_is_sane_cache_disabled only accepts DRAM). Claim all of it
// before any task stack is allocated so none can land there.
static void __attribute__((constructor(101))) reserve_spm(void)
{
    size_t n;
    while ((n = heap_caps_get_largest_free_block(MALLOC_CAP_SPM)) >= 16) heap_caps_malloc(n, MALLOC_CAP_SPM);
}

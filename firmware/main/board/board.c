// CrowPanel Advanced 7" ESP32-P4 (V1.1): 1024x600 EK79007 MIPI-DSI (2 lanes) + GT911 touch.
// Pins and timing from Elecrow's board_config.h, proven on this board by ~/Projects/ESP32-Truma (main/ui/bsp_shim.c).
#include "board.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_touch_gt911.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_H        1024
#define LCD_V        600
#define PIN_BL_PWM   31
#define PIN_TP_SDA   45
#define PIN_TP_SCL   46
#define PIN_TP_INT   42
#define PIN_TP_RST   40
#define PIN_LCD_RST  41
#define LCD_CLK_MHZ  51

// The panel shows its own blue "no signal" screen whenever the DSI stream stops (restart, early boot) while the
// backlight is lit. Keep the backlight dark from the earliest point of boot until the first frame is drawn, and
// switch it off before every software restart.
static void __attribute__((constructor(102))) backlight_off_early(void)
{
    gpio_set_direction(PIN_BL_PWM, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BL_PWM, 0);
}

static void backlight_off_on_restart(void)
{
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);   // idle level low
    esp_rom_delay_us(20000);                            // let the panel go dark before the stream stops
}

static void backlight_init(void)
{
    ledc_timer_config_t t = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                              .timer_num = LEDC_TIMER_0, .freq_hz = 30000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&t);
    ledc_channel_config_t c = { .gpio_num = PIN_BL_PWM, .speed_mode = LEDC_LOW_SPEED_MODE,
                                .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 0 };
    ledc_channel_config(&c);
}

void board_set_backlight(uint8_t pct)
{
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * pct / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

lv_display_t *board_init(void)
{
    backlight_init();
    esp_register_shutdown_handler(backlight_off_on_restart);

    // LDO3 feeds the MIPI D-PHY (without it the PHY never locks and init hangs); LDO4 the I2C/touch pull-ups.
    esp_ldo_channel_handle_t ldo3 = NULL, ldo4 = NULL;
    esp_ldo_channel_config_t l3 = { .chan_id = 3, .voltage_mv = 2500 }, l4 = { .chan_id = 4, .voltage_mv = 3300 };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&l3, &ldo3));
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&l4, &ldo4));

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bc = { .bus_id = 0, .num_data_lanes = 2, .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
                                    .lane_bit_rate_mbps = 1000 };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bc, &bus));
    vTaskDelay(pdMS_TO_TICKS(50));   // let the PHY settle before DBI transfers

    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dc = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(bus, &dc, &io));

    // Elecrow's timing for this panel (the driver's EK79007_1024_600_PANEL_60HZ_CONFIG differs and uses pre-IDF-6 fields).
    esp_lcd_dpi_panel_config_t dpi = {
        .virtual_channel = 0, .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT, .dpi_clock_freq_mhz = LCD_CLK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565, .out_color_format = LCD_COLOR_FMT_RGB565, .num_fbs = 2,
        .video_timing = { .h_size = LCD_H, .v_size = LCD_V,
                          .hsync_pulse_width = 70, .hsync_back_porch = 160, .hsync_front_porch = 160,
                          .vsync_pulse_width = 10, .vsync_back_porch = 23, .vsync_front_porch = 21 },
    };
    ek79007_vendor_config_t vc = { .mipi_config = { .dsi_bus = bus, .dpi_config = &dpi, .lane_num = 2 } };
    esp_lcd_panel_dev_config_t pc = { .reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
                                      .bits_per_pixel = 16, .vendor_config = &vc };
    esp_lcd_panel_handle_t panel;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(io, &pc, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    esp_err_t on = esp_lcd_panel_disp_on_off(panel, true);   // EK79007 has no on/off command
    if (on != ESP_OK && on != ESP_ERR_NOT_SUPPORTED) ESP_ERROR_CHECK(on);

    lvgl_port_cfg_t lp = ESP_LVGL_PORT_INIT_CONFIG();
    lp.task_priority = 4;
    lp.task_stack = 12288;
    ESP_ERROR_CHECK(lvgl_port_init(&lp));

    lvgl_port_display_cfg_t d = {
        .io_handle = io, .panel_handle = panel, .buffer_size = LCD_H * LCD_V, .double_buffer = true,
        .hres = LCD_H, .vres = LCD_V, .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_spiram = true, .direct_mode = true },
    };
    // No tearing: LVGL draws straight into the back one of the driver's two framebuffers (direct mode, changed areas
    // only) and the panel switches buffers at the end of a frame. avoid_tearing needs direct_mode or full_refresh:
    // without either, the port never signals flush-ready and the LVGL task blocks forever.
    lvgl_port_display_dsi_cfg_t dd = { .flags = { .avoid_tearing = true } };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&d, &dd);

    i2c_master_bus_handle_t i2c;
    i2c_master_bus_config_t ic = { .i2c_port = I2C_NUM_1, .sda_io_num = PIN_TP_SDA, .scl_io_num = PIN_TP_SCL,
                                   .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                   .flags.enable_internal_pullup = true };
    ESP_ERROR_CHECK(i2c_new_master_bus(&ic, &i2c));
    esp_lcd_panel_io_handle_t tio;
    esp_lcd_panel_io_i2c_config_t tc = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tc.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c, &tc, &tio));
    esp_lcd_touch_config_t tcfg = { .x_max = LCD_H, .y_max = LCD_V, .rst_gpio_num = PIN_TP_RST,
                                    .int_gpio_num = PIN_TP_INT };
    esp_lcd_touch_handle_t tp;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tio, &tcfg, &tp));
    lvgl_port_touch_cfg_t tpc = { .disp = disp, .handle = tp };
    lvgl_port_add_touch(&tpc);

    return disp;
}

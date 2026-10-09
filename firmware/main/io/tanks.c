#include "tanks.h"
#include "app/model.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#if CONFIG_VD_TANK_MODE_ADC
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#endif

static const char *TAG = "tanks";
static QueueHandle_t s_q;

#if CONFIG_VD_TANK_MODE_DIGITAL
static const int PINS[2][3] = {
    { CONFIG_VD_TANK_LPG_GPIO_1, CONFIG_VD_TANK_LPG_GPIO_2, CONFIG_VD_TANK_LPG_GPIO_3 },
    { CONFIG_VD_TANK_GREY_GPIO_1, CONFIG_VD_TANK_GREY_GPIO_2, CONFIG_VD_TANK_GREY_GPIO_3 },
};
#elif CONFIG_VD_TANK_MODE_ADC
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static const int CH[2] = { CONFIG_VD_TANK_LPG_ADC_CH, CONFIG_VD_TANK_GREY_ADC_CH };
#endif

static int8_t measure(int t)
{
#if CONFIG_VD_TANK_MODE_DIGITAL
    int lvl = 0;
    for (int i = 0; i < 3; i++) {
        if (PINS[t][i] < 0) continue;
        int v = gpio_get_level(PINS[t][i]);
#ifdef CONFIG_VD_TANK_ACTIVE_LOW
        v = !v;
#endif
        if (v) lvl = i + 1;
    }
    return lvl;
#elif CONFIG_VD_TANK_MODE_ADC
    if (CH[t] < 0) return -1;
    int sum = 0;
    for (int i = 0; i < 16; i++) {
        int raw = 0, mv;
        adc_oneshot_read(s_adc, CH[t], &raw);
        mv = raw * 3300 / 4095;
        if (s_cali) adc_cali_raw_to_voltage(s_cali, raw, &mv);
        sum += mv;
        vTaskDelay(1);
    }
    int mv = sum / 16, lvl = 0;
    const int th[3] = { CONFIG_VD_TANK_ADC_T1, CONFIG_VD_TANK_ADC_T2, CONFIG_VD_TANK_ADC_T3 };
    for (int i = 0; i < 3; i++) {
#ifdef CONFIG_VD_TANK_ADC_INVERT
        if (mv <= th[i]) lvl = i + 1;
#else
        if (mv >= th[i]) lvl = i + 1;
#endif
    }
    ESP_LOGI(TAG, "tank %d: %d mV -> level %d", t, mv, lvl);
    return lvl;
#else
#if CONFIG_VD_SIMULATE
    return (int8_t)(esp_random() % 4);
#else
    return -1;
#endif
#endif
}

static void task(void *arg)
{
    int t;
    for (;;) {
        if (!xQueueReceive(s_q, &t, portMAX_DELAY)) continue;
        if (CONFIG_VD_TANK_PWR_GPIO >= 0) gpio_set_level(CONFIG_VD_TANK_PWR_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));            // let the probes settle (also matches the UI's "Reading…")
        int8_t lvl = measure(t);
        if (CONFIG_VD_TANK_PWR_GPIO >= 0) gpio_set_level(CONFIG_VD_TANK_PWR_GPIO, 0);
        model_lock();
        M.tank[t].level = lvl;
        M.tank[t].reading = false;
        M.tank[t].read_at_us = now_us();
        model_commit();
    }
}

void tanks_init(void)
{
#if CONFIG_VD_TANK_PWR_GPIO >= 0
    {
        gpio_config_t io = { .pin_bit_mask = 1ULL << CONFIG_VD_TANK_PWR_GPIO, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level(CONFIG_VD_TANK_PWR_GPIO, 0);
    }
#endif
#if CONFIG_VD_TANK_MODE_DIGITAL
    for (int t = 0; t < 2; t++) for (int i = 0; i < 3; i++) {
        if (PINS[t][i] < 0) continue;
        gpio_config_t io = { .pin_bit_mask = 1ULL << PINS[t][i], .mode = GPIO_MODE_INPUT,
#ifdef CONFIG_VD_TANK_ACTIVE_LOW
                             .pull_up_en = GPIO_PULLUP_ENABLE,
#else
                             .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        };
        gpio_config(&io);
    }
#elif CONFIG_VD_TANK_MODE_ADC
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&uc, &s_adc);
    adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    for (int t = 0; t < 2; t++) if (CH[t] >= 0) adc_oneshot_config_channel(s_adc, CH[t], &cc);
    adc_cali_curve_fitting_config_t cf = { .unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cf, &s_cali) != ESP_OK) s_cali = NULL;
#endif
    s_q = xQueueCreate(4, sizeof(int));
    xTaskCreate(task, "tanks", 3072, NULL, 3, NULL);
}

void tanks_read(int t)
{
    if (t < 0 || t > 1) return;
    model_lock();
    if (M.tank[t].reading) { model_unlock(); return; }
    M.tank[t].reading = true;
    model_commit();
    xQueueSend(s_q, &t, 0);
}

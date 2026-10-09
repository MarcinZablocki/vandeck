#include "solar_log.h"
#include "model.h"
#include "net/time_svc.h"
#include <string.h>
#include <time.h>
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOG_DAYS  90
#define LOG_MAGIC 0x534C4731

typedef struct { uint32_t magic; uint16_t head, count; vd_day_t d[LOG_DAYS]; } log_t;   // head = newest
static log_t s_log;
static SemaphoreHandle_t s_mx;
static esp_timer_handle_t s_t;
static int s_ticks;
static int32_t s_last_counter = -1;

int32_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

void civil_from_days(int32_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)yoe + era * 400 + (*m <= 2);
}

int32_t solar_log_today(void)
{
    if (!time_valid()) return -1;
    time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
    return days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("vandeck", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "slog", &s_log, sizeof s_log);
    nvs_commit(h);
    nvs_close(h);
}

static void minute(void *arg)
{
    int32_t today = solar_log_today();
    if (today < 0) return;
    vd_solar_t s;
    model_lock(); s = M.solar; model_unlock();
    bool fresh = s.valid && now_us() - s.ts_us < 120LL * 1000000;

    xSemaphoreTake(s_mx, portMAX_DELAY);
    vd_day_t *r = s_log.count ? &s_log.d[s_log.head] : NULL;
    bool booted_today = false;
    if (!r || r->day != (uint32_t)today) {
        if (r) save();
        s_log.head = (s_log.head + 1) % LOG_DAYS;
        if (s_log.count < LOG_DAYS) s_log.count++;
        r = &s_log.d[s_log.head];
        memset(r, 0, sizeof *r);
        r->day = today;
    } else if (s_last_counter < 0) booted_today = true;

    if (fresh) {
        // Accumulate deltas of the controller's day counter: it resets after a dark period,
        // not at local midnight, so copying it directly would put energy on the wrong day.
        if (s_last_counter < 0) {
            time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
            if ((booted_today || r->yield_wh == 0) && lt.tm_hour >= 4 && s.yield_wh > r->yield_wh) r->yield_wh = s.yield_wh;
        } else if (s.yield_wh >= s_last_counter) r->yield_wh += s.yield_wh - s_last_counter;
        else r->yield_wh += s.yield_wh;
        s_last_counter = s.yield_wh;
        if (s.pv_w > r->max_w) r->max_w = (uint16_t)s.pv_w;
        if (!isnan(s.pv_v) && s.pv_v * 10 > r->max_v_x10) r->max_v_x10 = (uint16_t)(s.pv_v * 10);
        if (s.stage == STAGE_BULK) r->min_bulk++;
        else if (s.stage == STAGE_ABS) r->min_abs++;
        else if (s.stage == STAGE_FLOAT) r->min_float++;
    }
    if (++s_ticks % 15 == 0) save();
    xSemaphoreGive(s_mx);
    model_lock(); model_commit();
}

void solar_log_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    nvs_handle_t h;
    size_t sz = sizeof s_log;
    if (nvs_open("vandeck", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "slog", &s_log, &sz) != ESP_OK || sz != sizeof s_log || s_log.magic != LOG_MAGIC)
            memset(&s_log, 0, sizeof s_log);
        nvs_close(h);
    }
    s_log.magic = LOG_MAGIC;
    esp_timer_create_args_t a = { .callback = minute, .name = "slog" };
    esp_timer_create(&a, &s_t);
    esp_timer_start_periodic(s_t, 60LL * 1000000);
}

bool solar_log_get(int32_t day, vd_day_t *out)
{
    bool ok = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < s_log.count; i++) {
        const vd_day_t *r = &s_log.d[(s_log.head + LOG_DAYS - i) % LOG_DAYS];
        if ((int32_t)r->day == day) { *out = *r; ok = true; break; }
        if ((int32_t)r->day < day) break;
    }
    xSemaphoreGive(s_mx);
    return ok;
}

int32_t solar_log_first_day(void)
{
    int32_t d = -1;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_log.count) d = s_log.d[(s_log.head + LOG_DAYS - (s_log.count - 1)) % LOG_DAYS].day;
    xSemaphoreGive(s_mx);
    return d;
}

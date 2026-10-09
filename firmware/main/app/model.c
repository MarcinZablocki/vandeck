#include "model.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

vd_model_t M;
static SemaphoreHandle_t s_mx;

void model_init(void)
{
    s_mx = xSemaphoreCreateRecursiveMutex();
    memset(&M, 0, sizeof M);
    M.tank[0].level = M.tank[1].level = -1;
    M.solar.pv_v = NAN;
    M.solar.load_a = NAN;
    for (int i = 0; i < SL_NHIST; i++) M.sl.v[i] = NAN;
}
void model_lock(void)   { xSemaphoreTakeRecursive(s_mx, portMAX_DELAY); }
void model_unlock(void) { xSemaphoreGiveRecursive(s_mx); }
void model_commit(void) { M.version++; xSemaphoreGiveRecursive(s_mx); }
void model_snapshot(vd_model_t *out) { model_lock(); memcpy(out, &M, sizeof M); model_unlock(); }
int64_t now_us(void) { return esp_timer_get_time(); }

vd_devtype_t role_type(vd_role_t r)
{
    return r == ROLE_SOLAR ? DEVT_VICTRON : r == ROLE_SHELLY ? DEVT_SHELLY : DEVT_LITIME;
}
const char *role_name(vd_role_t r)
{
    static const char *n[] = { "SOLAR CHARGER", "BATTERY 1", "BATTERY 2", "STARLINK POWER" };
    return n[r];
}

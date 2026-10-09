#include "victron.h"
#include "app/model.h"
#include "app/settings.h"
#include "psa/crypto.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "victron";

static bool aes_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len)
{
    static bool init;
    if (!init) { if (psa_crypto_init() != PSA_SUCCESS) return false; init = true; }
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 128);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_CTR);
    psa_key_id_t k;
    if (psa_import_key(&a, key, 16, &k) != PSA_SUCCESS) return false;
    psa_cipher_operation_t op = PSA_CIPHER_OPERATION_INIT;
    size_t o1 = 0, o2 = 0;
    bool ok = psa_cipher_decrypt_setup(&op, k, PSA_ALG_CTR) == PSA_SUCCESS &&
              psa_cipher_set_iv(&op, iv, 16) == PSA_SUCCESS &&
              psa_cipher_update(&op, in, len, out, len, &o1) == PSA_SUCCESS &&
              psa_cipher_finish(&op, out + o1, len - o1, &o2) == PSA_SUCCESS;
    psa_cipher_abort(&op);
    psa_destroy_key(k);
    return ok;
}

static vd_stage_t map_state(uint8_t s)
{
    switch (s) {
    case 0: return STAGE_OFF;
    case 3: return STAGE_BULK;
    case 4: return STAGE_ABS;
    case 5: return STAGE_FLOAT;
    default: return STAGE_OTHER;
    }
}

void victron_on_adv(const uint8_t *m, int len)
{
    // Manufacturer data incl. company ID, as captured from a real charger (ESP32-Truma victronble skill):
    // [0..1] 0x02E1  [2] 0x10 instant readout  [3] prefix  [4..5] model  [6] readout type  [7..8] IV  [9] key[0]  [10..] encrypted
    if (len < 20 || m[2] != 0x10 || m[6] != 0x01) return;       // 0x01 = solar charger
    const uint8_t *key = g_cfg.dev[ROLE_SOLAR].key;
    if (m[9] != key[0]) {
        static int64_t last;
        if (now_us() - last > 60LL * 1000000) { ESP_LOGW(TAG, "encryption key doesn't match this device"); last = now_us(); }
        return;
    }
    uint8_t iv[16] = { m[7], m[8] }, pt[16];
    int n = len - 10; if (n > 16) n = 16;
    if (n < 10 || !aes_ctr(key, iv, m + 10, pt, n)) return;

    int16_t bv = (int16_t)(pt[2] | pt[3] << 8);
    int16_t ba = (int16_t)(pt[4] | pt[5] << 8);
    uint16_t y = pt[6] | pt[7] << 8;
    uint16_t pw = pt[8] | pt[9] << 8;
    uint16_t ld = n >= 12 ? (pt[10] | (pt[11] & 1) << 8) : 0x1FF;   // 9-bit load current, 0x1FF = no load output
    if (bv == 0x7FFF) return;   // charger hasn't populated its data yet
    model_lock();
    vd_solar_t *s = &M.solar;
    s->valid = true; s->ts_us = now_us();
    s->stage = map_state(pt[0]);
    s->mode = pt[0];
    s->err = pt[1] == 0xFF ? 0 : pt[1];
    s->load_a = ld == 0x1FF ? NAN : ld * 0.1f;
    s->batt_v = bv * 0.01f;
    s->batt_a = ba == 0x7FFF ? 0 : ba * 0.1f;
    s->yield_wh = y == 0xFFFF ? 0 : y * 10;
    s->pv_w = pw == 0xFFFF ? 0 : pw;
    s->pv_v = NAN;            // not part of instant readout — needs a GATT link (see README)
    M.dev_state[ROLE_SOLAR] = DEV_ON;
    model_commit();
}

// Truma Combi over LIN (TJA1020/1021 or MCP2003 on a UART).
//
// Default mode is iNet box emulation: the CP Plus stays the LIN master and keeps running the heater; this panel is
// a LIN slave (NAD 0x03) that reads the CP Plus status and hands it setting changes through the iNet-box transport
// protocol. Protocol from danielfett/inetbox.py (inetbox/lin.py + inetbox/inetbox.py), which works with an
// "inet ready" CP Plus. After first install, run "PR SET" on the CP Plus so it registers the iNet box.
//
// Sniff mode only logs frames. Master mode (replacing the CP Plus) is an unfinished skeleton.
#include "truma_lin.h"
#include "app/model.h"
#include "app/settings.h"
#include "net/time_svc.h"
#include <time.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "lin";
#define PORT CONFIG_VD_LIN_UART_NUM
static QueueHandle_t s_uq;
static volatile bool s_dirty;

static uint8_t lin_pid(uint8_t id)
{
    uint8_t p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 1;
    uint8_t p1 = ~((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5)) & 1;
    return (id & 0x3F) | (p0 << 6) | (p1 << 7);
}

// Classic checksum (enhanced when seeded with the protected ID): inverted 8-bit sum with carry.
static uint8_t lin_csum(uint8_t pid, const uint8_t *d, int n, bool enhanced)
{
    uint16_t s = enhanced ? pid : 0;
    for (int i = 0; i < n; i++) { s += d[i]; if (s > 0xFF) s -= 0xFF; }
    return ~s;
}

#if CONFIG_VD_LIN_MODE_INETBOX

#define NAD          0x03
#define PID_REQ      0x3C              // master request (transport layer)
#define PID_RESP     0x3D              // slave response (transport layer)
#define PID_POLL     0x18              // CP Plus asks the iNet box whether it has an update
static const uint8_t IDENT[4] = { 0x17, 0x46, 0x00, 0x1F };
static const uint8_t PREAMBLE[10] = { 0x00, 0x1E, 0x00, 0x00, 0x22, 0xFF, 0xFF, 0xFF, 0x54, 0x01 };
#define BUF_STATUS   0x33              // CP Plus -> us; we write it back with id - 1
#define BUF_COUNTER  0x0D
#define BUF_CLOCK    0x15              // CP Plus -> us (hh mm ss + display/mode bytes); we write it back with id - 1
#define CLOCK_LEN    10
#define STATUS_RW    12                // writable part of the 0x33 buffer
#define STATUS_LEN   20

// Transport-layer responses waiting for 0x3D headers.
static uint8_t s_q[8][8];
static int s_qh, s_qn;
// Multi-frame download/upload request being received.
static uint8_t s_rx_sid, s_rx[64];
static int s_rx_n, s_rx_want;
// Last 0x33 status buffer data from the CP Plus, and the command counter it expects.
static uint8_t s_status[STATUS_LEN];
static bool s_have_status;
static uint8_t s_counter;
// After an upload, ignore requested-value fields in incoming status until they match (or this time passes),
// so the UI doesn't snap back to the old values while the CP Plus is still applying ours.
static int64_t s_confirm_until;
static uint16_t s_sent_room, s_sent_water; static uint8_t s_sent_mode;
static int64_t s_last_frame, s_last_reg;
// The CP Plus's clock, and whether it needs setting from the panel's local time.
static uint8_t s_clock[CLOCK_LEN];
static bool s_have_clock, s_clock_dirty;
static int64_t s_clock_hold;               // after sending a time, don't judge the CP Plus clock again until this

static void q_push(const uint8_t f[8])
{
    if (s_qn == 8) return;
    memcpy(s_q[(s_qh + s_qn++) % 8], f, 8);
}

static void q_push1(uint8_t pci, uint8_t rsid, uint8_t b0)
{
    uint8_t f[8] = { NAD, pci, rsid, b0, 0xFF, 0xFF, 0xFF, 0xFF };
    q_push(f);
}

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static uint16_t temp_code(float c) { return c < 5 ? 0 : (uint16_t)lroundf((c + 273.0f) * 10); }
static float code_temp(uint16_t v) { return (v == 0 || v == 0xAAA || v == 0xAAAA) ? NAN : v / 10.0f - 273.0f; }
static uint16_t water_code(uint8_t w) { return w == WATER_ECO ? temp_code(40) : w == WATER_HOT ? temp_code(60) : 0; }

static void want(uint16_t *room, uint8_t *mode, uint16_t *water)
{
    model_lock();
    *room = g_cfg.heater_on ? temp_code(g_cfg.setpoint_c) : 0;
    // The iNet box can only set off / eco / boost; FAN_VENT has no equivalent here and is sent as eco.
    // Room heating (setpoint), hot water and the fan level are separate fields. The Combi needs a non-zero mode to run
    // at all, so water-only operation keeps the selected fan level and only a fully idle panel sends 0.
    *mode = !g_cfg.heater_on && g_cfg.water == WATER_OFF ? 0 : g_cfg.fan == FAN_HIGH ? 10 : 1;
    *water = water_code(g_cfg.water);
    model_unlock();
}

static void on_status(const uint8_t *buf, int n)
{
    if (n < 14 || memcmp(buf, PREAMBLE, sizeof PREAMBLE)) { ESP_LOGW(TAG, "status buffer without preamble"); return; }
    uint8_t id = buf[11], counter = buf[12];
    const uint8_t *d = buf + 14;
    if (id == BUF_COUNTER) { s_counter = counter; return; }
    if (id == BUF_CLOCK) {
        if (n < 14 + CLOCK_LEN) return;
        memcpy(s_clock, d, CLOCK_LEN);
        s_have_clock = true;
        if (time_valid() && now_us() > s_clock_hold) {
            time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
            int diff = (d[0] * 3600 + d[1] * 60 + d[2]) - (lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
            if (diff < 0) diff = -diff;
            if (diff > 43200) diff = 86400 - diff;
            if (diff > 90 && !s_clock_dirty) {
                s_clock_dirty = true;
                ESP_LOGI(TAG, "CP Plus clock %02u:%02u:%02u, panel %02d:%02d:%02d: will set it", d[0], d[1], d[2], lt.tm_hour, lt.tm_min, lt.tm_sec);
            }
        }
        return;
    }
    if (id != BUF_STATUS || n < 14 + STATUS_LEN) return;   // timer (0x3D) and clock (0x15) buffers aren't used
    memcpy(s_status, d, STATUS_LEN);
    s_have_status = true;

    uint16_t room = le16(d), water = le16(d + 6), err = d[18] * 100 + d[17];
    uint8_t mode = d[2], op = d[16];
    bool hold = now_us() < s_confirm_until && !(room == s_sent_room && mode == s_sent_mode && water == s_sent_water);
    if (!hold) s_confirm_until = 0;
    float rc = code_temp(le16(d + 14));

    model_lock();
    if (!hold && !s_dirty) {
        g_cfg.heater_on = room != 0 && mode != 0;
        if (room) g_cfg.setpoint_c = code_temp(room);
        if (mode) g_cfg.fan = mode == 10 ? FAN_HIGH : FAN_ECO;
        g_cfg.water = water == 0 ? WATER_OFF : water <= temp_code(40) ? WATER_ECO : WATER_HOT;   // boost (200) shows as hot
    }
    if (!isnan(rc)) M.heater.room_c = rc;
    M.heater.heating = op >= 4;                  // 0 off, 1 warning, 4 starting, 5 on
    M.heater.err = err;
    M.heater.warn = op == 1;
    model_commit();
    ESP_LOGI(TAG, "status: room %.1f set %.1f mode %u water %u op %u err %u", rc, code_temp(room), mode, water, op, err);
}

// Queue a 7-frame upload of one buffer: preamble + [len, id_write, counter, checksum] + data, padded to 38 bytes.
static void send_buffer(uint8_t id_write, const uint8_t *data, int len)
{
    s_counter = (s_counter + 1) % 0xFF;
    uint8_t ck[5 + STATUS_RW] = { PREAMBLE[8], PREAMBLE[9], (uint8_t)len, id_write, s_counter };
    memcpy(ck + 5, data, len);
    uint8_t buf[38] = { 0 };
    memcpy(buf, PREAMBLE, sizeof PREAMBLE);
    buf[10] = len; buf[11] = id_write; buf[12] = s_counter; buf[13] = lin_csum(0, ck, 5 + len, false);
    memcpy(buf + 14, data, len);

    uint8_t f[8] = { NAD, 0x10, 0x29, 0xFA, 0x00, 0x1F, buf[0], buf[1] };
    q_push(f);
    for (int i = 0; i < 6; i++) {
        f[1] = 0x21 + i;
        memcpy(f + 2, buf + 2 + i * 6, 6);
        q_push(f);
    }
}

// Build the 0x32 buffer (the CP Plus's 0x33 data with our wanted values) and queue it as a 7-frame upload.
static void upload_status(void)
{
    s_dirty = false;
    uint8_t d[STATUS_RW];
    memcpy(d, s_status, STATUS_RW);
    uint16_t room, water; uint8_t mode;
    want(&room, &mode, &water);
    put16(d, room); d[2] = mode; put16(d + 6, water);
    d[4] = d[8]; d[5] = d[9]; d[10] = d[11];     // the buffer repeats el_power_level and energy_mix; keep them equal

    send_buffer(BUF_STATUS - 1, d, STATUS_RW);
    s_sent_room = room; s_sent_mode = mode; s_sent_water = water;
    s_confirm_until = now_us() + 45LL * 1000000;   // the CP Plus only listens during its ~5 s burst every ~17 s
    ESP_LOGI(TAG, "uploading: room %u mode %u water %u", room, mode, water);
}

// Same for the clock (buffer 0x14): the CP Plus's own bytes with hh:mm:ss replaced by the panel's local time.
static void upload_clock(void)
{
    s_clock_dirty = false;
    if (!time_valid()) return;
    time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt);
    uint8_t d[CLOCK_LEN];
    memcpy(d, s_clock, CLOCK_LEN);
    d[0] = lt.tm_hour; d[1] = lt.tm_min; d[2] = lt.tm_sec;
    send_buffer(BUF_CLOCK - 1, d, CLOCK_LEN);
    s_clock_hold = now_us() + 180LL * 1000000;
    ESP_LOGI(TAG, "uploading clock %02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
}

static void upload(void)
{
    if (s_have_status && s_dirty) upload_status();
    else if (s_have_clock && s_clock_dirty) upload_clock();
}

static void on_request(const uint8_t *d)
{
    uint8_t nad = d[0], type = d[1] >> 4;
    if (type == 0 && d[2] == 0xB2 && !memcmp(d + 4, IDENT, 4)) {             // read by identifier
        uint8_t f[8] = { NAD, 0x06, 0xF2, IDENT[0], IDENT[1], IDENT[2], IDENT[3], 0x00 };
        q_push(f);
        return;
    }
    if (nad != NAD && nad != 0x7F) return;
    if (type == 0 && d[2] == 0xB9 && d[3] == 0x00 && d[4] == 0x1F) {          // heartbeat
        q_push1(0x02, 0xF9, 0x00);
        s_last_reg = now_us();
    } else if (type == 0 && d[2] == 0xB0 && !memcmp(d + 3, IDENT, 4)) {      // assign NAD (CP Plus "PR SET")
        if (d[7] != NAD) { ESP_LOGW(TAG, "CP Plus assigned NAD 0x%02X; only 0x03 is supported", d[7]); return; }
        q_push1(0x01, 0xF0, 0xFF);
        s_last_reg = now_us();
        ESP_LOGI(TAG, "registered with the CP Plus");
    } else if (type == 1 && (d[3] == 0xBA || d[3] == 0xBB) && d[4] == 0x00 && d[5] == 0x1F) {   // first frame
        s_rx_sid = d[3];
        s_rx_want = (((d[1] & 0x0F) << 8) | d[2]) - 3;                        // minus SID and the 00 1F prefix
        s_rx[0] = d[6]; s_rx[1] = d[7]; s_rx_n = 2;
        if (s_rx_want > (int)sizeof s_rx) s_rx_want = 0;
    } else if (type == 2 && s_rx_want > 0) {                                   // consecutive frame
        int k = s_rx_want - s_rx_n < 6 ? s_rx_want - s_rx_n : 6;
        memcpy(s_rx + s_rx_n, d + 2, k);
        s_rx_n += k;
        if (s_rx_n < s_rx_want) return;
        s_rx_want = 0;
        s_last_reg = now_us();
        if (s_rx_sid == 0xBB) { q_push1(0x01, 0xFB, 0xFF); on_status(s_rx, s_rx_n); }
        else upload();                                                          // 0xBA: CP Plus asks for our data
    }
}

// Frames other nodes publish: live readings even before the CP Plus has registered us.
static void on_frame(uint8_t id, const uint8_t *d)
{
    if (id == 0x21) {
        float rc = code_temp(d[0] | (d[1] & 0x0F) << 8);
        if (!isnan(rc)) { model_lock(); M.heater.room_c = rc; model_commit(); }
    }
}

static void answer(uint8_t id, uint8_t raw_pid)
{
    uint8_t f[9];
    if (id == PID_POLL) {
        // FF = we have an update ready (the CP Plus then requests it with 0xBA), FE = nothing.
        memset(f, 0xFF, 8);
        f[0] = (s_dirty && s_have_status) || (s_clock_dirty && s_have_clock) ? 0xFF : 0xFE;
        f[8] = lin_csum(raw_pid, f, 8, true);
    } else if (id == PID_RESP && s_qn) {
        memcpy(f, s_q[s_qh], 8);
        s_qh = (s_qh + 1) % 8; s_qn--;
        f[8] = lin_csum(0, f, 8, false);
    } else return;
    uart_write_bytes(PORT, f, 9);
}


static void update_link(void)
{
    int64_t t = now_us();
    bool link = s_last_frame && t - s_last_frame < 30LL * 1000000;   // the CP Plus bus idles ~12 s out of every ~17 s
    bool reg = s_last_reg && t - s_last_reg < 120LL * 1000000;
    model_lock();
    if (M.heater.link != link || M.heater.registered != reg) {
        M.heater.link = link; M.heater.registered = reg;
        model_commit();
    } else model_unlock();
}

// Frames are delimited by the master's break. As soon as a header (0x55, PID) is in, answer it if it's ours;
// the complete frame (with whoever answered, us included via the transceiver echo) is decoded at the next break.
static void task(void *arg)
{
    uint8_t fr[16]; int fl = 0; bool answered = false;
    uart_event_t ev;
    for (;;) {
        if (xQueueReceive(s_uq, &ev, pdMS_TO_TICKS(1000))) {
            if (ev.type == UART_BREAK) {
                if (fl >= 11 && fr[0] == 0x55 && lin_pid(fr[1] & 0x3F) == fr[1]) {
                    uint8_t id = fr[1] & 0x3F;
                    bool classic = id >= 0x3C;
                    if (fr[10] == lin_csum(fr[1], fr + 2, 8, !classic)) {
                        s_last_frame = now_us();
                        if (id == PID_REQ) on_request(fr + 2);
                        else on_frame(id, fr + 2);
                    }
                }
                fl = 0; answered = false;
            } else if (ev.type == UART_DATA) {
                uint8_t b[32];
                int n = uart_read_bytes(PORT, b, ev.size > sizeof b ? sizeof b : ev.size, 0);
                for (int i = 0; i < n; i++) {
                    if (fl == 0 && b[i] != 0x55) continue;   // skip the break byte(s) before the sync
                    if (fl < (int)sizeof fr) fr[fl++] = b[i];
                }
                if (fl >= 2 && !answered && lin_pid(fr[1] & 0x3F) == fr[1]) { answered = true; answer(fr[1] & 0x3F, fr[1]); }
            } else if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
                uart_flush_input(PORT); xQueueReset(s_uq); fl = 0;
            }
        }
        update_link();
    }
}

#elif CONFIG_VD_LIN_MODE_MASTER   // unfinished. Never run it with the CP Plus still on the bus.

static void lin_break(void)
{
    // 0x00 at half baud = 18 nominal bit times low (LIN needs >= 13), stop bit = 2-bit delimiter.
    uart_wait_tx_done(PORT, pdMS_TO_TICKS(20));
    uart_set_baudrate(PORT, CONFIG_VD_LIN_BAUD / 2);
    uint8_t z = 0;
    uart_write_bytes(PORT, &z, 1);
    uart_wait_tx_done(PORT, pdMS_TO_TICKS(20));
    uart_set_baudrate(PORT, CONFIG_VD_LIN_BAUD);
}

// Master requests a slave response. Returns data length or -1.
static int lin_request(uint8_t id, uint8_t *out, int n)
{
    uint8_t pid = lin_pid(id), hdr[2] = { 0x55, pid };
    uart_flush_input(PORT);
    lin_break();
    uart_write_bytes(PORT, hdr, 2);
    uint8_t rx[12];
    int need = 1 + 2 + n + 1;          // echo of break byte + header, then data + checksum
    int got = uart_read_bytes(PORT, rx, need, pdMS_TO_TICKS(30));
    if (got != need) return -1;
    memcpy(out, rx + 3, n);
    bool enh = id < 0x3C;
    return rx[3 + n] == lin_csum(pid, out, n, enh) ? n : -1;
}

// Master publishes a frame.
static void lin_publish(uint8_t id, const uint8_t *d, int n)
{
    uint8_t pid = lin_pid(id), f[12] = { 0x55, pid };
    memcpy(f + 2, d, n);
    f[2 + n] = lin_csum(pid, d, n, id < 0x3C);
    lin_break();
    uart_write_bytes(PORT, f, 3 + n);
    uart_wait_tx_done(PORT, pdMS_TO_TICKS(20));
    uart_flush_input(PORT);           // drop our own echo
}

static void master_cycle(void)
{
    if (s_dirty) {
        s_dirty = false;
        // TODO: encode g_cfg.heater_on / setpoint_c / water / fan into the CP Plus -> Combi frame(s),
        // e.g. lin_publish(0x??, payload, 8). ~/Projects/ESP32-Truma's truma-protocol skill has a Combi D frame map.
        (void)lin_publish;
    }
    uint8_t d[8];
    // TODO: poll the Combi status frame(s); set M.heater.link / heating / room_c / err from them.
    int n = lin_request(0x00 /* TODO */, d, 8);
    model_lock();
    M.heater.link = n == 8;
    model_commit();
}

static void task(void *arg)
{
    for (;;) { master_cycle(); vTaskDelay(pdMS_TO_TICKS(100)); }
}

#else   // sniff (also the fallback, so an unset choice can never transmit)

// Passive: log each frame (bytes between breaks) as hex.
static void task(void *arg)
{
    uint8_t frame[16]; int fl = 0;
    uart_event_t ev;
    for (;;) {
        if (!xQueueReceive(s_uq, &ev, portMAX_DELAY)) continue;
        if (ev.type == UART_BREAK) {
            if (fl > 2) {
                char hex[16 * 3 + 1] = { 0 };
                for (int i = 0; i < fl; i++) sprintf(hex + i * 3, "%02X ", frame[i]);
                ESP_LOGI(TAG, "id=0x%02X %s", frame[1] & 0x3F, hex);
                model_lock(); M.heater.link = true; model_commit();
            }
            fl = 0;
        } else if (ev.type == UART_DATA) {
            uint8_t b[32];
            int n = uart_read_bytes(PORT, b, ev.size > sizeof b ? sizeof b : ev.size, 0);
            for (int i = 0; i < n; i++) {
                if (fl == 0 && b[i] == 0x00) continue;   // the break itself reads as 0x00
                if (fl < (int)sizeof frame) frame[fl++] = b[i];
            }
        } else if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
            uart_flush_input(PORT); xQueueReset(s_uq); fl = 0;
        }
    }
}

#endif

void truma_init(void)
{
    if (CONFIG_VD_LIN_TX_GPIO < 0 || CONFIG_VD_LIN_RX_GPIO < 0) return;
#if CONFIG_VD_LIN_EN_GPIO >= 0
    {
        gpio_config_t io = { .pin_bit_mask = 1ULL << CONFIG_VD_LIN_EN_GPIO, .mode = GPIO_MODE_OUTPUT };
        gpio_config(&io);
        gpio_set_level(CONFIG_VD_LIN_EN_GPIO, 1);
    }
#endif
    uart_config_t u = { .baud_rate = CONFIG_VD_LIN_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT };
    uart_driver_install(PORT, 512, 256, 20, &s_uq, 0);
    uart_param_config(PORT, &u);
    uart_set_pin(PORT, CONFIG_VD_LIN_TX_GPIO, CONFIG_VD_LIN_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
#if CONFIG_VD_LIN_MODE_INETBOX
    // A slave must start its response right after the header, so hand over every byte immediately instead of
    // waiting for the FIFO threshold or RX timeout, and run above the UI and BLE tasks.
    uart_set_rx_full_threshold(PORT, 1);
    xTaskCreate(task, "lin", 4096, NULL, configMAX_PRIORITIES - 2, NULL);
#else
    xTaskCreate(task, "lin", 4096, NULL, 5, NULL);
#endif
}

static void changed(void) { s_dirty = true; model_commit(); settings_save_later(); }
void truma_set_power(bool on)    { model_lock(); g_cfg.heater_on = on; changed(); }
void truma_set_setpoint(float c) { if (c < 5) c = 5; if (c > 30) c = 30; model_lock(); g_cfg.setpoint_c = c; changed(); }
void truma_set_water(uint8_t m)  { model_lock(); g_cfg.water = m; changed(); }
void truma_set_fan(uint8_t m)    { model_lock(); g_cfg.fan = m; changed(); }

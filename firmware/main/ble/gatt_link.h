#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "host/ble_hs.h"

// Minimal central-side GATT link: connect, find up to 4 characteristics by UUID,
// optionally subscribe to one of them, then read/write by index.
#define GL_MAX_CHR 4

typedef struct gatt_link gatt_link_t;
typedef void (*gl_evt_cb)(gatt_link_t *l);
typedef void (*gl_data_cb)(gatt_link_t *l, int chr, const uint8_t *data, uint16_t len);

struct gatt_link {
    ble_addr_t addr;
    uint16_t conn;
    bool up, connecting;
    int64_t connect_at_us;   // when the pending connect started (see gl_watchdog)
    uint16_t connect_ms;     // connect window (0 = 3000)
    uint16_t slow_itvl;      // hold a standing link at this interval (x 1.25 ms); 0 = leave as negotiated
    ble_uuid_any_t uuid[GL_MAX_CHR];
    uint16_t h[GL_MAX_CHR];
    uint8_t n_chr;
    int8_t notify_chr;                  // -1 = none
    gl_evt_cb on_ready, on_down;
    gl_data_cb on_notify;
    gl_data_cb read_cb; int read_chr;   // internal
    gl_evt_cb write_cb;                 // internal
    void *user;
};

void gl_init(gatt_link_t *l);
int gl_connect(gatt_link_t *l);
void gl_disconnect(gatt_link_t *l);
int gl_write(gatt_link_t *l, int chr, const void *data, uint16_t len, gl_evt_cb done);   // done = NULL: write without response
int gl_read(gatt_link_t *l, int chr, gl_data_cb cb);
uint16_t gl_mtu(gatt_link_t *l);
void gl_watchdog(void);   // cancels connect attempts that never resolved (call periodically)

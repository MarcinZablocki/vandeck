#pragma once
#include "sdkconfig.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPS_DIR       "/root/gps"            // on BreezyBox's LittleFS ('storage' partition)
#define GPS_TRACK_DIR GPS_DIR "/t"           // one CSV per UTC day: YYYYMMDD.csv

void gps_init(void);

// For the 'gps' command and Settings > About: what isn't in M.gps.
typedef struct {
    bool enabled, tx, fs;                 // built with VD_GPS; module RX wired; storage mounted
    int rx_gpio, tx_gpio, baud;
    char sw[31], hw[11];                  // module version (UBX-MON-VER); "" = no answer (yet)
    uint32_t bytes, sentences, bad;       // received bytes; NMEA sentences with a good / bad checksum
    char aid[160];                        // what the module was given at start-up
    int64_t aid_saved;                    // unix time of the saved aiding data, 0 = none (saved only with a fix)
    int aid_frames, aop_frames;           // frames in the saved file; of those, AssistNow Autonomous (AID-AOP)
    int8_t aop;                           // AssistNow Autonomous on the module: 1 on, 0 refused, -1 not set (yet)
    int8_t aop_sats;                      // satellites the module has AOP orbits for (NAV-AOPSTATUS), -1 = not known
    int64_t boot_unix;                    // unix time of this start-up, 0 = clock not set yet
    int64_t fix_unix;                     // unix time of the last fix since start-up, 0 = none
    bool online;                          // built with AssistNow Online (VD_GPS_ASSISTNOW)
    bool online_token;                    // ... and a token is set
    char online_st[48];                   // its last result
    uint32_t stack_free;                  // task stack high-water mark, bytes
} gps_info_t;
void gps_info(gps_info_t *out);

// Why the saved aiding data is as old as it is, for 'gps' and Settings: "saved 10 h 30 min ago, before this
// start-up" (panel off or restarted) or "... no fix since". "" when nothing is saved.
void gps_aid_note(const gps_info_t *i, char *buf, size_t n);
// "45 s", "12 min", "10 h 30 min", "2 d 4 h"
void gps_fmt_age(int64_t s, char *buf, size_t n);

#if CONFIG_VD_GPS_ASSISTNOW
// AssistNow Online token (NVS vd_gps; never printed). "" clears it. False if it doesn't look like a token.
bool gps_set_token(const char *token);
#endif

// Raw NMEA as it arrives, for ms milliseconds (blocks the caller). Returns the length copied into buf.
size_t gps_raw(char *buf, size_t cap, int ms);

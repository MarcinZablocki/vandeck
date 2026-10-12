#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GPS_DIR       "/root/gps"            // on BreezyBox's LittleFS ('storage' partition)
#define GPS_TRACK_DIR GPS_DIR "/t"           // one CSV per UTC day: YYYYMMDD.csv

void gps_init(void);

// For the 'gps' command: what isn't in M.gps.
typedef struct {
    bool enabled, tx, fs;                 // built with VD_GPS; module RX wired; storage mounted
    int rx_gpio, tx_gpio, baud;
    char sw[31], hw[11];                  // module version (UBX-MON-VER); "" = no answer (yet)
    uint32_t bytes, sentences, bad;       // received bytes; NMEA sentences with a good / bad checksum
    char aid[80];                         // what the module was given at start-up
    int64_t aid_saved;                    // unix time of the saved ephemeris/almanac, 0 = none
    int aid_frames;
    uint32_t stack_free;                  // task stack high-water mark, bytes
} gps_info_t;
void gps_info(gps_info_t *out);

// Raw NMEA as it arrives, for ms milliseconds (blocks the caller). Returns the length copied into buf.
size_t gps_raw(char *buf, size_t cap, int ms);

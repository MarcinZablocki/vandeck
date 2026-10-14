#pragma once

// Serial console over Wi-Fi: everything ESP_LOG prints (also still on UART0) is kept in a PSRAM ring buffer and
// streamed to a TCP client on port 23 (tools/console.sh). The first line the client sends must be the OTA key.
void log_svc_capture(void);   // start mirroring log output into the buffer (call as early as possible)
void log_svc_start(void);     // start the TCP console (after Wi-Fi/mDNS are up)

// The serial port (UART0 and USB-JTAG) shows the log live during start-up only, so the shell there is usable;
// 'log' prints the kept log, 'log live on' streams it again.
#include <stdbool.h>
void log_svc_boot_done(void);         // end of start-up: stop the live log on the serial port
void log_svc_set_live(bool on);
bool log_svc_live(void);
void log_svc_print(int lines);        // the kept log (last 32 KB) to stdout; lines > 0 = only the last ones

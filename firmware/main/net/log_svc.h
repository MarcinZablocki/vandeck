#pragma once

// Serial console over Wi-Fi: everything ESP_LOG prints (also still on UART0) is kept in a PSRAM ring buffer and
// streamed to a TCP client on port 23 (tools/console.sh). The first line the client sends must be the OTA key.
void log_svc_capture(void);   // start mirroring log output into the buffer (call as early as possible)
void log_svc_start(void);     // start the TCP console (after Wi-Fi/mDNS are up)

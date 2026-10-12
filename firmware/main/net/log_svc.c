// Network console: a log mirror plus a few commands, one client at a time on TCP port 23.
#include "log_svc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mdns.h"
#include "ble/ble_svc.h"
#include "app/cli.h"
#include "esp_console.h"
#include <stdint.h>
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PORT     23
#define RING_SZ  (32 * 1024)

// The ring lives in PSRAM that start-up doesn't clear, and PSRAM keeps its contents through software, panic and
// watchdog resets: after a crash the console still shows how the previous run ended. A power-on leaves garbage,
// which the magic check throws away. total = bytes ever written; position = total % RING_SZ.
#define RING_MAGIC 0x56444C47u
static EXT_RAM_NOINIT_ATTR struct { uint32_t magic; uint64_t total; char d[RING_SZ]; } s_keep;
static char *s_ring;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_fmt_lock;       // guards s_fmt (log calls can come from any task)
static char s_fmt[512];
static vprintf_like_t s_uart;              // the original log writer (UART0)
static TaskHandle_t s_task;

static void ring_put(const char *d, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) s_ring[(s_keep.total + i) % RING_SZ] = d[i];
    s_keep.total += n;
    portEXIT_CRITICAL(&s_mux);
}

// The panic report goes straight to UART0, which nobody watches on the van. Add its gist to the ring (kept
// through the reset) so the Wi-Fi console shows it after the restart: reason, abort details, PC and return address
// (riscv32-esp-elf-addr2line -e build/vandeck.elf <pc> <ra>). No locks or printf here: the other core may hold them.
static void panic_put(const char *s) { while (s && *s) s_ring[s_keep.total++ % RING_SZ] = *s++; }
static void panic_hex(uint32_t v)
{
    char b[11] = "0x";
    for (int i = 0; i < 8; i++) b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0;
    panic_put(b);
}

void __real_esp_panic_handler(panic_info_t *info);
void __wrap_esp_panic_handler(panic_info_t *info)
{
    if (s_ring) {
        panic_put("\r\n!!! panic: ");
        panic_put(g_panic_abort ? g_panic_abort_details : info->reason);
        if (info->description) { panic_put(" - "); panic_put(info->description); }
        panic_put("\r\n!!! pc ");
        panic_hex((uint32_t)info->addr);
        if (info->frame) { panic_put(" ra "); panic_hex(((const RvExcFrame *)info->frame)->ra); }
        panic_put("\r\n");
    }
    __real_esp_panic_handler(info);
}

static int log_hook(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int r = s_uart(fmt, args);
    // Never block a logging task: if another task is formatting right now, this line only goes to UART.
    if (xSemaphoreTake(s_fmt_lock, 0) == pdTRUE) {
        int n = vsnprintf(s_fmt, sizeof s_fmt, fmt, copy);
        if (n > 0) ring_put(s_fmt, n < (int)sizeof s_fmt ? n : sizeof s_fmt - 1);
        xSemaphoreGive(s_fmt_lock);
        if (s_task) xTaskNotifyGive(s_task);
    }
    va_end(copy);
    return r;
}

void log_svc_capture(void)
{
    if (s_ring) return;
    s_fmt_lock = xSemaphoreCreateMutex();
    if (!s_fmt_lock) return;
    s_ring = s_keep.d;
    if (s_keep.magic != RING_MAGIC || esp_reset_reason() == ESP_RST_POWERON) { s_keep.magic = RING_MAGIC; s_keep.total = 0; }
    else {
        static const char M[] = "\r\n===== restarted: the log above is from before the reset =====\r\n\r\n";
        ring_put(M, sizeof M - 1);
    }
    s_uart = esp_log_set_vprintf(log_hook);
}

static void say(int fd, const char *fmt, ...)
{
    char b[256];
    va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (n > 0) send(fd, b, n < (int)sizeof b ? n : sizeof b - 1, 0);
}

// Read one line (telnet negotiation bytes and CR dropped). Returns length, 0 = nothing yet, -1 = closed.
static int read_line(int fd, char *line, int cap, int *len)
{
    char c;
    int n = recv(fd, &c, 1, MSG_DONTWAIT);
    if (n == 0) return -1;
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    if ((uint8_t)c >= 0xF0 || c == '\r') return 0;   // IAC & co. from telnet clients
    if (c == '\n') { line[*len] = 0; int l = *len; *len = 0; return l > 0 ? l : 0; }
    if (*len < cap - 1) line[(*len)++] = c;
    return 0;
}

static const char *LVL[] = { "none", "error", "warn", "info", "debug", "verbose" };

// Shell commands (esp_console) print to stdout. stdout is per task in ESP-IDF, so point this task's stdout at the
// socket for the duration of the command.
static int sock_write(void *cookie, const char *b, int n)
{
    int fd = (int)(intptr_t)cookie;
    for (int i = 0, start = 0; i <= n; i++) {
        if (i == n || b[i] == '\n') {
            if (i > start) send(fd, b + start, i - start, 0);
            if (i < n) send(fd, "\r\n", 2, 0);
            start = i + 1;
        }
    }
    return n;
}

static void run_shell(int fd, const char *line)
{
    FILE *f = fwopen((void *)(intptr_t)fd, sock_write);
    if (!f) return;
    FILE *old = stdout;
    stdout = f;
    shell_exec(line);
    fflush(f);
    stdout = old;
    fclose(f);
}

static void command(int fd, char *cmd)
{
    char line[96];
    strlcpy(line, cmd, sizeof line);
    char *arg = strchr(cmd, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(cmd, "help")) {
        size_t n;
        const esp_console_cmd_t *c = cli_cmds(&n);
        for (size_t i = 0; i < n; i++) {
            if (arg && strcmp(arg, c[i].command)) continue;
            say(fd, "  %-9s %s\r\n", c[i].command, c[i].help);
        }
        if (!arg) say(fd, "  console:  version | heap | tasks | log <tag|*> <none|error|warn|info|debug|verbose> | reboot | quit\r\n"
                          "  'help <command>' for one command. The USB serial port has the full BreezyBox shell.\r\n");
    } else if (!strcmp(cmd, "version")) {
        const esp_app_desc_t *d = esp_app_get_description();
        static const char *RST[] = { "unknown", "power-on", "external", "software", "panic", "interrupt watchdog",
                                     "task watchdog", "other watchdog", "deep sleep", "brownout", "SDIO", "USB",
                                     "JTAG", "eFuse", "power glitch", "CPU lockup" };
        esp_reset_reason_t rr = esp_reset_reason();
        say(fd, "%s %s, built %s %s, up %llds, last reset: %s\r\n", d->project_name, d->version, d->date, d->time,
            esp_timer_get_time() / 1000000, rr < sizeof RST / sizeof *RST ? RST[rr] : "?");
    } else if (!strcmp(cmd, "heap")) {
        say(fd, "internal free %u (largest %u, min %u), DMA free %u, PSRAM free %u\r\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    } else if (!strcmp(cmd, "tasks")) {
        // CPU share per task since boot, plus each task's lowest free stack. Big numbers on anything but IDLE = a hog.
        char *b = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
        if (b) {
            vTaskGetRunTimeStats(b);
            say(fd, "task            run-time        %%\r\n");
            for (char *l = strtok(b, "\n"); l; l = strtok(NULL, "\n")) say(fd, "%s\r\n", l);
            free(b);
        }
    } else if (!strcmp(cmd, "log") && arg) {
        char *lv = strchr(arg, ' ');
        if (lv) *lv++ = 0;
        for (int i = 0; lv && i < 6; i++) if (!strcasecmp(lv, LVL[i])) {
            esp_log_level_set(arg, (esp_log_level_t)i);
            say(fd, "log level of '%s' set to %s\r\n", arg, LVL[i]);
            return;
        }
        say(fd, "usage: log <tag|*> <none|error|warn|info|debug|verbose>\r\n");
    } else if (!strcmp(cmd, "reboot")) {
        say(fd, "rebooting\r\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (cmd[0]) {
        run_shell(fd, line);
    }
}

static void serve(int fd)
{
    char line[96]; int len = 0, n;
    say(fd, "VanDeck console. Key: ");
    int64_t until = esp_timer_get_time() + 30LL * 1000000;
    while ((n = read_line(fd, line, sizeof line, &len)) == 0 && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(50));
    if (n <= 0 || strcmp(line, CONFIG_VD_OTA_KEY)) { say(fd, "\r\nwrong key\r\n"); return; }

    const esp_app_desc_t *d = esp_app_get_description();
    say(fd, "\r\n%s %s - recent log follows, then live output. 'help' for commands.\r\n\r\n", d->project_name, d->version);
    portENTER_CRITICAL(&s_mux);
    uint64_t pos = s_keep.total > RING_SZ ? s_keep.total - RING_SZ : 0;
    portEXIT_CRITICAL(&s_mux);

    static char out[1024];
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        uint64_t total = s_keep.total;
        bool dropped = total - pos > RING_SZ;
        if (dropped) pos = total - RING_SZ;
        size_t k = 0;
        while (pos < total && k < sizeof out) out[k++] = s_ring[pos++ % RING_SZ];
        portEXIT_CRITICAL(&s_mux);
        if (dropped) say(fd, "\r\n[... output dropped, client too slow ...]\r\n");
        if (k && send(fd, out, k, 0) < 0) return;
        if (pos < total) continue;

        while ((n = read_line(fd, line, sizeof line, &len)) > 0) {
            if (!strcmp(line, "quit") || !strcmp(line, "exit")) return;
            command(fd, line);
        }
        if (n < 0) return;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    }
}

static int open_listener(void)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) return -1;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) || listen(ls, 1)) { close(ls); return -1; }
    return ls;
}

static void task(void *a)
{
    int ls;
    while ((ls = open_listener()) < 0) {   // e.g. no free socket yet: keep trying rather than giving up
        ESP_LOGW("console", "can't listen on port %d (errno %d), retrying", PORT, errno);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGI("console", "listening on port %d", PORT);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
        struct timeval tv = { .tv_sec = 10 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);   // a stuck client can't block us forever
        serve(fd);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

void log_svc_start(void)
{
    if (!s_ring || s_task) return;
    mdns_service_add(NULL, "_telnet", "_tcp", PORT, NULL, 0);
    // Above the UI and most services, so the console stays usable for finding whatever is hogging the CPU.
    xTaskCreate(task, "console", 16384, NULL, 6, &s_task);   // runs BreezyBox scripting lines
    ESP_LOGI("console", "log console on port %d (tools/console.sh)", PORT);
}

// Network console: a log mirror plus a few commands, one client at a time on TCP port 23.
// Works with nc (tools/console.sh) and with telnet clients: telnet option requests are refused and skipped, Enter
// may come as CR, LF, CR LF or CR NUL, and output lines end in CR LF.
#include "log_svc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"
#include "hal/cache_hal.h"
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
static volatile bool s_live = true;        // the log also goes to UART0 (false after start-up, see 'log live')
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
        // PSRAM is behind the cache; the reset would drop these lines (and the last log lines) before they land.
        cache_hal_writeback_addr((uint32_t)&s_keep, sizeof s_keep);
    }
    __real_esp_panic_handler(info);
}

static int log_hook(const char *fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int r = s_live ? s_uart(fmt, args) : 0;
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

void log_svc_boot_done(void)
{
    if (!s_ring || !s_live) return;
    printf("\n--- start-up done: the log stops here on the serial port. 'log' shows it (also 'log 50'), 'log live on'\n"
           "--- streams it again, 'help' lists the commands. Press Enter for a prompt.\n");
    fflush(stdout);
    s_live = false;
}

void log_svc_set_live(bool on) { if (s_ring) s_live = on; }
bool log_svc_live(void) { return s_live; }

void log_svc_print(int lines)
{
    if (!s_ring) { printf("no kept log\n"); return; }
    portENTER_CRITICAL(&s_mux);
    uint64_t total = s_keep.total;
    portEXIT_CRITICAL(&s_mux);
    uint64_t pos = total > RING_SZ ? total - RING_SZ : 0;
    // The last 'lines' lines start after the lines-th newline from the end (the final one doesn't count).
    for (uint64_t p = total, n = 0; lines > 0 && p > pos; p--)
        if (p != total && s_ring[(p - 1) % RING_SZ] == '\n' && ++n == (uint64_t)lines) { pos = p; break; }
    char b[512];
    while (pos < total) {
        portENTER_CRITICAL(&s_mux);
        if (s_keep.total - pos > RING_SZ) pos = s_keep.total - RING_SZ;   // overwritten meanwhile
        size_t k = 0;
        while (pos < total && k < sizeof b) b[k++] = s_ring[pos++ % RING_SZ];
        portEXIT_CRITICAL(&s_mux);
        fwrite(b, 1, k, stdout);
    }
    fflush(stdout);
}

static void say(int fd, const char *fmt, ...)
{
    char b[256];
    va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (n > 0) send(fd, b, n < (int)sizeof b ? n : sizeof b - 1, 0);
}

// One client: its socket, the line being typed and the telnet parser state.
#define IAC 0xFF
enum { T_DATA, T_IAC, T_OPT, T_SB, T_SB_IAC };
typedef struct { int fd; char line[96]; int len; uint8_t t, verb; bool cr; } conn_t;

// Telnet: refuse whatever the client offers (WILL -> DONT) or asks for (DO -> WONT), once per request; a refusal
// needs no answer. Sub-negotiations (IAC SB ... IAC SE) are skipped. Returns true for a byte that is data.
static bool telnet(conn_t *c, uint8_t b)
{
    switch (c->t) {
    case T_IAC:
        if (b == IAC) { c->t = T_DATA; return true; }               // IAC IAC = a 0xFF data byte
        if (b >= 0xFB) { c->verb = b; c->t = T_OPT; }               // WILL, WONT, DO, DONT + option
        else c->t = b == 0xFA ? T_SB : T_DATA;                      // SB, or a two-byte command (NOP, IP, ...)
        return false;
    case T_OPT:
        if (c->verb == 0xFB || c->verb == 0xFD) {
            uint8_t r[3] = { IAC, c->verb == 0xFB ? 0xFE : 0xFC, b };
            send(c->fd, r, 3, 0);
        }
        c->t = T_DATA;
        return false;
    case T_SB: if (b == IAC) c->t = T_SB_IAC; return false;
    case T_SB_IAC: c->t = b == 0xF0 ? T_DATA : T_SB; return false; // SE ends it
    default:
        if (b == IAC) { c->t = T_IAC; return false; }
        return true;
    }
}

// Read what has arrived. Returns the length of a finished line (in c->line), 0 = nothing yet, -1 = closed.
static int read_line(conn_t *c)
{
    uint8_t b;
    int n;
    while ((n = recv(c->fd, &b, 1, MSG_DONTWAIT)) == 1) {
        if (!telnet(c, b)) continue;
        bool after_cr = c->cr;
        c->cr = b == '\r';
        if (b == '\r' || (b == '\n' && !after_cr)) { c->line[c->len] = 0; int l = c->len; c->len = 0; if (l) return l; continue; }
        if (b == '\n' || b == 0) continue;                          // the LF or NUL after a CR
        if (b == 0x04 && !c->len) return -1;                        // Ctrl-D on an empty line
        if (b == 0x08 || b == 0x7F) { if (c->len) c->len--; continue; }
        if (b >= ' ' && c->len < (int)sizeof c->line - 1) c->line[c->len++] = b;
    }
    if (n == 0) return -1;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

// The log has bare LF line ends; a network terminal wants CR LF.
static int send_crlf(int fd, const char *b, size_t n, char *prev)
{
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (b[i] == '\n' && (i ? b[i - 1] : *prev) != '\r') {
            if ((i > start && send(fd, b + start, i - start, 0) < 0) || send(fd, "\r", 1, 0) < 0) return -1;
            start = i;
        }
    }
    if (n) *prev = b[n - 1];
    return n > start && send(fd, b + start, n - start, 0) < 0 ? -1 : 0;
}

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
        if (!arg) say(fd, "  console:  version | heap | tasks | reboot | quit\r\n"
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
    static conn_t c;
    c = (conn_t){ .fd = fd };
    int n;
    say(fd, "VanDeck console. Key: ");
    int64_t until = esp_timer_get_time() + 30LL * 1000000;
    while ((n = read_line(&c)) == 0 && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(50));
    if (n <= 0 || strcmp(c.line, CONFIG_VD_OTA_KEY)) { say(fd, "\r\nwrong key\r\n"); return; }

    const esp_app_desc_t *d = esp_app_get_description();
    say(fd, "\r\n%s %s - recent log follows, then live output. 'help' for commands.\r\n\r\n", d->project_name, d->version);
    portENTER_CRITICAL(&s_mux);
    uint64_t pos = s_keep.total > RING_SZ ? s_keep.total - RING_SZ : 0;
    portEXIT_CRITICAL(&s_mux);

    static char out[1024];
    char prev = '\n';
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        uint64_t total = s_keep.total;
        bool dropped = total - pos > RING_SZ;
        if (dropped) pos = total - RING_SZ;
        size_t k = 0;
        while (pos < total && k < sizeof out) out[k++] = s_ring[pos++ % RING_SZ];
        portEXIT_CRITICAL(&s_mux);
        if (dropped) say(fd, "\r\n[... output dropped, client too slow ...]\r\n");
        if (k && send_crlf(fd, out, k, &prev) < 0) return;
        if (pos < total) continue;

        while ((n = read_line(&c)) > 0) {
            if (!strcmp(c.line, "quit") || !strcmp(c.line, "exit")) return;
            command(fd, c.line);
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

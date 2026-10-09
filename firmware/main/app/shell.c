// Shell: BreezyBox (BusyBox-style: files, pipes, redirects, history) on the USB serial port, with the panel's own
// commands added. Without the 'storage' LittleFS partition BreezyBox can't start; then plain esp_console runs the
// same commands, and the Wi-Fi console still works.
#include "cli.h"
#include "breezybox.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include <stdio.h>

static bool s_breezy;

bool shell_start(void)
{
    // BreezyBox's line editor needs blocking reads from the console UART, i.e. the UART driver behind stdin.
    if (uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0) == ESP_OK) {
        uart_vfs_dev_use_driver(UART_NUM_0);
        uart_vfs_dev_port_set_rx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CR);
        uart_vfs_dev_port_set_tx_line_endings(UART_NUM_0, ESP_LINE_ENDINGS_CRLF);
    }
    size_t n;
    const esp_console_cmd_t *cmds = cli_cmds(&n);
    if (breezybox_start_stdio_ex(16384, 2, cmds, n) == ESP_OK) {   // 16 KB: scripting is stack-hungry
        s_breezy = true;
        ESP_LOGI("shell", "BreezyBox shell on the USB serial port");
        return true;
    }
    ESP_LOGW("shell", "no 'storage' partition: BreezyBox unavailable, plain console commands only");
    esp_console_config_t cc = ESP_CONSOLE_CONFIG_DEFAULT();
    cc.max_cmdline_length = 128;
    if (esp_console_init(&cc) == ESP_OK) {
        esp_console_register_help_command();
        cli_register();
    }
    return false;
}

int shell_exec(const char *line)
{
    if (s_breezy) return breezybox_exec_line(line);
    int ret = 0;
    esp_err_t e = esp_console_run(line, &ret);
    if (e == ESP_ERR_NOT_FOUND) { printf("unknown command (try help)\n"); return 127; }
    if (e != ESP_OK) { printf("error: %s\n", esp_err_to_name(e)); return 1; }
    return ret;
}

file(GLOB FONT_SRCS "fonts/*.c")
idf_component_register(
  SRCS "main.c"
       "board/board.c"
       "app/model.c" "app/settings.c" "app/solar_log.c" "app/sim.c" "app/cli.c" "app/shell.c"
       "net/wifi_svc.c" "net/time_svc.c" "net/starlink.c" "net/ota_svc.c" "net/log_svc.c" "net/tailnet_svc.c" "net/status_api.c" "net/weather.c" "net/update_svc.c"
       "ble/ble_svc.c" "ble/gatt_link.c" "ble/victron.c" "ble/litime.c" "ble/shelly.c"
       "io/truma_lin.c" "io/truma_errors.c" "io/tanks.c"
       "ui/ui.c" "ui/ui_widgets.c" "ui/img_logo.c" "ui/ui_energy.c" "ui/ui_solar.c" "ui/ui_tanks.c"
       "ui/ui_starlink.c" "ui/ui_settings.c" "ui/ui_entry.c" "ui/ui_weather.c"
       ${FONT_SRCS}
  INCLUDE_DIRS "."
  REQUIRES nvs_flash esp_netif esp_event esp_timer esp_http_client esp_lcd
           esp_driver_gpio esp_driver_uart esp_driver_ledc esp_driver_i2c esp_adc
           esp_wifi bt mbedtls esp_http_server app_update lwip console vfs
           fugo101__microlink json
)

# VanDeck firmware

ESP-IDF 6.0 + LVGL 9 firmware for the **Elecrow CrowPanel Advanced 7" ESP32-P4** (1024×600). It's a port of the `Van Panel.dc.html` design.

## Build

```bash
cd firmware
idf.py set-target esp32p4
idf.py menuconfig        # VanDeck menu: LIN pins, tank wiring, demo mode
idf.py build flash monitor
```

Optional large numerals (without them, the big numbers use Montserrat 48):

```bash
./tools/gen_fonts.sh path/to/Montserrat-SemiBold.ttf
# then enable VanDeck > "Use generated large numeral fonts"
```

Tested with ESP-IDF 6.0.1. Two dependency notes:

- `esp_hosted` is pinned to 2.x. 3.x renamed the hosted-BT Kconfig, and `ble_svc_init()` uses the 2.x calls.
- `esp_matter` 1.6.0 is built for IDF 5.5 and asks for `esp_wifi_remote` ^0.7. `components/espressif__esp_wifi_remote`
  is an unmodified copy of 1.6.0 (IDF 6 sources only) that `override_path` makes the solver accept, and the top-level
  `CMakeLists.txt` relaxes two truncation warnings in connectedhomeip. Drop both once esp-matter supports IDF 6.
- `components/esp_lvgl_port` is a copy of esp_lvgl_port 2.9.0, patched for IDF 6.0, which removed the DPI `on_frame_buf_complete` callback. Delete it, along with its `override_path` in `main/idf_component.yml`, once upstream has a fix.

## Updating over Wi-Fi (OTA)

After the first USB flash, updates go over Wi-Fi. Bump `PROJECT_VER` in `CMakeLists.txt`, then:

```bash
idf.py build && tools/ota.sh            # or: tools/ota.sh 10.0.45.162
```

- The panel serves `POST /ota` and `GET /version` on port 80 and announces itself as `vandeck.local`.
- Uploads need the key from `VanDeck > OTA upload key` (default `vandeck`; change it). `tools/ota.sh` reads it from `sdkconfig`.
- The image is written to the inactive slot (`ota_0`/`ota_1`) and checked before anything is switched: it must be a `vandeck` image, and `esp_ota_end()` verifies the whole file.
- A new image boots "pending verify" and confirms itself after 30 s of running. If it crashes or resets before that, the bootloader goes back to the previous one.
- The panel shows a progress screen during the upload. If an upload fails, the reason stays on screen until tapped.
- Changing `partitions.csv` or the bootloader still needs USB.

## Serial console over Wi-Fi

```bash
tools/console.sh            # or: tools/console.sh 10.0.45.162
```

You get the last 32 KB of log output, then live output: the same text UART0 prints, which still works too.
The first line sent must be the OTA key, which the script sends for you. Commands: `help`, `version`,
`heap`, `log <tag|*> <level>` (e.g. `log lin debug`), `reboot`, `quit`. One client at a time on TCP port 23.

## Remote access

Use a subnet router next to the panel. A small Linux box on the van LAN (a Raspberry Pi Zero W or Zero 2 W on
12 V through a USB buck works) running Tailscale with `--advertise-routes=192.168.1.0/24` makes the panel reachable
from the tailnet at its LAN address, with nothing running on the panel:

```bash
tools/ota.sh 192.168.1.77
tools/console.sh 192.168.1.77
curl http://192.168.1.77/version
```

Approve the route in the admin console and limit it with an ACL to your own devices. The client needs
`--accept-routes` on Linux. macOS and phones accept routes by default. A client whose own LAN is also 192.168.1.0/24
reaches its local network instead, so give the van LAN a less common subnet if that happens.

Anyone who can reach the panel can update it and read its console with only the OTA key, so change it from the
default (`VanDeck > OTA upload key`).

## Apple Home (Matter)

The panel is a Matter accessory (`VanDeck > Matter`, on by default). In the Home app it shows up as:

- **Battery 1** and **Battery 2**: temperature sensors. Open one for its charge (%), charging state and low battery
  (warning at 20 %, critical at 10 %).
- **Battery bank**: an electrical sensor with the bank's power, voltage and current. Power is positive while charging.
  It appears in the Energy view on iOS 27.

To pair, open Settings > Home and scan the QR code from the Home app (+ > Add Accessory), or type the setup code.
The phone must be on the same Wi-Fi as the panel. Pairing runs over Wi-Fi only, because CHIP-over-BLE would take
NimBLE away from the battery and charger scanning. Home warns that the accessory is uncertified, because it uses
the CHIP test vendor ID and test attestation certificates. Choose to add it anyway.

- The setup code is random per panel. It's generated on first start and kept in NVS (`vd_mt`), so it survives
  restarts and unpairing. It is not the well-known CHIP test code, so nobody else on a campsite network can pair the panel first.
- For remote access, Apple Home needs a home hub (HomePod or Apple TV) on the van's network.
- Settings > Home > *Unpair* (tap twice) or `matter unpair` removes the panel from every home and restarts it.
  Removing it from its last home in the Home app reopens pairing without a restart.
- CHIP's static state and heap are in PSRAM (`main/linker.lf`, `ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL`). Statically it
  adds about 5 KB of internal RAM and 0.7 MB of flash. Its use at run time hasn't been measured on the panel yet.
- **Not yet verified on hardware:** CHIP's own mDNS responder running beside `vandeck.local` (both bind port 5353),
  and what iOS 27 shows for a Power Source on a temperature sensor and for negative bank power.

## Shell

On the **USB serial port** the panel runs [BreezyBox](https://components.espressif.com/components/valdanylchuk/breezybox),
a BusyBox-style shell (`ls`, `cat`, pipes, redirects, history, tab completion, `free`, `df`, `wifi`, ...). Its
files live on the `storage` LittleFS partition, which only exists after a USB flash of the current partition table.
Without that partition the panel falls back to plain console commands. The Wi-Fi console runs its input
through the same shell, so `ls`, `df`, `cat` and redirects work there too.

Shell scripting is on (`CONFIG_BREEZYBOX_SHELL_SCRIPTING`): pipes (through temp files on the storage partition),
variables, `$((...))`, `&&`/`||`, `if`/`for`/`while`, globbing. For example `status | grep -i batt` or
`status > log.txt && wc -l log.txt`. VanDeck adds `grep [-i] [-v] <pattern> [file]`. BreezyBox's `head`/`tail`
only read files, so use `cmd > f; head -n 3 f` for those.

The panel's own commands work there and in the Wi-Fi console (`tools/console.sh`):

| Command | Shows |
|---|---|
| `status` | everything below at once |
| `batt` | each battery: V, A, SOC, Ah, temperature, reading age; bank total |
| `solar` | PV power, charge stage, battery side, yield, load, errors |
| `heater` | LIN link / registration, settings, room temperature |
| `tanks` | LPG and grey water levels |
| `starlink` | power, reachability, dish statistics |
| `net` | Wi-Fi network, RSSI, IP |
| `ble` | paired devices: RSSI, last advert, reading age |
| `matter [qr\|unpair]` | Apple Home pairing state and setup code; print the QR payload, or unpair from every home |
| `screen [30s\|1m\|5m\|never\|wake]` | screen saver state; set the timeout or wake the screen |
| `grep [-i] [-v] <pattern> [file]` | filter lines, e.g. `status \| grep -i batt` |

## Screen saver

Settings > Display: turn the screen off after 30 s / 1 min / 5 min / never, plus brightness. When it's off, the
first touch only wakes it. It doesn't press anything underneath. It never turns off during an update.

## Layout

```
main/
  main.c               start-up order
  board/               MIPI-DSI panel, GT911 touch, backlight, LVGL port
  app/model.*          shared state (mutex + version counter). Drivers write; the UI reads a snapshot every 200 ms
  app/settings.*       persisted config in NVS (debounced): devices, keys, Wi-Fi, time, units, heater
  app/solar_log.*      90-day daily solar log, recorded by the panel itself
  app/sim.*            demo data for any role that isn't paired (VD_SIMULATE)
  net/wifi_svc.*       Wi-Fi through the C6 (esp_wifi_remote)
  net/time_svc.*       SNTP, POSIX time zones, manual set
  net/starlink.*       dish status over grpc-web, auto-off
  net/ota_svc.*        OTA over Wi-Fi: HTTP upload, mDNS, rollback confirmation
  net/status_api.*     GET /status: read-only JSON of the panel's state (same key as /ota)
  net/matter_svc.*     Matter node for Apple Home: battery and bank endpoints, per-panel setup code, pairing state
  net/log_svc.*        serial console over Wi-Fi (log mirror + commands, port 23)
  app/cli.*, app/shell.c  shell commands; BreezyBox on USB serial (falls back to esp_console)
  ble/ble_svc.*        NimBLE scanning, role-based pairing, stale detection
  ble/gatt_link.*      small GATT-client helper (connect, discover, notify, read, write)
  ble/victron.*        instant-readout decrypt (AES-CTR, PSA crypto)
  ble/litime.*         LiTime BMS polling
  ble/shelly.*         Shelly Gen2 RPC over BLE (Switch.Set / GetStatus)
  io/truma_lin.*       LIN master primitives + sniff mode
  io/tanks.*           KIB probes: digital lines or ADC, read on demand
  ui/                  one file per screen; ui_widgets.* holds the palette and helpers
```

## Bring-up checklist

These are things I couldn't confirm from the docs. Each one is marked `VERIFY` or `TODO` in the code.

1. **Panel IC and reset pin:** I assumed EK79007, the common 1024×600 DSI controller. Check Elecrow's `factory_sourcecode` and swap the driver or init sequence in `board/board.c` if it's different.
2. **C6 SDIO pins and reset GPIO:** these are in `sdkconfig.defaults`. Copy them from Elecrow's sdkconfig. Board v1.1+ uses D0–D3 on 17/16/15/14.
3. **esp_hosted BLE start-up:** the call names in `ble_svc_init()` follow esp_hosted 2.x. The C6 must also run esp_hosted slave firmware with BT enabled.
4. **LiTime response offsets:** these are unverified. The first frame from each battery is logged as hex, so you can match the offsets against it.
5. **Truma (iNet box mode, the default):** the CP Plus stays in charge and this panel joins the LIN bus as an iNet box, following [inetbox.py](https://github.com/danielfett/inetbox.py). Your CP Plus must be "inet ready". Connect the transceiver to the bus with an RJ12 splitter, then run **PR SET** on the CP Plus so it registers the iNet box; the heater card shows "Run PR SET on the CP Plus" until it has. The first status can take up to a minute to arrive. Fit pull-ups on the transceiver's TXD and RXD (2.2–4.7 kΩ to 3.3 V) or frames fail their checksum. The iNet box can't select ventilation-only, so "Vent" is sent as Eco, and boost hot water shows as "Hot". `VanDeck > Truma Combi (LIN) > LIN role` also has a sniff-only mode, and an unfinished master mode that must never run with the CP Plus on the bus.
6. **Starlink field numbers:** confirm them against the schema the dish serves (`grpcurl … describe`). Snow-melt control is a TODO.
7. **Victron PV voltage:** instant readout doesn't carry it, so the panel shows `--`. The Bluetooth password is stored for a later GATT link, which is also the route to the controller's own history.

## Behaviour notes

- **Pairing:** each role (solar charger, battery 1, battery 2, Starlink power) takes one device. The scan only lists devices of that role's type, and hides devices already assigned to another role.
- **Tanks:** read on demand only. Probes are powered for about a second, and the reading stays on screen for `tank_show_sec` (default 30 s, 0 = keep).
- **Solar log:** adds up changes in the controller's yield counter, so energy lands on the right local day even though the counter resets after dark, not at midnight.
- **Settings:** saved to NVS 2 s after the last change.

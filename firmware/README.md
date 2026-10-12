# VanDeck firmware

ESP-IDF 6.0 + LVGL 9 firmware for the **Elecrow CrowPanel Advanced 7" ESP32-P4** (1024×600). It's a port of the `Van Panel.dc.html` design.

## Build

```bash
cd firmware
idf.py set-target esp32p4
idf.py menuconfig        # VanDeck menu: LIN pins, GPS pins, tank wiring, demo mode
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

## Remote website (Cloudflare)

The panel can keep one outbound WebSocket to a Cloudflare Worker, which serves a website with its current values and
the Truma controls, behind Cloudflare Access. Nothing listens on the panel. Setup, secrets and key rotation are in
`../cloud/README.md`. On the panel side: `relay url <wss://...>`, `relay key <secret>`, `relay on`. The key lives
only in NVS (`vd_relay`) and is never printed. Commands from the website go through the same calls as the touch
screen.

## Apple Home (Matter)

Matter is **off** in the current build (`CONFIG_VD_MATTER=n`): the code below still builds but never starts. Set it
back to `y` to use it.


The panel is a Matter accessory (`VanDeck > Matter`, on by default). Apple Home has no tile for a plain number and
shows one battery per accessory, so some values use stand-in device types. Home names the tiles generically, so
rename them once after pairing:

| # | Matter device | Rename to | Shows / does |
|---|---|---|---|
| 1 | Plug with power + energy metering | Battery bank | Power drawn from the bank (W, 0 while charging) and energy drawn (kWh), listed in the Energy view on iOS 27. Its switch is a stand-in that always reads on. Every tile's settings show the bank's charge, charging state and low battery (20 %, critical 10 %). |
| 2, 3 | Dimmable lights | Battery 1, Battery 2 | Brightness = charge, on the tile. |
| 4, 5 | Light sensors | Bank W, PV W | Lux = watts drawn from the bank, and PV power. |
| 6 | Thermostat | Heater | Truma heating: Off / Heat, setpoint 5–30 °C, room temperature. |
| 7 | Temperature sensor | Room | The CP Plus room temperature. |
| 8 | Fan | Hot water | Boiler: Off / Low = Eco / High = Hot. |
| 9 | Fan | Fan | Truma fan: Low = Eco / High = High. "Vent" isn't offered; the iNet box can't select it. |

- Changes made in Home reach the heater within a second. Changes on the panel or the CP Plus show in Home within 10 s.
- The stand-ins push back. Switching the bank plug or a battery light from Home is undone at the next update
  (10 s). The battery lights still count as lights, though: "turn off all the lights" and scenes include them,
  so leave them out of scenes. Turning the Truma fan off is undone too, because the Truma has no fan-off setting.
- Sensor tiles (Bank W, PV W, Room) appear under Climate. Add them to Favourites to see them on the home screen.
- Endpoints are numbered in creation order (`matter_svc.cpp`). Add new ones at the end, because a controller that's
  already paired keeps the old numbers. Changing a device type means removing the panel from Home and adding it again.

To pair, open Settings > Home and scan the QR code from the Home app (+ > Add Accessory), or type the setup code.
The phone must be on the same Wi-Fi as the panel. Pairing runs over Wi-Fi only, because CHIP-over-BLE would take
NimBLE away from the battery and charger scanning. Home warns that the accessory is uncertified (it uses the CHIP
test vendor ID and test attestation certificates). Choose to add it anyway.

- The setup code is random per panel. It's generated on first start and kept in NVS (`vd_mt`), so it survives
  restarts and unpairing. It is not the well-known CHIP test code, so nobody else on a campsite network can pair the panel first.
- For remote access, Apple Home needs a home hub (HomePod or Apple TV) on the van's network.
- Settings > Home > *Unpair* (tap twice) or `matter unpair` removes the panel from every home and restarts it.
  Removing it from its last home in the Home app reopens pairing without a restart.
- Matter starts once the running image has confirmed itself (30 s), so a Matter problem can't roll back an update.
  If a start ends in a panic or watchdog reset, the next start skips Matter (Settings > Home and `matter` say so)
  until `matter retry`. Power cuts don't count.
- CHIP's static state and heap are in PSRAM (`main/linker.lf`, `ESP_MATTER_MEM_ALLOC_MODE_EXTERNAL`). It adds about
  5 KB of static internal RAM and 0.7 MB of flash.
- CHIP's mDNS responder shares port 5353 with `vandeck.local`. That needs `espressif/mdns` in socket mode
  (`MDNS_NETWORKING_SOCKET`), which sets `SO_REUSEADDR`. CHIP's packet buffers need `PBUF_POOL_BUFSIZE=1536`,
  which is set in the top-level `CMakeLists.txt`. The 536-byte TCP MSS kept for the weak link would otherwise
  make them too small for Matter.
- Several clusters (temperature, illuminance, fan) are CHIP code-driven clusters. `attribute::update()` doesn't
  reach them; `matter_svc.cpp` sets them through the cluster objects.

### Test panel

A second panel on its UART port (CH340, `/dev/cu.wchusbserial*`) with demo data makes Matter work quick to check:

```bash
mkdir -p build_demo && sed 's/^# CONFIG_VD_SIMULATE is not set/CONFIG_VD_SIMULATE=y/' sdkconfig > build_demo/sdkconfig
idf.py -B build_demo -D SDKCONFIG=build_demo/sdkconfig -p /dev/cu.wchusbserial10 flash monitor
```

The USB-JTAG shell isn't on that port. To clear the crash guard there, erase NVS with
`python -m esptool --chip esp32p4 -p /dev/cu.wchusbserial10 erase-region 0x9000 0x60000`. Both panels answer as
`vandeck.local`, so use IP addresses while both are on the network.

## GPS

A u-blox NMEA module (developed with a GT-U7, u-blox 7) on the expansion header, ported from the GPS-A devkit sketch.
It gives the weather its location (ahead of the Starlink dish, then a location set by hand), sets the clock while
SNTP hasn't (no internet; automatic time on), keeps a track log, and shows on the remote website's GPS tab and in
`/status` (`gps` and `location`). The panel's own screens only show it as the weather location ("GPS").

Wiring (3.3 V logic only; the module's own 3.3 V regulator is fine on 3.3 V too):

| Module | CrowPanel header |
|---|---|
| VCC | a spare 3.3 V pin (one of the three feeds the LIN transceiver's SLP) |
| GND | GND |
| TX | IO50 (`VD_GPS_RX_GPIO`) |
| RX | IO49 (`VD_GPS_TX_GPIO`; -1 = listen only, no aiding) |

`VanDeck > GPS` in menuconfig: `VD_GPS` (on by default; without a module it just reports no data), UART port
(`VD_GPS_UART_NUM`, 2; UART0 is the console, UART1 the LIN bus), the two pins, and `VD_GPS_BAUD` (9600, the
module's default). If the fix never comes, `gps raw` shows what arrives: nothing at all means the module's TX isn't
on IO50 (swap the two wires or the two settings); garbage means the baud rate is wrong.

With the module's RX wired the panel also helps it to a faster first fix, as GPS-A did:

- at start-up, once SNTP has the time (or after 2 minutes without), UBX-AID-INI with the time and the last known
  position (the weather location), skipped if the module already has a fix;
- every 30 minutes with a fix, it reads the ephemeris, almanac and ionosphere data back from the module (AID-EPH,
  AID-ALM, AID-HUI) into `/root/gps/aid.bin`, and replays them at the next start (ephemeris only while under 4 h old,
  almanac up to 30 days).

The track is one CSV per UTC day, `/root/gps/t/YYYYMMDD.csv` (`time,lat,lon,alt,sats,hdop,kmh`). It gets a point only
after a real move: at least 10 m or 8 × HDOP, at most one every 30 s, HDOP 5 or better. When the storage partition
has under 256 KB free, the oldest days are deleted. Read it with `ls /root/gps/t` and `cat` in the shell. Both
files need the `storage` partition (see Shell).

Internal RAM: a 5 KB task stack and the 1 KB UART buffer; the parser lives in PSRAM. `gps` prints the stack
high-water mark, so the stack can be trimmed once it has run with a real module for a while.

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
| `gps [raw [s]]` | fix, satellites, HDOP, speed, GPS time; UART counters, module version, aiding, today's track; `raw` prints the NMEA as it arrives (default 3 s, up to 10) |
| `weather [refresh\|loc <lat> <lon>]` | forecast and where it's for (GPS, Starlink or by hand) |
| `net` | Wi-Fi network, RSSI, IP |
| `ble` | paired devices: RSSI, last advert, reading age |
| `relay [on\|off\|url <u>\|key <k>]` | Cloudflare relay state; switch it, or set its URL and key |
| `matter [qr\|unpair\|retry]` | Apple Home pairing state and setup code; print the QR payload, unpair from every home, or start Matter again after a skipped start |
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
  net/time_svc.*       SNTP, POSIX time zones, manual set, GPS time while SNTP hasn't synced
  net/starlink.*       dish status over grpc-web, auto-off
  net/ota_svc.*        OTA over Wi-Fi: HTTP upload, mDNS, rollback confirmation
  net/status_api.*     GET /status: read-only JSON of the panel's state (same key as /ota)
  net/relay_svc.*      outbound WebSocket to the Cloudflare relay: status out, heater commands in
  net/matter_svc.*     Matter node for Apple Home: bank, batteries, PV, Truma controls; per-panel setup code, pairing
  net/log_svc.*        serial console over Wi-Fi (log mirror + commands, port 23)
  app/cli.*, app/shell.c  shell commands; BreezyBox on USB serial (falls back to esp_console)
  ble/ble_svc.*        NimBLE scanning, role-based pairing, stale detection
  ble/gatt_link.*      small GATT-client helper (connect, discover, notify, read, write)
  ble/victron.*        instant-readout decrypt (AES-CTR, PSA crypto)
  ble/litime.*         LiTime BMS polling
  ble/shelly.*         Shelly Gen2 RPC over BLE (Switch.Set / GetStatus)
  io/truma_lin.*       LIN master primitives + sniff mode
  io/tanks.*           KIB probes: digital lines or ADC, read on demand
  io/gps.*             GPS module on UART2: NMEA GGA/RMC, UBX aiding, track log on LittleFS
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

# VanRelay

ESP32 that switches the Starlink 12 V feed for the VanDeck panel over BLE. It advertises as `VanRelay`; the panel
pairs it under Settings > Devices ("Starlink relay") and replaces the former Shelly.

## Wiring

Uses a ready-made 1-channel opto-isolated relay module (Valefod, **5 V** coil, JQC-3FF-S-Z, 51 x 26 x 19 mm); it already has
the driver, flyback diode and LEDs. The 12 V and 24 V versions of that module will not run from 5 V.

```
 +12V (fused) ──┬── buck 12V->5V ──┬── ESP32 5V/VIN
                │                  └── module VCC (left screw terminal)
 GND ───────────┴── buck GND ── module GND, ESP32 GND
 ESP32 GPIO25 ─────── module IN
 module COM ── +12V        module NO ──> step-up / Starlink   (right screw terminal)
```

The buck feeds the ESP32 and the module from ahead of the relay contacts, so they stay up while Starlink is off.
Budget about 0.2 A at 5 V (relay coil plus ESP32 with Bluetooth). Keep the jumper on H (high trigger, firmware default);
if you move it to L, pick active-low in menuconfig. Test with the module LED or a multimeter before connecting
Starlink, and check that the relay stays open while the ESP32 boots. An optional 10k from IN to GND keeps IN low
while the ESP32 is in reset.

## Build

Classic ESP32 (any NimBLE-capable chip works):

    idf.py set-target esp32
    idf.py menuconfig      # VanRelay: GPIO (default 25), shared key, power-up state, panel-silence timeout
    idf.py build flash

Set the same key in the panel (`VD_RELAY_KEY`). Protocol: one characteristic; read = 1 byte (0/1), write = state byte + key.
The key is sent unencrypted, so it only stops casual or accidental control.

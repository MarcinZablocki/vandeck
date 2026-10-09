# Display simulator

Runs the real UI (`main/ui`, `main/app/sim.c` demo data) on the desktop at **1024×600**: as the panel's colour LCD
(`--lcd`), or filtered the way an e-paper panel (black/white with a 4-grey mode) would show it. The UI is laid out for
1024×600 only; `--size` shows what another panel would clip (e.g. `--size 800x480` for a Waveshare 7.5" V2). Services are stubs
(`sim_services.c`) with canned heater, tank and weather data.

## Build

Needs SDL2 (`brew install sdl2`) and LVGL in `../managed_components` (run `idf.py reconfigure` in `firmware/` once).

```bash
cmake -S firmware/sim -B firmware/sim/build -DCMAKE_BUILD_TYPE=Release
cmake --build firmware/sim/build -j
firmware/sim/build/vandeck_sim --lcd                        # the panel (1024x600)
firmware/sim/build/vandeck_sim --size 800x480 --scale 2     # 800x480 e-paper
```

## Keys

| Key | |
|---|---|
| 1 | colour, no filter (what the LCD shows) |
| 2 | 1-bit threshold (default) |
| 3 | 1-bit Floyd–Steinberg dither |
| 4 | 4-level grey |
| I | invert: a quick stand-in for a light theme, which suits e-paper better |
| R | simulate panel refreshes (black/white flash on every update) |
| S | save a BMP of the panel |
| Q / Esc | quit |

The window title counts panel updates in the last minute. A real panel takes ~4 s for a full refresh
(~0.4 s partial), so that number needs to be small.

Options: `--lcd`, `--warmup SEC` (run that long before `--shots`, e.g. 65 for the battery time estimate), `--size WxH` (default `1024x600`, the panel), `--scale N`, `--mode 1..4`, `--invert`, `--no-refresh`, and `--shots DIR`, which renders every page
(in every mode, or colour only with `--lcd`), every Settings tab, both text-entry screens and a few Energy frames of the battery flow animation to `DIR`, then exits.

# Display simulator

Runs the real UI (`main/ui`, `main/app/sim.c` demo data) on the desktop at **800×480**: as a plain colour LCD
(`--lcd`), or filtered the way a Waveshare 7.5" e-paper panel (V2, black/white with a 4-grey mode) would show it. Services are stubs
(`sim_services.c`) with canned heater, tank and weather data.

## Build

Needs SDL2 (`brew install sdl2`) and LVGL in `../managed_components` (run `idf.py reconfigure` in `firmware/` once).

```bash
cmake -S firmware/sim -B firmware/sim/build -DCMAKE_BUILD_TYPE=Release
cmake --build firmware/sim/build -j
firmware/sim/build/vandeck_sim --lcd --scale 2     # 800x480 LCD
firmware/sim/build/vandeck_sim --scale 2           # 800x480 e-paper
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

Options: `--lcd`, `--size WxH` (e.g. `1024x600` for the current panel), `--scale N`, `--mode 1..4`, `--invert`, `--no-refresh`, and `--shots DIR`, which renders every page
(in every mode, or colour only with `--lcd`), every Settings tab, both text-entry screens and a few Energy frames of the battery flow animation to `DIR`, then exits.

The UI picks its compact layout (`g_compact` in `ui_widgets.h`) from the display size, so `--size 800x480` and
`--size 1024x600` show what each panel gets from the same firmware.

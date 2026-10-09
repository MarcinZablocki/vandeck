#!/usr/bin/env bash
# Generates the large numeral fonts used for PV watts (92), tank level (110) and daily yield (64).
# Needs Node.js and Montserrat-SemiBold.ttf (Google Fonts). Then enable VanDeck > "Use generated large numeral fonts".
set -e
TTF=${1:-Montserrat-SemiBold.ttf}
OUT=$(dirname "$0")/../main/fonts
mkdir -p "$OUT"
for SIZE in 64 92 110; do
  npx lv_font_conv --font "$TTF" --size $SIZE --bpp 4 --format lvgl --no-compress \
    --symbols "0123456789.-/%EF" --lv-include lvgl.h \
    --lv-font-name vd_num_$SIZE -o "$OUT/vd_num_$SIZE.c"
done
echo "Fonts written to $OUT"

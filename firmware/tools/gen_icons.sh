#!/usr/bin/env bash
# Generates the icon fonts (rail navigation + weather) from Material Design Icons (Apache 2.0).
# Needs Node.js. Glyphs are remapped to U+E000.. so they are 3-byte UTF-8; ui/ui_icons.h has the matching strings.
# Keep the order below in step with ui_icons.h.
set -e
MDI_VER=7.4.47
OUT=$(dirname "$0")/../main/fonts
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
(cd "$TMP" && npm pack @mdi/font@$MDI_VER --silent >/dev/null && tar xzf mdi-font-$MDI_VER.tgz)
TTF="$TMP/package/fonts/materialdesignicons-webfont.ttf"
mkdir -p "$OUT"

# Rail and small inline icons
NAV=(F140B F0A72 F058C F0471 F0493)                     # lightning-bolt solar-power water satellite-variant cog
# Weather: sunny night partly-cloudy night-partly-cloudy cloudy fog rainy pouring snowy snowy-heavy snowy-rainy
#          lightning-rainy hail
WX=(F0599 F0594 F0595 F0F31 F0590 F0591 F0597 F0596 F0598 F0F36 F067F F067E F0592)
EXTRA=(F034E F054B F059D F058E F059C F059B)            # map-marker umbrella-outline windy water-percent sunrise sunset

ranges() {   # ranges <first PUA index> <codepoints...>
  local i=$1; shift
  for cp in "$@"; do printf -- "--range 0x%s=>0x%X " "$cp" $((0xE000 + i)); i=$((i + 1)); done
}

ALL="$(ranges 0 "${NAV[@]}" "${WX[@]}" "${EXTRA[@]}")"
WXONLY="$(ranges ${#NAV[@]} "${WX[@]}")"
# shellcheck disable=SC2086
npx --yes lv_font_conv@1.5.3 --font "$TTF" $ALL --size 32 --bpp 4 --format lvgl --no-compress \
  --lv-include lvgl.h --lv-font-name vd_icons_32 -o "$OUT/vd_icons_32.c"
# shellcheck disable=SC2086
npx --yes lv_font_conv@1.5.3 --font "$TTF" $WXONLY --size 80 --bpp 4 --format lvgl --no-compress \
  --lv-include lvgl.h --lv-font-name vd_icons_80 -o "$OUT/vd_icons_80.c"
echo "Icon fonts written to $OUT"

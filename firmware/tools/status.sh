#!/bin/sh
# Read-only status of the panel as JSON (batteries, solar, heater, tanks, Starlink, Wi-Fi, tailnet).
#   tools/status.sh [host]        host defaults to vandeck.local (or set VANDECK_HOST); a tailnet name or 100.x address works too
cd "$(dirname "$0")/.."
HOST="${1:-${VANDECK_HOST:-vandeck.local}}"
KEY=$(sed -n 's/^CONFIG_VD_OTA_KEY="\(.*\)"$/\1/p' sdkconfig)
curl -fsS --max-time 10 -H "X-OTA-Key: $KEY" "http://$HOST/status"

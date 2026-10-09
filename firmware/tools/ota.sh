#!/bin/sh
# Upload build/vandeck.bin to the panel over Wi-Fi.
#   tools/ota.sh [host]          host defaults to vandeck.local (or set VANDECK_HOST)
# The key comes from sdkconfig (VanDeck > OTA upload key) so it matches what the panel was built with.
set -e
cd "$(dirname "$0")/.."
HOST="${1:-${VANDECK_HOST:-vandeck.local}}"
BIN=build/vandeck.bin
KEY=$(sed -n 's/^CONFIG_VD_OTA_KEY="\(.*\)"$/\1/p' sdkconfig)
[ -f "$BIN" ] || { echo "no $BIN - run idf.py build first" >&2; exit 1; }
echo "panel is running: $(curl -fsS --max-time 5 "http://$HOST/version")"
echo "uploading $BIN ($(wc -c < "$BIN" | tr -d ' ') bytes) to $HOST ..."
curl -f --progress-bar --max-time 900 -H "X-OTA-Key: $KEY" -H "Content-Type: application/octet-stream" \
     --data-binary "@$BIN" "http://$HOST/ota"
echo "waiting for the panel to come back ..."
for i in $(seq 1 30); do
    sleep 2
    if V=$(curl -fsS --max-time 2 "http://$HOST/version" 2>/dev/null); then echo "now running: $V"; exit 0; fi
done
echo "panel didn't answer within 60 s - check the screen or the serial log" >&2
exit 1

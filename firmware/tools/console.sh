#!/bin/sh
# Serial console over Wi-Fi: recent log, live output and commands (type 'help'). 'telnet <host>' works too.
#   tools/console.sh [host]       host defaults to vandeck.local (or set VANDECK_HOST); Ctrl-C to leave
cd "$(dirname "$0")/.."
HOST="${1:-${VANDECK_HOST:-vandeck.local}}"
KEY=$(sed -n 's/^CONFIG_VD_OTA_KEY="\(.*\)"$/\1/p' sdkconfig)
{ printf '%s\n' "$KEY"; cat; } | nc "$HOST" 23

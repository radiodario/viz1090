#!/bin/bash
# HackRF -> dump1090-fa (Beast on :30005) -> viz1090, centred on home,
# with the TfL departures page (press T to switch).
#
# Home location comes from home.conf next to this script (not committed):
#   LAT=51.5
#   LON=-0.1
# dump1090 is restarted automatically if the HackRF drops off USB; viz1090 reconnects by itself.
cd "$(dirname "$0")"
DUMP=../dump1090-fa/dump1090
if [ -f home.conf ]; then
  . ./home.conf
fi
if [ -z "$LAT" ] || [ -z "$LON" ]; then
  echo "Set LAT and LON in $(pwd)/home.conf" >&2
  exit 1
fi
LOG=/tmp/dump1090.log

(
  while true; do
    "$DUMP" --device-type hackrf --enable-amp --net --quiet --lat "$LAT" --lon "$LON" \
      --write-json /tmp/d1090json >> "$LOG" 2>&1 < /dev/null
    echo "$(date) dump1090 exited ($?), restarting in 2s" >> "$LOG"
    sleep 2
  done
) &
LOOP_PID=$!
trap 'kill $LOOP_PID 2>/dev/null; pkill -f "dump1090 --device-type hackrf"' EXIT
mkdir -p /tmp/d1090json
sleep 2
# map UI at 2x; board scale halved to keep its text the same size
./viz1090 --server localhost --lat "$LAT" --lon "$LON" --transit --weather --zoom 1 --uiscale 2 --transit-scale 0.75 "$@"

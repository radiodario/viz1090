#!/bin/bash
# viz1090 as a kiosk on a Raspberry Pi: straight to the display (no desktop),
# departures and weather, pages rotating (map 60 s, departures 30 s, one
# full weather cycle), no aircraft source.
#
# Location comes from ../home.conf (LAT=..., LON=...). Extra options are
# passed through. Sizes follow the screen height, so the same command suits a
# TV or a 640x480 CRT (--uiscale 1.2 makes everything 20% bigger). --server bunny
# shows aircraft from another machine's dump1090 (drop --no-adsb then).
cd "$(dirname "$0")/.."
. ./home.conf
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-kmsdrm}"
exec ./viz1090 --no-adsb --lat "$LAT" --lon "$LON" --transit --weather \
    --fullscreen --hide-cursor --zoom 1 \
    --page-times 60,30,cycle,30 --adsb-api "$@"

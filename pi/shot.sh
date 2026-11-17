#!/bin/bash
# Screenshot the Pi build without the Pi: runs build-pi/viz1090 in Docker on
# a virtual display (Xvfb) and saves a PNG.
#   pi/shot.sh OUT_DIR SECONDS [viz1090 options...]
# e.g. pi/shot.sh /tmp/shots 40 --lat 51.4826 --lon -0.0077 --zoom 1.5
# OUT_DIR doubles as HOME, so tile and walk caches persist between runs.
# Needs the test image: docker build -t viz1090-pi-test -f pi/Dockerfile.test pi
set -e
cd "$(dirname "$0")/.."
OUT="$(cd "$1" && pwd)"; WAIT="$2"; shift 2
docker run --rm --platform linux/arm64 -v "$PWD:/app:ro" -v "$OUT:/out" -w /app -e HOME=/out \
    viz1090-pi-test bash -c '
    Xvfb :1 -screen 0 1280x720x24 >/dev/null 2>&1 & sleep 1
    DISPLAY=:1 stdbuf -oL -eL ./build-pi/viz1090 --no-adsb --screensize 1280 720 "$@" > /out/log.txt 2>&1 &
    P=$!; sleep '"$WAIT"'; kill -USR1 $P; sleep 2; kill $P 2>/dev/null
    convert /tmp/viz1090-snapshot.bmp /out/shot.png && echo /out/shot.png' _ "$@"

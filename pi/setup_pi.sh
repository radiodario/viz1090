#!/bin/bash
# One-time setup on a fresh Raspberry Pi OS Lite: build deps, build viz1090,
# fonts, and the boot service. Run from the viz1090 checkout:  ./pi/setup_pi.sh
# Map data (*.bin, mapnames, airportnames) and home.conf are copied in separately.
set -e
cd "$(dirname "$0")/.."

sudo apt-get update
sudo apt-get install -y build-essential git python3 \
    libsdl2-dev libsdl2-ttf-dev libsdl2-gfx-dev libsdl2-image-dev \
    libcurl4-openssl-dev nlohmann-json3-dev

make -j"$(nproc)"
./getfonts.sh

sudo cp pi/viz1090.service /etc/systemd/system/viz1090.service
sudo systemctl daemon-reload
sudo systemctl enable viz1090.service
echo "Done. Start it with: sudo systemctl start viz1090"

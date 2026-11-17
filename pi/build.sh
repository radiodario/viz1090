#!/bin/bash
# Build viz1090 for the Pi on this Mac (Docker, arm64 Debian trixie, native
# on Apple Silicon) and install it: copies the binary over and restarts the
# service. Usage: pi/build.sh [ssh target, default pi@vizpi.local]
set -e
cd "$(dirname "$0")/.."
TARGET="${1:-pi@vizpi.local}"
SSH="ssh -o BatchMode=yes -o ProxyJump=none -i $HOME/.ssh/id_ed25519_vizpi"

# separate tree so the Mac's own objects never mix with the Pi's
mkdir -p build-pi
rsync -a --include='*.cpp' --include='*.c' --include='*.h' --include='Makefile' --exclude='*' ./ build-pi/
docker run --rm --platform linux/arm64 -v "$PWD/build-pi:/src" viz1090-pi-build make -j"$(sysctl -n hw.ncpu)" viz1090

COPYFILE_DISABLE=1 rsync -a -e "$SSH" build-pi/viz1090 "$TARGET:viz1090/viz1090.new"
$SSH "$TARGET" 'cd ~/viz1090 && mv viz1090.new viz1090 && sudo systemctl restart viz1090 && echo installed'

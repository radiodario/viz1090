# Build environment matching the Pi (Raspberry Pi OS / Debian trixie, arm64).
# On an Apple Silicon Mac this runs natively, so builds take seconds; see pi/build.sh.
FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential pkg-config \
        libsdl2-dev libsdl2-ttf-dev libsdl2-gfx-dev libsdl2-image-dev \
        libcurl4-openssl-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src

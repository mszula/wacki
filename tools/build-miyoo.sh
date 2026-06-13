#!/usr/bin/env bash
# Cross-build the engine for Miyoo Mini Plus (and pin-compatible
# SigmaStar SSD20x handhelds — Anbernic RG35XX, Powkiddy RGB30, etc.)
# by running the host Makefile inside a Miyoo cross-toolchain Docker
# image that ships SDL2 in the buildroot sysroot.
#
# Local usage:    ./tools/build-miyoo.sh
# CI usage:       same — invoked by `make miyoo` and the .github
#                 workflow's Miyoo leg.
#
# Override the image tag with MIYOO_DOCKER_IMAGE if you fork.
# Default image: bqcuongas/sdl2-miyoo (has buildroot toolchain at
# /opt/mmiyoo and SDL2 in its sysroot).
# WACKI_STRIP=1 strips the release binary; the default keeps symbols.

set -euo pipefail

IMAGE="${MIYOO_DOCKER_IMAGE:-bqcuongas/sdl2-miyoo:latest}"

cd "$(dirname "$0")/.."
. tools/lib/common.sh

require_data_exe
require_docker

# Resolve the version on the HOST (the container's git falls back to "unknown");
# pass it in via -e so the Makefile's `?=` picks it up. See common.sh.
VER="$(wacki_version)"
echo "[miyoo] version: $VER"

echo "[miyoo] using image: $IMAGE"
docker pull --platform linux/amd64 "$IMAGE"

# Mount the local-dev data/ symlink target so the in-tree symlink resolves
# inside the container (see tools/lib/common.sh).
wacki_data_mount

# Run the unchanged Makefile inside the container. Layout used:
#   /opt/mmiyoo/bin/arm-linux-gnueabihf-gcc                — compiler
#   /opt/mmiyoo/arm-buildroot-linux-gnueabihf/sysroot/...  — sysroot
#       usr/bin/sdl2-config                                 — config tool
#       usr/lib/libSDL2-2.0.so.0                            — runtime lib
docker run --rm --platform linux/amd64 \
    -e "WACKI_VERSION=$VER" \
    -e "WACKI_STRIP=${WACKI_STRIP:-0}" \
    -v "$(pwd):/root/workspace" \
    -w /root/workspace \
    "${WACKI_DATA_MOUNT[@]}" \
    "$IMAGE" \
    bash -c '
        set -euo pipefail
        export PATH=/opt/mmiyoo/bin:/opt/mmiyoo/arm-buildroot-linux-gnueabihf/sysroot/usr/bin:$PATH
        # Wipe any host-built artefacts so the cross-build doesnt
        # link against x86_64 .o files left over from a prior make.
        rm -rf dist src/embedded_wacki_pe.c
        make TARGET=miyoo \
             CROSS_COMPILE=arm-linux-gnueabihf- \
             SDL2_CFG=/opt/mmiyoo/arm-buildroot-linux-gnueabihf/sysroot/usr/bin/sdl2-config \
             engine
        if [ "${WACKI_STRIP:-0}" = 1 ]; then
            echo "[miyoo] WACKI_STRIP=1 — stripping release binary"
            arm-linux-gnueabihf-strip --strip-all dist/wacki-miyoo
        fi
    '

bin=dist/wacki-miyoo
if [ ! -f "$bin" ]; then
    echo "error: $bin not produced" >&2
    exit 1
fi

echo "[miyoo] built $bin"
file "$bin" 2>/dev/null || true
ls -lh "$bin"

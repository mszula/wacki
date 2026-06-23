#!/usr/bin/env bash
# tools/build-wii.sh — cross-compile TARGET=wii via the devkitpro/devkitwii
# Docker image (devkitPPC + libogc + SDL2 portlib preinstalled).
# Mirrors tools/build-switch.sh / tools/build-ps2.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

docker run --rm -v "$PWD:/wacki" -w /wacki devkitpro/devkitwii:latest sh -c '
    set -e
    . /etc/profile.d/devkit-env.sh 2>/dev/null || true
    export DEVKITPRO=/opt/devkitpro
    export DEVKITPPC=$DEVKITPRO/devkitPPC
    export PATH=$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH

    make TARGET=wii
'

echo "Gotowe: dist/wacki.dol"

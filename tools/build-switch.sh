#!/usr/bin/env bash
# tools/build-switch.sh — cross-compile TARGET=switch via devkitpro/devkita64.
set -euo pipefail
cd "$(dirname "$0")/.."

docker run --rm -v "$PWD:/wacki" -w /wacki devkitpro/devkita64:latest sh -c '
    set -e
    . /etc/profile.d/devkit-env.sh 2>/dev/null || true
    export DEVKITPRO=/opt/devkitpro
    export DEVKITA64=$DEVKITPRO/devkitA64
    export PATH=$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH
    make TARGET=switch
'
echo "Gotowe: dist/wacki.nro"

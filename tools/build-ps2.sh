#!/usr/bin/env bash
# Cross-build the engine for the PlayStation 2 by running the host
# Makefile inside the ps2dev Docker image (which ships the
# mips64r5900el-ps2-elf toolchain, ps2sdk, gsKit and a prebuilt SDL2-PS2
# under $PS2SDK/ports). Produces dist/wacki-ps2.elf.
#
# Experiment status — Droga A (reuse platform_sdl.c on top of SDL2-PS2,
# which has a real gsKit render backend + ps2 audio + libpad input). A
# minimal SDL2 program (init video+audio, create renderer+streaming
# texture) links into a valid EE ELF in this image; this drives the full
# engine through the same path. Whether it RENDERS is a PCSX2 / hardware
# question.
#
# Local usage:    ./tools/build-ps2.sh   (or: make ps2)
#
# Override the image with PS2_DOCKER_IMAGE if you fork.

set -euo pipefail

cd "$(dirname "$0")/.."

IMAGE="${PS2_DOCKER_IMAGE:-ps2dev/ps2dev:latest}"

if [ ! -f data/WACKI.EXE ]; then
    echo "error: data/WACKI.EXE missing — required for the embedded PE blob." >&2
    echo "       Drop the file in ./data/ before building." >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker not installed — install Docker Desktop / docker.io first." >&2
    exit 1
fi

echo "[ps2] using image: $IMAGE"
docker pull --platform linux/amd64 "$IMAGE" >/dev/null

# If ./data is a symlink (the local-dev layout that keeps the proprietary
# game data outside the repo), Docker won't follow it into the host — the
# link target sits outside the mount. Mount the resolved target too.
extra_mounts=()
if [ -L data ]; then
    data_target="$(cd "$(dirname "$(readlink data)")" && pwd)/$(basename "$(readlink data)")"
    if [ -d "$data_target" ]; then
        extra_mounts+=(-v "$data_target:$data_target")
        echo "[ps2] data/ is a symlink — mounting target $data_target"
    fi
fi

# SDL2-PS2 build flags. The image's mips64r5900el-ps2-elf-pkg-config is a
# wrapper around a host pkg-config that Alpine doesn't ship, so we spell
# the flags out from sdl2.pc + the extra library search paths SDL2-PS2
# needs at link time:
#   ports/lib  → libSDL2, libps2_drivers
#   gsKit/lib  → libgskit, libdmakit, libgskit_toolkit
#   ee/lib     → libaudsrv, libpadx, libmtap, libpatches (ps2sdk core)
PS2DEV=/usr/local/ps2dev
PORTS="$PS2DEV/ps2sdk/ports"
SDL_CFLAGS="-I$PORTS/include -I$PORTS/include/SDL2"
SDL_LIBS="-L$PORTS/lib -L$PS2DEV/gsKit/lib -L$PS2DEV/ps2sdk/ee/lib \
-lSDL2 -lpatches -lgskit -ldmakit -lgskit_toolkit -laudsrv -lpadx -lmtap -lps2_drivers -lm"

# Run the unchanged Makefile inside the container. The ps2dev image is
# Alpine and has no host C compiler / make, so add them first (HOSTCC
# builds embed-pe-data, which runs on the build host, not the PS2).
docker run --rm --platform linux/amd64 \
    -v "$(pwd):/work" -w /work \
    "${extra_mounts[@]}" \
    "$IMAGE" \
    sh -c "
        set -e
        apk add --quiet --no-progress make gcc musl-dev
        # Wipe host-built artefacts so the cross-build doesn't link against
        # leftover x86_64 .o files or a stale generated PE source.
        rm -rf dist src/embedded_wacki_pe.c
        make TARGET=ps2 \
             CROSS_COMPILE=mips64r5900el-ps2-elf- \
             HOSTCC=gcc \
             SDL2_CFG=true \
             SDL_CFG='$SDL_CFLAGS' \
             SDL_LIB='$SDL_LIBS' \
             engine
    "

bin=dist/wacki-ps2.elf
if [ ! -f "$bin" ]; then
    echo "error: $bin not produced" >&2
    exit 1
fi

echo "[ps2] built $bin"
file "$bin" 2>/dev/null || true
ls -lh "$bin"
echo
echo "Run it: copy $bin to a USB stick and launch via uLaunchELF on a"
echo "FreeMcBoot/FreeDVDBoot PS2, or open it in PCSX2. Game data goes on"
echo "USB at mass:/wacki/data/ (see src/data_root.c for the search order)."

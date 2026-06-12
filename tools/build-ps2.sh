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
# -I/tmp/embed: platform_ps2.c #includes the bin2c-generated IRX blobs
# (iomanX/fileXio/cdfs) generated in the container below.
# -lfileXio -lcdvd: the engine's PS2 file I/O (the fileXio backend of the
# storage HAL, in platform_ps2.c) reads through fileXio + CDVD, brought up
# after an IOP reset.
# ps2sdk EE/common includes + -D_EE: platform_ps2.c now pulls in sifrpc.h,
# loadfile.h, libcdvd.h etc. (and ps2sdk's tamtypes.h needs _EE defined).
SDL_CFLAGS="-I$PORTS/include -I$PORTS/include/SDL2 -I/tmp/embed \
-I$PS2DEV/ps2sdk/ee/include -I$PS2DEV/ps2sdk/common/include -I$PS2DEV/gsKit/include -D_EE"
SDL_LIBS="-L$PORTS/lib -L$PS2DEV/gsKit/lib -L$PS2DEV/ps2sdk/ee/lib \
-lSDL2 -lfileXio -lcdvd -lpatches -lgskit -ldmakit -lgskit_toolkit -laudsrv -lpadx -lmtap -lmc -lmouse -lps2_drivers -lm"

# Resolve the build version on the HOST (the container has no git, so the
# Makefile's `git describe` would fall back to "unknown" — which is what the
# menu/build string showed). Pass it into the container's make explicitly.
WACKI_VERSION="$(git describe --tags --always --dirty 2>/dev/null || echo unknown)"
echo "[ps2] version: $WACKI_VERSION"

# Optional video-mode test flags (NTSC 640x448i is the default).
#   WACKI_PS2_PAL=1         → PAL 640x512 interlaced (region-authentic, 50 Hz)
#   WACKI_PS2_PROGRESSIVE=1 → VGA 640x480p (needs a VGA/component display)
if [ "${WACKI_PS2_PAL:-0}" = 1 ]; then
    SDL_CFLAGS="$SDL_CFLAGS -DWACKI_PS2_PAL"
    echo "[ps2] video: PAL 640x512 (test build)"
fi
if [ "${WACKI_PS2_PROGRESSIVE:-0}" = 1 ]; then
    SDL_CFLAGS="$SDL_CFLAGS -DWACKI_PS2_PROGRESSIVE"
    echo "[ps2] video: progressive 640x480 (test build)"
fi
if [ "${WACKI_PS2_576P:-0}" = 1 ]; then
    SDL_CFLAGS="$SDL_CFLAGS -DWACKI_PS2_576P"
    echo "[ps2] video: PAL progressive 640x576 (test build)"
fi

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
        # Embed the IOP fileio modules platform_ps2.c loads at boot.
        mkdir -p /tmp/embed
        bin2c $PS2DEV/ps2sdk/iop/irx/iomanX.irx  /tmp/embed/iomanX_irx.c  iomanX_irx
        bin2c $PS2DEV/ps2sdk/iop/irx/fileXio.irx /tmp/embed/fileXio_irx.c fileXio_irx
        bin2c $PS2DEV/ps2sdk/iop/irx/cdfs.irx    /tmp/embed/cdfs_irx.c    cdfs_irx
        bin2c $PS2DEV/ps2sdk/iop/irx/audsrv.irx  /tmp/embed/audsrv_irx.c  audsrv_irx
        bin2c $PS2DEV/ps2sdk/iop/irx/mcman.irx   /tmp/embed/mcman_irx.c   mcman_irx
        bin2c $PS2DEV/ps2sdk/iop/irx/mcserv.irx  /tmp/embed/mcserv_irx.c  mcserv_irx
        # USB host driver + HID mouse driver (cursor via a USB mouse).
        bin2c $PS2DEV/ps2sdk/iop/irx/usbd.irx     /tmp/embed/usbd_irx.c     usbd_irx
        bin2c $PS2DEV/ps2sdk/iop/irx/ps2mouse.irx /tmp/embed/ps2mouse_irx.c ps2mouse_irx
        # USB mass storage (BDM FAT stack) — load game data off a USB stick on
        # real hardware (mass:/wacki/data/), loaded lazily if host:/cdfs: miss.
        bin2c $PS2DEV/ps2sdk/iop/irx/bdm.irx         /tmp/embed/bdm_irx.c         bdm_irx
        bin2c $PS2DEV/ps2sdk/iop/irx/bdmfs_fatfs.irx /tmp/embed/bdmfs_fatfs_irx.c bdmfs_fatfs_irx
        bin2c $PS2DEV/ps2sdk/iop/irx/usbmass_bd.irx  /tmp/embed/usbmass_bd_irx.c  usbmass_bd_irx
        # Wipe host-built artefacts so the cross-build doesn't link against
        # leftover x86_64 .o files or a stale generated PE source.
        rm -rf dist src/embedded_wacki_pe.c
        make TARGET=ps2 \
             CROSS_COMPILE=mips64r5900el-ps2-elf- \
             HOSTCC=gcc \
             WACKI_VERSION='$WACKI_VERSION' \
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

#!/usr/bin/env bash
# Cross-build the engine for PortMaster handhelds (Anbernic & friends).
# Produces two ELFs under dist/portmaster/:
#
#   wacki.aarch64   64-bit ARM — Allwinner H700 (RG35XX Plus/H/SP/2024,
#                   RG34XX, RG40XX, RG28XX, RG CubeXX), Rockchip RK3566
#                   (RG353x, RG503) / RK3399 (RG552), and most other
#                   PortMaster devices.
#   wacki.armhf     32-bit ARM — original RG35XX (Actions, Cortex-A9)
#                   and older armhf handhelds.
#
# Built inside a NATIVE amd64 Debian bullseye container (glibc 2.31,
# SDL2 2.0.14) using the Debian CROSS toolchains + multiarch SDL2 -dev
# packages — NOT QEMU. The compiler runs at the host's native speed and
# only EMITS ARM code, so a build that used to spend ten-plus minutes per
# arch under arm/v7 (and arm64) user-mode emulation now takes about a
# minute. glibc / SDL2 ABI compatibility is unchanged: we still link
# against bullseye's arm64 / armhf -dev libraries, so the floor stays
# glibc 2.31 / SDL2 2.0.14.
#
# SDL2 is linked DYNAMICALLY on purpose: the binary picks up
# PortMaster's per-device libSDL2 at run time (which is wired to the
# right KMSDRM/fbdev video driver) via the launch script's
# LD_LIBRARY_PATH. SDL2 keeps a stable 2.x ABI, so a 2.0.14 link runs
# fine against PortMaster's newer runtime.
#
# Usage:
#   ./tools/build-portmaster.sh                 # both arches
#   ./tools/build-portmaster.sh aarch64         # one arch
#   WACKI_STRIP=1 ./tools/build-portmaster.sh   # strip (release); default keeps symbols
#
# Requires Docker. No QEMU / binfmt setup needed — the build container is
# amd64 (native on x86_64 CI; Rosetta-accelerated on Apple Silicon, which
# still beats emulating the ARM toolchain itself).

set -euo pipefail
cd "$(dirname "$0")/.."
. tools/lib/common.sh

require_data_exe
require_docker

VER="$(wacki_version)"
OUT="dist/portmaster"
mkdir -p "$OUT"

# Mount the local-dev data/ symlink target so the in-tree symlink resolves
# inside the container (see tools/lib/common.sh).
wacki_data_mount

# build_one <out-arch> <gnu-triplet> <dpkg-arch>
#   out-arch    file suffix pack-portmaster.sh expects (aarch64 / armhf)
#   gnu-triplet CROSS_COMPILE prefix + multiarch lib dir (aarch64-linux-gnu / arm-linux-gnueabihf)
#   dpkg-arch   `dpkg --add-architecture` name (arm64 / armhf)
build_one() {
    arch="$1"; triplet="$2"; dpkg_arch="$3"
    echo "[portmaster] cross-building $arch ($triplet) — native compile, no emulation..."
    # Pin the container to amd64 so the toolchain is the deterministic
    # Debian cross compiler (host-native on x86_64 CI). Arch params go in
    # as env vars so the in-container script stays quote-clean.
    docker run --rm --platform linux/amd64 \
        -e "WACKI_VERSION=$VER" \
        -e "WACKI_STRIP=${WACKI_STRIP:-0}" \
        -e "OUT=$OUT" \
        -e "ARCH=$arch" \
        -e "TRIPLET=$triplet" \
        -e "DPKG_ARCH=$dpkg_arch" \
        "${WACKI_DATA_MOUNT[@]}" \
        -v "$PWD":/src -w /src \
        debian:bullseye-slim sh -euc '
            export DEBIAN_FRONTEND=noninteractive
            dpkg --add-architecture "$DPKG_ARCH"
            apt-get update -qq
            # build-essential = native cc/make for the HOSTCC embed-pe-data tool
            #   (crossbuild-essential only *recommends* it, so --no-install-
            #   recommends would drop it → "make: cc: No such file or directory").
            # crossbuild-essential-<arch> = the $TRIPLET-{gcc,strip} cross toolchain.
            # libsdl2-dev:<arch> = ARM headers + .so + sdl2.pc via multiarch.
            apt-get install -y -qq --no-install-recommends \
                build-essential \
                crossbuild-essential-"$DPKG_ARCH" \
                "libsdl2-dev:$DPKG_ARCH" \
                pkg-config xxd ca-certificates >/dev/null

            # Drop any host-built leftovers so we never link amd64 .o files
            # (and force the PE blob to regenerate via the native HOSTCC tool).
            rm -rf dist/pm-build src/embedded_wacki_pe.c

            # A single /usr/bin/sdl2-config cannot serve two arches, so resolve
            # the cross SDL2 flags through pkg-config against the TARGET
            # multiarch dir. `pkg-config sdl2 --cflags|--libs` is flag-compatible
            # with the sdl2-config calls the Makefile makes, so SDL2_CFG just
            # points at it (no wrapper script needed). PKG_CONFIG_LIBDIR replaces
            # the default search path so no amd64 .pc leaks in.
            export PKG_CONFIG_LIBDIR="/usr/lib/$TRIPLET/pkgconfig:/usr/share/pkgconfig"

            make engine TARGET=portmaster STATIC_SDL2=0 DIST=dist/pm-build \
                 CROSS_COMPILE="$TRIPLET-" \
                 SDL2_CFG="pkg-config sdl2"

            if [ "${WACKI_STRIP:-0}" = 1 ]; then
                echo "[portmaster] WACKI_STRIP=1 — stripping release binary"
                "$TRIPLET-strip" --strip-all dist/pm-build/wacki
            fi
            install -D dist/pm-build/wacki "$OUT/wacki.$ARCH"
            rm -rf dist/pm-build
        '
    file "$OUT/wacki.$arch"
}

case "${1:-all}" in
    aarch64) build_one aarch64 aarch64-linux-gnu   arm64 ;;
    armhf)   build_one armhf   arm-linux-gnueabihf armhf ;;
    all)
        build_one aarch64 aarch64-linux-gnu   arm64
        build_one armhf   arm-linux-gnueabihf armhf
        ;;
    *) echo "usage: $0 [aarch64|armhf|all]" >&2; exit 2 ;;
esac

echo "[portmaster] done → $OUT/"

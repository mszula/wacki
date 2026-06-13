#!/usr/bin/env bash
# Build one ELF per display mode for side-by-side HostFS testing. Each lands
# in dist/wacki-ps2-<mode>.elf; they all share dist/data, so in PCSX2 you
# Boot ELF whichever one you want to compare. build-ps2.sh wipes dist/ each
# run, so stage the ELFs outside dist and collect them at the end.
#
#   ./tools/build-ps2-modes.sh
#
# Modes:
#   ntsc → NTSC 640x448i (default)            pal  → PAL 640x512i (480 1:1 + bars)
#   480p → progressive VGA 640x480 (60 Hz)    576p → PAL progressive 640x576 (50 Hz)

set -euo pipefail
cd "$(dirname "$0")/.."
. tools/lib/common.sh

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

build_mode() {
    local name="$1"; shift
    echo "########## building $name ##########"
    env "$@" ./tools/build-ps2.sh 2>&1 | grep -E '\[ps2\] (version|video|built)|error:' || true
    [ -f dist/wacki-ps2.elf ] || { echo "ERROR: $name build produced no ELF" >&2; exit 1; }
    cp dist/wacki-ps2.elf "$stage/wacki-ps2-$name.elf"
}

build_mode ntsc
build_mode pal  WACKI_PS2_PAL=1
build_mode 480p WACKI_PS2_PROGRESSIVE=1
build_mode 576p WACKI_PS2_576P=1

# Collect the four into dist/ and restore the HostFS data symlink that the
# last build's `rm -rf dist` wiped. Drop the generic ELF to avoid confusion.
mkdir -p dist
rm -f dist/wacki-ps2.elf
mv "$stage"/wacki-ps2-*.elf dist/
if [ -L data ]; then
    ln -sfn "$(wacki_data_dir)" dist/data
fi

echo
echo "[ps2-modes] built 4 ELFs (Boot ELF any of them in PCSX2):"
ls -lh dist/wacki-ps2-*.elf

#!/usr/bin/env bash
# Build a bootable PS2 ISO with the engine + game data baked in, so it
# runs in PCSX2 (or a modded PS2 / FreeDVDBoot) with ZERO HostFS/config
# fiddling — just "Boot ISO" and go. The data is read off the disc via
# cdrom0: (the cdfs:/cdrom0: path rewrite lives in ps2_normalize_path,
# src/platform/ps2/storage_ps2.c).
#
# Layout written to the ISO:
#   SYSTEM.CNF            BOOT2 = cdrom0:\WACK_001.01;1
#   WACK_001.01          = dist/wacki-ps2.elf  (boot ELF named as the serial)
#   DATA/DANE_*.DTA      the original game archives (read via cdrom0:\DATA)
#
# Title ID / serial: WACK-00101. The PS2 boot ELF is conventionally named
# after the disc serial (LLLL_NNN.NN), and PCSX2 / catalogs derive the
# serial from that BOOT2 filename. "WACK" is a custom homebrew code (real
# serials start with S + a Sony-allocated region letter) so it can't clash
# with any released title's database entry.
#
# Usage:    ./tools/build-ps2-iso.sh   (or: make ps2-iso)
#           FORCE_ELF=1 ./tools/build-ps2-iso.sh   # force an ELF rebuild

set -euo pipefail

cd "$(dirname "$0")/.."
. tools/lib/common.sh

IMAGE="${PS2_DOCKER_IMAGE:-ps2dev/ps2dev:latest}"

require_docker

# 1. Ensure the ELF exists (build it if missing or forced).
if [ ! -f dist/wacki-ps2.elf ] || [ "${FORCE_ELF:-0}" = 1 ]; then
    echo "[ps2-iso] building the ELF first…"
    ./tools/build-ps2.sh
fi

# Resolve the real data directory (follow the local-dev symlink so Docker
# can see the archives — see tools/lib/common.sh).
data_dir="$(wacki_data_dir)"
if ! ls "$data_dir"/[Dd][Aa][Nn][Ee]_02.[Dd][Tt][Aa] >/dev/null 2>&1; then
    echo "error: no Dane_02 archive under $data_dir" >&2
    exit 1
fi

# 2. Author the ISO in the container (genisoimage from cdrkit). The data
#    dir is grafted in read-only — no 370 MB copy. SYSTEM.CNF needs CRLF.
docker run --rm --platform linux/amd64 \
    -v "$(pwd):/work" -w /work \
    -v "$data_dir:/data_src:ro" \
    "$IMAGE" \
    sh -c '
        set -e
        apk add --quiet --no-progress cdrkit >/dev/null
        printf "BOOT2 = cdrom0:\\WACK_001.01;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n" > /tmp/SYSTEM.CNF
        genisoimage -quiet -iso-level 2 -l \
            -sysid PLAYSTATION -V WACKI \
            -o dist/wacki-ps2.iso \
            -graft-points \
            SYSTEM.CNF=/tmp/SYSTEM.CNF \
            WACK_001.01=dist/wacki-ps2.elf \
            DATA/=/data_src/
    '

iso=dist/wacki-ps2.iso
if [ ! -f "$iso" ]; then
    echo "error: $iso not produced" >&2
    exit 1
fi

echo
echo "[ps2-iso] built $iso"
ls -lh "$iso"
echo
echo "Run it: PCSX2 → Boot ISO (or drag the .iso onto the window). No"
echo "HostFS, no config — the data is on the disc. Watch the PCSX2 console"
echo "for a '[data-root] matched on cdrom0:\\DATA' line."

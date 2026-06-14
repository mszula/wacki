#!/usr/bin/env bash
# Assemble the PlayStation 2 release .zip. Run tools/build-ps2.sh first to
# produce the boot ELF (dist/wacki-ps2.elf).
#
# Zip layout — a single `wacki/` folder the player drops onto a USB stick so
# the on-disk paths line up with the engine's USB data-root search
# (mass:/wacki/data/, see src/data_root.c / src/platform/ps2/storage_ps2.c):
#
#   wacki-ps2.zip
#   ├── README.txt              how to run (real HW via uLaunchELF, or PCSX2)
#   └── wacki/
#       ├── wacki-ps2.elf       the boot ELF
#       └── data/
#           └── README.txt      drop Dane_*.dta here
#
# Unlike the handheld packages there's no launcher script — the PS2 has no
# standard "ports" menu; the user boots the ELF via uLaunchELF (real HW) or
# "Boot ELF" (PCSX2). A self-contained bootable ISO (data baked in) is a
# separate, LOCAL-only artefact — tools/build-ps2-iso.sh — and is NOT shippable
# because it would embed the copyrighted Dane_*.dta.
#
# Usage: ./tools/pack-ps2.sh [dist/wacki-ps2.zip]

set -euo pipefail
cd "$(dirname "$0")/.."

elf="dist/wacki-ps2.elf"
if [ ! -f "$elf" ]; then
    echo "error: $elf missing — run ./tools/build-ps2.sh first." >&2
    exit 1
fi

out="${1:-dist/wacki-ps2.zip}"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

game="$stage/wacki"
mkdir -p "$game/data"
cp "$elf" "$game/wacki-ps2.elf"

# ---- data drop-off note -------------------------------------------------
cat > "$game/data/README.txt" <<'EOF'
Skopiuj tutaj wszystkie pliki Dane_*.dta z oryginalnej plyty CD
(Dane_01, Dane_02, Dane_10..14, Dane_21/22, 30/31/32, 40/41/42,
50/51/52). Na prawdziwym PS2 caly folder "wacki" wrzucasz na pendrive,
zeby silnik znalazl dane pod mass:/wacki/data/.
EOF

# ---- top-level README ---------------------------------------------------
cat > "$stage/README.txt" <<'EOF'
Wacki: Kosmiczna rozgrywka — port na PlayStation 2
==================================================

Natywna reimplementacja w C/SDL2 polskiej przygodowki point-and-click
z 1998 roku (Seven Stars Multimedia). Renderowanie sprzetowe gsKit,
dzwiek przez audsrv, sterowanie DualShockiem (lewa galka = kursor) lub
mysza USB.

WYMAGANE PLIKI GRY
------------------
Port to wylacznie silnik — nie zawiera materialow chronionych prawem
autorskim. Potrzebujesz wlasnej kopii oryginalnej plyty i plikow
Dane_*.dta z niej. Wrzuc je do folderu wacki/data/ (patrz nizej).

URUCHOMIENIE — prawdziwy PS2 (FreeMcBoot / FreeDVDBoot + uLaunchELF)
-------------------------------------------------------------------
1. Skopiuj caly folder "wacki" z tego archiwum na pendrive (FAT32),
   tak by powstalo:  <pendrive>/wacki/wacki-ps2.elf
                     <pendrive>/wacki/data/Dane_*.dta
2. Wloz pendrive do PS2, odpal uLaunchELF i uruchom
   mass:/wacki/wacki-ps2.elf.

URUCHOMIENIE — emulator PCSX2
-----------------------------
- Szybko: System -> Boot ELF -> wskaz wacki-ps2.elf. Dane podajesz przez
  HostFS albo zbuduj bootowalny obraz ISO (tools/build-ps2-iso.sh w repo).

STEROWANIE
----------
  Lewa galka / mysz USB ... ruch kursora
  X .................. chodzenie / interakcja (lewy klik)
  Kolko .............. zmiana postaci (prawy klik)
  Start .............. menu pauzy

Port: Mateusz Szula — https://github.com/mszula/wacki (GPLv3)
EOF

# ---- zip ----------------------------------------------------------------
out_abs="$(cd "$(dirname "$out")" && pwd)/$(basename "$out")"
rm -f "$out_abs"
( cd "$stage" && zip -rq "$out_abs" . )

echo "[ps2] built $out"
unzip -l "$out" | sed -n '4,$p'
echo "size: $(du -h "$out" | cut -f1)"

#!/usr/bin/env bash
# Stage the Miyoo build into the OnionOS Ports package layout
# documented at https://onionui.github.io/docs/ports.
#
# Output layout (zipped):
#
#   Roms/
#   └── PORTS/
#       ├── Games/Wacki/
#       │   ├── wacki                      cross-built engine binary
#       │   ├── data/                      user drops Dane_*.dta here
#       │   └── _required_files.txt        what the user must provide
#       ├── Imgs/Wacki.png                 launcher icon (optional)
#       └── Shortcuts/Adventure/Wacki.notfound   menu entry → calls launch_standalone.sh
#
# The whole tree is meant to be extracted at the SD-card root and
# merged into the existing Roms/ folder — that is OnionOS' install
# convention for ports.
#
# Usage:  ./tools/pack-miyoo.sh [output.zip]
#         Default output: dist/wacki-miyoo.zip

set -euo pipefail

cd "$(dirname "$0")/.."

bin=dist/wacki-miyoo
if [ ! -f "$bin" ]; then
    echo "error: $bin not built — run 'make miyoo' first." >&2
    exit 1
fi

out="${1:-dist/wacki-miyoo.zip}"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

# Display name shown in the OnionOS Ports menu. OnionOS labels a port by
# its Shortcut filename stem and matches the box-art icon by that same
# stem, so the shortcut file, the Imgs/ PNG and GameName all share it.
# The full title is "Wacki: Kosmiczna rozgrywka", but ':' is illegal in a
# FAT32/exFAT filename (and the label IS the filename here), so the menu
# drops the colon. GameDir / GameExecutable stay "Wacki" / "wacki" —
# those are real on-card paths and must not change.
disp="Wacki Kosmiczna rozgrywka"

# ---- layout under stage/ -------------------------------------------------
games_dir="$stage/Roms/PORTS/Games/Wacki"
imgs_dir="$stage/Roms/PORTS/Imgs"
shortcuts_dir="$stage/Roms/PORTS/Shortcuts/Adventure"

mkdir -p "$games_dir/data" "$imgs_dir" "$shortcuts_dir"

# Binary lands at <GameDir>/<GameExecutable> — OnionOS' launch_standalone.sh
# does `cd "$GameDir"; HOME="$GameDir"` before invoking it, so the engine
# finds ./data/ adjacent to argv[0] via SDL_GetBasePath fallback.
cp "$bin" "$games_dir/wacki"
chmod +x "$games_dir/wacki"

# Tell the user which files they have to provide. OnionOS' Package
# Manager surfaces this list in the "required files" UI.
#
# Full list of Dane_NN.dta files the engine references at runtime —
# determined by grep over the source plus the per-stage SceneDef
# table. Skipping any of these triggers intermittent
# "Brak takiego pliku w bazie Dane_NN.dta" failures: menu music
# (01), per-stage intros + finale + credits (10..14), and the
# per-stage alt archives (21..52, two per stage 1..5).
#
# Files NOT listed: Dane_03..09 don't exist on the original CD —
# the engine never references them.
cat > "$games_dir/_required_files.txt" <<'EOF'
data/Dane_01.dta
data/Dane_02.dta
data/Dane_10.dta
data/Dane_11.dta
data/Dane_12.dta
data/Dane_13.dta
data/Dane_14.dta
data/Dane_21.dta
data/Dane_22.dta
data/Dane_30.dta
data/Dane_31.dta
data/Dane_32.dta
data/Dane_40.dta
data/Dane_41.dta
data/Dane_42.dta
data/Dane_50.dta
data/Dane_51.dta
data/Dane_52.dta
EOF

# Optional launcher icon. Drop a 250×250 PNG at assets/icons/wacki-miyoo.png
# to embed it; otherwise the entry shows OnionOS' default port icon.
if [ -f assets/icons/wacki-miyoo.png ]; then
    cp assets/icons/wacki-miyoo.png "$imgs_dir/$disp.png"
fi

# Shortcut script — sits under Shortcuts/<Category>/<Name>.notfound and
# delegates to OnionOS' shared launch_standalone.sh. Filename starts as
# .notfound; first time GameDataFile is detected the system renames it
# to .port. Category folder controls grouping in the Ports menu.
cat > "$shortcuts_dir/$disp.notfound" <<'SHORTCUT'
#!/bin/sh
# Standalone port shortcut — calls OnionOS' shared launcher.

GameName="Wacki Kosmiczna rozgrywka"
GameDir="Wacki"
GameExecutable="wacki"

# Probe file the launcher checks under <GameDir>/ (max-depth 2) before
# starting; if absent OnionOS shows a "missing data" toast instead of
# launching a binary that would crash on fopen. Dane_02.dta is the
# canonical probe the engine itself uses to confirm the data root.
GameDataFile="Dane_02.dta"

# The engine drives SDL_AudioDevice directly, so OnionOS' shared
# audioserver has to be released before launch.
KillAudioserver=1

# 640x480 8-bpp + 22 kHz mixer is light — no CPU governor bump needed.
PerformanceMode=0

Arguments=""

/mnt/SDCARD/Emu/PORTS/launch_standalone.sh \
    "$GameName" "$GameDir" "$GameExecutable" \
    "$Arguments" "$GameDataFile" \
    "$KillAudioserver" "$PerformanceMode"
SHORTCUT
chmod +x "$shortcuts_dir/$disp.notfound"

# Short install README at the archive root so the user knows what to
# do with the zip even without reaching the project README.
cat > "$stage/README.txt" <<'README'
Wacki — port silnika dla Miyoo Mini Plus (i pin-kompatybilnych).

Instalacja (OnionOS):
  1. Rozpakuj zawartosc tego archiwum w glownym katalogu karty SD.
     Folder Roms/ z archiwum powinien zostac scalony z istniejacym
     Roms/ na karcie.
  2. Skopiuj pliki Dane_*.dta z oryginalnej plyty do:
        Roms/PORTS/Games/Wacki/data/
  3. Wlacz konsole. W menu Ports → Adventure → Wacki Kosmiczna rozgrywka.

Wymaga OnionOS 4.2 lub nowszego.
README

# Zip from inside stage/ so the archive's top entries are Roms/ and
# README.txt — extracting at SD root then merges Roms/ correctly.
mkdir -p "$(dirname "$out")"
# Resolve to an absolute path before chdir'ing into stage/.
case "$out" in
    /*) out_abs="$out" ;;
    *)  out_abs="$PWD/$out" ;;
esac
# Drop any pre-existing zip so we don't append into a stale archive
# (zip merges by default; that'd leave old layout entries behind).
rm -f "$out_abs"
(cd "$stage" && zip -rXq "$out_abs" Roms README.txt)

echo "[miyoo] packed → $out"
unzip -l "$out"
ls -lh "$out"

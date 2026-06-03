#!/usr/bin/env bash
# Wrap a freshly-built dist/wacki Mach-O binary in a double-clickable
# Wacki.app bundle. Without the bundle, Finder treats the CLI binary
# as a "Unix executable" and spawns Terminal.app every time the user
# double-clicks it — the average player doesn't want to see a shell
# behind their adventure game.
#
# Output:
#   Wacki.app/
#   └── Contents/
#       ├── Info.plist            — bundle metadata (icon, name, id)
#       ├── MacOS/
#       │   └── Wacki              — launcher shell script (entry point)
#       └── Resources/
#           ├── wacki              — the real Mach-O binary
#           ├── data/              — user drops Dane_*.dta here
#           │   └── README.txt
#           └── wacki.icns         — Finder + Dock icon
#
# Note the binary lives under Resources/ rather than MacOS/ to dodge
# the case-insensitivity gotcha: the launcher script in MacOS/ has to
# match CFBundleExecutable ("Wacki", capital W) and APFS would treat
# MacOS/Wacki + MacOS/wacki as the same path.
#
# Launcher cd's into Resources/ before exec'ing the binary so the
# engine's `./data/` lookup finds the user's Dane_*.dta. Finder
# launches CFBundleExecutable directly, no shell visible.
#
# Usage:
#   ./tools/pack-macos-app.sh dist/wacki Wacki.app
#   ./tools/pack-macos-app.sh                          # defaults

set -euo pipefail

BIN="${1:-dist/wacki}"
APP="${2:-dist/Wacki.app}"

if [ ! -x "$BIN" ]; then
    echo "error: $BIN missing — run \`make engine\` first." >&2
    exit 1
fi

if [ ! -f assets/icons/wacki.icns ]; then
    echo "error: assets/icons/wacki.icns missing." >&2
    exit 1
fi

# Clean previous bundle if any.
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/data"

# ---- Binary + launcher ---------------------------------------------------
cp "$BIN" "$APP/Contents/Resources/wacki"
chmod +x "$APP/Contents/Resources/wacki"

cat > "$APP/Contents/MacOS/Wacki" <<'LAUNCHER'
#!/bin/sh
# Wacki.app launcher — pivots cwd into Resources/ so the engine's
# stock ./data/ lookup finds the user-provided Dane_*.dta archives,
# then execs the bundled Mach-O sitting next to data/. Finder runs
# this script via CFBundleExecutable; the user never sees a Terminal
# window.
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE/Resources"
exec ./wacki "$@"
LAUNCHER
chmod +x "$APP/Contents/MacOS/Wacki"

# ---- Icon ----------------------------------------------------------------
cp assets/icons/wacki.icns "$APP/Contents/Resources/wacki.icns"

# ---- Info.plist ---------------------------------------------------------
#
# CFBundleExecutable = "Wacki"  → the launcher script above
# CFBundleIconFile   = "wacki"  → Finder appends .icns; needs to match
# LSMinimumSystemVersion        → arm64 macOS started at 11.0 (Big Sur)
# NSHighResolutionCapable       → use the @2x icons in the .icns
# LSApplicationCategoryType     → Adventure Games — Launchpad grouping
WACKI_VERSION="${WACKI_VERSION:-$(git describe --tags --always --dirty 2>/dev/null || echo dev)}"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>     <string>pl</string>
  <key>CFBundleExecutable</key>            <string>Wacki</string>
  <key>CFBundleIconFile</key>              <string>wacki</string>
  <key>CFBundleIdentifier</key>            <string>com.mszula.wacki</string>
  <key>CFBundleInfoDictionaryVersion</key> <string>6.0</string>
  <key>CFBundleName</key>                  <string>Wacki</string>
  <key>CFBundleDisplayName</key>           <string>Wacki: Kosmiczna rozgrywka</string>
  <key>CFBundlePackageType</key>           <string>APPL</string>
  <key>CFBundleShortVersionString</key>    <string>${WACKI_VERSION}</string>
  <key>CFBundleVersion</key>               <string>${WACKI_VERSION}</string>
  <key>CFBundleSignature</key>             <string>WACK</string>
  <key>LSApplicationCategoryType</key>     <string>public.app-category.adventure-games</string>
  <key>LSMinimumSystemVersion</key>        <string>11.0</string>
  <key>NSHighResolutionCapable</key>       <true/>
  <key>NSHumanReadableCopyright</key>      <string>© 2026 Mateusz Szuła • GPLv3</string>
</dict>
</plist>
PLIST

# ---- Data folder README -------------------------------------------------
cat > "$APP/Contents/Resources/data/README.txt" <<'EOF'
Wrzuć tutaj wszystkie pliki Dane_*.dta z oryginalnej płyty CD.

Po dodaniu możesz dwuklikiem uruchomić Wacki.app — silnik szuka
katalogu data/ obok binarki i znajdzie go tutaj.

Lista wymaganych plików:
  Dane_01.dta — muzyka menu
  Dane_02.dta — archiwum bazowe
  Dane_10.dta — intro AVI
  Dane_11.dta — finale intro
  Dane_12.dta — napisy końcowe AVI
  Dane_13.dta, Dane_14.dta — intra rozdziałów
  Dane_21..52  — per-stage archiwa (2 na etap, etapy 1..5)
EOF

# ---- Strip extended attributes so Gatekeeper doesn't quarantine ---------
# Useful if the bundle was assembled from files with com.apple.provenance
# or other xattrs picked up during git clone / curl etc.
xattr -cr "$APP" 2>/dev/null || true

echo "[macos-app] built $APP"
du -sh "$APP"

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
#       │   └── Wacki              — the real Mach-O binary (entry point)
#       └── Resources/
#           ├── data/              — user drops Dane_*.dta here
#           │   └── README.txt
#           └── wacki.icns         — Finder + Dock icon
#
# The Mach-O lives directly in MacOS/ as CFBundleExecutable. That's
# what makes [NSBundle mainBundle] resolve to the bundle, so SDL reads
# CFBundleName ("Wacki") for the menu-bar title — an earlier layout
# that exec'd the binary from Resources/ via a launcher script left
# mainBundle unresolved, and the menu fell back to the lowercase
# process name. No launcher is needed: the engine's FindDataRoot probes
# SDL_GetBasePath() (= Contents/Resources for a bundled app) and finds
# Resources/data/ there, so the old cwd-pivot hack is gone.
#
# There's only one file named "Wacki" in MacOS/, so the APFS
# case-insensitivity concern (MacOS/Wacki vs a second MacOS/wacki) does
# not arise.
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

# ---- Binary --------------------------------------------------------------
# The Mach-O is the bundle's CFBundleExecutable. Naming it "Wacki"
# (capital W) to match the plist is what lets NSBundle/SDL pick up
# CFBundleName for the menu-bar title.
cp "$BIN" "$APP/Contents/MacOS/Wacki"
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

# ---- Ad-hoc code signature ----------------------------------------------
# We're not in the Apple Developer Program, so we can't notarize — the
# Gatekeeper "unidentified developer" prompt is unavoidable (the README
# walks users through System Settings → Open Anyway / the xattr
# one-liner). But an ad-hoc signature still matters:
#   - arm64 Mach-O binaries MUST carry at least an ad-hoc signature or
#     the kernel refuses to exec them ("killed: 9" / "is damaged");
#     the linker applies one but `strip` (CI) can invalidate it.
#   - keeps the whole bundle's hashes internally consistent so the
#     one-time right-click-Open / xattr bypass sticks.
# --deep covers the bundle's Mach-O in MacOS/; --force overwrites the
# linker-applied signature. -s - is the ad-hoc identity.
if command -v codesign >/dev/null 2>&1; then
    codesign --force --deep -s - "$APP" 2>/dev/null \
        && echo "[macos-app] ad-hoc signed" \
        || echo "[macos-app] codesign failed (non-fatal)"
fi

# NOTE: we deliberately do NOT ship a "double-click to unblock"
# .command helper. It would be subject to the SAME Gatekeeper
# quarantine as the .app — Sequoia hard-blocks unsigned quarantined
# scripts, so the helper meant to fix the block is itself blocked.
# The README points users at System Settings → Open Anyway (pure
# GUI, no pre-step) and the Terminal xattr one-liner instead.

echo "[macos-app] built $APP"
du -sh "$APP"

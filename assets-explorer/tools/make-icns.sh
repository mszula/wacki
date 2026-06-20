#!/bin/sh
# make-icns.sh OUT.icns — build a macOS .icns from icon-source.png.
#
# Best-effort: if the source art or the macOS tools (sips/iconutil) are absent
# we skip silently and the .app just gets the system's generic icon. The window
# icon is set at runtime from the embedded PNG regardless, so this only affects
# the Finder/Dock icon.
set -eu

cd "$(dirname "$0")/.."        # assets-explorer/
out=$1

# Prefer the local high-res art (icon-source.png, gitignored); fall back to the
# committed 512px tools/app-icon.png so CI and fresh clones still get an icon.
if   [ -f icon-source.png ];    then src=icon-source.png
elif [ -f tools/app-icon.png ]; then src=tools/app-icon.png
else echo "make-icns: no icon source found — skipping app icon"; exit 0
fi
command -v sips     >/dev/null 2>&1 || { echo "make-icns: sips not found — skipping";     exit 0; }
command -v iconutil >/dev/null 2>&1 || { echo "make-icns: iconutil not found — skipping"; exit 0; }

tmp=$(mktemp -d)
set="$tmp/AppIcon.iconset"
mkdir -p "$set"

# Standard iconset: each size at 1x and 2x (Retina).
for s in 16 32 128 256 512; do
	d=$((s * 2))
	sips -z "$s" "$s" "$src" --out "$set/icon_${s}x${s}.png"     >/dev/null
	sips -z "$d" "$d" "$src" --out "$set/icon_${s}x${s}@2x.png"  >/dev/null
done

iconutil -c icns "$set" -o "$out"
rm -rf "$tmp"
echo "make-icns: wrote $out"

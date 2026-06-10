#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Generate the PS2 memory-card save-icon texture from the Windows .ico.
#
# The PS2 BIOS browser renders a save's 3D icon (icon.ico) with a 128x128
# texture in BGR555 (bit layout: X BBBBB GGGGG RRRRR, little-endian u16).
# This bakes assets/icons/wacki.ico down to that and emits a C array that
# platform_ps2.c embeds + writes to the card. Re-run when the art changes.
#
#   python3 tools/gen-ps2-icon.py
#
import os
from PIL import Image, ImageChops

# The PS2/PCSX2 icon renderer samples the icon texture with a constant
# horizontal offset (~5 px on a 128-wide texture: the image appears
# shifted right, the right edge wrapping to the left). Pre-roll the source
# left by the same amount so it lands correctly. Tune if it's still off.
ROLL_X = 5

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "assets/icons/wacki.ico")
OUT  = os.path.join(ROOT, "src/ps2_icon_tex.inc")

im = Image.open(SRC)
# Use the largest frame in the .ico, then box-filter down to 128x128.
try:
    im = im.ico.getimage(max(im.ico.sizes()))
except Exception:
    pass
im = im.convert("RGBA")
bg = Image.new("RGBA", im.size, (0, 0, 0, 255))          # flatten any alpha
im = Image.alpha_composite(bg, im).convert("RGB").resize((128, 128), Image.LANCZOS)
# The PS2 icon quad samples with V=0 at the bottom, so pre-flip vertically.
im = im.transpose(Image.FLIP_TOP_BOTTOM)
# Compensate the renderer's horizontal sampling offset (cyclic roll left).
if ROLL_X:
    im = ImageChops.offset(im, -ROLL_X, 0)

px = im.load()
vals = []
for y in range(128):
    for x in range(128):
        r, g, b = px[x, y]
        vals.append(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3))

with open(OUT, "w") as f:
    f.write("/* GENERATED from assets/icons/wacki.ico by tools/gen-ps2-icon.py.\n")
    f.write(" * 128x128 BGR555 (X BBBBB GGGGG RRRRR) PS2 save-icon texture. */\n")
    f.write("static const unsigned short s_wacki_icon_tex[128 * 128] = {\n")
    for i in range(0, len(vals), 12):
        f.write("  " + ",".join("0x%04x" % v for v in vals[i:i+12]) + ",\n")
    f.write("};\n")

print("wrote", OUT)

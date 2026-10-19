#!/usr/bin/env python3
"""Write a one-page vector A4 test PDF: testpage.py out.pdf"""
import sys

W, H = 595.28, 841.89
M = 12  # printer margin, pt


def text(x, y, size, s, font="F1"):
    s = s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")
    return f"BT /{font} {size} Tf {x:.2f} {y:.2f} Td ({s}) Tj ET\n"


ops = []
# imageable-area frame (0.5 pt) and corner ticks
ops.append(f"0 G 0.5 w {M + 1} {M + 1} {W - 2 * M - 2:.2f} {H - 2 * M - 2:.2f} re S\n")
for cx, cy in ((M + 1, M + 1), (W - M - 1, M + 1), (M + 1, H - M - 1), (W - M - 1, H - M - 1)):
    ops.append(f"2 w {cx - 0:.2f} {cy:.2f} m {cx + (20 if cx < W / 2 else -20):.2f} {cy:.2f} l S\n")
    ops.append(f"{cx:.2f} {cy - 0:.2f} m {cx:.2f} {cy + (20 if cy < H / 2 else -20):.2f} l S\n")

ops.append(text(50, 780, 22, "HP LaserJet 1010 - FastRaster driver test"))
ops.append(text(50, 758, 11, "rastertolj1010 for macOS  |  600 dpi  |  A4  |  DeltaPlus bands of 128 rows"))

# gray ramp 0..100 %
ops.append(text(50, 720, 10, "Gray ramp, 0% .. 100% black in 10% steps:"))
for i in range(11):
    g = 1 - i / 10
    x = 50 + i * 45
    ops.append(f"{g:.2f} g {x} 650 42 60 re f 0 g\n")
    ops.append(text(x + 10, 638, 8, f"{i * 10}%"))

# line weights
ops.append(text(50, 605, 10, "Line weights 0.1 / 0.25 / 0.5 / 1 / 2 pt:"))
for i, lw in enumerate((0.1, 0.25, 0.5, 1, 2)):
    y = 590 - i * 12
    ops.append(f"0 G {lw} w 50 {y} m 545 {y} l S\n")

# radial fan (aliasing / dithering check)
cx, cy = 150, 420
import math
for a in range(0, 360, 6):
    r = math.radians(a)
    ops.append(f"0.3 w {cx} {cy} m {cx + 90 * math.cos(r):.2f} {cy + 90 * math.sin(r):.2f} l S\n")

# smooth gradient made of thin strips
for i in range(200):
    g = 1 - i / 199
    ops.append(f"{g:.3f} g {300 + i * 1.2:.2f} 340 1.25 160 re f\n")
ops.append("0 g\n")
ops.append(text(300, 325, 8, "continuous gradient (error diffusion)"))

# text sizes
y = 290
for size in (4, 5, 6, 8, 10, 12, 14, 18):
    ops.append(text(50, y, size, f"{size} pt  The quick brown fox jumps over the lazy dog 0123456789"))
    y -= size + 6

# checkerboard 1 px-ish fine pattern
for i in range(40):
    for j in range(10):
        if (i + j) % 2 == 0:
            ops.append(f"{300 + i * 0.96:.2f} {120 + j * 0.96:.2f} 0.96 0.96 re f\n")
ops.append(text(300, 108, 8, "fine checker (approx. 8 px cells)"))

# bottom edge markers: last rows of the imageable area must print
ops.append(text(200, M + 6, 9, "bottom edge: this line must be visible"))
ops.append(f"0 g {W / 2 - 60:.2f} {M + 1} 120 3 re f\n")

content = "".join(ops).encode()
objs = [
    b"<< /Type /Catalog /Pages 2 0 R >>",
    b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    (f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {W} {H}] "
     f"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>").encode(),
    b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
]
out = bytearray(b"%PDF-1.4\n")
offsets = []
for i, o in enumerate(objs, 1):
    offsets.append(len(out))
    out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
xref = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for off in offsets:
    out += b"%010d 00000 n \n" % off
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
open(sys.argv[1], "wb").write(out)

#!/usr/bin/env python3
"""Decode an HP LaserJet 1010 FastRaster stream (rastertolj1010 / hpcups output).

Validates the PJL/PCL XL framing, decompresses every DeltaPlus band and writes
one PNG per page:  frdecode.py job.fr out_prefix
"""
import struct
import sys

from PIL import Image

UEL = b"\x1b%-12345X"
FR_BAND = 0x68702006
HT_DIST = 16

# PCL XL data type tag -> payload size (little-endian binding)
SCALARS = {0xC0: 1, 0xC1: 2, 0xC2: 4, 0xC3: 2, 0xC4: 4, 0xC5: 4,
           0xD0: 2, 0xD1: 4, 0xD2: 8, 0xD3: 4, 0xD4: 8, 0xD5: 8}
OPS = {0x41: "BeginSession", 0x42: "EndSession", 0x43: "BeginPage", 0x44: "EndPage",
       0x46: "VendorUnique", 0x48: "OpenDataSource", 0x49: "CloseDataSource",
       0x6A: "SetColorSpace", 0x6B: "SetCursor", 0x75: "SetPageOrigin", 0x7B: "SetROP"}


def read_count(data, pos, count, over):
    if count < over:
        return count, pos
    b = data[pos]; pos += 1
    count += b
    if b == 0xFE:
        count += data[pos]; pos += 1
    elif b == 0xFF:
        count += (data[pos] << 8) | data[pos + 1]; pos += 2
    return count, pos


def deltaplus_decode(data, row_width, rows):
    """Inverse of HPLIP's ModeDeltaPlus::compress (horz_ht_dist = 16)."""
    out = bytearray(row_width * rows)
    seed = bytes(row_width)
    pos = 0
    new_color = 0xFF
    for r in range(rows):
        cur = memoryview(out)[r * row_width:(r + 1) * row_width]
        col = 0
        while col < row_width:
            cmd = data[pos]; pos += 1
            isrun, loc = cmd >> 7, (cmd >> 5) & 3
            sc, pos = read_count(data, pos, (cmd >> 3) & 3, 3)
            if loc == 0:
                new_color = data[pos]; pos += 1
            cnt, pos = read_count(data, pos, cmd & 7, 7)
            assert col + sc <= row_width, f"seed copy past row end (row {r})"
            cur[col:col + sc] = seed[col:col + sc]
            col += sc
            if col == row_width:
                break                       # "fake run" closing the row
            first = (new_color if loc in (0, 3) else
                     seed[col + 1] if loc == 1 else seed[col - 1])
            cur[col] = first
            col += 1
            if isrun:
                for _ in range(cnt + 1):
                    assert col >= HT_DIST, "run before halftone distance"
                    cur[col] = cur[col - HT_DIST]
                    col += 1
            else:
                cur[col:col + cnt] = data[pos:pos + cnt]
                pos += cnt
                col += cnt
            assert col <= row_width, f"command overruns row {r}"
        seed = bytes(cur)
    assert pos == len(data), f"{len(data) - pos} trailing bytes in band"
    return bytes(out)


def main(path, prefix):
    data = open(path, "rb").read()
    assert data.startswith(b"\x1bE" + UEL), "missing reset + UEL"
    pos = 2 + len(UEL)
    end_pjl = data.index(b") HP-PCL XL;2;0;Comment\n", pos)
    for line in data[pos:end_pjl].decode().splitlines():
        print("PJL ", line)
    pos = end_pjl + len(b") HP-PCL XL;2;0;Comment\n")

    attrs, stack = {}, []
    pages, page, bands = [], None, 0
    while True:
        tag = data[pos]; pos += 1
        if tag in SCALARS:
            n = SCALARS[tag]
            stack.append((tag, data[pos:pos + n])); pos += n
        elif tag == 0xF8:
            attrs[data[pos]] = stack.pop(); pos += 1
        elif tag in OPS:
            op = OPS[tag]
            if op == "VendorUnique":
                ext = struct.unpack("<I", attrs[0x91][1])[0]
                if 0x92 in attrs:
                    n = struct.unpack("<I", attrs[0x92][1])[0]
                    payload = data[pos:pos + n]; pos += n
                    assert ext == FR_BAND, hex(ext)
                    h = payload[:25]
                    res, comp = struct.unpack(">HH", h[2:6])
                    size = struct.unpack(">I", h[12:16])[0]
                    width, height = struct.unpack(">HH", h[16:20])
                    y = struct.unpack(">H", h[22:24])[0]
                    assert (res, h[6], h[24]) == (600, 3, 1), h.hex()
                    assert size == n - 25, "band size field mismatch"
                    rw = width // 8
                    body = payload[25:]
                    pix = deltaplus_decode(body, rw, height) if comp == 24 else bytes(body)
                    assert comp in (2, 24) and len(pix) == rw * height
                    page["bands"].append((y, height, rw, pix))
                    bands += 1
                else:
                    print(f"PCLXL VendorUnique ext=0x{ext:08x}")
            elif op == "BeginPage":
                ms = attrs.get(0x25)
                cms = attrs.get(0x2F)
                page = {"media": ms[1][0] if ms else
                        "custom %.2fx%.2fin" % struct.unpack("<ff", cms[1]), "bands": []}
            elif op == "EndPage":
                pages.append(page); page = None
            elif op == "EndSession":
                break
            if op != "VendorUnique":
                print("PCLXL", op, {hex(k): v[1].hex() for k, v in attrs.items()})
            attrs, stack = {}, []
        else:
            raise SystemExit(f"unknown PCL XL tag 0x{tag:02x} at {pos - 1}")
    assert data[pos:] == UEL + b"@PJL EOJ\n" + UEL, f"bad trailer {data[pos:]!r}"

    for i, pg in enumerate(pages, 1):
        rw = pg["bands"][0][2]
        ys = [y for y, *_ in pg["bands"]]
        assert ys == list(range(0, 128 * len(ys), 128)), f"band offsets {ys[:5]}..."
        height = max(y + h for y, h, *_ in pg["bands"])
        # HPLIP declares 8x the real width (rows of W bytes, pixels only in the
        # first W/8); rastertolj1010 declares the real width. Keep real pixels.
        stride = (rw + 7) // 8 if rw * 8 > 20000 else rw
        canvas = bytearray(b"\xff" * (stride * height))
        for y, h, brw, pix in pg["bands"]:
            for r in range(h):
                row = pix[r * brw:r * brw + stride]
                canvas[(y + r) * stride:(y + r + 1) * stride] = bytes(b ^ 0xFF for b in row)
        img = Image.frombytes("1", (stride * 8, height), bytes(canvas))
        tail = bytes(b for _, h, brw, pix in pg["bands"] for r in range(h)
                     for b in pix[r * brw + stride:(r + 1) * brw])
        assert not any(tail), "data beyond the declared real width"
        out = f"{prefix}-p{i}.png"
        img.save(out)
        black = sum(bin(b ^ 0xFF).count("1") for b in canvas)
        print(f"page {i}: media={pg['media']} {stride * 8}x{height} px, {len(pg['bands'])} bands, "
              f"{black / (stride * 8 * height) * 100:.2f}% black -> {out}")
    print(f"OK: {len(pages)} page(s), {bands} band(s), stream fully consumed")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

"""Convert the 24-bit BMP written by `sbr64_pc.exe --screenshot` to a PNG (stdlib only).

Usage: python tools/bmp2png.py shot.bmp [shot.png]
"""
import struct
import sys
import zlib


def convert(src, dst):
    d = open(src, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(h - 1, -1, -1):  # BMP rows are stored bottom-up
        bgr = d[off + y * stride: off + y * stride + w * 3]
        rgb = bytearray(len(bgr))
        rgb[0::3], rgb[1::3], rgb[2::3] = bgr[2::3], bgr[1::3], bgr[0::3]
        rows.append(b"\x00" + bytes(rgb))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(b"".join(rows), 6))
           + chunk(b"IEND", b""))
    open(dst, "wb").write(png)
    return w, h


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1].rsplit(".", 1)[0] + ".png"
    print("%dx%d -> %s" % (*convert(sys.argv[1], out), out))

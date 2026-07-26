#!/usr/bin/env python3
"""Convert a binary PPM (P6), as produced by QEMU's screendump, to a PNG.

Written from scratch with zlib + struct so the repo needs no image library.
"""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        def token():
            tok = b""
            while True:
                c = f.read(1)
                if not c:
                    raise ValueError("unexpected end of PPM header")
                if c.isspace():
                    if tok:
                        return tok
                elif c == b"#":
                    while f.read(1) not in (b"\n", b""):
                        pass
                else:
                    tok += c

        magic = token()
        if magic != b"P6":
            raise ValueError(f"not a binary PPM: {magic!r}")
        width = int(token())
        height = int(token())
        maxval = int(token())
        if maxval != 255:
            raise ValueError(f"unsupported maxval {maxval}")
        return width, height, f.read(width * height * 3)


def write_png(path, width, height, rgb):
    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body
                + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    # Each scanline is prefixed with filter type 0 (None).
    raw = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3]
                   for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) != 3:
        print("usage: ppm2png.py in.ppm out.png")
        return 1
    width, height, rgb = read_ppm(sys.argv[1])
    write_png(sys.argv[2], width, height, rgb)
    print(f"ppm2png: {sys.argv[2]} ({width}x{height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

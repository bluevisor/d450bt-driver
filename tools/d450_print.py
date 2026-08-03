#!/usr/bin/env python3
"""Direct TSPL test tool for the D450BT — bypasses CUPS entirely.

Sends a test pattern (or an image) straight to the printer's Bluetooth
serial port. Useful for verifying the TSPL protocol before debugging the
CUPS pipeline.

Usage:
  ./d450_print.py --test                          # built-in test label
  ./d450_print.py image.png --width 100 --height 150
  ./d450_print.py --text "HELLO"                  # quick text label

Requires: pillow (only for image mode): pip install pillow
"""
import argparse
import sys
import time

DEFAULT_PORT = "/dev/cu.D450BT-Z"
EOL = b"\r\n"


def build_header(width_mm, height_mm, gap_mm=3, density=8, speed=4):
    cmds = [
        f"SIZE {width_mm:.1f} mm,{height_mm:.1f} mm".encode(),
        f"GAP {gap_mm} mm,0 mm".encode(),
        b"DIRECTION 0,0",
        b"REFERENCE 0,0",
        f"DENSITY {density}".encode(),
        f"SPEED {speed}".encode(),
        b"SET RIBBON OFF",
        b"SET TEAR ON",
        b"CLS",
    ]
    return EOL.join(cmds) + EOL


def bitmap_cmd(row_bytes, rows, bits):
    """TSPL BITMAP: mode 0, bit 0 = black."""
    return b"BITMAP 0,0,%d,%d,0," % (row_bytes, rows) + bits + EOL


def image_to_bits(img, width_px):
    from PIL import Image

    ratio = width_px / img.width
    img = img.resize((width_px, max(1, int(img.height * ratio))))
    img = img.convert("L").convert("1")  # Pillow dithers (Floyd-Steinberg)
    row_bytes = (img.width + 7) // 8
    out = bytearray([0xFF] * row_bytes * img.height)
    px = img.load()
    for y in range(img.height):
        for x in range(img.width):
            if px[x, y] == 0:  # black
                out[y * row_bytes + (x >> 3)] &= ~(0x80 >> (x & 7))
    return bytes(out), row_bytes, img.height


def test_pattern(width_px=400, height_px=240):
    """Border + diagonal + density bars, no dependencies."""
    row_bytes = (width_px + 7) // 8
    out = bytearray([0xFF] * row_bytes * height_px)

    def set_black(x, y):
        if 0 <= x < width_px and 0 <= y < height_px:
            out[y * row_bytes + (x >> 3)] &= ~(0x80 >> (x & 7))

    for x in range(width_px):
        for t in range(3):
            set_black(x, t)
            set_black(x, height_px - 1 - t)
    for y in range(height_px):
        for t in range(3):
            set_black(t, y)
            set_black(width_px - 1 - t, y)
        # diagonal
        x = int(y * width_px / height_px)
        for t in range(3):
            set_black(x + t, y)
    # density bars: 8 bars with increasing dot coverage
    for bar in range(8):
        x0 = 20 + bar * ((width_px - 40) // 8)
        for y in range(20, 60):
            for x in range(x0, x0 + 30):
                if (x + y * (bar + 1)) % 8 < bar + 1:
                    set_black(x, y)
    return bytes(out), row_bytes, height_px


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", nargs="?", help="PNG/JPEG to print")
    ap.add_argument("--test", action="store_true", help="print test pattern")
    ap.add_argument("--text", help="print a quick text label using TSPL fonts")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--width", type=float, default=50, help="label width mm")
    ap.add_argument("--height", type=float, default=30, help="label height mm")
    ap.add_argument("--gap", type=float, default=3, help="gap mm")
    ap.add_argument("--density", type=int, default=8)
    ap.add_argument("--speed", type=int, default=4)
    ap.add_argument("--dpi", type=int, default=203)
    args = ap.parse_args()

    payload = build_header(args.width, args.height, args.gap,
                           args.density, args.speed)

    if args.text:
        payload += (f'TEXT 20,20,"3",0,2,2,"{args.text}"').encode() + EOL
    elif args.test:
        bits, row_bytes, rows = test_pattern()
        payload += bitmap_cmd(row_bytes, rows, bits)
    elif args.image:
        from PIL import Image

        width_px = int(args.width / 25.4 * args.dpi)
        bits, row_bytes, rows = image_to_bits(Image.open(args.image), width_px)
        payload += bitmap_cmd(row_bytes, rows, bits)
    else:
        ap.error("give an image, --test, or --text")

    payload += b"PRINT 1,1" + EOL

    print(f"Sending {len(payload)} bytes to {args.port} ...")
    with open(args.port, "wb", buffering=0) as port:
        # BT SPP chokes on large single writes; chunk it.
        for i in range(0, len(payload), 512):
            port.write(payload[i:i + 512])
            time.sleep(0.02)
    print("Done.")


if __name__ == "__main__":
    sys.exit(main())

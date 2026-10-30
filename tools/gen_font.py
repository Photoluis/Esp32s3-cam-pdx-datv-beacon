"""Beacon DATV IZ7PDX - genera firmware/main/font8x16.h (ASCII 32..126, celle 8x16, un byte per riga, bit 7 = sinistra)
per la sovrimpressione disegnata dall'ESP sulle immagini della camera.

Uso: python gen_font.py C:/Windows/Fonts/consolab.ttf ../firmware/main/font8x16.h
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def main():
    font = ImageFont.truetype(sys.argv[1], 15)
    rows = []
    for c in range(32, 127):
        im = Image.new("L", (8, 16), 0)
        d = ImageDraw.Draw(im)
        bbox = d.textbbox((0, 0), chr(c), font=font)
        x = (8 - (bbox[2] - bbox[0])) // 2 - bbox[0]
        d.text((x, 0), chr(c), fill=255, font=font)
        glyph = []
        for y in range(16):
            v = 0
            for xx in range(8):
                if im.getpixel((xx, y)) >= 110:
                    v |= 0x80 >> xx
            glyph.append(v)
        rows.append(f"    {{{','.join(f'0x{v:02x}' for v in glyph)}}},  /* '{chr(c) if c != 92 else 'backslash'}' */")
    out = ["/* Beacon DATV IZ7PDX - font 8x16 generato da tools/gen_font.py (" + Path(sys.argv[1]).name + "), ASCII 32..126 */",
           "#pragma once", "#include <stdint.h>", "", "static const uint8_t font8x16[95][16] = {", *rows, "};", ""]
    Path(sys.argv[2]).write_text("\n".join(out), encoding="utf-8")
    print(f"scritto {sys.argv[2]}")


if __name__ == "__main__":
    main()

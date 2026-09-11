#!/usr/bin/env python3
"""Converts an XWD framebuffer dump (from Xvfb -fbdir) into a PNG without PIL."""
import struct, sys, zlib

def read_xwd(path):
    data = open(path, 'rb').read()
    hdr = struct.unpack('>25I', data[:100])
    header_size, file_version, pixmap_format, pixmap_depth, width, height = hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5]
    xoffset, byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, bits_per_pixel = hdr[6:12]
    bytes_per_line, visual_class, red_mask, green_mask, blue_mask, bits_per_rgb, colormap_entries, ncolors = hdr[12:20]
    off = header_size + ncolors * 12
    pixels = data[off:]
    return width, height, bytes_per_line, bits_per_pixel, byte_order, red_mask, green_mask, blue_mask, pixels

def mask_shift(m):
    s = 0
    while m and not (m & 1): m >>= 1; s += 1
    return s

def write_png(path, width, height, rows):
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)

def main():
    src, dst = sys.argv[1], sys.argv[2]
    w, h, bpl, bpp, order, rm, gm, bm, px = read_xwd(src)
    rs, gs, bs = mask_shift(rm), mask_shift(gm), mask_shift(bm)
    rows = []
    Bpp = bpp // 8
    for y in range(h):
        line = px[y * bpl : y * bpl + w * Bpp]
        out = bytearray(w * 3)
        for x in range(w):
            v = int.from_bytes(line[x * Bpp:(x + 1) * Bpp], 'big' if order == 1 else 'little')
            out[x*3+0] = (v & rm) >> rs
            out[x*3+1] = (v & gm) >> gs
            out[x*3+2] = (v & bm) >> bs
        rows.append(bytes(out))
    write_png(dst, w, h, rows)
    print('wrote', dst, w, 'x', h)

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Enclosed dark stone room with a fire (full bright sprite) block, like the user's screenshot."""
import gzip, struct, sys
W, H, L = 32, 24, 32
blocks = bytearray(W * H * L)
def idx(x, y, z): return (y * L + z) * W + x
def fill(x1, y1, z1, x2, y2, z2, b):
    for y in range(y1, y2 + 1):
        for z in range(z1, z2 + 1):
            for x in range(x1, x2 + 1): blocks[idx(x, y, z)] = b
STONE, COBBLE, FIRE, LAVA = 1, 4, 54, 11
fill(0, 0, 0, W-1, 9, L-1, STONE)
# room interior x 8..23, y 10..16, z 8..23 hollow inside a cobble shell
fill(6, 10, 6, 25, 18, 25, COBBLE)
fill(8, 10, 8, 23, 16, 23, 0)
fill(8, 9, 8, 23, 9, 23, STONE)  # floor
emitter = int(sys.argv[2]) if len(sys.argv) > 2 else FIRE
blocks[idx(16, 10, 16)] = emitter
sx, sy, sz, syaw, spitch = 9, 12, 9, 135, 15
def tag_byte(name, v):   return b'\x01' + struct.pack('>H', len(name)) + name.encode() + struct.pack('>b', v)
def tag_short(name, v):  return b'\x02' + struct.pack('>H', len(name)) + name.encode() + struct.pack('>h', v)
def tag_bytes(name, v):  return b'\x07' + struct.pack('>H', len(name)) + name.encode() + struct.pack('>i', len(v)) + bytes(v)
def tag_compound(name, children): return b'\x0a' + struct.pack('>H', len(name)) + name.encode() + b''.join(children) + b'\x00'
def packed(deg): return int(deg / 360.0 * 256) & 0xFF
def sb(v): return v - 256 if v > 127 else v
root = tag_compound('ClassicWorld', [
    tag_byte('FormatVersion', 1),
    tag_short('X', W), tag_short('Y', H), tag_short('Z', L),
    tag_compound('Spawn', [tag_short('X', sx), tag_short('Y', sy), tag_short('Z', sz), tag_byte('H', sb(packed(syaw))), tag_byte('P', sb(packed(spitch)))]),
    tag_bytes('BlockArray', blocks),
    tag_compound('Metadata', [tag_compound('CPE', [tag_compound('EnvColors', [
        tag_compound('Sky',    [tag_short('R', 20), tag_short('G', 20), tag_short('B', 40)]),
        tag_compound('Sunlight', [tag_short('R', 255), tag_short('G', 255), tag_short('B', 255)]),
        tag_compound('Ambient',  [tag_short('R', 30), tag_short('G', 30), tag_short('B', 30)]),
    ])])]),
])
open(sys.argv[1], 'wb').write(gzip.compress(root))

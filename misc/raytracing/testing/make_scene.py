#!/usr/bin/env python3
"""Writes a small ClassicWorld (.cw) test scene for the ray tracer."""
import gzip, struct, sys

W, H, L = 64, 32, 64
blocks = bytearray(W * H * L)

def idx(x, y, z): return (y * L + z) * W + x
def setb(x, y, z, b):
    if 0 <= x < W and 0 <= y < H and 0 <= z < L: blocks[idx(x, y, z)] = b
def fill(x1, y1, z1, x2, y2, z2, b):
    for y in range(y1, y2 + 1):
        for z in range(z1, z2 + 1):
            for x in range(x1, x2 + 1): setb(x, y, z, b)

STONE, GRASS, DIRT, COBBLE, SAPLING, WATER, LAVA, SAND, LOG, LEAVES, GLASS, SLAB, FLOWER, ROSE, GOLD, BOOKSHELF = 1, 2, 3, 4, 6, 9, 11, 12, 17, 18, 20, 44, 37, 38, 41, 47

fill(0, 0, 0, W - 1, 9, L - 1, STONE)
fill(0, 10, 0, W - 1, 10, L - 1, DIRT)
fill(0, 11, 0, W - 1, 11, L - 1, GRASS)

# Water pool (surface at y = 11, two blocks deep) with a sand floor
fill(8, 9, 8, 15, 11, 15, WATER)
fill(8, 8, 8, 15, 8, 15, SAND)
fill(8, 9, 8, 15, 9, 15, SAND)
fill(8, 10, 8, 15, 11, 15, WATER)

# Lava pool
fill(30, 11, 8, 33, 11, 11, LAVA)

# Stone wall right next to the lava pool, and a recessed lava block in a cobble frame
fill(34, 12, 7, 34, 14, 12, STONE)
fill(36, 12, 16, 38, 14, 16, COBBLE)
setb(37, 13, 16, LAVA)
fill(30, 12, 13, 38, 12, 20, 0)

# Tall stone wall for long shadows, with a cobblestone pillar
fill(22, 12, 14, 23, 18, 30, STONE)
fill(28, 12, 20, 28, 20, 20, COBBLE)

# Glass wall
fill(24, 12, 8, 27, 14, 8, GLASS)

# Tree
fill(16, 12, 26, 16, 16, 26, LOG)
fill(14, 16, 24, 18, 18, 28, LEAVES)
setb(16, 19, 26, LEAVES)

# Plants and a slab
setb(12, 12, 20, SAPLING)
setb(13, 12, 21, FLOWER)
setb(11, 12, 22, ROSE)
setb(14, 12, 22, SLAB)
setb(10, 12, 18, GOLD)
fill(18, 12, 30, 20, 13, 31, BOOKSHELF)

# A small cave entrance to show darkness/GI
fill(40, 8, 30, 44, 11, 44, 0)
fill(40, 12, 33, 44, 13, 44, STONE)
fill(41, 8, 34, 43, 11, 43, 0)

# Spawn. Yaw 0 looks along -z, 90 along +x, 180 along +z (Vec3_GetDirVector)
sx, sy, sz, syaw, spitch = int(sys.argv[2]) if len(sys.argv) > 2 else 6, int(sys.argv[6]) if len(sys.argv) > 6 else 15, int(sys.argv[3]) if len(sys.argv) > 3 else 20, int(sys.argv[4]) if len(sys.argv) > 4 else 300, int(sys.argv[5]) if len(sys.argv) > 5 else 15

def tag_byte(name, v):   return b'\x01' + struct.pack('>H', len(name)) + name.encode() + struct.pack('>b', v)
def tag_short(name, v):  return b'\x02' + struct.pack('>H', len(name)) + name.encode() + struct.pack('>h', v)
def tag_bytes(name, v):  return b'\x07' + struct.pack('>H', len(name)) + name.encode() + struct.pack('>i', len(v)) + bytes(v)
def tag_compound(name, children): return b'\x0a' + struct.pack('>H', len(name)) + name.encode() + b''.join(children) + b'\x00'

# pillar under an elevated spawn so the player doesn't fall
if sy > 13: fill(sx, 12, sz, sx, sy - 2, sz, STONE)

def packed(deg): return int(deg / 360.0 * 256) & 0xFF

root = tag_compound('ClassicWorld', [
    tag_byte('FormatVersion', 1),
    tag_short('X', W), tag_short('Y', H), tag_short('Z', L),
    tag_compound('Spawn', [tag_short('X', sx), tag_short('Y', sy), tag_short('Z', sz), tag_byte('H', packed(syaw) - 256 if packed(syaw) > 127 else packed(syaw)), tag_byte('P', packed(spitch) - 256 if packed(spitch) > 127 else packed(spitch))]),
    tag_bytes('BlockArray', blocks),
])

with gzip.open(sys.argv[1], 'wb') as f:
    f.write(root)
print('wrote', sys.argv[1])

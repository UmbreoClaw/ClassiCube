#!/usr/bin/env python3
"""Writes test/skinned.glb and test/skinned.gltf: a tiny skinned model used by the
plugin's loader test. Only needs the Python standard library.

Skeleton (glTF space, +Y up, front is +Z as per the spec):
  Root (0,0,0)
   +-- Head   at (0, 1.5, 0)
   +-- Arm.R  at (0.3, 1.4, 0)   (glTF +X; becomes -X in game after the 180 degree flip)
Mesh: three unit-ish boxes, each fully weighted to one joint, 12 triangles each.
Animations: "Idle" (does nothing for 1s), "Walk" swings Arm.R about X by 90 degrees over 1s,
and "Jump" (STEP interpolation) lifts Root by 1 unit at t=0.5.
The texture is a 4x4 PNG embedded through a bufferView (the .glb) / data URI (the .gltf).
"""
import base64
import json
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def png_4x4():
    rows = []
    for y in range(4):
        row = b"\x00"
        for x in range(4):
            row += bytes((255 if (x + y) % 2 else 0, 128, 64, 255))
        rows.append(row)
    raw = b"".join(rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def box(cx, cy, cz, sx, sy, sz):
    """Returns (positions, normals, uvs, indices) for an axis aligned box (CCW faces)."""
    x1, x2 = cx - sx / 2, cx + sx / 2
    y1, y2 = cy - sy / 2, cy + sy / 2
    z1, z2 = cz - sz / 2, cz + sz / 2
    faces = [  # normal, four corners CCW seen from outside
        ((0, 0, 1), [(x1, y1, z2), (x2, y1, z2), (x2, y2, z2), (x1, y2, z2)]),
        ((0, 0, -1), [(x2, y1, z1), (x1, y1, z1), (x1, y2, z1), (x2, y2, z1)]),
        ((1, 0, 0), [(x2, y1, z2), (x2, y1, z1), (x2, y2, z1), (x2, y2, z2)]),
        ((-1, 0, 0), [(x1, y1, z1), (x1, y1, z2), (x1, y2, z2), (x1, y2, z1)]),
        ((0, 1, 0), [(x1, y2, z2), (x2, y2, z2), (x2, y2, z1), (x1, y2, z1)]),
        ((0, -1, 0), [(x1, y1, z1), (x2, y1, z1), (x2, y1, z2), (x1, y1, z2)]),
    ]
    pos, nrm, uv, idx = [], [], [], []
    for n, corners in faces:
        base = len(pos)
        for k, c in enumerate(corners):
            pos.append(c)
            nrm.append(n)
            uv.append([(0, 1), (1, 1), (1, 0), (0, 0)][k])
        idx += [base, base + 1, base + 2, base, base + 2, base + 3]
    return pos, nrm, uv, idx


def build():
    # joints: 0 Root, 1 Head, 2 Arm.R
    parts = [
        (0, box(0, 0.75, 0, 0.5, 1.5, 0.25)),    # body -> Root
        (1, box(0, 1.75, 0, 0.5, 0.5, 0.5)),     # head -> Head (joint origin at 1.5)
        (2, box(0.3, 1.0, 0, 0.2, 0.8, 0.2)),    # arm  -> Arm.R (joint origin at 0.3,1.4)
    ]
    positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []
    for joint, (p, n, u, i) in parts:
        base = len(positions)
        positions += p
        normals += n
        uvs += u
        joints += [(joint, 0, 0, 0)] * len(p)
        weights += [(1.0, 0.0, 0.0, 0.0)] * len(p)
        indices += [base + k for k in i]

    def inverse_translation(x, y, z):
        return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1]

    inv_bind = inverse_translation(0, 0, 0) + inverse_translation(0, 1.5, 0) + inverse_translation(0.3, 1.4, 0)

    blobs = []

    def add(fmt, values, target=None):
        data = b"".join(struct.pack("<" + fmt, *v) if isinstance(v, tuple) else struct.pack("<" + fmt, v) for v in values)
        while len(data) % 4:
            data += b"\x00"
        blobs.append((data, target))
        return len(blobs) - 1

    view_pos = add("3f", positions, 34962)
    view_nrm = add("3f", normals, 34962)
    view_uv = add("2f", uvs, 34962)
    view_joints = add("4B", joints, 34962)
    view_weights = add("4f", weights, 34962)
    view_idx = add("H", indices, 34963)
    view_ibm = add("16f", [tuple(inv_bind[i:i + 16]) for i in range(0, 48, 16)])
    walk_times = [0.0, 0.5, 1.0]
    walk_rots = [(0, 0, 0, 1), (0.7071068, 0, 0, 0.7071068), (0, 0, 0, 1)]  # 90 degrees about X at t=0.5
    view_walk_t = add("f", walk_times)
    view_walk_r = add("4f", walk_rots)
    view_idle_t = add("f", [0.0, 1.0])
    view_idle_r = add("4f", [(0, 0, 0, 1), (0, 0, 0, 1)])
    view_jump_t = add("f", [0.0, 0.5, 1.0])
    view_jump_v = add("3f", [(0, 0, 0), (0, 1, 0), (0, 0, 0)])
    png = png_4x4()
    view_png = add("B", list(png))

    # Assemble the binary buffer
    buffer = b""
    views = []
    for data, target in blobs:
        view = {"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data)}
        if target:
            view["target"] = target
        views.append(view)
        buffer += data
    # trim the PNG view padding
    views[view_png]["byteLength"] = len(png)

    count = len(positions)
    accessors = [
        {"bufferView": view_pos, "componentType": 5126, "count": count, "type": "VEC3",
         "min": [min(p[i] for p in positions) for i in range(3)], "max": [max(p[i] for p in positions) for i in range(3)]},
        {"bufferView": view_nrm, "componentType": 5126, "count": count, "type": "VEC3"},
        {"bufferView": view_uv, "componentType": 5126, "count": count, "type": "VEC2"},
        {"bufferView": view_joints, "componentType": 5121, "count": count, "type": "VEC4"},
        {"bufferView": view_weights, "componentType": 5126, "count": count, "type": "VEC4"},
        {"bufferView": view_idx, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        {"bufferView": view_ibm, "componentType": 5126, "count": 3, "type": "MAT4"},
        {"bufferView": view_walk_t, "componentType": 5126, "count": 3, "type": "SCALAR", "min": [0], "max": [1]},
        {"bufferView": view_walk_r, "componentType": 5126, "count": 3, "type": "VEC4"},
        {"bufferView": view_idle_t, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1]},
        {"bufferView": view_idle_r, "componentType": 5126, "count": 2, "type": "VEC4"},
        {"bufferView": view_jump_t, "componentType": 5126, "count": 3, "type": "SCALAR", "min": [0], "max": [1]},
        {"bufferView": view_jump_v, "componentType": 5126, "count": 3, "type": "VEC3"},
    ]

    gltf = {
        "asset": {"version": "2.0", "generator": "make_skinned.py"},
        "scene": 0,
        "scenes": [{"nodes": [0, 4], "extras": {"cc_eye_height": 1.7, "cc_size": [0.6, 2.0, 0.6]}}],
        "nodes": [
            {"name": "Root", "children": [1, 2]},
            {"name": "Head", "translation": [0, 1.5, 0]},
            {"name": "Arm.R", "translation": [0.3, 1.4, 0]},
            {"name": "unused"},
            {"name": "Body", "mesh": 0, "skin": 0},
        ],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2, "JOINTS_0": 3, "WEIGHTS_0": 4},
                                     "indices": 5, "material": 0}]}],
        "skins": [{"joints": [0, 1, 2], "inverseBindMatrices": 6, "skeleton": 0}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
        "textures": [{"source": 0}],
        "images": [{"bufferView": view_png, "mimeType": "image/png"}],
        "animations": [
            {"name": "Idle", "channels": [{"sampler": 0, "target": {"node": 2, "path": "rotation"}}],
             "samplers": [{"input": 9, "output": 10, "interpolation": "LINEAR"}]},
            {"name": "Walk", "channels": [{"sampler": 0, "target": {"node": 2, "path": "rotation"}}],
             "samplers": [{"input": 7, "output": 8, "interpolation": "LINEAR"}]},
            {"name": "Jump", "channels": [{"sampler": 0, "target": {"node": 0, "path": "translation"}}],
             "samplers": [{"input": 11, "output": 12, "interpolation": "STEP"}]},
        ],
        "accessors": accessors,
        "bufferViews": views,
        "buffers": [{"byteLength": len(buffer)}],
    }
    return gltf, buffer


def write_glb(path, gltf, buffer):
    js = json.dumps(gltf, separators=(",", ":")).encode()
    while len(js) % 4:
        js += b" "
    header = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(buffer))
    with open(path, "wb") as f:
        f.write(header)
        f.write(struct.pack("<II", len(js), 0x4E4F534A) + js)
        f.write(struct.pack("<II", len(buffer), 0x004E4942) + buffer)


def write_gltf(path, gltf, buffer):
    gltf = json.loads(json.dumps(gltf))
    gltf["buffers"][0]["uri"] = "data:application/octet-stream;base64," + base64.b64encode(buffer).decode()
    with open(path, "w") as f:
        json.dump(gltf, f, indent=1)


if __name__ == "__main__":
    gltf, buffer = build()
    write_glb(os.path.join(HERE, "skinned.glb"), gltf, buffer)
    write_gltf(os.path.join(HERE, "skinned.gltf"), gltf, buffer)
    print("wrote skinned.glb and skinned.gltf (%d bytes of buffer)" % len(buffer))

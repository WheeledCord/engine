#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Writes tests/regression/assets/rig/test_rig.gltf: the smallest skinned glTF 2.0 model the
engine's animation checks need, with its buffer embedded as base64 so it is one file.

A chain of three bones, `root` at the origin, `arm` 1 m above it and `hand.R` 1 m above that, and
three boxes skinned rigidly to them: a flat base on `root`, a 1 m upright on `arm` and a 0.3 m cube
on `hand.R`. Two animations: `idle` holds the rest pose for 1 s; `wave` turns `arm` 90 degrees about
+Z over 1 s and back over the next, so looped it waves every 2 s and the hand moves from (0, 2, 0)
to (-1, 1, 0) in model space.

Plain Python 3, no third-party modules. Run from the repository root:
    python3 tools/make_test_rig.py [output path]
"""
import base64
import json
import math
import struct
import sys

OUT = "tests/regression/assets/rig/test_rig.gltf"

FLOAT, UBYTE, USHORT = 5126, 5121, 5123
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963

# name, parent, translation relative to the parent
BONES = [("root", -1, (0.0, 0.0, 0.0)), ("arm", 0, (0.0, 1.0, 0.0)), ("hand.R", 1, (0.0, 1.0, 0.0))]
# centre, size, bone: each box follows one bone rigidly
BOXES = [((0.0, 0.1, 0.0), (0.6, 0.2, 0.6), 0), ((0.0, 1.5, 0.0), (0.2, 1.0, 0.2), 1),
         ((0.0, 2.15, 0.0), (0.3, 0.3, 0.3), 2)]

# Six faces: normal, and the two axes spanning the face (u x v = normal, so winding is CCW outside).
FACES = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
         ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
         ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]


def box(centre, size, bone, positions, normals, uvs, joints, weights, indices):
    for n, u, v in FACES:
        base = len(positions)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [centre[k] + 0.5 * size[k] * (n[k] + su * u[k] + sv * v[k]) for k in range(3)]
            positions.append(p)
            normals.append(list(n))
            uvs.append([(su + 1) / 2, (1 - sv) / 2])
            joints.append([bone, 0, 0, 0])
            weights.append([1.0, 0.0, 0.0, 0.0])
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]


class Buffer:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def add(self, fmt, values, count, kind, component, target=None, bounds=False):
        while len(self.data) % 4:
            self.data.append(0)
        start = len(self.data)
        self.data += struct.pack("<" + fmt * len(values), *values)
        view = {"buffer": 0, "byteOffset": start, "byteLength": len(self.data) - start}
        if target:
            view["target"] = target
        self.views.append(view)
        accessor = {"bufferView": len(self.views) - 1, "componentType": component, "count": count, "type": kind}
        if bounds:
            width = len(values) // count
            accessor["min"] = [min(values[i::width]) for i in range(width)]
            accessor["max"] = [max(values[i::width]) for i in range(width)]
        self.accessors.append(accessor)
        return len(self.accessors) - 1


def flat(rows):
    return [x for row in rows for x in row]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    positions, normals, uvs, joints, weights, indices = [], [], [], [], [], []
    for centre, size, bone in BOXES:
        box(centre, size, bone, positions, normals, uvs, joints, weights, indices)
    n = len(positions)
    b = Buffer()
    a_pos = b.add("f", flat(positions), n, "VEC3", FLOAT, ARRAY_BUFFER, bounds=True)
    a_nrm = b.add("f", flat(normals), n, "VEC3", FLOAT, ARRAY_BUFFER)
    a_uv = b.add("f", flat(uvs), n, "VEC2", FLOAT, ARRAY_BUFFER)
    a_jnt = b.add("B", flat(joints), n, "VEC4", UBYTE, ARRAY_BUFFER)
    a_wgt = b.add("f", flat(weights), n, "VEC4", FLOAT, ARRAY_BUFFER)
    a_idx = b.add("H", indices, len(indices), "SCALAR", USHORT, ELEMENT_ARRAY_BUFFER)

    # Inverse bind matrices, column-major: each bone's rest world transform is a pure translation.
    world = []
    for name, parent, t in BONES:
        w = list(t) if parent < 0 else [world[parent][k] + t[k] for k in range(3)]
        world.append(w)
    ibm = []
    for w in world:
        ibm += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -w[0], -w[1], -w[2], 1]
    a_ibm = b.add("f", ibm, len(BONES), "MAT4", FLOAT)

    half = math.sqrt(0.5)  # a quarter turn about +Z: (0, 0, sin 45, cos 45)
    rest, turned = [0.0, 0.0, 0.0, 1.0], [0.0, 0.0, half, half]
    a_idle_t = b.add("f", [0.0, 1.0], 2, "SCALAR", FLOAT, bounds=True)
    a_idle_r = b.add("f", rest + rest, 2, "VEC4", FLOAT)
    a_wave_t = b.add("f", [0.0, 1.0, 2.0], 3, "SCALAR", FLOAT, bounds=True)
    a_wave_r = b.add("f", rest + turned + rest, 3, "VEC4", FLOAT)

    nodes = []
    for i, (name, parent, t) in enumerate(BONES):
        node = {"name": name, "translation": list(t)}
        children = [j for j, bone in enumerate(BONES) if bone[1] == i]
        if children:
            node["children"] = children
        nodes.append(node)
    nodes.append({"name": "body", "mesh": 0, "skin": 0})

    def clip(name, times, rotations):
        return {"name": name,
                "samplers": [{"input": times, "output": rotations, "interpolation": "LINEAR"}],
                "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}]}

    gltf = {
        "asset": {"version": "2.0", "generator": "tools/make_test_rig.py"},
        "scene": 0,
        "scenes": [{"nodes": [0, len(BONES)]}],
        "nodes": nodes,
        "meshes": [{"name": "body", "primitives": [{
            "attributes": {"POSITION": a_pos, "NORMAL": a_nrm, "TEXCOORD_0": a_uv, "JOINTS_0": a_jnt,
                           "WEIGHTS_0": a_wgt},
            "indices": a_idx, "material": 0}]}],
        "materials": [{"name": "grey", "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.8, 0.8, 1.0]}}],
        "skins": [{"name": "rig", "joints": list(range(len(BONES))), "skeleton": 0, "inverseBindMatrices": a_ibm}],
        "animations": [clip("idle", a_idle_t, a_idle_r), clip("wave", a_wave_t, a_wave_r)],
        "buffers": [{"byteLength": len(b.data),
                     "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(b.data)).decode()}],
        "bufferViews": b.views,
        "accessors": b.accessors,
    }
    with open(out, "w") as f:
        json.dump(gltf, f, indent=1)
        f.write("\n")
    print("wrote %s: %d bones, %d vertices, %d triangles, animations idle and wave" %
          (out, len(BONES), n, len(indices) // 3))


if __name__ == "__main__":
    main()

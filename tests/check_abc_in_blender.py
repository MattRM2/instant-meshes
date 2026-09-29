"""
check_abc_in_blender.py -- Import .abc files written by Instant Meshes into
Blender and compare them with the reference manifest.

Each file is matched to a manifest case by name: "copy_scene_ab.abc" and
"scene_ab.abc" both map to case "scene_ab". For every polygon mesh of the
case, Blender must import an object with the same vertex and face counts
and the same Y-up world bounding box (Blender converts back to Z-up on
import, so the box is compared after the inverse conversion).

Usage (Blender 5.x, headless, no user addons):
    blender -b --factory-startup --python tests/check_abc_in_blender.py -- \
        tests/data/manifest.json file1.abc [file2.abc ...]
Exit code 0 when every file matches, 1 otherwise.
"""

import bpy
import json
import math
import os
import sys
from mathutils import Matrix

Z_UP_TO_Y_UP = Matrix.Rotation(-math.pi / 2, 4, 'X')
TOLERANCE = 1e-4


def case_name(path, cases):
    base = os.path.splitext(os.path.basename(path))[0]
    for name in sorted(cases, key=len, reverse=True):
        if base == name or base.endswith("_" + name):
            return name
    return None


def imported_meshes():
    depsgraph = bpy.context.evaluated_depsgraph_get()
    result = []
    for obj in bpy.context.scene.objects:
        if obj.type != 'MESH':
            continue
        ev = obj.evaluated_get(depsgraph)
        me = ev.to_mesh()
        world = Z_UP_TO_Y_UP @ ev.matrix_world
        pts = [world @ v.co for v in me.vertices]
        lo = [min(p[i] for p in pts) for i in range(3)]
        hi = [max(p[i] for p in pts) for i in range(3)]
        result.append((len(me.vertices), len(me.polygons), lo, hi, obj.name))
        ev.to_mesh_clear()
    return result


def check(path, case):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.wm.alembic_import(filepath=path)
    got = imported_meshes()
    errors = []
    for want in case["meshes"]:
        (wlo, whi) = want["bbox_yup"]
        match = [g for g in got
                 if g[0] == want["vertices"] and g[1] == want["faces"]
                 and all(abs(a - b) < TOLERANCE for a, b in zip(g[2] + g[3], wlo + whi))]
        if not match:
            errors.append("mesh %s (V=%d F=%d) not found in import %s" % (
                want["name"], want["vertices"], want["faces"],
                [(g[4], g[0], g[1]) for g in got]))
        else:
            got.remove(match[0])
    if got:
        errors.append("unexpected extra meshes %s" % [g[4] for g in got])
    return errors


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(argv) < 2:
        print("usage: ... -- manifest.json file.abc [...]")
        sys.exit(2)
    cases = json.load(open(argv[0]))["cases"]
    failed = 0
    # Absolute paths: read_factory_settings() resets Blender's working directory
    for path in [os.path.abspath(p) for p in argv[1:]]:
        name = case_name(path, cases)
        if name is None:
            print("[FAIL] %s: no matching case in the manifest" % path)
            failed += 1
            continue
        errors = check(path, cases[name])
        if errors:
            failed += 1
            print("[FAIL] %s (case %s)" % (path, name))
            for e in errors:
                print("       " + e)
        else:
            print("[OK]   %s (case %s)" % (os.path.basename(path), name))
    print("%d file(s) checked, %d failed" % (len(argv) - 1, failed))
    sys.exit(1 if failed else 0)


main()

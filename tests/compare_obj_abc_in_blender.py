"""
compare_obj_abc_in_blender.py -- Checks that an .abc written by Instant
Meshes imports in Blender exactly like the .obj written by the same
(deterministic, -d) remesh: same vertices, same polygons with the same
corner order (hence the same normal orientation), same positions within the
OBJ text precision.

Usage (Blender 5.x, headless, no user addons):
    blender -b --factory-startup --python tests/compare_obj_abc_in_blender.py -- \
        a.obj a.abc [b.obj b.abc ...]
Exit code 0 when every pair matches, 1 otherwise.
"""

import bpy
import os
import sys


def import_mesh(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if path.lower().endswith(".obj"):
        bpy.ops.wm.obj_import(filepath=path)
    else:
        bpy.ops.wm.alembic_import(filepath=path)
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    if len(meshes) != 1:
        raise RuntimeError("%s: expected 1 mesh, got %d" % (path, len(meshes)))
    obj = meshes[0]
    world = obj.matrix_world
    verts = [tuple(world @ v.co) for v in obj.data.vertices]
    polys = [tuple(p.vertices) for p in obj.data.polygons]
    return obj.name, verts, polys


def compare(obj_path, abc_path):
    errors = []
    _, v1, p1 = import_mesh(obj_path)
    name, v2, p2 = import_mesh(abc_path)
    if len(v1) != len(v2):
        errors.append("vertices %d vs %d" % (len(v1), len(v2)))
    if len(p1) != len(p2):
        errors.append("polygons %d vs %d" % (len(p1), len(p2)))
    if not errors:
        extent = max(max(abs(c) for c in v) for v in v1) or 1.0
        dist = max(max(abs(a - b) for a, b in zip(x, y)) for x, y in zip(v1, v2))
        if dist > 1e-4 * extent:
            errors.append("positions differ by %g" % dist)
        different = sum(1 for a, b in zip(p1, p2) if a != b)
        if different:
            errors.append("%d polygons with a different corner order" % different)
    sizes = {}
    for p in p2:
        sizes[len(p)] = sizes.get(len(p), 0) + 1
    return name, len(v2), len(p2), sizes, errors


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(argv) < 2 or len(argv) % 2:
        print(__doc__)
        sys.exit(2)
    failed = 0
    for obj_path, abc_path in zip(argv[0::2], argv[1::2]):
        obj_path, abc_path = os.path.abspath(obj_path), os.path.abspath(abc_path)
        try:
            name, nv, np, sizes, errors = compare(obj_path, abc_path)
        except Exception as e:
            name, nv, np, sizes, errors = "?", 0, 0, {}, [str(e)]
        tag = "OK  " if not errors else "FAIL"
        failed += bool(errors)
        print("[%s] %s: object '%s', V=%d, polygons=%d %s" % (
            tag, os.path.basename(abc_path), name, nv, np,
            " ".join("%d-gon:%d" % kv for kv in sorted(sizes.items()))))
        for e in errors:
            print("       " + e)
    print("%d pair(s) checked, %d failed" % (len(argv) // 2, failed))
    sys.exit(1 if failed else 0)


main()

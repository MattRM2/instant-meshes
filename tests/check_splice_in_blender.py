"""
check_splice_in_blender.py -- Imports an Alembic file before and after a
per-mesh remesh (-m / --others) in Blender and checks that:
  - every object of the original is still there, with the same type,
    parent and world transform (cameras, curves, empties included);
  - meshes that were not remeshed are identical (vertices, polygons,
    positions, materials);
  - remeshed meshes stay in place: the center of their bounding box within
    5% of their size and no side growing by more than 5% (a coarse input
    may legitimately shrink a little when remeshed), and they keep their
    material when they had a single one.

Usage (Blender 5.x, headless, no user addons):
    blender -b --factory-startup --python tests/check_splice_in_blender.py -- \
        original.abc spliced.abc RemeshedObject [RemeshedObject ...]
(object names as Blender shows them after import). Exit code 0 when OK.
"""

import bpy
import os
import sys


def snapshot(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.wm.alembic_import(filepath=os.path.abspath(path))
    depsgraph = bpy.context.evaluated_depsgraph_get()
    scene = {}
    for obj in bpy.context.scene.objects:
        info = {
            "type": obj.type,
            "parent": obj.parent.name if obj.parent else None,
            "matrix": [tuple(r) for r in obj.matrix_world],
        }
        if obj.type == 'MESH':
            ev = obj.evaluated_get(depsgraph)
            me = ev.to_mesh()
            world = ev.matrix_world
            pts = [tuple(world @ v.co) for v in me.vertices]
            info["verts"] = pts
            info["polys"] = [tuple(p.vertices) for p in me.polygons]
            info["bbox"] = ([min(p[i] for p in pts) for i in range(3)],
                            [max(p[i] for p in pts) for i in range(3)])
            info["materials"] = [m.name for m in obj.data.materials if m]
            ev.to_mesh_clear()
        scene[obj.name] = info
    return scene


def close(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(argv) < 2:
        print(__doc__)
        sys.exit(2)
    original, spliced, remeshed = argv[0], argv[1], set(argv[2:])
    before, after = snapshot(original), snapshot(spliced)
    errors = []

    for name in sorted(before):
        a = before[name]
        b = after.get(name)
        if b is None:
            errors.append("%s: missing after the remesh" % name)
            continue
        if a["type"] != b["type"] or a["parent"] != b["parent"]:
            errors.append("%s: type/parent changed" % name)
        if not all(close(r1, r2, 1e-6) for r1, r2 in zip(a["matrix"], b["matrix"])):
            errors.append("%s: world transform changed" % name)
        if a["type"] != 'MESH':
            continue
        if name in remeshed:
            (lo1, hi1), (lo2, hi2) = a["bbox"], b["bbox"]
            size = max(h - l for l, h in zip(lo1, hi1))
            c1 = [(l + h) / 2 for l, h in zip(lo1, hi1)]
            c2 = [(l + h) / 2 for l, h in zip(lo2, hi2)]
            grows = any((h2 - l2) > (h1 - l1) + 0.05 * size
                        for l1, h1, l2, h2 in zip(lo1, hi1, lo2, hi2))
            if not close(c1, c2, 0.05 * size) or grows:
                errors.append("%s: moved or deformed (bbox %s -> %s)" % (name, a["bbox"], b["bbox"]))
            if len(a["materials"]) == 1 and b["materials"] != a["materials"]:
                errors.append("%s: material %s lost (%s)" % (name, a["materials"], b["materials"]))
            if b["polys"] == a["polys"]:
                errors.append("%s: not remeshed" % name)
            print("[remeshed ] %-12s %6d -> %6d polygons, materials %s" % (
                name, len(a["polys"]), len(b["polys"]), b["materials"]))
        else:
            same = (a["polys"] == b["polys"] and len(a["verts"]) == len(b["verts"]) and
                    all(close(p, q, 1e-6) for p, q in zip(a["verts"], b["verts"])) and
                    a["materials"] == b["materials"])
            if not same:
                errors.append("%s: should be unchanged but differs" % name)
            print("[unchanged] %-12s %6d polygons, materials %s%s" % (
                name, len(b["polys"]), b["materials"], "" if same else "  <-- DIFFERENT"))

    for name in sorted(set(after) - set(before)):
        errors.append("%s: unexpected new object" % name)
    for name in sorted(n for n in before if before[n]["type"] != 'MESH'):
        print("[kept     ] %-12s %s" % (name, before[name]["type"]))

    for e in errors:
        print("[FAIL] " + e)
    print("%d objects compared, %d problem(s)" % (len(before), len(errors)))
    sys.exit(1 if errors else 0)


main()

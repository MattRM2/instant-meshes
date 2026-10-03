"""
check_uv_in_blender.py -- Blender reads the UVs written by Instant Meshes.

Imports the files written by the unit tests (im_tests, build/test_tmp) and
the reference files they come from, then compares, per UV map, every face
corner as (world position, uv). Prints OK / FAILED per check, exit code 1
on any failure.

    blender -b --factory-startup --python tests/check_uv_in_blender.py -- <data dir> <test_tmp dir>
"""
import os
import sys

import bpy

args = sys.argv[sys.argv.index("--") + 1:]
DATA, TMP = args[0], args[1]
failed = 0


def load(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if path.endswith(".abc"):
        bpy.ops.wm.alembic_import(filepath=path)
    else:
        bpy.ops.wm.obj_import(filepath=path, forward_axis='NEGATIVE_Z', up_axis='Y')
    return {o.name: o for o in bpy.context.scene.objects if o.type == 'MESH'}


def corners(obj, uv_name):
    """Sorted (x, y, z, u, v) of every face corner, rounded"""
    me = obj.data
    layer = me.uv_layers.get(uv_name) if uv_name else me.uv_layers.active
    if layer is None:
        return None
    m = obj.matrix_world
    out = []
    for loop in me.loops:
        p = m @ me.vertices[loop.vertex_index].co
        uv = layer.data[loop.index].uv
        out.append(tuple(round(x, 4) for x in (p.x, p.y, p.z, uv.x, uv.y)))
    return sorted(out)


def triangulated(rows):
    """Corner lists only match up to the triangulation: compare the set of
    (position, uv) pairs instead of the multiset"""
    return sorted(set(rows)) if rows is not None else None


def check(label, ok):
    global failed
    print(("OK      " if ok else "FAILED  ") + label)
    failed += not ok


# Reference: the Blender-written cylinder
ref = load(os.path.join(DATA, "uv_sets.abc"))
ref_main = triangulated(corners(ref["Cylinder"], "UVMap"))
ref_planar = triangulated(corners(ref["Cylinder"], "Planar"))
ref_grid = triangulated(corners(ref["Grid"], "UVMap"))

# Whole-file Alembic written by Instant Meshes (triangles, both maps)
got = load(os.path.join(TMP, "uv_write.abc"))
obj = next(iter(got.values()))
check("uv_write.abc: map names %s" % [l.name for l in obj.data.uv_layers],
      [l.name for l in obj.data.uv_layers] == ["UVMap", "Planar"])
check("uv_write.abc: UVMap corners", triangulated(corners(obj, "UVMap")) == ref_main)
check("uv_write.abc: Planar corners", triangulated(corners(obj, "Planar")) == ref_planar)

# Whole-file OBJ (first map)
got = load(os.path.join(TMP, "uv_write.obj"))
obj = next(iter(got.values()))
check("uv_write.obj: UV corners", triangulated(corners(obj, None)) == ref_main)

# Per-object splice, Alembic: cylinder rewritten, grid untouched
got = load(os.path.join(TMP, "uv_splice.abc"))
check("uv_splice.abc: objects %s" % sorted(got), sorted(got) == sorted(ref))
check("uv_splice.abc: cylinder UVMap", triangulated(corners(got["Cylinder"], "UVMap")) == ref_main)
check("uv_splice.abc: cylinder Planar", triangulated(corners(got["Cylinder"], "Planar")) == ref_planar)
check("uv_splice.abc: grid UVMap kept", triangulated(corners(got["Grid"], "UVMap")) == ref_grid)

# Per-object splice, OBJ
ref_obj = load(os.path.join(DATA, "uv_sets.obj"))
ref_obj_cyl = triangulated(corners(ref_obj["Cylinder"], None))
ref_obj_grid = triangulated(corners(ref_obj["Grid"], None))
got = load(os.path.join(TMP, "uv_splice.obj"))
check("uv_splice.obj: cylinder UVs", triangulated(corners(got["Cylinder"], None)) == ref_obj_cyl)
check("uv_splice.obj: grid UVs kept", triangulated(corners(got["Grid"], None)) == ref_obj_grid)

# Irregular polygon (quad-dominant output): one pentagon, UV = (0.1 k, 0.2 k)
for ext in (".obj", ".abc"):
    got = load(os.path.join(TMP, "uv_pentagon" + ext))
    obj = next(iter(got.values()))
    rows = corners(obj, None) or []
    pent = [p for p in obj.data.polygons]
    ok = len(pent) == 1 and len(pent[0].vertices) == 5 and len(rows) == 5
    uvs = sorted((r[3], r[4]) for r in rows)
    ok = ok and uvs == sorted((round(0.1 * k, 4), round(0.2 * k, 4)) for k in range(5))
    check("uv_pentagon%s: one pentagon with its UVs" % ext, ok)

print("\n%s" % ("ALL OK" if failed == 0 else "%d FAILED" % failed))
sys.exit(1 if failed else 0)

"""
check_usd_overlay.py -- Pixar's USD composes a .usda layer written by
Instant Meshes over its input, and checks it.

    blender -b --factory-startup --python tests/check_usd_overlay.py -- <input> <output.usda> <replaced path>...

For every replaced mesh: its new points, placed in the world by USD, lie on
the original surface (within 1% of its size),
valid topology, right-handed, normals and non-constant primvars blocked,
UV primvars consistent with the faces, GeomSubsets inactive. Every other
mesh: points and faces unchanged. Stage metadata unchanged. Exit code 1
on any failure.
"""
import sys

import numpy as np
from pxr import Gf, Usd, UsdGeom

args = sys.argv[sys.argv.index("--") + 1:]
# "@file": the mesh paths listed in a file, one per line (scenes of thousands of meshes)
args = [l.strip() for a in args for l in (open(a[1:], encoding="utf-8") if a.startswith("@") else [a]) if l.strip()]
src_path, out_path, replaced = args[0], args[1], set(args[2:])
failed = 0


def check(label, ok):
    global failed
    print(("OK      " if ok else "FAILED  ") + label)
    failed += not ok


src = Usd.Stage.Open(src_path)
out = Usd.Stage.Open(out_path)
for key in ("upAxis", "metersPerUnit"):
    check("metadata %s" % key, src.GetMetadata(key) == out.GetMetadata(key))
check("defaultPrim", src.GetRootLayer().defaultPrim == out.GetRootLayer().defaultPrim)

time = Usd.TimeCode.EarliestTime()
cache_src = UsdGeom.XformCache(time)
cache_out = UsdGeom.XformCache(time)


def world_points(cache, prim):
    m = cache.GetLocalToWorldTransform(prim)
    return np.array([list(m.Transform(Gf.Vec3d(p))) for p in UsdGeom.Mesh(prim).GetPointsAttr().Get(time)])


def world_triangles(cache, prim):
    mesh = UsdGeom.Mesh(prim)
    pts = world_points(cache, prim)
    tris, offset = [], 0
    idx = mesh.GetFaceVertexIndicesAttr().Get()
    for n in mesh.GetFaceVertexCountsAttr().Get():
        for k in range(1, n - 1):
            tris.append([idx[offset], idx[offset + k], idx[offset + k + 1]])
        offset += n
    t = np.array(tris)
    return pts[t[:, 0]], pts[t[:, 1]], pts[t[:, 2]]


def distance_to_surface(p, a, b, c):
    """Distance from points p (n x 3) to the triangles (a, b, c): (m x 3) each"""
    best = np.full(len(p), np.inf)
    for i in range(len(a)):
        A, B, C = a[i], b[i], c[i]
        n = np.cross(B - A, C - A)
        nn = np.dot(n, n)
        # inside: distance to the plane
        if nn > 0:
            d = (p - A) @ n / nn
            q = p - np.outer(d, n)
            inside = (np.cross(B - A, q - A) @ n >= 0) & (np.cross(C - B, q - B) @ n >= 0) & \
                     (np.cross(A - C, q - C) @ n >= 0)
            plane = np.abs(d) * np.sqrt(nn)
            best = np.where(inside, np.minimum(best, plane), best)
        for X, Y in ((A, B), (B, C), (C, A)):
            e = Y - X
            t = np.clip(((p - X) @ e) / max(np.dot(e, e), 1e-30), 0, 1)
            best = np.minimum(best, np.linalg.norm(X + np.outer(t, e) - p, axis=1))
    return best


for prim in src.Traverse():
    if prim.GetTypeName() != "Mesh":
        continue
    path = str(prim.GetPath())
    o = out.GetPrimAtPath(path)
    check("%s exists" % path, o.IsValid())
    if not o.IsValid():
        continue
    m_src, m_out = UsdGeom.Mesh(prim), UsdGeom.Mesh(o)
    if path not in replaced:
        same = (list(m_src.GetPointsAttr().Get(time) or []) == list(m_out.GetPointsAttr().Get(time) or []) and
                list(m_src.GetFaceVertexIndicesAttr().Get() or []) == list(m_out.GetFaceVertexIndicesAttr().Get() or []))
        check("%s unchanged" % path, same)
        continue
    counts = m_out.GetFaceVertexCountsAttr().Get()
    idx = m_out.GetFaceVertexIndicesAttr().Get()
    pts = m_out.GetPointsAttr().Get()
    check("%s: %d faces, topology valid" % (path, len(counts)),
          len(counts) > 0 and sum(counts) == len(idx) and max(idx) < len(pts) and min(counts) >= 3)
    check("%s: right-handed" % path, m_out.GetOrientationAttr().Get() == UsdGeom.Tokens.rightHanded)
    src_pts = world_points(cache_src, prim)
    size = float(np.max(src_pts.max(axis=0) - src_pts.min(axis=0)))
    dist = distance_to_surface(world_points(cache_out, o), *world_triangles(cache_src, prim))
    check("%s: new points on the original surface (max %.3g of %.3g)" % (path, dist.max(), size),
          dist.max() <= 0.01 * size)
    normals = m_out.GetNormalsAttr()
    check("%s: normals blocked" % path, not normals.HasAuthoredValue() or normals.Get() is None)
    for pv in UsdGeom.PrimvarsAPI(o).GetPrimvars():
        interp = pv.GetInterpolation()
        value = pv.Get()
        name = pv.GetPrimvarName()
        if value is None:
            continue
        if interp == UsdGeom.Tokens.constant:
            continue
        if pv.GetTypeName().role == "TextureCoordinate" or name in ("st", "uv"):
            n = len(pv.GetIndices()) if pv.IsIndexed() else len(value)
            check("%s: UV %s per face corner (%d of %d)" % (path, name, n, len(idx)),
                  interp == UsdGeom.Tokens.faceVarying and n == len(idx))
        else:
            check("%s: primvar %s still set (%s)" % (path, name, interp), False)
    for child in o.GetAllChildren():   # inactive ones included
        if child.GetTypeName() == "GeomSubset":
            check("%s: subset %s inactive" % (path, child.GetName()), not child.IsActive())

print("\n%s" % ("ALL OK" if failed == 0 else "%d FAILED" % failed))
sys.exit(1 if failed else 0)

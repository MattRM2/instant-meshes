"""
check_usd_proxy.py -- Pixar's USD opens a scene to which Instant Meshes
added proxies (--proxy), and checks it.

    blender -b --factory-startup --python tests/check_usd_proxy.py -- <input> <output> [--tol=0.05] <mesh path>...

<input>: the scene before (a copy, when the proxies were added in place);
<output>: the scene after. Its layer is the input's, plus one reference
prepended to some root prims, to the proxy layer (a layer that sublayers
nothing): with those references removed, both layers are identical.

For every given mesh: its proxyPrim relationship targets a new Mesh; the
computed purposes are "render" for the mesh and "proxy" for the proxy; the
proxy lies on the surface of the mesh (within 1% of its size, --tol for
strong reductions, whose coarse proxies cut the curves) at the first
frame and at the last one (it follows animated parents); valid topology, no
subdivision, the material of the mesh (one of its subsets' when it has
some; outside the proxy's root prim, a stand-in that references it and
keeps its shading), UVs per face corner when the mesh has UVs. Every other mesh:
unchanged, no proxy. Stage metadata unchanged. Exit code 1 on any failure.
"""
import sys

import numpy as np
from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade

args = sys.argv[sys.argv.index("--") + 1:]
tol = float(next((a[6:] for a in args if a.startswith("--tol=")), 0.01))
args = [a for a in args if not a.startswith("--tol=")]
src_path, out_path, targets = args[0], args[1], set(args[2:])
failed = 0


def check(label, ok):
    global failed
    print(("OK      " if ok else "FAILED  ") + label)
    failed += not ok


# the layers: the scene's own, the reference added, the proxy layer
src_layer, out_layer = Sdf.Layer.FindOrOpen(src_path), Sdf.Layer.FindOrOpen(out_path)
check("sublayers kept", list(src_layer.subLayerPaths) == list(out_layer.subLayerPaths))
stripped = Sdf.Layer.CreateAnonymous(".usda")
stripped.TransferContent(out_layer)
added, made = set(), []
for prim in list(stripped.rootPrims):
    before = src_layer.GetPrimAtPath(prim.path)
    refs = prim.referenceList
    explicit = refs.isExplicit
    old = list((before.referenceList.explicitItems if explicit else before.referenceList.prependedItems)
               if before else [])
    items = list(refs.explicitItems if explicit else refs.prependedItems)
    for r in items:
        if r in old:
            continue
        added.add(r.assetPath)
        check("%s references %s%s%s" % (prim.path, r.assetPath, r.primPath, " (explicit list)" if explicit else ""),
              r.primPath == prim.path and r.assetPath.startswith("./") and "_proxy" in r.assetPath and
              items.index(r) == 0)
    if explicit:
        refs.explicitItems = old
    else:
        refs.prependedItems = old
    if not before:
        made.append(prim)   # an over made for the reference (root prim defined in a sublayer)
for prim in made:
    check("%s: an over holding the reference only" % prim.path,
          prim.specifier == Sdf.SpecifierOver and not prim.properties and not prim.nameChildren)
    del stripped.rootPrims[prim.name]
check("one proxy layer referenced (%s)" % ", ".join(sorted(added)), len(added) == 1)
want = Sdf.Layer.CreateAnonymous(".usda")
want.TransferContent(src_layer)
check("the layer is the input's, but for the references", stripped.ExportToString() == want.ExportToString())
for asset in added:
    proxy_layer = Sdf.Layer.FindOrOpenRelativeToLayer(out_layer, asset)
    check("%s: no sublayer" % asset, proxy_layer is not None and not list(proxy_layer.subLayerPaths))

src = Usd.Stage.Open(src_path)
out = Usd.Stage.Open(out_path)
for key in ("upAxis", "metersPerUnit"):
    check("metadata %s" % key, src.GetMetadata(key) == out.GetMetadata(key))
check("defaultPrim", src.GetRootLayer().defaultPrim == out.GetRootLayer().defaultPrim)

first = Usd.TimeCode.EarliestTime()
last = Usd.TimeCode(out.GetEndTimeCode()) if out.HasAuthoredTimeCodeRange() else first


def world_points(prim, time):
    m = UsdGeom.XformCache(time).GetLocalToWorldTransform(prim)
    return np.array([list(m.Transform(Gf.Vec3d(p))) for p in UsdGeom.Mesh(prim).GetPointsAttr().Get(time)])


def world_triangles(prim, time):
    mesh = UsdGeom.Mesh(prim)
    pts = world_points(prim, time)
    tris, offset = [], 0
    idx = mesh.GetFaceVertexIndicesAttr().Get(time)
    for n in mesh.GetFaceVertexCountsAttr().Get(time):
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
        if nn > 0:
            d = (p - A) @ n / nn
            q = p - np.outer(d, n)
            inside = (np.cross(B - A, q - A) @ n >= 0) & (np.cross(C - B, q - B) @ n >= 0) & \
                     (np.cross(A - C, q - C) @ n >= 0)
            best = np.where(inside, np.minimum(best, np.abs(d) * np.sqrt(nn)), best)
        for X, Y in ((A, B), (B, C), (C, A)):
            e = Y - X
            t = np.clip(((p - X) @ e) / max(np.dot(e, e), 1e-30), 0, 1)
            best = np.minimum(best, np.linalg.norm(X + np.outer(t, e) - p, axis=1))
    return best


def has_uvs(prim):
    return any(pv.GetTypeName().role == "TextureCoordinate" and pv.GetInterpolation() != UsdGeom.Tokens.constant
               for pv in UsdGeom.PrimvarsAPI(prim).GetPrimvars())


for prim in src.Traverse():
    if prim.GetTypeName() != "Mesh":
        continue
    path = str(prim.GetPath())
    r = out.GetPrimAtPath(path)
    rel = UsdGeom.Imageable(r).GetProxyPrimRel().GetTargets()
    if path not in targets:
        same = (list(UsdGeom.Mesh(prim).GetPointsAttr().Get(first) or []) ==
                list(UsdGeom.Mesh(r).GetPointsAttr().Get(first) or []))
        had = UsdGeom.Imageable(prim).GetProxyPrimRel().GetTargets()
        check("%s unchanged, no new proxy" % path, same and rel == had)
        continue
    check("%s: proxyPrim -> %s" % (path, [str(t) for t in rel]), len(rel) == 1)
    if len(rel) != 1:
        continue
    p = out.GetPrimAtPath(rel[0])
    check("%s: proxy is a Mesh" % rel[0], p.IsValid() and p.GetTypeName() == "Mesh")
    if not p.IsValid():
        continue
    check("%s: purpose render" % path, UsdGeom.Imageable(r).ComputePurpose() == UsdGeom.Tokens.render)
    check("%s: purpose proxy" % rel[0], UsdGeom.Imageable(p).ComputePurpose() == UsdGeom.Tokens.proxy)
    m = UsdGeom.Mesh(p)
    counts, idx, pts = m.GetFaceVertexCountsAttr().Get(), m.GetFaceVertexIndicesAttr().Get(), m.GetPointsAttr().Get()
    check("%s: %d faces, topology valid" % (rel[0], len(counts)),
          len(counts) > 0 and sum(counts) == len(idx) and max(idx) < len(pts) and min(counts) >= 3)
    check("%s: no subdivision" % rel[0], m.GetSubdivisionSchemeAttr().Get() == UsdGeom.Tokens.none)
    for label, time in (("first frame", first), ("last frame", last)):
        src_pts = world_points(prim, time)
        size = float(np.max(src_pts.max(axis=0) - src_pts.min(axis=0)))
        dist = distance_to_surface(world_points(p, time), *world_triangles(prim, time))
        check("%s: on the surface of the mesh, %s (max %.3g of %.3g)" % (rel[0], label, dist.max(), size),
              dist.max() <= tol * size)
    want, _ = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()
    subsets = [UsdShade.MaterialBindingAPI(s.GetPrim()).ComputeBoundMaterial()[0]
               for s in UsdGeom.Subset.GetAllGeomSubsets(UsdGeom.Imageable(prim))]
    allowed = {str(x.GetPath()) for x in [want] + subsets if x}
    got, _ = UsdShade.MaterialBindingAPI(p).ComputeBoundMaterial()
    # a material outside the proxy's root prim: a stand-in that references it
    via = [str(spec.path) for spec in got.GetPrim().GetPrimStack()] if got else []
    via = [v for v in via if v in allowed and v != str(got.GetPath())]
    shading = got and bool(got.GetSurfaceOutput().GetConnectedSources()[0]) if got else False
    got = str(got.GetPath()) if got else ""
    check("%s: material %s%s (of %s)" % (rel[0], got or "none", " referencing " + via[0] if via else "",
                                          sorted(allowed) or "none"),
          (got in allowed or (via and shading and got.split("/")[1] == rel[0].pathString.split("/")[1]))
          if allowed else got == "")
    if has_uvs(prim):
        uvs = [pv for pv in UsdGeom.PrimvarsAPI(p).GetPrimvars() if pv.GetTypeName().role == "TextureCoordinate"]
        ok = bool(uvs) and all(pv.GetInterpolation() == UsdGeom.Tokens.faceVarying and
                               (len(pv.GetIndices()) if pv.IsIndexed() else len(pv.Get())) == len(idx) for pv in uvs)
        check("%s: UVs per face corner (%s)" % (rel[0], ", ".join(pv.GetPrimvarName() for pv in uvs)), ok)

print("\n%s" % ("ALL OK" if failed == 0 else "%d FAILED" % failed))
sys.exit(1 if failed else 0)

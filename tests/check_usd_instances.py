"""
check_usd_instances.py -- Pixar's USD opens a scene whose instanced meshes
Instant Meshes remeshed (-m / --others, or --proxy), and checks that the
instancing survived.

    blender -b --factory-startup --python tests/check_usd_instances.py -- <input> <output> <replace|proxy> [--tol=0.1] <mesh path>...

Each given mesh is one appearance of an instanced mesh (a native instance's
mesh, or a PointInstancer prototype). Its group: every appearance of the same
prototype mesh, outside nested instances. For each member of a group:

  replace: its faces changed, the same count on every member; its world
           bounds still match the input's (within 10% of its size, --tol);
  proxy:   it has a proxyPrim to a Mesh with purpose proxy, fewer faces,
           the same count on every member, within the same bounds.

Every native instance stays an instance, the members of a group share one
prototype, a PointInstancer keeps its prototypes and points; every other
mesh is unchanged. Exit code 1 on any failure.
"""
import sys

from pxr import Gf, Usd, UsdGeom

args = sys.argv[sys.argv.index("--") + 1:]
# "@file": the mesh paths listed in a file, one per line (scenes of thousands of meshes)
args = [l.strip() for a in args for l in (open(a[1:], encoding="utf-8") if a.startswith("@") else [a]) if l.strip()]
tol = float(next((a[6:] for a in args if a.startswith("--tol=")), 0.1))
args = [a for a in args if not a.startswith("--tol=")]
src_path, out_path, mode, targets = args[0], args[1], args[2], args[3:]
failed = 0


def check(label, ok):
    global failed
    print(("OK      " if ok else "FAILED  ") + label)
    failed += not ok


src = Usd.Stage.Open(src_path)
out = Usd.Stage.Open(out_path)
traverse = Usd.TraverseInstanceProxies(Usd.PrimDefaultPredicate)


def meshes(stage):
    return [p for p in stage.Traverse(traverse) if p.GetTypeName() == "Mesh"]


def instance_levels(prim):
    n, p = 0, prim
    while p and not p.IsPseudoRoot():
        n += p.IsInstance() or p.GetTypeName() == "PointInstancer"
        p = p.GetParent()
    return n


def instance_root(prim):
    p = prim
    while p and not p.IsPseudoRoot():
        if p.IsInstance():
            return p
        p = p.GetParent()
    return None


def faces(prim):
    return len(UsdGeom.Mesh(prim).GetFaceVertexCountsAttr().Get() or [])


def bounds(prim):
    m = UsdGeom.XformCache().GetLocalToWorldTransform(prim)
    pts = [m.Transform(Gf.Vec3d(p)) for p in UsdGeom.Mesh(prim).GetPointsAttr().Get() or []]
    lo = [min(p[k] for p in pts) for k in range(3)]
    hi = [max(p[k] for p in pts) for k in range(3)]
    return lo, hi


def proto_key(prim):
    """The same mesh of the same prototype: the prototype's prim"""
    return str(prim.GetPrimInPrototype().GetPath()) if prim.IsInstanceProxy() else str(prim.GetPath())


# the groups, from the input
groups = {}
for path in targets:
    p = src.GetPrimAtPath(path)
    check("%s exists in the input" % path, p.IsValid())
    if p.IsValid():
        key = proto_key(p)
        groups[key] = [str(q.GetPath()) for q in meshes(src)
                       if proto_key(q) == key and instance_levels(q) <= 1]
members = {m for g in groups.values() for m in g}

for key, group in groups.items():
    counts, prototypes = set(), set()
    for path in group:
        a, b = src.GetPrimAtPath(path), out.GetPrimAtPath(path)
        if not b.IsValid():
            check("%s still there" % path, False)
            continue
        lo0, hi0 = bounds(a)
        size = max(h - l for l, h in zip(lo0, hi0))
        shown = b
        if mode == "proxy":
            rel = UsdGeom.Imageable(b).GetProxyPrimRel().GetTargets()
            proxy = out.GetPrimAtPath(rel[0]) if len(rel) == 1 else None
            ok = proxy is not None and proxy.IsValid() and proxy.GetTypeName() == "Mesh" and \
                UsdGeom.Imageable(proxy).ComputePurpose() == UsdGeom.Tokens.proxy
            check("%s: proxy %s" % (path, rel[0] if rel else "none"), ok)
            if not ok:
                continue
            shown = proxy
            check("%s: proxy lighter (%d < %d faces)" % (path, faces(proxy), faces(a)), faces(proxy) < faces(a))
            check("%s: unchanged itself" % path, faces(b) == faces(a))
        else:
            check("%s: remeshed (%d -> %d faces)" % (path, faces(a), faces(b)), faces(b) != faces(a))
        counts.add(faces(shown))
        lo1, hi1 = bounds(shown)
        err = max(max(abs(x - y) for x, y in zip(lo0, lo1)), max(abs(x - y) for x, y in zip(hi0, hi1)))
        check("%s: in place (bounds off by %.3g of %.3g)" % (path, err, size), err <= tol * size)
        root = instance_root(b)
        if root is not None:
            check("%s: still an instance" % root.GetPath(), root.IsInstance())
            prototypes.add(str(root.GetPrototype().GetPath()))
    check("%s: every appearance the same (%d) (%s)" % (key, len(group), sorted(counts)), len(counts) == 1)
    if prototypes:
        check("%s: its instances share one prototype" % key, len(prototypes) == 1)

for a in meshes(src):
    path = str(a.GetPath())
    if path in members:
        continue
    b = out.GetPrimAtPath(path)
    same = b.IsValid() and faces(a) == faces(b)
    rel = UsdGeom.Imageable(b).GetProxyPrimRel().GetTargets() if b.IsValid() else []
    had = UsdGeom.Imageable(a).GetProxyPrimRel().GetTargets()
    check("%s unchanged" % path, same and rel == had)

for pi in [p for p in src.Traverse() if p.GetTypeName() == "PointInstancer"]:
    a, b = UsdGeom.PointInstancer(pi), UsdGeom.PointInstancer(out.GetPrimAtPath(pi.GetPath()))
    check("%s: same prototypes and points" % pi.GetPath(),
          a.GetPrototypesRel().GetTargets() == b.GetPrototypesRel().GetTargets() and
          list(a.GetProtoIndicesAttr().Get()) == list(b.GetProtoIndicesAttr().Get()))

print("\n%s" % ("ALL OK" if failed == 0 else "%d FAILED" % failed))
sys.exit(1 if failed else 0)

"""
usd_reference.py -- USD reference data and ground truth, with Pixar's USD
library (the pxr module that ships with Blender 4.x / 5.x).

    blender -b --factory-startup --python tests/usd_reference.py -- make <dir>
        writes usd_scene.usda / .usdc / .usdz (the same scene in every format)
    blender -b --factory-startup --python tests/usd_reference.py -- dump <file>
        prints the layer in the canonical form of build/Release/usd_dump.exe

Set USD_WRITE_NEW_USDC_FILES_AS_VERSION (e.g. 0.8.0) before starting
Blender to write an older Crate version.
"""
import math
import os
import sys

from pxr import Gf, Kind, Sdf, Usd, UsdGeom, UsdShade, UsdUtils, Vt


def make_scene(path):
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 0.01)
    world = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(world.GetPrim())
    Usd.ModelAPI(world.GetPrim()).SetKind(Kind.Tokens.component)
    UsdGeom.Scope.Define(stage, "/World/geo")

    # Materials
    UsdGeom.Scope.Define(stage, "/World/mtl")
    red = UsdShade.Material.Define(stage, "/World/mtl/red")
    blue = UsdShade.Material.Define(stage, "/World/mtl/blue")

    # MeshA: an octagonal prism (8 quads + 2 octagons), translate / rotateXYZ /
    # scale, indexed face-varying st, face-varying normals, a GeomSubset
    seg, pts = 8, []
    for z in (-1.0, 1.0):
        for k in range(seg):
            a = 2 * math.pi * k / seg
            pts.append(Gf.Vec3f(math.cos(a), z, math.sin(a)))
    counts, idx = [], []
    for k in range(seg):
        counts.append(4)
        idx += [k, (k + 1) % seg, seg + (k + 1) % seg, seg + k]
    counts += [seg, seg]
    idx += list(reversed(range(seg))) + [seg + k for k in range(seg)]
    a = UsdGeom.Mesh.Define(stage, "/World/geo/MeshA")
    a.CreatePointsAttr(pts)
    a.CreateFaceVertexCountsAttr(counts)
    a.CreateFaceVertexIndicesAttr(idx)
    a.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
    a.AddTranslateOp().Set(Gf.Vec3d(-3, 0.5, 0))
    a.AddRotateXYZOp().Set(Gf.Vec3f(10, 20, 30))
    a.AddScaleOp().Set(Gf.Vec3f(1, 2, 1))
    st_vals, st_idx = [], []
    for c, n in enumerate(counts):
        for j in range(n):
            st_idx.append(len(st_vals))
            st_vals.append(Gf.Vec2f(0.1 * c / 10 + 0.05 * j, 0.3 + 0.01 * j))
    st = UsdGeom.PrimvarsAPI(a).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray,
                                             UsdGeom.Tokens.faceVarying)
    st.Set(st_vals)
    st.SetIndices(Vt.IntArray(st_idx))
    a.CreateNormalsAttr([Gf.Vec3f(0, 1, 0)] * len(idx))
    a.SetNormalsInterpolation(UsdGeom.Tokens.faceVarying)
    subset = UsdGeom.Subset.Define(stage, "/World/geo/MeshA/top")
    subset.CreateElementTypeAttr(UsdGeom.Tokens.face)
    subset.CreateFamilyNameAttr("materialBind")
    subset.CreateIndicesAttr([seg + 1])
    UsdShade.MaterialBindingAPI.Apply(subset.GetPrim()).Bind(red)
    UsdShade.MaterialBindingAPI.Apply(a.GetPrim()).Bind(blue)

    # Group: matrix transform + orient (quaternion); MeshB inside, left-handed
    # triangles with vertex UVs (not indexed)
    g = UsdGeom.Xform.Define(stage, "/World/geo/Group")
    m = Gf.Matrix4d().SetTranslate(Gf.Vec3d(2, 0, 1)) * Gf.Matrix4d().SetScale(1.5)
    g.AddTransformOp().Set(m)
    g.AddOrientOp().Set(Gf.Quatf(0.9238795, 0, 0.3826834, 0))
    b = UsdGeom.Mesh.Define(stage, "/World/geo/Group/MeshB")
    b.CreatePointsAttr([Gf.Vec3f(0, 0, 0), Gf.Vec3f(1, 0, 0), Gf.Vec3f(1, 1, 0), Gf.Vec3f(0, 1, 0),
                        Gf.Vec3f(0.5, 0.5, 1)])
    b.CreateFaceVertexCountsAttr([3, 3, 3, 3, 4])
    b.CreateFaceVertexIndicesAttr([0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4, 0, 3, 2, 1])
    b.CreateOrientationAttr(UsdGeom.Tokens.leftHanded)
    uv = UsdGeom.PrimvarsAPI(b).CreatePrimvar("uv", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex)
    uv.Set([Gf.Vec2f(0, 0), Gf.Vec2f(1, 0), Gf.Vec2f(1, 1), Gf.Vec2f(0, 1), Gf.Vec2f(0.5, 0.5)])

    # MeshC: a 40 x 30 grid with integer coordinates (compressed float and
    # int arrays in Crate), a lookup-table friendly displayColor
    nx, ny = 40, 30
    grid = [Gf.Vec3f(i, 0, j) for j in range(ny + 1) for i in range(nx + 1)]
    cidx = []
    for j in range(ny):
        for i in range(nx):
            v = j * (nx + 1) + i
            cidx += [v, v + nx + 1, v + nx + 2, v + 1]
    c = UsdGeom.Mesh.Define(stage, "/World/geo/MeshC")
    c.CreatePointsAttr(grid)
    c.CreateFaceVertexCountsAttr([4] * (nx * ny))
    c.CreateFaceVertexIndicesAttr(cidx)
    c.AddTranslateOp().Set(Gf.Vec3d(0, -2, 0))
    c.AddScaleOp().Set(Gf.Vec3f(0.1, 0.1, 0.1))
    c.CreateDisplayColorAttr([Gf.Vec3f(0.25, 0.5, 1.0)])

    # MeshD: animated points
    d = UsdGeom.Mesh.Define(stage, "/World/geo/MeshD")
    d.CreateFaceVertexCountsAttr([3])
    d.CreateFaceVertexIndicesAttr([0, 1, 2])
    pa = d.CreatePointsAttr()
    pa.Set([Gf.Vec3f(0, 0, 0), Gf.Vec3f(1, 0, 0), Gf.Vec3f(0, 1, 0)], 1)
    pa.Set([Gf.Vec3f(0, 0, 0), Gf.Vec3f(2, 0, 0), Gf.Vec3f(0, 2, 0)], 2)

    # Spin: an animated parent transform (first sample: 0 degrees) over MeshE
    spin = UsdGeom.Xform.Define(stage, "/World/geo/Spin")
    rot = spin.AddRotateYOp()
    rot.Set(0.0, 1)
    rot.Set(90.0, 10)
    e = UsdGeom.Mesh.Define(stage, "/World/geo/Spin/MeshE")
    e.CreatePointsAttr([Gf.Vec3f(0, 0, 0), Gf.Vec3f(1, 0, 0), Gf.Vec3f(1, 0, 1), Gf.Vec3f(0, 0, 1)])
    e.CreateFaceVertexCountsAttr([4])
    e.CreateFaceVertexIndicesAttr([0, 1, 2, 3])

    # An instance of a class prototype, variants, a camera
    proto = stage.CreateClassPrim("/Proto")
    pm = UsdGeom.Mesh.Define(stage, "/Proto/Box")
    pm.CreatePointsAttr([Gf.Vec3f(0, 0, 0), Gf.Vec3f(1, 0, 0), Gf.Vec3f(0, 1, 0)])
    pm.CreateFaceVertexCountsAttr([3])
    pm.CreateFaceVertexIndicesAttr([0, 1, 2])
    inst = UsdGeom.Xform.Define(stage, "/World/geo/Inst")
    inst.GetPrim().GetReferences().AddInternalReference("/Proto")
    inst.GetPrim().SetInstanceable(True)
    var = UsdGeom.Xform.Define(stage, "/World/geo/Var")
    vs = var.GetPrim().GetVariantSets().AddVariantSet("lod")
    for name in ("high", "low"):
        vs.AddVariant(name)
        vs.SetVariantSelection(name)
        with vs.GetVariantEditContext():
            vm = UsdGeom.Mesh.Define(stage, "/World/geo/Var/Lod")
            vm.CreatePointsAttr([Gf.Vec3f(0, 0, 0), Gf.Vec3f(1, 0, 0), Gf.Vec3f(0, 1, 0)])
            vm.CreateFaceVertexCountsAttr([3])
            vm.CreateFaceVertexIndicesAttr([0, 1, 2])
    vs.SetVariantSelection("high")
    UsdGeom.Camera.Define(stage, "/World/Cam")
    stage.GetRootLayer().Save()


# ---------------------------------------------------------------------------
# Canonical dump (same format as tests/usd_dump.cpp)
# ---------------------------------------------------------------------------

def fmt(x):
    if abs(x) < 5e-5:
        x = 0.0
    s = "%.4g" % x
    return s


def flatten(v, out):
    if isinstance(v, bool):
        out.append(1.0 if v else 0.0)
    elif isinstance(v, (int, float)):
        out.append(float(v))
    elif isinstance(v, (Gf.Quatf, Gf.Quatd, Gf.Quath)):
        out.append(float(v.GetReal()))
        out.extend(float(x) for x in v.GetImaginary())
    elif isinstance(v, (Gf.Matrix2d, Gf.Matrix3d, Gf.Matrix4d)):
        for row in range(len(v)):
            out.extend(float(x) for x in v[row])
    else:
        for x in v:
            flatten(x, out)


def summary(v):
    if v is None:
        return "-"
    if isinstance(v, Sdf.ValueBlock):
        return "None"
    if isinstance(v, (str, Sdf.AssetPath, Sdf.Path)):
        v = [v]
    if isinstance(v, (list, tuple, Vt.StringArray, Vt.TokenArray)) and (not v or isinstance(v[0], (str, Sdf.AssetPath, Sdf.Path))):
        items = [x.path if isinstance(x, Sdf.AssetPath) else str(x) for x in v]
        return "s%d [%s]" % (len(items), " | ".join(items[:4]))
    nums = []
    flatten(v, nums)
    return "n=%d sum=%s [%s]" % (len(nums), fmt(sum(nums)), " ".join(fmt(x) for x in nums[:4]))


def list_items(lop):
    if lop.isExplicit:
        return list(lop.explicitItems)
    return list(lop.prependedItems) + list(lop.appendedItems)


def dump_prim(spec):
    spec_name = {Sdf.SpecifierDef: "def", Sdf.SpecifierOver: "over", Sdf.SpecifierClass: "class"}[spec.specifier]
    print("PRIM %s %s %s" % (spec.path, spec_name, spec.typeName))
    for prop in sorted(spec.properties, key=lambda p: p.name):
        if isinstance(prop, Sdf.RelationshipSpec):
            print("  REL %s [%s]" % (prop.name, " | ".join(str(p) for p in list_items(prop.targetPathList))))
        else:
            uniform = "uniform " if prop.variability == Sdf.VariabilityUniform else ""
            default = prop.default if prop.HasDefaultValue() else None
            ts = " (timeSamples)" if prop.HasInfo("timeSamples") else ""
            print("  ATTR %s %s%s = %s%s" % (prop.name, uniform, prop.typeName, summary(default), ts))
            if ts:
                samples = prop.GetInfo("timeSamples")
                for t in sorted(samples.keys()):
                    print("    SAMPLE %s: %s" % (fmt(t), summary(samples[t])))
    for child in spec.nameChildren:
        dump_prim(child)


def dump(path):
    layer = Sdf.Layer.FindOrOpen(path)
    fmt_name = os.path.splitext(path)[1][1:]
    if fmt_name == "usd":
        fmt_name = "usdc" if open(path, "rb").read(8) == b"PXR-USDC" else "usda"
    print("FORMAT %s" % fmt_name)
    info = layer.pseudoRoot
    for key in ("upAxis", "metersPerUnit", "defaultPrim"):
        if layer.pseudoRoot.HasInfo(key):
            print("META %s = %s" % (key, summary(layer.pseudoRoot.GetInfo(key))))
    for child in info.nameChildren:
        dump_prim(child)


args = sys.argv[sys.argv.index("--") + 1:]
if args[0] == "make":
    out = os.path.abspath(args[1])
    os.makedirs(out, exist_ok=True)
    usda = os.path.join(out, "usd_scene.usda")
    make_scene(usda)
    layer = Sdf.Layer.FindOrOpen(usda)
    suffix = os.environ.get("USD_WRITE_NEW_USDC_FILES_AS_VERSION", "")
    usdc = os.path.join(out, "usd_scene%s.usdc" % ("_v" + suffix.replace(".", "") if suffix else ""))
    layer.Export(usdc)
    if not suffix:
        UsdUtils.CreateNewUsdzPackage(Sdf.AssetPath(usdc), os.path.join(out, "usd_scene.usdz"))
    print("[OK] usd reference data in", out)
elif args[0] == "dump":
    dump(args[1])

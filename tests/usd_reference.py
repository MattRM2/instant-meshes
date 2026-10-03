"""
usd_reference.py -- USD reference data and ground truth, with Pixar's USD
library (the pxr module that ships with Blender 4.x / 5.x).

    blender -b --factory-startup --python tests/usd_reference.py -- make <dir>
        writes usd_scene.usda / .usdc / .usdz (the same scene in every format),
        and usd_asset.usdc (geo/render convention, for --proxy)
    blender -b --factory-startup --python tests/usd_reference.py -- dump <file>
        prints the layer in the canonical form of build/Release/usd_dump.exe
    blender -b --factory-startup --python tests/usd_reference.py -- compose <dir>
        writes the assembly of tests/data/compose (every composition arc)
    blender -b --factory-startup --python tests/usd_reference.py -- dump_stage <file>
        prints the composed stage as usd_dump --stage does

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


def sphere(stage, path, rings=12, segs=24, radius=1.0):
    """A UV sphere of quads (triangles at the poles), with face-varying st"""
    pts = [Gf.Vec3f(0, radius, 0)]
    for r in range(1, rings):
        t = math.pi * r / rings
        for s in range(segs):
            p = 2 * math.pi * s / segs
            pts.append(Gf.Vec3f(radius * math.sin(t) * math.cos(p), radius * math.cos(t),
                                radius * math.sin(t) * math.sin(p)))
    pts.append(Gf.Vec3f(0, -radius, 0))
    south = len(pts) - 1
    ring = lambda r, s: 1 + (r - 1) * segs + s % segs
    counts, idx, st = [], [], []
    for s in range(segs):
        counts.append(3)
        idx += [0, ring(1, s + 1), ring(1, s)]
        st += [Gf.Vec2f((s + 0.5) / segs, 1), Gf.Vec2f((s + 1.0) / segs, 1 - 1.0 / rings),
               Gf.Vec2f(s / segs, 1 - 1.0 / rings)]
    for r in range(1, rings - 1):
        for s in range(segs):
            counts.append(4)
            idx += [ring(r, s), ring(r, s + 1), ring(r + 1, s + 1), ring(r + 1, s)]
            v0, v1 = 1 - r / rings, 1 - (r + 1) / rings
            st += [Gf.Vec2f(s / segs, v0), Gf.Vec2f((s + 1) / segs, v0), Gf.Vec2f((s + 1) / segs, v1),
                   Gf.Vec2f(s / segs, v1)]
    for s in range(segs):
        counts.append(3)
        idx += [south, ring(rings - 1, s), ring(rings - 1, s + 1)]
        st += [Gf.Vec2f((s + 0.5) / segs, 0), Gf.Vec2f(s / segs, 1.0 / rings),
               Gf.Vec2f((s + 1.0) / segs, 1.0 / rings)]
    mesh = UsdGeom.Mesh.Define(stage, path)
    mesh.CreatePointsAttr(pts)
    mesh.CreateFaceVertexCountsAttr(counts)
    mesh.CreateFaceVertexIndicesAttr(idx)
    UsdGeom.PrimvarsAPI(mesh).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray,
                                            UsdGeom.Tokens.faceVarying).Set(st)
    return mesh, len(counts)


def make_asset(path):
    """An asset under the geo/render convention, for --proxy: a body with
    GeomSubsets, a part whose material is bound on its parent, a wheel under
    an animated transform, a guide mesh"""
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 0.01)
    stage.SetStartTimeCode(1)
    stage.SetEndTimeCode(24)
    asset = UsdGeom.Xform.Define(stage, "/Asset")
    stage.SetDefaultPrim(asset.GetPrim())
    Usd.ModelAPI(asset.GetPrim()).SetKind(Kind.Tokens.component)
    UsdGeom.Scope.Define(stage, "/Asset/mtl")
    paint = UsdShade.Material.Define(stage, "/Asset/mtl/paint")
    metal = UsdShade.Material.Define(stage, "/Asset/mtl/metal")
    rubber = UsdShade.Material.Define(stage, "/Asset/mtl/rubber")
    geo = UsdGeom.Xform.Define(stage, "/Asset/geo")
    geo.AddTranslateOp().Set(Gf.Vec3d(0, 1, 0))
    UsdGeom.Scope.Define(stage, "/Asset/geo/render")

    body, n = sphere(stage, "/Asset/geo/render/Body", 12, 24, 2.0)
    body.AddRotateXYZOp().Set(Gf.Vec3f(0, 30, 0))
    body.AddScaleOp().Set(Gf.Vec3f(1.5, 1, 1))
    UsdShade.MaterialBindingAPI.Apply(body.GetPrim()).Bind(paint)
    bottom = UsdGeom.Subset.Define(stage, "/Asset/geo/render/Body/bottom")
    bottom.CreateElementTypeAttr(UsdGeom.Tokens.face)
    bottom.CreateFamilyNameAttr("materialBind")
    bottom.CreateIndicesAttr(list(range(n - 24 * 3, n)))
    UsdShade.MaterialBindingAPI.Apply(bottom.GetPrim()).Bind(rubber)

    parts = UsdGeom.Xform.Define(stage, "/Asset/geo/render/Parts")
    parts.AddTranslateOp().Set(Gf.Vec3d(0, 2.2, 0))
    parts.AddScaleOp().Set(Gf.Vec3f(0.5, 0.5, 0.5))
    UsdShade.MaterialBindingAPI.Apply(parts.GetPrim()).Bind(metal)
    sphere(stage, "/Asset/geo/render/Parts/Bolt", 8, 16, 1.0)

    spinner = UsdGeom.Xform.Define(stage, "/Asset/geo/render/Spinner")
    spinner.AddTranslateOp().Set(Gf.Vec3d(3.5, 0, 0))
    rot = spinner.AddRotateXOp()
    rot.Set(0.0, 1)
    rot.Set(180.0, 24)
    wheel, _ = sphere(stage, "/Asset/geo/render/Spinner/Wheel", 10, 20, 0.8)
    wheel.AddScaleOp().Set(Gf.Vec3f(0.4, 1, 1))
    UsdShade.MaterialBindingAPI.Apply(wheel.GetPrim()).Bind(rubber)

    guide = UsdGeom.Scope.Define(stage, "/Asset/geo/guide")
    guide.CreatePurposeAttr(UsdGeom.Tokens.guide)
    helper = UsdGeom.Mesh.Define(stage, "/Asset/geo/guide/Helper")
    helper.CreatePointsAttr([Gf.Vec3f(-3, -2, -3), Gf.Vec3f(3, -2, -3), Gf.Vec3f(3, -2, 3), Gf.Vec3f(-3, -2, 3)])
    helper.CreateFaceVertexCountsAttr([4])
    helper.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    stage.GetRootLayer().Save()


def make_compose(out):
    """An assembly that exercises every composition arc (tests/data/compose):
    a prop asset with a variant set, referenced, payloaded (binary copy),
    instanced, a sublayer, inherits, specializes, an internal reference, an
    inactive prim, an override of a referenced mesh"""
    os.makedirs(out, exist_ok=True)

    # The prop: /Prop/geo/render/Box, material bound inside the asset,
    # variant set "size" (small by default: a scale op in the variant)
    prop = Usd.Stage.CreateNew(os.path.join(out, "prop.usda"))
    UsdGeom.SetStageUpAxis(prop, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(prop, 0.01)
    root = UsdGeom.Xform.Define(prop, "/Prop")
    prop.SetDefaultPrim(root.GetPrim())
    Usd.ModelAPI(root.GetPrim()).SetKind(Kind.Tokens.component)
    mtl = UsdShade.Material.Define(prop, "/Prop/mtl/wood")
    box, _ = sphere(prop, "/Prop/geo/render/Box", 4, 8, 1.0)
    UsdShade.MaterialBindingAPI.Apply(box.GetPrim()).Bind(mtl)
    vset = root.GetPrim().GetVariantSets().AddVariantSet("size")
    for name, s in (("small", 0.5), ("big", 2.0)):
        vset.AddVariant(name)
        vset.SetVariantSelection(name)
        with vset.GetVariantEditContext():
            UsdGeom.Xform(prop.GetPrimAtPath("/Prop/geo")).AddScaleOp().Set(Gf.Vec3f(s, s, s))
    vset.SetVariantSelection("small")
    prop.GetRootLayer().Save()
    Sdf.Layer.FindOrOpen(os.path.join(out, "prop.usda")).Export(os.path.join(out, "prop.usdc"))

    # A sublayer of the assembly: a mesh of its own, an opinion on propA
    extra = Usd.Stage.CreateNew(os.path.join(out, "assembly_extra.usda"))
    over = extra.OverridePrim("/World/propA")
    UsdGeom.Xform(over).AddRotateYOp().Set(45.0)
    e, _ = sphere(extra, "/World/extra", 3, 6, 0.5)
    e.AddTranslateOp().Set(Gf.Vec3d(0, 3, 0))
    extra.GetRootLayer().Save()

    stage = Usd.Stage.CreateNew(os.path.join(out, "assembly.usda"))
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 0.01)
    stage.GetRootLayer().subLayerPaths.append("./assembly_extra.usda")
    world = UsdGeom.Xform.Define(stage, "/World")
    stage.SetDefaultPrim(world.GetPrim())

    a = UsdGeom.Xform.Define(stage, "/World/propA")                 # reference, default prim
    a.GetPrim().GetReferences().AddReference("./prop.usda")
    a.AddTranslateOp().Set(Gf.Vec3d(-4, 0, 0))
    b = UsdGeom.Xform.Define(stage, "/World/propB")                 # reference with prim path, big variant
    b.GetPrim().GetReferences().AddReference("./prop.usda", "/Prop")
    b.GetPrim().GetVariantSets().GetVariantSet("size").SetVariantSelection("big")
    b.AddTranslateOp().Set(Gf.Vec3d(0, 0, 4))
    ob = stage.OverridePrim("/World/propB/geo/render/Box")          # local opinion on referenced points
    UsdGeom.Mesh(ob).GetDisplayColorAttr().Set([Gf.Vec3f(1, 0, 0)])
    c = UsdGeom.Xform.Define(stage, "/World/propC")                 # payload of the binary copy
    c.GetPrim().GetPayloads().AddPayload("./prop.usdc")
    c.AddTranslateOp().Set(Gf.Vec3d(4, 0, 0))
    i = UsdGeom.Xform.Define(stage, "/World/inst")                  # instance
    i.GetPrim().GetReferences().AddReference("./prop.usda")
    i.GetPrim().SetInstanceable(True)
    i.AddTranslateOp().Set(Gf.Vec3d(0, 0, -4))
    off = UsdGeom.Xform.Define(stage, "/World/off")                 # inactive
    off.GetPrim().GetReferences().AddReference("./prop.usda")
    off.GetPrim().SetActive(False)

    cls = stage.CreateClassPrim("/_tree")                           # inherits: a class with a mesh
    leaves, _ = sphere(stage, "/_tree/leaves", 3, 6, 1.5)
    leaves.AddTranslateOp().Set(Gf.Vec3d(0, 2, 0))
    t = UsdGeom.Xform.Define(stage, "/World/tree")
    t.GetPrim().GetInherits().AddInherit("/_tree")
    t.AddTranslateOp().Set(Gf.Vec3d(-4, 0, -4))
    base = stage.CreateClassPrim("/_rock")                          # specializes
    rock, _ = sphere(stage, "/_rock/rock", 3, 5, 0.7)
    r = UsdGeom.Xform.Define(stage, "/World/rock")
    r.GetPrim().GetSpecializes().AddSpecialize("/_rock")
    r.AddTranslateOp().Set(Gf.Vec3d(4, 0, -4))
    copy = UsdGeom.Xform.Define(stage, "/World/treeCopy")           # internal reference
    copy.GetPrim().GetReferences().AddInternalReference("/World/tree")
    copy.AddTranslateOp(opSuffix="copy").Set(Gf.Vec3d(-2, 0, -2))
    stage.GetRootLayer().Save()
    Sdf.Layer.FindOrOpen(os.path.join(out, "assembly.usda")).Export(os.path.join(out, "assembly.usdc"))
    world_obj(os.path.join(out, "assembly.usda"), os.path.join(out, "assembly_world.obj"))


def dump_stage(path):
    """The composed stage, in the canonical form of usd_dump --stage"""
    stage = Usd.Stage.Open(path)
    layer = stage.GetRootLayer()
    fmt_name = os.path.splitext(path)[1][1:]
    if fmt_name == "usd":
        fmt_name = "usdc" if open(path, "rb").read(8) == b"PXR-USDC" else "usda"
    print("FORMAT %s" % fmt_name)
    for key in ("upAxis", "metersPerUnit", "defaultPrim"):
        if layer.pseudoRoot.HasInfo(key):
            print("META %s = %s" % (key, summary(layer.pseudoRoot.GetInfo(key))))
    names = {Sdf.SpecifierDef: "def", Sdf.SpecifierOver: "over", Sdf.SpecifierClass: "class"}
    it = iter(Usd.PrimRange(stage.GetPseudoRoot(), Usd.TraverseInstanceProxies(Usd.PrimAllPrimsPredicate)))
    next(it)   # the pseudo-root
    for prim in it:
        print("PRIM %s %s %s" % (prim.GetPath(), names[prim.GetSpecifier()], prim.GetTypeName()))
        for prop in sorted(prim.GetAuthoredProperties(), key=lambda p: p.GetName()):
            if isinstance(prop, Usd.Relationship):
                print("  REL %s [%s]" % (prop.GetName(), " | ".join(str(p) for p in prop.GetTargets())))
                continue
            uniform = "uniform " if prop.GetVariability() == Sdf.VariabilityUniform else ""
            samples = prop.GetTimeSamples()
            ts = " (timeSamples)" if samples else ""
            print("  ATTR %s %s%s = %s%s" % (prop.GetName(), uniform, prop.GetTypeName(),
                                            summary(prop.Get(Usd.TimeCode.Default())), ts))
            for t in samples:
                print("    SAMPLE %s: %s" % (fmt(t), summary(prop.Get(t))))


def world_obj(path, obj):
    """The meshes of the composed stage at their first frame, in world space,
    counter-clockwise (left-handed meshes reversed), in traversal order,
    without the instances: what Instant Meshes must load from the stage"""
    stage = Usd.Stage.Open(path)
    time = Usd.TimeCode.EarliestTime()
    cache = UsdGeom.XformCache(time)
    base = 1
    with open(obj, "w", newline="\n") as f:
        for prim in stage.Traverse():
            if prim.GetTypeName() != "Mesh" or prim.IsInstanceProxy():
                continue
            mesh = UsdGeom.Mesh(prim)
            m = cache.GetLocalToWorldTransform(prim)
            pts = [m.Transform(Gf.Vec3d(p)) for p in mesh.GetPointsAttr().Get(time)]
            left = mesh.GetOrientationAttr().Get() == UsdGeom.Tokens.leftHanded
            f.write("o %s\n" % prim.GetPath())
            for p in pts:
                f.write("v %.9g %.9g %.9g\n" % (p[0], p[1], p[2]))
            idx, offset = mesh.GetFaceVertexIndicesAttr().Get(time), 0
            for n in mesh.GetFaceVertexCountsAttr().Get(time):
                face = list(idx[offset:offset + n])
                if left:
                    face.reverse()
                f.write("f %s\n" % " ".join(str(base + i) for i in face))
                offset += n
            base += len(pts)


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
        world_obj(usda, os.path.join(out, "usd_scene_world.obj"))
        asset = os.path.join(out, "usd_asset.usda")
        make_asset(asset)
        Sdf.Layer.FindOrOpen(asset).Export(os.path.join(out, "usd_asset.usdc"))
        os.remove(asset)
    print("[OK] usd reference data in", out)
elif args[0] == "dump":
    dump(args[1])
elif args[0] == "compose":
    make_compose(os.path.abspath(args[1]))
    print("[OK] composition data in", args[1])
elif args[0] == "dump_stage":
    dump_stage(args[1])

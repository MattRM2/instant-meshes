"""
make_reference_data.py -- Generate the OBJ/ABC reference dataset for the
Alembic reader/writer tests.

Every case is built from scratch in an empty scene and exported twice:
  <case>.obj  (world space, Y-up, geometry only)   -> what load_obj() sees
  <case>.abc  (Blender defaults: uvs, normals, orcos, ...) -> realistic input

manifest.json records, per case, what the Alembic reader must reproduce:
world-space vertex/face counts, face-size histogram and Y-up bounding box
of every polygon mesh, plus the non-mesh objects it must skip.

Usage (Blender 5.x, headless, no user addons):
    blender -b --factory-startup --python tests/make_reference_data.py -- tests/data [case ...]
"""

import bpy
import bmesh
import json
import math
import os
import sys
from collections import Counter
from mathutils import Matrix

# Blender Z-up -> Alembic / OBJ Y-up: (x, y, z) -> (x, z, -y)
Z_UP_TO_Y_UP = Matrix.Rotation(-math.pi / 2, 4, 'X')


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.frame_start = scene.frame_end = scene.frame_current = 1
    return scene


def apply_all(obj):
    """Bake modifiers into the mesh data so OBJ and ABC see the same geometry."""
    bpy.context.view_layer.objects.active = obj
    for mod in list(obj.modifiers):
        bpy.ops.object.modifier_apply(modifier=mod.name)


def add_material(obj, name, faces=None):
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    obj.data.materials.append(mat)
    slot = len(obj.data.materials) - 1
    if faces is not None:
        for poly in obj.data.polygons:
            if faces(poly):
                poly.material_index = slot


# ---------------------------------------------------------------------------
# Cases
# ---------------------------------------------------------------------------

def case_cube_quads():
    """Smallest closed quad mesh: 24 quads, easy to debug by hand."""
    reset_scene()
    bpy.ops.mesh.primitive_cube_add(size=2)
    obj = bpy.context.object
    obj.name = obj.data.name = "Cube"
    obj.modifiers.new("subd", 'SUBSURF').levels = 1
    apply_all(obj)


def case_ngon_cylinder():
    """N-gon caps (12 sides) + one pentagon and one hexagon on the side wall."""
    reset_scene()
    bpy.ops.mesh.primitive_cylinder_add(vertices=12, radius=1, depth=2,
                                        end_fill_type='NGON')
    obj = bpy.context.object
    obj.name = obj.data.name = "Cylinder"
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.faces.ensure_lookup_table()
    side = [f for f in bm.faces if len(f.verts) == 4]
    # Split one side edge -> neighbouring quads become a pentagon;
    # split one more on another quad -> hexagon.
    e = [e for e in side[0].edges if abs(e.verts[0].co.z - e.verts[1].co.z) < 1e-6][0]
    bmesh.utils.edge_split(e, e.verts[0], 0.5)
    q = side[4]
    for e in [e for e in q.edges if abs(e.verts[0].co.z - e.verts[1].co.z) < 1e-6]:
        bmesh.utils.edge_split(e, e.verts[0], 0.5)
    bm.to_mesh(obj.data)
    bm.free()


def case_suzanne_open():
    """Open mesh (eye holes / boundaries)."""
    reset_scene()
    bpy.ops.mesh.primitive_monkey_add(size=2)
    obj = bpy.context.object
    obj.name = obj.data.name = "Suzanne"
    obj.modifiers.new("subd", 'SUBSURF').levels = 1
    apply_all(obj)


def case_hierarchy():
    """Nested transforms, non-uniform scale, mesh parented under a mesh."""
    reset_scene()
    bpy.ops.object.empty_add(location=(1, 2, 3))
    rig = bpy.context.object
    rig.name = "Rig"
    rig.rotation_euler = (math.radians(30), math.radians(45), math.radians(60))
    rig.scale = (1.0, 2.0, 0.5)

    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=2, radius=1)
    parent = bpy.context.object
    parent.name = parent.data.name = "Body"
    parent.parent = rig
    parent.location = (0.5, -1.0, 0.25)
    parent.rotation_euler = (0, math.radians(20), 0)

    bpy.ops.mesh.primitive_cube_add(size=0.5)
    child = bpy.context.object
    child.name = child.data.name = "Head"
    child.parent = parent
    child.location = (0, 0, 1.5)
    child.scale = (1.0, 1.0, 3.0)


def case_scene_ab():
    """Splice test scene: MeshA (to remesh) + MeshB (must stay intact),
    face sets / materials, and non-mesh objects the reader must skip."""
    reset_scene()
    bpy.ops.object.empty_add(location=(0, 0, 0))
    grp = bpy.context.object
    grp.name = "Props"
    grp.rotation_euler = (0, 0, math.radians(15))

    bpy.ops.mesh.primitive_monkey_add(size=2, location=(-2, 0, 0))
    a = bpy.context.object
    a.name = a.data.name = "MeshA"
    a.parent = grp
    a.modifiers.new("subd", 'SUBSURF').levels = 2
    apply_all(a)
    add_material(a, "MatA")

    bpy.ops.mesh.primitive_torus_add(location=(2, 0, 0))
    b = bpy.context.object
    b.name = b.data.name = "MeshB"
    b.parent = grp
    b.scale = (1.2, 1.2, 1.2)
    add_material(b, "MatB_outer")
    add_material(b, "MatB_inner", faces=lambda p: p.center.x < 0)

    bpy.ops.object.camera_add(location=(0, -8, 2), rotation=(math.radians(80), 0, 0))
    bpy.context.object.name = "Camera"
    bpy.ops.curve.primitive_bezier_circle_add(location=(0, 0, 2))
    bpy.context.object.name = "Curve"


def case_animated():
    """Robustness only (animation is out of scope): animated transform and
    deforming points. The reader must load frame 1 and not choke on samples."""
    reset_scene()
    scene = bpy.context.scene
    scene.frame_end = 10
    bpy.ops.mesh.primitive_cube_add(size=2)
    obj = bpy.context.object
    obj.name = obj.data.name = "Moving"
    obj.location = (0, 0, 0)
    obj.keyframe_insert("location", frame=1)
    obj.location = (3, 0, 0)
    obj.keyframe_insert("location", frame=10)
    obj.shape_key_add(name="Basis")
    key = obj.shape_key_add(name="Squash")
    for v in key.data:
        v.co.z *= 0.3
    key.value = 0.0
    key.keyframe_insert("value", frame=1)
    key.value = 1.0
    key.keyframe_insert("value", frame=10)
    scene.frame_set(1)


def case_instances():
    """A collection instanced twice: exported as Alembic instances
    (".instanceSource"), written out twice in the OBJ."""
    reset_scene()
    coll = bpy.data.collections.new("PillarSet")
    mesh = bpy.data.meshes.new("PillarMesh")
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, segments=8, radius1=0.5, radius2=0.5, depth=1,
                          cap_ends=True)
    bm.to_mesh(mesh)
    bm.free()
    pillar = bpy.data.objects.new("Pillar", mesh)
    coll.objects.link(pillar)
    for name, loc, rot, scale in (("InstA", (-1.5, 0, 0), 0, (1, 1, 1)),
                                  ("InstB", (1.5, 0.5, 0), 30, (1, 1, 2))):
        empty = bpy.data.objects.new(name, None)
        empty.instance_type = 'COLLECTION'
        empty.instance_collection = coll
        empty.location = loc
        empty.rotation_euler = (0, 0, math.radians(rot))
        empty.scale = scale
        bpy.context.scene.collection.objects.link(empty)


def case_subd():
    """Exported with the SubD schema: not a polygon mesh for the reader."""
    reset_scene()
    bpy.ops.mesh.primitive_cube_add(size=2)
    obj = bpy.context.object
    obj.name = obj.data.name = "SubdCube"
    obj.modifiers.new("subd", 'SUBSURF').levels = 1


CASES = {
    "cube_quads": case_cube_quads,
    "ngon_cylinder": case_ngon_cylinder,
    "suzanne_open": case_suzanne_open,
    "hierarchy": case_hierarchy,
    "scene_ab": case_scene_ab,
    "animated": case_animated,
    "instances": case_instances,
    "subd": case_subd,
}

# Per-case Alembic export overrides
ABC_OPTIONS = {
    "subd": {"subdiv_schema": True},
}

# Cases whose Alembic re-import cannot match the evaluated scene (Blender
# imports a SubD object as its base cage, without the subdivision)
NO_ROUNDTRIP = {"subd"}

# ---------------------------------------------------------------------------
# Export + manifest
# ---------------------------------------------------------------------------

def evaluated_meshes():
    """Yields (name, object, world matrix, mesh) for every evaluated mesh,
    collection instances included. The mesh is only valid until the next
    iteration (an instanced object is evaluated once, shared by instances)."""
    depsgraph = bpy.context.evaluated_depsgraph_get()
    items = []
    for inst in depsgraph.object_instances:
        obj = inst.object
        if obj.type != 'MESH':
            continue
        name = obj.name
        if inst.is_instance:
            name = inst.parent.name + "/" + obj.name
        items.append((name, obj, inst.matrix_world.copy()))
    for name, obj, matrix in sorted(items, key=lambda r: r[0]):
        me = obj.to_mesh()
        yield name, obj, matrix, me
        obj.to_mesh_clear()


def describe_scene():
    meshes = []
    for name, obj, matrix, me in evaluated_meshes():
        world = Z_UP_TO_Y_UP @ matrix
        pts = [world @ v.co for v in me.vertices]
        lo = [min(p[i] for p in pts) for i in range(3)]
        hi = [max(p[i] for p in pts) for i in range(3)]
        sizes = Counter(len(p.vertices) for p in me.polygons)
        original = obj.original
        meshes.append({
            "name": name,
            "parent": original.parent.name if original.parent else None,
            "vertices": len(me.vertices),
            "faces": len(me.polygons),
            "face_sizes": {str(k): v for k, v in sorted(sizes.items())},
            "triangles": sum(n - 2 for n in (len(p.vertices) for p in me.polygons)),
            "materials": [m.name for m in original.data.materials if m],
            "bbox_yup": [[round(x, 6) for x in lo], [round(x, 6) for x in hi]],
        })
    skipped = [{"name": o.name, "type": o.type}
               for o in sorted(bpy.context.scene.objects, key=lambda o: o.name)
               if o.type not in ('MESH', 'EMPTY')]
    return meshes, skipped


def export_case(name, outdir):
    base = os.path.join(outdir, name)
    scene = bpy.context.scene
    # OBJ: polygon meshes only (the OBJ exporter would also write curves as
    # 'l' polylines, whose vertices do not exist on the Alembic mesh side).
    for obj in scene.objects:
        obj.select_set(obj.type == 'MESH')
    bpy.ops.wm.obj_export(
        filepath=base + ".obj", check_existing=False, export_selected_objects=True,
        export_animation=False, apply_modifiers=True, apply_transform=True,
        export_uv=False, export_normals=False, export_materials=False,
        export_triangulated_mesh=False,
        forward_axis='NEGATIVE_Z', up_axis='Y')
    options = dict(
        filepath=base + ".abc", check_existing=False,
        start=scene.frame_start, end=scene.frame_end,
        init_scene_frame_range=False,
        face_sets=True, triangulate=False, apply_subdiv=False,
        subdiv_schema=False, export_hair=False, export_particles=False,
        evaluation_mode='VIEWPORT')
    options.update(ABC_OPTIONS.get(name, {}))
    bpy.ops.wm.alembic_export(**options)
    meshes, skipped = describe_scene()
    return {
        "frames": [scene.frame_start, scene.frame_end],
        "meshes": meshes,
        "skipped_objects": skipped,
        "total_vertices": sum(m["vertices"] for m in meshes),
        "total_triangles": sum(m["triangles"] for m in meshes),
    }


def verify_roundtrip(name, outdir, expected):
    """Re-import the .abc in Blender: counts must match what we exported."""
    reset_scene()
    bpy.ops.wm.alembic_import(filepath=os.path.join(outdir, name + ".abc"))
    got = {}
    for mesh_name, obj, matrix, me in evaluated_meshes():
        got[mesh_name] = (len(me.vertices), len(me.polygons))
    want = {m["name"]: (m["vertices"], m["faces"]) for m in expected["meshes"]}
    if sorted(got.values()) != sorted(want.values()):
        raise RuntimeError("%s: Alembic round-trip mismatch %s != %s" % (name, got, want))


def main():
    """Arguments after '--': output directory, then optionally the cases to
    (re)generate; the other cases of an existing manifest are kept."""
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    outdir = os.path.abspath(argv[0] if argv else "tests/data")
    selected = argv[1:] or list(CASES)
    os.makedirs(outdir, exist_ok=True)

    path = os.path.join(outdir, "manifest.json")
    manifest = {"generator": "Blender " + bpy.app.version_string, "cases": {}}
    if os.path.exists(path):
        manifest["cases"] = json.load(open(path))["cases"]
    for name in selected:
        CASES[name]()
        manifest["cases"][name] = export_case(name, outdir)
        if name not in NO_ROUNDTRIP:
            verify_roundtrip(name, outdir, manifest["cases"][name])
        info = manifest["cases"][name]
        print("[OK] %-14s meshes=%d V=%d T=%d" % (
            name, len(info["meshes"]), info["total_vertices"], info["total_triangles"]))

    manifest["cases"] = {k: manifest["cases"][k] for k in CASES if k in manifest["cases"]}
    with open(path, "w", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print("[OK] manifest ->", path)


main()

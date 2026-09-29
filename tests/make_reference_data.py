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
    blender -b --factory-startup --python tests/make_reference_data.py -- tests/data
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


CASES = {
    "cube_quads": case_cube_quads,
    "ngon_cylinder": case_ngon_cylinder,
    "suzanne_open": case_suzanne_open,
    "hierarchy": case_hierarchy,
    "scene_ab": case_scene_ab,
    "animated": case_animated,
}

# ---------------------------------------------------------------------------
# Export + manifest
# ---------------------------------------------------------------------------

def describe_scene():
    depsgraph = bpy.context.evaluated_depsgraph_get()
    meshes, skipped = [], []
    for obj in sorted(bpy.context.scene.objects, key=lambda o: o.name):
        if obj.type != 'MESH':
            if obj.type != 'EMPTY':
                skipped.append({"name": obj.name, "type": obj.type})
            continue
        ev = obj.evaluated_get(depsgraph)
        me = ev.to_mesh()
        world = Z_UP_TO_Y_UP @ ev.matrix_world
        pts = [world @ v.co for v in me.vertices]
        lo = [min(p[i] for p in pts) for i in range(3)]
        hi = [max(p[i] for p in pts) for i in range(3)]
        sizes = Counter(len(p.vertices) for p in me.polygons)
        meshes.append({
            "name": obj.name,
            "parent": obj.parent.name if obj.parent else None,
            "vertices": len(me.vertices),
            "faces": len(me.polygons),
            "face_sizes": {str(k): v for k, v in sorted(sizes.items())},
            "triangles": sum(n - 2 for n in (len(p.vertices) for p in me.polygons)),
            "materials": [m.name for m in obj.data.materials if m],
            "bbox_yup": [[round(x, 6) for x in lo], [round(x, 6) for x in hi]],
        })
        ev.to_mesh_clear()
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
    bpy.ops.wm.alembic_export(
        filepath=base + ".abc", check_existing=False,
        start=scene.frame_start, end=scene.frame_end,
        init_scene_frame_range=False,
        face_sets=True, triangulate=False, apply_subdiv=False,
        subdiv_schema=False, export_hair=False, export_particles=False,
        evaluation_mode='VIEWPORT')
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
    got = {o.name: (len(o.data.vertices), len(o.data.polygons))
           for o in bpy.context.scene.objects if o.type == 'MESH'}
    want = {m["name"]: (m["vertices"], m["faces"]) for m in expected["meshes"]}
    if sorted(got.values()) != sorted(want.values()):
        raise RuntimeError("%s: Alembic round-trip mismatch %s != %s" % (name, got, want))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    outdir = os.path.abspath(argv[0] if argv else "tests/data")
    os.makedirs(outdir, exist_ok=True)

    manifest = {"generator": "Blender " + bpy.app.version_string, "cases": {}}
    for name, build in CASES.items():
        build()
        manifest["cases"][name] = export_case(name, outdir)
        verify_roundtrip(name, outdir, manifest["cases"][name])
        info = manifest["cases"][name]
        print("[OK] %-14s meshes=%d V=%d T=%d" % (
            name, len(info["meshes"]), info["total_vertices"], info["total_triangles"]))

    with open(os.path.join(outdir, "manifest.json"), "w", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print("[OK] manifest ->", os.path.join(outdir, "manifest.json"))


main()

"""
test_cli.py -- End-to-end tests of the Instant Meshes command line (batch
mode) on the reference dataset. Standard library only.

Usage:
    python tests/test_cli.py build/Release/InstantMeshes.exe
Exit code 0 when every check passes.
"""

import math
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(ROOT, "data")

passed = failed = 0


def check(cond, label):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("  FAILED: " + label)


def run(exe, *args):
    """Runs the executable, returns (exit code, combined output)."""
    proc = subprocess.run([exe] + list(args), stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=300)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def face_count(path):
    with open(path) as f:
        return sum(1 for line in f if line.startswith("f "))


def test_abc_input(exe, tmp):
    print("batch mode reads .abc")
    for case in ("cube_quads", "ngon_cylinder", "hierarchy", "scene_ab", "instances"):
        out = os.path.join(tmp, case + "_abc.obj")
        # Explicit target: the default (1/16 of the input vertices) leaves
        # nothing of the smallest cases
        code, log = run(exe, os.path.join(DATA, case + ".abc"), "-o", out, "-d", "-f", "100%")
        check(code == 0 and os.path.exists(out) and face_count(out) > 0,
              "%s.abc: exit %d" % (case, code))


def test_percentage(exe, tmp):
    print("percentage face target (-f N%)")
    out = os.path.join(tmp, "pct.obj")
    for case, polygons in (("scene_ab.abc", 8448), ("suzanne_open.obj", 1968)):
        for pct in (25, 75, 150):
            for mode in ([], ["-D"]):
                if os.path.exists(out):
                    os.remove(out)
                code, log = run(exe, os.path.join(DATA, case), "-o", out, "-d",
                                "-f", "%d%%" % pct, *mode)
                target = polygons * pct / 100.0
                got = face_count(out) if code == 0 and os.path.exists(out) else -1
                error = (got - target) / target
                label = "%s %d%% %s: %d faces for a target of %d (%+.1f%%)" % (
                    case, pct, mode[0] if mode else "quad", got, target, 100 * error)
                check(code == 0 and abs(error) <= 0.10, label)
                check(("of %d input polygons" % polygons) in log, label + " (polygon count)")

    # Plain counts keep their original meaning
    code, log = run(exe, os.path.join(DATA, "suzanne_open.obj"), "-o", out, "-d", "-f", "1000")
    check(code == 0 and "Face count             = 1000" in log, "-f 1000 unchanged")


def test_abc_output(exe, tmp):
    print("batch mode writes .abc")
    dump = os.path.join(os.path.dirname(exe), "abc_dump.exe" if os.name == "nt" else "abc_dump")
    for case, mode in (("scene_ab.abc", []), ("scene_ab.abc", ["-D"]),
                       ("suzanne_open.obj", ["-r", "6", "-p", "6"]), ("ngon_cylinder.obj", ["-D"])):
        out = os.path.join(tmp, "out_%s%s.abc" % (case.split(".")[0], "".join(mode)))
        code, log = run(exe, os.path.join(DATA, case), "-o", out, "-d", "-f", "60%", *mode)
        check(code == 0 and os.path.exists(out) and not os.path.exists(out + ".tmp"),
              "%s %s -> .abc: exit %d" % (case, " ".join(mode), code))
        if os.path.exists(dump) and os.path.exists(out):
            code, log = run(dump, "--verify", out)
            check(code == 0 and "all hashes match" in log, "%s: hashes of the written file" % out)
            code, log = run(dump, out)
            check(log.count("AbcGeom_PolyMesh_v1:.geom") == 1, "%s: one polygon mesh" % out)
        # the written file can be read back and remeshed again
        again = out.replace(".abc", "_again.obj")
        code, log = run(exe, out, "-o", again, "-d", "-f", "100%")
        check(code == 0 and face_count(again) > 0, "%s read back" % out)

    # Output replacing its own input: the input is fully read (and closed)
    # before the atomic replacement
    src = os.path.join(tmp, "inplace.abc")
    with open(os.path.join(DATA, "scene_ab.abc"), "rb") as f, open(src, "wb") as g:
        g.write(f.read())
    code, log = run(exe, src, "-o", src, "-d", "-f", "50%")
    check(code == 0 and not os.path.exists(src + ".tmp"), "in-place .abc output: exit %d" % code)
    code, log = run(exe, src, "-o", os.path.join(tmp, "inplace.obj"), "-d", "-f", "100%")
    check(code == 0 and "input polygons" in log and "of 8448 input" not in log,
          "in-place output is the new mesh")


def plan_lines(log):
    """Plan lines printed by -m / --others: {mesh path: rest of the line}"""
    lines = {}
    for line in log.splitlines():
        line = line.strip()
        if line.startswith("/") and "faces" in line:
            path = line.split()[0]
            lines[path] = line[len(path):]
    return lines


def list_meshes(exe, path):
    code, log = run(exe, path, "--list")
    meshes = {}
    for line in log.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0].startswith("/") and parts[2] == "faces":
            meshes[parts[0]] = int(parts[1])
    return code, meshes


def test_mesh_rules(exe, tmp):
    print("per-mesh remeshing (-m / --others / --list / --dry-run)")
    scene = os.path.join(DATA, "scene_ab.abc")
    A, B = "/Props/MeshA/MeshA", "/Props/MeshB/MeshB"
    dump = os.path.join(os.path.dirname(exe), "abc_dump.exe" if os.name == "nt" else "abc_dump")

    # --list
    code, meshes = list_meshes(exe, scene)
    check(code == 0 and meshes == {A: 7872, B: 576}, "--list scene_ab: %s" % meshes)
    code, log = run(exe, os.path.join(DATA, "instances.abc"), "--list")
    check(code == 0 and log.count("(instanced x2)") == 1 and "/InstB/" not in log,
          "--list: an instanced mesh once, with its instance count")

    # --sort / --top
    def listed(*extra):
        code, log = run(exe, scene, "--list", *extra)
        return code, [l.split()[0] for l in log.splitlines() if l.strip().startswith("/")], log
    code, order, log = listed("--sort", "asc")
    check(code == 0 and order == [B, A] and "(ascending)" in log, "--sort asc: %s" % order)
    code, order, log = listed("--sort", "DESC")
    check(code == 0 and order == [A, B] and "(descending)" in log, "--sort desc: %s" % order)
    code, order, log = listed("--sort", "asc", "--top", "1")
    check(code == 0 and order == [B] and "first 1 shown" in log and ": 2," in log, "--top 1: %s" % order)
    code, order, log = listed("--top", "5")
    check(code == 0 and order == [A, B] and "shown" not in log, "--top larger than the list")
    code, order, log = listed()
    check(code == 0 and order == [A, B] and "face count" not in log, "--list alone keeps the file order")
    for args, expect in ((["--list", "--sort", "big"], "Invalid --sort order"),
                         (["--list", "--top", "0"], "Invalid --top count"),
                         (["--sort", "asc"], "--sort and --top apply to --list"),
                         (["-o", os.path.join(tmp, "e.abc"), "-m", "MeshA=50%", "--top", "3"],
                          "--sort and --top apply to --list")):
        code, log = run(exe, scene, *args)
        check(code != 0 and expect in log and "Optimizing" not in log, "%s -> '%s'" % (" ".join(args), expect))
    code, log = run(exe, os.path.join(DATA, "scene_ab.obj"), "--list", "--sort", "desc", "--top", "1")
    check(code == 0 and "/MeshA" in log and "/MeshB" not in log, "OBJ --list --sort desc --top 1")
    code, log = run(exe, os.path.join(DATA, "animated.abc"), "--list")
    check(code == 0 and "(animated)" in log, "--list flags animation")

    # --dry-run: plan only, nothing written
    out = os.path.join(tmp, "dry.abc")
    code, log = run(exe, scene, "-o", out, "-m", "Mesh*=75%", "-m", "MeshB=85%", "--dry-run")
    plan = plan_lines(log)
    check(code == 0 and not os.path.exists(out) and "Dry run" in log, "--dry-run writes nothing")
    check("75%" in plan.get(A, "") and "Mesh*=75%" in plan.get(A, ""), "wildcard rule on MeshA")
    check("85%" in plan.get(B, "") and "MeshB=85%" in plan.get(B, ""), "last matching rule wins")
    code, log = run(exe, scene, "-m", "/Props/MeshA=50%", "--dry-run")
    plan = plan_lines(log)
    check(code == 0 and "50%" in plan.get(A, "") and "kept unchanged" in plan.get(B, ""),
          "path rule with leading slash, dry run without -o")
    code, log = run(exe, scene, "-m", "Props/*=40%", "--dry-run")
    plan = plan_lines(log)
    check(code == 0 and "40%" in plan.get(A, "") and "40%" in plan.get(B, ""), "Props/* selects both")
    code, log = run(exe, scene, "-m", "Mesh?=30%", "-m", "MeshA=5000", "--dry-run")
    plan = plan_lines(log)
    check(code == 0 and "5000" in plan.get(A, "") and "30%" in plan.get(B, ""), "? wildcard and face count")
    code, log = run(exe, scene, "-m", "MeshA=75%", "--others", "25%", "--dry-run")
    plan = plan_lines(log)
    check(code == 0 and "--others 25%" in plan.get(B, ""), "--others takes the rest")

    # Real run: the example of the specification
    out = os.path.join(tmp, "spliced.abc")
    code, log = run(exe, scene, "-o", out, "-d", "-m", "MeshA=75%", "-m", "MeshB=85%", "--others", "25%")
    check(code == 0 and os.path.exists(out) and not os.path.exists(out + ".tmp"),
          "-m MeshA=75%% -m MeshB=85%% --others 25%%: exit %d" % code)
    code, meshes = list_meshes(exe, out)
    check(set(meshes) == {A, B}, "same meshes after the splice: %s" % meshes)
    check(abs(meshes.get(A, 0) - 5904) <= 0.1 * 5904, "MeshA ~75%%: %s" % meshes.get(A))
    check(abs(meshes.get(B, 0) - 490) <= 0.2 * 490, "MeshB ~85%%: %s" % meshes.get(B))
    if os.path.exists(dump):
        code, log = run(dump, "--verify", out)
        check(code == 0 and "all hashes match" in log, "hashes of the spliced file")
        code, log = run(dump, out)
        check("OBJECT /Camera" in log and "OBJECT /Curve" in log, "non-mesh objects kept")

    # Only MeshA: MeshB copied untouched (same polygon count)
    out = os.path.join(tmp, "only_a.abc")
    code, log = run(exe, scene, "-o", out, "-d", "-m", "MeshA=50%")
    code2, meshes = list_meshes(exe, out)
    check(code == 0 and meshes.get(B) == 576 and meshes.get(A, 0) < 7872, "only MeshA remeshed")

    # In place
    src = os.path.join(tmp, "inplace_rules.abc")
    with open(scene, "rb") as f, open(src, "wb") as g:
        g.write(f.read())
    code, log = run(exe, src, "-o", src, "-d", "-m", "MeshB=200%")
    code2, meshes = list_meshes(exe, src)
    check(code == 0 and meshes.get(A) == 7872 and meshes.get(B, 0) > 576 and
          not os.path.exists(src + ".tmp"), "in-place -m: %s" % meshes)

    # --others alone skips what cannot be remeshed, and says so
    code, log = run(exe, os.path.join(DATA, "animated.abc"), "--others", "50%", "--dry-run")
    check(code != 0 and "Nothing to remesh" in log and "kept unchanged (animated)" in log,
          "--others skips animated meshes")

    # an instanced mesh: remeshed where it is stored, every instance shows it, still an instance
    inst = os.path.join(tmp, "instances_out.abc")
    code, log = run(exe, os.path.join(DATA, "instances.abc"), "-o", inst, "-d", "-m", "InstB/Pillar-0=300%")
    code2, listed = run(exe, inst, "--list")
    code3, dumped = run(dump, inst)
    check(code == 0 and "/InstA/Pillar-0/PillarMesh" in listed and "(instanced x2)" in listed and
          " 10 faces" not in listed and 'instanceSource' in dumped and '"/InstA/Pillar-0/PillarMesh"' in dumped,
          "an instanced Alembic mesh remeshed once, for both instances")

    # Errors, all before any computation
    cases = [
        (["-o", os.path.join(tmp, "e.abc"), "-m", "Nope=50%"], "no polygon mesh matches"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "Mesh=50%"], "no polygon mesh matches"),
        (["-o", os.path.join(tmp, "e.obj"), "-m", "MeshA=50%"], "same format as the input"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "MeshA=50%", "-f", "50%"], "give the face targets there"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "MeshA"], "expected name=target"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "=50%"], "expected name=target"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "MeshA=0%"], "Invalid face percentage"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "MeshA=abc"], "Invalid target"),
        (["-o", os.path.join(tmp, "e.abc"), "--dry-run"], "--dry-run shows the plan"),
    ]
    for args, expect in cases:
        code, log = run(exe, scene, *args)
        check(code != 0 and expect in log and "Optimizing" not in log,
              "%s -> expected '%s'" % (" ".join(args), expect))
    check(not os.path.exists(os.path.join(tmp, "e.abc")), "no output after errors")
    code, log = run(exe, os.path.join(DATA, "cube_quads.ply"), "--list")
    check(code != 0 and "need one Alembic (.abc), OBJ (.obj) or USD" in log, "--list on a PLY refused")
    code, log = run(exe, os.path.join(DATA, "animated.abc"), "-o", os.path.join(tmp, "e.abc"),
                    "-m", "Moving=50%")
    check(code != 0 and "cannot be remeshed" in log, "animated target refused")


def test_obj_rules(exe, tmp):
    print("per-object remeshing of OBJ scenes")
    scene = os.path.join(DATA, "scene_ab.obj")
    A, B = "/MeshA", "/MeshB"

    code, meshes = list_meshes(exe, scene)
    check(code == 0 and meshes == {A: 7872, B: 576}, "--list scene_ab.obj: %s" % meshes)

    code, log = run(exe, scene, "-m", "Mesh*=75%", "-m", "MeshB=85%", "--dry-run")
    plan = plan_lines(log)
    check(code == 0 and "75%" in plan.get(A, "") and "85%" in plan.get(B, ""), "OBJ dry run plan")

    out = os.path.join(tmp, "obj_spliced.obj")
    code, log = run(exe, scene, "-o", out, "-d", "-m", "MeshA=50%")
    code2, meshes = list_meshes(exe, out)
    check(code == 0 and meshes.get(B) == 576 and abs(meshes.get(A, 0) - 3936) <= 0.1 * 3936,
          "OBJ -m MeshA=50%%: %s" % meshes)
    with open(scene) as f:
        original = f.read()
    with open(out) as f:
        spliced = f.read()
    b_vertices = [l for l in original[original.index("o MeshB"):].splitlines() if l.startswith("v ")]
    check(all(("\n" + l + "\n") in spliced for l in b_vertices[:50]) and len(b_vertices) == 576,
          "MeshB vertex lines copied verbatim")

    code, log = run(exe, scene, "-o", out, "-d", "-m", "MeshA=50%", "--others", "150%")
    code2, meshes = list_meshes(exe, out)
    check(code == 0 and meshes.get(B, 0) > 576, "OBJ --others: %s" % meshes)

    src = os.path.join(tmp, "inplace.obj")
    with open(scene, "rb") as f, open(src, "wb") as g:
        g.write(f.read())
    code, log = run(exe, src, "-o", src, "-d", "-m", "MeshB=200%")
    code2, meshes = list_meshes(exe, src)
    check(code == 0 and meshes.get(A) == 7872 and meshes.get(B, 0) > 576 and
          not os.path.exists(src + ".tmp"), "in-place OBJ: %s" % meshes)

    code, log = run(exe, scene, "-o", os.path.join(tmp, "x.abc"), "-m", "MeshA=50%")
    check(code != 0 and "same format as the input" in log, "OBJ input needs an OBJ output")
    code, log = run(exe, scene, "-o", out, "-m", "Nope=50%")
    check(code != 0 and "no polygon mesh matches" in log, "OBJ rule without match")


def test_skip_failed(exe, tmp):
    print("--skip-failed copies the meshes that cannot be remeshed")
    for scene, A, B in ((os.path.join(DATA, "scene_ab.abc"), "/Props/MeshA/MeshA", "/Props/MeshB/MeshB"),
                        (os.path.join(DATA, "scene_ab.obj"), "/MeshA", "/MeshB")):
        ext = os.path.splitext(scene)[1]
        out = os.path.join(tmp, "skip" + ext)
        # MeshB=1 face: no faces come out of the extraction
        code, log = run(exe, scene, "-o", out, "-d", "-m", "MeshA=50%", "-m", "MeshB=1")
        check(code != 0 and "produced no faces" in log and not os.path.exists(out),
              "%s: without --skip-failed a failed mesh stops everything" % ext)
        check(not os.path.exists(out + ".spool.tmp") and not os.path.exists(out + ".tmp"),
              "%s: no temporary file left after a failure" % ext)
        code, log = run(exe, scene, "-o", out, "-d", "-m", "MeshA=50%", "-m", "MeshB=1", "--skip-failed")
        code2, meshes = list_meshes(exe, out)
        check(code == 0 and meshes.get(B) == 576 and abs(meshes.get(A, 0) - 3936) <= 0.1 * 3936,
              "%s --skip-failed: %s" % (ext, meshes))
        check("Skipped 1 of 2 meshes" in log and B + ": Remeshing" in log, "%s: skip summary" % ext)
        check(not os.path.exists(out + ".spool.tmp"), "%s: spool file removed" % ext)
        # Every selected mesh failing still writes the (unchanged) scene
        out = os.path.join(tmp, "skip_all" + ext)
        code, log = run(exe, scene, "-o", out, "-d", "-m", "MeshB=1", "--skip-failed")
        code2, meshes = list_meshes(exe, out)
        check(code == 0 and meshes == {A: 7872, B: 576}, "%s: all skipped, scene copied: %s" % (ext, meshes))
    # --progress: a block after each mesh, skipped ones included, 100% at the end
    scene = os.path.join(DATA, "scene_ab.abc")
    out = os.path.join(tmp, "progress.abc")
    code, log = run(exe, scene, "-o", out, "-d", "-m", "MeshA=50%", "-m", "MeshB=1", "--skip-failed", "--progress")
    lines = [l for l in log.splitlines() if l.startswith(">>> Progress")]
    check(code == 0 and len(lines) == 3 and "  0%" in lines[0] and "0/2 meshes" in lines[0],
          "--progress: start block (%s)" % lines[:1])
    check(len(lines) == 3 and "1/2 meshes" in lines[1] and "/Props/MeshA/MeshA done" in lines[1] and
          "left" in lines[1] and "%" in lines[1], "--progress: after MeshA (%s)" % lines[1:2])
    check(len(lines) == 3 and "100%" in lines[2] and "2/2 meshes" in lines[2] and
          "/Props/MeshB/MeshB skipped" in lines[2] and "[" + "#" * 30 + "]" in lines[2],
          "--progress: 100%% at the end (%s)" % lines[2:])
    check(log.count(">" * 74) == 6, "--progress: blocks of three lines")
    # weighted by input faces: MeshA holds 7872 of 8448 faces
    check(len(lines) == 3 and " 93%" in lines[1], "--progress: weighted by faces (%s)" % lines[1:2])
    code, log = run(exe, scene, "-o", out, "-d", "-m", "MeshA=50%")
    check(">>> Progress" not in log, "no progress block without --progress")
    code, log = run(exe, os.path.join(DATA, "cube_quads.obj"), "-o", os.path.join(tmp, "e.obj"),
                    "-f", "50%", "--progress")
    check(code != 0 and "--progress applies to the -m / --others" in log, "--progress needs -m / --others")
    code, log = run(exe, os.path.join(DATA, "cube_quads.obj"), "-o", os.path.join(tmp, "e.obj"),
                    "-f", "50%", "--skip-failed")
    check(code != 0 and "--skip-failed applies to the -m / --others" in log, "--skip-failed needs -m / --others")


def plates_obj(path):
    """Two floor plates touching along x = 1 (different tessellations) and a
    third one along their top side, as quads"""
    def grid(x0, x1, y0, y1, nx, ny):
        v = [(x0 + (x1 - x0) * i / nx, y0 + (y1 - y0) * j / ny) for j in range(ny + 1) for i in range(nx + 1)]
        f = [(j * (nx + 1) + i, j * (nx + 1) + i + 1, (j + 1) * (nx + 1) + i + 1, (j + 1) * (nx + 1) + i)
             for j in range(ny) for i in range(nx)]
        return v, f
    base = 1
    with open(path, "w") as out:
        for name, (v, f) in (("PlateA", grid(0, 1, 0, 1, 12, 12)), ("PlateB", grid(1, 2.3, 0, 1, 17, 9)),
                             ("PlateC", grid(0, 2.3, 1, 1.7, 25, 7))):
            out.write("o %s\n" % name)
            for x, y in v:
                out.write("v %.9g %.9g 0\n" % (x, y))
            for face in f:
                out.write("f %s\n" % " ".join(str(base + i) for i in face))
            base += len(v)


def border_gap(original, remeshed):
    """Largest distance between the open borders of the same objects in two
    OBJ files, both ways (a remeshed border off the original one, or a part
    of the original border no longer covered)"""
    def read(path):
        verts, objects, current = [], {}, None
        for line in open(path):
            parts = line.split()
            if parts and parts[0] == "o":
                current = objects.setdefault(parts[1], [])
            elif parts and parts[0] == "v":
                verts.append(tuple(float(x) for x in parts[1:4]))
            elif parts and parts[0] == "f":
                current.append([int(p.split("/")[0]) - 1 for p in parts[1:]])
        return verts, objects

    def border(verts, faces):
        edges = set((f[i], f[(i + 1) % len(f)]) for f in faces for i in range(len(f)))
        return [(verts[a], verts[b]) for a, b in edges if (b, a) not in edges and a != b]

    def dist(p, a, b):
        d = [b[i] - a[i] for i in range(3)]
        dd = sum(x * x for x in d)
        t = 0 if dd == 0 else max(0, min(1, sum((p[i] - a[i]) * d[i] for i in range(3)) / dd))
        return math.sqrt(sum((a[i] + d[i] * t - p[i]) ** 2 for i in range(3)))

    def one_way(src, dst):
        pts = [tuple(a[i] + (b[i] - a[i]) * k / 4 for i in range(3)) for a, b in src for k in range(5)]
        return max(min(dist(p, a, b) for a, b in dst) for p in pts)

    v0, o0 = read(original)
    v1, o1 = read(remeshed)
    worst = 0
    for name in o0:
        b0, b1 = border(v0, o0[name]), border(v1, o1.get(name, []))
        if not b1:
            return float("inf")
        worst = max(worst, one_way(b0, b1), one_way(b1, b0))
    return worst


def test_keep_border(exe, tmp):
    print("--keep-border keeps touching objects closed")
    scene = os.path.join(tmp, "plates.obj")
    plates_obj(scene)
    rules = ["-m", "PlateA=50%", "-m", "PlateB=30%", "-m", "PlateC=80%"]
    out = os.path.join(tmp, "plates_out.obj")
    code, log = run(exe, scene, "-o", out, "-d", *rules)
    check(code == 0 and border_gap(scene, out) > 1e-3, "without --keep-border the borders move")
    for mode in ([], ["-D"], ["-r", "6", "-p", "6"]):
        code, log = run(exe, scene, "-o", out, "-d", "--keep-border", *rules, *mode)
        gap = border_gap(scene, out) if code == 0 else float("inf")
        check(gap < 1e-5, "--keep-border %s: border gap %.2e" % (" ".join(mode), gap))
        check(log.count("Keep border:") == 3 and "Keep border            = yes" in log,
              "--keep-border %s: reported" % " ".join(mode))
    # A target far above the input density: the input is subdivided first, and the summary says so
    code, log = run(exe, scene, "-o", out, "-d", "-m", "PlateA=800%")
    check(code == 0 and "Warning: the input was subdivided" in log and
          "Input subdivided before remeshing" in log and "/PlateA: 288 -> " in log, "subdivided input reported")
    # Whole file, Alembic output
    out_abc = os.path.join(tmp, "plates_out.abc")
    code, log = run(exe, scene, "-o", out_abc, "-d", "-f", "50%", "--keep-border")
    check(code == 0 and "Keep border: " in log and "input corners added" in log, "--keep-border whole file .abc")
    dump = os.path.join(os.path.dirname(exe), "abc_dump.exe" if os.name == "nt" else "abc_dump")
    if os.path.exists(dump):
        code, log = run(dump, "--verify", out_abc)
        check(code == 0 and "all hashes match" in log, "--keep-border .abc hashes")
    # Refused before any computation
    code, log = run(exe, scene, "-o", os.path.join(tmp, "x.ply"), "--keep-border")
    check(code != 0 and "needs an .obj, .abc or USD output" in log and "Loading" not in log, "--keep-border refuses .ply")
    code, log = run(exe, scene, "--keep-border")
    check(code != 0 and "applies to the batch mode" in log, "--keep-border needs -o")


def test_uv_transfer(exe, tmp):
    print("--uv transfer carries the UVs over to the remeshed meshes")
    dump = os.path.join(os.path.dirname(exe), "abc_dump.exe" if os.name == "nt" else "abc_dump")
    scene = os.path.join(DATA, "uv_sets.abc")

    # Per object, Alembic: both maps of the cylinder, the grid's map; NoUV has none
    out = os.path.join(tmp, "uv_out.abc")
    code, log = run(exe, scene, "-o", out, "-d", "--others", "80%", "--uv", "transfer")
    check(code == 0 and "UV transfer: 2 sets (UVMap, Planar)" in log and "UV transfer: 1 set (UVMap)" in log
          and "/NoUV/NoUV has no UVs" in log, "per-object .abc transfer reported")
    check("UVs                    = transferred from the input" in log, "--uv in the settings")
    if os.path.exists(dump):
        code, log = run(dump, "--verify", out)
        check(code == 0 and "all hashes match" in log, "transferred .abc: hashes")
        code, log = run(dump, out)
        check(log.count("uv {compound}") == 2 and "Planar {compound}" in log, "transferred .abc: uv and Planar written")

    # Per object, OBJ: "vt" for the remeshed objects
    out = os.path.join(tmp, "uv_out.obj")
    code, log = run(exe, os.path.join(DATA, "uv_sets.obj"), "-o", out, "-d", "-m", "Cylinder=80%", "--uv", "transfer")
    with open(out) as f:
        text = f.read()
    cyl = text[text.index("o Cylinder"):text.index("o ", text.index("o Cylinder") + 1)]
    faces = [l for l in cyl.splitlines() if l.startswith("f ")]
    check(code == 0 and faces and all("/" in l for l in faces) and "\nvt " in cyl, "per-object .obj: v/vt faces")

    # Whole file: scene_ab (both meshes have UVMap) -> OBJ with UVs
    out = os.path.join(tmp, "uv_whole.obj")
    code, log = run(exe, os.path.join(DATA, "scene_ab.abc"), "-o", out, "-d", "-f", "30%", "--uv", "transfer")
    with open(out) as f:
        text = f.read()
    check(code == 0 and "UV transfer: 1 set (UVMap)" in log and text.count("\nvt ") > 100, "whole-file transfer")

    # Without --uv: no UVs, as before
    out = os.path.join(tmp, "uv_none.obj")
    code, log = run(exe, os.path.join(DATA, "scene_ab.abc"), "-o", out, "-d", "-f", "30%")
    with open(out) as f:
        check(code == 0 and "\nvt " not in f.read() and "UV transfer" not in log, "no UVs without --uv")

    # --uv unwrap: new UVs for every remeshed mesh, NoUV included
    out = os.path.join(tmp, "uv_unwrap.abc")
    code, log = run(exe, scene, "-o", out, "-d", "--others", "80%", "--uv", "unwrap")
    check(code == 0 and log.count("UV unwrap: ") == 3 and "UVs                    = unwrapped (xatlas)" in log,
          "per-object unwrap reported")
    if os.path.exists(dump):
        code, log = run(dump, "--verify", out)
        check(code == 0 and "all hashes match" in log, "unwrapped .abc: hashes")
        code, log = run(dump, out)
        check(log.count("uv {compound}") == 3 and "sourceName=UVMap" in log, "unwrapped .abc: one UVMap per mesh")
    out = os.path.join(tmp, "uv_unwrap.obj")
    code, log = run(exe, os.path.join(DATA, "suzanne_open.obj"), "-o", out, "-d", "-f", "40%", "--uv", "unwrap")
    with open(out) as f:
        text = f.read()
    faces = [l for l in text.splitlines() if l.startswith("f ")]
    check(code == 0 and "UV unwrap: " in log and faces and all("/" in l for l in faces), "whole-file unwrap (.obj)")
    uvs = [tuple(float(x) for x in l.split()[1:3]) for l in text.splitlines() if l.startswith("vt ")]
    check(uvs and all(-1e-4 <= u <= 1 + 1e-4 and -1e-4 <= v <= 1 + 1e-4 for u, v in uvs), "unwrapped UVs in [0, 1]")

    # Refused before any computation
    for args, expect in ((["-o", os.path.join(tmp, "x.obj"), "--uv", "bogus"], "(none, transfer or unwrap)"),
                         (["-o", os.path.join(tmp, "x.ply"), "--uv", "transfer"], "PLY has no per-corner UVs"),
                         (["--uv", "transfer"], "--uv applies to the batch mode")):
        code, log = run(exe, scene, *args)
        check(code != 0 and expect in log and "Optimizing" not in log, "%s -> '%s'" % (" ".join(args), expect))


def test_usd_reader(exe, tmp):
    print("USD reader: the same layer in .usda / .usdc / .usdz, as Pixar's USD reads it")
    dump = os.path.join(os.path.dirname(exe), "usd_dump.exe" if os.name == "nt" else "usd_dump")
    if not os.path.exists(dump):
        return
    with open(os.path.join(DATA, "usd_scene.dump.txt")) as f:
        want = [l.rstrip("\n") for l in f]
    for name in ("usd_scene.usda", "usd_scene.usdc", "usd_scene.usdz"):
        code, log = run(dump, os.path.join(DATA, name))
        got = [l for l in log.replace("\r\n", "\n").split("\n") if l and not l.startswith("FORMAT")]
        check(code == 0 and got == want, "%s: %d lines, %d different" % (
            name, len(got), sum(1 for a, b in zip(got, want) if a != b) + abs(len(got) - len(want))))


def test_usd_scenes(exe, tmp):
    print("USD scenes: --list, per-object .usda layer, whole-file mode")
    import re
    import shutil
    want = {"/World/geo/MeshA": 10, "/World/geo/Group/MeshB": 5, "/World/geo/MeshC": 1200,
            "/World/geo/MeshD": 1, "/World/geo/Spin/MeshE": 1, "/World/geo/Inst/Box": 1, "/World/geo/Var/Lod": 1}
    for ext in ("usda", "usdc", "usdz"):
        src = os.path.join(tmp, "scene." + ext)
        shutil.copy(os.path.join(DATA, "usd_scene." + ext), src)
        code, meshes = list_meshes(exe, src)
        check(code == 0 and meshes == want, "--list usd_scene.%s: %s" % (ext, meshes))

        out = os.path.join(tmp, "retopo_%s.usda" % ext)
        code, log = run(exe, src, "-o", out, "-d", "-m", "MeshC=50%", "-m", "MeshA=300%", "--uv", "transfer")
        text = open(out, encoding="utf-8").read() if os.path.exists(out) else ""
        counts = re.findall(r"int\[\] faceVertexCounts = \[([^\]]*)\]", text)
        check(code == 0 and "subLayers" in text and "@./scene.%s@" % ext in text and
              len(counts) == 2 and abs(len(counts[1].split(",")) - 600) <= 120 and
              'over "MeshB"' not in text and "normals = None" in text and "texCoord2f[] primvars:st" in text,
              "%s -m -> .usda layer (%d meshes overridden)" % (ext, len(counts)))

        code, log = run(exe, src, "-o", src, "-m", "MeshA=50%")
        check(code != 0 and ("holds every file it uses" if ext == "usdz" else "cannot replace its input") in log,
              "%s: layer over itself refused" % ext)
        code, log = run(exe, src, "-o", os.path.join(tmp, "x.obj"), "-m", "MeshA=50%")
        check(code != 0 and "write a .usda or .usdc layer" in log, "%s: per-object .obj output refused" % ext)

    # whole file: every mesh merged, to OBJ, Alembic and USD
    src = os.path.join(DATA, "usd_scene.usdc")
    for ext in ("obj", "abc", "usda"):
        out = os.path.join(tmp, "whole_usd." + ext)
        code, log = run(exe, src, "-o", out, "-d", "-f", "100%")
        code2, meshes = list_meshes(exe, out) if ext != "obj" else (0, {"/": face_count(out)})
        check(code == 0 and code2 == 0 and sum(meshes.values()) > 300, "whole USD -> .%s: %s" % (ext, meshes))

    # an .usda written from an OBJ is a stage of its own, read back
    out = os.path.join(tmp, "cube.usda")
    code, log = run(exe, os.path.join(DATA, "cube_quads.obj"), "-o", out, "-d", "-f", "100%")
    code2, meshes = list_meshes(exe, out)
    check(code == 0 and code2 == 0 and len(meshes) == 1 and list(meshes.values())[0] > 0,
          "OBJ -> .usda read back: %s" % meshes)
    # units: an OBJ is in meters (USD readers take centimeters when none is
    # written); a USD input keeps its own
    text = open(out, encoding="utf-8").read() if os.path.exists(out) else ""
    check("metersPerUnit = 1\n" in text and 'upAxis = "Y"' in text, "OBJ -> .usda in meters")
    whole = os.path.join(tmp, "whole_usd.usda")
    text = open(whole, encoding="utf-8").read() if os.path.exists(whole) else ""
    check("metersPerUnit = 0.01\n" in text, "USD -> .usda keeps the units of the input")



def test_usd_proxies(exe, tmp):
    print("USD proxies: --proxy")
    import shutil
    dump = os.path.join(os.path.dirname(exe), "usd_dump.exe" if os.name == "nt" else "usd_dump")
    original = os.path.join(DATA, "usd_asset.usdc")
    src = os.path.join(tmp, "asset.usdc")
    shutil.copy(original, src)
    copy = os.path.join(tmp, "asset_px.usdc")
    layer = os.path.join(tmp, "asset_px_proxy.usdc")
    with open(original, "rb") as f:
        before = f.read()

    def read(path):
        if not os.path.exists(path):
            return b""
        with open(path, "rb") as f:
            return f.read()

    def dumped(*args):
        code, out = run(dump, *args)
        return out if code == 0 else ""

    code, log = run(exe, src, "-o", copy, "--proxy", "--others", "40%", "--dry-run")
    check(code == 0 and "get a proxy" in log and "proxy: /Asset/geo/proxy/Body" in log and
          "proxy: /Asset/geo/proxy/Spinner/Wheel" in log and "kept unchanged (guide)" in log and
          "asset_px_proxy.usdc\", referenced by" in log and "a copy of the scene" in log and
          not os.path.exists(copy) and not os.path.exists(layer), "--proxy --dry-run plan")

    # another name: a copy of the scene, that references the proxy layer
    code, log = run(exe, src, "-o", copy, "-d", "--proxy", "--others", "40%")
    after = read(copy)
    check(code == 0 and read(src) == before, "--proxy -o another name: the scene is not touched")
    check(len(after) > len(before) and after[:16] == before[:16] and after[24:len(before)] == before[24:],
          "the copy: the scene's bytes, then what is appended (references, new tables)")
    text = dumped(layer)
    check('PRIM /Asset/geo/proxy def Scope' in text and "PRIM /Asset/geo/proxy/Body def Mesh" in text and
          "REL proxyPrim [/Asset/geo/proxy/Body]" in text and "ATTR purpose uniform token = s1 [proxy]" in text and
          "ATTR purpose uniform token = s1 [render]" in text and "ATTR primvars:st texCoord2f[]" in text and
          "ATTR xformOp:rotateX float = - (timeSamples)" in text and "REL material:binding [/Asset/mtl/paint]" in text
          and "Helper" not in text and "subLayers" not in text,
          "the proxy layer (UVs transferred by default, transforms copied, no sublayer)")
    stage = dumped("--stage", copy)
    check("PRIM /Asset/geo/proxy/Spinner/Wheel def Mesh" in stage and
          "REL proxyPrim [/Asset/geo/proxy/Parts/Bolt]" in stage and "Helper_proxy" not in stage,
          "the copy composes the proxies (reference to the proxy layer)")

    # the scene's own name: edited in place, numbered layers for the next runs
    code, log = run(exe, src, "-o", src, "-d", "--proxy", "-m", "Bolt=50%", "--uv", "none")
    text = dumped(os.path.join(tmp, "asset_proxy.usdc"))
    after = read(src)
    check(code == 0 and "PRIM /Asset/geo/proxy/Parts/Bolt def Mesh" in text and "primvars:st" not in text and
          "proxy/Body" not in text and after[24:len(before)] == before[24:] and "the scene itself" in log,
          "--proxy -o the scene: edited in place (--uv none: no UVs)")
    code, log = run(exe, src, "-o", src, "-d", "--proxy", "-m", "Asset/geo/render/Body=40%")
    stage = dumped("--stage", src)
    check(code == 0 and os.path.exists(os.path.join(tmp, "asset_proxy2.usdc")) and
          "PRIM /Asset/geo/proxy/Body def Mesh" in stage and "PRIM /Asset/geo/proxy/Parts/Bolt def Mesh" in stage,
          "a second run: asset_proxy2.usdc, both referenced")
    code, log = run(exe, src, "-o", src, "-d", "--proxy", "-m", "Asset/geo/render/Body=40%")
    check(code != 0 and "already exists" in log and "Optimizing" not in log, "the same proxy again is refused")

    for args, expect in ((["-m", "Helper=50%"], "cannot get a proxy"),
                         ([], "--proxy adds proxies"),
                         (["-f", "50%"], "--proxy adds proxies")):
        code, log = run(exe, original, "-o", copy, "--proxy", *args)
        check(code != 0 and expect in log and "Optimizing" not in log, "--proxy %s -> '%s'" % (" ".join(args), expect))
    for out, expect in (("asset.usda", "with its format"), ("asset.usdz", "not a .usdz")):
        code, log = run(exe, original, "-o", os.path.join(tmp, out), "--proxy", "--others", "40%")
        check(code != 0 and expect in log and "Optimizing" not in log, "--proxy -o %s refused" % out)
    code, log = run(exe, os.path.join(DATA, "usd_scene.usdz"), "-o", os.path.join(tmp, "z.usdc"), "--proxy",
                    "--others", "40%")
    check(code != 0 and "proxy layer inside" in log and "Optimizing" not in log, "a package gives a package")

    # .usdz: the proxy layer inside the package, its other files as they were
    import zipfile

    def entries(path):
        with zipfile.ZipFile(path) as z:
            return [(i.filename, z.read(i.filename), i.header_offset) for i in z.infolist()]

    for name, root in (("usd_package.usdz", "scene.usda"), ("usd_package_c.usdz", "scene.usdc")):
        package = os.path.join(tmp, name)
        shutil.copy(os.path.join(DATA, name), package)
        before = entries(package)
        code, log = run(exe, package, "-o", package, "-d", "--proxy", "--others", "40%")
        after = entries(package)
        layer = name.replace(".usdz", "_proxy.usdc")
        names = [e[0] for e in after]
        kept = dict((e[0], e[1]) for e in before)
        with open(package, "rb") as f:
            data = f.read()
        aligned = all((e[2] + 30 + int.from_bytes(data[e[2] + 26:e[2] + 28], "little") +
                       int.from_bytes(data[e[2] + 28:e[2] + 30], "little")) % 64 == 0 for e in after)
        check(code == 0 and names == [root, "parts/asset.usdc", "textures/checker.png", layer] and
              all(e[1] == kept[e[0]] for e in after[1:3]) and after[0][1] != kept[root] and aligned and
              "the package itself" in log, "%s: the proxy layer inside, the other files kept, 64-byte aligned" % name)
        code, meshes = list_meshes(exe, package)
        check(code == 0 and "/World/geo/proxy/Body" in meshes and "/World/geo/render/Body" in meshes,
              "%s: its proxies composed (layers read inside the package)" % name)
    copy = os.path.join(tmp, "package_copy.usdz")
    code, log = run(exe, os.path.join(DATA, "usd_package_c.usdz"), "-o", copy, "-d", "--proxy", "-m", "Body=40%")
    check(code == 0 and os.path.exists(copy) and [e[0] for e in entries(copy)][-1] == "package_copy_proxy.usdc",
          "a copy of a package")
    code, log = run(exe, os.path.join(DATA, "scene_ab.abc"), "-o", os.path.join(tmp, "x.abc"), "--proxy",
                    "--others", "50%")
    check(code != 0 and "--proxy adds proxies" in log, "--proxy refused on Alembic")

    # .usda, in place: the reference is the only change; proxies next to the meshes without geo/render
    scene = os.path.join(tmp, "pscene.usda")
    shutil.copy(os.path.join(DATA, "usd_scene.usda"), scene)
    with open(scene, encoding="utf-8") as f:
        before = f.read()
    code, log = run(exe, scene, "-o", scene, "-d", "--proxy", "-m", "MeshA=300%", "-m", "MeshC=30%")
    with open(scene, encoding="utf-8") as f:
        after = f.read()
    path = os.path.join(tmp, "pscene_proxy.usda")
    text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
    ref = "prepend references = @./pscene_proxy.usda@</World>"
    check(code == 0 and ref in after and 'def Mesh "MeshA_proxy"' in text and
          "rel proxyPrim = </World/geo/MeshA_proxy>" in text and "subLayers" not in text,
          "--proxy next to the mesh, in a .usda scene")
    check([l for l in after.split("\n") if ref not in l] == before.split("\n"),
          "the .usda scene: only the reference added")

    # materials outside the root prim of the proxies: stand-ins that reference them
    looks = os.path.join(tmp, "looks.usda")
    shutil.copy(os.path.join(DATA, "usd_looks.usda"), looks)
    code, log = run(exe, looks, "-o", looks, "-d", "--proxy", "--others", "40%")
    path = os.path.join(tmp, "looks_proxy.usda")
    text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
    check(code == 0 and 'def Scope "proxy_materials"' in text and 'def Material "paint" (' in text and
          "prepend references = @./looks.usda@</materials/paint>" in text and 'def Material "paint_2" (' in text and
          "prepend references = @./looks.usda@</materials/car/paint>" in text and
          "rel material:binding = </World/proxy_materials/paint>" in text and
          "rel material:binding = </World/proxy_materials/paint_2>" in text and
          "rel material:binding = </World/mtl/rubber>" in text and "proxy_materials/rubber" not in text,
          "materials outside the root prim: stand-ins that reference them")


def test_usd_instances(exe, tmp):
    print("USD instances: native (external, internal with variants), PointInstancer, nested")
    import shutil
    work = os.path.join(tmp, "instances")
    os.makedirs(work)
    for name in ("usd_instances.usda", "usd_instances.usdc", "usd_asset.usdc"):
        shutil.copy(os.path.join(DATA, name), work)
    scene = os.path.join(work, "usd_instances.usda")

    code, log = run(exe, scene, "--list")
    check(code == 0 and log.count("(instanced x3)") == 6 and "(instanced x2)" in log and
          log.count("(nested instance)") == 2 and "/World/rocks/rock_2/" not in log and
          "/World/props/asset_3/" not in log, "--list: one entry per prototype, nested instances apart")

    out = os.path.join(work, "retopo.usda")
    code, log = run(exe, scene, "-o", out, "-d", "-m", "World/rocks/*=50%", "-m", "World/props/asset_2/geo/render/Body=40%",
                    "-m", "Pebble=50%")
    text = open(out, encoding="utf-8").read() if os.path.exists(out) else ""
    check(code == 0 and text.count('class "_IM_') == 4 and text.count("prepend inherits = </_IM_rock_1_") == 3 and
          text.count("prepend inherits = </_IM_asset_1_") == 3 and 'over "Pebble"' in text and
          "same geometry as /World/rocks/rock_1/geo: its result reused" in log,
          "native instances through classes, PointInstancer prototype in place, identical prototypes once")
    code, log = run(exe, scene, "-o", out, "-m", "World/cluster_1/*=50%")
    check(code != 0 and "nested instances" in log and "Optimizing" not in log, "nested instances refused")

    code, log = run(exe, scene, "-o", scene, "-d", "--proxy", "-m", "World/rocks/rock_6=50%", "-m", "Stick=50%")
    proxies = os.path.join(work, "usd_instances_proxy.usda")
    text = open(proxies, encoding="utf-8").read() if os.path.exists(proxies) else ""
    code2, meshes = list_meshes(exe, scene)
    check(code == 0 and 'class "_IM_rock_6_' in text and "prepend inherits = </World/_IM_rock_6_" in text and
          "/World/rocks/rock_6/geo_proxy" in meshes and "/World/scatter/Prototypes/Stick/mesh_proxy" in meshes,
          "--proxy: proxies inside the prototypes")

    code, log = run(exe, os.path.join(work, "usd_instances.usdc"), "-o", os.path.join(work, "whole.usda"), "-d",
                    "-f", "50%")
    check(code == 0 and "instanced meshes skipped" in log, "whole-file mode: instanced meshes skipped (said)")


def test_thin_objects(exe, tmp):
    print("thin objects: the face target kept, no crash (Kitchen Set boxes and bags)")
    # a closed box 30 x 20 x 2: at a low target its edge length exceeds its thickness
    path = os.path.join(tmp, "thinbox.obj")
    V, F = [], []

    def grid(origin, du, dv, nu, nv, flip):
        base = len(V)
        for j in range(nv + 1):
            for i in range(nu + 1):
                V.append([origin[k] + du[k] * i / nu + dv[k] * j / nv for k in range(3)])
        for j in range(nv):
            for i in range(nu):
                a = base + j * (nu + 1) + i
                q = [a, a + 1, a + nu + 2, a + nu + 1]
                F.append(q[::-1] if flip else q)
    X, Y, Z = 30.0, 20.0, 2.0
    grid([0, 0, 0], [X, 0, 0], [0, Y, 0], 30, 20, True)
    grid([0, 0, Z], [X, 0, 0], [0, Y, 0], 30, 20, False)
    grid([0, 0, 0], [X, 0, 0], [0, 0, Z], 30, 1, False)
    grid([0, Y, 0], [X, 0, 0], [0, 0, Z], 30, 1, True)
    grid([0, 0, 0], [0, Y, 0], [0, 0, Z], 20, 1, True)
    grid([X, 0, 0], [0, Y, 0], [0, 0, Z], 20, 1, False)
    with open(path, "w") as f:
        for v in V:
            f.write("v %g %g %g\n" % tuple(v))
        for q in F:
            f.write("f " + " ".join(str(i + 1) for i in q) + "\n")
    out = os.path.join(tmp, "thinbox_out.obj")
    code, log = run(exe, path, "-o", out, "-d", "-f", "5%")
    faces = sum(1 for l in open(out) if l.startswith("f ")) if os.path.exists(out) else 0
    target = len(F) * 0.05
    check(code == 0 and 0.7 <= faces / target <= 1.45 and "Face target missed" in log,
          "a thin box at 5%%: %d faces for ~%d (corrected)" % (faces, target))
    for args in (["-D", "-f", "5%"], ["-D", "-f", "50"], ["-f", "400"], ["-r", "6", "-p", "6", "-f", "5%"]):
        code, log = run(exe, path, "-o", out, "-d", *args)
        check(code == 0 and os.path.exists(out), "a thin box, %s: no crash" % " ".join(args))


def test_usd_composition(exe, tmp):
    print("USD composition: sublayers, references, payloads, variants, inherits, specializes")
    import shutil
    dump = os.path.join(os.path.dirname(exe), "usd_dump.exe" if os.name == "nt" else "usd_dump")
    data = os.path.join(DATA, "compose")
    with open(os.path.join(data, "assembly.dump.txt")) as f:
        want = [l.rstrip("\n") for l in f]
    for name in ("assembly.usda", "assembly.usdc"):
        code, log = run(dump, "--stage", os.path.join(data, name))
        got = [l for l in log.replace("\r\n", "\n").split("\n") if l and not l.startswith("FORMAT")]
        check(code == 0 and got == want, "%s composed as Pixar's USD does: %d lines, %d different" % (
            name, len(got), sum(1 for a, b in zip(got, want) if a != b) + abs(len(got) - len(want))))

    work = os.path.join(tmp, "compose")
    shutil.copytree(data, work)
    scene = os.path.join(work, "assembly.usda")
    code, meshes = list_meshes(exe, scene)
    check(code == 0 and len(meshes) == 8 and meshes.get("/World/propB/geo/render/Box") == 32 and
          meshes.get("/World/treeCopy/leaves") == 18 and meshes.get("/World/rock/rock") == 15,
          "--list of the composed stage: %s" % meshes)

    # a mesh that comes from a reference, replaced by an over in the layer
    out = os.path.join(work, "assembly_retopo.usda")
    code, log = run(exe, scene, "-o", out, "-d", "-m", "World/propB/geo/render/Box=300%")
    text = open(out, encoding="utf-8").read() if os.path.exists(out) else ""
    check(code == 0 and 'over "propB"' in text and "faceVertexCounts" in text and "@./assembly.usda@" in text,
          "-m on a referenced mesh")
    code, log = run(exe, scene, "-o", out, "-d", "-m", "World/inst/*=50%")
    text = open(out, encoding="utf-8").read() if os.path.exists(out) else ""
    check(code == 0 and "prepend inherits = </_IM_" in text and 'class "_IM_' in text,
          "an instanced mesh: remeshed in a class its instances inherit")

    # proxies of referenced meshes: geo/proxy inside each reference
    code, log = run(exe, scene, "-o", scene, "-d", "--proxy", "-m", "propA=200%", "-m", "propC=200%")
    out = os.path.join(work, "assembly_proxy.usda")
    text = open(out, encoding="utf-8").read() if os.path.exists(out) else ""
    check(code == 0 and "rel proxyPrim = </World/propA/geo/proxy/Box>" in text and
          "rel material:binding = </World/propA/mtl/wood>" in text and
          "rel proxyPrim = </World/propC/geo/proxy/Box>" in text and
          "@./assembly_proxy.usda@</World>" in open(scene, encoding="utf-8").read(), "--proxy on referenced meshes")
    code, meshes = list_meshes(exe, scene)
    check(code == 0 and "/World/propA/geo/proxy/Box" in meshes and "/World/propC/geo/proxy/Box" in meshes,
          "the assembly composes its proxies")


def test_usd_binary(exe, tmp):
    print("USD outputs as .usdc / .usdz: read back as their .usda")
    import shutil
    dump = os.path.join(os.path.dirname(exe), "usd_dump.exe" if os.name == "nt" else "usd_dump")
    work = os.path.join(tmp, "binary")
    os.makedirs(work)
    for name in ("usd_asset.usdc", "scene_ab.obj"):
        shutil.copy(os.path.join(DATA, name), work)

    def same(base, exts):
        code, ref = run(dump, base + ".usda")
        ref = [l for l in ref.splitlines() if not l.startswith("FORMAT")]
        for ext in exts:
            code2, got = run(dump, base + ext)
            got = [l for l in got.splitlines() if not l.startswith("FORMAT")]
            check(code == 0 and code2 == 0 and len(ref) > 3 and got == ref,
                  "%s%s reads as its .usda (%d lines)" % (os.path.basename(base), ext, len(got)))

    base = os.path.join(work, "whole")
    for ext in (".usda", ".usdc", ".usdz"):
        run(exe, os.path.join(work, "scene_ab.obj"), "-o", base + ext, "-d", "-f", "100%", "--uv", "unwrap")
    same(base, (".usdc", ".usdz"))
    with open(base + ".usdz", "rb") as f:
        head = f.read(64)
    check(head[:4] == b"PK\x03\x04" and (30 + head[26] + head[28]) % 64 == 0, ".usdz data 64-byte aligned")

    # the proxy layer of a .usdc copy, and of a .usd copy (text): the same layer
    for ext in (".usd", ".usdc"):
        run(exe, os.path.join(work, "usd_asset.usdc"), "-o", os.path.join(work, "copy" + ext), "-d", "--proxy",
            "--others", "40%")
    shutil.copy(os.path.join(work, "copy_proxy.usd"), os.path.join(work, "copy_proxy.usda"))
    same(os.path.join(work, "copy_proxy"), (".usdc",))
    code, log = run(exe, os.path.join(work, "usd_asset.usdc"), "-o", os.path.join(work, "layer.usdz"),
                    "-m", "Body=40%")
    check(code != 0 and "holds every file it uses" in log, "a layer over the scene cannot be a .usdz")


def test_projects(exe, tmp):
    print("projects (.imd): --save-imd, job.imd -o")
    import shutil
    work = os.path.join(tmp, "imd")
    os.makedirs(work)
    scene = os.path.join(work, "scene.abc")
    shutil.copy(os.path.join(DATA, "scene_ab.abc"), scene)
    job = os.path.join(work, "job.imd")

    code, log = run(exe, scene, "-d", "-m", "MeshA=50%", "--others", "80%", "--save-imd", job, "--dry-run")
    check(code == 0 and "Project saved" in log and os.path.exists(job) and "Dry run" in log, "--save-imd --dry-run")
    code, log = run(exe, job, "--dry-run")
    plan = plan_lines(log)
    check(code == 0 and "50%" in plan.get("/Props/MeshA/MeshA", "") and "--others 80%" in plan.get("/Props/MeshB/MeshB", ""),
          "job.imd --dry-run shows the saved plan")

    out = os.path.join(work, "job.abc")
    code, log = run(exe, job, "-o", out)
    code2, meshes = list_meshes(exe, out)
    check(code == 0 and code2 == 0 and abs(meshes.get("/Props/MeshA/MeshA", 0) - 3936) <= 0.1 * 3936,
          "job.imd -o runs the job: %s" % meshes)
    direct = os.path.join(work, "direct.abc")
    run(exe, scene, "-o", direct, "-d", "-m", "MeshA=50%", "--others", "80%")
    with open(out, "rb") as f, open(direct, "rb") as g:
        check(f.read() == g.read(), "the job writes what the command line writes")

    out2 = os.path.join(work, "again.abc")
    code, log = run(exe, job, "-o", out2)
    check(code == 0 and "=== " not in log and "done]" in log, "a finished job computes nothing again")
    with open(out, "rb") as f, open(out2, "rb") as g:
        check(f.read() == g.read(), "the saved results are written as computed")

    for args, expect in ((["-m", "MeshA=10%"], "carries its own targets"),
                         (["-o", os.path.join(work, "x.obj")], "write a .abc file")):
        code, log = run(exe, job, *args)
        check(code != 0 and expect in log, "job.imd %s -> '%s'" % (" ".join(args), expect))
    code, log = run(exe, scene, "--save-imd", os.path.join(work, "x.imd"), "-f", "50%")
    check(code != 0 and "saves the plan of -m / --others" in log, "--save-imd needs the per-mesh mode")
    with open(job, "rb") as f:
        data = bytearray(f.read())
    data[len(data) // 3] ^= 0x5A
    bad = os.path.join(work, "bad.imd")
    with open(bad, "wb") as f:
        f.write(data)
    code, log = run(exe, bad, "-o", os.path.join(work, "bad.abc"))
    check(code != 0 and ("corrupted" in log or "invalid" in log), "a damaged project is refused")


def test_errors(exe, tmp):
    print("argument errors are reported before any computation")
    src = os.path.join(DATA, "cube_quads.obj")
    out = os.path.join(tmp, "err.obj")
    cases = [
        (["-o", out, "-f", "0%"], "Invalid face percentage"),
        (["-o", out, "-f", "-5%"], "Invalid face percentage"),
        (["-o", out, "-f", "abc%"], "Could not parse"),
        (["-o", out, "-f", "75%", "-s", "0.1"], "Only one of"),
        (["-o", os.path.join(tmp, "x.xyz")], "unsupported output format"),
        (["-o", os.path.join(tmp, "x.fbx")], "(.obj/.ply/.abc/.usda/.usdc/.usdz are supported)"),
    ]
    for args, expect in cases:
        code, log = run(exe, src, *args)
        check(code != 0 and expect in log and "Loading" not in log,
              "%s -> expected '%s'" % (" ".join(args), expect))

    code, log = run(exe, os.path.join(DATA, "subd.abc"), "-o", out, "-f", "75%")
    check(code != 0 and "only subdivision surfaces" in log, "subd.abc refused")
    code, log = run(exe, os.path.join(tmp, "missing.abc"), "-o", out)
    check(code != 0 and "Unable to open" in log, "missing file refused")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix="im_cli_") as tmp:
        test_abc_input(exe, tmp)
        test_percentage(exe, tmp)
        test_abc_output(exe, tmp)
        test_mesh_rules(exe, tmp)
        test_obj_rules(exe, tmp)
        test_skip_failed(exe, tmp)
        test_keep_border(exe, tmp)
        test_uv_transfer(exe, tmp)
        test_usd_reader(exe, tmp)
        test_usd_scenes(exe, tmp)
        test_usd_proxies(exe, tmp)
        test_usd_instances(exe, tmp)
        test_thin_objects(exe, tmp)
        test_usd_composition(exe, tmp)
        test_usd_binary(exe, tmp)
        test_projects(exe, tmp)
        test_errors(exe, tmp)
    print("\n%d passed, %d failed" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

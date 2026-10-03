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
    check(code == 0 and log.count("(instanced)") == 2, "--list flags instances")

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
    code, log = run(exe, os.path.join(DATA, "instances.abc"), "--others", "50%", "--dry-run")
    check(code != 0 and "Nothing to remesh" in log and "kept unchanged (instanced)" in log,
          "--others skips instances")

    # Errors, all before any computation
    cases = [
        (["-o", os.path.join(tmp, "e.abc"), "-m", "Nope=50%"], "no polygon mesh matches"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "Mesh=50%"], "no polygon mesh matches"),
        (["-o", os.path.join(tmp, "e.obj"), "-m", "MeshA=50%"], "same format as the input"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "MeshA=50%", "-f", "50%"], "give the face targets there"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "MeshA"], "expected name=target"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "=50%"], "expected name=target"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "MeshA=0%"], "Invalid face percentage"),
        (["-o", os.path.join(tmp, "e.abc"), "-m", "MeshA=abc"], "Could not parse"),
        (["-o", os.path.join(tmp, "e.abc"), "--dry-run"], "--dry-run shows the plan"),
    ]
    for args, expect in cases:
        code, log = run(exe, scene, *args)
        check(code != 0 and expect in log and "Optimizing" not in log,
              "%s -> expected '%s'" % (" ".join(args), expect))
    check(not os.path.exists(os.path.join(tmp, "e.abc")), "no output after errors")
    code, log = run(exe, os.path.join(DATA, "cube_quads.ply"), "--list")
    check(code != 0 and "need one Alembic (.abc), OBJ (.obj) or USD" in log, "--list on a PLY refused")
    code, log = run(exe, os.path.join(DATA, "instances.abc"), "-o", os.path.join(tmp, "e.abc"),
                    "-m", "Pillar*=50%")
    check(code != 0 and "cannot be remeshed" in log, "instanced target refused")
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
    check(code != 0 and "needs an .obj, .abc or .usda output" in log and "Loading" not in log, "--keep-border refuses .ply")
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
        (["-o", os.path.join(tmp, "x.fbx")], "(.obj/.ply/.abc/.usda are supported)"),
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
        test_errors(exe, tmp)
    print("\n%d passed, %d failed" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

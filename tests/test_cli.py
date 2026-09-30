"""
test_cli.py -- End-to-end tests of the Instant Meshes command line (batch
mode) on the reference dataset. Standard library only.

Usage:
    python tests/test_cli.py build/Release/InstantMeshes.exe
Exit code 0 when every check passes.
"""

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
    check(code != 0 and "need one Alembic (.abc) or OBJ (.obj) input" in log, "--list on a PLY refused")
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
        code, log = run(exe, scene, "-o", out, "-d", "-m", "MeshA=50%", "-m", "MeshB=1", "--skip-failed")
        code2, meshes = list_meshes(exe, out)
        check(code == 0 and meshes.get(B) == 576 and abs(meshes.get(A, 0) - 3936) <= 0.1 * 3936,
              "%s --skip-failed: %s" % (ext, meshes))
        check("Skipped 1 of 2 meshes" in log and B + ": Remeshing" in log, "%s: skip summary" % ext)
        # Every selected mesh failing still writes the (unchanged) scene
        out = os.path.join(tmp, "skip_all" + ext)
        code, log = run(exe, scene, "-o", out, "-d", "-m", "MeshB=1", "--skip-failed")
        code2, meshes = list_meshes(exe, out)
        check(code == 0 and meshes == {A: 7872, B: 576}, "%s: all skipped, scene copied: %s" % (ext, meshes))
    code, log = run(exe, os.path.join(DATA, "cube_quads.obj"), "-o", os.path.join(tmp, "e.obj"),
                    "-f", "50%", "--skip-failed")
    check(code != 0 and "--skip-failed applies to the -m / --others" in log, "--skip-failed needs -m / --others")


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
        (["-o", os.path.join(tmp, "x.fbx")], "(.obj/.ply/.abc are supported)"),
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
        test_errors(exe, tmp)
    print("\n%d passed, %d failed" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

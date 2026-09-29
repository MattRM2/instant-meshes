"""
test_cli.py -- End-to-end tests of the Instant Meshes command line (batch
mode) on the reference dataset. Standard library only.

Usage:
    python tests/test_cli.py "build/Release/Instant Meshes.exe"
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
        (["-f", "75%"], "only available in batch mode"),
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
        test_errors(exe, tmp)
    print("\n%d passed, %d failed" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

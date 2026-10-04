"""
check_openusd_corpus.py -- reads every USD file of an OpenUSD source tree
(its test files: about 2,400 layers that exercise every corner of the
format) with Pixar's USD and with usd_dump, and compares.

    python tests/check_openusd_corpus.py <OpenUSD source dir> <work dir> [<blender.exe>] [--ours-only]

Pixar's side runs in Blender (its pxr module), in chunks; --ours-only reuses
the Pixar results of an earlier run. Printed: crashes and hangs (must be
none), the files read by Pixar that usd_dump refuses, and how many layers,
composed stages and meshes (points, faces) are identical. Stage differences
are filed apart when the files use what is not composed (relocates, value
clips, layer offsets); schema-driven differences (type names, uniform,
fallback values) are not counted. Exit code 1 on a crash or a hang.
"""
import collections
import concurrent.futures
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DUMP = os.path.join(ROOT, "build", "Release", "usd_dump.exe" if os.name == "nt" else "usd_dump")
BLENDER = r"D:\PROGRAMME_INSTALLED\Blender_Builds\custom\MattRM2_VFX_BUILD_1.1_Win64\MattRM2VFX.exe"

PXR_SIDE = r'''
import contextlib, io, os, sys
args = sys.argv[sys.argv.index("--") + 1:]
src = open(args[2], encoding="utf-8").read()
exec(compile(src[:src.index("\nargs = sys.argv")], "usd_reference", "exec"))
for line in open(args[0], encoding="utf-8"):
    idx, path = line.rstrip("\n").split("\t", 1)
    for kind, fn in (("layer", dump), ("stage", dump_stage)):
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                fn(path)
            text = buf.getvalue()
        except Exception as e:
            text = "ERROR %s: %s\n" % (type(e).__name__, str(e).splitlines()[0] if str(e) else "")
        with open(os.path.join(args[1], "%s.%s.txt" % (idx, kind)), "w", encoding="utf-8") as f:
            f.write(text)
'''


def run_pxr(files, work, blender):
    ref = os.path.join(work, "ref")
    os.makedirs(ref, exist_ok=True)
    side = os.path.join(work, "pxr_side.py")
    with open(side, "w", encoding="utf-8") as f:
        f.write(PXR_SIDE)

    def chunk(k, items):
        lst = os.path.join(work, "chunk_%03d.txt" % k)
        with open(lst, "w", encoding="utf-8") as f:
            f.writelines("%d\t%s\n" % (i, files[i]) for i in items)
        try:
            subprocess.run([blender, "-b", "--factory-startup", "--python", side, "--", lst, ref,
                            os.path.join(HERE, "usd_reference.py")], capture_output=True, timeout=900)
        except subprocess.TimeoutExpired:
            print("Pixar's side: chunk %d timed out" % k)
    chunks = [list(range(i, min(i + 150, len(files)))) for i in range(0, len(files), 150)]
    with concurrent.futures.ThreadPoolExecutor(4) as ex:
        list(ex.map(lambda kc: chunk(*kc), enumerate(chunks)))


def run_ours(files, work):
    ours = os.path.join(work, "ours")
    os.makedirs(ours, exist_ok=True)

    def one(i):
        for kind, extra in (("layer", []), ("stage", ["--stage"])):
            try:
                p = subprocess.run([DUMP] + extra + [files[i]], capture_output=True, timeout=60)
                out = p.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
                err, code = p.stderr.decode("utf-8", "replace").strip(), p.returncode
            except subprocess.TimeoutExpired:
                out, err, code = "", "TIMEOUT", -999
            with open(os.path.join(ours, "%d.%s.txt" % (i, kind)), "w", encoding="utf-8") as f:
                f.write("CODE %d\n%s\nSTDERR %s" % (code, out, err.splitlines()[-1] if err else ""))
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        list(ex.map(one, range(len(files))))


def norm_stage(lines):
    out = []
    for l in lines:
        if not l.strip() or l.startswith(("Warning:", "FORMAT")):
            continue
        l = re.sub(r"^(  ATTR \S+) (uniform )?\S+ = ", r"\1 = ", l)
        out.append(l.replace(": None", ": -").replace("= None", "= -"))
    return out


def meshes(text):
    out, cur = {}, None
    for l in text.splitlines():
        if l.startswith("PRIM "):
            parts = l.split(" ")
            cur = parts[1] if len(parts) > 3 and parts[2] == "def" and parts[3] == "Mesh" else None
            if cur:
                out[cur] = {}
        elif cur and l.startswith("  ATTR ") and l.split(" ")[3] in ("points", "faceVertexCounts", "faceVertexIndices"):
            out[cur][l.split(" ")[3]] = l.split(" = ", 1)[1].replace(" (timeSamples)", "").replace("None", "-")
    return out


def unsupported(path):
    blob = b""
    for f in glob.glob(os.path.join(os.path.dirname(path), "*.usd*")):
        try:
            blob += open(f, "rb").read()
        except OSError:
            pass
    found = {k for k, pat in (("relocates", b"relocates"), ("clips", b"clips"), ("offsets", b"offset ="),
                              ("offsets", b"scale ="), ("offsets", b"timeCodesPerSecond")) if pat in blob}
    return "+".join(sorted(found))


def compare(files, work, source):
    stats = collections.Counter()
    crashes, refused = [], []
    for i, path in enumerate(files):
        rel = os.path.relpath(path, source)
        docs = {}
        for kind in ("layer", "stage"):
            rp = os.path.join(work, "ref", "%d.%s.txt" % (i, kind))
            raw = open(os.path.join(work, "ours", "%d.%s.txt" % (i, kind)), encoding="utf-8", errors="replace").read()
            code = int(raw.split("\n", 1)[0].split()[1])
            body = raw.split("\n", 1)[1]
            err, body = body[body.rfind("STDERR ") + 7:].strip(), body[:body.rfind("STDERR ")]
            if code not in (0, 1):
                crashes.append("%s (%s): %s" % (rel, kind, "hang" if code == -999 else "exit code %d" % code))
                continue
            ref = open(rp, encoding="utf-8").read() if os.path.exists(rp) else None
            if ref is None:
                stats[kind + ": no Pixar result"] += 1
                continue
            pxr_ok, ours_ok = not ref.startswith("ERROR"), code == 0
            if not pxr_ok:
                stats[kind + ": Pixar refuses"] += 1
                continue
            if not ours_ok:
                stats[kind + ": refused here"] += 1
                if kind == "layer":
                    refused.append("%s: %s" % (rel, err[:160]))
                continue
            a, b = ref.splitlines(), body.splitlines()
            if kind == "stage":
                a, b = norm_stage(a), norm_stage(b)
                if len(a) == len(b):
                    # fallbacks of the schemas: Pixar shows a value nobody authored
                    b = [x if (y.endswith("= -") or "= - (timeSamples)" in y) and x.split(" = ")[0] == y.split(" = ")[0]
                         else y for x, y in zip(a, b)]
                docs = (ref, body)
            else:
                a, b = [l for l in a if l.strip()], [l for l in b if l.strip()]
            if a == b:
                stats[kind + ": identical"] += 1
            else:
                feat = unsupported(path) if kind == "stage" else ""
                stats[kind + ": different" + (" (uses " + feat + ", not composed)" if feat else "")] += 1
        if docs:
            ma, mb = meshes(docs[0]), meshes(docs[1])
            stats["meshes (Pixar)"] += len(ma)
            stats["meshes identical"] += sum(1 for k in ma if mb.get(k) == ma[k])
    print("%d files" % len(files))
    for k in sorted(stats):
        print("  %-55s %d" % (k, stats[k]))
    print("Crashes and hangs: %d" % len(crashes))
    for c in crashes:
        print("  " + c)
    print("Layers Pixar reads and usd_dump refuses: %d" % len(refused))
    for r in refused:
        print("  " + r)
    return not crashes


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        print(__doc__)
        sys.exit(2)
    source, work = os.path.abspath(args[0]), os.path.abspath(args[1])
    blender = args[2] if len(args) > 2 else BLENDER
    os.makedirs(work, exist_ok=True)
    files = sorted(p for ext in ("usd", "usda", "usdc", "usdz")
                   for p in glob.glob(os.path.join(source, "**", "*." + ext), recursive=True))
    if "--ours-only" not in sys.argv:
        run_pxr(files, work, blender)
    run_ours(files, work)
    sys.exit(0 if compare(files, work, source) else 1)


if __name__ == "__main__":
    main()

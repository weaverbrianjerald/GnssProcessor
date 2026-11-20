"""Fixtures for the self-tests of the grading scripts (card GRA, plan step G1).

Two kinds:
- synthetic: a JSON file in scripts/grading/fixtures/ that holds a few run folders as text, {"files": {"<path>": [lines]}};
  materialize() writes them under a temporary folder laid out as a runs root.  The fixtures are JSON because the
  repository ignores every logs/ folder and every *.log file (.gitignore), which run folders are made of.
- frozen: the first two hours of named network and user runs, copied once from D:/GnssProcessorSupport/runs_nom into
  D:/GnssProcessorSupport/gates/grader_fixtures_2026-11-18 (GRADING_FROZEN_ROOT overrides it) with its MANIFEST.sha256;
  frozen.json in this folder lists, per script, the commands run on it and the sha256 of their output.  A case with
  "suffix": "_h6" runs on a second window of the same runs (hours 6 to 8 of the day, ..._2026-11-18_h6), where the
  users hold integers (they hold few in the first two hours).  A case with "root": "<folder>" runs on that folder
  beside the default one instead (the graders added on 2026-11-19 use grader_fixtures_2026-11-19, windows of several
  runs, see its MANIFEST.sha256); a case with "mask_root": true has the fixture folder's path replaced by "{root}" in
  its output before hashing (a printout that names its runs by full path, as the two-run audit's does).
  check_frozen() reruns them; it is skipped when the frozen folder is absent (another machine, D: disconnected).

    py -3.14 scripts/grading/fixture.py --selftest
"""
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FROZEN_DEFAULT = "D:/GnssProcessorSupport/gates/grader_fixtures_2026-11-18"


def materialize(name, root):
    """Write the files of fixtures/<name> under root."""
    with open(os.path.join(HERE, "fixtures", name), encoding="utf-8") as fh:
        spec = json.load(fh)
    for rel, lines in spec["files"].items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8", newline="\n") as out:
            out.write("".join(line + "\n" for line in lines))


def frozen_root():
    """The frozen fixtures' folder, or None when it is not present."""
    root = os.environ.get("GRADING_FROZEN_ROOT") or FROZEN_DEFAULT
    return root if os.path.isfile(os.path.join(root, "MANIFEST.sha256")) else None


def run(script, args, root, exit_code=0):
    """The stdout of scripts/grading/<script> run with RAPID_RUNS_ROOT=root, from root (so relative lists resolve);
    an exit code other than exit_code raises (the two-run audit exits 1 when it finds a failing check, as the original
    does, so its cases declare "exit": 1)."""
    env = dict(os.environ, RAPID_RUNS_ROOT=root, PYTHONDONTWRITEBYTECODE="1")
    out = subprocess.run([sys.executable, os.path.join(HERE, script)] + args, cwd=root, env=env,
                         capture_output=True, text=True)
    if out.returncode != exit_code:
        raise subprocess.CalledProcessError(out.returncode, [script] + args, out.stdout, out.stderr)
    return out.stdout


def sha256_text(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def mask_root(text, root):
    """The text with every spelling of the fixture folder's path (as given, absolute, either slash) replaced by "{root}",
    so that a printout naming its runs by full path hashes the same wherever the fixtures sit."""
    forms = {root, os.path.abspath(root)}
    forms |= {f.replace("/", "\\") for f in forms} | {f.replace("\\", "/") for f in forms}
    for form in sorted(forms, key=len, reverse=True):
        text = text.replace(form, "{root}")
    return text


def check_frozen(script):
    """Rerun the frozen cases of one script; True when all match or when the frozen fixtures are absent."""
    root = frozen_root()
    if root is None:
        print("  frozen fixtures absent: skipped")
        return True
    path = os.path.join(HERE, "frozen.json")
    cases = []
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as fh:
            cases = json.load(fh).get(script, [])
    ok = True
    for c in cases:
        args = [x.replace("{fixtures}", os.path.join(HERE, "fixtures")) for x in c["args"]]
        if "root" in c:                              # another fixture folder beside the default one
            croot = os.path.join(os.path.dirname(os.path.abspath(root)), c["root"])
        else:                                        # a second window of the same runs, e.g. "_h6" (hours 6 to 8)
            croot = root + c.get("suffix", "")
        if not os.path.isfile(os.path.join(croot, "MANIFEST.sha256")):
            print(f"  frozen window {croot} absent: case skipped")
            continue
        text = run(script, args, croot, c.get("exit", 0))
        if c.get("mask_root"):
            text = mask_root(text, croot)
        got = sha256_text(text)
        if got != c["sha256"]:
            print(f"  FAIL frozen: {script} {' '.join(c['args'])}: {got[:16]} against {c['sha256'][:16]}")
            ok = False
    print(f"  frozen fixtures: {len(cases)} case(s) of {script} {'match' if ok else 'differ'}")
    return ok


def selftest():
    import tempfile
    ok = True
    with tempfile.TemporaryDirectory() as root:
        materialize("g1_reader.json", root)
        ok &= os.path.isfile(os.path.join(root, "_rap_fx", "logs", "network_ring_fix.csv"))
        ok &= sha256_text("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        ok &= mask_root(r"a D:/x/y/_rap_t b D:\x\y\_rap_t", "D:/x/y") == r"a {root}/_rap_t b {root}\_rap_t"
    print("selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    print(__doc__)

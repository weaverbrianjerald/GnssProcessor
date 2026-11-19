"""One orchestrator per bar group (card GRA, plan step G9): runs the graders of scripts/grading on named runs, writes each
child's output into one folder, fails when a child fails, and writes a manifest of the grade.

    py -3.14 scripts/grading/grade.py pair <run A> <run B> --out DIR
    py -3.14 scripts/grading/grade.py network <run> --xfile <list> --out DIR [--control <run>]
    py -3.14 scripts/grading/grade.py sim <run> <truth bed> --out DIR [--control <run>]
    py -3.14 scripts/grading/grade.py users --fix L --flt L --doy D --pilot a,b --combined c,d --net <run> --out DIR
    py -3.14 scripts/grading/grade.py identity <run A> <run B> --out DIR
    py -3.14 scripts/grading/grade.py signature <run> ... --doy-tow0 TOW --combined <list> --out DIR [--pairs A:B ...]
    py -3.14 scripts/grading/grade.py --selftest

The groups and their children (each a command of a script of this folder, its output DIR/<name>.txt):
  pair       the two-run audit, legacy (with its JSON) and by link (bar F4)
  network    the hourly table, exact (and the comparison with --control); links held through a flag by the chord's
             signals, Galileo and GPS (bar F5); the long one-signed holds, the window replay and the resumptions
  sim        the run-end truth lines; the frame-free cover in sorted order; the frame classes of the wrong holds; the
             holds against truth; with --control, the guard grader's lines (control and run)
  users      the both-lanes check with the runs by receiver type, per group and pair (bar F6); the hourly medians;
             the reference changes with their sums
  identity   the comparison of two runs by whole files first (bar B12)
  signature  the code signature by link (exact) and the arcs' spread (bar B13)
A child that exits with a code other than its accepted ones fails the grade (exit 1 at the end; the other children
still run).  The two-run audit exits 1 when it finds a failing check, as its original does: 0 and 1 are both results.
DIR/MANIFEST.json records the grading scripts' commit (and whether the folder has uncommitted changes), the Python
version, each child's arguments, exit code, duration and the sha256 of its output, and the size and modification
time of every input file of each run named (products.txt, arm.json, logs/*).
"""
import argparse
import datetime
import hashlib
import json
import os
import platform
import subprocess
import sys
import time

import runs

HERE = os.path.dirname(os.path.abspath(__file__))
PAIRS_USERS = (("pilot", "E L1C/L5Q"), ("pilot", "G L1C/L2W"), ("combined", "E L1X/L5X"), ("combined", "G L1C/L2W"))


def git_state():
    try:
        head = subprocess.run(["git", "-C", HERE, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", HERE, "status", "--porcelain", "--", "."], capture_output=True,
                               text=True).stdout.strip()
        return {"commit": head, "uncommitted_changes": bool(dirty)}
    except OSError:
        return {"commit": None, "uncommitted_changes": None}


def run_inputs(run):
    """Size and modification time of the run's products.txt, arm.json and logs/* (an absent run noted)."""
    d = runs.run_dir(run)
    if not os.path.isdir(d):
        return {"folder": d, "absent": True}
    files = {}
    for rel in ["products.txt", "arm.json"] + [os.path.join("logs", n) for n in sorted(os.listdir(os.path.join(d, "logs")))
                                               if os.path.isdir(os.path.join(d, "logs"))]:
        p = os.path.join(d, rel)
        if os.path.isfile(p):
            st = os.stat(p)
            files[rel.replace("\\", "/")] = {"bytes": st.st_size, "modified": datetime.datetime.fromtimestamp(
                st.st_mtime).isoformat(timespec="seconds")}
    return {"folder": d, "files": files}


class Grade:
    def __init__(self, group, out, inputs):
        self.group, self.out = group, out
        os.makedirs(out, exist_ok=True)
        self.children, self.failed = [], []
        self.manifest = {"group": group, "started": datetime.datetime.now().isoformat(timespec="seconds"),
                         "grading": git_state(), "python": platform.python_version(),
                         "runs_root": runs.runs_root(), "inputs": {r: run_inputs(r) for r in inputs}}

    def child(self, name, script, args, accept=(0,)):
        path = os.path.join(self.out, name + ".txt")
        t = time.time()
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        with open(path, "wb") as fh:
            p = subprocess.run([sys.executable, os.path.join(HERE, script)] + args, stdout=fh, stderr=subprocess.STDOUT,
                               env=env)
        with open(path, "rb") as fh:
            digest = hashlib.sha256(fh.read()).hexdigest()
        ok = p.returncode in accept
        self.children.append({"name": name, "script": script, "args": args, "exit": p.returncode,
                              "accepted": list(accept), "ok": ok, "seconds": round(time.time() - t, 1),
                              "output": name + ".txt", "sha256": digest})
        if not ok:
            self.failed.append(name)
        print(f"   {name}: exit {p.returncode}{'' if ok else ' FAILED'} ({round(time.time() - t)} s)")

    def close(self):
        self.manifest["children"] = self.children
        self.manifest["ended"] = datetime.datetime.now().isoformat(timespec="seconds")
        self.manifest["failed"] = self.failed
        with open(os.path.join(self.out, "MANIFEST.json"), "w", encoding="utf-8", newline="\n") as fh:
            json.dump(self.manifest, fh, indent=1)
            fh.write("\n")
        print(f"== {self.group}: {len(self.children)} children, {len(self.failed)} failed"
              + (f" ({', '.join(self.failed)})" if self.failed else "") + f"; manifest {self.out}/MANIFEST.json")
        return 1 if self.failed else 0


def g_pair(a):
    g = Grade("pair", a.out, [a.a, a.b])
    g.child("two_run_legacy", "two_run.py", [a.a, a.b, "--json", os.path.join(a.out, "two_run_legacy.json")], (0, 1))
    g.child("two_run_bylink", "two_run.py", [a.a, a.b, "--view", "bylink"], (0, 1))
    return g.close()


def g_network(a):
    g = Grade("network", a.out, [a.run] + ([a.control] if a.control else []))
    g.child("hourly_exact", "network_hourly.py", ["table", a.run, "--xfile", a.xfile, "--view", "exact"])
    if a.control:
        g.child("hourly_compare", "network_hourly.py", ["compare", a.run, a.control, "--xfile", a.xfile, "--view", "exact"])
    for sy in ("E", "G"):
        g.child(f"slip_chord_{sy}", "slip_hold.py", ["flagged", a.run, a.run, "--sys", sy, "--view", "chord"])
    g.child("onesigned", "held_integrity.py", ["onesigned", a.run, "--view", "exact"])
    g.child("window", "held_integrity.py", ["window", a.run, "--view", "exact"])
    g.child("resume", "held_integrity.py", ["resume", a.run])
    return g.close()


def g_sim(a):
    g = Grade("sim", a.out, [a.run] + ([a.control] if a.control else []))
    g.child("runend", "truth.py", ["runend", a.run])
    g.child("cover_sorted", "truth.py", ["cover", a.run, a.bed, "--order", "sorted"])
    g.child("classes", "truth.py", ["classes", a.run, a.bed, "--list", str(a.list)])
    g.child("held_truth", "held_integrity.py", ["truth", a.run, "--rule", "majority"])
    if a.control:
        g.child("guard", "truth.py", ["guard", f"{a.control}:{a.run}:{a.bed}"])
    return g.close()


def g_users(a):
    g = Grade("users", a.out, [a.net])
    sites = {"pilot": a.pilot, "combined": a.combined}
    for group, pair in PAIRS_USERS:
        g.child(f"both_{group}_{pair.replace(' ', '').replace('/', '')}", "users.py",
                ["both", "--fix", a.fix, "--flt", a.flt, "--doy", a.doy, "--sites", sites[group], "--pair", pair,
                 "--runs", "--type-from", a.net])
    for group in ("pilot", "combined"):
        g.child(f"hourly_{group}", "users.py", ["hourly", f"obs:{a.fix}", f"obs:{a.flt}", "--doy", a.doy, "--set",
                                                sites[group], "--hours", "24", "--view", "exact"])
    names = [f"_rap_nw_obs{a.fix}_{a.doy}_{s}" for s in (a.pilot + "," + a.combined).split(",")]
    g.child("refch", "users.py", ["refch"] + names + ["--sums"])
    return g.close()


def g_identity(a):
    g = Grade("identity", a.out, [a.a, a.b])
    g.child("compare_identity", "compare_runs.py", ["compare", a.a, a.b])
    return g.close()


def g_signature(a):
    g = Grade("signature", a.out, a.runs)
    g.child("links_exact", "code_signature.py", ["links"] + a.runs + ["--doy-tow0", str(a.doy_tow0), "--combined",
                                                                       a.combined, "--view", "exact"]
            + (["--pairs"] + a.pairs if a.pairs else []))
    g.child("arcs_exact", "code_signature.py", ["arcs"] + a.runs + ["--view", "exact"])
    return g.close()


def selftest():
    import tempfile
    import fixture
    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            print("  FAIL:", what)
            ok = False

    with tempfile.TemporaryDirectory() as root:
        fixture.materialize("g3_two_run.json", root)
        fixture.materialize("g6_compare.json", root)
        os.environ["RAPID_RUNS_ROOT"] = root
        out = os.path.join(root, "grade_pair")
        code = g_pair(build_parser().parse_args(["pair", "a", "b", "--out", out]))
        man = json.load(open(os.path.join(out, "MANIFEST.json"), encoding="utf-8"))
        check(code == 0 and [c["exit"] for c in man["children"]] == [1, 1] and not man["failed"],
              "the two-run audit's exit 1 is a result, not a failure")
        check(os.path.isfile(os.path.join(out, "two_run_legacy.json")), "the audit's JSON beside its printout")
        check(man["inputs"]["a"]["files"]["logs/network_ring_fix.csv"]["bytes"] > 0, "the inputs' sizes recorded")
        out2 = os.path.join(root, "grade_identity")
        code2 = g_identity(build_parser().parse_args(["identity", "sep", "absent_run", "--out", out2]))
        man2 = json.load(open(os.path.join(out2, "MANIFEST.json"), encoding="utf-8"))
        check(code2 == 1 and man2["failed"] == ["compare_identity"] and man2["inputs"]["absent_run"]["absent"],
              "a child that fails (an absent run) fails the grade and the manifest names it")
        code3 = g_identity(build_parser().parse_args(["identity", "sep", "sep2", "--out", os.path.join(root, "g3")]))
        check(code3 == 0, "a grade whose children pass")
    print("selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pair")
    p.add_argument("a")
    p.add_argument("b")
    n = sub.add_parser("network")
    n.add_argument("run")
    n.add_argument("--xfile", required=True)
    n.add_argument("--control", default="")
    s = sub.add_parser("sim")
    s.add_argument("run")
    s.add_argument("bed")
    s.add_argument("--control", default="")
    s.add_argument("--list", type=int, default=40)
    u = sub.add_parser("users")
    for k in ("--fix", "--flt", "--doy", "--pilot", "--combined", "--net"):
        u.add_argument(k, required=True)
    i = sub.add_parser("identity")
    i.add_argument("a")
    i.add_argument("b")
    g = sub.add_parser("signature")
    g.add_argument("runs", nargs="+")
    g.add_argument("--doy-tow0", type=float, required=True)
    g.add_argument("--combined", required=True)
    g.add_argument("--pairs", nargs="*", default=[])
    for x in (p, n, s, u, i, g):
        x.add_argument("--out", required=True)
    return ap


def main():
    if "--selftest" in sys.argv:
        return selftest()
    a = build_parser().parse_args()
    return {"pair": g_pair, "network": g_network, "sim": g_sim, "users": g_users, "identity": g_identity,
            "signature": g_signature}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())

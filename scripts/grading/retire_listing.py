"""The retire listing (card GRA, plan step G10): checks RETIRED.md against this folder and the old folders.

    py -3.14 scripts/grading/retire_listing.py [--s20 DIR] [--spc DIR] [--mgr DIR]
    py -3.14 scripts/grading/retire_listing.py --selftest

Checks, each printed with its count:
  1. every row of RETIRED.md names a file that exists in its folder (an absent folder is reported and skipped);
  2. every owner named ("<script>.py <view>") is a script of this folder;
  3. no script of this folder calls a retired one: no import of a retired module, no command line naming a retired
     script (".py" or ".sh" after a space or a quote), no path into the old folders or a temporary scratchpad
     (scripts_s20, scratchpad_copy, manager_2026-11-19, AppData/Local/Temp); a name in a docstring or comment saying
     which script a view reproduces is not a call;
  4. reported, not failed: the kept scripts of the old grading folder that still import or call a retired one (they
     would break if the retired ones were deleted, which nothing here does).
Exit 1 when check 1, 2 or 3 fails.
"""
import argparse
import ast
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FOLDERS = {"S20": "D:/GnssProcessorSupport/runs_nom/_irdu_grades/scripts_s20",
           "SPC": "D:/GnssProcessorSupport/gates/irdu_handover/scratchpad_copy",
           "MGR": "D:/GnssProcessorSupport/gates/manager_2026-11-19"}
OLD_PATHS = re.compile(r"scripts_s20|scratchpad_copy|manager_2026-11-19|AppData[/\\\\]+Local[/\\\\]+Temp", re.I)


def table(path):
    """[(old script, folder, status, owner)] of RETIRED.md's table."""
    rows = []
    for ln in open(path, encoding="utf-8"):
        p = [x.strip() for x in ln.strip().strip("|").split("|")]
        if len(p) == 4 and p[0] not in ("old script", "---") and not p[0].startswith("-"):
            rows.append(tuple(p))
    return rows


def calls_in(path, retired):
    """The calls of a retired script in one source file: imports (Python), command lines and old paths, outside
    docstrings and comments."""
    found = []
    text = open(path, encoding="utf-8", errors="replace").read()
    mods = {r[:-3] for r in retired if r.endswith(".py")}
    if path.endswith(".py"):
        try:
            tree = ast.parse(text)
        except SyntaxError:
            return [("unparsable", path)]
        docs = set()
        for node in ast.walk(tree):
            if isinstance(node, (ast.Module, ast.FunctionDef, ast.ClassDef, ast.AsyncFunctionDef)) and node.body                     and isinstance(node.body[0], ast.Expr) and isinstance(node.body[0].value, ast.Constant):
                docs.add(id(node.body[0].value))
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                found += [("import", n.name) for n in node.names if n.name in mods]
            elif isinstance(node, ast.ImportFrom) and node.module in mods:
                found.append(("import", node.module))
            elif isinstance(node, ast.Constant) and isinstance(node.value, str) and id(node) not in docs:
                v = node.value.strip()
                if OLD_PATHS.search(v):
                    found.append(("path", v[:80]))
                for r in retired:
                    if v == r or v.endswith(("/" + r, "\\" + r)) or re.search(
                            r"\b(py|python|bash|sh)\b[^\n]*\s" + re.escape(r) + r"(\s|$)", v):
                        found.append(("command", v[:80]))
        return found
    for ln in text.splitlines():
        code = ln.split("#", 1)[0]
        if OLD_PATHS.search(code):
            found.append(("path", code.strip()[:80]))
        for r in retired:
            if re.search(r"(^|[\s'\"/])" + re.escape(r) + r"($|[\s'\"])", code):
                found.append(("command", code.strip()[:80]))
    return found


def check(folders, out=print):
    rows = table(os.path.join(HERE, "RETIRED.md"))
    retired = [r[0] for r in rows]
    bad = 0
    absent, missing = [], []
    for old, folder, status, owner in rows:
        base = folders.get(folder)
        if base is None or not os.path.isdir(base):
            absent.append(folder)
            continue
        if not os.path.isfile(os.path.join(base, old)):
            missing.append(f"{folder}/{old}")
    out(f"1. rows {len(rows)}: files absent {len(missing)}" + (f" ({', '.join(missing)})" if missing else "")
        + (f"; folders not present, skipped: {', '.join(sorted(set(absent)))}" if absent else ""))
    bad += bool(missing)
    owners = sorted({o.split()[0] for _, _, s, o in rows if s in ("owned", "partly owned") and o})
    no_owner = [o for o in owners if not os.path.isfile(os.path.join(HERE, o))]
    out(f"2. owners named {len(owners)} ({', '.join(owners)}): absent {len(no_owner)}"
        + (f" ({', '.join(no_owner)})" if no_owner else ""))
    bad += bool(no_owner)
    mine = sorted(glob.glob(os.path.join(HERE, "*.py")) + glob.glob(os.path.join(HERE, "*.sh")))
    own = {os.path.basename(p) for p in mine}       # a retired name that is a script here (compare_runs.py) is its owner
    calls = [(os.path.basename(p), c) for p in mine if os.path.basename(p) != "retire_listing.py"
             for c in calls_in(p, [r for r in retired if r not in own])]
    out(f"3. scripts of this folder {len(mine)}: calls of a retired script or of an old folder {len(calls)}"
        + "".join(f"\n      {f}: {k} {v}" for f, (k, v) in calls))
    bad += bool(calls)
    s20 = folders.get("S20")
    if s20 and os.path.isdir(s20):
        kept = [p for p in sorted(glob.glob(os.path.join(s20, "*.py")) + glob.glob(os.path.join(s20, "*.sh")))
                if os.path.basename(p) not in retired]
        users = {}
        for p in kept:
            c = [v for k, v in calls_in(p, retired) if k in ("import", "command")]
            if c:
                users[os.path.basename(p)] = len(c)
        out(f"4. kept scripts of the old grading folder {len(kept)}: {len(users)} still import or call a retired one "
            f"(reported; nothing is deleted): " + ", ".join(f"{k} {v}" for k, v in sorted(users.items())))
    return 1 if bad else 0


def selftest():
    import tempfile
    ok = True
    with tempfile.TemporaryDirectory() as root:
        p = os.path.join(root, "a.py")
        with open(p, "w") as fh:
            fh.write('"""Legacy: old_tool.py byte for byte."""\nimport old_tool\nimport subprocess\n'
                     'subprocess.run(["py", "old_tool.py"])\nX = "D:/x/scripts_s20/y.py"\n')
        c = calls_in(p, ["old_tool.py"])
        kinds = sorted(k for k, _ in c)
        ok &= kinds == ["command", "import", "path"]
        q = os.path.join(root, "b.py")
        with open(q, "w") as fh:
            fh.write('"""The legacy view reproduces old_tool.py byte for byte."""\nimport os\n')
        ok &= calls_in(q, ["old_tool.py"]) == []
    print("selftest:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for k in FOLDERS:
        ap.add_argument("--" + k.lower(), default=FOLDERS[k])
    a = ap.parse_args()
    return check({"S20": a.s20, "SPC": a.spc, "MGR": a.mgr})


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Relocation verifier for card UOM step U2: the KalmanFilter organized by concern.

    python scripts/uom_u2_verify.py [--verbose] [--tree DIR]

The frozen source is src/core/filter/KalmanFilter.cpp at 5154324 (card UOM pre-registered),
read with `git show` (no copy is stored).  Comments and blank lines are ignored on both sides
(the lexer of scripts/rt_seam_verify.py), so a rewritten comment never fails the check; every
other character of a carried line is compared, indentation included.

A unit is one item of a new file (a function, a struct, a constant) written as an ordered
list of pieces:

  ("frozen", a, b, shift)   the frozen code lines a..b, `shift` leading spaces removed, after
                            the declared substitutions of SUBS on those lines;
  ("new", text)             one declared new code line (compared without its indentation);
  ("alias", [names])        one line per name binding the name the code had as a local of
                            the single function to the stage's work value, `... name = w.name;`
                            (the binding check: the alias and the member are the same name);
  ("publish", [names])      one line per name, `w.name = name;`, handing a stage's local to
                            the later stages;
  ("comment", text)         text the generator writes and the check ignores.

Exit 0 only when every check passes:

  1. Units.  Each unit is found in its declared file by its first code line, which occurs
     once in that file and in no other file of the set; its code lines equal the pieces one
     for one; and they close the item (balanced braces, the last line ends the item).
  2. Coverage.  Every code line of the frozen file is carried by exactly one unit, or lies in
     SCAFFOLD (the includes and namespace lines each new file writes for itself), or is a
     declared drop of DROPS, with its reason.  None is dropped silently, none carried twice.
  3. Nothing added.  Outside its units a new .cpp file holds only #include lines, namespace
     lines and their closing braces and `using kalman::<name>;` declarations; a header also
     holds declarations that end in `;` without a body, and the types of DECLARED_TYPES.
  4. Literals.  The multiset of string literals (the log formats among them) over the code of
     the new files equals the frozen file's, less the dropped lines' and the include lines',
     plus the literals of the declared new lines.

A carried line is compared as text: the check proves where each line now lives and that it
is unchanged.  The includes of each new file are new, and the compiler proves them
sufficient; the gate (scripts/uom_gate.py) proves the output unchanged.
"""
import argparse
import collections
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from rt_seam_verify import strip_comments, _CPP  # noqa: E402

FROZEN_REV = "5154324"
FROZEN_PATH = "src/core/filter/KalmanFilter.cpp"
F = "src/core/filter/"
H = "include/filter/"
FILES = [F + "KalmanFilter.cpp", F + "KalmanKernels.cpp", F + "KalmanResidualQc.cpp",
         F + "KalmanPostFitQc.cpp", F + "KalmanOutlierResponse.cpp",
         H + "KalmanKernels.hpp", H + "KalmanRowIdentity.hpp"]

# Frozen lines each new file replaces with its own: the includes, `namespace GnssProcessor {`,
# the anonymous namespace's braces, the namespace close.
SCAFFOLD = [(11, 25), (27, 27), (33, 33), (206, 206), (3240, 3240)]

# Frozen lines deleted on purpose, with the reason.
DROPS = [
    ((492, 591), "U2a: KalmanFilter::predictImu has no caller (the Grep tool and a grep over "
                 "src, include, tests and python find only its definition and declaration)"),
    ((1746, 1746), "U2a: the local epoch_tag of validatePostFitResiduals is never read"),
]

# Declared substitutions on single frozen lines: (line, old text, new text).
SUBS = [
    (199, "    std::pair<std::string, std::string> splitBssdSatId(",
          "    inline std::pair<std::string, std::string> splitBssdSatId("),
]

# Declarations a header may hold outside the units (new types and tables, no function
# bodies): header -> regexes of the declaration's first code line.
DECLARED_TYPES = {}


def fz(a, b=None, shift=0):
    return ("frozen", a, a if b is None else b, shift)


UNITS = [
    ("bandWavelengthForSat", H + "KalmanRowIdentity.hpp", [fz(42, 80)]),
    ("splitBssdSatId", H + "KalmanRowIdentity.hpp", [fz(199, 204)]),
    ("SparseHRow", H + "KalmanKernels.hpp", [fz(95, 98)]),
    ("kHSparseThreshold", F + "KalmanKernels.cpp", [fz(93)]),
    ("buildSparseH", F + "KalmanKernels.cpp", [fz(101, 116)]),
    ("sparseHP", F + "KalmanKernels.cpp", [fz(119, 132)]),
    ("sparseHPHt", F + "KalmanKernels.cpp", [fz(135, 152)]),
    ("sparseKH", F + "KalmanKernels.cpp", [fz(156, 169)]),
    ("sparseHv", F + "KalmanKernels.cpp", [fz(172, 186)]),
    ("spliceAndPropagate", F + "KalmanKernels.cpp", [fz(597, 634)]),
    ("josephPseudoUpdate", F + "KalmanKernels.cpp", [fz(3211, 3228)]),
    ("modeLabel", F + "KalmanFilter.cpp", [fz(188, 190)]),
    ("KalmanFilter()", F + "KalmanFilter.cpp", [fz(299, 385)]),
    ("~KalmanFilter()", F + "KalmanFilter.cpp", [fz(387)]),
    ("predict", F + "KalmanFilter.cpp", [fz(393, 486)]),
    ("predictAided", F + "KalmanFilter.cpp", [fz(636, 664)]),
    ("update", F + "KalmanFilter.cpp", [fz(670, 1060)]),
    ("updateFromPrior", F + "KalmanFilter.cpp", [fz(1066, 1172)]),
    ("takeRequestedPivotHandovers", F + "KalmanFilter.cpp", [fz(3113, 3117)]),
    ("takeRequestedPivotHandoverExcludeSats", F + "KalmanFilter.cpp", [fz(3119, 3124)]),
    ("takeRequestedPivotHandoverIonoReinflateSats", F + "KalmanFilter.cpp", [fz(3126, 3131)]),
    ("beginEpoch", F + "KalmanFilter.cpp", [fz(3133, 3154)]),
    ("endEpoch", F + "KalmanFilter.cpp", [fz(3156, 3171)]),
    ("logKalmanFilterTimings", F + "KalmanFilter.cpp", [fz(3173, 3205)]),
    ("applyPseudoObservation", F + "KalmanFilter.cpp", [fz(3230, 3238)]),
    ("computeResidualQc_", F + "KalmanResidualQc.cpp", [fz(1609, 1641)]),
    ("residualQcKernel", F + "KalmanResidualQc.cpp", [fz(1643, 1736)]),
    ("validatePostFitResiduals", F + "KalmanPostFitQc.cpp", [fz(1742, 3111)]),
    ("isUnsetVariance", F + "KalmanOutlierResponse.cpp", [fz(194, 196)]),
    ("seedCovarianceDiagonals", F + "KalmanOutlierResponse.cpp", [fz(212, 293)]),
    ("handlePhaseOutlier", F + "KalmanOutlierResponse.cpp", [fz(1178, 1317)]),
    ("handleBaselinePhaseOutlier", F + "KalmanOutlierResponse.cpp", [fz(1322, 1411)]),
    ("handleCodeOutlier", F + "KalmanOutlierResponse.cpp", [fz(1413, 1464)]),
    ("resetFilterCovariance", F + "KalmanOutlierResponse.cpp", [fz(1466, 1603)]),
]

ALLOWED_OUTSIDE = re.compile(r"^(#include\s.*|#pragma once|namespace \w+ \{|namespace \{|"
                             r"\}\s*|using kalman::\w+;)$")
ALIAS = re.compile(r"^(?:const\s+)?[\w:]+(?:<[^<>]*>)?\s*&?\s+(\w+)\s*=\s*w\.(\w+);$")


def blank_literals(text):
    out = []
    for m in _CPP.finditer(text):
        s = m.group()
        out.append("".join(c if c == "\n" else " " for c in s)
                   if m.lastgroup in ("str", "chr", "raw") else s)
    return "".join(out)


def literals(code_line):
    return [m.group() for m in _CPP.finditer(code_line) if m.lastgroup in ("str", "raw")]


class Source:
    """A file as comment-stripped code lines (1-based), and the same with literals blanked."""

    def __init__(self, text):
        text = text.lstrip("\ufeff").replace("\r\n", "\n")
        self.text = [""] + text.split("\n")
        self.code = [""] + strip_comments(text).split("\n")
        self.blank = [""] + blank_literals(strip_comments(text)).split("\n")

    def extent(self, i):
        """Last line of the declaration that starts at line i: the line of its first `;`
        when no `{` comes first, else the line that closes the first `{`."""
        depth, opened = 0, False
        for j in range(i, len(self.blank)):
            for ch in self.blank[j]:
                if ch == ";" and not opened:
                    return j
                if ch == "{":
                    depth += 1
                    opened = True
                elif ch == "}":
                    depth -= 1
                    if opened and depth == 0:
                        return j
        raise ValueError(f"unbalanced item at line {i}")


def git_show(rev, path):
    out = subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"],
                         capture_output=True, check=True)
    return out.stdout.decode("utf-8")


def in_ranges(i, ranges):
    return any(a <= i <= b for a, b in ranges)


def dropped(i):
    return any(a <= i <= b for (a, b), _ in DROPS)


def frozen_source():
    return Source(git_show(FROZEN_REV, FROZEN_PATH))


def frozen_with_subs(frozen, problems):
    code = list(frozen.code)
    for ln, old, new in SUBS:
        if old not in code[ln]:
            problems.append(f"substitution {old!r} not on frozen line {ln}")
            continue
        code[ln] = code[ln].replace(old, new)
    return code


def expected(code, pieces, claimed, problems, name):
    """[(kind, text)] of the code lines a unit must hold, in order."""
    exp = []
    for p in pieces:
        kind = p[0]
        if kind == "frozen":
            _, a, b, shift = p
            for i in range(a, b + 1):
                if not code[i].strip():
                    continue
                claimed[i] += 1
                if dropped(i):
                    continue
                if code[i][:shift].strip():
                    problems.append(f"{name}: frozen line {i} has fewer than {shift} leading spaces")
                exp.append(("frozen", code[i][shift:].rstrip()))
        elif kind == "new":
            exp.append(("new", strip_comments(p[1]).strip()))
        elif kind in ("alias", "publish"):
            exp.extend((kind, n) for n in p[1])
        elif kind != "comment":
            problems.append(f"{name}: unknown piece {kind}")
    return exp


def matches(kind, want, line):
    s = line.strip()
    if kind == "frozen":
        return line.rstrip() == want
    if kind == "new":
        return s == want
    if kind == "alias":
        m = ALIAS.match(s)
        return bool(m) and m.group(1) == want and m.group(2) == want
    return s == f"w.{want} = {want};"


def find_starts(src, exp, has_body):
    """Lines of src where the unit's first (up to three) code lines match in order.  A
    declaration without a body (a prototype) is not a match for a unit that has one."""
    rows = [i for i in range(1, len(src.code)) if src.code[i].strip()]
    k = min(3, len(exp))
    out = []
    for n, i in enumerate(rows):
        if n + k > len(rows):
            break
        if all(matches(exp[j][0], exp[j][1], src.code[rows[n + j]]) for j in range(k)):
            if has_body and "{" not in "".join(src.blank[i:src.extent(i) + 1]):
                continue
            out.append(i)
    return out


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--tree", default=str(ROOT),
                    help="directory holding the new files (default: this checkout)")
    a = ap.parse_args()
    tree = pathlib.Path(a.tree)

    problems = []
    frozen = frozen_source()
    code = frozen_with_subs(frozen, problems)
    new = {p: Source((tree / p).read_text(encoding="utf-8")) for p in FILES}
    claimed = collections.Counter()
    covered = {p: set() for p in FILES}
    new_literals = collections.Counter()

    # 1. Units.
    n_pass = 0
    for name, path, pieces in UNITS:
        exp = expected(code, pieces, claimed, problems, name)
        for kind, text in exp:
            if kind == "new":
                new_literals.update(literals(text))
        if not exp:
            problems.append(f"{name}: no code lines")
            continue
        has_body = any("{" in t for kind, t in exp if kind in ("frozen", "new"))
        src = new[path]
        starts = find_starts(src, exp, has_body)
        others = [p for p in FILES if p != path and find_starts(new[p], exp, has_body)]
        if len(starts) != 1 or others:
            problems.append(f"{name}: first line found {len(starts)} times in {path}"
                            + (f", also in {others}" if others else ""))
            continue
        s = starts[0]
        rows = [i for i in range(s, len(src.code)) if src.code[i].strip()][:len(exp)]
        e = rows[-1]
        covered[path].update(range(s, e + 1))
        bad = next((k for k, (kind, want) in enumerate(exp)
                    if k >= len(rows) or not matches(kind, want, src.code[rows[k]])), None)
        if bad is not None:
            got = src.code[rows[bad]].rstrip() if bad < len(rows) else "<end of file>"
            problems.append(f"{name}: differs at code line {bad + 1} of {len(exp)} "
                            f"({path}:{rows[bad] if bad < len(rows) else '?'}):\n"
                            f"      expected ({exp[bad][0]}): {exp[bad][1]}\n"
                            f"      found          : {got}")
            continue
        body = "".join(src.blank[s:e + 1])
        if body.count("{") != body.count("}") or not src.code[e].rstrip().endswith(("}", ";")):
            problems.append(f"{name}: the carried lines do not close the item at {path}:{e}")
            continue
        n_pass += 1
        if a.verbose:
            print(f"PASS {name}: {len(exp)} code lines at {path}:{s}-{e}")

    # 2. Coverage.
    for i in range(1, len(frozen.code)):
        if not frozen.code[i].strip():
            continue
        if in_ranges(i, SCAFFOLD):
            claimed[i] += 1
        if claimed[i] == 0 and not dropped(i):
            problems.append(f"coverage: frozen line {i} is carried by no unit: {frozen.code[i].strip()}")
        elif claimed[i] > 1:
            problems.append(f"coverage: frozen line {i} is carried {claimed[i]} times")

    # 3. Nothing added.
    for p in FILES:
        src = new[p]
        types = DECLARED_TYPES.get(p, [])
        i = 1
        while i < len(src.code):
            line = src.code[i].strip()
            if not line or i in covered[p] or ALLOWED_OUTSIDE.match(line):
                i += 1
                continue
            if p.endswith(".hpp"):
                e = src.extent(i)
                body = "".join(src.blank[i:e + 1])
                if any(re.match(t, line) for t in types):
                    i = e + 1
                    continue
                if "{" not in body and body.rstrip().endswith(";"):
                    i = e + 1
                    continue
            problems.append(f"added: {p}:{i}: {line}")
            i += 1

    # 4. Literals.
    lit_frozen = collections.Counter()
    for i in range(1, len(frozen.code)):
        if in_ranges(i, SCAFFOLD) or dropped(i):
            continue
        lit_frozen.update(literals(code[i]))
    lit_frozen.update(new_literals)
    lit_new = collections.Counter()
    for p in FILES:
        for line in new[p].code[1:]:
            if line.strip().startswith("#include"):
                continue
            lit_new.update(literals(line))
    if lit_frozen != lit_new:
        problems.append(f"literals: only expected {dict(lit_frozen - lit_new)}, "
                        f"only found {dict(lit_new - lit_frozen)}")

    for p in problems:
        print("FAIL", p)
    n_lines = sum(1 for i in range(1, len(frozen.code)) if frozen.code[i].strip())
    n_drop = sum(1 for i in range(1, len(frozen.code)) if frozen.code[i].strip() and dropped(i))
    print(f"units {n_pass}/{len(UNITS)} PASS; frozen code lines {n_lines} "
          f"({n_drop} dropped, {len(DROPS)} declared drops); substitutions {len(SUBS)}; "
          f"literals {sum(lit_new.values())}; problems {len(problems)}")
    print("RESULT", "PASS" if not problems else "FAIL")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

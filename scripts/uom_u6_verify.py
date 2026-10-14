#!/usr/bin/env python3
"""Relocation verifier for card UOM step U6 (gate G-UR): StateManager organized by concern.

    python scripts/uom_u6_verify.py [--verbose] [--new-root DIR] [--mutation-test]

The frozen source is src/core/filter/StateManager.cpp and its headers at f823a36 (the base
of step U6), read with `git show` (no copy is stored).  Comments and blank lines are ignored
on both sides (the lexer of scripts/rt_seam_verify.py), so a rewritten comment never fails
the check; every other character of a carried line is compared, indentation included.

The manifest below records the whole step against the frozen file: U6b's deletions (DROPS),
U6c's cut of getProcessNoiseMatrix into one function per block, U6d's cut of the
constructor, U6e's table reads (declared substitutions) and U6f's new code (declared new
lines).  A unit is one item of a new file, an ordered list of pieces:

  F(a, b, shift)  the frozen code lines a..b after the declared substitutions of SUBS, with
                  `shift` leading spaces removed, compared character for character;
  N(text)         one declared new code line (a signature, a call, a brace), compared without
                  its surrounding space.

Exit 0 only when every check passes:

  1. Units.  Each unit is found in its file by its first line, which occurs there once; its
     code lines equal the pieces one for one, consecutively.
  2. Coverage.  Every code line of the frozen StateManager.cpp is carried by exactly one
     unit, or is a declared drop (DROPS) with its reason.  None is dropped silently, none
     carried twice.
  3. Nothing added.  Outside its units a new file holds only #include lines and namespace
     lines with their closing braces (one lone closing brace per unclaimed namespace).
  4. Log calls and literals.  The ordered list of log calls of each unit, and the multiset
     of string literals and log calls over the new files, equal those of the frozen lines
     they carry plus the declared new lines: a move loses or adds no log text.
  5. Headers.  The frozen code lines of each header appear in its new version in the same
     order, apart from the declared header drops and substitutions, and StateManager's data
     members are the same names in the same order (their order is the construction order).

--mutation-test changes one character of one carried line (in memory) and requires the
check to FAIL, so a verifier that cannot see a change is caught.
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

FROZEN = "f823a36"
CPP = "src/core/filter/StateManager.cpp"
HPP = "include/filter/StateManager.hpp"
FAM = "include/filter/StateFamily.hpp"
SCH = "include/filter/StateKeySchema.hpp"
D = "src/core/filter/"
SM, PN, LY, FA = (D + "StateManager.cpp", D + "StateManagerProcessNoise.cpp",
                  D + "StateManagerLayout.cpp", D + "StateManagerFamilies.cpp")
NEW_FILES = [SM, PN, LY, FA]


def F(a, b=None, shift=0):
    return ("F", a, a if b is None else b, shift)


def N(text):
    return ("N", text)


# ------------------------------------------------------------------------------------------
# Manifest
# ------------------------------------------------------------------------------------------

U6B_DEAD = "U6b: no caller in src or tests"
U6B_ZERO = "U6b: sat_bias_term was the constant 0.0; x - 0.0 == x for every double"
U6B_PRIV = "U6b: called only inside the class, declared again in the private section"

# Frozen StateManager.cpp line -> [(from, to)] (plain text replace), applied before the shift.
SUBS = {
    1355: [(" - iono_term - sat_bias_term;", " - iono_term;")],     # U6b
}

# Frozen StateManager.cpp lines dropped, with the reason.
DROPS = {
    1353: U6B_ZERO,
}

# Frozen StateManager.cpp line ranges dropped whole (their code lines join DROPS).
DROP_RANGES = [
    (2118, 2168, U6B_DEAD + " (resetSatPhaseBiasState)"),
    (2413, 2462, U6B_DEAD + " (resetSatClockState)"),
]

UNITS = [
    dict(name="StateManager.cpp (carried whole)", file=SM, pieces=[F(11, 2745)]),
]

# Frozen header lines: substitutions and drops, per header.
HSUBS = {HPP: {}, FAM: {}, SCH: {}}
HDROPS = {
    HPP: {
        451: U6B_PRIV + " (reinflationName)",
        1114: U6B_PRIV + " (evictExpiredSatBiasCache)",
        **{n: U6B_DEAD + " (getSatBiasCache)" for n in (1120, 1121, 1122)},
        **{n: U6B_PRIV + " (addSatPhaseBiasStateWithVariance)" for n in (1152, 1153, 1154)},
        **{n: U6B_DEAD + " (resetSatPhaseBiasState)" for n in (1159, 1160, 1161)},
        **{n: U6B_PRIV + " (addSatClockStateWithVariance)" for n in (1276, 1277, 1278)},
        **{n: U6B_DEAD + " (resetSatClockState)" for n in (1283, 1284, 1285)},
        1593: U6B_DEAD + " (meas_var_scale_: no member function reads or writes it)",
    },
    FAM: {},
    SCH: {},
}


# ------------------------------------------------------------------------------------------
# Engine
# ------------------------------------------------------------------------------------------
LOG_CALL = re.compile(r"->\s*(trace|debug|info|warn|error|critical)\s*\(")
FURNITURE = re.compile(r"^\s*(#include\b.*|namespace(\s+\w+)?\s*\{\s*|using\s+[\w:]+\s*;\s*)$")


def git_show(rev, path):
    out = subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"],
                         capture_output=True, check=True)
    return out.stdout.decode("utf-8").replace("\r\n", "\n")


def code_lines(text):
    """1-based list of comment-stripped, right-stripped lines."""
    return [""] + [l.rstrip() for l in strip_comments(text).split("\n")]


def literals(line):
    return [m.group() for m in _CPP.finditer(line) if m.lastgroup in ("str", "raw")]


def log_calls(line):
    """The log calls of one code line: (level, first literal of the line or '')."""
    lits = literals(line)
    return [(m.group(1), lits[0] if lits else "") for m in LOG_CALL.finditer(line)]


class Frozen:
    def __init__(self, text, subs):
        self.raw = [""] + text.split("\n")
        raw = list(self.raw)
        for i, pairs in subs.items():
            for fr, to in pairs:
                if fr not in raw[i]:
                    raise ValueError(f"substitution {fr!r} not on frozen line {i}: {raw[i]!r}")
                raw[i] = raw[i].replace(fr, to)
        self.code = code_lines("\n".join(raw[1:]))
        self.plain = code_lines("\n".join(self.raw[1:]))


def expected(unit, FC, claims, problems):
    """[(text, kind, frozen_line)] the unit must hold; records the frozen lines it claims."""
    exp = []
    for p in unit["pieces"]:
        if p[0] == "F":
            _, a, b, k = p
            for i in range(a, b + 1):
                line = FC.code[i]
                if not line.strip():
                    continue
                claims.setdefault(i, []).append(unit["name"])
                if i in DROPS:
                    continue
                lead = len(line) - len(line.lstrip(" "))
                if lead < k:
                    problems.append(f"{unit['name']}: frozen {i} has fewer than {k} leading spaces")
                    continue
                exp.append((line[k:], "frozen", i))
        else:
            exp.append((p[1].strip(), "new", None))
    return exp


def match_unit(unit, exp, lines, problems):
    """Locate the unit in `lines` (1-based code lines of its file) and compare.  Returns the
    list of matched line numbers, or None."""
    if not exp:
        problems.append(f"{unit['name']}: empty unit")
        return None
    first, kind, _ = exp[0]
    cand = [i for i in range(1, len(lines))
            if (lines[i] if kind == "frozen" else lines[i].strip()) == first]
    if len(cand) != 1:
        problems.append(f"{unit['name']}: first line {first!r} found {len(cand)} times")
        return None
    got, j = [], cand[0]
    for text, kind, _ in exp:
        while j < len(lines) and not lines[j].strip():
            j += 1
        if j >= len(lines):
            problems.append(f"{unit['name']}: file ends before {text!r}")
            return None
        g = lines[j] if kind == "frozen" else lines[j].strip()
        if g != text:
            problems.append(f"{unit['name']}: new line {j} differs\n  want {text!r}\n  got  {lines[j]!r}")
            return None
        got.append(j)
        j += 1
    return got


def run_checks(texts, frozen_text, frozen_headers, verbose=False, quiet=False):
    """All checks over `texts` (new path -> text).  Returns the number of failed checks."""
    out = (lambda *a: None) if quiet else print
    FC = Frozen(frozen_text, SUBS)
    for a, b, why in DROP_RANGES:
        for i in range(a, b + 1):
            if FC.plain[i].strip():
                DROPS.setdefault(i, why)
    claims = {}
    lines_of = {p: code_lines(t) for p, t in texts.items()}
    claimed_new = {p: set() for p in texts}
    fails = 0
    lit_expected, lit_found = collections.Counter(), collections.Counter()
    log_expected, log_found = collections.Counter(), collections.Counter()

    out(f"frozen {FROZEN}: {CPP} ({len(FC.raw) - 1} lines)")
    out("\n1. Units")
    for unit in UNITS:
        problems = []
        exp = expected(unit, FC, claims, problems)
        if unit["file"] not in lines_of:
            problems.append(f"{unit['name']}: file {unit['file']} missing")
            got = None
        else:
            got = match_unit(unit, exp, lines_of[unit["file"]], problems) if not problems else None
        ok = got is not None and not problems
        if ok:
            claimed_new[unit["file"]].update(got)
            # ordered log calls of the unit equal those of its pieces
            want_logs = [c for text, _, _ in exp for c in log_calls(text)]
            got_logs = [c for j in got for c in log_calls(lines_of[unit["file"]][j])]
            if want_logs != got_logs:
                problems.append(f"{unit['name']}: ordered log calls differ")
                ok = False
        for text, kind, _ in exp:
            if text.strip().startswith("#include"):
                continue
            lit_expected.update(literals(text))
            log_expected.update(log_calls(text))
        n_f = sum(1 for _, k, _ in exp if k == "frozen")
        n_n = sum(1 for _, k, _ in exp if k == "new")
        out(f"  {'PASS' if ok else 'FAIL'}  {unit['name']}  "
            f"[{pathlib.Path(unit['file']).name}: {n_f} frozen, {n_n} new]")
        for p in problems:
            out("        " + p.replace("\n", "\n        "))
        fails += 0 if ok else 1

    out("\n2. Coverage of the frozen StateManager.cpp")
    cov = []
    for i in range(1, len(FC.plain)):
        if not FC.plain[i].strip():
            continue
        owners = claims.get(i, [])
        if i in DROPS and not owners:
            owners = ["drop"]
        if len(owners) != 1:
            cov.append(f"frozen {i} claimed {len(owners)} times {owners}: {FC.plain[i].strip()!r}")
    for i in DROPS:
        if not FC.plain[i].strip():
            cov.append(f"declared drop {i} is not a code line")
    n_code = sum(1 for l in FC.plain[1:] if l.strip())
    out(f"  {n_code} code lines; {len(DROPS)} declared drops; {'PASS' if not cov else 'FAIL'}")
    for p in cov:
        out("        " + p)
    fails += 1 if cov else 0

    out("\n3. Nothing added outside the units")
    extra = []
    for path, lines in lines_of.items():
        opens, closes = 0, 0
        for j in range(1, len(lines)):
            if not lines[j].strip() or j in claimed_new[path]:
                continue
            if FURNITURE.match(lines[j]):
                if lines[j].strip().startswith("namespace"):
                    opens += 1
                continue
            if lines[j].strip() == "}":
                closes += 1
                continue
            extra.append(f"{pathlib.Path(path).name}:{j}: {lines[j].strip()!r}")
        if opens != closes:
            extra.append(f"{pathlib.Path(path).name}: {opens} unclaimed namespace(s), {closes} lone brace(s)")
    out(f"  {len(lines_of)} file(s); {'PASS' if not extra else 'FAIL'}")
    for p in extra:
        out("        " + p)
    fails += 1 if extra else 0

    out("\n4. Log calls and literals over the new files")
    for path, lines in lines_of.items():
        for j in range(1, len(lines)):
            if lines[j].strip().startswith("#include"):
                continue
            lit_found.update(literals(lines[j]))
            log_found.update(log_calls(lines[j]))
    lit_bad = lit_expected != lit_found
    log_bad = log_expected != log_found
    out(f"  literals {sum(lit_found.values())} (expected {sum(lit_expected.values())}); "
        f"log calls {sum(log_found.values())} (expected {sum(log_expected.values())}); "
        f"{'PASS' if not (lit_bad or log_bad) else 'FAIL'}")
    if lit_bad:
        out(f"        literals only expected {dict(lit_expected - lit_found)}; "
            f"only found {dict(lit_found - lit_expected)}")
    if log_bad:
        out(f"        log calls only expected {dict(log_expected - log_found)}; "
            f"only found {dict(log_found - log_expected)}")
    fails += 1 if (lit_bad or log_bad) else 0

    out("\n5. Headers")
    for hdr, (frozen_h, new_h) in frozen_headers.items():
        FH = Frozen(frozen_h, HSUBS[hdr])
        new_lines = [l for l in code_lines(new_h)[1:] if l.strip()]
        want = [FH.code[i] for i in range(1, len(FH.code))
                if FH.code[i].strip() and i not in HDROPS[hdr]]
        j, missing = 0, None
        for w in want:
            while j < len(new_lines) and new_lines[j] != w:
                j += 1
            if j == len(new_lines):
                missing = w
                break
            j += 1
        hp = []
        if missing is not None:
            hp.append(f"frozen line not found in order: {missing!r}")
        for i in HDROPS[hdr]:
            if not FH.plain[i].strip():
                hp.append(f"declared header drop {i} is not a code line")
        if hdr == HPP:
            old_m = data_members([FH.code[i] for i in range(1, len(FH.code)) if i not in HDROPS[hdr]])
            new_m = data_members(new_lines)
            if old_m != new_m:
                hp.append(f"data members differ:\n  frozen {old_m}\n  new    {new_m}")
        added = len(new_lines) - len(want)
        out(f"  {'PASS' if not hp else 'FAIL'}  {hdr}: {len(want)} frozen code lines in order, "
            f"{len(HDROPS[hdr])} dropped, {added} code line(s) added")
        if verbose:
            pos = 0
            wset = collections.Counter(want)
            for l in new_lines:
                if wset[l] > 0:
                    wset[l] -= 1
                else:
                    out(f"        + {l.strip()}")
        for p in hp:
            out("        " + p.replace("\n", "\n        "))
        fails += 1 if hp else 0
    return fails


def data_members(lines):
    pat = re.compile(r"^(?!\s*(?:return|friend|using|typedef)\b)\s*[\w:<][\w:<>,\s\*&]*?"
                     r"[\w>\*&]\s+([A-Za-z_]\w*_)\s*(\{[^}]*\}|=[^;]*)?;\s*$")
    return [m.group(1) for l in lines if (m := pat.match(l))]


def main():
    for s in (sys.stdout, sys.stderr):
        s.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--new-root", default=str(ROOT))
    ap.add_argument("--mutation-test", action="store_true")
    a = ap.parse_args()
    root = pathlib.Path(a.new_root)

    frozen_text = git_show(FROZEN, CPP)
    texts = {p: (root / p).read_text(encoding="utf-8").replace("\r\n", "\n")
             for p in NEW_FILES if (root / p).exists()}
    headers = {h: (git_show(FROZEN, h), (root / h).read_text(encoding="utf-8").replace("\r\n", "\n"))
               for h in (HPP, FAM, SCH)}

    if a.mutation_test:
        # Change one character of the first carried line that holds a digit, in memory.
        for unit in UNITS:
            for p in unit["pieces"]:
                if p[0] != "F":
                    continue
                for i in range(p[1], p[2] + 1):
                    line = Frozen(frozen_text, SUBS).code[i]
                    m = re.search(r"\d", line)
                    if not m or i in DROPS:
                        continue
                    new_line = line[p[3]:]
                    t = texts[unit["file"]]
                    k = t.find(new_line.strip())
                    if k < 0:
                        continue
                    pos = k + new_line.strip().find(m.group())
                    mutated = dict(texts)
                    mutated[unit["file"]] = t[:pos] + ("1" if t[pos] != "1" else "2") + t[pos + 1:]
                    f = run_checks(mutated, frozen_text, headers, quiet=True)
                    print(f"mutation of {pathlib.Path(unit['file']).name} (frozen line {i}): "
                          f"{'FAIL as required' if f else 'NOT DETECTED'}")
                    return 0 if f else 1
        print("no line to mutate")
        return 1

    fails = run_checks(texts, frozen_text, headers, verbose=a.verbose)
    print("\nDeclared substitutions (frozen line: from -> to)")
    for i in sorted(SUBS):
        for fr, to in SUBS[i]:
            print(f"  cpp {i}: {fr!r} -> {to!r}")
    for h in HSUBS:
        for i in sorted(HSUBS[h]):
            for fr, to in HSUBS[h][i]:
                print(f"  {pathlib.Path(h).name} {i}: {fr!r} -> {to!r}")
    print("Declared drops")
    FCp = code_lines(frozen_text)
    for i in sorted(DROPS):
        print(f"  cpp {i}: {FCp[i].strip()!r}  ({DROPS[i]})")
    for h in HDROPS:
        hp = code_lines(git_show(FROZEN, h))
        for i in sorted(HDROPS[h]):
            print(f"  {pathlib.Path(h).name} {i}: {hp[i].strip()!r}  ({HDROPS[h][i]})")
    print(f"\nTOTAL: {len(UNITS)} unit(s); {'PASS' if fails == 0 else f'FAIL ({fails} check(s))'}")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

"""Verifier of card NOM step N3b: the readers on the roster (scripts/nom3_designs/N3b_decisions.md).
Checks the working tree against the frozen base (nom/wave2):
  V1 lines: in every touched existing file, every frozen line lands exactly once and in order;
     the only frozen lines that change are the closed list CHANGED, each changed exactly once,
     and each change keeps every frozen token in order (tokens are only inserted), except the
     declared value substitutions SUBST (the run-end table's row count).
  V2 brackets: every bracket of every touched C++ file is matched.
  V3 log calls and event writes: no line added to an existing file holds a log call or an event
     write, so the ordered log calls and event writes of every touched body are the frozen ones.
  V4 bindings: the two signatures that grew keep every frozen parameter (type, constness and
     reference category), and only append defaulted parameters.
Usage: python scripts/nom_n3b_verify.py [--base <rev>] [--controls]
  --controls runs the negative controls first; each must fail.
"""
import argparse
import difflib
import os
import re
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nom_cpp as nc  # noqa: E402
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = "nom/wave2"
ASM = "src/core/app/ApplicationNetworkAssembly.cpp"
SQC = "src/core/slip/NetworkSlipQc.cpp"
SQH = "include/slip/NetworkSlipQc.hpp"
TOUCHED = [ASM, SQC, SQH, "include/app/SignalRoster.hpp", "src/core/app/SignalRoster.cpp",
           "src/core/filter/NetworkRunEnd.cpp", "include/filter/NetworkRunEnd.hpp"]
NEW_FILES = ["src/core/app/SignalRosterSites.cpp", "tests/test_signal_roster_sites.cpp"]
# The closed list of frozen lines a change touches: (file, frozen line, stripped).
CHANGED = [
    (ASM, "st.model ? st.model->latchedCanonicalPhaseOnBandDigit(cons, bd)"),
    (ASM, "[&stations](int sta, const std::string& system, char band) -> std::string {"),
    (ASM, "return m->latchedCanonicalPhaseOnBandDigit(system, band);"),
    (ASM, "&local_carry, pl_now_s, ctx.knobs.pl_max_gap_s);"),
    (SQH, "double pl_now_s, double pl_max_gap_s, bool* via_carry = nullptr);"),
    (SQH, "double pl_now_s = 0.0, double pl_max_gap_s = 0.0);"),
    (SQC, "double pl_now_s, double pl_max_gap_s, bool* via_carry) {"),
    (SQC, "double pl_now_s, double pl_max_gap_s) {"),
    (SQC, "pl_max_gap_s, &via_carry);"),
    ("include/filter/NetworkRunEnd.hpp",
     "inline constexpr std::size_t kNetworkRunEndReportCount = 105;"),
]
# Declared value substitutions inside a changed line: (frozen token, new token).
SUBST = {("include/filter/NetworkRunEnd.hpp", "105"): "106"}
LOGRE = re.compile(r"\blog->(info|warn|error|debug|trace|critical)\b|emitEvent\s*\(|\bspdlog::")


class Fail(Exception):
    pass


def git_show(base, path):
    return subprocess.run(["git", "-C", ROOT, "show", f"{base}:{path}"], capture_output=True,
                          check=True).stdout.decode("utf-8").replace("\r\n", "\n")


def read(path, over):
    if path in over:
        return over[path]
    with open(os.path.join(ROOT, path), "rb") as f:
        return f.read().decode("utf-8").replace("\r\n", "\n")


def toks(line):
    return [t.text for t in nc.lex(line)]


def subseq(a, b):
    """True when token list @p a is an in-order subsequence of @p b."""
    it = iter(b)
    return all(any(x == y for y in it) for x in a)


def check(base, over):
    errs = []
    seen = {c: 0 for c in CHANGED}
    for path in TOUCHED:
        old = [ln.strip() for ln in git_show(base, path).split("\n")]
        new_raw = read(path, over).split("\n")
        new = [ln.strip() for ln in new_raw]
        sm = difflib.SequenceMatcher(a=old, b=new, autojunk=False)
        for op, a0, a1, b0, b1 in sm.get_opcodes():
            if op == "equal":
                continue
            added = new[b0:b1]
            for ln in added:
                if LOGRE.search(ln):
                    errs.append(f"V3 {path}: an added line holds a log call or event write: {ln[:80]}")
            if op == "insert":
                continue
            joined = chr(10).join(added)   # one line each: a // comment ends at its own line
            for ln in old[a0:a1]:
                if not ln or ln.startswith("//") or ln.startswith("*"):
                    continue   # a frozen blank or comment line is not a statement line
                key = (path, ln)
                if key not in seen:
                    errs.append(f"V1 {path}: frozen line dropped or changed: {ln[:90]}")
                    continue
                seen[key] += 1
                ft = [SUBST.get((path, t), t) for t in toks(ln)]
                if not subseq(ft, toks(joined)):
                    errs.append(f"V1 {path}: changed line loses frozen tokens: {ln[:90]}")
        T = nc.Tokens(read(path, over), path)
        bad = [T.toks[i].line for i in range(T.n)
               if T.k(i) == "op" and T.t(i) in "{}()[]" and T.match[i] < 0]
        if bad:
            errs.append(f"V2 {path}: unmatched brackets at lines {bad[:5]}")
    for key, n in seen.items():
        if n != 1:
            errs.append(f"V1 {key[0]}: declared change applied {n} times: {key[1][:80]}")
    for path in NEW_FILES:
        T = nc.Tokens(read(path, over), path)
        bad = [T.toks[i].line for i in range(T.n)
               if T.k(i) == "op" and T.t(i) in "{}()[]" and T.match[i] < 0]
        if bad:
            errs.append(f"V2 {path}: unmatched brackets at lines {bad[:5]}")
    # V4: each grown signature ends with the frozen parameter list plus defaulted parameters.
    for path, name in ((SQH, "networkWindupRestartTargets"), (SQH, "networkSlipDrainWindupRestart"),
                       (SQC, "networkWindupRestartTargets"), (SQC, "networkSlipDrainWindupRestart")):
        def params(text):
            T = nc.Tokens(text, path)
            for i in range(T.n):
                if T.t(i) == name and T.t(i + 1) == "(" and T.t(i - 1) not in (".", "->"):
                    j = T.match[i + 1]
                    if T.t(j + 1) in ("{", ";"):
                        return [t.text for t in T.toks[i + 2:j]]
            raise Fail(f"{path}: no declaration of {name}")
        try:
            p0, p1 = params(git_show(base, path)), params(read(path, over))
        except Fail as e:
            errs.append(f"V4 {e}")
            continue
        if p1[:len(p0)] != p0 or (p1[len(p0):] and p1[len(p0)] != ","):
            errs.append(f"V4 {path} {name}: a frozen parameter changed")
        elif path == SQH and p1[len(p0):].count("=") != p1[len(p0):].count(","):
            errs.append(f"V4 {path} {name}: an appended parameter has no default")
    return errs


def controls(base):
    """(name, overrides) of the negative controls; each must fail."""
    asm, sqc, ros = read(ASM, {}), read(SQC, {}), read("src/core/app/SignalRoster.cpp", {})
    hdr = read(SQH, {})
    a = "        if (!stations[i].model || !stations[i].sm) continue;\n"
    b = "        const auto& ledger = stations[i].model->wuRestarts();\n"
    return [
        ("a frozen line dropped", {ASM: asm.replace("    qc.epoch.reset(stations.size());\n", "", 1)}),
        ("two frozen lines swapped", {SQC: sqc.replace(a + b, b + a, 1)}),
        ("an undeclared change to a frozen line",
         {SQC: sqc.replace("if (rec.gap_s > carry_gap_s) {", "if (rec.gap_s >= carry_gap_s) {", 1)}),
        ("a log call added", {SQC: sqc.replace("            targets = std::move(named);\n",
                                               "            log->info(\"x\");\n"
                                               "            targets = std::move(named);\n", 1)}),
        ("a brace unbalanced", {"src/core/app/SignalRoster.cpp":
                                ros.replace("signalRosterSiteCensus(rs, ctr.n3b);",
                                            "signalRosterSiteCensus(rs, ctr.n3b); {", 1)}),
        ("a frozen parameter retyped",
         {SQH: hdr.replace("double pl_now_s, double pl_max_gap_s, bool* via_carry = nullptr,",
                           "double pl_now_s, float pl_max_gap_s, bool* via_carry = nullptr,", 1)}),
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=BASE)
    ap.add_argument("--controls", action="store_true")
    args = ap.parse_args()
    ok = True
    if args.controls:
        for name, over in controls(args.base):
            for path, text in over.items():
                if text == read(path, {}):
                    raise SystemExit(f"control {name!r} did not mutate {path}")
            errs = check(args.base, over)
            print(f"  [{'fails, as it must' if errs else 'PASSES: control broken'}] control: {name}"
                  + (f" ({errs[0][:110]})" if errs else ""))
            ok &= bool(errs)
    errs = check(args.base, {})
    for e in errs:
        print("  [FAIL] " + e)
    if not errs:
        print("  [pass] V1 frozen lines (closed change list), V2 brackets, V3 log calls and event "
              "writes, V4 grown signatures")
    ok &= not errs
    print("nom_n3b_verify: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

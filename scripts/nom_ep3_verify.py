"""Verifier of card NOM3 step EP3: the 21 measure-only instruments as the declared table kNetworkEpochObservers.

Checks the working tree against the frozen base (scripts/nom3_designs/EP3_design.md section 4):
  V1 spine: the frozen spine's statements, each slot's run of observer calls replaced by its slot call and
     the declared drops applied, equal the new spine's statements in order (so each lands exactly once);
     the slot calls appear once each, in the order of NetworkObserverSlot.
  V2 table: the rows of filter/NetworkEpochObservers.cpp equal the pre-registered rows (name, slot, order),
     each row's name is its function, and the frozen spine's observer calls are those rows in that order.
  V3 gates: the dropped driver gates are the seams' first statements after their bindings (negated); every
     binding before them is a member path of the run or the epoch (no call, so no side effect before a gate).
  V4 bodies: every observer's home file is token-identical to the base, except the one gate statement added
     to networkRhsAttribution; outside the spine function, the spine file gains only the table's include;
     every bracket of every touched file is matched (brace balance per function).
Log calls and event writes: no body moves in EP3, so their order is covered by V1 (the spine) and V4 (the seams).

Usage: python scripts/nom_ep3_verify.py [--base <rev>] [--controls]
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
BASE = "8f4f1db"
SPINE = "src/core/app/ApplicationNetworkEpoch.cpp"
TABLE = "src/core/filter/NetworkEpochObservers.cpp"
HEADER = "include/filter/NetworkEpochObservers.hpp"
END_MARK = "// ---- End of the bindings ----"

SLOTS = ["Admitted", "BeforeColumns", "AfterClockQc", "RosterTail", "FrameDiag", "StashTail"]
# The pre-registered rows (EP3_design.md section 1): (seam, slot, home file).
PRE = [
    ("networkParamInventoryNoteSatellites", "Admitted", "src/core/filter/NetworkParamInventory.cpp"),
    ("networkLevelAuthorityCensus", "BeforeColumns", "src/core/filter/NetworkLevelAuthority.cpp"),
    ("networkPhaseVisibleMonitor", "AfterClockQc", "src/core/filter/NetworkPhaseVisibleDiag.cpp"),
    ("networkLedgerEmitFold", "RosterTail", "src/core/filter/NetworkLedgerEmit.cpp"),
    ("networkSecondPhaseEpochTail", "RosterTail", "src/core/filter/NetworkSecondPhaseScope.cpp"),
    ("networkChannelAllEpoch", "RosterTail", "src/core/filter/NetworkChannelAll.cpp"),
    ("networkMultiGnssEpoch", "RosterTail", "src/core/filter/NetworkMultiGnss.cpp"),
    ("networkParamInventoryEpochTail", "RosterTail", "src/core/filter/NetworkParamInventory.cpp"),
    ("networkContributionsEpochTail", "RosterTail", "src/core/filter/NetworkContributions.cpp"),
    ("networkPostfitSigma0", "FrameDiag", "src/core/filter/NetworkFrameDiag.cpp"),
    ("networkShSpectrum", "FrameDiag", "src/core/filter/NetworkFrameDiag.cpp"),
    ("networkStationBlock", "FrameDiag", "src/core/filter/NetworkStationBlock.cpp"),
    ("networkPrefitCensus", "FrameDiag", "src/core/filter/NetworkPrefitCensus.cpp"),
    ("networkRedundancyCensus", "FrameDiag", "src/core/filter/NetworkRedundancyCensus.cpp"),
    ("networkBlockBaardaEmit", "FrameDiag", "src/core/filter/NetworkBlockBaarda.cpp"),
    ("networkCongruenceEmit", "FrameDiag", "src/core/filter/NetworkCongruence.cpp"),
    ("networkMdbFlush", "FrameDiag", "src/core/filter/NetworkMdb.cpp"),
    ("networkDatumGraphCensus", "FrameDiag", "src/core/filter/NetworkDatumGraph.cpp"),
    ("networkRhsAttribution", "FrameDiag", "src/core/filter/NetworkRhsAttrib.cpp"),
    ("networkRhsAttribTrip", "FrameDiag", "src/core/filter/NetworkRhsAttrib.cpp"),
    ("networkAmbStateTrace", "StashTail", "src/core/filter/NetworkAmbStateTrace.cpp"),
]
# The closed list of driver gates (frozen gate text, seam, the seam statement that replaces it, file).
GATES = [
    ("ctx . knobs . pc_walk_on", "networkPrefitCensus", "if ( ! ctx . knobs . pc_walk_on ) return ;",
     "src/core/filter/NetworkPrefitCensus.cpp"),
    ("ctx . knobs . dg_on", "networkDatumGraphCensus", "if ( ! ctx . knobs . dg_on ) return ;",
     "src/core/filter/NetworkDatumGraph.cpp"),
    ("est . attribArmed ( )", "networkRhsAttribution", "if ( ! est . attribArmed ( ) ) return ;",
     "src/core/filter/NetworkRhsAttrib.cpp"),
]
ADDED = {"src/core/filter/NetworkRhsAttrib.cpp": "if ( ! est . attribArmed ( ) ) return ;"}
DROPS = ["NetworkEstimator & est = run . engines . est ;"]
INCLUDE = '#include "filter/NetworkEpochObservers.hpp"'
ROW = re.compile(r'\{"(\w+)",\s*"[^"]*",\s*"([^"]*)",\s*Slot::(\w+),\s*&(\w+)\}')


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


def stext(T, s, e):
    return " ".join(T.toks[k].text for k in range(s, e))


def defs_of(T, name, nparams=None):
    """(open, close) brace tokens of every definition of @p name (optionally with @p nparams params)."""
    out = []
    for i, t in enumerate(T.toks):
        if t.text == name and t.kind == "id" and T.t(i + 1) == "(" and T.t(i - 1) not in (".", "->"):
            j = T.match[i + 1]
            if j > 0 and T.t(j + 1) == "{":
                n = 0 if j == i + 2 else 1 + sum(1 for k in nc.top_level(T, i + 2, j) if T.t(k) == ",")
                if nparams is None or n == nparams:
                    out.append((j + 1, T.match[j + 1]))
    return out


def one_def(T, name, nparams=None):
    d = defs_of(T, name, nparams)
    if len(d) != 1:
        raise Fail(f"{name}: {len(d)} definitions with {nparams} parameters")
    return d[0]


def body_stmts(T, name, nparams=None):
    o, c = one_def(T, name, nparams)
    return [(s, e, stext(T, s, e)) for s, e in nc.statements(T, o + 1, c)]


def edit_ops(a, b):
    """The non-equal opcodes of token lists @p a -> @p b, as one region between the common prefix and
    suffix (linear; a pure insertion or an identity is all the checks below accept)."""
    if a == b:
        return []
    p = 0
    while p < min(len(a), len(b)) and a[p] == b[p]:
        p += 1
    s = 0
    while s < min(len(a), len(b)) - p and a[-1 - s] == b[-1 - s]:
        s += 1
    op = "insert" if len(a) - s == p else "replace"
    return [(op, p, len(a) - s, p, len(b) - s)]


def brackets_matched(T, path, errs):
    bad = [T.toks[i].line for i in range(T.n) if T.k(i) == "op" and T.t(i) in "{}()[]" and T.match[i] < 0]
    if bad:
        errs.append(f"V4 {path}: unmatched brackets at lines {bad[:5]}")


def table_rows(over):
    return [(n, h, s, f) for n, h, s, f in ROW.findall(read(TABLE, over))]


def check(base, over):
    errs = []
    rows = table_rows(over)
    # ---- V2 the table ----
    if [(n, s) for n, _, s, _ in rows] != [(n, s) for n, s, _ in PRE]:
        errs.append("V2 the table's rows differ from the pre-registration: "
                    + ", ".join(f"{n}@{s}" for n, _, s, _ in rows))
    for n, h, s, f in rows:
        if n != f:
            errs.append(f"V2 row {n} points at {f}")
        if s not in SLOTS:
            errs.append(f"V2 row {n}: unknown slot {s}")
    hdr = read(HEADER, over)
    m = re.search(r"enum class NetworkObserverSlot[^{]*\{([^}]*)\}", hdr)
    enum = re.findall(r"^\s*(\w+),", m.group(1), re.M) if m else []
    if enum != SLOTS:
        errs.append(f"V2 the slot enum is {enum}")
    by_slot = {s: [n for n, _, s2, _ in rows if s2 == s] for s in SLOTS}
    homes = {n: h for n, h, _, _ in rows}
    for n, _, h in PRE:
        if "src/core/" + homes.get(n, "") != h:
            errs.append(f"V2 row {n}: home {homes.get(n)} (pre-registered {h})")
    # ---- V1 the spine ----
    FT, NT = nc.Tokens(git_show(base, SPINE)), nc.Tokens(read(SPINE, over))
    fz, nw = body_stmts(FT, "networkProcessEpoch_"), body_stmts(NT, "networkProcessEpoch_")
    obs_names = {n for n, _, _ in PRE}
    gate_of = {seam: g for g, seam, _, _ in GATES}

    def observer_of(text):
        mm = re.fullmatch(r"(?:if \( (.+) \) )?(\w+) \( run , ep \) ;", text)
        if not mm or mm.group(2) not in obs_names:
            return None
        gate, name = mm.group(1), mm.group(2)
        if gate is not None and gate_of.get(name) != gate:
            return None   # a gate outside the closed list is not a declared substitution
        return name

    expect, run, frozen_calls = [], [], []
    for _, _, t in fz + [(0, 0, "")]:
        name = observer_of(t) if t else None
        if name:
            run.append(name)
            frozen_calls.append(name)
            continue
        if run:
            slot = next((s for s in SLOTS if by_slot[s] == run), None)
            if slot is None:
                errs.append(f"V1 the frozen observer run {run} is no slot's rows in table order")
                slot = "?"
            expect.append(f"networkObserveEpoch ( run , ep , NetworkObserverSlot :: {slot} ) ;")
            run = []
        if t and t not in DROPS:
            expect.append(t)
    if frozen_calls != [n for n, _, _ in PRE]:
        errs.append(f"V2 the frozen spine's observer calls are {frozen_calls}")
    got = [t for _, _, t in nw]
    if got != expect:
        sm = difflib.SequenceMatcher(a=expect, b=got, autojunk=False)
        for op, a0, a1, b0, b1 in sm.get_opcodes():
            if op != "equal":
                errs.append(f"V1 spine statements differ ({op}): expected {expect[a0:a1][:2]} got {got[b0:b1][:2]}")
                break
    slots_called = [m.group(1) for t in got
                    for m in [re.fullmatch(r"networkObserveEpoch \( run , ep , NetworkObserverSlot :: (\w+) \) ;", t)] if m]
    if slots_called != SLOTS:
        errs.append(f"V1 the slot calls are {slots_called}")
    # outside the spine function: only the include added
    fo, fc = one_def(FT, "networkProcessEpoch_")
    no, nc_ = one_def(NT, "networkProcessEpoch_")
    outside_f = [FT.toks[k].text for k in list(range(0, fo)) + list(range(fc + 1, FT.n))]
    outside_n = [NT.toks[k].text for k in list(range(0, no)) + list(range(nc_ + 1, NT.n))]
    ops = edit_ops(outside_f, outside_n)
    if len(ops) != 1 or ops[0][0] != "insert" or [x.split("//")[0].strip() for x in outside_n[ops[0][3]:ops[0][4]]] != [INCLUDE]:
        errs.append(f"V4 {SPINE}: outside the spine function the change is not the one include: {ops}")
    brackets_matched(NT, SPINE, errs)
    # ---- V3 the gates ----
    for gate, seam, stmt, path in GATES:
        T = nc.Tokens(read(path, over))
        src = read(path, over).split("\n")
        epoch = one_def(T, seam, 2)
        st = [(s, e, stext(T, s, e)) for s, e in nc.statements(T, epoch[0] + 1, epoch[1])]
        if len(st) == 1 and re.fullmatch(rf"{seam} \( run \. .*\) ;", st[0][2]):
            # an adapter: its forwarding call passes member paths only; the gate is the explicit form's first line
            args = st[0][2][len(seam) + 4:-4]
            if "(" in args:
                errs.append(f"V3 {seam}: the forwarding call evaluates a call before the gate")
            explicit = [d for d in defs_of(T, seam) if d != epoch]
            if len(explicit) != 1:
                errs.append(f"V3 {seam}: {len(explicit)} explicit forms")
                continue
            first = nc.statements(T, explicit[0][0] + 1, explicit[0][1])[0]
            if stext(T, *first) != stmt:
                errs.append(f"V3 {seam}: the explicit form opens with {stext(T, *first)!r}, not the gate")
            continue
        lo, lc = T.toks[epoch[0]].line, T.toks[epoch[1]].line
        marks = [n for n in range(lo, lc + 1) if src[n - 1].strip() == END_MARK]
        if len(marks) != 1:
            errs.append(f"V3 {seam}: {len(marks)} binding end markers")
            continue
        binds = [x for x in st if T.toks[x[0]].line < marks[0]]
        after = [x for x in st if T.toks[x[0]].line > marks[0]]
        for _, _, t in binds:
            init = t.split(" = ", 1)[1] if " = " in t else ""
            if not re.fullmatch(r"&? ?(run|ep) \. [\w .]+ ;", init) or "(" in init:
                errs.append(f"V3 {seam}: the binding {t!r} is not a member path of the run or the epoch")
        if not after or after[0][2] != stmt:
            errs.append(f"V3 {seam}: the first statement after the bindings is "
                        f"{after[0][2] if after else None!r}, not {stmt!r}")
        if seam == "networkRhsAttribution" and "NetworkEstimator & est = run . engines . est ;" not in [b[2] for b in binds]:
            errs.append("V3 networkRhsAttribution: est is not bound to run.engines.est")
    # ---- V4 the observer homes ----
    for path in sorted({h for _, _, h in PRE}):
        ft = [t.text for t in nc.Tokens(git_show(base, path)).toks]
        NTh = nc.Tokens(read(path, over))
        nt = [t.text for t in NTh.toks]
        brackets_matched(NTh, path, errs)
        if ft == nt and path not in ADDED:
            continue
        ops = edit_ops(ft, nt)
        want = ADDED.get(path)
        ok = (want is not None and len(ops) == 1 and ops[0][0] == "insert"
              and " ".join(nt[ops[0][3]:ops[0][4]]) == want)
        if ok:
            o, c = one_def(NTh, "networkRhsAttribution", 2)
            ok = o < ops[0][3] < c
        if not ok:
            errs.append(f"V4 {path}: changed beyond the declared insertion: {ops[:2]}")
    brackets_matched(nc.Tokens(read(TABLE, over)), TABLE, errs)
    return errs


def controls(base):
    """(name, overrides) of the negative controls; each must fail."""
    tab, spine = read(TABLE, {}), read(SPINE, {})
    attrib = read("src/core/filter/NetworkRhsAttrib.cpp", {})
    prefit = read("src/core/filter/NetworkPrefitCensus.cpp", {})
    a = '    {"networkShSpectrum",'
    b = '    {"networkStationBlock",'
    i, j = tab.index(a), tab.index(b)
    k = tab.index("    {", j + 1)
    swapped = tab[:i] + tab[j:k] + tab[i:j] + tab[k:]
    fd = "    networkObserveEpoch(run, ep, NetworkObserverSlot::FrameDiag);\n"
    end = "    networkSubEnd(run, SB_FRAMEDIAG);\n"
    lev = "    networkLeveling(run, ep);\n"
    return [
        ("two rows swapped", {TABLE: swapped}),
        ("a row in the wrong slot", {TABLE: tab.replace("Slot::FrameDiag, &networkMdbFlush", "Slot::StashTail, &networkMdbFlush")}),
        ("a slot call moved one statement", {SPINE: spine.replace(fd + end, end + fd)}),
        ("the attribution gate removed", {"src/core/filter/NetworkRhsAttrib.cpp":
                                          attrib.replace("    if (!est.attribArmed()) return;", "", 1)}),
        ("a spine statement duplicated", {SPINE: spine.replace(lev, lev + lev, 1)}),
        ("the prefit explicit gate removed", {"src/core/filter/NetworkPrefitCensus.cpp":
                                              prefit.replace("    if (!ctx.knobs.pc_walk_on) return;\n", "", 1)}),
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
        print("  [pass] V1 spine statements, V2 table, V3 gates, V4 observer homes and brackets")
    ok &= not errs
    print("nom_ep3_verify: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Card NOM3 step EP2: the relocation verifier (a reduced gate G-R, authorized for this step).

    python scripts/nom_ep2_verify.py [--base 7141240] [--controls]

Checks the cut of Application::networkProcessEpoch_ against the frozen driver of the base,
independently of the generator (scripts/nom_ep2_cut.py: this file holds its own manifest and
imports nothing from it).  Pre-registration: scripts/nom3_designs/EP2_design.md, section 4.

  V1 coverage and order: the new driver, with each call of a moved function expanded into the
     function's body (the lines below `// ---- End of the bindings ----`), reproduces the
     frozen function's statement lines exactly once each, in order, up to the closed list of
     declared drops and substitutions (DROPS, SUBS, ADDED).
  V2 brace balance: every moved function and every moved body alone is bracket-balanced.
  V3 bindings: every frozen-scope name a moved body reads is bound once in its function, by
     the frozen preamble line verbatim, or by its declared epoch-side form (EPOCH_BIND), whose
     type equals the frozen declarator's up to a declared leading const (D3) or a declared
     change (D2 config_, D6 the timer baseline); every binding's root is the run, the epoch,
     an output parameter or an earlier binding.
  V4 effects: per moved function, the ordered logger calls, event writes, trace-stream writes
     and returns equal the frozen range's (D1 applied).
  V5 gates: each gate moved to a seam is absent from the driver and is the seam's first
     statement after its bindings.

A statement line is the code tokens of one source line (scripts/nom_cpp.py's lexer: comments
dropped, whitespace normalized).  With --controls, five mutations of the new text are checked
to FAIL first (a swapped pair of lines, a dropped line, a const added to a written binding, a
reordered log call, a gate left out of its seam), then the real tree is checked.
Exit 0 when every check passes.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import nom_cpp as nc   # noqa: E402

DRIVER = "src/core/app/ApplicationNetworkEpoch.cpp"
END_MARK = "// ---- End of the bindings ----"

# The moved functions: name -> (file, frozen ranges it holds, the driver line that calls it).
MOVED = {
    "networkFastForward":       ("src/core/app/NetworkEpochAdmission.cpp", [(213, 217)]),
    "networkAdmitEpoch":        ("src/core/app/NetworkEpochAdmission.cpp", [(265, 334)]),
    "networkSectionTimersBegin": ("src/core/app/NetworkRun.cpp", [(340, 346), (348, 352)]),
    "networkSlipFunnelOpen":    ("src/core/filter/NetworkSlipFunnel.cpp", [(387, 395), (397, 417)]),
    "networkSlipFunnelUnionRaw": ("src/core/filter/NetworkSlipFunnel.cpp", [(446, 449)]),
    "networkPhaseAnchorEpoch":  ("src/core/filter/NetworkPhaseAnchorEpoch.cpp", [(500, 1012)]),
    "networkReceiverPhaseBiasAllocation": ("src/core/filter/NetworkPhaseAnchorEpoch.cpp", [(1014, 1041)]),
    "networkAmbFloatDropEpoch": ("src/core/filter/NetworkAmbFloat.cpp", [(1087, 1093), (1095, 1105)]),
    "networkGapResumeCensus":   ("src/core/slip/NetworkGapResumeCensus.cpp", [(1166, 1256)]),
    "networkCarryDriverBeginEpoch": ("src/core/filter/NetworkDdPriorBuild.cpp", [(1260, 1269)]),
    "networkBlockBaardaPredict": ("src/core/filter/NetworkBlockBaarda.cpp", [(1447, 1447)]),
    "networkCongruencePredict": ("src/core/filter/NetworkCongruence.cpp", [(1451, 1451)]),
    "networkBufferedTraceDrains": ("src/core/filter/NetworkAmbStateTrace.cpp", [(1541, 1554)]),
    "networkSectionTimersReadout": ("src/core/app/NetworkRun.cpp", [(1557, 1602)]),
}
# D1, D6: lines a moved body adds at its end (removed before the comparison).
TAILS = {"networkFastForward": ["return EpochAdmission :: Process ;"],
         "networkAdmitEpoch": ["return EpochAdmission :: Process ;"],
         "networkSectionTimersBegin": ["return EpochSectionTimers { sec_t0 , sec_epoch_t0 } ;"]}
# D1: frozen line -> (old, new) inside a moved range.
SUBS = {216: ("return LoopSignal :: Next ;", "return EpochAdmission :: Skip ;"),
        321: ("return LoopSignal :: Stop ;", "return EpochAdmission :: Stop ;"),
        332: ("return LoopSignal :: Next ;", "return EpochAdmission :: Skip ;")}
# D4: the forwarder re-declared in these bodies (the frozen lines 366-372).
EMIT_FWD = (366, 372)
EMIT_IN = {"networkPhaseAnchorEpoch", "networkGapResumeCensus"}
# D8: driver lines added beside the calls (the verdict mapping).
ADDED = ["if ( admission == EpochAdmission :: Stop ) return LoopSignal :: Stop ;",
         "if ( admission == EpochAdmission :: Skip ) return LoopSignal :: Next ;"]
# D7 and D5: frozen statement lines that leave the driver and land nowhere.
DROP_PREAMBLE = {"brdc_rts", "brdc_nav_staged", "brdc_nav_cursor", "anchor_incumbent",
                 "this_epoch_anchor", "start_anchor_s", "local_carry", "pl_max_gap_s",
                 "start_offset_s", "n_gated_epochs", "n_gated_key_epochs", "max_dur_anchor_s",
                 "max_dur_anchored", "phase_datum_eq34", "carry_invariant", "carry_innov_gate",
                 "carry_driver", "carry_sat_vertex", "carry_group_id", "carry_prev_tow",
                 "carry_innov_pending", "phase_datum_prioritized", "n_loop_epochs",
                 "decim_counter", "sec_sub_ep0"}
DROP_LINES = {347: "epoch binding (to T2)", 396: "epoch binding (to F1)",
              424: "epoch binding", 428: "epoch binding", 430: "epoch binding",
              435: "epoch binding", 1055: "epoch binding (to T2)",
              1094: "S1 block brace", 1106: "S1 block brace",
              1084: "oracle gate (in the seam)", 1301: "install-carry gate brace"}
for _n in range(366, 373):
    DROP_LINES[_n] = "D4 forwarder"
# D5: frozen gate line -> (seam file, seam, the seam's first statement after its bindings).
GATES = {
    1299: ("src/core/filter/NetworkInstallCarry.cpp", "networkInstallCarry",
           "if ( ! ctx . knobs . carry_invariant ) return ;"),
    1314: ("src/core/filter/NetworkGrossLos.cpp", "networkGrossLosScreen",
           "if ( phase == GrossLosPhase :: Decide && ! ctx . knobs . gl_emit_exclude ) return ;"),
    1452: ("src/core/filter/NetworkFrameDiag.cpp", "networkPostfitSigma0",
           "if ( ! ctx . knobs . trace_postfit_on ) return ;"),
    1454: ("src/core/filter/NetworkFrameDiag.cpp", "networkShSpectrum",
           "if ( ! ctx . knobs . trace_sh_spectrum ) return ;"),
    1461: ("src/core/filter/NetworkStationBlock.cpp", "networkStationBlock",
           "if ( ! ctx . knobs . trace_station_block ) return ;"),
    1477: ("src/core/filter/NetworkRedundancyCensus.cpp", "networkRedundancyCensus",
           "if ( ! ctx . knobs . redun_census_on ) return ;"),
    1485: ("src/core/filter/NetworkBlockBaarda.cpp", "networkBlockBaardaEmit",
           "if ( ! ctx . knobs . bb_on ) return ;"),
    1494: ("src/core/filter/NetworkCongruence.cpp", "networkCongruenceEmit",
           "if ( ! ctx . knobs . cg_on ) return ;"),
    1504: ("src/core/filter/NetworkMdb.cpp", "networkMdbFlush",
           "if ( ! ctx . knobs . mdb_on ) return ;"),
}
GATE_TEXT = {1299: "if ( carry_invariant ) {", 1314: "if ( ctx . knobs . gl_emit_exclude )",
             1452: "if ( ctx . knobs . trace_postfit_on )", 1454: "if ( ctx . knobs . trace_sh_spectrum )",
             1461: "if ( ctx . knobs . trace_station_block )", 1477: "if ( ctx . knobs . redun_census_on )",
             1485: "if ( ctx . knobs . bb_on )", 1494: "if ( ctx . knobs . cg_on )",
             1504: "if ( ctx . knobs . mdb_on )", 1084: "if ( ctx . carry . oracle_pin )"}
ORACLE = ("src/core/filter/NetworkOraclePin.cpp", "networkOraclePinEpoch",
          ["auto * pin = ctx . carry . oracle_pin . get ( ) ;", "if ( ! pin ) return ;"])
# D2/D3/D6: the declared epoch-side bindings, per (function, name): the allowed statement.
FS = "std :: set < std :: pair < int , std :: string >>"
EPOCH_BIND = {
    "config_": "const GnssConfig & config_ = run . config ;",
    "se": "const SyncedEpoch & se = ep . se ;",
    "pl_now_s": "const double pl_now_s = ep . clock . pl_now_s ;",
    "n_present": "const int & n_present = ep . columns . n_present ;",
    "sec_asm_ms_epoch": "const double & sec_asm_ms_epoch = ep . assembly . sec_asm_ms_epoch ;",
    "sec_t0": "const NetworkNormalEquations :: SectionTimers & sec_t0 = sec . t0 ;",
    "sec_epoch_t0": "const std :: chrono :: steady_clock :: time_point & sec_epoch_t0 = sec . epoch_t0 ;",
}
EPOCH_BIND_FN = {
    ("networkSlipFunnelOpen", "carry_slipped_ambs"): f"{FS} & carry_slipped_ambs = funnel . carry_slipped_ambs ;",
    ("networkSlipFunnelUnionRaw", "carry_slipped_ambs"): f"{FS} & carry_slipped_ambs = funnel . carry_slipped_ambs ;",
    ("networkSlipFunnelUnionRaw", "carry_slipped_raw"): f"const {FS} & carry_slipped_raw = funnel . carry_slipped_raw ;",
    ("networkPhaseAnchorEpoch", "carry_slipped_ambs"): f"const {FS} & carry_slipped_ambs = ep . funnel . carry_slipped_ambs ;",
    ("networkGapResumeCensus", "carry_slipped_ambs"): f"const {FS} & carry_slipped_ambs = ep . funnel . carry_slipped_ambs ;",
    ("networkPhaseAnchorEpoch", "modeled"): "const std :: vector < std :: vector < PppObservation >> & modeled = pass_a . modeled ;",
    ("networkPhaseAnchorEpoch", "present"): "const std :: vector < bool > & present = pass_a . present ;",
    ("networkPhaseAnchorEpoch", "shared_phase_keys"): "std :: set < std :: string > & shared_phase_keys = pass_a . shared_phase_keys ;",
    ("networkReceiverPhaseBiasAllocation", "present"): "const std :: vector < bool > & present = ep . pass_a . present ;",
    ("networkReceiverPhaseBiasAllocation", "shared_phase_keys"): "const std :: set < std :: string > & shared_phase_keys = ep . pass_a . shared_phase_keys ;",
    ("networkAmbFloatDropEpoch", "modeled"): "const std :: vector < std :: vector < PppObservation >> & modeled = ep . pass_a . modeled ;",
}
# Names a moved body declares itself (shadowing a frozen name): verified to be declared there.
SHADOW = {"networkCarryDriverBeginEpoch": {"dt"}}
ROOTS = {"run", "ep", "funnel", "pass_a", "sec"}
EFFECT = re.compile(r"\blog -> \w+|\bemitEvent \(|\btraces \. emitEvent \(|\btraces \. \w+ <<|\breturn\b[^;]*;")


class Fail(Exception):
    pass


def git_show(base, path):
    return subprocess.run(["git", "-C", ROOT, "show", f"{base}:{path}"], capture_output=True,
                          check=True).stdout.decode("utf-8").replace("\r\n", "\n")


def read(path):
    with open(os.path.join(ROOT, path), "rb") as f:
        return f.read().decode("utf-8").replace("\r\n", "\n")


def code_lines(T, l0, l1):
    """[(line, normalized code)] for source lines l0..l1 (inclusive) of lexed text T."""
    by = {}
    for t in T.toks:
        if l0 <= t.line <= l1:
            by.setdefault(t.line, []).append(t.text)
    return [(ln, " ".join(by[ln])) for ln in sorted(by)]


def find_def(T, name):
    """(open-brace token, close-brace token) of the definition of @p name in T."""
    hits = []
    for i, t in enumerate(T.toks):
        if t.text == name and T.t(i + 1) == "(" and T.k(i) == "id":
            j = T.match[i + 1]
            if j > 0 and T.t(j + 1) == "{" and T.t(i - 1) not in (".", "->", "return", "=", "(", ","):
                hits.append((j + 1, T.match[j + 1]))
    if len(hits) != 1:
        raise Fail(f"{name}: {len(hits)} definitions")
    return hits[0]


class Fn:
    """A moved (or gated) function: its bindings and its body as statement lines."""

    def __init__(self, text, T, name):
        self.name = name
        o, c = find_def(T, name)
        self.T, self.open, self.close = T, o, c
        lo, lc = T.toks[o].line, T.toks[c].line
        src = text.split("\n")
        marks = [n for n in range(lo, lc + 1) if src[n - 1].strip() == END_MARK]
        if len(marks) != 1:
            raise Fail(f"{name}: {len(marks)} binding end markers")
        self.mark = marks[0]
        self.bind = code_lines(T, lo + 1, self.mark - 1)
        self.body = [x for x in code_lines(T, self.mark + 1, lc) if x[0] < lc or x[1] != "}"]
        last = code_lines(T, lc, lc)
        if last and last[0][1] != "}":   # the closing line holds code before the brace
            self.body.append((lc, last[0][1][:-1].strip()))


def frozen_scope(FT, fo, fc):
    """name -> (line, statement text, Decl) of every top-level declaration of the frozen body."""
    out = {}
    for s, e in nc.statements(FT, fo + 1, fc):
        try:
            ds = nc.parse_declaration(FT, s, e) or []
        except Exception:
            ds = []
        for d in ds:
            out[d.name] = (FT.toks[s].line, " ".join(FT.toks[k].text for k in range(s, e)), d)
    return out


def decl_type(T, d):
    a, b = d.type_span
    return " ".join(T.toks[k].text for k in range(a, b))


def check(base, files):
    """Run V1..V5 over @p files (path -> text, the new tree).  Returns the failure list."""
    fails = []
    F = git_show(base, DRIVER)
    FT = nc.Tokens(F, "frozen")
    fo, fc = find_def(FT, "networkProcessEpoch_")
    flines = dict(code_lines(FT, FT.toks[fo].line + 1, FT.toks[fc].line - 1))
    scope = frozen_scope(FT, fo, fc)
    pre = {n: v for n, v in scope.items() if 159 <= v[0] <= 204}

    toks = {}
    def T_of(path):
        if path not in toks:
            toks[path] = nc.Tokens(files[path], path)
        return toks[path]

    fns = {}
    for name, (path, _) in MOVED.items():
        try:
            fns[name] = Fn(files[path], T_of(path), name)
        except Fail as e:
            fails.append(f"V2 {e}")
    if fails:
        return fails

    # ---- V1: expansion of the new driver against the frozen lines ----
    expected = []
    drop_pre_lines = {pre[n][0] for n in DROP_PREAMBLE}
    for ln, s in flines.items():
        if ln in DROP_LINES or ln in drop_pre_lines or ln in GATE_TEXT:
            if ln in GATE_TEXT and s != GATE_TEXT[ln]:
                fails.append(f"V5 frozen {ln}: gate text {s!r}")
            continue
        if ln in SUBS:
            old, new = SUBS[ln]
            if old not in s:
                fails.append(f"V1 frozen {ln}: {old!r} not found")
            s = s.replace(old, new)
        expected.append((ln, s))
    DT = T_of(DRIVER)
    do, dc = find_def(DT, "networkProcessEpoch_")
    got = []
    for ln, s in code_lines(DT, DT.toks[do].line + 1, DT.toks[dc].line - 1):
        if s in ADDED:
            continue
        called = [n for n in MOVED if re.search(r"\b" + n + r" \(", s)]
        if not called:
            got.append((f"driver:{ln}", s))
            continue
        fn = fns[called[0]]
        body = fn.body
        if called[0] in EMIT_IN:
            fwd = [x for _, x in code_lines(FT, *EMIT_FWD)]
            head = [x for _, x in body[:len(fwd)]]
            if head != fwd:
                fails.append(f"V1 {called[0]}: the event forwarder is not its first lines")
            body = body[len(fwd):]
        tail = TAILS.get(called[0], [])
        if tail:
            if [x for _, x in body[-len(tail):]] != tail:
                fails.append(f"V1 {called[0]}: the declared tail is missing")
            body = body[:-len(tail)]
        got.extend((f"{called[0]}:{l}", x) for l, x in body)
    i = 0
    for i, (e, g) in enumerate(zip(expected, got)):
        if e[1] != g[1]:
            fails.append(f"V1 first difference: frozen {e[0]} {e[1][:90]!r} vs {g[0]} {g[1][:90]!r}")
            break
    else:
        if len(expected) != len(got):
            fails.append(f"V1 length: frozen {len(expected)} statement lines, new {len(got)}")
    # every frozen line of a moved range lands in its own function
    for name, (_, ranges) in MOVED.items():
        own = {l for a, b in ranges for l in range(a, b + 1)} & set(flines)
        own -= set(DROP_LINES) | set(GATE_TEXT)
        if len(own) != sum(1 for l, _ in fns[name].body) - len(TAILS.get(name, [])) - \
                (EMIT_FWD[1] - EMIT_FWD[0] + 1 if name in EMIT_IN else 0):
            fails.append(f"V1 {name}: {len(own)} frozen statement lines, body holds a different count")

    # ---- V2: brackets ----
    for name, fn in fns.items():
        T = fn.T
        for k in range(fn.open, fn.close + 1):
            if T.k(k) == "op" and T.t(k) in "{}()[]" and not (fn.open <= T.match[k] <= fn.close):
                fails.append(f"V2 {name}: unmatched {T.t(k)!r} at line {T.toks[k].line}")
                break
        b0 = next((k for k in range(fn.open, fn.close) if T.toks[k].line > fn.mark), fn.close)
        for k in range(b0, fn.close):
            if T.k(k) == "op" and T.t(k) in "{}()[]" and not (b0 <= T.match[k] < fn.close):
                fails.append(f"V2 {name}: body bracket {T.t(k)!r} at line {T.toks[k].line} leaves the body")
                break

    # ---- V3: bindings ----
    for name, fn in fns.items():
        T = fn.T
        bound = {}
        for s, e in nc.statements(T, fn.open + 1, next(k for k in range(fn.open, fn.close)
                                                        if T.toks[k].line > fn.mark)):
            ds = nc.parse_declaration(T, s, e) or []
            text = " ".join(T.toks[k].text for k in range(s, e))
            for d in ds:
                if d.name in bound:
                    fails.append(f"V3 {name}: {d.name} bound twice")
                bound[d.name] = (text, d)
        body_ids = {T.toks[k].text for k in range(fn.open, fn.close)
                    if T.toks[k].line > fn.mark and T.k(k) == "id"}
        own = set(SHADOW.get(name, set()))
        for n in own:
            if not re.search(r"\b\w+ " + n + r" =", " ".join(x for _, x in fn.body)):
                fails.append(f"V3 {name}: declared shadow {n} is not declared in the body")
        moved_lines = {l for r in MOVED[name][1] for l in range(r[0], r[1] + 1)}
        own |= {n for n, v in scope.items() if v[0] in moved_lines}   # declared in the moved text
        frozen_names = (set(scope) | {"config_"}) - {"emitEvent"} - own
        if name == "networkFastForward":
            frozen_names.discard("se_raw")
        for n in sorted(body_ids & frozen_names):
            if n not in bound:
                fails.append(f"V3 {name}: {n} is read but not bound")
        for n, (text, d) in bound.items():
            root = text.split("=", 1)[1].strip().split(" ")[0] if "=" in text else ""
            if root not in ROOTS and root not in bound:
                fails.append(f"V3 {name}: {n}'s root {root!r} is neither the run, the epoch, an output nor a binding")
            elif root in bound and list(bound).index(root) > list(bound).index(n):
                fails.append(f"V3 {name}: {n} is bound before its root {root}")
            if n in pre and n not in EPOCH_BIND:
                if text != pre[n][1]:
                    fails.append(f"V3 {name}: {n} differs from the frozen preamble: {text!r}")
                continue
            want = EPOCH_BIND_FN.get((name, n), EPOCH_BIND.get(n))
            if want is None:
                fails.append(f"V3 {name}: {n} has no declared binding")
                continue
            if text != want:
                fails.append(f"V3 {name}: {n} is {text!r}, declared {want!r}")
            if n in scope and n not in ("sec_t0", "sec_epoch_t0", "config_"):
                ft = decl_type(FT, scope[n][2]) + (" &" if scope[n][2].is_ref else "")
                nt = decl_type(T, d) + (" &" if d.is_ref else "")
                if nt != ft and nt != "const " + ft:
                    fails.append(f"V3 {name}: {n} type {nt!r} vs frozen {ft!r}")
                if nt == "const " + ft and re.search(r"\b" + n + r" \. (insert|erase|clear|swap)\b|\b" + n + r" (=|\+=|-=)",
                                                     " ".join(x for _, x in fn.body)):
                    fails.append(f"V3 {name}: {n} gained const but the body writes it")
            if text.endswith("funnel . carry_slipped_ambs ;") and "const" not in text.split("&")[0] \
                    and "carry_slipped_ambs . insert" not in " ".join(x for _, x in fn.body):
                fails.append(f"V3 {name}: {n} bound writable but never written")

    # ---- V4: effects ----
    for name, (_, ranges) in MOVED.items():
        fr = []
        for a, b in ranges:
            for l in range(a, b + 1):
                if l in flines and l not in DROP_LINES:
                    s = flines[l]
                    if l in SUBS:
                        s = s.replace(*SUBS[l])
                    fr.extend(EFFECT.findall(s))
        body = fns[name].body
        if name in EMIT_IN:
            body = body[EMIT_FWD[1] - EMIT_FWD[0] + 1:]
        nw = []
        for _, s in body:
            nw.extend(EFFECT.findall(s))
        for t in TAILS.get(name, []):
            if nw and nw[-1] == t:
                nw.pop()
        if fr != nw:
            fails.append(f"V4 {name}: effects differ ({len(fr)} frozen, {len(nw)} new)")

    # ---- V5: gates ----
    driver_text = {s for _, s in code_lines(DT, DT.toks[do].line, DT.toks[dc].line)}
    for ln, gt in GATE_TEXT.items():
        if gt in driver_text:
            fails.append(f"V5 the frozen gate {gt!r} is still in the driver")
    for ln, (path, seam, gate) in GATES.items():
        try:
            g = Fn(files[path], T_of(path), seam)
        except Fail as e:
            fails.append(f"V5 {e}")
            continue
        if not g.body or g.body[0][1] != gate:
            fails.append(f"V5 {seam}: first statement {g.body[0][1] if g.body else None!r}, declared {gate!r}")
    g = Fn(files[ORACLE[0]], T_of(ORACLE[0]), ORACLE[1])
    if [x for _, x in g.body[:2]] != ORACLE[2]:
        fails.append(f"V5 {ORACLE[1]}: its own null-pin gate is not its first statement")
    return fails


def tree_files():
    paths = {DRIVER, ORACLE[0]} | {p for p, _ in MOVED.values()} | {g[0] for g in GATES.values()}
    return {p: read(p) for p in paths}


def mutate(files, k):
    f = dict(files)
    def sub(path, old, new, count=1):
        assert old in f[path], (k, path, old)
        f[path] = f[path].replace(old, new, count)
    if k == 1:   # a swapped pair of lines in the anchor election
        p = "src/core/filter/NetworkPhaseAnchorEpoch.cpp"
        L = f[p].split("\n")
        i = next(n for n, l in enumerate(L) if "cand_usable.on           = true;" in l)
        L[i], L[i + 1] = L[i + 1], L[i]
        f[p] = "\n".join(L)
    elif k == 2:   # a dropped line in the gap-resume census
        sub("src/core/slip/NetworkGapResumeCensus.cpp", "                    else         ++slip_qc.counters.gap_clean_fresh;\n", "")
    elif k == 3:   # a const added to a written binding
        sub("src/core/filter/NetworkPhaseAnchorEpoch.cpp", "    std::set<std::string>& shared_phase_keys = pass_a.shared_phase_keys;",
            "    const std::set<std::string>& shared_phase_keys = pass_a.shared_phase_keys;")
    elif k == 4:   # a reordered log call in the timer readout: the D0b line moved ahead of D0
        p = "src/core/app/NetworkRun.cpp"
        L = f[p].split("\n")
        a = next(n for n, l in enumerate(L) if 'log->info("Network (D0) TOW' in l)
        b = next(n for n, l in enumerate(L) if 'log->info("Network (D0b) TOW' in l)
        L[a] = L[a].replace('log->info("Network (D0) TOW', 'log->info("Network (D0b) TOW')
        L[b] = L[b].replace('log->info("Network (D0b) TOW', 'log->debug("Network (D0) TOW')
        f[p] = "\n".join(L)
    elif k == 5:   # a gate left out of its seam
        sub("src/core/filter/NetworkMdb.cpp", "    if (!ctx.knobs.mdb_on) return;", "")
    return f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="7141240")
    ap.add_argument("--controls", action="store_true")
    a = ap.parse_args()
    files = tree_files()
    ok = True
    if a.controls:
        for k in range(1, 6):
            fl = check(a.base, mutate(files, k))
            print(f"control {k}: {'FAILS as required' if fl else 'PASSES (verifier defect)'}"
                  + (f" [{fl[0][:110]}]" if fl else ""))
            ok &= bool(fl)
    fl = check(a.base, files)
    for x in fl:
        print("FAIL", x)
    print(f"V1..V5 over {len(MOVED)} moved functions and {len(GATES) + 1} gates: "
          + ("PASS" if not fl else f"{len(fl)} failure(s)"))
    sys.exit(0 if ok and not fl else 1)


if __name__ == "__main__":
    main()

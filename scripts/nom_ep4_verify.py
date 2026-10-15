#!/usr/bin/env python3
"""Card NOM3 step EP4: the relocation verifier (a reduced gate G-R, authorized for this step).

    python scripts/nom_ep4_verify.py [--base 8f4f1db] [--controls]

Checks the cut of Application::runNetwork into setup builders against the frozen driver of the
base, independently of the generator (scripts/nom_ep4_cut.py: this file holds its own manifest
and imports nothing from it).  Pre-registration: scripts/nom3_designs/EP4_design.md, section 4.

  V1 coverage and order: the new driver, each builder call expanded into the builder's body
     (the lines below `// ---- End of the bindings ----`), reproduces the frozen function's
     statement lines exactly once each, in order, up to the closed list of declared drops
     (DROP_LINES, BIND_LINES, UNWRAP); each body holds exactly its own range's lines.
  V2 brace balance: every builder and every moved body alone is bracket-balanced.
  V3 bindings: every frozen-scope name (and the Application members) a body reads is bound
     once, by the frozen declarator verbatim (VERBATIM), by its declared text (DECLARED), or by
     a declared parameter (PARAMS); a leading const only where declared (CONST_OK); every
     binding's root is the run, a parameter or an earlier binding.
  V4 effects: per builder, the ordered logger calls, `traces.` member calls, throws and
     returns equal the frozen range's.
  V5 declarations: each builder is declared once in its header, preceded by a Doxygen block
     with @brief and one @param per parameter.

With --controls, seven mutations are checked to FAIL first.  Exit 0 when every check passes.
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

DRIVER = "src/core/app/ApplicationNetwork.cpp"
END_MARK = "// ---- End of the bindings ----"

MOVED = {   # name -> (definition file, declaration header, frozen range)
    "networkMarkerArpCensusCopy": ("src/core/app/ApplicationNetworkSetup.cpp", "include/app/NetworkSetupResult.hpp", (254, 264)),
    "networkSlipQcConfigure": ("src/core/slip/NetworkSlipQc.cpp", "include/slip/NetworkSlipQc.hpp", (280, 344)),
    "networkSlipOracleArm": ("src/core/slip/NetworkSlipOracle.cpp", "include/slip/NetworkSlipOracle.hpp", (345, 357)),
    "networkOraclePinArm": ("src/core/filter/NetworkOraclePinSetup.cpp", "include/filter/NetworkOraclePinSetup.hpp", (358, 375)),
    "networkSlipQcArmWavelengthProvider": ("src/core/slip/NetworkSlipQc.cpp", "include/slip/NetworkSlipQc.hpp", (377, 384)),
    "networkCarryVceApplyArm": ("src/core/filter/NetworkCarryVceApply.cpp", "include/filter/NetworkCarryVceApply.hpp", (389, 403)),
    "networkPriorResidualArm": ("src/core/filter/NetworkPriorResidual.cpp", "include/filter/NetworkPriorResidual.hpp", (404, 482)),
    "networkBlockBaardaArm": ("src/core/filter/NetworkBlockBaarda.cpp", "include/filter/NetworkBlockBaarda.hpp", (483, 524)),
    "networkCongruenceArm": ("src/core/filter/NetworkCongruenceSetup.cpp", "include/filter/NetworkCongruenceSetup.hpp", (525, 533)),
    "networkRunManifestStartup": ("src/core/logging/NetworkRunTraces.cpp", "include/logging/NetworkRunTraces.hpp", (536, 548)),
    "networkEstimatorConfigure": ("src/core/filter/NetworkEstimator.cpp", "include/filter/NetworkEstimator.hpp", (595, 646)),
    "networkSectionTimersArm": ("src/core/app/NetworkRun.cpp", "include/app/NetworkRun.hpp", (648, 668)),
    "networkRunArm": ("src/core/app/NetworkRun.cpp", "include/app/NetworkRun.hpp", (672, 717)),
    "networkCbAdmissionCensus": ("src/core/filter/NetworkBiasAdmission.cpp", "include/filter/NetworkBiasAdmission.hpp", (719, 808)),
}
UNWRAP = {286, 344, 367, 375}                       # D4: outer braces of a whole-body block
BIND_LINES = {285, 411}                             # D1/D5: frozen declarators inside a range
DROP_LINES = {237, 239, 240, 241, 242, 279, 534, 558, 559, 573}   # D5
LM = "LogManager & log_manager_"
PARAMS = {   # D2: the declared parameter lists (normalized tokens)
    "networkMarkerArpCensusCopy": ["NetworkRunContext & ctx", "const GnssConfig & config_",
                                   "const NetworkSetupResult & setup"],
    "networkSlipOracleArm": ["NetworkRun & run", LM], "networkOraclePinArm": ["NetworkRun & run", LM],
    "networkSlipQcArmWavelengthProvider": ["NetworkRun & run", LM], "networkRunArm": ["NetworkRun & run", LM],
    "networkRunManifestStartup": ["NetworkRun & run", "const std :: filesystem :: path & config_path_"],
    "networkEstimatorConfigure": ["NetworkRun & run", "const NetworkEngineSpec & engine_spec"],
}
VERBATIM = {"slip_qc", "prires_est", "carry_driver", "emit_engines", "est", "slip_raw_trace", "synced",
            "start_offset_s", "carry_cfg"}
DECLARED = {   # D1: name -> (binding text, the frozen type relation)
    "ctx": ("NetworkRunContext & ctx = run . ctx ;", "object->ref"),
    "traces": ("NetworkRunTraces & traces = run . traces ;", "object->ref"),
    "config_": ("const GnssConfig & config_ = run . config ;", "member"),
    "log_dir_": ("const std :: filesystem :: path & log_dir_ = run . log_dir ;", "member"),
    "nc": ("const NetworkConfig & nc = run . nc ;", "auto"),
    "log": ("const std :: shared_ptr < spdlog :: logger > & log = run . log ;", "auto"),
    "stations": ("std :: vector < NetStation > & stations = run . setup . stations ;", "auto"),
    "sync": ("MultiRinexSync & sync = * run . setup . sync ;", "same"),
    "n_threads": ("const int n_threads = run . setup . n_threads ;", "same"),
    "seam_threads": ("const int seam_threads = run . setup . seam_threads ;", "same"),
    "carry_par": ("const bool carry_par = run . setup . carry_par ;", "same"),
    "emitter": ("NetworkProductEmitter & emitter = * run . setup . emitter ;", "same"),
}
CONST_OK = {("networkPriorResidualArm", "slip_qc"), ("networkBlockBaardaArm", "slip_qc")}   # D3
APP_MEMBERS = {"config_", "log_dir_", "log_manager_", "config_path_"}
EFFECT = re.compile(r"\blog -> \w+|\btraces \. \w+ \(|\bthrow\b|\breturn\b[^;]*;")


class Fail(Exception):
    pass


def git_show(base, path):
    return subprocess.run(["git", "-C", ROOT, "show", f"{base}:{path}"], capture_output=True,
                          check=True).stdout.decode("utf-8").replace("\r\n", "\n")


def read(path):
    with open(os.path.join(ROOT, path), "rb") as f:
        return f.read().decode("utf-8").replace("\r\n", "\n")


def code_lines(T, l0, l1):
    by = {}
    for t in T.toks:
        if l0 <= t.line <= l1:
            by.setdefault(t.line, []).append(t.text)
    return [(ln, " ".join(by[ln])) for ln in sorted(by)]


def find_def(T, name):
    hits = []
    for i, t in enumerate(T.toks):
        if t.text == name and T.t(i + 1) == "(" and T.k(i) == "id":
            j = T.match[i + 1]
            if j > 0 and T.t(j + 1) == "{" and T.t(i - 1) not in (".", "->", "return", "=", "(", ","):
                hits.append((i, j + 1, T.match[j + 1]))
    if len(hits) != 1:
        raise Fail(f"{name}: {len(hits)} definitions")
    return hits[0]


class Fn:
    def __init__(self, text, T, name):
        self.name = name
        self.at, o, c = find_def(T, name)
        self.T, self.open, self.close = T, o, c
        lo, lc = T.toks[o].line, T.toks[c].line
        src = text.split("\n")
        marks = [n for n in range(lo, lc + 1) if src[n - 1].strip() == END_MARK]
        if len(marks) != 1:
            raise Fail(f"{name}: {len(marks)} binding end markers")
        self.mark = marks[0]
        self.body = [x for x in code_lines(T, self.mark + 1, lc) if x[0] < lc or x[1] != "}"]
        self.params = [p.strip() for p in " ".join(T.toks[k].text for k in range(self.at + 2, o - 1)).split(",")
                       if p.strip()]


def frozen_scope(FT, fo, fc):
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
    return " ".join(T.toks[k].text for k in range(a, b)) + (" &" if d.is_ref else "")


def check(base, files):
    fails = []
    F = git_show(base, DRIVER)
    FT = nc.Tokens(F, "frozen")
    _, fo, fc = find_def(FT, "runNetwork")
    flines = dict(code_lines(FT, FT.toks[fo].line + 1, FT.toks[fc].line - 1))
    scope = frozen_scope(FT, fo, fc)
    toks = {}

    def T_of(path):
        if path not in toks:
            toks[path] = nc.Tokens(files[path], path)
        return toks[path]

    fns = {}
    for name, (path, _, _) in MOVED.items():
        try:
            fns[name] = Fn(files[path], T_of(path), name)
        except Fail as e:
            fails.append(f"V2 {e}")
    if fails:
        return fails

    # ---- V1 ----
    expected = [(ln, s) for ln, s in flines.items() if ln not in DROP_LINES | BIND_LINES | UNWRAP]
    DT = T_of(DRIVER)
    _, do, dc = find_def(DT, "runNetwork")
    got = []
    for ln, s in code_lines(DT, DT.toks[do].line + 1, DT.toks[dc].line - 1):
        called = [n for n in MOVED if re.search(r"\b" + n + r" \(", s)]
        if not called:
            got.append((f"driver:{ln}", s))
            continue
        if len(called) != 1 or not re.fullmatch(called[0] + r" \([^;]*\) ;", s):
            fails.append(f"V1 driver:{ln}: a builder call is not alone on its line: {s!r}")
        got.extend((f"{called[0]}:{l}", x) for l, x in fns[called[0]].body)
    for e, g in zip(expected, got):
        if e[1] != g[1]:
            fails.append(f"V1 first difference: frozen {e[0]} {e[1][:90]!r} vs {g[0]} {g[1][:90]!r}")
            break
    else:
        if len(expected) != len(got):
            fails.append(f"V1 length: frozen {len(expected)} statement lines, new {len(got)}")
    for name, (_, _, (a, b)) in MOVED.items():
        own = {l for l in range(a, b + 1) if l in flines} - BIND_LINES - UNWRAP
        if len(own) != len(fns[name].body):
            fails.append(f"V1 {name}: {len(own)} frozen statement lines, body holds {len(fns[name].body)}")

    # ---- V2 ----
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

    # ---- V3 ----
    for name, fn in fns.items():
        T = fn.T
        want_params = PARAMS.get(name, ["NetworkRun & run"])
        if fn.params != want_params:
            fails.append(f"V3 {name}: parameters {fn.params} vs declared {want_params}")
        pnames = {p.split()[-1] for p in fn.params}
        bound = {}
        b_end = next(k for k in range(fn.open, fn.close + 1) if T.toks[k].line > fn.mark)
        for s, e in nc.statements(T, fn.open + 1, b_end):
            text = " ".join(T.toks[k].text for k in range(s, e))
            for d in nc.parse_declaration(T, s, e) or []:
                if d.name in bound or d.name in pnames:
                    fails.append(f"V3 {name}: {d.name} bound twice")
                bound[d.name] = (text, d)
        a, b = MOVED[name][2]
        own = {n for n, v in scope.items() if a <= v[0] <= b and v[0] not in BIND_LINES}
        body_ids = {T.toks[k].text for k in range(fn.open, fn.close)
                    if T.toks[k].line > fn.mark and T.k(k) == "id" and T.t(k - 1) not in (".", "->", "::")}
        need = (body_ids & (set(scope) | APP_MEMBERS)) - own
        for n in sorted(need):
            if n not in bound and n not in pnames:
                fails.append(f"V3 {name}: {n} is read but not bound")
        order = list(bound)
        for n, (text, d) in bound.items():
            rhs = text.split("=", 1)[1].strip().split(" ") if "=" in text else [""]
            root = rhs[1] if rhs[0] == "*" else rhs[0]
            if root != "run" and root not in pnames and root not in bound:
                fails.append(f"V3 {name}: {n}'s root {root!r} is neither the run, a parameter nor a binding")
            elif root in bound and order.index(root) > order.index(n):
                fails.append(f"V3 {name}: {n} is bound before its root {root}")
            if n in VERBATIM:
                ftext = scope[n][1]
                if text == ftext:
                    continue
                if text == "const " + ftext and (name, n) in CONST_OK:
                    continue
                fails.append(f"V3 {name}: {n} is {text!r}, frozen {ftext!r}")
                continue
            if n not in DECLARED:
                fails.append(f"V3 {name}: {n} has no declared binding")
                continue
            want, rel = DECLARED[n]
            if text != want:
                fails.append(f"V3 {name}: {n} is {text!r}, declared {want!r}")
            if n in scope:
                ft, nt = decl_type(FT, scope[n][2]), decl_type(T, d)
                ok = {"same": nt in (ft, "const " + ft), "object->ref": nt == ft + " &",
                      "auto": "auto" in ft.split()}.get(rel, False)
                if not ok:
                    fails.append(f"V3 {name}: {n} type {nt!r} vs frozen {ft!r} ({rel})")

    # ---- V4 ----
    for name, (_, _, (a, b)) in MOVED.items():
        fr = []
        for l in range(a, b + 1):
            if l in flines and l not in BIND_LINES | UNWRAP:
                fr.extend(EFFECT.findall(flines[l]))
        nw = []
        for _, s in fns[name].body:
            nw.extend(EFFECT.findall(s))
        if fr != nw:
            fails.append(f"V4 {name}: effects differ ({len(fr)} frozen, {len(nw)} new)")

    # ---- V5 ----
    for name, (_, hpp, _) in MOVED.items():
        L = files[hpp].split("\n")
        hits = [i for i, l in enumerate(L) if re.match(r"^void " + name + r"\(.*\);\s*$", l)]
        if len(hits) != 1:
            fails.append(f"V5 {name}: {len(hits)} declarations in {hpp}")
            continue
        i = hits[0]
        if L[i - 1].strip() != "*/":
            fails.append(f"V5 {name}: no Doxygen block above the declaration")
            continue
        j = max(k for k in range(i) if L[k].strip() == "/**")
        doc = "\n".join(L[j:i])
        if "@brief" not in doc:
            fails.append(f"V5 {name}: no @brief")
        for p in PARAMS.get(name, ["NetworkRun & run"]):
            if not re.search(r"@param " + p.split()[-1] + r"\b", doc):
                fails.append(f"V5 {name}: no @param {p.split()[-1]}")
    return fails


def tree_files():
    paths = {DRIVER} | {p for p, _, _ in MOVED.values()} | {h for _, h, _ in MOVED.values()}
    return {p: read(p) for p in paths}


def mutate(files, k):
    f = dict(files)

    def sub(path, old, new):
        assert old in f[path], (k, path, old)
        f[path] = f[path].replace(old, new, 1)

    def swap(path, a, b):
        L = f[path].split("\n")
        i = next(n for n, l in enumerate(L) if a in l)
        j = next(n for n, l in enumerate(L) if b in l)
        L[i], L[j] = L[j], L[i]
        f[path] = "\n".join(L)
    if k == 1:     # a swapped pair of lines in the slip-QC parameters
        swap("src/core/slip/NetworkSlipQc.cpp", "sp.k_mw                = sq.lli_k_mw;", "sp.k_gf                = sq.lli_k_gf;")
    elif k == 2:   # a dropped line in the admission census
        sub("src/core/filter/NetworkBiasAdmission.cpp", "                ++t.seen;\n", "")
    elif k == 3:   # a const added to a written binding
        sub("src/core/slip/NetworkSlipQc.cpp", "    SlipQcChannel& slip_qc = run.engines.slip_qc;\n    // ---- End",
            "    const SlipQcChannel& slip_qc = run.engines.slip_qc;\n    // ---- End")
    elif k == 4:   # a reordered log call in the admission census
        swap("src/core/filter/NetworkBiasAdmission.cpp", 'log->warn("Network (RTF-3): network.cb_admission census SKIPPED',
             'log->info("Network (CB-ADM): census over')
    elif k == 5:   # two builder calls swapped in the driver
        swap(DRIVER, "    networkPriorResidualArm(run);", "    networkBlockBaardaArm(run);")
    elif k == 6:   # a missing @param in a header
        sub("include/app/NetworkRun.hpp", " * @param log_manager_ ", " * log_manager_ ")
    elif k == 7:   # a binding to the wrong member
        sub("src/core/filter/NetworkEstimator.cpp", "const int seam_threads = run.setup.seam_threads;",
            "const int seam_threads = run.setup.n_threads;")
    return f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="8f4f1db")
    ap.add_argument("--controls", action="store_true")
    a = ap.parse_args()
    files = tree_files()
    ok = True
    if a.controls:
        for k in range(1, 8):
            fl = check(a.base, mutate(files, k))
            print(f"control {k}: {'FAILS as required' if fl else 'PASSES (verifier defect)'}"
                  + (f" [{fl[0][:110]}]" if fl else ""))
            ok &= bool(fl)
    fl = check(a.base, files)
    for x in fl:
        print("FAIL", x)
    print(f"V1..V5 over {len(MOVED)} builders: " + ("PASS" if not fl else f"{len(fl)} failure(s)"))
    sys.exit(0 if ok and not fl else 1)


if __name__ == "__main__":
    main()

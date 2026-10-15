#!/usr/bin/env python3
"""Relocation verifier of card NOM3 step SG (gate G-R; design section 4.2).

    python scripts/nom_sg_verify.py [--base 7141240] [--controls] [--verbose]

The frozen source is networkSolveGlue at the base (git show); the candidate is the working
tree.  The table is scripts/nom_sg/rows.py.  Exit 0 only when every built check passes.

Checks built (the design's numbering):
  S1  Coverage.  Every frozen code line of networkSolveGlue (comments and blank lines ignored)
      lies in exactly one piece of one new function or one declared range, and every piece,
      generated line, substitution and declared range names a class of the closed list
      (scripts/nom_sg/declared.py).
  S1b Block forms.  Per function, the running brace depth over its pieces starts at 0, never
      goes below 0 and ends at 0; each piece's net and minimum match its declared form; the
      first line of each generated block sits at the depth the earlier piece left.
  S2  Bodies.  Each function in its home file's generated region, below its preamble, equals
      its pieces (frozen lines, the declared substitutions applied, re-indented) and generated
      lines, then its tail, line for line; each region holds only the table's functions.
  S3  Bindings.  Each preamble statement is a frozen binding statement verbatim or a declared
      binding of the function's row; every solve-scope name the body reads is bound by the
      preamble, a parameter, or a declaration in its own pieces or generated lines; each
      parameter's declarator equals the row's, and a frozen reference or object stays a
      reference (a value only where the frozen name was a const scalar).
  S5  Order.  The orchestrator's stage calls, in the file's order, equal the order the frozen
      pieces imply (all pieces of a stage precede the next stage's, R17's DECL_MOVE aside) and
      the header banner table's.
  S6  Exits.  Per exit, the facts of the frozen exit (disposition literal, trace row and flag,
      status columns, H line and state, tail) equal its kSolveSkips row; the enumerators are
      in row order; each exit is taken after the stage whose pieces held the frozen exit;
      networkSolveSkip writes its effects in the frozen order; SolveSkipFacts::of stores the
      orchestrator's own addresses and leaves sigma0_adapt at 0.0 before T2 (VALUE_EQUAL).
  S7  Homes.  The three run-end reports and networkClockDatumDeclaration are byte-identical
      at their new homes and defined once; the run-end table names the new homes.
  S8  Documentation.  Each new public function's declaration has @brief, one @param per
      parameter and @return when it returns a value.
  S10 The replay chain.  The orchestrator passes ws, est_index_of, dd_build and funnel to
      networkWithinEpochAdapt unchanged, and its body calls
      networkBuildCarryDdPriors(run, ep, dd_build, funnel) exactly once.
  S11 Availability and one access path.  No new function spells ep.ws,
      ep.assembly.est_index_of, ep.dd_build, ep.funnel, ep.solve, ep.ar_fixes,
      ep.fixed_cycles, ep.prep or ep.emitted.
  S12 Stateless.  NetworkSbasis has no non-static data member and buildSharedDatum,
      completeSbasisDimension and assertNullSpaceRank are const.
  S13 Stray edits (file level).  Every file the diff against the base touches is in the
      declared set.
  S14 The knob.  NetworkKnobs::retro_feed's initializer is the frozen initializer with each
      alias replaced by its knob field, and the snapshot fingerprint does not list it.
Not built (cost; recorded in the card): S4 as a separate check (the realization lines are
checked by S2 and S3 as declared text), S9 (EP1's overload capture scan), S13 at hunk level.
"""
import argparse
import copy
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from nom_sg import stages as S    # noqa: E402
from nom_sg import rows as R      # noqa: E402
from nom_sg.declared import CLASSES  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
CRLF, LF = chr(13) + chr(10), chr(10)

STAGE_ORDER_HDR = "include/filter/NetworkSolveGlue.hpp"
STAGES = ["networkCensusSigma0", "networkCodeBiasDatum", "networkClockDatum", "networkPreGatePriors",
          "networkEstimabilityDump", "networkKaulaMargin", "networkSbasisCompletion", "networkRankGate",
          "networkClockModelWarm", "networkPostGatePriors", "networkPrimarySolve",
          "networkClockAnchorGuardEpoch", "networkWithinEpochAdapt", "networkSigma0Ceiling"]
DECL_MOVE_ROWS = {"R17"}
EXIT_OF_STAGE = {"networkClockDatum": "DatumFailed", "networkRankGate": "GateWithheld",
                 "networkPrimarySolve": "SolveFailed", "networkWithinEpochAdapt": "WeRestoreFailed",
                 "networkSigma0Ceiling": "Sigma0Withheld"}
EXIT_ROWS = {"DatumFailed": ["R12x"], "GateWithheld": ["R21"], "SolveFailed": ["R27x"],
             "WeRestoreFailed": ["R30x"], "Sigma0Withheld": ["R31x"]}
MOVED = [("src/core/filter/NetworkEstimator.cpp", "src/core/filter/NetworkRankGate.cpp", "networkRankGateRunEnd"),
         ("src/core/filter/NetworkRaim.cpp", "src/core/filter/NetworkRaimWithinEpoch.cpp",
          "networkWithinEpochRaimRunEnd"),
         ("src/core/filter/NetworkSbasis.cpp", "src/core/filter/NetworkSbasisCompletion.cpp",
          "networkSbasisCompletionRunEnd"),
         ("src/core/filter/NetworkSolveGlue.cpp", "src/core/filter/NetworkClockDatum.cpp",
          "networkClockDatumDeclaration")]
FORBIDDEN_EP = ["ep.ws", "ep.assembly.est_index_of", "ep.dd_build", "ep.funnel", "ep.solve", "ep.ar_fixes",
                "ep.fixed_cycles", "ep.prep", "ep.emitted"]
SCALARS = {"bool", "int", "double", "long long", "std::size_t", "size_t"}
DECLARED_FILES = {
    "CMakeLists.txt", "include/app/NetworkRunContext.hpp", "src/core/app/NetworkKnobs.cpp",
    "include/filter/NetworkSolveGlue.hpp", "src/core/filter/NetworkSolveGlue.cpp",
    "include/filter/NetworkCodeBiasDatum.hpp", "src/core/filter/NetworkCodeBiasDatum.cpp",
    "include/filter/NetworkClockDatum.hpp", "src/core/filter/NetworkClockDatum.cpp",
    "include/filter/NetworkRankGate.hpp", "src/core/filter/NetworkRankGate.cpp",
    "include/filter/NetworkSbasisCompletion.hpp", "src/core/filter/NetworkSbasisCompletion.cpp",
    "include/filter/NetworkRaimWithinEpoch.hpp", "src/core/filter/NetworkRaimWithinEpoch.cpp",
    "include/filter/NetworkPrefitCensus.hpp", "src/core/filter/NetworkPrefitCensus.cpp",
    "include/filter/NetworkBiasAdmission.hpp", "src/core/filter/NetworkBiasAdmission.cpp",
    "include/filter/NetworkClockAnchor.hpp", "src/core/filter/NetworkClockAnchor.cpp",
    "include/filter/NetworkDatumGraph.hpp", "src/core/filter/NetworkDatumGraph.cpp",
    "include/filter/NetworkEstimator.hpp", "src/core/filter/NetworkEstimator.cpp",
    "include/filter/NetworkRaim.hpp", "src/core/filter/NetworkRaim.cpp",
    "include/filter/NetworkSbasis.hpp", "src/core/filter/NetworkSbasis.cpp",
    "src/core/filter/NetworkRunEnd.cpp", "src/core/filter/NetworkHandoverStransform.cpp",
    "tests/test_network_run_end.cpp", "tests/test_network_solve_skip.cpp",
    "tests/test_network_knobs_epoch_driver.cpp",
    "scripts/nom_sg_cut.py", "scripts/nom_sg_verify.py", "scripts/nom_sg/__init__.py",
    "scripts/nom_sg/stages.py", "scripts/nom_sg/rows.py", "scripts/nom_sg/declared.py",
    "scripts/nom_td_cut.py", "scripts/nom_td_manifest.json", "scripts/nom_epoch_lint.py",
    # SG2 and SG3 (declared now so one list serves the three commits)
    "tests/test_network_we_choice.cpp", "include/filter/NetworkRhsAttrib.hpp",
    "src/core/filter/NetworkRhsAttrib.cpp", "src/core/filter/NetworkContributions.cpp",
    "src/core/filter/NetworkResponses.cpp", "CLAUDE.md", "docs/INDEX.md",
    "docs/refound/NOM3_SG_solve_orchestrator_2026-10-14.md", "src/core/app/ApplicationNetworkEpoch.cpp",
    "scripts/nom_sg/rows_sg2.py",
}


class Tree:
    """The candidate tree, with an in-memory overlay for the negative controls."""

    def __init__(self, root=ROOT):
        self.root, self.overlay = root, {}

    def read(self, path):
        if path in self.overlay:
            return self.overlay[path]
        p = self.root / path
        # A checkout under core.autocrlf holds CRLF; git stores LF, and the checks read LF.
        return p.read_bytes().decode("utf-8").replace(CRLF, LF) if p.exists() else None


# --------------------------------------------------------------------------- helpers
def fn_text(tree, fn):
    """The function's lines in its home file (from its signature to the closing `}`)."""
    text = tree.read(fn.home)
    if text is None:
        return None
    lines = text.split("\n")
    hits = [i for i, l in enumerate(lines) if l == fn.sig[0]]
    if len(hits) != 1:
        return None
    i = hits[0]
    j = i
    while j < len(lines) and lines[j] != "}":
        j += 1
    return lines[i:j + 1]


def split_preamble(lines, fn):
    """(signature, preamble lines, body lines) of a function's text."""
    n = len(fn.sig)
    sig, rest = lines[:n], lines[n:]
    pre_open = S.PRE_OPEN.split("\n")
    if rest[:len(pre_open)] == pre_open:
        k = rest.index(S.PRE_CLOSE)
        return sig, rest[len(pre_open):k], rest[k + 1:]
    return sig, [], rest


def statements(lines):
    """Group preamble lines into statements (a brace- and paren-balanced run ending in `;`)."""
    out, cur = [], []
    for l in lines:
        cur.append(l)
        code = "".join(S.code_lines(cur))
        depth = code.count("{") - code.count("}") + code.count("(") - code.count(")")
        if code.rstrip().endswith(";") and depth == 0:
            out.append(cur)
            cur = []
    if cur:
        out.append(cur)
    return out


def decl_name(stmt_lines):
    code = "".join(S.code_lines(stmt_lines))
    head = code.split("=", 1)[0] if "=" in code else code.rstrip("; ")
    ids = S.IDENT.findall(head)
    return ids[-1] if ids else None


def params_of(sig_lines):
    text = " ".join(l.strip() for l in sig_lines)
    inner = text[text.index("(") + 1:text.rindex(")")]
    parts, depth, cur = [], 0, ""
    for ch in inner:   # split at the top-level commas (template arguments hold commas)
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    parts.append(cur)
    out = {}
    for p in [p.strip() for p in parts if p.strip()]:
        name = S.IDENT.findall(p)[-1]
        out[name] = p
    return out


def brace_net_min(lines):
    depth, low = 0, 0
    for l in S.code_lines(lines):
        for ch in l:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                low = min(low, depth)
    return depth, low


class Report:
    def __init__(self, verbose=False):
        self.res, self.verbose = {}, verbose

    def check(self, key, ok, msg):
        self.res.setdefault(key, []).append((ok, msg))

    def failed(self, key):
        return any(not ok for ok, _ in self.res.get(key, []))

    def summary(self, keys):
        bad = 0
        for k in keys:
            items = self.res.get(k, [])
            fails = [m for ok, m in items if not ok]
            print(f"  [{'FAIL' if fails else 'pass'}] {k}: {len(items) - len(fails)}/{len(items)}")
            for m in fails[:8]:
                print(f"         - {m}")
            bad += bool(fails)
        return bad


# --------------------------------------------------------------------------- the checks
def s1_coverage(fz, rows, rep):
    covered = {}
    for fn in rows.FUNCTIONS:
        for item in fn.body:
            if isinstance(item, S.P):
                a, b = S.resolve(fz, item)
                for n in range(a, b + 1):
                    covered.setdefault(n, []).append(fn.name)
                for sub in item.subs:
                    rep.check("S1", sub.cls in CLASSES, f"{fn.name}: substitution class {sub.cls}")
                    if sub.cls == "COMMENT_EDIT":
                        same = [l.strip() for l in S.code_lines(sub.old) if l.strip()] ==                                [l.strip() for l in S.code_lines(sub.new) if l.strip()]
                        rep.check("S1", same, f"{fn.name}: COMMENT_EDIT changes code")
            else:
                rep.check("S1", item.cls in CLASSES, f"{fn.name}: generated class {item.cls}")
    for d in rows.DROPS:
        rep.check("S1", d.cls in CLASSES, f"declared range class {d.cls}")
        a = fz.find(d.first)
        b = a if d.last is None else (fz.find(d.last) if isinstance(d.last, tuple) else fz.find(d.last, a))
        for n in range(a, b + 1):
            covered.setdefault(n, []).append(d.cls)
    for n in range(fz.first, fz.last + 1):
        if not fz.code[n - 1].strip():
            continue
        c = covered.get(n, [])
        rep.check("S1", len(c) == 1, f"frozen line {n} carried {len(c)} times: {fz.line(n).strip()[:60]!r}")


def s1b_forms(fz, rows, rep):
    for fn in rows.FUNCTIONS:
        if fn.kind == "comment":
            continue
        depth = 0
        for item in fn.body:
            if isinstance(item, S.P):
                lines = S.render_piece(fz, item)
                net, low = brace_net_min(lines)
                want = {"whole": net == 0 and low >= 0, "open": net > 0 and low >= 0,
                        "close": net < 0, "mid": True}[item.form]
                rep.check("S1b", want, f"{fn.name} {item.row}: form {item.form} but net {net}, min {low}")
                rep.check("S1b", depth + low >= 0, f"{fn.name} {item.row}: depth below 0")
                depth += net
            else:
                first = next((l for l in item.lines if l.strip()), None)
                if first is not None:
                    ind = len(first) - len(first.lstrip(" "))
                    rep.check("S1b", ind == 4 * (depth + 1),
                              f"{fn.name}: generated {first.strip()[:40]!r} at indent {ind}, depth {depth}")
                net, _ = brace_net_min(item.lines)
                depth += net
        tail_net, _ = brace_net_min(fn.tail)
        rep.check("S1b", depth + tail_net == 0, f"{fn.name}: ends at depth {depth + tail_net}")


def s2_bodies(fz, rows, tree, rep):
    by_home = {}
    for fn in rows.FUNCTIONS:
        by_home.setdefault(fn.home, []).append(fn)
        if fn.kind == "comment":
            continue
        lines = fn_text(tree, fn)
        if lines is None:
            rep.check("S2", False, f"{fn.name}: not found once in {fn.home}")
            continue
        sig, _pre, body = split_preamble(lines, fn)
        expect = S.render_body(fz, fn) + fn.tail + ["}"]
        if body != expect:
            k = next((i for i, (x, y) in enumerate(zip(body, expect)) if x != y), min(len(body), len(expect)))
            got = body[k] if k < len(body) else "<end>"
            want = expect[k] if k < len(expect) else "<end>"
            rep.check("S2", False, f"{fn.name}: body line {k + 1}: {got.strip()[:50]!r} != {want.strip()[:50]!r}")
        else:
            rep.check("S2", True, fn.name)


def s2_regions(order, fz_of, docs, tree, rep):
    """Each region holds exactly the table's functions (SG1's, with T2 replaced by SG2's at stage
    sg2), each under its head comment, separated by one blank line."""
    by_home = {}
    for fn in order:
        by_home.setdefault(fn.home, []).append(fn)
    for home, fns in by_home.items():
        lines = (tree.read(home) or "").split(LF)
        try:
            a, b = lines.index(S.REGION_OPEN), lines.index(S.REGION_CLOSE)
        except ValueError:
            rep.check("S2", False, f"{home}: region markers missing")
            continue
        expect = []
        for i, fn in enumerate(fns):
            if i:
                expect.append("")
            if fn.kind == "comment":
                expect.extend(S.render_body(fz_of[fn.name], fn))
            else:
                t = fn_text(tree, fn) or []
                head = docs.get(fn.name) or [f"/// {fn.brief} (card NOM3 step SG; Doxygen at the declaration)."]
                expect.extend(list(head) + t if t else [""])
        rep.check("S2", lines[a + 1:b] == expect, f"{home}: the region holds other text than the table's functions")


def frozen_decl(fz, canon_by, name, locals_map=None):
    """The frozen declaring text of a solve-scope (or T2-scope) name."""
    locals_map = locals_map or S.SOLVE_LOCALS
    if name in canon_by:
        return "".join(canon_by[name][1])
    if name in S.SOLVE_PARAMS:
        return S.SOLVE_PARAMS[name]
    if name in locals_map:
        try:
            return fz.line(fz.find(locals_map[name]))
        except AssertionError:
            return None
    return None


def is_reference_decl(text, name):
    m = re.search(r"&\s*" + re.escape(name) + r"\b", text)
    return bool(m)


def frozen_scalar_const(text, name):
    code = S.blank_code(text, [False])
    m = re.search(r"\bconst\s+(bool|int|double|long long|std::size_t|size_t)\s+" + re.escape(name) + r"\b", code)
    return bool(m)


def s3_bindings(fz, rows, tree, rep, canon, names, locals_map=None, globals_=()):
    locals_map = locals_map or S.SOLVE_LOCALS
    canon_by = {c[0]: c for c in canon}
    canon_texts = {tuple(c[1]) for c in canon}
    for fn in rows.FUNCTIONS:
        if fn.kind == "comment":
            continue
        lines = fn_text(tree, fn)
        if lines is None:
            continue
        sig, pre, body = split_preamble(lines, fn)
        params = params_of(sig)
        # parameter declarators and categories
        for pname, ptext in params.items():
            want = fn.params.get(pname)
            rep.check("S3", want is not None and " ".join(ptext.split()) == " ".join(want.split()),
                      f"{fn.name}: parameter {ptext!r} is not the row's {want!r}")
            fd = frozen_decl(fz, canon_by, pname, locals_map) if pname in names else None
            if fd is not None and not is_reference_decl(ptext, pname):
                # A copy is allowed only of a const scalar, or of a scalar the function never
                # writes (it runs synchronously, so its copy equals the frozen name throughout).
                scalar = re.search(r"\b(bool|int|double|long long|std::size_t|size_t)\s+" + re.escape(pname)
                                   + r"\b", S.blank_code(fd, [False]))
                code = LF.join(S.code_lines(body))
                written = re.search(r"(\+\+|--)\s*" + re.escape(pname) + r"\b|\b" + re.escape(pname)
                                    + r"\s*(\+\+|--|[-+*/|&]?=(?!=))", code)
                rep.check("S3", frozen_scalar_const(fd, pname) or (scalar and not written),
                          f"{fn.name}: {pname} passed by value, frozen {fd.strip()[:50]!r}")
        # preamble statements
        binds = {b[1]: b for b in fn.binds}   # by full text (a binding may span lines)
        bound = set()
        for st in statements(pre):
            nm = decl_name(st)
            if tuple(st) in canon_texts:
                bound.add(nm)
                continue
            line = LF.join(l[4:] if l.startswith("    ") else l for l in st)
            if line in binds:
                b = binds[line]
                if b[2] in ("REALIZE", "STATE"):
                    rep.check("S3", is_reference_decl(line, b[0]), f"{fn.name}: REALIZE {line!r} is not a reference")
                bound.add(b[0])
                continue
            rep.check("S3", False, f"{fn.name}: preamble statement not declared: {' '.join(s.strip() for s in st)[:70]!r}")
        # every solve-scope name read is bound
        read = S.idents_read("\n".join(S.code_lines(body)))
        own = set()
        for item in fn.body:
            if isinstance(item, S.P):
                a, b = S.resolve(fz, item)
                for nm, anchor in locals_map.items():
                    try:
                        if a <= fz.find(anchor) <= b:
                            own.add(nm)
                    except AssertionError:
                        pass
            else:
                for l in item.lines:
                    m = re.match(r"^\s+(?:const\s+)?[\w:<>]+[&*]?\s+(\w+)\s*(?:=|;)", l)
                    if m and not l.strip().startswith("return"):
                        own.add(m.group(1))
        for nm in sorted(read & names):
            rep.check("S3", nm in bound or nm in params or nm in own or nm in globals_,
                      f"{fn.name}: solve-scope name {nm!r} read but not bound")


def orchestrator_calls(tree):
    fn = R.fn_by_name("networkSolveGlue")
    lines = fn_text(tree, fn) or []
    code = S.code_lines(lines)
    calls = []
    for l in code:
        for m in re.finditer(r"\b(network\w+)\s*\(", l):
            if m.group(1) in STAGES:
                calls.append(m.group(1))
    return calls, lines


def s5_order(fz, rows, tree, rep):
    spans = {}
    for fn in rows.FUNCTIONS:
        if fn.name not in STAGES:
            continue
        for item in fn.body:
            if isinstance(item, S.P) and item.row not in DECL_MOVE_ROWS:
                a, b = S.resolve(fz, item)
                lo, hi = spans.get(fn.name, (a, b))
                spans[fn.name] = (min(lo, a), max(hi, b))
    for fn in rows.FUNCTIONS:   # a stage without frozen pieces of its own (none today)
        if fn.name in STAGES and fn.name not in spans:
            rep.check("S5", False, f"{fn.name}: no frozen piece")
    implied = sorted(spans, key=lambda n: spans[n][0])
    for x, y in zip(implied, implied[1:]):
        rep.check("S5", spans[x][1] < spans[y][0], f"{x} and {y} interleave in the frozen order")
    calls, _ = orchestrator_calls(tree)
    rep.check("S5", calls == implied, f"orchestrator calls {calls} != frozen order {implied}")
    hdr = tree.read(STAGE_ORDER_HDR) or ""
    banner = re.findall(r"^ \*\s+\d+\s+(network\w+)\s", hdr, re.M)
    rep.check("S5", banner == implied, f"banner table {banner} != frozen order {implied}")


def frozen_exit_facts(fz, rows):
    """Per exit: the facts of its frozen tail (declared ranges and the sigma0 tail's piece)."""
    facts = {}
    for skip, rws in EXIT_ROWS.items():
        text = []
        for d in rows.DROPS:
            if d.row in rws:
                a = fz.find(d.first)
                b = a if d.last is None else (fz.find(d.last) if isinstance(d.last, tuple) else fz.find(d.last, a))
                text.extend(fz.lines[a - 1:b])
        t = "\n".join(text)
        flag = re.search(r'<< ",(\d),0,0,0,"', t)
        st = re.search(r"State::(\w+)", t)
        facts[skip] = dict(
            disposition=re.search(r'emitEpochStatus\("(\w+)"', t).group(1),
            col_datum_changed="es.datum_changed" in t,
            col_t2_sigma0="es.sigma0 = sigma0_adapt" in t,
            col_t2_meters="es.n_we_refloat_sats" in t,
            trace_flag=int(flag.group(1)) if flag else -1,
            h_line=bool(st), h_state=st.group(1) if st else "Unknown",
            n_skipped=t.count("++n_skipped;"))
    tail = R.fn_by_name("networkSigma0WithholdTail")
    a, b = S.resolve(fz, tail.body[0])
    facts["Sigma0Withheld"]["tail"] = "networkSigma0WithholdTail"
    for k in facts:
        facts[k].setdefault("tail", "nullptr")
    return facts


ROW_RE = re.compile(r'\{SolveSkip::(\w+),\s*"(\w+)",\s*"(\w+)",\s*(true|false),\s*(true|false),\s*(true|false),'
                    r'\s*(-?\d+),\s*(true|false),\s*NetworkProducerStatus::State::(\w+),\s*(\w+)\}')


def s6_exits(fz, rows, tree, rep):
    facts = frozen_exit_facts(fz, rows)
    sg = tree.read("src/core/filter/NetworkSolveGlue.cpp") or ""
    found = ROW_RE.findall(S.blank_code(sg.replace("\n", " "), [False]).replace("  ", " ")) if False else \
        ROW_RE.findall(" ".join(sg.split()))
    hdr = tree.read(STAGE_ORDER_HDR) or ""
    enum = re.search(r"enum class SolveSkip[^{]*\{([^}]*)\}", hdr)
    enums = [e.strip() for e in enum.group(1).split(",")] if enum else []
    rep.check("S6", [f[0] for f in found] == enums and len(found) == 5,
              f"kSolveSkips rows {[f[0] for f in found]} vs enumerators {enums}")
    for f in found:
        skip = f[0]
        want = facts.get(skip)
        if want is None:
            rep.check("S6", False, f"row {skip} has no frozen exit")
            continue
        got = dict(disposition=f[1], col_datum_changed=f[3] == "true", col_t2_sigma0=f[4] == "true",
                   col_t2_meters=f[5] == "true", trace_flag=int(f[6]), h_line=f[7] == "true",
                   h_state=f[8], tail=f[9])
        for k, v in got.items():
            rep.check("S6", want[k] == v, f"{skip}.{k}: row {v!r}, frozen {want[k]!r}")
        rep.check("S6", want["n_skipped"] == 1,
                  f"{skip}: frozen tail counts n_skipped {want['n_skipped']} times")
    # each exit is taken right after its stage
    _, lines = orchestrator_calls(tree)
    joined = " ".join(" ".join(lines).split())
    for stage, skip in EXIT_OF_STAGE.items():
        rep.check("S6", re.search(re.escape(stage) + r"\([^;]*\)[^;]*== EpochAction::SkipEpoch\) return "
                                  r"networkSolveSkip\(run, ep, SolveSkip::" + skip + r",", joined) is not None,
                  f"exit {skip} is not taken after {stage}")
    # networkSolveSkip's effects in the frozen order, and the heartbeat's sigma0 (VALUE_EQUAL)
    body = sg[sg.find("NetworkSolveGlueResult networkSolveSkip("):]
    body = body[:body.find("\n}\n")]
    order = ["++n_skipped;", "if (trace) trace <<", "networkSolveEpochStatusRow(", "networkWithheldHeartbeat(",
             "row.tail(run, ep, f)", "prev_week = se.time.week; prev_tow = se.time.tow;",
             "result.action = EpochAction::SkipEpoch;"]
    pos = [body.find(o) for o in order]
    rep.check("S6", all(p >= 0 for p in pos) and pos == sorted(pos), f"networkSolveSkip effect order {pos}")
    rep.check("S6", "ctx.knobs.h_sigma0_census ? ctx.carry.ep_h_sigma0_census" in body
              and ": f.sigma0_adapt);" in body, "heartbeat sigma0 is not census-or-sigma0_adapt")
    rep.check("S6", "<< ',' << f.plan->declared_dimension << ',' << row.trace_flag" in body
              and '<< ",0,0,0," << total_pins' in body, "trace row template differs from the frozen")
    of1 = sg[sg.find("SolveSkipFacts SolveSkipFacts::of(const SolveDatum& d) {"):]
    of1 = of1[:of1.find("\n}\n")]
    rep.check("S6", "f.plan = &d.plan;" in of1 and "sigma0_adapt" not in of1,
              "SolveSkipFacts::of(datum) must keep &d.plan and leave sigma0_adapt at 0.0")
    of2 = sg[sg.find("SolveSkipFacts SolveSkipFacts::of(const SolveDatum& d, const WithinEpochOutcome& we,"):]
    of2 = of2[:of2.find("\n}\n")]
    rep.check("S6", "f.est_index_of = &est_index_of;" in of2 and "SolveSkipFacts f = of(d);" in of2,
              "SolveSkipFacts::of(datum, we, est_index_of) must keep the orchestrator's addresses")


def func_def(text, name):
    i = text.find(f"{name}(")
    while i >= 0:
        ls = text.rfind("\n", 0, i) + 1
        line_end = text.find("\n", i)
        if not text[ls:i].startswith(" ") and text[ls:i].strip() and ")" in text[i:] and \
                "{" in text[i:text.find(";", i) if text.find(";", i) >= 0 else len(text)]:
            end = text.find("\n}\n", i)
            return text[ls:end + 3]
        i = text.find(f"{name}(", line_end)
    return None


def s7_homes(tree, base, rep):
    for old, new, name in MOVED:
        frozen = func_def(S.git_show(base, old), name)
        cur = func_def(tree.read(new) or "", name)
        rep.check("S7", frozen is not None and frozen == cur, f"{name}: not byte-identical at {new}")
        if old != new:
            rep.check("S7", func_def(tree.read(old) or "", name) is None, f"{name}: still defined in {old}")
    table = tree.read("src/core/filter/NetworkRunEnd.cpp") or ""
    for _o, new, name in MOVED[:3]:
        home = new.replace("src/core/", "")
        rep.check("S7", f'"{home}", {name}}}' in table, f"run-end table row of {name} does not name {home}")


TEXT_MOVES = [("src/core/filter/NetworkContributions.cpp",
               '"networkSolveGlue, a-priori clock anchor (NetworkSolveGlue.cpp)"',
               '"networkFoldColdClockAnchor (NetworkClockAnchor.cpp)"'),
              ("src/core/filter/NetworkResponses.cpp", '"NetworkSolveGlue.cpp within-epoch replay"',
               '"NetworkRaimWithinEpoch.cpp within-epoch replay"'),
              ("src/core/filter/NetworkResponses.cpp", "\"NetworkSolveGlue.cpp a withheld epoch's free response\"",
               "\"NetworkRaimWithinEpoch.cpp a withheld epoch's free response\"")]


def s7_sg3(tree, base, rep):
    """SG3: the three printed citations moved (declared text moves, decision 11), and the RHS
    trip's explicit form retired for networkRhsAttribTripAt (decision 12), its body unchanged."""
    for path, old, new in TEXT_MOVES:
        t = tree.read(path) or ""
        rep.check("S7", t.count(new) == 1 and old not in t, f"{path}: the citation {new} is not moved")
    cpp = tree.read("src/core/filter/NetworkRhsAttrib.cpp") or ""
    hpp = tree.read("include/filter/NetworkRhsAttrib.hpp") or ""
    rep.check("S7", "networkRhsAttribTrip(NetworkRunContext&" not in cpp + hpp, "the explicit trip form remains")
    rep.check("S7", cpp.count("void networkRhsAttribTripAt(") == 1 and hpp.count("void networkRhsAttribTripAt(") == 1,
              "networkRhsAttribTripAt is not defined and declared once")
    frozen = S.git_show(base, "src/core/filter/NetworkRhsAttrib.cpp")
    fb = frozen[frozen.index("    if (!ctx.knobs.rec_on) return;"):]
    fb = fb[:fb.index(LF + "}" + LF)]
    nb = cpp[cpp.index("    if (!ctx.knobs.rec_on) return;"):] if "    if (!ctx.knobs.rec_on) return;" in cpp else ""
    nb = nb[:nb.index(LF + "}" + LF)] if nb else ""
    rep.check("S7", fb == nb, "networkRhsAttribTripAt's body differs from the explicit form's")
    rep.check("S7", "networkRhsAttribTripAt(run, ep, ep.solve.plan.shared_datum, ep.assembly.est_index_of, "
              "&ep.solve.x_s);" in cpp, "the epoch form does not forward the epoch's datum, map and solution")


def s8_docs(rows, tree, rep, docs=None):
    headers = list((tree.root / "include" / "filter").glob("*.hpp"))
    for fn in rows.FUNCTIONS:
        if fn.kind != "fn":
            continue
        if fn.sig[0].startswith("static"):
            # a file-local function's Doxygen is its head comment in the .cpp (SG2's DOC rows)
            doc = "\n".join((docs or {}).get(fn.name, []))
            if not doc:
                continue
            ok = all(re.search(r"@param\s+" + re.escape(p) + r"\b", doc) for p in params_of(fn.sig))
            if not fn.sig[0].startswith("static void"):
                ok = ok and "@return" in doc
            rep.check("S8", ok, f"{fn.name}: file-local Doxygen lacks a @param or @return")
            continue
        sig0 = fn.sig[0].split("(")[0] + "("
        doc = None
        for h in headers:
            rel = str(h.relative_to(tree.root)).replace("\\", "/")
            text = tree.read(rel) or ""
            i = text.find("\n" + sig0)
            if i >= 0:
                j = text.rfind("/**", 0, i)
                doc = text[j:i]
                break
        if doc is None:
            rep.check("S8", False, f"{fn.name}: no declaration in include/filter")
            continue
        ok = "@brief" in doc and all(f"@param {p}" in doc or f"@param  {p}" in doc or re.search(
            r"@param\s+" + re.escape(p) + r"\b", doc) for p in params_of(fn.sig))
        if not fn.sig[0].startswith("void"):
            ok = ok and "@return" in doc
        rep.check("S8", ok, f"{fn.name}: Doxygen lacks @brief, a @param or @return")


def s10_replay(tree, rep, stage):
    calls, lines = orchestrator_calls(tree)
    joined = " ".join(" ".join(lines).split())
    rep.check("S10", "networkWithinEpochAdapt(run, ep, datum, x_s, ws, est_index_of, dd_build, funnel, handoff, we)"
              in joined, "the orchestrator does not pass its own ws / est_index_of / dd_build / funnel")
    if stage == "sg1":
        t2 = fn_text(tree, R.fn_by_name("networkWithinEpochAdapt")) or []
        n = sum(l.count("networkBuildCarryDdPriors(run, ep, dd_build, funnel)") for l in S.code_lines(t2))
        rep.check("S10", n == 1, f"the replay's DD rebuild call appears {n} times")
        return
    # SG2: the rebuild lives in weReplay, which every replay reaches through a forwarding lambda
    # that passes T2's own ws, est_index_of, dd_build, funnel and state (never a copy).
    from nom_sg import rows_sg2 as R2
    by = {f.name: f for f in R2.FUNCTIONS}
    rp = fn_text(tree, by["weReplay"]) or []
    n = sum(l.count("networkBuildCarryDdPriors(run, ep, dd_build, funnel)") for l in S.code_lines(rp))
    rep.check("S10", n == 1, f"weReplay's DD rebuild call appears {n} times")
    fwd = "return weReplay(run, ep, datum, ws, est_index_of, dd_build, funnel, st);"
    for name in ("networkWithinEpochAdapt", "weUndo"):
        t = fn_text(tree, by[name]) or []
        rep.check("S10", sum(l.count(fwd) for l in t) == 1, f"{name}: the replay is not forwarded with its own objects")


def s11_paths(rows, tree, rep):
    for fn in rows.FUNCTIONS:
        if fn.kind == "comment":
            continue
        lines = fn_text(tree, fn) or []
        code = "\n".join(S.code_lines(lines))
        for f in FORBIDDEN_EP:
            rep.check("S11", re.search(re.escape(f) + r"\b", code) is None, f"{fn.name} spells {f}")


def s12_stateless(tree, rep):
    text = tree.read("include/filter/NetworkSbasis.hpp") or ""
    i = text.find("class NetworkSbasis {")
    lines = text[i:].split("\n")
    code = S.code_lines(lines)
    flat_nl = "\n".join(code)
    k = flat_nl.index("{")
    depth, top, j, paren = 1, [], k + 1, 0
    while depth > 0 and j < len(flat_nl):   # the class body at its own depth, nested blocks elided
        ch = flat_nl[j]
        if depth == 1 and ch in "()":
            paren += 1 if ch == "(" else -1
            top.append(ch)
        elif ch == "{" and not (depth == 1 and paren > 0):
            depth += 1
            if depth == 2:
                top.append(";")
        elif ch == "}" and not (depth == 1 and paren > 0):
            depth -= 1
        elif depth == 1:
            top.append(ch)
        j += 1
    members = []
    for st in "".join(top).split(";"):
        st = " ".join(st.split())
        st = re.sub(r"^((public|private|protected)\s*:\s*)+", "", st)
        if not st or "(" in st or re.match(r"(static|using|typedef|friend|enum|struct|class|template)\b", st):
            continue
        members.append(st)
    rep.check("S12", not members, f"NetworkSbasis data members: {members[:3]}")
    flat = " ".join(" ".join(code).split())
    for m in ("buildSharedDatum", "completeSbasisDimension", "assertNullSpaceRank"):
        k = flat.find(f" {m}(")
        end = flat.find(";", k)
        rep.check("S12", k >= 0 and flat[k:end].rstrip().endswith(") const"), f"NetworkSbasis::{m} is not const")


def s13_files(base, rep):
    out = subprocess.run(["git", "-C", str(ROOT), "diff", "--name-only", base], capture_output=True,
                         text=True).stdout.split()
    new = subprocess.run(["git", "-C", str(ROOT), "ls-files", "--others", "--exclude-standard"],
                         capture_output=True, text=True).stdout.split()
    for f in out + new:
        rep.check("S13", f in DECLARED_FILES or f.startswith("docs/cards/"), f"undeclared file touched: {f}")


def s14_knob(tree, rep):
    k = tree.read("src/core/app/NetworkKnobs.cpp") or ""
    rep.check("S14", k.count("k.retro_feed = k.ura_retro || k.ura_refloat_inflate > 1.0;") == 1,
              "NetworkKnobs::retro_feed is not the frozen initializer with its aliases replaced")
    fp = tree.read("src/core/app/NetworkSnapshotFingerprint.cpp") or ""
    rep.check("S14", "retro_feed" not in fp, "retro_feed entered the snapshot fingerprint")


CHECKS = ["S1", "S1b", "S2", "S3", "S5", "S6", "S7", "S8", "S10", "S11", "S12", "S13", "S14"]


class Unit:
    """One frozen source and the rows cut from it: SG1 (the solve at the base) or SG2 (T2 at the
    SG1 commit).  FUNCTIONS / DROPS are the whole table (coverage); `checked` are the functions
    whose text the tree holds at the stage."""

    def __init__(self, fz, rows, checked, locals_map, globals_, docs, names):
        self.fz, self.FUNCTIONS, self.DROPS = fz, rows.FUNCTIONS, rows.DROPS
        self.checked = checked
        self.locals_map, self.globals_, self.docs, self.names = locals_map, globals_, docs, names


class _Checked:
    """A unit seen through the functions the tree holds at the stage."""

    def __init__(self, unit):
        self.FUNCTIONS, self.DROPS = unit.checked, unit.DROPS


def units_for(base, rows, stage):
    """The units of a stage and the region order of the functions."""
    fz = S.Frozen(S.git_show(base, S.SG_CPP))
    canon = S.canon(fz)
    names = {c[0] for c in canon} | set(S.SOLVE_LOCALS) | set(S.SOLVE_PARAMS)
    if stage == "sg1":
        u1 = Unit(fz, rows, list(rows.FUNCTIONS), S.SOLVE_LOCALS, (), {}, names)
        return canon, [u1], list(rows.FUNCTIONS), {f.name: fz for f in rows.FUNCTIONS}, {}
    from nom_sg import rows_sg2 as R2
    fz2 = S.Frozen(S.git_show(R2.SG1_COMMIT, S.WE_HOME), R2.T2_SIG)
    u1 = Unit(fz, rows, [f for f in rows.FUNCTIONS if f.name != "networkWithinEpochAdapt"], S.SOLVE_LOCALS,
              (), {}, names)
    names2 = names | set(R2.T2_LOCALS) | {"datum", "handoff", "we", "x_s"}
    u2 = Unit(fz2, R2, list(R2.FUNCTIONS), R2.T2_LOCALS, R2.FILE_CONSTS, R2.DOC, names2)
    order = []
    for f in rows.FUNCTIONS:
        order.extend(R2.FUNCTIONS if f.name == "networkWithinEpochAdapt" else [f])
    fz_of = {f.name: (fz2 if f in R2.FUNCTIONS else fz) for f in order}
    return canon, [u1, u2], order, fz_of, R2.DOC


def s2_chain(base, rep):
    """SG2's frozen T2 (the SG1 commit) is SG1's rendering of networkWithinEpochAdapt from the base."""
    import nom_sg_cut as C
    from nom_sg import rows_sg2 as R2
    fz = S.Frozen(S.git_show(base, S.SG_CPP))
    canon, names = C.solve_scope(fz)
    rendered = C.render_fn(fz, R.fn_by_name("networkWithinEpochAdapt"), canon, names)
    fz2 = S.Frozen(S.git_show(R2.SG1_COMMIT, S.WE_HOME), R2.T2_SIG)
    frozen = fz2.lines[fz2.first - 1:fz2.last]
    rep.check("S2", rendered[1:] == frozen, "SG2's frozen T2 is not SG1's rendering of networkWithinEpochAdapt")


def run_all(base, tree, rows, verbose=False, only=None, stage=None):
    import nom_sg_cut as C
    stage = stage or C.CURRENT
    S.ACTIVE["stage"] = stage
    canon, units, order, fz_of, docs = units_for(base, rows, stage)
    rep = Report(verbose)
    todo = only or CHECKS
    fz = units[0].fz
    for u in units:
        if "S1" in todo:
            s1_coverage(u.fz, u, rep)
        if "S1b" in todo:
            s1b_forms(u.fz, u, rep)
        if "S2" in todo:
            s2_bodies(u.fz, _Checked(u), tree, rep)
        if "S3" in todo:
            s3_bindings(u.fz, _Checked(u), tree, rep, canon, u.names, u.locals_map, u.globals_)
        if "S11" in todo:
            s11_paths(_Checked(u), tree, rep)
        if "S8" in todo:
            s8_docs(_Checked(u), tree, rep, u.docs)
    if "S2" in todo:
        if stage != "sg1":
            s2_chain(base, rep)
        s2_regions(order, fz_of, docs, tree, rep)
    if "S5" in todo:
        s5_order(fz, rows, tree, rep)
    if "S6" in todo:
        s6_exits(fz, rows, tree, rep)
    if "S7" in todo:
        s7_homes(tree, base, rep)
        if stage == "sg3":
            s7_sg3(tree, base, rep)
    if "S10" in todo:
        s10_replay(tree, rep, stage)
    if "S12" in todo:
        s12_stateless(tree, rep)
    if "S13" in todo and tree.overlay == {}:
        s13_files(base, rep)
    if "S14" in todo:
        s14_knob(tree, rep)
    return rep


# --------------------------------------------------------------------------- negative controls
WE = "src/core/filter/NetworkRaimWithinEpoch.cpp"
SGC = "src/core/filter/NetworkSolveGlue.cpp"
CBDC = "src/core/filter/NetworkCodeBiasDatum.cpp"


def mut_file(path, old, new, nth=1):
    def f(tree, rows):
        t = tree.read(path)
        assert t.count(old) >= max(nth, 1), f"control anchor not found in {path}: {old[:50]!r}"
        k = -1
        if nth < 0:   # the last occurrence
            k = t.rindex(old)
        for _ in range(max(nth, 0)):
            k = t.index(old, k + 1)
        tree.overlay[path] = t[:k] + new + t[k + len(old):]
    return f


def mut_rows(fn):
    def f(tree, rows):
        fn(rows)
    return f


def _drop_r11c(rows):
    cbd = rows.fn_by_name("networkCodeBiasDatum")
    cbd.body = [i for i in cbd.body if not (isinstance(i, S.P) and i.row == "R11c")]


def _r04_whole(rows):
    rows.fn_by_name("networkCodeBiasDatum").body[0].form = "whole"


CONTROLS = [
    ("C1", "networkWithinEpochAdapt takes ws by value", ["S3"],
     mut_file(WE, "Eigen::VectorXd& x_s, EpochWorkspace& ws,", "Eigen::VectorXd& x_s, EpochWorkspace ws,")),
    ("C2", "the replay's DD rebuild passes a local EpochDdBuild", ["S2", "S10"],
     mut_file(WE, "networkBuildCarryDdPriors(run, ep, dd_build, funnel);",
              "EpochDdBuild dd_local; networkBuildCarryDdPriors(run, ep, dd_local, funnel);")),
    ("C3", "networkPreGatePriors called after networkSbasisCompletion", ["S5"],
     mut_file(SGC, "    networkPreGatePriors(run, ep, datum, ws);\n    networkEstimabilityDump(run, ep, datum);\n"
                   "    networkKaulaMargin(run);\n    networkSbasisCompletion(run, ep, datum, ws);\n",
              "    networkEstimabilityDump(run, ep, datum);\n    networkKaulaMargin(run);\n"
              "    networkSbasisCompletion(run, ep, datum, ws);\n    networkPreGatePriors(run, ep, datum, ws);\n")),
    ("C4", "networkFoldCodeBiasKaulaPrior called after the clock rows", ["S2", "S5"],
     lambda tree, rows: (mut_file(CBDC, "        networkFoldCodeBiasKaulaPrior(run, ep, ws);\n", "")(tree, rows),
                         mut_file(SGC, "    networkPreGatePriors(run, ep, datum, ws);\n",
                                  "    networkFoldCodeBiasKaulaPrior(run, ep, ws);\n"
                                  "    networkPreGatePriors(run, ep, datum, ws);\n")(tree, rows))),
    ("C5", "a stage binds ep.solve.plan instead of datum.plan", ["S11", "S3"],
     mut_file(SGC, "    const NetworkSbasis::Plan& plan = datum.plan;\n",
              "    const NetworkSbasis::Plan& plan = ep.solve.plan;\n")),
    ("C6", "GateWithheld's trace flag set to 1", ["S6"],
     mut_file(SGC, "     true,  false, false,  0, true,  NetworkProducerStatus::State::WithheldGate,",
              "     true,  false, false,  1, true,  NetworkProducerStatus::State::WithheldGate,")),
    ("C7", "the sigma0 tail erases before the recorder dump", ["S2"],
     lambda tree, rows: _swap_tail(tree)),
    ("C8", "datum_changed bound to a stage-local copy", ["S3"],
     mut_file(SGC, "    const bool& datum_changed = datum.datum_changed;\n",
              "    bool datum_changed = datum.datum_changed;\n")),
    ("C9", "a data member added to NetworkSbasis", ["S12"],
     mut_file("include/filter/NetworkSbasis.hpp", "class NetworkSbasis {\n", "class NetworkSbasis {\n    int cache_{0};\n")),
    ("C10", "a one-token edit inside a moved body", ["S2"],
     mut_file("src/core/filter/NetworkRankGate.cpp", "    ++ctx.counters.n_gate_evaluated;",
              "    ctx.counters.n_gate_evaluated += 2;")),
    ("C11", "GateWithheld's H sigma0 without the 0.0 default", ["S6"],
     mut_file(SGC, "    f.datum_changed = d.datum_changed;\n    return f;",
              "    f.datum_changed = d.datum_changed;\n    f.sigma0_adapt = -1.0;\n    return f;")),
    ("C12", "the reduce-once check moved before the restore exit", ["S2"],
     lambda tree, rows: _move_reduce_once(tree)),
    ("C13", "we_sigma0_pool_c bound by value in networkSigma0Ceiling", ["S3"],
     mut_file(WE, "    std::map<char, std::vector<double>>& we_sigma0_pool_c = we.sigma0_pool_c;   // written\n",
              "    std::map<char, std::vector<double>> we_sigma0_pool_c = we.sigma0_pool_c;   // written\n",
              nth=-1)),
    ("C14", "WeRestoreFailed's row given the T2 meter columns", ["S6"],
     mut_file(SGC, "     true,  true,  false, -1, false, NetworkProducerStatus::State::Unknown,",
              "     true,  true,  true,  -1, false, NetworkProducerStatus::State::Unknown,")),
    ("C15", "the WCF resolves counter line dropped from the replay", ["S2"],
     lambda tree, rows: _drop_line(tree, WE, "we_culprit_withhold.resolves")),
    ("C16", "networkCensusSigma0 called after networkRankGate", ["S5"],
     mut_file(SGC, "    networkCensusSigma0(run, ep, est_index_of);\n    networkCodeBiasDatum(run, ep, ws);\n",
              "    networkCodeBiasDatum(run, ep, ws);\n")
     if False else lambda tree, rows: (
         mut_file(SGC, "    networkCensusSigma0(run, ep, est_index_of);\n", "")(tree, rows),
         mut_file(SGC, "    const bool clk_model_warm = networkClockModelWarm(run);\n",
                  "    networkCensusSigma0(run, ep, est_index_of);\n"
                  "    const bool clk_model_warm = networkClockModelWarm(run);\n")(tree, rows))),
    ("C17", "networkCensusSigma0 binds est_index_of through ep", ["S11", "S3"],
     lambda tree, rows: _census_via_ep(tree)),
    ("C18", "row R11c dropped from the table", ["S1", "S1b"], mut_rows(_drop_r11c)),
    ("C19", "SolveSkipFacts::of stores the address of a copy of the plan", ["S6"],
     mut_file(SGC, "    f.plan = &d.plan;\n", "    static NetworkSbasis::Plan copy; copy = d.plan;\n    f.plan = &copy;\n")),
    ("C20", "R04 declared whole instead of open", ["S1b"], mut_rows(_r04_whole)),
    # SG2 (T2's internals)
    ("C21", "weUndo does not restore the code de-weights", ["S2"],
     lambda tree, rows: _drop_line(tree, WE, "    dw_code_sats  = landed_dw;")),
    ("C22", "T2's replay is forwarded with a copy of its state", ["S2", "S10"],
     mut_file(WE, "            return weReplay(run, ep, datum, ws, est_index_of, dd_build, funnel, st);",
              "            WeEpochState copy = st; return weReplay(run, ep, datum, ws, est_index_of, dd_build, funnel, copy);")),
    # SG3 (the retired explicit form)
    ("C24", "the trip's epoch form forwards no solution", ["S7"],
     mut_file("src/core/filter/NetworkRhsAttrib.cpp", "ep.assembly.est_index_of, &ep.solve.x_s);",
              "ep.assembly.est_index_of, nullptr);")),
    ("C25", "the sigma0 tail passes the epoch's map instead of the solve's", ["S2", "S11"],
     mut_file(WE, "    networkRhsAttribTripAt(run, ep, plan.shared_datum, est_index_of, nullptr);",
              "    networkRhsAttribTripAt(run, ep, plan.shared_datum, ep.assembly.est_index_of, nullptr);")),
    ("C23", "weChooseCulprit binds the dropped satellites by value", ["S3"],
     mut_file(WE, "    const std::set<std::string>& drop_sats = culprits.sats;",
              "    const std::set<std::string> drop_sats = culprits.sats;")),
]


def _swap_tail(tree):
    t = tree.read(WE)
    a = t.index("    networkRhsAttribTrip")
    b = t.index("    if (ctx.knobs.wh_free_erase) {", a)
    c = t.index("\n    }\n", t.index("ResponseSite::AmbWithholdErase", b)) + 7
    tree.overlay[WE] = t[:a] + t[b:c] + t[a:b] + t[c:]


def _move_reduce_once(tree):
    t = tree.read(WE)
    a = t.index("    // Reduce-once invariant (design §3.5)")
    b = t.index("\n    }\n", t.index("the serial Schur path.", a)) + 7
    block = t[a:b]
    t = t[:a] + t[b:]
    k = t.index("    if (we_restore_failed) {")
    tree.overlay[WE] = t[:k] + block + t[k:]


def _drop_line(tree, path, needle):
    t = tree.read(path).split("\n")
    k = next(i for i, l in enumerate(t) if needle in l)
    tree.overlay[path] = "\n".join(t[:k] + t[k + 1:])


def _census_via_ep(tree):
    path = "src/core/filter/NetworkPrefitCensus.cpp"
    t = tree.read(path)
    t = t.replace("void networkCensusSigma0(NetworkRun& run, const NetworkEpoch& ep,\n"
                  "                         const std::vector<int>& est_index_of) {",
                  "void networkCensusSigma0(NetworkRun& run, const NetworkEpoch& ep,\n"
                  "                         const std::vector<int>& est_index_of_unused) {\n"
                  "    const std::vector<int>& est_index_of = ep.assembly.est_index_of;", 1)
    tree.overlay[path] = t


def run_controls(base):
    import importlib
    bad = 0
    for cid, what, must, mutate in CONTROLS:
        rows = importlib.reload(R)
        tree = Tree()
        mutate(tree, rows)
        rep = run_all(base, tree, rows, only=must)
        fired = [k for k in must if rep.failed(k)]
        ok = bool(fired)
        bad += not ok
        print(f"  [{'fired' if ok else 'MISSED'}] {cid} {what}: failed {fired or 'nothing'} (expected one of {must})")
    importlib.reload(R)
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=S.BASE)
    ap.add_argument("--controls", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)
    if a.controls:
        print("nom_sg_verify: negative controls")
        bad = run_controls(a.base)
        print("controls: " + ("all fired" if not bad else f"{bad} missed"))
        return 1 if bad else 0
    rep = run_all(a.base, Tree(), R, a.verbose)
    print(f"nom_sg_verify: base {a.base}, candidate {ROOT}")
    bad = rep.summary(CHECKS)
    print("nom_sg_verify: " + ("PASS" if not bad else f"FAIL ({bad} check(s))"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

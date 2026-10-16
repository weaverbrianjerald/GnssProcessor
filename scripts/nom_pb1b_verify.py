#!/usr/bin/env python3
"""Relocation verifier of card NOM3 step PB1b (the reduced gate G-PB-R authorized for the step).

    python scripts/nom_pb1b_verify.py [--controls] [-v]

Reads the frozen networkPassB_ at rows_pb1b.BASE (git show) and the working tree, and checks:

  V1 coverage   every code line of the frozen produce loop's try body lands exactly once, in
                order, in the station steps composed in walk order (kStationBuildSteps as the
                source declares it, the carry table spliced at the walk of carried_priors), or
                is the old side of a declared substitution (GUARD, SPLIT_GUARD, UNWRAP, EXIT,
                RESULT, FRAME, PURE_DECL_MOVE, CALLABLE, TYPE_MOVE, MEMBER_TO_FREE call sites,
                the two dropped closing braces); comments and spacing are ignored.
  V2 bindings   every binding statement of every generated function is a ROOT of the closed
                list whose declarator equals the frozen one (const may be added; em and config_
                are the declared RESULT / member exceptions), a FRAME member reference of the
                frozen type, a CALLABLE forwarder with the frozen parameter list (defaults
                included), or a verbatim copy of a frozen alias statement; every frozen name a
                body reads is bound in it or declared at its top level.
  V3 braces     each generated function's braces balance and it ends at its closing brace.
  V4 events     the ordered log calls and trace writes of the frozen loop equal those of the
                composed steps.
  V5 helpers    the five MEMBER_TO_FREE bodies equal their frozen bodies; parameters are the
                frozen ones behind the declared leading parameters.
  V6 callables  the three lambda bodies equal their functions'; PlPend's members are equal.
  V7 passB      networkPassB_'s preamble statements are frozen ones (nep -> ep), its body is the
                frozen body with the loop replaced by the call of networkAssemblyProduce; the
                produce is the frozen loop around the walk, under the closed binding list.
  V8 tables     the source tables equal the canonical rows (name, home, function, order), each
                function defined once in its home source and declared in its home header.
  V9 literals   every string literal of the changed src/ and include/ files, after the
                declared output moves are reversed, is a literal of the base.

--controls applies mutations to the candidate in memory and requires each to fail its named
check before a pass is believed.
"""
import argparse
import difflib
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import nom_cpp  # noqa: E402
from nom_pb1 import frozen_pb1b as fz  # noqa: E402
from nom_pb1 import rows_pb1b as rows  # noqa: E402

ROOT = HERE.parent
SB_SRC = "src/core/filter/NetworkStationBuild.cpp"
SB_HDR = "include/filter/NetworkStationBuild.hpp"
BIND_END = "// ---- End of the bindings ----"
BIND_HEAD = "// ---- From the station build (card NOM3 step PB1b"
EVENT = re.compile(r"\b(spdlog|log|logger)\b|->(info|warn|error|debug|critical)\(|"
                   r"\btrace\w*\s*<<|emitEpochStatus|\.emit\w*\(")


class Check(Exception):
    pass


# ---------------------------------------------------------------------------------------------
# Normalization
# ---------------------------------------------------------------------------------------------
def code_lines(lines):
    """The code of each line of @p lines (tokens joined, comments and spacing dropped), with
    empty lines removed."""
    text = "\n".join(lines)
    toks = nom_cpp.lex(text)
    by = {}
    for t in toks:
        by.setdefault(t.line, []).append(t)
    out = []
    for n in sorted(by):
        s, prev = [], False
        for t in by[n]:
            word = t.kind in ("id", "num")
            if word and prev:
                s.append(" ")
            s.append(t.text)
            prev = word
        out.append("".join(s))
    return out


def norm(s):
    c = code_lines([s.replace(chr(10), " ")])
    return c[0] if c else ""


def negate(c):
    c = c.strip()
    return "!" + c if re.fullmatch(r"[A-Za-z_][\w.]*", c) else "!(" + c + ")"


def conj(ops):
    return " && ".join(f"({o})" if ("||" in o or "?" in o) and len(ops) > 1 else o for o in ops)


# ---------------------------------------------------------------------------------------------
# The candidate tree
# ---------------------------------------------------------------------------------------------
class Tree:
    def __init__(self, overlay=None):
        self.overlay = overlay or {}
        self.cache = {}

    def read(self, rel):
        if rel in self.overlay:
            return self.overlay[rel]
        if rel not in self.cache:
            p = ROOT / rel
            self.cache[rel] = p.read_bytes().decode("utf-8").replace("\r\n", "\n") if p.exists() else None
        return self.cache[rel]


def find_function(text, sig_re, rel):
    """(lines, body_first_index) of the one definition matching @p sig_re (a regex over a line)."""
    L = text.split("\n")
    hits = [k for k, l in enumerate(L) if re.match(sig_re, l)]
    if len(hits) != 1:
        raise Check(f"{rel}: {len(hits)} definitions matching {sig_re!r}")
    k = hits[0]
    T = nom_cpp.Tokens(text, rel)
    a = next(i for i in range(T.n) if T.line(i) == k + 1)
    j = a
    while T.t(j) != "{":
        j += 1
    close = T.close(j)
    if close is None or close <= j:
        raise Check(f"{rel}: unbalanced braces in the function at line {k + 1}")
    end = T.line(close)
    if L[end - 1] != "}":
        raise Check(f"V3 {rel}: the function at line {k + 1} does not end at a closing brace "
                    f"on its own line (line {end}: {L[end - 1]!r})")
    return L[k:end], T.line(j) - k


def split_function(lines):
    """(bindings lines, body lines) of a generated function (lines from its signature)."""
    e = [k for k, l in enumerate(lines) if l.strip() == BIND_END]
    if len(e) != 1:
        raise Check(f"no single end-of-bindings marker in {lines[0]!r}")
    h = [k for k, l in enumerate(lines) if l.strip().startswith(BIND_HEAD)
         or l.strip().startswith("// ---- From the run and the epoch")]
    if len(h) != 1:
        raise Check(f"no single bindings head in {lines[0]!r}")
    return lines[h[0]:e[0] + 1], lines[e[0] + 1:-1]


# ---------------------------------------------------------------------------------------------
# The verifier
# ---------------------------------------------------------------------------------------------
class Verifier:
    def __init__(self, tree):
        self.t = tree
        self.P = fz.Frozen(fz.git_show(ROOT, rows.BASE, rows.ASM))
        self.alias_code = {norm(" ".join(s.text(with_comment=False))): s for s in self.P.aliases}
        self.rows = {r["name"]: (r, "top") for r in rows.TOP}
        self.rows.update({r["name"]: (r, "carry") for r in rows.CARRY})
        self.frame = {n: (b, t) for n, b, t in rows.FRAME_MEMBERS}
        self.functions = {}

    # ---- tables (V8) ---------------------------------------------------------------------------
    def tables(self):
        src = self.t.read(SB_SRC)
        out = {}
        for tname in ("kStationBuildSteps", "kStationCarrySteps"):
            m = re.search(tname + r" = \{\{(.*?)\n\}\};", src, re.S)
            if not m:
                raise Check(f"V8 {SB_SRC}: no {tname}")
            ents = re.findall(r'\{"(\w+)", "filter/(\w+)",.*?&(\w+)\}', m.group(1), re.S)
            out[tname] = ents
        want = {"kStationBuildSteps": [(r["name"], r["home"], r["func"]) for r in rows.TOP],
                "kStationCarrySteps": [(r["name"], r["home"], r["func"]) for r in rows.CARRY]}
        for tname in out:
            if out[tname] != want[tname]:
                raise Check(f"V8 {tname} differs from the canonical rows: {out[tname]}")
        return out

    def step(self, name, home, func, table):
        rel = f"src/core/filter/{home}.cpp"
        text = self.t.read(rel)
        if text is None:
            raise Check(f"V8 {rel} missing")
        sig = (rf"^StationStep {func}\(StationBuild& build\) \{{$" if table == "top"
               else rf"^void {func}\(StationCarryFrame& carry\) \{{$")
        lines, _ = find_function(text, sig, rel)
        hdr = self.t.read(f"include/filter/{home}.hpp") or ""
        decl = (f"StationStep {func}(StationBuild& build);" if table == "top"
                else f"void {func}(StationCarryFrame& carry);")
        if hdr.count(decl) != 1:
            raise Check(f"V8 include/filter/{home}.hpp: {decl!r} declared {hdr.count(decl)} times")
        self.functions[func] = (rel, lines)
        return split_function(lines)

    # ---- V1: the composed sequence -----------------------------------------------------------
    def reverse_step(self, name, body):
        r, table = self.rows[name]
        c = code_lines(body)
        if table == "top":
            if not c or c[-1] != norm("return StationStep::Next;"):
                raise Check(f"V1 {name}: does not end with its RETURN")
            c = c[:-1]
        ret = "return StationStep::Next;" if table == "top" else "return;"
        split, guard = r.get("split", []), r.get("guard")
        if name == "data_rows":
            res_old, res_new, res_ref = rows.RESULT
            c = [norm(res_old) if x == norm(res_new) else x for x in c]
            if norm(res_ref) not in c:
                raise Check("V1 data_rows: the RESULT reference is missing")
            c.remove(norm(res_ref))
            n = sum(x.count(norm(rows.EXIT[1])) for x in c)
            c = [x.replace(norm(rows.EXIT[1]), norm(rows.EXIT[0])) for x in c]
            if n != 2:
                raise Check(f"V1 data_rows: {n} EXIT returns (2 declared)")
        if r.get("head_old"):
            g = norm(f"if ({negate(conj(split))}) {ret}")
            if c[0] != g:
                raise Check(f"V1 {name}: the split guard reads {c[0]!r}, not {g!r}")
            c[0] = norm(r["head_old"])
            if c[-1] != norm(r["tail_insert"]):
                raise Check(f"V1 {name}: the inserted close is missing")
            c = c[:-1]
        elif guard or split:
            g = norm(f"if ({negate(conj(list(split) + ([guard] if guard else [])))}) {ret}")
            k = next((j for j, x in enumerate(c) if x.startswith("if(")), None)
            if k is None or c[k] != g:
                raise Check(f"V6 {name}: the guard reads {c[k] if k is not None else None!r}, "
                            f"not the frozen conjunction {g!r}")
            if guard:
                c = c[:k] + [norm(f"if ({guard}) {{")] + c[k + 1:] + ["}"]
            else:
                c = c[:k] + c[k + 1:]
        if r.get("unwrap"):
            c = ["{"] + c + ["}"]
        return [self.reverse_common(x) for x in c]

    def reverse_common(self, x):
        for old, new in rows.CALL_SITES:
            if x.startswith(norm(new)) or norm(new) in x:
                x = x.replace(norm(new), norm(old))
        for n, (b, _) in self.frame.items():
            if x == norm(b):
                return f"<FRAME {n}>"
        return x

    def reverse_carried(self, body, carry_seq):
        r, _ = self.rows["carried_priors"]
        c = code_lines(body)
        if c[-1] != norm("return StationStep::Next;"):
            raise Check("V1 carried_priors: does not end with its RETURN")
        c = c[:-1]
        g = norm(f"if ({negate(r['guard'])}) return StationStep::Next;")
        k = c.index(g) if g in c else None
        if k is None:
            raise Check("V6 carried_priors: the frozen guard is missing")
        c[k] = norm(f"if ({r['guard']}) {{")
        # The frame construction (FRAME / PURE_DECL_MOVE): removed after its initializers are
        # checked against the frozen text.
        s = next((j for j, x in enumerate(c) if x.startswith(norm("StationCarryFrame carry{"))), None)
        if s is None:
            raise Check("V1 carried_priors: no frame construction")
        e = s
        while not c[e].endswith("}};"):
            e += 1
        cons = "".join(c[s:e + 1])
        for n, src in self.frame_inits().items():
            if src not in cons:
                raise Check(f"V1 carried_priors: the frame's {n} initializer is not the frozen "
                            f"text {src!r}")
        if not cons.startswith(norm("StationCarryFrame carry{build, legacy_local_inject,")):
            raise Check("V1 carried_priors: the frame's first members are not build, "
                        "legacy_local_inject")
        c = c[:s] + c[e + 1:]
        w = norm("for (const StationCarryStep& s : kStationCarrySteps) s.build(carry);")
        if c.count(w) != 1:
            raise Check("V1 carried_priors: the carry walk is missing")
        k = c.index(w)
        c = c[:k] + carry_seq + c[k + 1:] + ["}"]
        return [self.reverse_common(x) for x in c]

    def frame_inits(self):
        P = self.P
        out = {}
        c1 = P.piece_lines("c1")
        k = next(j for j, l in enumerate(c1) if l.strip().startswith("const bool do_amb_carry ="))
        e = k
        while not c1[e].rstrip().endswith(";"):
            e += 1
        out["do_amb_carry"] = "".join(code_lines(c1[k:e + 1])).split("=", 1)[1].rstrip(";")
        t10 = P.piece_lines("T10")
        x = next(l for l in t10 if "const Eigen::VectorXd x_lin =" in l)
        out["x_lin"] = norm(x).split("=", 1)[1].rstrip(";")
        k = next(j for j, l in enumerate(t10) if l.strip().startswith("CouplingRowBuilder cb{"))
        e = k
        while not t10[e].rstrip().endswith(";"):
            e += 1
        out["cb"] = "".join(code_lines(t10[k:e + 1])).replace("CouplingRowBuilder cb{", "{").rstrip(";")
        return out

    def frozen_sequence(self):
        """The frozen try body's code lines with the declared old sides applied."""
        P = self.P
        keys = [k for k, _ in rows.PIECES if k not in ("prod_head", "prod_tail")]
        seq = {}
        for k in keys:
            lines = P.piece_lines(k)
            if k == "c1":
                lines = self.drop_callables(lines)
            seq[k] = self.frame_old(lines)
        order = [k for k in keys if k not in rows.DROPPED_PIECES and k != "LLI"]
        # PURE_DECL_MOVE: the frame predicate moves past raim_columns into carried_priors.
        i = order.index("T10")
        order.insert(i, "LLI")
        t9 = "\n".join(P.piece_lines("T9"))
        for op in ("carry_warm", "phase_datum_prioritized", "is_pivot", "zwd_on", "zwd_seam_live"):
            if re.search(r"\b" + op + r"\s*(=[^=]|\+\+|--|\+=|-=)", t9):
                raise Check(f"V1 PURE_DECL_MOVE: raim_columns writes the predicate operand {op}")
        out = []
        for k in order:
            out += seq[k]
        return out

    def drop_callables(self, lines):
        out = list(lines)
        for c in rows.CALLABLES:
            k = next(j for j, l in enumerate(out) if re.match(r"\s*const auto " + c["lam"] + r" = \[&\]\(", l))
            ind = len(out[k]) - len(out[k].lstrip())
            e = next(j for j in range(k, len(out)) if out[j] == " " * ind + "};")
            out = out[:k] + out[e + 1:]
        k = next(j for j, l in enumerate(out) if l.strip().startswith("struct PlPend {"))
        e = next(j for j in range(k, len(out)) if out[j].strip() == "};")
        return out[:k] + out[e + 1:]

    def frame_old(self, lines):
        """Code lines with each frozen frame-member declaration replaced by its placeholder."""
        out, k = [], 0
        pats = {n: re.compile(r"^\s*" + re.escape(t) + r"\s+" + re.escape(n) + r"\s*(=|;|\{)")
                for n, (_, t) in self.frame.items()}
        while k < len(lines):
            hit = next((n for n, p in pats.items() if p.match(lines[k])), None)
            if hit and hit != "legacy_local_inject":
                e = k
                while not re.sub(r"//.*$", "", lines[e]).rstrip().endswith(";"):
                    e += 1
                out.append(f"<FRAME {hit}>")
                k = e + 1
                continue
            out += code_lines([lines[k]])
            k += 1
        return out

    def composed_sequence(self, tables):
        carry = []
        for name, home, func in tables["kStationCarrySteps"]:
            binds, body = self.step(name, home, func, "carry")
            self.check_bindings(func, binds, body, "carry")
            carry += self.reverse_step(name, body)
        out = []
        for name, home, func in tables["kStationBuildSteps"]:
            binds, body = self.step(name, home, func, "top")
            self.check_bindings(func, binds, body, "top")
            out += (self.reverse_carried(body, carry) if name == "carried_priors"
                    else self.reverse_step(name, body))
        return out

    # ---- V2: bindings ----------------------------------------------------------------------------
    def frozen_decls(self):
        """name -> the frozen declarator text (before the name)."""
        out = {}
        for s in self.P.roots:
            out[s.name] = re.split(r"\b" + s.name + r"\b", norm(" ".join(s.text(with_comment=False))))[0]
        out["st"] = norm("NetStation&")
        out["R"] = norm("StationAssembly&")
        out["i"] = norm("int")
        out["h2local"] = norm("std::map<int, int>&")
        return out

    def check_bindings(self, func, binds, body, kind):
        fd = self.frozen_decls()
        roots = {norm(l): n for n, l in rows.ROOTS}
        roots[norm(rows.BUILD_FROM_CARRY)] = "build"
        frames = {norm(b): n for n, (b, _) in self.frame.items()}
        text = "\n".join(binds)
        T = nom_cpp.Tokens(text)
        bound = set()
        for s, e in nom_cpp.statements(T, 0, T.n):
            code = norm(T.src(s, e).replace("\n", " "))
            d = nom_cpp.parse_declaration(T, s, e)
            name = d[0].name if d else None
            if code in roots:
                n = roots[code]
                if n not in ("build", "em", "config_", "ctx", "est", "setup"):
                    want = fd.get(n)
                    got = re.split(r"\b" + n + r"\b", code)[0]
                    if want is None or (got.replace("const ", "", 1).strip()
                                        != want.replace("const ", "", 1).strip()):
                        raise Check(f"V2 {func}: ROOT {n} declared {got!r}, frozen {want!r}")
                bound.add(n)
            elif code in frames:
                bound.add(frames[code])
            elif code in self.alias_code:
                bound.add(self.alias_code[code].name)
            elif name in {c["lam"] for c in rows.CALLABLES}:
                self.check_forwarder(func, name, T, s, e)
                bound.add(name)
            else:
                raise Check(f"V2 {func}: binding {code!r} is no ROOT, FRAME, alias or forwarder")
        btext = "\n".join(body)
        used = fz.names_used(btext) - fz.top_level_decls(btext)
        names = ({n for n, _ in rows.ROOTS} | {s.name for s in self.P.aliases} | set(self.frame)
                 | {c["lam"] for c in rows.CALLABLES})
        missing = sorted(n for n in used & names if n not in bound)
        if missing:
            raise Check(f"V2 {func}: the body reads {missing} without a binding")

    def check_forwarder(self, func, name, T, s, e):
        frozen = self.lambda_params(name)
        src = T.src(s, e)
        m = re.match(r"\s*const auto \w+ = \[&\]\((.*?)\)\s*\{", src, re.S)
        if not m or norm(m.group(1)) != norm(frozen):
            raise Check(f"V2 {func}: the forwarder {name} does not take the frozen parameters "
                        f"{frozen!r}")
        c = next(x for x in rows.CALLABLES if x["lam"] == name)
        if c["func"] + "(carry," not in norm(src).replace(" ", ""):
            raise Check(f"V2 {func}: the forwarder {name} does not call {c['func']}")

    def lambda_params(self, name):
        c1 = self.P.piece_lines("c1")
        k = next(j for j, l in enumerate(c1) if re.match(r"\s*const auto " + name + r" = \[&\]\(", l))
        h = k
        while not c1[h].rstrip().endswith("{"):
            h += 1
        head = " ".join(l.strip() for l in c1[k:h + 1])
        return re.match(r"const auto \w+ = \[&\]\((.*)\) \{$", head).group(1)

    def lambda_body(self, name):
        c1 = self.P.piece_lines("c1")
        k = next(j for j, l in enumerate(c1) if re.match(r"\s*const auto " + name + r" = \[&\]\(", l))
        ind = len(c1[k]) - len(c1[k].lstrip())
        h = k
        while not c1[h].rstrip().endswith("{"):
            h += 1
        e = next(j for j in range(h, len(c1)) if c1[j] == " " * ind + "};")
        return c1[h + 1:e]

    # ---- V5, V6: helpers and callables ---------------------------------------------------------
    def helpers(self):
        P = self.P
        for h in rows.HELPERS:
            info = P.helpers[h["old"]]
            fb = P.lines[P.T.line(info["body_open"]):info["end"] - 1]
            rel = f"src/core/filter/{h['home']}.cpp"
            lines, bl = find_function(self.t.read(rel), r"^\w[\w:<>]* " + h["new"] + r"\(", rel)
            if code_lines(lines[bl:-1]) != code_lines(fb):
                raise Check(f"V5 {h['new']}: the body differs from Application::{h['old']}'s")
            sig = norm(" ".join(lines[:bl]))
            want = norm(f"{h['new']}(" + ", ".join(h["lead"] + P.helper_params(h["old"])) + ") {")
            if not sig.endswith(want):
                raise Check(f"V5 {h['new']}: signature {sig!r} is not the frozen parameters behind "
                            f"the declared leading ones")
            if "Application::" + h["old"] in (self.t.read(rows.ASM) or ""):
                raise Check(f"V5 Application::{h['old']} is still defined")
        for c in rows.CALLABLES:
            rel = f"src/core/filter/{rows.CALLABLE_HOME}.cpp"
            lines, _ = find_function(self.t.read(rel), r"^\w+ " + c["func"] + r"\(", rel)
            binds, body = split_function(lines)
            self.check_bindings(c["func"], binds, body, "helper")
            if code_lines(body) != code_lines(self.lambda_body(c["lam"])):
                raise Check(f"V6 {c['func']}: the body differs from the frozen lambda {c['lam']}")
        hdr = self.t.read(SB_HDR)
        m = re.search(r"struct PlPend \{(.*?)\n\};", hdr, re.S)
        c1 = self.P.piece_lines("c1")
        k = next(j for j, l in enumerate(c1) if l.strip().startswith("struct PlPend {"))
        e = next(j for j in range(k, len(c1)) if c1[j].strip() == "};")
        if not m or code_lines(m.group(1).split("\n")) != code_lines(c1[k + 1:e]):
            raise Check("V6 PlPend: the namespace type's members differ from the frozen struct's")

    # ---- V7: networkPassB_ and the produce -------------------------------------------------------
    def passb(self):
        P = self.P
        text = self.t.read(rows.ASM)
        lines, _ = find_function(text, r"^EpochAction Application::networkPassB_\(", rows.ASM)
        if re.search(r"\bnep\b", "\n".join(lines)):
            raise Check("V7 networkPassB_ still names nep")
        b = next(k for k, l in enumerate(lines) if l.strip().startswith(fz.PASSB_BANNER))
        pre = "\n".join(lines[:b])
        T = nom_cpp.Tokens(pre)
        a = next(i for i in range(T.n) if T.t(i) == "{") + 1
        froz = {norm(re.sub(r"\bnep\b", "ep", " ".join(s.text(with_comment=False))))
                for s in P.roots + P.aliases}
        for s, e in nom_cpp.statements(T, a, T.n):
            code = norm(T.src(s, e).replace("\n", " "))
            if code not in froz:
                raise Check(f"V7 networkPassB_: preamble statement {code!r} is not frozen")
        L = P.lines
        want = (code_lines([re.sub(r"\bnep\b", "ep", l) for l in L[P.banner - 1:P.pragma - 1]])
                + [norm("networkAssemblyProduce(run, ep, gim_, assembly);")]
                + code_lines([re.sub(r"\bnep\b", "ep", l) for l in L[P.loop_close:P.close_line]]))
        got = code_lines(lines[b:])
        if got != want:
            raise Check("V7 networkPassB_ body differs:\n" + diff(want, got))
        # The produce.
        src = self.t.read(SB_SRC)
        pl, _ = find_function(src, r"^void networkAssemblyProduce\(", SB_SRC)
        binds, body = split_function(pl)
        allowed = {norm(x) for x in (
            "NetworkRunContext& ctx = run.ctx;", "const GnssConfig& config_ = run.config;",
            "NetworkSetupResult& setup = run.setup;", "NetworkEstimator& est = run.engines.est;",
            "const SyncedEpoch& se = ep.se;", "const EpochClock& epoch_clock = ep.clock;",
            "const EpochPassA& pass_a = ep.pass_a;",
            "const std::vector<bool>& present = pass_a.present;",
            "std::vector<StationAssembly>& asm_res = assembly.asm_res;")}
        T = nom_cpp.Tokens("\n".join(binds))
        for s, e in nom_cpp.statements(T, 0, T.n):
            code = norm(T.src(s, e).replace("\n", " "))
            if code not in allowed and code not in self.alias_code:
                raise Check(f"V7 networkAssemblyProduce: binding {code!r} is not declared")
        s0, _ = P.pieces["prod_head"]
        t0, _ = P.pieces["prod_tail"]
        want = (code_lines(L[P.pragma - 1:s0])
                + [norm("StationBuild build{ctx, config_, est, setup, se, epoch_clock, pass_a, gim, i, st, R};"),
                   norm("if (!networkStationBuild(build)) continue;")]
                + code_lines(L[t0 - 1:P.loop_close]))
        got = code_lines(body)
        if got != want:
            raise Check("V7 networkAssemblyProduce body differs:\n" + diff(want, got))

    # ---- V9: literals ---------------------------------------------------------------------------
    def literals(self):
        names = subprocess.run(["git", "-C", str(ROOT), "diff", "--name-only", rows.BASE, "--",
                                "src", "include"], capture_output=True, text=True).stdout.split()
        names += subprocess.run(["git", "-C", str(ROOT), "ls-files", "--others", "--exclude-standard",
                                 "--", "src", "include"], capture_output=True, text=True).stdout.split()
        names += [r for r in self.t.overlay if r.startswith(("src/", "include/"))]
        sed = []
        for mv in rows.OUTPUT_MOVES:
            sed.append((mv[2].strip('"'), mv[1].strip('"')))
        base_lits, new_lits = set(), set()
        for rel in sorted(set(names)):
            try:
                b = fz.git_show(ROOT, rows.BASE, rel)
            except subprocess.CalledProcessError:
                b = ""
            base_lits |= lits(b)
            n = self.t.read(rel) or ""
            new_lits |= lits(n)
        # The contributions builder column: its declared new texts and their old ones.
        rs = (pathlib.Path("D:/GnssProcessorSupport/gates/pb1b/reverse.sed").read_text().split("\n"))
        for l in rs:
            m = re.match(r"^s/(.*)/(.*)/g$", l)
            if m:
                sed.append((sed_unesc(m.group(1)), sed_unesc(m.group(2))))
        added = {f'"{r["name"]}"' for r in rows.TOP + rows.CARRY} | \
                {f'"filter/{r["home"]}"' for r in rows.TOP + rows.CARRY} | \
                {'"ContributionKindSet holds one bit per kind"'}   # a static_assert, not printed
        bad = []
        for x in sorted(new_lits - base_lits):
            y = x
            for new, old in sed:
                y = y.replace(new, old)
            if y not in base_lits and x not in added and not any(y.strip('"') in b for b in base_lits):
                bad.append(x)
        if bad:
            raise Check("V9 string literals changed outside the declared output moves: "
                        + "; ".join(bad[:5]))

    # ---- V4 -------------------------------------------------------------------------------------
    @staticmethod
    def events(seq):
        return [x for x in seq if EVENT.search(x)]

    def run(self):
        tables = self.tables()
        got = self.composed_sequence(tables)
        want = self.frozen_sequence()
        if self.events(got) != self.events(want):
            raise Check("V4 the ordered log calls and trace writes differ:\n"
                        + diff(self.events(want), self.events(got)))
        if got != want:
            raise Check("V1 the composed steps differ from the frozen produce loop:\n" + diff(want, got))
        self.helpers()
        self.passb()
        self.literals()
        return len(want)


def sed_unesc(s):
    return re.sub(r"\\(.)", r"\1", s)


def lits(text):
    """The string literals of @p text, adjacent literals concatenated."""
    out, cur = set(), None
    for t in nom_cpp.lex(text):
        if t.kind == "str":
            cur = (cur[:-1] + t.text[1:]) if cur else t.text
        else:
            if cur:
                out.add(cur)
            cur = None
    if cur:
        out.add(cur)
    return out


def diff(a, b):
    return "\n".join(list(difflib.unified_diff(a, b, "frozen", "candidate", n=2, lineterm=""))[:40])


# ---------------------------------------------------------------------------------------------
# Negative controls
# ---------------------------------------------------------------------------------------------
def mutate(tree, rel, old, new, count=1):
    text = tree.read(rel)
    if text.count(old) < count:
        raise SystemExit(f"control: {rel}: {old!r} not found")
    return {rel: text.replace(old, new, count)}


def controls(verbose):
    base = Tree()
    sb = base.read(SB_SRC)
    m_op = re.search(r'    \{"oracle_pin",.*?\},\n', sb, re.S).group(0)
    m_zw = re.search(r'    \{"zwd",.*?\},\n', sb, re.S).group(0)
    swapped = sb.replace(m_op + m_zw, m_zw + m_op)
    cases = [
        ("P1 oracle_pin and zwd swapped in the carry table", {SB_SRC: swapped}, "V8"),
        ("P2 rx_cb's guard not the frozen condition",
         mutate(base, "src/core/filter/NetworkRxCodeBiasCarry.cpp",
                "if (!(rx_cb_on || rx_cb_datum)) return;",
                "if (!(rx_cb_on && rx_cb_datum)) return;"), "V6"),
        ("P3 a binding's type changed",
         mutate(base, "src/core/filter/NetworkZwdCarry.cpp", "const double pl_max_gap_s  =",
                "const float pl_max_gap_s  ="), "V2"),
        ("P4 a body line dropped",
         mutate(base, "src/core/filter/NetworkRxCodeBiasCarry.cpp", "++R.pl_cb_injected;", ""), "V1"),
        ("P5 the data_rows exits rewritten to Next",
         mutate(base, "src/core/filter/NetworkStationSplit.cpp", "return StationStep::Done;",
                "return StationStep::Next;", 2), "V1"),
        ("P6 a log line added inside a step",
         mutate(base, "src/core/filter/NetworkZwdCarry.cpp",
                "if (!(legacy_local_inject && (zwd_on || zwd_seam_live))) return;",
                "if (!(legacy_local_inject && (zwd_on || zwd_seam_live))) return;\n"
                "    spdlog::warn(\"zwd\");"), "V4"),
        ("P7 a moved helper's body edited",
         mutate(base, "src/core/filter/NetworkPhaseDatum.cpp",
                "if (!phase_datum_prioritized) return stations[i].is_pivot;",
                "if (!phase_datum_prioritized) return !stations[i].is_pivot;"), "V5"),
        ("P8 the pl_emit_row forwarder without its defaults",
         mutate(base, "src/core/filter/NetworkCoupling.cpp",
                "const std::string& sat = {},", "const std::string& sat,"), "V2"),
        ("P9 a printed literal edited outside the declared list",
         mutate(base, "src/core/filter/NetworkResponses.cpp", "carried ionospheres not folded",
                "carried ionospheres unfolded"), "V9"),
        ("P10 a step's closing brace removed",
         mutate(base, "src/core/filter/NetworkRxClockCarry.cpp",
                "\n}\n\n// ---- End of the PASS B station steps", "\n\n// ---- End of the PASS B station steps"),
         "V"),
        ("P11 a frame member declared as a local of its step",
         mutate(base, "src/core/filter/NetworkRxClockCarry.cpp",
                "std::vector<LocalCarry>& rxclk_pred = carry.rxclk_pred;",
                "std::vector<LocalCarry> rxclk_pred;"), "V1"),
    ]
    ok = True
    for name, overlay, want in cases:
        try:
            Verifier(Tree(overlay)).run()
            print(f"  CONTROL FAILED TO FAIL: {name}")
            ok = False
        except Check as ex:
            first = str(ex).split("\n")[0]
            hit = first.startswith(want) or (want == "V" and first.startswith("V"))
            print(f"  {'ok ' if hit else 'WRONG CHECK'} {name}: {first[:150]}")
            ok &= hit
        except SystemExit as ex:
            print(f"  ok  {name}: {str(ex)[:150]}")
    return ok


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--controls", action="store_true")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args(argv)
    if a.controls:
        print("negative controls (each must fail its named check):")
        if not controls(a.v):
            print("nom_pb1b_verify: a control did not fail as declared")
            return 1
    try:
        n = Verifier(Tree()).run()
    except Check as ex:
        print(f"nom_pb1b_verify: FAIL {ex}")
        return 1
    print(f"nom_pb1b_verify: PASS (V1-V9; {n} frozen code lines of the produce loop carried, "
          f"{len(rows.TOP)} + {len(rows.CARRY)} steps, {len(rows.HELPERS)} helpers, "
          f"{len(rows.CALLABLES)} callables)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

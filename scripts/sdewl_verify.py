#!/usr/bin/env python3
"""Relocation verifier of the per-satellite cut of sdEwlResolveArcs (src/core/network/ar/SdEwlRows.cpp).

    python scripts/sdewl_verify.py [--controls] [-v]

Reads the frozen function at BASE (git) and the working tree and checks (exit 0 only when all pass):

  V1 coverage   the frozen function, token by token, equals the new sdEwlResolveArcs with every call
                of a new function replaced by that function's body (nested calls inlined in call
                order). The declared changes are closed:
                  - the pre-loop statements of the frozen function that the new one drops;
                  - the leading binding statements of a new function (a verbatim copy of one of
                    those dropped statements, or `T& x = ps.x;` for a member of SdEwlPairSearch);
                  - the declaration `SdEwlPairSearch ps;`, replaced by the struct's members, which
                    must equal the frozen declarations they took over;
                  - the call statement (`f(...);`, or `if (!f(...)) return;` for a bool stage) and
                    the final `return true;` of a bool stage;
                  - `continue` <-> `return;` (the function the loop calls, and a tail stage) or
                    `return false;` (a bool stage), and only for a `continue` of the OUTER loop: the
                    frozen token must sit at no inner loop and in no lambda, and so must the new one.
                Comments and spacing are ignored.
  V2 dropped    every dropped pre-loop statement is copied verbatim as a binding in at least one new
                function; every binding is used.
  V3 bindings   a name the body writes is never a by-value parameter or binding: a written parameter
                is a non-const reference, a written `ps` member binding is a non-const reference of
                the member's declared type and `ps` is then a non-const reference; a parameter passed
                on to a callee's non-const reference is itself a non-const reference; call arguments
                are the callee's parameter names in order.
  V4 braces     each new function's brackets balance and it is defined once, declared once in the
                header with the same parameters and return type, with a @brief comment.
  V5 events     counts of trace calls and counter writes of the frozen body and the composed text.
  V6 prose      the lines added to the two files carry no em dash and no card citation.

--controls applies mutations to the candidate in memory; each must fail its named check.
"""
import argparse
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import nom_cpp as nc  # noqa: E402
from pb3_verify import is_write  # noqa: E402

ROOT = HERE.parent
BASE = "1549ed3"
SRC = "src/core/network/ar/SdEwlRows.cpp"
HDR = "include/network/ar/SdEwlRows.hpp"
TOP = "sdEwlResolveArcs"
NEW = ("sdEwlResolveSat", "sdEwlElectPair", "sdEwlCheckPair", "sdEwlBuildArc")
STRUCT, STRUCT_VAR = "SdEwlPairSearch", "ps"


class Tree:
    def __init__(self, over=None):
        self.over = over or {}

    def read(self, rel):
        if rel in self.over:
            return self.over[rel]
        return (ROOT / rel).read_bytes().decode("utf-8").replace("\r\n", "\n")


def inner_flags(T, a, b):
    """Per token of [a, b): 1 inside an inner loop body or a lambda body, else 0."""
    f = [0] * (b - a)
    for i in range(a, b):
        t = T.t(i)
        rng = None
        if T.k(i) == "id" and t in ("for", "while") and T.t(i + 1) == "(":
            cl = T.close(i + 1)
            rng = (cl + 1, nc.stmt_end(T, cl + 1, b))
        elif T.k(i) == "id" and t == "do":
            rng = (i + 1, nc.stmt_end(T, i + 1, b))
        elif T.k(i) == "op" and t == "[":
            lam = nc._lambda_at(T, i, b)
            if lam:
                rng = (lam[2], T.close(lam[2]) + 1)
        if rng:
            for j in range(rng[0], min(rng[1], b)):
                f[j - a] = 1
    return f


def texts(T, a, b):
    return [T.t(i) for i in range(a, b)]


def declared_name(tt):
    """Name declared by a binding statement `... name = expr;` (token before the first top-level =)."""
    d = 0
    for i, x in enumerate(tt):
        if x in "([{":
            d += 1
        elif x in ")]}":
            d -= 1
        elif x == "=" and d == 0:
            return tt[i - 1]
    return None


def uses(T, a, b, name):
    """Indices in [a, b) of identifier tokens `name` that are not member names (after . or ->)."""
    return [i for i in range(a, b) if T.k(i) == "id" and T.t(i) == name and T.t(i - 1) not in (".", "->", "::")]


class Check:
    def __init__(self, tree):
        self.tree, self.fails, self.notes = tree, [], []

    def fail(self, chk, msg):
        self.fails.append((chk, msg))

    # ---- loading ---------------------------------------------------------------------------
    def load(self):
        gt = nc.GitTree(BASE)
        self.fT = gt.tokens(SRC)
        fs = gt.index(SRC).find(TOP, None, True)
        assert len(fs) == 1
        self.ff = fs[0]
        self.T = nc.Tokens(self.tree.read(SRC), SRC)
        idx = nc.FileIndex(self.T)
        self.funcs = {}
        for name in (TOP,) + NEW:
            d = idx.find(name, None, True)
            if len(d) != 1:
                self.fail("V4", f"{name} defined {len(d)} times")
            if d:
                self.funcs[name] = d[0]
        self.H = nc.Tokens(self.tree.read(HDR), HDR)
        self.hidx = nc.FileIndex(self.H)
        for T, p in ((self.T, SRC), (self.H, HDR)):
            bad = [i for i in range(len(T)) if T.k(i) == "op" and T.t(i) in "(){}[]" and T.match[i] < 0]
            if bad:
                self.fail("V4", f"{p}:{T.line(bad[0])}: unbalanced bracket")
        self.load_struct()
        self.kept, self.bindings, self.ev_tokens, self.composed = [], {}, [], False
        self.pre_txt = [texts(self.fT, a, b) for a, b in self.frozen_parts()[0]]

    def load_struct(self):
        H = self.H
        self.fields, self.struct_body = {}, None
        for i in range(len(H) - 2):
            if H.t(i) == "struct" and H.t(i + 1) == STRUCT and H.t(i + 2) == "{":
                self.struct_body = (i + 3, H.close(i + 2))
                self.struct_at = i
        if not self.struct_body:
            self.fail("V4", f"struct {STRUCT} not found")
            return
        for a, b in nc.statements(H, *self.struct_body):
            tt = texts(H, a, b - 1)
            segs, cur, d = [], [], 0
            for x in tt:
                if x in "([{":
                    d += 1
                elif x in ")]}":
                    d -= 1
                if x == "," and d == 0:
                    segs.append(cur)
                    cur = []
                else:
                    cur.append(x)
            segs.append(cur)
            base = None
            for s in segs:
                e = s.index("=") if "=" in s else len(s)
                if base is None:
                    base = s[:e - 1]
                self.fields[s[e - 1]] = base

    # ---- the frozen stream -----------------------------------------------------------------
    def frozen_parts(self):
        T, (o, c) = self.fT, self.ff.body
        st = nc.statements(T, o + 1, c)
        li = [k for k, (a, b) in enumerate(st) if T.t(a) == "for"]
        assert li == [len(st) - 1], "frozen function is not `preamble; for (...) {...}`"
        pre = st[:-1]
        a, b = st[-1]
        ob = T.close(a + 1) + 1
        assert T.t(ob) == "{"
        return pre, (a, ob, T.close(ob))

    # ---- the composed stream ---------------------------------------------------------------
    def is_binding(self, T, f, a, b, pre_txt):
        tt = texts(T, a, b)
        if tt in pre_txt:
            return ("copy", tt)
        if len(tt) > 7 and tt[-5:-2] == ["=", STRUCT_VAR, "."] and tt[-2] == tt[-6] and tt[-1] == ";":
            return ("ps", tt)
        return None

    def split_args(self, T, open_):
        spans, end = nc.split_params(T, open_)
        return [texts(T, s, e) for s, e in spans]

    def match_call(self, T, a, b):
        """(callee, open paren, is_if_form) when the statement [a, b) is a call of a new function."""
        if T.k(a) == "id" and T.t(a) in NEW and T.t(a + 1) == "(" and T.close(a + 1) == b - 2 and T.t(b - 1) == ";":
            return T.t(a), a + 1, False
        if (T.t(a) == "if" and T.t(a + 1) == "(" and T.t(a + 2) == "!" and T.t(a + 3) in NEW
                and T.t(a + 4) == "(" and T.t(T.close(a + 4) + 1) == ")" and T.close(a + 1) == T.close(a + 4) + 1
                and T.t(T.close(a + 4) + 2) == "return" and T.t(T.close(a + 4) + 3) == ";" and b == T.close(a + 4) + 4):
            return T.t(a + 3), a + 4, True
        return None

    def expand(self, caller, name, tail_ok, out, is_top=False):
        f, T = self.funcs[name], self.T
        o, c = f.body
        st = nc.statements(T, o + 1, c)
        k = 0
        binds = []
        while k < len(st) and self.is_binding(T, f, *st[k], self.pre_txt):
            binds.append(st[k])
            k += 1
        self.bindings[name] = binds
        fl = inner_flags(T, o + 1, c)
        ret_bool = f.ret == "bool"
        for n, (a, b) in enumerate(st[k:]):
            last = n == len(st) - k - 1
            call = self.match_call(T, a, b)
            if call:
                callee, op, if_form = call
                self.check_call(f, callee, op, if_form, tail_ok)
                sub_tail = tail_ok and last and not if_form
                self.expand(name, callee, sub_tail, out)
                continue
            tt = texts(T, a, b)
            if tt == [STRUCT, STRUCT_VAR, ";"]:
                out.extend((x, False) for x in texts(self.H, *self.struct_body))
                continue
            if ret_bool and last and tt == ["return", "true", ";"]:
                continue
            i = a
            while i < b:
                x = T.t(i)
                if x == "return" and not fl[i - o - 1]:
                    if tail_ok and not ret_bool and T.t(i + 1) == ";":
                        out.extend([("continue", True), (";", False)])
                        i += 2
                        continue
                    if ret_bool and T.t(i + 1) == "false" and T.t(i + 2) == ";":
                        out.extend([("continue", True), (";", False)])
                        i += 3
                        continue
                out.append((x, False))
                i += 1
            self.ev_tokens.append((name, a, b))

    def check_call(self, f, callee, op, if_form, tail_ok):
        T, g = self.T, self.funcs[callee]
        callee_bool = g.ret == "bool"
        if callee_bool != if_form:
            self.fail("V1", f"{f.name} calls {callee} in the wrong form for its return type")
        if if_form and not tail_ok:
            self.fail("V1", f"{f.name} stops on {callee} where a return is not a continue of the outer loop")
        args = self.split_args(T, op)
        pn = [p.name for p in g.params]
        if [a[0] if len(a) == 1 else None for a in args] != pn:
            self.fail("V3", f"{f.name} -> {callee}: arguments {[' '.join(a) for a in args]} are not the parameters {pn} in order")
            return
        mine = {p.name: p for p in f.params}
        for a, p in zip(args, g.params):
            if p.kind == "W" and a[0] in mine and mine[a[0]].kind != "W":
                self.fail("V3", f"{f.name}: `{a[0]}` is passed to {callee} as a non-const reference but is not one here")

    def compose(self):
        T = self.T
        top = self.funcs[TOP]
        o, c = top.body
        st = nc.statements(T, o + 1, c)
        out, kept = [], []
        li = [k for k, (a, b) in enumerate(st) if T.t(a) == "for"]
        if li != [len(st) - 1]:
            self.fail("V1", f"{TOP} is not `bindings; for (...) {{...}}`")
            return None
        for a, b in st[:-1]:
            tt = texts(T, a, b)
            if tt not in self.pre_txt:
                self.fail("V1", f"{TOP}: statement at line {T.line(a)} is not a frozen pre-loop statement")
            kept.append(tt)
            out.extend((x, False) for x in tt)
        a, b = st[-1]
        ob = T.close(a + 1) + 1
        out.extend((x, False) for x in texts(T, a, ob + 1))
        body = nc.statements(T, ob + 1, T.close(ob))
        calls = [self.match_call(T, x, y) for x, y in body]
        if len(body) != 1 or not calls[0] or calls[0][0] != NEW[0] or calls[0][2]:
            self.fail("V1", f"{TOP}: the loop body is not the single call of {NEW[0]}")
            return None
        self.check_call(top, NEW[0], calls[0][1], False, True)
        self.expand(TOP, NEW[0], True, out)
        out.append(("}", False))
        self.kept, self.composed = kept, True
        return out

    def frozen_stream(self):
        fT = self.fT
        pre, (la, ob, cb) = self.frozen_parts()
        kept_txt = self.kept
        out = []
        for a, b in pre:
            if texts(fT, a, b) in kept_txt:
                out.extend((x, False) for x in texts(fT, a, b))
        fl = inner_flags(fT, ob + 1, cb)
        out.extend((x, False) for x in texts(fT, la, ob + 1))
        for i in range(ob + 1, cb):
            out.append((fT.t(i), fT.t(i) == "continue" and not fl[i - ob - 1]))
        out.append(("}", False))
        return out

    # ---- checks ----------------------------------------------------------------------------
    def v1(self):
        comp = self.compose()
        if comp is None:
            return
        frz = self.frozen_stream()
        n = 0
        while n < min(len(comp), len(frz)) and comp[n] == frz[n]:
            n += 1
        if n == len(comp) == len(frz):
            self.notes.append(f"V1 {len(frz)} frozen tokens equal the composed text "
                              f"({sum(1 for _, s in comp if s)} outer `continue` substitutions, "
                              f"{sum(1 for x, _ in frz if x == 'continue') - sum(1 for _, s in frz if s)} inner `continue` unchanged)")
            return
        lo = max(0, n - 3)
        self.fail("V1", f"first difference at token {n}: frozen `{' '.join(x for x, _ in frz[lo:n + 4])}` "
                        f"vs composed `{' '.join(x for x, _ in comp[lo:n + 4])}` "
                        f"(lengths {len(frz)}, {len(comp)})")

    def v2_v3(self):
        T = self.T
        dropped = [t for t in self.pre_txt if t not in self.kept]
        copied = {tuple(tt) for fn in self.bindings for (a, b) in self.bindings[fn]
                  for tt in [texts(T, a, b)] if tt in self.pre_txt}
        for t in dropped:
            if tuple(t) not in copied:
                self.fail("V2", f"dropped statement `{' '.join(t)[:60]}` is copied as a binding nowhere")
        self.notes.append(f"V2 {len(dropped)} dropped pre-loop statements, each copied as a binding")
        nb = 0
        for name, binds in self.bindings.items():
            f = self.funcs[name]
            o, c = f.body
            first_body = binds[-1][1] if binds else o + 1
            params = {p.name: p for p in f.params}
            for a, b in binds:
                tt = texts(T, a, b)
                kind = self.is_binding(T, f, a, b, self.pre_txt)[0]
                nb += 1
                if kind == "copy":
                    nm = declared_name(tt)
                    if not uses(T, first_body, c, nm):
                        self.fail("V2", f"{name}: binding `{nm}` is unused")
                else:
                    mem, bt = tt[-2], tt[:-6]
                    F = self.fields.get(mem)
                    if F is None:
                        self.fail("V3", f"{name}: `{mem}` is not a member of {STRUCT}")
                        continue
                    ref = bt[-1:] == ["&"]
                    core = bt[:-1] if ref else bt
                    wr = any(is_write(T, i) for i in uses(T, first_body, c, mem))
                    if not uses(T, first_body, c, mem):
                        self.fail("V2", f"{name}: binding `{mem}` is unused")
                    if core != F and core != ["const"] + F:
                        self.fail("V3", f"{name}: binding `{mem}` has type `{' '.join(core)}`, the member is `{' '.join(F)}`")
                    if wr and not (core == F and ref):
                        self.fail("V3", f"{name}: `{mem}` is written but its binding is not a non-const reference")
                    p = params.get(STRUCT_VAR)
                    if core == F and ref and (p is None or p.kind != "W"):
                        self.fail("V3", f"{name}: `{mem}` binds a non-const reference but `{STRUCT_VAR}` is not a non-const reference parameter")
            for pn, p in params.items():
                us = uses(T, o + 1, c, pn)
                if not us:
                    self.fail("V2", f"{name}: parameter `{pn}` is unused")
                if any(is_write(T, i) for i in us) and p.kind != "W":
                    self.fail("V3", f"{name}: `{pn}` is written but is not a non-const reference parameter")
        self.notes.append(f"V3 {nb} bindings and {sum(len(f.params) for f in self.funcs.values())} parameters keep their category")

    def v4(self):
        for name in NEW:
            if name not in self.funcs:
                continue
            f = self.funcs[name]
            d = self.hidx.find(name, None, False)
            if len(d) != 1:
                self.fail("V4", f"{name} declared {len(d)} times in the header")
                continue
            if [p.code for p in d[0].params] != [p.code for p in f.params] or d[0].ret != f.ret:
                self.fail("V4", f"{name}: header declaration differs from the definition")
            before = self.H.source[:self.H[d[0].start].pos].rstrip()
            blk = before[before.rfind("/**"):] if before.endswith("*/") else ""
            if "@brief" not in blk:
                self.fail("V4", f"{name}: no @brief comment before the declaration")
        if self.struct_body:
            before = self.H.source[:self.H[self.struct_at].pos].rstrip()
            if "@brief" not in before[before.rfind("/**"):] or not before.endswith("*/"):
                self.fail("V4", f"{STRUCT}: no @brief comment")
        self.notes.append(f"V4 {len(NEW)} new functions defined once, declared once with @brief")

    def v5(self):
        def count(T, ranges):
            n_tr = n_ctr = 0
            for a, b in ranges:
                for i in range(a, b):
                    if T.t(i) == "tr" and T.t(i + 1) == "(":
                        n_tr += 1
                    if T.t(i) == "ctr" and T.t(i + 1) == "." and (T.t(i - 1) == "++" or T.t(i + 3) in ("+=",)):
                        n_ctr += 1
            return n_tr, n_ctr
        pre, (la, ob, cb) = self.frozen_parts()
        z = count(self.fT, [(ob, cb)])
        c = count(self.T, [(a, b) for _, a, b in self.ev_tokens])
        if z != c:
            self.fail("V5", f"trace calls / counter writes: frozen {z}, composed {c}")
        else:
            self.notes.append(f"V5 {z[0]} trace calls and {z[1]} counter writes in both")

    def v6(self):
        r = subprocess.run(["git", "-C", str(ROOT), "diff", "-U0", BASE, "--", SRC, HDR], capture_output=True)
        if not self.tree.over:
            for ln in r.stdout.decode("utf-8", "replace").split("\n"):
                if ln.startswith("+") and not ln.startswith("+++"):
                    if "—" in ln or re.search(r"\bcards?\b", ln, re.I):
                        self.fail("V6", f"added line has an em dash or a card citation: {ln[:80]}")
        self.notes.append("V6 added lines carry no em dash and no card citation")

    def run(self):
        self.load()
        if self.funcs.keys() < set((TOP,) + NEW):
            return
        self.v1()
        if self.composed:
            self.v2_v3()
            self.v5()
        self.v4()
        self.v6()


def run_case(tree):
    c = Check(tree)
    try:
        c.run()
    except (ValueError, IndexError, AssertionError, KeyError) as e:
        c.fail("V4", f"verifier error on this text: {type(e).__name__}: {e}")
    return c


def swap_lines(text, first, second):
    L = text.split("\n")
    i = next(k for k, x in enumerate(L) if x.strip() == first)
    j = next(k for k, x in enumerate(L) if k > i and x.strip() == second)
    L[i], L[j] = L[j], L[i]
    return "\n".join(L)


def sub(text, old, new):
    assert text.count(old) == 1, (text.count(old), old)
    return text.replace(old, new)


def controls(verbose):
    base = Tree().read(SRC)
    ctl = [
        ("a statement moved (a.sig1 / a.sig2 swapped)", "V1",
         {SRC: swap_lines(base, "a.sig1 = sig1;", "a.sig2 = sig2;")}),
        ("a continue of the inner pair loop turned into a return", "V1",
         {SRC: sub(base, "if (!sig1.empty()) continue;", "if (!sig1.empty()) return false;")}),
        ("a by-value parameter for a written local (ps in sdEwlElectPair)", "V3",
         {SRC: sub(base, "SdEwlCandidates::const_iterator c2it, SdEwlPairSearch& ps) {",
                   "SdEwlCandidates::const_iterator c2it, SdEwlPairSearch ps) {")}),
        ("a by-value parameter for a written local (by_const in sdEwlBuildArc)", "V3",
         {SRC: sub(base, "const SdEwlPairSearch& ps, std::map<char, std::vector<SdEwlArc>>& by_const) {",
                   "const SdEwlPairSearch& ps, std::map<char, std::vector<SdEwlArc>> by_const) {")}),
        ("a by-value parameter passed on as a reference (by_const in sdEwlResolveSat)", "V3",
         {SRC: sub(base, "                     const SdEwlCandidates& cand, const PppObservation& o,\n"
                   "                     std::map<char, std::vector<SdEwlArc>>& by_const) {",
                   "                     const SdEwlCandidates& cand, const PppObservation& o,\n"
                   "                     std::map<char, std::vector<SdEwlArc>> by_const) {")}),
        ("a written member bound as a const reference", "V3",
         {SRC: sub(base, "\n    std::string& sig1 = ps.sig1;", "\n    const std::string& sig1 = ps.sig1;")}),
        ("a written binding copied by value (ctr)", "V1",
         {SRC: sub(base, "    auto& ctr = v.ctr;\n    const auto& tr = v.tr;\n\n    // --- Extra-wide-lane",
                   "    auto ctr = v.ctr;\n    const auto& tr = v.tr;\n\n    // --- Extra-wide-lane")}),
        ("two stages called in the wrong order", "V1",
         {SRC: swap_lines(base, "if (!sdEwlElectPair(v, o, cons, c1it, c2it, ps)) return;",
                          "if (!sdEwlCheckPair(v, o, f1, f2, ps)) return;")}),
        ("arguments swapped in a stage call", "V3",
         {SRC: sub(base, "sdEwlCheckPair(v, o, f1, f2, ps)", "sdEwlCheckPair(v, o, f2, f1, ps)")}),
        ("a skip that falls through (return false changed to return true)", "V1",
         {SRC: sub(base, '''        tr("BAD_LAMBDA", o.satellite, {}, sig1, sig2, zd.code1, zd.code2, n_cand,
           zd.count, zd.bia_corrected, zd.lambda_wl, zd.mean_m, 0.0, 0.0, 0.0);
        return false;''', '''        tr("BAD_LAMBDA", o.satellite, {}, sig1, sig2, zd.code1, zd.code2, n_cand,
           zd.count, zd.bia_corrected, zd.lambda_wl, zd.mean_m, 0.0, 0.0, 0.0);
        return true;''')}),
    ]
    bad = 0
    for label, chk, over in ctl:
        c = run_case(Tree(over))
        hit = [m for k, m in c.fails if k == chk]
        ok = bool(hit)
        bad += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {label}: expect {chk}, got {sorted({k for k, _ in c.fails}) or 'pass'}")
        if verbose and hit:
            print(f"        {hit[0][:150]}")
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--controls", action="store_true")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args(argv)
    c = run_case(Tree())
    for m in c.notes:
        print(m)
    for k, m in c.fails:
        print(f"FAIL {k}: {m}")
    rc = 1 if c.fails else 0
    if a.controls:
        print("negative controls")
        rc |= 1 if controls(a.v) else 0
    print("PASS" if rc == 0 else "FAILED")
    return rc


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Relocation verifier of step PB3: the PASS A slip scan and the SD-EWL row builder cut into stages.

    python scripts/pb3_verify.py [--controls] [-v]

Reads the frozen functions at BASE (git) and the working tree, and checks (exit 0 only when all pass):

  V1 coverage   the frozen function body, token by token, equals the new orchestrator with every
                call of a stage replaced by that stage's body (nested calls inlined in call order),
                apart from the closed list of declared changes: the call statements (a returned
                local's `T x = stage(...)` and the stage's final `return x;`), the view-init
                statement, the leading binding statements of a stage (`D x = v.x;`, or a verbatim
                copy of a dropped frozen alias or lambda statement), the lambda-to-function
                change of emitGroup (header and closing brace rebuilt from the function), and the
                two renames Arc -> SdEwlArc and Row -> SdEwlRow.  Comments and spacing are ignored.
  V2 dropped    every dropped frozen statement is either copied verbatim as a binding of at least
                one stage, or (a type moved to the header) equal to the header's struct.
  V3 bindings   every name a moved body uses resolves, in its stage, to a binding or parameter of
                the frozen category: reference stays reference, const may be added but never on a
                name the body writes, a name the body writes is never a by-value copy, a frozen
                const stays const, the declared type equals the frozen one (renames and the
                closed alias table applied; `auto` skipped).  Every binding is used.
  V4 braces     each new function's brackets balance and it is defined once, declared once in its
                header with the same parameters, with a @brief comment.
  V5 events     the ordered log calls, trace writes and event writes of the frozen body equal those
                of the composed text (counts printed).
  V6 view       the view-init statement names the view's members in declared order.
  V7 prose      the new headers carry no em dash and no card citation.
  V8 chain      sdEwlResolveArcs was later cut into a per-satellite function and three stages
                (scripts/sdewl_verify.py proves that cut against PER_SAT_BASE). The stage file is read
                with that cut undone (the pre-cut definition spliced back), so V1 to V7 above still
                see the cut of this step, and sdewl_verify must pass as well.

--controls applies mutations to the candidate in memory; each must fail its named check.
"""
import argparse
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import nom_cpp as nc  # noqa: E402

ROOT = HERE.parent
BASE = "deef142"
ASM = "src/core/network/driver/ApplicationNetworkAssembly.cpp"
SDE = "src/core/network/ar/SdEwlAr.cpp"
SDE_ROWS = "src/core/network/ar/SdEwlRows.cpp"
PER_SAT_BASE = "1549ed3"   # the revision whose sdEwlResolveArcs scripts/sdewl_verify.py cuts
PER_SAT_FNS = ("sdEwlResolveArcs", "sdEwlResolveSat", "sdEwlElectPair", "sdEwlCheckPair", "sdEwlBuildArc")

CASES = [
    dict(name="slip", frozen=ASM, orch=ASM, fn=("Application", "networkPassASlipScan_"),
         stage_files=["src/core/network/slip/PassASlipScan.cpp", "src/core/network/slip/PassASlipDrains.cpp"],
         headers=["include/network/slip/PassASlipScan.hpp", "include/network/slip/PassASlipDrains.hpp"],
         prefix="passA", view="PassAView", view_hdr="include/network/slip/PassASlipScan.hpp",
         dropped=[(234, 234, "copy"), (235, 235, "copy"), (236, 236, "copy"), (237, 237, "copy"),
                  (238, 238, "copy")],
         renames={}, aliases={}, pseudo={"config_": (True, True)},
         rets={"passAMwRefloatStage": "relevel_pend"}, lambdas={}),
    dict(name="sdewl", frozen=SDE, orch=SDE, fn=("", "networkAppendSdEwlRows"),
         stage_files=["src/core/network/ar/SdEwlRows.cpp"], headers=["include/network/ar/SdEwlRows.hpp"],
         prefix="sdEwl", view="SdEwlRowView", view_hdr="include/network/ar/SdEwlRows.hpp",
         dropped=[(791, 793, "copy"), (846, 848, "copy"), (862, 866, "copy"),
                  (797, 828, "type"), (1110, 1110, "type")],
         renames={"SdEwlArc": "Arc", "SdEwlRow": "Row"},
         aliases={"SdEwlCandidates": "std::map<std::pair<std::string,char>,std::vector<Cand>>"},
         pseudo={}, rets={}, lambdas={"sdEwlEmitGroup": "emitGroup"}),
]
ASSIGN = {"=", "+=", "-=", "*=", "/=", "%=", "|=", "&=", "^=", "<<=", ">>=", "++", "--"}
MUTATORS = {"push_back", "emplace_back", "emplace", "insert", "erase", "clear", "resize", "reserve",
            "assign", "swap", "pop_back", "append", "setZero", "setOnes", "conservativeResize",
            "insert_or_assign", "try_emplace", "fill", "reset", "merge", "splice"}
EVENT_HEADS = ({"log": "->"}, {"traces": "."}, {"trace": "<<"})


def unfold_per_sat(text):
    """The stage file with the per-satellite cut undone: the five contiguous definitions are replaced
    by the pre-cut sdEwlResolveArcs of PER_SAT_BASE."""
    T = nc.Tokens(text, SDE_ROWS)
    idx = nc.FileIndex(T)
    fs = [idx.find(n, None, True)[0] for n in PER_SAT_FNS]
    for f, g in zip(fs, fs[1:]):
        gap = text[T[f.body[1]].end:T[g.start].pos]
        assert f.body[1] < g.start and not gap.strip(), f"{f.name} and {g.name} are not adjacent"
    BT = nc.GitTree(PER_SAT_BASE).tokens(SDE_ROWS)
    bf = nc.FileIndex(BT).find(PER_SAT_FNS[0], None, True)[0]
    return text[:T[fs[0].start].pos] + BT.src(bf.start, bf.body[1] + 1) + text[T[fs[-1].body[1]].end:]


class Tree:
    def __init__(self, over=None):
        self.over, self.cache = over or {}, {}

    def read(self, rel):
        if rel in self.over:
            return self.over[rel]
        if rel not in self.cache:
            p = ROOT / rel
            self.cache[rel] = p.read_bytes().decode("utf-8").replace("\r\n", "\n") if p.exists() else None
            if rel == SDE_ROWS and self.cache[rel] and "sdEwlResolveSat" in self.cache[rel]:
                self.cache[rel] = unfold_per_sat(self.cache[rel])
        return self.cache[rel]


class Fn:
    """A function definition: its lexed file, token body span, parameters and scope tree."""

    def __init__(self, T, f):
        self.T, self.f = T, f
        self.o, self.c = f.body
        self._sc = None

    @property
    def scopes(self):
        if self._sc is None:
            self._sc = nc.Scopes(self.T, self.o, self.c, self.f.params)
        return self._sc

    def params(self):
        return self.f.params or []


def norm_type(s, ren, alias):
    toks = [t.text for t in nc.lex(s)]
    toks = [ren.get(t, t) for t in toks if t not in ("const", "&", "*", "&&", "volatile")]
    out = "".join(toks)
    for k, v in alias.items():
        out = out.replace(k, v)
    return out


def decl_info(fn, d, case):
    """(is_ref, is_ptr, is_const, type_text or None) of a Decl of @p fn."""
    T = fn.T
    if d.kind == "param":
        p = next((q for q in fn.params() if q.name == d.name), None)
        if p is None:
            return d.is_ref, d.is_ptr, d.is_const, None
        txt = p.text
        const = bool(re.match(r"\s*const\b", txt)) or d.is_const
        base = txt[:txt.rfind(d.name)] if d.name in txt else txt
        return d.is_ref, d.is_ptr, const, base
    ty = T.src(*d.type_span) if d.type_span else ""
    return d.is_ref, d.is_ptr, d.is_const or bool(re.match(r"\s*const\b", ty)), ty


def has_auto(s):
    return s is None or "auto" in [t.text for t in nc.lex(s)]


def is_write(T, k):
    if T.t(k - 1) in ("++", "--"):
        return True
    j = k + 1
    while True:
        if T.t(j) in (".", "->") and T.k(j + 1) == "id":
            if T.t(j + 2) == "(":
                return T.t(j + 1) in MUTATORS
            j += 2
            continue
        if T.t(j) == "[":
            j = T.close(j) + 1
            continue
        break
    return T.t(j) in ASSIGN


class Check:
    def __init__(self, tree, case, verbose=False):
        self.tree, self.case, self.verbose = tree, case, verbose
        self.fails, self.notes = [], []
        self.gt = nc.GitTree(BASE)

    def fail(self, chk, msg):
        self.fails.append((chk, msg))

    def note(self, msg):
        self.notes.append(msg)

    # ---- loading -------------------------------------------------------------------------
    def load(self):
        c = self.case
        cls, name = c["fn"]
        T = self.gt.tokens(c["frozen"])
        fs = self.gt.index(c["frozen"]).find(name, cls or None, True)
        assert len(fs) == 1, f"frozen {name} defined {len(fs)} times"
        self.fz = Fn(T, fs[0])
        t = nc.Tokens(self.tree.read(c["orch"]), c["orch"])
        os_ = nc.FileIndex(t).find(name, cls or None, True)
        assert len(os_) == 1, f"orchestrator {name} defined {len(os_)} times"
        self.orch = Fn(t, os_[0])
        self.stages = {}
        self.stage_file = {}
        for path in c["stage_files"]:
            tx = self.tree.read(path)
            tk = nc.Tokens(tx, path)
            self.check_brackets(tk, path)
            for f in nc.FileIndex(tk).funcs:
                if f.is_def and f.name.startswith(c["prefix"]):
                    if f.name in self.stages:
                        self.fail("V4", f"{f.name} defined twice")
                    self.stages[f.name] = Fn(tk, f)
                    self.stage_file[f.name] = path
        hdr_decl = {}
        for path in c["headers"]:
            tk = nc.Tokens(self.tree.read(path), path)
            self.check_brackets(tk, path)
            for f in nc.FileIndex(tk).funcs:
                if not f.is_def and f.name.startswith(c["prefix"]):
                    hdr_decl.setdefault(f.name, []).append((f, tk))
        self.hdr_decl = hdr_decl

    def check_brackets(self, tk, path):
        bad = [i for i in range(len(tk)) if tk.k(i) == "op" and tk.t(i) in "(){}[]" and tk.match[i] < 0]
        if bad:
            self.fail("V4", f"{path}:{tk.line(bad[0])}: unbalanced bracket")

    # ---- frozen stream ---------------------------------------------------------------------
    def frozen_stream(self):
        fz, T = self.fz, self.fz.T
        ranges = [(a, b, k) for a, b, k in self.case["dropped"]]
        self.dropped = {}
        out = []
        for i in range(fz.o + 1, fz.c):
            ln = T.line(i)
            hit = next((r for r in ranges if r[0] <= ln <= r[1]), None)
            if hit:
                self.dropped.setdefault(hit, []).append(i)
                continue
            out.append((T.t(i), i))
        for r in ranges:
            toks = self.dropped.get(r, [])
            if not toks:
                self.fail("V2", f"dropped range {r} holds no frozen tokens")
        self.drop_text = {r: [T.t(i) for i in toks] for r, toks in self.dropped.items()}
        return out

    # ---- composition -----------------------------------------------------------------------
    def compose(self):
        self.out = []
        self.used_copy = {r: 0 for r in self.case["dropped"] if r[2] == "copy"}
        self.bindings = []   # (fn, decl stmt (a, b), name)
        self.expand(self.orch, True)
        return self.out

    def emit(self, fn, i):
        t = fn.T.t(i)
        self.out.append((self.case["renames"].get(t, t), fn, i))

    def classify(self, fn, a, b):
        T = fn.T
        texts = [T.t(k) for k in range(a, b)]
        for r, ft in self.drop_text.items():
            if r[2] == "copy" and texts == ft:
                self.used_copy[r] += 1
                return "B2"
        if len(texts) >= 6 and texts[-4:-1] == ["v", ".", texts[-2]] and texts[-5] == "=" and texts[-1] == ";":
            ds = nc.parse_declaration(T, a, b)
            if ds and len(ds) == 1 and ds[0].name == texts[-2]:
                return "B1"
        m = self.case["lambdas"]
        if texts[:2] == ["const", "auto"] and texts[2] in m.values() and texts[3:7] == ["=", "[", "&", "]"]:
            return "B3"
        return None

    def expand(self, fn, top):
        T, c = fn.T, self.case
        sts = nc.statements(T, fn.o + 1, fn.c)
        k = 0
        binds = []
        if not top:
            while k < len(sts):
                kind = self.classify(fn, *sts[k])
                if kind is None:
                    break
                binds.append((kind, sts[k]))
                k += 1
            self.bindings.append((fn, binds))
        end = len(sts)
        ret = c["rets"].get(fn.f.name)
        if ret and end > k:
            a, b = sts[end - 1]
            if [T.t(x) for x in range(a, b)] == ["return", ret, ";"]:
                end -= 1
            else:
                self.fail("V1", f"{fn.f.name}: no final `return {ret};`")
        for kind, (a, b) in binds:
            if kind == "B3":
                self.expand_lambda(fn, a, b)
        for a, b in sts[k:end]:
            if top and [T.t(x) for x in range(a, a + 3)] == ["const", c["view"], "v"] and T.t(a + 3) == "{":
                self.view_init = (fn, a, b)
                continue
            self.scan(fn, a, b)

    def expand_lambda(self, fn, a, b):
        T = fn.T
        name = T.t(a + 2)
        callee = next((n for n, l in self.case["lambdas"].items() if l == name), None)
        st = self.stages.get(callee)
        if not st:
            self.fail("V1", f"lambda callee for {name} missing")
            return
        for x in ("const", "auto", name, "=", "[", "&", "]", "("):
            self.out.append((x, fn, a))
        ps = st.params()[1:]
        for n, p in enumerate(ps):
            for tk in nc.lex(p.text):
                self.out.append((self.case["renames"].get(tk.text, tk.text), st, st.f.paren[0]))
            if n + 1 < len(ps):
                self.out.append((",", st, st.f.paren[0]))
        for x in (")", "{"):
            self.out.append((x, st, st.o))
        self.expand(st, False)
        for x in ("}", ";"):
            self.out.append((x, st, st.c))

    def scan(self, fn, a, b):
        T = fn.T
        start_out = len(self.out)
        i = a
        while i < b:
            t = T.t(i)
            if t in self.stages and T.t(i + 1) == "(" and T.t(i - 1) not in (".", "->", "::"):
                ce = T.close(i + 1)
                if T.t(ce + 1) != ";":
                    self.fail("V1", f"{fn.f.name}:{T.line(i)}: call of {t} is not a statement")
                callee = self.stages[t]
                ret = self.case["rets"].get(t)
                if ret and T.t(i - 1) == "=" and T.t(i - 2) == ret:
                    typ = [self.out[x][0] for x in range(start_out, len(self.out) - 2)]
                    self.ret_decls = getattr(self, "ret_decls", []) + [(ret, "".join(typ))]
                    del self.out[start_out:]
                elif i != a and T.t(i - 1) not in (";", "{", "}", ")", "else"):
                    self.fail("V1", f"{fn.f.name}:{T.line(i)}: call of {t} not at a statement start")
                self.expand(callee, False)
                i = ce + 2
                continue
            self.emit(fn, i)
            i += 1

    # ---- checks ----------------------------------------------------------------------------
    def run(self):
        self.load()
        frz = self.frozen_stream()
        comp = self.compose()
        self.v1(frz, comp)
        self.v2()
        self.v3(frz, comp)
        self.v4()
        self.v5(frz, comp)
        self.v6()
        self.v7()
        return self.fails

    def v1(self, frz, comp):
        a, b = [x[0] for x in frz], [x[0] for x in comp]
        if a == b:
            self.note(f"V1 {self.case['name']}: {len(a)} frozen tokens equal the composed text ({len(self.stages)} stages)")
            return
        n = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
        fl = self.fz.T.line(frz[n][1]) if n < len(frz) else -1
        if n < len(comp):
            _, fn, ti = comp[n]
            where = f"{fn.f.name}:{fn.T.line(ti)}"
        else:
            where = "end"
        self.fail("V1", f"first difference at token {n}: frozen line {fl} `{' '.join(a[max(0, n - 3):n + 4])}` "
                        f"vs {where} `{' '.join(b[max(0, n - 3):n + 4])}` (lengths {len(a)}, {len(b)})")

    def v2(self):
        for r, n in self.used_copy.items():
            if n < 1:
                self.fail("V2", f"dropped statement at frozen lines {r[0]}-{r[1]} is copied nowhere")
        for r in self.case["dropped"]:
            if r[2] != "type":
                continue
            toks = self.drop_text.get(r, [])
            ren = self.case["renames"]
            hits = 0
            tk = nc.Tokens(self.tree.read(self.case["view_hdr"]), self.case["view_hdr"])
            for i in range(len(tk)):
                if tk.t(i) == "struct" and tk.t(i + 1) in ren and tk.t(i + 2) == "{":
                    e = tk.close(i + 2)
                    h = [ren.get(tk.t(x), tk.t(x)) for x in range(i, e + 2)]
                    if h == toks:
                        hits += 1
            if hits != 1:
                self.fail("V2", f"type at frozen lines {r[0]}-{r[1]} has {hits} equal header structs")
        if any(r[2] == "type" for r in self.case["dropped"]):
            self.note("V2 types moved to the header equal the frozen statements")

    def v3(self, frz, comp):
        fzs = self.fz.scopes
        memo, wmemo, bad = {}, {}, set()
        c = self.case
        ren, alias = c["renames"], c["aliases"]
        used = {}   # fn -> set of used names (own tokens after bindings)
        n_checked = 0
        for (ft, fi), (ct, fn, ti) in zip(frz, comp):
            if ft != ct or fn.T.k(ti) != "id" or fn.T.t(ti - 1) in (".", "->", "::"):
                continue
            used.setdefault(id(fn), set()).add(fn.T.t(ti))
            name = fn.T.t(ti)
            df = fzs.lookup(name, fi)
            dn = fn.scopes.lookup(name, ti)
            if df is None:
                if dn is not None and name not in c["pseudo"]:
                    bad.add((fn.f.name, name, "binds a name the frozen function did not declare"))
                elif dn is not None:
                    key = (id(fn), id(dn))
                    if key not in memo:
                        memo[key] = 1
                        ir, ip, ic, _ = decl_info(fn, dn, c)
                        if (ir, ic) != c["pseudo"][name]:
                            bad.add((fn.f.name, name, "pseudo binding category"))
                continue
            if dn is None:
                bad.add((fn.f.name, name, "unbound"))
                continue
            if dn.scope is fn.scopes.innermost(ti) or (dn.kind != "param" and not self.is_binding(fn, dn)):
                continue   # declared in the moved text itself
            key = (id(df), id(dn), id(fn))
            if key in memo:
                continue
            memo[key] = 1
            n_checked += 1
            fr, fp, fc, ft_ = decl_info(self.fz, df, c)
            nr, np_, nc_, nt = decl_info(fn, dn, c)
            wk = (id(fn), id(dn))
            if wk not in wmemo:
                wmemo[wk] = self.writes(fn, dn)
            w = wmemo[wk]
            why = None
            if fp != np_:
                why = "pointer category differs"
            elif fr and not nr:
                why = "reference became a copy"
            elif fc and not nc_:
                why = "const dropped"
            elif w and nc_:
                why = "const on a written binding"
            elif w and not nr and not np_:
                why = "written name bound by value"
            elif not has_auto(ft_) and not has_auto(nt) and \
                    norm_type(ft_, ren, alias) != norm_type(nt, ren, alias):
                why = f"type `{ft_.strip()}` vs `{nt.strip()}`"
            if why:
                bad.add((fn.f.name, name, why))
        for fn_, name, why in sorted(bad):
            self.fail("V3", f"{fn_}: `{name}` {why}")
        # every binding is used
        for fn, binds in self.bindings:
            body_names = set()
            T = fn.T
            bs = {a for _, (a, b) in binds}
            first = binds[-1][1][1] if binds else fn.o + 1
            for i in range(first, fn.c):
                if T.k(i) == "id":
                    body_names.add(T.t(i))
                elif T.k(i) == "pp":   # a pragma's clauses use run values
                    body_names |= set(re.findall(r"[A-Za-z_]\w*", T.t(i)))
            for kind, (a, b) in binds:
                ds = nc.parse_declaration(T, a, b)
                nm = ds[0].name if ds else T.t(a + 2)
                rest = set()
                for k2, (a2, b2) in binds:
                    if a2 > a:
                        rest |= {T.t(x) for x in range(a2, b2) if T.k(x) == "id"}
                if nm not in body_names and nm not in rest:
                    self.fail("V3", f"{fn.f.name}: binding `{nm}` is never used")
        if not bad:
            self.note(f"V3 {c['name']}: {n_checked} crossing names keep their frozen category")

    def is_binding(self, fn, d):
        for fb, binds in self.bindings:
            if fb is fn:
                return any(a <= d.tok < b for _, (a, b) in binds)
        return False

    def writes(self, fn, d):
        T = fn.T
        for i in range(fn.o + 1, fn.c):
            if T.t(i) == d.name and T.k(i) == "id" and T.t(i - 1) not in (".", "->", "::") and i != d.tok \
                    and not self.is_binding_tok(fn, i) and is_write(T, i):
                return True
        return False

    def is_binding_tok(self, fn, i):
        for fb, binds in self.bindings:
            if fb is fn:
                return any(a <= i < b for _, (a, b) in binds)
        return False

    def v4(self):
        for name, fn in self.stages.items():
            ds = self.hdr_decl.get(name, [])
            if len(ds) != 1:
                self.fail("V4", f"{name}: {len(ds)} header declarations")
                continue
            f, tk = ds[0]
            if [p.text for p in f.params or []] != [p.text for p in fn.params()]:
                self.fail("V4", f"{name}: header parameters differ from the definition")
            pre = tk.source[max(0, tk[f.start].pos - 900):tk[f.start].pos]
            if "@brief" not in pre or not pre.rstrip().endswith("*/"):
                self.fail("V4", f"{name}: no @brief comment before its declaration")
        self.note(f"V4 {self.case['name']}: {len(self.stages)} stages defined once, declared once, documented")

    def events(self, toks):
        out = []
        n = len(toks)
        for i in range(n):
            t = toks[i]
            if t == "log" and toks[i + 1:i + 2] == ["->"]:
                out.append(("log", toks[i + 2]))
            elif t in ("emitEvent", "prmRespond", "emitEpochStatus", "arcReasonExpand", "arSuspectNote"):
                out.append((t, ""))
            elif t == "trace" and toks[i + 1:i + 2] == ["<<"]:
                out.append(("trace", ""))
            elif t == "tr" or t == "tr2":
                if toks[i + 1:i + 2] == ["("] and toks[i + 2:i + 3] != []:
                    out.append((t, toks[i + 2]))
        return out

    def v5(self, frz, comp):
        a = self.events([x[0] for x in frz])
        b = self.events([x[0] for x in comp])
        if a != b:
            self.fail("V5", f"ordered event/log calls differ ({len(a)} frozen, {len(b)} composed)")
        else:
            self.note(f"V5 {self.case['name']}: {len(a)} ordered log/event/trace calls equal")

    def v6(self):
        if not hasattr(self, "view_init"):
            self.fail("V6", "no view-init statement")
            return
        fn, a, b = self.view_init
        T = fn.T
        names = [T.t(i) for i in range(a + 4, b - 2) if T.t(i) != ","]
        tk = nc.Tokens(self.tree.read(self.case["view_hdr"]), self.case["view_hdr"])
        mem = None
        for i in range(len(tk)):
            if tk.t(i) == "struct" and tk.t(i + 1) == self.case["view"] and tk.t(i + 2) == "{":
                ds = []
                for s, e in nc.statements(tk, i + 3, tk.close(i + 2)):
                    d = nc.parse_declaration(tk, s, e, "range")
                    if d:
                        ds += [x.name for x in d]
                mem = ds
        if mem != names:
            self.fail("V6", f"view-init names {names} differ from the members {mem}")
        else:
            self.note(f"V6 {self.case['name']}: the view-init names the {len(mem)} members in declared order")

    def v7(self):
        for p in self.case["headers"]:
            tx = self.tree.read(p)
            for k, ln in enumerate(tx.split("\n"), 1):
                if "—" in ln or re.search(r"\bcard\b|docs/refound|docs/SPEC", ln):
                    self.fail("V7", f"{p}:{k}: em dash or card citation")


def run_case(case, tree, verbose=False):
    ck = Check(tree, case, verbose)
    try:
        ck.run()
    except Exception as ex:   # noqa: BLE001
        ck.fail("V0", f"{type(ex).__name__}: {ex}")
    return ck


def lines_of(text):
    return text.split("\n")


def find_line(L, stripped, start=0):
    for k in range(start, len(L)):
        if L[k].strip() == stripped:
            return k
    raise AssertionError(f"control anchor missing: {stripped!r}")


def drop_line(text, stripped):
    L = lines_of(text)
    del L[find_line(L, stripped)]
    return "\n".join(L)


def swap_lines(text, first, second):
    L = lines_of(text)
    k = find_line(L, first)
    assert L[k + 1].strip() == second, f"control anchor not adjacent: {second!r}"
    L[k], L[k + 1] = L[k + 1], L[k]
    return "\n".join(L)


def edit_line(text, stripped, new, start_after=None):
    L = lines_of(text)
    s = next(k for k, ln in enumerate(L) if ln.startswith(start_after)) if start_after else 0
    k = find_line(L, stripped, s)
    L[k] = L[k][:len(L[k]) - len(L[k].lstrip())] + new
    return "\n".join(L)


def controls(verbose):
    base = Tree()
    scan = "src/core/network/slip/PassASlipScan.cpp"
    sdr = "src/core/network/ar/SdEwlRows.cpp"
    asm_, st, sd = base.read(ASM), base.read(scan), base.read(sdr)
    fold = "void passAFoldMwAdmission("
    pairs = [
        ("swapped line", "V1",
         {scan: swap_lines(st, "ctx.counters.n_mw_admit_legacy         += ac.legacy_admitted;",
                           "ctx.counters.n_mw_evidence_admitted    += ac.evidence_admitted;")}),
        ("dropped line", "V1", {scan: drop_line(st, "ctx.counters.n_mw_drop_loose           += ac.drop_loose;")}),
        ("const on a written binding", "V3",
         {scan: edit_line(st, "NetworkRunContext& ctx = v.ctx;", "const NetworkRunContext& ctx = v.ctx;", fold)}),
        ("reference became a copy", "V3",
         {scan: edit_line(st, "PassABuffers& pa = v.pa;", "PassABuffers pa = v.pa;", "void passAMwAdmit(")}),
        ("stage calls swapped", "V1",
         {ASM: swap_lines(asm_, "passAFoldPresent(v);        // present[] and the thrown-station log, serial",
                          "passAFoldDropReasons(v);    // elevation-mask and no-model reasons into the arc ledger")}),
        ("continue became break", "V1",
         {scan: edit_line(st, "if (r.sig1.empty() || r.sig2.empty()) continue;",
                          "if (r.sig1.empty() || r.sig2.empty()) break;")}),
        ("sd-ewl dropped line", "V1", {sdr: drop_line(sd, "ctr.rows_built += mc;")}),
        ("sd-ewl type text", "V3",
         {sdr: sd.replace("const std::vector<SdEwlRow>& rows", "const std::vector<SdEwlArc>& rows", 1)}),
    ]
    ok = True
    for label, chk, over in pairs:
        res = []
        for case in CASES:
            res += run_case(case, Tree(over)).fails
        status = "caught" if res else "MISSED"
        right = any(f[0] == chk for f in res)
        print(f"  control {label:30s} -> {status} by {sorted({f[0] for f in res})} (expected {chk})")
        if verbose and res:
            print("      ", res[0])
        ok = ok and right
    return ok


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--controls", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    ok = True
    for case in CASES:
        ck = run_case(case, Tree(), a.verbose)
        for n in ck.notes:
            print("  ok  ", n)
        for chk, msg in ck.fails:
            print(f"  FAIL {chk} [{case['name']}]: {msg}")
        ok = ok and not ck.fails
    import sdewl_verify   # V8: the per-satellite cut of sdEwlResolveArcs
    sv = sdewl_verify.run_case(sdewl_verify.Tree())
    for n in sv.notes:
        print("  ok  ", "V8 chain:", n)
    for chk, msg in sv.fails:
        print(f"  FAIL V8 [sdewl per-satellite cut] {chk}: {msg}")
    ok = ok and not sv.fails
    if a.controls:
        print("negative controls (each must fail its named check):")
        ok = controls(a.verbose) and ok
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

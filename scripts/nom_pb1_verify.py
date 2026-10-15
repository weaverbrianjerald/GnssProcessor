#!/usr/bin/env python3
"""Relocation verifier of card NOM3 step PB1a (gate G-PB-R, the PB1a subset).

    python scripts/nom_pb1_verify.py [--base 7141240] [--controls] [--verbose]

The frozen source is Application::networkPassB_ in src/core/app/ApplicationNetworkAssembly.cpp
at the base (git show; read by scripts/nom_pb1/frozen.py, lexed by scripts/nom_cpp.py).  The
candidate is the working tree.  The accepted edits are the closed list of
scripts/nom_pb1/declared.py.  Exit 0 only when every check passes.

  PB-V0 names.   `run` and `ep` are not free identifiers of any frozen stage piece (the stage
                 parameters cannot capture a frozen name), and every stage is defined once
                 under src/, in its declared home, and declared once in its home header.
  PB-V1 bodies.  The frozen text after the alias block is partitioned by the declared pieces
                 (only blank lines between them).  Each stage's body below its bindings is its
                 frozen piece, line for line, comments included, modulo GUARD (the declared
                 condition negated into `if (!C) return;`, the enclosed lines shifted exactly
                 four columns, the closing brace dropped) and RETURN (the declared statement
                 appended).  networkPassB_ below its bindings is the frozen text with each stage
                 piece replaced by its declared call line, at the frozen position, plus the
                 declared call-group comments; blank lines are not compared.  So every frozen
                 statement line lands exactly once, in order.
  PB-V2 braces.  Each new function's brackets balance (the lexer's match table) and its body
                 brace closes on its last line.
  PB-V3 bindings. Every statement above a function's body is a declaration that is (a) a frozen
                 alias statement, token for token after KNOB_MOVE; or (b) a frozen root binding
                 realized for the function (`nep.` read through `ep.`; an output group written by
                 the function kept verbatim; one it only reads bound const, through `ep` or its
                 const parameter: CONST_VIEW); or (c) a declared FREE_ROOT.  Each keeps its
                 frozen declarator category: reference, pointer and type text equal, `const` only
                 added (CONST_VIEW).  Every frozen binding name a body or a binding reads is
                 bound above its use (or is a parameter), and no binding is unread.
  PB-V3c staleness. A value binding copied into a stage is evaluated later than in the frozen
                 function; its sources are declared stable, or no frozen statement of PASS B
                 between the alias block and the stage writes them.
  PB-V10 literals. The string and character literals of the changed C++ files are, as a
                 multiset, those of the base (they only move).
  PB-V12 knobs.  The six snapshot statements equal the frozen initializers with every alias
                 name read through its knob (`k.`), each field is declared once, the frozen
                 alias lines read their field, and the harvest's four derivations read their
                 field and had the same conjunct set.
  PB-V13 contract. Each stage has a signature row in tests/test_network_epoch_contract.cpp.

--controls runs the negative controls (mutations of the candidate texts in memory, each of
which must fail its named check) and then the positive run.
"""
import argparse
import collections
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import nom_cpp  # noqa: E402
from nom_pb1 import declared as D  # noqa: E402
from nom_pb1 import frozen  # noqa: E402

ROOT = HERE.parent


# ---------------------------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------------------------
def git_show(rev, rel):
    try:
        return frozen.git_show(ROOT, rev, rel)
    except subprocess.CalledProcessError:
        return None


def joined(text):
    T = nom_cpp.Tokens(text)
    return T.join(0, T.n)


def negate(cond):
    c = cond.strip()
    if re.fullmatch(r"[A-Za-z_]\w*", c):
        return "!" + c
    return "!(" + c + ")"


def find_function(text, name, path):
    """(sig_line, open_line, close_line, Tokens, open_tok, close_tok) of the one definition of
    @p name in @p text; raises when there is not exactly one."""
    T = nom_cpp.Tokens(text, path)
    hits = []
    for i in range(T.n - 1):
        if T.t(i) == name and T.k(i) == "id" and T.t(i + 1) == "(" and T.t(i - 1) not in (".", "->"):
            j = T.match[i + 1]
            if j > 0 and T.t(j + 1) == "{":
                hits.append((i, j + 1))
    if len(hits) != 1:
        raise Check(f"{path}: {len(hits)} definitions of {name}")
    i, o = hits[0]
    c = T.match[o]
    return T.line(i), T.line(o), T.line(c), T, o, c


class Check(Exception):
    pass


class Problems:
    def __init__(self):
        self.p = collections.defaultdict(list)
        self.info = collections.defaultdict(list)

    def add(self, check, msg):
        self.p[check].append(msg)


# ---------------------------------------------------------------------------------------------
# The verifier
# ---------------------------------------------------------------------------------------------
class Verifier:
    CHECKS = ["PB-V0", "PB-V1", "PB-V2", "PB-V3", "PB-V3c", "PB-V10", "PB-V12", "PB-V13"]

    def __init__(self, base, files, frozen_edit=None):
        """@p files: {repo-relative path: candidate text} (read lazily from the tree when absent).
        @p frozen_edit: a control's mutation of the frozen text (never used by a real run)."""
        self.base = base
        self.files = files
        self.pr = Problems()
        f0 = git_show(base, D.ASM)
        if frozen_edit:
            f0 = frozen_edit(f0)
        self.F = frozen.PassB(f0)                       # frozen, unmoved
        self.Fm = frozen.PassB(self.knob_moved(f0))     # frozen, KNOB_MOVE applied
        self.names = {s.name for s in self.F.roots} | {s.name for s in self.F.aliases} | {"config_"}

    def cand(self, rel):
        if rel not in self.files:
            p = ROOT / rel
            self.files[rel] = p.read_bytes().decode("utf-8", "replace").replace("\r\n", "\n") if p.exists() else None
        return self.files[rel]

    def knob_moved(self, text):
        P = frozen.PassB(text)
        for name, old, new, _ in D.KNOB_MOVE:
            s = next(x for x in P.aliases if x.name == name)
            a, b = P.T[s.a].pos, P.T[s.b - 1].end
            seg = text[a:b]
            if seg.count(old) != 1:
                raise Check(f"KNOB_MOVE {name}: frozen initializer not found once")
            text = text[:a] + seg.replace(old, new) + text[b:]
            P = frozen.PassB(text)
        return text

    # -- the new functions -----------------------------------------------------------------
    def new_function(self, name, rel):
        text = self.cand(rel)
        if text is None:
            raise Check(f"{rel}: missing")
        sig, op, cl, T, o, c = find_function(text, name, rel)
        lines = text.split("\n")
        return dict(text=text, lines=lines, sig=sig, open=op, close=cl, T=T, o=o, c=c,
                    body=lines[op:cl - 1], rel=rel)

    # -- PB-V0 -----------------------------------------------------------------------------
    def v0(self):
        P = self.F
        for name, home, *_ in D.STAGES:
            p = self.piece_of(name)
            a, b = P.piece_tokens(p)
            bad = {n for n, _ in frozen.free_ids(P.T, a, b)} & {"run", "ep"}
            if bad:
                self.pr.add("PB-V0", f"{name}: {sorted(bad)} free in the frozen piece")
            # One definition under src/, in its home; one declaration in its home header.
            hits = []
            for f in sorted((ROOT / "src").rglob("*.cpp")):
                rel = f.relative_to(ROOT).as_posix()
                t = self.cand(rel)
                if t and re.search(rf"\b{name}\s*\(", t):
                    try:
                        find_function(t, name, rel)
                        hits.append(rel)
                    except Check:
                        pass
            if hits != [f"src/core/filter/{home}.cpp"]:
                self.pr.add("PB-V0", f"{name}: defined in {hits}, declared home {home}")
            h = self.cand(f"include/filter/{home}.hpp") or ""
            n_decl = len(re.findall(rf"^\w[\w:<>&\s]*\b{name}\(NetworkRun& run[^;{{]*\);", h, re.M))
            if n_decl != 1:
                self.pr.add("PB-V0", f"{name}: {n_decl} declarations in include/filter/{home}.hpp")
        self.pr.info["PB-V0"].append(f"{len(D.STAGES)} stages: names free, homes as declared")

    def piece_of(self, name):
        return next(p for p in self.F.pieces if p["name"] == name)

    # -- PB-V1 -----------------------------------------------------------------------------
    def expected_stage_body(self, name, cond, ret):
        P = self.F
        lines = P.piece_lines(self.piece_of(name))
        if cond:
            ci = [k for k, l in enumerate(lines) if l.strip() and not frozen.is_comment(l)]
            h, t = ci[0], ci[-1]
            if lines[h] != f"    if ({cond}) {{" or lines[t] != "    }":
                raise Check(f"{name}: the frozen piece is not one `if ({cond}) {{...}}`")
            inner = [l[4:] if l.strip() else l for l in lines[h + 1:t]]
            lines = lines[:h] + [f"    if ({negate(cond)}) return;"] + inner + lines[t + 1:]
        if ret:
            lines = lines + [f"    {ret}"]
        return lines

    def v1(self):
        P = self.F
        L = P.lines
        # Partition: the declared pieces cover the text after the alias block, blank lines between.
        starts = {**{s[3]: s[0] for s in D.STAGES}, **{v: k for k, v in D.KEPT.items()}}
        for k, p in enumerate(P.pieces):
            if p["start"] not in starts:
                self.pr.add("PB-V1", f"piece {p['piece']} is not declared")
            nxt = P.pieces[k + 1]["lo"] if k + 1 < len(P.pieces) else P.close_line
            gap = [l for l in L[p["hi"]:nxt - 1] if l.strip()]
            if gap:
                self.pr.add("PB-V1", f"text between piece {p['piece']} and the next: {gap[:2]}")
        if len(P.pieces) != len(starts):
            self.pr.add("PB-V1", "the pieces and the declared rows differ in number")
        # Each stage: its body ends with the transformed frozen piece; above it only bindings.
        self.stage_fns = {}
        for name, home, params, start, cond, ret, call in D.STAGES:
            try:
                fn = self.new_function(name, f"src/core/filter/{home}.cpp")
            except Check as e:
                self.pr.add("PB-V1", str(e))
                continue
            exp = self.expected_stage_body(name, cond, ret)
            body = fn["body"]
            if len(body) < len(exp) or body[len(body) - len(exp):] != exp:
                n = sum(1 for a, b in zip(reversed(body), reversed(exp)) if a == b)
                self.pr.add("PB-V1", f"{name}: the body is not its frozen piece (matches the last "
                                     f"{n} of {len(exp)} lines)")
            fn["pre"] = body[:len(body) - len(exp)]
            fn["exp"] = exp
            fn["params"] = params
            sigtext = re.sub(r"\s+", " ", " ".join(fn["lines"][fn["sig"] - 1:fn["open"]]))
            want = "(" + ", ".join(["NetworkRun& run", "const NetworkEpoch& ep"] + params) + ")"
            if want not in sigtext:
                self.pr.add("PB-V1", f"{name}: parameter list is not {want}")
            self.stage_fns[name] = fn
        # networkPassB_: the frozen text with the stage pieces replaced by their calls.
        try:
            nb = self.new_function("networkPassB_", D.ASM)
        except Check as e:
            self.pr.add("PB-V1", str(e))
            return
        body = nb["body"]
        end = next((k for k, l in enumerate(body) if l.strip() == frozen.EP1_END), None)
        first = next(k for k, l in enumerate(body) if l.startswith("    " + D.KEPT["S02"]))
        nb["pre"] = body[:first]
        rest = [l for l in body[first:] if l.strip()]
        exp, calls = [], {s[0]: s[6] for s in D.STAGES}
        for p in P.pieces:
            if p["name"] is None:
                exp += [l for l in P.piece_lines(p) if l.strip()]
            else:
                exp.append("    " + calls[p["name"]])
        got = [l for l in rest if l.strip() not in D.CALL_COMMENTS]
        if got != exp:
            k = next((i for i, (a, b) in enumerate(zip(got, exp)) if a != b), min(len(got), len(exp)))
            self.pr.add("PB-V1", f"networkPassB_: differs from the frozen text at its line {k}: "
                                 f"{got[k] if k < len(got) else None!r} vs {exp[k] if k < len(exp) else None!r}")
        if len(rest) - len(got) > len(D.CALL_COMMENTS):
            self.pr.add("PB-V1", "networkPassB_: a declared call comment repeats")
        self.passb = nb
        self.pr.info["PB-V1"].append(
            f"{len(P.pieces)} pieces partition the frozen text; {len(self.stage_fns)} stage bodies and "
            f"networkPassB_ ({len(exp)} non-blank lines) carried as declared")

    # -- PB-V2 -----------------------------------------------------------------------------
    def v2(self):
        n = 0
        for name, fn in list(self.stage_fns.items()) + [("networkPassB_", getattr(self, "passb", None))]:
            if fn is None:
                continue
            T, o, c = fn["T"], fn["o"], fn["c"]
            depth = 0
            for i in range(o, c + 1):
                t = T.t(i)
                if T.k(i) == "op" and t in "{}()[]" and len(t) == 1:
                    if T.match[i] < 0:
                        self.pr.add("PB-V2", f"{name}: unmatched {t!r} at line {T.line(i)}")
                        break
                    depth += 1 if t in "{([" else -1
                    if depth <= 0 and i != c:
                        self.pr.add("PB-V2", f"{name}: the body closes early at line {T.line(i)}")
                        break
            if fn["lines"][fn["close"] - 1] != "}":
                self.pr.add("PB-V2", f"{name}: the closing brace is not alone on its line")
            n += 1
        self.pr.info["PB-V2"].append(f"{n} functions balanced")

    # -- PB-V3 -----------------------------------------------------------------------------
    def frozen_decl(self, s, P):
        return s.decl, P.T.join(*s.decl.type_span) if s.decl.type_span else ""

    def v3(self):
        Fm = self.Fm
        alias_by = {joined("\n".join(s.text(Fm, with_comment=False))): s for s in Fm.aliases}
        roots = {s.name: s for s in Fm.roots}
        n_bind = n_const = 0
        targets = list(self.stage_fns.items())
        if getattr(self, "passb", None):
            targets.append(("networkPassB_", self.passb))
        for name, fn in targets:
            is_orch = name == "networkPassB_"
            params = fn.get("params", [])
            pnames = {"run", "nep" if is_orch else "ep"} | {p.split()[-1] for p in params}
            if is_orch:
                pnames |= {"pass_a", "assembly"}
            pre = "\n".join(fn["pre"])
            T = nom_cpp.Tokens(pre)
            bound, order = {}, []
            for s, e in nom_cpp.statements(T, 0, T.n):
                code = T.join(s, e)
                decls = nom_cpp.parse_declaration(T, s, e)
                if not decls:
                    self.pr.add("PB-V3", f"{name}: a statement above the body is not a declaration: {code[:60]!r}")
                    continue
                d = decls[0]
                ok, why = self.classify(code, d, T, alias_by, roots, params, is_orch)
                if not ok:
                    self.pr.add("PB-V3", f"{name}: binding `{code[:90]}` {why}")
                if why == "const-view":
                    n_const += 1
                if d.name in bound:
                    self.pr.add("PB-V3", f"{name}: `{d.name}` bound twice")
                # The names it reads are bound above it.
                for n_, _ in frozen.free_ids(T, s, e):
                    if n_ != d.name and n_ in self.names and n_ not in bound and n_ not in pnames \
                            and n_ != "log_manager_" and not (is_orch and n_ == "config_"):
                        self.pr.add("PB-V3", f"{name}: binding {d.name} reads {n_} before it is bound")
                bound[d.name] = code
                order.append(d.name)
                n_bind += 1
            # Every frozen binding name the body (and the bindings) read is bound; none unread.
            btext = "\n".join(fn["exp"]) if "exp" in fn else "\n".join(
                fn["body"][len(fn["pre"]):])
            BT = nom_cpp.Tokens(btext)
            used = {n_ for n_, _ in frozen.free_ids(BT, 0, BT.n)}
            for n_ in sorted(used & self.names):
                if n_ not in bound and n_ not in pnames and not (is_orch and n_ == "config_"):
                    self.pr.add("PB-V3", f"{name}: `{n_}` is read but not bound")
            reads_by_bindings = set()
            for n_, code in bound.items():
                TT = nom_cpp.Tokens(code)
                reads_by_bindings |= {x for x, _ in frozen.free_ids(TT, 0, TT.n) if x != n_}
            for n_ in order:
                if n_ not in used and n_ not in reads_by_bindings:
                    self.pr.add("PB-V3", f"{name}: `{n_}` is bound but never read")
            fn["bound"] = bound
        self.pr.info["PB-V3"].append(f"{n_bind} bindings in {len(targets)} functions; {n_const} CONST_VIEW")

    def classify(self, code, d, T, alias_by, roots, params, is_orch):
        if code in alias_by:
            return True, "alias"
        if not is_orch and d.name in D.FREE_ROOTS and code == joined(D.FREE_ROOTS[d.name]):
            return True, "free-root"
        if d.name not in roots:
            return False, "is neither a frozen alias, a frozen root nor a declared root"
        s = roots[d.name]
        fcode = s.code
        if is_orch:
            return (code == fcode, "root" if code == fcode else "differs from the frozen root")
        # Frozen form: <decl> name = <group>.<path>;
        m = re.match(r"^(.*?)\b" + re.escape(d.name) + r"=(\w+)\.(.*);$", fcode)
        if not m:
            return False, "has an unreadable frozen root"
        fdecl, group, path = m.group(1), m.group(2), m.group(3)
        writes = any(p == f"EpochPassA& {group}" or p == f"EpochAssembly& {group}" for p in params)
        const_param = any(p == f"const EpochAssembly& {group}" for p in params)
        cdecl = fdecl if fdecl.startswith("const ") else "const " + fdecl
        if group == "nep":
            want, kind = f"{fdecl}{d.name}=ep.{path};", "root"
        elif group == "run":
            want, kind = fcode, "root"
        elif writes:
            want, kind = fcode, "root"
        elif const_param:
            want, kind = f"{cdecl}{d.name}={group}.{path};", "const-view"
        elif group == "pass_a":
            want, kind = f"{cdecl}{d.name}=ep.{group}.{path};", "const-view"
        else:
            return False, f"reads the {group} group the caller writes, not through a parameter"
        # Category: reference / pointer equal, const only added.
        TT = nom_cpp.Tokens(fcode)
        fd = nom_cpp.parse_declaration(TT, 0, TT.n)[0]
        if (fd.is_ref, fd.is_ptr) != (d.is_ref, d.is_ptr) or (fd.is_const and not d.is_const):
            return False, "changes the declarator category"
        if kind != "const-view" and fd.is_const != d.is_const:
            return False, "changes const outside CONST_VIEW"
        return (code == joined(want), kind if code == joined(want) else f"is not {want!r}")

    # -- PB-V3c ----------------------------------------------------------------------------
    def v3c(self):
        P = self.F
        n_val = n_stable = 0
        for name, fn in self.stage_fns.items():
            lo = P.alias_end
            hi = self.piece_of(name)["lo"]
            between = "\n".join(P.lines[lo:hi - 1])
            for n_, code in fn.get("bound", {}).items():
                TT = nom_cpp.Tokens(code)
                d = nom_cpp.parse_declaration(TT, 0, TT.n)[0]
                if d.is_ref:
                    continue
                n_val += 1
                init = code.split("=", 1)[1]
                chains = re.findall(r"\b(?:ctx|config_|ep|run|setup)(?:\.\w+)+", init)
                bare = [x for x, _ in frozen.free_ids(TT, 0, TT.n)
                        if x != n_ and x in self.names and x not in ("ctx", "config_", "setup")]
                if chains and all(c.startswith(D.STABLE_SOURCES) or c + "." in D.STABLE_SOURCES
                                  or any(c.startswith(s.rstrip(".")) for s in D.STABLE_SOURCES)
                                  for c in chains) and not bare:
                    n_stable += 1
                    continue
                for c in chains + bare:
                    if re.search(rf"(?<![\w.]){re.escape(c)}\s*(?:[-+*/|&]?=(?!=)|\+\+|--)", between) \
                            or re.search(rf"(?:\+\+|--){re.escape(c)}\b", between):
                        self.pr.add("PB-V3c", f"{name}: value binding {n_} reads {c}, written before the stage")
        self.pr.info["PB-V3c"].append(f"{n_val} value bindings in the stages, {n_stable} from declared "
                                      f"stable sources, the rest scanned for writes")

    # -- PB-V10 ----------------------------------------------------------------------------
    def v10(self):
        out = subprocess.run(["git", "-C", str(ROOT), "diff", "--name-only", self.base],
                             capture_output=True, text=True).stdout.split()
        out += subprocess.run(["git", "-C", str(ROOT), "ls-files", "--others", "--exclude-standard"],
                              capture_output=True, text=True).stdout.split()
        rels = sorted({r for r in out if r.endswith((".cpp", ".hpp")) and r.startswith(("src/", "include/"))})
        old, new = collections.Counter(), collections.Counter()
        for rel in rels:
            for which, text in (("old", git_show(self.base, rel)), ("new", self.cand(rel))):
                if not text:
                    continue
                T = nom_cpp.Tokens(text, rel)
                c = collections.Counter(T.t(i) for i in range(T.n) if T.k(i) in ("str", "chr"))
                (old if which == "old" else new).update(c)
        for lit in D.LITERAL_DROPS:
            old[lit] -= 1
        if old != new:
            gone = list((old - new).elements())[:3]
            added = list((new - old).elements())[:3]
            self.pr.add("PB-V10", f"literals changed: removed {gone}, added {added}")
        self.pr.info["PB-V10"].append(f"{sum(old.values())} literals in {len(rels)} changed files, moved only")

    # -- PB-V12 ----------------------------------------------------------------------------
    def v12(self):
        kc, kh = self.cand(D.KNOBS_CPP) or "", self.cand(D.KNOBS_HPP) or ""
        fal = {s.name: s for s in self.F.aliases}
        knobmap = {}
        for s in self.F.aliases:
            m = re.match(r"^.*" + re.escape(s.name) + r"=ctx\.knobs\.(\w+);$", s.code)
            if m:
                knobmap[s.name] = m.group(1)
        knobmap.update({n: n for n, *_ in D.KNOB_MOVE})
        snaps = {joined(l) for l in kc.split("\n") if l.strip().startswith("k.")}
        for name, old, new, stmt in D.KNOB_MOVE:
            # The declared snapshot statement is the frozen initializer read through the knobs.
            expr = old.replace("ctx.knobs.", "k.")
            expr = re.sub(r"(?<![\w.])([A-Za-z_]\w*)\b(?!\s*\()",
                          lambda m: "k." + knobmap[m.group(1)] if m.group(1) in knobmap else m.group(1),
                          expr)
            if joined(f"k.{name} = {expr};") != joined(stmt):
                self.pr.add("PB-V12", f"{name}: declared snapshot {stmt!r} is not the frozen {old!r}")
            if joined(stmt) not in snaps:
                self.pr.add("PB-V12", f"{name}: the snapshot statement {stmt!r} is missing")
            if sum(1 for l in kc.split("\n") if re.match(rf"^\s*k\.{name}\s*=", l)) != 1:
                self.pr.add("PB-V12", f"{name}: assigned more than once in the snapshot")
            if len(re.findall(rf"^\s*bool {name}\{{\}};", kh, re.M)) != 1:
                self.pr.add("PB-V12", f"{name}: not declared once in {D.KNOBS_HPP}")
        # The harvest: each derivation reads its knob; its frozen conjunct set equals the snapshot's.
        hv0, hv1 = git_show(self.base, D.HARVEST), self.cand(D.HARVEST) or ""
        expand = {"pl_blk_on": ["pl_local_block", "carry_cpl_cond"]}
        snap_ops = {}
        for name, old, new, stmt in D.KNOB_MOVE:
            ops = [o.strip() for o in stmt.split("=", 1)[1].rstrip(";").split("&&")]
            flat = []
            for o in ops:
                o = o.replace("k.", "")
                flat += expand.get(o, [o])
            snap_ops[name] = set(flat)
        for name in D.HARVEST_MOVE:
            m0 = re.search(rf"const bool\s+{name}\s*=\s*([^;]*);", hv0)
            m1 = re.findall(rf"const bool\s+{name}\s*=\s*([^;]*);", hv1)
            if m1 != [f"ctx.knobs.{name}"]:
                self.pr.add("PB-V12", f"harvest {name}: reads {m1}, not ctx.knobs.{name}")
            ops = {o.strip().replace("ctx.knobs.", "") for o in m0.group(1).split("&&")} if m0 else set()
            if ops != snap_ops[name]:
                self.pr.add("PB-V12", f"harvest {name}: frozen conjuncts {sorted(ops)} vs the knob's "
                                      f"{sorted(snap_ops[name])}")
        self.pr.info["PB-V12"].append(f"{len(D.KNOB_MOVE)} knobs, {len(D.HARVEST_MOVE)} harvest derivations")

    # -- PB-V13 ----------------------------------------------------------------------------
    def v13(self):
        t = self.cand(D.CONTRACT_TEST) or ""
        for name, home, params, *_ in D.STAGES:
            if not re.search(rf"static_cast<\w+>\(&{name}\)", t):
                self.pr.add("PB-V13", f"{name}: no row in {D.CONTRACT_TEST}")
            if f'#include "filter/{home}.hpp"' not in t:
                self.pr.add("PB-V13", f"{name}: {D.CONTRACT_TEST} does not include filter/{home}.hpp")
        self.pr.info["PB-V13"].append(f"{len(D.STAGES)} contract rows")

    def run(self):
        for c in (self.v0, self.v1, self.v2, self.v3, self.v3c, self.v10, self.v12, self.v13):
            try:
                c()
            except (Check, ValueError) as e:
                self.pr.add(c.__name__.upper().replace("V", "PB-V"), str(e))
        return self.pr


# ---------------------------------------------------------------------------------------------
# Negative controls
# ---------------------------------------------------------------------------------------------
def mutate(rel, old, new, count=1):
    def f(files):
        t = files[rel]
        if t.count(old) < 1:
            raise Check(f"control: {old!r} not in {rel}")
        files[rel] = t.replace(old, new, count)
    return f


def move_line(src_rel, line_text, dst_rel, after_text):
    def f(files):
        s = files[src_rel]
        if s.count(line_text + "\n") < 1:
            raise Check(f"control: line not in {src_rel}")
        files[src_rel] = s.replace(line_text + "\n", "", 1)
        d = files[dst_rel]
        files[dst_rel] = d.replace(after_text + "\n", after_text + "\n" + line_text + "\n", 1)
    return f


SB, SS, SH = ("src/core/filter/NetworkStationBuild.cpp", "src/core/filter/NetworkSolveStations.cpp",
              "src/core/filter/NetworkShDevVtec.cpp")
CONTROLS = [
    ("N1", "the tally and the fuse swapped in networkPassB_ (stage order)", "PB-V1",
     [mutate(D.ASM, "    networkAssemblyTally(run, nep, assembly);\n    networkPrefitFuseEpoch(run, nep, assembly);",
             "    networkPrefitFuseEpoch(run, nep, assembly);\n    networkAssemblyTally(run, nep, assembly);")]),
    ("N2", "a binding's declarator kind changed: `auto& stations` copied by value (P5's analog)", "PB-V3",
     [mutate(SB, "    auto& stations = setup.stations;", "    auto stations = setup.stations;")]),
    ("N3", "a binding's root changed: sh_dev_on bound to the de-mean knob (P9's analog)", "PB-V3",
     [mutate(SH, "    const bool   sh_dev_on      = ctx.knobs.sh_dev_on;",
             "    const bool   sh_dev_on      = ctx.knobs.sh_dev_demean;")]),
    ("N4", "a printed literal edited in the consume (P10)", "PB-V10",
     [mutate(SS, "assembly failed ({}); dropping it this epoch\",", "assembly failed ({}); dropping it\",")]),
    ("N5", "the consume's H/state-mismatch warning moved into the tally (P11's analog)", "PB-V1",
     [move_line(SS, "            log->warn(\"Network: station '{}' H/state mismatch ({})\", st.name, R.err);",
                SB, "    networkParamInventoryDrain(ctx.param_inventory, asm_res, present, se.time.week, se.time.tow);")]),
    ("N6", "GUARD not negated in networkShDevCarryAge", "PB-V1",
     [mutate(SH, "    if (!sh_dev_on) return;", "    if (sh_dev_on) return;")]),
    ("N7", "a binding dropped: stations unbound in the in-solve track", "PB-V3",
     [lambda files: files.__setitem__(SS, re.sub(
         r"(void networkSolveStationsTrack\(.*?)\n    auto& stations = setup\.stations;", r"\1",
         files[SS], count=1, flags=re.S))]),
    ("N8", "a brace moved: the eq34 audit closes a block early", "PB-V2",
     [mutate("src/core/filter/NetworkChainDatum.cpp", "    auto& pa = ctx.counters.eq34_pin_audit;\n",
             "    auto& pa = ctx.counters.eq34_pin_audit;\n    }\n    {\n")]),
    ("N9", "a knob's snapshot expression altered (pl_blk_on without the coupling rows)", "PB-V12",
     [mutate(D.KNOBS_CPP, "k.pl_blk_on       = k.pl_local_block && k.carry_cpl_cond;",
             "k.pl_blk_on       = k.pl_local_block;")]),
    ("N10", "the harvest's ZWD derivation reads the wrong knob", "PB-V12",
     [mutate(D.HARVEST, "zwd_blk_on  = ctx.knobs.zwd_blk_on;", "zwd_blk_on  = ctx.knobs.iono_blk_on;")]),
    ("N11", "a stage's contract row removed", "PB-V13",
     [mutate(D.CONTRACT_TEST, "static_cast<SigEq34PinAudit>(&networkEq34PinAudit)",
             "static_cast<SigEq34PinAudit>(&networkEq34PinAuditX)")]),
    ("N12", "a value binding evaluated after a frozen write to its source: the tally binds the "
            "frozen alias zwd_seam_live while the frozen produce writes zwd_seam_ep_left", "PB-V3c",
     [mutate(SB, "    auto& n_cpl_row_dump = ctx.counters.n_cpl_row_dump;",
             "    auto& n_cpl_row_dump = ctx.counters.n_cpl_row_dump;\n"
             "    const bool   zwd_seam_live = ctx.carry.zwd_seam_ep_left > 0;")],
     lambda t: t.replace("    if (carry_cpl_tu || carry_cpl_reinit) {",
                         "    ctx.carry.zwd_seam_ep_left = 0;\n    if (carry_cpl_tu || carry_cpl_reinit) {", 1)),
]


def run_controls(base, verbose):
    names = [D.ASM, SB, SS, SH, "src/core/filter/NetworkChainDatum.cpp", D.KNOBS_CPP, D.HARVEST,
             D.CONTRACT_TEST]
    fired = 0
    for cid, what, check, muts, *fe in CONTROLS:
        files = {}
        v = Verifier(base, files)
        for n in names:
            v.cand(n)
        try:
            for m in muts:
                m(files)
        except Check as e:
            print(f"  [ERROR] {cid}: {e}")
            continue
        res = Verifier(base, files, frozen_edit=fe[0] if fe else None).run()
        ok = bool(res.p.get(check))
        fired += ok
        print(f"  [{'fired' if ok else 'MISSED'}] {cid} {check}: {what}")
        if verbose and ok:
            print("        " + res.p[check][0][:150])
    return fired


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--base", default=D.BASE)
    ap.add_argument("--controls", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)
    rc = 0
    if a.controls:
        print("negative controls:")
        fired = run_controls(a.base, a.verbose)
        print(f"  {fired}/{len(CONTROLS)} fired")
        if fired != len(CONTROLS):
            rc = 1
    pr = Verifier(a.base, {}).run()
    for c in Verifier.CHECKS:
        bad = pr.p.get(c, [])
        print(f"[{'FAIL' if bad else 'pass'}] {c}: " + "; ".join(pr.info.get(c, [])))
        for m in bad[:8]:
            print(f"    {m}")
    if any(pr.p.values()):
        rc = 1
    print("nom_pb1_verify: " + ("PASS" if rc == 0 else "FAIL"))
    return rc


if __name__ == "__main__":
    sys.exit(main())

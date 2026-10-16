"""The transforms of card NOM3 step EP1 (specification section 5.3), applied to the frozen source.

scripts/nom_ep1_cut.py builds one Gen per run.  Gen reads only the frozen base (through the
survey's Frozen: the git tree, the driver, the resolved rows) and produces, for every file it
owns, the frozen text with a set of non-overlapping edits applied once (textops.Edit), plus the
new files; it never reads its own output, so a second run writes nothing.

  T-A   epoch_function(): the lambda processSyncedEpoch becomes the body of
        Application::networkProcessEpoch_, four columns left, under a preamble that binds the
        driver names the body still reads, each line the frozen runNetwork declaration with
        the run's objects re-pointed.
  T-B   the epoch's locals: CTOR (NetworkEpoch ep(se) after the canonical-pair statement), WS
        (the workspace local dropped), ALIAS, SIZED, SCALAR (a local becomes its member, an
        alias kept only while the remaining text reads it), CLOCK (ep.clock stored after the
        three const locals), RESULT (a seam's result stored into its member; the frozen member
        aliases kept while read), ACCESSOR (x_emit is ep.x_emit()), EMITTED (ep.emitted after
        the emit bracket replaces the four counter comparisons).
  T-C   a converted call's argument list becomes (run, ep[, kept...][, outputs...]).
  T-D   seams(): each converted seam's signature, its Doxygen (check V7: the run state and
        the epoch members its preamble binds, read and written, and each former @param text
        under its binding), its preamble (the frozen declarators), and an adapter's epoch form
        (a forwarding definition in a generated region at the end of its source).
  T-E   subBegin / subEnd become networkSubBegin / networkSubEnd over the run.
  T-F   the solve's two callbacks: the driver's closures drop and their invocations become the
        builders' converted calls; the solve's replay calls the builders directly.
  T-G   run_network(): runNetwork's setup edits of specification 4.1 and the cut of the engines'
        constructor arguments into networkEngineSpec (NetworkRun.cpp), the run's construction
        after the traces, the bracket functions, the third-band predicate moved beside the DD
        build.

Every line number in the output is derived at the base; a frozen object that is missing at
the base is reported as a problem, never assumed.
"""
import collections
import hashlib
import re

from nom_cpp import DRIVER_PATH, parse_params, KEYWORDS
from nom_ep1 import model
from nom_ep1 import templates as tpl
from nom_ep1.textops import (removed_mask, Lines, leading_comment, collapse_ws, wrap_text)
from nom_ep1.model import RUN, ADAPTER

EPOCH_CPP = "src/core/app/ApplicationNetworkEpoch.cpp"
RUN_CPP = "src/core/app/NetworkRun.cpp"
RUN_HPP = "include/app/NetworkRun.hpp"
EPOCH_HPP = "include/app/NetworkEpoch.hpp"
GROUPS_HPP = "include/filter/NetworkRunGroups.hpp"
RUNCTX_HPP = "include/app/NetworkRunContext.hpp"
SOLVE_HPP = "include/filter/NetworkSolveGlue.hpp"
SOLVE_CPP = "src/core/filter/NetworkSolveGlue.cpp"
DDB_HPP = "include/filter/NetworkDdPriorBuild.hpp"
DDB_CPP = "src/core/filter/NetworkDdPriorBuild.cpp"
CONTRACT_TEST = "tests/test_network_epoch_contract.cpp"
ENGINE_SPEC_TEST = "tests/test_network_engine_spec.cpp"
CMAKE = "CMakeLists.txt"

WIDTH = 100
SHIFT = 4
SOLVE = "networkSolveGlue"
PREDICATE = "networkIsUncarriedThirdBand"
CALLABLE_LOCAL = "isUncarriedThirdBand"
BRACKETS = {"subBegin": "networkSubBegin", "subEnd": "networkSubEnd"}
TIMER_FIELDS = ("sec_sub_ep0", "sub_wall0", "sub_tmr0")
LAMBDA_ENUM = "LoopSignal"


class GenError(Exception):
    pass


def _sha(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


def comment_block(text, indent, width=WIDTH):
    """@p text as `//` comment lines at @p indent, wrapped to @p width columns; continuation
    lines are indented under the text when it opens with `---- `."""
    first = indent + "// "
    rest = indent + ("//      " if text.startswith("---- ") else "// ")
    return wrap_text(text, width, first, rest)


def pretty(text):
    """A realized binding with spaces around a conditional's ? and :."""
    t = re.sub(r"\s*\?\s*", " ? ", text)
    return re.sub(r"(?<![:\s]):(?![:])\s*", " : ", t).replace("  ", " ")


def rename(text, run_name, ep_name):
    t = re.sub(r"(?<![\w.>])run(?=\.)", run_name, text)
    return re.sub(r"(?<![\w.>])ep(?=\.)", ep_name, t)


def clean_ret(text):
    words = [w for w in re.split(r"\s+", text.strip()) if w]
    drop = {"static", "inline", "extern", "virtual", "constexpr", "friend", "[[nodiscard]]"}
    return " ".join(w for w in words if w not in drop)


def type_of_param(p):
    """A parameter's type text (its declarator without the name or the default)."""
    code = p.code
    if p.name and code.endswith(p.name):
        code = code[:-len(p.name)]
    return code.strip()


class SeamPlan:
    """One converted seam: names, outputs and the text of its new signature and preamble."""

    def __init__(self, rr):
        self.rr = rr
        self.row = rr.row
        self.presolve = False
        self.epoch_writable = False
        self.run_name = "run"
        self.ep_name = "ep"
        self.out_name = {}
        self.outs = []
        self.fallback = set()
        self.ret = ""
        self.comments = collections.defaultdict(list)   # decl param index -> moved comments
        self.preamble = []                              # [(decl text, binding, note)]
        self.call_args = {}                             # site index -> [arg texts]


class Gen:
    def __init__(self, fz, results, mode, stage, target_files=()):
        self.fz = fz
        self.tree = fz.tree
        self.drv = fz.drv
        self.T = fz.drv.T
        self.text = self.T.source
        self.L = Lines(self.text)
        self.mode = mode
        self.stage = stage
        self.target_files = set(target_files)
        self.conv = {rr.row.name: rr for rr in results}
        self.edits = collections.defaultdict(list)
        self.new = {}
        self.problems = []
        self.record = collections.defaultdict(list)
        self.key_res = self.drv.resolver()
        self.solve_idx = next((k for k, c in enumerate(self.drv.calls) if c.name == SOLVE), None)
        if self.solve_idx is None:
            raise GenError(f"{SOLVE} is not called by the frozen lambda")
        self._keys = {}
        self.decl_toks = {d.tok for sc in self.drv.scopes.all for d in sc.decls}
        self.plans = {}
        for name, rr in self.conv.items():
            self.plans[name] = self.plan_seam(rr)
        if SOLVE in self.conv:
            for _, b in model.SOLVE_BUILDERS:
                if b not in self.conv:
                    raise GenError(f"{SOLVE} is converted but {b} is not: the replay calls it directly (T-F)")

    # -----------------------------------------------------------------------------------------
    # Helpers over the frozen driver
    # -----------------------------------------------------------------------------------------
    def key_at(self, j, T=None, scopes=None):
        """The root key the identifier token j denotes in the driver ('D:x', 'L:x', 'B:...'),
        or None (a member access, a type, a namespace, a declaration's own name)."""
        T = T or self.T
        if j in self._keys:
            return self._keys[j]
        out = None
        if T.k(j) == "id" and T.t(j) not in KEYWORDS and T.t(j - 1) not in (".", "->", "::") \
                and T.t(j + 1) != "::" and j not in self.decl_toks:
            d = self.drv.scopes.lookup(T.t(j), j)
            if d is not None and d.tok != j:
                out = self.key_res.key(d)
            elif d is None and T.t(j) in self.drv.members:
                out = "A:" + T.t(j)
        self._keys[j] = out
        return out

    def stmt_line_span(self, s, e, with_comment):
        """Whole-line span of the driver statement [s, e): from its leading comment (when
        with_comment) to the end of the line of its last token."""
        T, L = self.T, self.L
        a_pos = T.toks[s].pos
        a = leading_comment(self.text, a_pos) if with_comment else L.start(a_pos)
        b = L.end(T.toks[e - 1].end)
        if not L.only_code_from(T, s, e - 1):
            raise GenError(f"{DRIVER_PATH}:{T.line(s)}: the statement shares its lines with other code")
        return a, b

    def lambda_body_range(self):
        T, L = self.T, self.L
        lo = L.end(T.toks[self.drv.lam_open].end)
        hi = L.start(T.toks[self.drv.lam_close].pos)
        return lo, hi

    def lam_decl(self, name):
        ds = [d for d in self.drv.lam_scope.decls if d.name == name and d.kind != "param"]
        if len(ds) != 1:
            raise GenError(f"the lambda declares {name} {len(ds)} times at its top level")
        return ds[0]

    def root_decl(self, name):
        ds = [d for d in self.drv.scopes.root.decls if d.name == name]
        if not ds:
            raise GenError(f"runNetwork declares no {name} at its outermost scope")
        return ds[0]

    def top_stmt(self, tok):
        se = self.drv.top_statement_of(tok)
        if se is None:
            raise GenError(f"{DRIVER_PATH}:{self.T.line(tok)}: not inside a top-level lambda statement")
        return se

    def col_after(self, T, j, text=None):
        text = text or T.source
        return T.toks[j].end - Lines(text).start(T.toks[j].pos)

    # -----------------------------------------------------------------------------------------
    # Seam plans
    # -----------------------------------------------------------------------------------------
    def plan_seam(self, rr):
        row = rr.row
        pl = SeamPlan(rr)
        if rr.missing or not rr.bindings or rr.decl is None or rr.defn is None:
            raise GenError(f"row {row.name} is not resolved at the base: {rr.missing or rr.problems}")
        if rr.refusals:
            raise GenError(f"row {row.name} has refusals: {rr.refusals}")
        first = self.drv.calls.index(rr.sites[0])
        pl.presolve = first <= self.solve_idx
        pl.epoch_writable = self.mode == "epoch" and pl.presolve and bool(row.outs)
        if row.mech != ADAPTER:
            pl.fallback = {a for a, _ in rr.fallback}
        pl.run_name = "nrun" if "run" in pl.fallback else "run"
        pl.ep_name = "nep" if "ep" in pl.fallback else "ep"
        pl.out_name = {o: (o + "_out" if o in pl.fallback else o) for o in row.outs}
        pl.outs = [] if pl.epoch_writable else list(row.outs)
        pl.ret = clean_ret(rr.decl.ret)
        # The fallback names must themselves be free in the body.
        if pl.fallback:
            T = self.tree.tokens(rr.defn.path)
            used = {T.t(j) for j in range(rr.defn.body[0], rr.defn.body[1]) if T.k(j) == "id"}
            for n in [pl.run_name, pl.ep_name, *pl.out_name.values()]:
                if n in pl.fallback:
                    continue
                if n in used and n not in ("run", "ep") and n not in row.outs:
                    raise GenError(f"{row.name}: the fallback name {n} is used by the body")
        # Comments inside the frozen call's argument list move to the preamble (spec 3.2).
        c = rr.sites[0]
        T = self.T
        for k, (s, e) in enumerate(c.args):
            nxt = c.args[k + 1][0] if k + 1 < len(c.args) else c.close
            gap = self.text[T.toks[e - 1].end:T.toks[nxt].pos]
            for m in re.finditer(r"//[^\n]*|/\*.*?\*/", gap, re.S):
                ctext = m.group(0)
                body = ctext[2:].strip() if ctext.startswith("//") else " ".join(ctext[2:-2].split())
                # A comment on the next argument's own line belongs to that argument.
                tail = gap[m.end():]
                own = k + 1 < len(c.args) and "\n" not in tail
                pl.comments[k + 1 if own else k].append(body)
            if k == 0:
                gap0 = self.text[T.toks[c.open].end:T.toks[s].pos]
                for m in re.finditer(r"//[^\n]*|/\*.*?\*/", gap0, re.S):
                    ctext = m.group(0)
                    pl.comments[0].append(ctext[2:].strip() if ctext.startswith("//")
                                          else " ".join(ctext[2:-2].split()))
        return pl

    def kept_index(self, pl):
        return [k for k, p in enumerate(pl.rr.decl.params) if p.name in pl.row.kept]

    def single_out_index(self, pl, o):
        for k, b in enumerate(pl.rr.bindings[0]):
            if b.out == o and b.gtext is None:
                return k
        raise GenError(f"{pl.row.name}: no frozen parameter is the single-member output {o}")

    def new_params(self, pl, which):
        """The new parameter declarations of @p pl ('decl': the header; 'def': the source)."""
        rr, row = pl.rr, pl.row
        params = rr.decl.params if which == "decl" else rr.defn.params
        run_name = "run" if which == "decl" else pl.run_name
        ep_name = "ep" if which == "decl" else pl.ep_name
        out = [f"NetworkRun& {run_name}"]
        if row.mech == RUN:
            return out
        out.append(("NetworkEpoch& " if pl.epoch_writable else "const NetworkEpoch& ") + ep_name)
        for k in self.kept_index(pl):
            p = params[k]
            t = collapse_ws(p.text)
            if which == "decl" and p.has_default:
                t += " = " + " ".join(p.default.split())
            out.append(t)
        for o in pl.outs:
            if o in tpl.GROUP_TYPES:
                name = o if which == "decl" else pl.out_name[o]
                out.append(f"{tpl.GROUP_TYPES[o]}& {name}")
            else:
                out.append(collapse_ws(params[self.single_out_index(pl, o)].text))
        return out

    def signature_types(self, pl):
        """The parameter types of the epoch form (the contract test's cast)."""
        rr, row = pl.rr, pl.row
        out = ["NetworkRun&"]
        if row.mech == RUN:
            return out
        out.append("NetworkEpoch&" if pl.epoch_writable else "const NetworkEpoch&")
        for k in self.kept_index(pl):
            out.append(type_of_param(rr.decl.params[k]))
        for o in pl.outs:
            if o in tpl.GROUP_TYPES:
                out.append(tpl.GROUP_TYPES[o] + "&")
            else:
                out.append(type_of_param(rr.decl.params[self.single_out_index(pl, o)]))
        return out

    def call_args(self, pl, site):
        """The converted call's arguments at frozen call site @p site (spec 3.1, T-C)."""
        row = pl.row
        if row.mech == RUN:
            return ["run"]
        args = ["run", "ep"]
        for k in self.kept_index(pl):
            s, e = site.args[k]
            t = collapse_ws(self.T.src(s, e))
            if t is None:
                raise GenError(f"{row.name}: the kept argument at {DRIVER_PATH}:{site.line} holds a line comment")
            args.append(t)
        for o in pl.outs:
            args.append(model.OUTPUTS[o])
        return args

    def bindings_for(self, pl):
        """[(decl index, frozen param, Binding)] of the epoch form, kept and removed included."""
        return list(zip(range(len(pl.rr.decl.params)), pl.rr.decl.params, pl.rr.bindings[0]))

    def binding_text(self, pl, k, b, for_adapter=False):
        """(text, note) a frozen parameter binds to: None text for a kept parameter, a removed
        callback or a single-member output (the parameter itself)."""
        row = pl.row
        dp = pl.rr.decl.params[k]
        if dp.name in row.kept:
            return (dp.name if for_adapter else None), "kept"
        rem = model.REMOVED_BY_PARAM.get((row.name, dp.name))
        if rem is not None:
            return None, rem.cls
        run_name = "run" if for_adapter else pl.run_name
        ep_name = "ep" if for_adapter else pl.ep_name
        if b.out:
            if pl.epoch_writable:
                return rename(b.rendered, run_name, ep_name), "written"
            if b.gtext:
                o, member = b.gtext.split(".", 1)
                name = o if for_adapter else pl.out_name[o]
                return f"{name}.{member}", "written"
            return (pl.rr.decl.params[k].name if for_adapter else None), "output"
        return rename(pretty(b.rendered), run_name, ep_name), "read"

    def preamble_lines(self, pl, indent):
        rr, row = pl.rr, pl.row
        if row.mech == RUN:
            head = ("From the run (card NOM3 step EP1; " + tpl.GEN + ", each line checked by "
                    "scripts/nom_ep1_verify.py).  The seam reads no epoch member.")
        elif pl.epoch_writable:
            head = ("From the run and the epoch (card NOM3 step EP1; " + tpl.GEN + ", each line "
                    "checked by scripts/nom_ep1_verify.py).  Reads and writes bind through `ep` "
                    "(--presolve-writer epoch); writes are marked `// written`.")
        else:
            head = ("From the run and the epoch (card NOM3 step EP1; " + tpl.GEN + ", each line "
                    "checked by scripts/nom_ep1_verify.py).  Reads bind through `ep`; writes bind "
                    "through the output parameters and are marked `// written`.")
        lines = comment_block("---- " + head + " ----", indent)
        if pl.fallback:
            names = sorted(pl.fallback)
            new = [{"run": pl.run_name, "ep": pl.ep_name}.get(n, pl.out_name.get(n, n)) for n in names]
            lines += comment_block("The frozen body declares or uses " + ", ".join(f"`{n}`" for n in names)
                                   + ", so this definition names the parameter"
                                   + ("s " if len(names) > 1 else " ") + ", ".join(new)
                                   + " (specification 3.2).", indent)
        for k, dp, b in self.bindings_for(pl):
            p = rr.defn.params[k]
            text, note = self.binding_text(pl, k, b)
            rem = model.REMOVED_BY_PARAM.get((row.name, dp.name))
            decl_text = collapse_ws(p.text)
            if rem is not None and rem.cls == "CALLABLE":
                cl = self.callable_signature()
                lines.append(f"{indent}{decl_text} =")
                lines.append(f"{indent}    [&{pl.run_name}]({cl[0]}){cl[1]} {{")
                lines.append(f"{indent}        return {PREDICATE}({pl.run_name}.ctx.knobs, "
                             f"{', '.join(cl[2])});")
                lines.append(f"{indent}    }};")
                pl.preamble.append((decl_text, f"[&{pl.run_name}]({cl[0]}){cl[1]} {{ return {PREDICATE}"
                                    f"({pl.run_name}.ctx.knobs, {', '.join(cl[2])}); }}", "CALLABLE"))
                continue
            if text is None:
                continue
            stmt = f"{decl_text} = {text};"
            cm = ""
            if note == "written":
                cm = "  // written"
            if pl.comments.get(k):
                cm = (cm + ";" if cm else "  //") + " " + " ".join(pl.comments[k])
            if len(indent) + len(stmt) <= WIDTH + 12:
                lines.append(indent + stmt + cm)
            else:
                lines.append(f"{indent}{decl_text} ={cm}")
                lines.append(f"{indent}    {text};")
            pl.preamble.append((decl_text, text, note))
        lines.append(indent + "// ---- End of the bindings ----")
        return lines

    # -----------------------------------------------------------------------------------------
    # The third-band predicate (CALLABLE)
    # -----------------------------------------------------------------------------------------
    def callable_decl(self):
        d = self.root_decl(CALLABLE_LOCAL)
        if d.kind != "closure" or d.lambda_scope is None:
            raise GenError(f"{CALLABLE_LOCAL} is not a closure at the base")
        return d

    def callable_signature(self):
        """(parameter text, trailing return text, parameter names) of the frozen closure."""
        T = self.T
        d = self.callable_decl()
        lam = d.lambda_scope
        cc = T.match[lam.start]
        po = cc + 1
        if T.t(po) != "(":
            raise GenError(f"{CALLABLE_LOCAL} has no parameter list")
        pc = T.close(po)
        params = collapse_ws(T.src(po + 1, pc))
        body_open = T.match[lam.end]
        ret = ""
        if T.t(pc + 1) == "->":
            ret = " -> " + T.join(pc + 2, body_open)
        names = [p.name for p in parse_params(T, po)[0]]
        return params, ret, names

    def callable_captures(self):
        """[(name, knob expression)] of the closure's init-captures (ctx.knobs.x -> knobs.x)."""
        T = self.T
        d = self.callable_decl()
        out = []
        for c in d.lambda_scope.decls:
            if c.kind != "capture":
                continue
            init = T.join(*c.init)
            if not init.startswith("ctx.knobs."):
                raise GenError(f"{CALLABLE_LOCAL}: the capture {c.name} = {init} is not a knob")
            out.append((c.name, init[len("ctx."):], T.line(c.tok)))
        return out

    # -----------------------------------------------------------------------------------------
    # Reads over the remaining text
    # -----------------------------------------------------------------------------------------
    def reads(self, edits, lo_tok, hi_tok):
        """{key: [token index]} of the identifiers in [lo_tok, hi_tok) the edited text still
        reads, plus each literal piece's declared reads (index -1)."""
        removed = removed_mask(self.T, edits)
        out = collections.defaultdict(list)
        for j in range(lo_tok, hi_tok):
            if removed[j]:
                continue
            k = self.key_at(j)
            if k is not None:
                out[k].append(j)
        for e in edits:
            for p in e.pieces:
                for r in p.reads:
                    out[r].append(-1)
        return out

"""Declared substitutions and declared non-conversions of card NOM3 step EP1 (spec 5.6, 3.6).

A conversion may change what a name in a moved body denotes only through one of the eleven
closed classes below; anything else is a failure of the relocation verifier.  Each entry names
the frozen object, says why the substitution preserves behaviour, and carries locators instead
of line numbers: the survey (scripts/nom_ep1_cut.py --survey; any tool can call locate()) finds
the frozen file:line at whichever base it is given, and reports an entry whose object is gone
(the REMOVE-key deletion may take the lane layer, FC-1, the PB emit filter and some iono_dw /
sh_health_gate code out before EP1a-1).

  EPOCH_MEMBER     a lambda local's storage is a NetworkEpoch member (24 locals), or, for the
                   three clock values, a copy of the const local stored once.
  OWNER_MOVE       a driver-local engine or group is a run-owned member and the frozen name a
                   reference to it (11).
  SPEC_CUT         a setup statement moves into networkEngineSpec (the ranges of spec 4.1).
  FIELD_MOVE       a field moves to a new owner path, type unchanged: the three hand-off fields
                   into NetworkPostFitHandoff, the three bracket timer vectors into
                   NetworkRunTiming.
  COPY_SOURCE      a const driver copy of a setup field is read from the field (4); its proof
                   (Writers) admits no write to the field outside networkSetup_.
  VALUE_EQUAL      `ctx.counters.n_emitted != fc_n_emitted_before` is ep.emitted; its proof
                   (Writers) admits no writer of the counter the per-epoch body reaches but
                   the emitter, used once in that body.
  ACCESSOR         x_emit is ep.x_emit().
  CALLABLE         the third-band closure is networkIsUncarriedThirdBand.
  CALLBACK_DIRECT  the solve's two replay callbacks are direct builder calls.
  BRACKET          the subBegin / subEnd closures are networkSubBegin / networkSubEnd.
  TYPE_MOVE        LoopSignal is Application::LoopSignal.

Not substitutions, and therefore failures: a change of declarator kind (reference to copy,
copy to reference, adding non-const), a change of root, a change of a kept argument's text, a
member realized at another position, an unlisted body edit.

NON_CONVERSIONS lists the calls of the frozen lambda that keep their text (spec 3.1, 3.6): the
input stage, every call inside an EP2 block (found by an anchor callee, so the block's calls
are derived rather than listed), and the method calls on run objects.  With the Seam rows they
cover every call of the call table exactly once (check V8).

This module does not import model.py: the verifier reads these entries, the generator realizes
through model.py, and the survey checks that the two agree.
"""
import collections
import re
from dataclasses import dataclass

from nom_cpp import (DRIVER_PATH, Ref, Resolver, Scopes, Tokens, calls_in, parse_declaration,
                     statements, stmt_end)

Hit = collections.namedtuple("Hit", "path line end note")
_ASSIGN_OPS = ("=", "+=", "-=", "*=", "/=", "|=", "&=")

SOLVE_HPP = "include/filter/NetworkSolveGlue.hpp"
SOLVE_CPP = "src/core/filter/NetworkSolveGlue.cpp"
DDBUILD_HPP = "include/filter/NetworkDdPriorBuild.hpp"


# ---------------------------------------------------------------------------------------------
# Locators: how an entry finds its frozen object at a base.  locate(fz) takes an object with
# .drv (nom_cpp.Driver) and .tree (nom_cpp.GitTree) and returns a list of Hit (empty: missing).
# ---------------------------------------------------------------------------------------------
def _line_span(T, s, e):
    return T.line(s), T.line(e - 1)


@dataclass(frozen=True)
class DriverDecl:
    """The declaration of @p name in runNetwork's outermost scope; @p expect, when given, is the
    normalized text its initializer must have."""
    name: str
    expect: str = ""

    def locate(self, fz):
        T = fz.drv.T
        out = []
        for d in fz.drv.scopes.root.decls:
            if d.name != self.name:
                continue
            a, b = _line_span(T, *d.stmt)
            note = "runNetwork outermost declaration"
            if self.expect:
                got = T.norm(*d.init) if d.init else ""
                note = f"initializer `{got}`" + ("" if got == self.expect
                                                  else f" (expected `{self.expect}`)")
            out.append(Hit(DRIVER_PATH, a, b, note))
        return out


@dataclass(frozen=True)
class LambdaDecl:
    """The declaration of @p name at the per-epoch lambda's top level."""
    name: str

    def locate(self, fz):
        T = fz.drv.T
        return [Hit(DRIVER_PATH, *_line_span(T, *d.stmt), "lambda top-level declaration")
                for d in fz.drv.lam_scope.decls if d.name == self.name and d.stmt]


@dataclass(frozen=True)
class FieldDecl:
    """Field @p field of class / struct @p struct declared in @p hpp."""
    hpp: str
    struct: str
    field: str

    def locate(self, fz):
        idx = fz.tree.index(self.hpp)
        if idx is None:
            return []
        line = idx.members.get(self.struct, {}).get(self.field)
        return [Hit(self.hpp, line, line, f"{self.struct}::{self.field}")] if line else []


def _def_body(fz, cpp, func):
    idx = fz.tree.index(cpp)
    if idx is None:
        return None, None
    defs = [f for f in idx.funcs if f.name == func and f.is_def]
    return (idx.T, defs[0]) if defs else (idx.T, None)


@dataclass(frozen=True)
class FieldWrites:
    """Assignments `<obj>.<field> = ...` inside the definition of @p func in @p cpp."""
    cpp: str
    func: str
    obj: str
    field: str

    def locate(self, fz):
        T, f = _def_body(fz, self.cpp, self.func)
        if f is None:
            return []
        a, b = f.body
        return [Hit(self.cpp, T.line(j), T.line(j), f"{self.obj}.{self.field} =")
                for j in range(a, b - 3)
                if T.t(j) == self.obj and T.t(j + 1) == "." and T.t(j + 2) == self.field
                and T.t(j + 3) == "=" and T.t(j - 1) not in (".", "->")]


@dataclass(frozen=True)
class FuncCalls:
    """Calls of @p callee (unqualified, not a member access) inside @p func in @p cpp."""
    cpp: str
    func: str
    callee: str

    def locate(self, fz):
        T, f = _def_body(fz, self.cpp, self.func)
        if f is None:
            return []
        return [Hit(self.cpp, c.line, c.line, f"{self.callee}() inside {self.func}")
                for c in calls_in(T, f.body[0] + 1, f.body[1]) if c.name == self.callee]


@dataclass(frozen=True)
class TextIn:
    """The token sequence of @p needle inside the definition of @p func in @p cpp."""
    cpp: str
    func: str
    needle: str

    def locate(self, fz):
        T, f = _def_body(fz, self.cpp, self.func)
        if f is None:
            return []
        want = [t.text for t in Tokens(self.needle).toks]
        a, b = f.body
        return [Hit(self.cpp, T.line(j), T.line(j + len(want) - 1), self.needle)
                for j in range(a, b - len(want))
                if all(T.t(j + q) == w for q, w in enumerate(want))]


@dataclass(frozen=True)
class ParamOf:
    """Parameter @p param of the declaration of @p seam in @p hpp."""
    seam: str
    hpp: str
    param: str

    def locate(self, fz):
        idx = fz.tree.index(self.hpp)
        if idx is None:
            return []
        out = []
        for f in idx.funcs:
            if f.name == self.seam and f.params:
                for p in f.params:
                    if p.name == self.param:
                        ln = idx.T.line(p.name_tok)
                        out.append(Hit(self.hpp, ln, ln, f"{self.seam}({p.code})"))
        return out


@dataclass(frozen=True)
class ExprSites:
    """Occurrences of the expression @p expr (as a token sequence) in the lambda body."""
    expr: str

    def locate(self, fz):
        T = fz.drv.T
        want = [t.text for t in Tokens(self.expr).toks]
        a, b = fz.drv.lam_open + 1, fz.drv.lam_close
        return [Hit(DRIVER_PATH, T.line(j), T.line(j + len(want) - 1), self.expr)
                for j in range(a, b - len(want))
                if all(T.t(j + q) == w for q, w in enumerate(want))]


@dataclass(frozen=True)
class ArgSites:
    """Call-table arguments in the lambda that are exactly the name @p name."""
    name: str

    def locate(self, fz):
        T = fz.drv.T
        return [Hit(DRIVER_PATH, c.line, c.line, f"argument of {c.name}")
                for c in fz.drv.calls for s, e in c.args if e == s + 1 and T.t(s) == self.name]


@dataclass(frozen=True)
class CallSitesOf:
    """Calls of the driver closure @p name inside the lambda body."""
    name: str

    def locate(self, fz):
        T = fz.drv.T
        cs = [c for c in calls_in(T, fz.drv.lam_open + 1, fz.drv.lam_close) if c.name == self.name]
        return [Hit(DRIVER_PATH, c.line, c.line, f"{self.name}({T.norm(c.open + 1, c.close)})")
                for c in cs]


@dataclass(frozen=True)
class DriverStmts:
    """The runNetwork setup statements that build @p names: their declarations, the expression
    statements that assign through them (`carry_cfg.qz = ...;`), and the if-statements whose
    every statement does.  A statement between them that does neither stays where it is; the
    survey prints those with the ranges so the cut's edges can be read."""
    names: tuple

    def _assigns(self, T, s, e):
        if T.t(s) in self.names:
            j = s + 1
            while T.t(j) == "." and T.k(j + 1) == "id":
                j += 2
            return j > s + 1 and T.t(j) in _ASSIGN_OPS
        if T.t(s) == "if":
            pc = T.close(s + 1)
            inner = [(a, b) for a, b in _flatten(T, pc + 1, e)]
            return bool(inner) and all(self._assigns(T, a, b) for a, b in inner)
        return False

    def locate(self, fz):
        T = fz.drv.T
        picked = []
        for s, e in fz.drv.driver_statements():
            if s >= fz.drv.lam_decl.tok:
                break
            ds = parse_declaration(T, s, e) or []
            if any(d.name in self.names for d in ds) or self._assigns(T, s, e):
                picked.append((s, e))
        out = []
        for s, e in picked:
            a, b = _line_span(T, s, e)
            if out and not _between(fz, out[-1].end, a):
                out[-1] = Hit(DRIVER_PATH, out[-1].line, b, out[-1].note)
            else:
                out.append(Hit(DRIVER_PATH, a, b, "cut"))
        return out


def _between(fz, end_line, start_line):
    """True when a code statement of runNetwork lies strictly between two lines."""
    T = fz.drv.T
    for s, e in fz.drv.driver_statements():
        if end_line < T.line(s) < start_line:
            return True
    return False


def _flatten(T, i, e):
    """The leaf statements of the statement starting at @p i (blocks opened)."""
    if i >= e:
        return []
    if T.t(i) == "{":
        out = []
        for a, b in statements(T, i + 1, T.close(i)):
            out.extend(_flatten(T, a, b))
        return out
    return [(i, stmt_end(T, i, e))]


@dataclass(frozen=True)
class LambdaStmtWith:
    """The lambda's top-level statement that holds a call-table call to @p anchor."""
    anchor: str

    def locate(self, fz):
        T = fz.drv.T
        out = []
        for c in fz.drv.calls:
            if c.name == self.anchor:
                se = fz.drv.top_statement_of(c.tok)
                if se and not any(h.line == T.line(se[0]) for h in out):
                    out.append(Hit(DRIVER_PATH, *_line_span(T, *se), f"holds {self.anchor}"))
        return out

    def spans(self, fz):
        out = []
        for c in fz.drv.calls:
            if c.name == self.anchor:
                se = fz.drv.top_statement_of(c.tok)
                if se and se not in out:
                    out.append(se)
        return out


@dataclass(frozen=True)
class LoopBody:
    """Where the per-epoch body is in a tree, for the write proofs: the file and the token range
    of the lambda (the frozen tree, an empty conversion) or of networkProcessEpoch_ (a converted
    tree), and the token spans of runNetwork's own closures that body calls (their bodies run
    inside the loop although they lie outside its range).  @p root names the function that runs
    the loop (Application::runNetwork): a use there, outside the per-epoch body, runs before or
    after the loop, and the walk back through callers stops at it."""
    path: str
    open: int
    close: int
    closures: tuple = ()
    root: str = "runNetwork"

    def holds(self, path, tok):
        return path == self.path and (self.open < tok < self.close
                                      or any(a < tok < b for a, b in self.closures))


def driver_loop(drv):
    """The LoopBody of a nom_cpp.Driver (the frozen lambda and the root closures it calls)."""
    T = drv.T
    called = {c.name for c in calls_in(T, drv.lam_open + 1, drv.lam_close)}
    spans = tuple((T.match[d.lambda_scope.end], d.lambda_scope.end) for d in drv.scopes.root.decls
                  if d.kind == "closure" and d.lambda_scope is not None and d.name in called)
    return LoopBody(DRIVER_PATH, drv.lam_open, drv.lam_close, spans)


def _enclosing_def(tree, path, tok):
    """The innermost function definition of @p path whose body holds token @p tok, or None."""
    idx = tree.index(path)
    best = None
    for f in (idx.funcs if idx else []):
        if f.is_def and f.body and f.body[0] < tok < f.body[1]:
            if best is None or f.body[0] > best.body[0]:
                best = f
    return best


@dataclass(frozen=True)
class Writers:
    """Writes to a member named @p member (`x.member = `, `++x.member`, `x.member += `, ...)
    in every file under src/: the proof obligation of COPY_SOURCE (no write to the setup field
    after networkSetup_ returns) and VALUE_EQUAL (the emitter is the only writer of the
    counter inside the loop).

    locate() lists every write by member name (the survey prints them).  prove() resolves each
    write's object with a scope walk of its enclosing function (a reference declaration is
    followed to its initializer) and keeps the writes whose object matches @p obj, a regular
    expression on the rendered object (`(^|[:.])setup\\.n_threads$`); an object that does not
    resolve is kept.  Each kept write must lie in a function named in @p writers, or, with
    @p preloop, in a function the per-epoch body cannot reach (no use of its name, direct or
    through other functions, inside the loop); a write inside the loop itself fails.  With
    @p once, each writer is used exactly once inside the loop (the store that follows it
    then sees the only in-loop write)."""
    member: str
    obj: str = ""
    writers: tuple = ()
    preloop: bool = False
    once: bool = False
    prefixes: tuple = ("src/",)

    def scan(self, tree):
        """[(path, Tokens, member token, first token of the written expression)]."""
        out = []
        for path in tree.files(*self.prefixes):
            if not path.endswith((".cpp", ".hpp", ".h")):
                continue
            text = tree.read(path)
            if not text or self.member not in text:
                continue
            T = tree.tokens(path)
            for j in range(1, T.n - 1):
                if T.t(j) != self.member or T.k(j) != "id":
                    continue
                nxt, k = T.t(j + 1), j - 1
                if T.t(k) in (".", "->"):
                    while T.t(k) in (".", "->") and T.k(k - 1) == "id":
                        k -= 2
                elif nxt == "=" and (T.k(k) == "id" or T.t(k) in ("&", "*", "&&", ">", ">>", "::")):
                    continue   # a declaration (`auto& n = ...;`, `std::size_t n = 0;`)
                if T.t(k) in ("++", "--") or nxt in _ASSIGN_OPS + ("++", "--"):
                    out.append((path, T, j, k + 1))
        return out

    def locate(self, fz):
        out = []
        for path, T, j, a in self.scan(fz.tree):
            pre = T.t(a - 1) in ("++", "--")
            note = T.src(a - 1, j + 1) if pre else T.src(a, j + 2)
            out.append(Hit(path, T.line(j), T.line(j), note))
        return out

    def written_object(self, tree, path, T, j, a):
        """The rendered object of the write at member token @p j (expression from @p a), and
        the enclosing function; None when no function holds it."""
        f = _enclosing_def(tree, path, j)
        if f is None:
            return None, None
        sc = Scopes(T, f.body[0], f.body[1], f.params)
        try:
            node = Resolver(sc, {id(sc.root): "S"}).resolve(a, j + 1)
        except (ValueError, IndexError):
            return None, f
        return (node.render() if isinstance(node, Ref) else None), f

    def prove(self, tree, loop):
        """(problems, notes) of the proof on @p tree, the per-epoch body being @p loop."""
        problems, notes = [], []
        pat = re.compile(self.obj) if self.obj else None
        kept = 0
        for path, T, j, a in self.scan(tree):
            obj, f = self.written_object(tree, path, T, j, a)
            where = f"{path}:{T.line(j)} `{T.src(a, j + 1)}`"
            if obj is not None and pat is not None and not pat.search(obj):
                notes.append(f"{where}: another object ({obj})")
                continue
            kept += 1
            if loop.holds(path, j):
                problems.append(f"{where}: a write inside the per-epoch body")
                continue
            if f is None:
                problems.append(f"{where}: a write outside any function definition")
                continue
            if f.name in self.writers:
                notes.append(f"{where}: in {f.name}, the declared writer")
                continue
            if not self.preloop:
                problems.append(f"{where}: written in {f.name}, not in {', '.join(self.writers)}")
                continue
            chain = reaches_loop(tree, f.name, loop)
            if chain:
                problems.append(f"{where}: written in {f.name}, which the per-epoch body reaches "
                                f"({' <- '.join(chain)})")
            else:
                notes.append(f"{where}: in {f.name}, which the per-epoch body does not reach")
        if self.once:
            T = tree.tokens(loop.path)
            for w in self.writers:
                uses = [j for j in range(loop.open + 1, loop.close) if T.t(j) == w and T.k(j) == "id"]
                if len(uses) != 1:
                    problems.append(f"{w} is used {len(uses)} times in the per-epoch body (once expected)")
        notes.append(f"{kept} writes of the object, {len(problems)} outside the proof")
        return problems, notes


def reaches_loop(tree, name, loop, limit=400):
    """The call chain [name, caller, ...] by which the per-epoch body @p loop uses a function
    named @p name (any use of the name, call or not, directly or through other functions under
    src/), or [] when it does not.  Names are compared unqualified, so a same-named function
    elsewhere counts as a caller (a conservative over-approximation)."""
    files = [p for p in tree.files("src/", "include/") if p.endswith((".cpp", ".hpp", ".h"))]
    seen, frontier = {name}, [[name]]
    while frontier and len(seen) < limit:
        chain = frontier.pop()
        n = chain[-1]
        for p in files:
            text = tree.read(p)
            if not text or n not in text:
                continue
            T = tree.tokens(p)
            idx = tree.index(p)
            own = {f.name_tok for f in idx.funcs if f.name == n}
            for j in range(T.n):
                if T.t(j) != n or T.k(j) != "id" or j in own:
                    continue
                if loop.holds(p, j):
                    return chain + [f"{p}:{T.line(j)}"]
                f = _enclosing_def(tree, p, j)
                if f is None:
                    return chain + [f"{p}:{T.line(j)} (a use outside any function: not proved)"]
                if f.name in seen or f.name == loop.root:
                    continue
                seen.add(f.name)
                frontier.append(chain + [f.name])
    if frontier:
        return [name, f"(more than {limit} callers: not proved)"]
    return []


# ---------------------------------------------------------------------------------------------
# The classes and their entries
# ---------------------------------------------------------------------------------------------
CLASSES = {
    "EPOCH_MEMBER": "a lambda local's storage is a NetworkEpoch member (or, for the three clock "
                    "values, a copy of the const local stored once); proved per member by V3c",
    "OWNER_MOVE": "a driver-local engine or group is a run-owned member and the frozen name a "
                  "reference to it; constructed once, at the run's construction, with arguments "
                  "equal by SPEC_CUT; nothing between the old and the new construction point "
                  "reads it",
    "SPEC_CUT": "a setup statement moved into networkEngineSpec, character-exact; proved by V12",
    "FIELD_MOVE": "a field moved to a new owner path, type text unchanged, no frozen use of the "
                  "old path left",
    "COPY_SOURCE": "a const driver copy of a setup field is read from the field; proof: no write "
                   "to the field outside networkSetup_ (Writers: a write scan over src/, each "
                   "write's object resolved)",
    "VALUE_EQUAL": "an expression replaced by an epoch value equal to it at every reader; proof: "
                   "the single writer of ctx.counters.n_emitted inside the loop is the emitter, "
                   "and no call between the store and the last reader reaches it",
    "ACCESSOR": "x_emit is ep.x_emit(), whose expression resolves to the frozen initializer under "
                "EPOCH_MEMBER",
    "CALLABLE": "the third-band closure is networkIsUncarriedThirdBand: body character-exact, "
                "init-captures as const preamble lines, the DD build's std::function parameter "
                "replaced by a preamble lambda forwarding to it",
    "CALLBACK_DIRECT": "the solve's two callbacks are direct calls whose arguments resolve, at the "
                       "replay, to the objects the closures captured",
    "BRACKET": "the two timing closures are functions, bodies character-exact; each call site "
               "gains `run`",
    "TYPE_MOVE": "LoopSignal is Application::LoopSignal",
}


@dataclass(frozen=True)
class Declared:
    cls: str
    name: str
    reason: str
    where: tuple

    def locate(self, fz):
        return [(loc, loc.locate(fz)) for loc in self.where]


def _epoch(name, transform, path):
    reasons = {
        "WS": "the lambda's first statement `EpochWorkspace ws;` is dropped; ep.ws is built with "
              "ep (T-B2)",
        "ALIAS": "default-constructed local; becomes a reference to its member at the same line, "
                 "while the remaining text reads it (T-B3)",
        "SIZED": "sized local; becomes a member assignment of the identical constructor "
                 "expression, plus an alias while read (T-B4)",
        "SCALAR": "scalar local whose initializer equals the member's default initializer; "
                  "dropped, an alias while read (T-B5)",
        "CLOCK": "const local kept for the inline blocks; ep.clock stores a copy once, after "
                 "the three clock locals (T-B6)",
        "RESULT": "a seam's result; the driver stores it into the member, the frozen member "
                  "aliases stay while read (T-B7)",
    }
    return Declared("EPOCH_MEMBER", name, f"{path}: {reasons[transform]}", (LambdaDecl(name),))


DECLARED = (
    # EPOCH_MEMBER: 24 locals and the 3 clock values.
    _epoch("ws", "WS", "ep.ws"),
    _epoch("dt", "CLOCK", "ep.clock.dt"),
    _epoch("pl_now_s", "CLOCK", "ep.clock.pl_now_s"),
    _epoch("carry_warm", "CLOCK", "ep.clock.carry_warm"),
    _epoch("carry_slipped_ambs", "ALIAS", "ep.funnel.carry_slipped_ambs"),
    _epoch("carry_slipped_raw", "ALIAS", "ep.funnel.carry_slipped_raw"),
    _epoch("modeled", "SIZED", "ep.pass_a.modeled"),
    _epoch("present", "SIZED", "ep.pass_a.present"),
    _epoch("shared_phase_keys", "ALIAS", "ep.pass_a.shared_phase_keys"),
    _epoch("n_dropped_sats", "SCALAR", "ep.pass_a.n_dropped_sats"),
    _epoch("registered_clock_sats", "ALIAS", "ep.columns.registered_clock_sats"),
    _epoch("registered_code_keys", "ALIAS", "ep.columns.registered_code_keys"),
    _epoch("col_desc", "ALIAS", "ep.columns.col_desc"),
    _epoch("n_clock", "SCALAR", "ep.columns.n_clock"),
    _epoch("n_pb", "SCALAR", "ep.columns.n_pb"),
    _epoch("n_cb", "SCALAR", "ep.columns.n_cb"),
    _epoch("n_present", "SCALAR", "ep.columns.n_present"),
    _epoch("asm_res", "SIZED", "ep.assembly.asm_res"),
    _epoch("est_index_of", "SIZED", "ep.assembly.est_index_of"),
    _epoch("total_pins", "SCALAR", "ep.assembly.total_pins"),
    _epoch("sec_asm_ms_epoch", "SCALAR", "ep.assembly.sec_asm_ms_epoch"),
    _epoch("carry_arc_amb_key", "ALIAS", "ep.dd_build.carry_arc_amb_key"),
    _epoch("carry_arc_amb_key2", "ALIAS", "ep.dd_build.carry_arc_amb_key2"),
    _epoch("sg_", "RESULT", "ep.solve"),
    _epoch("ar_fixes", "RESULT", "ep.ar_fixes"),
    _epoch("fce", "RESULT", "ep.fixed_cycles"),
    _epoch("sp_", "RESULT", "ep.prep"),

    # OWNER_MOVE: the driver's engines and groups, owned by the run.
    *(Declared("OWNER_MOVE", n, f"run.{p}: the driver name becomes `{n}` = a reference to it",
               (DriverDecl(n),))
      for n, p in (("est", "engines.est"), ("carry_driver", "engines.carry_driver"),
                   ("carry_vce_est", "engines.carry_vce"), ("prires_est", "engines.prires"),
                   ("slip_qc", "engines.slip_qc"), ("iono_deweight", "engines.iono_deweight"),
                   ("fc_ppp_utils", "engines.fc_ppp_utils"), ("emit_engines", "emit"),
                   ("lane_run", "lanes"), ("timing", "timing"), ("cl_tally", "closed_loop"))),

    # SPEC_CUT: the engines' constructor arguments (spec 4.1 table).
    Declared("SPEC_CUT", "pl_q_amb, pl_var_floor",
             "the carry-VCE seed rate and floor; their only reader in runNetwork was the VCE "
             "engine's constructor", (DriverStmts(("pl_q_amb", "pl_var_floor")),)),
    Declared("SPEC_CUT", "carry_vce_est",
             "the VCE engine's family list and constructor arguments (spec fields vce_*)",
             (DriverStmts(("carry_vce", "vce_families", "carry_vce_est")),)),
    Declared("SPEC_CUT", "prires_est",
             "the prior-residual census's constructor arguments (spec fields prires_*); the "
             "name becomes a reference (OWNER_MOVE)", (DriverStmts(("prires_est",)),)),
    Declared("SPEC_CUT", "carry_cfg",
             "the DD-carry driver's configuration; carry_invariant and carry_innov_gate, "
             "interleaved, stay; lane_cfg, lane_on and lane_joint move, the cut being their "
             "only reader", (DriverStmts(("carry_cfg", "lane_cfg", "lane_on", "lane_joint")),)),
    Declared("SPEC_CUT", "mw_wl_cfg",
             "the WL-1 layer's configuration; the debug_r_scale warning that follows stays and "
             "reads the spec field", (DriverStmts(("mw_wl_cfg",)),)),
    Declared("SPEC_CUT", "ewl_cfg", "the EWL lane's configuration; the EWL fill reads the spec field",
             (DriverStmts(("ewl_cfg",)),)),
    Declared("SPEC_CUT", "pb_filter_cfg",
             "the phase-bias emission filter's configuration (a spec field)",
             (DriverStmts(("pb_filter_cfg",)),)),
    Declared("SPEC_CUT", "clk_rebase_cfg",
             "the clock emission re-base's configuration (a spec field)",
             (DriverStmts(("clk_rebase_cfg",)),)),
    Declared("SPEC_CUT", "idw, iono_deweight",
             "the deweight gate's configuration copy and constructor arguments; the name "
             "becomes a reference (OWNER_MOVE); release tracking and the two warnings stay",
             (DriverStmts(("idw", "iono_deweight")),)),

    # FIELD_MOVE
    *(Declared("FIELD_MOVE", n, f"NetworkSolveGlueResult::{n} -> NetworkSolveGlueResult::handoff.{n} "
               "(NetworkPostFitHandoff); the solve's epilogue writes result.handoff."
               f"{n}", (FieldDecl(SOLVE_HPP, "NetworkSolveGlueResult", n),
                        FieldWrites(SOLVE_CPP, "networkSolveGlue", "result", n)))
      for n in ("clock_refloat_sats", "retro_flag_sats", "n_arc_refloat_epoch")),
    *(Declared("FIELD_MOVE", n, f"the bracket scratch moves into NetworkRunTiming::{n} (same "
               "initializer, run lifetime)", (DriverDecl(n),))
      for n in ("sec_sub_ep0", "sub_wall0", "sub_tmr0")),

    # COPY_SOURCE
    *(Declared("COPY_SOURCE", n, f"`const ... {n} = setup.{n};` is read as run.setup.{n}",
               (DriverDecl(n, expect=f"setup.{n}"),
                Writers(n, obj=r"(^|[:.])setup\." + n + "$", writers=("networkSetup_",))))
      for n in ("n_threads", "seam_threads", "carry_par", "brdc_rts")),

    # VALUE_EQUAL
    Declared("VALUE_EQUAL", "emitted",
             "ep.emitted, stored once after the emit bracket, replaces the four comparisons",
             (ExprSites("ctx.counters.n_emitted != fc_n_emitted_before"),
              LambdaDecl("fc_n_emitted_before"),
              Writers("n_emitted", obj=r"\.counters\.n_emitted$", writers=("networkEmitStore",),
                      preloop=True, once=True))),

    # ACCESSOR
    Declared("ACCESSOR", "x_emit",
             "`const Eigen::VectorXd& x_emit = fce.emit ? fce.x_held : x_s;` is ep.x_emit(); "
             "solution prep and the emitter bind it", (LambdaDecl("x_emit"), ArgSites("x_emit"))),

    # CALLABLE
    Declared("CALLABLE", "isUncarriedThirdBand",
             "the closure's body moves character-exact into networkIsUncarriedThirdBand beside "
             "the DD build; its three init-captures are knobs",
             (DriverDecl("isUncarriedThirdBand"),
              ParamOf("networkBuildCarryDdPriors", DDBUILD_HPP, "isUncarriedThirdBand"))),

    # CALLBACK_DIRECT (the WCF writer-end counter stays between the two calls)
    Declared("CALLBACK_DIRECT", "buildEnsembleGauges",
             "the replay calls networkBuildEnsembleGauges(run, ep) directly; the closure goes",
             (LambdaDecl("buildEnsembleGauges"),
              ParamOf("networkSolveGlue", SOLVE_HPP, "buildEnsembleGauges"),
              FuncCalls(SOLVE_CPP, "networkSolveGlue", "buildEnsembleGauges"))),
    Declared("CALLBACK_DIRECT", "buildCarryDdPriors",
             "the replay calls networkBuildCarryDdPriors(run, ep, dd_build, funnel) directly, "
             "card WCF's writer-end counter kept before it; the closure goes",
             (LambdaDecl("buildCarryDdPriors"),
              ParamOf("networkSolveGlue", SOLVE_HPP, "buildCarryDdPriors"),
              FuncCalls(SOLVE_CPP, "networkSolveGlue", "buildCarryDdPriors"),
              TextIn(SOLVE_CPP, "networkSolveGlue", "++ctx.counters.we_culprit_withhold.resolves;"))),

    # BRACKET
    *(Declared("BRACKET", n, f"the closure becomes network{n[0].upper() + n[1:]}(run, b); each "
               "call site gains `run`", (DriverDecl(n), CallSitesOf(n)))
      for n in ("subBegin", "subEnd")),

    # TYPE_MOVE
    Declared("TYPE_MOVE", "LoopSignal", "the lambda's local enum becomes Application::LoopSignal",
             (DriverDecl("LoopSignal"),)),
)


# The member order of NetworkEpoch (spec 2.3): the order the stages fill the groups, `ws`
# where the solve, its first writer, runs.  `NetworkEpoch ep(se);` constructs the members in
# this order and the epoch's end destroys them in its reverse, so the layout is declared, not
# left to the generator (check V3c).  Within a group the order is that of the group's
# EPOCH_MEMBER entries above.
EPOCH_LAYOUT = ("se", "clock", "funnel", "pass_a", "columns", "assembly", "dd_build", "ws", "solve",
                "ar_fixes", "fixed_cycles", "prep", "emitted")


def declared(cls=None):
    return [d for d in DECLARED if cls is None or d.cls == cls]


def find(cls, name):
    for d in DECLARED:
        if d.cls == cls and d.name == name:
            return d
    return None


# ---------------------------------------------------------------------------------------------
# Declared non-conversions (check V8)
# ---------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class NonConversion:
    """kind INPUT: @p match is a callee; EP2_BLOCK: @p match is the anchor callee of the block
    (every call inside the lambda's top-level statement holding it is covered); METHOD: @p match
    is a regular expression on the qualified callee."""
    kind: str
    match: str
    label: str
    reason: str

    def covers(self, call, block_spans):
        if self.kind == "INPUT":
            return call.name == self.match
        if self.kind == "METHOD":
            return re.match(self.match, call.name) is not None
        return any(s <= call.tok < e for s, e in block_spans)


NON_CONVERSIONS = (
    NonConversion("INPUT", "networkInjectEpoch", "the input stage",
                  "produces the epoch `ep` is built on (the event injector's scratch copy)"),
    NonConversion("INPUT", "networkCanonicalPairAdmission", "the input stage",
                  "produces the epoch `ep` is built on (the canonical-pair screen's scratch copy)"),
    NonConversion("EP2_BLOCK", "networkElectPhaseAnchors", "the phase-datum election",
                  "an inline block step EP2 moves whole (filter/NetworkPhaseAnchorEpoch); its "
                  "calls take block locals"),
    NonConversion("EP2_BLOCK", "networkAmbFloatApplyDrops", "the bounded drop",
                  "an inline block step EP2 moves whole; its calls take block locals"),
    NonConversion("EP2_BLOCK", "carry_driver.carriedChordValue", "the gap-resume census",
                  "an inline block step EP2 moves whole (slip/NetworkGapResumeCensus)"),
    NonConversion("METHOD", r"^(est|traces|carry_driver)\.\w+$", "a method call",
                  "a method of a run object; its text stays and reads the epoch function's "
                  "aliases"),
    NonConversion("METHOD", r"^ctx\.carry\.(bb|cg)\.beginEpoch$", "a method call",
                  "the census engines' epoch opening; its text stays (step EP3 wraps them)"),
    NonConversion("METHOD", r"^LogManager::setCurrentEpochTime$", "a static call",
                  "binds the log prefix before the injector; its text stays"),
    NonConversion("METHOD", r"^setup\.brdc_apriori->feedStore$", "a method call",
                  "the broadcast a-priori feed; its text stays (step EP2 moves the feed)"),
)


def ep2_blocks(fz):
    """{label: [(start, end) token spans]} of the EP2 blocks at the base."""
    out = {}
    for nc in NON_CONVERSIONS:
        if nc.kind == "EP2_BLOCK":
            out.setdefault(nc.label, []).extend(LambdaStmtWith(nc.match).spans(fz))
    return out

#!/usr/bin/env python3
"""Relocation verifier of card NOM3 step EP1 (gate G-R): checks V1 to V13 of the EP1 specification.

    python scripts/nom_ep1_verify.py --tree DIR [--base REV] [--rows NAME ...] [--seams A,B,...]
        [--presolve-writer {outputs,epoch}] [--manifest PATH] [--controls] [--only V1a,...]
        [--verbose]

Run it with PYTHONIOENCODING=utf-8 on a cp1252 console.  The frozen source <B> is read from
git at --base (default nom_ep1.DEFAULT_BASE), never from a working tree; the candidate is the
source tree at DIR (src/, include/, tests/, CMakeLists.txt), read from disk with CRLF folded
to LF.  Python 3 standard library only, with scripts/nom_cpp.py (lexer, statements,
declarations, scopes, resolver) and scripts/nom_epoch_lint.py (tree reader, comment-free
rendering, checks V4, V10, V11).  Exit 0 only when every check that applies passes (and,
with --controls, every control that can run fires); "n/a" is not a failure.

Independence.  The verifier reads the canonical table (scripts/nom_ep1/rows_*.py: which
seams, their mechanism, outputs, kept parameters) and the closed list of declared
substitutions (scripts/nom_ep1/declared.py: each entry's class, its realization path at the
head of its reason text, its frozen locators), and nothing the generator computes: it imports
neither scripts/nom_ep1_cut.py nor the realization tables of scripts/nom_ep1/model.py.  Every
binding is derived twice, from the frozen source (the frozen call site's argument, resolved
by the scope walk to its storage root) and from the candidate (the new call site, then the
seam's generated binding line or an adapter's forwarding argument, resolved with the seam's
parameters bound to that call site's arguments), and the two must agree in the frozen
vocabulary; a candidate path enters that vocabulary only through a declared substitution
(run.<member> through the NetworkRun construction or an OWNER_MOVE / FIELD_MOVE path;
ep.<path> through an EPOCH_MEMBER / FIELD_MOVE / ACCESSOR path or the epoch's construction
on `se`).  A copy of a const object (a const local, the knobs, a declared COPY_SOURCE field)
counts as the object.  The generator's manifest (--manifest, or every
scripts/nom_ep1/manifest_*.json of the tree) is read for pairing only: an entry
{"name": <seam>, "def": "<path>:<line>"} under "seams" must name the definition the verifier
found itself, else V8 fails; no binding is read from it.

The candidate is one of two shapes, found from the tree itself:
  empty   Application::runNetwork still holds the lambda processSyncedEpoch and no
          networkProcessEpoch_ exists: the frozen tree, or any tree before step EP1a-1.  It
          is verified as an empty conversion: the lambda is the epoch body (no column
          shift), the epoch locals named by declared.py's EPOCH_MEMBER entries stand for the
          epoch (V4), the spec-cut statements are still in runNetwork (V12); the checks that
          need the new types report n/a.
  epoch   Application::networkProcessEpoch_ exists (step EP1a-1 and later); its body is
          the lambda's moved four columns left, after a preamble that ends at the comment
          `// ---- End of the run's names ----`.
--rows names the row files whose seams the tree must have converted (app, solve, emit,
diag, rows_<name>, a path, or all), --seams single seams beyond them; without either no
seam is expected converted (step EP1a-1 and the empty conversion).  Every other row must
keep its frozen declaration and definition.  A converted seam is found by its definition's
first two parameters (NetworkRun&, const NetworkEpoch&); its bindings are the statements
between `// ---- From the run and the epoch` and `// ---- End of the bindings ----`.

The checks (comments and blank lines are ignored throughout, as in scripts/rt_seam_verify.py;
"character-exact" means the code text and its indentation):
  V1  Bodies.  (a) The lambda's top-level statements, in order, against the epoch body's:
      each carried character-exact (shifted), or the old side of a declared transform of
      spec 5.3 checked by rule (the epoch's construction after the canonical-pair statement,
      the dropped workspace, an epoch local's alias / sized store / result store at its
      declaration, the clock store after the three clock locals, ep.emitted after the emit
      bracket, the bracket and converted-call rewrites, the two replay closures).  (b) Each
      converted body below its bindings equals the frozen body, except the solve's two
      replay lines and three epilogue lines; the bindings region holds declarations only.
      (c) Every unconverted row's declaration and definition, and every adapter's explicit
      form, equal frozen (the solve's three epilogue lines excepted from step EP1a-1 on).
      (d) runNetwork equals frozen apart from the spec 4.1 edits, and what those edits took
      out is where the spec puts it: the predicate's and the two brackets' bodies (against
      the frozen closures), the timer scratch and the hand-off fields (types), LoopSignal.
      (e) Every changed code line of the tree (a line diff of src/, include/, tests/ and
      CMakeLists.txt against <B>) lies in a region another check verified, a new file of
      the step, an #include or forward-declaration line, or a declared hunk of step EP1a-1
      (the run's and the engines' structs, the hand-off struct, the build list).
  V2  Signatures: each converted seam has one epoch-form declaration and one definition,
      (NetworkRun&, const NetworkEpoch&, kept..., outs...) as its row says (the card's
      NetworkEpoch& under --presolve-writer epoch, for a writer before the solve); kept
      parameters keep their frozen declaration; after the solve only the hand-off and the
      prepared solution are outputs.
  V3  Bindings.  (a) At every frozen call site of a converted seam paired with its new site
      (callee and occurrence), and at the solve's two replay calls, every frozen parameter's
      binding agrees; the binding line keeps the frozen declarator and carries `// written`
      exactly when it writes the epoch; a kept argument keeps its text; a removed
      std::function parameter is a declared CALLABLE or CALLBACK_DIRECT; adapters' other
      callers keep their text.  (b) Every read of a captured driver name in the epoch body
      resolves to the frozen object; each preamble line declares a captured name, a copy
      only where the frozen name was one.  (c) Each epoch member is realized at its local's
      frozen position, with the local's type and initializer (NetworkEpoch.hpp), and bound
      nowhere before it; EpochClock's members are the clock store's order.  (d) The
      NetworkRun construction passes, for each reference member, a name of runNetwork's
      outermost scope that is the frozen object.  (e) No body reads a free run, ep, nrun or
      nep (the solve's replay calls excepted).  (f) Folded arguments are member reads with
      an optional & or *, a final .get(), ep.x_emit(), literals, conditionals over those,
      or TimeUtils::continuousGpsSeconds of a member read.
  V4  Driver writes into the epoch after the solve (scripts/nom_epoch_lint.py).
  V5  Deep const: no member of NetworkEpoch, or of a project type held by value inside it,
      is a non-const reference, a pointer to non-const, a smart pointer, a std::function or
      a std::reference_wrapper; `se` is the one reference.
  V6  Order: the execution-order events (network* and call-table calls, log calls with
      level and format literal, brackets with their SB_*, returns of LoopSignal, the three
      inserted stores) of the epoch body equal the frozen lambda's, a closure's events
      counted where it is invoked; runNetwork's equal frozen runNetwork's without the lambda
      (networkEngineSpec's call declared).
  V7  Documentation: each converted declaration has a Doxygen block that carries the frozen
      @param texts and names every epoch member its bindings read.
  V8  Completeness: every call of the frozen call table is covered once, by a row or a
      declared non-conversion; every row's call count matches; the contract test lists the
      converted seams; the manifest's pairing agrees.  Declared entries whose frozen object
      is missing at this base are listed.
  V9  No new overload: no translation unit that newly reaches app/NetworkRun.hpp or
      app/NetworkEpoch.hpp calls, in its frozen code, a function name that only the new
      headers it reaches declare; the two headers declare only their types,
      networkEngineSpec and the two brackets.
  V10, V11  Stage order and writer banners (scripts/nom_epoch_lint.py; banner writers not
      yet converted are skipped while the conversion is partial).
  V12 Spec cut: the setup statements that built the engines' constructor arguments are the
      frozen ones, character-exact and in order (in runNetwork for an empty conversion, in
      networkEngineSpec after it, a built local's declaration becoming a reference to its
      spec field or a field initializer); they read only config_, nc, the knobs and their
      own locals, call only the allowlist, and no other statement entered their range; each
      engine constructor argument of NetworkRunEngines and NetworkRun is the frozen
      argument or the spec field that holds it.
  V13 Names: a converted seam whose frozen body declares or reads run, ep or an output's
      name takes that parameter's fallback name (nrun, nep, <output>_out); a seam that
      needs none takes run and ep; no other definition of a row takes those names.

--controls builds each pre-registered negative control of spec 5.7 as a mutation of the
candidate (an overlay: one or two of its files replaced in memory, no disk copy) and runs
every check on it; a control fires when its named check fails (N8 V1a or V3c, N10 V1a or
V3e, N16 V4 or V6, N4 V3a or V10, N13 V2 or V8).  On an empty conversion N5, N6, N7, N8,
N10, N11 and N12 run in their frozen-tree form (spec 7: "N5-N12 fire on synthetic mutations
of <B>"); a control whose target does not exist in the candidate yet (an unconverted seam,
no epoch function) is reported as pending, never as passed.

Not covered here (spec 5.8): run-time behaviour (default construction then assignment,
construction and destruction order, code generation), which gate G-EP's arms cover; the
banner's prose "available from" column, which V10 replaces by each group's first write in
the epoch function.
"""
import argparse
import collections
import difflib
import json
import pathlib
import re
import sys
import time
import traceback

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from nom_cpp import (APPLICATION_HPP, DRIVER_PATH, KEYWORDS, LAMBDA_NAME, SEAM_CALL,  # noqa: E402
                     CallNode, Cond, Copy, Driver, Expr, GitTree, Lit, Opaque, Ref, Resolver,
                     Scopes, Tokens, calls_in, parse_declaration, statements)
from nom_ep1 import DEFAULT_BASE, ROW_MODULES, load_rows, declared  # noqa: E402
from nom_ep1.model import ADAPTER, MEMBER, RUN, Explicit  # noqa: E402  (the row type only)
import nom_epoch_lint as lint  # noqa: E402
from nom_epoch_lint import DiskTree, Sub, render, same_lines  # noqa: E402

CHECKS = ("V1a", "V1b", "V1c", "V1d", "V1e", "V2", "V3a", "V3b", "V3c", "V3d", "V3e", "V3f",
          "V4", "V5", "V6", "V7", "V8", "V9", "V10", "V11", "V12", "V13")
SHIFT = 4                                   # the lambda body moves four columns left
RUN_NAMES_END = re.compile(r"//\s*-{2,}\s*End of the run's names")
NEW_FILES = ("include/app/NetworkRun.hpp", "src/core/app/NetworkRun.cpp",
             "include/app/NetworkEpoch.hpp", "src/core/app/ApplicationNetworkEpoch.cpp",
             "tests/test_network_epoch_contract.cpp", "tests/test_network_engine_spec.cpp")
CONTRACT_TEST = "tests/test_network_epoch_contract.cpp"
RUN_HPP = "include/app/NetworkRun.hpp"
RUN_CPP = "src/core/app/NetworkRun.cpp"
SOLVE_CPP = "src/core/filter/NetworkSolveGlue.cpp"
SOLVE_HPP = "include/filter/NetworkSolveGlue.hpp"
GROUPS_HPP = "include/filter/NetworkRunGroups.hpp"
# The run's reference members and the frozen object each must bind (spec 2.1).
RUN_REFS = collections.OrderedDict([("ctx", "D:ctx"), ("config", "A:config_"), ("nc", "D:nc"),
                                    ("log", "D:log"), ("traces", "D:traces"), ("setup", "D:setup"),
                                    ("log_dir", "A:log_dir_")])
# Group outputs and their parameter types (spec 3.1); single-member outputs keep the frozen
# declarator.
GROUP_TYPES = {"pass_a": "EpochPassA", "funnel": "NetworkSlipFunnel", "columns": "EpochColumns",
               "assembly": "EpochAssembly", "dd_build": "EpochDdBuild",
               "handoff": "NetworkPostFitHandoff"}
# Calls the engine constructor arguments may make (V12): configuration getters, the network
# block, the VCE family parser, iterator reads of a configuration list.
SPEC_CALLS = re.compile(r"^(config_\.\w+|config_\.getNetworkConfig|NetworkCarryVce::parseComponents|"
                        r"[\w.]+\.(begin|end))$")


# ---------------------------------------------------------------------------------------------
# Results
# ---------------------------------------------------------------------------------------------
class Result:
    def __init__(self):
        self.status = {}
        self.problems = collections.defaultdict(list)
        self.info = collections.defaultdict(list)

    def fail(self, chk, msg):
        self.problems[chk].append(msg)

    def note(self, chk, msg):
        self.info[chk].append(msg)

    def na(self, chk, why):
        self.status[chk] = "n/a"
        self.info[chk].append(why)

    def done(self, chk):
        if self.status.get(chk) != "n/a":
            self.status[chk] = "FAIL" if self.problems[chk] else "pass"

    def failed(self):
        return [c for c in CHECKS if self.status.get(c) == "FAIL"]


# ---------------------------------------------------------------------------------------------
# The declared substitutions, as the verifier reads them (scripts/nom_ep1/declared.py)
# ---------------------------------------------------------------------------------------------
TRANSFORMS = {"2": "WS", "3": "ALIAS", "4": "SIZED", "5": "SCALAR", "6": "CLOCK", "7": "RESULT"}


class Substitutions:
    """The closed list of spec 5.6, parsed from declared.py: each entry's realization path is
    the leading `ep.<path>:` / `run.<path>:` of its reason text."""

    def __init__(self):
        self.epoch = collections.OrderedDict()     # local -> (path, transform)
        for d in declared.declared("EPOCH_MEMBER"):
            m = re.match(r"^(ep\.[\w.]+):", d.reason)
            t = re.search(r"\(T-B(\d)\)", d.reason)
            if not m or not t:
                raise SystemExit(f"declared EPOCH_MEMBER {d.name}: no `ep.<path>:` / (T-Bn) in its reason")
            self.epoch[d.name] = (m.group(1), TRANSFORMS[t.group(1)])
        self.accessor = {}
        for d in declared.declared("ACCESSOR"):
            m = re.search(r"is (ep\.\w+\(\))", d.reason)
            self.accessor[d.name] = m.group(1) if m else None
        self.owner = collections.OrderedDict()
        for d in declared.declared("OWNER_MOVE"):
            m = re.match(r"^(run\.[\w.]+):", d.reason)
            if not m:
                raise SystemExit(f"declared OWNER_MOVE {d.name}: no `run.<path>:` in its reason")
            self.owner[d.name] = m.group(1)
        self.field_solve, self.field_timing = {}, {}
        for d in declared.declared("FIELD_MOVE"):
            fd = [w for w in d.where if type(w).__name__ == "FieldDecl"]
            if fd:
                m = re.search(r"-> \w+::([\w.]+)", d.reason)
                self.field_solve[d.name] = (fd[0].struct, m.group(1) if m else d.name)
            else:
                m = re.search(r"NetworkRunTiming::(\w+)", d.reason)
                self.field_timing[d.name] = m.group(1) if m else d.name
        self.copy_sources = [d.name for d in declared.declared("COPY_SOURCE")]
        ve = declared.declared("VALUE_EQUAL")
        self.value_equal = next((w.expr for d in ve for w in d.where if hasattr(w, "expr")), None)
        self.callable = [d.name for d in declared.declared("CALLABLE")]
        self.callbacks = [d.name for d in declared.declared("CALLBACK_DIRECT")]
        self.bracket = [d.name for d in declared.declared("BRACKET")]
        self.type_move = [d.name for d in declared.declared("TYPE_MOVE")]
        self.spec_cut = declared.declared("SPEC_CUT")
        self.paths = {v[0]: k for k, v in self.epoch.items()}
        timing_owner = next((n for n, p in self.owner.items() if p == "run.timing"), None)
        self.run_paths = {p: "D:" + n for n, p in self.owner.items()}
        for n, f in self.field_timing.items():
            if timing_owner:
                self.run_paths[f"run.timing.{f}"] = "D:" + n

    def epoch_local_of(self, path):
        """(local, rest) of the longest declared epoch path that prefixes @p path."""
        best = None
        for p, local in self.paths.items():
            if path == p or path.startswith(p + ".") or path.startswith(p + "["):
                if best is None or len(p) > len(best[0]):
                    best = (p, local)
        if best is None:
            return None, None
        return best[1], path[len(best[0]):]


SUBST = Substitutions()


# ---------------------------------------------------------------------------------------------
# Small helpers over tokens
# ---------------------------------------------------------------------------------------------
def join_text(text):
    T = Tokens(text)
    return T.join(0, len(T))


def norm_subs(T, s, e, subs):
    """Tokens [s, e) joined with minimal spacing, each Sub's range replaced by its text."""
    by = {x.a: x for x in subs}
    out, i, prev_word = [], s, False
    while i < e:
        x = by.get(i)
        if x:
            out.append(x.text)
            i, prev_word = x.b, False
            continue
        t = T[i]
        word = t.kind in ("id", "num")
        if word and prev_word:
            out.append(" ")
        out.append(t.text)
        prev_word = word
        i += 1
    return "".join(out)


def decl_start(T, f):
    """First token of the declaration or definition @p f (its first specifier)."""
    st = getattr(f, "start", None)
    if st is not None:
        return st
    k = f.name_tok - 1
    while k >= 0:
        t = T.t(k)
        if T.k(k) == "pp" or t in (";", "{", "}") or (t == ":" and T.t(k - 1) in ("public", "private", "protected")):
            break
        k -= 1
    return k + 1


def decl_end(T, f):
    """One past the last token of @p f: its body's closing brace, or the `;` of a declaration."""
    if f.is_def:
        return f.body[1] + 1
    k = f.paren[1] + 1
    while k < T.n and T.t(k) != ";":
        k += 1
    return k + 1


def stmt_of(stmts, tok):
    for k, (s, e) in enumerate(stmts):
        if s <= tok < e:
            return k
    return None


def leaf(name):
    return re.split(r"\.|->|::", name)[-1]


def ids_in(T, a, b):
    """Identifier tokens of [a, b) that start a name (not after `.`, `->`, `::`; not a keyword)."""
    for j in range(a, b):
        if T.k(j) == "id" and T.t(j) not in KEYWORDS and T.t(j - 1) not in (".", "->", "::"):
            yield j


def ep_path_tokens(T, j, e):
    """End token of `ep(.id)*` starting at @p j (a final `.x_emit()` included)."""
    k = j + 1
    while k + 1 < e and T.t(k) == "." and T.k(k + 1) == "id":
        if T.t(k + 2) == "(" and T.t(k + 1) == "x_emit" and T.t(k + 3) == ")":
            return k + 4
        if T.t(k + 2) == "(":
            break
        k += 2
    return k


# ---------------------------------------------------------------------------------------------
# The frozen base and the candidate
# ---------------------------------------------------------------------------------------------
class RunNetwork:
    """Application::runNetwork of a tree (any shape): its scope tree (outermost frame D) and,
    when present, the per-epoch lambda (frame L)."""

    def __init__(self, tree):
        self.tree = tree
        self.T = tree.tokens(DRIVER_PATH)
        idx = tree.index(DRIVER_PATH)
        rn = [f for f in idx.funcs if f.qual == "Application::runNetwork" and f.is_def] if idx else []
        if len(rn) != 1:
            raise SystemExit(f"nom_ep1_verify: Application::runNetwork not found in {tree.rev}")
        self.rn = rn[0]
        self.scopes = Scopes(self.T, self.rn.body[0], self.rn.body[1])
        lam = [d for d in self.scopes.root.decls if d.name == LAMBDA_NAME and d.lambda_scope]
        self.lam = lam[0] if lam else None
        self.frames = {id(self.scopes.root): "D"}
        if self.lam:
            self.lam_scope = self.lam.lambda_scope
            self.lam_close = self.lam_scope.end
            self.lam_open = self.T.match[self.lam_close]
            self.frames[id(self.lam_scope)] = "L"
        app = tree.index(APPLICATION_HPP)
        self.members = set(app.members.get("Application", ())) if app else set()

    def resolver(self, stop=()):
        return Resolver(self.scopes, self.frames, stop, self.members)

    def statements(self):
        return statements(self.T, self.rn.body[0] + 1, self.rn.body[1])

    def lambda_statements(self):
        return statements(self.T, self.lam_open + 1, self.lam_close)


class Frozen:
    """<B>: git tree, the driver (nom_cpp.Driver, the fz of declared.py's locators) and a
    resolver that follows every reference to its storage root."""

    def __init__(self, base):
        self.base = base
        self.tree = GitTree(base)
        self.drv = Driver(self.tree)
        self.rn = RunNetwork(self.tree)
        self.res = self.rn.resolver()
        self.lam_stmts = self.rn.lambda_statements()
        self.calls = self.drv.calls


class CandFz:
    """The candidate as declared.py's locators see it (empty conversion only)."""

    def __init__(self, tree):
        self.tree = tree
        self.drv = Driver(tree)


class Candidate:
    def __init__(self, tree):
        self.tree = tree
        self.rn = RunNetwork(tree)
        self.fn = lint.epoch_function(tree)
        if self.fn is not None:
            self.mode = "epoch"
        elif self.rn.lam is not None:
            self.mode = "empty"
        else:
            raise SystemExit("nom_ep1_verify: the candidate has neither networkProcessEpoch_ nor the "
                             "lambda processSyncedEpoch")
        if self.mode == "empty":
            self.T = self.rn.T
            self.open, self.close = self.rn.lam_open, self.rn.lam_close
            self.scopes, self.frames = self.rn.scopes, self.rn.frames
            self.members = self.rn.members
            self.shift = 0
            self.body_key = "L"
            self.preamble, self.stmts = [], self.rn.lambda_statements()
        else:
            fn = self.fn
            self.T, self.open, self.close = fn.T, fn.open, fn.close
            self.scopes, self.frames = fn.scopes, fn.frames
            self.members = self.rn.members
            self.shift = SHIFT
            self.body_key = "E"
            allst = fn.statements()
            mark = lint.raw_pos(self.T, RUN_NAMES_END, self.open, self.close)
            if mark is None:
                self.preamble, self.stmts = [], allst
                self.no_marker = True
            else:
                self.preamble = [x for x in allst if self.T[x[0]].pos < mark]
                self.stmts = [x for x in allst if self.T[x[0]].pos > mark]
                self.no_marker = False

    def resolver(self, stop=()):
        return Resolver(self.scopes, self.frames, stop, self.members)


# ---------------------------------------------------------------------------------------------
# Canonical form: a candidate binding in the frozen vocabulary (spec 5.4, 5.6)
# ---------------------------------------------------------------------------------------------
class Canon:
    """Maps a resolved candidate node to frozen roots: run.<member> through the NetworkRun
    construction (reference members) and the declared OWNER_MOVE / FIELD_MOVE paths;
    ep.<path> through the declared EPOCH_MEMBER / FIELD_MOVE / ACCESSOR paths and the CTOR
    (ep.se is the epoch function's se); E:<name> (the epoch function's top level) to L:<name>
    for a name the frozen lambda declares at its top level; B:<line>.x to B:x."""

    def __init__(self, run_refs, lam_names, resolve_key, ep_keys=("E:ep",), run_keys=("E:run",)):
        self.run_refs = dict(run_refs)      # reference member -> its frozen object (a node)
        self.lam_names = set(lam_names)
        self.resolve_key = resolve_key      # a frozen key ('D:est') -> its storage node
        self.ep_keys, self.run_keys = set(ep_keys), set(run_keys)

    def run_path(self, chain):
        """run.<path> as its frozen object: a reference member through the NetworkRun
        construction (V3d), an owned member through its declared OWNER_MOVE / FIELD_MOVE
        path."""
        path = "run" + "".join(chain)
        best = None
        for p, root in list(SUBST.run_paths.items()) + [("run." + m, r) for m, r in self.run_refs.items()]:
            if path == p or path.startswith(p + ".") or path.startswith(p + "->") or path.startswith(p + "["):
                if best is None or len(p) > len(best[0]):
                    best = (p, root)
        if best is None:
            return Opaque(path, "no declared realization of this run path")
        root = best[1] if isinstance(best[1], (Ref, Copy)) else self.resolve_key(best[1])
        rest = tuple(_segs(path[len(best[0]):]))
        if isinstance(root, Copy):
            return Copy(root.inner.extend(rest)) if rest else root
        return root.extend(rest) if rest else root

    def ep_path(self, chain):
        path = "ep" + "".join(chain)
        for name, (struct, new) in SUBST.field_solve.items():
            p = "ep.solve." + new
            if path == p or path.startswith(p + ".") or path.startswith(p + "["):
                local = SUBST.paths.get("ep.solve")
                return Ref("L:" + (local or "sg_"), ("." + name,) + tuple(_segs(path[len(p):])))
        for local, p in SUBST.accessor.items():
            if p and path == p:
                return Ref("L:" + local)
        if path == "ep.emitted" and SUBST.value_equal:
            return Ref("L:emitted")          # VALUE_EQUAL: the frozen comparison, stored once
        if path == "ep.se" or path.startswith("ep.se."):
            return Ref("L:se", tuple(_segs(path[len("ep.se"):])))
        local, rest = SUBST.epoch_local_of(path)
        if local is None:
            return Opaque(path, "no declared epoch member realizes this path")
        return Ref("L:" + local, tuple(_segs(rest)))

    def __call__(self, node):
        if isinstance(node, Ref):
            root = node.root
            if root in self.run_keys:
                r = self.run_path(node.chain)
                return Ref(r.root, r.chain, node.prefix) if isinstance(r, Ref) else r
            if root in self.ep_keys:
                r = self.ep_path(node.chain)
                return Ref(r.root, r.chain, node.prefix) if isinstance(r, Ref) else r
            kind, _, name = root.partition(":")
            if kind == "E":
                return Ref(("L:" if name in self.lam_names else "E:") + name, node.chain, node.prefix)
            if kind == "B":
                return Ref("B:" + name.split(".", 1)[-1], node.chain, node.prefix)
            return node
        if isinstance(node, Copy):
            return Copy(self(node.inner))
        if isinstance(node, Cond):
            return Cond(self(node.c), self(node.a), self(node.b))
        if isinstance(node, CallNode):
            return CallNode(node.fn, [self(a) for a in node.args])
        if isinstance(node, Expr):
            return Expr(node.op, [self(a) for a in node.args])
        return node


def _segs(rest):
    return re.findall(r"(?:\.|->)\w+(?:\([^)]*\))?|\[[^\]]*\]", rest)


def frozen_canon(node):
    """A frozen node with block-local line numbers dropped (B:<line>.x -> B:x)."""
    if isinstance(node, Ref):
        if node.root.startswith("B:"):
            return Ref("B:" + node.root[2:].split(".", 1)[-1], node.chain, node.prefix)
        return node
    if isinstance(node, Copy):
        return Copy(frozen_canon(node.inner))
    if isinstance(node, Cond):
        return Cond(frozen_canon(node.c), frozen_canon(node.a), frozen_canon(node.b))
    if isinstance(node, CallNode):
        return CallNode(node.fn, [frozen_canon(a) for a in node.args])
    if isinstance(node, Expr):
        return Expr(node.op, [frozen_canon(a) for a in node.args])
    return node


def is_const_root(node, const_roots):
    if not isinstance(node, Ref):
        return False
    r = node.render()
    return any(r == c or r.startswith(c + ".") for c in const_roots)


def normalize(node, const_roots):
    """copy(copy(X)) -> copy(X); copy(X) -> X for a const X (spec 5.4: a copy of a const root
    or of a declared COPY_SOURCE equals its source at every reader)."""
    if isinstance(node, Copy):
        inner = normalize(node.inner, const_roots)
        if isinstance(inner, Copy):
            inner = inner.inner
        if is_const_root(inner, const_roots):
            return inner
        return Copy(inner)
    if isinstance(node, Cond):
        return Cond(normalize(node.c, const_roots), normalize(node.a, const_roots),
                    normalize(node.b, const_roots))
    if isinstance(node, CallNode):
        return CallNode(node.fn, [normalize(a, const_roots) for a in node.args])
    if isinstance(node, Expr):
        return Expr(node.op, [normalize(a, const_roots) for a in node.args])
    return node


def show(node):
    try:
        return node.render()
    except Exception:  # noqa: BLE001
        return repr(node)


def apply_declarator(kind, node):
    """The object a parameter of frozen kind @p kind holds when bound to @p node."""
    if kind == "V":
        return Copy(node) if isinstance(node, (Ref, Copy)) else node
    return node


# ---------------------------------------------------------------------------------------------
# The verifier
# ---------------------------------------------------------------------------------------------
class Verifier:
    def __init__(self, fz, tree, rows, converted, presolve_writer="outputs", manifest=None,
                 verbose=False):
        self.fz = fz
        self.tree = tree
        self.rows = rows
        self.by_name = {r.name: r for r in rows}
        self.converted = set(converted)
        self.presolve_writer = presolve_writer
        self.manifest = manifest
        self.verbose = verbose
        self.R = Result()
        self.cand = Candidate(tree)
        self.claims = collections.defaultdict(list)     # path -> [(first line, last line)]
        self.frozen_claims = collections.defaultdict(list)
        self.realized = {}        # epoch local -> dict(transform, stmt, alias)
        self.pairs = []           # (frozen stmt, new stmt) carried by plain comparison
        self.decls = lint.DeclIndex(tree)
        self.seamdefs = lint.SeamDefs(tree)
        self.fdecls = {}          # row name -> (decl Func, defn Func) at the base
        self.run_refs = {}
        lam_names = {d.name for d in fz.rn.lam_scope.decls}
        self.canon = Canon({}, lam_names, self.frozen_root)
        self.lam_names = lam_names
        self.const_roots = self._const_roots()

    # -- shared facts --------------------------------------------------------------------
    def frozen_root(self, key):
        """The frozen storage object a driver name denotes (references followed): `D:nc` is
        `A:config_.getNetworkConfig()`; an Application member or an unknown key is itself."""
        kind, _, name = key.partition(":")
        if kind != "D":
            return Ref(key)
        node = self.fz.res.name_ref(name, self.fz.rn.lam_open, ())
        return node if isinstance(node, (Ref, Copy)) else Ref(key)

    def _const_roots(self):
        """Frozen roots that are const objects: the const lambda and driver locals, the knobs,
        and the declared COPY_SOURCE setup fields (proved by their write scans)."""
        out = {"D:ctx.knobs"}
        for d in self.fz.rn.lam_scope.decls:
            if d.is_const and not d.is_ptr:
                out.add("L:" + d.name)
        for d in self.fz.rn.scopes.root.decls:
            if d.is_const and not d.is_ptr and not d.is_ref:
                out.add("D:" + d.name)
        for n in SUBST.copy_sources:
            out.add("D:setup." + n)
        return out

    def frozen_rows(self):
        T = self.fz.tree
        for r in self.rows:
            if isinstance(r.mech, Explicit):
                continue
            cls = "Application" if r.mech == MEMBER else ""
            hi, ci = T.index(r.hpp), T.index(r.cpp)
            decls = [f for f in (hi.funcs if hi else []) if f.name == r.name and (f.cls or "") == cls
                     and f.params is not None and not f.is_def]
            defs = [f for f in (ci.funcs if ci else []) if f.name == r.name and f.is_def
                    and (f.cls or "") == cls and f.params is not None]
            sites = [c for c in self.fz.calls if c.name == r.name]
            n = sites[0].nargs if sites else None
            decl = next((f for f in decls if len(f.params) == n), decls[0] if decls else None)
            defn = next((f for f in defs if decl and len(f.params) == len(decl.params)),
                        defs[0] if defs else None)
            self.fdecls[r.name] = (decl, defn)

    # -- run ---------------------------------------------------------------------------------
    def run(self, only=None):
        self.frozen_rows()
        steps = [("V8", self.v8), ("V3d", self.v3d), ("V1a", self.v1a), ("V3b", self.v3b),
                 ("V3e", self.v3e), ("V4", self.v4), ("V6", self.v6), ("V1c", self.v1c),
                 ("V1d", self.v1d), ("V12", self.v12), ("V13", self.v13), ("V2", self.v2),
                 ("V1b", self.v1b), ("V3a", self.v3a), ("V3f", self.v3f), ("V3c", self.v3c),
                 ("V5", self.v5), ("V7", self.v7), ("V9", self.v9), ("V10", self.v10_11),
                 ("V1e", self.v1e)]
        for chk, fn in steps:
            try:
                fn()
            except Exception as ex:  # noqa: BLE001  (a crash is a failed check, not a pass)
                self.R.fail(chk, f"the check raised {type(ex).__name__}: {ex}")
                if self.verbose:
                    traceback.print_exc()
        for c in CHECKS:
            if only and c not in only:
                continue
            if c not in self.R.status and not self.R.problems[c] and not self.R.info[c]:
                self.R.na(c, "not run")
            self.R.done(c)
        return self.R

    @property
    def epoch(self):
        return self.cand.mode == "epoch"

    def claim(self, path, a, b):
        self.claims[path].append((a, b))

    def fclaim(self, path, a, b):
        self.frozen_claims[path].append((a, b))

    # ==== V8 ===========================================================================
    def v8(self):
        R, fz = self.R, self.fz
        blocks = declared.ep2_blocks(fz)
        spans = [sp for sps in blocks.values() for sp in sps]
        by_name = collections.Counter(r.name for r in self.rows)
        n_rows = n_nc = 0
        for n, c in enumerate(fz.calls, 1):
            who = []
            if any(s <= c.tok < e for s, e in spans):
                who.append("EP2 block")
            else:
                who += ["row"] * by_name.get(c.name, 0)
                who += [nc.label for nc in declared.NON_CONVERSIONS
                        if nc.kind != "EP2_BLOCK" and nc.covers(c, ())]
            if len(who) != 1:
                R.fail("V8", f"call #{n} {c.name} ({DRIVER_PATH}:{c.line}) is covered "
                             f"{len(who)} times ({', '.join(who) or 'by nothing'})")
            elif who[0] == "row":
                n_rows += 1
            else:
                n_nc += 1
        for lab, sps in blocks.items():
            if not sps:
                R.fail("V8", f"EP2 block {lab}: its anchor call is not in the frozen lambda")
        for r in self.rows:
            k = sum(1 for c in fz.calls if c.name == r.name)
            if k != r.sites:
                R.fail("V8", f"row {r.name}: the frozen lambda calls it {k} times, the row says {r.sites}")
        gone = [f"{d.cls} {d.name}" for d in declared.DECLARED
                if any(not hits for _, hits in d.locate(fz))]
        R.note("V8", f"{len(fz.calls)} frozen calls: {n_rows} by rows, {n_nc} by declared "
                     "non-conversions" + (f"; declared entries with no object at this base: "
                                          f"{', '.join(gone)}" if gone else ""))
        text = self.tree.read(CONTRACT_TEST)
        if self.epoch and text is not None:
            listed = set(re.findall(r"&\s*(?:Application::)?(network\w+)\b", text))
            listed |= set(re.findall(r"NetworkEpochContractProbe::(network\w+)", text))
            want = {r for r in self.converted}
            if listed != want:
                R.fail("V8", f"{CONTRACT_TEST} lists {sorted(listed - want)} not converted, misses "
                             f"{sorted(want - listed)}")
            else:
                R.note("V8", f"the contract test lists the {len(want)} converted seams")
        elif self.epoch and self.converted:
            R.fail("V8", f"{CONTRACT_TEST} is missing")

    # ==== V3d: the run's construction ====================================================
    def v3d(self):
        R = self.R
        if not self.epoch:
            R.na("V3d", "empty conversion: no NetworkRun")
            self.canon = Canon({}, self.lam_names, self.frozen_root, ep_keys=(), run_keys=())
            return
        rn, T = self.cand.rn, self.cand.rn.T
        run_decl = next((d for d in rn.scopes.root.decls if d.name == "run" and d.type_span
                         and T.join(*d.type_span) == "NetworkRun"), None)
        if run_decl is None or run_decl.init_kind != "(":
            R.fail("V3d", f"{DRIVER_PATH}: no `NetworkRun run(...)` in runNetwork's outermost scope")
            self.canon = Canon({}, self.lam_names, self.frozen_root)
            return
        args = list(_split_top(T, *run_decl.init))
        hdr = self.tree.index(RUN_HPP)
        ctor = None
        if hdr:
            ctor = next((f for f in hdr.funcs if f.name == "NetworkRun" and f.cls == "NetworkRun"
                         and f.params and len(f.params) == len(args)), None)
        if ctor is None:
            R.fail("V3d", f"{RUN_HPP}: no NetworkRun constructor with {len(args)} parameters")
            self.canon = Canon({}, self.lam_names, self.frozen_root)
            return
        HT = self.tree.tokens(RUN_HPP)
        inits = _mem_initializers(HT, ctor)
        pidx = {p.name: k for k, p in enumerate(ctor.params)}
        res = rn.resolver()
        for member, want in RUN_REFS.items():
            init = inits.get(member)
            if init is None or init not in pidx:
                R.fail("V3d", f"NetworkRun::{member} is not initialized from a constructor parameter")
                continue
            s, e = args[pidx[init]]
            if e != s + 1 or T.k(s) != "id":
                R.fail("V3d", f"NetworkRun::{member}: the construction passes `{T.join(s, e)}`, not a name")
                continue
            d = rn.scopes.lookup(T.t(s), s)
            if d is None and T.t(s) not in rn.members:
                R.fail("V3d", f"NetworkRun::{member}: `{T.t(s)}` is not declared before the construction")
                continue
            if d is not None and d.scope is not rn.scopes.root:
                R.fail("V3d", f"NetworkRun::{member}: `{T.t(s)}` is not declared in runNetwork's outermost scope")
                continue
            got = res.resolve(s, e)
            want_node = self.frozen_root(want)
            if show(got) != show(want_node):
                R.fail("V3d", f"NetworkRun::{member} binds {show(got)}, the frozen object is "
                              f"{show(want_node)} ({want})")
            self.run_refs[member] = self.frozen_root(want)
        R.note("V3d", f"NetworkRun run(...) at {DRIVER_PATH}:{T.line(run_decl.tok)}: "
                      f"{len(self.run_refs)} reference members bound")
        self.claim(DRIVER_PATH, T.line(run_decl.stmt[0]), T.line(run_decl.stmt[1] - 1))
        self.canon = Canon(self.run_refs, self.lam_names, self.frozen_root)

    # ==== V1a: the lambda's lines =========================================================
    def frozen_subs(self, s, e, tf):
        """Rendering substitutions of a frozen statement: converted calls' arguments, and the
        two replay closures' invocations under T-F."""
        T = self.fz.drv.T
        subs = []
        for c in calls_in(T, s, e):
            if c.name in self.converted and c.close > c.open + 1:
                if any(x.a <= c.tok < x.b for x in subs):
                    continue
                subs.append(Sub(c.open + 1, c.close, f"<ARGS:{c.name}>", True, True))
            elif tf and c.name in SUBST.callbacks and c.nargs == 0:
                builder = "network" + c.name[0].upper() + c.name[1:]
                subs.append(Sub(c.tok, c.close + 1, f"{builder}(<ARGS:{builder}>)"))
        return _drop_nested(subs)

    def new_subs(self, s, e, stores=False):
        """Rendering substitutions of a candidate statement: converted calls' arguments, the
        bracket rewrite, ep.emitted, and the epoch paths of dropped locals back to the
        local's name (spec 5.3 T-B7, T-B9, T-E)."""
        T = self.cand.T
        subs = []
        for c in calls_in(T, s, e):
            if c.name in self.converted and c.close > c.open + 1:
                subs.append(Sub(c.open + 1, c.close, f"<ARGS:{c.name}>", True, True))
        if self.epoch:
            for c in calls_in(T, s, e):
                if c.name in ("networkSubBegin", "networkSubEnd") and c.nargs == 2:
                    a0 = c.args[1][0]
                    want = f"{c.name}(run, "
                    got = T.source[T[c.tok].pos:T[a0].pos]
                    if got != want:
                        self.R.fail("V1a", f"{T.path}:{c.line}: bracket call `{got}...` is not `{want}...`")
                    old = "subBegin(" if c.name == "networkSubBegin" else "subEnd("
                    subs.append(Sub(c.tok, a0, old, False, True))
            j = s
            while j < e:
                if T.t(j) == "ep" and T.k(j) == "id" and T.t(j - 1) not in (".", "->", "::"):
                    k = ep_path_tokens(T, j, e)
                    path = norm_subs(T, j, k, ())
                    if path == "ep.emitted" and T.t(k) != "=" and SUBST.value_equal:
                        subs.append(Sub(j, k, SUBST.value_equal))
                    else:
                        txt = self.inverse_path(path)
                        if txt is not None:
                            subs.append(Sub(j, k, txt))
                    j = k
                    continue
                if T.t(j) in self.result_aliases() and T.t(j + 1) == "." and T.t(j + 2) == "handoff":
                    f = T.t(j + 4) if T.t(j + 3) == "." else ""
                    if f in SUBST.field_solve:
                        subs.append(Sub(j, j + 5, f"{T.t(j)}.{f}"))
                        j += 5
                        continue
                j += 1
        return _drop_nested(subs)

    def result_aliases(self):
        return {loc for loc, (p, t) in SUBST.epoch.items() if t == "RESULT"}

    def inverse_path(self, path):
        """The frozen text a candidate epoch path stands for (`ep.solve.action` -> `sg_.action`,
        `ep.solve.handoff.retro_flag_sats` -> `sg_.retro_flag_sats`), or None."""
        for name, (struct, new) in SUBST.field_solve.items():
            p = "ep.solve." + new
            if path == p or path.startswith(p + "."):
                local = SUBST.paths.get("ep.solve", "sg_")
                return local + "." + name + path[len(p):]
        for local, p in SUBST.accessor.items():
            if path == p:
                return local
        local, rest = SUBST.epoch_local_of(path)
        if local is None:
            return None
        return local + rest

    def classify(self, s, e):
        """(kind, declarations) of a frozen lambda statement for the pairing of V1a."""
        T = self.fz.drv.T
        ds = parse_declaration(T, s, e) or []
        names = [d.name for d in ds]
        if not ds:
            return "PLAIN", ds
        if any(n in SUBST.epoch for n in names):
            tr = {SUBST.epoch[n][1] for n in names if n in SUBST.epoch}
            if tr == {"CLOCK"}:
                return "PLAIN", ds
            if tr == {"WS"}:
                return "WS", ds
            return "EPOCH_DECL", ds
        if any(n in SUBST.accessor for n in names):
            return "ACCESSOR", ds
        if any(n in SUBST.callbacks for n in names):
            return "CLOSURE", ds
        if len(ds) == 1 and ds[0].is_ref and ds[0].init:
            a, b = ds[0].init
            if T.k(a) == "id" and T.t(a) in self.result_aliases() and T.t(a + 1) == ".":
                return "MEMBER_ALIAS", ds
        return "PLAIN", ds

    def v1a(self):
        R, fz, cand = self.R, self.fz, self.cand
        fT, nT = fz.drv.T, cand.T
        F, N = fz.lam_stmts, cand.stmts
        if self.epoch and getattr(cand, "no_marker", False):
            R.fail("V1a", f"{cand.fn.path}: no `// ---- End of the run's names ----` marker after the "
                          "epoch function's preamble")
        tf = "networkSolveGlue" in self.converted
        canon_pair = next((k for k, (s, e) in enumerate(F)
                           if any(c.name == "networkCanonicalPairAdmission" for c in calls_in(fT, s, e))), None)
        clock_last = max((k for k, (s, e) in enumerate(F)
                          for d in (parse_declaration(fT, s, e) or [])
                          if d.name in SUBST.epoch and SUBST.epoch[d.name][1] == "CLOCK"), default=None)
        emit_close = None
        emit_k = next((k for k, (s, e) in enumerate(F)
                       if any(c.name == "networkEmitStore" for c in calls_in(fT, s, e))), None)
        if emit_k is not None:
            emit_close = next((k for k in range(emit_k + 1, len(F))
                               if fT.join(*F[k]) == "subEnd(SB_EMITSTORE);"), None)
        inserts = {}
        if self.epoch:
            if canon_pair is not None:
                inserts.setdefault(canon_pair, []).append(("CTOR", "NetworkEpoch ep(se);"))
            if clock_last is not None:
                clk = [n for n, (p, t) in SUBST.epoch.items() if t == "CLOCK"]
                inserts.setdefault(clock_last, []).append(
                    ("CLOCK", "ep.clock = EpochClock{ " + ", ".join(clk) + " };"))
            if emit_close is not None and SUBST.value_equal:
                inserts.setdefault(emit_close, []).append(
                    ("EMITTED", f"ep.emitted = {SUBST.value_equal};"))
        self.inserted = {}
        j = 0
        carried = transformed = 0
        dropped = []
        for i, (s, e) in enumerate(F):
            kind, ds = self.classify(s, e)
            if not self.epoch:
                kind = "PLAIN"
            if kind == "WS":
                for d in ds:
                    self.realized[d.name] = {"transform": "WS", "stmt": None, "frozen": i,
                                             "type": fT.join(*d.type_span), "path": SUBST.epoch[d.name][0]}
                transformed += 1
            elif kind == "EPOCH_DECL":
                j = self.realize(i, s, e, ds, j)
                transformed += 1
            elif kind == "ACCESSOR":
                if j < len(N) and nT.join(*N[j]) == join_text(
                        f"const Eigen::VectorXd& {ds[0].name} = {SUBST.accessor[ds[0].name]};"):
                    self.realized[ds[0].name] = {"transform": "ACCESSOR", "stmt": j, "frozen": i}
                    j += 1
                else:
                    self.realized[ds[0].name] = {"transform": "ACCESSOR", "stmt": None, "frozen": i}
                    dropped.append((ds[0].name, j))
                transformed += 1
            elif kind == "CLOSURE" and tf:
                transformed += 1
            elif kind == "MEMBER_ALIAS":
                if j < len(N) and self.same_stmt(s, e, *N[j], quiet=True):
                    self.pairs.append((i, j))
                    j += 1
                    carried += 1
                else:
                    dropped.append((ds[0].name, j))
                    transformed += 1
            else:
                k = self.match_ahead(s, e, N, j)
                if k is None:
                    R.fail("V1a", f"frozen {DRIVER_PATH}:{fT.line(s)}-{fT.line(e - 1)} is not carried: "
                                  f"`{fT.join(s, min(e, s + 12))}...`" + self.first_diff(s, e, N, j))
                else:
                    for x in range(j, k):
                        R.fail("V1a", f"{nT.path}:{nT.line(N[x][0])}: a statement the lambda does not "
                                      f"have: `{nT.join(N[x][0], min(N[x][1], N[x][0] + 12))}...`")
                    self.pairs.append((i, k))
                    j = k + 1
                    carried += 1
            for tag, text in inserts.get(i, ()):
                if j < len(N) and nT.join(*N[j]) == join_text(text):
                    self.inserted[tag] = j
                    self.claim(nT.path, nT.line(N[j][0]), nT.line(N[j][1] - 1))
                    j += 1
                else:
                    R.fail("V1a", f"the inserted `{text}` ({tag}) does not follow frozen "
                                  f"{DRIVER_PATH}:{fT.line(s)}")
                    continue
                if tag != "CTOR":
                    continue
                # T-B2: the workspace was declared before `ep` could exist; while explicit
                # calls read `ws`, its alias follows the epoch's construction.
                for local, (path, tr) in SUBST.epoch.items():
                    if tr != "WS" or j >= len(N):
                        continue
                    nd = parse_declaration(nT, *N[j]) or []
                    if len(nd) == 1 and nd[0].name == local and nd[0].is_ref and nd[0].init                             and nT.join(*nd[0].init) == path and not nd[0].is_const:
                        ntype = nT.join(*nd[0].type_span)
                        ftype = self.realized.get(local, {}).get("type") or ntype
                        if ntype not in ("auto", ftype):
                            R.fail("V1a", f"{nT.path}:{nT.line(N[j][0])}: the alias of {local} has "
                                          f"type `{ntype}`, the frozen local `{ftype}`")
                        self.realized.setdefault(local, {})["alias"] = j
                        self.claim(nT.path, nT.line(N[j][0]), nT.line(N[j][1] - 1))
                        j += 1
        for x in range(j, len(N)):
            R.fail("V1a", f"{nT.path}:{nT.line(N[x][0])}: a statement the lambda does not have: "
                          f"`{nT.join(N[x][0], min(N[x][1], N[x][0] + 12))}...`")
        # A dropped declaration's name is not read afterwards (unless a nested declaration
        # of the same name shadows it, as in the frozen text).
        for name, at in dropped:
            for k in range(at, len(N)):
                s, e = N[k]
                for t in ids_in(nT, s, e):
                    if nT.t(t) == name and nT.t(t + 1) != "(":
                        d = cand.scopes.lookup(name, t)
                        if d is None or d.scope is cand.scopes.root:
                            R.fail("V1a", f"{nT.path}:{nT.line(t)}: `{name}` is read but its declaration "
                                          "was dropped")
                            break
        self.fclaim(DRIVER_PATH, fT.line(fz.rn.lam.stmt[0]), fT.line(fz.rn.lam.stmt[1] - 1))
        if self.epoch:
            fn = cand.fn
            self.claim(fn.path, nT.line(decl_start(nT, fn.func)), nT.line(fn.close))
        R.note("V1a", f"{len(F)} frozen top-level statements: {carried} carried "
                      f"{'(shift ' + str(cand.shift) + ')' if cand.shift else 'verbatim'}, "
                      f"{transformed} by a declared transform; {len(self.inserted)} inserted stores")

    def match_ahead(self, s, e, N, j, ahead=6):
        for k in range(j, min(len(N), j + ahead)):
            if self.same_stmt(s, e, *N[k], quiet=True):
                return k
        return None

    def first_diff(self, s, e, N, j):
        if j >= len(N):
            return " (the epoch body ended)"
        fT, nT = self.fz.drv.T, self.cand.T
        tf = "networkSolveGlue" in self.converted
        a = render(fT, s, e, self.frozen_subs(s, e, tf))
        b = render(nT, N[j][0], N[j][1], self.new_subs(N[j][0], N[j][1]))
        d = same_lines(a, b, self.cand.shift)
        if d is None:
            return ""
        k, x, y = d
        return (f"; first difference against {nT.path}:{nT.line(N[j][0])}: frozen "
                f"`{x[1] if x else '<end>'}`, here `{y[1] if y else '<end>'}`")

    def same_stmt(self, s, e, ns, ne, quiet=False):
        fT, nT = self.fz.drv.T, self.cand.T
        tf = "networkSolveGlue" in self.converted
        a = render(fT, s, e, self.frozen_subs(s, e, tf))
        b = render(nT, ns, ne, self.new_subs(ns, ne))
        return same_lines(a, b, self.cand.shift) is None

    def realize(self, i, s, e, ds, j):
        """Pairs the frozen declaration of one or more epoch locals with its realization
        (spec 5.3 T-B3, T-B4, T-B5, T-B7); returns the next candidate statement."""
        R, fT, nT, N = self.R, self.fz.drv.T, self.cand.T, self.cand.stmts
        for d in ds:
            if d.name not in SUBST.epoch:
                R.fail("V1a", f"frozen {DRIVER_PATH}:{d.line}: `{d.name}` shares a declaration with "
                              "an epoch local but is not one")
                continue
            path, tr = SUBST.epoch[d.name]
            ftype = fT.join(*d.type_span)
            rec = {"transform": tr, "stmt": None, "alias": None, "frozen": i, "path": path,
                   "type": ftype, "init": fT.norm(*d.init) if d.init else "", "decl": d}
            self.realized[d.name] = rec
            if tr in ("SIZED", "RESULT"):
                if j >= len(N):
                    R.fail("V1a", f"{d.name} ({tr}): the epoch body ended before its store")
                    continue
                ns, ne = N[j]
                lhs = norm_subs(nT, ns, ep_path_tokens(nT, ns, ne), ())
                if nT.t(ns) != "ep" or lhs != path or nT.t(ep_path_tokens(nT, ns, ne)) != "=":
                    R.fail("V1a", f"{nT.path}:{nT.line(ns)}: expected the store `{path} = ...` of "
                                  f"{d.name} ({tr}) where frozen {DRIVER_PATH}:{d.line} declared it")
                    continue
                q = ep_path_tokens(nT, ns, ne) + 1
                rhs = norm_subs(nT, q, ne - 1, self.new_subs(q, ne - 1))
                if tr == "SIZED":
                    want = join_text(f"{fT.src(*d.type_span)}") + "(" + fT.join(*d.init) + ")"
                else:
                    want = norm_subs(fT, d.init[0], d.init[1],
                                     self.frozen_subs(d.init[0], d.init[1], "networkSolveGlue" in self.converted))
                if rhs != want:
                    R.fail("V1a", f"{nT.path}:{nT.line(ns)}: {d.name} ({tr}) stores `{rhs}`, the frozen "
                                  f"initializer is `{want}`")
                rec["stmt"] = j
                self.claim(nT.path, nT.line(ns), nT.line(ne - 1))
                j += 1
            if j < len(N):
                ns, ne = N[j]
                nd = parse_declaration(nT, ns, ne) or []
                if len(nd) == 1 and nd[0].name == d.name and nd[0].is_ref and nd[0].init:
                    init = norm_subs(nT, nd[0].init[0], nd[0].init[1], ())
                    ntype = nT.join(*nd[0].type_span)
                    okt = ntype in ("auto", "const auto", ftype, "const " + ftype,
                                    ftype.replace("const ", "", 1))
                    if init != path or not okt or nd[0].ops != "&":
                        R.fail("V1a", f"{nT.path}:{nT.line(ns)}: the alias of {d.name} binds `{init}` as "
                                      f"`{ntype}{nd[0].ops}`; expected `{path}` as a reference of type "
                                      f"auto or {ftype}")
                    rec["alias"] = j
                    if rec["stmt"] is None:
                        rec["stmt"] = j
                    self.claim(nT.path, nT.line(ns), nT.line(ne - 1))
                    j += 1
            if tr in ("ALIAS", "SCALAR") and rec["alias"] is None:
                rec["dropped_at"] = j
        return j

    # ==== V3b: captured names =============================================================
    def v3b(self):
        R, fz, cand = self.R, self.fz, self.cand
        fT, nT = fz.drv.T, cand.T
        fres = fz.res
        nres = cand.resolver()
        tf = "networkSolveGlue" in self.converted
        n = bad = 0
        for i, j in self.pairs:
            fs, fe = fz.lam_stmts[i]
            ns, ne = cand.stmts[j]
            fsubs = self.frozen_subs(fs, fe, tf)
            nsubs = self.new_subs(ns, ne)
            ftoks = [t for t in range(fs, fe) if not any(x.a <= t < x.b for x in fsubs)]
            ntoks = [t for t in range(ns, ne) if not any(x.a <= t < x.b for x in nsubs)]
            if len(ftoks) != len(ntoks):
                continue
            for a, b in zip(ftoks, ntoks):
                if fT.k(a) != "id" or fT.t(a) in KEYWORDS or fT.t(a - 1) in (".", "->", "::") \
                        or fT.t(a + 1) == "::" or fT.t(a) != nT.t(b):
                    continue
                d = fz.rn.scopes.lookup(fT.t(a), a)
                if d is None or d.kind == "type" or d.scope is not fz.rn.scopes.root:
                    continue
                if d.kind == "closure" or d.name in SUBST.bracket or d.name in SUBST.callable:
                    continue
                n += 1
                want = normalize(frozen_canon(fres.resolve(a, a + 1)), self.const_roots)
                got = normalize(self.canon(nres.resolve(b, b + 1)), self.const_roots)
                if show(want) != show(got):
                    bad += 1
                    if bad <= 25:
                        R.fail("V3b", f"{nT.path}:{nT.line(b)}: `{fT.t(a)}` resolves to {show(got)}; "
                                      f"the captured name is {show(want)}")
        if bad > 25:
            R.fail("V3b", f"... {bad - 25} more")
        if self.epoch:
            self.check_preamble()
        R.note("V3b", f"{n} reads of captured driver names compared with their frozen roots")

    def check_preamble(self):
        """Each line of the epoch function's preamble declares a name the frozen lambda
        captured (or the third-band predicate) and binds its frozen object (V3b)."""
        R, cand = self.R, self.cand
        T = cand.T
        res = cand.resolver()
        fres = self.fz.res
        caps = self.fz.drv.captures()
        froot = {d.name: d for d in self.fz.rn.scopes.root.decls}
        for s, e in cand.preamble:
            self.claim(T.path, T.line(s), T.line(e - 1))
            ds = parse_declaration(T, s, e) or []
            if not ds:
                R.fail("V3b", f"{T.path}:{T.line(s)}: the preamble holds a statement that declares "
                              f"nothing: `{T.join(s, min(e, s + 10))}...`")
            for d in ds:
                if d.name in SUBST.callable:
                    continue
                if d.name not in caps:
                    R.fail("V3b", f"{T.path}:{d.line}: the preamble declares `{d.name}`, which the frozen "
                                  "lambda did not capture")
                    continue
                fd = froot.get(d.name)
                if fd is None:
                    continue
                if fd.is_ref and not d.is_ref:
                    R.fail("V3b", f"{T.path}:{d.line}: `{d.name}` is a copy; the frozen name is a reference")
                if not fd.is_ref and not d.is_ref and not d.is_const:
                    R.fail("V3b", f"{T.path}:{d.line}: `{d.name}` is a non-const copy")
                if d.init:
                    got = normalize(self.canon(res.resolve(*d.init)), self.const_roots)
                    if not d.is_ref and not d.is_ptr:
                        got = normalize(Copy(got), self.const_roots)
                    want = normalize(frozen_canon(fres.name_ref(fd.name, self.fz.rn.lam_open, ())),
                                     self.const_roots)
                    if show(got) != show(want) and not (isinstance(got, Ref) and show(got) == "D:" + d.name):
                        R.fail("V3b", f"{T.path}:{d.line}: `{d.name}` binds {show(got)}; frozen {show(want)}")

    # ==== V3e: fresh names ================================================================
    def v3e(self):
        R, cand = self.R, self.cand
        T = cand.T
        n = 0
        decl_toks = {d.tok for sc in cand.scopes.all for d in sc.decls}
        for j in ids_in(T, cand.open + 1, cand.close):
            name = T.t(j)
            if name not in ("run", "ep", "nrun", "nep") or j in decl_toks:
                continue
            n += 1
            d = cand.scopes.lookup(name, j)
            if self.epoch:
                ok = d is not None and (d.kind != "param" or name == "run" or name == cand.fn.run_name)
                if name in ("nrun", "nep") and (d is None or d.scope is cand.scopes.root):
                    ok = False
            else:
                ok = d is not None
            if not ok:
                R.fail("V3e", f"{T.path}:{T.line(j)}: `{name}` is read but declared nowhere in scope")
        for name in sorted(self.converted):
            if self.by_name[name].mech == ADAPTER:
                continue
            for f in self.new_defs(name):
                DT = self.tree.tokens(f.path)
                sp = lint.preamble_span(DT, f)
                lo = sp[1] if sp else f.body[0] + 1
                pnames = {p.name for p in f.params[:2]}
                sc = Scopes(DT, f.body[0], f.body[1], f.params)
                builders = {"network" + c[0].upper() + c[1:] for c in SUBST.callbacks}
                allowed = [(c.open, c.close) for c in calls_in(DT, lo, f.body[1])
                           if name == lint.SOLVE and c.name in builders]
                for j in ids_in(DT, lo, f.body[1]):
                    if any(x < j < y for x, y in allowed):
                        continue
                    if DT.t(j) in pnames:
                        d = sc.lookup(DT.t(j), j)
                        if d is not None and d.kind == "param":
                            R.fail("V3e", f"{f.path}:{DT.line(j)}: {name}'s body below its bindings reads "
                                          f"the parameter `{DT.t(j)}`")
        R.note("V3e", f"{n} uses of run / ep / nrun / nep in the epoch body; "
                      f"{len(self.converted)} converted bodies scanned")

    # ==== V4 ==============================================================================
    def v4(self):
        R, cand = self.R, self.cand
        if self.epoch:
            lt = lint.Lint(self.tree)
            lt.v4()
            for p in lt.problems["V4"]:
                R.fail("V4", p)
            for p in lt.info["V4"]:
                R.note("V4", p)
            return
        T = cand.T
        res = cand.resolver()
        solve = next((c for c in calls_in(T, cand.open + 1, cand.close) if c.name == lint.SOLVE), None)
        if solve is None:
            R.fail("V4", "the lambda has no networkSolveGlue call")
            return
        k = stmt_of(cand.stmts, solve.tok)
        solve_end = cand.stmts[k][1]
        emit = next((c.tok for c in calls_in(T, cand.open + 1, cand.close) if c.name == lint.EMIT), None)
        const_locals = {d.name for d in cand.rn.lam_scope.decls if d.is_const}

        def epath(s, e):
            try:
                node = res.resolve(s, e)
            except (ValueError, IndexError):
                return None
            if not isinstance(node, Ref) or node.prefix == "*":
                return None
            kind, _, name = node.root.partition(":")
            if kind != "L" or name in const_locals:
                return None
            chain = "".join(node.chain)
            for f, (struct, new) in SUBST.field_solve.items():
                if name == SUBST.paths.get("ep.solve") and (chain == "." + f or chain.startswith("." + f + ".")):
                    return "ep.solve." + new + chain[len("." + f):]
            if name in SUBST.epoch:
                return SUBST.epoch[name][0] + chain
            return None

        probs, writes, allowed = lint.check_v4(T, solve_end, cand.close, epath, self.decls, emit)
        for p in probs:
            R.fail("V4", p)
        R.note("V4", f"empty conversion: {len(writes)} writes into the epoch locals after the solve "
                     f"({len(allowed)} declared outputs)")

    # ==== V6: order =======================================================================
    def events(self, T, a, b, scopes, closures, inserts=None, skip_closures=True):
        """Execution-order events of tokens [a, b) (see the module docstring)."""
        ev = []
        bodies = {}
        for name, (lo, hi) in closures.items():
            bodies[name] = (lo, hi)
        skip = [v for v in bodies.values() if a <= v[0] < b] if skip_closures else []
        stmts_ins = inserts or {}
        j = a
        while j < b:
            if any(lo <= j <= hi for lo, hi in skip):
                j = next(hi for lo, hi in skip if lo <= j <= hi) + 1
                continue
            if j in stmts_ins:
                ev.append(("store", stmts_ins[j]))
            t = T.t(j)
            if t == "return" and T.t(j + 1) == "LoopSignal" and T.t(j + 2) == "::":
                ev.append(("return", T.t(j + 3)))
            if T.k(j) == "id" and T.t(j + 1) == "(" and T.t(j - 1) not in (".", "->", "::"):
                name, k = t, j
                q = j
                while T.t(q - 1) in (".", "->", "::") and T.k(q - 2) == "id":
                    name = T.t(q - 2) + T.t(q - 1) + name
                    q -= 2
                ev.extend(self.call_event(T, j, name, closures))
            elif T.k(j) == "id" and T.t(j + 1) == "(" and T.t(j - 1) in (".", "->", "::"):
                q, name = j, t
                while T.t(q - 1) in (".", "->", "::") and T.k(q - 2) == "id":
                    name = T.t(q - 2) + T.t(q - 1) + name
                    q -= 2
                if T.t(q - 1) not in (".", "->", "::"):
                    ev.extend(self.call_event(T, j, name, closures))
            j += 1
        return ev

    def call_event(self, T, j, name, closures):
        if name == LAMBDA_NAME:
            return [("call", lint.EPOCH_FN)]
        if name in ("subBegin", "subEnd", "networkSubBegin", "networkSubEnd"):
            close = T.match[j + 1]
            args = [T.join(s, e) for s, e in _split_top(T, j + 2, close)]
            return [("bracket", name.replace("network", "").replace("SubB", "subB").replace("SubE", "subE"),
                     args[-1] if args else "")]
        if name in closures:
            lo, hi = closures[name]
            return self.events(T, lo + 1, hi, None, {k: v for k, v in closures.items() if k != name},
                               skip_closures=True)
        m = re.match(r"^log->(\w+)$", name)
        if m:
            close = T.match[j + 1]
            args = _split_top(T, j + 2, close)
            level = m.group(1)
            lit_at = 0
            if level == "log" and args:
                level = "log:" + T.join(*args[0])
                lit_at = 1
            lit = ""
            if len(args) > lit_at:
                s, e = args[lit_at]
                lit = "".join(T.t(q) for q in range(s, e) if T.k(q) == "str")
            return [("log", level, lit)]
        if name == LAMBDA_NAME:
            return [("call", lint.EPOCH_FN)]
        if SEAM_CALL.match(name) or re.match(r"^network\w+$", name):
            return [("call", name)]
        return []

    def closures_in(self, T, scope):
        out = {}
        for d in scope.decls:
            if d.kind == "closure" and d.lambda_scope is not None:
                ls = d.lambda_scope
                out[d.name] = (T.match[ls.end], ls.end)
        return out

    def v6(self):
        R, fz, cand = self.R, self.fz, self.cand
        fT, nT = fz.drv.T, cand.T
        fclos = self.closures_in(fT, fz.rn.lam_scope)
        fclos_drv = {k: v for k, v in self.closures_in(fT, fz.rn.scopes.root).items()
                     if k in SUBST.bracket}
        finserts = {}
        if self.epoch:
            F = fz.lam_stmts
            canon_k = next((k for k, (s, e) in enumerate(F)
                            if any(c.name == "networkCanonicalPairAdmission" for c in calls_in(fT, s, e))), None)
            clock_k = max((k for k, (s, e) in enumerate(F) for d in (parse_declaration(fT, s, e) or [])
                           if SUBST.epoch.get(d.name, ("", ""))[1] == "CLOCK"), default=None)
            emit_k = next((k for k, (s, e) in enumerate(F)
                           if any(c.name == "networkEmitStore" for c in calls_in(fT, s, e))), None)
            emit_close = next((k for k in range(emit_k + 1, len(F))
                               if fT.join(*F[k]) == "subEnd(SB_EMITSTORE);"), None) if emit_k is not None else None
            for k, tag in ((canon_k, "ep"), (clock_k, "clock"), (emit_close, "emitted")):
                if k is not None and k + 1 < len(F):
                    finserts[F[k + 1][0]] = tag
                elif k is not None:
                    finserts[fz.rn.lam_close] = tag
        fev = self.events(fT, fz.rn.lam_open + 1, fz.rn.lam_close + 1, None, dict(fclos, **fclos_drv),
                          finserts)
        nclos = self.closures_in(nT, cand.scopes.root if self.epoch else cand.rn.lam_scope)
        if not self.epoch:
            nclos.update({k: v for k, v in self.closures_in(nT, cand.rn.scopes.root).items()
                          if k in SUBST.bracket})
        ninserts = {}
        if self.epoch:
            for s, e in cand.stmts:
                txt = nT.join(s, e)
                if txt == join_text("NetworkEpoch ep(se);"):
                    ninserts[s] = "ep"
                elif txt.startswith("ep.clock="):
                    ninserts[s] = "clock"
                elif txt.startswith("ep.emitted="):
                    ninserts[s] = "emitted"
            a = cand.stmts[0][0] if cand.stmts else cand.close
        else:
            a = cand.open + 1
        nev = self.events(nT, a, cand.close + 1, None, nclos, ninserts)
        fev = [e for e in fev if e[0] != "call" or e[1] not in SUBST.callbacks]
        self.compare_events("V6", "the epoch body", fev, nev)
        # runNetwork against its frozen self minus the lambda.
        fskip = {LAMBDA_NAME: (fz.rn.lam_open, fz.rn.lam_close)}
        fev2 = self.events(fT, fz.rn.rn.body[0] + 1, fz.rn.rn.body[1], None,
                           dict(self.closures_in(fT, fz.rn.scopes.root), **fskip))
        rT = cand.rn.T
        nskip = self.closures_in(rT, cand.rn.scopes.root)
        if not self.epoch and cand.rn.lam:
            nskip[LAMBDA_NAME] = (cand.rn.lam_open, cand.rn.lam_close)
        nev2 = self.events(rT, cand.rn.rn.body[0] + 1, cand.rn.rn.body[1], None, nskip)
        # The lambda's body events are counted with the epoch body; at the two call sites of
        # the lambda (frozen) or of networkProcessEpoch_ (candidate) one call event stands.
        fev2 = _expand_lambda_calls(fev2)
        nev2 = _expand_lambda_calls(nev2)
        if self.epoch:
            nev2 = [e for e in nev2 if e != ("call", "networkEngineSpec")]
        self.compare_events("V6", "runNetwork", fev2, nev2)
        R.note("V6", f"epoch body {len(nev)} events, runNetwork {len(nev2)} events")

    def compare_events(self, chk, what, fev, nev):
        if fev == nev:
            return
        k = next((k for k in range(min(len(fev), len(nev))) if fev[k] != nev[k]), min(len(fev), len(nev)))
        self.R.fail(chk, f"{what}: the order differs at event {k} of {len(fev)}/{len(nev)}: frozen "
                         f"{fev[k] if k < len(fev) else '<end>'}, here {nev[k] if k < len(nev) else '<end>'}")

    # ==== V1c: unconverted rows and adapters' explicit forms ================================
    def v1c(self):
        R = self.R
        n = 0
        for r in self.rows:
            if isinstance(r.mech, Explicit):
                continue
            fdecl, fdef = self.fdecls.get(r.name, (None, None))
            if fdecl is None or fdef is None:
                R.fail("V1c", f"{r.name}: not found at the base")
                continue
            explicit_must_stay = r.name not in self.converted or r.mech == ADAPTER
            if not explicit_must_stay:
                continue
            for f, path in ((fdecl, r.hpp), (fdef, r.cpp)):
                nf = self.find_explicit(path, r, f)
                FT, NT = self.fz.tree.tokens(path), self.tree.tokens(path)
                if nf is None:
                    R.fail("V1c", f"{r.name}: its explicit {'definition' if f.is_def else 'declaration'} "
                                  f"is missing from {path}")
                    continue
                a = render(FT, decl_start(FT, f), decl_end(FT, f))
                b = render(NT, decl_start(NT, nf), decl_end(NT, nf))
                if r.name == lint.SOLVE and f.is_def and self.epoch:
                    a, b, _ = _solve_declared(a, b, R, replay=False)
                    self.claim(path, NT.line(decl_start(NT, nf)), NT.line(decl_end(NT, nf) - 1))
                    self.fclaim(path, FT.line(decl_start(FT, f)), FT.line(decl_end(FT, f) - 1))
                d = same_lines(a, b)
                n += 1
                if d is not None:
                    k, x, y = d
                    R.fail("V1c", f"{path}:{NT.line(decl_start(NT, nf)) }: {r.name}'s explicit "
                                  f"{'definition' if f.is_def else 'declaration'} differs from frozen at code "
                                  f"line {k}: frozen `{x[1] if x else '<end>'}`, here `{y[1] if y else '<end>'}`")
        R.note("V1c", f"{n} explicit declarations and definitions compared with frozen")

    def find_explicit(self, path, r, f):
        idx = self.tree.index(path)
        if idx is None:
            return None
        cls = "Application" if r.mech == MEMBER else ""
        cands = [g for g in idx.funcs if g.name == r.name and (g.cls or "") == cls and g.params is not None
                 and g.is_def == f.is_def and not lint.is_epoch_form(g)]
        same = [g for g in cands if [p.kind for p in g.params] == [p.kind for p in f.params]]
        return (same or cands or [None])[0]

    def new_defs(self, name):
        return self.seamdefs.find(name, True)

    # ==== V1d: runNetwork =================================================================
    def v1d(self):
        R, fz, cand = self.R, self.fz, self.cand
        fT, nT = fz.rn.T, cand.rn.T
        if not self.epoch:
            a = render(fT, fz.rn.rn.body[0], fz.rn.rn.body[1] + 1)
            b = render(nT, cand.rn.rn.body[0], cand.rn.rn.body[1] + 1)
            d = same_lines(a, b)
            if d is not None:
                k, x, y = d
                R.fail("V1d", f"runNetwork differs from frozen at code line {k}: frozen "
                              f"`{x[1] if x else '<end>'}`, here `{y[1] if y else '<end>'}`")
            R.note("V1d", f"empty conversion: runNetwork's {len(a)} code lines equal frozen")
            return
        F, N = fz.rn.statements(), cand.rn.statements()
        cut = set()
        for d in SUBST.spec_cut:
            for loc, hits in d.locate(fz):
                for h in hits:
                    for k, (s, e) in enumerate(F):
                        if h.line <= fT.line(s) <= h.end:
                            cut.add(k)
        removed_names = set(SUBST.callable) | set(SUBST.bracket) | set(SUBST.type_move) | \
            set(SUBST.field_timing) | {LAMBDA_NAME, "engines"}
        traces_k = next((k for k, (s, e) in enumerate(F) if fT.join(s, e) == join_text("NetworkRunTraces traces(log_dir_);")), None)
        j = 0
        for i, (s, e) in enumerate(F):
            ds = parse_declaration(fT, s, e) or []
            names = [d.name for d in ds]
            if i in cut:
                # removed, `const T& name = engine_spec.<name>;`, or (an owned engine) its run
                # reference `T& name = run.<path>;`
                if j < len(N) and len(ds) == 1:
                    nd = parse_declaration(nT, *N[j]) or []
                    ok = len(nd) == 1 and nd[0].name == ds[0].name and nd[0].is_ref and nd[0].init and (
                        nT.join(*nd[0].init) == "engine_spec." + ds[0].name
                        or (ds[0].name in SUBST.owner and nT.join(*N[j]) == join_text(
                            f"{fT.join(*ds[0].type_span)}& {ds[0].name} = {SUBST.owner[ds[0].name]};")))
                    if ok:
                        self.claim(DRIVER_PATH, nT.line(N[j][0]), nT.line(N[j][1] - 1))
                        j += 1
                self.fclaim(DRIVER_PATH, fT.line(s), fT.line(e - 1))
                continue
            if names and any(n in SUBST.owner for n in names):
                n0 = names[0]
                want = join_text(f"{fT.join(*ds[0].type_span)}& {n0} = {SUBST.owner[n0]};")
                if j < len(N) and nT.join(*N[j]) == want:
                    self.claim(DRIVER_PATH, nT.line(N[j][0]), nT.line(N[j][1] - 1))
                    j += 1
                self.fclaim(DRIVER_PATH, fT.line(s), fT.line(e - 1))
                continue
            if names and any(n in removed_names for n in names) or fT.t(s) == "enum" and \
                    any(n in SUBST.type_move for n in names):
                self.fclaim(DRIVER_PATH, fT.line(s), fT.line(e - 1))
                continue
            if fT.t(s) == "return" and any(c.name == "networkTeardown" for c in calls_in(fT, s, e)):
                want = join_text("return networkTeardown(ctx, config_, log, traces, setup, NetworkRunEndInputs{ "
                                 "run.engines, run.emit, run.lanes, run.timing, run.closed_loop });")
                if j < len(N) and nT.join(*N[j]) == want:
                    self.claim(DRIVER_PATH, nT.line(N[j][0]), nT.line(N[j][1] - 1))
                    j += 1
                else:
                    R.fail("V1d", f"the run end is not `{want}`")
                self.fclaim(DRIVER_PATH, fT.line(s), fT.line(e - 1))
                continue
            fsubs = [Sub(c.tok, c.open + 1, f"{lint.EPOCH_FN}(run, ") for c in calls_in(fT, s, e)
                     if c.name == LAMBDA_NAME]
            a = render(fT, s, e, fsubs)
            k = None
            for q in range(j, min(len(N), j + 6)):
                if same_lines(a, render(nT, *N[q])) is None:
                    k = q
                    break
            if k is None:
                R.fail("V1d", f"frozen {DRIVER_PATH}:{fT.line(s)} is not in runNetwork: "
                              f"`{fT.join(s, min(e, s + 12))}...`")
                continue
            for q in range(j, k):
                R.fail("V1d", f"{DRIVER_PATH}:{nT.line(N[q][0])}: an undeclared statement in runNetwork: "
                              f"`{nT.join(N[q][0], min(N[q][1], N[q][0] + 12))}...`")
            if fsubs:
                self.claim(DRIVER_PATH, nT.line(N[k][0]), nT.line(N[k][1] - 1))
                self.fclaim(DRIVER_PATH, fT.line(s), fT.line(e - 1))
            j = k + 1
            if i == traces_k:
                for want in ("const NetworkEngineSpec engine_spec = networkEngineSpec(config_, nc, ctx);",
                             "NetworkRun run(ctx, config_, nc, log, traces, setup, log_dir_, engine_spec);"):
                    if j < len(N) and nT.join(*N[j]) == join_text(want):
                        self.claim(DRIVER_PATH, nT.line(N[j][0]), nT.line(N[j][1] - 1))
                        j += 1
                    else:
                        R.fail("V1d", f"`{want}` does not follow `NetworkRunTraces traces(log_dir_);`")
        for q in range(j, len(N)):
            R.fail("V1d", f"{DRIVER_PATH}:{nT.line(N[q][0])}: an undeclared statement in runNetwork")
        R.note("V1d", f"{len(F)} frozen runNetwork statements against {len(N)}")
        self.v1d_moved()

    def v1d_moved(self):
        """Where the spec 4.1 edits put what they took out of runNetwork, each against frozen,
        each region claimed for V1e: the third-band predicate's body (CALLABLE), the two
        bracket closures' bodies (BRACKET), the timer scratch in NetworkRunTiming and the
        hand-off fields in NetworkPostFitHandoff (FIELD_MOVE), LoopSignal in Application
        (TYPE_MOVE), and the declared header hunks of step EP1a-1 (the engines' owner and
        spec, the run's and the epoch's declarations, the build list)."""
        R, fz = self.R, self.fz
        fT = fz.rn.T
        froot = {d.name: d for d in fz.rn.scopes.root.decls}
        # CALLABLE and BRACKET: a function whose body is the closure's, under a binding preamble.
        moved = [(n, "network" + n[0].upper() + n[1:]) for n in SUBST.callable + SUBST.bracket]
        for closure, fname in moved:
            fd = froot.get(closure)
            if fd is None or fd.lambda_scope is None:
                R.fail("V1d", f"the frozen closure {closure} was not found")
                continue
            ls = fd.lambda_scope
            fbody = render(fT, fT.match[ls.end] + 1, ls.end)
            defs = find_defs(self.tree, fname)
            if len(defs) != 1:
                R.fail("V1d", f"{fname}: {len(defs)} definitions (the {closure} closure's body moves into one)")
                continue
            f = defs[0]
            T = self.tree.tokens(f.path)
            st = statements(T, f.body[0] + 1, f.body[1])
            own = {x.name for a2, b2 in statements(fT, fT.match[ls.end] + 1, ls.end)
                   for x in (parse_declaration(fT, a2, b2) or [])}
            k = 0
            while k < len(st):
                ds2 = parse_declaration(T, *st[k]) or []
                if not ds2 or any(x.name in own for x in ds2):
                    break
                k += 1
            lo = st[k][0] if k < len(st) else f.body[1]
            nbody = render(T, lo, f.body[1])
            shift = (fbody[0][0] - nbody[0][0]) if fbody and nbody else 0
            d = same_lines(fbody, nbody, shift)
            if d is not None:
                R.fail("V1d", f"{f.path}:{f.line}: {fname}'s body differs from the frozen {closure} closure's at "
                              f"code line {d[0]}: frozen `{d[1][1] if d[1] else '<end>'}`, here "
                              f"`{d[2][1] if d[2] else '<end>'}`")
            if closure in SUBST.callable:
                caps = _init_captures(fT, fd)
                mine = {x.name: x for q in range(k) for x in parse_declaration(T, *st[q]) or []}
                p0 = f.params[0].name if f.params else ""
                for cname, ctext in caps.items():
                    x = mine.get(cname)
                    want = ctext.replace("ctx.knobs", p0, 1) if ctext.startswith("ctx.knobs") else ctext
                    if x is None or not x.is_const or T.join(*x.init) != want:
                        R.fail("V1d", f"{fname}: the init-capture {cname} = {ctext} is not the const binding "
                                      f"`{cname} = {want}`")
            self.claim(f.path, T.line(decl_start(T, f)), T.line(f.body[1]))
            for g in find_decls(self.tree, fname):
                GT = self.tree.tokens(g.path)
                self.claim(g.path, GT.line(decl_start(GT, g)), GT.line(decl_end(GT, g) - 1))
        # FIELD_MOVE: the bracket scratch in NetworkRunTiming, with the frozen type and size.
        timing = struct_members(self.tree, "NetworkRunTiming") or {}
        for name, member in SUBST.field_timing.items():
            fd = froot.get(name)
            m = timing.get(member)
            if fd is None or m is None:
                R.fail("V1d", f"NetworkRunTiming::{member} (the frozen {name}) is missing")
                continue
            ft = fT.join(*fd.type_span)
            init = fT.join(*fd.init) if fd.init else ""
            if m["type"] != ft or m["init"].replace(" ", "") not in (f"{ft}({init})".replace(" ", ""),
                                                                    "{" + init + "}", f"({init})"):
                R.fail("V1d", f"NetworkRunTiming::{member} is `{m['type']} = {m['init']}`, frozen `{ft} {name}({init})`")
        # FIELD_MOVE: the hand-off fields.
        fres = struct_members(fz.tree, "NetworkSolveGlueResult") or {}
        nres = struct_members(self.tree, "NetworkSolveGlueResult") or {}
        for name, (struct, newpath) in SUBST.field_solve.items():
            grp, _, fld = newpath.partition(".")
            if name in nres:
                R.fail("V1d", f"NetworkSolveGlueResult still has {name} (moved to {newpath})")
            g = nres.get(grp)
            hs = struct_members(self.tree, g["type"]) if g else None
            if not hs or fld not in hs or fres.get(name, {}).get("type") != hs[fld]["type"]:
                R.fail("V1d", f"{struct}::{newpath}: missing, or not of the frozen type of {struct}::{name}")
        # TYPE_MOVE: LoopSignal is an Application member enum with the frozen enumerators.
        for name in SUBST.type_move:
            fd = froot.get(name) or next((d for d in fz.rn.lam_scope.parent.decls if d.name == name), None)
            AT = self.tree.tokens(APPLICATION_HPP)
            hit = next((j for j in range(AT.n - 3) if AT.t(j) == "enum" and AT.t(j + 1) == "class"
                        and AT.t(j + 2) == name), None)
            if hit is None:
                R.fail("V1d", f"{APPLICATION_HPP}: no `enum class {name}`")
                continue
            e = hit
            while AT.t(e) != ";":
                e += 1
            if fd is not None and AT.join(hit, e + 1) != fT.join(*fd.stmt):
                R.fail("V1d", f"{APPLICATION_HPP}:{AT.line(hit)}: {name} is `{AT.join(hit, e + 1)}`, frozen "
                              f"`{fT.join(*fd.stmt)}`")
            self.claim(APPLICATION_HPP, AT.line(hit), AT.line(e))
        # The declared header hunks of step EP1a-1.
        AT = self.tree.tokens(APPLICATION_HPP)
        for f in self.tree.index(APPLICATION_HPP).funcs:
            if f.name == lint.EPOCH_FN or (f.params and lint.is_epoch_form(f)):
                self.claim(APPLICATION_HPP, AT.line(decl_start(AT, f)), AT.line(decl_end(AT, f) - 1))
        for path, structs in ((GROUPS_HPP, ("NetworkEngineSpec", "NetworkRunEngines", "NetworkRunTiming")),
                              (SOLVE_HPP, ("NetworkPostFitHandoff", "NetworkSolveGlueResult"))):
            for st_name in structs:
                sp = struct_span(self.tree, path, st_name)
                if sp is None:
                    R.fail("V1d", f"{path}: struct {st_name} is missing")
                else:
                    self.claim(path, *sp)
                fsp = struct_span(fz.tree, path, st_name)
                if fsp is not None:
                    self.fclaim(path, *fsp)
        for path in (GROUPS_HPP, APPLICATION_HPP, RUN_HPP, lint.EPOCH_HPP):
            T = self.tree.tokens(path)
            if T is None:
                continue
            for j in range(T.n):
                if T.t(j) == "static_assert":
                    e2 = T.close(j + 1) + 1
                    if re.search(r"NetworkRun|NetworkEpoch", T.join(j, e2)):
                        self.claim(path, T.line(j), T.line(e2))
                elif T.t(j) == "friend" and T.t(j + 2) == "NetworkEpochContractProbe":
                    self.claim(path, T.line(j), T.line(j + 3))
        cm = self.tree.read("CMakeLists.txt") or ""
        for k, ln in enumerate(cm.split("\n"), 1):
            if any(pathlib.PurePosixPath(p).name in ln for p in NEW_FILES):
                self.claim("CMakeLists.txt", k, k)

    # ==== V12: the spec cut ===============================================================
    def cut_statements(self, fzlike):
        """[(entry, [(s, e)])]: the runNetwork statements each SPEC_CUT entry locates."""
        drv = fzlike.drv
        T = drv.T
        stmts = [x for x in drv.driver_statements() if x[0] < drv.lam_decl.tok]
        out = []
        for d in SUBST.spec_cut:
            picked = []
            for loc, hits in d.locate(fzlike):
                for h in hits:
                    picked += [(s, e) for (s, e) in stmts if h.line <= T.line(s) <= h.end
                               and T.line(e - 1) <= h.end]
            out.append((d, sorted(set(picked))))
        return out

    def v12(self):
        R, fz = self.R, self.fz
        fT = fz.drv.T
        fcut = self.cut_statements(fz)
        n = sum(len(x) for _, x in fcut)
        if not self.epoch:
            cfz = CandFz(self.tree)
            nT = cfz.drv.T
            ncut = self.cut_statements(cfz)
            fst = [x for x in fz.drv.driver_statements() if x[0] < fz.drv.lam_decl.tok]
            nst = [x for x in cfz.drv.driver_statements() if x[0] < cfz.drv.lam_decl.tok]
            for (d, fs), (_, ns) in zip(fcut, ncut):
                if len(fs) != len(ns):
                    R.fail("V12", f"{d.name}: {len(ns)} cut statements here, {len(fs)} frozen")
                for (a, b), (c, e) in zip(fs, ns):
                    dd = same_lines(render(fT, a, b), render(nT, c, e))
                    if dd is not None:
                        R.fail("V12", f"{d.name}: {DRIVER_PATH}:{nT.line(c)} differs from frozen "
                                      f"{fT.line(a)}: `{dd[2][1] if dd[2] else '<end>'}`")
                # The range: no statement other than the frozen ones between its first and last.
                if fs and ns:
                    f_in = [x for x in fst if fs[0][0] <= x[0] <= fs[-1][0]]
                    n_in = [x for x in nst if ns[0][0] <= x[0] <= ns[-1][0]]
                    ftxt = [fT.join(*x) for x in f_in]
                    for x in n_in:
                        if nT.join(*x) not in ftxt:
                            R.fail("V12", f"{d.name}: {DRIVER_PATH}:{nT.line(x[0])} entered the cut range: "
                                          f"`{nT.join(x[0], min(x[1], x[0] + 10))}...`")
                res = Resolver(cfz.drv.scopes, {id(cfz.drv.scopes.root): "D"}, (),
                               RunNetwork(self.tree).members)
                locs = {x.name for d2, sts in ncut for a2, b2 in sts
                        for x in (parse_declaration(nT, a2, b2) or [])}
                for s, e in ns:
                    self.spec_tokens(nT, s, e, res, f"{d.name}", locs)
            R.note("V12", f"empty conversion: {n} cut statements in place, character-exact, and their "
                          "reads checked")
            return
        self.v12_epoch(fcut)

    def spec_tokens(self, T, s, e, res, what, locals_=()):
        """No log / traces / setup read; calls only from the allowlist; every name read is the
        configuration, nc, the knobs, a local the cut declares (@p locals_) or a frozen alias
        of one of those (V12)."""
        R = self.R
        for j in ids_in(T, s, e):
            if T.t(j) in ("log", "traces", "setup", "log_manager_") and T.t(j + 1) != "(":
                R.fail("V12", f"{what}: {T.path}:{T.line(j)} reads `{T.t(j)}`")
        if res is not None:
            for j in ids_in(T, s, e):
                if T.t(j + 1) in ("(", "::") or T.t(j) in locals_:
                    continue
                end, _ = lint._chain_end(T, j, e)
                node = res.resolve(j, end)
                inner = node.inner if isinstance(node, Copy) else node
                if not isinstance(inner, Ref):
                    continue            # a type, an enumerator, a function: not storage
                r = inner.render()
                ok = r.startswith(("A:config_", "D:ctx.knobs", "D:nc")) or \
                    any(r == "D:" + n or r.startswith("D:" + n + ".") for n in locals_)
                if r.startswith("D:") and not ok:
                    R.fail("V12", f"{what}: {T.path}:{T.line(j)} reads `{T.t(j)}` = {r}, not the "
                                  "configuration, nc, the knobs or a local of the cut")
        decl_names = {d.name for d in (parse_declaration(T, s, e) or [])}
        for c in calls_in(T, s, e):
            if c.name in decl_names:
                continue
            if not SPEC_CALLS.match(c.name):
                R.fail("V12", f"{what}: {T.path}:{c.line} calls {c.name}, not in the allowlist")

    def v12_epoch(self, fcut):
        """networkEngineSpec against the frozen cut (spec 2.5, 4.1): every frozen cut statement
        is in it, in frozen order, character-exact, except a built local's declaration, which
        becomes a reference to its field (`T& name = spec.F;`) or a field initializer
        (`spec.F = <the frozen initializer>;`), and an engine construction, whose arguments
        become field initializers (checked by engine_args).  Every other statement of the
        function is `NetworkEngineSpec spec;`, `return spec;`, a field initializer an engine
        argument accounts for, or a frozen driver declaration repeated with its frozen text."""
        R, fz = self.R, self.fz
        fT = fz.drv.T
        idx = self.tree.index(RUN_CPP)
        f = next((g for g in (idx.funcs if idx else []) if g.name == "networkEngineSpec" and g.is_def), None)
        if f is None:
            R.fail("V12", f"{RUN_CPP}: networkEngineSpec is not defined")
            return
        T = self.tree.tokens(RUN_CPP)
        self.claim(RUN_CPP, 1, T.line(T.n - 1))
        body = statements(T, f.body[0] + 1, f.body[1])
        texts = [render(T, s, e) for s, e in body]
        used = [None] * len(body)
        frozen = sorted(((a, b, d) for d, sts in fcut for a, b in sts), key=lambda x: x[0])
        cursor = 0

        def verbatim(want, k):
            got = texts[k]
            if len(got) != len(want) or not got:
                return False
            shift = want[0][0] - got[0][0]
            return same_lines(want, got, shift) is None

        for a, b, d in frozen:
            ds = parse_declaration(fT, a, b) or []
            want = render(fT, a, b)
            k = next((k for k in range(cursor, len(body)) if used[k] is None and verbatim(want, k)), None)
            if k is not None:
                used[k], cursor = "cut", k + 1
                continue
            if ds and any(x.name in SUBST.owner for x in ds):
                continue          # an engine construction: engine_args checks its arguments
            if len(ds) == 1:
                nm, ftype = ds[0].name, fT.join(*ds[0].type_span)
                k = next((k for k in range(cursor, len(body)) if used[k] is None
                          and self._binds_field(T, *body[k], nm, ftype)), None)
                if k is not None:
                    used[k], cursor = "binding", k + 1
                    continue
                if ds[0].init and ds[0].init_kind == "=":
                    rhs = fT.join(*ds[0].init)
                    k = next((k for k in range(cursor, len(body)) if used[k] is None
                              and T.t(body[k][0]) == "spec" and T.t(body[k][0] + 1) == "."
                              and T.t(body[k][0] + 3) == "="
                              and T.join(body[k][0] + 4, body[k][1] - 1) == rhs), None)
                    if k is not None:
                        used[k], cursor = "initializer", k + 1
                        continue
            R.fail("V12", f"{d.name}: frozen {DRIVER_PATH}:{fT.line(a)} is not in networkEngineSpec "
                          f"in frozen order: `{fT.join(a, min(b, a + 12))}...`")
        fields = self.engine_args(body, T)
        drv = {x.name: x for x in fz.rn.scopes.root.decls}
        for k, (s, e) in enumerate(body):
            if used[k] is not None:
                continue
            txt = T.join(s, e)
            if txt in (join_text("NetworkEngineSpec spec;"), join_text("return spec;")):
                continue
            if T.t(s) == "spec" and T.t(s + 1) == "." and T.t(s + 3) == "=" and T.t(s + 2) in fields:
                continue
            ds = parse_declaration(T, s, e) or []
            if len(ds) == 1 and ds[0].name in drv:
                fd = drv[ds[0].name]
                if same_lines(render(fT, *fd.stmt), texts[k],
                              render(fT, *fd.stmt)[0][0] - texts[k][0][0]) is None:
                    continue
            R.fail("V12", f"{RUN_CPP}:{T.line(s)}: networkEngineSpec holds a statement that is neither "
                          f"a frozen cut statement nor a declared form: `{T.join(s, min(e, s + 12))}...`")
        for s, e in body:
            self.spec_tokens(T, s, e, None, "networkEngineSpec")
        R.note("V12", f"networkEngineSpec: {len(body)} statements against {len(frozen)} frozen cut "
                      f"statements; {len(fields)} engine arguments are spec fields")

    @staticmethod
    def _binds_field(T, s, e, name, ftype):
        ds = parse_declaration(T, s, e) or []
        if len(ds) != 1 or ds[0].name != name or not ds[0].is_ref or not ds[0].init:
            return False
        a, b = ds[0].init
        if not (T.t(a) == "spec" and T.t(a + 1) == "." and b == a + 3):
            return False
        nt = T.join(*ds[0].type_span)
        return nt in (ftype, ftype.replace("const ", "", 1), "auto")

    def engine_args(self, body, ST):
        """Each frozen engine construction's arguments (the OWNER_MOVE declarations that
        construct with arguments) against its new constructor's, position by position: equal
        text, or a spec field whose initializer in networkEngineSpec is the frozen expression,
        or the frozen expression with each built local replaced by its field.  Returns the set
        of field names the comparison accounted for."""
        R, fz = self.R, self.fz
        fT = fz.rn.T
        field_expr, local_field = {}, {}
        for s, e in body:
            if ST.t(s) == "spec" and ST.t(s + 1) == "." and ST.t(s + 3) == "=":
                field_expr[ST.t(s + 2)] = ST.join(s + 4, e - 1)
            for d in parse_declaration(ST, s, e) or []:
                if d.is_ref and d.init and ST.t(d.init[0]) == "spec" and ST.t(d.init[0] + 1) == ".":
                    local_field[d.name] = ST.t(d.init[0] + 2)
        for d in fz.rn.scopes.root.decls:      # a frozen built local whose initializer is a field's
            if d.init and d.init_kind == "=":
                txt = fT.join(*d.init)
                for fld, ex in field_expr.items():
                    if ex == txt and d.name not in local_field:
                        local_field[d.name] = fld
        news = {}
        for path, cls in ((GROUPS_HPP, "NetworkRunEngines"), (RUN_HPP, "NetworkRun")):
            idx = self.tree.index(path)
            T = self.tree.tokens(path)
            ctor = next((g for g in (idx.funcs if idx else []) if g.name == cls and g.cls == cls
                         and g.is_def and g.params and lint.ptype(g.params[-1]) == "const NetworkEngineSpec&"),
                        None)
            if ctor is None:
                R.fail("V12", f"{path}: no {cls} constructor taking the NetworkEngineSpec")
                continue
            for m, args in _mem_init_args(T, ctor).items():
                news[(cls, m)] = (T, args, ctor.params[-1].name)
        used = set()
        n = 0
        for name, rpath in SUBST.owner.items():
            fd = next((x for x in fz.rn.scopes.root.decls if x.name == name), None)
            if fd is None or not fd.init or fd.init_kind not in ("(", "{"):
                continue
            parts = rpath.split(".")
            key = ("NetworkRunEngines", parts[2]) if parts[1] == "engines" else ("NetworkRun", parts[1])
            if key not in news:
                R.fail("V12", f"{name}: {key[0]} does not construct {key[1]}")
                continue
            T, args, sname = news[key]
            fargs = _split_top(fT, *fd.init)
            if len(fargs) != len(args):
                R.fail("V12", f"{name}: {len(fargs)} frozen constructor arguments, {len(args)} in "
                              f"{key[0]}::{key[1]}")
                continue
            for k, ((a, b), (c, e)) in enumerate(zip(fargs, args)):
                n += 1
                fa, na = fT.join(a, b), T.join(c, e)
                na = re.sub(r"\bspec\.", sname + ".", na)
                if na == fa:
                    continue
                m = re.fullmatch(re.escape(sname) + r"\.(\w+)", na)
                if m and field_expr.get(m.group(1)) == fa:
                    used.add(m.group(1))
                    continue
                if m and local_field.get(fa) == m.group(1):
                    used.add(m.group(1))
                    continue
                sub = fa
                for loc, fld in local_field.items():
                    sub = re.sub(r"(?<![\w.])" + re.escape(loc) + r"\b", f"{sname}.{fld}", sub)
                if sub == na:
                    used.update(local_field[x] for x in local_field if re.search(r"\b" + x + r"\b", fa))
                    continue
                R.fail("V12", f"{name}: argument {k} is `{T.join(c, e)}` in {key[0]}::{key[1]}, frozen "
                              f"`{fa}`, and no field of networkEngineSpec holds it")
        return used

    # ==== V13: names ======================================================================
    def v13(self):
        R = self.R
        n = 0
        for r in self.rows:
            if isinstance(r.mech, Explicit):
                continue
            fdecl, fdef = self.fdecls.get(r.name, (None, None))
            if fdef is None:
                continue
            FT = self.fz.tree.tokens(r.cpp)
            need = _preflight(FT, fdef.body[0] + 1, fdef.body[1], set(r.outs) & set(GROUP_TYPES))
            if r.name in self.converted and r.mech != ADAPTER:
                for f in self.new_defs(r.name):
                    n += 1
                    names = [p.name for p in f.params]
                    slots = [("run", 0)] + ([("ep", 1)] if r.mech != RUN else [])
                    k0 = 2 + len(r.kept)
                    if not lint.is_card_form(f):
                        slots += [(o, k0 + q) for q, o in enumerate(r.outs) if o in GROUP_TYPES]
                    for base, pos in slots:
                        got = names[pos] if pos < len(names) else None
                        fallback = "n" + base if base in ("run", "ep") else base + "_out"
                        if base in need:
                            ok = got == fallback
                        elif need:
                            ok = got in (base, fallback)
                        else:
                            ok = got == base
                        if not ok:
                            R.fail("V13", f"{f.path}:{f.line}: {r.name}'s parameter {pos} is `{got}`; the "
                                          f"pre-flight requires `{fallback if base in need else base}` "
                                          f"(the frozen body uses {sorted(need) or 'none of run, ep, the outputs'})")
            # Every definition of the seam in the tree: a parameter named run / ep / nrun /
            # nep that the frozen definition did not have, or that collides with a name the
            # body declares, fails.
            frozen_names = {p.name for p in (fdef.params or [])}
            idx = self.tree.index(r.cpp)
            for f in (idx.funcs if idx else []):
                if f.name != r.name or not f.is_def or f.params is None:
                    continue
                if lint.is_epoch_form(f) and r.name in self.converted:
                    continue
                n += 1
                NT = self.tree.tokens(r.cpp)
                body_names = _preflight(NT, f.body[0] + 1, f.body[1], set(), declared_only=True)
                for p in f.params:
                    if p.name in ("run", "ep", "nrun", "nep") and p.name not in frozen_names:
                        why = (f"; the body declares `{p.name}` too" if p.name in body_names else "")
                        R.fail("V13", f"{r.cpp}:{f.line}: {r.name} has a parameter named `{p.name}` "
                                      f"(not a converted epoch form){why}")
        fb = sorted(r.name for r in self.rows if not isinstance(r.mech, Explicit) and r.mech != ADAPTER
                    and self.fdecls.get(r.name, (None, None))[1] is not None
                    and _preflight(self.fz.tree.tokens(r.cpp), self.fdecls[r.name][1].body[0] + 1,
                                   self.fdecls[r.name][1].body[1], set(r.outs) & set(GROUP_TYPES)))
        R.note("V13", f"{n} definitions checked; the frozen pre-flight names {len(fb)} fallback seams: "
                      + ", ".join(fb))

    # ==== V2: signatures ==================================================================
    def v2(self):
        R = self.R
        if not self.converted:
            for r in self.rows:
                if isinstance(r.mech, Explicit):
                    continue
                if self.new_defs(r.name):
                    R.fail("V2", f"{r.name} has an epoch-form definition but its rows are not converted "
                                 "(--rows)")
            if not R.problems["V2"]:
                R.na("V2", "no seam converted (--rows names none)")
            return
        solve_k = next((k for k, c in enumerate(self.fz.calls) if c.name == lint.SOLVE), None)
        for name in sorted(self.converted):
            r = self.by_name[name]
            fdecl, fdef = self.fdecls[name]
            defs = self.new_defs(name)
            hidx = self.tree.index(r.hpp)
            decls = [f for f in (hidx.funcs if hidx else []) if f.name == name and not f.is_def
                     and f.params is not None and lint.is_epoch_form(f)]
            if len(defs) != 1 or len(decls) != 1:
                R.fail("V2", f"{name}: {len(decls)} epoch-form declarations in {r.hpp}, {len(defs)} "
                             "definitions (one each expected)")
                continue
            sites = [k for k, c in enumerate(self.fz.calls) if c.name == name]
            presolve = solve_k is None or not sites or sites[0] <= solve_k
            card_form = self.presolve_writer == "epoch" and r.outs and r.mech != RUN and presolve
            want = ["NetworkRun&"] if r.mech == RUN else ["NetworkRun&", "const NetworkEpoch&"]
            if card_form:
                want = ["NetworkRun&", "NetworkEpoch&"]
            fparams = {p.name: p for p in fdecl.params}
            for kp in r.kept:
                p = fparams.get(kp)
                want.append(lint.ptype(p) if p else "?")
            if not card_form:
                for o in r.outs:
                    if o in GROUP_TYPES:
                        want.append(GROUP_TYPES[o] + "&")
                    else:
                        p = fparams.get(o)
                        want.append(lint.ptype(p) if p else "?")
            DT0, HT0 = self.tree.tokens(defs[0].path), self.tree.tokens(decls[0].path)
            hd = doc_before(HT0, decl_start(HT0, decls[0]))
            self.claim(decls[0].path, hd[1] if hd else decls[0].line, HT0.line(decl_end(HT0, decls[0]) - 1))
            self.claim(defs[0].path, DT0.line(decl_start(DT0, defs[0])), DT0.line(defs[0].body[0]))
            if r.mech != ADAPTER and fdecl is not None:
                FH = self.fz.tree.tokens(r.hpp)
                self.fclaim(r.hpp, FH.line(decl_start(FH, fdecl)), FH.line(decl_end(FH, fdecl) - 1))
            for f in (decls[0], defs[0]):
                got = [lint.ptype(p) for p in f.params]
                if got != want:
                    R.fail("V2", f"{f.path}:{f.line}: {name}({', '.join(got)}); expected ({', '.join(want)})")
            for kp in r.kept:
                p = next((q for q in decls[0].params if q.name == kp), None)
                if p is None or join_text(p.text) != join_text(fparams[kp].text):
                    R.fail("V2", f"{name}: kept parameter {kp} does not keep its frozen declaration")
            if not presolve:
                bad = [o for o in r.outs if o not in ("handoff", "sol")]
                if bad:
                    R.fail("V2", f"{name}: after the solve it writes {bad}")
        R.note("V2", f"{len(self.converted)} converted seams' signatures checked")

    # ==== V1b: converted bodies ===========================================================
    def v1b(self):
        R = self.R
        if not self.converted:
            R.na("V1b", "no seam converted")
            return
        n = 0
        for name in sorted(self.converted):
            r = self.by_name[name]
            if r.mech == ADAPTER:
                for f in self.new_defs(name):
                    NT = self.tree.tokens(f.path)
                    st = statements(NT, f.body[0] + 1, f.body[1])
                    self.claim(f.path, NT.line(decl_start(NT, f)), NT.line(f.body[1]))
                    if len(st) != 1 or not any(c.name == name for c in calls_in(NT, *st[0])):
                        R.fail("V1b", f"{f.path}:{f.line}: {name}'s epoch form is not one forwarding call")
                continue
            fdecl, fdef = self.fdecls[name]
            FT = self.fz.tree.tokens(r.cpp)
            for f in self.new_defs(name):
                NT = self.tree.tokens(f.path)
                sp = lint.preamble_span(NT, f)
                if sp is None:
                    R.fail("V1b", f"{f.path}:{f.line}: {name} has no generated binding markers")
                    continue
                self.claim(f.path, NT.line(decl_start(NT, f)), NT.line(sp[1] - 1) if sp[1] > sp[0] else f.line)
                self.fclaim(r.cpp, FT.line(decl_start(FT, fdef)), FT.line(fdef.body[0]))
                names = {p.name for p in fdef.params} | set(SUBST.callable)
                for s2, e2 in statements(NT, *sp):
                    ds2 = parse_declaration(NT, s2, e2) or []
                    if not ds2 or any(x.name not in names for x in ds2):
                        R.fail("V1b", f"{f.path}:{NT.line(s2)}: {name}'s bindings hold a statement that "
                                      f"binds no frozen parameter: `{NT.join(s2, min(e2, s2 + 10))}...`")
                a = render(FT, fdef.body[0] + 1, fdef.body[1])
                b = render(NT, sp[1], f.body[1])
                edited = []
                if name == lint.SOLVE:
                    a0 = a
                    a, b, edited = _solve_declared(a, b, self.R,
                                                   card=lint.is_card_form(f))
                d = same_lines(a, b)
                for k in edited:
                    if (d is None or k < d[0]) and k < len(b):
                        self.claim(f.path, b[k][2], b[k][2])
                        self.fclaim(r.cpp, a0[k][2], a0[k][2])
                n += 1
                if d is not None:
                    k, x, y = d
                    R.fail("V1b", f"{f.path}: {name}'s body below its bindings differs from frozen at code "
                                  f"line {k}: frozen `{x[1] if x else '<end>'}`, here `{y[1] if y else '<end>'}`")
        R.note("V1b", f"{n} converted bodies compared with frozen")

    # ==== V3a: bindings ===================================================================
    def v3a(self):
        R = self.R
        if not self.converted:
            R.na("V3a", "no seam converted")
            return
        cand = self.cand
        nT = cand.T
        nres = cand.resolver()
        n = 0
        for name in sorted(self.converted):
            r = self.by_name[name]
            fdecl, fdef = self.fdecls[name]
            fsites = [c for c in self.fz.calls if c.name == name]
            nsites = [c for c in calls_in(nT, cand.open + 1, cand.close) if c.name == name]
            if len(fsites) != len(nsites):
                R.fail("V3a", f"{name}: {len(fsites)} frozen call sites, {len(nsites)} here")
                continue
            defs = self.new_defs(name)
            if not defs:
                continue
            f = defs[0]
            DT = self.tree.tokens(f.path)
            for fc, nc in zip(fsites, nsites):
                actual = {}
                for k, p in enumerate(f.params):
                    if k < nc.nargs:
                        actual[p.name] = nres.resolve(*nc.args[k])
                kept_text = {p.name: nT.join(*nc.args[k]) for k, p in enumerate(f.params) if k < nc.nargs}
                n += self.compare_site(r, fdecl, fc, f, DT, actual, kept_text, f"{nT.path}:{nc.line}")
        n += self.replay_sites(nres)
        self.other_callers()
        R.note("V3a", f"{n} parameter bindings compared")

    def compare_site(self, r, fdecl, fc, f, DT, actual, kept_text, where):
        """Every frozen parameter's binding at frozen call site @p fc against the converted
        definition @p f evaluated with its parameters bound to @p actual (V3a)."""
        R, fT, fres = self.R, self.fz.drv.T, self.fz.res
        b = self.new_bindings(r, f, DT, actual)
        n = 0
        for k, p in enumerate(fdecl.params):
            if k >= fc.nargs:
                continue
            if p.name in r.kept:
                ftext = fT.join(*fc.args[k])
                if kept_text.get(p.name) != ftext:
                    R.fail("V3a", f"{r.name} at {where}: kept {p.name} is `{kept_text.get(p.name)}`, frozen `{ftext}`")
                continue
            if p.kind == "F":
                self.removed_param(r, p, f, DT)
                continue
            n += 1
            want = apply_declarator(p.kind, frozen_canon(fres.resolve(*fc.args[k])))
            if fT.join(*fc.args[k]).replace(" ", "") == (SUBST.value_equal or "?").replace(" ", ""):
                want = apply_declarator(p.kind, Ref("L:emitted"))
            got = b.get(p.name)
            if got is None:
                R.fail("V3a", f"{r.name}: no binding line for frozen parameter {p.name}")
                continue
            node, dtext, written = got
            if dtext is not None and join_text(dtext) != join_text(p.text):
                R.fail("V3a", f"{r.name}: {p.name} is declared `{dtext}`, frozen `{p.text}`")
            if dtext is not None and r.mech != ADAPTER:
                is_out = isinstance(node, Ref) and node.root in self.canon.ep_keys and p.kind in ("W", "W*")
                if is_out != written:
                    R.fail("V3a", f"{r.name}: the binding of {p.name} "
                                  + ("writes the epoch but is not marked `// written`" if is_out
                                     else "is marked `// written` but writes no epoch member"))
            gw = normalize(self.canon(node), self.const_roots)
            ww = normalize(want, self.const_roots)
            if show(gw) != show(ww):
                R.fail("V3a", f"{r.name} at {where}: {p.name} binds {show(gw)}, frozen {show(ww)}")
        return n

    def replay_sites(self, nres):
        """CALLBACK_DIRECT: the solve's replay calls the two builders directly; at each such
        call the builders' bindings, evaluated through the solve's own call site, must be the
        objects the frozen closures captured (the frozen primary call inside each closure)."""
        R = self.R
        if lint.SOLVE not in self.converted:
            return 0
        cand = self.cand
        sdefs = self.new_defs(lint.SOLVE)
        ssite = [c for c in calls_in(cand.T, cand.open + 1, cand.close) if c.name == lint.SOLVE]
        if not sdefs or not ssite:
            return 0
        sd = sdefs[0]
        ST = self.tree.tokens(sd.path)
        s_actual = {p.name: nres.resolve(*ssite[0].args[k]) for k, p in enumerate(sd.params)
                    if k < ssite[0].nargs}
        ssc = Scopes(ST, sd.body[0], sd.body[1], sd.params)
        sres = Resolver(ssc, {id(ssc.root): "S"}, (), set(), params=s_actual)
        n = 0
        for cb in SUBST.callbacks:
            builder = "network" + cb[0].upper() + cb[1:]
            r = self.by_name.get(builder)
            if r is None or builder not in self.converted:
                R.fail("V3a", f"{lint.SOLVE} is converted but {builder} is not (CALLBACK_DIRECT needs both)")
                continue
            calls = [c for c in calls_in(ST, sd.body[0] + 1, sd.body[1]) if c.name == builder]
            if not calls:
                R.fail("V3a", f"{lint.SOLVE}: the replay does not call {builder} directly")
                continue
            fdecl, _ = self.fdecls[builder]
            fc = next((c for c in self.fz.calls if c.name == builder), None)
            bdefs = self.new_defs(builder)
            if fc is None or not bdefs:
                continue
            bd = bdefs[0]
            BT = self.tree.tokens(bd.path)
            for c in calls:
                actual = {p.name: sres.resolve(*c.args[k]) for k, p in enumerate(bd.params) if k < c.nargs}
                n += self.compare_site(r, fdecl, fc, bd, BT, actual, {}, f"{sd.path}:{c.line} (replay)")
        return n

    def new_bindings(self, r, f, DT, actual):
        """{frozen parameter name: (node, declarator text or None, written)} of a converted
        definition: its binding lines (or an adapter's forwarding arguments), resolved with the
        definition's parameters bound to @p actual."""
        out = {}
        sc = Scopes(DT, f.body[0], f.body[1], f.params)
        res = Resolver(sc, {id(sc.root): "S"}, (), set(), params=actual)
        for p in f.params[2:]:
            out[p.name] = (actual.get(p.name), p.text, True)
        if r.mech == ADAPTER:
            st = statements(DT, f.body[0] + 1, f.body[1])
            fwd = next((c for c in calls_in(DT, *st[0]) if c.name == r.name), None) if st else None
            fdecl = self.fdecls[r.name][0]
            if fwd is not None:
                for k, p in enumerate(fdecl.params):
                    if k < fwd.nargs:
                        out[p.name] = (apply_declarator(p.kind, res.resolve(*fwd.args[k])), None, False)
            return out
        sp = lint.preamble_span(DT, f)
        if sp is None:
            return out
        for s, e in statements(DT, sp[0], sp[1]):
            for d in parse_declaration(DT, s, e) or []:
                if not d.init:
                    continue
                node = res.resolve(*d.init)
                if not d.is_ref and not d.is_ptr:
                    node = Copy(node) if isinstance(node, (Ref, Copy)) else node
                text = DT.src(s, d.init[0] - 1).strip() if DT.t(d.init[0] - 1) == "=" else None
                line_end = DT.source.find("\n", DT[e - 1].end)
                written = "// written" in DT.source[DT[e - 1].end:line_end if line_end >= 0 else None]
                out[d.name] = (node, text, written)
        return out

    def removed_param(self, r, p, f, DT):
        if p.name in SUBST.callable:
            sp = lint.preamble_span(DT, f)
            a, b = sp if sp else (f.body[0], f.body[1])
            txt = DT.src(a, b)
            if p.name not in txt or "networkIsUncarriedThirdBand" not in txt:
                self.R.fail("V3a", f"{r.name}: no binding of {p.name} forwarding to networkIsUncarriedThirdBand")
        elif p.name not in SUBST.callbacks:
            self.R.fail("V3a", f"{r.name}: std::function parameter {p.name} removed without a declared class")

    def other_callers(self):
        """Adapters' other callers keep their frozen text (spec 3.6)."""
        names = {n for n in self.converted if self.by_name[n].mech == ADAPTER}
        for path in self.fz.tree.files("src", "tests"):
            if not path.endswith((".cpp", ".hpp")) or path == DRIVER_PATH:
                continue
            text = self.fz.tree.read(path)
            if not text or not any(n in text for n in names):
                continue
            FT, NT = self.fz.tree.tokens(path), self.tree.tokens(path)
            if NT is None:
                self.R.fail("V3a", f"{path}: missing (it calls an adapter)")
                continue
            fc = [c for c in calls_in(FT, 0, FT.n) if c.name in names]
            nc = [c for c in calls_in(NT, 0, NT.n) if c.name in names]
            fdecls = {f.name_tok for f in self.fz.tree.index(path).funcs}
            ndecls = {f.name_tok for f in self.tree.index(path).funcs}
            fc = [c for c in fc if c.tok not in fdecls]
            nc = [c for c in nc if c.tok not in ndecls]
            fn_ = [FT.join(c.tok, c.close + 1) for c in fc]
            nn_ = [NT.join(c.tok, c.close + 1) for c in nc]
            for x in fn_:
                if x not in nn_:
                    self.R.fail("V3a", f"{path}: the adapter call `{x[:80]}...` changed")

    # ==== V3f: fold grammar ===============================================================
    def v3f(self):
        R = self.R
        if not self.converted:
            R.na("V3f", "no seam converted")
            return
        n = 0
        for name in sorted(self.converted):
            r = self.by_name[name]
            for f in self.new_defs(name):
                DT = self.tree.tokens(f.path)
                sc = Scopes(DT, f.body[0], f.body[1], f.params)
                res = Resolver(sc, {id(sc.root): "S"}, ())
                spans = []
                if r.mech == ADAPTER:
                    st = statements(DT, f.body[0] + 1, f.body[1])
                    fwd = next((c for c in calls_in(DT, *st[0]) if c.name == name), None) if st else None
                    spans = list(fwd.args) if fwd else []
                else:
                    sp = lint.preamble_span(DT, f)
                    for s, e in (statements(DT, *sp) if sp else []):
                        for d in parse_declaration(DT, s, e) or []:
                            if d.init and d.kind != "closure":
                                spans.append(d.init)
                for s, e in spans:
                    n += 1
                    why = fold_problem(res.resolve(s, e))
                    if why:
                        R.fail("V3f", f"{f.path}:{DT.line(s)}: {name}: `{DT.join(s, e)}` is outside the fold "
                                      f"grammar: {why}")
        R.note("V3f", f"{n} folded expressions checked")

    # ==== V3c: realization ================================================================
    def v3c(self):
        R = self.R
        if not self.epoch:
            R.na("V3c", "empty conversion: every epoch local keeps its frozen declaration (V1a)")
            return
        nT, N = self.cand.T, self.cand.stmts
        hdr = self.tree.read(lint.EPOCH_HPP)
        if hdr is None:
            R.fail("V3c", f"{lint.EPOCH_HPP} does not exist")
            return
        groups = struct_members(self.tree, "NetworkEpoch")
        for local, (path, tr) in SUBST.epoch.items():
            if tr == "CLOCK":
                continue            # kept const locals; the store and EpochClock are checked below
            rec = self.realized.get(local)
            if rec is None:
                R.fail("V3c", f"{local}: not realized")
                continue
            parts = path.split(".")
            member = None
            if len(parts) == 2:
                member = groups.get(parts[1])
            elif len(parts) == 3 and parts[1] in groups:
                gtype = groups[parts[1]]["type"].replace("const ", "")
                member = struct_members(self.tree, gtype).get(parts[2])
            if member is None:
                R.fail("V3c", f"{local}: {path} is not a member path of NetworkEpoch")
                continue
            ftype = rec.get("type") or ""
            mt = member["type"]
            if tr not in ("WS",) and ftype and mt.replace("const ", "") != ftype.replace("const ", ""):
                R.fail("V3c", f"{local}: member {path} has type `{mt}`, the local `{ftype}`")
            if tr == "SCALAR":
                if member["init"] != rec.get("init"):
                    R.fail("V3c", f"{local}: member {path} default `{member['init']}`, frozen initializer "
                                  f"`{rec.get('init')}`")
            elif member["init"] not in ("", "{}") and tr not in ("CLOCK",):
                R.fail("V3c", f"{local}: member {path} has a default initializer `{member['init']}`")
            at = rec.get("stmt")
            if at is None:
                at = rec.get("dropped_at", 0)
            for k in range(0, min(at, len(N))):
                s, e = N[k]
                for j in range(s, e):
                    if nT.t(j) == "ep" and nT.t(j - 1) not in (".", "->"):
                        q = ep_path_tokens(nT, j, e)
                        p2 = norm_subs(nT, j, q, ())
                        if p2 == path or p2.startswith(path + "."):
                            R.fail("V3c", f"{nT.path}:{nT.line(j)}: {p2} is bound before its realization")
        clock = struct_members(self.tree, "EpochClock")
        clk = [n for n, (p, t) in SUBST.epoch.items() if t == "CLOCK"]
        if list(clock.keys())[:len(clk)] != clk:
            R.fail("V3c", f"EpochClock members {list(clock.keys())}, the CLOCK store's order is {clk}")
        R.note("V3c", f"{len(SUBST.epoch)} epoch members checked against their locals")

    # ==== V5: deep const ==================================================================
    def v5(self):
        R = self.R
        if not self.epoch or self.tree.read(lint.EPOCH_HPP) is None:
            R.na("V5", "no NetworkEpoch in this tree")
            return
        seen, stack, n = set(), [("NetworkEpoch", "ep")], 0
        while stack:
            name, where = stack.pop()
            if name in seen:
                continue
            seen.add(name)
            ms = struct_members(self.tree, name)
            if ms is None:
                continue
            for m, info in ms.items():
                n += 1
                t = info["type"] + info["ops"]
                if re.search(r"\b(unique_ptr|shared_ptr|weak_ptr|function|reference_wrapper)\b", t):
                    R.fail("V5", f"{name}::{m} ({where}.{m}) is `{t}`")
                if "&" in info["ops"] and not (name == "NetworkEpoch" and m == "se" and t.startswith("const")):
                    R.fail("V5", f"{name}::{m} is a reference `{t}`")
                if "*" in info["ops"] and not info["type"].startswith("const"):
                    R.fail("V5", f"{name}::{m} is a pointer to non-const `{t}`")
                for ident in re.findall(r"\b([A-Z]\w+)\b", info["type"]):
                    if ident not in seen:
                        stack.append((ident, f"{where}.{m}"))
        R.note("V5", f"{len(seen)} types, {n} members scanned from NetworkEpoch")

    # ==== V7: documentation ===============================================================
    def v7(self):
        R = self.R
        if not self.converted:
            R.na("V7", "no seam converted")
            return
        for name in sorted(self.converted):
            r = self.by_name[name]
            fdecl, _ = self.fdecls[name]
            FT = self.fz.tree.tokens(r.hpp)
            fdoc = doc_before(FT, decl_start(FT, fdecl))
            hidx = self.tree.index(r.hpp)
            NT = self.tree.tokens(r.hpp)
            decls = [f for f in (hidx.funcs if hidx else []) if f.name == name and not f.is_def
                     and f.params and lint.is_epoch_form(f)]
            if not decls:
                continue
            ndoc = doc_before(NT, decl_start(NT, decls[0]))
            self.claim(r.hpp, ndoc[1] if ndoc else decls[0].line, NT.line(decl_end(NT, decls[0]) - 1))
            if not ndoc:
                R.fail("V7", f"{r.hpp}:{decls[0].line}: {name}'s epoch form has no Doxygen block")
                continue
            ntext = doc_words(ndoc[0])
            # An adapter's explicit declaration keeps its Doxygen (V1c), @param texts included,
            # directly above; its epoch form names the bindings.
            for pname, ptext in ([] if r.mech == ADAPTER else params_doc(fdoc[0] if fdoc else "")):
                if pname in r.kept:
                    continue
                t = doc_words(ptext)
                if t and t not in ntext:
                    R.fail("V7", f"{r.hpp}: {name}: the frozen @param {pname} text is not carried")
            for f in self.new_defs(name):
                DT = self.tree.tokens(f.path)
                for path in sorted({p for p, _ in lint.binding_reads(DT, f)}):
                    leafp = path.split(".", 1)[1]
                    if leafp not in ntext and path not in ntext:
                        R.fail("V7", f"{r.hpp}: {name}: the bound member {path} is not named in its Doxygen")
        R.note("V7", f"{len(self.converted)} converted declarations' documentation checked")

    # ==== V9: no new overload =============================================================
    def v9(self):
        R = self.R
        if not self.epoch:
            R.na("V9", "empty conversion: no new header")
            return
        allowed = {"networkEngineSpec", "networkSubBegin", "networkSubEnd", "x_emit", "NetworkRun",
                   "NetworkEpoch", "operator="}
        for h in (RUN_HPP, lint.EPOCH_HPP):
            idx = self.tree.index(h)
            if idx is None:
                R.fail("V9", f"{h} is missing")
                continue
            for f in idx.funcs:
                if f.name not in allowed and not f.cls:
                    R.fail("V9", f"{h}:{f.line}: declares {f.name}")
            self.claim(h, 1, self.tree.tokens(h).line(self.tree.tokens(h).n - 1))
        fg, ng = include_graph(self.fz.tree), include_graph(self.tree)
        newh = {RUN_HPP, lint.EPOCH_HPP}
        fmemo, nmemo = {}, {}

        def declared_names(tree, memo, path):
            if path not in memo:
                idx = tree.index(path)
                memo[path] = {f.name for f in (idx.funcs if idx else []) if not f.cls}
            return memo[path]

        n = 0
        for tu in [p for p in self.tree.files("src", "tests") if p.endswith(".cpp")]:
            if tu not in fg:
                continue                  # a new translation unit has no frozen calls
            nreach = reach(ng, tu) | {tu}
            if not (nreach & newh):
                continue
            n += 1
            old = set().union(*(declared_names(self.fz.tree, fmemo, h) for h in reach(fg, tu) | {tu}))
            new = set().union(*(declared_names(self.tree, nmemo, h) for h in nreach))
            FT = self.fz.tree.tokens(tu)
            called = {leaf(c.name) for c in calls_in(FT, 0, FT.n)}
            hit = ((new - old) & called) - allowed
            if hit:
                R.fail("V9", f"{tu}: newly visible {sorted(hit)} are names its frozen code calls")
        R.note("V9", f"{n} translation units newly reach the run or epoch header")

    # ==== V10, V11 ========================================================================
    def v10_11(self):
        R = self.R
        if not self.epoch or self.tree.read(lint.EPOCH_HPP) is None:
            R.na("V10", "no epoch function or NetworkEpoch.hpp")
            R.na("V11", "no epoch function or NetworkEpoch.hpp")
            return
        lt = lint.Lint(self.tree)
        lt.v10()
        if self.presolve_writer == "epoch":
            for p in lt.problems["V10"]:
                R.fail("V10", p)
            for p in lt.info["V10"]:
                R.note("V10", p)
            R.na("V11", "--presolve-writer epoch: a pre-solve writer takes the whole epoch, so its "
                        "spine line names no group the banner could be checked against")
            return
        lt.v11(partial=len(self.converted) < len([r for r in self.rows if not isinstance(r.mech, Explicit)]))
        for chk in ("V10", "V11"):
            for p in lt.problems[chk]:
                R.fail(chk, p)
            for p in lt.info[chk]:
                R.note(chk, p)

    # ==== V1e: every hunk accounted for ===================================================
    def v1e(self):
        R = self.R
        paths = set(p for p in self.fz.tree.files("src", "include", "tests", "CMakeLists.txt"))
        paths |= set(p for p in self.tree.files("src", "include", "tests", "CMakeLists.txt"))
        n_files = n_lines = 0
        for path in sorted(paths):
            if not path.endswith((".cpp", ".hpp", ".h", ".txt", ".cmake")):
                continue
            ft, nt = self.fz.tree.read(path), self.tree.read(path)
            if ft == nt:
                continue
            if ft is None:
                if path in NEW_FILES and self.epoch:
                    continue
                R.fail("V1e", f"{path}: a new file outside the step's declared new files")
                continue
            if nt is None:
                R.fail("V1e", f"{path}: deleted")
                continue
            fl = _code_lines(ft)
            nl = _code_lines(nt)
            if [x[1] for x in fl] == [x[1] for x in nl]:
                continue
            n_files += 1
            sm = difflib.SequenceMatcher(None, [x[1] for x in fl], [x[1] for x in nl], autojunk=False)
            for op, a0, a1, b0, b1 in sm.get_opcodes():
                if op == "equal":
                    continue
                for q in range(b0, b1):
                    ln, txt = nl[q]
                    n_lines += 1
                    if re.match(r"^[{}]+;?$", txt.strip()) and any(
                            0 <= x < len(nl) and self.claimed(path, *nl[x]) for x in (q - 1, q + 1)):
                        continue      # a lone brace the diff paired on the far side of a region
                    if not self.claimed(path, ln, txt):
                        R.fail("V1e", f"{path}:{ln}: an unaccounted change: `{txt.strip()[:90]}`")
                for q in range(a0, a1):
                    ln, txt = fl[q]
                    if not self.fclaimed(path, ln, txt):
                        R.fail("V1e", f"{path}: frozen line {ln} removed or changed outside a declared "
                                      f"region: `{txt.strip()[:90]}`")
        R.note("V1e", f"{n_files} changed files, {n_lines} changed code lines")

    def claimed(self, path, ln, txt):
        s = txt.strip()
        if s.startswith("#include") or re.match(r"^(struct|class)\s+\w+;$", s) or \
                re.match(r"^namespace\s+\w+\s*\{\s*(class|struct)\s+\w+;\s*\}$", s):
            return True
        return any(a <= ln <= b for a, b in self.claims.get(path, ()))

    def fclaimed(self, path, ln, txt):
        s = txt.strip()
        if s.startswith("#include") or re.match(r"^(struct|class)\s+\w+;$", s):
            return True
        return any(a <= ln <= b for a, b in self.frozen_claims.get(path, ()))


# ---------------------------------------------------------------------------------------------
# Helpers of the checks
# ---------------------------------------------------------------------------------------------
def _drop_nested(subs):
    subs = sorted(subs, key=lambda x: (x.a, -(x.b - x.a)))
    out, end = [], -1
    for x in subs:
        if x.a >= end:
            out.append(x)
            end = x.b
    return out


def _split_top(T, a, b):
    """Argument spans of tokens [a, b) split at top-level commas."""
    out, start, d = [], a, 0
    for j in range(a, b):
        v = T.t(j)
        if T.k(j) == "op" and v in "([{":
            d += 1
        elif T.k(j) == "op" and v in ")]}":
            d -= 1
        elif d == 0 and v == ",":
            out.append((start, j))
            start = j + 1
    if start < b:
        out.append((start, b))
    return out


def _mem_initializers(T, f):
    """{member: the single name it is initialized from} of a constructor's initializer list."""
    out = {}
    k = f.paren[1] + 1
    if T.t(k) != ":":
        return out
    k += 1
    end = f.body[0] if f.body else T.n
    while k < end:
        if T.k(k) == "id" and T.t(k + 1) in ("(", "{"):
            c = T.close(k + 1)
            inner = [T.t(q) for q in range(k + 2, c)]
            out[T.t(k)] = inner[0] if len(inner) == 1 else " ".join(inner)
            k = c + 1
            continue
        k += 1
    return out


def find_defs(tree, name):
    """Free-function definitions of @p name under src/ and include/."""
    out = []
    for p in tree.files("src", "include"):
        if not p.endswith((".cpp", ".hpp", ".h")):
            continue
        text = tree.read(p)
        if not text or name not in text:
            continue
        out += [f for f in tree.index(p).funcs if f.name == name and f.is_def and not f.cls]
    return out


def find_decls(tree, name):
    out = []
    for p in tree.files("include"):
        text = tree.read(p)
        if not text or name not in text:
            continue
        out += [f for f in tree.index(p).funcs if f.name == name and not f.is_def]
    return out


def struct_span(tree, path, name):
    """(first line, last line) of the definition of struct / class @p name in @p path."""
    T = tree.tokens(path)
    if T is None:
        return None
    for i in range(T.n - 2):
        if T.t(i) in ("struct", "class") and T.t(i + 1) == name:
            k = i + 2
            while k < T.n and T.t(k) not in ("{", ";"):
                k += 1
            if T.t(k) == "{":
                return T.line(i), T.line(T.close(k) + 1)
    return None


def _init_captures(T, closure):
    """{name: initializer text} of a closure declaration's init-captures."""
    out = {}
    a, b = closure.init
    if T.t(a) != "[":
        return out
    c = T.close(a)
    for s, e in _split_top(T, a + 1, c):
        if T.t(s + 1) == "=":
            out[T.t(s)] = T.join(s + 2, e)
    return out


def _mem_init_args(T, f):
    """{member: [argument spans]} of a constructor's initializer list."""
    out = {}
    k = f.paren[1] + 1
    if T.t(k) != ":":
        return out
    k += 1
    end = f.body[0] if f.body else T.n
    while k < end:
        if T.k(k) == "id" and T.t(k + 1) in ("(", "{"):
            c = T.close(k + 1)
            out[T.t(k)] = _split_top(T, k + 2, c)
            k = c + 1
            continue
        k += 1
    return out


def _expand_lambda_calls(ev):
    return [("call", lint.EPOCH_FN) if e == ("call", LAMBDA_NAME) else e for e in ev]


def _preflight(T, a, b, outs, declared_only=False):
    """Names among run, ep and @p outs that the body [a, b) declares (any depth) or, unless
    @p declared_only, reads as a free identifier."""
    names = {"run", "ep"} | set(outs)
    found = set()
    for j in ids_in(T, a, b):
        if T.t(j) in names:
            if declared_only:
                p = T.t(j - 1)
                if p in ("&", "*", ">", ">>") or (T.k(j - 1) == "id" and p not in ("return", "case")):
                    found.add(T.t(j))
            else:
                found.add(T.t(j))
    return found


def _code_lines(text):
    out = []
    for k, ln in enumerate(lint.strip_comments(text).split("\n"), 1):
        if ln.strip():
            out.append((k, ln.rstrip()))
    return out


def _solve_declared(a, b, R, replay=True, card=False):
    """The solve's declared body edits (spec 3.4): the replay's two callback lines become
    direct calls (WCF's resolves counter kept), the three epilogue lines write result.handoff."""
    fa = [ln for ln in a]
    out_a, out_b, edited = [], list(b), []
    hits = sum(1 for ln in fa if re.match(r"^result\.(clock_refloat_sats|retro_flag_sats|"
                                           r"n_arc_refloat_epoch)\b", ln[1].strip()))
    if hits != 3:
        R.fail("V1b", f"networkSolveGlue: {hits} frozen epilogue lines of the hand-off fields, 3 expected")
    for k, ln in enumerate(fa):
        t = ln[1].strip()
        if replay and t in ("buildEnsembleGauges();", "buildCarryDdPriors();"):
            builder = "network" + t[0].upper() + t[1:t.index("(")]
            args = "(run, ep)" if "Ensemble" in builder or card else "(run, ep, dd_build, funnel)"
            out_a.append([ln[0], ln[1].replace(t, builder + args + ";")] + ln[2:])
            edited.append(k)
            continue
        m = re.match(r"^result\.(clock_refloat_sats|retro_flag_sats|n_arc_refloat_epoch)\b", t)
        if m:
            out_a.append([ln[0], ln[1].replace("result.", "result.handoff.", 1)] + ln[2:])
            edited.append(k)
            continue
        out_a.append(ln)
    return out_a, out_b, edited


def struct_members(tree, name):
    """{member: {'type', 'ops', 'init', 'line'}} of struct / class @p name defined under
    include/, in declaration order; None when no definition is found."""
    pat = re.compile(r"\b(?:struct|class)\s+" + re.escape(name) + r"\b[^;{]*\{")
    for p in [lint.EPOCH_HPP] + [x for x in tree.files("include") if x.endswith((".hpp", ".h"))]:
        text = tree.read(p)
        if not text or name not in text or not pat.search(text):
            continue
        T = tree.tokens(p)
        for i in range(T.n - 2):
            if T.t(i) in ("struct", "class") and T.t(i + 1) == name:
                k = i + 2
                while k < T.n and T.t(k) not in ("{", ";"):
                    k += 1
                if T.t(k) != "{":
                    continue
                return _members_of(T, k, T.close(k))
    return None


def _members_of(T, a, b):
    out = collections.OrderedDict()
    i = a + 1
    while i < b:
        e = statements(T, i, b)[0][1] if i < b else b
        t = T.t(i)
        if t in ("public", "private", "protected") and T.t(i + 1) == ":":
            i += 2
            continue
        if t in ("using", "static_assert", "friend", "enum", "template", "typedef", "static") or T.k(i) == "pp":
            i = e
            continue
        if t in ("struct", "class") and any(T.t(q) == "{" for q in range(i, e)):
            i = e
            continue
        is_func = any(T.t(q) == "(" and T.k(q - 1) == "id" and T.t(q - 1) not in KEYWORDS
                      and not any(T.t(r) == "=" for r in range(i, q)) for q in range(i, e)) and \
            not any(T.t(q) == "{" and T.k(q - 1) == "id" for q in range(i, e))
        ds = None if is_func else parse_declaration(T, i, e)
        if ds and T.t(e - 1) in (";", "}"):
            for d in ds:
                if d.kind in ("var", "closure"):
                    init = T.norm(*d.init) if d.init else ""
                    if d.init_kind == "{" and not init:
                        init = "{}"
                    out[d.name] = {"type": T.join(*d.type_span), "ops": d.ops, "init": init,
                                   "line": d.line}
        i = e
    return out


def fold_problem(node):
    """None when @p node is in the fold grammar of spec 3.2, else why not."""
    if isinstance(node, Ref):
        for k, seg in enumerate(node.chain):
            if seg.startswith("[") or ("(" in seg and seg not in (".get()", ".x_emit()")):
                return f"`{seg}` in a member chain"
            if seg in (".get()", ".x_emit()") and k != len(node.chain) - 1:
                return f"`{seg}` before the end of a member chain"
        return None
    if isinstance(node, Copy):
        return fold_problem(node.inner)
    if isinstance(node, Lit):
        return None
    if isinstance(node, Cond):
        c = node.c.args[0] if isinstance(node.c, Expr) and node.c.op == "!" else node.c
        for part in (c, node.a, node.b):
            if not isinstance(part, (Ref, Copy, Lit)):
                return "a conditional over something other than member reads and literals"
            p = fold_problem(part)
            if p:
                return p
        return None
    if isinstance(node, CallNode):
        if node.fn != "TimeUtils::continuousGpsSeconds":
            return f"a call of {node.fn}"
        for a in node.args:
            if not isinstance(a, (Ref, Copy)) or fold_problem(a):
                return "a call argument that is not a member read"
        return None
    if isinstance(node, Opaque):
        return f"not resolved ({node.why})"
    return "an operator expression"


def doc_before(T, start):
    """(text, first line) of the comment block directly above token @p start, or None."""
    src = T.source
    pos = T[start].pos
    line_start = src.rfind("\n", 0, pos) + 1
    k = line_start
    lines = []
    first = None
    while k > 0:
        prev_end = k - 1
        prev_start = src.rfind("\n", 0, prev_end) + 1
        ln = src[prev_start:prev_end].strip()
        if ln.startswith(("///", "//", "*", "/**", "/*")) or ln.endswith("*/"):
            lines.insert(0, ln)
            first = src.count("\n", 0, prev_start) + 1
            k = prev_start
            continue
        break
    return ("\n".join(lines), first) if lines else None


def doc_words(text):
    """A comment block's words: the comment markers of each line (/**, */, ///, //, a leading
    *) removed and the whitespace collapsed, so a re-wrapped paragraph compares equal."""
    out = []
    for ln in text.split("\n"):
        ln = ln.strip()
        ln = re.sub(r"^(/\*\*|/\*|///|//|\*/|\*)", "", ln)
        ln = re.sub(r"\*/$", "", ln)
        out.append(ln)
    return " ".join(" ".join(out).split())


def params_doc(text):
    """[(name, description)] of the @param entries of a Doxygen block's text."""
    t = doc_words(text)
    out = []
    for m in re.finditer(r"@param\s+(\w+)\s+(.*?)(?=@param|@return|@brief|$)", t, re.S):
        out.append((m.group(1), m.group(2).strip()))
    return out


def include_graph(tree):
    """{file: [the project headers it includes]} over src/, include/ and tests/."""
    g = {}
    for p in tree.files("src", "include", "tests"):
        if not p.endswith((".cpp", ".hpp", ".h")):
            continue
        text = tree.read(p) or ""
        incs = []
        for m in re.finditer(r'^\s*#\s*include\s+"([^"]+)"', text, re.M):
            inc = m.group(1)
            cand = "include/" + inc
            if tree.read(cand) is not None:
                incs.append(cand)
        g[p] = incs
    return g


def reach(g, start):
    seen, stack = set(), [start]
    while stack:
        x = stack.pop()
        for y in g.get(x, ()):
            if y not in seen:
                seen.add(y)
                stack.append(y)
    return seen


# ---------------------------------------------------------------------------------------------
# Negative controls (spec 5.7)
# ---------------------------------------------------------------------------------------------
class Control:
    def __init__(self, cid, check, what, build):
        self.cid, self.check, self.what, self.build = cid, check, what, build


def _replace(text, a, b, new):
    return text[:a] + new + text[b:]


def ctl_n5(v):
    c, T = v.cand, v.cand.T
    if c.mode == "empty":
        s = c.stmts[0][0]
        line_start = T.source.rfind("\n", 0, T[s].pos) + 1
        ind = T.source[line_start:T[s].pos]
        return {T.path: _replace(T.source, line_start, line_start, ind + "auto stations = setup.stations;\n")}, \
            "a by-value `auto stations = setup.stations;` opens the lambda"
    for s, e in c.preamble:
        if T.join(s, e) == join_text("auto& stations = setup.stations;"):
            return {T.path: _replace(T.source, T[s].pos, T[e - 1].end, "auto stations = run.setup.stations;")}, \
                "the preamble copies stations"
    s = c.preamble[-1][1] if c.preamble else c.open + 1
    return {T.path: _replace(T.source, T[s].pos, T[s].pos, "auto stations = run.setup.stations;\n    ")}, \
        "the preamble copies stations"


def ctl_n6(v):
    c, T = v.cand, v.cand.T

    def holds(name):
        for s, e in c.stmts:
            if any(x.name == name for x in calls_in(T, s, e)) and T.t(s) == name:
                return s, e
        return None
    a, b = holds("networkIonoDiag"), holds("networkDdFrac")
    if not a or not b:
        return None, "the two calls are not statements of the epoch body"
    ta, tb = T.source[T[a[0]].pos:T[a[1] - 1].end], T.source[T[b[0]].pos:T[b[1] - 1].end]
    src = T.source
    src = src[:T[b[0]].pos] + ta + src[T[b[1] - 1].end:]
    src = src[:T[a[0]].pos] + tb + src[T[a[1] - 1].end:]
    return {T.path: src}, "networkIonoDiag and networkDdFrac swapped"


def ctl_n7(v):
    c, T = v.cand, v.cand.T
    if c.mode == "empty":
        k = next((k for k, (s, e) in enumerate(c.stmts) if any(x.name == lint.SOLVE for x in calls_in(T, s, e))), None)
        if k is None:
            return None, "no solve"
        s = c.stmts[k + 1][1]       # after the skip-epoch return
        return {T.path: _replace(T.source, T[s - 1].end, T[s - 1].end, "\n        present.clear();")}, \
            "`present.clear();` after the solve"
    k = next((k for k, (s, e) in enumerate(c.stmts) if T.join(s, min(e, s + 5)).startswith("ep.solve=")), None)
    if k is None:
        return None, "no `ep.solve = ...` statement"
    s = c.stmts[k][1]
    return {T.path: _replace(T.source, T[s - 1].end, T[s - 1].end, "\n    ep.pass_a.present.clear();")}, \
        "`ep.pass_a.present.clear();` after the solve"


def ctl_n8(v):
    c, T = v.cand, v.cand.T
    for s, e in c.stmts:
        txt = T.join(s, e)
        if "est_index_of" in txt and "-1" in txt and ("std::vector<int>" in txt):
            for j in range(s, e):
                if T.t(j) == "-" and T.t(j + 1) == "1":
                    return {T.path: _replace(T.source, T[j].pos, T[j + 1].end, "0")}, \
                        "est_index_of sized with 0 instead of -1"
    return None, "the est_index_of sizing statement is not in the epoch body"


def ctl_n10(v):
    c, T = v.cand, v.cand.T
    for j in range(c.open, c.close):
        if T.t(j) == "dt" and T.t(j - 1) == "double" and T.t(j - 2) == "const" and T.t(j + 1) == "=" \
                and "isfinite" in T.source[T[j].pos:T[j].pos + 200].split(";")[0]:
            e = j + 2
            while T.t(e) != ";":
                e += 1
            return {T.path: _replace(T.source, T[j + 2].pos, T[e - 1].end, "ep.clock.dt")}, \
                "the DD-carry predict's shadowing dt reads ep.clock.dt"
    return None, "the shadowing dt is not in the epoch body"


def ctl_n11(v):
    c = v.cand
    rT = c.rn.T
    warn = None
    for s, e in c.rn.statements():
        if rT.t(s) == "if" and "debug_r_scale" in rT.join(s, e) and any(x.name == "log->warn" for x in calls_in(rT, s, e)):
            warn = (s, e)
    if warn is None:
        return None, "the debug_r_scale warning is not in runNetwork"
    wtext = rT.source[rT[warn[0]].pos:rT[warn[1] - 1].end]
    if c.mode == "empty":
        last = None
        for s, e in c.rn.statements():
            if e <= warn[0] and rT.t(s) == "mw_wl_cfg" and rT.t(s + 1) == ".":
                last = (s, e)
        if last is None:
            return None, "no mw_wl_cfg cut statement before the warning"
        src = rT.source
        src = src[:rT[warn[0]].pos] + src[rT[warn[1] - 1].end:]
        src = src[:rT[last[0]].pos] + wtext + "\n    " + src[rT[last[0]].pos:]
        return {DRIVER_PATH: src}, "the debug_r_scale warning moved into the mw_wl_cfg cut range"
    idx = v.tree.index(RUN_CPP)
    f = next((g for g in (idx.funcs if idx else []) if g.name == "networkEngineSpec" and g.is_def), None)
    if f is None:
        return None, "networkEngineSpec does not exist yet"
    ST = v.tree.tokens(RUN_CPP)
    ret = next((s for s, e in statements(ST, f.body[0] + 1, f.body[1]) if ST.t(s) == "return"), f.body[1])
    src = rT.source[:rT[warn[0]].pos] + rT.source[rT[warn[1] - 1].end:]
    return {DRIVER_PATH: src, RUN_CPP: _replace(ST.source, ST[ret].pos, ST[ret].pos, wtext + "\n    ")}, \
        "the debug_r_scale warning cut into networkEngineSpec"


def ctl_n12(v):
    r = v.by_name.get("networkFixedCycles")
    if r is None:
        return None, "no networkFixedCycles row"
    T = v.tree.tokens(r.cpp)
    idx = v.tree.index(r.cpp)
    if "networkFixedCycles" in v.converted:
        f = next((g for g in idx.funcs if g.name == "networkFixedCycles" and g.is_def and lint.is_epoch_form(g)), None)
        if f is None:
            return None, "the converted networkFixedCycles definition is missing"
        p = f.params[1]
        return {r.cpp: _replace(T.source, T[p.name_tok].pos, T[p.name_tok].end, "ep")}, \
            "networkFixedCycles defined with `ep` as its epoch parameter's name"
    f = next((g for g in idx.funcs if g.name == "networkFixedCycles" and g.is_def), None)
    p = next((q for q in f.params if q.name == "se"), None) if f else None
    if p is None:
        return None, "networkFixedCycles' definition has no `se` parameter"
    return {r.cpp: _replace(T.source, T[p.name_tok].pos, T[p.name_tok].end, "ep")}, \
        "networkFixedCycles defined with a parameter named `ep` (empty-conversion form)"


def ctl_preamble_edit(seam, pname, frm, to, what):
    """A control that rewrites one binding line of a converted seam."""
    def f(v):
        if seam not in v.converted:
            return None, f"{seam} is not converted in this tree"
        for d in v.new_defs(seam):
            DT = v.tree.tokens(d.path)
            sp = lint.preamble_span(DT, d)
            span = sp if sp else (d.body[0] + 1, d.body[1])
            for s, e in statements(DT, *span):
                txt = DT.source[DT[s].pos:DT[e - 1].end]
                if pname in txt and frm in txt:
                    return {d.path: _replace(DT.source, DT[s].pos, DT[e - 1].end, txt.replace(frm, to, 1))}, what
        return None, f"no binding of {pname} found in {seam}"
    return f


def ctl_n9(v):
    for name in sorted(v.converted):
        if v.by_name[name].mech == ADAPTER:
            continue
        for d in v.new_defs(name):
            DT = v.tree.tokens(d.path)
            sp = lint.preamble_span(DT, d)
            if not sp:
                continue
            for j in range(sp[1], d.body[1]):
                if DT.t(j) == "+" and DT.k(j) == "op":
                    return {d.path: _replace(DT.source, DT[j].pos, DT[j].end, "-")}, f"one `+` in {name}'s body made `-`"
    return None, "no converted seam body"


def ctl_n13(v):
    if "networkClockQc" not in v.converted:
        return None, "networkClockQc is not converted"
    r = v.by_name["networkClockQc"]
    out = {}
    for path in (r.hpp, r.cpp):
        T = v.tree.tokens(path)
        f = next((g for g in v.tree.index(path).funcs if g.name == "networkClockQc" and lint.is_epoch_form(g)), None)
        if f is None:
            return None, "networkClockQc's epoch form is missing"
        out[path] = _replace(T.source, T[f.paren[1]].pos, T[f.paren[1]].pos, ", EpochPassA& pass_a")
    return out, "networkClockQc given EpochPassA& as a second output"


def ctl_n14(v):
    if lint.SOLVE not in v.converted:
        return None, "networkSolveGlue is not converted"
    T = v.tree.tokens(SOLVE_CPP)
    k = T.source.find("++ctx.counters.we_culprit_withhold.resolves;")
    if k < 0:
        return None, "the WCF resolves counter line is not in the solve"
    return {SOLVE_CPP: _replace(T.source, k, k + len("++ctx.counters.we_culprit_withhold.resolves;"), ";")}, \
        "the replay's WCF resolves counter dropped"


def ctl_n15(v):
    seam = "networkHandoverStransformBeforeRows"
    if seam not in v.converted:
        return None, f"{seam} is not converted"
    for d in v.new_defs(seam):
        DT = v.tree.tokens(d.path)
        k = DT.source.find("ep.se.time", DT[d.body[0]].pos, DT[d.body[1]].pos)
        if k >= 0:
            return {d.path: _replace(DT.source, k, k + len("ep.se.time"), "ep.solve.plan")}, \
                f"{seam} binds ep.solve.plan"
    return None, f"{seam}'s epoch form does not read ep.se.time"


def ctl_n16(v):
    c, T = v.cand, v.cand.T
    if c.mode != "epoch":
        return None, "no epoch function (ep.emitted does not exist)"
    st = next(((s, e) for s, e in c.stmts if T.join(s, min(e, s + 4)).startswith("ep.emitted=")), None)
    before = next(((s, e) for s, e in c.stmts if "fc_n_emitted_before" in T.join(s, e)
                   and T.t(s) == "const"), None)
    if st is None or before is None:
        return None, "the ep.emitted store or fc_n_emitted_before is missing"
    text = T.source[T[st[0]].pos:T[st[1] - 1].end]
    src = T.source[:T[st[0]].pos] + T.source[T[st[1] - 1].end:]
    pos = T[before[1] - 1].end
    return {T.path: src[:pos] + "\n    " + text + src[pos:]}, "ep.emitted stored before networkEmitStore"


def ctl_n4(v):
    if "networkRhsAttribTrip" not in v.converted or lint.SOLVE not in v.converted:
        return None, "networkRhsAttribTrip and the solve are not both converted"
    T = v.tree.tokens(SOLVE_CPP)
    for c in calls_in(T, 0, T.n):
        if c.name == "networkRhsAttribTrip" and c.nargs > 2:
            return {SOLVE_CPP: _replace(T.source, T[c.open + 1].pos, T[c.close].pos, "run, ep")}, \
                "the solve's internal networkRhsAttribTrip call takes the epoch form"
    return None, "the solve's internal networkRhsAttribTrip call is not found"


CONTROLS = (
    Control("N1", "V3a", "WCF shape: a by-value copy of we_culprits in the DD build's bindings",
            ctl_preamble_edit("networkBuildCarryDdPriors", "we_culprits", "const WithinEpochCulprits& we_culprits",
                              "WithinEpochCulprits we_culprits", "we_culprits copied")),
    Control("N2", "V3a", "networkPassASlipScan_ binds funnel.carry_slipped_ambs",
            ctl_preamble_edit("networkPassASlipScan_", "carry_slipped_ambs", "funnel.carry_slipped_raw",
                              "funnel.carry_slipped_ambs", "the raw subset bound to the funnel")),
    Control("N3", "V3a", "networkSolPrep's adapter forwards ep.solve.x_s",
            ctl_preamble_edit("networkSolPrep", "x_emit", "ep.x_emit()", "ep.solve.x_s", "x_s instead of x_emit")),
    Control("N4", "V3a", "the solve's internal networkRhsAttribTrip call bound to the epoch", ctl_n4),
    Control("N5", "V3b", "a copy of a captured reference", ctl_n5),
    Control("N6", "V6", "networkIonoDiag and networkDdFrac swapped", ctl_n6),
    Control("N7", "V4", "a driver write into the epoch after the solve", ctl_n7),
    Control("N8", "V1a", "est_index_of sized with 0 instead of -1", ctl_n8),
    Control("N9", "V1b", "a one-token edit inside a converted body", ctl_n9),
    Control("N10", "V1a", "the shadowing dt rewritten to ep.clock.dt", ctl_n10),
    Control("N11", "V12", "the debug_r_scale warning cut into the engine spec", ctl_n11),
    Control("N12", "V13", "networkFixedCycles defined with `ep` as a parameter name", ctl_n12),
    Control("N13", "V2", "networkClockQc given EpochPassA& as a second output", ctl_n13),
    Control("N14", "V1b", "the replay's WCF resolves counter dropped", ctl_n14),
    Control("N15", "V10", "networkHandoverStransformBeforeRows binds ep.solve.plan", ctl_n15),
    Control("N16", "V4", "ep.emitted stored before networkEmitStore", ctl_n16),
)
ALSO = {"N8": ("V3c",), "N10": ("V3e",), "N16": ("V6",), "N4": ("V10",), "N13": ("V8",)}


def run_controls(fz, tree, rows, converted, args, out):
    base = Verifier(fz, tree, rows, converted, args.presolve_writer)
    base.frozen_rows()
    results = []
    for ctl in CONTROLS:
        try:
            ov, what = ctl.build(base)
        except Exception as ex:  # noqa: BLE001
            ov, what = None, f"the mutation could not be built ({type(ex).__name__}: {ex})"
        if ov is None:
            results.append((ctl, "pending", what, []))
            out(f"  {ctl.cid:4s} {ctl.check:4s} pending   {ctl.what}: {what}")
            continue
        mtree = tree.overlay(ov)
        t0 = time.time()
        try:
            v = Verifier(fz, mtree, rows, converted, args.presolve_writer)
            res = v.run()
            failed = res.failed()
        except SystemExit as ex:
            failed, res = ["(the mutated tree could not be read: %s)" % ex], None
        names = (ctl.check,) + ALSO.get(ctl.cid, ())
        fired = any(n in failed for n in names)
        results.append((ctl, "fires" if fired else "DOES NOT FIRE", what, failed))
        first = ""
        if res is not None:
            for n in names:
                if res.problems.get(n):
                    first = res.problems[n][0]
                    break
        out(f"  {ctl.cid:4s} {ctl.check:4s} {'fires    ' if fired else 'NOT FIRED'} {what}; failing: "
            f"{', '.join(failed) or 'none'} ({time.time() - t0:.0f} s)")
        if first:
            out(f"       {first[:160]}")
    return results


# ---------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------
def pick_rows(names):
    if not names:
        return []
    if names == ["all"]:
        return list(ROW_MODULES)
    out = []
    for n in names:
        stem = pathlib.Path(n).stem
        stem = stem if stem.startswith("rows_") else "rows_" + stem
        if stem not in ROW_MODULES:
            raise SystemExit(f"nom_ep1_verify: unknown row file {n!r} (one of {', '.join(ROW_MODULES)})")
        out.append(stem)
    return out


def check_manifest(v, path):
    """Pairing cross-check against a manifest scripts/nom_ep1_cut.py wrote (V8): its base, its
    converted seams, the frozen lambda's and the epoch function's line ranges, and each seam
    entry's frozen and new declaration / definition lines must be the ones the verifier found
    by itself.  No binding is read from it."""
    R = v.R
    name_ = pathlib.Path(path).name
    try:
        m = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as ex:
        R.fail("V8", f"manifest {name_}: unreadable ({ex})")
        return
    if not isinstance(m, dict):
        R.fail("V8", f"manifest {name_}: not a JSON object")
        return
    sha = v.fz.tree.sha
    if m.get("base_sha") and not sha.startswith(str(m["base_sha"])) and not str(m["base_sha"]).startswith(sha):
        R.fail("V8", f"manifest {name_}: written at base {m.get('base_sha')}, verified against {sha}")
    conv = set(m.get("converted") or [])
    if conv != v.converted:
        R.fail("V8", f"manifest {name_}: converts {sorted(conv - v.converted)} beyond --rows/--seams and "
                     f"not {sorted(v.converted - conv)}")
    ef = m.get("epoch_function") or {}
    fz = v.fz
    lam = [fz.drv.T.line(fz.rn.lam_open), fz.drv.T.line(fz.rn.lam_close)]
    if ef.get("frozen_lambda") and list(ef["frozen_lambda"]) != lam:
        R.fail("V8", f"manifest {name_}: frozen lambda {ef['frozen_lambda']}, the verifier found {lam}")
    if v.epoch and ef.get("function_lines"):
        T = v.cand.T
        got = [T.line(decl_start(T, v.cand.fn.func)), T.line(v.cand.close)]
        fl = list(ef["function_lines"])
        others = [f.line for f in v.tree.index(v.cand.fn.path).funcs
                  if f is not v.cand.fn.func and got[1] < f.line <= fl[1]]
        if not (got[0] <= fl[0] <= T.line(v.cand.open)) or fl[1] < got[1] or others:
            R.fail("V8", f"manifest {name_}: epoch function lines {fl}, the verifier found {got}")
    n = 0
    for e in m.get("seams", []):
        name = e.get("seam") or e.get("name")
        if not name or name not in v.converted or name not in v.by_name:
            continue
        r = v.by_name[name]
        fdecl, fdef = v.fdecls.get(name, (None, None))
        kind, path_, nl, fl = e.get("kind"), e.get("file"), e.get("new_line"), e.get("frozen_line")
        if kind == "declaration":
            idx = v.tree.index(path_)
            found = [f.line for f in (idx.funcs if idx else []) if f.name == name and not f.is_def
                     and f.params and lint.is_epoch_form(f)]
            frozen = fdecl.line if fdecl else None
        elif kind in ("definition", "adapter definition"):
            found = [f.line for f in v.new_defs(name) if f.path == path_]
            frozen = fdef.line if fdef else None
            if kind == "adapter definition":
                fl = frozen
        else:
            continue
        n += 1
        if nl is not None and nl not in found:
            R.fail("V8", f"manifest {name_}: {name} {kind} at {path_}:{nl}, the verifier found "
                         f"{', '.join(map(str, found)) or 'none'}")
        if fl is not None and frozen is not None and fl != frozen and r.mech is not None:
            R.fail("V8", f"manifest {name_}: {name} frozen {kind} line {fl}, the verifier found {frozen}")
    R.note("V8", f"manifest {name_}: {n} seam entries paired")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tree", required=True, help="the candidate source tree (a directory)")
    ap.add_argument("--base", default=DEFAULT_BASE, help=f"frozen revision <B> (default {DEFAULT_BASE})")
    ap.add_argument("--rows", nargs="*", default=[], help="row files the tree has converted (app solve emit "
                    "diag, or all); default none")
    ap.add_argument("--seams", default="", help="further seams the tree has converted, comma-separated "
                    "(a worker part way through its rows)")
    ap.add_argument("--presolve-writer", choices=("outputs", "epoch"), default="outputs")
    ap.add_argument("--manifest", help="the generator's manifest (pairing cross-check only)")
    ap.add_argument("--controls", action="store_true", help="also run the negative controls of spec 5.7")
    ap.add_argument("--only", help="comma-separated checks to run (default all)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)
    t0 = time.time()
    rows = load_rows(ROW_MODULES)
    converted = [r.name for r in load_rows(pick_rows(args.rows))]
    names = {r.name for r in rows}
    for x in filter(None, args.seams.split(",")):
        if x not in names:
            ap.error(f"--seams: {x} is not a row of the canonical table")
        if x not in converted:
            converted.append(x)
    fz = Frozen(args.base)
    tree = DiskTree(args.tree)
    only = set(args.only.split(",")) if args.only else None
    v = Verifier(fz, tree, rows, converted, args.presolve_writer, args.manifest, args.verbose)
    print(f"NOM3 step EP1 relocation verifier: base {args.base} ({fz.tree.sha[:10]}), tree {args.tree}")
    print(f"  candidate shape: {v.cand.mode}"
          + (f" ({v.cand.fn.path})" if v.epoch else " (the lambda still in runNetwork: an empty conversion)")
          + f"; converted rows: {', '.join(pick_rows(args.rows)) or 'none'}"
          + (f" + seams {args.seams}" if args.seams else "") + f" ({len(converted)} seams)")
    res = v.run(only)
    mpaths = [args.manifest] if args.manifest else sorted(
        str(p) for p in (pathlib.Path(args.tree) / "scripts" / "nom_ep1").glob("manifest_*.json"))
    for mp in mpaths:
        check_manifest(v, mp)
        res.done("V8")
    for c in CHECKS:
        if only and c not in only:
            continue
        st = res.status.get(c, "n/a")
        print(f"  [{st if st != 'pass' else 'pass'}] {c}: " + "; ".join(res.info[c]))
        for p in res.problems[c][:40]:
            print("      " + p)
        if len(res.problems[c]) > 40:
            print(f"      ... {len(res.problems[c]) - 40} more")
    failed = res.failed()
    print(f"verification: {'PASS' if not failed else 'FAIL (' + ', '.join(failed) + ')'} "
          f"({time.time() - t0:.0f} s)")
    rc = 1 if failed else 0
    if args.controls:
        print("\nNegative controls (spec 5.7): each must fail its named check")
        results = run_controls(fz, tree, rows, converted, args, print)
        ran = [x for x in results if x[1] != "pending"]
        nf = [x for x in ran if x[1] != "fires"]
        print(f"controls: {len(ran) - len(nf)} of {len(ran)} fire, {len(results) - len(ran)} pending"
              + (f"; NOT FIRED: {', '.join(x[0].cid for x in nf)}" if nf else ""))
        if nf:
            rc = 1
    fz.tree.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())

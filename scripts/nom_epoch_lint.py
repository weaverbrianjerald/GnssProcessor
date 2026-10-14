#!/usr/bin/env python3
"""Permanent lint of the network producer's epoch function (card NOM3 step EP1, checks V4, V10, V11).

    python scripts/nom_epoch_lint.py [--tree DIR] [--partial] [--presolve-writer {outputs,epoch}]

Run it with PYTHONIOENCODING=utf-8 on a cp1252 console, before committing a change to a per-epoch seam signature or to
src/core/app/ApplicationNetworkEpoch.cpp (the seam contract in CLAUDE.md).  It reads the
working files of the tree (default: the repository this script sits in), needs no git
revision and no build, and uses only the Python 3 standard library and scripts/nom_cpp.py.
Exit 0 when every check passes, 1 when one fails, 2 when the epoch function or the epoch
type cannot be found.

What it proves (spec sections 2.3, 3.1 and 5.5 of the EP1 specification):

  V4  Driver writes after the solve.  In Application::networkProcessEpoch_, after the
      statement `ep.solve = networkSolveGlue(...)`, the only writes into `ep` are the four
      driver stores `ep.ar_fixes =`, `ep.fixed_cycles =`, `ep.prep =`, `ep.emitted =` (each
      once; `ep.emitted` after the networkEmitStore call) and the three output arguments
      (`ep.solve.handoff` to networkPostFitRaim and networkClockQc, `ep.prep.sol` to
      networkEmitStore).  A write is an assignment, an increment, a member call outside the
      read-only method list, or an argument passed to a non-const reference or pointer
      parameter; a name bound by reference to an epoch member is followed to the member, so a
      write through an alias counts.  `ep.clock` is assigned exactly once, before any read of
      it in the function and before the first seam whose bindings read it.  An argument of
      a call whose declaration cannot be found is reported as unclassified (a failure),
      except for the logger and the formatting calls.
  V10 Stage order.  Every `ep.<group>` a converted seam's bindings read (its preamble, or an
      adapter's forwarding call) names a group already available at the seam's spine line.
      A group is available from its first write in the epoch function: the epoch's
      construction for `se` and `ws`, else the first statement that assigns into it, calls a
      member function that writes it, or passes it to an output parameter (a reference
      binding alone is not a write).  A converted seam that a spine seam's body calls with
      the caller's own run and epoch (the solve's replay builders, at any depth) reads only
      groups written strictly before the caller's spine line: the caller's own writes are
      still under way while it runs, so a builder inside the solve cannot read ep.solve.
      The banner's prose "available from" column is not machine-read; the first write is
      its executable form.
  V11 Writer banners.  For every group, the set of seams whose spine call passes `ep.<group>`
      (or a member of it) to an output parameter equals the seams named in the group's
      "written by" cell of the banner table in include/app/NetworkEpoch.hpp, and the seams
      whose return the driver stores into the group (`ep.<group> = networkX(...)`) equal the
      cell's "stores networkX's return" phrases ("the solve's return" names networkSolveGlue,
      "that seam" the seam of the "available from" cell).  A banner writer with no epoch
      form fails here; with --partial (a tree part way through the conversion: step
      EP1a-1, a worker's branch of step EP1b, the merges before the last), and in the
      relocation verifier's runs on such a tree, such writers are skipped instead.  Once
      the last worker's rows are merged the lint runs without --partial.  On the card's
      pre-solve form (--presolve-writer epoch, as in the generator and the verifier: the
      writers before the solve take `NetworkEpoch& ep`) a spine line names no group, so V11
      is reported n/a; in the default form a seam that takes `NetworkEpoch&` fails V11.

A converted seam is recognized by its definition: its first parameter is `NetworkRun&` and
its second `const NetworkEpoch&` (the parameter names may be the fallback names nrun / nep).
Its bindings are the lines between the generated markers `// ---- From the run` (and the
epoch) and `// ---- End of the bindings ----`, which open the body (preamble_leads; the
relocation verifier's V1b fails code before them); an adapter's epoch form is one forwarding
call.

The module also holds the tree reader and the comment-free statement rendering that
scripts/nom_ep1_verify.py uses (DiskTree, render(), epoch_function()), so the relocation
verifier and this lint read a tree the same way.  The verifier runs V4 on an empty
conversion too (the frozen lambda, its epoch locals named by scripts/nom_ep1/declared.py),
through the same write scan with another path mapping.
"""
import argparse
import collections
import os
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from nom_cpp import (FileIndex, Ref, Resolver, Scopes, Tokens, calls_in, statements,  # noqa: E402
                     strip_parens)
from rt_seam_verify import strip_comments  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
EPOCH_CPP = "src/core/app/ApplicationNetworkEpoch.cpp"
EPOCH_HPP = "include/app/NetworkEpoch.hpp"
EPOCH_FN = "networkProcessEpoch_"
SOLVE = "networkSolveGlue"
EMIT = "networkEmitStore"
PREAMBLE_OPEN = re.compile(r"//\s*-{2,}\s*From the run\b")
PREAMBLE_CLOSE = re.compile(r"//\s*-{2,}\s*End of the bindings")

# The driver's stores after the solve, and the output arguments a post-solve seam may take.
DRIVER_STORES = ("ep.ar_fixes", "ep.fixed_cycles", "ep.prep", "ep.emitted")
POST_SOLVE_OUTPUTS = {"ep.solve.handoff": ("networkPostFitRaim", "networkClockQc"),
                      "ep.prep.sol": ("networkEmitStore",)}
# Member functions that read their object (a call of any other member is a write).
READ_METHODS = frozenset("""
size empty count find contains at begin end cbegin cend rbegin rend front back data get value
has_value is_open rows cols norm squaredNorm sum dot transpose segment head tail maxCoeff
minCoeff cwiseAbs lower_bound upper_bound equal_range c_str length str substr compare test
any none all namesAmbiguities x_emit
""".split())
# Calls whose arguments are never written (formatting and logging take their arguments by
# forwarding reference only to format them).
READ_CALLEES = re.compile(r"^(log->\w+|fmt::\w+|std::\w+(::\w+)*|spdlog::\w+|TimeUtils::\w+)$")
_ASSIGN = frozenset(("=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="))


# ---------------------------------------------------------------------------------------------
# A tree on disk (or an overlay of one)
# ---------------------------------------------------------------------------------------------
class DiskTree:
    """Read-only access to a source tree on disk, with the interface of nom_cpp.GitTree
    (read, tokens, index, files).  Text is read as UTF-8 with CRLF folded to LF.  overlay()
    returns a tree whose named files have other text and which shares every other file's
    lexed tokens and index with this one, so a mutated copy costs only its changed files."""

    SCAN = ("src", "include", "tests")

    def __init__(self, root, overrides=None, parent=None):
        self.root = pathlib.Path(root)
        self.rev = str(self.root)
        self.sha = "(working files)"
        self.overrides = dict(overrides or {})
        self.parent = parent
        self._text, self._tokens, self._index, self._files = {}, {}, {}, {}

    def overlay(self, overrides):
        base = self.parent or self
        merged = dict(self.overrides)
        merged.update(overrides)
        return DiskTree(self.root, merged, base)

    def _shared(self, path):
        return self.parent is not None and path not in self.overrides

    def read(self, path):
        if path in self.overrides:
            return self.overrides[path]
        if self._shared(path):
            return self.parent.read(path)
        if path not in self._text:
            p = self.root / path
            try:
                self._text[path] = p.read_bytes().decode("utf-8", errors="replace").replace("\r\n", "\n")
            except OSError:
                self._text[path] = None
        return self._text[path]

    def tokens(self, path):
        if self._shared(path):
            return self.parent.tokens(path)
        if path not in self._tokens:
            text = self.read(path)
            self._tokens[path] = None if text is None else Tokens(text, path)
        return self._tokens[path]

    def index(self, path):
        if self._shared(path):
            return self.parent.index(path)
        if path not in self._index:
            T = self.tokens(path)
            self._index[path] = None if T is None else FileIndex(T)
        return self._index[path]

    def files(self, *prefixes):
        if self.parent is not None:
            base = self.parent.files(*prefixes)
            extra = [p for p in self.overrides if p not in base and p.startswith(prefixes)
                     and self.overrides[p] is not None]
            return [p for p in base if self.overrides.get(p, "") is not None] + sorted(extra)
        if prefixes not in self._files:
            out = []
            for pre in prefixes:
                top = self.root / pre
                if top.is_file():
                    out.append(pre)
                    continue
                for dp, dn, fn in os.walk(top):
                    dn[:] = [d for d in dn if not d.startswith((".", "build"))]
                    for f in fn:
                        out.append((pathlib.Path(dp) / f).relative_to(self.root).as_posix())
            self._files[prefixes] = sorted(out)
        return self._files[prefixes]

    def close(self):
        pass


# ---------------------------------------------------------------------------------------------
# Comment-free rendering of a token range
# ---------------------------------------------------------------------------------------------
class Sub:
    """A rendering substitution: tokens [a, b) are replaced by @p text; eat_before / eat_after
    drop the source gap before token a / after token b - 1."""
    __slots__ = ("a", "b", "text", "eat_before", "eat_after")

    def __init__(self, a, b, text, eat_before=False, eat_after=False):
        self.a, self.b, self.text = a, b, text
        self.eat_before, self.eat_after = eat_before, eat_after


def _column(T, i):
    pos = T[i].pos
    return pos - (T.source.rfind("\n", 0, pos) + 1)


def render(T, s, e, subs=()):
    """The code lines of tokens [s, e): a list of [indent column, text, source line].  Comments
    and blank lines are dropped (as in scripts/rt_seam_verify.py); the spacing between two
    tokens on one line is kept verbatim; each line's indentation is its first token's column.
    @p subs replace token ranges by text (see Sub)."""
    by_start = {x.a: x for x in subs}
    lines = [[_column(T, s), "", T.line(s)]] if s < e else []
    i, prev, eat = s, None, False
    while i < e:
        sub = by_start.get(i)
        if prev is not None and not eat and not (sub and sub.eat_before):
            gap = strip_comments(T.source[T[prev].end:T[i].pos])
            if "\n" in gap:
                lines[-1][1] = lines[-1][1].rstrip()
                lines.append([len(gap.rsplit("\n", 1)[1]), "", T.line(i)])
            else:
                lines[-1][1] += gap
        eat = False
        if sub:
            lines[-1][1] += sub.text
            prev, i, eat = sub.b - 1, sub.b, sub.eat_after
            continue
        lines[-1][1] += T[i].text
        prev, i = i, i + 1
    for ln in lines:
        ln[1] = ln[1].rstrip()
    return [ln for ln in lines if ln[1]]


def same_lines(a, b, shift=0):
    """None when the code lines @p a and @p b are equal, the indentation of @p b being that of
    @p a minus @p shift; else (index, line of a, line of b) of the first difference."""
    for k in range(max(len(a), len(b))):
        x = a[k] if k < len(a) else None
        y = b[k] if k < len(b) else None
        if x is None or y is None or x[1] != y[1] or y[0] != x[0] - shift:
            return k, x, y
    return None


def raw_pos(T, pattern, a, b):
    """Character position of the first match of @p pattern in the raw source between the
    characters of tokens a and b (comments included), or None."""
    lo = T[a].pos if a < T.n else len(T.source)
    hi = T[b].pos if b < T.n else len(T.source)
    m = pattern.search(T.source, lo, hi)
    return m.start() if m else None


# ---------------------------------------------------------------------------------------------
# The epoch function and converted seams
# ---------------------------------------------------------------------------------------------
def ptype(p):
    """A parameter's type text (minimal spacing), its name removed."""
    if p.name and p.code.endswith(p.name):
        return p.code[: len(p.code) - len(p.name)]
    return p.code


def is_epoch_form(f):
    """True when @p f (a nom_cpp Func) takes (NetworkRun&, const NetworkEpoch&, ...) or is the
    run-only form (NetworkRun&)."""
    if not f.params:
        return False
    if ptype(f.params[0]) != "NetworkRun&":
        return False
    return len(f.params) == 1 or ptype(f.params[1]) in ("const NetworkEpoch&", "NetworkEpoch&")


class EpochFunction:
    """Application::networkProcessEpoch_ in a tree: its file, tokens, body braces, scope tree
    and a resolver whose roots are E:<name> (the function's top level, its parameters
    included: `run` is E:run, the epoch E:ep)."""

    def __init__(self, tree, path, func):
        self.tree, self.path, self.func = tree, path, func
        self.T = tree.tokens(path)
        self.open, self.close = func.body
        self.scopes = Scopes(self.T, self.open, self.close, func.params)
        self.frames = {id(self.scopes.root): "E"}
        self.run_name = func.params[0].name if func.params else "run"

    def resolver(self, stop=()):
        return Resolver(self.scopes, self.frames, stop)

    def statements(self):
        return statements(self.T, self.open + 1, self.close)

    def ep_decl(self):
        """The `NetworkEpoch ep(se);` declaration, or None."""
        for d in self.scopes.root.decls:
            if d.name == "ep" and d.type_span and self.T.join(*d.type_span) == "NetworkEpoch":
                return d
        return None


def epoch_function(tree):
    """The EpochFunction of @p tree, or None when Application::networkProcessEpoch_ is not
    defined (the frozen tree, before step EP1a-1)."""
    paths = [EPOCH_CPP] + [p for p in tree.files("src") if p.endswith(".cpp") and p != EPOCH_CPP]
    for p in paths:
        text = tree.read(p)
        if not text or EPOCH_FN not in text:
            continue
        idx = tree.index(p)
        for f in idx.funcs:
            if f.name == EPOCH_FN and f.is_def and f.cls == "Application":
                return EpochFunction(tree, p, f)
    return None


class SeamDefs:
    """Definitions of converted seams in a tree, found by name (src/ files that mention it)."""

    def __init__(self, tree):
        self.tree = tree
        self._cache = {}

    def find(self, name, epoch_form=True):
        key = (name, epoch_form)
        if key in self._cache:
            return self._cache[key]
        out = []
        for p in self.tree.files("src"):
            if not p.endswith(".cpp"):
                continue
            text = self.tree.read(p)
            if not text or name not in text:
                continue
            for f in self.tree.index(p).funcs:
                if f.name == name and f.is_def and f.params is not None \
                        and is_epoch_form(f) == epoch_form:
                    out.append(f)
        self._cache[key] = out
        return out


def is_card_form(f):
    """True for the card's form of a writer before the solve (--presolve-writer epoch): the
    epoch arrives as a non-const NetworkEpoch& and the writes bind through it."""
    return bool(f.params) and len(f.params) >= 2 and ptype(f.params[1]) == "NetworkEpoch&"


def written_bindings(T, f):
    """The epoch paths a definition's bindings mark `// written` (through its second
    parameter): the groups a card-form writer writes."""
    span = preamble_span(T, f)
    if span is None or len(f.params) < 2:
        return []
    epn = f.params[1].name
    out = []
    for s, e in statements(T, *span):
        line_end = T.source.find("\n", T[e - 1].end)
        if "// written" not in T.source[T[e - 1].end:line_end if line_end >= 0 else None]:
            continue
        for j in range(s, e):
            if T.t(j) == epn and T.t(j + 1) == "." and T.k(j + 2) == "id" and T.t(j - 1) == "=":
                path, k = "ep." + T.t(j + 2), j + 3
                while T.t(k) == "." and T.k(k + 1) == "id":
                    path += "." + T.t(k + 1)
                    k += 2
                out.append(path)
                break
    return out


def preamble_span(T, f):
    """(first token, end token) of the generated bindings of definition @p f: the statements
    between the two markers, or None when the markers are absent."""
    a, b = f.body
    lo = raw_pos(T, PREAMBLE_OPEN, a, b)
    hi = raw_pos(T, PREAMBLE_CLOSE, a, b)
    if lo is None or hi is None or hi < lo:
        return None
    first = next((i for i in range(a + 1, b) if T[i].pos > lo), b)
    end = next((i for i in range(first, b) if T[i].pos > hi), b)
    return first, end


def preamble_leads(T, f):
    """True when the generated bindings of definition @p f open its body: no code token lies
    between the body's `{` and the opening marker (code there would run before the bindings
    and outside every check that reads the bindings or the body below them)."""
    span = preamble_span(T, f)
    return span is not None and span[0] == f.body[0] + 1


def binding_reads(T, f):
    """The epoch paths ('ep.pass_a.present', ...) a converted definition's bindings read, with
    the token of each.  Reads go through the second parameter (ep or nep); an adapter's epoch
    form (no markers) reads through its forwarding call."""
    if len(f.params) < 2:
        return []
    epn = f.params[1].name
    span = preamble_span(T, f)
    a, b = span if span else (f.body[0] + 1, f.body[1])
    out = []
    for j in range(a, b):
        if T.t(j) == epn and T.k(j) == "id" and T.t(j - 1) not in (".", "->", "::") \
                and T.t(j + 1) == "." and T.k(j + 2) == "id":
            if T.t(j + 2) == "x_emit" and T.t(j + 3) == "(":
                out.append(("ep.x_emit()", j))
                continue
            path, k = "ep." + T.t(j + 2), j + 3
            while T.t(k) == "." and T.k(k + 1) == "id" and T.t(k + 2) != "(":
                path += "." + T.t(k + 1)
                k += 2
            if T.t(k) == "." and T.t(k + 1) == "x_emit":
                path += ".x_emit()"
            out.append((path, j))
    return out


def group_of(path):
    """'ep.solve.handoff.clock_refloat_sats' -> 'solve'; 'ep.x_emit()' -> 'fixed_cycles' (the
    accessor reads fixed_cycles and solve, and fixed_cycles is written later)."""
    parts = path.split(".")
    if len(parts) < 2:
        return None
    if parts[1] == "x_emit()":
        return "fixed_cycles"
    return parts[1]


# ---------------------------------------------------------------------------------------------
# Parameter kinds of a callee (declaration lookup)
# ---------------------------------------------------------------------------------------------
class DeclIndex:
    """Parameter lists of named functions declared under include/ (and Application members),
    looked up by the callee's last name component."""

    def __init__(self, tree):
        self.tree = tree
        self._cache = {}

    def lookup(self, leaf):
        if leaf in self._cache:
            return self._cache[leaf]
        out = []
        for p in self.tree.files("include"):
            if not p.endswith((".hpp", ".h")):
                continue
            text = self.tree.read(p)
            if not text or leaf not in text:
                continue
            for f in self.tree.index(p).funcs:
                if f.name == leaf and f.params is not None:
                    out.append(f)
        self._cache[leaf] = out
        return out

    def arg_kind(self, callee, nargs, k):
        """'W' when some declaration of @p callee with @p nargs arguments takes argument @p k
        by non-const reference or pointer, 'R' when every one takes it by const reference,
        pointer to const or value, None when no declaration is found."""
        leaf = re.split(r"\.|->|::", callee)[-1]
        decls = [f for f in self.lookup(leaf)
                 if sum(1 for p in f.params if not p.has_default) <= nargs <= len(f.params)]
        if not decls:
            return None
        kinds = {f.params[k].kind for f in decls if k < len(f.params)}
        return "W" if kinds & {"W", "W*"} else "R"


# ---------------------------------------------------------------------------------------------
# Writes into the epoch (V4)
# ---------------------------------------------------------------------------------------------
Write = collections.namedtuple("Write", "path kind tok line text callee")


def _chain_end(T, j, e):
    """End of the postfix chain name(.m|->m|[..])* starting at @p j (a method call's `.m(`
    excluded), and the method name when the chain is followed by `.m(`."""
    k = j + 1
    while k < e:
        if T.t(k) in (".", "->") and T.k(k + 1) == "id":
            if T.t(k + 2) == "(":
                return k, T.t(k + 1)
            k += 2
            continue
        if T.t(k) == "[":
            k = T.close(k) + 1
            continue
        break
    return k, None


def scan_writes(T, a, b, epath, decls, skip=()):
    """Writes into epoch paths in tokens [a, b).  @p epath(s, e) maps an expression span to an
    epoch path or None; @p decls is a DeclIndex for argument kinds.  Spans in @p skip (token
    ranges) are not scanned.  Returns [Write]."""
    out = []
    calls = calls_in(T, a, b)
    arg_owner = {}
    for c in calls:
        for k, (s, e) in enumerate(c.args):
            s2, e2 = strip_parens(T, s, e)
            if T.t(s2) == "&":
                s2 += 1
            arg_owner[s2] = (c, k, e2)
    j = a
    while j < b:
        if any(x <= j < y for x, y in skip):
            j += 1
            continue
        if T.k(j) != "id" or T.t(j - 1) in (".", "->", "::") or T.t(j + 1) == "::":
            j += 1
            continue
        end, method = _chain_end(T, j, b)
        path = epath(j, end)
        if path is None:
            j += 1
            continue
        line, text = T.line(j), T.join(j, end)
        nxt, prv = T.t(end), T.t(j - 1)
        if method is None and nxt in _ASSIGN and not (nxt == "=" and _is_decl_init(T, j)):
            out.append(Write(path, "assign", j, line, text + " " + nxt, None))
        elif nxt in ("++", "--") or prv in ("++", "--"):
            out.append(Write(path, "increment", j, line, text, None))
        elif method is not None and method not in READ_METHODS:
            out.append(Write(path, "member call", j, line, f"{text}.{method}()", None))
        elif j in arg_owner and arg_owner[j][2] == end and method is None:
            c, k, _ = arg_owner[j]
            if READ_CALLEES.match(c.name):
                pass
            else:
                kind = decls.arg_kind(c.name, c.nargs, k)
                if kind == "W":
                    out.append(Write(path, "output argument", j, line, text, c.name))
                elif kind is None:
                    out.append(Write(path, "argument of an unclassified call", j, line, text, c.name))
        j = end if end > j else j + 1
    return out


def _is_decl_init(T, j):
    """True when the name at @p j is the declarator of a declaration (`T& x = ...`)."""
    p = T.t(j - 1)
    return p in ("&", "*", "&&", ">", ">>") or (T.k(j - 1) == "id" and p not in ("return", "case", "else"))


def epoch_path_of(node, ep_key):
    """'ep.<chain>' for a resolved node whose root is the epoch object @p ep_key, else None."""
    if isinstance(node, Ref) and node.root == ep_key and not node.prefix:
        return "ep" + "".join(seg for seg in node.chain)
    if isinstance(node, Ref) and node.root == ep_key and node.prefix == "&":
        return "ep" + "".join(node.chain)
    return None


def check_v4(T, solve_end, b, epath, decls, emit_call_tok, skip=()):
    """V4 over the epoch body tokens [solve_end, b): the writes into the epoch after the solve.
    @p emit_call_tok is the networkEmitStore call's token (None when absent).  Returns
    (problems, writes, allowed writes)."""
    problems, allowed = [], []
    writes = scan_writes(T, solve_end, b, epath, decls, skip)
    seen = collections.Counter()
    for w in writes:
        ok, why = False, ""
        if w.kind == "assign" and w.path in DRIVER_STORES:
            seen[w.path] += 1
            ok = seen[w.path] == 1
            why = "" if ok else " (a second store)"
            if ok and w.path == "ep.emitted" and (emit_call_tok is None or w.tok < emit_call_tok):
                ok, why = False, " (stored before the networkEmitStore call)"
        elif w.kind == "output argument":
            leaf = re.split(r"\.|->|::", w.callee)[-1]
            for prefix, callees in POST_SOLVE_OUTPUTS.items():
                if (w.path == prefix or w.path.startswith(prefix + ".")) and leaf in callees:
                    ok = True
        if ok:
            allowed.append(w)
        else:
            problems.append(f"{T.path}:{w.line}: after the solve, {w.kind} into {w.path} "
                            f"(`{w.text}`{' -> ' + w.callee if w.callee else ''}){why}")
    return problems, writes, allowed


# ---------------------------------------------------------------------------------------------
# The banner of include/app/NetworkEpoch.hpp (V10, V11)
# ---------------------------------------------------------------------------------------------
# Phrases of the banner's "written by" cells that name a seam without its identifier.
BANNER_PHRASES = {"the solve": SOLVE}


def parse_banner(text):
    """{member: {'type', 'avail', 'written', 'writers', 'stores'}} from the banner table of
    NetworkEpoch.hpp: the comment lines after the header line holding `member`, `available
    from` and `written by`, up to the first empty comment line.  A row starts with the member
    name at the header's member column; its cells are separated by two or more spaces (group
    type, available from, written by); a line with nothing at the member column continues the
    previous row's "written by" cell.  writers: the seam identifiers of that cell, except
    those in a "stores <seam>'s return" phrase (stores); "that seam" names the seam of the
    "available from" cell."""
    lines = text.split("\n")
    head = next((k for k, ln in enumerate(lines)
                 if "member" in ln and "available from" in ln and "written by" in ln), None)
    if head is None:
        return None
    col_m = lines[head].index("member")
    col_wb = lines[head].index("written by")
    rows, cur = collections.OrderedDict(), None
    for ln in lines[head + 1:]:
        body = ln.rstrip()
        if not body.strip(" */") or body.lstrip().startswith("*/"):
            break
        lead, rest = body[:col_m], body[col_m:]
        if lead.strip(" *") == "" and rest[:1].strip() and re.match(r"^\w+\s{2,}", rest):
            cells = re.split(r"\s{2,}", rest.strip())
            aligned = len(body) > col_wb and body[col_wb - 1] == " " and body[col_wb] != " "
            written = body[col_wb:].strip() if aligned else " ".join(cells[3:])
            cur = rows.setdefault(cells[0], {"type": cells[1] if len(cells) > 1 else "",
                                             "avail": cells[2] if len(cells) > 2 else "",
                                             "written": written})
        elif cur is not None:
            cur["written"] += " " + body.strip(" *").strip()
    for r in rows.values():
        w = r["written"]
        stores = set(re.findall(r"stores\s+(network\w+)'s return", w))
        for phrase, seam in BANNER_PHRASES.items():
            if re.search(r"stores\s+" + re.escape(phrase) + r"'s return", w):
                stores.add(seam)
        names = set(re.findall(r"\b(network\w+)\b", w))
        if "that seam" in w:
            names |= set(re.findall(r"\b(network\w+)\b", r["avail"]))
        r["stores"], r["writers"] = stores, names - stores
    return rows


# ---------------------------------------------------------------------------------------------
# The lint
# ---------------------------------------------------------------------------------------------
class SpineCall:
    __slots__ = ("callee", "call", "stmt", "defn", "out_paths", "reads", "nested")

    def __init__(self, callee, call, stmt):
        self.callee, self.call, self.stmt = callee, call, stmt
        self.defn = None
        self.out_paths = []   # epoch paths passed to output parameters
        self.reads = []       # epoch paths the bindings read
        self.nested = []      # (callee, path:line, epoch paths its bindings read) of the converted
                              # seams this seam's body calls with its own run and epoch


def nested_epoch_calls(tree, defs, f, depth=0, seen=None):
    """The converted seams the body of converted definition @p f calls in epoch form, passing
    its own run and epoch parameters (the solve's replay builders, for example), at any depth:
    [(callee, "path:line", [epoch paths its bindings read])]."""
    seen = set() if seen is None else seen
    out = []
    if depth > 4 or len(f.params) < 2 or (f.path, f.line) in seen:
        return out
    seen.add((f.path, f.line))
    T = tree.tokens(f.path)
    span = preamble_span(T, f)
    lo = span[1] if span else f.body[0] + 1
    rn, en = f.params[0].name, f.params[1].name
    for c in calls_in(T, lo, f.body[1]):
        if c.nargs < 2 or T.join(*c.args[0]) != rn or T.join(*c.args[1]) != en:
            continue
        leaf = re.split(r"\.|->|::", c.name)[-1]
        ds = [g for g in defs.find(leaf) if len(g.params) == c.nargs]
        if not ds:
            continue
        DT = tree.tokens(ds[0].path)
        out.append((leaf, f"{f.path}:{c.line}", [p for p, _ in binding_reads(DT, ds[0])]))
        out += nested_epoch_calls(tree, defs, ds[0], depth + 1, seen)
    return out


def arg_epoch_path(T, s, e):
    """'ep.a.b' when the argument tokens [s, e) are `ep(.id)*` (or `&ep...`), else None."""
    s, e = strip_parens(T, s, e)
    if T.t(s) == "&":
        s += 1
    if T.t(s) != "ep" or T.k(s) != "id":
        return None
    path, k = "ep", s + 1
    while k + 1 < e and T.t(k) == "." and T.k(k + 1) == "id":
        path += "." + T.t(k + 1)
        k += 2
    if k == e - 2 and T.t(k) == "(" and T.t(k + 1) == ")" and path.endswith(".x_emit"):
        return path + "()"
    return path if k == e else None


class Lint:
    """The epoch function of a tree and its spine: every call of a converted seam."""

    def __init__(self, tree):
        self.tree = tree
        self.fn = epoch_function(tree)
        self.defs = SeamDefs(tree)
        self.decls = DeclIndex(tree)
        self.spine = []
        self.problems = collections.defaultdict(list)
        self.info = collections.defaultdict(list)
        if self.fn is None:
            return
        T = self.fn.T
        stmts = self.fn.statements()
        for c in calls_in(T, self.fn.open + 1, self.fn.close):
            if not c.args:
                continue
            a0 = T.join(*c.args[0])
            if a0 not in ("run", self.fn.run_name):
                continue
            if c.nargs >= 2 and T.join(*c.args[1]) != "ep":
                continue
            st = next((k for k, (s, e) in enumerate(stmts) if s <= c.tok < e), None)
            sc = SpineCall(c.name, c, st)
            leaf = re.split(r"\.|->|::", c.name)[-1]
            ds = [f for f in self.defs.find(leaf) if len(f.params) == c.nargs]
            if ds:
                sc.defn = ds[0]
                DT = self.tree.tokens(sc.defn.path)
                sc.reads = [p for p, _ in binding_reads(DT, sc.defn)]
                for k in range(2, c.nargs):
                    path = arg_epoch_path(T, *c.args[k])
                    if path and sc.defn.params[k].kind in ("W", "W*"):
                        sc.out_paths.append(path)
                if is_card_form(sc.defn):
                    sc.out_paths += written_bindings(DT, sc.defn)
                sc.nested = nested_epoch_calls(self.tree, self.defs, sc.defn)
            self.spine.append(sc)

    # -- positions --
    def stmt_index(self, tok):
        for k, (s, e) in enumerate(self.fn.statements()):
            if s <= tok < e:
                return k
        return None

    def solve_stmt(self):
        T = self.fn.T
        for k, (s, e) in enumerate(self.fn.statements()):
            if T.t(s) == "ep" and T.t(s + 1) == "." and T.t(s + 2) == "solve" and T.t(s + 3) == "=" \
                    and T.t(s + 4) == SOLVE:
                return k, (s, e)
        return None, None

    def first_writes(self):
        """{group: statement index} of each group's first write in the epoch function: the
        construction for se and ws; else the first statement that assigns into the group,
        calls a member function that writes it, or passes it to an output parameter (a
        reference binding alone is not a write: `sec_asm_ms_epoch` is bound at the epoch top,
        written by PASS B)."""
        T, fn = self.fn.T, self.fn
        out = {}
        epd = fn.ep_decl()
        stmts = fn.statements()
        if epd is not None:
            k = next(k for k, (s, e) in enumerate(stmts) if s <= epd.tok < e)
            out["se"] = out["ws"] = k
        res = fn.resolver()

        def epath(s, e):
            try:
                return epoch_path_of(res.resolve(s, e), "E:ep")
            except (ValueError, IndexError):
                return None

        for k, (s, e) in enumerate(stmts):
            for w in scan_writes(T, s, e, epath, self.decls):
                g = group_of(w.path)
                if g and g not in out:
                    out[g] = k
            for sc in self.spine:
                if sc.stmt == k:
                    for p in sc.out_paths:
                        g = group_of(p)
                        if g and g not in out:
                            out[g] = k
        return out

    # -- checks --
    def v4(self):
        fn = self.fn
        T = fn.T
        k, span = self.solve_stmt()
        if span is None:
            self.problems["V4"].append(f"{fn.path}: no `ep.solve = {SOLVE}(...)` statement")
            return
        res = fn.resolver()

        def epath(s, e):
            try:
                return epoch_path_of(res.resolve(s, e), "E:ep")
            except (ValueError, IndexError):
                return None

        emit = next((sc.call.tok for sc in self.spine if sc.callee == EMIT), None)
        if emit is None:
            emit = next((c.tok for c in calls_in(T, fn.open + 1, fn.close) if c.name == EMIT), None)
        probs, writes, allowed = check_v4(T, span[1], fn.close, epath, self.decls, emit)
        self.problems["V4"].extend(probs)
        self.info["V4"].append(f"{len(writes)} writes into ep after the solve, {len(allowed)} declared")
        # ep.clock: assigned exactly once, before any read.
        assigns = [w for w in scan_writes(T, fn.open + 1, fn.close, epath, self.decls)
                   if w.path == "ep.clock" and w.kind == "assign"]
        if len(assigns) != 1:
            self.problems["V4"].append(f"{fn.path}: `ep.clock` assigned {len(assigns)} times (expected once)")
        else:
            at = assigns[0].tok
            reads = [j for j in range(fn.open + 1, at) if T.t(j) == "ep" and T.t(j + 1) == "."
                     and T.t(j + 2) == "clock"]
            for j in reads:
                self.problems["V4"].append(f"{fn.path}:{T.line(j)}: `ep.clock` read before its store")
            for sc in self.spine:
                if sc.call.tok < at and any(p.startswith("ep.clock") for p in sc.reads):
                    self.problems["V4"].append(f"{fn.path}:{T.line(sc.call.tok)}: {sc.callee} reads "
                                               "ep.clock before its store")

    def v10(self):
        """Every group a spine seam's bindings read is written at or before its spine line; a
        converted seam that a spine seam's body calls with the caller's run and epoch (the
        solve's replay builders) reads only groups written strictly before the caller's spine
        line, since the caller's own writes are still under way while it runs."""
        avail = self.first_writes()
        n = n_nested = 0
        for sc in self.spine:
            if sc.defn is None:
                continue
            for p in sc.reads:
                g = group_of(p)
                if g is None:
                    continue
                n += 1
                if g not in avail:
                    self.problems["V10"].append(
                        f"{self.fn.path}:{sc.call.line}: {sc.callee} reads {p}, a group the "
                        "epoch function never writes")
                elif avail[g] > sc.stmt:
                    first = self.fn.statements()[avail[g]][0]
                    self.problems["V10"].append(
                        f"{self.fn.path}:{sc.call.line}: {sc.callee} reads {p} before the group is "
                        f"available (first written at line {self.fn.T.line(first)})")
            for callee, where, reads in sc.nested:
                for p in reads:
                    g = group_of(p)
                    if g is None:
                        continue
                    n_nested += 1
                    if g not in avail or avail[g] >= sc.stmt:
                        self.problems["V10"].append(
                            f"{where}: {callee}, called inside {sc.callee} (spine line "
                            f"{self.fn.path}:{sc.call.line}), reads {p}, which is not written before "
                            "that spine line")
        self.info["V10"].append(f"{len(self.spine)} spine calls, {n} epoch reads checked against "
                                f"{len(avail)} groups' first writes; {n_nested} reads of seams called "
                                "inside a seam")

    def v11(self, partial=False):
        """@p partial: the tree is mid-conversion (the relocation verifier's EP1a-1 and EP1b
        runs), so a banner writer with no epoch form yet is skipped instead of failed."""
        text = self.tree.read(EPOCH_HPP)
        rows = parse_banner(text or "")
        if not rows:
            self.problems["V11"].append(f"{EPOCH_HPP}: the banner table (member / available from / "
                                        "written by) was not found")
            return
        got = collections.defaultdict(set)
        for sc in self.spine:
            for p in sc.out_paths:
                got[group_of(p)].add(re.split(r"\.|->|::", sc.callee)[-1])
        stores = collections.defaultdict(set)
        T = self.fn.T
        for s, e in self.fn.statements():
            if T.t(s) == "ep" and T.t(s + 1) == "." and T.k(s + 2) == "id" and T.t(s + 3) == "=" \
                    and T.t(s + 4).startswith("network") and T.t(s + 5) == "(":
                stores[T.t(s + 2)].add(T.t(s + 4))
        skipped = set()
        for g in sorted(set(rows) | set(got) | set(stores)):
            want = set(rows.get(g, {}).get("writers", set()))
            if partial:
                pend = {w for w in want if not self.defs.find(w)}
                skipped |= pend
                want -= pend
            if g not in rows and (got[g] or stores[g]):
                self.problems["V11"].append(f"group {g}: written ({sorted(got[g] | stores[g])}) but "
                                            "not in the banner")
                continue
            if got[g] != want:
                self.problems["V11"].append(
                    f"group {g}: spine writers {sorted(got[g])}, banner {sorted(want)}")
            wst = rows.get(g, {}).get("stores", set())
            if stores[g] != wst:
                self.problems["V11"].append(
                    f"group {g}: driver stores {sorted(stores[g])}, banner {sorted(wst)}")
        self.info["V11"].append(f"{len(rows)} banner rows, {sum(len(v) for v in got.values())} "
                                "output arguments on the spine"
                                + (f"; {len(skipped)} banner writers not converted yet" if skipped else ""))


def run(tree, out=print, partial=False, presolve_writer="outputs"):
    """The three checks on @p tree; 0 pass, 1 a check fails, 2 no epoch function or type.
    @p partial: the tree is part way through the conversion (steps EP1a-1 and EP1b before
    the last merge), so a banner writer with no epoch form yet is skipped in V11.
    @p presolve_writer: "epoch" for the card's pre-solve form (the writers before the solve
    take `NetworkEpoch& ep`), where V11 is n/a; in the default form a seam taking
    `NetworkEpoch&` fails V11."""
    out(f"nom_epoch_lint: {tree.rev}" + (" (partial conversion)" if partial else "")
        + (" (the card's pre-solve form)" if presolve_writer == "epoch" else ""))
    lint = Lint(tree)
    if lint.fn is None:
        out(f"nom_epoch_lint: Application::{EPOCH_FN} is not defined in {tree.rev}")
        return 2
    if tree.read(EPOCH_HPP) is None:
        out(f"nom_epoch_lint: {EPOCH_HPP} does not exist in {tree.rev}")
        return 2
    lint.v4()
    lint.v10()
    card = presolve_writer == "epoch"
    if card:
        # The card's pre-solve form: a pre-solve writer takes the whole epoch, so its spine
        # line names no group the banner could be checked against (the relocation verifier
        # reports V11 as n/a for the same reason).
        n = len({sc.callee for sc in lint.spine if sc.defn is not None and is_card_form(sc.defn)})
        lint.info["V11"].append(f"the card's pre-solve form ({n} writers take NetworkEpoch&), "
                                "no group outputs on the spine")
    else:
        lint.v11(partial=partial)
        for callee in sorted({sc.callee for sc in lint.spine
                              if sc.defn is not None and is_card_form(sc.defn)}):
            lint.problems["V11"].append(
                f"{callee} takes NetworkEpoch& (the card's pre-solve form); in this form a writer "
                "names its groups as output parameters (--presolve-writer epoch lints the card's form)")
    bad = 0
    for chk in ("V4", "V10", "V11"):
        probs = lint.problems[chk]
        bad += len(probs)
        st = "n/a" if chk == "V11" and card else ("pass" if not probs else "FAIL")
        out(f"  [{st}] {chk}: " + "; ".join(lint.info[chk]))
        for p in probs:
            out("      " + p)
    out(f"nom_epoch_lint: {'PASS' if not bad else f'FAIL ({bad})'}")
    return 0 if not bad else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tree", default=str(ROOT), help="the source tree to lint (default: this repository)")
    ap.add_argument("--partial", action="store_true",
                    help="the conversion is not finished (steps EP1a-1 and EP1b before the last merge): "
                         "V11 skips banner writers that have no epoch form yet")
    ap.add_argument("--presolve-writer", choices=("outputs", "epoch"), default="outputs",
                    help="the tree's pre-solve form: named group outputs (default) or the card's "
                         "NetworkEpoch& (V11 n/a)")
    args = ap.parse_args(argv)
    return run(DiskTree(args.tree), partial=args.partial, presolve_writer=args.presolve_writer)


if __name__ == "__main__":
    sys.exit(main())

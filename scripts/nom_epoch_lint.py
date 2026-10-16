#!/usr/bin/env python3
"""Permanent lint of the network producer's epoch function (card NOM3 step EP1, checks V0, V4, V10, V11)
and of the entity-table writers (check V12).

    python scripts/nom_epoch_lint.py [--tree DIR] [--partial] [--presolve-writer {outputs,epoch}]

Run it with PYTHONIOENCODING=utf-8 on a cp1252 console, before committing a change to a per-epoch seam signature or to
src/core/app/ApplicationNetworkEpoch.cpp (the seam contract in CLAUDE.md).  It reads the
working files of the tree (default: the repository this script sits in), needs no git
revision and no build, and uses only the Python 3 standard library and scripts/nom_cpp.py.
Exit 0 when every check passes, 1 when one fails, 2 when the epoch function or the epoch
type cannot be found.

What it proves (spec sections 2.3, 3.1 and 5.5 of the EP1 specification):

  V0  The reading.  The epoch function the lint reads is the one the compiler builds and its
      calls reach the seams they name: Application::networkProcessEpoch_ is defined once
      under src/; its translation unit holds no preprocessor directive but #include and
      conditional groups of #include lines (a #define of a seam's name, an #if 0 around the
      read copy with the compiled one under #else, are what this rules out); no name the
      epoch function calls is declared in that translation unit outside the function (a
      better-matching overload, a variable or a type of a seam's name would capture the
      call); and each converted seam the spine calls has one epoch-form definition of that
      arity.  The text read is the text compiled: the unit, every project file it reaches and
      CMakeLists.txt hold no line splice (a backslash before a line end: the compiler splices
      `/* a *\\<NL>/ code /* b */` into live code the lexer reads as one comment), no
      carriage return that ends no line, no control character and no bytes that are not UTF-8
      (nom_cpp.reading_problems).  What the translation unit reads beyond its own text:
      every tree file it reaches through #include, directly or through a header, is a header (.hpp or .h under include/
      or third_party/), so no source, test or inline file carries a declaration into it, and
      every directive names its header literally; no declaration in that closure can take a
      spine call from its seam (a declaration of the seam's name whose first parameter binds
      `run` exactly, through NetworkRun, an alias of it, a template parameter or `auto`, has
      the parameter types of the seam's one epoch-form definition: a narrowed overload taking
      `NetworkEpoch&` would be the better match).  How the build compiles it: CMakeLists.txt
      lists the translation unit once, as a target's source, and names it nowhere else (no
      per-source compile option, definition or flag but `COMPILE_FLAGS "/bigobj"`); no compile
      definition defines a name the epoch function calls; no option forces a header into
      every translation unit (/FI, -include, precompiled headers).
  V4  Driver writes after the solve.  In Application::networkProcessEpoch_, after the
      statement `ep.solve = networkSolveGlue(...)`, the only writes into `ep` are the four
      driver stores `ep.ar_fixes =`, `ep.fixed_cycles =`, `ep.prep =`, `ep.emitted =` (each
      once; `ep.emitted` after the networkEmitStore call) and the output arguments
      (`ep.solve.handoff` to networkPostFitRaim and networkClockQc, `ep.prep.sol` to
      networkEmitStore, `ep.events` to the post-solve writers of the epoch's events listed
      in POST_SOLVE_OUTPUTS).  A write is an assignment, an increment, a member call outside the
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

The module also holds the tree reader, the comment-free statement rendering and the
preprocessor reading that scripts/nom_ep1_verify.py uses (DiskTree, render(),
epoch_function(), pp_directive(), cond_include_lines()), so the relocation verifier and this
lint read a tree the same way.  The verifier runs V4 on an empty
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
from nom_cpp import (FileIndex, Ref, Resolver, Scopes, Tokens, calls_in, match_angle, reading_problems,  # noqa: E402
                     statements, strip_parens)
from rt_seam_verify import strip_comments  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[1]
EPOCH_CPP = "src/core/app/ApplicationNetworkEpoch.cpp"
EPOCH_HPP = "include/app/NetworkEpoch.hpp"
EPOCH_FN = "networkProcessEpoch_"
SOLVE = "networkSolveGlue"
EMIT = "networkEmitStore"
OBSERVE = "networkObserveEpoch"   # the observer table's slot walk (card NOM3 step EP3)
OBSERVERS_CPP = "src/core/filter/NetworkEpochObservers.cpp"
_OBSERVER_ROW = re.compile(r'\{"(\w+)",\s*"[^"]*",\s*"[^"]*",\s*Slot::(\w+),\s*&(\w+)\}')
PREAMBLE_OPEN = re.compile(r"//\s*-{2,}\s*From the run\b")
PREAMBLE_CLOSE = re.compile(r"//\s*-{2,}\s*End of the bindings")

# The driver's stores after the solve, and the output arguments a post-solve seam may take.
DRIVER_STORES = ("ep.ar_fixes", "ep.fixed_cycles", "ep.prep", "ep.emitted")
POST_SOLVE_OUTPUTS = {"ep.solve.handoff": ("networkPostFitRaim", "networkClockQc"),
                      "ep.prep.sol": ("networkEmitStore",),
                      # the epoch's events: each writer its own group (app/NetworkEpoch.hpp)
                      "ep.events": ("networkInstallCarry", "networkGrossLosScreen", "networkSolPrep",
                                    "networkShHealth", "networkLeveling", "networkClockQc",
                                    "networkObserveEpoch", "networkStashStatus")}
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
def pp_directive(text):
    """The directive name of a source line or pp token that is a preprocessor directive (`#`
    or the digraph `%:` first), else None."""
    m = re.match(r"\s*(?:#|%:)\s*(\w*)", text)
    return m.group(1) if m else None


def cond_include_lines(T):
    """Lines of the conditional directives (#if, #ifdef, #ifndef, #elif, #else, #endif) of
    groups that hold only #include directives and nested such groups: the one conditional
    form a step file may carry (`#ifdef _OPENMP / #include <omp.h> / #endif`), since it can
    only choose a header, never compile a definition in or out."""
    ok, stack = set(), []
    for j in range(T.n):
        d = pp_directive(T.t(j)) if T.k(j) == "pp" else None
        if d in ("if", "ifdef", "ifndef"):
            stack.append({"lines": [T.line(j)], "clean": True})
            continue
        if not stack:
            continue
        if d in ("elif", "else"):
            stack[-1]["lines"].append(T.line(j))
            continue
        if d == "endif":
            grp = stack.pop()
            if grp["clean"]:
                ok.update(grp["lines"] + [T.line(j)])
            elif stack:
                stack[-1]["clean"] = False
            continue
        if d == "include":
            continue
        stack[-1]["clean"] = False        # code, or another directive, inside the group
    return ok


def cond_include_groups(T):
    """[(directive lines, [(line, include_target)])] of the groups cond_include_lines accepts
    (those that hold only #include directives and nested such groups), each with the
    #include lines it holds, nested groups' included.  The relocation verifier fails a group
    that enters the tree around a frozen #include (V1e): a group can choose a header, and
    `#if 0` chooses none."""
    out, stack = [], []
    for j in range(T.n):
        d = pp_directive(T.t(j)) if T.k(j) == "pp" else None
        if d in ("if", "ifdef", "ifndef"):
            stack.append({"lines": [T.line(j)], "clean": True, "incs": []})
            continue
        if not stack:
            continue
        if d in ("elif", "else"):
            stack[-1]["lines"].append(T.line(j))
            continue
        if d == "endif":
            grp = stack.pop()
            if grp["clean"]:
                out.append((grp["lines"] + [T.line(j)], grp["incs"]))
                if stack:
                    stack[-1]["incs"] += grp["incs"]
            elif stack:
                stack[-1]["clean"] = False
            continue
        if d == "include":
            stack[-1]["incs"].append((T.line(j), include_target(T.t(j))))
            continue
        stack[-1]["clean"] = False
    return out


# ---------------------------------------------------------------------------------------------
# What a translation unit reads: its #include closure, the declarations there, the build list
# ---------------------------------------------------------------------------------------------
# The include directories of the project's targets (CMakeLists.txt: include/ for every target,
# third_party/sofa through the sofa library gnss_core links); a name found in none of them, nor
# beside an including file, is a library or system header.
INCLUDE_DIRS = ("include", "third_party/sofa")
HEADER_EXT = (".hpp", ".h")
CMAKE = "CMakeLists.txt"
# The C++17 standard library headers (an angle include of one names no file of the tree).
STD_HEADERS = frozenset("""
algorithm any array atomic bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes
ciso646 climits clocale cmath codecvt complex condition_variable csetjmp csignal cstdarg cstddef
cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype deque exception execution filesystem
forward_list fstream functional future initializer_list iomanip ios iosfwd iostream istream
iterator limits list locale map memory memory_resource mutex new numeric optional ostream queue
random ratio regex scoped_allocator set shared_mutex sstream stack stdexcept streambuf string
string_view system_error thread tuple type_traits typeindex typeinfo unordered_map
unordered_set utility valarray variant vector
""".split())
_INCLUDE = re.compile(r'\s*(?:#|%:)\s*include\s*(?:"([^"\n]*)"|<([^>\n]*)>)')


def include_target(text):
    """(quote, name) of an #include directive's text: ('"', 'app/X.hpp') or ('<', 'vector');
    (None, the directive's text) when it names its header through a macro."""
    m = _INCLUDE.match(text)
    if not m:
        return None, text.strip()
    return ('"', m.group(1)) if m.group(1) is not None else ("<", m.group(2))


def include_spelling(quote, name):
    return f'"{name}"' if quote == '"' else f"<{name}>"


def resolve_include(tree, includer, quote, name, tu=None):
    """The tree file an #include names, searched as the compiler searches: a quoted name beside
    the including file, then beside the translation unit @p tu, then in each include
    directory; an angle name in the include directories only.  None when no tree file matches
    (a library or system header).  A name that leaves the tree (`..` above the root, an
    absolute path) is returned as normalized, starting with `../` or `/`, so no caller takes
    it for a header of the tree."""
    import posixpath
    if name.startswith(("/", "\\")) or re.match(r"^[A-Za-z]:", name):
        return "/" + name.lstrip("/\\")
    name = name.replace("\\", "/")
    dirs = []
    if quote == '"':
        dirs.append(posixpath.dirname(includer))
        if tu is not None and posixpath.dirname(tu) not in dirs:
            dirs.append(posixpath.dirname(tu))
    dirs += list(INCLUDE_DIRS)
    for d in dirs:
        c = posixpath.normpath(posixpath.join(d, name)) if d else posixpath.normpath(name)
        if c == ".." or c.startswith("../"):
            return c
        if tree.read(c) is not None:
            return c
    return None


def is_header_path(path):
    """A header of the project or of a vendored library: a .hpp or .h file under include/ or
    third_party/."""
    return path is not None and path.endswith(HEADER_EXT) and path.startswith(("include/", "third_party/"))


# What a file declares, includes and calls depends on its text alone, so the readings below are
# kept per text: a file the candidate shares with the frozen tree is read once.
_BY_TEXT = {}


def _by_text(kind, tree, path, compute):
    text = tree.read(path)
    if text is None:
        return None
    key = (kind, text)
    if key not in _BY_TEXT:
        _BY_TEXT[key] = compute(text)
    return _BY_TEXT[key]


_INCLUDE_LINE = re.compile(r"^[ \t]*(?:#|%:)[ \t]*include\b.*$", re.M)


def file_includes(tree, path):
    """[(line, quote, name)] of every #include directive of @p path (conditional ones too), in
    order, read from its comment-free text; quote None for a directive that names its header
    through a macro."""
    def compute(text):
        out = []
        if "include" in text:
            text = strip_comments(text)
            for m in _INCLUDE_LINE.finditer(text):
                q, name = include_target(m.group())
                out.append((text.count("\n", 0, m.start()) + 1, q, name))
        return out
    return _by_text("includes", tree, path, compute) or []


def overlay_parts(tree):
    """(the tree that owns the unchanged files, the overlay's replaced paths, whether the overlay
    adds or deletes a file): an overlay that only edits files resolves every include as its
    parent does."""
    root = getattr(tree, "parent", None) or tree
    ov = getattr(tree, "overrides", None) or {}
    structural = any(v is None or root.read(p) is None for p, v in ov.items())
    return root, ov, structural


def include_closure(tree, tu):
    """{file: [(line, quote, name, resolved)]} of the translation unit @p tu and every tree file
    it reaches through #include, conditional directives included (a superset of what one
    configuration compiles); resolved is None for a library or system header.  The closure of
    a tree is kept; an overlay that only edits files outside it reuses its parent's."""
    root, ov, structural = overlay_parts(tree)
    cache = root.__dict__.setdefault("_ep1_closures", {})
    hit = cache.get(tu)
    if hit is not None and not structural and not (set(ov) & set(hit)):
        return hit
    rcache = {} if structural else root.__dict__.setdefault("_ep1_resolved", {})
    clo = _include_closure(tree, tu, rcache)
    if not ov:
        cache[tu] = clo
    return clo


def _include_closure(tree, tu, rcache):
    import posixpath
    tudir = posixpath.dirname(tu)
    out, stack = {}, [tu]
    while stack:
        p = stack.pop()
        if p in out:
            continue
        items = []
        if not p.startswith(("../", "/")) and p != "..":
            for ln, q, name in file_includes(tree, p):
                r = None
                if q is not None:
                    key = (p, q, name, tudir if q == '"' else None)
                    if key not in rcache:
                        rcache[key] = resolve_include(tree, p, q, name, tu)
                    r = rcache[key]
                items.append((ln, q, name, r))
                if r is not None and r not in out:
                    stack.append(r)
        out[p] = items
    return out


def _is_template(T, f):
    """True when the declaration @p f follows a `template <...>` head."""
    if T.t(f.start - 1) not in (">", ">>"):
        return False
    for k in range(f.start - 2, max(-1, f.start - 400), -1):
        if T.t(k) == "template" and T.t(k + 1) == "<":
            m = match_angle(T, k + 1, T.n)
            return m == f.start - 1
    return False


def _first_type(types):
    """The bare type name of a parameter list's first parameter: cv-qualifiers, references,
    pointers and namespace qualifiers removed (`const ns::NetworkRun&` reads `NetworkRun`)."""
    if not types:
        return ""
    t = re.sub(r"\b(const|volatile)\b", " ", types[0])
    t = re.sub(r"[&*\s]+", " ", t).strip()
    return t.split("::")[-1].strip()


def file_decls(tree, path):
    """What one file declares that can change how a call resolves: {name: [(parameter types,
    template?)]} of its free functions (declarations and definitions, in any namespace;
    member functions excluded), the names it #defines, the names its using-declarations
    bring in, whether it holds a using-directive, and {alias: aliased type text} of its
    type aliases (`using A = B;`, `typedef B A;`)."""
    return _by_text("decls", tree, path, lambda text: _file_decls(tree, path)) or ({}, set(), set(), False, {})


def _file_decls(tree, path):
    T, idx = tree.tokens(path), tree.index(path)
    funcs = collections.defaultdict(list)
    macros, usings, aliases, directive = set(), set(), {}, False
    if T is not None:
        for f in idx.funcs:
            if f.cls:
                continue
            types = tuple(ptype(q) for q in f.params) if f.params is not None else ("?",)
            funcs[f.name].append((types, _is_template(T, f)))
        for j in range(T.n):
            x = T.t(j)
            if T.k(j) == "pp":
                m = re.match(r"\s*(?:#|%:)\s*define\s+(\w+)", x)
                if m:
                    macros.add(m.group(1))
                continue
            if x == "using":
                e = j + 1
                while e < T.n and T.t(e) not in (";", "{", "}"):
                    e += 1
                body = [T.t(q) for q in range(j + 1, e)]
                if body[:1] == ["namespace"]:
                    directive = True
                elif "=" in body and body and T.k(j + 1) == "id":
                    aliases[body[0]] = " ".join(body[body.index("=") + 1:])
                elif body:
                    usings.add(body[-1])
            elif x == "typedef":
                e = j + 1
                while e < T.n and T.t(e) not in (";", "{", "}"):
                    e += 1
                if e - 1 > j + 1 and T.k(e - 1) == "id":
                    aliases[T.t(e - 1)] = " ".join(T.t(q) for q in range(j + 1, e - 1))
    return dict(funcs), macros, usings, directive, aliases


def file_calls(tree, path):
    """({called name}, {identifier}) of @p path: the last component of every `name(` call and
    every identifier token."""
    def compute(text):
        T = tree.tokens(path)
        return ({re.split(r"\.|->|::", c.name)[-1] for c in calls_in(T, 0, T.n)},
                {T.t(j) for j in range(T.n) if T.k(j) == "id"})
    return _by_text("calls", tree, path, compute) or (set(), set())


def cmake_commands(text):
    """The command invocations of a CMake file: [(name, [(argument, line)], first line)], the
    name lower-cased, quoted arguments without their quotes, comments (`#` to the end of the
    line, `#[[...]]`) removed."""
    out, i, n, line = [], 0, len(text), 1

    def skip_comment(i, line):
        m = re.match(r"#\[(=*)\[", text[i:])
        if m:
            end = text.find("]" + m.group(1) + "]", i)
            end = n if end < 0 else end + len(m.group(1)) + 2
        else:
            end = text.find("\n", i)
            end = n if end < 0 else end
        return end, line + text.count("\n", i, end)

    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c == "#":
            i, line = skip_comment(i, line)
        elif re.match(r"[A-Za-z_]", c):
            m = re.match(r"[A-Za-z_]\w*", text[i:])
            name, j = m.group(), i + m.end()
            while j < n and text[j] in " \t":
                j += 1
            if j >= n or text[j] != "(":
                i = j
                continue
            first, depth, args, j = line, 1, [], j + 1
            while j < n and depth:
                c = text[j]
                if c == "\n":
                    line += 1
                    j += 1
                elif c in " \t\r":
                    j += 1
                elif c == "#":
                    j, line = skip_comment(j, line)
                elif c == "(":
                    depth += 1
                    j += 1
                elif c == ")":
                    depth -= 1
                    j += 1
                elif c == '"':
                    k, start = j + 1, line
                    while k < n and text[k] != '"':
                        if text[k] == "\\":
                            k += 1
                        elif text[k] == "\n":
                            line += 1
                        k += 1
                    args.append((text[j + 1:k], start))
                    j = k + 1
                elif c == "[" and re.match(r"\[=*\[", text[j:]):
                    m2 = re.match(r"\[(=*)\[", text[j:])
                    end = text.find("]" + m2.group(1) + "]", j)
                    end = n if end < 0 else end
                    args.append((text[j + m2.end():end], line))
                    line += text.count("\n", j, end)
                    j = end + len(m2.group(1)) + 2
                else:
                    m2 = re.match(r'(?:[^\s()#"\\]|\\.)+', text[j:])
                    if not m2:
                        j += 1
                        continue
                    args.append((m2.group(), line))
                    j += m2.end()
            out.append((name.lower(), args, first))
            i = j
        else:
            i += 1
    return out


_CMAKE_KEYWORDS = frozenset(("PRIVATE", "PUBLIC", "INTERFACE", "BEFORE", "AFTER", "PROPERTIES",
                             "SOURCE", "TARGET", "DIRECTORY", "APPEND", "APPEND_STRING", "GLOBAL"))


def cmake_macro_names(cmds):
    """{name: line} of every preprocessor macro the CMake commands define for a compile: the
    entries of add_compile_definitions / target_compile_definitions and of a COMPILE_DEFINITIONS
    property, and any `-D` / `/D` option (in add_definitions, compile options or a flags
    string)."""
    out = {}
    for name, args, _ in cmds:
        defs = name in ("add_compile_definitions", "target_compile_definitions")
        in_prop = False
        for k, (a, ln) in enumerate(args):
            for m in re.finditer(r"(?:^|[\s;])[-/]D\s*([A-Za-z_]\w*)", a):
                out.setdefault(m.group(1), ln)
            if a.upper().startswith("COMPILE_DEFINITIONS"):
                in_prop = True
                continue
            if defs and (k == 0 and name == "target_compile_definitions"):
                continue
            if (defs or in_prop) and a not in _CMAKE_KEYWORDS:
                for item in a.split(";"):
                    m = re.match(r"\s*([A-Za-z_]\w*)", item)
                    if m:
                        out.setdefault(m.group(1), ln)
    return out


def cmake_forced_includes(cmds):
    """[(line, text)] of the CMake commands and options that force a header into every
    translation unit of a target (`/FI`, `-include`, target_precompile_headers)."""
    out = []
    for name, args, first in cmds:
        if name == "target_precompile_headers":
            out.append((first, name))
        for a, ln in args:
            if re.search(r"(?:^|[\s;])(?:[-/]FI|-include\b|/Yu|/Yc)", a):
                out.append((ln, a))
    return out


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
    __slots__ = ("callee", "call", "stmt", "defn", "out_paths", "reads", "nested", "observers")

    def __init__(self, callee, call, stmt):
        self.callee, self.call, self.stmt = callee, call, stmt
        self.defn = None
        self.out_paths = []   # epoch paths passed to output parameters
        self.reads = []       # epoch paths the bindings read
        self.nested = []      # (callee, path:line, epoch paths its bindings read) of the converted
                              # seams this seam's body calls with its own run and epoch
        self.observers = []   # a slot walk's rows (card NOM3 step EP3), read as spine calls here


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


def observer_rows(tree):
    """slot -> [seam name] of the observer table (card NOM3 step EP3), in table order; a row
    whose name is not its function is returned under the slot "?" so the caller reports it."""
    out = collections.defaultdict(list)
    for name, slot, fn in _OBSERVER_ROW.findall(tree.read(OBSERVERS_CPP) or ""):
        out[slot if name == fn else "?"].append(fn)
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
            if leaf == OBSERVE and c.nargs == 3:
                # The slot walk runs its rows at this spine line: their reads are this call's.
                rows = observer_rows(self.tree)
                for bad in rows.get("?", []):
                    self.problems["V10"].append(f"{OBSERVERS_CPP}: the row of {bad} names another seam")
                slot = re.split(r"::", T.join(*c.args[2]))[-1]
                if not rows.get(slot):
                    self.problems["V10"].append(f"{self.fn.path}:{c.line}: slot {slot} has no row")
                for name in rows.get(slot, []):
                    ods = [f for f in self.defs.find(name) if len(f.params) == 2]
                    if not ods:
                        self.problems["V10"].append(f"{OBSERVERS_CPP}: row {name} has no epoch form")
                        continue
                    OT = self.tree.tokens(ods[0].path)
                    sc.reads += [p for p, _ in binding_reads(OT, ods[0])]
                    sc.nested += nested_epoch_calls(self.tree, self.defs, ods[0])
                    sc.observers.append(name)
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
    def v0(self):
        """The reading (module docstring, V0)."""
        fn, T, P = self.fn, self.fn.T, self.problems["V0"]
        defs = []
        for p in self.tree.files("src"):
            text = self.tree.read(p)
            if p.endswith(".cpp") and text and EPOCH_FN in text:
                defs += [f"{p}:{f.line}" for f in self.tree.index(p).funcs
                         if f.name == EPOCH_FN and f.is_def and f.cls == "Application"]
        if len(defs) != 1:
            P.append(f"Application::{EPOCH_FN} has {len(defs)} definitions ({', '.join(defs)}); the lint "
                     "reads the first, the compiler builds one")
        condl = cond_include_lines(T)
        for j in range(T.n):
            if T.k(j) == "pp" and pp_directive(T.t(j)) != "include" and T.line(j) not in condl:
                P.append(f"{fn.path}:{T.line(j)}: the directive `{T.t(j)[:70]}` in the epoch function's "
                         "translation unit (only #include and conditional groups of #include lines)")
        start = fn.func.name_tok - 1
        while start >= 0 and T.k(start) != "pp" and T.t(start) not in (";", "{", "}"):
            start -= 1
        start += 1
        called = {re.split(r"\.|->|::", c.name)[-1] for c in calls_in(T, fn.open + 1, fn.close)}
        hits = sorted({T.t(j) for j in list(range(0, start)) + list(range(fn.close + 1, T.n))
                       if T.k(j) == "id" and T.t(j) in called})
        if hits:
            P.append(f"{fn.path}: {hits} named outside the epoch function in its translation unit; each is a "
                     "name the epoch function calls, so a declaration there could capture the call")
        for sc in self.spine:
            leaf = re.split(r"\.|->|::", sc.callee)[-1]
            ds = [f for f in self.defs.find(leaf) if len(f.params) == sc.call.nargs]
            if len(ds) > 1:
                P.append(f"{fn.path}:{T.line(sc.call.tok)}: {sc.callee} has {len(ds)} epoch-form definitions "
                         f"of arity {sc.call.nargs} ({', '.join(f'{f.path}:{f.line}' for f in ds)})")
        self.info["V0"].append(f"one definition, {len(called)} called names, {len(self.spine)} spine calls")
        clo = self.v0_includes()
        self.v0_reading(clo)
        self.v0_overloads(clo)
        self.v0_cmake(called)

    def v0_includes(self):
        """The epoch function's translation unit reads only headers: every tree file it
        reaches through #include, directly or through a header, is a .hpp or .h file under
        include/ or third_party/, and every directive names its header literally.  A source
        file, a test file, an inline file or a file beside the translation unit would carry
        declarations into the epoch function's scope that no header review reads.  Returns the
        include closure ({file: [(line, quote, name, resolved)]})."""
        P = self.problems["V0"]
        clo = include_closure(self.tree, self.fn.path)
        for p in sorted(clo):
            for ln, q, name, r in clo[p]:
                if q is None:
                    P.append(f"{p}:{ln}: `{name[:70]}` names its header through a macro (the epoch function's "
                             "translation unit includes headers by name only)")
                elif r is not None and not is_header_path(r):
                    P.append(f"{p}:{ln}: `#include {include_spelling(q, name)}` reaches {r}, which is not a header "
                             "under include/ or third_party/: the epoch function's translation unit reads "
                             "headers only")
        self.info["V0"].append(f"{len(clo)} files in its translation unit")
        return clo

    def v0_reading(self, clo):
        """The lint reads the text the compiler reads: the epoch function's translation unit,
        every project file it reaches (under src/ or include/) and CMakeLists.txt hold nothing
        nom_cpp.reading_problems names (a line splice, a carriage return that ends no line, a
        control character, bytes that are not UTF-8).  The lexer implements no translation
        phase 1 or 2, so `/* a *\\<NL>/ return LoopSignal::Next; /* b */` in the epoch
        function is one comment to every check and a return to the compiler, and a `//`
        comment ending in a backslash and a carriage return hides the next line from the
        checks only.  Library headers under third_party/ are not read here (the relocation
        verifier diffs them against the base when the tree carries them)."""
        P = self.problems["V0"]
        n = 0
        for p in sorted(set(clo) | {self.fn.path}):
            if p.startswith(("../", "/")) or not p.startswith(("src/", "include/")):
                continue
            text = self.tree.read(p)
            if text is None:
                continue
            n += 1
            for ln, why in reading_problems(text, cpp=True):
                P.append(f"{p}:{ln}: {why}, which the lint reads otherwise than the compiler")
        cm = self.tree.read(CMAKE)
        for ln, why in reading_problems(cm or "", cpp=False):
            P.append(f"{CMAKE}:{ln}: {why}, which the lint reads otherwise than CMake")
        self.info["V0"].append(f"{n} files of the translation unit read as the compiler reads them")

    def v0_overloads(self, clo):
        """No declaration the translation unit sees can take a spine call from its seam: every
        declaration of a spine seam's name in the include closure whose first parameter can
        bind `run` exactly (its bare type NetworkRun or an alias of it, a template parameter,
        `auto`) has the parameter types of the seam's one epoch-form definition.  A narrowed
        overload (`NetworkEpoch&` for `const NetworkEpoch&`) is an exact match that overload
        resolution prefers, so the spine would call it instead of the seam."""
        P = self.problems["V0"]
        decls = {p: file_decls(self.tree, p) for p in clo if not p.startswith(("../", "/"))}
        run_names, grew = {"NetworkRun"}, True
        while grew:
            grew = False
            for _, _, _, _, aliases in decls.values():
                for a, target in aliases.items():
                    if a not in run_names and _first_type([target]) in run_names:
                        run_names.add(a)
                        grew = True
        n = 0
        for sc in self.spine:
            leaf = re.split(r"\.|->|::", sc.callee)[-1]
            ds = [f for f in self.defs.find(leaf) if len(f.params) == sc.call.nargs]
            if len(ds) != 1:
                continue
            want = tuple(ptype(q) for q in ds[0].params)
            for p, (funcs, _, _, _, _) in sorted(decls.items()):
                for types, tmpl in funcs.get(leaf, ()):
                    n += 1
                    first = _first_type(types)
                    if types != want and (tmpl or first in run_names or first in ("auto", "")
                                          or first.startswith("decltype")):
                        P.append(f"{p}: declares {leaf}({', '.join(types)}){' (a template)' if tmpl else ''}, which "
                                 f"can take the spine call at {self.fn.path}:{self.fn.T.line(sc.call.tok)} from the "
                                 f"seam's epoch form {leaf}({', '.join(want)})")
        self.info["V0"].append(f"{n} declarations of spine seams' names in the translation unit")

    def v0_cmake(self, called):
        """The build compiles the epoch function's translation unit as it reads: CMakeLists.txt
        lists src/core/app/ApplicationNetworkEpoch.cpp once, as a target's source, and names it
        nowhere else (no per-source property: compile options, definitions or flags, the one
        exception being MSVC's section-count flag `COMPILE_FLAGS "/bigobj"`); no compile
        definition defines a name the epoch function calls (`networkIonoDiag=networkDdFrac`
        is a macro the compiler applies and the lint would not read); no option forces a header
        into the translation units (`/FI`, `-include`, precompiled headers)."""
        P = self.problems["V0"]
        text = self.tree.read(CMAKE)
        if text is None:
            self.info["V0"].append(f"no {CMAKE} in the tree")
            return
        cmds = cmake_commands(text)
        base = EPOCH_CPP.rsplit("/", 1)[-1]
        listed = 0
        for name, args, first in cmds:
            hits = [(a, ln) for a, ln in args if base in a]
            if not hits:
                continue
            if name in ("add_library", "add_executable", "target_sources"):
                for a, ln in hits:
                    if re.sub(r"^\$\{CMAKE_(?:CURRENT_)?SOURCE_DIR\}/", "", a) == EPOCH_CPP:
                        listed += 1
                    else:
                        P.append(f"{CMAKE}:{ln}: `{a[:70]}` in {name}(...) names the epoch function's translation "
                                 f"unit otherwise than as the source {EPOCH_CPP}")
                continue
            props = [a for a, _ in args]
            if name == "set_source_files_properties" and "PROPERTIES" in props and \
                    props[props.index("PROPERTIES") + 1:] == ["COMPILE_FLAGS", "/bigobj"]:
                continue
            P.append(f"{CMAKE}:{first}: {name}(...) names {EPOCH_CPP}: a per-source property compiles the epoch "
                     "function otherwise than the lint reads it")
        if listed != 1:
            P.append(f"{CMAKE}: {EPOCH_CPP} is listed {listed} times as a target's source (once expected)")
        for m, ln in sorted(cmake_macro_names(cmds).items()):
            if m in called:
                P.append(f"{CMAKE}:{ln}: a compile definition of `{m}`, a name the epoch function calls")
        for ln, a in cmake_forced_includes(cmds):
            P.append(f"{CMAKE}:{ln}: `{a[:70]}` forces a header into every translation unit of a target")
        self.info["V0"].append(f"{CMAKE}: {len(cmds)} commands, the translation unit listed {listed} time(s)")

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
        n_obs = sum(len(sc.observers) for sc in self.spine)
        self.info["V10"].append(f"{len(self.spine)} spine calls ({n_obs} observer rows read at their "
                                f"slot walk), {n} epoch reads checked against "
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



# ---------------------------------------------------------------------------------------------
# V12: the entity-table writers
# ---------------------------------------------------------------------------------------------
ENTITIES_CPP = "src/core/app/NetworkEntities.cpp"
TABLE_OF_TYPE = {"ConstellationTable": "constellations", "SatelliteTable": "sats",
                 "SatSignalTable": "sat_signals", "StationTable": "stations", "LosTable": "los",
                 "ArcTable": "arcs", "NetworkEntities": "all"}
TABLE_BIND = re.compile(r"(?m)^[ \t]*(?!const\b)(?:net::)?(\w+)[ \t]*&[ \t]*\w+[ \t]*=[ \t]*"
                        r"(?:n?run\.)?ctx\.entities(?:\.(\w+))?[ \t]*;")


def enclosing_function(text, pos):
    """The name of the function whose body holds @p pos (namespace braces are transparent)."""
    stack = []   # (open position, is_function, name)
    i = 0
    while i < pos:
        c = text[i]
        if c == "{":
            head = text[max(0, i - 4000):i]
            m = re.search(r"\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[\w:<>]+\s*)?$", head)
            name = None
            if m and not re.search(r"\b(if|for|while|switch|catch)\s*\([^;{}]*\)\s*$", head):
                j = m.start()   # the closing parenthesis of the parameter list
                d, k = 0, j
                while k >= 0:
                    if head[k] == ")":
                        d += 1
                    elif head[k] == "(":
                        d -= 1
                        if d == 0:
                            break
                    k -= 1
                nm = re.search(r"([A-Za-z_]\w*)\s*$", head[:k])
                if nm and nm.group(1) not in ("if", "for", "while", "switch", "catch", "return"):
                    name = nm.group(1)
            stack.append((i, name))
        elif c == "}" and stack:
            stack.pop()
        i += 1
    for _, name in stack:
        if name is not None:
            return name
    return None


def entity_writer_bindings(tree):
    """{(function, home): set of tables} over src/core: every non-const entity-table binding."""
    got = collections.defaultdict(set)
    for path in tree.files("src/core"):
        if not path.endswith(".cpp"):
            continue
        raw = tree.read(path)
        if raw is None or "ctx.entities" not in raw:
            continue
        text = strip_comments(raw)
        for m in TABLE_BIND.finditer(text):
            typ, member = m.group(1), m.group(2)
            table = member if member else TABLE_OF_TYPE.get(typ, "all" if typ == "auto" else "?")
            fn = enclosing_function(text, m.start())
            got[(fn, path[len("src/core/"):])].add(table)
    return got


def entity_writer_rows(tree):
    """{(seam, home): set of tables} from kEntityWriters in NetworkEntities.cpp."""
    text = tree.read(ENTITIES_CPP) or ""
    k = text.find("kEntityWriters = {")
    if k < 0:
        return None
    body = text[k:text.index("};", k)]
    return {(s, h): set(t.split()) for s, h, t in re.findall(r'\{"(\w+)",\s*"([^"]+)",\s*"([^"]*)"\}', body)}


def check_v12(tree):
    """(problems, info): every non-const entity-table binding has its kEntityWriters row, and
    every row is supported by such a binding."""
    rows = entity_writer_rows(tree)
    if rows is None:
        return [f"{ENTITIES_CPP}: kEntityWriters is not defined"], ""
    got = entity_writer_bindings(tree)
    problems = []
    for key, tables in sorted(got.items(), key=lambda kv: (kv[0][1], kv[0][0] or "")):
        want = rows.get(key)
        if want is None:
            problems.append(f"src/core/{key[1]}: {key[0]} binds {' '.join(sorted(tables))} for writing "
                            "and has no kEntityWriters row")
        elif want != tables:
            problems.append(f"src/core/{key[1]}: {key[0]} binds {' '.join(sorted(tables))} for writing; "
                            f"its kEntityWriters row says {' '.join(sorted(want))}")
    for key in sorted(set(rows) - set(got)):
        problems.append(f"kEntityWriters row {key[0]} ({key[1]}): no non-const binding supports it")
    return problems, f"{len(got)} functions bind entity tables for writing, {len(rows)} kEntityWriters rows"


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
    lint.v0()
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
    v12_problems, v12_info = check_v12(tree)
    lint.problems["V12"].extend(v12_problems)
    lint.info["V12"].append(v12_info)
    bad = 0
    for chk in ("V0", "V4", "V10", "V11", "V12"):
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

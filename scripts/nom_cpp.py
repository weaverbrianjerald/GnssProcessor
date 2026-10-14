#!/usr/bin/env python3
"""C++ reading machinery for the card NOM3 step EP1 tools (lexer, statements, scopes, bindings).

    python scripts/nom_cpp.py --selftest [--base REV] [--calltable PATH] [--expect KEY=N,...]

Python 3 standard library only; no compiler is involved.  Every frozen file is read from git
(`git cat-file --batch` at a revision), never from the working tree, so the tools that import
this module can be re-run after the base moves.  What the module provides:

  lex(text)             the token lexer: identifiers, numbers (digit separators included),
                        string, character and raw-string literals, operators (longest match),
                        one token per preprocessor directive; comments are dropped and every
                        token keeps its line and its character span in the source.
  Tokens                a lexed file with its bracket-match table and three text views of a
                        token range: src() verbatim (comments inside kept), norm() the call
                        table's normalized text, join() minimal spacing.
  stmt_end()            the statement splitter (blocks, if/else, for, while, do, switch,
                        try/catch, labels, expression and declaration statements).
  parse_declaration()   the declaration parser for a statement or a condition: every
                        declarator with its name, reference / pointer / const form and
                        initializer span (structured bindings, multi-declarators, enum and
                        alias types included).
  split_params(), Param the parameter-list parser: kinds W (non-const reference), W* (pointer
                        to non-const), R (const reference), R* (pointer to const), V (by value),
                        F (std::function), the declarator text kept verbatim, the default
                        argument split off.
  FileIndex             every function declaration and definition of one file (namespaces,
                        classes and their data members, out-of-class definitions,
                        constructor initializer lists).
  Scopes                the scope walker over one function body: blocks, the scopes of if /
                        for / while / switch conditions, catch clauses, lambdas with their
                        parameters and init-captures; lookup(name, pos) searches the innermost
                        scope first and sees only declarations made before pos.
  calls_in()            call extraction with each argument's token span (the inventory's rule:
                        an identifier followed by `(`, qualified backwards over `.`, `->`, `::`).
  Resolver              an argument expression to a binding: the head name's declaration by the
                        scope walk; a reference declaration replaced by its initializer and
                        resolved again; a by-value declaration copy-initialized from a binding
                        giving Copy(...); a declaration in the caller's stop set (the
                        realization tables) ending the walk.  Results are Ref (root, member
                        chain, at most one & or *), Copy, Cond, Call, Lit, Expr or Opaque.
  GitTree               the frozen revision: rev-parse, ls-tree, cat-file --batch.
  Driver                the shape of Application::runNetwork at a revision: its body, the
                        per-epoch lambda processSyncedEpoch, the names it captures from the
                        driver, and the calls of the call table.

What the self-test proves (exit 0 only when every line passes):
  1. The machinery reproduces the counts of the EP1 design's inventory on the frozen driver
     (its call table at the EP0 base, made by the inventory's own lexer and scripts): the
     lambda captures 62 driver names plus LoopSignal; it makes 99 calls of the call table's
     kinds, 79 of them network* calls carrying 753 arguments.  Each count is printed with its
     expected value.  With --calltable PATH (that call table, a Markdown file), the call
     sequence (callee, argument count) is also compared with it row by row.  At a later base
     whose driver changed, --expect KEY=N,... states the counts expected there.
  2. The scope walker resolves the shadowing facts the EP1 binding model depends on (card NOM3
     EP1 specification, section 5.4): the `dt` that carry_driver.beginEpoch reads is the
     DD-carry predict's block-local TOW-only step, while the `dt` networkSolveGlue reads is the
     lambda's week-aware dt; `x_s` at networkInstallCarry follows its reference to the solve
     result's member; `stations` follows its reference to the setup's member; the argument
     networkPassASlipScan_ binds to its `carry_slipped_ambs` parameter is the raw-subset local.
"""
import argparse
import collections
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

# ---------------------------------------------------------------------------------------------
# Lexer
# ---------------------------------------------------------------------------------------------
_TOKEN = re.compile(r"""
 (?P<nl>\n)
|(?P<ws>(?:[ \t\r\f\v]|\\\r?\n)+)
|(?P<lc>//(?:\\\r?\n|[^\n])*)
|(?P<bc>/\*.*?(?:\*/|\Z))
|(?P<raw>(?:u8|u|U|L)?R"(?P<delim>[^()\\\s]{0,16})\(.*?\)(?P=delim)")
|(?P<str>(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*"?)
|(?P<chr>(?:u8|u|U|L)?'(?:[^'\\\n]|\\.)*'?)
|(?P<num>\.?\d(?:[eEpP][+-]|'[0-9A-Za-z_]|[0-9A-Za-z_.])*)
|(?P<id>[A-Za-z_]\w*)
|(?P<op>>>=|<<=|->\*|\.\.\.|<=>|->|::|&&|\|\||==|!=|<=|>=|\+\+|--|\+=|-=|\*=|/=|%=|&=|\|=|\^=|<<|>>|\.\*|.)
""", re.X | re.S)
_PP = re.compile(r"#(?:\\\r?\n|/\*.*?(?:\*/|\Z)|[^\n])*", re.S)

Tok = collections.namedtuple("Tok", "kind text line pos end")


def lex(text):
    """Tokens of @p text: Tok(kind, text, line, pos, end); kind is id, num, str, chr, op or pp.
    Comments and whitespace are dropped; a preprocessor directive (a `#` first on its line) is
    one pp token running to the end of the line, continuation lines included."""
    toks = []
    line, pos, n = 1, 0, len(text)
    at_line_start = True
    match = _TOKEN.match
    while pos < n:
        m = match(text, pos)
        g = m.lastgroup
        s = m.group()
        if g == "nl":
            line += 1
            at_line_start = True
        elif g in ("ws", "lc", "bc"):
            line += s.count("\n")
        elif g == "op" and s == "#" and at_line_start:
            m = _PP.match(text, pos)
            s = m.group()
            toks.append(Tok("pp", s, line, pos, m.end()))
            line += s.count("\n")
            at_line_start = False
        else:
            kind = "str" if g == "raw" else g
            toks.append(Tok(kind, s, line, pos, m.end()))
            line += s.count("\n")
            at_line_start = False
        pos = m.end()
    return toks


OPEN = {"(": ")", "[": "]", "{": "}"}
CLOSE = {v: k for k, v in OPEN.items()}

KEYWORDS = frozenset("""
alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t
char32_t class co_await co_return co_yield compl concept const consteval constexpr constinit
const_cast continue decltype default delete do double dynamic_cast else enum explicit export
extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq
nullptr operator or or_eq private protected public register reinterpret_cast requires return
short signed sizeof static static_assert static_cast struct switch template this thread_local
throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while
xor xor_eq
""".split())
BUILTIN_TYPES = frozenset("void bool char wchar_t char8_t char16_t char32_t short int long float "
                          "double signed unsigned".split())
DECL_SPECS = frozenset("const volatile static constexpr consteval constinit inline thread_local "
                       "mutable extern register typename".split())


class Tokens:
    """A lexed source file: the tokens, the bracket-match table, and text views of a range."""

    def __init__(self, text, path=""):
        self.source = text
        self.path = path
        self.toks = lex(text)
        self.n = len(self.toks)
        self.match = [-1] * self.n
        stack = []
        for i, t in enumerate(self.toks):
            if t.kind != "op":
                continue
            if t.text in OPEN:
                stack.append(i)
            elif t.text in CLOSE:
                # Recover from a stray closer (unbalanced preprocessor branches): pop to the
                # nearest opener of the same kind, leaving the openers above it unmatched.
                k = len(stack) - 1
                while k >= 0 and self.toks[stack[k]].text != CLOSE[t.text]:
                    k -= 1
                if k >= 0:
                    j = stack[k]
                    del stack[k:]
                    self.match[j] = i
                    self.match[i] = j

    def __len__(self):
        return self.n

    def __getitem__(self, i):
        return self.toks[i]

    def t(self, i):
        return self.toks[i].text if 0 <= i < self.n else ""

    def k(self, i):
        return self.toks[i].kind if 0 <= i < self.n else ""

    def line(self, i):
        return self.toks[i].line if 0 <= i < self.n else 0

    def close(self, i):
        """Index of the bracket matching the opener at @p i (raises on an unmatched one)."""
        j = self.match[i]
        if j < 0:
            raise ValueError(f"{self.path}:{self.line(i)}: unmatched {self.t(i)!r}")
        return j

    def src(self, a, b):
        """The verbatim source of tokens [a, b): comments and spacing inside kept."""
        if a >= b:
            return ""
        return self.source[self.toks[a].pos:self.toks[b - 1].end]

    def join(self, a, b):
        """Tokens [a, b) with a space only where two word tokens meet."""
        out, prev_word = [], False
        for i in range(a, b):
            t = self.toks[i]
            word = t.kind in ("id", "num")
            if word and prev_word:
                out.append(" ")
            out.append(t.text)
            prev_word = word
        return "".join(out)

    def norm(self, a, b):
        """The inventory call table's normalized text of tokens [a, b): identifiers padded,
        one double space folded, then the spaces around . , ( ) & * : < > ? ! [ ] - removed
        (so `a != b` reads `a!= b`; compare such text with its spaces removed)."""
        s = "".join(" " + self.toks[i].text + " " if self.toks[i].kind == "id" else self.toks[i].text
                    for i in range(a, b)).replace("  ", " ").strip()
        return re.sub(r"\s*([.,()&*:<>?!\[\]-])\s*", r"\1", s)


def norm_text(text):
    """norm() of a source string (used to compare a stored expression with a frozen one)."""
    T = Tokens(text)
    return T.norm(0, len(T))


def match_angle(T, i, e):
    """@p i at `<`: index of the `>` (or `>>`) closing it before @p e, or None when the `<`
    does not open a template argument list (a comparison, a shift)."""
    depth, j = 0, i
    while j < e:
        x, kind = T.t(j), T.k(j)
        if kind == "op":
            if x == "<":
                depth += 1
            elif x == ">":
                depth -= 1
            elif x == ">>":
                depth -= 2
            elif x in OPEN:
                j = T.match[j]
                if j < 0:
                    return None
            elif x in CLOSE or x in (";", "&&", "||", ">=", ">>=", "<=", "=", "?"):
                return None
            if depth <= 0:
                return j
        j += 1
    return None


def top_level(T, a, b):
    """Indices in [a, b) at bracket depth 0 relative to a."""
    j = a
    while j < b:
        yield j
        if T.k(j) == "op" and T.t(j) in OPEN and T.match[j] > j:
            j = T.match[j]
        j += 1


def strip_parens(T, a, b):
    while b - a >= 2 and T.t(a) == "(" and T.match[a] == b - 1:
        a, b = a + 1, b - 1
    return a, b


# ---------------------------------------------------------------------------------------------
# Statements
# ---------------------------------------------------------------------------------------------
def stmt_end(T, i, limit=None):
    """Index one past the statement that starts at @p i."""
    limit = T.n if limit is None else limit
    t, kind = T.t(i), T.k(i)
    if kind == "pp":
        return i + 1
    if t == "{":
        return T.close(i) + 1
    if t == ";":
        return i + 1
    if t == "if":
        j = i + 1
        if T.t(j) in ("constexpr", "consteval", "!"):
            j += 1
        j = T.close(j) + 1
        j = stmt_end(T, j, limit)
        if j < limit and T.t(j) == "else":
            j = stmt_end(T, j + 1, limit)
        return j
    if t in ("for", "while", "switch"):
        return stmt_end(T, T.close(i + 1) + 1, limit)
    if t == "do":
        j = stmt_end(T, i + 1, limit)
        if T.t(j) == "while":
            j = T.close(j + 1) + 1
        return j + 1 if T.t(j) == ";" else j
    if t == "try":
        j = T.close(i + 1) + 1
        while T.t(j) == "catch":
            j = T.close(T.close(j + 1) + 1) + 1
        return j
    if t in ("case", "default"):
        j = i + 1
        while j < limit and not (T.t(j) == ":" and T.k(j) == "op"):
            j = T.close(j) + 1 if T.t(j) in OPEN else j + 1
        return j + 1
    if kind == "id" and t not in KEYWORDS and T.t(i + 1) == ":":
        return i + 2   # a label
    j = i
    while j < limit:
        x = T.t(j)
        if T.k(j) == "op":
            if x in OPEN:
                j = T.close(j) + 1
                continue
            if x == ";":
                return j + 1
        j += 1
    return limit


def statements(T, a, b):
    """The statements of the token range [a, b) as (start, end) pairs."""
    out, i = [], a
    while i < b:
        j = stmt_end(T, i, b)
        out.append((i, j))
        i = j
    return out


# ---------------------------------------------------------------------------------------------
# Declarations
# ---------------------------------------------------------------------------------------------
class Decl:
    """One declared name.  kind: var, closure (a variable initialized by a lambda), param,
    capture (an init-capture), binding (a structured binding), type (enum, class, alias)."""
    __slots__ = ("name", "tok", "kind", "scope", "is_ref", "is_ptr", "is_const", "init",
                 "init_kind", "type_span", "stmt", "line", "lambda_scope", "ops")

    def __init__(self, name, tok, kind, line, **kw):
        self.name, self.tok, self.kind, self.line = name, tok, kind, line
        self.scope = None
        self.is_ref = kw.get("is_ref", False)
        self.is_ptr = kw.get("is_ptr", False)
        self.is_const = kw.get("is_const", False)
        self.init = kw.get("init")              # (a, b) token span of the initializer
        self.init_kind = kw.get("init_kind")    # '=', '(', '{' or None
        self.type_span = kw.get("type_span")    # (a, b) token span of the type, specifiers included
        self.stmt = kw.get("stmt")
        self.ops = kw.get("ops", "")
        self.lambda_scope = None

    def __repr__(self):
        return f"Decl({self.name}@{self.line} {self.kind}{' &' if self.is_ref else ''})"


def parse_type(T, j, e):
    """Specifiers and a type starting at @p j: (index after the type, const seen) or None."""
    is_const = False
    while j < e and (T.t(j) in DECL_SPECS or (T.t(j) == "[" and T.t(j + 1) == "[")):
        if T.t(j) == "[":
            j = T.close(j) + 1
            continue
        if T.t(j) == "const":
            is_const = True
        j += 1
    if j >= e:
        return None
    t = T.t(j)
    if t in ("struct", "class", "union", "enum"):
        j += 1
        t = T.t(j)
    if t == "auto":
        j += 1
    elif t == "decltype":
        if T.t(j + 1) != "(":
            return None
        j = T.close(j + 1) + 1
    elif t in BUILTIN_TYPES:
        while j < e and T.t(j) in BUILTIN_TYPES:
            j += 1
    elif (T.k(j) == "id" and t not in KEYWORDS) or t == "::":
        if t == "::":
            j += 1
        if T.k(j) != "id" or T.t(j) in KEYWORDS:
            return None
        j += 1
        while j < e:
            if T.t(j) == "<":
                m = match_angle(T, j, e)
                if m is None:
                    break
                j = m + 1
                continue
            if T.t(j) == "::" and T.k(j + 1) == "id":
                j += 2
                continue
            if T.t(j) == "::" and T.t(j + 1) == "template" and T.k(j + 2) == "id":
                j += 3
                continue
            break
    else:
        return None
    while j < e and T.t(j) in ("const", "volatile"):
        if T.t(j) == "const":
            is_const = True
        j += 1
    return j, is_const


def _init_end(T, j, e):
    """End of a copy-initializer starting at @p j: the first top-level `,`, `;` or `)` before
    @p e.  A `<` right after a name that opens a template argument list followed by `(`, `{`
    or `::` is skipped, so `std::map<int, int>{}` stays one initializer."""
    while j < e:
        x = T.t(j)
        if T.k(j) == "op":
            if x in OPEN:
                j = T.close(j) + 1
                continue
            if x in (",", ";") or x in CLOSE:
                return j
            if x == "<" and T.k(j - 1) == "id":
                m = match_angle(T, j, e)
                if m is not None and T.t(m + 1) in ("(", "{", "::"):
                    j = m + 1
                    continue
        j += 1
    return e


def parse_declaration(T, i, e, context="stmt"):
    """The declarators of a declaration statement (or condition) in [i, e), or None.

    @p context: 'stmt' (a statement), 'cond' (an if / while / switch condition, which must
    have an initializer), 'range' (the declaration of a range-for, ended by `:`).  A
    reference declarator must have an initializer except in 'range'; that rule keeps an
    expression such as `a && b` from reading as a declaration of b."""
    t = T.t(i)
    if T.k(i) != "id" and t not in ("::", "["):
        return None
    if t in ("enum",):
        j = i + 1
        if T.t(j) in ("class", "struct"):
            j += 1
        if T.k(j) == "id":
            return [Decl(T.t(j), j, "type", T.line(j), stmt=(i, e))]
        return None
    if t in ("struct", "class", "union") and T.k(i + 1) == "id":
        k = i + 2
        while k < e and T.t(k) not in ("{", ";", "("):
            k += 1
        if T.t(k) in ("{", ";"):
            return [Decl(T.t(i + 1), i + 1, "type", T.line(i + 1), stmt=(i, e))]
    if t == "using":
        if T.k(i + 1) == "id" and T.t(i + 2) == "=":
            return [Decl(T.t(i + 1), i + 1, "type", T.line(i + 1), stmt=(i, e))]
        return None
    if t == "typedef":
        k = e - 1
        while k > i and T.t(k) in (";",):
            k -= 1
        if T.k(k) == "id":
            return [Decl(T.t(k), k, "type", T.line(k), stmt=(i, e))]
        return None
    if t in KEYWORDS and t not in DECL_SPECS and t not in BUILTIN_TYPES and t != "auto" \
            and t != "decltype":
        return None
    r = parse_type(T, i, e)
    if r is None:
        return None
    j, type_const = r
    type_end = j
    decls = []
    while True:
        ops, k, ptr_const = [], j, False
        while k < e and T.t(k) in ("*", "&", "&&", "const", "volatile"):
            if T.t(k) in ("*", "&", "&&"):
                ops.append(T.t(k))
            elif ops and ops[-1] == "*" and T.t(k) == "const":
                ptr_const = True
            k += 1
        if T.t(k) == "[" and T.t(type_end - 1) == "auto":
            cl = T.close(k)
            names = [q for q in range(k + 1, cl) if T.k(q) == "id"]
            k = cl + 1
            init, ik = None, None
            if T.t(k) == "=":
                ie = _init_end(T, k + 1, e)
                init, ik, k = (k + 1, ie), "=", ie
            elif T.t(k) in ("{", "("):
                cl2 = T.close(k)
                init, ik, k = (k + 1, cl2), T.t(k), cl2 + 1
            for q in names:
                decls.append(Decl(T.t(q), q, "binding", T.line(q), is_ref=bool(ops),
                                  init=init, init_kind=ik, type_span=(i, type_end),
                                  stmt=(i, e), ops="".join(ops)))
        elif T.k(k) == "id" and T.t(k) not in KEYWORDS:
            name_tok = k
            k += 1
            if T.t(k) == "::":
                return None
            while T.t(k) == "[":
                k = T.close(k) + 1
            init, ik = None, None
            if T.t(k) == "=":
                ie = _init_end(T, k + 1, e)
                init, ik, k = (k + 1, ie), "=", ie
            elif T.t(k) == "(":
                cl = T.close(k)
                init, ik, k = (k + 1, cl), "(", cl + 1
            elif T.t(k) == "{":
                cl = T.close(k)
                init, ik, k = (k + 1, cl), "{", cl + 1
            is_ref = bool(ops) and ops[0] in ("&", "&&")
            is_ptr = bool(ops) and ops[0] == "*"
            if context == "cond" and init is None:
                return None
            if is_ref and init is None and context != "range":
                return None
            d = Decl(T.t(name_tok), name_tok, "var", T.line(name_tok), is_ref=is_ref,
                     is_ptr=is_ptr, is_const=(type_const if not is_ptr else ptr_const),
                     init=init, init_kind=ik, type_span=(i, type_end), stmt=(i, e),
                     ops="".join(ops))
            if ik == "=" and T.t(init[0]) == "[" and _lambda_at(T, init[0], init[1]):
                d.kind = "closure"
            decls.append(d)
        else:
            return None
        if k >= e or T.t(k) in (";", ")"):
            break
        if T.t(k) == ":" and context == "range":
            break
        if T.t(k) == ",":
            j = k + 1
            continue
        return None
    return decls or None


# ---------------------------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------------------------
class Param:
    """One function parameter.  kind: W, W*, R, R*, V or F (the inventory's classification: the
    first & / && / * outside template arguments decides, const before it makes it R or R*);
    text: the declarator verbatim with the default argument removed;
    code: the same tokens, minimal spacing."""
    __slots__ = ("kind", "name", "has_default", "text", "code", "default", "span", "name_tok")

    def __init__(self, kind, name, has_default, text, code, default, span, name_tok):
        self.kind, self.name, self.has_default = kind, name, has_default
        self.text, self.code, self.default = text, code, default
        self.span, self.name_tok = span, name_tok

    def __repr__(self):
        return f"Param({self.kind} {self.name})"


def split_params(T, i):
    """@p i at `(`: the parameter spans, split at commas outside (), [], {} and <>."""
    end = T.close(i)
    out, cur_start, d, a = [], i + 1, 0, 0
    for j in range(i + 1, end):
        v = T.t(j)
        if T.k(j) != "op":
            continue
        if v in "([{":
            d += 1
        elif v in ")]}":
            d -= 1
        elif v == "<" and d == 0:
            a += 1
        elif v == ">" and d == 0:
            a = max(0, a - 1)
        elif v == ">>" and d == 0:
            a = max(0, a - 2)
        elif v == "," and d == 0 and a == 0:
            out.append((cur_start, j))
            cur_start = j + 1
    if cur_start < end:
        out.append((cur_start, end))
    return out, end


def classify_param(T, s, e):
    vals = [T.t(q) for q in range(s, e)]
    has_def = "=" in vals
    cut = s + vals.index("=") if has_def else e
    vals = vals[:cut - s]
    a, refs, seen_const = 0, [], False
    for v in vals:
        if v == "<":
            a += 1
            continue
        if v == ">":
            a = max(0, a - 1)
            continue
        if v == ">>":
            a = max(0, a - 2)
            continue
        if a:
            continue
        if v == "const":
            seen_const = True
        if v in ("&", "&&", "*"):
            refs.append((v, seen_const))
    name, name_tok = "", None
    if len(vals) > 1 and T.k(cut - 1) == "id" and vals[-1] not in KEYWORDS and T.t(cut - 2) != "::":
        name, name_tok = vals[-1], cut - 1
    if "function" in vals and "std" in vals:
        kind = "F"
    elif not refs:
        kind = "V"
    elif refs[0][0] in ("&", "&&"):
        kind = "R" if refs[0][1] else "W"
    else:
        kind = "R*" if refs[0][1] else "W*"
    default = T.src(cut + 1, e) if has_def else ""
    return Param(kind, name, has_def, T.src(s, cut), T.join(s, cut), default, (s, e), name_tok)


def parse_params(T, i):
    spans, end = split_params(T, i)
    ps = [classify_param(T, s, e) for s, e in spans]
    if len(ps) == 1 and ps[0].code == "void":
        ps = []
    return ps, end


# ---------------------------------------------------------------------------------------------
# File index: function declarations and definitions
# ---------------------------------------------------------------------------------------------
class Func:
    """A function declaration or definition found in a file.  start: the token that begins
    the declaration (its first specifier or return-type token)."""
    __slots__ = ("name", "qual", "cls", "params", "ret", "line", "name_tok", "paren", "body",
                 "is_def", "path", "start")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))

    def __repr__(self):
        return f"Func({self.qual}@{self.line}{' def' if self.is_def else ''})"


NOT_FUNCS = frozenset(("decltype", "sizeof", "alignof", "noexcept", "static_assert", "alignas",
                       "__declspec", "__attribute__", "if", "while", "for", "switch", "return"))


class FileIndex:
    """Every function declaration and definition in one lexed file, with the data members of
    each class.  funcs: list of Func; members: class name -> {data-member name: line}."""

    def __init__(self, T):
        self.T = T
        self.funcs = []
        self.members = collections.defaultdict(dict)
        self._scan(0, T.n, [])

    def find(self, name, cls=None, defs=None):
        out = []
        for f in self.funcs:
            if f.name != name:
                continue
            if cls is not None and (f.cls or "") != cls:
                continue
            if defs is not None and bool(f.is_def) != defs:
                continue
            out.append(f)
        return out

    def _skip_to_semicolon(self, i, b):
        T = self.T
        while i < b:
            x = T.t(i)
            if T.k(i) == "op" and x in OPEN:
                i = T.close(i) + 1
                continue
            if x == ";":
                return i + 1
            i += 1
        return b

    def _scan(self, a, b, cls_stack):
        T = self.T
        i = a
        while i < b:
            t, kind = T.t(i), T.k(i)
            if kind == "pp" or t == ";":
                i += 1
                continue
            if t == "namespace":
                k = i + 1
                while k < b and T.t(k) not in ("{", ";", "="):
                    k += 1
                if T.t(k) == "{":
                    self._scan(k + 1, T.close(k), cls_stack)
                    i = T.close(k) + 1
                else:
                    i = self._skip_to_semicolon(k, b)
                continue
            if t == "extern" and T.k(i + 1) == "str":
                if T.t(i + 2) == "{":
                    self._scan(i + 3, T.close(i + 2), cls_stack)
                    i = T.close(i + 2) + 1
                else:
                    i += 2
                continue
            if t == "template" and T.t(i + 1) == "<":
                m = match_angle(T, i + 1, b)
                i = (m + 1) if m is not None else i + 1
                continue
            if t in ("public", "private", "protected") and T.t(i + 1) == ":":
                i += 2
                continue
            if t in ("using", "typedef", "static_assert", "friend"):
                i = self._skip_to_semicolon(i, b)
                continue
            if t == "enum":
                i = self._skip_to_semicolon(i, b)
                continue
            if t in ("class", "struct", "union"):
                k = i + 1
                while k < b and T.t(k) not in ("{", ";", "(", "="):
                    if T.t(k) == "<":
                        m = match_angle(T, k, b)
                        k = (m + 1) if m is not None else k + 1
                        continue
                    k += 1
                if T.t(k) == "{":
                    name = T.t(i + 1) if T.k(i + 1) == "id" else ""
                    self._scan(k + 1, T.close(k), cls_stack + [name])
                    i = self._skip_to_semicolon(T.close(k) + 1, b)
                    continue
                if T.t(k) == ";":
                    i = k + 1
                    continue
            # A declaration: the first top-level `(` after a name, before `;`, `{` or `=`.
            k, fn = i, None
            while k < b:
                x = T.t(k)
                if x == "(":
                    if T.k(k - 1) == "id" and T.t(k - 1) not in KEYWORDS and k > i:
                        fn = k
                        break
                    k = T.close(k) + 1
                    continue
                if x == "<" and T.k(k - 1) == "id":
                    m = match_angle(T, k, b)
                    if m is not None:
                        k = m + 1
                        continue
                if x == "[":
                    k = T.close(k) + 1
                    continue
                if x in (";", "{", "=", "}"):
                    break
                k += 1
            if fn is None:
                ds = parse_declaration(T, i, b)
                if ds and cls_stack:
                    for d in ds:
                        if d.kind in ("var", "closure"):
                            self.members[cls_stack[-1]][d.name] = d.line
                if T.t(k) == "{":
                    k = T.close(k) + 1
                    i = k + 1 if T.t(k) == ";" else k
                else:
                    i = self._skip_to_semicolon(k, b)
                continue
            pc = T.close(fn)
            k = pc + 1
            while k < b:
                x = T.t(k)
                if x in ("const", "override", "final", "volatile", "&", "&&", "mutable"):
                    k += 1
                elif x == "noexcept":
                    k = T.close(k + 1) + 1 if T.t(k + 1) == "(" else k + 1
                elif x == "[" and T.t(k + 1) == "[":
                    k = T.close(k) + 1
                else:
                    break
            if T.t(k) == "->":
                while k < b and T.t(k) not in ("{", ";", "="):
                    if T.t(k) == "<":
                        m = match_angle(T, k, b)
                        k = (m + 1) if m is not None else k + 1
                        continue
                    k = T.close(k) + 1 if T.t(k) in OPEN else k + 1
            if T.t(k) == ":":
                # Constructor initializer list: `name(...)` or `name{...}`, comma separated;
                # a `{` that does not follow a name (or a template's `>`) opens the body.
                k += 1
                while k < b:
                    while k < b and T.t(k) not in ("(", "{", ";"):
                        if T.t(k) == "<":
                            m = match_angle(T, k, b)
                            k = (m + 1) if m is not None else k + 1
                            continue
                        k += 1
                    if T.t(k) == "(":
                        k = T.close(k) + 1
                    elif T.t(k) == "{" and (T.k(k - 1) == "id" or T.t(k - 1) == ">"):
                        k = T.close(k) + 1
                    if T.t(k) == ",":
                        k += 1
                        continue
                    break
            name_tok = fn - 1
            q = name_tok
            while T.t(q - 1) == "::" and T.k(q - 2) == "id":
                q -= 2
            qual = T.join(q, fn)
            parts = qual.split("::")
            cls = cls_stack[-1] if cls_stack else (parts[-2] if len(parts) > 1 else "")
            try:
                params, _ = parse_params(T, fn)
            except ValueError:
                params = None
            f = Func(name=T.t(name_tok), qual=qual, cls=cls, params=params,
                     ret=T.join(i, q), line=T.line(name_tok), name_tok=name_tok,
                     paren=(fn, pc), body=None, is_def=False, path=T.path, start=i)
            if T.t(k) == "{":
                f.body = (k, T.close(k))
                f.is_def = True
                self.funcs.append(f)
                i = T.close(k) + 1
                continue
            self.funcs.append(f)
            i = self._skip_to_semicolon(k, b)


# ---------------------------------------------------------------------------------------------
# Scopes
# ---------------------------------------------------------------------------------------------
class Scope:
    __slots__ = ("kind", "start", "end", "parent", "decls", "children", "line", "depth")

    def __init__(self, kind, start, end, parent, line):
        self.kind, self.start, self.end, self.parent, self.line = kind, start, end, parent, line
        self.decls, self.children = [], []
        self.depth = 0 if parent is None else parent.depth + 1

    def __repr__(self):
        return f"Scope({self.kind}@{self.line} d{self.depth})"


def _lambda_at(T, i, e):
    """@p i at `[`: (captures close, params `(` or None, body `{`) when a lambda starts here."""
    if T.t(i) != "[" or T.t(i + 1) == "[":
        return None
    p = T.t(i - 1) if i > 0 else ""
    pk = T.k(i - 1) if i > 0 else ""
    if pk in ("num", "str", "chr") or p in (")", "]") or (pk == "id" and p not in KEYWORDS):
        return None
    cc = T.match[i]
    if cc < 0:
        return None
    k = cc + 1
    if T.t(k) == "<":
        m = match_angle(T, k, e)
        if m is None:
            return None
        k = m + 1
    po = None
    if T.t(k) == "(":
        po = k
        k = T.close(k) + 1
    while k < e:
        x = T.t(k)
        if x in ("mutable", "constexpr", "consteval", "static"):
            k += 1
        elif x == "noexcept":
            k = T.close(k + 1) + 1 if T.t(k + 1) == "(" else k + 1
        elif x == "[" and T.t(k + 1) == "[":
            k = T.close(k) + 1
        elif x == "->":
            k += 1
            while k < e and T.t(k) != "{":
                if T.t(k) == "<":
                    m = match_angle(T, k, e)
                    k = (m + 1) if m is not None else k + 1
                elif T.t(k) in ("(", "["):
                    k = T.close(k) + 1
                else:
                    k += 1
            break
        else:
            break
    if T.t(k) != "{":
        return None
    return cc, po, k


class Scopes:
    """The scope tree of one function body (or of a lambda's body)."""

    def __init__(self, T, body_open, body_close, params=(), kind="function"):
        self.T = T
        self.root = Scope(kind, body_open, body_close, None, T.line(body_open))
        self.all = [self.root]
        for p in params or ():
            if p.name:
                d = Decl(p.name, p.name_tok, "param", T.line(p.name_tok),
                         is_ref=p.kind in ("W", "R"), is_ptr=p.kind in ("W*", "R*"),
                         is_const=p.kind in ("R", "R*"))
                self._add(self.root, d)
        self._walk(self.root, body_open + 1, body_close)

    # -- construction --
    def _child(self, parent, kind, start, end):
        s = Scope(kind, start, end, parent, self.T.line(start))
        parent.children.append(s)
        self.all.append(s)
        return s

    def _add(self, scope, d):
        d.scope = scope
        scope.decls.append(d)

    def _walk(self, scope, a, b):
        i = a
        while i < b:
            e = stmt_end(self.T, i, b)
            self._stmt(scope, i, e)
            i = e

    def _sub(self, scope, i, e):
        """A substatement: a compound opens its own block, anything else an implicit one."""
        if i >= e:
            return
        if self.T.t(i) == "{":
            self._stmt(scope, i, e)
        else:
            s = self._child(scope, "stmt", i, e - 1)
            self._stmt(s, i, e)

    def _decl_or_expr(self, scope, a, b, context="stmt"):
        T = self.T
        ds = parse_declaration(T, a, b, context) if a < b else None
        if ds:
            for d in ds:
                if d.init:
                    self._expr(scope, d.init[0], d.init[1], owner=d)
                self._add(scope, d)
            return True
        self._expr(scope, a, b)
        return False

    def _stmt(self, scope, i, e):
        T = self.T
        t = T.t(i)
        if T.k(i) == "pp" or t == ";":
            return
        if t == "{":
            c = T.close(i)
            s = self._child(scope, "block", i, c)
            self._walk(s, i + 1, c)
            return
        if t in ("if", "switch"):
            j = i + 1
            if T.t(j) in ("constexpr", "consteval", "!"):
                j += 1
            po, pc = j, T.close(j)
            s = self._child(scope, t, i, e - 1)
            semi = next((q for q in top_level(T, po + 1, pc) if T.t(q) == ";"), None)
            cs = po + 1
            if semi is not None:
                self._decl_or_expr(s, po + 1, semi + 1)
                cs = semi + 1
            self._decl_or_expr(s, cs, pc, "cond")
            then_s = pc + 1
            then_e = stmt_end(T, then_s, e)
            self._sub(s, then_s, then_e)
            if then_e < e and T.t(then_e) == "else":
                self._sub(s, then_e + 1, e)
            return
        if t == "for":
            po, pc = i + 1, T.close(i + 1)
            s = self._child(scope, "for", i, e - 1)
            semis = [q for q in top_level(T, po + 1, pc) if T.t(q) == ";"]
            if not semis:
                colon = next((q for q in top_level(T, po + 1, pc)
                              if T.t(q) == ":" and T.k(q) == "op"), None)
                if colon is not None:
                    ds = parse_declaration(T, po + 1, colon + 1, "range")
                    for d in ds or ():
                        self._add(s, d)
                    self._expr(s, colon + 1, pc)
            else:
                self._decl_or_expr(s, po + 1, semis[0] + 1)
                if len(semis) > 1:
                    self._decl_or_expr(s, semis[0] + 1, semis[1], "cond")
                    self._expr(s, semis[1] + 1, pc)
            self._sub(s, pc + 1, e)
            return
        if t == "while":
            po, pc = i + 1, T.close(i + 1)
            s = self._child(scope, "while", i, e - 1)
            self._decl_or_expr(s, po + 1, pc, "cond")
            self._sub(s, pc + 1, e)
            return
        if t == "do":
            be = stmt_end(T, i + 1, e)
            self._sub(scope, i + 1, be)
            self._expr(scope, be, e)
            return
        if t == "try":
            c = T.close(i + 1)
            self._stmt(scope, i + 1, c + 1)
            j = c + 1
            while T.t(j) == "catch":
                po, pc = j + 1, T.close(j + 1)
                bo = pc + 1
                bc = T.close(bo)
                s = self._child(scope, "catch", j, bc)
                ds = parse_declaration(T, po + 1, pc, "range")
                for d in ds or ():
                    self._add(s, d)
                self._walk(s, bo + 1, bc)
                j = bc + 1
            return
        if t in ("return", "break", "continue", "goto", "throw", "case", "default",
                 "co_return", "else"):
            self._expr(scope, i, e)
            return
        if T.k(i) == "id" and t not in KEYWORDS and T.t(i + 1) == ":":
            return
        self._decl_or_expr(scope, i, e)

    def _expr(self, scope, a, b, owner=None):
        """Lambdas inside the expression tokens [a, b) become child scopes."""
        T = self.T
        j = a
        while j < b:
            if T.t(j) == "[":
                lam = _lambda_at(T, j, b)
                if lam:
                    cc, po, bo = lam
                    bc = T.close(bo)
                    s = self._child(scope, "lambda", j, bc)
                    if owner is not None and owner.kind == "closure" and j == owner.init[0]:
                        owner.lambda_scope = s
                    self._captures(s, j, cc)
                    if po is not None:
                        params, _ = parse_params(T, po)
                        for p in params:
                            if p.name:
                                self._add(s, Decl(p.name, p.name_tok, "param",
                                                  T.line(p.name_tok),
                                                  is_ref=p.kind in ("W", "R"),
                                                  is_ptr=p.kind in ("W*", "R*"),
                                                  is_const=p.kind in ("R", "R*")))
                    self._walk(s, bo + 1, bc)
                    j = bc + 1
                    continue
            j += 1

    def _captures(self, s, co, cc):
        """Init-captures `[x = expr]` / `[&x = expr]` are declarations of the lambda scope."""
        T = self.T
        start = co + 1
        items, d = [], 0
        for q in range(co + 1, cc):
            if T.k(q) == "op" and T.t(q) in OPEN:
                d += 1
            elif T.k(q) == "op" and T.t(q) in CLOSE:
                d -= 1
            elif T.t(q) == "," and d == 0:
                items.append((start, q))
                start = q + 1
        if start < cc:
            items.append((start, cc))
        for a, b in items:
            k = a + 1 if T.t(a) == "&" else a
            if T.k(k) == "id" and T.t(k + 1) in ("=", "{", "("):
                if T.t(k + 1) == "=":
                    init, ik = (k + 2, b), "="
                else:
                    init, ik = (k + 2, T.close(k + 1)), T.t(k + 1)
                self._expr(s.parent, init[0], init[1])
                self._add(s, Decl(T.t(k), k, "capture", T.line(k), is_ref=(k != a),
                                  init=init, init_kind=ik))

    # -- queries --
    def innermost(self, pos):
        s = self.root
        while True:
            for c in s.children:
                if c.start <= pos <= c.end:
                    s = c
                    break
            else:
                return s

    def lookup(self, name, pos):
        """The declaration @p name denotes at token @p pos (innermost scope first), or None."""
        s = self.innermost(pos)
        while s is not None:
            best = None
            for d in s.decls:
                if d.name == name and (d.kind in ("param", "capture") or d.tok < pos):
                    best = d
            if best is not None:
                return best
            s = s.parent
        return None

    def decls_named(self, name):
        return [d for s in self.all for d in s.decls if d.name == name]


# ---------------------------------------------------------------------------------------------
# Calls
# ---------------------------------------------------------------------------------------------
class Call:
    """A call found in a token range: name (qualified backwards over . -> ::), its name token,
    the argument spans, the parentheses."""
    __slots__ = ("name", "tok", "line", "args", "open", "close", "qstart")

    def __init__(self, name, tok, line, args, open_, close, qstart):
        self.name, self.tok, self.line, self.args = name, tok, line, args
        self.open, self.close, self.qstart = open_, close, qstart

    @property
    def nargs(self):
        return len(self.args)

    def __repr__(self):
        return f"Call({self.name}@{self.line}/{self.nargs})"


def split_args(T, i):
    """@p i at `(`: argument spans split at top-level commas ((), [], {} balanced; angle
    brackets ignored, as in the inventory)."""
    end = T.close(i)
    out, start, d = [], i + 1, 0
    for j in range(i + 1, end):
        v = T.t(j)
        if T.k(j) == "op" and v in OPEN:
            d += 1
        elif T.k(j) == "op" and v in CLOSE:
            d -= 1
        if d == 0 and T.k(j) == "op" and v == ",":
            out.append((start, j))
            start = j + 1
    if start < end:
        out.append((start, end))
    return out


_NOT_CALLS = frozenset(("if", "for", "while", "switch", "return", "sizeof", "static_cast", "catch"))


def calls_in(T, a, b):
    """Every `name(` call in [a, b), nested ones included, in source order."""
    out = []
    for j in range(a, b):
        if T.k(j) == "id" and j + 1 < b and T.t(j + 1) == "(" and T.t(j) not in _NOT_CALLS:
            name, k = T.t(j), j
            while k - 2 >= a and T.t(k - 1) in (".", "->", "::") and T.k(k - 2) == "id":
                name = T.t(k - 2) + T.t(k - 1) + name
                k -= 2
            out.append(Call(name, j, T.line(j), split_args(T, j + 1), j + 1, T.close(j + 1), k))
    return out


# ---------------------------------------------------------------------------------------------
# Bindings
# ---------------------------------------------------------------------------------------------
class Node:
    __slots__ = ()


class Ref(Node):
    """root: a storage key (D:x driver outermost, L:x lambda top level, A:x Application member,
    B:<line>.x block local); chain: member segments ('.m', '->m', '.f()', '[i]'); prefix: '', '&'
    or '*'."""
    __slots__ = ("root", "chain", "prefix")

    def __init__(self, root, chain=(), prefix=""):
        self.root, self.chain, self.prefix = root, tuple(chain), prefix

    def extend(self, chain):
        if not chain:
            return self
        if self.prefix == "*" and chain[0].startswith("."):
            return Ref(self.root, self.chain + ("->" + chain[0][1:],) + tuple(chain[1:]))
        if self.prefix:
            return Opaque(self.render() + "".join(chain), "member access through a prefixed binding")
        return Ref(self.root, self.chain + tuple(chain), "")

    def render(self, realize=None):
        r = realize(self.root) if realize else self.root
        return self.prefix + r + "".join(self.chain)

    def __repr__(self):
        return f"Ref({self.render()})"


class Copy(Node):
    __slots__ = ("inner",)

    def __init__(self, inner):
        self.inner = inner

    def render(self, realize=None):
        return "copy(" + self.inner.render(realize) + ")"

    def __repr__(self):
        return self.render()


class Cond(Node):
    __slots__ = ("c", "a", "b")

    def __init__(self, c, a, b):
        self.c, self.a, self.b = c, a, b

    def render(self, realize=None):
        return f"{self.c.render(realize)}?{self.a.render(realize)}:{self.b.render(realize)}"


class CallNode(Node):
    __slots__ = ("fn", "args")

    def __init__(self, fn, args):
        self.fn, self.args = fn, tuple(args)

    def render(self, realize=None):
        return f"{self.fn}(" + ",".join(a.render(realize) for a in self.args) + ")"


class Lit(Node):
    __slots__ = ("text",)

    def __init__(self, text):
        self.text = text

    def render(self, realize=None):
        return self.text


class Expr(Node):
    __slots__ = ("op", "args")

    def __init__(self, op, args):
        self.op, self.args = op, tuple(args)

    def render(self, realize=None):
        if len(self.args) == 1:
            return self.op + self.args[0].render(realize)
        return self.args[0].render(realize) + self.op + self.args[1].render(realize)


class Opaque(Node):
    __slots__ = ("text", "why")

    def __init__(self, text, why):
        self.text, self.why = text, why

    def render(self, realize=None):
        return "<" + self.text + ">"

    def __repr__(self):
        return f"Opaque({self.text!r}: {self.why})"


_BINARY_RANK = {"||": 1, "&&": 2, "|": 3, "^": 4, "&": 5, "==": 6, "!=": 6, "<": 7, ">": 7,
                "<=": 7, ">=": 7, "<=>": 7, "<<": 8, ">>": 8, "+": 9, "-": 9, "*": 10, "/": 10,
                "%": 10}
_ASSIGN = frozenset(("=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="))


class Resolver:
    """Resolves argument expressions of one Scopes tree to bindings.

    frames: scope -> root prefix ('D', 'L', ...); a declaration in any other scope is a block
    local B:<line>.name.  stop: root keys at which the walk ends (the realization tables).
    members: the Application data members (roots A:name).  params: optional name -> Node
    substitutions for the analysed function's parameters."""

    def __init__(self, scopes, frames, stop=(), members=(), params=None):
        self.S = scopes
        self.T = scopes.T
        self.frames = frames
        self.stop = set(stop)
        self.members = set(members)
        self.params = params or {}

    def key(self, d):
        f = self.frames.get(id(d.scope))
        if f is not None:
            return f"{f}:{d.name}"
        return f"B:{d.line}.{d.name}"

    def resolve(self, a, b, depth=0):
        T = self.T
        a, b = strip_parens(T, a, b)
        text = T.norm(a, b)
        if a >= b:
            return Opaque("", "empty expression")
        if depth > 40:
            return Opaque(text, "resolution too deep")
        tl = list(top_level(T, a, b))
        if any(T.t(q) in _ASSIGN for q in tl):
            return Opaque(text, "assignment")
        q = next((q for q in tl if T.t(q) == "?"), None)
        if q is not None:
            nest, c = 0, None
            for r in tl:
                if r <= q:
                    continue
                if T.t(r) == "?":
                    nest += 1
                elif T.t(r) == ":" and T.k(r) == "op":
                    if nest == 0:
                        c = r
                        break
                    nest -= 1
            if c is None:
                return Opaque(text, "conditional without ':'")
            return Cond(self.resolve(a, q, depth + 1), self.resolve(q + 1, c, depth + 1),
                        self.resolve(c + 1, b, depth + 1))
        best, best_rank = None, 99
        for q in tl:
            x = T.t(q)
            if x not in _BINARY_RANK or q == a:
                continue
            p, pk = T.t(q - 1), T.k(q - 1)
            if not (pk in ("num", "str", "chr") or p in (")", "]")
                    or (pk == "id" and p not in KEYWORDS) or p in ("true", "false", "nullptr", "this")):
                continue
            if x == "<" and pk == "id":
                m = match_angle(T, q, b)
                if m is not None and T.t(m + 1) in ("(", "{", "::"):
                    continue
            if x in (">", ">>") and any(T.t(r) == "<" for r in tl if r < q):
                continue
            r = _BINARY_RANK[x]
            if r <= best_rank:
                best, best_rank = q, r
        if best is not None:
            return Expr(T.t(best), (self.resolve(a, best, depth + 1),
                                    self.resolve(best + 1, b, depth + 1)))
        t = T.t(a)
        if t in ("&", "*"):
            inner = self.resolve(a + 1, b, depth + 1)
            if isinstance(inner, Ref) and inner.prefix == "":
                return Ref(inner.root, inner.chain, t)
            if isinstance(inner, Ref) and t == "&" and inner.prefix == "*" and not inner.chain:
                return Ref(inner.root, (), "")
            return Opaque(text, "unary operand is not a plain binding")
        if t in ("!", "-", "+", "~"):
            return Expr(t, (self.resolve(a + 1, b, depth + 1),))
        if b - a == 1 and (T.k(a) in ("num", "str", "chr") or t in ("true", "false", "nullptr")):
            return Lit(t)
        if T.k(a) in ("str",) and all(T.k(q) == "str" for q in range(a, b)):
            return Lit(text)
        return self._postfix(a, b, depth, text)

    def _postfix(self, a, b, depth, text):
        T = self.T
        i = a
        lead = ""
        if T.t(i) == "::":
            lead, i = "::", i + 1
        if T.k(i) != "id":
            return Opaque(text, "not a name")
        names = [T.t(i)]
        i += 1
        while i + 1 < b and T.t(i) == "::" and T.k(i + 1) == "id":
            names.append(T.t(i + 1))
            i += 2
        qual = lead + "::".join(names)
        if i < b and T.t(i) == "<":
            m = match_angle(T, i, b)
            if m is not None and T.t(m + 1) == "(":
                qual += T.norm(i, m + 1)   # a template function: static_cast<double>(...)
                i = m + 1
        if i < b and T.t(i) == "(":
            c = T.close(i)
            args = tuple(self.resolve(s, e, depth + 1) for s, e in split_args(T, i))
            if c + 1 < b:
                return Opaque(text, "member access on a call's result")
            return CallNode(qual, args)
        if len(names) > 1 or lead:
            if i == b:
                return Lit(qual)
            return Opaque(text, "qualified name followed by more tokens")
        chain = []
        while i < b:
            x = T.t(i)
            if x in (".", "->") and T.k(i + 1) == "id":
                seg = x + T.t(i + 1)
                i += 2
                if i < b and T.t(i) == "(":
                    c = T.close(i)
                    seg += "(" + T.norm(i + 1, c) + ")"
                    i = c + 1
                chain.append(seg)
                continue
            if x == "[":
                c = T.close(i)
                chain.append("[" + T.norm(i + 1, c) + "]")
                i = c + 1
                continue
            return Opaque(text, f"unexpected token {x!r}")
        return self.name_ref(names[0], a, tuple(chain), depth)

    def name_ref(self, name, pos, chain, depth=0):
        if name == "this":
            return Ref("A:this", chain)
        d = self.S.lookup(name, pos)
        if d is None:
            if name in self.members:
                return Ref("A:" + name, chain)
            return Opaque(name + "".join(chain), "unresolved name")
        key = self.key(d)
        if key in self.stop:
            return Ref(key, chain)
        if d.kind == "param" and name in self.params:
            node = self.params[name]
            return node.extend(chain) if isinstance(node, Ref) else node
        if d.init is not None and d.kind in ("var", "binding", "capture"):
            if d.is_ref and d.init_kind in ("=", "{", "("):
                inner = self.resolve(d.init[0], d.init[1], depth + 1)
                if isinstance(inner, Ref):
                    return inner.extend(chain)
                if isinstance(inner, Copy) and d.is_const:
                    return Copy(inner.inner.extend(chain))
                return Ref(key, chain)
            if not d.is_ref and not d.is_ptr and d.init_kind == "=":
                inner = self.resolve(d.init[0], d.init[1], depth + 1)
                if isinstance(inner, Ref) and inner.prefix == "":
                    ext = inner.extend(chain)
                    return Copy(ext) if isinstance(ext, Ref) else ext
                if isinstance(inner, Copy):
                    ext = inner.inner.extend(chain)
                    return Copy(ext) if isinstance(ext, Ref) else ext
        return Ref(key, chain)


# ---------------------------------------------------------------------------------------------
# The frozen revision
# ---------------------------------------------------------------------------------------------
class GitTree:
    """Read-only access to one commit of the repository (never the working tree)."""

    def __init__(self, rev, root=ROOT):
        self.rev = rev
        self.root = pathlib.Path(root)
        r = subprocess.run(["git", "-C", str(self.root), "rev-parse", "--verify", "--quiet",
                            f"{rev}^{{commit}}"], capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"nom_cpp: revision {rev!r} does not name a commit")
        self.sha = r.stdout.strip()
        self._proc = None
        self._text = {}
        self._tokens = {}
        self._index = {}
        self._files = {}

    def files(self, *prefixes):
        key = prefixes
        if key not in self._files:
            r = subprocess.run(["git", "-C", str(self.root), "ls-tree", "-r", "--name-only",
                                self.sha, "--", *prefixes], capture_output=True, check=True)
            self._files[key] = r.stdout.decode("utf-8").split("\n")[:-1]
        return self._files[key]

    def read(self, path):
        """The file's text at the revision (LF line ends), or None when it does not exist."""
        if path in self._text:
            return self._text[path]
        if self._proc is None:
            self._proc = subprocess.Popen(["git", "-C", str(self.root), "cat-file", "--batch"],
                                          stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        self._proc.stdin.write(f"{self.sha}:{path}\n".encode("utf-8"))
        self._proc.stdin.flush()
        head = self._proc.stdout.readline().decode("utf-8").split()
        if len(head) < 3 or head[-1] == "missing" or head[1] != "blob":
            self._text[path] = None
            return None
        size = int(head[2])
        data = self._proc.stdout.read(size)
        self._proc.stdout.read(1)
        text = data.decode("utf-8", errors="replace").replace("\r\n", "\n")
        self._text[path] = text
        return text

    def tokens(self, path):
        if path not in self._tokens:
            text = self.read(path)
            self._tokens[path] = None if text is None else Tokens(text, path)
        return self._tokens[path]

    def index(self, path):
        if path not in self._index:
            T = self.tokens(path)
            self._index[path] = None if T is None else FileIndex(T)
        return self._index[path]

    def close(self):
        if self._proc is not None:
            self._proc.stdin.close()
            self._proc.wait()
            self._proc = None


# ---------------------------------------------------------------------------------------------
# The driver: Application::runNetwork and its per-epoch lambda
# ---------------------------------------------------------------------------------------------
DRIVER_PATH = "src/core/app/ApplicationNetwork.cpp"
APPLICATION_HPP = "include/app/Application.hpp"
LAMBDA_NAME = "processSyncedEpoch"

# The call table's kinds (the inventory's SEAM pattern): the seams, the trace and estimator
# methods, the two carry engines' begin-epoch calls, the broadcast feed and the log prefix.
SEAM_CALL = re.compile(r"^(network\w+|traces\.\w+|ambStateNoteErase|arcReasonExpand|prmRespond|"
                       r"est\.\w+|carry_driver\.\w+|ctx\.carry\.(bb|cg)\.beginEpoch|"
                       r"setup\.brdc_apriori->feedStore|LogManager::setCurrentEpochTime)$")


class Driver:
    """The frozen driver at a revision.

    T, index: the lexed ApplicationNetwork.cpp and its function index; rn: runNetwork's Func;
    scopes: the scope tree of runNetwork's body (the lambda is a child scope); lam_decl,
    lam_scope, lam_open, lam_close: the lambda's declaration, scope and body braces; calls: the
    call table (SEAM_CALL calls anywhere in the lambda body, source order); members: the
    Application data members."""

    def __init__(self, tree):
        self.tree = tree
        T = tree.tokens(DRIVER_PATH)
        if T is None:
            raise SystemExit(f"nom_cpp: {DRIVER_PATH} does not exist at {tree.rev}")
        self.T = T
        self.index = tree.index(DRIVER_PATH)
        rn = [f for f in self.index.funcs if f.qual == "Application::runNetwork" and f.is_def]
        if len(rn) != 1:
            raise SystemExit(f"nom_cpp: Application::runNetwork definition not found ({len(rn)})")
        self.rn = rn[0]
        self.scopes = Scopes(T, self.rn.body[0], self.rn.body[1])
        lam = [d for d in self.scopes.root.decls if d.name == LAMBDA_NAME]
        if len(lam) != 1 or lam[0].lambda_scope is None:
            raise SystemExit(f"nom_cpp: the per-epoch lambda {LAMBDA_NAME} was not found")
        self.lam_decl = lam[0]
        self.lam_scope = lam[0].lambda_scope
        self.lam_close = self.lam_scope.end
        self.lam_open = T.match[self.lam_close]
        app = tree.index(APPLICATION_HPP)
        self.members = set(app.members.get("Application", ())) if app else set()
        self.calls = [c for c in calls_in(T, self.lam_open + 1, self.lam_close)
                      if SEAM_CALL.match(c.name)]
        self.frames = {id(self.scopes.root): "D", id(self.lam_scope): "L"}

    def lambda_statements(self):
        return statements(self.T, self.lam_open + 1, self.lam_close)

    def driver_statements(self):
        return statements(self.T, self.rn.body[0] + 1, self.rn.body[1])

    def captures(self):
        """Names declared in runNetwork's outermost scope before the lambda and read by the
        lambda (resolved by the scope walk), with their reference counts."""
        T, S = self.T, self.scopes
        root = S.root
        out = collections.OrderedDict()
        for j in range(self.lam_open + 1, self.lam_close):
            if T.k(j) != "id" or T.t(j - 1) in (".", "->", "::"):
                continue
            d = S.lookup(T.t(j), j)
            if d is None or d.scope is not root or d is self.lam_decl:
                continue
            out[d.name] = out.get(d.name, 0) + 1
        return out

    def resolver(self, stop=(), params=None):
        return Resolver(self.scopes, self.frames, stop, self.members, params)

    def top_statement_of(self, tok):
        """The lambda's top-level statement (start, end) that holds token @p tok."""
        for s, e in self.lambda_statements():
            if s <= tok < e:
                return s, e
        return None


# ---------------------------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------------------------
# The inventory's counts at the EP0 base (a453448 / c483a11).  At a later base whose driver
# changed (the REMOVE-key deletion), the machinery is still proved at the EP0 base, and
# --expect states the counts expected at the new one.
EXPECTED = {"captured": 62, "loopsignal": 1, "calls": 99, "network_calls": 79,
            "network_args": 753}


def _calltable_rows(path):
    rows = []
    for line in pathlib.Path(path).read_text(encoding="utf-8").splitlines():
        m = re.match(r"^\| (\d+) \| (\d+) \| `([^`]+)` \| (\d+) \|", line)
        if m:
            rows.append((int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4))))
    return rows


def selftest(base, calltable=None, expect=None):
    want = dict(EXPECTED, **(expect or {}))
    tree = GitTree(base)
    drv = Driver(tree)
    T = drv.T
    fails = 0

    def check(label, got, want):
        nonlocal fails
        ok = got == want
        fails += 0 if ok else 1
        print(f"  [{'pass' if ok else 'FAIL'}] {label}: {got} (expected {want})")

    print(f"nom_cpp self-test at {base} ({tree.sha[:10]})")
    print(f"  runNetwork body lines {T.line(drv.rn.body[0])}-{T.line(drv.rn.body[1])}; "
          f"lambda {LAMBDA_NAME} body lines {T.line(drv.lam_open)}-{T.line(drv.lam_close)}")
    caps = drv.captures()
    names = [n for n in caps if n != "LoopSignal"]
    print("  captured: " + ", ".join(f"{n}({caps[n]})" for n in caps))
    check("captured names", len(names), want["captured"])
    check("LoopSignal captured", int("LoopSignal" in caps), want["loopsignal"])
    check("calls", len(drv.calls), want["calls"])
    net = [c for c in drv.calls if c.name.startswith("network")]
    check("network* calls", len(net), want["network_calls"])
    check("network* arguments", sum(c.nargs for c in net), want["network_args"])
    print(f"  (all calls carry {sum(c.nargs for c in drv.calls)} arguments)")

    if calltable:
        rows = _calltable_rows(calltable)
        got = [(c.name, c.nargs) for c in drv.calls]
        want = [(r[2], r[3]) for r in rows]
        diffs = [(k + 1, g, w) for k, (g, w) in enumerate(zip(got, want)) if g != w]
        moved = sum(1 for c, r in zip(drv.calls, rows) if c.line != r[1])
        check(f"call sequence equals {pathlib.Path(calltable).name} (callee, arguments)",
              "equal" if not diffs and len(got) == len(want) else f"{len(diffs)} differ",
              "equal")
        for k, g, w in diffs[:10]:
            print(f"      #{k}: here {g}, table {w}")
        print(f"      ({moved} of {len(rows)} calls sit on another line than the table's)")

    # The shadowing facts and reference walks of the binding model (spec 5.4).
    res = drv.resolver()

    def arg_of(callee, param_index, occurrence=0):
        cs = [c for c in drv.calls if c.name == callee]
        if len(cs) <= occurrence:
            return None
        s, e = cs[occurrence].args[param_index]
        return res.resolve(s, e)

    def show(node):
        return node.render() if node is not None else "(call not found)"

    begin = [c for c in drv.calls if c.name == "carry_driver.beginEpoch"]
    dt_block = None
    if begin:
        c = begin[0]
        for j in range(c.open + 1, c.close):
            if T.t(j) == "dt":
                d = drv.scopes.lookup("dt", j)
                dt_block = res.key(d) if d else None
                break
    check("beginEpoch's dt is the DD-carry predict's block-local step",
          bool(dt_block and dt_block.startswith("B:")), True)
    print(f"      (resolved to {dt_block})")
    sg = [c for c in drv.calls if c.name == "networkSolveGlue"]
    dt_lam = None
    if sg:
        for s, e in sg[0].args:
            if T.t(s) == "dt" and e == s + 1:
                d = drv.scopes.lookup("dt", s)
                dt_lam = res.key(d) if d else None
    check("networkSolveGlue's dt is the lambda's dt", dt_lam, "L:dt")
    inst = [c for c in drv.calls if c.name == "networkInstallCarry"]
    xs = None
    if inst:
        for s, e in inst[0].args:
            if T.t(s) == "x_s" and e == s + 1:
                xs = res.resolve(s, e).render()
    check("x_s at networkInstallCarry follows its reference", xs, "L:sg_.x_s")
    st = None
    roster = [c for c in drv.calls if c.name == "networkBuildSignalRoster"]
    if roster:
        s, e = roster[0].args[-1]
        st = res.resolve(s, e).render()
    check("stations follows its reference to the setup", st, "D:setup.stations")
    raw = arg_of("networkPassASlipScan_", 10)
    check("networkPassASlipScan_ binds carry_slipped_ambs to the raw subset", show(raw),
          "L:carry_slipped_raw")
    tree.close()
    print(f"nom_cpp self-test: {'PASS' if not fails else f'FAIL ({fails})'}")
    return 0 if not fails else 1


def main(argv=None):
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    from nom_ep1 import DEFAULT_BASE  # noqa: E402
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true", help="run the self-test (the default)")
    ap.add_argument("--base", default=DEFAULT_BASE, help=f"frozen revision (default {DEFAULT_BASE})")
    ap.add_argument("--calltable", help="also compare the call sequence with this call table (.md)")
    ap.add_argument("--expect", default="", metavar="KEY=N,...",
                    help="override expected counts (keys: " + ", ".join(EXPECTED) + ")")
    args = ap.parse_args(argv)
    expect = {}
    for item in filter(None, args.expect.split(",")):
        k, _, v = item.partition("=")
        if k not in EXPECTED or not v.isdigit():
            ap.error(f"--expect: {item!r} (keys: {', '.join(EXPECTED)})")
        expect[k] = int(v)
    return selftest(args.base, args.calltable, expect)


if __name__ == "__main__":
    sys.exit(main())

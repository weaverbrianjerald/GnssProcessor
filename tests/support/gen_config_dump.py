#!/usr/bin/env python3
"""Generate tests/support/ConfigDumpGenerated.inc: one dump line per configuration member.

The configuration (class GnssConfig) exposes its loaded values through public const getters
that take no argument; several of them return plain sub-structs (NetworkConfig and its nested
structs, RealTimeConfig, CycleSlipConfig, ...) whose fields are the members themselves.  This
script reads the two headers, include/config/GnssConfig.hpp and
include/config/GnssConfigTypes.hpp, and writes one C++ statement per member that appends
"name=value" to the dump through the lambda `add` of tests/support/ConfigDump.hpp:

  * a getter of a printable type (bool, integer, floating point, string, path, enum, and
    vectors, arrays, sets, maps, pairs and optionals of those) gives one line named after the
    getter;
  * a getter returning a parsed struct is expanded field by field, the name being the dotted
    path ("getNetworkConfig.ambiguity_resolution.enable");
  * a vector or array of a parsed struct gives its size and then every element field by field
    ("getNetworkConfig.stations[0].name");
  * a credential (a std::string member named like a password, secret or token) is printed as
    "<set>" or "<empty>" through `addSecret`, never as its text;
  * any other type (a pointer to another class, a function, a type of another header) is not
    printed and is listed in the comment at the top of the generated file, so a member is never
    skipped silently.  The private data members of GnssConfig that no listed getter reads are
    listed there too.

`--review` prints the getters whose body is more than a plain
"return member;" (computed values) and those defined out of line, for a reader to check that
each is a pure read.

The list is generated, not maintained by hand: after a member or a getter is added to the
headers, re-run

    python tests/support/gen_config_dump.py

from the repository root (or from anywhere: paths are taken relative to this file) and commit
the regenerated file.  `--check` exits with status 1 when the checked-in file differs from what
the headers give now.  Standard library only.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GNSS_CONFIG_HPP = ROOT / "include" / "config" / "GnssConfig.hpp"
TYPES_HPP = ROOT / "include" / "config" / "GnssConfigTypes.hpp"
LOG_MANAGER_HPP = ROOT / "include" / "logging" / "LogManager.hpp"
OUT_INC = ROOT / "tests" / "support" / "ConfigDumpGenerated.inc"

MAX_DEPTH = 8   # nesting bound of the struct expansion (a cycle would otherwise never end)


# ---------------------------------------------------------------------------
# Lexing helpers
# ---------------------------------------------------------------------------

def strip_comments(text: str) -> str:
    """Remove // and /* */ comments; string, character and raw string literals are kept."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            seg = text[i:n if j < 0 else j + 2]
            out.append("\n" * seg.count("\n"))   # keep line numbers
            i = n if j < 0 else j + 2
            continue
        if c == "R" and i + 1 < n and text[i + 1] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            m = re.match(r'R"([^(\s]*)\(', text[i:])
            if m:
                end = text.find(")" + m.group(1) + '"', i)
                end = n if end < 0 else end + len(m.group(1)) + 2
                out.append(text[i:end])
                i = end
                continue
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def match_brace(text: str, i: int) -> int:
    """Index of the '}' closing the '{' at @p i (literals in @p text are skipped)."""
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    raise ValueError("unbalanced braces")


def split_top(s: str, sep: str = ",") -> list[str]:
    """Split @p s on @p sep outside <>, (), [] and {}."""
    parts, depth, cur = [], 0, []
    for c in s:
        if c in "<([{":
            depth += 1
        elif c in ">)]}":
            depth -= 1
        if c == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
    parts.append("".join(cur))
    return [p.strip() for p in parts]


def norm_ws(s: str) -> str:
    s = re.sub(r"\s+", " ", s).strip()
    s = re.sub(r"\s*([<>,:&*])\s*", r"\1", s)
    s = s.replace("const ", "const ").replace(",", ", ")
    return s


# ---------------------------------------------------------------------------
# A class or struct body
# ---------------------------------------------------------------------------

@dataclass
class Field:
    type: str
    name: str
    access: str


@dataclass
class Function:
    ret: str
    name: str
    params: str
    quals: str
    access: str
    static: bool
    body: str | None   # None: declared only


@dataclass
class Scope:
    name: str                       # qualified, "RealTimeConfig::Corrections"
    fields: list[Field] = field(default_factory=list)
    functions: list[Function] = field(default_factory=list)
    skipped: list[str] = field(default_factory=list)


STRUCTS: dict[str, Scope] = {}
ENUMS: set[str] = set()
ALIASES: dict[str, tuple[str, str]] = {}   # qualified alias -> (target as written, scope it is read in)

FUNC_TAIL = re.compile(r"\)\s*(const)?\s*(noexcept(\s*\([^)]*\))?)?\s*(override|final)?\s*(->\s*[\w:<>, &*]+)?\s*$")
ACCESS = re.compile(r"^\s*(public|private|protected)\s*:\s*")


def parse_body(body: str, qual: str, default_access: str) -> Scope:
    """Parse the members of one class or struct body (the text between its braces)."""
    scope = Scope(qual)
    access = default_access
    buf = []
    i, n = 0, len(body)

    def flush_statement(stmt: str) -> None:
        stmt =re.sub(r"\[\[[^\]]*\]\]", " ", stmt).strip()
        if not stmt:
            return
        classify_statement(stmt, scope, access)

    while i < n:
        c = body[i]
        if c in "\"'":
            j = i + 1
            while j < n and body[j] != c:
                j += 2 if body[j] == "\\" else 1
            buf.append(body[i:j + 1])
            i = j + 1
            continue
        if c == "#":   # preprocessor line inside a body
            j = body.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "{":
            head = "".join(buf)
            head_s = re.sub(r"\[\[[^\]]*\]\]", " ", head).strip()
            close = match_brace(body, i)
            inner = body[i + 1:close]
            m_struct = re.match(r"^(?:template\s*<[^>]*>\s*)?(struct|class|union)\s+(\w+)\s*(?:final)?\s*(?::[^{]*)?$", head_s)
            m_enum = re.match(r"^enum(?:\s+(?:class|struct))?\s+(\w+)\s*(?::[^{]*)?$", head_s)
            if m_struct:
                sub_qual = f"{qual}::{m_struct.group(2)}" if qual else m_struct.group(2)
                sub = parse_body(inner, sub_qual, "private" if m_struct.group(1) == "class" else "public")
                STRUCTS[sub_qual] = sub
                # "struct X {...} name;" declares a field of the nested type too
                semi = body.find(";", close)
                tail = body[close + 1:semi].strip()
                if tail and re.fullmatch(r"\w+", tail):
                    scope.fields.append(Field(sub_qual, tail, access))
                buf = []
                i = semi + 1
                continue
            if m_enum:
                ENUMS.add(f"{qual}::{m_enum.group(1)}" if qual else m_enum.group(1))
                semi = body.find(";", close)
                buf = []
                i = semi + 1
                continue
            if FUNC_TAIL.search(head_s) or re.search(r"\)\s*:\s*[\w:]+\s*[({]", head_s) and "=" not in head_s.split("(")[0]:
                record_function(head_s, inner, scope, access)
                buf = []
                i = close + 1
                continue
            # a brace initializer: keep a placeholder and go on to the ';'
            buf.append("{}")
            i = close + 1
            continue
        if c == ";":
            flush_statement("".join(buf))
            buf = []
            i += 1
            continue
        buf.append(c)
        if c == ":":
            m = ACCESS.match("".join(buf))
            if m and "".join(buf).strip().endswith(":") and not "".join(buf).strip().endswith("::"):
                access = m.group(1)
                buf = []
        i += 1
    return scope


def record_function(head: str, body: str | None, scope: Scope, access: str) -> None:
    """Record a member function declaration or definition whose text before the body is @p head."""
    h = head
    h = re.sub(r"^template\s*<.*?>\s*", "", h, flags=re.S)
    if re.match(r"^template\b", h):
        scope.skipped.append(head)
        return
    h = re.sub(r"\)\s*:\s*[\w:]+\s*[({].*$", ")", h, flags=re.S)   # constructor initializer list
    m = re.match(r"^(?P<pre>.*?)\b(?P<name>~?\w+|operator\s*[^\s(]+)\s*\((?P<params>.*)\)(?P<quals>[^()]*)$", h, flags=re.S)
    if not m:
        scope.skipped.append(head)
        return
    pre = m.group("pre").strip()
    static = bool(re.search(r"\bstatic\b", pre))
    ret = re.sub(r"\b(inline|constexpr|virtual|explicit|static|friend)\b", " ", pre)
    scope.functions.append(Function(norm_ws(ret), m.group("name"), m.group("params").strip(),
                                    norm_ws(m.group("quals")), access, static, body))


def classify_statement(stmt: str, scope: Scope, access: str) -> None:
    s = stmt.strip()
    m_alias = re.match(r"^using\s+(\w+)\s*=\s*(.+)$", s, flags=re.S)
    if m_alias:
        qual = f"{scope.name}::{m_alias.group(1)}" if scope.name else m_alias.group(1)
        ALIASES[qual] = (m_alias.group(2).strip(), scope.name)
        return
    if re.match(r"^(using|typedef|friend|static_assert|template|enum|struct|class|union)\b", s):
        if re.match(r"^template\b", s) and "(" in s:
            record_function(s, None, scope, access)
        else:
            scope.skipped.append(s)
        return
    if re.match(r"^static\b", s) and "(" not in s.split("=")[0]:
        scope.skipped.append(s)      # a static data member is not a member of one configuration
        return
    # the declarator part: up to the first top-level '=' or brace initializer
    depth = 0
    cut = len(s)
    for k, c in enumerate(s):
        if c in "<([":
            depth += 1
        elif c in ">)]":
            depth -= 1
        elif depth == 0 and (c == "=" or c == "{"):
            if c == "=" and k + 1 < len(s) and s[k + 1] == "=":
                continue
            cut = k
            break
    decl = s[:cut].strip()
    if "(" in decl:
        if re.search(r"=\s*(default|delete|0)\s*$", s):
            s = re.sub(r"=\s*(default|delete|0)\s*$", "", s)
        record_function(s, None, scope, access)
        return
    parts = split_top(decl)
    m = re.match(r"^(?P<type>.*?[\w>*&\s])\s*\b(?P<name>\w+)\s*(?P<arr>\[[^\]]*\])?$", parts[0], flags=re.S)
    if not m:
        scope.skipped.append(s)
        return
    base_type = norm_ws(re.sub(r"\b(mutable|inline)\b", " ", m.group("type")))
    names = [m.group("name") + (m.group("arr") or "")]
    for extra in parts[1:]:
        mm = re.match(r"^\s*(\w+)\s*(\[[^\]]*\])?\s*$", extra)
        if mm:
            names.append(mm.group(1) + (mm.group(2) or ""))
    for nm in names:
        if "[" in nm:
            scope.fields.append(Field(base_type + nm[nm.index("["):], nm[:nm.index("[")], access))
        else:
            scope.fields.append(Field(base_type, nm, access))


def parse_file_scopes(text: str) -> None:
    """Register every namespace-level struct, class and enum of a header."""
    t = strip_comments(text)
    for m in re.finditer(r"(?m)^\s*(?:template\s*<[^>]*>\s*)?(struct|class)\s+(\w+)\s*(?:final)?\s*(?::[^{;]*)?\{", t):
        name = m.group(2)
        if name in STRUCTS:
            continue
        # a struct nested in another is registered by the parse of its parent
        start = m.end() - 1
        close = match_brace(t, start)
        if is_nested(t, m.start()):
            continue
        STRUCTS[name] = parse_body(t[start + 1:close], name,
                                   "private" if m.group(1) == "class" else "public")
    for m in re.finditer(r"(?m)^\s*enum(?:\s+(?:class|struct))?\s+(\w+)\s*(?::[^{;]*)?\{", t):
        if not is_nested(t, m.start()):
            ENUMS.add(m.group(1))


def is_nested(t: str, pos: int) -> bool:
    """True when @p pos lies inside a class or struct body (brace depth beyond the namespaces)."""
    stack = []   # one entry per open brace: "ns" for a namespace, "x" for anything else
    for m in re.finditer(r"namespace\s+[\w:]*\s*\{|\{|\}", t[:pos]):
        tok = m.group(0)
        if tok.startswith("namespace"):
            stack.append("ns")
        elif tok == "{":
            stack.append("x")
        else:
            if stack:
                stack.pop()
    return any(s == "x" for s in stack)


# ---------------------------------------------------------------------------
# Types
# ---------------------------------------------------------------------------

INTEGERS = {
    "bool", "char", "signed char", "unsigned char", "short", "unsigned short", "int", "unsigned",
    "unsigned int", "long", "unsigned long", "long long", "unsigned long long", "size_t",
    "std::size_t", "std::int8_t", "std::int16_t", "std::int32_t", "std::int64_t", "std::uint8_t",
    "std::uint16_t", "std::uint32_t", "std::uint64_t", "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "std::ptrdiff_t",
}
FLOATS = {"double", "float", "long double"}
STRINGS = {"std::string", "std::filesystem::path", "const char*", "char*"}
LISTS = {"std::vector": 1, "std::array": 1, "std::set": 1, "std::optional": 1, "std::deque": 1}
PAIRS = {"std::pair": 2, "std::map": 2}


def clean_type(t: str) -> str:
    t = norm_ws(t)
    t = re.sub(r"^const\s+", "", t)
    t = t.replace(" const", "").rstrip("&").strip()
    if t.startswith("const "):
        t = t[6:]
    if t == "const char*":
        return t
    return t


def template_args(t: str) -> tuple[str, list[str]] | None:
    m = re.match(r"^([\w:]+)<(.*)>$", t)
    if not m:
        return None
    return m.group(1), split_top(m.group(2))


def resolve(name: str, scope: str, table) -> str | None:
    """The qualified name of @p name seen from @p scope, if @p table has it."""
    name = name.removeprefix("GnssProcessor::").removeprefix("::")
    parts = scope.split("::") if scope else []
    while True:
        cand = "::".join(parts + [name])
        if cand in table:
            return cand
        if not parts:
            return None
        parts.pop()


def kind_of(t: str, scope: str) -> tuple[str, str]:
    """('scalar'|'struct'|'struct_list'|'unprintable', resolved type) of a member type."""
    t = clean_type(t)
    alias = resolve(t, scope, ALIASES)
    if alias:
        target, target_scope = ALIASES[alias]
        return kind_of(target, target_scope)
    if re.search(r"\[[^\]]*\]$", t):
        return "unprintable", t       # a C array
    if t in INTEGERS or t in FLOATS or t in STRINGS:
        return "scalar", t
    if t == "const char*":
        return "scalar", t
    if resolve(t, scope, ENUMS):
        return "scalar", t
    st = resolve(t, scope, STRUCTS)
    if st:
        return "struct", st
    ta = template_args(t)
    if ta:
        tmpl, args = ta
        if tmpl in LISTS:
            k, r = kind_of(args[0], scope)
            if k == "scalar":
                return "scalar", t
            if k == "struct" and tmpl in ("std::vector", "std::array", "std::deque"):
                return "struct_list", r
            return "unprintable", t
        if tmpl in PAIRS:
            if all(kind_of(a, scope)[0] == "scalar" for a in args):
                return "scalar", t
            return "unprintable", t
    return "unprintable", t


# ---------------------------------------------------------------------------
# Emission
# ---------------------------------------------------------------------------

@dataclass
class Out:
    lines: list[str] = field(default_factory=list)
    unprintable: list[str] = field(default_factory=list)
    lists: list[str] = field(default_factory=list)
    secrets: list[str] = field(default_factory=list)
    members: int = 0


def cxx_str(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


SECRET = re.compile(r"password|passwd|secret|token", re.I)


@dataclass(frozen=True)
class Name:
    """A dump name as C++: an optional std::string variable (inside a loop) plus a literal tail."""
    var: str | None
    lit: str

    def plus(self, suffix: str) -> "Name":
        return Name(self.var, self.lit + suffix)

    def cxx(self) -> str:
        if self.var is None:
            return cxx_str(self.lit)
        return self.var if not self.lit else f"{self.var} + {cxx_str(self.lit)}"

    def cxx_string(self) -> str:
        """The name as an expression of type std::string (never a bare literal)."""
        return f"std::string({cxx_str(self.lit)})" if self.var is None else self.cxx()


def emit_struct(out: Out, st_name: str, expr: str, name: Name, indent: str, depth: int,
                display: str) -> None:
    """Emit every field of the struct @p st_name reached by the C++ expression @p expr.

    @p name is the dump name prefix, @p display the same prefix for the comment list."""
    if depth > MAX_DEPTH:
        out.unprintable.append(f"{display} ({st_name}: nesting deeper than {MAX_DEPTH})")
        return
    st = STRUCTS[st_name]
    for f in st.fields:
        if f.access != "public":
            out.unprintable.append(f"{display}.{f.name} ({f.type}: not public)")
            continue
        k, r = kind_of(f.type, st_name)
        emit_member(out, k, r, f.type, f"{expr}.{f.name}", name.plus("." + f.name),
                    f"{display}.{f.name}", indent, depth)


def emit_member(out: Out, k: str, r: str, raw_type: str, expr: str, name: Name, disp: str,
                indent: str, depth: int) -> None:
    leaf = re.split(r"[.\]]", disp)[-1]
    if k == "scalar" and SECRET.search(leaf) and clean_type(raw_type) == "std::string":
        out.lines.append(f"{indent}addSecret({name.cxx()}, {expr});")
        out.members += 1
        out.secrets.append(disp)
    elif k == "scalar":
        out.lines.append(f"{indent}add({name.cxx()}, {expr});")
        out.members += 1
    elif k == "struct":
        emit_struct(out, r, expr, name, indent, depth + 1, disp)
    elif k == "struct_list":
        v = f"v{depth}_"
        ix = f"i{depth}_"
        p = f"p{depth}_"
        out.lines.append(f"{indent}{{")
        out.lines.append(f"{indent}    const auto& {v} = {expr};")
        out.lines.append(f"{indent}    add({name.plus('.size').cxx()}, {v}.size());")
        out.lines.append(f"{indent}    for (std::size_t {ix} = 0; {ix} < {v}.size(); ++{ix}) {{")
        out.lines.append(f"{indent}        const std::string {p} = {name.cxx_string()} + \"[\" + std::to_string({ix}) + \"]\";")
        emit_struct(out, r, f"{v}[{ix}]", Name(p, ""), indent + "        ", depth + 1, disp + "[i]")
        out.lines.append(f"{indent}    }}")
        out.lines.append(f"{indent}}}")
        out.members += 1
        out.lists.append(f"{disp} ({clean_type(raw_type)}: size, then each element)")
    else:
        out.unprintable.append(f"{disp} ({clean_type(raw_type)})")


def getters(cls: Scope) -> list[Function]:
    """The public, non-static, const member functions of @p cls that take no argument."""
    seen = set()
    res = []
    for f in cls.functions:
        if f.access != "public" or f.static or f.name.startswith("operator") or f.name.startswith("~"):
            continue
        if f.params not in ("", "void") or not re.match(r"^const\b", f.quals):
            continue
        if f.ret in ("void", "") or f.name in seen:
            continue
        seen.add(f.name)
        res.append(f)
    return res


def generate() -> tuple[str, list[str]]:
    STRUCTS.clear()
    ENUMS.clear()
    ALIASES.clear()
    parse_file_scopes(LOG_MANAGER_HPP.read_text(encoding="utf-8"))
    parse_file_scopes(TYPES_HPP.read_text(encoding="utf-8"))
    cfg_text = strip_comments(GNSS_CONFIG_HPP.read_text(encoding="utf-8"))
    m = re.search(r"\bclass\s+GnssConfig\s*\{", cfg_text)
    if not m:
        raise SystemExit("class GnssConfig not found in " + str(GNSS_CONFIG_HPP))
    start = m.end() - 1
    close = match_brace(cfg_text, start)
    cls = parse_body(cfg_text[start + 1:close], "GnssConfig", "private")
    STRUCTS.pop("GnssConfig", None)

    out = Out()
    gs = getters(cls)
    for g in gs:
        k, r = kind_of(g.ret, "GnssConfig")
        emit_member(out, k, r, g.ret, f"cfg.{g.name}()", Name(None, g.name), g.name, "", 0)

    # Private data members of GnssConfig that no listed getter reads: the dump cannot see them.
    getter_text = " ".join(g.body or "" for g in gs)
    hidden = [f"{f.name} ({clean_type(f.type)})" for f in cls.fields
              if f.access != "public" and not re.search(r"\b" + re.escape(f.name) + r"\b", getter_text)]

    review = []
    for g in gs:
        if g.body is None:
            review.append(f"declared only (defined in a .cpp): {g.ret} {g.name}()")
        elif not re.fullmatch(r"\s*return\s+[\w.]+\s*;\s*", g.body):
            review.append(f"computed body: {g.ret} {g.name}() {{ {' '.join(g.body.split())[:160]} }}")

    head = [
        "// GENERATED by tests/support/gen_config_dump.py from include/config/GnssConfig.hpp and",
        "// include/config/GnssConfigTypes.hpp.  Do not edit: re-run the script after a header change.",
        "//",
        "// Included inside configMemberDump() (tests/support/ConfigDump.hpp), where `cfg` is the",
        "// loaded GnssConfig and `add(name, value)` appends one \"name=value\" line.",
        "//",
        f"// {len(gs)} public const getters without arguments; {out.members} dump statements.",
        "//",
        "// Vectors of structs (their size, then every element field by field):",
    ]
    head += [f"//   {s}" for s in out.lists] or ["//   (none)"]
    head += ["//", "// Credentials (printed as <set> or <empty>, never as their text):"]
    head += [f"//   {s}" for s in out.secrets] or ["//   (none)"]
    head += ["//", "// NOT PRINTED (type the dump cannot print):"]
    head += [f"//   {s}" for s in out.unprintable] or ["//   (none)"]
    head += ["//", "// NOT PRINTED (private members of GnssConfig that no getter above reads):"]
    head += [f"//   {s}" for s in hidden] or ["//   (none)"]
    head += [""]
    text = "\n".join(head + out.lines) + "\n"
    return text, review


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail when the checked-in file is stale")
    ap.add_argument("--review", action="store_true", help="list getters whose body is not a plain return")
    args = ap.parse_args()
    text, review = generate()
    if args.review:
        print("\n".join(review))
    if args.check:
        old = OUT_INC.read_text(encoding="utf-8") if OUT_INC.exists() else ""
        if old != text:
            print(f"{OUT_INC} is stale: re-run tests/support/gen_config_dump.py", file=sys.stderr)
            return 1
        print(f"{OUT_INC} is current")
        return 0
    OUT_INC.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {OUT_INC} ({text.count(chr(10))} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

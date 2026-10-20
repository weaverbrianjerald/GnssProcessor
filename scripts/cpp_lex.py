#!/usr/bin/env python3
"""A small C++ lexer for the source-reading scripts: comments, string literals, the rest.

The verifiers and linters under scripts/ read C++ source as text.  A comment must not count
as code, and a comment marker inside a string must not start a comment, so they share this
one pass instead of each rolling a regular expression.

    strip_comments(text)     the text with every comment removed, newlines kept (line numbers hold)
    string_literals(text)    the contents of the string literals of the text, comments excluded
    CPP_TOKEN                the token pattern both are built on (named groups lc, bc, raw, str,
                             chr, num, other)

It was written for the verifier of the real-time engine's seam cut (rt_seam_verify.py, retired
2026-10-17) and lives here so that the tools which kept using it do not depend on a retired one.
"""
import re

# One pass over C++ source: comments, then the literals a comment marker can hide
# in (raw, string, char), then numbers (so a digit separator is not read as a
# char literal), then anything else.
CPP_TOKEN = re.compile(r"""
 (?P<lc>//(?:\\\r?\n|[^\n])*)
|(?P<bc>/\*.*?(?:\*/|\Z))
|(?P<raw>(?:u8|u|U|L)?R"(?P<delim>[^()\\\s]{0,16})\(.*?\)(?P=delim)")
|(?P<str>(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*"?)
|(?P<chr>(?:u8|u|U|L)?'(?:[^'\\\n]|\\.)*'?)
|(?P<num>\.?\d(?:[eEpP][+-]|'[0-9A-Za-z_]|[0-9A-Za-z_.])*)
|(?P<other>[A-Za-z_]\w*|.)
""", re.X | re.S)
_CPP = CPP_TOKEN        # the name the older verifiers import


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments, keeping every newline so line numbers hold.

    A line that held only a comment becomes blank; a trailing comment leaves the
    code and its spacing.  A // comment continued by a trailing backslash covers
    the next line too, as it does for the compiler.
    """
    out = []
    for m in CPP_TOKEN.finditer(text):
        s = m.group()
        out.append("\n" * s.count("\n") if m.lastgroup in ("lc", "bc") else s)
    return "".join(out)


def string_literals(text: str):
    """The contents (between the quotes, escapes left as written) of every ordinary string
    literal in `text`.  Comments and char literals are skipped; a raw string is not returned."""
    return [m.group()[m.group().index('"') + 1:].removesuffix('"')
            for m in CPP_TOKEN.finditer(text) if m.lastgroup == "str"]

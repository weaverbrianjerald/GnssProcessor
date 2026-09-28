#!/usr/bin/env python3
"""Relocation verifier for the RealTimeEngine seam decomposition.

    python scripts/rt_seam_verify.py

This is the primary gate for a pure-relocation refactor, and for that purpose it
is stronger than the runtime A/B: the A/B certifies only the paths a fixture
executes (the seam-gate arms leave every SSR datum-break reaction body and every
TurboEdit result loop at zero iterations), while this covers all of the moved
code, including branches no fixture reaches.

Mechanic (the inverse of the extraction, per the network decomposition's
seamcut.py note): for each seam, take the relocated body out of its new .cpp by
brace matching, apply the inverse of the declared substitutions, re-indent by the
recorded dedent, and require every code line to match the recorded line range of
the frozen pre-cut source character for character, indentation included.

Comments and blank lines are ignored on both sides, so rewriting a comment in a
relocated body does not fail the gate.  They are removed before anything else
runs: the definition search, the brace matching, the preamble skip, the tail
check and the comparison all see code only.  Any other edit that is not a
declared substitution fails.

Reads docs/rt_seam_manifest.json:

  { "frozen": "runs/rtseam_goldens/pre/src/RealTimeEngine.cpp.frozen",
    "seams": [
      { "file":   "src/core/realtime/RealTimeEngineSsrBreaks.cpp",
        "func":   "RealTimeEngine::reactToGenerationChange",
        "head":   [1647, 1671],      # inclusive, 1-based, in the frozen file
        "dedent": 4,
        "skip":   3,                  # alias-preamble code lines to drop first
        "subs":   {},                 # normally empty; see below
        "tail":   "return true;" }    # optional: a declared appended statement
    ] }

Every seam opens with an alias preamble that reconstructs the driver's local
names (`auto log = ...; auto& modeled_obs = ctx.modeled_obs;`), which is why
`subs` is empty for almost every seam: the relocated body reads the original
identifiers.  `skip` counts the preamble's code lines; comment and blank lines
are not counted.  A non-empty `subs` is a declared exception and should be rare
enough to check by eye.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs" / "rt_seam_manifest.json"

# One pass over C++ source: comments, then the literals a comment marker can hide
# in (raw, string, char), then numbers (so a digit separator is not read as a
# char literal), then anything else.
_CPP = re.compile(r"""
 (?P<lc>//(?:\\\r?\n|[^\n])*)
|(?P<bc>/\*.*?(?:\*/|\Z))
|(?P<raw>(?:u8|u|U|L)?R"(?P<delim>[^()\\\s]{0,16})\(.*?\)(?P=delim)")
|(?P<str>(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*"?)
|(?P<chr>(?:u8|u|U|L)?'(?:[^'\\\n]|\\.)*'?)
|(?P<num>\.?\d(?:[eEpP][+-]|'[0-9A-Za-z_]|[0-9A-Za-z_.])*)
|(?P<other>[A-Za-z_]\w*|.)
""", re.X | re.S)


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments, keeping every newline so line numbers hold.

    A line that held only a comment becomes blank; a trailing comment leaves the
    code and its spacing.  A // comment continued by a trailing backslash covers
    the next line too, as it does for the compiler.
    """
    out = []
    for m in _CPP.finditer(text):
        s = m.group()
        out.append("\n" * s.count("\n") if m.lastgroup in ("lc", "bc") else s)
    return "".join(out)


def extract_body(path: pathlib.Path, func: str):
    """Return the function body lines (between the opening { and its match),
    with comments removed."""
    text = strip_comments(path.read_text(errors="replace")).splitlines()
    # Find the definition line: "<ret> RealTimeEngine::name(" possibly wrapped.
    short = func.split("::")[-1]
    start = None
    for i, line in enumerate(text):
        if re.search(r"\b" + re.escape(func.replace("::", "::")) + r"\s*\(", line) \
           or re.search(r"\b" + re.escape(short) + r"\s*\(", line) and "RealTimeEngine::" in line:
            start = i
            break
    if start is None:
        raise SystemExit(f"{path}: definition of {func} not found")

    # Walk to the opening brace of the body, then brace-match.
    depth, i, opened = 0, start, False
    while i < len(text):
        for ch in text[i]:
            if ch == "{":
                depth += 1; opened = True
            elif ch == "}":
                depth -= 1
        if opened and depth == 0:
            return start, i, text[start:i + 1]
        i += 1
    raise SystemExit(f"{path}: unbalanced braces in {func}")


def body_only(lines, first_lineno):
    """Return (line number, text) for the lines between the opening-brace line
    and the final closing-brace line.  @p first_lineno is the 1-based number of
    lines[0]."""
    for k, line in enumerate(lines):
        if line.rstrip().endswith("{"):
            return [(first_lineno + j, lines[j]) for j in range(k + 1, len(lines) - 1)]
    raise SystemExit("could not locate opening brace line")


def main():
    if not MANIFEST.exists():
        sys.exit(f"missing manifest: {MANIFEST}")
    m = json.loads(MANIFEST.read_text())
    frozen = strip_comments((ROOT / m["frozen"]).read_text(errors="replace")).splitlines()

    n_ok = n_bad = 0
    for seam in m["seams"]:
        func = seam["func"]
        path = ROOT / seam["file"]
        if not path.exists():
            print(f"FAIL {func}: {seam['file']} does not exist"); n_bad += 1; continue

        start, _, whole = extract_body(path, func)
        # Code lines only: comments are already gone, so this also drops the
        # lines that held nothing but a comment.
        moved = [(n, l) for n, l in body_only(whole, start + 1) if l.strip()]

        # Drop the alias preamble (declared, not inferred).
        skip = seam.get("skip", 0)
        if skip:
            if len(moved) < skip:
                print(f"FAIL {func}: skip={skip} exceeds body length {len(moved)}")
                n_bad += 1
                continue
            moved = moved[skip:]

        # The trailing statement is a declared addition, not a silent edit: drop
        # exactly the declared one before comparing.  Anything else at the tail
        # is a real edit and fails.
        tail = seam.get("tail")
        if tail:
            if moved and moved[-1][1].strip() == tail:
                moved.pop()
            else:
                last = moved[-1][1].strip() if moved else "<empty>"
                print(f"FAIL {func}: declared tail {tail!r} but last statement "
                      f"is {last!r}")
                n_bad += 1
                continue

        # Inverse substitution.  rt_seamcut.py applies subs with a plain
        # str.replace, so the inverse is a plain replace of the relocated text
        # back to the frozen text, not an identifier rename (the declared subs
        # are whole statements, e.g. a local declaration becoming a ctx write).
        # Longest replacement first so one sub cannot eat another's prefix.
        inv = sorted(seam.get("subs", {}).items(), key=lambda kv: -len(kv[1]))
        # Undo the generator's transform exactly: it emitted
        #     "    " + original[dedent:]
        # so rebuilding means stripping that 4-space function-body indent and
        # restoring the measured dedent.
        rebuilt, bad_indent = [], None
        for n, line in moved:
            for src, dst in inv:
                line = line.replace(dst, src)
            if not line.strip():
                continue
            if not line.startswith("    "):
                bad_indent = (n, line)
                break
            rebuilt.append((n, " " * seam.get("dedent", 4) + line[4:]))
        if bad_indent:
            print(f"FAIL {func}: {seam['file']}:{bad_indent[0]} lacks the 4-space "
                  f"function indent: {bad_indent[1]!r}")
            n_bad += 1
            continue

        a, b = seam["head"]
        orig = [(a + j, l) for j, l in enumerate(frozen[a - 1:b]) if l.strip()]

        # Trailing spaces are cosmetic on both sides.
        o_txt = [l.rstrip() for _, l in orig]
        r_txt = [l.rstrip() for _, l in rebuilt]
        if r_txt == o_txt:
            print(f"OK   {func}  ({len(orig)} code lines match frozen {a}-{b})")
            n_ok += 1
        else:
            n_bad += 1
            print(f"FAIL {func}  (frozen {a}-{b}): {len(orig)} original vs "
                  f"{len(rebuilt)} rebuilt code lines")
            for i in range(max(len(orig), len(rebuilt))):
                o = o_txt[i] if i < len(orig) else "<none>"
                r = r_txt[i] if i < len(rebuilt) else "<none>"
                if o != r:
                    where_o = f"frozen line {orig[i][0]}" if i < len(orig) else "frozen <end>"
                    where_r = (f"{seam['file']}:{rebuilt[i][0]}" if i < len(rebuilt)
                               else f"{seam['file']} <end>")
                    print(f"     first difference ({where_o}; {where_r}):")
                    print(f"       frozen : {o}")
                    print(f"       rebuilt: {r}")
                    break

    print(f"\n{n_ok} verbatim in code, {n_bad} FAILED (comments and blank lines ignored)")
    sys.exit(1 if n_bad else 0)


if __name__ == "__main__":
    main()

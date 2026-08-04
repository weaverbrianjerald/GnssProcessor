#!/usr/bin/env python3
"""Verbatim-relocation verifier for the RealTimeEngine seam decomposition.

    python scripts/rt_seam_verify.py

This is the PRIMARY gate for a pure-relocation refactor, and it is strictly
stronger than the runtime A/B for this purpose: the A/B certifies only the paths
a fixture happens to execute (the seam-gate arms leave every SSR datum-break
REACTION body and every TurboEdit result loop at zero iterations), whereas this
covers 100% of the moved text -- including branches no fixture reaches.

Mechanic (the inverse of the extraction, per the network decomposition's
seamcut.py note): for each seam, take the relocated body out of its new .cpp by
brace matching, apply the INVERSE of the declared substitutions, re-indent by
the recorded dedent, and assert the result is a CHARACTER-EXACT match for the
recorded line range of the frozen pre-cut source.  Any edit that is not a
declared substitution fails.

Reads docs/rt_seam_manifest.json:

  { "frozen": "runs/rtseam_goldens/pre/src/RealTimeEngine.cpp.frozen",
    "seams": [
      { "file":   "src/core/realtime/RealTimeEngineSsrBreaks.cpp",
        "func":   "RealTimeEngine::reactToGenerationChange",
        "head":   [1647, 1671],      # inclusive, 1-based, in the frozen file
        "dedent": 4,
        "skip":   3,                  # alias-preamble lines to drop first
        "subs":   {},                 # normally EMPTY -- see below
        "trailing_return": false }    # true => a `return true;` was appended
    ] }

Every seam opens with an ALIAS PREAMBLE that reconstructs the driver's local
names (`auto log = ...; auto& modeled_obs = ctx.modeled_obs;`), which is why
`subs` is empty for almost every seam: the relocated body reads the ORIGINAL
identifiers and is verbatim in the strictest sense.  `skip` is the count of
those preamble lines.  A non-empty `subs` is a declared exception and should be
rare enough to eyeball.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs" / "rt_seam_manifest.json"


def extract_body(path: pathlib.Path, func: str):
    """Return the function body lines (between the opening { and its match)."""
    text = path.read_text(errors="replace").splitlines()
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


def body_only(lines):
    """Strip the signature line(s) up to and including the opening brace line,
    and the final closing-brace line."""
    for k, line in enumerate(lines):
        if line.rstrip().endswith("{"):
            return lines[k + 1:-1]
    raise SystemExit("could not locate opening brace line")


def main():
    if not MANIFEST.exists():
        sys.exit(f"missing manifest: {MANIFEST}")
    m = json.loads(MANIFEST.read_text())
    frozen = (ROOT / m["frozen"]).read_text(errors="replace").splitlines()

    n_ok = n_bad = 0
    for seam in m["seams"]:
        func = seam["func"]
        path = ROOT / seam["file"]
        if not path.exists():
            print(f"FAIL {func}: {seam['file']} does not exist"); n_bad += 1; continue

        _, _, whole = extract_body(path, func)
        moved = body_only(whole)

        # Drop the alias preamble (declared, not inferred) plus the blank line
        # that separates it from the relocated body.
        skip = seam.get("skip", 0)
        if skip:
            if len(moved) < skip:
                print(f"FAIL {func}: skip={skip} exceeds body length {len(moved)}")
                n_bad += 1
                continue
            moved = moved[skip:]
        while moved and not moved[0].strip():
            moved.pop(0)

        # The trailing statement is a DECLARED addition, not a silent edit --
        # drop exactly the declared one (plus surrounding blanks) before
        # comparing.  Anything else at the tail is a real edit and must fail.
        tail = seam.get("tail")
        if tail:
            while moved and not moved[-1].strip():
                moved.pop()
            if moved and moved[-1].strip() == tail:
                moved.pop()
            else:
                last = moved[-1].strip() if moved else "<empty>"
                print(f"FAIL {func}: declared tail {tail!r} but last statement "
                      f"is {last!r}")
                n_bad += 1
                continue
            while moved and not moved[-1].strip():
                moved.pop()

        # Inverse substitution.  rt_seamcut.py applies subs with a plain
        # str.replace, so the inverse is a plain replace of the relocated text
        # back to the frozen text -- NOT an identifier rename (the declared subs
        # are whole statements, e.g. a local DECLARATION becoming a ctx write).
        # Longest replacement first so one sub cannot eat another's prefix.
        inv = sorted(seam.get("subs", {}).items(), key=lambda kv: -len(kv[1]))
        # Undo the generator's transform exactly: it emitted
        #     "    " + original[dedent:]
        # so rebuilding means stripping that 4-space function-body indent and
        # restoring the measured dedent.
        rebuilt = []
        for line in moved:
            for src, dst in inv:
                line = line.replace(dst, src)
            if not line.strip():
                rebuilt.append(line)
                continue
            if not line.startswith("    "):
                print(f"FAIL {func}: body line lacks the 4-space function indent: {line!r}")
                break
            rebuilt.append(" " * seam.get("dedent", 4) + line[4:])
        else:
            pass

        a, b = seam["head"]
        original = frozen[a - 1:b]

        # Trailing blank lines are cosmetic on both sides.
        norm = lambda xs: [x.rstrip() for x in xs]
        while rebuilt and not rebuilt[-1].strip():
            rebuilt.pop()
        orig = list(original)
        while orig and not orig[-1].strip():
            orig.pop()

        if norm(rebuilt) == norm(orig):
            print(f"OK   {func}  ({len(orig)} lines verbatim from frozen {a}-{b})")
            n_ok += 1
        else:
            n_bad += 1
            print(f"FAIL {func}  (frozen {a}-{b}): {len(orig)} original vs "
                  f"{len(rebuilt)} rebuilt lines")
            for i in range(max(len(orig), len(rebuilt))):
                o = norm(orig)[i] if i < len(orig) else "<none>"
                r = norm(rebuilt)[i] if i < len(rebuilt) else "<none>"
                if o != r:
                    print(f"     line {a + i}:")
                    print(f"       frozen : {o}")
                    print(f"       rebuilt: {r}")
                    break

    print(f"\n{n_ok} verbatim, {n_bad} FAILED")
    sys.exit(1 if n_bad else 0)


if __name__ == "__main__":
    main()

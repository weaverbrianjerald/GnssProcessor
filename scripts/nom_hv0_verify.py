#!/usr/bin/env python3
"""Relocation verifier for card NOM3 step HV0: app/NetworkRunContext.cpp split by concern.

    python scripts/nom_hv0_verify.py [--rev e1a9ef0] [--verbose]

The frozen source is src/core/app/NetworkRunContext.cpp at main e1a9ef0, read with `git show`
(no copy is stored).  Its top-level items inside `namespace GnssProcessor` are found by brace
matching with comments and literals blanked (the lexer of scripts/rt_seam_verify.py, as in
scripts/nom_td_verify.py): every function definition, and the anonymous namespace that holds
the one file-local helper.  An item runs from the first non-blank line after the previous item
(so a function's leading comment travels with it) to its closing brace; its head is its code
from that first line to the item's opening brace.  Exit 0 only when all four checks pass:

  1. The frozen file is accounted for.  Its items, its preamble (file comment, includes and
     the namespace opening, which every new file replaces with its own), the blank lines
     between items and the namespace close cover every line.  Outside its items it holds
     only comments, #include lines and the namespace lines, and the head of every item is
     one declarator: `namespace`, or a function signature with exactly one parameter list
     (no `;`, no preprocessor line, no second parenthesized group).  So no file-scope
     declaration, using-declaration or macro, between items or in front of one, rides along
     with a body that a new file would carry and the others would not.
  2. Bodies.  Every frozen item appears byte-exact, comments and whitespace included, as an
     item of exactly one new file, and its text occurs nowhere else in the new files (the
     checkout's CRLF, if any, is normalized to LF first; git stores LF).  The one exception
     is COMMENT_EDITS: each is applied to the frozen text of its item first, must match
     there exactly once, and must leave the item's comment-stripped text unchanged, so it
     can only edit comments.
  3. Nothing added or dropped.  The new files hold no item that is not a frozen item; each
     holds the items SPLIT assigns it, in their frozen order; outside its items a file holds
     only comments, #include lines and the namespace lines, and every item's head is one
     declarator, as in check 1.  The old file is gone.
  4. Build list and header.  CMakeLists.txt lists each new file once and never names the
     old one; every function the frozen file defines and include/app/NetworkRunContext.hpp
     declares carries, in the comment block above its declaration, exactly one line
     "Defined in src/core/app/<file>." naming the file that defines it now; and both
     statements of the three-place rule in that header (the file comment and the comment
     above the free functions) name the three files of the snapshot channel.

A body is compared as text, so the check proves where each function now lives, not what
its names refer to: the includes of each new file are new, and the compiler, not this
script, proves them sufficient.

Historical.  The check holds for the HV0 cut as it landed.  Later work edited the relocated
bodies without declaring the edits here: card NOM3 step EP0 (NetworkKnobs::snapshot) and the
retired-key deletion (card RKD: the phase hold inject, the LV-5 pre-pass, the leveling
harvest, the carry stash, the knob snapshot).  On such a tree the script reports those items
as missing; it is kept as the record of the cut, not as a gate.
"""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from rt_seam_verify import strip_comments, _CPP  # noqa: E402

FROZEN_PATH = "src/core/app/NetworkRunContext.cpp"
HEADER = "include/app/NetworkRunContext.hpp"
APP = "src/core/app/"

# The split: new file -> its items, in frozen order.  "namespace{readmitNoteSeed}" is the
# anonymous namespace holding the D2 fold's file-local helper.
SPLIT = {
    "NetworkKnobs.cpp": ["NetworkKnobs::snapshot"],
    "NetworkSnapshotFingerprint.cpp": ["networkSnapshotFingerprint"],
    "NetworkWarmResume.cpp": ["networkTryWarmResume", "networkRestoreCanonicalSignals",
                              "networkApplyRxCbLinRestore"],
    "NetworkStateSnapshotWrite.cpp": ["networkWriteStateSnapshot"],
    "NetworkBorderPrior.cpp": ["networkInjectShVtecPrior", "networkInjectPhaseHoldPrior",
                               "networkInjectPinBridgePrior", "namespace{readmitNoteSeed}",
                               "networkInjectD2CarryPrior", "networkCouplingExactPrepare",
                               "networkInjectClockOscPrior"],
    "NetworkBorderStash.cpp": ["networkRetroUraBookkeeping", "networkStashCarry"],
    "NetworkLocalCarryHarvest.cpp": ["networkRxCbDemeanPrepass", "networkHarvestLocalCarries"],
}
# Declared comment edits, item -> [(frozen text, new text)].  The review round of step HV0:
# two comments in the knob table pointed at the fingerprint as "below", which it was in the
# frozen file and is not after the split; they name its file instead.
COMMENT_EDITS = {
    "NetworkKnobs::snapshot": [
        ("// card NBP (fingerprinted below)",
         "// card NBP (fingerprinted in NetworkSnapshotFingerprint.cpp)"),
        ("Not appended to the snapshot fingerprint below: a QC knob",
         "Not appended to the snapshot fingerprint (NetworkSnapshotFingerprint.cpp): a QC knob"),
    ],
}
# The three files of the snapshot channel, which the header's three-place rule names.
THREE_PLACE = ("NetworkSnapshotFingerprint.cpp", "NetworkWarmResume.cpp",
               "NetworkStateSnapshotWrite.cpp")
NS_OPEN = re.compile(r"^namespace GnssProcessor \{\s*$")
NS_CLOSE = re.compile(r"^\}\s*//\s*namespace GnssProcessor\s*$")
INCLUDE = re.compile(r"^#\s*include\s*[<\"][^>\"]+[>\"]$")


def blank_literals(text):
    """Comments already stripped: blank string, char and raw literals, keeping newlines."""
    out = []
    for m in _CPP.finditer(text):
        s = m.group()
        if m.lastgroup in ("str", "chr", "raw"):
            out.append("".join(c if c == "\n" else " " for c in s))
        else:
            out.append(s)
    return "".join(out)


def git_show(rev, path):
    out = subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"],
                         capture_output=True, check=True)
    return out.stdout.decode("utf-8").replace("\r\n", "\n")


def read(path):
    return (ROOT / path).read_bytes().decode("utf-8").replace("\r\n", "\n")


def item_name(code_lines):
    """The name of an item from its comment-stripped lines."""
    code = "\n".join(code_lines)
    if re.match(r"\s*namespace\s*\{", code):
        inner = re.findall(r"^[A-Za-z_][\w:<>,\s\*&]*?\b([A-Za-z_]\w*)\s*\(", code, re.M)
        return "namespace{" + ",".join(inner) + "}"
    m = re.search(r"([A-Za-z_][\w:]*)\s*\(", code)
    return m.group(1) if m else "?"


def split_items(text, where):
    """(items, outside) of a file: items are dicts {name, first, last, text, head, tail}
    (0-based lines; `head` the item's (line, code) rows from its first line up to, not
    including, its opening brace, and `tail` the code after its closing brace on its last
    line, both with comments stripped and literals blanked), `outside` the 0-based indices
    of lines outside every item."""
    lines = text.split("\n")
    code = strip_comments(text).split("\n")
    blank = blank_literals("\n".join(code)).split("\n")
    opens = [i for i, l in enumerate(code) if NS_OPEN.match(l)]
    if len(opens) != 1:
        raise ValueError(f"{where}: {len(opens)} `namespace GnssProcessor {{` lines")
    depth, start, prev_end, items, close = 0, None, opens[0], [], None
    for i in range(opens[0] + 1, len(blank)):
        for j, ch in enumerate(blank[i]):
            if ch == "{":
                if depth == 0:
                    start = (i, j)
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    a = prev_end + 1
                    while not lines[a].strip():
                        a += 1
                    head = [(k, blank[k]) for k in range(a, start[0])]
                    head.append((start[0], blank[start[0]][:start[1]]))
                    items.append({"name": item_name(code[a:i + 1]), "first": a, "last": i,
                                  "text": "\n".join(lines[a:i + 1]), "head": head,
                                  "tail": blank[i][j + 1:]})
                    prev_end, start = i, None
                elif depth < 0:
                    close = i
                    break
        if close is not None:
            break
    if close is None or not NS_CLOSE.match(lines[close]):
        raise ValueError(f"{where}: no `}} // namespace GnssProcessor` closing the items")
    inside = set()
    for it in items:
        inside.update(range(it["first"], it["last"] + 1))
    outside = [i for i in range(len(lines)) if i not in inside]
    return lines, code, items, outside, opens[0], close


def outside_is_frame(lines, code, outside, ns_open, ns_close):
    """Lines outside the items: comments, blanks, #include lines, the two namespace lines.
    Any other line (a declaration, a using-declaration, a macro) is returned as code."""
    bad = []
    for i in outside:
        c = code[i].strip()
        if not c or INCLUDE.match(c) or i in (ns_open, ns_close):
            continue
        bad.append(i)
    return bad


DECLARATOR = re.compile(r"[\w:<>,\s\*&~]*?[A-Za-z_~][\w:~]*\s*")
QUALIFIERS = re.compile(r"(?:\s*\b(?:const|noexcept|override|final)\b)*\s*")


def head_problem(item):
    """None when the item's head is one declarator (`namespace`, or a function signature
    with exactly one parameter list) and nothing follows its closing brace on that line;
    else (0-based line, what rides along with the body).  A declaration, using-declaration,
    macro or preprocessor line placed between two items lands in the head of the second
    (or, on the closing line, the tail of the first), so this is where check 1 and check 3
    find it."""
    if item["tail"].strip():
        return item["last"], f"after its closing brace, code {item['tail'].strip()[:60]!r}"
    rows = item["head"]
    for k, t in rows:
        if t.strip().startswith("#"):
            return k, f"in front of its body, a preprocessor line {t.strip()[:60]!r}"
        if ";" in t:
            return k, f"in front of its body, a `;`-terminated declaration {t.strip()[:60]!r}"
    text = "\n".join(t for _, t in rows)
    if text.strip() == "namespace":
        return None
    depth, groups, lo, hi = 0, 0, None, None
    for k, ch in enumerate(text):
        if ch == "(":
            if depth == 0:
                groups += 1
                lo = k if lo is None else lo
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0 and hi is None:
                hi = k
    if groups != 1 or depth != 0:
        return rows[0][0], f"its head is not one declarator ({groups} parenthesized groups at its top level)"
    if not DECLARATOR.fullmatch(text[:lo]) or not QUALIFIERS.fullmatch(text[hi + 1:]):
        return rows[0][0], f"its head is not one declarator: {' '.join(text.split())[:80]!r}"
    return None


def main():
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rev", default="e1a9ef0", help="the frozen commit (default main e1a9ef0)")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    problems = []

    # 1. The frozen file.
    frozen = git_show(a.rev, FROZEN_PATH)
    flines, fcode, fitems, fout, fopen, fclose = split_items(frozen, f"{a.rev}:{FROZEN_PATH}")
    for i in outside_is_frame(flines, fcode, fout, fopen, fclose):
        problems.append(f"frozen line {i + 1} is code outside every item: {flines[i].strip()[:80]!r}")
    for it in fitems:
        hp = head_problem(it)
        if hp:
            problems.append(f"frozen line {hp[0] + 1}: {it['name']}: {hp[1]}")
    after = [i for i in fout if i > fclose and flines[i].strip()]
    if after:
        problems.append(f"frozen lines after the namespace close: {[i + 1 for i in after]}")
    fnames = [it["name"] for it in fitems]
    want = [n for f in SPLIT for n in SPLIT[f]]
    if sorted(fnames) != sorted(want):
        problems.append(f"the frozen items {fnames} are not the items SPLIT assigns {want}")
    n_frozen_lines = sum(it["last"] - it["first"] + 1 for it in fitems)
    print(f"frozen {a.rev}:{FROZEN_PATH}: {len(flines) - 1} lines, {len(fitems)} items "
          f"({n_frozen_lines} lines), preamble lines 1-{fopen + 1}, namespace close {fclose + 1}")

    # 2 and 3. The new files.
    if (ROOT / FROZEN_PATH).exists():
        problems.append(f"{FROZEN_PATH} still exists")
    new_text, new_items = {}, {}
    for f, names in SPLIT.items():
        path = APP + f
        if not (ROOT / path).exists():
            problems.append(f"{path} is missing")
            continue
        text = read(path)
        new_text[f] = text
        lines, code, items, out, nopen, nclose = split_items(text, path)
        new_items[f] = items
        for i in outside_is_frame(lines, code, out, nopen, nclose):
            problems.append(f"{path}:{i + 1} is code outside every item: {lines[i].strip()[:80]!r}")
        for it in items:
            hp = head_problem(it)
            if hp:
                problems.append(f"{path}:{hp[0] + 1}: {it['name']}: {hp[1]}")
        got = [it["name"] for it in items]
        if got != names:
            problems.append(f"{path} holds {got}, SPLIT assigns {names} (order included)")
    by_frozen = {it["name"]: it for it in fitems}

    # The text each frozen item must have in its new file: the frozen text, with the
    # declared comment edits applied and proved comment-only.
    expected = {it["name"]: it["text"] for it in fitems}
    n_edits = 0
    for name, edits in COMMENT_EDITS.items():
        if name not in expected:
            problems.append(f"COMMENT_EDITS names {name}, which is not a frozen item")
            continue
        text = expected[name]
        for old, new in edits:
            n = text.count(old)
            if n != 1:
                problems.append(f"COMMENT_EDITS {name}: {old!r} occurs {n} times in the frozen "
                                f"text, expected once")
                continue
            text = text.replace(old, new)
            n_edits += 1
        if strip_comments(text) != strip_comments(expected[name]):
            problems.append(f"COMMENT_EDITS {name}: the edits change code, not only comments")
        expected[name] = text

    n_ok = 0
    for f, items in new_items.items():
        for it in items:
            fz = by_frozen.get(it["name"])
            if fz is None:
                problems.append(f"{APP + f}: item {it['name']} is not in the frozen file (added)")
                continue
            if it["text"] != expected[it["name"]]:
                fl, nl = expected[it["name"]].split("\n"), it["text"].split("\n")
                k = next((k for k in range(min(len(fl), len(nl))) if fl[k] != nl[k]),
                         min(len(fl), len(nl)))
                problems.append(f"{APP + f}: {it['name']} differs from the frozen text at its line "
                                f"{k + 1} (frozen {fz['first'] + k + 1}):\n"
                                f"       frozen: {fl[k] if k < len(fl) else '<end>'!r}\n"
                                f"       new   : {nl[k] if k < len(nl) else '<end>'!r}")
                continue
            n_ok += 1
            if a.verbose:
                print(f"  {it['name']:36s} frozen {fz['first'] + 1:5d}-{fz['last'] + 1:5d} -> "
                      f"{APP + f}:{it['first'] + 1}-{it['last'] + 1}")
    for fz in fitems:
        hits = sum(t.count(expected[fz["name"]]) for t in new_text.values())
        if hits != 1:
            problems.append(f"{fz['name']}: its frozen text occurs {hits} times in the new files")

    # 4. CMakeLists.txt and the header.
    cm = read("CMakeLists.txt")
    if FROZEN_PATH in cm:
        problems.append(f"CMakeLists.txt still names {FROZEN_PATH}")
    for f in SPLIT:
        n = len(re.findall(r"^\s*" + re.escape(APP + f) + r"\s*$", cm, re.M))
        if n != 1:
            problems.append(f"CMakeLists.txt lists {APP + f} {n} times")
    hdr = read(HEADER).split("\n")
    home = {n: f for f, ns in SPLIT.items() for n in ns}
    n_decl = 0
    for name, f in home.items():
        if name.startswith("namespace{"):
            continue
        short = name.split("::")[-1]
        pat = (re.compile(r"^\s*static NetworkKnobs snapshot\(") if name == "NetworkKnobs::snapshot"
               else re.compile(r"^\s*void " + re.escape(short) + r"\("))
        decl = [i for i, l in enumerate(hdr) if pat.match(l)]
        if len(decl) != 1:
            problems.append(f"{HEADER}: {len(decl)} declarations of {name}")
            continue
        n_decl += 1
        doc, i = [], decl[0] - 1
        while i >= 0 and re.match(r"^\s*(//|/\*|\*)", hdr[i]):
            doc.append(hdr[i])
            i -= 1
        line = f"Defined in {APP}{f}."
        named = [d for d in doc if "Defined in " in d]
        if len(named) != 1 or line not in named[0]:
            problems.append(f"{HEADER}:{decl[0] + 1}: {name} does not carry {line!r} "
                            f"(found {[d.strip() for d in named]})")

    rules = [i for i, l in enumerate(hdr) if re.search(r"\b[Tt]hree-place rule:", l)]
    if len(rules) != 2:
        problems.append(f"{HEADER}: {len(rules)} statements of the three-place rule, expected 2")
    for i in rules:
        window = " ".join(hdr[i:i + 6])
        missing = [f for f in THREE_PLACE if f not in window]
        if missing:
            problems.append(f"{HEADER}:{i + 1}: the three-place rule does not name {missing}")

    for p in problems:
        print("FAIL " + p)
    n_new = sum(len(v) for v in new_items.values())
    print(f"{n_ok} of {len(fitems)} frozen items byte-exact in {len(new_items)} new files "
          f"({n_new} items there), {len(COMMENT_EDITS)} of them after {n_edits} declared "
          f"comment-only edits; {n_decl} header declarations and {len(rules)} three-place "
          f"statements checked; "
          f"{'PASS' if not problems else 'FAIL'}")
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Drop the "Network" prefix from the file names inside the network tree.

Scope: every tracked file under include/network/ and src/core/network/ whose basename
starts with "Network" followed by an upper-case letter (NetworkRaim.hpp -> Raim.hpp).
Only file names change. Type, function and variable names do not, and a file stays in its
folder, so a path edit is always a basename edit. Files named ApplicationNetwork*.cpp keep
their names (they define Application members), and tests under tests/network/ keep theirs.

The table is scripts/netdir_rename.tsv (old path, new path, tab separated). Subcommands:
  --emit            derive the table from the tree and write it. A rename is refused (listed,
                    left out of the table) when the new path already exists or when another
                    rename in the same folder produces the same new basename.
  --apply           git mv every row, then rewrite every mention of an old basename (with its
                    extension) in the tracked C++ files and CMakeLists.txt: #include strings by
                    exact match, comments and Doxygen, and string literals. Text outside those
                    three places that names an old basename is reported and left alone.
                    --reverse-sed PATH writes the sed script that maps each new basename that
                    now appears in a production string literal back to its old spelling.
  --rewrite-refs F  rewrite basename mentions (with extension, and the extension-less
                    network/<concern>/<stem> form) in the given docs and scripts, as plain text.
  --check           prove the result against a base revision: each changed line equals its base
                    line once the new basenames are mapped back, no old basename is left in the
                    live scope, and every moved file is present under its new name.
  --stems           list the bare stems (no extension) that still appear in comments, split by
                    whether the stem is also a code identifier.

The base revision for --check is netdir/move (--base overrides).
"""
import argparse
import bisect
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = ROOT / "scripts" / "netdir_rename.tsv"
TREES = ("include/network/", "src/core/network/")
CODE_EXT = (".hpp", ".cpp", ".h", ".c", ".cc", ".inl")
NAME_FULL = re.compile(r"^Network([A-Z][A-Za-z0-9_]*)(\.[A-Za-z]+)$")
BOUND_L = r"(?<![A-Za-z0-9_])"
BOUND_R = r"(?![A-Za-z0-9_])"
INC_LINE = re.compile(r"[ \t]*#[ \t]*include[ \t]*")


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True,
                          capture_output=True).stdout.decode("utf-8", "replace")


def tracked():
    return git("ls-files").splitlines()


def read_table():
    return [tuple(l.split("\t")) for l in TABLE.read_text().splitlines() if l.strip()]


def base_of(path):
    return path.rsplit("/", 1)[-1]


def inc_string(path):
    return path.split("/", 1)[1] if path.startswith("include/") and \
        path.endswith((".hpp", ".h", ".inl")) else None


def name_regex(names):
    """Whole-word regex over a set of basenames (longest first)."""
    alt = "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True))
    return re.compile(BOUND_L + "(?:" + alt + ")" + BOUND_R)


def read_text(path):
    """Text with a 1:1 byte mapping, so a rewrite never changes an untouched byte."""
    return (ROOT / path).read_bytes().decode("latin-1")


def write_text(path, text):
    (ROOT / path).write_bytes(text.encode("latin-1"))


# ---------------------------------------------------------------- C++ lexical segments
TOKEN = re.compile(r'//|/\*|(?<![A-Za-z0-9_])(?:u8|[uUL])?R"([^()\\\s"]{0,16})\(|"|\'')
STRING = re.compile(r'"(?:[^"\\\n]|\\[\s\S])*"?')
CHARLIT = re.compile(r"'(?:[^'\\\n]|\\[\s\S])*'?")


def segments(text):
    """Non-code segments of C++ text: (start, end, kind) with kind 'c' (comment), 's'
    (string or character literal) or 'i' (the string of a quoted #include)."""
    out = []
    pos = 0
    n = len(text)
    while pos < n:
        m = TOKEN.search(text, pos)
        if not m:
            break
        tok = m.group(0)
        s = m.start()
        if tok == "//":
            e = s
            while True:
                e = text.find("\n", e)
                if e < 0:
                    e = n
                    break
                k = e - 1
                if k > s and text[k] == "\r":
                    k -= 1
                if k > s and text[k] == "\\":
                    e += 1
                    continue
                break
            out.append((s, e, "c"))
            pos = e
        elif tok == "/*":
            e = text.find("*/", s + 2)
            e = n if e < 0 else e + 2
            out.append((s, e, "c"))
            pos = e
        elif tok == '"':
            ls = text.rfind("\n", 0, s) + 1
            kind = "i" if INC_LINE.fullmatch(text, ls, s) else "s"
            e = STRING.match(text, s).end()
            out.append((s, e, kind))
            pos = e
        elif tok == "'":
            j = s - 1
            while j >= 0 and (text[j].isalnum() or text[j] == "_"):
                j -= 1
            if s > 0 and text[s - 1].isalnum() and text[j + 1].isdigit():
                pos = s + 1                     # digit separator, not a character literal
                continue
            e = CHARLIT.match(text, s).end()
            out.append((s, e, "s"))
            pos = e
        else:                                   # raw string literal
            close = ")" + m.group(1) + '"'
            e = text.find(close, m.end())
            e = n if e < 0 else e + len(close)
            out.append((s, e, "s"))
            pos = e
    return out


def newline_index(text):
    return [i for i, ch in enumerate(text) if ch == "\n"]


# ---------------------------------------------------------------- table
def build_rows():
    files = tracked()
    existing = {f.lower() for f in files}
    rows, refused = [], []
    for f in sorted(files):
        if not f.startswith(TREES):
            continue
        d, name = f.rsplit("/", 1)
        if not name.startswith("Network"):
            continue
        m = NAME_FULL.match(name)
        if not m:
            refused.append((f, "no upper-case letter after the prefix"))
            continue
        rows.append((f, d + "/" + m.group(1) + m.group(2)))
    news = {}
    for o, n in rows:
        news.setdefault(n.lower(), []).append(o)
    keep = []
    for o, n in rows:
        if n.lower() in existing:
            refused.append((o, "new path exists: " + n))
        elif len(news[n.lower()]) > 1:
            refused.append((o, "same new basename as " + ", ".join(
                x for x in news[n.lower()] if x != o)))
        else:
            keep.append((o, n))
    return keep, refused


def cmd_emit():
    rows, refused = build_rows()
    TABLE.write_text("".join(f"{o}\t{n}\n" for o, n in rows), newline="\n")
    print(len(rows), "rows;", len(refused), "refused")
    for f, why in refused:
        print("REFUSED:", f, "-", why)
    olds = [base_of(o) for o, _ in rows]
    dup = {b for b in olds if olds.count(b) > 1}
    others = {base_of(f) for f in tracked()} - set(olds)
    for b in sorted(dup):
        print("AMBIGUOUS old basename (in several rows):", b)
    for b in sorted(set(olds) & others):
        print("AMBIGUOUS old basename (also an untouched file):", b)


def maps(rows):
    """(forms, inc_map): forms maps every old spelling to its new one (a basename with its
    extension, or the extension-less network/<concern>/<stem> path); inc_map maps the
    #include string of each renamed header."""
    forms = {base_of(o): base_of(n) for o, n in rows}
    forms.update(stem_forms(rows))
    inc_map = {inc_string(o): inc_string(n) for o, n in rows if inc_string(o)}
    return forms, inc_map


# ---------------------------------------------------------------- C++ rewrite
def rewrite_cpp(text, forms, inc_map, name_re):
    """Rewrite the old basenames found in comments, strings and #include strings.
    Returns (new text, {'i': lines, 'c': lines, 's': lines}, flagged)."""
    segs = segments(text)
    starts = [s for s, _, _ in segs]
    nl = None
    lines = {"i": set(), "c": set(), "s": set()}
    flagged = []
    out, last = [], 0
    for m in name_re.finditer(text):
        k = bisect.bisect_right(starts, m.start()) - 1
        kind = None
        if k >= 0 and segs[k][0] <= m.start() < segs[k][1]:
            kind = segs[k][2]
        if nl is None:
            nl = newline_index(text)
        line = bisect.bisect_left(nl, m.start()) + 1
        new = forms[m.group(0)]
        if kind == "i":
            s0, e0, _ = segs[k]
            body = text[s0 + 1:e0 - 1]
            if body not in inc_map:
                flagged.append((line, "include string not in the table: " + body))
                continue
        elif kind is None:
            flagged.append((line, "outside comments and strings: " + m.group(0)))
            continue
        out.append(text[last:m.start()])
        out.append(new)
        last = m.end()
        lines[kind].add(line)
    out.append(text[last:])
    return "".join(out), lines, flagged


def cmd_apply(reverse_sed):
    rows = read_table()
    for o, n in rows:
        if (ROOT / n).exists() or not (ROOT / o).exists():
            raise SystemExit(f"cannot move {o} -> {n}")
    for o, n in rows:
        git("mv", o, n)
    print("moved", len(rows))
    forms, inc_map = maps(rows)
    name_re = name_regex(forms)
    per = {"i": 0, "c": 0, "s": 0}
    prod_strings, test_strings = 0, 0
    prod_string_names = set()
    changed, bad = 0, 0
    for f in tracked():
        if f.startswith("third_party/") or not f.endswith(CODE_EXT):
            continue
        text = read_text(f)
        out, lines, flagged = rewrite_cpp(text, forms, inc_map, name_re)
        for ln, why in flagged:
            print(f"FLAGGED {f}:{ln}: {why}")
            bad += 1
        if out != text:
            write_text(f, out)
            changed += 1
        for k in per:
            per[k] += len(lines[k])
        if lines["s"]:
            if f.startswith("tests/"):
                test_strings += len(lines["s"])
            else:
                prod_strings += len(lines["s"])
                segs = segments(text)
                for s0, e0, kind in segs:
                    if kind == "s":
                        for m in name_re.finditer(text, s0, e0):
                            prod_string_names.add(m.group(0))
    print(f"code files changed: {changed}")
    print(f"lines edited: include {per['i']}, comment {per['c']}, string {per['s']}")
    print(f"string lines: production {prod_strings}, tests {test_strings}")
    p = ROOT / "CMakeLists.txt"
    text = read_text("CMakeLists.txt")
    out, k = name_re.subn(lambda m: forms[m.group(0)], text)
    write_text("CMakeLists.txt", out)
    print("CMakeLists.txt basename edits:", k)
    if reverse_sed:
        write_reverse_sed(Path(reverse_sed), forms, prod_string_names)
    return 1 if bad else 0


def write_reverse_sed(path, forms, names):
    pairs = sorted(((forms[o], o) for o in names), key=lambda p: (-len(p[0]), p[0]))
    lines = ["# Reverse of the file-name edits inside production string literals:",
             "# each new basename maps back to its old spelling (sed -E -f reverse.sed).",
             "# Longest names first; every pattern is a whole word with its extension."]
    for new, old in pairs:
        lines.append("s/\\b" + re.escape(new).replace("/", "\\/") + "\\b/" + old.replace("\\", "\\\\") + "/g")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", newline="\n")
    print(f"reverse sed: {len(pairs)} names -> {path}")


# ---------------------------------------------------------------- plain-text references
def stem_forms(rows):
    """network/<concern>/NetworkStem -> network/<concern>/Stem (both separators)."""
    out = {}
    for o, n in rows:
        rel = o.split("/network/", 1)[1] if "/network/" in o else None
        if rel is None:
            continue
        rel = "network/" + rel.rsplit(".", 1)[0]
        new = "network/" + n.split("/network/", 1)[1].rsplit(".", 1)[0]
        out[rel] = new
        out[rel.replace("/", "\\")] = new.replace("/", "\\")
    return out


def cmd_rewrite_refs(paths):
    forms, _ = maps(read_table())
    name_re = name_regex(forms)
    for f in paths:
        text = read_text(f)
        out, k = name_re.subn(lambda m: forms[m.group(0)], text)
        if out != text:
            write_text(f, out)
        print(f"{k:4d}  {f}")


# ---------------------------------------------------------------- bare stems
def cmd_stems():
    rows = read_table()
    stems = {base_of(o).rsplit(".", 1)[0] for o, _ in rows}
    ident = set()
    in_comment = {}
    stem_re = re.compile(BOUND_L + "(" + "|".join(sorted(map(re.escape, stems), key=len,
                                                           reverse=True)) + ")" + BOUND_R)
    word = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
    for f in tracked():
        if f.startswith("third_party/") or not f.endswith(CODE_EXT):
            continue
        text = read_text(f)
        segs = segments(text)
        pos = 0
        for s0, e0, kind in segs + [(len(text), len(text), "x")]:
            for m in word.finditer(text, pos, s0):
                if m.group(0) in stems:
                    ident.add(m.group(0))
            pos = e0
            if kind in ("c", "s"):
                for m in stem_re.finditer(text, s0, e0):
                    in_comment.setdefault((m.group(1), kind), []).append(f)
    for kind, label in (("c", "comments"), ("s", "strings")):
        print(f"# bare stems in {label}")
        for (stem, k), fs in sorted(in_comment.items()):
            if k == kind:
                print(f"  {'IDENT' if stem in ident else 'file-only':9s} {stem:36s} {len(fs):4d} "
                      f"in {len(set(fs))} files")


# ---------------------------------------------------------------- check
def show(rev, path):
    return subprocess.run(["git", "-C", str(ROOT), "show", f"{rev}:{path}"], check=True,
                          capture_output=True).stdout.decode("latin-1")


def norm(text):
    return text.replace("\r\n", "\n").split("\n")


def cmd_check(base, live):
    rows = read_table()
    forms, inc_map = maps(rows)
    new_to_old = {v: k for k, v in forms.items()}
    new_re = name_regex(new_to_old)
    old_re = name_regex(forms)
    bad = 0
    for o, n in rows:
        if (ROOT / o).exists() or not (ROOT / n).exists():
            print("MOVE INCOMPLETE:", o)
            bad += 1
    moved_new = {n for _, n in rows}
    old_of = {n: o for o, n in rows}
    changed = [f for f in git("diff", "--name-only", "--no-renames", base).splitlines()
               if (ROOT / f).exists()]
    cats = {"include": 0, "comment": 0, "string": 0, "other": 0}
    nfiles = 0
    for f in sorted(set(changed) | moved_new):
        src = old_of.get(f, f)
        try:
            a, b = show(base, src), read_text(f)
        except subprocess.CalledProcessError:
            continue
        la, lb = norm(a), norm(b)
        if len(la) != len(lb):
            print("LINE COUNT DIFFERS:", f)
            bad += 1
            continue
        segs = starts = None
        nlines = 0
        off = 0
        offsets = []
        for ln in lb:
            offsets.append(off)
            off += len(ln) + 1
        for i, (x, y) in enumerate(zip(la, lb)):
            if x == y:
                continue
            nlines += 1
            if new_re.sub(lambda m: new_to_old[m.group(0)], y) != x:
                print(f"EDIT BEYOND BASENAMES: {f}:{i + 1}")
                bad += 1
                continue
            if f.endswith(CODE_EXT):
                if segs is None:
                    text = "\n".join(lb)
                    segs = segments(text)
                    starts = [s for s, _, _ in segs]
                kinds = set()
                for m in new_re.finditer(y):
                    p = offsets[i] + m.start()
                    k = bisect.bisect_right(starts, p) - 1
                    kinds.add(segs[k][2] if k >= 0 and segs[k][0] <= p < segs[k][1] else "x")
                kinds = kinds or {"x"}
                if "x" in kinds:
                    print(f"EDIT IN CODE: {f}:{i + 1}")
                    bad += 1
                cats["include" if "i" in kinds else "string" if "s" in kinds else
                     "comment" if "c" in kinds else "other"] += 1
            else:
                cats["other"] += 1
        nfiles += 1 if nlines else 0
    print(f"{len(rows)} renames; files with edited lines: {nfiles}; lines: "
          + ", ".join(f"{k} {v}" for k, v in cats.items()))
    stale = 0
    scope = [f for f in tracked() if (f.endswith(CODE_EXT) or f == "CMakeLists.txt"
                                      or f in live) and not f.startswith("third_party/")]
    for f in scope:
        for i, line in enumerate(norm(read_text(f)), 1):
            if old_re.search(line):
                print(f"OLD BASENAME LEFT: {f}:{i}: {line.strip()[:110]}")
                stale += 1
    print("old-basename references left in the scope:", stale)
    return 1 if (bad or stale) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    for a in ("emit", "apply", "check", "stems"):
        g.add_argument("--" + a, action="store_true")
    g.add_argument("--rewrite-refs", nargs="+", metavar="FILE")
    ap.add_argument("--base", default="netdir/move")
    ap.add_argument("--reverse-sed", default=None)
    ap.add_argument("--live", nargs="*", default=[], help="extra files --check scans")
    a = ap.parse_args()
    if a.emit:
        cmd_emit()
    elif a.apply:
        sys.exit(cmd_apply(a.reverse_sed))
    elif a.stems:
        cmd_stems()
    elif a.rewrite_refs:
        cmd_rewrite_refs(a.rewrite_refs)
    else:
        sys.exit(cmd_check(a.base, set(a.live)))


if __name__ == "__main__":
    main()

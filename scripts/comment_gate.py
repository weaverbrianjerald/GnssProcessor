#!/usr/bin/env python3
"""Gate for comment-only rewrites of C++ and Python sources.

    python scripts/comment_gate.py rank [--top N]
    python scripts/comment_gate.py snapshot FILE...
    python scripts/comment_gate.py check FILE...
    python scripts/comment_gate.py lint FILE [--lines A-B] [--all]
    python scripts/comment_gate.py read FILE A B
    python scripts/comment_gate.py splice FILE A B NEWFILE SHA

`snapshot` stores a pre-image of each file under .git/comment_gate/pre/ (never
committed).  `read` prints lines A..B and the sha of that range; `splice`
replaces lines A..B with the text of NEWFILE only if the range still has that
sha and the result passes the code check, working on bytes so encoding, BOM and
line endings outside the range are untouched.  `check` compares each file
against its pre-image, or against HEAD when no pre-image exists, and fails when:

  * the code changed.  C++ is compared as a token stream with comments removed
    (string literals exact, preprocessor directive boundaries kept); Python is
    compared as an AST with docstring text blanked.
  * a comment lost a hard reference: a config knob key (the vocabulary is read
    from the JSON keys the config loaders accept) or a docs reference (a docs/
    path, a *.md name, a SPEC_* stem).  docs index tooling counts these.
  * the file's byte-level form changed: BOM, line-ending style, or any byte
    sequence that is not valid UTF-8.

It also reports, without failing, the soft facts the comments no longer carry
(numbers, section signs, labels, event names, file:line pointers) and the style
counts before and after.  `lint` lists the comment blocks that still carry the
old style: em dashes, all-caps emphasis, warning glyphs, second person,
narrative and dramatic wording, and blocks that open with a card or step label.
"""
import argparse
import ast
import io
import os
import re
import subprocess
import sys
import tokenize
from collections import Counter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PRE_DIR = os.path.join(ROOT, ".git", "comment_gate", "pre")
SCAN_DIRS = ("src", "include", "tests", "tools", "python", "scripts", "pyviz")
CPP_EXT = (".cpp", ".hpp", ".h", ".cc", ".cxx")
PY_EXT = (".py",)

# --------------------------------------------------------------------------- C++

_CPP = re.compile(r"""
 (?P<splice>\\\r?\n)
|(?P<nl>\n)
|(?P<ws>[ \t\r\f\v]+)
|(?P<lc>//(?:\\\r?\n|[^\n])*)
|(?P<bc>/\*.*?(?:\*/|\Z))
|(?P<raw>(?:u8|u|U|L)?R"(?P<delim>[^()\\\s]{0,16})\(.*?\)(?P=delim)")
|(?P<str>(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*"?)
|(?P<chr>(?:u8|u|U|L)?'(?:[^'\\\n]|\\.)*'?)
|(?P<num>\.?\d(?:[eEpP][+-]|'[0-9A-Za-z_]|[0-9A-Za-z_.])*)
|(?P<id>[A-Za-z_$][A-Za-z0-9_$]*)
|(?P<op>>>=|<<=|->\*|\.\.\.|<=>|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||::|\+=|-=|\*=|/=|%=|&=|\|=|\^=|\#\#|\.\*)
|(?P<p>.)
""", re.X | re.S)


def cpp_layout(text):
    """Code lines with comments removed and trailing blanks stripped, each prefixed
    with "+" when a blank line separates it from the previous code line.

    Equal layouts mean no code line was re-indented or re-spaced and no blank line
    between two code lines was added or dropped, which the token comparison alone
    does not see.  Comment-only lines and blank lines inside a comment block do not
    count, so comments can be reflowed or moved freely.
    """
    text = text.replace("\r\n", "\n")
    spans = []
    lex_cpp(text, spans)
    parts, pos = [], 0
    for a, b in spans:
        parts.append(text[pos:a])
        parts.append("\n" * text.count("\n", a, b))   # keep the line structure
        pos = b
    parts.append(text[pos:])
    stripped = "".join(parts).split("\n")
    original = text.split("\n")
    # Lines that a comment touches are never blank separators, even when empty
    # (an empty line inside a /* */ block).
    import bisect
    nl = [i for i, ch in enumerate(text) if ch == "\n"]
    in_comment = bytearray(len(original))
    for a, b in spans:
        for ln in range(bisect.bisect_right(nl, a - 1), bisect.bisect_right(nl, b - 1) + 1):
            in_comment[ln] = 1
    out, saw_blank = [], False
    for ln, (code, orig) in enumerate(zip(stripped, original)):
        if code.strip():
            out.append(("+" if saw_blank and out else "") + code.rstrip())
            saw_blank = False
        elif not orig.strip() and not in_comment[ln]:
            saw_blank = True
    return out


def lex_cpp(text, spans=None):
    """Return (tokens, comments).

    tokens: code tokens with comments removed; literals are kept exact and each
    preprocessor directive is bracketed by <BOD>/<EOD> so a comment edit that
    moves a directive boundary is caught.
    comments: list of (first_line, last_line, text, trailing) where trailing is
    True when code precedes the comment on its first line.
    """
    toks, comments = [], []
    line, at_bol, in_dir, code_on_line = 1, True, False, False
    for m in _CPP.finditer(text):
        kind, s = m.lastgroup, m.group()
        if kind == "delim":
            kind = "raw"
        if kind == "splice":
            line += 1
            continue
        if kind == "nl":
            if in_dir:
                toks.append("<EOD>")
                in_dir = False
            line += 1
            at_bol, code_on_line = True, False
            continue
        if kind == "ws":
            continue
        if kind in ("lc", "bc"):
            n = s.count("\n")
            comments.append((line, line + n, s, code_on_line))
            if spans is not None:
                spans.append((m.start(), m.end()))
            line += n
            continue
        if kind == "p" and s == "#" and at_bol:
            toks.append("<BOD>")
            in_dir, at_bol, code_on_line = True, False, True
            continue
        if kind in ("raw", "str", "chr"):
            toks.append(("L", s))
            line += s.count("\n")
        else:
            toks.append(s)
        at_bol, code_on_line = False, True
    if in_dir:
        toks.append("<EOD>")
    return toks, comments


def cpp_comment_text(raw):
    """Strip comment markers, keep the prose."""
    if raw.startswith("//"):
        body = raw[2:]
        body = body.lstrip("/!<")
        return body
    body = raw[2:-2] if raw.endswith("*/") else raw[2:]
    body = body.lstrip("*!<")
    return "\n".join(re.sub(r"^\s*\*+ ?", "", l) for l in body.split("\n"))

# ------------------------------------------------------------------------ Python


def _docstring_nodes(tree):
    out = []
    for node in ast.walk(tree):
        if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
            body = getattr(node, "body", [])
            if body and isinstance(body[0], ast.Expr) and isinstance(getattr(body[0], "value", None), ast.Constant) \
                    and isinstance(body[0].value.value, str):
                out.append(body[0].value)
    return out


def py_code_fingerprint(text):
    tree = ast.parse(text)
    for c in _docstring_nodes(tree):
        c.value = "<doc>"
    return ast.dump(tree)


def lex_py(text):
    """Return comments as (first_line, last_line, text, trailing) incl. docstrings."""
    comments = []
    try:
        tree = ast.parse(text)
        for c in _docstring_nodes(tree):
            seg = ast.get_source_segment(text, c) or ""
            comments.append((c.lineno, c.end_lineno, seg, False))
    except SyntaxError:
        pass
    try:
        last_code_line = 0
        for tok in tokenize.generate_tokens(io.StringIO(text).readline):
            if tok.type == tokenize.COMMENT:
                comments.append((tok.start[0], tok.start[0], tok.string, last_code_line == tok.start[0]))
            elif tok.type not in (tokenize.NL, tokenize.NEWLINE, tokenize.INDENT, tokenize.DEDENT,
                                  tokenize.ENCODING, tokenize.ENDMARKER):
                last_code_line = tok.end[0]
    except (tokenize.TokenError, IndentationError, SyntaxError):
        pass
    comments.sort()
    return comments


def py_comment_text(raw):
    if raw.startswith("#"):
        return raw.lstrip("#")
    return raw.strip("\"'rbuRBU")

# ------------------------------------------------------------------------ shared


def read(path):
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        return f.read()


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def byte_form(b):
    """BOM, line-ending style and the invalid-UTF-8 byte runs of a file."""
    crlf = b.count(b"\r\n")
    lf = b.count(b"\n") - crlf
    eol = "mixed" if crlf and lf else ("crlf" if crlf else "lf")
    bad, i = [], 0
    while i < len(b):
        try:
            b[i:].decode("utf-8")
            break
        except UnicodeDecodeError as e:
            bad.append(b[i + e.start: i + e.end])
            i += e.end
    return b.startswith(b"\xef\xbb\xbf"), eol, sorted(bad)


def same_code(path, old_text, new_text):
    """(same, detail) for the code outside comments."""
    if path.endswith(CPP_EXT):
        to, _ = lex_cpp(old_text)
        tn, _ = lex_cpp(new_text)
        if to == tn:
            lo, ln = cpp_layout(old_text), cpp_layout(new_text)
            if lo == ln:
                return True, ""
            i = next((j for j, (x, y) in enumerate(zip(lo, ln)) if x != y), min(len(lo), len(ln)))
            was = lo[i] if i < len(lo) else "<end>"
            now = ln[i] if i < len(ln) else "<end>"
            return False, (" (layout only: a code line was re-indented or re-spaced, or a blank"
                           " line between code lines was added or dropped; '+' marks a blank"
                           f" line before the code line)\n      was: {was!r}\n      now: {now!r}")
        i = next((j for j, (x, y) in enumerate(zip(to, tn)) if x != y), min(len(to), len(tn)))
        ctx = lambda ts: " ".join(str(x[1] if isinstance(x, tuple) else x) for x in ts[max(0, i - 8): i + 8])
        return False, f"\n      was: {ctx(to)}\n      now: {ctx(tn)}"
    try:
        if py_code_fingerprint(old_text) == py_code_fingerprint(new_text):
            return True, ""
        return False, "\n      AST differs (code outside comments/docstrings changed)"
    except SyntaxError as e:
        return False, f"\n      syntax error: {e}"


def sha(b):
    import hashlib
    return hashlib.sha1(b).hexdigest()[:12]


def rel(path):
    return os.path.relpath(os.path.abspath(path), ROOT).replace("\\", "/")


def baseline_bytes(path):
    """Pre-image if one was snapshotted, else HEAD.  Returns (bytes, source_label)."""
    pre = os.path.join(PRE_DIR, rel(path))
    if os.path.isfile(pre):
        return read_bytes(pre), "snapshot"
    r = subprocess.run(["git", "-C", ROOT, "show", "HEAD:" + rel(path)],
                       capture_output=True)
    if r.returncode != 0:
        return None, "none"
    return r.stdout, "HEAD"


def baseline(path):
    b, src = baseline_bytes(path)
    return (None if b is None else b.decode("utf-8", errors="replace")), src


def comments_of(path, text):
    if path.endswith(CPP_EXT):
        return [(a, b, cpp_comment_text(t), tr) for a, b, t, tr in lex_cpp(text)[1]]
    return [(a, b, py_comment_text(t), tr) for a, b, t, tr in lex_py(text)]

# --------------------------------------------------------------------- facts


_KNOBS = None


def knob_vocab():
    """Every dotted JSON key the config loaders read, e.g. network.raim_k."""
    global _KNOBS
    if _KNOBS is None:
        keys = set()
        cfg = os.path.join(ROOT, "src", "core", "config")
        for fn in os.listdir(cfg):
            if fn.endswith(".cpp"):
                keys |= set(re.findall(r'"([a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+)"', read(os.path.join(cfg, fn))))
        _KNOBS = {k for k in keys if not re.search(r"\.(json|csv|md|txt|rnx|sp3|clk|bia|erp|atx|nc|gz)$", k)}
    return _KNOBS


_DOCREF = re.compile(r"docs/[\w./-]+|\b[\w-]+\.md\b|\bSPEC_\w+|\bCONFIG_promotion_ledger\b")
_SOFT = {
    "number": re.compile(r"(?<![\w.])[-+−]?\d+(?:\.\d+)?(?:[eE][-+−]?\d+)?(?![\w.])"),
    "section": re.compile(r"§\s*[\w.()]+"),
    "label": re.compile(r"\b(?:[A-Z][A-Z0-9]{0,5}-\d+[a-z]?|[Cc]ard[ -][A-Za-z0-9][\w-]*)\b"),
    "event": re.compile(r"\b[A-Z][A-Z0-9]+(?:_[A-Z0-9]+)+\b"),
    "fileref": re.compile(r"\b[\w-]+\.(?:cpp|hpp|h|py|ps1|json|csv)(?::\d+(?:-\d+)?)?\b"),
    "invariant": re.compile(r"invariant\s*#?\s*\d+"),
}


def facts(comment_text):
    knobs = set()
    vocab = knob_vocab()
    for k in re.findall(r"\b[a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+\b", comment_text):
        if k in vocab:
            knobs.add(k)
    docs = set(m.rstrip(".") for m in _DOCREF.findall(comment_text))
    soft = {name: set(p.findall(comment_text)) for name, p in _SOFT.items()}
    return knobs, docs, soft

# ---------------------------------------------------------------------- lint

_ACRONYMS = set("""
AR ARP API ATX BDS BIA BSSD BRSD CB CDDIS CLK CLI CMAKE CPU CSV CN0 DCB DD DOP DOY ECEF EKF ENU EOT20 ERP EWL
FIXME GAL GDOP GF GIM GLO GLONASS GMF GNSS GPS GPST GPT3 HDOP HTTP IF IFB IGS ILS IMU INS IO IONEX IRNSS ITRF
JSON JPL KF LAMBDA LDL LOS LS MDB MGEX MLAMBDA MSVC MW NAD83 NGS NL NTRIP OK OSB OSU PB PCO PCV PD PDOP PPP
PZ QR QZSS RAIM RINEX RMS RTCM RTK RTS SBAS SBF SD SH SIF SNR SP3 SPEC SSR STEC SVD TEC TECU TGD TODO TOW URA
UT UTC VCE VDOP VMF3 VTEC WGS84 WL WLS XXX ZHD ZWD NOTE HACK PASS FAIL FALSIFIED PROMOTED KEEP_OFF NO-GO
RECOMMEND_ON SCOPED OPEN CLOSED PARKED WARN ERROR INFO DEBUG NaN NAN IEEE ASCII UTF CRC LSB MSB ID IDS UID
IAU IERS ICRF ITRS GCRS TT TAI TDB UT1 PRN SV SVN BIPM CODE ESA CNES GFZ WHU JAXA DLR TUM TXT CRX RNX GZ
QC WE ZIP SI SSE AVX FMA NLOPT PSD PSF CSC SPD QK QP PPPRTK OBS NAV MN ORB
""".split())

_EMPH_SHORT = {"NO", "NOT", "ALL", "ANY", "AND", "BUT", "ONE", "TWO", "IS", "ARE", "BE", "DO", "MUST", "ONLY",
               "NEVER", "EVER", "ALWAYS", "BOTH", "SAME", "NEW", "OLD", "EACH", "EVERY", "NONE"}
_CODE_CAPS = None


def code_caps():
    """All-caps identifiers that exist in code (constants, enum values, event names)."""
    global _CODE_CAPS
    if _CODE_CAPS is None:
        caps = set()
        for d in ("src", "include"):
            for dp, _, fns in os.walk(os.path.join(ROOT, d)):
                for fn in fns:
                    if fn.endswith(CPP_EXT):
                        t = read(os.path.join(dp, fn))
                        caps |= set(re.findall(r"\b[A-Z][A-Z0-9]*_[A-Z0-9_]+\b", t))
        _CODE_CAPS = caps
    return _CODE_CAPS


_DRAMA = re.compile(r"\b(?:disease|poison\w*|scream\w*|launder\w*|scars?\b|heal\w*|honest\w*|dishonest|"
                    r"precisely|the whole point|belt[- ]and[- ](?:braces|suspenders)|gotcha|bites?\b|"
                    r"cure[ds]?\b|infect\w*|doom\w*|lethal|sick\b|lies\b|lying|magic)", re.I)
_HISTORY = re.compile(r"\b(?:used to|no longer|before this card|this card|was found|we found|turned out|"
                      r"originally|previously|20\d\d-\d\d-\d\d|(?:Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)"
                      r"-\d{1,2}\b|runs/[\w-]+)", re.I)
_SECOND = re.compile(r"\b(?:you|your|yours|you're|you'll|you've)\b", re.I)
_FIRST_PL = re.compile(r"\b(?:we|our|us)\b")
_GLYPH = re.compile(r"[⚠★☆✓✗✔✘❌✅‼]")
_LABEL_LEAD = re.compile(r"^\W*(?:[A-Z][A-Z0-9]{0,5}-\d+[a-z]?\b|[Cc]ard\s+[\w-]+|§|QUEUE\b|[A-Z]\d[a-z]?\b[: ])")


def style_hits(text):
    hits = Counter()
    hits["em_dash"] = text.count("—")
    hits["glyph"] = len(_GLYPH.findall(text))
    caps = code_caps()
    for w in re.findall(r"(?<![\w-])[A-Z][A-Z-]*[A-Z](?![\w-])", text):
        if w in _ACRONYMS or w in caps:
            continue
        if len(w) >= 4 or w in _EMPH_SHORT:
            hits["caps"] += 1
    hits["second_person"] = len(_SECOND.findall(text))
    hits["we"] = len(_FIRST_PL.findall(text))
    hits["drama"] = len(_DRAMA.findall(text))
    hits["history"] = len(_HISTORY.findall(text))
    if _LABEL_LEAD.search(text.lstrip()):
        hits["label_lead"] = 1
    return +hits


def blocks(comments):
    """Group whole-line comments on consecutive lines into blocks."""
    out, cur = [], None
    for a, b, t, trailing in comments:
        if cur and not trailing and not cur[3] and a == cur[1] + 1:
            cur = (cur[0], b, cur[2] + "\n" + t, False)
        else:
            if cur:
                out.append(cur)
            cur = (a, b, t, trailing)
    if cur:
        out.append(cur)
    return out

# ------------------------------------------------------------------ commands


def iter_sources():
    for d in SCAN_DIRS:
        for dp, dns, fns in os.walk(os.path.join(ROOT, d)):
            dns[:] = [x for x in dns if x not in ("__pycache__", "third_party", "build", ".git")]
            for fn in fns:
                if fn.endswith(CPP_EXT + PY_EXT):
                    yield os.path.join(dp, fn)


def cmd_rank(args):
    rows = []
    for p in iter_sources():
        t = read(p)
        try:
            cs = comments_of(p, t)
        except Exception:
            continue
        lines = set()
        for a, b, _, _ in cs:
            lines.update(range(a, b + 1))
        hits = Counter()
        for blk in blocks(cs):
            hits.update(style_hits(blk[2]))
        rows.append((len(lines), t.count("\n") + 1, sum(hits.values()), hits.get("em_dash", 0),
                     hits.get("caps", 0), rel(p)))
    rows.sort(reverse=True)
    tot_c = sum(r[0] for r in rows)
    print(f"{len(rows)} files, {tot_c:,} comment lines, {sum(r[1] for r in rows):,} lines")
    print(f"{'comment':>8} {'lines':>7} {'share':>6} {'style':>6} {'emdash':>6} {'caps':>5}  file")
    for c, n, s, e, k, f in rows[: args.top]:
        print(f"{c:8d} {n:7d} {100 * c / max(n, 1):5.0f}% {s:6d} {e:6d} {k:5d}  {f}")


def cmd_snapshot(args):
    for p in args.files:
        dst = os.path.join(PRE_DIR, rel(p))
        if os.path.isfile(dst) and not args.force:
            print(f"kept     {rel(p)} (pre-image exists; --force replaces it)")
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(read_bytes(p))
        print(f"snapshot {rel(p)}")


def _split_lines(b):
    return b.splitlines(keepends=True)


def cmd_read(args):
    lines = _split_lines(read_bytes(args.file))
    a, b = args.a, min(args.b, len(lines))
    chunk = b"".join(lines[a - 1:b])
    out = chunk.decode("utf-8", errors="replace").replace("\r\n", "\n")
    # The header line keeps a blank first line visible: tool output is trimmed, and a
    # blank line that is not seen is not reproduced in the rewrite.
    print(f"@@ begin {rel(args.file)} lines {a}-{b} (every line below, blank ones included)")
    sys.stdout.write(out if out.endswith("\n") or not out else out + "\n")
    print(f"@@ end   {rel(args.file)} lines {a}-{b} of {len(lines)}  sha {sha(chunk)}")


def cmd_splice(args):
    data = read_bytes(args.file)
    lines = _split_lines(data)
    a, b = args.a, args.b
    old = b"".join(lines[a - 1:b])
    if sha(old) != args.sha:
        print(f"REFUSED  lines {a}-{b} no longer have sha {args.sha} (now {sha(old)}); re-read the range")
        sys.exit(2)
    new = read_bytes(args.newfile).replace(b"\r\n", b"\n")
    if new.startswith(b"\xef\xbb\xbf") and a != 1:
        new = new[3:]
    eol = b"\r\n" if b"\r\n" in (old or data) else b"\n"
    if eol == b"\r\n":
        new = new.replace(b"\n", b"\r\n")
    if old.endswith((b"\n",)) and new and not new.endswith(b"\n"):
        new += eol
    out = b"".join(lines[:a - 1]) + new + b"".join(lines[b:])
    base, src = baseline(args.file)
    ok, detail = same_code(args.file, base if base is not None else data.decode("utf-8", "replace"),
                           out.decode("utf-8", errors="replace"))
    if not ok:
        print(f"REFUSED  the code would change (vs {src}); file untouched{detail}")
        sys.exit(1)
    if byte_form(out) != byte_form(data):
        print("REFUSED  encoding, BOM or line endings would change; file untouched")
        sys.exit(1)
    with open(args.file, "wb") as f:
        f.write(out)
    n_new = len(_split_lines(new))
    print(f"OK  replaced lines {a}-{b} with {n_new} line(s); they now end at line {a + n_new - 1} "
          f"(delta {n_new - (b - a + 1):+d})")


def _union(cs):
    k, d, soft = set(), set(), {n: set() for n in _SOFT}
    for _, _, t, _ in cs:
        a, b, c = facts(t)
        k |= a
        d |= b
        for n in soft:
            soft[n] |= c[n]
    return k, d, soft


def cmd_check(args):
    bad = 0
    for p in args.files:
        new = read(p)
        old, src = baseline(p)
        name = rel(p)
        if old is None:
            print(f"SKIP  {name}: no baseline")
            continue
        # 1. code identity and byte form
        same, detail = same_code(p, old, new)
        old_b, _ = baseline_bytes(p)
        fo, fn = byte_form(old_b), byte_form(read_bytes(p))
        if src == "HEAD":   # git may convert line endings on checkout; compare BOM and bad bytes only
            fo, fn = (fo[0], fo[2]), (fn[0], fn[2])
        form_ok = fo == fn
        # 2. hard references
        ko, do, so = _union(comments_of(p, old))
        kn, dn, sn = _union(comments_of(p, new))
        lost_k = sorted(ko - kn)
        # A reference that was split across a line break ("SPEC_synthetic_" / "minimum_...")
        # is preserved when the rewrite joins it.
        lost_d = sorted(d for d in do - dn
                        if not (d.endswith(("_", "-", "/")) and any(x.startswith(d) for x in dn)))
        ok = same and form_ok and not lost_k and not lost_d
        bad += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name}  (vs {src})")
        if not same:
            print(f"   code changed{detail}")
        if not form_ok:
            print(f"   byte form changed: was {fo[:2]}, now {fn[:2]} (BOM, line endings or non-UTF-8 bytes)")
        if lost_k:
            print(f"   lost knob refs: {', '.join(lost_k)}")
        if lost_d:
            print(f"   lost doc refs: {', '.join(lost_d)}")
        # 3. soft facts and style counts (report only)
        for n in _SOFT:
            lost = sorted(so[n] - sn[n])
            if lost:
                shown = ", ".join(lost[:25]) + (f" (+{len(lost) - 25})" if len(lost) > 25 else "")
                print(f"   dropped {n}: {shown}")
        ho, hn = Counter(), Counter()
        for blk in blocks(comments_of(p, old)):
            ho.update(style_hits(blk[2]))
        for blk in blocks(comments_of(p, new)):
            hn.update(style_hits(blk[2]))
        keys = sorted(set(ho) | set(hn))
        print("   style: " + ", ".join(f"{k} {ho.get(k, 0)}->{hn.get(k, 0)}" for k in keys))
    sys.exit(1 if bad else 0)


def cmd_lint(args):
    p = args.file
    lo, hi = 1, 10 ** 9
    if args.lines:
        lo, hi = (int(x) for x in args.lines.split("-"))
    n = 0
    for a, b, t, _ in blocks(comments_of(p, read(p))):
        if b < lo or a > hi:
            continue
        h = style_hits(t)
        if not h and not args.all:
            continue
        n += 1
        tag = ", ".join(f"{k}{'' if v == 1 else ' x' + str(v)}" for k, v in sorted(h.items())) or "clean"
        print(f"{rel(p)}:{a}-{b}  [{tag}]")
    print(f"{n} block(s)")


def main():
    # Comments hold non-ASCII text (Δ, σ, ≈); a cp1252 console cannot print it.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("rank")
    r.add_argument("--top", type=int, default=40)
    s = sub.add_parser("snapshot")
    s.add_argument("files", nargs="+")
    s.add_argument("--force", action="store_true")
    c = sub.add_parser("check")
    c.add_argument("files", nargs="+")
    l = sub.add_parser("lint")
    l.add_argument("file")
    l.add_argument("--lines")
    l.add_argument("--all", action="store_true")
    rd = sub.add_parser("read")
    rd.add_argument("file")
    rd.add_argument("a", type=int)
    rd.add_argument("b", type=int)
    sp = sub.add_parser("splice")
    sp.add_argument("file")
    sp.add_argument("a", type=int)
    sp.add_argument("b", type=int)
    sp.add_argument("newfile")
    sp.add_argument("sha")
    args = ap.parse_args()
    {"rank": cmd_rank, "snapshot": cmd_snapshot, "check": cmd_check, "lint": cmd_lint,
     "read": cmd_read, "splice": cmd_splice}[args.cmd](args)


if __name__ == "__main__":
    main()

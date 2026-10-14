"""Text operations of the card NOM3 step EP1 generator (scripts/nom_ep1_cut.py).

Every generated file is the frozen file's text (read from git at the base, LF line ends)
with a set of non-overlapping edits applied once; the generator never reads its own output,
which is what makes a second run a no-op.  This module holds the pieces that know nothing of
EP1 itself:

  Piece, Edit, apply_edits()   an edit replaces a character span [a, b) by a sequence of
                               pieces: literal text, or a span of the frozen text copied
                               through.  A frozen token inside a replaced span is removed
                               unless a copied piece carries it; removed_mask() answers that
                               for every token, so the generator can count what the new text
                               still reads.
  Lines                        line arithmetic over one text: the start and end of a line,
                               its indentation, whole-line spans of a statement.
  leading_comment()            the comment block attached directly above a statement (no
                               blank line between), and trailing_comment() the comment that
                               follows its last token on the same line.
  gap_comments()               the comments between two tokens (an argument list's inline
                               comments).
  doc_before()                 the Doxygen block (`/** */` or `///` lines) directly above a
                               declaration, and parse_doc() its head, @param entries and tail.
  wrap_args(), wrap_text()     line wrapping of a generated argument list and of prose.
  dedent()                     the four-column shift of a relocated body.
  read_target(), write_target()  the target tree's files: their line ends are kept (CRLF in
                               a Windows checkout), a file is written only when its bytes
                               change.
"""
import pathlib
import re
import textwrap


# ---------------------------------------------------------------------------------------------
# Edits
# ---------------------------------------------------------------------------------------------
class Piece:
    """Literal text (kind 'lit') or a copy of the frozen span [a, b) (kind 'src').  reads:
    root keys a literal piece reads (the generator counts them as reads of those names)."""
    __slots__ = ("kind", "text", "a", "b", "reads")

    def __init__(self, kind, text="", a=0, b=0, reads=()):
        self.kind, self.text, self.a, self.b, self.reads = kind, text, a, b, tuple(reads)

    def render(self, source):
        return self.text if self.kind == "lit" else source[self.a:self.b]


def lit(text, reads=()):
    return Piece("lit", text, reads=reads)


def src(a, b):
    return Piece("src", a=a, b=b)


class Edit:
    """Replace [a, b) of the frozen text by @p pieces.  tag names the transform (the
    manifest's record); a == b is an insertion (pieces at the same position keep the order
    the edits were added in)."""
    __slots__ = ("a", "b", "pieces", "tag", "seq")
    _seq = 0

    def __init__(self, a, b, pieces, tag):
        if isinstance(pieces, str):
            pieces = [lit(pieces)]
        self.a, self.b, self.pieces, self.tag = a, b, list(pieces), tag
        Edit._seq += 1
        self.seq = Edit._seq

    def text(self, source):
        return "".join(p.render(source) for p in self.pieces)


class EditError(Exception):
    pass


def sorted_edits(edits):
    out = sorted(edits, key=lambda e: (e.a, e.b, e.seq))
    for x, y in zip(out, out[1:]):
        if y.a < x.b:
            raise EditError(f"overlapping edits: [{x.a},{x.b}) {x.tag} and [{y.a},{y.b}) {y.tag}")
    return out


def apply_edits(source, edits, lo=0, hi=None):
    """The text of source[lo:hi] with every edit inside [lo, hi) applied."""
    hi = len(source) if hi is None else hi
    out, pos = [], lo
    for e in sorted_edits([e for e in edits if lo <= e.a and e.b <= hi]):
        out.append(source[pos:e.a])
        out.append(e.text(source))
        pos = e.b
    out.append(source[pos:hi])
    return "".join(out)


def removed_mask(T, edits):
    """removed[j] is True when frozen token j lies in a replaced span and no copied piece
    carries it through."""
    removed = [False] * T.n
    spans = []
    for e in edits:
        if e.b > e.a:
            keep = [(p.a, p.b) for p in e.pieces if p.kind == "src"]
            spans.append((e.a, e.b, keep))
    if not spans:
        return removed
    spans.sort()
    k = 0
    for j in range(T.n):
        pos = T.toks[j].pos
        while k < len(spans) and spans[k][1] <= pos:
            k += 1
        q = k
        while q < len(spans) and spans[q][0] <= pos:
            a, b, keep = spans[q]
            if a <= pos < b and not any(x <= pos < y for x, y in keep):
                removed[j] = True
                break
            q += 1
    return removed


# ---------------------------------------------------------------------------------------------
# Lines
# ---------------------------------------------------------------------------------------------
class Lines:
    def __init__(self, text):
        self.text = text

    def start(self, pos):
        """Index of the first character of the line holding pos."""
        return self.text.rfind("\n", 0, pos) + 1

    def end(self, pos):
        """Index one past the newline that ends the line holding pos (or len)."""
        k = self.text.find("\n", pos)
        return len(self.text) if k < 0 else k + 1

    def indent(self, pos):
        s = self.start(pos)
        m = re.match(r"[ \t]*", self.text[s:])
        return m.group(0)

    def line_text(self, pos):
        return self.text[self.start(pos):self.end(pos)].rstrip("\n")

    def only_code_from(self, T, first_tok, last_tok):
        """True when the statement tokens [first_tok, last_tok] own their lines: nothing
        but blanks before the first token on its line, nothing but blanks or a comment after
        the last token on its line."""
        s = self.start(T.toks[first_tok].pos)
        if self.text[s:T.toks[first_tok].pos].strip():
            return False
        after = self.text[T.toks[last_tok].end:self.end(T.toks[last_tok].end)].strip()
        return after == "" or after.startswith("//") or (after.startswith("/*") and after.endswith("*/"))


def _is_comment_line(line):
    s = line.strip()
    return s.startswith("//") or s.startswith("/*") or s.startswith("*") or s.endswith("*/")


def leading_comment(text, pos):
    """Start of the comment block directly above the line holding pos (no blank line
    between), or the line's own start when there is none."""
    L = Lines(text)
    s = L.start(pos)
    cur = s
    while cur > 0:
        prev = L.start(cur - 1)
        line = text[prev:cur - 1]
        if not line.strip() or not _is_comment_line(line):
            break
        # A block comment's inner line ('*' first) belongs only when the block closes below.
        cur = prev
    return cur


def trailing_comment(text, end):
    """The comment text after position end on the same line ('' when none)."""
    L = Lines(text)
    rest = text[end:L.end(end)].strip()
    return rest if rest.startswith("//") or rest.startswith("/*") else ""


_COMMENT = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)


def gap_comments(text, a, b):
    """The comments in text[a:b], each with its whitespace normalized."""
    out = []
    for m in _COMMENT.finditer(text, a, b):
        c = m.group(0)
        if c.startswith("//"):
            out.append(c[2:].strip())
        else:
            out.append(" ".join(c[2:-2].split()))
    return out


def collapse_ws(text):
    """Whitespace runs that contain a newline become one space (an argument's text moved onto
    one line); refused (None) when the text holds a line comment."""
    if re.search(r"//[^\n]*\n", text):
        return None
    return re.sub(r"\s*\n\s*", " ", text).strip()


def dedent(text, n=4):
    """Remove up to n leading spaces from every line."""
    pat = re.compile(r"^ {1,%d}" % n, re.M)
    return pat.sub("", text)


def indent_block(text, prefix):
    return "".join(prefix + ln if ln.strip() else ln for ln in text.splitlines(True))


# ---------------------------------------------------------------------------------------------
# Doxygen blocks
# ---------------------------------------------------------------------------------------------
_TAG = re.compile(r"^[@\\](param|return|returns|retval|see|sa|note|pre|post|throws|throw|"
                  r"exception|warning|attention|deprecated|tparam|par)\b")


def doc_before(text, pos):
    """(a, b) of the Doxygen block that ends on the line directly above the line holding
    pos: a `/** ... */` block (from its first line) or a run of `///` lines; None when
    that line is neither."""
    L = Lines(text)
    s = L.start(pos)
    if s == 0:
        return None
    prev = L.start(s - 1)
    line = text[prev:s - 1].strip()
    if line.endswith("*/"):
        a = text.rfind("/*", 0, s - 1)
        if a < 0 or not text.startswith(("/**", "/*!"), a):
            return None
        return L.start(a), s
    if line.startswith("///"):
        a = prev
        while a > 0:
            p2 = L.start(a - 1)
            if text[p2:a - 1].strip().startswith("///"):
                a = p2
            else:
                break
        return a, s
    return None


def doc_lines(raw):
    """The content lines of a raw Doxygen block (markers stripped, one leading space kept
    off, trailing blanks dropped)."""
    out = []
    raw = raw.rstrip()
    if raw.lstrip().startswith("/**"):
        body = raw.lstrip()[3:]
        if body.endswith("*/"):
            body = body[:-2]
        for ln in body.split("\n"):
            s = ln.strip()
            if s.startswith("*"):
                s = s[1:]
                if s.startswith(" "):
                    s = s[1:]
            out.append(s.rstrip())
    else:
        for ln in raw.split("\n"):
            s = ln.strip()
            if s.startswith("///"):
                s = s[3:]
                if s.startswith("<"):
                    s = s[1:]
                if s.startswith(" "):
                    s = s[1:]
            out.append(s.rstrip())
    while out and not out[0].strip():
        out.pop(0)
    while out and not out[-1].strip():
        out.pop()
    return out


def parse_doc(lines):
    """(head, params, tail): the lines before the first block tag; {name: text} of the
    @param entries (direction marks kept in the text); the other tag paragraphs, in order."""
    head, params, tail = [], {}, []
    cur = None          # ('param', name) or ('tail', index)
    seen_tag = False
    for ln in lines:
        s = ln.strip()
        m = _TAG.match(s)
        if m:
            seen_tag = True
            if m.group(1) == "param":
                rest = s[m.end():].strip()
                direction = ""
                dm = re.match(r"^\[(in|out|in,\s*out|in/out)\]\s*", rest)
                if dm:
                    direction = "[" + dm.group(1) + "] "
                    rest = rest[dm.end():]
                parts = rest.split(None, 1)
                name = parts[0] if parts else ""
                params[name] = direction + (parts[1] if len(parts) > 1 else "")
                cur = ("param", name)
            else:
                tail.append(ln)
                cur = ("tail", len(tail) - 1)
            continue
        if not seen_tag:
            head.append(ln)
            continue
        if not s:
            cur = None
            tail.append("")
            continue
        if cur and cur[0] == "param":
            params[cur[1]] = (params[cur[1]] + " " + s).strip()
        else:
            tail.append(ln)
    while head and not head[-1].strip():
        head.pop()
    while tail and not tail[-1].strip():
        tail.pop()
    while tail and not tail[0].strip():
        tail.pop(0)
    return head, params, tail


def render_doc(lines, indent, style="block"):
    """A Doxygen block of @p lines at @p indent (always the `/** */` form)."""
    out = [indent + "/**"]
    for ln in lines:
        out.append((indent + " * " + ln).rstrip())
    out.append(indent + " */")
    return "\n".join(out) + "\n"


_DOXY_CMD = re.compile(r"([@\\](?:ref|c|p|a|b|e))\s+")


def wrap_text(text, width, first="", rest=""):
    """@p text reflowed to @p width columns, the first line prefixed by @p first and the
    others by @p rest.  A Doxygen command that takes a word (@ref, @c, @p) is never split
    from its word."""
    glued = _DOXY_CMD.sub(lambda m: m.group(1) + "\x00", " ".join(text.split()))
    out = textwrap.wrap(glued, width=width, initial_indent=first, subsequent_indent=rest,
                        break_long_words=False, break_on_hyphens=False) or [first.rstrip()]
    return [ln.replace("\x00", " ") for ln in out]


def wrap_args(args, open_col, width=100, close=")"):
    """The text of an argument list that starts right after a `(` at column @p open_col:
    the arguments joined by ", " on one line when the line fits in @p width columns, else
    broken after commas with each continuation line aligned on @p open_col.  @p close is
    appended after the last argument (pass "" to leave the parenthesis out)."""
    one = ", ".join(args) + close
    if open_col + len(one) <= width or len(args) <= 1:
        return one
    lines, cur = [], ""
    for k, a in enumerate(args):
        piece = a + ("," if k < len(args) - 1 else close)
        if not cur:
            cur = piece
        elif open_col + len(cur) + 1 + len(piece) <= width:
            cur += " " + piece
        else:
            lines.append(cur)
            cur = piece
    lines.append(cur)
    return ("\n" + " " * open_col).join(lines)


# ---------------------------------------------------------------------------------------------
# The target tree
# ---------------------------------------------------------------------------------------------
def detect_eol(data):
    return "\r\n" if b"\r\n" in data else "\n"


def read_target(root, rel):
    p = pathlib.Path(root) / rel
    if not p.exists():
        return None
    return p.read_bytes()


def encode(text, eol):
    t = text.replace("\r\n", "\n")
    if eol != "\n":
        t = t.replace("\n", eol)
    return t.encode("utf-8")


def write_target(root, rel, text, eol, check=False):
    """Write @p text (LF) with line ends @p eol; returns 'unchanged', 'written' or, under
    @p check, 'differs'."""
    p = pathlib.Path(root) / rel
    data = encode(text, eol)
    old = p.read_bytes() if p.exists() else None
    if old == data:
        return "unchanged"
    if check:
        return "differs"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    return "written"

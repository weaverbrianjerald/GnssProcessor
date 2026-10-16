"""The reading layer of card NOM3 step PB1b: networkPassB_'s produce loop as pieces.

Shared by scripts/nom_pb1b_cut.py (the generator) and scripts/nom_pb1b_verify.py (the
relocation verifier); both read the frozen base the same way.  Lexing and declarations are
scripts/nom_cpp.py's; identifiers are scripts/nom_pb1/frozen.py's free_ids.

    Frozen(text)   networkPassB_ at the base: its EP1 roots and alias statements (with their
                   attached comments), the produce loop (pragma, for, try body, catch), the
                   pieces of rows_pb1b.PIECES, and the five MEMBER_TO_FREE helpers.
"""
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import nom_cpp  # noqa: E402
from nom_pb1 import rows_pb1b as rows  # noqa: E402
from nom_pb1.frozen import free_ids, is_comment  # noqa: E402

EP1_HEAD = "// ---- From the run and the epoch"
EP1_END = "// ---- End of the bindings ----"
PASSB_BANNER = "// --- PASS B: per-station assembly, then serial scatter"


def git_show(root, rev, rel):
    out = subprocess.run(["git", "-C", str(root), "show", f"{rev}:{rel}"],
                         capture_output=True, check=True).stdout
    return out.decode("utf-8", "replace").replace("\r\n", "\n")


def names_used(text):
    T = nom_cpp.Tokens(text)
    return {n for n, _ in free_ids(T, 0, T.n)}


def top_level_decls(text):
    """Names declared by the top-level statements of the fragment @p text."""
    T = nom_cpp.Tokens(text)
    out = set()
    for s, e in nom_cpp.statements(T, 0, T.n):
        d = nom_cpp.parse_declaration(T, s, e)
        if d:
            out |= {x.name for x in d}
    return out


class Stmt:
    def __init__(self, lines, T, a, b, floor):
        self.a, self.b = a, b
        self.first, self.last = T.line(a), T.line(b - 1)
        d = nom_cpp.parse_declaration(T, a, b)
        if not d:
            raise SystemExit(f"{rows.ASM}:{self.first}: not a declaration in a binding region")
        self.decl = d[0]
        self.name = d[0].name
        self.code = T.join(a, b)
        self.reads = {n for n, _ in free_ids(T, a, b) if n != self.name}
        c = self.first
        while c - 1 > floor and is_comment(lines[c - 2]):
            c -= 1
        self.comment_first = c
        self._lines = lines

    def text(self, with_comment=True):
        lo = self.comment_first if with_comment else self.first
        return self._lines[lo - 1:self.last]


class Frozen:
    def __init__(self, text):
        self.text = text
        self.lines = L = text.split("\n")
        self.T = T = nom_cpp.Tokens(text, rows.ASM)
        k = self._def("networkPassB_")
        pc = T.close(k + 3)
        self.body_open = pc + 1
        self.body_close = T.close(self.body_open)
        self.sig_first = T.line(k - 1)
        self.open_line = T.line(self.body_open)
        self.close_line = T.line(self.body_close)

        def find(prefix, lo, hi):
            hits = [n for n in range(lo, hi + 1) if L[n - 1].strip().startswith(prefix)]
            if len(hits) != 1:
                raise SystemExit(f"{rows.ASM}: {prefix!r} resolves {len(hits)} times")
            return hits[0]

        self.ep1_head = find(EP1_HEAD, self.open_line, self.close_line)
        self.ep1_end = find(EP1_END, self.open_line, self.close_line)
        self.banner = find(PASSB_BANNER, self.open_line, self.close_line)
        self.roots = self._stmts(self.ep1_head, self.ep1_end)
        self.aliases = self._stmts(self.ep1_end, self.banner - 1)
        # The produce loop.
        self.pragma = find("#pragma omp parallel for schedule(dynamic)", self.banner, self.close_line)
        ft = next(i for i in range(T.n) if T.line(i) > self.pragma and T.t(i) == "for")
        fo = T.close(ft + 1) + 1
        self.for_line = T.line(ft)
        self.loop_close = T.line(T.close(fo))
        tr = next(i for i in range(fo, T.n) if T.t(i) == "try")
        self.try_line = T.line(tr)
        self.try_close = T.line(T.close(tr + 1))
        # Pieces.
        self.pieces = {}
        order, lo = [], self.try_line + 1
        for key, anchor in rows.PIECES:
            hit = None
            for n in range(lo, self.try_close):
                l = L[n - 1]
                if (anchor.startswith(" ") and l == anchor) or \
                        (not anchor.startswith(" ") and l.strip().startswith(anchor)):
                    hit = n
                    break
            if hit is None:
                raise SystemExit(f"{rows.ASM}: piece {key} anchor {anchor!r} not found after {lo}")
            order.append((key, hit))
            lo = hit + 1
        for j, (key, s) in enumerate(order):
            e = (order[j + 1][1] if j + 1 < len(order) else self.try_close) - 1
            while e > s and not L[e - 1].strip():
                e -= 1
            self.pieces[key] = (s, e)
        # Helpers (MEMBER_TO_FREE).
        self.helpers = {}
        prev_end = None
        for h in sorted(rows.HELPERS, key=lambda h: self._def(h["old"])):
            k = self._def(h["old"])
            oc = T.close(k + 3)
            j = oc + 1
            while T.t(j) != "{":
                j += 1
            start = T.line(k - 1)
            c = start
            while c - 1 >= 1 and is_comment(L[c - 2]):
                c -= 1
            self.helpers[h["old"]] = dict(row=h, def_tok=k, popen=k + 3, pclose=oc,
                                          body_open=j, body_close=T.close(j),
                                          comment_first=c, sig_first=start,
                                          end=T.line(T.close(j)))
            prev_end = T.line(T.close(j))
        self.helpers_first = min(v["comment_first"] for v in self.helpers.values())
        self.helpers_last = max(v["end"] for v in self.helpers.values())

    def _def(self, name):
        T = self.T
        hits = [i for i in range(T.n - 3)
                if T.t(i) == "Application" and T.t(i + 1) == "::" and T.t(i + 2) == name
                and T.t(i + 3) == "(" and T.t(T.close(i + 3) + 1) in ("{", "const")]
        if len(hits) != 1:
            raise SystemExit(f"{rows.ASM}: {len(hits)} definitions of Application::{name}")
        return hits[0]

    def _stmts(self, lo_line, hi_line):
        T = self.T
        a = next(i for i in range(self.body_open, self.body_close) if T.line(i) > lo_line)
        b = next(i for i in range(a, self.body_close + 1)
                 if T.line(i) > hi_line or i == self.body_close)
        return [Stmt(self.lines, T, s, e, lo_line) for s, e in nom_cpp.statements(T, a, b)]

    def piece_lines(self, key):
        s, e = self.pieces[key]
        return self.lines[s - 1:e]

    def helper_params(self, old):
        """The parameter declarators of helper @p old, verbatim (whitespace folded)."""
        h = self.helpers[old]
        spans, _ = nom_cpp.split_params(self.T, h["popen"])
        return [re.sub(r"\s+", " ", self.T.src(a, b)).strip() for a, b in spans]

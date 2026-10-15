"""The reading layer of card NOM3 step PB1a: Application::networkPassB_ as regions and pieces.

Shared by scripts/nom_pb1_cut.py (which writes the cut) and scripts/nom_pb1_verify.py (which
checks it): both read the frozen function the same way, and the verifier then reads the new
tree on its own.  Lexing and declarations are scripts/nom_cpp.py's.

    PassB(text)          the function in @p text: its signature, the EP1 root bindings, the
                         alias block (statements with their attached comments, declared
                         name, the names each one reads), and the pieces of ORDER.
    free_ids(T, a, b)    identifiers of tokens [a, b) that are not member names or qualifiers.
"""
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import nom_cpp  # noqa: E402
from nom_pb1 import rows  # noqa: E402

EP1_HEAD = "// ---- From the run and the epoch"
EP1_END = "// ---- End of the bindings ----"


def git_show(root, rev, rel):
    out = subprocess.run(["git", "-C", str(root), "show", f"{rev}:{rel}"],
                         capture_output=True, check=True).stdout
    return out.decode("utf-8", "replace").replace("\r\n", "\n")


def free_ids(T, a, b):
    """[(name, token index)] of identifiers in [a, b) not after `.`, `->` or `::` and not
    before `::`."""
    out = []
    for i in range(a, b):
        if T.k(i) == "pp" and T.t(i).lstrip("# \t").startswith("pragma"):
            # An OpenMP clause names variables (num_threads(n_threads) if(parallel_on)).
            out += [(n, i) for n in re.findall(r"[A-Za-z_]\w*", T.t(i))]
            continue
        if T.k(i) != "id":
            continue
        if T.t(i - 1) in (".", "->", "::") or T.t(i + 1) == "::":
            continue
        out.append((T.t(i), i))
    return out


def is_comment(line):
    s = line.strip()
    return s.startswith("//") or s.startswith("/*") or s.startswith("*")


class Stmt:
    """One declaration statement of a binding region."""

    def __init__(self, P, a, b):
        T = P.T
        self.a, self.b = a, b
        self.first, self.last = T.line(a), T.line(b - 1)
        decls = nom_cpp.parse_declaration(T, a, b)
        if not decls:
            raise SystemExit(f"{rows.ASM}:{self.first}: not a declaration in a binding region")
        self.decl = decls[0]
        self.name = self.decl.name
        self.code = T.join(a, b)
        self.reads = {n for n, _ in free_ids(T, a, b) if n != self.name}
        # The comment block attached directly above (no blank line between).
        c = self.first
        while c - 1 > P.region_floor and is_comment(P.lines[c - 2]):
            c -= 1
        self.comment_first = c

    def text(self, P, with_comment=True):
        lo = self.comment_first if with_comment else self.first
        return P.lines[lo - 1:self.last]


class PassB:
    def __init__(self, text, path=rows.ASM, name="networkPassB_"):
        self.text = text
        self.lines = text.split("\n")
        self.T = T = nom_cpp.Tokens(text, path)
        k = None
        for i in range(T.n - 3):
            if (T.t(i) == "Application" and T.t(i + 1) == "::" and T.t(i + 2) == name
                    and T.t(i + 3) == "("):
                if k is not None:
                    raise SystemExit(f"{path}: two definitions of {name}")
                k = i
        if k is None:
            raise SystemExit(f"{path}: no definition of {name}")
        pc = T.close(k + 3)
        self.body_open = pc + 1
        if T.t(self.body_open) != "{":
            raise SystemExit(f"{path}: {name} is not a definition")
        self.body_close = T.close(self.body_open)
        self.sig_first = T.line(k - 1)            # the return type's line
        self.open_line = T.line(self.body_open)
        self.close_line = T.line(self.body_close)
        L = self.lines

        def find(prefix, lo, hi, unique=True):
            hits = [n for n in range(lo, hi + 1) if L[n - 1].startswith("    " + prefix)]  # top level only
            if len(hits) != 1:
                raise SystemExit(f"{path}: anchor {prefix!r} resolves {len(hits)} times in {name}")
            return hits[0]

        self.ep1_head = find(EP1_HEAD, self.open_line, self.close_line)
        self.ep1_end = find(EP1_END, self.open_line, self.close_line)
        # Pieces.
        starts = [find(r["start"], self.ep1_end, self.close_line) for r in rows.ORDER]
        if starts != sorted(starts):
            raise SystemExit(f"{path}: the pieces of ORDER are not in frozen order")
        self.pieces = []
        for j, r in enumerate(rows.ORDER):
            lo = starts[j]
            hi = (starts[j + 1] if j + 1 < len(starts) else self.close_line) - 1
            while hi > lo and not L[hi - 1].strip():
                hi -= 1
            self.pieces.append(dict(r, lo=lo, hi=hi))
        self.alias_end = starts[0] - 1
        # Binding regions: statements of the EP1 block and of the alias block.
        self.region_floor = self.ep1_end
        self.roots = self._stmts(self.ep1_head, self.ep1_end, floor=self.ep1_head)
        self.aliases = self._stmts(self.ep1_end, self.alias_end, floor=self.ep1_end)

    def _stmts(self, lo_line, hi_line, floor):
        T = self.T
        a = next(i for i in range(self.body_open, self.body_close) if T.line(i) > lo_line)
        b = next(i for i in range(a, self.body_close + 1) if T.line(i) > hi_line or i == self.body_close)
        self.region_floor = floor
        out = [Stmt(self, s, e) for s, e in nom_cpp.statements(T, a, b)]
        return out

    def piece(self, key):
        return next(p for p in self.pieces if p["piece"] == key)

    def piece_lines(self, p):
        return self.lines[p["lo"] - 1:p["hi"]]

    def piece_tokens(self, p):
        T = self.T
        a = next(i for i in range(T.n) if T.line(i) >= p["lo"])
        b = next((i for i in range(a, T.n) if T.line(i) > p["hi"]), T.n)
        return a, b

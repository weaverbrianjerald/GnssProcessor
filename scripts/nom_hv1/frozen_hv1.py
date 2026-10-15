"""The frozen harvest of card NOM3 step HV1 at rows_hv1.BASE: the anchors resolved, the table
moved onto base lines, the coverage and drop-kind checks (the design's hv1_table.py, made to
run on the real base).  Shared by scripts/nom_hv1_cut.py and scripts/nom_hv1_verify.py.
"""
import copy
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import nom_cpp as nc  # noqa: E402
from nom_hv1 import rows_hv1 as rows  # noqa: E402

ROOT = HERE.parent.parent


class Fail(SystemExit):
    pass


def git_show(rev, rel, root=ROOT):
    out = subprocess.run(["git", "-C", str(root), "show", f"{rev}:{rel}"],
                         capture_output=True, check=True).stdout
    return out.decode("utf-8").replace("\r\n", "\n")


def is_code(line):
    s = line.strip()
    return bool(s) and not s.startswith("//")


def code_text(line):
    return line.split("//")[0].strip()


_HF = "(?:" + "|".join(rows.HF_NAMES) + ")"
HF_PATTERNS = [
    re.compile(r"^std::vector<.*>\s+" + _HF + r";$"),
    re.compile(r"^" + _HF + r"\.resize\(stations\.size\(\)\);$"),
    re.compile(r"^" + _HF + r"\.assign\(stations\.size\(\), -1\);$"),
    re.compile(r"^" + _HF + r"\[i\]\s*=\s*(hblk|cols|iono_row0|rxclk_row0|rxcb_row0);$"),
    re.compile(r"^if \((?:" + "|".join(rows.HF_FLAGS) + r")\) \{$"),
    re.compile(r"^\}$"),
]


def validate_drop(kind, lines):
    code = [code_text(l) for l in lines if is_code(l)]
    if kind == "lambda_head":
        j = " ".join(code)
        if ";" in j or not j.startswith("auto harvestCoupling = [&](") or not j.endswith("{"):
            return f"not a lambda head: {j[:80]!r}"
        return None
    if kind == "lambda_close":
        return None if code == ["};"] else f"not the lambda's close: {code!r}"
    if kind == "hand_forward":
        for c in code:
            if not any(p.match(c) for p in HF_PATTERNS):
                return f"not hand-forward text: {c[:80]!r}"
        return None
    return f"unknown drop kind {kind!r}"


class Frozen:
    """The frozen file, its function and the table moved onto base lines."""

    def __init__(self, rev=rows.BASE, table=None, text=None):
        self.rev = rev
        self.text = text if text is not None else git_show(rev, rows.FROZEN)
        self.L = self.text.split("\n")          # L[n-1] is line n
        self.T = nc.Tokens(self.text, rows.FROZEN)
        T = self.T
        k = next(i for i in range(T.n) if T.t(i) == rows.FUNC_NAME and T.t(i + 1) == "(")
        self.sig_line = T.line(k)
        p = T.close(k + 1)
        self.open = p + 1
        if T.t(self.open) != "{":
            raise Fail("frozen: the harvest's body does not follow its parameter list")
        self.close = T.close(self.open)
        self.func = (self.sig_line, T.line(self.close))
        self.params = nc.parse_params(T, k + 1)[0]
        self.table = copy.deepcopy(table if table is not None else rows.ROWS)
        self._index_statements()
        self.offsets = {}
        self.failures = []
        self._resolve()

    def _index_statements(self):
        T = self.T
        self.sibling = {}

        def walk(lo, hi):
            sts = nc.statements(T, lo, hi)
            starts = [s for s, _ in sts]
            for k, (s, e) in enumerate(sts):
                self.sibling[s] = (starts, k)
                j = s
                while j < e:
                    if T.t(j) == "{" and T.k(j) == "op":
                        c = T.close(j)
                        walk(j + 1, c)
                        j = c + 1
                    else:
                        j += 1
        walk(self.open + 1, self.close)
        self.starts = sorted(self.sibling)

    def anchor_hits(self, anchor):
        T = self.T
        pfx = [t.text for t in nc.lex(anchor)]
        return [s for s in self.starts
                if all(s + q < T.n and T.t(s + q) == pfx[q] for q in range(len(pfx)))]

    def _resolve(self):
        """Move every row by its anchor's offset; record failures (anchor count, first line)."""
        T, L = self.T, self.L
        for row in self.table:
            f, home, shape, pieces, ex = row
            hits = self.anchor_hits(ex["anchor"])
            if len(hits) != 1:
                self.failures.append((f, f"anchor matches {len(hits)} statement starts "
                                         f"{[T.line(h) for h in hits]}"))
                continue
            sibs, k = self.sibling[hits[0]]
            k2 = k + ex.get("at", 0)
            if not 0 <= k2 < len(sibs):
                self.failures.append((f, "anchor `at` leaves its block"))
                continue
            got = T.line(sibs[k2])
            want = rows.FIRST_CODE[f]
            off = got - want
            self.offsets[f] = off
            self._move(row, off)
            # the moved row's first code line must be the anchored statement
            first = ex["first_code"] if "first_code" in ex else next(
                (n for a, b, _ in row[3] for n in range(a, b + 1) if is_code(L[n - 1])), None)
            if first != got:
                self.failures.append((f, f"anchor resolves to {got}, the piece starts at {first}"))
            if "merge" in ex:
                th = self.anchor_hits(ex["merge"]["twin_anchor"])
                if len(th) != 1 or T.line(th[0]) != ex["merge"]["twin"][0]:
                    self.failures.append((f, f"twin anchor at {[T.line(h) for h in th]}, "
                                             f"twin piece at {ex['merge']['twin'][0]}"))
        # the signature and the alias block are found, not moved: the alias block ends at the
        # ledger row's first piece
        ledger = next(r for r in self.table if r[0] == "networkHarvestHandoverLedger")
        sig_end = self.T.line(self.open)
        self.replaced = {(self.sig_line, sig_end): "signature",
                         (sig_end + 1, ledger[3][0][0] - 1): "alias"}
        if self.func[1] != rows.P_FUNC[1] + self.offsets.get(rows.FUNC_NAME, 0):
            self.failures.append(("<function>", f"closes at {self.func[1]}"))

    def _move(self, row, off):
        f, home, shape, pieces, ex = row
        row[3][:] = [(a + off, b + off, s) for a, b, s in pieces]
        if "first_code" in ex:
            ex["first_code"] += off
        for key in ("subs",):
            if key in ex:
                ex[key] = {n + off: v for n, v in ex[key].items()}
        if "drops" in ex:
            ex["drops"] = {(a + off, b + off): k for (a, b), k in ex["drops"].items()}
        if "extract" in ex:
            ex["extract"] = [(a + off, b + off, fn, call) for a, b, fn, call in ex["extract"]]
        if "merge" in ex:
            a, b = ex["merge"]["twin"]
            ex["merge"]["twin"] = (a + off, b + off)

    def row(self, name):
        return next(r for r in self.table if r[0] == name)

    # ---- coverage and drop kinds (design 3.1, hv1_table.py) ----
    def coverage(self):
        L = self.L
        owner = {}
        fails = list(self.failures)

        def claim(n, who):
            if is_code(L[n - 1]):
                owner.setdefault(n, []).append(who)
        for (a, b) in self.replaced:
            for n in range(a, b + 1):
                claim(n, "REPLACED")
        extracted = {}
        for f, home, shape, pieces, ex in self.table:
            dropped = set()
            for (a, b), kind in ex.get("drops", {}).items():
                if not (shape == "drop" or any(pa <= a and b <= pb for pa, pb, _ in pieces)):
                    fails.append((f, f"drop {a}-{b} lies outside the row's pieces"))
                msg = validate_drop(kind, L[a - 1:b])
                if msg:
                    fails.append((f, f"drop {a}-{b} ({kind}): {msg}"))
                for n in range(a, b + 1):
                    dropped.add(n)
                    claim(n, f"DROP:{kind}")
            for a, b, _ in pieces:
                for n in range(a, b + 1):
                    if n not in dropped:
                        claim(n, f)
            for e in ex.get("extract", []):
                extracted[(e[0], e[1])] = f
        for (a, b), caller in extracted.items():
            for n in range(a, b + 1):
                if n in owner and caller in owner[n]:
                    owner[n].remove(caller)
        for f, home, shape, pieces, ex in self.table:
            if "merge" in ex:
                ta, tb = ex["merge"]["twin"]
                for n in range(ta, tb + 1):
                    if n in owner and f not in owner[n]:
                        owner[n].append(f + "(twin)")
        total = 0
        for n in range(self.func[0], self.func[1] + 1):
            if not is_code(L[n - 1]):
                continue
            total += 1
            w = owner.get(n, [])
            if len(w) != 1:
                fails.append(("coverage", f"line {n}: claimed by {w}: {L[n - 1].strip()[:60]}"))
        counts = {}
        for n, w in owner.items():
            if len(w) == 1 and w[0].startswith(("DROP:", "REPLACED")):
                counts[w[0]] = counts.get(w[0], 0) + 1
        return total, counts, fails


def load(rev=rows.BASE, table=None, text=None):
    return Frozen(rev, table, text)

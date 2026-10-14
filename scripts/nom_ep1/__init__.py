"""The canonical table of card NOM3 step EP1 (docs/refound/NOM3_network_giants_2026-10-09.md).

The package holds what the EP1 generator (scripts/nom_ep1_cut.py) and the relocation verifier
read: the row type, the fold grammar and the realization tables (model.py), the declared
substitutions and the declared non-conversions (declared.py), and one row per converted seam,
one file per worker (rows_app.py W1, rows_solve.py W2, rows_emit.py W3, rows_diag.py W4).  No
row carries a binding or a line number: the survey derives both from the frozen source <B>.

The generator's own modules, which the verifier does not import: gen.py (the seam plans and
the reads over edited text), gen_epoch.py (the epoch function), gen_seams.py (the seam
conversion and the contract test), gen_setup.py (runNetwork, NetworkRun.cpp and the EP1a-1
edits), templates.py (the designed text of the types) and textops.py (edits, lines, comments,
Doxygen blocks, the target tree).  Each run writes manifest_<rows>.json here (under --root).

DEFAULT_BASE is the frozen source <B> every tool reads with `git show` when --base is not
given.  It names nom/ep0 at the time of writing (main 366385b plus step EP0); it is re-pointed
to the commit that merges EP0 into main before step EP1a-1 starts, and every tool re-derives
every line number from whichever base it is given.

The base also decides which table entries apply (bind_base below).  The REMOVE-key deletion
retires configuration keys and the code behind them, and some of that code is named by the
tables: six seams of the per-epoch lambda, the lane layer, the fast clock (FC-1), the PB emit
filter, the iono-deweight gate, the ensemble gauges and the third-band predicate.  An entry
that names such an object is marked `retired_by=REMOVE_KEY`.  Every tool calls bind_base()
once, right after reading the base; from then on the tables it reads (load_rows(),
declared.declared() and declared.find(), model.py's realization tables, the templates'
retirement edits) hold only what applies there:
  - a marked entry whose object the base no longer has does not apply; the survey and the
    generator list it as retired (the same tables serve the EP0 base, where every object
    exists, and the base after the deletion);
  - a marked entry whose object the base has applies as written;
  - a marked entry whose object the base has only in part (a half-applied deletion) is a
    survey problem;
  - an unmarked entry must find its object at every base, as before.
A name the deletion re-spelled (RENAMED) is spelled the way the base declares it.
"""
import pathlib
import sys

_SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))   # nom_cpp, rt_seam_verify live beside this package

DEFAULT_BASE = "0e14eb9"

ROW_MODULES = ("rows_app", "rows_solve", "rows_emit", "rows_diag")

# The deletion that retires the entries marked `retired_by=REMOVE_KEY` (the banner above).
REMOVE_KEY = "the REMOVE-key deletion"

# Names the REMOVE-key deletion re-spelled: the EP0 spelling -> the spelling after it.  The
# tables spell the EP0 name; at a base that declares the other spelling (and not the EP0
# one), bind_base() re-spells every entry that names it.  `fc_n_emitted_before` lost the fast
# clock's prefix with FC-1: the counter snapshot before networkEmitStore, whose comparison
# with ctx.counters.n_emitted becomes ep.emitted (VALUE_EQUAL).
RENAMED = {"fc_n_emitted_before": "n_emitted_before"}


class BaseFit:
    """What bind_base() found at a base.

    retired:   [(table, entry, what is gone)] for each entry marked retired_by whose object the
               base no longer has: it does not apply at this base.
    respelled: {EP0 name: the base's name} for each RENAMED name the base spells the new way.
    problems:  each entry marked retired_by whose object the base has only in part (a
               half-applied deletion); the survey fails on them."""

    def __init__(self, base):
        self.base = base
        self.retired = []
        self.respelled = {}
        self.problems = []
        self.dropped_rows = set()

    def lines(self):
        """The fit as report lines (the survey's and the generator's)."""
        out = [f"{table} {name}: retired by {REMOVE_KEY}; {why}" for table, name, why in self.retired]
        out += [f"{old} is spelled {new} at this base; the entries naming it are re-spelled"
                for old, new in self.respelled.items()]
        return out

    def summary(self):
        """The fit in at most three lines (the verifier's header); empty at a base that has
        every object the deletion retires."""
        out = []
        if self.retired:
            out.append(f"retired at this base ({REMOVE_KEY}): {len(self.retired)} table entries do "
                       f"not apply, among them the rows {', '.join(sorted(self.dropped_rows)) or '(none)'}")
        if self.respelled:
            out.append("re-spelled at this base: " + ", ".join(f"{a} -> {b}" for a, b in self.respelled.items()))
        if self.problems:
            out.append(f"HALF APPLIED at this base: {'; '.join(self.problems)}")
        return out


_FIT = None


def bound_fit():
    """The BaseFit of the base the tables are bound to, or None before bind_base()."""
    return _FIT


def declares(fz, key):
    """Whether the base declares root key @p key: `D:x` in runNetwork's outermost scope,
    `L:x` at the per-epoch lambda's top level (a parameter does not count)."""
    kind, _, name = key.partition(":")
    scope = fz.drv.scopes.root if kind == "D" else fz.drv.lam_scope
    return any(d.name == name and d.kind != "param" for d in scope.decls)


def presence(found):
    """'present' when every witness of an object is found at the base, 'absent' when none
    is, 'partial' otherwise (a half-applied deletion)."""
    found = list(found)
    if all(found):
        return "present"
    return "absent" if not any(found) else "partial"


def row_witnesses(fz, row):
    """[(what, found)] of a row's seam at the base: the frozen lambda calls it, its header
    declares it, its source defines it (an Application member for a MEMBER row)."""
    cls = "Application" if row.mech == "MEMBER" else ""
    h, c = fz.tree.index(row.hpp), fz.tree.index(row.cpp)
    return [("called by the frozen lambda", any(x.name == row.name for x in fz.drv.calls)),
            (f"declared in {row.hpp}", bool(h) and any(f.name == row.name and (f.cls or "") == cls
                                                       for f in h.funcs)),
            (f"defined in {row.cpp}", bool(c) and any(f.name == row.name and f.is_def
                                                      and (f.cls or "") == cls for f in c.funcs))]


def bind_base(fz):
    """Binds the tables to the frozen base @p fz (an object with .drv, a nom_cpp.Driver, and
    .tree, a nom_cpp.GitTree: the survey's and the verifier's Frozen) and returns the
    BaseFit.  Decides each entry marked retired_by from its object at the base, re-spells the
    RENAMED names the base spells the new way, and binds load_rows(), declared.py and model.py
    to the entries that apply.  Called once per process, before any table is read."""
    global _FIT
    from nom_ep1 import declared, model
    fit = BaseFit(fz.base)
    for old, new in RENAMED.items():
        if any(declares(fz, k + new) and not declares(fz, k + old) for k in ("L:", "D:")):
            fit.respelled[old] = new
    for row in _load(ROW_MODULES):
        if not row.retired_by:
            continue
        w = row_witnesses(fz, row)
        p = presence(f for _, f in w)
        if p == "absent":
            fit.dropped_rows.add(row.name)
            fit.retired.append((f"row ({row.worker})", row.name,
                                "gone at this base (not called, declared or defined)"))
        elif p == "partial":
            fit.problems.append(f"row {row.name} ({REMOVE_KEY}): half applied at this base: "
                                + "; ".join(("" if f else "not ") + what for what, f in w))
    declared.bind_base(fz, fit)
    model.bind_base(fz, fit)
    _FIT = fit
    return fit


def _load(names):
    import importlib
    rows = []
    for n in names:
        mod = importlib.import_module(f"nom_ep1.{n}")
        rows.extend(mod.ROWS)
    return rows


def load_rows(names=ROW_MODULES):
    """The Seam rows of the named row modules, in module order; after bind_base(), without
    the rows the REMOVE-key deletion retired at the bound base."""
    rows = _load(names)
    if _FIT is not None:
        rows = [r for r in rows if r.name not in _FIT.dropped_rows]
    return rows

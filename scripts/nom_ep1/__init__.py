"""The canonical table of card NOM3 step EP1 (docs/refound/NOM3_network_giants_2026-10-09.md).

The package holds what the EP1 generator (scripts/nom_ep1_cut.py) and the relocation verifier
read: the row type, the fold grammar and the realization tables (model.py), the declared
substitutions and the declared non-conversions (declared.py), and one row per converted seam,
one file per worker (rows_app.py W1, rows_solve.py W2, rows_emit.py W3, rows_diag.py W4).  No
row carries a binding or a line number: the survey derives both from the frozen source <B>.

DEFAULT_BASE is the frozen source <B> every tool reads with `git show` when --base is not
given.  It names nom/ep0 at the time of writing (main 366385b plus step EP0); it is re-pointed
to the commit that merges EP0 into main before step EP1a-1 starts, and every tool re-derives
every line number from whichever base it is given.
"""
import pathlib
import sys

_SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))   # nom_cpp, rt_seam_verify live beside this package

DEFAULT_BASE = "c483a11"

ROW_MODULES = ("rows_app", "rows_solve", "rows_emit", "rows_diag")


def load_rows(names=ROW_MODULES):
    """The Seam rows of the named row modules, in module order."""
    import importlib
    rows = []
    for n in names:
        mod = importlib.import_module(f"nom_ep1.{n}")
        rows.extend(mod.ROWS)
    return rows

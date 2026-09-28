"""Trace-folder resolution: the Python analogue of ``gnssVizLogDir`` /
``gnssTracePath``.

A :class:`TraceContext` binds one run's log folder and hands out CSV paths and
presence checks. Unlike the MATLAB version it is an explicit object rather
than session-global ``appdata``: a viewer is handed a context, which keeps
the data layer free of hidden global state, and makes it safe to render two
runs at once (for example a post-processing run and its smoother trace).

The default-folder search order mirrors ``gnssVizLogDir``: the first existing
of ``C:\\GnssProcessor\\logs``, the Release log dir, the Debug log dir.
"""

from __future__ import annotations

import os
from typing import Optional, Sequence

_DEFAULT_DIRS: Sequence[str] = (
    r"C:\GnssProcessor\logs",
    r"C:\GnssProcessor\build\x64-Release\logs",
    r"C:\GnssProcessor\build\x64-Debug\logs",
)


def default_log_dir() -> str:
    """First existing default trace folder (falls back to the first entry)."""
    for d in _DEFAULT_DIRS:
        if os.path.isdir(d):
            return d
    return _DEFAULT_DIRS[0]


class TraceContext:
    """Resolves trace-CSV paths inside one run's log folder.

    Parameters
    ----------
    log_dir:
        The folder holding the ``*_trace.csv`` files.  ``None`` -> resolve the
        first existing default (see :func:`default_log_dir`).
    """

    def __init__(self, log_dir: Optional[str] = None, *,
                 network_config: Optional[str] = None,
                 data_root: Optional[str] = None,
                 truth_dir: Optional[str] = None,
                 run_config: Optional[str] = None,
                 ref_ecef=None):
        self.log_dir = os.path.abspath(log_dir) if log_dir else default_log_dir()
        #: Optional path to the network run's config JSON: lets the geographic
        #: viewers resolve station positions/roles when no roster CSV was emitted.
        self.network_config = os.path.abspath(network_config) if network_config else None
        #: Optional data root searched for an IONEX GIM (the ionosphere backdrop).
        self.data_root = os.path.abspath(data_root) if data_root else None
        #: Optional GnssSimulator answer-key folder (truth_*.csv +
        #: scenario_summary.json).  When set, the truth-referenced ``exam_*``
        #: viewers light up; when unset they auto-skip (a plain processor run is
        #: unchanged).  Mirrors the ``network_config`` / ``data_root`` pattern.
        self.truth_dir = os.path.abspath(truth_dir) if truth_dir else None
        #: Optional path to this run's processor config JSON (comments tolerated,
        #: read via :mod:`gnssviz.runconfig`).  Supplies the surveyed reference
        #: position (``receiver.reference_position_ecef``) to the
        #: ``rt_scorecard`` viewer and the ``app_mode`` to the report title.
        self.run_config = os.path.abspath(run_config) if run_config else None
        #: Optional explicit surveyed reference ECEF ``(x, y, z)`` [m].  Wins
        #: over ``run_config``'s block when both are given; ``None`` (with no
        #: usable ``run_config``) auto-skips the truth-referenced scorecard.
        self.ref_ecef = (tuple(float(v) for v in ref_ecef)
                         if ref_ecef is not None else None)

    def path(self, csv_name: str) -> str:
        """Full path of a trace CSV inside this run's folder."""
        return os.path.join(self.log_dir, csv_name)

    def truth_path(self, name: str) -> Optional[str]:
        """Full path of a truth file inside :attr:`truth_dir` (``None`` if unset)."""
        return os.path.join(self.truth_dir, name) if self.truth_dir else None

    def has_truth(self, name: str) -> bool:
        """``True`` iff :attr:`truth_dir` is set and holds a non-empty ``name``.

        Same "exists and has at least one data row" semantics as
        :meth:`has_data` for CSVs (so a header-only truth file skips its viewer);
        a non-CSV answer-key file (e.g. ``scenario_summary.json``) counts as
        present when it is a non-empty file.  ``False`` whenever ``truth_dir`` is
        unset, so a non-exam run never lights up an exam viewer.
        """
        if not self.truth_dir:
            return False
        p = self.truth_path(name)
        try:
            if not p or os.path.getsize(p) <= 0:
                return False
            if not name.lower().endswith(".csv"):
                return True                       # non-empty non-CSV (JSON answer key)
            with open(p, "r", encoding="utf-8", errors="replace") as fh:
                fh.readline()                     # header
                return bool(fh.readline().strip())  # at least one data row
        except OSError:
            return False

    def find_ionex(self) -> Optional[str]:
        """Locate an IONEX GIM (``*GIM*.INX``) for the ionosphere backdrop.

        Searches :attr:`data_root` (when set) then the default project ``data``
        tree, returning the first match (lexicographically) or ``None``.
        """
        import glob
        roots = [r for r in (self.data_root, r"C:\GnssProcessor\data") if r]
        for root in roots:
            hits = sorted(glob.glob(os.path.join(root, "**", "*GIM*.INX"),
                                    recursive=True))
            if hits:
                return hits[0]
        return None

    def has_data(self, csv_name: str) -> bool:
        """``True`` if the CSV exists and has at least one *data* row.

        A zero-byte or header-only trace means the logger was enabled but never
        fired (for example ``wls_trace.csv`` on a run without WLS reseeds).
        pandas can read a header-only file but viewers then index empty arrays,
        so skip them, the same way ``view_all`` treats ``readtable`` errors as
        absent. Only the first two lines are read, so this is cheap even on
        large traces.
        """
        p = self.path(csv_name)
        try:
            if os.path.getsize(p) <= 0:
                return False
            with open(p, "r", encoding="utf-8", errors="replace") as fh:
                fh.readline()                    # header
                return bool(fh.readline().strip())   # at least one data row
        except OSError:
            return False

    def __repr__(self) -> str:
        return f"TraceContext({self.log_dir!r})"

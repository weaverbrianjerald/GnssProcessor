"""Trace-folder resolution -- the Python analogue of ``gnssVizLogDir`` /
``gnssTracePath``.

A :class:`TraceContext` binds one run's log folder and hands out CSV paths and
presence checks.  Unlike the MATLAB version it is an explicit object rather than
session-global ``appdata`` -- a viewer is handed a context, which keeps the data
layer free of hidden global state (and makes it safe to render two runs at once,
e.g. a post-processing run and its smoother trace).

The default-folder search order mirrors ``gnssVizLogDir``: the first existing of
``C:\\GnssProcessor\\logs`` -> the Release log dir -> the Debug log dir.
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

    def __init__(self, log_dir: Optional[str] = None):
        self.log_dir = os.path.abspath(log_dir) if log_dir else default_log_dir()

    def path(self, csv_name: str) -> str:
        """Full path of a trace CSV inside this run's folder."""
        return os.path.join(self.log_dir, csv_name)

    def has_data(self, csv_name: str) -> bool:
        """``True`` if the CSV exists and has at least one *data* row.

        A zero-byte OR header-only trace means the logger was enabled but never
        fired (e.g. ``wls_trace.csv`` on a run without WLS reseeds).  pandas can
        read a header-only file but viewers then index empty arrays, so -- like
        ``view_all`` treating ``readtable`` errors as absent -- skip them.  Only
        the first two lines are read, so this is cheap even on large traces.
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

"""Viewer contract.

A *viewer* is the Python analogue of one MATLAB ``view_*`` / ``plot*`` script: it
reads one (or more) trace CSVs from a :class:`~gnssviz.trace_paths.TraceContext`,
prepares data, and returns :class:`Figure` specs plus an optional plain-text
summary (the console read-outs the MATLAB scripts ``fprintf``).  It never touches
a plotting library: rendering is the backend's job.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, List

from ..animation import Animation
from ..figures import Figure
from ..trace_paths import TraceContext


class ViewerSkip(Exception):
    """Raised by a viewer's ``build`` to declare it has nothing to draw.

    The driver turns this into a *skipped* (not *failed*) entry, with the
    exception text as the reason.  Used by viewers whose applicability cannot be
    expressed as a missing ``required_csvs``, e.g. the geographic viewers,
    which depend on a station roster / config / IONEX resolved at build time.
    """


@dataclass
class ViewerResult:
    """What a viewer produces for one run."""
    figures: List[Figure] = field(default_factory=list)
    summary: str = ""        # plain-text stats block, surfaced in the HTML report
    animations: List[Animation] = field(default_factory=list)  # GIF movies (e.g. iono over epochs)


@dataclass
class Viewer:
    """Registry entry binding a name + its required CSV(s) to a build function.

    ``default_enabled`` controls whether the viewer is part of the *default*
    report/dashboard composition.  When ``False`` the driver skips it unless it
    is explicitly requested (``--only <name>``) or the caller opts every viewer
    in (``--all`` / ``include_disabled=True``).  Use it for heavy or noisy
    viewers that are useful on demand but shouldn't clutter the standard
    summary, e.g. the ``residual_strip`` raster.
    """
    name: str
    title: str
    required_csvs: tuple
    build: Callable[[TraceContext], ViewerResult]
    default_enabled: bool = True

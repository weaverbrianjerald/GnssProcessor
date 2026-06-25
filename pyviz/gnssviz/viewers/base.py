"""Viewer contract.

A *viewer* is the Python analogue of one MATLAB ``view_*`` / ``plot*`` script: it
reads one (or more) trace CSVs from a :class:`~gnssviz.trace_paths.TraceContext`,
prepares data, and returns :class:`Figure` specs plus an optional plain-text
summary (the console read-outs the MATLAB scripts ``fprintf``).  It never touches
a plotting library -- rendering is the backend's job.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, List

from ..figures import Figure
from ..trace_paths import TraceContext


@dataclass
class ViewerResult:
    """What a viewer produces for one run."""
    figures: List[Figure] = field(default_factory=list)
    summary: str = ""        # plain-text stats block, surfaced in the HTML report


@dataclass
class Viewer:
    """Registry entry binding a name + its required CSV(s) to a build function."""
    name: str
    title: str
    required_csvs: tuple
    build: Callable[[TraceContext], ViewerResult]

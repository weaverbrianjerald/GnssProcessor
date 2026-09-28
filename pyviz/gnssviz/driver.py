"""Run dispatch: the Python analogue of ``view_all.m``.

Holds the viewer registry, decides which viewers run for a given trace folder
(skipping any whose required CSV is missing or empty, matching ``view_all``),
and returns the results.  Rendering and report assembly are left to the caller,
so the same dispatch feeds both the offline HTML report and (later) an
interactive or real-time front-end.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Sequence, Tuple

from .trace_paths import TraceContext
from .viewers import REGISTRY
from .viewers.base import Viewer, ViewerResult, ViewerSkip


@dataclass
class RunOutput:
    ran: List[Tuple[Viewer, ViewerResult]] = field(default_factory=list)
    skipped: List[Tuple[str, str]] = field(default_factory=list)   # (name, reason)
    failed: List[Tuple[str, str]] = field(default_factory=list)    # (name, error)


def run_viewers(
    ctx: TraceContext,
    *,
    only: Optional[Sequence[str]] = None,
    include_disabled: bool = False,
    registry: Sequence[Viewer] = REGISTRY,
) -> RunOutput:
    """Build every applicable viewer for the run at ``ctx``.

    Parameters
    ----------
    only:
        Optional whitelist of viewer names to run (others skipped silently).
        An explicit name always wins over ``default_enabled``: requesting a
        default-off viewer by name runs it.
    include_disabled:
        When no ``only`` whitelist is given, also run viewers whose
        ``default_enabled`` is ``False`` (the ``--all`` escape hatch).  Ignored
        when ``only`` is set.
    """
    out = RunOutput()
    wanted = set(only) if only is not None else None

    for viewer in registry:
        if wanted is not None:
            if viewer.name not in wanted:
                continue
        elif not viewer.default_enabled and not include_disabled:
            out.skipped.append(
                (viewer.name, f"off by default (run with --only {viewer.name} or --all)"))
            continue
        # A required input may live in the run's log dir (trace CSVs) or in the
        # answer-key truth dir (the exam viewers' truth_*.csv / scenario_summary).
        # has_truth is False whenever no truth_dir is bound, so a plain processor
        # run skips every exam viewer.
        missing = [c for c in viewer.required_csvs
                   if not (ctx.has_data(c) or ctx.has_truth(c))]
        if missing:
            out.skipped.append((viewer.name, f"missing/empty {', '.join(missing)}"))
            continue
        try:
            result = viewer.build(ctx)
            out.ran.append((viewer, result))
        except ViewerSkip as skip:    # viewer opted out at build time (no roster/IONEX/etc.)
            out.skipped.append((viewer.name, str(skip)))
        except Exception as exc:  # match view_all: report + continue
            out.failed.append((viewer.name, f"{type(exc).__name__}: {exc}"))
    return out


def registry_by_name() -> Dict[str, Viewer]:
    return {v.name: v for v in REGISTRY}

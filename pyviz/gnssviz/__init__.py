"""gnssviz: Python port of the GnssProcessor MATLAB visualization suite.

Backend-agnostic by design:

    data layer  (io, geodesy, colormaps, trace_paths)   : pure pandas/numpy
        |
    viewers      build  Figure  specs (figures.py)        : no plotting library
        |
    backends     render Figure -> pixels                  : matplotlib today,
                                                             plotly/real-time later

Typical offline use::

    from gnssviz import TraceContext, run_viewers
    from gnssviz.backends import get_backend
    from gnssviz.report import write_html_report

    ctx = TraceContext(r"C:\\GnssProcessor\\logs")
    out = run_viewers(ctx)
    write_html_report("report.html",
                      [(v.title, r) for (v, r) in out.ran],
                      get_backend("matplotlib"),
                      log_dir=ctx.log_dir, skipped=out.skipped)
"""

from __future__ import annotations

from .driver import RunOutput, registry_by_name, run_viewers
from .trace_paths import TraceContext, default_log_dir

__all__ = [
    "TraceContext",
    "default_log_dir",
    "run_viewers",
    "RunOutput",
    "registry_by_name",
]

__version__ = "0.1.0"

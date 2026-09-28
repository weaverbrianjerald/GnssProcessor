"""gnssviz.eval: truth-referenced position scoring for the regression suite.

This subpackage is the data layer behind the "standard test run" harness: it
maps a processor run's ``state_trace.csv`` to an ENU position error against an
*independent* truth coordinate, and reduces that to a small scorecard
(bias / RMS / convergence time) suitable for A-B config diffing and
regression gating.

Two pieces:

* :mod:`gnssviz.eval.truth`: ``TruthSource`` backends.  ``SinexTruth``
  (primary; an IGS/ITRF SINEX or SSC, consumed nowhere in the C++ engine, so a
  fully independent grader) and ``SifTruth`` (fallback + cross-check; a faithful
  port of the C++ ``resolveStationCoordinate``).  ``CompositeTruth`` chains
  SINEX -> SIF.

* :mod:`gnssviz.eval.metrics`: ENU error of a ``state_trace.csv`` vs a truth
  ECEF, with a convergence window, packaged as :class:`PositionMetrics`.

See ``docs/test_suite.md`` for the design and the frame/epoch caveats.
"""

from __future__ import annotations

from .truth import (
    CompositeTruth,
    SifTruth,
    SinexTruth,
    SiteTruth,
    TruthCoord,
    TruthSource,
    make_default_truth,
)
from .metrics import (
    ErrorSeries,
    PositionMetrics,
    doy_to_decimal_year,
    enu_error_series,
    evaluate_state_trace,
    gps_to_decimal_year,
)
from .fetch import (
    ITRF2020_IGS_SSC_URL,
    ensure_itrf_ssc,
    find_sinex_files,
)
from .regression import (
    CrossCheckRow,
    MetricDelta,
    compare_metrics,
    cross_check_truth,
    index_metrics,
)
from .suites import (
    DESIGNS,
    expand_manifest_suites,
    expand_suite,
)

__all__ = [
    "TruthSource",
    "TruthCoord",
    "SinexTruth",
    "SifTruth",
    "SiteTruth",
    "CompositeTruth",
    "make_default_truth",
    "PositionMetrics",
    "ErrorSeries",
    "evaluate_state_trace",
    "enu_error_series",
    "gps_to_decimal_year",
    "doy_to_decimal_year",
    "ensure_itrf_ssc",
    "find_sinex_files",
    "ITRF2020_IGS_SSC_URL",
    "cross_check_truth",
    "compare_metrics",
    "index_metrics",
    "CrossCheckRow",
    "MetricDelta",
    "expand_suite",
    "expand_manifest_suites",
    "DESIGNS",
]

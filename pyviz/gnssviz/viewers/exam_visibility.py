"""Satellite visibility & DOP from the answer key — the exam's geometry panel.

Reads the GnssSimulator answer key ``truth_visibility.csv`` (one row per epoch:
per-constellation counts + the position/geometry DOPs the synthesizer computed
from the *true* sky) and draws, on a single time axis, how many satellites the
receiver actually saw against how strong that geometry was:

  * a per-constellation stacked area of the used counts (GPS / Galileo, coloured
    by :func:`gnssviz.palettes.system_color`) with a bold black total overlay on
    the LEFT axis (``Number of satellites``);
  * PDOP and GDOP as lines on the RIGHT axis (``DOP``), with a PDOP = 6 geometry-
    quality reference pinned to that same DOP scale.

Unlike the processor-side :mod:`dops` viewer (which reads ``dop_trace.csv``), this
is the *truth* geometry — the sky the simulator built — so it degrades cleanly on
a non-exam run (``truth_visibility.csv`` absent → :class:`ViewerSkip`).  The storm
window overlays the time series so the reader can tell whether a DOP excursion is
geometry or ionosphere.
"""

from __future__ import annotations

import numpy as np

from ..figures import Axes, Figure, Legend, Line, StackedArea
from ..io import add_utc
from ..palettes import system_color
from .base import Viewer, ViewerResult
from . import exam_common as ec

_PDOP_THRESHOLD = 6.0

# system label, CSV count column, RGB  (canonical stacked order)
_SYS = [
    ("GPS", "n_gps", system_color("GPS")),
    ("Galileo", "n_gal", system_color("Galileo")),
]
# CSV column, legend label, RGB, line width  (right-axis DOP lines)
_DOPS = [
    ("pdop", "PDOP", (0.85, 0.20, 0.20), 1.6),
    ("gdop", "GDOP", (0.45, 0.10, 0.10), 1.2),
]

_C_TOTAL = (0.0, 0.0, 0.0)
_C_REF = (0.55, 0.55, 0.55)


def build_exam_visibility(ctx) -> ViewerResult:
    df = ec.require_truth(ctx, "truth_visibility.csv")
    df = add_utc(df, sort=True)
    t = df["utc"]
    storm = ec.storm_vlines(ctx)

    # Constellations that ever contributed a satellite, in canonical order.
    sys_present = [(lbl, col, c) for (lbl, col, c) in _SYS
                   if col in df.columns and (df[col].to_numpy(float) > 0).any()]

    ax = Axes(title="Satellite visibility & DOP",
              xlabel="GPS Time", ylabel="Number of satellites",
              y2label="DOP", legend=Legend(loc="bestoutside"))

    # LEFT axis: stacked per-constellation counts + bold total overlay.
    if sys_present:
        ys = [df[col].to_numpy(float) for (_, col, _) in sys_present]
        ax.traces.append(
            StackedArea(x=t, ys=ys,
                        labels=[lbl for (lbl, _, _) in sys_present],
                        colors=[c for (_, _, c) in sys_present], alpha=0.85))
    if "n_sats" in df.columns:
        n_sats = df["n_sats"].to_numpy(float)
        ax.traces.append(Line(x=t, y=n_sats, color=_C_TOTAL, width=1.6,
                              label="Total"))
        top = max(float(np.nanmax(n_sats)) + 2, 8)
        ax.ylim = (0, top)

    # RIGHT axis: position/geometry DOPs + a PDOP=6 reference on the DOP scale.
    # (HLine can only draw on the primary axis, so the reference is a flat Line
    #  pinned to axis="right" — otherwise a y=6 line would land on the count axis.)
    for col, lbl, c, lw in _DOPS:
        if col in df.columns:
            ax.traces.append(Line(x=t, y=df[col].to_numpy(float), color=c,
                                  width=lw, axis="right", label=lbl))
    if "pdop" in df.columns:
        ax.traces.append(Line(x=t, y=np.full(len(df), _PDOP_THRESHOLD),
                              color=_C_REF, width=1.0, style="--", axis="right",
                              label=f"PDOP = {_PDOP_THRESHOLD:.0f}"))

    ax.traces += storm
    fig = Figure(name="Satellite visibility & DOP", axes=[ax], width=14, height=4.6)

    return ViewerResult(figures=[fig], summary=_summary(df))


def _summary(df) -> str:
    t = df["utc"]
    out = ["=== Satellite visibility & DOP (answer key) ===",
           f"Epochs     : {len(df)}",
           f"Span (GPS) : {t.iloc[0]} -> {t.iloc[-1]}",
           "",
           f"{'Metric':<12} {'mean':>8} {'median':>8} {'max':>8}",
           "-" * 40]
    metrics = [("n_sats", "Total sats"), ("n_gps", "GPS sats"),
               ("n_gal", "Galileo sats"), ("pdop", "PDOP"), ("gdop", "GDOP")]
    for col, lbl in metrics:
        if col not in df.columns:
            continue
        v = df[col].to_numpy(float)
        v = v[np.isfinite(v)]
        if v.size:
            out.append(f"{lbl:<12} {np.mean(v):8.3f} {np.median(v):8.3f} {np.max(v):8.3f}")

    if "pdop" in df.columns:
        p = df["pdop"].to_numpy(float)
        p = p[np.isfinite(p)]
        if p.size:
            good = int(np.sum(p <= _PDOP_THRESHOLD))
            out += ["",
                    f"PDOP <= {_PDOP_THRESHOLD:.0f} : {good} epochs "
                    f"({100.0 * good / p.size:.1f}%)"]
    return "\n".join(out)


viewer = Viewer(name="exam_visibility", title="Satellite visibility & DOP",
                required_csvs=("truth_visibility.csv",),
                build=build_exam_visibility)

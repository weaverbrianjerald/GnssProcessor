"""DOP / satellite-count viewer: the port of ``plotDops.m``.

Reads ``dop_trace.csv`` (one row per epoch) and produces three figures:
  1. used-satellites: per-constellation stacked area + bold total overlay;
  2. position DOPs (PDOP/GDOP/HDOP/VDOP/TDOP) with a PDOP=6 reference line;
  3. Teunissen ADOP on a log y-axis, with the ambiguity count on a 2nd y-axis.
Plus a text summary (epoch span, per-metric mean/median/max, per-constellation
mean count, PDOP good/weak split).
"""

from __future__ import annotations

import numpy as np

from ..io import add_utc, load_trace, require_cols
from ..figures import Axes, Figure, HLine, Legend, Line, StackedArea
from .base import Viewer, ViewerResult

_PDOP_THRESHOLD = 6.0

# system label, CSV column, RGB  (canonical column order)
_SYS = [
    ("GPS", "n_used_gps", (0.15, 0.45, 0.85)),
    ("Galileo", "n_used_galileo", (0.20, 0.65, 0.30)),
    ("GLONASS", "n_used_glonass", (0.85, 0.20, 0.20)),
    ("BeiDou", "n_used_beidou", (0.55, 0.30, 0.75)),
    ("QZSS", "n_used_qzss", (0.95, 0.55, 0.10)),
    ("SBAS", "n_used_sbas", (0.50, 0.50, 0.50)),
]
# column, label, RGB, line width
_DOPS = [
    ("pdop", "PDOP", (0.85, 0.20, 0.20), 1.6),
    ("gdop", "GDOP", (0.45, 0.10, 0.10), 1.2),
    ("hdop", "HDOP", (0.15, 0.45, 0.85), 1.0),
    ("vdop", "VDOP", (0.20, 0.65, 0.30), 1.0),
    ("tdop", "TDOP", (0.50, 0.50, 0.50), 0.8),
]


def build_dops(ctx) -> ViewerResult:
    df = load_trace(ctx.path("dop_trace.csv"))
    require_cols(df, ("week", "tow_s", "n_used_total", "pdop", "adop_m"), "dop_trace.csv")
    df = add_utc(df, sort=True)
    t = df["utc"]
    figures = []

    # Systems that ever had a non-zero used count, in canonical order.
    sys_present = [(lbl, col, c) for (lbl, col, c) in _SYS
                   if col in df.columns and (df[col].to_numpy(float) > 0).any()]

    # Figure 1: stacked sat counts + total overlay.
    if sys_present:
        ys = [df[col].to_numpy(float) for (_, col, _) in sys_present]
        ax = Axes(title="Used satellites per epoch (per system, stacked) + total",
                  xlabel="GPS Time", ylabel="Number of used satellites",
                  legend=Legend(loc="bestoutside"))
        ax.traces = [
            StackedArea(x=t, ys=ys, labels=[lbl for (lbl, _, _) in sys_present],
                        colors=[c for (_, _, c) in sys_present], alpha=0.85),
            Line(x=t, y=df["n_used_total"].to_numpy(float), color=(0, 0, 0),
                 width=1.5, label="Total"),
        ]
        top = max(float(np.nanmax(df["n_used_total"].to_numpy(float))) + 2, 8)
        ax.ylim = (0, top)
        figures.append(Figure(name="Used satellites per epoch", axes=[ax],
                              width=14, height=4.6))

    # Figure 2: position DOPs + PDOP=6 reference.
    ax = Axes(title="Position dilution-of-precision indicators",
              xlabel="GPS Time", ylabel="DOP (unitless)", legend=Legend(loc="bestoutside"))
    for col, lbl, c, lw in _DOPS:
        if col in df.columns:
            ax.traces.append(Line(x=t, y=df[col].to_numpy(float), color=c,
                                  width=lw, label=lbl))
    ax.traces.append(HLine(y=_PDOP_THRESHOLD, label=f"PDOP = {_PDOP_THRESHOLD:.1f}",
                           color=(0.6, 0.6, 0.6)))
    ax.ylim = (0, None)
    figures.append(Figure(name="Position DOPs", axes=[ax], width=14, height=4.6))

    # Figure 3: ADOP (log y) + ambiguity count on the right axis.
    adop = df["adop_m"].to_numpy(float)
    adop = np.where(np.isfinite(adop) & (adop > 0), adop, np.nan)
    ax = Axes(title=r"Teunissen ambiguity DOP  $(\det Q_a)^{1/(2n)}$  [m]",
              xlabel="GPS Time", ylabel="ADOP (m, log scale)", yscale="log",
              legend=Legend(loc="bestoutside"))
    ax.traces = [Line(x=t, y=adop, color=(0.15, 0.35, 0.75), width=1.2, label="ADOP (m)")]
    if "n_ambiguities" in df.columns:
        ax.traces.append(Line(x=t, y=df["n_ambiguities"].to_numpy(float),
                              color=(0.7, 0.7, 0.7), width=0.7, axis="right",
                              label="# ambiguities (ADOP basis)"))
        ax.y2label = "# ambiguities (right axis)"
    figures.append(Figure(name="Ambiguity DOP (ADOP)", axes=[ax], width=14, height=4.6))

    return ViewerResult(figures=figures, summary=_summary(df, sys_present))


def _summary(df, sys_present) -> str:
    t = df["utc"]
    out = ["=== DOP / Sat-Count Trace Summary ===",
           f"Epochs     : {len(df)}",
           f"Span (GPS) : {t.iloc[0]} -> {t.iloc[-1]}",
           "",
           f"{'Metric':<12} {'mean':>8} {'median':>8} {'max':>8}",
           "-" * 40]
    metrics = [("n_used_total", "Total sats"), ("pdop", "PDOP"), ("hdop", "HDOP"),
               ("vdop", "VDOP"), ("tdop", "TDOP"), ("gdop", "GDOP"), ("adop_m", "ADOP (m)")]
    for col, lbl in metrics:
        if col not in df.columns:
            continue
        v = df[col].to_numpy(float)
        v = v[np.isfinite(v)]
        if v.size:
            out.append(f"{lbl:<12} {np.mean(v):8.3f} {np.median(v):8.3f} {np.max(v):8.3f}")

    out += ["", "=== Mean used sats per constellation ==="]
    for lbl, col, _ in sys_present:
        out.append(f"  {lbl:<8} : {np.mean(df[col].to_numpy(float)):.2f}")

    if "pdop" in df.columns:
        p = df["pdop"].to_numpy(float)
        p = p[np.isfinite(p)]
        good = int(np.sum(p <= _PDOP_THRESHOLD))
        weak = int(np.sum(p > _PDOP_THRESHOLD))
        if good + weak:
            out += ["",
                    f"PDOP <= {_PDOP_THRESHOLD:.1f} : {good} epochs ({100*good/(good+weak):.1f}%)",
                    f"PDOP >  {_PDOP_THRESHOLD:.1f} : {weak} epochs ({100*weak/(good+weak):.1f}%)"]
    return "\n".join(out)


viewer = Viewer(name="dops", title="DOPs & satellite counts",
                required_csvs=("dop_trace.csv",), build=build_dops)

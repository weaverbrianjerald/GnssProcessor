"""Sky plot of az/el tracks + cycle-slip events: the exam's slip-geometry panel.

Draws a single polar sky plot (compass-north zero, clockwise, elevation rings)
from the GnssSimulator answer key's per-satellite pierce geometry
(``truth_atmo.csv``, required via :func:`~.exam_common.require_truth`):

  * one :class:`~gnssviz.figures.PolarScatter` of ``(azimuth, 90-elevation)`` per
    constellation, coloured by :func:`~gnssviz.palettes.system_color` (GPS blue,
    Galileo green); the storm-window samples (``in_storm_window == 1``) are drawn
    darker and larger so the disturbed-geometry epochs stand out;
  * every truth cycle slip (``truth_slips.csv``, optional) placed at the
    ``(az, el)`` it recovers by joining to the atmo grid on the integer
    ``(week, tow-ms)`` key + satellite (SPEC R5, via :func:`~.exam_common.tow_key`),
    coloured by :func:`~.exam_common.slip_class` (flagged / unflagged / scripted /
    false-alarm) and sized by ``|Delta cycles|``.  A ``Delta=0`` false alarm still
    plots at the size floor.

Because a :class:`~gnssviz.figures.PolarScatter` carries no per-class legend, and
the polar backends render only PolarScatter traces, dropping any TextBox on a
polar axes, the colour key (constellation colours + slip class->count) is drawn
as TextBox lines on a companion frameless axes beneath the sky plot.  When
``truth_slips.csv`` is absent the viewer still draws the tracks (no crash); slips
whose ``(week, tow, satellite)`` finds no atmo ``(az, el)`` are counted and
reported but cannot be placed.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..figures import Axes, Figure, PolarScatter, TextBox
from ..io import first_col
from ..palettes import SYSTEM_ORDER, system_color
from .base import Viewer, ViewerResult, ViewerSkip
from . import exam_common as ec

# Satellite-ID letter -> constellation (matches palettes.SYSTEM_ORDER names).
_SYS_OF_LETTER = {"G": "GPS", "E": "Galileo", "R": "GLONASS", "C": "BeiDou",
                  "J": "QZSS", "S": "SBAS", "I": "IRNSS"}

_BASE_SIZE = 3.0      # quiet-epoch track marker (area units; backends sqrt-scale)
_STORM_SIZE = 9.0     # in-storm sample emphasis


def _darken(rgb, f: float = 0.55):
    """A darker shade of an RGB tuple (in-storm emphasis over the base colour)."""
    return tuple(float(f) * float(c) for c in rgb)


def _slip_size(delta_cycles) -> float:
    """Marker size ~ ``(6 + 6*log10(max(1, |Delta|)))`` mapped to an area value.

    The parenthesised term is a diameter proxy (~6 for a Delta=0 false alarm up to
    ~18 for a large slip); squaring it makes the *area* fed to the backends grow
    with ``|Delta|`` while staying visibly larger than the base tracks.
    """
    try:
        d = abs(float(delta_cycles))
    except (TypeError, ValueError):
        d = 0.0
    diam = 3.5 + 3.0 * np.log10(max(1.0, d))
    return float(diam * diam)


def build_exam_skyplot_slips(ctx) -> ViewerResult:
    atmo = ec.require_truth(ctx, "truth_atmo.csv")

    az = first_col(atmo, "az_deg")
    el = first_col(atmo, "elev_deg")
    if az is None or el is None or "satellite" not in atmo.columns:
        raise ViewerSkip("truth_atmo.csv lacks az_deg / elev_deg / satellite "
                         "(no sky geometry to plot)")

    atmo = atmo.copy()
    atmo["satellite"] = atmo["satellite"].astype(str)
    atmo["az_deg"] = pd.to_numeric(atmo["az_deg"], errors="coerce")
    atmo["elev_deg"] = pd.to_numeric(atmo["elev_deg"], errors="coerce")
    atmo["system"] = atmo["satellite"].str[:1].map(_SYS_OF_LETTER).fillna("Unknown")
    storm = first_col(atmo, "in_storm_window")
    atmo["_storm"] = (pd.to_numeric(storm, errors="coerce").fillna(0.0) > 0.5
                      if storm is not None else False)

    # Constellations in canonical order, any unrecognised system (Unknown) last.
    present = list(dict.fromkeys(atmo["system"].tolist()))
    sys_present = [s for s in SYSTEM_ORDER if s in present]
    sys_present += [s for s in present if s not in sys_present]

    sky = Axes(projection="polar",
               title="Sky plot: az/el tracks + cycle slips",
               theta_zero="N", theta_dir=-1, rlim=(0.0, 90.0),
               rticks=[0, 30, 60, 90], rtick_labels=["90", "60", "30", "0"],
               x_is_datetime=False)
    traces: list = []

    # --- Base tracks: quiet (non-storm) then in-storm emphasis, per system. ---
    for sysname in sys_present:
        d = atmo[atmo["system"] == sysname]
        base = d[~d["_storm"]]
        ok = base["az_deg"].notna() & base["elev_deg"].notna()
        if ok.any():
            traces.append(PolarScatter(
                theta_deg=base.loc[ok, "az_deg"].to_numpy(float),
                r=90.0 - base.loc[ok, "elev_deg"].to_numpy(float),
                color=system_color(sysname), size=_BASE_SIZE, label=sysname))
        d_storm = d[d["_storm"]]
        oks = d_storm["az_deg"].notna() & d_storm["elev_deg"].notna()
        if oks.any():
            traces.append(PolarScatter(
                theta_deg=d_storm.loc[oks, "az_deg"].to_numpy(float),
                r=90.0 - d_storm.loc[oks, "elev_deg"].to_numpy(float),
                color=_darken(system_color(sysname)), size=_STORM_SIZE,
                label=f"{sysname} (storm)"))

    # --- Slip overlay: recover each slip's (az, el) from the atmo grid. ---
    slips = ec.truth_frame(ctx, "truth_slips.csv")
    slip_counts: dict = {}
    n_slips = 0
    n_unmatched = 0
    have_slips = slips is not None and {"week", "tow_s", "satellite",
                                        "delta_cycles"}.issubset(slips.columns)
    if have_slips:
        slips = slips.copy()
        slips["satellite"] = slips["satellite"].astype(str)
        slips["_key"] = ec.tow_key(slips["week"], slips["tow_s"])
        slips["_class"] = ec.classify_slips(slips)
        n_slips = len(slips)

        lut = atmo.copy()
        lut["_key"] = ec.tow_key(lut["week"], lut["tow_s"])
        lut = (lut[["_key", "satellite", "az_deg", "elev_deg"]]
               .dropna(subset=["az_deg", "elev_deg"])
               .drop_duplicates(["_key", "satellite"])
               .rename(columns={"az_deg": "_az", "elev_deg": "_el"}))
        joined = slips.merge(lut, on=["_key", "satellite"], how="left")

        matched = joined["_az"].notna() & joined["_el"].notna()
        n_unmatched = int((~matched).sum())
        jm = joined[matched]
        slip_counts = jm["_class"].value_counts().to_dict()

        # One PolarScatter per slip so each carries its own |Delta|-scaled size
        # with a fixed per-class RGB (color=, not c=: no cmap for slip classes).
        for _, row in jm.iterrows():
            cls = str(row["_class"])
            traces.append(PolarScatter(
                theta_deg=[float(row["_az"])],
                r=[90.0 - float(row["_el"])],
                color=ec.SLIP_CLASS_COLORS.get(cls, (0.0, 0.0, 0.0)),
                size=_slip_size(row["delta_cycles"])))

    sky.traces = traces

    legend_ax = _legend_axes(sys_present, atmo, slip_counts, n_slips,
                             n_unmatched, have_slips)

    fig = Figure(name="Sky plot - slips",
                 title="Sky plot: satellite geometry + cycle-slip events",
                 axes=[sky, legend_ax], width=9.0, height=10.0, link_x=False)

    summary = _summary(atmo, sys_present, have_slips, n_slips, slip_counts,
                       n_unmatched)
    return ViewerResult(figures=[fig], summary=summary)


# --------------------------------------------------------------------------- #
#  Companion legend axes (PolarScatter has no per-class legend of its own)
# --------------------------------------------------------------------------- #
def _legend_axes(sys_present, atmo, slip_counts, n_slips, n_unmatched,
                 have_slips) -> Axes:
    ax = Axes(axis_off=True, grid=False, x_is_datetime=False)
    lines: list = []
    y, step = 0.96, 0.072

    lines.append(TextBox(x=0.02, y=y, text="Base tracks (colour = constellation):",
                         bold=True, va="top"))
    y -= step
    for s in sys_present:
        n = int((atmo["system"] == s).sum())
        lines.append(TextBox(x=0.05, y=y, text=f"{s}: {n} samples",
                             color=system_color(s), va="top", boxed=True))
        y -= step
    lines.append(TextBox(x=0.05, y=y,
                         text="storm-window samples: darker + larger",
                         va="top"))
    y -= step * 1.3

    if have_slips:
        lines.append(TextBox(
            x=0.02, y=y, bold=True, va="top",
            text=f"Cycle slips (size ~ |Delta cycles|), {n_slips} total:"))
        y -= step
        for cls in ec.SLIP_CLASS_ORDER:
            c = int(slip_counts.get(cls, 0))
            if c:
                lines.append(TextBox(x=0.05, y=y, text=f"{cls}: {c}",
                                     color=ec.SLIP_CLASS_COLORS[cls],
                                     va="top", boxed=True))
                y -= step
        if n_unmatched:
            lines.append(TextBox(x=0.05, y=y, va="top", color=(0.4, 0.4, 0.4),
                                 text=f"unmatched (no atmo az/el): {n_unmatched}"))
    else:
        lines.append(TextBox(x=0.02, y=y, va="top", color=(0.4, 0.4, 0.4),
                             text="truth_slips.csv absent - tracks only"))

    ax.traces = lines
    return ax


# --------------------------------------------------------------------------- #
#  Summary
# --------------------------------------------------------------------------- #
def _summary(atmo, sys_present, have_slips, n_slips, slip_counts,
             n_unmatched) -> str:
    sats = sorted(atmo["satellite"].unique())
    n_storm = int(atmo["_storm"].sum())
    out = ["=== Sky plot: geometry + cycle slips ==="]
    out.append(f"Satellites plotted : {len(sats)} ({', '.join(sats)})")
    out.append(f"Atmo samples       : {len(atmo)} ({n_storm} in storm window)")
    out.append(f"Constellations     : {', '.join(sys_present)}")
    if not have_slips:
        out.append("Cycle slips        : truth_slips.csv absent (tracks only)")
    else:
        parts = [f"{k}={int(slip_counts.get(k, 0))}"
                 for k in ec.SLIP_CLASS_ORDER if slip_counts.get(k, 0)]
        s = f"{n_slips} total"
        if parts:
            s += " (" + ", ".join(parts) + ")"
        if n_unmatched:
            s += f"; {n_unmatched} unmatched (no atmo az/el)"
        out.append(f"Cycle slips        : {s}")
    return "\n".join(out)


viewer = Viewer(name="exam_skyplot_slips",
                title="Sky plot: geometry & cycle slips",
                required_csvs=("truth_atmo.csv",),
                build=build_exam_skyplot_slips)

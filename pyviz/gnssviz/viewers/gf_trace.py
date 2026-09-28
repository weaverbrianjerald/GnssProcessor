"""GF-slip trace viewer: the port of ``view_gf_trace.m`` / ``plot_gf_trace``.

Reads ``gf_slip_trace.csv`` (written by the STAGED ``GfSlipDetector``, rarely
wired in) and reproduces the four diagnostic figures of the MATLAB script. Each
figure is faceted *by constellation*: one :class:`Axes` (tile) per system that
appears in the trace, matching MATLAB's ``tiledlayout(1, numSystems)`` /
``tiledlayout(2, numSystems)`` row-major layout.

Expected CSV header (from the detector)::

    week,tow_s,satellite,system,elevation_deg,
    sig1,sig2,gf_m,td2_gf_m,
    mad_sigma_m,threshold_m,arc_count,
    ivi_m,iono_stable,
    is_slip,suppressed

Figures
-------
1. **Raw GF observable** ``GF = L1 - L2`` (m), one tile per constellation, a
   scatter per per-arc *key* (``<sat>_<sig1>_<sig2>``).
2. **TD2 GF** (2nd-order time-differenced GF) with the adaptive ``±threshold``
   envelope (dashed, lightened per-key colour), confirmed slips marked with a
   red ``x``, and suppressed detections with a hollow orange diamond.
3. **Detector diagnostics**: a 2-row layout, MAD ``σ`` (top row, one tile per
   system) and arc count (bottom row, one tile per system).
4. **Ionospheric Variability Index (IVI)**: one tile per system, one IVI value
   per epoch (first satellite per epoch), unstable epochs over-plotted in red,
   and a dashed ``IVI threshold`` reference line at 0.06 m.

Colour / marker scheme (faithful to ``key_style``)
--------------------------------------------------
Colour is keyed to the *satellite index* within the global satellite set
(``distinguishable_colors`` over ``max(8, N)`` satellites), so every arc of one
satellite shares a colour; the marker cycles by the per-system per-key index over
``{o,s,d,^,v,>,<,p,h,x,+}``.  Sentinel ``-1`` values in ``mad_sigma_m`` /
``threshold_m`` are mapped to NaN for clean plots.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import add_utc, load_trace, require_cols
from ..figures import Axes, Figure, HLine, Legend, Scatter, TextBox
from ..palettes import distinguishable_colors
from .base import Viewer, ViewerResult

# Marker cycle mirroring the MATLAB ``markerList`` (figures.py / matplotlib
# share these glyph codes for the ones used here).
_MARKERS = ["o", "s", "d", "^", "v", ">", "<", "p", "h", "x", "+"]

# Required columns (the ``need`` set in the MATLAB script).
_NEED = ["week", "tow_s", "satellite", "system", "sig1", "sig2",
         "gf_m", "td2_gf_m", "threshold_m", "is_slip", "suppressed",
         "ivi_m", "iono_stable"]

_RED = (1.0, 0.0, 0.0)
_ORANGE = (1.0, 0.6, 0.0)
_BLACK = (0.0, 0.0, 0.0)
_IVI_THRESHOLD = 0.06


def _key_style(this_key, sat_index, colors, fallback_idx):
    """Consistent (colour, marker) for a per-arc key: port of ``key_style``.

    Colour follows the *satellite* index (so all arcs of a satellite share a
    colour); marker cycles by the per-system per-key order index.  ``fallback_idx``
    and ``sat_index`` are 0-based here (MATLAB used 1-based).
    """
    sat = str(this_key).split("_", 1)[0]
    sat_idx = sat_index.get(sat, fallback_idx)
    c = colors[sat_idx % len(colors)]
    m = _MARKERS[fallback_idx % len(_MARKERS)]
    return c, m


def _lighten(c):
    """``min(c + 0.4, 1.0)`` per channel (the MATLAB ``clite`` envelope tint)."""
    return tuple(min(ch + 0.4, 1.0) for ch in c)


def build_gf_trace(ctx) -> ViewerResult:
    df = load_trace(ctx.path("gf_slip_trace.csv"))
    require_cols(df, _NEED, "gf_trace")

    # Numeric coercion (mirrors the explicit ``double(...)`` casts).
    for col in ("week", "tow_s", "elevation_deg", "gf_m", "td2_gf_m",
                "mad_sigma_m", "threshold_m", "arc_count", "ivi_m",
                "iono_stable", "is_slip", "suppressed"):
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    # Sentinel (-1) -> NaN for the diagnostics that use it.
    for col in ("mad_sigma_m", "threshold_m"):
        if col in df.columns:
            df.loc[df[col] < 0, col] = np.nan

    # Per-arc key "<sat>_<sig1>_<sig2>" and GPS-time axis.
    df["key"] = (df["satellite"].astype(str) + "_"
                 + df["sig1"].astype(str) + "_" + df["sig2"].astype(str))
    df = add_utc(df)

    # Shared colour/marker setup: distinguishable_colors over max(8, N) sats.
    # Sorted satellite order -> stable colour-by-sat matching MATLAB's satCats.
    sat_cats = sorted(pd.unique(df["satellite"].astype(str)))
    colors = distinguishable_colors(max(8, len(sat_cats)))
    sat_index = {s: i for i, s in enumerate(sat_cats)}

    # Constellation facet order (categories() = sorted unique, like MATLAB
    # categorical).
    systems = sorted(pd.unique(df["system"].astype(str)))

    figures = [
        _figure_raw_gf(df, systems, sat_index, colors),
        _figure_td2(df, systems, sat_index, colors),
        _figure_diagnostics(df, systems, sat_index, colors),
        _figure_ivi(df, systems),
    ]

    return ViewerResult(figures=figures, summary=_summary(df, sat_cats, systems))


# --------------------------------------------------------------------------- #
#  Figure 1: raw GF time series, one tile per constellation
# --------------------------------------------------------------------------- #
def _figure_raw_gf(df, systems, sat_index, colors):
    axes = []
    for sysName in systems:
        ax = Axes(title=sysName, xlabel="GPS Time", ylabel="GF  (m)",
                  legend=Legend(loc="best"))
        sub = df[df["system"].astype(str) == sysName]
        if sub.empty:
            ax.traces.append(_no_data())
            axes.append(ax)
            continue
        for k, this_key in enumerate(pd.unique(sub["key"])):
            kk = sub[sub["key"] == this_key]
            c, m = _key_style(this_key, sat_index, colors, k)
            ax.traces.append(Scatter(
                x=kk["utc"], y=kk["gf_m"].to_numpy(float), label=str(this_key),
                color=c, size=18.0, marker=m, alpha=0.6))
        axes.append(ax)
    return Figure(name="GF: Raw GF (L1-L2)",
                  title="Geometry-Free Observable  GF = L1 - L2  (m)",
                  axes=axes, width=14, height=4.5)


# --------------------------------------------------------------------------- #
#  Figure 2: TD2 GF with ±threshold, slips, suppressed
# --------------------------------------------------------------------------- #
def _figure_td2(df, systems, sat_index, colors):
    axes = []
    for sysName in systems:
        ax = Axes(title=sysName, xlabel="GPS Time", ylabel="TD2_GF  (m)",
                  legend=Legend(loc="best"))
        sub = df[df["system"].astype(str) == sysName]
        if sub.empty:
            ax.traces.append(_no_data())
            axes.append(ax)
            continue
        for k, this_key in enumerate(pd.unique(sub["key"])):
            kk = sub[sub["key"] == this_key]
            c, m = _key_style(this_key, sat_index, colors, k)

            ax.traces.append(Scatter(
                x=kk["utc"], y=kk["td2_gf_m"].to_numpy(float), label=str(this_key),
                color=c, size=18.0, marker=m, alpha=0.6))

            # ±threshold envelope, lightened per-key colour, drawn as scatter
            # markers: no Line-with-NaN-gaps primitive matches the per-sample
            # MATLAB plot of a non-monotone-time arc, so scatter keeps it faithful
            # to the threshold *samples* without spanning data gaps.
            clite = _lighten(c)
            thr = kk["threshold_m"].to_numpy(float)
            ax.traces.append(Scatter(
                x=kk["utc"], y=thr, color=clite, size=4.0, marker=".",
                alpha=0.4, show_in_legend=False))
            ax.traces.append(Scatter(
                x=kk["utc"], y=-thr, color=clite, size=4.0, marker=".",
                alpha=0.4, show_in_legend=False))

            # Confirmed slips: red X.
            slip = kk[kk["is_slip"] == 1]
            if not slip.empty:
                ax.traces.append(Scatter(
                    x=slip["utc"], y=slip["td2_gf_m"].to_numpy(float),
                    color=_RED, size=120.0, marker="x", show_in_legend=False))

            # Suppressed detections: hollow orange diamond.
            supp = kk[kk["suppressed"] == 1]
            if not supp.empty:
                ax.traces.append(Scatter(
                    x=supp["utc"], y=supp["td2_gf_m"].to_numpy(float),
                    color=_ORANGE, size=80.0, marker="d", show_in_legend=False))
        axes.append(ax)
    return Figure(
        name="GF: TD2 GF + Threshold",
        title="2nd-Order Time-Differenced GF  (TD2_GF)  with Adaptive Threshold",
        axes=axes, width=14, height=4.5)


# --------------------------------------------------------------------------- #
#  Figure 3: MAD sigma (row 1) and arc count (row 2), one column per system
# --------------------------------------------------------------------------- #
def _figure_diagnostics(df, systems, sat_index, colors):
    axes = []
    # Row 1: MAD sigma.
    for sysName in systems:
        ax = Axes(title=f"{sysName} — MAD σ", xlabel="GPS Time",
                  ylabel="MAD σ  (m)")
        sub = df[df["system"].astype(str) == sysName]
        if sub.empty:
            ax.title = f"{sysName} MAD σ"
            ax.traces.append(_no_data())
            axes.append(ax)
            continue
        for k, this_key in enumerate(pd.unique(sub["key"])):
            kk = sub[sub["key"] == this_key]
            c, m = _key_style(this_key, sat_index, colors, k)
            ax.traces.append(Scatter(
                x=kk["utc"], y=kk["mad_sigma_m"].to_numpy(float),
                color=c, size=12.0, marker=m, alpha=0.5, show_in_legend=False))
        axes.append(ax)
    # Row 2: arc count.
    for sysName in systems:
        ax = Axes(title=f"{sysName} — Arc Count", xlabel="GPS Time",
                  ylabel="Arc Count")
        sub = df[df["system"].astype(str) == sysName]
        if sub.empty:
            ax.title = f"{sysName} Arc Count"
            ax.traces.append(_no_data())
            axes.append(ax)
            continue
        for k, this_key in enumerate(pd.unique(sub["key"])):
            kk = sub[sub["key"] == this_key]
            c, m = _key_style(this_key, sat_index, colors, k)
            ax.traces.append(Scatter(
                x=kk["utc"], y=kk["arc_count"].to_numpy(float),
                color=c, size=12.0, marker=m, alpha=0.5, show_in_legend=False))
        axes.append(ax)
    return Figure(name="GF: MAD Sigma & Arc Count", title="GF Detector Diagnostics",
                  axes=axes, width=14, height=7.0)


# --------------------------------------------------------------------------- #
#  Figure 4: Ionospheric Variability Index, one tile per system
# --------------------------------------------------------------------------- #
def _figure_ivi(df, systems):
    axes = []
    for sysName in systems:
        ax = Axes(title=f"{sysName} — Iono Variability Index",
                  xlabel="GPS Time", ylabel="IVI  (m)")
        sub = df[df["system"].astype(str) == sysName]
        if sub.empty:
            ax.title = sysName
            ax.traces.append(_no_data())
            axes.append(ax)
            continue
        # One IVI value per epoch: first satellite per epoch (unique utc, stable).
        epoch = sub.drop_duplicates(subset="utc", keep="first")

        ax.traces.append(Scatter(
            x=epoch["utc"], y=epoch["ivi_m"].to_numpy(float),
            color=_BLACK, size=20.0, marker="o", alpha=0.5, show_in_legend=False))

        unstable = epoch[epoch["iono_stable"] == 0]
        if not unstable.empty:
            ax.traces.append(Scatter(
                x=unstable["utc"], y=unstable["ivi_m"].to_numpy(float),
                color=_RED, size=50.0, marker="o", alpha=0.3,
                show_in_legend=False))

        ax.traces.append(HLine(y=_IVI_THRESHOLD, label="IVI threshold",
                               color=_RED, style="--"))
        axes.append(ax)
    return Figure(name="GF: Ionospheric Variability Index",
                  title="Ionospheric Variability Index  (IVI)",
                  axes=axes, width=14, height=3.5)


# --------------------------------------------------------------------------- #
#  Helpers
# --------------------------------------------------------------------------- #
def _no_data():
    """Centred 'No Data' label in axes-fraction coords (the MATLAB text() call)."""
    return TextBox(x=0.5, y=0.5, text="No Data", ha="center", va="center")


def _summary(df, sat_cats, systems) -> str:
    n_slip = int((df.get("is_slip", pd.Series(dtype=float)) == 1).sum())
    n_supp = int((df.get("suppressed", pd.Series(dtype=float)) == 1).sum())
    n_keys = int(df["key"].nunique())
    lines = [
        "=== GF-slip trace summary ===",
        f"Epochs (rows)      : {len(df)}",
        f"Satellites         : {len(sat_cats)}",
        f"Per-arc keys       : {n_keys}",
        f"Constellations     : {', '.join(systems) if systems else '(none)'}",
        f"Confirmed slips    : {n_slip}",
        f"Suppressed         : {n_supp}",
    ]
    return "\n".join(lines)


viewer = Viewer(name="gf_trace", title="GF slip trace",
                required_csvs=("gf_slip_trace.csv",), build=build_gf_trace)

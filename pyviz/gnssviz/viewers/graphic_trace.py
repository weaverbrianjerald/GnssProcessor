"""GRAPHIC cycle-slip-detector viewer: the port of ``view_graphic_trace.m``
(the ``plot_graphic_trace`` function therein).

Reads ``graphic_slip_trace.csv`` as written by ``GraphicSlipDetector``. Expected
header::

    week,tow_s,satellite,system,elevation_deg,
    signal,graphic_m,dg_m,
    mad_sigma_m,threshold_m,arc_count,
    slip_cycles,fractional,is_slip

The GRAPHIC observable is the divergence-free code+phase average
``G = (P + L) / 2``; the detector watches the between-epoch difference
``dG = G(t) - G(t-1)`` against a robust (MAD-derived) per-arc threshold and flags
a slip when ``|dG|`` breaches it. This viewer reproduces the three MATLAB
figures, one tile per constellation:

  1. **dG + threshold**: ``dG`` as a translucent per-key scatter, with the
     ``±threshold`` envelope drawn as faint lines and detected slips marked with
     a heavy red ``x``.
  2. **Detector diagnostics**: two stacked rows, per-key MAD ``σ`` (top) and
     per-key arc count (bottom), one column-tile per constellation. Because the
     backend stacks axes vertically rather than in a 2-D tile grid, the tiles are
     emitted in reading order: all MAD-σ tiles first, then all arc-count tiles.
  3. **Slip events**: the estimated integer slip magnitude ``slip_cycles``
     (``dN = round(2·dG / λ)``) at each flagged epoch, per key.

Styling fidelity
-----------------
Colour and marker are assigned the same way as the MATLAB ``key_style`` helper:

* Satellites are gathered as ``unique(categorical(T.satellite))``, i.e. the
  sorted-unique satellite list, and a palette of ``max(8, N)``
  ``distinguishable_colors`` is built once. A key ``"<SAT>_<SIGNAL>"`` takes the
  colour at the (mod-wrapped) index of its satellite within that sorted list.
* The marker cycles through ``{o s d ^ v > < p h x +}`` by the within-tile key
  index (the MATLAB ``fallbackIdx``), so two keys in the same constellation tile
  get distinct markers even when they share a satellite colour.

Sentinel handling matches MATLAB: ``mad_sigma_m`` / ``threshold_m`` values ``< 0``
are mapped to NaN before plotting. The constellation tile order follows the
``categories(...)`` order of the ``system`` column (i.e. its sorted-unique
values), so the produced figures track the MATLAB layout one for one.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import add_utc, load_trace, require_cols
from ..figures import Axes, Figure, Legend, Scatter, Line, TextBox
from .base import Viewer, ViewerResult
from ..palettes import distinguishable_colors

# MATLAB marker cycle (view_graphic_trace.m, line 84).
_MARKERS = ["o", "s", "d", "^", "v", ">", "<", "p", "h", "x", "+"]


def _key_style(sat, sat_cats, colors, fallback_idx):
    """Colour + marker for a key, mirroring MATLAB ``key_style``.

    ``sat`` is the satellite portion of the ``"<SAT>_<SIGNAL>"`` key.  Its index
    within the sorted-unique satellite list (``sat_cats``) selects the colour
    (mod the palette length); ``fallback_idx`` (the 0-based within-tile key
    index) selects the marker (mod the marker count) and is also the colour
    fallback when the satellite is somehow absent from ``sat_cats``.
    """
    try:
        sat_idx = sat_cats.index(sat)
    except ValueError:
        sat_idx = fallback_idx
    color = colors[sat_idx % len(colors)]
    marker = _MARKERS[fallback_idx % len(_MARKERS)]
    return color, marker


def _lighten(color):
    """MATLAB ``min(c + 0.4, 1.0)``: the faint threshold-envelope colour."""
    return tuple(min(ch + 0.4, 1.0) for ch in color)


def build_graphic_trace(ctx) -> ViewerResult:
    df = load_trace(ctx.path("graphic_slip_trace.csv"))

    # Same required-column gate as the MATLAB script (lower-cased compare).
    require_cols(
        df,
        ("week", "tow_s", "satellite", "system", "signal",
         "graphic_m", "dg_m", "threshold_m", "is_slip",
         "slip_cycles", "fractional"),
        "graphic_slip_trace.csv",
    )

    # Numerics, then map the negative sentinel (-1 and below) to NaN for MAD sigma / threshold.
    for c in ("dg_m", "mad_sigma_m", "threshold_m", "arc_count",
              "slip_cycles", "is_slip"):
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")
    if "mad_sigma_m" in df.columns:
        df.loc[df["mad_sigma_m"] < 0, "mad_sigma_m"] = np.nan
    df.loc[df["threshold_m"] < 0, "threshold_m"] = np.nan

    df["satellite"] = df["satellite"].astype(str)
    df["signal"] = df["signal"].astype(str)
    df["system"] = df["system"].astype(str)
    df["key"] = df["satellite"] + "_" + df["signal"]

    df = add_utc(df)

    # Shared palette: unique(categorical(satellite)) is the sorted-unique list;
    # palette size is max(8, N) (MATLAB line 78-83).
    sat_cats = sorted(df["satellite"].unique())
    n_sat = len(sat_cats)
    colors = distinguishable_colors(max(8, n_sat))

    # categories(categorical(system)) -> sorted-unique system list (tile order).
    system_list = sorted(df["system"].unique())

    figures = []
    figures.append(_figure_dg(df, system_list, sat_cats, colors))
    figures.append(_figure_diagnostics(df, system_list, sat_cats, colors))
    figures.append(_figure_slips(df, system_list, sat_cats, colors))

    return ViewerResult(figures=figures, summary=_summary(df, system_list))


# --------------------------------------------------------------------------- #
#  Figure 1: dG with +/- threshold, one tile per constellation
# --------------------------------------------------------------------------- #
def _figure_dg(df, system_list, sat_cats, colors) -> Figure:
    axes = []
    for sys_name in system_list:
        ax = Axes(title=sys_name, xlabel="GPS Time", ylabel="dG  (m)",
                  legend=Legend(loc="bestoutside"))
        t_sys = df[df["system"] == sys_name]
        if t_sys.empty:
            ax.traces.append(TextBox(x=0.5, y=0.5, text="No Data",
                                     ha="center", va="center"))
            axes.append(ax)
            continue

        keys_in_sys = sorted(t_sys["key"].unique())
        for k, this_key in enumerate(keys_in_sys):
            sub = t_sys[t_sys["key"] == this_key]
            sat = this_key.split("_", 1)[0]
            color, marker = _key_style(sat, sat_cats, colors, k)
            clite = _lighten(color)

            # dG scatter (translucent), in legend (one entry per key).
            ax.traces.append(Scatter(
                x=sub["utc"], y=sub["dg_m"].to_numpy(float),
                color=color, marker=marker, size=14, alpha=0.5,
                label=this_key, show_in_legend=True))

            # +/- threshold envelope (faint lines, not in legend).
            thr = sub["threshold_m"].to_numpy(float)
            ax.traces.append(Line(x=sub["utc"], y=thr, color=clite, width=0.5,
                                  style="-", show_in_legend=False))
            ax.traces.append(Line(x=sub["utc"], y=-thr, color=clite, width=0.5,
                                  style="-", show_in_legend=False))

            # Detected slips: heavy red X.
            slip = sub[sub["is_slip"] == 1]
            if not slip.empty:
                ax.traces.append(Scatter(
                    x=slip["utc"], y=slip["dg_m"].to_numpy(float),
                    color=(1.0, 0.0, 0.0), marker="x", size=120, alpha=1.0,
                    show_in_legend=False))
        axes.append(ax)

    return Figure(
        name="GRAPHIC dG + Threshold",
        title="GRAPHIC Between-Epoch Difference  dG = G(t) - G(t-1)  with Threshold",
        axes=axes, width=14, height=4.5)


# --------------------------------------------------------------------------- #
#  Figure 2: MAD sigma (row 1) + arc count (row 2), one column per system
# --------------------------------------------------------------------------- #
def _figure_diagnostics(df, system_list, sat_cats, colors) -> Figure:
    have_mad = "mad_sigma_m" in df.columns
    have_arc = "arc_count" in df.columns

    def _diag_axes(value_col, ylabel, title_suffix, present):
        out = []
        for sys_name in system_list:
            ax = Axes(title=f"{sys_name} - {title_suffix}",
                      xlabel="GPS Time", ylabel=ylabel)
            t_sys = df[df["system"] == sys_name]
            if t_sys.empty or not present:
                ax.traces.append(TextBox(x=0.5, y=0.5, text="No Data",
                                         ha="center", va="center"))
                out.append(ax)
                continue
            keys_in_sys = sorted(t_sys["key"].unique())
            for k, this_key in enumerate(keys_in_sys):
                sub = t_sys[t_sys["key"] == this_key]
                sat = this_key.split("_", 1)[0]
                color, marker = _key_style(sat, sat_cats, colors, k)
                ax.traces.append(Scatter(
                    x=sub["utc"], y=sub[value_col].to_numpy(float),
                    color=color, marker=marker, size=12, alpha=0.5,
                    show_in_legend=False))
            out.append(ax)
        return out

    # Reading order: all MAD-sigma tiles, then all arc-count tiles (the backend
    # stacks axes vertically, so this preserves the MATLAB row-1 / row-2 split).
    axes = (_diag_axes("mad_sigma_m", "MAD sigma  (m)", "MAD sigma", have_mad)
            + _diag_axes("arc_count", "Arc Count", "Arc Count", have_arc))

    return Figure(
        name="GRAPHIC MAD Sigma & Arc Count",
        title="GRAPHIC Detector Diagnostics",
        axes=axes, width=14, height=9.0)


# --------------------------------------------------------------------------- #
#  Figure 3: slip events with integer estimates, one tile per constellation
# --------------------------------------------------------------------------- #
def _figure_slips(df, system_list, sat_cats, colors) -> Figure:
    axes = []
    for sys_name in system_list:
        ax = Axes(title=sys_name, xlabel="GPS Time", ylabel="dN  (cycles)",
                  legend=Legend(loc="bestoutside"))
        t_sys = df[df["system"] == sys_name]
        if t_sys.empty:
            ax.traces.append(TextBox(x=0.5, y=0.5, text="No Data",
                                     ha="center", va="center"))
            axes.append(ax)
            continue

        t_slip = t_sys[t_sys["is_slip"] == 1]
        if t_slip.empty:
            ax.traces.append(TextBox(x=0.5, y=0.5, text="No Slips",
                                     ha="center", va="center"))
            axes.append(ax)
            continue

        keys_in_slip = sorted(t_slip["key"].unique())
        for k, this_key in enumerate(keys_in_slip):
            sub = t_slip[t_slip["key"] == this_key]
            sat = this_key.split("_", 1)[0]
            color, marker = _key_style(sat, sat_cats, colors, k)
            ax.traces.append(Scatter(
                x=sub["utc"], y=sub["slip_cycles"].to_numpy(float),
                color=color, marker=marker, size=60, alpha=0.7,
                label=this_key, show_in_legend=True))
        axes.append(ax)

    return Figure(
        name="GRAPHIC Slip Events",
        title=r"GRAPHIC Detected Slips - Integer Estimate  dN = round(2 dG / lambda)",
        axes=axes, width=14, height=3.5)


# --------------------------------------------------------------------------- #
#  Text summary (the console read-outs)
# --------------------------------------------------------------------------- #
def _summary(df, system_list) -> str:
    t = df["utc"]
    n_slip = int((df["is_slip"] == 1).sum())
    out = ["=== GRAPHIC Slip-Detector Trace Summary ===",
           f"Epochs (rows)   : {len(df)}",
           f"Span (GPS)      : {t.iloc[0]} -> {t.iloc[-1]}",
           f"Satellites      : {df['satellite'].nunique()}",
           f"Keys (sat_sig)  : {df['key'].nunique()}",
           f"Detected slips  : {n_slip}",
           "",
           "=== Slips per constellation ==="]
    for sys_name in system_list:
        sub = df[df["system"] == sys_name]
        out.append(f"  {sys_name:<10} : {int((sub['is_slip'] == 1).sum())} slips "
                   f"({sub['key'].nunique()} keys)")
    return "\n".join(out)


viewer = Viewer(name="graphic_trace", title="GRAPHIC slip detector",
                required_csvs=("graphic_slip_trace.csv",),
                build=build_graphic_trace)

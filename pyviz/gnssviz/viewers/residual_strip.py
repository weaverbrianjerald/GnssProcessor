"""Measurement-residual strip viewer -- the port of ``plotResidualStrip.m``.

(satellite x epoch) raster of per-satellite measurement residuals from
``measurement_residuals.csv``, styled after the satellite-status strip.  The
MATLAB script splits each constellation into *separate* Code and Phase figures
(``GPS Code``, ``GPS Phase``, ``Galileo Code``, ...), one strip-chart tile per
signal of that measurement type, so each tile stays tall enough to read
individual satellite rows.  Each tile is a (satellite x epoch) raster coloured
by the residual value on a symmetric blue->white->red diverging map; cells where
a satellite is not tracked stay transparent.

``plot_value`` selects which CSV column to render (defaulting to the MATLAB
default):
  * ``'standardized'`` -> ``standardized_residual`` (sigma units; one global
    +/-5 sigma colour limit -- the natural cross-signal default);
  * ``'residual'``     -> ``residual_m`` (raw post-fit residual, meters); code
    figures get a 10x looser colour limit than phase figures so the visual
    scale is comparable;
  * ``'innovation'``   -> ``innovation_m`` (pre-fit residual, meters; same
    code/phase scaling).

BSSD rows are logged as ``"<primary>-<pivot>"``; the primary is kept as the row
satellite (so one physical sat reads as a single row across pivot handovers) and
-- when ``show_pivot_rows`` is True (the MATLAB default) -- one synthetic
``residual=0`` row per (week, tow_s, pivot, signal) is inserted so the pivot's
row shows as the neutral mid-colour during its tenure instead of going
transparent.  Synthetic pivot rows are tagged with ``residual_std_m = NaN`` and
excluded from the per-tile mu/sigma annotation and the text summary.

Built on the shared :func:`gnssviz.strip.build_sat_epoch_strip` renderer, called
twice per content mode (once filtered to code signals, once to phase) so each
figure is single-measurement-type and its right-hand colorbar reads uniformly
across every tile.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import GPS_EPOCH, add_utc, load_trace, require_cols
from ..strip import build_sat_epoch_strip
from .base import Viewer, ViewerResult

# Canonical system render order (matches the MATLAB ``sysOrder``).
_SYS_ORDER = ("GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS")

# First-letter -> system, used both to tag rows and (in the summary) order them.
_SYS_PREFIX = {
    "G": "GPS", "E": "Galileo", "R": "GLONASS",
    "C": "BeiDou", "J": "QZSS", "S": "SBAS",
}

# (label, signal prefix, code-vs-phase colour-limit scale) -- code signals start
# with 'C', phase signals with 'L'.  ``code_scale`` matches the MATLAB
# ``codeScaleFactor`` default (10x looser limit for code in m-valued modes).
_MEAS_TYPES = (
    ("Code", "C", 10.0),
    ("Phase", "L", 1.0),
)

# MATLAB content-switch defaults.
_CAXIS_STANDARDIZED = 5.0     # +/- sigma
_CAXIS_METERS = 0.010         # +/- m (code rows get code_scale x this)


def build_residual_strip(
    ctx,
    plot_value: str = "standardized",
    show_pivot_rows: bool = True,
    show_summary: bool = True,
) -> ViewerResult:
    """Build per-(constellation, code/phase) residual strip figures.

    Defaults mirror the MATLAB content switches: ``plot_value='standardized'``,
    ``show_pivot_rows=True``, ``show_summary=True``.
    """
    df = load_trace(ctx.path("measurement_residuals.csv"))
    require_cols(
        df,
        ("week", "tow_s", "satellite", "signal",
         "residual_m", "standardized_residual"),
        "measurement_residuals.csv",
    )

    # --- Resolve which column to render + label / colour limit ------------- #
    pv = plot_value.lower()
    if pv == "standardized":
        value_col, value_label = "standardized_residual", r"Standardized residual ($\sigma$)"
        caxis_lim, per_sig_caxis = _CAXIS_STANDARDIZED, False
    elif pv == "residual":
        value_col, value_label = "residual_m", "Residual (m)"
        caxis_lim, per_sig_caxis = _CAXIS_METERS, True
    elif pv == "innovation":
        if "innovation_m" not in df.columns:
            raise ValueError("innovation_m column not present in this CSV")
        value_col, value_label = "innovation_m", "Innovation (m)"
        caxis_lim, per_sig_caxis = _CAXIS_METERS, True
    else:
        raise ValueError(
            "plot_value must be 'standardized', 'residual', or 'innovation'")

    # --- BSSD compound ID handling + pivot-row synthesis ------------------- #
    df = _split_bssd_and_synthesize_pivots(df, value_col, show_pivot_rows)

    # --- GPS week + TOW -> datetime (pure GPS scale) ----------------------- #
    df = add_utc(df)

    # --- Tag each row with a system --------------------------------------- #
    df["system"] = _tag_system(df["satellite"])

    # --- Build figures: one builder call per (content mode x meas type) ---- #
    # Splitting code (C*) and phase (L*) into separate builder calls reproduces
    # the MATLAB one-figure-per-(cons, meas-type) layout and keeps each figure
    # single-type so its colorbar limit is uniform across tiles.
    ref_ticks = [-caxis_lim, -3, 0, 3, caxis_lim] if not per_sig_caxis else None

    figures = []
    for meas_label, meas_prefix, meas_scale in _MEAS_TYPES:
        sig_mask = df["signal"].astype(str).str.startswith(meas_prefix)
        d_meas = df[sig_mask]
        if d_meas.empty:
            continue

        fig_caxis = caxis_lim * meas_scale if per_sig_caxis else caxis_lim
        long = _melt(d_meas, value_col)
        if long.empty:
            continue

        figs = build_sat_epoch_strip(
            long,
            mode="continuous",
            colormap="bwr",                 # MATLAB blue->white->red diverging ramp
            diverging=True,
            caxis=[-fig_caxis, fig_caxis],
            fig_title=f"{meas_label} residuals ({value_label})",
            value_label=value_label,
            ref_ticks=ref_ticks,
            fig_tag=f"residual-{meas_label.lower()}",
            sys_order=_SYS_ORDER,
        )
        figures.extend(figs)

    summary = ""
    if show_summary:
        summary = _summary(df, value_col, plot_value)

    return ViewerResult(figures=figures, summary=summary)


def _split_bssd_and_synthesize_pivots(
    df: pd.DataFrame, value_col: str, show_pivot_rows: bool
) -> pd.DataFrame:
    """Strip ``-<pivot>`` from BSSD sat IDs; synthesize 0-residual pivot rows.

    The residual logger writes ``"<primary>-<pivot>"`` for BSSD rows.  We keep
    the primary as the satellite (so a physical sat is one row across handovers)
    and stash the pivot.  When ``show_pivot_rows`` is set, one synthetic row per
    unique (week, tow_s, pivot, signal) at residual 0 keeps each pivot's row
    continuous and distinguishable from a not-tracked (transparent) cell.
    """
    df = df.copy()
    sat = df["satellite"].astype(str)
    has_dash = sat.str.contains("-")
    pivot = sat.where(has_dash, "").str.split("-").str[-1]
    pivot = pivot.where(has_dash, "")
    primary = sat.where(~has_dash, sat.str.split("-").str[0])
    df["satellite"] = primary
    df["pivot"] = pivot.fillna("")

    if not show_pivot_rows:
        return df

    bssd = df[df["pivot"] != ""]
    if bssd.empty:
        return df

    # First row per (week, tow_s, pivot, signal); zero out the value columns.
    grp = bssd.groupby(["week", "tow_s", "pivot", "signal"], sort=False, dropna=False)
    syn = bssd.loc[grp.head(1).index].copy()
    syn["satellite"] = syn["pivot"].to_numpy()
    for col in ("residual_m", "innovation_m", "standardized_residual"):
        if col in syn.columns:
            syn[col] = 0.0
    if "residual_std_m" in syn.columns:
        # NaN std flags synthetic rows so the stats / summary exclude them.
        syn["residual_std_m"] = np.nan
    syn["pivot"] = ""

    out = pd.concat([df, syn], ignore_index=True)
    return out.sort_values(["week", "tow_s"], kind="stable").reset_index(drop=True)


def _tag_system(satellite: pd.Series) -> pd.Series:
    """First-letter -> system name (``Unknown`` otherwise)."""
    first = satellite.astype(str).str[0]
    return first.map(_SYS_PREFIX).fillna("Unknown")


def _melt(df: pd.DataFrame, value_col: str) -> pd.DataFrame:
    """Long-form table (system, satellite, facet=signal, utc, value, annot) for
    the strip renderer; drop blank signals and non-finite values.

    ``annot`` is False for synthetic pivot rows (residual_std_m = NaN) so the
    per-tile mu/sigma/N read-out excludes them -- matching MATLAB's realMask --
    while the cells still render.
    """
    sig = df["signal"].astype(str)
    if "residual_std_m" in df.columns:
        annot = np.isfinite(df["residual_std_m"].to_numpy(dtype=float))
    else:
        annot = np.ones(len(df), dtype=bool)
    d = pd.DataFrame({
        "system": df["system"].astype(str),
        "satellite": df["satellite"].astype(str),
        "facet": sig,
        "utc": df["utc"],
        "value": df[value_col].to_numpy(dtype=float),
        "annot": annot,
    })
    d = d[(d["facet"] != "") & (d["facet"].str.lower() != "nan")]
    return d[np.isfinite(d["value"].to_numpy(float))]


def _summary(df: pd.DataFrame, value_col: str, plot_value: str) -> str:
    """Per-(system, signal) text summary mirroring the MATLAB ``fprintf`` block.

    Excludes synthetic pivot rows (``residual_std_m`` NaN).  Uses MATLAB sample
    std (ddof=1; 0.0 for a single sample) and the |sres|>3 outlier percentage.
    """
    epochs = np.sort(pd.unique(df["utc"]))
    n_epochs = epochs.size
    has_std = "residual_std_m" in df.columns

    span_lo = pd.Timestamp(epochs[0]) if n_epochs else GPS_EPOCH
    span_hi = pd.Timestamp(epochs[-1]) if n_epochs else GPS_EPOCH

    lines = []
    lines.append("=== Residual Strip Summary ===")
    lines.append(f"Epochs         : {n_epochs}")
    lines.append(f"Span (GPS)     : {span_lo} -> {span_hi}")
    lines.append(f"Mode           : {plot_value} ({value_col})")
    lines.append("")
    lines.append(f"{'System':<9} {'Signal':<6} {'N':>8} {'Mean':>10} "
                 f"{'Sigma':>10} {'|sres|>3 %':>8}")
    lines.append("-" * 60)

    sys_present = [s for s in _SYS_ORDER if (df["system"] == s).any()]
    for sys_name in sys_present:
        d_sys = df[df["system"] == sys_name]
        if d_sys.empty:
            continue
        sigs = sorted(s for s in d_sys["signal"].astype(str).unique()
                      if s and s.lower() != "nan")
        for sig in sigs:
            mask = d_sys["signal"].astype(str) == sig
            if has_std:
                mask = mask & ~d_sys["residual_std_m"].isna()
            d_sel = d_sys[mask]
            if d_sel.empty:
                continue
            vals = d_sel[value_col].to_numpy(dtype=float)
            sres = d_sel["standardized_residual"].to_numpy(dtype=float)
            vals = vals[np.isfinite(vals)]
            sres = sres[np.isfinite(sres)]
            if vals.size == 0:
                continue
            mean = float(vals.mean())
            sigma = float(vals.std(ddof=1)) if vals.size > 1 else 0.0
            pct_out = (100.0 * np.sum(np.abs(sres) > 3) / sres.size
                       if sres.size else 0.0)
            lines.append(f"{sys_name:<9} {sig:<6} {vals.size:>8d} "
                         f"{mean:>10.4f} {sigma:>10.4f} {pct_out:>7.2f}%")

    return "\n".join(lines)


viewer = Viewer(
    name="residual_strip",
    title="Measurement residual strip",
    required_csvs=("measurement_residuals.csv",),
    build=build_residual_strip,
)

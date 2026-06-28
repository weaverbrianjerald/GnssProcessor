"""Measurement-residual viewer -- the port of ``measurement_residuals.m``.

Reads ``measurement_residuals.csv`` (one row per (epoch, satellite, signal)) and
draws per-(satellite, signal) residual time series, faceted by *signal* within
each *constellation*, split into code vs phase and raw vs standardized.  The
MATLAB script defaults to ``plotLayout = 'per_signal'`` with ``plotStyle =
'lines'``, ``showStandardizedPlots = true`` and ``showLegend = false`` -- this
port reproduces that default path: up to four detail figures

  1. Code Residuals by Signal and System          (value = residual_m)
  2. Phase Residuals by Signal and System         (value = residual_m)
  3. Code Standardized Residuals by Signal/System (value = standardized_residual)
  4. Phase Standardized Residuals by Signal/System

Each figure is the ``plotResidualGrid`` MATLAB layout: an outer 1 x nCons grid
where every constellation column owns an inner vertical stack of *its own*
signals (different constellations may have different signal counts -- no blank
``GPS / C7Q`` tiles).  The Figure spec here is a single vertical stack of axes,
so the columns are flattened in constellation-major order (all GPS signals top
to bottom, then all Galileo signals, ...); the constellation name leads every
tile title so the column grouping is unambiguous, and the raster-style
linkaxes('xy') x-sharing is provided by the figure's ``link_x``.

Faithful details carried over from the MATLAB:
  * **BSSD compound-ID stripping.** The C++ logger writes ``<primary>/<pivot>``
    (or ``<primary>-<pivot>``) for between-satellite single-difference rows.
    The pivot suffix is stripped so a sat that changes pivot mid-run stays one
    series; the pivot is kept in a sidecar column.
  * **Synthetic pivot zero rows.** A pivot has no SD residual against itself, so
    one synthetic 0-residual row per (week, tow_s, pivot, signal) is inserted so
    the pivot's line sits on zero during its tenure (``residual_std_m`` -> NaN so
    those rows are excluded from the mu/sigma read-out).
  * **Gap breaks.** Within each (sat, signal) series a NaN is inserted wherever
    ``dt > gapFactor * median(dt)`` so the line breaks instead of crossing data
    gaps (``insertGapBreaks``); ``gapFactor = 3``.
  * **Colour by PRN.** Line colour follows the numeric PRN (chars 2-3 of the sat
    ID) through ``distinguishable_colors`` so a given PRN is stable across tiles.
  * **+/-3 sigma rails** drawn on the standardized figures only.
  * **Per-tile mu/sigma** annotation (top-right), computed over finite values of
    real (non-synthetic) rows -- sample std (ddof=1; 0 for a single sample).

The console summary reproduces the MATLAB ``=== Residual Statistics by System
and Type ===`` block (N, RMS, mean, sample-std of residual_m, and the count /
percent of |standardized_residual| > 3) per (system, Code|Phase).
"""

from __future__ import annotations

from functools import partial

import numpy as np
import pandas as pd

from ..io import add_utc, first_col, load_trace, require_cols
from ..figures import Axes, Figure, HLine, Line, TextBox
from ..palettes import distinguishable_colors
from .base import Viewer, ViewerResult

# A gap is declared whenever dt > gapFactor * median(dt) for that series.
_GAP_FACTOR = 3.0

# Leading constellation letter -> system name (mirrors the MATLAB startsWith map;
# the script only handles these letters).
_SYS_BY_LETTER = {
    "G": "GPS", "E": "Galileo", "R": "GLONASS",
    "C": "BeiDou", "J": "QZSS", "S": "SBAS",
}
# Constellation column / summary order.  measurement_residuals.m derives this
# from categories(T.system), which is ALPHABETICAL (BeiDou, GLONASS, GPS,
# Galileo, QZSS, SBAS).  Python sorted() reproduces that code-point order, so we
# order present systems alphabetically here (see _present_systems) rather than a
# canonical GPS-first list -- matching the MATLAB figure columns and summary.

_MEAS_TYPES = ["Code", "Phase"]
_SIGMA_RAIL = (1.0, 0.0, 0.0)   # dashed red +/-3 sigma rails


# --------------------------------------------------------------------------- #
#  Helpers
# --------------------------------------------------------------------------- #
def _matlab_std(v: np.ndarray) -> float:
    """MATLAB ``std``: sample std (ddof=1), but 0 for a single element."""
    v = np.asarray(v, float)
    v = v[np.isfinite(v)]
    if v.size == 0:
        return float("nan")
    if v.size == 1:
        return 0.0
    return float(np.std(v, ddof=1))


def _prn_color_row(sat: str) -> int:
    """1-based PRN colour index (str2double of chars 2-3); -1 if unparseable."""
    try:
        return int(str(sat)[1:3])
    except (ValueError, IndexError):
        return -1


def _insert_gap_breaks(t: pd.Series, y: np.ndarray, gap_factor: float):
    """Insert NaT/NaN where dt > gap_factor * median(dt) (mirrors insertGapBreaks).

    Returns sorted-by-time (t, y) with break rows so a line plot draws a gap
    rather than crossing it.
    """
    t = pd.to_datetime(pd.Series(t).reset_index(drop=True))
    y = np.asarray(y, float)
    if t.size < 2:
        return t, y

    order = np.argsort(t.to_numpy())
    t_sorted = t.iloc[order].reset_index(drop=True)
    y_sorted = y[order]

    dt = np.diff(t_sorted.to_numpy()).astype("timedelta64[ns]").astype(np.float64) / 1e9
    med = np.median(dt)
    if not np.isfinite(med) or med <= 0:
        med = 1.0
    gap_idx = np.flatnonzero(dt > gap_factor * med)  # 0-based: gap after this idx
    if gap_idx.size == 0:
        return t_sorted, y_sorted

    t_out, y_out = [], []
    start = 0
    for g in gap_idx:
        t_out.extend(t_sorted.iloc[start:g + 1].tolist())
        y_out.extend(y_sorted[start:g + 1].tolist())
        t_out.append(pd.NaT)            # the break row
        y_out.append(np.nan)
        start = g + 1
    t_out.extend(t_sorted.iloc[start:].tolist())
    y_out.extend(y_sorted[start:].tolist())
    return pd.Series(t_out), np.asarray(y_out, float)


def _strip_pivot(df: pd.DataFrame) -> pd.DataFrame:
    """Strip ``<primary>/<pivot>`` (or ``-``) -> primary; keep pivot sidecar.

    The CSV variant in the wild uses '-' as the SD separator; the MATLAB only
    splits on '/'.  Handle both so the primary satellite is one categorical
    value across the whole run regardless of which separator the logger used.
    """
    out = df.copy()
    sat = out["satellite"].astype(str)
    pivot = pd.Series([""] * len(out), index=out.index)
    primary = sat.copy()
    for sep in ("/", "-"):
        has = sat.str.contains(sep, regex=False)
        if has.any():
            # only split rows that still look compound and weren't split yet
            split_mask = has & (pivot == "")
            parts = sat[split_mask].str.split(sep, n=1, expand=True)
            primary.loc[split_mask] = parts[0]
            pivot.loc[split_mask] = parts[1]
    out["satellite"] = primary
    out["pivot"] = pivot
    return out


def _synthesize_pivot_rows(df: pd.DataFrame) -> pd.DataFrame:
    """Insert one synthetic 0-residual row per (week, tow_s, pivot, signal).

    Mirrors the MATLAB block: the pivot's own line should sit on zero during its
    tenure.  ``residual_std_m`` is set NaN on synthetic rows so they are excluded
    from the per-tile mu/sigma annotation.
    """
    bssd = df[df["pivot"].astype(str) != ""]
    if bssd.empty:
        return df

    # First row per group (splitapply(@(x) x(1), ...)).
    grp_keys = ["week", "tow_s", "pivot", "signal"]
    syn = bssd.groupby(grp_keys, sort=False, as_index=False).first().copy()
    syn["satellite"] = syn["pivot"]
    for col in ("residual_m", "standardized_residual", "innovation_m"):
        if col in syn.columns:
            syn[col] = 0.0
    if "residual_std_m" in syn.columns:
        syn["residual_std_m"] = np.nan       # no meaningful sigma for pivot row
    syn["pivot"] = ""
    out = pd.concat([df, syn[df.columns]], ignore_index=True)
    return out.sort_values(["week", "tow_s"], kind="stable").reset_index(drop=True)


def _classify(df: pd.DataFrame) -> pd.DataFrame:
    """Add meas_type (Code/Phase from C*/L*) and system (from sat letter)."""
    out = df.copy()
    sig = out["signal"].astype(str)
    mt = np.full(len(out), "unknown", dtype=object)
    mt[sig.str.startswith("C").to_numpy()] = "Code"
    mt[sig.str.startswith("L").to_numpy()] = "Phase"
    out["meas_type"] = mt

    letter = out["satellite"].astype(str).str[0]
    out["system"] = letter.map(_SYS_BY_LETTER).fillna("Unknown")
    return out


# --------------------------------------------------------------------------- #
#  One detail figure (the plotResidualGrid equivalent)
# --------------------------------------------------------------------------- #
def _residual_grid_figure(df: pd.DataFrame, meas_type: str, value_col: str,
                          value_label: str, fig_title: str,
                          systems_present, prn_colors, plot_style: str,
                          gap_factor: float, add_sigma_lines: bool):
    """Build one Figure: constellation columns x per-constellation signal rows.

    Returns ``None`` if there are no rows for this meas_type (MATLAB skips the
    figure entirely with a console note).
    """
    tm = df[df["meas_type"] == meas_type]
    if tm.empty:
        return None

    # Per-constellation, lexicographically-sorted signal list.
    sigs_per_cons = {}
    for sysname in systems_present:
        rows = tm[tm["system"] == sysname]
        if not rows.empty:
            s = sorted(x for x in rows["signal"].astype(str).unique() if x and x != "nan")
            if s:
                sigs_per_cons[sysname] = s
    if not sigs_per_cons:
        return None

    cons_active = [s for s in systems_present if s in sigs_per_cons]
    n_cols = len(cons_active)
    max_sigs = max(len(sigs_per_cons[c]) for c in cons_active)

    axes = []
    for sysname in cons_active:
        sigs = sigs_per_cons[sysname]
        n_sigs = len(sigs)
        for r, sig in enumerate(sigs):
            sub = tm[(tm["system"] == sysname) & (tm["signal"].astype(str) == sig)]
            ax = Axes(
                title=f"{sysname} - {sig.replace('_', '/')}",
                xlabel="GPS Time" if r == n_sigs - 1 else "",
                ylabel=value_label,
                grid=True,
            )
            ax.traces = _tile_traces(sub, value_col, prn_colors,
                                     plot_style, gap_factor, add_sigma_lines)
            axes.append(ax)

    if not axes:
        return None

    # Figure geometry tracks the MATLAB figWidth/figHeight heuristic, but the
    # Figure model is a single vertical stack, so height scales with the *total*
    # tile count across all columns rather than the per-column max.
    width = max(9.0, 2.8 * n_cols + 1.0)
    height = max(4.5, 1.7 * len(axes) + 1.2)
    return Figure(name=fig_title, title=fig_title, axes=axes,
                  width=width, height=min(height, 60.0), link_x=True)


def _tile_traces(sub: pd.DataFrame, value_col: str, prn_colors,
                 plot_style: str, gap_factor: float, add_sigma_lines: bool):
    """Per-tile traces: one line/scatter per satellite + sigma rails + mu/sigma."""
    traces = []
    sats = sorted(sub["satellite"].astype(str).unique())
    for k, sat in enumerate(sats):
        sat_rows = sub[sub["satellite"].astype(str) == sat]
        if sat_rows.empty:
            continue
        color = prn_colors.get(sat, distinguishable_colors(k + 1)[-1])
        t = sat_rows["utc"]
        y = sat_rows[value_col].to_numpy(float)
        if plot_style == "lines":
            tt, yy = _insert_gap_breaks(t, y, gap_factor)
            traces.append(Line(x=tt, y=yy, color=color, width=1.0,
                               label=sat, style="-"))
        else:
            traces.append(_scatter_or_marker(t, y, color, sat, value_col))

    if add_sigma_lines:
        traces.append(HLine(y=3.0, color=_SIGMA_RAIL, style="--"))
        traces.append(HLine(y=-3.0, color=_SIGMA_RAIL, style="--"))

    # Per-tile mu/sigma over finite values of real (non-synthetic) rows.
    if "residual_std_m" in sub.columns:
        real = sub[~sub["residual_std_m"].isna()]
    else:
        real = sub
    vals = real[value_col].to_numpy(float)
    vals = vals[np.isfinite(vals)]
    if vals.size:
        traces.append(TextBox(
            x=0.98, y=0.97,
            text=f"μ={_g(np.mean(vals))}  σ={_g(_matlab_std(vals))}",
            ha="right", va="top", boxed=True))
    return traces


def _scatter_or_marker(t, y, color, sat, value_col):
    """MATLAB scatter path: filled dots for residual_m, markers otherwise."""
    from ..figures import Scatter
    return Scatter(x=t, y=y.to_numpy(float) if hasattr(y, "to_numpy") else np.asarray(y, float),
                   color=color, size=10.0, marker="o", label=sat)


def _g(x: float) -> str:
    """MATLAB ``%.3g`` formatting."""
    return f"{x:.3g}"


# --------------------------------------------------------------------------- #
#  Build
# --------------------------------------------------------------------------- #
def build_measurement_residuals(ctx, plot_style: str = "lines",
                                show_standardized: bool = True) -> ViewerResult:
    df = load_trace(ctx.path("measurement_residuals.csv"))
    require_cols(df, ("week", "tow_s", "satellite", "signal", "residual_m"),
                 "measurement_residuals.csv")

    has_std = "standardized_residual" in df.columns

    df = _strip_pivot(df)
    df = _synthesize_pivot_rows(df)
    df = _classify(df)
    df = add_utc(df)

    # PRN-stable colours over the whole table (distinguishable_colors indexed by
    # PRN, matching colors(colorRow,:) where colorRow = PRN number).
    prn_colors = _build_prn_colors(df)

    # Systems present, alphabetical (matches MATLAB categories(T.system)); drop Unknown.
    present = sorted(s for s in df["system"].astype(str).unique() if s != "Unknown")

    figures = []
    specs = [
        ("Code", "residual_m", "Residual (m)",
         "Code Residuals by Signal and System", False),
        ("Phase", "residual_m", "Residual (m)",
         "Phase Residuals by Signal and System", False),
    ]
    if show_standardized and has_std:
        specs += [
            ("Code", "standardized_residual", "Std. Residual",
             "Code Standardized Residuals by Signal and System", True),
            ("Phase", "standardized_residual", "Std. Residual",
             "Phase Standardized Residuals by Signal and System", True),
        ]

    for meas_type, value_col, value_label, fig_title, add_sigma in specs:
        fig = _residual_grid_figure(
            df, meas_type, value_col, value_label, fig_title,
            present, prn_colors, plot_style, _GAP_FACTOR, add_sigma)
        if fig is not None:
            figures.append(fig)

    return ViewerResult(figures=figures,
                        summary=_summary(df, present, has_std))


def _build_prn_colors(df: pd.DataFrame):
    """Colour per satellite by PRN number, matching ``colors(colorRow,:)``.

    MATLAB indexes a fixed distinguishable_colors(1000) table by the PRN integer
    (1-based row).  Reproduce that exact mapping: row = PRN, with modulo wrap and
    a fallback for unparseable IDs.
    """
    table = distinguishable_colors(1000)
    out = {}
    for k, sat in enumerate(sorted(df["satellite"].astype(str).unique())):
        row = _prn_color_row(sat)
        if row < 1:
            row = (k % 1000) + 1
        if row > 1000:
            row = ((row - 1) % 1000) + 1
        out[sat] = table[row - 1]      # 1-based -> 0-based
    return out


def _summary(df: pd.DataFrame, present, has_std: bool) -> str:
    """Reproduce the ``=== Residual Statistics by System and Type ===`` block."""
    lines = ["=== Residual Statistics by System and Type ===", ""]
    for sysname in present:
        for mt in _MEAS_TYPES:
            sub = df[(df["system"] == sysname) & (df["meas_type"] == mt)]
            if sub.empty:
                continue
            res = sub["residual_m"].to_numpy(float)
            n = res.size
            res_f = res[np.isfinite(res)]
            rms = float(np.sqrt(np.mean(res_f ** 2))) if res_f.size else float("nan")
            mean = float(np.mean(res_f)) if res_f.size else float("nan")
            std = _matlab_std(res)
            if has_std:
                sres = sub["standardized_residual"].to_numpy(float)
                over = int(np.sum(np.abs(sres) > 3))
            else:
                over = 0
            pct = 100.0 * over / n if n else 0.0
            lines.append(
                f"{sysname} {mt}: N={n}, RMS={rms:.4f} m, Mean={mean:.4f} m, "
                f"Std={std:.4f} m, |Std.Res|>3: {over} ({pct:.1f}%)")
    return "\n".join(lines)


# The standardized-residual figures (figs 3 & 4) are off by default in the
# report: ``build_measurement_residuals`` keeps the MATLAB-faithful
# ``show_standardized=True`` default for direct callers, but the *registered*
# viewer renders only the raw Code/Phase residual series.  Flip the partial to
# ``True`` (or drop it) to restore the standardized panels.
viewer = Viewer(name="measurement_residuals",
                title="Measurement residuals by signal & system",
                required_csvs=("measurement_residuals.csv",),
                build=partial(build_measurement_residuals,
                              show_standardized=False))

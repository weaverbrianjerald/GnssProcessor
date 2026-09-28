"""Satellite code-bias viewer: the port of ``plotSatBiases.m``.

Reads ``sat_bias_trace.csv`` (one row per (epoch, satellite, signal)) and draws
the per-(satellite, signal) hardware code-bias estimates produced by the
PPP / PPP-RTK uncombined Kalman filter.  Expected header::

    week,tow_s,satellite,signal,system,value_m,std_m,
    elevation_deg,is_pivot,pivot_satellite

Three figure groups (each guarded by a content switch, MATLAB defaults kept):

1. ``showValue``: sat-bias value time series, per (system, signal) tile,
   one line per satellite (colour by PRN), with NaN-gap breaks, pivot rings on
   the BSSD-frozen rows, and thin vertical handover lines.  Linear y-axis.
2. ``showSigma``: sat-bias state sigma (``std_m``) on a log y-axis, same
   per-(system, signal) tiling, a convergence monitor.  No pivot rings.
3. ``showPerKeyBand``: small-multiples, one tile per (sat, signal) key (capped
   at 36 per page), the value line with its +/-1 sigma envelope, pivot rings, and
   a per-tile ``mu / sigma`` read-out.

BSSD pivot handling (mirrors ``plotIonosphere.m``).  In PPP-RTK the sat-bias
state of non-pivot sat X on signal s is ``b_X^s - b_pivot^s``; every pivot
rotation injects a synthetic per-(sat, signal) jump of ``b_old^s - b_new^s``
that does not correspond to real hardware behaviour.  ``biasReferenceMode='firstPivot'``
(default) walks each (constellation, signal) chronologically, accumulates a
per-handover offset, and re-references the whole series to the first pivot seen,
so the column reads as if a single global pivot had been used throughout and
the (originally zero) pivot row becomes the running offset
``b_pivot^s - b_globalpivot^s``.  ``biasReferenceMode='rawBssd'`` shows the raw
filter state with the handover jumps intact.  Handover epochs are marked with
thin vertical lines in either mode.  ``std_m`` is not re-propagated: the
displayed sigma is a lower bound for absolute-frame uncertainty.

Backend-agnostic: builds :class:`Figure` specs only, no plotting library.

Layout note
-----------
``plotSatBiases.m`` lays out an outer ``tiledlayout(1, nConstellations)`` with
an inner vertical stack of signal tiles per column.  :class:`Figure` stacks
axes vertically only, so the faithful mapping is one Figure per constellation
with its signal tiles stacked top to bottom (the inner column).  The figure title
carries the constellation name; the shared figure super-title carries the
MATLAB ``title(outerTl, ...)`` string.  ``plotStyle='lines'`` and
``plotLayout='per_signal'`` are the MATLAB defaults and the only mode wired here
(the ``compact`` line-style layout is a secondary MATLAB switch).
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import add_utc, load_trace, require_cols
from ..figures import (Axes, Band, Figure, Legend, Line, Scatter, TextBox,
                       VLine)
from ..identifiers import system_of
from ..palettes import distinguishable_colors
from .base import Viewer, ViewerResult

# ----- MATLAB style switches (defaults preserved) ----------------------------
_PLOT_STYLE = "lines"           # 'lines' | 'scatter'
_GAP_FACTOR = 3.0               # dt > gapFactor*median(dt) declares a gap
_SHOW_VALUE = True
_SHOW_SIGMA = True
_SHOW_PER_KEY_BAND = True
_SHOW_SUMMARY = True
_SHOW_PIVOT_MARKERS = True
_BIAS_REFERENCE_MODE = "firstPivot"   # 'firstPivot' | 'rawBssd'
_SHOW_HANDOVER_LINES = True
_MAX_PER_FIG = 36               # per-key band small-multiples cap

# Handover vertical line + pivot ring styling (match the MATLAB greys).
_HANDOVER_RGB = (0.3, 0.3, 0.3)
_PIVOT_RGB = (0.0, 0.0, 0.0)
_BAND_ALPHA = 0.4

# Marker cycle for plotStyle='scatter' (mirrors markerList in the MATLAB).
_MARKER_LIST = ["o", "s", "d", "^", "v", ">", "<", "p", "h", "x", "+"]

# distinguishable_colors(max(8, 1e3)) in the MATLAB; sized on demand here, but the
# PRN-keyed index (chars 2-3) is what selects the row, so 1..99 covers every PRN.
_N_COLORS = 1000
_COLORS = distinguishable_colors(_N_COLORS)


def _color_row(sat: str) -> int:
    """PRN-keyed colour row (0-based), matching ``localColorRow`` in the MATLAB.

    PRN = chars 2-3 of the satellite ID (1-based MATLAB row); wraps modulo the
    palette length.  Falls back to row 1 (->0 here) for unparseable IDs.
    """
    row = np.nan
    s = str(sat)
    if len(s) >= 3:
        try:
            row = float(int(s[1:3]))
        except ValueError:
            row = np.nan
    if not np.isfinite(row):
        row = 1.0
    row = int(row)
    if row > _N_COLORS:
        row = (row - 1) % _N_COLORS + 1
    if row < 1:
        row = 1
    return row - 1                  # MATLAB 1-based -> Python 0-based


def _color_for(sat: str):
    return _COLORS[_color_row(sat)]


def _system_for(satellite: str, system) -> str:
    """Constellation name, deriving from the satellite letter for older traces."""
    s = str(system)
    if s and s not in ("nan", "Unknown", ""):
        return s
    return system_of(satellite)


def _insert_gap_breaks(t, y, gap_factor=_GAP_FACTOR):
    """Insert NaT/NaN rows where the (sorted) series has a time gap.

    Cloned from ``insertGapBreaks`` in the MATLAB: a gap is declared whenever
    ``dt > gap_factor * median(dt)``; a NaN sample is inserted after each gap so
    the line/band breaks rather than bridging.  ``t`` is datetime-like, ``y`` is
    numeric.  Returns ``(t_out, y_out)`` numpy object/float arrays.
    """
    t = pd.to_datetime(np.asarray(t))
    y = np.asarray(y, dtype=float)
    if t.size < 2:
        return t, y
    order = np.argsort(t.values)
    t_sorted = t.values[order]
    y_sorted = y[order]
    dt_sec = np.diff(t_sorted) / np.timedelta64(1, "s")
    med = np.median(dt_sec)
    if not np.isfinite(med) or med <= 0:
        med = 1.0
    gap_idx = np.flatnonzero(dt_sec > gap_factor * med)  # gap after this index
    if gap_idx.size == 0:
        return pd.to_datetime(t_sorted), y_sorted

    t_out = []
    y_out = []
    r_start = 0
    for g in gap_idx:
        t_out.extend(t_sorted[r_start:g + 1])
        y_out.extend(y_sorted[r_start:g + 1])
        t_out.append(np.datetime64("NaT"))
        y_out.append(np.nan)
        r_start = g + 1
    t_out.extend(t_sorted[r_start:])
    y_out.extend(y_sorted[r_start:])
    return pd.to_datetime(np.array(t_out, dtype="datetime64[ns]")), np.asarray(y_out, float)


def _collect_pivot_handovers(df: pd.DataFrame):
    """system -> sorted list of handover epochs (``pivot_satellite`` changes).

    A handover is declared when ``pivot_satellite`` differs between two
    successive unique epochs (both non-empty).  Signal-agnostic -- a
    per-constellation property.  Empty dict for traces lacking pivot info.
    """
    out = {}
    if "pivot_satellite" not in df.columns:
        return out
    piv = df["pivot_satellite"].astype("string").fillna("")
    if (piv == "").all():
        return out
    for sysName in sorted(df["system_resolved"].dropna().unique()):
        if sysName == "Unknown":
            continue
        sub = df[df["system_resolved"] == sysName].sort_values("utc")
        if sub.empty:
            continue
        # First-row pivot per unique epoch (stable order preserved by sort).
        per_epoch = sub.groupby("utc", sort=True)["pivot_satellite"].first()
        per_epoch = per_epoch.astype("string").fillna("")
        epochs = per_epoch.index.to_numpy()
        vals = per_epoch.to_numpy()
        if epochs.size < 2:
            continue
        change = []
        for e in range(1, epochs.size):
            if vals[e] != vals[e - 1] and vals[e] != "" and vals[e - 1] != "":
                change.append(epochs[e])
        if change:
            out[sysName] = [pd.Timestamp(c) for c in change]
    return out


def _re_reference_first_pivot(df: pd.DataFrame) -> pd.DataFrame:
    """Re-reference BSSD sat-bias to the first pivot per (constellation, signal).

    Port of ``reReferenceBiasToFirstPivot``.  Walks each (system, signal) in
    chronological order accumulating a running offset ``O``; at each handover
    ``P_old -> P_new`` it adds the new pivot's bias at the previous epoch
    (= ``b_Pnew - b_Pold`` for this signal) to ``O``, then adds ``O`` to every
    row at this epoch.  The pivot row (raw value 0) becomes exactly ``O``.

    Returns a copy with ``value_m`` overwritten.  No-op if pivot info is absent.
    """
    if "pivot_satellite" not in df.columns:
        return df
    piv = df["pivot_satellite"].astype("string").fillna("")
    if (piv == "").all():
        return df

    out = df.copy()
    value_out = out["value_m"].to_numpy(float).copy()
    sat = out["satellite"].astype(str).to_numpy()
    sig = out["signal"].astype(str).to_numpy()
    pivc = piv.to_numpy()

    for sysName in sorted(out["system_resolved"].dropna().unique()):
        if sysName == "Unknown":
            continue
        sys_mask = (out["system_resolved"] == sysName).to_numpy()
        sigs_here = sorted(set(sig[sys_mask]) - {"", "nan"})
        for sigName in sigs_here:
            sub_mask = sys_mask & (sig == sigName)
            sub_idx = np.flatnonzero(sub_mask)
            if sub_idx.size == 0:
                continue
            # Chronological order within this (system, signal).
            order = np.argsort(out["utc"].to_numpy()[sub_idx], kind="stable")
            sub_idx = sub_idx[order]
            utc_sub = out["utc"].to_numpy()[sub_idx]
            piv_sub = pivc[sub_idx]
            sat_sub = sat[sub_idx]
            val_sub = out["value_m"].to_numpy(float)[sub_idx]

            uEpochs, inv = np.unique(utc_sub, return_inverse=True)
            nE = uEpochs.size
            # First-row pivot per unique epoch.
            piv_per_epoch = np.empty(nE, dtype=object)
            for e in range(nE):
                first = np.flatnonzero(inv == e)[0]
                piv_per_epoch[e] = piv_sub[first]
            if all(p == "" for p in piv_per_epoch):
                continue

            O = 0.0
            for e in range(nE):
                if (e > 0 and piv_per_epoch[e] != piv_per_epoch[e - 1]
                        and piv_per_epoch[e] != "" and piv_per_epoch[e - 1] != ""):
                    prev_mask = (inv == (e - 1)) & (sat_sub == piv_per_epoch[e])
                    prev_rows = np.flatnonzero(prev_mask)
                    if prev_rows.size:
                        O = O + float(val_sub[prev_rows[0]])
                epoch_rows = sub_idx[inv == e]
                value_out[epoch_rows] = df["value_m"].to_numpy(float)[epoch_rows] + O

    out["value_m"] = value_out
    return out


def _std_omitnan(v):
    """MATLAB ``std(v,'omitnan')``: sample std (ddof=1); 0.0 for a single value."""
    w = np.asarray(v, float)
    w = w[np.isfinite(w)]
    if w.size == 0:
        return float("nan")
    if w.size == 1:
        return 0.0
    return float(np.std(w, ddof=1))


def _fmt_g3(x: float) -> str:
    """Mimic MATLAB ``%.3g`` for the tile read-outs."""
    if not np.isfinite(x):
        return "NaN"
    return f"{x:.3g}"


def _handover_traces(handovers, sysName):
    """Thin vertical lines at each handover epoch for one constellation."""
    if not _SHOW_HANDOVER_LINES or sysName not in handovers:
        return []
    return [VLine(x=ev, color=_HANDOVER_RGB, style="-", width=0.5)
            for ev in handovers[sysName]]


def _series_traces(sub_sat, value_col, draw_pivot, marker_idx):
    """Per-satellite line (or scatter) + optional pivot rings for one tile."""
    sub_sat = sub_sat.sort_values("utc")
    sat = str(sub_sat["satellite"].iloc[0])
    c = _color_for(sat)
    t = sub_sat["utc"]
    y = sub_sat[value_col].to_numpy(float)
    traces = []
    if _PLOT_STYLE == "lines":
        tt, yy = _insert_gap_breaks(t, y)
        traces.append(Line(x=tt, y=yy, color=c, width=0.9, label=sat))
    else:
        traces.append(Scatter(x=t, y=y, color=c, size=14, marker=_MARKER_LIST[marker_idx],
                              label=sat))
    if (draw_pivot and value_col == "value_m" and "is_pivot" in sub_sat.columns
            and (sub_sat["is_pivot"].to_numpy(float) > 0).any()):
        pm = sub_sat["is_pivot"].to_numpy(float) > 0
        traces.append(Scatter(x=t[pm], y=y[pm], color=_PIVOT_RGB, size=22,
                              marker="o", show_in_legend=False))
    return traces


def _tile_stats_text(values):
    """Top-right ``mu / sigma / N`` read-out over finite samples (MATLAB text)."""
    v = np.asarray(values, float)
    v = v[np.isfinite(v)]
    if v.size == 0:
        return None
    txt = (f"μ={_fmt_g3(float(np.mean(v)))}  "
           f"σ={_fmt_g3(_std_omitnan(v))}  N={v.size}")
    return TextBox(x=0.98, y=0.97, text=txt, ha="right", va="top", boxed=True)


def _grid_figures(df, value_col, value_label, fig_title, systems_present,
                  handovers, draw_pivot, yscale):
    """One Figure per constellation; signal tiles stacked top->bottom.

    Mirrors ``plotBiasGrid`` (per_signal layout): each tile is one
    (system, signal), satellites distinguished by colour, NaN-gap-aware lines,
    pivot rings, handover verticals, per-tile read-out, log/linear y as given.
    """
    figures = []
    for sysName in systems_present:
        sys_df = df[df["system_resolved"] == sysName]
        sigs = sorted(s for s in sys_df["signal"].astype(str).unique()
                      if s not in ("", "nan"))
        if not sigs:
            continue
        axes = []
        for r, sig in enumerate(sigs):
            tile = sys_df[sys_df["signal"].astype(str) == sig]
            ax = Axes(title=f"{sysName} - {sig}", yscale=yscale,
                      ylabel=value_label if True else "",
                      xlabel="GPS Time" if r == len(sigs) - 1 else "")
            traces = []
            for mk, (_, sub_sat) in enumerate(tile.groupby("satellite", sort=True)):
                if sub_sat.empty:
                    continue
                traces.extend(_series_traces(sub_sat, value_col, draw_pivot,
                                             mk % len(_MARKER_LIST)))
            traces.extend(_handover_traces(handovers, sysName))
            stat = _tile_stats_text(tile[value_col].to_numpy(float))
            if stat is not None:
                traces.append(stat)
            ax.traces = traces
            axes.append(ax)
        if axes:
            figures.append(Figure(name=f"{fig_title} - {sysName}", title=fig_title,
                                  subtitle=sysName, axes=axes, width=11,
                                  height=max(3.0, 1.8 * len(axes))))
    return figures


def _per_key_band_figures(df, handovers):
    """Small-multiples: one tile per (sat, signal), value + 1-sigma band.

    Port of Figure 3.  Keys are ``unique(sat + '|' + signal)``; capped at
    ``_MAX_PER_FIG`` per page (paged into multiple Figures rather than dropping,
    so nothing is silently lost; the MATLAB only warns and shows the first 36).
    """
    keys = sorted(set(df["satellite"].astype(str) + "|" + df["signal"].astype(str)))
    figures = []
    for page_start in range(0, len(keys), _MAX_PER_FIG):
        page_keys = keys[page_start:page_start + _MAX_PER_FIG]
        axes = []
        for key in page_keys:
            sat, sig = key.split("|", 1)
            sub = df[(df["satellite"].astype(str) == sat)
                     & (df["signal"].astype(str) == sig)].sort_values("utc")
            ax = Axes(title=key, ylabel="bias (m)", xlabel="GPS Time")
            if sub.empty:
                ax.traces = [TextBox(x=0.5, y=0.5, text="No Data",
                                     ha="center", va="center")]
                axes.append(ax)
                continue
            t = sub["utc"]
            vals = sub["value_m"].to_numpy(float)
            err = sub["std_m"].to_numpy(float)
            c = _color_for(sat)
            band_rgb = tuple(1 - 0.4 * (1 - np.asarray(c)))  # MATLAB 1-0.4*(1-c)

            tt_lo, lo = _insert_gap_breaks(t, vals - err)
            _, hi = _insert_gap_breaks(t, vals + err)
            traces = [Band(x=tt_lo, lo=lo, hi=hi, color=band_rgb, alpha=_BAND_ALPHA)]
            tt2, vv2 = _insert_gap_breaks(t, vals)
            traces.append(Line(x=tt2, y=vv2, color=c, width=0.9))

            if (_SHOW_PIVOT_MARKERS and "is_pivot" in sub.columns
                    and (sub["is_pivot"].to_numpy(float) > 0).any()):
                pm = sub["is_pivot"].to_numpy(float) > 0
                traces.append(Scatter(x=t[pm], y=vals[pm], color=_PIVOT_RGB,
                                      size=18, marker="o", show_in_legend=False))

            finite = vals[np.isfinite(vals)]
            if finite.size:
                txt = (f"μ={_fmt_g3(float(np.mean(finite)))}  "
                       f"σ={_fmt_g3(_std_omitnan(finite))}")
                traces.append(TextBox(x=0.98, y=0.95, text=txt, ha="right",
                                      va="top", boxed=True))
            ax.traces = traces
            axes.append(ax)
        if axes:
            page_no = page_start // _MAX_PER_FIG + 1
            name = "Sat-bias +/-1 sigma (per key)"
            if len(keys) > _MAX_PER_FIG:
                name += f" [page {page_no}]"
            figures.append(Figure(
                name=name,
                title=r"Per-(sat,signal) hardware bias with $\pm1\sigma$ envelope",
                axes=axes, width=14, height=max(4.0, 1.6 * len(axes))))
    return figures


def build_sat_biases(ctx) -> ViewerResult:
    df = load_trace(ctx.path("sat_bias_trace.csv"))
    require_cols(df, ("week", "tow_s", "satellite", "signal", "value_m", "std_m"),
                 "sat_bias_trace.csv")
    df = add_utc(df)

    # Derive / fill optional columns the same way the MATLAB does.
    df["satellite"] = df["satellite"].astype(str)
    df["signal"] = df["signal"].astype(str)
    sys_col = df["system"] if "system" in df.columns else None
    df["system_resolved"] = [
        _system_for(s, sys_col.iloc[i] if sys_col is not None else None)
        for i, s in enumerate(df["satellite"])
    ]
    if "elevation_deg" not in df.columns:
        df["elevation_deg"] = np.nan
    if "is_pivot" not in df.columns:
        df["is_pivot"] = 0.0
    if "pivot_satellite" not in df.columns:
        df["pivot_satellite"] = ""

    handovers = _collect_pivot_handovers(df)
    n_handovers = sum(len(v) for v in handovers.values())

    log_lines = []
    if _BIAS_REFERENCE_MODE.lower() == "firstpivot" and n_handovers > 0:
        log_lines.append(
            f"Re-referencing {n_handovers} pivot handover(s) across "
            f"{len(handovers)} constellation(s) to the first pivot per "
            f"(constellation, signal) (mode='firstPivot').")
        df = _re_reference_first_pivot(df)
    elif _BIAS_REFERENCE_MODE.lower() == "rawbssd" and n_handovers > 0:
        log_lines.append(
            f"Showing raw BSSD sat-bias states (mode='rawBssd'); "
            f"{n_handovers} handover jump(s) will be visible.")

    # Active systems (drop Unknown), in canonical-ish appearance order.
    systems_present = [s for s in
                       sorted(df["system_resolved"].dropna().unique())
                       if s != "Unknown" and (df["system_resolved"] == s).any()]
    if not systems_present:
        raise ValueError("No recognized GNSS systems found in sat_bias_trace")

    figures = []
    if _SHOW_VALUE:
        figures += _grid_figures(
            df, "value_m", "Sat bias (m)",
            "Per-(satellite,signal) hardware bias by system and signal",
            systems_present, handovers, draw_pivot=_SHOW_PIVOT_MARKERS,
            yscale="linear")
    if _SHOW_SIGMA:
        figures += _grid_figures(
            df, "std_m", "sigma (m)",
            "Sat-bias state sigma (m) - convergence monitor",
            systems_present, handovers, draw_pivot=False, yscale="log")
    if _SHOW_PER_KEY_BAND:
        figures += _per_key_band_figures(df, handovers)

    summary = _summary(df, handovers, ctx.path("sat_bias_trace.csv"), log_lines) \
        if _SHOW_SUMMARY else ""
    return ViewerResult(figures=figures, summary=summary)


def _summary(df, handovers, fname, log_lines) -> str:
    out = list(log_lines)
    if out:
        out.append("")
    out.append("=== Satellite-Bias Trace Summary ===")
    out.append(f"File           : {fname}")
    out.append(f"Epochs         : {df['utc'].nunique()} unique")
    keys = sorted(set(df["satellite"].astype(str) + "|" + df["signal"].astype(str)))
    out.append(f"Sat-signal keys: {len(keys)}")
    out.append(f"Span (UTC)     : {df['utc'].min()} -> {df['utc'].max()}")
    out.append("")
    out.append(f"{'Satellite':<9} {'Signal':<7} {'System':<9} {'N':>6} "
               f"{'Mean(m)':>10} {'Std(m)':>10} {'MeanSigma':>10}")
    out.append("-" * 70)
    for key in keys:
        sat, sig = key.split("|", 1)
        sub = df[(df["satellite"].astype(str) == sat)
                 & (df["signal"].astype(str) == sig)]
        if sub.empty:
            continue
        vals = sub["value_m"].to_numpy(float)
        sigs = sub["std_m"].to_numpy(float)
        sysName = str(sub["system_resolved"].iloc[0])
        mean_v = float(np.nanmean(vals)) if np.isfinite(vals).any() else float("nan")
        mean_s = float(np.nanmean(sigs)) if np.isfinite(sigs).any() else float("nan")
        out.append(f"{sat:<9} {sig:<7} {sysName:<9} {len(sub):>6} "
                   f"{mean_v:>10.4f} {_std_omitnan(vals):>10.4f} {mean_s:>10.4f}")

    if (df["is_pivot"].to_numpy(float) > 0).any():
        out.append("")
        out.append("=== BSSD Pivot Tenure (sat-bias rows) ===")
        piv_df = df[df["is_pivot"].to_numpy(float) > 0]
        for sat in sorted(piv_df["satellite"].astype(str).unique()):
            tp = piv_df[piv_df["satellite"].astype(str) == sat]["utc"]
            out.append(f"  {sat} pivot for {len(tp)} epoch-rows : "
                       f"{tp.min()} -> {tp.max()}")
    return "\n".join(out)


viewer = Viewer(name="sat_biases", title="Satellite code biases",
                required_csvs=("sat_bias_trace.csv",), build=build_sat_biases)

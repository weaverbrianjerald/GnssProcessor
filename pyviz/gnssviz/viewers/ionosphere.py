"""Ionosphere / slant-iono viewer -- the port of ``plotIonosphere.m``.

Reads ``ionosphere_trace.csv`` (the PPP-RTK uncombined Kalman filter's per-
satellite slant ionosphere delay state) and produces the same five figures plus
console summary the MATLAB script draws:

  1. Slant ionosphere delay (m) -- one tile (axes) per constellation, one
     NaN-gap-aware line per satellite coloured by PRN, hollow-black pivot rings on
     the value line, and thin grey vertical lines at every BSSD pivot handover.
  2. VTEC (vertical iono = slant / mapping factor) -- same per-system layout.
  3. State sigma (m) on a *log* y-axis -- the convergence monitor (no pivot rings).
  4. Per-satellite small multiples: slant +/-1 sigma envelope (gap-aware
     :class:`Band`), the mean line, pivot rings, and a top-right mu/sigma stat box;
     capped at the first 36 satellites like the MATLAB ``maxPerFig``.
  5. Sky plot per constellation: a :class:`PolarScatter` of (azimuth, zenith
     angle) coloured by slant iono through one shared colour scale (2nd/98th
     percentile robust limits), compass-north zero / clockwise, with elevation
     ring labels.

BSSD pivot re-referencing
-------------------------
In PPP-RTK BSSD the slant-iono state for a non-pivot satellite X is
``I_X - I_pivot``; every pivot rotation injects a synthetic per-satellite jump of
``I_old_pivot - I_new_pivot`` that is not real ionosphere.  Matching the MATLAB
default (``ionoReferenceMode = 'firstPivot'``), this viewer re-references the time
series to the first pivot seen per constellation so handovers read as continuous
lines; VTEC is recomputed from the re-referenced slant through the mapping factor.
Handover epochs are still marked with vertical lines on figures 1-3.  Set the
``iono_reference_mode`` kwarg of :func:`build_ionosphere` to ``'rawBssd'`` to view
the raw filter state with the handover jumps intact.

Fidelity notes
--------------
* Gap breaks: a NaN row is inserted whenever ``dt > gapFactor * median(dt)`` for a
  series (``gapFactor = 3``), exactly as ``insertGapBreaks`` does, so handover /
  reseed holes render as breaks rather than bridges.
* ``std`` in the stat read-outs is the MATLAB *sample* std (``ddof=1``), and 0.0
  for a single sample (MATLAB ``std`` of one element is 0, not NaN).
* Mapping factor and VTEC are derived from the trace columns when present (this
  trace carries them); the thin-shell fallback mirrors
  ``thinShellMappingFactor`` (Re = 6371.23 km, H = 450 km) for older traces.
* Trace times are pure GPS time; :func:`add_utc` performs the conversion.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import add_utc, first_col, load_trace, require_cols
from ..figures import (Axes, Band, ColorBar, Figure, Legend, Line, PolarScatter,
                       Scatter, TextBox, VLine)
from ..palettes import color_by_prn
from .base import Viewer, ViewerResult

# Canonical constellation render order (the "Unknown" system is dropped, as in
# the MATLAB ``systemList = systemList(~strcmp(systemList,'Unknown'))``).
_SYS_ORDER = ("GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS")

_GAP_FACTOR = 3.0           # MATLAB gapFactor
_MAX_PER_FIG = 36           # MATLAB maxPerFig in the per-sat band figure
_HANDOVER_COLOR = (0.3, 0.3, 0.3)
_PIVOT_RING = (0.0, 0.0, 0.0)

# Thin-shell mapping-factor constants (match GlobalIonosphereMap.cpp).
_RE = 6371230.0
_H = 450.0e3


# --------------------------------------------------------------------------- #
#  Build
# --------------------------------------------------------------------------- #
def build_ionosphere(ctx, *, plot_style: str = "lines",
                     iono_reference_mode: str = "firstPivot",
                     show_pivot_markers: bool = True,
                     show_handover_lines: bool = True) -> ViewerResult:
    """Build the ionosphere figures.

    Parameters
    ----------
    plot_style:
        ``'lines'`` (MATLAB default) -- one gap-aware line per satellite; or
        ``'scatter'`` -- markers only (useful when sample spacing varies wildly).
    iono_reference_mode:
        ``'firstPivot'`` (MATLAB default) -- re-reference each constellation to
        its first-seen pivot so handover jumps are removed; or ``'rawBssd'`` --
        show the raw BSSD filter state with the jumps intact.
    show_pivot_markers:
        Draw a hollow-black ring on every epoch a satellite served as BSSD pivot
        (figures 1 and 4 only -- where the ring sits on the slant value line).
    show_handover_lines:
        Draw a thin grey vertical line at each pivot handover on figures 1-3.
    """
    df = load_trace(ctx.path("ionosphere_trace.csv"))
    require_cols(df, ("week", "tow_s", "satellite", "iono_m", "std_m",
                      "elevation_deg"), "ionosphere_trace.csv")

    df = df.copy()
    df["satellite"] = df["satellite"].astype(str)

    # --- Derive system / mapping factor / vtec for older short-schema traces ---
    if "system" not in df.columns:
        df["system"] = df["satellite"].map(_system_of_letter)
    else:
        df["system"] = df["system"].astype(str)

    el = df["elevation_deg"].to_numpy(float)
    if "mapping_factor" not in df.columns:
        df["mapping_factor"] = _thin_shell_mapping_factor(el)
    if "vtec_m" not in df.columns:
        df["vtec_m"] = df["iono_m"].to_numpy(float) / df["mapping_factor"].to_numpy(float)
    if "azimuth_deg" not in df.columns:
        df["azimuth_deg"] = np.nan
    if "is_pivot" not in df.columns:
        df["is_pivot"] = 0.0
    if "pivot_satellite" not in df.columns:
        df["pivot_satellite"] = ""
    df["pivot_satellite"] = df["pivot_satellite"].fillna("").astype(str)

    # --- GPS week/tow -> datetime (pure GPS time, no leap offset) ---
    df = add_utc(df)

    # --- Collect handovers BEFORE re-referencing, then apply re-reference ---
    handovers = _collect_pivot_handovers(df)
    n_handovers = sum(len(v) for v in handovers.values())
    if iono_reference_mode.lower() == "firstpivot" and n_handovers > 0:
        df = _re_reference_to_first_pivot(df)

    # Satellite -> colour by PRN (matches the per-PRN colouring convention).
    sat_color = color_by_prn(df["satellite"].tolist())

    sys_present = [s for s in _SYS_ORDER if (df["system"] == s).any()]
    if not sys_present:
        raise ValueError("No recognized GNSS systems found in ionosphere CSV")

    figures: list = []

    # Figure 1: slant iono delay per system.
    figures.append(_plot_by_system(
        df, "iono_m", "Slant iono delay (m)",
        "Slant ionosphere delay by system", sys_present, sat_color,
        plot_style, show_pivot_markers=show_pivot_markers, yscale="linear",
        handovers=handovers, show_handover_lines=show_handover_lines,
        name="Slant ionosphere delay"))

    # Figure 2: VTEC per system.
    figures.append(_plot_by_system(
        df, "vtec_m", "Vertical iono delay (m, L1)",
        "Vertical ionosphere delay (slant / mapping factor) by system",
        sys_present, sat_color, plot_style, show_pivot_markers=False,
        yscale="linear", handovers=handovers,
        show_handover_lines=show_handover_lines,
        name="Vertical ionosphere delay"))

    # Figure 3: state sigma per system (log y, convergence monitor).
    figures.append(_plot_by_system(
        df, "std_m", "sigma_iono (m)",
        "Ionosphere state sigma (m) - convergence monitor",
        sys_present, sat_color, plot_style, show_pivot_markers=False,
        yscale="log", handovers=handovers,
        show_handover_lines=show_handover_lines,
        name="Ionosphere state sigma"))

    # Figure 4: per-satellite slant +/- 1 sigma small multiples.
    fig4 = _plot_per_sat_band(df, sat_color, show_pivot_markers)
    if fig4 is not None:
        figures.append(fig4)

    # Figure 5: sky plot per constellation coloured by slant iono.
    fig5 = _plot_sky(df, sys_present)
    if fig5 is not None:
        figures.append(fig5)

    return ViewerResult(figures=figures,
                        summary=_summary(df, sys_present, handovers,
                                         iono_reference_mode, n_handovers,
                                         ctx.path("ionosphere_trace.csv")))


# --------------------------------------------------------------------------- #
#  Figure 1-3: per-system tiled lines
# --------------------------------------------------------------------------- #
def _plot_by_system(df, value_col, value_label, fig_title, sys_present,
                    sat_color, plot_style, *, show_pivot_markers, yscale,
                    handovers, show_handover_lines, name) -> Figure:
    axes_list: list = []
    n_sys = len(sys_present)
    for col, sys_name in enumerate(sys_present):
        d_sys = df[df["system"] == sys_name]
        ax = Axes(title=sys_name, xlabel="GPS Time",
                  ylabel=(value_label if col == 0 else ""),
                  yscale=yscale, grid=True)
        traces: list = []

        if d_sys.empty:
            ax.traces = [TextBox(x=0.5, y=0.5, text="No Data",
                                 ha="center", va="center")]
            axes_list.append(ax)
            continue

        for sat in _prn_order(d_sys["satellite"].unique()):
            sub = d_sys[d_sys["satellite"] == sat].sort_values("utc")
            t = sub["utc"].to_numpy("datetime64[ns]")
            v = sub[value_col].to_numpy(float)
            if t.size == 0:
                continue
            c = sat_color[sat]
            if plot_style.lower() == "lines":
                tt, yy = _insert_gap_breaks(t, v, _GAP_FACTOR)
                traces.append(Line(x=tt, y=yy, color=c, width=0.9, label=sat,
                                   show_in_legend=False))
            else:
                traces.append(Scatter(x=t, y=v, size=18, marker="o", color=c,
                                      label=sat, show_in_legend=False))

            # Pivot ring overlay -- only meaningful on the slant value line.
            if show_pivot_markers and value_col == "iono_m":
                piv = sub["is_pivot"].to_numpy(float) > 0
                if piv.any():
                    traces.append(Scatter(
                        x=sub["utc"].to_numpy("datetime64[ns]")[piv], y=v[piv],
                        size=22, marker="o", color=_PIVOT_RING, alpha=1.0,
                        show_in_legend=False))

        # Pivot-handover vertical lines for this constellation.
        if show_handover_lines and sys_name in handovers:
            for ev in handovers[sys_name]:
                traces.append(VLine(x=pd.Timestamp(ev), color=_HANDOVER_COLOR,
                                    style="-", width=0.5))

        # Per-tile mu/sigma annotation, top-right.
        v_all = d_sys[value_col].to_numpy(float)
        v_all = v_all[np.isfinite(v_all)]
        if v_all.size:
            traces.append(TextBox(
                x=0.98, y=0.97,
                text=f"μ={v_all.mean():.3g}  σ={_std(v_all):.3g}  "
                     f"N={v_all.size}",
                ha="right", va="top", boxed=True))

        ax.traces = traces
        axes_list.append(ax)

    # Per-system tiles are side-by-side in MATLAB (1 x nSys); the stacked-axes
    # backend lays them vertically, but x-links them, preserving the pan/zoom
    # sync of the MATLAB ``linkaxes(...,'x')``.
    return Figure(name=name, title=fig_title, axes=axes_list,
                  width=max(11.0, 5.0 * n_sys),
                  height=min(12.0, 3.2 * n_sys), link_x=True)


# --------------------------------------------------------------------------- #
#  Figure 4: per-satellite slant +/- 1 sigma small multiples
# --------------------------------------------------------------------------- #
def _plot_per_sat_band(df, sat_color, show_pivot_markers):
    sats = _prn_order(df["satellite"].unique())
    n_sats = len(sats)
    if n_sats == 0:
        return None
    n_plot = min(n_sats, _MAX_PER_FIG)

    axes_list: list = []
    for sat in sats[:n_plot]:
        sub = df[df["satellite"] == sat].sort_values("utc")
        ax = Axes(title=sat, xlabel="GPS Time", ylabel="Iono (m)", grid=True)
        if sub.empty:
            ax.traces = [TextBox(x=0.5, y=0.5, text="No Data",
                                 ha="center", va="center")]
            axes_list.append(ax)
            continue

        t = sub["utc"].to_numpy("datetime64[ns]")
        v = sub["iono_m"].to_numpy(float)
        e = sub["std_m"].to_numpy(float)
        c = sat_color[sat]
        # Desaturated band colour (MATLAB ``1 - 0.4*(1 - c)``).
        band_rgb = tuple(1.0 - 0.4 * (1.0 - np.asarray(c)))

        # Gap-aware +/-1 sigma band; NaN in lo/hi breaks the fill into segments.
        tt, lo = _insert_gap_breaks(t, v - e, _GAP_FACTOR)
        _, hi = _insert_gap_breaks(t, v + e, _GAP_FACTOR)
        traces: list = [Band(x=tt, lo=lo, hi=hi, color=band_rgb, alpha=0.4)]

        # Mean line, gap-aware.
        tt2, vv2 = _insert_gap_breaks(t, v, _GAP_FACTOR)
        traces.append(Line(x=tt2, y=vv2, color=c, width=0.9,
                           show_in_legend=False))

        # Pivot ring overlay.
        if show_pivot_markers:
            piv = sub["is_pivot"].to_numpy(float) > 0
            if piv.any():
                traces.append(Scatter(x=t[piv], y=v[piv], size=20, marker="o",
                                      color=_PIVOT_RING, show_in_legend=False))

        # Compact stats annotation, top-right.
        finite = v[np.isfinite(v)]
        if finite.size:
            traces.append(TextBox(
                x=0.98, y=0.95,
                text=f"μ={finite.mean():.3g}  σ={_std(finite):.3g}",
                ha="right", va="top", boxed=True))

        ax.traces = traces
        axes_list.append(ax)

    return Figure(name="Iono per Satellite (+/- 1 sigma)",
                  title="Per-satellite slant iono delay with +/-1 sigma envelope",
                  axes=axes_list, width=14.0,
                  height=min(13.0, 1.7 * len(axes_list) + 1.0), link_x=False)


# --------------------------------------------------------------------------- #
#  Figure 5: sky plot per system coloured by slant iono
# --------------------------------------------------------------------------- #
def _plot_sky(df, sys_present):
    az = df["azimuth_deg"].to_numpy(float)
    if not np.any(np.isfinite(az)):
        return None

    # Shared colour scale across constellations (2nd/98th percentile).
    iono_all = df["iono_m"].to_numpy(float)
    iono_all = iono_all[np.isfinite(iono_all)]
    if iono_all.size:
        c_min = float(np.quantile(iono_all, 0.02))
        c_max = float(np.quantile(iono_all, 0.98))
    else:
        c_min = c_max = None
    clim = ((c_min, c_max) if (c_min is not None and c_max is not None
                               and np.isfinite(c_min) and np.isfinite(c_max)
                               and c_max > c_min) else None)

    cmap = _viridis(256)

    axes_list: list = []
    n_sys = len(sys_present)
    for i, sys_name in enumerate(sys_present):
        d_sys = df[df["system"] == sys_name]
        ax = Axes(projection="polar", title=sys_name,
                  theta_zero="N", theta_dir=-1, rlim=(0.0, 90.0),
                  rticks=[0, 30, 60, 90],
                  rtick_labels=["90", "60", "30", "0"],  # elevation labels
                  cmap=cmap, clim=clim, x_is_datetime=False)

        if not d_sys.empty:
            a = d_sys["azimuth_deg"].to_numpy(float)
            rho = 90.0 - d_sys["elevation_deg"].to_numpy(float)
            cval = d_sys["iono_m"].to_numpy(float)
            ok = np.isfinite(a) & np.isfinite(rho) & np.isfinite(cval)
            if ok.any():
                ax.traces = [PolarScatter(theta_deg=a[ok], r=rho[ok],
                                          c=cval[ok], size=14)]

        # One shared colorbar, attached to the last polar tile (mirrors the
        # MATLAB single ``colorbar(ax,'eastoutside')`` after the loop).
        if i == n_sys - 1:
            ax.colorbar = ColorBar(label="Slant iono (m)")
        axes_list.append(ax)

    return Figure(name="Sky plot - iono",
                  title="Sky plot colored by slant iono (m)",
                  axes=axes_list, width=max(11.0, 4.2 * n_sys),
                  height=min(11.0, 4.0 * n_sys), link_x=False)


# --------------------------------------------------------------------------- #
#  BSSD pivot helpers (handover detection + first-pivot re-reference)
# --------------------------------------------------------------------------- #
def _collect_pivot_handovers(df) -> dict:
    """Map constellation -> sorted datetime64[] of every BSSD pivot handover.

    A handover is declared when ``pivot_satellite`` changes between two
    successive unique epochs (both sides non-empty), per constellation.  Returns
    an empty dict for traces without pivot info.
    """
    out: dict = {}
    if "pivot_satellite" not in df.columns:
        return out
    piv = df["pivot_satellite"].astype(str)
    if (piv == "").all():
        return out

    for sys_name in _SYS_ORDER:
        d_sys = df[df["system"] == sys_name]
        if d_sys.empty:
            continue
        d_sys = d_sys.sort_values("utc")
        # Pivot per unique epoch (first row of each epoch group).
        per = (d_sys.groupby(d_sys["utc"].to_numpy("datetime64[ns]"),
                             sort=True)["pivot_satellite"]
               .first().astype(str))
        epochs = per.index.to_numpy()
        pivs = per.to_numpy()
        if epochs.size < 2:
            continue
        change = np.zeros(epochs.size, dtype=bool)
        for ei in range(1, epochs.size):
            if (pivs[ei] != pivs[ei - 1] and pivs[ei] != "" and pivs[ei - 1] != ""):
                change[ei] = True
        if change.any():
            out[sys_name] = epochs[change]
    return out


def _re_reference_to_first_pivot(df):
    """Re-reference BSSD iono states to the first pivot seen per constellation.

    Walk epochs chronologically per constellation, accumulating a running offset
    ``O = I_pivot(now) - I_pivot(first)``.  At each handover P_old -> P_new, the
    new pivot's stored value at the LAST epoch in P_old's frame is
    ``I_P_new - I_P_old`` -- exactly the increment to O.  O is then added to every
    row at the current epoch (the pivot row's raw 0 becomes O, its iono delay in
    the first-pivot reference).  ``std_m`` is intentionally NOT re-propagated
    (the displayed sigma is a lower bound).  VTEC is recomputed from the
    re-referenced slant through the mapping factor.
    """
    if "pivot_satellite" not in df.columns:
        return df
    if (df["pivot_satellite"].astype(str) == "").all():
        return df

    df = df.copy()
    iono_out = df["iono_m"].to_numpy(float).copy()
    utc_ns = df["utc"].to_numpy("datetime64[ns]")
    sat_str = df["satellite"].astype(str).to_numpy()
    piv_str = df["pivot_satellite"].astype(str).to_numpy()
    iono_in = df["iono_m"].to_numpy(float)

    for sys_name in _SYS_ORDER:
        rows_sys = np.where(df["system"].to_numpy() == sys_name)[0]
        if rows_sys.size == 0:
            continue
        # Chronological order within the constellation.
        order = np.argsort(utc_ns[rows_sys], kind="stable")
        rows_sorted = rows_sys[order]

        u = utc_ns[rows_sorted]
        unique_epochs, ix = np.unique(u, return_inverse=True)
        n_e = unique_epochs.size

        # Pivot at each unique epoch (first row of the group).
        piv_per_epoch = np.empty(n_e, dtype=object)
        for e in range(n_e):
            first_local = np.where(ix == e)[0][0]
            piv_per_epoch[e] = piv_str[rows_sorted[first_local]]

        if all(p == "" for p in piv_per_epoch):
            continue

        offset = 0.0
        for e in range(n_e):
            if (e > 0 and piv_per_epoch[e] != piv_per_epoch[e - 1]
                    and piv_per_epoch[e] != "" and piv_per_epoch[e - 1] != ""):
                # New pivot's value at the previous epoch (its old-frame
                # I_newpivot - I_oldpivot); add to running offset.
                prev_local = np.where(
                    (ix == (e - 1))
                    & (sat_str[rows_sorted] == piv_per_epoch[e]))[0]
                if prev_local.size:
                    offset += float(iono_in[rows_sorted[prev_local[0]]])

            this_local = np.where(ix == e)[0]
            orig_idx = rows_sorted[this_local]
            iono_out[orig_idx] = iono_in[orig_idx] + offset

    df["iono_m"] = iono_out
    if "mapping_factor" in df.columns and "vtec_m" in df.columns:
        df["vtec_m"] = df["iono_m"].to_numpy(float) / df["mapping_factor"].to_numpy(float)
    return df


# --------------------------------------------------------------------------- #
#  Small numeric / formatting helpers
# --------------------------------------------------------------------------- #
def _insert_gap_breaks(t, y, gap_factor):
    """Insert NaT/NaN rows where the series has a gap (MATLAB ``insertGapBreaks``).

    ``t`` is a ``datetime64[ns]`` array, ``y`` a float array.  A gap is declared
    when ``dt > gap_factor * median(dt)``; a NaN sample is inserted so the line
    draws a break rather than a bridge.  Returns sorted (t_out, y_out).
    """
    t = np.asarray(t, dtype="datetime64[ns]")
    y = np.asarray(y, dtype=float)
    if t.size < 2:
        return t, y
    order = np.argsort(t, kind="stable")
    t = t[order]
    y = y[order]
    dt_s = np.diff(t).astype("timedelta64[ns]").astype(np.float64) / 1e9
    med = float(np.median(dt_s))
    if not np.isfinite(med) or med <= 0:
        med = 1.0
    gap_idx = np.where(dt_s > gap_factor * med)[0]
    if gap_idx.size == 0:
        return t, y

    t_out: list = []
    y_out: list = []
    r_start = 0
    nat = np.datetime64("NaT")
    for g in gap_idx:
        t_out.extend(t[r_start:g + 1].tolist())
        y_out.extend(y[r_start:g + 1].tolist())
        t_out.append(nat)
        y_out.append(np.nan)
        r_start = g + 1
    t_out.extend(t[r_start:].tolist())
    y_out.extend(y[r_start:].tolist())
    return np.array(t_out, dtype="datetime64[ns]"), np.array(y_out, dtype=float)


def _std(v):
    """MATLAB sample std (ddof=1); 0.0 for a single sample, NaN for empty."""
    v = np.asarray(v, dtype=float)
    v = v[np.isfinite(v)]
    if v.size == 0:
        return float("nan")
    return float(np.std(v, ddof=1)) if v.size > 1 else 0.0


def _prn_order(sats):
    """Stable unique satellite IDs sorted by numeric PRN (chars 2-3), rest last."""
    uniq = list(dict.fromkeys(str(s) for s in sats))

    def key(s):
        try:
            return (0, int(s[1:3]))
        except (ValueError, IndexError):
            return (1, uniq.index(s))
    return sorted(uniq, key=key)


def _system_of_letter(sat):
    return {"G": "GPS", "E": "Galileo", "R": "GLONASS", "C": "BeiDou",
            "J": "QZSS", "S": "SBAS"}.get(str(sat)[:1], "Unknown")


def _thin_shell_mapping_factor(el_deg):
    """Single-layer iono mapping factor (slant/vertical) -- ``thinShellMappingFactor``."""
    el = np.asarray(el_deg, dtype=float) * np.pi / 180.0
    sin_zp = (_RE / (_RE + _H)) * np.cos(el)
    cos_zp = np.sqrt(np.maximum(0.0, 1.0 - sin_zp ** 2))
    with np.errstate(divide="ignore", invalid="ignore"):
        mf = 1.0 / cos_zp
    bad = ~np.isfinite(np.asarray(el_deg, dtype=float)) | (np.asarray(el_deg, dtype=float) <= 0)
    mf = np.where(bad, np.nan, mf)
    return mf


def _viridis(n):
    """Viridis (n,3) RGB array -- reuse the strip-colormap anchor tables."""
    from ..colormaps import strip_colormap
    return strip_colormap("viridis", n)


# --------------------------------------------------------------------------- #
#  Summary
# --------------------------------------------------------------------------- #
def _summary(df, sys_present, handovers, mode, n_handovers, fname) -> str:
    sats = _prn_order(df["satellite"].unique())
    utc = df["utc"]
    out = ["=== Ionosphere Trace Summary ==="]
    out.append(f"File           : {fname}")
    out.append(f"Epochs         : {utc.nunique()} unique")
    out.append(f"Satellites     : {len(sats)} ({', '.join(sats)})")
    out.append(f"Span (GPS)     : {utc.min()} -> {utc.max()}")
    if mode.lower() == "firstpivot" and n_handovers > 0:
        out.append(f"Re-referenced  : {n_handovers} handover(s) across "
                   f"{len(handovers)} constellation(s) to first pivot "
                   f"(mode='firstPivot')")
    elif mode.lower() == "rawbssd" and n_handovers > 0:
        out.append(f"Raw BSSD       : {n_handovers} handover jump(s) visible "
                   f"(mode='rawBssd')")

    out.append("")
    out.append(f"{'Satellite':<9} {'System':<9} {'N':>6} {'Mean(m)':>10} "
               f"{'Std(m)':>10} {'MeanVTEC':>10} {'MeanSigma':>10} {'MeanEl':>8}")
    out.append("-" * 80)
    for sat in sats:
        d = df[df["satellite"] == sat]
        if d.empty:
            continue
        vals = d["iono_m"].to_numpy(float)
        vtec = d["vtec_m"].to_numpy(float)
        sig = d["std_m"].to_numpy(float)
        el = d["elevation_deg"].to_numpy(float)
        sysn = str(d["system"].iloc[0])
        out.append(f"{sat:<9} {sysn:<9} {len(d):>6} "
                   f"{_nanmean(vals):>10.3f} {_std(vals):>10.3f} "
                   f"{_nanmean(vtec):>10.3f} {_nanmean(sig):>10.4f} "
                   f"{_nanmean(el):>8.2f}")

    # BSSD pivot tenure.
    piv_rows = df[df["is_pivot"].to_numpy(float) > 0]
    if not piv_rows.empty:
        out.append("")
        out.append("=== BSSD Pivot Tenure ===")
        for sat in _prn_order(piv_rows["satellite"].unique()):
            tp = piv_rows[piv_rows["satellite"] == sat]["utc"]
            out.append(f"  {sat} pivot for {len(tp)} epochs : "
                       f"{tp.min()} -> {tp.max()}")
    return "\n".join(out)


def _nanmean(v):
    v = np.asarray(v, dtype=float)
    v = v[np.isfinite(v)]
    return float(v.mean()) if v.size else float("nan")


viewer = Viewer(name="ionosphere", title="Ionosphere / slant-iono & VTEC",
                required_csvs=("ionosphere_trace.csv",), build=build_ionosphere)

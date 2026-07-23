"""Truth-referenced ionospheric storm — the exam's injected-iono answer key.

Reads the GnssSimulator answer key ``truth_atmo.csv`` (the per-satellite slant
ionosphere the synthesizer *injected*, bound to a
:class:`~gnssviz.trace_paths.TraceContext` via ``--truth-dir``) and shows the
storm exactly as it was written into the observations, so the reviewer can read
the injected disturbance off the same time base as the recovered solution:

  1. A diverging ``imagesc``-style :class:`Raster` of the storm slant delay
     ``dI_L1_m`` over (satellite rows × epoch columns): a symmetric red/blue map
     centred on zero (``clim = ±max|dI|``) makes the enhancement (positive) vs
     depletion (negative) lobes read at a glance; NaN (satellite not visible that
     epoch) cells stay transparent, so each satellite's arc shows as a coloured
     band across the storm window.
  2. A per-satellite ``dI_L1`` spaghetti plot vs GPS time — one gap-aware line per
     satellite coloured by PRN, the storm window drawn as paired :class:`VLine`s,
     and the storm peak (from ``scenario_summary.json`` when present, else the
     ``argmax`` of ``dI_L1``) called out with a ring + stat box.

``dI_L1`` is the storm-only slant delay (``stec_total − stec_quiet`` scaled to L1
metres); the quiet Klobuchar background is deliberately *not* drawn here — this
viewer isolates the injected disturbance the processor must ride through.
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..colormaps import strip_colormap
from ..figures import Axes, ColorBar, Figure, Legend, Line, Raster, Scatter, TextBox
from ..io import add_utc, first_col, gps_to_datetime
from ..palettes import color_by_prn
from .base import Viewer, ViewerResult, ViewerSkip
from . import exam_common as ec

# System render rank so raster rows group GPS block, then Galileo, ... (a
# constellation-blocked answer key reads more naturally than a bare PRN sort).
_SYS_RANK = {"G": 0, "E": 1, "R": 2, "C": 3, "J": 4, "S": 5}
_GAP_FACTOR = 3.0                       # NaN-break a line when dt > 3*median(dt)
_PEAK_RING = (0.0, 0.0, 0.0)


# --------------------------------------------------------------------------- #
#  Small helpers (local — no cross-viewer imports beyond exam_common)
# --------------------------------------------------------------------------- #
def _prn_order(sats):
    """Unique satellite IDs ordered by (system rank, numeric PRN); rest last."""
    uniq = list(dict.fromkeys(str(s) for s in sats))

    def key(s):
        rank = _SYS_RANK.get(s[:1], 9)
        try:
            return (0, rank, int(s[1:3]))
        except (ValueError, IndexError):
            return (1, rank, uniq.index(s))
    return sorted(uniq, key=key)


def _insert_gap_breaks(t, y, gap_factor=_GAP_FACTOR):
    """Insert a NaT/NaN sample wherever ``dt > gap_factor*median(dt)``.

    ``t`` is ``datetime64[ns]``; ``y`` float.  A break stops a line from bridging
    a satellite's set/rise hole (matches the ``insertGapBreaks`` idiom used across
    the suite).  Returns sorted ``(t_out, y_out)``.
    """
    t = np.asarray(t, dtype="datetime64[ns]")
    y = np.asarray(y, dtype=float)
    if t.size < 2:
        return t, y
    order = np.argsort(t, kind="stable")
    t, y = t[order], y[order]
    dt_s = np.diff(t).astype("timedelta64[ns]").astype(np.float64) / 1e9
    med = float(np.median(dt_s))
    if not np.isfinite(med) or med <= 0:
        med = 1.0
    gaps = np.where(dt_s > gap_factor * med)[0]
    if gaps.size == 0:
        return t, y
    t_out, y_out, r0, nat = [], [], 0, np.datetime64("NaT")
    for g in gaps:
        t_out.extend(t[r0:g + 1].tolist())
        y_out.extend(y[r0:g + 1].tolist())
        t_out.append(nat)
        y_out.append(np.nan)
        r0 = g + 1
    t_out.extend(t[r0:].tolist())
    y_out.extend(y[r0:].tolist())
    return np.array(t_out, dtype="datetime64[ns]"), np.array(y_out, dtype=float)


def _storm_peak(ctx, df):
    """``(sat, tow_s, dI_m, utc)`` of the storm peak.

    Prefer the recorded answer-key peak in ``scenario_summary.json``
    (``storm.peak_sat`` / ``peak_tow_s`` / ``peak_slant_dI_L1_m``); fall back to
    the ``argmax`` of ``dI_L1_m`` in the truth frame.  ``utc`` is derived from the
    frame's ``week`` at the peak epoch.  Returns ``None`` when unavailable.
    """
    ss = ec.scenario_summary(ctx)
    storm = ss.get("storm", {}) if isinstance(ss, dict) else {}
    sat = storm.get("peak_sat") if isinstance(storm, dict) else None
    tow = storm.get("peak_tow_s") if isinstance(storm, dict) else None
    val = storm.get("peak_slant_dI_L1_m") if isinstance(storm, dict) else None

    di = df["dI_L1_m"].to_numpy(float)
    if not np.any(np.isfinite(di)):
        return None
    if sat is None or tow is None or val is None:
        i = int(np.nanargmax(di))
        sat = str(df["satellite"].iloc[i])
        tow = float(df["tow_s"].iloc[i])
        val = float(di[i])
    try:
        tow = float(tow)
        val = float(val)
    except (TypeError, ValueError):
        return None

    wk = first_col(df, "week")
    if wk is not None and len(wk):
        # Prefer this satellite's own week at that epoch; else any row's week.
        row = df[(df["satellite"].astype(str) == str(sat))
                 & (np.isclose(df["tow_s"].to_numpy(float), tow))]
        week = float(row["week"].iloc[0]) if not row.empty else float(wk.iloc[0])
        utc = list(gps_to_datetime([week], [tow]))[0]
    else:
        utc = None
    return str(sat), tow, val, utc


# --------------------------------------------------------------------------- #
#  Build
# --------------------------------------------------------------------------- #
def _iono_raster(df, sats, value_col, *, diverging, title, cbar_label,
                 width=13.0):
    """A (sat × epoch) :class:`Raster` of ``value_col`` from truth_atmo.

    ``diverging`` picks a symmetric red/blue map centred on 0 (for the storm
    delta); otherwise a sequential viridis clamped at 0 (for the total delay).
    Returns ``None`` if the column is absent.
    """
    if value_col not in df.columns:
        return None
    piv = df.pivot_table(index="satellite", columns="tow_s",
                         values=value_col, aggfunc="mean").reindex(index=sats)
    tows = piv.columns.to_numpy(float)
    grid = piv.to_numpy(float)                         # (n_sat, n_epoch); NaN gaps
    wk_by_tow = df.groupby("tow_s")["week"].first()
    weeks = wk_by_tow.reindex(piv.columns).to_numpy(float)
    col_times = list(gps_to_datetime(weeks, tows))

    vals = df[value_col].to_numpy(float)
    finite = np.any(np.isfinite(vals))
    if diverging:
        vmax = float(np.nanmax(np.abs(vals))) if finite else 0.0
        if not (vmax > 0):
            vmax = 1.0
        cmap, clim = strip_colormap("rdbu", 256), (-vmax, vmax)
    else:
        vmax = float(np.nanmax(vals)) if finite else 0.0
        if not (vmax > 0):
            vmax = 1.0
        cmap, clim = strip_colormap("viridis", 256), (0.0, vmax)

    ax = Axes(traces=[Raster(x=col_times, row_labels=sats, grid=grid)],
              title=title, xlabel="GPS Time", ylabel="Satellite",
              grid=False, x_is_datetime=True)
    return Figure(name=title, axes=[ax], width=width,
                  height=min(10.0, 0.34 * len(sats) + 2.0),
                  cmap=cmap, clim=clim, colorbar=ColorBar(label=cbar_label))


def build_exam_iono_truth(ctx) -> ViewerResult:
    df = ec.require_truth(ctx, "truth_atmo.csv").copy()
    if first_col(df, "dI_L1_m") is None:
        raise ViewerSkip("truth_atmo.csv lacks dI_L1_m (no storm slant-delay column)")
    df["satellite"] = df["satellite"].astype(str)
    df = add_utc(df)

    sats = _prn_order(df["satellite"].unique())
    sat_color = color_by_prn(df["satellite"].tolist())
    figures: list = []

    # ---- Figure 1: TOTAL slant iono I_L1 (quiet Klobuchar + storm) ---------- #
    # The realistic full slant delay riding on every ray — the background a
    # single-frequency (Part B) student must model/remove; it is NOT zero away
    # from the storm.  Sequential (all-positive) map, clamped at 0.
    fig_total = _iono_raster(df, sats, "I_L1_m", diverging=False,
                             title="Total slant iono I_L1 (Klobuchar + storm)",
                             cbar_label="I_L1 (m)")
    if fig_total is not None:
        figures.append(fig_total)

    # ---- Figure 2: storm ENHANCEMENT dI_L1 (total − quiet), diverging ------- #
    # The injected disturbance ON TOP of the quiet background — ~0 outside the
    # storm window by construction; the symmetric caxis shows enhancement vs
    # depletion lobes.
    fig_delta = _iono_raster(df, sats, "dI_L1_m", diverging=True,
                             title="Storm enhancement dI_L1 (total - quiet)",
                             cbar_label="dI_L1 (m)")
    if fig_delta is not None:
        figures.append(fig_delta)

    # ---- Figure 3: per-satellite TOTAL I_L1 spaghetti + storm peak ---------- #
    val_col = "I_L1_m" if "I_L1_m" in df.columns else "dI_L1_m"
    is_total = val_col == "I_L1_m"
    ax_s = Axes(title=("Per-satellite total slant iono I_L1 with storm peak"
                       if is_total else "Per-satellite dI_L1 with storm peak"),
                xlabel="GPS Time", ylabel=f"{val_col[:-2]} (m)", grid=True,
                legend=Legend(loc="best"))
    traces: list = []
    for sat in sats:
        sub = df[df["satellite"] == sat].sort_values("utc")
        t = sub["utc"].to_numpy("datetime64[ns]")
        v = sub[val_col].to_numpy(float)
        if t.size == 0:
            continue
        tt, vv = _insert_gap_breaks(t, v)
        traces.append(Line(x=tt, y=vv, color=sat_color[sat], width=0.9,
                           label=sat, show_in_legend=False))

    traces += ec.storm_vlines(ctx)

    peak = _storm_peak(ctx, df)
    if peak is not None:
        p_sat, p_tow, p_val, p_utc = peak
        # The peak stat is the storm delta; on a total-I_L1 plot mark the ring at
        # the sat's total delay there so it sits on the drawn line.
        p_total = p_val
        if is_total:
            row = df[(df["satellite"] == p_sat)
                     & (np.isclose(df["tow_s"].to_numpy(float), p_tow))]
            if not row.empty:
                p_total = float(row["I_L1_m"].iloc[0])
        if p_utc is not None:
            traces.append(Scatter(x=[p_utc], y=[p_total], size=44, marker="o",
                                  color=_PEAK_RING, alpha=1.0,
                                  label="storm peak", show_in_legend=True,
                                  zorder=5))
        traces.append(TextBox(
            x=0.02, y=0.97, mono=True, bold=True, boxed=True,
            text=(f"storm peak dI_L1: {p_val:+.2f} m\n"
                  f"sat {p_sat}  @ tow {p_tow:.1f} s"
                  + (f"\n{pd.Timestamp(p_utc):%H:%M:%S} GPS" if p_utc is not None else ""))))
    ax_s.traces = traces
    figures.append(Figure(name="Per-satellite I_L1", axes=[ax_s],
                          width=13.0, height=4.6))

    return ViewerResult(figures=figures, summary=_summary(df, sats, peak))


def _summary(df, sats, peak) -> str:
    di = df["dI_L1_m"].to_numpy(float)
    n_ep = int(df["tow_s"].nunique())
    out = ["=== Injected ionospheric storm (truth_atmo) ==="]
    out.append(f"Satellites       : {len(sats)} ({', '.join(sats)})")
    out.append(f"Epochs           : {n_ep} unique")
    out.append(f"Samples          : {len(df)} (sat x epoch rows)")
    instorm = None
    if "in_storm_window" in df.columns:
        instorm = (pd.to_numeric(df["in_storm_window"], errors="coerce").fillna(0.0)
                   > 0.5).to_numpy()
        out.append(f"In-storm samples : {int(instorm.sum())} of {len(df)}")
    # Total slant iono (Klobuchar background + storm) — the delay that is NOT zero
    # away from the storm and that a single-freq student must model (cancels in the
    # dual-freq IF combination, so dual-freq PPP is flat).
    if "I_L1_m" in df.columns:
        il1 = df["I_L1_m"].to_numpy(float)
        if instorm is not None:
            q = il1[(~instorm) & np.isfinite(il1)]
            s = il1[instorm & np.isfinite(il1)]
            if q.size:
                out.append(f"I_L1 quiet bkgd  : {q.min():.2f} .. {q.max():.2f} m "
                           f"(mean {q.mean():.2f}) — Klobuchar background")
            if s.size:
                out.append(f"I_L1 in storm    : {s.min():.2f} .. {s.max():.2f} m "
                           f"(mean {s.mean():.2f})")
        else:
            fin = il1[np.isfinite(il1)]
            if fin.size:
                out.append(f"I_L1 total       : {fin.min():.2f} .. {fin.max():.2f} m")
    if np.any(np.isfinite(di)):
        out.append(f"dI_L1 storm delta: {np.nanmin(di):+.3f} .. {np.nanmax(di):+.3f} m")
    if peak is not None:
        p_sat, p_tow, p_val, p_utc = peak
        stamp = f" ({pd.Timestamp(p_utc):%H:%M:%S} GPS)" if p_utc is not None else ""
        out.append(f"Peak dI_L1       : {p_val:+.3f} m @ {p_sat}, "
                   f"tow {p_tow:.1f} s{stamp}")
    return "\n".join(out)


viewer = Viewer(name="exam_iono_truth", title="Injected ionospheric storm (truth)",
                required_csvs=("truth_atmo.csv",),
                build=build_exam_iono_truth)

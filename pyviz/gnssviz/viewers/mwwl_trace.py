"""Melbourne-Wubbena wide-lane (MW WL) viewer -- the port of
``view_mwwl_trace.m``.

Reads ``mwwl_trace.csv`` (the per-epoch MW slip-detector trace) and, when it is
present next to it, ``turbo_edit_trace.csv`` (the TurboEdit gap-bridge trace),
and reproduces the MATLAB diagnostic suite:

  1. **Grid figures** -- one figure per scalar quantity, with one tile per
     (constellation, signal-pair) cell, one coloured line (``plotStyle='lines'``,
     with NaN gap-breaks) or scatter (``plotStyle='scatter'``) per satellite, a
     dashed zero / detector-gate reference line, and a per-tile mu/sigma read-out
     in the corner.  Mirrors ``plotMwwlGrid``.  The MATLAB default switch set
     emits two grids:

       * **MW WL deviation** from the arc mean (``dev_from_mean_cycles``) -- the
         primary diagnostic;
       * **MW WL local-median deviation** (``dev_local_cycles``) -- the
         trend-immune statistic, emitted only when the June-2026 detection-
         reference columns (``local_ref_cycles`` + ``detect_mode``) are present.

     The z-score, threshold-ratio and raw-MW grids are off by default in the
     MATLAB (``showZScorePlot`` / ``showThresholdRatioPlot`` /
     ``showRawMwPlot`` all start ``false``) and here; the ``show_*_plot``
     keyword flags mirror those switches and build the grids when set.

  2. **Slip-example figures** (``showSlipExampleFigs=true``) -- a two-panel zoom
     on one *detected* and one *repair-eligible* slip event, built from the
     pre-``hideSlipEpochs`` snapshot.  Top panel: raw MW with its Hatch-mean and
     local-median reference levels (and, for the repaired exhibit, the
     dN-removed "repaired" series); bottom panel: the active deviation against
     the +/- per-row detector-threshold envelope, with the dN rounding band.
     Mirrors ``plotSlipExample``.

  3. **Gap-bridge figures** (``showGapBridgeFigs=true``, capped at
     ``maxGapBridgeFigs=2``) -- a three-panel zoom on one TurboEdit across-gap
     adjudication: raw MW + flag timing, GF level + pre-gap / predicted-post
     reference, and the gated TD2/across-gap quantity vs the +/- GF gate.
     Mirrors ``plotGapBridgeExample``.  Skipped gracefully when
     ``turbo_edit_trace.csv`` is absent or lacks the gap-bridge columns.

  4. A text **summary** of per-(system, signal-pair) |dev| statistics, plus the
     detection-mode row inventory and the detected/bridged event inventories the
     MATLAB ``fprintf``\\s.

Fidelity notes
--------------
* The MATLAB lays the grid out as ``tiledlayout(1, nCons)`` columns with an
  inner ``tiledlayout(nSigPairs, 1)`` per column.  The backend-agnostic
  :class:`Figure` is a *vertical* stack of x-linked axes, so each grid becomes a
  single column of (constellation -- signal-pair) tiles ordered constellation-
  major, signal-pair-minor (lexicographic), which carries every populated cell
  the MATLAB drew without blank filler tiles.
* ``hideSlipEpochs=true`` (default): the grid figures and summary statistics drop
  rows the C++ detector flagged ``is_slip=1`` (they can be hundreds of cycles and
  would crush the noise floor); the full snapshot is retained for the slip-example
  figures.
* MATLAB ``std`` is the sample standard deviation (ddof=1), 0.0 for a single
  sample; reproduced in :func:`_mu_sigma`.  MATLAB ``round`` is half-away-from-zero
  (reproduced in :func:`_round_half_away`).
* Colours are by numeric PRN via :func:`color_by_prn`, mirroring the MATLAB
  ``distinguishable_colors`` indexed by ``str2double(sat(2:3))``.
* Gap-breaks are inserted where ``dt > gapFactor * median(dt)`` per series
  (:func:`_insert_gap_breaks`), as in ``insertGapBreaks``.
"""

from __future__ import annotations

import os

import numpy as np
import pandas as pd

from ..io import gps_to_datetime, load_trace, require_cols
from ..figures import (Axes, Figure, HLine, Legend, Line, Scatter, Stem,
                       TextBox, VLine)
from ..palettes import color_by_prn
from .base import Viewer, ViewerResult

# Reference-line colours (gray for the y=0 line, red for a threshold boundary).
_GRAY = (0.5, 0.5, 0.5)
_RED = (0.85, 0.1, 0.1)

# Slip-example panel colours (match the MATLAB hard-coded RGB triples).
_C_MW = (0.0, 0.35, 0.75)        # raw MW / dev_active series
_C_HATCH = (0.85, 0.55, 0.0)     # Hatch mean
_C_LOCAL = (0.2, 0.6, 0.2)       # local median
_C_REPAIR = (0.75, 0.0, 0.1)     # dN-removed repaired series
_C_GATE = (0.85, 0.1, 0.1)       # +/- threshold envelope
_C_BRIDGE = (0.75, 0.1, 0.75)    # magenta bridged epochs / across-gap dGF
_C_FLAG = (1.0, 0.3, 0.3)        # MW flag stars (gap-bridge panel 1)
_C_EVENT_FACE = (1.0, 0.85, 0.0) # slip-epoch star fill


# --------------------------------------------------------------------------- #
#  Fidelity helpers
# --------------------------------------------------------------------------- #
def _round_half_away(x):
    """MATLAB ``round`` -- half away from zero (``np.round`` is banker's)."""
    x = np.asarray(x, dtype=float)
    return np.sign(x) * np.floor(np.abs(x) + 0.5)


def _mu_sigma(vals):
    """(mean, std) over finite values; std is sample (ddof=1), 0.0 for one.

    Matches MATLAB ``mean`` / ``std`` semantics (``std`` of a single element is
    0, not NaN).  Returns ``(nan, nan)`` when empty.
    """
    v = np.asarray(vals, dtype=float)
    v = v[np.isfinite(v)]
    if v.size == 0:
        return (float("nan"), float("nan"))
    sd = float(np.std(v, ddof=1)) if v.size > 1 else 0.0
    return (float(np.mean(v)), sd)


def _insert_gap_breaks(t, y, gap_factor):
    """Insert NaT/NaN rows where ``dt > gap_factor * median(dt)``.

    Mirrors ``insertGapBreaks``: sort by time, find gaps larger than the
    factor times the median sample spacing, and splice a NaT/NaN row into each
    so a line trace draws a break rather than crossing the gap.  Returns
    ``(t_out, y_out)`` as numpy arrays (datetime64 / float).
    """
    t = pd.to_datetime(np.asarray(t)).to_numpy()
    y = np.asarray(y, dtype=float)
    if t.size < 2:
        return t, y
    order = np.argsort(t, kind="stable")
    ts = t[order]
    ys = y[order]
    dt_s = np.diff(ts).astype("timedelta64[ns]").astype(float) / 1e9
    med = float(np.median(dt_s))
    if not np.isfinite(med) or med <= 0:
        med = 1.0
    gap_idx = np.flatnonzero(dt_s > gap_factor * med)
    if gap_idx.size == 0:
        return ts, ys
    t_out, y_out = [], []
    start = 0
    for g in gap_idx:
        t_out.append(ts[start:g + 1])
        y_out.append(ys[start:g + 1])
        t_out.append(np.array(["NaT"], dtype="datetime64[ns]"))
        y_out.append(np.array([np.nan]))
        start = g + 1
    t_out.append(ts[start:])
    y_out.append(ys[start:])
    return np.concatenate(t_out), np.concatenate(y_out)


def _utc(df):
    """GPS week + tow_s -> datetime (pure GPS time), as a numpy datetime64."""
    return gps_to_datetime(df["week"].to_numpy(float),
                           df["tow_s"].to_numpy(float)).to_numpy()


# --------------------------------------------------------------------------- #
#  Loading / derived columns
# --------------------------------------------------------------------------- #
_REQUIRED = ["week", "tow_s", "satellite", "system", "sig1", "sig2",
             "mw_cycles", "dev_from_mean_cycles", "mw_sigma_cycles",
             "threshold_cycles"]


def _prepare(df):
    """Add the derived columns the MATLAB synthesizes (signal, utc, deviations).

    Returns ``(df, has_local_ref, has_active_dev)`` with ``df`` carrying:
    ``signal``, ``utc``, ``mw_zscore``, ``dev_local_cycles`` (if local-ref
    columns present), ``dev_active_cycles``, and ``mw_threshold_ratio``.
    """
    df = df.copy()
    # Coerce numerics (readtable may mis-type).
    for c in ("week", "tow_s", "mw_cycles", "dev_from_mean_cycles",
              "mw_sigma_cycles", "threshold_cycles"):
        df[c] = pd.to_numeric(df[c], errors="coerce")
    for c in ("is_slip", "is_lli", "arc_count", "wl_locked", "rounded_wl",
              "mw_mean_cycles", "detect_mode", "local_ref_cycles",
              "local_sigma_cycles", "active_dev_cycles"):
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")
    if "is_slip" in df.columns:
        df["is_slip"] = df["is_slip"].fillna(0.0)

    # Signal-pair partition key and GPS-time axis.
    df["signal"] = df["sig1"].astype(str) + "/" + df["sig2"].astype(str)
    df["utc"] = _utc(df)

    # z-score (synthesized) -- dev / sigma, infinities -> NaN.
    with np.errstate(divide="ignore", invalid="ignore"):
        z = df["dev_from_mean_cycles"].to_numpy(float) / df["mw_sigma_cycles"].to_numpy(float)
    z[~np.isfinite(z)] = np.nan
    df["mw_zscore"] = z

    # Detection-reference columns (June 2026 schema).
    has_local_ref = ("local_ref_cycles" in df.columns
                     and "detect_mode" in df.columns)
    has_active_dev = "active_dev_cycles" in df.columns
    if has_local_ref:
        df["detect_mode"] = df["detect_mode"].fillna(0.0)
        df["dev_local_cycles"] = (df["mw_cycles"].to_numpy(float)
                                  - df["local_ref_cycles"].to_numpy(float))

    dev_from_mean = df["dev_from_mean_cycles"].to_numpy(float)
    if has_active_dev:
        act = df["active_dev_cycles"].to_numpy(float).copy()
        nan_rows = ~np.isfinite(act)
        act[nan_rows] = dev_from_mean[nan_rows]
        df["dev_active_cycles"] = act
    elif has_local_ref:
        act = dev_from_mean.copy()
        dev_local = df["dev_local_cycles"].to_numpy(float)
        loc_rows = (df["detect_mode"].to_numpy(float) == 1) & np.isfinite(dev_local)
        if "is_slip" in df.columns:
            backstop = (df["is_slip"].to_numpy(float).astype(bool)
                        & (df["detect_mode"].to_numpy(float) == 1)
                        & (np.abs(df["threshold_cycles"].to_numpy(float) - 0.4) < 1e-6))
            loc_rows = loc_rows & ~backstop
        act[loc_rows] = dev_local[loc_rows]
        df["dev_active_cycles"] = act
    else:
        df["dev_active_cycles"] = dev_from_mean

    # Threshold-ratio (synthesized, detector-faithful): dev_active / threshold.
    with np.errstate(divide="ignore", invalid="ignore"):
        r = df["dev_active_cycles"].to_numpy(float) / df["threshold_cycles"].to_numpy(float)
    r[~np.isfinite(r)] = np.nan
    df["mw_threshold_ratio"] = r
    return df, has_local_ref, has_active_dev


def _systems_present(df):
    """Constellations present, in first-appearance order (categories order)."""
    out = []
    for s in df["system"].astype(str):
        if s not in out:
            out.append(s)
    return out


# --------------------------------------------------------------------------- #
#  Grid figure (plotMwwlGrid)
# --------------------------------------------------------------------------- #
def _build_grid(df, value_column, value_label, fig_name, systems_present,
                plot_style, gap_factor, show_legend, ref_lines):
    """One figure: a vertical stack of (constellation, signal-pair) tiles.

    Each tile draws one line (gap-broken) or scatter per satellite coloured by
    PRN, the requested dashed reference line(s), and a mu/sigma corner read-out.
    Returns ``None`` for an empty / unpopulated quantity (MATLAB skips the figure).
    """
    if df.empty:
        return None

    axes = []
    for sys_name in systems_present:
        sys_rows = df["system"].astype(str) == sys_name
        if not sys_rows.any():
            continue
        sigs = sorted(s for s in df.loc[sys_rows, "signal"].astype(str).unique()
                      if s and s != "<NA>")
        for sig in sigs:
            sub = df[(df["system"].astype(str) == sys_name)
                     & (df["signal"].astype(str) == sig)]
            if sub.empty:
                continue
            ax = Axes(title=f"{sys_name} - {sig}", ylabel=value_label, grid=True,
                      legend=Legend(loc="bestoutside") if show_legend else None)

            sats = list(sub["satellite"].astype(str).unique())
            cmap = color_by_prn(sats)
            for sat in sats:
                s_sat = sub[sub["satellite"].astype(str) == sat]
                color = cmap[sat]
                t = s_sat["utc"].to_numpy()
                y = s_sat[value_column].to_numpy(float)
                if plot_style == "lines":
                    tt, yy = _insert_gap_breaks(t, y, gap_factor)
                    ax.traces.append(Line(x=tt, y=yy, color=color, width=1.0,
                                          label=sat, show_in_legend=show_legend))
                else:
                    ax.traces.append(Scatter(x=t, y=y, color=color, size=10.0,
                                             marker="o", label=sat,
                                             show_in_legend=show_legend))

            for yref in np.atleast_1d(ref_lines):
                if yref == 0:
                    ax.traces.append(HLine(y=float(yref), color=_GRAY, style="--"))
                else:
                    ax.traces.append(HLine(y=float(yref), color=_RED, style="--"))

            mu, sg = _mu_sigma(sub[value_column].to_numpy(float))
            if np.isfinite(mu):
                ax.traces.append(TextBox(
                    x=0.98, y=0.97,
                    text=f"μ={mu:.3g}  σ={sg:.3g}",
                    ha="right", va="top", boxed=True))
            axes.append(ax)

    if not axes:
        return None
    axes[-1].xlabel = "GPS Time"
    n_tiles = len(axes)
    return Figure(name=fig_name, title=fig_name, axes=axes,
                  width=13.0, height=max(4.5, 2.4 * n_tiles))


# --------------------------------------------------------------------------- #
#  Slip-example figure (plotSlipExample)
# --------------------------------------------------------------------------- #
def _slip_example(tall, ev, fig_name, dN, has_local_ref, gap_factor):
    """Two-panel zoom on one slip event (raw MW + active-dev-vs-gate)."""
    half_win = 60
    sat = str(ev["satellite"])
    sig = str(ev["signal"])
    t0 = ev["utc"]

    rows = ((tall["satellite"].astype(str) == sat)
            & (tall["signal"].astype(str) == sig))
    S = tall[rows].sort_values("utc").reset_index(drop=True)
    # Event index: prefer the flagged row at t0, else any row at t0.
    same_t = S["utc"].to_numpy() == np.datetime64(pd.Timestamp(t0))
    flagged = (S["is_slip"].to_numpy(float).astype(bool)
               if "is_slip" in S.columns else np.zeros(len(S), bool))
    cand = np.flatnonzero(same_t & flagged)
    if cand.size == 0:
        cand = np.flatnonzero(same_t)
    if cand.size == 0:
        return None
    i0 = int(cand[0])
    lo = max(0, i0 - half_win)
    hi = min(len(S), i0 + half_win + 1)
    W = S.iloc[lo:hi].reset_index(drop=True)
    i_ev = i0 - lo

    dN_finite = np.isfinite(dN)
    evt_desc = (f"dN_WL = {int(_round_half_away(dN)):+d} cyc" if dN_finite
                else "unknown integer")
    title = f"{fig_name} -- {sat} {sig} @ {pd.Timestamp(t0)} ({evt_desc})"

    # --- Panel 1: raw MW + reference levels ---
    ax1 = Axes(title="Raw wide-lane and reference levels",
               ylabel="MW WL (cycles)", legend=Legend(loc="best"))
    tt, yy = _insert_gap_breaks(W["utc"], W["mw_cycles"], gap_factor)
    ax1.traces.append(Line(x=tt, y=yy, color=_C_MW, width=0.8, label="MW raw"))
    if "mw_mean_cycles" in W.columns:
        tt, yy = _insert_gap_breaks(W["utc"], W["mw_mean_cycles"], gap_factor)
        ax1.traces.append(Line(x=tt, y=yy, color=_C_HATCH, width=1.1,
                               label="Hatch mean"))
    if has_local_ref and "local_ref_cycles" in W.columns:
        tt, yy = _insert_gap_breaks(W["utc"], W["local_ref_cycles"], gap_factor)
        ax1.traces.append(Line(x=tt, y=yy, color=_C_LOCAL, width=1.1,
                               label="local median"))
    if dN_finite and dN != 0:
        post = W["utc"].to_numpy() >= np.datetime64(pd.Timestamp(t0))
        tt, yy = _insert_gap_breaks(W["utc"][post],
                                    W["mw_cycles"][post].to_numpy(float) - dN,
                                    gap_factor)
        ax1.traces.append(Line(x=tt, y=yy, color=_C_REPAIR, width=1.2,
                               style="--",
                               label=f"repaired: MW - ({int(dN):+d})"))
    ax1.traces.append(VLine(x=pd.Timestamp(t0), color=(0, 0, 0), style=":"))
    ax1.traces.append(Scatter(x=[W["utc"].iloc[i_ev]],
                              y=[float(W["mw_cycles"].iloc[i_ev])],
                              color=_C_EVENT_FACE, size=80.0, marker="*",
                              label="slip epoch"))

    # --- Panel 2: active deviation vs detector gate ---
    ax2 = Axes(title="Active deviation vs detector gate",
               xlabel="GPS Time", ylabel="dev_active (cycles)",
               legend=Legend(loc="best"))
    tt, yy = _insert_gap_breaks(W["utc"], W["dev_active_cycles"], gap_factor)
    ax2.traces.append(Line(x=tt, y=yy, color=_C_MW, width=0.8, label="dev_active"))
    tt, yy = _insert_gap_breaks(W["utc"], W["threshold_cycles"], gap_factor)
    ax2.traces.append(Line(x=tt, y=yy, color=_C_GATE, width=0.9, style="--",
                           label="+/- threshold"))
    tt, yy = _insert_gap_breaks(W["utc"], -W["threshold_cycles"].to_numpy(float),
                                gap_factor)
    ax2.traces.append(Line(x=tt, y=yy, color=_C_GATE, width=0.9, style="--",
                           show_in_legend=False))
    if dN_finite:
        ax2.traces.append(HLine(y=float(dN), color=(0.4, 0.4, 0.4), style=":",
                                label=f"dN = {int(dN):+d}"))
        ax2.traces.append(HLine(y=float(dN) + 0.25, color=(0.7, 0.7, 0.7), style=":"))
        ax2.traces.append(HLine(y=float(dN) - 0.25, color=(0.7, 0.7, 0.7), style=":"))
    ax2.traces.append(VLine(x=pd.Timestamp(t0), color=(0, 0, 0), style=":"))
    ax2.traces.append(Scatter(x=[W["utc"].iloc[i_ev]],
                              y=[float(W["dev_active_cycles"].iloc[i_ev])],
                              color=_C_EVENT_FACE, size=80.0, marker="*",
                              label="slip epoch"))
    dev = float(ev["dev_active_cycles"])
    thr = float(ev["threshold_cycles"])
    ratio = abs(dev) / thr if thr else float("nan")
    ax2.traces.append(TextBox(
        x=0.02, y=0.95,
        text=f"dev = {dev:+.3f} cyc   thresh = {thr:.3f} cyc   "
             f"|dev|/thresh = {ratio:.1f}",
        ha="left", va="top", boxed=True))

    return Figure(name=fig_name, title=title, axes=[ax1, ax2],
                  width=9.2, height=6.4)


# --------------------------------------------------------------------------- #
#  Gap-bridge figure (plotGapBridgeExample)
# --------------------------------------------------------------------------- #
def _gap_bridge_example(te, tmw, ev, has_ints, gap_factor):
    """Three-panel zoom on one TurboEdit gap-bridge adjudication."""
    half_win_s = 90.0
    sat = str(ev["satellite"])
    sig = str(ev["signal"])
    t0 = pd.Timestamp(ev["utc"])
    t0_64 = np.datetime64(t0)

    def _within(tbl):
        dt = (tbl["utc"].to_numpy().astype("datetime64[ns]") - t0_64)
        dt_s = np.abs(dt.astype("timedelta64[ns]").astype(float)) / 1e9
        return ((tbl["satellite"].astype(str) == sat)
                & (tbl["signal"].astype(str) == sig)
                & (dt_s <= half_win_s))

    W = te[_within(te)].sort_values("utc").reset_index(drop=True)
    if W.empty:
        return None
    is_br = W["bridged"].to_numpy(float) > 0
    ev_mask = W["utc"].to_numpy() == t0_64
    i_ev = int(np.flatnonzero(ev_mask)[0]) if ev_mask.any() else None

    act_txt = str(ev["action"])
    if has_ints and (ev.get("dN1", 0) != 0 or ev.get("dN2", 0) != 0):
        act_txt += f"  (dN1={int(ev['dN1']):+d}, dN2={int(ev['dN2']):+d})"
    title = f"Gap-bridge adjudication -- {sat} {sig} @ {t0} -- {act_txt}"

    # --- Panel 1: raw MW + flag timing (from the MW trace) ---
    ax1 = Axes(title="Raw wide-lane: when the MW evidence landed",
               ylabel="MW WL (cycles)", legend=Legend(loc="best"))
    M = tmw[_within(tmw)].sort_values("utc").reset_index(drop=True)
    if M.empty:
        ax1.traces.append(TextBox(x=0.5, y=0.5, text="no MW rows in window",
                                  ha="center", va="center"))
    else:
        tt, yy = _insert_gap_breaks(M["utc"], M["mw_cycles"], gap_factor)
        ax1.traces.append(Line(x=tt, y=yy, color=_C_MW, width=0.8, label="MW raw"))
        if "mw_mean_cycles" in M.columns:
            tt, yy = _insert_gap_breaks(M["utc"], M["mw_mean_cycles"], gap_factor)
            ax1.traces.append(Line(x=tt, y=yy, color=_C_HATCH, width=1.1,
                                   label="Hatch mean"))
        if "is_slip" in M.columns:
            fl = M["is_slip"].to_numpy(float).astype(bool)
            if fl.any():
                ax1.traces.append(Scatter(
                    x=M["utc"][fl].to_numpy(),
                    y=M["mw_cycles"][fl].to_numpy(float),
                    color=_C_FLAG, size=80.0, marker="*", label="MW flag"))
    ax1.traces.append(VLine(x=t0, color=(0, 0, 0), style=":"))

    # --- Panel 2: GF level, pre-gap reference, predicted post level ---
    ax2 = Axes(title="GF level: the across-gap jump the rebuilt TD2 chain cannot see",
               ylabel="GF = lambda1*L1 - lambda2*L2 (m)", legend=Legend(loc="best"))
    tt, yy = _insert_gap_breaks(W["utc"], W["gf_m"], gap_factor)
    ax2.traces.append(Line(x=tt, y=yy, color=_C_MW, width=0.8, label="GF level"))
    if is_br.any():
        ax2.traces.append(Scatter(x=W["utc"][is_br].to_numpy(),
                                  y=W["gf_m"][is_br].to_numpy(float),
                                  color=_C_BRIDGE, size=64.0, marker="s",
                                  label="bridged epoch"))
    pre_idx = None
    if is_br.any():
        first_br = int(np.flatnonzero(is_br)[0])
        if first_br > 0:
            pre_idx = first_br - 1
    elif len(W) > 2:
        dt_s = np.diff(W["utc"].to_numpy()).astype("timedelta64[ns]").astype(float) / 1e9
        pre_idx = int(np.argmax(dt_s))
    if pre_idx is not None:
        pre_lvl = float(W["gf_m"].iloc[pre_idx])
        ax2.traces.append(HLine(y=pre_lvl, color=(0.45, 0.45, 0.45), style="--",
                                label="pre-gap level"))
        if has_ints and (ev.get("dN1", 0) != 0 or ev.get("dN2", 0) != 0):
            pred = (pre_lvl + float(ev["lambda1_m"]) * float(ev["dN1"])
                    - float(ev["lambda2_m"]) * float(ev["dN2"]))
            ax2.traces.append(HLine(
                y=pred, color=_RED, style="--",
                label=f"pre + l1*dN1 - l2*dN2 = {pred:.3f} m"))
    if i_ev is not None:
        ax2.traces.append(Scatter(x=[W["utc"].iloc[i_ev]],
                                  y=[float(W["gf_m"].iloc[i_ev])],
                                  color=_C_EVENT_FACE, size=90.0, marker="*",
                                  label="adjudication epoch"))
    ax2.traces.append(VLine(x=t0, color=(0, 0, 0), style=":"))

    # --- Panel 3: gated quantity vs GF gate ---
    ax3 = Axes(title="Gated quantity vs GF gate: flat chain TD2, exceeding across-gap jump",
               xlabel="GPS Time", ylabel="TD2 / dGF_gap (m)",
               legend=Legend(loc="best"))
    chain = ~is_br
    if chain.any():
        tt, yy = _insert_gap_breaks(W["utc"][chain], W["td2_gf_m"][chain], gap_factor)
        ax3.traces.append(Line(x=tt, y=yy, color=_C_MW, width=0.8, label="chain TD2"))
    if is_br.any():
        ax3.traces.append(Stem(x=W["utc"][is_br].to_numpy(),
                               y=W["td2_gf_m"][is_br].to_numpy(float),
                               color=_C_BRIDGE, marker="s",
                               label="across-gap dGF"))
    tt, yy = _insert_gap_breaks(W["utc"], W["thresh_m"], gap_factor)
    ax3.traces.append(Line(x=tt, y=yy, color=_C_GATE, width=0.9, style="--",
                           label="+/- GF gate"))
    tt, yy = _insert_gap_breaks(W["utc"], -W["thresh_m"].to_numpy(float), gap_factor)
    ax3.traces.append(Line(x=tt, y=yy, color=_C_GATE, width=0.9, style="--",
                           show_in_legend=False))
    ax3.traces.append(VLine(x=t0, color=(0, 0, 0), style=":"))
    if i_ev is not None:
        frac_txt = ""
        if "frac" in W.columns and np.isfinite(float(ev.get("frac", np.nan))):
            frac_txt = f"   frac = {float(ev['frac']):.3f}"
        ax3.traces.append(TextBox(
            x=0.02, y=0.95,
            text=f"action = {str(ev['action'])}   dGF = {float(ev['td2_gf_m']):+.4f} m   "
                 f"gate = {float(ev['thresh_m']):.4f} m{frac_txt}",
            ha="left", va="top", boxed=True))

    return Figure(name="TurboEdit gap-bridge adjudication (example)",
                  title=title, axes=[ax1, ax2, ax3], width=9.4, height=7.6)


def _build_gap_bridge_figs(te_path, tall, gap_factor, max_figs, summary):
    """Build up to ``max_figs`` gap-bridge figures from turbo_edit_trace.csv.

    Appends inventory lines to ``summary``.  Returns a list of Figures (possibly
    empty); degrades gracefully on missing file / schema.
    """
    if not os.path.isfile(te_path):
        summary.append("No turbo_edit_trace.csv beside the MW trace -- "
                       "skipping gap-bridge figures.")
        return []
    te = load_trace(te_path)
    need_te = ["week", "tow_s", "satellite", "sig1", "sig2", "gf_m",
               "td2_gf_m", "thresh_m", "action"]
    if any(c not in te.columns for c in need_te) or "bridged" not in te.columns:
        summary.append("turbo_edit_trace.csv schema lacks the gap-bridge columns "
                       "-- skipping gap-bridge figures.")
        return []
    for c in ("week", "tow_s", "gf_m", "td2_gf_m", "thresh_m", "bridged"):
        te[c] = pd.to_numeric(te[c], errors="coerce")
    for c in ("lambda1_m", "lambda2_m", "dN1", "dN2", "dN_wl", "frac"):
        if c in te.columns:
            te[c] = pd.to_numeric(te[c], errors="coerce")
    te["bridged"] = te["bridged"].fillna(0.0)
    te["utc"] = _utc(te)
    te["signal"] = te["sig1"].astype(str) + "/" + te["sig2"].astype(str)

    bridged_rows = te["bridged"].to_numpy(float) > 0
    adjudicated = bridged_rows & (te["action"].astype(str).to_numpy() != "none")
    if not bridged_rows.any():
        summary.append("No bridged gap-bridge rows in turbo_edit_trace -- "
                       "skipping gap-bridge figures.")
        return []

    has_ints = all(c in te.columns for c in ("dN1", "dN2", "lambda1_m", "lambda2_m"))
    ev_idx = np.flatnonzero(adjudicated)
    summary.append("")
    summary.append(f"=== TurboEdit gap-bridge adjudications: {ev_idx.size} "
                   f"(bridged rows total: {int(bridged_rows.sum())}) ===")
    for e in ev_idx:
        row = te.iloc[int(e)]
        ints_txt = ""
        if has_ints and (row["dN1"] != 0 or row["dN2"] != 0):
            ints_txt = f"  (dN1={int(row['dN1']):+d}, dN2={int(row['dN2']):+d})"
        summary.append(
            f"  {pd.Timestamp(row['utc'])}  {row['satellite']} {str(row['signal']):<9} "
            f"dGF={float(row['td2_gf_m']):+8.4f} m  gate={float(row['thresh_m']):6.4f} m  "
            f"{row['action']}{ints_txt}")

    # Exhibit anchors: actioned rows ranked repair < moot < code_event < reset,
    # then by descending |dGF|; fall back to largest un-actioned bridged jump.
    def _rank(a):
        return {"repair": 1, "moot": 2, "code_event": 3, "reset": 4}.get(str(a), 9)

    if ev_idx.size:
        keys = [(_rank(te.iloc[int(e)]["action"]),
                 -abs(float(te.iloc[int(e)]["td2_gf_m"]))) for e in ev_idx]
        order = np.argsort(np.array(keys, dtype=[("r", float), ("g", float)]),
                           order=("r", "g"))
        picks = ev_idx[order]
    else:
        b_idx = np.flatnonzero(bridged_rows)
        order = np.argsort([-abs(float(te.iloc[int(e)]["td2_gf_m"])) for e in b_idx])
        picks = b_idx[order]

    figs = []
    shown = []  # list of (sat, sig, utc)
    for p in picks:
        row = te.iloc[int(p)]
        sat, sig, utc = str(row["satellite"]), str(row["signal"]), pd.Timestamp(row["utc"])
        dup = any(s == sat and g == sig and abs((utc - u).total_seconds()) <= 30
                  for (s, g, u) in shown)
        if dup:
            continue
        fig = _gap_bridge_example(te, tall, row, has_ints, gap_factor)
        if fig is None:
            continue
        shown.append((sat, sig, utc))
        figs.append(fig)
        if len(figs) >= max_figs:
            break
    return figs


# --------------------------------------------------------------------------- #
#  Slip-event inventory + example figures
# --------------------------------------------------------------------------- #
def _build_slip_examples(tall, has_local_ref, gap_factor, summary):
    """Build the detected + repaired slip-example figures; append inventory.

    Mirrors the MATLAB ``showSlipExampleFigs`` block: the detected example is the
    largest |dev|/threshold exceedance among readable in-arc events, the repaired
    example the most confident integer among repair-eligible events.
    """
    if "is_slip" not in tall.columns:
        return []
    evt = tall[tall["is_slip"].to_numpy(float).astype(bool)].reset_index(drop=True)
    if evt.empty:
        summary.append("")
        summary.append("No detected slips in trace -- skipping example slip figures.")
        return []

    dev_evt = evt["dev_active_cycles"].to_numpy(float)
    thr = evt["threshold_cycles"].to_numpy(float)
    with np.errstate(divide="ignore", invalid="ignore"):
        ratio = np.abs(dev_evt) / thr

    in_arc = (evt["arc_count"].to_numpy(float) > 1 if "arc_count" in evt.columns
              else np.ones(len(evt), bool))
    wlock = (np.nan_to_num(evt["wl_locked"].to_numpy(float)) if "wl_locked" in evt.columns
             else np.zeros(len(evt)))
    is_backstop = (((evt["detect_mode"].to_numpy(float) == 1)
                    & (np.abs(thr - 0.4) < 1e-6)) if "detect_mode" in evt.columns
                   else np.zeros(len(evt), bool))
    frac = np.abs(dev_evt - _round_half_away(dev_evt))
    repairable = (~wlock.astype(bool) & in_arc & ~is_backstop
                  & (np.abs(dev_evt) >= 0.5) & (frac < 0.25) & np.isfinite(dev_evt))

    # Locked-arc TD confirmation action rows (collapse adjacent pairs).
    is_locked_td = wlock.astype(bool)
    if "detect_mode" in evt.columns:
        is_locked_td = is_locked_td & (evt["detect_mode"].to_numpy(float) >= 2)
    sats = evt["satellite"].astype(str).to_numpy()
    sigs = evt["signal"].astype(str).to_numpy()
    utc = evt["utc"].to_numpy()
    is_confirm_action = np.zeros(len(evt), bool)
    for e in range(len(evt)):
        if not is_locked_td[e]:
            continue
        same_arc = is_locked_td & (sats == sats[e]) & (sigs == sigs[e])
        dt = (utc[e] - utc).astype("timedelta64[ns]").astype(float) / 1e9
        if np.any(same_arc & (dt > 0) & (dt <= 2.5)):
            is_confirm_action[e] = True

    summary.append("")
    summary.append(f"=== Detected MW slip events: {len(evt) - int(is_confirm_action.sum())} "
                   f"(repair-eligible: {int(repairable.sum())}) ===")
    n_show = min(len(evt), 30)
    for e in range(n_show):
        tags = ""
        if not in_arc[e]:
            tags += " init"
        if is_backstop[e]:
            tags += " backstop"
        if is_confirm_action[e]:
            tags += " confirm-action"
        if repairable[e]:
            tags += f" dN={int(_round_half_away(dev_evt[e])):+d} (frac {frac[e]:.3f})"
        summary.append(
            f"  {pd.Timestamp(utc[e])}  {sats[e]} {sigs[e]:<9} "
            f"dev={dev_evt[e]:+9.3f} cyc  thresh={thr[e]:7.3f} cyc{tags}")
    if len(evt) > n_show:
        summary.append(f"  ... ({len(evt) - n_show} more)")

    figs = []
    # Detected example: preference cascade.
    cand = np.flatnonzero(in_arc & (np.abs(dev_evt) <= 10) & np.isfinite(ratio))
    if cand.size == 0:
        cand = np.flatnonzero(in_arc & np.isfinite(ratio))
    if cand.size == 0:
        cand = np.flatnonzero(np.isfinite(ratio))
    if cand.size:
        k = cand[int(np.argmax(ratio[cand]))]
        fig = _slip_example(tall, evt.iloc[int(k)],
                            "Detected MW cycle slip (example)", float("nan"),
                            has_local_ref, gap_factor)
        if fig is not None:
            figs.append(fig)

    # Repaired example: most confident integer.
    cand_r = np.flatnonzero(repairable)
    if cand_r.size == 0:
        summary.append("No repair-eligible slips (known-integer gate) in trace -- "
                       "skipping repaired-slip example figure.")
    else:
        k = cand_r[int(np.argmin(frac[cand_r]))]
        fig = _slip_example(tall, evt.iloc[int(k)],
                            "Repaired MW cycle slip (example)",
                            float(_round_half_away(dev_evt[k])),
                            has_local_ref, gap_factor)
        if fig is not None:
            figs.append(fig)
    return figs


# --------------------------------------------------------------------------- #
#  Build entry point
# --------------------------------------------------------------------------- #
def build_mwwl_trace(
    ctx,
    plot_style: str = "lines",
    gap_factor: float = 3.0,
    show_raw_mw_plot: bool = False,
    show_zscore_plot: bool = False,
    show_threshold_ratio_plot: bool = False,
    show_slip_example_figs: bool = True,
    show_gap_bridge_figs: bool = True,
    max_gap_bridge_figs: int = 2,
    show_legend: bool = False,
    hide_slip_epochs: bool = True,
) -> ViewerResult:
    """Build the MW WL diagnostic figures + summary (defaults match the MATLAB).

    Keyword arguments mirror the MATLAB top-of-script switches: ``plot_style``
    ('lines' | 'scatter'), ``gap_factor`` (NaN-break threshold), the
    ``show_*_plot`` content toggles (z-score / threshold-ratio / raw-MW grids,
    all off by default as in the MATLAB), ``show_slip_example_figs`` /
    ``show_gap_bridge_figs`` (on), ``max_gap_bridge_figs``, ``show_legend``, and
    ``hide_slip_epochs`` (drop detector-flagged rows from the grid figures /
    summary).
    """
    raw = load_trace(ctx.path("mwwl_trace.csv"))
    require_cols(raw, _REQUIRED, "mwwl_trace.csv")
    df, has_local_ref, has_active_dev = _prepare(raw)

    summary: list = []

    if has_local_ref:
        mode_names = ["hatch", "local", "td1", "td2"]
        dm = df["detect_mode"].to_numpy(float)
        parts = [f"{mode_names[m]}={int(np.sum(dm == m))}"
                 for m in range(4) if np.sum(dm == m) > 0]
        summary.append(f"Detection-mode rows: {', '.join(parts)} "
                       f"(active_dev column: {int(has_active_dev)}).")

    # Full snapshot feeds the slip-example figures; the grid / summary use the
    # (optionally) slip-filtered view.
    tall = df
    if hide_slip_epochs and "is_slip" in df.columns:
        slip_mask = df["is_slip"].to_numpy(float).astype(bool)
        n_dropped = int(slip_mask.sum())
        if n_dropped > 0:
            df = df[~slip_mask].reset_index(drop=True)
            summary.append(f"Dropped {n_dropped} slip-flagged row(s) from "
                           f"mwwl_trace (toggle hide_slip_epochs=False to keep them).")

    systems_present = _systems_present(df)
    if not systems_present:
        return ViewerResult(figures=[], summary="No usable constellations in mwwl_trace.csv")

    figures = []

    # Primary grid: deviation from arc mean (zero reference line).
    fig = _build_grid(df, "dev_from_mean_cycles", "Res. (cyc.)",
                      "MW WL Deviation by Signal Pair and Constellation",
                      systems_present, plot_style, gap_factor, show_legend, [0])
    if fig is not None:
        figures.append(fig)

    if show_zscore_plot:
        fig = _build_grid(df, "mw_zscore", "dev / sigma_arc",
                          "MW WL z-score by Signal Pair and Constellation",
                          systems_present, plot_style, gap_factor, show_legend, [0])
        if fig is not None:
            figures.append(fig)

    if show_threshold_ratio_plot:
        fig = _build_grid(df, "mw_threshold_ratio", "dev / thresh_C++",
                          "MW WL Detector Gate (dev / threshold) by Signal Pair and Constellation",
                          systems_present, plot_style, gap_factor, show_legend, [-1, 1])
        if fig is not None:
            figures.append(fig)

    if show_raw_mw_plot:
        fig = _build_grid(df, "mw_cycles", "MW WL (cycles)",
                          "MW WL Raw by Signal Pair and Constellation",
                          systems_present, plot_style, gap_factor, show_legend, [])
        if fig is not None:
            figures.append(fig)

    # Local-median deviation grid (June 2026 schema only).
    if has_local_ref:
        fig = _build_grid(df, "dev_local_cycles", "Res._local (cyc.)",
                          "MW WL Local-Median Deviation by Signal Pair and Constellation",
                          systems_present, plot_style, gap_factor, show_legend, [0])
        if fig is not None:
            figures.append(fig)

    # Slip-example figures (from the full snapshot).
    if show_slip_example_figs:
        figures.extend(_build_slip_examples(tall, has_local_ref, gap_factor, summary))

    # Gap-bridge figures (join with turbo_edit_trace.csv).
    if show_gap_bridge_figs:
        te_path = os.path.join(os.path.dirname(ctx.path("mwwl_trace.csv")),
                               "turbo_edit_trace.csv")
        figures.extend(_build_gap_bridge_figs(te_path, tall, gap_factor,
                                              max_gap_bridge_figs, summary))

    # Summary statistics by system + signal pair (on the slip-filtered view).
    summary.append("")
    summary.append("=== MW WL Statistics by System and Signal Pair ===")
    summary.append("")
    for sys_name in systems_present:
        sys_rows = df["system"].astype(str) == sys_name
        sig_list = sorted(df.loc[sys_rows, "signal"].astype(str).unique())
        for sig in sig_list:
            idx = sys_rows & (df["signal"].astype(str) == sig)
            if not idx.any():
                continue
            dev = df.loc[idx, "dev_from_mean_cycles"].to_numpy(float)
            dev = dev[np.isfinite(dev)]
            if dev.size == 0:
                continue
            mu_abs = float(np.mean(np.abs(dev)))
            _, sd = _mu_sigma(dev)
            mx = float(np.max(np.abs(dev)))
            summary.append(f"{sys_name} {sig}: N={dev.size}, |dev| mean={mu_abs:.3f}, "
                           f"std={sd:.3f}, max={mx:.3f} cycles")

    return ViewerResult(figures=figures, summary="\n".join(summary))


viewer = Viewer(name="mwwl_trace", title="MW wide-lane slip detector",
                required_csvs=("mwwl_trace.csv",), build=build_mwwl_trace)

"""Cycle-slip detector *gate* strip viewer, the port of
``view_slip_gate_strip.m``.

(satellite x epoch) raster charts of the cycle-slip detector gate: the
deviation, the adaptive threshold it is tested against, or their ratio, shown
across the whole constellation at once.  Complements the per-arc line plots in
``view_mwwl_trace.m`` / ``view_turbo_edit_trace.m``: the raster reveals whether a
near-gate epoch is one satellite or a constellation-wide event, and where each
detector runs hot.  Built on the shared :func:`gnssviz.strip.build_sat_epoch_strip`
renderer: one tile per signal pair (e.g. L1C/L2W); slip-flag epochs overlaid as
glyphs.

``source`` selects the trace and statistic:
  * ``'mw'`` -> ``mwwl_trace.csv``      (Melbourne-Wubbena wide-lane), default;
  * ``'gf'`` -> ``turbo_edit_trace.csv`` (TurboEdit geometry-free TD2).

``plot_value`` selects the rendered quantity (default ``'threshold'`` matching
the MATLAB script's ``plotValue`` default):
  * ``'ratio'``     -> deviation / threshold  (|y|>1 == over gate), vik diverging
                       with reference ticks at [-cap, -1, 0, 1, cap];
  * ``'deviation'`` -> the raw statistic tested (cyc for MW, m for GF), vik autosym;
  * ``'threshold'`` -> the adaptive threshold itself, viridis, auto limits.

Markers: detector-flagged slip epochs.  For MW, slip (circle) versus LLI-only
(square); for GF, the action that touched the state: repair (circle) or reset
(magenta x).
"""

from __future__ import annotations

import numpy as np
import pandas as pd

from ..io import gps_to_datetime, load_trace
from ..strip import build_sat_epoch_strip
from .base import Viewer, ViewerResult


def _get_num(df: pd.DataFrame, col: str) -> np.ndarray:
    """Numeric column fetch with missing-column tolerance (MATLAB ``getNum``)."""
    if col not in df.columns:
        return np.full(len(df), np.nan)
    return pd.to_numeric(df[col], errors="coerce").to_numpy(dtype=float)


def build_slip_gate_strip(
    ctx,
    source: str = "mw",
    plot_value: str = "threshold",
    ratio_cap: float = 3.0,
    show_slip_markers: bool = True,
) -> ViewerResult:
    src = source.lower()

    # --- Load + melt the chosen trace -------------------------------------
    if src == "mw":
        df = load_trace(ctx.path("mwwl_trace.csv"))
        facet = df["sig1"].astype(str) + "/" + df["sig2"].astype(str)

        # Active deviation the detector gated; fall back to dev_from_mean on
        # init/LLI rows that carry no active_dev.
        dev = _get_num(df, "active_dev_cycles")
        dev_fm = _get_num(df, "dev_from_mean_cycles")
        if np.all(np.isnan(dev)):
            dev = dev_fm.copy()
        nanrows = ~np.isfinite(dev)
        dev[nanrows] = dev_fm[nanrows]
        thr = _get_num(df, "threshold_cycles")

        dev_lbl = "MW active dev (cyc)"
        thr_lbl = "MW threshold (cyc)"
        rat_lbl = "MW dev / threshold"

        # Markers: detector-flagged slip epochs (LLI distinguished).
        is_slip = _get_num(df, "is_slip") == 1
        is_lli = _get_num(df, "is_lli") == 1
        mk_kind = np.full(len(df), "", dtype=object)
        mk_kind[is_slip] = "slip"
        mk_kind[is_lli & ~is_slip] = "lli"
        mk_mask = mk_kind != ""
    elif src == "gf":
        df = load_trace(ctx.path("turbo_edit_trace.csv"))
        facet = df["sig1"].astype(str) + "/" + df["sig2"].astype(str)

        dev = _get_num(df, "td2_gf_m")
        thr = _get_num(df, "thresh_m")
        thr[thr < 0] = np.nan  # -1 sentinel = immature

        dev_lbl = "GF TD2 (m)"
        thr_lbl = "GF threshold (m)"
        rat_lbl = "GF TD2 / threshold"

        # Markers: any flagged event that touched the state (action != none).
        flagged = (_get_num(df, "mw_flagged") == 1) | (_get_num(df, "gf_flagged") == 1)
        act = df["action"].astype(str) if "action" in df.columns else pd.Series(
            ["none"] * len(df))
        act = act.to_numpy()
        flagged = flagged & (act != "none")
        mk_kind = np.full(len(df), "", dtype=object)
        mk_kind[flagged] = act[flagged]  # "repair" / "reset"
        mk_mask = flagged
    else:
        raise ValueError("source must be 'mw' or 'gf'")

    # --- Resolve plot_value -> value column + render options --------------
    pv = plot_value.lower()
    if pv == "deviation":
        value = dev
        val_lbl = dev_lbl
        cmap_name = "vik"
        cax = "autosym"
        divr = True
        ref_ticks = None
    elif pv == "threshold":
        value = thr
        val_lbl = thr_lbl
        cmap_name = "viridis"
        cax = "auto"
        divr = False
        ref_ticks = None
    elif pv == "ratio":
        with np.errstate(divide="ignore", invalid="ignore"):
            value = dev / thr
        val_lbl = rat_lbl
        cmap_name = "vik"
        cax = [-ratio_cap, ratio_cap]
        divr = True
        ref_ticks = [-ratio_cap, -1, 0, 1, ratio_cap]
    else:
        raise ValueError("plot_value must be 'ratio', 'deviation', or 'threshold'")

    # --- Long-form table for the renderer ---------------------------------
    utc = gps_to_datetime(df["week"], df["tow_s"])
    long = pd.DataFrame({
        "system": df["system"].astype(str),
        "satellite": df["satellite"].astype(str),
        "facet": facet,
        "utc": utc,
        "value": np.asarray(value, dtype=float),
    })
    long = long[np.isfinite(long["value"].to_numpy(dtype=float))].reset_index(drop=True)

    # --- Markers table ----------------------------------------------------
    markers = None
    if show_slip_markers and mk_mask.any():
        markers = pd.DataFrame({
            "system": df["system"].astype(str).to_numpy()[mk_mask],
            "satellite": df["satellite"].astype(str).to_numpy()[mk_mask],
            "facet": facet.to_numpy()[mk_mask],
            "utc": pd.Series(utc).to_numpy()[mk_mask],
            "kind": mk_kind[mk_mask].astype(str),
        })

    marker_styles = {
        "slip": {"marker": "o", "color": (0, 0, 0), "size": 6},
        "lli": {"marker": "s", "color": (0.2, 0.2, 0.2), "size": 6},
        "repair": {"marker": "o", "color": (0, 0, 0), "size": 6},
        "reset": {"marker": "x", "color": (0.85, 0, 0.85), "size": 8},
    }

    fig_title = f"{src.upper()} slip-detector gate ({pv})"
    figs = build_sat_epoch_strip(
        long,
        mode="continuous",
        colormap=cmap_name,
        diverging=divr,
        caxis=cax,
        ref_ticks=ref_ticks,
        fig_title=fig_title,
        value_label=val_lbl,
        subtitle="tile = signal pair    markers = detector-flagged slip epochs",
        fig_tag=f"{src}-gate",
        markers=markers,
        marker_styles=marker_styles,
    )
    return ViewerResult(figures=figs)


viewer = Viewer(name="slip_gate_strip", title="Slip-detector gate strip",
                required_csvs=("mwwl_trace.csv",),
                build=build_slip_gate_strip)

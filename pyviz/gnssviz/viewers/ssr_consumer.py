"""Consumer SSR-application viewer -- what the user engine did with the products.

Reads ``ssr_consumer_trace.csv`` (the PPP-RTK USER consumer's per-epoch, per-
satellite record of the SSR corrections it actually applied: clock age, applied
clock/URA, R-inflation, phase-bias age/freshness, and datum-discontinuity
counters) and shows, over the run:

  1. **Correction age** -- a per-sat ``ssr_age_s`` strip (how stale each applied
     satellite correction was) plus per-epoch max/median age.  A growing age is
     the tell-tale of a stalling / dropped product feed.  The freshness LIMIT is
     a *consumer config* knob (``processing.ssr_max_age_s``, default 30 s) and is
     NOT in the trace, so any reference line is annotated "limit unknown".
  2. **URA R-inflation** -- the applied measurement-noise inflation
     ``sqrt(r_add_var_m2)`` (m) per satellite, one constellation per tile: how
     much the consumer down-weighted each satellite for product uncertainty.
  3. **Events + gaps** -- per-epoch step counts of product gaps
     (``clk_applied==0``), stale-hold flips (``pb_fresh`` 1->0), and datum
     discontinuity resets (``clk_disc`` / ``pb_disc_max`` increments).  When a
     ``closed_loop_trace.csv`` is present the held-out user 3D position error is
     overlaid (log-y) with the product-gap epochs shaded, so a position bump can
     be read against the gap that caused it.

This is the "did the consumer trust, weight, and time-align the products
correctly?" view -- the mirror image of the producer's ``network_products``.
"""

from __future__ import annotations

from typing import List

import numpy as np
import pandas as pd

from ..figures import Axes, Band, Figure, HLine, Legend, Line
from ..io import add_utc, load_trace, require_cols
from ..palettes import color_by_prn
from ..strip import build_sat_epoch_strip
from .base import Viewer, ViewerResult, ViewerSkip
from .network_products import _by_system, _SYS_OF, _SYS_ORDER

# Documented default for the (out-of-trace) freshness limit; used only to LABEL a
# reference line -- never to gate/threshold data.
_SSR_MAX_AGE_DEFAULT_S = 30.0
_MAX_STRIP_COLS = 2000

_C_AGE_MAX = (0.85, 0.30, 0.10)
_C_AGE_MED = (0.13, 0.40, 0.70)
_C_GAP = (0.85, 0.16, 0.16)
_C_STALE = (0.85, 0.55, 0.10)
_C_DISC = (0.45, 0.30, 0.65)
_C_POS = (0.10, 0.10, 0.10)
_GAP_SHADE = (0.85, 0.16, 0.16)


def build_ssr_consumer(ctx) -> ViewerResult:
    df = load_trace(ctx.path("ssr_consumer_trace.csv"))
    require_cols(df, ("week", "tow", "sat", "ssr_age_s", "clk_applied", "c0_m",
                      "ura_m", "r_add_var_m2", "pb_age_s", "n_pb", "clk_disc",
                      "pb_disc_max", "cb_disc_max", "pb_fresh"),
                 "ssr_consumer_trace.csv")
    if df.empty:
        raise ViewerSkip("ssr_consumer_trace.csv has no rows")

    df = df.copy()
    df["sat"] = df["sat"].astype(str)
    df["system"] = df["sat"].map(lambda s: _SYS_OF.get(s[:1], "Unknown"))
    df = add_utc(df)

    figures: List[Figure] = []
    figures.extend(_age_figures(df))       # per-epoch line + one strip per system
    ura_fig = _ura_figure(df)
    if ura_fig is not None:
        figures.append(ura_fig)

    ev_fig, gap_stats, cl_causality = _events_figure(ctx, df)
    figures.append(ev_fig)

    return ViewerResult(figures=figures,
                        summary=_summary(df, gap_stats, cl_causality))


# --------------------------------------------------------------------------- #
#  (1) Correction age -- per-sat strip + per-epoch max/median
# --------------------------------------------------------------------------- #
def _age_figures(df) -> List[Figure]:
    # Long-form for the shared sat x epoch strip renderer.
    D = pd.DataFrame({
        "system": df["system"],
        "satellite": df["sat"],
        "facet": "",
        "utc": df["utc"],
        "value": df["ssr_age_s"].astype(float),
    })

    # Roll our OWN column decimation: the strip renders one column per unique
    # epoch, so thin the epoch set (not the rows) to <= _MAX_STRIP_COLS by an
    # even stride.  No decimation helper exists in pyviz.
    epochs = np.sort(pd.unique(D["utc"]))
    if epochs.size > _MAX_STRIP_COLS:
        stride = int(np.ceil(epochs.size / _MAX_STRIP_COLS))
        keep = set(epochs[::stride].tolist())
        D = D[D["utc"].isin(keep)]

    strip_figs = build_sat_epoch_strip(
        D, colormap="viridis", diverging=False, caxis="auto",
        fig_title="Applied SSR correction age", value_label="SSR age (s)",
        fig_tag="ssr-age",
        subtitle="per-sat applied clock-correction latency")

    # Per-epoch max / median applied age (over sats with a correction applied).
    g = df.copy()
    g["ssr_age_s"] = g["ssr_age_s"].astype(float)
    per = g.groupby("utc")["ssr_age_s"]
    t = np.sort(pd.unique(g["utc"])).astype("datetime64[ns]")
    age_max = per.max().reindex(pd.Index(t, name="utc")).to_numpy(float)
    age_med = per.median().reindex(pd.Index(t, name="utc")).to_numpy(float)

    ax = Axes(title="Applied SSR correction age per epoch", ylabel="age (s)",
              xlabel="GPS Time", legend=Legend(loc="best"))
    ax.traces = [
        Line(x=t, y=age_max, color=_C_AGE_MAX, width=1.2, label="max age"),
        Line(x=t, y=age_med, color=_C_AGE_MED, width=1.0, label="median age"),
        HLine(y=_SSR_MAX_AGE_DEFAULT_S, color=(0.6, 0.6, 0.6), style="--",
              label=f"limit unknown (config: processing.ssr_max_age_s "
                    f"default {int(_SSR_MAX_AGE_DEFAULT_S)} s)"),
    ]
    line_fig = Figure(name="SSR age (per-epoch)",
                      title="Applied SSR correction age -- per-epoch envelope",
                      axes=[ax], width=12.0, height=4.0, link_x=True)

    # Per-epoch line figure first, then the (possibly several) per-system strips.
    return [line_fig] + list(strip_figs)


# --------------------------------------------------------------------------- #
#  (2) URA R-inflation -- per-sat sqrt(r_add_var_m2) per constellation tile
# --------------------------------------------------------------------------- #
def _ura_figure(df):
    d = df.copy()
    d["value_m"] = np.sqrt(d["r_add_var_m2"].astype(float))
    d = d[np.isfinite(d["value_m"].to_numpy(float))]
    if d.empty:
        return None
    sys_present = [s for s in _SYS_ORDER if (d["system"] == s).any()]
    if not sys_present:
        return None
    # _by_system draws one line per key (key_sat -> one per satellite).
    fig = _by_system(d, color_by_prn(d["sat"].tolist()), sys_present, "key_sat",
                     "sqrt(R add) (m)",
                     "Applied URA R-inflation (per satellite)",
                     "SSR URA R-inflation")
    return fig


# --------------------------------------------------------------------------- #
#  (3) Events + gaps -- per-epoch step counts, optional closed-loop overlay
# --------------------------------------------------------------------------- #
def _events_figure(ctx, df):
    g = df.copy()
    g["clk_applied"] = g["clk_applied"].astype(float)
    g["pb_fresh"] = g["pb_fresh"].astype(float)
    for c in ("clk_disc", "pb_disc_max"):
        g[c] = g[c].astype(float)

    t = np.sort(pd.unique(g["utc"])).astype("datetime64[ns]")
    idx = pd.Index(t, name="utc")

    # Product gap: sats with NO clock applied, per epoch.
    gap_cnt = (g.assign(gap=(g["clk_applied"] <= 0).astype(int))
               .groupby("utc")["gap"].sum().reindex(idx).to_numpy(float))

    # Stale-hold flips: per-sat pb_fresh 1 -> 0 transitions, counted per epoch.
    stale_cnt = _flip_counts(g, "pb_fresh", idx, direction="down")

    # Discontinuity resets: per-sat clk_disc / pb_disc_max increments per epoch.
    disc_cnt = (_incr_counts(g, "clk_disc", idx)
                + _incr_counts(g, "pb_disc_max", idx))

    # Detect the (optional) closed-loop overlay up front: when present, the held-out
    # 3D error owns the LEFT axis on a LOG scale (the model has a single yscale that
    # maps to the left axis; there is no right-axis log), and the per-epoch counts
    # move to the RIGHT (linear) axis.  Without the overlay, counts stay on the left.
    ct = err = None
    have_overlay = False
    if ctx.has_data("closed_loop_trace.csv"):
        cl = load_trace(ctx.path("closed_loop_trace.csv"))
        if {"week", "pos_err_3d_m"}.issubset(cl.columns) and \
                ("tow" in cl.columns or "tow_s" in cl.columns) and not cl.empty:
            cl = add_utc(cl, sort=True)
            ct = cl["utc"].to_numpy("datetime64[ns]")
            err = cl["pos_err_3d_m"].to_numpy(float)
            have_overlay = True

    count_axis = "right" if have_overlay else "left"
    ax = Axes(title="Consumer SSR events -- product gaps / stale holds / disc resets",
              ylabel=("held-out 3D error (m)" if have_overlay else "count per epoch"),
              y2label=("count per epoch" if have_overlay else ""),
              xlabel="GPS Time", legend=Legend(loc="best"),
              yscale=("log" if have_overlay else "linear"))
    traces = [
        Line(x=t, y=gap_cnt, color=_C_GAP, width=1.2, style="-", axis=count_axis,
             label="product gap (no clk applied)"),
        Line(x=t, y=stale_cnt, color=_C_STALE, width=1.0, axis=count_axis,
             label="stale-hold flips (pb_fresh 1->0)"),
        Line(x=t, y=disc_cnt, color=_C_DISC, width=1.0, axis=count_axis,
             label="disc resets (clk/pb)"),
    ]

    cl_causality = None
    if have_overlay:
        # log-y needs a positive floor; err on the LEFT (log) axis.
        err_pos = np.where(np.isfinite(err) & (err > 0), err, np.nan)
        traces.insert(0, Line(x=ct, y=err_pos, color=_C_POS, width=1.2,
                              label="held-out 3D err (m)"))
        # Gap-epoch shading via a Band that is NaN outside gap epochs (no VSpan
        # primitive exists).  The Band renders on the LEFT (log) axis, so span the
        # error range at gap epochs (positive lo for the log scale); an explicit
        # ylim keeps the fill from blowing up the autoscale.
        finite = err_pos[np.isfinite(err_pos)]
        e_lo = float(np.min(finite)) if finite.size else 1e-2
        e_hi = float(np.max(finite)) if finite.size else 1.0
        floor = max(e_lo * 0.5, 1e-3)
        ceil = max(e_hi * 2.0, floor * 10.0)
        lo, hi = _gap_band(t, gap_cnt, floor, ceil)
        if lo is not None:
            traces.append(Band(x=t, lo=lo, hi=hi, color=_GAP_SHADE, alpha=0.18,
                               label="product-gap epochs", show_in_legend=True))
            ax.ylim = (floor, ceil)
        cl_causality = _causality(t, gap_cnt, ct, err)

    ax.traces = traces
    fig = Figure(name="Consumer SSR events",
                 title="Consumer SSR application events & product gaps",
                 axes=[ax], width=12.0, height=4.5, link_x=True)

    gap_stats = {
        "gap_epochs": int((gap_cnt > 0).sum()),
        "stale_flips": int(np.nansum(stale_cnt)),
        "disc_resets": int(np.nansum(disc_cnt)),
    }
    return fig, gap_stats, cl_causality


def _flip_counts(g, col, idx, direction="down") -> np.ndarray:
    """Per-epoch count of per-sat 1->0 (down) or 0->1 (up) transitions in ``col``."""
    counts = pd.Series(0, index=idx, dtype=float)
    for _, sub in g.sort_values(["sat", "utc"]).groupby("sat", sort=False):
        v = sub[col].to_numpy(float)
        if v.size < 2:
            continue
        prev, cur = v[:-1], v[1:]
        if direction == "down":
            flip = (prev > 0) & (cur <= 0)
        else:
            flip = (prev <= 0) & (cur > 0)
        ep = sub["utc"].to_numpy("datetime64[ns]")[1:][flip]
        for e in ep:
            counts.loc[e] += 1.0
    return counts.reindex(idx).to_numpy(float)


def _incr_counts(g, col, idx) -> np.ndarray:
    """Per-epoch count of per-sat increments (value strictly rises) in ``col``.

    Blank/NaN cells never count as an increment (a NaN comparison is False).
    """
    counts = pd.Series(0, index=idx, dtype=float)
    for _, sub in g.sort_values(["sat", "utc"]).groupby("sat", sort=False):
        v = sub[col].to_numpy(float)
        if v.size < 2:
            continue
        rose = v[1:] > v[:-1]            # NaN comparisons -> False
        ep = sub["utc"].to_numpy("datetime64[ns]")[1:][rose]
        for e in ep:
            counts.loc[e] += 1.0
    return counts.reindex(idx).to_numpy(float)


def _gap_band(t, gap_cnt, lo_val, hi_val):
    """Build (lo, hi) arrays that are finite ONLY on gap epochs (else NaN).

    A ``Band`` breaks into segments at NaN, so this shades exactly the gap
    epochs.  The band spans ``lo_val..hi_val`` (both positive for the log axis).
    """
    is_gap = gap_cnt > 0
    if not is_gap.any():
        return None, None
    lo = np.full(t.size, np.nan)
    hi = np.full(t.size, np.nan)
    lo[is_gap] = lo_val
    hi[is_gap] = hi_val
    return lo, hi


def _causality(t, gap_cnt, ct, err):
    """Median held-out 3D error INSIDE vs OUTSIDE gap-shaded epochs.

    Closed-loop epochs are matched to the consumer epoch set by exact timestamp;
    an epoch-set mismatch is tolerated (unmatched closed-loop epochs are counted
    as OUTSIDE, since only consumer-known gaps define the shaded windows).
    """
    gap_set = set(t[gap_cnt > 0].astype("datetime64[ns]").astype("int64").tolist())
    ck = ct.astype("datetime64[ns]").astype("int64")
    inside_mask = np.array([k in gap_set for k in ck], dtype=bool)
    fin = np.isfinite(err)
    ins = err[inside_mask & fin]
    out = err[(~inside_mask) & fin]
    return {
        "n_inside": int(ins.size),
        "n_outside": int(out.size),
        "median_inside": float(np.median(ins)) if ins.size else float("nan"),
        "median_outside": float(np.median(out)) if out.size else float("nan"),
    }


# --------------------------------------------------------------------------- #
#  Summary
# --------------------------------------------------------------------------- #
def _summary(df, gap_stats, cl) -> str:
    out = ["=== Consumer SSR Application ==="]
    out.append(f"Epochs         : {df['utc'].nunique()} "
               f"({df['utc'].min()} .. {df['utc'].max()})")
    out.append(f"Sat-rows       : {len(df)}  ({df['sat'].nunique()} sats)")

    age = df["ssr_age_s"].to_numpy(float)
    age = age[np.isfinite(age)]
    if age.size:
        out.append(f"SSR age (s)    : median {np.median(age):.1f}, "
                   f"std {_std(age):.1f}, max {np.max(age):.1f}")

    # Applied-URA stat: median + std + worst-sat NAME (convention: never rms).
    ura = np.sqrt(df["r_add_var_m2"].astype(float))
    m = np.isfinite(ura.to_numpy(float))
    if m.any():
        u = ura[m]
        per_sat = df.loc[m].assign(u=u).groupby("sat")["u"].median()
        worst_sat = per_sat.idxmax()
        out.append(f"URA R-infl (m) : median {np.median(u):.3f}, "
                   f"std {_std(u.to_numpy(float)):.3f}, "
                   f"worst-sat {worst_sat} ({per_sat.max():.3f})")

    out.append(f"Product gaps   : {gap_stats['gap_epochs']} epoch(s) with a "
               f"withheld clock")
    out.append(f"Stale-hold flip: {gap_stats['stale_flips']} (pb_fresh 1->0)")
    out.append(f"Disc resets    : {gap_stats['disc_resets']} (clk/pb datum re-ref)")

    if cl is not None:
        mi, mo = cl["median_inside"], cl["median_outside"]
        out.append("")
        out.append("--- Closed-loop causality (3D error, m) ---")
        out.append(f"  INSIDE  gap epochs (n={cl['n_inside']}) : "
                   f"{_fmt(mi)}")
        out.append(f"  OUTSIDE gap epochs (n={cl['n_outside']}) : "
                   f"{_fmt(mo)}")
        if np.isfinite(mi) and np.isfinite(mo):
            out.append(f"  inside/outside ratio             : {mi / mo:.2f}x"
                       if mo > 0 else "  inside/outside ratio             : n/a")
    return "\n".join(out)


def _fmt(x) -> str:
    return f"{x:.3f}" if np.isfinite(x) else "n/a"


def _std(v) -> float:
    v = np.asarray(v, float)
    v = v[np.isfinite(v)]
    return float(np.std(v, ddof=1)) if v.size > 1 else 0.0


viewer = Viewer(name="ssr_consumer",
                title="Consumer SSR application (age / URA / events)",
                required_csvs=("ssr_consumer_trace.csv",),
                build=build_ssr_consumer)

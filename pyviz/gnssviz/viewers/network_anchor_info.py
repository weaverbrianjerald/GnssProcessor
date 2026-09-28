"""Network per-sat anchor / carry / data information viewer: the lock-in probe.

Reads ``network_anchor_info.csv`` (the network producer's per-epoch, per-sat
decomposition of the satellite-clock information into its **anchor** (datum pin),
**carry** (prior carried across epochs), and **data** (fresh measurement)
contributions).  When the carried prior comes to dominate a satellite, with the
carry fraction pinned near 1 while the fresh-data fraction starves, that clock
has locked in: it stops responding to new observations, the signature tracked
in the armed-validation runs.

Columns
-------
``week, tow, sat, anchor_info, carry_info, data_info, total_info,
anchor_frac_data, carry_frac, frac, is_pivot``

The pivot satellite carries the datum pin, which inflates its ``data_info`` /
``anchor_frac_data``.  Rows with ``is_pivot == 1`` are therefore excluded from
the strips, the worst-sat pick, and every stat (the exclusion is noted in the
summary and figure subtitles).

Figures
-------
* **(a)** ``carry_frac`` strip (viridis, caxis 0..1): how much of each sat's
  information is carried prior each epoch.  Sustained bright rows = lock-in.
* **(b)** ``anchor_frac_data`` strip (viridis, caxis 0..0.5) with an advisory
  0.1 :class:`HLine` on the colorbar-adjacent note -- anchor share of the
  data-space information.
* **(c)** a :class:`StackedArea` of data / carry / anchor info for the
  run-worst satellite (highest sustained carry_frac), plus cross-sat median
  :class:`Line` s of each component with a p10-p90 :class:`Band`.

Summary lists any satellite whose ``carry_frac`` stays > 0.9 for >= 10 epochs --
the lock-in roster.
"""

from __future__ import annotations

from typing import List

import numpy as np
import pandas as pd

from ..figures import (Axes, Band, Figure, HLine, Legend, Line, StackedArea,
                       TextBox)
from ..identifiers import system_of
from ..io import add_utc, load_trace, require_cols
from ..strip import build_sat_epoch_strip
from .base import Viewer, ViewerResult, ViewerSkip

_LOCK_CARRY = 0.9        # carry_frac threshold for the lock-in signature
_LOCK_EPOCHS = 10        # sustained-epoch count for the lock-in signature
_ANCHOR_ADVISORY = 0.1   # anchor_frac_data advisory level

_C_DATA = (0.20, 0.60, 0.25)     # green  -- fresh data
_C_CARRY = (0.85, 0.45, 0.10)    # orange -- carried prior
_C_ANCHOR = (0.55, 0.30, 0.65)   # purple -- datum anchor


def build_network_anchor_info(ctx) -> ViewerResult:
    # The driver normally guards via required_csvs, but keep the build fn safe to
    # call directly (and skip gracefully on a header-only / missing trace).
    if not ctx.has_data("network_anchor_info.csv"):
        raise ViewerSkip("network_anchor_info.csv absent or empty")
    df = load_trace(ctx.path("network_anchor_info.csv"))
    require_cols(df, ("week", "tow", "sat", "anchor_info", "carry_info",
                      "data_info", "total_info", "anchor_frac_data",
                      "carry_frac", "frac", "is_pivot"),
                 "network_anchor_info.csv")
    if df.empty:
        raise ViewerSkip("network_anchor_info.csv has no rows")

    df = df.copy()
    df["sat"] = df["sat"].astype(str)
    for c in ("anchor_info", "carry_info", "data_info", "total_info",
              "anchor_frac_data", "carry_frac", "frac", "is_pivot"):
        df[c] = df[c].astype(float)
    df = add_utc(df)

    # Exclude pivot rows from every stat: the datum pin inflates data_info.
    n_pivot_rows = int((df["is_pivot"] > 0.5).sum())
    df = df[df["is_pivot"] <= 0.5]
    if df.empty:
        raise ViewerSkip("network_anchor_info.csv: only pivot rows present")

    df["system"] = df["sat"].map(system_of)

    figures: List[Figure] = []
    figures += _strip(df, "carry_frac", caxis=(0.0, 1.0),
                      title="Carry fraction (prior share) — sustained bright = lock-in",
                      value_label="carry_frac", tag="dd_carry_frac")
    figures += _strip(df, "anchor_frac_data", caxis=(0.0, 0.5),
                      title=f"Anchor fraction of data info (advisory {_ANCHOR_ADVISORY})",
                      value_label="anchor_frac_data", tag="dd_anchor_frac",
                      advisory=_ANCHOR_ADVISORY)
    figures.append(_worst_sat_figure(df))

    return ViewerResult(figures=figures,
                        summary=_summary(df, n_pivot_rows))


# --------------------------------------------------------------------------- #
def _strip(df, col, *, caxis, title, value_label, tag, advisory=None):
    """Build per-constellation strip figures for one fractional column."""
    D = pd.DataFrame({
        "system": df["system"].to_numpy(),
        "satellite": df["sat"].to_numpy(),
        "facet": "",
        "utc": df["utc"].to_numpy("datetime64[ns]"),
        "value": df[col].to_numpy(float),
    })
    figs = build_sat_epoch_strip(
        D, colormap="viridis", diverging=False, caxis=caxis,
        annotate=True, fig_title=title, value_label=value_label, fig_tag=tag,
        subtitle="pivot rows excluded",
    )
    if advisory is not None:
        for f in figs:
            for ax in f.axes:
                ax.traces.append(HLine(y=advisory, color=(0.85, 0.16, 0.16),
                                       style="--",
                                       label=f"advisory {advisory:g}"))
    return figs


def _worst_sat_figure(df) -> Figure:
    """StackedArea for the run-worst sat + cross-sat median lines w/ p10-p90 band."""
    worst = _worst_sat(df)
    epochs = np.sort(np.unique(df["utc"].to_numpy("datetime64[ns]")))

    # --- Axes 1: stacked data / carry / anchor info for the worst sat ---
    ws = df[df["sat"] == worst].sort_values("utc")
    wt = ws["utc"].to_numpy("datetime64[ns]")
    ax_stack = Axes(
        title=f"Info decomposition for run-worst sat {worst}",
        xlabel="GPS Time", ylabel="information", grid=True,
        legend=Legend(loc="best"),
        traces=[StackedArea(
            x=wt,
            ys=[ws["data_info"].to_numpy(float),
                ws["carry_info"].to_numpy(float),
                ws["anchor_info"].to_numpy(float)],
            labels=["data", "carry", "anchor"],
            colors=[_C_DATA, _C_CARRY, _C_ANCHOR])],
    )

    # --- Axes 2: cross-sat median of each component + p10-p90 band ---
    ax_med = Axes(title="Cross-satellite info components (median, p10-p90 band)",
                  xlabel="GPS Time", ylabel="information", grid=True,
                  legend=Legend(loc="best"))
    traces = []
    for col, color, lab in (("data_info", _C_DATA, "data"),
                            ("carry_info", _C_CARRY, "carry"),
                            ("anchor_info", _C_ANCHOR, "anchor")):
        med, lo, hi = _epoch_stats(df, epochs, col)
        traces.append(Band(x=epochs, lo=lo, hi=hi, color=color, alpha=0.18,
                           label=f"{lab} p10-p90", show_in_legend=False))
        traces.append(Line(x=epochs, y=med, color=color, width=1.2,
                           label=f"median {lab}"))
    ax_med.traces = traces

    return Figure(name="Network anchor/carry/data info",
                  title="Per-sat information decomposition (lock-in probe)",
                  subtitle="pivot rows excluded",
                  axes=[ax_stack, ax_med], width=12.0, height=8.0, link_x=True)


def _worst_sat(df) -> str:
    """Run-worst sat = highest sustained carry_frac (max count of carry>0.9)."""
    best_sat, best_n, best_mean = None, -1, -1.0
    for sat, sub in df.groupby("sat"):
        cf = sub["carry_frac"].to_numpy(float)
        n_high = int(np.sum(cf > _LOCK_CARRY))
        mean_cf = float(np.nanmean(cf)) if cf.size else 0.0
        # Rank by sustained-high count, then mean carry_frac as tiebreak.
        if (n_high, mean_cf) > (best_n, best_mean):
            best_sat, best_n, best_mean = str(sat), n_high, mean_cf
    return best_sat


def _epoch_stats(df, epochs, col):
    """Per-epoch cross-sat median + p10 + p90 of one column (aligned to epochs)."""
    ev = df["utc"].to_numpy("datetime64[ns]")
    vals = df[col].to_numpy(float)
    med = np.full(epochs.size, np.nan)
    lo = np.full(epochs.size, np.nan)
    hi = np.full(epochs.size, np.nan)
    for i, e in enumerate(epochs):
        v = vals[ev == e]
        v = v[np.isfinite(v)]
        if v.size:
            med[i] = np.median(v)
            lo[i] = np.percentile(v, 10)
            hi[i] = np.percentile(v, 90)
    return med, lo, hi


def _summary(df, n_pivot_rows) -> str:
    out = ["=== Network per-sat anchor / carry / data information ==="]
    out.append(f"Satellites (non-pivot): {df['sat'].nunique()}   "
               f"epochs: {df['utc'].nunique()}")
    out.append(f"Pivot rows excluded from all stats: {n_pivot_rows} "
               "(datum pin inflates data_info).")
    out.append("")

    locked = _locked_in(df)
    if locked:
        out.append(f"LOCK-IN SIGNATURE: carry_frac > {_LOCK_CARRY} sustained "
                   f">= {_LOCK_EPOCHS} epochs on:")
        out.append(f"  {'Sat':<6} {'maxRun':>7} {'meanCarry':>10}")
        for sat, run, mean_cf in locked:
            out.append(f"  {sat:<6} {run:>7} {mean_cf:>10.3f}")
    else:
        out.append(f"No satellite held carry_frac > {_LOCK_CARRY} for "
                   f">= {_LOCK_EPOCHS} consecutive epochs (no lock-in).")

    out.append("")
    worst = _worst_sat(df)
    ws = df[df["sat"] == worst]["carry_frac"].to_numpy(float)
    out.append(f"Run-worst sat: {worst}  "
               f"(mean carry_frac {np.nanmean(ws):.3f}, max {np.nanmax(ws):.3f})")
    return "\n".join(out)


def _locked_in(df):
    """Sats with carry_frac > 0.9 for >= 10 consecutive epochs (by time order)."""
    out = []
    for sat, sub in df.groupby("sat"):
        sub = sub.sort_values("utc")
        cf = sub["carry_frac"].to_numpy(float)
        # Longest run of consecutive-epoch samples above threshold.
        run = best = 0
        for x in cf:
            if np.isfinite(x) and x > _LOCK_CARRY:
                run += 1
                best = max(best, run)
            else:
                run = 0
        if best >= _LOCK_EPOCHS:
            out.append((str(sat), best, float(np.nanmean(cf)) if cf.size else 0.0))
    out.sort(key=lambda t: t[1], reverse=True)
    return out


viewer = Viewer(name="network_anchor_info",
                title="Network per-sat anchor/carry/data info (lock-in probe)",
                required_csvs=("network_anchor_info.csv",),
                build=build_network_anchor_info)

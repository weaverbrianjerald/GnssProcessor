"""Closed-loop held-out consumer viewer -- did the products recover integers?

Reads ``closed_loop_trace.csv`` (the PPP-RTK NETWORK producer's closed-loop
integer-recovery harness: a held-out user station consuming the network's own
products epoch-by-epoch) and shows whether, and how well, the user recovers:

  1. **Position error** -- 3D error on a log axis with 1 dm / 5 cm reference
     lines: the convergence of the held-out user fed the network products.
  2. **East / North / Up** -- the error decomposed, to read out any vertical bias.
  3. **Ambiguity resolution** -- the LAMBDA ratio per epoch, integers fixed, and
     the epochs AR actually accepted a fix.

The headline question -- "does our own user engine recover integers from our own
products on an independent station?" -- is answered by the fix rate and the
converged-window position in the summary.
"""

from __future__ import annotations

import numpy as np

from ..figures import Axes, Figure, HLine, Legend, Line, Scatter
from ..io import add_utc, load_trace, require_cols
from .base import Viewer, ViewerResult, ViewerSkip

_C_3D = (0.10, 0.10, 0.10)
_C_E = (0.13, 0.40, 0.70)
_C_N = (0.20, 0.60, 0.25)
_C_U = (0.85, 0.30, 0.10)
_C_RATIO = (0.35, 0.30, 0.65)
_C_FIX = (0.10, 0.55, 0.20)


def build_network_closed_loop(ctx) -> ViewerResult:
    df = load_trace(ctx.path("closed_loop_trace.csv"))
    require_cols(df, ("week", "tow", "n_sats", "ar_ran", "ar_success",
                      "num_fixed", "num_candidates", "ratio", "pos_err_3d_m",
                      "east_m", "north_m", "up_m"), "closed_loop_trace.csv")
    if df.empty:
        raise ViewerSkip("closed_loop_trace.csv has no rows")

    df = add_utc(df, sort=True)
    t = df["utc"].to_numpy("datetime64[ns]")
    err3d = df["pos_err_3d_m"].to_numpy(float)

    # --- Axes 1: 3D position error (log) ---
    ax_3d = Axes(title="Held-out user 3D position error", ylabel="3D error (m)",
                 xlabel="GPS Time", yscale="log", legend=Legend(loc="best"))
    ax_3d.traces = [
        Line(x=t, y=err3d, color=_C_3D, width=1.2, label="3D error"),
        HLine(y=0.10, color=(0.4, 0.6, 0.9), style="--", label="1 dm"),
        HLine(y=0.05, color=(0.4, 0.8, 0.5), style=":", label="5 cm"),
    ]

    # --- Axes 2: E/N/U ---
    ax_enu = Axes(title="East / North / Up error", ylabel="error (m)",
                  xlabel="GPS Time", legend=Legend(loc="best"))
    ax_enu.traces = [
        Line(x=t, y=df["east_m"].to_numpy(float), color=_C_E, width=1.0, label="East"),
        Line(x=t, y=df["north_m"].to_numpy(float), color=_C_N, width=1.0, label="North"),
        Line(x=t, y=df["up_m"].to_numpy(float), color=_C_U, width=1.0, label="Up"),
        HLine(y=0.0, color=(0.6, 0.6, 0.6), style="-"),
    ]

    # --- Axes 3: ambiguity resolution ---
    ratio = df["ratio"].to_numpy(float)
    fixed = df["ar_success"].to_numpy(float) > 0
    ax_ar = Axes(title="Ambiguity resolution (ratio, fixes)", ylabel="LAMBDA ratio",
                 y2label="integers fixed", xlabel="GPS Time", legend=Legend(loc="best"))
    traces = [Line(x=t, y=ratio, color=_C_RATIO, width=1.0, label="ratio"),
              Line(x=t, y=df["num_fixed"].to_numpy(float), color=(0.7, 0.5, 0.1),
                   width=1.0, axis="right", label="num fixed")]
    if fixed.any():
        traces.append(Scatter(x=t[fixed], y=ratio[fixed], color=_C_FIX, marker="o",
                              size=26, label=f"accepted fix ({int(fixed.sum())})"))
    ax_ar.traces = traces

    fig = Figure(name="Closed-loop recovery",
                 title="Network closed-loop held-out integer recovery",
                 axes=[ax_3d, ax_enu, ax_ar], width=12.0, height=9.5, link_x=True)

    return ViewerResult(figures=[fig], summary=_summary(df, err3d, fixed))


def _summary(df, err3d, fixed) -> str:
    n = len(df)
    ran = df["ar_ran"].to_numpy(float) > 0
    n_ran, n_fix = int(ran.sum()), int(fixed.sum())
    fix_rate = 100.0 * n_fix / n_ran if n_ran else 0.0
    out = ["=== Network Closed-Loop Integer Recovery (held-out user) ==="]
    out.append(f"Epochs processed : {n} ({df['utc'].min()} .. {df['utc'].max()})")
    out.append(f"AR ran / accepted: {n_ran} / {n_fix}  ({fix_rate:.1f}% fix rate)")
    first_fix = df[fixed]
    if not first_fix.empty:
        ff = first_fix.iloc[0]
        out.append(f"First fix        : week {int(ff['week'])} tow {ff['tow']:.1f}")
    win = err3d[np.isfinite(err3d)][-30:]
    if win.size:
        out.append(f"3D error (m)     : final {err3d[np.isfinite(err3d)][-1]:.3f}, "
                   f"last-{win.size} median {np.median(win):.3f}, "
                   f"max {np.max(win):.3f}")
    return "\n".join(out)


viewer = Viewer(name="network_closed_loop",
                title="Network closed-loop held-out recovery",
                required_csvs=("closed_loop_trace.csv",),
                build=build_network_closed_loop)

"""Network solver-health dashboard -- per-epoch estimability & convergence.

Reads ``network_trace.csv`` (the PPP-RTK NETWORK producer's per-epoch aggregate
diagnostics) and shows, over the run:

  1. **Coverage** -- stations present, satellite clocks, and phase biases solved
     each epoch (the size of the shared satellite block), with the declared datum
     dimension.
  2. **Clock-delta** -- the RMS and max estimable satellite-clock correction (m):
     the convergence / stability of the network's headline product.
  3. **Estimability** -- which epochs emitted vs were withheld by the null-space
     observability gate, plus the pivot-ambiguity pin count.

This is the at-a-glance "is the network healthy?" view: long stretches of
withheld epochs or a growing clock-delta RMS flag a thin or ill-posed network.
"""

from __future__ import annotations

import numpy as np

from ..figures import Axes, Figure, HLine, Legend, Line, Scatter
from ..io import add_utc, load_trace, require_cols
from ..palettes import SYSTEM_COLORS
from .base import Viewer, ViewerResult, ViewerSkip

_C_PRESENT = (0.13, 0.40, 0.70)
_C_CLOCK = (0.85, 0.45, 0.10)
_C_PHASE = (0.20, 0.60, 0.25)
_C_DIM = (0.55, 0.30, 0.65)
_C_WITHHELD = (0.85, 0.16, 0.16)


def build_network_health(ctx) -> ViewerResult:
    df = load_trace(ctx.path("network_trace.csv"))
    require_cols(df, ("week", "tow", "n_present", "n_clock_sats", "n_phase_bias",
                      "declared_dim", "gate_ok", "solved", "rms_clock_m",
                      "max_clock_m", "n_pivot_pins"), "network_trace.csv")
    if df.empty:
        raise ViewerSkip("network_trace.csv has no rows")

    df = add_utc(df, sort=True)
    t = df["utc"].to_numpy("datetime64[ns]")
    solved = df["solved"].to_numpy(float) > 0
    rms = np.where(solved, df["rms_clock_m"].to_numpy(float), np.nan)
    mx = np.where(solved, df["max_clock_m"].to_numpy(float), np.nan)

    # --- Axes 1: coverage counts ---
    ax_cov = Axes(title="Shared-block coverage", ylabel="count", xlabel="GPS Time",
                  legend=Legend(loc="best"))
    ax_cov.traces = [
        Line(x=t, y=df["n_present"].to_numpy(float), color=_C_PRESENT,
             width=1.2, label="stations present"),
        Line(x=t, y=df["n_clock_sats"].to_numpy(float), color=_C_CLOCK,
             width=1.2, label="sat clocks"),
        Line(x=t, y=df["n_phase_bias"].to_numpy(float), color=_C_PHASE,
             width=1.0, label="phase biases"),
        Line(x=t, y=df["declared_dim"].to_numpy(float), color=_C_DIM,
             width=1.0, style="--", label="declared datum dim"),
    ]

    # --- Axes 2: clock-delta convergence ---
    ax_clk = Axes(title="Estimable satellite clock correction", ylabel="clock Δ (m)",
                  xlabel="GPS Time", legend=Legend(loc="best"))
    ax_clk.traces = [
        Line(x=t, y=rms, color=_C_CLOCK, width=1.2, label="RMS |clkΔ|"),
        Line(x=t, y=mx, color=(0.7, 0.2, 0.2), width=0.9, style="--",
             label="max |clkΔ|"),
    ]

    # --- Axes 3: estimability (emitted vs withheld) + pivot pins ---
    ax_est = Axes(title="Estimability — emitted epochs vs gate-withheld",
                  ylabel="pivot pins", xlabel="GPS Time", legend=Legend(loc="best"))
    withheld = ~solved
    traces = [Line(x=t, y=df["n_pivot_pins"].to_numpy(float),
                   color=(0.4, 0.4, 0.4), width=0.9, label="pivot pins")]
    if withheld.any():
        ylevel = float(np.nanmax(df["n_pivot_pins"].to_numpy(float))) or 1.0
        traces.append(Scatter(x=t[withheld],
                              y=np.full(int(withheld.sum()), ylevel * 1.05),
                              color=_C_WITHHELD, marker="v", size=24,
                              label=f"withheld ({int(withheld.sum())})"))
    ax_est.traces = traces

    fig = Figure(name="Network health",
                 title="PPP-RTK network solver health",
                 axes=[ax_cov, ax_clk, ax_est], width=12.0, height=9.5, link_x=True)

    return ViewerResult(figures=[fig], summary=_summary(df, solved, rms))


def _summary(df, solved, rms) -> str:
    n = len(df)
    n_solved = int(solved.sum())
    out = ["=== PPP-RTK Network Solver Health ==="]
    out.append(f"Epochs in trace : {n}")
    out.append(f"Emitted (solved): {n_solved}  ({100.0 * n_solved / max(n, 1):.1f}%)")
    out.append(f"Withheld/skipped: {n - n_solved}")
    gate = df["gate_ok"].to_numpy(float)
    out.append(f"Gate-OK epochs  : {int((gate > 0).sum())}")
    if n_solved:
        r = rms[np.isfinite(rms)]
        out.append(f"Clock Δ RMS (m) : first {r[0]:.3f}, last {r[-1]:.3f}, "
                   f"median {np.median(r):.3f}, max {np.nanmax(r):.3f}")
        last = df[solved].iloc[-1]
        out.append(f"Final epoch     : {int(last['n_present'])} stations, "
                   f"{int(last['n_clock_sats'])} clocks, "
                   f"{int(last['n_phase_bias'])} phase biases, "
                   f"datum dim {int(last['declared_dim'])}")
    return "\n".join(out)


viewer = Viewer(name="network_health", title="Network solver-health dashboard",
                required_csvs=("network_trace.csv",), build=build_network_health)

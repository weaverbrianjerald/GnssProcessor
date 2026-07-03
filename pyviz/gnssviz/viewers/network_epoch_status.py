"""Network per-epoch disposition timeline -- the QC "verdict" per epoch.

Reads ``network_epoch_status.csv`` (the PPP-RTK NETWORK producer's L1 trace: ONE
row per processed epoch, emitted OR skipped) and lays out three x-linked axes
that together answer "what did the network do this epoch, and why?":

  1. **Disposition lanes** -- a categorical scatter placing each epoch in one of
     six fixed lanes (``emitted`` / ``gate_withheld`` / ``sigma0_withheld`` /
     ``solve_failed`` / ``datum_failed`` / ``sync_skip``) with a fixed palette
     (emitted green, sigma0 red, gate grey, failures black).  A vertical line
     marks every epoch where the clock datum changed (``datum_changed == 1``).
  2. **Robust variance factor** -- the post-adaptation σ̂0 line (blank/NaN at
     non-emitted dispositions, never plotted as zero), with the config-echoed
     trigger (dashed) and withhold ceiling (solid) horizontal lines, plus
     scatter overlays on epochs that refloated a satellite or applied a retro
     ambiguity bump.
  3. **Clock stats** -- the estimable satellite-clock median with a ±std band
     and the worst-satellite deviation, the headline product-stability trace.

A plateau heuristic in the summary compares the last-third vs first-third median
``clock_std_m`` over EMITTED epochs; a ratio > 2 raises an advisory
"LOCK-IN SUSPECTED" note (a lock-in freezes the spread instead of shrinking it).
"""

from __future__ import annotations

import numpy as np

from ..figures import Axes, Band, Figure, HLine, Legend, Line, Scatter, VLine
from ..io import add_utc, load_trace, require_cols
from .base import Viewer, ViewerResult, ViewerSkip

# The six dispositions the producer emits, in lane order (bottom -> top), each
# with a FIXED colour: emitted green, sigma0 red, gate grey, failures black.
_DISPOSITIONS = (
    "emitted",
    "gate_withheld",
    "sigma0_withheld",
    "solve_failed",
    "datum_failed",
    "sync_skip",
)
_DISP_COLOR = {
    "emitted":         (0.16, 0.63, 0.24),   # green
    "gate_withheld":   (0.55, 0.55, 0.55),   # grey
    "sigma0_withheld": (0.85, 0.16, 0.16),   # red
    "solve_failed":    (0.05, 0.05, 0.05),   # black
    "datum_failed":    (0.05, 0.05, 0.05),   # black
    "sync_skip":       (0.35, 0.35, 0.35),   # grey (present-but-no-datum)
}
_UNKNOWN_COLOR = (0.75, 0.35, 0.75)

_C_SIGMA0 = (0.15, 0.35, 0.70)
_C_TRIGGER = (0.85, 0.55, 0.10)
_C_CEILING = (0.85, 0.16, 0.16)
_C_REFLOAT = (0.20, 0.55, 0.75)
_C_RETRO = (0.75, 0.30, 0.65)
_C_MEDIAN = (0.13, 0.40, 0.70)
_C_WORST = (0.80, 0.25, 0.20)
_C_BAND = (0.35, 0.55, 0.80)


def build_network_epoch_status(ctx) -> ViewerResult:
    df = load_trace(ctx.path("network_epoch_status.csv"))
    require_cols(df, ("week", "tow", "disposition", "sigma0", "n_present",
                      "clock_median_m", "clock_std_m", "worst_sat",
                      "worst_dev_m", "n_refloat_sats", "n_retro_applied",
                      "datum_changed", "sigma0_trigger", "withhold_ceiling"),
                 "network_epoch_status.csv")
    if df.empty:
        raise ViewerSkip("network_epoch_status.csv has no rows")

    df = add_utc(df, sort=True)
    t = df["utc"].to_numpy("datetime64[ns]")
    disp = df["disposition"].astype("string").fillna("").to_numpy()

    # --- Axes 1: disposition lanes (categorical scatter) ---
    lane_index = {d: i for i, d in enumerate(_DISPOSITIONS)}
    ax_disp = Axes(title="Per-epoch disposition", ylabel="disposition",
                   xlabel="", legend=Legend(loc="upper right"),
                   ylim=(-0.5, len(_DISPOSITIONS) - 0.5))
    lane_traces = []
    for d in _DISPOSITIONS:
        mask = disp == d
        if not mask.any():
            continue
        lane_traces.append(Scatter(
            x=t[mask], y=np.full(int(mask.sum()), lane_index[d]),
            color=_DISP_COLOR[d], marker="s", size=14,
            label=f"{d} ({int(mask.sum())})"))
    # Any disposition string outside the known set -> a distinct lane at the top.
    unknown = ~np.isin(disp, np.array(_DISPOSITIONS))
    if unknown.any():
        lane_traces.append(Scatter(
            x=t[unknown], y=np.full(int(unknown.sum()), len(_DISPOSITIONS) - 0.5),
            color=_UNKNOWN_COLOR, marker="x", size=20,
            label=f"other ({int(unknown.sum())})"))
    # Vertical line at every datum change.
    dc = df["datum_changed"].to_numpy(float)
    for x in t[dc == 1.0]:
        lane_traces.append(VLine(x=x, color=(0.55, 0.30, 0.65), style=":",
                                 width=0.9, label=None))
    ax_disp.traces = lane_traces

    # --- Axes 2: robust variance factor sigma0 (NaN where blank) ---
    sig = df["sigma0"].to_numpy(float)   # blanks already NaN via read_csv
    ax_sig = Axes(title="Robust variance factor σ̂0", ylabel="σ̂0", xlabel="",
                  legend=Legend(loc="best"))
    sig_traces = [Line(x=t, y=sig, color=_C_SIGMA0, width=1.1, label="σ̂0")]
    trig = _const(df["sigma0_trigger"])
    ceil = _const(df["withhold_ceiling"])
    if np.isfinite(trig):
        sig_traces.append(HLine(y=trig, color=_C_TRIGGER, style="--",
                                label=f"trigger ({trig:.2f})"))
    if np.isfinite(ceil):
        sig_traces.append(HLine(y=ceil, color=_C_CEILING, style="-",
                                label=f"withhold ceiling ({ceil:.2f})"))
    # Overlays: epochs that refloated a sat / applied a retro bump (on the σ̂0 line).
    n_ref = df["n_refloat_sats"].to_numpy(float)
    ref_mask = np.isfinite(n_ref) & (n_ref > 0)
    if ref_mask.any():
        sig_traces.append(Scatter(
            x=t[ref_mask], y=_at(sig, ref_mask, trig, ceil), color=_C_REFLOAT,
            marker="^", size=26, label=f"refloat ({int(ref_mask.sum())})"))
    n_ret = df["n_retro_applied"].to_numpy(float)
    ret_mask = np.isfinite(n_ret) & (n_ret > 0)
    if ret_mask.any():
        sig_traces.append(Scatter(
            x=t[ret_mask], y=_at(sig, ret_mask, trig, ceil), color=_C_RETRO,
            marker="D", size=22, label=f"retro bump ({int(ret_mask.sum())})"))
    ax_sig.traces = sig_traces

    # --- Axes 3: clock median +/- std, worst-sat deviation ---
    med = df["clock_median_m"].to_numpy(float)
    std = df["clock_std_m"].to_numpy(float)
    worst_dev = df["worst_dev_m"].to_numpy(float)
    ax_clk = Axes(title="Estimable satellite clock — median ± std",
                  ylabel="clock (m)", xlabel="GPS Time",
                  legend=Legend(loc="best"))
    ax_clk.traces = [
        Band(x=t, lo=med - std, hi=med + std, color=_C_BAND, alpha=0.20,
             label="±std", show_in_legend=True),
        Line(x=t, y=med, color=_C_MEDIAN, width=1.3, label="clock median"),
        Line(x=t, y=worst_dev, color=_C_WORST, width=0.9, style="--",
             label="worst-sat |dev|"),
    ]

    fig = Figure(name="Network epoch status",
                 title="PPP-RTK network — per-epoch disposition & stability",
                 axes=[ax_disp, ax_sig, ax_clk], width=12.0, height=10.0,
                 link_x=True)

    return ViewerResult(figures=[fig], summary=_summary(df, disp))


# --------------------------------------------------------------------------- #
def _const(series) -> float:
    """First finite value of a config-echo column (constant across rows)."""
    v = series.to_numpy(float)
    v = v[np.isfinite(v)]
    return float(v[0]) if v.size else float("nan")


def _at(sig, mask, trig, ceil):
    """y-positions for σ̂0 overlays: use σ̂0 where finite, else a sensible level."""
    y = sig[mask].copy()
    if not np.all(np.isfinite(y)):
        fallback = trig if (trig is not None and np.isfinite(trig)) else 1.0
        if ceil is not None and np.isfinite(ceil):
            fallback = ceil
        y = np.where(np.isfinite(y), y, fallback)
    return y


def _plateau(std_emitted: np.ndarray) -> tuple:
    """(ratio, verdict-text): last-third vs first-third median clock_std over emitted."""
    v = std_emitted[np.isfinite(std_emitted)]
    n = v.size
    if n < 6:
        return (float("nan"), "insufficient emitted epochs for plateau test")
    third = max(1, n // 3)
    first = float(np.median(v[:third]))
    last = float(np.median(v[-third:]))
    if not (first > 0):
        return (float("nan"), "first-third median std is zero — no plateau ratio")
    ratio = last / first
    if ratio > 2.0:
        verdict = (f"LOCK-IN SUSPECTED — clock-std plateaued (last/first = "
                   f"{ratio:.2f} > 2; spread grew instead of shrinking) [advisory]")
    else:
        verdict = (f"clock-std shrinking/stable (last/first = {ratio:.2f}) — "
                   f"no lock-in signature")
    return (ratio, verdict)


def _summary(df, disp) -> str:
    n = len(df)
    out = ["=== PPP-RTK Network Epoch Status ==="]
    out.append(f"Epochs processed : {n}")

    # Disposition counts (fixed order, then any others).
    for d in _DISPOSITIONS:
        c = int((disp == d).sum())
        if c:
            out.append(f"  {d:<16}: {c}  ({100.0 * c / max(n, 1):.1f}%)")
    others = sorted(set(disp) - set(_DISPOSITIONS))
    for d in others:
        if str(d) == "":
            continue
        out.append(f"  {str(d):<16}: {int((disp == d).sum())}  (unrecognised)")

    emitted = disp == "emitted"
    n_emit = int(emitted.sum())
    out.append(f"Emitted          : {n_emit}  "
               f"({100.0 * n_emit / max(n, 1):.1f}%)")
    out.append(f"Datum changes    : {int((df['datum_changed'].to_numpy(float) == 1.0).sum())}")

    trig = _const(df["sigma0_trigger"])
    ceil = _const(df["withhold_ceiling"])
    out.append(f"σ̂0 trigger       : {trig:.3f}    withhold ceiling: {ceil:.3f}")

    if n_emit:
        sig = df["sigma0"].to_numpy(float)[emitted]
        sig = sig[np.isfinite(sig)]
        if sig.size:
            out.append(f"σ̂0 (emitted)     : median {np.median(sig):.3f}, "
                       f"max {np.max(sig):.3f}")
        med = df["clock_median_m"].to_numpy(float)[emitted]
        std = df["clock_std_m"].to_numpy(float)[emitted]
        med_f = med[np.isfinite(med)]
        std_f = std[np.isfinite(std)]
        if med_f.size:
            out.append(f"Clock median (m) : median {np.median(med_f):.3f}, "
                       f"|max| {np.nanmax(np.abs(med_f)):.3f}")
        if std_f.size:
            out.append(f"Clock std (m)    : median {np.median(std_f):.3f}, "
                       f"max {np.max(std_f):.3f}")

        # Run-worst satellite by NAME (per convention: never rms alone).
        ws = df.loc[emitted, "worst_sat"].astype("string")
        wd = df.loc[emitted, "worst_dev_m"].to_numpy(float)
        finite = np.isfinite(wd) & ws.notna().to_numpy() & (ws != "").to_numpy()
        if finite.any():
            i = int(np.nanargmax(np.where(finite, np.abs(wd), -np.inf)))
            out.append(f"Run-worst sat    : {str(ws.iloc[i])} "
                       f"(|dev| {abs(wd[i]):.3f} m)")

        ratio, verdict = _plateau(df["clock_std_m"].to_numpy(float)[emitted])
        out.append("")
        out.append(f"Plateau test     : {verdict}")
    else:
        out.append("No emitted epochs — nothing to assess for stability.")

    n_ref = df["n_refloat_sats"].to_numpy(float)
    n_ret = df["n_retro_applied"].to_numpy(float)
    out.append("")
    out.append(f"Refloat epochs   : {int((np.isfinite(n_ref) & (n_ref > 0)).sum())}")
    out.append(f"Retro-bump epochs: {int((np.isfinite(n_ret) & (n_ret > 0)).sum())}")
    return "\n".join(out)


viewer = Viewer(name="network_epoch_status",
                title="Network epoch status (disposition timeline)",
                required_csvs=("network_epoch_status.csv",),
                build=build_network_epoch_status)

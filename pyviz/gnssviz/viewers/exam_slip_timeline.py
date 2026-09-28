"""Cycle-slip timeline: the exam's answer key of where the phase breaks are.

Reads the GnssSimulator answer key ``truth_slips.csv`` (bound via ``--truth-dir``)
and lays every injected cycle slip out on a per-satellite lane vs GPS time, so the
recovered solution's slip handling can be graded against ground truth (SPEC §4.2 /
R4).  Each slip is drawn once, coloured + shaped by its class
(:data:`gnssviz.viewers.exam_common.SLIP_CLASS_COLORS`): flagged (LLI=1),
unflagged (LLI=0, must be caught via MW/GF), scripted (hand-authored edge case), or
false-alarm (Δ=0, LLI-only), with the marker size scaled by ``|Δcycles|`` so a
40-cycle jump reads bigger than a 1-cycle one.  The storm window is overlaid as
paired dashed :class:`VLine`s.

Because the lanes are categorical (one integer lane per PRN-sorted satellite) the
lane-to-satellite key is drawn as left-edge text (placed in axes-fraction coords so
it lands identically under both backends) and repeated in the summary.  The
summary also breaks the slips down by class and by signal, names the largest jump,
and flags any ``(satellite, signal)`` **burst**: three or more slips on
consecutive epochs (epoch spacing = ``interval_s`` from ``truth_scalars``).

Pure data layer: emits :mod:`gnssviz.figures` specs only, no plotting library.
"""

from __future__ import annotations

import math

import numpy as np
import pandas as pd

from ..figures import Axes, Figure, Legend, Scatter, TextBox
from ..identifiers import prn_num, system_of
from ..io import add_utc, first_col
from ..palettes import SYSTEM_ORDER
from .base import Viewer, ViewerResult, ViewerSkip
from . import exam_common as ec

# One marker glyph per slip class (colour comes from ec.SLIP_CLASS_COLORS).
_CLASS_MARKER = {
    "flagged":     "o",
    "unflagged":   "x",
    "scripted":    "^",
    "false-alarm": "s",
}


def _sat_sort_key(sat: str):
    """PRN sort: group by constellation (canonical order) then numeric PRN."""
    sys = system_of(sat)
    try:
        si = list(SYSTEM_ORDER).index(sys)
    except ValueError:
        si = len(SYSTEM_ORDER)
    p = prn_num(sat)
    return (si, p if p is not None else 9999, str(sat))


def _slip_size(delta) -> float:
    """Marker size ~ |Δcycles|: ``8 + 4·log10(max(1, |Δ|))`` (Δ=0 → 8)."""
    try:
        d = abs(float(delta))
    except (TypeError, ValueError):
        d = 0.0
    if not math.isfinite(d):
        d = 0.0
    return 8.0 + 4.0 * math.log10(max(1.0, d))


def build_exam_slip_timeline(ctx) -> ViewerResult:
    df = ec.require_truth(ctx, "truth_slips.csv")
    if "delta_cycles" not in df.columns or "satellite" not in df.columns:
        raise ViewerSkip("truth_slips.csv lacks delta_cycles/satellite columns")
    df = add_utc(df)
    df = df.assign(_cls=ec.classify_slips(df).to_numpy())

    have_signal = "signal" in df.columns
    sig = df["signal"].astype(str) if have_signal else pd.Series([""] * len(df), index=df.index)

    # PRN-sorted lane assignment (one lane per satellite).
    sats = sorted({str(s) for s in df["satellite"]}, key=_sat_sort_key)
    lane_of = {s: i for i, s in enumerate(sats)}
    n = len(sats)

    # ------------------------------------------------------------------ #
    #  Figure: slip lanes, one Scatter group per (class, size bucket).
    # ------------------------------------------------------------------ #
    traces = list(ec.storm_vlines(ctx))
    for cls in ec.SLIP_CLASS_ORDER:
        sub = df[df["_cls"] == cls]
        if sub.empty:
            continue
        color = ec.SLIP_CLASS_COLORS[cls]
        marker = _CLASS_MARKER.get(cls, "o")
        # Bucket by rounded size so per-point sizing survives the scalar-size
        # trace contract (Scatter.size is one value) without one trace per point.
        sizes = np.round([_slip_size(d) for d in sub["delta_cycles"]], 1)
        buck = sub.assign(_size=sizes)
        first = True
        for sz, grp in buck.groupby("_size"):
            xs = list(grp["utc"])
            ys = [lane_of[str(s)] for s in grp["satellite"]]
            traces.append(Scatter(
                x=xs, y=ys, color=color, marker=marker, size=float(sz), alpha=0.85,
                label=(cls if first else None), show_in_legend=first))
            first = False

    # Lane-to-satellite labels at the left edge.  data_coords=False maps to
    # axes-fraction in both backends; with ylim fixed to (-0.5, n-0.5) lane i sits
    # at fraction (i+0.5)/n, so a fractional label lands exactly on its row.
    for s in sats:
        i = lane_of[s]
        frac = (i + 0.5) / n if n else 0.5
        traces.append(TextBox(x=0.006, y=frac, text=s, ha="left", va="center",
                              mono=True, boxed=True, fontsize=7.0))

    ax = Axes(title="Cycle-slip timeline (by class)",
              xlabel="GPS Time", ylabel="satellite (lane)",
              ylim=(-0.5, (n - 0.5) if n else 0.5),
              legend=Legend(loc="upper right"), x_is_datetime=True)
    ax.traces = traces
    height = ec.sat_strip_height(n)   # shared tile height (consistent cells across strips)
    fig = Figure(name="Cycle-slip timeline", axes=[ax], width=13, height=height)

    summary = _summary(ctx, df, sig, sats, lane_of)
    return ViewerResult(figures=[fig], summary=summary)


def _find_bursts(df: pd.DataFrame, interval, tol: float = 0.5):
    """(sat, signal) runs of >=3 slips on consecutive epochs (Δtow == interval).

    Returns ``(bursts, available)``; ``available`` is ``False`` when no usable
    ``interval_s`` is known so the summary can say so rather than silently drop
    the check.
    """
    try:
        iv = float(interval)
    except (TypeError, ValueError):
        return [], False
    if not math.isfinite(iv) or iv <= 0:
        return [], False

    have_signal = "signal" in df.columns
    keys = (["satellite", "signal"] if have_signal else ["satellite"])
    bursts = []
    for key, g in df.groupby(keys):
        # pandas yields a tuple for a multi-key groupby and (2.2+) a 1-tuple even
        # for a single-element key list; normalise so the label is never str(tuple).
        key = key if isinstance(key, tuple) else (key,)
        g = g.sort_values("tow_s")
        tows = g["tow_s"].to_numpy(float)
        utcs = list(g["utc"])
        sat = key[0]
        signal = key[1] if have_signal else ""
        run_start = 0
        for i in range(1, len(tows) + 1):
            broken = (i == len(tows)) or (abs(tows[i] - tows[i - 1] - iv) > tol)
            if broken:
                run_len = i - run_start
                if run_len >= 3:
                    bursts.append({
                        "sat": str(sat), "signal": str(signal), "count": run_len,
                        "t0": utcs[run_start], "t1": utcs[i - 1]})
                run_start = i
    return bursts, True


def _summary(ctx, df: pd.DataFrame, sig: pd.Series, sats, lane_of) -> str:
    n = len(df)
    out = ["=== Cycle-slip timeline (truth answer key) ===",
           f"Total slips      : {n}"]

    # By class (SLIP_CLASS_ORDER, only non-empty).
    counts = df["_cls"].value_counts()
    parts = [f"{k}={int(counts.get(k, 0))}" for k in ec.SLIP_CLASS_ORDER if counts.get(k, 0)]
    out.append("  by class       : " + (", ".join(parts) if parts else "(none)"))

    # By signal.
    if (sig != "").any():
        sc = sig.value_counts()
        sparts = [f"{s}={int(c)}" for s, c in sc.items() if str(s)]
        out.append("  by signal      : " + ", ".join(sparts))
    else:
        out.append("  by signal      : (no signal column)")

    # Largest |Δ|.
    absd = df["delta_cycles"].abs()
    if absd.notna().any():
        j = absd.idxmax()
        r = df.loc[j]
        sg = str(r["signal"]) if "signal" in df.columns else ""
        t = r["utc"]
        tstr = t.strftime("%H:%M:%S") if hasattr(t, "strftime") else str(t)
        out.append(f"  largest |Δ|     : {int(round(abs(float(r['delta_cycles']))))} cyc "
                   f"@ {r['satellite']}/{sg} {tstr} (Δ={int(round(float(r['delta_cycles'])))})")

    # Bursts (>=3 on consecutive epochs).
    sc = ec.scalars(ctx)
    bursts, available = _find_bursts(df, sc.get("interval_s"))
    out.append("")
    if not available:
        out.append("Bursts           : (interval_s unknown — consecutive-epoch check skipped)")
    elif not bursts:
        out.append("Bursts           : none (no (sat,signal) with >=3 consecutive-epoch slips)")
    else:
        out.append(f"Bursts (>=3 consecutive epochs): {len(bursts)}")
        for b in bursts:
            t0 = b["t0"].strftime("%H:%M:%S") if hasattr(b["t0"], "strftime") else str(b["t0"])
            t1 = b["t1"].strftime("%H:%M:%S") if hasattr(b["t1"], "strftime") else str(b["t1"])
            sgl = f"/{b['signal']}" if b["signal"] else ""
            out.append(f"  {b['sat']}{sgl}: {b['count']} epochs  {t0} -> {t1}")

    # Lane key (lane index -> satellite).
    out.append("")
    out.append("Lane key         : " + ", ".join(f"{lane_of[s]}={s}" for s in sats))
    return "\n".join(out)


viewer = Viewer(name="exam_slip_timeline", title="Cycle-slip timeline (truth)",
                required_csvs=("truth_slips.csv",),
                build=build_exam_slip_timeline)

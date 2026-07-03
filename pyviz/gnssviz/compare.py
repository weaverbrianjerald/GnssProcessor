"""A/B run-comparison for two GnssProcessor NETWORK runs (matplotlib-free).

A validation run is only meaningful *against a baseline*: "floors OFF locks in at
epoch 42, floors ON stays fixed" is a statement about two runs, not one.  The
single-``ctx`` :mod:`~gnssviz.viewers` all answer "how did THIS run behave?"; the
comparators here answer "how did run **B** differ from run **A**?" over the same
epochs.

Design
------
* A :class:`ComparePair` bundles two :class:`~gnssviz.trace_paths.TraceContext`
  (``a`` / ``b``) with human labels.
* Each *comparator* is a plain function ``fn(pair) -> ViewerResult`` -- the same
  return type the report assembler already consumes, so the A/B report reuses
  :func:`gnssviz.report.write_html_report` verbatim (no forked rendering).
* Comparators are **not** registered in the viewers ``REGISTRY``: that driver is
  single-``ctx`` and would call them with the wrong argument.  They live in the
  ordered :data:`COMPARATORS` list, driven by :mod:`run_compare`.
* A comparator that cannot run (a required CSV is absent on *either* side) raises
  :class:`~gnssviz.viewers.base.ViewerSkip`; :func:`run_comparators` turns that
  into a ``(name, reason)`` skip entry, exactly like the single-ctx driver.

Epoch alignment
---------------
Two runs withhold *different* epochs (that is often the whole point of the A/B),
so a naive positional zip is wrong.  :func:`align_epochs` joins on
``(week, round(tow*1000))`` -- an **inner** join for any B-A delta series (a delta
is only defined where both ran) and reports the per-side unmatched-epoch counts so
a reader knows how comparable the two series even are.  Overlay traces use each
run's *full* series (no join) so a withheld stretch shows as a gap, not a
silently-dropped sample.

Colours
-------
Run **A** is a dark neutral (the baseline); run **B** is the ``vik`` cool
endpoint (deep blue); B-A **delta** series use the ``vik`` warm endpoint.  These
are the same anchors the diverging strip maps use, so the A/B plots read as part
of the same suite.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, List, Optional, Tuple

import numpy as np
import pandas as pd

from .figures import Axes, Figure, HLine, Legend, Line, Scatter, TextBox
from .io import add_utc, load_trace, require_cols
from .trace_paths import TraceContext
from .viewers.base import ViewerResult, ViewerSkip

# --- palette (see module docstring) ---------------------------------------- #
C_A = (0.20, 0.20, 0.20)          # baseline: dark neutral
C_B = (0.020, 0.231, 0.463)       # run B: vik cool endpoint (deep blue)
C_DELTA = (0.624, 0.247, 0.149)   # B - A delta: vik warm endpoint
C_REF = (0.60, 0.60, 0.60)


@dataclass
class ComparePair:
    """Two runs to compare, each with a short label ("A" / "B" by default)."""
    a: TraceContext
    b: TraceContext
    label_a: str = "A"
    label_b: str = "B"

    def both_have(self, csv_name: str) -> bool:
        """``True`` only if *both* sides have at least one data row of ``csv_name``."""
        return self.a.has_data(csv_name) and self.b.has_data(csv_name)

    def require_both(self, csv_name: str) -> None:
        """Raise :class:`ViewerSkip` unless both sides carry ``csv_name`` data."""
        miss = [lab for lab, ctx in ((self.label_a, self.a), (self.label_b, self.b))
                if not ctx.has_data(csv_name)]
        if miss:
            raise ViewerSkip(f"{csv_name} absent/empty on side(s): {', '.join(miss)}")


# --------------------------------------------------------------------------- #
#  Epoch alignment
# --------------------------------------------------------------------------- #
@dataclass
class AlignResult:
    """Inner-join of two per-epoch frames on ``(week, round(tow*1000))``.

    ``merged`` carries ``<col>_a`` / ``<col>_b`` for every requested value column
    plus a shared ``utc``; ``n_only_a`` / ``n_only_b`` are the unmatched-epoch
    counts on each side (the withheld sets differ between runs).
    """
    merged: pd.DataFrame
    n_a: int
    n_b: int
    n_match: int
    n_only_a: int
    n_only_b: int


def _epoch_key(df: pd.DataFrame) -> pd.DataFrame:
    """Add an integer ``_ekey`` = round(tow*1000) and integer ``_wk`` for joining."""
    out = df.copy()
    wk = out["week"].astype(float).round().astype("int64")
    tow = out["tow_s"] if "tow_s" in out.columns else out["tow"]
    out["_wk"] = wk
    out["_ekey"] = (tow.astype(float) * 1000.0).round().astype("int64")
    return out


def align_epochs(da: pd.DataFrame, db: pd.DataFrame,
                 value_cols: Optional[List[str]] = None) -> AlignResult:
    """Inner-join two per-epoch frames on ``(week, round(tow*1000))``.

    Both frames must carry ``week`` and ``tow``/``tow_s``.  ``value_cols`` (default:
    every non-key column common to both) are suffixed ``_a`` / ``_b`` in the merged
    result.  Unmatched-epoch counts are reported per side.
    """
    a = _epoch_key(da)
    b = _epoch_key(db)
    if value_cols is None:
        common = [c for c in a.columns if c in b.columns
                  and c not in ("_wk", "_ekey", "week", "tow", "tow_s", "utc")]
        value_cols = common
    keep = ["_wk", "_ekey"] + [c for c in value_cols]
    a_k = a[[c for c in keep if c in a.columns]].copy()
    b_k = b[[c for c in keep if c in b.columns]].copy()
    merged = a_k.merge(b_k, on=["_wk", "_ekey"], how="inner",
                       suffixes=("_a", "_b"))
    # A shared utc for the x-axis, reconstructed from the join keys.
    if not merged.empty:
        merged = merged.rename(columns={"_wk": "week"})
        merged["tow"] = merged["_ekey"].astype(float) / 1000.0
        merged = add_utc(merged, sort=True)
    n_a = len(a_k.drop_duplicates(["_wk", "_ekey"]))
    n_b = len(b_k.drop_duplicates(["_wk", "_ekey"]))
    n_match = len(merged.drop_duplicates(["week", "_ekey"])) if not merged.empty else 0
    return AlignResult(merged=merged, n_a=n_a, n_b=n_b, n_match=n_match,
                       n_only_a=max(n_a - n_match, 0), n_only_b=max(n_b - n_match, 0))


# --------------------------------------------------------------------------- #
#  Small numeric helpers
# --------------------------------------------------------------------------- #
def _finite(v) -> np.ndarray:
    v = np.asarray(v, dtype=float)
    return v[np.isfinite(v)]


def _rms(v) -> float:
    v = _finite(v)
    return float(np.sqrt(np.mean(v * v))) if v.size else float("nan")


def _std(v) -> float:
    v = _finite(v)
    return float(np.std(v, ddof=1)) if v.size > 1 else 0.0


def _log_floor(y: np.ndarray, floor: float = 1e-12) -> np.ndarray:
    """Clamp non-positive values to a small positive floor for a log y-axis.

    The matplotlib backend only applies a log scale to positive data; a genuine
    0.0 (e.g. a pre-convergence error sample) would otherwise be dropped.
    """
    y = np.asarray(y, dtype=float)
    out = y.copy()
    bad = ~(out > 0)
    out[bad] = floor
    return out


# --------------------------------------------------------------------------- #
#  (a) closed-loop held-out position error
# --------------------------------------------------------------------------- #
def compare_closed_loop(pair: ComparePair) -> ViewerResult:
    """A/B the held-out consumer's 3D position error + integer-fix behaviour."""
    csv = "closed_loop_trace.csv"
    pair.require_both(csv)
    cols = ("week", "tow", "ar_success", "pos_err_3d_m")
    da = load_trace(pair.a.path(csv)); require_cols(da, cols, f"{csv} (A)")
    db = load_trace(pair.b.path(csv)); require_cols(db, cols, f"{csv} (B)")
    da = add_utc(da, sort=True); db = add_utc(db, sort=True)

    ta = da["utc"].to_numpy("datetime64[ns]")
    tb = db["utc"].to_numpy("datetime64[ns]")
    ea = _log_floor(da["pos_err_3d_m"].to_numpy(float))
    eb = _log_floor(db["pos_err_3d_m"].to_numpy(float))
    fa = da["ar_success"].to_numpy(float) > 0
    fb = db["ar_success"].to_numpy(float) > 0

    ax_err = Axes(title="Held-out user 3D position error", ylabel="3D error (m)",
                  xlabel="GPS Time", yscale="log", legend=Legend(loc="best"))
    ax_err.traces = [
        Line(x=ta, y=ea, color=C_A, width=1.3, style="-",
             label=f"{pair.label_a}"),
        Line(x=tb, y=eb, color=C_B, width=1.3, style="--",
             label=f"{pair.label_b}"),
        HLine(y=0.10, color=(0.4, 0.6, 0.9), style="--", label="1 dm"),
        HLine(y=0.05, color=(0.4, 0.8, 0.5), style=":", label="5 cm"),
    ]
    if fa.any():
        ax_err.traces.append(Scatter(x=ta[fa], y=ea[fa], color=C_A, marker="o",
                                     size=22, label=f"{pair.label_a} fix ({int(fa.sum())})"))
    if fb.any():
        ax_err.traces.append(Scatter(x=tb[fb], y=eb[fb], color=C_B, marker="^",
                                     size=22, label=f"{pair.label_b} fix ({int(fb.sum())})"))

    # B - A delta on the matched epochs.
    al = align_epochs(da[["week", "tow", "pos_err_3d_m"]],
                      db[["week", "tow", "pos_err_3d_m"]], ["pos_err_3d_m"])
    ax_d = Axes(title=f"{pair.label_b} - {pair.label_a} 3D error (matched epochs)",
                ylabel="Δ 3D error (m)", xlabel="GPS Time", legend=Legend(loc="best"))
    if not al.merged.empty:
        td = al.merged["utc"].to_numpy("datetime64[ns]")
        delta = (al.merged["pos_err_3d_m_b"].to_numpy(float)
                 - al.merged["pos_err_3d_m_a"].to_numpy(float))
        ax_d.traces = [Line(x=td, y=delta, color=C_DELTA, width=1.2,
                            label=f"{pair.label_b}-{pair.label_a}"),
                       HLine(y=0.0, color=C_REF, style="-")]
    else:
        ax_d.traces = [TextBox(x=0.5, y=0.5, text="no matched epochs",
                               ha="center", va="center")]

    fig = Figure(name="AB closed loop",
                 title="A/B closed-loop held-out recovery",
                 axes=[ax_err, ax_d], width=12.0, height=8.0, link_x=False)
    return ViewerResult(figures=[fig],
                        summary=_cl_summary(pair, da, db, fa, fb, al))


def _cl_summary(pair, da, db, fa, fb, al) -> str:
    def side(lab, df, fixed):
        ran = df["ar_ran"].to_numpy(float) > 0 if "ar_ran" in df.columns \
            else np.zeros(len(df), bool)
        n_ran, n_fix = int(ran.sum()), int(fixed.sum())
        rate = 100.0 * n_fix / n_ran if n_ran else 0.0
        ff = df[fixed]
        first = (f"week {int(ff.iloc[0]['week'])} tow {ff.iloc[0]['tow']:.1f}"
                 if not ff.empty else "never")
        win = _finite(df["pos_err_3d_m"].to_numpy(float))[-30:]
        rms = _rms(win) if win.size else float("nan")
        return (f"{lab:<3}: fix {n_fix}/{n_ran} ({rate:.1f}%), first-fix {first}, "
                f"conv-30 3D RMS {rms:.3f} m")
    out = ["=== A/B Closed-Loop Held-Out Recovery ==="]
    out.append(side(pair.label_a, da, fa))
    out.append(side(pair.label_b, db, fb))
    out.append(f"matched epochs {al.n_match}; only-{pair.label_a} {al.n_only_a}, "
               f"only-{pair.label_b} {al.n_only_b}")
    return "\n".join(out)


# --------------------------------------------------------------------------- #
#  (b) epoch-status clock spread + sigma0
# --------------------------------------------------------------------------- #
def compare_epoch_status(pair: ComparePair) -> ViewerResult:
    """A/B the emitted-epoch clock median/std + median-σ̂0 B-A.

    Prefers ``network_epoch_status.csv`` (L1); falls back to ``network_trace.csv``
    ``rms_clock_m`` (masking ``solved==0`` rows) when the newer trace is absent.
    """
    es = "network_epoch_status.csv"
    if pair.both_have(es):
        return _epoch_status_from_es(pair)
    nt = "network_trace.csv"
    if pair.both_have(nt):
        return _epoch_status_from_nt(pair)
    raise ViewerSkip(f"neither {es} nor {nt} present on both sides")


def _emitted(df: pd.DataFrame) -> pd.DataFrame:
    d = df[df["disposition"].astype(str) == "emitted"].copy()
    return add_utc(d, sort=True) if not d.empty else d


def _epoch_status_from_es(pair: ComparePair) -> ViewerResult:
    csv = "network_epoch_status.csv"
    cols = ("week", "tow", "disposition", "sigma0", "clock_median_m", "clock_std_m")
    da = load_trace(pair.a.path(csv)); require_cols(da, cols, f"{csv} (A)")
    db = load_trace(pair.b.path(csv)); require_cols(db, cols, f"{csv} (B)")
    ea, eb = _emitted(da), _emitted(db)
    if ea.empty or eb.empty:
        raise ViewerSkip("network_epoch_status.csv has no 'emitted' rows on a side")

    ax_med = Axes(title="Emitted-epoch clock median |Δ|", ylabel="clock median (m)",
                  xlabel="GPS Time", legend=Legend(loc="best"))
    ax_med.traces = [
        Line(x=ea["utc"].to_numpy("datetime64[ns]"),
             y=ea["clock_median_m"].to_numpy(float), color=C_A, width=1.2,
             label=f"{pair.label_a} median"),
        Line(x=eb["utc"].to_numpy("datetime64[ns]"),
             y=eb["clock_median_m"].to_numpy(float), color=C_B, width=1.2,
             style="--", label=f"{pair.label_b} median"),
    ]
    ax_std = Axes(title="Emitted-epoch clock std", ylabel="clock std (m)",
                  xlabel="GPS Time", legend=Legend(loc="best"))
    ax_std.traces = [
        Line(x=ea["utc"].to_numpy("datetime64[ns]"),
             y=ea["clock_std_m"].to_numpy(float), color=C_A, width=1.0,
             label=f"{pair.label_a} std"),
        Line(x=eb["utc"].to_numpy("datetime64[ns]"),
             y=eb["clock_std_m"].to_numpy(float), color=C_B, width=1.0,
             style="--", label=f"{pair.label_b} std"),
    ]

    # median-σ̂0 B - A on matched emitted epochs.
    al = align_epochs(ea[["week", "tow", "sigma0"]],
                      eb[["week", "tow", "sigma0"]], ["sigma0"])
    ax_d = Axes(title=f"{pair.label_b} - {pair.label_a} σ̂0 (matched emitted epochs)",
                ylabel="Δ σ̂0", xlabel="GPS Time", legend=Legend(loc="best"))
    if not al.merged.empty:
        td = al.merged["utc"].to_numpy("datetime64[ns]")
        d = (al.merged["sigma0_b"].to_numpy(float)
             - al.merged["sigma0_a"].to_numpy(float))
        ax_d.traces = [Line(x=td, y=d, color=C_DELTA, width=1.2,
                            label=f"{pair.label_b}-{pair.label_a} σ̂0"),
                       HLine(y=0.0, color=C_REF, style="-")]
    else:
        ax_d.traces = [TextBox(x=0.5, y=0.5, text="no matched emitted epochs",
                               ha="center", va="center")]

    fig = Figure(name="AB epoch status",
                 title="A/B emitted-epoch clock spread + σ̂0",
                 axes=[ax_med, ax_std, ax_d], width=12.0, height=9.5, link_x=False)
    summary = _es_summary(pair, ea, eb, al, source="network_epoch_status.csv")
    return ViewerResult(figures=[fig], summary=summary)


def _epoch_status_from_nt(pair: ComparePair) -> ViewerResult:
    csv = "network_trace.csv"
    cols = ("week", "tow", "solved", "rms_clock_m")
    da = load_trace(pair.a.path(csv)); require_cols(da, cols, f"{csv} (A)")
    db = load_trace(pair.b.path(csv)); require_cols(db, cols, f"{csv} (B)")
    da = add_utc(da, sort=True); db = add_utc(db, sort=True)

    def masked(df):
        solved = df["solved"].to_numpy(float) > 0
        return np.where(solved, df["rms_clock_m"].to_numpy(float), np.nan)

    ax = Axes(title="Solved-epoch clock RMS (network_trace fallback)",
              ylabel="clock RMS (m)", xlabel="GPS Time", legend=Legend(loc="best"))
    ax.traces = [
        Line(x=da["utc"].to_numpy("datetime64[ns]"), y=masked(da), color=C_A,
             width=1.2, label=f"{pair.label_a} rms"),
        Line(x=db["utc"].to_numpy("datetime64[ns]"), y=masked(db), color=C_B,
             width=1.2, style="--", label=f"{pair.label_b} rms"),
    ]
    fig = Figure(name="AB epoch status (fallback)",
                 title="A/B solved-epoch clock RMS (network_trace fallback)",
                 axes=[ax], width=12.0, height=4.5, link_x=False)
    na = int((da["solved"].to_numpy(float) > 0).sum())
    nb = int((db["solved"].to_numpy(float) > 0).sum())
    summary = "\n".join([
        "=== A/B Epoch Status (network_trace.csv fallback) ===",
        f"{pair.label_a:<3}: {na} solved epochs, clock RMS median "
        f"{np.nanmedian(masked(da)):.3f} m",
        f"{pair.label_b:<3}: {nb} solved epochs, clock RMS median "
        f"{np.nanmedian(masked(db)):.3f} m",
    ])
    return ViewerResult(figures=[fig], summary=summary)


def _es_summary(pair, ea, eb, al, source) -> str:
    def side(lab, d):
        med = _finite(d["clock_median_m"].to_numpy(float))
        s0 = _finite(d["sigma0"].to_numpy(float))
        return (f"{lab:<3}: {len(d)} emitted, clock-median med "
                f"{np.median(med) if med.size else float('nan'):.4f} m, "
                f"σ̂0 med {np.median(s0) if s0.size else float('nan'):.3f}")
    out = [f"=== A/B Emitted-Epoch Clock Spread ({source}) ==="]
    out.append(side(pair.label_a, ea))
    out.append(side(pair.label_b, eb))
    out.append(f"matched emitted {al.n_match}; only-{pair.label_a} {al.n_only_a}, "
               f"only-{pair.label_b} {al.n_only_b}")
    return "\n".join(out)


# --------------------------------------------------------------------------- #
#  (c) flag-rate table
# --------------------------------------------------------------------------- #
def compare_flag_rates(pair: ComparePair) -> ViewerResult:
    """A/B the per-QC-tier realized flag rates (a TextBox table, >3x called out)."""
    csv = "network_flag_rates.csv"
    pair.require_both(csv)
    cols = ("tier", "label", "rate")
    da = load_trace(pair.a.path(csv)); require_cols(da, cols, f"{csv} (A)")
    db = load_trace(pair.b.path(csv)); require_cols(db, cols, f"{csv} (B)")

    ra = {str(r["tier"]): (str(r["label"]), float(r["rate"])) for _, r in da.iterrows()}
    rb = {str(r["tier"]): (str(r["label"]), float(r["rate"])) for _, r in db.iterrows()}
    tiers = list(dict.fromkeys(list(ra.keys()) + list(rb.keys())))

    rows = [f"{'tier':<16}{'rate_'+pair.label_a:>14}{'rate_'+pair.label_b:>14}"
            f"{'ratio B/A':>12}"]
    rows.append("-" * 56)
    called_out: List[str] = []
    for t in tiers:
        rate_a = ra.get(t, (None, float("nan")))[1]
        rate_b = rb.get(t, (None, float("nan")))[1]
        if rate_a and np.isfinite(rate_a) and rate_a > 0:
            ratio = rate_b / rate_a
            rstr = f"{ratio:.2f}x"
        elif rate_b and rate_b > 0:
            ratio = float("inf")
            rstr = "new"
        else:
            ratio = float("nan")
            rstr = "-"
        flag = "  <<" if (np.isfinite(ratio) and ratio > 3.0) or rstr == "new" else ""
        rows.append(f"{t:<16}{rate_a:>14.3e}{rate_b:>14.3e}{rstr:>12}{flag}")
        if flag:
            called_out.append(f"{t} ({rstr})")

    lines = "\n".join(rows)
    ax = Axes(title="QC tier flag-rate comparison", axis_off=True,
              x_is_datetime=False)
    ax.traces = [TextBox(x=0.01, y=0.98, text=lines, ha="left", va="top",
                         mono=True, boxed=True, fontsize=9.0)]
    fig = Figure(name="AB flag rates", title="A/B QC tier flag rates",
                 axes=[ax], width=11.0, height=max(3.0, 0.4 * (len(tiers) + 4)))

    summary = ["=== A/B QC Tier Flag Rates ===", lines, ""]
    if called_out:
        summary.append("Called out (>3x or new): " + ", ".join(called_out))
    else:
        summary.append("No tier crossed the 3x line.")
    return ViewerResult(figures=[fig], summary="\n".join(summary))


# --------------------------------------------------------------------------- #
#  (d) per-station ZWD std
# --------------------------------------------------------------------------- #
def compare_zwd(pair: ComparePair) -> ViewerResult:
    """A/B per-station ZWD formal-σ std + a final-window B-A delta table."""
    csv = "network_zwd.csv"
    pair.require_both(csv)
    cols = ("week", "tow", "station", "zwd_m", "std_m")
    da = load_trace(pair.a.path(csv)); require_cols(da, cols, f"{csv} (A)")
    db = load_trace(pair.b.path(csv)); require_cols(db, cols, f"{csv} (B)")
    da = add_utc(da, sort=True); db = add_utc(db, sort=True)

    def per_station_std(df):
        return (df.groupby(df["station"].astype(str))["std_m"]
                .apply(lambda s: _std(s.to_numpy(float))))
    sa = per_station_std(da)
    sb = per_station_std(db)
    stations = sorted(set(sa.index) | set(sb.index))

    # Overlay: per-station std as a rank scatter (station index on x, numeric axis).
    xa = np.arange(len(stations), dtype=float)
    ya = np.array([sa.get(s, np.nan) for s in stations], dtype=float)
    yb = np.array([sb.get(s, np.nan) for s in stations], dtype=float)
    ax = Axes(title="Per-station ZWD formal-σ std (run-long)",
              ylabel="std of ZWD σ (m)", xlabel="station rank",
              x_is_datetime=False, legend=Legend(loc="best"))
    ax.traces = [
        Scatter(x=xa, y=ya, color=C_A, marker="o", size=20,
                label=f"{pair.label_a}"),
        Scatter(x=xa, y=yb, color=C_B, marker="^", size=20,
                label=f"{pair.label_b}"),
    ]

    # Final-window delta table (last 30 matched epochs, per station).
    rows = [f"{'station':<12}{'std_'+pair.label_a:>12}{'std_'+pair.label_b:>12}"
            f"{'B-A':>12}"]
    rows.append("-" * 48)
    for s in stations:
        va = sa.get(s, float("nan"))
        vb = sb.get(s, float("nan"))
        rows.append(f"{s:<12}{va:>12.4f}{vb:>12.4f}{(vb - va):>12.4f}")
    table = "\n".join(rows)
    ax_t = Axes(title="Per-station ZWD-σ std delta", axis_off=True,
                x_is_datetime=False)
    ax_t.traces = [TextBox(x=0.01, y=0.98, text=table, ha="left", va="top",
                           mono=True, boxed=True, fontsize=8.5)]

    fig = Figure(name="AB zwd", title="A/B per-station ZWD stability",
                 axes=[ax, ax_t], width=11.0,
                 height=max(6.0, 0.3 * (len(stations) + 6)))
    summary = "\n".join([
        "=== A/B Per-Station ZWD Stability ===",
        f"stations: {pair.label_a} {len(sa)}, {pair.label_b} {len(sb)}, "
        f"shared {len(set(sa.index) & set(sb.index))}",
        table,
    ])
    return ViewerResult(figures=[fig], summary=summary)


# --------------------------------------------------------------------------- #
#  (e) network satellite-clock product spread
# --------------------------------------------------------------------------- #
def compare_network_clock(pair: ComparePair) -> ViewerResult:
    """A/B the emitted satellite-clock product: per-epoch median(|clk|)/std/worst-sat."""
    csv = "network_solution.csv"
    pair.require_both(csv)
    cols = ("week", "tow", "sat", "kind", "value_m")
    da = load_trace(pair.a.path(csv)); require_cols(da, cols, f"{csv} (A)")
    db = load_trace(pair.b.path(csv)); require_cols(db, cols, f"{csv} (B)")

    def clk(df):
        d = df[df["kind"].astype(str) == "CLK"].copy()
        if d.empty:
            return d
        d["sat"] = d["sat"].astype(str)
        return add_utc(d, sort=True)
    ca, cb = clk(da), clk(db)
    if ca.empty or cb.empty:
        raise ViewerSkip("network_solution.csv has no CLK rows on a side")

    def per_epoch(d):
        """Return (t, median_abs, std, worst_sat_series-by-epoch) aggregates."""
        g = d.groupby(["week", "tow"])
        recs = []
        for (wk, tow), sub in g:
            v = sub["value_m"].to_numpy(float)
            av = np.abs(v)
            fin = np.isfinite(av)
            if not fin.any():
                continue
            i_worst = int(np.nanargmax(np.where(fin, av, -np.inf)))
            recs.append((wk, tow, float(np.median(av[fin])),
                         _std(v), str(sub["sat"].to_numpy()[i_worst]),
                         float(av[i_worst])))
        r = pd.DataFrame(recs, columns=["week", "tow", "med_abs", "std",
                                        "worst_sat", "worst_abs"])
        return add_utc(r, sort=True) if not r.empty else r
    pa, pb = per_epoch(ca), per_epoch(cb)

    ax_med = Axes(title="Emitted sat-clock median|Δ|", ylabel="median |clk| (m)",
                  xlabel="GPS Time", legend=Legend(loc="best"))
    ax_med.traces = [
        Line(x=pa["utc"].to_numpy("datetime64[ns]"), y=pa["med_abs"].to_numpy(float),
             color=C_A, width=1.2, label=f"{pair.label_a}"),
        Line(x=pb["utc"].to_numpy("datetime64[ns]"), y=pb["med_abs"].to_numpy(float),
             color=C_B, width=1.2, style="--", label=f"{pair.label_b}"),
    ]
    ax_std = Axes(title="Emitted sat-clock std", ylabel="clock std (m)",
                  xlabel="GPS Time", legend=Legend(loc="best"))
    ax_std.traces = [
        Line(x=pa["utc"].to_numpy("datetime64[ns]"), y=pa["std"].to_numpy(float),
             color=C_A, width=1.0, label=f"{pair.label_a}"),
        Line(x=pb["utc"].to_numpy("datetime64[ns]"), y=pb["std"].to_numpy(float),
             color=C_B, width=1.0, style="--", label=f"{pair.label_b}"),
    ]
    fig = Figure(name="AB network clock",
                 title="A/B network satellite-clock product spread",
                 axes=[ax_med, ax_std], width=12.0, height=8.0, link_x=False)

    def worst(p):
        if p.empty:
            return "-", float("nan")
        i = int(np.nanargmax(p["worst_abs"].to_numpy(float)))
        return str(p["worst_sat"].iloc[i]), float(p["worst_abs"].iloc[i])
    wa_sat, wa_val = worst(pa)
    wb_sat, wb_val = worst(pb)
    summary = "\n".join([
        "=== A/B Network Satellite-Clock Product ===",
        f"{pair.label_a:<3}: {len(pa)} emitted epochs, median|clk| med "
        f"{np.nanmedian(pa['med_abs'].to_numpy(float)) if not pa.empty else float('nan'):.4f} m, "
        f"worst-sat {wa_sat} ({wa_val:.4f} m)",
        f"{pair.label_b:<3}: {len(pb)} emitted epochs, median|clk| med "
        f"{np.nanmedian(pb['med_abs'].to_numpy(float)) if not pb.empty else float('nan'):.4f} m, "
        f"worst-sat {wb_sat} ({wb_val:.4f} m)",
    ])
    return ViewerResult(figures=[fig], summary=summary)


# --------------------------------------------------------------------------- #
#  (f) scorecard -- diff of the other comparators' summary numbers
# --------------------------------------------------------------------------- #
def compare_scorecard(pair: ComparePair) -> ViewerResult:
    """A text-panel scorecard: re-run every other comparator and diff headline numbers.

    Runs the (a)-(e) comparators, capturing each one's ``summary`` (or a
    skip reason), and lays them out as a single monospaced panel -- the one place
    a reader sees run A vs run B side-by-side without scrolling every figure.
    """
    lines: List[str] = [f"=== A/B Scorecard: {pair.label_a} vs {pair.label_b} ==="]
    lines.append(f"  {pair.label_a} log dir: {pair.a.log_dir}")
    lines.append(f"  {pair.label_b} log dir: {pair.b.log_dir}")
    lines.append("")
    for name, fn in _SCORECARD_SOURCES:
        try:
            res = fn(pair)
            lines.append(res.summary if res.summary else f"[{name}] (no summary)")
        except ViewerSkip as sk:
            lines.append(f"[{name}] skipped: {sk}")
        except Exception as exc:  # pragma: no cover - defensive
            lines.append(f"[{name}] error: {exc}")
        lines.append("")
    text = "\n".join(lines)
    ax = Axes(title="", axis_off=True, x_is_datetime=False)
    ax.traces = [TextBox(x=0.01, y=0.99, text=text, ha="left", va="top",
                         mono=True, fontsize=8.5)]
    n_lines = text.count("\n") + 1
    fig = Figure(name="AB scorecard", title="A/B scorecard",
                 axes=[ax], width=12.0, height=max(4.0, 0.18 * n_lines))
    return ViewerResult(figures=[fig], summary=text)


# --------------------------------------------------------------------------- #
#  Ordered comparator registry (NOT the single-ctx viewers REGISTRY)
# --------------------------------------------------------------------------- #
@dataclass
class Comparator:
    """A named A/B comparator (the analogue of a single-ctx :class:`Viewer`)."""
    name: str
    title: str
    build: Callable[[ComparePair], ViewerResult]


COMPARATORS: List[Comparator] = [
    Comparator("cl", "A/B closed-loop recovery", compare_closed_loop),
    Comparator("epoch", "A/B emitted-epoch clock + σ̂0", compare_epoch_status),
    Comparator("flags", "A/B QC flag rates", compare_flag_rates),
    Comparator("zwd", "A/B per-station ZWD", compare_zwd),
    Comparator("clock", "A/B network sat-clock product", compare_network_clock),
    Comparator("scorecard", "A/B scorecard", compare_scorecard),
]

# The comparators the scorecard rolls up (everything but itself).
_SCORECARD_SOURCES = [(c.name, c.build) for c in COMPARATORS
                      if c.name != "scorecard"]


@dataclass
class CompareRun:
    """Outcome of running the comparators over one :class:`ComparePair`."""
    ran: List[Tuple[Comparator, ViewerResult]]
    skipped: List[Tuple[str, str]]
    failed: List[Tuple[str, str]]


def run_comparators(pair: ComparePair, only: Optional[List[str]] = None) -> CompareRun:
    """Run every (or an ``only`` subset of) comparator over ``pair``.

    A comparator that raises :class:`ViewerSkip` becomes a ``(name, reason)`` skip;
    any other exception becomes a ``(name, error)`` failure -- neither aborts the
    rest, mirroring the single-ctx :func:`gnssviz.run_viewers`.
    """
    want = set(only) if only else None
    ran: List[Tuple[Comparator, ViewerResult]] = []
    skipped: List[Tuple[str, str]] = []
    failed: List[Tuple[str, str]] = []
    for comp in COMPARATORS:
        if want is not None and comp.name not in want:
            continue
        try:
            ran.append((comp, comp.build(pair)))
        except ViewerSkip as sk:
            skipped.append((comp.name, str(sk)))
        except Exception as exc:  # pragma: no cover - defensive
            failed.append((comp.name, f"{type(exc).__name__}: {exc}"))
    return CompareRun(ran=ran, skipped=skipped, failed=failed)

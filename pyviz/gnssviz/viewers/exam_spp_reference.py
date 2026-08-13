"""Part-B SPP reference runs — single-frequency broadcast vs ionosphere-free precise.

The only *graded* part of the exam whose reference solution the processor cannot
produce: ``gnss_processor`` has no WLS/SPP estimator mode, so the two Part-B legs
are solved author-side by ``gnss_simulator spp-reference``, which reads the
shipped RINEX back through the production ``RinexParser`` (the student's view) and
runs an epoch-wise elevation-weighted Gauss-Newton WLS.  Its outputs
(``truth_spp.csv`` per epoch + ``truth_spp_summary.json`` stats) are pure numbers;
this viewer is their dashboard.  It is truth-only — no ``state_trace`` join — so it
draws on a truth dir alone and auto-skips when ``--truth-dir`` is unset.

Panels:

  (a) 3D error vs time for both legs on a log axis, with each leg's median;
  (b) the hourly-mean ladder — leg 1 tracks the storm's VTEC, leg 2 is flat
      (code IF cancels first-order iono; scintillation is phase-only);
  (c) signed E/N/U on ONE shared scale — leg 1's error is almost entirely Up
      (the iono projects onto the vertical), leg 2's is not;
  (d) the four paired A/B ablations (Klobuchar, TGD, broadcast-vs-precise orbits,
      OSB) as per-epoch differences about their mean — the rubric's credit items.
      **Figure off by default** (``show_ablations``); the numbers stay in the
      read-out;
  (d2) formal σ3D vs realised 3D error as a per-leg **scatter** on log-log axes,
      with a 1:1 reference and each leg's correlation — the covariance-honesty
      exhibit;
  (e) receiver clock: the leg-1-minus-leg-2 ``c·dt`` difference, i.e. the
      common-mode ionosphere the single-frequency solution hides in its clock.
      **Figure off by default** (``show_clock_difference``); the median stays in
      the read-out. The "each leg about its own median" panel still draws;
  (f) satellites used + PDOP per leg — leg 1 screens the broadcast-unhealthy G01,
      leg 2 keeps it (the precise products carry it).

Statistics are computed from ``truth_spp.csv`` itself, since the grading bands are
keyed to *that file* at the student's chosen epochs.  ``truth_spp_summary.json``
is read only for the model ledger (estimator / modeled / unmodeled prose) and is
cross-checked against the computed medians — a mismatch means the summary is
stale relative to the CSV and says so in the read-out.

Pairing note (matches the C++ ``paired_mean_delta_3d_m``): the precise-orbit twin
is the **no-TGD** variant, so the broadcast orbit/clock ablation is
``l1_notgd − l1_prec``, not ``l1 − l1_prec``.
"""

from __future__ import annotations

import json
import os

import numpy as np

from ..figures import Axes, Figure, HLine, Legend, Line, Scatter, TextBox
from ..io import first_col, gps_to_datetime
from .base import Viewer, ViewerResult, ViewerSkip
from . import exam_common as ec

# Leg colours, stable across every panel: leg 1 warm (the degraded real-time
# stack), leg 2 cool (the precise dual-frequency one).
_C_L1 = (0.80, 0.25, 0.12)
_C_IF = (0.10, 0.35, 0.75)
_C_E = (0.00, 0.00, 1.00)
_C_N = (0.00, 0.60, 0.00)
_C_U = (1.00, 0.00, 0.00)
_C_REF = (0.45, 0.45, 0.45)

#: Ablation series -> (label, colour).  Order is the read-out order.  Colours are
#: kept clear of the storm-window amber (``ec.STORM_COLOR``) so an ablation trace
#: is never mistaken for a storm marker.
_ABLATIONS = (
    ("klobuchar", "Klobuchar off − on", "l1_noklob_3d_m", "l1_3d_m", (0.75, 0.05, 0.25)),
    ("tgd", "TGD off − on", "l1_notgd_3d_m", "l1_3d_m", (0.45, 0.20, 0.65)),
    ("orbits", "broadcast − precise orbits", "l1_notgd_3d_m", "l1_prec_3d_m", (0.20, 0.55, 0.30)),
    ("osb", "OSB off − on", "if_noosb_3d_m", "if_3d_m", (0.10, 0.45, 0.70)),
)

_MS_PER_DAY = 86400 * 1000
_MS_PER_HOUR = 3600 * 1000


def _summary_json(ctx):
    """``truth_spp_summary.json`` from the truth dir (``None`` when absent)."""
    p = ctx.truth_path("truth_spp_summary.json") if ctx.truth_dir else None
    if not p or not os.path.exists(p) or os.path.getsize(p) <= 0:
        return None
    try:
        with open(p, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except Exception:
        return None


def _hour_of_day(week, tow):
    """Integer hour-of-day for each epoch, on the CSV's own (GPS) time scale.

    Rounds ``tow`` to whole milliseconds *before* the hour division, matching the
    C++ bucketing: a calendar->tow conversion can land an ulp below an exact hour
    and would otherwise shift every hourly label by one.
    """
    ms = np.rint(np.asarray(tow, dtype=float) * 1000.0).astype(np.int64)
    return ((ms % _MS_PER_DAY) // _MS_PER_HOUR).astype(int)


def _hourly_mean(hours, values):
    """``(hour, mean, n)`` per populated hour, ascending."""
    v = np.asarray(values, dtype=float)
    out = []
    for h in np.unique(hours):
        sel = (hours == h) & np.isfinite(v)
        if sel.any():
            out.append((int(h), float(v[sel].mean()), int(sel.sum())))
    return out


def _full_hours(*series, frac: float = 0.5) -> set:
    """Hours whose bucket is at least ``frac`` of the modal bucket size.

    Keyed off the modal count rather than a hardcoded epoch count so it holds at
    any sample interval (30 s -> 120/hour, 1 s -> 3600/hour).  Falls back to
    every populated hour if the counts are too irregular to find a mode.
    """
    counts = [n for s in series for _h, _m, n in s]
    if not counts:
        return set()
    modal = max(set(counts), key=counts.count)
    keep = {h for s in series for h, _m, n in s if n >= frac * modal}
    return keep or {h for s in series for h, _m, _n in s}


def _stats(v):
    """median / mean / p90 / max of a series, NaN-safe (``None`` if all-NaN)."""
    a = np.asarray(v, dtype=float)
    a = a[np.isfinite(a)]
    if a.size == 0:
        return None
    return dict(median=float(np.median(a)), mean=float(a.mean()),
                p90=float(np.percentile(a, 90)), max=float(a.max()), n=int(a.size))


def _need(df, *names):
    """First present column as a float array, or raise :class:`ViewerSkip`."""
    col = first_col(df, *names)
    if col is None:
        raise ViewerSkip(f"truth_spp.csv lacks column {names[0]}")
    return col.to_numpy(float)


def _rank(a):
    """Ordinal ranks of a 1-D array (ties broken by position).

    Enough for a Spearman ρ on continuous per-epoch series -- exact ties are
    vanishingly unlikely in metre-scale floats, and this keeps the viewer layer
    free of a scipy dependency.
    """
    order = np.argsort(a, kind="stable")
    r = np.empty(a.size, dtype=float)
    r[order] = np.arange(a.size, dtype=float)
    return r


def _corr(x, y):
    """(Pearson r on log10, Spearman ρ, n) for a strictly-positive pair."""
    fin = np.isfinite(x) & np.isfinite(y) & (x > 0) & (y > 0)
    n = int(fin.sum())
    if n < 3:
        return float("nan"), float("nan"), n
    lx, ly = np.log10(x[fin]), np.log10(y[fin])
    def _p(a, b):
        sa, sb = a.std(), b.std()
        return float(((a - a.mean()) * (b - b.mean())).mean() / (sa * sb)) if sa > 0 and sb > 0 \
            else float("nan")
    return _p(lx, ly), _p(_rank(lx), _rank(ly)), n


def build_exam_spp_reference(ctx, *, show_ablations: bool = False,
                             show_clock_difference: bool = False) -> ViewerResult:
    """Part-B reference dashboard.

    ``show_ablations`` / ``show_clock_difference`` default **off** (professor
    review 2026-08-06: the dashboard was too crowded).  Both are figure-level
    switches only -- the ablation table and the clock-difference median stay in
    the text read-out either way, so nothing is lost from the grader record.
    Turn them back on by binding the partial, the way ``run_report._registry_for``
    already does for the standardized residual panels.
    """
    spp = ec.require_truth(ctx, "truth_spp.csv")
    if "week" not in spp.columns or "tow_s" not in spp.columns:
        raise ViewerSkip("truth_spp.csv lacks week/tow_s")

    t = gps_to_datetime(spp["week"], spp["tow_s"])
    hours = _hour_of_day(spp["week"], spp["tow_s"])

    l1_3d = _need(spp, "l1_3d_m")
    if_3d = _need(spp, "if_3d_m")
    storm = ec.storm_vlines(ctx)
    figures = []

    # ---------------------------------------------------------------- (a) 3D
    # Log y: the two legs live a decade apart (≈11 m vs ≈2.7 m median) and the
    # ablations reach 35 m, so a linear axis buries leg 2 on the floor.
    s_l1, s_if = _stats(l1_3d), _stats(if_3d)
    axa = Axes(title="Part-B 3D position error — leg 1 (single-freq broadcast) vs leg 2 (IF + precise)",
               xlabel="GPS Time", ylabel="3D error (m, log)", yscale="log",
               legend=Legend(loc="best"))
    axa.traces = [
        Line(x=t, y=np.where(l1_3d > 0, l1_3d, np.nan), color=_C_L1, width=1.1,
             label="leg 1: C1C + brdc + TGD + Klobuchar"),
        Line(x=t, y=np.where(if_3d > 0, if_3d, np.nan), color=_C_IF, width=1.1,
             label="leg 2: IF(C1C,C2W) + COD SP3/CLK + OSB"),
    ]
    # Medians go in a corner TextBox, not as inline HLine labels: leg 2's median
    # sits inside its own dense noise band, where inline text is unreadable.
    if s_l1:
        axa.traces.append(HLine(y=s_l1["median"], color=_C_L1, style=":"))
    if s_if:
        axa.traces.append(HLine(y=s_if["median"], color=_C_IF, style=":"))
    if s_l1 and s_if:
        axa.traces.append(TextBox(
            x=0.02, y=0.97, mono=True, boxed=True,
            text=(f"leg 1 median {s_l1['median']:6.2f} m   p90 {s_l1['p90']:5.2f}\n"
                  f"leg 2 median {s_if['median']:6.2f} m   p90 {s_if['p90']:5.2f}\n"
                  f"ratio        {s_l1['median'] / s_if['median']:6.2f}x")))
    axa.traces += storm
    figures.append(Figure(name="Part-B 3D error", axes=[axa], width=13, height=4.4))

    # ------------------------------------------------------- (b) hourly ladder
    # Step-drawn so each bar spans its hour; this is the panel that shows leg 1
    # tracking the VTEC while leg 2 ignores the storm entirely.
    h1 = _hourly_mean(hours, l1_3d)
    h2 = _hourly_mean(hours, if_3d)
    if h1 and h2:
        # An arc that starts/ends mid-hour leaves a stub bucket (the hero's last
        # hour holds ONE epoch).  A 1-sample mean is not an hourly mean: quoting
        # the range over it turned leg 2's genuinely flat 2.5-3.4 m ladder into a
        # spurious "2.5 -> 5.2 m". Stub buckets stay on the plot, drawn dashed and
        # excluded from the quoted range.
        full = _full_hours(h1, h2)
        axb = Axes(title="Hourly mean 3D error — leg 1 tracks the storm's VTEC, leg 2 is flat",
                   xlabel="Hour of day (GPS scale)", ylabel="mean 3D error (m)",
                   x_is_datetime=False, legend=Legend(loc="best"))
        axb.traces = [
            Line(x=[r[0] for r in h1], y=[r[1] for r in h1], color=_C_L1, width=1.8,
                 label="leg 1 hourly mean"),
            Line(x=[r[0] for r in h2], y=[r[1] for r in h2], color=_C_IF, width=1.8,
                 label="leg 2 hourly mean"),
        ]
        r1 = [m for h, m, n in h1 if h in full]
        r2 = [m for h, m, n in h2 if h in full]
        if r1 and r2:
            stub = sorted({h for h, _m, _n in h1 + h2} - full)
            note = (f"\n(hours {', '.join(map(str, stub))} partial — excluded)"
                    if stub else "")

            def _span(rows):
                lo, hi = min(rows), max(rows)
                # A sub-hour arc collapses to one bucket; guard the ratio rather
                # than dividing by a zero/absent spread.
                ratio = f"×{hi / lo:.1f}" if lo > 0 else "n/a"
                return f"{lo:.1f} → {hi:.1f} m  ({ratio})"

            axb.traces.append(TextBox(
                x=0.02, y=0.97, mono=True, boxed=True,
                text=(f"leg 1  {_span(r1)}\nleg 2  {_span(r2)}" + note)))
        figures.append(Figure(name="Hourly ladder", axes=[axb], width=13, height=3.8))

    # -------------------------------------------------------------- (c) E/N/U
    # Both legs on ONE shared symmetric scale — the whole point is that leg 1's
    # error is a vertical bias and leg 2's is not; per-axes autoscaling would
    # hide exactly that.
    have_enu = all(c in spp.columns for c in
                   ("l1_e_m", "l1_n_m", "l1_u_m", "if_e_m", "if_n_m", "if_u_m"))
    if have_enu:
        l1e, l1n, l1u = (spp["l1_e_m"].to_numpy(float), spp["l1_n_m"].to_numpy(float),
                         spp["l1_u_m"].to_numpy(float))
        ife, ifn, ifu = (spp["if_e_m"].to_numpy(float), spp["if_n_m"].to_numpy(float),
                         spp["if_u_m"].to_numpy(float))
        lim = float(np.nanmax(np.abs(np.concatenate([l1e, l1n, l1u, ife, ifn, ifu]))))
        lim = 1.05 * lim if np.isfinite(lim) and lim > 0 else 1.0
        axc1 = Axes(title="Leg 1 (single-freq broadcast) — signed E / N / U",
                    xlabel="", ylabel="error (m)", ylim=(-lim, lim),
                    legend=Legend(loc="best"))
        axc1.traces = [Line(x=t, y=l1e, color=_C_E, width=0.9, label="E"),
                       Line(x=t, y=l1n, color=_C_N, width=0.9, label="N"),
                       Line(x=t, y=l1u, color=_C_U, width=1.2, label="U"),
                       HLine(y=0.0, color=_C_REF, style="-")] + storm
        axc2 = Axes(title="Leg 2 (IF + precise) — signed E / N / U (same scale)",
                    xlabel="GPS Time", ylabel="error (m)", ylim=(-lim, lim),
                    legend=Legend(loc="best"))
        axc2.traces = [Line(x=t, y=ife, color=_C_E, width=0.9, label="E"),
                       Line(x=t, y=ifn, color=_C_N, width=0.9, label="N"),
                       Line(x=t, y=ifu, color=_C_U, width=1.2, label="U"),
                       HLine(y=0.0, color=_C_REF, style="-")] + storm
        figures.append(Figure(name="ENU by leg", axes=[axc1, axc2], width=13, height=7.0))

    # ------------------------------------------------------- (d) A/B ablations
    # Statistics are always computed (they feed the read-out); the FIGURE is
    # opt-in — four overlaid difference traces are the densest panel here and the
    # numbers below carry the same information.
    ab_rows = []
    axd = Axes(title="Paired A/B ablations (per-epoch Δ3D; dotted = mean)",
               xlabel="GPS Time", ylabel="Δ 3D error (m)", legend=Legend(loc="best"))
    axd.traces = []
    for _key, label, off_col, on_col, color in _ABLATIONS:
        if off_col not in spp.columns or on_col not in spp.columns:
            continue
        d = spp[off_col].to_numpy(float) - spp[on_col].to_numpy(float)
        good = d[np.isfinite(d)]
        if good.size == 0:
            continue
        mean = float(good.mean())
        ab_rows.append((label, mean, float(np.median(good)),
                        float(np.percentile(good, 5)), float(np.percentile(good, 95))))
        axd.traces.append(Line(x=t, y=d, color=color, width=0.9,
                               label=f"{label} (mean {mean:+.2f} m)"))
        axd.traces.append(HLine(y=mean, color=color, style=":"))
    if show_ablations and axd.traces:
        axd.traces.append(HLine(y=0.0, color=_C_REF, style="-"))
        axd.traces += storm
        figures.append(Figure(name="A/B ablations", axes=[axd], width=13, height=4.6))

    # ------------------------------------ (d2) formal sigma vs realised error
    # Part B asks students to document a stochastic model at three epochs of
    # their choosing, so the reference publishes what each leg's model actually
    # yields per epoch.  The punchline is the RATIO: leg 2's formal sigma
    # describes its error (the iono cancels, so what is left really is noise),
    # while leg 1's does not -- its error is a systematic the covariance cannot
    # see, so it reports a SMALLER sigma than leg 2 while being several times
    # worse.  That is the Part-B/D lesson in the covariance domain.
    # Drawn as a SCATTER of realised error against formal sigma (log-log), one
    # cloud per leg, rather than four overlaid time series (professor request,
    # 2026-08-06).  Two independent things are readable off it, and they are not
    # the same question:
    #
    #   * DISTANCE FROM 1:1 = is the covariance honest.  Leg 2 sits on the line
    #     (median err/sigma3D 0.91x); leg 1 sits a factor ~4.5 ABOVE it, because
    #     its error is an ionospheric systematic the stochastic model cannot see.
    #     This is the Part-B/D lesson and it is what the ratio in the box states.
    #   * CORRELATION = does sigma track the error epoch to epoch.  Measured on
    #     the hero: leg 1 r_log +0.37, leg 2 +0.03 -- i.e. WEAK for both, and
    #     weakest for the honest leg.  That is not a contradiction: leg 2's sigma
    #     is nearly constant (geometry-driven) while its error is noise, so there
    #     is little covariation to find.  A leg can be perfectly calibrated in
    #     magnitude and still uncorrelated in time.
    #
    # Correlation is reported per leg both ways -- Pearson on log10 (the plotted
    # coordinates) and Spearman rho (rank, outlier-robust).
    sig_rows = []
    if all(c in spp.columns for c in ("l1_sig_e_m", "l1_sig_n_m", "l1_sig_u_m")):
        axg = Axes(title="Formal σ3D (a-posteriori scaled) vs realised 3D error, per leg",
                   xlabel="formal σ3D (m, log)", ylabel="realised 3D error (m, log)",
                   xscale="log", yscale="log", x_is_datetime=False,
                   legend=Legend(loc="best"))
        axg.traces = []
        corr_rows, lo, hi = [], np.inf, -np.inf
        for pre, lbl, color in (("l1", "leg 1", _C_L1), ("if", "leg 2", _C_IF)):
            se = spp[f"{pre}_sig_e_m"].to_numpy(float)
            sn = spp[f"{pre}_sig_n_m"].to_numpy(float)
            su = spp[f"{pre}_sig_u_m"].to_numpy(float)
            e3 = spp[f"{pre}_3d_m"].to_numpy(float)
            s3 = np.sqrt(se ** 2 + sn ** 2 + su ** 2)
            fin = np.isfinite(e3) & np.isfinite(s3) & (s3 > 0) & (e3 > 0)
            ratio = float(np.median(e3[fin] / s3[fin])) if fin.any() else float("nan")
            sig_rows.append((lbl, float(np.nanmedian(np.hypot(se, sn))),
                             float(np.nanmedian(su)), float(np.nanmedian(e3)), ratio))
            r_log, rho, n = _corr(s3, e3)
            corr_rows.append((lbl, r_log, rho, n, ratio))
            if fin.any():
                lo = min(lo, float(np.min(s3[fin])), float(np.min(e3[fin])))
                hi = max(hi, float(np.max(s3[fin])), float(np.max(e3[fin])))
            # Small, translucent markers: ~1400 epochs per leg, and the shape of
            # the cloud is the message, not any individual epoch.
            axg.traces.append(Scatter(
                x=s3[fin], y=e3[fin], color=color, size=7, alpha=0.30,
                label=f"{lbl}  (r_log {r_log:+.2f}, ρ {rho:+.2f}, n {n})"))
        # 1:1 reference — "the covariance is honest" — spanning the pooled range.
        if np.isfinite(lo) and np.isfinite(hi) and lo > 0 and hi > lo:
            axg.traces.append(Line(x=[lo, hi], y=[lo, hi], color=_C_REF, style="--",
                                   width=1.2, label="1:1 (error = σ3D)"))
        if corr_rows:
            axg.traces.append(TextBox(
                x=0.02, y=0.97, mono=True, boxed=True,
                text="\n".join(f"{l:<6} err/σ3D median {rt:5.2f}x   r_log {rl:+.2f}   ρ {rp:+.2f}"
                               for l, rl, rp, _n, rt in corr_rows)
                     + "\n(on the 1:1 line = covariance honest in MAGNITUDE;"
                       "\n r/ρ = does σ track the error epoch to epoch)"))
        figures.append(Figure(name="Formal sigma vs error", axes=[axg], width=13, height=5.6))

    # ------------------------------------------------------------- (e) clock
    cdt_med = None
    if "l1_cdt_m" in spp.columns and "if_cdt_m" in spp.columns:
        c1 = spp["l1_cdt_m"].to_numpy(float)
        c2 = spp["if_cdt_m"].to_numpy(float)
        dc = c1 - c2
        fin = dc[np.isfinite(dc)]
        cdt_med = float(np.median(fin)) if fin.size else None
        # The leg-1-minus-leg-2 difference panel is opt-in: its one number (the
        # median, i.e. the common-mode ionosphere the single-frequency solution
        # absorbs into its clock) is in the read-out below, and the trace itself
        # is a restatement of the leg-1 error already plotted twice above.
        axes_e = []
        if show_clock_difference:
            axe1 = Axes(title="Receiver clock: leg 1 − leg 2 c·dt — the common-mode "
                              "ionosphere the single-frequency solution absorbs",
                        xlabel="", ylabel="Δ c·dt (m)", legend=Legend(loc="best"))
            axe1.traces = [Line(x=t, y=dc, color=(0.35, 0.15, 0.55), width=1.1,
                                label="leg 1 − leg 2 c·dt")]
            if cdt_med is not None:
                axe1.traces.append(HLine(y=cdt_med, color=_C_REF, style="--",
                                         label=f"median {cdt_med:+.2f} m"))
            axe1.traces += storm
            axes_e.append(axe1)
        # Each leg about its own DE-TRENDED baseline.  Removing the median alone was
        # enough while the simulated receiver clock was a constant bias; since the
        # truth clock drifts (see the exam ANSWER_KEY receiver-clock note) it ramps
        # ~13 km over a 12 h arc, which is ~600x the ~22 m of real leg-to-leg
        # structure — the panel autoscales to the ramp and both legs collapse onto
        # one straight line with the physics at sub-pixel scale.  Fit and subtract a
        # straight line per leg (the oscillator's bias + frequency offset, which is
        # nuisance here) and what is left is exactly what the panel is about.
        def _detrend(y):
            good = np.isfinite(y)
            if good.sum() < 3:
                return y - np.nanmedian(y)
            x = np.arange(y.size, dtype=float)
            a, b = np.polyfit(x[good], y[good], 1)
            r = y - (a * x + b)
            return r - np.nanmedian(r)

        d1, d2_ = _detrend(c1), _detrend(c2)
        # Report the removed rate so the ramp is disclosed, not silently hidden.
        dt_s = 1.0
        if t is not None and len(t) > 1:
            try:
                dt_s = max(float((t[-1] - t[0]) / np.timedelta64(1, "s")) / (len(t) - 1), 1e-9)
            except Exception:
                dt_s = 1.0
        rate = np.nan
        good = np.isfinite(c1)
        if good.sum() >= 3:
            rate = float(np.polyfit(np.arange(c1.size, dtype=float)[good],
                                    c1[good], 1)[0]) / dt_s
        axe2 = Axes(title="Each leg's c·dt, linear trend removed (the shared clock model)"
                          + ("" if not np.isfinite(rate)
                             else f" — removed drift {rate:+.4f} m/s"),
                    xlabel="GPS Time", ylabel="c·dt − (fit) (m)", legend=Legend(loc="best"))
        axe2.traces = [
            Line(x=t, y=d1, color=_C_L1, width=1.0, label="leg 1"),
            Line(x=t, y=d2_, color=_C_IF, width=1.0, label="leg 2"),
        ] + storm
        axes_e.append(axe2)
        figures.append(Figure(name="Receiver clock", axes=axes_e, width=13,
                              height=6.6 if len(axes_e) == 2 else 3.6))

    # ------------------------------------------------------ (f) sats + PDOP
    if "l1_n" in spp.columns and "if_n" in spp.columns:
        n1 = spp["l1_n"].to_numpy(float)
        n2 = spp["if_n"].to_numpy(float)
        axf = Axes(title="Satellites used per leg (leg 1 screens the broadcast-unhealthy "
                         "G01; the precise products carry it)",
                   xlabel="GPS Time", ylabel="satellites used",
                   y2label="PDOP (right axis)", legend=Legend(loc="best"))
        # Leg 2 wide underneath, leg 1 narrow on top: the counts agree at most
        # epochs, so equal widths let leg 2 paint over leg 1 entirely and the
        # panel reads as a single series.
        axf.traces = [
            Line(x=t, y=n2, color=_C_IF, width=2.6,
                 label=f"leg 2 (mean {np.nanmean(n2):.2f})"),
            Line(x=t, y=n1, color=_C_L1, width=1.1,
                 label=f"leg 1 (mean {np.nanmean(n1):.2f})"),
        ]
        d_n = n2 - n1
        fin = d_n[np.isfinite(d_n)]
        if fin.size:
            n_diff = int((fin != 0).sum())
            axf.traces.append(TextBox(
                x=0.02, y=0.97, mono=True, boxed=True,
                text=(f"legs differ at {n_diff} / {fin.size} epochs\n"
                      f"max extra sats in leg 2: {int(fin.max())}")))
        for col, color, lbl in (("l1_pdop", _C_L1, "leg 1 PDOP"),
                                ("if_pdop", _C_IF, "leg 2 PDOP")):
            if col in spp.columns:
                axf.traces.append(Line(x=t, y=spp[col].to_numpy(float), color=color,
                                       width=0.7, style="--", axis="right", label=lbl))
        axf.traces += storm
        figures.append(Figure(name="Satellites & PDOP", axes=[axf], width=13, height=4.2))

    return ViewerResult(
        figures=figures,
        summary=_build_summary(ctx, spp, l1_3d, if_3d, ab_rows, cdt_med, have_enu, sig_rows))


def _build_summary(ctx, spp, l1_3d, if_3d, ab_rows, cdt_med, have_enu, sig_rows=()) -> str:
    js = _summary_json(ctx)
    out = ["=== Part-B SPP reference runs (author-side, `gnss_simulator spp-reference`) ==="]

    n_ep = len(spp)
    n_l1 = int(np.isfinite(l1_3d).sum())
    n_if = int(np.isfinite(if_3d).sum())
    out.append(f"Epochs in truth_spp.csv : {n_ep}   solved: leg 1 {n_l1}, leg 2 {n_if}")
    if js:
        claimed = js.get("n_epochs")
        if isinstance(claimed, int) and claimed != n_ep:
            out.append(f"  !! truth_spp_summary.json claims {claimed} epochs but the CSV holds "
                       f"{n_ep} — the summary is STALE relative to the CSV")

    out += ["", f"{'leg':<44}{'median':>9}{'mean':>9}{'p90':>9}{'max':>9}"]
    for lbl, v in (("leg 1  C1C + brdc eph/clk + TGD + Klobuchar", l1_3d),
                   ("leg 2  IF(C1C,C2W) + COD SP3/CLK + OSB", if_3d)):
        s = _stats(v)
        if s:
            out.append(f"{lbl:<44}{s['median']:>9.2f}{s['mean']:>9.2f}"
                       f"{s['p90']:>9.2f}{s['max']:>9.2f}   (m 3D)")

    if have_enu:
        out.append("")
        for lbl, e, n, u in (("leg 1", "l1_e_m", "l1_n_m", "l1_u_m"),
                             ("leg 2", "if_e_m", "if_n_m", "if_u_m")):
            h = np.hypot(spp[e].to_numpy(float), spp[n].to_numpy(float))
            uu = spp[u].to_numpy(float)
            out.append(f"  {lbl} horizontal median {np.nanmedian(h):6.2f} m   "
                       f"signed Up median {np.nanmedian(uu):+7.2f} m")

    if ab_rows:
        out += ["", "Paired A/B ablations (variant − canonical, per epoch):",
                f"  {'ablation':<32}{'mean':>9}{'median':>9}{'p5':>9}{'p95':>9}"]
        for lbl, mean, med, p5, p95 in ab_rows:
            out.append(f"  {lbl:<32}{mean:>+9.2f}{med:>+9.2f}{p5:>+9.2f}{p95:>+9.2f}   (m)")
        out.append("  (the precise-orbit twin is the no-TGD variant, so the orbit row is "
                   "l1_notgd − l1_prec)")

    if sig_rows:
        out += ["", "Formal per-epoch uncertainty (a-posteriori scaled, medians) — the "
                    "Part-B stochastic-model reference:",
                f"  {'leg':<8}{'σ_h':>8}{'σ_U':>8}{'actual 3D':>12}{'err/σ3D':>10}"]
        for lbl, sh, su, e3, ratio in sig_rows:
            out.append(f"  {lbl:<8}{sh:>8.2f}{su:>8.2f}{e3:>12.2f}{ratio:>10.2f}   (m)")
        out.append("  err/σ3D ≈ 1 means the covariance describes the error. Leg 2 is honest; "
                   "leg 1 is NOT —")
        out.append("  it reports a SMALLER σ than leg 2 while being several times worse, because "
                   "its error is")
        out.append("  an unmodelled ionospheric systematic the stochastic model cannot see. "
                   "A student who")
        out.append("  quotes a leg-1 σ without that caveat has the arithmetic right and the "
                   "inference wrong.")

    if cdt_med is not None:
        out += ["", f"Receiver clock  leg 1 − leg 2 c·dt median : {cdt_med:+.2f} m"
                    "   = the common-mode ionosphere absorbed into the single-freq clock"]

    for col, lbl in (("l1_n", "leg 1"), ("if_n", "leg 2")):
        if col in spp.columns:
            out.append(f"  mean satellites used, {lbl} : {np.nanmean(spp[col].to_numpy(float)):.2f}")

    if js:
        out.append("")
        if isinstance(js.get("date"), dict):
            d = js["date"]
            out.append(f"Arc            : {d.get('iso', '?')} (DOY {d.get('doy', '?')})"
                       f"   obs {js.get('obs_rinex', '?')}")
        for key, lbl in (("estimator", "Estimator"), ("modeled", "Modeled"),
                         ("unmodeled_by_design", "Unmodeled")):
            if js.get(key):
                out.append(f"{lbl:<15}: {js[key]}")
        if js.get("elevation_mask_deg") is not None:
            out.append(f"{'Elevation mask':<15}: {js['elevation_mask_deg']}°")

        # Cross-check the shipped summary against what the CSV actually holds.
        drift = []
        for vkey, series in (("l1_broadcast", l1_3d), ("if_precise", if_3d)):
            got = (js.get("variants", {}).get(vkey, {}).get("err3d_m", {}) or {}).get("median")
            s = _stats(series)
            if isinstance(got, (int, float)) and s and abs(got - s["median"]) > 1e-3:
                drift.append(f"{vkey}: summary {got:.4f} vs CSV {s['median']:.4f}")
        if drift:
            out.append("  !! summary/CSV median mismatch — " + "; ".join(drift))
    else:
        out.append("")
        out.append("(truth_spp_summary.json absent — model ledger omitted; "
                   "all statistics above are computed from truth_spp.csv)")

    return "\n".join(out)


viewer = Viewer(name="exam_spp_reference",
                title="Part-B SPP reference: single-freq broadcast vs IF precise",
                required_csvs=("truth_spp.csv",),
                build=build_exam_spp_reference)

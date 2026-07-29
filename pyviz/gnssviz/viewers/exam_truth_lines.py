"""Answer-key constants overlaid on the processor's estimated states.

The self-referenced :mod:`gnssviz.viewers.state` viewer draws the processor's
``tropo_m`` / ``Galileo_m`` / ``clock_m`` states but has no way to draw the
*truth* they should converge to — those live in the GnssSimulator answer key as
constants (``truth_scalars.csv``: ``zwd_m``, ``isb_gps_gal_m``) or a per-epoch
truth series (``truth_clock.csv``).  This viewer supplies exactly those overlay
lines so the exam reader can eyeball recovery against the construction truth:

  1. Estimated ZWD (``tropo_m`` ±1σ) vs the truth wet delay — the **per-epoch**
     ``truth_tropo.csv`` series when it exists (``truth_scalars``' ``zwd_m`` is
     only the arc *mean* once the wet delay is time-varying, and drawing a mean
     as a flat truth line fabricates an error that is really the truth moving),
     else the legacy constant.  Graded as RMS over epochs, like the Verifier.
  2. Recovered GPS→Galileo ISB (``Galileo_m`` ±1σ) vs the truth ISB
     (``isb_gps_gal_m``) — skipped on a GPS-only run (no ``Galileo_m`` column).
  3. Receiver clock residual ``clock_m − clock_bias_m`` on the (week, tow-ms)
     grid — only when ``truth_clock.csv`` is present.

The truth constants have no ``(week, tow)`` stamp, so figures 1–2 read the state
trace straight (``load_trace`` + ``add_utc``) and pull the constants from
:func:`exam_common.scalars`; only the clock residual (fig 3) uses the sanctioned
:func:`exam_common.merge_truth_trace` join.  The storm window overlays every
figure.  A summary reports the final recovered ZWD/ISB and its error in mm.
"""

from __future__ import annotations

import numpy as np

from ..figures import Axes, Band, Figure, HLine, Legend, Line
from ..io import add_utc, first_col, load_trace
from .base import Viewer, ViewerResult
from . import exam_common as ec

_C_ZWD = (0.0, 0.6, 0.0)          # green  — estimated ZWD
_C_ISB = (0.15, 0.35, 0.75)       # blue   — recovered ISB
_C_CLK = (0.55, 0.25, 0.70)       # purple — clock residual
_C_TRUTH = (0.0, 0.0, 0.0)        # black  — truth reference lines
_C_ZERO = (0.5, 0.5, 0.5)         # grey   — zero reference


def _num(v):
    """``float(v)`` or ``None`` (scalars() may hand back a string sentinel)."""
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _last_finite(a):
    """Final finite sample of ``a`` (``NaN`` if none) — the 'recovered' value."""
    a = np.asarray(a, float)
    a = a[np.isfinite(a)]
    return float(a[-1]) if a.size else float("nan")


def _est_vs_truth_fig(t, est, sig, truth_val, *, est_color, name, title,
                      ylabel, est_label, truth_label, storm, truth_series=None):
    """One tile: estimate (±1σ shaded band) against the truth.

    ``truth_series`` is an optional ``(t, y)`` pair drawn as a line — use it
    whenever the truth actually moves.  Falling back to ``truth_val`` as a flat
    :class:`HLine` is only correct for a genuinely constant truth; drawing a
    *mean* as a flat line invites the reader to measure the estimate against a
    value it was never supposed to sit on.
    """
    traces = []
    if sig is not None:
        traces.append(Band(x=t, lo=est - sig, hi=est + sig, color=est_color,
                           alpha=0.18, label="±1σ", show_in_legend=True))
    traces.append(Line(x=t, y=est, color=est_color, width=1.3, label=est_label))
    if truth_series is not None:
        tt, ty = truth_series
        traces.append(Line(x=tt, y=ty, color=_C_TRUTH, width=1.2, style="--",
                           label=truth_label))
    elif truth_val is not None:
        traces.append(HLine(y=truth_val, color=_C_TRUTH, style="--", label=truth_label))
    traces += storm
    ax = Axes(title=title, xlabel="GPS Time", ylabel=ylabel, legend=Legend(loc="best"))
    ax.traces = traces
    return Figure(name=name, axes=[ax], width=11, height=3.8)


def build_exam_truth_lines(ctx) -> ViewerResult:
    # Primary answer-key input: raises ViewerSkip on a non-exam run (no truth dir).
    ec.require_truth(ctx, "truth_scalars.csv")
    sc = ec.scalars(ctx)

    state = add_utc(load_trace(ctx.path("state_trace.csv")))
    t = state["utc"]
    storm = ec.storm_vlines(ctx)

    figures = []
    lines = ["=== Answer-key constants vs recovered states ==="]

    # --- Figure 1: estimated ZWD vs truth ZWD (guarded on tropo_m) -----------
    # truth_scalars' zwd_m is the ARC MEAN once the wet delay is time-varying
    # (sim_sar_fiji swings 0.09-0.21 m), so measuring the estimate against it as
    # a constant manufactures an error that is really just the truth moving.
    # Prefer the per-epoch truth_tropo.csv series and grade RMS over epochs, the
    # way the C++ Verifier does; fall back to the scalar only when it is absent.
    trop_col = first_col(state, "tropo_m", "tropo_zwd_m")
    zwd_truth = _num(sc.get("zwd_m"))
    if trop_col is not None:
        trop = trop_col.to_numpy(float)
        trop_s = first_col(state, "tropo_std_m")
        trop_s = trop_s.to_numpy(float) if trop_s is not None else None

        ttro = ec.truth_frame(ctx, "truth_tropo.csv")
        series, resid = None, None
        if ttro is not None and first_col(ttro, "zwd_m") is not None:
            m = ec.merge_truth_trace(ttro, state)
            m_est = first_col(m, "tropo_m", "tropo_zwd_m")
            m_tru = first_col(m, "zwd_m")
            if not m.empty and m_est is not None and m_tru is not None:
                series = (m["utc"], m_tru.to_numpy(float))
                # Grade the steady state only, with the SAME skip the Verifier
                # applies — otherwise this panel and the scorecard beside it
                # report different ZWD errors for the same run.
                skip = min(ec.converge_skip(ctx), max(0, len(m) - 1))
                resid = (m_est.to_numpy(float) - m_tru.to_numpy(float))[skip:]

        figures.append(_est_vs_truth_fig(
            t, trop, trop_s, zwd_truth, est_color=_C_ZWD,
            name="ZWD vs truth", title="Estimated ZWD vs truth",
            ylabel="Zenith wet delay (m)", est_label="estimated ZWD",
            truth_label="truth ZWD (per epoch)" if series is not None else "truth ZWD",
            storm=storm, truth_series=series))

        rec = _last_finite(trop)
        if resid is not None:
            r = resid[np.isfinite(resid)]
            if r.size:
                lines.append(
                    f"ZWD  : RMS(est-truth) {np.sqrt(np.mean(r**2))*1000:6.1f} mm | "
                    f"max |err| {np.max(np.abs(r))*1000:6.1f} mm | {r.size} steady-state "
                    f"epochs (truth varies "
                    f"{series[1].min()*100:.1f}-{series[1].max()*100:.1f} cm)")
        elif zwd_truth is not None and np.isfinite(rec):
            lines.append(f"ZWD  : truth {zwd_truth*100:7.2f} cm | recovered "
                         f"{rec*100:7.2f} cm | error {(rec-zwd_truth)*1000:+7.1f} mm")
        else:
            lines.append(f"ZWD  : recovered {rec*100:7.2f} cm (no truth constant)")

    # --- Figure 2: recovered GPS->Galileo ISB vs truth (GPS-only skips) ------
    gal_col = first_col(state, "Galileo_m")
    isb_truth = _num(sc.get("isb_gps_gal_m"))
    if gal_col is not None:
        gal = gal_col.to_numpy(float)
        gal_s = first_col(state, "Galileo_std_m")
        gal_s = gal_s.to_numpy(float) if gal_s is not None else None
        figures.append(_est_vs_truth_fig(
            t, gal, gal_s, isb_truth, est_color=_C_ISB,
            name="ISB vs truth", title="Recovered GPS->Galileo ISB vs truth",
            ylabel="GPS->Galileo ISB (m)", est_label="recovered ISB",
            truth_label="truth ISB", storm=storm))
        rec = _last_finite(gal)
        if isb_truth is not None and np.isfinite(rec):
            lines.append(f"ISB  : truth {isb_truth:8.3f} m  | recovered "
                         f"{rec:8.3f} m  | error {(rec-isb_truth)*1000:+7.1f} mm")
        else:
            lines.append(f"ISB  : recovered {rec:8.3f} m (no truth constant)")

    # --- Figure 3: clock residual (est - truth) — only if truth_clock present -
    tclk = ec.truth_frame(ctx, "truth_clock.csv")
    if (tclk is not None and "clock_bias_m" in tclk.columns
            and first_col(state, "clock_m") is not None):
        m = ec.merge_truth_trace(tclk, state)
        if not m.empty and first_col(m, "clock_m") is not None \
                and first_col(m, "clock_bias_m") is not None:
            resid = (first_col(m, "clock_m").to_numpy(float)
                     - first_col(m, "clock_bias_m").to_numpy(float))
            ax = Axes(title="Receiver clock residual (estimate - truth)",
                      xlabel="GPS Time", ylabel="Clock residual (m)",
                      legend=Legend(loc="best"))
            ax.traces = [Line(x=m["utc"], y=resid, color=_C_CLK, width=1.2,
                              label="clock_m - clock_bias_m"),
                         HLine(y=0.0, color=_C_ZERO, style="--", label="zero")] + storm
            figures.append(Figure(name="Clock residual vs truth", axes=[ax],
                                  width=11, height=3.6))
            rc = _last_finite(resid)
            if np.isfinite(rc):
                lines.append(f"Clock: final residual {rc*1000:+7.1f} mm "
                             f"({len(m)} epochs joined)")

    if len(lines) == 1:
        lines.append("(no tropo_m / Galileo_m / truth_clock overlays available)")

    return ViewerResult(figures=figures, summary="\n".join(lines))


viewer = Viewer(name="exam_truth_lines", title="Truth constants on estimated states",
                required_csvs=("truth_scalars.csv", "state_trace.csv"),
                build=build_exam_truth_lines)

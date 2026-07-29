"""Truth-referenced ocean tidal loading — the one term built NOT to cancel.

Reads the GnssSimulator answer key ``truth_otl.csv`` (written whenever the
synthesizer injects ocean loading, bound to a
:class:`~gnssviz.trace_paths.TraceContext` via ``--truth-dir``).

Every other correction in the M7 stack is injected by *reusing* the processor's
own model, so it cancels and leaves nothing behind.  Ocean loading is the
exception, and deliberately so: it is a **field**, but the processor synthesizes
its coefficients once at the RINEX ``APPROX POSITION`` and reuses them for the
whole arc (only the ENU→ECEF rotation follows the receiver).  With
``corrections.ocean_tide_along_track`` the simulator samples the real EOT20 grid
*along the track*, so what is left in the data is

    d_up(t) = inj_up(t) − mod_up(t)

— zero at the first epoch, growing as the vessel leaves ``x₀``.  Figures:

  1. Two x-linked tiles: the radial loading the vessel actually felt
     (``inj_up_m``) against what the processor models (``mod_up_m``) — visually
     one line, because the difference is ~5% of the signal — and beneath it the
     remainder ``d_up_m`` that no model removes.
  2. ``d_up`` against straight-line distance from the APPROX POSITION: the
     envelope grows with distance, but the term is a *beat between two nearby
     tidal phasors*, not a ramp, so the cloud fans out rather than following a
     line.
  3. ``d_up`` on the same axes as the run's actual vertical position error (when
     ``truth_position.csv`` + ``state_trace.csv`` are present).  This is the
     panel that matters: ``−sin(el)`` **is** the range partial with respect to
     the up coordinate, so ``d_up`` is exactly degenerate with height and lands
     almost entirely on the height estimate — leaving no clock/ZWD signature and
     essentially no post-fit residual.  It is invisible because it is millimetres
     against centimetres of vertical error, *not* because it hides beneath the
     residual floor.

In frozen mode (``ocean_tide`` without ``ocean_tide_along_track``) ``d_up`` is
identically zero and the viewer says so — that is the control, not a failure.
"""

from __future__ import annotations

import numpy as np

from ..figures import Axes, Figure, HLine, Legend, Line, Scatter, TextBox
from ..io import add_utc, first_col, load_trace
from .base import Viewer, ViewerResult, ViewerSkip
from . import exam_common as ec

_C_INJ = (0.85, 0.45, 0.10)       # amber  — loading the vessel felt
_C_MOD = (0.15, 0.35, 0.75)       # blue   — loading the processor models
_C_D = (0.80, 0.15, 0.15)         # red    — unmodelled remainder
_C_VERT = (0.35, 0.35, 0.35)      # grey   — actual vertical error, for scale
_C_ZERO = (0.5, 0.5, 0.5)


def _mm(series):
    return np.asarray(series, float) * 1000.0


def _vertical_error_mm(ctx):
    """``(utc, vertical error in mm)`` for this run, or ``None``.

    Uses only :mod:`exam_common` helpers (the sanctioned ``(week, tow-ms)`` join
    plus the shared ENU rotation), so this viewer stays independent of
    :mod:`exam_position_truth` while reporting the same quantity.
    """
    truth = ec.truth_frame(ctx, "truth_position.csv")
    if truth is None:
        return None
    try:
        state = add_utc(load_trace(ctx.path("state_trace.csv")))
    except Exception:
        return None
    try:
        m = ec.merge_truth_trace(truth, state)
    except ValueError:
        return None
    if m.empty:
        return None
    cols = [first_col(m, f"{c}_trace") for c in ("x_m", "y_m", "z_m")]
    cols += [first_col(m, f"{c}_truth") for c in ("x_m", "y_m", "z_m")]
    cols += [first_col(m, "lat_deg"), first_col(m, "lon_deg")]
    if any(c is None for c in cols):
        return None
    ex, ey, ez, tx, ty, tz, lat, lon = (c.to_numpy(float) for c in cols)
    _, _, u = ec.enu_error(ex, ey, ez, tx, ty, tz, lat, lon)
    # Drop the filter's convergence transient with the same skip the Verifier
    # uses, so the scale quoted here is the scale on the scorecard (including it
    # would report 124 mm RMS against the Verifier's 40 mm).
    skip = min(ec.converge_skip(ctx), max(0, len(m) - 1))
    return m["utc"].to_numpy()[skip:], u[skip:] * 1000.0


def build_exam_otl_truth(ctx) -> ViewerResult:
    df = ec.require_truth(ctx, "truth_otl.csv").copy()
    missing = [c for c in ("inj_up_m", "mod_up_m", "d_up_m") if first_col(df, c) is None]
    if missing:
        raise ViewerSkip(f"truth_otl.csv lacks {', '.join(missing)}")
    df = add_utc(df)

    t = df["utc"]
    inj = _mm(df["inj_up_m"])
    mod = _mm(df["mod_up_m"])
    dup = _mm(df["d_up_m"])
    storm = ec.storm_vlines(ctx)

    sc = ec.scalars(ctx)
    along = bool(float(sc.get("otl_along_track", 0.0) or 0.0) > 0.5)
    ss = ec.scenario_summary(ctx)
    ol = ss.get("ocean_loading", {}) if isinstance(ss, dict) else {}
    anchors = ol.get("anchors") if isinstance(ol, dict) else None
    spacing = ol.get("anchor_spacing_km") if isinstance(ol, dict) else None

    peak = float(np.nanmax(np.abs(dup))) if np.any(np.isfinite(dup)) else float("nan")
    fin = dup[np.isfinite(dup)]
    rms = float(np.sqrt(np.mean(fin ** 2))) if fin.size else float("nan")

    figures = []

    # ---- Figure 1: felt vs modelled, and what is left ----------------------- #
    ax_a = Axes(title="Ocean tidal loading: what the vessel felt vs what the processor models",
                xlabel="", ylabel="Radial loading (mm)", legend=Legend(loc="best"))
    ax_a.traces = [
        Line(x=t, y=inj, color=_C_INJ, width=1.4, label="injected (along track)"
             if along else "injected (frozen at APPROX POSITION)"),
        Line(x=t, y=mod, color=_C_MOD, width=1.1, style="--",
             label="modelled (frozen APPROX-POSITION coefficients)"),
    ] + storm

    ax_b = Axes(title="Unmodelled remainder d_up = injected - modelled",
                xlabel="GPS Time", ylabel="d_up (mm)", legend=Legend(loc="best"))
    ax_b.traces = [
        Line(x=t, y=dup, color=_C_D, width=1.3, label="d_up (no model removes this)"),
        HLine(y=0.0, color=_C_ZERO, style="--", label="zero"),
        TextBox(x=0.02, y=0.95, mono=True, bold=True, boxed=True,
                text=(f"peak |d_up| {peak:.2f} mm\nRMS      {rms:.2f} mm"
                      + (f"\n{int(anchors)} anchors @ {float(spacing):g} km"
                         if anchors and spacing else "")
                      + ("" if along else "\nFROZEN: cancels exactly"))),
    ] + storm
    figures.append(Figure(name="Ocean loading vs model", axes=[ax_a, ax_b],
                          width=12.0, height=6.6))

    # ---- Figure 2: remainder vs distance from the APPROX POSITION ----------- #
    if first_col(df, "dist_from_approx_km") is not None:
        dist = df["dist_from_approx_km"].to_numpy(float)
        ax_d = Axes(title="Unmodelled loading vs straight-line distance from the APPROX POSITION",
                    xlabel="Distance from APPROX POSITION (km)", ylabel="d_up (mm)",
                    x_is_datetime=False, legend=Legend(loc="best"))
        ax_d.traces = [
            Scatter(x=dist, y=dup, size=7, marker="o", color=_C_D, alpha=0.45,
                    label="epoch", show_in_legend=True),
            HLine(y=0.0, color=_C_ZERO, style="--", label="zero"),
            TextBox(x=0.02, y=0.95, mono=True, boxed=True,
                    text=("the envelope grows with distance, but this is a BEAT\n"
                          "between two nearby tidal phasors - not a ramp")),
        ]
        figures.append(Figure(name="Unmodelled loading vs distance", axes=[ax_d],
                              width=11.0, height=4.2))

    # ---- Figure 3: against the run's actual vertical error ------------------ #
    vert = _vertical_error_mm(ctx)
    vert_mean = float("nan")
    if vert is not None:
        vt, vu = vert
        f = np.isfinite(vu)
        # MEAN |U| over steady-state epochs — exactly what the Verifier prints as
        # "V" (Verifier.cpp accumulates sumv += |enu.z()| and divides by n_used),
        # so the scale quoted here is the number on the scorecard.  Not the RMS
        # (slip transients inflate it) and not the median (the Verifier's V is a
        # mean, and quoting a different statistic under the same name is how a
        # dashboard starts disagreeing with the thing it is describing).
        vert_mean = float(np.mean(np.abs(vu[f]))) if f.any() else float("nan")
        ax_v = Axes(title="Unmodelled loading against the run's actual vertical error "
                          "(same axis, same units)",
                    xlabel="GPS Time", ylabel="Up (mm)", legend=Legend(loc="best"))
        ax_v.traces = [
            Line(x=vt, y=vu, color=_C_VERT, width=0.9, label="vertical position error"),
            Line(x=t, y=dup, color=_C_D, width=1.4, label="unmodelled ocean loading"),
            HLine(y=0.0, color=_C_ZERO, style="--", label="zero"),
            TextBox(x=0.02, y=0.95, mono=True, boxed=True,
                    text=("d_up is EXACTLY degenerate with height (-sin(el) is the up\n"
                          "partial), so it lands on the height estimate - no clock/ZWD\n"
                          "signature, no post-fit residual.  It is invisible because it\n"
                          f"is {peak:.1f} mm against {vert_mean:.0f} mm of vertical error.")),
        ] + storm
        figures.append(Figure(name="Unmodelled loading vs vertical error", axes=[ax_v],
                              width=12.0, height=4.4))

    return ViewerResult(figures=figures,
                        summary=_summary(df, inj, dup, along, anchors, spacing,
                                         peak, rms, vert_mean))


def _summary(df, inj, dup, along, anchors, spacing, peak, rms, vert_mean) -> str:
    out = ["=== Ocean tidal loading (truth_otl) ==="]
    out.append(f"Mode             : {'ALONG-TRACK (does NOT cancel)' if along else 'FROZEN at APPROX POSITION (cancels)'}")
    if anchors and spacing:
        out.append(f"EOT20 sampling   : {int(anchors)} anchors @ {float(spacing):g} km along the track")
    out.append(f"Epochs           : {len(df)}")
    f_inj = inj[np.isfinite(inj)]
    if f_inj.size:
        out.append(f"Injected radial  : {f_inj.min():+.1f} .. {f_inj.max():+.1f} mm (the full loading signal)")
    out.append(f"Unmodelled d_up  : peak {peak:.2f} mm, RMS {rms:.2f} mm"
               + ("" if along else "  <- exactly zero: the control"))
    d = first_col(df, "dist_from_approx_km")
    if d is not None:
        dd = d.to_numpy(float)
        if np.any(np.isfinite(dd)):
            out.append(f"Distance from x0 : max {np.nanmax(dd):.1f} km straight-line "
                       f"(ends at {dd[-1]:.1f} km)")
    if np.isfinite(vert_mean):
        out.append(f"Vertical error   : {vert_mean:.1f} mm mean |U| (steady state; = the Verifier's V) "
                   f"— d_up is exactly")
        out.append(f"                   degenerate with HEIGHT, so THIS is the yardstick, not the")
        out.append(f"                   post-fit residual floor (the term leaves no residual)")
    return "\n".join(out)


viewer = Viewer(name="exam_otl_truth", title="Ocean tidal loading (truth)",
                required_csvs=("truth_otl.csv",),
                build=build_exam_otl_truth)

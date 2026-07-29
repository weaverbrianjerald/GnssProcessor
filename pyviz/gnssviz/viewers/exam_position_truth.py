"""Truth-referenced position error — the exam's headline accuracy panel.

Joins ``truth_position.csv`` against the processor's ``state_trace.csv`` on the
integer ``(week, tow-ms)`` grid (SPEC R5) and draws, about each epoch's *true*
receiver point (unlike the self-referenced ``state`` viewer, which can only show
error about its own median):

  (a) truth-referenced E / N / U + 3D error vs time, with the Verifier's
      convergence-skip boundary marked;
  (b) 3D error with the ``k·σ3D`` covariance envelope on a log y-axis, where
      ``σ3D = sqrt(x_std² + y_std² + z_std²)`` (rotation-invariant — we do NOT
      fabricate per-axis ENU σ from the diagonal-only trace);
  (c) the coverage ratio ``|err3D| / σ3D`` against the ``k`` reference line, with
      the % of steady-state epochs inside 1/2/3σ (the float-PPP covariance-
      optimism diagnostic, Part D).

Storm window + cycle-slip epochs overlay every time series.
"""

from __future__ import annotations

import numpy as np

from ..figures import Axes, Band, Figure, HLine, Legend, Line, TextBox, VLine
from ..io import first_col, gps_to_datetime, load_trace
from .base import Viewer, ViewerResult
from . import exam_common as ec

_C_E = (0.0, 0.0, 1.0)
_C_N = (0.0, 0.6, 0.0)
_C_U = (1.0, 0.0, 0.0)
_C_3D = (0.0, 0.0, 0.0)
_C_SIG = (0.85, 0.20, 0.20)
_SLIP_C = (0.45, 0.45, 0.45)


def _slip_vlines(ctx):
    """Thin grey VLines at each truth-slip epoch (empty if no truth_slips)."""
    df = ec.truth_frame(ctx, "truth_slips.csv")
    if df is None or not {"week", "tow_s"}.issubset(df.columns):
        return []
    ts = gps_to_datetime(df["week"], df["tow_s"])
    return [VLine(x=t, color=_SLIP_C, style=":", width=0.6) for t in ts]


def build_exam_position_truth(ctx) -> ViewerResult:
    truth = ec.require_truth(ctx, "truth_position.csv")
    state = load_trace(ctx.path("state_trace.csv"))
    m = ec.merge_truth_trace(truth, state)
    if m.empty:
        from .base import ViewerSkip
        raise ViewerSkip("no overlapping (week, tow) epochs between truth_position and state_trace")

    t = m["utc"]
    est_x = first_col(m, "x_m_trace").to_numpy(float)
    est_y = first_col(m, "y_m_trace").to_numpy(float)
    est_z = first_col(m, "z_m_trace").to_numpy(float)
    tru_x = first_col(m, "x_m_truth").to_numpy(float)
    tru_y = first_col(m, "y_m_truth").to_numpy(float)
    tru_z = first_col(m, "z_m_truth").to_numpy(float)
    lat = first_col(m, "lat_deg").to_numpy(float)
    lon = first_col(m, "lon_deg").to_numpy(float)

    e, n, u = ec.enu_error(est_x, est_y, est_z, tru_x, tru_y, tru_z, lat, lon)
    err3d = np.sqrt(e ** 2 + n ** 2 + u ** 2)

    xs = first_col(m, "x_std_m")
    ys = first_col(m, "y_std_m")
    zs = first_col(m, "z_std_m")
    have_sig = xs is not None and ys is not None and zs is not None
    if have_sig:
        sig3 = np.sqrt(xs.to_numpy(float) ** 2 + ys.to_numpy(float) ** 2
                       + zs.to_numpy(float) ** 2)
    else:
        sig3 = np.full(len(m), np.nan)

    rep = ec.verify_report(ctx)
    k = float(rep.get("k_sigma", 3.0)) if rep else 3.0
    skip = int(rep.get("converge_skip", 30)) if rep else 30
    skip = min(skip, max(0, len(m) - 1))

    storm = ec.storm_vlines(ctx)
    slips = _slip_vlines(ctx)
    skip_vline = ([VLine(x=t.iloc[skip], color=(0.4, 0.4, 0.4), style="--", width=1.2,
                         label=f"converge skip ({skip})")] if skip > 0 else [])

    figures = []

    # (a) ENU + 3D error.  Rigid +/-1 m y-limits so the cm-dm detail is legible on a
    # fixed scale (occasional slip/grounding transients clip rather than rescale).
    axa = Axes(title="Truth-referenced position error (E/N/U + 3D)",
               xlabel="GPS Time", ylabel="Error (m)", legend=Legend(loc="best"),
               ylim=(-1.0, 1.0))
    axa.traces = [
        Line(x=t, y=e, color=_C_E, width=1.0, label="E"),
        Line(x=t, y=n, color=_C_N, width=1.0, label="N"),
        Line(x=t, y=u, color=_C_U, width=1.0, label="U"),
        Line(x=t, y=err3d, color=_C_3D, width=1.4, label="3D"),
    ] + storm + skip_vline + slips
    figures.append(Figure(name="Position error (truth)", axes=[axa], width=13, height=4.4))

    # (b) 3D error + k·σ3D envelope, log-y.
    axb = Axes(title=f"3D error vs {k:.0f}·σ3D covariance envelope",
               xlabel="GPS Time", ylabel="3D error (m, log)", yscale="log",
               legend=Legend(loc="best"))
    axb.traces = [Line(x=t, y=np.where(err3d > 0, err3d, np.nan),
                       color=_C_3D, width=1.4, label="3D error")]
    if have_sig:
        axb.traces.append(Line(x=t, y=k * sig3, color=_C_SIG, style="--", width=1.2,
                               label=f"{k:.0f}·σ3D"))
    axb.traces += storm + slips
    figures.append(Figure(name="3D error vs covariance", axes=[axb], width=13, height=4.2))

    # (c) coverage ratio + within-σ percentages (steady-state only).
    summary = _summary(err3d, sig3, e, n, u, k, skip, have_sig, len(m))
    if have_sig:
        with np.errstate(divide="ignore", invalid="ignore"):
            ratio = err3d / sig3
        axc = Axes(title="Coverage ratio |err3D| / σ3D",
                   xlabel="GPS Time", ylabel="ratio (σ units)",
                   legend=Legend(loc="best"))
        axc.traces = [Line(x=t, y=ratio, color=(0.15, 0.35, 0.75), width=1.0,
                           label="|err3D|/σ3D"),
                      HLine(y=k, color=_C_SIG, style="--", label=f"k = {k:.0f}")]
        ss = slice(skip, None)
        pcts = _within_pcts(ratio[ss])
        axc.traces.append(TextBox(
            x=0.02, y=0.97, mono=True, bold=True, boxed=True,
            text=(f"within 1σ: {pcts[0]:5.1f}%\n"
                  f"within 2σ: {pcts[1]:5.1f}%\n"
                  f"within {k:.0f}σ: {pcts[2]:5.1f}%")))
        axc.traces += storm
        figures.append(Figure(name="Coverage ratio", axes=[axc], width=13, height=4.0))

    return ViewerResult(figures=figures, summary=summary)


def _within_pcts(ratio):
    r = np.asarray(ratio, float)
    r = r[np.isfinite(r)]
    if r.size == 0:
        return (float("nan"),) * 3
    return (100.0 * np.mean(r <= 1.0), 100.0 * np.mean(r <= 2.0), 100.0 * np.mean(r <= 3.0))


def _summary(err3d, sig3, e, n, u, k, skip, have_sig, n_ep) -> str:
    ss = slice(skip, None)
    err_s = np.asarray(err3d)[ss]
    err_s = err_s[np.isfinite(err_s)]
    out = ["=== Truth-referenced position error (steady state) ===",
           f"Matched epochs   : {n_ep} (skipping first {skip} for stats)"]
    if err_s.size:
        out += [f"3D median        : {np.median(err_s)*100:.1f} cm",
                f"3D mean / p90    : {np.mean(err_s)*100:.1f} / "
                f"{np.percentile(err_s, 90)*100:.1f} cm",
                f"3D max           : {np.max(err_s)*100:.1f} cm"]
        for lbl, v in (("E", e), ("N", n), ("U", u)):
            vs = np.asarray(v)[ss]
            vs = vs[np.isfinite(vs)]
            if vs.size:
                out.append(f"  {lbl} bias / RMS   : {np.mean(vs)*100:+.1f} / "
                           f"{np.sqrt(np.mean(vs**2))*100:.1f} cm")
    if have_sig:
        with np.errstate(divide="ignore", invalid="ignore"):
            ratio = (np.asarray(err3d) / np.asarray(sig3))[ss]
        p = _within_pcts(ratio)
        out += ["", f"Coverage (k={k:.0f}) : within 1σ {p[0]:.1f}%, 2σ {p[1]:.1f}%, "
                    f"{k:.0f}σ {p[2]:.1f}%  (float-PPP optimism expected)"]
    else:
        out.append("(no covariance columns — coverage panel skipped)")
    return "\n".join(out)


viewer = Viewer(name="exam_position_truth", title="Position error vs truth",
                required_csvs=("truth_position.csv", "state_trace.csv"),
                build=build_exam_position_truth)

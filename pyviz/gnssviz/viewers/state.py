"""State / position viewer: the port of ``state_base_parameters.m``.

Reads ``state_trace.csv`` (or the smoother schema) and produces, guarded by
column presence:
  * ENU and 3D position error vs time, with per-component mean/sigma read-outs
    (cm) over the post-converged window;
  * receiver clock bias (GPS / Galileo / phase) with +/-1 sigma, when present;
  * tropospheric zenith delay with +/-1 sigma;
  * tropospheric N/E azimuthal gradients, when present.

Reference point for the ENU frame is the median ECEF of the post-converged
second half of the run (matching the active choice in the MATLAB script).
"""

from __future__ import annotations

import numpy as np

from ..geodesy import ecef_to_enu, ecef_to_lla
from ..io import add_utc, first_col, load_trace
from ..figures import Axes, Figure, Legend, Line, TextBox
from .base import Viewer, ViewerResult

# Component colours, matching the MATLAB script.
_C_E = (0.0, 0.0, 1.0)        # blue
_C_N = (0.0, 0.6, 0.0)        # green
_C_U = (1.0, 0.0, 0.0)        # red
_C_3D = (0.0, 0.0, 0.0)       # black
_RED_DASH = (1.0, 0.0, 0.0)


def _half_index(n: int) -> int:
    """0-based start of the post-converged window (MATLAB ``lxyz-round(lxyz/2)``)."""
    r = int(np.floor(n / 2.0 + 0.5))         # MATLAB round: half away from zero
    return max((n - r) - 1, 0)               # 1-based (n-r), converted to 0-based


def _sigma_lines(t, y, s, base_color, label):
    """A solid line + dashed +/-1 sigma envelope (only if sigma present)."""
    out = [Line(x=t, y=y, color=base_color, width=1.2, label=label)]
    if s is not None:
        out.append(Line(x=t, y=np.asarray(y) + np.asarray(s), color=_RED_DASH,
                        style="--", width=1.0, show_in_legend=False))
        out.append(Line(x=t, y=np.asarray(y) - np.asarray(s), color=_RED_DASH,
                        style="--", width=1.0, show_in_legend=False))
    return out


def build_state(ctx) -> ViewerResult:
    df = load_trace(ctx.path("state_trace.csv"))
    df = add_utc(df)
    t = df["utc"]

    x = first_col(df, "x_m", "x").to_numpy(float)
    y = first_col(df, "y_m", "y").to_numpy(float)
    z = first_col(df, "z_m", "z").to_numpy(float)
    xs = _arr(first_col(df, "x_std_m", "sigma_x"))
    ys = _arr(first_col(df, "y_std_m", "sigma_y"))
    zs = _arr(first_col(df, "z_std_m", "sigma_z"))

    clk = _arr(first_col(df, "clock_m"))
    # Deliberate divergence from MATLAB: state_base_parameters.m falls back to
    # clock_m itself as the sigma when clock_std_m is absent, drawing a
    # nonsensical clk +/- clk band. This viewer draws no band instead, favoring
    # correctness over matching that behavior; the real state_trace.csv carries
    # no clock columns anyway.
    clks = _arr(first_col(df, "clock_std_m"))
    trop = _arr(first_col(df, "tropo_m", "tropo_zwd_m"))
    trops = _arr(first_col(df, "tropo_std_m", "sigma_tropo_m"))
    gal = _arr(first_col(df, "Galileo_m"))
    gals = _arr(first_col(df, "Galileo_std_m"))
    gn = _arr(first_col(df, "gn_m"))
    gns = _arr(first_col(df, "gn_std_m"))
    ge = _arr(first_col(df, "ge_m"))
    ges = _arr(first_col(df, "ge_std_m"))
    pclk = _arr(first_col(df, "phase_clock_m"))
    pclks = _arr(first_col(df, "phase_clock_std_m"))

    # --- ENU about the post-converged median reference ---
    n = min(len(x), len(y), len(z))
    h0 = _half_index(n)
    x0, y0, z0 = _ref_median(x, h0), _ref_median(y, h0), _ref_median(z, h0)
    ref_ok = bool(np.isfinite([x0, y0, z0]).all())

    figures = []
    stats = None
    moving = False

    # Figure 1: ENU & 3D position error + stat read-outs (cm) over converged
    # window.  Skipped if the converged window has no finite position samples
    # (a degenerate run), rather than emitting an all-NaN blank figure.
    if ref_ok:
        lat, lon, hgt = ecef_to_lla(x0, y0, z0)
        e, nth, u = ecef_to_enu(x, y, z, lat, lon, hgt)
        pos_err = np.sqrt(e ** 2 + nth ** 2 + u ** 2)
        stats = _enu_stats(e, nth, u, pos_err, h0)

        # For a moving receiver this ENU-about-median is the vessel's track (tens
        # of km), not an error, so it is unreadable and misleading here. Draw the
        # self-referenced panel only for a near-static receiver; on a kinematic run
        # the truth-referenced error lives in the exam position-error panel
        # (exam_position_truth.py) instead.
        moving = bool(np.nanmax(pos_err) > 100.0)

    if ref_ok and not moving:
        ax = Axes(title="ENU & 3D Position Error", xlabel="GPS Time",
                  ylabel="Error (m)", legend=Legend(loc="best"))
        ax.traces = [
            Line(x=t, y=e, color=_C_E, width=1.2, label="E"),
            Line(x=t, y=nth, color=_C_N, width=1.2, label="N"),
            Line(x=t, y=u, color=_C_U, width=1.2, label="U"),
            Line(x=t, y=pos_err, color=_C_3D, width=1.4, label="3D"),
        ]
        for ypos, col, lbl in ((0.97, _C_E, "E"), (0.93, _C_N, "N"),
                               (0.89, _C_U, "U"), (0.85, _C_3D, "3D")):
            mu, sg = stats[lbl]
            ax.traces.append(TextBox(
                x=0.02, y=ypos,
                text=f"{lbl:>3}: mean = {mu:4.1f} cm; sigma = {sg:4.1f} cm",
                color=col, mono=True, bold=True))
        figures.append(Figure(name="Position Error", axes=[ax], width=11, height=4.2))

    # Clock biases (each only if its column exists).
    for col, sig, ttl in ((clk, clks, "Receiver Clock Bias"),
                          (gal, gals, "Galileo Clock Bias"),
                          (pclk, pclks, "Phase Clock Bias")):
        if col is not None:
            a = Axes(title=ttl, xlabel="GPS Time", ylabel="Clock Bias (m)")
            a.traces = _sigma_lines(t, col, sig, (0, 0, 0), None)
            figures.append(Figure(name=ttl, axes=[a], width=11, height=3.6))

    # Troposphere with +/-1 sigma.
    if trop is not None:
        a = Axes(title="Tropospheric Zenith Delay with 1σ",
                 xlabel="GPS Time", ylabel="Tropo Delay (m)")
        a.traces = _sigma_lines(t, trop, trops, (0, 0.6, 0), None)
        figures.append(Figure(name="Troposphere", axes=[a], width=11, height=3.6))

    # Tropo azimuthal gradients (2 tiles), only if both present.
    if gn is not None and ge is not None:
        an = Axes(title="Tropospheric azimuthal gradients", ylabel="North (m)")
        an.traces = _sigma_lines(t, gn, gns, (0, 0.6, 0), None)
        ae = Axes(xlabel="GPS Time", ylabel="East (m)")
        ae.traces = _sigma_lines(t, ge, ges, (0, 0, 1), None)
        figures.append(Figure(name="Tropo gradient", axes=[an, ae],
                              width=11, height=6.0))

    summary = _summary_text(stats, n, h0, moving)
    return ViewerResult(figures=figures, summary=summary)


def _arr(series):
    return None if series is None else series.to_numpy(float)


def _ref_median(v, h0):
    """Median of the finite post-converged samples; NaN if none (no warning)."""
    w = np.asarray(v)[h0:]
    w = w[np.isfinite(w)]
    return float(np.median(w)) if w.size else float("nan")


def _enu_stats(e, nth, u, p, h0):
    """Mean/sigma (cm) over the post-converged window; sigma is sample (ddof=1).

    A single finite sample yields sigma 0 (matching MATLAB ``std`` of one
    element) rather than the NaN that ``ddof=1`` would produce.
    """
    def ms(v):
        w = np.asarray(v)[h0:]
        w = w[np.isfinite(w)]
        if w.size == 0:
            return (float("nan"), float("nan"))
        sd = float(np.std(w, ddof=1) * 100) if w.size > 1 else 0.0
        return (float(np.mean(w) * 100), sd)
    return {"E": ms(e), "N": ms(nth), "U": ms(u), "3D": ms(p)}


def _summary_text(stats, n, h0, moving=False) -> str:
    lines = ["=== Position-error summary (post-converged window) ===",
             f"Epochs total      : {n}",
             f"Converged from idx: {h0} (0-based)"]
    if stats is None:
        lines.append("  (no finite position samples in converged window — "
                     "ENU figure skipped)")
        return "\n".join(lines)
    if moving:
        lines.append("  (kinematic run: the self-referenced ENU is the vessel's "
                     "track, not an error — panel skipped; see the truth-referenced "
                     "position-error panel)")
    for lbl in ("E", "N", "U", "3D"):
        mu, sg = stats[lbl]
        lines.append(f"  {lbl:>3}: mean = {mu:6.1f} cm   sigma = {sg:6.1f} cm")
    return "\n".join(lines)


viewer = Viewer(name="state", title="State / position",
                required_csvs=("state_trace.csv",), build=build_state)

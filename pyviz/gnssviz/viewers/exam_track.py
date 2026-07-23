"""Plan-view track — truth vs recovered trajectory in a fixed local ENU frame.

Joins ``truth_position.csv`` against the processor's ``state_trace.csv`` on the
integer ``(week, tow-ms)`` grid (SPEC R5) and draws the horizontal trajectory in
a **local East/North frame about a single fixed origin** — the first matched
truth point — so 1 m East reads the same as 1 m North (``aspect_equal``).  Unlike
the time-series accuracy panels this is a *spatial* plot (``x_is_datetime`` is
off), so the storm window is shown by recolouring the affected track segment
rather than by the usual paired :class:`VLine`s.

  * KINEMATIC (SPEC R7): the grey truth path and the blue recovered path overlaid
    E-vs-N, with green/red start/end markers and the storm-window portion of the
    estimate recoloured amber (the same muted amber the time-series viewers use).
  * STATIC: the recovered ENU points scattered about the (single) truth point at
    the origin, coloured by epoch index through a viridis ramp + colorbar, so the
    convergence walk and the steady-state cluster are both legible.

Both origins are the SAME fixed geodetic point (truth lat/lon/h at the first
matched epoch); the truth and estimate ECEF are each rotated into that one frame
via :func:`gnssviz.geodesy.ecef_to_enu` so the horizontal offset is exact.
"""

from __future__ import annotations

import numpy as np

from ..colormaps import strip_colormap
from ..figures import Axes, ColorBar, Figure, Legend, Line, Scatter
from ..geodesy import ecef_to_enu
from ..io import first_col, load_trace
from .base import Viewer, ViewerResult, ViewerSkip
from . import exam_common as ec

_C_TRUTH = (0.45, 0.45, 0.45)     # grey
_C_EST = (0.15, 0.35, 0.75)       # blue
_C_START = (0.00, 0.60, 0.00)     # green
_C_END = (0.85, 0.15, 0.15)       # red
_C_ORIGIN = (0.0, 0.0, 0.0)       # black (static truth point)


def _storm_tow_bounds(ctx):
    """``(w0, w1)`` storm-window time-of-week bounds, or ``(None, None)``."""
    sc = ec.scalars(ctx)
    w0 = sc.get("storm_window_start_tow_s")
    w1 = sc.get("storm_window_end_tow_s")
    try:
        w0, w1 = float(w0), float(w1)
    except (TypeError, ValueError):
        return None, None
    return (w0, w1) if w1 > w0 else (None, None)


def build_exam_track(ctx) -> ViewerResult:
    truth = ec.require_truth(ctx, "truth_position.csv")
    state = load_trace(ctx.path("state_trace.csv"))
    m = ec.merge_truth_trace(truth, state)
    if m.empty:
        raise ViewerSkip("no overlapping (week, tow) epochs between truth_position and state_trace")

    # Fixed ENU origin = the first matched truth point (lat/lon/h are truth-only
    # columns, so the merge leaves their names unsuffixed).
    lat = first_col(m, "lat_deg").to_numpy(float)
    lon = first_col(m, "lon_deg").to_numpy(float)
    hgt = first_col(m, "h_m").to_numpy(float)
    lat0, lon0, h0 = float(lat[0]), float(lon[0]), float(hgt[0])

    tru_x = first_col(m, "x_m_truth").to_numpy(float)
    tru_y = first_col(m, "y_m_truth").to_numpy(float)
    tru_z = first_col(m, "z_m_truth").to_numpy(float)
    est_x = first_col(m, "x_m_trace").to_numpy(float)
    est_y = first_col(m, "y_m_trace").to_numpy(float)
    est_z = first_col(m, "z_m_trace").to_numpy(float)

    eT, nT, _ = ecef_to_enu(tru_x, tru_y, tru_z, lat0, lon0, h0)
    eE, nE, _ = ecef_to_enu(est_x, est_y, est_z, lat0, lon0, h0)

    # Horizontal error about each epoch's truth point (E/N offset of the estimate).
    dE = eE - eT
    dN = nE - nT
    horiz = np.hypot(dE, dN)

    kinematic = ec.is_kinematic(ctx)
    figures = []

    if kinematic:
        ax = Axes(title="Plan-view track (ENU about truth start)",
                  xlabel="East (m)", ylabel="North (m)",
                  x_is_datetime=False, aspect_equal=True,
                  legend=Legend(loc="best"))
        traces = [
            Line(x=eT, y=nT, color=_C_TRUTH, width=1.4, label="truth"),
            Line(x=eE, y=nE, color=_C_EST, width=1.2, label="estimate"),
        ]

        # Recolour the estimate segment inside the storm window (amber).
        tow = first_col(m, "tow_s_truth", "tow_s", "tow")
        w0, w1 = _storm_tow_bounds(ctx)
        if tow is not None and w0 is not None:
            tw = tow.to_numpy(float)
            mask = (tw >= w0) & (tw <= w1)
            if mask.any():
                traces.append(Line(x=eE[mask], y=nE[mask], color=ec.STORM_COLOR,
                                   width=2.6, label="storm"))

        # Start / end markers on the truth path (the clean reference trajectory).
        traces.append(Scatter(x=[eT[0]], y=[nT[0]], color=_C_START, size=70,
                              marker="o", edgecolor=(0, 0, 0), linewidth=0.6,
                              zorder=6, label="start"))
        traces.append(Scatter(x=[eT[-1]], y=[nT[-1]], color=_C_END, size=70,
                              marker="s", edgecolor=(0, 0, 0), linewidth=0.6,
                              zorder=6, label="end"))
        ax.traces = traces
        figures.append(Figure(name="Plan-view track", axes=[ax], width=7.5, height=7.0))
    else:
        n_ep = len(m)
        idx = np.arange(n_ep, dtype=float)
        ax = Axes(title="Static scatter about truth",
                  xlabel="East (m)", ylabel="North (m)",
                  x_is_datetime=False, aspect_equal=True,
                  cmap=strip_colormap("viridis", 256),
                  clim=(0.0, float(max(n_ep - 1, 1))),
                  colorbar=ColorBar(label="epoch"),
                  legend=Legend(loc="best"))
        ax.traces = [
            Scatter(x=eE, y=nE, c=idx, size=16, marker="o", alpha=0.85,
                    zorder=4, show_in_legend=False),
            Scatter(x=[0.0], y=[0.0], color=_C_ORIGIN, size=120, marker="*",
                    edgecolor=(1, 1, 1), linewidth=0.6, zorder=6, label="truth"),
        ]
        figures.append(Figure(name="Static scatter", axes=[ax], width=7.5, height=7.0))

    summary = _summary(horiz, dE, dN, eT, nT, kinematic, len(m))
    return ViewerResult(figures=figures, summary=summary)


def _summary(horiz, dE, dN, eT, nT, kinematic, n_ep) -> str:
    h = np.asarray(horiz, float)
    h = h[np.isfinite(h)]
    out = ["=== Plan-view track (horizontal error about truth) ===",
           f"Matched epochs   : {n_ep}",
           f"Mode             : {'KINEMATIC' if kinematic else 'STATIC'}"]
    if h.size:
        rms = float(np.sqrt(np.mean(h ** 2)))
        out += [f"Horizontal median: {np.median(h) * 100:.1f} cm",
                f"Horizontal RMS   : {rms * 100:.1f} cm",
                f"Horizontal max   : {np.max(h) * 100:.1f} cm"]
    if kinematic:
        de = np.diff(np.asarray(eT, float))
        dn = np.diff(np.asarray(nT, float))
        seg = np.hypot(de, dn)
        length = float(np.nansum(seg))
        out.append(f"Truth track length: {length:.1f} m")
    else:
        de = np.asarray(dE, float)
        dn = np.asarray(dN, float)
        de = de[np.isfinite(de)]
        dn = dn[np.isfinite(dn)]
        se = float(np.std(de)) if de.size else float("nan")
        sn = float(np.std(dn)) if dn.size else float("nan")
        out += [f"E / N scatter σ  : {se * 100:.1f} / {sn * 100:.1f} cm",
                f"2-D DRMS         : {np.hypot(se, sn) * 100:.1f} cm"]
    return "\n".join(out)


viewer = Viewer(name="exam_track", title="Plan-view track vs truth",
                required_csvs=("truth_position.csv", "state_trace.csv"),
                build=build_exam_track)

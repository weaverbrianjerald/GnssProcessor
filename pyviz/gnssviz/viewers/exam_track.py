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
from ..figures import Axes, Band, ColorBar, Figure, HLine, Legend, Line, Polygon, Scatter, TextBox
from ..geodesy import ecef_to_enu
from ..io import first_col, gps_to_datetime, load_trace
from .base import Viewer, ViewerResult, ViewerSkip
from . import exam_common as ec

_C_TRUTH = (0.45, 0.45, 0.45)     # grey
_C_EST = (0.15, 0.35, 0.75)       # blue
_C_START = (0.00, 0.60, 0.00)     # green
_C_END = (0.85, 0.15, 0.15)       # red
_C_ORIGIN = (0.0, 0.0, 0.0)       # black (static truth point)
_C_BOUND = (0.85, 0.20, 0.20)     # red — the k-sigma bound
_C_CORR = (0.15, 0.35, 0.75)      # blue — corridor fill
_C_ALONG = (0.10, 0.45, 0.20)     # green — along-track
_C_CROSS = (0.55, 0.20, 0.65)     # purple — cross-track

def _ellipse_rings(cx, cy, C2, k, scale, n_pts=40):
    """NaN-separated ring vertices for k-sigma ellipses at each (cx, cy)."""
    th = np.linspace(0.0, 2.0 * np.pi, n_pts)
    unit = np.stack([np.cos(th), np.sin(th)])           # (2, n_pts)
    xs, ys = [], []
    for i in range(len(cx)):
        C = C2[i]
        if not np.all(np.isfinite(C)):
            continue
        w, V = np.linalg.eigh(C)
        w = np.clip(w, 0.0, None)
        pts = (V * (k * scale * np.sqrt(w))) @ unit    # (2, n_pts)
        xs.append(pts[0] + cx[i]); xs.append([np.nan])
        ys.append(pts[1] + cy[i]); ys.append([np.nan])
    if not xs:
        return None, None
    return np.concatenate(xs), np.concatenate(ys)


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
            if ec.storm_overlay_is_informative(mask):
                traces.append(Line(x=eE[mask], y=nE[mask], color=ec.STORM_COLOR,
                                   width=2.6, label="storm"))
            elif mask.any():
                # Whole-arc storm: the overlay is 2.6 wide over a 1.2-wide estimate,
                # so drawing it hides the very track it annotates.  Say it instead.
                traces.append(TextBox(x=0.02, y=0.98, va="top", boxed=True,
                                      fontsize=7.5, color=ec.STORM_COLOR,
                                      text="entire arc inside the storm window"))

        # Start / end markers on the truth path (the clean reference trajectory).
        traces.append(Scatter(x=[eT[0]], y=[nT[0]], color=_C_START, size=70,
                              marker="o", edgecolor=(0, 0, 0), linewidth=0.6,
                              zorder=6, label="start"))
        traces.append(Scatter(x=[eT[-1]], y=[nT[-1]], color=_C_END, size=70,
                              marker="s", edgecolor=(0, 0, 0), linewidth=0.6,
                              zorder=6, label="end"))
        ax.traces = traces
        figures.append(Figure(name="Plan-view track", axes=[ax], width=7.5, height=7.0))

        # ---- error bounds ON the trajectory -----------------------------------
        C2 = ec.enu_cov2(m, lat0, lon0)
        if C2 is not None:
            k = float(np.sqrt(ec.CHI2_95))
            tvec, nvec = ec.track_frame(eT, nT)
            sig_cross = ec.proj_sigma(C2, nvec)
            sig_along = ec.proj_sigma(C2, tvec)

            # (i) Plan view with the k-sigma corridor.  A 2 cm bound on a 300 km
            # track is ~7 parts in 1e8 -- invisible at true scale -- so the
            # corridor is EXAGGERATED by a factor chosen to make the median bound
            # ~1.5% of the track's bounding box, snapped to a 1/2/5 decade.  The
            # factor is stated in the axes title and the legend; nothing here is
            # to scale, and it must never be read as one.
            span = max(float(np.nanmax(eT) - np.nanmin(eT)),
                       float(np.nanmax(nT) - np.nanmin(nT)), 1.0)
            # Scale off the STEADY-STATE p90, not the whole-arc median: the filter's
            # convergence transient carries sigma ~2.5 m against a steady-state ~0.6 cm,
            # and letting it into the factor made one opening ellipse 450 km across.
            skip = min(ec.converge_skip(ctx), max(0, len(sig_cross) - 2))
            exag = ec.exaggeration(k * sig_cross, span, skip=skip)

            # Even so, clamp any single exaggerated offset to 5% of the span so a
            # residual spike cannot rescale the axes and hide the track.
            cap = 0.05 * span
            off = np.minimum((k * exag) * sig_cross, cap)[:, None] * nvec
            lo_e, lo_n = eE - off[:, 0], nE - off[:, 1]
            hi_e, hi_n = eE + off[:, 0], nE + off[:, 1]
            good = np.isfinite(lo_e) & np.isfinite(hi_e) & np.isfinite(lo_n) & np.isfinite(hi_n)
            good[:skip] = False
            axc = Axes(title=f"Trajectory with 95% error bounds "
                             f"(cross-track corridor, EXAGGERATED {exag:,.0f}x)",
                       xlabel="East (m)", ylabel="North (m)",
                       x_is_datetime=False, aspect_equal=True, legend=Legend(loc="best"))
            ring_e, ring_n = _ellipse_rings(eE[skip::60], nE[skip::60], C2[skip::60], k, exag)
            axc.traces = [
                Polygon(x=np.concatenate([hi_e[good], lo_e[good][::-1]]),
                        y=np.concatenate([hi_n[good], lo_n[good][::-1]]),
                        fill=_C_CORR, alpha=0.22, zorder=1,
                        label=f"95% cross-track bound (x{exag:,.0f})", show_in_legend=True),
                Line(x=eT, y=nT, color=_C_TRUTH, width=1.2, label="truth"),
                Line(x=eE, y=nE, color=_C_EST, width=1.0, label="estimate"),
            ]
            if ring_e is not None:
                axc.traces.append(Line(x=ring_e, y=ring_n, color=_C_BOUND, width=0.8,
                                       label=f"95% ellipses (every 60th epoch, x{exag:,.0f})"))
            figures.append(Figure(name="Track error bounds", axes=[axc],
                                  width=7.5, height=7.0))

            # (ii) The same bounds at TRUE scale, which only reads in the
            # track-relative frame: cross- and along-track error against their own
            # +/- k*sigma envelopes.  This is the panel to quote a number from.
            tow = first_col(m, "tow_s_truth", "tow_s", "tow")
            wk = first_col(m, "week_truth", "week")
            t = (gps_to_datetime(wk, tow) if (wk is not None and tow is not None)
                 else np.arange(len(eE), dtype=float))
            err = np.stack([dE, dN], axis=1)
            e_cross = np.einsum("ni,ni->n", err, nvec)
            e_along = np.einsum("ni,ni->n", err, tvec)
            storm = ec.storm_vlines(ctx)
            panels = []
            for lbl, e_, s_, col in (("cross-track", e_cross, sig_cross, _C_CROSS),
                                     ("along-track", e_along, sig_along, _C_ALONG)):
                a = Axes(title=f"{lbl.capitalize()} error vs its 95% bound (true scale)",
                         xlabel="GPS Time" if lbl == "along-track" else "",
                         ylabel=f"{lbl} error (m)", legend=Legend(loc="best"))
                inside = float(np.mean(np.abs(e_[np.isfinite(e_)]) <=
                                       (k * s_)[np.isfinite(e_)]) * 100.0)
                a.traces = [
                    Band(x=t, lo=-k * s_, hi=k * s_, color=_C_BOUND, alpha=0.18,
                         label=f"±{k:.2f}·σ (95%)", show_in_legend=True),
                    Line(x=t, y=e_, color=col, width=1.0, label=f"{lbl} error"),
                    HLine(y=0.0, color=(0.5, 0.5, 0.5), style="-"),
                    TextBox(x=0.01, y=0.97, mono=True, boxed=True,
                            text=(f"σ median {np.nanmedian(s_)*100:5.1f} cm\n"
                                  f"inside   {inside:5.1f}%  (nominal 95%)")),
                ] + storm
                panels.append(a)
            figures.append(Figure(name="Track-relative error bounds", axes=panels,
                                  width=13, height=6.6))
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

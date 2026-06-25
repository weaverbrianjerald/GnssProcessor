"""WLS-trace viewer -- the port of ``wls.m``.

Reads ``wls_trace.csv`` (WLS seed / clock-jump diagnostics, one row per reseed)
and produces: ECEF X/Y/Z components, ENU components, 3D error magnitude, clock
estimates (WLS/KF/Δ), tropo, residual metrics (RMS / max-abs), and the
jump/seed boolean flags as stem plots.  ENU is referenced to the first epoch.
"""

from __future__ import annotations

import numpy as np

from ..geodesy import ecef_to_enu, ecef_to_lla
from ..io import add_utc, load_trace
from ..figures import Axes, Figure, Legend, Line, Stem
from .base import Viewer, ViewerResult

_BLUE = (0, 0, 1)
_MAGENTA = (1, 0, 1)
_GREEN = (0, 0.6, 0)
_RED = (1, 0, 0)
_BLACK = (0, 0, 0)


def build_wls(ctx) -> ViewerResult:
    df = add_utc(load_trace(ctx.path("wls_trace.csv")))
    t = df["utc"]
    x, y, z = (df["x_m"].to_numpy(float), df["y_m"].to_numpy(float),
               df["z_m"].to_numpy(float))

    lat, lon, h = ecef_to_lla(x[0], y[0], z[0])     # ref = first epoch
    e, n, u = ecef_to_enu(x, y, z, lat, lon, h)
    pos_err = np.sqrt(e ** 2 + n ** 2 + u ** 2)

    figures = []

    # ECEF components (3 stacked tiles).
    figures.append(_triple(t, [(x, "X (m)"), (y, "Y (m)"), (z, "Z (m)")], _BLUE,
                           "WLS Position Components (ECEF)", "WLS Position Components (ECEF)"))
    # ENU components.
    figures.append(_triple(t, [(e, "East (m)"), (n, "North (m)"), (u, "Up (m)")], _MAGENTA,
                           "WLS ENU Components", "WLS ENU Position Components"))

    # 3D error magnitude.
    figures.append(_single(t, pos_err, _BLACK, "WLS Position Error Magnitude",
                           "3D Position Error Magnitude (ENU)", "Error (m)", width=1.4))

    # Clock estimates.
    ax = Axes(title="Clock Estimates (WLS, KF, Δ)", xlabel="GPS Time",
              ylabel="Clock (m)", legend=Legend("best"))
    ax.traces = [
        Line(x=t, y=df["wls_clock_m"].to_numpy(float), color=_BLUE, label="WLS Clock"),
        Line(x=t, y=df["kf_clock_m"].to_numpy(float), color=_GREEN, label="KF Clock"),
        Line(x=t, y=df["delta_clock_m"].to_numpy(float), color=_RED, label="Δ Clock"),
    ]
    figures.append(Figure(name="Clock Estimates", axes=[ax], width=11, height=3.6))

    # Tropo.
    figures.append(_single(t, df["tropo_m"].to_numpy(float), _MAGENTA, "Tropo Estimate",
                           "WLS Troposphere Estimate", "Tropo (m)"))

    # Residual metrics.
    ax = Axes(title="WLS Residual Metrics", xlabel="GPS Time", ylabel="Residual (m)",
              legend=Legend("best"))
    ax.traces = [
        Line(x=t, y=df["rms_m"].to_numpy(float), color=_BLUE, label="RMS (m)"),
        Line(x=t, y=df["max_abs_m"].to_numpy(float), color=_RED, label="Max Abs (m)"),
    ]
    figures.append(Figure(name="Residual Metrics", axes=[ax], width=11, height=3.6))

    # Boolean flags (stem).
    aj = Axes(title="WLS Boolean Flags", ylabel="Jump")
    aj.traces = [Stem(x=t, y=df["jump_detected"].to_numpy(float), color=_RED)]
    asd = Axes(xlabel="GPS Time", ylabel="Seed")
    asd.traces = [Stem(x=t, y=df["should_seed"].to_numpy(float), color=_BLACK)]
    figures.append(Figure(name="WLS Flags", axes=[aj, asd], width=11, height=4.8))

    return ViewerResult(figures=figures)


def _triple(t, series, color, name, title):
    axes = []
    for i, (v, ylab) in enumerate(series):
        ax = Axes(ylabel=ylab, xlabel=("GPS Time" if i == len(series) - 1 else ""),
                  title=(title if i == 0 else ""))
        ax.traces = [Line(x=t, y=v, color=color, width=1.2)]
        axes.append(ax)
    return Figure(name=name, axes=axes, width=11, height=6.6)


def _single(t, v, color, name, title, ylabel, width=1.2):
    ax = Axes(title=title, xlabel="GPS Time", ylabel=ylabel)
    ax.traces = [Line(x=t, y=v, color=color, width=width)]
    return Figure(name=name, axes=[ax], width=11, height=3.6)


viewer = Viewer(name="wls", title="WLS seed / clock-jump diagnostics",
                required_csvs=("wls_trace.csv",), build=build_wls)

"""DD baseline-trace viewer -- the port of ``baseline_trace_parameters.m``.

Reads ``baseline_trace.csv`` (rover position + 1σ + DD throughput + ambiguity
count from ``Application::runBaseline()``) and produces: ECEF X/Y/Z with ±1σ,
ENU relative to the late-arc mean (the baseline-convergence view), an empirical
convergence curve (log y), the 1σ evolution (log y), DD throughput, DD ambiguity
count, and baseline length.  (The MATLAB 3D-trajectory figure is omitted -- the
ENU/length/convergence panels carry the same information without a 3D axes.)
"""

from __future__ import annotations

import numpy as np

from ..geodesy import ecef_to_enu, ecef_to_lla
from ..io import add_utc, first_col, load_trace
from ..figures import Axes, Figure, Legend, Line
from .base import Viewer, ViewerResult

# Reference station ECEF for the baseline-length series (dataset-specific;
# default = JPLM / IGS_Pasadena, matching the MATLAB script).  A wrong value
# only shifts the length offset, it doesn't break the plots.
_REF = (-2493304.6796, -4655215.1032, 3565497.5918)

_BLUE, _MAGENTA, _GREEN, _RED, _BLACK = (0, 0, 1), (1, 0, 1), (0, 0.6, 0), (1, 0, 0), (0, 0, 0)
_REDDASH = (1, 0, 0)


def build_baseline(ctx) -> ViewerResult:
    df = add_utc(load_trace(ctx.path("baseline_trace.csv")))
    t = df["utc"]
    x = first_col(df, "x_m", "x").to_numpy(float)
    y = first_col(df, "y_m", "y").to_numpy(float)
    z = first_col(df, "z_m", "z").to_numpy(float)
    xs = first_col(df, "x_std_m", "sigma_x").to_numpy(float)
    ys = first_col(df, "y_std_m", "sigma_y").to_numpy(float)
    zs = first_col(df, "z_std_m", "sigma_z").to_numpy(float)
    dds_applied = _arr(first_col(df, "dds_applied"))
    dds_built = _arr(first_col(df, "dds_built", "n_dd_total"))
    n_ambs = _arr(first_col(df, "n_ambs"))

    n = len(t)
    i0 = max(0, int(np.floor(0.75 * n + 0.5)) - 1)    # late-arc window (MATLAB round)
    xf, yf, zf = np.mean(x[i0:]), np.mean(y[i0:]), np.mean(z[i0:])
    lat, lon, h = ecef_to_lla(xf, yf, zf)             # ENU origin = late-arc mean
    e, nth, u = ecef_to_enu(x, y, z, lat, lon, h)
    conv_err = np.sqrt((x - xf) ** 2 + (y - yf) ** 2 + (z - zf) ** 2)
    baseline_len = np.sqrt((x - _REF[0]) ** 2 + (y - _REF[1]) ** 2 + (z - _REF[2]) ** 2)

    figures = []

    # ECEF with ±1σ.
    figures.append(_triple_sigma(t, [(x, xs, "X (m)"), (y, ys, "Y (m)"), (z, zs, "Z (m)")],
                                 "ECEF Position", "Rover ECEF Position with 1σ Uncertainty"))
    # ENU baseline vector.
    figures.append(_triple_plain(t, [(e, "East (m)"), (nth, "North (m)"), (u, "Up (m)")],
                                 _MAGENTA, "ENU Baseline Vector",
                                 "Rover ENU Position (relative to reference station)"))
    # Convergence curve (log y).
    ac = Axes(title="3D Position Error vs. Late-Arc Mean Position", xlabel="GPS Time",
              ylabel="Distance to late-arc mean (m)", yscale="log")
    ac.traces = [Line(x=t, y=conv_err, color=_BLACK, width=1.4)]
    figures.append(Figure(name="Convergence to late-arc mean", axes=[ac], width=11, height=3.6))

    # 1σ evolution (log y).
    asig = Axes(title="Position Uncertainty (Filter Covariance Diagonal)",
                xlabel="GPS Time", ylabel="1σ (m)", yscale="log", legend=Legend("best"))
    asig.traces = [Line(x=t, y=xs, color=_BLUE, label="σ_x"),
                   Line(x=t, y=ys, color=_GREEN, label="σ_y"),
                   Line(x=t, y=zs, color=_RED, label="σ_z")]
    figures.append(Figure(name="Position 1σ", axes=[asig], width=11, height=3.6))

    # DD throughput.
    if dds_applied is not None or dds_built is not None:
        ax = Axes(title="Double-Difference Measurement Throughput", xlabel="GPS Time",
                  ylabel="DD count / epoch", legend=Legend("best"))
        if dds_built is not None:
            ax.traces.append(Line(x=t, y=dds_built, color=(0.6, 0.6, 0.6), width=1.0,
                                  label="DDs built (pre-update)"))
        if dds_applied is not None:
            ax.traces.append(Line(x=t, y=dds_applied, color=_BLUE, width=1.2,
                                  label="DDs applied (post-rebuild)"))
        figures.append(Figure(name="DD Throughput", axes=[ax], width=11, height=3.6))

    # DD ambiguity state count.
    if n_ambs is not None and np.nansum(n_ambs > 0) > 0:
        ax = Axes(title="Active DD Phase Ambiguities", xlabel="GPS Time",
                  ylabel="# DD ambiguity states")
        ax.traces = [Line(x=t, y=n_ambs, color=_BLACK, width=1.2)]
        figures.append(Figure(name="DD Ambiguity States", axes=[ax], width=11, height=3.6))

    # Baseline length.
    ax = Axes(title="Rover-to-Reference Baseline Length per Epoch", xlabel="GPS Time",
              ylabel="Baseline length (km)")
    ax.traces = [Line(x=t, y=baseline_len / 1000.0, color=_BLUE, width=1.2)]
    figures.append(Figure(name="Baseline Length", axes=[ax], width=11, height=3.6))

    return ViewerResult(figures=figures, summary=_summary(t, n, i0, baseline_len,
                                                          conv_err, xs, ys, zs,
                                                          xf, yf, zf))


def _arr(s):
    return None if s is None else s.to_numpy(float)


def _triple_sigma(t, series, name, title):
    axes = []
    for i, (v, s, ylab) in enumerate(series):
        ax = Axes(ylabel=ylab, xlabel=("GPS Time" if i == len(series) - 1 else ""),
                  title=(title if i == 0 else ""))
        ax.traces = [Line(x=t, y=v, color=_BLUE, width=1.2),
                     Line(x=t, y=v + s, color=_REDDASH, style="--", width=1.0,
                          show_in_legend=False),
                     Line(x=t, y=v - s, color=_REDDASH, style="--", width=1.0,
                          show_in_legend=False)]
        axes.append(ax)
    return Figure(name=name, axes=axes, width=11, height=6.6)


def _triple_plain(t, series, color, name, title):
    axes = []
    for i, (v, ylab) in enumerate(series):
        ax = Axes(ylabel=ylab, xlabel=("GPS Time" if i == len(series) - 1 else ""),
                  title=(title if i == 0 else ""))
        ax.traces = [Line(x=t, y=v, color=color, width=1.2)]
        axes.append(ax)
    return Figure(name=name, axes=axes, width=11, height=6.6)


def _summary(t, n, i0, blen, conv_err, xs, ys, zs, xf, yf, zf) -> str:
    rms = float(np.sqrt(np.mean(conv_err[i0:] ** 2)))
    return "\n".join([
        "=== Baseline trace summary ===",
        f"  epochs:                 {n}",
        f"  time span:              {t.iloc[0]}  ->  {t.iloc[-1]}",
        f"  late-arc mean ECEF:     ({xf:.4f}, {yf:.4f}, {zf:.4f}) m",
        f"  late-arc baseline len:  {np.mean(blen[i0:]) / 1000:.4f} km",
        f"  late-arc 3D RMS:        {rms:.4f} m",
        f"  late-arc 1σ (x,y,z) m:  ({np.mean(xs[i0:]):.4f}, "
        f"{np.mean(ys[i0:]):.4f}, {np.mean(zs[i0:]):.4f})",
    ])


viewer = Viewer(name="baseline", title="DD baseline rover trace",
                required_csvs=("baseline_trace.csv",), build=build_baseline)

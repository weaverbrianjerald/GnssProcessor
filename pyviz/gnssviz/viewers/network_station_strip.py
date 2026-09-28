"""Per-station network residual-field viewer (Card L13, pyviz half).

Reads ``network_station_residuals.csv``, the PPP-RTK network producer's
per-(station, satellite, epoch) post-fit residual field, and renders two views
of the station health:

  1. **Station health raster**: a raw (station x epoch) ``imagesc`` grid whose
     cell value is the RMS of ``s_norm`` over that station's satellites at that
     epoch (a single "how noisy is this station right now" scalar).  Stations are
     sorted by their overall RMS of ``s_norm`` with the worst on top (row 0 is
     drawn at the top of the raster), so a chronically noisy or thin station reads
     at a glance.  Coloured with the sequential **viridis** map: the residual
     magnitudes are absolute / non-negative (``s_raw``/``s_norm`` are non-negative
     magnitudes), so a sequential ramp is correct; there is no sign to a diverging
     map.
  2. **Worst-station drill-down**: for the single worst station, a per-(sat,
     epoch) strip built through the shared :func:`build_sat_epoch_strip` renderer
     (``colormap="viridis"``, ``caxis="auto"``), so the offending satellites /
     epochs behind that station's RMS are visible individually.

CSV schema (exact header):
``week,tow,station,sat,s_raw,s_norm,sigma0``
where ``s_raw`` / ``s_norm`` are non-negative raw and normalized residual
magnitudes and ``sigma0`` the a-priori sigma used to normalize.

Coverage note: rows exist only for non-pivot stations that carry ambiguities;
the network pivot station is the differencing reference and does not appear
here.  Rows are also only written on RAIM (residual-QC) epochs, so the
expected row count is roughly ``sats x non-pivot-stations x RAIM-epochs``, a
heavy file.  The viewer is therefore ``default_enabled=False`` and only runs
via ``--only network_station_strip`` / ``--all``.

Metric convention (matches the network clock-stat read-outs): per station,
report the median, std and worst-satellite (by name) of ``s_norm``, and a
top-3 ranking of the worst stations.
"""

from __future__ import annotations

from typing import List

import numpy as np
import pandas as pd

from ..colormaps import strip_colormap
from ..identifiers import system_of
from ..io import add_utc, load_trace, require_cols
from ..strip import build_sat_epoch_strip
from .base import Viewer, ViewerResult, ViewerSkip

_SYS_ORDER = ("GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS")


def build_network_station_strip(ctx) -> ViewerResult:
    """Build the station-health raster + worst-station drill-down strip."""
    df = load_trace(ctx.path("network_station_residuals.csv"))
    require_cols(df, ("week", "tow", "station", "sat", "s_raw", "s_norm",
                      "sigma0"), "network_station_residuals.csv")
    if df.empty:
        raise ViewerSkip("network_station_residuals.csv has no rows")

    df = df.copy()
    df["station"] = df["station"].astype(str)
    df["sat"] = df["sat"].astype(str)
    df["s_norm"] = df["s_norm"].to_numpy(dtype=float)
    df = add_utc(df)

    # Drop rows with no usable normalized residual -- they carry no signal for
    # either the raster RMS or the ranking.
    df = df[np.isfinite(df["s_norm"].to_numpy(float))]
    if df.empty:
        raise ViewerSkip("network_station_residuals.csv has no finite s_norm rows")

    # --- Rank stations by overall RMS of s_norm (worst first) -------------- #
    def _rms(v: np.ndarray) -> float:
        v = np.asarray(v, float)
        v = v[np.isfinite(v)]
        return float(np.sqrt(np.mean(v * v))) if v.size else float("nan")

    station_rms = (df.groupby("station")["s_norm"]
                   .apply(lambda s: _rms(s.to_numpy(float))))
    # Worst (largest RMS) first; NaN RMS sorts last.
    ranked = list(station_rms.sort_values(ascending=False, na_position="last").index)

    # --- (1) Station-health raster: raw Raster/ColorBar/Axes --------------- #
    epochs = np.sort(pd.unique(df["utc"]))
    n_epochs = epochs.size
    epoch_pos = {int(k): j for j, k in enumerate(
        epochs.astype("datetime64[ns]").astype("int64"))}

    # Per-(station, epoch) RMS over that station's sats.
    per_se = (df.groupby(["station", "utc"])["s_norm"]
              .apply(lambda s: _rms(s.to_numpy(float)))
              .reset_index(name="rms"))

    n_sta = len(ranked)
    sta_pos = {s: i for i, s in enumerate(ranked)}     # row 0 == worst == TOP
    grid = np.full((n_sta, n_epochs), np.nan)
    ri = per_se["station"].map(sta_pos).to_numpy()
    ci = pd.Series(per_se["utc"].to_numpy("datetime64[ns]").astype("int64")).map(
        epoch_pos).to_numpy()
    vv = per_se["rms"].to_numpy(float)
    ok = ~pd.isna(ri) & ~pd.isna(ci)
    grid[ri[ok].astype(int), ci[ok].astype(int)] = vv[ok]

    from ..figures import Axes, ColorBar, Figure, Raster
    finite = grid[np.isfinite(grid)]
    clim = (float(finite.min()), float(finite.max())) if finite.size else (0.0, 1.0)
    if not (clim[1] > clim[0]):
        clim = (clim[0], clim[0] + 1.0)

    raster_ax = Axes(
        traces=[Raster(x=epochs, row_labels=ranked, grid=grid)],
        xlabel="GPS Time", ylabel="Station (worst on top)",
        title=(f"Per-station normalized-residual RMS  "
               f"(stations={n_sta}, epochs={n_epochs})"),
    )
    raster_fig = Figure(
        name="Network station health",
        title="PPP-RTK network per-station residual field",
        subtitle="cell = RMS of |s_norm| over that station's sats",
        axes=[raster_ax],
        width=14.0, height=min(9.5, 0.34 * n_sta + 1.8),
        cmap=strip_colormap("viridis", 256), clim=clim,
        colorbar=ColorBar(label="RMS |s_norm| (σ)"),
    )
    figures: List = [raster_fig]

    # --- (2) Worst-station drill-down strip -------------------------------- #
    worst = ranked[0] if ranked else None
    if worst is not None:
        d_w = df[df["station"] == worst]
        long = _melt(d_w)
        if not long.empty:
            figs = build_sat_epoch_strip(
                long,
                mode="continuous",
                colormap="viridis",       # non-negative residuals -> sequential map
                diverging=False,
                caxis="auto",
                fig_title=f"Worst station {worst} -- normalized residual |s_norm|",
                value_label="|s_norm| (σ)",
                fig_tag="net-station-worst",
                sys_order=_SYS_ORDER,
            )
            figures.extend(figs)

    return ViewerResult(figures=figures,
                        summary=_summary(df, ranked, station_rms))


def _melt(df: pd.DataFrame) -> pd.DataFrame:
    """Long-form (system, satellite, facet, utc, value) table for the strip.

    No signal facet is emitted (one row per (sat, epoch)); ``facet`` is blank.
    """
    d = pd.DataFrame({
        "system": df["sat"].map(system_of).astype(str),
        "satellite": df["sat"].astype(str),
        "facet": "",
        "utc": df["utc"],
        "value": df["s_norm"].to_numpy(dtype=float),
    })
    return d[np.isfinite(d["value"].to_numpy(float))]


def _summary(df: pd.DataFrame, ranked, station_rms) -> str:
    """Per-station median + std + worst-satellite (by name) of ``s_norm``.

    Std is the MATLAB sample std (ddof=1; 0.0 for a single sample).  ``worst-sat``
    is the satellite whose mean ``s_norm`` is largest for that station.
    """
    epochs = np.sort(pd.unique(df["utc"]))
    n_epochs = epochs.size
    out = ["=== PPP-RTK Network Per-Station Residual Field ==="]
    out.append(f"Stations   : {df['station'].nunique()} (non-pivot only)")
    out.append(f"Epochs     : {n_epochs}")
    if n_epochs:
        out.append(f"Span (GPS) : {pd.Timestamp(epochs[0])} -> "
                   f"{pd.Timestamp(epochs[-1])}")
    out.append(f"Rows       : {len(df)}")
    out.append("")

    top3 = ranked[:3]
    if top3:
        out.append("Top-3 worst stations (by RMS |s_norm|):")
        for i, sta in enumerate(top3, 1):
            r = station_rms.get(sta, float("nan"))
            out.append(f"  {i}. {sta:<12} RMS={r:.3f} σ")
        out.append("")

    out.append(f"{'Station':<12} {'N':>7} {'medS':>9} {'stdS':>9} {'worstSat':>9}")
    out.append("-" * 50)
    for sta in ranked:
        d = df[df["station"] == sta]
        v = d["s_norm"].to_numpy(float)
        v = v[np.isfinite(v)]
        if v.size == 0:
            continue
        med = float(np.median(v))
        std = float(np.std(v, ddof=1)) if v.size > 1 else 0.0
        by_sat = d.groupby("sat")["s_norm"].mean()
        worst_sat = str(by_sat.idxmax()) if not by_sat.empty else "-"
        out.append(f"{sta:<12} {v.size:>7d} {med:>9.3f} {std:>9.3f} "
                   f"{worst_sat:>9}")
    return "\n".join(out)


viewer = Viewer(
    name="network_station_strip",
    title="Network per-station residual field",
    required_csvs=("network_station_residuals.csv",),
    build=build_network_station_strip,
    # Heavy (~ sats x non-pivot-stations x RAIM-epochs rows); off by default.
    # Enable on demand with ``run_report.py --only network_station_strip`` / --all.
    default_enabled=False,
)

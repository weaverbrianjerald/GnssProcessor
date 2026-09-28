"""Network ionosphere pierce-point map: the network's own estimated VTEC.

Consumes ``network_ionosphere.csv`` (per-station, per-satellite back-substituted
slant ionosphere plus elevation/azimuth, emitted when ``network.trace_ionosphere``
is on) and the station roster, and renders the network's estimated ionosphere as
**ionospheric pierce points** on a world map: each (station, satellite) slant
delay is mapped to its thin-shell pierce point and converted to vertical TEC, so
the whole network paints a snapshot of the ionosphere it sees.

Three products:

  1. **Coverage**: every pierce point over the whole run, coloured by VTEC, the
     spatial footprint of the network's ionospheric sampling.
  2. **Snapshot**: one epoch's pierce points over the faint CODE IONEX VTEC
     field (when a GIM is found), so the network estimate can be compared
     against the published global map on the same colour scale.
  3. **Animation**: a GIF stepping through epochs (the "ionosphere movie").

VTEC is ``slant_iono(L1, m) / K_L1 * cos(z')``: the slant delay converted to
STEC (TECU) and de-projected through the thin-shell mapping the C++ pierce-point
code uses (Re=6371 km, H=450 km).  Skips (does not fail) when the iono trace or
the station roster/config is absent.
"""

from __future__ import annotations

import datetime as _dt
from typing import Dict, List, Optional, Tuple

import numpy as np

from ..animation import Animation
from ..figures import Axes, ColorBar, Figure, Legend, Mesh, Scatter, TextBox
from ..geodesy import ionospheric_pierce_point
from ..io import add_utc, load_trace, require_cols
from ..network_config import load_stations
from ..colormaps import strip_colormap
from .base import Viewer, ViewerResult, ViewerSkip

# L1 slant ionospheric delay (m) per 1 TECU of slant TEC: 40.308*1e16 / f_L1^2.
_F_L1 = 1.57542e9
_K_L1 = 40.308e16 / (_F_L1 * _F_L1)        # ~0.16242 m / TECU
_RE_KM = 6371.0
_H_KM = 450.0
_MAX_FRAMES = 36                            # animation frame cap (epochs subsampled)
_MAX_POINTS = 200_000                       # render cap for the coverage scatter


# --------------------------------------------------------------------------- #
def build_network_iono_map(ctx) -> ViewerResult:
    df = load_trace(ctx.path("network_ionosphere.csv"))
    require_cols(df, ("week", "tow", "station", "sat", "iono_m",
                      "elevation_deg", "azimuth_deg"), "network_ionosphere.csv")
    if df.empty:
        raise ViewerSkip("network_ionosphere.csv has no rows "
                         "(enable network.trace_ionosphere on the producer)")

    stations = load_stations(ctx.path("network_stations.csv"), ctx.network_config)
    if not stations:
        raise ViewerSkip("no station roster/config — cannot place pierce points")
    pos: Dict[str, Tuple[float, float]] = {s.id: (s.lat, s.lon) for s in stations}

    df = df.copy()
    df["station"] = df["station"].astype(str)
    df["sat"] = df["sat"].astype(str)
    df = df[df["station"].isin(pos)]
    if df.empty:
        raise ViewerSkip("no iono rows match a known station position")

    rx_lat = df["station"].map(lambda s: pos[s][0]).to_numpy(float)
    rx_lon = df["station"].map(lambda s: pos[s][1]).to_numpy(float)
    el = df["elevation_deg"].to_numpy(float)
    az = df["azimuth_deg"].to_numpy(float)
    iono = df["iono_m"].to_numpy(float)

    ipp_lat, ipp_lon = ionospheric_pierce_point(rx_lat, rx_lon, az, el,
                                                h_ion_km=_H_KM, re_km=_RE_KM)
    df["ipp_lat"] = ipp_lat
    df["ipp_lon"] = ipp_lon
    df["vtec"] = _slant_to_vtec(iono, el)
    df = add_utc(df)
    df = df[np.isfinite(df["ipp_lat"]) & np.isfinite(df["ipp_lon"])
            & np.isfinite(df["vtec"])]
    if df.empty:
        raise ViewerSkip("no finite pierce points after geometry")

    # Shared robust colour scale across all three products.
    v = df["vtec"].to_numpy(float)
    clim = (max(0.0, float(np.percentile(v, 2))), float(np.percentile(v, 98)))
    if not (clim[1] > clim[0]):
        clim = (float(np.nanmin(v)), float(np.nanmax(v)) + 1.0)
    cmap = strip_colormap("viridis", 256)

    ionex = _load_ionex(ctx)

    figures: List[Figure] = [
        _coverage_figure(df, stations, cmap, clim),
        _snapshot_figure(df, stations, cmap, clim, ionex),
    ]
    anim = _animation(df, stations, cmap, clim, ionex)
    animations = [anim] if anim is not None else []

    return ViewerResult(figures=figures,
                        summary=_summary(df, stations, clim, ionex),
                        animations=animations)


# --------------------------------------------------------------------------- #
def _slant_to_vtec(iono_m, el_deg):
    """L1 slant iono delay (m) -> vertical TEC (TECU) via the thin-shell mapping."""
    stec = np.asarray(iono_m, float) / _K_L1                 # TECU (slant)
    el = np.radians(np.asarray(el_deg, float))
    sin_zp = (_RE_KM / (_RE_KM + _H_KM)) * np.cos(el)
    cos_zp = np.sqrt(np.maximum(0.0, 1.0 - sin_zp ** 2))     # = 1 / mapping factor
    with np.errstate(invalid="ignore"):
        vtec = stec * cos_zp
    bad = ~np.isfinite(np.asarray(el_deg, float)) | (np.asarray(el_deg, float) <= 0)
    return np.where(bad, np.nan, vtec)


def _geo_axes(title, cmap, clim, *, colorbar=False, legend=False) -> Axes:
    ax = Axes(projection="geo", title=title, cmap=cmap, clim=clim,
              x_is_datetime=False, grid=False, coastline=True, graticule_deg=30.0,
              geo_extent=(-180.0, 180.0, -90.0, 90.0))
    if colorbar:
        ax.colorbar = ColorBar(label="VTEC (TECU)")
    if legend:
        ax.legend = Legend(loc="lower left")
    return ax


def _station_markers(stations, *, size=10) -> Scatter:
    return Scatter(x=[s.lon for s in stations], y=[s.lat for s in stations],
                   color=(0.0, 0.0, 0.0), marker="^", size=size,
                   edgecolor=(1.0, 1.0, 1.0), linewidth=0.4, zorder=6,
                   show_in_legend=False)


def _ionex_mesh(ionex, when: Optional[_dt.datetime], *, alpha=0.45) -> Optional[Mesh]:
    if ionex is None:
        return None
    from ..ionex import nearest_map
    m = nearest_map(ionex, when) if when is not None else (ionex.maps[0]
                                                           if ionex.maps else None)
    if m is None:
        return None
    return Mesh(lon=ionex.lons, lat=ionex.lats, grid=m.vtec, alpha=alpha, zorder=0)


# --------------------------------------------------------------------------- #
def _coverage_figure(df, stations, cmap, clim) -> Figure:
    d = df
    note = ""
    if len(d) > _MAX_POINTS:
        d = d.sample(_MAX_POINTS, random_state=0)
        note = f"  (subsampled to {_MAX_POINTS:,} of {len(df):,} pierce points)"
    ax = _geo_axes(f"Ionospheric pierce-point coverage — network VTEC{note}",
                   cmap, clim, colorbar=True)
    ax.traces = [
        Scatter(x=d["ipp_lon"].to_numpy(float), y=d["ipp_lat"].to_numpy(float),
                c=d["vtec"].to_numpy(float), size=6, marker="o", alpha=0.55,
                zorder=4, show_in_legend=False),
        _station_markers(stations),
    ]
    return Figure(name="Iono pierce-point coverage",
                  title="Network ionospheric pierce-point coverage (VTEC)",
                  axes=[ax], width=13.0, height=7.2, link_x=False)


def _snapshot_figure(df, stations, cmap, clim, ionex) -> Figure:
    # The best-covered epoch makes the clearest single-frame snapshot.
    epoch = df.groupby("utc").size().idxmax()
    d = df[df["utc"] == epoch]
    backdrop = _ionex_mesh(ionex, _as_pydatetime(epoch))
    title = (f"Ionosphere snapshot @ {epoch} — {len(d)} pierce points"
             + ("  (over CODE IONEX field)" if backdrop is not None else ""))
    ax = _geo_axes(title, cmap, clim, colorbar=True)
    traces = []
    if backdrop is not None:
        traces.append(backdrop)
    traces.append(Scatter(x=d["ipp_lon"].to_numpy(float),
                          y=d["ipp_lat"].to_numpy(float),
                          c=d["vtec"].to_numpy(float), size=42, marker="o",
                          edgecolor=(1.0, 1.0, 1.0), linewidth=0.4, zorder=5,
                          show_in_legend=False))
    traces.append(_station_markers(stations))
    ax.traces = traces
    return Figure(name="Iono snapshot", title="Network ionosphere snapshot",
                  axes=[ax], width=13.0, height=7.2, link_x=False)


def _animation(df, stations, cmap, clim, ionex) -> Optional[Animation]:
    epochs = sorted(df["utc"].unique())
    if len(epochs) < 2:
        return None
    if len(epochs) > _MAX_FRAMES:
        idx = np.linspace(0, len(epochs) - 1, _MAX_FRAMES).round().astype(int)
        epochs = [epochs[i] for i in sorted(set(idx))]
    frames: List[Figure] = []
    markers = _station_markers(stations, size=8)
    for ep in epochs:
        d = df[df["utc"] == ep]
        backdrop = _ionex_mesh(ionex, _as_pydatetime(ep), alpha=0.40)
        ax = _geo_axes(f"Network ionosphere VTEC @ {ep}", cmap, clim, colorbar=True)
        traces = []
        if backdrop is not None:
            traces.append(backdrop)
        traces.append(Scatter(x=d["ipp_lon"].to_numpy(float),
                              y=d["ipp_lat"].to_numpy(float),
                              c=d["vtec"].to_numpy(float), size=34, marker="o",
                              edgecolor=(1.0, 1.0, 1.0), linewidth=0.3, zorder=5,
                              show_in_legend=False))
        traces.append(markers)
        ax.traces = traces
        frames.append(Figure(name=f"iono_{ep}", axes=[ax], width=12.0, height=6.6,
                             link_x=False))
    return Animation(name="Iono movie",
                     title="Network ionosphere VTEC over epochs",
                     frames=frames, fps=4.0,
                     summary=f"{len(frames)} frames spanning {epochs[0]} .. {epochs[-1]}")


# --------------------------------------------------------------------------- #
def _load_ionex(ctx):
    path = ctx.find_ionex()
    if not path:
        return None
    try:
        from ..ionex import read_ionex
        data = read_ionex(path)
        return data if data.maps else None
    except (OSError, ValueError):
        return None


def _as_pydatetime(ts) -> Optional[_dt.datetime]:
    try:
        import pandas as pd
        return pd.Timestamp(ts).to_pydatetime()
    except Exception:
        return None


def _summary(df, stations, clim, ionex) -> str:
    out = ["=== Network Ionosphere Pierce-Point Map ==="]
    out.append(f"Pierce points  : {len(df):,}")
    out.append(f"Stations       : {df['station'].nunique()} "
               f"(roster has {len(stations)})")
    out.append(f"Satellites     : {df['sat'].nunique()}")
    out.append(f"Epochs         : {df['utc'].nunique()} "
               f"({df['utc'].min()} .. {df['utc'].max()})")
    v = df["vtec"].to_numpy(float)
    out.append(f"VTEC (TECU)    : min {np.nanmin(v):.1f}, median {np.nanmedian(v):.1f}, "
               f"max {np.nanmax(v):.1f}  (colour scale {clim[0]:.1f}..{clim[1]:.1f})")
    out.append(f"IONEX backdrop : {'CODE GIM found' if ionex is not None else 'none'}")
    out.append("")
    out.append(f"{'Station':<10} {'PP':>8} {'medVTEC':>9} {'medEl':>7}")
    out.append("-" * 38)
    for sid in sorted(df["station"].unique()):
        d = df[df["station"] == sid]
        out.append(f"{sid:<10} {len(d):>8} "
                   f"{np.nanmedian(d['vtec']):>9.1f} "
                   f"{np.nanmedian(d['elevation_deg']):>7.1f}")
    return "\n".join(out)


viewer = Viewer(name="network_iono_map",
                title="Network ionosphere pierce-point map (VTEC)",
                required_csvs=("network_ionosphere.csv",),
                build=build_network_iono_map)

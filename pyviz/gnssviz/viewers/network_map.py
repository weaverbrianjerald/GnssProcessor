"""Network worldmap viewer -- stations + baselines on an equirectangular map.

Draws the PPP-RTK NETWORK geometry: every estimating peer station, the
clock-datum pivot, and (when present) the held-out closed-loop consumer / user
station, on a world coastline, with great-circle baselines fanning out from the
datum (the star topology the network time reference defines).

Station positions / roles come from :func:`gnssviz.network_config.load_stations`
-- preferring the producer-emitted ``network_stations.csv`` roster (it reflects
the stations that actually loaded), falling back to the run config JSON passed
via ``TraceContext.network_config``.  With neither, the viewer skips (it has no
geometry to draw), like a missing-CSV viewer.

The figure auto-frames the network: a regional cluster zooms to its bounding box
(padded), a global IGS network shows the whole world.
"""

from __future__ import annotations

from typing import List

import numpy as np

from ..figures import Axes, Figure, Legend, Line, Scatter, TextBox
from ..geodesy import great_circle
from ..network_config import (ROLE_DATUM, ROLE_HELD_OUT, ROLE_NETWORK,
                              Station, load_stations)
from .base import Viewer, ViewerResult, ViewerSkip

# Per-role marker styling (color, matplotlib marker, point size, z-order).
_STYLE = {
    ROLE_NETWORK:  {"color": (0.13, 0.40, 0.70), "marker": "o", "size": 42,  "z": 5},
    ROLE_DATUM:    {"color": (0.96, 0.70, 0.10), "marker": "*", "size": 320, "z": 7},
    ROLE_HELD_OUT: {"color": (0.86, 0.16, 0.16), "marker": "^", "size": 130, "z": 6},
}
_ROLE_LABEL = {ROLE_NETWORK: "network station", ROLE_DATUM: "clock datum",
               ROLE_HELD_OUT: "held-out user"}
_LABEL_ALL_MAX = 18   # label every station only for small networks; else just datum/user


# --------------------------------------------------------------------------- #
def build_network_map(ctx, *, baselines: bool = True) -> ViewerResult:
    """Build the network station worldmap.

    Parameters
    ----------
    baselines:
        Draw great-circle arcs from the datum station to every peer (default on).
    """
    stations = load_stations(ctx.path("network_stations.csv"), ctx.network_config)
    if not stations:
        raise ViewerSkip("no network_stations.csv roster and no --network-config "
                         "(network station positions unavailable)")

    datum = next((s for s in stations if s.role == ROLE_DATUM), None)
    extent = _auto_extent(stations)

    # Title lives on the Figure (suptitle); leave the axes title empty to avoid
    # a duplicate heading.
    ax = Axes(projection="geo", title="", geo_extent=extent,
              legend=Legend(loc="lower left"), grid=False, x_is_datetime=False,
              coastline=True, graticule_deg=30.0)
    traces: list = []

    # Baselines from the datum (drawn first, under the markers).
    if baselines and datum is not None:
        for s in stations:
            if s.role == ROLE_DATUM or s.role == ROLE_HELD_OUT:
                continue
            blon, blat = great_circle(datum.lat, datum.lon, s.lat, s.lon, 60)
            traces.append(Line(x=blon, y=blat, color=(0.55, 0.62, 0.72),
                               width=0.4, show_in_legend=False))
        # Dashed lines of sight to the held-out user (distinct from peer baselines).
        for s in stations:
            if s.role != ROLE_HELD_OUT:
                continue
            blon, blat = great_circle(datum.lat, datum.lon, s.lat, s.lon, 60)
            traces.append(Line(x=blon, y=blat, color=(0.86, 0.40, 0.40),
                               width=0.6, style="--", show_in_legend=False))

    # Station markers, one Scatter per role so the legend reads cleanly.
    for role in (ROLE_NETWORK, ROLE_DATUM, ROLE_HELD_OUT):
        pts = [s for s in stations if s.role == role]
        if not pts:
            continue
        st = _STYLE[role]
        traces.append(Scatter(
            x=[p.lon for p in pts], y=[p.lat for p in pts],
            c=None, color=st["color"], marker=st["marker"], size=st["size"],
            edgecolor=(1.0, 1.0, 1.0), linewidth=0.6, zorder=st["z"],
            label=f"{_ROLE_LABEL[role]} ({len(pts)})"))

    # Labels: every station for a small network, else only datum + held-out.
    label_all = len(stations) <= _LABEL_ALL_MAX
    for s in stations:
        if label_all or s.role in (ROLE_DATUM, ROLE_HELD_OUT):
            traces.append(TextBox(x=s.lon + 1.5, y=s.lat + 1.5, text=s.label,
                                  data_coords=True, fontsize=7,
                                  color=(0.1, 0.1, 0.1), zorder=8,
                                  bold=(s.role != ROLE_NETWORK)))

    ax.traces = traces
    fig = Figure(name="Network map", title=_title(stations), axes=[ax],
                 width=13.0, height=_height_for(extent), link_x=False)

    return ViewerResult(figures=[fig], summary=_summary(stations, datum))


# --------------------------------------------------------------------------- #
def _title(stations: List[Station]) -> str:
    n_net = sum(s.role in (ROLE_NETWORK, ROLE_DATUM) for s in stations)
    n_user = sum(s.role == ROLE_HELD_OUT for s in stations)
    extra = f" + {n_user} held-out" if n_user else ""
    return f"PPP-RTK network — {n_net} stations{extra}"


def _auto_extent(stations: List[Station]):
    """Bounding box (lon_min,lon_max,lat_min,lat_max) padded; world if global."""
    lats = np.array([s.lat for s in stations], dtype=float)
    lons = np.array([s.lon for s in stations], dtype=float)
    lon_span = float(lons.max() - lons.min())
    lat_span = float(lats.max() - lats.min())
    # A network straddling the antimeridian or spanning most of the globe: world.
    if lon_span > 200.0 or lat_span > 130.0:
        return (-180.0, 180.0, -90.0, 90.0)
    pad_lon = max(8.0, 0.18 * lon_span)
    pad_lat = max(8.0, 0.18 * lat_span)
    lon0 = max(-180.0, float(lons.min()) - pad_lon)
    lon1 = min(180.0, float(lons.max()) + pad_lon)
    lat0 = max(-90.0, float(lats.min()) - pad_lat)
    lat1 = min(90.0, float(lats.max()) + pad_lat)
    if (lon1 - lon0) > 300.0 or (lat1 - lat0) > 160.0:
        return (-180.0, 180.0, -90.0, 90.0)
    return (lon0, lon1, lat0, lat1)


def _height_for(extent) -> float:
    """Figure height (inches) matching the geo aspect (equal lon/lat degrees)."""
    lon_span = extent[1] - extent[0]
    lat_span = extent[3] - extent[2]
    if lon_span <= 0:
        return 7.0
    h = 13.0 * (lat_span / lon_span) + 1.0
    return float(min(11.0, max(4.0, h)))


def _baseline_km(a: Station, b: Station) -> float:
    return float(np.sqrt((a.x - b.x) ** 2 + (a.y - b.y) ** 2 + (a.z - b.z) ** 2)) / 1000.0


def _summary(stations: List[Station], datum) -> str:
    out = ["=== PPP-RTK Network Map ==="]
    n_net = sum(s.role == ROLE_NETWORK for s in stations)
    out.append(f"Estimating peers : {n_net + (1 if datum else 0)}"
               + (f" (datum '{datum.id}')" if datum else " (no datum found)"))
    held = [s for s in stations if s.role == ROLE_HELD_OUT]
    if held:
        out.append(f"Held-out user(s) : {', '.join(s.id for s in held)}")
    lats = [s.lat for s in stations]
    lons = [s.lon for s in stations]
    out.append(f"Lat extent       : {min(lats):.1f} .. {max(lats):.1f} deg")
    out.append(f"Lon extent       : {min(lons):.1f} .. {max(lons):.1f} deg")

    if datum is not None:
        bl = sorted(_baseline_km(datum, s) for s in stations
                    if s.role == ROLE_NETWORK)
        if bl:
            out.append(f"Baselines from datum (km): min {bl[0]:.0f}, "
                       f"median {bl[len(bl) // 2]:.0f}, max {bl[-1]:.0f}")
        for s in held:
            out.append(f"Datum -> {s.id} baseline : {_baseline_km(datum, s):.0f} km")

    out.append("")
    out.append(f"{'Station':<10} {'Role':<12} {'Lat':>9} {'Lon':>10} {'H(m)':>9}")
    out.append("-" * 54)
    order = {ROLE_DATUM: 0, ROLE_NETWORK: 1, ROLE_HELD_OUT: 2}
    for s in sorted(stations, key=lambda s: (order.get(s.role, 9), s.id)):
        out.append(f"{s.id:<10} {s.role:<12} {s.lat:>9.3f} {s.lon:>10.3f} "
                   f"{s.height:>9.1f}")
    return "\n".join(out)


viewer = Viewer(name="network_map", title="Network map (stations & baselines)",
                required_csvs=(), build=build_network_map)

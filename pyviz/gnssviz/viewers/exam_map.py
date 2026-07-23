"""Geographic context — where the SAR vessel is, on a coastline map.

A *context* companion to :mod:`exam_track` (which shows the cm–dm accuracy in a
local ENU plan view): this plots the truth trajectory in geographic lat/lon over
the vendored Natural-Earth coastline, so a grader sees the scenario concretely —
a vessel adrift in the South Pacific near Fiji.

  1. **World context** — the whole coastline with a marker at the vessel's mean
     position (the ~10 km track is a dot at world scale; the marker says *where*).
  2. **Regional zoom** — a **satellite-imagery basemap** of the Fiji region + the
     truth track near the vessel, with start/end markers and the storm-window
     segment highlighted.  The imagery is vendored offline
     (:mod:`gnssviz.basemap`, ``scripts/fetch_satellite.py``); when it is
     unavailable the panel falls back to the filled land/ocean coastline basemap.

Deliberately not a map *projection*: it draws on an ordinary axes with
``aspect_equal`` (which both backends honour, unlike the ``geo`` projection the
plotly dashboard can't render).  The satellite raster is EPSG:4326 (plate
carrée), so lon/lat map linearly to x/y and it aligns with the track.  Because the
scenario sits on the ±180° dateline (Fiji spans it), the regional panel uses a
**0–360° longitude** convention so the land is contiguous instead of split across
both map edges.  The ~10 km vessel motion is invisible against country outlines,
so truth vs recovered is NOT shown here — that stays in ``exam_track``.
"""

from __future__ import annotations

import numpy as np

from ..basemap import extent_overlaps, load_basemap
from ..coastline import load_coastline
from ..figures import (Axes, Figure, Legend, Line, Polygon, RgbImage, Scatter,
                       TextBox)
from ..io import first_col
from .base import Viewer, ViewerResult, ViewerSkip
from . import exam_common as ec

_C_OCEAN = (0.82, 0.90, 0.96)   # basemap ocean
_C_LAND = (0.91, 0.89, 0.80)    # basemap land
_C_COAST = (0.45, 0.45, 0.45)
_C_TRACK = (0.10, 0.25, 0.60)
_C_TRACK_IMG = (1.00, 0.90, 0.15)   # bright track over dark satellite imagery
_C_START = (0.00, 0.55, 0.00)
_C_END = (0.85, 0.15, 0.15)
_C_MARK = (0.85, 0.15, 0.15)


def _rings_in_extent(clon, clat, x0, x1, y0, y1):
    """Coastline rings whose bounding box overlaps the map extent, NaN-separated.

    Fills only nearby land (e.g. the Fiji islands), dropping distant continents
    entirely — so their open-arc closing lines can never cross the view.  Call
    with the SAME lon convention as the extent (0–360° here, so a dateline-spanning
    island reads as one contiguous ring).  A small local island force-closes cleanly.
    """
    x = np.asarray(clon, float)
    y = np.asarray(clat, float)
    brk = np.flatnonzero(~(np.isfinite(x) & np.isfinite(y)))
    out_x, out_y = [], []
    start = 0
    for stop in list(brk) + [x.size]:
        rx, ry = x[start:stop], y[start:stop]
        m = np.isfinite(rx) & np.isfinite(ry)
        rx, ry = rx[m], ry[m]
        if (rx.size >= 3 and rx.min() <= x1 and rx.max() >= x0
                and ry.min() <= y1 and ry.max() >= y0):
            out_x.extend(rx.tolist() + [np.nan])
            out_y.extend(ry.tolist() + [np.nan])
        start = stop + 1
    return np.asarray(out_x), np.asarray(out_y)


def _storm_mask(ctx, df):
    """Boolean mask of rows inside the storm window (all-False if none)."""
    sc = ec.scalars(ctx)
    w0, w1 = sc.get("storm_window_start_tow_s"), sc.get("storm_window_end_tow_s")
    tow = first_col(df, "tow_s", "tow")
    try:
        w0, w1 = float(w0), float(w1)
    except (TypeError, ValueError):
        return np.zeros(len(df), dtype=bool)
    if tow is None or not (w1 > w0):
        return np.zeros(len(df), dtype=bool)
    t = tow.to_numpy(float)
    return (t >= w0) & (t <= w1)


def build_exam_map(ctx) -> ViewerResult:
    truth = ec.require_truth(ctx, "truth_position.csv")
    lat_c = first_col(truth, "lat_deg")
    lon_c = first_col(truth, "lon_deg")
    if lat_c is None or lon_c is None:
        raise ViewerSkip("truth_position.csv lacks lat_deg/lon_deg (no geographic track)")
    lat = lat_c.to_numpy(float)
    lon = lon_c.to_numpy(float)
    ok = np.isfinite(lat) & np.isfinite(lon)
    if not ok.any():
        raise ViewerSkip("no finite lat/lon in truth_position.csv")
    lat, lon = lat[ok], lon[ok]
    # Work in 0–360° longitude throughout: signed lon is discontinuous across the
    # ±180° dateline the track crosses, so np.mean/np.diff on it are meaningless.
    lon360 = lon % 360.0
    mean_lon360 = float(np.mean(lon360))
    mean_lon_signed = mean_lon360 - 360.0 if mean_lon360 > 180.0 else mean_lon360

    clon, clat = load_coastline()          # NaN-separated polylines (may be empty)
    have_coast = clon.size > 0
    figures = []

    # --- Figure 1: world context (signed lon), coastline + vessel marker -------
    # World panel is a clean coastline OUTLINE (not filled): the 110 m coastline's
    # ~13 open continent arcs would force-close into cross-map slivers if filled.
    # The filled land/ocean basemap is the regional panel below (its closed island
    # rings fill cleanly, and any stray closing line lies outside the extent).
    axw = Axes(title="Vessel location — world context", xlabel="Longitude (°)",
               ylabel="Latitude (°)", x_is_datetime=False, aspect_equal=True,
               grid=True, legend=Legend(loc="lower left"),
               xlim=(-180.0, 180.0), ylim=(-90.0, 90.0))
    if have_coast:
        axw.traces.append(Line(x=clon, y=clat, color=_C_COAST, width=0.6,
                               show_in_legend=False))
    axw.traces.append(Scatter(x=[mean_lon_signed], y=[float(np.mean(lat))],
                              color=_C_MARK, size=90, marker="*",
                              edgecolor=(0, 0, 0), linewidth=0.5, zorder=6,
                              label="SAR vessel"))
    figures.append(Figure(name="World context", axes=[axw], width=12, height=6.2))

    # --- Figure 2: regional zoom (0–360° lon, dateline-safe) -------------------
    pad = 3.0
    x0, x1 = float(lon360.min()) - pad, float(lon360.max()) + pad
    y0, y1 = float(lat.min()) - pad, float(lat.max()) + pad
    axr = Axes(title="SAR vessel track (regional — near Fiji, South Pacific)",
               xlabel="Longitude (°E, 0–360)", ylabel="Latitude (°)",
               x_is_datetime=False, aspect_equal=True, grid=True,
               legend=Legend(loc="best"), facecolor=_C_OCEAN,
               xlim=(x0, x1), ylim=(y0, y1))
    # Basemap: prefer vendored SATELLITE IMAGERY (offline), else the filled
    # land/ocean coastline.  Both sit at zorder 0, under the track and markers.
    basemap = load_basemap()
    used_imagery = basemap is not None and extent_overlaps(basemap.extent, x0, x1, y0, y1)
    if used_imagery:
        axr.traces.append(RgbImage(rgb=basemap.rgb, extent=basemap.extent, zorder=0,
                                   encoded=basemap.encoded, mime=basemap.mime))
    elif have_coast:
        # Fill closed land rings in 0–360 lon (the ±180 dateline becomes contiguous,
        # 179→181, so Fiji fills whole; the Greenwich seam is far outside this
        # Pacific extent).  Only closed rings are filled (no continent-arc slivers).
        lx, ly = _rings_in_extent(clon % 360.0, clat, x0, x1, y0, y1)
        if lx.size:
            axr.traces.append(Polygon(x=lx, y=ly, fill=_C_LAND, edge=_C_COAST,
                                      width=0.7, zorder=0))
    # Truth track (quiet then storm-window segment recoloured).  Over dark
    # satellite imagery a bright track reads far better than the on-paper navy.
    track_color = _C_TRACK_IMG if used_imagery else _C_TRACK
    track_width = 2.0 if used_imagery else 1.6
    mask = _storm_mask(ctx, truth.iloc[np.flatnonzero(ok)])
    axr.traces.append(Line(x=lon360, y=lat, color=track_color, width=track_width,
                           label="truth track"))
    if mask.any():
        seg = np.where(mask, lon360, np.nan)
        axr.traces.append(Line(x=seg, y=np.where(mask, lat, np.nan),
                               color=ec.STORM_COLOR, width=3.0, label="storm window"))
    axr.traces.append(Scatter(x=[float(lon360[0])], y=[float(lat[0])], color=_C_START,
                              size=70, marker="o", edgecolor=(0, 0, 0), linewidth=0.5,
                              zorder=6, label="start"))
    axr.traces.append(Scatter(x=[float(lon360[-1])], y=[float(lat[-1])], color=_C_END,
                              size=70, marker="s", edgecolor=(0, 0, 0), linewidth=0.5,
                              zorder=6, label="end"))
    if used_imagery:
        # Required imagery credit — a small corner chip over the ocean.
        axr.traces.append(TextBox(x=0.99, y=0.015, text=f"Imagery: {basemap.attribution}",
                                  color=(0.15, 0.15, 0.15), ha="right", va="bottom",
                                  boxed=True, fontsize=6.5, zorder=7))
    elif not have_coast:
        axr.traces.append(TextBox(x=0.02, y=0.04, text="(coastline asset unavailable)",
                                  color=(0.6, 0.6, 0.6), va="bottom"))
    figures.append(Figure(name="Regional track", axes=[axr], width=9.5, height=8.0))

    crosses = bool((lon360.min() < 180.0) and (lon360.max() > 180.0))
    return ViewerResult(figures=figures,
                        summary=_summary(lat, lon360, mean_lon_signed, mean_lon360, crosses))


def _summary(lat, lon360, mean_lon_signed, mean_lon360, crosses) -> str:
    # Ground-track length in the CONTIGUOUS 0–360 lon (signed lon jumps at ±180).
    latm = np.radians(np.mean(lat))
    dx = np.diff(lon360) * 111320.0 * np.cos(latm)
    dy = np.diff(lat) * 110540.0
    length_km = float(np.sum(np.hypot(dx, dy))) / 1000.0
    return "\n".join([
        "=== Geographic context (truth track) ===",
        f"Epochs           : {lat.size}",
        f"Mean position    : {np.mean(lat):.4f}°, {mean_lon_signed:.4f}° "
        f"({mean_lon360:.4f}°E) — South Pacific, near Fiji",
        f"Lat span         : {lat.min():.4f} .. {lat.max():.4f}°",
        f"Lon span (0–360) : {lon360.min():.4f} .. {lon360.max():.4f}°E"
        + ("  — CROSSES the ±180° dateline" if crosses else ""),
        f"Ground-track len : {length_km:.3f} km",
        "(context only — cm/dm truth-vs-recovered accuracy is in the plan-view track panel)",
    ])


viewer = Viewer(name="exam_map", title="Geographic context (vessel track)",
                required_csvs=("truth_position.csv",), build=build_exam_map)

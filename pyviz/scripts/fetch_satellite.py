#!/usr/bin/env python3
"""Fetch + vendor the satellite-imagery basemap for the exam ``exam_map`` viewer.

The exam dashboard's *Geographic context* panel draws the SAR vessel track over a
basemap of the Fiji region.  By default that basemap is the vendored Natural-Earth
coastline (filled land / ocean).  This script fetches a real **satellite-imagery**
raster once and vendors it next to the coastline, so the dashboard can draw photo
imagery *fully offline* (embedded base64 in the HTML report) — no render-time
network, no cartopy/contextily/tile-server dependency (same philosophy as
``coastline.py``).  Run it only when (re)generating the asset; the committed
``assets/basemap_esri_fiji.*`` is what the report actually uses.

Source: Esri "World Imagery" (ArcGIS REST ``export``), EPSG:4326 (plate carree)
so lon/lat map linearly to x/y and the raster aligns pixel-for-pixel with the
viewer's equal-aspect *degrees* axes.  Attribution — required, and drawn on the
map by the viewer — is "Esri, Maxar, Earthstar Geographics".

Dateline: the Fiji scenarios straddle +/-180.  A single EPSG:4326 export cannot
span the dateline (it needs xmin < xmax), so we fetch the two halves either side
of 180 and stitch them.  The vendored extent is expressed in a **continuous
0-360 longitude** convention (176 .. 185 degE) — exactly the convention
``exam_map``'s regional panel plots in — so the seam at 180 is contiguous.

Usage (from the repo, with the project venv):
    venv/Scripts/python.exe pyviz/scripts/fetch_satellite.py
    venv/Scripts/python.exe pyviz/scripts/fetch_satellite.py --px-per-deg 150 --quality 85
"""
from __future__ import annotations

import argparse
import datetime as _dt
import io
import json
import os
import ssl
import urllib.parse
import urllib.request

import numpy as np
from PIL import Image

# --- vendored asset location (next to coastline_110m.json) ------------------ #
_ASSETS = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "gnssviz", "assets")
_STEM = "basemap_esri_fiji"
ATTRIBUTION = "Esri, Maxar, Earthstar Geographics"

_EXPORT = ("https://services.arcgisonline.com/arcgis/rest/services/"
           "World_Imagery/MapServer/export")

# Geographic box, continuous 0-360 longitude.  Chosen to cover every Fiji exam
# scenario's padded regional extent: origins at ~180.1 degE (sar_fiji) and
# ~181.8 degE (the -178.2 standard origin), each padded +/-3 deg, lat ~ -18.5.
LON0_360, LON1_360 = 176.0, 185.0     # west edge .. east edge (deg east)
LAT0, LAT1 = -23.0, -12.0             # south .. north (deg)
DATELINE_360 = 180.0                  # split here into two EPSG:4326 halves


def _export(bbox, size, fmt="jpg", quality=85, timeout=60):
    """One Esri EPSG:4326 export; returns a decoded ``(H, W, 3)`` uint8 array.

    ``bbox`` is ``(xmin, ymin, xmax, ymax)`` in signed degrees; ``size`` is
    ``(width_px, height_px)``.
    """
    q = {
        "bbox": ",".join(f"{v:.6f}" for v in bbox),
        "bboxSR": "4326", "imageSR": "4326",
        "size": f"{size[0]},{size[1]}",
        "format": fmt, "f": "image",
    }
    url = _EXPORT + "?" + urllib.parse.urlencode(q)
    req = urllib.request.Request(url, headers={"User-Agent": "gnssviz-basemap/1.0"})
    ctx = ssl.create_default_context()
    with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
        if r.status != 200:
            raise RuntimeError(f"Esri export HTTP {r.status} for bbox {bbox}")
        raw = r.read()
    ct = r.headers.get("Content-Type", "")
    if "image" not in ct:
        raise RuntimeError(f"Esri export returned {ct!r}, not an image (bbox {bbox}):\n"
                           f"{raw[:300]!r}")
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    return np.asarray(img, dtype=np.uint8)


def fetch(px_per_deg: float, quality: int) -> tuple[np.ndarray, dict]:
    """Fetch + stitch the two dateline halves; return ``(rgb_uint8, sidecar)``.

    ``rgb`` is north-up (row 0 = northmost, ``LAT1``), west-to-east across the
    columns from ``LON0_360`` to ``LON1_360``.
    """
    h_px = int(round((LAT1 - LAT0) * px_per_deg))
    # West half: [LON0_360, 180) in signed degrees == same numbers (all < 180).
    w_west_deg = DATELINE_360 - LON0_360
    w_west_px = int(round(w_west_deg * px_per_deg))
    west = _export((LON0_360, LAT0, DATELINE_360, LAT1), (w_west_px, h_px), quality=quality)
    # East half: [180, LON1_360] in 0-360 == [-180, LON1_360-360] signed.
    w_east_deg = LON1_360 - DATELINE_360
    w_east_px = int(round(w_east_deg * px_per_deg))
    east = _export((-180.0, LAT0, LON1_360 - 360.0, LAT1), (w_east_px, h_px), quality=quality)

    if west.shape[0] != east.shape[0]:      # guard: equal height for a clean seam
        h = min(west.shape[0], east.shape[0])
        west, east = west[:h], east[:h]
    rgb = np.concatenate([west, east], axis=1)

    sidecar = {
        "source": "Esri World Imagery (ArcGIS REST export)",
        "attribution": ATTRIBUTION,
        "lon_convention": "0-360",
        "extent_lon360": [LON0_360, LON1_360],
        "extent_lat": [LAT0, LAT1],
        "size_px": [int(rgb.shape[1]), int(rgb.shape[0])],
        "px_per_deg": px_per_deg,
        "dateline_seam_lon360": DATELINE_360,
        "fetched_utc": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "note": ("Two EPSG:4326 halves fetched either side of the 180 dateline and "
                 "stitched; extent is continuous 0-360 longitude to match exam_map."),
    }
    return rgb, sidecar


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--px-per-deg", type=float, default=150.0,
                    help="pixels per degree (default 150 => ~1350x1650)")
    ap.add_argument("--quality", type=int, default=85, help="JPEG quality (default 85)")
    ap.add_argument("--out-dir", default=_ASSETS, help="asset output dir")
    args = ap.parse_args(argv)

    print(f"Fetching Esri World Imagery for Fiji box "
          f"lon360 [{LON0_360}, {LON1_360}], lat [{LAT0}, {LAT1}] "
          f"@ {args.px_per_deg} px/deg ...")
    rgb, sidecar = fetch(args.px_per_deg, args.quality)
    os.makedirs(args.out_dir, exist_ok=True)
    jpg = os.path.join(args.out_dir, _STEM + ".jpg")
    Image.fromarray(rgb, "RGB").save(jpg, format="JPEG", quality=args.quality,
                                     optimize=True)
    side = os.path.join(args.out_dir, _STEM + ".json")
    with open(side, "w", encoding="utf-8") as fh:
        json.dump(sidecar, fh, indent=2)
    kb = os.path.getsize(jpg) / 1024.0
    print(f"  wrote {jpg}  ({rgb.shape[1]}x{rgb.shape[0]} px, {kb:.0f} KB)")
    print(f"  wrote {side}")
    print(f"  attribution: {ATTRIBUTION}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

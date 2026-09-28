#!/usr/bin/env python
"""Regenerate the vendored world-coastline asset used by the pyviz geo viewers.

The network-level visualizations (``viewers/network_map`` and the ionosphere
pierce-point map) draw a world coastline as their backdrop.  To keep the pyviz
suite installable with only numpy/pandas/matplotlib (the repo philosophy: no
cartopy / no pyproj / no shapely), the coastline is *vendored* as a compact
JSON polyline list at ``pyviz/gnssviz/assets/coastline_110m.json`` rather than
parsed from a shapefile at runtime.

This script (re)builds that asset from Natural Earth's public-domain 110 m
coastline GeoJSON.  Run it only when refreshing the asset: the committed JSON
is what the viewers load; no network access happens at render time.

    python scripts/fetch_coastline.py            # default 110 m coastline
    python scripts/fetch_coastline.py --round 1  # coarser (smaller asset)

Natural Earth is public domain (https://www.naturalearthdata.com/about/terms-of-use/).
"""

from __future__ import annotations

import argparse
import json
import os
import urllib.request

# Raw GeoJSON from the canonical Natural Earth vector mirror (public domain).
_NE_URL = ("https://raw.githubusercontent.com/nvkelso/natural-earth-vector/"
           "master/geojson/ne_110m_coastline.geojson")

_ASSET = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "pyviz", "gnssviz", "assets", "coastline_110m.json")


def build(url: str = _NE_URL, out_path: str = _ASSET, ndigits: int = 2) -> str:
    """Download the NE coastline GeoJSON and write the compact polyline asset.

    The asset is ``{"lon": [...], "lat": [...]}``: two flat arrays where a
    ``None`` (rendered as NaN) separates successive polylines, so the whole
    coastline draws with a single gap-aware ``plot`` call.  Coordinates are
    rounded to ``ndigits`` decimals (2 ≈ 1 km, ample for a world map).
    """
    with urllib.request.urlopen(url, timeout=60) as fh:   # noqa: S310 (trusted host)
        gj = json.loads(fh.read().decode("utf-8"))

    lon: list = []
    lat: list = []

    def add(coords):
        for x, y in coords:
            lon.append(round(float(x), ndigits))
            lat.append(round(float(y), ndigits))
        lon.append(None)
        lat.append(None)

    n_lines = 0
    for feat in gj["features"]:
        geom = feat["geometry"]
        if geom["type"] == "LineString":
            add(geom["coordinates"]); n_lines += 1
        elif geom["type"] == "MultiLineString":
            for part in geom["coordinates"]:
                add(part); n_lines += 1

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    payload = {"name": "ne_110m_coastline",
               "license": "public domain (Natural Earth)",
               "lon": lon, "lat": lat}
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, separators=(",", ":"))

    n_pts = sum(1 for v in lon if v is not None)
    print(f"wrote {out_path}: {n_lines} polylines, {n_pts} vertices, "
          f"{os.path.getsize(out_path)} bytes")
    return out_path


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default=_NE_URL, help="source GeoJSON URL")
    ap.add_argument("--out", default=_ASSET, help="output asset path")
    ap.add_argument("--round", type=int, default=2, dest="ndigits",
                    help="decimal places to round coordinates (default 2)")
    args = ap.parse_args(argv)
    build(args.url, os.path.abspath(args.out), args.ndigits)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

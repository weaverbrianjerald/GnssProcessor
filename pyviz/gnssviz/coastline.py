"""Vendored world-coastline loader for the geo viewers.

The geographic viewers (``viewers/network_map`` and the ionosphere pierce-point
map) draw a world coastline as their backdrop.  Rather than depend on a heavy
GIS stack (cartopy / pyproj / shapely), which the suite's numpy/pandas/matplotlib-only
design avoids, the coastline is *vendored* as a compact polyline JSON under
``assets/coastline_110m.json`` (Natural Earth 110 m, public domain; regenerate
with ``scripts/fetch_coastline.py``).

:func:`load_coastline` returns two ``float`` arrays ``(lon, lat)`` with ``NaN``
separators between polylines, so the whole coastline draws with one gap-aware
``plot(lon, lat)`` call (the same NaN-break idiom the time-series viewers use).
If the asset is missing the loader returns empty arrays, and callers degrade to a
graticule-only map rather than failing.
"""

from __future__ import annotations

import json
import os
from functools import lru_cache
from typing import Tuple

import numpy as np

_ASSET = os.path.join(os.path.dirname(__file__), "assets", "coastline_110m.json")


@lru_cache(maxsize=1)
def load_coastline(path: str = _ASSET) -> Tuple[np.ndarray, np.ndarray]:
    """Return ``(lon, lat)`` float arrays for the world coastline.

    ``None`` entries in the asset become ``NaN`` so successive polylines render
    as separate strokes.  Cached: the asset is parsed once per process.  Missing
    or malformed asset -> two empty arrays (graticule-only fallback).
    """
    try:
        with open(path, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        lon = np.array([np.nan if v is None else float(v) for v in data["lon"]],
                       dtype=float)
        lat = np.array([np.nan if v is None else float(v) for v in data["lat"]],
                       dtype=float)
        return lon, lat
    except (OSError, ValueError, KeyError):
        return np.array([], dtype=float), np.array([], dtype=float)


def has_coastline(path: str = _ASSET) -> bool:
    """True if the vendored coastline asset is present and non-empty."""
    lon, _ = load_coastline(path)
    return lon.size > 0

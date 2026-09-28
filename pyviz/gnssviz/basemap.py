"""Vendored satellite-imagery basemap loader for the geographic ``exam_map`` viewer.

Companion to :mod:`gnssviz.coastline`. Where the coastline gives a filled
land/ocean *outline* basemap, this loads a real satellite-imagery raster
(``assets/basemap_esri_fiji.jpg`` plus a ``.json`` sidecar), vendored once by
``scripts/fetch_satellite.py`` so the dashboard can draw photo imagery fully
offline: the report embeds the pixels (base64) with no render-time network and
no cartopy, contextily, or tile-server dependency, matching the suite's
numpy/pandas/matplotlib-only philosophy.

:func:`load_basemap` returns a :class:`Basemap` (decoded RGB, geographic extent,
attribution, and the original compressed bytes) or ``None`` when the asset, or
Pillow used to decode it, is unavailable. Callers (``exam_map``) then fall
back to the coastline land/ocean basemap, exactly like the coastline loader's
empty-array fallback.

The extent is expressed in continuous 0-360 longitude (the convention
``exam_map``'s dateline-crossing regional panel plots in), so the imagery and the
lon/lat track share one coordinate frame.
"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass
from functools import lru_cache
from typing import Optional, Tuple

import numpy as np

_ASSETS = os.path.join(os.path.dirname(__file__), "assets")
_STEM = "basemap_esri_fiji"
_MIME = {".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".png": "image/png"}


@dataclass(frozen=True)
class Basemap:
    """A georeferenced satellite basemap ready for a :class:`RgbImage` trace.

    * ``rgb``: ``(H, W, 3)`` uint8, row 0 = northmost (top).
    * ``extent``: ``(x0, x1, y0, y1)`` in 0-360 longitude degrees and latitude.
    * ``attribution``: required on-map credit string (drawn by the viewer).
    * ``encoded`` / ``mime``: the original compressed bytes of the same pixels,
      so a backend can embed them verbatim instead of re-encoding (smaller HTML).
    """
    rgb: np.ndarray
    extent: Tuple[float, float, float, float]
    attribution: str
    encoded: bytes
    mime: str


def _asset_paths() -> Optional[Tuple[str, str]]:
    """Return ``(image_path, sidecar_path)`` if both exist, else ``None``."""
    side = os.path.join(_ASSETS, _STEM + ".json")
    if not os.path.exists(side):
        return None
    for ext in (".jpg", ".jpeg", ".png"):
        img = os.path.join(_ASSETS, _STEM + ext)
        if os.path.exists(img):
            return img, side
    return None


@lru_cache(maxsize=1)
def load_basemap() -> Optional[Basemap]:
    """Load the vendored satellite basemap, or ``None`` (graceful fallback).

    Cached: the asset is decoded once per process. Any failure (missing asset,
    missing or broken sidecar, Pillow absent, unreadable image) returns ``None``
    so a caller degrades to the coastline basemap rather than raising.
    """
    paths = _asset_paths()
    if paths is None:
        return None
    img_path, side_path = paths
    try:
        with open(side_path, "r", encoding="utf-8") as fh:
            meta = json.load(fh)
        lon = meta["extent_lon360"]
        lat = meta["extent_lat"]
        extent = (float(lon[0]), float(lon[1]), float(lat[0]), float(lat[1]))
        attribution = str(meta.get("attribution", ""))
        with open(img_path, "rb") as fh:
            encoded = fh.read()
        from PIL import Image      # optional dep; guarded -> None fallback
        import io
        rgb = np.asarray(Image.open(io.BytesIO(encoded)).convert("RGB"), dtype=np.uint8)
    except Exception:
        return None
    if rgb.ndim != 3 or rgb.shape[2] != 3 or rgb.size == 0:
        return None
    mime = _MIME.get(os.path.splitext(img_path)[1].lower(), "image/png")
    return Basemap(rgb=rgb, extent=extent, attribution=attribution,
                   encoded=encoded, mime=mime)


def has_basemap() -> bool:
    """True if the vendored satellite basemap is present and loadable."""
    return load_basemap() is not None


def extent_overlaps(extent, x0: float, x1: float, y0: float, y1: float) -> bool:
    """True if basemap ``extent`` (x0,x1,y0,y1) overlaps the view box (same lon
    convention). Used to skip drawing imagery for a view outside its coverage."""
    bx0, bx1, by0, by1 = extent
    return (min(bx0, bx1) <= max(x0, x1) and max(bx0, bx1) >= min(x0, x1)
            and min(by0, by1) <= max(y0, y1) and max(by0, by1) >= min(y0, y1))

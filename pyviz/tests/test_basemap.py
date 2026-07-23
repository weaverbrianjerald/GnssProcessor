"""Vendored satellite basemap: loader, sidecar consistency, PNG encoder."""
import io
import json
import os

import numpy as np
import pytest

from gnssviz import basemap as bm
from gnssviz.pngio import encode_png


# --------------------------------------------------------------------------- #
#  The vendored asset (assets/basemap_esri_fiji.*)
# --------------------------------------------------------------------------- #
def test_basemap_loads_with_expected_metadata():
    b = bm.load_basemap()
    assert b is not None, "vendored basemap asset should be present"
    assert bm.has_basemap()
    # Fiji box, continuous 0-360 longitude; north-up uint8 RGB.
    x0, x1, y0, y1 = b.extent
    assert 150.0 < x0 < x1 <= 360.0 and -30.0 < y0 < y1 < 0.0
    assert b.rgb.dtype == np.uint8 and b.rgb.ndim == 3 and b.rgb.shape[2] == 3
    assert b.attribution and "Esri" in b.attribution
    assert b.mime in ("image/jpeg", "image/png") and len(b.encoded) > 0


def test_basemap_extent_matches_sidecar():
    b = bm.load_basemap()
    side = os.path.join(os.path.dirname(bm.__file__), "assets",
                        "basemap_esri_fiji.json")
    meta = json.loads(open(side, encoding="utf-8").read())
    assert list(b.extent[:2]) == [float(v) for v in meta["extent_lon360"]]
    assert list(b.extent[2:]) == [float(v) for v in meta["extent_lat"]]
    # Sidecar pixel size (W, H) must match the decoded array (H, W).
    w, h = meta["size_px"]
    assert b.rgb.shape[:2] == (h, w)


def test_basemap_covers_fiji_scenarios():
    # The standard (-178.2 -> 181.8 degE) and sar_fiji (~180.1 degE) origins,
    # each padded +/-3 deg, must fall inside the vendored box.
    b = bm.load_basemap()
    for lon360, lat in ((181.8, -18.5), (180.1, -16.35)):
        assert bm.extent_overlaps(b.extent, lon360 - 3, lon360 + 3, lat - 3, lat + 3)


def test_extent_overlaps_logic():
    ext = (176.0, 185.0, -23.0, -12.0)
    assert bm.extent_overlaps(ext, 180.0, 182.0, -18.0, -16.0)   # inside
    assert bm.extent_overlaps(ext, 170.0, 178.0, -25.0, -20.0)   # partial
    assert not bm.extent_overlaps(ext, 100.0, 110.0, -18.0, -16.0)  # far west
    assert not bm.extent_overlaps(ext, 178.0, 182.0, -5.0, 0.0)     # far north


def test_missing_asset_returns_none(monkeypatch):
    # No sidecar -> None (graceful fallback), like the coastline loader.
    bm.load_basemap.cache_clear()
    monkeypatch.setattr(bm, "_asset_paths", lambda: None)
    try:
        assert bm.load_basemap() is None
        assert bm.has_basemap() is False
    finally:
        # Always drop the cached None so other tests see the real asset again,
        # even if an assertion above fails.
        bm.load_basemap.cache_clear()


# --------------------------------------------------------------------------- #
#  Stdlib PNG encoder (plotly data-URI fallback path)
# --------------------------------------------------------------------------- #
def test_encode_png_uint8_roundtrips_exactly():
    PIL = pytest.importorskip("PIL.Image")
    rng = np.random.default_rng(1)
    arr = (rng.random((11, 13, 3)) * 255).astype(np.uint8)
    back = np.asarray(PIL.open(io.BytesIO(encode_png(arr))).convert("RGB"))
    assert np.array_equal(arr, back)


def test_encode_png_float_scales_to_0_255():
    PIL = pytest.importorskip("PIL.Image")
    arr = np.zeros((3, 3, 3), float)
    arr[0, 0] = [1.0, 0.0, 0.5]
    back = np.asarray(PIL.open(io.BytesIO(encode_png(arr))).convert("RGB"))
    assert back[0, 0, 0] == 255 and back[0, 0, 1] == 0 and abs(int(back[0, 0, 2]) - 128) <= 1


def test_encode_png_rejects_bad_channel_count():
    with pytest.raises(ValueError):
        encode_png(np.zeros((4, 4, 2), np.uint8))


def test_encode_png_is_valid_png_without_pillow():
    # The encoder exists for the Pillow-absent plotly path, so validate its
    # output using only the stdlib (struct + zlib) — no PIL round-trip.
    import struct
    import zlib
    arr = np.arange(3 * 5 * 3, dtype=np.uint8).reshape(3, 5, 3)   # H=3, W=5, RGB
    png = encode_png(arr)
    assert png[:8] == b"\x89PNG\r\n\x1a\n"                        # signature
    assert png[12:16] == b"IHDR"
    w, h, bit, ctype = struct.unpack(">IIBB", png[16:26])
    assert (w, h, bit, ctype) == (5, 3, 8, 2)                     # 8-bit truecolor RGB
    i = png.index(b"IDAT")
    dlen = struct.unpack(">I", png[i - 4:i])[0]
    raw = zlib.decompress(png[i + 4:i + 4 + dlen])
    assert len(raw) == h * (1 + w * 3)                           # filter byte + RGB/row
    # Row 0, col 0 = filter(0) then the first pixel bytes (0,1,2).
    assert raw[0] == 0 and raw[1:4] == bytes([0, 1, 2])

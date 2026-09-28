"""Minimal, dependency-free PNG *encoder* (stdlib ``zlib`` + ``struct`` only).

The plotly backend embeds a georeferenced :class:`~gnssviz.figures.RgbImage` as a
base64 ``data:`` URI (so the interactive report stays fully offline). When the
image does not already carry pre-compressed bytes, it is encoded here, without
pulling in Pillow or matplotlib on the render path. Encoding an already-8-bit
RGB(A) array to PNG is lossless.
"""
from __future__ import annotations

import struct
import zlib

import numpy as np


def _chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def encode_png(rgb) -> bytes:
    """Encode an ``(H, W, 3|4)`` (or ``(H, W)`` grey) array to PNG bytes.

    ``uint8`` is used verbatim; float input is treated as 0..1 and scaled.  Row 0
    is the top row (PNG's native order), matching :class:`RgbImage` semantics.
    """
    arr = np.asarray(rgb)
    if arr.dtype != np.uint8:
        arr = (np.clip(np.asarray(arr, dtype=float), 0.0, 1.0) * 255.0).round().astype(np.uint8)
    if arr.ndim == 2:                       # greyscale -> RGB
        arr = np.repeat(arr[:, :, None], 3, axis=2)
    h, w, ch = arr.shape
    if ch == 4:
        color_type = 6                      # truecolor + alpha
    elif ch == 3:
        color_type = 2                      # truecolor
    else:
        raise ValueError(f"encode_png expects 3 or 4 channels, got {ch}")
    arr = np.ascontiguousarray(arr)
    # Prepend a zero (None) filter byte to each scanline, then deflate.
    raw = np.hstack([np.zeros((h, 1), dtype=np.uint8), arr.reshape(h, w * ch)])
    idat = zlib.compress(raw.tobytes(), 6)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, color_type, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n"
            + _chunk(b"IHDR", ihdr)
            + _chunk(b"IDAT", idat)
            + _chunk(b"IEND", b""))

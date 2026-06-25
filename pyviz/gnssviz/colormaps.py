"""Perceptually-uniform / colourblind-aware colormaps -- the port of
``stripColormap.m``.

Returns an ``(n, 3)`` RGB array for a named map so every strip viewer shares one
set of maps instead of hand-rolled rainbow ramps.  The anchor tables are copied
verbatim from the MATLAB source so the Python figures are colour-identical to the
MATLAB ones.  Backends wrap the array in whatever colormap object they need
(matplotlib ``ListedColormap``, plotly colourscale, ...).

Sequential : parula turbo viridis cividis magma inferno  (+ ``_r`` reversed)
Diverging  : rdbu vik bwr
Legacy aliases : freshness/elevation/sequential -> viridis; diverging -> vik.
"""

from __future__ import annotations

import numpy as np

# --- perceptually-uniform anchor tables (sampled 0:0.1:1) ------------------- #
_VIRIDIS = np.array([
    [0.267004, 0.004874, 0.329415], [0.282623, 0.140926, 0.457517],
    [0.253935, 0.265254, 0.529983], [0.206756, 0.371758, 0.553117],
    [0.163625, 0.471133, 0.558148], [0.127568, 0.566949, 0.550556],
    [0.134692, 0.658636, 0.517649], [0.266941, 0.748751, 0.440573],
    [0.477504, 0.821444, 0.318055], [0.741388, 0.873449, 0.149561],
    [0.993248, 0.906157, 0.143936],
])
_CIVIDIS = np.array([
    [0.000000, 0.135112, 0.304751], [0.000000, 0.183846, 0.402529],
    [0.249425, 0.299309, 0.412259], [0.353330, 0.371851, 0.433811],
    [0.444070, 0.439060, 0.452470], [0.491484, 0.479875, 0.471140],
    [0.575160, 0.546157, 0.446631], [0.671162, 0.616818, 0.420708],
    [0.770914, 0.690087, 0.383493], [0.881443, 0.769230, 0.326856],
    [0.995737, 0.909344, 0.217772],
])
_MAGMA = np.array([
    [0.001462, 0.000466, 0.013866], [0.078815, 0.054184, 0.211667],
    [0.232077, 0.059889, 0.437695], [0.390384, 0.100379, 0.501864],
    [0.550287, 0.161158, 0.505719], [0.716387, 0.214982, 0.475290],
    [0.868793, 0.287728, 0.409303], [0.967671, 0.439703, 0.359630],
    [0.994738, 0.624350, 0.427397], [0.996580, 0.795190, 0.543864],
    [0.987053, 0.991438, 0.749504],
])
_INFERNO = np.array([
    [0.001462, 0.000466, 0.013866], [0.087411, 0.044556, 0.224813],
    [0.258234, 0.038571, 0.406485], [0.416331, 0.090203, 0.432943],
    [0.578304, 0.148039, 0.404411], [0.735683, 0.215906, 0.330245],
    [0.865006, 0.316822, 0.226055], [0.954506, 0.468744, 0.099874],
    [0.987622, 0.652180, 0.211624], [0.964394, 0.843848, 0.273391],
    [0.988362, 0.998364, 0.644924],
])
_RDBU = np.array([
    [5, 48, 97], [33, 102, 172], [67, 147, 195], [146, 197, 222],
    [209, 229, 240], [247, 247, 247], [253, 219, 199], [244, 165, 130],
    [214, 96, 77], [178, 24, 43], [103, 0, 31],
], dtype=float) / 255.0
_VIK = np.array([
    [0.001, 0.098, 0.310], [0.020, 0.231, 0.463], [0.137, 0.373, 0.580],
    [0.337, 0.518, 0.660], [0.580, 0.682, 0.749], [0.859, 0.859, 0.847],
    [0.780, 0.616, 0.541], [0.722, 0.420, 0.310], [0.624, 0.247, 0.149],
    [0.510, 0.149, 0.075], [0.384, 0.090, 0.043],
])

_ANCHORS = {
    "viridis": _VIRIDIS, "cividis": _CIVIDIS, "magma": _MAGMA,
    "inferno": _INFERNO, "rdbu": _RDBU, "vik": _VIK,
    # legacy aliases from the first-generation viewers
    "freshness": _VIRIDIS, "elevation": _VIRIDIS, "sequential": _VIRIDIS,
    "diverging": _VIK,
}


def _interp(anchors: np.ndarray, n: int) -> np.ndarray:
    xi = np.linspace(0.0, 1.0, anchors.shape[0])
    xq = np.linspace(0.0, 1.0, n)
    out = np.stack([np.interp(xq, xi, anchors[:, c]) for c in range(3)], axis=1)
    return np.clip(out, 0.0, 1.0)


def _bwr(n: int) -> np.ndarray:
    half = n // 2
    blue = np.stack([np.linspace(0.10, 1, half), np.linspace(0.30, 1, half),
                     np.linspace(0.75, 1, half)], axis=1)
    red = np.stack([np.linspace(1, 0.75, n - half), np.linspace(1, 0.10, n - half),
                    np.linspace(1, 0.10, n - half)], axis=1)
    return np.vstack([blue, red])


# `parula` (MATLAB-proprietary) and `turbo` have no portable anchor table here,
# so the *pure data layer* maps them to viridis -- deterministic and free of any
# plotting-library import (the architectural boundary: nothing below the backend
# touches matplotlib).  A backend that wants the genuine builtin can special-case
# these names; viewers in this POC use viridis / categorical maps, not these.
_VIRIDIS_FALLBACK = {"parula", "turbo"}


def strip_colormap(name, n: int = 256) -> np.ndarray:
    """Return an ``(n, 3)`` RGB colormap for ``name``.

    An ``(N, 3)`` array passed as ``name`` is returned unchanged (escape hatch).
    """
    if isinstance(name, np.ndarray) and name.ndim == 2 and name.shape[1] == 3:
        return name
    nm = str(name).lower()
    rev = nm.endswith("_r")
    if rev:
        nm = nm[:-2]

    if nm == "bwr":
        base = _bwr(n)
    elif nm in _ANCHORS:
        base = _interp(_ANCHORS[nm], n)
    else:
        base = _interp(_VIRIDIS, n)        # parula/turbo/unknown -> viridis

    if rev:
        base = np.flipud(base)
    return base

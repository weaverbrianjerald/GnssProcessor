"""Self-contained WGS84 coordinate transforms.

Replaces the MATLAB Mapping Toolbox calls used by the state viewer
(``ecef2lla`` / ``ecef2enu`` / ``referenceEllipsoid('wgs84')``).  Hand-rolled on
purpose: the math is standard and exact to sub-millimetre, it avoids a heavy
``pyproj`` dependency, and it keeps the suite installable with just
numpy/pandas/matplotlib.

All angles are in **degrees** at the public boundary (matching the MATLAB
signatures); heights and ECEF/ENU are in metres.
"""

from __future__ import annotations

from typing import Tuple

import numpy as np

# WGS84 ellipsoid.
_A = 6378137.0                      # semi-major axis [m]
_F = 1.0 / 298.257223563            # flattening
_E2 = _F * (2.0 - _F)               # first eccentricity squared


def lla_to_ecef(lat_deg, lon_deg, h):
    """Geodetic (lat, lon, height) -> ECEF (x, y, z), all metres."""
    lat = np.radians(np.asarray(lat_deg, dtype=float))
    lon = np.radians(np.asarray(lon_deg, dtype=float))
    h = np.asarray(h, dtype=float)
    sl, cl = np.sin(lat), np.cos(lat)
    n = _A / np.sqrt(1.0 - _E2 * sl * sl)
    x = (n + h) * cl * np.cos(lon)
    y = (n + h) * cl * np.sin(lon)
    z = (n * (1.0 - _E2) + h) * sl
    return x, y, z


def ecef_to_lla(x, y, z) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    """ECEF (x, y, z) [m] -> geodetic (lat_deg, lon_deg, h_m).

    Iterative latitude solution (Bowring-seeded); converges to sub-mm in a few
    iterations for terrestrial heights.
    """
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)
    z = np.asarray(z, dtype=float)
    lon = np.arctan2(y, x)
    p = np.hypot(x, y)
    lat = np.arctan2(z, p * (1.0 - _E2))   # seed assuming h~0
    for _ in range(6):
        sl = np.sin(lat)
        n = _A / np.sqrt(1.0 - _E2 * sl * sl)
        h = p / np.cos(lat) - n
        lat = np.arctan2(z, p * (1.0 - _E2 * n / (n + h)))
    sl = np.sin(lat)
    n = _A / np.sqrt(1.0 - _E2 * sl * sl)
    h = p / np.cos(lat) - n
    return np.degrees(lat), np.degrees(lon), h


def ecef_to_enu(x, y, z, lat0_deg, lon0_deg, h0):
    """ECEF points -> local East/North/Up about a geodetic reference.

    Matches ``ecef2enu(x,y,z, lat0,lon0,h0, wgs84, 'degrees')``: the reference is
    given as geodetic lat/lon/height; points are ECEF; output is ENU [m].
    """
    x0, y0, z0 = lla_to_ecef(lat0_deg, lon0_deg, h0)
    dx = np.asarray(x, dtype=float) - x0
    dy = np.asarray(y, dtype=float) - y0
    dz = np.asarray(z, dtype=float) - z0

    lat = np.radians(lat0_deg)
    lon = np.radians(lon0_deg)
    sl, cl = np.sin(lat), np.cos(lat)
    so, co = np.sin(lon), np.cos(lon)

    e = -so * dx + co * dy
    n = -sl * co * dx - sl * so * dy + cl * dz
    u = cl * co * dx + cl * so * dy + sl * dz
    return e, n, u

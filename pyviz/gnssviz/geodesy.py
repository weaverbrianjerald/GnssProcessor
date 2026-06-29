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


# Thin-shell single-layer ionosphere constants — match GlobalIonosphereMap.cpp
# (Re = base radius, H = shell height).  These are the IONEX defaults the C++
# pierce-point computation uses; keep them in sync if the GIM header changes.
_RE_KM = 6371.0
_H_KM = 450.0


def ionospheric_pierce_point(lat_deg, lon_deg, az_deg, el_deg,
                             h_ion_km: float = _H_KM, re_km: float = _RE_KM):
    """Geodetic receiver + look angles -> ionospheric pierce point (lat, lon) deg.

    Single-layer (thin-shell) geometry, the exact construction
    ``GlobalIonosphereMap::slantIonoDelay`` uses (GnssProcessor C++):

        sin(z') = Re/(Re+H) * cos(el)          # zenith at the shell
        psi     = pi/2 - el - asin(sin z')     # earth-central angle rx->IPP
        lat_ipp = asin(sin lat * cos psi + cos lat * sin psi * cos az)
        lon_ipp = lon + asin(sin psi * sin az / cos lat_ipp)

    All inputs in **degrees**; outputs in degrees with longitude wrapped to
    [-180, 180).  Vectorized over array inputs (broadcast like numpy).  Elevations
    <= 0 (or non-finite) yield NaN pierce points.
    """
    lat = np.radians(np.asarray(lat_deg, dtype=float))
    lon = np.radians(np.asarray(lon_deg, dtype=float))
    az = np.radians(np.asarray(az_deg, dtype=float))
    el = np.asarray(el_deg, dtype=float)
    el_r = np.radians(el)

    sin_zp = (re_km / (re_km + h_ion_km)) * np.cos(el_r)
    sin_zp = np.clip(sin_zp, -1.0, 1.0)
    psi = (np.pi / 2.0) - el_r - np.arcsin(sin_zp)   # central angle rx -> IPP

    lat_ipp = np.arcsin(np.clip(
        np.sin(lat) * np.cos(psi) + np.cos(lat) * np.sin(psi) * np.cos(az),
        -1.0, 1.0))
    dlon = np.arcsin(np.clip(
        np.sin(psi) * np.sin(az) / np.maximum(np.cos(lat_ipp), 1e-12),
        -1.0, 1.0))
    lon_ipp = lon + dlon

    lat_out = np.degrees(lat_ipp)
    lon_out = (np.degrees(lon_ipp) + 180.0) % 360.0 - 180.0   # wrap to [-180, 180)

    bad = ~np.isfinite(el) | (el <= 0.0)
    lat_out = np.where(bad, np.nan, lat_out)
    lon_out = np.where(bad, np.nan, lon_out)
    return lat_out, lon_out


def great_circle(lat1_deg, lon1_deg, lat2_deg, lon2_deg, n: int = 48):
    """Sample a great-circle arc between two points -> (lon[], lat[]) degrees.

    Spherical-slerp interpolation of the two unit vectors; ``n`` points
    inclusive of both endpoints.  Returns longitudes wrapped to [-180, 180);
    intended for drawing baseline arcs on an equirectangular world map.  (No
    antimeridian splitting — a span crossing +/-180 draws as one segment; for
    the global IGS networks the baselines-from-datum arcs render cleanly.)
    """
    lat1, lon1 = np.radians(lat1_deg), np.radians(lon1_deg)
    lat2, lon2 = np.radians(lat2_deg), np.radians(lon2_deg)
    p1 = np.array([np.cos(lat1) * np.cos(lon1),
                   np.cos(lat1) * np.sin(lon1), np.sin(lat1)])
    p2 = np.array([np.cos(lat2) * np.cos(lon2),
                   np.cos(lat2) * np.sin(lon2), np.sin(lat2)])
    dot = float(np.clip(np.dot(p1, p2), -1.0, 1.0))
    omega = np.arccos(dot)
    t = np.linspace(0.0, 1.0, max(2, int(n)))
    if omega < 1e-9:                       # coincident / antipodal-safe degenerate
        pts = np.outer(1.0 - t, p1) + np.outer(t, p2)
    else:
        s = np.sin(omega)
        pts = (np.outer(np.sin((1.0 - t) * omega) / s, p1)
               + np.outer(np.sin(t * omega) / s, p2))
    lat = np.degrees(np.arcsin(np.clip(pts[:, 2], -1.0, 1.0)))
    lon = np.degrees(np.arctan2(pts[:, 1], pts[:, 0]))
    lon = (lon + 180.0) % 360.0 - 180.0
    return lon, lat

"""WGS84 transform tests: round-trip and ENU consistency."""
import numpy as np

from gnssviz.geodesy import ecef_to_enu, ecef_to_lla, lla_to_ecef


def test_ecef_lla_roundtrip():
    lat, lon, h = 40.0, -83.0, 250.0
    x, y, z = lla_to_ecef(lat, lon, h)
    lat2, lon2, h2 = ecef_to_lla(x, y, z)
    assert abs(float(lat2) - lat) < 1e-9
    assert abs(float(lon2) - lon) < 1e-9
    assert abs(float(h2) - h) < 1e-6        # sub-micron height


def test_enu_zero_at_reference():
    lat, lon, h = 35.0, -120.0, 100.0
    x, y, z = lla_to_ecef(lat, lon, h)
    e, n, u = ecef_to_enu(x, y, z, lat, lon, h)
    assert abs(float(e)) < 1e-6 and abs(float(n)) < 1e-6 and abs(float(u)) < 1e-6


def test_enu_up_matches_height_offset():
    lat, lon, h = 12.0, 77.0, 900.0
    x, y, z = lla_to_ecef(lat, lon, h + 10.0)     # 10 m higher, same lat/lon
    e, n, u = ecef_to_enu(x, y, z, lat, lon, h)
    assert abs(float(e)) < 1e-4 and abs(float(n)) < 1e-4
    assert abs(float(u) - 10.0) < 1e-4


def test_vectorized():
    x = np.array([4.0e6, 4.1e6]); y = np.array([1.0e6, 1.0e6]); z = np.array([4.6e6, 4.6e6])
    lat, lon, hh = ecef_to_lla(x, y, z)
    assert lat.shape == (2,) and lon.shape == (2,) and hh.shape == (2,)

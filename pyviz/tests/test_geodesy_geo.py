"""Tests for the geo helpers: ionospheric pierce point + great-circle arc."""
import numpy as np

from gnssviz.geodesy import great_circle, ionospheric_pierce_point


def test_ipp_zenith_is_receiver():
    # A satellite at zenith (el=90) pierces the shell directly overhead, so the
    # pierce point sits at the receiver lat/lon (azimuth irrelevant).
    lat, lon = ionospheric_pierce_point(34.0, -118.0, 123.0, 90.0)
    assert abs(float(lat) - 34.0) < 1e-6
    assert abs(float(lon) - (-118.0)) < 1e-6


def test_ipp_known_value():
    # Matches the thin-shell construction in GlobalIonosphereMap.cpp
    # (Re=6371 km, H=450 km) for el=30, az=45 from (34N, 118W).
    lat, lon = ionospheric_pierce_point(34.0, -118.0, 45.0, 30.0)
    assert abs(float(lat) - 38.135) < 0.05
    assert abs(float(lon) - (-112.597)) < 0.05


def test_ipp_displaces_along_azimuth():
    # Due north look (az=0) moves the pierce point north, longitude ~unchanged.
    lat, lon = ionospheric_pierce_point(0.0, 0.0, 0.0, 20.0)
    assert float(lat) > 0.0
    assert abs(float(lon)) < 1e-6


def test_ipp_low_elevation_farther():
    _, lon_hi = ionospheric_pierce_point(0.0, 0.0, 90.0, 60.0)   # east, high el
    _, lon_lo = ionospheric_pierce_point(0.0, 0.0, 90.0, 15.0)   # east, low el
    assert abs(float(lon_lo)) > abs(float(lon_hi))   # lower el -> farther pierce


def test_ipp_invalid_elevation_nan():
    lat, lon = ionospheric_pierce_point(34.0, -118.0, 45.0, 0.0)
    assert not np.isfinite(float(lat)) and not np.isfinite(float(lon))


def test_ipp_vectorized():
    lat, lon = ionospheric_pierce_point(np.array([34.0, 0.0]),
                                        np.array([-118.0, 0.0]),
                                        np.array([45.0, 90.0]),
                                        np.array([30.0, 30.0]))
    assert lat.shape == (2,) and lon.shape == (2,)


def test_great_circle_endpoints():
    lon, lat = great_circle(34.0, -118.0, 35.0, 139.0, 20)
    assert lon.shape == (20,) and lat.shape == (20,)
    assert abs(lat[0] - 34.0) < 1e-6 and abs(lon[0] - (-118.0)) < 1e-6
    assert abs(lat[-1] - 35.0) < 1e-6 and abs(lon[-1] - 139.0) < 1e-6


def test_great_circle_wrapped_longitudes():
    lon, lat = great_circle(0.0, 170.0, 0.0, -170.0, 10)
    assert np.all(lon >= -180.0) and np.all(lon <= 180.0)

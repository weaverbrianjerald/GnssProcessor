"""Tests for gnssviz.eval.metrics -- ENU error recovery and convergence window."""

from __future__ import annotations

import math

import numpy as np
import pandas as pd
import pytest

from gnssviz.geodesy import ecef_to_lla, lla_to_ecef
from gnssviz.eval.metrics import (
    doy_to_decimal_year,
    enu_error_series,
    evaluate_state_trace,
    gps_to_decimal_year,
)


def _enu_to_ecef_delta(e, n, u, lat_deg, lon_deg):
    """Independent ENU->ECEF offset (transpose of geodesy.ecef_to_enu).

    Written out here on purpose: the metrics code uses geodesy.ecef_to_enu to go
    the *other* way, so a sign error in either rotation breaks the round-trip.
    """
    lat, lon = math.radians(lat_deg), math.radians(lon_deg)
    sl, cl = math.sin(lat), math.cos(lat)
    so, co = math.sin(lon), math.cos(lon)
    dx = -so * e - sl * co * n + cl * co * u
    dy = co * e - sl * so * n + cl * so * u
    dz = cl * n + sl * u
    return np.array([dx, dy, dz])


def _make_trace(tmp_path, truth, enu_offset, n_epochs=40, dt_s=30.0,
                warmup_garbage=4):
    """Write a synthetic state_trace.csv: truth + a constant ENU offset, with a
    few large-error 'unconverged' leading epochs."""
    lat, lon, h = (float(np.ravel(v)[0]) for v in ecef_to_lla(*truth))
    delta = _enu_to_ecef_delta(*enu_offset, lat, lon)
    rows = []
    week, tow0 = 2300, 100000.0
    for i in range(n_epochs):
        if i < warmup_garbage:
            est = truth + _enu_to_ecef_delta(5.0, -7.0, 9.0, lat, lon)  # metres of junk
        else:
            est = truth + delta
        rows.append((week, tow0 + i * dt_s, est[0], est[1], est[2]))
    df = pd.DataFrame(rows, columns=["week", "tow_s", "x_m", "y_m", "z_m"])
    p = tmp_path / "state_trace.csv"
    df.to_csv(p, index=False)
    return p


def test_recovers_known_enu_bias(tmp_path):
    truth = np.array(lla_to_ecef(34.2, -118.1, 300.0))
    offset = (0.020, -0.030, 0.050)  # E, N, U metres
    path = _make_trace(tmp_path, truth, offset, dt_s=30.0, warmup_garbage=4)

    # warmup_s = 100 -> first 4 epochs (t=0,30,60,90) excluded.
    m = evaluate_state_trace(path, truth, warmup_s=100.0)
    assert m.n_converged == 36
    assert np.allclose(m.bias_enu, offset, atol=1e-6)
    assert np.allclose(m.std_enu, (0.0, 0.0, 0.0), atol=1e-9)  # constant offset
    assert m.rms_3d == pytest.approx(math.sqrt(0.02**2 + 0.03**2 + 0.05**2), abs=1e-6)
    assert m.rms_horizontal == pytest.approx(math.sqrt(0.02**2 + 0.03**2), abs=1e-6)
    assert not m.used_fallback_window


def test_warmup_excludes_garbage_epochs(tmp_path):
    truth = np.array(lla_to_ecef(0.0, 0.0, 0.0))
    offset = (0.01, 0.01, 0.01)
    path = _make_trace(tmp_path, truth, offset, warmup_garbage=4)

    # With no warmup the garbage epochs poison the bias; with warmup they don't.
    m_no = evaluate_state_trace(path, truth, warmup_s=0.0)
    m_yes = evaluate_state_trace(path, truth, warmup_s=100.0)
    assert abs(m_no.bias_enu[2]) > abs(m_yes.bias_enu[2])
    assert np.allclose(m_yes.bias_enu, offset, atol=1e-6)


def test_fallback_window_when_warmup_too_long(tmp_path):
    truth = np.array(lla_to_ecef(45.0, 10.0, 100.0))
    path = _make_trace(tmp_path, truth, (0.0, 0.0, 0.0), n_epochs=20, dt_s=30.0,
                       warmup_garbage=0)
    # warmup longer than the whole run -> fallback to last 25 %.
    m = evaluate_state_trace(path, truth, warmup_s=10_000.0)
    assert m.used_fallback_window
    assert m.n_converged > 0


def test_decimal_year_helpers():
    # 2015:001:00000 is exactly 2015.0 (non-leap year, day-of-year 1).
    assert gps_to_decimal_year(0, 0) == pytest.approx(1980.0 + 5.0 / 366.0, abs=1e-6)
    assert doy_to_decimal_year(2024, 129, 0.0) == pytest.approx(
        2024 + 128.0 / 366.0, abs=1e-9)          # 2024 is a leap year
    assert doy_to_decimal_year(2023, 1, 43200.0) == pytest.approx(
        2023 + 0.5 / 365.0, abs=1e-9)


def test_header_only_trace_returns_nan_metric(tmp_path):
    p = tmp_path / "state_trace.csv"
    p.write_text("week,tow_s,x_m,y_m,z_m\n", encoding="utf-8")   # header, zero rows
    m = evaluate_state_trace(p, [1e6, 2e6, 3e6])
    assert m.n_converged == 0
    assert m.rms_3d != m.rms_3d                                  # NaN
    assert "no epochs" in m.notes


def test_missing_position_column_returns_nan_metric(tmp_path):
    p = tmp_path / "state_trace.csv"
    p.write_text("week,tow_s,clock_m\n2300,100000,0.5\n", encoding="utf-8")
    m = evaluate_state_trace(p, [1e6, 2e6, 3e6])
    assert m.n_converged == 0 and m.rms_3d != m.rms_3d


def test_enu_error_series_recovers_offset_and_matches_metric(tmp_path):
    truth = np.array(lla_to_ecef(34.2, -118.1, 300.0))
    offset = (0.020, -0.030, 0.050)                  # E, N, U metres
    path = _make_trace(tmp_path, truth, offset, dt_s=30.0, warmup_garbage=0)

    es = enu_error_series(path, truth)
    assert es is not None
    assert es.elapsed_s[0] == 0.0 and es.elapsed_s[-1] > es.elapsed_s[0]
    # Same sign convention as evaluate_state_trace: constant ENU offset recovered.
    assert np.allclose(es.e, offset[0], atol=1e-6)
    assert np.allclose(es.n, offset[1], atol=1e-6)
    assert np.allclose(es.u, offset[2], atol=1e-6)
    exp_3d = math.sqrt(sum(c ** 2 for c in offset))
    assert np.allclose(es.err_3d, exp_3d, atol=1e-6)
    assert np.allclose(es.err_horizontal, math.hypot(offset[0], offset[1]), atol=1e-6)


def test_enu_error_series_none_on_unusable_trace(tmp_path):
    p = tmp_path / "state_trace.csv"
    p.write_text("week,tow_s,x_m,y_m,z_m\n", encoding="utf-8")     # header only
    assert enu_error_series(p, [1e6, 2e6, 3e6]) is None
    p.write_text("week,tow_s,clock_m\n2300,100000,0.5\n", encoding="utf-8")  # no pos
    assert enu_error_series(p, [1e6, 2e6, 3e6]) is None
    assert enu_error_series(tmp_path / "nope.csv", [1e6, 2e6, 3e6]) is None  # missing

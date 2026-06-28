"""Tests for gnssviz.eval.regression -- truth cross-check + golden gate."""

from __future__ import annotations

import numpy as np

from gnssviz.eval.regression import (
    compare_metrics,
    cross_check_truth,
    index_metrics,
)


class _FakeTruth:
    """Minimal TruthSource stand-in: station -> ecef dict."""
    def __init__(self, table, tag):
        self.table = table
        self.tag = tag

    def resolve(self, station, year):
        from gnssviz.eval.truth import TruthCoord
        if station not in self.table:
            return None
        return TruthCoord(ecef=np.asarray(self.table[station], float),
                          epoch_year=year, frame="TEST", source=f"{self.tag}:{station}")


def test_cross_check_flags_only_large_disagreement():
    base = np.array([3970000.0, 1000000.0, 4900000.0])
    primary = _FakeTruth({"AAAA": base, "BBBB": base, "CCCC": base}, "p")
    secondary = _FakeTruth({
        "AAAA": base + [0.01, 0.0, 0.0],     # 1 cm -> ok
        "BBBB": base + [0.0, 0.0, 0.20],     # 20 cm -> flagged
        "DDDD": base,                         # only secondary -> informational
    }, "s")
    rows, n_flagged = cross_check_truth(primary, secondary,
                                        ["AAAA", "BBBB", "CCCC", "DDDD"],
                                        2024.0, tol_m=0.05)
    by = {r.station: r for r in rows}
    assert by["AAAA"].ok and by["AAAA"].dist_m < 0.05
    assert not by["BBBB"].ok and by["BBBB"].dist_m > 0.05
    assert by["CCCC"].note.startswith("only")      # primary-only, no cross-check
    assert by["DDDD"].ok and "only" in by["DDDD"].note
    assert n_flagged == 1


def _metric(station, cell, bias, rms3d):
    return {"station": station, "cell": cell,
            "bias_enu_m": list(bias), "rms_3d_m": rms3d}


def test_compare_metrics_detects_regression():
    golden = [
        _metric("JPLM", "baseline", (0.014, -0.004, 0.055), 0.061),
        _metric("JPLM", "no_solid_tide", (-0.012, -0.013, -0.085), 0.104),
    ]
    current = [
        _metric("JPLM", "baseline", (0.015, -0.004, 0.057), 0.062),   # ~ok (<2cm)
        _metric("JPLM", "no_solid_tide", (-0.012, -0.013, 0.050), 0.060),  # U moved 13.5cm
    ]
    deltas, n_reg = compare_metrics(current, golden, bias_tol_m=0.02, rms_tol_m=0.02)
    by = {d.key: d for d in deltas}
    assert by[("JPLM", "baseline")].status == "ok"
    assert by[("JPLM", "no_solid_tide")].status == "regressed"
    assert n_reg == 1
    assert any("bias U" in r for r in by[("JPLM", "no_solid_tide")].reasons)


def test_compare_metrics_new_and_missing():
    golden = [_metric("JPLM", "baseline", (0, 0, 0), 0.05)]
    current = [_metric("GCGO", "baseline", (0, 0, 0), 0.05)]
    deltas, n_reg = compare_metrics(current, golden)
    by = {d.key: d for d in deltas}
    assert by[("GCGO", "baseline")].status == "new"
    assert by[("JPLM", "baseline")].status == "missing"
    assert n_reg == 0      # shape changes aren't regressions


def test_index_metrics_keys_by_station_cell():
    idx = index_metrics([_metric("A", "x", (0, 0, 0), 0.0),
                         _metric("B", "y", (0, 0, 0), 0.0)])
    assert set(idx) == {("A", "x"), ("B", "y")}


def _nan_metric(station, cell):
    nan = float("nan")
    return {"station": station, "cell": cell,
            "bias_enu_m": [nan, nan, nan], "rms_3d_m": nan}


def test_nan_metric_is_a_regression_not_a_silent_pass():
    # A diverged current run (all-NaN) vs a healthy golden MUST regress -- the
    # magnitude tests alone would silently pass it.
    golden = [_metric("JPLM", "baseline", (0.014, -0.004, 0.055), 0.061)]
    current = [_nan_metric("JPLM", "baseline")]
    deltas, n_reg = compare_metrics(current, golden)
    assert n_reg == 1
    assert deltas[0].status == "regressed"
    assert any("NaN" in r for r in deltas[0].reasons)


def test_recovery_from_nan_is_also_flagged():
    golden = [_nan_metric("JPLM", "baseline")]
    current = [_metric("JPLM", "baseline", (0.0, 0.0, 0.0), 0.05)]
    _, n_reg = compare_metrics(current, golden)
    assert n_reg == 1


def test_incomplete_golden_cell_does_not_mask_regression():
    # Golden present but missing bias/rms keys -> must not default to a silent pass.
    golden = [{"station": "JPLM", "cell": "baseline"}]
    current = [_metric("JPLM", "baseline", (0.30, 0.0, 0.0), 0.40)]
    _, n_reg = compare_metrics(current, golden)
    assert n_reg == 1


def test_compare_metrics_empty_lists():
    deltas, n_reg = compare_metrics([], [])
    assert deltas == [] and n_reg == 0


def test_cross_check_neither_backend_resolves():
    a = _FakeTruth({}, "p")
    b = _FakeTruth({}, "s")
    rows, n_flagged = cross_check_truth(a, b, ["ZZZZ"], 2024.0)
    assert n_flagged == 0
    assert rows[0].ok and "neither" in rows[0].note

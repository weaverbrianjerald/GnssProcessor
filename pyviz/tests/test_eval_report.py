"""Tests for gnssviz.eval.report -- scorecard, regression, convergence, HTML."""

from __future__ import annotations

import json

import numpy as np
import pandas as pd

from gnssviz.geodesy import ecef_to_lla, lla_to_ecef
from gnssviz.eval.report import (
    convergence_figures,
    regression_figures,
    regression_summary,
    render_scorecard_html,
    render_suite_report,
    scorecard_figures,
    scorecard_text,
)

_SUITE = {
    "date": "2024-129", "epoch_decimal_year": 2024.3511, "warmup_s": 1800.0,
    "ref_cells": {"ppp": "ppp_static"},
    "cross_check": [
        {"station": "JPLM00USA", "primary": "sinex:JPLM", "secondary": "sif:JPLM00USA",
         "dist_m": 0.006, "enu_m": [0, 0, 0], "ok": True, "note": ""},
        {"station": "GCGO00USA", "primary": "sinex:GCGO", "secondary": "sif:GCGO00USA",
         "dist_m": 0.111, "enu_m": [-0.057, 0.094, 0.002], "ok": False, "note": ""},
    ],
    "metrics": [
        {"ok": True, "station": "JPLM", "cell": "ppp_static", "base": "ppp",
         "bias_enu_m": [0.014, -0.004, 0.055], "rms_3d_m": 0.061,
         "rms_horizontal_m": 0.027, "time_to_threshold_s": 2970, "n_converged": 2820},
        {"ok": True, "station": "JPLM", "cell": "ppp_no_solid_tide", "base": "ppp",
         "bias_enu_m": [-0.012, -0.013, -0.085], "rms_3d_m": 0.104,
         "rms_horizontal_m": 0.048, "time_to_threshold_s": None, "n_converged": 2820},
        {"ok": False, "station": "CIT1", "cell": "ppp_gps_only", "reason": "run failed"},
    ],
}


def test_scorecard_text_has_rows_crosscheck_and_ab():
    txt = scorecard_text(_SUITE)
    assert "JPLM/ppp_static" in txt
    assert "JPLM/ppp_no_solid_tide" in txt
    assert "DIFF" in txt and "GCGO00USA" in txt          # cross-check flag
    assert "FAILED: run failed" in txt                   # failed cell surfaced
    assert "A/B deltas" in txt
    # solid-tide off should show a large negative dU vs the reference cell.
    ab = [ln for ln in txt.splitlines() if "ppp_no_solid_tide" in ln and "vs" in ln]
    assert ab and "dU-14" in ab[0].replace(" ", "")


def test_scorecard_text_survives_malformed_failed_cell():
    # A failed cell missing station/cell must not crash the report render.
    txt = scorecard_text({"metrics": [{"ok": False, "reason": "boom"}]})
    assert "FAILED: boom" in txt          # no KeyError


def test_scorecard_figures_shape():
    figs = scorecard_figures(_SUITE)
    assert len(figs) == 1
    assert len(figs[0].axes) == 2          # 3D-RMS + Up-bias panels


def test_render_scorecard_html(tmp_path):
    p = tmp_path / "suite_metrics.json"
    p.write_text(json.dumps(_SUITE), encoding="utf-8")
    out = tmp_path / "scorecard.html"
    render_scorecard_html(p, out)
    htmltext = out.read_text(encoding="utf-8")
    assert "<pre>" in htmltext and "Scorecard" in htmltext
    assert "data:image/png;base64" in htmltext   # the bar figure rendered


# ---------------------------------------------------------------------------
# regression vs golden
# ---------------------------------------------------------------------------

def _suite(metrics):
    return {"date": "2024-129", "warmup_s": 1800.0, "threshold_m": 0.1,
            "metrics": metrics}


def _m(station, cell, bias, rms3d, *, ok=True, truth=None):
    d = {"ok": ok, "station": station, "cell": cell, "base": "ppp",
         "bias_enu_m": list(bias), "rms_3d_m": rms3d,
         "rms_horizontal_m": (bias[0] ** 2 + bias[1] ** 2) ** 0.5,
         "time_to_threshold_s": 2000, "n_converged": 2820}
    if truth is not None:
        d["truth_ecef_m"] = list(truth)
    return d


def test_regression_summary_pass_when_unchanged():
    g = _suite([_m("JPLM", "ppp_static", (0.014, -0.004, 0.055), 0.061)])
    text, n_total, deltas = regression_summary(g, g)
    assert n_total == 0
    assert "Regression gate: PASS" in text
    assert all(d.status == "ok" for d in deltas)


def test_regression_summary_fail_on_moved_cell():
    golden = _suite([_m("JPLM", "ppp_static", (0.014, -0.004, 0.055), 0.061)])
    current = _suite([_m("JPLM", "ppp_static", (0.014, -0.004, 0.200), 0.061)])  # U +14.5cm
    text, n_total, deltas = regression_summary(current, golden,
                                               bias_tol_m=0.02, rms_tol_m=0.02)
    assert n_total == 1
    assert "Regression gate: FAIL" in text
    assert any(d.status == "regressed" for d in deltas)


def test_regression_summary_counts_broken_cell():
    # A golden-ok cell that is now failed/missing is 'broken', tripping the gate.
    golden = _suite([_m("JPLM", "ppp_static", (0.0, 0.0, 0.0), 0.06),
                     _m("CIT1", "ppp_static", (0.0, 0.0, 0.0), 0.07)])
    current = _suite([_m("JPLM", "ppp_static", (0.0, 0.0, 0.0), 0.06)])  # CIT1 gone
    text, n_total, _ = regression_summary(current, golden)
    assert n_total == 1
    assert "BROKEN CIT1/ppp_static" in text


def test_regression_figures_only_compares_shared_cells():
    golden = _suite([_m("JPLM", "ppp_static", (0.0, 0.0, 0.05), 0.06),
                     _m("JPLM", "ppp_extra", (0.0, 0.0, 0.0), 0.05)])
    current = _suite([_m("JPLM", "ppp_static", (0.0, 0.0, 0.09), 0.10)])
    figs = regression_figures(current, golden)
    assert len(figs) == 1
    assert len(figs[0].axes) == 3            # current-vs-golden + Δ3D-RMS + ΔUp-bias


def test_regression_figures_empty_when_no_overlap():
    golden = _suite([_m("JPLM", "a", (0, 0, 0), 0.05)])
    current = _suite([_m("CIT1", "b", (0, 0, 0), 0.05)])
    assert regression_figures(current, golden) == []


# ---------------------------------------------------------------------------
# convergence auto-figures (rebuilt from per-cell state_trace.csv)
# ---------------------------------------------------------------------------

def _write_trace(path, truth, enu_ecef_offset, n=30, dt_s=30.0):
    path.parent.mkdir(parents=True, exist_ok=True)
    est = np.asarray(truth, float) + np.asarray(enu_ecef_offset, float)
    rows = [(2300, 100000.0 + i * dt_s, est[0], est[1], est[2]) for i in range(n)]
    pd.DataFrame(rows, columns=["week", "tow_s", "x_m", "y_m", "z_m"]).to_csv(
        path, index=False)


def _suite_with_traces(tmp_path):
    truth = np.array(lla_to_ecef(34.2, -118.1, 300.0))
    metrics = [_m("JPLM", "ppp_static", (0.01, 0.0, 0.02), 0.03, truth=truth),
               _m("JPLM", "ppp_gps_only", (0.0, 0.0, 0.04), 0.05, truth=truth)]
    for mm in metrics:
        _write_trace(tmp_path / mm["station"] / mm["cell"] / "logs" / "state_trace.csv",
                     truth, (0.02, -0.01, 0.03))
    return _suite(metrics), truth


def test_convergence_figures_built_from_traces(tmp_path):
    suite, _ = _suite_with_traces(tmp_path)
    figs = convergence_figures(suite, tmp_path)
    assert len(figs) == 1                              # one panel for station JPLM
    # two cell lines + threshold HLine + warmup VLine.
    kinds = [type(t).__name__ for t in figs[0].axes[0].traces]
    assert kinds.count("Line") == 2
    assert "HLine" in kinds and "VLine" in kinds


def test_convergence_figures_skip_missing_traces(tmp_path):
    # Metric present but no trace on disk -> cell skipped, no figure for it.
    suite = _suite([_m("JPLM", "ppp_static", (0, 0, 0.05), 0.06,
                       truth=[1e6, 2e6, 3e6])])
    assert convergence_figures(suite, tmp_path) == []


# ---------------------------------------------------------------------------
# full suite report
# ---------------------------------------------------------------------------

def test_render_suite_report_with_golden_pass(tmp_path):
    suite, _ = _suite_with_traces(tmp_path)
    sp = tmp_path / "suite_metrics.json"
    sp.write_text(json.dumps(suite), encoding="utf-8")
    gp = tmp_path / "golden.json"
    gp.write_text(json.dumps(suite), encoding="utf-8")          # identical -> PASS
    out = tmp_path / "report.html"
    render_suite_report(sp, out, golden_path=gp)
    htmltext = out.read_text(encoding="utf-8")
    assert "REGRESSION GATE: PASS" in htmltext
    assert "banner pass" in htmltext
    assert "Regression vs golden" in htmltext and "Convergence" in htmltext
    assert "data:image/png;base64" in htmltext


def test_render_suite_report_fail_banner(tmp_path):
    suite, truth = _suite_with_traces(tmp_path)
    golden = _suite([_m("JPLM", "ppp_static", (0.01, 0.0, 0.02), 0.03, truth=truth),
                     _m("JPLM", "ppp_gps_only", (0.0, 0.0, 0.40), 0.45, truth=truth)])
    sp = tmp_path / "suite_metrics.json"
    sp.write_text(json.dumps(suite), encoding="utf-8")
    gp = tmp_path / "golden.json"
    gp.write_text(json.dumps(golden), encoding="utf-8")
    out = tmp_path / "report.html"
    render_suite_report(sp, out, golden_path=gp)
    htmltext = out.read_text(encoding="utf-8")
    assert "REGRESSION GATE: FAIL" in htmltext and "banner fail" in htmltext


def test_render_suite_report_missing_golden_shows_error_banner(tmp_path):
    # --golden given but the file does not exist: the suite gate exits rc=2, so
    # the report must not look clean/green -- it shows a red ERROR banner.
    suite, _ = _suite_with_traces(tmp_path)
    sp = tmp_path / "suite_metrics.json"
    sp.write_text(json.dumps(suite), encoding="utf-8")
    out = tmp_path / "report.html"
    render_suite_report(sp, out, golden_path=tmp_path / "no_such_golden.json")
    htmltext = out.read_text(encoding="utf-8")
    assert "banner fail" in htmltext
    assert "REGRESSION GATE: ERROR" in htmltext and "golden not found" in htmltext
    assert "Regression vs golden" not in htmltext      # no section without a golden


def test_render_suite_report_no_golden_omits_regression(tmp_path):
    suite, _ = _suite_with_traces(tmp_path)
    sp = tmp_path / "suite_metrics.json"
    sp.write_text(json.dumps(suite), encoding="utf-8")
    out = tmp_path / "report.html"
    render_suite_report(sp, out)
    htmltext = out.read_text(encoding="utf-8")
    assert "REGRESSION GATE" not in htmltext          # no gate banner without a golden
    assert "Convergence" in htmltext                  # but convergence still renders

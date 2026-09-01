"""Build-time tests for the simulated-network truth viewers (synthetic artifacts).

Exercises ``simnet_scorecard`` / ``simnet_theory`` / ``simnet_truth_series``
against stub ``simnet_grade.json`` / ``simnet_grade.txt`` /
``simnet_grade_series.csv`` files — the artifacts ``scripts/simnet_report.py``
writes into a run's log dir — without rendering.
"""
import json

import pytest

from gnssviz.figures import HLine, Line, VLine
from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.simnet_scorecard import build_simnet_scorecard
from gnssviz.viewers.simnet_theory import build_simnet_theory
from gnssviz.viewers.simnet_truth_series import build_simnet_truth_series

_TOW0 = 302400.0


def _stats(n=10, mean=0.1, rms=0.2, p50=0.1, mx=0.5):
    return {"n": n, "mean": mean, "std": 0.1, "rms": rms,
            "p50_abs": p50, "max_abs": mx}


def _write_grade_json(tmp_path, *, consumer_refused=False, cb_no_pin=False):
    g = {
        "s_line": {"ref_rx": "S30000SIM",
                   "pivots": {"G": "G01", "E": "E02"},
                   "cbpin": {"G": ["C1C", "C2W"]},
                   "gfpin": {},
                   "apriori_id": "x", "extra": {}},
        "n_epochs": 720,
        "product_census": {"S": 1, "E": 720, "C": 100, "B": 200, "P": 200, "U": 100},
        "clock": {"residual": _stats(), "contrast": _stats(), "vf": 3.5,
                  "n_vf": 10, "n_no_sigma": 0, "pivot": {"G": "G01"},
                  "n_pivot_skipped": 5},
        "phase": {"G:L1C": _stats()},
        "code_bias": ({"armed": True, "no_pin": True, "refused": {"G": "no_pin"}}
                      if cb_no_pin else
                      {"armed": True, "residual": _stats(), "vf": 0.9, "n_vf": 50}),
        "rx_states": {"n_cb": 40, "n_clk": 40, "contrast": _stats(),
                      "level": _stats(), "sigma": _stats(), "clk_contrast": _stats(),
                      "n_stations_truth": 2},
        "zwd": {"residual": _stats(), "vf": 1.2, "n_vf": 30, "missing_truth": 0},
        "consumer": ({"refused": "no_bcp", "n_epochs": 360, "ar_success": 0}
                     if consumer_refused else
                     {"err3d": _stats(), "up": _stats(), "n_epochs": 360,
                      "ar_success": 12}),
    }
    (tmp_path / "simnet_grade.json").write_text(json.dumps(g), encoding="utf-8")
    return g


def _write_series(tmp_path, *, n=8, with_cb=True):
    rows = ["tow,channel,id,value,n"]
    settle = _TOW0 + 30.0 * (n // 2)
    rows.append(f"{settle},_settle,,1.0,")
    for k in range(n):
        tow = _TOW0 + 30.0 * k
        # Error decades above sigma => the log-scale flip must engage.
        rows.append(f"{tow},clk_rms,,{1.0 + 0.1 * k},10")
        rows.append(f"{tow},clk_sig_med,,0.01,10")
        rows.append(f"{tow},clk_contrast_rms,,{1.1 + 0.1 * k},9")
        for sta, e in (("STA_A", 0.05), ("STA_B", -0.4)):
            rows.append(f"{tow},zwd_e,{sta},{e},")
            rows.append(f"{tow},zwd_sig,{sta},0.008,")
        rows.append(f"{tow},phase_frac_med,G:L1C,0.24,20")
        rows.append(f"{tow},rxdcb_level,,1.2,2")
        rows.append(f"{tow},rxdcb_contrast_rms,,0.8,1")
        if with_cb and tow >= settle:
            rows.append(f"{tow},cb_rms,,0.7,30")
            rows.append(f"{tow},cb_chmean_absmax,,0.001,6")
    (tmp_path / "simnet_grade_series.csv").write_text("\n".join(rows) + "\n",
                                                      encoding="utf-8")


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


# ---------------------------------------------------------------- scorecard --
def test_scorecard_full(tmp_path):
    _write_grade_json(tmp_path)
    (tmp_path / "simnet_grade.txt").write_text(
        "SYNTHETIC MINIMUM NETWORK — truth grade\n  fixture: x\n", encoding="utf-8")
    res = build_simnet_scorecard(_ctx(tmp_path))
    s = res.summary
    assert not res.figures                      # text-only viewer
    assert "pivot {'G': 'G01', 'E': 'E02'}" in s
    assert "OPTIMISTIC" in s                    # clock vf 3.5 > 2.0
    assert "absorbed gauge" in s                # rx level caveat
    assert "AR accepted 12/360" in s
    assert "Full grader readout" in s           # the verbatim txt was appended
    assert "SYNTHETIC MINIMUM NETWORK" in s


def test_scorecard_refusals(tmp_path):
    _write_grade_json(tmp_path, consumer_refused=True, cb_no_pin=True)
    res = build_simnet_scorecard(_ctx(tmp_path))
    assert "REFUSED — no usable cbpin" in res.summary
    assert "VOID (no_bcp)" in res.summary


# ------------------------------------------------------------------- theory --
def test_theory_panels(tmp_path):
    _write_grade_json(tmp_path)
    res = build_simnet_theory(_ctx(tmp_path))
    assert len(res.figures) == 3
    names = [f.name for f in res.figures]
    assert names == ["simnet_theory_model", "simnet_theory_sbasis",
                     "simnet_theory_grading"]
    # The S-basis panel quotes THIS run's realized datum, read from the JSON.
    sbasis_texts = " ".join(t.text for t in res.figures[1].axes[0].traces)
    assert "G: G01" in sbasis_texts and "E: E02" in sbasis_texts
    assert "C1C/C2W" in sbasis_texts
    assert "single-layer posture" in sbasis_texts     # empty gfpin
    assert "G01" in res.summary


# ------------------------------------------------------------- truth series --
def test_truth_series_figures(tmp_path):
    _write_series(tmp_path)
    res = build_simnet_truth_series(_ctx(tmp_path))
    names = [f.name for f in res.figures]
    assert names == ["simnet_error_vs_sigma", "simnet_zwd_stations",
                     "simnet_rx_channels", "simnet_phase_frac"]
    # Calibration exhibit: clock axis flips to log (error decades above sigma)
    # and carries the settle boundary.
    ax_clk = res.figures[0].axes[0]
    assert ax_clk.yscale == "log"
    assert any(isinstance(t, VLine) for t in ax_clk.traces)
    # ZWD spaghetti: one line per station + zero line.
    ax_zwd = res.figures[1].axes[0]
    labels = {t.label for t in ax_zwd.traces if isinstance(t, Line)}
    assert {"STA_A", "STA_B"} <= labels
    assert any(isinstance(t, HLine) for t in ax_zwd.traces)
    # Phase panel keeps the 0.25-uniform reference line, unlabeled.
    ax_ph = res.figures[3].axes[0]
    hl = [t for t in ax_ph.traces if isinstance(t, HLine)]
    assert hl and hl[0].y == 0.25 and hl[0].label is None
    # Summary states the settle boundary and settled medians.
    assert "settle boundary" in res.summary
    assert "med|frac| G:L1C" in res.summary


def test_truth_series_partial_channels(tmp_path):
    _write_series(tmp_path, with_cb=False)
    res = build_simnet_truth_series(_ctx(tmp_path))
    # Without cb rows the calibration figure still builds (clock + zwd axes).
    fig0 = res.figures[0]
    assert fig0.name == "simnet_error_vs_sigma"
    assert len(fig0.axes) == 2


def test_truth_series_empty_skips(tmp_path):
    (tmp_path / "simnet_grade_series.csv").write_text(
        "tow,channel,id,value,n\n", encoding="utf-8")
    with pytest.raises(Exception):
        build_simnet_truth_series(_ctx(tmp_path))

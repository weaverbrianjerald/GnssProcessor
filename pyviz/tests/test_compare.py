"""Build-time tests for the A/B run-comparison comparators (synthetic runs).

Two synthetic run dirs under ``tmp_path`` are populated with stub trace CSVs.  A
*constructed level shift* between run A and run B makes each delta's sign
predictable, so the tests assert the delta direction, not just that a figure was
built.  Comparators are exercised through their ``build`` functions directly
(spec objects, not rendered), so these stay fast and backend-free.
"""
import numpy as np
import pandas as pd
import pytest

from gnssviz.compare import (
    ComparePair, align_epochs, run_comparators,
    compare_closed_loop, compare_epoch_status, compare_flag_rates,
    compare_zwd, compare_network_clock, compare_scorecard, COMPARATORS,
)
from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip


# --------------------------------------------------------------------------- #
#  fixtures
# --------------------------------------------------------------------------- #
def _pair(tmp_path):
    a = tmp_path / "runA"
    b = tmp_path / "runB"
    a.mkdir()
    b.mkdir()
    return a, b, ComparePair(a=TraceContext(log_dir=str(a)),
                             b=TraceContext(log_dir=str(b)),
                             label_a="A", label_b="B")


def _closed_loop(dir_, err_scale, n=8):
    rows = ["week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,ratio,"
            "pos_err_3d_m,east_m,north_m,up_m"]
    for k in range(n):
        tow = 259200 + 30 * k
        err = err_scale * (0.5 / (k + 1))
        rows.append(f"2313,{tow}.000,10,1,{1 if k > 3 else 0},"
                    f"{8 if k > 3 else 0},10,{3.0 if k > 3 else 0.0},"
                    f"{err:.4f},0.01,0.01,0.02")
    (dir_ / "closed_loop_trace.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")


def _epoch_status(dir_, clk_median, sigma0, n=8, n_withheld=0):
    """Write network_epoch_status.csv; first ``n_withheld`` rows are withheld."""
    hdr = ("week,tow,disposition,sigma0,n_adapt,n_present,n_clock_sats,"
           "clock_median_m,clock_std_m,worst_sat,worst_dev_m,n_refloat_sats,"
           "n_retro_applied,datum_changed,sigma0_trigger,withhold_ceiling,"
           "n_arc_refloat")   # T2d
    rows = [hdr]
    for k in range(n):
        tow = 259200 + 30 * k
        if k < n_withheld:
            rows.append(f"2313,{tow}.000,gate_withheld,,,,,,,,,,,,3.0,10.0,")
        else:
            rows.append(f"2313,{tow}.000,emitted,{sigma0:.4f},0,3,18,"
                        f"{clk_median:.4f},0.0500,G10,0.2000,0,0,0,3.0,10.0,0")
    (dir_ / "network_epoch_status.csv").write_text("\n".join(rows) + "\n",
                                                   encoding="utf-8")


def _network_trace(dir_, rms, n=8):
    hdr = ("week,tow,n_present,n_clock_sats,n_phase_bias,declared_dim,gate_ok,"
           "solved,rms_clock_m,max_clock_m,n_pivot_pins")
    rows = [hdr]
    for k in range(n):
        tow = 259200 + 30 * k
        rows.append(f"2313,{tow}.000,3,18,60,2,1,1,{rms:.4f},2.3,60")
    (dir_ / "network_trace.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")


def _flag_rates(dir_, raim_cell_rate):
    hdr = "tier,label,opportunities,flags,rate,k,implied_alpha,has_alpha"
    rows = [hdr,
            f"T3_raim_cell,RAIM cell,10000,{int(raim_cell_rate * 10000)},"
            f"{raim_cell_rate:.6e},3.5,4.652582e-04,1",
            "T1_if_slip,IF slip,20000,4,2.000000e-04,5.0,5.733031e-07,1"]
    (dir_ / "network_flag_rates.csv").write_text("\n".join(rows) + "\n",
                                                 encoding="utf-8")


def _zwd(dir_, std_scale, n=6):
    hdr = "week,tow,station,zwd_m,std_m,carried"
    rows = [hdr]
    rng = np.linspace(0.01, 0.05, n)
    for k in range(n):
        tow = 259200 + 30 * k
        for sta in ("JPLM00USA", "GCGO00USA"):
            rows.append(f"2313,{tow}.000,{sta},2.1000,"
                        f"{std_scale * rng[k]:.5f},0")
    (dir_ / "network_zwd.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")


def _network_solution(dir_, clk_scale, worst_sat="E15", n=6):
    hdr = "week,tow,sat,kind,signal,value_m,disc_counter"
    rows = [hdr]
    for k in range(n):
        tow = 259200 + 30 * k
        rows.append(f"2313,{tow}.000,G10,CLK,,{clk_scale * 0.20:.4f},0")
        # worst_sat carries the biggest |value|.
        rows.append(f"2313,{tow}.000,{worst_sat},CLK,,{clk_scale * 0.90:.4f},0")
    (dir_ / "network_solution.csv").write_text("\n".join(rows) + "\n",
                                               encoding="utf-8")


# --------------------------------------------------------------------------- #
#  align_epochs
# --------------------------------------------------------------------------- #
def test_align_inner_join_and_unmatched_counts():
    a = pd.DataFrame({"week": [2313] * 4, "tow": [0.0, 30.0, 60.0, 90.0],
                      "v": [1.0, 2.0, 3.0, 4.0]})
    # B is missing tow=0, has an extra tow=120 -> 3 matched, 1 only-A, 1 only-B.
    b = pd.DataFrame({"week": [2313] * 4, "tow": [30.0, 60.0, 90.0, 120.0],
                      "v": [10.0, 20.0, 30.0, 40.0]})
    al = align_epochs(a, b, ["v"])
    assert al.n_match == 3
    assert al.n_only_a == 1 and al.n_only_b == 1
    # joined length <= min run length
    assert len(al.merged) <= min(al.n_a, al.n_b)
    # B - A on a matched row has the expected sign / value.
    d = al.merged["v_b"].to_numpy(float) - al.merged["v_a"].to_numpy(float)
    assert np.all(d > 0)


# --------------------------------------------------------------------------- #
#  (a) closed loop -- constructed level shift, sign of delta
# --------------------------------------------------------------------------- #
def test_compare_closed_loop_delta_sign(tmp_path):
    a, b, pair = _pair(tmp_path)
    _closed_loop(a, err_scale=1.0)     # A worse
    _closed_loop(b, err_scale=0.5)     # B better -> B - A < 0
    res = compare_closed_loop(pair)
    assert len(res.figures) == 1
    # delta axis is the 2nd; its first Line is the B-A series.
    delta_ax = res.figures[0].axes[1]
    delta_line = next(t for t in delta_ax.traces if hasattr(t, "y"))
    assert np.all(np.asarray(delta_line.y, float) <= 0)
    assert "3D RMS" in res.summary


def test_compare_closed_loop_skip_one_side_missing(tmp_path):
    a, b, pair = _pair(tmp_path)
    _closed_loop(a, err_scale=1.0)     # only A present
    with pytest.raises(ViewerSkip):
        compare_closed_loop(pair)


# --------------------------------------------------------------------------- #
#  (b) epoch status -- primary + fallback
# --------------------------------------------------------------------------- #
def test_compare_epoch_status_sigma0_delta_sign(tmp_path):
    a, b, pair = _pair(tmp_path)
    _epoch_status(a, clk_median=0.10, sigma0=1.0)
    _epoch_status(b, clk_median=0.10, sigma0=2.0)   # B - A sigma0 = +1
    res = compare_epoch_status(pair)
    assert len(res.figures) == 1 and len(res.figures[0].axes) == 3
    delta_ax = res.figures[0].axes[2]
    delta_line = next(t for t in delta_ax.traces if hasattr(t, "y"))
    d = np.asarray(delta_line.y, float)
    assert np.allclose(d, 1.0)


def test_compare_epoch_status_matched_le_min(tmp_path):
    a, b, pair = _pair(tmp_path)
    # A: 8 rows, 2 withheld -> 6 emitted.  B: 8 rows, 0 withheld -> 8 emitted.
    _epoch_status(a, clk_median=0.1, sigma0=1.0, n=8, n_withheld=2)
    _epoch_status(b, clk_median=0.1, sigma0=1.0, n=8, n_withheld=0)
    al = align_epochs(
        pd.read_csv(a / "network_epoch_status.csv").query("disposition=='emitted'"),
        pd.read_csv(b / "network_epoch_status.csv").query("disposition=='emitted'"),
        ["sigma0"])
    assert al.n_match <= min(al.n_a, al.n_b)
    assert al.n_match == 6


def test_compare_epoch_status_fallback_to_network_trace(tmp_path):
    a, b, pair = _pair(tmp_path)
    _network_trace(a, rms=1.0)
    _network_trace(b, rms=2.0)
    res = compare_epoch_status(pair)   # no epoch_status -> fallback
    assert len(res.figures) == 1
    assert "fallback" in res.summary.lower()


def test_compare_epoch_status_skip_when_neither(tmp_path):
    a, b, pair = _pair(tmp_path)
    with pytest.raises(ViewerSkip):
        compare_epoch_status(pair)


# --------------------------------------------------------------------------- #
#  (c) flag rates -- >3x call-out
# --------------------------------------------------------------------------- #
def test_compare_flag_rates_calls_out_3x(tmp_path):
    a, b, pair = _pair(tmp_path)
    _flag_rates(a, raim_cell_rate=1.0e-3)
    _flag_rates(b, raim_cell_rate=5.0e-3)   # 5x -> called out
    res = compare_flag_rates(pair)
    assert len(res.figures) == 1
    assert "T3_raim_cell" in res.summary
    assert "Called out" in res.summary


def test_compare_flag_rates_skip_one_side(tmp_path):
    a, b, pair = _pair(tmp_path)
    _flag_rates(a, raim_cell_rate=1.0e-3)   # only A
    with pytest.raises(ViewerSkip):
        compare_flag_rates(pair)


# --------------------------------------------------------------------------- #
#  (d) zwd -- final-window delta sign
# --------------------------------------------------------------------------- #
def test_compare_zwd_delta_sign(tmp_path):
    a, b, pair = _pair(tmp_path)
    _zwd(a, std_scale=1.0)
    _zwd(b, std_scale=2.0)   # B std larger -> B-A > 0
    res = compare_zwd(pair)
    assert len(res.figures) == 1
    # last axis is the delta table (TextBox).
    tbox = res.figures[0].axes[-1].traces[0]
    assert "B-A" in tbox.text
    assert "JPLM00USA" in res.summary


def test_compare_zwd_skip_one_side(tmp_path):
    a, b, pair = _pair(tmp_path)
    _zwd(a, std_scale=1.0)
    with pytest.raises(ViewerSkip):
        compare_zwd(pair)


# --------------------------------------------------------------------------- #
#  (e) network clock -- worst-sat surfaced
# --------------------------------------------------------------------------- #
def test_compare_network_clock_worst_sat(tmp_path):
    a, b, pair = _pair(tmp_path)
    _network_solution(a, clk_scale=1.0, worst_sat="E15")
    _network_solution(b, clk_scale=2.0, worst_sat="E15")
    res = compare_network_clock(pair)
    assert len(res.figures) == 1 and len(res.figures[0].axes) == 2
    assert "E15" in res.summary


def test_compare_network_clock_skip_no_clk(tmp_path):
    a, b, pair = _pair(tmp_path)
    # PB-only solution on B -> no CLK rows -> skip
    _network_solution(a, clk_scale=1.0)
    (b / "network_solution.csv").write_text(
        "week,tow,sat,kind,signal,value_m,disc_counter\n"
        "2313,259200.000,G10,PB,L1C,75.5,0\n", encoding="utf-8")
    with pytest.raises(ViewerSkip):
        compare_network_clock(pair)


# --------------------------------------------------------------------------- #
#  (f) scorecard -- contains a worst-sat name; rolls up others
# --------------------------------------------------------------------------- #
def test_compare_scorecard_contains_worst_sat(tmp_path):
    a, b, pair = _pair(tmp_path)
    _network_solution(a, clk_scale=1.0, worst_sat="E11")
    _network_solution(b, clk_scale=1.5, worst_sat="E11")
    _closed_loop(a, err_scale=1.0)
    _closed_loop(b, err_scale=0.5)
    res = compare_scorecard(pair)
    assert len(res.figures) == 1
    # the network-clock roll-up puts the worst-sat NAME into the scorecard text.
    assert "E11" in res.summary
    # a comparator with no data is recorded as skipped, not crashed.
    assert "skipped" in res.summary


# --------------------------------------------------------------------------- #
#  run_comparators orchestration
# --------------------------------------------------------------------------- #
def test_run_comparators_mixed(tmp_path):
    a, b, pair = _pair(tmp_path)
    _closed_loop(a, err_scale=1.0)
    _closed_loop(b, err_scale=0.5)
    # flags only on A -> that comparator skips; scorecard always runs.
    _flag_rates(a, raim_cell_rate=1.0e-3)
    out = run_comparators(pair)
    ran_names = {c.name for c, _ in out.ran}
    skipped_names = {n for n, _ in out.skipped}
    assert "cl" in ran_names
    assert "scorecard" in ran_names
    assert "flags" in skipped_names
    assert not out.failed


def test_run_comparators_only_subset(tmp_path):
    a, b, pair = _pair(tmp_path)
    _closed_loop(a, err_scale=1.0)
    _closed_loop(b, err_scale=0.5)
    out = run_comparators(pair, only=["cl"])
    assert {c.name for c, _ in out.ran} == {"cl"}


def test_comparators_registry_excludes_viewers_registry():
    # comparators are their own list -- NOT the single-ctx viewers REGISTRY.
    from gnssviz.viewers import REGISTRY
    reg_names = {v.name for v in REGISTRY}
    for c in COMPARATORS:
        assert c.name not in reg_names

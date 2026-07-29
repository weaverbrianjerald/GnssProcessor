"""Exam data-layer helpers: TraceContext.truth_* + the (week, tow-ms) matcher."""
import numpy as np
import pandas as pd
import pytest

from gnssviz import TraceContext
from gnssviz.viewers import exam_common as ec


# --------------------------------------------------------------------------- #
#  TraceContext truth access (SPEC §4.1)
# --------------------------------------------------------------------------- #
def test_truth_path_and_has_truth_unset():
    ctx = TraceContext(None)                       # no truth dir bound
    assert ctx.truth_dir is None
    assert ctx.truth_path("truth_position.csv") is None
    assert ctx.has_truth("truth_position.csv") is False


def test_has_truth_present_and_headeronly(exam_fixture):
    truth, logs = exam_fixture()
    ctx = TraceContext(logs, truth_dir=truth)
    assert ctx.has_truth("truth_position.csv") is True
    assert ctx.has_truth("scenario_summary.json") is True     # non-CSV, non-empty
    assert ctx.has_truth("does_not_exist.csv") is False


def test_has_truth_headeronly_csv_is_false(tmp_path):
    (tmp_path / "t").mkdir()
    p = tmp_path / "t" / "truth_slips.csv"
    p.write_text("week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg\n",
                 encoding="utf-8")
    ctx = TraceContext(str(tmp_path), truth_dir=str(tmp_path / "t"))
    assert ctx.has_truth("truth_slips.csv") is False          # header only -> no data


# --------------------------------------------------------------------------- #
#  Truth <-> trace join on the integer (week, tow-ms) grid (SPEC R5)
# --------------------------------------------------------------------------- #
def test_tow_key_is_int_and_ms_snapped():
    k = ec.tow_key([2313, 2313], [302400.0, 302400.0004])     # <0.5 ms apart
    assert k.dtype == np.int64
    assert k[0] == k[1]                                       # snapped to same ms
    k2 = ec.tow_key([2313], [302400.030])
    assert int(k2[0]) - int(k[0]) == 30                       # +30 ms


def test_merge_exact_grid():
    t = pd.DataFrame({"week": [2313, 2313], "tow_s": [302400.0, 302430.0],
                      "x_m": [1.0, 2.0]})
    s = pd.DataFrame({"week": [2313, 2313], "tow_s": [302400.000, 302430.000],
                      "x_m": [1.1, 2.2], "x_std_m": [0.1, 0.1]})
    m = ec.merge_truth_trace(t, s)
    assert len(m) == 2
    # overlapping columns get suffixed; unique ones keep their name.
    assert "x_m_truth" in m and "x_m_trace" in m and "x_std_m" in m and "utc" in m
    assert list(m["x_m_truth"]) == [1.0, 2.0]


def test_merge_dropped_epoch_and_ragged():
    t = pd.DataFrame({"week": [2313] * 3, "tow_s": [302400.0, 302430.0, 302460.0],
                      "x_m": [1.0, 2.0, 3.0]})
    # trace missing the middle epoch (302430) -> inner join yields 2 rows.
    s = pd.DataFrame({"week": [2313, 2313], "tow_s": [302400.0, 302460.0],
                      "x_m": [1.1, 3.3]})
    m = ec.merge_truth_trace(t, s)
    assert len(m) == 2
    assert set(np.round(m["tow_s_truth"], 3)) == {302400.0, 302460.0}


def test_merge_never_string_compares():
    # Float tows that are byte-different strings but equal to the ms still match.
    t = pd.DataFrame({"week": [2313], "tow_s": [302400.0], "x_m": [1.0]})
    s = pd.DataFrame({"week": [2313], "tow_s": [302400.00000001], "x_m": [1.0]})
    assert len(ec.merge_truth_trace(t, s)) == 1


# --------------------------------------------------------------------------- #
#  Slip classification (optional source column, SPEC §4.2)
# --------------------------------------------------------------------------- #
@pytest.mark.parametrize("delta,flagged,source,expected", [
    (0, 1, "random", "false-alarm"),
    (0, 1, "scripted", "false-alarm"),       # Delta=0 wins over scripted
    (-3, 0, "scripted", "scripted"),
    (-3, 1, "random", "flagged"),
    (-3, 0, "random", "unflagged"),
    (-3, 0, "", "unflagged"),                # missing source -> LLI split
])
def test_slip_class(delta, flagged, source, expected):
    assert ec.slip_class(delta, flagged, source) == expected


def test_classify_slips_with_and_without_source():
    df_src = pd.DataFrame({"delta_cycles": [-3, 0, 9], "flagged": [1, 1, 0],
                           "source": ["random", "random", "scripted"]})
    assert list(ec.classify_slips(df_src)) == ["flagged", "false-alarm", "scripted"]
    df_nos = df_src.drop(columns=["source"])
    assert list(ec.classify_slips(df_nos)) == ["flagged", "false-alarm", "unflagged"]


# --------------------------------------------------------------------------- #
#  scalars / mode / storm / verify report / banner
# --------------------------------------------------------------------------- #
def test_scalars_and_mode_and_storm(exam_fixture):
    truth, logs = exam_fixture(kinematic=True)
    ctx = TraceContext(logs, truth_dir=truth)
    sc = ec.scalars(ctx)
    assert sc["isb_gps_gal_m"] == 8.0 and sc["zwd_m"] == 0.15
    assert ec.is_kinematic(ctx) is True
    t0, t1 = ec.storm_window_utc(ctx)
    assert t0 is not None and t1 > t0
    assert len(ec.storm_vlines(ctx)) == 2


def test_mode_fallback_to_summary_then_kinematic(exam_fixture, tmp_path):
    # No truth_scalars kinematic key -> fall back to scenario_summary; else True.
    truth, logs = exam_fixture(kinematic=False)
    ctx = TraceContext(logs, truth_dir=truth)
    assert ec.is_kinematic(ctx) is False
    # empty truth dir -> assume kinematic (R7 legacy).
    empty = tmp_path / "empty"; empty.mkdir()
    assert ec.is_kinematic(TraceContext(logs, truth_dir=str(empty))) is True


def test_verify_report_and_banner(exam_fixture):
    truth, logs = exam_fixture(with_verify=True)
    ctx = TraceContext(logs, truth_dir=truth)
    rep = ec.verify_report(ctx)
    assert rep is not None and rep["pass"] is True and len(rep["checks"]) == 3
    html = ec.verifier_banner_html(rep, "logs/verify_report.json")
    assert "banner pass" in html and "VERIFIER PASS" in html and "isb" in html
    # absent report -> empty banner (no crash).
    assert ec.verifier_banner_html(None) == ""

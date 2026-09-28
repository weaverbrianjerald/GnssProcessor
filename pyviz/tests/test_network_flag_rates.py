"""Build-time tests for the network fault-QC flag-rate viewer (L3, pyviz half).

Exercises ``build_network_flag_rates`` against synthetic ``network_flag_rates.csv``
fixtures: calibration verdicts, the zero-opportunity (no div-by-zero) path, the
T6 blank-alpha row, the optional ``network_run_summary.csv`` companion, and the
:class:`ViewerSkip` paths.  Figures are *built* (spec objects), not rendered.
"""
import pytest

from gnssviz.figures import Line, Scatter, VLine
from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_flag_rates import build_network_flag_rates

_HEADER = ("tier,label,opportunities,flags,rate,k,implied_alpha,has_alpha\n")


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


def _write_flags(tmp_path, body: str):
    (tmp_path / "network_flag_rates.csv").write_text(_HEADER + body,
                                                     encoding="utf-8")


def test_flag_rates_offband_tier_named(tmp_path):
    # T1 well-calibrated (~1x); T3 realized rate ~15x its implied alpha -> off-band.
    body = (
        "T1,chi2,10000,10,1.000000e-01,15.5,1.000000e-01,1\n"
        "T3,raim,10000,150,1.500000e+00,20.0,1.000000e-01,1\n"
    )
    _write_flags(tmp_path, body)
    res = build_network_flag_rates(_ctx(tmp_path))
    assert len(res.figures) == 1
    assert len(res.figures[0].axes) == 2
    # T3 must be named as an off-band offender; T1 must not head the offender list.
    assert "OFF-BAND" in res.summary
    assert "T3" in res.summary
    # Verdicts appear per-tier.
    assert "MIS-CALIBRATED" in res.summary
    assert "OK" in res.summary


def test_flag_rates_all_within_band(tmp_path):
    body = (
        "T1,chi2,10000,10,1.000000e-01,15.5,1.000000e-01,1\n"
        "T2,nis,10000,12,1.200000e-01,10.0,1.000000e-01,1\n"
    )
    _write_flags(tmp_path, body)
    res = build_network_flag_rates(_ctx(tmp_path))
    assert "within" in res.summary.lower()
    assert "OFF-BAND" not in res.summary


def test_flag_rates_zero_opportunity_no_divzero(tmp_path):
    # T5 has zero opportunities: must not divide by zero and must render a hollow
    # (no-exposure) marker rather than a filled realized-rate marker.
    body = (
        "T1,chi2,10000,10,1.000000e-01,15.5,1.000000e-01,1\n"
        "T5,dia,0,0,0.000000e+00,3.0,1.000000e-06,1\n"
    )
    _write_flags(tmp_path, body)
    res = build_network_flag_rates(_ctx(tmp_path))   # must not raise
    assert "T5" in res.summary
    assert "no exposure" in res.summary.lower()
    # A "no exposure" hollow marker (white fill) exists on the rate axis.
    scatters = [t for t in res.figures[0].axes[0].traces if isinstance(t, Scatter)]
    assert any(getattr(s, "label", None) == "no exposure" for s in scatters)


def test_flag_rates_t6_blank_alpha(tmp_path):
    # T6 carries no calibrated alpha (has_alpha==0, implied_alpha blank).
    body = (
        "T1,chi2,10000,10,1.000000e-01,15.5,1.000000e-01,1\n"
        "T6,advisory,500,3,6.000000e-01,0.0,,0\n"
    )
    _write_flags(tmp_path, body)
    res = build_network_flag_rates(_ctx(tmp_path))
    # T6 reported as advisory-only (no ratio / calibration).
    assert "T6" in res.summary
    assert "advisory" in res.summary.lower()
    # No alpha segment (Line) drawn for T6, but the T1 segment exists.
    lines = [t for t in res.figures[0].axes[0].traces if isinstance(t, Line)]
    assert len(lines) == 1     # only the calibrated T1 row gets a segment


def test_flag_rates_zero_rate_clamped_for_log(tmp_path):
    # A 0.0 rate must be clamped to a positive floor so the log-x axis is valid.
    body = "T5,dia,10000,0,0.000000e+00,3.0,1.000000e-06,1\n"
    _write_flags(tmp_path, body)
    res = build_network_flag_rates(_ctx(tmp_path))
    ax = res.figures[0].axes[0]
    assert ax.xscale == "log"
    assert ax.x_is_datetime is False
    xs = []
    for t in ax.traces:
        if isinstance(t, Scatter):
            xs.extend(float(v) for v in t.x)
    assert xs and all(v > 0.0 for v in xs)


def test_flag_rates_alpha_guides_present(tmp_path):
    body = "T1,chi2,10000,10,1.000000e-01,15.5,1.000000e-01,1\n"
    _write_flags(tmp_path, body)
    res = build_network_flag_rates(_ctx(tmp_path))
    vlines = [t for t in res.figures[0].axes[0].traces if isinstance(t, VLine)]
    guide_x = sorted(float(v.x) for v in vlines)
    assert 1e-6 in guide_x and 1e-3 in guide_x


def test_flag_rates_run_summary_appended(tmp_path):
    _write_flags(tmp_path, "T1,chi2,10000,10,1.000000e-01,15.5,1.000000e-01,1\n")
    (tmp_path / "network_run_summary.csv").write_text(
        "n_synced,n_emitted,n_skipped,n_gate_withheld,n_sigma0_withheld,"
        "loop_ms,ms_per_epoch,n_stations,n_threads,parallel\n"
        "120,118,2,1,6,2400.0,20.0,50,8,true\n", encoding="utf-8")
    res = build_network_flag_rates(_ctx(tmp_path))
    # withheld_frac = 6 / 120 = 0.05 -> surfaced in both table and summary.
    assert "withheld_frac" in res.summary
    assert "0.05" in res.summary
    tab = res.figures[0].axes[1].traces[0].text
    assert "Run summary" in tab
    assert "n_synced" in tab and "withheld_frac" in tab


def test_flag_rates_missing_run_summary_tolerated(tmp_path):
    _write_flags(tmp_path, "T1,chi2,10000,10,1.000000e-01,15.5,1.000000e-01,1\n")
    res = build_network_flag_rates(_ctx(tmp_path))   # no run-summary file present
    assert len(res.figures) == 1
    tab = res.figures[0].axes[1].traces[0].text
    assert "Run summary" not in tab


def test_flag_rates_skip_absent(tmp_path):
    with pytest.raises(ViewerSkip):
        build_network_flag_rates(_ctx(tmp_path))


def test_flag_rates_skip_empty(tmp_path):
    # Header-only (no data rows) -> ViewerSkip.
    (tmp_path / "network_flag_rates.csv").write_text(_HEADER, encoding="utf-8")
    with pytest.raises(ViewerSkip):
        build_network_flag_rates(_ctx(tmp_path))

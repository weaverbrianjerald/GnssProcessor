"""Build-time tests for the network_events viewer (synthetic CSVs).

Covers: per-constellation figure/marker contract, event-name -> shape mapping,
the optional Figure-2 disposition raster from network_trace.csv (present and
absent), the all-system-wide (no sat-attributed) degrade path, summary tallies,
and the ViewerSkip / missing-CSV paths.
"""
import importlib

import pytest

from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_events import (build_network_events, _shape_for)

_HEADER = ("week,tow,tier,event,sat,station,statistic,threshold,action,detail\n")


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


def _events_csv(tmp_path, rows):
    (tmp_path / "network_events.csv").write_text(_HEADER + "".join(rows),
                                                 encoding="utf-8")


def _row(tow, tier, event, sat="", station="", stat="", thr="", action="",
         detail=""):
    return (f"2313,{tow}.000,{tier},{event},{sat},{station},{stat},{thr},"
            f"{action},{detail}\n")


def test_shape_mapping():
    # refloat -> x, withhold -> v, retro_bump -> s, pivot/handover -> D.
    assert _shape_for("we_refloat_sat") == "x"
    assert _shape_for("consensus_refloat") == "x"
    assert _shape_for("refsat_reset") == "x"
    assert _shape_for("withhold") == "v"
    assert _shape_for("we_exclude_station") == "v"
    assert _shape_for("retro_bump") == "s"
    assert _shape_for("handover") == "D"
    assert _shape_for("something_unmapped") == "o"


def test_events_by_satellite_markers(tmp_path):
    from gnssviz.figures import Markers, Raster
    rows = [
        _row(259200, "T2_within_epoch", "we_refloat_sat", sat="G13"),
        _row(259230, "T3_raim", "consensus_refloat", sat="G13"),
        _row(259260, "T6_clock_monitor", "clk_monitor_refloat", sat="E15"),
        _row(259290, "RETRO", "retro_bump", sat="G13"),
    ]
    _events_csv(tmp_path, rows)
    res = build_network_events(_ctx(tmp_path))
    assert len(res.figures) >= 1
    fig1 = res.figures[0]
    # Two constellations present (GPS, Galileo) -> two axes.
    titles = [ax.title for ax in fig1.axes]
    assert "GPS" in titles and "Galileo" in titles
    gps_ax = next(ax for ax in fig1.axes if ax.title == "GPS")
    assert any(isinstance(t, Raster) for t in gps_ax.traces)
    markers = [t for t in gps_ax.traces if isinstance(t, Markers)]
    assert markers, "expected marker layers on the GPS tile"
    # Marker rows are 0-based raster indices.
    for m in markers:
        assert all(isinstance(r, int) and r >= 0 for r in m.rows)
    # Summary tallies tiers + top sats.
    assert "Events per tier" in res.summary
    assert "G13" in res.summary


def test_disposition_raster_from_network_trace(tmp_path):
    from gnssviz.figures import Raster
    rows = [_row(259200, "GATE", "gate_withheld", action="withhold_epoch")]
    _events_csv(tmp_path, rows)
    # network_trace.csv present -> Figure 2 appears.
    trace = "".join(
        f"2313,{259200 + 30 * k},3,18,60,2,1,{1 if k > 0 else 0},"
        f"1.1,2.3,60\n" for k in range(4))
    (tmp_path / "network_trace.csv").write_text(
        "week,tow,n_present,n_clock_sats,n_phase_bias,declared_dim,gate_ok,"
        "solved,rms_clock_m,max_clock_m,n_pivot_pins\n" + trace,
        encoding="utf-8")
    res = build_network_events(_ctx(tmp_path))
    assert len(res.figures) == 2
    fig2 = res.figures[1]
    assert any(isinstance(t, Raster) for ax in fig2.axes for t in ax.traces)
    assert "Aggregate trace" in res.summary


def test_degrades_without_network_trace(tmp_path):
    # No network_trace.csv -> only Figure 1, no crash.
    rows = [_row(259200, "T3_raim", "consensus_refloat", sat="G05")]
    _events_csv(tmp_path, rows)
    res = build_network_events(_ctx(tmp_path))
    assert len(res.figures) == 1


def test_all_system_wide_events_no_sat(tmp_path):
    # Every event has an empty sat (GATE/SIGMA0 epoch-wide) -> Figure 1 still
    # valid (a text-note tile), summary says all events are system-wide.
    rows = [
        _row(259200, "GATE", "gate_withheld", action="withhold_epoch"),
        _row(259230, "SIGMA0", "withhold", action="withhold_epoch"),
    ]
    _events_csv(tmp_path, rows)
    res = build_network_events(_ctx(tmp_path))
    assert len(res.figures) >= 1
    assert "system-wide" in res.summary.lower()


def test_skip_when_missing_csv(tmp_path):
    with pytest.raises(FileNotFoundError):
        build_network_events(_ctx(tmp_path))


def test_skip_when_header_only(tmp_path):
    (tmp_path / "network_events.csv").write_text(_HEADER, encoding="utf-8")
    with pytest.raises(ViewerSkip):
        build_network_events(_ctx(tmp_path))


def test_source_has_no_plotting_import():
    src = importlib.util.find_spec("gnssviz.viewers.network_events").origin
    text = open(src, encoding="utf-8").read()
    assert "import matplotlib" not in text and "from matplotlib" not in text
    assert "import plotly" not in text and "from plotly" not in text

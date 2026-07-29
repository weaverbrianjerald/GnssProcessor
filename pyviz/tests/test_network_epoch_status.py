"""Build-time tests for the network_epoch_status viewer (synthetic CSVs).

Covers: figure/axes contract, the disposition classification with BLANK numeric
fields at non-emitted dispositions, the plateau (LOCK-IN) heuristic on a growing
clock-std vs a flat one, and the ViewerSkip / missing-CSV paths.  Figures are
built (spec objects), never rendered, so these stay fast and backend-free.
"""
import importlib

import pytest

from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_epoch_status import build_network_epoch_status

_HEADER = ("week,tow,disposition,sigma0,n_adapt,n_present,n_clock_sats,"
           "clock_median_m,clock_std_m,worst_sat,worst_dev_m,n_refloat_sats,"
           "n_retro_applied,datum_changed,sigma0_trigger,withhold_ceiling,"
           "n_arc_refloat\n")   # T2d: isolated-arc RAIM refloats this epoch


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


def _emitted_row(k, std):
    """A fully-populated emitted row with a given clock_std_m."""
    tow = 259200 + 30 * k
    return (f"2313,{tow}.000,emitted,{1.0 + 0.02 * k:.4f},20,18,60,"
            f"{0.10:.4f},{std:.4f},G13,{0.25 + 0.01 * k:.4f},"
            f"{1 if k % 5 == 0 else 0},{1 if k == 3 else 0},"
            f"{1 if k == 2 else 0},3.0000,5.0000,0")   # +n_arc_refloat


def _skip_row(k, disposition):
    """A non-emitted row: numeric stat fields BLANK (-> NaN), datum unchanged."""
    tow = 259200 + 30 * k
    # week,tow,disposition,sigma0(blank),n_adapt(blank),n_present,n_clock_sats(blank),
    # clock_median_m(blank),clock_std_m(blank),worst_sat(blank),worst_dev_m(blank),
    # n_refloat_sats(blank),n_retro_applied(blank),datum_changed,trigger,ceiling
    return (f"2313,{tow}.000,{disposition},,,12,,,,,,,,0,3.0000,5.0000,")   # +n_arc_refloat (blank)


def test_plateau_fixture_flags_lockin(tmp_path):
    # clock_std GROWS from ~0.02 to ~0.30 over 12 emitted epochs: last-third
    # median >> 2x the first-third median -> LOCK-IN SUSPECTED.
    stds = [0.02, 0.02, 0.03, 0.03, 0.04, 0.04, 0.15, 0.18, 0.22, 0.26, 0.30, 0.32]
    body = "".join(_emitted_row(k, s) + "\n" for k, s in enumerate(stds))
    (tmp_path / "network_epoch_status.csv").write_text(_HEADER + body,
                                                       encoding="utf-8")
    res = build_network_epoch_status(_ctx(tmp_path))
    assert len(res.figures) == 1
    assert len(res.figures[0].axes) == 3
    assert "LOCK-IN SUSPECTED" in res.summary
    # Run-worst sat surfaced by NAME, not rms alone.
    assert "G13" in res.summary


def test_flat_fixture_does_not_flag_lockin(tmp_path):
    # A flat / shrinking clock-std: last/first <= 2 -> no lock-in.
    stds = [0.30, 0.28, 0.26, 0.22, 0.18, 0.15, 0.12, 0.10, 0.09, 0.08, 0.08, 0.07]
    body = "".join(_emitted_row(k, s) + "\n" for k, s in enumerate(stds))
    (tmp_path / "network_epoch_status.csv").write_text(_HEADER + body,
                                                       encoding="utf-8")
    res = build_network_epoch_status(_ctx(tmp_path))
    assert "LOCK-IN SUSPECTED" not in res.summary
    assert "no lock-in signature" in res.summary


def test_disposition_classification_with_blank_fields(tmp_path):
    # Mix of emitted + every skip disposition; skip rows have BLANK numeric fields
    # which load_trace reads as NaN.  The viewer must classify all of them without
    # tripping over the NaNs, and must NOT count a datum change on the skip rows.
    rows = [_emitted_row(0, 0.05), _emitted_row(1, 0.06)]
    for k, disp in enumerate(("gate_withheld", "sigma0_withheld",
                              "solve_failed", "datum_failed", "sync_skip"),
                             start=2):
        rows.append(_skip_row(k, disp))
    body = "".join(r + "\n" for r in rows)
    (tmp_path / "network_epoch_status.csv").write_text(_HEADER + body,
                                                       encoding="utf-8")
    res = build_network_epoch_status(_ctx(tmp_path))
    s = res.summary
    for disp in ("emitted", "gate_withheld", "sigma0_withheld",
                 "solve_failed", "datum_failed", "sync_skip"):
        assert disp in s
    # Disposition lane axis has a scatter per present disposition (>= 6 lanes).
    lanes = res.figures[0].axes[0].traces
    # At least the 6 dispositions each contribute a scatter (VLines may add more).
    from gnssviz.figures import Scatter
    assert sum(isinstance(t, Scatter) for t in lanes) >= 6


def test_datum_change_draws_vline(tmp_path):
    from gnssviz.figures import VLine
    stds = [0.05, 0.05, 0.05, 0.05, 0.05, 0.05]
    body = "".join(_emitted_row(k, s) + "\n" for k, s in enumerate(stds))
    (tmp_path / "network_epoch_status.csv").write_text(_HEADER + body,
                                                       encoding="utf-8")
    res = build_network_epoch_status(_ctx(tmp_path))
    lane_ax = res.figures[0].axes[0]
    assert any(isinstance(t, VLine) for t in lane_ax.traces)  # k==2 has datum_changed=1
    assert "Datum changes    : 1" in res.summary


def test_skip_when_missing_csv(tmp_path):
    with pytest.raises(FileNotFoundError):
        build_network_epoch_status(_ctx(tmp_path))


def test_skip_when_header_only(tmp_path):
    (tmp_path / "network_epoch_status.csv").write_text(_HEADER, encoding="utf-8")
    with pytest.raises(ViewerSkip):
        build_network_epoch_status(_ctx(tmp_path))


def test_source_has_no_plotting_import():
    src = importlib.util.find_spec(
        "gnssviz.viewers.network_epoch_status").origin
    text = open(src, encoding="utf-8").read()
    assert "import matplotlib" not in text and "from matplotlib" not in text
    assert "import plotly" not in text and "from plotly" not in text

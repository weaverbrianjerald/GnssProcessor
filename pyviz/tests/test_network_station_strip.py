"""Build-time tests for the network per-station residual-field viewer (L13).

Exercises ``build_network_station_strip`` against a synthetic two-station fixture
-- enough to catch schema / contract regressions -- plus the ``ViewerSkip`` path.
Figures are *built* (spec objects), not rendered, so these stay fast and
backend-free.
"""
import pytest

from gnssviz.figures import Figure, Raster
from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_station_strip import build_network_station_strip


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


_HEADER = "week,tow,station,sat,s_raw,s_norm,sigma0\n"


def _write(tmp_path, body):
    (tmp_path / "network_station_residuals.csv").write_text(
        _HEADER + body, encoding="utf-8")


def test_two_station_worst_ranks_first(tmp_path):
    """STAB0 has large residuals, STAA0 small -> STAB0 must rank worst (row 0)."""
    rows = []
    for k in range(4):
        tow = 259200 + 30 * k
        # STAA0: quiet station (small absolute normalized residuals).
        rows.append(f"2313,{tow}.000,STAA0,G10,0.10,0.20,0.5")
        rows.append(f"2313,{tow}.000,STAA0,E15,0.09,0.18,0.5")
        # STAB0: noisy station (large absolute normalized residuals).
        rows.append(f"2313,{tow}.000,STAB0,G10,3.10,4.50,0.5")
        rows.append(f"2313,{tow}.000,STAB0,E15,2.90,5.20,0.5")
    _write(tmp_path, "\n".join(rows) + "\n")

    res = build_network_station_strip(_ctx(tmp_path))

    # Figure 0 is the station-health raster; its row_labels are RMS-ranked with
    # the worst on top (row 0).
    raster_fig = res.figures[0]
    assert isinstance(raster_fig, Figure)
    raster = next(t for ax in raster_fig.axes for t in ax.traces
                  if isinstance(t, Raster))
    assert list(raster.row_labels)[0] == "STAB0"        # worst ranked first
    assert set(raster.row_labels) == {"STAA0", "STAB0"}

    # A worst-station drill-down strip figure was also built (>= 1 constellation).
    assert len(res.figures) >= 2

    # Summary reports the top-3 ranking with the worst station first.
    assert "STAB0" in res.summary
    idx_b = res.summary.index("STAB0")
    idx_a = res.summary.index("STAA0")
    assert idx_b < idx_a                                 # worst mentioned first


def test_summary_has_median_std_worstsat(tmp_path):
    rows = []
    for k in range(3):
        tow = 259200 + 30 * k
        rows.append(f"2313,{tow}.000,STAA0,G10,0.10,0.20,0.5")
        rows.append(f"2313,{tow}.000,STAA0,G13,0.90,2.00,0.5")   # G13 worst sat
    _write(tmp_path, "\n".join(rows) + "\n")
    res = build_network_station_strip(_ctx(tmp_path))
    # Per-station worst-satellite by mean s_norm should be G13 for STAA0.
    assert "G13" in res.summary
    assert "worstSat" in res.summary
    assert "medS" in res.summary


def test_strip_uses_viridis_sequential(tmp_path):
    """Drill-down strip must be sequential (non-diverging) for absolute values."""
    rows = []
    for k in range(3):
        tow = 259200 + 30 * k
        rows.append(f"2313,{tow}.000,STAB0,G10,3.0,4.0,0.5")
        rows.append(f"2313,{tow}.000,STAA0,G10,0.1,0.2,0.5")
    _write(tmp_path, "\n".join(rows) + "\n")
    res = build_network_station_strip(_ctx(tmp_path))
    # A raster figure carries a figure-level cmap; every built figure is a raster.
    for fig in res.figures:
        assert fig.cmap is not None
        # Sequential map: colour limits are non-negative (no symmetric ±).
        assert fig.clim is not None and fig.clim[0] >= 0.0


def test_skip_on_empty(tmp_path):
    _write(tmp_path, "")               # header only, no data rows
    with pytest.raises(ViewerSkip):
        build_network_station_strip(_ctx(tmp_path))


def test_skip_on_all_nan_snorm(tmp_path):
    # Present rows but s_norm all blank (NaN) -> nothing to draw.
    rows = ["2313,259200.000,STAA0,G10,0.1,,0.5",
            "2313,259230.000,STAA0,G10,0.1,,0.5"]
    _write(tmp_path, "\n".join(rows) + "\n")
    with pytest.raises(ViewerSkip):
        build_network_station_strip(_ctx(tmp_path))

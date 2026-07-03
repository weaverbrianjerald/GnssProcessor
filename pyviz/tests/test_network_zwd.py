"""Build-time tests for the network per-station ZWD viewer (synthetic CSVs).

Exercises ``build_network_zwd`` against stub ``network_zwd.csv`` traces -- the
sigma-sharpening (carried) two-station fixture, the station ranking, and the
:class:`ViewerSkip` paths (absent / header-only) -- without rendering.
"""
import pytest

from gnssviz.figures import Band, Line, Stem, TextBox
from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_zwd import build_network_zwd


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


def _write_two_station(tmp_path):
    """Two stations: STA_A un-carried (wider σ), STA_B carried (sharpening σ)."""
    rows = ["week,tow,station,zwd_m,std_m,carried"]
    for k in range(8):
        tow = 259200 + 30 * k
        # STA_A: never carried, σ stays wide.
        rows.append(f"2313,{tow}.000,STA_A,0.150,0.020,0")
        # STA_B: carried for k>=4, σ sharpens (shrinks) once carried.
        carried = 1 if k >= 4 else 0
        sig = 0.005 if carried else 0.018
        rows.append(f"2313,{tow}.000,STA_B,0.140,{sig:.3f},{carried}")
    (tmp_path / "network_zwd.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")


def test_network_zwd_basic(tmp_path):
    _write_two_station(tmp_path)
    res = build_network_zwd(_ctx(tmp_path))
    assert len(res.figures) == 1
    fig = res.figures[0]
    # Three axes: ZWD, sigma (V7), station-rank.
    assert len(fig.axes) == 3
    ax_zwd, ax_sig, ax_rank = fig.axes
    # ZWD axis has band + line traces.
    assert any(isinstance(t, Band) for t in ax_zwd.traces)
    assert any(isinstance(t, Line) for t in ax_zwd.traces)


def test_network_zwd_carried_shading_and_sharpening(tmp_path):
    _write_two_station(tmp_path)
    res = build_network_zwd(_ctx(tmp_path))
    ax_sig = res.figures[0].axes[1]
    # The V7 axis must draw a carried-window shading Band and a carried Stem.
    assert any(isinstance(t, Band) for t in ax_sig.traces)
    assert any(isinstance(t, Stem) for t in ax_sig.traces)
    # Sigma sharpening: STA_B's carried σ (0.005) is smaller than its
    # un-carried σ (0.018) and than STA_A's σ (0.020) -- so STA_B ranks BEST
    # (last, smallest last-30-min median σ) and STA_A ranks WORST (first).
    summary = res.summary
    assert "worst STA_A" in summary
    # carried fraction of the whole trace is > 0 (STA_B carried half its epochs).
    assert "carried" in summary


def test_network_zwd_ranking_present(tmp_path):
    _write_two_station(tmp_path)
    res = build_network_zwd(_ctx(tmp_path))
    ax_rank = res.figures[0].axes[2]
    # Ranking axis is numeric-x (no categorical xticks) with TextBox labels.
    assert ax_rank.x_is_datetime is False
    labels = [t.text for t in ax_rank.traces if isinstance(t, TextBox)]
    assert "STA_A" in labels and "STA_B" in labels
    # Summary carries the required phrasing.
    assert "per-station sigma (last 30 min): median" in res.summary


def test_network_zwd_skip_absent(tmp_path):
    with pytest.raises(ViewerSkip):
        build_network_zwd(_ctx(tmp_path))


def test_network_zwd_skip_empty(tmp_path):
    (tmp_path / "network_zwd.csv").write_text(
        "week,tow,station,zwd_m,std_m,carried\n", encoding="utf-8")
    with pytest.raises(ViewerSkip):
        build_network_zwd(_ctx(tmp_path))

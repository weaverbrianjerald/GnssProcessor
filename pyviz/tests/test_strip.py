"""Strip renderer internals: percentile, PRN sort, caxis, figure build."""
import numpy as np
import pandas as pd

from gnssviz.figures import Raster
from gnssviz.strip import _pctl, _prn_sort, _resolve_caxis, build_sat_epoch_strip


def test_pctl_nearest_rank():
    v = np.arange(1, 101, dtype=float)            # 1..100
    # nearest-rank: idx = round(q/100*(n-1)); q=2 -> idx 2 -> value 3; q=98 -> idx 97 -> 98
    assert _pctl(v, 2) == 3.0
    assert _pctl(v, 98) == 98.0


def test_pctl_half_away_from_zero():
    # n=126, q=2 -> 2*(125)/100 = 2.5 exactly. MATLAB rounds half away from zero
    # (idx 3 -> value 4), not banker's-to-even (idx 2 -> value 3).
    v = np.arange(1, 127, dtype=float)
    assert _pctl(v, 2) == 4.0


def test_prn_sort_numeric():
    assert _prn_sort(["G10", "G02", "G01"]) == ["G01", "G02", "G10"]
    # unparseable PRNs sort last, stably
    out = _prn_sort(["E05", "BAD", "E01"])
    assert out[:2] == ["E01", "E05"] and out[-1] == "BAD"


def test_resolve_caxis_passthrough_and_symmetric():
    assert _resolve_caxis([0, 90], np.array([1.0, 2.0]), False) == (0.0, 90.0)
    lo, hi = _resolve_caxis("autosym", np.array([-5.0, 3.0, 4.0]), True)
    assert lo == -hi and hi > 0


def test_build_categorical_strip_makes_raster():
    n = 6
    t = pd.to_datetime("2024-05-08") + pd.to_timedelta(np.arange(n) * 30, unit="s")
    D = pd.DataFrame({
        "system": ["GPS"] * n,
        "satellite": ["G01", "G02"] * (n // 2),
        "facet": [""] * n,
        "utc": list(t[:n // 2]) * 2,
        "value": [1.0, 2.0, 1.0, 1.0, 2.0, 2.0],
    })
    figs = build_sat_epoch_strip(
        D, mode="categorical",
        cat={"labels": ["A", "B"], "colors": np.array([[0, 0, 1.0], [1.0, 0, 0]])},
        fig_title="t", value_label="status", fig_tag="sat")
    assert len(figs) == 1
    fig = figs[0]
    assert fig.categorical and fig.clim == (0.5, 2.5)
    rasters = [tr for ax in fig.axes for tr in ax.traces if isinstance(tr, Raster)]
    assert len(rasters) == 1 and rasters[0].grid.shape[0] == 2   # 2 satellites


def test_empty_input_returns_no_figures():
    assert build_sat_epoch_strip(pd.DataFrame()) == []


def test_annot_mask_excludes_cells_from_stat():
    from gnssviz.figures import TextBox
    t = pd.to_datetime("2024-05-08") + pd.to_timedelta(np.arange(4) * 30, unit="s")
    # 2 real cells (10, 20) + 2 synthetic (0.0, annot=False) on distinct sats.
    D = pd.DataFrame({
        "system": ["GPS"] * 4,
        "satellite": ["G01", "G02", "G03", "G04"],
        "facet": [""] * 4,
        "utc": list(t),
        "value": [10.0, 20.0, 0.0, 0.0],
        "annot": [True, True, False, False],
    })
    figs = build_sat_epoch_strip(D, mode="continuous", value_label="r", annotate=True)
    boxes = [tr for ax in figs[0].axes for tr in ax.traces if isinstance(tr, TextBox)]
    assert boxes and "N=2" in boxes[0].text          # synthetics excluded
    assert "15" in boxes[0].text                      # mean of (10, 20), not (10,20,0,0)

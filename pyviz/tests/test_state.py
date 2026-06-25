"""State viewer helpers: converged-window stats and reference-point guards."""
import numpy as np

from gnssviz.viewers.state import _enu_stats, _half_index, _ref_median, _summary_text


def test_single_sample_sigma_is_zero_not_nan():
    # One finite sample in the window -> sigma 0 (MATLAB std of one element), not NaN.
    e = np.array([np.nan, 5.0])
    stats = _enu_stats(e, e, e, e, h0=1)
    mu, sd = stats["E"]
    assert sd == 0.0 and np.isfinite(mu)


def test_ref_median_all_nan_returns_nan_no_warning():
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("error")          # any RuntimeWarning -> failure
        assert np.isnan(_ref_median(np.array([np.nan, np.nan]), 0))


def test_summary_handles_skipped_enu():
    txt = _summary_text(None, n=3, h0=1)
    assert "ENU figure skipped" in txt


def test_half_index_matches_matlab_round():
    # MATLAB: halfIdx(1-based) = n - round(n/2); 0-based start = that - 1.
    assert _half_index(100) == 49        # n-50-1
    assert _half_index(5) == 1           # round(2.5)=3 -> 5-3-1=1

"""Build-time tests for the network DD fractional-cycle viewer (synthetic CSVs).

Mirrors ``test_network_viewers.py``: writes stub CSV text, calls the ``build``
fn directly, and asserts on the returned :class:`Figure` / summary specs (no
rendering).  Also pins the robust-sigma replication so it can never silently
drift from the standalone ``scripts/view_network_dd_fractional.py``.
"""
import numpy as np
import pytest

from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_dd_fractional import (build_network_dd_fractional,
                                                   robust_std, wrap_half)

_HDR = ("week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,dd_m,lambda_m,"
        "dd_cyc,frac_cyc")


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


def _write(tmp_path, rows):
    (tmp_path / "network_dd_fractional.csv").write_text(
        _HDR + "\n" + "\n".join(rows) + "\n", encoding="utf-8")


def _row(tow, cons, sig, sat_i, ref, a, b, frac, lam=0.19029):
    # dd_m / dd_cyc are not consumed by the viewer beyond frac_cyc; fill plausibly.
    dd_cyc = 5.0 + frac
    return (f"2313,{tow}.000,{cons},{sig},{sat_i},{ref},{a},{b},"
            f"{dd_cyc * lam:.5f},{lam},{dd_cyc:.5f},{frac:.6f}")


def test_robust_std_matches_standalone_semantics():
    # Upper-median of |v| (index len//2, no interpolation) * 1.4826.
    vals = [0.01, -0.02, 0.03, -0.40]          # n even -> upper of the two middles
    s = sorted(abs(v) for v in vals)           # [0.01,0.02,0.03,0.40]
    assert robust_std(vals) == pytest.approx(1.4826 * s[len(s) // 2])  # 0.03
    # np.median would average 0.02 & 0.03 -> a different number; ensure we differ.
    assert robust_std(vals) != pytest.approx(1.4826 * float(np.median(np.abs(vals))))


def test_wrap_half():
    assert wrap_half(0.4) == pytest.approx(0.4)
    assert wrap_half(1.4) == pytest.approx(0.4)
    assert wrap_half(-1.4) == pytest.approx(-0.4)


def test_clustered_fractional_small_sigma_and_go(tmp_path):
    rng = np.random.default_rng(0)
    rows = []
    tow0 = 259200
    for k in range(30):                      # 30 epochs, several DD pairs each
        for j in range(6):
            frac = float(rng.normal(0.0, 0.01))   # tight cluster near 0
            rows.append(_row(tow0 + 30 * k, "G", "L1C", f"G{10 + j:02d}",
                             "G32", "STA1", f"STA{2 + j}", frac))
    _write(tmp_path, rows)
    res = build_network_dd_fractional(_ctx(tmp_path))
    assert len(res.figures) == 2                        # histogram + sigma-over-time
    title = res.figures[0].axes[0].title
    assert "GPS L1C" in title
    assert "within +/-0.1" in title
    # A clustered group reports a small sigma and a GO advisory.
    sigma = float(title.split("sigma=")[1].split(" cyc")[0])
    assert sigma < 0.05
    assert "ADVISORY: GO" in res.summary


def test_uniform_fractional_large_sigma_nogo(tmp_path):
    rng = np.random.default_rng(1)
    rows = []
    for k in range(40):
        for j in range(6):
            frac = float(rng.uniform(-0.5, 0.5))         # smeared / flat
            rows.append(_row(259200 + 30 * k, "E", "E1C", f"E{10 + j:02d}",
                             "E30", "STA1", f"STA{2 + j}", frac))
    _write(tmp_path, rows)
    res = build_network_dd_fractional(_ctx(tmp_path))
    # Uniform spread -> much larger robust sigma than the clustered case + NO-GO.
    title = res.figures[0].axes[0].title
    sigma = float(title.split("sigma=")[1].split(" cyc")[0])
    assert sigma > 0.1
    assert "NO-GO" in res.summary


def test_lambda_zero_rows_ignored(tmp_path):
    rows = [
        _row(259200, "G", "L1C", "G10", "G32", "STA1", "STA2", 0.02),
        # lambda_m == 0 -> must be dropped (unknown signal, cycle cols are 0).
        _row(259230, "G", "LXX", "G10", "G32", "STA1", "STA2", 0.40, lam=0.0),
    ]
    _write(tmp_path, rows)
    res = build_network_dd_fractional(_ctx(tmp_path))
    # Only the L1C group survives; the LXX (lambda 0) group is gone.
    titles = " ".join(ax.title for f in res.figures for ax in f.axes)
    assert "L1C" in titles
    assert "LXX" not in titles
    assert "Groups (cons,signal): 1" in res.summary


def test_skip_when_absent(tmp_path):
    with pytest.raises(ViewerSkip):
        build_network_dd_fractional(_ctx(tmp_path))


def test_skip_when_all_lambda_zero(tmp_path):
    _write(tmp_path, [_row(259200, "G", "LXX", "G10", "G32",
                           "STA1", "STA2", 0.4, lam=0.0)])
    with pytest.raises(ViewerSkip):
        build_network_dd_fractional(_ctx(tmp_path))

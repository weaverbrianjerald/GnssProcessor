"""Build-time tests for the network anchor/carry/data-info viewer (synthetic CSVs).

Mirrors ``test_network_viewers.py``: writes stub CSV text, calls ``build``
directly, and asserts on the returned specs.  Covers the lock-in naming, the
pivot-row exclusion, and the ViewerSkip paths.
"""
import pytest

from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_anchor_info import build_network_anchor_info

_HDR = ("week,tow,sat,anchor_info,carry_info,data_info,total_info,"
        "anchor_frac_data,carry_frac,frac,is_pivot")


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


def _row(tow, sat, *, anchor, carry, data, carry_frac, anchor_frac_data,
         is_pivot=0):
    total = anchor + carry + data
    return (f"2313,{tow}.000,{sat},{anchor},{carry},{data},{total},"
            f"{anchor_frac_data},{carry_frac},0.0,{is_pivot}")


def _write(tmp_path, rows):
    (tmp_path / "network_anchor_info.csv").write_text(
        _HDR + "\n" + "\n".join(rows) + "\n", encoding="utf-8")


def test_carry_dominated_sat_named(tmp_path):
    rows = []
    for k in range(20):                        # 20 epochs
        tow = 259200 + 30 * k
        # G13 is carry-dominated (locked in): carry_frac pinned high, data starved.
        rows.append(_row(tow, "G13", anchor=0.1, carry=9.0, data=0.2,
                         carry_frac=0.97, anchor_frac_data=0.05))
        # G20 is healthy: fresh data dominates, carry low.
        rows.append(_row(tow, "G20", anchor=0.1, carry=1.0, data=8.0,
                         carry_frac=0.20, anchor_frac_data=0.02))
    _write(tmp_path, rows)
    res = build_network_anchor_info(_ctx(tmp_path))
    # 3 figs: carry_frac strip (GPS), anchor_frac strip (GPS), worst-sat combo.
    assert len(res.figures) == 3
    assert "LOCK-IN SIGNATURE" in res.summary
    assert "G13" in res.summary
    assert "Run-worst sat: G13" in res.summary
    # Worst-sat StackedArea tile names the locked sat.
    combo = res.figures[-1]
    assert "G13" in combo.axes[0].title


def test_no_lockin_when_carry_low(tmp_path):
    rows = [_row(259200 + 30 * k, "G20", anchor=0.1, carry=1.0, data=8.0,
                 carry_frac=0.20, anchor_frac_data=0.02) for k in range(15)]
    _write(tmp_path, rows)
    res = build_network_anchor_info(_ctx(tmp_path))
    assert "no lock-in" in res.summary.lower()


def test_pivot_rows_excluded(tmp_path):
    rows = []
    for k in range(12):
        tow = 259200 + 30 * k
        # Pivot sat looks "locked" (data_info inflated by the pin) but is EXCLUDED.
        rows.append(_row(tow, "G01", anchor=5.0, carry=9.0, data=50.0,
                         carry_frac=0.99, anchor_frac_data=0.4, is_pivot=1))
        rows.append(_row(tow, "G20", anchor=0.1, carry=1.0, data=8.0,
                         carry_frac=0.20, anchor_frac_data=0.02))
    _write(tmp_path, rows)
    res = build_network_anchor_info(_ctx(tmp_path))
    # Pivot G01 is excluded -> not the worst sat, not a lock-in hit, counted out.
    assert "Run-worst sat: G20" in res.summary
    assert "G01" not in res.summary
    assert "Pivot rows excluded from all stats: 12" in res.summary
    assert "no lock-in" in res.summary.lower()


def test_skip_when_absent(tmp_path):
    with pytest.raises(ViewerSkip):
        build_network_anchor_info(_ctx(tmp_path))


def test_skip_when_only_pivot_rows(tmp_path):
    _write(tmp_path, [_row(259200, "G01", anchor=5.0, carry=9.0, data=50.0,
                           carry_frac=0.99, anchor_frac_data=0.4, is_pivot=1)])
    with pytest.raises(ViewerSkip):
        build_network_anchor_info(_ctx(tmp_path))

"""Build-time tests for the ``network_clock_nis`` viewer (synthetic CSVs).

Covers the two column-remapped modes served by the single
``network_clock_monitor.csv``: model (nis/mode present, one over-k fire),
monitor (blank nis, ",," prev/jump), a LEGACY file with no nis/mode columns
routed by the ``abs_max_m.nunique()`` heuristic, and the ViewerSkip / mode-
resolution edge cases.  Figures are *built* (spec objects), not rendered.
"""
import pytest

from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_clock_nis import (
    _resolve_mode,
    build_network_clock_nis,
)
from gnssviz.io import load_trace


# network_clock_monitor.csv column order (L4 header):
#   week,tow,sat,clk_m,median_m,dev_m,prev_clk_m,jump_m,abs_max_m,jump_max_m,
#   flag_abs,flag_jump,flag_ext,refloated,nis,mode
_HEADER = ("week,tow,sat,clk_m,median_m,dev_m,prev_clk_m,jump_m,abs_max_m,"
           "jump_max_m,flag_abs,flag_jump,flag_ext,refloated,nis,mode")


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


# --------------------------------------------------------------------------- #
#  Model mode (mode col present; per-row √S band; one over-k fire)
# --------------------------------------------------------------------------- #
def _write_model(tmp_path):
    rows = [_HEADER]
    k = 6.0
    for i in range(6):
        tow = 259200 + 30 * i
        for sat, base_nis in (("G10", 0.4), ("E15", 0.9), ("G13", 0.6)):
            # G13 at i==4 is the over-k fire.
            nis = 7.5 if (sat == "G13" and i == 4) else base_nis + 0.02 * i
            root_s = 0.010 + 0.001 * i          # per-row √S (VARIES -> model)
            dev = nis * root_s
            drift = 1e-4 * (1 + 0.01 * i)       # prev_clk_m = drift (m/s)
            clk = 0.5 + 0.01 * i
            pred = clk - dev                    # median_m = PRE-update prediction
            innov = dev                         # jump_m = raw innov
            flag_abs = 1 if abs(nis) > k else 0
            refloated = flag_abs
            rows.append(
                f"2313,{tow}.000,{sat},{clk:.4f},{pred:.4f},{dev:.4f},"
                f"{drift:.6f},{innov:.4f},{root_s:.4f},{k:.1f},"
                f"{flag_abs},0,0,{refloated},{nis:.4f},model")
    (tmp_path / "network_clock_monitor.csv").write_text(
        "\n".join(rows) + "\n", encoding="utf-8")


def test_model_mode(tmp_path):
    _write_model(tmp_path)
    df = load_trace(_ctx(tmp_path).path("network_clock_monitor.csv"))
    assert _resolve_mode(df) == "model"

    res = build_network_clock_nis(_ctx(tmp_path))
    # Fig1 NIS scatter + Fig2 worst-sats + at least one strip + Fig4 drift.
    assert len(res.figures) >= 3
    assert "model mode" in res.summary
    assert "|NIS| > k fires   : 1" in res.summary
    # The over-k fire is G13 and its TOW must be surfaced.
    assert "G13" in res.summary
    assert "259320.0" in res.summary  # tow for i==4


# --------------------------------------------------------------------------- #
#  Monitor mode (blank nis; ",," prev/jump on quiet rows)
# --------------------------------------------------------------------------- #
def _write_monitor(tmp_path):
    rows = [_HEADER]
    abs_max = 20.0   # CONSTANT bound -> monitor
    jump_max = 10.0
    for i in range(5):
        tow = 259200 + 30 * i
        for sat, dev in (("G10", 2.0 + i), ("E15", -3.0), ("G13", 25.0 if i == 3 else 5.0)):
            flag_abs = 1 if abs(dev) > abs_max else 0
            # First epoch of each sat has no previous adjacent value -> ",," blanks.
            if i == 0:
                prev, jump, jflag = "", "", 0
            else:
                jump = 1.0 + 0.5 * i
                prev = f"{dev - jump:.4f}"
                jump = f"{jump:.4f}"
                jflag = 0
            clk = dev + 100.0
            median = 100.0
            refloated = flag_abs
            rows.append(
                f"2313,{tow}.000,{sat},{clk:.4f},{median:.4f},{dev:.4f},"
                f"{prev},{jump},{abs_max:.2f},{jump_max:.2f},"
                f"{flag_abs},{jflag},0,{refloated},,monitor")
    (tmp_path / "network_clock_monitor.csv").write_text(
        "\n".join(rows) + "\n", encoding="utf-8")


def test_monitor_mode(tmp_path):
    _write_monitor(tmp_path)
    df = load_trace(_ctx(tmp_path).path("network_clock_monitor.csv"))
    assert _resolve_mode(df) == "monitor"

    res = build_network_clock_nis(_ctx(tmp_path))
    assert len(res.figures) == 1
    fig = res.figures[0]
    # dev axis always present; jump axis present because jump_m has finite values.
    assert len(fig.axes) == 2
    assert "clock-monitor" in res.summary.lower()
    # G13 @ i==3 (tow 259290) exceeds the 20 m absolute bound.
    assert "abs=1" in res.summary
    assert "259290.0" in res.summary


# --------------------------------------------------------------------------- #
#  Legacy file: no nis / mode columns -> routed by abs_max_m.nunique heuristic
# --------------------------------------------------------------------------- #
_LEGACY_HEADER = ("week,tow,sat,clk_m,median_m,dev_m,prev_clk_m,jump_m,"
                  "abs_max_m,jump_max_m,flag_abs,flag_jump,flag_ext,refloated")


def test_legacy_model_heuristic(tmp_path):
    """No mode col, abs_max_m VARIES -> model (per-row √S)."""
    rows = [_LEGACY_HEADER]
    k = 6.0
    for i in range(5):
        tow = 259200 + 30 * i
        for sat in ("G10", "G13"):
            root_s = 0.010 + 0.001 * i          # VARIES -> nunique>1 -> model
            nis = 7.2 if (sat == "G13" and i == 2) else 0.5 + 0.05 * i
            dev = nis * root_s
            clk = 0.5 + 0.01 * i
            pred = clk - dev
            flag_abs = 1 if abs(nis) > k else 0
            rows.append(
                f"2313,{tow}.000,{sat},{clk:.4f},{pred:.4f},{dev:.4f},"
                f"0.0001,{dev:.4f},{root_s:.4f},{k:.1f},"
                f"{flag_abs},0,0,{flag_abs}")
    (tmp_path / "network_clock_monitor.csv").write_text(
        "\n".join(rows) + "\n", encoding="utf-8")

    df = load_trace(_ctx(tmp_path).path("network_clock_monitor.csv"))
    assert "mode" not in df.columns
    assert _resolve_mode(df) == "model"

    res = build_network_clock_nis(_ctx(tmp_path))
    # |nis| recovered from dev_m/√S; the over-k fire (G13@i==2) surfaces.
    assert len(res.figures) >= 2
    assert "model mode" in res.summary
    assert "G13" in res.summary


def test_legacy_monitor_heuristic(tmp_path):
    """No mode col, abs_max_m CONSTANT -> monitor."""
    rows = [_LEGACY_HEADER]
    for i in range(4):
        tow = 259200 + 30 * i
        for sat, dev in (("G10", 2.0 + i), ("G13", 30.0 if i == 2 else 4.0)):
            flag_abs = 1 if abs(dev) > 20.0 else 0
            rows.append(
                f"2313,{tow}.000,{sat},{dev + 100:.4f},100.0000,{dev:.4f},"
                f",,20.00,10.00,{flag_abs},0,0,{flag_abs}")
    (tmp_path / "network_clock_monitor.csv").write_text(
        "\n".join(rows) + "\n", encoding="utf-8")

    df = load_trace(_ctx(tmp_path).path("network_clock_monitor.csv"))
    assert _resolve_mode(df) == "monitor"
    res = build_network_clock_nis(_ctx(tmp_path))
    assert len(res.figures) == 1
    assert "clock-monitor" in res.summary.lower()


# --------------------------------------------------------------------------- #
#  Mode-resolution edge cases
# --------------------------------------------------------------------------- #
def test_single_row_defaults_monitor(tmp_path):
    """Degenerate single-row legacy file (nunique==1) -> default monitor."""
    rows = [_LEGACY_HEADER,
            "2313,259200.000,G10,100.0,100.0,5.0,,,20.00,10.00,0,0,0,0"]
    (tmp_path / "network_clock_monitor.csv").write_text(
        "\n".join(rows) + "\n", encoding="utf-8")
    df = load_trace(_ctx(tmp_path).path("network_clock_monitor.csv"))
    assert _resolve_mode(df) == "monitor"
    res = build_network_clock_nis(_ctx(tmp_path))
    assert len(res.figures) == 1


def test_mode_col_wins_over_heuristic(tmp_path):
    """mode col says model even though abs_max_m is constant here."""
    rows = [_HEADER]
    for i in range(3):
        tow = 259200 + 30 * i
        rows.append(f"2313,{tow}.000,G10,0.5,0.5,0.01,0.0001,0.01,"
                    f"0.0100,6.0,0,0,0,0,1.0,model")   # abs_max_m constant
    (tmp_path / "network_clock_monitor.csv").write_text(
        "\n".join(rows) + "\n", encoding="utf-8")
    df = load_trace(_ctx(tmp_path).path("network_clock_monitor.csv"))
    assert _resolve_mode(df) == "model"


# --------------------------------------------------------------------------- #
#  Skip on empty / header-only
# --------------------------------------------------------------------------- #
def test_skip_empty(tmp_path):
    (tmp_path / "network_clock_monitor.csv").write_text(
        _HEADER + "\n", encoding="utf-8")
    with pytest.raises(ViewerSkip):
        build_network_clock_nis(_ctx(tmp_path))

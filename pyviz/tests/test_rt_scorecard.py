"""rt_scorecard: reference resolution, suffix-stable convergence, figures, banner.

Follows the suite convention of real files in a tmp dir (no mocks): a synthetic
``state_trace.csv`` whose error decays through the 1.0/0.5/0.2 m rungs but never
holds 0.1 m, plus a comment-bearing run config carrying the reference — the
``config/sbf_replay_covl145.json`` shape.
"""
import json
import os

import numpy as np
import pytest

from gnssviz import TraceContext
from gnssviz.geodesy import lla_to_ecef
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.rt_scorecard import (
    THRESHOLDS, build_rt_scorecard, compute_metrics, rt_banner_html,
    _suffix_converged_index)
from gnssviz import runconfig

_WEEK = 2420
_TOW0 = 108000.0
_LAT, _LON, _H = 44.567, -123.276, 100.0          # Corvallis-ish rooftop


def _write_state_trace(log_dir, n=400, hold_m=0.15):
    """Error |up| decays 5 m -> ``hold_m`` (never below), 1 Hz, with stds."""
    rx, ry, rz = lla_to_ecef(_LAT, _LON, _H)
    rows = ["week,tow_s,x_m,y_m,z_m,x_std_m,y_std_m,z_std_m,tropo_m,tropo_std_m"]
    up = np.array([0.0, 0.0, 1.0])
    # Up unit vector in ECEF at the reference:
    la, lo = np.radians(_LAT), np.radians(_LON)
    up_ecef = np.array([np.cos(la) * np.cos(lo), np.cos(la) * np.sin(lo), np.sin(la)])
    for i in range(n):
        err = max(5.0 * np.exp(-i / 30.0), hold_m)
        x, y, z = np.array([rx, ry, rz]) + err * up_ecef
        rows.append(f"{_WEEK},{_TOW0 + i:.3f},{x:.6f},{y:.6f},{z:.6f},"
                    f"0.05,0.05,0.08,0.10,0.02")
    with open(os.path.join(log_dir, "state_trace.csv"), "w", newline="\n") as fh:
        fh.write("\n".join(rows) + "\n")
    return (float(rx), float(ry), float(rz))


def _write_run_config(path, ref, app_mode="real_time"):
    with open(path, "w", newline="\n") as fh:
        fh.write("// jsonc: comments must be tolerated, incl. \"C://\" in strings\n"
                 "{\n"
                 f'  "app_mode": "{app_mode}",  /* block comment */\n'
                 '  "files": { "root": "C://GnssProcessor//data" },\n'
                 '  "receiver": { "reference_position_ecef": {\n'
                 f'    "x": {ref[0]!r}, "y": {ref[1]!r}, "z": {ref[2]!r} }}\n'
                 "  }\n"
                 "}\n")


def test_suffix_converged_index_semantics():
    # A dip below thr that later diverges must NOT count as converged.
    e = np.array([5.0, 0.3, 0.3, 2.0, 0.4, 0.4, 0.4])
    assert _suffix_converged_index(e, 0.5) == 4
    assert _suffix_converged_index(e, 0.2) is None
    # Non-finite samples break the hold.
    e2 = np.array([0.1, np.nan, 0.1, 0.1])
    assert _suffix_converged_index(e2, 0.5) == 2
    assert _suffix_converged_index(np.array([]), 0.5) is None


def test_runconfig_jsonc_and_ref(tmp_path):
    p = str(tmp_path / "cfg.json")
    _write_run_config(p, (1.0, 2.0, 3.0))
    cfg = runconfig.load_run_config(p)
    assert runconfig.is_realtime_config(cfg)
    assert runconfig.ref_ecef_from_config(cfg) == (1.0, 2.0, 3.0)
    # Absent / partial / all-zero blocks resolve to None.
    assert runconfig.ref_ecef_from_config({}) is None
    assert runconfig.ref_ecef_from_config(
        {"receiver": {"reference_position_ecef": {"x": 1.0, "y": 2.0}}}) is None
    assert runconfig.ref_ecef_from_config(
        {"receiver": {"reference_position_ecef": {"x": 0, "y": 0, "z": 0}}}) is None
    assert runconfig.load_run_config(str(tmp_path / "missing.json")) is None


def test_scorecard_metrics_and_figures(tmp_path):
    logs = tmp_path / "logs"
    logs.mkdir()
    ref = _write_state_trace(str(logs))
    cfgp = str(tmp_path / "run.json")
    _write_run_config(cfgp, ref)

    ctx = TraceContext(str(logs), run_config=cfgp)
    m = compute_metrics(ctx)
    assert m is not None and m["n_epochs"] == 400
    # Ladder: monotonic (finer rung never earlier), 0.1 m never held.
    assert m["conv"][1.0] is not None and m["conv"][0.2] is not None
    assert m["conv"][0.1] is None
    idx = [m["conv"][t] for t in THRESHOLDS if m["conv"][t] is not None]
    assert idx == sorted(idx)
    # Pure-up error: steady-state U ~ +hold, E/N ~ 0, H << U.  The window opens
    # at the ≤1.0 m rung so the mean carries the 1.0→0.15 decay tail by design;
    # the MEDIAN sits on the floor.
    st = m["stats"]
    assert st["U"]["p50"] == pytest.approx(0.15, abs=0.02)
    assert st["U"]["mean"] > 0.15 and st["U"]["mean"] < 0.30
    assert st["H"]["rms"] < 0.02
    assert m["rate_s"] == pytest.approx(1.0)

    res = build_rt_scorecard(ctx)
    assert len(res.figures) >= 3               # error, convergence, scatter, CDF
    assert "Convergence" in res.summary and "—" in res.summary  # 0.1 m: never held
    banner = rt_banner_html(ctx)
    assert banner and "banner" in banner and "epochs" in banner

    # Explicit --ref-ecef wins over the config block.
    ctx2 = TraceContext(str(logs), run_config=cfgp,
                        ref_ecef=(ref[0] + 100.0, ref[1], ref[2]))
    m2 = compute_metrics(ctx2)
    assert m2["ref_src"] == "--ref-ecef"
    assert m2["stats"]["H"]["rms"] > 50.0      # graded against the shifted point


def test_scorecard_skips_without_reference(tmp_path):
    logs = tmp_path / "logs"
    logs.mkdir()
    _write_state_trace(str(logs), n=20)
    ctx = TraceContext(str(logs))               # no run_config, no ref_ecef
    assert compute_metrics(ctx) is None
    assert rt_banner_html(ctx) == ""            # non-RT reports unchanged
    with pytest.raises(ViewerSkip):
        build_rt_scorecard(ctx)


def test_report_title_and_banner_plumbing(tmp_path):
    """run_report.py end-to-end: RT title + banner land in the written HTML."""
    import run_report
    logs = tmp_path / "logs"
    logs.mkdir()
    ref = _write_state_trace(str(logs), n=60)
    cfgp = str(tmp_path / "run.json")
    _write_run_config(cfgp, ref)
    out = str(tmp_path / "r.html")
    rc = run_report.main([str(logs), "--run-config", cfgp, "-o", out,
                          "--only", "rt_scorecard"])
    assert rc == 0 and os.path.exists(out)
    with open(out, encoding="utf-8") as fh:
        html = fh.read()
    assert "GnssProcessor real-time run report" in html
    assert "RUN vs SURVEYED REFERENCE" in html

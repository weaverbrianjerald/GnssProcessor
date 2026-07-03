"""Build-time tests for the five network viewers (synthetic trace CSVs).

Exercises each viewer's ``build`` against stub CSVs -- enough to catch schema /
contract regressions -- plus the :class:`ViewerSkip` paths.  Figures are *built*
(spec objects), not rendered, so these stay fast and backend-free.
"""
import pytest

from gnssviz.trace_paths import TraceContext
from gnssviz.viewers.base import ViewerSkip
from gnssviz.viewers.network_closed_loop import build_network_closed_loop
from gnssviz.viewers.network_health import build_network_health
from gnssviz.viewers.network_iono_map import build_network_iono_map
from gnssviz.viewers.network_map import build_network_map
from gnssviz.viewers.network_products import build_network_products


def _ctx(tmp_path):
    return TraceContext(log_dir=str(tmp_path))


def _roster(tmp_path):
    (tmp_path / "network_stations.csv").write_text(
        "station,role,ecef_x,ecef_y,ecef_z\n"
        "JPLM00USA,datum,-2493304.99,-4655214.79,3565497.67\n"
        "GCGO00USA,network,-2281621.75,-1453585.08,5756964.89\n"
        "CIT100USA,held_out,-2491490.72,-4660802.85,3559129.19\n", encoding="utf-8")


def test_network_map_from_roster(tmp_path):
    _roster(tmp_path)
    res = build_network_map(_ctx(tmp_path))
    assert len(res.figures) == 1
    assert res.figures[0].axes[0].projection == "geo"
    assert "datum" in res.summary.lower()


def test_network_map_skip_without_data(tmp_path):
    with pytest.raises(ViewerSkip):
        build_network_map(_ctx(tmp_path))


def test_network_health(tmp_path):
    body = "".join(f"2313,{259200 + 30 * k}.000,3,18,60,2,1,1,"
                   f"{1.1 + 0.01 * k:.4f},2.3,60\n" for k in range(5))
    (tmp_path / "network_trace.csv").write_text(
        "week,tow,n_present,n_clock_sats,n_phase_bias,declared_dim,gate_ok,"
        "solved,rms_clock_m,max_clock_m,n_pivot_pins\n" + body, encoding="utf-8")
    res = build_network_health(_ctx(tmp_path))
    assert len(res.figures) == 1 and len(res.figures[0].axes) == 3
    assert "100.0%" in res.summary


def test_network_products(tmp_path):
    rows = ["week,tow,sat,kind,signal,value_m,disc_counter"]
    for k in range(4):
        tow = 259200 + 30 * k
        rows.append(f"2313,{tow}.000,G10,CLK,,{0.4 + 0.01 * k:.4f},0")
        rows.append(f"2313,{tow}.000,E15,CLK,,0.9000,0")
        rows.append(f"2313,{tow}.000,G10,PB,L1C,75.5000,{1 if k > 1 else 0}")
    (tmp_path / "network_solution.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    res = build_network_products(_ctx(tmp_path))
    assert len(res.figures) >= 1


def test_network_products_appends_contract_check(tmp_path):
    # L14 hook: network_contract_check.csv (when present) is appended to the summary.
    rows = ["week,tow,sat,kind,signal,value_m,disc_counter",
            "2313,259200.000,G10,CLK,,0.4000,0"]
    (tmp_path / "network_solution.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    (tmp_path / "network_contract_check.csv").write_text(
        "check,status,detail\n"
        "file_readable,PASS,\n"
        "overall,PASS,1 realized pivot(s) serialized\n", encoding="utf-8")
    res = build_network_products(_ctx(tmp_path))
    assert "S-basis contract check" in res.summary
    assert "overall" in res.summary and "PASS" in res.summary


def test_network_iono_map(tmp_path):
    _roster(tmp_path)
    rows = ["week,tow,station,sat,iono_m,elevation_deg,azimuth_deg"]
    for k in range(3):
        tow = 259200 + 30 * k
        for sat, el, az in (("G10", 40, 120), ("E15", 30, 300), ("G13", 60, 200)):
            rows.append(f"2313,{tow}.000,JPLM00USA,{sat},{1.0 + 0.1 * k:.4f},{el},{az}")
            rows.append(f"2313,{tow}.000,GCGO00USA,{sat},1.2000,{el},{az}")
    (tmp_path / "network_ionosphere.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    ctx = _ctx(tmp_path)
    ctx.find_ionex = lambda: None    # hermetic: no IONEX backdrop
    res = build_network_iono_map(ctx)
    assert len(res.figures) >= 2
    assert len(res.animations) == 1 and res.animations[0].n_frames >= 2


def test_network_iono_map_skip_without_roster(tmp_path):
    (tmp_path / "network_ionosphere.csv").write_text(
        "week,tow,station,sat,iono_m,elevation_deg,azimuth_deg\n"
        "2313,259200.000,JPLM00USA,G10,1.0,40,120\n", encoding="utf-8")
    ctx = _ctx(tmp_path)
    ctx.find_ionex = lambda: None
    with pytest.raises(ViewerSkip):
        build_network_iono_map(ctx)


def test_network_closed_loop(tmp_path):
    rows = ["week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,ratio,"
            "pos_err_3d_m,east_m,north_m,up_m"]
    for k in range(6):
        rows.append(f"2313,{259200 + 30 * k}.000,10,{1 if k > 1 else 0},"
                    f"{1 if k > 3 else 0},{8 if k > 3 else 0},10,"
                    f"{2.5 if k > 1 else 0.0},{0.5 / (k + 1):.4f},0.01,0.01,0.02")
    (tmp_path / "closed_loop_trace.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    res = build_network_closed_loop(_ctx(tmp_path))
    assert len(res.figures) == 1 and len(res.figures[0].axes) == 3

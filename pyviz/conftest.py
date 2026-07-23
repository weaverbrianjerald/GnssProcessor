"""Make the bundled ``gnssviz`` package importable when running pytest from here.

Also provides the ``exam_fixture`` factory: it writes a tiny but schema-faithful
GnssSimulator answer-key ``truth/`` dir + processor ``logs/`` dir into a tmp path
so the ``exam_*`` viewer tests run against real files (not mocks).  Knobs omit
individual files/columns to exercise the graceful-degradation paths.
"""
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

_WEEK = 2313
_TOW0 = 302400.0
_STEP = 30.0
_SATS = ["G05", "G12", "E10"]          # 2 GPS + 1 Galileo
# A base ECEF near Fiji (the scenario origin) + tiny per-epoch drift.
_X0, _Y0, _Z0 = -6047592.0, -190053.0, -2010942.0


def _write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


@pytest.fixture
def exam_fixture(tmp_path):
    """Return ``build(**opts) -> (truth_dir, log_dir)`` writing fixture CSVs.

    Options (all default True unless noted): ``kinematic``, ``with_verify``,
    ``with_source`` (truth_slips source column), ``slips``, ``atmo``,
    ``visibility``, ``clock``, ``galileo`` (Galileo_m in state_trace + isb),
    ``scenario`` (scenario_summary.json), ``storm`` (storm window scalars),
    ``residuals`` (measurement_residuals.csv), ``n`` (epochs, default 6).
    """
    def build(*, kinematic=True, with_verify=True, with_source=True, slips=True,
              atmo=True, visibility=True, clock=True, galileo=True, scenario=True,
              storm=True, residuals=True, n=6):
        truth = tmp_path / "truth"
        logs = tmp_path / "logs"
        truth.mkdir(exist_ok=True)
        logs.mkdir(exist_ok=True)
        tows = [_TOW0 + i * _STEP for i in range(n)]
        # Storm covers the middle third.
        s0, s1 = tows[n // 3], tows[2 * n // 3]

        # --- truth_position.csv ---
        pos = ["week,tow_s,x_m,y_m,z_m,lat_deg,lon_deg,h_m,clock_bias_s,heading_deg,speed_mps"]
        for i, tw in enumerate(tows):
            dx = (i * 3.0 if kinematic else 0.0)
            lat = -18.5 + (i * 1e-4 if kinematic else 0.0)
            pos.append(f"{_WEEK},{tw:.3f},{_X0+dx:.4f},{_Y0:.4f},{_Z0:.4f},"
                       f"{lat:.7f},-178.2000000,0.0000,0.0001000000,35.00,"
                       f"{4.5 if kinematic else 0.0:.2f}")
        _write(truth / "truth_position.csv", "\n".join(pos) + "\n")

        # --- truth_scalars.csv ---
        sc = ["key,value", f"kinematic,{1 if kinematic else 0}"]
        if galileo:
            sc.append("isb_gps_gal_m,8.000000")
        sc.append("zwd_m,0.150000")
        if storm:
            sc += [f"storm_window_start_tow_s,{s0:.3f}",
                   f"storm_window_end_tow_s,{s1:.3f}", "peak_dI_L1_m,6.1175"]
        sc += ["n_cycle_slips,3", "n_cycle_slips_flagged,1",
               f"week,{_WEEK}", f"first_tow_s,{_TOW0:.3f}",
               f"interval_s,{_STEP:.3f}", f"n_epochs,{n}"]
        _write(truth / "truth_scalars.csv", "\n".join(sc) + "\n")

        # --- truth_clock.csv ---
        if clock:
            ck = ["week,tow_s,clock_bias_s,clock_bias_m,isb_gps_gal_m"]
            for tw in tows:
                ck.append(f"{_WEEK},{tw:.3f},0.000100000000,29979.2458,8.0000")
            _write(truth / "truth_clock.csv", "\n".join(ck) + "\n")

        # --- truth_visibility.csv ---
        if visibility:
            vis = ["week,tow_s,n_sats,n_gps,n_gal,gdop,pdop,hdop,vdop,sat_list"]
            for tw in tows:
                vis.append(f"{_WEEK},{tw:.3f},16,10,6,1.415,1.259,0.635,1.087,"
                           "G05 G12 E10")
            _write(truth / "truth_visibility.csv", "\n".join(vis) + "\n")

        # --- truth_atmo.csv (per sat x epoch) ---
        if atmo:
            at = ["week,tow_s,satellite,elev_deg,az_deg,ipp_lat_deg,ipp_lon_deg,"
                  "stec_quiet_tecu,dstec_sed_tecu,dstec_tid_tecu,stec_total_tecu,"
                  "I_L1_m,dI_L1_m,in_storm_window"]
            for i, tw in enumerate(tows):
                in_storm = 1 if s0 <= tw <= s1 else 0
                for j, s in enumerate(_SATS):
                    el = 20.0 + 10 * j + i
                    az = 30.0 + 40 * j + 5 * i
                    di = (2.0 + j) * in_storm
                    at.append(f"{_WEEK},{tw:.3f},{s},{el:.4f},{az:.4f},"
                              f"-11.6,179.3,20.5,0.0,{di:.4f},20.5,3.3,{di:.4f},{in_storm}")
            _write(truth / "truth_atmo.csv", "\n".join(at) + "\n")

        # --- truth_slips.csv (flagged / unflagged / scripted classes) ---
        if slips:
            hdr = "week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg"
            rows = [
                (tows[1], "G12", "L2W", -17, 0),   # unflagged
                (tows[2], "E10", "L5Q", -40, 1),   # flagged
                (tows[3], "G05", "L1C", 9, 0),     # scripted (with source variant)
            ]
            if with_source:
                hdr += ",source"
            lines = [hdr]
            srcs = ["random", "random", "scripted"]
            for k, (tw, sat, sig, dcy, flg) in enumerate(rows):
                base = f"{_WEEK},{tw:.3f},{sat},{sig},{dcy},{flg},25.00"
                lines.append(base + (f",{srcs[k]}" if with_source else ""))
            _write(truth / "truth_slips.csv", "\n".join(lines) + "\n")

        # --- scenario_summary.json ---
        if scenario:
            summary = {
                "scenario": "SAR vessel adrift, South Pacific near Fiji",
                "date": "2024-129 (2024-05-08)", "station": "SARV",
                "kinematic": bool(kinematic), "week": _WEEK, "first_tow_s": _TOW0,
                "interval_s": _STEP, "n_epochs": n,
                "signals": {"GPS": ["C1C", "L1C", "C2W", "L2W"],
                            "Galileo": ["C1C", "L1C", "C5Q", "L5Q"]},
                "truth": {"gps_galileo_isb_m": 8.0 if galileo else 0.0, "zwd_m": 0.15,
                          "final_position_ecef_m": [_X0, _Y0, _Z0]},
                "noise": {"seed": 20240315},
            }
            if storm:
                summary["storm"] = {"window_tow_s": [s0, s1],
                                    "peak_slant_dI_L1_m": 6.1175,
                                    "peak_sat": "E10", "peak_tow_s": tows[n // 3]}
            _write(truth / "scenario_summary.json", json.dumps(summary, indent=2))

        # --- state_trace.csv (processor solution) ---
        cols = ["week", "tow_s", "x_m", "y_m", "z_m", "x_std_m", "y_std_m",
                "z_std_m", "tropo_m", "tropo_std_m", "clock_m", "clock_std_m"]
        if galileo:
            cols += ["Galileo_m", "Galileo_std_m"]
        st = [",".join(cols)]
        for i, tw in enumerate(tows):
            dx = (i * 3.0 if kinematic else 0.0)
            row = [str(_WEEK), f"{tw:.3f}",
                   f"{_X0+dx+0.05:.6f}", f"{_Y0-0.03:.6f}", f"{_Z0+0.04:.6f}",
                   "0.030000", "0.020000", "0.025000",
                   "0.148000", "0.010000", "29979.30", "0.5"]
            if galileo:
                row += ["7.95", "0.06"]
            st.append(",".join(row))
        _write(logs / "state_trace.csv", "\n".join(st) + "\n")

        # --- measurement_residuals.csv ---
        if residuals:
            mr = ["week,tow_s,satellite,signal,innovation_m,residual_m,"
                  "residual_std_m,standardized_residual,redundancy,w_test,mdb_m"]
            for tw in tows:
                for s in _SATS:
                    mr.append(f"{_WEEK},{tw:.3f},{s},C1C_C2W,0.5,0.01,2.5,0.4,nan,nan,nan")
                    mr.append(f"{_WEEK},{tw:.3f},{s},L1C_L2W,0.0,0.001,0.02,1.2,nan,nan,nan")
            _write(logs / "measurement_residuals.csv", "\n".join(mr) + "\n")

        # --- verify_report.json ---
        if with_verify:
            rep = {
                "pass": True, "failures": 0, "k_sigma": 3, "converge_skip": 1,
                "checks": [
                    {"id": "position_median", "pass": True, "value": 0.07,
                     "gate": 0.35, "units": "m", "detail": "kinematic"},
                    {"id": "isb", "pass": True, "value": 0.05, "gate": 0.10,
                     "units": "m", "detail": "recovered 7.95 vs truth 8.000"},
                    {"id": "zwd", "pass": True, "value": 0.002, "gate": 0.06,
                     "units": "m", "detail": "recovered 0.148 vs truth 0.150"},
                ],
                "report_lines": ["=== GnssSimulator Verifier ===", "=== VERIFIER PASS ==="],
            }
            _write(logs / "verify_report.json", json.dumps(rep, indent=2))

        return str(truth), str(logs)

    return build

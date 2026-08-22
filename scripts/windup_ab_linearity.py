#!/usr/bin/env python3
"""windup_ab_linearity.py -- decide the carrier-phase wind-up SIGN defect on real data.

docs/SPEC_phase_windup_sign.md section 8 item 1.  The simulator is structurally blind
to the defect (it injects wind-up through the same computePhaseWindUp), so the only
decisive evidence is a real-data three-arm ladder:

    arm1  wind-up ON, shipped exe   -> phase residual  2W + N   (W = wrapped wind-up,
    arm2  wind-up OFF               -> phase residual   W + N        N = the fractional-
    arm3  wind-up ON, FIXED exe     -> phase residual       N         mode wrap staircase)

Because the arms differ only by an ADDITIVE per-observation term, the estimator is
linear in it to second order, so two identities must hold:

    (1)  second difference   arm1 - 2*arm2 + arm3 == 0   (exact, both accumulation modes)
    (2)  ladder slope        (arm1 - arm3) == 2 * (arm2 - arm3)

Under the NULL hypothesis (the shipped sign is already correct) arm3 IS arm1, so the
slope is 0 and the second difference equals -(arm2 - arm1).  Slope 2 vs slope 0 is the
discriminator, and it needs no truth coordinate -- only that the three runs are
deterministic.  The truth-graded scorecard printed alongside is the PRICE, not the proof.

Usage
-----
    ...python.exe scripts/windup_ab_linearity.py
        --shipped test_suite_results/windup/A_shipped
        --fixed   test_suite_results/windup/B_fixed
        --station JPLM --on a_wu_on_frac --off b_wu_off_frac
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "pyviz"))

from gnssviz.eval import (CompositeTruth, SifTruth, SinexTruth,   # noqa: E402
                          doy_to_decimal_year, find_sinex_files)
from gnssviz.geodesy import ecef_to_enu, ecef_to_lla                # noqa: E402
from gnssviz.io import load_trace                                    # noqa: E402

KEY = ["week", "tow_s"]


def enu(run_dir: Path, truth: np.ndarray) -> pd.DataFrame:
    df = load_trace(str(run_dir / "logs" / "state_trace.csv"))
    lat0, lon0, h0 = (float(np.ravel(v)[0]) for v in ecef_to_lla(*truth))
    e, n, u = ecef_to_enu(df["x_m"].to_numpy(float), df["y_m"].to_numpy(float),
                          df["z_m"].to_numpy(float), lat0, lon0, h0)
    return pd.DataFrame({"week": df["week"].astype(int), "tow_s": df["tow_s"].astype(float),
                         "e": np.asarray(e, float), "n": np.asarray(n, float),
                         "u": np.asarray(u, float)})


def resid_rms(run_dir: Path) -> dict:
    """Phase-residual RMS with the per-(satellite, signal) mean removed -- the part no
    constant float ambiguity can absorb.  The sharpest observable in the ladder."""
    p = run_dir / "logs" / "measurement_residuals.csv"
    if not p.is_file():
        return {}
    d = pd.read_csv(p)
    sig = d["signal"].astype(str).str.upper()
    ph = d[sig.str.startswith("L")]
    if ph.empty:
        ph = d
    g = ph.groupby(["satellite", "signal"])["residual_m"]
    dm = (ph["residual_m"] - g.transform("mean")).to_numpy(float)
    dm = dm[np.isfinite(dm)]
    return {"n": int(dm.size), "rms_mm": float(np.sqrt(np.mean(dm ** 2)) * 1000),
            "p95_mm": float(np.percentile(np.abs(dm), 95) * 1000)}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--shipped", type=Path, required=True,
                    help="output_root of the shipped-exe pass")
    ap.add_argument("--fixed", type=Path, required=True,
                    help="output_root of the sign-corrected-exe pass")
    ap.add_argument("--station", default="JPLM")
    ap.add_argument("--truth-key", default=None)
    ap.add_argument("--on", default="a_wu_on_frac", help="the wind-up-ON cell label")
    ap.add_argument("--off", default="b_wu_off_frac", help="the wind-up-OFF cell label")
    ap.add_argument("--year", type=int, default=2024)
    ap.add_argument("--doy", type=int, default=129)
    ap.add_argument("--warmup-s", type=float, default=1800.0)
    args = ap.parse_args(argv)
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    ep = doy_to_decimal_year(args.year, args.doy)
    key = args.truth_key or (args.station.upper() + "00USA")
    srcs = [SinexTruth(p) for p in find_sinex_files(REPO / "data" / "static" / "sinex")]
    sif = [REPO / "data/static/sif/igs20_SIF.json", REPO / "data/static/sif/ncn20_SIF.json"]
    srcs = [s for s in srcs if s.stations()] + [SifTruth([p for p in sif if p.is_file()])]
    tc = CompositeTruth(srcs).resolve(key, ep)
    if tc is None:
        print("error: no truth for " + key, file=sys.stderr)
        return 2
    print("== wind-up sign ladder ==  %s (%s)  %d-%03d  truth %s [%s]"
          % (args.station, key, args.year, args.doy, tc.source, tc.frame))

    dirs = {"arm1_ON_shipped": args.shipped / args.station / args.on,
            "arm2_OFF": args.shipped / args.station / args.off,
            "arm3_ON_fixed": args.fixed / args.station / args.on,
            "null_OFF_fixed": args.fixed / args.station / args.off}
    for k, d in dirs.items():
        if not (d / "logs" / "state_trace.csv").is_file():
            print("error: missing %s" % (d / "logs" / "state_trace.csv"), file=sys.stderr)
            return 2

    # ---- control 0: wind-up OFF must be BYTE-IDENTICAL across the two exes ----
    a = (dirs["arm2_OFF"] / "logs" / "state_trace.csv").read_bytes()
    b = (dirs["null_OFF_fixed"] / "logs" / "state_trace.csv").read_bytes()
    print("\n[control] wind-up-OFF state_trace, shipped vs fixed exe: %s"
          % ("BYTE-IDENTICAL (the two exes differ only in the wind-up path)" if a == b
             else "*** DIFFERS -- a second variable is loose ***"))

    # ---- align the three arms epoch-by-epoch ----
    t = {k: enu(d, tc.ecef) for k, d in dirs.items() if k != "null_OFF_fixed"}
    third = t["arm3_ON_fixed"].rename(columns={"e": "e_3", "n": "n_3", "u": "u_3"})
    m = t["arm1_ON_shipped"].merge(t["arm2_OFF"], on=KEY, suffixes=("_1", "_2")) \
                            .merge(third, on=KEY)
    print("[align] %d common epochs (%d/%d/%d per arm)"
          % (len(m), len(t["arm1_ON_shipped"]), len(t["arm2_OFF"]), len(t["arm3_ON_fixed"])))
    if len(m) < 100:
        print("error: too few common epochs", file=sys.stderr)
        return 2

    print("\n%-4s %14s %14s %12s %14s %14s"
          % ("cmp", "slope(pred 2)", "R^2(pred 1)", "|S| RMS mm", "max|2nd diff|mm", "median|S| mm"))
    slopes = []
    for c in "enu":
        S = (m[c + "_2"] - m[c + "_3"]).to_numpy()      # one unit of wind-up
        D = (m[c + "_1"] - m[c + "_3"]).to_numpy()      # two units
        sec = (m[c + "_1"] - 2 * m[c + "_2"] + m[c + "_3"]).to_numpy()
        den = float(S @ S)
        slope = float(D @ S) / den if den > 0 else float("nan")
        dv = float(np.sum((D - D.mean()) ** 2))
        r2 = 1.0 - float(np.sum((D - slope * S) ** 2)) / dv if dv > 0 else float("nan")
        print("%-4s %14.6f %14.6f %12.4f %14.4e %14.4f"
              % (c.upper(), slope, r2, np.sqrt(np.mean(S ** 2)) * 1000,
                 np.max(np.abs(sec)) * 1000, np.median(np.abs(S)) * 1000))
        slopes.append(slope)

    print("\nVERDICT  slope ~= 2.000, R^2 ~= 1, second difference at the numerical floor")
    print("         => the shipped model applies EXACTLY the negative of the fixed one")
    print("            (SPEC section 1 confirmed inside the solve, on real data).")
    print("         slope ~= 0 => the shipped sign was already right and the 'fix' is the defect.")
    print("         measured slopes E/N/U: %.4f / %.4f / %.4f" % tuple(slopes))

    # ---- truth-graded price over the converged window ----
    el = (m["tow_s"] - m["tow_s"].iloc[0]).to_numpy()
    el = np.where(el < 0, el + 604800.0, el)
    conv = el >= args.warmup_s
    print("\n%-16s | %-32s | %-10s | %s"
          % ("arm", "ENU bias vs truth (mm)", "3D-RMS mm", "phase-residual RMS mm (arc-demeaned)"))
    bias = {}
    for k, sfx in (("arm1_ON_shipped", "_1"), ("arm2_OFF", "_2"), ("arm3_ON_fixed", "_3")):
        e, n, u = (m[c + sfx].to_numpy()[conv] for c in "enu")
        bias[k] = np.array([e.mean(), n.mean(), u.mean()])
        r3 = np.sqrt(np.mean(e ** 2 + n ** 2 + u ** 2))
        rr = resid_rms(dirs[k])
        print("%-16s | E%+9.2f N%+9.2f U%+9.2f  | %10.2f | %s"
              % (k, bias[k][0] * 1000, bias[k][1] * 1000, bias[k][2] * 1000, r3 * 1000,
                 ("%.3f (p95 %.3f, n=%d)" % (rr["rms_mm"], rr["p95_mm"], rr["n"])) if rr else "n/a"))
    d12 = (bias["arm1_ON_shipped"] - bias["arm2_OFF"]) * 1000
    d23 = (bias["arm2_OFF"] - bias["arm3_ON_fixed"]) * 1000
    print("\n[price] arm1-arm2 = E%+.2f N%+.2f U%+.2f mm (|3D| %.2f)"
          % (d12[0], d12[1], d12[2], float(np.linalg.norm(d12))))
    print("[price] arm2-arm3 = E%+.2f N%+.2f U%+.2f mm (|3D| %.2f)   "
          "(these two must be EQUAL under the sign hypothesis)"
          % (d23[0], d23[1], d23[2], float(np.linalg.norm(d23))))
    print("[price] |ENU error vs truth|: arm1 %.2f  arm2 %.2f  arm3 %.2f mm  "
          "(the sign hypothesis predicts arm3 < arm2 < arm1)"
          % tuple(float(np.linalg.norm(bias[k])) * 1000 for k in
                  ("arm1_ON_shipped", "arm2_OFF", "arm3_ON_fixed")))
    return 0


if __name__ == "__main__":
    sys.exit(main())

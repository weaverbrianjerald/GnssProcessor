#!/usr/bin/env python3
"""compare_ppp_runs.py: put two single-receiver PPP runs side by side.

Reads ``state_trace.csv`` from two ``logs`` folders (run A = baseline, run B =
the candidate change) and prints, for each run:

  * East / North / Up RMS about a common reference position,
  * the convergence time: first time after which the 3-D error stays below a
    threshold (default 10 cm) for the rest of the run,
  * the mean formal 3-D sigma the filter reports.

The reference position is, in order of preference, ``--ref X Y Z`` (ECEF metres,
e.g. from an IGS SINEX) or the median of run A over its last ``--tail-hours``
hours.  With the second choice the numbers measure precision and convergence, not
absolute accuracy, and should be reported as such.

Usage (from the repository root)::

    python scripts/compare_ppp_runs.py logs_elev logs_floor
    python scripts/compare_ppp_runs.py logs_elev logs_floor --label-a elevation --label-b floor
    python scripts/compare_ppp_runs.py A B --ref -2281621.5 -1453585.0 5756964.9

Used by docs/onboarding/03_worked_example.md, stage 8.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd


def load(run_dir: str) -> pd.DataFrame:
    path = Path(run_dir)
    if path.is_dir():
        path = path / "state_trace.csv"
    if not path.exists():
        sys.exit(f"not found: {path}  (is logging.enable_state_trace on?)")
    df = pd.read_csv(path)
    df["t_h"] = ((df["week"] - df["week"].iloc[0]) * 604800.0
                 + (df["tow_s"] - df["tow_s"].iloc[0])) / 3600.0
    return df


def ecef_to_enu_matrix(ref: np.ndarray) -> np.ndarray:
    """Rotation ECEF -> local East/North/Up at @p ref (spherical latitude is
    accurate to ~0.2 deg, which is ample for rotating centimetre residuals)."""
    x, y, z = ref
    lon = np.arctan2(y, x)
    lat = np.arctan2(z, np.hypot(x, y))
    sl, cl, sp, cp = np.sin(lon), np.cos(lon), np.sin(lat), np.cos(lat)
    return np.array([[-sl, cl, 0.0],
                     [-sp * cl, -sp * sl, cp],
                     [cp * cl, cp * sl, sp]])


def summarize(df: pd.DataFrame, ref: np.ndarray, rot: np.ndarray,
              threshold_m: float, skip_hours: float) -> dict:
    enu = (df[["x_m", "y_m", "z_m"]].to_numpy() - ref) @ rot.T
    err3d = np.linalg.norm(enu, axis=1)

    # Convergence: first epoch from which the 3-D error never again exceeds the threshold.
    above = np.nonzero(err3d > threshold_m)[0]
    if above.size == 0:
        conv_h = 0.0
    elif above[-1] + 1 >= len(df):
        conv_h = float("nan")
    else:
        conv_h = float(df["t_h"].iloc[above[-1] + 1])

    settled = df["t_h"].to_numpy() >= skip_hours
    rms = np.sqrt(np.mean(enu[settled] ** 2, axis=0))
    sigma3d = np.sqrt(df["x_std_m"] ** 2 + df["y_std_m"] ** 2 + df["z_std_m"] ** 2)
    return {
        "epochs": len(df),
        "conv_h": conv_h,
        "rms_e_cm": 100 * rms[0],
        "rms_n_cm": 100 * rms[1],
        "rms_u_cm": 100 * rms[2],
        "rms_3d_cm": 100 * float(np.sqrt(np.sum(rms ** 2))),
        "sigma_cm": 100 * float(sigma3d[settled].mean()),
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_a", help="baseline run: a logs folder or a state_trace.csv")
    ap.add_argument("run_b", help="changed run")
    ap.add_argument("--label-a", default="A")
    ap.add_argument("--label-b", default="B")
    ap.add_argument("--ref", nargs=3, type=float, metavar=("X", "Y", "Z"),
                    help="reference ECEF position in metres")
    ap.add_argument("--tail-hours", type=float, default=6.0,
                    help="without --ref: reference = median of run A's last N hours (default 6)")
    ap.add_argument("--threshold-cm", type=float, default=10.0,
                    help="3-D convergence threshold (default 10 cm)")
    ap.add_argument("--skip-hours", type=float, default=2.0,
                    help="RMS and sigma ignore the first N hours (default 2)")
    args = ap.parse_args(argv)

    a, b = load(args.run_a), load(args.run_b)
    if args.ref:
        ref = np.array(args.ref)
        ref_desc = "--ref (absolute accuracy)"
    else:
        tail = a[a["t_h"] >= a["t_h"].iloc[-1] - args.tail_hours]
        ref = tail[["x_m", "y_m", "z_m"]].median().to_numpy()
        ref_desc = (f"median of run A's last {args.tail_hours:g} h "
                    "(precision / convergence only, NOT absolute accuracy)")
    rot = ecef_to_enu_matrix(ref)

    rows = {args.label_a: summarize(a, ref, rot, args.threshold_cm / 100, args.skip_hours),
            args.label_b: summarize(b, ref, rot, args.threshold_cm / 100, args.skip_hours)}

    print(f"reference : {ref_desc}")
    print(f"            X={ref[0]:.4f}  Y={ref[1]:.4f}  Z={ref[2]:.4f}")
    print(f"RMS/sigma : epochs after the first {args.skip_hours:g} h")
    print()
    head = f"{'run':<14}{'epochs':>7}{'conv<' + format(args.threshold_cm, 'g') + 'cm [h]':>16}" \
           f"{'E [cm]':>9}{'N [cm]':>9}{'U [cm]':>9}{'3D [cm]':>9}{'sigma3D [cm]':>14}"
    print(head)
    print("-" * len(head))
    for label, r in rows.items():
        print(f"{label:<14}{r['epochs']:>7}{r['conv_h']:>16.2f}{r['rms_e_cm']:>9.2f}"
              f"{r['rms_n_cm']:>9.2f}{r['rms_u_cm']:>9.2f}{r['rms_3d_cm']:>9.2f}{r['sigma_cm']:>14.2f}")

    # Direct difference between the two solutions, epoch-matched.
    m = a.merge(b, on=["week", "tow_s"], suffixes=("_a", "_b"))
    d = (m[["x_m_b", "y_m_b", "z_m_b"]].to_numpy() - m[["x_m_a", "y_m_a", "z_m_a"]].to_numpy()) @ rot.T
    settled = m["t_h_a"].to_numpy() >= args.skip_hours
    print()
    print(f"B - A, {settled.sum()} common epochs after {args.skip_hours:g} h:  "
          f"mean E/N/U = {100 * d[settled, 0].mean():+.2f} / {100 * d[settled, 1].mean():+.2f} / "
          f"{100 * d[settled, 2].mean():+.2f} cm,  max |3D| = "
          f"{100 * np.linalg.norm(d[settled], axis=1).max():.2f} cm")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

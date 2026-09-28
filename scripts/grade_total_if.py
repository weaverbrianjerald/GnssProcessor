#!/usr/bin/env python3
"""Grade the total_IF level channel and test whether it has plateaued.

``total_IF`` is the ionosphere-free clock plus code-bias level a consumer actually
forms for a satellite:

    total_IF(s) = CLK(s) + a1*CB(s, band1) - a2*CB(s, band2)

with ``a1 = f1^2/(f1^2 - f2^2)``, ``a2 = f2^2/(f1^2 - f2^2)``.  It is graded as a
deviation from the per-epoch median across satellites, which makes it
datum-invariant (standing invariant #5): any common shift is absorbed by the
median, so what is left is between-satellite structure, the part a user cannot
absorb into its receiver clock.

Two traps this encodes:

  * **Grade each constellation separately.**  GPS L1/L2 and Galileo E1/E5a have
    different IF coefficients (2.5457/1.5457 vs 2.2606/1.2610).  Applying the GPS
    pair to a Galileo signal is not ionosphere-free at all.  Coefficients here are
    derived from the carrier frequencies rather than hard-coded, so a new band
    cannot silently inherit the wrong pair.  Pooling also folds the inter-system
    time offset into the spread.
  * **Convergence is measured, not assumed.**  The "about 2 h budget" was measured
    at 63 stations; a denser network separates bias from ambiguity within an epoch
    (the bias is common across receivers, the ambiguities are not), so it may
    settle sooner.  This reports per-window spread and flags a plateau when the
    relative change between consecutive windows stays under ``--tol``, so a run
    is declared gradeable on evidence, not on elapsed time.

Usage:
  python scripts/grade_total_if.py runs/agu305_gps_1h [--window-min 10]
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd

REPO = Path(__file__).resolve().parent.parent

# Carrier frequencies (Hz) for the IF pair used per constellation.
_BANDS = {
    "G": ("C1W", "C2W", 1575.42e6, 1227.60e6, "GPS L1/L2"),
    "E": ("C1C", "C5Q", 1575.42e6, 1176.45e6, "Galileo E1/E5a"),
}


def if_coeffs(f1: float, f2: float) -> tuple[float, float]:
    d = f1 * f1 - f2 * f2
    return f1 * f1 / d, f2 * f2 / d


def total_if(sol: pd.DataFrame, cons: str) -> pd.DataFrame | None:
    b1, b2, f1, f2, _ = _BANDS[cons]
    a1, a2 = if_coeffs(f1, f2)
    d = sol[sol["sat"].astype(str).str.startswith(cons)]
    clk = d[d["kind"] == "CLK"].set_index(["tow", "sat"])["value_m"]
    cb = d[d["kind"] == "CB"]
    c1 = cb[cb["signal"] == b1].set_index(["tow", "sat"])["value_m"]
    c2 = cb[cb["signal"] == b2].set_index(["tow", "sat"])["value_m"]
    tif = (clk + a1 * c1 - a2 * c2).dropna()
    if tif.empty:
        return None
    out = tif.reset_index(name="tif")
    out["dev"] = out["tif"] - out.groupby("tow")["tif"].transform("median")
    return out


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--window-min", type=float, default=10.0)
    ap.add_argument("--tol", type=float, default=0.05,
                    help="relative change between consecutive windows below which "
                         "the channel counts as plateaued (default 5%%)")
    args = ap.parse_args()

    run = Path(args.run_dir)
    if not run.is_absolute():
        run = REPO / run
    f = run / "logs" / "network_solution.csv"
    if not f.is_file():
        raise SystemExit(f"missing {f}")
    sol = pd.read_csv(f)
    sol = sol[(sol["tow"] >= 0) & (sol["tow"] < 604800)].dropna(
        subset=["tow", "kind", "value_m"])
    sol["signal"] = sol["signal"].fillna("")
    t0 = float(sol["tow"].min())

    for cons, (b1, b2, f1, f2, label) in _BANDS.items():
        tf = total_if(sol, cons)
        if tf is None:
            continue
        a1, a2 = if_coeffs(f1, f2)
        tf["min"] = (tf["tow"] - t0) / 60.0
        print(f"\n=== {label}  ({b1}/{b2}, a1={a1:.6f} a2={a2:.6f}) ===")
        print(f"{'window (min)':>14} {'n':>6} {'spread(std)':>12} "
              f"{'p90|dev|':>10} {'worst':>9}")
        rows = []
        w = args.window_min
        for lo in np.arange(0.0, tf["min"].max() + 1e-9, w):
            s = tf[(tf["min"] >= lo) & (tf["min"] < lo + w)]
            if len(s) < 10:
                continue
            sd = float(s["dev"].std())
            rows.append((lo, sd))
            print(f"{lo:6.0f}-{lo+w:<7.0f} {len(s):6d} {sd:12.3f} "
                  f"{np.percentile(s['dev'].abs(), 90):10.3f} "
                  f"{s['dev'].abs().max():9.3f}")
        # Plateau test: consecutive relative change under tol, twice running.
        #
        # A flat tail is not necessarily convergence: on the 61-station 12 h run
        # this channel falls to 7.0 m by hour 3-4 and then rises back to about
        # 11.9 m, and a plateau test that only looks at the tail would call the
        # degraded tail converged.  Always report the minimum window and how far
        # the final window sits above it, and refuse to call a run gradeable
        # while it is drifting away from its own best.
        streak, plateau_at = 0, None
        for i in range(1, len(rows)):
            prev, cur = rows[i - 1][1], rows[i][1]
            if abs(cur - prev) / max(prev, 1e-9) < args.tol:
                streak += 1
                if streak >= 2 and plateau_at is None:
                    plateau_at = rows[i - 1][0]
            else:
                streak = 0

        best_i = int(np.argmin([r[1] for r in rows]))
        best_at, best_sd = rows[best_i]
        last_at, last_sd = rows[-1]
        drift = (last_sd - best_sd) / max(best_sd, 1e-9)
        print(f"  minimum {best_sd:.3f} m at window {best_at:.0f} min; "
              f"final {last_sd:.3f} m ({100*drift:+.0f}% vs minimum)")
        if drift > 0.20:
            print(f"  => DEGRADING, not converged — the channel left its own best "
                  f"by {100*drift:.0f}%. A flat tail here is a wandering channel, "
                  f"NOT convergence. Do not grade absolute levels from the tail.")
        elif plateau_at is not None:
            print(f"  => PLATEAU from ~{plateau_at:.0f} min "
                  f"(<{100*args.tol:.0f}% change over 2 consecutive windows, and "
                  f"within 20% of the run minimum) — gradeable on this evidence.")
        else:
            print("  => NO PLATEAU yet — still moving at run end; "
                  "this run is NOT gradeable on total_IF.")


if __name__ == "__main__":
    main()

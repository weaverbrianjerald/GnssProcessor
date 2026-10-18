#!/usr/bin/env python3
"""Card CE / s24.17 leg 3 (G4) reader: the guard's behaviour in the +/-N epochs around an event.

    python ce_g4_read.py <arm> [<arm> ...] [--tow 334020] [--eps 20]

Per arm: the CLK_ANCHOR cta_decouple / cta_recouple rows in the window (the two edges of
invariant #7), the epoch dispositions (emitted vs withheld: adapt-and-emit is the pass
condition, a withhold is the failure), and the consumer's 3D through the window.
"""
import argparse
import csv
import os
import sys
from collections import Counter, defaultdict

import numpy as np

RUNS = "C:/GnssProcessor/runs"


def rows(p):
    if not os.path.isfile(p):
        return []
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        return list(csv.DictReader(fh))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("arms", nargs="+")
    ap.add_argument("--tow", type=float, default=334020.0)   # 20:47:00 UT DOY-129
    ap.add_argument("--eps", type=int, default=20)
    a = ap.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    lo, hi = a.tow - 30.0 * a.eps, a.tow + 30.0 * a.eps

    for arm in a.arms:
        d = os.path.join(RUNS, f"_cm_{arm}")
        print("=" * 104)
        print(f"### {arm}   window tow {lo:.0f} .. {hi:.0f}  (event {a.tow:.0f} = 20:47:00 UT, +/-{a.eps} epochs)")

        ev = [r for r in rows(os.path.join(d, "logs", "network_events.csv"))
              if r.get("tow") and lo - 1e-3 <= float(r["tow"]) <= hi + 1e-3]
        cta = [r for r in ev if r.get("tier") == "CLK_ANCHOR"]
        dec = [r for r in cta if r.get("event") == "cta_decouple"]
        rec = [r for r in cta if r.get("event") == "cta_recouple"]
        print(f"  CLK_ANCHOR in window: {len(dec)} cta_decouple / {len(rec)} cta_recouple "
              f"(run total {sum(1 for r in rows(os.path.join(d,'logs','network_events.csv')) if r.get('event')=='cta_decouple')}"
              f" / {sum(1 for r in rows(os.path.join(d,'logs','network_events.csv')) if r.get('event')=='cta_recouple')})")
        if dec:
            print("   decoupling edge (marks):")
            for r in dec:
                print(f"     tow {float(r['tow']):.0f}  {r['sat']:<4} |w| {float(r['statistic']):7.3f} > {float(r['threshold']):.1f}"
                      f"  action={r['action']}  {r['detail']}")
        if rec:
            print("   re-coupling edge:")
            for r in rec:
                print(f"     tow {float(r['tow']):.0f}  {r['sat']:<4} stat {float(r['statistic']):7.3f}  action={r['action']}  {r['detail']}")
        other = Counter(r["tier"] for r in ev if r.get("tier") != "CLK_ANCHOR")
        if other:
            print(f"   other event tiers in window: {dict(other)}")

        st = [r for r in rows(os.path.join(d, "logs", "network_epoch_status.csv"))
              if r.get("tow") and lo - 1e-3 <= float(r["tow"]) <= hi + 1e-3]
        disp = Counter((r.get("disposition") or r.get("status") or "?") for r in st)
        print(f"  epoch dispositions in window ({len(st)} epochs): {dict(disp)}")

        cl = [r for r in rows(os.path.join(d, "logs", "closed_loop_trace.csv"))
              if r.get("tow") and lo - 1e-3 <= float(r["tow"]) <= hi + 1e-3]
        if cl:
            v = np.array([float(r["pos_err_3d_m"]) for r in cl if r.get("pos_err_3d_m")])
            pre = np.array([float(r["pos_err_3d_m"]) for r in cl if float(r["tow"]) < a.tow])
            post = np.array([float(r["pos_err_3d_m"]) for r in cl if float(r["tow"]) >= a.tow])
            print(f"  consumer 3D in window: n {v.size}  p50 {np.percentile(v,50):.3f}  max {v.max():.3f} m"
                  f"   | pre-event p50 {np.percentile(pre,50):.3f} (n {pre.size})"
                  f"   post-event p50 {np.percentile(post,50):.3f} (n {post.size})"
                  f"   max |step| {np.abs(np.diff(v)).max() if v.size>1 else float('nan'):.3f} m")
        else:
            print("  consumer: no closed_loop_trace.csv rows in the window")


if __name__ == "__main__":
    sys.exit(main())

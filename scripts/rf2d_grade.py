#!/usr/bin/env python3
"""RF-2d one-command grade: the held-basis handover fix against its controls (a DIAGNOSTIC + the two bars).

Per run: exit, emitted / withheld, the cold consumer p50 by hour (refound_grade's own read of
closed_loop_trace.csv, in METRES), the per-satellite IF-correction dispersion (refound_grade.if_dispersion),
the declaration census (STRANSFORM rows: continuous / cold x marked), the reref erase count from the events,
and rf3_if_attrib's handover-mode table (cold handovers with an IF step > 1 m is the mechanism bar).

Usage: python scripts/rf2d_grade.py runs/_rf0g_sim6 runs/_rf3p_sim6 runs/_rf2d_sim6_mark ...
"""
import csv
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refound_grade as rg  # noqa: E402
import rf3_if_attrib as ra  # noqa: E402


def census(run_dir):
    c = Counter()
    p = os.path.join(run_dir, "logs", "network_events.csv")
    with open(p, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            if r["tier"] == "STRANSFORM" and r["event"] in ("sts_applied", "sts_cold_declared"):
                kind = "cont" if r["event"] == "sts_applied" else "cold"
                c[f"{kind}_m{1 if 'marked=1' in r['detail'] else 0}"] += 1
            elif r["tier"] == "CLK_STEP":
                c["clk_step"] += 1
            elif r["tier"] == "CLK_ANCHOR" and r["event"] == "cta_decouple":
                c["cta_decouple"] += 1
    h = Counter()
    with open(os.path.join(run_dir, "products.txt"), encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if ln.startswith("H,"):
                h["ok" if ln.split(",")[3] == "ok" else "withheld"] += 1
    return c, h


def main():
    for run in sys.argv[1:]:
        ex = open(os.path.join(run, "exitcode.txt")).read().split() if os.path.exists(os.path.join(run, "exitcode.txt")) else ["?"]
        c, h = census(run)
        rows = rg.read_consumer(run)
        tab, _ = rg.consumer_table(rows, 6)
        d = rg.if_dispersion(run) or {}
        print(f"\n##### {run}  exit {ex[0]}  wall {ex[1] if len(ex) > 1 else '?'} s  emitted {h['ok']} withheld {h['withheld']}")
        print("  consumer p50 by hour (m): " + " / ".join(f"{t['p50']:.2f}" for t in tab)
              + "   p90: " + " / ".join(f"{t['p90']:.2f}" for t in tab))
        if d:
            print(f"  IF dispersion p50 / p90 / max (m): {d['p50']:.3f} / {d['p90']:.3f} / {d['max']:.2f}  worst "
                  + ", ".join(f"{w['sat']} {w['dev_m']:+.2f}" for w in d['worst'][:3]))
        print(f"  declarations: continuous marked {c['cont_m1']} / unmarked {c['cont_m0']};  cold marked {c['cold_m1']} / "
              f"unmarked {c['cold_m0']};  CLK_STEP {c['clk_step']}  cta_decouple {c['cta_decouple']}")
        ra.handover_table(run)


if __name__ == "__main__":
    main()

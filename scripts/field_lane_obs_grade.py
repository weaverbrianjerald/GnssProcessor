#!/usr/bin/env python3
"""Grade a lane-observatory run's per-event LANE_OBS rows against the
SPEC_lane_slip_response §7.6 pre-registered field rule.

Input: a run directory whose logs/network_events.csv carries tier=LANE_OBS rows
(the §7.6 instrument; requires network.trace_events + relevel_lane_snap armed).

The pre-registered rule (§7.6, registered before the instrumented re-run):
  * grade the detector-driven (non-T4) sub-population at el > 30 deg;
      PASS      = >=5-cyc mass ~ 0 there and median |delta| inside the null
                  band (0.4-0.9 cyc);
      FALSIFIED = persistent >=5-cyc mass on that clean sub-population;
  * the T4-freed sub-population is a refloat-justification census (dMW ~ 0
    there, so a large delta is the carry error the refloat removed) and is
    never pooled into the lane-cost grade;
  * a detector sub-population under ~30 measured rows at el > 30 triggers
    §7.4's redesign, not a verdict.

This script reports; the verdict sentence belongs in the SPEC, written by a
person reading the report.
"""

import argparse
import collections
import csv
import sys
from pathlib import Path

BINS = [0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 25.0]
BIN_LABELS = ["<0.1", "<0.25", "<0.5", "<1", "<2", "<5", "<10", "<25", ">=25"]


def bin_of(ad: float) -> int:
    for i, edge in enumerate(BINS):
        if ad < edge:
            return i
    return len(BINS)


def parse_detail(detail: str) -> dict:
    out = {}
    for part in detail.split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    return out


def load_rows(run_dir: Path):
    path = run_dir / "logs" / "network_events.csv"
    if not path.is_file():
        sys.exit(f"no {path} — arm network.trace_events + relevel_lane_snap")
    rows = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r.get("tier") != "LANE_OBS":
                continue
            d = parse_detail(r.get("detail", ""))
            rows.append({
                "tow": float(r["tow"]),
                "sat": r["sat"],
                "sta": r["station"],
                "t4": r["event"] == "t4_delta",
                "measured": r["action"] == "measured",
                "outcome": r["action"],
                "delta": float(r["statistic"]) if r.get("statistic") else None,
                "dmw": float(r["threshold"]) if r.get("threshold") else 0.0,
                "cause": d.get("cause", "none"),
                "scope": d.get("scope", "?"),
                "pair": d.get("pair", "?"),
                "el": float(d.get("el", "0") or 0.0),
            })
    return rows


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if not n:
        return float("nan")
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])


def report_pop(tag, rows):
    meas = [r for r in rows if r["measured"]]
    print(f"  {tag}: {len(rows)} attempt(s), {len(meas)} measured")
    if not meas:
        return
    ads = [abs(r["delta"]) for r in meas]
    hist = [0] * 9
    for ad in ads:
        hist[bin_of(ad)] += 1
    n = len(ads)
    ge5 = sum(1 for ad in ads if ad >= 5.0)
    print(f"    med|d| {median(ads):.3f} cyc  mean {sum(ads)/n:.3f}  max {max(ads):.3f}"
          f"  <2cyc {100.0*sum(1 for a in ads if a < 2.0)/n:.1f}%  >=5cyc {ge5} ({100.0*ge5/n:.1f}%)")
    print("    hist  " + " | ".join(f"{lbl} {c}" for lbl, c in zip(BIN_LABELS, hist)))
    skips = collections.Counter(r["outcome"] for r in rows if not r["measured"])
    if skips:
        print("    skips " + ", ".join(f"{k} {v}" for k, v in sorted(skips.items())))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir", type=Path)
    ap.add_argument("--el-cut", type=float, default=30.0,
                    help="elevation cut for the pre-registered clean sub-population (deg)")
    ap.add_argument("--big", type=float, default=10.0,
                    help="attribution listing threshold (cyc)")
    ap.add_argument("--cluster-floor", type=float, default=5.0,
                    help="min |delta| for coherence clustering (cyc)")
    ap.add_argument("--cluster-gap", type=float, default=90.0,
                    help="max tow gap inside a cluster (s)")
    ap.add_argument("--cluster-min", type=int, default=3,
                    help="min distinct stations per cluster")
    args = ap.parse_args()

    rows = load_rows(args.run_dir)
    det = [r for r in rows if not r["t4"]]
    t4 = [r for r in rows if r["t4"]]
    print(f"== {args.run_dir}  ({len(rows)} LANE_OBS rows: {len(det)} detector / {len(t4)} T4)")

    print("\n-- DETECTOR population (the lane-cost grade) --")
    report_pop("all detector", det)
    hi = [r for r in det if r["el"] > args.el_cut]
    lo = [r for r in det if 0.0 < r["el"] <= args.el_cut]
    report_pop(f"el > {args.el_cut:.0f} (THE PRE-REGISTERED POPULATION)", hi)
    report_pop(f"el <= {args.el_cut:.0f}", lo)
    for scope in ("vertex", "band"):
        report_pop(f"scope={scope}", [r for r in det if r["scope"] == scope])
    print("  cause split (measured, bit counts): "
          + ", ".join(f"{k} {v}" for k, v in sorted(collections.Counter(
              c for r in det if r["measured"] for c in r["cause"].split("|")).items())))

    n_hi_meas = sum(1 for r in hi if r["measured"])
    if n_hi_meas < 30:
        print(f"\n  !! detector el>{args.el_cut:.0f} measured = {n_hi_meas} < 30 — §7.4 REDESIGN TRIGGER, not a verdict")

    print("\n-- T4 population (refloat-justification census; NEVER pooled into the grade) --")
    report_pop("all T4", t4)
    report_pop(f"T4 el > {args.el_cut:.0f}", [r for r in t4 if r["el"] > args.el_cut])

    # §7.7 splits by satellite coherence: a frame step is satellite-common at one
    # epoch (near-identical |delta| at several stations simultaneously), while a
    # per-arc carried error is not. A cluster is the same satellite, tows within
    # --cluster-gap of each other, at least --cluster-min distinct stations, all
    # with measured |delta| >= --cluster-floor. Clustered rows are reported apart
    # and the census is re-stated without them. The in-run gauge guard already
    # vets the traced frame channels, so a surviving cluster is either an
    # untraced frame event or a genuinely satellite-common carried error; the
    # spread column is the discriminator (a frame constant has close to zero spread).
    sus = sorted((r for r in t4 if r["measured"] and abs(r["delta"]) >= args.cluster_floor),
                 key=lambda r: (r["sat"], r["tow"]))
    clusters, cur = [], []
    for r in sus:
        if cur and (r["sat"] != cur[-1]["sat"] or r["tow"] - cur[-1]["tow"] > args.cluster_gap):
            clusters.append(cur); cur = []
        cur.append(r)
    if cur:
        clusters.append(cur)
    clusters = [c for c in clusters if len({r["sta"] for r in c}) >= args.cluster_min]
    clustered = {id(r) for c in clusters for r in c}
    print(f"\n-- §7.7 coherence clusters (sat-common, >= {args.cluster_min} sta, "
          f"|d| >= {args.cluster_floor:.0f}, gap <= {args.cluster_gap:.0f} s): {len(clusters)} --")
    for c in sorted(clusters, key=lambda c: -max(abs(r["delta"]) for r in c)):
        ads = sorted(abs(r["delta"]) for r in c)
        med = median(ads)
        spread = (ads[-1] - ads[0]) / med if med else float("inf")
        print(f"  {c[0]['sat']}  tow {c[0]['tow']:.0f}-{c[-1]['tow']:.0f}  n={len(c)}"
              f" sta={len({r['sta'] for r in c})}  med|d| {med:.1f}  range {ads[0]:.1f}-{ads[-1]:.1f}"
              f"  spread/med {spread:.2f}  stas={','.join(sorted({r['sta'][:4] for r in c}))}")
    report_pop("T4 EXCLUDING clustered rows", [r for r in t4 if id(r) not in clustered])
    report_pop(f"T4 excl. clustered, el > {args.el_cut:.0f}",
               [r for r in t4 if id(r) not in clustered and r["el"] > args.el_cut])

    big = sorted((r for r in rows if r["measured"] and abs(r["delta"]) >= args.big),
                 key=lambda r: -abs(r["delta"]))
    print(f"\n-- attribution: every measured row >= {args.big:.0f} cyc ({len(big)}) --")
    for r in big:
        print(f"  {'T4 ' if r['t4'] else '    '}tow {r['tow']:.0f}  {r['sta']}/{r['sat']}"
              f"  |d| {abs(r['delta']):8.3f}  dMW {r['dmw']:+8.3f}  el {r['el']:.1f}"
              f"  {r['pair']}  cause={r['cause']}  scope={r['scope']}")


if __name__ == "__main__":
    main()

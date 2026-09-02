#!/usr/bin/env python3
"""field_eclipse_census.py -- wire-only per-sat exclusion/return census.

The field analog of simnet_sat_clock_census (STABILITY targets 1/3, SPEC_
synthetic_minimum_network §18): on a lineage with exclude_eclipsing_satellites
armed, a satellite's ~55-min emission absence IS the exclusion window, readable
straight off the products with no truth: this script finds per-sat emission
gaps >= --min-gap epochs and measures, for each return,

  * the median-demeaned clock step across the gap (d = C - median_const(C),
    post-return minus pre-gap -- the same datum-free statistic the sim census
    grades, self-differenced so it needs no truth),
  * the per-signal code-bias (B record) steps across the gap (the clock<->bias
    split relocation channel), and
  * the URA at the return epoch vs the run median (is the edge priced?).

Join gate: on the 24 h sim bed this script's windows and steps must reproduce
the truth-based census (same 15 sats, same windows to the epoch, steps matching
the d(t) jumps).

Usage:
  python scripts/field_eclipse_census.py <run_dir> [--min-gap 10] [--pre 5] [--post 5]
"""
from __future__ import annotations

import argparse
import collections
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simnet_grade import parse_products  # noqa: E402


def p50(v):
    return sorted(v)[len(v) // 2] if v else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--min-gap", type=int, default=10,
                    help="emission absence (epochs) that counts as a window")
    ap.add_argument("--pre", type=int, default=5,
                    help="epochs averaged for the pre-gap baseline")
    ap.add_argument("--post", type=int, default=5,
                    help="epochs averaged for the post-return value")
    args = ap.parse_args()

    for st in (sys.stdout, sys.stderr):
        try:
            st.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError):
            pass

    s_line, epochs, _ = parse_products(os.path.join(args.run_dir, "products.txt"))
    pivots = (s_line or {}).get("pivots", {})
    if not epochs:
        raise SystemExit("no epochs parsed")
    t0 = epochs[0]["tow"]
    interval = (epochs[1]["tow"] - epochs[0]["tow"]) if len(epochs) > 1 else 30.0

    # Per-epoch median-demeaned clock d per sat, plus raw B values and URA.
    d_series = collections.defaultdict(list)     # sat -> [(idx, d)]
    b_series = collections.defaultdict(list)     # (sat, sig) -> [(idx, v)]
    u_series = collections.defaultdict(list)     # sat -> [(idx, ura)]
    present = collections.defaultdict(list)      # sat -> [idx]
    for i, ep in enumerate(epochs):
        by_c = collections.defaultdict(dict)
        for sat, c0 in ep["clocks"].items():
            if sat and sat != pivots.get(sat[0]):
                by_c[sat[0]][sat] = c0
        for cst, grp in by_c.items():
            med = p50(list(grp.values()))
            for sat, c0 in grp.items():
                d_series[sat].append((i, c0 - med))
        for sat in ep["clocks"]:
            present[sat].append(i)
        for (sat, sig), v in ep["code_bias"].items():
            b_series[(sat, sig)].append((i, v))
        for sat, u in ep["ura"].items():
            u_series[sat].append((i, u))

    print("=" * 100)
    print(f"WIRE-ONLY exclusion/return census — {args.run_dir}")
    print(f"  {len(epochs)} epochs, t0 tow {t0:.0f}, interval {interval:.0f} s, "
          f"pivots {pivots}; window = absence >= {args.min_gap} epochs")
    print("=" * 100)

    n_win = 0
    for sat in sorted(present):
        idxs = present[sat]
        gaps = []
        for a, b in zip(idxs, idxs[1:]):
            if b - a > args.min_gap:
                gaps.append((a, b))
        if not gaps:
            continue
        dmap = dict(d_series[sat])
        umap = dict(u_series[sat])
        u_med = p50([u for _, u in u_series[sat]])
        for (a, b) in gaps:
            n_win += 1
            pre = [dmap[i] for i in range(a - args.pre + 1, a + 1) if i in dmap]
            post = [dmap[i] for i in range(b, b + args.post) if i in dmap]
            step = (p50(post) - p50(pre)) if (pre and post) else float("nan")
            u_ret = umap.get(b, float("nan"))
            h_a = (epochs[a]["tow"] - t0) / 3600.0
            h_b = (epochs[b]["tow"] - t0) / 3600.0
            print(f"\n{sat}: window h{h_a:.2f} -> h{h_b:.2f} "
                  f"({(b - a) * interval / 60:.1f} min absent)")
            print(f"  clock d step across gap: {step:+8.3f} m   "
                  f"(pre {p50(pre):+.3f} -> post {p50(post):+.3f});  "
                  f"URA at return {u_ret:.3f} vs run p50 {u_med:.3f}")
            for (s2, sig), ser in sorted(b_series.items()):
                if s2 != sat:
                    continue
                bm = dict(ser)
                bpre = [bm[i] for i in range(a - args.pre + 1, a + 1) if i in bm]
                bpost = [bm[i] for i in range(b, b + args.post) if i in bm]
                if bpre and bpost:
                    bstep = p50(bpost) - p50(bpre)
                    if abs(bstep) > 0.05:
                        print(f"  B {sig}: {p50(bpre):+8.3f} -> {p50(bpost):+8.3f}  "
                              f"(step {bstep:+.3f})")
    if n_win == 0:
        print("\nno emission windows found (nothing excluded, or min-gap too large)")
    else:
        print(f"\n{n_win} window(s) total")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""simnet_sat_clock_census.py -- STABILITY target 3: per-sat settled clock tails.

The 12 h quiet bed shows metre-class SETTLED clock tails on a few satellites
(G25/G26/G12 reported, fixture-common to both datum arms, never root-caused).
This is the instrument-first census: per-satellite settled clock error joined
against coverage (how many stations truth-see the sat) and wire presence
(emitted vs truth-visible -- contiguous not-emitted-while-visible windows are
the eclipse-exclusion signature: the producer arms
exclude_eclipsing_satellites and logs no per-event line), plus the producer's
own URA sigma (is the tail flagged or silent?).

Datum handling: on this null-truth bed the emitted C record IS the error in
the producer's own frame (grade_clock's null test).  The per-sat tail metric
is the per-epoch per-constellation MEDIAN-demeaned residual d_sat(t) --
robust common-mode removal, so a wandering datum level cannot masquerade as a
per-sat tail and the metric is comparable across datum postures.  Pivot rows
(pinned 0) are excluded.

Usage:
  python scripts/simnet_sat_clock_census.py <fixture_dir> --run <run> [--run <run2> ...]
                                            [--settle-frac 0.5] [--top 12]
"""
from __future__ import annotations

import argparse
import collections
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simnet_grade import parse_products  # noqa: E402

EP = 30.0


def p50(v):
    return sorted(v)[len(v) // 2] if v else float("nan")


def rms(v):
    return math.sqrt(sum(x * x for x in v) / len(v)) if v else float("nan")


def load_visibility(fixture_dir):
    """(tow, sat) -> n stations truth-seeing it; plus the epoch tow list."""
    cov = collections.Counter()
    tows = set()
    stas = [d for d in os.listdir(fixture_dir)
            if os.path.isfile(os.path.join(fixture_dir, d, "truth_visibility.csv"))]
    for sta in sorted(stas):
        with open(os.path.join(fixture_dir, sta, "truth_visibility.csv"),
                  newline="") as f:
            import csv
            for r in csv.DictReader(f):
                tow = round(float(r["tow_s"]), 3)
                tows.add(tow)
                for sat in r["sat_list"].split():
                    cov[(tow, sat)] += 1
    return cov, sorted(tows), len(stas)


def census_run(run_dir, cov, settle_frac):
    """Per-sat settled stats + emit/visibility gap windows for one run."""
    s_line, epochs, _ = parse_products(os.path.join(run_dir, "products.txt"))
    pivots = (s_line or {}).get("pivots", {})
    n_ep = len(epochs)
    start = int(n_ep * settle_frac)
    t0 = epochs[0]["tow"] if epochs else 0.0

    per = collections.defaultdict(list)         # sat -> [(tow, d)] settled
    own = collections.defaultdict(list)         # sat -> [e own-frame] settled
    sig = collections.defaultdict(list)         # sat -> [ura] settled
    gaps = collections.defaultdict(list)        # sat -> [(tow_start, tow_end)] visible-not-emitted
    open_gap = {}
    cov_of_emit = collections.defaultdict(list)  # sat -> coverage when emitted (settled)

    for i_ep, ep in enumerate(epochs):
        tow = round(ep["tow"], 3)
        settled = i_ep >= start
        by_c = collections.defaultdict(dict)
        for sat, c0 in ep["clocks"].items():
            if not sat or sat == pivots.get(sat[0]):
                continue
            by_c[sat[0]][sat] = c0
        for cst, grp in by_c.items():
            med = p50(list(grp.values()))
            for sat, c0 in grp.items():
                if settled:
                    per[sat].append((tow, c0 - med))
                    own[sat].append(c0)
                    u = ep["ura"].get(sat)
                    if u is not None and u > 0:
                        sig[sat].append(u)
                    cov_of_emit[sat].append(cov.get((tow, sat), 0))
        # emit-vs-visibility gap tracking (whole arc, not just settled): a sat
        # truth-visible at >= 10 stations (the producer's min_stations) but not
        # in this epoch's C set is producer-side excluded (eclipse or QC).
        candidates = set(ep["clocks"]) | set(open_gap)
        for sat in candidates | VIS_BY_TOW.get(tow, set()):
            visible = cov.get((tow, sat), 0) >= 10
            emitted = sat in ep["clocks"]
            if visible and not emitted:
                if sat not in open_gap:
                    open_gap[sat] = tow
            else:
                if sat in open_gap:
                    gaps[sat].append((open_gap.pop(sat), tow - EP))
    for sat, t_open in open_gap.items():
        gaps[sat].append((t_open, epochs[-1]["tow"]))
    return {"per": per, "own": own, "sig": sig, "gaps": gaps,
            "cov_of_emit": cov_of_emit, "pivots": pivots, "t0": t0,
            "n_ep": n_ep}


VIS_BY_TOW = {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fixture_dir")
    ap.add_argument("--run", action="append", required=True)
    ap.add_argument("--settle-frac", type=float, default=0.5)
    ap.add_argument("--top", type=int, default=12)
    args = ap.parse_args()

    for st in (sys.stdout, sys.stderr):
        try:
            st.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, OSError):
            pass

    cov, tows, n_sta = load_visibility(args.fixture_dir)
    # visibility index tow -> set(sats) for the gap scan
    for (tow, sat) in cov:
        VIS_BY_TOW.setdefault(tow, set()).add(sat)

    print("=" * 100)
    print(f"PER-SAT SETTLED CLOCK CENSUS — fixture {args.fixture_dir} "
          f"({n_sta} stations with truth_visibility)")
    print("=" * 100)

    results = {}
    for run in args.run:
        results[run] = census_run(run, cov, args.settle_frac)

    for run, R in results.items():
        per, sig = R["per"], R["sig"]
        t0 = R["t0"]
        print(f"\n### run {run}  (pivots {R['pivots']}, {R['n_ep']} epochs, "
              f"settled from frac {args.settle_frac})")
        rank = sorted(per, key=lambda s: -rms([d for _, d in per[s]]))
        print(f"  {'sat':<5} {'n':>5} {'rms(d)':>8} {'p50|d|':>8} {'max|d|':>8} "
              f"{'own rms':>8} {'URA p50':>8} {'cov p50':>7} {'cov min':>7} "
              f"{'worst hour (rms)':>18}")
        show = set(rank[:args.top]) | {"G25", "G26", "G12"}
        for sat in rank:
            if sat not in show:
                continue
            d = [x for _, x in per[sat]]
            hour_rms = collections.defaultdict(list)
            for tow, x in per[sat]:
                hour_rms[int((tow - t0) // 3600)].append(x)
            hr = {h: rms(v) for h, v in hour_rms.items()}
            wh = max(hr, key=hr.get) if hr else -1
            covs = R["cov_of_emit"][sat]
            print(f"  {sat:<5} {len(d):>5} {rms(d):>8.3f} "
                  f"{p50([abs(x) for x in d]):>8.3f} "
                  f"{max(abs(x) for x in d):>8.3f} {rms(R['own'][sat]):>8.3f} "
                  f"{p50(sig[sat]):>8.3f} {p50(covs):>7.0f} "
                  f"{min(covs) if covs else 0:>7.0f} "
                  f"{f'h{wh} ({hr.get(wh, float() ):.3f})':>18}")
        # gap windows (eclipse-exclusion signature)
        gap_sats = {s: g for s, g in R["gaps"].items() if g}
        if gap_sats:
            print(f"\n  visible(>=10 sta)-but-NOT-emitted windows "
                  f"({len(gap_sats)} sats):")
            for sat in sorted(gap_sats,
                              key=lambda s: -sum(b - a for a, b in gap_sats[s])):
                spans = gap_sats[sat]
                tot_min = sum(b - a + EP for a, b in spans) / 60.0
                span_str = ", ".join(
                    f"[h{(a - t0) / 3600:.2f}-h{(b - t0) / 3600:.2f}]"
                    for a, b in spans[:4])
                more = f" (+{len(spans) - 4} more)" if len(spans) > 4 else ""
                print(f"    {sat:<5} {tot_min:7.1f} min in {len(spans)} "
                      f"window(s): {span_str}{more}")
        else:
            print("\n  no visible-but-not-emitted windows (no exclusion "
                  "signature on this run)")

    if len(results) > 1:
        print("\n### cross-run rms(d) comparison (datum-independence check)")
        sats = sorted({s for R in results.values() for s in R["per"]})
        hdr = "".join(f"{os.path.basename(r):>28}" for r in results)
        print(f"  {'sat':<5}{hdr}")
        rows = []
        for sat in sats:
            vals = [rms([d for _, d in R["per"].get(sat, [])]) or float("nan")
                    for R in results.values()]
            rows.append((max(vals), sat, vals))
        rows.sort(reverse=True)
        for _, sat, vals in rows[:args.top]:
            print(f"  {sat:<5}" + "".join(f"{v:>28.3f}" for v in vals))


if __name__ == "__main__":
    main()

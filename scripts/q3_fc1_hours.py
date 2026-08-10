#!/usr/bin/env python3
"""The FC-1 SHADOW HOUR TABLE — the score for Card Q3 work.

``project_fast_clock_card``: *"KEEP network.fast_clock.shadow as a standing
instrument ... it is the only way to answer 'how much of the emitted clock is
real?'.  Re-run it after any Q3 work — the hour table above IS the score for that
work."*  The table it names is, per hour:

    | producer Δc0 | phase-measured Δc0 | corr |

both common-mode removed (standing invariant #5 — the per-epoch clock ensemble
is a free gauge), where "phase-measured" is FC-1's TDCP tick: the satellite clock
change derived from CONTINUOUS CARRIER PHASE, which shares no machinery with the
producer's own clock estimate.

The 2026-07-30 baseline this is graded against (50 sta, GPS+Gal, 3 h):

    hour 0  producer 79.4 cm | phase 1.74 cm | corr -0.009
    hour 1           67.8    |       1.73    |      -0.001
    hour 2           83.1    |       1.93    |      +0.018

i.e. ~98 % of the emitted clock's epoch-to-epoch motion was NOT information.  A
successful anchor moves the producer column toward the phase column and lifts
the correlation off zero.  Both readings matter and the correlation is the
harder one: shrinking the producer column while corr stays 0 means the walk got
quieter without getting more real.

⚠ The producer column here is the RMS of the common-removed per-epoch change of
the EMITTED clock — the same object the dev channel of
``scripts/product_step_census.py`` reports as a robust sigma.  It is NOT
comparable to a raw (un-common-removed) clock RMS.

Usage:
    python scripts/q3_fc1_hours.py <run_dir> [<run_dir> ...]
      (each run_dir needs products.txt and network_fast_clock.csv, the latter
       either alongside it or under logs/)
"""
import collections
import math
import os
import statistics as st
import sys


def find(run, name):
    for p in (os.path.join(run, name), os.path.join(run, "logs", name)):
        if os.path.exists(p):
            return p
    return None


def load_products(path):
    clk, tow = collections.defaultdict(dict), None
    with open(path) as f:
        for line in f:
            if line.startswith("E,"):
                tow = round(float(line.split(",")[2]), 3)
            elif line.startswith("C,") and tow is not None:
                p = line.rstrip("\n").split(",")
                clk[tow][p[1]] = float(p[2])
    return clk


def load_marked(path):
    """{tow: {sat}} carried by R records — per-satellite discontinuities the
    producer DECLARED at that epoch.

    Why this matters for the score: Card Q3's guard answers a detected clock
    event by refloating the satellite and MARKING it, so the emitted value takes
    a deliberate one-epoch step and the consumer is told to reset there.  An RMS
    taken over those epochs charges the card for doing exactly what it says it
    does, and RMS is tail-dominated, so a handful of marked steps can move the
    aggregate the opposite way from the body of the distribution (measured: the
    3 h anchor arm improved every satellite in the worst-5 table while its
    aggregate SD-RMS rose 21.7 -> 27.1 cm).  Both numbers are reported below;
    neither is quoted alone.
    """
    marked, tow = collections.defaultdict(set), None
    with open(path) as f:
        for line in f:
            if line.startswith("E,"):
                tow = round(float(line.split(",")[2]), 3)
            elif line.startswith("R,") and tow is not None:
                for s in line.rstrip("\n").split(",")[1:]:
                    if s:
                        marked[tow].add(s)
    return marked


def load_ticks(path):
    per = collections.defaultdict(dict)
    with open(path) as f:
        if not f.readline().startswith("week,tow,kind"):
            raise SystemExit("unexpected fast-clock trace header")
        for raw in f:
            p = raw.rstrip("\r\n").split(",")
            if len(p) < 12 or p[2] != "S" or not p[9]:
                continue
            per[round(float(p[1]), 3)][p[9]] = float(p[10])
    return per


def demean(d):
    """Remove the per-epoch common mode (the free clock-ensemble gauge)."""
    if len(d) < 2:
        return {}
    m = st.median(d.values())
    return {k: v - m for k, v in d.items()}


def main():
    for run in sys.argv[1:]:
        pp, tp = find(run, "products.txt"), find(run, "network_fast_clock.csv")
        if not pp or not tp:
            print(f"{run}: missing products.txt or network_fast_clock.csv "
                  f"(is network.fast_clock.shadow + trace armed?)")
            continue
        clk, per = load_products(pp), load_ticks(tp)
        marked = load_marked(pp)
        tows = sorted(clk)
        prev = {tows[i]: tows[i - 1] for i in range(1, len(tows))}
        t0 = tows[0]
        rows = collections.defaultdict(list)     # hour -> [(producer, phase, is_marked)]
        for t in sorted(per):
            if t not in prev:
                continue
            a = prev[t]
            prod, phase = {}, {}
            for sat, dc in per[t].items():
                if sat in clk.get(t, {}) and sat in clk.get(a, {}):
                    prod[sat] = clk[t][sat] - clk[a][sat]
                    phase[sat] = dc
            pd_, ph = demean(prod), demean(phase)
            common = sorted(set(pd_) & set(ph))
            if len(common) < 4:
                continue
            h = int((t - t0) // 3600)
            mk = marked.get(t, set())
            for s in common:
                rows[h].append((pd_[s], ph[s], s in mk))
        if not rows:
            print(f"{run}: no gradable epochs (the shadow tier produced no per-sat rows)")
            continue
        print("=" * 74)
        print(f"{run}")
        print("  hour      n   producer  phase-meas     corr    ratio   prod_rob  prod_unmk")

        def block(vals, label):
            p = [x[0] for x in vals]
            q = [x[1] for x in vals]
            n = len(p)
            rp = math.sqrt(sum(v * v for v in p) / n)
            rq = math.sqrt(sum(v * v for v in q) / n)
            mp, mq = sum(p) / n, sum(q) / n
            sp = math.sqrt(sum((v - mp) ** 2 for v in p) / n)
            sq = math.sqrt(sum((v - mq) ** 2 for v in q) / n)
            cov = sum((p[i] - mp) * (q[i] - mq) for i in range(n)) / n
            c = cov / (sp * sq) if sp > 0 and sq > 0 else float("nan")
            # prod_rob : robust sigma (1.4826*MAD) of the SAME producer column --
            #            the BODY of the distribution, insensitive to the marked
            #            steps that dominate an RMS.
            # prod_unmk: the RMS with (sat,epoch) pairs the producer MARKED removed
            #            -- a declared discontinuity is not an error the consumer
            #            integrates, it is one it is told to reset at.
            ap_ = sorted(abs(v) for v in p)
            rob = 1.4826 * ap_[len(ap_) // 2]
            um = [x[0] for x in vals if not x[2]]
            rum = math.sqrt(sum(v * v for v in um) / len(um)) if um else float("nan")
            print(f"  {label:>4} {n:>6} {100 * rp:>10.2f} {100 * rq:>11.2f} "
                  f"{c:>+8.3f} {(rq / rp if rp > 0 else float('nan')):>8.3f} "
                  f"{100 * rob:>10.2f} {100 * rum:>10.2f}")

        for h in sorted(rows):
            block(rows[h], str(h))
        block([x for h in rows for x in rows[h]], "ALL")
        print("  (cm; both columns common-mode removed.  ~98 % churn = ratio ~0.02 and")
        print("   corr ~0; a working anchor raises BOTH.  prod_rob = robust sigma of the")
        print("   producer column (the BODY); prod_unmk = its RMS with producer-MARKED")
        print("   (sat,epoch) pairs removed.  Quote all three -- an RMS alone charges a")
        print("   guarded anchor for the declared steps that ARE its response.)")


if __name__ == "__main__":
    main()

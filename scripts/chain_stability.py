#!/usr/bin/env python3
"""Stability / convergence assessment for a DOY-129-style continuous network run.

WHAT THIS GRADES, AND WHY IT CHANGED
------------------------------------
Earlier versions of this script graded ``rms_clock_m`` from network_trace.csv.
That was wrong twice over:

  1. ``rms_clock_m`` is RMS *about zero of the raw satellite clock state*
     (NetworkStashStatus.cpp:149-155).  It is datum-dependent, so it confounds
     benign frame drift with real defects -- exactly what standing invariant #5
     ("grade only datum-invariant / SD quantities") forbids.

  2. Even the datum-free clock statistics (clock_median_m / clock_std_m /
     worst_sat / worst_dev_m, NetworkStashStatus.cpp:176-195, already written to
     network_epoch_status.csv) are INSUFFICIENT.  The clock datum pins one
     satellite's clock to exactly 0.0000 -- measured on DOY 129: G01 for 2051 of
     2160 epochs.  Damage that lands in that satellite's CODE BIAS is invisible
     to every clock-only metric.  On DOY 129 the only persistent damage of the
     day did exactly that: G01's IF code bias stepped 15.488 -> 64.553 m in one
     30 s epoch and was still +42.68 m off the constellation at end of day,
     while its clock read 0.0000 throughout.

So this script grades the TOTAL ionosphere-free code correction a consumer
actually applies:

    total_IF(sat) = CLK(sat) + c1*CB(sat, L1 code) - c2*CB(sat, L2 code)
    c1 = f1^2/(f1^2 - f2^2) = 2.545728      f1 = 1575.42 MHz
    c2 = f2^2/(f1^2 - f2^2) = 1.545728      f2 = 1227.60 MHz

taken BETWEEN SATELLITES (deviations from the per-epoch median over all
satellites, including the pinned one -- a user cannot exclude the pivot, it
applies its C and B lines like any other).  The only remaining gauge freedom is
an epoch-common constant, which a user's receiver clock absorbs; the median
removes it.  Phase bias (PB) is deliberately NOT graded: it is integer-ambiguous
and its between-satellite SD is dominated by whole-cycle branch offsets.

    python scripts/chain_stability.py [--runroot ...] [--mask 20] [--csv out.csv]
"""
import argparse
import csv
import glob
import os
import statistics
from collections import Counter, defaultdict

DAY_ANCHOR_TOW = 259200.0          # 2024-05-08 00:00:00 GPS
EPOCH_S = 30.0
C1 = 2.545728                       # f1^2/(f1^2-f2^2)  -- GPS L1/L2, kept for reference
C2 = 1.545728                       # f2^2/(f1^2-f2^2)
DEFAULT_L1 = ("C1W", "C1C", "C1X", "C1P")
DEFAULT_L2 = ("C2W", "C2C", "C2X", "C2P", "C2L", "C2S")

# Carrier frequencies in Hz, keyed by system letter then RINEX band digit.
# Source of these numbers: include/common/Constants.hpp (GPS_L1_FREQ etc.) --
# do not retype them from a datasheet, keep them equal to the producer's.
FREQ_HZ = {
    "G": {"1": 1575.42e6, "2": 1227.60e6, "5": 1176.45e6},
    "E": {"1": 1575.42e6, "5": 1176.45e6, "6": 1278.75e6, "7": 1207.14e6,
          "8": 1191.795e6},
    "C": {"1": 1575.42e6, "2": 1561.098e6, "5": 1176.45e6, "6": 1268.52e6,
          "7": 1207.14e6},
}

# Per-system ionosphere-free pin pair, as (band1_codes, band2_codes) in RINEX
# attribute-preference order (most preferred first).  These mirror the pair the
# producer's W6 code-bias IF pin actually selects for each constellation:
#   GPS      L1/L2  -- the legacy IGS convention the .BIA products are cast in
#   Galileo  E1/E5a -- Galileo has NO L2 band at all, so the GPS list can never
#                      match it; before this table every Galileo satellite was
#                      silently dropped from the grade.
#   BeiDou   B1/B2a
IF_PAIR = {
    "G": (("C1W", "C1C", "C1X", "C1P"), ("C2W", "C2C", "C2X", "C2P", "C2L", "C2S")),
    "E": (("C1C", "C1X", "C1B"), ("C5Q", "C5X", "C5I")),
    "C": (("C1P", "C1X", "C2I"), ("C5P", "C5X", "C5D")),
}

SYS_NAME = {"G": "GPS", "E": "Galileo", "C": "BeiDou"}


def if_coeffs(sysid, code1, code2):
    """(c1, c2) for the IF combination formed from two RINEX observation codes.

    c1 = f1^2/(f1^2-f2^2), c2 = f2^2/(f1^2-f2^2), derived from the band digit of
    each code so that every constellation gets ITS OWN coefficients.  Applying the
    GPS L1/L2 pair (2.545728, 1.545728) to a Galileo E1/E5a pair -- which is what
    naively widening the band-2 code list would do -- yields a garbage combination
    that is not ionosphere-free at all.  Galileo E1/E5a is (2.260604, 1.260604).
    """
    f1 = FREQ_HZ[sysid][code1[1]]
    f2 = FREQ_HZ[sysid][code2[1]]
    den = f1 * f1 - f2 * f2
    return (f1 * f1) / den, (f2 * f2) / den


def hms(tow):
    s = int(round(tow - DAY_ANCHOR_TOW))
    return f"{s // 3600:02d}:{(s % 3600) // 60:02d}:{s % 60:02d}"


def load_solution(path, sysid="G"):
    """tow -> {sat: total_IF_m} for ONE constellation, plus per-epoch disc bumps.

    Returns (epochs, disc_events) where epochs is an ordered list of
    (tow, {sat: total_if}) and disc_events is a list of (tow, sat, signal, counter).

    One constellation at a time, deliberately.  Each constellation carries its own
    clock datum and its own per-(constellation,signal) code-bias sum-to-zero, so
    total_IF values from different systems live in DIFFERENT gauges.  Pooling them
    into one per-epoch median would fold the inter-system time offset into the
    spread and report a datum artifact as producer error (standing invariant #5).
    """
    clk = defaultdict(dict)
    cb = defaultdict(lambda: defaultdict(dict))
    disc_last = {}
    disc_events = []
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                tow = float(r["tow"])
                sat = r["sat"]
                kind = r["kind"]
                val = float(r["value_m"])
            except (ValueError, KeyError, TypeError):
                continue
            if not sat.startswith(sysid):
                continue
            if kind == "CLK":
                clk[tow][sat] = val
            elif kind == "CB":
                sig = r.get("signal", "")
                cb[tow][sat][sig] = val
                dc = r.get("disc_counter")
                if dc not in (None, ""):
                    key = (sat, sig)
                    try:
                        dcv = int(dc)
                    except ValueError:
                        continue
                    if key in disc_last and dcv != disc_last[key]:
                        disc_events.append((tow, sat, sig, dcv))
                    disc_last[key] = dcv

    band1_codes, band2_codes = IF_PAIR[sysid]
    epochs = []
    for tow in sorted(clk):
        sats = {}
        for sat, c in clk[tow].items():
            sig = cb[tow].get(sat, {})
            k1 = next((s for s in band1_codes if s in sig), None)
            k2 = next((s for s in band2_codes if s in sig), None)
            if k1 is None or k2 is None:
                continue
            c1, c2 = if_coeffs(sysid, k1, k2)
            sats[sat] = c + c1 * sig[k1] - c2 * sig[k2]
        if len(sats) >= 3:
            epochs.append((tow, sats))
    return epochs, disc_events


def epoch_stats(sats):
    """Gauge-invariant spread: deviations from the epoch median over all sats."""
    med = statistics.median(sats.values())
    dev = {s: v - med for s, v in sats.items()}
    worst_sat = max(dev, key=lambda s: abs(dev[s]))
    return {
        "n_sat": len(sats),
        "spread": statistics.pstdev(dev.values()),
        "worst": abs(dev[worst_sat]),
        "worst_sat": worst_sat,
        "worst_signed": dev[worst_sat],
        "dev": dev,
    }


def load_epoch_status(path):
    """The producer's own datum-free clock stats -- reported for comparison only."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            try:
                out[float(r["tow"])] = (float(r["clock_std_m"]), float(r["worst_dev_m"]))
            except (ValueError, KeyError, TypeError):
                continue
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runroot", default=r"C:/GnssProcessor/runs/day129_cont")
    ap.add_argument("--mask", type=int, default=20,
                    help="epochs to drop after each segment start (seam spike)")
    ap.add_argument("--event-threshold", type=float, default=40.0,
                    help="worst-satellite deviation (m) that marks an excursion epoch")
    ap.add_argument("--csv", default=None)
    ap.add_argument("--sys", default="G",
                    help="constellation(s) to grade: G, E, C, a comma list, or 'all'. "
                         "Each is graded SEPARATELY -- their total_IF values are in "
                         "different gauges and must never share a median. Default G "
                         "(GPS), which is what this script has always graded.")
    args = ap.parse_args()

    segs = sorted(glob.glob(os.path.join(args.runroot, "seg*")))
    if not segs:
        # A single continuous run is the degenerate one-segment chain: no seg*/
        # subdirectories, logs/ sits directly under the runroot.  Grade it as one
        # segment rather than refusing -- the metric and the masking are identical,
        # there is simply only one segment start to mask (the cold-start ignition).
        if os.path.exists(os.path.join(args.runroot, "logs", "network_solution.csv")):
            segs = [args.runroot]
        else:
            raise SystemExit(
                f"no segments found: {args.runroot} has neither seg*/ subdirectories "
                f"nor logs/network_solution.csv")

    if args.sys.lower() == "all":
        systems = [s for s in ("G", "E", "C")
                   if any(present_systems(os.path.join(sg, "logs", "network_solution.csv"))
                          & {s} for sg in segs)]
    else:
        systems = [s.strip().upper() for s in args.sys.split(",") if s.strip()]
    unknown = [s for s in systems if s not in IF_PAIR]
    if unknown:
        raise SystemExit(f"--sys: no IF pin pair defined for {unknown}; known: {sorted(IF_PAIR)}")
    if not systems:
        raise SystemExit("--sys: no gradable constellation found in the solution trace")

    rc = 0
    for i, sysid in enumerate(systems):
        if i:
            print("\n")
        print("#" * 96)
        print(f"# CONSTELLATION {sysid} ({SYS_NAME.get(sysid, sysid)})"
              f"   pin pair {IF_PAIR[sysid][0][0]}/{IF_PAIR[sysid][1][0]}")
        print("#" * 96)
        rc |= report_for_system(segs, sysid, args)
    return rc


def present_systems(path):
    """System letters that appear as CLK rows in a solution trace."""
    out = set()
    if not os.path.exists(path):
        return out
    with open(path, newline="") as fh:
        for r in csv.DictReader(fh):
            if r.get("kind") == "CLK" and r.get("sat"):
                out.add(r["sat"][0])
    return out


def report_for_system(segs, sysid, args):
    rows = []          # (tow, stats, seg_index_within_segment)
    disc_all = []
    status_all = {}
    for seg in segs:
        sol = os.path.join(seg, "logs", "network_solution.csv")
        if not os.path.exists(sol):
            print(f"{os.path.basename(seg)}: no network_solution.csv")
            continue
        eps, disc = load_solution(sol, sysid)
        disc_all.extend(disc)
        status_all.update(load_epoch_status(os.path.join(seg, "logs", "network_epoch_status.csv")))
        for i, (tow, sats) in enumerate(eps):
            rows.append((tow, epoch_stats(sats), i))
        if not eps:
            print(f"{os.path.basename(seg):<8} no {sysid} epochs with a complete "
                  f"{IF_PAIR[sysid][0][0]}/{IF_PAIR[sysid][1][0]} pin pair")
            continue
        print(f"{os.path.basename(seg):<8} {len(eps)} epochs  "
              f"{hms(eps[0][0])} -> {hms(eps[-1][0])}")

    if not rows:
        raise SystemExit("no gradable epochs")
    rows.sort(key=lambda r: r[0])
    masked = [r for r in rows if r[2] >= args.mask]

    print()
    print("=" * 96)
    print("PRODUCT QUALITY -- total IF code correction, between-satellite")
    _k1, _k2 = IF_PAIR[sysid][0][0], IF_PAIR[sysid][1][0]
    _c1, _c2 = if_coeffs(sysid, _k1, _k2)
    print(f"  total_IF = CLK + {_c1:.6f}*CB({_k1}) - {_c2:.6f}*CB({_k2});"
          f"  deviations from per-epoch median")
    print(f"  (gauge-invariant WITHIN {SYS_NAME.get(sysid, sysid)}; includes the pinned "
          f"pivot, which a consumer cannot exclude)")
    print("=" * 96)

    def col(vals, f=".3f"):
        return format(statistics.median(vals), f) if vals else "-"

    print(f"{'hr':>3}{'n':>6}{'nsat':>6}{'spread_med':>12}{'spread_p90':>12}"
          f"{'worst_med':>11}{'worst_max':>11}{'worst_sat':>11}{'clk_std':>9}")
    print("-" * 96)
    hourly = []
    buckets = defaultdict(list)
    for tow, st, _ in masked:
        buckets[int((tow - DAY_ANCHOR_TOW) // 3600)].append((tow, st))
    for h in sorted(buckets):
        b = buckets[h]
        spreads = sorted(st["spread"] for _, st in b)
        worsts = [st["worst"] for _, st in b]
        wsat = Counter(st["worst_sat"] for _, st in b).most_common(1)[0][0]
        cstd = [status_all[t][0] for t, _ in b if t in status_all]
        rec = {
            "hour": h, "n": len(b),
            "nsat": statistics.mode([st["n_sat"] for _, st in b]),
            "spread_med": statistics.median(spreads),
            "spread_p90": spreads[min(len(spreads) - 1, 9 * len(spreads) // 10)],
            "worst_med": statistics.median(worsts),
            "worst_max": max(worsts),
            "worst_sat": wsat,
            "clock_std_med": statistics.median(cstd) if cstd else float("nan"),
        }
        hourly.append(rec)
        print(f"{h:>3}{rec['n']:>6}{rec['nsat']:>6}{rec['spread_med']:>12.3f}"
              f"{rec['spread_p90']:>12.3f}{rec['worst_med']:>11.2f}{rec['worst_max']:>11.2f}"
              f"{wsat:>11}{rec['clock_std_med']:>9.2f}")

    # ---- events -----------------------------------------------------------
    print()
    print(f"EXCURSIONS  (contiguous epochs with worst-satellite deviation > {args.event_threshold:.0f} m)")
    runs, cur = [], []
    for tow, st, _ in rows:
        if st["worst"] > args.event_threshold:
            cur.append((tow, st))
        elif cur:
            runs.append(cur); cur = []
    if cur:
        runs.append(cur)
    if not runs:
        print("  none")
    for r in runs:
        pk = max(r, key=lambda x: x[1]["worst"])
        print(f"  {hms(r[0][0])} -> {hms(r[-1][0])}  {len(r):>4} ep ({len(r) * 0.5:5.1f} min)"
              f"  peak worst={pk[1]['worst']:9.2f} m on {pk[1]['worst_sat']}"
              f"  peak spread={max(x[1]['spread'] for x in r):8.2f} m")

    # ---- quiescent trend --------------------------------------------------
    ev = set()
    for r in runs:
        for tow, _ in r:
            ev.add(tow)
    quiet = [(t, s) for t, s, _ in masked if t not in ev]
    print()
    print("QUIESCENT TREND  (excursion epochs removed -- does the producer drift over a day?)")
    if len(quiet) >= 60:
        n3 = len(quiet) // 3
        a = statistics.median([s["spread"] for _, s in quiet[:n3]])
        c = statistics.median([s["spread"] for _, s in quiet[2 * n3:]])
        span_h = (quiet[-1][0] - quiet[0][0]) / 3600.0
        print(f"  n={len(quiet)} over {span_h:.1f} h   first third {a:.3f} m -> last third {c:.3f} m"
              f"   ratio {c / a:.3f}")
        print(f"  verdict: {'NO DRIFT (spread shrinks)' if c <= a * 1.05 else 'DRIFT -- investigate'}")
    else:
        print("  not enough quiescent epochs")

    # ---- between-hour distortion, fully datum-free ------------------------
    #
    # NB: do NOT read per-satellite "deviation from the epoch median" changes as
    # per-satellite damage.  The median itself moves, so a set of satellites that
    # actually held station together can all appear to shift by ~equal amounts
    # (observed on DOY 129: G02/G21/G28/G03 each "moved" 17-23 m while their
    # MUTUAL differences changed by <5 m -- they were fine; the median moved).
    # The only honest statement is about PAIRWISE differences, which carry no
    # datum at all.  Report the SPAN of pairwise change across satellites, and
    # always against quiet->quiet controls, because the quiet baseline is itself
    # large (11-21 m/hour on DOY 129).
    print()
    print("BETWEEN-HOUR DISTORTION  (pairwise between-satellite change; datum-free)")
    hour_med = defaultdict(lambda: defaultdict(list))
    for tow, st, _ in masked:
        h = int((tow - DAY_ANCHOR_TOW) // 3600)
        for s, d in st["dev"].items():
            hour_med[h][s].append(d)
    lev = {h: {s: statistics.median(v) for s, v in d.items() if len(v) > 50}
           for h, d in hour_med.items()}

    def span(ha, hb):
        A, B = lev.get(ha, {}), lev.get(hb, {})
        com = sorted(set(A) & set(B))
        if len(com) < 5:
            return None
        ref = com[0]
        ch = [(B[s] - B[ref]) - (A[s] - A[ref]) for s in com[1:]]
        return len(com), max(ch) - min(ch), min(ch), max(ch)

    hrs = sorted(lev)
    consecutive = [(hrs[i], hrs[i + 1]) for i in range(len(hrs) - 1)]
    spans = [(a, b, span(a, b)) for a, b in consecutive]
    good = [s[2][1] for s in spans if s[2]]
    baseline = statistics.median(good) if good else float("nan")
    print(f"  consecutive-hour pairwise-change span (median over the day) = {baseline:.2f} m")
    print(f"  {'hours':>12}{'nsat':>6}{'span_m':>10}{'min':>9}{'max':>9}   flag")
    for a, b, r in spans:
        if not r:
            continue
        n, sp, lo, hi = r
        flag = "  <== 2x+ baseline" if sp > 2.0 * baseline else ""
        print(f"  {f'hr{a:02d}->hr{b:02d}':>12}{n:>6}{sp:>10.2f}{lo:>9.2f}{hi:>9.2f}{flag}")
    print("  (a large span = genuine multi-satellite redistribution; a large uniform")
    print("   min/max offset with a SMALL span = one satellite moved, everything else held)")

    # ---- discontinuity flags ---------------------------------------------
    print()
    print("CODE-BIAS DISCONTINUITY FLAGS  (disc_counter bumps)")
    if not disc_all:
        print("  none recorded -- NB network.code_bias_disc_jump_test defaults FALSE")
        print("  (GnssConfigLoadNetwork.cpp:1814-1816), so real steps are NOT flagged.")
    else:
        by_sat = Counter(s for _, s, _, _ in disc_all)
        print(f"  {len(disc_all)} bumps; top sats: {dict(by_sat.most_common(5))}")
        # Per-hour rate, normalised per (sat,signal) channel.  The run-total alone
        # cannot distinguish cold-start code-bias initialisation -- where every
        # channel legitimately bumps for the first few minutes -- from steady-state
        # disc CHURN, which disc_threshold_frac_cycles explicitly does NOT fix
        # ("it equalizes SEMANTICS only"); the PB emit filter owns churn.  A rate
        # that decays toward zero is convergence; a flat rate is churn.
        chans = len({(s, g) for _, s, g, _ in disc_all}) or 1
        per_hr = defaultdict(int)
        eps_hr = defaultdict(set)
        for tow, _, _, _ in disc_all:
            per_hr[int((tow - DAY_ANCHOR_TOW) // 3600)] += 1
        for tow, _, _ in rows:
            eps_hr[int((tow - DAY_ANCHOR_TOW) // 3600)].add(tow)
        print(f"  {chans} distinct (sat,signal) channels bumped")
        print(f"  {'hr':>4}{'bumps':>9}{'epochs':>8}{'bumps/ep':>10}{'per_chan/ep':>13}")
        # Iterate the hours that have EPOCHS, not the hours that have bumps.  An hour
        # with zero bumps is the best possible outcome; omitting its row makes it look
        # like missing data instead, which is the "0 driven / 0 folded" ambiguity in
        # reverse -- a reader cannot tell a clean hour from an absent trace.
        for h in sorted(eps_hr):
            ne = len(eps_hr[h]) or 1
            nb = per_hr.get(h, 0)
            print(f"  {h:>4}{nb:>9}{ne:>8}{nb / ne:>10.1f}{nb / ne / chans:>13.3f}")
        print("  (per_chan/ep ~1.0 = every channel bumping every epoch; decay = converging,")
        print("   flat = steady-state churn -- read against the PB emit filter, not the")
        print("   disc threshold, which cannot fix churn by construction)")

    if args.csv and hourly:
        # One file per constellation -- the tables are in different gauges and must
        # not be concatenated into a single sheet.
        stem, ext = os.path.splitext(args.csv)
        out = args.csv if args.sys.upper() == sysid else f"{stem}_{sysid}{ext or '.csv'}"
        with open(out, "w", newline="") as fh:
            w = csv.DictWriter(fh, fieldnames=list(hourly[0].keys()))
            w.writeheader()
            w.writerows(hourly)
        print(f"\nhourly table -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

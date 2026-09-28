#!/usr/bin/env python3
"""Grade the FC-1 split-cadence fast clock against the full solve.

Two modes, both grading datum-invariant quantities only (standing invariant #5: raw
wire clocks confound benign frame drift with real defects, so every comparison below has
the per-epoch common mode removed before anything is measured):

``shadow``
    The INC-2 gate.  A shadow run computes the fast tick on full epochs and discards it,
    so for every epoch there is both a fast delta and the full solve's own delta over the
    same interval.  Grades fast vs full directly.  Passes when the common-mode-removed RMS
    sits at or below a small multiple of the producer's own clock sigma.

``split``
    The INC-4 decisive gate.  Two runs of the same build over the same data:

      REF   : ``epoch_decimation_n = 1``, a full solve at every epoch (today's mode).
      SPLIT : ``epoch_decimation_n = N`` with ``fast_clock`` on, full solves every N
              epochs and clocks-only ticks in between.

    At every epoch that SPLIT covered with a tick (not a full solve), compare against
    REF's full-solve clock, after removing the per-satellite arm-frame offset measured at
    the bracketing common full epoch and held across the era:

      offset     = SPLIT full c0(base) - REF c0(base),  base = latest common full <= t
      fast error = (SPLIT anchor c0 + fast delta) - REF c0 - offset
      ZOH  error = (SPLIT anchor c0)              - REF c0 - offset  (what a consumer gets today)

    The alignment is not cosmetic: the two arms' full solves realize independent frame
    random walks (measured 317.1 cm SD-RMS apart at common epochs on the 2026-07-30 runs), so
    without it both errors saturate at the inter-arm divergence
    (sqrt(317.1^2 + 76.9^2) = 326.3 against the 326.4 measured), and the maximum
    measurable improvement factor was 1.03x against a 5x gate, at any churn level.  With
    the anchor-held offset removed, the ZOH error reduces algebraically to REF's own step
    across the era (the true hold error) and the fast error to the tick's miss of that
    step, which is the comparison the gate was specified over.  The removed divergence is
    printed on every run so a saturated grade cannot pass unnoticed.

    Passes when ``SD-RMS(fast) <= (1/5) * SD-RMS(ZOH)``.  Beating ZOH by 5x is the bar
    because ZOH is already the MMSE predictor for a random-walk clock given the slow
    solve (the C1 card measured the emitted c0 as a random walk with sigma_w = 0.157 m per
    30 s and an optimal rate-shrinkage of exactly zero).  A fast tier that merely ties ZOH
    has bought nothing but complexity.

Both modes report the screen rate and, in split mode, the per-satellite spread, because a
headline RMS that passes while one satellite carries all the error is not a pass.
"""

from __future__ import annotations

import argparse
import bisect
import math
import statistics
import sys
from collections import defaultdict


# --------------------------------------------------------------------------- parsing

def parse_products(path):
    """products.txt -> {tow: {sat: c0_m}}, in file order.

    Only ``E`` (epoch) and ``C`` (satellite clock) records matter here.  ``G`` records are
    a wall-clock generation stamp and are deliberately ignored: they differ between any
    two runs and are not part of the estimate.
    """
    epochs = {}
    fast = {}
    tow = None
    ftow = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\r\n")
            if not line or line[0] == "#":
                continue
            f = line.split(",")
            if f[0] == "E" and len(f) >= 3:
                # Bucket to the millisecond.  The product file carries the raw RINEX time
                # arithmetic (259259.99999642372), while the trace formats to 3 decimals;
                # keying on the exact float would silently find zero overlap and report a
                # wiring failure that is really a formatting mismatch.
                tow = round(float(f[2]), 3)
                ftow = None
                epochs.setdefault(tow, {})
            elif f[0] == "C" and tow is not None and len(f) >= 3:
                try:
                    epochs[tow][f[1]] = float(f[2])
                except ValueError:
                    pass
            elif f[0] == "F" and len(f) >= 5:
                # FC-1 fast epoch: tow, then the anchor full solve it is a delta on.
                ftow = round(float(f[2]), 3)
                fast[ftow] = {"anchor": round(float(f[4]), 3), "sats": {}}
            elif f[0] == "K" and ftow is not None and len(f) >= 3:
                try:
                    sig = 0.0
                    for tok in f[3:]:
                        if tok.startswith("sigk="):
                            sig = float(tok[5:])
                    fast[ftow]["sats"][f[1]] = (float(f[2]), sig)
                except ValueError:
                    pass
    return epochs, fast


def parse_fast_trace(path):
    """network_fast_clock.csv -> (ticks, per_sat)

    ticks:   [{tow, dt_s, shadow, n_los, n_screened, n_sat_est, emitted}]
    per_sat: {tow: {sat: (dc_m, sigma_m)}}
    """
    ticks, per_sat = [], defaultdict(dict)
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        head = fh.readline()
        if not head.startswith("week,tow,kind"):
            raise SystemExit("unexpected fast-clock trace header: %r" % head[:80])
        for raw in fh:
            f = raw.rstrip("\r\n").split(",")
            if len(f) < 12:
                continue
            kind, tow = f[2], round(float(f[1]), 3)   # same millisecond bucketing
            if kind == "T":
                ticks.append({
                    "tow": tow, "dt_s": float(f[3]), "shadow": f[4] == "1",
                    "n_los": int(f[5]), "n_screened": int(f[6]),
                    "n_sat_est": int(f[7]), "emitted": f[8] == "1",
                })
            elif kind == "S" and f[9]:
                per_sat[tow][f[9]] = (float(f[10]), float(f[11]))
    return ticks, per_sat


# --------------------------------------------------------------------------- stats

def sd_stats(errors_by_sat):
    """Common-mode-removed statistics over one epoch's {sat: error}.

    Removing the per-epoch median is the single-realization datum-invariance step: an
    epoch-wide constant is a frame offset, not an error, and grading it would report
    benign datum drift as fast-clock failure.  The median, not the mean, so one bad
    satellite cannot define the frame it is then judged against.
    """
    if len(errors_by_sat) < 2:
        return None
    med = statistics.median(errors_by_sat.values())
    return {s: e - med for s, e in errors_by_sat.items()}


def rms(vals):
    vals = [v for v in vals if v is not None and math.isfinite(v)]
    if not vals:
        return float("nan")
    return math.sqrt(sum(v * v for v in vals) / len(vals))


# --------------------------------------------------------------------------- modes

def grade_shadow(products, ticks, per_sat, sigma_mult):
    print("=== FC-1 shadow grade (INC-2 gate a) ===")
    print("fast delta vs the full solve's OWN delta over the same interval,")
    print("common mode removed per epoch (invariant #5).\n")

    tows = sorted(products)
    prev_of = {tows[i]: tows[i - 1] for i in range(1, len(tows))}

    all_sd, all_sigma, n_ep = [], [], 0
    per_sat_abs = defaultdict(list)
    for t in sorted(per_sat):
        if t not in prev_of:
            continue
        t0 = prev_of[t]
        full, fast = {}, {}
        for sat, (dc, sig) in per_sat[t].items():
            if sat in products.get(t, {}) and sat in products.get(t0, {}):
                full[sat] = products[t][sat] - products[t0][sat]
                fast[sat] = dc
                all_sigma.append(sig)
        if len(full) < 2:
            continue
        err = {s: fast[s] - full[s] for s in full}
        sd = sd_stats(err)
        if sd is None:
            continue
        n_ep += 1
        for s, v in sd.items():
            all_sd.append(v)
            per_sat_abs[s].append(abs(v))

    if not all_sd:
        print("NO COMPARABLE EPOCHS — the shadow trace and the product file do not overlap.")
        print("That is a wiring failure, not a quiet result.")
        return 1

    r = rms(all_sd)
    mean_sigma = statistics.fmean(all_sigma) if all_sigma else float("nan")
    worst = sorted(((statistics.fmean(v), s) for s, v in per_sat_abs.items()), reverse=True)[:5]

    scr = sum(t["n_screened"] for t in ticks)
    tot = sum(t["n_los"] + t["n_screened"] for t in ticks)
    print(f"epochs graded          : {n_ep}")
    print(f"satellites             : {len(per_sat_abs)}")
    print(f"SD-RMS(fast - full)    : {r*100:.3f} cm")
    print(f"mean formal sigma      : {mean_sigma*100:.3f} cm")
    print(f"screen rate            : {100.0*scr/tot if tot else 0:.2f}% of LOS "
          f"({scr}/{tot})")
    print("worst satellites (mean |SD err|):")
    for m, s in worst:
        print(f"    {s}  {m*100:.3f} cm")

    bar = sigma_mult * mean_sigma
    ok = math.isfinite(r) and math.isfinite(bar) and r <= bar
    print(f"\nGATE: SD-RMS {r*100:.3f} cm  vs  {sigma_mult:g} x formal sigma = {bar*100:.3f} cm"
          f"  -> {'PASS' if ok else 'FAIL'}")
    if tot and 100.0 * scr / tot >= 1.0:
        print("NOTE: screen rate >= 1% — above the quiet-data expectation; check whether the"
              " window is genuinely eventful before tuning k.")
    return 0 if ok else 1


def grade_split(ref_products, split_products, split_fast, ticks, ratio_bar):
    print("=== FC-1 split-cadence grade (INC-4 decisive gate) ===")
    print("At every FAST epoch on the wire: fast vs REF, and ZOH vs REF, with the")
    print("per-satellite arm-frame offset (SPLIT full - REF full, held from the")
    print("bracketing common full epoch) removed from BOTH, then the per-epoch common")
    print("mode removed.  Graded on the EMITTED K values — what a consumer actually")
    print("receives, including the cdot*dt drift term — not on an intermediate.\n")

    # The two arms realize independent frame random walks; unaligned, both errors
    # saturate at the inter-arm divergence (see the module docstring for the measured
    # instance).  The offset is estimated at the latest common full epoch at or before
    # the tick: in a healthy split run that is the tick's own anchor, which makes the
    # aligned ZOH error exactly REF's step across the era.
    common_fulls = sorted(t for t, sats in split_products.items() if sats and t in ref_products)
    if not common_fulls:
        print("NO COMMON FULL EPOCHS between the arms — the arm-frame offset cannot be")
        print("estimated, and an unaligned grade measures only inter-arm divergence.")
        print("Check that both product files cover the same window.")
        return 1

    fast_sd, zoh_sd, n_ep = [], [], 0
    per_sat_fast = defaultdict(list)
    div_sd = []   # the offsets actually removed, SD'd per epoch, and reported below

    for t in sorted(split_fast):
        ref_ep = ref_products.get(t)
        if not ref_ep:
            continue
        anchor = split_fast[t]["anchor"]
        anchor_ep = split_products.get(anchor, {})
        if not anchor_ep:
            continue
        i = bisect.bisect_right(common_fulls, t) - 1
        if i < 0:
            continue
        base = common_fulls[i]
        off = {sat: split_products[base][sat] - ref_products[base][sat]
               for sat in split_products[base] if sat in ref_products[base]}

        f_err, z_err = {}, {}
        for sat, (c0_fast, _sig) in split_fast[t]["sats"].items():
            if sat not in ref_ep or sat not in anchor_ep or sat not in off:
                continue
            f_err[sat] = c0_fast - ref_ep[sat] - off[sat]
            z_err[sat] = anchor_ep[sat] - ref_ep[sat] - off[sat]
        if len(f_err) < 2:
            continue
        f_sd, z_sd = sd_stats(f_err), sd_stats(z_err)
        if f_sd is None or z_sd is None:
            continue
        n_ep += 1
        for s, v in f_sd.items():
            fast_sd.append(v)
            per_sat_fast[s].append(abs(v))
        zoh_sd.extend(z_sd.values())
        o_sd = sd_stats({s: off[s] for s in f_err})
        if o_sd is not None:
            div_sd.extend(o_sd.values())

    if not fast_sd:
        print("NO COMPARABLE TICK EPOCHS — check that REF covers the SPLIT tick epochs.")
        return 1

    rf, rz, rdiv = rms(fast_sd), rms(zoh_sd), rms(div_sd)
    scr = sum(t["n_screened"] for t in ticks)
    tot = sum(t["n_los"] + t["n_screened"] for t in ticks)
    worst = sorted(((statistics.fmean(v), s) for s, v in per_sat_fast.items()), reverse=True)[:5]

    print(f"fast epochs graded     : {n_ep}")
    print(f"arm-frame divergence   : SD-RMS {rdiv*100:.3f} cm REMOVED from both errors")
    print("                         (unaligned grading saturates at this level and can")
    print("                         show at most ~1x however good the ticks are)")
    print(f"SD-RMS(fast vs REF)    : {rf*100:.3f} cm")
    print(f"SD-RMS(ZOH  vs REF)    : {rz*100:.3f} cm   <- what a consumer gets today")
    print(f"improvement factor     : {rz/rf if rf > 0 else float('inf'):.2f}x")
    print(f"screen rate            : {100.0*scr/tot if tot else 0:.2f}% of LOS")
    print("worst satellites (mean |SD err|, fast):")
    for m, s in worst:
        print(f"    {s}  {m*100:.3f} cm")

    ok = rf > 0 and rf <= rz / ratio_bar
    print(f"\nGATE: fast <= ZOH/{ratio_bar:g}  ({rf*100:.3f} <= {rz/ratio_bar*100:.3f} cm)"
          f"  -> {'PASS' if ok else 'FAIL'}")
    if not ok and rf < rz:
        print("NOTE: the fast tier BEATS ZOH but not by the required margin.  Report that"
              " honestly — it is a NO-GO for the card as specified, not a tuning miss.")
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["shadow", "split"])
    ap.add_argument("--products", required=True,
                    help="shadow: the run's products.txt; split: the SPLIT arm's")
    ap.add_argument("--trace", required=True, help="network_fast_clock.csv")
    ap.add_argument("--ref-products", help="split mode: the REF arm's products.txt")
    ap.add_argument("--sigma-mult", type=float, default=2.0,
                    help="shadow gate: multiple of the formal sigma (default 2)")
    ap.add_argument("--ratio", type=float, default=5.0,
                    help="split gate: required ZOH/fast ratio (default 5)")
    a = ap.parse_args(argv)

    ticks, per_sat = parse_fast_trace(a.trace)
    products, fast = parse_products(a.products)

    if a.mode == "shadow":
        if not per_sat:
            print("The trace has no per-satellite rows — set"
                  " network.fast_clock.trace_sat_stride to 1.  Without them there is"
                  " nothing to grade.")
            return 1
        return grade_shadow(products, ticks, per_sat, a.sigma_mult)
    if not a.ref_products:
        ap.error("split mode needs --ref-products")
    if not fast:
        print("The SPLIT product file contains no F/K fast records — the tier emitted"
              " nothing.  That is inert wiring, not a quiet result: check the FC-1"
              " run-end counters before grading anything.")
        return 1
    ref, _ = parse_products(a.ref_products)
    return grade_split(ref, products, fast, ticks, a.ratio)


if __name__ == "__main__":
    sys.exit(main())

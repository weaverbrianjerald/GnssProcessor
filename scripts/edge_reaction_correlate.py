"""Consumer-side datum-break reaction census, and the v1-contract threshold audit.

Companion to product_step_census.py: that script reports what the producer shipped;
this one reports what the consumer did about it, and whether the two count the same
events.

Three readouts (docs/HANDOFF_edge_response_queue.md edge 1):

  1. Wire and threshold split.  Mid-arc phase-bias steps, classified against both
     thresholds: the producer's per-lambda gate (network.disc_threshold_frac_cycles)
     and the flat kDefaultDiscThresholdM the replay consumer falls back to when the
     product is a v1 file with no S contract line (written before card OWD, when
     network.serialize_sbasis_contract was off: NetworkProductReader::makeReplayEmitter
     returns the default emitter, and since card OWD a replay admits such a wire only
     with network.replay_allow_undeclared_wire).  The two thresholds disagree in
     either direction depending on band, so the split is reported per direction and
     per band.

  2. Reactions.  What RealTimeEngineSsrBreaks actually fired, parsed from rt_engine.log.
     logging.enable_rt_engine defaults to false, so an empty rt_engine.log means the
     logger was off, not that the reaction path is inert.  The toggle is solve-neutral
     (verified byte-identical on closed_loop_trace.csv), so re-running an arm with it
     armed is a free way to make the layer visible.

  3. Excursion correlation.  Whether those reactions cluster at the onsets of the
     consumer's own position excursions, or merely occur at base rate.  Base-rate
     enrichment is the grade: a reaction channel that fires at 1.0x inside excursions
     is not the channel driving them.

Usage:
  python scripts/edge_reaction_correlate.py <products.txt> <rt_engine.log> <closed_loop_trace.csv>
  python scripts/edge_reaction_correlate.py <run_dir> <replay_run_dir>
     [--pb-step 0.10] [--frac 0.25] [--flat 0.05] [--exc 20.0]
"""
import argparse
import collections
import os
import re
import sys

if hasattr(sys.stdout, "reconfigure"):        # the tables carry lambda / x / degree signs
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

C = 299792458.0
F0 = 10.23e6
# RINEX band digit -> nominal carrier wavelength (m).  Matches SignalBand's classifier
# for the GPS/Galileo bands this wire carries; a band not listed falls back to the flat
# gate, exactly as NetworkProductEmitter::discThresholdM does on lambda == 0.
LAMBDA = {'1': C / (154 * F0), '2': C / (120 * F0), '5': C / (115 * F0),
          '6': C / (125 * F0), '7': C / (118 * F0), '8': C / (116.5 * F0)}

RE_RESET = re.compile(
    r"\[(\d+), ([\d.]+) .*?SSR phase-bias discontinuity: reset (\S+) (\S+)")
OTHER_REACTIONS = [
    ("phase-bias freshness flip", "SSR phase-bias freshness flip"),
    ("code-bias disc",            "SSR code-bias discontinuity"),
    ("code-bias freshness flip",  "SSR code-bias freshness flip"),
    ("clock disc",                "SSR clock discontinuity"),
    ("generation-id change",      "solution-generation id changed"),
    ("producer status change",    "Network producer status"),
]


def wire_steps(path):
    """Mid-arc phase-bias steps: (tow, sat, signal, |step|), consecutive epochs only."""
    prev_val, prev_tow, steps, tow = {}, {}, [], None
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("E,"):
                tow = round(float(line.split(",")[2]))
            elif line.startswith("P,") and tow is not None:
                p = line.rstrip("\n").split(",")
                if len(p) < 4:
                    continue
                key = (p[1], p[2])
                try:
                    bias = float(p[3])
                except ValueError:
                    continue
                if key in prev_val and prev_tow[key] == tow - 30:
                    d = abs(bias - prev_val[key])
                    if d > 0:
                        steps.append((tow, p[1], p[2], d))
                prev_val[key], prev_tow[key] = bias, tow
    return steps


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--pb-step", type=float, default=0.10)
    ap.add_argument("--frac", type=float, default=0.25,
                    help="producer network.disc_threshold_frac_cycles (0 = flat only)")
    ap.add_argument("--flat", type=float, default=0.05,
                    help="consumer fallback kDefaultDiscThresholdM")
    ap.add_argument("--exc", type=float, default=20.0)
    ap.add_argument("--onset-pre", type=float, default=120.0)
    ap.add_argument("--onset-post", type=float, default=60.0)
    a = ap.parse_args()

    if len(a.paths) == 2:
        prod = os.path.join(a.paths[0], "products.txt")
        rtlog = os.path.join(a.paths[1], "logs", "rt_engine.log")
        trace = os.path.join(a.paths[1], "logs", "closed_loop_trace.csv")
    elif len(a.paths) == 3:
        prod, rtlog, trace = a.paths
    else:
        ap.error("give <products.txt> <rt_engine.log> <closed_loop_trace.csv>, "
                 "or <producer_run_dir> <replay_run_dir>")

    def thr_prod(sig):
        if a.frac <= 0:
            return a.flat
        lam = LAMBDA.get(sig[1] if len(sig) > 1 else "", 0.0)
        return a.frac * lam if lam else a.flat

    # ---- 1. wire ---------------------------------------------------------
    steps = wire_steps(prod)
    big = [s for s in steps if s[3] > a.pb_step]
    pf = [s for s in steps if s[3] > thr_prod(s[2])]
    cf = [s for s in steps if s[3] > a.flat]
    miss = [s for s in steps if s[3] > thr_prod(s[2]) and s[3] <= a.flat]
    extra = [s for s in steps if s[3] <= thr_prod(s[2]) and s[3] > a.flat]

    ver = "v2 (S contract present)" if a.frac <= 0 else "v1 fallback assumed"
    print("=" * 76)
    print(f"1. WIRE — mid-arc phase-bias steps; producer {a.frac}·lambda vs consumer "
          f"flat {a.flat} m  [{ver}]")
    print("=" * 76)
    print(f"  mid-arc steps > {a.pb_step} m (product_step_census tie-out) : {len(big)}")
    print(f"  fire at PRODUCER threshold ({a.frac}·lambda)               : {len(pf)}")
    print(f"  fire at CONSUMER fallback  (flat {a.flat} m)                 : {len(cf)}")
    print(f"  -> producer flags, consumer MISSES                     : {len(miss)}"
          f"   by band {dict(collections.Counter(s[2][1] for s in miss))}")
    print(f"  -> consumer flags, producer did NOT (SPURIOUS)         : {len(extra)}"
          f"   by band {dict(collections.Counter(s[2][1] for s in extra))}")
    if not os.path.exists(rtlog):
        print(f"\n  (no rt_engine.log at {rtlog} — stopping after the wire audit)")
        return 0

    # ---- 2. reactions ----------------------------------------------------
    resets, txt = [], open(rtlog, errors="replace").read()
    for line in txt.splitlines():
        m = RE_RESET.search(line)
        if m:
            resets.append((round(float(m.group(2))), m.group(3), m.group(4)))
    print()
    print("=" * 76)
    print("2. CONSUMER — reactions that actually fired (RealTimeEngineSsrBreaks)")
    print("=" * 76)
    if not txt.strip():
        print("  rt_engine.log is EMPTY — logging.enable_rt_engine was OFF for this run.")
        print("  That is a LOGGER state, not an inert reaction path.  Re-run with")
        print("  \"logging\": { \"enable_rt_engine\": true } (solve-neutral) to see it.")
    print(f"  phase-bias disc resets         : {len(resets)}"
          f"  on {len(set(r[0] for r in resets))} distinct epochs")
    print(f"  distinct satellites            : {len(set(r[1] for r in resets))}")
    print(f"  by signal                      : "
          f"{dict(collections.Counter(r[2] for r in resets))}")
    for label, pat in OTHER_REACTIONS:
        print(f"  {label:<30} : {txt.count(pat)}")
    seen = set((r[1], r[2]) for r in resets)
    vis = [s for s in cf if (s[1], s[2]) in seen]
    if vis:
        print(f"  wire steps > {a.flat} m on tracked (sat,signal) keys : {len(vis)}"
              f"   -> resets/steps {len(resets)/len(vis):.2f}")

    # ---- 3. excursions ---------------------------------------------------
    rows = []
    with open(trace, errors="replace") as f:
        next(f)
        for line in f:
            p = line.rstrip("\n").split(",")
            if len(p) > 8:
                rows.append((round(float(p[1])), float(p[8])))
    if not rows:
        return 0
    tows = [r[0] for r in rows]
    wins, cur = [], None
    for t, e in rows:
        if e > a.exc:
            cur = [t, t] if cur is None else [cur[0], t]
        elif cur:
            wins.append(tuple(cur))
            cur = None
    if cur:
        wins.append(tuple(cur))

    at = collections.Counter(r[0] for r in resets)
    base = len(resets) / len(rows)
    print()
    print("=" * 76)
    print(f"3. EXCURSIONS (err3d > {a.exc} m) vs the reactions — enrichment IS the grade")
    print("=" * 76)
    print(f"  base reset rate {base:.4f}/epoch ({len(resets)} over {len(rows)} ep)")
    print(f"  {'window (h)':>15} {'ep':>5} {'peak m':>9} {'onset':>7} {'enrich':>8} {'inside':>7}")
    t0, tot = tows[0], 0
    for s, e in wins:
        nep = sum(1 for t in tows if s <= t <= e)
        peak = max(v for t, v in rows if s <= t <= e)
        onset = sum(at.get(t, 0) for t in tows
                    if s - a.onset_pre <= t <= s + a.onset_post)
        nons = sum(1 for t in tows if s - a.onset_pre <= t <= s + a.onset_post)
        exp = base * nons
        inw = sum(at.get(t, 0) for t in tows if s <= t <= e)
        tot += inw
        print(f"  {(s-t0)/3600:6.2f}–{(e-t0)/3600:5.2f} {nep:5d} {peak:9.1f} "
              f"{onset:7d} {onset/exp if exp else 0:7.2f}x {inw:7d}")
    nex = sum(1 for _, e in rows if e > a.exc)
    exp_in = base * nex
    print(f"  TOTAL {len(wins)} windows, {nex} ep > {a.exc} m ({100*nex/len(rows):.1f}%); "
          f"resets inside {tot}/{len(resets)}, expected {exp_in:.1f} "
          f"-> {tot/exp_in if exp_in else 0:.2f}x")
    print("  Reading: ~1.0x inside AND 0.00x at the onsets means this reaction channel")
    print("  is NOT the one driving the excursions — look for an event class with no")
    print("  wire representation (refsat_reset / col_removed / sbc_engage / eq34_reattach).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Phase A of SPEC_withhold_era_release §12: replay the level-health classifier
over a recorded run's own artifacts — zero C++ — and grade it against the
labeled truth set (era-3's 7 toxic / era-2's 16 clean recovered epochs).

Implements §12.2's CAUSAL limb exactly:
  d_causal(t) = a00(t) − median{ last K=--ref-epochs a00 of class-`ok` emitted
                epochs with content age ≤ --ref-max-age s }
  classes: c = T2 emitted_contaminated event row on the epoch (the existing
               trigger criterion, replayed from the events trace);
           r = within --relevel-hold s after a mass-re-level edge (replay
               detector: first emitted epoch after a withhold streak of
               ≥ --cliff-epochs consecutive withheld epochs — the 900 s
               cliff signature);
           s = |d_causal| > --dev-tecu with a valid reference;
           na = reference starved (< K valid entries in the age window).
  c/r/s/na epochs are all EXCLUDED from the reference (self-exclusion is
  load-bearing: without it the reference tracks the dip and the recovery
  false-flags).

Inputs (run working dir with logs/): network_vtec_sh.csv (a00 = n=0,m=0,trig=C),
network_epoch_status.csv (dispositions), network_events.csv (T2 rows).
"""

import argparse
import csv
from collections import deque
from pathlib import Path


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])


def load_a00(run_dir: Path):
    out = {}
    with open(run_dir / "logs" / "network_vtec_sh.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["n"] == "0" and r["m"] == "0" and r["trig"] == "C":
                out[float(r["tow"])] = float(r["value_tecu"])
    return out


def load_dispositions(run_dir: Path):
    """tow -> emitted?  (epoch_status has one row per processed epoch)."""
    emitted, withheld = [], set()
    with open(run_dir / "logs" / "network_epoch_status.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            tow = float(r["tow"])
            status = (r.get("status") or r.get("disposition") or "").lower()
            if "emit" in status:
                emitted.append(tow)
            else:
                withheld.add(tow)
    return sorted(emitted), withheld


def load_t2_flags(run_dir: Path):
    flagged = set()
    with open(run_dir / "logs" / "network_events.csv", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["tier"] == "T2_within_epoch" and r["event"] == "emitted_contaminated":
                flagged.add(float(r["tow"]))
    return flagged


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir", type=Path)
    ap.add_argument("--ref-epochs", type=int, default=5)
    ap.add_argument("--ref-max-age", type=float, default=900.0)
    ap.add_argument("--dev-tecu", type=float, default=6.0)
    ap.add_argument("--relevel-hold", type=float, default=900.0)
    ap.add_argument("--cliff-epochs", type=int, default=25)
    ap.add_argument("--label-toxic", type=str, default="",
                    help="comma-separated TOWs expected to flag `s` (truth set)")
    ap.add_argument("--label-clean", type=str, default="",
                    help="comma-separated TOWs expected NOT to flag `s`")
    args = ap.parse_args()

    a00 = load_a00(args.run_dir)
    emitted, withheld = load_dispositions(args.run_dir)
    t2 = load_t2_flags(args.run_dir)

    # r-edges: first emitted epoch after >= cliff-epochs consecutive withheld.
    all_tows = sorted(set(emitted) | withheld)
    r_edges, streak = [], 0
    for tow in all_tows:
        if tow in withheld:
            streak += 1
        else:
            if streak >= args.cliff_epochs:
                r_edges.append(tow)
            streak = 0

    refs = deque()  # (tow, a00) of `ok` epochs
    rows, census = [], {"ok": 0, "na": 0}
    for tow in emitted:
        if tow not in a00:
            continue
        v = a00[tow]
        while refs and tow - refs[0][0] > args.ref_max_age:
            refs.popleft()
        d = None
        if len(refs) >= args.ref_epochs:
            d = v - median([x[1] for x in refs])
        flags = ""
        if tow in t2:
            flags += "c"
        if any(0.0 <= tow - e <= args.relevel_hold for e in r_edges):
            flags += "r"
        if d is not None and abs(d) > args.dev_tecu:
            flags += "s"
        # Reference maintenance: c/r/s epochs are excluded (self-exclusion is
        # load-bearing), but a WARMING epoch (d None for want of references)
        # with no c/r flag MUST seed the reference — §12.2 as first written
        # excluded `na` too, which is a bootstrap deadlock (the reference can
        # never fill).  Phase A finding; the card is amended to match.
        if not flags:
            refs.append((tow, v))
            if len(refs) > args.ref_epochs:
                refs.popleft()
        if d is None and not flags:
            flags = "na"
        elif not flags:
            flags = "ok"
        census[flags] = census.get(flags, 0) + 1
        rows.append((tow, v, d, flags))

    print(f"== {args.run_dir}  ({len(rows)} emitted epochs classified; "
          f"r-edges at {['%.0f' % e for e in r_edges]})")
    print("  class census: " + ", ".join(f"{k} {v}" for k, v in sorted(census.items())))
    n_s = sum(1 for _, _, _, f in rows if "s" in f)
    print(f"  `s` epochs total: {n_s}")
    for tow, v, d, f in rows:
        if "s" in f:
            print(f"    s  tow {tow:.0f}  a00 {v:7.2f}  d {d:+7.2f}  flags={f}")

    for name, tows, want_s in [("TOXIC", args.label_toxic, True),
                               ("CLEAN", args.label_clean, False)]:
        if not tows:
            continue
        want = [float(t) for t in tows.split(",")]
        hit = [t for t in want for tow, _, _, f in rows
               if tow == t and (("s" in f) == want_s)]
        miss = [t for t in want if t not in hit]
        print(f"  {name} truth set: {len(hit)}/{len(want)} "
              f"{'flagged s' if want_s else 'passed (no s)'}"
              + (f"  MISSES: {miss}" if miss else ""))
        for t in miss:
            for tow, v, d, f in rows:
                if tow == t:
                    print(f"    miss detail: tow {tow:.0f} a00 {v:.2f} "
                          f"d {'None' if d is None else '%+.2f' % d} flags={f}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""RTB-7(d) RS card BR-2, branch-faithfulness instrument: diff two carry blobs.

Between-sat SD of CLK is an omnibus readout at the end of the station-layer to
phase-conduit to clock chain; it cannot attribute. The Card-4 carry blob is the
branch, and a carried DD is both datum-invariant (a double difference) and
integer-meaningful, so comparing blobs grades the thing the feature restores.

Id translation is central to this comparison. Vertex and group ids are
assigned in first-seen order per run and stored per snapshot (`SV` / `GG`
records), so two runs that saw satellites in a different order carry different
ids for the same satellite. Comparing raw ids is a silent mis-binding trap.
Every chord is therefore translated to
``(constellation, signal, rx_roster_index, sat_name)`` using each file's own
maps before any comparison. The receiver vertex is the station roster index
(stable, since roster order is inside the config fingerprint); the satellite
vertex is whichever endpoint appears in that file's `SV` map.

Reports, per file pair:
  * n common chords, and set churn (only-in-A / only-in-B)
  * |Δz| distribution in cycles (median / p95)
  * branch-agree: fraction with |Δz| < tol (same real-valued branch)
  * branch-flip: count with |Δz - round(Δz)| < tol and round(Δz) != 0
    (an integer-cycle jump: the branch moved to a different integer)
Thresholds are provisional; the full distribution is printed so they can be
judged.

Usage:
    python scripts/diff_carry_blobs.py A.snap B.snap [--tol 0.05] [--top 10]
"""
import argparse, sys
from collections import defaultdict

# Static GPS wavelengths (m) by band digit: enough for this card (GPS-only runs).
_C = 299792458.0
_FREQ = {"1": 1575.42e6, "2": 1227.60e6, "5": 1176.45e6,
         "6": 1278.75e6, "7": 1207.14e6, "8": 1191.795e6}


def lam_for(signal):
    """Wavelength for a RINEX signal like 'L1C' / 'L2W' / '1C', by band digit."""
    for ch in signal:
        if ch in _FREQ:
            return _C / _FREQ[ch]
    return None


def parse_snapshot(path):
    """-> (sat_by_vertex, key_by_group, components[list of (group, [(u,v)], [z])])."""
    sat_by_vertex, key_by_group, comps = {}, {}, []
    in_carry = False
    it = iter(open(path, encoding="utf-8", errors="replace").read().splitlines())
    for line in it:
        if not in_carry:
            if line.startswith("SV,"):
                f = line.split(",")
                if len(f) == 3:
                    sat_by_vertex[int(f[2])] = f[1]
            elif line.startswith("GG,"):
                f = line.split(",")
                if len(f) == 4:
                    key_by_group[int(f[3])] = (f[1], f[2])
            elif line.startswith("CARRY,"):
                in_carry = True
            continue
        if line == "CEND":
            in_carry = False
            continue
        if line.startswith("B,"):
            f = line.split(",")
            grp = int(f[1]); zdim = int(f[5]); nch = int(f[6])
            z, chords = [], []
            for nxt in it:
                if nxt.startswith("Z,"):
                    z = [float(x) for x in nxt.split(",")[1:]]
                elif nxt.startswith("P,"):
                    continue                       # covariance not needed for the branch
                elif nxt.startswith("H,"):
                    v = [int(x) for x in nxt.split(",")[1:]]
                    chords = list(zip(v[0::2], v[1::2]))
                elif nxt.startswith("X,"):
                    continue
                elif nxt.startswith("A,"):
                    break                          # last row of this component
            comps.append((grp, chords, z))
    return sat_by_vertex, key_by_group, comps


def keyed_chords(path):
    """-> {(const, signal, rx_idx, sat_name): z_cycles}, plus a no-lambda count."""
    sat_by_vertex, key_by_group, comps = parse_snapshot(path)
    out, nolam = {}, 0
    for grp, chords, z in comps:
        gk = key_by_group.get(grp)
        if gk is None:
            continue                                # untranslatable group -> skip loudly below
        const, signal = gk
        lam = lam_for(signal)
        # a pair group's key is "sigA+sigB"; grade band-1 (first K rows)
        if lam is None:
            lam = lam_for(signal.split("+")[0]) if "+" in signal else None
        for i, (u, v) in enumerate(chords):
            if i >= len(z):
                break
            if u in sat_by_vertex:
                sat, rx = sat_by_vertex[u], v
            elif v in sat_by_vertex:
                sat, rx = sat_by_vertex[v], u
            else:
                continue                            # unmappable vertex -> skip
            if lam is None:
                nolam += 1
                continue
            out[(const, signal, rx, sat)] = z[i] / lam
    return out, nolam


def pct(vals, p):
    if not vals:
        return float("nan")
    s = sorted(vals)
    return s[min(len(s) - 1, int(p * (len(s) - 1)))]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a"); ap.add_argument("b")
    ap.add_argument("--tol", type=float, default=0.05, help="cycles (CANDIDATE)")
    ap.add_argument("--top", type=int, default=10)
    args = ap.parse_args()

    A, nla = keyed_chords(args.a)
    B, nlb = keyed_chords(args.b)
    common = sorted(set(A) & set(B))
    only_a, only_b = len(set(A) - set(B)), len(set(B) - set(A))
    print(f"A: {len(A)} chords ({args.a})")
    print(f"B: {len(B)} chords ({args.b})")
    print(f"common={len(common)}  only-in-A={only_a}  only-in-B={only_b}"
          + (f"  [skipped-no-lambda A={nla} B={nlb}]" if (nla or nlb) else ""))
    if not common:
        print("NO COMMON CHORDS — id translation or blob parse is wrong.")
        return 1

    d = [A[k] - B[k] for k in common]
    ad = [abs(x) for x in d]
    n = len(common)
    # The same-integer count is the branch instrument. A carried DD is refined by
    # data every epoch: with q_z=1e-4 m²/s over 30 s its own process noise is
    # already sqrt(3e-3) = 55 mm, about 0.29 L1 cycles per epoch, so a sub-0.05-cycle
    # bar measures estimate precision, not which integer the branch sits on. Two
    # runs are on the same branch if and only if their DDs round to the same integer.
    same_int = sum(1 for x in d if round(x) == 0)
    agree = sum(1 for x in ad if x < args.tol)
    flip = sum(1 for x in d if abs(x - round(x)) < args.tol and round(x) != 0)
    print(f"\n|dz| cycles: median={pct(ad,.5):.4f}  p95={pct(ad,.95):.4f}  max={max(ad):.4f}")
    print(f"SAME-INTEGER branch (round(dz)==0):   {same_int}/{n} = {100.0*same_int/n:.1f}%")
    print(f"tight-agree  (|dz| < {args.tol} cyc):    {agree}/{n} = {100.0*agree/n:.1f}%  [precision, not branch]")
    print(f"branch-flip  (near-integer, round!=0): {flip}/{n} = {100.0*flip/n:.1f}%")
    worst = sorted(common, key=lambda k: -abs(A[k] - B[k]))[:args.top]
    if worst:
        print(f"\nlargest {len(worst)} |dz| (cycles):")
        for k in worst:
            print(f"   {k[0]} {k[1]:<5} rx{k[2]:<4} {k[3]:<5} dz={A[k]-B[k]:+.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

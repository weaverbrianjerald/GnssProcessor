#!/usr/bin/env python3
"""Undeclared wire-step census across an FP-2 leg matrix.

    python scripts/fp2_wire_step_census.py [--runs C:/GnssProcessor/runs]
                                           [--prefix fp2] [--tol 0.5]
                                           [--json out.json]

Why this is not one of §20's gates: G-C reads the injected satellite's wire and
a "worst other <= 2x the floor" bar, both as event minus null.  A step that
happens in both the event leg and its null cancels there, and a step whose epoch
merely moves between them shows up as an unexplained "worst other" rather than
as what it is.  This census reads each leg on its own: every emitted satellite
clock (``C``) and phase bias (``P``) series, every between-epoch jump above
--tol, and whether the producer declared it, meaning the ``phase_disc`` /
``clock_disc`` counter incrementing and the emitted sigma opening.

Standing invariant #7: a mechanism that re-realizes a datum is an event at both
edges, and the declaration is the consumer's only notice.  An undeclared step is
therefore the reportable class, a metre-scale move the consumer cannot see, and
it is a property of one arm, so the pin/off counts are the comparison.
"""
import argparse
import json
import os
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):                        # pragma: no cover
    pass

EVENTS = ("null", "e1", "e2", "e3g24", "e3g25", "e5")
ARMS = ("off", "pin")
EPOCH_S = 30.0


def records(path, t0):
    """{(kind, sat, sig): [(k, value, disc, sigma)]} from a products.txt."""
    out, tow = {}, None
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if not line or line[0] == "#":
                continue
            f = line.rstrip().split(",")
            if f[0] == "E" and len(f) >= 3:
                try:
                    tow = float(f[2])
                except ValueError:
                    tow = None
                continue
            if tow is None:
                continue
            # Record layout (products.txt header, v2):
            #   C,sat,clock_c0_m[,clock_c1_m_s[,clock_disc]]...
            #   P,sat,signal,bias_m,signal_integer,wide_lane_integer[,phase_disc]
            #                [,sigma=<m>][,sigc=<m>]
            # The disc column is optional, so index by the documented position and
            # accept it only when it is a bare integer; sigma is a key=value token.
            if f[0] == "C" and len(f) >= 3:
                key, vi = ("C", f[1], ""), 2
                disc = f[4] if len(f) > 4 and f[4].isdigit() else None
            elif f[0] == "P" and len(f) >= 4:
                key, vi = ("P", f[1], f[2]), 3
                disc = f[6] if len(f) > 6 and f[6].isdigit() else None
            else:
                continue
            try:
                v = float(f[vi])
            except ValueError:
                continue
            sig = next((x.split("=", 1)[1] for x in f if x.startswith("sigma=")), None)
            out.setdefault(key, []).append(
                (int(round((tow - t0) / EPOCH_S)), v, disc, sig))
    return out


def steps(series, tol):
    """[(k, prev, now, |d|, declared)] for jumps above tol."""
    out = []
    for i in range(1, len(series)):
        k0, v0, d0, _ = series[i - 1]
        k1, v1, d1, s1 = series[i]
        if k1 != k0 + 1:                     # a gap is not a step
            continue
        if abs(v1 - v0) > tol:
            # Declared means the disc counter moved at this epoch.  A missing counter
            # (the toggle that emits it is off) can never declare: count it as
            # undeclared and say so, rather than silently passing.
            declared = (d0 is not None and d1 is not None and d0 != d1)
            out.append((k1, v0, v1, abs(v1 - v0), declared, d0, d1, s1))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", default=r"C:\GnssProcessor\runs")
    ap.add_argument("--prefix", default="fp2")
    ap.add_argument("--tol", type=float, default=0.5)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    t0 = None
    for key in EVENTS:
        mp = os.path.join(a.runs, f"evleg_{a.prefix}_e1_off", "leg_meta.json")
        if os.path.isfile(mp):
            t0 = float(json.load(open(mp, encoding="utf-8"))["event_tow"])
            break
    if t0 is None:
        raise SystemExit("FATAL: cannot read event_tow")

    print("=" * 96)
    print(f"FP-2 WIRE-STEP CENSUS  |step| > {a.tol} m between consecutive epochs, "
          f"per leg, per emitted key")
    print("  DECLARED = the disc counter incremented at that epoch "
          "(the consumer's only notice)")
    print("=" * 96)
    print(f"\n{'leg':<22}{'steps':>7}{'declared':>10}{'UNDECL':>8}   worst undeclared")
    print("-" * 96)
    res, tot = {}, {"off": 0, "pin": 0}
    for key in EVENTS:
        for arm in ARMS:
            nm = f"{a.prefix}_{key}_{arm}"
            p = os.path.join(a.runs, f"evleg_{nm}", "products.txt")
            if not os.path.isfile(p):
                print(f"{nm:<22}   (missing)")
                continue
            allst = []
            for k, ser in records(p, t0).items():
                for st in steps(ser, a.tol):
                    allst.append((k, st))
            dec = [x for x in allst if x[1][4]]
            und = [x for x in allst if not x[1][4]]
            und.sort(key=lambda x: -x[1][3])
            tot[arm] += len(und)
            worst = ("", )
            if und:
                kk, st = und[0]
                worst = (f"{'/'.join(y for y in kk if y):<14} k={st[0]:+d} "
                         f"{st[1]:.4f}->{st[2]:.4f} |d|={st[3]:.4f} "
                         f"disc {st[5]}->{st[6]} sigma {st[7]}", )
            print(f"{nm:<22}{len(allst):>7}{len(dec):>10}{len(und):>8}   {worst[0]}")
            res[nm] = {"n_steps": len(allst), "n_declared": len(dec),
                       "n_undeclared": len(und),
                       "undeclared": [["/".join(y for y in kk if y), st[0],
                                       round(st[3], 4), st[5], st[6], st[7]]
                                      for kk, st in und]}
            for kk, st in und[1:]:
                print(f"{'':<22}{'':>25}   {'/'.join(y for y in kk if y):<14} "
                      f"k={st[0]:+d} |d|={st[3]:.4f} disc {st[5]}->{st[6]} "
                      f"sigma {st[7]}")

    print(f"\nUNDECLARED steps total:  OFF {tot['off']}   PIN {tot['pin']}")
    if tot["pin"] > tot["off"]:
        print("  ⚠ the PIN posture emits MORE undeclared metre-scale wire steps than "
              "OFF — invariant #7's declaration edge, on a channel no §20 gate reads.")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump({"tol": a.tol, "legs": res, "totals": tot}, fh, indent=1)
        print(f"\njson -> {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

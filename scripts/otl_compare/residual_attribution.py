#!/usr/bin/env python3
"""What the 342-line HARDISP development carries beyond the 11 BLQ constituents.

The production model evaluates the 11 BLQ constituents (with the nodal factors when
`otl_nodal_corrections` is on).  HARDISP adds, per component, every other line of its
development with an admittance interpolated between the constituents of the same band.
This script splits the HARDISP Up displacement of one station into

  main       the 11 main lines
  node       their node side lines (what the nodal factors reproduce)
  other-LP / other-D / other-SD
             all remaining lines of the long-period, diurnal and semidiurnal band (the
             minor constituents the 11-constituent model leaves out)

and prints the peak of each class over one UTC day.  The sum of the "other" classes is the
floor under the production-versus-HARDISP difference of `otl_gate.py`.

    python -I residual_attribution.py [--station KOKB00USA] [--date 2024-129] [--step 120]
"""
import argparse
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hardisp_port as hp          # noqa: E402
import otl_gate as og              # noqa: E402


def classify(h):
    """Class of each of the 342 lines, in the order ADMINT emits them."""
    mains = set(hp.IDT)
    mains_core = {m[:4] + m[5:] for m in hp.IDT}
    out = []
    for d in h.idd:
        if d in mains:
            out.append("main")
        elif (d[:4] + d[5:]) in mains_core and d[4] != 0:
            out.append("node")
        else:
            out.append({0: "other-LP", 1: "other-D", 2: "other-SD"}[d[0]])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--station", default="KOKB00USA")
    ap.add_argument("--date", default="2024-129")
    ap.add_argument("--step", type=int, default=120)
    a = ap.parse_args()
    year, doy = (int(v) for v in a.date.split("-"))
    h = hp.Hardisp()
    rec = og.sif_record(a.station)
    amp, phs = og.hardisp_columns(rec, 0.0)
    cls = classify(h)
    names = ["main", "node", "other-LP", "other-D", "other-SD"]
    peaks = {n: 0.0 for n in names}
    peak_tot = 0.0
    for i in range(0, 86400 // a.step):
        sod = i * a.step
        hh, mm, ss = sod // 3600, (sod // 60) % 60, sod % 60
        h._setup_args((year, doy, hh, mm, ss))
        comp = 0                                              # Up
        am, ph = h.admint([amp[c][comp] for c in range(11)], [-phs[c][comp] for c in range(11)])
        # ADMINT skips the long-period lines only when no long-period input exists; the
        # emitted lines are all 342 here, in table order.
        parts = {n: 0.0 for n in names}
        for j, c in enumerate(cls[:len(am)]):
            parts[c] += am[j] * math.cos(hp.DR * ph[j])
        tot = sum(parts.values())
        peak_tot = max(peak_tot, abs(tot))
        for n in names:
            peaks[n] = max(peaks[n], abs(parts[n]))
    print(f"{a.station} {a.date} Up, HARDISP peak {peak_tot * 1000:.2f} mm; peak of each class (mm):")
    print("  " + "  ".join(f"{n} {peaks[n] * 1000:.2f}" for n in names))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

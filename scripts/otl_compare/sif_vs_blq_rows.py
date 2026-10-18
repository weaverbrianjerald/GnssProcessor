#!/usr/bin/env python3
"""Compare the SIF OTL block with independent BLQ coefficients at the two stations printed in
the HARDISP.F header (Onsala and Reykjavik, CSR4.0 model, Scherneck and Bos).

The SIF (FES2014b through CARGA) and the header coefficients (CSR4.0 through OLMPP) are
different ocean tide models, so amplitudes and phases differ by tens of degrees at most.  A
relabelled or sign-reversed horizontal pair differs by about 180 degrees.  The script
therefore reports, per constituent and horizontal row, the circular phase difference
between the SIF column and the BLQ row under

  reading B  SIF east/north column = BLQ west/south row            (M-PAGES reading)
  reading A  SIF east/north column = true east/north, phase + 180  (the model's shift)

    python -I sif_vs_blq_rows.py
"""
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hardisp_port as hp          # noqa: E402

NAMES = ["M2", "S2", "N2", "K2", "K1", "O1", "P1", "Q1", "Mf", "Mm", "Ssa"]


def circ(d):
    return (d + 180.0) % 360.0 - 180.0


def sif_station(prefix):
    for path in ("C:/GnssProcessor/data/static/sif/igs20_SIF.json",
                 "C:/GnssProcessor/data/static/sif/ncn20_SIF.json"):
        for s in json.load(open(path))["STATIONS"]:
            if s["ID"].startswith(prefix) and s.get("OCEAN_TIDE_LOADING"):
                return s
    raise KeyError(prefix)


def report(label, sif_prefix, amp_rows, phs_rows):
    s = sif_station(sif_prefix)
    otl = s["OCEAN_TIDE_LOADING"]
    print(f"\n{label}: SIF {s['ID']} (FES2014b) against the HARDISP.F header BLQ rows (CSR4.0)")
    print("  const  row     BLQ amp/phase      SIF amp/phase     dphi B   dphi A")
    tot_b = tot_a = wsum = 0.0
    for k, n in enumerate(NAMES[:8]):
        c = otl[n if n in otl else n.upper()]
        for row, (sa, sp, name) in enumerate(((c["EAST_AMPLITUDE"], c["EAST_PHASE"], "W"),
                                              (c["NORTH_AMPLITUDE"], c["NORTH_PHASE"], "S")), start=1):
            ba, bp = amp_rows[row][k], phs_rows[row][k]
            db = circ(sp - bp)
            da = circ(sp + 180.0 - bp)
            w = min(ba, sa)
            tot_b += w * abs(db)
            tot_a += w * abs(da)
            wsum += w
            print(f"  {n:4s}   {name}   {1000 * ba:6.2f} mm {bp:7.1f}   {1000 * sa:6.2f} mm {sp:7.1f}"
                  f"   {db:7.1f}  {da:7.1f}")
        # radial row as the control: the same convention on both sides, only the model differs
        print(f"  {n:4s}   U   {1000 * amp_rows[0][k]:6.2f} mm {phs_rows[0][k]:7.1f}   "
              f"{1000 * c['UP_AMPLITUDE']:6.2f} mm {c['UP_PHASE']:7.1f}   {circ(c['UP_PHASE'] - phs_rows[0][k]):7.1f}")
    print(f"  amplitude-weighted mean |dphi| over the 8 largest constituents, horizontal rows:"
          f" reading B {tot_b / wsum:.1f} deg, reading A {tot_a / wsum:.1f} deg")


report("Onsala", "ONSA", hp.ONSALA_AMP, hp.ONSALA_PHS)
report("Reykjavik", "REYK", hp.REYK_AMP, hp.REYK_PHS)

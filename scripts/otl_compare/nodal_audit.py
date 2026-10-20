#!/usr/bin/env python3
"""Nodal sidelines of the 11 BLQ constituents, read from the HARDISP development.

The 18.6-year nodal modulation of a constituent appears in the Cartwright-Tayler-Edden
development of the tide-generating potential as side lines that differ from the main line
only in the lunar node multiplier n5 (HARDISP ADMINT.F, tables TAMP and IDD).  Treating the
loading admittance as flat across a main line and its side lines, the nodal factor f and
phase correction u of a constituent are

    f * exp(i u) = 1 + sum_k (TAMP_k / TAMP_main) * exp(-i n5_k * Omega)

with Omega the longitude of the Moon's ascending node (HARDISP's N' = -Omega).  The script
prints the side-line table used by OceanTideModel::nodalFactor and compares it with the
closed-form Schureman series that is usually quoted.

    python -I nodal_audit.py [--admint ADMINT.F]
"""
import argparse
import cmath
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hardisp_port as hp          # noqa: E402

NAMES = ["M2", "S2", "N2", "K2", "K1", "O1", "P1", "Q1", "Mf", "Mm", "Ssa"]


def sidelines(h):
    out = {}
    for name, ct in zip(NAMES, hp.IDT):
        km = h.idd.index(ct)
        t0 = h.tamp[km]
        rows = []
        for i, d in enumerate(h.idd):
            if i == km:
                continue
            if d[:4] == ct[:4] and d[5] == ct[5] and d[4] != 0:
                rows.append((d[4], h.tamp[i] / t0, h.tamp[i], d))
        rows.sort()
        out[name] = (t0, rows)
    return out


def nodal(rows, omega):
    z = 1.0 + sum(r * cmath.exp(-1j * n5 * omega) for n5, r, _, _ in rows)
    return abs(z), cmath.phase(z)


# Schureman / IHO closed forms (N = Omega); u in degrees, f dimensionless
def schureman(name, n):
    c = [math.cos(k * n) for k in (1, 2, 3)]
    s = [math.sin(k * n) for k in (1, 2, 3)]
    d2r = math.pi / 180.0
    if name in ("M2", "N2"):
        return 1.0004 - 0.0373 * c[0] + 0.0002 * c[1], -2.14 * s[0] * d2r
    if name == "K2":
        return 1.0241 + 0.2863 * c[0] + 0.0083 * c[1], (-17.74 * s[0] + 0.68 * s[1] - 0.04 * s[2]) * d2r
    if name == "K1":
        return 1.0060 + 0.1150 * c[0] - 0.0088 * c[1] + 0.0006 * c[2], (-8.86 * s[0] + 0.68 * s[1] - 0.07 * s[2]) * d2r
    if name in ("O1", "Q1"):
        return 1.0089 + 0.1871 * c[0] - 0.0147 * c[1] + 0.0014 * c[2], (10.80 * s[0] - 1.34 * s[1] + 0.19 * s[2]) * d2r
    if name == "Mf":
        return 1.0429 + 0.4135 * c[0] - 0.0040 * c[1], (-23.74 * s[0] + 2.68 * s[1] - 0.38 * s[2]) * d2r
    if name == "Mm":
        return 1.0000 - 0.1300 * c[0] + 0.0013 * c[1], 0.0
    return 1.0, 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--admint", default=hp.DEFAULT_ADMINT)
    a = ap.parse_args()
    h = hp.Hardisp(a.admint)
    sl = sidelines(h)
    print("Node side lines (n5 = Doodson multiplier of N', ratio = TAMP_side / TAMP_main):")
    for n in NAMES:
        t0, rows = sl[n]
        print(f"  {n:3s} main TAMP {t0:+.6f}: " + ", ".join(f"n5={n5:+d} r={r:+.5f}" for n5, r, _, _ in rows))
    print("\nf and u (deg) at Omega in 30 degree steps: development / Schureman closed form")
    worst = {}
    for n in NAMES:
        t0, rows = sl[n]
        wf = wu = 0.0
        for k in range(12):
            om = math.radians(30.0 * k)
            f1, u1 = nodal(rows, om)
            f2, u2 = schureman(n, om)
            wf = max(wf, abs(f1 - f2))
            wu = max(wu, abs(math.degrees(u1 - u2)))
        worst[n] = (wf, wu)
    for n in NAMES:
        print(f"  {n:3s} max |df| {worst[n][0]:.4f}   max |du| {worst[n][1]:.2f} deg")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

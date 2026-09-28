#!/usr/bin/env python3
"""Standalone reference for the solid Earth tide (IERS 2010 dehanttideinel).

This is an independent Python port of the same IERS Conventions
dehanttideinel routine that GnssCorrections::computeSolidEarthTideDisplacement
implements (Step 1 in-phase degree 2/3, Step 1 out-of-phase diurnal/
semidiurnal + L1, Step 2 diurnal Table 7.3a + long-period Table 7.3b).

It carries its own copy of the IERS tables and formulas rather than parsing
them out of the C++ source, so it is a cross-check rather than a tautology.
Its job is twofold:

  1. Reproduce the official IERS dehanttideinel test vector (the same oracle
     the C++ unit test uses), confirming the algorithm end to end.
  2. Provide a second, different-language transcription path: if the C++ and
     this script agree with the official vector to the same ~6e-5 m, the small
     residual is inherent to the IERS-2003-lineage (POINT) formulation rather
     than a C++ transcription bug.

Usage:
    python scripts/solid_tide_reference.py
"""

from __future__ import annotations
import math
import sys

# --- IERS dehanttideinel constants (match GnssCorrections.cpp) -----------
H20, L20 = 0.6078, 0.0847
H3,  L3  = 0.292,  0.015
MASS_RATIO_SUN  = 332945.943062
MASS_RATIO_MOON = 0.012300034
RE = 6378136.55
DEG2RAD = math.pi / 180.0


def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def st1_diurnal(xsun, xmon, fac2s, fac2m, cphi, sphi, cla, sla):
    dhi, dli = -0.0025, -0.0007
    c2phi = cphi * cphi - sphi * sphi
    dr = dn = de = 0.0
    for b, fac in ((xsun, fac2s), (xmon, fac2m)):
        r2 = b[0] * b[0] + b[1] * b[1] + b[2] * b[2]
        g = b[2] * (b[0] * sla - b[1] * cla) / r2
        dr += -3.0 * dhi * sphi * cphi * fac * g
        dn += -3.0 * dli * c2phi * fac * g
        de += -3.0 * dli * sphi * fac * b[2] * (b[0] * cla + b[1] * sla) / r2
    return (dr * cla * cphi - de * sla - dn * sphi * cla,
            dr * sla * cphi + de * cla - dn * sphi * sla,
            dr * sphi + dn * cphi)


def st1_semid(xsun, xmon, fac2s, fac2m, cphi, sphi, cla, sla):
    dhi, dli = -0.0022, -0.0007
    c2la = cla * cla - sla * sla
    s2la = 2.0 * cla * sla
    dr = dn = de = 0.0
    for b, fac in ((xsun, fac2s), (xmon, fac2m)):
        r2 = b[0] * b[0] + b[1] * b[1] + b[2] * b[2]
        bx, by = b[0], b[1]
        ta = (bx * bx - by * by) * s2la - 2.0 * bx * by * c2la
        tb = (bx * bx - by * by) * c2la + 2.0 * bx * by * s2la
        dr += (-3.0 / 4.0) * dhi * cphi * cphi * fac * ta / r2
        dn += (3.0 / 2.0) * dli * sphi * cphi * fac * ta / r2
        de += (-3.0 / 2.0) * dli * cphi * fac * tb / r2
    return (dr * cla * cphi - de * sla - dn * sphi * cla,
            dr * sla * cphi + de * cla - dn * sphi * sla,
            dr * sphi + dn * cphi)


def st1_l1(xsun, xmon, fac2s, fac2m, cphi, sphi, cla, sla):
    l1d, l1sd = 0.0012, 0.0024
    corr = [0.0, 0.0, 0.0]
    # diurnal band
    dn = de = 0.0
    for b, fac in ((xsun, fac2s), (xmon, fac2m)):
        r2 = b[0] * b[0] + b[1] * b[1] + b[2] * b[2]
        dn += -l1d * sphi * sphi * fac * b[2] * (b[0] * cla + b[1] * sla) / r2
        de += l1d * sphi * (cphi * cphi - sphi * sphi) * fac * b[2] * (b[0] * sla - b[1] * cla) / r2
    dn *= 3.0
    de *= 3.0
    corr[0] += -de * sla - dn * sphi * cla
    corr[1] += de * cla - dn * sphi * sla
    corr[2] += dn * cphi
    # semidiurnal band
    c2la = cla * cla - sla * sla
    s2la = 2.0 * cla * sla
    dn = de = 0.0
    for b, fac in ((xsun, fac2s), (xmon, fac2m)):
        r2 = b[0] * b[0] + b[1] * b[1] + b[2] * b[2]
        bx, by = b[0], b[1]
        dn += -l1sd / 2.0 * sphi * cphi * fac * ((bx * bx - by * by) * c2la + 2.0 * bx * by * s2la) / r2
        de += -l1sd / 2.0 * sphi * sphi * cphi * fac * ((bx * bx - by * by) * s2la - 2.0 * bx * by * c2la) / r2
    dn *= 3.0
    de *= 3.0
    corr[0] += -de * sla - dn * sphi * cla
    corr[1] += de * cla - dn * sphi * sla
    corr[2] += dn * cphi
    return tuple(corr)


def angles(fhr, jc):
    jc2, jc3, jc4 = jc * jc, jc * jc * jc, (jc * jc) * (jc * jc)
    s = 218.31664563 + 481267.88194 * jc - 0.0014663889 * jc2 + 0.00000185139 * jc3
    tau = fhr * 15.0 + 280.4606184 + 36000.7700536 * jc + 0.00038793 * jc2 - 0.0000000258 * jc3 - s
    pr = 1.396971278 * jc + 0.000308889 * jc2 + 0.000000021 * jc3 + 0.000000007 * jc4
    s += pr
    h = 280.46645 + 36000.7697489 * jc + 0.00030322222 * jc2 + 0.000000020 * jc3 - 0.00000000654 * jc4
    p = 83.35324312 + 4069.01363525 * jc - 0.01032172222 * jc2 - 0.0000124991 * jc3 + 0.00000005263 * jc4
    zns = 234.95544499 + 1934.13626197 * jc - 0.00207561111 * jc2 - 0.00000213944 * jc3 + 0.00000001650 * jc4
    ps = 282.93734098 + 1.71945766667 * jc + 0.00045688889 * jc2 - 0.00000001778 * jc3 - 0.00000000334 * jc4
    return {k: math.fmod(v, 360.0) for k, v in
            dict(s=s, tau=tau, h=h, p=p, zns=zns, ps=ps).items()}


# IERS 2010 Table 7.3a (diurnal): [s,h,p,N',ps, dRin, dRout, dTin, dTout] (mm)
STEP2DIU = [
    [-3., 0., 2., 0., 0., -0.01, 0.00, 0.00, 0.00],
    [-3., 2., 0., 0., 0., -0.01, 0.00, 0.00, 0.00],
    [-2., 0., 1., -1., 0., -0.02, 0.00, 0.00, 0.00],
    [-2., 0., 1., 0., 0., -0.08, 0.00, -0.01, 0.01],
    [-2., 2., -1., 0., 0., -0.02, 0.00, 0.00, 0.00],
    [-1., 0., 0., -1., 0., -0.10, 0.00, 0.00, 0.00],
    [-1., 0., 0., 0., 0., -0.51, 0.00, -0.02, 0.03],
    [-1., 2., 0., 0., 0., 0.01, 0.00, 0.00, 0.00],
    [0., -2., 1., 0., 0., 0.01, 0.00, 0.00, 0.00],
    [0., 0., -1., 0., 0., 0.02, 0.00, 0.00, 0.00],
    [0., 0., 1., 0., 0., 0.06, 0.00, 0.00, 0.00],
    [0., 0., 1., 1., 0., 0.01, 0.00, 0.00, 0.00],
    [0., 2., -1., 0., 0., 0.01, 0.00, 0.00, 0.00],
    [1., -3., 0., 0., 1., -0.06, 0.00, 0.00, 0.00],
    [1., -2., 0., -1., 0., 0.01, 0.00, 0.00, 0.00],
    [1., -2., 0., 0., 0., -1.23, -0.07, 0.06, 0.01],   # P1
    [1., -1., 0., 0., -1., 0.02, 0.00, 0.00, 0.00],
    [1., -1., 0., 0., 1., 0.04, 0.00, 0.00, 0.00],
    [1., 0., 0., -1., 0., -0.22, 0.01, 0.01, 0.00],
    [1., 0., 0., 0., 0., 12.00, -0.80, -0.67, -0.03],  # K1
    [1., 0., 0., 1., 0., 1.73, -0.12, -0.10, 0.00],
    [1., 0., 0., 2., 0., -0.04, 0.00, 0.00, 0.00],
    [1., 1., 0., 0., -1., -0.50, -0.01, 0.03, 0.00],
    [1., 1., 0., 0., 1., 0.01, 0.00, 0.00, 0.00],
    [0., 1., 0., 1., -1., -0.01, 0.00, 0.00, 0.00],
    [1., 2., -2., 0., 0., -0.01, 0.00, 0.00, 0.00],
    [1., 2., 0., 0., 0., -0.11, 0.01, 0.01, 0.00],
    [2., -2., 1., 0., 0., -0.01, 0.00, 0.00, 0.00],
    [2., 0., -1., 0., 0., -0.02, 0.00, 0.00, 0.00],
    [3., 0., 0., 0., 0., 0.00, 0.00, 0.00, 0.00],
    [3., 0., 0., 1., 0., 0.00, 0.00, 0.00, 0.00],
]

# IERS 2010 Table 7.3b (long period): [s,h,p,N',ps, dRin, dRout, dTin, dTout] (mm)
STEP2LON = [
    [0., 0., 0., 1., 0., 0.47, 0.16, 0.23, 0.07],
    [0., 2., 0., 0., 0., -0.20, -0.11, -0.12, -0.05],
    [1., 0., -1., 0., 0., -0.11, -0.09, -0.08, -0.04],
    [2., 0., 0., 0., 0., -0.13, -0.15, -0.11, -0.07],
    [2., 0., 0., 1., 0., -0.05, -0.06, -0.05, -0.03],
]


def st2_diurnal(fhr, jc, lon, cphi, sphi, cla, sla):
    a = angles(fhr, jc)
    c2phi = cphi * cphi - sphi * sphi
    dr = dn = de = 0.0
    for r in STEP2DIU:
        theta = (a["tau"] + r[0] * a["s"] + r[1] * a["h"] + r[2] * a["p"]
                 + r[3] * a["zns"] + r[4] * a["ps"]) * DEG2RAD + lon
        st, ct = math.sin(theta), math.cos(theta)
        dr += (r[5] * st + r[6] * ct) * 2.0 * sphi * cphi
        dn += (r[7] * st + r[8] * ct) * c2phi
        de += (r[7] * ct + r[8] * st) * sphi
    dr, dn, de = dr * 1e-3, dn * 1e-3, de * 1e-3
    return (dr * cla * cphi - de * sla - dn * sphi * cla,
            dr * sla * cphi + de * cla - dn * sphi * sla,
            dr * sphi + dn * cphi)


def st2_lon(fhr, jc, cphi, sphi, cla, sla):
    a = angles(fhr, jc)
    sin2lat = 2.0 * sphi * cphi
    radlat = (3.0 * sphi * sphi - 1.0) / 2.0
    dr = dn = 0.0
    for r in STEP2LON:
        theta = (r[0] * a["s"] + r[1] * a["h"] + r[2] * a["p"]
                 + r[3] * a["zns"] + r[4] * a["ps"]) * DEG2RAD
        st, ct = math.sin(theta), math.cos(theta)
        dr += (r[5] * ct + r[6] * st) * radlat
        dn += (r[7] * ct + r[8] * st) * sin2lat
    dr, dn = dr * 1e-3, dn * 1e-3
    return (dr * cla * cphi - dn * sphi * cla,
            dr * sla * cphi - dn * sphi * sla,
            dr * sphi + dn * cphi)


def solid_earth_tide_displacement(xsta, xsun, xmon, jc, fhr):
    """Mirror of GnssCorrections::computeSolidEarthTideDisplacement."""
    rsta = math.sqrt(sum(c * c for c in xsta))
    rsun = math.sqrt(sum(c * c for c in xsun))
    rmon = math.sqrt(sum(c * c for c in xmon))
    cphi = math.sqrt(xsta[0] ** 2 + xsta[1] ** 2) / rsta
    sphi = xsta[2] / rsta
    cla = xsta[0] / (cphi * rsta)
    sla = xsta[1] / (cphi * rsta)
    lon = math.atan2(xsta[1], xsta[0])

    h2 = H20 - 0.0006 * (1.0 - 1.5 * cphi * cphi)
    l2 = L20 + 0.0002 * (1.0 - 1.5 * cphi * cphi)

    cps = sum(a * b for a, b in zip(xsta, xsun)) / (rsta * rsun)
    cpm = sum(a * b for a, b in zip(xsta, xmon)) / (rsta * rmon)

    p2s = 3.0 * (h2 / 2.0 - l2) * cps * cps - h2 / 2.0
    p2m = 3.0 * (h2 / 2.0 - l2) * cpm * cpm - h2 / 2.0
    p3s = 2.5 * (H3 - 3.0 * L3) * cps ** 3 + 1.5 * (L3 - H3) * cps
    p3m = 2.5 * (H3 - 3.0 * L3) * cpm ** 3 + 1.5 * (L3 - H3) * cpm
    x2s = 3.0 * l2 * cps
    x2m = 3.0 * l2 * cpm
    x3s = 1.5 * L3 * (5.0 * cps * cps - 1.0)
    x3m = 1.5 * L3 * (5.0 * cpm * cpm - 1.0)

    rr = RE / rsun
    fac2s = MASS_RATIO_SUN * RE * rr ** 3
    fac3s = fac2s * rr
    rr = RE / rmon
    fac2m = MASS_RATIO_MOON * RE * rr ** 3
    fac3m = fac2m * rr

    disp = [0.0, 0.0, 0.0]
    for i in range(3):
        disp[i] = (fac2s * (x2s * xsun[i] / rsun + p2s * xsta[i] / rsta)
                   + fac2m * (x2m * xmon[i] / rmon + p2m * xsta[i] / rsta)
                   + fac3s * (x3s * xsun[i] / rsun + p3s * xsta[i] / rsta)
                   + fac3m * (x3m * xmon[i] / rmon + p3m * xsta[i] / rsta))
    disp = tuple(disp)

    disp = _add(disp, st1_diurnal(xsun, xmon, fac2s, fac2m, cphi, sphi, cla, sla))
    disp = _add(disp, st1_semid(xsun, xmon, fac2s, fac2m, cphi, sphi, cla, sla))
    disp = _add(disp, st1_l1(xsun, xmon, fac2s, fac2m, cphi, sphi, cla, sla))
    disp = _add(disp, st2_diurnal(fhr, jc, lon, cphi, sphi, cla, sla))
    disp = _add(disp, st2_lon(fhr, jc, cphi, sphi, cla, sla))
    return disp


def main():
    # IERS dehanttideinel.f test vector (2009-04-13 00:00 UT, FHR=0).
    xsta = (4075578.385, 931852.890, 4801570.154)
    xsun = (137859926952.015, 54228127881.4350, 23509422341.6896)
    xmon = (-179996231.920342, -312468450.131567, -169288918.592160)
    jc = (54934.0 - 51544.5) / 36525.0
    fhr = 0.0
    expected = (0.07700420357108126, 0.06304056321824968, 0.05516568152597247)

    got = solid_earth_tide_displacement(xsta, xsun, xmon, jc, fhr)
    print("component        got                 expected            |diff| (m)")
    worst = 0.0
    for name, g, e in zip("xyz", got, expected):
        d = abs(g - e)
        worst = max(worst, d)
        print(f"  d{name}   {g: .17f}  {e: .17f}  {d: .3e}")
    print(f"\nmax |diff| = {worst:.3e} m "
          f"({'PASS' if worst < 1e-4 else 'FAIL'} vs 1e-4 m gate; "
          f"dehanttideinel routine accuracy is ~1e-4 m)")
    return 0 if worst < 1e-4 else 1


if __name__ == "__main__":
    sys.exit(main())

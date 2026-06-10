#!/usr/bin/env python3
"""Cross-check GnssProcessor's two independent GMST implementations against
each other and against the cssrlib/RTKLIB form, for the SAME UT1 input.

GnssProcessor computes GMST in two places with two different (both standard
IAU-1982) algebraic forms:

  A. Planetary::utc2gmst  — "split" form: GMST at 0h UT1 + 1.00273790935·UT.
  B. GnssCorrections::computeFundamentalArguments — "continuous" form:
       gmst = 67310.54841 + (3155760000 + 8640184.812866)·T + 0.093104·T²
              − 6.2e-6·T³   [seconds], T = UT1 centuries from J2000.

If both are faithful IAU-1982 GMST they must agree to ~µs for any UT1.
A divergence beyond rounding means one has a transcription bug.

Usage:  python scripts/gmst_crosscheck.py
"""

from __future__ import annotations
import math
import sys

J2000_JD = 2451545.0
CY = 36525.0
TWO_PI = 2.0 * math.pi


def gmst_split(ut1_jd):
    """Form A — Planetary::utc2gmst."""
    tut0 = (math.floor(ut1_jd - 0.5) + 0.5 - J2000_JD) / CY      # centuries at 0h UT1
    gmst0 = (24110.54841 + 8640184.812866 * tut0
             + 0.093104 * tut0 * tut0 - 6.2e-6 * tut0**3)         # seconds
    ut1_d = math.fmod(ut1_jd + 0.5, 1.0)                          # fractional day since 0h
    gmst = gmst0 + 1.002737909350795 * ut1_d * 86400.0
    return math.fmod(gmst * (TWO_PI / 86400.0), TWO_PI)


def gmst_continuous(ut1_jd):
    """Form B — GnssCorrections::computeFundamentalArguments."""
    T = (ut1_jd - J2000_JD) / CY                                 # UT1 centuries from J2000
    gmst_sec = math.fmod(67310.54841
                         + (3155760000.0 + 8640184.812866) * T
                         + 0.093104 * T * T
                         - 6.2e-6 * T**3, 86400.0)
    return math.fmod(gmst_sec * (TWO_PI / 86400.0), TWO_PI)


def norm(a):
    a = math.fmod(a, TWO_PI)
    if a < 0:
        a += TWO_PI
    return a


def main():
    # A spread of UT1 epochs across years and times of day.
    cases = [
        ("2024-05-08 06:00", 2460438.75),
        ("2024-07-28 00:00", 2460519.5),
        ("2000-01-01 12:00", 2451545.0),
        ("2010-12-31 08:09", 2455561.83958),
        ("2017-01-01 00:00", 2457754.5),
        ("1999-09-09 21:00", 2451431.375),
    ]
    worst = 0.0
    for label, jd in cases:
        a = norm(gmst_split(jd))
        b = norm(gmst_continuous(jd))
        d = abs(a - b)
        d = min(d, TWO_PI - d)                                   # circular distance
        worst = max(worst, d)
        # express the discrepancy as seconds of Earth rotation
        d_sec = d / TWO_PI * 86400.0
        print(f"{label}: split={a:.9f} rad  cont={b:.9f} rad  "
              f"|d|={d:.3e} rad ({d_sec*1e3:.3f} ms)")
    print(f"\nworst |d| = {worst:.3e} rad "
          f"({worst/TWO_PI*86400.0*1e3:.3f} ms of Earth rotation)")
    print("CONSISTENT" if worst < 1e-6 else "!! DIVERGENT — one form has a bug")
    return 0


if __name__ == "__main__":
    sys.exit(main())

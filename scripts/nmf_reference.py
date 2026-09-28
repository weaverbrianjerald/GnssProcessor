#!/usr/bin/env python3
"""Canonical Niell Mapping Function (NMF) reference, mirroring cssrlib's
tropmapfNiell / interpc (gnss.py) verbatim: itself a port of RTKLIB
tropmapf.  Used to generate golden mf_d / mf_w for the GnssProcessor
regression test that locks the May-2026 NMF fixes:

  1. hydrostatic seasonal amplitude arrays carry a leading 0.0 (15°),
  2. seasonal combination is  avg − amp·cos(2π(doy−28)/365.25).

Both POINT (Troposphere.cpp abc_amp) and cssrlib agree on these; the
prior GnssProcessor code dropped the 0.0 (shifting amplitudes one bin)
and added the amplitude term instead of subtracting it.

Usage:
    python scripts/nmf_reference.py
"""

from __future__ import annotations
import math
import sys

# Coefficient table: copied verbatim from cssrlib gnss.py:1417-1426.
COEF = [
    [1.2769934E-3, 1.2683230E-3, 1.2465397E-3, 1.2196049E-3, 1.2045996E-3],  # ah_avg
    [2.9153695E-3, 2.9152299E-3, 2.9288445E-3, 2.9022565E-3, 2.9024912E-3],  # bh_avg
    [62.610505E-3, 62.837393E-3, 63.721774E-3, 63.824265E-3, 64.258455E-3],  # ch_avg
    [0.0000000E-0, 1.2709626E-5, 2.6523662E-5, 3.4000452E-5, 4.1202191E-5],  # ah_amp
    [0.0000000E-0, 2.1414979E-5, 3.0160779E-5, 7.2562722E-5, 11.723375E-5],  # bh_amp
    [0.0000000E-0, 9.0128400E-5, 4.3497037E-5, 84.795348E-5, 170.37206E-5],  # ch_amp
    [5.8021897E-4, 5.6794847E-4, 5.8118019E-4, 5.9727542E-4, 6.1641693E-4],  # aw
    [1.4275268E-3, 1.5138625E-3, 1.4572752E-3, 1.5007428E-3, 1.7599082E-3],  # bw
    [4.3472961E-2, 4.6729510E-2, 4.3908931E-2, 4.4626982E-2, 5.4736038E-2],  # cw
]
AHT = [2.53E-5, 5.49E-3, 1.14E-3]


def interpc(row, lat):
    """cssrlib interpc (lat step = 15°)."""
    i = int(lat / 15.0)
    if i < 1:
        return row[0]
    if i > 4:
        return row[4]
    d = lat / 15.0 - i
    return row[i - 1] * (1.0 - d) + row[i] * d


def mapf(el_rad, a, b, c):
    """Marini continued fraction (cssrlib mapf)."""
    s = math.sin(el_rad)
    return (1.0 + a / (1.0 + b / (1.0 + c))) / (s + a / (s + b / (s + c)))


def nmf(lat_deg, doy, el_deg, hgt_m):
    el = math.radians(el_deg)
    y = (doy - 28.0) / 365.25
    if lat_deg < 0.0:
        y += 0.5
    cosy = math.cos(2.0 * math.pi * y)
    lat = abs(lat_deg)

    c = [interpc(COEF[k], lat) for k in range(9)]
    # avg − amp·cosy (the canonical form: subtraction, not addition)
    ah = [c[0] - c[3] * cosy, c[1] - c[4] * cosy, c[2] - c[5] * cosy]
    aw = [c[6], c[7], c[8]]

    dm = (1.0 / math.sin(el) - mapf(el, AHT[0], AHT[1], AHT[2])) * hgt_m * 1e-3
    mapfh = mapf(el, ah[0], ah[1], ah[2]) + dm
    mapfw = mapf(el, aw[0], aw[1], aw[2])
    return mapfh, mapfw


def main():
    # Columns: (label, lat_deg, lon_deg, h_m, doy, el_deg).
    # doy 129.25 is 2024-05-08 06:00 (week 2313, tow 280800).
    # doy 210.0 is 2024-07-28 00:00 (week 2325, tow 0), where cos_annual ≈ -1,
    # giving the seasonal term its largest magnitude.
    cases = [
        ("mid45_doy129_el10",  45.0,  -75.0, 200.0, 129.25, 10.0),
        ("high70_doy129_el15", 70.0,   10.0, 100.0, 129.25, 15.0),
        ("mid45_doy210_el5",   45.0,  -75.0,   0.0, 210.0,   5.0),
        ("south40_doy210_el20", -40.0, 150.0,  50.0, 210.0,  20.0),
        ("trop15_doy210_el8",  15.0,  100.0,   0.0, 210.0,   8.0),
    ]
    for label, lat, lon, h, doy, el in cases:
        mfh, mfw = nmf(lat, doy, el, h)
        print(f'    {{ "{label}", {lat!r}, {lon!r}, {h!r}, {el!r}, {mfh!r}, {mfw!r} }},')
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Net translation of the SIF diurnal ocean tide loading field (the table of
OceanTideModel.cpp, removeOtlDiurnalTranslation).

The SIF ocean tide loading block (FES2014b through CARGA, centre-of-mass frame) of the 597
stations of igs20_SIF.json is read as the BLQ rows (up, west, south), converted to east, north
and up phasors (east = -west, north = -south) and fitted, per constituent, with one uniform
ECEF vector (three complex numbers) by least squares. The stations are weighted by the
area of their 10 degree by 10 degree cell divided by the number of stations in the cell, so
that the dense regions do not set the vector. The phasor convention is that of the BLQ
file: z = A exp(-i phi), phi the Greenwich phase lag in degrees.

The vector is the part of the displacement field that moves every station by the same ECEF
displacement, that is a translation of the reference frame origin. For the diurnal species it
carries 45 % (K1), 40 % (O1) and 45 % (P1) of the east, north and up variance of the field
(area weighted); for the semidiurnal species 6 %, which is regional structure leaking into the
fit, not a translation (a species-2 tide has no degree-1 forcing).

    python -I net_translation.py                 print the fit and the share of variance
    python -I net_translation.py --emit-cpp      print the C++ table
    python -I net_translation.py --check <OceanTideModel.cpp>
                                                 compare the table in the source with the fit

The fit is data dependent: it holds for the SIF named above. A different registry needs the
table regenerated (card OTL, section 9).
"""
import cmath
import collections
import json
import math
import re
import sys

import numpy as np

SIF = "C:/GnssProcessor/data/static/sif/igs20_SIF.json"
NAMES = ["M2", "S2", "N2", "K2", "K1", "O1", "P1", "Q1", "MF", "MM", "SSA"]
DIURNAL = ["K1", "O1", "P1", "Q1"]


def llh(x, y, z):
    a = 6378137.0
    f = 1 / 298.257223563
    e2 = f * (2 - f)
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1 - e2))
    for _ in range(6):
        n = a / math.sqrt(1 - e2 * math.sin(lat) ** 2)
        h = p / math.cos(lat) - n
        lat = math.atan2(z, p * (1 - e2 * n / (n + h)))
    return lat, lon


def enu_rows(lat, lon):
    sl, cl, so, co = math.sin(lat), math.cos(lat), math.sin(lon), math.cos(lon)
    return np.array([[-so, co, 0.0], [-sl * co, -sl * so, cl], [cl * co, cl * so, sl]])


def phasor(block, key):
    return block[key + "_AMPLITUDE"] * cmath.exp(-1j * math.radians(block[key + "_PHASE"]))


def stations():
    out = []
    for s in json.load(open(SIF))["STATIONS"]:
        otl = s.get("OCEAN_TIDE_LOADING")
        if otl and "M2" in otl:
            lat, lon = llh(*s["APPROXIMATE_COORDINATES"])
            out.append((s["ID"], lat, lon, otl))
    return out


def weights(st):
    cell = collections.defaultdict(list)
    for i, (_, lat, lon, _) in enumerate(st):
        cell[(int(math.floor(math.degrees(lat) / 10)), int(math.floor(math.degrees(lon) / 10)))].append(i)
    w = np.zeros(len(st))
    for (la, _), idx in cell.items():
        for i in idx:
            w[i] = math.cos(math.radians(la * 10 + 5)) / len(idx)
    return w


def fit(st, w, name):
    rows, rhs = [], []
    for _, lat, lon, otl in st:
        b = enu_rows(lat, lon)
        c = otl[name]
        # SIF columns are the BLQ rows up, west, south; ENU = (-west, -south, up)
        for r, val in ((0, -phasor(c, "EAST")), (1, -phasor(c, "NORTH")), (2, phasor(c, "UP"))):
            rows.append(b[r])
            rhs.append(val)
    sw = np.sqrt(np.repeat(w, 3))
    a = np.array(rows)
    y = np.array(rhs)
    x, *_ = np.linalg.lstsq(a * sw[:, None], y * sw, rcond=None)
    res = y - a @ x
    share = 1.0 - float(np.sum(sw ** 2 * abs(res) ** 2) / np.sum(sw ** 2 * abs(y) ** 2))
    return x, share


def table_lines():
    st = stations()
    w = weights(st)
    lines = []
    report = []
    for name in NAMES:
        x, share = fit(st, w, name)
        report.append((name, x, share))
        if name in DIURNAL:
            amps = ", ".join(f"{abs(v):.7f}" for v in x)
            phases = ", ".join(f"{-math.degrees(cmath.phase(v)):.3f}" for v in x)
            lines.append(f"            {{ {NAMES.index(name)}, {{ {amps} }}, {{ {phases} }} }},  // {name}")
    return len(st), report, lines


def main():
    n, report, lines = table_lines()
    if "--emit-cpp" in sys.argv:
        print("\n".join(lines))
        return 0
    if "--check" in sys.argv:
        src = open(sys.argv[sys.argv.index("--check") + 1], encoding="utf-8").read()
        ok = True
        squash = lambda t: "".join(t.split())
        for ln in lines:
            if squash(ln) not in squash(src):
                print("MISSING in source:", ln.strip())
                ok = False
        print("net translation table matches the fit" if ok else "net translation table DIFFERS from the fit")
        return 0 if ok else 1
    print(f"{n} stations, area-weighted uniform ECEF vector per constituent (BLQ phasor convention)")
    print("const   |g| mm   X mm  Y mm  Z mm   share of ENU variance")
    for name, x, share in report:
        print(f"{name:4s} {np.linalg.norm(x) * 1000:7.2f} {abs(x[0]) * 1000:6.2f} {abs(x[1]) * 1000:5.2f} {abs(x[2]) * 1000:5.2f}   {share * 100:5.1f} %")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Which horizontal convention does the SIF OTL block carry?

The SIF stores ocean tide loading as UP / EAST / NORTH amplitude and phase.  The block
comes from BLQ files (its comment lines read "RADI TANG" and "Computed using CARGA"), and
the BLQ rows are radial, WEST positive and SOUTH positive.  Two readings of the SIF
columns are possible:

  A  true East and North displacement phases   (the model's present +180 deg shift)
  B  the BLQ west and south rows relabelled    (M-PAGES passes them straight to HARDISP)

The two readings differ by the sign of the whole horizontal vector.  Physical test that
needs no reference software: ocean loading draws the crust toward the load.  At the
instant the radial displacement is most negative (the local tide is high, the crust is
pressed down) the horizontal displacement of a coastal station therefore points toward the
sea.  For every coastal SIF station the script forms the horizontal vector at that instant
under each reading and counts how often it points toward the nearest coast point (the sea
side of a land station).

    python -I sif_horizontal_convention.py [--sif igs20_SIF.json ...] [--max-km 100]
"""
import argparse
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
NAMES = ["M2", "S2", "N2", "K2", "K1", "O1", "P1", "Q1", "Mf", "Mm", "Ssa"]
A_E = 6378137.0


def find_asset(rel):
    for base in (ROOT, "C:/GnssProcessor"):
        p = os.path.join(base, rel)
        if os.path.exists(p):
            return p
    raise FileNotFoundError(rel)


def ecef_to_llh(x, y, z):
    f = 1 / 298.257223563
    e2 = f * (2 - f)
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1 - e2))
    for _ in range(6):
        n = A_E / math.sqrt(1 - e2 * math.sin(lat) ** 2)
        lat = math.atan2(z + e2 * n * math.sin(lat), p)
    return math.degrees(lat), math.degrees(lon)


def load_coast(path):
    d = json.load(open(path))
    pts = [(la, lo) for la, lo in zip(d["lat"], d["lon"]) if la is not None and lo is not None]
    return pts


def haversine_bearing(lat1, lon1, lat2, lon2):
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    a = math.sin((p2 - p1) / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    dist = 2 * 6371.0 * math.asin(min(1.0, math.sqrt(a)))
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return dist, math.atan2(y, x)       # bearing from north, clockwise


def seaward(lat, lon, coast):
    best = (1e9, 0.0)
    for la, lo in coast:
        if abs(la - lat) > 3.0:
            continue
        d, b = haversine_bearing(lat, lon, la, lo)
        if d < best[0]:
            best = (d, b)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sif", nargs="*", default=None)
    ap.add_argument("--max-km", type=float, default=100.0)
    ap.add_argument("--min-up-mm", type=float, default=5.0)
    ap.add_argument("--gradient", action="store_true",
                    help="run the coastline-free gradient test instead of the coastline test")
    a = ap.parse_args()
    sifs = a.sif or [find_asset("data/static/sif/igs20_SIF.json"), find_asset("data/static/sif/ncn20_SIF.json")]
    if a.gradient:
        for cn in ("M2", "S2", "N2", "K1", "O1"):
            for rad in (60.0, 120.0):
                n, pos = gradient_test(sifs, rad, 6, cn)
                if n:
                    print(f"{cn:3s} radius {rad:5.0f} km: {n:4d} stations with a measurable radial gradient; "
                          f"horizontal points up the depression gradient under reading B (BLQ rows): "
                          f"{pos} ({100.0 * pos / n:.0f} %), reading A: {n - pos} ({100.0 * (n - pos) / n:.0f} %)")
        return 0
    coast = load_coast(find_asset("pyviz/gnssviz/assets/coastline_110m.json"))
    rows = []
    seen = set()
    for path in sifs:
        for s in json.load(open(path))["STATIONS"]:
            otl = s.get("OCEAN_TIDE_LOADING")
            if not otl or "M2" not in otl or s["ID"][:4] in seen:
                continue
            xyz = s.get("APPROXIMATE_COORDINATES")
            if not xyz or abs(xyz[0]) + abs(xyz[1]) + abs(xyz[2]) < 1e6:
                continue
            seen.add(s["ID"][:4])
            lat, lon = ecef_to_llh(*xyz)
            dist, brg = seaward(lat, lon, coast)
            rows.append((s["ID"], lat, lon, dist, brg, otl))
    print(f"stations with OTL: {len(rows)}")
    for cname in ("M2", "S2", "N2", "K1", "O1"):
        n = pos = 0
        for sid, lat, lon, dist, brg, otl in rows:
            if dist > a.max_km:
                continue
            c = otl[cname if cname in otl else cname.upper()]
            if c["UP_AMPLITUDE"] * 1000.0 < a.min_up_mm:
                continue
            pu = math.radians(c["UP_PHASE"])
            pe = math.radians(c["EAST_PHASE"])
            pn = math.radians(c["NORTH_PHASE"])
            # reading B (BLQ west/south rows): east = +Aw cos(pu-pw), north = +As cos(pu-ps),
            # evaluated at the radial minimum.  Reading A is the same vector with the sign reversed.
            e = c["EAST_AMPLITUDE"] * math.cos(pu - pe)
            nn = c["NORTH_AMPLITUDE"] * math.cos(pu - pn)
            se, sn = math.sin(brg), math.cos(brg)
            toward = e * se + nn * sn
            n += 1
            pos += 1 if toward > 0 else 0
        if n:
            print(f"{cname:3s}: {n:3d} coastal stations (<= {a.max_km:.0f} km, Up >= {a.min_up_mm:.0f} mm); "
                  f"horizontal points seaward under reading B (BLQ rows): {pos} ({100.0 * pos / n:.0f} %), "
                  f"under reading A (true east/north): {n - pos} ({100.0 * (n - pos) / n:.0f} %)")
    return 0


def gradient_test(sifs, radius_km, min_nb, cname="M2", min_up_mm=3.0, min_grad_mm=0.3):
    """Second physical test, with no coastline: the horizontal vector should point toward
    increasing radial depression, whose gradient is measured from the neighbouring stations
    of the same SIF.  Needs a dense network (NCN CORS)."""
    st = []
    seen = set()
    for path in sifs:
        for s in json.load(open(path))["STATIONS"]:
            otl = s.get("OCEAN_TIDE_LOADING")
            xyz = s.get("APPROXIMATE_COORDINATES")
            if not otl or cname not in otl or not xyz or s["ID"][:4] in seen:
                continue
            if abs(xyz[0]) + abs(xyz[1]) + abs(xyz[2]) < 1e6:
                continue
            seen.add(s["ID"][:4])
            lat, lon = ecef_to_llh(*xyz)
            st.append((s["ID"], lat, lon, otl[cname]))
    buckets = {}
    for i, (_, la, lo, _) in enumerate(st):
        buckets.setdefault((int(la), int(lo)), []).append(i)
    n = pos = 0
    for i, (sid, la, lo, c) in enumerate(st):
        if c["UP_AMPLITUDE"] * 1000.0 < min_up_mm:
            continue
        pu0 = math.radians(c["UP_PHASE"])
        theta = pu0 + math.pi                       # instant of the central station's radial minimum
        nb = []
        dlat = radius_km / 111.0
        dlon = radius_km / (111.0 * max(0.2, math.cos(math.radians(la))))
        for bi in range(int(la - dlat) - 1, int(la + dlat) + 2):
            for bj in range(int(lo - dlon) - 1, int(lo + dlon) + 2):
                for j in buckets.get((bi, bj), []):
                    if j == i:
                        continue
                    _, lb, lob, cb = st[j]
                    d, brg = haversine_bearing(la, lo, lb, lob)
                    if d <= radius_km:
                        dd = -cb["UP_AMPLITUDE"] * math.cos(theta - math.radians(cb["UP_PHASE"]))
                        nb.append((d * math.sin(brg), d * math.cos(brg), dd))
        if len(nb) < min_nb:
            continue
        # plane fit D = D0 + gE*dE + gN*dN, via the normal equations
        sx = sy = sxx = syy = sxy = sd = sdx = sdy = 0.0
        m = len(nb) + 1
        pts = nb + [(0.0, 0.0, -c["UP_AMPLITUDE"] * math.cos(0.0 + math.pi))]
        for x, y, d in pts:
            sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y
            sd += d; sdx += d * x; sdy += d * y
        det = (m * (sxx * syy - sxy * sxy) - sx * (sx * syy - sxy * sy) + sy * (sx * sxy - sxx * sy))
        if abs(det) < 1e-12:
            continue
        # solve [m sx sy; sx sxx sxy; sy sxy syy] [d0 gE gN] = [sd sdx sdy]
        def det3(a):
            return (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
                    - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                    + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
        A = [[m, sx, sy], [sx, sxx, sxy], [sy, sxy, syy]]
        b = [sd, sdx, sdy]
        D = det3(A)
        def sub(col):
            M = [r[:] for r in A]
            for r in range(3):
                M[r][col] = b[r]
            return det3(M)
        gE, gN = sub(1) / D, sub(2) / D                 # m per km
        if math.hypot(gE, gN) * radius_km * 1000.0 < min_grad_mm:
            continue
        e = c["EAST_AMPLITUDE"] * math.cos(pu0 - math.radians(c["EAST_PHASE"]))   # reading B
        nn = c["NORTH_AMPLITUDE"] * math.cos(pu0 - math.radians(c["NORTH_PHASE"]))
        n += 1
        pos += 1 if (e * gE + nn * gN) > 0 else 0
    return n, pos


if __name__ == "__main__":
    raise SystemExit(main())

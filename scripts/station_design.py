"""Station-set design for the fast network tests: inventory, coverage measure, quota-ruled selection.

Reads only RINEX 3 observation files, the day's SP3 orbit and the receiver ATX already on disk.
No engine run, no download.

Usage (one day at a time; the cache is reused by every later step):

  python scripts/station_design.py scan      --day C:/GnssProcessor/data/2024-129 --cache <dir> [--workers 12]
  python scripts/station_design.py inventory --cache <dir> --out <dir>/inventory.csv
  python scripts/station_design.py coverage  --cache <dir> --set <config.json | list.txt | ALL>
  python scripts/station_design.py design    --cache <dir> --out <dir> [--sizes 40,60,80,100]
                                             [--compare <config.json> ...] [--candidates <cache of another day>]
  python scripts/station_design.py tables    --json <dir>/design_<tag>.json

Definitions (all stated as rules, none hand-picked):
  signal class    system + band + tracking attribute, e.g. E5Q (Galileo E5a, pilot "Q") or E5X.
  tracks          the station delivers BOTH the code and the phase of that class at that epoch and the
                  satellite is at or above the elevation mask (default 15 deg, the goldens' mask).
  universe        satellite-epochs of a class that at least one station of the whole population tracks
                  (with --candidates, the modeled stations count too).
  coverage n      for a station set, the number of its stations that track a satellite-epoch.
  delivered share of a station and class: tracked rows / that station's own rows above the mask of the
                  satellites that transmit the class (a satellite transmits a class when any station tracks it).
  eligible        epoch completeness >= 90 %, GPS L1 and L2 (non-X channels) delivered on >= 50 % of GPS rows,
                  antenna known to the ATX (radome NONE fallback allowed), not in --exclude.
  delivers (for quotas)  delivered share >= --deliver-min (default 0.8).
  regions         the 12 equal cells around the vertices of an icosahedron (nearest vertex).
Selection: greedy on a concave utility (a satellite-epoch's (n+1)-th tracker is worth 4^(2-n), each class
normalised by its universe size) after the quotas are met, then pairwise exchange.  Quotas:
  Q1 every make present in every class it delivers in the population (>= 1 station);
  Q2 every receiver type with >= 3 eligible stations has >= 2 in the network (types with 1 or 2 stations
     are kept for held-out users of a type outside the network);
  Q3 per region and per region-family class (default E5Q, E5X): if the population has >= 2 there, >= 2;
  Q4 every region with candidates has >= 2 network stations (>= 1 where it has only one).
Held-out users (separate, same rules): >= 2 per make where the population allows, about one third of a
receiver type outside the network, spread by farthest point.
"""
import argparse
import csv
import gzip
import json
import math
import os
import re
import sys
from collections import Counter, defaultdict

import numpy as np

STEP_S = 30
NEPOCH = 86400 // STEP_S
SYS_N = {"G": 32, "E": 36, "C": 64, "J": 10, "R": 28}
SYS_NAME = {"G": "GPS", "E": "Galileo", "C": "BeiDou", "J": "QZSS", "R": "GLONASS"}
MAKES = (("SEPT", "Septentrio"), ("TRIMBLE", "Trimble"), ("LEICA", "Leica"), ("JAVAD", "Javad"),
         ("JPS", "Javad"), ("TPS", "Topcon"), ("TOPCON", "Topcon"), ("NOV", "NovAtel"), ("ASHTECH", "Ashtech"),
         ("CHC", "CHC"), ("STONEX", "Stonex"), ("COMNAV", "ComNav"), ("UNICORE", "Unicore"))
WGS_A, WGS_F = 6378137.0, 1.0 / 298.257223563


# ----------------------------------------------------------------------------------------------- inputs
def open_text(path):
    if path.lower().endswith(".gz"):
        return gzip.open(path, "rt", encoding="ascii", errors="replace")
    return open(path, "r", encoding="ascii", errors="replace")


def make_of(rec_type):
    up = rec_type.upper()
    for key, name in MAKES:
        if key in up:
            return name
    return "Other"


def obs_file_of(folder):
    names = sorted(os.listdir(folder))
    for pat in (r"_mo\.rnx(\.gz)?$", r"\.rnx(\.gz)?$", r"\.\d\do(\.gz)?$"):
        for n in names:
            if re.search(pat, n.lower()):
                return os.path.join(folder, n)
    return None


def ecef_to_geodetic(x, y, z):
    e2 = WGS_F * (2 - WGS_F)
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1 - e2))
    for _ in range(6):
        n = WGS_A / math.sqrt(1 - e2 * math.sin(lat) ** 2)
        h = p / math.cos(lat) - n
        lat = math.atan2(z, p * (1 - e2 * n / (n + h)))
    return math.degrees(lat), math.degrees(lon), h


def read_sp3(path):
    """Satellite ECEF positions (m) on the 30 s grid, per system: {sys: (NEPOCH, nsat, 3)}, NaN where absent."""
    times, rows = [], defaultdict(dict)
    with open_text(path) as f:
        for line in f:
            if line.startswith("*"):
                p = line.split()
                h, m, s = int(p[4]), int(p[5]), float(p[6])
                t = h * 3600 + m * 60 + s
                while times and t <= times[-1]:
                    t += 86400.0          # the closing epoch 00:00 of the next day
                times.append(t)
            elif line.startswith("P") and times:
                sysc, prn = line[1], int(line[2:4])
                if sysc in SYS_N and prn <= SYS_N[sysc]:
                    xyz = [float(line[4 + 14 * i:18 + 14 * i]) for i in range(3)]
                    if abs(xyz[0]) > 1e-6 and abs(xyz[0]) < 9e5:
                        rows[len(times) - 1][(sysc, prn)] = xyz
    t = np.array(times)
    grid = np.arange(NEPOCH) * STEP_S
    out = {}
    for sysc, ns in SYS_N.items():
        arr = np.full((len(t), ns, 3), np.nan)
        for k in range(len(t)):
            for (sc, prn), xyz in rows[k].items():
                if sc == sysc:
                    arr[k, prn - 1] = xyz
        res = np.full((NEPOCH, ns, 3), np.nan)
        for j in range(ns):
            for c in range(3):
                v = arr[:, j, c]
                ok = ~np.isnan(v)
                if ok.sum() >= 2:
                    res[:, j, c] = np.interp(grid, t[ok], v[ok], left=np.nan, right=np.nan)
                    # linear extrapolation over the last orbit interval (23:55 to 23:59:30)
                    tail = grid > t[ok][-1]
                    if tail.any():
                        tt, vv = t[ok][-2:], v[ok][-2:]
                        res[tail, j, c] = vv[1] + (grid[tail] - tt[1]) * (vv[1] - vv[0]) / (tt[1] - tt[0])
        out[sysc] = res * 1000.0
    return out


def elevations(orbits, xyz):
    lat, lon, _ = ecef_to_geodetic(*xyz)
    la, lo = math.radians(lat), math.radians(lon)
    up = np.array([math.cos(la) * math.cos(lo), math.cos(la) * math.sin(lo), math.sin(la)])
    out = {}
    for sysc, pos in orbits.items():
        d = pos - np.asarray(xyz)
        r = np.linalg.norm(d, axis=2)
        with np.errstate(invalid="ignore"):
            out[sysc] = np.degrees(np.arcsin((d @ up) / r))
    return out


def read_atx(path):
    """{'ANTENNA RADOME' (20 chars stripped of nothing): set of systems with frequencies} for receiver antennas."""
    ants = {}
    cur, systems = None, set()
    with open_text(path) as f:
        for line in f:
            lab = line[60:80].strip()
            if lab == "TYPE / SERIAL NO":
                serial = line[20:40].strip()
                cur = line[0:20] if not serial else None
                systems = set()
            elif lab == "START OF FREQUENCY" and cur is not None:
                systems.add(line[3])
            elif lab == "END OF ANTENNA" and cur is not None:
                ants[cur.rstrip()] = systems
                cur = None
    return ants


# --------------------------------------------------------------------------------------------- scanning
def scan_station(job):
    """Parse one observation file; write <cache>/<NAME>.npz with presence bitmaps and a JSON header."""
    path, cache, orbit_path, mask = job
    hdr = {"rec_type": "", "rec_fw": "", "ant": "", "radome": "", "xyz": None, "obs": {}, "interval": None}
    obs = defaultdict(list)
    cur = None
    with open_text(path) as f:
        for line in f:
            lab = line[60:80].strip()
            if lab == "MARKER NAME":
                hdr["marker"] = line[:60].strip()
            elif lab == "REC # / TYPE / VERS":
                hdr["rec_type"], hdr["rec_fw"] = line[20:40].strip(), line[40:60].strip()
            elif lab == "ANT # / TYPE":
                hdr["ant"], hdr["radome"] = line[20:36].strip(), line[36:40].strip()
            elif lab == "APPROX POSITION XYZ":
                hdr["xyz"] = [float(v) for v in line[:42].split()]
            elif lab == "INTERVAL":
                hdr["interval"] = float(line[:10])
            elif lab == "SYS / # / OBS TYPES":
                if line[0] != " ":
                    cur = line[0]
                obs[cur] += line[7:60].split()
            elif lab == "END OF HEADER":
                break
        hdr["obs"] = {k: v for k, v in obs.items()}
        # field index of every code (C) and phase (L) type per system
        fields = {s: [(i, t) for i, t in enumerate(v) if t[0] in "CL"] for s, v in obs.items() if s in SYS_N}
        bits = {s: {t: np.zeros((NEPOCH, SYS_N[s]), bool) for _, t in fields[s]} for s in fields}
        rows = {s: np.zeros((NEPOCH, SYS_N[s]), bool) for s in fields}
        epochs = np.zeros(NEPOCH, bool)
        k = -1
        for line in f:
            if line[0] == ">":
                try:
                    sec = int(line[13:15]) * 3600 + int(line[16:18]) * 60 + float(line[19:29])
                except ValueError:
                    k = -1
                    continue
                isec = int(round(sec))
                k = isec // STEP_S if (isec % STEP_S == 0 and line[31] in "01") else -1
                if k >= NEPOCH:
                    k = -1
                if k >= 0:
                    epochs[k] = True
                continue
            if k < 0:
                continue
            s = line[0]
            fl = fields.get(s)
            if fl is None:
                continue
            try:
                j = int(line[1:3]) - 1
            except ValueError:
                continue
            if j < 0 or j >= SYS_N[s]:
                continue
            rows[s][k, j] = True
            n = len(line.rstrip("\n"))
            bs = bits[s]
            for i, t in fl:
                a = 3 + 16 * i
                if a < n and line[a:a + 14].strip():
                    bs[t][k, j] = True
    orbits = np.load(orbit_path)
    el = elevations({s: orbits[s] for s in SYS_N}, hdr["xyz"])
    arrays = {"epochs": epochs}
    for s in fields:
        vis = el[s] >= mask
        arrays["vis_" + s] = vis
        arrays["rows_" + s] = rows[s] & vis
        for t, b in bits[s].items():
            arrays["b_%s_%s" % (s, t)] = b & vis
    name = os.path.basename(path)[:9].upper()
    hdr["name"] = name
    hdr["file"] = path.replace("\\", "/")
    np.savez_compressed(os.path.join(cache, name + ".npz"), header=json.dumps(hdr), **arrays)
    return name


def cmd_scan(a):
    os.makedirs(a.cache, exist_ok=True)
    prod = os.path.join(a.day, "products")
    sp3 = next(os.path.join(prod, n) for n in sorted(os.listdir(prod)) if "ORB.SP3" in n.upper())
    orbit_path = os.path.join(a.cache, "_orbits.npz")
    np.savez_compressed(orbit_path, **read_sp3(sp3))
    jobs = []
    obs_root = os.path.join(a.day, "obs")
    for st in sorted(os.listdir(obs_root)):
        p = obs_file_of(os.path.join(obs_root, st))
        if p and not (a.skip_done and os.path.exists(os.path.join(a.cache, os.path.basename(p)[:9].upper() + ".npz"))):
            jobs.append((p, a.cache, orbit_path, a.mask))
    json.dump({"day": a.day.replace("\\", "/"), "sp3": sp3.replace("\\", "/"), "mask_deg": a.mask},
              open(os.path.join(a.cache, "_meta.json"), "w"))
    print("scanning %d files with %d workers" % (len(jobs), a.workers), flush=True)
    from multiprocessing import Pool
    with Pool(a.workers) as pool:
        for i, name in enumerate(pool.imap_unordered(scan_station, jobs), 1):
            if i % 25 == 0:
                print("  %d/%d (%s)" % (i, len(jobs), name), flush=True)
    print("done", flush=True)




# ------------------------------------------------------------------------------------------- population
ICO = np.array([(0, 1, 1.618034), (0, -1, 1.618034), (0, 1, -1.618034), (0, -1, -1.618034),
                (1, 1.618034, 0), (-1, 1.618034, 0), (1, -1.618034, 0), (-1, -1.618034, 0),
                (1.618034, 0, 1), (-1.618034, 0, 1), (1.618034, 0, -1), (-1.618034, 0, -1)])
ICO = ICO / np.linalg.norm(ICO, axis=1)[:, None]
GPS_L1_OK = ("G1C", "G1W", "G1L", "G1S", "G1P")
GPS_L2_OK = ("G2W", "G2L", "G2S", "G2P")
R_EARTH_KM = 6371.0
WINDOW = (0, 6 * 3600 // STEP_S)   # the fast tests' 00-06 UT window, reported beside the whole day
# stations the record already dropped (name:reason); data-driven rules do the rest
DEFAULT_EXCLUDE = ["MIZU00JPN:coordinates 1.6 m off its data",
                   "BSHM00ISR:local block not positive definite, wet-delay steps",
                   "INEG00MEX:GPS L2 on the X channel only",
                   "OAFA00ARG:GPS L2 on the X channel only"]


def unit_of(lat, lon):
    la, lo = np.radians(lat), np.radians(lon)
    return np.stack([np.cos(la) * np.cos(lo), np.cos(la) * np.sin(lo), np.sin(la)], axis=-1)


def fib_grid(n):
    i = np.arange(n) + 0.5
    z = 1 - 2 * i / n
    r = np.sqrt(1 - z * z)
    phi = math.pi * (1 + 5 ** 0.5) * i
    return np.stack([r * np.cos(phi), r * np.sin(phi), z], axis=1)


class Population:
    """Every station of one cache (plus optional pseudo stations), with full-rate class bitmaps packed."""

    def __init__(self, cache, systems, atx_path, exclude, thin):
        self.cache, self.systems, self.thin, self.exclude = cache, systems, thin, exclude
        self.meta = json.load(open(os.path.join(cache, "_meta.json")))
        self.orbits = np.load(os.path.join(cache, "_orbits.npz"))
        self.atx = read_atx(atx_path)
        self.st, self.bits, self.rowcnt = [], [], []
        names = sorted(n[:-4] for n in os.listdir(cache) if n.endswith(".npz") and not n.startswith("_"))
        for n in names:
            z = np.load(os.path.join(cache, n + ".npz"))
            h = json.loads(str(z["header"]))
            h["pseudo"] = False
            h["completeness"] = float(z["epochs"].mean())
            bits, rc = {}, {}
            for s in SYS_N:
                if "rows_" + s not in z.files:
                    continue
                rc[s] = z["rows_" + s].sum(axis=0)
                codes = {k[5:] for k in z.files if k.startswith("b_%s_C" % s)}
                for c in sorted(codes):
                    lk = "b_%s_L%s" % (s, c)
                    if lk in z.files:
                        b = z["b_%s_C%s" % (s, c)] & z[lk]
                        if b.any():
                            bits[s + c] = np.packbits(b)
            self.st.append(h)
            self.bits.append(bits)
            self.rowcnt.append(rc)
        self.finish()

    def add_pseudo(self, other, names, dmin):
        """Stations of another day's cache that this day lacks: presence = visibility today x delivers there."""
        for n in names:
            i = other.index[n]
            h = dict(other.st[i])
            h["pseudo"], h["completeness"] = True, 1.0
            el = elevations({s: self.orbits[s] for s in SYS_N}, h["xyz"])
            bits, rc = {}, {}
            for s in SYS_N:
                vis = el[s] >= self.meta["mask_deg"]
                rc[s] = vis.sum(axis=0)
                for k, share in other.share[i].items():
                    if k[0] == s and share >= dmin and k in self.transmit:
                        bits[k] = np.packbits(vis & self.transmit[k][None, :])
            self.st.append(h)
            self.bits.append(bits)
            self.rowcnt.append(rc)
        self.finish()

    def nsat(self, k):
        return SYS_N[k[0]]

    def unpack(self, i, k):
        b = self.bits[i].get(k)
        if b is None:
            return None
        return np.unpackbits(b, count=NEPOCH * self.nsat(k)).reshape(NEPOCH, self.nsat(k)).astype(bool)

    def finish(self):
        self.names = [h["name"] for h in self.st]
        self.index = {n: i for i, n in enumerate(self.names)}
        self.classes = sorted({k for b in self.bits for k in b if k[0] in self.systems})
        # the universe of a class: satellite-epochs tracked by at least one station (modeled ones included)
        self.npop = {}
        for k in self.classes:
            cnt = np.zeros((NEPOCH, self.nsat(k)), np.int16)
            for i in range(len(self.st)):
                u = self.unpack(i, k)
                if u is not None:
                    cnt += u
            self.npop[k] = cnt
        self.transmit = {k: self.npop[k].sum(axis=0) > 0 for k in self.classes}
        self.share = []
        for i, h in enumerate(self.st):
            sh = {}
            for k in self.classes:
                b, rc = self.bits[i].get(k), self.rowcnt[i].get(k[0])
                if b is None or rc is None:
                    continue
                den = rc[self.transmit[k]].sum()
                sh[k] = int(np.unpackbits(b).sum()) / den if den else 0.0
            self.share.append(sh)
        for i, h in enumerate(self.st):
            h["make"] = make_of(h["rec_type"])
            lat, lon, hgt = ecef_to_geodetic(*h["xyz"])
            h["lat"], h["lon"], h["hgt"] = lat, lon, hgt
            h["region"] = int(np.argmax(ICO @ unit_of(lat, lon)))
            key = (h["ant"].ljust(16) + h["radome"]).rstrip()
            fb = h["ant"].ljust(16) + "NONE"
            h["atx"] = "exact" if key in self.atx else ("radome fallback" if fb in self.atx else "none")
            h["atx_gal"] = "E" in self.atx.get(key, self.atx.get(fb, set()))
            sh = self.share[i]
            reasons = []
            if h["completeness"] < 0.9:
                reasons.append("completeness %.2f" % h["completeness"])
            if max([sh.get(k, 0) for k in GPS_L1_OK]) < 0.5 or max([sh.get(k, 0) for k in GPS_L2_OK]) < 0.5:
                reasons.append("GPS L1/L2 non-X < 50 %")
            if h["atx"] == "none":
                reasons.append("antenna not in ATX")
            if h["name"] in self.exclude:
                reasons.append(self.exclude[h["name"]])
            h["eligible"] = not reasons
            h["reason"] = "; ".join(reasons)
        self.elig = np.array([h["eligible"] for h in self.st])

    def delivers(self, i, k, dmin):
        return self.share[i].get(k, 0.0) >= dmin

    def coverage(self, members, classes=None):
        """Per class over its universe: min, p05, median, satellite-epochs (30 s) below 2 and below 3."""
        out = {}
        for k in classes or self.classes:
            uni = self.npop[k] > 0
            if not uni.any():
                continue
            n = np.zeros((NEPOCH, self.nsat(k)), np.int16)
            for i in members:
                u = self.unpack(i, k)
                if u is not None:
                    n += u
            v, p = n[uni], self.npop[k][uni]
            win = uni.copy()
            win[:WINDOW[0]] = False
            win[WINDOW[1]:] = False
            out[k] = {"below2_win": int((n[win] < 2).sum()), "below3_win": int((n[win] < 3).sum()),
                      "pop_below2_win": int((self.npop[k][win] < 2).sum()),
                      "pop_below3_win": int((self.npop[k][win] < 3).sum()), "universe_win": int(win.sum()),"universe": int(uni.sum()), "min": int(v.min()), "p05": float(np.percentile(v, 5)),
                      "median": float(np.median(v)), "below2": int((v < 2).sum()), "below3": int((v < 3).sum()),
                      "pop_below2": int((p < 2).sum()), "pop_below3": int((p < 3).sum()),
                      "sats_below2": int(((n < 2) & uni).any(axis=0).sum())}
        return out

    def describe(self, members):
        hs = [self.st[i] for i in members]
        types = Counter(h["rec_type"] for h in hs)
        u = unit_of(np.array([h["lat"] for h in hs]), np.array([h["lon"] for h in hs]))
        d = np.arccos(np.clip(fib_grid(4000) @ u.T, -1, 1)).min(axis=1) * R_EARTH_KM
        rows = sum(int(sum(self.rowcnt[i].get(s, np.zeros(1)).sum() for s in ("G", "E"))) for i in members)
        return {"n": len(members), "makes": dict(Counter(h["make"] for h in hs).most_common()),
                "types": len(types), "types_ge2": sum(1 for v in types.values() if v >= 2),
                "antennas": len(Counter(h["ant"] for h in hs)), "radomes": len(Counter(h["radome"] for h in hs)),
                "regions": len({h["region"] for h in hs}), "hole_km": float(d.max()),
                "hole_p95_km": float(np.percentile(d, 95)), "mean_gap_km": float(d.mean()),
                "rows_GE": rows, "pseudo": sum(1 for h in hs if h["pseudo"]),
                "galileo_family": dict(Counter(galileo_family(self, i) for i in members))}


def galileo_family(pop, i):
    sh = pop.share[i]
    q, x = sh.get("E5Q", 0), sh.get("E5X", 0)
    return "C/Q" if q >= x and q > 0 else ("X" if x > 0 else "none")


# ------------------------------------------------------------------------------------------- selection
def gvec(n):
    """Worth of the next tracker of a satellite-epoch already tracked by n stations: 4^(2-n)."""
    return np.power(4.0, 2.0 - n)


class Designer:
    def __init__(self, pop, classes, region_classes, dmin, reserve_max_type, keep_users, users_n):
        self.pop, self.classes, self.dmin, self.users_n = pop, classes, dmin, users_n
        th = np.arange(0, NEPOCH, pop.thin)
        self.M, self.w = {}, {}
        for k in classes:
            cols = np.flatnonzero((pop.npop[k][th] > 0).ravel())
            m = np.zeros((len(pop.st), len(cols)), bool)
            for i in range(len(pop.st)):
                u = pop.unpack(i, k)
                if u is not None:
                    m[i] = u[th].ravel()[cols]
            self.M[k] = m
            self.w[k] = 1.0 / max(1, len(cols))
        st = pop.st
        elig = np.flatnonzero(pop.elig)
        self.keep_users = [pop.index[n] for n in keep_users if n in pop.index and st[pop.index[n]]["eligible"]]
        self.type_cnt = Counter(st[i]["rec_type"] for i in elig)
        self.make_cnt = Counter(st[i]["make"] for i in elig)
        self.reserved_types = {t for t, c in self.type_cnt.items() if c <= reserve_max_type}
        # network cap per make: leave min(2, e_m - 1) eligible stations of the make for the users
        self.make_cap = {m: c - min(2, c - 1) for m, c in self.make_cnt.items()}
        self.cand = np.array([bool(pop.elig[i]) and st[i]["rec_type"] not in self.reserved_types
                              and i not in self.keep_users for i in range(len(st))])
        q = []
        for k in classes:
            for m in sorted(self.make_cnt):
                mask = np.array([self.cand[i] and st[i]["make"] == m and pop.delivers(i, k, dmin) for i in range(len(st))])
                if mask.any():
                    q.append(("Q1 %s in %s" % (m, k), mask, 1))
        for t, c in sorted(self.type_cnt.items()):
            if c >= 3:
                mask = np.array([self.cand[i] and st[i]["rec_type"] == t for i in range(len(st))])
                q.append(("Q2 type %s" % t, mask, min(2, int(mask.sum()))))
        for k in region_classes:
            if k in classes:
                for r in range(12):
                    mask = np.array([self.cand[i] and st[i]["region"] == r and pop.delivers(i, k, dmin)
                                     for i in range(len(st))])
                    if mask.sum() >= 2:
                        q.append(("Q3 region %d %s" % (r, k), mask, 2))
        for r in range(12):
            mask = np.array([self.cand[i] and st[i]["region"] == r for i in range(len(st))])
            if mask.any():
                q.append(("Q4 region %d" % r, mask, min(2, int(mask.sum()))))
        self.q_lab = [x[0] for x in q]
        self.Q = np.array([x[1] for x in q]) if q else np.zeros((0, len(st)), bool)
        self.need = np.array([x[2] for x in q], int)

    def counts(self, S):
        return {k: (self.M[k][S].sum(axis=0) if S else np.zeros(self.M[k].shape[1], np.int64)) for k in self.classes}

    def gains(self, n):
        g = np.zeros(len(self.pop.st))
        for k in self.classes:
            g += self.M[k] @ (self.w[k] * gvec(n[k]))
        return g

    def deficit(self, S):
        have = self.Q[:, S].sum(axis=1) if S else np.zeros(len(self.need), int)
        return np.maximum(0, self.need - have)

    def allowed(self, S):
        mk = Counter(self.pop.st[i]["make"] for i in S)
        ok = self.cand.copy()
        ok[list(S)] = False
        for i in np.flatnonzero(ok):
            m = self.pop.st[i]["make"]
            if mk[m] >= self.make_cap.get(m, 0):
                ok[i] = False
        return ok

    def greedy(self, N, S=None, record=None):
        """Quota units first (largest deficit reduction), coverage gain as the tie-break; then gain only."""
        S = list(S or [])
        n = self.counts(S)
        while len(S) < N:
            ok = self.allowed(S)
            if not ok.any():
                break
            g = self.gains(n)
            d = self.deficit(S)
            red = self.Q[d > 0].sum(axis=0) if d.any() else np.zeros(len(g))
            c = int(np.argmax(np.where(ok, red * 1e6 + g, -np.inf)))
            S.append(c)
            for k in self.classes:
                n[k] = n[k] + self.M[k][c]
            if record is not None:
                record(S, n)
        return S

    def exchange(self, S, max_iter=400):
        """Pairwise swaps that raise the utility without opening a quota deficit."""
        S = list(S)
        n = self.counts(S)
        base_def = int(self.deficit(S).sum())
        for _ in range(max_iter):
            loss = np.zeros(len(S))
            for k in self.classes:
                loss += self.M[k][S] @ (self.w[k] * gvec(n[k] - 1))
            done = False
            for j in np.argsort(loss)[:12]:
                s = S[j]
                rest = S[:j] + S[j + 1:]
                n2 = {k: n[k] - self.M[k][s] for k in self.classes}
                g = self.gains(n2)
                ok = self.allowed(rest)
                ok[s] = False
                for c in np.argsort(-np.where(ok, g, -np.inf))[:20]:
                    if not ok[c] or g[c] <= loss[j] * (1 + 1e-6):
                        break
                    T = rest + [int(c)]
                    if int(self.deficit(T).sum()) <= base_def:
                        S, n = T, {k: n2[k] + self.M[k][c] for k in self.classes}
                        done = True
                        break
                if done:
                    break
            if not done:
                break
        return S

    def design(self, N):
        S = self.exchange(self.greedy(N))
        return sorted(S, key=lambda i: self.pop.names[i]), [self.q_lab[i] for i in np.flatnonzero(self.deficit(S))]

    def users(self, S):
        """Held-out users: >= 2 per make where possible, >= 1/3 of a type outside the network and >= 1/3 inside;
        farthest point among the users for the rest."""
        pop, st = self.pop, self.pop.st
        net_types = {st[i]["rec_type"] for i in S}
        avail = set(int(i) for i in np.flatnonzero(pop.elig)) - set(S)
        U = [i for i in self.keep_users if i in avail]
        third = int(math.ceil(self.users_n / 3))
        units = []
        for m in sorted(self.make_cnt):
            mask = np.array([i in avail and st[i]["make"] == m for i in range(len(st))])
            if mask.any():
                units.append((mask, min(2, int(mask.sum()))))
        units.append((np.array([i in avail and st[i]["rec_type"] not in net_types for i in range(len(st))]), third))
        units.append((np.array([i in avail and st[i]["rec_type"] in net_types for i in range(len(st))]), third))
        Qu = np.array([u[0] for u in units])
        need = np.array([u[1] for u in units])
        xyz = unit_of(np.array([h["lat"] for h in st]), np.array([h["lon"] for h in st]))
        while len(U) < self.users_n:
            rest = sorted(avail - set(U))
            if not rest:
                break
            d = np.maximum(0, need - (Qu[:, U].sum(axis=1) if U else 0))
            red = Qu[d > 0][:, rest].sum(axis=0) if d.any() else np.zeros(len(rest))
            ref = U if U else list(S)
            dist = np.arccos(np.clip(xyz[rest] @ xyz[ref].T, -1, 1)).min(axis=1)
            U.append(rest[int(np.lexsort((-dist, -red))[0])])
        return sorted(U, key=lambda i: pop.names[i])


# ------------------------------------------------------------------------------------------- commands
def read_set(spec, pop):
    if spec == "ALL":
        return [i for i, h in enumerate(pop.st) if not h["pseudo"]]
    if spec.endswith(".json"):
        cfg = json.load(open(spec, encoding="utf-8"))
        names = [s["name"] if isinstance(s, dict) else s for s in cfg["network"]["stations"]]
    else:
        names = [l.split()[0] for l in open(spec) if l.strip() and not l.startswith("#")]
    miss = [n for n in names if n.upper() not in pop.index]
    if miss:
        print("  not in this population:", miss)
    return [pop.index[n.upper()] for n in names if n.upper() in pop.index]


def parse_exclude(items):
    out = {}
    for it in items or []:
        name, _, why = it.partition(":")
        out[name.upper()] = why or "excluded by argument"
    return out


def write_inventory(pop, path):
    cls = pop.classes
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "make", "rec_type", "firmware", "antenna", "radome", "atx", "atx_galileo", "lat", "lon",
                    "height_m", "region", "completeness", "eligible", "reason"] + ["declared_" + s for s in "GEC"]
                   + ["share_" + k for k in cls])
        for i, h in enumerate(pop.st):
            if h["pseudo"]:
                continue
            dec = [" ".join(t for t in h["obs"].get(s, []) if t[0] in "CL") for s in "GEC"]
            w.writerow([h["name"], h["make"], h["rec_type"], h["rec_fw"], h["ant"], h["radome"], h["atx"],
                        int(h["atx_gal"]), "%.4f" % h["lat"], "%.4f" % h["lon"], "%.1f" % h["hgt"], h["region"],
                        "%.3f" % h["completeness"], int(h["eligible"]), h["reason"]] + dec
                       + ["%.3f" % pop.share[i].get(k, 0.0) for k in cls])


def load(a):
    return Population(a.cache, a.systems, a.atx, parse_exclude(a.exclude), a.thin)


def cmd_inventory(a):
    pop = load(a)
    write_inventory(pop, a.out)
    print("%d stations, %d eligible; makes %s" % (len(pop.st), pop.elig.sum(), dict(Counter(h["make"] for h in pop.st))))
    print("ineligible:", Counter(h["reason"] for h in pop.st if not h["eligible"]).most_common(8))
    for k in pop.classes:
        nd = sum(1 for i in range(len(pop.st)) if pop.delivers(i, k, 0.8))
        nany = sum(1 for i in range(len(pop.st)) if pop.share[i].get(k, 0) > 0)
        print("  %-5s delivered >= 80 %% at %3d stations, any at %3d; makes %s" % (k, nd, nany, dict(Counter(
            pop.st[i]["make"] for i in range(len(pop.st)) if pop.delivers(i, k, 0.8)))))


def cmd_coverage(a):
    pop = load(a)
    S = read_set(a.set, pop)
    print(json.dumps(pop.describe(S)))
    for k, c in pop.coverage(S).items():
        print("%-5s universe %7d min %2d median %5.1f below2 %6d below3 %6d (population %d / %d)" % (
            k, c["universe"], c["min"], c["median"], c["below2"], c["below3"], c["pop_below2"], c["pop_below3"]))


def objective_classes(pop, min_stations, dmin):
    return [k for k in pop.classes
            if sum(1 for i in np.flatnonzero(pop.elig) if pop.delivers(i, k, dmin) and not pop.st[i]["pseudo"])
            >= min_stations]


def cmd_design(a):
    pop = load(a)
    tag = a.tag or os.path.basename(os.path.normpath(pop.meta["day"]))
    os.makedirs(a.out, exist_ok=True)
    write_inventory(pop, os.path.join(a.out, "inventory_%s.csv" % tag))
    classes = objective_classes(pop, a.min_class_stations, a.deliver_min)
    if a.candidates:
        other = Population(a.candidates, a.systems, a.atx, parse_exclude(a.exclude), a.thin)
        extra = [n for n in other.names if n not in pop.index and other.st[other.index[n]]["eligible"]]
        pop.add_pseudo(other, extra, a.deliver_min)
        with open(os.path.join(a.out, "download_candidates_%s.csv" % tag), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["name", "make", "rec_type_other_day", "antenna_other_day", "lat", "lon", "note"])
            for n in extra:
                h = pop.st[pop.index[n]]
                w.writerow([n, h["make"], h["rec_type"], h["ant"], "%.3f" % h["lat"], "%.3f" % h["lon"],
                            "not on disk for this day; receiver from the other day's header; availability unverified"])
    D = Designer(pop, classes, a.region_classes.split(","), a.deliver_min, a.reserve_max_type,
                 a.keep_users.split(",") if a.keep_users else [], a.users)
    print("%s: %d stations (%d pseudo), %d candidates, %d objective classes, %d quota units, %d reserved types"
          % (tag, len(pop.st), sum(h["pseudo"] for h in pop.st), D.cand.sum(), len(classes), len(D.need),
             len(D.reserved_types)), flush=True)
    result = {"tag": tag, "classes": classes, "all_classes": pop.classes, "sets": {}, "quota_units": D.q_lab,
              "reserved_types": sorted(D.reserved_types), "make_cap": D.make_cap}

    def entry(label, S, unmet=None, users=None):
        cov = pop.coverage(S)
        e = {"stations": [pop.names[i] for i in S], "describe": pop.describe(S), "coverage": cov,
             "unmet_quotas": unmet or []}
        if users is not None:
            net_types = {pop.st[i]["rec_type"] for i in S}
            e["users"] = [pop.names[i] for i in users]
            e["users_describe"] = pop.describe(users)
            e["users_outside_type"] = [pop.names[i] for i in users if pop.st[i]["rec_type"] not in net_types]
        result["sets"][label] = e
        short = [k for k in classes if cov[k]["below2"] > cov[k]["pop_below2"]]
        print("  %-14s n=%3d  makes %s  classes short of 2 beyond the population: %s" % (
            label, len(S), e["describe"]["makes"], ",".join(short) or "none"), flush=True)

    for cfg in a.compare or []:
        entry("today", read_set(cfg, pop))
    entry("all_on_disk", [i for i, h in enumerate(pop.st) if not h["pseudo"]])
    if a.candidates:
        entry("all_with_downloads", list(range(len(pop.st))))
    for N in [int(x) for x in a.sizes.split(",")]:
        S, unmet = D.design(N)
        entry("design%d" % N, S, unmet, D.users(S))
    # saturation: one greedy order over every candidate; size at which each class reaches its floor (thin grid)
    curve = {k: [] for k in classes}

    def rec(S, n):
        for k in classes:
            curve[k].append((len(S), int((n[k] < 2).sum()), int((n[k] < 3).sum())))

    D.greedy(int(D.cand.sum()), record=rec)
    sat = {}
    for k in classes:
        cn = D.M[k][D.cand].sum(axis=0)
        f2, f3 = int((cn < 2).sum()), int((cn < 3).sum())
        sat[k] = {"size_floor2": next((m for m, b2, _ in curve[k] if b2 <= f2), None),
                  "size_floor3": next((m for m, _, b3 in curve[k] if b3 <= f3), None),
                  "floor2_thin": f2, "floor3_thin": f3, "thin_universe": int(D.M[k].shape[1]),
                  "curve": curve[k][::5]}
    result["saturation"] = sat
    json.dump(result, open(os.path.join(a.out, "design_%s.json" % tag), "w"), indent=1)
    for label, e in result["sets"].items():
        if not label.startswith("design"):
            continue
        with open(os.path.join(a.out, "stations_%s_%s.txt" % (tag, label)), "w") as f:
            f.write("# %s network, %d stations (scripts/station_design.py design, rules in its docstring)\n"
                    % (tag, len(e["stations"])))
            for n in e["stations"]:
                h = pop.st[pop.index[n]]
                f.write("%s  %-10s %-22s %-16s %s\n" % (n, h["make"], h["rec_type"], h["ant"],
                                                     "needs download" if h["pseudo"] else ""))
            f.write("# held-out users, %d\n" % len(e["users"]))
            for n in e["users"]:
                h = pop.st[pop.index[n]]
                f.write("# user %s  %-10s %-22s %-16s %s%s\n" % (
                    n, h["make"], h["rec_type"], h["ant"], "type outside the network" if n in e["users_outside_type"]
                    else "type inside the network", "; needs download" if h["pseudo"] else ""))
    print("wrote", a.out)


# ---------------------------------------------------------------------------------------------- tables
MAKE_ABBR = {"Septentrio": "Sep", "Trimble": "Tri", "Javad": "Jav", "Leica": "Lei", "Topcon": "Top",
             "Stonex": "Sto", "Other": "Oth"}


def cmd_tables(a):
    """Markdown tables of one design JSON: the sets, coverage per class, saturation, users."""
    r = json.load(open(a.json))
    sets = r["sets"]
    order = [k for k in ("today", "design40", "design55", "design60", "design80", "design100", "all_on_disk",
                         "all_with_downloads") if k in sets]
    base = sets.get("today", sets[order[0]])["describe"]["rows_GE"]
    out = ["## %s: the station sets" % r["tag"], "",
           "| set | stations | makes | receiver types (with 2 or more) | antennas | regions of 12 | largest gap km "
           "| mean gap km | Galileo C/Q : X | observation load vs today |", "|" + "---|" * 10]
    for k in order:
        d = sets[k]["describe"]
        mk = " ".join("%s %d" % (MAKE_ABBR.get(m, m), c) for m, c in d["makes"].items())
        gf = d.get("galileo_family", {})
        out.append("| %s | %d%s | %s | %d (%d) | %d | %d | %.0f | %.0f | %d : %d | %.2f |" % (
            k, d["n"], " (%d to download)" % d["pseudo"] if d.get("pseudo") else "", mk, d["types"], d["types_ge2"],
            d["antennas"], d["regions"], d["hole_km"], d["mean_gap_km"], gf.get("C/Q", 0), gf.get("X", 0),
            d["rows_GE"] / base))
    out += ["", "Satellite-epochs (30 s, elevation 15 deg or more) tracked by fewer than 2 / fewer than 3 stations of "
            "the set, per signal class; `pop` is the floor of every station on disk. Classes in the selection "
            "objective only.", "",
            "| class | universe | " + " | ".join(order) + " | pop floor |", "|" + "---|" * (len(order) + 3)]
    for c in r["classes"]:
        cells = []
        for k in order:
            v = sets[k]["coverage"].get(c)
            cells.append("%d / %d" % (v["below2"], v["below3"]) if v else "-")
        v0 = sets["all_on_disk"]["coverage"][c]
        out.append("| %s | %d | %s | %d / %d |" % (c, v0["universe"], " | ".join(cells), v0["pop_below2"],
                                                    v0["pop_below3"]))
    core = [c for c in ("G1C", "G1W", "G2W", "G2L", "G5Q", "G5X", "E1C", "E1X", "E5Q", "E5X", "C2I", "C6I")
            if c in r["classes"]]
    out += ["", "The same count inside the 00-06 UT window of the fast tests (720 epochs), classes of the goldens "
            "(GPS L1/L2, Galileo E1/E5a, BeiDou B1I/B3I) and GPS L5.", "",
            "| class | universe | " + " | ".join(order) + " | pop floor |", "|" + "---|" * (len(order) + 3)]
    for c in core:
        cells = []
        for k in order:
            v = sets[k]["coverage"].get(c)
            cells.append("%d / %d" % (v["below2_win"], v["below3_win"]) if v and "below2_win" in v else "-")
        v0 = sets["all_on_disk"]["coverage"][c]
        out.append("| %s | %d | %s | %d / %d |" % (c, v0.get("universe_win", 0), " | ".join(cells),
                                                    v0.get("pop_below2_win", 0), v0.get("pop_below3_win", 0)))
    out += ["", "Size of a greedy station order at which each class reaches the floor of all candidates "
            "(fewer than 2, fewer than 3; 2-min grid).", "", "| class | reaches its floor of 2 at | floor of 3 at | "
            "floor (<2 / <3, 2-min sat-epochs) |", "|---|---|---|---|"]
    for c in r["classes"]:
        s = r["saturation"][c]
        out.append("| %s | %s | %s | %d / %d of %d |" % (c, s["size_floor2"], s["size_floor3"], s["floor2_thin"],
                                                         s["floor3_thin"], s["thin_universe"]))
    out += ["", "Held-out users of each design (rules in the tool docstring).", "",
            "| design | users | makes | of a type outside the network | unmet network quotas |", "|---|---|---|---|---|"]
    for k in order:
        e = sets[k]
        if "users" not in e:
            continue
        mk = " ".join("%s %d" % (MAKE_ABBR.get(m, m), c) for m, c in e["users_describe"]["makes"].items())
        out.append("| %s | %d | %s | %d | %s |" % (k, len(e["users"]), mk, len(e["users_outside_type"]),
                                                  "; ".join(e["unmet_quotas"]) or "none"))
    path = a.out or os.path.join(os.path.dirname(a.json), os.path.basename(a.json).replace("design_", "tables_")[:-5] + ".md")
    open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
    print("wrote", path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sc = sub.add_parser("scan")
    sc.add_argument("--day", required=True)
    sc.add_argument("--cache", required=True)
    sc.add_argument("--mask", type=float, default=15.0)
    sc.add_argument("--workers", type=int, default=12)
    sc.add_argument("--skip-done", action="store_true")
    for name in ("inventory", "coverage", "design"):
        p = sub.add_parser(name)
        p.add_argument("--cache", required=True)
        p.add_argument("--systems", default="GEC")
        p.add_argument("--atx", default="C:/GnssProcessor/data/static/atx/igs20.atx")
        p.add_argument("--exclude", nargs="*", default=DEFAULT_EXCLUDE)
        p.add_argument("--thin", type=int, default=4, help="epoch thinning of the selection matrices")
        if name == "inventory":
            p.add_argument("--out", required=True)
        if name == "coverage":
            p.add_argument("--set", required=True, help="config .json, station list .txt, or ALL")
        if name == "design":
            p.add_argument("--out", required=True)
            p.add_argument("--tag", default="")
            p.add_argument("--sizes", default="40,60,80,100")
            p.add_argument("--compare", nargs="*", help="today's network config, graded beside the designs")
            p.add_argument("--candidates", default="", help="cache of another day: its extra stations as pseudo")
            p.add_argument("--deliver-min", type=float, default=0.8)
            p.add_argument("--min-class-stations", type=int, default=3)
            p.add_argument("--region-classes", default="E5Q,E5X")
            p.add_argument("--reserve-max-type", type=int, default=1)
            p.add_argument("--keep-users", default="WTZZ00DEU")
            p.add_argument("--users", type=int, default=16)
    tb = sub.add_parser("tables")
    tb.add_argument("--json", required=True, help="design_<tag>.json written by the design step")
    tb.add_argument("--out", default="")
    a = ap.parse_args()
    if a.cmd == "scan":
        cmd_scan(a)
    elif a.cmd == "tables":
        cmd_tables(a)
    else:
        {"inventory": cmd_inventory, "coverage": cmd_coverage, "design": cmd_design}[a.cmd](a)


if __name__ == "__main__":
    main()

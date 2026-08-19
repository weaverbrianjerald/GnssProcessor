"""Diurnal sweep of the receiver fleet GF-bias term (SPEC §17 item 0's diagnostic rider).

Same estimator-free instrument as scripts/extcb_gim_verdict.py (raw RINEX + COD SP3/OSB
vs the day's GIM), swept over hour buckets: per-station bias = median over ~10 sats x 5
epochs of  y_slant_tecu - m(el)*GIM_h(IPP), then the fleet mean/median/std per hour.
Receiver thermal drift => station-specific slow curves; GIM/satellite-side error drift
=> common-mode moves.  EL_CUT and station subsample match the 00:00 verdict script.
"""
import csv, glob, math, statistics as st
from math import sin, cos, asin, radians, degrees, pi, sqrt

C = 299792458.0
OMEGA_E = 7.2921151467e-5
F1, F2 = 1575.42e6, 1227.60e6
KGF = (F1 / F2) ** 2 - 1.0
M_TECU_L1 = 40.3e16 / (F1 * F1)
RE, HSH = 6371000.0, 450000.0
EL_CUT = 20.0
root = r"C:\GnssProcessor\data\2024-129"
HOURS = [0, 2, 4, 6, 8, 10]
EP_OFF = [0.0, 30.0, 60.0, 90.0, 120.0]

def load_gim(path):
    maps = []
    with open(path, errors="replace") as f:
        lines = f.readlines()
    i = 0; exp10 = -1
    while i < len(lines):
        lab = lines[i][60:].strip()
        if lab == "EXPONENT": exp10 = int(lines[i][:60].split()[0])
        if lab == "START OF TEC MAP":
            grid = {}; j = i + 1
            while lines[j][60:].strip() != "END OF TEC MAP":
                if lines[j][60:].strip() == "LAT/LON1/LON2/DLON/H":
                    s = lines[j]
                    lat = float(s[2:8]); lon1 = float(s[8:14]); lon2 = float(s[14:20]); dlon = float(s[20:26])
                    nlon = int(round((lon2 - lon1) / dlon)) + 1
                    vals = []; k = j + 1
                    while len(vals) < nlon:
                        vals += [int(lines[k][x:x+5]) for x in range(0, min(80, len(lines[k].rstrip())), 5)]
                        k += 1
                    grid[lat] = (lon1, dlon, [v * 10.0 ** exp10 for v in vals])
                    j = k
                else: j += 1
            maps.append(grid); i = j
        i += 1
    return maps

MAPS = load_gim(glob.glob(root + r"\products\*GIM.INX")[0])

def gim_vtec(g, lat, lon):
    lats = sorted(g.keys())
    lon = (lon + 180.0) % 360.0 - 180.0
    la = max(min(lat, max(lats)), min(lats))
    l0 = max(l for l in lats if l <= la); l1 = min(l for l in lats if l >= la)
    def row(lv):
        lon1, dlon, vals = g[lv]
        x = (lon - lon1) / dlon
        i0 = max(0, min(len(vals) - 2, int(math.floor(x))))
        f = x - i0
        return vals[i0] * (1 - f) + vals[i0 + 1] * f
    if l1 == l0: return row(l0)
    f = (la - l0) / (l1 - l0)
    return row(l0) * (1 - f) + row(l1) * f

sta_xyz = {}
with open(r"C:\GnssProcessor\runs\ext63_dual_20ep_rxcb_loose\logs\network_stations.csv") as f:
    for row in csv.DictReader(f):
        sta_xyz[row["station"]] = (float(row["ecef_x"]), float(row["ecef_y"]), float(row["ecef_z"]))
stations = sorted(sta_xyz)[::2]

pos = {}
with open(sorted(glob.glob(root + r"\products\*.SP3"))[0]) as f:
    sod = None
    for line in f:
        if line.startswith("*"):
            p = line.split()
            sod = int(p[4]) * 3600 + int(p[5]) * 60 + int(float(p[6])) + (86400 if int(p[3]) != 8 else 0)
        elif line.startswith("PG") and sod is not None:
            pos.setdefault(line[1:4], {})[sod] = (float(line[4:18]) * 1e3,
                                                  float(line[18:32]) * 1e3, float(line[32:46]) * 1e3)

def sp3_interp(sat, t):
    ns = sorted(sorted(pos[sat]), key=lambda n: abs(n - t))[:8]; ns.sort()
    out = [0.0, 0.0, 0.0]
    for i, ni in enumerate(ns):
        w = 1.0
        for j, nj in enumerate(ns):
            if i != j: w *= (t - nj) / (ni - nj)
        for k in range(3): out[k] += w * pos[sat][ni][k]
    return out

osb = {}
with open(glob.glob(root + r"\products\*OSB.BIA")[0]) as f:
    in_sol = False
    for line in f:
        if line.startswith("+BIAS/SOLUTION"): in_sol = True; continue
        if line.startswith("-BIAS/SOLUTION"): in_sol = False; continue
        if not in_sol or not line.startswith(" OSB"): continue
        prn = line[11:14].strip(); sig = line[25:29].strip(); val = line[70:91].strip()
        if len(prn) == 3 and prn[0] == "G" and sig and val:
            try: osb[(prn, sig)] = float(val) * C * 1e-9
            except ValueError: pass

WANT = sorted(h * 3600 + o for h in HOURS for o in EP_OFF)
WANT_SET = set(WANT)

def read_rnx(path):
    obs_types = []; data = {}; cur = None
    with open(path, errors="replace") as f:
        hdr = True; grabbing = False
        for line in f:
            if hdr:
                lab = line[60:].strip()
                if lab == "SYS / # / OBS TYPES":
                    if line[0] == "G": grabbing = True; obs_types = line[7:60].split()
                    elif line[0] == " " and grabbing: obs_types += line[7:60].split()
                    else: grabbing = False
                elif lab == "END OF HEADER": hdr = False
                continue
            if line.startswith(">"):
                p = line.split()
                sod = int(p[4]) * 3600 + int(p[5]) * 60 + float(p[6])
                cur = sod if sod in WANT_SET else None
                if sod > WANT[-1]: break
                continue
            if cur is None or not line.startswith("G"): continue
            rec = {}
            for i, ot in enumerate(obs_types):
                s = line[3 + 16 * i: 3 + 16 * i + 14]
                if s.strip():
                    try: rec[ot] = float(s)
                    except ValueError: pass
            data[(cur, line[0:3])] = rec
    return data

def unit(v):
    n = sqrt(sum(x * x for x in v)); return [x / n for x in v]

# --------------------------------------------------------------------------
# Geodesy -- port of math/Coordinates.cpp, so the pierce point this script
# builds is the SAME one computeIppGeometry() builds in the C++.  The seed and
# the local frame were previously GEOCENTRIC (lat = asin(z/|r|), up = r/|r|)
# where the C++ uses the GEODETIC latitude and the ellipsoid-normal ENU frame:
# up to 0.192 deg of latitude at 45 deg, carried into el -> psi -> the mapping
# m(el) that multiplies GIM VTEC in the residual.  Same fixed-point iteration
# as scripts/q5_reanchor_hourly.py, for the same reason -- a different but
# equally valid solver would leave a residue indistinguishable from a defect.
# --------------------------------------------------------------------------
_A = 6378137.0
_F = 1.0 / 298.257223563
_E2 = _F * (2.0 - _F)


def ecef_to_llh(ecef):
    """Port of ``Coordinates::ecefToLlh`` (Bowring seed + <=10 fixed-point steps)."""
    x, y, z = (float(v) for v in ecef)
    p = math.hypot(x, y)
    lon = math.atan2(y, x)
    if p < 1e-12:
        lat = math.copysign(math.pi / 2.0, z if z != 0.0 else 1.0)
        n = _A / math.sqrt(1.0 - _E2 * math.sin(lat) ** 2)
        return lat, lon, abs(z) - n * (1.0 - _E2)
    lat = math.atan2(z, p * (1.0 - _E2))
    h = 0.0
    for _ in range(10):
        n = _A / math.sqrt(1.0 - _E2 * math.sin(lat) ** 2)
        h = p / math.cos(lat) - n
        denom = p * (1.0 - _E2 * (n / (n + h)))
        if abs(denom) < 1e-15:
            break
        lat_next = math.atan2(z, denom)
        if abs(lat_next - lat) < 1e-13:
            lat = lat_next
            break
        lat = lat_next
    return lat, lon, h


def geodetic_up(lat, lon):
    """Ellipsoid-normal Up in ECEF -- column 2 of ``Coordinates::enuToEcefMatrix``."""
    return [cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat)]


# hour -> station -> [residual slant tecu]
resid = {h: {} for h in HOURS}
for sta in stations:
    files = glob.glob(root + r"\obs" + "\\" + sta.lower() + r"\*.rnx")
    if not files: continue
    rx = sta_xyz[sta]
    lat_r, lon_r, _ = ecef_to_llh(rx)          # GEODETIC, matching computeIppGeometry
    up_r = geodetic_up(lat_r, lon_r)           # ellipsoid normal, not r/|r|
    try: data = read_rnx(files[0])
    except Exception: continue
    for (sod, sat), rec in data.items():
        p1, p2 = rec.get("C1C"), rec.get("C2W")
        o1, o2 = osb.get((sat, "C1C")), osb.get((sat, "C2W"))
        if None in (p1, p2, o1, o2) or sat not in pos: continue
        h = int(sod // 3600)
        sp = sp3_interp(sat, sod - 0.075)
        ang = OMEGA_E * 0.075
        sr = [sp[0] * cos(ang) + sp[1] * sin(ang), -sp[0] * sin(ang) + sp[1] * cos(ang), sp[2]]
        d = [sr[i] - rx[i] for i in range(3)]
        u = up_r; dn = unit(d)
        el = asin(sum(a * b for a, b in zip(dn, u)))
        if degrees(el) < EL_CUT: continue
        e_east = unit([-rx[1], rx[0], 0.0])
        e_north = [u[1] * e_east[2] - u[2] * e_east[1],
                   u[2] * e_east[0] - u[0] * e_east[2],
                   u[0] * e_east[1] - u[1] * e_east[0]]
        az = math.atan2(sum(a * b for a, b in zip(dn, e_east)),
                        sum(a * b for a, b in zip(dn, e_north)))
        psi = pi / 2 - el - asin(RE / (RE + HSH) * cos(el))
        lat_i = asin(sin(lat_r) * cos(psi) + cos(lat_r) * sin(psi) * cos(az))
        lon_i = lon_r + asin(sin(psi) * sin(az) / cos(lat_i))
        m = 1.0 / sqrt(1.0 - (RE / (RE + HSH) * cos(el)) ** 2)
        y_tecu = ((p2 - o2) - (p1 - o1)) / KGF / M_TECU_L1
        g = gim_vtec(MAPS[h], degrees(lat_i), degrees(lon_i))
        resid[h].setdefault(sta, []).append(y_tecu - m * g)

print("hour | n_sta | fleet bias mean / median / std (slant TECU)")
bias = {}
for h in HOURS:
    b = {s: st.median(v) for s, v in resid[h].items() if len(v) >= 12}
    bias[h] = b
    vs = sorted(b.values())
    if vs:
        print(f"  {h:02d} | {len(vs):3d}  | {st.mean(vs):+7.2f} / {st.median(vs):+7.2f} / {st.pstdev(vs):6.2f}")

# per-station drift: hour-0-referenced
common = [s for s in bias[HOURS[0]] if all(s in bias[h] for h in HOURS)]
print(f"\nper-station drift vs its own 00h bias ({len(common)} stations present in all buckets):")
drifts = {h: [bias[h][s] - bias[HOURS[0]][s] for s in common] for h in HOURS}
for h in HOURS:
    dv = sorted(drifts[h])
    print(f"  {h:02d}h: mean {st.mean(dv):+6.2f}  median {st.median(dv):+6.2f}  "
          f"sd {st.pstdev(dv):5.2f}  IQR [{dv[len(dv)//4]:+.2f},{dv[3*len(dv)//4]:+.2f}]")
print("\nlargest per-station drifts at 10h (station: 00h bias -> 10h bias):")
for s in sorted(common, key=lambda s: -abs(bias[10][s] - bias[0][s]))[:8]:
    print(f"  {s}: {bias[0][s]:+7.2f} -> {bias[10][s]:+7.2f}  (d {bias[10][s]-bias[0][s]:+.2f})")

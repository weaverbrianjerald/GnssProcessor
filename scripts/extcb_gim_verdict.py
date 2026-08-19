"""GIM-referenced closing analysis for DEFECT 2.

Per LOS (29 stations x ~10 sats x 5 epochs, el>20):
  y_tecu   = [(P2W-osb2)-(P1C-osb1)]/KGF/M_TECU  (slant TECU + rx bias/M)
  r_gim    = y_tecu - m(el)*GIM(IPP)             -> per-station median = rx bias term
  sat verdict: per-sat median of (r_gim - sta_bias) -> satellite GF error vs GIM
  field:   VTEC_net(IPP) from network_vtec_sh.csv (d15 run, ep1 and ep20)
           field-GIM per station -> where/how much the network field is low
  raw-GIM per station (instrument sanity)
"""
import csv, glob, math, statistics as st
from math import lgamma, sin, cos, asin, radians, degrees, pi, sqrt, exp

C = 299792458.0
OMEGA_E = 7.2921151467e-5
F1, F2 = 1575.42e6, 1227.60e6
KGF = (F1 / F2) ** 2 - 1.0
M_TECU_L1 = 40.3e16 / (F1 * F1)
RE, HSH = 6371000.0, 450000.0
EPOCH_SODS = [0.0, 30.0, 60.0, 90.0, 120.0]
EL_CUT = 20.0
WEEK, TOW0 = 2313, 259200.0
root = r"C:\GnssProcessor\data\2024-129"

# ---------------- GIM ----------------
def load_gim(path):
    maps = []
    with open(path, errors="replace") as f:
        lines = f.readlines()
    i = 0; exp10 = -1
    while i < len(lines):
        lab = lines[i][60:].strip()
        if lab == "EXPONENT": exp10 = int(lines[i][:60].split()[0])
        if lab == "START OF TEC MAP":
            grid = {}
            j = i + 1
            while lines[j][60:].strip() != "END OF TEC MAP":
                if lines[j][60:].strip() == "LAT/LON1/LON2/DLON/H":
                    s = lines[j]
                    lat = float(s[2:8]); lon1 = float(s[8:14]); lon2 = float(s[14:20]); dlon = float(s[20:26])
                    nlon = int(round((lon2 - lon1) / dlon)) + 1
                    vals = []
                    k = j + 1
                    while len(vals) < nlon:
                        vals += [int(lines[k][x:x + 5]) for x in range(0, min(80, len(lines[k].rstrip())), 5)]
                        k += 1
                    grid[lat] = (lon1, dlon, [v * 10.0 ** exp10 for v in vals])
                    j = k
                else:
                    j += 1
            maps.append(grid)
            i = j
        i += 1
    return maps

gim = load_gim(glob.glob(root + r"\products\*GIM.INX")[0])[0]     # 00:00 map
gim_lats = sorted(gim.keys())

def gim_vtec(lat, lon):
    lon = (lon + 180.0) % 360.0 - 180.0
    la = max(min(lat, max(gim_lats)), min(gim_lats))
    lo_i = [l for l in gim_lats if l <= la]; hi_i = [l for l in gim_lats if l >= la]
    l0, l1 = max(lo_i), min(hi_i)
    def row(lv):
        lon1, dlon, vals = gim[lv]
        x = (lon - lon1) / dlon
        i0 = int(math.floor(x)); i0 = max(0, min(len(vals) - 2, i0))
        f = x - i0
        return vals[i0] * (1 - f) + vals[i0 + 1] * f
    if l1 == l0: return row(l0)
    f = (la - l0) / (l1 - l0)
    return row(l0) * (1 - f) + row(l1) * f

# ---------------- network SH field ----------------
def norm_factor(n, m):
    d = 1.0 if m == 0 else 2.0
    return sqrt(d * (2 * n + 1) * exp(lgamma(n - m + 1) - lgamma(n + m + 1)))

def legendre(x, nmax):
    s = sqrt(max(0.0, 1.0 - x * x))
    p = {(0, 0): 1.0}
    for m in range(1, nmax + 1): p[(m, m)] = (2 * m - 1) * s * p[(m - 1, m - 1)]
    for m in range(0, nmax + 1):
        if m + 1 <= nmax: p[(m + 1, m)] = (2 * m + 1) * x * p[(m, m)]
        for n in range(m + 2, nmax + 1):
            p[(n, m)] = ((2 * n - 1) * x * p[(n - 1, m)] - (n + m - 1) * p[(n - 2, m)]) / (n - m)
    return p

def sun_lon(week, tow):
    jd = 2444244.5 + week * 7.0 + tow / 86400.0
    n = jd - 2451545.0
    L = radians((280.460 + 0.9856474 * n) % 360.0)
    g = radians((357.528 + 0.9856003 * n) % 360.0)
    lam = L + radians(1.915) * sin(g) + radians(0.020) * sin(2 * g)
    eps = radians(23.439 - 4.0e-7 * n)
    ra = math.atan2(cos(eps) * sin(lam), cos(lam))
    gmst = radians((280.46061837 + 360.98564736629 * n) % 360.0)
    return (ra - gmst + pi) % (2 * pi) - pi

def load_coeffs(csv_path, tow):
    out = []
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            if abs(float(row["tow"]) - tow) < 1e-6:
                out.append((int(row["layer"]), int(row["n"]), int(row["m"]),
                            row["trig"], float(row["value_tecu"])))
    return out

def field_vtec(coeffs, week, tow, lat, lon):
    ls = sun_lon(week, tow)
    s = radians(lon) - ls
    total = 0.0
    by_layer = {}
    for layer, n, m, trig, v in coeffs: by_layer.setdefault(layer, []).append((n, m, trig, v))
    for layer, rows in by_layer.items():
        nmax = max(n for n, _, _, _ in rows)
        p = legendre(sin(radians(lat)), nmax)
        for n, m, trig, v in rows:
            ang = cos(m * s) if trig == "C" else sin(m * s)
            total += v * p[(n, m)] * norm_factor(n, m) * ang
    return total

coef_ep1 = load_coeffs(r"C:\GnssProcessor\runs\ext63_dual_20ep_d15\logs\network_vtec_sh.csv", 259200.0)
coef_ep20 = load_coeffs(r"C:\GnssProcessor\runs\ext63_dual_20ep_d15\logs\network_vtec_sh.csv", 259770.0)

# ---------------- products / obs (as before) ----------------
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
        elif line.startswith("PG") and sod is not None and sod <= 3600:
            pos.setdefault(line[1:4], {})[sod] = (float(line[4:18]) * 1e3,
                                                  float(line[18:32]) * 1e3, float(line[32:46]) * 1e3)

def sp3_interp(sat, t):
    nodes = sorted(pos[sat]); ns = sorted(nodes, key=lambda n: abs(n - t))[:8]; ns.sort()
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

def read_rnx(path, want_sods):
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
                cur = sod if sod in want_sods else None
                if sod > max(want_sods): break
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


rows_all = []                                # (sta, sat, m, y_tecu, gim_ipp, net1_ipp, net20_ipp)
for sta in stations:
    files = glob.glob(root + r"\obs" + "\\" + sta.lower() + r"\*.rnx")
    if not files: continue
    rx = sta_xyz[sta]
    lat_r, lon_r, _ = ecef_to_llh(rx)          # GEODETIC, matching computeIppGeometry
    up_r = geodetic_up(lat_r, lon_r)           # ellipsoid normal, not r/|r|
    try:
        data = read_rnx(files[0], set(EPOCH_SODS))
    except Exception:
        continue
    for (sod, sat), rec in data.items():
        p1, p2 = rec.get("C1C"), rec.get("C2W")
        o1, o2 = osb.get((sat, "C1C")), osb.get((sat, "C2W"))
        if None in (p1, p2, o1, o2) or sat not in pos: continue
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
        la, lo = degrees(lat_i), degrees(lon_i)
        m = 1.0 / sqrt(1.0 - (RE / (RE + HSH) * cos(el)) ** 2)
        y_tecu = ((p2 - o2) - (p1 - o1)) / KGF / M_TECU_L1
        g = gim_vtec(la, lo)
        n1 = field_vtec(coef_ep1, WEEK, TOW0, la, lo)
        n20 = field_vtec(coef_ep20, WEEK, 259770.0, la, lo)
        rows_all.append((sta, sat, m, y_tecu, g, n1, n20))

# per-station receiver bias vs GIM; then satellite verdict; then field deltas
by_sta = {}
for r in rows_all: by_sta.setdefault(r[0], []).append(r)
sta_bias = {}; raw_minus_gim = {}; net1_minus_gim = {}; net20_minus_gim = {}
for sta, rs in by_sta.items():
    if len(rs) < 12: continue
    resid = [y - m * g for _, _, m, y, g, _, _ in rs]      # slant TECU incl. bias
    sta_bias[sta] = st.median(resid)
    raw_minus_gim[sta] = st.median([(y - sta_bias[sta]) / m - g for _, _, m, y, g, _, _ in rs])
    net1_minus_gim[sta] = st.median([n1 - g for _, _, _, _, g, n1, _ in rs])
    net20_minus_gim[sta] = st.median([n20 - g for _, _, _, _, g, _, n20 in rs])

sat_err = {}
for sta, rs in by_sta.items():
    if sta not in sta_bias: continue
    for _, sat, m, y, g, _, _ in rs:
        sat_err.setdefault(sat, []).append((y - sta_bias[sta] - m * g) / m)

print(f"LOS rows: {len(rows_all)}, stations: {len(sta_bias)}")
bs = sorted(sta_bias.values())
print(f"\nreceiver GF bias term (slant TECU): mean {st.mean(bs):+.2f}  median {st.median(bs):+.2f}  "
      f"std {st.pstdev(bs):.2f}  [{bs[0]:+.1f} .. {bs[-1]:+.1f}]")
print(f"  (equivalent metres of GF code: mean {st.mean(bs)*M_TECU_L1*KGF:+.3f} m)")

rg = list(raw_minus_gim.values()); n1g = list(net1_minus_gim.values()); n20g = list(net20_minus_gim.values())
print(f"\nraw-data VTEC minus GIM at IPPs (bias-removed, TECU): median {st.median(rg):+.2f}  "
      f"IQR [{sorted(rg)[len(rg)//4]:+.2f},{sorted(rg)[3*len(rg)//4]:+.2f}]")
print(f"NETWORK field (d15 ep1)  minus GIM at IPPs: median {st.median(n1g):+.2f}  "
      f"IQR [{sorted(n1g)[len(n1g)//4]:+.2f},{sorted(n1g)[3*len(n1g)//4]:+.2f}]")
print(f"NETWORK field (d15 ep20) minus GIM at IPPs: median {st.median(n20g):+.2f}  "
      f"IQR [{sorted(n20g)[len(n20g)//4]:+.2f},{sorted(n20g)[3*len(n20g)//4]:+.2f}]")

es = {s: st.median(v) for s, v in sat_err.items() if len(v) >= 10}
print(f"\nper-sat GF error vs GIM (TECU, geography removed): std {st.pstdev(list(es.values())):.2f}")
for s in sorted(es, key=lambda s: -abs(es[s]))[:8]:
    print(f"  {s}  {es[s]:+6.2f}  (n={len(sat_err[s])})")

print("\nper-station: bias_TECU  raw-GIM  net1-GIM  net20-GIM")
for sta in sorted(sta_bias, key=lambda s: net1_minus_gim.get(s, 0)):
    print(f"  {sta}  {sta_bias[sta]:+7.2f}  {raw_minus_gim[sta]:+7.2f}  "
          f"{net1_minus_gim[sta]:+7.2f}  {net20_minus_gim[sta]:+7.2f}")

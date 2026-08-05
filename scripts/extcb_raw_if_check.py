"""Estimator-free per-satellite IF code residuals from raw RINEX + COD products.

resid(sta,sat) = IF(C1C-osb, C2W-osb) - [ |r_rx - r_apc| - c*dt_CLK - rel + tropo ]
station-median removed (rx clock), then per-sat median over stations/epochs.
If e_IF(s) reproduces the loose-arm CLK residual pattern -> modeled-chain defect.
If e_IF(s) ~ 0 -> the +-4.5 m clock states are estimator redistribution.
"""
import csv, glob, math, statistics as st
from datetime import datetime, timezone

C = 299792458.0
OMEGA_E = 7.2921151467e-5
F1, F2 = 1575.42e6, 1227.60e6
A_IF = F1 * F1 / (F1 * F1 - F2 * F2)
B_IF = -F2 * F2 / (F1 * F1 - F2 * F2)
T0 = datetime(2024, 5, 8, tzinfo=timezone.utc)          # DOY 129 00:00
EPOCH_SODS = [0.0, 30.0, 60.0]
EL_CUT = 30.0

root = r"C:\GnssProcessor\data\2024-129"

# ---------- stations (name -> ecef) from the run ----------
sta_xyz = {}
with open(r"C:\GnssProcessor\runs\ext63_dual_20ep_rxcb_loose\logs\network_stations.csv") as f:
    for row in csv.DictReader(f):
        sta_xyz[row["station"]] = (float(row["ecef_x"]), float(row["ecef_y"]), float(row["ecef_z"]))
stations = sorted(sta_xyz)[::4][:16]                     # ~16 spread across the list

# ---------- SP3 positions ----------
pos = {}                                                  # sat -> {sod: xyz m}
sp3 = sorted(glob.glob(root + r"\products\*.SP3"))[0]
with open(sp3) as f:
    sod = None
    for line in f:
        if line.startswith("*"):
            p = line.split()
            day = int(p[3])
            sod = int(p[4]) * 3600 + int(p[5]) * 60 + int(float(p[6])) + (86400 if day != 8 else 0)
        elif line.startswith("PG") and sod is not None and sod <= 3600:
            pos.setdefault(line[1:4], {})[sod] = (float(line[4:18]) * 1e3,
                                                  float(line[18:32]) * 1e3, float(line[32:46]) * 1e3)

def sp3_interp(sat, t):
    """Lagrange interpolation over the 8 nearest 300-s nodes."""
    nodes = sorted(pos[sat])
    ns = sorted(nodes, key=lambda n: abs(n - t))[:8]
    ns.sort()
    out = [0.0, 0.0, 0.0]
    for i, ni in enumerate(ns):
        w = 1.0
        for j, nj in enumerate(ns):
            if i != j: w *= (t - nj) / (ni - nj)
        for k in range(3): out[k] += w * pos[sat][ni][k]
    return out

# ---------- CLK ----------
clk = {}                                                  # sat -> {sod: sec}
clkf = sorted(glob.glob(root + r"\products\*.CLK"))[0]
with open(clkf) as f:
    hdr = True
    for line in f:
        if hdr:
            if "END OF HEADER" in line: hdr = False
            continue
        if not line.startswith("AS G"): continue
        p = line.split()
        sod = int(p[5]) * 3600 + int(p[6]) * 60 + float(p[7])
        if sod <= 3600: clk.setdefault(p[1], {})[sod] = float(p[9])

# ---------- OSB ----------
osb = {}
bia = glob.glob(root + r"\products\*OSB.BIA")[0]
with open(bia) as f:
    in_sol = False
    for line in f:
        if line.startswith("+BIAS/SOLUTION"): in_sol = True; continue
        if line.startswith("-BIAS/SOLUTION"): in_sol = False; continue
        if not in_sol or not line.startswith(" OSB"): continue
        prn = line[11:14].strip(); sig = line[25:29].strip()
        val = line[70:91].strip()
        if len(prn) == 3 and prn[0] == "G" and sig and val:
            try: osb[(prn, sig)] = float(val) * C * 1e-9
            except ValueError: pass

# ---------- ATX satellite z-PCO (per PRN valid at 2024-05-08) ----------
zpco = {}                                                 # prn -> (z1, z2) m
atx = root + r"\..\static\atx\igs20.atx"
with open(atx) as f:
    prn = None; ok_time = False; freq = None; z1 = z2 = None; t_from = t_until = None
    for line in f:
        lab = line[60:].strip()
        if lab == "START OF ANTENNA":
            prn = None; freq = None; z1 = z2 = None; t_from = t_until = None
        elif lab == "TYPE / SERIAL NO":
            s = line[20:40].strip()
            prn = s if len(s) == 3 and s[0] == "G" else None
        elif lab == "VALID FROM" and prn:
            p = line.split()[:6]; t_from = datetime(int(p[0]), int(p[1]), int(p[2]), tzinfo=timezone.utc)
        elif lab == "VALID UNTIL" and prn:
            p = line.split()[:6]; t_until = datetime(int(p[0]), int(p[1]), int(p[2]), tzinfo=timezone.utc)
        elif lab == "START OF FREQUENCY" and prn:
            freq = line[3:6].strip()
        elif lab == "NORTH / EAST / UP" and prn and freq in ("G01", "G02"):
            p = line.split()
            z = float(p[2]) * 1e-3
            if freq == "G01": z1 = z
            else: z2 = z
        elif lab == "END OF ANTENNA" and prn and z1 is not None and z2 is not None:
            if (t_from is None or t_from <= T0) and (t_until is None or t_until >= T0):
                zpco[prn] = (z1, z2)

# ---------- RINEX v3 minimal reader (first N sods, GPS only) ----------
def read_rnx(path, want_sods):
    obs_types = []
    data = {}                                             # (sod, sat) -> {sig: val}
    with open(path, errors="replace") as f:
        hdr = True
        for line in f:
            if hdr:
                lab = line[60:].strip()
                if lab == "SYS / # / OBS TYPES" and (line[0] == "G" or (line[0] == " " and obs_types)):
                    if line[0] == "G": obs_types = line[7:60].split()
                    else: obs_types += line[7:60].split()
                elif lab == "END OF HEADER":
                    hdr = False
                continue
            if line.startswith(">"):
                p = line.split()
                sod = int(p[4]) * 3600 + int(p[5]) * 60 + float(p[6])
                cur = sod if sod in want_sods else None
                if sod > max(want_sods): break
                continue
            if cur is None or not line.startswith("G"): continue
            sat = line[0:3]
            rec = {}
            for i, ot in enumerate(obs_types):
                s = line[3 + 16 * i: 3 + 16 * i + 14]
                if s.strip():
                    try: rec[ot] = float(s)
                    except ValueError: pass
            data[(cur, sat)] = rec
    return data

def unit(v):
    n = math.sqrt(sum(x * x for x in v)); return [x / n for x in v]

def elevation(rx, sat):
    d = [sat[i] - rx[i] for i in range(3)]
    up = unit(rx)
    return math.degrees(math.asin(sum(a * b for a, b in zip(unit(d), up))))

def tropo(rx, el):
    h = math.sqrt(sum(x * x for x in rx)) - 6371000.0
    p_hpa = 1013.25 * (1 - 2.2557e-5 * max(h, 0.0)) ** 5.2568
    zhd = 0.0022768 * p_hpa
    m = 1.0 / math.sin(math.radians(el))
    return (zhd + 0.10) * m

resid = {}                                                # sat -> [values]
n_sta_used = 0
for sta in stations:
    d = root + r"\obs" + "\\" + sta.lower()
    files = glob.glob(d + r"\*.rnx") or glob.glob(d + r"\*.RNX")
    if not files or sta not in sta_xyz: continue
    rx = sta_xyz[sta]
    try:
        data = read_rnx(files[0], set(EPOCH_SODS))
    except Exception:
        continue
    n_sta_used += 1
    for sod in EPOCH_SODS:
        rows = {}
        for (s, sat), rec in data.items():
            if s != sod or sat not in pos or sat not in clk: continue
            p1 = rec.get("C1C"); p2 = rec.get("C2W")
            o1 = osb.get((sat, "C1C")); o2 = osb.get((sat, "C2W"))
            if None in (p1, p2, o1, o2) or sat not in zpco: continue
            # flight-time iteration with earth rotation
            tau = 0.075
            for _ in range(3):
                sp = sp3_interp(sat, sod - tau)
                ang = OMEGA_E * tau
                sr = [sp[0] * math.cos(ang) + sp[1] * math.sin(ang),
                      -sp[0] * math.sin(ang) + sp[1] * math.cos(ang), sp[2]]
                tau = math.dist(sr, rx) / C
            el = elevation(rx, sr)
            if el < EL_CUT: continue
            # APC: CoM - z_if * r_hat  (body +z toward Earth)
            z1, z2 = zpco[sat]
            z_if = A_IF * z1 + B_IF * z2
            rn = unit(sr)
            apc = [sr[i] - z_if * rn[i] for i in range(3)]
            rng = math.dist(apc, rx)
            # clock at nearest node (epochs align with product sampling)
            csod = min(clk[sat], key=lambda t: abs(t - sod))
            if abs(csod - sod) > 16.0: continue
            dts = clk[sat][csod]
            # relativity from interpolated positions +-30 s
            pa = sp3_interp(sat, sod - tau - 30); pb = sp3_interp(sat, sod - tau + 30)
            v = [(pb[i] - pa[i]) / 60.0 for i in range(3)]
            rel = -2.0 * sum(sp[i] * v[i] for i in range(3)) / C
            pred = rng - C * dts - rel + tropo(rx, el)
            if_obs = A_IF * (p1 - o1) + B_IF * (p2 - o2)
            rows[sat] = if_obs - pred
        if len(rows) >= 5:
            med = st.median(rows.values())
            for sat, r in rows.items():
                resid.setdefault(sat, []).append(r - med)

print(f"stations used: {n_sta_used}, sats: {len(resid)}")
e_if = {s: st.median(v) for s, v in resid.items() if len(v) >= 6}

# loose-arm CLK medians
clk_res = {}
with open(r"C:\GnssProcessor\runs\ext63_dual_20ep_rxcb_loose\logs\network_solution.csv") as f:
    for row in csv.DictReader(f):
        if row["kind"] == "CLK":
            clk_res.setdefault(row["sat"], []).append(float(row["value_m"]))
clk_med = {s: st.median(v) for s, v in clk_res.items()}

sats = sorted(set(e_if) & set(clk_med))
xs = [e_if[s] for s in sats]; ys = [clk_med[s] for s in sats]
mx, my = st.mean(xs), st.mean(ys)
sxx = sum((x - mx) ** 2 for x in xs); syy = sum((y - my) ** 2 for y in ys)
sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
print(f"e_IF: mean {mx:+.3f} std {st.pstdev(xs):.3f} m   "
      f"corr(e_IF, looseCLK) = {sxy / math.sqrt(sxx * syy):+.4f}   slope = {sxy / sxx:+.4f}")
print("\nsat   e_IF_m   n    looseCLK_m")
for s in sorted(sats, key=lambda s: -abs(clk_med[s])):
    print(f"{s}  {e_if[s]:+8.3f}  {len(resid[s]):3d}  {clk_med[s]:+8.3f}")

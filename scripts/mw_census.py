"""Estimator-free WL integer census: MW combination from raw RINEX, first N epochs.
Median MW per (station,sat) arc -> star-basis DD within same band-2 channel group -> fractional stats."""
import glob, math, statistics as st
from collections import defaultdict

C = 299792458.0
F1, F2 = 1575.42e6, 1227.60e6
LAM_WL = C / (F1 - F2)          # 0.8619 m
N_EP = 30

def parse_station(path):
    obs_idx = None; ntypes = 0
    series = defaultdict(list)  # (sat, ch2) -> [mw_cyc]
    ep_count = 0
    with open(path, 'r', errors='replace') as f:
        # header
        types = []
        for line in f:
            if 'SYS / # / OBS TYPES' in line and (line[0] == 'G' or (not line[0].strip() and types)):
                if line[0] == 'G':
                    ntypes = int(line[3:6])
                    types = line[7:60].split()
                else:
                    types += line[7:60].split()
                if len(types) >= ntypes and ntypes > 0:
                    pass
            if 'END OF HEADER' in line:
                break
        if not types: return {}
        def idx(t):
            return types.index(t) if t in types else -1
        i_c1, i_l1 = idx('C1C'), idx('L1C')
        ch2 = None
        for c in ('W', 'L', 'S', 'X'):
            if idx('C2'+c) >= 0 and idx('L2'+c) >= 0:
                ch2 = c; break
        if min(i_c1, i_l1) < 0 or ch2 is None: return {}
        i_c2, i_l2 = idx('C2'+ch2), idx('L2'+ch2)
        # epochs
        for line in f:
            if line.startswith('>'):
                ep_count += 1
                if ep_count > N_EP: break
                continue
            if ep_count == 0 or not line.startswith('G'): continue
            sat = line[0:3]
            def field(i):
                s = line[3+16*i : 3+16*i+14].strip()
                try: return float(s)
                except: return None
            p1, l1, p2, l2 = field(i_c1), field(i_l1), field(i_c2), field(i_l2)
            if None in (p1, l1, p2, l2) or 0.0 in (p1, l1, p2, l2): continue
            # MW in WL cycles: RINEX L is in cycles, so WL phase = l1 - l2;
            # narrow-lane code average converted to WL cycles via lambda_WL.
            mw = (l1 - l2) - (F1*p1 + F2*p2)/((F1 + F2)*LAM_WL)
            series[(sat, ch2)].append(mw)
    out = {}
    for (sat, c2), vals in series.items():
        if len(vals) >= 15:
            out[(sat, c2)] = (st.median(vals), len(vals), st.pstdev(vals))
    return out

stations = {}
for p in sorted(glob.glob('C:/GnssProcessor/data/2024-129/obs/*/*_30S_MO.rnx')):
    import os
    name = os.path.basename(os.path.dirname(p)).upper()
    s = parse_station(p)
    if s: stations[name] = s

# group stations by band-2 channel actually used
groups = defaultdict(dict)   # ch2 -> station -> {sat: median}
for sta, sats in stations.items():
    by_ch = defaultdict(dict)
    for (sat, c2), (med, n, sd) in sats.items():
        by_ch[c2][sat] = med
    ch = max(by_ch, key=lambda c: len(by_ch[c]))
    groups[ch][sta] = by_ch[ch]

print(f"stations parsed: {len(stations)}; groups: {{{', '.join(f'{c}: {len(g)}' for c, g in sorted(groups.items()))}}}")
for ch, g in sorted(groups.items(), key=lambda kv: -len(kv[1])):
    if len(g) < 10: continue
    sat_count = defaultdict(int)
    for s in g.values():
        for sat in s: sat_count[sat] += 1
    ref_sat = max(sat_count, key=sat_count.get)
    # ref station must lie inside ref_sat's visibility cone
    cands = [s for s in g if ref_sat in g[s]]
    ref_sta = max(cands, key=lambda s: len(g[s]))
    fracs = []
    for sta, sats in g.items():
        if sta == ref_sta or ref_sat not in sats or ref_sat not in g[ref_sta]: continue
        for sat, v in sats.items():
            if sat == ref_sat or sat not in g[ref_sta]: continue
            dd = v - sats[ref_sat] - g[ref_sta][sat] + g[ref_sta][ref_sat]
            f = dd - round(dd)
            fracs.append(f)
    if not fracs: continue
    n = len(fracs)
    af = sorted(abs(x) for x in fracs)
    R = math.hypot(sum(math.cos(2*math.pi*x) for x in fracs)/n, sum(math.sin(2*math.pi*x) for x in fracs)/n)
    print(f"group C2{ch}/L2{ch}: {len(g)} sta, ref {ref_sta}/{ref_sat}, {n} DDs | mean|f|={sum(af)/n:.3f} med|f|={af[n//2]:.3f} P(<0.10)={sum(1 for x in af if x<0.10)/n:.3f} P(<0.15)={sum(1 for x in af if x<0.15)/n:.3f} circR={R:.3f} [uniform: 0.25/0.25/0.20/0.30/0.00]")

"""Estimator-free EWL/WL integer census (WL-2 spec eyeball).

Extends scripts/mw_census.py to the tri-frequency lanes: for each pair the MW
combination is formed per epoch from raw RINEX, per-(station,sat) arc medians over
the first n epochs are star-basis double-differenced within a uniform channel
group, and the fractional-cycle stats are reported for n = 1, 2, 5, 30.

Lanes:
  GPS  WL  (L1,L2)  lam 0.862 m   (reference — the known circR~0.62 @ n=30)
  GPS  EWL (L2,L5)  lam 5.861 m   (the WL-2 front rung)
  GAL  WL  (E1,E5a) lam 0.751 m   (reference)
  GAL  EWL (E5b,E5a) lam 9.768 m
  GAL  SWL (E5b,E5)  lam 19.54 m  (AltBOC luxury rung)
  GAL  SWL (E5,E5a)  lam 19.54 m
"""
import glob, math, os, statistics as st
from collections import defaultdict

C = 299792458.0
N_EP = 30

FREQ = {  # (sys, band) -> Hz
    ('G', '1'): 1575.42e6, ('G', '2'): 1227.60e6, ('G', '5'): 1176.45e6,
    ('E', '1'): 1575.42e6, ('E', '5'): 1176.45e6, ('E', '7'): 1207.14e6,
    ('E', '8'): 1191.795e6,
}
# channel preference per (sys, band) — same-channel code/phase required
CHAN = {
    ('G', '1'): 'CWX', ('G', '2'): 'WLSX', ('G', '5'): 'QXI',
    ('E', '1'): 'CX',  ('E', '5'): 'QX',   ('E', '7'): 'QX', ('E', '8'): 'QX',
}
PAIRS = [  # (label, sys, band_hi_freq, band_lo_freq)
    ('GPS WL  (1,2) 0.86m',  'G', '1', '2'),
    ('GPS EWL (2,5) 5.86m',  'G', '2', '5'),
    ('GAL WL  (1,5) 0.75m',  'E', '1', '5'),
    ('GAL EWL (7,5) 9.77m',  'E', '7', '5'),
    ('GAL SWL (7,8) 19.5m',  'E', '7', '8'),
    ('GAL SWL (8,5) 19.5m',  'E', '8', '5'),
]

def parse_header(f):
    """-> {sys: [obs types]}, file positioned after END OF HEADER."""
    types = {}
    cur = None
    for line in f:
        if 'END OF HEADER' in line:
            return types
        if 'SYS / # / OBS TYPES' in line:
            if line[0].strip():
                cur = line[0]
                types[cur] = line[7:60].split()
            elif cur:
                types[cur] += line[7:60].split()
    return types

def pick(types, sys, band):
    """-> (code_idx, phase_idx, chan) for the preferred same-channel pair."""
    tl = types.get(sys)
    if not tl:
        return None
    for ch in CHAN[(sys, band)]:
        c, l = 'C' + band + ch, 'L' + band + ch
        if c in tl and l in tl:
            return (tl.index(c), tl.index(l), ch)
    return None

def parse_station(path):
    """-> {pair_label: {'chan': (ch_hi, ch_lo), 'series': {sat: [mw_cyc,...]}}}"""
    out = {}
    with open(path, 'r', errors='replace') as f:
        types = parse_header(f)
        want = {}   # sys -> set of field indices we need, per pair spec
        for label, sys, b1, b2 in PAIRS:
            p1, p2 = pick(types, sys, b1), pick(types, sys, b2)
            if p1 and p2:
                f1, f2 = FREQ[(sys, b1)], FREQ[(sys, b2)]
                out[label] = {'chan': (p1[2], p2[2]), 'series': defaultdict(list),
                              'idx': (p1[0], p1[1], p2[0], p2[1]), 'f': (f1, f2),
                              'lam': C / (f1 - f2), 'sys': sys}
        if not out:
            return out
        ep = 0
        for line in f:
            if line.startswith('>'):
                ep += 1
                if ep > N_EP:
                    break
                continue
            if ep == 0:
                continue
            sysc = line[0]
            if sysc not in ('G', 'E'):
                continue
            sat = line[0:3]
            for label, d in out.items():
                if d['sys'] != sysc:
                    continue
                ic1, il1, ic2, il2 = d['idx']
                def field(i):
                    s = line[3 + 16 * i: 3 + 16 * i + 14].strip()
                    try:
                        v = float(s)
                        return v if v != 0.0 else None
                    except ValueError:
                        return None
                p1, l1, p2, l2 = field(ic1), field(il1), field(ic2), field(il2)
                if None in (p1, l1, p2, l2):
                    continue
                f1, f2 = d['f']
                mw = (l1 - l2) - (f1 * p1 + f2 * p2) / ((f1 + f2) * d['lam'])
                d['series'][sat].append(mw)
    return out

stations = {}
for p in sorted(glob.glob('C:/GnssProcessor/data/2024-129/obs/*/*_30S_MO.rnx')):
    name = os.path.basename(os.path.dirname(p)).upper()
    s = parse_station(p)
    if s:
        stations[name] = s
print(f"stations parsed: {len(stations)}")

for label, sys, b1, b2 in PAIRS:
    # group stations by exact channel tuple (channel-mixing discipline)
    groups = defaultdict(dict)   # chan tuple -> station -> {sat: [vals]}
    noise = []
    for sta, pairs in stations.items():
        d = pairs.get(label)
        if not d:
            continue
        groups[d['chan']][sta] = dict(d['series'])
        for vals in d['series'].values():
            if len(vals) >= 20:
                noise.append(st.pstdev(vals))
    if not groups:
        print(f"\n{label}: no stations")
        continue
    chan, g = max(groups.items(), key=lambda kv: len(kv[1]))
    med_noise = st.median(noise) if noise else float('nan')
    print(f"\n{label}: {sum(len(v) for v in groups.values())} sta total; largest "
          f"channel group {chan}: {len(g)} sta | per-epoch arc noise med sigma = {med_noise:.3f} cyc")
    for n in (1, 2, 5, 30):
        # per-station per-sat medians over first n epochs
        med = {sta: {sat: st.median(vals[:n]) for sat, vals in sats.items() if len(vals) >= min(n, 20)}
               for sta, sats in g.items()}
        med = {sta: sats for sta, sats in med.items() if sats}
        if len(med) < 10:
            print(f"  n={n:2d}: <10 stations with data")
            continue
        sat_count = defaultdict(int)
        for s in med.values():
            for sat in s:
                sat_count[sat] += 1
        ref_sat = max(sat_count, key=sat_count.get)
        cands = [s for s in med if ref_sat in med[s]]
        ref_sta = max(cands, key=lambda s: len(med[s]))
        fracs = []
        for sta, sats in med.items():
            if sta == ref_sta or ref_sat not in sats:
                continue
            for sat, v in sats.items():
                if sat == ref_sat or sat not in med[ref_sta]:
                    continue
                dd = v - sats[ref_sat] - med[ref_sta][sat] + med[ref_sta][ref_sat]
                fracs.append(dd - round(dd))
        if not fracs:
            continue
        m = len(fracs)
        af = sorted(abs(x) for x in fracs)
        R = math.hypot(sum(math.cos(2 * math.pi * x) for x in fracs) / m,
                       sum(math.sin(2 * math.pi * x) for x in fracs) / m)
        print(f"  n={n:2d}: {m:5d} DDs | mean|f|={sum(af)/m:.3f} med|f|={af[m//2]:.3f} "
              f"P(<0.15)={sum(1 for x in af if x < 0.15)/m:.3f} "
              f"P(<0.25)={sum(1 for x in af if x < 0.25)/m:.3f} circR={R:.3f}")
print("\nuniform baseline: mean|f|=0.250 med|f|=0.250 P(<0.15)=0.300 P(<0.25)=0.500 circR=0.000")

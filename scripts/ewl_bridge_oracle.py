"""Estimator-free oracle of the WL-2 bridge arc (SPEC_ewl_wl_cascade_card §3.4).

Chain, per station-pair/sat-pair DD, straight from raw RINEX (GPS, W/Q channels):
  1. EWL integer from MW(2,5) at n=2 epochs, FFR-gated (|frac|<=0.20)   [spec §3.2]
  2. bridged WL:  z = (DD[gfb] + lam25*N_EWL) / lam12
        gfb = lam12*(l1-l2) - lam25*(l2-l5)  [m]  =>  z = N12 - 0.5064*I1_dd[m]
  3. compare the bridged-WL frac maturity curve vs plain code MW(1,2) on the same
     DDs, the card's headline claim (iono-limited vs code-limited)
  4. integrity: |round(z@n5) - DD[mw12]@n30| > 1.5 cyc flags a wrong EWL integer
     (one EWL error displaces z by lam25/lam12 = 6.80 cyc, unmissable vs MW12)

DDs are built between geographically near station pairs (nearest neighbor), bucketed
by baseline length, plus a far-pair control bucket. The locality of the iono term
(0.5 cyc per meter of DD iono) is the open spec question §11 Q2.
"""
import glob, math, os, random, sys, statistics as st
from collections import defaultdict
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rinex_ids import norm_sat  # noqa: E402

C = 299792458.0
F1, F2, F5 = 1575.42e6, 1227.60e6, 1176.45e6
LAM12 = C / (F1 - F2)          # 0.8619 m
LAM25 = C / (F2 - F5)          # 5.861 m
LAM15 = C / (F1 - F5)          # 0.7514 m
IONO_CYC_PER_M = 0.5064        # bridge iono sensitivity, WL cycles per m of DD I1
N_EP = 30
EWL_GATE = 0.20

def parse_station(path):
    pos = None
    types = []
    cur = None
    series = defaultdict(lambda: defaultdict(list))   # stat -> sat -> [val]
    ch5 = None
    with open(path, 'r', errors='replace') as f:
        for line in f:
            if 'APPROX POSITION XYZ' in line:
                try:
                    pos = tuple(float(line[i:i+14]) for i in (0, 14, 28))
                except ValueError:
                    pos = None
            if 'SYS / # / OBS TYPES' in line:
                if line[0].strip():
                    cur = line[0]
                    if cur == 'G':
                        types = line[7:60].split()
                elif cur == 'G':
                    types += line[7:60].split()
            if 'END OF HEADER' in line:
                break
        def idx(t):
            return types.index(t) if t in types else -1
        i_c1, i_l1 = idx('C1C'), idx('L1C')
        i_c2, i_l2 = idx('C2W'), idx('L2W')
        for c in 'QX':
            if idx('C5'+c) >= 0 and idx('L5'+c) >= 0:
                ch5 = c
                break
        if min(i_c1, i_l1, i_c2, i_l2) < 0 or ch5 is None or pos is None:
            return None
        i_c5, i_l5 = idx('C5'+ch5), idx('L5'+ch5)
        ep = 0
        for line in f:
            if line.startswith('>'):
                ep += 1
                if ep > N_EP:
                    break
                continue
            if ep == 0 or not line.startswith('G'):
                continue
            sat = norm_sat(line[0:3])
            def field(i):
                s = line[3 + 16*i: 3 + 16*i + 14].strip()
                try:
                    v = float(s)
                    return v if v != 0.0 else None
                except ValueError:
                    return None
            p1, l1 = field(i_c1), field(i_l1)
            p2, l2 = field(i_c2), field(i_l2)
            p5, l5 = field(i_c5), field(i_l5)
            if None in (p1, l1, p2, l2, p5, l5):
                continue
            series['mw12'][sat].append((l1 - l2) - (F1*p1 + F2*p2) / ((F1 + F2) * LAM12))
            series['mw25'][sat].append((l2 - l5) - (F2*p2 + F5*p5) / ((F2 + F5) * LAM25))
            series['mw15'][sat].append((l1 - l5) - (F1*p1 + F5*p5) / ((F1 + F5) * LAM15))
            series['gfb'][sat].append(LAM12 * (l1 - l2) - LAM25 * (l2 - l5))
    return {'pos': pos, 'ch5': ch5, 's': series}

stations = {}
for p in sorted(glob.glob('C:/GnssProcessor/data/2024-129/obs/*/*_30S_MO.rnx')):
    name = os.path.basename(os.path.dirname(p)).upper()
    d = parse_station(p)
    if d:
        stations[name] = d
names = sorted(stations)
print(f"tri-freq W/Q|X stations: {len(names)}")

def dist_km(a, b):
    pa, pb = stations[a]['pos'], stations[b]['pos']
    return math.dist(pa, pb) / 1e3

# nearest-neighbor pairs (same ch5), bucketed; + far control pairs
pairs = set()
for a in names:
    best, bd = None, 1e12
    for b in names:
        if b == a or stations[b]['ch5'] != stations[a]['ch5']:
            continue
        d = dist_km(a, b)
        if d < bd:
            best, bd = b, d
    if best and bd < 1000.0:
        pairs.add((min(a, best), max(a, best), bd))
rng = random.Random(129)
far = []
for a in rng.sample(names, min(80, len(names))):
    cands = [b for b in names if b != a and stations[b]['ch5'] == stations[a]['ch5']
             and 2000.0 < dist_km(a, b) < 6000.0]
    if cands:
        b = rng.choice(cands)
        far.append((min(a, b), max(a, b), dist_km(a, b)))

BUCKETS = [('<50km', 0, 50), ('50-150km', 50, 150), ('150-400km', 150, 400),
           ('400-1000km', 400, 1000), ('far 2-6Mm', 2000, 6000)]
by_bucket = defaultdict(list)
for a, b, d in pairs:
    for label, lo, hi in BUCKETS[:4]:
        if lo <= d < hi:
            by_bucket[label].append((a, b, d))
by_bucket['far 2-6Mm'] = far[:60]

def med_n(vals, n):
    return st.median(vals[:n]) if len(vals) >= n else None

for label, _, _ in BUCKETS:
    plist = by_bucket.get(label, [])
    if not plist:
        continue
    rows = []          # (bridge_frac_by_n, mw12_frac_by_n, iono_m, closure_bad)
    gate_skip = 0
    for a, b, d in plist:
        sa, sb = stations[a]['s'], stations[b]['s']
        common = [s for s in sa['mw25'] if s in sb['mw25']
                  and all(len(x[s]) >= 5 for x in (sa['mw25'], sb['mw25'], sa['gfb'],
                                                   sb['gfb'], sa['mw12'], sb['mw12']))]
        if len(common) < 2:
            continue
        ref = max(common, key=lambda s: min(len(sa['mw25'][s]), len(sb['mw25'][s])))
        for s in common:
            if s == ref:
                continue
            def dd(stat, n):
                vs = [med_n(x[stat][t], n) for x in (sa, sb) for t in (s, ref)]
                if None in vs:
                    return None
                return (vs[0] - vs[1]) - (vs[2] - vs[3])
            e2 = dd('mw25', 2)
            if e2 is None:
                continue
            if abs(e2 - round(e2)) > EWL_GATE:
                gate_skip += 1
                continue
            n_ewl = round(e2)
            bf, mf = {}, {}
            for n in (1, 2, 5, 30):
                g = dd('gfb', n)
                m12 = dd('mw12', n)
                bf[n] = None if g is None else (g + LAM25 * n_ewl) / LAM12
                mf[n] = m12
            if bf[5] is None or mf[30] is None:
                continue
            iono_m = -(bf[5] - round(bf[5])) / IONO_CYC_PER_M
            closure_bad = abs(round(bf[5]) - mf[30]) > 1.5
            rows.append((bf, mf, iono_m, closure_bad))
    if not rows:
        print(f"\n[{label}] {len(plist)} pairs: no usable DDs")
        continue
    m = len(rows)
    print(f"\n[{label}] {len(plist)} pairs, {m} DDs (EWL gate skipped {gate_skip}) | "
          f"med implied |DD iono| = {st.median(sorted(abs(r[2]) for r in rows)):.2f} m | "
          f"closure flags: {sum(1 for r in rows if r[3])}/{m}")
    for n in (1, 2, 5, 30):
        for tag, k in (('bridge', 0), ('codeMW', 1)):
            fr = [r[k][n] for r in rows if r[k][n] is not None]
            if not fr:
                continue
            fx = [v - round(v) for v in fr]
            q = len(fx)
            af = sorted(abs(x) for x in fx)
            R = math.hypot(sum(math.cos(2*math.pi*x) for x in fx)/q,
                           sum(math.sin(2*math.pi*x) for x in fx)/q)
            print(f"  n={n:2d} {tag}: {q:4d} DDs med|f|={af[q//2]:.3f} "
                  f"P(<0.15)={sum(1 for x in af if x < 0.15)/q:.3f} circR={R:.3f}")
print("\nuniform baseline: med|f|=0.250 P(<0.15)=0.300 circR=0.000")

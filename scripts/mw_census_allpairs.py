"""Estimator-free ALL-PAIRS MW census (UWL-1 ladder survey, Jul-16 2026).

Every channel combination across DISTINCT frequency bands, per constellation
(G/E/C; R excluded FDMA), from raw RINEX — first N epochs.  Per pair:
  lambda_WL, coverage (stations/sats/arcs), per-SAMPLE sigma (cyc — the n=1
  decidability number), and star-basis DD fractional stats (med|f|, P<0.10,
  P<0.15, circR — the network closed-cycle constraint check; receiver and
  satellite MW biases cancel in the DD, so clustering on integers is the
  direct measure of the pair's usable integer content).
Ordered by lambda descending: the integer-accessibility ladder, measured.

Usage: python mw_census_allpairs.py [n_epochs] [obs_glob]
"""
import glob, math, os, sys, statistics as st
from collections import defaultdict

C = 299792458.0
N_EP = int(sys.argv[1]) if len(sys.argv) > 1 else 10
OBS_GLOB = sys.argv[2] if len(sys.argv) > 2 else 'C:/GnssProcessor/data/2024-129/obs/*/*_30S_MO.rnx'
MIN_ARC = max(4, int(0.6 * N_EP))   # samples per (sta,sat,pair) arc for a usable median

FREQ = {
    'G': {'1': 1575.42e6, '2': 1227.60e6, '5': 1176.45e6},
    'E': {'1': 1575.42e6, '5': 1176.45e6, '6': 1278.75e6, '7': 1207.14e6, '8': 1191.795e6},
    'C': {'1': 1575.42e6, '2': 1561.098e6, '5': 1176.45e6, '6': 1268.52e6,
          '7': 1207.14e6, '8': 1191.795e6},
}

def parse_station(path):
    """-> {(sat, pairkey): [mw_cyc,...]} over the first N_EP epochs.
    pairkey = (sys, 'L1C/L5Q') with the HIGHER-frequency side first."""
    types = {}          # sys char -> [obs types]
    cur_sys, remaining = None, 0
    series = defaultdict(list)
    with open(path, 'r', errors='replace') as f:
        for line in f:
            if 'SYS / # / OBS TYPES' in line:
                if line[0].strip():
                    cur_sys = line[0]
                    remaining = int(line[3:6])
                    types[cur_sys] = []
                got = line[7:60].split()
                types.setdefault(cur_sys, []).extend(got)
            if 'END OF HEADER' in line:
                break
        # Per system: usable (band,channel) sides = phase AND code present.
        sides = {}   # sys -> [(band, ch, i_code, i_phase, freq)]
        for s, tl in types.items():
            if s not in FREQ:
                continue
            out = []
            for t in tl:
                if len(t) != 3 or t[0] != 'L':
                    continue
                band, ch = t[1], t[2]
                if band not in FREQ[s]:
                    continue
                fq = FREQ[s][band]
                # RINEX 3.02 legacy: BDS-2 B1I is written as band '1' with I/Q
                # attributes but sits at 1561.098 MHz (B1C is 1575.42).
                if s == 'C' and band == '1' and ch in 'IQ':
                    fq = 1561.098e6
                code = 'C' + band + ch
                if code in tl:
                    out.append((band, ch, tl.index(code), tl.index(t), fq))
            sides[s] = out
        if not sides:
            return series
        ep = 0
        for line in f:
            if line.startswith('>'):
                ep += 1
                if ep > N_EP:
                    break
                continue
            if ep == 0:
                continue
            s = line[0]
            if s not in sides or not sides[s]:
                continue
            sat = line[0:3]
            vals = {}
            for band, ch, ic, il, fq in sides[s]:
                def field(i):
                    seg = line[3 + 16 * i: 3 + 16 * i + 14].strip()
                    try:
                        v = float(seg)
                        return v if v != 0.0 else None
                    except ValueError:
                        return None
                p, l = field(ic), field(il)
                if p is not None and l is not None:
                    vals[(band, ch)] = (p, l, fq)
            keys = sorted(vals)
            for i in range(len(keys)):
                for j in range(len(keys)):
                    if i == j:
                        continue
                    (b1, c1), (b2, c2) = keys[i], keys[j]
                    if b1 == b2:
                        continue
                    p1, l1, f1 = vals[(b1, c1)]
                    p2, l2, f2 = vals[(b2, c2)]
                    if f1 <= f2:      # higher-frequency side first, each unordered pair once
                        continue
                    lam_wl = C / (f1 - f2)
                    mw = (l1 - l2) - (f1 * p1 + f2 * p2) / ((f1 + f2) * lam_wl)
                    series[(sat, (s, f'L{b1}{c1}/L{b2}{c2}'))].append(mw)
    return series

stations = {}
for p in sorted(glob.glob(OBS_GLOB)):
    name = os.path.basename(os.path.dirname(p)).upper()
    s = parse_station(p)
    if s:
        stations[name] = s
print(f"stations parsed: {len(stations)}; epochs: {N_EP}")

# (sys,pair) -> station -> {sat: (median, per-sample sigma)}
groups = defaultdict(lambda: defaultdict(dict))
for sta, ser in stations.items():
    for (sat, pk), vals in ser.items():
        if len(vals) >= MIN_ARC:
            groups[pk][sta][sat] = (st.median(vals),
                                    st.pstdev(vals) if len(vals) > 1 else float('nan'))

rows = []
def freq_of(sysc, band, ch):
    if sysc == 'C' and band == '1' and ch in 'IQ':
        return 1561.098e6   # RINEX 3.02 legacy B1I (see header parse)
    return FREQ[sysc][band]

for (sysc, pair), g in groups.items():
    b1, c1, b2, c2 = pair[1], pair[2], pair[5], pair[6]
    lam = C / (freq_of(sysc, b1, c1) - freq_of(sysc, b2, c2))
    # star-basis DD within the exact channel pair
    sat_count = defaultdict(int)
    for s in g.values():
        for sat in s:
            sat_count[sat] += 1
    if not sat_count:
        continue
    ref_sat = max(sat_count, key=lambda k: (sat_count[k], k))
    cands = [s for s in g if ref_sat in g[s]]
    if not cands:
        continue
    ref_sta = max(cands, key=lambda s: (len(g[s]), s))
    fracs = []
    for sta, sats in g.items():
        if sta == ref_sta or ref_sat not in sats:
            continue
        for sat, (v, _) in sats.items():
            if sat == ref_sat or sat not in g[ref_sta]:
                continue
            dd = v - sats[ref_sat][0] - g[ref_sta][sat][0] + g[ref_sta][ref_sat][0]
            fracs.append(dd - round(dd))
    sig = [sg for s in g.values() for (_, sg) in s.values() if not math.isnan(sg)]
    med_sig = st.median(sig) if sig else float('nan')
    n_arcs = sum(len(s) for s in g.values())
    if fracs:
        n = len(fracs)
        af = sorted(abs(x) for x in fracs)
        R = math.hypot(sum(math.cos(2 * math.pi * x) for x in fracs) / n,
                       sum(math.sin(2 * math.pi * x) for x in fracs) / n)
        stats = (n, af[n // 2], sum(1 for x in af if x < 0.10) / n,
                 sum(1 for x in af if x < 0.15) / n, R)
    else:
        stats = (0, float('nan'), float('nan'), float('nan'), float('nan'))
    rows.append((sysc, lam, pair, len(g), len(sat_count), n_arcs, med_sig, stats))

print(f"{'sys':>3} {'lam_m':>8} {'pair':>12} {'sta':>4} {'sat':>4} {'arcs':>5} "
      f"{'sig1(cyc)':>9} | {'nDD':>5} {'med|f|':>6} {'P<.10':>5} {'P<.15':>5} {'circR':>6}")
for sysc in 'GEC':
    for (s, lam, pair, nsta, nsat, narc, msig, (n, mf, p10, p15, R)) in \
            sorted(rows, key=lambda r: -r[1]):
        if s != sysc:
            continue
        print(f"{s:>3} {lam:8.3f} {pair:>12} {nsta:4d} {nsat:4d} {narc:5d} "
              f"{msig:9.4f} | {n:5d} {mf:6.3f} {p10:5.2f} {p15:5.2f} {R:6.3f}")
print("[uniform baseline: med|f|=0.250 P<.10=0.20 P<.15=0.30 circR=0.00]")

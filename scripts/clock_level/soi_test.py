"""Card SOI: the second-order ionospheric term against the field stations' leak (tests T1 and T2).

    python scripts/clock_level/soi_test.py <run> [<run> ...]

Per station, satellite and epoch, the ionosphere-free phase second-order term
    p = q / (2 f1 f2 (f1 + f2)),   q = 7527 c STEC B.k
with k the propagation direction (satellite to receiver), STEC from logs/network_ionosphere.csv
(`iono_total_m`, L1 metres), the line of sight from its azimuth and elevation at the station's ECEF
(logs/network_stations.csv), a pierce point on a 450 km shell, and B from the centred dipole whose
constants are parsed from src/core/atmosphere/MagneticField.cpp (card SOI section 1).

T1  the ionosphere-free post-fit phase residual 2.5457 v(L1) - 1.5457 v(L2W) (logs/network_pf_rows.csv,
    v = y - A x) over station-satellite arcs of at least 40 epochs (split at gaps above 90 s and at
    ambiguity births), against p in the estimator's absorbed form (double-centred per epoch over the
    epoch's lines of sight, then de-meaned per arc); pooled regression of the 20-epoch block means
    with arc-clustered errors; the placebo (p of another satellite of the same station over the same
    epochs); the epoch-level regression; the rank correlation across arcs of the residual's
    persistence (rms of its block means) with p's size.
T2  per satellite's L2W block clone (logs/network_pb_hold_clone.csv, rows of age at least 30 minutes)
    and lag k of 5, 20, 60 and 120 epochs: the change of the walk share Phi_D against the change of
    p (station-centred per epoch) averaged over the lines of sight present at both ends, both
    de-commoned across satellites per epoch; slope, r and r^2.
"""
import collections, csv, importlib.util, math, os, re, statistics as st, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
_spec = importlib.util.spec_from_file_location('rev_wire', os.path.join(HERE, 'rev_wire.py'))
rw = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rw)

C = 299792458.0
F1, F2 = 1575.42e6, 1227.60e6
A1, A2 = F1 * F1 / (F1 * F1 - F2 * F2), -F2 * F2 / (F1 * F1 - F2 * F2)
TECU_L1_M = 40.3e16 / (F1 * F1)
KQ = 7527.0 * C * 1.0e16                     # per TECU
KP = 1.0 / (2.0 * F1 * F2 * (F1 + F2))
SHELL_M = 450.0e3
LAGS = (5, 20, 60, 120)


def dipole():
    src = open(os.path.join(ROOT, 'src', 'core', 'atmosphere', 'MagneticField.cpp')).read()
    g = lambda n: float(re.search(r'constexpr\s+double\s+' + n + r'\s*=\s*([-0-9.eE+]+)\s*;', src).group(1))
    lat, lon = math.radians(g('kPoleLatDeg')), math.radians(g('kPoleLonDeg'))
    m = (math.cos(lat) * math.cos(lon), math.cos(lat) * math.sin(lon), math.sin(lat))
    return m, g('kB0Tesla'), g('kRefEarthRadiusM')


MHAT, B0, RREF = dipole()


def bfield(pos):
    r = math.sqrt(sum(x * x for x in pos))
    rh = [x / r for x in pos]
    d = sum(a * b for a, b in zip(MHAT, rh))
    s = B0 * (RREF / r) ** 3
    return [s * (MHAT[i] - 3.0 * d * rh[i]) for i in range(3)]


def geodetic(x, y, z):
    a, f = 6378137.0, 1.0 / 298.257223563
    e2 = f * (2 - f)
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1 - e2))
    for _ in range(6):
        n = a / math.sqrt(1 - e2 * math.sin(lat) ** 2)
        h = p / math.cos(lat) - n
        lat = math.atan2(z, p * (1 - e2 * n / (n + h)))
    return lat, lon


def stations(d):
    out = {}
    for r in csv.DictReader(open(d + '/logs/network_stations.csv')):
        pos = (float(r['ecef_x']), float(r['ecef_y']), float(r['ecef_z']))
        lat, lon = geodetic(*pos)
        out[r['station']] = (pos, lat, lon)
    return out


def term(sta, az_deg, el_deg, stec):
    """p (m) and B.k (T) for one line of sight."""
    pos, lat, lon = sta
    az, el = math.radians(az_deg), math.radians(el_deg)
    e, n, u = math.cos(el) * math.sin(az), math.cos(el) * math.cos(az), math.sin(el)
    sl, cl, so, co = math.sin(lat), math.cos(lat), math.sin(lon), math.cos(lon)
    ux = -so * e - sl * co * n + cl * co * u
    uy = co * e - sl * so * n + cl * so * u
    uz = cl * n + sl * u
    r2 = sum(x * x for x in pos)
    rs = math.sqrt(r2) + SHELL_M
    ru = pos[0] * ux + pos[1] * uy + pos[2] * uz
    s = -ru + math.sqrt(ru * ru - r2 + rs * rs)
    ipp = (pos[0] + s * ux, pos[1] + s * uy, pos[2] + s * uz)
    b = bfield(ipp)
    bk = -(b[0] * ux + b[1] * uy + b[2] * uz)       # k = -u, satellite to receiver
    return KQ * stec * bk * KP, bk


def load(d):
    sta = stations(d)
    P = {}          # (tow, sta, sat) -> (p, bk, stec, el, az)
    for r in csv.DictReader(open(d + '/logs/network_ionosphere.csv')):
        s = sta.get(r['station'])
        if s is None:
            continue
        stec = float(r['iono_total_m']) / TECU_L1_M
        p, bk = term(s, float(r['azimuth_deg']), float(r['elevation_deg']), stec)
        P[(round(float(r['tow']), 1), r['station'], r['sat'])] = (p, bk, stec, float(r['elevation_deg']),
                                                                 float(r['azimuth_deg']))
    v1, v2 = {}, {}
    with open(d + '/logs/network_pf_rows.csv') as f:
        for r in csv.DictReader(ln for ln in f if not ln.startswith('#')):
            if r['kind'] != 'phase':
                continue
            k = (round(float(r['tow']), 1), r['station'], r['sat'])
            if r['signal'] in ('L1C', 'L1W'):
                v1[k] = float(r['resid_m'])
            elif r['signal'] == 'L2W':
                v2[k] = float(r['resid_m'])
    R = {k: A1 * v1[k] + A2 * v2[k] for k in v1 if k in v2}
    births = collections.defaultdict(set)
    with open(d + '/logs/network_amb_state.csv') as f:
        for r in csv.DictReader(ln for ln in f if not ln.startswith('#')):
            if r['event'] == 'birth':
                births[(r['station'], r['sat'])].add(round(float(r['tow']), 1))
    return sta, P, R, births


def center(vals, iters=3):
    """Double-centre {(sta, sat): x} over one epoch: station and satellite means removed."""
    x = dict(vals)
    for _ in range(iters):
        for idx in (0, 1):
            g = collections.defaultdict(list)
            for k, v in x.items():
                g[k[idx]].append(v)
            m = {a: sum(b) / len(b) for a, b in g.items()}
            x = {k: v - m[k[idx]] for k, v in x.items()}
    return x


def stcenter(vals):
    g = collections.defaultdict(list)
    for k, v in vals.items():
        g[k[0]].append(v)
    m = {a: sum(b) / len(b) for a, b in g.items()}
    return {k: v - m[k[0]] for k, v in vals.items()}


def regress(pairs_by_cluster):
    """Pooled slope of y on x (no intercept; inputs centred), clustered SE, r, n."""
    sxx = sxy = syy = 0.0
    n = 0
    for pts in pairs_by_cluster.values():
        for x, y in pts:
            sxx += x * x
            sxy += x * y
            syy += y * y
            n += 1
    if sxx <= 0 or syy <= 0:
        return float('nan'), float('nan'), float('nan'), n
    b = sxy / sxx
    meat = 0.0
    for pts in pairs_by_cluster.values():
        g = sum(x * (y - b * x) for x, y in pts)
        meat += g * g
    se = math.sqrt(meat) / sxx
    return b, se, sxy / math.sqrt(sxx * syy), n


def ranks(v):
    o = sorted(range(len(v)), key=lambda i: v[i])
    r = [0.0] * len(v)
    for j, i in enumerate(o):
        r[i] = float(j)
    return r


def pearson(a, b):
    n = len(a)
    if n < 3:
        return float('nan')
    ma, mb = sum(a) / n, sum(b) / n
    sa = math.sqrt(sum((x - ma) ** 2 for x in a))
    sb = math.sqrt(sum((x - mb) ** 2 for x in b))
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (sa * sb) if sa > 0 and sb > 0 else float('nan')


def t1(sta, P, R, births):
    by_ep = collections.defaultdict(dict)
    for (t, s, v), _ in R.items():
        if (t, s, v) in P:
            by_ep[t][(s, v)] = P[(t, s, v)][0]
    pdc = {}
    for t, vals in by_ep.items():
        for k, x in center(vals).items():
            pdc[(t,) + k] = x
    # arcs
    series = collections.defaultdict(list)
    for (t, s, v), x in pdc.items():
        series[(s, v)].append(t)
    arcs = []
    for key, ts in series.items():
        ts.sort()
        b = births.get(key, set())
        cur = [ts[0]]
        for t in ts[1:]:
            if t - cur[-1] > 90.0 or t in b:
                arcs.append((key, cur))
                cur = []
            cur.append(t)
        arcs.append((key, cur))
    arcs = [(k, ts) for k, ts in arcs if len(ts) >= 40]
    blk, ep, plc = {}, {}, {}
    persist, size = [], []
    sgn = collections.Counter()
    by_sta_arcs = collections.defaultdict(list)
    for i, (key, ts) in enumerate(arcs):
        by_sta_arcs[key[0]].append(i)
    arc_p = {}
    for i, (key, ts) in enumerate(arcs):
        y = [R[(t,) + key] for t in ts]
        x = [pdc[(t,) + key] for t in ts]
        my, mx = sum(y) / len(y), sum(x) / len(x)
        y = [a - my for a in y]
        x = [a - mx for a in x]
        arc_p[i] = dict(zip(ts, x))
        ep[i] = list(zip(x, y))
        bl = collections.defaultdict(list)
        for t, a, b in zip(ts, x, y):
            bl[int((t - ts[0]) // 600)].append((a, b))
        bm = [(sum(a for a, _ in v) / len(v), sum(b for _, b in v) / len(v)) for v in bl.values() if len(v) >= 15]
        if len(bm) >= 2:
            blk[i] = bm
            sx = math.sqrt(sum(a * a for a, _ in bm) / len(bm))
            sy = math.sqrt(sum(b * b for _, b in bm) / len(bm))
            persist.append(sy)
            size.append(sx)
            if sx >= 3e-4:
                sxy = sum(a * b for a, b in bm)
                sgn['pos' if sxy > 0 else 'neg'] += 1
    # placebo: p of the next satellite (sorted) of the same station with the most overlap
    for i, (key, ts) in enumerate(arcs):
        if i not in blk:
            continue
        tset = set(ts)
        best, bo = None, 0
        for j in by_sta_arcs[key[0]]:
            if j == i or arcs[j][0][1] == key[1]:
                continue
            o = len(tset & set(arcs[j][1]))
            if o > bo:
                best, bo = j, o
        if best is None or bo < 0.8 * len(ts):
            continue
        xp = arc_p[best]
        common = [t for t in ts if t in xp]
        y = [R[(t,) + key] for t in common]
        x = [xp[t] for t in common]
        my, mx = sum(y) / len(y), sum(x) / len(x)
        bl = collections.defaultdict(list)
        for t, a, b in zip(common, x, y):
            bl[int((t - ts[0]) // 600)].append((a - mx, b - my))
        bm = [(sum(a for a, _ in v) / len(v), sum(b for _, b in v) / len(v)) for v in bl.values() if len(v) >= 15]
        if len(bm) >= 2:
            plc[i] = bm
    rk = pearson(ranks(persist), ranks(size))
    allp = [P[k][0] for k in R if k in P]
    return dict(arcs=len(arcs), blk=regress(blk), plc=regress(plc), ep=regress(ep), rank=rk, sgn=sgn,
                p_rms=math.sqrt(sum(a * a for a in allp) / len(allp)),
                pdc_rms=math.sqrt(sum(a * a for v in ep.values() for a, _ in v) / sum(len(v) for v in ep.values())),
                pblk_rms=math.sqrt(sum(a * a for v in blk.values() for a, _ in v) / sum(len(v) for v in blk.values())),
                rblk_rms=math.sqrt(sum(b * b for v in blk.values() for _, b in v) / sum(len(v) for v in blk.values())))


def t2(d, P):
    by_ep = collections.defaultdict(dict)
    for (t, s, v), val in P.items():
        by_ep[t][(s, v)] = val[0]
    psc = {}
    for t, vals in by_ep.items():
        for (s, v), x in stcenter(vals).items():
            psc[(t, v, s)] = x
    los = collections.defaultdict(dict)        # (t, sat) -> {sta: p}
    for (t, v, s), x in psc.items():
        los[(t, v)][s] = x
    rows = rw.load(rw.rundir(d) if not os.path.isdir(d) else d)
    out = {}
    for k in LAGS:
        recs = collections.defaultdict(list)    # t -> [(sat, dphi, dm)]
        for (sat, sig), lst in rows.items():
            if sig != 'L2W':
                continue
            for cl in rw.clones(lst):
                cl = [x for x in cl if x['age'] >= 1800.0]
                by = {x['tow']: x for x in cl}
                for x in cl:
                    y = by.get(round(x['tow'] + 30.0 * k, 1))
                    if y is None:
                        continue
                    a, b = los.get((x['tow'], sat), {}), los.get((y['tow'], sat), {})
                    both = [s for s in a if s in b]
                    if not both:
                        continue
                    dm = sum(b[s] - a[s] for s in both) / len(both)
                    recs[x['tow']].append((sat, y['phi'] - x['phi'], dm))
        clus = collections.defaultdict(list)
        raw = collections.defaultdict(list)
        for t, lst in recs.items():
            if len(lst) < 3:
                continue
            mp = sum(a for _, a, _ in lst) / len(lst)
            mm = sum(c for _, _, c in lst) / len(lst)
            for sat, a, c in lst:
                clus[sat].append((c - mm, a - mp))
                raw[sat].append((c, a))
        b, se, r, n = regress(clus)
        dm_rms = math.sqrt(sum(x * x for v in clus.values() for x, _ in v) / max(1, n))
        dp_rms = math.sqrt(sum(y * y for v in clus.values() for _, y in v) / max(1, n))
        out[k] = (b, se, r, n, dm_rms, dp_rms, regress(raw)[2])
    return out


def main(runs):
    for run in runs:
        d = rw.rundir(run)
        sta, P, R, births = load(d)
        print(f'== {run}: {len(P)} lines of sight with p, {len(R)} with an IF phase residual')
        bks = [abs(v[1]) for v in P.values()]
        stecs = [v[2] for v in P.values()]
        print(f'   |B.k| p50/p90 {1e6 * st.median(bks):.1f}/{1e6 * rw.q(bks, 0.9):.1f} uT; STEC p10/p50/p90 '
              f'{rw.q(stecs, 0.1):.0f}/{st.median(stecs):.0f}/{rw.q(stecs, 0.9):.0f} TECU; '
              f'|p| p50/p90 {1e3 * st.median(abs(v[0]) for v in P.values()):.2f}/{1e3 * rw.q([abs(v[0]) for v in P.values()], 0.9):.2f} mm')
        r1 = t1(sta, P, R, births)
        b, se, r, n = r1['blk']
        print(f'   T1 arcs {r1["arcs"]}; rms p {1e3 * r1["p_rms"]:.2f} mm, absorbed form {1e3 * r1["pdc_rms"]:.2f} mm, '
              f'its 20-epoch block means {1e3 * r1["pblk_rms"]:.2f} mm; residual block means {1e3 * r1["rblk_rms"]:.2f} mm')
        print(f'   T1 block-mean slope {b:+.3f} (clustered se {se:.3f}, t {b / se:+.1f}), r {r:+.3f}, n {n}')
        b, se, r, n = r1['plc']
        print(f'   T1 placebo slope {b:+.3f} (se {se:.3f}, t {b / se:+.1f}), r {r:+.3f}, n {n}')
        b, se, r, n = r1['ep']
        print(f'   T1 epoch-level slope {b:+.3f} (se {se:.3f}, t {b / se:+.1f}), r {r:+.3f}, n {n}')
        print(f'   T1 arcs with block-mean p rms >= 0.3 mm: positive within-arc slope {r1["sgn"]["pos"]}, '
              f'negative {r1["sgn"]["neg"]}; rank corr of residual persistence with p size {r1["rank"]:+.3f}')
        r2 = t2(d, P)
        for k, (b, se, r, n, dm, dp, rraw) in r2.items():
            print(f'   T2 lag {k:3d}: slope {b:+.3f} (se {se:.3f}), r {r:+.3f}, r2 {r * r:.3f}, n {n}; '
                  f'rms dm {1e3 * dm:.2f} mm, rms dPhi {1e3 * dp:.2f} mm (r without de-commoning {rraw:+.3f})')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])

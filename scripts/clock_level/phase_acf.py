"""The colour of the ionosphere-free post-fit phase residuals, per station and station class.

    python scripts/clock_level/phase_acf.py <run> [<run> ...]

From logs/network_pf_rows.csv (signed v = y - A x) and logs/network_stations.csv: per station and
satellite, the ionosphere-free phase residual 2.5457 v(L1) - 1.5457 v(L2W) over arcs of at least
40 epochs (split at gaps above 90 s), de-meaned per arc; per station the pooled autocorrelation at
lags 1, 2, 5, 20 and 60 epochs and the residual's rms; then per class (tropical |latitude| < 23.5,
mid 23.5 to 50, high above 50 degrees) the medians, and a white-plus-Gauss-Markov decomposition of
the class median: phi = acf(2) / acf(1), the Gauss-Markov share s = acf(1) / phi of the variance,
its time constant -30 / ln(phi) s, and the two sigmas from the class's median rms.  This is the
calibration target of a sim fixture's coloured phase component (card FWS section 5): a post-fit
quantity, so the sim is graded on the same instrument, not on its injected parameters.
"""
import collections, csv, math, os, statistics as st, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
F1, F2 = 1575.42e6, 1227.60e6
A1, A2 = F1 * F1 / (F1 * F1 - F2 * F2), -F2 * F2 / (F1 * F1 - F2 * F2)
LAGS = (1, 2, 5, 20, 60)


def rundir(r):
    return r if os.path.isdir(r) else ROOT + '/runs/' + r


def latitude(x, y, z):
    a, f = 6378137.0, 1.0 / 298.257223563
    e2 = f * (2 - f)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1 - e2))
    for _ in range(5):
        n = a / math.sqrt(1 - e2 * math.sin(lat) ** 2)
        h = p / math.cos(lat) - n
        lat = math.atan2(z, p * (1 - e2 * n / (n + h)))
    return math.degrees(lat)


def klass(lat):
    a = abs(lat)
    return 'tropical' if a < 23.5 else ('mid' if a <= 50.0 else 'high')


def per_station(d):
    lat = {}
    for r in csv.DictReader(open(d + '/logs/network_stations.csv')):
        lat[r['station']] = latitude(float(r['ecef_x']), float(r['ecef_y']), float(r['ecef_z']))
    v1, v2 = {}, {}
    with open(d + '/logs/network_pf_rows.csv') as f:
        for r in csv.DictReader(ln for ln in f if not ln.startswith('#')):
            if r['kind'] != 'phase':
                continue
            k = (r['station'], r['sat'], round(float(r['tow']), 1))
            if r['signal'] in ('L1C', 'L1W'):
                v1[k] = float(r['resid_m'])
            elif r['signal'] == 'L2W':
                v2[k] = float(r['resid_m'])
    ser = collections.defaultdict(list)
    for k, a in v1.items():
        if k in v2:
            ser[(k[0], k[1])].append((k[2], A1 * a + A2 * v2[k]))
    acc = collections.defaultdict(lambda: {'n0': 0, 's0': 0.0, **{f's{L}': 0.0 for L in LAGS}})
    for (sta, sat), lst in ser.items():
        lst.sort()
        arcs, cur = [], [lst[0]]
        for t, v in lst[1:]:
            if t - cur[-1][0] > 90.0:
                arcs.append(cur)
                cur = []
            cur.append((t, v))
        arcs.append(cur)
        for arc in arcs:
            if len(arc) < 40:
                continue
            m = sum(v for _, v in arc) / len(arc)
            by = {round(t, 1): v - m for t, v in arc}
            a = acc[sta]
            for t, v in by.items():
                a['n0'] += 1
                a['s0'] += v * v
                for L in LAGS:
                    w = by.get(round(t + 30.0 * L, 1))
                    if w is not None:
                        a[f's{L}'] += v * w
    out = {}
    for sta, a in acc.items():
        if a['n0'] < 200 or a['s0'] <= 0:
            continue
        out[sta] = dict(lat=lat.get(sta, float('nan')), rms=math.sqrt(a['s0'] / a['n0']),
                        acf={L: a[f's{L}'] / a['s0'] for L in LAGS})
    return out


def main(runs):
    for run in runs:
        res = per_station(rundir(run))
        print(f'== {run}: {len(res)} stations')
        cls = collections.defaultdict(list)
        for sta, r in res.items():
            cls[klass(r['lat'])].append(r)
        for c in ('tropical', 'mid', 'high'):
            rs = cls.get(c, [])
            if not rs:
                continue
            med = {L: st.median(r['acf'][L] for r in rs) for L in LAGS}
            rms = st.median(r['rms'] for r in rs)
            line = (f'   {c:8s} n {len(rs):2d}: rms {1e3 * rms:.2f} mm; acf ' +
                    ' / '.join(f'{med[L]:+.2f}' for L in LAGS))
            if med[1] > 0.02 and 0.0 < med[2] < med[1]:
                phi = med[2] / med[1]
                s = min(1.0, med[1] / phi)
                tau = -30.0 / math.log(phi)
                line += (f'; GM share {s:.2f}, tau {tau:.0f} s, sigma GM {1e3 * rms * math.sqrt(s):.2f} mm, '
                         f'white {1e3 * rms * math.sqrt(max(0.0, 1 - s)):.2f} mm')
            print(line)
        worst = sorted(res.items(), key=lambda kv: -kv[1]['acf'][1])[:8]
        print('   largest acf(1): ' + ', '.join(f'{s} {r["acf"][1]:.2f}' for s, r in worst))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])

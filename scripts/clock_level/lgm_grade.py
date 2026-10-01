"""Card LGM: the phase-colour state's estimate against the sim fixture's injected colour (gate G1).

    python scripts/clock_level/lgm_grade.py <run> [<run> ...] [--fixture runs/simnet60_quiet_col_24h]
                                            [--min-age 600]

From the run's logs/network_phase_colour.csv (network.trace_phase_colour: per station, satellite and
epoch the state's estimate x and its stated sd) and each station's truth_phase_colour.csv in the
fixture (e_m, the band-common colour added to every phase observation).  Rows are grouped into arcs
per (station, satellite), broken at a gap above 90 s; only rows whose arc is at least --min-age
seconds old are graded (the state starts from its prior at every arc's start).  Per station class
(tropical |latitude| < 23.5, mid 23.5 to 50, high above 50 degrees, the classes of phase_acf.py):

  ratio      rms(x - e) / rms(e), the pre-registered G1 statistic (at most 0.7 passes)
  corr       the correlation of x with e
  ratio_dm   the same ratio with x - e de-meaned per arc (descriptive: the float ambiguities take the
             arc mean of the colour, which the state is not asked to carry)
  nees       mean ((x - e) / sd)^2 (the state's stated sd against its actual error; about 1 when
             consistent, a lower bound on the de-meaned error's)
"""
import argparse, collections, csv, math, os, statistics as st

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')


def rundir(r):
    return r if os.path.isdir(r) else ROOT + '/runs/' + r


def latitude(x, y, z):
    a, f = 6378137.0, 1.0 / 298.257223563
    e2 = f * (2 - f)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1 - e2))
    for _ in range(5):
        n = a / math.sqrt(1 - e2 * math.sin(lat) ** 2)
        lat = math.atan2(z + e2 * n * math.sin(lat), p)
    return math.degrees(lat)


def klass(lat):
    a = abs(lat)
    return 'tropical' if a < 23.5 else ('mid' if a <= 50.0 else 'high')


def station_classes(d):
    out = {}
    with open(d + '/logs/network_stations.csv') as f:
        for r in csv.DictReader(f):
            name = r.get('station') or r.get('name')
            try:
                x, y, z = float(r['ecef_x']), float(r['ecef_y']), float(r['ecef_z'])
            except (KeyError, ValueError):
                continue
            out[name] = klass(latitude(x, y, z))
    return out


def load_truth(fix, station, tows):
    p = f'{fix}/{station}/truth_phase_colour.csv'
    out = {}
    if not os.path.exists(p):
        return out
    lo, hi = min(tows) - 1.0, max(tows) + 1.0
    with open(p) as f:
        for r in csv.DictReader(f):
            t = float(r['tow_s'])
            if lo <= t <= hi:
                out[(r['satellite'], round(t, 1))] = float(r['e_m'])
    return out


def grade(run, fix, min_age):
    d = rundir(run)
    cls = station_classes(d)
    rows = collections.defaultdict(list)          # (station, sat) -> [(tow, x, sd)]
    with open(d + '/logs/network_phase_colour.csv') as f:
        for r in csv.DictReader(f):
            rows[(r['station'], r['sat'])].append((float(r['tow']), float(r['x_m']), float(r['sd_m'])))
    tows_by_sta = collections.defaultdict(set)
    for (sta, _), lst in rows.items():
        for t, _, _ in lst:
            tows_by_sta[sta].add(t)
    truth = {sta: load_truth(fix, sta, ts) for sta, ts in tows_by_sta.items()}
    acc = collections.defaultdict(lambda: {'d2': 0.0, 'e2': 0.0, 'n': 0, 'xe': [], 'dm2': 0.0, 'z2': 0.0})
    missing = 0
    for (sta, sat), lst in rows.items():
        lst.sort()
        arcs, cur = [], [lst[0]]
        for a, b in zip(lst, lst[1:]):
            if b[0] - a[0] > 90.0:
                arcs.append(cur)
                cur = []
            cur.append(b)
        arcs.append(cur)
        c = cls.get(sta, '?')
        for arc in arcs:
            t0 = arc[0][0]
            use = []
            for t, x, sd in arc:
                if t - t0 < min_age:
                    continue
                e = truth[sta].get((sat, round(t, 1)))
                if e is None:
                    missing += 1
                    continue
                use.append((x, e, sd))
            if len(use) < 2:
                continue
            md = st.fmean(x - e for x, e, _ in use)
            A = acc[c]
            for x, e, sd in use:
                A['d2'] += (x - e) ** 2
                A['e2'] += e * e
                A['dm2'] += (x - e - md) ** 2
                A['z2'] += ((x - e) / sd) ** 2 if sd > 0 else 0.0
                A['n'] += 1
                A['xe'].append((x, e))
    print(f'{os.path.basename(d)} (min arc age {min_age:.0f} s; {missing} rows without truth)')
    for c in ('tropical', 'mid', 'high'):
        A = acc.get(c)
        if not A or A['n'] < 10:
            continue
        n = A['n']
        xs = [x for x, _ in A['xe']]
        es = [e for _, e in A['xe']]
        mx, me = st.fmean(xs), st.fmean(es)
        sxy = sum((x - mx) * (e - me) for x, e in A['xe'])
        sxx = sum((x - mx) ** 2 for x in xs)
        syy = sum((e - me) ** 2 for e in es)
        corr = sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else float('nan')
        print(f'   {c:8s} n {n:6d}: rms e {1e3 * math.sqrt(A["e2"] / n):.2f} mm, rms x '
              f'{1e3 * math.sqrt(sum(x * x for x in xs) / n):.2f} mm; ratio {math.sqrt(A["d2"] / A["e2"]):.3f}, '
              f'corr {corr:+.3f}, ratio_dm {math.sqrt(A["dm2"] / A["e2"]):.3f}, nees {A["z2"] / n:.2f}')


def main():
    p = argparse.ArgumentParser()
    p.add_argument('runs', nargs='+')
    p.add_argument('--fixture', default=ROOT + '/runs/simnet60_quiet_col_24h')
    p.add_argument('--min-age', type=float, default=600.0)
    a = p.parse_args()
    for r in a.runs:
        grade(r, a.fixture, a.min_age)


if __name__ == '__main__':
    main()

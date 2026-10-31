#!/usr/bin/env python3
"""Grade of the roster-coordinate arms: the consumer, the census and the clock wire.

Arms are full 6 h field wires (tags `snxr_<arm>`, directories `_rap_snxr_<arm>` under the runs
root).  Their consumers are scored against two coordinate sources: the held coordinates (the
SSC extrapolation the configs carry) and the weekly SINEX.  The census replays of each wire are
written by gcensus.py with keys `<key>` (held truth, tracked site list) and `<key>x` (SINEX
truth, `--sites site_list_sinex.json`).

  consumer [--ref TAG] <tag> ...
                            the closed-loop consumer of each wire by hour, against both truths
                            (the SINEX truth is the trace shifted by the coordinate difference);
                            with --ref, the change of each wire from TAG by hour and its largest
                            absolute value over hours 2 to 6
  census <refkey> <key> ... per census key: the median site p50 by hour, and against <refkey> the paired
                            change of each site's mean hourly p50 over hours 2 to 6 and of hour 1,
                            on the truth the keys were replayed against; `x` keys are paired with `<refkey>x`
  sites <refkey> <key> ...  per census site (and WTZZ00DEU): the coordinate difference SINEX minus held, the mean hourly
                            p50 over hours 2 to 6 of <refkey> on the held truth and on the SINEX truth, and the change of
                            each <key> from <refkey> on the held truth
  truth-check <key> ...     the replays of `<key>` and `<key>x` differ by the coordinate difference only
                            (the consumer does not read its truth): the largest departure, in metres
  clock <ref tag> <tag> ... per epoch RMS over satellites of the clock difference to the reference wire,
                            after removing the per-epoch common mean (per constellation, and over all
                            satellites at once), by hour
  congruence [--stations A,B] <tag> ...
                            the station congruence statistic geo(t) of each run (default station MIZU00JPN),
                            recomputed from logs/network_congruence.csv by the rule of Congruence.hpp and
                            checked against the run-end line of stdout.txt; the roster median and the
                            stations above 4
  r3rows <file> ...         the hours 2 to 6 e_clk p50 rows of saved `ftc_step_attrib.py --r3` outputs, per
                            system (G, E) and set (all, tracked, each also de-commoned), the first file as
                            the reference for the change rows

Runs root: RAPID_RUNS_ROOT as for rapid_arm.py.
"""
import collections
import csv
import json
import math
import os
import re
import statistics as st
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import msc_sites  # noqa: E402
from roster_from_sinex import ecef_to_enu  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE)).replace('\\', '/')
RUNS = os.environ.get('RAPID_RUNS_ROOT', ROOT + '/runs').replace('\\', '/')
T0, NH = 259200.0, 6
SITES_HELD = os.path.join(HERE, 'site_list.json')
SITES_SINEX = os.path.join(HERE, 'site_list_sinex.json')


def rundir(tag):
    """A run directory by tag (`_rap_<tag>` under the runs root) or by its own name."""
    return f'{RUNS}/{tag}' if os.path.isdir(f'{RUNS}/{tag}') else f'{RUNS}/_rap_{tag}'


def hour_of(t):
    return int(round((t - T0) * 1000)) // 3600000


def trace(d):
    """Rows of a closed_loop_trace.csv as (tow, east, north, up, 3D) with the epoch's own 3D error."""
    out = []
    p = f'{d}/logs/closed_loop_trace.csv'
    if not os.path.exists(p):
        return out
    for r in csv.DictReader(open(p)):
        try:
            out.append((float(r['tow']), float(r['east_m']), float(r['north_m']), float(r['up_m']), float(r['pos_err_3d_m'])))
        except (ValueError, TypeError, KeyError):
            continue
    return out


def p50_by_hour(rows):
    by = {h: [] for h in range(NH)}
    for t, e, n, u, d3 in rows:
        h = hour_of(t)
        if h in by:
            by[h].append(d3)
    return [st.median(by[h]) if by[h] else float('nan') for h in range(NH)]


def shifted(rows, d):
    """The rows against the other source: the error against truth B is the error against A minus (B - A)."""
    return [(t, e - d[0], n - d[1], u - d[2], math.sqrt((e - d[0]) ** 2 + (n - d[1]) ** 2 + (u - d[2]) ** 2))
            for t, e, n, u, _ in rows]


def site_shift(held, sinex):
    """ENU of (sinex - held) at held, metres."""
    return ecef_to_enu([s - h for s, h in zip(sinex, held)], held)


def fmt(v):
    return ' '.join('  nan' if x != x else f'{x:5.3f}' for x in v)


def mean26(v):
    return st.mean(v[1:])


# ----------------------------------------------------------------------------- consumer
def cmd_consumer(tags, ref=None):
    sx = json.load(open(SITES_SINEX))['consumer']
    print('closed-loop consumer WTZZ00DEU, p50 of the 3D error by hour (m), hours from 00:00; mean over hours 2 to 6')
    print(f'{"wire":10s} {"truth":6s} ' + ' '.join(f'h{h + 1:<5d}' for h in range(NH)) + '   mean 2-6')
    p50 = {}
    for tag in tags + ([ref] if ref and ref not in tags else []):
        d = rundir(tag)
        arm = json.load(open(f'{d}/arm.json'))
        held = arm['network']['closed_loop']['station']['position_ecef']
        rows = trace(d)
        if not rows:
            print(f'{tag:10s} no trace')
            continue
        sh = site_shift(held, sx['ecef'])
        for name, r in (('held', rows), ('sinex', shifted(rows, sh))):
            v = p50_by_hour(r)
            p50[(tag, name)] = v
            if tag in tags:
                print(f'{tag:10s} {name:6s} ' + ' '.join(f'{x:6.4f}' for x in v) + f'   {mean26(v):.4f}')
    first = json.load(open(f'{rundir(tags[0])}/arm.json'))['network']['closed_loop']['station']['position_ecef']
    print('coordinate difference SINEX minus held at the consumer (E, N, U mm): '
          + ', '.join(f'{1000 * x:+.1f}' for x in site_shift(first, sx['ecef'])))
    if ref:
        print(f'change from {ref} (m), by hour, and the largest absolute change over hours 2 to 6')
        for tag in tags:
            for name in ('held', 'sinex'):
                if (tag, name) in p50 and (ref, name) in p50 and tag != ref:
                    d = [a - b for a, b in zip(p50[(tag, name)], p50[(ref, name)])]
                    print(f'{tag:10s} {name:6s} ' + ' '.join(f'{x:+.4f}' for x in d)
                          + f'   largest |d| h2-6 {max(abs(x) for x in d[1:]):.4f}')


# ----------------------------------------------------------------------------- census
def names_of(path):
    return ['wtzz'] + [s['name'][:4].lower() for s in json.load(open(path))['sites']]


def key_hours(key):
    out = {}
    for n in names_of(SITES_HELD):
        rows = trace(f'{RUNS}/_rap_{key}_{n}')
        out[n] = p50_by_hour(rows) if rows else None
    return out


def cmd_census(ref, keys):
    def pair(key):
        if key == ref + 'x':
            return ref                      # the same wire on the other truth: the effect of the truth alone
        return ref + ('x' if key.endswith('x') and not ref.endswith('x') else '')
    for key in [ref] + keys:
        K = key_hours(key)
        sites = [n for n in K if n != 'wtzz' and K[n]]
        med = [st.median([K[n][h] for n in sites]) for h in range(NH)]
        print(f'== {key}: {len(sites)} sites; median site p50 by hour ' + fmt(med)
              + ('; WTZZ ' + fmt(K['wtzz']) if K['wtzz'] else ''))
        if key == ref:
            continue
        R = key_hours(pair(key))
        both = [n for n in sites if R[n]]
        d26 = [mean26(K[n]) - mean26(R[n]) for n in both]
        d1 = [K[n][0] - R[n][0] for n in both]
        worse10 = [n for n, x in zip(both, d26) if x > 0.010]
        print(f'   vs {pair(key)} paired, n {len(both)}: hours 2-6 median {st.median(d26):+.5f} m, mean {st.mean(d26):+.5f} m, '
              f'better {sum(x < -0.01 for x in d26)}, worse {sum(x > 0.01 for x in d26)} at 1 cm, largest |d| {max(abs(x) for x in d26):.4f} m, '
              f'above +0.010 m: {len(worse10)}{" (" + ",".join(n.upper() for n in worse10) + ")" if worse10 else ""}')
        print(f'   hour 1 median {st.median(d1):+.4f} m, largest |d| {max(abs(x) for x in d1):.4f} m; '
              f'WTZZ replay hours 2-6 {mean26(K["wtzz"]) - mean26(R["wtzz"]):+.4f} m')


# ----------------------------------------------------------------------------- sites
def cmd_sites(ref, keys):
    held_l = {s['name'][:4].lower(): s for s in json.load(open(SITES_HELD))['sites']}
    sx_doc = json.load(open(SITES_SINEX))
    sx_l = {s['name'][:4].lower(): s for s in sx_doc['sites']}
    R, RX = key_hours(ref), key_hours(ref + 'x')
    K = {k: key_hours(k) for k in keys}
    print(f'{"site":10s} {"SNX-held mm":>11s}  {ref} h2-6 held  sinex  d(truth)  ' + '  '.join(f'd({k})' for k in keys))
    for n in names_of(SITES_HELD):
        if n == 'wtzz':
            arm = json.load(open(f'{RUNS}/_rap_{ref}_wtzz/arm.json'))
            held, sx, label = arm['network']['closed_loop']['station']['position_ecef'], sx_doc['consumer']['ecef'], 'WTZZ00DEU'
        else:
            held, sx, label = held_l[n]['ecef'], sx_l[n]['ecef'], held_l[n]['name']
        off = math.sqrt(sum((a - b) ** 2 for a, b in zip(sx, held))) * 1000
        a, b = mean26(R[n]), mean26(RX[n])
        print(f'{label:10s} {off:11.1f}  {a:19.4f} {b:7.4f} {b - a:+9.4f}  '
              + '  '.join(f'{mean26(K[k][n]) - a:+8.4f}' for k in keys))


# ----------------------------------------------------------------------------- truth check
def cmd_truth_check(keys):
    held_l = {s['name'][:4].lower(): s for s in json.load(open(SITES_HELD))['sites']}
    sx_doc = json.load(open(SITES_SINEX))
    sx_l = {s['name'][:4].lower(): s for s in sx_doc['sites']}
    for key in keys:
        worst, nrows, worst_site = 0.0, 0, ''
        for n in names_of(SITES_HELD):
            a, b = trace(f'{RUNS}/_rap_{key}_{n}'), trace(f'{RUNS}/_rap_{key}x_{n}')
            if not a or len(a) != len(b):
                print(f'{key} {n}: traces missing or of different length ({len(a)}, {len(b)})')
                continue
            if n == 'wtzz':
                arm = json.load(open(f'{RUNS}/_rap_{key}_wtzz/arm.json'))
                held = arm['network']['closed_loop']['station']['position_ecef']
                sx = sx_doc['consumer']['ecef']
            else:
                held, sx = held_l[n]['ecef'], sx_l[n]['ecef']
            d = site_shift(held, sx)
            for (t, e, nn, u, _), (t2, e2, n2, u2, _) in zip(a, b):
                assert abs(t - t2) < 1e-6
                dev = max(abs((e - e2) - d[0]), abs((nn - n2) - d[1]), abs((u - u2) - d[2]))
                nrows += 1
                if dev > worst:
                    worst, worst_site = dev, n
        print(f'{key}: {nrows} epoch rows over {len(names_of(SITES_HELD))} consumers; the held-truth and SINEX-truth replays differ '
              f'by the coordinate difference to within {worst:.2e} m (largest at {worst_site.upper()})')


# ----------------------------------------------------------------------------- clock
def clock_records(d):
    """{tow: {sat: c0_m}} from the C records of a network products file."""
    out, cur = {}, None
    for line in open(f'{d}/products.txt', errors='replace'):
        c = line[:1]
        if c == 'E':
            p = line.split(',')
            cur = out.setdefault(round(float(p[2]), 3), {})
        elif c == 'C' and cur is not None:
            p = line.split(',')
            try:
                cur[p[1]] = float(p[2])
            except ValueError:
                pass
    return out


def rms(v):
    return math.sqrt(sum(x * x for x in v) / len(v)) if v else float('nan')


def cmd_clock(ref, tags):
    R = clock_records(rundir(ref))
    print(f'clock difference to {ref}: per epoch RMS over satellites (m) after removing the per-epoch common mean; '
          f'mean of the epochs of each hour; epochs in both wires')
    print(f'{"wire":8s} {"set":9s} ' + ' '.join(f'h{h + 1:<5d}' for h in range(NH)) + '   hours 2-6 mean   max epoch')
    for tag in tags:
        A = clock_records(rundir(tag))
        epochs = sorted(set(R) & set(A))
        series = {'GPS': {}, 'Galileo': {}, 'pooled': {}, 'raw GPS': {}, 'raw Gal': {}}
        per_sat = {}                                    # satellite -> differences after the per-constellation mean, hours 2 to 6
        for t in epochs:
            h = hour_of(t)
            if h not in range(NH):
                continue
            d = {s: A[t][s] - R[t][s] for s in set(R[t]) & set(A[t])}
            for name, sel in (('GPS', 'G'), ('Galileo', 'E')):
                v = [x for s, x in d.items() if s[0] == sel]
                if len(v) >= 3:
                    m = st.mean(v)
                    series[name].setdefault(h, []).append(rms([x - m for x in v]))
                    if h >= 1:
                        for s_, x in d.items():
                            if s_[0] == sel:
                                per_sat.setdefault(s_, []).append(x - m)
                    series['raw ' + ('GPS' if sel == 'G' else 'Gal')].setdefault(h, []).append(rms(v))
            allv = [x for s, x in d.items() if s[0] in 'GE']
            if len(allv) >= 3:
                m = st.mean(allv)
                series['pooled'].setdefault(h, []).append(rms([x - m for x in allv]))
        print(f'{tag:8s} epochs in both {len(epochs)} of {len(R)} and {len(A)}')
        for name in ('GPS', 'Galileo', 'pooled', 'raw GPS', 'raw Gal'):
            by = series[name]
            row = [st.mean(by[h]) if by.get(h) else float('nan') for h in range(NH)]
            allv = [x for h in range(1, NH) for x in by.get(h, [])]
            print(f'{tag:8s} {name:9s} ' + ' '.join(f'{x:6.4f}' if x == x else '   nan' for x in row)
                  + f'   {st.mean(allv) if allv else float("nan"):.5f}        {max(allv) if allv else float("nan"):.4f}')
        means = {k: st.mean(v) for k, v in per_sat.items() if len(v) > 100}
        if means:
            tot = rms([x for v in per_sat.values() for x in v])
            top = sorted(means.items(), key=lambda kv: -abs(kv[1]))[:5]
            print(f'{tag:8s} hours 2-6, per-constellation set: RMS {1000 * tot:.2f} mm, of which the constant per-satellite part '
                  f'(RMS of the satellite means, {len(means)} satellites) {1000 * rms(list(means.values())):.2f} mm; largest means (mm): '
                  + ', '.join(f'{k} {1000 * m:+.1f}' for k, m in top))


# ----------------------------------------------------------------------------- congruence
CHI2_GEO_MEAN_1 = 0.2807297418      # kBlockChi2GeoMean1, include/network/qc/BlockBaarda.hpp


def chi2_geo_mean(b):
    """Geometric mean of a chi-square variate with b degrees of freedom, 2 exp(psi(b/2)) (blockChi2GeoMean)."""
    if b <= 1:
        return CHI2_GEO_MEAN_1
    psi = -1.9635100260214235 if b % 2 == 1 else -0.5772156649015329
    x = 0.5 if b % 2 == 1 else 1.0
    while x + 1.0 <= 0.5 * b + 1e-9:
        psi += 1.0 / x
        x += 1.0
    return 2.0 * math.exp(psi)


def congruence_geo(d):
    """{station: (geo(t), station-epochs, epochs above t 4)} of a run, from logs/network_congruence.csv.
    geo(t) is the geometric mean of the chi-square-1 equivalent of T_raw / sigma0^2 divided by its null
    value (NetworkCongruence::Ladder::geoT); every row folds in, degenerate ones included."""
    logs, cnt, over = collections.defaultdict(list), collections.Counter(), collections.Counter()
    for r in csv.DictReader(open(f'{d}/logs/network_congruence.csv')):
        q, T, s0 = int(r['q']), float(r['T_raw']), float(r['sigma0'])
        cnt[r['station']] += 1
        over[r['station']] += float(r['t']) > 4.0
        if q < 1 or not (T >= 0.0) or not math.isfinite(T):
            continue
        u = T * q * (CHI2_GEO_MEAN_1 / chi2_geo_mean(q)) / (s0 * s0 if s0 > 0 else 1.0)
        if u > 0.0 and math.isfinite(u):
            logs[r['station']].append(math.log(u))
    return {k: (math.exp(sum(v) / len(v)) / CHI2_GEO_MEAN_1, cnt[k], over[k]) for k, v in logs.items()}


def congruence_stdout(d):
    """{station: geo(t)} of the run-end lines of stdout.txt (empty when the run has none)."""
    out = {}
    if os.path.exists(f'{d}/stdout.txt'):
        for line in open(f'{d}/stdout.txt', encoding='utf-8', errors='replace'):
            m = re.search(r'station congruence\) (\w{9})/CLEAN: n = \d+ .*geo\(t\) = ([0-9.e+-]+), \d+ over', line)
            if m:
                out[m.group(1)] = float(m.group(2))
    return out


def cmd_congruence(tags, stations):
    for tag in tags:
        d = rundir(tag)
        g, tab = congruence_geo(d), congruence_stdout(d)
        vals = [v[0] for v in g.values()]
        print(f'{tag}: {len(g)} stations; roster median geo(t) {st.median(vals):.3f}; above 4: '
              + (', '.join(f'{k} {v[0]:.2f}' for k, v in sorted(g.items(), key=lambda kv: -kv[1][0]) if v[0] > 4.0) or 'none'))
        for name in stations:
            if name not in g:
                print(f'   {name}: absent from the run')
                continue
            geo, n, over = g[name]
            ok = name in tab and abs(tab[name] - geo) <= 0.006 * max(1.0, geo)
            print(f'   {name}: geo(t) {geo:.3f} ({n} station-epochs, {over} above t 4); run-end line of stdout.txt '
                  + (f'{tab[name]:g}: {"agrees" if ok else "DISAGREES"}' if name in tab else 'not present'))


# ----------------------------------------------------------------------------- r3 rows
R3_SETS = (('all', 'all satellites'), ('trk', 'consumer-tracked'), ('all_dm', 'all, de-commoned'), ('trk_dm', 'tracked, de-commoned'))


def r3_rows(path):
    """{(system, set): (p50 of the hours 2-6 e_clk within-arc RMS, arcs)} of one saved ftc_step_attrib --r3 output."""
    out, sysl, sec = {}, None, None
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'R3  TRUTH GRADE AT THE IF LEVEL -- .*\[system (\w)\]', line)
        if m:
            sysl, sec = m.group(1), None
            continue
        if line.startswith('--- ') and sysl:
            if 'DIFFMEDIAN' in line:
                sec = 'all_dm' if 'ALL' in line else 'trk_dm'
            else:
                sec = 'trk' if 'TRACKED' in line else 'all' if 'ALL wire' in line else None
            continue
        t = line.split()
        if sysl and sec and t[:3] == ['hours', '2-6', 'e_clk'] and len(t) >= 6:
            out[(sysl, sec)] = (float(t[5]), int(t[3]))
    return out


def cmd_r3rows(files):
    rows = [r3_rows(f) for f in files]
    print('R3 e_clk, hours 2 to 6, p50 of the within-arc RMS against the CODE clock (m); change from the first file')
    print(f'{"system":6s} {"set":22s} ' + ' '.join(f'{os.path.basename(f)[:18]:>18s}' for f in files))
    for sysl in 'GE':
        for key, label in R3_SETS:
            if not all((sysl, key) in r for r in rows):
                print(f'{sysl:6s} {label:22s} missing in at least one file')
                continue
            v = [r[(sysl, key)][0] for r in rows]
            print(f'{sysl:6s} {label:22s} ' + ' '.join(f'{x:18.4f}' for x in v))
            if len(v) > 1:
                print(f'{"":6s} {"  change from first":22s} ' + ' '.join(f'{x - v[0]:+18.4f}' for x in v))


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    def consumer_args(x):
        return cmd_consumer(x[2:], x[1]) if x and x[0] == '--ref' else cmd_consumer(x)

    def congruence_args(x):
        return cmd_congruence(x[2:], x[1].split(',')) if x and x[0] == '--stations' else cmd_congruence(x, ['MIZU00JPN'])

    {'consumer': lambda: consumer_args(a[1:]), 'census': lambda: cmd_census(a[1], a[2:]),
     'congruence': lambda: congruence_args(a[1:]), 'r3rows': lambda: cmd_r3rows(a[1:]),
     'sites': lambda: cmd_sites(a[1], a[2:]), 'truth-check': lambda: cmd_truth_check(a[1:]), 'clock': lambda: cmd_clock(a[1], a[2:])}[a[0]]()

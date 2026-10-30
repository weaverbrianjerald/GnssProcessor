#!/usr/bin/env python3
"""The spread of the 16-site census under a neutral change of the wire (card NSP, 2026-10-29).

A neutral arm is the golden with one key changed that re-groups floating-point sums and nothing else
(`network.parallel_max_threads`, `network.parallel_carry`; card NSP section 2).  Two verbs.

wire  [--day D] <control run dir> <run dir> [<run dir> ...]
      How far each run's wire and decisions moved from the control's:
      - products.txt byte identity and the first epoch whose records differ;
      - the records present in one wire only (C clock, B code bias, P phase bias);
      - over hours 2 to 6, the between-satellite move: per epoch and constellation (per signal for B and P)
        the run-minus-control difference less its median over the satellites, a datum-invariant quantity;
        p50, p95 and max of its magnitude in mm (P can carry whole cycles where an integer level differs);
      - the decisions: rows of logs/network_events.csv keyed by (tow, tier, event, sat, station, action),
        the statistic left out, present in one run only, and the first epoch of such a row.

grade [--day D] [--obs-root R] [--piv KEY] [--json OUT] <control key> <neutral key> [<neutral key> ...]
      Reads the consumer-only replays of scripts/clock_level/gcensus.py (tags <key>_<site4> under
      RAPID_RUNS_ROOT, as gcensus.py does).  Per site (WTZZ00DEU and the 16 census sites): x = the mean of
      the hourly p50 over hours 2 to 6; d = x(neutral key) - x(control key), paired by site; over the K
      neutral keys the mean m, the sample SD s and the envelope max|d|.  Per key: the sites worse or better
      by more than 1 cm and the paired median.  With --piv, the PIV key's change d_P at each site against
      the neutral sample: r = the neutral changes at least as large as d_P, and the one-sided prediction
      bounds U95, U99 = m + t * s * sqrt(1 + 1/K) (Student's t, K - 1 degrees of freedom).  Reading
      (fixed in card NSP section 5): inside when r >= 1 or d_P <= U95; outside when r = 0 and d_P > U99;
      undecided otherwise.
"""
import csv, hashlib, json, math, os, statistics as st, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'clock_level'))

# One-sided Student's t quantiles by degrees of freedom (0.95 and 0.99).
T95 = {1: 6.314, 2: 2.920, 3: 2.353, 4: 2.132, 5: 2.015, 6: 1.943, 7: 1.895, 8: 1.860, 9: 1.833, 10: 1.812,
       11: 1.796, 12: 1.782, 13: 1.771, 14: 1.761, 15: 1.753, 16: 1.746, 17: 1.740, 18: 1.734, 19: 1.729,
       20: 1.725}
T99 = {1: 31.821, 2: 6.965, 3: 4.541, 4: 3.747, 5: 3.365, 6: 3.143, 7: 2.998, 8: 2.896, 9: 2.821, 10: 2.764,
       11: 2.718, 12: 2.681, 13: 2.650, 14: 2.624, 15: 2.602, 16: 2.583, 17: 2.567, 18: 2.552, 19: 2.539,
       20: 2.528}
CLAUSE_M = 0.01   # card BOF: a site is worse (better) when its change exceeds +1 cm (-1 cm)


def take(a, flag, conv, default):
    """Remove `flag value` from the argument list `a` and return the converted value (or the default)."""
    if flag in a:
        i = a.index(flag)
        v = conv(a[i + 1])
        del a[i:i + 2]
        return v
    return default


def gps_dow(doy, year=2024):
    """GPS day of week (Sunday 0) of day-of-year doy."""
    import datetime
    return (datetime.date(year, 1, 1) + datetime.timedelta(days=doy - 1)).isoweekday() % 7


def hms(tow):
    s = int(round(tow)) % 86400
    return f'{s // 3600:02d}:{s // 60 % 60:02d}:{s % 60:02d}'


def pct(v, q):
    """The q-quantile of v by linear interpolation (v non-empty)."""
    v = sorted(v)
    x = q * (len(v) - 1)
    i = int(math.floor(x))
    j = min(i + 1, len(v) - 1)
    return v[i] + (v[j] - v[i]) * (x - i)


def sha16(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()[:16]


# ----------------------------------------------------------------------------- wire
def read_wire(path):
    """{tow: {('C', sat) | ('B', sat, signal) | ('P', sat, signal): value}} from a products.txt."""
    ep, cur = {}, None
    with open(path, encoding='ascii', errors='replace') as f:
        for line in f:
            tag = line[:2]
            if tag == 'E,':
                cur = ep.setdefault(float(line.split(',')[2]), {})
            elif cur is None:
                continue
            elif tag == 'C,':
                p = line.split(',')
                cur[('C', p[1])] = float(p[2])
            elif tag in ('B,', 'P,'):
                p = line.split(',')
                cur[(p[0], p[1], p[2])] = float(p[3])
    return ep


def read_events(run):
    """Decision rows of logs/network_events.csv as a multiset keyed without the statistic."""
    rows = {}
    p = os.path.join(run, 'logs', 'network_events.csv')
    if not os.path.exists(p):
        return None
    with open(p, newline='', encoding='ascii', errors='replace') as f:
        for r in csv.DictReader(f):
            k = (r['tow'], r['tier'], r['event'], r['sat'], r['station'], r['action'])
            rows[k] = rows.get(k, 0) + 1
    return rows


def wire_move(ctl, run, t0):
    """First differing epoch, records in one wire only, and the between-satellite moves over hours 2-6."""
    first, only, res = None, {'C': 0, 'B': 0, 'P': 0}, {'C': [], 'B': [], 'P': []}
    for tow in sorted(set(ctl) | set(run)):
        a, b = ctl.get(tow, {}), run.get(tow, {})
        if first is None and a != b:
            first = tow
        for k in set(a) ^ set(b):
            only[k[0]] += 1
        if not 1.0 <= (tow - t0) / 3600.0 < 6.0:
            continue
        groups = {}
        for k in set(a) & set(b):
            g = (k[0], k[1][0]) if k[0] == 'C' else (k[0], k[1][0], k[2])
            groups.setdefault(g, []).append(b[k] - a[k])
        for g, d in groups.items():
            if len(d) < 2:
                continue   # one satellite has no between-satellite move
            med = st.median(d)
            res[g[0]].extend(abs(x - med) for x in d)
    return first, only, res


def event_move(ec, er):
    """Rows (with multiplicity) present in one run only, and the first epoch of such a row."""
    if ec is None or er is None:
        return None, None
    n, first = 0, None
    for k in set(ec) | set(er):
        dk = abs(ec.get(k, 0) - er.get(k, 0))
        if dk:
            n += dk
            t = float(k[0])
            first = t if first is None or t < first else first
    return n, first


def cmd_wire(a):
    day = take(a, '--day', int, 129)
    t0 = gps_dow(day) * 86400.0
    ctl_dir, runs = a[0], a[1:]
    ctl = read_wire(os.path.join(ctl_dir, 'products.txt'))
    ec = read_events(ctl_dir)
    h0 = sha16(os.path.join(ctl_dir, 'products.txt'))
    print(f'control {ctl_dir}: products sha256[:16] {h0}, {len(ctl)} epochs, '
          f'{sum(ec.values()) if ec else 0} event rows')
    print('run | products | first wire diff | in one wire only C/B/P | between-satellite move, hours 2-6 (mm): '
          'C p50/p95/max | B p50/p95/max | P p50/p95/max | event rows in one run only | first event diff')
    for rd in runs:
        h = sha16(os.path.join(rd, 'products.txt'))
        if h == h0:
            print(f'{os.path.basename(rd)} | IDENTICAL {h} | none | 0/0/0 | - | - | - | - | -')
            continue
        run = read_wire(os.path.join(rd, 'products.txt'))
        first, only, res = wire_move(ctl, run, t0)
        n_ev, first_ev = event_move(ec, read_events(rd))

        def q(v):
            return (f'{pct(v, 0.5) * 1e3:.3f}/{pct(v, 0.95) * 1e3:.3f}/{max(v) * 1e3:.1f}' if v else '-')
        print(f'{os.path.basename(rd)} | differs {h} | {hms(first) if first is not None else "none"} | '
              f'{only["C"]}/{only["B"]}/{only["P"]} | {q(res["C"])} | {q(res["B"])} | {q(res["P"])} | '
              f'{n_ev if n_ev is not None else "-"} | {hms(first_ev) if first_ev is not None else "-"}')


# ----------------------------------------------------------------------------- grade
def site_x(gc, key):
    """{site4: (mean hourly p50 over hours 2-6, hour-1 p50)} for the replays of `key`."""
    out = {}
    for n in gc.NAMES:
        h = gc.hours(f'{key}_{n}')
        if h and not any(math.isnan(v) for v in h):
            out[n] = (st.mean(h[1:]), h[0])
    return out


def cmd_grade(a):
    day = take(a, '--day', int, 129)
    obs = take(a, '--obs-root', str, None)
    piv = take(a, '--piv', str, None)
    jout = take(a, '--json', str, None)
    import gcensus as gc
    gc.configure(day=day, obs_root=obs, hours=6)
    ctl, keys = a[0], a[1:]
    X0 = site_x(gc, ctl)
    XK = {k: site_x(gc, k) for k in keys}
    XP = site_x(gc, piv) if piv else None
    sites = [n for n in gc.NAMES if n in X0 and all(n in XK[k] for k in keys)]
    census = [n for n in sites if n != 'wtzz']
    print(f'DOY {day}: control {ctl}, {len(keys)} neutral keys, {len(census)} census sites'
          + (f', PIV {piv}' if piv else ''))

    out = {'day': day, 'control': ctl, 'keys': keys, 'piv': piv, 'sites': {}, 'per_key': {}}
    print('\nper neutral key: sites worse / better by more than 1 cm (hours 2-6), census paired median, '
          'WTZZ00DEU change, hour-1 paired median')
    alarms = 0
    for k in keys + ([piv] if piv else []):
        X = XP if k == piv else XK[k]
        d = {n: X[n][0] - X0[n][0] for n in census if n in X}
        worse = sorted((n for n in d if d[n] > CLAUSE_M), key=lambda n: -d[n])
        better = sorted((n for n in d if d[n] < -CLAUSE_M), key=lambda n: d[n])
        d1 = [X[n][1] - X0[n][1] for n in census if n in X]
        dw = X['wtzz'][0] - X0['wtzz'][0] if 'wtzz' in X and 'wtzz' in X0 else float('nan')
        if k != piv and worse:
            alarms += 1
        print(f'  {k:16s} worse {len(worse)} '
              + (('(' + ', '.join(f'{n.upper()} {d[n] * 1e3:+.1f}' for n in worse) + ') ') if worse else '')
              + f'better {len(better)} '
              + (('(' + ', '.join(f'{n.upper()} {d[n] * 1e3:+.1f}' for n in better) + ') ') if better else '')
              + f'| median {st.median(d.values()) * 1e3:+.2f} mm | WTZZ {dw * 1e3:+.2f} mm'
              + f' | hour 1 median {st.median(d1) * 1e3:+.1f} mm')
        out['per_key'][k] = {'worse': {n: d[n] for n in worse}, 'better': {n: d[n] for n in better},
                             'median': st.median(d.values()), 'wtzz': dw, 'hour1_median': st.median(d1)}
    print(f'  clause false alarms: {alarms} of {len(keys)} neutral keys have a site worse by more than 1 cm')
    out['false_alarms'] = alarms

    K = len(keys)
    print('\nper site, changes against the control (mm; hours 2-6 mean of the hourly p50): '
          'x_ctl | d per neutral key | m | s | envelope'
          + (' | d_PIV | r | U95 | U99 | reading' if piv else ''))
    for n in sites:
        d = [XK[k][n][0] - X0[n][0] for k in keys]
        m = st.mean(d)
        s = st.stdev(d) if K > 1 else float('nan')
        env = max(abs(x) for x in d)
        row = {'x_ctl': X0[n][0], 'd': d, 'm': m, 's': s, 'envelope': env}
        line = (f'  {n.upper():5s} {X0[n][0] * 1e3:7.1f} | ' + ' '.join(f'{x * 1e3:+6.1f}' for x in d)
                + f' | {m * 1e3:+6.1f} | {s * 1e3:5.1f} | {env * 1e3:5.1f}')
        if piv and n in XP:
            dp = XP[n][0] - X0[n][0]
            r = sum(1 for x in d if x >= dp)
            f = math.sqrt(1.0 + 1.0 / K)
            u95 = m + T95[K - 1] * s * f
            u99 = m + T99[K - 1] * s * f
            if r >= 1 or dp <= u95:
                reading = 'inside'
            elif dp > u99:
                reading = 'outside'
            else:
                reading = 'undecided'
            row.update({'d_piv': dp, 'r': r, 'u95': u95, 'u99': u99, 'reading': reading})
            line += f' | {dp * 1e3:+6.1f} | {r} | {u95 * 1e3:+6.1f} | {u99 * 1e3:+6.1f} | {reading}'
        out['sites'][n] = row
        print(line)
    if jout:
        json.dump(out, open(jout, 'w'), indent=2)
        print(f'\nwrote {jout}')


if __name__ == '__main__':
    args = sys.argv[1:]
    if not args or args[0] not in ('wire', 'grade'):
        print(__doc__)
        sys.exit(2)
    {'wire': cmd_wire, 'grade': cmd_grade}[args[0]](args[1:])

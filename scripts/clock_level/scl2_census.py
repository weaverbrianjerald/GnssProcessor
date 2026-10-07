#!/usr/bin/env python3
"""Card SCL2: the census statistics of gates G4 (field) and G5 (sim), as card SCL section 9 read them.

    python scripts/clock_level/scl2_census.py field <ref key> <key> [<key> ...]
    python scripts/clock_level/scl2_census.py sim   <ref key> <key> [<key> ...] [--t0 run|day] [--h 2,3,4,5]

field  census6h.py's replays (tags msc6_<site>_<key>): per site, the change of the mean hourly p50 over hours 2 to 6
       (census6h.hours, indices 1 to 5) against the reference key; the median and the sites worse / better by more
       than 0.02 m, over the 16 census sites without WTZZ00DEU (card SCL's set) and over 17 with it.
sim    simcensus6h.py's replays (tags smc6_<station>_<key>): per station, the change of the mean hourly p50 over the
       chosen hours.  The replays start at 01:01:30 and run past the wire's end, so the hours are counted from each
       replay's first epoch (--t0 run, the default; card SCL section 9 read hours 2 to 5 this way); --t0 day bins from
       00:00 and does not reproduce card SCL's figures.
Card SCL's B1-alone figures reproduce with this script (card SCL2 section 10): field s16on against s16c +0.0034 m,
5 worse, 4 better; sim s16ons against s16cs +0.0049 m, 2 worse.
"""
import csv, importlib.util, os, statistics as st, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'
SIM_STATIONS = [f'S{i:02d}' for i in list(range(1, 58, 4)) + [60]]


def field(ref, keys):
    spec = importlib.util.spec_from_file_location('c6', ROOT + '/scripts/clock_level/census6h.py')
    argv = sys.argv; sys.argv = ['x']
    c6 = importlib.util.module_from_spec(spec); spec.loader.exec_module(c6)
    sys.argv = argv
    sites = [s['name'][:4].lower() for s in c6.SL]
    m25 = lambda v: (lambda w: sum(w) / len(w) if w else None)([x for x in v[1:] if x == x])
    R = {n: c6.hours(f'msc6_{n}_{ref}') for n in sites + ['wtzz']}
    for key in keys:
        d = {}
        for n in sites + ['wtzz']:
            K = c6.hours(f'msc6_{n}_{key}')
            if R[n] and K and m25(R[n]) is not None and m25(K) is not None:
                d[n] = m25(K) - m25(R[n])
        for lab, ns in (('16 sites', sites), ('17 with WTZZ', sites + ['wtzz'])):
            ch = [d[n] for n in ns if n in d]
            if not ch:
                print(f'{key} vs {ref} [{lab}]: no paired sites'); continue
            w = [n for n in ns if n in d and d[n] > 0.02]
            b = [n for n in ns if n in d and d[n] < -0.02]
            print(f'{key} vs {ref} [{lab}]: n {len(ch)}, median {st.median(ch):+.4f} m, '
                  f'worse {len(w)} {w}, better {len(b)} {b}')
        print('   per site: ' + ', '.join(f'{n} {x:+.3f}' for n, x in sorted(d.items(), key=lambda t: t[1])))


def sim_hours(tag, t0mode):
    p = f'{RUNS}/_rap_{tag}/logs/closed_loop_trace.csv'
    if not os.path.exists(p):
        return None
    rows = []
    for r in csv.DictReader(open(p)):
        try:
            rows.append((float(r['tow']), float(r['pos_err_3d_m'])))
        except (ValueError, TypeError, KeyError):
            pass
    if not rows:
        return None
    T0 = 259200.0 if t0mode == 'day' else rows[0][0]
    by = {}
    for t, e in rows:
        by.setdefault(int(round((t - T0) * 1000)) // 3600000 + 1, []).append(e)
    return {h: st.median(v) for h, v in by.items()}


def sim(ref, keys, t0mode, H):
    R = {s: sim_hours(f'smc6_{s.lower()}_{ref}', t0mode) for s in SIM_STATIONS}
    for key in keys:
        K_all = {s: sim_hours(f'smc6_{s.lower()}_{key}', t0mode) for s in SIM_STATIONS}
        d = []
        for s in SIM_STATIONS:
            r, k = R[s], K_all[s]
            if not r or not k:
                continue
            rv = [r[h] for h in H if h in r]; kv = [k[h] for h in H if h in k]
            if rv and kv:
                d.append((s, sum(kv) / len(kv) - sum(rv) / len(rv)))
        if not d:
            print(f'{key} vs {ref}: no paired stations'); continue
        ch = [x for _, x in d]
        worse = [n for n, x in d if x > 0.02]; better = [n for n, x in d if x < -0.02]
        med = st.median(ch)
        print(f'{key} vs {ref} (t0 {t0mode}, hours {H}): n {len(d)}, median {med:+.4f} m, worse {len(worse)} {worse}, '
              f'better {len(better)} {better}; G5 bar {"PASS" if med <= 0.010 and len(worse) <= 3 else "FAIL"}')
        print('   per station: ' + ', '.join(f'{n} {x:+.3f}' for n, x in sorted(d, key=lambda t: t[1])))


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) < 3 or a[0] not in ('field', 'sim'):
        raise SystemExit(__doc__)
    if a[0] == 'field':
        field(a[1], a[2:])
    else:
        t0, H = 'run', [2, 3, 4, 5]
        if '--t0' in a:
            i = a.index('--t0'); t0 = a[i + 1]; del a[i:i + 2]
        if '--h' in a:
            i = a.index('--h'); H = [int(x) for x in a[i + 1].split(',')]; del a[i:i + 2]
        sim(a[1], a[2:], t0, H)

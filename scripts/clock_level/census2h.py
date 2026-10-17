#!/usr/bin/env python3
"""The 16-site census on a 2 h wire, graded on hours 1 and 2 against a reference key.

make : python scripts/clock_level/census2h.py make <key> <wire run dir, bare> <exe> [key.path=value ...]
       (extra settings go to every replay arm, e.g. a consumer knob)
       Consumer-only replays of the wire (site_list.json sites plus WTZZ), tags
       msc_<site>_<key>_d, 2 h, base arm runs/_rf0hv10_field6/arm.json.
grade: python scripts/clock_level/census2h.py grade <refkey> <key> [<key> ...]
       Per site the hour-1 and hour-2 p50 of each key against the reference key's same hours
       (the census replays are causal, so a 6 h reference's first two hours are comparable),
       split by the site's latched band-1 code (site_list latch_c1).
"""
import csv, json, os, statistics as st, subprocess, sys

import msc_sites

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'; T0 = 259200.0
SL = msc_sites.load()


def make(key, wire, exe, extra=()):
    tags = []
    def one(tag, site):
        cmd = [sys.executable, ROOT + '/scripts/rapid_arm.py', 'make', '--base',
               f'{RUNS}/_rf0hv10_field6/arm.json', '--tag', tag, '--hours', '2',
               '--set', f'network.closed_loop.replay_products_path={RUNS}/{wire}/products.txt']
        for kv in extra:
            cmd += ['--set', kv]
        if site is not None:
            cmd += ['--set', f"network.closed_loop.station.name={site['name']}",
                    '--set', f"network.closed_loop.station.rinex_path={site['path']}",
                    '--set', 'network.closed_loop.station.position_ecef=' + json.dumps(site['ecef'])]
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, (tag, r.stdout[-400:], r.stderr[-400:])
        tags.append(tag)
    one(f'msc_wtzz_{key}_d', None)
    for s in SL:
        one(f"msc_{s['name'][:4].lower()}_{key}_d", s)
    r = subprocess.run([sys.executable, ROOT + '/scripts/rapid_arm.py', 'run', '--tags', *tags,
                        '--jobs', '4', '--exe', exe], capture_output=True, text=True, cwd=ROOT)
    print(key, len(tags), 'replays;', r.stdout.count('exit 0'), 'exit 0')


def hours(tag):
    p = f'{RUNS}/_rap_{tag}/logs/closed_loop_trace.csv'
    if not os.path.exists(p):
        return None
    by = {0: [], 1: []}
    for r in csv.DictReader(open(p)):
        try:
            t = float(r['tow']); e = float(r['pos_err_3d_m'])
        except (ValueError, TypeError):
            continue
        h = int(round((t - T0) * 1000)) // 3600000
        if h in by:
            by[h].append(e)
    if not by[0] or not by[1]:
        return None
    return [st.median(by[0]), st.median(by[1])]


def grade(ref, keys):
    names = [(s['name'][:4].lower(), s.get('latch_c1', '?')) for s in SL]
    R = {n: hours(f'msc_{n}_{ref}_d') for n, _ in names}
    print(f'reference {ref}: median hour 1 / 2 p50 '
          f"{st.median(v[0] for v in R.values() if v):.3f} / {st.median(v[1] for v in R.values() if v):.3f}")
    for key in keys:
        K = {n: hours(f'msc_{n}_{key}_d') for n, _ in names}
        ks = [n for n, _ in names if R[n] and K[n]]
        print(f'{key}: n={len(ks)} median hour 1 / 2 p50 {st.median(K[n][0] for n in ks):.3f} / '
              f'{st.median(K[n][1] for n in ks):.3f}')
        for fam in ('C1W', 'C1C'):
            fs = [n for n, lc in names if lc == fam and n in ks]
            if not fs:
                continue
            d1 = [K[n][0] - R[n][0] for n in fs]; d2 = [K[n][1] - R[n][1] for n in fs]
            print(f'   {fam} sites ({len(fs)}): hour-1 change median {st.median(d1):+.3f} '
                  f'(better {sum(x < -0.05 for x in d1)}, worse {sum(x > 0.05 for x in d1)}); '
                  f'hour-2 change median {st.median(d2):+.3f} '
                  f'(better {sum(x < -0.01 for x in d2)}, worse {sum(x > 0.01 for x in d2)})')
        print('   per site hour 2 (ref > key): ' + ' '.join(
            f'{n.upper()}[{dict(names)[n][1:]}] {R[n][1]:.3f}>{K[n][1]:.3f}' for n in ks))


if __name__ == '__main__':
    if sys.argv[1] == 'make':
        make(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:])
    else:
        grade(sys.argv[2], sys.argv[3:])

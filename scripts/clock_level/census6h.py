#!/usr/bin/env python3
"""The 16-site census on a 6 h wire, graded per hour (1..6).

make : python scripts/clock_level/census6h.py make <key> <wire run dir, bare> <exe> [key.path=value ...]
       Consumer-only replays (runs/_msc/site_list.json sites plus WTZZ), tags msc6_<site>_<key>,
       6 h, base arm runs/_rf0hv10_field6/arm.json; PBF (closed_loop.pb_hold_presence_flip) is ON
       by default, as on the goldens.
grade: python scripts/clock_level/census6h.py grade <refkey> <key> [<key> ...]
       Per hour: the median over sites of the site p50, and the paired change against the reference
       (median, better / worse at 1 cm), split by the site's latched band-1 code.
"""
import csv, json, os, statistics as st, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'; T0 = 259200.0; NH = 6
SL = json.load(open(RUNS + '/_msc/site_list.json'))['sites']
if os.environ.get('CENSUS_SITES'):
    SL = [s for s in SL if s['name'][:4].lower() in os.environ['CENSUS_SITES'].split(',')]
NO_WTZZ = bool(os.environ.get('CENSUS_SITES'))


def make(key, wire, exe, extra=()):
    tags = []
    def one(tag, site):
        cmd = [sys.executable, ROOT + '/scripts/rapid_arm.py', 'make', '--base',
               f'{RUNS}/_rf0hv10_field6/arm.json', '--tag', tag, '--hours', str(NH),
               '--set', f'network.closed_loop.replay_products_path={RUNS}/{wire}/products.txt',
               '--set', 'network.closed_loop.pb_hold_presence_flip=true']
        for kv in extra:
            cmd += ['--set', kv]
        if site is not None:
            cmd += ['--set', f"network.closed_loop.station.name={site['name']}",
                    '--set', f"network.closed_loop.station.rinex_path={site['path']}",
                    '--set', 'network.closed_loop.station.position_ecef=' + json.dumps(site['ecef'])]
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, (tag, r.stdout[-400:], r.stderr[-400:])
        tags.append(tag)
    if not NO_WTZZ:
        one(f'msc6_wtzz_{key}', None)
    for s in SL:
        one(f"msc6_{s['name'][:4].lower()}_{key}", s)
    r = subprocess.run([sys.executable, ROOT + '/scripts/rapid_arm.py', 'run', '--tags', *tags,
                        '--jobs', '6', '--exe', exe], capture_output=True, text=True, cwd=ROOT)
    print(key, len(tags), 'replays;', r.stdout.count('exit 0'), 'exit 0')


def hours(tag):
    p = f'{RUNS}/_rap_{tag}/logs/closed_loop_trace.csv'
    if not os.path.exists(p):
        return None
    by = {h: [] for h in range(NH)}
    for r in csv.DictReader(open(p)):
        try:
            t = float(r['tow']); e = float(r['pos_err_3d_m'])
        except (ValueError, TypeError):
            continue
        h = int(round((t - T0) * 1000)) // 3600000
        if h in by:
            by[h].append(e)
    return [st.median(by[h]) if by[h] else float('nan') for h in range(NH)]


def grade(ref, keys):
    names = [(s['name'][:4].lower(), s.get('latch_c1', '?')) for s in SL]
    R = {n: hours(f'msc6_{n}_{ref}') for n, _ in names}
    def med(K, h, ns):
        v = [K[n][h] for n in ns if K[n] and K[n][h] == K[n][h]]
        return st.median(v) if v else float('nan')
    ns_all = [n for n, _ in names if R[n]]
    print(f'{ref:10s} ' + ' '.join(f'{med(R, h, ns_all):.3f}' for h in range(NH)))
    for key in keys:
        K = {n: hours(f'msc6_{n}_{key}') for n, _ in names}
        ks = [n for n, _ in names if R[n] and K[n]]
        print(f'{key:10s} ' + ' '.join(f'{med(K, h, ks):.3f}' for h in range(NH)) + f'   (n={len(ks)})')
        for fam in ('C1W', 'C1C'):
            fs = [n for n, lc in names if lc == fam and n in ks]
            row = []
            for h in range(NH):
                d = [K[n][h] - R[n][h] for n in fs]
                row.append(f'{st.median(d):+.3f}({sum(x < -0.01 for x in d)}/{sum(x > 0.01 for x in d)})')
            print(f'   {fam} ({len(fs)}) change, better/worse: ' + ' '.join(row))
        worst = sorted(ks, key=lambda n: -max(K[n][h] - R[n][h] for h in range(1, NH)))[:5]
        print('   largest hour 2-6 regressions: ' + '; '.join(
            f"{n.upper()} " + ' '.join(f'{R[n][h]:.2f}>{K[n][h]:.2f}' for h in range(1, NH)) for n in worst))


if __name__ == '__main__':
    if sys.argv[1] == 'make':
        make(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:])
    else:
        grade(sys.argv[2], sys.argv[3:])

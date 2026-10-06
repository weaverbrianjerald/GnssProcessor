"""The 16-site consumer census per day of the multi-day replay (a day-aware census6h.py).

make  : python scripts/clock_level/mday_census.py make <doy> <wire run dir name> <exe> <obs root of the day's census sites>
        Consumer-only replays of the day's wire: the 16 held-out sites of runs/_msc/site_list.json (their
        RINEX for the day under the given root) plus the wire's own WTZZ consumer, 6 h, base arm = the
        wire's own arm.json (so every knob, the accumulating wind-up and PBF included, is the wire's).
grade : python scripts/clock_level/mday_census.py grade <doy> [<doy> ...]
        Per day and hour: the median over sites of the site p50 of the 3-D consumer error.
--prefix P (either verb): tags P_<doy>_<site> instead of mdc_<doy>_<site>, for a second wire of one day.
"""
import csv, json, os, statistics as st, subprocess, sys

ROOT = 'C:/GnssProcessor'
RUNS = ROOT + '/runs'
NH = 6
SL = json.load(open(RUNS + '/_msc/site_list.json'))['sites']
DOW = {128: 2, 129: 3, 130: 4, 131: 5, 132: 6}   # GPS week 2313 day of week


PREFIX = 'mdc'   # --prefix P: a second census of one day (another wire) under its own tags


def tag(doy, site):
    return f'{PREFIX}_{doy}_{site}'


def make(doy, wire, exe, obs_root):
    base = f'{RUNS}/{wire}/arm.json'
    tags = []

    def one(site_tag, site):
        cmd = [sys.executable, ROOT + '/scripts/rapid_arm.py', 'make', '--base', base,
               '--tag', tag(doy, site_tag), '--hours', str(NH),
               '--set', f'network.closed_loop.replay_products_path={RUNS}/{wire}/products.txt']
        if site is not None:
            d = site['name'].lower()
            path = f"{obs_root}/2024-{doy:03d}/obs/{d}/{site['name']}_R_2024{doy:03d}0000_01D_30S_MO.rnx"
            if not os.path.exists(path):
                print('missing', path)
                return
            cmd += ['--set', f"network.closed_loop.station.name={site['name']}",
                    '--set', f'network.closed_loop.station.rinex_path={path}',
                    '--set', 'network.closed_loop.station.position_ecef=' + json.dumps(site['ecef'])]
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, (site_tag, r.stdout[-400:], r.stderr[-400:])
        tags.append(tag(doy, site_tag))

    one('wtzz', None)
    for s in SL:
        one(s['name'][:4].lower(), s)
    r = subprocess.run([sys.executable, ROOT + '/scripts/rapid_arm.py', 'run', '--tags', *tags,
                        '--jobs', '6', '--exe', exe], capture_output=True, text=True, cwd=ROOT)
    print(doy, len(tags), 'replays;', r.stdout.count('exit 0'), 'exit 0')


def hours(doy, site_tag):
    p = f'{RUNS}/_rap_{tag(doy, site_tag)}/logs/closed_loop_trace.csv'
    if not os.path.exists(p):
        return None
    t0 = DOW[doy] * 86400.0
    by = {h: [] for h in range(NH)}
    for r in csv.DictReader(open(p)):
        try:
            t = float(r['tow']); e = float(r['pos_err_3d_m'])
        except (ValueError, TypeError):
            continue
        h = int(round((t - t0) * 1000)) // 3600000
        if h in by:
            by[h].append(e)
    return [st.median(by[h]) if by[h] else float('nan') for h in range(NH)]


def grade(doys):
    names = [s['name'][:4].lower() for s in SL]
    print('day  sites  WTZZ p50 by hour                         census median of site p50 by hour')
    for doy in doys:
        w = hours(doy, 'wtzz')
        K = {n: hours(doy, n) for n in names}
        ks = [n for n in names if K[n]]
        med = [st.median([K[n][h] for n in ks if K[n][h] == K[n][h]]) for h in range(NH)] if ks else []
        print(f'{doy}  {len(ks):5d}  ' + (' '.join(f'{x:.3f}' for x in w) if w else '-') + '   '
              + ' '.join(f'{x:.3f}' for x in med))
        if ks:
            worst = sorted(ks, key=lambda n: -max(K[n][1:]))[:4]
            print('      worst sites (max p50 hours 2-6): ' + '; '.join(
                f'{n.upper()} {max(K[n][1:]):.2f}' for n in worst))


if __name__ == '__main__':
    argv = sys.argv[1:]
    if '--prefix' in argv:
        i = argv.index('--prefix')
        PREFIX = argv[i + 1]
        del argv[i:i + 2]
    if argv[0] == 'make':
        make(int(argv[1]), argv[2], argv[3], argv[4])
    else:
        grade([int(x) for x in argv[1:]])

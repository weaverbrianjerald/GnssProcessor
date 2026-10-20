#!/usr/bin/env python3
"""The 16-site census for cards GAL2 and L5R (2026-10-08): consumer-only replays of a 6 h field wire.

make  : python gcensus.py make <key> <consumer base run> <wire run> <exe> [key.path=value ...] [--rinex OP ...]
        Replays at WTZZ00DEU (the base's own consumer) and the 16 sites of site_list.json,
        6 h, built with rapid_arm.py make from <consumer base run>/arm.json (so every consumer knob is
        that run's: the wire's own arm for a consumer of the wire's posture, the dual control's arm for
        a dual GPS consumer of another wire), reading <wire run>/products.txt.  Tags <key>_<site4>.
        Each --rinex OP goes to rapid_arm.py make with the census site already installed as the closed-loop
        consumer, so an op with consumer=1 edits the site's own RINEX copy; `{SITE}` in an op is replaced by the
        site's name, so stations={SITE};consumer=1 edits the consumer's file alone (card L5B).
grade : python gcensus.py grade <refkey> <key> [<key> ...]
sites : `--sites FILE` anywhere on the line (or MSC_SITE_LIST in the environment) reads the site list from FILE
        instead of site_list.json, for example site_list_sinex.json.  A `consumer` entry of that list replaces the
        base's own WTZZ00DEU coordinate, so every consumer of a key is scored against the same coordinate source.
        Default unchanged.
        Per key: the median over sites of the site's hourly p50 (3D, hours from 00:00), and against
        <refkey> the paired change of each site's mean hourly p50 over hours 2 to 6 and of hour 1,
        better / worse at 1 cm, the median change; then the per-site rows.

Day options (either verb; card MDAY, 2026-10-19).  With none of them the tool is the DOY-129, 00:00, 6 h census of
the cards above, command for command and line for line.
  --day D          day of year 2024 of the sites' RINEX and of the hour clock (default 129).  The site files
                   are <obs root>/2024-<D>/obs/<site>/<SITE>_R_2024<D>0000_01D_30S_MO.rnx; the consumer at the
                   base's own station keeps the path of the base arm.
  --obs-root R     the root that holds the day's obs/ directories (default C:/GnssProcessor/data; the days
                   128 and 130 to 132 are under D:/GnssProcessorSupport/data_mday).
  --start-hour H   the replays start H hours into the day (network.start_offset_hours) and the hours are
                   counted from there (default 0).
  --hours N        replay length and number of graded hours (default 6).  Hours 2 to N are the settled ones.
  --duration-s S   replay length in seconds in place of --hours N for the replays (a day whose products end
                   short of N hours); N still sets the number of graded hours.
  --jobs J         replays run at once (default 6).
"""
import csv, json, os, statistics as st, subprocess, sys

import msc_sites

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace('\\', '/')   # this checkout
RUNS = os.environ.get('RAPID_RUNS_ROOT', ROOT + '/runs').replace('\\', '/')   # the runs root rapid_arm.py uses
T0, NH = 259200.0, 6
DAY, START_H, OBS_ROOT, DURATION_S, JOBS = 129, 0.0, None, None, 6   # the day options (configure)
SL = msc_sites.load()
NAMES = ['wtzz'] + [s['name'][:4].lower() for s in SL]
CONSUMER = msc_sites.load_consumer()


def use_site_list(path):
    """Read the sites (and the consumer coordinate, when the list has one) from `path`."""
    global SL, NAMES, CONSUMER
    msc_sites.PATH = path.replace('\\', '/')
    SL = msc_sites.load()
    NAMES = ['wtzz'] + [s['name'][:4].lower() for s in SL]
    CONSUMER = msc_sites.load_consumer()


def gps_day_of_week(doy, year=2024):
    """GPS day of week (Sunday 0) of day-of-year doy of the year (DOY 129 of 2024 is a Wednesday, 3)."""
    import datetime
    return (datetime.date(year, 1, 1) + datetime.timedelta(days=doy - 1)).isoweekday() % 7


def configure(day=129, start=0.0, hours=6, obs_root=None, duration_s=None, jobs=6):
    """Set the day options; the defaults are the DOY-129, 00:00, 6 h census.  Also called by graders that import
    this module (hours() and grade() read the module globals T0 and NH)."""
    global T0, NH, DAY, START_H, OBS_ROOT, DURATION_S, JOBS
    DAY, START_H, OBS_ROOT, DURATION_S, JOBS = day, float(start), obs_root, duration_s, jobs
    NH = int(hours) if float(hours).is_integer() else float(hours)
    T0 = gps_day_of_week(day) * 86400.0 + START_H * 3600.0


def site_path(s):
    """The RINEX of census site s on the configured day (the site list's own path for DOY 129 under its root)."""
    if DAY == 129 and OBS_ROOT is None:
        return s['path']
    root = (OBS_ROOT or ROOT + '/data').replace('\\', '/').rstrip('/')
    return f"{root}/2024-{DAY:03d}/obs/{s['name'].lower()}/{s['name']}_R_2024{DAY:03d}0000_01D_30S_MO.rnx"


def make(key, base, wire, exe, extra):
    rinex = []
    while '--rinex' in extra:
        i = extra.index('--rinex')
        rinex.append(extra[i + 1])
        extra = extra[:i] + extra[i + 2:]
    tags = []
    for s4 in NAMES:
        tag = f'{key}_{s4}'
        base_json = f'{RUNS}/{base}/arm.json'
        site = []
        if s4 != 'wtzz':
            s = next(x for x in SL if x['name'][:4].lower() == s4)
            site = [('network.closed_loop.station.name', s['name']),
                    ('network.closed_loop.station.rinex_path', site_path(s)),
                    ('network.closed_loop.station.position_ecef', s['ecef'])]
        elif CONSUMER:
            site = [('network.closed_loop.station.position_ecef', CONSUMER['ecef'])]
        if rinex and site:
            # rapid_arm.py applies RINEX ops before --set, to the base's own consumer: write a per-site base
            # whose closed-loop consumer already is the census site, so a consumer=1 op edits the site's file.
            cfg = json.load(open(base_json))
            for k, v in site:
                node = cfg
                for part in k.split('.')[:-1]:
                    node = node[part]
                node[k.split('.')[-1]] = v
            os.makedirs(f'{RUNS}/_gcensus_bases', exist_ok=True)
            base_json = f'{RUNS}/_gcensus_bases/{tag}.json'
            json.dump(cfg, open(base_json, 'w'), indent=2)
            site = []
        cmd = [sys.executable, '-I', ROOT + '/scripts/rapid_arm.py', 'make', '--base', base_json,
               '--tag', tag] + (['--hours', str(NH)] if DURATION_S is None else []) + [
               '--set', f'network.closed_loop.replay_products_path={RUNS}/{wire}/products.txt']
        if DURATION_S is not None:   # exact seconds: rapid_arm's --hours is hours * 3600 in floating point
            cmd += ['--set', f'processing.max_duration_s={DURATION_S}', '--set', f'network.max_duration_s={DURATION_S}']
        if START_H:
            cmd += ['--set', f'network.start_offset_hours={START_H}']
        for kv in extra:
            cmd += ['--set', kv]
        site_name = s['name'] if s4 != 'wtzz' else json.load(open(f'{RUNS}/{base}/arm.json'))['network']['closed_loop']['station']['name']
        for op in rinex:
            cmd += ['--rinex', op.replace('{SITE}', site_name)]
        for k, v in site:
            cmd += ['--set', f'{k}=' + (json.dumps(v) if isinstance(v, list) else v)]
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
        assert r.returncode == 0, (tag, r.stdout[-400:], r.stderr[-400:])
        tags.append(tag)
    r = subprocess.run([sys.executable, '-I', ROOT + '/scripts/rapid_arm.py', 'run', '--tags', *tags,
                        '--jobs', str(JOBS), '--exe', exe], capture_output=True, text=True, cwd=ROOT)
    print(key, len(tags), 'replays;', r.stdout.count('exit 0'), 'exit 0')
    if r.stdout.count('exit 0') != len(tags):
        print(r.stdout)


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


def summary(d, label):
    b = sum(1 for x in d if x < -0.01); w = sum(1 for x in d if x > 0.01)
    return f'{label}: median {st.median(d):+.4f} m, better {b}, worse {w}, n {len(d)}'


def grade(ref, keys):
    R = {n: hours(f'{ref}_{n}') for n in NAMES}
    for key in [ref] + keys:
        K = {n: hours(f'{key}_{n}') for n in NAMES}
        sites = [n for n in NAMES[1:] if K[n]]
        med = [st.median([K[n][h] for n in sites]) for h in range(NH)]
        print(f'== {key}: {len(sites)} sites; median site p50 by hour ' + ' '.join(f'{x:.3f}' for x in med)
              + ('; WTZZ ' + ' '.join(f'{x:.3f}' for x in K['wtzz']) if K['wtzz'] else ''))
        if key == ref:
            continue
        pair = [n for n in sites if R[n]]
        d26 = [st.mean(K[n][1:]) - st.mean(R[n][1:]) for n in pair]
        d1 = [K[n][0] - R[n][0] for n in pair]
        print('   ' + summary(d26, 'hours 2-6 paired') + ' | ' + summary(d1, 'hour 1 paired'))
        for n in pair:
            print(f'   {n.upper():5s} ref ' + ' '.join(f'{x:.3f}' for x in R[n]) + '  arm '
                  + ' '.join(f'{x:.3f}' for x in K[n]) + f'  d2-6 {st.mean(K[n][1:]) - st.mean(R[n][1:]):+.4f}')


def take_options(a):
    """Strip the day options from the argument list, configure(), and return the rest."""
    opt = {'--day': ('day', int), '--obs-root': ('obs_root', str), '--start-hour': ('start', float),
           '--hours': ('hours', float), '--duration-s': ('duration_s', float), '--jobs': ('jobs', int)}
    kw = {}
    while True:
        hit = [i for i, x in enumerate(a) if x in opt]
        if not hit:
            break
        i = hit[0]
        name, conv = opt[a[i]]
        kw[name] = conv(a[i + 1])
        a = a[:i] + a[i + 2:]
    if kw:
        configure(**kw)
    return a


if __name__ == '__main__':
    a = sys.argv[1:]
    if '--sites' in a:
        i = a.index('--sites')
        use_site_list(a[i + 1])
        a = a[:i] + a[i + 2:]
    a = take_options(a)
    if a[0] == 'make':
        make(a[1], a[2], a[3], a[4], a[5:])
    else:
        grade(a[1], a[2:])

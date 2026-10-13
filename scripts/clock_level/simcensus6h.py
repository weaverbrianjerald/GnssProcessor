#!/usr/bin/env python3
"""A 16-station consumer census on a 6 h SIM wire, graded per hour (1..6).

The sim bed has one held-out consumer (SZZ000SIM); the other 60 stations are in the network,
so these replays are IN-NETWORK consumers (optimistic in absolute terms; the paired change
between two wires of one solve is what is read).  Stations S01, S05, ..., S57 and S60.

make : python scripts/clock_level/simcensus6h.py make <key> <wire run dir, bare> <exe>
       tags smc6_<station>_<key>, base runs/_rf0hv13_sim6/arm.json (the sim golden's consumer on
       the re-based fixture, PBF on; $SIMCENSUS_BASE overrides it), 6 h.  Since card GAD (2026-10-13)
       the sim golden is GPS + Galileo while this default base is GPS only: set SIMCENSUS_BASE to the
       wire's own arm (for example runs/_rf0hv15_sim6/arm.json) to census the golden's consumers.
grade: python scripts/clock_level/simcensus6h.py grade <refkey> <key> [<key> ...]

The replay's start: a replay inherits the base arm's network.start_offset_hours (0.504167 on the sim golden) and the
replay driver applies it from the wire's first epoch, so by default every replay starts at 01:01:30, an extra half hour
after a wire that itself starts at 00:30:15 (card SCL2B section 9).  SIMCENSUS_FROM_START=1 sets the replay's offset to 0,
so the replays start with the wire (byte-identical to the producer's in-loop consumer on the same station).  Keys made
the two ways are different statistics: compare only keys made the same way.
"""
import csv, json, os, statistics as st, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'; T0 = 259200.0; NH = 6
# The consumers' base: the sim golden's baseline on the re-based fixture (card SFR, 2026-10-01).
# Keys made before card SFR used runs/_rf0hv11_sim6/arm.json (the superseded fixture); set
# SIMCENSUS_BASE to re-make or re-read one of them.
BASE = os.environ.get('SIMCENSUS_BASE', f'{RUNS}/_rf0hv13_sim6/arm.json')
_ST = json.load(open(BASE))['network']['stations']
PICK = [f'S{i:02d}000SIM' for i in list(range(1, 58, 4)) + [60]]
SL = [s for s in _ST if s['name'] in PICK]
assert len(SL) == 16, len(SL)


def make(key, wire, exe):
    tags = []
    for s in SL:
        tag = f"smc6_{s['name'][:3].lower()}_{key}"
        cmd = [sys.executable, ROOT + '/scripts/rapid_arm.py', 'make', '--base', BASE, '--tag', tag,
               '--hours', str(NH),
               '--set', f'network.closed_loop.replay_products_path={RUNS}/{wire}/products.txt',
               '--set', 'network.closed_loop.pb_hold_presence_flip=true',
               '--set', f"network.closed_loop.station.name={s['name']}",
               '--set', f"network.closed_loop.station.rinex_path={s['rinex_path']}",
               '--set', 'network.closed_loop.station.position_ecef=' + json.dumps(s['position_ecef'])]
        if os.environ.get('SIMCENSUS_FROM_START') == '1':
            cmd += ['--set', 'network.start_offset_hours=0']
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, (tag, r.stdout[-400:], r.stderr[-400:])
        tags.append(tag)
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
    names = [s['name'][:3].lower() for s in SL]
    R = {n: hours(f'smc6_{n}_{ref}') for n in names}
    ns = [n for n in names if R[n]]
    med = lambda K, h, ks: st.median([K[n][h] for n in ks if K[n][h] == K[n][h]])
    print(f'{ref:10s} ' + ' '.join(f'{med(R, h, ns):.3f}' for h in range(NH)) + f'   (n={len(ns)})')
    for key in keys:
        K = {n: hours(f'smc6_{n}_{key}') for n in names}
        ks = [n for n in names if R[n] and K[n]]
        print(f'{key:10s} ' + ' '.join(f'{med(K, h, ks):.3f}' for h in range(NH)) + f'   (n={len(ks)})')
        row = []
        for h in range(NH):
            d = [K[n][h] - R[n][h] for n in ks]
            row.append(f'{st.median(d):+.3f}({sum(x < -0.01 for x in d)}/{sum(x > 0.01 for x in d)})')
        print('   change, better/worse at 1 cm: ' + ' '.join(row))


if __name__ == '__main__':
    if sys.argv[1] == 'make':
        make(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        grade(sys.argv[2], sys.argv[3:])

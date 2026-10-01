"""Card WUP: the gates' reads (G1 to G3).

census: python scripts/clock_level/wup_grade.py census <field|sim> <refkey> <key> [<key> ...]
        Card WUP's frozen statistic: per site, the change of the mean hourly p50 (hours 2 to 6) of
        the replayed consumer against the reference key; the median change, the sites worse and
        better by more than 0.02 m, and the bar (median at most +0.005 m, at most 3 sites worse).
        Field keys are census6h.py's (msc6_<site>_<key>), sim keys simcensus6h.py's (smc6_...).
wire  : python scripts/clock_level/wup_grade.py wire <run> [<run> ...]
        Per run: code-bias records (`B,` lines of products.txt) per epoch, the wind-up census and
        restart-response run-end lines, the IF-TD pair line, and the restart events by action.
"""
import collections, csv, importlib.util, os, re, statistics as st, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE)).replace(os.sep, '/')


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + '.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def census(bed, ref, keys):
    if bed == 'field':
        c6 = _load('census6h')
        sites = [s['name'][:4].lower() for s in c6.SL] + ['wtzz']
        tag = lambda n, k: f'msc6_{n}_{k}'
        hours = c6.hours
    else:
        c6 = _load('simcensus6h')
        sites = [s['name'][:3].lower() for s in c6.SL]
        tag = lambda n, k: f'smc6_{n}_{k}'
        hours = _load('census6h').hours
    R = {n: hours(tag(n, ref)) for n in sites}
    for key in keys:
        d = []
        for n in sites:
            K = hours(tag(n, key))
            if not R[n] or not K:
                continue
            r = [x for x in R[n][1:] if x == x]
            k = [x for x in K[1:] if x == x]
            if not r or not k:
                continue
            d.append((n, sum(k) / len(k) - sum(r) / len(r)))
        if not d:
            print(f'{key}: no paired sites')
            continue
        ch = [x for _, x in d]
        worse = [n for n, x in d if x > 0.02]
        better = [n for n, x in d if x < -0.02]
        med = st.median(ch)
        ok = med <= 0.005 and len(worse) <= 3
        print(f'{bed} {key} vs {ref}: sites {len(d)}, median change {med:+.4f} m, worse {len(worse)} '
              f'{worse}, better {len(better)} {better}; bar {"PASS" if ok else "FAIL"}')
        print('   per site: ' + ', '.join(f'{n} {x:+.3f}' for n, x in sorted(d, key=lambda t: t[1])))


def wire(runs):
    for run in runs:
        d = run if os.path.isdir(run) else ROOT + '/runs/' + run
        nb, ne = 0, 0
        with open(d + '/products.txt', errors='replace') as f:
            for ln in f:
                if ln.startswith('B,'):
                    nb += 1
                elif ln.startswith('E,'):
                    ne += 1
        print(f'== {run}: code-bias records {nb}, epochs {ne}, per epoch {nb / max(1, ne):.2f}')
        log = open(d + '/logs/main_log.log', errors='replace').read()
        for pat in (r'Network \(wind-up census[^\n]*', r'Network \(wind-up restart response[^\n]*',
                    r'Network \(IF-TD pairs[^\n]*', r'READING \[[a-z]+\][^\n]*'):
            for m in re.findall(pat, log)[:2]:
                print('   ' + m[:600])
        ev = collections.Counter()
        p = d + '/logs/network_events.csv'
        rows = []
        if os.path.exists(p):
            rows = list(csv.DictReader(ln for ln in open(p) if not ln.startswith('#')))
        wu = [r for r in rows if r['event'] == 'wu_restart']
        for r in wu:
            ev[r['action']] += 1
        print(f'   wu_restart events by action: {dict(ev)}')
        # The same epoch's arc classification (card G's arc_resume rows, written at the PASS-B
        # inject site after every drain): an in-horizon restart whose arc still reads *_bridged
        # had its leveling carry folded across the restart (card WUP G1).
        cls = collections.defaultdict(list)
        for r in rows:
            if r['event'] == 'arc_resume':
                cls[(r['tow'], r['station'], r['sat'])].append(r['action'])
        inh = [r for r in wu if r['action'] not in ('beyond_carry', 'stale_refused')]
        bridged = [(r['tow'], r['station'], r['sat'], r['action'])
                   for r in inh if any(a.endswith('_bridged') for a in cls.get((r['tow'], r['station'], r['sat']), []))]
        print(f'   in-horizon restarts {len(inh)}; with a same-epoch arc_resume row *_bridged: {len(bridged)} {bridged[:6]}')


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    if sys.argv[1] == 'census':
        census(sys.argv[2], sys.argv[3], sys.argv[4:])
    else:
        wire(sys.argv[2:])

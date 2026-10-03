#!/usr/bin/env python3
"""Card SCL one-line scorecard per arm (gates G1 to G3).

    python scripts/clock_level/scl_grade.py <walk_csv|none> <run> [<run> ...]       (run = dir, or tag under runs/_rap_)

Sim arms (a truth_bias exists for the fixture): the pivot-frame level error rms and median
e^2/(1/I_L) at epochs 60 / 120 / 239 (levelnees with the a priori error added), the broadcast
motion hidden over every held emission (card SMH's G3 grader, re-based fixture), the per-epoch rms
of the level error's change after the first half hour.  Both beds: the IF code spread in hour 2
and the anchor trips (ctugrade), card PVN's NIS mean (pvnees, when the trace exists), the consumer
3D p50 by hour.
"""
import csv, importlib.util, math, os, re, statistics as st, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('clklevel', ROOT + '/scripts/clock_level/clklevel.py')
argv = sys.argv; sys.argv = ['x']; cl = importlib.util.module_from_spec(spec); spec.loader.exec_module(cl); sys.argv = argv
EP = [59, 119, 238]


def resolve(a):
    for c in (a, f'{ROOT}/runs/{a}', f'{ROOT}/runs/_rap_{a}'):
        if os.path.isdir(c):
            return c
    raise SystemExit('no run ' + a)


def load_walk(p):
    W = {}
    if p != 'none':
        for r in csv.DictReader(open(p)):
            W.setdefault(r['sat'], {})[round(float(r['tow']))] = float(r['w_m'])
    return W


def level(d, W, ds):
    wk = lambda s, t: W.get(s, {}).get(int(round(t / 30.0)) * 30, 0.0)
    c, tows = cl.clk(d)
    mon = {}
    p = os.path.join(d, 'logs', 'network_level_bound.csv')
    if os.path.exists(p):
        for r in csv.DictReader(open(p)):
            mon.setdefault(round(float(r['tow']), 3), {})[r['sat']] = float(r['inv_info_m2'])
    out = []
    for i in EP:
        if i >= len(tows):
            out.append('  -  '); continue
        t = tows[i]
        m = mon.get(t, {})
        piv = [s for s in c if t in c[s] and s in ds and (s not in m if m else s == 'G02')]
        pv = piv[0] if len(piv) == 1 else 'G02'
        if t not in c.get(pv, {}):
            out.append('  -  '); continue
        e0 = c[pv][t] + ds[pv] + wk(pv, t)
        es, nn = [], []
        for s in c:
            if s == pv or s not in ds or t not in c[s]:
                continue
            e = c[s][t] + ds[s] + wk(s, t) - e0
            es.append(e)
            if s in m:
                nn.append(e * e / m[s])
        rms = math.sqrt(sum(x * x for x in es) / len(es)) if es else float('nan')
        out.append(f'{rms:.3f}' + (f'/{st.median(nn):.1f}' if nn else ''))
    # per-epoch step of the clock error after the first half hour, de-commoned per epoch (card
    # CLK-R's frame hold removes the common step from the emitted clocks, so a pivot-frame step
    # would read the pivot's own a priori walk as error on every satellite; card SCL section 9)
    dc = {}
    for t in tows:
        e = {s: c[s][t] + wk(s, t) for s in c if t in c[s] and s in ds}
        if len(e) >= 3:
            m = st.median(e.values())
            dc[t] = {s: v - m for s, v in e.items()}
    steps = []
    for a, b in zip(tows, tows[1:]):
        if abs(b - a - 30) > 1 or a < tows[0] + 1800 or a not in dc or b not in dc:
            continue
        for s in dc[a]:
            if s in dc[b]:
                steps.append(dc[b][s] - dc[a][s])
    step = math.sqrt(sum(x * x for x in steps) / len(steps)) if steps else float('nan')
    return ' '.join(out), step


def run_txt(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stdout


def main():
    W = load_walk(sys.argv[1])
    ds = cl.datum_share()
    walk = sys.argv[1]
    print('arm                      | level rms/e2(1/I_L) @60 120 239  | step mm | hidden | IFsd h2 trips | PVN NIS | consumer p50 by hour')
    for a in sys.argv[2:]:
        d = resolve(a)
        name = os.path.basename(d).replace('_rap_', '')
        sim = 'SIM' in open(os.path.join(d, 'products.txt'), errors='replace').readline() + open(os.path.join(d, 'products.txt'), errors='replace').read(400)
        lv, step, hid = '', float('nan'), ''
        if sim:
            lv, step = level(d, W, ds)
            g = run_txt([sys.executable, HERE + '/g3grade.py', walk if walk != 'none' else 'none', d])
            m = re.search(r'gen\s+: hidden ([\-0-9.na]+)', g)
            hid = m.group(1) if m else ''
        g = run_txt([sys.executable, ROOT + '/scripts/clock_level/ctugrade.py', d])
        m = re.search(r'IFsd h2 ([0-9.]+).*?trips (\d+)', g)
        ifs = f'{m.group(1)} {m.group(2)}' if m else ''
        nis = ''
        if os.path.exists(os.path.join(d, 'logs', 'network_phase_visible.csv')):
            g = run_txt([sys.executable, ROOT + '/scripts/clock_level/pvnees.py', d])
            m = re.search(r'NIS mean ([0-9.]+)', g)
            nis = m.group(1) if m else ''
        g = run_txt([sys.executable, ROOT + '/scripts/clock_level/p50.py', d])
        p50 = ' '.join(g.split()[1:])
        print(f'{name:24s} | {lv:32s} | {step * 1000:7.1f} | {hid:6s} | {ifs:13s} | {nis:7s} | {p50}')


if __name__ == '__main__':
    main()

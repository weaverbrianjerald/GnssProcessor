#!/usr/bin/env python3
"""Card CTU one-screen grade of an arm (sim or field; the sim-only rows are skipped on the field).
Sim:   datum-free level rms across satellites (per-epoch median removed) at epochs 1..8 and
       16 / 60 / 120 / 240; the pivot-frame normalized error along the level (e^2 / P_L, median
       over satellites) where the monitor trace exists.
Both:  the per-satellite median IF(C1C,C2W) post-fit code residual in the last hour (sd across
       satellites) and in hour 2 when the run is >= 2 h; anchor folds / floor share / trips (NIS,
       abs, other); RAIM-attrib re-floats; SBC completions; consumer 3D p50 by hour.
    python scripts/clock_level/ctugrade.py <run> [<run> ...]      (run = dir, or tag under runs/_rap_<tag>)"""
import csv, os, re, statistics as st, sys
from collections import defaultdict
import importlib.util

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'
T0 = 259230.0
F1 = 1575.42e6; F2 = 1227.60e6; A = F1 * F1 / (F1 * F1 - F2 * F2); B = F2 * F2 / (F1 * F1 - F2 * F2)
spec = importlib.util.spec_from_file_location('clklevel', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'clklevel.py'))
argv = sys.argv; sys.argv = ['clklevel.py']
cl = importlib.util.module_from_spec(spec); spec.loader.exec_module(cl)
sys.argv = argv


def resolve(a):
    if os.path.isdir(a):
        return a
    for c in (os.path.join(RUNS, a), os.path.join(RUNS, '_rap_' + a)):
        if os.path.isdir(c):
            return c
    raise SystemExit('no run ' + a)


def is_sim(d):
    try:
        return 'simnet60' in open(os.path.join(d, 'arm.json')).read()[:200000]
    except OSError:
        return 'sim' in os.path.basename(d)


def level(d):
    ds = cl.datum_share(); c, tows = cl.clk(d); out = {}
    for i, t in enumerate(tows):
        v = {s: c[s][t] + ds[s] for s in c if t in c[s] and s in ds}
        if len(v) < 3:
            continue
        m = st.median(v.values()); out[i] = {s: x - m for s, x in v.items()}
    return out, c, tows, ds


def rms(v):
    v = list(v)
    return (sum(x * x for x in v) / len(v)) ** 0.5 if v else float('nan')


def nees(d, c, tows, ds, eps):
    p = os.path.join(d, 'logs', 'network_level_bound.csv')
    if not os.path.exists(p):
        return {}
    mon = defaultdict(dict)
    for r in csv.DictReader(open(p)):
        mon[round(float(r['tow']), 3)][r['sat']] = float(r['p_level_m2'])
    out = {}
    for i in eps:
        if i >= len(tows):
            continue
        t = tows[i]; m = mon.get(t, {})
        piv = [s for s in c if t in c[s] and s in ds and s not in m]
        if len(piv) != 1:
            continue
        e0 = c[piv[0]][t] + ds[piv[0]]
        n = [((c[s][t] + ds[s] - e0) ** 2) / P for s, P in m.items() if t in c.get(s, {}) and s in ds and P > 0]
        if n:
            out[i] = st.median(n)
    return out


def ifspread(d, lo, hi, t0=T0):
    pend = {}; per = defaultdict(list)
    p = os.path.join(d, 'logs', 'network_pf_rows.csv')
    if not os.path.exists(p):
        return float('nan')
    with open(p) as f:
        rd = csv.reader(f); h = next(rd); ix = {k: i for i, k in enumerate(h)}
        for r in rd:
            if r[ix['kind']] != 'code' or r[ix['sat']][0] != 'G':
                continue
            t = float(r[ix['tow']]) - t0
            if not (lo <= t < hi):
                continue
            try:
                v = float(r[ix['resid_m']]); w = float(r[ix['w']])
            except ValueError:
                continue
            if w < 1e-2:
                continue
            k = (r[ix['station']], r[ix['sat']], t); sg = r[ix['signal']]
            a, b = pend.get(k, (None, None))
            if sg == 'C1C':
                a = v
            elif sg == 'C2W':
                b = v
            else:
                continue
            if a is not None and b is not None:
                per[r[ix['sat']]].append(A * a - B * b); pend.pop(k, None)
            else:
                pend[k] = (a, b)
    meds = [st.median(v) for v in per.values() if len(v) > 50]
    return st.pstdev(meds) if len(meds) >= 3 else float('nan')


def runend(d):
    out = {'trips': '', 'floor_pct': '', 'raim_ep': 0, 'raim_amb': 0, 'sbc': '', 'emitted': ''}
    trip_kind = defaultdict(int)
    p = os.path.join(d, 'logs', 'network_events.csv')
    if os.path.exists(p):
        for r in csv.reader(open(p)):
            if len(r) > 9 and r[2] == 'CLK_ANCHOR' and r[3] == 'cta_decouple':
                m = re.search(r'reason=(\w+)', r[9]); trip_kind[m.group(1) if m else '?'] += 1
    out['trips'] = sum(trip_kind.values()); out['trip_kinds'] = dict(trip_kind)
    for ln in open(os.path.join(d, 'stdout.txt'), encoding='utf-8', errors='replace'):
        m2 = re.search(r'(?:Card Q3 / )?guarded temporal clock anchor\): (\d+) epoch\(s\); (\d+) clock carry', ln)
        m = re.search(r', (\d+) at the variance floor', ln)
        if m and m2:
            out['floor_pct'] = f'{100.0 * int(m.group(1)) / max(int(m2.group(2)), 1):.1f}'
        m = re.search(r'RAIM-attrib\]: TOW [\d.]+ refloated (\d+) carried', ln)
        if m:
            out['raim_ep'] += 1; out['raim_amb'] += int(m.group(1))
        m = re.search(r'S-basis completion constrained (\d+) extra', ln)
        if m:
            out['sbc'] = int(m.group(1))
        m = re.search(r'producer complete: (\d+) epochs emitted, (\d+) skipped', ln)
        if m:
            out['emitted'] = f'{m.group(1)}/{int(m.group(1)) + int(m.group(2))}'
    if out['sbc'] == '':
        out['sbc'] = 0
    return out


def consumer(d, t0=T0):
    p = os.path.join(d, 'logs', 'closed_loop_trace.csv'); by = defaultdict(list)
    if not os.path.exists(p):
        return ''
    for r in csv.DictReader(open(p)):
        try:
            t = float(r['tow']); e = float(r['pos_err_3d_m'])
        except (ValueError, KeyError, TypeError):
            continue
        by[int((t - t0) // 3600)].append(e)
    return ' '.join(f'{st.median(by[h]):.3f}' for h in sorted(by))


def main():
    for a in sys.argv[1:]:
        d = resolve(a); sim = is_sim(d)
        re_ = runend(d)
        c, tows = cl.clk(d)
        span = (tows[-1] - tows[0]) if tows else 0.0
        line = [os.path.basename(d).replace('_rap_', ''), f"ep {len(tows)}"]
        if sim:
            L, c, tows, ds = level(d)
            ign = ' '.join(f'{rms(L[i].values()):.2f}' for i in range(8) if i in L)
            key = ' / '.join(f'{rms(L[i].values()):.2f}' for i in (15, 59, 119, 239) if i in L)
            line += [f'ign[{ign}]', f'lvl16/60/120/240 {key}']
            n = nees(d, c, tows, ds, (59, 119, 239))
            if n:
                line.append('nees ' + '/'.join(f'{n[i]:.0f}' for i in sorted(n)))
        last = span + 30.0
        t0 = tows[0] if tows else T0
        if span >= 7000:
            line.append(f'IFsd h2 {ifspread(d, 3600.0, 7200.0, t0):.3f}')
        line.append(f'IFsd last-h {ifspread(d, max(0.0, last - 3600.0), last + 1, t0):.3f}')
        line += [f"trips {re_['trips']} {re_['trip_kinds'] if re_['trip_kinds'] else ''}".strip(),
                 f"floor% {re_['floor_pct']}", f"refl {re_['raim_ep']}/{re_['raim_amb']}",
                 f"sbc {re_['sbc']}", f"emit {re_['emitted']}", f'p50 {consumer(d, t0)}']
        print(' | '.join(line))


if __name__ == '__main__':
    main()

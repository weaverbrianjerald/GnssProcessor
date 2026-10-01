"""Card PAC section 4: the field's 2 h control (pivot G01) against the arm with G14 pinned.

    python scripts/clock_level/pac_arm.py

Per run hour 1 and 2: the frame-free common D slope and sum, the pivot's own D, and the pivot's
ionosphere-free code post-fit residual (mean over stations, from logs/network_pf_rows.csv) against
the other satellites' median.  Runs runs/_rap_wsx_ctl_f and runs/_rap_pac_g14_f.
"""
import csv, collections, statistics as st, importlib.util, sys
import numpy as np
spec = importlib.util.spec_from_file_location('rw', __import__('os').path.join(__import__('os').path.dirname(__import__('os').path.abspath(__file__)), 'rev_wire.py')); rw = importlib.util.module_from_spec(spec); spec.loader.exec_module(rw)
def fit(pts):
    if len(pts) < 5: return float('nan')
    x = np.array([t for t, _ in pts]); y = np.array([v for _, v in pts]); return np.polyfit(x, y, 1)[0] * 3600.0
def grade(run, pivot):
    d = rw.rundir(run); rows = rw.load(d); t0 = rw.run_start(d)
    inc = collections.defaultdict(dict)
    for (sat, sig), lst in rows.items():
        if sig != 'L2W': continue
        for cl in rw.clones(lst):
            for x, y in zip(cl, cl[1:]):
                if abs(y['tow'] - x['tow'] - 30) < 0.5: inc[y['tow']][sat] = y['sd'] - x['sd']
    com, piv = [], []; a = p = 0.0
    for t in sorted(inc):
        if len(inc[t]) < 4: continue
        a += st.median(inc[t].values()); com.append((t, a))
        if pivot in inc[t]: p += inc[t][pivot]; piv.append((t, p))
    out = []
    for h in (0, 1):
        seg = [q for q in com if t0 + h * 3600 <= q[0] < t0 + (h + 1) * 3600]
        segp = [q for q in piv if t0 + h * 3600 <= q[0] < t0 + (h + 1) * 3600]
        cm = (seg[-1][1] - seg[0][1]) if len(seg) > 1 else float('nan')
        pm = (segp[-1][1] - segp[0][1]) if len(segp) > 1 else float('nan')
        out.append(f'h{h+1}: common D slope {fit(seg):+.3f} m/h, sum {100*cm:+6.1f} cm, pivot {pivot} own {100*pm:+6.1f} cm ({len(segp)} ep)')
    # pivot IF code post-fit residual, mean over stations, by hour
    acc = collections.defaultdict(dict)
    for r in csv.DictReader(open(d + '/logs/network_pf_rows.csv')):
        if r['kind'] != 'code' or r['signal'] not in ('C1W', 'C1C', 'C2W'): continue
        k = (round(float(r['tow']), 1), r['station'], r['sat']); sg = 'C2W' if r['signal'] == 'C2W' else 'L1'
        acc[k][sg] = float(r['resid_m'])
    per = collections.defaultdict(lambda: collections.defaultdict(list))
    for (t, sta, sat), v in acc.items():
        if 'L1' in v and 'C2W' in v: per[sat][int((t - t0) // 3600)].append(2.5457 * v['L1'] - 1.5457 * v['C2W'])
    for h in (0, 1):
        oth = [np.mean(per[s][h]) for s in per if s != pivot and len(per[s][h]) > 50]
        print_ = f'h{h+1}: pivot IF code residual {1000*np.mean(per[pivot][h]) if per[pivot][h] else float("nan"):+7.1f} mm (n {len(per[pivot][h])}), others median {1000*np.median(oth):+6.1f} mm'
        out.append(print_)
    print(run, f'(pivot {pivot})'); [print('   ' + o) for o in out]
grade('_rap_wsx_ctl_f', 'G01'); grade('_rap_pac_g14_f', 'G14')

"""Per-satellite pivot-frame level error e_s(t) (walk + datum share removed) and its change across
the satellite's phase-datum events (PHASE_ANCHOR / PBGAUGE / STRANSFORM) vs ordinary epochs."""
import csv, sys, os, statistics as st, math
import importlib.util
spec = importlib.util.spec_from_file_location('clklevel', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'clklevel.py'))
argv = sys.argv; sys.argv = ['x']; cl = importlib.util.module_from_spec(spec); spec.loader.exec_module(cl); sys.argv = argv
W = {}
if sys.argv[1] != 'none':
    for r in csv.DictReader(open(sys.argv[1])):
        W.setdefault(r['sat'], {})[round(float(r['tow']))] = float(r['w_m'])
wk = lambda s, t: W.get(s, {}).get(int(round(t / 30.0)) * 30, 0.0)
ds = cl.datum_share()
for a in sys.argv[2:]:
    d = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'runs', a)
    c, tows = cl.clk(d)
    piv = 'G02'
    ev = {}
    for r in csv.DictReader(open(d + '/logs/network_events.csv')):
        if r['tier'] in ('PHASE_ANCHOR', 'PBGAUGE', 'STRANSFORM') and r['sat']:
            ev.setdefault(r['sat'][:3], set()).add(round(float(r['tow'])))
    jump_ev, jump_no, tot = [], [], []
    for s in c:
        if s == piv or s not in ds: continue
        ts = [t for t in tows if t in c[s] and t in c[piv]]
        e = {t: c[s][t] + ds[s] + wk(s, t) - (c[piv][t] + ds[piv] + wk(piv, t)) for t in ts}
        for i in range(1, len(ts)):
            if ts[i] - ts[i-1] > 31 or ts[i] < tows[0] + 1800: continue
            dj = e[ts[i]] - e[ts[i-1]]
            (jump_ev if round(ts[i]) in ev.get(s, ()) else jump_no).append(dj)
    rms = lambda x: math.sqrt(sum(v*v for v in x)/len(x)) if x else float('nan')
    n_ev = len(jump_ev)
    print(f'{a}: event epochs n={n_ev} rms step {rms(jump_ev):.4f} m (sum sq {sum(v*v for v in jump_ev):.4f}); '
          f'other epochs n={len(jump_no)} rms step {rms(jump_no):.4f} m (sum sq {sum(v*v for v in jump_no):.4f})')

#!/usr/bin/env python3
"""Ignition: per-satellite datum-free level error, epochs 1..N, for runs side by side.
    python scripts/clock_level/ign.py [-v] <run> [<run> ...]      (N = $IGN_N, default 12; -v per satellite)"""
import os, statistics as st, sys
import importlib.util
spec = importlib.util.spec_from_file_location('clklevel', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'clklevel.py'))
argv = sys.argv; sys.argv = ['clklevel.py']
cl = importlib.util.module_from_spec(spec); spec.loader.exec_module(cl)
sys.argv = argv
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
N = int(os.environ.get("IGN_N", "12"))
def paths(d, n=N):
    ds = cl.datum_share(); c, tows = cl.clk(d); out = []
    for i in range(min(n, len(tows))):
        t = tows[i]; v = {s: c[s][t] + ds[s] for s in c if t in c[s] and s in ds}
        m = st.median(v.values()); out.append({s: x - m for s, x in v.items()})
    return out
verbose = '-v' in sys.argv
for a in [x for x in sys.argv[1:] if not x.startswith('-')]:
    d = a if os.path.isdir(a) else ROOT + '/runs/' + a
    P = paths(d)
    rms = [(sum(x*x for x in p.values())/len(p))**0.5 for p in P]
    print(f'{os.path.basename(d):24s} ' + ' '.join(f'{r:.2f}' for r in rms))
    if verbose:
        sats = sorted(P[0])
        for s in sats:
            print('   ', s, ' '.join(f"{P[i].get(s, float('nan')):+6.2f}" for i in range(len(P))))

"""Slope of the wire clock's de-commoned increment (C) and of the wire phase bias's (P, L1C) on the
truth's de-commoned clock increment -dw, within generations: how much of the walk the estimator's
clock takes, and how much the emitted phase bias moves with it.  python scripts/clock_level/g3track.py <walk> <run>..."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g3grade as g
w = g.load_walk(sys.argv[1])
for a in sys.argv[2:]:
    d = g.resolve(a); C, P = g.load_wire(d)
    tows = sorted(set(C) & set(P))
    dc, dp, dt = {}, {}, {}
    for t in tows:
        sats = [s for s in C[t] if s in P[t]]
        dc[t] = g.decommon({s: C[t][s] for s in sats})
        dp[t] = g.decommon({s: P[t][s][0] for s in sats})
        dt[t] = g.decommon({s: -g.walk_at(w, s, t) for s in sats})
    first = {}; pc = []; pp = []
    for t in tows:
        for s, (p, gen) in P[t].items():
            if s not in dt[t]: continue
            k = (s, gen)
            if k not in first: first[k] = t; continue
            t0 = first[k]
            if s not in dt.get(t0, {}): continue
            x = dt[t][s] - dt[t0][s]
            pc.append((x, dc[t][s] - dc[t0][s])); pp.append((x, dp[t][s] - dp[t0][s]))
    sc = g.slope(pc)[0]; sp = g.slope(pp)[0]
    print(f'{os.path.basename(d)}: clock takes {sc:.3f} of the walk, the held phase bias moves {sp:.3f}, D passes {sc - sp:.3f}')

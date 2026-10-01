"""Card FWS: the persistence of the per-satellite walk share, for one-change arms.

    python scripts/clock_level/wsf_persist.py <run> [<run> ...]

From logs/network_pb_hold_clone.csv (each satellite's L2W block clone, card REV's columns): over
clone rows of age at least 30 minutes, the rms change of the walk share Phi_D over a lag of k epochs
against the white expectation sqrt(k) * rms(one-epoch step) (the persistence ratio; 1 for
independent steps, card FWS section 1), at k = 5, 20 and 60; the rms one-epoch step; and the median
|Phi_D| at clone age 1 h.  A 2 h arm reads run time 0.6 to 2 h.
"""
import importlib.util, math, os, statistics as st, sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('rev_wire', os.path.join(HERE, 'rev_wire.py'))
rw = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rw)

LAGS = (5, 20, 60)


def persist(run):
    d = rw.rundir(run)
    rows = rw.load(d)
    steps, lag = [], {k: [] for k in LAGS}
    at1h = []
    for (sat, sig), lst in rows.items():
        if sig != 'L2W':
            continue
        for cl in rw.clones(lst):
            near = [x for x in cl if abs(x['age'] - 3600.0) <= 60.0]
            if near:
                at1h.append(abs(min(near, key=lambda r: abs(r['age'] - 3600.0))['phi']))
            cl = [x for x in cl if x['age'] >= 1800.0]
            by = {round(x['tow'], 1): x for x in cl}
            for a, b in zip(cl, cl[1:]):
                if abs(b['tow'] - a['tow'] - 30.0) < 0.5:
                    steps.append((b['phi'] - a['phi']) ** 2)
            for x in cl:
                for k in LAGS:
                    y = by.get(round(x['tow'] + 30.0 * k, 1))
                    if y is not None:
                        lag[k].append((y['phi'] - x['phi']) ** 2)
    if not steps:
        return None
    s1 = math.sqrt(sum(steps) / len(steps))
    ratios = {k: (math.sqrt(sum(v) / len(v)) / (math.sqrt(k) * s1) if v else float('nan'), len(v)) for k, v in lag.items()}
    return s1, ratios, (st.median(at1h) if at1h else float('nan')), len(at1h)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    print('run: rms step (mm) | persistence ratio at lag 5 / 20 / 60 epochs (rows) | median |Phi_D| at age 1 h (clones)')
    for r in sys.argv[1:]:
        out = persist(r)
        if out is None:
            print(f'{r}: no rows')
            continue
        s1, ratios, m1h, n1h = out
        print(f'{r}: {1000 * s1:.3f} | ' + ' / '.join(f'{ratios[k][0]:.2f} ({ratios[k][1]})' for k in LAGS)
              + f' | {1000 * m1h:.1f} mm ({n1h})')

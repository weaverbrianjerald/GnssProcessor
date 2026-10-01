"""Card WSL: the walk share and the stated contrast against the stations observing each satellite.

    python scripts/clock_level/wsl_link.py <field blk_run> <sim blk_run>

Station count n(t, sat): the number of stations carrying an ambiguity of the satellite on L2W at
that epoch (logs/network_amb_state.csv).
L1  per satellite and epoch, card PVN's stated contrast sd (logs/network_phase_visible.csv,
    sqrt_sp_m) against n: the slope of log sd on log n within each run hour (2 to 6) and pooled with
    the hour's mean removed.
L2  per block clone (L2W key), at clone age 1 h (the row nearest 3600 s within 60 s): the realized
    walk share |Phi_D|, the structural share f = W / (V0 + W), and n-bar, the mean n over the clone's
    rows up to that age; the slopes of log |Phi_D| and log f on log n-bar.
L3  the beds' n-bar distributions, and the beds' median |Phi_D| at age 1 h in bins of n-bar that hold
    clones of both beds, against the unmatched ratio.
"""
import collections, csv, importlib.util, math, os, statistics as st, sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('rev_wire', os.path.join(HERE, 'rev_wire.py'))
rw = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rw)


def counts(d):
    """(tow, sat) -> number of stations carrying an L2W ambiguity of the satellite."""
    n = collections.defaultdict(set)
    with open(d + '/logs/network_amb_state.csv') as f:
        rows = (ln for ln in f if not ln.startswith('#'))
        for r in csv.DictReader(rows):
            if r['signal'] == 'L2W':
                n[(round(float(r['tow']), 1), r['sat'])].add(r['station'])
    return {k: len(v) for k, v in n.items()}


def ols(xy):
    """Slope and its standard error of y on x."""
    n = len(xy)
    if n < 3:
        return float('nan'), float('nan')
    mx = sum(x for x, _ in xy) / n
    my = sum(y for _, y in xy) / n
    sxx = sum((x - mx) ** 2 for x, _ in xy)
    if sxx <= 0:
        return float('nan'), float('nan')
    b = sum((x - mx) * (y - my) for x, y in xy) / sxx
    res = sum((y - my - b * (x - mx)) ** 2 for x, y in xy)
    return b, math.sqrt(res / (n - 2) / sxx)


def bed(blk):
    d = rw.rundir(blk)
    t_start = rw.run_start(d)
    n = counts(d)
    # L1: the stated contrast sd against n, within run hours.
    by_h = collections.defaultdict(list)
    for r in csv.DictReader(open(d + '/logs/network_phase_visible.csv')):
        try:
            sd = float(r['sqrt_sp_m'])
        except ValueError:
            continue
        t = round(float(r['tow']), 1)
        k = n.get((t, r['sat']))
        if not k or not (sd > 0.0):
            continue
        h = int((t - t_start) // 3600)
        by_h[h].append((math.log(k), math.log(sd)))
    l1 = {}
    pooled = []
    for h in sorted(by_h):
        if h < 1:
            continue
        b, se = ols(by_h[h])
        l1[h + 1] = (b, se, len(by_h[h]))
        mx = sum(x for x, _ in by_h[h]) / len(by_h[h])
        my = sum(y for _, y in by_h[h]) / len(by_h[h])
        pooled += [(x - mx, y - my) for x, y in by_h[h]]
    l1['pooled'] = (*ols(pooled), len(pooled))
    # L2: clones at age 1 h.
    rows = rw.load(d)
    cl1 = []
    for (sat, sig), lst in rows.items():
        if sig != 'L2W':
            continue
        for cl in rw.clones(lst):
            near = [x for x in cl if abs(x['age'] - 3600.0) <= 60.0]
            if not near:
                continue
            x = min(near, key=lambda r: abs(r['age'] - 3600.0))
            ks = [n.get((r['tow'], sat)) for r in cl if r['age'] <= x['age']]
            ks = [k for k in ks if k]
            if not ks or not (abs(x['phi']) > 0.0):
                continue
            f = x['w'] / (x['vb'] + x['w']) if (x['vb'] + x['w']) > 0 else float('nan')
            cl1.append((sum(ks) / len(ks), abs(x['phi']), f, sat, cl[0]['t0']))
    return dict(l1=l1, clones=cl1, ncount=list(n.values()))


def main(field, sim):
    res = {'field': bed(field), 'sim': bed(sim)}
    for name, r in res.items():
        print(f'== {name}: station counts per satellite-epoch p10/p50/p90 '
              + '/'.join(str(sorted(r["ncount"])[int(q * (len(r["ncount"]) - 1))]) for q in (0.1, 0.5, 0.9)))
        print('   L1 slope of log stated contrast sd on log n by run hour (se, rows): '
              + '; '.join(f'h{h}: {b:+.2f} ({se:.2f}, {m})' for h, (b, se, m) in r['l1'].items() if h != 'pooled')
              + '; pooled {:+.2f} ({:.2f}, {})'.format(*r['l1']['pooled']))
        c = r['clones']
        b1, se1 = ols([(math.log(k), math.log(p)) for k, p, f, _, _ in c])
        b2, se2 = ols([(math.log(k), math.log(f)) for k, p, f, _, _ in c if f > 0])
        nb = sorted(k for k, *_ in c)
        print(f'   L2 clones at age 1 h: {len(c)}; n-bar p10/p50/p90 {nb[len(nb) // 10]:.1f}/{nb[len(nb) // 2]:.1f}/'
              f'{nb[9 * len(nb) // 10]:.1f}; median |Phi_D| {1000 * st.median(p for _, p, *_ in c):.1f} mm; slope of '
              f'log |Phi_D| on log n-bar {b1:+.2f} ({se1:.2f}); of log f {b2:+.2f} ({se2:.2f})')
    F, S = res['field']['clones'], res['sim']['clones']
    if F and S:
        un = st.median(p for _, p, *_ in F) / st.median(p for _, p, *_ in S)
        print(f'== L3: unmatched ratio of median |Phi_D| at age 1 h, field / sim {un:.2f}')
        edges = [0, 8, 12, 16, 20, 24, 28, 36, 100]
        for lo, hi in zip(edges, edges[1:]):
            fb = [p for k, p, *_ in F if lo <= k < hi]
            sb = [p for k, p, *_ in S if lo <= k < hi]
            if fb and sb:
                print(f'   n-bar [{lo},{hi}): field {len(fb):3d} clones median {1000 * st.median(fb):6.1f} mm | sim '
                      f'{len(sb):3d} clones median {1000 * st.median(sb):6.1f} mm | ratio {st.median(fb) / st.median(sb):.2f}')
            elif fb or sb:
                print(f'   n-bar [{lo},{hi}): field {len(fb)} clones, sim {len(sb)} clones (not matched)')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])

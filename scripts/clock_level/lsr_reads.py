"""Card LSR: the trace reads R1 (the troposphere's signature) and R2 (station-set changes).

    python scripts/clock_level/lsr_reads.py <run> [<run> ...]

R1  per station and epoch (at least 4 lines of sight), the slope c of the ionosphere-free post-fit
    phase residuals (card SOI's T1 residual) on the wet mapping function 1 / sin e less the station's
    epoch mean: the zenith wet-delay misfit the estimator did not follow, after the receiver clock.
    Its 20-epoch block rms by station class (tropical: |latitude| below 23.5 degrees) and per
    station; then, per satellite's L2W block clone (rows of age at least 30 minutes) and lag k of 60
    and 120 epochs, the walk share's change against the satellite-mean (lines of sight present at
    both ends) change of g = c (1 / sin e - mean), both de-commoned across satellites per epoch.
R2  per clone window of 60 and 120 epochs, the number of stations starting or stopping to observe
    the satellite inside it (logs/network_ionosphere.csv station sets); the walk share's squared
    change regressed on the count (intercept: no change; slope: per change) and its rms by count bin.
"""
import collections, importlib.util, math, os, statistics as st, sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('soi_test', os.path.join(HERE, 'soi_test.py'))
so = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(so)
rw = so.rw
LAGS = (60, 120)


def r1(sta, P, R):
    ep = collections.defaultdict(list)          # (t, station) -> [(sat, m, resid)]
    for k, v in R.items():
        if k in P:
            el = P[k][3]
            ep[(k[0], k[1])].append((k[2], 1.0 / math.sin(math.radians(el)), v))
    c, g = {}, {}
    for (t, s), lst in ep.items():
        if len(lst) < 4:
            continue
        mb = sum(m for _, m, _ in lst) / len(lst)
        rb = sum(v for _, _, v in lst) / len(lst)
        sxx = sum((m - mb) ** 2 for _, m, _ in lst)
        if sxx < 1e-6:
            continue
        cc = sum((m - mb) * (v - rb) for _, m, v in lst) / sxx
        c[(t, s)] = cc
        for sat, m, _ in lst:
            g[(t, sat, s)] = cc * (m - mb)
    # block rms by station
    by_sta = collections.defaultdict(list)
    for (t, s), v in c.items():
        by_sta[s].append((t, v))
    blk = {}
    for s, lst in by_sta.items():
        lst.sort()
        t0 = lst[0][0]
        b = collections.defaultdict(list)
        for t, v in lst:
            b[int((t - t0) // 600)].append(v)
        bm = [sum(v) / len(v) for v in b.values() if len(v) >= 15]
        if bm:
            blk[s] = math.sqrt(sum(x * x for x in bm) / len(bm))
    return c, g, blk


def windows(d, P, g):
    los = collections.defaultdict(dict)         # (t, sat) -> {sta: g}
    sets = collections.defaultdict(set)         # (t, sat) -> stations observing
    for (t, s, v) in P:
        sets[(t, v)].add(s)
    for (t, v, s), x in g.items():
        los[(t, v)][s] = x
    rows = rw.load(d)
    out = {}
    for k in LAGS:
        recs = collections.defaultdict(list)
        r2 = []
        for (sat, sig), lst in rows.items():
            if sig != 'L2W':
                continue
            for cl in rw.clones(lst):
                cl = [x for x in cl if x['age'] >= 1800.0]
                by = {x['tow']: x for x in cl}
                for x in cl:
                    y = by.get(round(x['tow'] + 30.0 * k, 1))
                    if y is None:
                        continue
                    dphi = y['phi'] - x['phi']
                    n = 0
                    prev = sets.get((x['tow'], sat), set())
                    for j in range(1, k + 1):
                        cur = sets.get((round(x['tow'] + 30.0 * j, 1), sat), set())
                        n += len(cur ^ prev)
                        prev = cur
                    r2.append((n, dphi))
                    a, b = los.get((x['tow'], sat), {}), los.get((y['tow'], sat), {})
                    both = [s for s in a if s in b]
                    if both:
                        recs[x['tow']].append((sat, dphi, sum(b[s] - a[s] for s in both) / len(both)))
        clus = collections.defaultdict(list)
        for t, lst in recs.items():
            if len(lst) < 3:
                continue
            mp = sum(a for _, a, _ in lst) / len(lst)
            mm = sum(c for _, _, c in lst) / len(lst)
            for sat, a, c in lst:
                clus[sat].append((c - mm, a - mp))
        b, se, r, n = so.regress(clus)
        dg = math.sqrt(sum(x * x for v in clus.values() for x, _ in v) / max(1, n))
        # R2: OLS of dphi^2 on the count
        xs = [n_ for n_, _ in r2]
        ys = [d * d for _, d in r2]
        mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
        sxx = sum((x - mx) ** 2 for x in xs)
        slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx > 0 else float('nan')
        icpt = my - slope * mx
        bins = collections.defaultdict(list)
        for n_, d in r2:
            bins[0 if n_ == 0 else 1 if n_ <= 2 else 2 if n_ <= 5 else 3].append(d * d)
        out[k] = dict(t2=(b, se, r, n, dg), icpt=icpt, slope=slope, nwin=len(r2), mcount=mx,
                      bins={i: ((len(bins[i]), math.sqrt(sum(bins[i]) / len(bins[i]))) if bins.get(i) else (0, float('nan')))
                            for i in range(4)})
    return out


def main(runs):
    for run in runs:
        d = rw.rundir(run)
        sta, P, R, births = so.load(d)
        c, g, blk = r1(sta, P, R)
        trop = {s for s, (_, lat, _) in sta.items() if abs(math.degrees(lat)) < 23.5}
        bt = [v for s, v in blk.items() if s in trop]
        bo = [v for s, v in blk.items() if s not in trop]
        print(f'== {run}: {len(c)} station-epochs with c; tropical stations {len(bt)}, others {len(bo)}')
        print(f'   R1 c block rms (zenith mm) median tropical {1e3 * st.median(bt):.2f}, others {1e3 * st.median(bo):.2f}; '
              f'all p50/p90 {1e3 * st.median(blk.values()):.2f}/{1e3 * rw.q(list(blk.values()), 0.9):.2f}')
        top = sorted(blk.items(), key=lambda kv: -kv[1])[:8]
        print('   R1 largest: ' + ', '.join(f'{s} {1e3 * v:.1f}{"*" if s in trop else ""}' for s, v in top))
        w = windows(d, P, g)
        for k, o in w.items():
            b, se, r, n, dg = o['t2']
            print(f'   R1 lag {k:3d}: slope {b:+.3f} (se {se:.3f}), r {r:+.3f}, n {n}, rms dg {1e3 * dg:.2f} mm')
            print(f'   R2 lag {k:3d}: windows {o["nwin"]}, mean changes {o["mcount"]:.2f}; dPhi^2 = {1e6 * o["icpt"]:.2f} + '
                  f'{1e6 * o["slope"]:.3f} n (mm^2); rms by count 0 / 1-2 / 3-5 / 6+: '
                  + ' / '.join(f'{1e3 * v[1]:.2f} ({v[0]})' for v in o['bins'].values()))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])

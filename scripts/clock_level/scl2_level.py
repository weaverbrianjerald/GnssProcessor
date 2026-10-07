#!/usr/bin/env python3
"""Card SCL2: the satellite clock level against truth over a whole run (sim), split into its parts.

    python scripts/clock_level/scl2_level.py series <walk_csv|none> <run> [--every 30] [--persat out.csv]
    python scripts/clock_level/scl2_level.py decomp <walk_csv|none> <run> [<run> ...]
    python scripts/clock_level/scl2_level.py stated <persat.csv> <run> <q_m2s>

The level error is scl_grade.py's: e_s(t) = C_s + ds_s + w_s(t) minus the same for the pivot, with C the
wire clock record, ds the IF(C1C,C2W) truth OSB share (clklevel.datum_share) and w the a priori error
(`g3_walk.py` format, column w_m; `none` on a COD bed, where the a priori is the truth).  The pivot is
the satellite with a wire clock and no `network_level_bound.csv` row at that epoch.

series   per epoch (every N): rms e, the across-satellite mean (the pivot-common part: minus the pivot's
         own level error), the rms with that mean removed (the de-commoned level), median e^2/(1/I_L)
         against the code-only bound, and median e^2/P_L; --persat writes every (epoch, satellite) row.
decomp   per 120-epoch block from the first emitted epoch: the pivot-frame rms, the common part, the
         de-commoned rms (card SCL2 section 3.1) and the all-satellite rms (amendment A2: the pivot included,
         its pivot-frame error 0, every satellite about the mean of all).  The common part is minus the
         pinned pivot's own level error (card SCL2 section 11); the de-commoned rms leaves the pivot ungraded.
         --restart-bound (series only): the code-only bound 1/I_L restarted at each satellite's readmission
         (a gap above 10 epochs in its monitor rows), since the producer's bound keeps the pre-park
         information (card SCL2 section 11, G26); written as IL_restart_m2 in the per-satellite file.
stated   per 30-epoch window: the de-commoned error against the producer's statement, the prior
         clock-contrast variance of `network_level_innov.csv` (p_prior_m2 at epoch t+1) less the walk
         q*30 s; rms actual, sd stated (median), mean NEES e_dm^2 / stated.
"""
import csv, importlib.util, math, os, statistics as st, sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
spec = importlib.util.spec_from_file_location('clklevel', ROOT + '/scripts/clock_level/clklevel.py')
argv = sys.argv; sys.argv = ['x']; cl = importlib.util.module_from_spec(spec); spec.loader.exec_module(cl); sys.argv = argv


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


def series(d, W, ds):
    """[(tow, pivot, {sat: (e, P_L, 1/I_L)})] for every emitted epoch."""
    wk = lambda s, t: W.get(s, {}).get(int(round(t / 30.0)) * 30, 0.0)
    c, tows = cl.clk(d)
    mon = {}
    p = os.path.join(d, 'logs', 'network_level_bound.csv')
    if os.path.exists(p):
        for r in csv.DictReader(open(p)):
            mon.setdefault(round(float(r['tow']), 3), {})[r['sat']] = (float(r['p_level_m2']), float(r['inv_info_m2']))
    out = []
    for t in tows:
        m = mon.get(t, {})
        piv = [s for s in c if t in c[s] and s in ds and (s not in m if m else s == 'G02')]
        pv = piv[0] if len(piv) == 1 else 'G02'
        if t not in c.get(pv, {}):
            out.append((t, pv, {})); continue
        e0 = c[pv][t] + ds[pv] + wk(pv, t)
        row = {}
        for s in c:
            if s == pv or s not in ds or t not in c[s]:
                continue
            PL, IL = m.get(s, (float('nan'), float('nan')))
            row[s] = (c[s][t] + ds[s] + wk(s, t) - e0, PL, IL)
        out.append((t, pv, row))
    return out


def restart_bound(S, gap_ep=10):
    """{(epoch index, sat): 1/I_L restarted at the satellite's last readmission}.  The producer's bound
    accumulates the code-only information without decay, so a satellite readmitted after a park keeps its
    pre-park epochs; the information at the last monitor row before a gap above gap_ep epochs is subtracted."""
    last, offset, out = {}, {}, {}
    for i, (_, _, row) in enumerate(S):
        for s, (_, _, IL) in row.items():
            if not (IL == IL and IL > 0.0):
                continue
            info = 1.0 / IL
            if s in last and i - last[s][0] > gap_ep:
                offset[s] = last[s][1]
            last[s] = (i, info)
            rem = info - offset.get(s, 0.0)
            out[(i, s)] = 1.0 / rem if rem > 0.0 else float('nan')
    return out


def cmd_series(a):
    W, d = load_walk(a[0]), resolve(a[1])
    every = int(a[a.index('--every') + 1]) if '--every' in a else 30
    persat = a[a.index('--persat') + 1] if '--persat' in a else None
    S = series(d, W, cl.datum_share())
    RB = restart_bound(S) if '--restart-bound' in a else None
    il = (lambda i, s, v: RB.get((i, s), float('nan'))) if RB is not None else (lambda i, s, v: v[2])
    f = lambda L: st.median(L) if L else float('nan')
    print('==', os.path.basename(d), 'epochs', len(S), '(bound restarted at readmissions)' if RB is not None else '')
    print('   ep pivot   n   rms_e  mean_e  rms_dm  med_e2/IL  med_e2/PL  sdIL_mm')
    for i in range(59, len(S), every):
        t, pv, row = S[i]
        if not row:
            continue
        es = [v[0] for v in row.values()]
        n = len(es); mu = sum(es) / n
        rms = math.sqrt(sum(x * x for x in es) / n)
        rdm = math.sqrt(sum((x - mu) ** 2 for x in es) / n)
        ILs = {s: il(i, s, v) for s, v in row.items()}
        nIL = [v[0] ** 2 / ILs[s] for s, v in row.items() if ILs[s] == ILs[s]]
        nPL = [v[0] ** 2 / v[1] for v in row.values() if v[1] == v[1]]
        sIL = [math.sqrt(x) for x in ILs.values() if x == x]
        print(f'{i + 1:5d} {pv} {n:3d}  {rms:6.3f}  {mu:+6.3f}  {rdm:6.3f}  {f(nIL):8.2f}  {f(nPL):8.2f}  {1000 * f(sIL):7.1f}')
    if persat:
        with open(persat, 'w', newline='') as fo:
            w = csv.writer(fo)
            w.writerow(['ep', 'tow', 'pivot', 'sat', 'e_m', 'PL_m2', 'IL_m2'] + (['IL_restart_m2'] if RB is not None else []))
            for i, (t, pv, row) in enumerate(S):
                for s, (e, PL, IL) in sorted(row.items()):
                    extra = [f'{RB.get((i, s), float("nan")):.6e}'] if RB is not None else []
                    w.writerow([i + 1, f'{t:.3f}', pv, s, f'{e:.6f}', f'{PL:.6e}', f'{IL:.6e}'] + extra)


def cmd_decomp(a):
    W, ds = load_walk(a[0]), cl.datum_share()
    for r in a[1:]:
        d = resolve(r)
        hc, hd, hp, ha = defaultdict(list), defaultdict(list), defaultdict(list), defaultdict(list)
        for i, (t, pv, row) in enumerate(series(d, W, ds)):
            es = [v[0] for v in row.values()]
            if len(es) < 5:
                continue
            mu = sum(es) / len(es); h = i // 120
            hc[h].append(mu * mu)
            hd[h].append(sum((x - mu) ** 2 for x in es) / len(es))
            hp[h].append(sum(x * x for x in es) / len(es))
            # Card SCL2 amendment A2: every satellite, the pivot included (its pivot-frame error is 0).
            ea = es + [0.0]; ma = sum(ea) / len(ea)
            ha[h].append(sum((x - ma) ** 2 for x in ea) / len(ea))
        f = lambda L: ' '.join(f'{math.sqrt(sum(L[h]) / len(L[h])):.3f}' for h in sorted(L))
        print(f'{os.path.basename(d):22s} pivot-frame {f(hp)} | common {f(hc)} | de-commoned {f(hd)} | all-sat {f(ha)}')


def cmd_stated(a):
    ps, run, q = a[0], resolve(a[1]), float(a[2])
    E, EP = defaultdict(dict), {}
    for r in csv.DictReader(open(ps)):
        t = round(float(r['tow'])); E[t][r['sat']] = float(r['e_m']); EP[t] = int(r['ep'])
    P = defaultdict(dict)
    for r in csv.DictReader(open(os.path.join(run, 'logs', 'network_level_innov.csv'))):
        P[round(float(r['tow'])) - 30][r['sat']] = float(r['p_prior_m2']) - q * 30.0
    win = defaultdict(lambda: {'e2': [], 'p': [], 'nees': []})
    for t, row in E.items():
        F = [s for s in row if s in P.get(t, {})]
        if len(F) < 3:
            continue
        mu = sum(row[s] for s in F) / len(F); w = (EP[t] - 1) // 30
        for s in F:
            e = row[s] - mu
            win[w]['e2'].append(e * e); win[w]['p'].append(P[t][s])
            if P[t][s] > 0:
                win[w]['nees'].append(e * e / P[t][s])
    print('==', os.path.basename(run), 'q', q)
    print('   epochs      n  rms_e_dm_mm  sd_stated_mm  mean_NEES  rms/sd')
    for w in sorted(win):
        d = win[w]
        if not d['e2'] or not d['nees']:
            continue
        r = math.sqrt(sum(d['e2']) / len(d['e2'])); sp = math.sqrt(max(0.0, st.median(d['p'])))
        print(f'{30 * w + 1:4d}-{30 * w + 30:4d} {len(d["e2"]):6d}  {1000 * r:10.1f}  {1000 * sp:11.1f}  '
              f'{sum(d["nees"]) / len(d["nees"]):9.2f}  {r / sp if sp > 0 else float("nan"):6.2f}')


if __name__ == '__main__':
    if len(sys.argv) < 3 or sys.argv[1] not in ('series', 'decomp', 'stated'):
        raise SystemExit(__doc__)
    {'series': cmd_series, 'decomp': cmd_decomp, 'stated': cmd_stated}[sys.argv[1]](sys.argv[2:])

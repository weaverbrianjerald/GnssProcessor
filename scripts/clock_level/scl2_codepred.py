#!/usr/bin/env python3
"""Card SCL2 section 12: the sim level graded against the level that follows the fixture's own code noise.

    python scripts/clock_level/scl2_codepred.py noise <fixture dir> <out.pkl> [--jobs 4] [--tow T0,T1]
    python scripts/clock_level/scl2_codepred.py rows  <run> <out.pkl>
    python scripts/clock_level/scl2_codepred.py pred  <noise.pkl> <rows.pkl> <out.pkl> [--mirror G20,G12]
    python scripts/clock_level/scl2_codepred.py grade <pred.pkl> <run> <walk_csv|none> [--hours 3,4,5,6] [--sats G20,G12]

noise  The exact IF(C1C, C2W) code-noise realization of a sim fixture, per (tow, station, GPS satellite), from each
       station directory's RINEX and truth files (truth_ambiguities, truth_bias, truth_rx_bias, truth_windup,
       truth_phase_colour).  From the simulator's observation equations (src/sim/main.cpp, the row.values lines),
       with IF(x) = A x1 - B x2 (range, troposphere, antenna and first-order ionosphere cancel):
           n_IF = IF(C) - IF(lam L) - IF(osbC - osbL) - IF(rxbC - rxbL) + wc IF(lam) + IF(lam N) + pc
       Columns tow, station, sat, n_if (m).  One process per station (--jobs, default 4); --tow keeps a TOW window
       (default the whole file).  The RINEX epoch becomes GPS seconds of week from its calendar date.
rows   The producer's used code rows of a run (logs/network_pf_rows.csv: GPS, C1C and C2W both present, weight
       w >= 1e-2) with the IF post-fit residual.  The run's first emitted epoch and its interval (products.txt E
       records) travel with the file and fix the epoch numbering of pred and grade.
pred   The code-following prediction; reads no truth level.  Phase connects each satellite's clock within one
       visibility segment (a new segment after a gap above 10 epochs in the satellite's used rows, i.e. a
       readmission).  Each code row reads n_ks(t) = alpha_k - beta_s(seg) + noise, alpha a constant per station
       and beta a constant per satellite segment, solved by weighted least squares (the producer's row weights)
       over all rows up to epoch t, every 10 epochs, gauged by sum(beta) = 0 over the live segments.  P2 = beta_hat,
       the error of a level that follows its code exactly (an undecayed exact filter); sd2 its code-only bound in
       row-weight units; P1 = minus the satellite's own weighted running mean of n since its segment start,
       de-commoned.  --mirror negates the named satellites' noise (the prediction for a mirrored-code arm).
       Writes ep, tow, sat, seg, p1, p2, sd2 and prints P2 (P1) [code-only sd = 3 sd2] by hour, in mm (the
       prototype's pred_<key>.txt printed sd2 itself under that label).
grade  The run's truth level error e (scl2_level.py series: scl_grade.py's definition, pivot frame, pivot e = 0)
       de-commoned per epoch over every satellite, the pivot included (amendment A2): d_e.  P2 is re-commoned over
       the graded set at the prediction's epochs.  Hourly means per satellite over the chosen hours (default 3 to
       6; hour k = epochs 120(k-1)+1 .. 120k from the first emitted epoch): the number of satellite-hours, rms d_e,
       rms P2, corr(d_e, P2), the slope of d_e on P2, rms(d_e - P2) and the mean z^2 of d_e and of d_e - P2 against
       the code-only sd = 3 sd2 (the IF row information is w / 8.9 on simnet60_quiet_col_24h; pred prints the
       fixture's w var(n) as the check).  --sats prints those satellites' hourly d_e, P2 and sd.

Card SCL2 section 12 reproduces with this script (fixture runs/simnet60_quiet_col_24h, walk none, hours 3 to 6):
corr 0.911 / 0.919, slope 0.71, rms d_e 19.0 / 18.8 mm, rms(d_e - P2) 10.5 / 10.2 mm on _rap_scl2g_ck1_s / ck2_s,
and corr 0.42, rms(d_e - P2) 40.8 mm on _rap_scl2d_d2_s.  The truth level uses clklevel.datum_share(), which reads
the truth_bias.csv of runs/simnet60_quiet_wu93_24h (its GPS rows are those of simnet60_quiet_col_24h).
"""
import csv, datetime, glob, importlib.util, os, sys
from concurrent.futures import ProcessPoolExecutor
import numpy as np
import pandas as pd

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
F1, F2 = 1575.42, 1227.60
A = F1 * F1 / (F1 * F1 - F2 * F2)
B = F2 * F2 / (F1 * F1 - F2 * F2)
GPS0 = datetime.date(1980, 1, 6)
W_MIN = 1e-2        # rows below this weight are not used by the producer
GAP_EP = 10         # a gap above this many epochs starts a new satellite segment (a readmission)
SOLVE_EVERY = 10    # pred solves at every 10th epoch and at epochs 1 to 4
SD_SCALE = 3.0      # code-only sd = 3 sd2: the IF row information is w / 8.9 on this fixture


def _sl():
    """scl2_level.py as a module (it loads clklevel.py itself)."""
    spec = importlib.util.spec_from_file_location('scl2_level', ROOT + '/scripts/clock_level/scl2_level.py')
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    return m


def _opt(a, name, default=None):
    return a[a.index(name) + 1] if name in a else default


# ---------------------------------------------------------------------------------------------- noise
def _rinex_gps_cols(f):
    """Column indices of C1C, L1C, C2W, L2W in the GPS records; consumes the header."""
    obs, cur = [], None
    for line in f:
        if line[60:79].strip() == 'SYS / # / OBS TYPES':
            cur = line[0] if line[0] != ' ' else cur    # a blank system letter continues the previous line
            if cur == 'G':
                obs += line[7:58].split()
        if 'END OF HEADER' in line:
            break
    try:
        return [obs.index(c) for c in ('C1C', 'L1C', 'C2W', 'L2W')]
    except ValueError:
        raise SystemExit(f'GPS obs types {obs} lack one of C1C L1C C2W L2W')


def noise_station(job):
    """[(tow, station, sat, n_if)] for one station directory."""
    d, win = job
    sta = os.path.basename(d)
    t0, t1 = win if win else (-np.inf, np.inf)
    rnx = sorted(glob.glob(f'{d}/*.rnx'))[0]
    amb = {}; lam = {}
    for r in csv.DictReader(open(f'{d}/truth_ambiguities.csv')):
        amb[(r['satellite'], r['signal'])] = int(r['N_int']); lam[(r['satellite'], r['signal'])] = float(r['wavelength_m'])
    osb = {(r['satellite'], r['signal']): float(r['osb_m']) for r in csv.DictReader(open(f'{d}/truth_bias.csv'))}
    rxb = {(r['kind'], r['code']): float(r['value_m']) for r in csv.DictReader(open(f'{d}/truth_rx_bias.csv'))}
    wu = pd.read_csv(f'{d}/truth_windup.csv')
    wu = wu[(wu.tow_s >= t0) & (wu.tow_s <= t1) & wu.satellite.str.startswith('G')]
    wu = {(round(t, 3), s): w for t, s, w in zip(wu.tow_s, wu.satellite, wu.windup_cycles)}
    pc = pd.read_csv(f'{d}/truth_phase_colour.csv')
    pc = pc[(pc.tow_s >= t0) & (pc.tow_s <= t1) & pc.satellite.str.startswith('G')]
    pc = {(round(t, 3), s): e for t, s, e in zip(pc.tow_s, pc.satellite, pc.e_m)}
    rx = (A * (rxb.get(('code', 'C1C'), 0.0) - rxb.get(('phase', 'L1C'), 0.0))
          - B * (rxb.get(('code', 'C2W'), 0.0) - rxb.get(('phase', 'L2W'), 0.0)))
    out = []
    tow = None
    with open(rnx) as f:
        ic1, il1, ic2, il2 = _rinex_gps_cols(f)
        for line in f:
            if line[0] == '>':
                y, mo, dd = int(line[2:6]), int(line[7:9]), int(line[10:12])
                hh, mm, ss = int(line[13:15]), int(line[16:18]), float(line[19:29])
                day = float(((datetime.date(y, mo, dd) - GPS0).days % 7) * 86400)
                tow = round(day + hh * 3600 + mm * 60 + ss, 3)
                continue
            if tow is None or tow < t0 or tow > t1 or line[0] != 'G':
                continue
            sat = line[0:3]
            try:
                c1, l1, c2, l2 = (float(line[3 + 16 * i: 3 + 16 * i + 14]) for i in (ic1, il1, ic2, il2))
            except ValueError:
                continue
            la1, la2 = lam[(sat, 'L1C')], lam[(sat, 'L2W')]
            n = (A * c1 - B * c2) - (A * la1 * l1 - B * la2 * l2) \
                - (A * (osb[(sat, 'C1C')] - osb[(sat, 'L1C')]) - B * (osb[(sat, 'C2W')] - osb[(sat, 'L2W')])) \
                - rx \
                + wu.get((tow, sat), np.nan) * (A * la1 - B * la2) \
                + (A * la1 * amb[(sat, 'L1C')] - B * la2 * amb[(sat, 'L2W')]) \
                + pc.get((tow, sat), 0.0)
            out.append((tow, sta, sat, n))
    return out


def cmd_noise(a):
    fix, out = os.path.abspath(a[0]).replace(os.sep, '/'), a[1]
    jobs = int(_opt(a, '--jobs', 4))
    win = tuple(float(x) for x in _opt(a, '--tow').split(',')) if '--tow' in a else None
    stas = sorted((p for p in glob.glob(f'{fix}/*') if os.path.isdir(p)
                   and os.path.exists(f'{p}/truth_ambiguities.csv') and glob.glob(f'{p}/*.rnx')),
                  key=os.path.basename)
    if not stas:
        raise SystemExit('no station directories (truth_ambiguities.csv + *.rnx) under ' + fix)
    rows = []
    with ProcessPoolExecutor(jobs) as ex:
        for r in ex.map(noise_station, [(s, win) for s in stas]):
            rows += r
    df = pd.DataFrame(rows, columns=['tow', 'station', 'sat', 'n_if'])
    df.attrs.update({'fixture': fix, 'tow_window': list(win) if win else None})
    df.to_pickle(out)
    print(f'{len(df)} rows, {df.station.nunique()} stations, tow {df.tow.min():.0f} .. {df.tow.max():.0f}, '
          f'n_if mean {df.n_if.mean():+.4f} sd {df.n_if.std():.4f} m, NaN {int(df.n_if.isna().sum())} -> {out}')


# ---------------------------------------------------------------------------------------------- rows
def emitted_epochs(d):
    """(first emitted tow, interval) from the E records of products.txt (tows rounded as clklevel.clk does)."""
    tows = []
    with open(os.path.join(d, 'products.txt'), errors='replace') as f:
        for line in f:
            if line[0] == 'E':
                tows.append(round(float(line.split(',')[2]), 3))
    if len(tows) < 2:
        raise SystemExit('fewer than two emitted epochs in ' + d)
    return tows[0], float(np.median(np.diff(tows)))


def cmd_rows(a):
    d, out = _sl().resolve(a[0]), a[1]
    t0, dt = emitted_epochs(d)
    df = pd.read_csv(os.path.join(d, 'logs', 'network_pf_rows.csv'),
                     usecols=['tow', 'station', 'sat', 'signal', 'kind', 'resid_m', 'w', 'elev_deg'])
    df = df[(df.kind == 'code') & df.sat.str.startswith('G') & (df.w >= W_MIN)]
    c1 = df[df.signal == 'C1C'].set_index(['tow', 'station', 'sat'])
    c2 = df[df.signal == 'C2W'].set_index(['tow', 'station', 'sat'])
    j = c1[['resid_m', 'w', 'elev_deg']].join(c2[['resid_m', 'w']], rsuffix='2', how='inner')
    j['if'] = A * j.resid_m - B * j.resid_m2
    j = j.reset_index()
    j.attrs.update({'run': os.path.abspath(d).replace(os.sep, '/'), 't0': t0, 'dt': dt})
    j.to_pickle(out)
    print(f'{len(j)} IF code rows, {j.station.nunique()} stations, {j.sat.nunique()} satellites; '
          f'first emitted tow {t0:.3f}, interval {dt:g} s -> {out}')


# ---------------------------------------------------------------------------------------------- pred
def cmd_pred(a):
    n = pd.read_pickle(a[0]); r = pd.read_pickle(a[1]); out = a[2]
    mirror = _opt(a, '--mirror').split(',') if '--mirror' in a else []
    t0, dt = r.attrs['t0'], r.attrs['dt']
    hep = int(round(3600.0 / dt))
    n['tow'] = n.tow.round(3); r['tow'] = r.tow.round(3)
    m = r[['tow', 'station', 'sat', 'w']].merge(n, on=['tow', 'station', 'sat'])
    m['ep'] = ((m.tow - t0) / dt).round().astype(int) + 1
    m = m[m.ep >= 1]
    bad = int(m.n_if.isna().sum())
    if bad:
        m = m[m.n_if.notna()]
    print(f'{len(m)} of {len(r)} used rows matched to the noise ({bad} without a wind-up truth dropped); '
          f'mean w n^2 = {(m.w * m.n_if ** 2).mean():.2f} (grade assumes {SD_SCALE ** 2:g})')
    m.loc[m.sat.isin(mirror), 'n_if'] *= -1.0
    # segments per satellite from its used epochs
    seg = {}
    for s, d in m.groupby('sat'):
        eps = np.sort(d.ep.unique())
        brk = np.concatenate([[0], np.cumsum(np.diff(eps) > GAP_EP)])
        seg.update({(s, e): f'{s}#{b}' for e, b in zip(eps, brk)})
    m['seg'] = [seg[(s, e)] for s, e in zip(m.sat, m.ep)]
    stas = sorted(m.station.unique()); segs = sorted(m.seg.unique())
    ia = {k: i for i, k in enumerate(stas)}; ib = {k: len(stas) + i for i, k in enumerate(segs)}
    dim = len(stas) + len(segs)
    N = np.zeros((dim, dim)); b = np.zeros(dim)
    S1 = {}; W1 = {}
    rows = []
    for ep, d in m.groupby('ep'):
        for k, sg, w, v in zip(d.station, d.seg, d.w, d.n_if):
            i, j = ia[k], ib[sg]
            N[i, i] += w; N[j, j] += w; N[i, j] -= w; N[j, i] -= w
            b[i] += w * v; b[j] -= w * v
            S1[sg] = S1.get(sg, 0.0) + w * v; W1[sg] = W1.get(sg, 0.0) + w
        if not (ep % SOLVE_EVERY == 0 or ep < 5):
            continue
        live = sorted(d.seg.unique())
        # gauge by a Lagrange row: the live segments' betas sum to zero
        act = [i for i in range(dim) if N[i, i] > 0]
        idx = {v: q for q, v in enumerate(act)}
        g = np.zeros(len(act))
        for sg in live:
            g[idx[ib[sg]]] = 1.0
        K = np.block([[N[np.ix_(act, act)], g[:, None]], [g[None, :], np.zeros((1, 1))]])
        sol = np.linalg.lstsq(K, np.concatenate([b[act], [0.0]]), rcond=None)[0]
        Kinv = np.linalg.pinv(K)
        p1raw = {sg: -S1[sg] / W1[sg] for sg in live}
        mu1 = np.mean(list(p1raw.values()))
        for sg in live:
            q = idx[ib[sg]]
            rows.append((ep, t0 + dt * (ep - 1), sg.split('#')[0], sg, p1raw[sg] - mu1, sol[q], np.sqrt(max(Kinv[q, q], 0.0))))
    P = pd.DataFrame(rows, columns=['ep', 'tow', 'sat', 'seg', 'p1', 'p2', 'sd2'])
    P.attrs.update({'t0': t0, 'dt': dt, 'run': r.attrs.get('run'), 'mirror': mirror})
    P.to_pickle(out)
    P['h'] = (P.ep - 1) // hep + 1
    H = P.groupby(['sat', 'h']).agg(p1=('p1', 'mean'), p2=('p2', 'mean'), sd2=('sd2', 'mean')).reset_index()
    print(f'code-following de-commoned level by hour (mm): P2 (P1) [code-only sd], mirror {",".join(mirror) or "none"}, '
          f'no truth level read -> {out}')
    for s, d in H.groupby('sat'):
        print(s + ' ' + ' '.join(f'{1e3 * x:+6.1f}({1e3 * y:+6.1f})[{1e3 * SD_SCALE * z:5.1f}]'
                                 for x, y, z in zip(d.p2, d.p1, d.sd2)))


# ---------------------------------------------------------------------------------------------- grade
def truth(sl, d, walk, t0, dt):
    """Per (ep, sat): the pivot-frame level error e, the pivot's row (e = 0) included; ep counted from t0."""
    S = sl.series(d, sl.load_walk(walk), sl.cl.datum_share())
    if S and abs(S[0][0] - t0) > 1e-3:
        print(f'warning: the run first emits at tow {S[0][0]:.3f}, the prediction counts epochs from {t0:.3f}')
    out = []
    for t, pv, row in S:
        if not row:
            continue
        ep = int(round((t - t0) / dt)) + 1
        out += [(ep, t, pv, s, e) for s, (e, _, _) in sorted(row.items())]
        out.append((ep, t, pv, pv, 0.0))
    return pd.DataFrame(out, columns=['ep', 'tow', 'pivot', 'sat', 'e_m'])


def cmd_grade(a):
    P = pd.read_pickle(a[0]); sl = _sl(); d = sl.resolve(a[1]); walk = a[2]
    hours = [int(x) for x in _opt(a, '--hours', '3,4,5,6').split(',')]
    sats = _opt(a, '--sats').split(',') if '--sats' in a else []
    t0, dt = P.attrs['t0'], P.attrs['dt']
    hep = int(round(3600.0 / dt))
    if P.attrs.get('run') and os.path.normcase(os.path.abspath(P.attrs['run'])) != os.path.normcase(os.path.abspath(d)):
        print(f'note: the prediction was built from the used rows of {P.attrs["run"]}')
    t = truth(sl, d, walk, t0, dt)
    t = t[t.ep.isin(P.ep.unique())].copy()
    t['d_e'] = t.e_m - t.groupby('ep').e_m.transform('mean')
    m = t.merge(P[['ep', 'sat', 'p1', 'p2', 'sd2']], on=['ep', 'sat'], how='inner')
    m['p2'] = m.p2 - m.groupby('ep').p2.transform('mean')    # re-common over exactly the graded set
    m['sd'] = SD_SCALE * m.sd2
    m['h'] = (m.ep - 1) // hep + 1
    H = m.groupby(['sat', 'h']).agg(de=('d_e', 'mean'), p2=('p2', 'mean'), sd=('sd', 'mean')).reset_index()
    x = H[H.h.isin(hours)]
    if len(x) < 3:
        raise SystemExit(f'{len(x)} satellite-hours in hours {hours}')
    c = np.corrcoef(x.de, x.p2)[0, 1]; slope = np.polyfit(x.p2, x.de, 1)[0]
    rms = lambda v: 1e3 * np.sqrt((v ** 2).mean())
    print('run                     hours    n_sat-hr  rms_d_e  rms_P2  corr(d_e,P2)  slope  rms(d_e-P2)  z2(d_e)  z2(d_e-P2)'
          '   [mm; hourly means per satellite]')
    print(f'{os.path.basename(d):22s}  h{"".join(map(str, hours)):7s}  {len(x):8d}  {rms(x.de):7.1f}  {rms(x.p2):6.1f}  '
          f'{c:12.3f}  {slope:5.2f}  {rms(x.de - x.p2):11.1f}  {((x.de / x.sd) ** 2).mean():7.2f}  '
          f'{(((x.de - x.p2) / x.sd) ** 2).mean():10.2f}')
    for s in sats:
        y = H[H.sat == s]
        if y.empty:
            continue
        f = lambda col, fmt: ' '.join(fmt.format(1e3 * v) for v in y[col])
        print(f'   {s} d_e {f("de", "{:+6.1f}")}   P2 {f("p2", "{:+6.1f}")}   sd {f("sd", "{:4.1f}")}  '
              f'hours {"".join(str(h) for h in y.h)}')


if __name__ == '__main__':
    cmds = {'noise': (cmd_noise, 2), 'rows': (cmd_rows, 2), 'pred': (cmd_pred, 3), 'grade': (cmd_grade, 3)}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds or len([x for x in sys.argv[2:] if not x.startswith('--')]) < cmds[sys.argv[1]][1]:
        raise SystemExit(__doc__)
    cmds[sys.argv[1]][0](sys.argv[2:])

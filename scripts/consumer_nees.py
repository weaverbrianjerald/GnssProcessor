#!/usr/bin/env python3
"""Formal-against-actual consistency of the closed-loop consumer's position, per hour, from
logs/closed_loop_trace.csv (the columns east_m,north_m,up_m and sig_e_m,sig_n_m,sig_u_m).

    python scripts/consumer_nees.py <run_dir> [--from-s S]
    python scripts/consumer_nees.py --selftest

Per hour (counted from the consumer's first row, or from --from-s seconds after it) and overall:
  z2_e, z2_n, z2_u   the mean of (e_i / sigma_i)^2 per axis (1 under consistency)
  nees               the mean of the diagonal NEES, z2_e + z2_n + z2_u per row (3 under consistency;
                     E[e_i^2] = sigma_i^2 per axis whatever the cross-correlations, so the diagonal
                     form keeps the mean of the full NEES; the trace carries no cross-covariance)
  r_e, r_n, r_u      rms(e_i) / median(sigma_i), the ratio of actual to formal error per axis
  sig_*_med          the median formal sigma per axis (m)
  reading            by the largest and the smallest r of the hour: `consistent` when every axis has
                     0.5 <= r <= 2, `over-confident` when an axis has r > 2 (formal sigma too small),
                     `pessimistic` when an axis has r < 0.5 and none exceeds 2

The error is against the configured coordinate, so a coordinate or frame bias enters it.  Rows with a
zero sigma (a covariance that has not been built) are skipped.  The errors are time-correlated, so the
epoch count does not give a chi-square interval; the reading is a convention, frozen in card VIN.

gcensus.py (`grade ... --nees`) calls grade() from this module, so the statistic has one owner.
"""
import argparse, csv, math, os, statistics as st, sys

AXES = ('e', 'n', 'u')
ERR_COLS = {'e': 'east_m', 'n': 'north_m', 'u': 'up_m'}
SIG_COLS = {'e': 'sig_e_m', 'n': 'sig_n_m', 'u': 'sig_u_m'}
R_LO, R_HI = 0.5, 2.0


def read_rows(path, from_s=0.0):
    """[(hour, {axis: (err, sigma)})] from a trace; hours counted from the first row plus from_s.
    Returns None when the trace has no sigma columns (a pre-VIN executable)."""
    out = []
    t0 = None
    with open(path, encoding='utf-8', errors='replace') as fh:
        rd = csv.DictReader(l for l in fh if not l.startswith('#'))
        if not rd.fieldnames or any(c not in rd.fieldnames for c in SIG_COLS.values()):
            return None
        for r in rd:
            try:
                t = int(r['week']) * 604800.0 + float(r['tow'])
                v = {a: (float(r[ERR_COLS[a]]), float(r[SIG_COLS[a]])) for a in AXES}
            except (KeyError, TypeError, ValueError):
                continue
            if t0 is None:
                t0 = t
            el = t - t0 - from_s
            if el < 0 or any(s <= 0.0 for _, s in v.values()):
                continue
            out.append((int(round(el * 1000)) // 3600000, v))
    return out


def stats(rows):
    """The statistics of one group of rows (see the module docstring)."""
    if not rows:
        return None
    z2 = {a: st.fmean((v[a][0] / v[a][1]) ** 2 for _, v in rows) for a in AXES}
    nees = st.fmean(sum((v[a][0] / v[a][1]) ** 2 for a in AXES) for _, v in rows)
    sig = {a: st.median(v[a][1] for _, v in rows) for a in AXES}
    rms = {a: math.sqrt(st.fmean(v[a][0] ** 2 for _, v in rows)) for a in AXES}
    r = {a: rms[a] / sig[a] for a in AXES}
    if max(r.values()) > R_HI:
        reading = 'over-confident'
    elif min(r.values()) < R_LO:
        reading = 'pessimistic'
    else:
        reading = 'consistent'
    return {'n': len(rows), 'z2': z2, 'nees': nees, 'sig': sig, 'rms': rms, 'r': r, 'reading': reading}


def grade(run_dir, from_s=0.0):
    """{'hours': {h: stats}, 'all': stats} for one run directory, or None (no trace or no sigma)."""
    p = os.path.join(run_dir, 'logs', 'closed_loop_trace.csv')
    if not os.path.isfile(p):
        return None
    rows = read_rows(p, from_s)
    if rows is None:
        return None
    hours = {}
    for h, v in rows:
        hours.setdefault(h, []).append((h, v))
    return {'hours': {h: stats(g) for h, g in sorted(hours.items())}, 'all': stats(rows)}


def print_grade(label, g):
    print(f'consumer NEES: {label}')
    if g is None:
        print('  no closed_loop_trace.csv with sigma columns')
        return
    print(f'  {"hour":>4s} {"rows":>5s} {"z2_e":>8s} {"z2_n":>8s} {"z2_u":>8s} {"nees":>8s} '
          f'{"r_e":>6s} {"r_n":>6s} {"r_u":>6s} {"sig_e":>7s} {"sig_n":>7s} {"sig_u":>7s}  reading')
    items = [(str(h + 1), s) for h, s in g['hours'].items()] + [('all', g['all'])]
    for lab, s in items:
        if s is None:
            continue
        print(f'  {lab:>4s} {s["n"]:5d} ' + ' '.join(f'{s["z2"][a]:8.2f}' for a in AXES)
              + f' {s["nees"]:8.2f} ' + ' '.join(f'{s["r"][a]:6.2f}' for a in AXES) + ' '
              + ' '.join(f'{s["sig"][a]:7.4f}' for a in AXES) + f'  {s["reading"]}')


def selftest():
    import random, tempfile
    ok = True

    def check(name, cond):
        nonlocal ok
        print(f'  {"PASS" if cond else "FAIL"}  {name}')
        ok &= bool(cond)

    def write(d, scale):
        """2 h at 30 s; errors drawn with sigma (0.02, 0.03, 0.06) m, written sigma times `scale`."""
        os.makedirs(os.path.join(d, 'logs'), exist_ok=True)
        rng = random.Random(7)
        sig = (0.02, 0.03, 0.06)
        with open(os.path.join(d, 'logs', 'closed_loop_trace.csv'), 'w') as f:
            f.write('week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,ratio,pos_err_3d_m,'
                    'east_m,north_m,up_m,ar_gate,p_succ,sig_e_m,sig_n_m,sig_u_m,sig_3d_m\n')
            for i in range(240):
                e = [rng.gauss(0.0, s) for s in sig]
                f.write(f'2313,{259200 + 30 * i:.3f},20,0,0,0,0,0.00,{math.sqrt(sum(x * x for x in e)):.4f},'
                        + ','.join(f'{x:.6f}' for x in e) + ',not_run,0.000000,'
                        + ','.join(f'{s * scale:.6f}' for s in sig) + f',{math.sqrt(sum(s * s for s in sig)) * scale:.6f}\n')

    with tempfile.TemporaryDirectory() as td:
        write(os.path.join(td, 'ok'), 1.0)
        g = grade(os.path.join(td, 'ok'))
        check('two hours graded', sorted(g['hours']) == [0, 1])
        check('consistent trace: NEES within 3 +- 0.6', abs(g['all']['nees'] - 3.0) < 0.6)
        check('consistent trace: every ratio within 1 +- 0.15', all(abs(g['all']['r'][a] - 1.0) < 0.15 for a in AXES))
        check('consistent trace reads consistent', g['all']['reading'] == 'consistent')
        write(os.path.join(td, 'over'), 0.4)
        g = grade(os.path.join(td, 'over'))
        check('sigma x0.4: ratios about 2.5', all(abs(g['all']['r'][a] - 2.5) < 0.4 for a in AXES))
        check('sigma x0.4: NEES about 18.75', abs(g['all']['nees'] - 18.75) < 4.0)
        check('sigma x0.4 reads over-confident', g['all']['reading'] == 'over-confident')
        write(os.path.join(td, 'pess'), 3.0)
        check('sigma x3 reads pessimistic', grade(os.path.join(td, 'pess'))['all']['reading'] == 'pessimistic')
        # A pre-VIN trace (no sigma columns) is reported, not mis-graded.
        os.makedirs(os.path.join(td, 'old', 'logs'))
        with open(os.path.join(td, 'old', 'logs', 'closed_loop_trace.csv'), 'w') as f:
            f.write('week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,ratio,pos_err_3d_m,'
                    'east_m,north_m,up_m,ar_gate,p_succ\n2313,259200.000,20,0,0,0,0,0.00,1,0,0,1,not_run,0\n')
        check('a trace without sigma columns grades None', grade(os.path.join(td, 'old')) is None)
    print('selftest', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('run_dir', nargs='?')
    ap.add_argument('--from-s', type=float, default=0.0,
                    help='start the hour clock this many seconds after the first row')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.run_dir:
        ap.error('a run directory is required')
    g = grade(a.run_dir, a.from_s)
    print_grade(a.run_dir, g)
    return 0 if g else 1


if __name__ == '__main__':
    sys.exit(main())

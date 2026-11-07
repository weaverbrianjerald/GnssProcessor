#!/usr/bin/env python3
"""Convergence grade of the closed-loop consumer from logs/closed_loop_trace.csv.

    python scripts/convergence_grade.py <target> [--convention span|count] [--run-s S] [--sites a,b,...]
    python scripts/convergence_grade.py --selftest

<target> is one of
  - a run directory holding logs/closed_loop_trace.csv (one consumer);
  - a census prefix, e.g. D:/GnssProcessorSupport/runs_nom/_rap_bofc_ctl, whose replays are the
    directories <prefix>_<site> (the gcensus.py naming);
  - a directory whose subdirectories are run directories.

Per consumer, every time is counted from the consumer's first row (its cold start), in minutes:
  t01      the first epoch with pos_err_3d_m < 0.10 m
  run01    the start of the first 10-minute run under 0.10 m
  run005   the start of the first 10-minute run under 0.05 m
  p50/p90  the 3D error pooled over hours 2 to 6 of the run's own clock (rows with elapsed time in
           [3600 s, 21600 s)), in metres
A run is consecutive rows under the threshold with no row gap above 1.5 cadences (the median row
spacing).  Its length: `span` (default), the last row's time minus the first's is at least --run-s
(600 s: 21 rows at 30 s); `count`, rows times the cadence is at least --run-s (20 rows at 30 s).
A consumer that never meets a criterion reads `never` and counts as +inf in the census percentiles.

Over a census: per site, then the median and the nearest-rank p90 (the ceil(0.9 n)-th smallest)
across the census sites, with the held-out user (the site `wtzz`, or --headline) listed beside and
not pooled.  The times are joint producer-ignition plus user-convergence times when the producer
starts with the consumer (a live harness run); a replay of a recorded wire from its start has the
same joint meaning, since the wire's first hour is the producer's ignition.

gcensus.py (`grade ... --convergence`) calls converge() and census_stats() from this module, so the
statistic has one owner.
"""
import argparse, csv, glob, math, os, statistics as st, sys

THRESH_FIRST = 0.10
THRESH_RUNS = (0.10, 0.05)
RUN_S = 600.0
SETTLED = (3600.0, 21600.0)


def read_trace(path):
    """(elapsed_s, err3d_m) rows of a closed_loop_trace.csv, in file order, from its first row."""
    rows = []
    with open(path, encoding='utf-8', errors='replace') as fh:
        for r in csv.DictReader(l for l in fh if not l.startswith('#')):
            try:
                t = int(r['week']) * 604800.0 + float(r['tow'])
                e = float(r['pos_err_3d_m'])
            except (KeyError, TypeError, ValueError):
                continue
            rows.append((t, e))
    if not rows:
        return []
    t0 = rows[0][0]
    return [(t - t0, e) for t, e in rows]


def cadence(rows):
    """Median row spacing (s); 30 s when the trace has fewer than two rows."""
    d = [b[0] - a[0] for a, b in zip(rows, rows[1:]) if b[0] > a[0]]
    return st.median(d) if d else 30.0


def first_run(rows, thresh, run_s=RUN_S, convention='span'):
    """Elapsed time (s) of the first row of the first run under `thresh` long enough by `convention`,
    or None."""
    dt = cadence(rows)
    start = None; n = 0; prev = None
    for t, e in rows:
        if e < thresh and (start is None or (prev is not None and t - prev <= 1.5 * dt)):
            if start is None:
                start, n = t, 0
            n += 1
        elif e < thresh:            # under the threshold after a gap: a new run
            start, n = t, 1
        else:
            start, n = None, 0
        prev = t
        if start is not None:
            long_enough = (t - start >= run_s - 1e-6) if convention == 'span' else (n * dt >= run_s - 1e-6)
            if long_enough:
                return start
    return None


def nearest_rank(vals, q):
    """Nearest-rank percentile (the ceil(q n)-th smallest); +inf entries sort last."""
    v = sorted(vals)
    if not v:
        return float('nan')
    k = max(1, math.ceil(q * len(v)))
    return v[k - 1]


def median(vals):
    """Median that tolerates +inf (the mean of the two middle values, inf when either is inf)."""
    v = sorted(vals)
    if not v:
        return float('nan')
    m = len(v) // 2
    if len(v) % 2:
        return v[m]
    a, b = v[m - 1], v[m]
    return float('inf') if math.isinf(a) or math.isinf(b) else 0.5 * (a + b)


def converge(path, run_s=RUN_S, convention='span'):
    """The convergence grade of one trace: a dict of minutes (None = never) and settled metres."""
    rows = read_trace(path)
    if not rows:
        return None
    t01 = next((t for t, e in rows if e < THRESH_FIRST), None)
    runs = {th: first_run(rows, th, run_s, convention) for th in THRESH_RUNS}
    settled = [e for t, e in rows if SETTLED[0] <= t < SETTLED[1]]
    q = (lambda p: nearest_rank(settled, p)) if settled else (lambda p: float('nan'))
    mins = lambda s: None if s is None else s / 60.0
    return {'n': len(rows), 'span_h': rows[-1][0] / 3600.0, 'cadence_s': cadence(rows),
            't01': mins(t01), 'run01': mins(runs[0.10]), 'run005': mins(runs[0.05]),
            'p50': median(settled) if settled else float('nan'), 'p90': q(0.9)}


def census_stats(grades):
    """Median and nearest-rank p90 (and max) across sites of each time, never = +inf; the settled
    p50 and p90 by their median across sites."""
    out = {}
    for k in ('t01', 'run01', 'run005'):
        v = [float('inf') if g[k] is None else g[k] for g in grades]
        out[k] = {'median': median(v), 'p90': nearest_rank(v, 0.9), 'max': max(v) if v else float('nan'),
                  'never': sum(1 for g in grades if g[k] is None)}
    for k in ('p50', 'p90'):
        out[k] = {'median': median([g[k] for g in grades])}
    return out


def fmt_min(x):
    if x is None or (isinstance(x, float) and math.isinf(x)):
        return 'never'
    return f'{x:.1f}'


def discover(target):
    """{site label: trace path} for a run directory, a census prefix or a directory of runs."""
    target = target.rstrip('/\\')
    one = os.path.join(target, 'logs', 'closed_loop_trace.csv')
    if os.path.isfile(one):
        return {os.path.basename(target): one}
    found = {}
    for p in sorted(glob.glob(target + '_*' + '/logs/closed_loop_trace.csv')):
        run = os.path.basename(os.path.dirname(os.path.dirname(p)))
        found[run[len(os.path.basename(target)) + 1:]] = p
    if not found and os.path.isdir(target):
        for p in sorted(glob.glob(os.path.join(target, '*', 'logs', 'closed_loop_trace.csv'))):
            found[os.path.basename(os.path.dirname(os.path.dirname(p)))] = p
    return found


def report(target, run_s=RUN_S, convention='span', headline='wtzz', sites=None):
    traces = discover(target)
    if sites:
        traces = {k: v for k, v in traces.items() if k in sites}
    if not traces:
        print(f'no closed_loop_trace.csv under {target}')
        return 1
    G = {k: converge(p, run_s, convention) for k, p in traces.items()}
    G = {k: g for k, g in G.items() if g}
    print(f'convergence grade: {target} ({len(G)} consumer(s); run length {run_s:.0f} s, convention '
          f'{convention}; times in minutes from the first consumer epoch)')
    print(f'  {"site":10s} {"rows":>5s} {"span_h":>6s} {"t<0.10":>7s} {"run0.10":>8s} {"run0.05":>8s} '
          f'{"p50_h2-6":>9s} {"p90_h2-6":>9s}')
    for k, g in G.items():
        print(f'  {k:10s} {g["n"]:5d} {g["span_h"]:6.2f} {fmt_min(g["t01"]):>7s} {fmt_min(g["run01"]):>8s} '
              f'{fmt_min(g["run005"]):>8s} {g["p50"]:9.4f} {g["p90"]:9.4f}')
    census = [g for k, g in G.items() if k != headline]
    if len(G) > 1 and census:
        s = census_stats(census)
        print(f'  census ({len(census)} sites, {headline} not pooled):')
        for k, lab in (('t01', 'first epoch < 0.10 m'), ('run01', '10-min run < 0.10 m'),
                       ('run005', '10-min run < 0.05 m')):
            print(f'    {lab:22s} median {fmt_min(s[k]["median"]):>6s}  p90 {fmt_min(s[k]["p90"]):>6s}  '
                  f'max {fmt_min(s[k]["max"]):>6s}  never {s[k]["never"]}')
        print(f'    hours 2-6 3D          median of site p50 {s["p50"]["median"]:.4f} m, '
              f'median of site p90 {s["p90"]["median"]:.4f} m')
    return 0


def selftest():
    import tempfile
    ok = True

    def check(name, cond):
        nonlocal ok
        print(f'  {"PASS" if cond else "FAIL"}  {name}')
        ok &= bool(cond)

    def write(rows, d):
        os.makedirs(os.path.join(d, 'logs'), exist_ok=True)
        p = os.path.join(d, 'logs', 'closed_loop_trace.csv')
        with open(p, 'w') as f:
            f.write('week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,ratio,pos_err_3d_m,'
                    'east_m,north_m,up_m,ar_gate,p_succ,sig_e_m,sig_n_m,sig_u_m,sig_3d_m\n')
            for t, e in rows:
                f.write(f'2313,{259200 + t:.3f},20,0,0,0,0,0.00,{e:.4f},0,0,0,not_run,0.000000,0,0,0,0\n')
        return p

    with tempfile.TemporaryDirectory() as td:
        # 6 h at 30 s: 1.0 m for 20 min, 0.08 m to 60 min, 0.04 m after; one excursion to 0.2 m at 70 min.
        rows = []
        for i in range(720):
            t = 30.0 * i
            e = 1.0 if t < 1200 else (0.08 if t < 3600 else 0.04)
            if abs(t - 4200) < 1:
                e = 0.2
            rows.append((t, e))
        p = write(rows, os.path.join(td, '_rap_x_aaaa'))
        g = converge(p)
        check('first epoch under 0.10 m at 20 min', abs(g['t01'] - 20.0) < 1e-9)
        check('10-min run under 0.10 m starts at 20 min', abs(g['run01'] - 20.0) < 1e-9)
        check('10-min run under 0.05 m starts after the excursion (70.5 min)', abs(g['run005'] - 70.5) < 1e-9)
        check('hours 2-6 p50 is 0.04 m', abs(g['p50'] - 0.04) < 1e-12)
        # The two run conventions: a 20-row run (570 s span) passes `count` and fails `span`.
        rows2 = [(30.0 * i, 0.04 if 10 <= i < 30 else 0.5) for i in range(60)]
        p2 = write(rows2, os.path.join(td, 'two'))
        check('20 rows pass the count convention', abs(converge(p2, convention='count')['run005'] - 5.0) < 1e-9)
        check('20 rows fail the span convention', converge(p2, convention='span')['run005'] is None)
        # A gap of three cadences breaks a run.
        rows3 = [(30.0 * i, 0.04) for i in range(15)] + [(30.0 * i, 0.04) for i in range(18, 40)]
        p3 = write(rows3, os.path.join(td, 'three'))
        check('a row gap breaks the run (restart at 9 min)', abs(converge(p3)['run005'] - 9.0) < 1e-9)
        # Never.
        p4 = write([(30.0 * i, 0.5) for i in range(100)], os.path.join(td, '_rap_x_bbbb'))
        check('a consumer that never converges reads None', converge(p4)['t01'] is None)
        # Census statistics with a never.
        s = census_stats([converge(p), converge(p4)])
        check('census median with a never is inf', math.isinf(s['t01']['median']) and s['t01']['never'] == 1)
        check('nearest-rank p90 of 1..10 is 9', nearest_rank(list(range(1, 11)), 0.9) == 9)
        check('discover finds a census prefix', set(discover(os.path.join(td, '_rap_x'))) == {'aaaa', 'bbbb'})
    print('selftest', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('target', nargs='?')
    ap.add_argument('--convention', choices=('span', 'count'), default='span')
    ap.add_argument('--run-s', type=float, default=RUN_S)
    ap.add_argument('--headline', default='wtzz', help='site listed beside the census, not pooled')
    ap.add_argument('--sites', help='comma-separated site labels to keep')
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.target:
        ap.error('a target is required')
    return report(a.target, a.run_s, a.convention, a.headline, a.sites.split(',') if a.sites else None)


if __name__ == '__main__':
    sys.exit(main())

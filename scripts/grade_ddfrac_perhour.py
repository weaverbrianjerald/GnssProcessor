#!/usr/bin/env python3
"""Per-hour + per-bucket DD-step breakdown for the Card-4 D5 acceptance.

Reuses grade_ddfrac's methodology exactly (epoch-adjacent |ΔDD| within a fixed
(cons,signal,sat_i,ref_sat,sta_a,sta_b) series, gap-aware) but buckets the steps
by wall-clock hour (and optional 10-min event buckets) instead of one tail window.

Usage:
    python scripts/grade_ddfrac_perhour.py <run_dir> [bucket_s=3600]

Prints, per bucket: n_steps, median |ΔDD| (cyc/ep), med|frac|.  Also prints the
overall tail(3600s) DD-step and the run-end innov_flags/retaps if present.
"""
import csv, math, sys, os, statistics as st
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from grade_ddfrac import _f, infer_epoch_dt, adjacent_steps  # exact same methodology


def load(run):
    p = os.path.join(run, 'logs', 'network_dd_fractional.csv')
    if not os.path.exists(p):
        sys.exit(f"not found: {p}")
    rows = [r for r in csv.DictReader(open(p))
            if r.get('frac_cyc') not in (None, '') and _f(r.get('tow')) is not None]
    return rows


def steps_in(rows):
    """Epoch-adjacent DD steps over `rows` (grade_ddfrac methodology)."""
    if not rows:
        return []
    series = defaultdict(list)
    for r in rows:
        tw, dc = _f(r.get('tow')), _f(r.get('dd_cyc'))
        if tw is None or dc is None:
            continue
        series[(r['cons'], r['signal'], r['sat_i'], r['ref_sat'],
                r['sta_a'], r['sta_b'])].append((tw, dc))
    dt0 = infer_epoch_dt(_f(r['tow']) for r in rows)
    tow0 = min(_f(r['tow']) for r in rows)
    return adjacent_steps(series, dt0, tow0)


def main():
    run = sys.argv[1]
    bucket = float(sys.argv[2]) if len(sys.argv) > 2 else 3600.0
    rows = load(run)
    tow0 = min(_f(r['tow']) for r in rows)
    tmax = max(_f(r['tow']) for r in rows)
    span_h = (tmax - tow0) / 3600.0
    print(f"run: {os.path.basename(run.rstrip('/'))}  tow0={tow0:.0f} tmax={tmax:.0f} "
          f"span={span_h:.2f}h  n_rows={len(rows)}")

    # Per-bucket step: rows are bucketed first by (tow-tow0)/bucket, then differenced
    # within each bucket, which is an approximation of bucketing the later sample of
    # each adjacent pair by its own tow.  This drops one step per bucket edge, which
    # is negligible against the per-bucket step count.
    nb = int(math.ceil((tmax - tow0) / bucket))
    print(f"  bucket={bucket:.0f}s  ({nb} buckets)")
    for b in range(nb):
        lo, hi = tow0 + b * bucket, tow0 + (b + 1) * bucket
        br = [r for r in rows if lo <= _f(r['tow']) < hi]
        s = steps_in(br)
        fr = [abs(_f(r['frac_cyc'])) for r in br if _f(r['frac_cyc']) is not None]
        med = st.median(s) if s else float('nan')
        mf = st.median(fr) if fr else float('nan')
        label = f"h{b}" if bucket == 3600.0 else f"b{b}[{(hi-tow0)/60:.0f}m]"
        print(f"    {label:>10}  n_step={len(s):5d}  DDstep_med={med:8.3f}  med|f|={mf:.3f}")

    # overall tail(3600) for direct comparison to grade_ddfrac
    tail = [r for r in rows if _f(r['tow']) >= tmax - 3600.0]
    ts = steps_in(tail)
    print(f"  tail(3600s): n_step={len(ts)}  DDstep_med={st.median(ts) if ts else float('nan'):.3f}")

    # run-end innovation counters, if the run.out is alongside
    for cand in ('run.out', os.path.join('logs', 'run.log')):
        p = os.path.join(run, cand)
        if os.path.exists(p):
            for line in open(p, encoding='utf-8', errors='replace'):
                if 'D5 fine-slip innovation gate):' in line:
                    msg = line.strip()[line.find('Network'):]
                    msg = msg.encode('ascii', 'replace').decode('ascii')  # cp1252-safe stdout
                    print('  [D5] ' + msg[:180])
            break


if __name__ == '__main__':
    main()

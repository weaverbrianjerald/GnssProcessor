#!/usr/bin/env python3
"""Grade a network producer run's datum-invariant double-differenced ambiguities.

This is the go/no-go instrument for producer-side (and downstream user) integer
AR: it reads ``<run>/logs/network_dd_fractional.csv`` (the between-station,
between-satellite DD of the float ambiguities per (constellation, signal)) and
reports how tightly those DDs sit on integers (fractional pile-up, circular R)
and how much a fixed DD wanders epoch-to-epoch (the DD step).

Why the DD, not the wire (the Psychas single-realization mandate)
-------------------------------------------------------------------
The emitted phase-bias wire (``network_solution.csv`` PB ``value_m``) is datum-
dependent: the whole per-(constellation,signal) frame drifts with the datum
receiver's fractional phase bias (2-4 cyc/ep of benign common-mode is normal,
not a defect). Grading the raw wire step penalises that benign drift and gives
a misleading "sick" verdict. A double difference cancels both the receiver and
the satellite frame terms, so the DD is invariant to the datum choice, and
grading it measures genuine solve health. Grade the DD here; to look at the
wire directly, use ``grade_wire.py``, which pivot-differences per constellation
first so it, too, judges a datum-invariant quantity.

Tow-jitter and gap robustness
-------------------------------
The DD step is computed between epoch-adjacent samples only. Product tow
carries about 1e-5 s of jitter and the network skips epochs (arc births/deaths,
missed syncs), so a naive ``zip(sorted, sorted[1:])`` would pair samples across
multi-epoch gaps and report a spurious step. The nominal epoch spacing is
inferred, each sample is snapped to an epoch index, and a step counts only
where the index advances by exactly one. For reference the uncorrected
(gap-crossing) median is printed in parentheses so the effect of the fix is
visible.

Usage
-----
    python scripts/grade_ddfrac.py <run_dir> [tail_seconds=3600]

``<run_dir>`` holds ``logs/network_dd_fractional.csv`` (enable
``network.trace_dd_fractional``). ``tail_seconds`` restricts every statistic to
the final window (default 3600 s) so warm-up transients do not dominate.

Reading the numbers
-------------------
* ``mean|f|`` / ``med|f|``: fractional-cycle magnitude; uniform (no integer
  information) is 0.25. AR-grade biases pull this well below 0.25.
* ``P(<0.1)``: fraction of DDs within 0.1 cyc of an integer; uniform is 0.20.
* ``circR``: circular resultant of 2*pi*frac; 0 is uniform, approaching 1 means
  the DDs sit on integers.
* ``DD step med``: median |ΔDD| between adjacent epochs (cyc/ep). A healthy
  producer holds a fixed DD steady (golden lineage about 0.085 cyc/ep); a
  churning chain datum re-levels DDs network-wide and inflates this. A step is
  only differenced within a fixed (…, ref_sat, …) series, so a re-level that
  coincides with a reference/pivot-satellite flip (ref_sat changes) is split
  across two series and not counted: a known blind spot that can under-report
  churn at pivot switches.
* ``calib``: DD step / (sqrt(2)*formal σ), a rough optimism ratio. It is not
  centred on 1: for temporally-independent DD errors |Δ| is half-normal, whose
  median is 0.6745*sqrt(2)*σ, so a well-calibrated filter reads calib about
  0.67; carried DDs are time-correlated, which pushes that baseline lower
  still. Read it relatively: calib well above 1 flags formal-σ optimism, and a
  jump between arms matters more than the absolute value.

Compares only SD/datum-invariant quantities, never the raw wire (see above).
"""
import csv, math, cmath, sys, os, statistics as st
from collections import defaultdict


def _f(x, default=None):
    """float() that returns `default` on blank/malformed cells instead of raising."""
    try:
        return float(x)
    except (TypeError, ValueError):
        return default


def infer_epoch_dt(tows):
    """Nominal epoch spacing: median gap between distinct sorted tows.

    Robust to the ~1e-5 s product-tow jitter (distinct-tow rounding absorbs it)
    and to occasional multi-epoch gaps (single-epoch spacing is the majority, so
    the median of consecutive distinct-tow diffs lands on it)."""
    u = sorted({round(t, 3) for t in tows})
    diffs = [b - a for a, b in zip(u, u[1:]) if b - a > 1e-6]
    return st.median(diffs) if diffs else 1.0


def adjacent_steps(series, dt0, tow0):
    """|Δvalue| between epoch-adjacent samples of each series.

    series: {key: [(tow, value), ...]}. A sample's epoch index is
    round((tow - tow0)/dt0); one value per epoch (last wins); a step counts only
    when the index advances by exactly 1 (never across a gap)."""
    steps = []
    for v in series.values():
        by_e = {}
        for tow, val in v:
            by_e[round((tow - tow0) / dt0)] = val
        es = sorted(by_e)
        steps += [abs(by_e[b] - by_e[a]) for a, b in zip(es, es[1:]) if b - a == 1]
    return steps


def main():
    run = sys.argv[1]
    tail_s = float(sys.argv[2]) if len(sys.argv) > 2 else 3600.0
    p = os.path.join(run, 'logs', 'network_dd_fractional.csv')
    if not os.path.exists(p):
        sys.exit(f"not found: {p}\n(enable network.trace_dd_fractional and re-run)")
    rows = [r for r in csv.DictReader(open(p)) if r.get('frac_cyc') not in (None, '')]
    if not rows:
        sys.exit(f"no usable rows in {p} (enable network.trace_dd_fractional)")
    rows = [r for r in rows if _f(r.get('tow')) is not None]
    tmax = max(_f(r['tow']) for r in rows)
    tail = [r for r in rows if _f(r['tow']) >= tmax - tail_s]
    frv = [x for x in (_f(r.get('frac_cyc')) for r in tail) if x is not None]
    n = len(frv)
    if n == 0:
        sys.exit("no parseable frac_cyc rows in the tail window")
    fr = [abs(x) for x in frv]
    R = abs(sum(cmath.exp(2j * math.pi * x) for x in frv)) / n
    sig = [s for s in (_f(r.get('dd_sigma_cyc')) for r in tail) if s is not None and s > 0]

    # per-series DD step (epoch-adjacent) + reference naive (gap-crossing) step
    series = defaultdict(list)
    for r in tail:
        tw, dc = _f(r.get('tow')), _f(r.get('dd_cyc'))
        if tw is None or dc is None:
            continue  # blank/malformed row (e.g. a truncated tail line)
        series[(r['cons'], r['signal'], r['sat_i'], r['ref_sat'], r['sta_a'], r['sta_b'])].append((tw, dc))
    dt0 = infer_epoch_dt(float(r['tow']) for r in tail)
    tow0 = min(float(r['tow']) for r in tail)
    steps = adjacent_steps(series, dt0, tow0)
    naive = []
    for v in series.values():
        v.sort()
        naive += [abs(b[1] - a[1]) for a, b in zip(v, v[1:])]
    med_step = st.median(steps) if steps else float('nan')
    med_naive = st.median(naive) if naive else float('nan')
    med_sig = st.median(sig) if sig else float('nan')

    print('n=%d  mean|f|=%.3f  med|f|=%.3f  P(<0.1)=%.3f  circR=%.3f' %
          (n, sum(fr) / n, st.median(fr), sum(1 for x in fr if x < 0.1) / n, R))
    print('formal dd_sigma med=%.3f cyc   DD step med=%.3f cyc/ep (naive incl. gaps %.3f; '
          'dt0=%.0fs)   calib(step/(sqrt2*sig))=%.2fx' %
          (med_sig, med_step, med_naive, dt0,
           med_step / (math.sqrt(2) * med_sig) if sig and steps else float('nan')))

    # Health trace: fraction of LOS residuals beyond 3 sigma (card T1a)
    h = os.path.join(run, 'logs', 'network_vtec_sh_health.csv')
    if os.path.exists(h):
        hr = [r for r in csv.DictReader(open(h)) if r.get('tow')]
        ht = [r for r in hr if float(r['tow']) >= tmax - tail_s]
        cols = ht[0].keys() if ht else []
        c3 = [c for c in cols if '3s' in c or '3sig' in c or 'over3' in c]
        cn = [c for c in cols if c.startswith('n_los') or c == 'n_rows' or c == 'nlos']
        if ht and c3:
            f3 = sum(float(r[c3[0]]) for r in ht)
            nn = sum(float(r[cn[0]]) for r in ht) if cn else len(ht)
            print('LOS>3sig frac (tail) = %.3f  [cols %s/%s]' % (f3 / max(nn, 1), c3[0], cn[0] if cn else '-'))
        else:
            print('health cols:', list(cols))
    a = os.path.join(run, 'logs', 'network_vtec_sh.csv')
    if os.path.exists(a):
        ar = [r for r in csv.DictReader(open(a)) if r.get('n') == '0' and r.get('m') == '0']
        at = [float(r['value_tecu']) for r in ar if float(r['tow']) >= tmax - tail_s] if ar and 'value_tecu' in ar[0] else []
        if at:
            print('a00 tail = %.1f +/- %.1f TECU' % (st.mean(at), st.stdev(at) if len(at) > 1 else 0))


if __name__ == '__main__':
    main()

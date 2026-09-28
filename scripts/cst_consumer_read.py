#!/usr/bin/env python3
"""Card CST: read `logs/closed_loop_sat_trace.csv` (the per-satellite closed-loop
consumer trace) against the consumer's own position trace and the producer's events.

    python scripts/cst_consumer_read.py <run_dir> [--tow-min T --tow-max T]
           [--section overview|steps|states|resid|events|attrib|all]
           [--sat G23] [--top N] [--step-m M] [--var-ratio R]

Trace-only: launches nothing, writes nothing but stdout, does not plot.

The one piece of algebra is standing invariant #5. Every epoch-to-epoch
per-satellite delta is de-commoned by the median change over the satellites (or
(sat,signal) cells) present at both epochs, because the network-common part of a
correction is absorbed by the consumer's own receiver-clock state and grading it
raw confounds benign frame motion with a real step. A cell whose availability
changes between two epochs is skipped rather than differenced.

What is and is not a measurement here
  * `innov_*` are pre-fit innovations and carry the consumer's receiver clock
    (~ -98 m on this bed). They are reported de-commoned or not at all.
  * `res_*` are post-fit residuals; the receiver clock is already absorbed, so
    they are quoted raw.
  * a declared step is one whose disc counter (`pb_disc` / `cb_disc` /
    `clk_disc`) changed at the same epoch. Everything else is undeclared, which
    is the population EV6 measured at 451 of 545 on the producer side.
"""
from __future__ import annotations

import argparse
import csv
import os
import statistics
import sys
from collections import defaultdict


# --------------------------------------------------------------------------
# loading
# --------------------------------------------------------------------------
def _f(s):
    if s is None or s == '':
        return None
    try:
        return float(s)
    except ValueError:
        return None


def _i(s):
    if s is None or s == '':
        return None
    try:
        return int(s)
    except ValueError:
        return None


def load_sat(run_dir, tmin, tmax):
    """tow -> {(sat,sig): row}; row values already typed."""
    path = os.path.join(run_dir, 'logs', 'closed_loop_sat_trace.csv')
    if not os.path.exists(path):
        sys.exit('missing %s — was network.trace_consumer_sat armed?' % path)
    ep = defaultdict(dict)
    with open(path, newline='') as fh:
        for r in csv.DictReader(fh):
            tow = _f(r['tow'])
            if tow is None or tow < tmin or tow > tmax:
                continue
            ep[tow][(r['sat'], r['sig'])] = {
                'sat': r['sat'], 'sig': r['sig'], 'code_sig': r['code_sig'],
                'elev': _f(r['elev_deg']), 'n_sig': _i(r['n_sig']),
                'clk': _f(r['clk_m']), 'cb': _f(r['cb_m']), 'pb': _f(r['pb_m']),
                'clk_disc': _i(r['clk_disc']), 'cb_disc': _i(r['cb_disc']),
                'pb_disc': _i(r['pb_disc']),
                'ev_pb': r['ev_pb'], 'ev_cb': r['ev_cb'], 'ev_clk': r['ev_clk'],
                'innov_code': _f(r['innov_code_m']), 'innov_phase': _f(r['innov_phase_m']),
                'res_code': _f(r['res_code_m']), 'res_phase': _f(r['res_phase_m']),
                'amb': _f(r['amb_m']), 'amb_var': _f(r['amb_var']),
                'iono': _f(r['iono_m']), 'iono_var': _f(r['iono_var']),
            }
    return dict(sorted(ep.items()))


def load_pos(run_dir, tmin, tmax):
    path = os.path.join(run_dir, 'logs', 'closed_loop_trace.csv')
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, newline='') as fh:
        for r in csv.DictReader(fh):
            tow = _f(r['tow'])
            if tow is None or tow < tmin or tow > tmax:
                continue
            out[tow] = {'e3d': _f(r['pos_err_3d_m']), 'e': _f(r['east_m']),
                        'n': _f(r['north_m']), 'u': _f(r['up_m']),
                        'n_sats': _i(r['n_sats']), 'ar_gate': r.get('ar_gate', '')}
    return out


def load_events(run_dir, tmin, tmax):
    path = os.path.join(run_dir, 'logs', 'network_events.csv')
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, newline='') as fh:
        for r in csv.DictReader(fh):
            tow = _f(r.get('tow'))
            if tow is None or tow < tmin or tow > tmax:
                continue
            rows.append((tow, r))
    return rows


# --------------------------------------------------------------------------
# invariant #5: de-commoned pair deltas
# --------------------------------------------------------------------------
def pair_deltas(prev, cur, field, key_is_sat=False):
    """Per-cell delta of `field` between two epochs, de-commoned by the median
    over the cells present (with a finite value) at both epochs.

    key_is_sat: collapse (sat,sig) to sat first, since the satellite clock and
    the slant iono are per-satellite quantities that repeat across a satellite's
    rows, and differencing them per row would weight a two-band satellite twice
    in the median.

    For per-(sat,signal) quantities the median is taken within each signal, not
    across the epoch: L1W and L2W phase biases do not share a common mode (the
    band-differential part is absorbed by the receiver's code-bias / phase-clock
    states, the band-common part by the receiver clock), so one median over a
    mixed L1/L2 population leaks a band-differential motion into every cell's
    residual. The returned `med` is then the median of the per-signal medians,
    reported only as a scale.
    """
    def collapse(ep):
        out = {}
        for (sat, sig), row in ep.items():
            v = row[field]
            if v is None:
                continue
            out[sat if key_is_sat else (sat, sig)] = v
        return out

    a, b = collapse(prev), collapse(cur)
    common = sorted(set(a) & set(b))
    if not common:
        return {}, 0.0, 0
    raw = {k: b[k] - a[k] for k in common}
    if key_is_sat:
        med = statistics.median(raw.values())
        return {k: raw[k] - med for k in common}, med, len(common)
    by_sig = defaultdict(list)
    for (sat, sig), v in raw.items():
        by_sig[sig].append(v)
    meds = {sig: statistics.median(v) for sig, v in by_sig.items()}
    out = {k: raw[k] - meds[k[1]] for k in common}
    return out, statistics.median(meds.values()), len(common)


def declared(prev, cur, key, disc_field):
    """Did the producer declare at `cur` for this cell? (disc counter changed)"""
    p, c = prev.get(key), cur.get(key)
    if p is None or c is None:
        return None
    if p[disc_field] is None or c[disc_field] is None:
        return None
    return p[disc_field] != c[disc_field]


# --------------------------------------------------------------------------
# sections
# --------------------------------------------------------------------------
def sec_overview(ep, pos, args):
    print('== OVERVIEW: consumer position, sky, reactions, residual scale ==')
    print('%10s %8s %8s %8s %8s %5s %4s %5s %9s %9s %9s  %s' %
          ('tow', '3D', 'east', 'north', 'up', 'nsat', 'row', 'nsig',
           'p50|res_ph|', 'p50|res_cd|', 'max|res_ph|', 'events(!=none)'))
    tows = sorted(ep)
    for t in tows:
        cells = ep[t]
        sats = sorted({s for s, _ in cells})
        rp = sorted(abs(c['res_phase']) for c in cells.values() if c['res_phase'] is not None)
        rc = sorted(abs(c['res_code']) for c in cells.values() if c['res_code'] is not None)
        evs = []
        for (sat, sig), c in sorted(cells.items()):
            for tag, fld in (('pb', 'ev_pb'), ('cb', 'ev_cb'), ('clk', 'ev_clk')):
                if c[fld] and c[fld] != 'none':
                    e = '%s/%s:%s' % (sat, sig if tag != 'clk' else 'CLK', c[fld])
                    if e not in evs:
                        evs.append(e)
        p = pos.get(t, {})
        print('%10.1f %8s %8s %8s %8s %5s %4d %5d %9s %9s %9s  %s' % (
            t,
            '%.3f' % p['e3d'] if p.get('e3d') is not None else '',
            '%+.3f' % p['e'] if p.get('e') is not None else '',
            '%+.3f' % p['n'] if p.get('n') is not None else '',
            '%+.3f' % p['u'] if p.get('u') is not None else '',
            p.get('n_sats', ''), len(sats), len(cells),
            '%.4f' % statistics.median(rp) if rp else '',
            '%.4f' % statistics.median(rc) if rc else '',
            '%.4f' % rp[-1] if rp else '',
            ' '.join(evs[:6]) + (' +%d' % (len(evs) - 6) if len(evs) > 6 else '')))


def sec_steps(ep, pos, args):
    print('== WIRE STEPS AT THE CONSUMER (de-commoned; invariant #5) ==')
    print('   a cell is listed when |de-commoned delta| > %.3f m on any channel.' % args.step_m)
    print('%10s %5s %5s %9s %5s %9s %5s %9s %5s  %s' %
          ('tow', 'sat', 'sig', 'dPB', 'decl', 'dCB', 'decl', 'dCLK', 'decl', 'consumer event'))
    tows = sorted(ep)
    n_hit = n_undecl = 0
    for i in range(1, len(tows)):
        t0, t1 = tows[i - 1], tows[i]
        prev, cur = ep[t0], ep[t1]
        dpb, mpb, _ = pair_deltas(prev, cur, 'pb')
        dcb, mcb, _ = pair_deltas(prev, cur, 'cb')
        dclk, mclk, _ = pair_deltas(prev, cur, 'clk', key_is_sat=True)
        for key in sorted(set(dpb) | set(dcb)):
            sat, sig = key
            vpb, vcb = dpb.get(key), dcb.get(key)
            vclk = dclk.get(sat)
            worst = max(abs(v) for v in (vpb, vcb, vclk) if v is not None)
            if worst <= args.step_m:
                continue
            if args.sat and sat != args.sat:
                continue
            n_hit += 1
            d_pb = declared(prev, cur, key, 'pb_disc')
            d_cb = declared(prev, cur, key, 'cb_disc')
            d_clk = None
            for k2 in cur:
                if k2[0] == sat:
                    d_clk = declared(prev, cur, k2, 'clk_disc')
                    break
            big_undecl = ((vpb is not None and abs(vpb) > args.step_m and d_pb is False)
                          or (vcb is not None and abs(vcb) > args.step_m and d_cb is False)
                          or (vclk is not None and abs(vclk) > args.step_m and d_clk is False))
            if big_undecl:
                n_undecl += 1
            c = cur[key]
            ev = ';'.join(x for x in (c['ev_pb'], c['ev_cb'], c['ev_clk']) if x != 'none') or '-'
            def fmt(v, d):
                return ('%+9.3f' % v if v is not None else '' + ' ' * 9,
                        {True: 'YES', False: 'NO', None: ''}[d])
            a, b = fmt(vpb, d_pb)
            cc, dd = fmt(vcb, d_cb)
            e, f = fmt(vclk, d_clk)
            print('%10.1f %5s %5s %9s %5s %9s %5s %9s %5s  %s' %
                  (t1, sat, sig, a, b, cc, dd, e, f, ev))
    print('-- %d cell-epochs over the bar, %d of them UNDECLARED on the stepping channel'
          % (n_hit, n_undecl))
    print('-- per-epoch COMMON MODE removed (median dPB / dCB / dCLK) is printed by --section common')


def sec_common(ep, pos, args):
    print('== THE COMMON MODE THAT WAS REMOVED (what the rx clock absorbs) ==')
    print('%10s %10s %10s %10s %6s' % ('tow', 'med dPB', 'med dCB', 'med dCLK', 'ncell'))
    tows = sorted(ep)
    for i in range(1, len(tows)):
        prev, cur = ep[tows[i - 1]], ep[tows[i]]
        _, mpb, npb = pair_deltas(prev, cur, 'pb')
        _, mcb, _ = pair_deltas(prev, cur, 'cb')
        _, mclk, _ = pair_deltas(prev, cur, 'clk', key_is_sat=True)
        print('%10.1f %10.4f %10.4f %10.4f %6d' % (tows[i], mpb, mcb, mclk, npb))


def sec_states(ep, pos, args):
    print('== CONSUMER STATES: ambiguity + slant iono, epoch to epoch ==')
    print('   dAMB / dIONO are DE-COMMONED (invariant #5).  var_ratio = var(t)/var(t-1):')
    print('   >> 1 is a RESET (variance re-inflation, the declared response);')
    print('   ~ 1 with a large dAMB is a FLOAT re-convergence, which nothing declared.')
    print('%10s %5s %5s %10s %10s %9s %10s %10s %9s  %s' %
          ('tow', 'sat', 'sig', 'dAMB', 'amb_var', 'var_rat', 'dIONO', 'iono_var',
           'ion_rat', 'event'))
    tows = sorted(ep)
    for i in range(1, len(tows)):
        t1 = tows[i]
        prev, cur = ep[tows[i - 1]], ep[t1]
        damb, _, _ = pair_deltas(prev, cur, 'amb')
        dion, _, _ = pair_deltas(prev, cur, 'iono', key_is_sat=True)
        for key in sorted(set(damb)):
            sat, sig = key
            if args.sat and sat != args.sat:
                continue
            c, p = cur[key], prev[key]
            vr = (c['amb_var'] / p['amb_var']) if (c['amb_var'] and p['amb_var']) else None
            ir = None
            if c['iono_var'] and p['iono_var']:
                ir = c['iono_var'] / p['iono_var']
            hit = (abs(damb[key]) > args.step_m
                   or (vr is not None and vr > args.var_ratio)
                   or (dion.get(sat) is not None and abs(dion[sat]) > args.step_m)
                   or c['ev_pb'] != 'none')
            if not hit:
                continue
            ev = ';'.join(x for x in (c['ev_pb'], c['ev_cb'], c['ev_clk']) if x != 'none') or '-'
            print('%10.1f %5s %5s %+10.4f %10.4g %9s %10s %10.4g %9s  %s' % (
                t1, sat, sig, damb[key], c['amb_var'] or 0.0,
                '%.2f' % vr if vr is not None else '',
                '%+.4f' % dion[sat] if dion.get(sat) is not None else '',
                c['iono_var'] or 0.0,
                '%.2f' % ir if ir is not None else '', ev))


def sec_resid(ep, pos, args):
    print('== POST-FIT RESIDUALS PER SATELLITE (raw: the rx clock is already absorbed) ==')
    tows = sorted(ep)
    keys = sorted({k for t in tows for k in ep[t]})
    if args.sat:
        keys = [k for k in keys if k[0] == args.sat]
    print('%5s %5s %6s %10s %10s %10s %10s %6s' %
          ('sat', 'sig', 'n_ep', 'p50|ph|', 'max|ph|', 'p50|cd|', 'max|cd|', 'elev'))
    for key in keys:
        ph = [abs(ep[t][key]['res_phase']) for t in tows
              if key in ep[t] and ep[t][key]['res_phase'] is not None]
        cd = [abs(ep[t][key]['res_code']) for t in tows
              if key in ep[t] and ep[t][key]['res_code'] is not None]
        el = [ep[t][key]['elev'] for t in tows if key in ep[t]]
        if not ph and not cd:
            continue
        print('%5s %5s %6d %10.4f %10.4f %10.4f %10.4f %6.1f' % (
            key[0], key[1], len(el),
            statistics.median(ph) if ph else 0.0, max(ph) if ph else 0.0,
            statistics.median(cd) if cd else 0.0, max(cd) if cd else 0.0,
            statistics.median(el) if el else 0.0))


def sec_drift(ep, pos, args):
    """Monotone drift over the whole window, per cell: the ramp instrument."""
    print('== MONOTONE DRIFT OVER THE WINDOW (cumulative de-commoned, pair-consistent) ==')
    print('   cumulative sum of the de-commoned pair deltas; a cell missing at either')
    print('   epoch of a pair contributes nothing to that pair (never a manufactured step).')
    tows = sorted(ep)
    cum = defaultdict(float)
    cumi = defaultdict(float)
    cuma = defaultdict(float)
    for i in range(1, len(tows)):
        prev, cur = ep[tows[i - 1]], ep[tows[i]]
        dpb, _, _ = pair_deltas(prev, cur, 'pb')
        dion, _, _ = pair_deltas(prev, cur, 'iono', key_is_sat=True)
        damb, _, _ = pair_deltas(prev, cur, 'amb')
        for k, v in dpb.items():
            cum[k] += v
        for k, v in damb.items():
            cuma[k] += v
        for k, v in dion.items():
            cumi[k] += v
    print('%5s %5s %12s %12s %12s' % ('sat', 'sig', 'cum dPB', 'cum dAMB', 'cum dIONO(sat)'))
    rows = sorted(cum, key=lambda k: -max(abs(cum[k]), abs(cuma.get(k, 0.0))))
    for k in rows[:args.top]:
        print('%5s %5s %+12.4f %+12.4f %+12.4f' %
              (k[0], k[1], cum[k], cuma.get(k, 0.0), cumi.get(k[0], 0.0)))


def sec_events(ep, pos, args):
    print('== PRODUCER EVENTS IN THE WINDOW (network_events.csv) ==')
    sky = {s for t in ep for s, _ in ep[t]}
    rows = load_events(args.run_dir, args.tow_min, args.tow_max)
    if not rows:
        print('  (no network_events.csv rows in the window)')
        return
    for tow, r in rows:
        ident = (r.get('sat') or '') + ('/' + r.get('station') if r.get('station') else '')
        sat = (r.get('sat') or '')[:3]
        if args.sat and sat != args.sat:
            continue
        mark = 'IN-SKY' if sat and sat in sky else ('      ' if sat else '   net')
        print('%10.1f %6s %-14s %-14s %-24s %-12s %s' %
              (tow, mark, r.get('tier', ''), r.get('event', ''), ident,
               r.get('statistic', ''), (r.get('detail', '') or '')[:96]))


def sec_census(ep, pos, args):
    """Run-wide: how often the consumer resets, and how big the step it reset for was.

    The question EV6 could not ask from the producer side: when the producer
    declares a phase-bias discontinuity, how much did the emitted value actually
    move at the consumer? A declaration with no step behind it costs the
    consumer its whole phase memory for nothing."""
    print('== RESET CENSUS: the consumer\'s response vs the step it responded to ==')
    tows = sorted(ep)
    per_epoch = []
    decl_step, undecl_step, empty_decl = [], [], 0
    n_reset_cells = n_cells = 0
    for i in range(1, len(tows)):
        t1 = tows[i]
        prev, cur = ep[tows[i - 1]], ep[t1]
        dpb, _, _ = pair_deltas(prev, cur, 'pb')
        n_r = n_d = 0
        for key, v in dpb.items():
            n_cells += 1
            d = declared(prev, cur, key, 'pb_disc')
            reset = cur[key]['ev_pb'].startswith('reset')
            if reset:
                n_r += 1
                n_reset_cells += 1
            if d is True:
                n_d += 1
                decl_step.append(abs(v))
                if abs(v) < args.empty_m:
                    empty_decl += 1
            elif d is False:
                undecl_step.append(abs(v))
        if n_r or n_d:
            per_epoch.append((t1, len(cur), n_d, n_r,
                              max((abs(v) for v in dpb.values()), default=0.0)))
    print('%10s %6s %6s %6s %12s' % ('tow', 'cells', 'declrd', 'reset', 'max|dPB|'))
    for t, nc, nd, nr, mx in per_epoch:
        print('%10.1f %6d %6d %6d %12.4f' % (t, nc, nd, nr, mx))
    def q(v, p):
        return sorted(v)[min(len(v) - 1, int(p * len(v)))] if v else float('nan')
    print('-- cell-epoch pairs %d; RESET on %d (%.2f %%)'
          % (n_cells, n_reset_cells, 100.0 * n_reset_cells / max(1, n_cells)))
    print('-- DECLARED cell-epochs %d: |de-commoned dPB| p50 %.4f  p90 %.4f  max %.4f m'
          % (len(decl_step), q(decl_step, .5), q(decl_step, .9), max(decl_step or [0])))
    print('-- of those, %d (%.1f %%) moved LESS than %.3f m — an EMPTY declaration'
          % (empty_decl, 100.0 * empty_decl / max(1, len(decl_step)), args.empty_m))
    print('-- UNDECLARED cell-epochs %d: |de-commoned dPB| p50 %.4f  p90 %.4f  max %.4f m'
          % (len(undecl_step), q(undecl_step, .5), q(undecl_step, .9),
             max(undecl_step or [0])))


def sec_pair(ep, pos, args):
    """The RF-0b pair: does the consumer's slant iono move with its ambiguities?"""
    print('== AMBIGUITY <-> SLANT-IONO PAIR (cumulative de-commoned over the window) ==')
    tows = sorted(ep)
    cuma, cumi = defaultdict(float), defaultdict(float)
    for i in range(1, len(tows)):
        prev, cur = ep[tows[i - 1]], ep[tows[i]]
        damb, _, _ = pair_deltas(prev, cur, 'amb')
        dion, _, _ = pair_deltas(prev, cur, 'iono', key_is_sat=True)
        for k, v in damb.items():
            cuma[k] += v
        for k, v in dion.items():
            cumi[k] += v
    xs, ys = [], []
    print('%5s %5s %12s %12s' % ('sat', 'sig', 'cum dAMB', 'cum dIONO'))
    for k in sorted(cuma, key=lambda k: -abs(cuma[k])):
        if k[0] not in cumi:
            continue
        xs.append(cuma[k])
        ys.append(cumi[k[0]])
        print('%5s %5s %+12.4f %+12.4f' % (k[0], k[1], cuma[k], cumi[k[0]]))
    if len(xs) > 2:
        mx, my = statistics.fmean(xs), statistics.fmean(ys)
        sxy = sum((a - mx) * (b - my) for a, b in zip(xs, ys))
        sxx = sum((a - mx) ** 2 for a in xs)
        syy = sum((b - my) ** 2 for b in ys)
        r = sxy / ((sxx * syy) ** 0.5) if sxx > 0 and syy > 0 else float('nan')
        print('-- Pearson r(cum dAMB, cum dIONO) = %.3f over %d cells; '
              'slope %.3f m/m' % (r, len(xs), sxy / sxx if sxx else float('nan')))


def sec_impact(ep, pos, args):
    """Every mass-reset epoch, with the consumer's position before and after.

    A reset does not automatically cost the consumer: EV6 measured harmless
    producer events on this run. This table separates the reset event from its
    cost."""
    print('== MASS-RESET EPOCHS AND WHAT THEY COST THE CONSUMER ==')
    print('   reset >= %d cells.  before/after = consumer 3D p50 over the 15 min'
          % args.min_reset)
    print('   on each side; peak = max 3D in the 30 min after.')
    print('%10s %6s %6s %10s %10s %10s %10s %8s' %
          ('tow', 'cells', 'reset', 'max|dPB|', 'before', 'after', 'peak', 'cost'))
    tows = sorted(ep)
    ptows = sorted(pos)

    def p50(a, b):
        v = [pos[t]['e3d'] for t in ptows if a <= t <= b and pos[t]['e3d'] is not None]
        return statistics.median(v) if v else float('nan')

    def pk(a, b):
        v = [pos[t]['e3d'] for t in ptows if a <= t <= b and pos[t]['e3d'] is not None]
        return max(v) if v else float('nan')

    for i in range(1, len(tows)):
        t1 = tows[i]
        prev, cur = ep[tows[i - 1]], ep[t1]
        dpb, _, _ = pair_deltas(prev, cur, 'pb')
        n_r = sum(1 for k in cur if cur[k]['ev_pb'].startswith('reset'))
        if n_r < args.min_reset:
            continue
        mx = max((abs(v) for v in dpb.values()), default=0.0)
        before, after, peak = p50(t1 - 900, t1 - 30), p50(t1 + 30, t1 + 900), pk(t1, t1 + 1800)
        print('%10.1f %6d %6d %10.4f %10.3f %10.3f %10.3f %8.3f' %
              (t1, len(cur), n_r, mx, before, after, peak, after - before))


SECTIONS = {'overview': sec_overview, 'steps': sec_steps, 'common': sec_common,
            'impact': sec_impact,
            'states': sec_states, 'resid': sec_resid, 'drift': sec_drift,
            'events': sec_events, 'census': sec_census, 'pair': sec_pair}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_dir')
    ap.add_argument('--tow-min', type=float, default=-1e18)
    ap.add_argument('--tow-max', type=float, default=1e18)
    ap.add_argument('--section', default='overview',
                    choices=sorted(SECTIONS) + ['all'])
    ap.add_argument('--sat', default=None)
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--step-m', type=float, default=0.10)
    ap.add_argument('--var-ratio', type=float, default=5.0)
    ap.add_argument('--min-reset', type=int, default=8,
                    help='--section impact: cells reset to count as a MASS reset')
    ap.add_argument('--empty-m', type=float, default=0.02,
                    help='a DECLARED step smaller than this is an EMPTY declaration')
    args = ap.parse_args()

    ep = load_sat(args.run_dir, args.tow_min, args.tow_max)
    pos = load_pos(args.run_dir, args.tow_min, args.tow_max)
    if not ep:
        sys.exit('no rows in [%g, %g]' % (args.tow_min, args.tow_max))
    print('# %s  epochs %d  tow %.1f .. %.1f  cells/epoch %d..%d' % (
        args.run_dir, len(ep), min(ep), max(ep),
        min(len(v) for v in ep.values()), max(len(v) for v in ep.values())))
    names = sorted(SECTIONS) if args.section == 'all' else [args.section]
    for n in names:
        print()
        SECTIONS[n](ep, pos, args)


if __name__ == '__main__':
    main()

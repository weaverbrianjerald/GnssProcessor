#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""D5 v5 (``network.carry_invariant_innov_carried_only``) A/B grader.

The knob restricts the Card-4 D5 innovation gate to chords genuinely CARRIED into
the epoch.  A chord BORN this epoch has no carried prior — its Cov(v) is the diffuse
``carry_invariant_fresh_prior_var`` — so the standardized ratio is not a hypothesis
test, the "reset" it triggers is a no-op (a diffuse chord's posterior already IS the
data-only tap-off), and the flag's one lasting effect is the funnel entry that
re-borns the chord next epoch, which flags again.  ``NetworkInvariantCarryDriver.cpp``
``bornSkipped()`` skips a row when it is **topologically born** (``chord_carried``
false) OR **informationally diffuse** (``var_v >= 0.5 * fresh_prior_var``), counted
BEFORE the threshold test — so ``innov_born_skipped`` is the whole born population,
not just the part that would have flagged.

This script reads what an arm actually did and prints a machine-comparable JSON blob:

  RUN-END   the D5 stdout counters (flags / re-taps / v3-suppressed / v5-born-skipped
            / D5-keep / sigma0), the invariant-carry birth + carried-DD totals,
            ``attrib_arc``, epochs emitted, withheld epochs, COLD START.
  EPOCH     ``logs/network_epoch_status.csv`` — ``n_d5_flags``, ``n_d5_retaps``,
            ``n_chord_births``, ``n_slip_refloats`` per epoch, over a window.
  FLAGS     ``logs/network_d5_flags.csv`` — every row the gate actually installed,
            split BORN (``var_v >= frac * fresh_prior_var``, default frac 0.5, the
            same bar the C++ uses) vs CARRIED.  This is the V2 instrument: the knob
            must drive the BORN half to zero and leave the CARRIED half alone.
  LIFETIME  ``logs/network_dd_fractional.csv`` — chord population per epoch and
            chord LIFETIME = the number of consecutive epochs one chord id persists.
            Two keyings are reported: ``full`` (includes ``ref_sat``) and ``reffree``
            (drops it — the trace's reference satellite rotates and is in every
            chord, SPEC_amb_iono §6.12, so a lifetime keyed on it measures the
            reference's tenure as much as the chord's).
  EVENT     ``logs/network_events.csv`` rows for a (station, satellite) in an epoch
            window — the V1 identity instrument.

Usage
  python scripts/d5v5_grade.py <rundir> [--json out.json]
        [--window-from N] [--window-to N]      # epoch index relative to first epoch
        [--event-station GOPE00CZE --event-sat G24 --event-from 9 --event-to 12]
        [--fresh-prior-var 1e6] [--diffuse-frac 0.5]
  python scripts/d5v5_grade.py --compare a.json b.json   # A/B table + the V2/V3 gates
"""
from __future__ import annotations

import argparse
import csv
import io
import json
import os
import re
import sys
from collections import Counter, defaultdict


# --------------------------------------------------------------------------- utils
def _read_text(path):
    with io.open(path, 'r', encoding='utf-8', errors='replace') as f:
        return f.read()


def _rows(path):
    if not os.path.exists(path):
        return []
    with io.open(path, 'r', encoding='utf-8', errors='replace', newline='') as f:
        return list(csv.DictReader(f))


def _f(x, d=None):
    try:
        return float(x)
    except (TypeError, ValueError):
        return d


def _i(x, d=None):
    v = _f(x)
    return d if v is None else int(v)


def _median(xs):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    n = len(xs)
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])


# ------------------------------------------------------------------- stdout parsing
# Every pattern is anchored on the emitter's own wording (NetworkTeardown.cpp) so a
# renamed counter fails loudly (value None) instead of silently reading zero.
_PATTERNS = {
    'd5_flags': (r'D5 fine-slip innovation gate\):\s*([\d,]+)\s*carried-DD innovation flag', int),
    'd5_retaps': (r'D5 fine-slip innovation gate\):[^\n]*?([\d,]+)\s*fresh re-tap', int),
    'v3_suppressed': (r'D5 v3 corroboration gate\):\s*([\d,]+)\s*of\s*[\d,]+\s*gate trips SUPPRESSED', int),
    'v3_trips': (r'D5 v3 corroboration gate\):\s*[\d,]+\s*of\s*([\d,]+)\s*gate trips SUPPRESSED', int),
    'v3_retapped': (r'D5 v3 corroboration gate\):[^\n]*?;\s*([\d,]+)\s*re-tapped', int),
    'v5_born_skipped': (r'D5 v5 carried-only\):\s*([\d,]+)\s*chord-row\(s\) NOT gated', int),
    'd5keep_kept': (r'D5-keep\):\s*([\d,]+)\s*D5-only re-tap', int),
    'd5keep_erased': (r'D5-keep\):[^\n]*?([\d,]+)\s*raw-confirmed erase', int),
    'sigma0_postrun': (r'D5 v2 LS-VCE calibration\): post-run empirical inflation [^=]*=\s*([\d.eE+-]+)', float),
    'd5_nonpos_var_rows': (r'D5 rows the gate could not test \(Cov\(v\) not positive\):\s*([\d,]+)', int),
    'carry_restored_dd': (r'carry restored YES \(([\d,]+) carried DD at restore\)', int),
    # "Network (Card 4/M1 invariant carry): N epoch(s) driven, M carried DD(s) at run
    # end; B chord birth(s), ... , R slip refloat(s), ..."  — the carry-side totals.
    'carry_epochs_driven': (r'invariant carry\):\s*([\d,]+)\s*epoch\(s\) driven', int),
    'carried_dd_runend': (r'invariant carry\):[^\n]*?([\d,]+)\s*carried DD\(s\) at run end', int),
    'chord_births_runend': (r'invariant carry\):[^\n]*?([\d,]+)\s*chord birth\(s\)', int),
    'marginalizations': (r'invariant carry\):[^\n]*?([\d,]+)\s*marginalization\(s\)', int),
    'slip_refloats_runend': (r'invariant carry\):[^\n]*?([\d,]+)\s*slip refloat\(s\)', int),
    'emitted_epochs': (r'post-fit [^\n]{0,12}0\):\s*([\d,]+)\s*emitted epoch\(s\)', int),
}


def parse_stdout(path):
    """Pull the run-end D5 readouts + the run's structural facts out of stdout."""
    out = {'stdout_present': os.path.exists(path)}
    if not out['stdout_present']:
        return out
    t = _read_text(path)
    for key, (pat, cast) in _PATTERNS.items():
        m = re.search(pat, t)
        out[key] = cast(m.group(1).replace(',', '')) if m else None

    # The v5 INFO line itself — standing invariant #4: the arm must SAY it is on.
    out['v5_line_present'] = bool(
        re.search(r'D5 v5\): CARRIED-ONLY innovation gate ON', t))
    out['v5_inert_warn'] = bool(
        re.search(r'D5 v5\): carried-only is ARMED but skipped NOTHING', t))
    # A warm resume that cold-started is not an A/B arm, it is a different run.  The
    # POSITIVE evidence is the RTB-7d resume event ("carry restored YES (N carried DD
    # at restore)"); a bare /cold start/i search false-positives on the loader's own
    # prose ("...is a hard run error instead of a silent cold start", "cold_clock_seed
    # ON"), which is exactly the kind of absence-by-grep this project has been bitten
    # by — so the flag is derived from the restore line, not from the phrase.
    out['carry_restored'] = ('carry restored YES' in t)
    out['cold_start'] = not out['carry_restored']
    m = re.search(r'attrib_arc[^\d]{0,40}([\d,]+)', t)
    out['attrib_arc'] = int(m.group(1).replace(',', '')) if m else None
    # Back-compat aliases for the compare() table.
    out['chord_births_line'] = out.get('chord_births_runend')
    out['carried_dd_line'] = out.get('carried_dd_runend')
    return out


# --------------------------------------------------------------------- CSV channels
def epoch_status(rundir, w_from=None, w_to=None):
    rows = _rows(os.path.join(rundir, 'logs', 'network_epoch_status.csv'))
    if not rows:
        return {'present': False}
    tows = [_f(r['tow']) for r in rows]
    t0 = min(t for t in tows if t is not None)
    series, disp = [], Counter()
    for r in rows:
        tow = _f(r['tow'])
        ep = int(round((tow - t0) / 30.0)) if tow is not None else None
        d = (r.get('disposition') or '').strip()
        disp[d] += 1
        series.append({
            'ep': ep, 'tow': tow, 'disposition': d,
            'n_d5_flags': _i(r.get('n_d5_flags'), 0),
            'n_d5_retaps': _i(r.get('n_d5_retaps'), 0),
            'n_chord_births': _i(r.get('n_chord_births'), 0),
            'n_slip_refloats': _i(r.get('n_slip_refloats'), 0),
            'n_arc_refloat': _i(r.get('n_arc_refloat'), 0),
        })
    win = [s for s in series
           if (w_from is None or s['ep'] >= w_from) and (w_to is None or s['ep'] <= w_to)]
    agg = {}
    for k in ('n_d5_flags', 'n_d5_retaps', 'n_chord_births', 'n_slip_refloats', 'n_arc_refloat'):
        vals = [s[k] for s in win]
        agg[k] = {'sum': sum(vals), 'median': _median(vals), 'n_ep': len(vals)}
    return {'present': True, 'n_epochs': len(series), 'first_tow': t0,
            'dispositions': dict(disp),
            'withheld': sum(v for k, v in disp.items() if 'withh' in k.lower()),
            'window': {'from': w_from, 'to': w_to}, 'agg': agg, 'series': series}


def d5_flags(rundir, fresh_prior_var, diffuse_frac, w_from=None, w_to=None, t0=None):
    """Split the installed D5 flags into BORN vs CARRIED on the C++'s own bar."""
    rows = _rows(os.path.join(rundir, 'logs', 'network_d5_flags.csv'))
    if not rows:
        return {'present': False, 'n_rows': 0, 'born': 0, 'carried': 0}
    bar = diffuse_frac * fresh_prior_var
    if t0 is None:
        t0 = min(_f(r['tow']) for r in rows)
    born, carried, per_ep = 0, 0, defaultdict(lambda: [0, 0])
    born_keys, carried_keys = Counter(), Counter()
    for r in rows:
        tow = _f(r['tow'])
        ep = int(round((tow - t0) / 30.0)) if tow is not None else None
        if (w_from is not None and ep < w_from) or (w_to is not None and ep > w_to):
            continue
        v = _f(r.get('var_v'))
        key = '%s|%s' % (r.get('station', ''), r.get('amb_key', ''))
        if v is not None and v >= bar:
            born += 1
            per_ep[ep][0] += 1
            born_keys[key] += 1
        else:
            carried += 1
            per_ep[ep][1] += 1
            carried_keys[key] += 1
    return {
        'present': True, 'diffuse_bar_var': bar,
        'n_rows': born + carried, 'born': born, 'carried': carried,
        'born_frac': (born / (born + carried)) if (born + carried) else None,
        'per_epoch': {str(k): {'born': v[0], 'carried': v[1]} for k, v in sorted(per_ep.items())},
        'top_born_keys': born_keys.most_common(10),
        'top_carried_keys': carried_keys.most_common(10),
    }


def chord_lifetime(rundir, w_from=None, w_to=None, t0=None):
    """Chord population per epoch + lifetime (consecutive epochs one chord persists).

    ``full`` keys on the whole DD row incl. ``ref_sat``; ``reffree`` drops ``ref_sat``.
    The trace differences every satellite against one reference that rotates, and that
    reference is in EVERY chord (SPEC_amb_iono §6.12) — so a full-key lifetime partly
    measures the reference's tenure.  Both are reported; compare like with like.
    """
    rows = _rows(os.path.join(rundir, 'logs', 'network_dd_fractional.csv'))
    if not rows:
        return {'present': False}
    tows = sorted({_f(r['tow']) for r in rows})
    # Epoch indices MUST be referenced to the run's first processed epoch (the
    # epoch-status t0), not to this trace's own first row: a trace that starts late
    # would otherwise shift every index and silently mis-window the comparison.
    if t0 is None:
        t0 = tows[0]
    ep_of = {t: int(round((t - t0) / 30.0)) for t in tows}
    out = {'present': True, 'n_rows': len(rows)}
    for mode in ('full', 'reffree'):
        seen = defaultdict(set)
        pop = Counter()
        for r in rows:
            ep = ep_of[_f(r['tow'])]
            if (w_from is not None and ep < w_from) or (w_to is not None and ep > w_to):
                continue
            if mode == 'full':
                k = (r['cons'], r['signal'], r['sat_i'], r['ref_sat'], r['sta_a'], r['sta_b'])
            else:
                k = (r['cons'], r['signal'], r['sat_i'], r['sta_a'], r['sta_b'])
            seen[k].add(ep)
            pop[ep] += 1
        runs = []
        for k, eps in seen.items():
            eps = sorted(eps)
            run = 1
            for a, b in zip(eps, eps[1:]):
                if b == a + 1:
                    run += 1
                else:
                    runs.append(run)
                    run = 1
            runs.append(run)
        out[mode] = {
            'n_chords': len(seen),
            'n_runs': len(runs),
            'lifetime_median': _median(runs),
            'lifetime_mean': (sum(runs) / len(runs)) if runs else None,
            'lifetime_max': max(runs) if runs else None,
            'lifetime_ge2_frac': (sum(1 for r in runs if r >= 2) / len(runs)) if runs else None,
            'pop_median': _median(list(pop.values())),
            'pop_per_epoch': {str(k): v for k, v in sorted(pop.items())},
        }
    return out


def consumer(rundir, w_from, w_to, t0):
    """Closed-loop consumer over an epoch window, DE-BIASED per standing invariant #5.

    A static coordinate realization at the hold-out station enters ``pos_err_3d_m``
    directly and has nothing to do with the knob (SPEC_event_harness §15.10), so the
    graded quantity is |ENU − the ARM'S OWN window-mean ENU|.  The raw mean is
    reported beside it so the confound stays visible instead of hiding inside the
    number.
    """
    rows = _rows(os.path.join(rundir, 'logs', 'closed_loop_trace.csv'))
    if not rows:
        return {'present': False}
    sel = []
    for r in rows:
        tow = _f(r['tow'])
        ep = int(round((tow - t0) / 30.0)) if (tow is not None and t0 is not None) else None
        if ep is None or (w_from is not None and ep < w_from) or (w_to is not None and ep > w_to):
            continue
        e, n, u = _f(r.get('east_m')), _f(r.get('north_m')), _f(r.get('up_m'))
        if None in (e, n, u):
            continue
        sel.append((ep, e, n, u, _f(r.get('pos_err_3d_m'))))
    if not sel:
        return {'present': True, 'n': 0}
    me = sum(s[1] for s in sel) / len(sel)
    mn = sum(s[2] for s in sel) / len(sel)
    mu = sum(s[3] for s in sel) / len(sel)
    deb = [((s[1] - me) ** 2 + (s[2] - mn) ** 2 + (s[3] - mu) ** 2) ** 0.5 for s in sel]
    raw = [s[4] for s in sel if s[4] is not None]
    return {'present': True, 'n': len(sel), 'window': [w_from, w_to],
            'mean_enu': [me, mn, mu],
            'debias_mean': sum(deb) / len(deb), 'debias_p50': _median(deb),
            'debias_rms': (sum(d * d for d in deb) / len(deb)) ** 0.5,
            'raw3d_mean': (sum(raw) / len(raw)) if raw else None,
            'raw3d_p50': _median(raw)}


def a00(rundir, w_from, w_to, t0):
    """SH a00 (layer 0, n 0, m 0, trig C) — the frame channel, in TECU."""
    rows = _rows(os.path.join(rundir, 'logs', 'network_vtec_sh.csv'))
    if not rows:
        return {'present': False}
    vals = []
    for r in rows:
        try:
            if not (int(r.get('layer', 0)) == 0 and int(r.get('n', -1)) == 0
                    and int(r.get('m', -1)) == 0 and (r.get('trig') or 'C').upper().startswith('C')):
                continue
        except (TypeError, ValueError):
            continue
        tow = _f(r['tow'])
        ep = int(round((tow - t0) / 30.0)) if (tow is not None and t0 is not None) else None
        if ep is None or (w_from is not None and ep < w_from) or (w_to is not None and ep > w_to):
            continue
        v = _f(r.get('value_tecu'))
        if v is not None:
            vals.append((ep, v))
    if not vals:
        return {'present': True, 'n': 0}
    xs = [v for _, v in vals]
    return {'present': True, 'n': len(xs), 'median': _median(xs),
            'mean': sum(xs) / len(xs), 'min': min(xs), 'max': max(xs),
            'series': {str(e): v for e, v in sorted(vals)}}


def event_rows(rundir, station, sat, ep_from, ep_to, t0):
    """Verbatim (tier, event, action, detail) rows for one (station, sat) in a window."""
    rows = _rows(os.path.join(rundir, 'logs', 'network_events.csv'))
    if not rows:
        return {'present': False, 'rows': []}
    out = []
    for r in rows:
        if station and r.get('station') != station:
            continue
        if sat and r.get('sat') != sat:
            continue
        tow = _f(r['tow'])
        ep = int(round((tow - t0) / 30.0)) if (tow is not None and t0 is not None) else None
        if ep_from is not None and (ep is None or ep < ep_from):
            continue
        if ep_to is not None and (ep is None or ep > ep_to):
            continue
        out.append({'ep': ep, 'tow': tow, 'tier': r.get('tier'), 'event': r.get('event'),
                    'statistic': r.get('statistic'), 'threshold': r.get('threshold'),
                    'action': r.get('action'), 'detail': r.get('detail')})
    return {'present': True, 'rows': out}


def d5_rows_for(rundir, station, sat, ep_from, ep_to, t0):
    """D5 flag rows for one (station, sat) in an epoch window — V1's other half."""
    rows = _rows(os.path.join(rundir, 'logs', 'network_d5_flags.csv'))
    out = []
    for r in rows:
        if station and r.get('station') != station:
            continue
        if sat and sat not in (r.get('amb_key') or ''):
            continue
        tow = _f(r['tow'])
        ep = int(round((tow - t0) / 30.0)) if (tow is not None and t0 is not None) else None
        if ep_from is not None and (ep is None or ep < ep_from):
            continue
        if ep_to is not None and (ep is None or ep > ep_to):
            continue
        out.append({'ep': ep, 'amb_key': r.get('amb_key'), 'std_innov': _f(r.get('std_innov')),
                    'var_v': _f(r.get('var_v')), 'raw_stepped': r.get('raw_stepped')})
    return out


# ------------------------------------------------------------------------ main grade
def grade(args):
    rundir = os.path.abspath(args.rundir)
    st = parse_stdout(os.path.join(rundir, 'stdout.txt'))
    es = epoch_status(rundir, args.window_from, args.window_to)
    t0 = es.get('first_tow')
    fl = d5_flags(rundir, args.fresh_prior_var, args.diffuse_frac,
                  args.window_from, args.window_to, t0)
    lt = chord_lifetime(rundir, args.window_from, args.window_to, t0)
    cons = consumer(rundir, args.cons_from, args.cons_to, t0)
    frame = a00(rundir, args.window_from, args.window_to, t0)
    ec = {}
    if args.event_station or args.event_sat:
        ec = {
            'events': event_rows(rundir, args.event_station, args.event_sat,
                                 args.event_from, args.event_to, t0),
            'd5_rows': d5_rows_for(rundir, args.event_station, args.event_sat,
                                   args.event_from, args.event_to, t0),
        }
    exitcode = None
    p = os.path.join(rundir, 'exitcode.txt')
    if os.path.exists(p):
        exitcode = _read_text(p).strip().splitlines()
    # CARRY-SIDE chord lifetime.  ``network_dd_fractional.csv``'s chord POPULATION is
    # visibility-determined, not carry-determined: measured on the 20-epoch smoke pair
    # the two arms' row keys are set-IDENTICAL (931 rows each) while 674 of the 931
    # frac_cyc VALUES differ — so a lifetime read off that trace is structurally blind
    # to this knob and its "ON >= OFF" bar passes by construction.  The carry's own
    # numbers are sensitive, so estimate mean lifetime there by Little's law:
    #     W = L / lambda,  L = carried DDs at run end,  lambda = births per epoch.
    # It assumes a roughly stationary carried population over the leg; it is an
    # ESTIMATE, reported beside the raw inputs so the assumption stays checkable.
    st_ = st
    L = st_.get('carried_dd_runend')
    Bth = st_.get('chord_births_runend')
    NE = st_.get('carry_epochs_driven') or es.get('n_epochs')
    carry_lt = None
    if L and Bth and NE:
        lam = Bth / float(NE)
        carry_lt = (L / lam) if lam > 0 else None
    lt['carry_little'] = {'carried_dd_runend': L, 'births': Bth, 'epochs': NE,
                          'births_per_epoch': (Bth / float(NE)) if (Bth and NE) else None,
                          'mean_lifetime_ep': carry_lt}
    return {'rundir': rundir, 'exitcode': exitcode, 'runend': st,
            'epoch_status': es, 'd5_flags': fl, 'lifetime': lt,
            'consumer': cons, 'a00': frame, 'event': ec}


# --------------------------------------------------------------------------- compare
def _g(d, *path, default=None):
    for p in path:
        if not isinstance(d, dict) or p not in d:
            return default
        d = d[p]
    return d


def compare(a_path, b_path):
    A, B = json.load(open(a_path)), json.load(open(b_path))
    lines, gates = [], []

    def row(label, av, bv, fmt='%s'):
        sa = 'n/a' if av is None else fmt % av
        sb = 'n/a' if bv is None else fmt % bv
        lines.append('  %-34s %14s %14s' % (label, sa, sb))

    lines.append('  %-34s %14s %14s' % ('', 'OFF(A)', 'ON(B)'))
    lines.append('  RUN-END')
    for k in ('d5_flags', 'd5_retaps', 'v3_trips', 'v3_suppressed', 'v5_born_skipped',
              'd5keep_kept', 'd5keep_erased', 'd5_nonpos_var_rows', 'sigma0_postrun',
              'carried_dd_line', 'chord_births_line', 'attrib_arc'):
        row(k, _g(A, 'runend', k), _g(B, 'runend', k))
    row('cold_start', _g(A, 'runend', 'cold_start'), _g(B, 'runend', 'cold_start'))
    row('v5_line_present', _g(A, 'runend', 'v5_line_present'), _g(B, 'runend', 'v5_line_present'))
    row('n_epochs', _g(A, 'epoch_status', 'n_epochs'), _g(B, 'epoch_status', 'n_epochs'))
    row('withheld', _g(A, 'epoch_status', 'withheld'), _g(B, 'epoch_status', 'withheld'))

    lines.append('  D5 FLAG SPLIT (window)')
    for k in ('n_rows', 'born', 'carried'):
        row('flags.' + k, _g(A, 'd5_flags', k), _g(B, 'd5_flags', k))
    lines.append('  EPOCH-STATUS SUMS (window)')
    for k in ('n_d5_flags', 'n_d5_retaps', 'n_chord_births', 'n_slip_refloats'):
        row(k + '.sum', _g(A, 'epoch_status', 'agg', k, 'sum'),
            _g(B, 'epoch_status', 'agg', k, 'sum'))
        row(k + '.median', _g(A, 'epoch_status', 'agg', k, 'median'),
            _g(B, 'epoch_status', 'agg', k, 'median'))
    lines.append('  CHORD LIFETIME (window)')
    for mode in ('full', 'reffree'):
        for k in ('n_chords', 'lifetime_median', 'lifetime_mean', 'pop_median'):
            row('%s.%s' % (mode, k), _g(A, 'lifetime', mode, k), _g(B, 'lifetime', mode, k))

    # ---- V2 gates, exactly as pre-registered -------------------------------------
    a_born = _g(A, 'd5_flags', 'born')
    b_born = _g(B, 'd5_flags', 'born')
    a_car = _g(A, 'd5_flags', 'carried')
    b_car = _g(B, 'd5_flags', 'carried')
    bs = _g(B, 'runend', 'v5_born_skipped')
    gates.append(('V2a born-chord D5 flags ON = 0', b_born == 0,
                  'OFF %s -> ON %s' % (a_born, b_born)))
    gates.append(('V2b born-skipped counter > 0', (bs or 0) > 0, 'born_skipped = %s' % bs))
    if a_car:
        rel = abs((b_car - a_car) / a_car)
        gates.append(('V2c carried flags within +-10%%', rel <= 0.10,
                      'OFF %s -> ON %s (%.1f%%)' % (a_car, b_car, 100 * rel)))
    for k, label in (('n_chord_births', 'V2d births/ep ON <= OFF'),
                     ('n_d5_retaps', 'V2e re-taps ON <= OFF')):
        av = _g(A, 'epoch_status', 'agg', k, 'sum')
        bv = _g(B, 'epoch_status', 'agg', k, 'sum')
        if av is not None and bv is not None:
            gates.append((label, bv <= av, 'OFF %s -> ON %s' % (av, bv)))
    for mode in ('full', 'reffree'):
        av = _g(A, 'lifetime', mode, 'lifetime_median')
        bv = _g(B, 'lifetime', mode, 'lifetime_median')
        if av is not None and bv is not None:
            note = '' if av != bv else '  (IDENTICAL - trace population is visibility-determined, see grade())'
            gates.append(('V2f median chord lifetime ON >= OFF (%s)' % mode, bv >= av,
                          'OFF %s -> ON %s%s' % (av, bv, note)))
    av = _g(A, 'lifetime', 'carry_little', 'mean_lifetime_ep')
    bv = _g(B, 'lifetime', 'carry_little', 'mean_lifetime_ep')
    if av is not None and bv is not None:
        gates.append(('V2f* carry-side mean lifetime ON >= OFF', bv >= av,
                      'OFF %.1f -> ON %.1f ep (Little)' % (av, bv)))
    # ---- V3 no-harm gates on the channels this script owns -----------------------
    lines.append('  CONSUMER (de-biased, window %s) / FRAME' %
                 (_g(A, 'consumer', 'window'),))
    for k in ('n', 'debias_mean', 'debias_p50', 'debias_rms', 'raw3d_mean'):
        row('consumer.' + k, _g(A, 'consumer', k), _g(B, 'consumer', k))
    for k in ('median', 'mean', 'min', 'max'):
        row('a00.' + k, _g(A, 'a00', k), _g(B, 'a00', k))

    ac, bc = _g(A, 'consumer', 'debias_mean'), _g(B, 'consumer', 'debias_mean')
    if ac is not None and bc is not None:
        gates.append(('V3d consumer de-biased 3D mean <= 0.03 m', abs(bc - ac) <= 0.03,
                      '|%.4f - %.4f| = %.4f m' % (bc, ac, abs(bc - ac))))
    aa, ba = _g(A, 'a00', 'median'), _g(B, 'a00', 'median')
    if aa is not None and ba is not None:
        gates.append(('V3g a00 median within 0.5 TECU', abs(ba - aa) <= 0.5,
                      '|%.4f - %.4f| = %.4f TECU' % (ba, aa, abs(ba - aa))))
    aw = _g(A, 'epoch_status', 'withheld')
    bw = _g(B, 'epoch_status', 'withheld')
    if aw is not None and bw is not None:
        gates.append(('V3f withheld epochs equal', aw == bw, 'OFF %s / ON %s' % (aw, bw)))

    # ---- V1 identity: the injected chord's event + D5 rows must match EXACTLY -----
    ae, be = _g(A, 'event', 'events', 'rows'), _g(B, 'event', 'events', 'rows')
    if ae is not None and be is not None:
        same = (ae == be)
        gates.append(('V1a injected (sta,sat) event rows identical', same,
                      'OFF %d rows / ON %d rows' % (len(ae), len(be))))
        if not same:
            lines.append('  V1 EVENT ROW DIFF')
            for r in ae:
                if r not in be:
                    lines.append('    OFF-only: %s' % r)
            for r in be:
                if r not in ae:
                    lines.append('    ON-only : %s' % r)
    ad, bd = _g(A, 'event', 'd5_rows'), _g(B, 'event', 'd5_rows')
    if ad is not None and bd is not None:
        ka = sorted((r['ep'], r['amb_key']) for r in ad)
        kb = sorted((r['ep'], r['amb_key']) for r in bd)
        gates.append(('V1b injected chord D5 flag/re-tap set identical', ka == kb,
                      'OFF %s / ON %s' % (ka, kb)))

    print('\n'.join(lines))
    print('\n  GATES')
    ok = True
    for name, passed, detail in gates:
        ok &= bool(passed)
        print('   [%s] %-46s %s' % ('PASS' if passed else 'FAIL', name, detail))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rundir', nargs='?')
    ap.add_argument('--json')
    ap.add_argument('--window-from', type=int)
    ap.add_argument('--window-to', type=int)
    ap.add_argument('--fresh-prior-var', type=float, default=1e6)
    ap.add_argument('--diffuse-frac', type=float, default=0.5)
    ap.add_argument('--event-station')
    ap.add_argument('--event-sat')
    ap.add_argument('--event-from', type=int)
    ap.add_argument('--event-to', type=int)
    ap.add_argument('--cons-from', type=int, default=30,
                    help='consumer window start epoch (V3 pre-registers +30)')
    ap.add_argument('--cons-to', type=int, default=60,
                    help='consumer window end epoch (V3 pre-registers +60)')
    ap.add_argument('--compare', nargs=2, metavar=('A_JSON', 'B_JSON'))
    args = ap.parse_args()
    if args.compare:
        return compare(*args.compare)
    if not args.rundir:
        ap.error('rundir required (or --compare A B)')
    g = grade(args)
    if args.json:
        with io.open(args.json, 'w', encoding='utf-8') as f:
            json.dump(g, f, indent=1)
        print('wrote', args.json)
    r = g['runend']
    print('%-40s exit=%s warm_resume=%s v5_line=%s' %
          (os.path.basename(g['rundir']), g['exitcode'], r.get('carry_restored'),
           r.get('v5_line_present')))
    print('  epochs=%s emitted=%s withheld=%s | D5 flags=%s retaps=%s | v3 trips=%s '
          'supp=%s | v5 born_skipped=%s' %
          (_g(g, 'epoch_status', 'n_epochs'), r.get('emitted_epochs'),
           _g(g, 'epoch_status', 'withheld'),
           r.get('d5_flags'), r.get('d5_retaps'), r.get('v3_trips'),
           r.get('v3_suppressed'), r.get('v5_born_skipped')))
    print('  carry: %s carried DD at run end, %s births, %s slip refloats, %s marg' %
          (r.get('carried_dd_runend'), r.get('chord_births_runend'),
           r.get('slip_refloats_runend'), r.get('marginalizations')))
    print('  flag rows=%s  born=%s carried=%s (bar var_v>=%.4g)' %
          (_g(g, 'd5_flags', 'n_rows'), _g(g, 'd5_flags', 'born'),
           _g(g, 'd5_flags', 'carried'), _g(g, 'd5_flags', 'diffuse_bar_var') or 0))
    for mode in ('full', 'reffree'):
        print('  lifetime[%s] n_chords=%s median=%s mean=%s pop_median=%s' %
              (mode, _g(g, 'lifetime', mode, 'n_chords'),
               _g(g, 'lifetime', mode, 'lifetime_median'),
               _g(g, 'lifetime', mode, 'lifetime_mean'),
               _g(g, 'lifetime', mode, 'pop_median')))
    return 0


if __name__ == '__main__':
    sys.exit(main())

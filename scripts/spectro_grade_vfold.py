#!/usr/bin/env python3
"""Grade the S2 VIRTUAL FOLD — the between-leg state delta per state class.

docs/SPEC_lane_spectrometer.md §4 / §12.4.  Reads the four legs emitted by
``scripts/spectro_make_vfold.py`` (``ctl`` / ``off`` / ``on`` / ``inert``) and reports:

 1. VALIDITY, before any number is interpreted:
    * ``off`` vs ``inert`` must be byte-identical on every artifact — the only test that
      the fixed-row branch is inert when its gate does not admit anything.  A difference
      here means the branch is load-bearing even when it fixes nothing, and every
      other number below is contaminated.
    * the DATUM (the gauge-pin set from ``network_phase_anchor.csv``) must be identical
      between the legs being differenced.  A between-leg Delta_x is only meaningful in a
      COMMON datum; if the fold flipped an observability / PD gate the pin set moves and
      the delta is a frame change, not a redistribution (standing invariant #5).
    * the legs must share bit-identical LEADING epochs (no rows exist before the arcs
      mature), which is what makes the first folding epoch an exact single-epoch read.
 2. The graded epoch: the FIRST with ``fixed=1`` rows in ``network_sd_ewl.csv``.
 3. Delta per state class at that epoch — satellite CLOCK / phase bias (PB) / code bias
    (CB) from ``network_solution.csv``, SH-VTEC coefficients from
    ``network_vtec_sh.csv``, ZWD from ``network_zwd.csv`` — each reported BOTH raw and
    de-medianed per constellation.  The de-medianed column is the datum-invariant one:
    a whole-constellation common mode is a frame shift the emitted product absorbs,
    while the SPREAD around it is the redistribution S2 exists to measure.
 4. Post-fit redistribution (``network_pf_keyed.csv`` sigma0) and the row/integer
    counters, so a null result can be told apart from a null EXECUTION.

Prediction to test, from the row algebra: the PB columns move first, because a
satellite's phase-bias leverage in the row is +24.00/-23.00 (GPS) or +39.33/-38.33
(Galileo) against +-0.51/0.49 on the code-bias columns.

Usage
-----
    python scripts/spectro_grade_vfold.py [--root runs/spectro_vfold]
"""
import argparse
import csv
import hashlib
import os
import statistics as st
import sys
from collections import defaultdict

LEGS = ('ctl', 'off', 'on', 'inert')


def sha(path):
    if not os.path.exists(path):
        return None
    h = hashlib.sha256()
    with open(path, 'rb') as fh:
        for blk in iter(lambda: fh.read(1 << 20), b''):
            h.update(blk)
    return h.hexdigest()


def logdir(root, leg):
    return os.path.join(root, leg, 'logs')


def read_rows(path, keep=None):
    if not os.path.exists(path):
        return []
    with open(path) as fh:
        return [r for r in csv.DictReader(fh) if keep is None or keep(r)]


def artifact_compare(root, a, b, label):
    """Per-file sha comparison of two legs' logs.  Wall-clock artifacts are named and
    excluded from the verdict rather than silently skipped."""
    WALL = {'network_run_summary.csv', 'network_service_health.csv'}
    # Log-only by construction, so a difference here is NOT contamination: the sd_ewl row
    # trace gains three columns when fix_enable is armed (an armed-only header, so an
    # increment-1 artifact set stays byte-identical), and the AR decision trace exists
    # ONLY when it is armed.  Neither is read by the solve.
    TRACE_ONLY = {'network_sd_ewl.csv', 'network_sd_ewl_ar.csv'}
    da, db = logdir(root, a), logdir(root, b)
    if not os.path.isdir(da) or not os.path.isdir(db):
        print('  %s: MISSING leg directory' % label)
        return None
    names = sorted(set(os.listdir(da)) | set(os.listdir(db)))
    diff, same, wall, trace, empty = [], [], [], [], 0
    for n in names:
        if not n.endswith('.csv'):
            continue
        pa, pb = os.path.join(da, n), os.path.join(db, n)
        ha, hb = sha(pa), sha(pb)
        if ha is None or hb is None:
            (trace if n in TRACE_ONLY else diff).append(n + ' (missing on one side)')
            continue
        if os.path.getsize(pa) == 0 and os.path.getsize(pb) == 0:
            empty += 1
            continue
        if ha == hb:
            same.append(n)
        elif n in WALL:
            wall.append(n)
        elif n in TRACE_ONLY:
            trace.append(n)
        else:
            diff.append(n)
    print('  %s: %d identical, %d wall-clock-only (%s), %d armed-only trace (%s), '
          '%d DIFFER%s, %d empty-both'
          % (label, len(same), len(wall), ','.join(wall) or '-',
             len(trace), ','.join(trace) or '-', len(diff),
             (': ' + ', '.join(diff)) if diff else '', empty))
    return diff


def first_fixed_epoch(root, leg):
    """(week, tow) of the first epoch whose sd_ewl trace folded a FIXED row."""
    p = os.path.join(logdir(root, leg), 'network_sd_ewl.csv')
    for r in read_rows(p):
        if r.get('fixed') == '1':
            return r['week'], r['tow']
    return None, None


def datum_identical(root, a, b, tow):
    """The gauge-pin set at `tow` must match, or Delta_x is a frame change."""
    def pins(leg):
        p = os.path.join(logdir(root, leg), 'network_phase_anchor.csv')
        return {(r['cons'], r['signal'], r['sat'], r['anchor_sta'])
                for r in read_rows(p, lambda r: r['tow'] == tow)}
    pa, pb = pins(a), pins(b)
    if not pa or not pb:
        return None, 0, 0
    return pa == pb, len(pa - pb), len(pb - pa)


def solution_delta(root, a, b, tow):
    """-> {kind: [(sat, signal, delta_m)]} from network_solution.csv at `tow`."""
    def load(leg):
        p = os.path.join(logdir(root, leg), 'network_solution.csv')
        out = {}
        for r in read_rows(p, lambda r: r['tow'] == tow):
            try:
                out[(r['kind'], r['sat'], r.get('signal', ''))] = float(r['value_m'])
            except (TypeError, ValueError):
                continue
        return out
    va, vb = load(a), load(b)
    per = defaultdict(list)
    for k in sorted(set(va) & set(vb)):
        per[k[0]].append((k[1], k[2], vb[k] - va[k]))
    return per


def report_class(name, rows, unit='m'):
    """Raw and de-medianed (per constellation) delta statistics for one state class."""
    if not rows:
        print('    %-6s (no common entries)' % name)
        return
    d = [x[2] for x in rows]
    ad = sorted(abs(x) for x in d)
    n = len(ad)
    bycon = defaultdict(list)
    for sat, sig, v in rows:
        bycon[sat[:1] if sat else '?'].append(v)
    med = {c: st.median(v) for c, v in bycon.items()}
    dm = [v - med[(sat[:1] if sat else '?')] for sat, sig, v in rows]
    adm = sorted(abs(x) for x in dm)
    worst = max(rows, key=lambda x: abs(x[2]))
    print('    %-6s n=%4d  |d| med %.4g  p90 %.4g  max %.4g %s   '
          '| de-medianed |d| med %.4g  max %.4g   | common-mode %s'
          % (name, n, ad[n // 2], ad[int(.9 * n)], ad[-1], unit,
             adm[n // 2], adm[-1],
             ' '.join('%s:%+.4g' % (c, m) for c, m in sorted(med.items()))))
    print('           worst: %s %s  %+.4g %s' % (worst[0], worst[1] or '-',
                                                 worst[2], unit))


def scalar_delta(root, a, b, tow, fname, keycols, valcol, label, unit):
    def load(leg):
        p = os.path.join(logdir(root, leg), fname)
        out = {}
        for r in read_rows(p, lambda r: r['tow'] == tow):
            try:
                out[tuple(r[c] for c in keycols)] = float(r[valcol])
            except (TypeError, ValueError, KeyError):
                continue
        return out
    va, vb = load(a), load(b)
    keys = sorted(set(va) & set(vb))
    if not keys:
        print('    %-6s (no common entries)' % label)
        return
    d = [vb[k] - va[k] for k in keys]
    ad = sorted(abs(x) for x in d)
    n = len(ad)
    w = max(keys, key=lambda k: abs(vb[k] - va[k]))
    print('    %-6s n=%4d  |d| med %.4g  p90 %.4g  max %.4g %s   worst %s %+.4g'
          % (label, n, ad[n // 2], ad[int(.9 * n)], ad[-1], unit,
             '/'.join(w), vb[w] - va[w]))


def counters(root, leg, tow):
    p = os.path.join(logdir(root, leg), 'network_sd_ewl.csv')
    rows = read_rows(p, lambda r: r['tow'] == tow)
    tot = defaultdict(int)
    for r in rows:
        tot[r['status']] += 1
        if r.get('fixed') == '1':
            tot['_fixed'] += 1
    ar = os.path.join(logdir(root, leg), 'network_sd_ewl_ar.csv')
    arr = read_rows(ar, lambda r: r['tow'] == tow)
    for r in arr:
        tot['ar_' + r['status']] += 1
    return tot


def sigma0(root, leg, tow):
    p = os.path.join(logdir(root, leg), 'network_pf_keyed.csv')
    rows = read_rows(p, lambda r: r['tow'] == tow and r['kind'] == 'STA')
    if not rows:
        return None
    c = [float(r['s0_code']) for r in rows if r.get('s0_code')]
    f = [float(r['s0_phase']) for r in rows if r.get('s0_phase')]
    return (st.median(c) if c else float('nan'),
            st.median(f) if f else float('nan'))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='C:/GnssProcessor/runs/spectro_vfold')
    args = ap.parse_args(argv[1:])
    root = args.root

    print('== S2 VIRTUAL FOLD (docs/SPEC_lane_spectrometer.md §4) ==')
    print('root %s' % root)
    for leg in LEGS:
        d = logdir(root, leg)
        print('  leg %-6s %s' % (leg, 'present' if os.path.isdir(d) else 'MISSING'))

    print('\n-- VALIDITY 1: inert arm must be byte-identical to the float arm --')
    inert_diff = artifact_compare(root, 'off', 'inert', 'off vs inert')
    print('-- reference deltas (expected NON-identical) --')
    artifact_compare(root, 'ctl', 'off', 'ctl vs off  (isolates the FLOAT layer)')
    artifact_compare(root, 'off', 'on', 'off vs on   (isolates THE INTEGER)')

    week, tow = first_fixed_epoch(root, 'on')
    if tow is None:
        print('\nNO FIXED ROW WAS EVER FOLDED in the `on` leg — the A/B is INERT and no '
              'delta can be read.  This is the Card-1e trap; read the run-end '
              '"Network (SD EWL integer fix)" lines for the named dominant cause '
              'before changing anything else.')
        return 1
    print('\n-- graded epoch: week %s tow %s (first with a FIXED row) --' % (week, tow))

    ok, only_a, only_b = datum_identical(root, 'off', 'on', tow)
    if ok is None:
        print('  DATUM CHECK SKIPPED — no network_phase_anchor.csv (arm '
              'network.trace_phase_anchor in the legs)')
    elif ok:
        print('  datum IDENTICAL between off and on (gauge-pin sets match) => the '
              'delta below is a redistribution in a COMMON frame')
    else:
        print('  *** DATUM DIFFERS between off and on (%d pins only in off, %d only in '
              'on) — the delta is a FRAME CHANGE, not a redistribution.  Standing '
              'invariant #5: do not read it as physics. ***' % (only_a, only_b))

    print('\n-- counters at the graded epoch --')
    for leg in ('off', 'on'):
        t = counters(root, leg, tow)
        print('  %-5s BUILT %d  fixed %d  | AR: FIXED %d LINK %d R0 %d FLIP %d '
              'NO_LINK %d' % (leg, t.get('BUILT', 0), t.get('_fixed', 0),
                              t.get('ar_FIXED', 0), t.get('ar_LINK', 0),
                              t.get('ar_R0', 0), t.get('ar_FLIP', 0),
                              t.get('ar_NO_LINK', 0)))
    for leg in ('ctl', 'off', 'on'):
        s = sigma0(root, leg, tow)
        if s:
            print('  %-5s post-fit sigma0 (median over stations): code %.4f  phase %.4f'
                  % (leg, s[0], s[1]))

    for a, b, what in (('ctl', 'off', 'FLOAT LAYER (ctl -> off)'),
                       ('off', 'on', 'THE INTEGER (off -> on)')):
        print('\n-- Delta per state class: %s --' % what)
        per = solution_delta(root, a, b, tow)
        for kind in ('CLK', 'PB', 'CB', 'URA', 'C1'):
            if kind in per:
                report_class(kind, per[kind])
        for kind in sorted(k for k in per if k not in ('CLK', 'PB', 'CB', 'URA', 'C1')):
            report_class(kind, per[kind])
        scalar_delta(root, a, b, tow, 'network_vtec_sh.csv',
                     ('n', 'm', 'trig'), 'value_tecu', 'SH', 'TECU')
        scalar_delta(root, a, b, tow, 'network_zwd.csv',
                     ('station',), 'zwd_m', 'ZWD', 'm')

    print('\n-- reading it --')
    print('  The row algebra predicts PB moves FIRST: per-satellite phase-bias leverage')
    print('  is +24.00/-23.00 (GPS) and +39.33/-38.33 (Galileo) against +-0.51/0.49 on')
    print('  the code-bias columns.  PB >> CB in the de-medianed column confirms the')
    print('  constraint reached the intended state family; CB >= PB means it did not,')
    print('  and a large COMMON MODE with a small de-medianed spread means the fold')
    print('  moved the frame rather than the estimable directions.')
    if inert_diff:
        print('  *** the inert arm DIFFERED — treat every number above as contaminated '
              'until that is explained. ***')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""SAME-REALIZATION A/B for the D5 v5 card: two legs off ONE snapshot, arm-to-arm.

Why this is not ``d5v5_seam_ab.py``.  That script grades each arm against the
continuous seed it resumed from, which works only inside the seed's own span.  The
pre-registered window here (+120..+240 epochs past a seed that ends at +116) has NO
seed overlap, so every channel is read DIRECTLY between the two arms instead.  For a
same-realization pair that is the stronger statistic anyway: both legs restore the
identical snapshot, so the ambiguity/datum realization, the station set and the
observation stream are common, and the arm-to-arm difference is the knob and nothing
else.  (The 6 h independent-cold-start twin could not say this — its phase-bias tail
was one satellite's level realized differently, the standing-invariant-#5 confound.)

Channels, all over the pre-registered epoch window:

  CONSUMER   ``logs/closed_loop_trace.csv``.  Two readings:
             - de-biased 3D mean, each arm de-biased by its OWN MEDIAN ENU over the
               window (a static coordinate realization at the hold-out station is not
               the knob — standing invariant #5);
             - the DIRECT arm-to-arm 3D separation per epoch, which the de-biased
               means cannot see: two arms that drift apart TOGETHER keep identical
               spreads.  Reported, not gated.
  WIRE       ``products.txt``.  Satellite clock as the DATUM-INVARIANT CONTRAST (each
             arm's per-epoch median over satellites removed FIRST, then differenced),
             and per-signal phase bias as raw |delta| — legitimate here only because
             the two arms share one restored datum realization.
  ZWD        ``logs/network_zwd.csv`` per station, |ON - OFF| on the stations and
             epochs present in both.
  STATUS     ``logs/network_epoch_status.csv`` dispositions, so withheld-epoch
             equality is checked rather than assumed.

Usage:
  python scripts/d5v5_same_realization_ab.py <off_rundir> <on_rundir>
         [--from 120] [--to 240] [--t0 <tow of leg epoch 0>]
"""
from __future__ import annotations

import argparse
import csv
import io
import os
import statistics as st
import sys
from collections import Counter

EPOCH_S = 30.0


def _rows(path):
    if not os.path.exists(path):
        return []
    with io.open(path, 'r', encoding='utf-8', errors='replace', newline='') as f:
        return list(csv.DictReader(f))


def _pct(xs, p):
    if not xs:
        return None
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p / 100.0 * len(xs)))]


def leg_t0(rundir):
    r = _rows(os.path.join(rundir, 'logs', 'network_epoch_status.csv'))
    return min(float(x['tow']) for x in r) if r else None


def consumer(rundir, t0, lo, hi):
    """(epoch -> ENU) over the window, plus the arm's own median-de-biased 3D mean."""
    out = {}
    for r in _rows(os.path.join(rundir, 'logs', 'closed_loop_trace.csv')):
        ep = int(round((float(r['tow']) - t0) / EPOCH_S))
        if lo <= ep <= hi:
            out[ep] = (float(r['east_m']), float(r['north_m']), float(r['up_m']))
    if not out:
        return out, None, None
    med = [st.median(c) for c in zip(*out.values())]
    deb = [sum((v - m) ** 2 for v, m in zip(s, med)) ** 0.5 for s in out.values()]
    return out, st.mean(deb), med


def products(path, t0, lo, hi):
    """(epoch -> {sat: clock_m}), (epoch -> {(sat,signal): phase_bias_m}).

    Payload rows carry no time tag; they are written as a block under the epoch's
    H/E header, so the epoch is carried forward from the last header seen.
    """
    C, P, tow = {}, {}, None
    with io.open(path, 'r', encoding='utf-8', errors='replace') as f:
        for ln in f:
            p = ln.rstrip('\n').split(',')
            if not p or p[0].startswith('#'):
                continue
            if p[0] in ('H', 'E', 'F', 'G') and len(p) >= 3:
                try:
                    tow = float(p[2])
                except ValueError:
                    tow = None
            elif tow is not None and p[0] == 'C' and len(p) >= 3:
                ep = int(round((tow - t0) / EPOCH_S))
                if lo <= ep <= hi:
                    C.setdefault(ep, {})[p[1]] = float(p[2])
            elif tow is not None and p[0] == 'P' and len(p) >= 4:
                ep = int(round((tow - t0) / EPOCH_S))
                if lo <= ep <= hi:
                    P.setdefault(ep, {})[(p[1], p[2])] = float(p[3])
    return C, P


def zwd(rundir, t0, lo, hi):
    out = {}
    for r in _rows(os.path.join(rundir, 'logs', 'network_zwd.csv')):
        ep = int(round((float(r['tow']) - t0) / EPOCH_S))
        if lo <= ep <= hi:
            try:
                out.setdefault(ep, {})[r['station']] = float(r['zwd_m'])
            except (TypeError, ValueError):
                pass
    return out


def dispositions(rundir):
    c = Counter((r.get('disposition') or '').strip()
                for r in _rows(os.path.join(rundir, 'logs', 'network_epoch_status.csv')))
    return c, sum(v for k, v in c.items() if 'withh' in k or 'fail' in k)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('off')
    ap.add_argument('on')
    ap.add_argument('--from', dest='lo', type=int, default=120)
    ap.add_argument('--to', dest='hi', type=int, default=240)
    ap.add_argument('--t0', type=float)
    a = ap.parse_args()
    t0 = a.t0 if a.t0 is not None else leg_t0(a.off)
    lo, hi = a.lo, a.hi
    ok = True

    def gate(name, passed, detail):
        nonlocal ok
        ok &= bool(passed)
        print('   [%s] %-44s %s' % ('PASS' if passed else 'FAIL', name, detail))

    print('window +%d..+%d (leg epoch 0 = tow %.5f)\n' % (lo, hi, t0))

    # ---- consumer -------------------------------------------------------------
    ca, da, ma = consumer(a.off, t0, lo, hi)
    cb, db, mb = consumer(a.on, t0, lo, hi)
    common = sorted(set(ca) & set(cb))
    sep = [sum((ca[e][i] - cb[e][i]) ** 2 for i in range(3)) ** 0.5 for e in common]
    print('  CONSUMER  n_off=%d n_on=%d common=%d' % (len(ca), len(cb), len(common)))
    print('    median ENU OFF %s' % ['%.4f' % v for v in ma])
    print('    median ENU ON  %s' % ['%.4f' % v for v in mb])
    print('    de-biased 3D mean   OFF %.4f   ON %.4f   m' % (da, db))
    print('    arm-to-arm 3D sep   mean %.4f  p50 %.4f  p95 %.4f  max %.4f  m'
          % (st.mean(sep), _pct(sep, 50), _pct(sep, 95), max(sep)))
    rel = abs(db - da) / da if da else None
    gate('consumer de-biased 3D mean ON within 5%%', rel is not None and rel <= 0.05,
         'OFF %.4f -> ON %.4f  = %+.2f%%' % (da, db, 100 * (db - da) / da))

    # ---- wire -----------------------------------------------------------------
    Ca, Pa = products(os.path.join(a.off, 'products.txt'), t0, lo, hi)
    Cb, Pb = products(os.path.join(a.on, 'products.txt'), t0, lo, hi)
    clk = []
    for e in set(Ca) & set(Cb):
        qa, qb = st.median(Ca[e].values()), st.median(Cb[e].values())
        clk += [abs((Ca[e][s] - qa) - (Cb[e][s] - qb)) for s in set(Ca[e]) & set(Cb[e])]
    pb = []
    for e in set(Pa) & set(Pb):
        pb += [abs(Pa[e][k] - Pb[e][k]) for k in set(Pa[e]) & set(Pb[e])]
    za, zb = zwd(a.off, t0, lo, hi), zwd(a.on, t0, lo, hi)
    zd = []
    for e in set(za) & set(zb):
        zd += [abs(za[e][s] - zb[e][s]) for s in set(za[e]) & set(zb[e])]
    print('\n  WIRE / ZWD  |ON - OFF| pooled over the window')
    for tag, xs, bar in (('clock contrast (datum-free)', clk, 0.02),
                         ('phase bias', pb, 0.01),
                         ('station ZWD', zd, 0.02)):
        if not xs:
            print('    %-28s (no overlap)' % tag)
            continue
        print('    %-28s n=%-7d p50 %.5f  p95 %.5f  max %.4f  m'
              % (tag, len(xs), _pct(xs, 50), _pct(xs, 95), max(xs)))
    print()
    for tag, xs, bar in (('clock contrast p50', clk, 0.02),
                         ('PB wire p50', pb, 0.01),
                         ('ZWD p50', zd, 0.02)):
        if xs:
            gate('%s within %g m' % (tag, bar), _pct(xs, 50) <= bar,
                 '%.5f m' % _pct(xs, 50))

    # ---- status ---------------------------------------------------------------
    dA, wA = dispositions(a.off)
    dB, wB = dispositions(a.on)
    print('\n  STATUS  OFF %s\n          ON  %s' % (dict(dA), dict(dB)))
    gate('withheld epochs equal', wA == wB, 'OFF %d / ON %d' % (wA, wB))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

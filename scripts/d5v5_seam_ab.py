#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""A/B two ``ev_seam_grade.py`` JSONs on the V3 no-harm channels of the D5 v5 card.

``ev_seam_grade.py`` grades ONE leg against the continuous seed it resumed from, so
each arm's numbers carry the same resume-seam transient.  Differencing the two arms'
readings cancels that transient and leaves the knob's own effect, which is what the
pre-registered V3 bars are written against:

  clock contrast pooled p50   within 0.02 m   (datum-invariant, standing invariant #5)
  PB wire        pooled p50   within 0.01 m   (median over the per-epoch p50s; the
                                               pooled raw sample is not kept in the JSON)
  ZWD            pooled p50   within 0.02 m
  withheld epochs             equal

The clock contrast is the datum-free one ev_seam_grade already forms (each arm's
per-epoch median over satellites removed before differencing), so a common frame
drift cannot enter either arm's number.

Usage:  python scripts/d5v5_seam_ab.py off.json on.json
"""
from __future__ import annotations

import json
import sys


def _median(xs):
    xs = sorted(x for x in xs if x is not None)
    if not xs:
        return None
    n = len(xs)
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])


def _g(d, *path):
    for p in path:
        if not isinstance(d, dict) or p not in d:
            return None
        d = d[p]
    return d


def pb_pooled(doc, k_from=5):
    """Median over the per-epoch PB p50s from +k_from on (G5 keeps no pooled PB)."""
    rows = doc.get('rows') or []
    return _median([r.get('pb_p50') for r in rows
                    if r.get('k') is not None and r['k'] >= k_from])


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    A, B = (json.load(open(p, encoding='utf-8')) for p in sys.argv[1:3])
    checks = [
        ('clock contrast p50 (m)', _g(A, 'gates', 'G5', 'clk_p50'),
         _g(B, 'gates', 'G5', 'clk_p50'), 0.02),
        ('PB wire p50 (m)', pb_pooled(A), pb_pooled(B), 0.01),
        ('ZWD p50 (m)', _g(A, 'gates', 'G5', 'zwd_p50'),
         _g(B, 'gates', 'G5', 'zwd_p50'), 0.02),
        ('code bias p50 (m)', _g(A, 'gates', 'G5', 'cb_p50'),
         _g(B, 'gates', 'G5', 'cb_p50'), None),
        ('clock contrast p95 (m)', _g(A, 'gates', 'G5', 'clk_p95'),
         _g(B, 'gates', 'G5', 'clk_p95'), None),
        ('ZWD p95 (m)', _g(A, 'gates', 'G5', 'zwd_p95'),
         _g(B, 'gates', 'G5', 'zwd_p95'), None),
        ('worst |d_a00| from +5 (TECU)', _g(A, 'gates', 'G3', 'a00_worst'),
         _g(B, 'gates', 'G3', 'a00_worst'), 0.5),
        ('births +5..end', _g(A, 'gates', 'G4', 'births_leg_5_end'),
         _g(B, 'gates', 'G4', 'births_leg_5_end'), None),
    ]
    print('  %-32s %12s %12s %12s %s' % ('channel', 'OFF', 'ON', 'ON-OFF', 'bar'))
    ok = True
    for name, a, b, bar in checks:
        if a is None or b is None:
            print('  %-32s %12s %12s %12s %s' % (name, a, b, 'n/a', bar or '-'))
            continue
        d = b - a
        verdict = ''
        if bar is not None:
            passed = abs(d) <= bar
            ok &= passed
            verdict = '[%s] +-%g' % ('PASS' if passed else 'FAIL', bar)
        print('  %-32s %12.5f %12.5f %12.5f %s' % (name, a, b, d, verdict))

    aw = len(_g(A, 'gates', 'withheld_k') or [])
    bw = len(_g(B, 'gates', 'withheld_k') or [])
    ae = _g(A, 'gates', 'n_emitted')
    be = _g(B, 'gates', 'n_emitted')
    print('  %-32s %12s %12s' % ('withheld epochs', aw, bw))
    print('  %-32s %12s %12s' % ('emitted epochs', ae, be))
    ok &= (aw == bw)
    print('  [%s] V3f withheld epochs equal' % ('PASS' if aw == bw else 'FAIL'))
    # PB "silent step" epochs — a > 5 cm move on > 10 % of keys with no declared R.
    ap = len(_g(A, 'gates', 'G5', 'pb_bad_epochs') or [])
    bp = len(_g(B, 'gates', 'G5', 'pb_bad_epochs') or [])
    print('  %-32s %12s %12s   (report only)' % ('PB >5cm-on->10%-keys epochs', ap, bp))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

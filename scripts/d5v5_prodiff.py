#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Reports which record kinds of two ``products.txt`` wires differ, and by how much.

A sha mismatch says "not byte-identical" and nothing else.  For a default-OFF knob
the useful statement is *which* channel moved and by what magnitude, so this keys
each record by its identity fields, aligns the two files on that key, and reports
per kind: keys only in A, keys only in B, keys present in both whose numeric fields
differ, and the worst |Δ| per numeric column.

Record kinds in ``gnss_network_product v2`` (kind = field 0):
  S  the run's self-describing header (one row)
  C  satellite clock       B  satellite code bias (per signal)
  P  satellite phase bias  U/H/E  auxiliary / health / ephemeris-like rows
  R  declared discontinuity bumps

Usage:  python scripts/d5v5_prodiff.py A/products.txt B/products.txt [--kinds C,B,P]
"""
from __future__ import annotations

import argparse
import io
import sys
from collections import defaultdict


def load(path):
    """kind -> {key: (numeric fields, raw line)}.

    The wire carries no time tag on the payload rows (C/B/P/U/R): they are written as a
    block under the epoch's ``H``/``E`` header, so the epoch must be carried forward
    from the last header seen, or every epoch's rows collapse onto one key.  The key
    is (epoch tow, the row's non-numeric identity fields) and the value is the row's
    numeric fields, which is what gets differenced.
    """
    out = defaultdict(dict)
    tow = None
    with io.open(path, 'r', encoding='utf-8', errors='replace') as f:
        for ln in f:
            ln = ln.rstrip('\n')
            if not ln or ln.startswith('#'):
                continue
            p = ln.split(',')
            kind = p[0]
            if kind in ('H', 'E', 'F', 'G') and len(p) >= 3:
                try:
                    tow = float(p[2])
                except ValueError:
                    pass
            nums, ident = [], []
            for i, tok in enumerate(p[1:], start=1):
                # Self-describing 'key=value' tokens (sigma=, sigcc=, iode=, ...) are
                # values, not identity. Leaving them in the key would make every row with
                # a moved sigma read as "only in A" plus "only in B" instead of as one
                # differing row.
                if '=' in tok:
                    kk, _, vv = tok.partition('=')
                    try:
                        nums.append(('%s' % kk, float(vv)))
                        continue
                    except ValueError:
                        ident.append((i, tok))
                        continue
                try:
                    nums.append((i, float(tok)))
                except ValueError:
                    ident.append((i, tok))
            key = (tow,) + tuple(ident)
            if kind in ('H', 'E', 'F', 'G'):
                # Header rows carry their own tow in the numeric list; drop the
                # week/tow pair from the compared values so the key is not diffed.
                nums = [(i, v) for i, v in nums if not (isinstance(i, int) and i <= 2)]
            if key in out[kind]:          # same identity twice in one epoch
                key = key + ('dup%d' % len(out[kind]),)
            out[kind][key] = (nums, ln)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--kinds', default='')
    ap.add_argument('--show', type=int, default=3, help='example differing lines per kind')
    args = ap.parse_args()
    A, B = load(args.a), load(args.b)
    kinds = args.kinds.split(',') if args.kinds else sorted(set(A) | set(B))
    print('%-4s %8s %8s %9s %9s %9s   %s' %
          ('kind', 'nA', 'nB', 'A-only', 'B-only', 'differ', 'worst |delta| per column'))
    for k in kinds:
        a, b = A.get(k, {}), B.get(k, {})
        only_a = len(set(a) - set(b))
        only_b = len(set(b) - set(a))
        differ, worst = 0, defaultdict(float)
        examples = []
        for key in set(a) & set(b):
            na, nb = a[key][0], b[key][0]
            if len(na) != len(nb):
                differ += 1
                continue
            d = False
            for (ia, va), (ib, vb) in zip(na, nb):
                if va != vb:
                    d = True
                    worst[ia] = max(worst[ia], abs(va - vb))
            if d:
                differ += 1
                if len(examples) < args.show:
                    examples.append((a[key][1], b[key][1]))
        ws = ' '.join(('f%s:%.4g' % (i, v)) if isinstance(i, int) else ('%s:%.4g' % (i, v))
                      for i, v in sorted(worst.items(), key=lambda x: -x[1])[:6])
        print('%-4s %8d %8d %9d %9d %9d   %s' % (k, len(a), len(b), only_a, only_b, differ, ws))
        for ea, eb in examples:
            print('      A: %s' % ea[:150])
            print('      B: %s' % eb[:150])
    return 0


if __name__ == '__main__':
    sys.exit(main())

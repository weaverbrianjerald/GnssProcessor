#!/usr/bin/env python3
"""Same-posture identity checks of card ZTL section 6 (G2b, G2c).

usage:
  ztl_identity.py same <run A> <run B>
      products.txt without '#' lines and every logs/*.csv present in either run, with the
      wall-clock columns blanked (network_datum_graph.csv ms; network_run_summary.csv loop_ms and
      ms_per_epoch; network_service_health.csv ms_per_epoch_window).  Prints one line per file and
      exits 1 when anything differs.
  ztl_identity.py prefix <base run> <new run> [--declared ZWD:recouple,ZWD:first_fold,ZWD:gap_prune]
      For the measure-only check: finds the first epoch (TOW) at which products.txt differs, then
      lists the rows of logs/network_events.csv that are in one run and not the other before that
      epoch, by tier and event, separating the declared rows from the rest; and the first
      undeclared events row of the new run at or after that epoch.
"""
import collections
import csv
import io
import os
import sys

WALL = {'network_datum_graph.csv': {'ms'},
        'network_run_summary.csv': {'loop_ms', 'ms_per_epoch'},
        'network_service_health.csv': {'ms_per_epoch_window'}}


def products(rd):
    with open(os.path.join(rd, 'products.txt'), encoding='utf-8', errors='replace') as f:
        return [l for l in f if not l.startswith('#')]


def csv_norm(path, name):
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        text = f.read()
    blank = WALL.get(name)
    if not blank:
        return text
    rows = list(csv.reader(io.StringIO(text)))
    if not rows:
        return text
    idx = [i for i, h in enumerate(rows[0]) if h in blank]
    for r in rows[1:]:
        for i in idx:
            if i < len(r):
                r[i] = ''
    return '\n'.join(','.join(r) for r in rows)


def same(a, b):
    bad = 0
    pa, pb = products(a), products(b)
    ok = pa == pb
    bad += not ok
    print(f"products.txt (no '#'): {'identical' if ok else 'DIFFERS'} ({len(pa)} / {len(pb)} lines)")
    la = {f for f in os.listdir(os.path.join(a, 'logs')) if f.endswith('.csv')}
    lb = {f for f in os.listdir(os.path.join(b, 'logs')) if f.endswith('.csv')}
    n_same = 0
    for f in sorted(la | lb):
        if f not in la or f not in lb:
            print(f'  {f}: only in {"A" if f in la else "B"}')
            bad += 1
            continue
        if csv_norm(os.path.join(a, 'logs', f), f) == csv_norm(os.path.join(b, 'logs', f), f):
            n_same += 1
        else:
            print(f'  {f}: DIFFERS')
            bad += 1
    print(f'logs/*.csv: {n_same} of {len(la | lb)} identical')
    return 1 if bad else 0


def epoch_tow(line):
    # The epoch header record of products.txt: "H,<week>,<tow>,...".
    p = line.split(',')
    if p[0] == 'H' and len(p) > 2:
        return round(float(p[2]), 1)
    return None


def events(rd):
    with open(os.path.join(rd, 'logs', 'network_events.csv'), encoding='utf-8', newline='') as f:
        return list(csv.reader(f))


def prefix(a, b, declared):
    pa, pb = products(a), products(b)
    first = None
    for k, (x, y) in enumerate(zip(pa, pb)):
        if x != y:
            first = k
            break
    if first is None and len(pa) == len(pb):
        print('products.txt identical over the whole run')
        t_div = float('inf')
    else:
        k = first if first is not None else min(len(pa), len(pb))
        t_div = None
        for j in range(k, -1, -1):
            t = epoch_tow(pa[j]) if j < len(pa) else None
            if t is not None:
                t_div = t
                break
        print(f'products.txt first differs at line {k + 1} (epoch TOW {t_div})')
    ea, eb = events(a), events(b)
    hdr = ea[0]
    ti, ei = hdr.index('tow'), hdr.index('event')
    tier_i = hdr.index('tier')
    ca = collections.Counter(tuple(r) for r in ea[1:] if float(r[ti]) < t_div)
    cb = collections.Counter(tuple(r) for r in eb[1:] if float(r[ti]) < t_div)
    only_b = cb - ca
    only_a = ca - cb
    by = collections.Counter()
    undeclared = []
    for r, n in only_b.items():
        key = f'{r[tier_i]}:{r[ei]}'
        by[key] += n
        if key not in declared:
            undeclared.append(r)
    print(f'events rows before TOW {t_div}: {sum(only_b.values())} only in the new run, '
          f'{sum(only_a.values())} only in the base run')
    for k_, n in sorted(by.items()):
        print(f'   new {k_}: {n}{"" if k_ in declared else "  <- UNDECLARED"}')
    for r in list(only_a)[:10]:
        print('   base-only:', ','.join(r))
    for r in undeclared[:10]:
        print('   undeclared:', ','.join(r))
    if t_div != float('inf'):
        nxt = [r for r in eb[1:] if float(r[ti]) >= t_div and f'{r[tier_i]}:{r[ei]}' not in declared
               and tuple(r) not in set(tuple(x) for x in ea[1:] if float(x[ti]) == float(r[ti]))]
        print('first undeclared new-run rows at or after the divergence:')
        for r in nxt[:8]:
            print('   ', ','.join(r))
    return 0 if not undeclared and not only_a else 1


def main():
    if sys.argv[1] == 'same':
        sys.exit(same(sys.argv[2], sys.argv[3]))
    if sys.argv[1] == 'prefix':
        declared = {'ZWD:recouple', 'ZWD:first_fold', 'ZWD:gap_prune'}
        if '--declared' in sys.argv:
            declared = set(sys.argv[sys.argv.index('--declared') + 1].split(','))
        sys.exit(prefix(sys.argv[2], sys.argv[3], declared))
    print(__doc__)
    sys.exit(2)


if __name__ == '__main__':
    main()

"""Card SMH G0: compare two runs' products.txt record by record.

    python scripts/clock_level/smh_g0.py <run_a> <run_b>

Reports: records identical per type; P records whose value differs (and whether their
generation stamp agrees); the S line difference.  A mode that is emit-only must leave
every non-P record identical and change P values only (generations included in the
comparison).  Also compares closed_loop_trace.csv byte for byte.
"""
import os, sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'


def resolve(a):
    for c in (a, f'{RUNS}/{a}', f'{RUNS}/_rap_{a}'):
        if os.path.isdir(c):
            return c
    raise SystemExit('no run ' + a)


def main():
    a, b = resolve(sys.argv[1]), resolve(sys.argv[2])
    la = open(os.path.join(a, 'products.txt'), encoding='utf-8').read().splitlines()
    lb = open(os.path.join(b, 'products.txt'), encoding='utf-8').read().splitlines()
    print(f'{os.path.basename(a)}: {len(la)} lines, {os.path.basename(b)}: {len(lb)} lines')
    n = min(len(la), len(lb))
    same, diff = Counter(), Counter()
    pdiff_gen_same = 0
    pmax = 0.0
    for i in range(n):
        x, y = la[i], lb[i]
        t = x[:2]
        if x == y:
            same[t] += 1
            continue
        diff[t] += 1
        if t == 'P,' and y[:2] == 'P,':
            fx, fy = x.split(','), y.split(',')
            if fx[:3] == fy[:3] and fx[4:] == fy[4:]:
                pdiff_gen_same += 1
                try:
                    pmax = max(pmax, abs(float(fx[3]) - float(fy[3])))
                except ValueError:
                    pass
        if t == 'S,':
            print('S a:', x[-80:])
            print('S b:', y[-80:])
    print('identical:', dict(same))
    print('different:', dict(diff))
    print(f'P records differing in value only (key and generation equal): {pdiff_gen_same}, '
          f'max |dP| {pmax:.6f} m')
    ca = os.path.join(a, 'logs', 'closed_loop_trace.csv')
    cb = os.path.join(b, 'logs', 'closed_loop_trace.csv')
    for p in (ca, cb):
        if not os.path.exists(p):
            ca = os.path.join(a, 'closed_loop_trace.csv')
            cb = os.path.join(b, 'closed_loop_trace.csv')
    if os.path.exists(ca) and os.path.exists(cb):
        print('closed_loop_trace identical:', open(ca, 'rb').read() == open(cb, 'rb').read())


if __name__ == '__main__':
    main()

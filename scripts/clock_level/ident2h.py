#!/usr/bin/env python3
"""Windowed identity of two runs on their first N epochs, post-OWD rules:
products.txt records only ('#' comments and the 'S' contract line are header), cut at the
(N+1)-th epoch's 'H' heartbeat; closed_loop_trace.csv without its n_sats column (card OWD O4:
it now counts admitted systems only); network_epoch_status.csv verbatim.
    python scripts/clock_level/ident2h.py <run A> <run B> [N=240]"""
import csv, hashlib, io, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
RUNS = ROOT + '/runs'


def h(lines):
    m = hashlib.sha256()
    for l in lines:
        m.update(l.encode('utf-8')); m.update(b'\n')
    return m.hexdigest()[:16]


def products(d, n):
    out, blocks = [], 0
    with io.open(os.path.join(d, 'products.txt'), encoding='utf-8', errors='replace') as f:
        for ln in f:
            if ln.startswith('#') or ln.startswith('S,'):
                continue
            s = ln.rstrip('\n').rstrip('\r')
            if s.startswith('H,'):
                blocks += 1
                if blocks > n:
                    break
            out.append(s)
    return out


def trace(d, n, drop=()):
    p = os.path.join(d, 'logs', 'closed_loop_trace.csv') if 'closed' in drop else None
    return p


def cl(d, n):
    rows = list(csv.reader(open(os.path.join(d, 'logs', 'closed_loop_trace.csv'))))
    hdr = rows[0]
    keep = [i for i, c in enumerate(hdr) if c != 'n_sats']
    return [','.join(r[i] for i in keep if i < len(r)) for r in rows[:n + 1]]


def es(d, n):
    with open(os.path.join(d, 'logs', 'network_epoch_status.csv')) as f:
        return [l.rstrip('\n') for l in f][:n + 1]


def main():
    a, b = [x if os.path.isabs(x) else os.path.join(RUNS, x) for x in sys.argv[1:3]]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 240
    ok = True
    for name, fn in (('products', products), ('closed_loop', cl), ('epoch_status', es)):
        la, lb = fn(a, n), fn(b, n)
        same = (la == lb)
        ok &= same
        first = next((i for i, (x, y) in enumerate(zip(la, lb)) if x != y), None)
        print(f'{name:13s} {h(la)}/{len(la)}  {h(lb)}/{len(lb)}  {"IDENTICAL" if same else "DIFFER at line " + str(first)}')
        if not same and first is not None:
            print('   A:', la[first][:160]); print('   B:', lb[first][:160])
    print('RESULT', 'IDENTICAL' if ok else 'DIFFERENT')


if __name__ == '__main__':
    main()

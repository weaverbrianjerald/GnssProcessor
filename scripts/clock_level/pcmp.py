"""Compare the P records (and optionally everything else) of two product files.

    python scripts/clock_level/pcmp.py <a/products.txt> <b/products.txt>
Prints the number of P records, the max |bias difference|, how many differ by > 1e-12, and whether
every non-P, non-comment line is identical.
"""
import sys


def load(p):
    pv, other, tow = {}, [], None
    for ln in open(p, encoding='utf-8', errors='replace'):
        ln = ln.rstrip('\n')
        if not ln or ln[0] == '#':
            continue
        if ln.startswith('E,'):
            tow = round(float(ln.split(',')[2]), 3)
        if ln.startswith('P,') and tow is not None:
            f = ln.split(',')
            pv[(tow, f[1], f[2])] = (float(f[3]), ','.join(f[4:]))
        else:
            other.append(ln)
    return pv, other


a, b = load(sys.argv[1]), load(sys.argv[2])
ka, kb = set(a[0]), set(b[0])
common = ka & kb
dmax, n_bad, n_tok = 0.0, 0, 0
worst = None
for k in common:
    d = abs(a[0][k][0] - b[0][k][0])
    if d > dmax:
        dmax, worst = d, k
    if d > 1e-12:
        n_bad += 1
    if a[0][k][1] != b[0][k][1]:
        n_tok += 1
print(f'P records a {len(ka)} b {len(kb)} common {len(common)}; max|diff| {dmax:.3e} at {worst}; '
      f'>1e-12: {n_bad}; token mismatches {n_tok}')
oa = [x for x in a[1] if not x.startswith('S,')]
ob = [x for x in b[1] if not x.startswith('S,')]
print('non-P, non-S lines identical:', oa == ob, len(oa), len(ob))
sa = [x for x in a[1] if x.startswith('S,')]
sb = [x for x in b[1] if x.startswith('S,')]
print('S lines:', sa[:1], sb[:1])

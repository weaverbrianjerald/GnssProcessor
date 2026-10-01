"""P records of two product files that differ by more than 1e-9 m, grouped by key: the number
of keys, then the 12 keys with the largest difference (epochs, first tow, max |d|, last d).
    python scripts/clock_level/pkeydiff.py <a/products.txt> <b/products.txt>"""
import sys
from collections import defaultdict
def load(p):
    out = {}; tow = None
    for line in open(p, errors='replace'):
        if line[0] == 'E': tow = round(float(line.split(',')[2]), 1)
        elif line[0] == 'P' and tow is not None:
            pr = line.split(','); out[(tow, pr[1], pr[2])] = float(pr[3])
    return out
a = load(sys.argv[1]); b = load(sys.argv[2])
by = defaultdict(list)
for k, v in a.items():
    if k in b and abs(v - b[k]) > 1e-9: by[(k[1], k[2])].append((k[0], v - b[k]))
print(len(by), 'keys differ')
for key, v in sorted(by.items(), key=lambda kv: -max(abs(d) for t, d in kv[1]))[:12]:
    print(key, len(v), 'epochs, first tow', v[0][0], 'max |d|', f'{max(abs(d) for t, d in v):.4f}', 'last d', f'{v[-1][1]:+.4f}')

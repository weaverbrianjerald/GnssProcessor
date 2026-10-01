"""Card SMD: a preview wire on the phase-clock hold's wire with the block clones' passed motion.

    python scripts/clock_level/smd_ctlwire.py <lah_run> <blk_run> <out_dir> <column> [<alpha>]

On every held key, P = P_LAH - (frozen + Phi), where Phi is the trace column <column> of the
block member's run (phi_m = Phi_D, the member itself; b_phic_m = the block's clock row, the
lifetime control) on the epochs the block is live, and frozen is the sum of the final Phi of the
key's earlier block clones in the same hold generation (the hold keeps the transfer continuous at
every retirement, so what a clone had reached stays in the key's transfer).  On an epoch with no
live block the key carries LAH's transfer plus that frozen sum.

With <column> = phi_m the output is the block member's own wire rebuilt from LAH's; the script
then compares it with the block run's products.txt P records (the attribution check).
"""
import bisect, collections, csv, os, sys

lah, blk, out, col = sys.argv[1:5]
alpha = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0   # the shrinkage of the passed motion (card QPV)
rows = collections.defaultdict(list)   # (sat, sig) -> [(tow, gen, t0, phi)]
for r in csv.DictReader(open(blk + '/logs/network_pb_hold_clone.csv')):
    v = r.get(col, '')
    if not v:
        continue
    tow = round(float(r['tow']), 1)
    age = float(r['age_s'] if col == 'phi_m' else r['b_age_s'])
    rows[(r['sat'], r['sig'])].append((tow, int(r['gen']), round(tow - age, 1), float(v)))
# Per key, the value to subtract at each tow: frozen + Phi on live epochs; the frozen sum is
# carried to the bridge epochs after a clone's last live emission.
events = {}
for key, lst in rows.items():
    lst.sort()
    seq = []            # (tow, subtract_on_this_tow, frozen_after)
    frozen, gen, t0, last = 0.0, None, None, None
    for tow, g, c0, phi in lst:
        if g != gen:
            frozen, gen, t0, last = 0.0, g, c0, None
        elif c0 != t0:
            frozen += last if last is not None else 0.0
            t0 = c0
        seq.append((tow, g, frozen + phi, frozen + phi))
        last = phi
    events[key] = seq
towlists = {k: [e[0] for e in v] for k, v in events.items()}


def subtract(key, tow, gen):
    """The amount to subtract from LAH's P at (key, tow): the live value, else the frozen sum
    (the last live emission's frozen + Phi) of the same generation, else 0."""
    seq = events.get(key)
    if not seq:
        return 0.0
    i = bisect.bisect_right(towlists[key], tow) - 1
    if i < 0:
        return 0.0
    t, g, val, after = seq[i]
    if t == tow:
        return val
    return after if g == gen else 0.0


def pgen(f):
    """The P record's hold generation (its gen= token), or None."""
    for x in reversed(f):
        if x.startswith('gen='):
            return int(x[4:])
    return None


os.makedirs(out + '/logs', exist_ok=True)
n = 0
tow = None
with open(out + '/products.txt', 'w', encoding='utf-8', newline='\n') as fo:
    for ln in open(lah + '/products.txt', encoding='utf-8'):
        if ln[0] == 'E':
            tow = round(float(ln.split(',')[2]), 1)
        if ln[0] == 'P' and tow is not None:
            f = ln.rstrip('\n').split(',')
            key = (f[1], f[2])
            d = alpha * subtract(key, tow, pgen(f))
            if d != 0.0:
                f[3] = repr(float(f[3]) - d)
                n += 1
                ln = ','.join(f) + '\n'
        fo.write(ln)
print(n, 'P records moved;', len(events), 'keys')
if col == 'phi_m' and alpha == 1.0:
    # The attribution check against the block run's own wire.
    def pvals(path):
        out_, tw = {}, None
        for ln in open(path, encoding='utf-8'):
            if ln[0] == 'E':
                tw = round(float(ln.split(',')[2]), 1)
            elif ln[0] == 'P' and tw is not None:
                f = ln.rstrip('\n').split(',')
                out_[(tw, f[1], f[2])] = float(f[3])
        return out_
    a, b = pvals(out + '/products.txt'), pvals(blk + '/products.txt')
    dmax = max((abs(a[k] - b[k]) for k in a if k in b), default=0.0)
    print('rebuilt vs the block wire: max |dP|', f'{dmax:.3e}', 'm over', sum(1 for k in a if k in b))

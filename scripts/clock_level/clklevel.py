#!/usr/bin/env python3
"""Clock LEVEL grade on the sim bed (the a priori clock is the truth, so the wire's C record
is the clock error): rms across GPS satellites of the per-satellite clock error, raw and with the
per-epoch median removed, at chosen epochs; the per-satellite median ionosphere-free code
residual over the last hour of the window (sd and range across satellites); the consumer 3D p50
by hour and the count below 0.1 m.
    python scripts/clock_level/clklevel.py <run dir> [<run dir> ...] [--datumfree]"""
import csv, os, statistics as st, sys
from collections import defaultdict
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
T0 = 259200.0
F1 = 1575.42e6; F2 = 1227.60e6; A = F1 * F1 / (F1 * F1 - F2 * F2); B = F2 * F2 / (F1 * F1 - F2 * F2)
EPOCHS = [0, 3, 7, 15, 59, 119, 239, 479, 719]


def clk(d):
    tow = None; c = defaultdict(dict); tows = []
    with open(os.path.join(d, 'products.txt'), errors='replace') as f:
        for line in f:
            if line[0] == '#':
                continue
            if line[0] == 'E':
                pr = line.split(','); tow = round(float(pr[2]), 3); tows.append(tow)
            elif line[0] == 'C' and tow is not None:
                pr = line.split(',')
                if pr[1][0] == 'G':
                    c[pr[1]][tow] = float(pr[2])
    return c, tows


def ifres(d, t_from, t_to):
    pend = {}; per = defaultdict(list)
    with open(os.path.join(d, 'logs', 'network_pf_rows.csv')) as f:
        for r in csv.DictReader(f):
            if r['kind'] != 'code' or r['sat'][0] != 'G':
                continue
            try:
                v = float(r['resid_m']); w = float(r['w']); t = float(r['tow'])
            except ValueError:
                continue
            if w < 1e-2 or not (t_from <= t - T0 < t_to):
                continue
            k = (r['station'], r['sat'], t)
            if r['signal'] == 'C1C':
                pend[k] = (v, pend.get(k, (None, None))[1])
            elif r['signal'] == 'C2W':
                pend[k] = (pend.get(k, (None, None))[0], v)
            else:
                continue
            a, b = pend[k]
            if a is not None and b is not None:
                per[r['sat']].append(A * a - B * b); del pend[k]
    meds = [st.median(v) for v in per.values() if len(v) > 100]
    if len(meds) < 3:
        return float('nan'), float('nan'), float('nan')
    return st.pstdev(meds), min(meds), max(meds)


def consumer(d):
    p = os.path.join(d, 'logs', 'closed_loop_trace.csv'); by = defaultdict(list); sub = 0; n = 0
    if not os.path.exists(p):
        return '', 0, 0
    for r in csv.DictReader(open(p)):
        try:
            t = float(r['tow']); e = float(r['pos_err_3d_m'])
        except (ValueError, KeyError):
            continue
        h = int(round((t - T0) * 1000)) // 3600000
        by[h].append(e); sub += e < 0.1; n += 1
    return ' '.join(f"{st.median(by[h]):.3f}" for h in sorted(by)), sub, n


for d in [a for a in sys.argv[1:] if not a.startswith('--')]:
    c, tows = clk(d)
    if not tows:
        print(d, 'no wire'); continue
    out = []
    for i in EPOCHS:
        if i >= len(tows):
            break
        t = tows[i]; v = [c[s][t] for s in c if t in c[s]]
        if len(v) < 3:
            continue
        m = st.median(v); dm = [x - m for x in v]
        out.append(f"{i + 1}:{(sum(x * x for x in v) / len(v)) ** 0.5:.2f}/{(sum(x * x for x in dm) / len(dm)) ** 0.5:.2f}")
    last = (tows[-1] - T0)
    sd, lo, hi = ifres(d, max(0.0, last - 3600.0), last + 1.0)
    cp, sub, n = consumer(d)
    print(f"{os.path.basename(d):22s} clock err rms raw/demed @epoch {' '.join(out)}")
    print(f"{'':22s} IF code resid medians (last hour): sd {sd:.2f} range {lo:+.2f}..{hi:+.2f} | consumer {cp} | below 0.1 m {sub} of {n}")


def datum_share():
    """Per-satellite IF(C1C, C2W) of the product OSB the sim injects as truth: the share of the
    wire clock offset that is the network's datum (IF pinned at zero) and not error."""
    import glob
    tb = glob.glob(ROOT + '/runs/simnet60_quiet_wu93_24h/S01000SIM/truth_bias.csv')
    d = defaultdict(dict)
    if not tb:
        return {}
    for r in csv.DictReader(open(tb[0])):
        d[r['satellite']][r['signal']] = float(r['osb_m'])
    return {s: A * v['C1C'] - B * v['C2W'] for s, v in d.items() if 'C1C' in v and 'C2W' in v}


if __name__ == '__main__' and '--datumfree' in sys.argv:
    ds = datum_share()
    for d in [a for a in sys.argv[1:] if not a.startswith('--')]:
        c, tows = clk(d)
        out = []
        for i in EPOCHS:
            if i >= len(tows):
                break
            t = tows[i]; v = [c[s][t] + ds[s] for s in c if t in c[s] and s in ds]
            if len(v) < 3:
                continue
            m = st.median(v); dm = [x - m for x in v]
            out.append(f"{i + 1}:{(sum(x * x for x in dm) / len(dm)) ** 0.5:.2f}")
        print(f"{os.path.basename(d):22s} DATUM-FREE clock error rms (median removed) @epoch {' '.join(out)}")

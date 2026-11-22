"""Grade a resumed network run (L) against the uninterrupted run (U) over L's epochs.

Usage:
  python wr_grade.py --u <U run dir> --l <L run dir> [--out <prefix>] [--bin-min 10]

Reads products.txt, logs/network_ring_fix.csv and logs/closed_loop_trace.csv of both runs.
All statistics are over the epochs L emits (U is cut to them).

1. Products, datum-invariant: per epoch, per constellation and record kind (clock C, code bias B per
   signal, phase bias P per signal), d = L - U over the satellites in both runs; the residual
   r = d - median(d) removes any common shift (the clock and bias datums), so r is a between-satellite
   difference against the median satellite (no single reference satellite).  Reported per bin:
   median |r|, 95th percentile |r|, max |r|; for phase biases also the residual wrapped to the
   signal's wavelength (an integer-cycle offset is an integer-datum difference, invisible to a user
   that fixes its own integers).  Keys present in one run only are counted.
2. Held links (ring fix trace, state 'held'): per epoch the held set of each run, keyed by
   (station, satellite, sig1, sig2), with the integers (n1, n2).  Counted: held in both with the same
   integers, both with other integers (split into the whole-turn class, an equal shift on both bands,
   and other), held in U only, held in L only.  Truth audit (simulation): 'right' and 'WRONG' rows of
   each run.
3. Built-in user: pos_err_3d_m of the closed-loop trace, median per bin per run, and the number of
   fixed ambiguities.
"""
import argparse, csv, json, math, os, statistics as st
from collections import defaultdict

LAMBDA = {  # metres
    'L1C': 0.19029367279836487, 'L1X': 0.19029367279836487, 'L1W': 0.19029367279836487,
    'L2W': 0.24421021342456825, 'L2L': 0.24421021342456825, 'L2X': 0.24421021342456825,
    'L5Q': 0.25480240374522910, 'L5X': 0.25480240374522910, 'L5I': 0.25480240374522910,
}


def read_products(path, t_min=None):
    """Return {tow: {('C', const, '')|('B', const, sig)|('P', const, sig): {sat: value}}}."""
    out = {}
    cur = None
    with open(path, encoding='utf-8') as fh:
        for line in fh:
            if not line or line[0] == '#':
                continue
            f = line.rstrip('\n').split(',')
            k = f[0]
            if k == 'E':
                tow = round(float(f[2]), 3)
                if t_min is not None and tow < t_min - 1e-6:
                    cur = None
                    continue
                cur = out.setdefault(tow, defaultdict(dict))
            elif cur is None:
                continue
            elif k == 'C':
                cur[('C', f[1][0], '')][f[1]] = float(f[2])
            elif k == 'B':
                cur[('B', f[1][0], f[2])][f[1]] = float(f[3])
            elif k == 'P':
                cur[('P', f[1][0], f[2])][f[1]] = float(f[3])
    return out


def read_held(path, t_min):
    """Return {tow: {(sta, sat, s1, s2): (n1, n2, audit)}} for state 'held'."""
    out = defaultdict(dict)
    with open(path, encoding='utf-8') as fh:
        r = csv.DictReader(fh)
        for row in r:
            if row['state'] != 'held':
                continue
            tow = round(float(row['tow']), 3)
            if tow < t_min - 1e-6:
                continue
            try:
                n1, n2 = int(row['n1']), int(row['n2'])
            except ValueError:
                continue
            out[tow][(row['station'], row['sat'], row['sig1'], row['sig2'])] = (n1, n2, row.get('audit', ''))
    return out


def read_user(path, t_min):
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding='utf-8') as fh:
        r = csv.DictReader(fh)
        for row in r:
            tow = round(float(row['tow']), 3)
            if tow < t_min - 1e-6:
                continue
            try:
                out[tow] = (float(row['pos_err_3d_m']), int(row['num_fixed']))
            except (ValueError, KeyError):
                pass
    return out


def wrap(x, lam):
    return x - lam * round(x / lam)


def pct(v, q):
    if not v:
        return float('nan')
    v = sorted(v)
    i = min(len(v) - 1, max(0, int(math.ceil(q * len(v))) - 1))
    return v[i]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--u', required=True)
    ap.add_argument('--l', required=True)
    ap.add_argument('--out', default=None)
    ap.add_argument('--bin-min', type=float, default=10.0)
    a = ap.parse_args()

    PL = read_products(os.path.join(a.l, 'products.txt'))
    tows = sorted(PL)
    t0 = tows[0]
    PU = read_products(os.path.join(a.u, 'products.txt'), t_min=t0)
    common = [t for t in tows if t in PU]
    binw = a.bin_min * 60.0

    def bin_of(t):
        return int((t - t0) // binw)

    # 1. products
    prod = defaultdict(lambda: defaultdict(list))   # (bin, kind) -> stat -> list of per-epoch values
    first = {}
    only = defaultdict(int)
    for t in common:
        el, eu = PL[t], PU[t]
        per_kind = defaultdict(list)
        per_kind_w = defaultdict(list)
        for key in set(el) | set(eu):
            ml, mu = el.get(key, {}), eu.get(key, {})
            sats = [s for s in ml if s in mu]
            only[key[0]] += len(set(ml) ^ set(mu))
            if len(sats) < 2:
                continue
            d = [ml[s] - mu[s] for s in sats]
            med = st.median(d)
            res = [x - med for x in d]
            kind = key[0]
            per_kind[kind] += [abs(x) for x in res]
            if kind == 'P':
                lam = LAMBDA.get(key[2])
                if lam:
                    per_kind_w[kind] += [abs(wrap(x, lam)) for x in res]
        for kind, v in per_kind.items():
            b = bin_of(t)
            prod[(b, kind)]['med'].append(st.median(v))
            prod[(b, kind)]['p95'].append(pct(v, 0.95))
            prod[(b, kind)]['max'].append(max(v))
            if kind in per_kind_w and per_kind_w[kind]:
                vw = per_kind_w[kind]
                prod[(b, kind)]['wmed'].append(st.median(vw))
                prod[(b, kind)]['wmax'].append(max(vw))
            if t == common[0]:
                first[kind] = {'med': st.median(v), 'p95': pct(v, 0.95), 'max': max(v)}

    # 2. held links
    HL = read_held(os.path.join(a.l, 'logs', 'network_ring_fix.csv'), t0)
    HU = read_held(os.path.join(a.u, 'logs', 'network_ring_fix.csv'), t0)
    held = defaultdict(lambda: defaultdict(int))
    held_first = None
    diff_links = defaultdict(int)
    for t in common:
        hl, hu = HL.get(t, {}), HU.get(t, {})
        c = defaultdict(int)
        c['epochs'] = 1
        c['U'] = len(hu)
        c['L'] = len(hl)
        for k, (n1, n2, au) in hu.items():
            if au == 'right':
                c['U_right'] += 1
            elif au.lower() == 'wrong':
                c['U_wrong'] += 1
            if k in hl:
                m1, m2, _ = hl[k]
                if (m1, m2) == (n1, n2):
                    c['same'] += 1
                elif (m1 - n1) == (m2 - n2):
                    c['turn'] += 1
                    diff_links[k] += 1
                else:
                    c['other'] += 1
                    diff_links[k] += 1
            else:
                c['U_only'] += 1
        # Whole-turn differences that shift every common link of a satellite alike are a difference of that
        # satellite's integer frame (its phase biases differ by whole cycles on both bands), not of single links.
        by_sat = defaultdict(set)
        any_same = defaultdict(bool)
        for k, (n1, n2, _au) in hu.items():
            if k in hl:
                m1, m2, _ = hl[k]
                if (m1, m2) == (n1, n2):
                    any_same[k[1]] = True
                else:
                    by_sat[k[1]].add((m1 - n1, m2 - n2))
        for sat, shifts in by_sat.items():
            if not any_same[sat] and len(shifts) == 1 and next(iter(shifts))[0] == next(iter(shifts))[1]:
                c['satframe'] += sum(1 for k in hu if k[1] == sat and k in hl)
        for k, (m1, m2, al) in hl.items():
            if al == 'right':
                c['L_right'] += 1
            elif al.lower() == 'wrong':
                c['L_wrong'] += 1
            if k not in hu:
                c['L_only'] += 1
        b = bin_of(t)
        for k, v in c.items():
            held[b][k] += v
        if held_first is None:
            held_first = dict(c)

    # 3. user
    UL = read_user(os.path.join(a.l, 'logs', 'closed_loop_trace.csv'), t0)
    UU = read_user(os.path.join(a.u, 'logs', 'closed_loop_trace.csv'), t0)
    user = defaultdict(lambda: {'L': [], 'U': [], 'fL': [], 'fU': []})
    for t in common:
        b = bin_of(t)
        if t in UL:
            user[b]['L'].append(UL[t][0]); user[b]['fL'].append(UL[t][1])
        if t in UU:
            user[b]['U'].append(UU[t][0]); user[b]['fU'].append(UU[t][1])

    nb = bin_of(common[-1]) + 1
    lines = []
    lines.append(f'U = {a.u}')
    lines.append(f'L = {a.l}')
    lines.append(f'leg epochs {len(tows)} (TOW {t0} to {tows[-1]}), common with U {len(common)}; bins of {a.bin_min:g} min')
    lines.append('keys present in one run only (summed over epochs): ' + ', '.join(f'{k} {v}' for k, v in sorted(only.items())))
    lines.append('first epoch, products |r| (mm): ' + '; '.join(
        f"{k} med {v['med']*1e3:.2f} p95 {v['p95']*1e3:.2f} max {v['max']*1e3:.1f}" for k, v in sorted(first.items())))
    if held_first:
        lines.append('first epoch, held links: ' + ', '.join(f'{k} {v}' for k, v in sorted(held_first.items())))
    lines.append('')
    lines.append('bin  | clock |r| mm med/p95/max     | code bias |r| mm med/p95/max  | phase bias |r| mm med/p95/max (wrapped med/max)')
    for b in range(nb):
        row = [f'{b:3d} ']
        for kind in ('C', 'B', 'P'):
            s = prod.get((b, kind))
            if not s:
                row.append(' ' * 30); continue
            txt = f"{st.median(s['med'])*1e3:7.2f} {st.median(s['p95'])*1e3:8.2f} {max(s['max'])*1e3:8.1f}"
            if kind == 'P' and s.get('wmed'):
                txt += f"  ({st.median(s['wmed'])*1e3:.2f} / {max(s['wmax'])*1e3:.1f})"
            row.append(txt)
        lines.append(' | '.join(row))
    lines.append('')
    lines.append('bin  | held link-epochs U, L | same  turn  other  U-only  L-only | audit U right/WRONG | L right/WRONG | turn of which satellite-frame')
    for b in range(nb):
        h = held.get(b, {})
        lines.append(f"{b:3d}  | {h.get('U',0):7d} {h.get('L',0):7d} | {h.get('same',0):7d} {h.get('turn',0):5d} {h.get('other',0):5d} {h.get('U_only',0):7d} {h.get('L_only',0):7d} | {h.get('U_right',0):7d}/{h.get('U_wrong',0):<5d} | {h.get('L_right',0):7d}/{h.get('L_wrong',0):<6d} | {h.get('satframe',0)}")
    lines.append('')
    lines.append('bin  | user 3D median m  U       L       L-U    | fixed median U  L')
    for b in range(nb):
        u = user.get(b)
        if not u or not u['U'] or not u['L']:
            lines.append(f'{b:3d}  | (no user rows)'); continue
        mu, ml = st.median(u['U']), st.median(u['L'])
        lines.append(f"{b:3d}  |           {mu:7.4f} {ml:7.4f} {ml-mu:+8.4f} | {st.median(u['fU']):5.0f} {st.median(u['fL']):5.0f}")
    lines.append('')
    worst = sorted(diff_links.items(), key=lambda kv: -kv[1])[:15]
    lines.append('links held by both with other integers (link-epochs): ' + '; '.join(f'{k[0]} {k[1]} {k[2]}/{k[3]} {v}' for k, v in worst))
    txt = '\n'.join(lines)
    print(txt)
    if a.out:
        with open(a.out + '.txt', 'w', encoding='utf-8') as fh:
            fh.write(txt + '\n')
        js = {
            't0': t0, 'n_leg': len(tows), 'n_common': len(common), 'only': dict(only), 'first': first,
            'held_first': held_first,
            'products': {f'{b}:{k}': {s: (st.median(v) if s in ('med', 'p95', 'wmed') else max(v)) for s, v in d.items()}
                         for (b, k), d in prod.items()},
            'held': {b: dict(v) for b, v in held.items()},
            'user': {b: {'U': st.median(v['U']) if v['U'] else None, 'L': st.median(v['L']) if v['L'] else None,
                         'fU': st.median(v['fU']) if v['fU'] else None, 'fL': st.median(v['fL']) if v['fL'] else None}
                     for b, v in user.items()},
        }
        with open(a.out + '.json', 'w', encoding='utf-8') as fh:
            json.dump(js, fh, indent=1)


if __name__ == '__main__':
    main()

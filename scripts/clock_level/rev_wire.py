"""Card REV: the revision member of the hold family, replayed from a block arm's clone trace.

wire : python scripts/clock_level/rev_wire.py wire <lah_run> <blk_run> <out_dir> <rule> [satwise]
       Rebuilds P = P_LAH - (frozen + X) on every held key over the phase-clock hold's wire of
       the same solve, X from the block clone's trace columns (card REV sections 1 to 3):
         phi      X = Phi_D (b_phid_m), card SMD's member; checked against the block run's own
                  P records (the attribution check)
         rev      X = R (b_revd_m), the revision D(t0|t) - D(t0|t0), unfrozen
         revk<k>  X = R held from the clone's freeze: the first row after birth with
                  V(prev) - V(row) < k (W(row) - W(prev)), V = b_vd0_m2 (b_vd0b_m2 at birth),
                  W = b_wd_m2 (0 at birth); k = 1 is the member, 10 and 100 the ladder
         both     X = S_D (b_sd_m) = Phi_D + R
         phidc    X = Phi_D with each epoch's constellation-common step of its increments removed
                  (card SMC; use with satwise), and the wire read of the between-satellite spread
       frozen is the sum of the final X of the key's earlier block clones in the same hold
       generation (the bridge keeps what a clone reached, card SMH section 12); an epoch with no
       live block carries that sum, as in smd_ctlwire.py.
reads: python scripts/clock_level/rev_wire.py reads <blk_run> [<blk_run> ...]
       The trace reads: the identity Phi_D + R = S_D; |R|, |Phi_D|, |S_D| by clone age (one row
       per satellite and epoch, as smd_split.py) with the bars of R1 and R2; the freeze at k = 1,
       10 and 100 (cause, age, V and |R| at the freeze, the share of rows frozen by hour of the
       run, the first freeze of the ignition clones, the re-coupling edges) for E; row gaps
       within a clone.
"""
import bisect, collections, csv, json, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
TOW_DAY0 = 259200.0     # 2024 DOY 129 00:00 GPS, both beds
EPOCH_S = 30.0


def rundir(r):
    return r if os.path.isdir(r) else ROOT + '/runs/' + r


def run_start(d):
    """The run's first epoch (TOW): the day start plus the arm's start offset."""
    try:
        off = json.load(open(d + '/arm.json')).get('network', {}).get('start_offset_hours') or 0.0
    except (OSError, ValueError):
        off = 0.0
    return TOW_DAY0 + 3600.0 * float(off)


def q(v, f):
    """The quantile of smd_split.py (index int(f n) of the sorted values)."""
    v = sorted(v)
    return v[min(len(v) - 1, int(f * len(v)))] if v else float('nan')


def load(d):
    """Per key (sat, sig): its block rows in time order, each a dict with the floats needed."""
    rows = collections.defaultdict(list)
    order = []   # (key, row) in file order, for the one-row-per-satellite reads
    for r in csv.DictReader(open(d + '/logs/network_pb_hold_clone.csv')):
        if not r.get('b_phid_m'):
            continue
        if not r.get('b_revd_m'):
            sys.exit('the trace has no card REV columns (b_revd_m): run the arm on the instrument build')
        tow = round(float(r['tow']), 1)
        age = float(r['b_age_s'])
        x = dict(
            tow=tow, gen=int(r['gen']), t0=round(tow - age, 1), age=age,
            phi=float(r['b_phid_m']), rev=float(r['b_revd_m']), sd=float(r['b_sd_m']),
            v=float(r['b_vd0_m2']), vb=float(r['b_vd0b_m2']), pd=float(r['b_pd_m2']),
            w=float(r['b_wd_m2']), dw=float(r['b_dwd_m2']))
        rows[(r['sat'], r['sig'])].append(x)
        order.append(((r['sat'], r['sig']), x))
    for lst in rows.values():
        lst.sort(key=lambda x: x['tow'])
    load.order = order
    return rows


def clones(lst):
    """Split one key's rows into clones (a change of generation or of birth time)."""
    out, cur = [], []
    for x in lst:
        if cur and (x['gen'] != cur[-1]['gen'] or x['t0'] != cur[-1]['t0']):
            out.append(cur)
            cur = []
        cur.append(x)
    if cur:
        out.append(cur)
    return out


def freeze(cl, k):
    """The freeze of one clone at scale k: (row index or None, cause).  The first row after
    birth whose fall of V since the previous row is below k times the walk added over the same
    span; the previous row of the first row is the birth (V at birth, W = 0)."""
    vprev, wprev = cl[0]['vb'], 0.0
    for i, x in enumerate(cl):
        dv, dw = vprev - x['v'], x['w'] - wprev
        if dv < k * dw or (dw <= 0.0 and dv < 0.0):
            return i, ('rise' if dv < 0.0 else 'plateau')
        vprev, wprev = x['v'], x['w']
    return None, None


def xseries(cl, rule, common=None):
    """X on each row of one clone under a rule (@p common: tow -> the constellation-common step of
    the walk share, rule phidc)."""
    if rule == 'phi':
        return [x['phi'] for x in cl]
    if rule == 'phidc':
        out, acc, prev = [], 0.0, 0.0
        for x in cl:
            acc += (x['phi'] - prev) - (common.get(x['tow'], 0.0) if common else 0.0)
            prev = x['phi']
            out.append(acc)
        return out
    if rule == 'rev':
        return [x['rev'] for x in cl]
    if rule == 'both':
        return [x['sd'] for x in cl]
    if rule.startswith('revk'):
        i, _ = freeze(cl, float(rule[4:]))
        if i is None:
            return [x['rev'] for x in cl]
        held = cl[i]['rev']
        return [x['rev'] if j < i else held for j, x in enumerate(cl)]
    sys.exit('unknown rule ' + rule)


def wire(lah, blk, out, rule, satwise=False):
    """With satwise, every key of a satellite takes the value of its L2W key at the same epoch
    (its own generation read from that key's P record), so the two keys an L1W + L2W user
    combines carry one frozen sum (the per-key bridge lets them differ, card REV section 6)."""
    lah, blk = rundir(lah), rundir(blk)
    rows = load(blk)
    common = None
    if rule == 'phidc':
        # Card SMC: each epoch's constellation-common step of the walk share, the median over the
        # satellites (their L2W key's clone) of the increment of Phi_D (four or more).
        import statistics
        steps = collections.defaultdict(list)
        for (sat, sig), lst in rows.items():
            if sig != 'L2W':
                continue
            for cl in clones(lst):
                prev = 0.0
                for x in cl:
                    steps[x['tow']].append(x['phi'] - prev)
                    prev = x['phi']
        common = {t: statistics.median(v) for t, v in steps.items() if len(v) >= 4}
    # Per key, the value to subtract at each row's tow and the frozen sum carried after it.
    events, towlists = {}, {}
    for key, lst in rows.items():
        seq, frozen, gen = [], 0.0, None
        for cl in clones(lst):
            if cl[0]['gen'] != gen:
                frozen, gen = 0.0, cl[0]['gen']
            xs = xseries(cl, rule, common)
            for x, v in zip(cl, xs):
                seq.append((x['tow'], x['gen'], frozen + v))
            frozen += xs[-1]
        events[key] = seq
        towlists[key] = [e[0] for e in seq]

    def subtract(key, tow, gen):
        seq = events.get(key)
        if not seq:
            return 0.0
        i = bisect.bisect_right(towlists[key], tow) - 1
        if i < 0:
            return 0.0
        t, g, val = seq[i]
        if t == tow:
            return val
        return val if g == gen else 0.0

    def pgen(f):
        for x in reversed(f):
            if x.startswith('gen='):
                return int(x[4:])
        return None

    l2gen = {}   # (tow, sat) -> the generation of the satellite's L2W P record
    if satwise:
        tw = None
        for ln in open(lah + '/products.txt', encoding='utf-8'):
            if ln[0] == 'E':
                tw = round(float(ln.split(',')[2]), 1)
            elif ln[0] == 'P' and tw is not None:
                f = ln.rstrip('\n').split(',')
                if f[2] == 'L2W':
                    l2gen[(tw, f[1])] = pgen(f)

    os.makedirs(out + '/logs', exist_ok=True)
    n, tow, moved = 0, None, []
    with open(out + '/products.txt', 'w', encoding='utf-8', newline='\n') as fo:
        for ln in open(lah + '/products.txt', encoding='utf-8'):
            if ln[0] == 'E':
                tow = round(float(ln.split(',')[2]), 1)
            if ln[0] == 'P' and tow is not None:
                f = ln.rstrip('\n').split(',')
                if satwise and (tow, f[1]) in l2gen:
                    d = subtract((f[1], 'L2W'), tow, l2gen[(tow, f[1])])
                else:
                    d = subtract((f[1], f[2]), tow, pgen(f))
                if d != 0.0:
                    f[3] = repr(float(f[3]) - d)
                    n += 1
                    moved.append(abs(d))
                    ln = ','.join(f) + '\n'
            fo.write(ln)
    print(f'{rule}: {n} P records moved, {len(events)} keys; |P - P_LAH| p50 / p90 / max '
          f'{1000*q(moved,.5):.1f} / {1000*q(moved,.9):.1f} / {1000*max(moved, default=0):.1f} mm')
    if rule == 'phidc':
        # The wire read of card SMC: per epoch in run hours 3 to 6, the standard deviation over the
        # satellites (L2W keys) of the subtracted amount, this rule against card SMD's (phi).
        import statistics
        t_start = run_start(blk)
        def spreads(r):
            by_tow = collections.defaultdict(list)
            for (sat, sig), lst in rows.items():
                if sig != 'L2W':
                    continue
                fz, gen = 0.0, None
                for cl in clones(lst):
                    if cl[0]['gen'] != gen:
                        fz, gen = 0.0, cl[0]['gen']
                    xs = xseries(cl, r, common)
                    for x, v in zip(cl, xs):
                        if 2.0 <= (x['tow'] - t_start) / 3600.0 < 6.0:
                            by_tow[x['tow']].append(fz + v)
                    fz += xs[-1]
            return sorted(statistics.pstdev(v) for v in by_tow.values() if len(v) >= 4)
        a, b = spreads('phi'), spreads('phidc')
        print(f'  between-satellite sd of the subtracted amount, run hours 3 to 6, median over epochs: '
              f'phi {1000*a[len(a)//2]:.1f} mm, phidc {1000*b[len(b)//2]:.1f} mm (ratio {b[len(b)//2]/a[len(a)//2]:.2f})')
    if rule == 'phi':
        def pvals(path):
            o, tw = {}, None
            for ln in open(path, encoding='utf-8'):
                if ln[0] == 'E':
                    tw = round(float(ln.split(',')[2]), 1)
                elif ln[0] == 'P' and tw is not None:
                    f = ln.rstrip('\n').split(',')
                    o[(tw, f[1], f[2])] = float(f[3])
            return o
        a, b = pvals(out + '/products.txt'), pvals(blk + '/products.txt')
        common = [k for k in a if k in b]
        dmax = max((abs(a[k] - b[k]) for k in common), default=0.0)
        print(f'rebuilt vs the block wire: max |dP| {dmax:.3e} m over {len(common)} P records '
              f'({len(a)} rebuilt, {len(b)} in the block run)')


def reads(blk):
    d = rundir(blk)
    rows = load(d)
    t_start = run_start(d)
    print(os.path.basename(d), f'(run start TOW {t_start:.0f})')
    # The identity, every row.
    ident = max((abs(x['phi'] + x['rev'] - x['sd']) for lst in rows.values() for x in lst), default=0.0)
    print(f'  identity: max |Phi_D + R - S_D| = {ident:.2e} m over {sum(map(len, rows.values()))} rows')
    # One row per satellite and epoch (the first key in file order, as smd_split.py).
    seen, by_age = set(), collections.defaultdict(lambda: collections.defaultdict(list))
    for key, x in load.order:
        tow = x['tow']
        if (tow, key[0]) in seen:
            continue
        seen.add((tow, key[0]))
        h = int(x['age'] // 3600)
        tab = by_age[h]
        tab['phi'].append(abs(x['phi'])); tab['rev'].append(abs(x['rev'])); tab['sd'].append(abs(x['sd']))
        tab['v'].append(x['v']); tab['pd'].append(x['pd'])
    print('  by clone age (h): rows | |R| p50/p90 | |Phi_D| p50/p90 | |S_D| p50/p90 (mm) | '
          'V^.5 p50 | P_D^.5 p50 (mm) | p90 ratio |R|/|Phi_D|')
    for h in sorted(by_age):
        t = by_age[h]
        rr = q(t['rev'], .9) / q(t['phi'], .9) if q(t['phi'], .9) > 0 else float('inf')
        print(f'    {h}: {len(t["phi"]):6d} | {1000*q(t["rev"],.5):6.1f} / {1000*q(t["rev"],.9):6.1f} | '
              f'{1000*q(t["phi"],.5):6.1f} / {1000*q(t["phi"],.9):6.1f} | '
              f'{1000*q(t["sd"],.5):6.1f} / {1000*q(t["sd"],.9):6.1f} | '
              f'{1000*q(t["v"],.5)**.5:6.1f} | {1000*q(t["pd"],.5)**.5:6.1f} | {rr:5.2f}')
    h0 = by_age.get(0)
    if h0:
        ok = q(h0['rev'], .9) >= 3 * q(h0['phi'], .9)
        print(f'  R1 trace bar (age 0-1 h, |R| p90 >= 3 |Phi_D| p90): {"HOLDS" if ok else "FAILS"}')
    late = [h for h in sorted(by_age) if h >= 1 and len(by_age[h]['phi']) >= 500]
    ok2 = all(q(by_age[h]['rev'], .9) > q(by_age[h]['phi'], .9) for h in late)
    print(f'  R2 trace bar (ages {late} h, |R| p90 > |Phi_D| p90): {"HOLDS" if ok2 else "FAILS"}')
    # Clones (per key; the two keys of a satellite born on one stash are one clone).
    cls, cseen = [], set()
    for key in sorted(rows):
        for cl in clones(rows[key]):
            ck = (key[0], cl[0]['t0'])
            if ck in cseen:
                continue
            cseen.add(ck)
            cls.append((key, cl))
    gaps = sum(1 for _, cl in cls for a, b in zip(cl, cl[1:]) if b['tow'] - a['tow'] > EPOCH_S + 0.5)
    steps = sum(max(0, len(cl) - 1) for _, cl in cls)
    dwchk = max((abs((b['w'] - a['w']) - b['dw']) for _, cl in cls for a, b in zip(cl, cl[1:])
                 if b['tow'] - a['tow'] <= EPOCH_S + 0.5), default=0.0)
    print(f'  clones: {len(cls)}; row gaps within a clone {gaps} of {steps} steps; '
          f'max |W(row) - W(prev) - dW(row)| on consecutive rows {dwchk:.1e} m2')
    nh = 1 + int((max(x['tow'] for lst in rows.values() for x in lst) - t_start) // 3600)
    for k in (1.0, 10.0, 100.0):
        causes = collections.Counter()
        fz_age, fz_v, fz_r, first_ign = [], [], [], []
        frozen_rows = collections.Counter()
        all_rows = collections.Counter()
        recouple, recouple_r = 0, []
        last_frozen = {}   # (sat, gen) -> True when the key's previous clone froze
        for key, cl in cls:
            i, cause = freeze(cl, k)
            born_h = (cl[0]['t0'] - t_start) / 3600.0
            gk = (key, cl[0]['gen'])
            if last_frozen.get(gk):
                recouple += 1
                upto = cl if i is None else cl[:i + 1]
                recouple_r.append(max(abs(x['rev']) for x in upto))
            last_frozen[gk] = i is not None
            if i is not None:
                causes[cause] += 1
                fz_age.append(cl[i]['age'] / 3600.0)
                fz_v.append(cl[i]['v'])
                fz_r.append(abs(cl[i]['rev']))
            if born_h < 1.0:
                first_ign.append((cl[i]['tow'] - t_start) / 3600.0 if i is not None else float('inf'))
            for j, x in enumerate(cl):
                h = int((x['tow'] - t_start) // 3600)
                all_rows[h] += 1
                if i is not None and j >= i:
                    frozen_rows[h] += 1
        share = ' '.join(f'{frozen_rows[h] / all_rows[h]:.2f}' if all_rows[h] else '-' for h in range(nh))
        h35 = sum(frozen_rows[h] for h in (2, 3, 4)) / max(1, sum(all_rows[h] for h in (2, 3, 4)))
        ign_med = q(first_ign, .5) if first_ign else float('nan')
        print(f'  freeze k={k:g}: {sum(causes.values())} of {len(cls)} clones '
              f'({causes["plateau"]} plateau, {causes["rise"]} rise); age at freeze p10/p50/p90 '
              f'{q(fz_age,.1):.2f}/{q(fz_age,.5):.2f}/{q(fz_age,.9):.2f} h; V^.5 at freeze p50 '
              f'{1000*q(fz_v,.5)**.5:.1f} mm; |R| at freeze p50/p90 {1000*q(fz_r,.5):.1f}/{1000*q(fz_r,.9):.1f} mm')
        print(f'    share of rows frozen by run hour (1..{nh}): {share}; hours 3-5: {h35:.2f}')
        print(f'    ignition clones (born in run hour 1): {len(first_ign)}, median run time of the '
              f'freeze {ign_med:.2f} h ({sum(1 for t in first_ign if t == float("inf"))} never)')
        print(f'    re-coupling edges (a birth after a frozen clone of the key): {recouple}, '
              f'|R| reached before its own freeze p50/p90 {1000*q(recouple_r,.5):.1f}/{1000*q(recouple_r,.9):.1f} mm')


if __name__ == '__main__':
    if len(sys.argv) >= 6 and sys.argv[1] == 'wire':
        wire(rundir(sys.argv[2]), rundir(sys.argv[3]), sys.argv[4], sys.argv[5],
             satwise=len(sys.argv) > 6 and sys.argv[6] == 'satwise')
    elif len(sys.argv) >= 3 and sys.argv[1] == 'reads':
        for b in sys.argv[2:]:
            reads(b)
    else:
        sys.exit(__doc__)

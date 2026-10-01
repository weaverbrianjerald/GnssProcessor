"""Card FXA: the field's excess phase-visible motion, read with the martingale property of the
smoothed generation-start value.

    python scripts/clock_level/fxa_attrib.py <blk_run> [<blk_run> ...]

From a block arm's clone trace (card REV's columns) and its network_events.csv.  One row per
satellite and epoch: each satellite's L2W key (every GPS satellite has it on both beds).  Per
block clone, the revision R(t) = D(t0|t) - D(t0|t0) and the stated variance V(t) of D(t0): under the
model the increments dR are uncorrelated with variance V(t-1) - V(t), so z = dR / sqrt(V(t-1) - V(t))
is white with unit variance.  Increments are taken between rows 30 s apart only (the birth counts
as the row before a first row at age 30 s, with R = 0 and V at birth).

Per population (clones born in the first run hour, "ignition", and after it, "later"):
  increment NIS      mean z^2
  lag-k correlation  pooled within clones, k = 1, 5, 20, 60
  NEES at 1 h        R^2 / (V0 - V) at clone age 1 h (3540 to 3660 s), mean over clones, against
                     the mean z^2 of the same clones up to that age; the persistence
                     R(1 h)^2 / sum dR^2 (1 for white increments)
  event share        the share of sum z^2 within 2 epochs of a declared event on the satellite
                     (any station; card FXA section 1's families) against the share of rows
Then, descriptive (section 4): the frame-free motion of D by lag against the stated walk, by clone
population and age, and the converged rows' motion with each epoch's constellation-common step
removed.
"""
import collections, csv, importlib.util, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('rev_wire', os.path.join(HERE, 'rev_wire.py'))
rw = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rw)

FAMILIES = {
    'slip_resume': {'lli_slip', 'if_slip', 'mw_wl_slip', 'reentry_relevel', 'arc_resume',
                    'sig_resume', 'attrib_arc'},
    'code_channel': {'companion_open', 'companion_close'},
    'datum_anchor': {'anchor_handover', 'handover_continuous', 'handover_cold',
                     'anchor_slip_handover', 'sts_applied', 'sts_cold_declared', 'refsat_reset'},
    'clock_seed': {'cta_decouple', 'cta_recouple', 'cta_disc_landed', 'seed_clock',
                   'seed_code_bias', 'seed_held', 'readmit', 'reseed_disc_landed'},
    'columns': {'col_added', 'col_removed'},
    'emission': {'retro_bump', 'gross_los_emit_disc', 'gen_start'},
}
EP = 30.0
WIN = 2   # epochs either side


def events(d):
    """(sat, family) -> sorted event tows."""
    out = collections.defaultdict(list)
    fam_of = {e: f for f, es in FAMILIES.items() for e in es}
    for r in csv.DictReader(open(d + '/logs/network_events.csv')):
        f = fam_of.get(r.get('event', ''))
        if f and r.get('sat'):
            out[(r['sat'], f)].append(round(float(r['tow']), 1))
    for v in out.values():
        v.sort()
    return out


def near(tows, t):
    """True when a tow of the sorted list lies within WIN epochs of t."""
    import bisect
    i = bisect.bisect_left(tows, t - WIN * EP - 0.5)
    return i < len(tows) and tows[i] <= t + WIN * EP + 0.5


def corr(pairs):
    sxy = sum(a * b for a, b in pairs)
    sxx = sum(a * a for a, _ in pairs)
    syy = sum(b * b for _, b in pairs)
    return sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else float('nan')


def read(blk):
    d = rw.rundir(blk)
    rows = rw.load(d)
    t_start = rw.run_start(d)
    ev = events(d)
    print(os.path.basename(d), f'(run start TOW {t_start:.0f})')
    pops = {'ignition': [], 'later': []}
    for (sat, sig), lst in sorted(rows.items()):
        if sig != 'L2W':
            continue
        for cl in rw.clones(lst):
            born_h = (cl[0]['t0'] - t_start) / 3600.0
            pops['ignition' if born_h < 1.0 else 'later'].append((sat, cl))
    for name, cls in pops.items():
        zs_by_clone, nonpos = [], 0
        z2_all, rows_all = 0.0, 0
        fam_z2 = collections.Counter()
        fam_rows = collections.Counter()
        any_z2, any_rows = 0.0, 0
        nees1h, nis1h, pers1h = [], [], []
        for sat, cl in cls:
            prev = None
            if abs(cl[0]['age'] - EP) < 0.5:
                prev = {'tow': cl[0]['tow'] - EP, 'rev': 0.0, 'v': cl[0]['vb']}
            zs, sdr2, z2_upto, n_upto = [], 0.0, 0.0, 0
            for x in cl:
                if prev is not None and abs(x['tow'] - prev['tow'] - EP) < 0.5:
                    dr = x['rev'] - prev['rev']
                    dv = prev['v'] - x['v']
                    if dv > 0.0:
                        z = dr / math.sqrt(dv)
                        zs.append((x['tow'], z))
                        z2_all += z * z
                        rows_all += 1
                        hit = False
                        for f in FAMILIES:
                            if near(ev.get((sat, f), []), x['tow']):
                                fam_z2[f] += z * z
                                fam_rows[f] += 1
                                hit = True
                        if hit:
                            any_z2 += z * z
                            any_rows += 1
                        if x['age'] <= 3660.0:
                            sdr2 += dr * dr
                            z2_upto += z * z
                            n_upto += 1
                    else:
                        nonpos += 1
                        zs.append((x['tow'], None))
                else:
                    zs.append((x['tow'], None))   # a break in the chain
                if 3540.0 <= x['age'] <= 3660.0 and n_upto > 0 and not pers1h_done(pers1h, sat, cl):
                    den = x['vb'] - x['v']
                    if den > 0.0 and sdr2 > 0.0:
                        nees1h.append(x['rev'] ** 2 / den)
                        nis1h.append(z2_upto / n_upto)
                        pers1h.append((sat, cl[0]['t0'], x['rev'] ** 2 / sdr2))
                prev = x
            zs_by_clone.append(zs)
        print(f'  {name}: {len(cls)} clones, {rows_all} increments ({nonpos} with no variance fall)')
        if rows_all == 0:
            continue
        nis = z2_all / rows_all
        lags = {}
        for k in (1, 5, 20, 60):
            pairs = []
            for zs in zs_by_clone:
                for i in range(k, len(zs)):
                    a, b = zs[i - k][1], zs[i][1]
                    if a is not None and b is not None and abs(zs[i][0] - zs[i - k][0] - k * EP) < 0.5:
                        pairs.append((a, b))
            lags[k] = (corr(pairs), len(pairs))
        print(f'    increment NIS {nis:.3f}; lag correlation of z: '
              + ', '.join(f'{k}: {c:+.3f} ({n})' for k, (c, n) in lags.items()))
        if nees1h:
            mn = sum(nees1h) / len(nees1h)
            mi = sum(nis1h) / len(nis1h)
            pv = sorted(p for _, _, p in pers1h)
            print(f'    at clone age 1 h ({len(nees1h)} clones): revision NEES {mn:.2f}, increment NIS of the '
                  f'same rows {mi:.3f}, ratio {mn / mi if mi > 0 else float("nan"):.2f}; persistence '
                  f'R^2 / sum dR^2 p50 {pv[len(pv) // 2]:.2f}, mean {sum(pv) / len(pv):.2f}')
        share = (any_z2 / z2_all, any_rows / rows_all)
        print(f'    event windows (+-{WIN} ep, any family): {100 * share[0]:.1f} % of sum z^2 on '
              f'{100 * share[1]:.1f} % of rows, enrichment {share[0] / share[1] if share[1] else float("nan"):.2f}')
        for f in FAMILIES:
            if fam_rows[f]:
                a, b = fam_z2[f] / z2_all, fam_rows[f] / rows_all
                print(f'      {f:13s} {100 * a:5.1f} % of sum z^2 on {100 * b:5.1f} % of rows, enrichment {a / b:.2f}')
        pops[name] = (nis, lags, nees1h, nis1h, share)
    return pops


def structure(blk, lags=(1, 5, 20, 60, 120, 240), pop='later', min_age_s=0.0, max_age_s=1e9):
    """The frame-free motion of D by time scale (card FXA's descriptive read for the next card):
    over the L2W key's clones, the mean of (S_D(t + k) - S_D(t))^2 at lag k epochs (S_D the
    cumulative frame-free step of D since birth, so differences within a clone are the
    estimate's own motion), against the stated walk k * dW (dW the clone's mean walk per epoch).
    Clones born after the first run hour only (converged, so the differences are motion rather
    than ignition convergence); the median of the squared differences beside their mean."""
    d = rw.rundir(blk)
    rows = rw.load(d)
    t_start = rw.run_start(d)
    acc = {k: [0.0, 0, 0.0, []] for k in lags}
    for (sat, sig), lst in rows.items():
        if sig != 'L2W':
            continue
        for cl in rw.clones(lst):
            later = (cl[0]['t0'] - t_start) >= 3600.0
            if (pop == 'later' and not later) or (pop == 'ignition' and later):
                continue
            by_tow = {round(x['tow'], 1): x for x in cl}
            for x in cl:
                if not (min_age_s <= x['age'] <= max_age_s):
                    continue
                for k in lags:
                    y = by_tow.get(round(x['tow'] + k * EP, 1))
                    if y is not None:
                        acc[k][0] += (y['sd'] - x['sd']) ** 2
                        acc[k][1] += 1
                        acc[k][2] += y['w'] - x['w']
                        acc[k][3].append((y['sd'] - x['sd']) ** 2)
    print(os.path.basename(d) + f': frame-free motion of D by lag, {pop} clones, start age {min_age_s / 3600:.1f} '
          f'to {min(max_age_s, 1e5) / 3600:.1f} h (rms mm | median-based rms mm | stated walk rms mm | '
          'ratio of variances, mean / median)')
    for k in lags:
        s, n, w, v = acc[k]
        if n:
            v.sort()
            med = v[len(v) // 2] / 0.4549   # the median of a chi-square(1) is 0.4549 of its mean
            print(f'    lag {k:4d} ep ({k * EP / 60:6.1f} min): {1000 * math.sqrt(s / n):7.2f} | '
                  f'{1000 * math.sqrt(med):7.2f} | {1000 * math.sqrt(max(w / n, 0.0)):6.2f} | '
                  f'{s / w if w > 0 else float("nan"):8.1f} / {med * n / w if w > 0 else float("nan"):6.1f}   (n={n})')


def common_split(blk, lags=(5, 20, 60, 120)):
    """The converged rows' motion of D with and without each epoch's constellation-common step
    (card FXA section 4): rows of ignition clones past 1.5 h of age and of later clones past
    30 minutes, the L2W key; per epoch the median of the satellites' frame-free D steps (four or
    more) is the common step; the median-based rms of the motion over a lag, raw and with the
    common step removed."""
    import statistics as st
    d = rw.rundir(blk)
    rows = rw.load(d)
    t_start = rw.run_start(d)
    inc = collections.defaultdict(dict)   # tow -> sat -> frame-free step of D
    for (sat, sig), lst in rows.items():
        if sig != 'L2W':
            continue
        for cl in rw.clones(lst):
            later = (cl[0]['t0'] - t_start) >= 3600.0
            for a, b in zip(cl, cl[1:]):
                if abs(b['tow'] - a['tow'] - EP) > 0.5:
                    continue
                if (later and b['age'] < 1800.0) or ((not later) and b['age'] < 5400.0):
                    continue
                inc[b['tow']][sat] = b['sd'] - a['sd']
    com = {t: (st.median(v.values()) if len(v) >= 4 else None) for t, v in inc.items()}
    raw, res = collections.defaultdict(list), collections.defaultdict(list)
    for t in sorted(inc):
        for s, x in inc[t].items():
            raw[s].append((t, x))
            if com[t] is not None:
                res[s].append((t, x - com[t]))

    def sf(series, k):
        v = []
        for lst in series.values():
            cum, c, prev = {}, 0.0, None
            for t, x in lst:
                if prev is not None and abs(t - prev - EP) > 0.5:
                    c, cum = 0.0, {}
                c += x
                cum[t] = c
                prev = t
                if t - k * EP in cum:
                    v.append((c - cum[t - k * EP]) ** 2)
        v.sort()
        return (1000.0 * math.sqrt(v[len(v) // 2] / 0.4549) if v else float('nan'), len(v))

    print(os.path.basename(d) + ': converged rows, median-based rms (mm) of the motion of D over a lag: '
          'all | the common step of each epoch removed')
    for k in lags:
        a, b = sf(raw, k), sf(res, k)
        print(f'    lag {k:4d} ep ({k * EP / 60:6.1f} min): {a[0]:7.1f} | {b[0]:7.1f}   (n={a[1]} / {b[1]})')


def revision_nees_dc(blk):
    """The block clones' revision NEES R^2 / (V0 - V) by clone age, raw and with each epoch's
    constellation-common revision step (the median over the satellites' L2W clones of the
    increment of R, four or more) removed before accumulating (card SMC section 5, the
    correction to card REV's field instrument)."""
    import statistics as st
    d = rw.rundir(blk)
    rows = rw.load(d)
    t_start = rw.run_start(d)
    cls = [(sat, cl) for (sat, sig), lst in rows.items() if sig == 'L2W' for cl in rw.clones(lst)]
    steps = collections.defaultdict(list)
    for sat, cl in cls:
        prev = 0.0
        for x in cl:
            steps[x['tow']].append(x['rev'] - prev)
            prev = x['rev']
    com = {t: (st.median(v) if len(v) >= 4 else 0.0) for t, v in steps.items()}
    tab = collections.defaultdict(lambda: ([], []))
    for sat, cl in cls:
        later = (cl[0]['t0'] - t_start) >= 3600.0
        prev, rdc = 0.0, 0.0
        for x in cl:
            rdc += (x['rev'] - prev) - com[x['tow']]
            prev = x['rev']
            den = x['vb'] - x['v']
            if den > 0.0:
                h = min(int(x['age'] // 3600), 3)
                tab[(later, h)][0].append(x['rev'] ** 2 / den)
                tab[(later, h)][1].append(rdc ** 2 / den)
    print(os.path.basename(d) + ': revision NEES mean by clone age 0 / 1 / 2 / 3 h, raw -> common step removed')
    for later in (False, True):
        cells = []
        for h in range(4):
            a, b = tab[(later, h)]
            cells.append(f'{sum(a) / len(a):6.2f} -> {sum(b) / len(b):5.2f}' if a else '      -')
        print(f'    {"later" if later else "ignition":8s} ' + '  '.join(cells))


def pers1h_done(pers1h, sat, cl):
    """One age-1-h reading per clone."""
    return any(s == sat and t0 == cl[0]['t0'] for s, t0, _ in pers1h)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    res = {b: read(b) for b in sys.argv[1:]}
    for b in sys.argv[1:]:
        structure(b)                                         # later clones, every age
        structure(b, pop='later', max_age_s=1800.0)          # later clones in their first half hour
        structure(b, pop='later', min_age_s=1800.0)          # later clones past it
        structure(b, pop='ignition', min_age_s=5400.0)       # ignition clones long converged
        common_split(b)                                      # the constellation-common part
        revision_nees_dc(b)                                  # card SMC's correction to card REV

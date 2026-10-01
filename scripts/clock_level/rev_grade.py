"""Card REV: the censuses of the hold family's members, graded against the pre-registered bars.

    python scripts/clock_level/rev_grade.py <ref> <phi> <rev> <revk1> [<key> ...]

Keys are census keys of census6h.py (field: tags msc6_<site>_<key>) and simcensus6h.py (sim: the
same key with an 's' appended, tags smc6_<station>_<key>s).  <ref> is the phase-clock hold, <phi>
card SMD's member on the same rebuild, <rev> the unfrozen revision member, <revk1> the member
frozen at k = 1; further keys are read the same way (no bars).  Per hour (1..6): the median over
sites of the site p50 and the median paired change against <ref> with better / worse at 1 cm,
all 16 field sites, the C1W and C1C families, WTZZ00DEU alone, and the 16 sim stations; then
the bars of R1, R2 and R3 (card REV section 4).
"""
import importlib.util, os, statistics as st, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def _mod(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + '.py'))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


F = _mod('census6h')
S = _mod('simcensus6h')
NH = 6
FIELD = [(s['name'][:4].lower(), s.get('latch_c1', '?')) for s in F.SL]
SIM = [s['name'][:3].lower() for s in S.SL]


def field(key):
    return {n: F.hours(f'msc6_{n}_{key}') for n, _ in FIELD}


def sim(key):
    return {n: S.hours(f'smc6_{n}_{key}s') for n in SIM}


def change(K, R, names):
    """Per hour: (median paired change, better, worse) over the names present in both."""
    ns = [n for n in names if K.get(n) and R.get(n)]
    out = []
    for h in range(NH):
        d = [K[n][h] - R[n][h] for n in ns if K[n][h] == K[n][h] and R[n][h] == R[n][h]]
        out.append((st.median(d) if d else float('nan'), sum(x < -0.01 for x in d),
                    sum(x > 0.01 for x in d), len(d)))
    return out


def level(K, names):
    return [st.median([K[n][h] for n in names if K.get(n) and K[n][h] == K[n][h]] or [float('nan')])
            for h in range(NH)]


def fmt(ch):
    return ' '.join(f'{m:+.3f}({b}/{w})' for m, b, w, _ in ch)


def main(ref, keys):
    allf = [n for n, _ in FIELD]
    c1w = [n for n, lc in FIELD if lc == 'C1W']
    c1c = [n for n, lc in FIELD if lc == 'C1C']
    RF, RS = field(ref), sim(ref)
    RW = F.hours(f'msc6_wtzz_{ref}')
    print(f'field {ref:10s} ' + ' '.join(f'{v:.3f}' for v in level(RF, allf))
          + f'   WTZZ ' + ' '.join(f'{v:.3f}' for v in RW or []))
    print(f'sim   {ref:10s} ' + ' '.join(f'{v:.3f}' for v in level(RS, SIM)))
    res = {}
    for key in keys:
        KF, KS = field(key), sim(key)
        KW = F.hours(f'msc6_wtzz_{key}')
        r = dict(all=change(KF, RF, allf), c1w=change(KF, RF, c1w), c1c=change(KF, RF, c1c),
                 sim=change(KS, RS, SIM))
        res[key] = r
        print(f'\nfield {key:10s} ' + ' '.join(f'{v:.3f}' for v in level(KF, allf))
              + f'   (n={sum(1 for n in allf if KF.get(n))})   WTZZ '
              + ' '.join(f'{v:.3f}' for v in KW or []))
        print(f'   all ({len(allf)}): ' + fmt(r['all']))
        print(f'   C1W ({len(c1w)}): ' + fmt(r['c1w']))
        print(f'   C1C ({len(c1c)}): ' + fmt(r['c1c']))
        print(f'sim   {key:10s} ' + ' '.join(f'{v:.3f}' for v in level(KS, SIM))
              + f'   (n={sum(1 for n in SIM if KS.get(n))})')
        print(f'   all ({len(SIM)}): ' + fmt(r['sim']))
    if len(keys) < 3:
        return
    phi, rev, rk1 = keys[0], keys[1], keys[2]
    P, U, K1 = res[phi], res[rev], res[rk1]
    v = lambda ok: 'HOLDS' if ok else 'FAILS'
    print('\nbars (card REV section 4):')
    b1 = (U['all'][0][0] <= -0.080, U['c1w'][0][0] <= -0.078, U['sim'][0][0] <= -0.005)
    print(f'  R1 census: field all-site h1 {U["all"][0][0]:+.3f} <= -0.080 {v(b1[0])}; '
          f'C1W h1 {U["c1w"][0][0]:+.3f} <= -0.078 {v(b1[1])}; sim h1 {U["sim"][0][0]:+.3f} '
          f'<= -0.005 {v(b1[2])}   (card SMD on this rebuild: all {P["all"][0][0]:+.3f}, '
          f'C1W {P["c1w"][0][0]:+.3f}, sim {P["sim"][0][0]:+.3f})')
    nf = sum(U['c1w'][h][0] > P['c1w'][h][0] for h in (2, 3, 4))
    ns = sum(U['sim'][h][0] > P['sim'][h][0] for h in (2, 3, 4))
    print(f'  R2 census: field C1W hours 3-5 above card SMD in {nf} of 3 ({v(nf >= 2)}); '
          f'sim hours 3-5 above card SMD in {ns} of 3 ({v(ns >= 2)})')
    c1w_ok = all(abs(K1['c1w'][h][0]) <= 0.005 and K1['c1w'][h][2] <= 3 for h in (2, 3, 4))
    sim_ok = all(abs(K1['sim'][h][0]) <= 0.005 for h in (2, 3, 4, 5))
    h1_ok = K1['all'][0][0] <= -0.03
    print(f'  R3 census (k = 1): field C1W hours 3-5 within +-0.005 with <= 3 worse '
          f'{v(c1w_ok)} ({fmt(K1["c1w"][2:5])}); sim hours 3-6 within +-0.005 {v(sim_ok)} '
          f'({fmt(K1["sim"][2:6])}); all-site h1 {K1["all"][0][0]:+.3f} <= -0.03 {v(h1_ok)}')


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2:])

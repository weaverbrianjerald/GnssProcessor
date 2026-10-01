"""Card SMD P1: the split of the block clones' passed motion by the clone's own age.

    python scripts/clock_level/smd_split.py <run> [<run> ...]

From logs/network_pb_hold_clone.csv (the b_* columns, one row per satellite and epoch): per
clone-age hour, the rows, the level part |Phi_c - Phi_D| p50 / p90, the phase-visible part
|Phi_D| p50 / p90 (mm), and the mean of r_D; also the same by hour of the day.
"""
import collections, csv, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
T0 = 259200.0


def q(v, f):
    v = sorted(v)
    return v[min(len(v) - 1, int(f * len(v)))] if v else float('nan')


for run in sys.argv[1:]:
    d = run if os.path.isdir(run) else ROOT + '/runs/' + run
    by_age = collections.defaultdict(lambda: ([], [], []))
    by_hod = collections.defaultdict(lambda: ([], [], []))
    seen = set()
    for r in csv.DictReader(open(d + '/logs/network_pb_hold_clone.csv')):
        if not r.get('b_phid_m'):
            continue
        k = (r['tow'], r['sat'])
        if k in seen:
            continue
        seen.add(k)
        a = float(r['b_age_s']); t = float(r['tow'])
        lvl, pv = abs(float(r['b_lvl_m'])), abs(float(r['b_phid_m']))
        rd = float(r['b_rd']) if r['b_rd'] else None
        for tab, h in ((by_age, int(a // 3600)), (by_hod, int((t - T0) // 3600))):
            tab[h][0].append(lvl); tab[h][1].append(pv)
            if rd is not None:
                tab[h][2].append(rd)
    print(os.path.basename(d))
    for name, tab in (('clone age (h)', by_age), ('hour of day', by_hod)):
        print(f'  by {name}: rows | level p50 / p90 mm | Phi_D p50 / p90 mm | mean r_D')
        for h in sorted(tab):
            lv, pv, rd = tab[h]
            mr = sum(rd) / len(rd) if rd else float('nan')
            print(f'    {h}: {len(lv):6d} | {1000*q(lv,.5):6.1f} / {1000*q(lv,.9):6.1f} | '
                  f'{1000*q(pv,.5):6.1f} / {1000*q(pv,.9):6.1f} | {mr:.2f}')

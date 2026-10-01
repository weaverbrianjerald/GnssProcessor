"""Card QPV: the anchor-q ladder's rule.

    python scripts/clock_level/qpv_ladder.py

Per rung and bed, from logs/network_pb_hold_clone.csv of the smoothed hold's clock clones (one
row per satellite and epoch): mean r by clone-age hour and the mean over ages 1 to 3 h, |Phi|
p50 / p90 by age; the field's excess x(q) = r_f - r_s over ages 1 to 3 h, and the derived q where
x = 1 (linear in log q between the bracketing rungs).
"""
import collections, csv, math, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUNS = ROOT + '/runs'
RUNGS = [(7.25e-9, 'qpv_q025'), (1.45e-8, 'qpv_q05'), (2.9e-8, 'smd2_ex6'), (5.8e-8, 'qpv_q2'),
         (1.16e-7, 'qpv_q4')]


def q(v, f):
    v = sorted(v)
    return v[min(len(v) - 1, int(f * len(v)))] if v else float('nan')


def read(tag):
    p = f'{RUNS}/_rap_{tag}/logs/network_pb_hold_clone.csv'
    if not os.path.exists(p):
        return None
    r_age = collections.defaultdict(list)
    phi_age = collections.defaultdict(list)
    seen = set()
    for row in csv.DictReader(open(p)):
        k = (row['tow'], row['sat'])
        if k in seen:
            continue
        seen.add(k)
        a = int(float(row['age_s']) // 3600)
        if row['phi_m']:
            phi_age[a].append(abs(float(row['phi_m'])))
        if row['r']:
            r_age[a].append(float(row['r']))
    r13 = [x for a in (1, 2) for x in r_age.get(a, [])]
    return {'r_age': {a: sum(v) / len(v) for a, v in r_age.items() if v},
            'n_age': {a: len(v) for a, v in phi_age.items()},
            'phi_age': {a: (q(v, .5), q(v, .9)) for a, v in phi_age.items()},
            'r13': sum(r13) / len(r13) if r13 else float('nan')}


xs = []
for qv, tag in RUNGS:
    f, s = read(tag + '_f'), read(tag + '_s')
    if not f or not s:
        print(f'{qv:.3g}: missing')
        continue
    x = f['r13'] - s['r13']
    xs.append((qv, x))
    print(f'q {qv:.3g}: field r(1-3 h) {f["r13"]:.2f}, sim {s["r13"]:.2f}, excess x {x:+.2f}')
    for name, d in (('field', f), ('sim', s)):
        ages = sorted(d['n_age'])
        print(f'   {name}: ' + '; '.join(
            f'a{a} n{d["n_age"][a]} r {d["r_age"].get(a, float("nan")):.2f} |Phi| '
            f'{1000*d["phi_age"][a][0]:.1f}/{1000*d["phi_age"][a][1]:.1f} mm' for a in ages))
der = None
for (q0, x0), (q1, x1) in zip(xs, xs[1:]):
    if (x0 - 1.0) * (x1 - 1.0) <= 0 and x0 != x1:
        t = (1.0 - x0) / (x1 - x0)
        der = math.exp(math.log(q0) + t * (math.log(q1) - math.log(q0)))
        break
if der:
    print(f'derived q (x = 1): {der:.3g} m2/s, {der / 2.9e-8:.2f} x the floor')
elif xs:
    print('x does not cross 1 on the ladder:', ', '.join(f'{qv:.3g}:{x:+.2f}' for qv, x in xs))

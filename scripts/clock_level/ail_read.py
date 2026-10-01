"""Card AIL: where the float vehicle's carried ambiguity loses its information.

    python scripts/clock_level/ail_read.py <run> [<run> ...]

From logs/network_amb_if.csv (network.trace_amb_if): per line of sight and epoch the variance of the
ionosphere-free ambiguity combination before the solve (the block the inject folded) and after it
(the harvest block), conditional on the border, in full and with the station's common mode projected
out (`_dm`).  Over consecutive epochs of one line of sight:

  loss at the inject   prior(t) - post(t - 1)   the variance the time update adds
  gain in the solve    prior(t) - post(t)       the variance the epoch's data remove

reported as medians by arc age (epochs since the arc's first stash), with the posterior standard
deviation, in mm (a variance v reads as sign(v) sqrt(|v|)).
"""
import collections, csv, math, os, statistics as st, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))).replace(os.sep, '/')
BINS = ((0, 20), (20, 80), (80, 160), (160, 10 ** 9))


def rundir(r):
    return r if os.path.isdir(r) else ROOT + '/runs/' + r


def mm(v):
    return math.copysign(1e3 * math.sqrt(abs(v)), v)


def read(run):
    d = rundir(run)
    rows = collections.defaultdict(dict)
    with open(d + '/logs/network_amb_if.csv') as f:
        for r in csv.DictReader(f):
            k = (r['station'], r['sat'])
            rows[k][round(float(r['tow']), 1)] = r
    out = {b: collections.defaultdict(list) for b in BINS}
    for k, by in rows.items():
        for t, r in by.items():
            age = int(r['age_ep'])
            b = next((bb for bb in BINS if bb[0] <= age < bb[1]), None)
            if b is None:
                continue
            try:
                pr, po = float(r['prior_var_m2']), float(r['post_var_m2'])
                prd, pod = float(r['prior_var_dm_m2']), float(r['post_var_dm_m2'])
            except ValueError:
                continue
            if math.isfinite(po):
                out[b]['post_sd'].append(math.sqrt(max(po, 0.0)))
            if math.isfinite(pod):
                out[b]['post_sd_dm'].append(math.sqrt(max(pod, 0.0)))
            if math.isfinite(pr) and math.isfinite(po):
                out[b]['gain'].append(pr - po)
            if math.isfinite(prd) and math.isfinite(pod):
                out[b]['gain_dm'].append(prd - pod)
            p = by.get(round(t - 30.0, 1))
            if p is None:
                continue
            try:
                ppo, ppod = float(p['post_var_m2']), float(p['post_var_dm_m2'])
            except ValueError:
                continue
            if math.isfinite(pr) and math.isfinite(ppo):
                out[b]['loss'].append(pr - ppo)
            if math.isfinite(prd) and math.isfinite(ppod):
                out[b]['loss_dm'].append(prd - ppod)
    print(f'{os.path.basename(d)}: medians by arc age (mm; loss = prior(t) - post(t-1), gain = prior(t) - post(t))')
    for b in BINS:
        o = out[b]
        if not o['post_sd']:
            continue
        def med(key, sd=False):
            v = o.get(key)
            if not v:
                return '   -  '
            m = st.median(v)
            return f'{1e3 * m:6.2f}' if sd else f'{mm(m):6.2f}'
        hi = '+' if b[1] > 10 ** 8 else f'-{b[1]}'
        print(f'   age {b[0]:3d}{hi:>5s} (n {len(o["post_sd"]):6d}): post sd {med("post_sd", True)} '
              f'/ dm {med("post_sd_dm", True)} | loss {med("loss")} / dm {med("loss_dm")} | '
              f'gain {med("gain")} / dm {med("gain_dm")}')


for r in sys.argv[1:]:
    read(r)

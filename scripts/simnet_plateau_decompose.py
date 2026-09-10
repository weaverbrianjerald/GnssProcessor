"""Plateau decomposition for the post-relevel-fix stationary ZWD/iono plateau (QUEUE 8b).

Promoted from the 2026-08-20 session scratch join (docs/SPEC_iono_carry_8b.md s2).
Join gates: on runs/camp60_f2 this reproduces grade.json's settled ZWD rms
(2.8451), the SPEC s8 hourly table, and simnet_iono_interaction's settled
interaction rms (3.5678) byte-for-byte before reporting anything new.

Runs against an existing camp60 arm + fixture. Pure stdlib + numpy.

  python plateau_decompose.py <run_dir> <fixture_dir> [--json out.json]

D0: reproduce settled ZWD rms + hourly bins (sanity gate vs SPEC/grade.json).
D1: per-event iono interaction jump (pre vs post windows) by event class.
D2: per-station aggregates: ZWD rms vs event rate vs station-common iono error; exchange corr.
D3: arc-age stratification of interaction rms.
"""
import csv, json, math, os, sys
from collections import defaultdict
import numpy as np

TOW0 = 302400.0
SETTLE_S = 1800.0
EP = 30.0

def q(tow):
    return int(round(float(tow)))

def load_iono(run_dir):
    """(sta,sat) -> dict tow_int -> iono_total_m"""
    out = defaultdict(dict)
    with open(os.path.join(run_dir, 'logs', 'network_ionosphere.csv')) as f:
        r = csv.DictReader(f)
        assert 'iono_total_m' in r.fieldnames, 'need iono_total_m (trace_ionosphere)'
        for row in r:
            out[(row['station'], row['sat'])][q(row['tow'])] = float(row['iono_total_m'])
    return out

def load_truth_atmo(fixture, stations):
    out = defaultdict(dict)
    for sta in stations:
        p = os.path.join(fixture, sta, 'truth_atmo.csv')
        if not os.path.exists(p):
            continue
        with open(p) as f:
            for row in csv.DictReader(f):
                out[(sta, row['satellite'])][q(row['tow_s'])] = float(row['I_L1_m'])
    return out

def load_zwd(run_dir, fixture):
    est = defaultdict(dict)
    with open(os.path.join(run_dir, 'logs', 'network_zwd.csv')) as f:
        for row in csv.DictReader(f):
            est[row['station']][q(row['tow'])] = float(row['zwd_m'])
    err = defaultdict(dict)
    for sta in est:
        p = os.path.join(fixture, sta, 'truth_tropo.csv')
        if not os.path.exists(p):
            continue
        truth = {}
        with open(p) as f:
            for row in csv.DictReader(f):
                truth[q(row['tow_s'])] = float(row['zwd_m'])
        for t, v in est[sta].items():
            if t in truth:
                err[sta][t] = v - truth[t]
    return err

def load_events(run_dir):
    det, t4 = [], []
    with open(os.path.join(run_dir, 'logs', 'network_events.csv')) as f:
        for row in csv.DictReader(f):
            if row['action'] != 'refloat':
                continue
            k = (row['station'], row['sat'], q(row['tow']))
            if row['tier'] in ('T0_mw_detect', 'T1_if_slip'):
                det.append(k)
            elif row['tier'] == 'T4_attrib':
                t4.append(k)
    return det, t4

def two_way_fit(e_by_key, settled_min):
    """alternating-means fit of e = mu + a(sta) + b(sat) over settled triples"""
    rows = []
    for (sta, sat), series in e_by_key.items():
        for t, v in series.items():
            if t >= settled_min:
                rows.append((sta, sat, v))
    mu = float(np.mean([v for _, _, v in rows]))
    a = defaultdict(float); b = defaultdict(float)
    for _ in range(6):
        acc = defaultdict(list)
        for sta, sat, v in rows:
            acc[sta].append(v - mu - b[sat])
        a = {k: float(np.mean(vv)) for k, vv in acc.items()}
        acc = defaultdict(list)
        for sta, sat, v in rows:
            acc[sat].append(v - mu - a[sta])
        b = {k: float(np.mean(vv)) for k, vv in acc.items()}
        resid_mean = float(np.mean([v - mu - a[sta] - b[sat] for sta, sat, v in rows]))
        mu += resid_mean
    r = [v - mu - a[sta] - b[sat] for sta, sat, v in rows]
    return mu, a, b, float(np.sqrt(np.mean(np.square(r))))

def main():
    run_dir, fixture = sys.argv[1], sys.argv[2]
    out_json = None
    if '--json' in sys.argv:
        out_json = sys.argv[sys.argv.index('--json') + 1]
    settled_min = TOW0 + SETTLE_S
    res = {'run_dir': run_dir, 'fixture': fixture}

    # ---------- D0 ZWD sanity ----------
    zerr = load_zwd(run_dir, fixture)
    stations = sorted(zerr.keys())
    settled_half_min = TOW0 + 720 * EP / 2  # grade.py settle-frac 0.5
    all_settled = [v for sta in zerr for t, v in zerr[sta].items() if t >= settled_half_min]
    hourly = []
    for h in range(6):
        lo, hi = TOW0 + h * 3600, TOW0 + (h + 1) * 3600
        vv = [v for sta in zerr for t, v in zerr[sta].items() if lo <= t < hi]
        hourly.append(round(float(np.sqrt(np.mean(np.square(vv)))), 3) if vv else None)
    res['zwd'] = {'settled_rms': round(float(np.sqrt(np.mean(np.square(all_settled)))), 4),
                  'hourly_rms': hourly}
    print('D0 ZWD settled rms %.4f  hourly %s' % (res['zwd']['settled_rms'], hourly))

    # ---------- iono error ----------
    iono = load_iono(run_dir)
    truth = load_truth_atmo(fixture, set(s for s, _ in iono.keys()))
    e = defaultdict(dict)
    for key, series in iono.items():
        tk = truth.get(key)
        if not tk:
            continue
        for t, v in series.items():
            if t in tk:
                e[key][t] = v - tk[t]
    mu, a, b, r_rms = two_way_fit(e, settled_min)
    res['interaction_rms_settled'] = round(r_rms, 4)
    print('two-way fit: mu %.3f  interaction rms (settled) %.4f' % (mu, r_rms))
    # interaction residual for every triple, using settled-window lumps
    r = defaultdict(dict)
    for (sta, sat), series in e.items():
        if sta not in a or sat not in b:
            continue
        for t, v in series.items():
            r[(sta, sat)][t] = v - mu - a[sta] - b[sat]

    # ---------- D1 per-event jumps ----------
    det, t4 = load_events(run_dir)
    res['event_counts'] = {'detector': len(det), 't4': len(t4)}

    def jump_stats(events, label):
        deltas, pres, posts, at0 = [], [], [], []
        by_sta = defaultdict(list)
        zwd_at = []
        for sta, sat, t0 in events:
            series = r.get((sta, sat))
            if not series:
                continue
            pre = [series[t] for t in (t0 - 90, t0 - 60, t0 - 30) if t in series]
            post = [series[t] for t in (t0 + 30, t0 + 60, t0 + 90) if t in series]
            if len(pre) < 2 or len(post) < 2:
                continue
            p, po = float(np.mean(pre)), float(np.mean(post))
            deltas.append(po - p); pres.append(p); posts.append(po)
            if t0 in series:
                at0.append(series[t0])
            by_sta[sta].append(po - p)
            if sta in zerr and t0 in zerr[sta]:
                zwd_at.append((po - p, zerr[sta][t0]))
        d = np.array(deltas); pr = np.array(pres); po = np.array(posts)
        if d.size == 0:   # e.g. raim_arc_persist_epochs starves T4 to zero events
            res['jump_' + label] = {'n': 0}
            print('D1 %-9s n=0 (empty population — nothing to summarise)' % label)
            return
        stats = {
            'n': len(d),
            'delta_rms': round(float(np.sqrt(np.mean(d ** 2))), 4),
            'delta_p50_abs': round(float(np.median(np.abs(d))), 4),
            'delta_p90_abs': round(float(np.percentile(np.abs(d), 90)), 4),
            'delta_mean': round(float(np.mean(d)), 4),
            'pre_rms': round(float(np.sqrt(np.mean(pr ** 2))), 4),
            'post_rms': round(float(np.sqrt(np.mean(po ** 2))), 4),
            'corr_delta_pre': round(float(np.corrcoef(d, pr)[0, 1]), 3) if len(d) > 3 else None,
        }
        # does the post level track -pre (erase of error) or pre (persist)?
        stats['corr_post_pre'] = round(float(np.corrcoef(po, pr)[0, 1]), 3) if len(d) > 3 else None
        if zwd_at:
            za = np.array(zwd_at)
            stats['corr_delta_zwderr'] = round(float(np.corrcoef(za[:, 0], za[:, 1])[0, 1]), 3)
        sta_means = np.array([np.mean(v) for v in by_sta.values() if len(v) >= 5])
        stats['sta_mean_delta_rms'] = round(float(np.sqrt(np.mean(sta_means ** 2))), 4) if len(sta_means) else None
        res['jump_' + label] = stats
        print('D1 %-9s n=%-6d drms=%.3f p50|d|=%.3f pre_rms=%.3f post_rms=%.3f c(d,pre)=%s c(post,pre)=%s c(d,zwd)=%s' % (
            label, stats['n'], stats['delta_rms'], stats['delta_p50_abs'], stats['pre_rms'],
            stats['post_rms'], stats['corr_delta_pre'], stats['corr_post_pre'],
            stats.get('corr_delta_zwderr')))

    jump_stats(det, 'detector')
    jump_stats(t4, 't4')

    # ---------- D2 station aggregates ----------
    ev_count = defaultdict(int)
    for sta, sat, t0 in det + t4:
        ev_count[sta] += 1
    # station-common interaction per epoch
    c_sta = defaultdict(dict)
    for (sta, sat), series in r.items():
        for t, v in series.items():
            c_sta[sta].setdefault(t, []).append(v)
    per_sta = []
    for sta in stations:
        zs = [v for t, v in zerr[sta].items() if t >= settled_min]
        zrms = float(np.sqrt(np.mean(np.square(zs)))) if zs else None
        cc = {t: float(np.mean(v)) for t, v in c_sta.get(sta, {}).items() if t >= settled_min}
        common = np.array([cc[t] for t in sorted(cc)])
        # exchange corr: zwd error vs station-common interaction, settled epochs
        ts = sorted(set(cc) & set(t for t in zerr[sta] if t >= settled_min))
        corr = None
        if len(ts) > 20:
            zv = np.array([zerr[sta][t] for t in ts]); cv = np.array([cc[t] for t in ts])
            if np.std(zv) > 0 and np.std(cv) > 0:
                corr = round(float(np.corrcoef(zv, cv)[0, 1]), 3)
        per_sta.append({'sta': sta, 'zwd_rms': round(zrms, 3) if zrms else None,
                        'events': ev_count[sta],
                        'common_std': round(float(np.std(common)), 3) if len(common) else None,
                        'corr_zwd_common': corr})
    per_sta.sort(key=lambda x: -(x['zwd_rms'] or 0))
    res['stations_worst5'] = per_sta[:5]
    res['stations_best5'] = per_sta[-5:]
    zr = np.array([s['zwd_rms'] for s in per_sta if s['zwd_rms']])
    evn = np.array([s['events'] for s in per_sta if s['zwd_rms']])
    cs = np.array([s['common_std'] for s in per_sta if s['zwd_rms'] and s['common_std']])
    res['fleet_corr'] = {
        'corr_log_zwdrms_events': round(float(np.corrcoef(np.log(zr), np.log(np.maximum(evn, 1)))[0, 1]), 3),
        'corr_log_zwdrms_commonstd': round(float(np.corrcoef(np.log(zr[:len(cs)]), np.log(cs))[0, 1]), 3),
    }
    ex_corrs = [s['corr_zwd_common'] for s in per_sta if s['corr_zwd_common'] is not None]
    res['fleet_corr']['median_exchange_corr'] = round(float(np.median(ex_corrs)), 3)
    print('D2 worst5:', [(s['sta'], s['zwd_rms'], s['events'], s['common_std'], s['corr_zwd_common']) for s in per_sta[:5]])
    print('D2 fleet:', res['fleet_corr'])

    # ---------- D3 arc-age stratification ----------
    ev_by_key = defaultdict(list)
    for sta, sat, t0 in det + t4:
        ev_by_key[(sta, sat)].append(t0)
    for k in ev_by_key:
        ev_by_key[k].sort()
    buckets = [(0, 60), (60, 150), (150, 300), (300, 600), (600, 1200), (1200, 1e9)]
    bins = {b: [] for b in buckets}
    never = []
    import bisect
    for key, series in r.items():
        evs = ev_by_key.get(key, [])
        for t, v in series.items():
            if t < settled_min:
                continue
            if not evs:
                never.append(v); continue
            i = bisect.bisect_right(evs, t)
            if i == 0:
                never.append(v); continue
            age = t - evs[i - 1]
            for b in buckets:
                if b[0] <= age < b[1]:
                    bins[b].append(v); break
    res['age_strata'] = {}
    for b in buckets:
        vv = np.array(bins[b])
        res['age_strata']['%d-%d' % (b[0], min(b[1], 99999))] = {
            'n': len(vv), 'rms': round(float(np.sqrt(np.mean(vv ** 2))), 4) if len(vv) else None}
    nv = np.array(never)
    res['age_strata']['no_event_yet'] = {'n': len(nv), 'rms': round(float(np.sqrt(np.mean(nv ** 2))), 4) if len(nv) else None}
    print('D3 age strata:', {k: v['rms'] for k, v in res['age_strata'].items()},
          'n:', {k: v['n'] for k, v in res['age_strata'].items()})

    if out_json:
        with open(out_json, 'w') as f:
            json.dump(res, f, indent=1)
        print('wrote', out_json)

if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Grade a 305-sta closed-loop consumer run from its trace files.

Consumer logs are dark at 305 stations, so grading uses the network trace
CSVs written under <rundir>/logs/:

  closed_loop_trace.csv : held-out consumer (ZIM200CHE) position error
  network_solution.csv  : per-sat CLK / PB producer state (kind column)

Reports several candidate statistics for the two derived metrics
(de-medianed between-sat clock wander, PB disc churn) so the exact
definition can be pinned against the known `stable` baseline
(medU -1.13, clock wander ~47 cm, PB churn 8672).

Usage: python grade_codeloose.py <rundir> [<rundir> ...]
"""
import csv, sys, statistics, math
from collections import defaultdict

def sat_const(sat):
    return sat[0] if sat else '?'

def grade_consumer(path):
    ups, e3d, nf = [], [], []
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            try:
                ups.append(float(row['up_m']))
                e3d.append(float(row['pos_err_3d_m']))
                nf.append(int(row['num_fixed']))
            except (KeyError, ValueError):
                continue
    if not ups:
        return None
    def med(x): return statistics.median(x)
    def std(x): return statistics.pstdev(x) if len(x) > 1 else 0.0
    return {
        'n_epochs': len(ups),
        'up_final': ups[-1],
        'up_median': med(ups),
        'up_mean': sum(ups)/len(ups),
        'up_std': std(ups),          # U-oscillation
        'up_min': min(ups),
        'up_max': max(ups),
        'e3d_final': e3d[-1],
        'e3d_median': med(e3d),
        'sum_num_fixed': sum(nf),
    }

def grade_network(path):
    # CLK[(sat)] -> list of (tow, value); PB[(sat,signal)] -> list of (tow, disc)
    clk = defaultdict(list)
    pb  = defaultdict(list)
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            kind = row.get('kind','')
            try:
                tow = float(row['tow']); sat = row['sat']
            except (KeyError, ValueError):
                continue
            if kind == 'CLK':
                try: clk[sat].append((tow, float(row['value_m'])))
                except ValueError: pass
            elif kind == 'PB':
                sig = row.get('signal','')
                try: pb[(sat,sig)].append((tow, int(float(row['disc_counter']))))
                except ValueError: pass

    # ---- de-medianed between-sat clock wander ----
    # Group CLK values by (tow, constellation); remove per-epoch per-const median.
    by_epoch_const = defaultdict(list)   # (tow, const) -> [(sat, val)]
    for sat, series in clk.items():
        for tow, val in series:
            by_epoch_const[(tow, sat_const(sat))].append((sat, val))
    resid = defaultdict(list)            # sat -> [de-medianed residual over time]
    pooled = []
    for (tow, const), pairs in by_epoch_const.items():
        vals = [v for _, v in pairs]
        m = statistics.median(vals)
        for sat, v in pairs:
            r = v - m
            resid[sat].append(r)
            pooled.append(r)
    # candidate scalars for "clock wander"
    pooled_std = statistics.pstdev(pooled) if len(pooled) > 1 else 0.0
    per_sat_tstd = {s: (statistics.pstdev(r) if len(r) > 1 else 0.0) for s, r in resid.items()}
    mean_per_sat_tstd = sum(per_sat_tstd.values())/len(per_sat_tstd) if per_sat_tstd else 0.0
    med_per_sat_tstd  = statistics.median(per_sat_tstd.values()) if per_sat_tstd else 0.0
    pooled_rms = math.sqrt(sum(r*r for r in pooled)/len(pooled)) if pooled else 0.0

    # ---- PB disc churn ----
    inc_count = 0     # number of epochs where disc_counter increased
    inc_sum = 0       # total magnitude of increments
    for key, series in pb.items():
        series.sort(key=lambda x: x[0])
        prev = None
        for tow, disc in series:
            if prev is not None and disc > prev:
                inc_count += 1
                inc_sum += (disc - prev)
            prev = disc
    return {
        'n_clk_sats': len(clk),
        'n_pb_series': len(pb),
        'clk_pooled_std_m': pooled_std,
        'clk_pooled_rms_m': pooled_rms,
        'clk_mean_persat_tstd_m': mean_per_sat_tstd,
        'clk_med_persat_tstd_m': med_per_sat_tstd,
        'pb_disc_inc_count': inc_count,
        'pb_disc_inc_sum': inc_sum,
    }

def main():
    for rundir in sys.argv[1:]:
        rundir = rundir.rstrip('/\\')
        print(f"\n================ {rundir} ================")
        c = grade_consumer(rundir + '/logs/closed_loop_trace.csv')
        n = grade_network(rundir + '/logs/network_solution.csv')
        if c:
            print("  CONSUMER (closed_loop_trace.csv):")
            print(f"    epochs           : {c['n_epochs']}")
            print(f"    up_final    (m)  : {c['up_final']:+.4f}")
            print(f"    up_median   (m)  : {c['up_median']:+.4f}")
            print(f"    up_mean     (m)  : {c['up_mean']:+.4f}")
            print(f"    up_std/osc  (m)  : {c['up_std']:.4f}")
            print(f"    up_min/max  (m)  : {c['up_min']:+.4f} / {c['up_max']:+.4f}")
            print(f"    e3d_final   (m)  : {c['e3d_final']:.4f}")
            print(f"    e3d_median  (m)  : {c['e3d_median']:.4f}")
            print(f"    sum_num_fixed    : {c['sum_num_fixed']}")
        else:
            print("  CONSUMER: no data")
        if n:
            print("  NETWORK (network_solution.csv):")
            print(f"    CLK sats / PB series           : {n['n_clk_sats']} / {n['n_pb_series']}")
            print(f"    clk de-median pooled std   (m) : {n['clk_pooled_std_m']:.4f}")
            print(f"    clk de-median pooled rms   (m) : {n['clk_pooled_rms_m']:.4f}")
            print(f"    clk mean per-sat temporal std  : {n['clk_mean_persat_tstd_m']:.4f}")
            print(f"    clk med  per-sat temporal std  : {n['clk_med_persat_tstd_m']:.4f}")
            print(f"    PB disc inc count (churn)      : {n['pb_disc_inc_count']}")
            print(f"    PB disc inc sum                : {n['pb_disc_inc_sum']}")
        else:
            print("  NETWORK: no data")

if __name__ == '__main__':
    main()

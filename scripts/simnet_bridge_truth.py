"""Truth audit of relevel delta-bridge replace events against the sim fixture.

Joins a run's ``bridge_replace`` event rows (logs/network_events.csv, tier
T1_if_slip, requires network.trace_events) against the fixture's per-station
``truth_slips.csv``, per band, and classifies every applied bridge:

  correct  - every solved band's float delta within --tol cyc of the cumulative
             truth delta in the +-window around the flag epoch (tow - lag_s);
  wrong    - some solved band off by more than --tol;
  phantom  - no truth slip in the window on any solved band, yet some
             |delta| >= --tol (the bridge invented a slip);
  trivial  - no truth slip and every |delta| < --tol (a near-zero refinement).

Validation: on runs/camp60_f4 (bridge v2, 32 applies) this reproduces the
SPEC_slip_relevel_walk s8.2 scratch audit: 25 correct / 7 wrong / 0 phantom
(identical at window 15 and 90; the worst wrong row reads 8.12 cyc here versus
the scratch join's 3.5, a difference traced to a double-slip window detail on
one row).

Usage:
  python scripts/simnet_bridge_truth.py <run_dir> <fixture_dir>
      [--window-s 15] [--tol 0.5] [--json out.json]

Traps this join inherits from the family (see simnet_iono_interaction.py):
tow is quantized to int seconds; truth is per-station directory files; the
detail field is ';'-separated with self-describing key=value tokens from
route= onward (v2 rows have no route= token, treated as route "mw").
"""
import argparse
import csv
import json
import os
import sys
from collections import defaultdict


def load_truth(fixture, station):
    """(sat, signal) -> list of (tow_int, delta_cycles)"""
    out = defaultdict(list)
    p = os.path.join(fixture, station, 'truth_slips.csv')
    if not os.path.exists(p):
        return out
    with open(p) as f:
        for row in csv.DictReader(f):
            out[(row['satellite'], row['signal'])].append(
                (int(round(float(row['tow_s']))), int(row['delta_cycles'])))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run_dir')
    ap.add_argument('fixture_dir')
    ap.add_argument('--window-s', type=float, default=15.0,
                    help='truth window half-width around the flag epoch (s).  Default '
                         'matches the exact flag epoch: consecutive same-LOS slips are '
                         'each bridged by their OWN event, and a wide window sums the '
                         'neighbor into this event\'s truth (measured on camp60_b3: '
                         '19 complementary-pair "36-112 cyc wrong" rows at 90 s that '
                         'are 0 at 15 s — every one a correct bridge).')
    ap.add_argument('--tol', type=float, default=0.5,
                    help='per-band correctness tolerance (cycles)')
    ap.add_argument('--json', default=None)
    args = ap.parse_args()

    ev_path = os.path.join(args.run_dir, 'logs', 'network_events.csv')
    if not os.path.exists(ev_path):
        sys.exit('missing %s (arm network.trace_events)' % ev_path)

    truth_cache = {}
    events = []
    with open(ev_path) as f:
        for row in csv.DictReader(f):
            if row['event'] != 'bridge_replace':
                continue
            detail = row['detail'].split(';')
            sigs = [detail[0], detail[1]]
            kv = {}
            for tok in detail[2:]:
                if '=' in tok:
                    k, v = tok.split('=', 1)
                    kv[k] = v
            dns = [float(row['statistic']), float(row['threshold'])]
            if 'dn3' in kv:
                # band 3's signal is not in the row; resolve it from truth keys
                dns.append(float(kv['dn3']))
                sigs.append(None)   # filled from truth below
            events.append({
                'tow': float(row['tow']), 'sat': row['sat'],
                'station': row['station'], 'sigs': sigs, 'dns': dns,
                'lag_s': float(kv.get('lag_s', 0.0)),
                'route': kv.get('route', 'mw'),
            })

    classes = defaultdict(int)
    by_route = defaultdict(lambda: defaultdict(int))
    wrong_rows, worst_err = [], 0.0
    for ev in events:
        sta = ev['station']
        if sta not in truth_cache:
            truth_cache[sta] = load_truth(args.fixture_dir, sta)
        truth = truth_cache[sta]
        flag_tow = int(round(ev['tow'] - ev['lag_s']))
        # resolve band-3 signal: the sat's signal not among sigs[0:2]
        if len(ev['sigs']) == 3 and ev['sigs'][2] is None:
            cands = {sig for (s, sig) in truth if s == ev['sat']} \
                    - set(ev['sigs'][:2])
            ev['sigs'][2] = sorted(cands)[0] if cands else '?'
        errs, any_truth = [], False
        for sig, dn in zip(ev['sigs'], ev['dns']):
            tsum = 0
            for (t, d) in truth.get((ev['sat'], sig), []):
                if abs(t - flag_tow) <= args.window_s:
                    tsum += d
                    any_truth = True
            errs.append(abs(dn - tsum))
        worst = max(errs)
        worst_err = max(worst_err, worst)
        if not any_truth:
            cls = 'phantom' if any(abs(d) >= args.tol for d in ev['dns']) else 'trivial'
        else:
            cls = 'correct' if worst <= args.tol else 'wrong'
        classes[cls] += 1
        by_route[ev['route']][cls] += 1
        if cls in ('wrong', 'phantom'):
            wrong_rows.append({'tow': ev['tow'], 'sat': ev['sat'],
                               'station': sta, 'sigs': ev['sigs'],
                               'dns': ev['dns'], 'errs': [round(e, 2) for e in errs],
                               'route': ev['route'], 'class': cls})

    res = {'run_dir': args.run_dir, 'n_applied': len(events),
           'classes': dict(classes),
           'by_route': {k: dict(v) for k, v in by_route.items()},
           'worst_abs_err_cyc': round(worst_err, 2),
           'wrong_rows': wrong_rows}
    print('bridge truth audit: %d applied -> %s (worst |err| %.2f cyc)' %
          (len(events), dict(classes), worst_err))
    for r, c in sorted(by_route.items()):
        print('  route %-3s: %s' % (r, dict(c)))
    for w in wrong_rows[:20]:
        print('  %(class)s: tow %(tow).0f %(station)s %(sat)s %(sigs)s dn=%(dns)s err=%(errs)s route=%(route)s' % w)
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(res, f, indent=1)
        print('wrote', args.json)


if __name__ == '__main__':
    main()

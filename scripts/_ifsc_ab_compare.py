#!/usr/bin/env python3
"""Compare the two IF-TD pool-scale arms (SPEC_amb_iono_split_overcount §6.14.5 defect 2).

old = reference satellite's structural zero included in the robust scale (as shipped)
new = it is excluded (the fix)

Verifies the claim the fix must satisfy: the pool's flag population is unchanged on a
clean bed, while the scale itself de-deflates. Also reports tier T1r (defect 1), which
is telemetry-only and so must be identical across the two arms.
"""
import csv, sys, statistics as st
from pathlib import Path

def flag_rates(p):
    out = {}
    if not p.exists(): return out
    for r in csv.DictReader(open(p, encoding='utf-8', errors='replace')):
        out[r['tier']] = (int(r['opportunities']), int(r['flags']), r['label'])
    return out

def if_slip(p):
    rows = {}
    if not p.exists(): return rows
    for r in csv.DictReader(open(p, encoding='utf-8', errors='replace')):
        key = (r['week'], r['tow'], r['station'], r['sat'], r['sig1'], r['sig2'])
        rows[key] = r
    return rows

def p50(v): return st.median(v) if v else float('nan')

old_d, new_d = Path('runs/ifsc_old/logs'), Path('runs/ifsc_new/logs')
fo, fn = flag_rates(old_d/'network_flag_rates.csv'), flag_rates(new_d/'network_flag_rates.csv')

print("=" * 78)
print("FLAG-RATE TABLE  (opportunities / flags)")
print("=" * 78)
print(f"{'tier':<5} {'label':<24} {'old opp':>12} {'old flg':>8} {'new opp':>12} {'new flg':>8}  verdict")
for t in sorted(set(fo) | set(fn)):
    o = fo.get(t, (0, 0, '?')); n = fn.get(t, (0, 0, '?'))
    same = (o[0], o[1]) == (n[0], n[1])
    print(f"{t:<5} {n[2][:24]:<24} {o[0]:>12} {o[1]:>8} {n[0]:>12} {n[1]:>8}  "
          f"{'IDENTICAL' if same else '*** CHANGED ***'}")

ro, rn = if_slip(old_d/'network_if_slip.csv'), if_slip(new_d/'network_if_slip.csv')
print()
print("=" * 78)
print("IF-SLIP DEEP TRACE  (row-aligned on week,tow,station,sat,sig1,sig2)")
print("=" * 78)
print(f"rows: old={len(ro)}  new={len(rn)}  common={len(set(ro) & set(rn))}")
common = sorted(set(ro) & set(rn))
if not common:
    print("NO COMMON ROWS — cannot compare."); sys.exit(1)

# The sample SET must be identical: the fix touches the scale, never which samples exist.
print(f"only-in-old={len(set(ro)-set(rn))}  only-in-new={len(set(rn)-set(ro))}"
      f"   -> sample set {'IDENTICAL' if len(ro)==len(rn)==len(common) else '*** DIFFERS ***'}")

fo_set = {k for k in common if ro[k]['flagged'] == '1'}
fn_set = {k for k in common if rn[k]['flagged'] == '1'}
print(f"\nflagged samples: old={len(fo_set)}  new={len(fn_set)}   "
      f"new-only={len(fn_set-fo_set)}  old-only={len(fo_set-fn_set)}  "
      f"-> {'IDENTICAL FLAG POPULATION' if fo_set==fn_set else '*** FLAG POPULATION DIFFERS ***'}")
for k in sorted(fn_set ^ fo_set)[:10]:
    side = 'new-only' if k in fn_set else 'old-only'
    r = rn[k] if k in fn_set else ro[k]
    print(f"   {side}: {k[2]} {k[3]} {k[4]}/{k[5]} tow={k[1]} "
          f"abs_dev={r['abs_dev_m']} thr_old={ro[k]['thr_m']} thr_new={rn[k]['thr_m']}")

# dd/med must be bit-identical: the fix cannot move the measurement or its centre.
dd_ident  = all(ro[k]['dd_m']  == rn[k]['dd_m']  for k in common)
med_ident = all(ro[k]['med_m'] == rn[k]['med_m'] for k in common)
print(f"\ndd_m identical: {dd_ident}    med_m identical: {med_ident}"
      "   (both MUST be True — the fix touches only the scale)")

print("\n" + "=" * 78)
print("THE SCALE ITSELF  (p50 over all trace rows; new/old > 1 == de-deflated)")
print("=" * 78)
for col in ('s_pool_m', 'ewma_scale_m', 's_eff_m', 'thr_m'):
    vo = [float(ro[k][col]) for k in common]
    vn = [float(rn[k][col]) for k in common]
    a, b = p50(vo), p50(vn)
    print(f"  {col:<14} old p50 {a:.6f}   new p50 {b:.6f}   ratio {b/a if a else float('nan'):.4f}"
          f"   ({100*(b/a-1) if a else float('nan'):+.1f} %)")

# Warm rows only (count >= shrink_kappa): where the EWMA half dominates s_eff.
warm = [k for k in common if int(rn[k]['count']) >= 20]
print(f"\n  warm rows (station-pair count >= kappa=20): {len(warm)} of {len(common)}")
for col in ('s_eff_m', 'thr_m'):
    vo = [float(ro[k][col]) for k in warm]; vn = [float(rn[k][col]) for k in warm]
    a, b = p50(vo), p50(vn)
    print(f"  {col:<14} old p50 {a:.6f}   new p50 {b:.6f}   ratio {b/a if a else float('nan'):.4f}"
          f"   ({100*(b/a-1) if a else float('nan'):+.1f} %)")

# Independent cross-check of tier T1r against the event stream (invariant #4:
# a counter is corroborated against a second instrument before it is trusted).
print("\n" + "=" * 78)
print("T1r CROSS-CHECK vs network_events.csv refsat_reset")
print("=" * 78)
for arm, d in (('old', old_d), ('new', new_d)):
    ev = d / 'network_events.csv'
    n_ref = n_if = 0
    if ev.exists():
        for r in csv.DictReader(open(ev, encoding='utf-8', errors='replace')):
            v = ','.join(r.values())
            if 'refsat_reset' in v: n_ref += 1
            elif 'if_slip' in v:    n_if  += 1
    t1r = (fo if arm == 'old' else fn).get('T1r', (0, 0, ''))
    t1  = (fo if arm == 'old' else fn).get('T1',  (0, 0, ''))
    print(f"  {arm}: events refsat_reset={n_ref:<7} T1r flags={t1r[1]:<7} | "
          f"events if_slip={n_if:<7} T1 flags={t1[1]}")

# The solve must not move if no flag moved.
print("\n" + "=" * 78)
print("SOLUTION")
print("=" * 78)
so, sn = old_d/'network_solution.csv', new_d/'network_solution.csv'
if so.exists() and sn.exists():
    bo, bn = so.read_bytes(), sn.read_bytes()
    print(f"  network_solution.csv  old={len(bo)} B  new={len(bn)} B  -> "
          f"{'BYTE-IDENTICAL' if bo == bn else '*** DIFFERS ***'}")
else:
    print("  network_solution.csv missing in one or both arms")

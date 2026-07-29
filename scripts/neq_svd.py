"""Offline spectral analysis of a network NEQ export (logs/neq_export/).

Usage: python neq_svd.py <export_dir> [n_weak]

1. Loads the full arrowhead (border + per-station blocks + labels).
2. Validates the export: rebuilds the Schur complement from the blocks and
   compares against the dumped reduced border (difference = folded carry
   priors / gauges, reported not failed).
3. Assembles the full system, Jacobi-scales it (D^-1/2 N D^-1/2), and
   eigen-decomposes.
4. Reports the weakest post-datum directions with per-family mass and the
   top-loading labeled columns.
"""
import csv, sys, math
import numpy as np
from pathlib import Path
from collections import defaultdict

d = Path(sys.argv[1])
N_WEAK = int(sys.argv[2]) if len(sys.argv) > 2 else 12

meta = next(csv.DictReader(open(d / 'meta.csv')))
ns = int(meta['n_shared'])
print(f"export: week {meta['week']} tow {meta['tow']} epoch {meta['epoch']} | n_shared={ns} n_stations={meta['n_stations']}")

shared_labels = [''] * ns
for r in csv.DictReader(open(d / 'shared_labels.csv')):
    shared_labels[int(r['col'])] = r['label']

sta = list(csv.DictReader(open(d / 'stations_index.csv')))   # est_slot,name,n_local (file order = concat order)
sta_labels = defaultdict(dict)
for r in csv.DictReader(open(d / 'stations_labels.csv')):
    sta_labels[int(r['est_slot'])][int(r['local_col'])] = r['label']

Nss_raw = np.fromfile(d / 'Nss_raw.bin').reshape(ns, ns)
us_raw  = np.fromfile(d / 'us_raw.bin')
Nss_red = np.fromfile(d / 'Nss_red.bin').reshape(ns, ns)

nsr_all = np.fromfile(d / 'stations_Nsr.bin')
nrr_all = np.fromfile(d / 'stations_Nrr.bin')
off_sr = off_rr = 0
blocks = []
for s in sta:
    nl = int(s['n_local'])
    Nsr = nsr_all[off_sr:off_sr + ns * nl].reshape(ns, nl); off_sr += ns * nl
    Nrr = nrr_all[off_rr:off_rr + nl * nl].reshape(nl, nl); off_rr += nl * nl
    blocks.append((int(s['est_slot']), s['name'], nl, Nsr, Nrr))

# --- 2. Schur validation -----------------------------------------------------
S = Nss_raw.copy()
for slot, name, nl, Nsr, Nrr in blocks:
    if nl == 0: continue
    S -= Nsr @ np.linalg.solve(Nrr, Nsr.T)
resid = Nss_red - S
print(f"schur check: ||rebuilt - dumped_red||_F / ||dumped_red||_F = "
      f"{np.linalg.norm(resid) / max(np.linalg.norm(Nss_red), 1e-30):.3e} "
      f"(nonzero part = folded carry priors / gauges / datum)")

# --- 3. Full assembly + scaled spectrum --------------------------------------
n_loc = sum(b[2] for b in blocks)
n_tot = n_loc + ns
print(f"full system: {n_tot} states ({n_loc} local + {ns} shared)")
N = np.zeros((n_tot, n_tot))
labels, family = [], []

def fam_of(lbl):
    head = lbl.split(':')[0].upper()
    # normalize StateManager label heads and shared heads into families
    if head.startswith('AMB'): return 'AMB'
    if 'IONO' in head: return 'IONO'
    if 'TROP' in head or head == 'ZWD': return 'TROPO'
    if head in ('CLK', 'CLOCK', 'RCLK'): return 'CLK'
    if head.startswith('CB') or 'CODE_BIAS' in head or 'CODEBIAS' in head: return 'CB'
    if head.startswith('PB') or 'PHASE_BIAS' in head or 'PHASEBIAS' in head: return 'PB'
    if head.startswith('SH'): return 'SH'
    if 'POS' in head: return 'POS'
    return head[:8]

pos = 0
for slot, name, nl, Nsr, Nrr in blocks:
    if nl == 0: continue
    N[pos:pos + nl, pos:pos + nl] = Nrr
    N[pos:pos + nl, n_loc:] = Nsr.T
    N[n_loc:, pos:pos + nl] = Nsr
    for c in range(nl):
        lbl = sta_labels[slot].get(c, '?')
        labels.append(f"{name}:{lbl}")
        family.append(fam_of(lbl))
    pos += nl
N[n_loc:, n_loc:] = Nss_raw
labels += shared_labels
family += [fam_of(l) for l in shared_labels]

diag = np.diag(N).copy()
diag[diag <= 0] = 1.0
dinv = 1.0 / np.sqrt(diag)
Nsc = N * dinv[:, None] * dinv[None, :]
w, V = np.linalg.eigh(Nsc)
print(f"scaled spectrum: min={w[0]:.3e} max={w[-1]:.3e} cond={w[-1]/max(w[0],1e-300):.2e}")

# --- 4. Weak-direction attribution -------------------------------------------
fam_set = sorted(set(family))
fam_idx = {f: np.array([i for i, x in enumerate(family) if x == f]) for f in fam_set}
print(f"\nweakest {N_WEAK} directions (scaled eigenvalue; family mass %; top loadings):")
for k in range(N_WEAK):
    v = V[:, k]
    masses = {f: float(np.sum(v[idx] ** 2)) for f, idx in fam_idx.items()}
    top_f = sorted(masses.items(), key=lambda kv: -kv[1])[:4]
    mass_str = ' '.join(f"{f}:{m*100:.0f}%" for f, m in top_f if m > 0.02)
    order = np.argsort(-v ** 2)[:4]
    load_str = ' | '.join(f"{labels[i]}({v[i]**2*100:.0f}%)" for i in order)
    print(f"  ev[{k}]={w[k]:.3e}  {mass_str:40s}  {load_str}")

# family-average diagonal information (how well-determined each family is on average)
print("\nper-family scaled marginal sigma (median sqrt of scaled-inverse diagonal):")
Ninv = np.linalg.pinv(Nsc, rcond=1e-12)
md = np.sqrt(np.maximum(np.diag(Ninv), 0))
for f in fam_set:
    vals = md[fam_idx[f]]
    print(f"  {f:8s} n={len(vals):5d}  med={np.median(vals):9.3f}  p90={np.percentile(vals,90):9.3f}")

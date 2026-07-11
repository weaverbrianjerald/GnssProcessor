# HANDOFF — T4 run results: GPS+Gal L1/L2/L5 network A/B (2026-07-10)

Executes TIER 4 of `docs/HANDOFF_ddfrac_shvtec_treatment.md` (RUN, not build). All
Tier-1/2/3 cards were already built (uncommitted); this is the definitive experiment.
Source memories: `project_ddfrac_shvtec_cards_impl`, `project_network_ddfrac_rootcause_shvtec`,
`feedback_network_nonnegotiable_configs`.

## Config finding (BEFORE any run): "GPS-only L1/L2 single-channel" is unobservable with SH

The user's requested fast config (GPS-only, L1/L2, single channel) **cannot produce any
network products with SH-VTEC on** — the observability gate withholds every epoch. Proven
across a 7-config smoke matrix:

| systems | channel | freq | SH | emits? |
|---|---|---|---|---|
| GPS | single | L1/L2 | on | 0/10 ✗ |
| GPS | all | L1/L2 | on | 0/10 ✗ |
| GPS | single | L1/L2, a00 prior 15±3 | on | 0/10 ✗ |
| GPS+Gal | single | L1/L2 | on | 0/10 ✗ |
| GPS+Gal | all | L1/L2 (Dual) | on | 0/10 ✗ |
| **GPS+Gal** | **all** | **L1/L2/L5 (Triple)** | **on** | **10/10 ✓** |
| GPS | single | L1/L2 | **off** | 10/10 ✓ |

Mechanism: `estimate_iono_sh` inherently co-estimates **per-(sat,signal) satellite code
biases** (`gnss_config` banner: "co-estimating the SH-VTEC … jointly with the satellite
code biases"). Observing those OSBs needs ≥2 constellations (DCB↔TEC-level separation) +
multi-channel + ≥3 frequencies. Single-constellation / single-channel / dual-frequency
each independently trips the gate. **Minimal SH-viable config = GPS+Galileo, all-channel,
Triple (L1/L2/L5 + E1/E5a/E5b).** User approved this substitution.

## Configs (config/test_suite/)

`_t4_gg_untreated_3h.json`, `_t4_gg_treated_3h.json`, `_t4_gg_treated_d12_3h.json`
(+ `_smoke` variants). 50 stations, GPS+Gal, Triple, all-channel, 25° mask, degree-4 SH
(d12 arm = degree-12), zero-q phase leveling + MW slip (auto), DD-frac tracer
(`trace_dd_fractional`, max_pairs 2000), `epoch_decimation_n=2` (60 s effective), closed-loop
CIT100USA consumer AR armed (σ-gate force-capped 0.20 m). Treated arms T1b
`row_decimation_epochs=3` (SH-row σ-inflation), T1c floors (pl 4e-4 / pb 3e-6 / cb 3e-4),
T1d warmup-on-convergence + refloat. **A/B contrast = ONLY those 7 treatment vars.**
Runtime: 50 sta, ~540 ms/epoch (deg-4) / ~800 ms (deg-12), 180 epochs, ~2-3 min wall/run.
All three: 180/180 emitted, 0 withheld, no crash.

## Grading (final 60 min, tow ≥ 266340; grader = scratch grade_t4.py, viewer benchmark)

| metric | untreated d4 | treated d4 | treated d12 | AR-grade target |
|---|---|---|---|---|
| (a) mean\|frac\| | 0.239 | 0.241 | 0.251 | ≪ 0.25 |
| (a) P(\|f\|<0.1) | 0.233 | 0.214 | 0.201 | ≫ 0.20 |
| (a) circular R | 0.056 | 0.059 | 0.011 | ≫ 0 |
| (b) σ-calib (scatter/formal) | **46.9×** | **3.70×** | 3.85× | ~1 |
| (b) formal dd_σ (cyc) | 0.005 | 0.104 | 0.104 | — |
| (d) DD step med (cyc/ep) | 0.023* | 0.068 | 0.063 | < 0.05 |
| (e) AR gate | **sigma_gate** | **ratio_battery** | ratio_battery | reach + pass ratio |
| (e) consumer float 3D min | 8.19 m | 0.98 m | 0.51 m | < 0.20 m σ |
| (e) best ratio | 0.00 (never ran) | 1.12 | 1.21 | ≥ 3.0 |
| (e) fixes | 0 | 0 | 0 | > 0 |
| SH a00 (TECU) | 49.2 ± 13.2 | 40.4 ± 4.1 | 39.9 ± 5.1 | 15±30 band |
| SH LOS>3σ frac | 0.853 | 0.769 | 0.793 | ≪ |
| churn (log hits) | 7 | 7 | 7 | low |

\* untreated's *smaller* step is the σ-incest freeze artifact (formal σ collapsed to
0.005 cyc → stiff filter → floats frozen at wrong values), not stability.

Tail |frac| deciles (%), uniform = 10 each:
```
untreat d4  :  9  9  9  9 10 12 10  9  9  9
treated d4  :  9  9 10 11 10 10  9  9  8  9
treated d12 : 10  9  9 10 10  9  9  9 10 10
```

## Verdict

**PRIMARY T4 falsifier FAILS (decisive negative, as T4a anticipated):** the DD fractions
do **not** leave the uniform regime — flat within ±2% of uniform in all three arms. The
network still emits phase biases carrying **zero integer information**; user AR reaches
MLAMBDA (treated) but the ratio is ~1.1-1.2 (no dominant integer) → 0 fixes.

**BUT the Tier-1 treatment delivers real, measurable secondary gains** (untreated→treated,
deg-4): σ-calibration 46.9×→3.70× overconfident (13×), formal σ 0.005→0.104 cyc (σ-incest
collapse cured 21×), consumer float 8.19→0.98 m, SH a00 stability ±13.2→±4.1 TECU, and it
**unblocks AR** — the gate moves from `sigma_gate` (float never converges, MLAMBDA never
runs — the rootcause's "0.20 m force-cap blocked MLAMBDA") to `ratio_battery` (float
converges, MLAMBDA runs, ratio fails). So the treatment fixes the *σ-incest / warmup-lock*
half of the rootcause; it does **not** fix the *SH-representation-error → ambiguity leakage*
half.

**Degree is NOT the lever (confound ruled out):** degree-12 (169 coeff) vs degree-4 (25)
leaves the fractions uniform (R 0.011) and the LOS>3σ misfit essentially unchanged
(0.79 vs 0.77). ~77-79% of per-LOS slant-iono residuals exceed 3σ at any degree — the
single-layer SH surface structurally cannot represent per-LOS slant iono (per-station /
per-satellite DCB-like + mapping-error structure), and that misfit is squeezed dispersively
into the ambiguities. Confirms the rootcause's degree-independent / station-independent claim
directly on this config.

## Next (expand from here) — target the SH representation error, NOT scale

Scaling to 305-sta / degree-15 (T4b) is **not** expected to help (degree- and
station-independent, proven above + rootcause). The real levers are FABLE-GATE items:

1. **Correlated SH representativeness-error state** (rootcause §1, beyond T1b's white-row
   σ-inflation): model the per-LOS SH misfit as a time-correlated / per-station error state
   so it stops leaking into the ambiguities. The σ-inflation already helped calibration
   (47→3.7×); a proper correlated-error state should push it toward 1 AND stop the leakage.
2. **Per-LOS / local iono augmentation**: add per-station VTEC offset or local gradient
   states to absorb the ~78% >3σ residual the global surface can't fit.
3. **Eq-34 integer datum** (per-(receiver,signal) phase-bias states + single-receiver datum):
   the DD floats being uniform also carries the datum-non-integer defect; both must be solved
   for AR-grade output.

The consumer float now converges to sub-metre and MLAMBDA runs — the *only* remaining
blocker for fixes is the non-integer DD floats themselves (ratio ~1.1). That isolates the
target cleanly for the structural work above.

## T4b confirmation — 305 stations, degree-15, 2 h (the definitive run)

Ran the full T4b confirmation to rule out convergence at scale + degree: **305 stations,
degree-15 SH, GPS+Gal all-channel, treated (T1b/c/d armed), producer-only, 2 h / decim-2,
DD tracer subsampled, snapshots armed** (`_t4b_305_treated_2h.json`, `runs/t4b_305_2h/`).
Ran clean: **120/120 epochs emitted, 0 withheld, no crash** (the ~1.5 h non-PD risk appeared
only as station SULP00UKR dropped for 3 epochs near t=1.9 h — graceful per-epoch degradation,
not a whole-run crash). 9.8 s/epoch, ~21 min wall.

**Grade (final 60 min, n = 122,000 DD samples):**

| metric | 305 deg-15 treated | 50 deg-4 treated | AR target |
|---|---|---|---|
| mean\|frac\| | **0.250** | 0.241 | ≪ 0.25 |
| P(\|f\|<0.1) | **0.202** | 0.214 | ≫ 0.20 |
| circular R | **0.007** | 0.059 | ≫ 0 |
| σ-calib | 4.53× | 3.70× | ~1 |
| formal dd_σ | 0.099 cyc | 0.104 cyc | — |
| DD step med | 0.077 cyc/ep | 0.068 | < 0.05 |
| SH a00 | **24.1 ± 2.5 TECU** | 40.4 ± 4.1 | 15±30 |
| SH LOS>3σ frac | **0.809** | 0.769 | ≪ |

deciles(%): `9 9 9 9 10 10 9 10 10 10` — flat to ±1% over 122k samples.

**Verdict: CONVERGENCE RULED OUT.** At full scale + degree-15, the DD fractions are
statistically indistinguishable from uniform (R=0.007) — zero integer information. Station
count and SH degree are both confirmed irrelevant (50→305, deg-4→12→15 all flat). The network
does not converge to AR-grade phase biases.

**Two treatment wins survive even here, both meaningful:**
1. **a00 divergence TAMED.** The untreated deg-15/305 config diverges to a00 ≈ +296 → −91 TECU
   (the audit pathology, memory `project_network_ddfrac_rootcause_shvtec`). The treated run holds
   **a00 = 24.1 ± 2.5 TECU** (in-band, stable, kaula_ratio_deg0 = 0.80) — T1b σ-inflation + T1d
   warmup-on-convergence cure the a00 blow-up at scale.
2. **σ-incest slowed** (formal σ 0.099 vs untreated ~0.005 cyc collapse).

**But the definitive blocker is untouched:** even a degree-15 surface (256 coeff) leaves
**80.9 % of per-LOS slant-iono residuals > 3σ**. The single-layer SH representation error is
degree-invariant, and its dispersive leakage into the ambiguities keeps the fractions uniform.
The a00 *level* was never the issue; the per-LOS *structure* the smooth surface cannot carry is.
→ Next = correlated/per-station iono-error state, NOT more scale or degree.

## For the Fable session — correlated per-station iono representativeness-error state

This is a FABLE-GATE item (StateManager / SH↔slant coupling change; Opus should not free-hand
it). Concrete spec-starter:

**Problem (measured, this doc):** each per-(station,sat) slant-iono state is pinned to the
global SH-VTEC surface by a pseudo-obs row appended every epoch (grep `appendShIonoRows` in
`src/core/app/ApplicationNetwork.cpp` — was ~2196-2291; line numbers drift, re-grep). σ =
max(0.05 m floor, 5 TECU·map·0.162). The smooth single-layer surface cannot represent the
real per-LOS slant (per-station/per-sat DCB-like + mapping-function + small-scale structure):
**80.9 % of LOS residuals > 3σ at degree-15.** That misfit is forced into the only nearby free
parameter — the float ambiguity — dispersively (μ_j band-ratio signature, memory
`project_network_ddfrac_rootcause_shvtec`), which is why the DD fractions are uniform.

**Fix to design:** give the SH→slant pin a *correlated representativeness-error component* so
the misfit lands in an iono-error state instead of the ambiguity. Two variants to weigh:
1. Augment each (station,sat) slant-iono state with a **time-correlated (Gauss-Markov)
   representativeness-error term** (the pseudo-obs error is currently applied WHITE every epoch
   — T1b only inflated its σ; it never modeled the correlation). This is the minimal,
   StateManager-local version.
2. Add **per-station VTEC-offset / local-gradient states** (a few per station) that absorb the
   systematic part of the per-station misfit the global surface leaves behind — a coarse local
   iono layer on top of the global SH.

Either way: keep it default-OFF + byte-identical when off (project convention), and the SH
non-negotiable stays on.

**Falsifier (already built — reuse verbatim):** rerun `config/test_suite/_t4b_305_treated_2h.json`
(or the 50-sta `_t4_gg_treated_3h.json` for fast iteration) with the new state armed, then
`python <scratch>/grade_t4.py <run_dir> 3600`. Success = **SH LOS>3σ frac drops well below 0.8
AND the DD fractions leave uniform (R ≫ 0, P(<0.1) ≫ 0.20, σ-calib → ~1)**. The consumer float
already converges sub-metre and MLAMBDA already runs (gate = `ratio_battery`, ratio ~1.1), so
the *only* thing standing between here and integer fixes is making these fractions non-uniform.
If LOS>3σ drops but fractions stay uniform, that's itself a decisive finding (misfit was
re-absorbed elsewhere, not the ambiguities).

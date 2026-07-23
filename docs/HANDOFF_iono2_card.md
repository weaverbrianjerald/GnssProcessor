# Iono-2 card — dual-layer SH-VTEC + exact per-LOS deviation carry (2026-07-17)

The NL-AR iono-representation card. Successor to T4/T5 per the Jul-17 decisive
A/B (`_wlver_150sta_60ep_d10_gps_tri_sh15_nolane` vs 50/deg-8): OSB tightened
3.5-5x with 3x stations + 3x SH capacity while **NL circR was unmoved and
LOS>3sigma stayed flat ~70%** — the NL blocker is the iono *representation*
(single thin shell, per-epoch white pin), not observability.  Combined with the
Jul-15 structural finding (the producer is per-epoch; per-LOS slant iono is
re-solved fresh every epoch — `ionosphere_q` is a phantom knob on this path),
NL needs what EWL/WL got from the MW/lane cards: a **temporal accumulation
channel**, here for the per-LOS ionosphere.

Two mechanisms, both default-OFF, independently armable, one card:

## A. Dual-layer SH backbone (`layer2_max_degree > 0`)

Second thin shell (plasmasphere-side) with its own coefficient set, shell
height, Kaula scale, RW q, and a00 anchor.  The pin row becomes

    iota  =  Sum_l  M_l(el) * tecu_to_L1m * SH_l(IPP_l) * c_l   (+ d)

with per-layer IPP geometry (different H => different mapping AND different
pierce lat/lon).  Rationale: the two mapping profiles (450 vs 1300 km:
M=2.55 vs 1.74 at el=10, 1.60 vs 1.36 at 25) plus IPP displacement give the
field elevation-dependent per-LOS structure a single shell cannot represent,
and shrink the deviation the GM carry (mechanism B) must absorb.

Separability discipline (the a00-split direction is quasi-null): layer-2 a00
gets a TIGHT climatology anchor (`layer2_mean_vtec_prior_tecu` +/-
`layer2_mean_vtec_prior_sigma_tecu`, default 3 +/- 5 TECU) while the total
stays data-determined through layer-1's loose 15 +/- 30.  The a00 physical-band
WARN monitors the LAYER SUM.  Layer-2 Kaula default 3 TECU (smoother, smaller
upper field).

Config (`estimation.global_vtec_sh.*`, all parsed in GnssConfigLoad.cpp):
`layer2_max_degree` (int, 0 = OFF), `layer2_height_km` (1300),
`layer2_kaula_scale_tecu` (3), `layer2_q_per_degree` (1e-2),
`layer2_mean_vtec_prior_tecu` (3), `layer2_mean_vtec_prior_sigma_tecu` (5).

Key schema: layer-1 coefficients keep `SH|n|m|C` unchanged; layer-2 uses the
distinct prefix `SH2|n|m|C` (parseVtecShKey hard-rejects extra fields, so a
5-field key is NOT backward-parseable — distinct prefix is).  Snapshot
D-records smuggle the layer in the (empty-for-VtecSh) sat field as "L1"
(old readers mismatch the tuple key -> climatology reseed, safe by
construction).  Snapshot fingerprint gains a conditional `shl2=<deg>@<H>km`
term only when armed (pre-existing fingerprints unchanged).

Touched sites (from the Jul-17 surface map): StateKeySchema make/parse/is;
NetworkEstimator ColumnDesc/SharedPrior sh_layer + registerVtecCoeff /
vtecCoeffColumn layer overloads + label `SH2:C:n:m`; per-epoch registration
(Assembly ~875); networkInjectShVtecPrior per-layer Kaula/q/anchor + 4-tuple
carry key; networkStashCarry CarryPrior.sh_layer; NetworkCoupling couplingId
`V2|n|m|t` + per-layer couplingQ (shmarg stays kind-based = covers both);
row builder layer loop; T5 harvest sh_hat layer sum; consumer closed-loop
supply layer sum + formal-sigma over concatenated basis; health sweep misfit
vs SUM + per-layer degree-RMS columns (armed-only, so OFF artifact set is
unchanged); vtec_sh.csv gains a `layer` column armed-only; a00 WARN on sum;
pl_warmup latch stays layer-1 (low-degree convergence proxy).  RealTimeEngine
consumer is slant-value agnostic — untouched.  SH still has no on-disk
product serialization.

## B. Exact per-LOS deviation carry (`rep_error_tau_s > 0`, T5 v2)

The T5 v1 scalar filter was falsified as CIRCULAR (Jul-10): it re-consumed the
solved pin residual — which its own RHS-shifted row had regularized toward
`dev_pred` — as an independent floor-noise observation, so P collapsed to
floor^2 after one update and dev degenerated to a lag-1 copy.  The memory
noted "an exact-state version would differ"; this is that version.  Schur
elimination of an explicit GM state d from {pin row `iota - SH*c - d = eps(f^2)`,
prior `d ~ N(d_pred, P)`} reproduces the EXISTING assembly row
`iota - SH*c = d_pred + eps(f^2 + P)` exactly — so the assembly mechanism is
already correct and only the harvest changes:

Exact scalar posterior per (station, sat), all in the epoch's slant frame
(f = iono_weighting.floor_m, effective f^2/w_s under station deweight):

    u        = iota - Sum_l b_l^T c_l          (the pin functional)
    u_hat    = resid  (existing computation, layer-summed)
    V_hat    = Var_post(u) = cond_var + (m_iota + b_s)^T Sigma_free (m_iota + b_s)
               — ONE est.stationRegression(est_idx, iono_col) per LOS
               (cond_var + regression row m_iota), Sigma_free =
               est.datumFrameSharedCov(datum) built ONCE per epoch; b_s = the
               row's border weights (+M_l*tecu*basis_l on each layer's cols)
               embedded in shared space.  Sign collapse per the bordered
               inverse: Cov(iota, c) = -(Sigma_free m_iota)|_c.  Pinned border
               columns self-zero (exact gauge treatment — never amputate the
               row, Card-1e lesson).
    a        = P_pred + f_eff^2
    d_hat    = d_pred + [P_pred / a] * (u_hat - d_pred)          (gain from the
               PREDICTED variance — the v1 bug used the collapsing posterior)
    U        = V_hat * a / (a - V_hat)     (data-only info recovered; V_hat >= a
               => data added nothing => P+ = P_pred, d_hat per formula)
    P+       = P_pred * (U + f_eff^2) / (U + a)
               limits: U->inf => P_pred (no info);  U->0 => P_pred*f^2/a.

Carried in **vertical TECU** (divide by M_1(el)*tecu_to_L1m at harvest,
multiply at consumption): the deviation and its GM steady state become
elevation-stationary, fixing v1's slant-meter carry whose level was never
re-scaled as elevation changed.  sigma_ss = representation_sigma_tecu, now a
constant (no per-harvest var_ss refresh).  GM aging stays in the serial
PASS-B pre-pass: lam = exp(-dt/tau); d *= lam; P = lam^2 P + (1-lam^2)
sigma_ss^2; erase at dt > 5 tau.

Level-lump note: resid contains the per-(rx,const) receiver-DCB level (Odijk
lump).  It rides inside d deliberately — the common-mode-free R keeps the pin
rows level-blind, so the carried level is inert in the solve and is refreshed
at every harvest; V_hat correctly prices it because the level is GF-code
observed (dm-scale), not sigma_c-scale.  No de-meaning in v1.

Slip integration (user spec Jul-12, built in from day one): the PASS-B serial
pre-pass ERASES the (station,sat) entry when any of the epoch's
`carry_slipped_ambs` entries (per-signal keys; PASS-A MW/LLI/IF-TD detection
site + prior-epoch D5 innovation drain — the C3c funnel, immune to the
pending-reseed clear) projects to that satellite via parseAmbiguityKey.
Reset-not-repair: cold re-tap at (0, sigma_ss^2).  The set reaches the
assembly by const ref (read-only projection; producers keep the non-const
funnel contract).  The satellite-level within-epoch RAIM channel
(carry_we_drop_*) is NOT merged — it is live only inside the RAIM re-solve
window and inert under carry_invariant; revisit if RAIM activates.

Known v1 approximations (documented, falsifiable via carry_vce / M2 LS-VCE
calib): (i) per-LOS scalar treatment ignores cross-LOS posterior correlation
and the common-mode row coupling (conservative direction for the level);
(ii) cross-epoch correlation between d and the border SH carry is compensated
only by the GM leak (1-lam^2) sigma_ss^2 — the Card-1d exact border-motion
treatment is the v2 escalation if calib shows incest; (iii) w_s <= 0 stations
(full iono-deweight decouple) get NO harvest update (no pin row => no new
info) — v1 updated them anyway, a defect fixed here.

Detector-degradation loop (named per user spec): storm iono degrades the
MW/GF detectors (false alarms => excess dev resets exactly when accumulation
matters most; misses => poisoned carry).  MW thresholds stay empirical
(MAD+Welford+EWMA); the D5 innovation gate on the carried DDs is the
estimation-side backstop; `shdev_slip_resets` in the run-end line is the
observable.  Confidence-gated updates are a v2 option.

Counters (invariant #4 — run-end line gated on sh_dev_on):
`shdev_warm` (warm-prior rows consumed), `shdev_slip_resets`,
`shdev_cold` (cold seeds), `shdev_nodata` (V_hat >= a skips).

## Gates

1. Unit: layer key round-trip + old-key compat; two-layer registration
   distinct columns + per-layer prior routing; two-layer row synthesis ==
   analytic sum at both IPPs; exact-update pure function (extracted to
   NetworkRunTypes) — weak-data limit P+ -> P_pred, strong-data limit
   P+ -> P_pred*f^2/a, NON-COLLAPSE vs the v1 recursion; slip projection
   helper (per-signal set -> per-sat verdict, LLI single-band union).
2. Suite green Debug+Release; OFF byte-identity same-build vs pre-card
   Release on the 10-ep bench (renamed exe, compare_network_run.py protocol).
3. ON smoke (10-ep derivative config `_wlver_50sta_10ep_d10_gps_tri_iono2`):
   counters live (nonzero warm/cold; slip resets if events), a00 sum
   physical, LOS>3sigma vs 0.73 baseline.
4. 60-ep readout (`_wlver_50sta_60ep_d10_gps_tri_iono2`): grade_ddfrac
   per-signal NL circR vs the 0.02-0.06 baseline + DD-step trend + dev-P
   contraction; decisive multi-hour run on command per staged cadence.

Validation configs derive from `_wlver_50sta_{10,60}ep_d10_gps_tri_sh8.json`
(diff: + rep_error_tau_s 900, + layer2 block deg-4 @ 1300 km).  Do NOT mutate
the _wlver bench files.

## AS-BUILT GATE RESULTS (2026-07-21, all PASS)

- Suite: 827 ran / 826 pass / 1 skip, Debug AND Release (incl. the 6 new
  gates: layer key round-trip, dual-layer view + slant-sum, per-layer
  estimator registration/priors, and the 3 shDevExactUpdate gates — the
  non-collapse falsifier shows v1 collapsing to <3 f^2 while v2 holds >30 f^2
  and reproduces the batch information sum exactly).
- OFF byte-identity: 13-arm decomp matrix vs the frozen goldens
  (--maskspec goldens/decomp/maskspec.json), renamed exe
  gnss_processor_iono2.exe: 11/13 byte-identical.  The 2 fails (lane, uwl)
  are each EXACTLY the pre-existing `fold_blocked=false` WL-3 config-echo
  token from the LF-1 session's uncommitted loader change (GnssConfigLoadNetwork
  381/2279, a file this card never touched) — zero solve-artifact diffs; those
  goldens need an LF-1-side refresh.
- FP lesson (caught by the consumer arm, cured): the row builder's sigma was
  regrouped rep*(map*tecu) vs the legacy (rep*map)*tecu — non-associative FP
  drifted the solve checksum at 1e-14 and the dd-frac/product last digits.
  Restored the literal product chain.  Note the drift showed in the
  consumer arm ONLY (em25/deg-4/Gal operand set); allon/estim passed with the
  regrouped form — passing SOME arms is not passing the gate.
- ON smoke (10 ep): 11 emitted / 0 withheld; counters live — 2791 warm, 425
  cold, 127 slip resets (C3c funnel), 1669 no-info skips; a00 25.11 =
  24.87 + 0.24 (split anchored).  Harvest cost ~40 ms/ep @ 50 sta (3.2% net).
- ON 60-ep vs OFF control (same build, same grader, whole-run pooled):
  circR 0.016 vs 0.019, step med 3.707 vs 3.742 cyc/ep, calib 0.45 vs 0.46 —
  STATISTICAL PARITY, expected: tau=900 s means the carry completes ~2 time
  constants in a 30-min run; no regression, 60/60 emitted, a00 in band.
  no-info harvest fraction fell 52% (10 ep) -> 11% (60 ep) = information
  flowing as the field converges.  Layer-1 a00 tail 10.4+/-8.9 (vs OFF
  21.5+/-4.0) with layer-2 ~2.8: the deviations + second shell re-apportion
  structure the single-layer mean previously absorbed — watch the SUM on the
  long run (grade_ddfrac's a00 column reads layer-1 only under dual-layer).
- LOS>3sigma reads 0.78 under T5v2 BY DESIGN (the health sweep standardizes
  the residual vs the SH surface alone; the tracked deviation IS that
  residual).  It is no longer the misfit-suppression metric when the carry is
  armed.

## NEXT (on command per the staged-cadence directive)

The decisive falsifier is the MULTI-HOUR arm (3h+, ideally the eq34 3h
lineage cadence): tau=900 s accumulation + position/tropo convergence are the
preconditions the 60-ep bench cannot supply.  Success = per-signal NL circR
rising above the 0.02-0.06 floor + DD step trending below the ~2 cyc/ep
shared-param floor + carry_vce calib staying ~1 (the v1-incest watchdog).
Tuning axes if partial: tau (900 -> 1800/3600 as arcs mature), layer2 degree
(4 -> 8), representation_sigma_tecu.

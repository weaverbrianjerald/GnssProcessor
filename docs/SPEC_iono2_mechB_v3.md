# SPEC — the Iono-2 card in one file: dual-layer SH backbone (mech A), exact per-LOS deviation carry (mech B v2), the v3 de-meaned anti-ratchet, and the v3k erase split

2026-07-31, absorbing `docs/HANDOFF_iono2_card.md` (2026-07-17) on 2026-08-15 — that file is now
`docs/attic/HANDOFF_iono2_card.md`. §1–§5 + §3b are the v3/v3k record and keep their original numbering; the
absorbed mechanism-A design, the mechanism-B v2 harvest algebra and the as-built gate results append as
§6–§10. Successor to HANDOFF_iono2_card **§B** (now §8) after the seven-arm attribution matrix
(runs/iono2attr_*, all 3 h / 50 sta / GPS-triple / DOY-129 00:00–03:00) localized the field divergence to
mechanism B.

**Reading order** (numbering is append-only; position is not): §6 why the card exists → §7 mechanism A → §8
mechanism B v2 → §1/§2 the measured v3 problem and its degenerate signature → §3 the v3 fix → §3b the v3k
addendum → §4 as-built (v3/v3k), §9 as-built (A/B) → §5 gates (v3), §10 the decisive falsifier.

**Knob ledger** — all `estimation.global_vtec_sh.*`, all loader-default OFF (invariant #6):

| knob | default | inert when | fingerprint |
|---|---|---|---|
| `layer2_max_degree` | 0 = OFF | — | conditional `shl2=<deg>@<height_km>`, armed only |
| `layer2_height_km` / `layer2_kaula_scale_tecu` / `layer2_q_per_degree` | 1300 / 3 / 1e-2 | `layer2_max_degree = 0` | — |
| `layer2_mean_vtec_prior_tecu` / `_sigma_tecu` | 3 / 5 | `layer2_max_degree = 0` | — |
| `rep_error_tau_s` | 0.0 = OFF | — | — |
| `rep_error_demean` (v3) | false | `rep_error_tau_s = 0` | — |
| `rep_error_d5_keep` (v3k) | false = v2/v3 behaviour | `rep_error_tau_s = 0`; loader WARNs if armed without `rep_error_demean` | — |

⚠ The fingerprint token carries **no literal `km`** — the emitter is `add(fmt::format("shl2={}@{}",
shc.layer2_max_degree, shc.layer2_height_km))` at `NetworkRunContext.cpp:722`, so an armed deg-4 @ 1300 km run
writes `shl2=4@1300`. (This SPEC previously documented `shl2=<deg>@<H>km`; that string never existed.)

**Provenance**: every `runs/<name>` path here (`runs/iono2attr_*`, the `_wlver` bench outputs) resolves under
`D:\GnssProcessor_archive\2026-08-12\runs\`, not the working tree. These measurements cannot be re-derived
from this tree.

## 1. Problem — measured

With `rep_error_tau_s = 900` armed, the global SH-VTEC field diverges from ~4τ after cold start regardless of
slip detectors, elevation powers, or config posture:

| arm | mechanisms | a00 SUM peak | verdict |
|---|---|---|---|
| sliphard-only (no iono2)   | —   | 43 TECU @80 min  | clean |
| mech-A-only (dual layer)   | A   | 47 TECU @87 min  | clean |
| mech-B-only (dev carry)    | B   | 104 TECU @138 min | half-diverged |
| A+B (4 arms, both postures)| A+B | 207–255 TECU @130–136 min | diverged |

Mechanism B is the ratchet; mechanism A is clean alone but amplifies B ~2.2× (wider quasi-null cross-space).
The golden-posture cell additionally showed the divergence is invisible to every RAIM tier (coherent
field-state drift — consensus walks with it) and is caught only by the a00 physical-band gate.

## 2. Mechanism — the degenerate signature

The deviations are carried in VERTICAL TECU. A deviation component that is COMMON (in vertical TECU) across
LOS has the row signature M(el)·tecu_to_m — **identical to the a00 column's signature**. The pair (common
vertical deviation ↔ a00) is therefore exactly quasi-null in the row space; the data cannot allocate between
them. v2's scalar harvest is exact per LOS, but each harvest re-splits the un-allocatable common component
between d̂ and ĉ by prior weights alone (GM pull toward 0 at σ_ss = 5 vs the deliberately loose a00 prior 15 ±
30), and the harvest→age→re-assert alternation runs that split at loop gain > 1: the a00 walks, the deviations
absorb the complement, both grow. The per-(station,const) receiver-DCB lump rides the same degeneracy one
level down (§8 "level-lump note": v1/v2 deliberately did NOT de-mean — that choice is what v3 revokes).
Approximation (ii) in §8 (d̂↔ĉ cross-covariance compensated only by the GM leak) is the same defect stated in
covariance language.

## 3. Fix

### Increment 1 (THIS build) — contract-enforcing de-mean of the carried prior

The deviation state's contract is "per-LOS structure the SH surface cannot represent". A common vertical
component IS representable (it is a00); a per-station common component is the receiver lump. Enforce the
contract at the carry, once per epoch, in the serial PASS-B pre-pass (after slip-erase + GM aging, before
assembly):

1. over warm entries (t_s > 0), information weights w = 1/max(P, ε);
2. if n ≥ 30: subtract the weighted GLOBAL mean g from every dev (the a00-twin direction);
3. per station with ≥ 4 warm LOS: subtract the weighted station mean of the residual (the receiver-lump
   twin).

Variances are left UNCHANGED (conservative: the prior loses an estimated mean, keeps its spread). The
estimation still sees the common component in the DATA each epoch — the free a00/c absorbs it where it
belongs; the carry simply never re-asserts it as independent prior information. This removes the ratchet's
feedback path by construction rather than by damping.

Knob: `estimation.global_vtec_sh.rep_error_demean` (bool, default false — invariant #6). Inert without
`rep_error_tau_s > 0`. Thresholds 30/4 are constants in the seam (not knobs) — chosen ≫ the ~10 LOS/station
needed for a stable weighted mean at 50 stations; revisit only with data.

Protection stays the proven one: arm `sh_health_gate` in the Iono-2 lineage configs (the golden cell measured
engage → withhold 62 ep → release).

### Increment 2 (DEFERRED, pre-registered) — exact d̂↔ĉ cross-term carry

Card-1d-style: carry Cov(d̂, ĉ) per LOS and fold the joint prior. Heavier (the fold treats cross-correlated
priors — the known hazard class the WL fold got wrong), and unnecessary if increment 1 kills the measured
mode. Escalation trigger: mech-B-v3 3 h arm still shows a00 walk or carry-VCE calibration drifting from ~1.

### Rejected

- Tightening the a00 prior — ledger: must stay loose, data-determined
  (`network_global_sh_iono_design_2026-06-29.md` §6.2, "keep mean VTEC data-determined, do NOT clamp to an
  external mean").
- τ/degree tuning — structural failure, not a tuning miss.
- An a00-sum rate anchor — defense-in-depth deferred; gate + de-mean first.

## 4. As-built map

- `GnssConfigTypes.hpp` GlobalVtecShConfig: + `rep_error_demean`.
- `GnssConfigLoad.cpp`: parse next to `rep_error_tau_s`; the T5v2 banner gains the de-mean clause.
- `NetworkRunTypes.hpp`: pure `shDevDemean(carry, min_global, min_station)` → `ShDevDemeanStats {applied,
  n_used, g_tecu, n_sta}`.
- `ApplicationNetworkAssembly.cpp` PASS-B pre-pass: de-mean block after the aging loop; flag threaded
  explicitly (`sh_dev_demean`) per the existing `sh_dev_tau/sh_dev_on` pattern.
- `NetworkRunContext.hpp` counters: `shdev_demean_epochs_total`, `shdev_demean_sta_total`,
  `shdev_demean_abs_g_sum`.
- `NetworkTeardown.cpp` run-end line: de-mean clause appended ONLY when armed (OFF line byte-identical).
- Tests: suite `ShDevDemean` in `tests/test_shdev_carry.cpp`.

## 3b. ADDENDUM (same day) — v3k erase-policy split (`rep_error_d5_keep`)

v3's field validation exposed the next bottleneck: the C3c funnel erases the deviation on EVERY entry and the
D5 drain dominates it — mean deviation lifetime ≈ 0.7·τ (erase ~4.8 %/ep vs ~360 carried), so the accumulation
channel rarely matures. But 97 % of D5 re-taps coincide with NO raw observable step (slip-QC audit): they are
iono-representation tension — the exact signal the deviation exists to absorb.

Split the response by provenance, captured at insertion (PASS-A now fills a separate `carry_slipped_raw` set
which the driver unions into the funnel — every existing funnel consumer sees the identical union;
NetworkSlipQc only ever inserts, verified):

- RAW-confirmed slip (MW/LLI/IF-TD this epoch) → **ERASE**, unchanged (invariant #1: a slip aliases n·λ into
  the GF iono; the pre-slip deviation is poison).
- D5-ONLY re-tap → **KEEP** the level, re-tap the variance to σ_ss² (`shDevD5Retap`; idempotent under the
  aging step). A genuinely missed fine slip then enters as a weak prior and is re-estimated away within a few
  harvests instead of being carried stiffly; the v3 de-mean drains any common mode of kept levels, so the keep
  cannot re-open the ratchet. Loader WARNs if armed without `rep_error_demean`.

Expected: erases collapse to the raw rate, cold seeds/ep drop ~an order, carried population and warm-harvest
fraction rise, deviations finally mature past τ. Safety gate: a00 SUM must STAY in the clean band — this arm
is the first that lets levels survive the D5 storm, and the de-mean is the guard.

## 5. Gates

1. Unit: global weighted mean ≡ 0 after; station means ≡ 0 for qualifying stations; below-threshold epochs
   untouched; cold entries excluded and unmodified; variances unmodified; empty map safe.
2. Suite green Debug + Release.
3. OFF byte-identity: same-build mechBonly smoke, knob absent vs `rep_error_demean: false` — byte-identical
   artifacts.
4. ON smoke (10 ep, `_wlver_50sta_10ep_d10_gps_tri_iono2`): counters live (nonzero warm/cold; slip resets if
   events), |g| plausible (≲ a few TECU on a warm field), a00 sum physical, and LOS>3sigma read against the
   **0.73 OFF baseline** — under T5v2 it reads 0.78 BY DESIGN (§9's metric caveat), so 0.73 is the reference
   the by-design shift is measured from, not a target.
5. Field A/B (7 min/arm): `mechBonly_v3` vs today's `mechBonly` — PASS = a00 SUM stays in the clean band (≲
   50 TECU class) for 3 h with shdev warm-harvest volume comparable (the carry must still accumulate, not just
   die); then `combined_v3` (A+B+demean) to measure whether the de-mean also removes A's amplification
   channel.
6. Only after 5 passes: the actual D1 question (NL circR off the floor — the §10 criteria).

---

*Sections §6–§10 are absorbed from `docs/HANDOFF_iono2_card.md` (2026-07-17), the design record for mechanisms
A and B. They describe the same live code the v3/v3k sections amend.*

## 6. Why the card exists

The NL-AR iono-**representation** card, successor to T4/T5. The Jul-17 decisive A/B
(`_wlver_150sta_60ep_d10_gps_tri_sh15_nolane` vs 50 sta / deg-8): OSB tightened **3.5–5× with 3× stations + 3×
SH capacity while NL circR was UNMOVED and LOS>3σ stayed flat ~70 %** ⇒ the NL blocker is the iono
*representation* (single thin shell, per-epoch white pin), not observability. Combined with the Jul-15
structural finding — the producer is per-epoch, per-LOS slant iono is re-solved fresh every epoch, so
**`ionosphere_q` is a PHANTOM knob on this path** — NL needs what EWL/WL got from the MW/lane cards: a
**temporal accumulation channel**, here for the per-LOS ionosphere. Two mechanisms, both default-OFF,
independently armable, one card.

## 7. Mechanism A — dual-layer SH backbone (`layer2_max_degree > 0`)

Second thin shell (plasmasphere-side) with its own coefficient set, shell height, Kaula scale, RW q and a00
anchor. The pin row becomes

    iota  =  Sum_l  M_l(el) * tecu_to_L1m * SH_l(IPP_l) * c_l   (+ d)

with per-layer IPP geometry (different H ⇒ different mapping AND different pierce lat/lon). **Rationale**: the
two mapping profiles (450 vs 1300 km: M = 2.55 vs 1.74 at el = 10°, 1.60 vs 1.36 at 25°) plus IPP displacement
give the field elevation-dependent per-LOS structure a single shell cannot represent, and shrink the deviation
the GM carry (mechanism B) must absorb.

**Separability discipline** (the a00-split direction is quasi-null): layer-2 a00 gets a TIGHT climatology
anchor (`layer2_mean_vtec_prior_tecu` ± `layer2_mean_vtec_prior_sigma_tecu`, default 3 ± 5 TECU) while the
TOTAL stays data-determined through layer-1's loose 15 ± 30. The a00 physical-band WARN monitors the **LAYER
SUM**. Layer-2 Kaula default 3 TECU (smoother, smaller upper field).

**Config** (`estimation.global_vtec_sh.*`, all parsed in `GnssConfigLoad.cpp`): `layer2_max_degree` (int, 0 =
OFF), `layer2_height_km` (1300), `layer2_kaula_scale_tecu` (3), `layer2_q_per_degree` (1e-2),
`layer2_mean_vtec_prior_tecu` (3), `layer2_mean_vtec_prior_sigma_tecu` (5).

**Key schema and snapshot compatibility**: layer-1 coefficients keep `SH|n|m|C` unchanged; layer-2 uses the
DISTINCT prefix `SH2|n|m|C`, because `parseVtecShKey` hard-rejects extra fields — a 5-field key is NOT
backward-parseable, a distinct prefix is. Snapshot D-records smuggle the layer in the (empty-for-VtecSh) sat
field as `"L1"`, so old readers mismatch the tuple key and fall back to a climatology reseed — safe by
construction. The snapshot fingerprint gains the conditional `shl2` term **only when armed** (pre-existing
fingerprints unchanged).

**Touched sites** (Jul-17 surface map): StateKeySchema make/parse/is; NetworkEstimator ColumnDesc/SharedPrior
`sh_layer` + `registerVtecCoeff` / `vtecCoeffColumn` layer overloads + label `SH2:C:n:m`; per-epoch
registration (Assembly ~875); `networkInjectShVtecPrior` per-layer Kaula/q/anchor + 4-tuple carry key;
`networkStashCarry` `CarryPrior.sh_layer`; NetworkCoupling `couplingId V2|n|m|t` + per-layer `couplingQ`
(shmarg stays kind-based ⇒ covers both); row-builder layer loop; T5 harvest `sh_hat` layer sum; consumer
closed-loop supply layer sum + formal sigma over the concatenated basis; health sweep misfit vs SUM +
per-layer degree-RMS columns (armed-only, so the OFF artifact set is unchanged); `vtec_sh.csv` gains a `layer`
column armed-only; a00 WARN on the sum; `pl_warmup` latch stays layer-1 (low-degree convergence proxy).
RealTimeEngine's consumer is slant-value agnostic — untouched. **SH still has no on-disk product
serialization.**

## 8. Mechanism B v2 — the exact per-LOS deviation carry (`rep_error_tau_s > 0`, T5 v2)

**Why v1 was FALSIFIED as CIRCULAR (Jul-10)**: the T5 v1 scalar filter re-consumed the solved pin residual —
which its own RHS-shifted row had regularized toward `dev_pred` — as an independent floor-noise observation,
so P collapsed to floor² after one update and dev degenerated to a lag-1 copy. This is the exact-state
version. Schur elimination of an explicit GM state d from {pin row `iota − SH*c − d = eps(f²)`, prior `d ~
N(d_pred, P)`} reproduces the EXISTING assembly row `iota − SH*c = d_pred + eps(f² + P)` exactly — so the
assembly mechanism is **already correct and only the harvest changes**.

Exact scalar posterior per (station, sat), all in the epoch's slant frame (f = `iono_weighting.floor_m`,
effective f²/w_s under station deweight):

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

**Carried in VERTICAL TECU** (divide by M_1(el)·tecu_to_L1m at harvest, multiply at consumption): the
deviation and its GM steady state become elevation-stationary, fixing v1's slant-metre carry whose level was
never re-scaled as elevation changed. `sigma_ss = representation_sigma_tecu`, now a constant (no per-harvest
`var_ss` refresh). GM aging stays in the serial PASS-B pre-pass: `lam = exp(-dt/tau)`; `d *= lam`; `P = lam^2
P + (1-lam^2) sigma_ss^2`; **erase at dt > 5 tau**.

**Level-lump note** (v3 §2 cites this; it is the choice v3 revokes): `resid` contains the per-(rx,const)
receiver-DCB level (Odijk lump). It rides inside d **deliberately** — the common-mode-free R keeps the pin
rows level-blind, so the carried level is inert in the solve and is refreshed at every harvest; `V_hat`
correctly prices it because the level is GF-code observed at dm scale, not σ_c scale. **No de-meaning in v1.**

**Slip integration** (user spec Jul-12, built in from day one): the PASS-B serial pre-pass ERASES the
(station,sat) entry when any of the epoch's `carry_slipped_ambs` entries (per-signal keys; PASS-A MW/LLI/IF-TD
detection site + prior-epoch D5 innovation drain — the **C3c funnel**, immune to the pending-reseed clear)
projects to that satellite via `parseAmbiguityKey`. Reset-not-repair: cold re-tap at (0, σ_ss²). The set
reaches assembly by const ref (read-only projection; producers keep the non-const funnel contract). The
satellite-level within-epoch RAIM channel (`carry_we_drop_*`) is **NOT** merged — it is live only inside the
RAIM re-solve window and INERT under `carry_invariant`; revisit if RAIM activates.

**Three known v1/v2 approximations** (falsifiable via `carry_vce` / M2 LS-VCE calib): (i) per-LOS scalar
treatment ignores cross-LOS posterior correlation and common-mode row coupling (conservative direction for the
level); (ii) cross-epoch correlation between d and the border SH carry is compensated only by the GM leak
(1−λ²)σ_ss² — the Card-1d exact border-motion treatment is the escalation if calib shows incest; (iii) w_s ≤ 0
stations (full iono-deweight decouple) get **NO** harvest update (no pin row ⇒ no new info) — v1 updated them
anyway, a defect fixed in v2.

**The detector-degradation loop** (named per user spec): storm iono degrades the MW/GF detectors, so false
alarms cause excess dev resets exactly when accumulation matters most, and misses poison the carry. MW
thresholds stay empirical (MAD + Welford + EWMA); the D5 innovation gate on the carried DDs is the
estimation-side backstop; `shdev_slip_resets` is the observable. Confidence-gated updates are an option.

**Counters** (invariant #4 — run-end line gated on `sh_dev_on`): `shdev_warm` (warm-prior rows consumed),
`shdev_slip_resets`, `shdev_cold` (cold seeds), `shdev_nodata` (V_hat ≥ a skips).

## 9. AS-BUILT GATE RESULTS for mechanisms A and B (2026-07-21, all PASS)

- **Suite**: 827 ran / 826 pass / 1 skip, Debug AND Release (a 2026-07-21 snapshot), including the 6 new
  gates: layer key round-trip, dual-layer view + slant-sum, per-layer estimator registration/priors, and the 3
  `shDevExactUpdate` gates — **the non-collapse falsifier shows v1 collapsing to < 3 f² while v2 holds > 30
  f²** and reproduces the batch information sum exactly.
- **OFF byte-identity**: 13-arm decomp matrix vs the frozen goldens (`--maskspec
  goldens/decomp/maskspec.json`), renamed exe — **11/13 byte-identical**. The 2 fails (lane, uwl) are each
  EXACTLY the pre-existing `fold_blocked=false` WL-3 config-echo token from the LF-1 session's uncommitted
  loader change (`GnssConfigLoadNetwork` 381/2279, a file this card never touched) — **zero solve-artifact
  diffs**; those goldens need an LF-1-side refresh.
- **The FP-associativity lesson** (generalizable): the row builder's σ was regrouped `rep*(map*tecu)` vs the
  legacy `(rep*map)*tecu` — non-associative FP drifted the solve checksum at 1e-14 and the dd-frac/product
  last digits. Cured by restoring the literal product chain. The drift showed in the **CONSUMER arm ONLY**
  (em25 / deg-4 / Gal operand set) while allon/estim passed with the regrouped form — **passing SOME arms is
  not passing the gate.**
- **ON smoke (10 ep)**: 11 emitted / 0 withheld; counters live — 2791 warm, 425 cold, 127 slip resets (C3c
  funnel), 1669 no-info skips; a00 25.11 = 24.87 + 0.24 (split anchored). Harvest cost ~40 ms/ep at 50 sta =
  3.2 % net.
- **ON 60-ep vs OFF control** (same build, same grader, whole-run pooled): circR 0.016 vs 0.019, step med
  3.707 vs 3.742 cyc/ep, calib 0.45 vs 0.46 — **STATISTICAL PARITY, as expected**: τ = 900 s means the carry
  completes ~2 time constants in a 30-min run; no regression, 60/60 emitted, a00 in band. No-info harvest
  fraction fell **52 % (10 ep) → 11 % (60 ep)** = information flowing as the field converges. Layer-1 a00 tail
  10.4+/-8.9 (vs OFF 21.5+/-4.0) with layer-2 ~2.8: the deviations + second shell re-apportion structure the
  single-layer mean previously absorbed — watch the SUM on the long run (`grade_ddfrac`'s a00 column reads
  layer-1 only under dual-layer).
- **Metric caveat**: LOS>3σ reads **0.78 under T5v2 BY DESIGN** (against the 0.73 OFF baseline of §5 gate 3)
  — the health sweep standardizes the residual against the SH surface alone, and the tracked deviation IS that
  residual. It is no longer the misfit-suppression metric when the carry is armed.
- Mechanism-A/B unit gates as specified: layer key round-trip + old-key compat; two-layer registration
  distinct columns + per-layer prior routing; two-layer row synthesis == analytic sum at both IPPs; the
  exact-update pure function (extracted to `NetworkRunTypes`) — weak-data limit P+ → P_pred, strong-data limit
  P+ → P_pred·f²/a, NON-COLLAPSE vs the v1 recursion; slip projection helper (per-signal set → per-sat
  verdict, LLI single-band union).

## 10. The decisive multi-hour falsifier, and bench-file hygiene

The decisive falsifier is the **MULTI-HOUR arm (3 h+**, ideally the eq34 3 h lineage cadence): τ = 900 s
accumulation plus position/tropo convergence are preconditions the 60-ep bench **cannot** supply. **Success**
= per-signal NL circR rising above the 0.02–0.06 floor **+** DD step trending below the ~2 cyc/ep shared-param
floor **+** `carry_vce` calib staying ~1 (the v1-incest watchdog). **Tuning axes if partial**: τ (900 →
1800/3600 as arcs mature), layer2 degree (4 → 8), `representation_sigma_tecu`.

**Bench hygiene**: validation configs derive from `_wlver_50sta_{10,60}ep_d10_gps_tri_sh8.json` (diff: +
`rep_error_tau_s 900`, + the layer2 block deg-4 @ 1300 km). **Do NOT mutate the `_wlver` bench files.**

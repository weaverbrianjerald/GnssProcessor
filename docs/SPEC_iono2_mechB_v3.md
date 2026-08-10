# SPEC — Iono-2 mechanism-B v3: de-meaned deviation carry (the anti-ratchet)

2026-07-31.  Successor to `docs/HANDOFF_iono2_card.md` §B after the seven-arm
attribution matrix (runs/iono2attr_*, all 3 h / 50 sta / GPS-triple / DOY-129
00:00–03:00) localized the field divergence to mechanism B.

## 1. Problem — measured

With `rep_error_tau_s = 900` armed, the global SH-VTEC field diverges from
~4τ after cold start regardless of slip detectors, elevation powers, or
config posture:

| arm | mechanisms | a00 SUM peak | verdict |
|---|---|---|---|
| sliphard-only (no iono2)   | —   | 43 TECU @80 min  | clean |
| mech-A-only (dual layer)   | A   | 47 TECU @87 min  | clean |
| mech-B-only (dev carry)    | B   | 104 TECU @138 min | half-diverged |
| A+B (4 arms, both postures)| A+B | 207–255 TECU @130–136 min | diverged |

Mechanism B is the ratchet; mechanism A is clean alone but amplifies B ~2.2×
(wider quasi-null cross-space).  The golden-posture cell additionally showed
the divergence is invisible to every RAIM tier (coherent field-state drift —
consensus walks with it) and is caught only by the a00 physical-band gate.

## 2. Mechanism — the degenerate signature

The deviations are carried in VERTICAL TECU.  A component of the deviation
field that is COMMON (in vertical TECU) across LOS has the row signature
M(el)·tecu_to_m — **identical to the a00 column's signature**.  The pair
(common vertical deviation ↔ a00) is therefore exactly quasi-null in the row
space; the data cannot allocate between them.  v2's scalar harvest is exact
per LOS, but each harvest re-splits the un-allocatable common component
between d̂ and ĉ by prior weights alone (GM pull toward 0 at σ_ss = 5 vs the
deliberately loose a00 prior 15 ± 30), and the harvest→age→re-assert
alternation runs that split at loop gain > 1: the a00 walks, the deviations
absorb the complement, both grow.  The per-(station,const) receiver-DCB
lump rides the same degeneracy one level down (handoff §B "level-lump note":
v1/v2 deliberately did NOT de-mean — that choice is what v3 revokes).
Approximation (ii) of the handoff (d̂↔ĉ cross-covariance compensated only by
the GM leak) is the same defect stated in covariance language.

## 3. Fix

### Increment 1 (THIS build) — contract-enforcing de-mean of the carried prior

The deviation state's contract is "per-LOS structure the SH surface cannot
represent".  A common vertical component IS representable (it is a00); a
per-station common component is the receiver lump.  Enforce the contract at
the carry, once per epoch, in the serial PASS-B pre-pass (after slip-erase +
GM aging, before assembly):

1. over warm entries (t_s > 0), information weights w = 1/max(P, ε);
2. if n ≥ 30: subtract the weighted GLOBAL mean g from every dev
   (the a00-twin direction);
3. per station with ≥ 4 warm LOS: subtract the weighted station mean of the
   residual (the receiver-lump twin).

Variances are left UNCHANGED (conservative: the prior loses an estimated
mean, keeps its spread).  The estimation still sees the common component in
the DATA each epoch — the free a00/c absorbs it where it belongs; the carry
simply never re-asserts it as independent prior information.  This removes
the ratchet's feedback path by construction rather than by damping.

Knob: `estimation.global_vtec_sh.rep_error_demean` (bool, default false —
invariant #6).  Inert without `rep_error_tau_s > 0`.  Thresholds 30/4 are
constants in the seam (not knobs) — chosen ≫ the ~10 LOS/station needed for
a stable weighted mean at 50 stations; revisit only with data.

Protection stays the proven one: arm `sh_health_gate` in the Iono-2 lineage
configs (the golden cell measured engage → withhold 62 ep → release).

### Increment 2 (DEFERRED, pre-registered) — exact d̂↔ĉ cross-term carry

Card-1d-style: carry Cov(d̂, ĉ) per LOS and fold the joint prior.  Heavier
(the fold treats cross-correlated priors — the known hazard class the WL
fold got wrong), and unnecessary if increment 1 kills the measured mode.
Escalation trigger: mech-B-v3 3 h arm still shows a00 walk or carry-VCE
calibration drifting from ~1.

### Rejected

- Tightening the a00 prior (ledger: must stay loose — data-determined).
- τ/degree tuning (structural failure, not a tuning miss).
- An a00-sum rate anchor (defense-in-depth deferred; gate + de-mean first).

## 4. As-built map

- `GnssConfigTypes.hpp` GlobalVtecShConfig: + `rep_error_demean`.
- `GnssConfigLoad.cpp`: parse next to `rep_error_tau_s`; T5v2 banner gains
  the de-mean clause.
- `NetworkRunTypes.hpp`: pure `shDevDemean(carry, min_global, min_station)`
  → `ShDevDemeanStats {applied, n_used, g_tecu, n_sta}`.
- `ApplicationNetworkAssembly.cpp` PASS-B pre-pass: de-mean block after the
  aging loop; flag threaded explicitly (`sh_dev_demean`) per the existing
  `sh_dev_tau/sh_dev_on` pattern.
- `NetworkRunContext.hpp` counters: `shdev_demean_epochs_total`,
  `shdev_demean_sta_total`, `shdev_demean_abs_g_sum`.
- `NetworkTeardown.cpp` run-end line: de-mean clause appended ONLY when armed
  (OFF line byte-identical).
- Tests: suite `ShDevDemean` in `tests/test_shdev_carry.cpp`.

## 3b. ADDENDUM (same day) — v3k erase-policy split (`rep_error_d5_keep`)

v3's field validation exposed the next bottleneck: the C3c funnel erases the
deviation on EVERY entry, and the D5 drain dominates it — mean deviation
lifetime ≈ 0.7·τ (erase ~4.8 %/ep vs ~360 carried), so the accumulation
channel rarely matures.  But 97 % of D5 re-taps coincide with NO raw
observable step (slip-QC audit): they are iono-representation tension —
the exact signal the deviation exists to absorb.

Split the response by provenance, captured at insertion (PASS-A now fills a
separate `carry_slipped_raw` set which the driver unions into the funnel —
every existing funnel consumer sees the identical union; NetworkSlipQc only
ever inserts, verified):

- RAW-confirmed slip (MW/LLI/IF-TD this epoch) → ERASE, unchanged
  (invariant #1: a slip aliases n·λ into the GF iono; the pre-slip deviation
  is poison).
- D5-ONLY re-tap → KEEP the level, re-tap the variance to σ_ss²
  (`shDevD5Retap`; idempotent under the aging step).  A genuinely missed
  fine slip then enters as a weak prior and is re-estimated away within a
  few harvests instead of being carried stiffly; the v3 de-mean drains any
  common mode of kept levels, so the keep cannot re-open the ratchet.
  Loader WARNs if armed without `rep_error_demean`.

Expected: erases collapse to the raw rate, cold seeds/ep drop ~an order,
carried population and warm-harvest fraction rise, deviations finally mature
past τ.  Safety gate: a00 SUM must STAY in the clean band — this arm is the
first that lets levels survive the D5 storm, and the de-mean is the guard.

## 5. Gates

1. Unit: global weighted mean ≡ 0 after; station means ≡ 0 for qualifying
   stations; below-threshold epochs untouched; cold entries excluded and
   unmodified; variances unmodified; empty map safe.
2. Suite green Debug + Release.
3. OFF byte-identity: same-build mechBonly smoke, knob absent vs
   `rep_error_demean: false` — byte-identical artifacts.
4. ON smoke: counters live, |g| plausible (≲ a few TECU on a warm field).
5. Field A/B (7 min/arm): `mechBonly_v3` vs today's `mechBonly` —
   PASS = a00 SUM stays in the clean band (≲ 50 TECU class) for 3 h with
   shdev warm-harvest volume comparable (the carry must still accumulate,
   not just die); then `combined_v3` (A+B+demean) to measure whether the
   de-mean also removes A's amplification channel.
6. Only after 5 passes: the actual D1 question (NL circR off the floor —
   handoff §NEXT criteria unchanged).

# Cycle-Slip Detection & Repair Correctness Audit — 2026-06-10

Multi-agent audit (8 dimension finders, 3-lens adversarial verification, ~123 agents) of the
TurboEdit MW+GF slip machinery and every layer it touches: `TurboEditResolver`,
`CycleSlipManager`, `MwSlipDetector`, aux detectors, `StateManager`/`KalmanFilter` mutation
primitives, BSSD pivot machinery, `AmbiguityResolver`, RTS `Smoother`, `RealTimeEngine`,
and the RTCM/SBF ingestion that feeds the RT path. Findings below were verified by direct
code reading (file:line cited); the one marked ✓✓✓ also passed full 3-lens fleet verification.

Severity: **critical** = wrong-integer state corruption at converged covariance in realistic
operation; **major** = corruption or convergence destruction in plausible scenarios;
**minor** = robustness/latent.

---

## Status update — fixes applied 2026-06-10 (same day)

| Finding | Status |
|---|---|
| A1 equal-slip vs locked-arc absorb | **FIXED** — GF-only split now gated on affirmative per-epoch MW cleanliness (`MwSlipDetector::getEpochCleanPairs`, |dev| ≤ 0.3 λ_WL on a mature arc); unaffirmed GF-only flags fall back to reset. New `resolve()` parameter wired into both consumers. |
| A2 no k+1 confirmation | **Substantially mitigated by A1**: a single-signal phase excursion moves MW too (ΔN_WL ≠ 0) → no clean affirmation → reset, not repair. Residual exposure: equal-magnitude excursions on both phases. Full k+1 confirmation still recommended. |
| B1 LLI override | **FIXED** — LLI sats skip both culprit paths and take the full reset (now incl. the reinflation block); matches the RT engine's LLI-authoritative policy. |
| B2 dropped non-culprit keys | **FIXED** — residual mandated resets for keys not adjudicated by the culprit analysis (cross-consistency adjudicates all analyzed pair signals; TE adjudicates culprits + `clean_signals` partners). |
| B3 LLI never clears TE GF arcs | **FIXED** in both consumers (`turbo_/turbo_edit_resolver_->resetSatellite` on LLI; MW EWMA deliberately preserved per its design). |
| C1 combined-IF swallow | **Structurally closed by B2** — the un-adjudicated IF amb_key now takes the residual reset (pre-TurboEdit behavior restored). Per-raw-signal repairs remain inert no-ops in IF mode; a mode-aware culprit→IF-key mapping is still the better long-term fix. |
| E1 RT 120 s gap tolerance | **FIXED** — `setMaxGapSeconds(1.5 × max(gnss_rate_s, 1.0))` at construction; post-outage arcs now restart instead of bridging TD2. |
| E2 RT arc clearing after KF resets | **FIXED** — rebuild branch now clears MW + TE arcs per reset signal/sat (and on complete reset), mirroring `Application::runFilterUpdateLoop`. |

All builds clean (gnss_core + all 20 targets). Numerical validation pending a replay run
(suggest `config/sbf_replay_covl145.json`; `turbo_edit_trace.csv`'s `wl_known`/`action`
columns now show the A1 gate decisions directly).

Known behavior trade-off from A1: true dN1==dN2 blind-spot slips on **low-elevation** arcs
(MW sample noise above 0.3 λ_WL) now reset instead of repair — conservative by policy
(reset over wrong repair).

Still open: A3 (intra-E5 |denom| floor), A5/A6, D1/D2 (reinflation on TE/aux/RT-slip reset
paths), E3 (decoder lock-time), E4, F1 (smoother slip-awareness), G1–G4, H1–H9.

---

## A. The GF-only "equal-slip" assumption is unsafe (resolver core)

### A1. [critical] Locked-arc absorb breaks the "MW silent ⇒ ΔN_WL=0" premise
`TurboEditResolver.cpp:325-328` treats the absence of an MW flag as ΔN_WL=0. But
`MwSlipDetector.cpp:1096-1126` deliberately emits **no** `MwSlipResult` when a **wl_locked**
arc takes a threshold exceedance — the sample is absorbed into Welford and drift-demotion
fires only ~0.4·eff_n epochs later (eff_n ≤ 100). Locked arcs are the steady state in
BIA-corrected PPP-RTK. So any unequal slip on a mature arc is MW-silent at the slip epoch
while GF flags it, and the joint solve runs with ΔN_WL forced to 0:
`dN2_f = dN2 − w·λ1/(λ1−λ2) = dN2 − 3.5295·w` (GPS L1/L2).
- w = ±2, ±4, ±6 → frac 0.059 / 0.118 / 0.177 ≤ 0.30 → **confident wrong repair on both
  signals** (e.g. true (2,0) → applied (−7,−7), both ambiguities off ~1.7 m at converged covariance).
- w odd → frac ~0.47/0.41 → whole-sat reset (precision lost, but safe).

**Fix direction:** absence-of-flag is not evidence. Before trusting ΔN_WL=0, require
*affirmative* MW cleanliness for the pair this epoch — e.g. MwSlipDetector exposes the
per-arc |dev| and lock state; gate the GF-only split on `wl_locked && |dev| < ~0.3·λ_WL`,
else emit reset. (The detector already computes both quantities.)

### A2. [major] No consecutive-epoch confirmation — isolated GF outliers become permanent repairs
`TurboEditResolver.cpp:347` repairs in the same epoch the flag fires; `did_slip` then clears
the arc (`:392-395`), destroying the evidence that GF returned to the pre-event level at k+1.
Since dN2=0 would require |TD2| ≤ 0.3·|denom| ≈ 1.6 cm < the 4 cm floor, every GF-only
exceedance resolves to a nonzero integer (or reset). A one-epoch 6 cm multipath/tracking
excursion that lands near an integer (~60 % of arbitrary magnitudes) becomes a (±1,±1)
repair; the KF then claws it back via outlier resets. Classic TurboEdit defers the decision
one epoch (slip persists, outlier recovers). At 1 Hz the latency cost of a k+1 confirmation
is one second — cheap insurance. Note the repair-then-revert cost is bounded (KF backstop),
but it converts transients the policy says to deweight into state mutations.

### A3. [major] Near-degenerate intra-E5 pairs: the 1e-6 m denominator guard is ~3 orders too small
`collectPhasePairs` (`TurboEditResolver.cpp:95-124`) forms **all** distinct-frequency pairs,
including Galileo L7/L5 (|λ1−λ2| = 6.5 mm), L8/L5 (3.3 mm), L7/L8 (3.2 mm) and the BeiDou
B2 equivalents. The only conditioning gate is `|denom| < 1e-6` (`:334`). GF TD2 noise of
5–10 mm amplifies ×150–310 into dN2_f → the rounded integer is noise and passes frac ≤ 0.30
~60 % of the time. Their EWL MW (λ_WL 9.8–19.5 m) flags readily, feeding the garbage solve;
the wrong candidate then conflicts with the well-conditioned L1/L5 resolution →
`conflict_sats` → **whole-sat reset** (every multi-E5 slip event), or, with E1 shadowed,
wrong integers applied outright.
**Fix direction:** impose a |denom| floor (~2 cm), or weight candidate reconciliation by
conditioning and prefer the best-conditioned pair instead of demanding unanimity.

### A4. [major] Half-cycle slips alias through the frac gate
A 0.5-cycle slip on L2 alone → dN2_f = 2.265 → frac 0.265 → accepted as (2,2);
on L1 alone → (−2,−2) at frac 0.235. No half-cycle hypothesis test exists. Exposure depends
on the receiver surfacing half-cycle events (see E3 — in RT today neither decoder provides
usable lock/half-cycle telemetry, so nothing protects this path).

### A5. [minor] IVI defaults to "stable" on insufficient evidence; `rate_ok` is advisory only
`computeIvi` returns 0.0 for < 3 satellites per constellation (`TurboEditResolver.cpp:164-165`)
— the failure direction is wrong (insufficient evidence should disable the split, not
green-light it; lone QZSS/BDS sats). `out.rate_ok` is set (`:186`) but grep confirms **no
consumer** in either caller — at 30 s sampling the pipeline runs warn-only.

### A6. [minor] Immature-window MAD contamination
`TurboEditResolver.cpp:399`: TD2 outliers are kept out of the MAD window only when
`gf_exceeds` is true; while the threshold is still NaN (count < 5+2) slip-sized TD2 values
enter the window and inflate the threshold for up to `kGfWindowSize`=30 epochs.

---

## B. LLI arbitration in the post-processing manager

### B1. [critical] TurboEdit culprits override receiver LLI resets; RT path disagrees
On an LLI epoch `MwSlipDetector.cpp:1505-1528` pushes signal-less resets and `continue`s
(no MW pair evidence). TE skips signal-less entries (`TurboEditResolver.cpp:210`) but its GF
arcs roll across the loss-of-lock and resolve the jump GF-only under ΔN_WL=0. In the per-sat
loop, `CycleSlipManager.cpp:680-689` adopts the TE culprits and `:744-745`
(`reset_sats.insert(sat_id); continue;`) **skips the full-reset loop over `amb_keys`** — the
only consumer of the LLI reset keys. A re-acquisition slip with even ΔN_WL gets a confident
wrong equal-slip repair *instead of* the receiver-mandated reset (w odd / non-integer →
TE puts the sat in `reset_sats`, culprits stay empty, full reset correctly fires).
`RealTimeEngine.cpp:725-737` implements the opposite, documented policy
(`if (!is_lli && turbo_handled_sats.count(sat)) continue;` — LLI always resets).
**The PP path is the non-conforming one.** Fix: exclude LLI-flagged satellites from the TE
culprit fallback (or run the amb_keys reset for keys not covered by culprit repairs — see B2).

### B2. [major] Culprit branch silently drops *all* non-culprit reset keys
Same `continue` at `CycleSlipManager.cpp:744-745`: when culprit resolution succeeds (either
path), any `amb_keys` member whose signal is not a culprit — an L5 LLI key, a pair whose GF
arc was in warm-up — is neither repaired nor reset. Fix: after culprit handling, reset the
residual keys instead of skipping the loop.

### B3. [major] LLI / lock-loss never clears TE GF arc history (both consumers)
`CycleSlipManager::resetSatellite/-Signal` do clear TE arcs (`:76-90`) but are invoked only
from the KF-outlier path (`Application.cpp:777-794`). The LLI full-reset path never calls
them; RT has **zero** resetSatellite calls (grep). A sub-threshold-at-t LLI slip stays baked
in `prev_gf`; at t+1 MW flags with the true ΔN_WL (its design contract,
`MwSlipDetector.cpp:1516-1518`) and TE pairs it with the **echo** TD2 (sign-flipped) →
plausible wrong repair on a freshly reseeded ambiguity.

---

## C. Mode-coverage holes (per-band key convention assumed everywhere)

### C1. [critical when configured] Combined-IF PPP + turbo_edit: slips swallowed entirely
IF-mode state keys are `G01_L1C_L2W`; TE culprits are raw (`L2W`). In the culprit branch
`repairAmbiguityBssdAware` fails (`getAmbiguityIndex<0`, `CycleSlipManager.cpp:277-281`),
the fallback reset targets the same nonexistent raw key and `StateManager::resetAmbiguity`
refuses unknown keys (`StateManager.cpp:976-979`), then `:744-745` skips the full reset of
the real IF `amb_keys`. **Net: zero state mutation for an MW-confirmed slip** — a regression
vs `turbo_edit=false`, where the full reset fires. The GF-only pass has the same hole
(`:1313` `continue`, no fallback). RT is structurally immune (its `idx<0 → continue` skips
the handled-sat insert, so the default reset of the IF amb_key still runs). The KF post-fit
backstop still works in IF mode (its keys are IF-labeled), so the slip is eventually caught,
late. Fix: gate TE culprit adoption on uncombined mode, or map per-signal culprits onto the
IF key (any culprit ⇒ reset the IF ambiguity).

### C2. [major] nl_wl ambiguity basis: no slip path can touch the state
Under `estimation.state_ambiguity_basis = nl_wl` (live, validated option,
`GnssConfigLoad.cpp:832-844`) state keys are `G01_NL_1` / `G01_WL_1_2`, but every consumer
builds `sat + "_" + signal`: CycleSlipManager, RealTimeEngine, and the KF backstop
`handlePhaseOutlier` (`KalmanFilter.cpp:1188`, warns "ambiguity key not found"). Grep
confirms zero `makeNl/makeWl` usage under `src/core/slip`. Under that basis **no mechanism
in the codebase resets or repairs an NL/WL ambiguity after any slip** — a blocker for the
standard-PPP AR initiative that basis serves. A correct repair also needs the basis
transform (dNL on the reference band; dWL = ±(dN1−dN2)).

---

## D. Reset state-hygiene (the fleet's ✓✓✓-confirmed finding family)

### D1. [major ✓✓✓] TE/aux full-sat resets skip the iono/sat-bias reinflation guard
The MW full-reset path reinflates per-sat iono and sat-bias variances with a documented
rationale (`CycleSlipManager.cpp:763-804`). The same all-ambiguities outcome via
`applyTurboEditGfOnlyResolutions` (`:1337-1361`) and the EWL BandDifference path
(`:1274-1278`) performs **no** reinflation — and the TE reset trigger (`!iono_stable_cons`)
correlates with disturbed ionosphere, exactly when the stale partition is most wrong.
Same class as the known `handleCodeOutlier` scrub gap.

### D2. [major] RT pre-update slip resets skip reinflation entirely — and aren't BSSD-aware
`RealTimeEngine.cpp:708-738`: TE reset_sats and the default MW/LLI loop use plain
`state_manager_->resetAmbiguity` (no SD-aware reseed, no reinflation).
`reinflateResetSatStates` (`:818`) keys off `kalman_filter_->getLastResetSatellites()`,
which pre-update resets never populate.

---

## E. Real-time parity

### E1. [major] RT never tightens the 120 s GF-arc gap tolerance
PP: `CycleSlipManager::setSampleInterval` → `turbo_->setMaxGapSeconds(1.5×cadence)`
(`CycleSlipManager.cpp:111-118`). RT: grep confirms **no** `setMaxGapSeconds` /
`setSampleInterval` call in `RealTimeEngine.cpp`. After a 2–120 s stream outage every arc
bridges the gap (`skipped_prev_epoch` is false because `prev_epoch_s_` stalls with all
arcs), TD2 mixes a g-second leg with 1 s legs → mass GF flags at reconnect → IVI-gated mass
reset (≥3 sats) or aliased false repairs (<3 sats). Also slips *during* the outage are
solved against gap-contaminated TD2. One-line fix at construction plus per-epoch dt sanity.

### E2. [major] RT never clears MW/TE arc state after KF-outlier resets
`MwSlipDetector.hpp:200-203` documents the contract; `Application.cpp:777-794` honors it;
grep shows the RT engine never calls `resetSatellite`/`resetSatelliteSignal` on either the
MW detector or the resolver. After a KF reset of a missed slip, the stale MW EWMA / GF arc
flags next epoch and TE can apply a wrong repair on the freshly reseeded state.

### E3. [major] Receiver lock-time dropped by both RT decoders
`RtcmMsmDecoder.cpp:380/387` decodes lock-time, never compares it (LLI gets only the
half-cycle bit); the SBF MeasEpoch LockTime field is skipped. RT therefore has **no true
loss-of-lock signal**: the "LLI authoritative" RT policy protects nothing live, and TE can
repair across receiver-declared re-acquisitions. (This also removes the only mitigation for
A4 half-cycles in RT.)

### E4. [minor] RT ignores `estimation.cycle_slip.mw`; runs no aux detectors
`RealTimeEngine.cpp:127` constructs MwSlipDetector unconditionally; TE is gated only on
`turbo_edit` (PP refuses TE without MW, `CycleSlipManager.cpp:50-61`). `{mw:false,
turbo_edit:true}` → PP runs neither, RT runs both.

---

## F. RTS smoother

### F1. [major ✓1/1] Smoother is blind to all slip-phase repairs and resets
`Application.cpp:1652` captures the prior **before** `detectCycleSlips` (`:1662`);
`epoch_reset_sats` (`:1667`) is filled only from KF-internal resets (`:762/779/795` +
relinearization merge). `CycleSlipEpochResult` carries only counters, so the slip phase
*cannot* forward identities. In the backward pass
(`x_s(k)=x_post(k)+G(x_s(k+1)−x_pred(k+1))`), a covariance-preserved repair's dN·λ appears
as a near-unity-gain innovation → smoothed pre-slip ambiguities shift by ≈dN·λ, leaking into
iono/tropo (and position in kinematic runs; static smoothed position is constant by
construction) across the entire preceding segment.
**Fix direction:** extend `CycleSlipEpochResult` to carry per-key actions; in
`capturePosteriorSnapshot` (a) add slip-reset sats to `snap.reset_satellites` (segment
break), (b) for repairs, **compensate the stored prior**: `x_pred(amb_idx) += dN·λ` — a
known deterministic jump needs prior compensation, not segmentation.

---

## G. MW ↔ AR interplay

### G1. [major] MW-pivot ZD slip leaves locked peers' WL integers stale → AR drags repaired states wrong
SD arcs are referenced to the MW-internal pivot (`MwSlipDetector.cpp:1876`). A pivot ZD slip
shifts every peer's SD sample by −dN_p; **locked** peers absorb it (A1 branch) and keep
their pre-slip `rounded_wl`, which `getResolvedWlAmbiguities` (`:1945-1950`) exposes (pivot
contributes 0 by construction, `:1960-1966`). `resolveWlOnly` then applies stale DD WL
integers as pseudo-observations (`AmbiguityResolver.cpp:1457, 1516-1533`) for ~0.4·eff_n
epochs until drift demotion. The KF float repair is correct; AR walks it back off. Fix: on a
pivot ZD slip with known integer, shift locked peers' Hatch mean/rounded_wl by −dN_p (the
arc-state analog of the in-place SD repair), or suppress `getResolvedWl*` for the group
until re-lock.

### G2. [minor] `resolveWlOnly` 100 m² inflation fires even when a repair already aligned the float
`AmbiguityResolver.cpp:1479-1506`: WL-integer change → unconditional row/col zero +
100 m² on both SD bands, no innovation-magnitude gate. After a covariance-preserving repair,
the re-lock at the new integer has v≈0 — the inflation throws away exactly the precision the
repair preserved. Gate on |v| > ~0.3 cycles.

### G3. [minor] MW pivot slip mass-resets unlocked clean peers
Re-referenced `dN=0, wl_known=true` results (`MwSlipDetector.cpp:1889`) are emitted for
clean unlocked peers and treated as actionable by both consumers (no `dN==0 && known` skip);
TE resolves (0,0) → "none" → no culprit → **full reset** of every unlocked peer in the group.

### G4. [minor] Unbounded MW jump integer
`MwSlipDetector.cpp:1136-1140`: no upper bound on `|dev_cyc|`; out-of-int-range cast is UB.
`computeCulpritSlipIntegers` → `repairAmbiguityBssdAware` apply it uncapped (TE's own path
is protected by `kMaxSlipCycles`). Add the same cap here.

---

## H. Other paths

### H1. [major when enabled] GraphicSlipDetector tests raw |dg| against a spread threshold
`GraphicSlipDetector.cpp:215-218`: `dg = graphic − prev_graphic` contains full range-rate +
receiver clock drift (hundreds of m at 1 s); the MAD threshold measures the *spread* of the
window, so `|dg| > thresh` fires continuously once armed. Should test the deviation from the
window median (or difference a geometry-corrected GRAPHIC). As-is, enabling
`estimation.cycle_slip.graphic` prevents convergence.

### H2. [major when used] Baseline mode: LLI results carry an empty satellite field → resets no-op
`ApplicationBaseline.cpp:565-567` inserts `r.satellite` directly (LLI entries default it to
`{}` via `pushReset`), so `eraseAmbiguitiesForSat("")` matches nothing. PP/RT guard with
`satIdFromAmbiguityKey` fallback; baseline doesn't. Equal-slip re-acquisitions then never
get caught (no TE in baseline).

### H3. [minor] GLONASS FDMA details
(a) In-place pivot repair applies `(dN_peer − dN_pivot)` and `−dN_pivot` shifts with the
**peer's** λ (`CycleSlipManager.cpp:907, 975` → `:296-297`); exact for CDMA, wrong by
dN_p·(λ_piv−λ_peer) for FDMA (gated on `enable_glonass_bssd`).
(b) Slip detectors' own `PppUtils` instances never receive the RINEX SLOT/FRQ map — only
`PppModel.cpp:247` calls `setGlonassSlots`. Lookup failure (−999) silently forms zero
MW/GF pairs: a GLONASS sat the KF estimates can have no slip protection beyond LLI.

### H4. [minor] Inconclusive culprit identification treated as "confidently clean"
`CycleSlipManager.cpp:856-860`: `identifySlippedSignals` returns `{}` when inconclusive;
`.count(sig)==0` then classifies the sat as clean and the clean-peer pass shifts its
just-reseeded ambiguity by −dN_pivot (double handling; bounded by reset variance).

### H5. [minor] Cross-consistency clean-pair premise vs equal slips; reset_sats gating skips TE's correct repairs
A "clean" MW pair can hide a dN1==dN2 slip (`CycleSlipManager.cpp:405-409`): triple-freq
case attributes the wrong integer to the third signal and the per-sat `reset_sats` entry
(`:744`) blocks the GF-only pass (`:1309/:1338`) where TE held the correct equal-slip
resolution.

### H6. [minor] Aux-detector arbitration is satellite-granular
`CycleSlipManager.cpp:1260`: a per-signal MW repair suppresses aux events on *other*
signals of the same satellite for the epoch.

### H7. [minor] No observable-domain receiver clock-jump repair
Symmetric ms steps cancel in MW/GF (verified); code-only/phase-only steps mass-flag MW on
all satellites → full constellation reset per jump event on receivers that step pseudorange
to bound their clock. The 0.5 ms detector (`Application.cpp:344`) only re-seeds the KF clock.

### H8. [info] Traces blind to TE GF-only events; RT trace passes an empty slip set
`Application.cpp:1236-1245` derives slip flags from `getLastMwSlips()` only;
`RealTimeEngine.cpp:889-899` hardcodes `slipped_sats={}` with a stale comment. Mid-arc
ambiguity jumps from TE repairs appear in traces as unexplained — costly given trace
inspection is the project's stated verification method.

### H9. [info] No AR-ability gate on integer repairs
Policy check (design intent: reset-over-repair for bias-less signals): GLONASS/bias-less
ambiguities receive covariance-preserving repairs. Numerically sound — slip integers are
time-differences, bias-free — but worth an explicit policy decision; the
`isAmbiguityUnresolvable` set exists and no repair site consults it.

---

## Verified clean (worth recording)

- The **sequential WL-then-GF solve** (ΔN2 = (ΔΦ_GF − λ1·ΔN_WL)/(λ1−λ2)) is the correct
  formulation — it avoids the naive 2×2 conditioning trap; sign conventions
  (ΔN_WL = ΔN1−ΔN2, GF = λ1N1−λ2N2 in meters, repair applied as x += dN·λ) are consistent
  end-to-end across resolver → manager → StateManager and the RT consumer.
- `computeCulpritSlipIntegers` sign handling (sig2 lone culprit ⇒ −ΔN_WL) is correct, with
  cross-pair consistency disqualification.
- The (0,0) "resolved to no slip" veto correctly leaves arcs rolling on MW false alarms.
- Per-sat candidate reconciliation (conflict ⇒ whole-sat reset) correctly catches
  cross-pair disagreements when both pairs produce candidates the same epoch.
- The dual-trigger arc restart (absolute gap + skipped-previous-epoch) is rate-agnostic and
  correct for per-signal dropouts (the global-outage case is E1, an RT config gap, not a
  resolver logic flaw).
- MW flags during GF warm-up correctly degrade to full reset in PP uncombined mode (the
  manager's fall-through), and to plain resets in RT — not swallowed (except C1).
- `mw_by_pair` dedup of the two-per-pair uncombined entries; `satOf` fallback parsing;
  `getActiveArcKeys` maturity gating for cross-consistency clean references.

## Coverage gaps (not yet audited)

~~Two finder dimensions died on a session usage limit~~ — **closed by Pass 2 below.**

Verification status (Pass 1): 1 finding fleet-confirmed 3/3, 1 at 1/1; all other findings
above were hand-verified against the cited code during synthesis. None of the fleet's
findings were refuted by verification.

---

# PASS 2 — completion run (same day, against the fixed tree)

The two missing dimensions ran against the working tree including the Pass-1 fixes
(F1–F4), with those fixes explicitly in scope for adversarial review. 36 agents, full
3-lens verification on every finding: **11 confirmed, 0 refuted.** Raw results with all
verdicts: `docs/slip_repair_audit_2026-06-10_pass2_results.json`.
The completeness critic additionally built the full tree and ran `gnss_tests` (189/190,
1 fixture skip) — note **the "no test suite" known-state in CLAUDE.md is stale**: 190
registered tests across 45 suites exist, none covering the slip chain.

## Pass-2 confirmed findings

### P2-1. [critical→major(calibrated), 3/3] RTS smoother blind to routine BSSD pivot handovers
`capturePriorSnapshot` (Application.cpp:1676) precedes the update loop in which the
ROUTINE first-pass handover re-references every SD ambiguity/iono/sat-bias of a
constellation in place (MeasurementModel preparePivots → PivotManager:391/430/480), under
unchanged keys. `x_s(k+1)` is then in the NEW frame while the stored `x_pred(k+1)` is in
the OLD frame: the backward pass injects a meter-scale `−x(new_pivot_SD)` pseudo-innovation
through near-unity ambiguity gain into the entire preceding segment, in fully nominal
operation (no slip required). Strictly bigger trigger than F1's slip-phase blindness.
**Fix direction:** store the per-epoch handover transform (or constellation re-reference
offsets) in the snapshot and either compensate `x_pred`/`P_pred` or segment the affected
constellation's states. Fold into the same snapshot rework as F1.

### P2-2. [major, 3/3] Mixed-frame BSSD reseeds before a same-epoch pivot handover
`resetAmbiguityBssdAware`'s observation seed (`seed(sat) − seed(pivot)`, CSM:221) is
post-slip-frame; if a forced handover then re-references the constellation, the
already-correct fresh seed gets transformed again — off by `−dN_pivot·λ`.

### P2-3. [major, 3/3 → PATCHED] Forced-handover exclude set blind to LLI / TE-flagged sats
The exclude set was built from `slips_by_sat` only (MW pair slips); LLI sats (signal-less
entries) and TurboEdit GF-only culprits/reset sats could be elected as the new SD anchor —
a just-reseeded or mid-slip satellite re-referencing the whole constellation
(`scorePivotCandidate`'s variance penalty saturates at −1.0 and `handleDisturbedPivot`
applies no elevation gate, so election is reachable).
**Patched same day:** `lli_sats`, `te_culprits_by_sat` keys, and `te.reset_sats` are now
unioned into `exclude_sats`.

### P2-4. [major, 3/3 → PATCHED] Clean-peer pass shifted just-reset LLI satellites
`confidentlyCleanOnSignal` treated an LLI sat (no pair slips, mature preserved arcs) as
"confidently clean", handing its freshly obs-reseeded ambiguity an additional `−dN_pivot`
shift. Created by F2 (LLI full reset) interacting with the pre-existing clean-peer pass.
**Patched same day:** (a) `confidentlyCleanOnSignal` returns false for `lli_sats`;
(b) new `epoch_reset_amb_keys` registry (full / targeted / residual resets) and the
clean-peer pass skips any key reset this epoch — which also closes the H4
inconclusive-culprit double-handling variant and the residual-reset analog.

### P2-5. [major, 3/3 → PATCHED] F1's 0.3·λ_WL clean gate was σ- and maturity-blind
`computeAdaptiveThreshold` never returns NaN (warm-up/cold-start branches are finite), so
"finite threshold" was a vacuous maturity proxy — a 2-sample arc could affirm. And the
fixed 0.3-cycle bound admitted a real ΔN_WL=±1 slip with probability up to ~13 % at high
noise; the false ΔN_WL=0 split then ALIASES TO AN ACCEPTED wrong dual-signal repair on
L1/L5-class pairs (λ1/(λ1−λ2) ≈ −2.95 → frac 0.05; E1/E5b ≈ −3.28 → frac 0.28; only
L1/L2's −3.53 → frac 0.47 was safe).
**Patched same day:** affirmation now requires `count ≥ 10` AND `3σ_MAD ≤ 0.7·λ_WL`
(discriminating power: false-affirm of a real ±1 slip is a ≥3σ event, ≤0.13 %) AND
`|dev| ≤ 0.3·λ_WL`. Noisy low-elevation arcs simply never affirm → GF-only falls back
to reset there.

### P2-6. [major, 3/3 → PATCHED] KF multi-band slip reset invisible to all downstream invalidation
The non-pivot multi-band detector (KalmanFilter.cpp:2455 region) reset every phase
ambiguity of a satellite without pushing `last_reset_satellites_`/`last_reset_signals_` —
invisible to PP/RT arc clearing (incl. fix F4), to `reinflateResetSatStates`, and to
smoother segmentation; the stale wl-locked arc kept exporting the pre-slip integer to AR.
**Patched same day:** the site now pushes `last_reset_satellites_` (satellite-only, per
the documented contract for sat-wide resets) — which simultaneously makes
`reinflateResetSatStates`' sat-wide signature producible (see P2-7).

### P2-7. [major, 3/3 → PATCHED via P2-6] `reinflateResetSatStates` was dead code
Its trigger (a satellite in `getLastResetSatellites()` with no per-signal entry) had zero
producers — every KF reset site pushed both lists as a pair. Neither the PP call
(Application.cpp:810) nor the F4 RT call could ever reinflate; the only live reinflation
was the CycleSlipManager slip-phase path. P2-6's satellite-only push is the first
producer, putting the helper in service for exactly the sat-wide case it documents.
(The per-sat fallback arc-clear loops in both drivers also become reachable.)

### P2-8. [minor, 3/3] `getEpochCleanPairs` affirms in the MW-internal SD frame
A peer co-slipping the MW pivot's exact ΔN_WL has SD dev ≈ 0 → affirmed clean, but its ZD
wide-lane changed — TE's GF-only split then runs with a false ΔN_WL=0 (narrow co-slip
window; conflict reconciliation catches multi-pair sats).

### P2-9. [minor, 2/3] Residual reset on an un-adjudicated pivot key defeats the in-place "retain pivot" decision
Inflating the pivot's frozen variance triggers a handover the same epoch the in-place
machinery chose to retain it. Conservative (handover is lossless) but wasteful.

### P2-10. [minor, 3/3] Slip-phase pivot retirement erased by the epoch's first-pass handover
PivotManager.cpp:531 — the bounce-back backstop is disabled for slip-retired pivots.

### P2-11. [minor, 3/3] Reset-σ conventions fragmented
`getAmbiguityStd()` is a hard-coded 1e3 m (1e6 m² variance) not read from config;
AmbiguityResolver's `kAmbResetVar = 100 m²` carries a comment claiming it "matches
handlePhaseOutlier convention" — off by 10⁴.

## Pass-2 critic gaps (remaining work, ranked)

1. **LLI telemetry semantics** — `hasAnyPhaseLockLoss` treats ANY nonzero LLI as
   loss-of-lock, every epoch (level-triggered), but every producer encodes STATE flags:
   SbfParser maps the Septentrio half-cycle bit to 0x2 continuously; Meas3Decoder sets 0x2
   for the whole young-lock bucket; RINEX bits 1/2 also trip it. A persistent flag drives
   the LLI full-reset path every epoch — and the Pass-1 B1/F2 fixes raised the per-trigger
   cost (reinflation + TE arc clear), so a reset-storm on the active sbf_replay_covl145
   config is now more expensive. **Also corrects Pass-1's E3**: Meas3Decoder DOES emit a
   true lock-regression bit (0x1) — "RT has no loss-of-lock signal" holds only for the
   RTCM-MSM and SBF-Meas paths. Targeted pass needed: which SBF block the capture carries,
   lti3 bucket duration, and whether consumers should mask to bit 0x1 / producers should
   edge-trigger. Check replay logs for per-epoch "LLI lock loss" warnings before trusting
   the next run.
2. **IonoVariabilityMonitor**: live (adaptive iono process noise, up to 100× q overrides on
   the very states the slip machinery reinflates), a third independent GF implementation,
   zero audit coverage, and its `resetSatellite` has zero call sites (not registry-managed,
   not cleared by `CycleSlipManager::resetSatellite`). PP-only (RT never constructs it).
3. **AR hold-side lifecycle**: fix-and-hold pseudo-obs (1e-3 m² hold variance) have no
   slip-release path; `previous_dd_wl_` is keyed on the MW pivot (re-keys on every MW
   pivot handover → integer-change detection silently passes as "first time") and never
   pruned.
4. **EwlSlipDetector**: live, registered, one config token from active — zero coverage in
   both passes; pairs ANY two non-L1 bands, so the FDMA and intra-E5 degeneracy issues
   recur there unreviewed.
5. **Non-BSSD (ZD) mode**: the F2/F3 paths were never walked with
   `enableBetweenSatelliteSd()=false` — asserted clean by extrapolation only.
6. **Diff-scoped review + tests**: the uncommitted tree also carries non-slip edits
   (`max_duration_s`, replay-config toggles incl. `refine_emission_time: false`) that got
   no adversarial read; the slip chain has zero tests despite the 190-test harness —
   `TurboEditResolver::resolve` / `getEpochCleanPairs` are ideal pure-function
   characterization-test targets.

## Pass-2 patches applied (same day, after the completion run)

- P2-3: exclude-set union (CycleSlipManager).
- P2-4: `confidentlyCleanOnSignal` LLI guard + `epoch_reset_amb_keys` registry + clean-peer skip (CycleSlipManager).
- P2-5: σ-aware + maturity-gated epoch-clean affirmation (MwSlipDetector).
- P2-6/P2-7: satellite-only `last_reset_satellites_` push at the multi-band reset site (KalmanFilter).

Build clean (all 19 targets); `gnss_tests` 189/190 (1 fixture skip) — unchanged from the
pre-patch baseline measured by the critic.

**Still open after both passes:** P2-1/P2-2 (smoother/snapshot frame rework — fold in F1),
P2-8/P2-9/P2-10/P2-11, the six critic gaps above, and the Pass-1 open list (A3 intra-E5
denominator floor, A5/A6, D1/D2 reinflation on TE/aux/RT-slip resets, E3 decoder lock-time
— amended by critic gap 1, E4, G1–G4, H1–H9).

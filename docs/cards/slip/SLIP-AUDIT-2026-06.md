# SLIP-AUDIT: TurboEdit MW+GF slip machinery: the two-pass audit of June 2026 and its open register

**STATUS**

- **id:** SLIP-AUDIT
- **topic:** slip
- **status:** reference
- **summary:** Open register of the June 2026 two-pass audit of the TurboEdit MW+GF slip machinery (pass 2: 11 confirmed, 0 refuted), with the thresholds the committed fixes rest on; the full audit is in the attic.
- **dates in the text:** 2026-06-10 to 2026-10-07
- **source:** agent memory note `project_slip_audit_jun2026`, copied 2026-10-24; full audit `docs/attic/slip_repair_audit_2026-06-10.md`

**Outcome.** Every Pass-1, Pass-2 and smoother-frame fix of the audit is committed. The note below is the only live index of what stayed OPEN, plus the thresholds the fixes rest on (do not re-tune blind). Open items were spot-checked in the tree on 2026-10-07; the status note records which were not re-verified on 2026-10-24. The attic report's claim that the slip chain has zero tests is retracted by `docs/slip_comparison_report.md` section 4.

> Results are appended TO THIS FILE, below the existing text.

---

## 2026-10-24: open register and fix thresholds of the June 2026 slip audit

These results were recorded only in the agent memory note `project_slip_audit_jun2026` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: register items 2, 4 and 5 (locked-peer `rounded_wl`, TurboEdit and aux reinflation, `resolveWlOnly` inflation without an innovation gate; a screen now exists earlier in that function) and the minors were not re-verified; item 3 (intra-E5) may already be bounded by the later split gate `turbo_edit_split_max_sigma`; the review status of `EwlSlipDetector` is unknown.

Two-pass audit of the TurboEdit MW+GF slip machinery (memory note `project_turboedit_mwgf`) and every linked
layer, 2026-06-10 to 06-15 (Pass 2: 11 confirmed, 0 refuted). All Pass-1, Pass-2 and smoother-frame
fixes are COMMITTED (`Smoother::inverseReReferenceGroup`, `epoch_reset_amb_keys`,
`MwSlipDetector::getEpochCleanPairs`, `tests/test_smoother_frame.cpp`). The full record is
`docs/attic/slip_repair_audit_2026-06-10.md` (attic'd), so this memory is the only live index of what
is OPEN. ⚠ That doc's "the slip chain has zero tests" claim is RETRACTED by
`docs/slip_comparison_report.md` §4. Open items spot-checked still present 2026-10-07.

## Thresholds the fixes rest on (do not re-tune blind)
- MW cleanliness affirmation (`getEpochCleanPairs`): arc count >= 10, 3 sigma_MAD <= 0.7 lambda_WL,
  |dev| <= 0.3 lambda_WL. Gates the GF-only equal-slip split; unaffirmed GF-only flags RESET. Kills
  locked-arc-absorb aliasing (even dN_WL minting wrong (k,k) repairs). Deliberate trade: low-elevation
  blind-spot slips now reset rather than repair.
- LLI outranks everything: LLI satellites skip both culprit paths (full reset + reinflation); TE GF
  arcs cleared on LLI (MW EWMA deliberately preserved); LLI+TE-flagged satellites cannot be elected SD
  anchor; `confidentlyCleanOnSignal` refuses LLI satellites; `epoch_reset_amb_keys` stops the clean-peer pass shifting a just-reset key by -dN_pivot.
- RT parity: `setMaxGapSeconds(1.5 * max(gnss_rate, 1.0))` at construction.
- Smoother frame: repairs compensate `x_pred(amb) += dN*lambda` right after `detectCycleSlips`;
  resets segment; a CLEAN pivot handover inverse-references the smoothed estimate to the OLD pivot on
  the full state/cov; disturbed handovers, iono / sat-bias changes and any state-size change segment.
  NOT replay-validated (needs `run_smoother` over a gappy, handover-rich PPP-RTK run).
- `reinflateResetSatStates` was DEAD CODE until the KF multi-band slip reset began pushing
  `last_reset_satellites_`. A reinflation helper with no producer looks wired and does nothing
  (invariant #4).

## OPEN bugs, ranked
3. **LLI telemetry semantics** (active `config/sbf_replay_covl145.json`): producers emit STATE flags
   (SbfParser half-cycle and Meas3 young-lock -> 0x2 continuously; Meas3 lock-regression -> 0x1, a
   true loss) but `hasAnyPhaseLockLoss` fires on ANY nonzero LLI: possible per-epoch reset storms.
   Decide: mask the consumer to 0x1 or edge-trigger the producers. First step: grep a replay log for
   per-epoch "LLI lock loss".
4. MW-pivot ZD slip leaves LOCKED peers' `rounded_wl` stale; `resolveWlOnly` pseudo-obs then drag
   correctly repaired floats for ~0.4 eff_n epochs. Fix: shift locked peers by -dN_p, or suppress
   `getResolvedWl*` for the group until re-lock.
5. Intra-E5 pairs are numerically explosive (Gal L5/L7/L8, BDS B2a/B2b/B2ab; |lambda1-lambda2| 3 to
   7 mm): GF noise amplified x150 to 310, only guard `|denom| < 1e-6`. Fix: a ~2 cm floor. Same issue
   UNREVIEWED in `EwlSlipDetector`.
6. TE/aux full-satellite resets in CSM (`applyTurboEditGfOnlyResolutions`, EWL BandDifference) and RT
   pre-update resets skip iono / sat-bias reinflation; RT resets are plain, not BSSD-aware.
7. AR interactions: `resolveWlOnly`'s 100 m^2 inflation has no innovation gate; the fix-and-hold
   1e-3 m^2 pseudo-obs is never released on a slip; `previous_dd_wl_` is keyed on the MW pivot
   (re-keys on handover, change detection passes as "first time") and never pruned. Reset-sigma
   fragmentation: `getAmbiguityStd()` 1e3 m vs `kAmbResetVar` 100 m^2, off by 1e4.
8. nl_wl basis has NO slip path (CSM, RT, `handlePhaseOutlier` cannot address NL/WL keys); canonical
   in memory note `project_nl_wl_basis`.
9. Minors: half-cycle slips alias through frac <= 0.30 (no half-cycle hypothesis; ties to LLI 0x2); IVI says "stable" for < 3 satellites and
   `rate_ok` is unconsumed; MW jump integer unbounded (UB int cast); `GraphicSlipDetector` tests raw
   |dg| against a spread threshold and flags everything when enabled; baseline LLI results carry an
   empty satellite (DD erase no-ops); GLONASS peer shift uses the PEER's lambda and detectors'
   `PppUtils` never get the slot map; slip-retired pivots are erased by the first-pass handover; a
   residual reset on an un-adjudicated pivot defeats in-place retain; epoch-clean affirmation is
   SD-frame (a peer co-slipping the MW pivot's dN_WL is falsely affirmed); traces are blind to TE GF-only events; no AR-ability gate on repairs; no receiver
   clock-jump repair.

## UNAUDITED
`IonoVariabilityMonitor` (live, post-processing; modulates iono q up to 100x on exactly the states
slips reinflate; `resetSatellite` has ZERO call sites, re-verified 2026-10-07; a third GF
implementation); `EwlSlipDetector`; the ZD / non-BSSD slip chain.

Queued: characterization tests for `TurboEditResolver::resolve` + `getEpochCleanPairs`; consolidate
the three GF implementations (TurboEditResolver, IonoVariabilityMonitor, staged GfSlipDetector);
first validation run `sbf_replay_covl145` with `turbo_edit_trace.csv` (expect more resets at low
elevation).

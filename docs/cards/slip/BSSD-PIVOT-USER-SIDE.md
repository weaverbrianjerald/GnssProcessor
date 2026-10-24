# BSSD-PIVOT: user-side SD pivot defects: the MW reset that strands SD arcs, and the code-only PPP-RTK root causes

**STATUS**

- **id:** BSSD-PIVOT
- **topic:** slip
- **status:** open
- **summary:** Two user-side SD pivot defects with no other record: the MW reset at a pivot change (pinned by a test, fix NOT landed, awaiting the author) and the six code-only BSSD root causes (fixes committed, none validated on real data).
- **dates in the text:** 2026-04-16 to 2026-10-07
- **source:** agent memory notes `project_mw_reset_pivot_handover` and `project_codeonly_pivot_iono_scrub_gap`, copied 2026-10-24

**Outcome.** A satellite reset erases the SD pivot entry, so a coincident election change leaves every surviving SD arc stepping by N_WL(old) - N_WL(new); `tests/test_mw_pivot_reset.cpp` pins the defect, the fix knob `estimation.cycle_slip.mw_reset_pivot_handover` was prototyped on 2026-09-23 and lost, and landing it awaits the author's approval. Separately, code-only PPP-RTK/BSSD had six root causes behind a code outlier that recurred every epoch after a pivot handover (June 2026): all fixes are committed, none was validated on real data. Both notes follow, unchanged apart from dated status notes.

> Results are appended TO THIS FILE, below the existing text.

---

## 2026-10-24: MW reset at an SD pivot change: defect pinned by a test, fix not landed (design kept)

These results were recorded only in the agent memory note `project_mw_reset_pivot_handover` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: re-checked in the tree: the knob, the released-pivot map and the counters are absent, `tests/test_mw_pivot_reset.cpp` still pins the defect, and `RealTimeEngine::mwSlipDetector()` named below is not in the tree (it belonged to the lost prototype). Landing the fix awaits the author's approval; no register entry existed before this card.

Status (re-checked 2026-10-07): defect present at HEAD, pinned by `tests/test_mw_pivot_reset.cpp` (suite `MwPivotReset`, 4 cases); the fix knob `estimation.cycle_slip.mw_reset_pivot_handover` was prototyped and verified 2026-09-23 in a scratch worktree but NOT landed, awaiting author approval. ⚠ The prototype is gone (that session's scratchpad is empty as of 2026-10-07): rebuild it from the design below. The only repo record is the test file's header.

**Defect** (present since the reset path landed, `1f68d73`, 2026-04-16): `resetSatellite` erases every `sd_pivot_sat_` entry the satellite anchors; the pivot-change block in `detectLargeMwResets` (S-transform or erase, drop the old pivot's ZD arc) runs only against a stored pivot. If the next election picks another satellite, nothing is re-referenced and each surviving SD arc steps by N_WL(old) - N_WL(new). Unit bed (6 GPS sats, +5 cyc step, no real slip): the whole group flags within 6 epochs (the unlocked survivor publishes a known +5, the locked survivors drift-demote together, the new pivot's arc stays an SD arc and later publishes a known 3; epoch detail in the test) and re-converges about 15 epochs later. Same observations without the reset: silent.

**Exposure:** only with a BIA product (SD groups), only via `resetSatellite` (complete filter reset, or sats in `getLastResetSatellites()` without per-signal entries: the BSSD "NON-PIVOT SLIP" path; callers in `Application.cpp`, `RealTimeEngineEstimate.cpp`, `ApplicationBaseline.cpp`). KF pivot-slip paths go through `resetSatelliteSignal`, which keeps the pivot: NOT a trigger. The network producer never calls `resetSatellite`. Needs a coincident election change, so rare. Sim check (golden_sim_refound 2 h, prototype counters): consumer SZZ000SIM 240 SD elections, 1 change, 0 resets; producer 60 sta 98 changes (3 with the old pivot unobserved, no false slips after start-up), 0 resets; knob off/on/HEAD byte-identical except wall-clock ms columns (`D:/GnssProcessorSupport/runs/_rap_mwrph2_{off,on,head}`), so validation needs an injected consumer event.

**Fix design:** `released_pivot_sat_` map written in `resetSatellite` always, consumed at the group's next election (counted in both states, used as the previous pivot only with the knob on); `pivotResetCounters()` (elections, changes, old_pivot_unobserved, released, replaced, handled) with a run-end line in `NetworkTeardown` via `RealTimeEngine::mwSlipDetector()`; cleared in `resetAll`. Knob ON made the reset arm identical to the control in all three MW reference modes. Rejected: erasing dependent arcs at reset (loses every lock even when the same pivot is re-elected).

**Related gap (not built):** the old pivot's ZD arc is dropped only if it is observed at the change epoch; unobserved, it flags on return as a non-pivot (`OldPivotUnobservedAtTheChangeFlagsOnReturn`). Extends Rule 2 of memory note `project_pivot_handover_flipflop_fix`.

**On approval:** rebuild the hunks on the current MW files (the 2026-09-28 comment rewrite `b3a8e48` has landed, so context moved), add knob-ON arms beside the existing cases, validate on a PPP-RTK run with BIA and pivot resets and read the counter line.

## 2026-10-24: code-only PPP-RTK/BSSD: six root causes of the code outlier that recurred after a pivot handover (June 2026)

These results were recorded only in the agent memory note `project_codeonly_pivot_iono_scrub_gap` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Symptom (June 2026): in CODE-ONLY uncombined PPP-RTK/BSSD, a code outlier coinciding with a
pivot-slip detection recurred every epoch after the handover. Six root causes; all fixes committed
(`recordIonoReinflate` and `takeRequestedPivotHandoverIonoReinflateSats` in `KalmanFilter.cpp`,
`include/app/EpochUpdateSupport.hpp`; re-checked 2026-10-07). NONE was validated on real data.

Mechanisms worth remembering:
1. **Strip phase and the repair machinery goes dead.** A coherent pivot-side code bias is absorbed
   into the non-pivot per-sat iono states; the forced handover's lossless `reReferenceIonosphere`
   CONSERVES the error; `isPivotDisturbed` iterates `phase_sigs` (empty, so always false); iono
   reinflation is reachable only from phase paths. Fix: on a code-detector forced handover,
   re-inflate iono + sat-bias variance for the firing group's non-pivot members (not the
   `recordHandoverExcludes` deviators). ⚠ Apply INSIDE `handlePivotHandover` AFTER `setCovariance(P)`:
   re-inflating before T P T^T corrupts the re-reference.
2. **A shrinking denominator fires a percentage gate early.** The complete-reset check divided by the
   per-pass `res.size()`, tripping at ~37 % instead of the configured 50 %, then calling
   `resetFilterCovariance` (epoch-local exclusions return next epoch: oscillation). Likely the
   PRIMARY driver. Fix: `total_obs_count = num_obs + excluded_measurements.size()` captured once at
   the top of `validatePostFitResiduals`.
3. **Sat-bias seeding frame under BSSD** (the E03 case: seeded 35.8 km off its incumbents, a ~165 m
   outlier excluded every epoch and never repaired, since an excluded row never updates its bias).
   BSSD has no receiver-clock state, so `seedSatBias` absorbs the clock at the seeding epoch. Fix:
   under `enableBetweenSatelliteSd()`, `seed -= innov_pivot` when the pivot already has a same-band
   bias state. The sat-bias analogue of the BSSD frame pitfall.
4. **A pass budget sized by the wrong proxy freezes the filter.** `max_passes = max(10, n_amb+3)` is
   10 in code-only; one outlier peeled per pass, so 14 outliers exit without a commit and
   posterior = prior (frozen, P inflating). Field signature: RMS|sres| 22.9, sigma_pos ~22 m,
   14/41 over threshold (34 %, below the reset, so in that case the reset was NOT the culprit). Fix: `computeRebuildPassBudget`, min(256, max(10,
   n_amb+3, ceil(bad_meas_percent n_meas)+3)).
5. **RT had no rebuild loop**: `RealTimeEngine` ignored `update()`'s return and never drained
   `takeRequestedPivotHandovers()`; `BssdMeasurementBuilder::preparePivots` acts only on a pending
   forced handover, so RT forced pivot handovers never happened (E05 stayed pivot). Fix: a focused rebuild loop; AR runs after it on the committed state.
6. **Anti-drift extraction:** `EpochUpdateSupport` owns `computeRebuildPassBudget` and
   `forwardKfPivotHandovers` (previously triplicated / missing in RT). Loop envelopes deliberately
   NOT merged.

Open secondaries: `selectPivotCandidates`' exclude-blind fallback can re-elect an EXCLUDED old pivot
when the pool is exhausted (silent no-op handover); `handleCodeOutlier` is non-mutating and the code
detector lacks temporal corroboration; commit-the-clean-subset on exhaustion not implemented; RT
deferrals (per-(sat,signal) MW-history reset, complete-reset MW wipe, relinearization).

**Reinflation scope (mechanism confirmed, scope open).** `getSlipStateReinflationConfig()`: enabled
by default, `iono_std_m` / `sat_bias_std_m` 5 m, value-preserving, skips frozen pivots.
- Keep the one-shot P reinflation (zero row and column, set the diagonal, keep the mean; PSD-safe).
- Do NOT switch to Q inflation: a slip is a discrete event in one state; a Q bump degrades I/b every
  later epoch. Category error.
- It is stronger than Bayesian-minimal (which touches only the ambiguity); justified only because it
  is gated to full-satellite resets and code-detector disturbances.
- A/B later: zero only the cross-covariance between (I, b) and the reset ambiguity.
- The iono half is the suspect one (code re-anchors slant iono every epoch except in code-only). If a
  post-reset position/ZWD transient appears, suspect the iono reinflation, not the bias.

(the non-mutating `handleCodeOutlier` as an RT cascade amplifier).

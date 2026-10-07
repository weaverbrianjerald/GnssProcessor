# R2 sticky: the sticky continuous handover target, composed with the carry reref

**STATUS**

- **id:** R2 sticky
- **topic:** network-vehicle
- **status:** superseded
- **summary:** VALIDATED on the quiet 60-station sim (scoped, default OFF): the sticky continuous target ends the handover churn and halves the consumer's settled error; its pin target is superseded by RF-2's held basis.
- **dates in the text:** 2026-08-31
- **knobs / posture:** `network.phase_datum_handover_continuous` composed with `network.phase_anchor_carry_reref`, `network.phase_datum_handover_max_var_m2: 1e-4` (border-exact); armed scoped in `_camp60wu_quiet12h_r2.json`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/camp60wu_quiet12h` (baseline), `runs/camp60wu_quiet12h_r2` (R2), `runs/camp60wu_quiet12h_r2v1` (the falsified-ceiling arm)
- **source:** `docs/archive/SPEC_pivot_visibility_gauge_defect.md` §5.09, §5.09.1, §5.09.2

**Outcome.** The question: can a sticky continuous handover target compose with the 1(d) carry reref without erasing valid carries. The first 12 h arm refused 552 of 582 successors because the ceiling read the border-inclusive variance (FALSIFIED); the border-exact variance gave 573 continuous and cut reref erasures 10 236 → 165. The trade, named: code-bias settled rms 0.39 → 0.54 m (level lock-in), clock walk unchanged, AR 0 in both arms. The 1e-4 m² ceiling later refused every honest-σ successor ([AMB-IONO open questions](../ionosphere/AMB-IONO-OPEN-QUESTIONS.md) item 15), and [RF-2](../../refound/RF-2_held_basis_2026-09-15.md)'s held basis supersedes `anchor_pin_target` and retires the reref erase under its knob, armed on the goldens since [GRB](../../refound/GRB_golden_rebase_2026-09-22.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_pivot_visibility_gauge_defect.md -->
## §5.09 R2 EXECUTED (2026-08-31) — the sticky continuous target, and the pairing COMPOSED

**Trigger.** The 12 h zero-slip quiet flagship (`runs/camp60wu_quiet12h`,
SPEC_synthetic_minimum_network §17) put hard numbers on §5.08's residual: with the
reref armed and the continuous handover loader-disarmed, **582 of 621 anchor
handovers took the COLD path and the 1(d) response erased 10 236 phase-leveling
local carries over the run — on a fixture with zero true slips**.  Handover count
itself is near the geometric floor (per-(sat,signal) keys × pass durations;
0 bounce-backs — the sticky election is fine).  The cost is the response: every
~2.6 h per satellite, each family's carries die network-wide and every float arc
restarts, which is exactly the churn × arc-truncation mechanism that keeps the
clock/bias/iono separation code-limited (AGU-26 "churn, not accuracy, blocks").

**The build (this section's diff).**
- `NetworkCarryState::anchor_pin_target` — the pin target each incumbent is
  currently realized at, per "sat_signal" key.  Populated ONLY by continuous
  handovers; rebuilt wholesale each election beside `anchor_incumbent` (a key
  that leaves the union, or hands over cold, drops its target); never
  snapshotted (a warm resume's first handover is honestly cold); empty forever
  on the knob-OFF path, which keeps the OFF election byte-identical.
- `networkElectPhaseAnchors`: a RETAINED incumbent with a stored target keeps
  realizing it — the §5.07.3 T+1 revert (pin back to 0, family steps one epoch
  late) is gone.  Chains compose: the ledger is harvested in whatever frame the
  standing pins realize, so successive continuous handovers extend one frame.
- The 1(d) mark narrows to COLD re-realizations: `(is_handover && !continuous)
  || incumbent-less`.  A continuous handover under the sticky target provably
  realizes the same frame at T and after, so its family's carried state stays
  in-frame — marking it would marginalize a valid fold and erase live carries.
  The incumbent-less clause (first sighting / bridge exit, §5.08.6 finding 2)
  is untouched.
- Loader: the §5.08.6 pairing disarm is REPLACED by the composition (info log).
  The §5.0 measured-null verdict stands for the UNSTICKY mechanism it graded;
  R2 is the "compose only after an R2-style sticky target exists" condition
  that section itself set.

**⚠ The ceiling is now load-bearing for CARRY VALIDITY, not just datum sanity.**
A continuous handover preserves the frame only to the ledger value's own error
ε (≤ √max_var 1σ), and the surviving carries are mm-stiff — an ε of decimetres
would inject visible tension (the unexplained G21 +17.07 m / 378 σ incident in
§5.0 was accepted under the default 1.0 m² ceiling).  The armed configs
therefore run `network.phase_datum_handover_max_var_m2: 1e-4` (σ ≤ 1 cm): a
settled arc (mm-class after hours of zero-q carry) passes; a fresh arc fails to
the honest COLD path, which is the §5.0-validated behavior.  The loader default
stays 1.0 for compatibility; the info line now nags about tightness.

**Tests.** `tests/test_phase_datum_handover.cpp` (29 cases): sticky persistence
across tenure, cold-drop of the stored target, union-exit pruning, the
cold-marks/continuous-skips split in one election, and the loader composition
(`CarryRerefComposesWithTheContinuousPairing` replaces the disarm case).

**Validation ladder** (this session): OFF-path byte-identity old-exe vs new-exe
on a 120-ep smoke of the 12 h fixture; ON smoke counter sanity (continuous ≫
cold, erasures ≈ first sightings); then the 12 h flagship re-run graded against
truth (SPEC_synthetic_minimum_network §17) — results recorded below when run.

### §5.09.1 First ON arm FALSIFIED the ceiling's QUANTITY — border-exact re-validation (same session)

The first 12 h R2 arm produced 582 handovers with only **30 continuous — all 552
cold were REFUSED ledger values** (no misses), uniformly across the day, not just
in ignition.  The refusals were not young arcs: settled DD ambiguity σ implies
per-column relative variance ~1.2e-05 m², an order under the 1e-4 ceiling.  The
harvest was measuring the WRONG VARIANCE: `conditionalLocalVarianceDiag`
propagates the FREE SHARED BORDER's covariance into the local diagonal (the
right flavor for the leveling carry's prior, which re-injects against next
epoch's border), but in the handover step the family's shared phase-bias
uncertainty moves the pin target and the realized frame TOGETHER — common-mode,
cancels in the step.  Gating on the border-inclusive marginal therefore (a)
refused every settled successor whose family bias was momentarily loose, and (b)
retro-explains §5.0's "measured but unexplained" G21: a +17.07 m target passed
the 1.0 m² ceiling while landing a 378 σ spike, because a tight border can mask
a poor RELATIVE value exactly as a loose border masks a good one.

Fix: `NetworkEstimator::pureConditionalLocalVarianceDiag` — diag(N_rr⁻¹) on the
requested columns with the entire border treated as known (Σ_ss ≡ 0) — consumed
by the ledger harvest only; the leveling carry keeps its border-inclusive
flavor.  The ceiling now bounds the successor's error RELATIVE to the shared
frame, which is the quantity the surviving carries actually feel.

### §5.09.2 The 12 h A/B verdict — churn dead, consumer 2×, and the trade named

`runs/camp60wu_quiet12h` (baseline) vs `runs/camp60wu_quiet12h_r2` (single
mechanism: the composed pairing + 1e-4 border-exact ceiling), 1440/1440 both,
same fixture/build/datum.  (`runs/camp60wu_quiet12h_r2v1` is the §5.09.1
falsified-ceiling arm, kept as evidence.)

- **Mechanism: closed.**  582 handovers: 573 CONTINUOUS / 9 cold (all
  refused-var = honest fresh arcs; zero ledger misses).  Reref erasures
  10 236 → 165 (−98.4 %); marked family-epochs 789 → 216 (≈ first sightings).
  Largest avoided frame step 29.6 m.
- **Consumer (the product): the headline.**  3D med by quarter
  0.49/0.59/0.51/0.23 → 1.13/1.23/**0.16/0.19** m — R2 pays a worse ignition
  (frames lock while still raw) then converges and STAYS; settled-half rms
  0.493 → 0.265 m (−46 %), up rms 0.331 → 0.151 m (−54 %).  AR 0/1440 in both
  (pre-registered: this card was never the AR unblock — the per-band integer
  wall is iono-repr + float-only, unchanged).
- **Clock level walk: UNCHANGED** (rms by quarter 0.39/1.35/1.94/1.85 →
  0.27/1.32/1.99/2.01) — the §5.08 residue exactly as recorded: the per-sat
  walk is Q3 estimator work and the level needs an external authority; R2
  neither fixed nor worsened it.
- **The lock-in tax, named:** code-bias settled rms 0.39 → 0.54 m
  (p50 1.4 → 2.5 cm); ZWD converges to the same endpoint slightly slower
  (Q3 7.3 vs 9.3 cm, Q4 3.8 vs 4.3 cm).  The baseline's constant cold
  refloats were, perversely, a crude level re-tap toward code truth; with
  arcs surviving, whatever level the early run established persists.  That
  is the standing LEVEL-AUTHORITY frontier (LV-6), now unmasked rather than
  randomized — not a new defect.
- **vf's double across channels** (clock 15.5k → 33.4k) with σ_med TIGHTER
  (clk 1.5 → 1.1 cm): retention accumulates formal information faster than
  physical error shrinks — the carry-aware-covariance σ-honesty frontier,
  more exposed, as that card predicts for any retention gain.

**Disposition.**  The R2 pairing is VALIDATED on the quiet sim at 60 sta / 12 h
for what it claimed: frame + carry survival and product-stream continuity, with
the consumer as the beneficiary.  It stays default-OFF, armed scoped in
`_camp60wu_quiet12h_r2.json`; a field arm inherits the usual fresh-convergence
rule.  Follow-ups it sharpens rather than closes: the cb/level lock-in wants
the LV-6 per-epoch level authority; the vf inflation wants the carry-aware σ;
the clock walk stays Q3.

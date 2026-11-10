# RF-1-decision: the manager's decisions on the design read (2026-09-14)

**STATUS**

- **id:** RF-1-decision
- **topic:** network-vehicle
- **status:** closed
- **summary:** Build GO for the float-ambiguity vehicle in commit order: birth floor a-priori (0.5 m)², `value_abs` kept across a drop, declared handover S-transform in RF-1; step 2's read gates the mechanism.
- **dates in the text:** 2026-09-14
- **knobs / posture:** `network.amb_float_birth_var_m2` (a-priori (0.5 m)², elevation-mapped, `max(var_first, floor)`); the F3 fold machinery (`CouplingRowBuilder::buildLocalBlock`) re-pointed under the vehicle knob; the F3 posture (`phase_leveling_local_block`) off in RF-0
- **hashes named in the text:** none
- **run dirs named in the text:** none
- **source:** [RF-1_float_ambiguities_design.md](RF-1_float_ambiguities_design.md) sections 3.3, 3.4, 7.3, 7.6 and 9

**Outcome.** Of the design read's three questions, the election policy goes to RF-2 and the floor was to be set from D4's young-bucket σ̂₀(pl). [Steps 1 and 2](RF-1_steps12_read_2026-09-15.md) found the mechanism WARRANTED (field young-bucket σ̂₀ 12.0, not about 1), but the bucket was too heavy-tailed to set a floor, so the a-priori value stayed. [Steps 3 to 5](RF-1_steps345_mechanism_2026-09-15.md) built a correct vehicle, not sufficient on the field, where the phase datum churned. Since 2026-09-22 the float vehicle is the goldens' posture ([GRB](GRB_golden_rebase_2026-09-22.md)).

**Status as recorded (2026-09-14):** the build is GO in the design's commit order (`RF-1_float_ambiguities_design.md` §7.3), steps 1–2 first and READ before
any mechanism; steps 3–5 as separately armed A/Bs; graded only on the two instruments as deltas against RF-0 (§7.6).  The author may
overrule any of the three answers below; each is an engineering call inside the program the author approved.

## The three questions (§9), answered

1. **The birth floor** — the empirical route: `network.amb_float_birth_var_m2` starts at the posture's code-noise scale (an a-priori
   (0.5 m)² at the reference elevation, elevation-mapped like the row weights) so step 2 can run, and is then SET from D4's measured
   `σ̂₀(pl)` in the young bucket on the RF-0 posture, once, per bed, recorded in the card.  The floor is `max(var_first, floor)` as §3.3(b)
   proposes.  Rationale: risk #1 is exactly a floor the model cannot supply; the measurement is one read and it is pre-registered.
2. **`value_abs` survives a drop** — yes (§3.4 as written).  A drop is about confidence, not identity; the value is the linearization
   point, an exact solve no-op (`project_network_xlin_invariance`), and keeping it is what the lane-preserving refloat needs later.
   The erase stays for the gap / re-admission case, counted.
3. **The declared handover S-transform stays in RF-1** (step 5, its own arm).  A state that is erased at every anchor handover is not a
   state; RF-1's story is incomplete without it.  RF-2 owns the ELECTION policy (elevation, minimum age, no incumbent refloat without a
   declared handover) and the datum-side transforms — not the operator, which RF-1 lands and RF-2 reuses.

## Bars and readings

Exactly §7.6 of the design, with the RF-0 6 h baseline substituted for the PV-B placeholders when RF-0 publishes.  The three frozen
readings of §7.6 stand; in addition: **step 2's D1 / D4 read is a gate on the mechanism** — if D4 shows `σ̂₀(pl)` ≈ 1 in the young
bucket on RF-0 (the prior is already honest) the card's premise is refuted and steps 3–5 do not start without a new pre-registration.

## Scope note

The decision's "no F3" cut means the F3 POSTURE as shipped (`phase_leveling_local_block` with its integer ambitions) is off in RF-0.
RF-1 re-points F3's fold machinery (`CouplingRowBuilder::buildLocalBlock`) under the new vehicle knob with the birth / drop / handover
semantics fixed — the sim validates it first.  That is not the F3 card returning; it is the covariance fold, which was never the
defect, being used for the state it was built to carry.

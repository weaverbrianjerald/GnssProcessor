# RF-1 — the manager's decisions on the design read (2026-09-14)

**Status:** the build is GO in the design's commit order (`RF-1_float_ambiguities_design.md` §7.3), steps 1–2 first and READ before
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

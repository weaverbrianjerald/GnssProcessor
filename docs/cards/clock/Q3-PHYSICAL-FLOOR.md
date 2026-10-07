# Q3 floor: the physical clock floor measured, trap 5 resolved, the FC-1 gate retired

**STATUS**

- **id:** Q3 floor
- **topic:** clock
- **status:** closed
- **summary:** The clock corrections beyond the COD a priori walk at q_phys ≈ 2.9e-8 m²/s beneath a 2.7 mm measurement floor: trap 5 resolved, the FC-1 correlation gate RETIRED, the ladder re-scoped.
- **dates in the text:** 2026-08-10 → 2026-08-11
- **knobs / posture:** `clock_anchor_var_floor_m2` to be set from this measurement (the (2.7 mm)² floor plus margin, ~1e-5 m² class); instrument `scripts/q3_clock_structure_function.py` (32 satellites, 3 h, common mode removed)
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/cta3_ctl`
- **source:** `docs/archive/SPEC_clock_temporal_anchor.md` §7

**Outcome.** The question: how much of the clock correction is walk, and how much floor? The TDCP structure function is flat at 0.26 to 0.27 cm for lags of 30 to 120 s, then grows as √L (0.94 mm per 30 s): both earlier "physical change" figures were the floor. The FC-1 gate is retired as unliftable and the variance floor predicted to bind; it binds at q 2.9e-8 on the field leveling vehicle ([Q3-F](../clock/Q3-F-self-contained-time-link.md)), the goldens' q, which presumes a precise a priori clock ([SCL](../../refound/SCL_self_contained_clock_level_2026-10-03.md) section 1). Standing principle (the author): set stochastic models to measured physics.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_clock_temporal_anchor.md -->
## §7 (2026-08-10, Fable) THE PHYSICAL FLOOR MEASURED — trap 5 resolved, the FC-1 gate RETIRED, the ladder re-scoped

`scripts/q3_clock_structure_function.py` on `runs/cta3_ctl` (32 sats, 3 h, common-mode
removed).  The per-tick TDCP telescopes — accumulated change over lag L is the endpoint phase
difference, interior noise cancels — so a structure function separates floor from walk:

| lag [s] | SF robust [cm] | SF/√L |
|---|---|---|
| 30 / 60 / 120 | 0.269 / 0.262 / 0.268 | falling — **floor, flat** |
| 300 → 2400 | 0.318 → 0.846 | **0.0163–0.0173, constant — walk** |

**q_phys ≈ 2.9e-8 m²/s → 0.94 mm per 30 s (fractional stability ~1e-13).**  The clock
corrections beyond the COD a-priori are quasi-static at the mm-per-epoch level — the user's
clock-linearity expectation, confirmed quantitatively.  Consequences, each load-bearing:

1. **Trap 5 is RESOLVED, and both prior "physical change" numbers were the FLOOR, not the
   walk.**  FC-1's 1.7–2.0 cm hour table (its horizon column was already flat 30 s → 1200 s =
   non-accumulating) and this lineage's 0.30 cm TDCP column both measured SF(30 s)-class
   floors; the accumulating physics is 20× below even the smaller of them.
2. **The FC-1 correlation gate is RETIRED, not merely unmet.**  0.9 mm true per-step motion
   against a 2.7 mm measurement floor ⇒ structurally unliftable at ANY reachable q rung.  At
   the floor, quiet IS real.  Replacement gate set: dev census ex-pivot floor/ceiling,
   σ̂₀_code flat, consumer live+replay unchanged-or-better, guard-population health (§3), G4's
   event response.  "QUIETER not MORE REAL" stands as a description of q = 3e-5, no longer a
   criticism.
3. **The §3 knee (σ̂₀_clk → 1 at q ≈ 1.0–1.5e-5) is the DATA-INFORMATION knee, not physics** —
   where the carry meets the per-epoch measurement information, ~3 orders above q_phys.  Below
   it σ̂₀ cannot validate; the descent 3e-6 → 3e-7 → 1e-7 is graded on the replacement set.
4. **The variance floor changes role from "armed but inactive" to GOVERNING.**  At q ≤ 3e-7,
   q·Δt ≤ (3 mm)² sits below any honest correlated-error floor, so `clock_anchor_var_floor_m2`
   must be SET FROM THIS MEASUREMENT (the (2.7 mm)² floor + margin ⇒ ~1e-5 m² class) and is
   expected to BIND — deviation #1's consistency cap becoming load-bearing is the design, not
   a warning condition.  Invariant-#1 pressure is maximal at the bottom rung (a near-zero-q
   carry): G4 gates any arming.

**Design principle (user, 2026-08-11), standing for this ladder and beyond: aim to be
PHYSICALLY CORRECT — set stochastic models to measured physics and let model errors express
elsewhere as visible residuals/events, rather than absorbing them into inflated q.**  An
inflated q is how the 9 cm walk stayed invisible for a year: the error hid in the loosest
state instead of showing up as a residual anyone would grade.


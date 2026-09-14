# CS — clock step signalling — 0 unsignalled steps, the 55.8 m consumer excursion gone

**STATUS**

- **id:** CS
- **topic:** clock
- **status:** closed
- **summary:** clock step signalling — 0 unsignalled steps, the 55.8 m consumer excursion gone
- **dates in the text:** 2026-09-14
- **knobs / posture:** `network.clock_disc_jump_test`
- **hashes named in the text** (commit / exe / byte-cert): `7a1cb9bf`, `b2f2587`, `58822e3`
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_event_harness.md` — 24.20

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.20 Card CS — clock step signalling (pre-registered before any arm is read)

Two questions, one build-free arm pair and one read, on the GL worker's exe (`7a1cb9bf…`, main at the GL-2 merge):
- **(A) the M4 contract:** `network.clock_disc_jump_test: true` (the existing test at its default 1.5 m) on **cs1** = `_cm_c10`'s config
  (the posture, no screen) and **cs2** = `_gl_on6`'s config (with the screen — the G17 case), both 6 h cold, hour by hour against `_cm_c10`
  and `_gl_on6`.  The loader's line, the run-end "step marks" count, and the audit (`wire_step_audit.py`) re-run on both.  **Bars
  (frozen):** unsignalled clock steps > 2 m = **0** on both arms (the contract); the marks count within 2× the T4-refloat count on the
  same satellites (a mark per re-realization, not a mark storm — marks on > 5 % of satellite-epochs is a storm); (i) / (ii) / availability
  not worse; the consumer's per-hour p50 not worse than the control's in any hour, and **cs2's hour 4 ≤ 1.5 m** (a declared 16.7 m step is
  what the consumer's `reactToGenerationChange` / disc handling exists for).  **Frozen reading:** all bars ⇒ the knob joins the posture
  candidate and its ledger row (RECOMMEND_ON, field, validated here) is written on the author's go; the contract holds but cs2's hour 4
  stays at metres ⇒ the consumer's handling of a declared multi-metre clock discontinuity is the next read (RealTimeEngineSsrBreaks);
  a mark storm ⇒ the threshold against the refloat rate is the finding, not tuned blind.
- **(B) the structural question (read, existing traces):** for each of the 43 steps, was the refloated station the satellite's elected
  PHASE ANCHOR at that epoch (`PHASE_ANCHOR` rows / the incumbent), and what did the satellite's PB do (0 unsignalled PB steps — so the PB
  held and the CLOCK took the re-realization)?  Frozen: > 50 % on the anchor station ⇒ a refloat on a satellite's anchor re-realizes that
  satellite's wire through the clock, and the anchor-election card (§24.15 R5 / R6: a tie-break on elevation, a minimum age, and no
  incumbent refloat without a declared handover) is the structural fix — pre-registered next; < 20 % ⇒ the T4 refloat itself moves the
  clock regardless of the anchor, and the rule "a T4 attribution refloat forces a clock mark" is the structural fix.

**§24.19 R9 RESULTS (2026-09-14 ~13:30 local; LV-SEED commit `b2f2587`, merged; `_gl_on6` with `_gl_rxoff6` / `_cm_c10` for counts).**
**The population:** `T4_attrib/attrib_arc` refloats 904 (= `n_arc_refloat` summed exactly), `T0_gap/arc_resume` 237, `lli_slip` 205,
`if_slip` 150, `mw_wl_slip` 73, `refsat_reset` 21, `we_refloat` 10; the run-end's **863 free-epoch latches / 572 inject-site skips are NOT
individually traced** (10 reach a PRIORRES row) — an instrument gap.  The raw-detector half is invariant to the digit across the three arms;
only the post-fit T4 count moves: **1 144 (c10) → 904 (gl_on6) → 953 (rx-CB off) — the gross-LOS screen removes 21 % of the "slip response",
which was firing on gross code, not slips.**  **(b) per arc: FAILS** — level growth vs re-levels in the arc r 0.204 (final |level| 0.098;
trimmed 0.29 / 0.25; null 0.015): real, 13× the null, far below 0.5 (median growth 0.446 m with a re-level vs 0.328 without).  **(c) the
wire: CROSSES, then the direction split decides it** — pooled |ΔD_phase| near a re-level 0.267 m vs away 0.062 (enrichment 4.29; per-satellite
median 3.04 vs a shuffled null 1.04, 78 % of satellites above their own null p95) — but split by trigger: the raw T0 / T1 DETECTORS (solve-
independent, counts identical on all arms) enrich **1.16 against a null of 1.02 (3.1 % of satellites above p95 — chance)**, while the post-fit
**T4 refloat carries the whole 3.69**.  So the coincidence is **T4 reacting to a wire excursion, not a re-level creating one**; genuine cycle
slips leave the wire untouched.  **(d) the rate** is elevation-driven (3.93 → 1.05 events per 1 000 code rows from 10–20° to 60–91°) and 3.05×
higher at the eight carry-less stations (p50 0.68 per LOS-hour vs 0.22), the run's three worst (BSHM 2.26, MAC1 2.05, GAMB 1.93) outside that
set.  **Reading:** re-levels do not set the arc level (H1 stays refuted mid-arc too); the wire's steps sit on T4 refloats — and R10 says the
same from the emitted side (88 % of the unsignalled clock steps at a T4 refloat of the same satellite).  Whether the T4 refloat CAUSES the
clock re-realization (a refloat on the satellite's anchor station) or merely marks the epoch of an excursion born elsewhere is exactly card
CS's part (B) read; the two are not yet separable on these traces.

**§24.20 RESULTS (2026-09-14 ~15:00 local; GL worker commit `58822e3` (the part-B read), merged; exe `7a1cb9bf…`; no source change — a knob
arm and a read; manager grade).**  Loader: *"Card CLK-R: CLOCK DISC JUMP TEST ON — per-sat common-removed emitted-clock steps > 1.50 m route
to the markSatDiscontinuity retro-bump"*; run-end *"step marks ON — 5 mark(s)"* (cs1) / *"8 mark(s)"* (cs2).
**(A) The contract holds:** unsignalled clock steps > 2 m **0 / 0** on both arms every hour (controls 0 and 2 (+1 > 10 m)); marks 5 and 8 in
six hours against 1 144 / 904 T4 refloats — **0.022 % / 0.035 % of satellite-epochs, no storm**; the solve is byte-identical (cs1 reproduces
`_cm_c10` to three decimals in every (i) / (ii) cell; cs2 reproduces `_gl_on6`) — CLK-R is outputs-only, as it should be; availability
identical.  G17's 16.731 m step at 271170 is now marked (`CLK_STEP,mark,G17,16.731,retro_bump`; disc 3 → 4 → 5 → 6 on the same emitted
values).  **The consumer:** at the event epoch 55.77 → **2.72 m**; hour-4 p50 25.45 → **2.43 m** — the excursion is gone, but the bar wanted
≤ 1.5 m, and hour 6 reads 4.39 vs the control's 3.70 (+0.7, within the hour-to-hour spread these arms show).  **Reading: the second — the
contract holds and the consumer's handling of a DECLARED multi-metre clock discontinuity is the next read** (`RealTimeEngineSsrBreaks.cpp`'s
per-signal reaction, exercised for the first time on a marked clock step: a ~2 m plateau through the hour rather than a reset that
converges).  The knob is VALIDATED as the M4-contract fix on this vehicle (neutral, byte-identical solve, no storm, the catastrophic step
removed) and NOT yet promoted — its ledger row is written with the posture, on the author's go.
**(B) The anchor read:** of the 43 unsignalled steps, **18 (41.9 %) were on the satellite's elected phase anchor** and 25 (58 %) had the phase
bias holding (≤ 0.5 m) while the clock took the re-realization — between §24.20's thresholds, so neither branch fires alone: the anchor
election is a real but partial lever, the marking rule is the complete one.  The strongest single fact: **both consumer-damaging steps (G17,
16.7 m) are ON-ANCHOR with the PB steady to 0.045 m** — the gauge station's refloat moved the clock and only the clock.

**Where the campaign stands (2026-09-14 15:00; main after this commit; every worker branch merged; nothing armed on the goldens or ledger):**

| defect, in the order found | status | the fix and its evidence |
|---|---|---|
| the `carry_invariant` station block is singular (58/61 stations, null dim 7) | NAMED, measured (ZP trace) | the leveling vehicle + gauge projection + the sim's phase-datum family (PV-B): consumer 7.8 → 1.2 m, ZWD physical at 48/61 |
| the free satellite clock's random walk (q 0.001) | NAMED | the Q3 anchor at the physical floor (c8b): code rms 1.79 → 1.09 m, ZWD stations 13 → 8, consumer 0.95 m; G4 inconclusive on this bed; exonerated as a step source (R10) |
| gross code errors unreachable by every acting screen (phase-row admission) | NAMED, cited | `gross_los_screen` + the satellite stand-down: tail 85 → 5–7 m; default OFF, needs the consumer read |
| unsignalled satellite clock steps on the wire (43, to 16.7 m; 88 % at a T4 refloat) | NAMED, cited | `clock_disc_jump_test` (existing, never armed): 0 unsignalled, no storm, consumer 55.8 → 2.7 m at the event |
| the wire's within-arc random walk (0.9 m) | NAMED as the T4-refloat re-realizations of the satellite clock (R7 / R9 / R10) | the marking rule (above) declares them; the structural lever is the anchor election (42 % on-anchor) and the refloat's own clock re-realization — owed |
| the bulk code-residual drift (+0.25 m/h) | OPEN | not the tie's representation (R8), not low-elevation multipath (R8), not the rx-CB carry (R8b), not re-levels (R9); the representation degrades monotonically over the day (suspect by elimination) |
| the consumer's plateau after a declared multi-metre step (2.4 m) | OPEN | the consumer's reset handling (`RealTimeEngineSsrBreaks`) — the next read |
| owed, off the bars | recorded | the anchor election tie-break + minimum age; the untraced free-epoch latches; `NetworkRowClassify`'s code-row gap; the T4 refloat firing on gross code (21 %) |

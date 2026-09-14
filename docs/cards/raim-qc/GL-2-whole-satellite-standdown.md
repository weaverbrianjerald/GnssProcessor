# GL-2 + R8 — the whole-satellite stand-down, and R8 — the bulk drift's channel

**STATUS**

- **id:** GL-2 + R8
- **topic:** raim-qc
- **status:** open
- **summary:** the whole-satellite stand-down, and R8 — the bulk drift's channel
- **dates in the text:** 2026-09-14
- **knobs / posture:** `network.carry_rx_code_bias`, `network.gross_los_sat_standdown`, `network.clock_disc_jump_test`
- **hashes named in the text** (commit / exe / byte-cert): `6a6d73e`, `87a33bf0`, `579ae70`, `f33ce82e`, `1d482b7`, `7a1cb9bf`
- **run dirs named in the text:** `runs/_gl2_on6`, `runs/_gl_rxoff6`
- **source:** `docs/archive/SPEC_event_harness.md` — 24.19

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
### 24.19 GL-2 (the whole-satellite stand-down) and R8 (the bulk drift's channel) — pre-registered before either is read

- **GL-2 (build, default OFF):** when the gross-LOS rule fires on the SAME satellite at ≥ `gross_los_sat_min_stations` (3) stations in one
  epoch (or its over-bar station count reaches it), it is a satellite fault: the per-LOS declarations for that satellite are NOT made
  (or are released), and the satellite is routed to the satellite-level response the producer already has for a consensus fault (the
  worker names it from `raim_min_stations`'s path — a per-satellite wire withhold / non-emission for the epoch with the disc counter,
  never an epoch withhold), an EVENT at both edges with counters (satellites stood down / released, epochs, the stations).  Gates as
  §24.18.  Arm `runs/_gl2_on6` = `_gl_on6`'s config + GL-2 on, 6 h, against `_gl_on6` and `_cm_c10` hour by hour.  **Bars (frozen):** station
  max < 7 m every hour (re-based on §24.18's measured 5.4–6.8); the consumer's per-hour p50 not worse than c10's in ANY hour (hour 4 must
  read ≤ 1.5 m); (ii) not worse; availability identical; the G17 event quoted with the stand-down's lines.  **Frozen reading:** all bars ⇒
  the screen is consumer-safe and joins the posture candidate; the stand-down fires on > 2 satellites per hour ⇒ satellite-level faults are
  common on this bed and the wire's per-satellite emission policy is the next card; the consumer still blows up ⇒ the fault is not G17's
  code and the read is the consumer's own G17 handling.
- **R8 (read, on `_gl_on6` hours 2–6, where the tail is bounded):** the bulk drift's channel.  (a) per-LOS level by hour for LOS present
  across hours — its growth against the arc's elevation trend (rising / setting), the station's local solar time and latitude; (b) (i) by
  hour split into stations with and without a gross declaration; (c) the SH-tie residual |iono − field| p50 / p90 and the field's a₀₀ by
  hour (`network_ionosphere.csv`, `network_vtec_sh.csv`); (d) the clock anchor's trips and the rx-CB carry folds by hour; (e) the wire's
  D_phase by hour beside them.  **Frozen reading:** the per-LOS growth tracks the tie residual (r > 0.5) and the tie residual grows with the
  hour ⇒ the ionosphere REPRESENTATION (degree 8, one layer, 5 TECU) is the bulk channel — the card is the representation, by local time;
  the growth tracks the elevation trend (setting arcs) ⇒ multipath / group-delay at low elevation — an elevation-weighting card, and the
  floor is real; neither ⇒ the level channel's memory (the frozen rx-CB seeds aging) — decided by a 6 h `carry_rx_code_bias: false` twin.

**§24.19 R8 RESULTS (2026-09-14 ~10:00 local; worker commit `6a6d73e`, merged; `_gl_on6` hours 2–6 with `_cm_c10` as contrast; no twin
needed).**  Neither pre-registered branch crosses its bar.  Per-LOS level growth against the tie-residual growth: r **0.378** (against the
level 0.422), Spearman 0.13 / 0.07, trimmed 0.28 / 0.18 — real against the naive null (0.03) but only +0.18 / +0.20 against the
WITHIN-STATION null (0.198 / 0.223) that keeps station structure; against the elevation trend r **−0.056** (null 0.033) — refused, though the
binned medians do fall 0.043 → 0.016 m/h from setting to rising arcs; local solar time (circular R 0.058, null 0.052) and latitude: nothing.
**Half of the first branch is nonetheless true:** the SH-tie residual grows monotonically with the hour — p50 1.64 → 2.39 m (+46 %), p90 4.5 →
7.2 m (+59 %) — faster than the field's own a₀₀ (27.6 → 35.7 TECU, +29 %), still rising after normalising through hour 5: the
representation IS degrading over the window; it just does not predict WHICH lines of sight grow.  Per hour: gross-named stations carry the
worse (i) in four of five hours and invert only in hour 4 (the G17 stand-down hour); the clock anchor's decouples are non-uniform (25 / 114
/ 46 / 69 / 57; run-end 321 trips, 352 re-couplings, 81.8 % of folds at the variance floor); D_phase per hour (0.87 / 0.29 / 0.70 / 0.97 /
0.51) does not track (i) at all.  **The third reading applies: the bulk drift is named by neither the representation nor low-elevation
group delay, which leaves the level channel's own memory** — and the two facts on the table point there: the rx-CB carry withheld **6 704
folds at its 10σ gate with EIGHT stations carrying nothing for the whole six hours** (CAS1 CPVG GOPE KOKV LCK4 LMMF MRO1 OAFA — GOPE and
LCK4 among the persistent unphysical-ZWD stations), and its flagged-row count FALLS (31 → 14) while (i) rises.

**R8b (pre-registered): the 6 h `carry_rx_code_bias: false` twin.**  `runs/_gl_rxoff6` = `_gl_on6`'s config + `network.carry_rx_code_bias:
false`, 6 h cold, main's Release rebuilt after the GL merge (hash, smoke `87a33bf0…`), hour by hour against `_gl_on6`.  **Frozen reading:**
(i) in hours 3–6 within 0.2 m of hour 2 (and the eight carry-less stations no longer distinguished) ⇒ the rx-CB carry's frozen first-epoch
memory and its frame-blind gate (§21.18) ARE the bulk drift — the level channel's memory, exactly invariant #1's stale-value warp — and
the card is the gate's frame (§21.18's open fix) or the carry's retirement on this vehicle; the drift stays ⇒ the carry is exonerated on
the drift too and, by elimination, the representation's monotone degradation is the residual suspect — a representation-by-local-time
card (degree / layer / tie by hour), pre-registered next, not now.

**§24.19 R8b RESULTS (2026-09-14 ~11:00 local; worker commit `579ae70`, merged; main's Release rebuilt after the GL merge, exe `f33ce82e…`,
smoke `87a33bf0…`; `runs/_gl_rxoff6` = `_gl_on6` + `carry_rx_code_bias: false`, 6 h, 720/720, the carry provably inert — arming line and
both run-end counters gone, zero rxcb rows every hour).**  **The drift STAYS: the rx-CB carry is exonerated on the bulk drift.**  (i) p50 above
hour 2: +0.37 / +0.87 / +0.75 / +0.41 m with the carry off against +0.40 / +0.99 / +0.80 / +0.58 with it on — a 10–30 % shave, nowhere near the
0.2 m bar; the eight carry-less stations are STILL distinguished at the same ratios with the carry off (1.64 / 1.19 / 0.81 / 0.85 / 0.93; |ZWD|
2–2.6× the rest) — their carry was withheld BECAUSE they are the bad stations, not the reverse.  The tie residual grows the same way (p50 1.70 →
2.39 m, p90 4.6 → 6.9); the per-LOS tie correlation reads 0.71 / 0.66 on this arm only because its within-station null rises to 0.49 / 0.53 with
it (gap ≈ +0.2, unchanged).  Two unasked findings: the carry off REMOVED the hour-4 consumer blow-up (25.5 → 2.69 m p50; the 240-min mark
19.0 → 1.78) — confounded, the screen's firing pattern changed with the solve (G17 led `_gl_on6` with 13 declarations and does not appear in
`_gl_rxoff6`'s list, where G24 leads with 19: the ten-station G17 stand-down did not recur rather than being cured) — and WORSENED the tail
((i) max 5.37 → 19.7 m at hour 4, 6.77 → 10.6 at hour 6).  **Reading:** the carry is not the channel; the representation's monotone
degradation remains a suspect by elimination only (r ≈ +0.2 over the structure-preserving null) — too weak to build on.  One population
was never tested: **the mid-arc RE-LEVELS.**  §24.13's H1 refuted the level BORN at arc start; R6 tested the wire's energy beside DECLARED
boundaries and elections; neither looked at the slip response's own re-levelings — 394 free-epoch latches in 2 h (~3 per epoch), each
re-seeding a zero-q level from one epoch of code with its station-common error — invariant #1's re-coupling trap, the very walk
`relevel_free_epoch` killed on the sim (720/720) and has never been graded on the field.  R1's "74–77 % of levels GROW over the arc" and R7's
"random walk or steeper within the arc" are both what accumulating re-levels look like.

**R9 (pre-registered, LV-SEED worker, existing traces of `_gl_on6` / `_gl_rxoff6` / `_cm_c10`): the re-level population.**  (a) The
re-level events per (station, sat, signal) and epoch from `network_events.csv` / the slip traces (free-epoch latches, T4 refloats,
`relevel`-class rows — the worker names the rows and quotes one of each) — their count per hour beside (i) per hour, and per station beside
the station's hourly code rms (Spearman across stations, per hour); (b) per LOS arc, the number of re-levels inside the arc against the
arc's level growth (R1's |m_late − m_early|) and against its final |level| — r with a size-matched null; (c) the wire: per satellite, |ΔD|
at epochs where ANY tracking station re-levelled that satellite (± 1 epoch) against epochs where none did — the enrichment ratio and a
shuffled null (as R4 / R6), pooled and for the noisy four; (d) the re-level rate by elevation and by station (the eight carry-less stations
beside the rest).  **Frozen reading:** (c) enrichment > 1.5 over the null AND (b) r > 0.5 ⇒ the slip response's re-leveling IS the wire's
random walk and the bulk drift's channel — the card is the level's re-seed (a re-level that averages over the arc's accumulated code, or
the lane-preserving refloat SPEC_lane_slip_response owes) and the standing slip-response cost of FP-2 (§20.3) is the same defect; (c) ≈ 1
and (b) < 0.3 ⇒ re-levels are exonerated and the representation card is pre-registered next; a rate > 1 per LOS per hour at the eight
stations against < 0.3 elsewhere ⇒ the re-level rate is itself the bad-station signature (false slips on noisy code), and the detector's
field thresholds are the card.

**§24.19 GL-2 RESULTS (2026-09-14 ~12:00 local; worker commit `1d482b7`, merged; suite 2112 / 287 (0 failed); smokes OFF and ON `87a33bf0…`;
`--verify` PASS; exe `7a1cb9bf…`; manager grade).**  `network.gross_los_sat_standdown` (+ `_sat_min_stations` 3, `_sat_wire_withhold`)
invents nothing: on the over-bar station count it refuses / releases the per-LOS declarations and routes the satellite to the two
consensus responses the producer already has (the retro disc bump via `ura_retro_sats` → `NetworkSolPrep` → `markSatDiscontinuity`; the
per-satellite wire withhold via the `sol.sats` swap-prune), never an epoch withhold.  `runs/_gl2_on6` (6 h, 720/720): 20 stand-downs / 19
releases over 313 epochs, 1 499 per-LOS declarations refused, 352 satellite-epochs pruned; G17 stood down at tow **271170 on 15 over-bar
stations — one epoch BEFORE `_gl_on6`'s first per-LOS declaration** — and withheld from the wire from 271200.  **Bars FAIL:** station max
19.8 / 17.5 / 25.4 m in hours 4–6 (worse than `_gl_on6`'s 5.4 / 5.9 / 6.8); consumer hour 4 **22.25 m** (bar ≤ 1.5); (ii) better in 4 of 6;
availability identical; 3.33 stand-downs / hour on 9 satellites (> 2).  **What the failure actually is — §24.18's attribution was wrong:**
the consumer steps to **55.77 m at tow 271170 identically in BOTH screen arms**, BEFORE any G17 declaration existed in `_gl_on6`; the emitted
wire shows the producer's own **G17 clock jumping −0.81 → +15.92 m in one epoch with the discontinuity counter frozen at 3** (`emit_clock_disc`
and `emit_clock_frame_hold` both armed), one epoch after the Q3 anchor re-coupled ten satellites (`cta_recouple … in_band=4` at 271140).
G17 being 4–49 m out at fifteen stations was the SYMPTOM of the producer's own clock excursion — the screen read it correctly, GL-2
classified it correctly, and the wire withhold is one epoch late because the emit seams run before SB_RAIM.  c10 (anchor on, screen off) has
no excursion there: the step needs the screen's deweights (which change the code information feeding the shared clocks) AND the anchor's
re-coupling, which discharged it into G17.  **Two defects, both above the screen:** (1) **an unsignalled 16.7 m satellite clock step reached
the wire** — an M4 contract violation independent of its cause (the consumer must be TOLD; CLK-R's "613 → 1 wire discs" was measured on
`carry_invariant`, and its emit-side detector is evidently blind to this step); (2) **the anchor's re-coupling edge is BARE** — invariant #7's
re-coupling without re-validation, and the exact edge G4 was meant to grade: the guard fired, marked and re-coupled, and the re-coupling
itself discharged.  GL / GL-2 stay BUILT, default OFF, not consumer-safe; the per-satellite emission policy waits on (1).

**R10 (pre-registered, GL worker, existing runs `_cm_pvb`, `_cm_c8b`, `_cm_c10`, `_gl_on6`, `_gl2_on6`, `pvb6`, `g4a` / `g4b`): the wire's
unsignalled steps — an M4 contract audit.**  Per satellite per epoch from the emitted products (`network_solution.csv` / `products.txt`):
|Δclock| epoch-to-epoch (and |ΔPB| per signal) against the disc counters' increments; the count of steps > 0.5 / > 2 / > 10 m WITH and
WITHOUT a signalled discontinuity, per arm and per hour; each unsignalled step > 2 m attributed to the nearest producer event within ± 2
epochs (`cta_recouple` / `cta_decouple`, a phase-anchor election, a GROSSLOS row, a retro bump, a datum change, a refloat burst) with the
consumer's 3D step at the same epoch; and the CLK-R emit-side detector's own view of each (which test it applies, from the code, cited, and
why it did not fire on 271170).  **Frozen reading:** unsignalled steps > 2 m exist on an arm ⇒ the emit-side disc detector is defective on
this vehicle — a field-blocking card independent of the screen, graded on the consumer's steps; > 50 % of them within ± 2 epochs of a
`cta_recouple` ⇒ the anchor's re-coupling edge is the source and the anchor is NOT armable until its re-coupling marks a wire
discontinuity (or re-validates the clock before re-engaging); none on `_cm_c10` and `pvb6` ⇒ the steps are screen-induced only and GL's
deweight needs to feed the clocks' disc test.

**§24.19 R10 RESULTS (2026-09-14 ~13:00 local; GL worker commit `3168660` — `scripts/wire_step_audit.py`, merged; eight arms, no producer run).**
**The phase-bias wire is clean on every arm (0 unsignalled steps in 590 k rows); the CLOCK wire carries 43 unsignalled steps > 2 m on six
of eight arms, up to 16.7 m**, every one a per-satellite re-realization (the 32-satellite median step |·| ≤ 0.012 m at each): `_cm_pvb` 1,
`_cm_c8b` 0, `_cm_c10` 0, `_gl_on6` 2, `_gl2_on6` 1, `_cm_pvb6` 5, `g4a` 17, `g4b` 17.  **Attribution: 38 of 43 (88 %) sit at dt = 0 on a
`T4_attrib/attrib_arc <the same satellite> refloat`** — the post-fit RAIM attribution refloat re-realizes the satellite's clock and the emitter
ships it silently; 3 on a `PBGAUGE/handover_continuous`, 1 on a `cta_decouple` at −30 s, 1 the GROSSLOS pair; **`cta_recouple`: 0 of 43 —
the anchor is EXONERATED** (§24.19's GL-2 attribution, and mine, were wrong); screen-off arms carry them as readily as screen-on ones — the
defect belongs to the VEHICLE, not to card GL.  The consumer's step at the same epoch: 54.6 m once (G17 / 271170), 0.008–1.56 m otherwise.
**Why nothing signalled them — cited:** the emitter's stage-2 per-satellite step test (`NetworkClockEmitRebase.cpp:83–95`: `dev = innov` under
the frame hold, marked when `|dev| > jump_m` = 1.5 m, the mark reaching `markSatDiscontinuity` and a `CLK_STEP/mark` event at
`NetworkEmitStore.cpp:345–382`) is gated on `cfg_.jump_test` = **`network.clock_disc_jump_test`, default false (`GnssConfigLoadNetwork.cpp:
7647–7649`) and armed on NONE of these lineages** — the frame hold runs, the per-satellite test does not, and every run-end prints "step
marks off — 0 mark(s) > 1.50 m".  G17's common-mode-removed step was +16.731 m against a median of +0.0006 — 11× the disarmed threshold,
exactly the statistic the test computes.  **Readings:** "unsignalled steps exist ⇒ the detector is defective on this vehicle, field-blocking"
FIRES; "> 50 % at a `cta_recouple`" does NOT (0 %); "none on c10 / pvb6 ⇒ screen-induced" is REFUTED (pvb6 has 5 with no screen).
CLAUDE.md Amendment 5's "13 product-wide clock jumps silent to the consumer" is this defect on `carry_invariant`; the knob it names
(`emit_clock_disc`) populates the counter, and the knob that would MARK a step has never been armed on any lineage.


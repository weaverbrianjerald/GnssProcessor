# G-B: satellite-clock step injection (first class only)

**STATUS**

- **id:** G-B
- **topic:** sim-bed
- **status:** closed
- **summary:** The 0.5 to 2 m recovery floor is RETRACTED as a window artifact: graded against the null arm, a satellite-clock step is recovered at 92.4 to 92.8 % with no floor.
- **dates in the text:** 2026-08-11 to 2026-08-15
- **knobs / posture:** arms `config/sim/simnet5_gb{05,2,10}.json` (5 stations, one step on G14 at epoch 120 of 240), `sat_clock_q = 0.01`; `emit_clock_disc` and `clock_disc_jump_test` NOT armed
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` §9.6

**Outcome.** The question: how much of an injected satellite-clock step (0.5, 2 and 10 m) the producer recovers, and whether the step is marked. The original table charged the null arm's own −0.464 m drift between windows to the step; graded differentially the absorption is a uniform ~7.5 %, and a 0.5 m step is not undetected. Marking was untested (marker unarmed, an inert instrument); armed in [G-A 50-sta](../sim-bed/SIMNET-50STA-QC-TIERS.md) §9.7.3, a 10 m step gives a `CLK_STEP` mark. The owed list moved to [injection bench](../sim-bed/SIM-INJECTION-BENCH.md) §15.7; `network.clock_disc_jump_test` was later validated on the field ([CS](../clock/CS-clock-step-signalling.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §9.6 G-B — satellite-clock step, FIRST CLASS ONLY (measured 2026-08-11)

> ⚠️ **RETRACTED 2026-08-11 by adversarial review — do NOT quote the table below.**  The
> "recovery floor between 0.5 and 2.0 m" is a **WINDOW ARTIFACT**.  Pre-step, the G-B arms
> are bit-identical to the null arm (max |ΔC| = 0.000000 over 118 epochs), and the **null arm
> itself drifts −0.464 m between the pre- and post-windows** — so differencing pre-vs-post
> inside one arm charges that drift to the injected step.  Graded DIFFERENTIALLY against the
> null arm's own post-window (the seeds make this exact), recovery is **92.8 / 92.8 / 92.4 %**
> at 0.5 / 2 / 10 m: a uniform ~7.5 % linear absorption, **no floor, and 0.5 m is NOT
> "undetected"**.  G-B must be graded differentially against the null arm.  Related debt:
> §3.1's promised "re-worded 1 mm gate" was never built — the SPP is fed unperturbed rows, so
> nothing asserts the injection reached the observables.  (§9.7.5 now does that job for the
> slip classes, via `trace_slip_raw`.)

Bench demonstration, not the full matrix.  Arms `config/sim/simnet5_gb{05,2,10}.json` — 5
solve stations, one step on **G14** (40–44° elevation, well observed, so the result is not
confounded with the low-elevation noise that dominates G02/G13) at epoch 120 of 240.
Recovered magnitude = median C(G14) over epochs 125–150 minus median over 95–118 (C is
already the estimable contrast; the pivot is pinned to 0).

| injected | pre-step | post-step | **recovered** | error | latency |
|---|---|---|---|---|---|
| 0.5 m | −0.347 | −0.361 | **−0.014 m** | −0.514 | **not detected** |
| 2.0 m | −0.347 | +1.030 | **+1.377 m** | −0.623 | 0 epochs |
| 10.0 m | −0.347 | +8.415 | **+8.762 m** | −1.238 | 0 epochs |

**Empirical recovery floor is between 0.5 m and 2.0 m** at this configuration — a 0.5 m step
is entirely absorbed (recovered 3 % of it), while 2 m and 10 m are tracked within one epoch.
Recovery is **biased LOW by 12–31 %**, the balance going into the other estimable channels.
Latency 0 is expected rather than impressive: `sat_clock_q = 0.01` gives √(q·Δt) = 0.55 m of
process noise per 30 s epoch, so the clock state is nearly free and simply re-estimates —
the posture the Q3 card exists to change, and it means **this arm measures the absorption
floor, not a guard.**

⚠️ **Marking is NOT TESTED by these arms.**  Zero RAIM flags and zero R records at every
magnitude including 10 m — but `emit_clock_disc` and `clock_disc_jump_test`
were **not armed**, so the marker channel was never populated.  Per invariant #4 that is an
inert instrument, not a failing guard.  Arming them is the first item of the real G-B matrix.

**Still owed for G-B — ⚠ SUPERSEDED 2026-08-15 by §15.7, the single owed-list.**  Two items
this paragraph named are DISCHARGED: the marked re-run of the clock-step class happened in
§9.7.3, and cycle slips ARE now wired into an event matrix — the §15 bench, with a
±1/2/3/5/10/50-cycle magnitude ladder and an empirical MDB of **2 cycles**.  §15.7 also
carries a fact this paragraph did not know: **CB step/ramp is currently NOT INJECTABLE AT
ALL**, because both bias channels are epoch-constant by construction.  Read §15.7, not this
paragraph.

---


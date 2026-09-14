# PIVOT DEPARTURE — the consumer shock at a pivot handover is producer-internal carry free-fall — `scoped_handover_response` + `handover_stransform`, both promoted

**STATUS**

- **id:** PIVOT DEPARTURE
- **topic:** network-vehicle
- **status:** closed
- **summary:** the consumer shock at a pivot handover is producer-internal carry free-fall — `scoped_handover_response` + `handover_stransform`, both promoted
- **dates in the text:** 2026-08-04 → 2026-09-12
- **knobs / posture:** `network.scoped_handover_response`, `network.readmit_seed_horizon_s`, `network.clock_datum_sum_to_zero`, `network.clock_datum_broadcast_anchored`, `network.handover_stransform`
- **hashes named in the text** (commit / exe / byte-cert): `6a3531a1afb2b40b`, `cc689d5`, `c5b5df834d185ac8`, `1729cf5a`, `db80fcaa`
- **run dirs named in the text:** `runs/evseed_pivfresh_off`, `runs/evseed_pivfresh_scoped`, `runs/golden_sim_campaign_zwdblk_scoped`, `runs/evseed_stsg1_`, `runs/evseed_stsg2_off`, `runs/evseed_stsg2_`, `runs/golden_sim_campaign_zwdblk_sts`
- **source:** `docs/archive/SPEC_event_harness.md` — 19, 19.1, 19.2, 19.3, 19.4, 19.5, 19.6

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 19. The PIVOT DEPARTURE card — the 1.30 m consumer shock is producer-internal carry free-fall, not a datum move (census 2026-09-11 evening, read-only worker; pre-registered BEFORE increment 0's legs were read)

**The event (§16.5 stub).**  On the 6 h storm arms (start tow 302400) the GPS clock-datum pivot G01 sets at tow **319260** (4.6833 h): `PIVOT handover G:G01->G02 datum_bump`, 51 `cta_decouple` (21 E + 31 G) + 52 `cta_disc_landed`, an E record `datum_changed=1` scope G, an R record naming 52 satellites, 156 CB + 156 PB + 52 CLK discontinuity-counter bumps; the held-out consumer goes 0.074 → **1.338 m** 3D (Up −1.216) in one epoch, plateaus at 0.18–0.24 m and never regains 0.074 before the Galileo switch (`E:E02->E03`, 321900) re-hits it.  Arm-invariant (mwevid 1.301, cbscope3 1.301, cbscope4 1.309, cbscope5 1.338).

### 19.1 What the census established (cbscope5 `products.txt`, step across 319230 → 319260)

| quantity | n | step mean | step sd | control sd (319200 → 319230) |
|---|---|---|---|---|
| GPS clock | 31 | **+1.8371 m** | 0.0054 | 0.0033 |
| Galileo clock | 22 | −0.0029 | 0.0044 | 0.0024 |
| CB G C1C / C2W / C5Q | 31 | ≈ 0 (1e-6) | 0.0092 / 0.0108 / **0.1666** | 0.00003 / 0.00003 / 0.0003 |
| PB G L1C / L2W | 31 | −0.026 / −0.006 | **0.184 / 0.249** | 0.0008 / 0.0013 |
| PB E L1C (a GPS switch!) | 22 | +0.040 | **0.446** | 0.0005 |

1. **The clock step is a pure S-transform.**  G02's emitted clock after the switch is bit-identical to G01's before it (−0.234255…): a rigid +1.839995 m shift on every GPS satellite, scatter = ordinary epoch noise.  The consumer's white-noise receiver clock absorbs it with no action (`RealTimeEngineSsrBreaks.cpp:290-292` says so itself).
2. **The bias DATUMS held** (step means 1e-6 m — the W6 time-linked code datum and the prioritized phase datum are not tied to the pivot) **but every bias re-realized**: spreads ×150–460, decaying over ~30 min (P_L1C 0.186 → 0.054 at +4 min → 0.012 at +14 min), and **Galileo's biases re-realize on a GPS switch** (and GPS's on the Galileo switch at 5.4167 h).
3. **The mechanism:** `src/core/app/NetworkRunContext.cpp:2483` `if ((!datum_changed || scoped_handover) && !prev_carry.empty() && !fullcov_on)` — with `network.scoped_handover_response` at its default `false` (absent from the golden), `datum_changed` skips the ENTIRE D2 shared-carry prior block for one epoch — every carried shared parameter (sat clock, sat CB, sat PB, rx CB, VTEC SH) across ALL constellations solves from one epoch of data and re-accumulates for ~30 min: `SPEC_pivot_visibility_gauge_defect.md` §8.2's "one epoch of free fall".  Mirrored at :2611 (dense) and :2792–2812 (oscillator).  `NetworkClockAnchor.cpp:234-243` decouples EVERY anchored clock on `datum_changed` (unscoped); the landed marks fill the R record and `retro_emit_sats`.
4. **`emit_clock_frame_hold` is disarmed on exactly this epoch by design:** `NetworkEmitStore.cpp:319-322` forces every key of the changed constellation, the μ vote is empty (`min_common` 4), μ = 0, the +1.84 m goes to the wire (run-end Σ|μ| drained 3.59 m vs the two handovers' 2.43 m undrained).
5. **The consumer's cost is the resets, not the clock:** at 319260 `resetOnPhaseBiasDisc` fires 57× (19 sats × 3 signals), `reinflateOnCodeBiasDisc` 57×, `reinflateOnClockDisc` 19×, then 53/40/38 PB resets on the next three epochs; no generation change.  The wire carries counters and flags, never magnitudes (`NetworkProductIO.cpp:569-583, 634-680`), so a consumer cannot transform what it is not told.

### 19.2 The harness leg — the departure with NO injection

Quiet seed `evseed_quiet6h`: the same switch at tow **276150 = epoch 565** (one GPS repeat earlier; seed signature GPS clock step +0.5930 ± 0.0055, B_C5Q 0.0011 → 0.171, P_L1C 0.0024 → 0.531; consumer 42 PB resets / 40 CB / 13 CLK at the switch).  Snapshot `state_snapshot.txt.ep0545` (tow 275550, n_emitted 545, fingerprint 6a3531a1afb2b40b) ⇒ leg first epoch 275580 ⇒ **the departure lands at `at_ep 19`, the 20th leg epoch, +600 s, with `event: null`**; run ≥ 80 epochs to cover the ~30 min decay.  Gradeable on a leg: the wire step series per product (clock mean ± sd = the S-transform test; CB/PB step rms per signal per epoch and its decay; cross-constellation bleed), the declared identities (`PIVOT`, `cta_decouple` / `cta_disc_landed` by constellation, R/E record scope), the consumer's per-epoch RESET counts from `rt_engine.log` (driven by declared discs, not accuracy — clean on a cold consumer), the producer's truth error in the realized basis (`event_frame_grade.py` C / P / oth channels), the CLK-R teardown.  **Storm-only:** the 1.30 m shock and its recovery (the cold leg consumer sits at a 1.6–2.5 m floor).

### 19.3 Pre-registered increments and gates

**Increment 0 — config only, `network.scoped_handover_response: true`** (built, RTA-2, default false): legs `pivot_off` / `pivot_scoped` from ep0545, 80 epochs, one exe (`gnss_processor_opin_rb.exe` F4FF8EF0…, main cc689d5 + the inert oracle-pin seam).  Expectations: Galileo CB/PB/clock step spreads at control level under scoped, GPS spreads unchanged; `cta_decouple` 51 → 31 (G only), R record G only; consumer resets at the switch ~57/57/19 → ~30/30/10; the GPS clock step unchanged (+0.593).  Verdict: confirms the §19.1(3) mechanism if and only if the Galileo half disappears.

**Increment 1 — the clock-carry S-transform** (`SPEC_pivot_visibility_gauge_defect.md` §8, queue 1(b), SPEC'd 2026-08-04, UNBUILT — `handover_stransform` absent from `src/`): at the switch transform the carried shared means by `c₀ = x_q_pre / v_c[q]` along `NetworkSbasis`'s structural per-constellation clock null direction (`x ← x − v_c·c₀`, `var += v_c²·var_q`; one scalar per constellation per edge) so the D2 fold is NEVER skipped and no carry free-falls; decoupling edge = the PIVOT event + `sts_applied` / `sts_rows_retained` counters (> 0 or the wiring is inert), re-coupling edge = the transformed carry's first fold, evented per satellite; OFF byte-identical.  Gates on the leg: CB/PB step rms at the switch at control level for BOTH constellations, consumer resets at the switch → 0 PB / 0 CB (clock notes may remain), the emitted GPS clock step either held (μ absorbs it) or still the rigid +Δ with scatter at noise — never a per-satellite scatter; §8.7 gate 6 (de-medianed states matching at the edge then diverging) = halt, do not tune.  Then ONE storm arm: consumer at 4.6833 h ≤ 0.15 m instead of 1.30, hourly p50 h4–h5 not worse than cbscope5, everything else in the §16.8 table not worse.

**Not pursued first — the consumer-side transform:** the only part it could absorb cleanly (the clock) is already free; the 57 per-(sat,signal) PB jumps are under-determined from one epoch and unvalidatable against a wire that carries no magnitudes.

### 19.4 Increment 0 RESULT — `network.scoped_handover_response` removes the collateral constellation's re-realization entirely; the consumer's metre-class shock was mostly collateral (2026-09-11 ~22:30 local; fresh-convergence pair `runs/evseed_pivfresh_off` / `runs/evseed_pivfresh_scoped`, 643 epochs each, one exe F4FF8EF0…, single knob)

**The pre-registered leg pair could not run — finding #1.**  `scoped_handover_response` is solve-affecting and therefore in the warm-resume fingerprint: leg B was REFUSED before its first epoch (`state_snapshot_require` — snapshot 6a3531a1afb2b40b vs live c5b5df834d185ac8), which is the guard doing its job.  **Every A/B of a fingerprinted knob on the harness must be a fresh-convergence pair (or a re-written seed per posture); increment 1's `handover_stransform` is specified fingerprinted too (gauge-defect SPEC §8.5).**  §19.3 is amended accordingly.  The substitute — two 5.35 h fresh runs from the fixture start, today's golden ± one knob — is also the ledger's own pre-registered settling run for this knob, which had zero measurements before today.  Validity: only knob differing; the RTA-2 line 1× / 0×; both edges at identical tows (Galileo 271230, GPS 276150); the fresh OFF arm reproduces the Sep-10 seed's signature to 4–5 digits across a new exe and four new slip knobs.

**The Galileo edge (271230) is the clean test** — the first datum edge of the lineage, arms provably identical before it (controls to 5 digits, consumer bins identical), and the GPS datum provably does not move there (GPS clock step +0.0103 m = noise), so every GPS effect is 100 % collateral:

| GPS-side channel | control | OFF @switch | scoped @switch | verdict |
|---|---|---|---|---|
| GPS clock step mean / sd | −0.0005 / 0.0033 | +0.0103 / 0.0053 | −0.0001 / 0.0042 | no frame move |
| CB G C1C / C5Q step rms | 0.00004 / 0.00045 | 0.01085 / 0.19375 | **0.00004 / 0.00046** | at control (271× / 421×) |
| PB G L1C / L2W / L5Q step rms | 0.0019 / 0.0026 / 0.0026 | **0.704 / 0.895 / 0.937** | **0.0033 / 0.0052 / 0.0055** | at control (212× / 173× / 172×) |
| PB E L1C (the datum that DID change) | 0.0020 | 0.5576 | 0.5573 | identical — Galileo's own share untouched |
| `cta_decouple` / R record | — | 52 (21 E + 31 G) | **21 (E only)** | scoped |
| consumer resets PB / CB / CLK | — | 42 / 42 / 14 | **18 / 18 / 6** | = the E share, exactly |
| consumer 3D, 5-min bin at the edge (med / max) | 0.103 / 0.124 before, both | **1.143 / 1.615** | **0.117 / 0.139** | the shock is gone |

The GPS edge (276150) is symmetric: Galileo CB/PB step rms to control (158–179×), `cta_decouple` 52 → 31 (G only), resets 42/40/13 → 24/24/8 (the G share, exactly), consumer 1.629 / 1.989 → 0.272 / 0.298 m; the GPS clock step +0.593 m is unchanged in both (the datum did move; the consumer absorbs it free).  Whole-run settled consumer: 3D rms **0.5618 → 0.1851 (−67 %)**, p50 0.3639 → 0.1348, max **1.989 → 0.361 (−82 %)**, Up rms 0.436 → 0.145; AR accepted 0 in both (no AR confound).

**The producer's own products were never more accurate for the free-fall:** `simnet_grade` whole-run settled — sat-clock contrast rms 1.6357 vs 1.6356 (identical), ZWD 0.0076 vs 0.0075, rx-DCB contrast 1.2325 vs 1.2284, rx-clock contrast 1.9442 → 1.8562 (−4.5 %), code bias a sign-inconsistent wash (rms +3 %, p50 −21 %; at the Galileo edge the OFF free-fall transiently pulled GPS code bias *closer* to truth).  Methodological: absolute `clk_rms` steps at every edge in BOTH arms (1.37 → 1.63) while the datum-invariant contrast is flat and identical — the absolute jump is the datum moving (invariant #5, live).  **Scoping does not make the products better; it stops the producer telling the consumer to discard state that was still valid.**  The sharpest structural point: the OFF arm's E record already says `datum_changed=1, scope=E` while the R record bumps all 52 satellites — the producer computes the correct scope, writes it on the wire, and ignores it internally.

**Residuals, recorded:** the SH-VTEC carry skip stays global by design (`NetworkRunContext.cpp:2153`, the RTA-2 line says so); there is NO run-end counter for clock-pivot handovers (only the `PIVOT` event row and the E/R records) — §8.7 gate 2's `sts_applied` / `sts_rows_retained` need that baseline built alongside increment 1; `event_frame_grade.py` cannot grade a network-wide datum event (it needs `--null` + an injected arc) — `simnet_grade --series` per constellation was used instead.

**Increment 1 re-scoped.**  Increment 0 collects the collateral share (most of the shock) for free; what the S-transform must deliver is the handing-over constellation's OWN carry (GPS at 276150: PB L1C step rms still 0.583, resets 24/24/8) — the population `c₀ = x_q_pre / v_c[q]` re-expresses instead of discarding.  Gate for increment 1: CB/PB step rms at control for the CHANGED constellation too and resets → 0 PB / 0 CB, graded on the datum-invariant contrast and the consumer, with increment 0 armed as the baseline; A/B as a fresh-convergence pair.

**Pre-registered final validation (in flight):** ONE storm arm `runs/golden_sim_campaign_zwdblk_scoped` = cbscope5 + the knob, exe F4FF8EF0…: consumer 3D at the GPS-edge epoch ≤ 0.35 m (C5 1.338) and at the Galileo edge ≤ 0.35; pooled settled p50 ≤ 0.1204 and window-excluded ≤ 0.1028 (both expected to improve — the pivot-recovery window alone was 0.221 in C5); hourly h1–h3 within ±2 cm of C5; resets and `cta_decouple` scoped at both edges; ZWD ≤ 0.0069, WL ≥ 99.2, L1C ≥ 89.9, T4 ≤ 11, W2 absent, split 0.0/0, cold ≤ 78; contrasts within ±3 % of C5.  PASS ⇒ `network.scoped_handover_response` RECOMMEND_ON in regime multi-constellation + closed-loop/service consumer, pinned in `golden_sim_tri_w6` (sim-validated; field arm owed — the knob's RT origin is RTA-2).

### 19.5 Storm arm + PROMOTION — `network.scoped_handover_response` is RECOMMEND_ON and pinned in `golden_sim_tri_w6` (2026-09-11; `runs/golden_sim_campaign_zwdblk_scoped` = cbscope5 + the knob, exe `F4FF8EF0…`, 720/720, 1242 s, single knob)

The §19.4 fresh pair established the mechanism on a 643-epoch lineage; this is the
final-validation arm on the storm bed, against **C5 = cbscope5** — the same base,
one knob.

| channel | C5 | **scoped** | verdict |
|---|---|---|---|
| **consumer settled p50 / rms** | 0.1204 / 0.2215 | **0.0825 / 0.0899** | −31 % median, −59 % rms |
| W1 window-excluded p50 / rms | 0.1028 / 0.1848 | **0.0804 / 0.0853** | −22 % / −54 % |
| consumer hourly p50 h0…h5 | .524 .113 .114 .101 .090 .236 | .524 .113 .114 .101 .089 **.047** | h0–h3 IDENTICAL, h5 5× better |
| pivot-recovery window 4.68–5.40 h / last 36 min | 0.2211 / 0.2671 | **0.0938 / 0.0553** | the window this card is named for |
| **settled max \|3D\|** | 1.3377 | **0.2009** | the metre-class shock is gone |
| consumer Up rms / max | 0.1557 / 1.2164 | **0.0752 / 0.1968** | |
| ZWD settled truth rms | 0.0069 | **0.0066** | better |
| ref-free WL / EWL / L1C | 99.2 / 100 / 89.9 | 99.2 / 100 / **90.2** | better |
| T4 events | 11 | 11 | unchanged |
| W2 4.9917 h fallout | 0/0/0 | 0/0/0 | absent |
| split-referenced mean / max | 0.0 / 0 | 0.0 / 0 | unchanged |
| PHASE_ANCHOR handovers / cold | 315 / 78 | **315 / 78** | IDENTICAL |
| PB-GAUGE elected / continuous / cold | 462 / 362 / 100 | **462 / 362 / 100** | IDENTICAL |
| sat-clock contrast / code-bias rms / rx-DCB / rx-clock | 0.7649 / 0.0912 | 0.7657 / 0.0902 / +0.01 % / +0.41 % | producer's own products move < 0.5 % |
| run time | 1050 s | 1242 s | **the only channel worse — CONFOUNDED (different exe)** |

**The pre-registered settling run is satisfied: the blast radius shrinks to the
constellation whose datum actually changed, measured at BOTH edges.**

| edge | channel | OFF (C5) | scoped |
|---|---|---|---|
| GPS (319260) | consumer 3D at the edge epoch (Up) | **1.338 (−1.216)** | **0.126 (−0.118)** |
| | Galileo CB C1C / C7Q step rms (the control) | 0.0038 / 0.159 | **0.00001 / 0.00027** |
| | Galileo PB E L1C / L5Q step rms | 0.447 / 0.649 | **0.0012 / 0.0017** |
| | GPS clock step (the datum that DID move) | +1.8371 | +1.8365 — unchanged |
| | `cta_decouple` / R record | 52 (21 E + 31 G) | **31 (G only)** |
| | consumer resets PB / CB / CLK | 57 / 57 / 19 | **30 / 30 / 10** — exactly the G share |
| Galileo (321900) | consumer 3D at the edge epoch | **0.777** | **0.031** |
| | GPS CB C1C / C5Q step rms | 0.0063 / 0.179 | **0.00007 / 0.0020** |
| | GPS PB L1C step rms | 0.542 | **0.045** (12×; the residual is concurrent anchor handovers / slip responses, not the datum) |
| | `cta_decouple` | 50 | **21 (E only)** |
| | consumer resets PB / CB / CLK | 54 / 51 / 17 | **27 / 24 / 8** |

**Isolation.**  The ONLY event counters that moved are `cta_decouple` 103 → 52,
`cta_disc_landed` 103 → 52, `cta_recouple` 154 → 103 and `seed_held` 24 → 31.
`common_band` / `if_slip` / `mw` / `lli` / `anchor_handover` /
`handover_continuous` / `anchor_slip_handover` / `handover_cold` / `col_removed` /
`col_added` are all identical; the CLK-R clause is identical except Σ|μ| 3.59 →
3.57 m; the PB-GAUGE clause is byte-identical.  The knob touches the pivot-handover
response and nothing else.

**A second defect fell out.**  The READMIT run-end clause's "**7 dropped at a datum
change**" goes to **0**: parked re-admission entries belonging to the constellation
whose datum did NOT change were being discarded, so the network-wide response was
silently degrading the already-promoted `network.readmit_seed_horizon_s` card.  Q3
trips within 300 s of a clock seed 1 → 0 for the same reason.

**PROMOTION (this section is the pass).**  `network.scoped_handover_response` →
**RECOMMEND_ON**, regime = a PIVOT clock datum (inert under the anchor-free
`clock_datum_sum_to_zero` / `clock_datum_broadcast_anchored`, where handovers cannot
occur) **AND ≥ 2 constellations** — the benefit is entirely "spare the other
constellation", so one constellation has nothing to spare — **and a consumer of the
products** (closed-loop / service / RT), since the producer's own contrasts move
< 0.5 % and the whole effect is in what the wire tells the consumer to discard.
Executable regime: `n_sys >= 2 and not on("network.clock_datum_sum_to_zero") and not
on("network.clock_datum_broadcast_anchored")`.  Posture row added (Tier::ON), pinned
in `golden_sim_tri_w6`, generator re-run: **verify PASS** (55 recommended knobs armed
in regime, 2 documented exceptions, 0 failures), `config_posture_check.py
--verify-loader` **0 coverage gaps**.

**Interlocks kept.**  `service_mode` force-arms it unconditionally
(`GnssConfigLoadNetwork.cpp:3093`), including under the anchor-free datums where it
is armed-but-inert.  **⚠ FINGERPRINTED — fresh-convergence only**: it is
solve-affecting, so it cannot be adopted mid-chain and **no warm-resume A/B is
possible** (which is precisely why §19.4 had to be a fresh-convergence pair).

**Residuals, recorded not fixed.**  The SH-VTEC carry skip stays GLOBAL by design
(`NetworkRunContext.cpp:2153` — the RTA-2 line says so).  There is still no run-end
clock-pivot handover counter, so the handover population has to be recovered from
the R records.  The 1050 → 1242 s run time is a **different exe**, not a measured
cost — a same-exe timing pair is owed, and the knob removes work rather than adding
it.  And the FIELD arm is owed: `golden_field_gps_dual` is GPS-only, so the knob is
structurally inert there — a documented manifest **exception, not a pin** — and it
becomes armable when that lineage goes multi-constellation.

### 19.6 Increment 1 BUILT + PROMOTED — the clock-carry S-transform (`network.handover_stransform`): the changed constellation's own carry stops free-falling; every WIRE gate passes at 250–575× back to control, and the two CONSUMER edge gates that failed were mis-specified (2026-09-11 night build, 2026-09-12 promotion; worktree `worktree-stransform`, exe `B3A7E11B…` / promoted build `C2F2B297…`)

Queue 1(b), SPEC'd 2026-08-04 in `SPEC_pivot_visibility_gauge_defect.md` §8 and unbuilt
since.  **Built as specified**, default OFF, byte-identical OFF, FINGERPRINTED when armed.

**The algebra as implemented.**  `v_c` = `NetworkSbasis::clockNullDirection` — the
STRUCTURAL direction the datum builder owns, derived from the same `by_const` grouping and
lexical sort `buildSharedDatum` pins with.  Inside the shared border it is the indicator
over the constellation's registered satellite-clock columns at unit weight: the receiver
clock/ISB that complete the gauge are LOCAL states the Schur reduction eliminates, and
**every other shared family has a ZERO component** because its gauge is realized
independently (W6 per-satellite IF pin + per-(const,signal) sum-to-zero for the code bias;
anchor-receiver ambiguity pins for the phase bias).  The §19.1 census is the measurement
behind that: at the 319260 handover the clock step is a rigid +1.8365 m while the code-bias
step MEAN is 1e-6 m and the phase-bias step mean is −0.026 m against 0.18 m of scatter —
the biases do not ride the clock frame.  Kernel `applyClockStransform` (pure, no context):
`c0 = x_q/v_c[q]`, `x -= v_c*c0`, `var += v_c[i]^2 * var_q`; cross term dropped
(conservative, §8.1 rule 2), full `S D S^T` still deferred by §8.8.

**The load-bearing half is the RELEASE (§8.4 INC-1), and most of the measured benefit is
there, not in the arithmetic.**  Only clocks are transformed; the code-bias and phase-bias
carries were never in the wrong frame — they were simply DROPPED.  The seam records the
transformed constellations in `NetworkCarryState::sts_consts` and the RTA-2
`handover_affects` skips release for exactly those: the D2 diagonal
(`NetworkRunContext.cpp:2508`) and dense (`:2625`) folds, the READMIT parked-entry drop
(`:2477`), and `NetworkStashStatus.cpp:122/:225` (the VCE monitor and the prior-residual
census `folded` bit, which must mirror the injector clause for clause or the census
mislabels exactly the rows this card fixes).  Seam call site: `NetworkSolveGlue.cpp:1012`,
immediately before `networkInjectD2CarryPrior` — the last point at which "every consumer of
the carry sees the same frame" is guaranteed by construction rather than by an audit of the
intervening seams.

**Deliberately NOT released, both documented:** the `NetworkClockQc` jump test (its
statistic is the RAW clock under a PIN datum, which genuinely steps by the datum delta —
releasing it would flag the whole constellation; the edge stays classified as FRAME, §8.4
INC-2's other permitted answer) and the two-state clock oscillator (`clock_model` is a
documented NO-GO, so transforming it would be untested code on a dead path).

**Gate 1 — OFF byte-identity: PASS, twice over.**  Fresh 2 h null pair
`runs/evseed_stsg1_{off,false}` (knob ABSENT vs knob FALSE, one exe): `products.txt` sha256
identical (`1729cf5a…`), and `network_dd_fractional` / `_events` / `_solution` /
`closed_loop_trace` / `_epoch_status` all identical.  Stronger: `runs/evseed_stsg2_off`
(this exe, knob absent) reproduces `runs/evseed_pivfresh_scoped` (increment 0's exe
`F4FF8EF0…`, source `cc689d5`) **byte-identical on `products.txt`** (`db80fcaa…`), so the
OFF path is a true no-op against the pre-card baseline across a different source revision.

**Gate 2 — actuation + the wire, fresh 643-epoch pair `runs/evseed_stsg2_{off,on}`, one
exe, single knob.**  Run-end (invariant #4): *2 clock-pivot handover(s) [E:1, G:1]; **2
edge(s) TRANSFORMED, 52 row(s) re-expressed, 376 row(s) RETAINED**, 0 no-op, **0 self-check
failure(s)**; |c0| max 1.019227 m.*  The PIVOT rows carry `sts=applied;c0=…;rows=…` (INC-2).
The Galileo edge 271230 is the clean test — first edge of the lineage, arms identical
before it:

| channel (the CHANGED constellation's own) | control | OFF | **sts** | factor |
|---|---|---|---|---|
| E CB C1C / C5Q / C7Q step rms | 0.00003 / 0.00003 / 0.00050 | 0.01083 / 0.01213 / 0.25582 | **0.00003 / 0.00002 / 0.00060** | 361x / 607x / 426x |
| E PB L1C / L5Q / L7Q step rms | 0.00203 / 0.00288 / 0.00278 | 0.55734 / 0.76102 / 0.73368 | **0.00174 / 0.00254 / 0.00242** | 320x / 300x / 303x — BELOW control |
| consumer resets PB/CB/CLK, switch epoch | — | 18 / 18 / 6 | 18 / 18 / 6 | unchanged (by design, below) |
| consumer resets, the five FOLLOWING epochs | — | 18,15,17,15,6 PB | **0,0,0,0,0** | the aftermath is gone |

GPS edge 276150 is symmetric: CB C1C 0.01294 → **0.00003**, PB L1C 0.58319 → **0.00084**
(the pre-registered bar was ≤ 0.01 — met by 12x), follow-on PB resets 19,15,21,10,7 →
**0,0,0,0,0**.  The emitted GPS clock step stays the rigid +Δ with scatter at noise
(+0.5937 ± 0.00546 → **+0.5878 ± 0.00467**, control sd 0.00494) — never per-satellite
scatter.  **§8.7 gate 6 did NOT fire**: the ON arm's step rms is flat at the edge and at
+1 / +4 / +14 / +30 min (E PB L1C 0.00174 / 0.00191 / 0.00165 / 0.00156 / 0.00152), with no
geometric divergence and 0 self-check failures.  Truth (`simnet_grade`, whole-run settled,
gate ±1 %): sat-clock contrast rms 1.6356 → **1.6356** (0.00 %), sat-clock residual 1.4053 →
1.4034, code bias 0.1445 → 0.1443 (p50 0.0468 → 0.0334), rx-DCB 1.2284 → 1.2228, rx-clock
1.8562 → 1.8482, ZWD 0.0075 → 0.0073.  **The transform does not change accuracy, only
continuity — exactly what it claims.**  On this bed the consumer also improves a lot: 3D rms
**0.1851 → 0.1070 (−42 %)**, max 0.3605 → 0.1719 (−52 %), Up rms 0.1453 → 0.0565.

**Gate 3 — ONE storm arm `runs/golden_sim_campaign_zwdblk_sts`** (= the tracked scoped arm
+ the knob, 720/720, 1303 s) vs **S = `runs/golden_sim_campaign_zwdblk_scoped`**.  c0 at the
GPS edge is **−1.839995 m** — the transform independently re-derived the exact rigid shift
§19.1 measured off the wire.

| channel | S | **sts** | gate | verdict |
|---|---|---|---|---|
| G CB C1C / C2W / C5Q step rms at 319260 (control 0.00003/0.00003/0.00034) | 0.00921 / 0.01081 / 0.16678 | **0.00002 / 0.00002 / 0.00029** | control | **PASS** 460x / 540x / 575x |
| G PB L1C / L2W step rms (control 0.00083/0.00132) | 0.19914 / 0.25947 | **0.00079 / 0.00124** | control | **PASS** — below control |
| E CB C1C / C7Q step rms at 321900 (control 0.00001/0.00031) | 0.00516 / 0.17809 | **0.00001 / 0.00024** | control | **PASS** |
| E PB L1C / L5Q step rms (control 0.00096/0.00153) | 0.40473 / 0.55652 | **0.00096 / 0.00145** | control | **PASS** |
| consumer resets at each switch epoch | 30/30/10, 27/24/8 | **unchanged** | ≤ 3 PB/CB | **FAIL as written** (see below) |
| consumer resets, five following epochs (PB) | 27,21,18,12,13 / 23,19,21,16,2 | **0,0,0,0,0 / 3,3,3,2,0** | — | aftermath gone |
| **consumer 3D at the GPS edge epoch** | 0.126 | **0.125** | ≤ 0.05 | **FAIL** |
| **consumer 3D at the Galileo edge epoch** | 0.031 | **0.036** | ≤ 0.031 | **FAIL** |
| pooled settled p50 / rms | 0.0825 / 0.0899 | **0.0807** / 0.0901 | p50 ≤ 0.0825 | **PASS** (rms flat) |
| W1 window-excluded p50 / rms | 0.0804 / 0.0853 | **0.0746 / 0.0818** | — | better |
| consumer 3.0–4.68 h p50 (pre-first-edge) | 0.0905 | **0.0905** | — | IDENTICAL (a controlled pair) |
| hourly p50 h0…h5 | .524 .113 .114 .101 .089 .047 | **.524 .113 .114 .101 .089 .042** | h0–h3 ±2 cm | **PASS** — identical h0–h4 |
| ZWD settled truth rms / span p50 / max | 0.0066 / 0.0205 / 0.0801 | **0.0059 / 0.0169 / 0.0261** | ≤ 0.0066 | **PASS** −11 % / −18 % / −67 % |
| ref-free WL / EWL / L1C | 99.2 / 100.0 / 90.2 | **99.2 / 100.0 / 90.4** | ≥ 99.2 / ≥ 90.2 | **PASS** |
| T4 events / ambient | 11 / 0.196 | **11 / 0.196** | ≤ 11 | **PASS** identical |
| W2 4.9917 h fallout | 0/0/0 | **0/0/0** | absent | **PASS** |
| G4 keeps / unanswered / spurious | 4300 / 0 / 2 | **4300 / 0 / 2** | — | identical |
| split-referenced mean / max | 0.0 / 0 | **0.0 / 0** | 0/0 | **PASS** |
| PHASE_ANCHOR handovers / cold | 315 / 78 | **315 / 78** | ≤ 78 | **PASS** identical |
| sat-clock contrast / code-bias rms / rx-DCB / rx-clock | 0.7657 / 0.0902 / 1.3022 / 1.4213 | **0.7665 / 0.0916 / 1.3020 / 1.4251** | ±3 % | **PASS** (+0.10 / +1.55 / −0.02 / +0.27 %) |
| run time | 1242 s | 1303 s | — | +4.9 % |

**Isolation (the §19.5 list, re-run).**  Every event-kind total is **IDENTICAL**:
`common_band` 5413, `if_slip` 4747, `mw_wl_slip` 2539, `lli_slip` 1205, `anchor_handover`
483, `handover_continuous` 362, `anchor_slip_handover` 315, `cta_recouple` 103,
`handover_cold` 100, `col_removed` 63, `col_added` 56, `cta_disc_landed` 52, `cta_decouple`
52, `seed_held` 31.  The CLK-R, PB-GAUGE and READMIT run-end clauses are byte-identical.
The only counters that move are the new `sts_*` ones.  **The knob touches the handover
carry response and nothing else.**

**Why the consumer gates fail, and what it means.**  They were written expecting the
transform to remove a consumer shock that **increment 0 had already removed**: on the
scoped baseline the GPS-edge epoch is 0.126 m against 0.129 m the epoch BEFORE it — there is
no shock left to take out, and a bar of ≤ 0.05 m asked for an improvement on an already-flat
channel.  The two switch-epoch reset counts are unchanged for a reason §8.2 states
explicitly: **the emitted wire still steps by the datum delta BY DESIGN and the disc
channels still describe it** — the card "does not change the emitted datum, the wire, or the
disc-channel contract".  What the transform removes is the AFTERMATH, and that it removes
completely (72 and 74 follow-on PB resets → 0 and 8).

**The newly-visible follow-on defect, named.**  With the transform armed the producer still
declares 30 PB + 30 CB discontinuities at the switch for biases whose step rms is now
0.0008 m — i.e. for keys that did NOT move.  They are collateral of the CLOCK disc:
`cta_disc_landed` marks every decoupled clock satellite and `markSatDiscontinuity` bumps
that satellite's phase, code AND clock counters together.  That is an EMISSION question,
explicitly out of scope for §8, and it is the natural **increment 2**: split the
per-satellite disc bump by family so a clock-only re-reference stops resetting the
consumer's ambiguities.  Expected prize: the switch-epoch 30/30 → 0/0.

**Verdict as reported (2026-09-11 night).**  Every WIRE and QUALITY gate passes and nothing
regressed beyond noise; the two consumer-facing gates fail because their target had already
been collected.  The worker's recommendation was KEEP_OFF pending a decision, on the card's
own all-gates-or-nothing rule.

**DECISION — PROMOTE (2026-09-12, manager).**  `network.handover_stransform` is
**RECOMMEND_ON** in regime `on("network.scoped_handover_response") and not
on("network.clock_datum_sum_to_zero") and not on("network.clock_datum_broadcast_anchored")`
— the same exclusions as the scoped row, with NO `n_sys >= 2` term, because the transform
acts on the CHANGED constellation's own carry and one constellation is therefore enough for
it to do work.  `fingerprinted: true`.  Pinned in `golden_sim_tri_w6`; a documented
exception in `golden_field_gps_dual`.  The reasoning, for the record:

- Every wire gate passes at **250–575× back to control**, `OFF` is **byte-identical across
  two source revisions**, **§8.7 gate 6 never fired**, all producer truth channels move
  within **±1.6 %** of a ±3 % bar, and the pooled consumer p50, the window-excluded p50, ZWD
  and L1C are all better or equal with the hourly profile identical.  **No channel is worse
  beyond noise.**
- **The two consumer EDGE gates were MIS-SPECIFIED by the manager, not missed by the card.**
  They demanded an improvement AT the switch epoch, which increment 0 had already flattened:
  on the scoped baseline the epoch before the GPS edge reads 0.129 m and the edge epoch
  itself 0.126 m, so the bar asked for a gain on a channel with nothing left in it.
- The unchanged switch-epoch reset COUNT is an **emission property, not a carry property**:
  the per-satellite disc bump marks phase, code and clock together, so a clock-only
  re-reference still resets the consumer's ambiguities.  §8 states explicitly that the
  emitted wire still steps by the datum delta BY DESIGN and does not touch the disc-channel
  contract — that work is **increment 2**, pre-registered below.

Sim-validated on two beds (quiet 643-epoch fresh pair, slipped 720-epoch storm arm), single
knob, one exe.  **Field arm owed** — and owed *behind* its prerequisite: the field golden is
GPS-only and `scoped_handover_response` is itself a field exception there, so the regime
predicate is false on that lineage and the knob is out of regime rather than suppressed.

**INCREMENT 2, pre-registered (before any code is written).**  *Split the per-satellite
discontinuity bump by family, so a transformed clock switch declares CLK discs only.*  Today
`cta_disc_landed` marks every decoupled clock satellite and `markSatDiscontinuity` bumps that
satellite's phase, code AND clock counters together; with the transform armed the producer
therefore still declares 30 PB + 30 CB discontinuities at the GPS edge for biases whose step
rms is now **0.0008 m** — keys that provably did not move.  Target: switch-epoch consumer
resets **30/30/10 → 0/0/10** (the clock notes stay; the receiver's white-noise clock absorbs
the common mode for free, `RealTimeEngineSsrBreaks.cpp:290-292`).  Gates: OFF byte-identical;
the R record and E record still name the satellites whose CLOCK re-referenced; PB/CB disc
counters unchanged on every satellite whose bias actually stepped (so a genuine bias
discontinuity is never suppressed — the failure mode this card must not create); consumer 3D
at the edge epoch not worse; and the §16.8 table not worse.  ⚠ This is the first edit in the
card that touches the **emitted disc-channel contract**, so it needs a consumer-replay arm as
well as a producer arm.

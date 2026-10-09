# RPA: the post-fit RAIM pool restricted to data rows

**STATUS**

- **id:** RPA
- **topic:** network-vehicle
- **status:** closed
- **summary:** KEEP_OFF, FAILED G3. Admitting observation rows only to the RAIM pools catches masked spikes (KIT300UZB) but removes the only check that stopped a G07 clock runaway after a slip at BSHM00ISR: clock wire +69 % / +60 %, census 0 better / 1 worse. The prior rows need their own test channel, and the runaway its own response (section 4).
- **source:** card [NOM](NOM_network_object_model_2026-10-08.md) section 16 (the confirmed defect, its replay and its numbers); [RF-3](RF-3_code_row_qc_2026-09-22.md) (the T4 counts on the re-founded posture); the RAIM cards in `docs/cards/raim-qc/`
- **knobs / posture:** new `network.raim_pool_data_rows_only` (default off, fingerprint-safe like the other QC knobs: it moves QC decisions, not the state layout); the goldens as they are
- **dates in the text:** 2026-10-08
- **hashes / run dirs:** branch `rpa/knob` `8e98646`, landed `9dbf0b8`; exe `gnss_processor_rpa1.exe` (`3a84a0c3`); runs `runs/_rap_rpa1_{ref,off,on}_{f2,s2}`, `runs/_rap_rpa_{on,off}_{f6,s6,s6ds}`, census `runs/_rap_rpac_{on,off}_<site>`, diagnostic `runs/_rap_rpad_{on,off}_arht_x07`
- **gates (frozen at pre-registration):** section 3

**Outcome.** Question: should the RAIM pools admit observation rows only? No. The knob catches masked observation spikes and removes refloats that cost nothing, but at BSHM00ISR the carried-prior rows were the only check of a G07 clock runaway after a slip (-1.6 m per epoch, absorbed by the clock); without their refloats it reaches -11.7 m and the clock wire worsens 60 to 69 %. G3 FAILS, G4 passes on the consumer; reading R-b. KEEP_OFF; the prior rows need their own test and the runaway its own response (section 4).

## 1. The question

The post-fit RAIM pool (`NetworkRaim.cpp` around 329 to 333) and the within-epoch tier (`NetworkSolveGlue.cpp` around 1601) admit a row of a station's local block to an ambiguity column's pool when its coefficient on that column exceeds 0.5. The test was written for phase observation rows. On the float vehicle the station block also holds whitened carried-prior rows (the ambiguity block, the receiver code bias in block) whose coefficients clear it, and one such row enters the pool of every column it clears. Card NOM section 16 replayed the T4 isolated-arc criterion on the 2 h and 6 h field goldens: the producer's own table reproduces every emitted refloat (12 of 12, 26 of 26); a table of observation rows only keeps 1 of 12 and 2 of 26, and flags real observation spikes the producer missed (KIT300UZB, TOW 263670 to 263730). A refloat drops the arc's prior, so each one changes every later solve and the product.

Two readings, pre-registered:

- **R-a (the defect reading):** the prior rows are not observations and their residuals do not test the arc; restricting the pool to observation rows removes refloats that cost information and catches spikes the prior rows mask. Prediction: the consumer and the census are equal or better, the slip-free sim moves little.
- **R-b (the carry-check reading):** a large whitened prior residual is itself evidence of a carry inconsistency (BSHM00ISR is a known defective station), and the refloats it triggers are useful. Prediction: removing them costs the consumer at the stations they fired on, unless another tier catches the same carries.

## 2. The change

`network.raim_pool_data_rows_only`: with it on, both pools admit only the rows card NOM's row tags (step N2) declare as phase observations, each in the pool of its own ambiguity column. The carried-prior rows keep their place on the prior side of the prior-residual census, attributed to their own column (`cpl_row_local_cols`). Off is byte-identical (the declared rule of step N2b reproduces today's admission).

## 3. Gates (frozen)

- **G0 (off identity).** Off against the pre-change build: products, consumer trace and epoch status identical on 2 h arms of both goldens.
- **G1 (mechanism, invariant 4).** On the 2 h field golden with the knob on: the pool's admitted rows per kind (observation 100 %, prior 0), the T4 refloat count, and each refloat's cell named with its observation-row residual.
- **G2 (the events).** The two ready-made events, read before any aggregate: BSHM00ISR TOW 259470 to 259590 (refloats with no observation spike: are they gone, and does the station's later solution move?) and KIT300UZB TOW 263670 to 263730 (observation spikes now flagged: are the right arcs refloated?).
- **G3 (field, 6 h, adoption grade).** WTZZ00DEU 3D p50 by hour not worse than the golden by more than 0.010 m in any hour; the 16-site census (`scripts/clock_level/gcensus.py`) paired change over hours 2 to 6: a median at or below 0 and no more sites worse than better; hour 1 reported; the field clock wire (`scripts/ftc_step_attrib.py --r3`) not worse than the golden's by more than 10 %.
- **G4 (sim, 6 h from 00:30:15).** The consumer within 0.010 m of the golden in every hour, and the slipped sim (`rapid_arm.py` RINEX op `dropslip`) with the injected slips still answered (the phase-only tiers' counters non-zero on the injected arcs).
- **G5 (multi-day).** If G3 passes: DOY 130 and 131 (card MDR's data) with the census, so a one-day result is not adopted.

Arming is the author's decision, through a ledger row and the golden generator.

<!-- Results are appended below this line under dated headings. -->

## 4. 2026-10-09: result (built and graded by workers; the decisive numbers re-graded by the manager)

Built on branch `rpa/knob` (`8e98646`, landed as `9dbf0b8` after a rebase over documentation commits), exe `gnss_processor_rpa1.exe` (`3a84a0c3`). With the knob on, both pools admit the observation phase rows only, each to the pool of the ambiguity column its tag names (`raimPoolAdmitsRow`); the own-column check found 0 disagreements with the coefficient test on every arm. Unit suite 2 949 run, 2 925 passed, 24 skipped, 0 failed (`RaimPoolDataRowsOnly`, 6 cases).

| gate | measured | verdict |
| --- | --- | --- |
| G0 | off against main's build on 2 h field and sim goldens: products, consumer trace, epoch status and every CSV identical (timing columns aside) | PASS |
| G1 | 2 h field golden, on: the pools take 203 446 observation rows and refuse 1 653 864 ambiguity block and 295 004 receiver code-bias block visits; T4 flags 12 off, 16 on; T3 64 to 47; arcs dropped 21 to 32. Each fired cell's value now equals its largest observation residual | read |
| G2, KIT300UZB TOW 263670 to 263730 | on: the four largest observation spikes (G21 132.4, G08 40.9, G19 37.6, G02 30.1) are flagged and refloated at 263670, two epochs earlier than the off arm's single G21 refloat; the station is clean at 263700. G02, G08 and G19 may be collateral (the off arm cleared the station with G21 alone) | for R-a |
| G2, BSHM00ISR TOW 259470 to 259590 | G07 slipped at BSHM00ISR at 259410 and the emitted G07 clock correction then drifts about -1.6 m per epoch in BOTH arms. Off, the prior-driven refloats at 259470 stop it at -2.98 m and it recovers. On, no observation row at any of the 28 stations seeing G07 exceeds 4.92 (the clock absorbs the error), the refloats do not happen, and the drift reaches -11.67 m at 259620, ending only when G07 leaves BSHM00ISR. On minus off, the G07 C record: -9.60 m at 259620, -1.39 m at 262800, -0.86 m at the run end | for R-b |
| G3, field 6 h | WTZZ00DEU 0.1129 0.0229 0.0164 0.0197 0.0299 0.0263 m against 0.1076 0.0238 0.0163 0.0198 0.0300 0.0263 (PASS); clock wire (`ftc_step_attrib.py --r3`, e_clk, hours 2 to 6) 0.0365 / 0.0298 m against 0.0216 / 0.0186 m all / tracked, +69 / +60 % (bar 10 %: FAIL); census hours 2 to 6 median +0.0001 m, 0 better and 1 worse (ARHT00ATA +0.086 m; with G07 excluded from its replay the on wire is 0.032 m better there, so the loss is G07's residue), hour 1 median +0.0007 m (6 better, 5 worse): FAIL on both census bars; T4 26 to 17, the off arm's 13 prior-driven refloats in hours 3 to 6 gone with no measurable cost | FAIL |
| G4, sim 6 h from 00:30:15 | slip-free: products and consumer identical (0 T3, 0 T4 either way). Slipped (`dropslip`, 493 returns): consumer within 0.007 m every hour (PASS); the gap re-entry, band-scope and MW responses identical (796 / 844 / 796 / 94), but T3 and T4 go from 334 and 314 flags to 0 and 0. Off, T4 fired on 203 injected returns, every one a second response to an arc the gap re-entry tier had already answered | PASS on the consumer; the slip clause depends on whether "the phase-only tiers" includes T3 and T4 |
| G5 | not run (G3 failed) | |

**Reading.** R-b is favoured. The carried-prior rows are noise for the arc test at most stations and most epochs (R-a holds locally: KIT300UZB, the 13 removed refloats, the redundant second responses on the slipped sim), but at BSHM00ISR they are the only instrument that sees a satellite clock running away after a slip, because the clock absorbs the error and every observation row stays small. Removing them removes the one brake on that runaway, and the network's clock product carries the residue for hours. The knob stays default off; ledger verdict KEEP_OFF.

**What this points to.** Two defects, not one. (1) The prior rows should be tested, but on their own channel and against their own column (the row's `cpl_row_local_cols`), so that a large prior residual triggers a response to the carry it belongs to without being spread over every column it touches and without diluting the observation pool. (2) A slip at one station drives a satellite clock runaway of metres per epoch network-wide, and the clock step test marks it (6 marks on G07) without stopping it: the response to that edge is missing (standing invariant 1). Both are cards of their own; neither is armed or built here.

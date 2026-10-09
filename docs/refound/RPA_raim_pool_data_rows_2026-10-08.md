# RPA: the post-fit RAIM pool restricted to data rows

**STATUS**

- **id:** RPA
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered. The post-fit RAIM pool admits every row whose coefficient on an ambiguity column exceeds 0.5, so the float vehicle's whitened carried-prior rows enter it as phase data and decide most T4 isolated-arc refloats on the golden. The card tests admitting observation rows only, behind a default-off knob, on both beds with the census. Built after card NOM step N2b.
- **source:** card [NOM](NOM_network_object_model_2026-10-08.md) section 16 (the confirmed defect, its replay and its numbers); [RF-3](RF-3_code_row_qc_2026-09-22.md) (the T4 counts on the re-founded posture); the RAIM cards in `docs/cards/raim-qc/`
- **knobs / posture:** new `network.raim_pool_data_rows_only` (default off, fingerprint-safe like the other QC knobs: it moves QC decisions, not the state layout); the goldens as they are
- **dates in the text:** 2026-10-08
- **hashes / run dirs:** none yet
- **gates (frozen at pre-registration):** section 3

**Outcome.** Pre-registered; no result yet.

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

# THI: fixed thresholds in the decision paths, the inventory (2026-11-19)

**STATUS**

- **id:** THI
- **topic:** tooling
- **status:** open
- **summary:** An inventory, with no build and no engine run, of the fixed thresholds in the decision paths of the network engine (CHAIN e7bc098) and the user engine (MAIN 5ddc422): 622 thresholds, 328 live in the adopted configuration, 436 (70 %) with no source found, 122 live and absolute (metres, cycles, degrees); 45 live rows decide or hold integers, 26 of them with no source. Each row names the class of error its statistic can see and the data-driven form, or why none is feasible. The second stage (the skeptic and the writer of the data-driven forms) is not released.
- **source:** package `tooling-threshold-inventory` of the day program of 2026-11-19 (`D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/tooling-threshold-inventory/`: `INVENTORY.md`, `CHECKPOINT_1.md`, `BRIEF.md`, the full table `inventory_annotated.csv`, parts and scripts); the author's preference of 2026-11-19 for data-driven thresholds where feasible.
- **filed:** 2026-11-21, after the fact, from the package named under source.

**Outcome.** Of 622 fixed thresholds in the decision paths, 328 are live in the adopted configuration and 238 of those have no source; 122 live ones are absolute numbers, and only 34 are scaled by a formal or an empirical sigma. The class of error a statistic can see comes first: the simulator's wrong holds (near-null steps on Galileo E1/E5a, a 5.6 mm footprint) and the observed one-signed holds are different classes, and no single residual test sees both; a third frequency or the slant ionosphere step at a hold change can see the near-null class. Open: the second stage (data-driven forms for the live integer rows).

## Filing note (2026-11-21)

The sections below are the package's `INVENTORY.md` and `CHECKPOINT_1.md`, unchanged apart from heading levels. The
brief that set the package up (`BRIEF.md`) and the 622-row table stay in the package folder.

## INVENTORY.md

### Fixed thresholds in the decision paths: the inventory (2026-11-19)

Package tooling-threshold-inventory. Code read at CHAIN e7bc098 (network engine, worktree `chain`, branch
`chain/audit`) and MAIN 5ddc422 (user engine). No build, no engine run. The full table is
`inventory_annotated.csv` (622 rows). Each row gives the value and unit, where the value is applied (path:line), its
scaling, its source or "no source found", the class of error its statistic can see, and the data-driven form or the
reason none is feasible. Rows with measured evidence also give the ten-times test and a pointer to the evidence.

#### Summary table

| Area | Thresholds | Live in the reference | No source (all) | No source (live) | Live and absolute |
| --- | --- | --- | --- | --- | --- |
| Ring fix and chain | 96 | 32 | 29 | 11 | 10 |
| Slip detectors, RAIM, screens | 280 | 164 | 243 | 132 | 40 |
| Carried quantities, admissions | 152 | 74 | 92 | 42 | 41 |
| User engine | 94 | 58 | 72 | 53 | 31 |
| **Total** | **622** | **328** | **436 (70 %)** | **238 (73 % of live)** | **122** |

- **Live** means the threshold's key is on, or it has no key, in the adopted network arm (`_rap_ad_n146/arm.json`) or the fixing user arm. 294 rows sit behind keys that are off in the reference configuration.
- **Scaling of the 328 live rows:**
  - 122 are absolute numbers (metres, cycles, degrees);
  - 20 are scaled by a formal sigma and 14 by an empirical scale;
  - 20 mix a scaled test with a fixed floor;
  - the rest are counts, windows and shares with no statistic.
- **Integer decisions and holds:** 45 live rows, 26 of them with no source.
- **Measured evidence:** 48 rows carry hand-written entries from the two analyses. Every other row has a rule-based entry, marked "(rule)" in the table.

#### What the statistics can see

The class of error a statistic can see comes before the question of a data-driven value. The simulator's wrong holds and the observed one-signed holds are different classes, and no single residual test sees both.

1. **Near-null (3, 4) steps on Galileo E1/E5a.**
   - On the three simulator beds with truth these are every wrong hold. Their ionosphere-free footprint is 5.6 mm, about 2.3 robust scales of a clean held residual, so no threshold on a held link's residual sees them.
   - The fraction bound and the odds rule let them through at the decision, because the float sits near the wrong integer. On the simulator the wrong-decision rate rises with log-odds: the wrong decisions have the smallest formal sigma.
   - The near-null code guard does not separate right from wrong, with either a fixed or a class-scaled level (`parts/analyst_b.md`, section "The near-null class").
   - Statistics that can see the step:
     - a third frequency: the E1/E5a/E5b geometry-free ionosphere-free phase carries 8.5 m per step, about 240 sigma in one epoch;
     - the slant ionosphere step at a hold change;
     - the block-Baarda report, which flagged both off holds of DOY 146 (quoted from the brief, not re-checked here).
   - `network.ring_fix.nn_monitor` is off in the adopted day runs (loader default false at CHAIN GnssConfigLoadNetwork.cpp:1640; ledger verdict TRACE; no adopted arm.json sets it). It is armed only in `_rap_nnm44_2h`.
2. **Narrow-lane one-cycle holds on observed data** (38 one-signed holds of 12 to 18 mm in the six adopted runs).
   - The monitor of 0.03 m on 4 consecutive epochs releases 2 of the 38.
   - A sequential test scaled by the receiver class catches them (item 1 of the next table).
3. **Whole-cycle wide-lane code offsets at the user** (the wrong UTQI holds).
   - The wide-lane admission sees only the fractional part, and the ratio test sees only near ties.
   - The one statistic that sees these holds is an independent float's misfit (UE-14 read on the float sibling).
4. **Directions held only by priors.**
   - The receiver geometry-free direction rests on the 10 m DCB prior (CQ-83). That number cannot be data-driven, because the data carry no information on that direction.
   - Tests on the reduced shared normals (the rank gate CQ-107, the S-basis rows CQ-108 to CQ-112, the estimability dump) are blind to station-local directions by construction (systems-rank-reference).

#### Measured thresholds

Where the fixed value sits, the ten-times test (would it be reasonable for a receiver class with ten times the noise),
and the data-driven form. Network rows are from `parts/analyst_ringfix.md` (A) and `parts/analyst_b.md` (B); the user
rows are from B. Levels for the ten-times verdict were written before computing, in each part file.

| # | Threshold (id) | Where it sits | 10x | Data-driven form |
| --- | --- | --- | --- | --- |
| 1 | Held-link monitor 0.03 m, 4 epochs (RF-14, RF-15; RingFix.hpp:81, :83; RingFix.cpp:471-477) | 12.6 robust scales of the clean held residual (2.39 mm); 1.21 releases per 1000 held link-hours, 102 of 104 on holds with no other evidence against them; catches 2 of 38 one-signed holds | fails: 84 % of clean holds released | CUSUM over the hourly robust scale of the same receiver class and system (other links), reference half the one-cycle footprint: 38 of 38 at the same false rate, 0 of 15 411 right simulator holds (A) |
| 2 | Gate fraction 0.20 cycle (RF-02) | decided frac median 0.056; refuses 16.6 per candidate link-hour, 58 % of which the odds would accept | harmless (the odds refuse first) | frac / sigma: +33 % right decisions at the same wrong rate on the simulator (A) |
| 3 | Gate sigma 0.10 cycle (RF-01) | inert under gate odds; 69 % of decisions exceed it | n/a | none needed (A) |
| 4 | Log-odds ln(1e6) and sigma_scale 1 (RF-03, RF-04) | formal-scaled; wrong rate rises with odds on the simulator | passes in form | sigma_scale from the class's empirical-to-formal ratio: 1.0 to 1.3 clean, 3.4 to 4.0 Galileo with code errors (A, B) |
| 5 | Wide-lane fraction 0.25 (RF-05) | about 2 formal sigma; refused rows z median 3.4 to 4.5 | fails: 80 % refused | frac_wl / (r sigma_wl), level near 3 (B) |
| 6 | Chain certification scale 3.0 (CQ-104) | needed 1.0 to 1.3 clean, about 4 Galileo with code errors | fails if sigma does not track | per-family empirical-to-formal ratio of later-verified chords (B) |
| 7 | Persistence 10, window 600 s, gap 45 s (RF-09, RF-07, RF-08) | persistence binds at 10 in 96 %; the gap sits between 30 s and 10 min pauses | n/a | times or effective sample sizes tied to the epoch interval (A) |
| 8 | Near-null guard 0.30 m, 60 epochs (RF-22, RF-21) | right and wrong chords overlap | fails in purpose | none feasible from the code signature (B) |
| 9 | GF arc jump floor 0.04 m, k 6 (SR-135, SR-134) | floor sets 75 to 78 % of scaled bars | passes in form | floor as a multiple of the class's GF step scale by elevation (B) |
| 10 | IF-TD floor 1.0 lambda_IF, k 8 (SR-109, SR-108) | floor sets 94.6 % of bars, 32 scales | fails | k on s_eff only, about 11 for 1e-3 per clean row (B) |
| 11 | RAIM k 6, sigma0 floor 1.0, trigger 3, withhold 6 (SR-158/159/171/172) | sigma0-hat at its floor on 100 % of epochs (unfloored about 0.22); trigger and withhold never reached | fails (floor binds) | no floor, or a floor from the measured sigma0-hat (B) |
| 12 | Gross-LOS cold bound 12 m (SR-191) | 68 class scales (0.18 m) | fails on rate | k times the class's cold code scale (B) |
| 13 | Clock anchor k 6 and floors (CQ-63 to CQ-66) | phase floor binds on 99.6 %: a fixed 0.12 m; 0 trips in 1.03 M rows | fixed in effect | floor from the innovation's measured spread (B) |
| 14 | ZWD event inflation 0.02 m (CQ-24) | 8.7 scales of the event step (2.3 mm) | fails | k times the class's ZWD event step (B) |
| 15 | User ratio 5 and upper bound 1e4 (UE-01, UE-02) | the upper bound refuses 77 % of attempts (near-singular Q from held satellites, no error class); wrong admissions at ratio 18 to 5 248 | UE-01 fails with formal fixed | covariance scaled by the class ratio; the upper bound replaced by a conditioning test or held candidates left out of the search (B) |
| 16 | User narrow-lane cap 0.15 and floor 0.04 (UE-04, UE-13) | cap is 2.2 to 2.7 scales today; floor 0.73 to 0.95 empirical scales | fail | z = frac / sig_k > 2.5; floor from the class's narrow-lane scale (B) |
| 17 | User wide-lane 0.40 with 3 sigma (UE-08, UE-09) | admissions pile at the bar; formal sigma_w optimistic by 1.22 to 2.42 | fails | sigma_w times the class ratio: refuses both wrong UTQI admissions, defers 77 to 89 % of right first admissions (B) |
| 18 | User position sigma 0.05 m (UE-07) | 38 converged medians | borderline | a bound in narrow-lane wavelengths (B) |
| 19 | User hold release 0.25 (UE-14) | never fires as coded (it reads the pulled state) | fails | an independent float's misfit over the class scale: 6 of 6 wrong caught early, 1 to 10 % of right released (B) |

Not computable from existing traces: UE-44 (user residuals blank), CQ-102 and RF-29 (offset before the keep not
traced), CQ-105 (conditional sigma not traced), the re-attach and cascade bounds (off; refused choices not traced),
SR-129 to SR-132 (the gap table was not built in time).

#### Cautions that apply to every conversion

- A scale estimated from the tested link's own residuals grows with the fault. Candidate (iii) of A, the link's own trailing scale, catches 28 of 38 against 38 of 38 for the network class scale. The scale must come from other links of the class.
- Formal sigmas collapse on the simulator. On observed data the formal wide-lane sigma is optimistic by 1.2 to 2.4 at the user. Every formal-sigma form needs a class ratio.
- The simulator gives no calibration for a residual monitor. Its near-null wrong holds look like right holds, and its residual scale (4.9 mm) and serial correlation (0.63 to 0.86) differ from observed (2.4 mm, 0.37 to 0.42).
- The catch of 38 of 38 rests on one-signed holds that were selected for being steady offsets. Only 5 of them are confirmed cycle errors.
- A count over a threshold misleads on a constant offset: the 4-epoch count never accumulates a steady 12 to 18 mm.
- Adaptive ionosphere process noise was tried and falsified (`network.adaptive_iono_q`, deleted by RKD at commit 1d35585). The deweight release re-coupled parked errors (CHAIN `docs/record/SPEC_deweight_release_hygiene.md`).
- A fixed floor under an empirical scale is where the fixed number hides. In every case measured, the floor sets the bar most of the time.

#### Decisions for the author

1. **Convert the held-link monitor first, as a measure-only trace, then as a default-off release.** The form is a CUSUM over the network receiver-class scale. Recommendation: yes. It is the case behind the question, it is invariant to a noisier receiver class, and today's rule releases 102 holds with no evidence against them. Calibration: the simulator cannot calibrate it, so the observed clean false-trip rate and the cross-run evidence of the 42 one-signed holds stand in for truth.
2. **Remove the fixed floors that bind most of the time** (IF-TD 1.0 lambda_IF, RAIM sigma0 floor 1.0, clock-anchor phase floor 0.02 m, GF arc jump 0.04 m) in favour of class-scaled floors. Recommendation: one default-off key per floor, judged against today's flag rates. The RAIM floor first, because with sigma0-hat at about 0.22 the trigger and withhold can never act.
3. **Scale the formal covariance of the decisions by a class ratio** (ring-fix sigma_scale, chain certification 3.0, user wide-lane sigma_w). Recommendation: measure the ratio on observed data from later-verified holds before any change, since the simulator ratio depends on the code-error model.
4. **The near-null class needs a new statistic, not a new level.** Recommendation: the third frequency and the slant ionosphere step at a hold change. Arm `network.ring_fix.nn_monitor` only as a trace; the code-only corrector and release are already falsified.
5. **Add the missing trace columns** so that the remaining live thresholds can be graded: the user standardized residual, the offset of a re-host before the keep, the chain's conditional sigma, and the re-attach choices. Recommendation: yes, measure-only, no output change.
6. **Waiting for the managing session's release (budget notice, 13:20):** the skeptic's recomputation of the five highest-ranked distributions and the design document with three ranked conversions and their calibration plans. Both are scoped and not started.

#### Files

- `inventory_annotated.csv`: the inventory with the class seen, the data-driven form, the ten-times verdict and the evidence.
- `inventory_thresholds.csv`: the four readers' rows merged.
- `parts/inventory_<area>.csv`, `parts/reader_<area>.md`: the readers' rows and notes.
- `parts/analyst_ringfix.md`, `parts/analyst_b.md` (with `parts/analyst_b_results.json`): the evidence, each with its rules written before computing.
- `tables/ringfix_*.csv`, `tables/b_*.csv`: the distribution tables.
- `scripts/*.py`, `scripts/COMMANDS.md`: the scripts that make them, including `merge_inventory.py` and `annotate_inventory.py`.
- `CHECKPOINT_1.md`, `BRIEF.md`.


## CHECKPOINT_1.md

### Checkpoint 1: the inventory (2026-11-19)

**Result.** 622 fixed thresholds in the decision paths: 96 in the ring fix and chain, 280 in slip, RAIM and screens,
152 in the carried quantities and admissions, 94 in the user engine. 436 of the 622 (70 %) have no source in the record;
by area 29 of 96, 243 of 280, 92 of 152, 72 of 94. 288 sit behind keys that are off in the reference configuration.

**File.** `inventory_thresholds.csv` (merged by `scripts/merge_inventory.py` from `parts/inventory_<area>.csv`;
reader notes in `parts/reader_<area>.md`). Code read at CHAIN e7bc098 (network) and MAIN 5ddc422 (user engine).

**Scaling.** 237 rows are absolute numbers in physical units or cycles, 38 are scaled by a formal sigma, 33 by an
empirical scale, 26 mixed, 288 are counts or windows with no statistic.

**Integer decisions and holds.** 52 integer-decision rows and 29 hold-release rows. Of those that are live in the
reference configuration, 45 in all, 26 have no source and 17 are absolute.

**Base for "no source found".** Each such row records two searches (the name, and the value with its unit) in
`source_note`. For short values (0.5, 6, 10) the value search matches many unrelated lines, so those verdicts rest
mainly on the name search. Sources were read in full for live rows. For several inert rows the source is the ledger
note naming a record file.

**Corrections found on the way.**
- The network keys load in CHAIN `src/core/config/GnssConfigLoadNetwork.cpp`, not `GnssConfigLoad.cpp`.
- `network.ring_fix.nn_monitor` has default false (GnssConfigLoadNetwork.cpp:1640), ledger verdict TRACE, and no
  adopted arm.json sets it. The adopted day runs therefore carry no near-null monitor output.
- The gate's sigma bound 0.10 (RingFix.hpp:68) enters no decision of the reference. The gate is `odds`
  (RingFix.cpp:515-518), and 69 % of observed decisions have sigma_if above 0.10.
- The monitor's 0.03 m has no source. It is stated without measurement in NRF section 4.3 item 5 and section 7 item 5.

**Stage 2 so far** (analyst A, `parts/analyst_ringfix.md`).
- Spread of the clean observed held residual: 2.39 mm (robust scale), so 0.03 m is 12.6 scales.
- Ten-times test: fails. At ten times the noise the rule releases 84 % of clean holds.
- Present catch: 2 of 38 one-signed holds of 12 to 18 mm. The other 102 of its 104 releases end holds that nothing
  else marks as wrong.
- A CUSUM scaled by the receiver class's hourly robust scale, at the same false rate, catches 38 of 38 and trips 0 of
  15 411 right simulator holds. It does not change at ten times the noise.
- Analyst B (user engine, slip, screens, carried quantities, the near-null class) is running.

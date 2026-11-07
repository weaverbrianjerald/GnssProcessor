# SRN: a scientific review of the network processing engine

**STATUS**

- **id:** SRN
- **topic:** network-vehicle
- **status:** closed
- **summary:** Closed 2026-11-07. An independent scientific review of the network producer at main `adef08b` on the golden posture (GPS L1/L2 + Galileo E1/E5a, float vehicle, free slant ionosphere, Q3 clock anchor) along nine dimensions: 87 reviewer findings (Fable, 3.99 M tokens), 39 verified by two Opus referees each (9.45 M tokens), 15 same-executable arms. Verdict: the estimation core is sound (exact solve, exact factored recursion, minimal S-basis), and most departures are inert for the float user. Material: the code weight is 2.4 times pessimistic in sigma and every quality-control threshold reads that scale (the within-epoch tier adapted nothing in 1,440 epochs; the anchor guard's deciding statistic is floor-governed at 0.12 m); a system-blind companion test divides 8,051 Galileo C1C rows by 100; the emission hold ties both instruments to the a priori clock within a generation; the headline user is unrepresentative and convergence is never graded. The wet delay has no reachable time link and the standard-atmosphere troposphere drives Antarctic wet delays negative (GPT3 removes it, user unchanged). One finding refuted at verification (BSHM00ISR's code channel: three short episodes the screen already handled). Six builds started in their own worktrees with gates (section 2.5).
- **source:** main `adef08b` (worktree `D:/GnssProcessorSupport/worktrees/scirev`, branch `review/network-engine`); the baseline pair `runs/_rf0hv17_{field,sim}6` (card BOF, exe `gnss_processor_bofarm.exe`, sha256[:16] `91143c81de9a9547`); the goldens `config/golden/golden_{field,sim}_refound.json`; the ledger `docs/CONFIG_promotion_ledger.json`
- **dates in the text:** 2026-11-06
- **knobs / posture:** the goldens as generated; nothing armed or changed by this card
- **hashes / run dirs:** main `adef08b`; `runs/_rf0hv17_field6`, `runs/_rf0hv17_sim6`

**Outcome.** Question: where does the network producer, as armed on the goldens, depart from estimation theory and from the PPP-RTK literature, which departures are material, and which experiments settle the open ones? Answer, after two-model verification: the algebra is conformant and most departures are inert for the float user. The material residue: the code weight scale and the quality-control thresholds that read it; a system-blind companion weight; the dependence of both grading instruments on the a priori clock within a hold generation; a headline user that is the best-served point of the roster, graded without a convergence statistic. The wet delay has no reachable time link and the a priori troposphere fails at the poles; both have builds with gates, started.

## 1. Pre-registration (2026-11-06)

### 1.1 The object under review

The network producer: `runNetwork` and the eight stages of one epoch (`docs/onboarding/07_network_tour.md`), the shared observation equation (`include/measurement/ObservationEquation.hpp`), the station parameter set and its declared dispositions (`include/network/assembly/ParamInventory.hpp`), the realized S-basis (`docs/cards/network-vehicle/SBASIS-CONTRACT.md`), the carry (`network/carry/`, `network/solve/NormalEquations`), the quality-control tiers (`network/qc/`), the slip layer (`network/slip/`), the clock anchor and the emission (`network/solve/ClockAnchor`, `network/emit/`), and the product grammar (`products/NetworkProductIO`). The posture is the pair of goldens as generated at `adef08b`; the telemetry is the baseline pair of card BOF.

### 1.2 The nine dimensions

| # | dimension | the questions |
| --- | --- | --- |
| D1 | functional model and physical corrections | what the equation models, which standard corrections are absent or approximated at the millimetre level, which conventions are verified by tests |
| D2 | stochastic model and variance components | row weights, the a priori variance factor against the post-fit sigma0, the carried covariance's consistency, the URA |
| D3 | parameterization and the S-basis | every rank deficiency of the two-constellation uncombined model against its realized constraint; minimality; the held basis and its handovers; what the products mean |
| D4 | temporal recursion and the carry | the exact information-form recursion against the code's border prior, conditional station prior, exact time update, births, departures and pins; what the tests prove |
| D5 | atmosphere | the white wet delay and the white slant ionosphere: cost, the correct time link, the absent ionosphere product |
| D6 | quality control | which residual cofactor each test uses, whether anything adapts on the goldens, the identification logic, the standing of the hard bounds |
| D7 | cycle slips and arc management | detection power and false-alarm rate at 30 s, the response cost, the gap rule, the mass-slip of a clock step |
| D8 | clock level, anchor and product | what the clock product is under the a priori anchor, the hold's standing, the covariance information on the wire, the contract against SSR practice |
| D9 | methodology and evidence | the user error and the sim truth errors by hour with formal sigmas, what the two instruments cannot see, what a scientific validation adds |

### 1.3 Method

Wave 1: one independent reviewer per dimension reads the worktree and the baseline telemetry and returns findings in a fixed schema (claim, evidence as file and line, theory, materiality, prior coverage, falsification route, confidence). The findings are read, deduplicated across dimensions and ranked by hand. Wave 2: every kept finding is attacked by two refuters with distinct lenses (the code truth of the claim; its scientific materiality and prior coverage). A finding survives only when the code claim is confirmed and the materiality is not refuted. Open questions whose answer is cheap are settled by a run: a 2 h arm per bed, a golden copy with one change, on a renamed Release executable, graded against a same-executable control on the held-out user and on datum-invariant wire quantities.

### 1.4 What counts, and what is not re-litigated

A finding is a departure from theory or practice with a mechanism and an expected size. Materiality: field-blocking (the posture cannot be trusted with it), material (a measurable effect on the user or on the product), inert (true but without effect on this posture), unknown (an experiment is owed). Not re-litigated: the do-not-retry list of the datum line (ensemble phase datum, `clock_datum_sum_to_zero`, the broadcast-anchored clock datum, lever B and lever C of card SBASIS-THEORY-E), the falsified ionosphere levers (adaptive iono q, SH as a producer constraint, GIM rows), the retired carries (`carry_invariant`, the lane layer), producer-side integer search as a product mechanism (card AB3's wide-lane exception stands). A finding that re-encounters one of these is reported as a re-encounter in one line.

### 1.5 Deliverables

Section 2: the findings table, ranked. Section 3: the experiments run and their results. Section 4: the builds the surviving findings justify, each with its owner card or a new pre-registration. Section 5: the verdict in plain words.
## 2. Findings (2026-11-07)

Wave 1 returned 87 findings from nine reviewers (3.99 million tokens, 836 tool uses, 47 minutes, no errors). The findings below are the deduplicated, ranked result; each line carries its verification status: **run** (an arm of section 4 settled it), **recomputed** (the reviewer's number was reproduced from the run directory by hand, section 3), **read** (a reviewer's reading of the code, adversarial verification by a second model pending, section 7). Materiality is graded as pre-registered. The reviewer ids (D1-01 and so on) index the full reports kept at `D:/GnssProcessorSupport/srn/wave1.json`.

### 2.1 Material findings, ranked

**F1. The variance factor is never estimated or applied, so the quality-control tier, the anchor guard, the product sigmas and the carry's consistency all read in a priori units that are two to four times pessimistic on real data** (D2-01, D2-04, D2-05, D2-08, D4-05, D6-01, D6-02, D8-03, D8-04; recomputed and run). Every code row is weighted at 0.3 m over sin(el) and every phase row at 2 mm (3 mm on the sim); the field's post-fit variance factor sits at 0.27 (0.54 on the sim, where the generator draws from the assumed law and the code reads 1.00), and the per-row normalized residuals read 0.29 to 0.49 for every signal (section 3.2). The within-epoch tier forms its statistic as max(1.0, 1.4826 median) over a pool that is 94 % carried-prior rows and triggers at 3.0, so it read exactly 1.0 on 1,440 of 1,440 epochs and adapted nothing (section 3.4); the arm that lowers the floor to 0.25 leaves the pool at 0.25 to 0.32, still below the trigger, with the products unchanged (section 4): the dead tier is the mis-scale, not the floor. The Q3 anchor guard standardizes by a formal predicted-residual variance that overstates the realized one by 4 to 16 in sigma, so its k = 6 is a 0.26 m (field) or 0.13 m (sim) bar instead of the designed 1 to 2 cm, which is why the scenario matrix's satellite clock step fails on every posture (card SMX). The URA is the formal clock sigma under a sigma0 fixed at 1 (median 126 to 23 mm by hour on the field against a user error flat at 12 to 23 mm). Rescaling the a priori sigmas alone (code 0.12 m, phase 1 mm) is not the cure: the arm moved the field products materially (E12 code bias 0.25 m, 42 bias keys above 5 mm, clock median 2.1 cm) and the user slightly worse in hour 2 (0.015 against 0.012 m), because the carried prior variances and the anchor q were tuned under the old scale. Theory: the a posteriori variance factor and the components of the stochastic model are estimable (Teunissen and Amiri-Simkooei 2008); a DIA procedure needs its statistics in their own units (Teunissen 2018). Route: section 5, build B1.

**F2. The station wet delay has no time link, and the only vehicle the record did not falsify is unreachable on this posture** (D1-08, D5-01 to D5-03, D5-10, D2-10 contested; recomputed, loader read). The wet delay is a local column born at zero every 30 s; the diagonal carry is KEEP_OFF (a station-common double count, card ZWD-SINK), and the in-block carry is disabled by a loader interlock that tests `network.phase_leveling_local_block`, the F3 knob the float vehicle forces off (`GnssConfigLoadNetwork.cpp` near lines 11519 to 11547), although the receiver code-bias in-block carry is vehicle-aware and runs (156 rows per epoch). The estimate moves 2.6 mm (field) and 3.2 mm (sim) per 30 s against 0.17 mm for a random walk at the table's rate, with a formal sigma of 5 to 8 mm; the sim's error against truth is 27 mm rms in hour 2 and 7 mm from hour 4, with a consistent formal sigma from hour 4 (section 3.5). The carried ambiguities already transmit most of the time information at the plateau (consecutive estimates are correlated: the increment is 0.34 of what independent estimates would give), so the cost is convergence, re-convergence after events, and an event sink: at a slip epoch the white column absorbs the station misfit (HKSL00HKG −0.77 m for one epoch at a 9 mm formal sigma, four such steps in hours 2 to 6, all within 30 s of a slip or attribution event). One reviewer (D2-10) read the column as time-linked through the coupling rows; the run-end contributions table shows 0 block ZWD rows, so that reading is set aside pending verification. Route: build B2.

**F3. The a priori hydrostatic delay comes from a fixed standard atmosphere; at the four Antarctic stations it is 59 to 95 mm too large and the wet delay goes negative; the GPT3 pressure removes it** (D1-01, D5-04, D5-05; run). The Saastamoinen delay uses 1013.25 hPa and 288.15 K reduced to the ellipsoidal height; the GPT3 grid of the repository gives 26 to 42 hPa less at SCTB00ATA, CAS100ATA, SYOG00ATA and PALM00ATA on day 129. The baseline's wet delay medians there are −0.087, −0.088, −0.012 and +0.014 m; with `receiver.troposphere_apriori_source: gpt3` they read −0.002, +0.007, +0.071 and +0.063 m, with the held-out user unchanged to the millimetre (section 4). What survives in the rows is the mapping mismatch (mf_w minus mf_h) times the error, about 3 mm at the 15 degree mask at those stations, and a polar user would carry a 0.1 to 0.2 m wet-delay start-up transient. The mapping function configured and logged as GMF is Niell's NMF (the tables at `Troposphere.cpp` lines 243 to 264); GMF is not implemented, and the VMF3 path exists unarmed. Route: build B3.

**F4. The emission hold freezes each phase bias at its generation start, so the converged estimate never reaches the wire, and within a generation the wire's phase-visible clock is the a priori product's** (D3-05, D8-02, D8-09, D9-05; read, run-end table recomputed). The emitted P record is the generation-start estimate plus the de-commoned clock transfer (`PbHold.cpp`); the run-end hold table reports |estimate − held| p50 1.14 m, p90 5.69 m, max 128.8 m over 128,391 held emissions. A float user is unaffected (its ambiguities absorb a constant), which is why the hold passed its bars (card PBH); an integer user cannot fix on it, and both standing instruments grade the a priori clock on the phase rows. The record's wording that the Q3 anchor is a permanent blend toward CODE is corrected by the reviewer of D8: the anchor is a random walk on the time difference (q 2.9e-8 m²/s) and the cold anchor toward the a priori is disarmed (sigma 1000 m against a 100 m gate); the a priori clock reaches the user through the hold's transfer, not through the anchor. The integer consequence is the subject of card ARR. Route: ARR.

**F5. The headline user is co-located with two roster stations and reads three to five times better than the census; convergence time is never graded; formal-versus-actual consistency is graded nowhere at the user** (D9-02, D9-03, D9-07; read, census replays on disk). WTZZ00DEU sits at the Wettzell site with WTZR00DEU and WTZS00DEU in the roster and 14 roster stations within 350 km; on the day-129 control wire its settled error is 0.013 m against a census median of 0.043 m, and it converges in 6 minutes where the census median is 29 minutes to the first epoch under 0.1 m (p90 82 minutes, max 101; 77 minutes to a sustained 0.05 m). The consumer trace carries no formal sigma, so no ratio of actual to formal error has ever been a bar. These are float-PPP class convergence times, which is the quantity by which the literature distinguishes a network product from PPP (Teunissen and Khodabandeh 2015). Route: build B5.

**F6. A persistent gross code channel at one station stays in the solve and moves the code-bias product** (manager finding SRN-M1, D6-05, D6-10; recomputed and run). BSHM00ISR's C1C rows carry an rms of 10.9 m over 4,284 weighted rows in hours 2 to 6 (section 3.3); the gross line-of-sight screen declared only 4 times in 603,242 line-of-sight epochs and the within-epoch tier never triggers (F1). Removing the station moves the C1W code bias of G22 by 1.5 cm and of G14 by 1.2 cm (medians over hour 2, three keys above the 5 mm bar), the clock medians by 6.6 mm, and leaves the held-out user unchanged (section 4). The estimable block-Baarda census is the one armed check that sees code multipath and nothing responds to it (OPEN_ITEMS). Route: build B4.

**F7. Integer information on the baseline wire is pair-dependent, not constellation-dependent, and its limit is the code-averaged ionosphere level under a white wet delay** (manager measurements; recomputed). Reference-free, the field's wide lanes are integer (94 to 95 % within 0.15 cycle for GPS L1C minus L2W and Galileo L1C minus L5Q) and the narrow lanes split by station pair: the European Galileo baselines reach 0.015 to 0.03 cycle, the remote GPS baselines of the trace sit at the uniform null (0.26 on NIUM00NIU to PARK00AUS), and a pair present in both constellations reads alike (section 3.6). On the sim the narrow-lane error falls as one over the square root of the arc length, the signature of a level determined by code averaging: under a white per-epoch slant ionosphere the narrow lane's level enters only through the geometry-free code, and under a white wet delay the ionosphere-free double difference separates from the two stations' wet delays only through the within-epoch elevation spread. The formal narrow-lane double-difference sigma grows from 0.17 cycle in hour 3 to 0.47 in hour 6 as the arc population turns over. Route: ARR.

**F8. Post-fit identification is a median-polish heuristic, no whole-satellite response is reachable, and the station heteroscedasticity is real but does not explain the flagged stations** (D6-04, D6-02, D6-09; read and run). The T4 identification tables per-arc maxima of |v| sqrt(w) without the redundancy, fits a two-way additive model and declares at 6 sigma0 with sigma0 = 1; no w-test maximum, no separability check. The per-satellite consensus runs only when attribution is off, and the attribution's whole-satellite set fills only above the unreachable 3.0 trigger (F1), so the only reachable post-fit response is the isolated arc. The measure-only station factor (arm `srn_stafac_f6`) spans 0.17 to 1.08 around a median of 0.29 (MAJU00MHL 1.08, SOLO00SLB 0.82, SGOC00LKA 0.64), yet the two stations holding 25 of 55 attribution flags are not the noisiest (HKSL00HKG 0.44, BSHM00ISR 0.22): their flags are event-driven, which refutes the noise attribution of D6-09 while confirming the spread. The redundancy kernel itself draws the priors and the datum correctly (D6-03), so a proper w-test over observation rows is available. Route: build B1.

**F9. Three slip-layer defects** (D7-01, D7-02, D7-06; read, verification pending). The gap re-entry level test differences the raw geometry-free phase across the whole gap and gates it at a 30 s scale, so on the slip-free sim every inside-horizon re-entry refloat is false (22 in 6 h, gaps of 23 to 28 epochs). The Melbourne-Wubbena detector and the re-entry gates treat any non-zero loss-of-lock indicator as a loss of lock, while RINEX 3.04 defines bit 2 as BOC tracking: at the four Leica GR50 stations (REYK00ISL, WROC00POL, WTZR00DEU, HKSL00HKG) 17,000 Galileo E1 rows carry the value 4 and the detector withholds every such sample. Of the loss-of-lock assertions acted on, 58 % at the eight Trimble Alloy stations have no measurable wide-lane or geometry-free step (7 % at Septentrio). Route: build B6.

**F10. Physical model gaps at the millimetre level** (D1-02, D1-03, D1-05, D1-09; read; the 2 h gradient arm inconclusive). No troposphere gradients at producer or user (the user's standing +1 cm north mean in hours 3 to 6 is unattributed: the sim consumer shows north means of the same size without a gradient; the 2 h consumer-gradient arm changed nothing, a 6 h arm is owed). The Galileo FOC modified yaw law and the GPS Block III law are absent while seven Galileo satellites sit inside the modified-yaw regime on day 129 and the umbra exclusion costs 678 satellite-epochs in 6 h. The Sun and Moon are precessed twice on the default path (0.34 degrees in 2024, up to 2 mm of solid tide; known, the fix exists off). The legacy ocean-loading horizontal reversal is live in the code but off the goldens' path. Route: build B7 (yaw law) and the owed arm.

**F11. Product semantics and the realized datum, as read** (D3-02 to D3-08, D8-07, D8-08; read, verification pending). The realized S-basis is minimal and every rank deficiency of the two-constellation model maps to one constraint (D3-01, sound). The receiver code-bias datum is a per-station null direction realized as a per-epoch prior that accumulates on a carried column (0.37 m of datum weight after 720 epochs), and the record's description of a fleet-mean datum is stale. At a clock-pivot handover the diagonal S-transform leaves the dense block's off-diagonals in the outgoing frame for one fold; the exact kernel is built and off, and arming it changed the emitted clocks by at most 1.4 cm in one satellite's median (G01 0.14 m at one epoch) with the user identical (section 4): inert at the user. Under today's basis each satellite's phase bias sits in the frame of its own first anchor receiver (21 to 30 anchor stations per epoch), so the user cannot recover narrow-lane integers from the P records without the eq34 datum (already covered by card IRD). The Galileo C5X code bias is asserted equal to C5Q at the broadcast group delay with no live guard against a receiver that bridges the two, and the per-satellite pin pair follows coverage (22 reselections in 6 h, each a declared discontinuity with emitted clock steps to 10.5 cm). The contract against SSR practice: the orbit is identified by file name only, no clock rate or latency model, the Galileo reference pair undeclared on the field wire, and the GPS clock level scatters 0.165 m against CODE after the convention transform where Galileo reaches 0.086 m (unexplained; a C1W-priority arm proposed). The clock datum station MBAR00UGA is the epoch schedule master and the S-line reference receiver, not a datum of the estimation.

**F12. Methodology** (D9-04 to D9-06, D9-09; read and recomputed). The sim baseline pair of record starts at 00:00:30, inside the fixture's start artifact the record says to skip, so its hour 1 and its clock level carry the artifact (the arms of this card start at 00:30:15). Within a hold generation both instruments grade the a priori clock (F4). Neither instrument sees integer consistency, and the Melbourne-Wubbena combination of the wire's biases minus the sim's truth biases has between-satellite fractions at the uniform null in every hour while the producer's own double-difference wide lanes are 98 % round-correct: consistent with the hold of F4, to be settled in card ARR.

### 2.2 Recorded as sound

The solve kernel is an exact marginalization and conditioning chain with every numerical guard silent on both beds (D4-01); the harvest and the coupling rows factor the joint posterior without loss and the exact conditional time update is derived and applied correctly (D4-02, D4-03); the pins, scales and tolerances are consistent and the three armed parallel stages are byte-identical by construction (D4-09, D4-10); the shipped per-row form drops the cross-row time-update covariance, a deviation the SCL cards measured and that matters only when the self-contained level replaces the a priori clock (D4-04); the realized S-basis is minimal and verified on both beds (D3-01); the redundancy kernel draws the priors and the datum correctly (D6-03); the gross line-of-sight screen is a sound two-edge bound (D6-05); the pivot handover at emission and the frame hold are consistent (D8-05, D8-06); the free per-line-of-sight slant ionosphere is the ionosphere-float full-rank model working as the theory says, with per-epoch noise of 1 to 4 mm (D5-06); the slip responses hold the reset-not-repair doctrine and one slip frees an arc once (D7-05, D7-08); the shared observation equation carries the IGS conventions with the right signs (D1-06).

### 2.3 Recorded as inert, no action

The float drop's conditional-for-marginal variance (D4-06, under 0.3 % in steady state); the zeroed within-station cross terms at drop edges (D4-08, 0.14 % of pairs); the repeated receiver DCB datum prior (D4-12); the common-band scope on a two-band posture (D7-07, a ledger note owed); the one-epoch lag of the gross-LOS adaptation behind the declaring emission (D6-05); the higher-order ionosphere (D5-09, settled by card SOI); the OTL legacy path and the double precession (D1-04, D1-05); the prefit census phase column, degenerate at 1.25e8 sigma and inert only through the event mask (D6-07, an OPEN_ITEMS item); the prior-residual census numerator under the conditional coupling (D6-08, its ladder unreadable until aligned); the LS-VCE carry monitor reporting no ratio because the hand floors are 0 (D2-07); the Galileo family birth prior (D2-11, consistent with the measured code noise).

### 2.4 Second-model verification of the material findings (2026-11-07)

Wave 2 ran on Opus after the Fable limit: two referees per finding (the code truth of the claim; its scientific materiality and prior coverage) on 39 findings, 78 agents, 9.45 million tokens, 16 minutes, no errors; the full verdicts are kept at `D:/GnssProcessorSupport/srn/wave2_verdicts.json`. Code lens: 9 confirmed, 29 partly, 1 refuted. Science lens: 37 partly, 2 refuted. "Partly" means the code claim holds and the size, the attribution or the prior coverage was corrected. The corrections, by finding of section 2.1:

- **F1, corrected.** Only the code scale is established: 2.4 times pessimistic in sigma (6 in variance) on two independent statistics, the standardized post-fit residuals and the estimable block-Baarda sigma0 (0.41 on the field against 0.88 to 0.90 on the sim). The phase scale is not identifiable from post-fit residuals: with a free per-line-of-sight slant ionosphere and equal phase weights the two phase residuals of a line of sight obey v_L1 + gamma v_L2 = 0 (measured correlation −0.9999), so the per-band phase "structure" of D2-02 is algebra, not noise. The within-epoch tier is inert because its trigger 3.0 reads in the same units as the floored sigma0, not because of the floor (the arm of section 4 showed it); the tier is a median-triggered network-wide backstop by design. The anchor guard's deciding statistic on the goldens is the phase-space one (`network.clock_anchor_guard_space` phase) with its own 0.02 m floor, which makes k = 6 a 0.12 m bar on 95 to 99 percent of rows; the clock-space statistic the reviewer graded decides nothing, and its denominator projects per-satellite marginals as independent, so the "carry consistency 0.06 to 0.38" reading of D4-05 is withdrawn. The URA is read by nothing on the goldens. A defect found by the referee of D2-03: the second-code companion membership test (`StationSplit.cpp` lines 529 to 541) matches on the observation code only, not on the system, so at the 18 stations whose GPS companion is C1C the Galileo C1C rows are divided by 100 as well (8,051 rows in hours 2 to 6 at 3 m over sin(el)). Materiality: material for the quality-control scale and the companion defect; inert for the float user's position. Build B1 amended (the code factor; the guard is not a reader); build SCC2 added for the companion defect.
- **F2, confirmed as stated.** The wet delay is carried by no route (`zwd_blk_on` false, the trace's `carried` column 0 on every row; the reading of D2-10 is set aside), the interlock defect sits at `GnssConfigLoadNetwork.cpp` lines 11083 to 11093 and 11526 to 11532 (card CLV had recorded the three in-block knobs as byte-identical under the float vehicle without the diagnosis), and the event-sink episodes masked no arc (the attribution test refloated the implicated arcs in both), so the response of build B2 rests on standing invariant 1, not on a measured masking. Inert for current products, since no golden arms the carry; build B2 is the experiment.
- **F3, confirmed, sizes corrected.** With the code's bilinear grid query the pressure deficits are 37.5, 42.2, 37.1 and 21.5 hPa (85, 96, 84 and 49 mm of hydrostatic delay) at SCTB00ATA, CAS100ATA, SYOG00ATA and PALM00ATA; WTZZ00DEU is 15.5 mm the other way. What survives in the rows after the fit is 1.5 to 2.9 mm at the 15 degree mask at four of 58 stations, 0.5 mm at the held-out user. Material at the stations, inert at the user; build B3 proceeds.
- **F4, confirmed and already in the record.** The hold's mechanism and its posture limit are stated in `PbHold.hpp`, card PBH section 6.5, card LAH section 5 (|estimate − held| 1.30 to 1.75 m, dominated by frame re-realizations) and card SCB section 6. Both referees of D8-02 grade it material: every quiet-day user number of the baseline rests on the a priori clock's motion within a hold generation, so the instruments cannot support a claim of a self-contained product until the broadcast-level cards land.
- **F5, facts corrected.** WTZZ00DEU is 1.6 m from WTZR00DEU and 67 m from WTZS00DEU; 7 roster stations lie within 350 km (11 within 460 km), not 14; WTZZ00DEU is the best of 17 replayed consumers, but BAKE00CAN reaches 0.017 m and BADG00RUS also converges in 6 minutes, so co-location is a candidate cause, not an established one; card MSC (closed 2026-09-21) had already ruled WTZZ00DEU unrepresentative and made the census standing; card IRD pre-registered convergence bars (K-c) that the standing grading never adopted; the census times are joint producer-ignition and user-convergence times; at 4 of the 16 census sites the hours-2-to-6 window is not settled. Material for reporting only; build B5 amended.
- **F6, refuted.** BSHM00ISR has no persistent gross C1C channel: its 10.9 m rms comes from 9 to 12 rows in three short episodes at the returns of G22 (TOW 269340, 273450) and G13 (273240) after loss-of-lock gaps, 60 to 416 m each, with every other C1C satellite at the station between 0.15 and 0.53 m rms and the station's hourly C1C rms at 0.23 to 0.25 m. The gross line-of-sight screen declared and deweighted both persistent episodes (card RF-3 traced them); the C1C code biases of G13 and G22 did not move at those epochs; the removal arm's 1.2 to 1.5 cm moves on G22 and G14 are the roster change, not the channel. Build B4 withdrawn.
- **F7, stands.** A manager measurement outside the selected set; not re-verified.
- **F8, corrected.** The median-polish identification is confirmed as read, and its effect is confined to companion-arc refloats at two event stations; the absence of a whole-satellite response holds by construction under attribution (`Raim.cpp` records the 305-station lock-in that wholesale refloats caused); the noise attribution of D6-09 is refuted by the station-factor arm (section 4). Material as a design departure, inert on the posture.
- **F9, confirmed, inert.** The gap gate ignores the gap length (D7-01), but on the armed posture the wind-up restart refloat frees the same arcs in the same epoch (11 of 11 sim arcs, 16 of 16 field), so products do not move; the loss-of-lock predicate `lli != 0` (D7-02) was flagged by the 2026-06-10 audit and no slip population is lost at the four stations on this day; the receiver-family contrast (D7-06) is a selection artifact of the corroboration gate. Conformance fixes, low priority.
- **F10, corrected.** The attribution of the user's +1 cm north mean to a gradient does not survive: the sim consumer shows north means of the same size without a gradient, and the horizontal sensitivity at a 15 degree mask with the goldens' weights is about 3.2, not 7; the gap stands. The Galileo yaw law is inert at the millimetre on this day (no raised residuals in the noon windows; card NTA's strongest turn moved the user 2.2 mm).
- **F11, corrected.** The receiver DCB datum as a per-station null direction is confirmed and already recorded in card CTU2 section 2; the diagonal handover kernel lasts one fold per edge because the anchor trips the changed constellation's clocks loose at the datum change; the 305-station rank-gate risk of D3-06 already happened: on `_rap_bds305_ge_f6` the relative cutoff rose above the weak class during hour 1 and the completion held 74 phase-bias directions (card CTU2); the Galileo C5X assertion cannot be bridged under `channel_mode` single; the 0.165 m GPS level against CODE reproduces, its candidate explanation (the C1C inter-signal assertion) is refuted by the D1 arm, where CODE's own pair gave 0.59 m, and card CLV section 8.3 had measured it; inert for a float user and cancelling for an integer user's phase.
- **F12, confirmed.** The existing grader `scripts/simnet_wire_integer_consistency.py` reproduces the wire's wide lane at the uniform null against the truth biases, which is expected by construction on the float datum; the consumer's formal covariance was already found uncalibrated by card NIS, so the position NEES of build B5 is read beside that record.

Net reading after verification: the estimation core is sound; most deviations are inert for the float user on this posture; the material residue is the code weight scale and the quality-control thresholds that read it (F1), the companion-weight defect (new), the dependence of both instruments on the a priori clock within a hold generation (F4), and the methodology of the headline (F5, F12). The wet-delay time link (F2) and the meteorological troposphere (F3) are improvements whose user effect is owed to their gates.

### 2.5 Amendments to the builds (2026-11-07)

Build B4 (the station code-channel screen) is withdrawn with F6. Added: **SCC2**, the system-aware companion membership (one key, default off, byte-identical when off; the field 2 h and 6 h gate on the Galileo C1C weights, the Galileo code biases, the user and the census), and **CRO**, a multi-epoch oracle test of the carry recursion across an arc birth, a slip drop, a departure and return and a clock-pivot handover (tests only; the measured departures recorded as numbers). Build B1 is amended to the code factor with the phase class reported with its redundancy, and the anchor guard removed from its readers. Builds B6 and B7 are demoted to conformance items without a product effect on this day.

## 3. Measurements of the baseline pair made for this review (2026-11-07)

Every number below was recomputed from the run directories of the baseline pair (`runs/_rf0hv17_field6`, `runs/_rf0hv17_sim6`) with the scripts named, independently of the reviewers' reports. Hours count from the run start (field 00:00 UT; the sim pair of record starts at 00:00:30, inside the fixture's start artifact, so its hour 1 is not graded).

### 3.1 The held-out user by hour

`logs/closed_loop_trace.csv` (`pos_err_3d_m`, `east_m`, `north_m`, `up_m`), 120 epochs per hour, no AR (`ar_mode` 0, 0 fixes).

| bed | hour | 3D p50 | 3D p90 | horizontal p50 | up p50 |
| --- | --- | --- | --- | --- | --- |
| field WTZZ00DEU | 1 | 0.059 | 0.133 | 0.040 | 0.034 |
| | 2 | 0.012 | 0.019 | 0.010 | 0.005 |
| | 3 | 0.023 | 0.031 | 0.014 | 0.015 |
| | 4 | 0.012 | 0.017 | 0.010 | 0.004 |
| | 5 | 0.017 | 0.021 | 0.015 | 0.007 |
| | 6 | 0.012 | 0.015 | 0.010 | 0.004 |
| sim SZZ000SIM | 2 | 0.072 | 0.142 | 0.024 | 0.068 |
| | 3 | 0.031 | 0.051 | 0.015 | 0.024 |
| | 4 | 0.024 | 0.044 | 0.009 | 0.021 |
| | 5 | 0.026 | 0.040 | 0.010 | 0.024 |
| | 6 | 0.018 | 0.032 | 0.007 | 0.015 |

All values in metres. The field numbers reproduce the record's baseline paragraph.

### 3.2 Post-fit residuals by constellation, signal and elevation

`logs/network_pf_rows.csv` (`resid_m`, `w`, `elev_deg`), hours 2 to 6, every second epoch (`scratch pf_rows_by_signal.py`). `rms_norm` is the root mean square of `resid * sqrt(w)`, the residual in units of its a priori sigma.

| bed | signal | kind | 15 to 30 deg rms (m) | 30 to 60 deg rms (m) | 30 to 60 deg rms_norm |
| --- | --- | --- | --- | --- | --- |
| field | G C1C | code | 2.079 | 1.219 | 2.129 |
| field | G C1W | code | 0.412 | 0.225 | 0.286 |
| field | G C2W | code | 0.447 | 0.208 | 0.446 |
| field | E C1C | code | 0.390 | 0.194 | 0.404 |
| field | E C1X | code | 0.341 | 0.193 | 0.420 |
| field | E C5Q | code | 0.328 | 0.239 | 0.493 |
| field | E C5X | code | 0.310 | 0.185 | 0.399 |
| field | G L1C | phase | 0.0040 | 0.0018 | 0.568 |
| field | G L2W | phase | 0.0022 | 0.0011 | 0.336 |
| field | E L1C | phase | 0.0034 | 0.0017 | 0.541 |
| field | E L5Q | phase | 0.0019 | 0.0010 | 0.305 |
| sim | G C1C | code | 0.841 | 0.470 | 1.024 |
| sim | G C2W | code | 0.835 | 0.465 | 1.011 |
| sim | E C5Q | code | 0.826 | 0.462 | 1.006 |
| sim | G L1C | phase | 0.0035 | 0.0022 | 0.474 |
| sim | G L2W | phase | 0.0021 | 0.0013 | 0.288 |

Two readings. On the sim, whose generator draws the code noise from the same law the estimator assumes, the normalized code residual is 1.00 to 1.04: the statistic is calibrated by construction. On the field it is 0.29 to 0.49 for every code and phase signal but GPS C1C: the a priori sigmas are pessimistic by a factor of two to three (sigma), and the two bands differ (the band-2 and band-5 phases read 0.3, the band-1 phases 0.55). The GPS C1C rms of 1.2 to 2.1 m is a tail, not a level: the median |residual| of GPS C1C (0.14 m at 30 to 60 degrees) matches Galileo C1C (0.10 m).

### 3.3 The gross GPS code tail is one station

`logs/network_pf_rows.csv`, GPS code rows with |residual| above 1 m, hours 2 to 6 (`scratch pf_gross_census.py`).

| signal | rows | above 1 m | share |
| --- | --- | --- | --- |
| C1C | 282,686 | 3,281 | 1.16 % |
| C1W | 195,246 | 1,574 | 0.81 % |
| C2W | 281,847 | 2,040 | 0.72 % |

By station, BSHM00ISR holds a C1C rms of 10.89 m over 4,284 rows (hours 2 to 6); the next station is CPVG00CPV at 0.64 m, then POVE00BRA 0.56 m, SCTB00ATA 0.50 m. The gross rows are spread over all GPS satellites (G06 175, G09 163, G29 145 at the top) and are steady by hour (594 to 719 per hour on C1C). Without BSHM00ISR the GPS code noise matches Galileo's.

### 3.4 Variance factor and the within-epoch tier

`logs/network_epoch_status.csv` (`sigma0`, `n_adapt`, `sigma0_pf`), both beds, 720 epochs each (`scratch baseline_numbers.py`).

| bed | within-epoch sigma0 (every epoch) | n_adapt (run) | post-fit sigma0 p50 by hour 1 to 6 |
| --- | --- | --- | --- |
| field | 1.000 | 0 | 0.203, 0.243, 0.255, 0.263, 0.268, 0.271 |
| sim | 1.000 | 0 | 0.417, 0.451, 0.474, 0.509, 0.533, 0.539 |

The within-epoch statistic reads exactly 1.0 on 1,440 of 1,440 epochs and adapted nothing; the post-fit variance factor sits at 0.27 (field) and 0.54 (sim).

### 3.5 The wet delay

`logs/network_zwd.csv` (`zwd_m`, `std_m`), hours 2 to 6; the sim truth from `runs/simnet60_quiet_col_24h/<station>/truth_tropo.csv` (`zwd_m`).

| bed | epoch-to-epoch difference sd, median over stations | formal sigma p50 | against truth by hour 2 to 6, rms (m) | NEES mean by hour 2 to 6 |
| --- | --- | --- | --- | --- |
| field (58 stations) | 0.0026 m | 0.0054 m | no truth | |
| sim (60 stations) | 0.0032 m | 0.0081 m | 0.027, 0.012, 0.008, 0.007, 0.007 | 12.6, 2.28, 1.06, 0.87, 0.77 |

A random walk at the table's rate (1e-9 m^2/s) moves 0.17 mm in 30 s; the per-epoch estimate moves 15 to 20 times that. The formal sigma is honest from hour 4 on the sim (NEES near 1) and over-confident by 1.5 to 3.5 in hours 2 and 3.

### 3.6 Integer information on the baseline wire

Field, truth-free, reference-free (`scripts/dd_frac_reref.py`, hours pooled, groups of three or more satellites, a size-matched null beside each number); sim against truth (`scripts/simnet_dd_reref.py`, `scripts/simnet_amb_integer.py --ref-free --by-age`).

| bed | signal | reference-free median |fraction| (cycle) | null | within 0.1 cycle |
| --- | --- | --- | --- | --- |
| field | G L1C | 0.129 | 0.121 | 44.6 % |
| field | G L2W | 0.122 | 0.119 | 46.1 % |
| field | E L1C | 0.059 | 0.111 | 63.9 % |
| field | E L1X | 0.082 | 0.115 | 55.4 % |
| field | E L5X | 0.101 | 0.115 | 49.8 % |
| sim | G and E L1C, hours 2 to 6 | 0.082 to 0.146 (error against truth) | | 84 to 91 % round correct |

Wide lanes (`scripts/ab3_wl_readiness.py`, hours 3 to 6, reference-free): GPS L1C minus L2W 94.0 % within 0.15 cycle (null 55 %), Galileo L1C minus L5Q 94.9 %; the sim wide lane against truth is 98.4 % round-correct in the last quarter.

The sim's narrow-lane error against truth falls with the arc's age as one over the square root of its length (slope -0.52 on a log-log fit; 1.53 cycle in the first ten epochs, 0.12 cycle past 300 epochs), which is the signature of a level determined by code averaging.

The apparent constellation asymmetry on the field is the trace's station sampling, not a GPS defect (`scratch dd_station_health.py`, hours 3 to 6, per station pair): the GPS L1C chords come from 13 remote stations on intercontinental baselines (worst pairs NIUM00NIU to PARK00AUS 0.262 cycle, NYA200NOR to REYK00ISL 0.230), the Galileo L1C chords from a European cluster (best pairs CEBR00ESP to WTZR00DEU 0.015, STJO00CAN to PTBB00DEU 0.024; worst REYK00ISL to WROC00POL 0.221). On a pair present in both, JPLM00USA to HOLB00CAN, GPS reads 0.105 and Galileo 0.110. Integer information is therefore pair-dependent: mid-latitude baselines carry it, low-latitude island and auroral stations and the longest baselines do not; a few satellites are worst (E11 0.234, E14 0.164, G22 0.256, G24 0.246).

## 4. Experiments run for this card (2026-11-07)

Executable `gnss_processor_srn.exe` (Release, built from the worktree at `adef08b`, sha256[:16] `7c9c28a376321d21`), one change per arm on a golden copy, same-executable controls, sim arms from 00:30:15; run directories under `D:/GnssProcessorSupport/runs_nom/_rap_<tag>`; grader `scratch grade_arms.py` (kept beside the card's data at `D:/GnssProcessorSupport/srn/`). User error in metres (3D p50 by hour); "identical" means every record within the grader's resolution.

| arm | change | user by hour (arm / control) | products | reading |
| --- | --- | --- | --- | --- |
| `srn_gpt3_f2` | `receiver.troposphere_apriori_source: gpt3`, grid `data/static/gpt3/gpt3_5.grd` | h2 0.012 / 0.012 | Antarctic ZWD medians SCTB −0.002 (ctl −0.087), CAS1 +0.007 (−0.088), SYOG +0.071 (−0.012), PALM +0.063 (+0.014) | F3 confirmed; user inert; census owed |
| `srn_grad_f2` | `estimation.estimate_tropo_gradients: true` (consumer only) | h2 0.013 / 0.012, north mean +0.004 / +0.003 | producer identical | inconclusive at 2 h; the +1 cm north mean sits in hours 3 to 6 |
| `srn_s0floor_f2` | `network.raim_sigma0_floor: 0.25` | h2 0.013 / 0.012 | sigma0 reads 0.25 (max 0.319), n_adapt 0; B records within 0.5 mm, C within 1.1 mm | the tier stays dead below the 3.0 trigger: F1 |
| `srn_s0floor_s2` | the same, 0.5, sim | identical | identical | as above |
| `srn_noise_f2` | code 0.12 m, phase 1 mm | h1 0.055 / 0.059 (p90 0.179 / 0.133), h2 0.015 / 0.012 | sigma0_pf 0.50 (code 0.66, phase 0.44); 42 B keys above 5 mm (E12 C5Q 0.25 m); C median 2.1 cm | a rescale alone moves the products and costs the user: F1 |
| `srn_noise_s2` | the same, sim | h2 0.035 / 0.033, h3 0.059 / 0.055 | sigma0_pf 1.05 (code 2.49, phase 0.58); B within 2 mm; C median 4.0 cm | the sim's code is 0.3 m by construction, so 0.12 m is a mis-weight there: a control for the direction of the effect |
| `srn_nobshm_f2` | BSHM00ISR removed | h1 0.053 / 0.059 (p90 0.193 / 0.133), h2 0.012 / 0.012 | B: G22 C1W 1.5 cm, G14 C1W 1.2 cm, E24 C1X 0.6 cm (3 keys above 5 mm); C median 6.6 mm, max 4.7 cm | F6 material for the B records, inert at the user |
| `srn_hsd_f6` | `network.handover_stransform_dense: true`, 6 h | identical to 1 mm in every hour | C median-of-medians 0.000, E25 1.3 cm, G01 0.14 m at one epoch | inert at the user; the kernel is a cleanliness matter |
| `srn_hsd_s6` | the same, sim 6 h | identical | C within 5 mm | as above |
| `srn_stafac_f6` | `network.raim_station_factor: true` (measure-only), 6 h | identical | factor median 0.29 (p10 0.17, p90 0.49, max 1.08 MAJU00MHL); HKSL00HKG 0.44, BSHM00ISR 0.22 | heteroscedasticity real; the flagged stations are not the noisiest: F8 |

Controls: `srn_ctl_f2` (h1 0.059, h2 0.012), `srn_ctl_s2` (h2 0.033, h3 0.055 from 00:30:15), `srn_ctl_f6` (0.059, 0.012, 0.023, 0.012, 0.017, 0.012: the record's baseline reproduced on the review executable), `srn_ctl_s6` (0.099, 0.033, 0.040, 0.016, 0.018, 0.015). Every arm emitted every epoch with exit code 0.

## 5. The builds the findings justify

Ranked by the product and user effect the evidence supports; each default off, byte-identical when off, sim first with truth where the sim can see it, then field against a same-executable control on the held-out user and the 16-site census. Owner: a new card each, pre-registered before the build.

- **B1. A calibrated variance factor and a standardized identification** (F1, F8). An LS-VCE pass per measurement class (code and phase; per constellation and band) with the carried prior variances and the anchor q re-balanced in the same step, applied as the scale every acting threshold reads (the within-epoch trigger, the T4 bar, the anchor guard, the URA); then identification as a w-test maximum on the redundancy kernel's cofactor over observation rows with per-station factors. Gate: sigma0 near 1 by class on both beds; the SMX satellite clock step of 0.1 m tripped at its epoch; the user within 2 mm of the control in hours 2 to 6; the census median within 0.5 mm. Risk: the rescale arm shows that the balance, not one knob, is the lever.
- **B2. A time link for the wet delay on the float vehicle** (F2). A vehicle-aware loader interlock (as the receiver code-bias carry has) and the wet delay as a column of the station's whitened block with the table's random-walk rate; the prior-residual census family `zwd` fed from the block. Gate on the sim against `truth_tropo.csv`: hour-2 rms from 27 mm to under 12 mm, hours 4 to 6 from 7 mm to under 4 mm, NEES 0.7 to 1.3 by hour; the event-sink steps of the field (HKSL00HKG, KIT300UZB) gone; user and census within the neutral spread. Then read the narrow-lane convergence of card ARR on the arm.
- **B3. A meteorological a priori troposphere on the field golden** (F3). `receiver.troposphere_apriori_source: gpt3` with the grid path, a ledger row (RECOMMEND_SCOPED, field only: the sim cancels the delay by construction), the generator and the goldens; the mapping label corrected to NMF or the VMF3 path validated. Gate: the 6 h field arm and the census, with the four Antarctic stations' wet delays non-negative and the polar census sites' hour-1 errors read.
- **B4. A station channel census with a response** (F6). A per-(station, signal) post-fit code rms over a window, a declared bar in sigma units once B1 lands (in metres meanwhile), and the response of excluding the channel from the shared solve while it fails (an event at both edges, invariant 7). Gate: BSHM00ISR's C1C excluded within 10 minutes on the field; the G22 and G14 code biases move by the 1.2 to 1.5 cm the removal arm measured; the quiet sim fires nothing.
- **B5. Validation instruments** (F5, F12). The consumer's formal sigmas written to `closed_loop_trace.csv` and a NEES grade per hour at the user and the census sites; a convergence grade (time to a sustained 0.1 m and 0.05 m) in `gcensus.py`; the census median and p90 as the headline beside WTZZ00DEU; the sim golden started at 00:30:15. No run: a trace change and a reporting change.
- **B6. Slip-layer fixes** (F9). The loss-of-lock predicate on bit 0 with bit 2 as its own class; the gap re-entry geometry-free gate scaled by the gap length (or the shifted verdict requiring the Melbourne-Wubbena leg); a receiver-aware loss-of-lock policy for the Trimble Alloy class. Gate: the 22 false sim re-entry refloats to 0; the Leica stations' Galileo Melbourne-Wubbena samples admitted; the field 6 h user and census within the neutral spread.
- **B7. The Galileo FOC yaw law and GPS Block III** (F10). Behind a key in `SatAttitude`, shared by the antenna offsets and the wind-up like the noon turn. Gate: the phase residual rms of the seven satellites inside the regime in their noon windows; the user and census.

Owed arms without a build: a 6 h consumer-gradient arm (F10); the C1W-priority arm for the GPS level against CODE (F11); a bridging-receiver roster census before any Galileo X-family change (F11); a 305-station rank-gate trace (D3-06).

## 6. Verdict

The estimation core is sound: the solve is an exact marginalization chain, the recursion is an exact factorization of each epoch's posterior with a correctly derived time update, the S-basis is minimal with every rank deficiency of the two-constellation model mapped to one constraint, and the responses hold the reset-not-repair doctrine. The departures that matter are not in the algebra but in its calibration and its observability: nothing estimates the variance factor, so the quality-control tier, the anchor guard and the product sigmas read in units two to four times too large and have never acted on real data; the wet delay has no reachable time link; the a priori troposphere is a fixed standard atmosphere that fails at the poles; one station's ruined code channel sits in the solve for hours; the emitted phase bias is frozen at its generation start, so the wire cannot carry the integer information the producer holds; and the headline user is the best-served point on Earth for this roster, graded without a convergence statistic or a formal-error check. Each of these has a build with a gate (section 5), and the first four are cheap.

## 7. Verification status and limits of this review

Wave 2 (two referees per finding, 39 findings selected, 78 agents) was launched at 00:20 and lost 76 of 78 agents to the Fable model limit at 00:32; relaunched on Opus at 02:40, it completed at 02:56 with every agent returning (section 2.4). Numbers the referees did not recompute: the beta angles of D1-03 and the convergence times of D9-03 beyond the census replays. Six implementation agents (B1, B2, B3, B5, SCC2, CRO), each in its own worktree with a pre-registered card, tests and gated arms, were started at 03:40 after a first launch at 03:05 was stopped when F6 fell; their cards land under `docs/refound/` on their own branches and report to this review's session. The integer review of card ARR ran on the same model in parallel.

# SCW2: a watch on the code level of one satellite (2026-11-18)

**STATUS**

- **id:** SCW2
- **topic:** code-bias
- **status:** open
- **summary:** Design of a default-off watch on the level of one satellite's code column: a signed 2 h mean of pooled code residuals per (satellite, signal), normalised by the spread measured across columns, a pooled raw Melbourne-Wubbena step as confirmation, and a bounded, declared response split by column type. At the adopted threshold of 4.5 and under correlated code multipath, the 2 h detectable step is at least about 23 cm at 10 stations and 13 cm at 30 (lower bounds), and the confirmation step rarely reaches its bar, so a true step would seldom be classified as one (corrections, items 1 and 6). The monitor comes first; the response is built only if an inventory of free columns, the reference configuration included, shows a wandering column.
- **source:** plan package 20 of the post-reset program 2026-11-18, "Watch on a satellite code column's level"; commits read: `7fb0737` (branch chain/audit) for network code, configurations and the record, main `5ddc422` for user-engine code. Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** No armed statistic sees a 15 cm step of one satellite's code column, and none acts on it. The usable statistic is new: a signed 2 h mean of the satellite's pooled code residuals, normalised by the spread measured across satellites. At the 4.5 threshold under 5 min code multipath, the 2 h detectable step is at least 23 cm at 10 stations and 13 cm at 30 (7 and 4 cm white), lower bounds. The pooled Melbourne-Wubbena confirmation rarely reaches its bar under the same multipath. The response is bounded, never touches integers, and waits on an inventory of free columns.

Read at: network code, configurations and the record at commit 7fb0737 (D:/GnssProcessorSupport/worktrees/read_7fb0737); user-engine slip code at main 5ddc422 (C:/GnssProcessor). Abbreviations: RC = config/golden/golden_field_refound.json, RS = config/golden/golden_sim_refound.json (both 7fb0737). Line citations are to 7fb0737 unless "main" is named. No build, run or test was made for this document.

---

## 0. The question, the two failures, and the skeptic's verdict in brief

Two different failures show the same signature (a pooled code residual of one satellite):

- **A truth step.** The satellite's code on one signal changes (hardware, a tracking change). The carried column stays where it was; its residuals at every station that sees the satellite shift together; the raw Melbourne-Wubbena (MW) combination of every such station steps at the same epoch.
- **An estimate's wander.** The truth stays; the estimate moves. The record's case is E11 on DOY 146: the combined-mode code column moved 15 cm over about 4 h with formal sigma 0.00 to 0.02 m (docs/refound/NRF_network_ring_fixing_2026-11-14_part16.md:72). The column's own q allows 0.6 mm/h (part16.md:111), so the move came through the coupling of the dense carry and the gauge rows, not through the column's own data gain (the skeptic's W1; the shared wide-lane phase level is the record's suspected path, part16.md:69, 76, inferred).

The two need different responses. Loosening one column is right for a truth step. For a wander driven by coupling, loosening one column can hand the tension on to the coupled states (skeptic W2). The design therefore detects both, classifies them, and acts only on a confirmed truth step.

The skeptic confirmed: the description of the present guard; the pinned-pair rule and the free same-band column; the configuration values; that the post-fit pass already forms the rows a pooled signed statistic needs; the per-epoch bound of the detectable-bias table. The skeptic refuted or marked unsupported: every scalar "bleed" figure of the carry (3.8 h, 280 h, 13 to 32 h); the detectable-bias finding that the carry absorbs a step slowly; the prior-residual t values; the claim that a pinned pair is unwatchable; the 1 to 2 h sensitivity of the witness-datum part; the placement of the acting half in a measure-only observer. Each is carried below with its status.

**Where the skeptic found the design a retry of a falsified approach.** The design as first proposed (witness-datum part) raised the loosened column's q to a watch value q_w for as long as it stayed loose. That is a local, temporary form of the satellite code q raise that the record found to be "the carry giving up" rather than a better model (docs/record/SPEC_cb_carry_physical_q.md:157, 161, 171; old posture, so a lead, not a verdict). A loosened column under the dense carry without a stated cross-covariance rule repeats the class of the ensemble phase-bias datum, whose cross-covariance "ran the clock away" (project falsified-approach list, entry "Ensemble PB datum"). Loosening a pinned-pair column would be process noise on a non-estimable direction, which that entry forbids. A response that moved or released a held integer on code residuals would repeat the near-null guard, falsified on the simulator on 2026-11-17 (NRF part14 section 108.1; ledger row network.ring_fix.nn_guard, KEEP_OFF). The skeptic's verdict: not a retry provided (a) the response acts only on estimable departures (free columns, with a cap per channel), (b) it never touches integers, (c) it clears the record's stop rule on structure function and code sigma0. This document drops the sustained q_w, specifies the dense fold, gives pin signals a declared re-reference instead of a loosen, and bars every integer action. A plain q raise, or a loosen without the cross-covariance rule, would still be a retry.

---

## 1. What the present guard cannot see

The present guard is `network.cb_excursion_reinit`. It tests satellite clock deltas, not code columns: the median over satellites of |x_clk(t) - x_clk(t-1)| against `network.cb_excursion_median_m` (default 50 m), with at least 8 satellites, suppressed on any datum change (src/core/network/qc/ClockQc.cpp:433-470; defaults src/core/config/GnssConfigLoadNetwork.cpp:7996-8002). It is absent from RC (no match), hence off. Confirmed by the skeptic (H1) and by the record, which calls it a step detector that "would not fire on a drift at all" (docs/record/SPEC_cb_carry_physical_q.md:175).

It cannot see:

1. **One satellite.** The median ignores a single satellite by design (ClockQc.cpp:419-422). The per-satellite list of jumpers is computed but only counted (`cb_exc_nonpivot_would`), and only on an epoch that already tripped (ClockQc.cpp:485-490).
2. **Anything under the common-mode bar.** Every event from centimetres to metres has median |delta clock| below 50 m.
3. **A code step that does not move the clock.** Phase holds the satellite clock, so a code-bias step leaves the statistic flat (inferred).
4. **A drift.** A wander of 0.3 mm per epoch (the E11 rate, part16.md:111) is invisible to any per-epoch delta test.
5. **Non-pivot satellites.** By default only previous pivots are reinitialised (ClockQc.cpp:477-484).
6. **Datum-change epochs and epochs with fewer than 8 satellites** holding a previous value (ClockQc.cpp:460).

When it trips, the marked satellites' code carry is reinitialised at the next stash (BorderStash.cpp:284-287; Responses.cpp:282), a CB_EXC discontinuity is queued (ClockQc.cpp:496-497, 512-515), and the run-end line is `networkCbExcursionRunEnd` (ClockQc.cpp:525-559). It has one edge: the re-coupling is the next epoch's fold, with no event (a gap against invariant 7).

**What can be wrong at all.** Under the reference code datum (time-linked code datum, time-linked GF row on the broadcast group delay, family difference on; `sat_isc_state`, `sat_gf_extra_band_free` and `code_datum_pair_continuity` absent from RC and RS, so at their defaults, off per GnssConfigLoadNetwork.cpp:3750, 3757, 8387-8388 as cited by the witness-datum part):

- The ionosphere-free pin pair is fixed completely: the IF and GF rows "together leave nothing estimable on the pinned pair" (src/core/network/solve/Sbasis.cpp:1514-1516). Within one epoch nothing on the pair is estimable.
- Free columns exist: the same-band companion of the first pin signal (GPS C1W beside C1C; `n_sat_gf_sameband_free`, Sbasis.cpp:1666-1669), Galileo secondary-family columns (C1X, C5X) when codes are kept apart, and extra-band columns the data determine. Each is free only relative to its channel's carried level (the veteran sum, Sbasis.cpp:380-402, or the family sum).
- E11's class (C1X/C5X with codes kept apart) is merged away where `network.phase_family_codes` is armed (NRF section 118; docs/refound/CHA_chain_audit_2026-11-18_part3.md:100 lists it in the scoped tier of the integer-fixing configuration). Under RC it remains.

**Disagreement, settled toward the skeptic (W3).** The witness-datum part concluded that a pinned-pair column "cannot hold an estimable error" and is not to be watched. That holds within one epoch. Across epochs the pin rows are time-linked to the previous solved values (Sbasis.cpp:1430-1606, as cited; not re-read line by line), phase holds the satellite clock and the carried phase bias, and float ambiguities are constant over an arc. A true step on a pin signal therefore stays in that satellite's code residuals until arcs renew (inferred). It is watchable by the same statistic; its response differs (section 4).

---

## 2. The witnesses compared

Noise figures assume code sigma 0.30 m at zenith with elevation power 1 (RC:185, 188-189), about 15 stations per satellite (detectable-bias model), 30 s epochs. "15 cm step" is a truth step on one signal of one satellite.

| Witness (key, state in RC) | What it measures | Noise | Threshold | Report or act | A 15 cm step |
|---|---|---|---|---|---|
| Present guard `cb_excursion_reinit` (off) | median over satellites of per-epoch clock change | not relevant to code | 50 m median | acts: reinit of pivots' code carry, one edge | invisible (section 1) |
| Station residual trace `trace_station_residuals` (RC:304) | max \|v\|sqrt(w) per (station, satellite) over PHASE rows on carried ambiguities (Raim.cpp:318-419, 575-580) | phase level | none | trace only | not a code witness; sees only what leaks into phase |
| Keyed post-fit pools `trace_postfit_keyed` (RC:273), SAT row | per satellite: n_code, s0_code = 1.4826 median\|v sqrt(w)\|, rms, max (FrameDiag.cpp:228-253, 343-345) | unsigned; spread between satellites | none | trace only, no edge | s0_code up about 16 %, rms about 12 %: inside the spread between satellites (inferred) |
| Signed post-fit rows `trace_postfit_rows` (RC:385) | signed v per (station, satellite, signal, kind) with w and elevation (FrameDiag.cpp:256-339) | 0.30 m per row at zenith | none | trace only | each row about 0.5 sigma; the raw material of the proposed watch |
| Estimable block-Baarda `block_baarda_estimable` (RC:381, 954) | per (station, line of sight) block form T_B over redundancy directions, t_B = T_B / sigma0_f^2 (BlockBaarda.cpp:102-130, 186-201) | per station, sigma0 per family over 60 epochs | t_B > 4, stride 10 epochs | reports only ("Nothing here writes a weight, a state or a decision", BlockBaarda.cpp:7-11); onset event, no return | t_B about 0.06 to 0.25 at every station: invisible; never pooled over a satellite's stations |
| Emitted code-bias jump test `code_bias_disc_jump_test` (RC:237, 334) | \|b(t) - b(t_prev)\| of the EMITTED bias per (satellite, signal) (NetworkProductEmitter.cpp:186-196, 816-827) | estimate's per-epoch motion | 0.25 cycle x lambda: 4.8 cm (L1, E1), 6.1 cm (L2), 6.4 cm (E5a) | marks the stream discontinuity only, one counter (:825, 834-842) | sees the estimate, not the truth; never trips on a stiff column; E11's 0.3 mm per epoch against 5 cm (part16.md:111) |
| MW detector per station `cycle_slip.mw_detect` (RC:239) | raw MW per arc, gate k max(sigma_arc, 0.01 m), k = 8 (main: src/core/slip/MwSlipDetectorStats.cpp:280, 341) | about 0.2 m at 30 s (inferred) | about 1.6 m | acts: wide-lane slip refloat of the station's arc (SlipQc.cpp:775): wrong subject | step in MW is f1/(f1+f2) d: 0.56 d (L1), 0.57 d (E1); 8.4 to 8.6 cm against 1.6 m: invisible |
| RAIM prior channel `raim_prior_channel` (RC:931) | station coupling rows (ambiguity, slant iono, colour, rx code bias, wet delay), median polish over (station, subject), interaction test at 6 (RaimPriorChannel.cpp:85-201) | robust per table | 6 sigma on interactions | acts on rx code bias, arcs, wet delay | invisible by construction: the satellite column is a shared prior, not a station row, and a satellite-wide effect is removed by the polish (:165-172) |
| RAIM observation channel | phase rows only (Raim.cpp:288-330); satellite column only when sigma0 > 3 (:720-722) | phase level | sigma0 3 | acts: refloat of the satellite's ambiguities network-wide | wrong subject; not a code witness |
| Prior-residual report, family cb (RC:376-379) | e = carried - solved per (satellite, signal), dispersion fold_var - post_var, empirical family sigma0 (StashStatus.cpp:438-480; PriorResidual.cpp:8-11) | calibrated by the family's own sigma0 | \|t\| > 4 | measures only | per-epoch t well below 4; the first-stage values (0.7 and 0.08) are unsupported: they ignore the empirical scale (a variance factor near 13 in the old posture would shrink t about 3.6 times, skeptic W5) and the S of 0.045 m^2 is inconsistent with the gain it was built from (W1) |
| GF row against the broadcast group delay | nothing after first sighting: the time link targets the previous solved GF value; the latch is used only at a first sighting and a return (Sbasis.cpp:1588-1614) | not relevant | none | run-start report only | invisible; a per-record comparison would be a datum-change reporter with a 7 cm floor (one broadcast step, ApplicationNetworkSetup.cpp:750) |
| Pin-pair re-election (Sbasis.cpp:38-80) | signal set changes only | not relevant | none | re-seed and discontinuity mark | no response; a continued re-election can PROMOTE a wrong free column into the datum (inferred), which the watch must bar |
| **Proposed: signed pooled code mean** | weighted mean of the code post-fit residuals on (satellite, signal) over the stations that see it, channel mean removed, over 240 epochs | measured across satellites (section 3) | 4.5 measured sigma | report; act only after confirmation | about 3 sigma at n=10, 4.5 at about n=20, under 5 min multipath (section 3) |
| **Proposed confirmation: pooled MW step** | mean over the satellite's stations of the raw MW change at the estimated onset, 20 epochs each side | about 6 cm per station, 1.4 cm pooled at n=20 (white, inferred) | 4 sigma, sign agrees with the code mean | confirms a step; cannot confirm a drift or a code step with f1 d1 + f2 d2 = 0 | 8.5 cm against 1.4 cm: about 6 sigma (white, inferred); less under correlated MW multipath |

[corrected 2026-11-19: see Corrections, item 3] Summary of the table: none of the eleven existing statistics can see the step; the only ones whose noise falls with the number of stations are trace-only rows that no statistic reads.

---

## 3. Detectable bias and the thresholds taken from it

**Model** (detectable-bias part; script D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/SCW2_satellite_code_level_watch_2026-11-18_parts/_scratch/detectable-bias/mdb_sat_code_column.py, output out.txt beside it). Code row at station r after the phase-determined terms are removed: y_r = b^s + b_r + e_r, Var(e_r) = 0.30^2 / sin^2(el) (StochasticModel.cpp:129-136; RC:185, 188-189). Stations uniform inside the 15 degree cap (RC:34): E[sin^2 el] = 0.412, effective per-station sigma 0.467 m, about 15 of 58 stations see a GPS satellite (Galileo 16). Two-sided alpha 0.001 per test, power 0.8 (lambda 17.07). Multipath case: AR(1) with a 5 min correlation time on the whole code error (pessimistic bound). t80 = 80th percentile of the first crossing, Monte Carlo, 1500 trials.

| Witness | sd n=10 (cm) | MDB n = 5 / 10 / 20 / 30 (cm) | t80, 15 cm, n=10 / 30 (min) |
|---|---|---|---|
| WHITE: one epoch | 14.8 | 86.6 / 61.2 / 43.3 / 35.3 | 213 / 16 |
| WHITE: mean of 60 epochs | 2.1 | 12.5 / 8.8 / 6.2 / 5.1 | 18 / 11 |
| WHITE: mean of 240 epochs | 1.35 | 7.9 / 5.6 / 3.9 / 3.2 | 56 / 37 |
| MULTIPATH 5 min: one epoch | 14.8 | 86.6 / 61.2 / 43.3 / 35.3 | >480 / 155 |
| MULTIPATH: mean of 60 | 7.9 | 45.9 / 32.5 / 23.0 / 18.7 | >480 / >480 |
| MULTIPATH: mean of 240 | 4.3 | 25.0 / 17.7 / 12.5 / 10.2 | >480 / 113 |
| MULTIPATH: one-signed mean of 240 | 4.3 | 23.8 / 16.9 / 11.9 / 9.7 | >480 / 109 |

Ramps (one-signed, 240 epochs): a ramp at the carry's own pace (below 1 mm/h) is never caught and needs no watch; an E11-like 37.5 mm/h ramp is caught after 132 to 139 min (white, n=10) or 254 to 258 min (multipath, n=30), at or after the 4 h peak of the observed wander.

Multipliers on any MDB: false-alarm budget of one per network-day (60 satellites, alpha 5.8e-6, z 4.53): x1.30; receiver code term per epoch over 8 satellites instead of carried: x1.22; worst decile of station geometry: x1.26 (n=5) to x1.09 (n=30); a column fed only by second-channel rows at sigma x10 (RC:342): x10 (which columns are second-channel only was not traced).

**Status of the numbers after the skeptic.**

- The per-epoch bound is confirmed (H5): S is at least the pooled R (about 0.47^2/15 = 0.015 m^2), so one epoch cannot give more than about 1.2 sigma on 15 cm.
- The MDBs are NOISE FLOORS of a residual statistic and stand as lower bounds. How much of a step the dense solve leaves in the code residuals, instead of moving it into receiver code biases, per-line-of-sight slant ionosphere or the satellite clock, is not known. The scalar model leaves all of it in the residuals, so the true MDB is larger by the inverse of that share.
- Finding 4 of the detectable-bias part ("the carry absorbs a step slowly; a residual witness and a frozen-anchor witness give the same MDB within 3 %") rests on a scalar gain. RC sets `shared_carry_full_cov` true (RC:309), which makes the satellite code column a dense-border route (ParamRegistry.cpp:32-33, 134). The finding is **unsupported** for the dense carry; the E11 record contradicts the scalar picture. The witness-residuals F2 figures (3.8 h from ClockQc.cpp:417-419, a figure for the pivot under excursion pressure; 280 h) are likewise unsupported.
- [corrected 2026-11-19: see Corrections, item 2] The witness-datum part's sensitivity (5 sigma in 1 to 2 h at 10 stations, from a model-based effective count) is **refuted** by the multipath rows: under 5 min correlation the 60-epoch mean has 7.9 cm sd at n=10. Its statistic T_c, normalised by a model sigma, would generate false alarms: the detectable-bias part measured per-test false-alarm rates of 0.23 (10 epochs), 0.42 (60) and 0.45 (240) instead of 0.001 when the white sigma is used under 5 min multipath.

**Thresholds taken.**

- **T1. Statistic.** For each (satellite s, signal j) column c with code rows at n_c stations: g_c(t) = (h' W r) / (h' W h) from the post-fit code residuals r on the column (h its row entries, W the row weights), minus the weighted mean of g over the other veterans of c's channel (a common-mode shift of the channel must not trip; it belongs to the receiver code biases). Window mean m_c over 240 epochs (2 h at 30 s), evaluated every 10 epochs. Formal sd s_c = sqrt(mean of 1/(h'Wh)) / sqrt(240).
- **T2. Normaliser measured, not modelled.** t_c = m_c / (kappa s_c), with kappa = 1.4826 median |m_k / s_k| over the columns k of the same constellation and signal at the same evaluation (at least 8 columns, else unarmed). This is the writer's specification of the detectable-bias part's "sigma measured on the data": it is available after the first window, needs no history, and absorbs multipath correlation because every column of the group shares it (inferred; to be checked on the uninjected simulator run, unit case 3). One stepped column is one of about 30 and moves kappa little; two stepped columns at once still leave the median robust.
- **T3. Threshold 4.5** (about one false alarm per network-day). Reported at the first exceedance.
- **T4. Station floor 10** contributing stations; below it the 2 h MDB exceeds 20 cm under multipath and the column is unarmed (counted). The skeptic's M2 stands: combined-mode receivers are about half the links (part16.md:74), so a Galileo X column may sit below the floor, which leaves the E11 class unarmed (inferred; author's decision 2).
- [corrected 2026-11-19: see Corrections, item 1] **T5. Scope of the claim.** About 18 cm (n=10) to 10 cm (n=30) in 2 h under pessimistic multipath, 6 to 3 cm white. A 5 cm requirement (the correction stream's 0.05 m discontinuity bar) is out of reach of code residuals alone and needs another witness (a multi-hour code-minus-phase level of the satellite, or an independent level; author's decision 8).
- **T6. Arc renewal.** After a true step, new arcs' slant ionosphere levels (set by code) can absorb the GF part, so the residual signature may decay with arc turnover (skeptic M3, inferred). A 2 h window is short against typical arc lives, but the decay is measured on the simulator, not assumed.

---

## 4. Trigger, confirmation, response

### 4.1 Trigger and confirmation

- **Trigger:** t_c > 4.5 (T1 to T3) on an armed column (T4).
- **Onset estimate:** the epoch that maximises a two-segment fit of g_c over the window (inferred design).
- **Confirmation:** the pooled raw MW step of satellite s at the onset: the mean over the satellite's stations of (mean MW over 20 epochs after) minus (mean MW over 20 epochs before), normalised by the cross-station robust spread of the same quantity, against 4, with the sign and size agreeing with the code mean (expected f1/(f1+f2) d on band 1, f2/(f1+f2) d on band 2) within 2 sigma. The raw MW series per arc is built in the network's scan (src/core/network/slip/PassASlipScan.cpp:165-177) from raw observables, so it is independent of how the solve split the misfit.
- **Classification:** `step` (confirmed), `wander` (no raw step: an estimate moving under coupling, or a truth drift), `unresolved` (fewer than 10 stations with a clean MW series across the onset, or the code-step direction with f1 d1 + f2 d2 = 0, to which MW is blind).

Only `step` leads to an act. `wander` and `unresolved` are reported with full identities and no state change (author's decision 1).

### 4.2 Response by column type (sub-key `_act`, default false)

**Free column, classified `step`: a bounded loosen.**

- **Decoupling edge.** The column's carried prior folds with the existing `CarryDisposition::Loose` (today set only for anchored satellite clocks under cta, CarryDisposition.cpp:89-102). The dense fold treats Loose like Folded with its raised increment (BorderPrior.cpp:761-764), unlike Reinit, which decorrelates the column at q = infinity (BorderPrior.cpp:758-760). The increment is sigma_w^2 = (0.30 m)^2, added once, on the column's diagonal of the dense block at the trip epoch; cross-covariances are kept as they stand. A non-negative diagonal increment keeps the matrix positive semi-definite and shrinks every correlation of the column automatically. This is the cross-covariance rule the skeptic required (W7). There is **no sustained q raise**: q returns to `sat_code_bias_q` (RC:166) from the next epoch, so the response is one bounded event, not the record's q lever.
- While loose, the column leaves its channel's veteran sum (and the Galileo family sum) through an input to the datum-row builder (CodeBiasDatum.cpp:390-405, as cited by the witness-datum part), so the other veterans do not absorb its move (without it a move of delta redistributes as -delta/(n-1), about 5 mm at n = 30, inferred). The channel row references each satellite to its own previous value, so the exclusion does not move the target (Sbasis.cpp:383-386).
- It is barred from election as a pin signal while loose (the promotion of section 2).
- A loosened Galileo X column is treated as born by the family prior (Sbasis.cpp:1948-1955) and re-folds the 0.10 m birth prior (RC:953): a second bound. It also pulls toward the previous family offset; pooled data dominate it within a few epochs (inferred).
- **Cap per channel** (skeptic M4): at most one loose column per (constellation, channel) at a time, and never fewer than 3 veterans left. If the loose columns emptied the veteran set, the channel row would fall back to a re-seed of the whole channel (Sbasis.cpp:397-400), an unintended re-reference. A trip that would break the cap is reported and refused.

**Pin signal, classified `step`: a declared re-reference.** Loosening a pinned column would be noise on a non-estimable direction (section 0). The satellite is placed in `this_epoch_cb_reseed` by the same pattern as the channel re-seed (Sbasis.cpp:2025-2040), which drives the stream's discontinuity mark and its counters. Which targets re-seed (the IF row only, letting the clock and phase bias take the IF part, or the IF and GF rows) is open (author's decision 4); the satellite clock and phase bias step by the IF part either way, which is a declared datum change (inferred).

**Bound while decoupled.** The single increment sigma_w; with the pooled R of about 0.015 to 0.03 m^2 per epoch, the posterior sigma is back near 0.1 m within one epoch and near 2 cm after 60 epochs (white, inferred). If the column moves more than 1 m from its pre-trip value, or is still loose after `_max_loose_windows` = 4 windows (8 h), it is withheld from the correction stream (a declared withhold; the emission withhold path was not checked) and stays loose.

**Re-coupling edge, with re-validation.** The statistic restarts at the trip from post-trip data only. Re-coupling requires t_c < 2.25 (k/2) over one full window and posterior sigma < 0.03 m, and the column's change since the trip consistent with the MW-implied step within 3 sigma. Then: the column rejoins its veteran sum with its new value as previous value (no target move), the election bar lifts, the stream discontinuity counter for (s, j) is bumped once (the emitted value moved), and a CB_WATCH recouple event records the change since the trip. A change inconsistent with the MW step is reported (`recouple_inconsistent`) and the column stays loose up to the cap of 4 windows.

**Integer bar** (skeptic M5). The response never moves, releases, re-floats or re-seeds a held integer or a hold row. The integer-fixing monitor decides on its own data. Telemetry only: the number of held links of satellite s at the trip and at the re-coupling, and the monitor's releases on s while loose.

### 4.3 Counters, events, identities

Counters (rows of the response table, kind SatCodeBias; Responses.cpp): `cb_watch_evaluated`, `cb_watch_armed_columns`, `cb_watch_below_floor`, `cb_watch_report_step`, `cb_watch_report_wander`, `cb_watch_report_unresolved`, `cb_watch_trip_free` (P::Loose at a bounded increment), `cb_watch_reref_pin`, `cb_watch_cap_refused`, `cb_watch_recouple`, `cb_watch_recouple_inconsistent`, `cb_watch_withhold`, `cb_watch_vetsum_excluded` (column-epochs).

Events: `CB_WATCH` with action report, trip, reref, refused, recouple, withhold; members SatId, SignalId, column type (pin or free), channel, n_c, m_c (m), t_c, kappa, onset time, MW step (m) and its t, classification, increment, posterior sigma at both edges, held links of s. Identities from include/common/GnssIdentity.hpp (SatId, SignalId); no string parsing of satellite or signal. Run-end: `networkSatCodeWatchRunEnd`, one row of `kNetworkRunEndReports`, one line per constellation (evaluated, reports by class, trips, refusals, re-couplings, withholds, columns loose at run end).

### 4.4 Where the code goes (skeptic W6)

- **Detector:** a per-epoch qc seam `networkSatCodeWatch(NetworkRun&, const NetworkEpoch&, ...)` in network/qc/SatCodeWatch.{hpp,cpp} (name proposed), called where post-fit rows are final. It is NOT a row of `kNetworkEpochObservers`: an observer writes only its own counters, trace rows, events and manifest records (include/network/driver/EpochObservers.hpp:29-36), and the window is cross-epoch state that the response reads.
- **Window and loose state:** entity columns of the satellite-signal entity, each with its declaration row in state/Entities.hpp and a `net::kEntityWriters` row; a new epoch-contract row in tests/network/test_network_epoch_contract.cpp; `nom_n4_verify.py` and `nom_epoch_lint.py` pass.
- **Residual formation:** one owner. The post-fit pass already forms v = y - A x for every row (FrameDiag.cpp:256-339). The formation is factored into a helper both the trace and the watch call, rather than duplicated (author's decision 7: this touches a trace path, explained and compared before it lands).
- **Acting half:** the loose set is read by `carryFoldDecide` (state/CarryDisposition.cpp) for the SatCodeBias kind and by the datum-row builder (veteran exclusion, election bar, cap) through their parameters, never through the seam.

### 4.5 Keys and ledger rows

JSON section `network`, loader in GnssConfigLoadNetwork.cpp beside the excursion keys, snapshot in NetworkKnobs:

- `network.sat_code_level_watch` (bool, default false): detector and reports.
- `network.sat_code_level_watch_act` (bool, default false, read only with the parent): the response; in the warm-resume fingerprint (it changes state).
- Parameters (each a PARAM row with the parent): `_window_epochs` 240, `_stride_epochs` 10, `_k` 4.5, `_min_stations` 10, `_min_group_columns` 8, `_mw_k` 4.0, `_loose_sigma_m` 0.30, `_recouple_sigma_m` 0.03, `_max_loose_windows` 4, `_max_loose_move_m` 1.0.

Ledger row text (docs/CONFIG_promotion_ledger.json, in the shape of the existing rows):

```
knob:    network.sat_code_level_watch
verdict: UNVALIDATED
area:    QcTiers
default: false
why:     Watch on the level of one satellite's code column: for each (satellite, signal)
         column with code rows at 10 or more stations, the signed 2 h mean of its pooled
         post-fit code residuals, its channel's mean removed, normalised by the robust spread
         of the same statistic over the columns of its constellation and signal; reported at
         4.5 and classified step, wander or unresolved by the pooled raw Melbourne-Wubbena
         step at the estimated onset. CB_WATCH events and run-end counters. Measures only;
         false is off and byte-identical. Pre-registered in SCW2 (2026-11-18), simulator
         event pending.
```

```
knob:    network.sat_code_level_watch_act
verdict: UNVALIDATED
area:    QcTiers
default: false
parent:  network.sat_code_level_watch
why:     Response of the code-level watch to a confirmed step: a free column folds Loose with
         one bounded increment (0.30 m) on its dense diagonal, cross-covariances kept, leaves
         its channel's veteran sum and the pin election while loose (one loose column per
         channel, at least 3 veterans kept); a pin signal is re-referenced through the
         re-seed path with a stream discontinuity. Re-couples after one window under k/2
         with sigma below 0.03 m and a change consistent with the raw step; withheld after
         4 windows. Never moves or releases a held integer. False is off and byte-identical;
         in the warm-resume fingerprint.
```

---

## 5. Unit cases

Suite `NetworkSatCodeWatch`, one new file tests/network/test_network_sat_code_watch.cpp (one owner per suite name); the injector case in the existing injector suite's file.

1. Statistic: g_c equals h'Wr/h'Wh on synthetic rows; the channel veterans' mean is removed.
2. Normaliser, white noise: 30 columns, 1000 trials; kappa within 5 % of 1 and the exceedance rate at 4.5 within Monte Carlo noise of nominal.
3. Normaliser, AR(1) code noise (phi 0.905): exceedance rate at 4.5 at most twice nominal, where the model sigma gives about 0.4 per test (the detectable-bias figure).
4. Step 15 cm on one column, n = 30, white: report within 60 min (table t80 37 min); classified `step` when the synthetic MW steps by 0.56 d.
5. Common-mode shift of every column of a channel: no report.
6. Ramp 37.5 mm/h with no MW step: classified `wander`, no act.
7. Station floor: 9 stations, column unarmed, `cb_watch_below_floor` counted; fewer than 8 columns in the group, unarmed.
8. Free column trip: disposition Loose, increment exactly 0.09 m^2 on the diagonal, off-diagonals unchanged, matrix positive semi-definite; next epoch q back to the configured value.
9. Veteran exclusion: the loose column is absent from the channel row's Jacobian, the row's target equals the sum of the remaining veterans' previous values.
10. Cap: a second trip in the same channel is refused and counted; a trip that would leave fewer than 3 veterans is refused.
11. Promotion bar: a pin re-election does not pick a loose signal.
12. Pin signal step: the satellite enters `this_epoch_cb_reseed`, the discontinuity counter moves once.
13. Re-coupling: under k/2 for a window with sigma below 0.03 m and a consistent change, the column rejoins with one discontinuity mark; an inconsistent change keeps it loose; at 4 windows it is withheld.
14. Integer bar: hold rows and held integers identical before and after a trip and a re-coupling.
15. Off: with the key false the seam returns at its first statement, no entity column is written, counters stay zero (epoch-contract row).
16. Injector kind `sat_code_step` (section 7, step 1): adds the step to one code signal of one satellite at every station from the onset, phase unchanged; the optional ramp adds rate x elapsed; the loader refuses a missing signal, a phase code or a zero step.

---

## 6. Validation (pre-registered; listed, none requested)

**No injection kind exists for a satellite code step.** The network injector (`network.inject`, src/core/network/carry/Inject.cpp) has `sat_clock_step`, which adds the step to every code and phase observable of the satellite (Inject.cpp:227-266), a clock event, not a code event. The kinds are validated at load (GnssConfigLoadNetwork.cpp:14066-14070). A new kind `sat_code_step` (code observable of one signal only, optional `ramp_m_per_h`) is step 1 of the plan. Because the injector edits the observation before the models derive both the metres and the raw cycles (Inject.cpp:229-233), the raw MW sees the step as a truth step would.

**The one injected event (simulator first).**

- Bed: a copy of RS (`scripts/rapid_arm.py`), 6 h, `start_offset_hours` 0.504167, Release exe, one build for every arm.
- Event: `sat_code_step` of 0.15 m on one GPS satellite's free same-band column (C1W beside C1C) at 02:30:15 after the start (settled, outside ignition), to the end of the run. The column is the one step 0 finds present and armed (at least 10 stations) on the simulator fixture; if the fixture carries no free GPS column, the event moves to the satellite's first pin signal and the pin re-reference is the response under test (pre-registered here, not chosen after the fact).
- Arms: A uninjected, watch measure-only (control, false alarms); B injected, key off (the step's own effect); C injected, measure-only; D injected, `_act` on; E uninjected, key off, against the build without the change (identity).
- Read-out without a bar: the share of the step left in the code residuals against what moved into receiver code biases, slant ionosphere (`iono_total_m`) and the satellite clock, from run B (network_pf_rows.csv, network_ionosphere.csv, the satellite bias states). This number decides the true MDB (section 3).

**Bars.**

- S1 detection: C reports column c within 120 min of the onset, onset estimate within 10 min, classified `step` with the right sign.
- S2 false alarms: A reports at most one column in 6 h (budget one per network-day).
- S3 benefit: D's truth error of column c from onset + 2 h to the end is at most half of B's; the held-out user SZZ000SIM by hour after the onset not worse than B by more than 2 mm; code sigma0 and the structure function of the emitted code biases not above B's (the record's stop rule).
- S4 integers: zero held integers moved, released or re-floated at or after the trip in D that are not also in C.
- S5 identity: E is byte-identical to the build without the change.

**Falsifiers.** S1 failing falsifies the witness for this column class at this station count. S2 failing falsifies the measured normaliser. S3 failing falsifies the response (a loosen that makes the column, the user or the stop-rule quantities worse is the carry giving up, not a fix). S4 failing is the near-null guard's failure repeated: the response is withdrawn, not tuned. S5 failing blocks the merge.

**Observed data, after the simulator (listed, not requested).**

1. One 24 h observed-data run of an RC copy, measure-only: kappa per group, reports per satellite-day, the inventory of `wander` reports.
2. The DOY 146 codes-kept-apart configuration of `n41_n146`, measure-only: whether E11 reports as `wander` from about hour 3 (part16.md:72).
3. The integer-fixing configuration, measure-only, 6 h.
4. If the response passes S1 to S5: 6 h RC and RS arms, `_act` on against off, same build, graded on the held-out user by hour and the 16-site set, hours 2 to 6.
5. The BeiDou lineage, measure-only, later.

---

## 7. Step plan (each step identical in output when the key is off)

- **Step 0 (no code, no engine run): inventory of free columns** (skeptic M1). From existing run traces: which (satellite, signal) columns are free under RC, RS, the integer-fixing configuration and the DOY 146 codes-apart configuration; their station counts; the change of each estimate by hour. The record saw "C1W - C1C drifting metres" in the old posture (docs/record/SPEC_cb_carry_physical_q.md:172). **Gate:** if no free column in the integer-fixing configuration moves more than 5 cm in 6 h and none is found by the observed-data inventory, the response (steps 4 and 5) is not built and the watch stays a monitor.
- **Step 1: injector kind `sat_code_step`** with the optional ramp, beside `addRangeStep` in Inject.cpp and the loader's kind list. Unit case 16. Absent is identical (the injector is off unless events are listed).
- **Step 2: detector, measure-only.** qc seam, entity columns, shared residual formation, CB_WATCH report events, run-end line, an optional trace stream written only under the key. Unit cases 1 to 3, 5, 7, 15. The shared residual formation is a refactor of a trace path: a same-build comparison of the post-fit traces with the key off before it lands.
- **Step 3: MW confirmation and classification.** Unit cases 4 and 6. Then the simulator arms A, B, C and E (listed): bars S1, S2, S5 and the read-out. Decision point for the author.
- **Step 4: response under `_act`.** Loose disposition for the SatCodeBias kind, veteran exclusion, election bar, cap, pin re-reference, re-coupling, withhold, integer bar. Unit cases 8 to 14. `_act` false is identical to step 3.
- **Step 5: simulator arm D (listed)**, bars S3 and S4; then observed data items 1 to 5; then a ledger verdict by the author.

---

## Disagreements between the parts, stated plainly

- **Pinned pair watchable?** Witness-datum: no. Skeptic: yes across epochs. This document follows the skeptic (inferred) and gives pin signals a re-reference, not a loosen.
- **Sensitivity.** Witness-datum: 15 cm at 5 sigma in 1 to 2 h at 10 stations. Detectable-bias and the skeptic: about 3 sigma at 10 stations in 2 h under 5 min multipath. The latter is used.
- **Normaliser.** Witness-datum: model sigma plus a multipath correction. Detectable-bias: measured sigma. This document specifies the measured sigma as the cross-satellite robust spread (the writer's choice, unit case 3 and bar S2 test it).
- **Sustained q_w while loose** (witness-datum). Dropped after the skeptic's W2: one bounded increment instead.
- **Best confirmation.** Witness-residuals and the skeptic: pooled MW. Kept, with the limits of section 4.1.
- **Seam.** Witness-datum: a row of the epoch observers. Skeptic: a qc seam with declared state. The skeptic is followed.
- **Bleed time of the column.** All three first-stage parts used a scalar gain; the skeptic showed the carry is dense in RC. No bleed figure is used in this document.

---

## Decisions for the author

1. **The E11 class** (an estimate wandering under coupling, no truth step): report only, as proposed, or a response of its own (which would need a different mechanism, on the coupling, not on the column).
2. **Station floor** of 10 against a lower floor or a longer window for Galileo X columns, where combined-mode receivers hold about half the links.
3. **Default-off identity of the response table:** print the new counter rows only under the key, or accept an exception to the default-path identity rule (the family-row precedent, Sbasis.cpp:2045-2054, chose not to add a line to every run's table).
4. **Pin-signal re-reference:** re-seed the IF row only, or the IF and GF rows.
5. **After 4 loose windows:** withhold from the correction stream, as proposed, or leave the column loose and only report.
6. **Whether to build the response at all** before step 0 shows a wandering free column in the integer-fixing configuration.
7. **One residual formation** shared by the post-fit trace and the watch (a refactor of a trace path, compared before it lands), or the watch keeping its own copy.
8. **The 5 cm level** is out of reach of code residuals: whether a code-minus-phase level witness of the satellite is a separate package.

## Not checked

- The integer-fixing configuration's JSON was not opened: which free columns exist there, and whether `sat_isc_state`, `sat_gf_extra_band_free` or `code_datum_pair_continuity` are armed in it.
- Whether the simulator fixture carries a free GPS column (C1W) at 10 or more stations; which signals it simulates was not read.
- How a step divides between code residuals, receiver code biases, slant ionosphere and the satellite clock under the dense carry: reasoned only; the read-out of run B measures it.
- The actual per-epoch gain and cross-covariances of a free code column in the dense carry: no run was read.
- MW noise at 30 s on the reference stations (0.2 m assumed), its correlation time, and whether pooled multipath is independent across stations.
- The decay of the residual signature with arc turnover (T6).
- The cross-satellite normaliser's behaviour on observed data (bar S2 is on the simulator only).
- The emission withhold path a withheld column would use, and the CodeBiasDatum.cpp:390-405 interface the veteran exclusion would feed (cited from the witness-datum part, not re-read).
- Whether adding entity columns and contract rows changes any run-start table when the key is off.
- Lines cited from the first-stage parts and not re-opened here: Raim.cpp, BlockBaarda.cpp, FrameDiag.cpp, StashStatus.cpp, PriorResidual.cpp, NetworkProductEmitter.cpp, BorderStash.cpp, Responses.cpp, ApplicationNetworkSetup.cpp, Sbasis.cpp:38-80, 1430-1606, and main src/core/slip/MwSlipDetectorStats.cpp. Re-opened and confirmed for this document: Sbasis.cpp:380-402, 1512-1517, 1664-1670, 1948-1956, 2025-2056; CarryDisposition.cpp:85-105; BorderPrior.cpp:755-766; Inject.cpp:222-281; RC:166, 185, 237, 309, 334, 376-379; RS:164, 183, 232, 304, 329, 371-374; part16.md:70-76, 109-112; SPEC_cb_carry_physical_q.md:157-175; the ledger row schema and the injector rows.

## 2026-11-19: corrections from the check against the code

The check read network code and configurations on `7fb0737` (D:/GnssProcessorSupport/worktrees/read_7fb0737) and the user slip code on main `5ddc422`, and re-derived the detection figures with `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/_critical_read/_scratch/SCW2/check.py`. Its verdict: usable with corrections; the inventory conclusion stands, while the detection figures at the adopted threshold, the strength of the Melbourne-Wubbena confirmation and the reading of the E11 case need correcting. Items 1 to 3 are ruled wrong and are marked in the body; items 4 to 14 are unconfirmed or incomplete statements, not marked. The original words stand above. RC = `config/golden/golden_field_refound.json` (7fb0737).

1. **The draft's Outcome and section 3, T5: "about 18 cm at 10 stations and 10 cm at 30 stations in 2 h".** Those detectable biases are at alpha 0.001 (z 3.29, lambda 17.07). At the adopted threshold k = 4.5 the same model gives 22.9 cm (n=10) and 13.2 cm (n=30) under 5 min multipath, 7.3 and 4.2 cm under white noise; section 3 itself calls them lower bounds (`_parts/_scratch/detectable-bias/out.txt`; `check.py`). Corrected T5: at alpha 0.001, 18 cm (n=10) to 10 cm (n=30) under 5 min multipath, 6 to 3 cm white; at the adopted 4.5, 23 to 13 cm and 7 to 4 cm. The Outcome above is rewritten accordingly.
2. **Section 3, bullet on the witness-datum part: "measured per-test false-alarm rates of 0.23, 0.42 and 0.45".** These rates are computed analytically (erfc, the white-calibrated block of `mdb_sat_code_column.py`), not measured.
3. **Section 2, summary: "none of the eleven existing statistics".** The table has twelve rows, and it omits the armed gross line-of-sight screen (per line-of-sight code residuals at k = 8, with the satellite rule `gross_los_sat_min_stations`) and the code class of the variance factor (`network.vcf`) (RC:938-939, 942-945; `src/core/config/GnssConfigLoadNetwork.cpp:7228-7233, 7325-7327`; `src/core/network/qc/GrossLos.cpp:55-96`, 7fb0737). Neither sees 15 cm (k = 8 against rows of 0.3 to 0.5 m), so the conclusion stands. Corrected: none of the twelve statistics in the table, nor the armed gross line-of-sight screen (k = 8 per line of sight) or the code variance factor.
4. **Section 0: "so the move came through the coupling of the dense carry and the gauge rows, not through the column's own data gain" (unconfirmed).** At the record's formal sigma of 1 to 2 cm and 7 to 15 stations the scalar gain is 3e-3 to 1.3e-2 per epoch (time constant 40 min to 2.6 h), enough to move the column decimetres in 4 h; the skeptic's 3e-5 to 6e-4 used the model's steady state (2 to 6 mm), not the recorded sigma (arithmetic; `docs/refound/NRF_network_ring_fixing_2026-11-14_part16.md:72`, 7fb0737). Corrected: the record does not show which path moved it; at the recorded formal sigma the column's own data gain could also move it decimetres in 4 h (inferred both ways).
5. **Sections 0 and 1: "the combined-mode code column moved 15 cm" and "E11's class ... Under RC it remains" (qualified, unconfirmed).** The recorded quantity is W, the wide-lane combination of (C1X - C1C) and (C5X - C5Q) centred over satellites, in configuration n41_n146 with phase families on, not one column under RC (part16.md:72). RC has no `network.phase_families`; the suspected path is the shared wide-lane phase level of the phase-family table, and `phase_family_codes` is read only with `phase_families` (part16.md:69, 105). Corrected: under RC the C1X and C5X columns exist, but the phase-family coupling the record suspects is absent.
6. **Sections 2 and 4.1, pooled MW confirmation: "8.5 cm against 1.4 cm: about 6 sigma" (unconfirmed, overstated).** The arithmetic holds for white 0.2 m noise. With the document's own 5 min AR(1) applied to MW, the pooled t is 1.3 (n=10) to 1.8 (n=20), below the bar of 4; and 0.2 m is below the MW noise the code sigma implies (about 0.7 of the code noise) (`check.py`). Most true steps would be classified `wander`.
7. **Section 3, T3: "Threshold 4.5 (about one false alarm per network-day)" (unconfirmed).** z 4.53 was derived for 60 satellites tested at every 30 s epoch; with a stride of 10 epochs and 60 to 150 columns the same budget gives z 4.0 to 4.2 before window overlap, so 4.5 is conservative, not matched (`mdb_sat_code_column.py`, `za_day`; `check.py`).
8. **Section 4.5 and decision 3: the response "default off and byte-identical" (unconfirmed).** Every response-table site goes to the run manifest record `param_responses` whether armed or not, so new rows change every run with the key off unless that walker changes (`src/core/network/assembly/Responses.cpp:499-509`, 7fb0737). The ledger text asserts what decision 3 leaves open; corrected ledger text: false is off; identity of the run manifest's `param_responses` record pending decision 3.
9. **Section 3, multipliers: "which columns are second-channel only was not traced" (traceable).** GPS priority puts C1W first and the default second-code list is C1C, C1W, so under RC the C1W column is fed at sigma x1 and the pinned C1C rows are second-channel rows at sigma x10 at stations tracking C1W (inferred from RC:43-45, 341-342 and `include/config/GnssConfigTypes.hpp:2430`, 7fb0737).
10. **Section 4.3: "P::Loose".** No such policy exists; the names are `P::Loosen` and, at the re-coupling, `P::Recouple` (`include/network/assembly/ParamRegistry.hpp:88-110`, 7fb0737).
11. **Section 0: the ensemble phase-bias datum entry.** The content is confirmed, but its source is the project's auto-memory list of falsified approaches (`project_network_falsified_approaches.md`, entry "Ensemble PB datum"), not `docs/record`; it is cited as such.
12. **Section 1 (addition).** The guard `network.cb_excursion_reinit` also has a ledger verdict KEEP_OFF ("Pivot-only scope falsified on DOY-129"), not cited in the draft (`docs/CONFIG_promotion_ledger.json`, 7fb0737).
13. **Section 6 (addition).** `scripts/nrf_inject_code_bed.py` and `scripts/nrf_code_bias_bed.py` edit constant per-link code biases into a simulator fixture, without an onset; the absence of a satellite code step injection kind is confirmed (`GnssConfigLoadNetwork.cpp:14062-14067`; `src/core/network/carry/Inject.cpp:229-264, 517-529`, 7fb0737).
14. **Section 2, block-Baarda row (refinement).** The t_B values 0.06 to 0.25 are computed, not marked inferred.

### Line numbers that moved

The claims at these citations are confirmed; the deciding lines differ (all 7fb0737 unless main is named):

- `src/core/network/qc/ClockQc.cpp:419-422` -> 420-422.
- `src/core/config/GnssConfigLoadNetwork.cpp:7996-8002` -> 7996-7999.
- `src/core/network/state/BorderStash.cpp:284-287` -> 285-287; `src/core/network/assembly/Responses.cpp:282` -> 281-282.
- `src/core/network/qc/RaimPriorChannel.cpp:85-201` -> the median polish and its actions at 153-201.
- `src/core/network/assembly/ParamRegistry.cpp:32-33, 134` -> 32-33, 126-137; the dense route also reads `src/core/network/driver/Knobs.cpp:96`.
- `src/core/network/solve/Sbasis.cpp:383-386` and 397-400 -> 383-401; 1948-1955 -> 1952-1953; the GPS pin pair C1C/C2W at 61-75 and 1342-1349.
- `src/core/network/carry/Inject.cpp:227-266` -> 229-264; the loader's kind list `GnssConfigLoadNetwork.cpp:14066-14070` -> 14062-14067.
- The network code rows take the stochastic-model weight at `src/core/network/assembly/Contributions.cpp:118`; MW gate parameters on main at `include/config/GnssConfigTypes.hpp:5073` and `include/slip/MwArcState.hpp:93`.

### What stands

- The central finding: no armed statistic sees or acts on a 15 cm step of one satellite's code column; the added screens (item 3) do not change it.
- The dense-route point; the scalar bleed figures are correctly marked unsupported.
- The detection claim is weaker: at the adopted threshold a 15 cm step is detectable in 2 h only at roughly 25 or more stations under 5 min multipath. The simulator bar S1 (quiet fixture) may pass while observed data would not.
- The response half is weaker than written: under the document's own multipath assumption the MW confirmation rarely reaches 4, so `step` would seldom be declared.
- The E11 case motivates the watch but comes from a phase-family configuration; its relevance to RC is unshown. The inventory gate of step 0 stands and includes RC explicitly.
- The response is designed as an event at both edges, with counters and SatId/SignalId identities, default off; its identity when off is open (item 8). As specified it does not repeat a falsified approach (no sustained q, no integer action, no loosen on a pinned direction).

The decisions put to the author, after correction:

1. The E11 class (report only, or a response of its own on the coupling): stands; with the confirmation rarely reaching its bar, more reports than expected fall in `wander`, which bears on this decision.
2. Station floor of 10 against a lower floor or a longer window for Galileo X columns: stands.
3. Default-off identity of the response table: stands, and covers the run manifest record `param_responses` as well as the printed table (item 8).
4. Pin-signal re-reference, IF row only or IF and GF rows: stands.
5. After 4 loose windows, withhold or report only: stands.
6. Whether to build the response at all before step 0 shows a wandering free column: strengthened; the monitor alone until step 0.
7. One residual formation shared by the post-fit trace and the watch: stands.
8. The 5 cm level out of reach of code residuals, and a code-minus-phase level witness as a separate package: stands; the reach at the adopted threshold is 23 to 13 cm (item 1).

### Left unconfirmed

- Lines not re-opened: `PriorResidual.cpp`, `StashStatus.cpp`, `CodeBiasDatum.cpp:390-405`, `FrameDiag.cpp` beyond the pool and row blocks, `ApplicationNetworkSetup.cpp` beyond line 750.
- The signal-integrity count, the redundancy and congruence screens and the MDB report as possible witnesses (their banners read measure-only).
- Whether the simulator fixture carries C1W at 10 or more stations; the free columns of the integer-fixing configuration.
- The Monte Carlo time-to-detect values were compared with `out.txt`, not rerun.

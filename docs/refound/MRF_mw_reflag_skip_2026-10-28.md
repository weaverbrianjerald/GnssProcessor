# MRF: one slip frees a carried arc once (the MW re-flag)

**STATUS**

- **id:** MRF
- **topic:** slip
- **status:** open
- **summary:** BUILT and CHECKED 2026-10-28, default off, UNVALIDATED, not armed. One cycle slip freed a carried arc twice: the receiver's loss-of-lock flag (LLI) or another response freed it at its epoch, and the Melbourne-Wubbena (MW) detector, whose arc statistics the uncombined LLI policy keeps, re-detected the same step later against the pre-slip level (an unlocked arc one epoch on; a locked arc three to four samples on, after absorbing the step and demoting its lock) and the drain freed it again. Field golden 6 h: 15 MW flags re-detected a step from an earlier epoch (30 to 420 s before), 11 of them on a band another response had already freed. The detector now reports the epoch each flagged step entered the data, and `network.cycle_slip.mw_reflag_skip` stops the MW drain from freeing a band re-levelled at or after it. Off: byte-identical on 2 h arms of both goldens and the field 6 h, on the 16 user-side arms and on the real-time arms, apart from one run-end line. On, field 6 h: BSHM00ISR / G19 freed once per slip, 18 of 30 re-flag bands not freed again, MW refloats 43 to 36, held-out user within 0.35 mm by hour (hours 2 to 6), 16-site census median 0.0 mm. No benefit shown.
- **source:** the 29 October status deck (the BSHM00ISR / G19 figures, `fig_slip_real_pass.py`, `fig_carry_arc.py`); run `runs/_rf0hv15_field6` (the field golden baseline of card GAD)
- **knobs / posture:** new `network.cycle_slip.mw_reflag_skip` (bool, default false, ledger row UNVALIDATED); both goldens leave it off; it requires `network.cycle_slip.mw_detect`
- **dates in the text:** 2026-10-28
- **hashes / run dirs:** branch `claude/slip-double-refloat-a56896` from main `7bdcebc`; base exe `gnss_processor_sdr_base.exe` (main `7bdcebc`, sha256[:16] `4ebb0b607741c8ea`), candidate exe `gnss_processor_sdr.exe` (sha256[:16] `7df467d1a187fea8`); field arms under `D:/GnssProcessorSupport/runs_nom/`: `_rap_sdr_dbg2_f6` (the detector's decision lines, with `bshm_g19_capture.txt`), `_rap_sdr_{base,off}_{f2,s2}` (the off path, 2 h), `_rap_sdr_dbg_f6` (base exe, 6 h) and `_rap_sdr_{ctl,on}_f6` (the field 6 h pair), census `_rap_sdrc_{ctl,on}_<site>`; user-side gate `_uom_sdr{ub,uc}` and real-time gate `rtseam_goldens/sdr{b,c}` under `D:/GnssProcessorSupport/gates/mrf/`, with the scripts of the re-flag census of the baseline `runs/_rf0hv15_field6` (section 1: `reflag_census.py`, `reflag_series.py`) and of the gates (`mrf_grade.py`)
- **gates (frozen at pre-registration):** section 6, written before any run with the knob on

**Outcome.** Question: why does one slip free a carried arc twice, and can the second free go safely? The MW detector keeps its arc across an LLI epoch and re-detects the step: one epoch later on an unlocked arc, three to four on a locked one (absorbed, demoted, then flagged on a noise draw), and the drain frees the arc again. The detector now dates each step; `mw_reflag_skip` (default off) frees no band re-levelled since. Field 6 h: BSHM00ISR / G19 freed once per slip, 18 of 30 re-flag bands skipped, held-out user and census within 2 mm; bars B0 to B4 hold. Standing: UNVALIDATED, not armed (section 8).

## 1. The defect, as observed

Field golden, 8 May 2024, 6 h (`runs/_rf0hv15_field6`). Station BSHM00ISR, satellite G19, signals L1C and L2W.

1. TOW 275040 (04:24:00): the receiver sets loss of lock on both bands. `network_events.csv`: `T0_band_scope common_band keep rule=lli_epoch;implicated=L1C;L2W;basis=lli=L1C;L2W;mw=withheld`, `T0_mw_detect lli_slip refloat LLI;mw_step`, `T1_if_slip refsat_reset refloat L1C;L2W`. `network_amb_state.csv`: `drop:relevel_free` on both bands (variance back to the birth value, 2.88 m2 on L1C, 7.81 m2 on L2W).
2. TOW 275130: a band-scope row with `basis=mw(L1C+L2W)flag`, and no MW action. TOW 275160 (04:26:00): `T0_mw_detect mw_wl_slip refloat L1C;L2W` and `drop:relevel_free` again on both bands. The L1C variance had settled from 2.88 to 0.0227 m2 over the four epochs in between and is thrown back to 2.88.
3. `network_slip_raw.csv`: the raw MW is -12.5 to -12.8 m before 275040 and +0.2 to +0.5 m after, flat. There is no second step: the late flag is the first step seen again.
4. The arc's second slip, TOW 279000 (05:30:00), flag on L2W only: `rule=lli_epoch;implicated=L2W;narrowed=1`, `lli_slip refloat LLI;mw_step` (L2W), `if_slip refloat L1C;L2W;cb_kept=L1C`; TOW 279030: `mw_wl_slip refloat L1C;L2W`. L2W is freed at 279000 and again at 279030; L1C at 279030.

Not one event. Of the 43 MW re-floats in the 6 h run, 15 follow another free of the same station and satellite within 300 s (an LLI 3, an IF-TD flag 7 plus the reference-satellite reset above, a T4 attribution 2, a gap re-entry 2), at a lag of 1 to 9 epochs; 8 of them fall in hour 1 at MAC100AUS. In 11 of the 15 the raw MW of the primary pair shows no second step between the two frees. In 2 it shows one (KIT300UZB / G07, -2.0 m at the MW epoch with a -2.7 m code step; MAC100AUS / G32, +0.94 m, 1.1 cycles), and 2 are unclear (MAC100AUS / G04, a second LLI epoch with a 0.45 m step; LMMF00MTQ / E36 at 268680, no step on the primary pair at all).

## 2. Why the re-flag comes late (question a)

The detector's decision lines (`MWDEC`, `MWSKIP` on the `mw_slip_detector` logger at debug) were read on a 5.75 h copy of the field golden on the base exe (`_rap_sdr_dbg2_f6`: `logging.enable_mw_slip_detector` true, `logging.levels.mw_slip_detector` debug; the level key alone leaves the logger off, and the log rotates at four files of 5 MB, so the station's lines were captured while the run went). The lane is the BIA-corrected single difference against the group's MW pivot (`has_bia=1`, path `BSSD-pass2`), so it integer-locks. `dev` is the sample's deviation from the arc's Hatch mean, `thr` the adaptive gate, `n` the arc count before the sample.

| TOW | line | dev (WL cycles) | thr (WL) | n | flag | what the detector does |
| --- | --- | --- | --- | --- | --- | --- |
| 275010 | MWDEC | +0.226 | 0.844 | 62 | 0 | locked arc at its level |
| 275040 | MWSKIP | | | | | LLI on L1C and L2W: the sample is withheld, the arc kept |
| 275070 | MWDEC | +15.047 | 0.888 | 63 | 1 | `lli_shadow=1`; a locked arc treats an exceedance as an outlier candidate: absorbed into the Hatch mean, which moves by dev / (n + 1) |
| 275100 | MWDEC | +14.703 | 0.888 | 64 | 1 | absorbed |
| 275130 | MWDEC | +14.671 | 0.888 | 65 | 1 | absorbed; the mean has drifted past the 0.4-cycle lock bound: drift demotion, an unknown-integer result, refused at PASS-A admission (the legacy gate needs a known integer, the evidence rule refuses BIA-corrected lanes); the band-scope row of 275130 |
| 275160 | MWDEC | +14.142 | 0.888 | 66 | 1 | the unlocked arc flags against the partly drifted mean: 14.142 rounds to 14 with a residual of 0.142, inside the 0.15 repair gate, so it is admitted and the drain frees the arc again; the arc is erased |
| 279000 | MWSKIP | | | | | LLI on L2W: withheld (the arc restarted at 278790 after the 278460 to 278760 gap) |
| 279030 | MWDEC | -8.934 | 3.302 | 7 | 1 | `lli_shadow=1`; seven samples, not yet locked (the lock needs 10): flagged at once, -9 cycles with a residual of 0.066, admitted, the arc freed again |

So the comment in `preserveArcsOnLli_` ("a real cycle slip ... trips the threshold check in processMwSample on the very next sample") holds for an unlocked arc only. At 05:30 the next sample flags, as the comment says, but the flag is not tied to the LLI that announced the step, so the drain frees the arc a second time one epoch after the first. At 04:24 the arc is locked: in the default `hatch_mean` reference a locked arc absorbs an exceedance (a single sample two cycles off a locked integer is far more often noise than a slip), the mean moves about 0.23 cycle per sample (dev over n + 1, the arc being younger than the 100-sample Hatch cap), the third sample demotes the lock with an unknown integer that PASS A refuses, and the fourth flags on the unlocked arc. That fourth flag is admitted only because its residual (0.142) fell inside the 0.15 repair gate. From the four deviations, the samples sat 15.05, 14.94, 15.13 and 14.83 cycles above the pre-step mean, and the mean had moved 0.24, 0.46 and 0.68 cycle after the first three; without noise the fourth would have read 15 - 0.68 = 14.32, a residual of 0.32 and an unknown integer. The sample sat 0.17 cycle low (noise, well inside the 0.77 m gate) and brought it to 14.14. A locked arc's re-flag is therefore a matter of a noise draw (with no noise, a drift whose residual would round inside the gate re-locks the arc before the gate is tested, and the step is absorbed again); an unlocked arc's re-flag is certain. The integer the late flag reports (14) is also wrong by the drift: the step was 15 cycles (15.047 at 275070). The drain never applies it, so this costs nothing beyond the second free.

## 3. Who reads the second flag (question b)

The MW drain's response to an admitted candidate is the free of each band it reaches: `markPendingReseed` on the station's ambiguity and the free-epoch latch `RelevelEpochState::free_ambs` with its cause `kCauseMw` (`networkSlipDrainMwRefloat`, `src/core/network/slip/SlipQc.cpp`). The latch is read by the float vehicle's drop (`ar/AmbFloat.cpp`), the leveling carry's inject skip and re-level stamp (`carry/LevelingCarry.cpp`), the producer AR exclusions (`ar/ArLattice.cpp`, `ar/ProducerAr.cpp`, `ar/ArSuspect.cpp`), the phase datum's candidacy screen and the anchor-slip handover staging (`solve/PhaseAnchorEpoch.cpp`, `solve/PhaseDatum.cpp`), the second-phase scope, the signal roster census, the within-epoch RAIM tier, the gap-resume census and the arc GF jump trigger. Each of them answers a free with its own response (a drop to the birth variance, an AR exclusion, a reset maturity, a staged handover when the arc anchors a family). A second free of the same step re-applies all of them and adds nothing: none reads the MW integer.

The MW integer (`dn_wl`) is read in two places only, both in the same drain: the delta bridge's pend intercept (`relevel_delta_bridge`, off in both goldens), which runs before the band scope and before the new test, so a bridge pend still receives it; and the bridge's same-epoch candidate list (`mw_cands`), which pairs a candidate that frees a band with an IF-TD flag of the same epoch; a fully covered re-flag frees no band and joins no list, and a partly covered one joins it as before. The PASS-A admission census (`MwAdmitCounts`, including the drop-unknown satellites of the AR suspect mask) and the common-band scope's evidence read the detector's results before the drain and are unchanged.

## 4. The 05:30 narrowing (question c)

At 279000 the receiver flags L2W alone; the LLI-epoch rule of the common-band scope implicates L2W and keeps L1C, although the ionosphere-free test flagged the L1C+L2W pair (`cb_kept=L1C`). The raw observables of `network_slip_raw.csv` (one-epoch steps across 279000): MW -7.649 m (-8.9 wide-lane cycles), GF -2.151 m, L1 minus C1 -0.070 m. With L1 unmoved and L2 moved by +9 cycles, MW steps by -9 cycles and GF by -9 x 0.2442 = -2.198 m; with L1 also moved by one cycle (L2 by +10), MW steps by -9 cycles and GF by -2.252 m. The data favour L1 unmoved (GF within 4.7 cm against 10 cm, at 18 degrees elevation over 30 s), and L1 minus C1 shows no step beyond its 0.5 m noise (an L1 slip of 3 cycles or more would show). The narrowing was right on this event, as far as 30 s data can tell. It is not a defect of its own: the ionosphere-free test cannot say which band moved, and the receiver's flag can.

The MW flag at 279030 cannot say which band moved either (it measures the difference of the two). It frees L1C on the strength of a step that L2W alone explains. That free is the conservative answer of a detector blind to the split, and the fix keeps it: L1C was not re-levelled at 279000, so its free at 279030 is a first response, not a second one. Telling which band moved at 30 s needs a band-localizing test (the GF split, or code minus carrier on each band), which the slip rules leave out at this rate; it is not part of this card.

## 5. The fix

**The detector reports the epoch a flagged step entered the data** (`MwSlipResult::step_epoch_s`, `include/slip/MwSlipTypes.hpp`). Each arc keeps the step epoch of its open excursion (`MwArcStats::step_epoch_s`): the excursion opens at the first sample outside the gate and closes at the first sample back inside it; inside an open excursion, a sample whose own time difference leaves the gate is a new step and moves the epoch to it. The epoch is the sample's own, except at the first sample after an LLI epoch: the detector now keeps the raw MW of each pair at the withheld LLI epoch (`prev_lli_mw_`), and when the first sample after it sits within the gate of that withheld sample, the step had entered by the LLI epoch, and the step epoch is the LLI epoch. Every flag of the excursion reports the step epoch, so a locked arc's drift demotion and the unlocked flag after it name the step they re-detect. Bookkeeping only: no detector decision reads it, and the user-side and real-time paths that use the same detector are unchanged.

**The drain's rule** (`networkMwReflagCovers`, `include/network/slip/SlipQc.hpp`): a band re-levelled at or after the step epoch (`LocalCarry::relevel_s`, the stamp every free, drop and re-birth of the carry writes) holds no pre-step level, so the step is already answered there. A step of the current epoch is a first detection and is never covered; an unknown step epoch or an unstamped carry (a warm-resume-restored entry) is never covered.

**The knob** `network.cycle_slip.mw_reflag_skip` (default off): armed, the MW drain does not free a covered band; a band of the same candidate that is not covered (L1C at 279030) is freed as before. The test runs after the delta-bridge intercept, so the integer still reaches a waiting pend.

**Events and counters (standing invariants 4 and 7).** The census runs on every `mw_detect` run, armed or not (`SlipQcCounters::mw_reflag_*`, run-end line `Network (MW re-flag)`): candidates whose step entered at an earlier epoch, the bands the band scope let the drain reach, covered and open (closure checked at run end), the candidates with every band covered, and the delay from the step epoch to the flag. Armed, each candidate with a covered band writes one `T0_mw_detect / mw_wl_reflag` row (action `skip`, statistic the delay in seconds), naming the pair, the bands skipped and freed, the step epoch and the covering re-levels. The skip is the decoupling edge: the MW flag's information does not reach the carry of a covered band, and the row names the re-level that already took it. There is no later re-coupling edge to re-validate: the skipped band stays in the state its covering re-level put it in, and the arc's next step opens a new excursion whose flag is a first detection.

**Residual risk, stated.** The withheld LLI epoch's sample is what dates a step to the LLI epoch. A second, unflagged slip in the one interval after the LLI epoch moves the first sample after it away from the withheld one by more than the gate, and the step epoch is then that sample's own, so the slip is freed (case `AStepAfterTheLliEpochIsANewSlip`). What the rule cannot see is a second step that leaves the MW unchanged (a band-common slip) inside an open excursion; the MW cannot see it either, with or without the rule, and the ionosphere-free test is the detector that answers it.

## 6. The bars, written before any run with the knob on

Arms made with `scripts/rapid_arm.py` from the goldens, Release only. Base exe: `gnss_processor_sdr_base.exe` (main `7bdcebc`, this branch's base). Candidate exe: the branch head.

- **B0 (the off path).** Knob off, `products.txt`, `closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical to the base exe on 2 h arms of the field golden and of the sim golden (`network.start_offset_hours` 0.504167). The detector's new bookkeeping and the drain's census change no product; the run-end log gains the `Network (MW re-flag)` line.
- **B1 (the target).** Field 6 h, armed: BSHM00ISR / G19 freed once per band at 04:24 (275040, both bands) with an `mw_wl_reflag` skip row where the control frees again; at 05:30 L2W freed once (279000) and L1C once (279030, not covered).
- **B2 (the telemetry).** The control's census reports a nonzero would-skip population (candidates and covered bands); armed, the covered bands are skipped, the MW refloat counter falls by the candidates with every band covered, the LLI refloat counter is unchanged, and the `mw_wl_reflag` rows equal the candidates with a covered band. A zero on an armed run whose control census is nonzero is dead wiring.
- **B3 (the held-out user).** WTZZ00DEU hourly medians (3D, `closed_loop_trace.csv`), hours 2 to 6: no hour worse than the control by more than 0.01 m (the bar of cards WUP and WGB).
- **B4 (the census).** `scripts/clock_level/gcensus.py`, the arm's own wire replayed at the 16 sites: the median change of the mean hourly p50 over hours 2 to 6 at most +0.005 m, and at most 3 sites worse by more than 0.02 m.

Predictions. P1: the control's census counts 10 to 20 candidates in 6 h (section 1 found 15 re-floats within 300 s of another free, 11 with no second step), most with every band covered; the 2 with a second step are not covered. P2: armed, the MW refloat counter falls by about 10 and the solution moves (products differ from the first skip on, which section 1 places in hour 1 at MAC100AUS). P3: the held-out user and the census move by a few millimetres at most: a re-flag costs one arc the re-convergence of one to four epochs, at a handful of stations. B3 and B4 are no-harm bars; no benefit is predicted on them.

What follows. B0 fails: the bookkeeping or the census moved a shared path; back to the build. B1 or B2 fails: the rule does not reach the re-flags; back to the build. B3 or B4 fails: recorded, the knob stays off. All pass: the verdict stays UNVALIDATED until the author decides on arming, through a ledger row and the generator.

## 7. Results (2026-10-28)

Exes: base `gnss_processor_sdr_base.exe` (main `7bdcebc`, sha256[:16] `4ebb0b607741c8ea`), candidate `gnss_processor_sdr.exe` (this branch, sha256[:16] `7df467d1a187fea8`), both Release, built with `scripts/wt_build.ps1`. Every arm a golden copy made with `scripts/rapid_arm.py`; the candidate's arms differ from the base's only in the exe, and `sdr_on_f6` from `sdr_ctl_f6` only in `network.cycle_slip.mw_reflag_skip` true.

**Unit gate.** `tests/network/test_network_mw_reflag.cpp`, suite `NetworkMwReflag`, 9 cases, all pass in the Debug build. The bed is five GPS satellites with an in-memory BIA product, so the slipping satellite's lane is the single difference against the group's pivot and integer-locks as on the field; the drain is the real `networkSlipDrainMwRefloat`, fed through the real PASS-A admission rule, with the harvest's re-level stamp emulated. The four sequence cases pin the defect on the unarmed side and the single free on the armed side: the 05:30 shape (a young arc, LLI on L2W, the flag one epoch later; unarmed L2W freed at 128 and 129, armed at 128 only, L1C at 129 either way), both bands on a young arc, the 04:24 shape (a locked arc: demotion with an unknown integer at k+3, the admitted flag at k+4, both reporting the LLI epoch; unarmed freed at 130 and 134, armed at 130), and a locked arc after an IF-TD free without LLI. With the skip disabled in the drain (a probe build, reverted), those four fail and the other five pass. The two safety cases hold armed: a step that enters one epoch after the LLI epoch and a second step inside an open excursion are dated to their own epochs and freed. Full `gnss_tests.exe` (Debug): 3611 tests from 488 suites, 3578 passed, 33 skipped, 0 failed; after the comment-only edit named at the end of this section, the MW, common-band, TurboEdit and IF-TD re-seed suites (129 tests) pass again on the rebuilt binary. `python scripts/nom_epoch_lint.py`, `python scripts/nom_n4_verify.py --whole`, `python scripts/counters_split_check.py`, `python scripts/make_golden_config.py --verify` and `python scripts/config_posture_check.py --verify-loader` (0 coverage gaps) pass.

**B0, the off path: holds.** Knob off, the candidate against the base exe, `products.txt`, `closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical:

| arm pair | products.txt | closed_loop_trace.csv | network_epoch_status.csv |
| --- | --- | --- | --- |
| field golden 2 h, `_rap_sdr_base_f2` / `_rap_sdr_off_f2` | `f14c268282734b6c` both | `3d60defadeb5f489` both | `dc9688b859e292c5` both |
| sim golden 2 h from 00:30:15, `_rap_sdr_base_s2` / `_rap_sdr_off_s2` | `d6a37c8fd7148368` both | `c0b1b34ab95e3c94` both | `bc3abcf8f757a38c` both |
| field golden 6 h, `_rap_sdr_dbg_f6` (base) / `_rap_sdr_ctl_f6` | `c6de838b83ed7eb5` both | `3526329081618453` both | `2f6b9bd4809b28ea` both |

(The base 6 h arm carries `logging.levels.mw_slip_detector` debug with the logger itself off, which writes nothing.) The detector is shared with the user side and the real-time engine: the user-side gate (`scripts/uom_gate.py`, quick set, sixteen 600 s arms: PPP, RTK, BSSD, TurboEdit, smoother, SBF) compares 16 of 16 arms IDENTICAL (every CSV byte for byte, the logs after the gate's masks, the exit codes); the real-time seam gate (`scripts/rt_seam_gate.ps1`, four arms, compared by `scripts/rt_seam_compare.py`): the three SBF replay arms (`sbf`, `sbf_bssd`, `sbf_te`) PASS, every artifact identical, and the closed-loop network arm `cl` differs by one inserted line in `console.log` and `main_log.log`, the run-end `Network (MW re-flag)` census line, with every CSV identical (the SBF arm configs needed `files.internal_data_root` pointed at `C:/GnssProcessor/data` for the run, since the worktree's `data/` is empty; the edit was reverted).

**B1, the target: holds.** Field 6 h, BSHM00ISR / G19, control (`_rap_sdr_ctl_f6`) against armed (`_rap_sdr_on_f6`):

| TOW | control | armed |
| --- | --- | --- |
| 275040 | LLI frees L1C and L2W (variance back to 2.88 and 7.81 m2) | the same |
| 275160 | `mw_wl_slip refloat L1C;L2W`: both back to 2.88 and 7.81 m2 (L1C had reached 0.0227) | `mw_wl_reflag skip pair=L1C+L2W;skip=L1C+L2W;free=none;step_tow=275040.0;relevel_tow=275040.0+275040.0`; L1C continues 0.0227, 0.0204, 0.0189 m2 |
| 279000 | LLI frees L2W | the same |
| 279030 | `mw_wl_slip refloat L1C;L2W`: both dropped | `mw_wl_reflag skip pair=L1C+L2W;skip=L2W;free=L1C;step_tow=279000.0;relevel_tow=279000.0` and `mw_wl_slip refloat`: L1C dropped, L2W continues (0.0941 to 0.0873 m2) |

The arc is freed once per slip on every band the earlier response freed; L1C at 279030 is freed once, by the MW flag, as before (section 4).

**B2, the telemetry: holds.** Run-end lines of the two 6 h arms:

| counter | control (knob off) | armed |
| --- | --- | --- |
| MW re-flag census: candidates / reached bands / covered / open / every band covered | 15 / 30 / 18 / 12 / 7 | 15 / 30 / 18 skipped / 12 / 7 |
| delay from the step epoch to the flag, mean / max | 100 s / 420 s | 100 s / 420 s |
| wide-lane detections (refloated) | 47 (43) | 40 (36) |
| LLI detections (refloated) | 227 (213) | 227 (213) |
| free-epoch latches / inject-site skips | 1047 / 743 | 1011 / 725 |
| `mw_wl_reflag` rows | 0 | 11 |

The armed MW detections and refloats fall by the 7 candidates with every band covered; the 11 rows are those 7 and the 4 partly covered ones (18 covered bands = 7 x 2 + 4). Of the 12 open bands, 4 are the kept band of a narrowed epoch (L1X twice at LMMF00MTQ / E36, L1C at KIT300UZB / G14 and at BSHM00ISR / G19), and 8 belong to the 4 candidates whose step no earlier response had freed on either band; for those the late MW flag is the first response, and it frees as before. The census is the same in both runs, so the skip changed no later detection.

The 11 skipped re-flags, by station: MAC100AUS G32, G31, G28 and E07 (hour 1, LLI epochs 259560, 259710, 259710 and 259800), LCK400IND G04 (T4 attribution with LLI at 259830, flag 60 s later), YKRO00CIV E10 (an IF-TD flag without LLI at 260490, a 198 m step, flag 90 s later), LMMF00MTQ E36 twice (L5X skipped, L1X freed: at 268680 the flag came 420 s after a 205-cycle L5X step at 268230 to 268260, which the arc had absorbed while demoting and re-locking every epoch; at 268920 30 s after an L5X LLI), KIT300UZB G14 (L2W skipped, freed at 276510 by the per-signal gap re-entry, L1C freed), and BSHM00ISR G19 twice. Section 1 had counted MAC100AUS / G32 as a possible second step (+0.94 m of MW between the LLI epoch and the flag); its GF moved +1.4 cm over the same interval, where a one-cycle slip on either band moves it 19 to 24 cm, so the move was code noise and the skip is right. KIT300UZB / G07 (a new step at its MW epoch) is not skipped.

**B3, the held-out user: holds.** WTZZ00DEU, median 3D error by hour (m):

| arm | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| control | 0.05805 | 0.01130 | 0.01835 | 0.01025 | 0.01545 | 0.01050 |
| armed | 0.05700 | 0.01165 | 0.01835 | 0.01035 | 0.01545 | 0.01050 |
| change (mm) | -1.05 | +0.35 | 0.00 | +0.10 | 0.00 | 0.00 |

Hours 2 to 6 within 0.4 mm, inside the 0.01 m bar.

**B4, the census: holds.** `python scripts/clock_level/gcensus.py make sdrc_{ctl,on} _rap_sdr_{ctl,on}_f6 _rap_sdr_{ctl,on}_f6 <candidate exe>` (each wire replayed by its own arm at WTZZ00DEU and the 16 sites, 17 of 17 replays exit 0 on each side), then `grade sdrc_ctl sdrc_on`. Median site p50 by hour, control and armed alike: 0.136 0.054 0.041 0.036 0.040 0.037 m. Paired change of each site's mean hourly p50 over hours 2 to 6: median +0.0000 m, 0 sites better and 0 worse at 1 cm, n 16; the largest change is KOKB -0.0017 m (better), every other site within 0.0003 m. Hour 1: median -0.0003 m, 1 site better at 1 cm (KOKB, 3.292 to 3.051 m), none worse.

**Predictions.** P1: 15 candidates (inside 10 to 20), but not "most with every band covered": 7 of 15 candidates, 18 of 30 bands; the narrowed LLI epochs (one band freed, the other kept) are more common than predicted, and their MW flags rightly keep freeing the kept band. Section 1's two second-step cases: KIT300UZB / G07 is not skipped (as predicted); MAC100AUS / G32 is skipped, and was not a second step (above). P2: the MW refloat counter falls by 7, not about 10; the products move from the first skip on (`products.txt` first differs at 259590, MAC100AUS / G32). P3: holds; the held-out user moves 0.35 mm at most in hours 2 to 6 and the census sites 1.7 mm at most. A re-flag costs one arc one to fourteen epochs of re-convergence (the census delay: mean 100 s, max 420 s) at a handful of stations, which the wire barely sees.

The committed source differs from the graded candidate exe by one comment only: the explanation in `preserveArcsOnLli_` (`src/core/slip/MwSlipDetectorEpoch.cpp`), which said a real slip trips the gate "on the very next sample", now states that a locked arc absorbs the step and flags it three to four samples on, and that either flag carries the LLI epoch as its step epoch.

## 8. Standing

The defect is real and general. One slip frees a carried arc twice whenever the MW detector re-detects a step that another response already answered: on the field golden 15 MW candidates in 6 h re-detect a step from an earlier epoch (30 to 420 s after it), and for 11 of them an earlier response (an LLI epoch, an IF-TD flag without LLI, a T4 attribution, a gap re-entry) had already freed at least one band. An unlocked arc re-detects one epoch after an LLI epoch, always; a locked arc absorbs the step, demotes its lock and re-detects it three to four samples later, when a noise draw brings the late flag's residual inside the repair gate (section 2). The fix is built and default off: the detector dates each flagged step (`MwSlipResult::step_epoch_s`), and `network.cycle_slip.mw_reflag_skip` does not free a band re-levelled at or after that epoch. Every bar holds (B0 to B4): the off path is byte-identical on both goldens and on the user-side and real-time gates, apart from the declared run-end line; armed, BSHM00ISR / G19 is freed once per slip, 18 of the 30 re-flag bands are not freed again, and the held-out user and the census move by 0.35 mm and 1.7 mm at most.

No benefit at the user is shown: the wire barely sees a second free of a few arcs. The ledger row stays UNVALIDATED and both goldens leave the knob off; arming is the author's decision, through the ledger row and the generator.

Open, not part of this card:

- The kept band of a narrowed epoch (section 4). When the LLI or a per-signal gap re-entry frees one band, the MW flag that follows frees the other band too, because MW cannot say which band moved: 4 of the 30 re-flag bands, among them L1C at 279030, which had not moved, and L1X at LMMF00MTQ / E36, which had not moved either. Keeping it needs a band-localizing test (the GF split or code minus carrier per band), which the slip rules leave out at 30 s.
- A locked arc's late flag reports an integer wrong by the mean's drift (14 for the 15-cycle step at 275160). The MW drain never applies it, but the delta bridge (`network.cycle_slip.relevel_delta_bridge`, off in both goldens) fills a waiting pend from it whether or not the band is skipped; its corroboration gate is the guard there.
- The LLI shadow's band-scope rows and the demotion rows of a locked arc (every epoch from 268290 to 268620 at LMMF00MTQ / E36) are emitted results that PASS A refuses; they feed the common-band scope as flags. Unchanged here.

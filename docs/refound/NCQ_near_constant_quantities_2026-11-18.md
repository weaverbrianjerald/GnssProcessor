# NCQ: near-constant quantities of the network and their responses (2026-11-18)

**STATUS**

- **id:** NCQ
- **topic:** network-vehicle
- **status:** open
- **summary:** One table of every near-constant quantity of the network engine (satellite clock, code and phase biases, pins, latches, receiver biases, wet delay, ambiguities) with its stiffness, what moves its truth, the wired response, its unit test and whether the adopted integer-fixing configuration arms it, followed by the empty and untested cells ranked by risk, the valve table of `docs/SPEC_prefit_census_observability.md` section 13 re-dated to 2026-11-18, the kinds of truth movement no detector acts on, fourteen response tests that need no engine run, a plan in eight steps and seven decisions. The check against the code confirmed the table and the ranking; it found an overstated Outcome and seven wrong cells (one test file, two counts, one arithmetic, one conditional statement given as general, two test-coverage cells), corrected in the last section. Nothing in the code was changed.
- **source:** plan package 2 of the post-reset program (2026-11-18), "Near-constant quantities: stiffness, what moves the truth, response, test"; network code read at 7fb0737 (branch `chain/audit`, frozen checkout `D:/GnssProcessorSupport/worktrees/read_7fb0737`), user-engine code at main 5ddc422. Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** Of the satellite-side near-constant quantities, only the satellite clock has a test of its carried value against the data with a loosening response and a recouple; on the receiver side, the armed RAIM prior channel resets a flagged receiver code-bias carry and the ring-fix monitor releases a held integer. The satellite code and phase biases, the receiver phase bias of the integer-fixing configuration and the receiver code-bias companions hold stiff priors whose responses are unarmed or trip only above 0.5 m to 5 m. No detector acts on a slow ramp or a constant per-link code offset. The plan leads with a per-satellite carry-consistency test.

Read at commit **7fb0737** (worktree, network code, cited "wt:"). One user-engine finding is cited from the main checkout at 5ddc422 ("main:"). Nothing was built, run or edited.

**Configurations.** "Reference" is `config/golden/golden_field_refound.json` (wt). "Integer-fixing" is the adopted configuration of NRF section 123: the reference plus the differences listed at wt: `docs/refound/CHA_chain_audit_2026-11-18_part3.md:112-118` (cited "CHA3"). Its JSON lies in run folders that were not opened, so a value not named in CHA3 is taken from the reference (inferred). Two consequences used below: the integer-fixing configuration arms the eq34 chain with the receiver phase bias in block and the receiver phase-bias floor at 0 (CHA3:113), and turns off `phase_datum_held_basis` and `emit_pb_hold_trip_generation` (CHA3:117); `emit_pb_hold` is off there (CHA3:102). `ambiguity_vehicle: float_kf` (reference:915) is not in the CHA3 list of changed keys, so the integer-fixing configuration keeps the float vehicle (inferred).

---

## 1. The table

Stiffness is the process noise q and any floor or pin weight. "Armed" is for the integer-fixing configuration, with the reference in parentheses where it differs. Test names are unit tests in the worktree; a test file that names a mechanism is listed, it was not run.

| # | Quantity | How stiff | What moves the truth | Response wired today | Unit test | Armed |
|---|---|---|---|---|---|---|
| S1 | Satellite clock, anchored (wt: src/core/network/solve/ClockAnchor.cpp:555-930) | q 2.9e-8 m^2/s (1 cm/h), floor 1e-5 m^2 (reference:328, 332) | clock phase or frequency jump; eclipse thermal effects; datum handover | anchor guard: decouple on a gap over 120 s, an IF-TD clock event, a datum change, an external refloat, NIS above k 6 or above 2 m (:576-640, 760-881); same-epoch value correction; recouple after 4 in-band epochs (:899-921); disc mark | ClockAnchorGuard.GenuineStepIsDetectedRefloatedAndValueCorrectedInEpoch, .WholeConstellationCommonModeDoesNotTrip, .RecouplingNeedsTheConsecutiveInBandRunAndDoesNotMark (tests/network/test_clock_anchor.cpp:357-462); ClockAnchorSlipClk.* (test_clock_anchor_slip_clk.cpp:411). Slow ramp: NONE | yes |
| S2 | Satellite code-bias carry (wt: src/core/network/assembly/ParamRegistry.cpp:126-138) | q 1e-10 m^2/s (0.6 mm/h; reference:166); variance floor `network.shared_carry_var_floor_cb_m2` default 0, not set (wt: src/core/config/GnssConfigLoadNetwork.cpp:4861-4862); gauge rows at 1e3 m^-2 targeting the last kept epoch (wt: src/core/network/solve/Sbasis.cpp:1599-1604, 2022-2024); formal sigma about 1 cm after hour 1 (NRF 112.7) | receiver code multipath at the few receivers of a mode; the coupled phase level (NRF 112.6, inferred there); hardware or temperature (inferred) | Reinit only for a satellite in `clock_refloat_sats` or `cb_excursion_sats` (wt: src/core/network/state/BorderStash.cpp:269-287); both sources unarmed (clock model, clock monitor, RAIM clock coupling, `cb_excursion`: GnssConfigLoadNetwork.cpp:7980, 7986, 7997). READMIT park and seed at var + q gap (wt: src/core/network/assembly/Responses.cpp:158-175). Emission step test above 0.25 cycle marks only (wt: src/core/products/NetworkProductEmitter.cpp:816-838) | StashHardRefloatTest.EachEntryLeftOutIsCountedAtItsOwnSite, .TheSoftReinitStashesTheEntriesAndCountsNoHardRefloat (tests/network/test_net_small_defects.cpp:269, 314); NetworkProductEmitterTest.CodeBiasJumpTestBumpsOnValueJump (test_network_emit.cpp:963). Per-satellite wander with a loosen response: NONE. Slow ramp at the emitter: NONE in either direction (section 4, item 1) | carry yes; reinit routes no |
| S3 | First-sighting family prior d_X - d_P (Sbasis.cpp:1964-2016; CodeBiasDatum.cpp:459-461) | sigma 0.10 m (reference:953), once per (satellite, birth) | the satellite's true d_X - d_P away from the veterans' mean; a clock error absorbed at birth (Sbasis.cpp:1937-1941) | none after the birth; a loosened carry counts as a new birth | CodeDatumFamilyTest.BirthPrior* (tests/test_code_datum_family.cpp:1811, 1866, 2273, 2368). Wrong birth value persisting: NONE | yes (inert for Galileo's merged combined mode, inferred from NRF part18:19) |
| S4 | Broadcast group-delay latch, the GF pin target (wt: src/core/network/driver/ApplicationNetworkSetup.cpp:684-762; Sbasis.cpp:121-200) | one value per run over [t0, t0 + duration) | control-segment update of T_GD or BGD; satellite hardware change; the run window itself (24 h against 5 h: 0.07 to 0.49 m on 22 Galileo satellites, CHA3:34) | none inside the run; run-start log of changed and zero values (Setup:721-762); at a resume the saved values replace the latch, changes logged only (wt: src/core/network/state/ChainMemory.cpp:115-127) | GroupDelayLatch.* (tests/test_group_delay_latch.cpp:51-126) test the rule. A broadcast change inside a run: NONE | yes |
| S5 | Satellite GF pin row, time-linked (Sbasis.cpp:1542-1623) | gauge row 1e3 every epoch, target the previous kept epoch | nothing estimable (a datum); a pair the group delay does not describe falls to a zero target, counted (:1566-1584) | a return absent at the last kept epoch re-anchors at the absolute target; `n_sat_gf_reanchored`, measure only (:1608-1614; row CbTimeLinkReanchor, Responses.cpp:176) | W6IonoConstraintTest.SatGfTimeLink* (tests/test_w6_iono_constraint.cpp:809, 836); GalileoNetworkTest.GalileoPairPinnedAtTheBroadcastGroupDelay (tests/network/test_galileo_network.cpp:246). Re-anchor at a return: NONE | yes |
| S6 | IF pin pair, per-channel sum-to-zero, Galileo family difference (Sbasis.cpp:1334-1510, 1878-2057) | gauge rows 1e3 | datum only; a coverage change re-selects the pair | hold one kept epoch, continue, or re-seed with a disc mark and counters | CodeDatumFamilyTest.PairChange*, .EnteringSignalKeepsTheHeldPair, .GroupDelayRefusesAHoldThatWouldMisanchor, .FamilyDifferenceRowOnVeterans, .FamilyRowWithoutAnyVeteranReseeds, .FamilyTimeLinkCarriesVeteransOrReseeds (test_code_datum_family.cpp:517-1223) | yes |
| S7 | Satellite phase-bias carry (ParamRegistry.cpp:114-125) | q 1e-12 m^2/s (0.06 mm/h), floor 0 | inter-frequency phase-bias variation with temperature (mainly a third band, inferred); a hardware reset | Reref at an unknown chain frame step (wt: src/core/network/solve/ChainDatum.cpp:2234); Reinit only on `clock_refloat_sats` (BorderStash.cpp:237-244), unarmed; a phase-mode anchor trip loosens the clock only (inferred from ClockAnchor.cpp:458-490) | ClockAnchorClockShare.AKeyCarriedShiftTripsWithoutTheRequirement (test_clock_anchor_clock_share.cpp:454); NetworkChainFrame.BirthReturningSlipAndMerge (test_network_chain_frame.cpp:458). Slow drift: NONE | yes |
| S8 | Chain tree-edge pins (wt: src/core/network/solve/PhaseDatum.cpp:1551-1557) | hard, weight 1e10; fresh-pin scale 1.0 (CHA3:113) | slip on the pinned arc; loss of the arc; a wrong rounding at a re-host | slip-aware election and slip row (ChainDatum.cpp:431-435, 752-768); rehost, returning and merge rows; at an unknown step the satellite phase-bias carry is re-referenced and the receiver phase-bias carry erased (:2234-2244); keep-carry entry check | Eq34ReattachHygiene.SlipAwareElectionDeseatsASlippedIncumbentTreeEdge (tests/test_eq34_reattach_hygiene.cpp:730); NetworkChainFrame.* (:412, 1997). A re-host target checked against the satellite's other held links: NONE (NRF 120) | yes (reference no) |
| S9 | Held integer of the ring fix (wt: src/core/network/ar/RingFix.cpp:470-477, 590-621) | hard hold | slip; wrong integer | monitor: IF residual above 0.03 m for 4 epochs releases; release on chain change, slip latch, refloat, gap; no re-decision for 600 s (include/network/ar/RingFix.hpp:72, 81-83) | RingFixMonitor.FourConsecutiveEpochsAboveTheBoundTripTheMonitor; RingFix release causes (tests/network/test_ring_fix.cpp:280-333) | yes (reference no) |
| S10 | Phase-bias hold at emission (wt: src/core/network/emit/PbHold.cpp:83-250) | emitted value frozen per generation | satellite phase bias moving under the hold | new generation at first emission, after a gap or a clock event (PbHold.cpp:185-199) | PbHoldTripGeneration.* (test_pb_hold_trip_generation.cpp:161, 257). Drift without a clock event: NONE | no (reference yes) |
| R1 | Ambiguity, float vehicle (wt: include/network/state/Entities.hpp:960) | q 0 (GnssConfigTypes.hpp:1617) | slip; gap | drop to birth variance, value kept, cover from the measured band step (AmbFloat.cpp:153-283); RAIM drop or erase; gross hold or erase; handover shift or erase; gap drop and prune | AmbFloatVehicleTest.*, BadEpochResponse.*, GapReentry*, NetworkGrossLos*, PhaseDatumHandover.CarryRerefEraseDropsEveryStationOfTheFamily, ParamResponses.CarryGapPruneErasesExactlyTheEntriesPastTheHorizon. Sites with no test that names them: AmbDatumSlipErase (PassASlipDrains.cpp:86), AmbWithholdErase (RaimWithinEpoch.cpp:1590), AmbT2ReplayFree (RaimWithinEpoch.cpp:1552) | yes |
| R2 | Receiver code-bias companions and IFB, in block (wt: src/core/network/carry/RxCodeBiasCarry.cpp:145-233) | q 1e-9 m^2/s (GnssConfigLoadNetwork.cpp:5730); horizon 900 s (RunTypes.hpp:1928) | restart, firmware, antenna or cable swap, temperature | harvest step erase at 10 sigma with a 0.25 m^2 guard, floor 5 m (:456-475); inject screen off on this route (:207-211); RAIM prior-channel erase (wt: src/core/network/qc/RaimPriorChannel.cpp:68-77); IFB re-seed erase (:437-447); gap skip and prune (:212-218, 670-674) | step erase: NONE (no test names `rxcb_step_erase` or `n_rxcb_blk_step_reset`); RxCbInjectGate.* (tests/network/test_rx_cb_carry.cpp:36-86) test the diagonal route only; RaimPriorChannel.AFlagGoesToTheResponseOfItsSubject (test_raim_prior_channel.cpp:276) | yes (reference:368, 947) |
| R3 | Receiver DCB zero-mean prior (RxCodeBiasCarry.cpp:313-334) | sigma 10 m, folded every epoch before the in-block skip at :338, so re-folded on a column that carries it; effective sigma about 10/sqrt(n) (inferred) | the true DCB; a gauge direction with free slant ionosphere | none; the estimate equals the prior (wt: docs/record/refound/IRDG_floor_off_rpb_in_block_2026-11-09.md:184) | RxCbDatumGauge.* (test_rx_cb_datum_gauge.cpp:144-197) count only | yes (reference:301) |
| R4 | Receiver phase bias, in block, unpinned (wt: src/core/network/carry/RxPhaseBiasCarry.cpp:59-200) | q 3e-6 m^2/s (GnssConfigTypes.hpp:1618-1619; integer-fixing value not confirmed); horizon 900 s | restart by a fraction of a cycle or whole cycles; firmware; temperature (mm, diurnal) | step erase at 10 sigma with a 0.0025 m^2 guard, floor 0.5 m (include/network/carry/RxPhaseBiasCarry.hpp:51, 54; .cpp:176-188); gap skip and prune; eq34 re-reference erase (Responses.cpp:110-118). No slip-latch row; the RAIM prior channel's default branch leaves it unwired (RaimPriorChannel.cpp:79) | [corrected 2026-11-19: see Corrections, item 3] RxPhaseBiasCarry.StepRuleComparesTwoHarvestsAgainstTheirVariancesTheWalkAndTheGuard, .BlockGateDoesNotFoldPastTheHorizonOrWithoutASharedBlockRow (tests/network/test_rx_pb_carry.cpp:151, 187). Restart under 0.5 m: NONE | yes (reference no) |
| R5 | Datum receiver's phase-bias pin (PhaseDatum.cpp:1563-1577; ChainDatum.cpp:1918-1979) | hard, 1e10, at 0 or the carried target | restart of the datum receiver | root handover pins at the carried value; a root that stays keeps its target, never refreshed (ChainDatum.cpp:1924-1932) | NetworkArcGraphOracle.RootRow* (test_network_arc_graph_oracles.cpp:6735, 6826, 6937). Restart: NONE | yes (reference no) |
| R6 | Zenith wet delay (wt: src/core/network/carry/ZwdCarry.cpp:362-565) | q 5e-8 m^2/s (reference:958), 13 mm/h; horizon 900 s | weather, fronts, rain | slip latch: erase or inflate by `zwd_event_inflate_m` (:300-357; default 0 = erase, Load:5161-5162; not set in the reference); RAIM erase (Raim.cpp:1078-1087, 1222-1237); gap prune with edge record. Live only with `zwd_blk_on && amb_float_kf` (wt: src/core/network/driver/Knobs.cpp:178-179). No innovation screen | [corrected 2026-11-19: see Corrections, item 4] ZwdCoupleEdgeTest.* (test_zwd_couple_edge.cpp:65-224); ZwdTimeLinkFloatVehicleTest.* (:288, 305). Weather step: NONE | yes; integer-fixing inflate value unconfirmed |
| R7 | Phase colour (Entities.hpp:905; ParamRegistry.cpp:267-279) | Gauss-Markov, sigma 0.005 m, tau 300 s; not near-constant | multipath, short-term phase nuisance | RAIM erase (Raim.cpp:1040-1043); gross erase (GrossLos.cpp:1029-1031); gap prune (PhaseColourCarry.cpp:302-306) | carry responses and decay: NONE | yes (reference:959) |
| R8 | Receiver clock and ISB | free per epoch; in-block carry off (Load:5189-5191) | jump, steering, restart | none needed while free; jump test on the unarmed route | CarryRxClockInBlockTest.* (unarmed route) | no |
| R9 | Slant ionosphere in block (Entities.hpp:882) | 2.5e-3 m^2 per epoch | continuous across a slip (inferred) | RAIM, gross, gap | CarryIonoInBlockTest.* | no |
| R10 | Station coordinates (ParamRegistry.cpp:281-298) | pinned at the roster ARP for the whole run | earthquake, antenna or monument change, unmodelled loading | NONE | NONE | pinned |
| X1 | Coupling rows and their exact update (Coupling.cpp:130-248, 423; CouplingExact.hpp) | each in-block entry's regression on border columns | a border column departing, re-initialized or frame-stepped | marginalize into the row variance; exact re-attribution through P | NetworkCouplingTest.*, CouplingExactUpdateTest.DecoupledAndDepartedColumnsEqualTheBatch | yes |

---

## 2. Empty and untested cells, ranked by risk on observed data

The ranking is the skeptic's, kept after checking; each line states why.

1. **Satellite code-bias carry (S2).** No per-satellite wander test, no loosen response, floor 0, both reinit routes unarmed. The class survives the merge of Galileo's tracking modes for every remaining column, GPS L1C/L2W included. An existing lever is untested and unused: `network.shared_carry_var_floor_cb_m2`, whose log line says it caps the information so a stiff carry cannot lock a biased product in (wt: GnssConfigLoadNetwork.cpp:12157-12161).
2. **A constant code offset of one link** (inter-mode or receiver-satellite) after the merge. No detector acts on it (section 6, item 2). Inferred.
3. [corrected 2026-11-19: see Corrections, item 3] **Receiver phase bias in the integer-fixing configuration (R4, R5).** A restart step under 0.5 m passes the step test; no slip-latch response; unwired in the RAIM prior channel (confirmed at RaimPriorChannel.cpp:79); the restart case is untested. With tree edges pinned to integers a fractional step has nowhere else to go (inferred).
4. **Receiver code-bias companions and IFB (R2).** The in-block step test is untested and has a 5 m floor. This matters for the estimable companion and IFB columns; the DCB itself is a gauge.
5. **Group-delay latch (S4).** No in-run response. A datum only, so the solution's estimable part is unbiased (Sbasis.cpp:1513-1534), but emitted code biases depend on the run window.
6. [corrected 2026-11-19: see Corrections, item 5] **Wet delay (R6).** No innovation screen; a weather step is untested. A 2 to 4 cm/h front is a 2 to 3 sigma walk per hour at q 5e-8, so the carry lags unflagged (inferred).
7. **Phase colour (R7).** Responses and decay untested; low risk because tau is 300 s.
8. **Satellite phase-bias drift (S7).** q 1e-12; low risk on two bands.
9. **Station coordinates (R10).** No response; low risk within a day.
10. **DCB prior double count (R3) and the topology-ledger memory loss (section 5).** Correctness or diagnostic defects with little effect on accuracy.

---

## 3. Corrections and disagreements between the parts

Kept as stated by the skeptic after checking, or settled here at the line.

1. **The emitter ramp test.** The satellite-side part cited NetworkProductEmitterTest.CodeBiasNoBumpOnSteadyStateOrDrift as proof that a drift goes unmarked. That test covers the default emitter with the code step test off (wt: tests/network/test_network_emit.cpp:659-661) and uses a 0.45 m step, not a ramp. The reference arms `code_bias_disc_jump_test` (reference:237). Correct statement: the slow ramp has no test in either direction.
2. **NRF 123 is not a single-system network.** The responses-and-tests part read it as one system and inferred that the clock runaway check's magnitude test goes blind. NRF 123 merges Galileo's tracking modes into one family per band; GPS stays (pairs include G:L1C/L2W, wt: docs/refound/NRF_network_ring_fixing_2026-11-14_part18.md:19-21). The warning at GnssConfigLoadNetwork.cpp:8118-8129 fires only with one active system. Refuted.
3. **The receiver DCB.** The receiver-side part said the true DCB is pushed into the slant ionosphere and the receiver phase bias. With free per-LOS slant ionosphere the DCB is a gauge direction, and a shrinking gauge sigma moves no estimable quantity (inferred). The double count is a correctness and readability defect; it becomes an accuracy risk only if a time-linked slant-ionosphere carry is armed (`iono_blk` is off).
4. **The 15 cm column of E11 (NRF 112.6, 112.7).** In the integer-fixing configuration `phase_family_codes` merges C1X into C1C (NRF part18:19), so that column no longer exists. What remains, inferred and unconfirmed: each satellite's inter-mode code offset lands in the code residuals of the receivers that track the combined mode, a constant per-link offset (ranked item 2).
5. **RAIM and the receiver code bias.** The carried-families part said no RAIM site writes the receiver code bias. The RAIM prior channel routes a flagged `BlockRxCbCarry` or coupling row to the receiver code-bias subject (RaimPriorChannel.cpp:68-77, read here). The receiver-side part is right.
6. **Receiver phase bias armed or not.** Off in the reference (eq34 off, reference:354), armed in the integer-fixing configuration (CHA3:113). Both parts are right for their configuration; this document grades against the integer-fixing configuration.
7. **Wet-delay inflate.** The receiver-side part gives 0.02 m on the ring-fix runs (inferred from NRF part 5); the reference leaves the default 0 (erase). The integer-fixing value is unconfirmed. Whether the slip and RAIM responses are live depends on the float vehicle being kept, which is inferred (section header).
8. **Topology-ledger memory loss.** Real (wt: src/core/network/qc/PrefitCensus.cpp:660-662), but it sits in the tie-row part, which the adopted configurations no longer build; the LOS row-kind map has the same loss on absence (:663). Low risk, mostly diagnostic.

---

## 4. The valve table, re-dated 2026-11-18

Source: `docs/SPEC_prefit_census_observability.md` section 13, status of Aug-1. D = decoupling edge, R = re-coupling edge (counter / identity), T = a test file that names the mechanism.

| Valve | Exists | D | R | T | Status 2026-11-18 |
|---|---|---|---|---|---|
| iono deweight gate | no; keys retired (wt: include/config/RetiredConfigKeys.hpp:114-116, 272-286); trace columns blank (RunTraces.cpp:439-444) | - | - | - | retired |
| SH health gate | no (RetiredConfigKeys.hpp:269-271) | - | - | - | retired |
| eq34 anchor handover | PhaseAnchorEpoch.cpp:374, 420, 444; transform AmbFloat.cpp:366-425 | events; `prm_ref_calls` | AnchorHygiene.cpp:73 | test_handover_stransform*.cpp | closed |
| code-bias admission | BiasAdmission.cpp:150-184, 337-360 | `n_cb_quarantined_census`, `n_cb_late_quarantined`; log per constellation, no per-pair event | none: run-latched by design | test_network_bias_admission.cpp | run-latched; no per-pair identity |
| D5 innovation withhold | `carry_invariant` only (InvariantCarryDriver.cpp:988-997) | `n_d5_flags`, no identity | `n_d5_retaps`, no identity | none located | retired posture |
| coverage gate, phase datum | PhaseAnchorEpoch.cpp:283-318 | `n_pd_gate_drop_edges` / `gate_drop` | `pd_reentry` / arc-ledger reason | test_phase_gate_drop_edge.cpp | closed |
| station vanish-return | SolveStations.cpp:343-379; PrefitCensus.cpp:599-663 | `sta_pd_dropped`; plain absence: counter only | `sta_readmit_events` / `sta_readmitted`; re-validates nothing | ledger only (tests/test_prefit_census.cpp:734-751); no test of `sta_readmitted` | open: memory lost on absence (section 3, item 8) |
| warm-resume seam | PhaseFrameHold.cpp:283; EmitStore.cpp:401 | `resume_seam` events | `resume_warmup_vet` built (Knobs.cpp:502; SignalIntegrity.cpp:109-113, 278-283) | test_signal_integrity.cpp:534-545 | closed (Aug-1 "unbuilt" stale) |
| RAIM withhold and re-entry | RaimWithinEpoch.cpp:1522, 1546 | `withhold`, `withhold_era` | `readmit` (BorderStash.cpp:391); `readmit_seed_check` | loader only (test_withhold_release_config.cpp) | partly: re-entry tested at configuration level |
| eclipse exclusion | [corrected 2026-11-19: see Corrections, item 6] ModelGeometry.cpp:787-795 (`DropReason::Eclipsing`); armed in all four reference-lineage configurations (golden_*_refound.json:34-36) | no network counter or event; satellite-status trace only | gap path: `cta_decouple`, `cta_recouple` (ClockAnchor.cpp:484, 918), `cta_disc_landed` (SolPrep.cpp:867) | test_clock_anchor.cpp:260 (gap length) | partly open |

**Mechanisms missing from the table** (added since Aug-1):

| Valve | D | R | T | Gap |
|---|---|---|---|---|
| ring fix hold (RingFixSeams.cpp:985, 1307, 1461) | `hold`, `would_hold`, `cascade_hold` | n/a | test_ring_fix.cpp:141-242 | none |
| ring fix release (RingFixSeams.cpp:818, 915, 949) | `release[cause]` events | re-decision after the window | test_ring_fix.cpp:280-333 | release events untested |
| ring fix entry check (RingFixSeams.cpp:718-760) | `entry_refused` | `entry_known` counted, no event | test_ring_fix_entry_revalidate.cpp | accepted re-coupling has no identity |
| keep-carry at a re-host (ChainDatum.cpp:1644-1665, 1732-1762) | `keep_refused` | `rehost_kept` counted; CHAIN `rehost` rows only (ChainFrame.cpp:640-661) | test_ring_fix_cascade.cpp:32 | no field says "kept" (inferred) |
| near-null guard and monitor (RingFixSeams.cpp:1659-1699) | `nn_release`, `nn_move`, `nn_monitor_on` | `nn_monitor_off` | test_ring_fix_nn_guard.cpp, _nn_monitor.cpp | none (off) |
| code-event window refusal (RingFix.hpp:160) | `refused[CodeEvent]`, no identity | window expiry, no row | test_ring_fix.cpp:918 | no identity |
| chain event rows (ChainFrame.cpp:38-70, 631-700) | CHAIN rows | `returning` with gap and previous parent | test_network_chain_frame.cpp | `Birth` writes no event (ChainFrame.cpp:42) |
| phase-family intake (RinexParser.cpp:379-399; Setup:240-265) | one log line per file | run constant; streaming refused (Setup:250-253) | test_phase_family*.cpp | not an in-run valve; blocks streaming |
| bad-epoch response (AmbFloat.cpp:142-270; Raim.cpp:1081, 1222) | counters; wet-delay events only | variance re-learnt, no edge | test_bad_epoch_response.cpp:107-256 | drop cover has no identity unless `trace_amb_state` |
| group-delay latch (Setup:706-752; ChainMemory.cpp:115-127) | run-start log | resume: `n_changed` logged | test_group_delay_latch.cpp; test_chain_memory.cpp | no in-run edge (invariant 1) |
| saved ring fix and chain memory restore (ChainMemory.cpp:58-134) | write | one totals log line | test_chain_memory.cpp | re-coupling with no per-link event, no re-validation |
| bias-file coverage test (main: src/core/corrections/GnssCorrections.cpp:1873-1876; main: src/core/measurement/MeasurementModelRow.cpp:38-46, 80-84) | none | none | none found under main: tests | time-blind set membership decides state opening, deweights and slip-detector pairs; identification of the intended mechanism inferred |
| gross line-of-sight screen (GrossLos.cpp:482-892) | yes / yes | release :634 | test_network_gross_los*.cpp | none |
| RAIM maturity gate (Raim.cpp:1494, 1510) | `arc_held` | `lift` | test_network_raim_maturity_gate.cpp | none |
| clock anchor decouple, phase withheld (ClockAnchor.cpp:484, 514, 544, 918) | yes | `cta_recouple`, `phi_withheld_end` | test_clock_anchor*.cpp | none |
| anchor level hold (AnchorLevelHold.cpp:419-468) | `hold_start` | `hold_end`, `hold_handover` | test_network_anchor_level_hold.cpp | none |
| second-band admission (SecondBandAdmission.cpp:210-269) | `withhold` | `readmit` | test_second_band_admission.cpp | none |
| within-epoch code deweight (RaimWithinEpoch.cpp:361, 982) | `we_deweight_sat_code` | epoch-local | test_network_we_choice.cpp (inferred from its name) | epoch-local |
| constellation withhold (SolPrep.cpp:329, 391) | `constellation_withhold` | none found | not located | no re-coupling edge found |
| satellite code-bias and phase-bias reinit (BorderStash.cpp:237-287) | carry marked Reinit on clock sources | none of its own | StashHardRefloatTest.* | not listed; its sources are unarmed |

---

## 5. Kinds of truth movement no detector watches

1. **A slow ramp of a stiff carried column below every per-epoch bar.** Every armed detector differences one epoch or one return, or compares a residual with a scale (clock anchor k 6, emitted-clock step 1.5 m, emitter phase and code step 0.25 cycle, MW 8 sigma, IF-TD 0.5 lambda_IF, gross LOS k 8, RAIM 6 sigma0, ring-fix monitor 0.03 m for 4 epochs). A ramp absorbed by the column's own q never trips. Columns exposed: satellite code bias, satellite phase bias, the satellite clock under the anchor, the receiver code-bias companions, the receiver phase bias. A drift reaches the residuals only past the residual scale, then shows as many small RAIM or gross flags on receivers, not on the drifting column (inferred).
2. **A constant code offset of one link.** Gross LOS needs r above 8 for 2 epochs and RAIM 6 sigma0; a constant decimetre offset at code sigma passes both (inferred from the bars). The block-Baarda report sees it and cannot act: "Nothing here writes a weight, a state or a decision" (wt: src/core/network/qc/BlockBaarda.cpp:7-11). The near-null guard reads code signatures on held Galileo links only and is off.
3. **A broadcast group-delay change inside a run** (S4). Logged at run start, never watched in the run.
4. **A sub-0.5 m receiver phase-bias restart and a sub-5 m receiver code-bias step** (R4, R2).
5. **A restart of the datum receiver** (R5): the kept target is never re-examined.
6. **Weather steps on the wet delay** (R6): q only, no innovation screen.
7. **Emission steps under the bars:** emitted-clock steps under 1.5 m and phase-bias steps under 0.25 cycle carry no mark; the emitter's clock counter has no magnitude test.
8. [corrected 2026-11-19: see Corrections, item 7] **A single-band return after a gap:** LLI only (wt: src/core/network/slip/RawObservables.cpp:499-503); band-5 pairs are outside the MW detector under the "primary" scope (PassASlipScan.cpp:217-232).
9. **Station coordinate, antenna or monument steps:** no detector found in qc, slip, solve, carry or emit (inferred from the search).
10. **Steps common to a whole constellation** are gauge by design (ClockAnchorGuard.WholeConstellationCommonModeDoesNotTrip); not a gap, recorded so a new test does not treat it as one.

---

## 6. Response tests that can be written without an engine run

Each is a GoogleTest case on a small constructed state; each documents present behaviour first, so it passes today and changes only when a response lands.

1. **RxcbStepErase boundary:** the in-block harvest step test just below and above 10 sigma with the 0.25 m^2 guard; counter `n_rxcb_blk_step_reset` and the response row (RxCodeBiasCarry.cpp:456-475; Responses.cpp:94).
2. [corrected 2026-11-19: see Corrections, item 3] **RpbStepErase restart:** a 0.1 m step passes the test today (documents the gap); a whole-cycle step at L1 above 0.5 m trips (RxPhaseBiasCarry.cpp:176-188).
3. **Emitter slow ramp with `code_bias_disc_jump_test` on:** 0.3 mm per epoch over 480 epochs, no bump (documents the gap); one 0.1 m step, a bump.
4. **Stash reinit routes:** a satellite in `clock_refloat_sats` marks its code-bias and phase-bias carries Reinit; with both sources empty, nothing is marked (BorderStash.cpp:237-287).
5. **Receiver DCB prior fold count:** the carried variance after n epochs with the prior re-folded every epoch, against one fold per birth (RxCodeBiasCarry.cpp:313-338).
6. **Phase-colour responses:** RAIM erase, gross erase, gap prune, and the decay phi = exp(-gap/tau) (PhaseColourCarry.cpp:204-213, 302-306).
7. **The three untested ambiguity sites:** AmbDatumSlipErase, AmbWithholdErase, AmbT2ReplayFree.
8. **Topology ledger return:** a station absent from one walk and back must not read as a first sighting (PrefitCensus.cpp:606, 660-663); fails today, so it is written as an expected-failure record or lands with its fix.
9. **Station return event:** `sta_readmitted` counter and event (SolveStations.cpp:343-379).
10. **Ring fix:** release events per cause; the accepted entry check counted under `entry_known`.
11. **Satellite GF re-anchor at a return:** `n_sat_gf_reanchored` steps (Sbasis.cpp:1608-1614).
12. **Group-delay latch:** a record changing inside the window does not change the latch (documents the run constant); the resume replacement count.
13. **Clock runaway quarantine and clock monitor thresholds** by name (GnssConfigLoadNetwork.cpp:7980-7991, 8100-8110; ClockQc.cpp:286-293); no test was found.
14. **Bias-file coverage (main tree):** an entry valid only outside the epoch still reports coverage today (main: GnssCorrections.cpp:1873-1876).

---

## 7. Plan in steps

1. **Settle the adopted values.** Read the integer-fixing configuration's `ambiguity_vehicle`, `zwd_event_inflate_m`, `phase_datum_eq34_rpb_q_m2s` and `rx_code_bias_datum_sigma_m` from its JSON (outside this package's reading rule). Until then rows R4 and R6 are graded as inferred.
2. **Write the tests of section 6.** No behaviour change; they pin present behaviour and name each gap.
3. **Per-satellite carry-consistency test** for the satellite code-bias and phase-bias columns, modelled on the clock anchor: the prior-minus-posterior difference against its gauge-projected variance, plus a windowed drift statistic for wanders under the per-epoch bar. Response: loosen that satellite's columns for N epochs, an event at both edges with identities and counters, rows in Responses.cpp, a disc mark. Default off, identical when off. Unit cases: a step, a ramp of 0.3 mm per epoch over 4 h, a common-mode shift that must not trip, the recouple. The existing variance floor `shared_carry_var_floor_cb_m2` is the smaller first lever (one ledger row, no code).
4. **Receiver phase bias:** a step guard that follows the wavelength so a sub-cycle restart trips; a slip-latch response row when a whole station latches; the RAIM prior channel wired for the receiver phase-bias block rows; a re-examination of the datum receiver's kept target at its restart. Default off.
5. **Receiver code bias:** a step guard scaled to the column's own variance; the DCB prior folded once per birth, on the model of `network.phase_datum_eq34_rpb_prior_birth_only` (GnssConfigLoadNetwork.cpp:8477-8488). Default off.
6. **Valve table repairs:** an eclipse decoupling counter and event; an event for the accepted entry check; an identity for a kept re-host; per-link events at the saved-state restore; the topology memory fix; an identity for the bad-epoch cover; a re-coupling edge for the constellation withhold.
7. **Constant per-link code offset:** a design for an acting response fed by the block-Baarda statistic (a deweight with both edges evented), default off; design only until step 3 lands.
8. **Group-delay latch:** an in-run report when a broadcast value changes inside the run; the response (none, or a datum re-reference with a disc mark) is the author's choice.

**Validation runs needed later (none requested now):**
- the simulator with a code ramp on one satellite (steps 3, 7);
- the simulator with a 0.1 to 0.2 m receiver phase-bias restart step (step 4);
- the simulator with a 0.5 to 3 m receiver code step (step 5);
- DOY 146 with the codes kept apart, E11 as the positive case (step 3);
- DOY 146 and 147 with the codes merged, inspecting the code residuals of the combined-mode receivers (step 7);
- a same-build control with every new key off, for each step that lands code.

---

## Decisions for the author

1. Whether the first response to the satellite code-bias stiffness is the existing variance floor `shared_carry_var_floor_cb_m2` (a ledger row and a value) or the new per-satellite consistency test, or both in sequence.
2. Whether the broadcast group-delay latch stays a run constant (a datum convention, window dependence accepted) or gets an in-run report and response.
3. Whether the receiver phase-bias step guard should follow the wavelength, accepting more false trips for sub-cycle restarts.
4. Whether the receiver DCB prior should fold once per birth (output moves only when the new key is on).
5. Whether a constant per-link code offset gets an acting response or stays measured by the block-Baarda report.
6. That the integer-fixing configuration (receiver phase bias armed, phase-bias hold off) is the configuration every row is graded against.
7. Whether station coordinate steps are in scope for this program.

## Not checked

- The integer-fixing configuration's JSON (in run folders, not opened): `ambiguity_vehicle`, `zwd_event_inflate_m`, `phase_datum_eq34_rpb_q_m2s`, `rx_code_bias_datum_sigma_m`, `cycle_slip.datum_station_detect`.
- Whether E11's column moved because its estimate moved or because the coupled phase level stepped (NRF 112.6 proposes, does not prove).
- The steady-state formal sigma of a satellite code column under q 1e-10 with realistic code weights (inferred millimetres against the measured 1 cm).
- Whether the prior-residual tally of code biases (`prires_cb`, StashStatus.cpp:195-216) or carry VCE feeds any automatic response.
- Whether anything outside RxCodeBiasCarry.cpp offsets the 10/sqrt(n) stiffening of the DCB prior (Coupling.cpp, CouplingExact.cpp not traced).
- Whether a whole-station slip latch reaches the receiver phase bias indirectly through the eq34 re-reference erase.
- Whether each receiver family's gap prune has a unit test (only the wet delay's was confirmed).
- The `edgeDetail` text of the CHAIN `rehost` row (whether it marks a kept re-host).
- The re-validation depth of RAIM re-entry and of the station return.
- The eclipse follow-ups (gap-aware clock test, re-seed at return, pivot hygiene).
- User-engine valves on main (`realtime/InnovationScreen`, the SSR break reactions).
- Which mechanism "the coverage test over the parsed file" names; the time-blind bias-file test is the inferred match.
- Whether test_network_we_choice.cpp covers the within-epoch deweight event (inferred from the name).
- The receiver clock clone (ClockClone.cpp) and LosState::sh_dev (off) for carried state.
- Real restart jump sizes of receiver DCB and phase bias for the roster's receiver types.
- No test was run; "T" means a test names the mechanism, not that it passes.

---

## 2026-11-19: corrections from the check against the code

Trees: "7fb0737" is the frozen checkout `D:/GnssProcessorSupport/worktrees/read_7fb0737`; "main" is `C:/GnssProcessor` at 5ddc422. The check found the table and the ranking sound at the code. Items 1 and 2 concern the draft's Outcome, which the header above replaces; items 3 to 7 are marked in the body where they apply.

1. **Outcome: "only the satellite clock has a test of its carried value against the data with a loosening response and a recouple".** Overstated. The armed RAIM prior channel tests carried block rows against the data and resets the receiver code-bias carry: `src/core/network/qc/RaimPriorChannel.cpp:68-77`, `src/core/network/qc/Raim.cpp:1200-1213`, `config/golden/golden_field_refound.json:931` (7fb0737). The ring-fix monitor releases a held integer on its residual: `include/network/ar/RingFix.hpp:80-82` (7fb0737). Corrected reading: of the satellite-side near-constant quantities, only the satellite clock has such a test; on the receiver side the RAIM prior channel resets a flagged receiver code-bias carry and the ring-fix monitor releases a held integer.
2. **Outcome: "No detector watches a slow ramp or a constant per-link code offset".** Wrong wording, contradicted by section 5 item 2, which says the block-Baarda report sees the offset and cannot act: `src/core/network/qc/BlockBaarda.cpp:7-8` (7fb0737). Corrected reading: no detector acts on a slow ramp or on a constant per-link code offset; the block-Baarda report can measure the latter and writes nothing (inferred).
3. **Table R4 ("Restart under 0.5 m: NONE"), ranking item 3 ("the restart case is untested") and section 6 item 2.** Wrong in part. `tests/network/test_rx_pb_carry.cpp:154-155` already asserts that a 0.49 m step does not trip the rule and a 0.51 m step does (7fb0737). What no test names is the harvest-site erase: `RpbStepErase`, counter `n_rxpb_blk_step_reset`, `src/core/network/carry/RxPhaseBiasCarry.cpp:179-188` (7fb0737). Corrected reading: the rule's 0.5 m boundary is tested (0.49 m passes); the harvest-site erase and its counter have no test. Section 6 item 2: the rule half exists; the new case is the harvest site.
4. **Table R6: "ZwdTimeLinkFloatVehicleTest.* (:288, 305)", read as lines of `test_zwd_couple_edge.cpp`.** Wrong file. The cases are ZwdTimeLinkFloatVehicleTest.SlipLatchStationsExcludeReentryOnlyLatches and .EventDropErasesTheFlaggedStationOnly at `tests/network/test_zwd_time_link_float_vehicle.cpp:288, 305` (7fb0737).
5. **Ranking item 6: a 2 to 4 cm/h front is "a 2 to 3 sigma walk per hour".** Wrong arithmetic. sqrt(5e-8 x 3600) = 13.4 mm per hour, so the front is a 1.5 to 3 sigma walk per hour.
6. **Section 4, eclipse row: eclipse exclusion armed in "all four reference-lineage configurations".** Wrong count. Three files match: `config/golden/golden_field_refound.json:35`, `config/golden/golden_field_bds_refound.json:36`, `config/golden/golden_sim_refound.json:34` (7fb0737).
7. **Section 5 item 8: "band-5 pairs are outside the MW detector under the "primary" scope".** Conditional statement presented as applying. It holds only under `mw_detect_pairs` "primary"; the default is "all" (`src/core/config/GnssConfigLoadNetwork.cpp:6003-6007`, 7fb0737) and the reference leaves the key unset. Corrected reading: under the "primary" scope band-5 pairs would be outside the MW detector; the reference keeps the default "all".

Wording, not ruled wrong: "the skeptic" in the body (sections 2 and 3) names the internal review step and reads as "the review"; in the section 4 row on keep-carry, "no field says "kept"" means no trace column says "kept". The draft's source line named the live chain worktree; the check read the frozen checkout at the same commit.

### Line numbers that moved

The draft cites the live chain worktree. At 7fb0737 its lines match, except:

- Table S9: `include/network/ar/RingFix.hpp:72, 81-83` is at `RingFix.hpp:71, 80, 82`.
- Table R1: the ambiguity q 0 is at `include/config/GnssConfigTypes.hpp:1618`, not 1617 (1617 is the station wet delay).
- Table R4: the receiver phase-bias q is at `GnssConfigTypes.hpp:1619`, not 1618-1619.

The check cited slightly different ranges for some confirmed claims (for example `BorderStash.cpp:236-287`, `RxCodeBiasCarry.cpp:452-474`, `ChainDatum.cpp:2231-2244`, `RxPhaseBiasCarry.hpp:50-53`, `PrefitCensus.cpp:659-662`, `RetiredConfigKeys.hpp:112-116`, `ApplicationNetworkSetup.cpp:704-725`); the claims stand at either range.

### What stands

- Ranked item 1 stands at the code: the satellite code-bias carry has q 0.6 mm/h, floor 0, both reinit routes unarmed, no wander response, and an emitter bar of about 5 cm per emission. The recorded E11 case (15 cm over hours under a 1 cm formal sigma, no response) stands for the reference configuration; in the integer-fixing configuration that column is merged away, as the body says (`ApplicationNetworkSetup.cpp:252-262`, 7fb0737).
- Ranked items 3 to 5 stand with correction 3. Ranked item 2 (a constant per-link offset after the merge) rests on inference and is marked so.
- The rest of the table (S1 to S8, R2, R3, R5, R7) and the section 3 settlements are confirmed; the 10/sqrt(n) effect of the DCB prior is not.
- The plan in eight steps stands.
- Decision 1 (the existing variance floor `network.shared_carry_var_floor_cb_m2`, the new per-satellite consistency test, or both in sequence) stands, with this context: the ledger marks `network.shared_carry_var_floor_cb_m2` UNVALIDATED, and its phase-bias sibling was graded neutral at 0.01 and 0.0025 m^2 and kept off.
- Decision 2 (the group-delay latch as a run constant, or an in-run report and response) stands.
- Decision 3 (a receiver phase-bias step guard that follows the wavelength) stands; with correction 3 the gap is the harvest-site erase and the sub-cycle threshold, not an untested rule.
- Decision 4 (the receiver DCB prior folded once per birth) stands.
- Decision 5 (an acting response to a constant per-link code offset, or measurement only by the block-Baarda report) stands.
- Decision 6 (every row graded against the integer-fixing configuration) stands.
- Decision 7 (station coordinate steps in or out of scope) stands.

### Left unconfirmed

- The integer-fixing configuration's JSON was not opened, so `ambiguity_vehicle`, `zwd_event_inflate_m`, the eq34 receiver phase-bias q, `rx_code_bias_datum_sigma_m` and whether the RAIM prior channel is armed there remain inferred.
- The 10/sqrt(n) stiffening of the receiver DCB prior.
- The response details of `AmbFloat.cpp` (row R1), rows R8 to R10 and X1, `Coupling.cpp`, `InvariantCarryDriver.cpp` (the D5 row).
- The remaining section 4 additions (anchor level hold, second-band admission, near-null guard).
- The section 5 thresholds other than the clock and gross line-of-sight bars.
- The first-stage parts beyond the R6 and eclipse cells.

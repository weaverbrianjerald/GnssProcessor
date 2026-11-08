# The current program: card index, part 2

Continuation of [CARDS.md](CARDS.md): the entries from SNX (pre-registered 2026-10-19) to the end of the list, moved here
verbatim on 2026-11-07 under the 60 KB bar. Entries are in pre-registration order; new entries are appended below.

- [SNX](SNX_sinex_station_coordinates_2026-10-19.md): closed, 2026-10-31: IGS weekly SINEX (week 2313) against the held coordinates: median 3D 6.0 mm, 20 of 71 above 1 cm, 4 above 5 cm, MIZU00JPN 1.68 m; frame and marker reduction not the cause; the NGS SIF post-seismic term double counted the displacement accumulated by 2020.0 (fixed, merge `bedc424`). Phase 1 arms (SNXB sections 10 to 12): the roster at its SINEX values moves the satellite clocks 5 to 7 mm RMS and the consumers 2.4 mm at most on the 2026-10-19 wire, on the current golden 7.4 mm at ARHT00ATA and 1.6 mm at the next site; MIZU00JPN restored at its SINEX coordinate is repaired (congruence statistic 0.707 against 74.2 at the held coordinate). The roster-file route does what a loader key would: phase 2 not built, nothing armed. The open list of section 9 is triaged.
- [SNXB](SNXB_sinex_roster_arms_2026-10-19.md): closed, 2026-10-31: card SNX's continuation (that file is at the 60 KB bar). Phase 1 of the coordinate-source plan run on the field golden, 6 h. 2026-10-19: the roster at its SINEX values (S1 six stations, S2 54) against a control and its byte-identical repeat; emitted satellite clocks move 5 to 7 mm RMS (a per-satellite level); WTZZ00DEU and the 16 census sites by at most 2.4 mm; the SINEX as grading truth moves the census median -1.35 mm (CEDU00AUS -11.5 mm). 2026-10-31 (pre-registered, current golden, exe of the baseline pair `_rf0hv17`): the same roster moves ARHT00ATA 7.4 mm and the next site 1.6 mm; MIZU00JPN at the held coordinate reads a congruence statistic of 74.2 and moves the wire 84 mm, at its SINEX coordinate 0.707, clock rows within 0.0005 m of the roster without it and WTZZ00DEU within 0.0025 m. B1 and B4 (b) met; B3 (ARHT00ATA +11.2 mm) and B4 (a) not met. Nothing armed, phase 2 not built.
- [PLANT](PLANT_planted_coordinate_offsets_2026-10-19.md): closed, 2026-10-19: one sim station's held coordinate planted wrong by 1 cm to 1 km, East and Up: wire 28.5 (East) and 6.2 (Up) mm of satellite-clock RMS per metre, consumer up to 3.4 mm per metre, no tier acts below 1 km; congruence screen names the station at 10 m at once, at 1 m after hours; 1 km Up declared at epoch 1, 1 km East poisons the network (47 other stations declared, consumer 335 m); measurement only, nothing armed.
- [MDAY](MDAY_multiday_candidate_2026-10-19.md): closed, 2026-10-20: the field golden (GPS + Galileo) and the BeiDou candidate (second-band
  admission, per-signal wire, sparse normals) on five 24 h days, DOY 128 to 132 of 2024, one Release exe (DOY 132 to 23:54:30, the
  DOY 133 products being absent). All ten arms exit 0 and emit every epoch. WTZZ00DEU at most 0.060 m (golden) and 0.047 m (candidate) per hour after
  ignition, census median site 0.02 to 0.07 m, no consumer above 1 m after hour 1 except the golden's ARHT00ATA for 26 min on DOY 130
  (2.2 m). Candidate against golden: census paired median -0.8 to -3.8 mm, none worse than +2.9 mm, ARHT00ATA -0.08 to -0.13 m on four
  days, hour 1 better in the median; cost about twice. One wire difference read (G03 on DOY 130). Not armed.
- [R1D](R1D_station_level_datum_2026-10-23.md): closed, 2026-10-31: L5 step R-1d, the station level under three bands. A datum row on the receiver band-5 delay and second-channel offset does not hold MAJU00MHL's level (10 m: nothing; 1 m: 0.95 m into the C1W rows); the transmission is the phase-datum anchor loop (barring the station: no event, clock wire 0.91 times dual); the coupling projections break the gauge or cost the dual clock wire 40 %, and a re-seed handover does not undo it. Five record branches, none merged. Continued in card ALD; L5 SEQUENCED.
- [ALD](ALD_anchor_level_datum_2026-10-23.md): open, 2026-11-06: L5 step R-1d continued (successor of R1D): an
  anchor station's level directions held as part of the phase datum. Derived 2026-10-23; built and graded 2026-11-06
  on branch `ald/anchor-level-hold` (default off, not merged, not armed). Gates 1 to 3 pass and the off path is
  byte-identical; gate 4 FAILED (MAJU00MHL's code biases still step at TOW 260040 by half the control's with its hold
  in force, 28 decouples against 21; the all-stations hold the same); gate 5: the dual bar passes (clock wire 0.70
  times), the three-band bars fail (wire 1.79 times the dual control, code offsets at 19 stations). The hold
  redistributes a network-wide level re-realization; the transport reading is refuted. L5 SEQUENCED.
- [MRF](MRF_mw_reflag_skip_2026-10-28.md): closed, 2026-10-28: MW re-flag: one slip freed a carried arc twice (the LLI or IF-TD
  response, then the MW detector's re-detection 30 to 420 s later; 11 in the field 6 h). The detector dates each step and
  `mw_reflag_skip` frees no band re-levelled since; off byte-identical, on frees once, user and census within 2 mm. ARMED
  2026-10-28 at the author's decision (ledger, both goldens, the loader's posture row); baseline pair `runs/_rf0hv16_{field,sim}6`.
- [BOF](BOF_built_off_follow_through_2026-10-28.md): closed, 2026-10-28: the built-but-off register worked through on the
  golden of main `ef9f75d`: a 24 h day (PIV, OTL, NTA), a second PIV day, the block-Baarda positive control, NSD, the ARL
  slip fixes, posture C at 305 stations and a 2 h triage of 22 never-graded keys. OTL, NTA, the estimable block test and
  NSD pass; PIV misses one site clause (ARHT00ATA, DOY 130); the ARL fixes stay off. ARMED at the author's decision:
  OTL and the noon-turn attitude (field golden), the estimable block test and the seed mirrors (both goldens); the loader
  warns on the out-of-scope combined sigma; baseline pair `runs/_rf0hv17_{field,sim}6`.
- [CVF](CVF_conditional_variance_frame_2026-10-29.md): closed, 2026-10-29: card NETCOST's optimization 2, BUILT, GATED and
  merged into main (branch `perf/cvf`). The border-known variance (`pureConditionalLocalVarianceDiag`) read as diag(N_rr⁻¹)
  without the product with an all-zero n_s x n_s matrix, and the datum-frame shared covariance built once per solve for
  every consumer that passes the solve's datum; no key. 23-arm gate 24 of 24, user side 16 of 16, real time wall-clock
  text only; field golden loop -9.5 to -10.5 % over the whole run and -12.8 to -13.5 % with both runs of a pair live,
  sim -4.2 to -6.8 %, products byte-identical; every gate passes.
- [RCG](RCG_refloat_cutting_keys_2026-10-29.md): closed, 2026-10-29: the two keys that cut refloats in card BOF's 2 h
  triage, graded on 6 h field arms, the 16-site census and 6 h sim arms against a same-exe control (byte-identical to
  `_rf0hv17_field6`). `raim_refloat_maturity_gate` at 120 epochs PASSES (T4 refloats 107 to 55, slip-class responses
  -6.9 %, users within 0.1 mm, sim identical) but counts nothing it suppresses; at 60 epochs -3.1 %.
  `signal_integrity.census` is harmless, not less churn (-3.3 %). Neither moves a user. Nothing armed: the author decides.
- [NSP](NSP_census_neutral_spread_2026-10-29.md): closed, 2026-10-29: the 16-site census's spread under a neutral change
  of the wire, six 6 h arms per day on DOY 130 and DOY 129 on card BOF's exe and posture. The registered thread-count
  and serial-carry arms do not act on the float-vehicle golden (products byte-identical); amendment A1 re-rounds the
  roster coordinates (within 0.05 mm per axis). Every consumer's trace changes, yet no census site moves by more than
  0.19 mm (ARHT00ATA s 0.11 mm) and no arm trips the clause. Card PIV's DOY 130 ARHT00ATA +40.6 mm reads OUTSIDE the
  spread: a response to the election's wire, not noise (the null does not reach its clock-anchor decisions).
- [BDS305](BDS305_beidou_305_stations_6h_2026-10-29.md): closed, 2026-10-30: BeiDou posture C at 305 producer stations, a
  6 h pair on the current field golden (the check card BOF named for the hour-2 flag of card MDAY section 10; bars
  pre-registered 2026-10-29). Both arms emit 720 of 720 epochs and withhold none; WTZZ00DEU hour 2 +6.05 mm under C (the
  flag reproduces), hours 2 to 6 -2.61 mm, three off-roster consumers -5.5 mm at the median. Verdict: a transient of 01:00
  to 01:30 UT that recurs (also at DAEJ00KOR) and enters through the consumer's BeiDou satellites, not a settled cost; a
  GPS + Galileo user of the C products is unchanged. 1.2 to 1.9 times GE per epoch on a loaded machine. Nothing armed.
- [MGT](MGT_maturity_gate_telemetry_2026-10-30.md): closed, 2026-10-30: the telemetry card RCG found owed for
  `network.raim_refloat_maturity_gate`, BUILT and GATED (branch `claude/confident-elgamal-6b01b9`, merged into main). A run-scope tally
  (flags, arc-epochs and epochs held, the lift), one event row per held flag and one at the lift (tier `RAIM_MATURITY`), a
  run-end section, an arming line, and the `acted` column of the attribution trace read as a refloat taken (the persistence
  gate had the same defect). Gate off: byte-identical on 2 h field and sim arms. Gate on at 120 epochs, 2 h field: 27 flags
  held on 11 epochs (19 arcs), the lift at 62.5 min, 3 of the 4 refloats after it on arcs held before it. Nothing armed;
  ledger verdicts unchanged.
- [WGB2](WGB2_windup_bridge_current_golden_day_2026-10-31.md): closed, 2026-10-31: card WGB's continuation (that file is at
  the 60 KB bar), pre-registered in WGB section 13. The wind-up gap bridge against same-exe controls on the field golden of
  card BOF (exe `gnss_processor_bofarm.exe`), DOY 129, 6 h and 24 h. G1 to G3 hold on both pairs (day: 267 of 280 gaps
  bridged, 13 refused in one umbra event, arcs refloated 547 to 23, consumer within 0.15 mm in every hour, census median
  -0.01 mm); the day has no GPS noon-turn refusal, so that guard is not exercised. Independent recount of every attempt
  (own SP3 reader, Sun, attitude and dipole angle): I1 and I2 hold on the 6 h pair (48 of 48, 2 = 2) and on the day pair
  only under amendment A2 (91 of 91, 45 = 45); the pre-registered origin rule misses both (90 of 91, 46 against 45), a fault
  of the check's origin model. Benefit bar (1 mm) not met. Deferrals triaged: warm-resume re-derivation field-inert,
  provider unit test a register line, simulator bed not built. Verdict: correct and inert; ledger
  KEEP_OFF, recommended here and decided by the author on 2026-10-31.
- [WGB2-T](WGB2_attempt_tables.md): reference, 2026-10-31: companion table of card WGB2: every attempt of the bridge arms
  `_rap_wgbc_on_f6` (61 attempts) and `_rap_wgbc_on_f24` (280 attempts) with the bridge's whole-cycle count and the
  independent recount of `scripts/windup_gap_independent_check.py`.
- [SCB](SCB_sim_clock_error_bed_2026-10-24.md): closed, 2026-10-24: a sim fixture with an injected satellite clock error (offset 0.5 m, rate 1e-5 m/s, random walk 3e-8) beside the quiet one. Quiet: every posture's clock about 2 cm from truth at hour 6 from 00:30:15 (the baseline's 0.32 m constants are the start artifact). Clock fixture: the self-contained regime and the native posture unchanged (2.3 and 1.7 cm, consumer within 1 mm); the golden lags the drift 35 min and its consumer inherits the a priori's error through the phase-clock hold (0.2 to 0.5 m against 1.5 to 3 cm), undeclared. Measurement only, nothing armed.
- [SMX](SMX_scenario_matrix_2026-10-24.md): open, 2026-10-24: the standing fault-scenario matrix (18 rows on the sim golden's three clock-level postures, event minus null against bars frozen before the run; four injector shapes built default off). Version 1 run: 22 of 55 cells pass; the satellite clock step and the slip storm fail on every posture, the clock drift on the golden only, a 1 m station step on the self-contained regime; identity and control gates pass. Nothing armed.
- [SCK](SCK_sat_clock_step_2026-10-25.md): closed, 2026-10-25: why card SMX's +0.5 m satellite clock step fails on every posture. Common: IF-TD reads it as a slip at all 16 stations of the satellite and refloats every arc, the phase datum re-realizes cold within one epoch (the level falls to the code), and the phase-bias hold keeps the wire's phase-visible clock with no generation edge. Golden: the anchor trips and the re-solve is exact, then re-couples on a code level 0.21 m off. Self-contained: the consumer accepts a 0.5 m phase innovation (1.5 m; 0.007 m with the hold off). Native: the guard loses its phase statistic in the step epoch and never decouples. SMX's leg consumer is 0.7 to 1.2 m off in every null. Nothing armed.
- [SCR](SCR_sat_clock_step_response_2026-10-26.md): open, 2026-10-26: builds card SCK decisions 1 and 3, default off. A satellite flagged by the IF-TD pool at most of its stations with one magnitude keeps its arcs and has its clock anchor tripped and re-solved; a tripped clock opens a phase-bias hold generation so the consumer refloats the satellite. Pre-registered bars: SMX row sat_clk passes on g, c and n, no other cell changes verdict, knobs-off identity with smx1.
- [ARS](ARS_anchor_resolve_coupling_2026-10-26.md): closed, 2026-10-26: the clock anchor's same-epoch withdraw-and-re-solve on the sim golden moves a tripped clock by metres because the exact coupling rows were conditioned on the anchored clock and the prior is withdrawn after them (unit: 17 % of the step, linear; the leg's 24-fold size not reproduced). The default-off deferral `network.clock_anchor_defer_coupled` passes its bars (storm g: 0 of 22 clocks pushed away against 16 of 31; no matrix verdict changed; c and n byte-identical); P1's trip clause and P2's second clause falsified. UNVALIDATED; arming is the author's decision.
- [CRS](CRS_leg_consumer_resume_2026-10-26.md): closed, 2026-10-26: why every event-harness leg consumer resumes 0.7 to 1.2 m off. The snapshot is the converged consumer and is restored. Producer, every posture: the phase-bias hold is not snapshotted, so a warm resume restarts every held key at generation 1 and re-levels every phase bias by up to 21 m (clocks and code biases within 47 mm) under a seam mark held keys ignore; the kept ambiguities absorb 0.1 to 2.4 m. Consumer, GPS + Galileo: the initializer latches are not snapshotted, so the ISB and receiver DCBs are re-seeded (14 m on an unbroken wire). The seam alone breaks a consumer that never restarted (about 100 m); both fixes owed. Nothing armed.
- [PES](PES_parallel_epoch_stages_2026-10-29.md): open, 2026-10-29: BUILT, GATED and ARMED, merged into main 2026-11-02 (branch `perf/pes`).
  The station normal formation, the Schur reduce (split by border columns, not by stations) and the redundancy census run
  in parallel behind `network.parallel_station_normals`, `parallel_schur_columns` and `parallel_redundancy_census`
  (loader default off; ARMED and pinned in both goldens at the author's decision): key on IDENTICAL to off on both
  goldens for each key and all three, off IDENTICAL to main; loop -27.9 % (field), -20.3 % (sim), -39.7 % at 305
  stations after card CVF. The baseline pair stands. Continued 2026-11-06, branch `perf/pes-next`, ARMED 2026-11-07 (section 9):
  `network.parallel_carry_harvest`, `parallel_iono_traces` and `parallel_coupling_prepare` (each IDENTICAL to off, off IDENTICAL to main), PASS A rejected on its timing, one OpenMP team size in every region. Loop -28.9 % (field), -25.7 % (sim) against main; all six stages -48.7 % against none at 305 stations over 2 h.
  2026-11-07: a fourth key, `network.sbasis_values_first` (the S-basis completion on eigenvalues first, the rank gate reading
  them), identical; main against all four keys -41.5 % (field), -34.8 % (sim).
- [RKT](RKT_retired_key_tombstone_2026-10-30.md): closed, 2026-10-30: card RKD's follow-on F4, a warn-once tombstone of
  the 150 retired network keys, BUILT and GATED on branch `claude/pensive-ptolemy-4f6046` (merged into main 2026-11-02): a declared table
  beside the loader (path, flat spelling, deletion commit) and one warning per retired key a configuration sets. Both
  2 h goldens and the 16 user-side arms identical on a same-build pair; a golden with 30 retired keys differs only by
  the 30 warnings. 1,153 of the 1,234 network-mode configurations under `config/` now warn. Card RKD's notes written,
  two still owed.
- [RPC](RPC_raim_prior_channel_2026-10-30.md): closed, 2026-10-30: PASS, the post-fit RAIM's prior channel (card RPA's
  first defect), `network.raim_prior_channel` built default off on branch `claude/awesome-leavitt-2520ab` (merged into main 2026-11-02).
  The pools keep observation rows; each carried-prior row is tested once, in its own column's cell, with the T4
  isolated-arc test, and a flag answers its carry. Key off byte-identical (2 h field and sim). With the cold-start bound
  at 0, BSHM00ISR's G07 runaway (11.9 m without a brake) is held at 0.44 m by the line of sight's own carry row. 6 h:
  WTZZ00DEU +0.25 mm, census median 0.0 mm, clock wire within 2 %, sim byte-identical; refloats 55 to 71, mostly one
  faulty line of sight answered every epoch. UNVALIDATED; a hold and multi-day owed.
- [NOM5](NOM5_station_parameter_set_tail_2026-10-30.md): closed, 2026-10-31: card NOM step N6's remaining steps, BUILT and
  GATED on branch `nom/n6-tail` (merged into main 2026-11-02). N6f: the producer's rows sparse, every output identical (24 of 24 network
  arms with the snapshot, 16 of 16 user arms, real time), the loop cost flat (no gain shown). N6g:
  `estimation.seed_full_equation` leaves the producer byte-identical and moves the consumer under 1 mm at its births;
  every bar clause passes, a neutral change. N6h: a user layout flag that reaches no parameter is refused at load (the
  closed-loop consumer's configuration is a copy of the network's, so with a consumer the flags set its layout);
  `network.estimate_tropo_gradients` conditions well, but its unbounded columns fit noise and at HKSL00HKG held part of
  a station-level step for an hour. Its bound (section 6, `network.tropo_gradient_prior_sigma_m`, a 1 mm zero-mean prior
  per column and epoch) removes both, but in the event's own epoch the gradient still takes up part of the misfit
  (HKSL00HKG 101 mm; two arcs escaped the attribution refloat): field-blocking. N6j: the tests, onboarding pages 7 and
  8, the routing. Both keys UNVALIDATED; nothing armed.
- [BDG](BDG_beidou_third_golden_2026-11-02.md): closed, 2026-11-02: the BeiDou golden, the author's course B of card BDSB: `golden_field_bds_refound`, the field
  golden's output plus BeiDou B1I / B3I, the second-band admission and one wire bias per modelled signal; the three BeiDou ledger rows
  RECOMMEND_SCOPED under the new regime predicate `has_bds`, so the GPS + Galileo goldens are unchanged; gates G1 to G5 all pass (the two
  GPS + Galileo goldens and 16 user-side arms identical; the 6 h BeiDou golden byte-identical in `products.txt` to card BDSB's arm C6, its baseline); merged into main 2026-11-02.
- [L2Q](L2Q_l2c_l2w_quarter_cycle_2026-11-06.md): closed, 2026-11-06: L2C against L2W quarter-cycle alignment in the field files: the L2C minus L2W phase fraction of 294 stations over DOY 128 to 132 (579 station-days, 21 receiver types, 34 482 arcs) is 0 at every station-day and never a quarter cycle (largest station offset 0.018 cycle, satellite part at most 0.004); the SYS / PHASE SHIFT records disagree across families (0, -0.25, +0.25) while every file is aligned; no correction for field integer work, `model_corrections.apply_phase_shifts` stays off (on, it un-aligns 162 stations; KEEP_OFF row landed 2026-11-06); every golden station latches L2W and a station-consistent offset cancels in every double difference.
- [ARHT](ARHT_consumer_mechanism_2026-11-06.md): closed, 2026-11-06: the mechanism of the ARHT00ATA consumer's response to the wire, named
  from consumer replays of existing wires: the wire declares a satellite's level steps and motion only through its held phase biases, and
  the site latches the L1W family under the one sky no L1W roster station covers (none south of 45 S), so 60 % of its L1W phase rows run
  without a wire bias; the 100 to 190 m divergences of DOY 128 and 131 are G01's leaked return record meeting such a row (removed by
  deleting seven wire records at one epoch, or G01 at the consumer for ten minutes; not by another satellite), the standing 0.2 m and its
  centimetre responses collapse on the covered L1C + C1C family and on the per-signal wires; the "ARHT00ATA wire gain" of posture C is
  the per-signal wire (D6 -208.5 mm, C6 -214.6 mm), not BeiDou; nothing armed.
- [SRN](SRN_scientific_review_network_2026-11-06.md): closed, 2026-11-07: a scientific review of the network producer at `adef08b`, nine dimensions, 87 findings, 39 verified by two referees each, 15 arms: the core is sound and most departures are inert for the float user; material: the code weight 2.4 times pessimistic with every QC threshold reading it, a system-blind companion weight on 8,051 Galileo rows, both instruments tied to the a priori clock within a hold generation, an unrepresentative headline user; the wet delay has no reachable time link and the standard-atmosphere troposphere fails at the poles; one finding refuted; six builds returned the same day on their own branches (cards TRP, ZTL, SCC2, VIN, VCF, CRO; nothing armed; one referee each, section 8): GPT3 moves the polar wet delays by their hydrostatic shifts with the user unchanged and a GPT3 pressure defect found and fixed; the wet-delay time link is reachable and smoother but fails the sim 4 mm bar; the companion defect gated; the consumer's formal sigma and the convergence graders landed; the code factor 0.35 to 0.45 confirmed; a parked-clock handover defect candidate confirmed in the production code.
- [ARR](ARR_integer_ambiguity_review_2026-11-07.md): closed, 2026-11-07: the network's integer ambiguity resolution capabilities: the producer holds integer wide lanes on both beds (field 90 to 99 % within 0.15 cycle, superseding AB3-0's 57 %) and the user never receives them: each satellite's phase bias sits in its own anchor receiver's frame (same-anchor pairs 0.136 cycle, cross-anchor at the null) and the hold freezes the cold entry estimate plus a phase-only clock transfer (an identity to 1e-14 m); the wire declares every bias integer; field GPS narrow lanes short across receiver families; field covariance optimistic; route: integer-consistent emission first (card SZE, closed 2026-11-07: the datum binds on every bed, the emission hold on the sim beds; neither fix alone recovers the population there), then the IRD datum, lanes after L5, user resolution; 14 findings verified, nothing refuted.
- [CRS2](CRS2_resume_hold_and_latches_2026-11-06.md): closed, 2026-11-06: card CRS's two owed fixes built behind four default-off keys (the phase-bias hold in the producer snapshot, the initializer's seed latches in the consumer snapshot), the seam census moved after the hold; the leg consumer resumes at 1 to 7 cm instead of 0.7 to 1.2 m, off-path identity on both beds, no matrix verdict changes, three consumer channels pass, the clock-step row passes on g with the SCR and ARS knobs. Remainder: the clone row in the snapshot.
- [BRD](BRD_broadcast_referenced_level_2026-11-02.md): closed, 2026-11-03 (pre-registered 2026-11-02): a network referenced to the broadcast
  clock, SP3 orbits the only precise product. Two causes of card NBP's cost measured: the IF-TD test's one-way centring (false refloats at
  every station on real clock motion) and the clock time update's broadcast prior mean (22.6 against 6.9 mm of phase-visible wander per hour).
  Two knobs, default off: `network.cycle_slip.if_phase_td_sat_centre` and `network.apriori_clock_track`. GPS, 6 h: WTZZ00DEU -0.008 and
  +0.008 m against the GPS golden (before +0.021 and +0.027), census +0.013 to +0.007 m pooled; DOY 129 passes, DOY 132 fails. GPS + Galileo
  fails with the receivers' frozen inter-system offset. Both keys UNVALIDATED; nothing armed.
- [BRD2](BRD2_broadcast_remainder_hold_days_2026-11-03.md): closed, 2026-11-03 (pre-registered 2026-11-03): card BRD's three owed items on
  the GPS broadcast-referenced posture with both knobs. DOY 130 and 131 pass the day bars against the GPS golden (WTZZ00DEU -0.010 and
  -0.002 m, census +0.005 and +0.002 m); over the four days 25 % of site-days are worse by 2 cm (bar 20 %). The DOY-132 remainder (+0.016 m
  against the same estimator on the CODE a priori) is the clock anchor's statistic decouplings, passed to users through the smoothed hold's
  bridge; with `network.clock_anchor_k` 1e6 it vanishes and all four days pass (census pooled +0.0021 m, 17 %), pending a satellite clock
  step test (field-blocking). The phase-clock hold on the tracked a priori (`network.apriori_clock_track_phase_clock_hold`, KEEP_OFF) walks
  112 to 116 mm per 120 epochs and costs users 0.3 to 0.6 m. Nothing armed.
- [BRD3](BRD3_anchor_decoupling_clock_share_2026-11-06.md): open, pre-registered 2026-11-06: card BRD2's remainder read apart against
  CODE. The smoothed hold's bridge follows the a priori while a clock is decoupled and drops the correction's motion D over the bridge;
  D is the LOOSE clock's own excursion plus the a priori's drift, the a priori-following bridge is the right member on the CODE regime
  (lasting phase-visible error 6 against 13 mm) and a coin flip on the tracked a priori (15 against 13.5 mm, 12 of 26), so the bridge
  member is refuted as the lever and the decoupling is the carrier. Every one of DOY 132's 59 statistic trips on both postures is carried
  by the phase-bias keys (clock-space |w| at most 0.57; a true 0.5 m step at q 5e-6 reads 7.94). Next build, pre-registered with frozen
  bars: `network.clock_anchor_clock_k`, a clock-share requirement on the phase statistic's trip, refused below q 1e-6. Built here,
  measure-only: the guard trace's statistic parts, the matrix postures `t` and `tk`; run here: the injected clock step on the tracked
  posture with the test on and off, the instrumented DOY-132 rerun, short arms around card ICL5's lone trips. Nothing armed.
- [ICL](ICL_inter_constellation_level_2026-11-03.md): closed, 2026-11-03 (pre-registered the same day): the GPS-to-Galileo level of card
  BRD's broadcast-referenced posture, held for receivers whose inter-system offset is frozen. Measured before building: each
  constellation's phase-visible level is its clock pivot's a priori error (under the tracked a priori a random walk, 188 and 193 mm over
  120 epochs against the golden's 2.5), passed to the user by the smoothed hold; the clock frame hold keeps it off the code side. A held
  frame on the emitted phase-visible clock (`network.emit_phase_frame_hold`, default off, outputs only): 9 and 32 mm, census +0.206 and
  +0.116 to +0.011 and +0.017 m against the golden, receivers as configured equal to free-offset receivers (paired -0.0002 and +0.0044
  m); on the CODE a priori it cures the regime's own GPS + Galileo cost. G1, G4 (DOY 129) and G5 fail. UNVALIDATED; nothing armed.
- [ICL2](ICL2_frame_day_one_level_2026-11-03.md): closed, 2026-11-03 (pre-registered the same day): card ICL's two owed items on
  card BRD's GPS + Galileo broadcast posture with both knobs and the phase-visible frame. (1) Over 24 h on DOY 129 and 132 the frame
  keeps receivers whose offset is frozen equal to free-offset receivers (paired +0.0009 and +0.0019 m); one marked leash re-anchor
  (GPS, after 23.4 h, harmless) and one unmarked 0.403 m step when the clock anchor decoupled 18 of 22 Galileo satellites (D2 fails
  on DOY 129; D1, D3, D4 pass). (2) One level for code and phase: chosen offline over the clock frame hold voting on C + U, the frame
  voting in the phase-bias hold's level (`network.phase_frame_hold_level_vote`, default off) cuts each constellation's code-to-phase
  drift from 20 to 25 to 6 to 9 mm per 120 epochs, an exact no-op on the golden, receivers unchanged; the GPS-to-Galileo level then
  follows the code side (19.0 mm on DOY 129, from 9.4). K2 and K3 fail on their bars. UNVALIDATED; nothing armed.
- [ICL3](ICL3_frame_quorum_2026-11-05.md): closed, 2026-11-05 (pre-registered the same day): the failure of card ICL2's D2. At a clock-anchor decoupling of 18 of 22
  Galileo satellites the phase-visible frame voted on the 4 left and turned a decaying excursion of 3 of them into a permanent, unmarked 0.40 m level step. A quorum on the vote
  (`network.phase_frame_quorum`, default off; chosen by a frozen rule on replayed receivers, a tie among pause, widen and mark): below half of the constellation the satellites marked by
  their own bumps vote too. The level over the day 15.8 mm per 120 epochs (from 58.3), the receiver's offset -0.040 m at the event (from -0.487 m), positions unchanged. Q2 fails on one
  clause (offset rms 2.10 against 2.0 mm). One natural event; injections did not reproduce it. UNVALIDATED; nothing armed.
- [ICL4](ICL4_post_flag_slip_scatter_2026-11-06.md): closed, 2026-11-06 (pre-registered the same day): why one slip scatters a constellation's clocks. A second, real slip one epoch after an
  IF-TD flag, when that test has no double difference and the MW signal is dropped; the handover is a coincidence. The misclosure spreads through the receiver's Galileo offset and `network.clock_anchor_q` sets how far (18
  decouplings at 5e-6, 2 at 2.9e-8, on the same posture). `network.cycle_slip.if_phase_td_reseed_on_flag` flags the slip at 94.1 m and removes the event on both postures and on DOY 132's natural event. All gates pass;
  receivers not graded (a 24 h grade of the key is owed); nothing armed.
- [ICL5](ICL5_reseed_key_over_a_day_2026-11-06.md): closed, 2026-11-06 (pre-registered the same day): `network.cycle_slip.if_phase_td_reseed_on_flag` over 24 h on DOY 129 and 132, on the golden and on the broadcast posture, with the
  16-site census. It removes the post-flag events (R1 passes on all 6); one flag before the solve stands in for several arcs T4 dropped after it, so T4 arcs halve and refloats fall 2.4 to 6.0 % while flags rise 22 to 32 %. Card ICL2's D2 on DOY 129
  passes with the key alone (58.3 to 15.7 mm, 9.49 to 1.94 mm). Golden receivers do not move; broadcast receivers stay flat over the day. Two clauses fail: R3's decouplings on the golden at DOY 132 (82 to 84, two lone trips, cause not traced) and R5's hour
  clause on the broadcast posture at DOY 132 (hours 5 and 14). Nothing armed.
- [ICL6](ICL6_anchor_trip_family_shift_2026-11-06.md): closed, 2026-11-07 (pre-registered 2026-11-06): why the clock anchor trips alone in card ICL5. The statistic mixes two signal families and reads a shift of one, invisible to every row (14 of 15 Galileo trips on a mixed pair, held about 2.7 % of the time);
  a freed arc's pre-slip level pushes a receiver's DCB gauge by metres (force balance r 0.93 to 0.998, and within 2.4 % at a prior of 100 m2); the shift is the push times the family's variance. The key only softens that variance: 35 `phi` trips in the two golden controls, 36 with it.
  The 100 m2 floor, used as the instrument, removes all of them, cuts T4 arcs 81 and 68 % and moves no census site by more than 5.2 mm. M0, H4, H5 pass; H1 (unread at three trips), H2 and H3 fail clauses. Nothing armed.
- [RF0HV18](RF0HV18_combined_candidate_2026-11-06.md): closed, 2026-11-07: the end-of-week combined golden candidate C (field golden plus the N3c decisions D1, D6, D7 and the refloat maturity gate) on one Release exe; control reproduces `_rf0hv17` byte for byte; C fails four clauses (DOY 129 census median +0.2 mm, DOY 132 DAV1 +27.5 mm from D7, sim hour 2 +1.75 mm and site s05 +12.7 mm from the gate); D6 clean, D1 mixed, pivot election, re-seed and full-equation seeds pass against C where run; nothing armed.

- [RF0HV18B](RF0HV18B_reduced_candidate_2026-11-07.md): closed, 2026-11-07: the reduced golden candidate after card RF0HV18 (D1 + D6 = c2, and D6 alone, without the D7 keys and the maturity gate) on DOY 129, 130, 132 and the sim, one Release exe; c2 fails three clauses (census median +0.165 mm on DOY 129 and +0.170 mm on DOY 130, WTZZ00DEU hour 2 +4.15 mm on DOY 130), byte-identical to the control on the sim; D6 alone passes every clause on the three field days (not run on the sim); the re-seed is neutral against c2 and inert on the sim; the coverage pivot election fails on the sim and on DOY 132; nothing armed by the card's grading. 2026-11-07 section 5: ten keys armed under the author's rule, four kept off, health check passes on all five beds, sim ignition hour 50 to 90 mm worse.
- [AV2](AV2_avx2_build_measurement_2026-11-07.md): closed, 2026-11-07: what an `/arch:AVX2` Release build of the network producer returns on the two goldens and what it costs in reproducibility (branch `perf/avx2-measure`, not merged, nothing armed). As the build stands, loop -8.5 to -8.8 % (field), no gain (sim): the Schur reduce and the coupling inverse decline to serial on the SSE2-tile checks. With the two checks taught (scratch, identical output), -17.8 % (field) and -15.7 % (sim), -20.8 % and -13.8 % with card PES section 7's keys. Each build reproduces itself; AVX2 moves the census by at most 0.010 mm and changes no decision. Unit suite: 10 of 3 839 fail under AVX2. Recommended: AVX2 with the separate step and a new baseline pair, the author's decision. Section 6 (2026-11-07, branch `perf/avx2-teach`, not merged): the separate step built. The two checks accept the AVX2 tile (`nr` 4, row panels at multiples of `mr`) and take every split, identical to the untaught AVX2 build on both goldens; the SSE2 build is identical to before; the sparse formation reports formed sparse and formed dense at run end; the four numerical tests are settled as test-reference rounding (no production defect). The full unit suite passes in both Release trees and in Debug.
- [CRO](CRO_carry_recursion_oracle_2026-11-07.md): closed, 2026-11-07: a multi-epoch oracle of the carry recursion: the shipped per-station form against a joint filter across an arc birth, a slip drop, a READMIT seed and a clock-pivot handover (tests only, suite `CarryRecursionOracle`). Exact claims hold to 5e-9 m; the drop's kept mean moves the border 5.6 cm and persists; the diagonal handover kernel leaves the incoming pivot's phase bias 15 % over-confident (NEES 22; the dense kernel removes it); a clock parked across a handover seeds in the outgoing frame (0.68 m, a defect candidate confirmed in production; the field count found 0 instances in 9 handovers, the guard blind at a seed epoch; fix card owed).
- [VIN](VIN_validation_instruments_2026-11-07.md): closed, 2026-11-07: validation instruments (build B5 of card SRN), measure-only:
  the consumer's formal position sigma in `closed_loop_trace.csv` (four trailing columns, one shared schema, products and traces otherwise
  identical); position NEES consistent at the field and sim users in hours 2 to 6, mildly over-confident in hour 1; census convergence
  medians 30.5 min to 0.1 m and 94 min to a sustained 0.05 m (WTZZ00DEU 6 and 40 min); the sim pair of record starts inside the
  start artifact; replacement baseline paragraph for CLAUDE.md and the sim golden's start pin proposed, not applied.
- [TRP](TRP_gpt3_apriori_troposphere_2026-11-07.md): closed (armed on the field golden 2026-11-07, card RF0HV18B section 6), 2026-11-07: the GPT3 a priori troposphere (card SRN build B3) and the mapping label: `gmf` is Niell's NMF, relabelled `nmf` with products identical; the GPT3 pressure reduction fixed to the virtual-temperature form (17 to 22 hPa at the Antarctic coast); 6 h field: station wet delays move by their GPT3 shifts, user and census within 0.5 mm, low-elevation phase residuals unchanged (clause FAILED); two Antarctic wet delays stay negative; ledger UNVALIDATED, manifest change proposed, nothing armed.
- [SCC2](SCC2_companion_weight_system_aware_2026-11-07.md): closed, 2026-11-07: the second-code weight matched on the constellation as well as the signal. The station split divided by 100 every code row whose observation code matched a receiver inter-signal state of its station, so 8,051 Galileo C1C rows at CHUR00CAN and STJO00CAN (hours 2 to 6 of the field baseline) took the GPS companion's scale. `network.second_code_system_aware` (default off, UNVALIDATED): off byte-identical on both beds; on restores exactly those rows, sim identical, census median -0.2 mm; G4 FAILED as frozen (WTZZ00DEU hour 3 3.3 mm better, outside the two-sided bar); DAV1 7.7 mm worse. Arming is the author's decision.
- [VCF](VCF_variance_factor_calibration_2026-11-07.md): closed, 2026-11-07: the per-class variance factor (build B1 of card SRN): a measure-only rider on the redundancy census sweep (`network.vcf`, TRACE) reads the code factor at 1.00 on the sim and 0.35 in sigma on the field, flat by hour; the phase factor is not a noise measurement (equal across bands by algebra, absorbed by the carried ambiguities). The URA reader (`network.vcf_apply`) changes only the U records, UNVALIDATED without a truth grade; the RAIM readers (`network.vcf_apply_phase`) refloat 69 % more arcs and catch no satellite clock step, KEEP_OFF; nothing armed.
- [ZTL](ZTL_zwd_time_link_float_vehicle_2026-11-07.md): open, 2026-11-07: build B2 of card SRN, a time link for the station wet delay on the
  float vehicle: the in-block wet-delay carry made reachable under `float_kf` (vehicle-aware interlock) with an erase-for-one-epoch event
  response (slip latch, RAIM per-arc attribution), off and byte-identical by default. At q 5e-8 the field wet delay moves 0.73 mm per epoch
  (was 2.59), the slip-epoch excursions are gone, user and census unchanged; the sim truth bar fails (hours 4 to 6 rms 6.4 mm against 4,
  NEES to 2.8; 0.77 to 0.86 with the phase colour state); no integer gain. Not recommended for arming alone; the colour pairing is owed.

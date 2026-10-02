# The current program: card index

The cards of the network re-founding program
([NETWORK_REFOUNDING_2026-09-14.md](NETWORK_REFOUNDING_2026-09-14.md)), one entry per
card in pre-registration order, from ALC-ARC (2026-09-16) on. The list was moved here
verbatim from [docs/INDEX.md](../INDEX.md) on 2026-09-25 to keep the index under the
60 KB bar; only the link paths changed (`refound/X` became `X`). A program card's index
line (rule 3 of the index: updated at pre-registration and at close) is its entry here.

- [ALC-ARC](ALC-ARC_reset_generation_2026-09-16.md) — closed, 2026-09-16:
  reset-gap fix; 82 focused tests pass and ALC-OFF products are byte-identical;
  longer accuracy campaigns deferred.
- [PBT-AUDIT](PBT-AUDIT_translation_premise_2026-09-16.md) — closed,
  2026-09-16: blanket translation premise falsified; seven controlled witnesses;
  frame-transfer contract recommended, runtime unchanged.
- [ALC-STAT](ALC-STAT_statistics_audit_2026-09-17.md) — closed,
  2026-09-17: statistical interpretation audited; 40 focused tests pass;
  correlation and nuisance bias prevent a calibrated 4-sigma claim; no tuning.
- [ALC-BAND](ALC-BAND_window_scope_review_2026-09-17.md) — closed,
  2026-09-17: retain paired restarts; per-band dual-code levels need a separate
  statistical design; 44 focused tests pass, runtime unchanged.
- [ALC2](ALC2_restart_policy_adoption_2026-09-19.md): closed,
  2026-09-19: the level-check restart policy refused on both beds and its
  branch left unmerged; translate + level check + consumer-sat trace adopted
  into the RF-0hv working bases; the pair re-based on post-ALC-ARC main.
- [PBH](PBH_phase_bias_hold_2026-09-19.md): open, 2026-09-19: the wire's
  phase-bias movement is harmful error for a float consumer (sim 3.0 mm clock alone
  against 19.9 mm with the phase bias) and 0.2 % of the translated mass is a known
  frame move; the config-only dead-band ladder PASSED (zero dead band: field hours
  3 to 6 at 0.11 m with 167 sub-dm epochs, sim 0.06 m with 448); the emit-side
  phase-bias hold is now BUILT as `network.emit_pb_hold` (default OFF,
  identity-passing `gen=` wire contract) and all five PBH-1 bars PASS on both beds:
  the solve, the clock records and the code-bias records are byte-identical, every
  emitted phase bias is constant within its generation, the phase-bias channel's
  contribution to the within-arc error of the phase correction goes to exactly
  0.0000 m so the product's phase quality IS its clock quality (field 0.0693 to
  0.0274 m, sim 0.0199 to 0.0030 m), and the consumer reproduces the zero dead band
  to 0.3 mm, which measures the two as the same operation. ADOPTED into the
  RF-0hv working bases 2026-09-20 (baseline pair `runs/_rf0hv3_{field,sim}6`); no
  loader default, golden or ledger row changed.
- [AB1](AB1_sh_consumer_product_2026-09-20.md): parked, 2026-09-20: add-back 1,
  the SH VTEC field as a consumer product; the in-solve field is loader-refused, and
  the trace-only prototype fails the frozen rule (a causal global product beats the
  dual-frequency consumer's own ionosphere for 2 epochs only, sim with truth): the
  consumer constraint is KEEP_OFF, the field is specified as a product (degree 6,
  25 degree mask, per-satellite bias terms) and not built.
- [AB2](AB2_l5_third_frequency_2026-09-20.md): open (re-opened 2026-09-20,
  section 7: the L5 rows fit at the typical station, the damage reads as station-local,
  the station-scope arms T3s / T3sp are pre-registered); first verdict 2026-09-20: add-back 2,
  GPS L5 as a third frequency, KEEP_OFF on the field: the triple NETWORK damages the
  L1/L2 wire on real data (consumer hours 4 to 6 at 0.6 to 1.1 m against 0.11; a dual
  consumer on the triple wire reproduces it; the (L1, L5) IF-TD pairs and doubled
  post-fit refloats), the sim is neutral to better; an L5 phase-term model and a
  band-5 slip-tier calibration are owed.
- [GRS](GRS_gap_reentry_per_signal_2026-09-20.md): open, 2026-09-20: build card,
  the gap re-entry response per SIGNAL; the satellite-level bookkeeping misses a
  primary-pair loss of lock while band 5 or a fallback signal tracks through, which
  is how L5 damages the L1/L2 wire (AB2 sections 10 to 12) and a defect of the dual
  posture too; default OFF rider of `gap_reentry_relevel`, gates and bars frozen.
- [CPA](CPA_canonical_pair_admission_2026-09-20.md): closed, 2026-09-20, KEEP_OFF:
  canonical-pair admission, built exactly and falsified as the cause; the RINEX emulation
  that motivated it (203 to 627 epochs below a decimetre) had also moved the producer's
  first-epoch canonical-signal latch at 13 stations, and the gain belongs to that latch
  (every station on its priority-first signal: 601 epochs with no rule).
- [CSL](CSL_canonical_signal_latch_2026-09-20.md): open (built and ARMED 2026-09-20, baseline
  pair `runs/_rf0hv5_*`; the mechanism read is open), 2026-09-20: the canonical-signal
  latch; the first-epoch satellite count prefers a civil signal, the dual field base latches
  L1C at 47 stations although 18 carry L1W; every station on its priority-first signal takes
  the consumer from 162 to 601 epochs below a decimetre (275 to 983 of 1440 in 12 h); mechanism
  unread; the decision rule is built behind a knob, bars frozen.
- [TBI](TBI_three_band_ignition_2026-09-20.md): closed, 2026-09-20: the three-band
  ignition; the network closes band 5's code datum by asserting a zero inter-signal correction
  per satellite where the truth is metres (raw triple-code combination against the wire's:
  correlation 0.947 field, 0.962 sim); the misfit is pushed into band 2 and the WIRE carries the
  damage; band-5 code removed at the stations alone: 0.27 0.10 0.05 0.04 0.06 0.02 m, 535 epochs
  below a decimetre (314 with it, 603 dual); no config-only cure; L5 adoption bars frozen.
- [B5D](B5D_band5_code_datum_2026-09-20.md): open (built 2026-09-20, NOT armed), build
  card, the band-5 code datum (`network.sat_gf_extra_band_free`, default OFF): the zero
  inter-signal assertion is gone, the wire carries 2.78 of the 2.97 m truth, the band-2
  contamination is cured; triple field 314 to 390 epochs below a decimetre, which fails the
  L5 adoption bars (dual pair 603, emulation 535); the emulation had also removed the band-5
  wide-lane detector pair, an unread lead.
- [L5P](L5P_l5_phase_posture_2026-09-21.md): open (built 2026-09-21, NOT armed), the L5
  phase posture; two default-OFF knobs (`network.cycle_slip.mw_detect_pairs`,
  `network.extra_band_code_sigma_scale`); the gap is the band-5 CODE ROWS (scale 10: 597 epochs
  below a decimetre at WTZZ00DEU, every hour better than the dual pair), not the detector
  pairs (391); the scale fails the clock-wire bar and does not transfer across the 16 census
  sites; L5 not yet adopted and SEQUENCED, never abandoned (the author's directive; return
  plan R-1 to R-4 in section 4).
- [MSC](MSC_multi_site_consumer_2026-09-21.md): open (MSC-1 result 2026-09-21), the
  multi-site consumer census; WTZZ00DEU is not representative (603 epochs below a decimetre
  against a median of 145 at 16 independent sites); by the frozen count rule no adoption of
  2026-09-20 holds across sites, the three together improve 13 of 16; the CSL support reading
  is refuted; the priority latch thinned the wire's C1W code biases and a consumer diverges
  on the dropouts; the census fails its own bar as a standing instrument at 16 sites.
- [RXP](RXP_rinex3_blank_prn_2026-09-21.md): open (FIXED 2026-09-21), DEFECT, the RINEX 3
  reader dropped every satellite numbered below 10 in a blank-padded file (`G 7`); 16 of 310
  files of the day, four roster stations; fix unconditional, inert where no file is affected;
  the corrected field run is the new baseline `runs/_rap_r6_dual` and is worse at WTZZ00DEU
  (533 against 603): CAS100ATA and LCK400IND together amplify the all-satellite clock-anchor
  decouple of 05:22 UT (post-fit refloats 21 to 72), the dominant open loss of the field bed.
- [EV7](EV7_all_sat_clock_decouple_2026-09-21.md): open (fix ARMED 2026-09-22 in the
  working bases, baseline pair `runs/_rf0hv6_{field,sim}6`; every frozen gate passed), event card, the all-satellite
  clock-anchor decouple of 05:22 UT: DEFECT of the coverage gate on the float vehicle (a
  phase-bias column removed with one tracker left; that station's carried ambiguity absorbed
  the whole bias in one epoch; 13 edges in 6 h carry 49 of the run's 50 clock trips). Fix
  `network.phase_datum_gate_drop_erase` (default OFF): trips 50 to 1, no all-satellite epoch,
  clock wire 0.0091 / 0.0072 against 0.0184 / 0.0107 m, census 13 of 16 sites better (last
  half hour's median 0.171 to 0.099 m), sim identical and the relabelled truth arm cured;
  12 h trips 134 to 8. The guard made inert changes nothing on the fixed base.
- [RF-3](RF-3_code_row_qc_2026-09-22.md): open (ARMED 2026-09-22, the author's decision: the
  screen's one-row rule and same-epoch emission exclusion in both bases, ledger A11, golden pins,
  baseline pair `runs/_rf0hv10_{field,sim}6`; the drop floor built and OFF, the hold KEEP OFF;
  RF-3c's wire writer owed), QC
  that acts on code rows: the 101 gross post-fit code rows at full weight are the RE-COUPLING edge
  of the slip response, not code faults (five of six episodes have clean data; a refloat re-births
  the ambiguity at the arc's first-stash variance and the level walks for 24 epochs); the gross-LOS
  screen cures what it declares (one-row rule: 101 to 33), the prefit fuse is blind; a birth floor
  removes the walk but slows every birth (hour 1 +0.10 m). Build: a DROP-only floor (RF-3a), the
  screen's one-row rule, bounded decoupling, same-epoch emission and station tier (RF-3b), the
  wire's discontinuity tokens (RF-3c).
- [GRB](GRB_golden_rebase_2026-09-22.md): closed (2026-09-22, the author's instruction), the
  goldens re-based from the RF-0g leveling posture onto the RF-0hv float-vehicle posture as armed
  today (frozen bases `config/refound/{field,sim}_gps_dual_rf0hv_golden_2026-09-22.json`, 27
  pins, the same 7 exceptions); `make_golden_config.py --verify` PASS on the first pass, armed in
  regime 51 to 57 (the six float-vehicle rows enter the regime armed); certified by 2 h arms
  byte-identical to the confirmation pair `runs/_rf0hv9_{field,sim}6`. Owed: ledger rows for the
  vehicle pair, `tracker_modeled_first` and `emit_pb_hold`.
- [SID](SID_signal_identity_2026-09-22.md): open (phases 1 to 6a BUILT 2026-09-22: the observatory,
  the tracker reader, the ledger-driven re-entry response, the reason-mask writers, the anchor
  re-entry and the roster declared on the wire, all byte-identical armed; the consumer's declared
  latch a trade, OFF; the second phase channel KEEP OFF, the L1W hole a receiver-tracking limit;
  channel_mode all correct and KEEP OFF: a family with no canonical station freezes its satellite
  level at ignition, the next read), signal identity and arc lifecycle: the estimator is per signal and stays; the
  layer deciding which signal stands for a band is decided in 128 sites with six independent
  rules, and 31 arc-lifecycle holders have both edges silent. Field baseline: the tracker's
  signal differs from the carried one at 46 cells of 44 of 58 stations, 0 of 31 band-1
  re-entries at L1W stations answered, the wire's L1W phase-bias family missing for 23.6 percent
  of satellite-epochs (the phase twin of card SCC's code defect, and card EV7's mechanism).
  Plan: SignalRoster and ArcLedger as an observatory, then five migration phases ending in
  channel_mode all.
- [GRK](GRK_gap_reentry_key_mismatch_2026-09-22.md): closed (BUILT and ARMED 2026-09-22 in
  the RF-0hv working bases; baseline pair `runs/_rf0hv9_{field,sim}6`), the gap re-entry response keyed on the slip
  tracker's signal, not on the carried one: the tracker takes the lexically lowest band-1 phase
  (L1C) while ten stations carry L1W, so 34 of 88 shifted re-entries are answered on band 2 only
  and the stale band-1 ambiguity folds across the break; emulation with L1C stripped at those
  stations: gross post-fit code rows above 10 m 507 to 103 (SGOC00LKA G13 393 to 1), consumer
  and wire unchanged. Built: `network.cycle_slip.tracker_modeled_first` names each band by the
  latched phase; gross rows 507 to 101 (12 h 759 to 261), unreachable re-entries 21 to 0; the
  exposure corrected to all 18 L1W stations (card SID).
- [SCU](SCU_station_coordinates_unknowns_2026-09-22.md): parked (2026-09-22, the author's
  decision: MIZU00JPN DROPPED from the field roster, baseline field `runs/_rf0hv8_field6`; the
  constrained-coordinate build deferred until the network works), station coordinates held or
  estimated. The
  roster audit (each roster station replayed as a consumer on the network's own wire) finds
  MIZU00JPN held 1.6 m from its data: the coordinate resolver applies no post-seismic model.
  Moved to its data: clock wire 0.0068 to 0.0044 m on all satellites, 11 of 16 census sites
  better, KOKB00USA loses half of its Up offset. The measure-only station-congruence screen
  had flagged it (and GRAZ00AUT, KRGG00ATF before card UPO) at 22 to 29 against a median of
  0.9 in every run, unread; it detects and cannot measure. All-free coordinates are not
  estimable next to free satellite clocks; proposed: a constrained coordinate state per
  station with a release for flagged stations.
- [UPO](UPO_consumer_up_offset_2026-09-22.md): closed (ARMED 2026-09-22 in the RF-0hv
  working bases; baseline pair `runs/_rf0hv7_{field,sim}6`), the consumers' standing Up offset:
  DEFECT, the network roster was held at MONUMENT coordinates while the observations refer to
  the antenna (48 of 59 stations, mean eccentricity 0.128 m, GRAZ00AUT 1.964 m; a station's wet
  delay absorbs a quarter of it, the wire carries the rest regionally).
  `network.station_marker_to_arp`: WTZZ00DEU 0.48 0.05 0.06 0.09 0.09 0.09 to 0.52 0.04 0.01
  0.01 0.02 0.01 m, mean Up -0.085 m to zero, clock wire 0.0072 to 0.0034 m; the sim truth bed
  reproduces the defect with injected eccentricities and the knob restores it byte for byte;
  12 h 875 to 1046 epochs below a decimetre; at the roster's own stations the absolute mean Up
  of a replayed consumer is smaller at 40 of 56.
- [SCC](SCC_second_code_channel_2026-09-21.md): closed (ARMED 2026-09-22 at scale 10 by
  the author's decision, AGAINST the card's failed frozen bar B1), build card, the second code
  channel (`network.second_code_channel`): a station contributes every allow-listed band-1
  code it tracks as code-only rows, with one receiver inter-signal-bias state; the wire's C1W
  coverage becomes complete (24 to 31 satellites per epoch, census code-bias dropouts 50 to 4),
  census hour 1 better by a median of 0.136 m, no consumer excursion left; it costs WTZZ00DEU
  0.02 m from hour 3, which the roster replays show to be a regional Up shift over Europe
  (eleven stations, mean -0.017 m; none elsewhere). The sim truth gate fails as frozen and the
  diagnosis names a standing per-satellite misfit of the PINNED code datum (0.285 m on the sim).
- [AB3](AB3_lanes_2026-09-20.md): open, 2026-09-20: add-back 3, lanes; the
  lane layer as built is loader-refused on the posture (it needs the retired
  invariant carry and the L5 extra-wide lane), so lanes are a new build on the float
  states; the readiness read AB3-0 PASSED: wide-lane integers exist on the FIELD under
  the float vehicle (concentration gap +0.283 against a bar of 0.10, 57 % of fractions
  within 0.15 cycle against 30 % uniform, bands still uniform); a lane design
  decision is owed, observatory first.
- [GAL](GAL_gps_galileo_dual_2026-09-22.md): open (BUILT 2026-09-22, main 787eca5, NOT
  ARMED: both beds run with every datum declared, the sim consumer better in every hour and its
  standing Up bias gone, WTZZ00DEU at 0.006 to 0.010 m in hours 4 to 6, the census 348 to 434.5;
  three defects fixed on the way; the split emulation exposes a posture-wide root, every
  satellite's clock level frozen at an ignition value, 1.5 m rms on the GPS-only sim baseline,
  which the author chose to fix before adoption), Galileo E1 and E5a beside GPS L1 and L2 on the RF-0hv posture: every roster
  station and every census site tracks Galileo, in two disjoint channel families (28 pilot, 30
  combined, WTZZ00DEU combined); the satellite-side inter-family truth is 0.05 m sd (COD OSB);
  gates: byte-identity, the pilot family on the sim against truth, a sim emulation of the split
  with injected inter-family constants, then the field with the 16-site census; the reading
  under test: the consumer errors left are geometric and regional.
- [CLV](CLV_clock_level_2026-09-22.md): BUILT e47b267, NOT ARMED, SEQUENCED; sections 7 and 8 (2026-09-25): return-plan batch 1, the follow share 1 - (f2/f1)^2 right on the split emulation (0.080 m), a C1W user already 0.36 m off CODE on the baseline wire (R-A2 next), hour 1 lost at a cold consumer (section 6, 2026-09-22: the GPS-only sim gates pass, datum-free 0.07 m at epoch 240 against 1.15; the field and the Galileo split emulation fail because a satellite with a free same-band code-bias family absorbs the level in that family and the re-base leaves it behind; return plan R-A to R-C; pre-registered 2026-09-22, the author's
  decision to fix the root card GAL exposed before continuing), the satellite clock level frozen at
  ignition by the carry: datum-free error 1.12 m rms across satellites on the GPS-only sim
  baseline for six hours, 71 percent absorbed by every station's free slant ionosphere; a
  22-arm ladder finds no single knob and one lever (the free-ambiguity era converges it at the
  code's rate); design: the border carry inflated along each satellite's level direction by the
  code-only level variance, `network.clock_level_carry`.
- [OWD](OWD_owed_defects_2026-09-22.md): CLOSED 2026-09-23, BUILT `d64c230` (section 5: every gate passes and the products are byte-identical on both beds; a replay of the new wire is the in-process consumer; the census counts equal at 16 of 16 sites; sequenced O6, O7 and health; pre-registered 2026-09-22, the author's
  direction to fix the mistakes card GAL uncovered before continuing), GAL's owed list less the
  clock level (card CLV): the S contract line on every wire, naming the run's own product files,
  and an undeclared replay refused (`network.replay_allow_undeclared_wire`); every state-manager
  re-inflation raises and never shrinks; the group delay latched as the processing window's
  median healthy record; one admission door (`processing.active_systems`); post-fit rows named
  by signal; the BeiDou pair rule, the sim fixture, the Galileo yaw law and health sequenced.
- [HLT](HLT_broadcast_health_2026-09-25.md): open (BUILT 2026-09-25, NOT ARMED: neither mode is
  consistently better on 2 h arms of four beds and the census, because any change of the satellite
  set or the pivot re-draws card CLV's ignition-frozen levels; the mode decision is the author's,
  sequenced behind card CLV), 2026-09-25: build card, broadcast satellite health on the network path (the health item card OWD sequenced); DOY 129 flags three processed
  satellites all day (G01, the GPS clock pivot by lexical order, health 63; E14 and E18) that
  CODE's final products carry at full rate; `network.sat_health` (default off) measures two
  modes side by side, "pivot" (the election passes over an unhealthy satellite) and "exclude"
  (its observations removed at the synced-epoch door), on 2 h arms of four beds and the
  16-site census.
- [CLV2](CLV2_clock_level_batch2_2026-09-25.md): open (section 6.8, 2026-09-28: the section 6 code kept
  on branch `wip/clv2-station-share` as the record, not merged, the author having chosen a consistent
  recursion instead; section 6, 2026-09-28: the re-base made phase-exact
  on the field by moving each station's ambiguities and the held datum with the level
  (`network.clock_level_station_kappa`, built, off) and arming on a settled level: hour-1 phase-row
  change 0.001 to 0.005 m, census +0.007 / +0.001 m; the sim's first hour and the Galileo split
  still owed; section 5, 2026-09-28: the census loss is in the
  wire's phase rows, not its code rows (hybrid wires: code rows only cost nothing, phase rows only
  reproduce the loss); the hold transfers each declared level step to the phase bias while the
  estimator keeps a median 0.54 to 0.89 of it in the clock, so the phase row moves by the remainder,
  0.07 to 0.09 m rms; revised batch 3: a phase-exact re-base; BUILT 2026-09-25, NOT ARMED: R-A2 see-saws with the
  solve, the consumer response repairs the split emulation's hour 1 (0.516 to 0.222 m) and not the
  census, the candidate costs the 10 C1W census sites +0.59 / +0.19 m; batch 3 proposed), 2026-09-25:
  card CLV's return plan, batch 2;
  R-A2 re-bases each free same-band code-bias column (GPS C1W) from the same-band code
  difference at the 40 stations tracking both codes (its per-satellite effect already reads the
  wire's C1W - C1C error against CODE at correlation 0.68 to 0.74), and the consumer answers a
  declared level step by inflating the stepped satellite's phase-blind ionosphere-ambiguity
  direction, where it absorbed 71 percent of the old code level; R-C's common-shift declaration
  sequenced; bars C0 to C3 frozen.
- [PBF](PBF_pb_hold_presence_edges_2026-09-28.md): closed (ARMED 2026-09-28 at the author's instruction:
  working bases, golden pins, ledger A13, posture row; baseline pair `runs/_rf0hv11_{field,sim}6`,
  byte-identical to the post-OWD references, 0 edges at either held-out consumer; BUILT 2026-09-28:
  P0, P1 and P3 pass, P2 fails as frozen with the catastrophe removed, ARHT00ATA 803.9 to 0.762 m in
  hour 2 on the CLV wire, the rest card CLV2's phase-side cost; a candidate for the author's arming
  decision), 2026-09-28: build card, presence edges of a held phase bias at the consumer. Under the
  hold (card PBH) a bias that appears on, or leaves, a phase row the consumer already uses steps the
  row by the whole bias (G32 L1W at 92.4 m) and nothing answers it; the Kalman peel then decides, and
  at ARHT00ATA it resets the wrong satellites and the solution leaves by 490 m in one epoch (14 of 90
  census keys above 400 m in hour 1). `processing.ssr_pb_hold_presence_flip` (harness
  `network.closed_loop.pb_hold_presence_flip`) gives a held key's A5 edge the reaction of an unheld
  key; the consumer replay now writes card CST's per-satellite trace.
- [CTU](CTU_coupling_time_update_2026-09-28.md): open (section 14, 2026-09-29: the follow-up is card
  CTU2; section 13, 2026-09-28: BUILT, default
  off, not armed; floor folds 63 % to 0 % and sim level 1.15 to 0.28 m, but G1, G2 and G4 fail;
  next: the ignition damage, the anchor guard on the level, a level instrument), 2026-09-28: pre-registered from the
  outside review of the brief "Clock Level Handoff" (card CLV2 section 6.8). The clock level's
  root is the coupling rows' scalar time update: a clock term is shrunk with the legacy 1e-3 m2/s
  while the border folds the clock at the anchor's 2.9e-8 with a 1e-5 floor (kappa 3.3e-4 on the
  clock term, 1 on the phase-bias term), so every carried ambiguity row, with the station's two
  phase rows, fixes the satellite clock and the slant ionosphere at phase precision; the
  switch-off arms confirm it (sim level 1.15 to 0.19 m; 4602 of 7327 clock folds at the anchor
  floor in the control, 0 with the update off). Build: the exact conditional time update (one rule
  for present, decoupled, departed and pinned columns, Q read from the fold), the level's
  code-only bound 1 / I_L as a raise at the switch-on and a monitor on every run (card CLV's
  accumulator kept, its corrector retired), and the hold decision in the same card; gates frozen
  before any arm.
- [CTU2](CTU2_level_recursion_followup_2026-09-29.md): open, 2026-09-29: card CTU's follow-up, nothing
  armed. The sim's ignition damage is a fixture artifact (orbit extrapolation in the 24 h sim fixture's
  first epochs; from 00:30:15 the control converges to 0.14 m and freezes). The diagonal receiver
  code-bias carry doubled the border information every epoch (`network.carry_rx_code_bias_in_block`
  folds it in the station block). The monitor carries a truth-free level instrument (split-half
  NEES), the anchor guard a level-blind phase statistic (`network.clock_anchor_guard_space`), and a
  float-vehicle drop now decouples an arc at birth strength (`network.amb_float_drop_decouple_always`).
  Clean-start sim with the exact update: G1 and G2 pass (0.03 m at 2 h); field G4 0.164 m against
  0.15; G5 fails (a switch-on keeps the level, held by the restored ambiguities); the consumer is
  worse with the exact update under every hold setting, so the hold needs a level-aware form next.
  6 h: the departed pivot's deficit is spread over the present clocks (0 withheld epochs, was 30).
- [LAH](LAH_level_aware_hold_2026-09-29.md): open, 2026-09-29: the level-aware phase-bias hold
  (card CTU2 section 9 item 1), built and gated, default off, not armed. Every hold that keeps the
  phase-visible clock c - p at its generation-start value plus a motion term is level-blind; the
  smoothed motion (the estimator's own model) passes the ionosphere-free phase-bias wander because
  the estimator states the phase-visible clock to millimetres while it wanders centimetres, so the
  zero-motion member is built (`network.emit_pb_hold_phase_clock`, `pbhold=2`). 6 h census with the
  exact update: 2.27 / 0.75 / 0.57 / 0.38 / 0.26 / 0.20 m under the plain hold, 0.59 / 0.17 / 0.12 /
  0.08 / 0.10 / 0.08 with this one (control 0.67 / 0.17 / 0.10 / 0.08 / 0.09 / 0.07); named C1W
  regressions (KOKB, QUIN, UTQI, ARHT); posture limit: the a priori clock's motion is load-bearing
  on the phase side within a generation.
- [PVN](PVN_phase_visible_nees_2026-09-29.md): open, 2026-09-29: pre-registered (card LAH's critical
  path): the estimator's phase-visible consistency instrument, measure-only
  (`network.phase_visible_monitor`): the normalized update of the ionosphere-free phase-visible
  clock on both beds and its within-arc state NEES on the sim. Built and read the same day: sim
  NIS 0.87 / 1.03, NEES 0.2 / 0.3 (control / exact); field NIS 0.40 / 0.10, NEES 0.6 / 0.2. The
  covariance along D is consistent to conservative, not overconfident, which corrects card LAH's
  premise for rejecting the smoothed hold; the smoothed member is owed a build.
- [PRM](PRM_parameter_registry_2026-09-29.md): open, 2026-09-29: pre-registered (the author's
  question after cards CTU and CTU2: parameter objects whose properties a human can read). The
  PPP path has a registry (`StateManager`, `StateKeySchema`); the network path has identities
  without properties: a border parameter's process noise is decided at four sites plus card
  CTU's mirror, its floors at two, its event responses on four paths, and the ten carried maps
  that reference it are enumerated by hand in six functions, the shape of five defects of the
  last month (the CTU root, the two-of-ten re-level, the rx code-bias double count, the drop
  that did not decouple, the hand-built re-base). Build: a declared descriptor per parameter
  kind read by every mechanism, one traversal over every carried reference, a run-end "model as
  armed" table, a declarative knob table; five byte-identity-gated phases after CTU2's code
  lands; no estimator class hierarchy, no unification of the epoch-skip payloads.
- [SMH](SMH_smoothed_hold_clone_2026-09-29.md): open, 2026-09-29: pre-registered (the author's
  decision after card PVN): the smoothed member of card LAH's hold family, P_j(t) = p_j(t0|t0) +
  [c(t0|t) - c(t0|t0)], with c(t0|t) carried as a clone (a coupling row with no observations in
  its own carry site, time-updated by card CTU's exact rule, never shrunk; reverts to the
  phase-clock transfer when its clone dies). `network.emit_pb_hold_smoothed`, `pbhold=3`,
  emit-side only. Gates: consumer equal to the phase-clock hold's within millimetres on both beds;
  the passed motion against its stated variance as a baseline; one adaptation arm with a known
  walk injected into the a priori clock product and a pre-registered hidden fraction. BUILT, not
  armed (sections 7 to 11): the clone lives while card Q3's anchor holds its clock; one frame for
  the hold family (the transfer is card LAH's minus the passed motion); G3 passes 97 % of an
  injected walk with the anchor's q matched; `network.dense_fold_kappa` default 1e-12; the 6 h
  field census loses at the C1W sites from hour 3 by exactly the passed motion, read as mostly
  level (section 11); the member that separates the walks is card SMD.
- [SMD](SMD_smoothed_hold_block_member_2026-09-30.md): open, 2026-09-30: pre-registered (the
  outside review's fourth note after SMH section 10): the smoothed hold on the ionosphere-free
  phase-visible clock D = c - sum_j w_j p_j, passing Phi_D = D(t|t) - D(t0|t) on a (c, p_1, p_2)
  block clone with the band pair fixed at birth; a level walk cancels, a clock walk passes, and a
  phase-bias re-realization is hidden by retiring the clone at every re-reference, new anchor
  tenure and emitter mark of its pair. `network.emit_pb_hold_smoothed_block`, `pbhold=4`, emit-side
  only. Predictions: on the field the level part carries the centimetres and the phase-visible part
  is small; the census's hour-3 loss disappears and its hour-1 gain stays; on the sim the two
  members differ by the sim's small level wander. BUILT, NOT armed (sections 7 and 8): P1 and P2
  FAIL: the level part is zero (the phase biases are constants of the model, so the smoother reads
  every level move as a revision) and the census loss from hour 3 stays; the passed motion is all
  phase-visible, about 1 cm p90 of estimator wander on the sim and twice that on the field. A
  defect of card SMH's bridge (the epoch's clock step dropped at every switch) found and fixed.
- [QPV](QPV_anchor_q_phase_visible_2026-09-30.md): open, 2026-09-30: pre-registered (the outside
  review's decision 2, once card SMD's split existed): the anchor's clock q derived from the
  phase-visible passed motion, by a ladder of `network.clock_anchor_q` (a quarter to four times
  card Q3's 2.9e-8 m2/s) on both beds, the rule that the field's passed motion net of the sim's
  floor equals its stated walk, the census at the derived q and an emit-side shrinkage read.
  Predictions: the derived q within a factor 2 of the floor; no q at or above it removes the
  hour-3 loss. READ: the rule gives 3.1 times the floor (Q1 fails); at that q the census loss
  stays and the producer pays (Q3 holds); the anchor's q stays at the floor; the shrinkage read
  scales the hour-1 gain and the later loss together.
- [REV](REV_revision_member_replay_2026-09-30.md): closed, 2026-09-30: pre-registered (the outside
  review's note after card QPV, on QPV's last lever): the revision member of the hold family, the
  wire's phase side at D(t0|t) (the smoothed generation-start value of the phase-visible clock), so
  the hold passes the revision R = D(t0|t) - D(t0|t0) instead of card SMD's walk share, frozen per
  block clone when the clone's own stated variance of D(t0) falls by less than the stated walk adds.
  Replayed offline from card SMD's 6 h block arms on both beds (the rebuilt-wire trick) with five
  trace columns added and the arms re-run; no member built. Predictions: the hour-1 gain above card
  SMD's; unfrozen, more late wander than card SMD; frozen, the phase-clock hold from the freeze on;
  a timing model expects the freeze late on the sim and not within 6 h on the field. FALSIFIED the
  same day: R1 fails (hour-1 change +1.2 m on the field and +1.3 m on the sim against the phase-clock
  hold), R3 fails, R2 and E hold; the ignition convergence is nearly all revision and passing it
  reproduces the plain hold; a float consumer absorbs any constant phase-side value and pays for
  every change. Not built; the declared-record alternative loses its premise; lever 1 remains.
  Section 8 (2026-10-01): the field revision NEES of 4 to 18 read a constellation-common drift
  (with the common step removed, 0.04 to 1.19, the sim's size).
- [WSX](WSX_within_station_cross_terms_2026-09-30.md): closed, 2026-09-30: pre-registered (lever 1
  after card REV, the first step of the joint-covariance sequence): the exact time update's
  covariance between two carried rows of one station, M_i' B M_k, dropped by the shipped form,
  re-built behind `network.carry_coupling_exact_cross` in the whitened local block the float vehicle
  keeps live; the CTU section 13.4 (c) drift arm predates four posture changes. The unit gate gains
  the shipped form against the exact batch. Predictions: the shipped form states the level tighter
  than the form with the cross terms, both within a factor 2 of the batch; no drift on the current
  posture; the satellite-side instruments move by less than 0.1. KEEP_OFF the same day: harmless
  and almost invisible on 2 h arms of both beds (no drift, consumer within 0.006 m); the unit
  network's size study shows the shipped form over-stating a between-satellite phase-visible
  contrast (0.885 / 0.661 / 0.336 of the exact variance at 8 / 16 / 48 stations), the dropped
  cross-station half, which the within-station half worsens. Card PVN's contrast statistics cannot
  see an over-statement of this size; the deciding read is the augmented previous border's.
- [FXA](FXA_field_excess_phase_visible_motion_2026-09-30.md): closed, 2026-09-30: pre-registered
  (lever 1 after cards REV and WSX): the field's excess phase-visible motion (clones born after
  convergence revised 2 to 4 times beyond their stated variance, absent on the 60-station sim)
  tested on card REV's traces with the martingale property of the smoothed generation-start value:
  steps at declared events, increments larger than stated, or a drift. Prediction: a drift. READ the
  same day: a drift (the field's increments persistently correlated, the sim's white; events at
  their base rate), stationary and common to the constellation (the per-satellite remainder is the
  sim's size): a clock-datum drift that the smoothed hold's per-clone walk share makes
  between-satellite, a candidate for the census's C1W loss from hour 3.
- [SMC](SMC_smoothed_hold_common_step_removed_2026-10-01.md): closed, 2026-10-01: pre-registered
  (card FXA's amended decision): card SMD's member replayed offline with each epoch's
  constellation-common step removed from its walk share, so a common drift the clones' different
  births made between-satellite no longer reaches the wire as a difference. Predictions: the C1W
  loss from hour 3 disappears, the hour-1 gain keeps half its size, the sim does not get worse.
  FALSIFIED the same day: the common drift is not in the walk share (the smoother books it as
  revision), the wire moves by at most 3 mm and the census not at all; what card SMD's member
  passes is per-satellite. The hold family is closed on this estimator; the phase-clock hold stays.
- [WSL](WSL_walk_share_stations_link_2026-10-01.md): closed, 2026-10-01: pre-registered (the outside
  review's note after cards WSX, FXA and SMC): whether the field's larger walk share (25 against
  9 mm between satellites) is card WSX's carry defect seen through the hold, tested from existing
  traces and the unit network: the stated contrast and the walk share against the stations
  observing each satellite, with clone age as the rival, on both beds. If it holds, the augmented
  previous border is the critical path. FALSIFIED the same day: the field's satellites are not seen
  by more stations (median 12 against 15), the stated contrast does not tighten with the count,
  matched counts leave the beds 1.5 apart; the augmented border is not the critical path.
- [PAC](PAC_common_drift_pivot_clock_2026-10-01.md): closed, 2026-10-01: the author's question after
  card FXA, whether the constellation-common drift is the clock pivot's a priori clock. The traces
  say no: the pivot's own phase-visible clock and the smoothest receiver clocks move with the
  fleet, so the drift is the network's phase-visible time scale floating on the pinned code clock;
  one 2 h field arm with a well-tracked pivot pinned tests whether the unhealthy, sparsely tracked
  G01 is what lets the field's float grow. ANSWERED: the drift is the pinned pivot's level converging
  on card CTU's posture; G14 pinned cuts the settled drift to the sim's size, consumer unchanged.
- [FWS](FWS_field_walk_share_sources_2026-10-01.md): closed, 2026-10-01: the author's question after
  card WSL. The field's walk share is larger because its per-satellite phase-visible steps are
  persistent (correlated over minutes to an hour), not larger; four trace reads and seven one-change
  bisection arms on both beds attribute the persistence; the station side is the leading candidate.
  ANSWERED: short scales are the stations' time-correlated phase errors seen through thin coverage
  (the sim's noise is white); long scales mostly the field's fractional wind-up staircase
  (accumulating it removes 45 % of the hour-scale excess, consumer-neutral under the phase-clock hold).
  CORRECTED (section 6): the restart response refloated no arc; the RINEX-op sim arms ran 1.53 h
  (re-run at 2 h, verdicts unchanged); the census consumers were fractional.
- [SOI](SOI_second_order_ionosphere_leak_2026-10-01.md): closed, 2026-10-01: pre-registered (the outside
  review's note after card FWS): is the second-order ionospheric term, which the ionosphere-free
  combination keeps and which changes sign with the line of sight's angle to the geomagnetic field,
  what leaks from the field stations into the satellites' phase-visible clocks? A truth-free test on
  existing traces: the term predicted from the estimator's own slant ionosphere and the dipole field
  against the signed post-fit residuals and the walk share's changes, the sim as the null.
  FALSIFIED: the term is 0.7 mm rms in its unabsorbable part against 4.1 mm of slow residual, no
  slope at the residual level (-0.02 +/- 0.08) and no correlation with the walk share at any lag.
  ANNOTATED (card I2S): the sign question it left open is settled for section 1's derivation; the
  engine's convention was inverted and is fixed.
- [LSR](LSR_long_scale_remainder_2026-10-01.md): closed, 2026-10-01: pre-registered (the outside
  review's note after cards FWS and SOI): the field's walk-share persistence that remains at an hour
  and two hours with the wind-up staircase removed, against its two remaining candidates
  (station-set changes as satellites rise and set; tropical wet-delay variability the estimator's
  1.9 mm per root hour random walk cannot follow). Two trace reads and two one-change 6 h field arms
  (the wet-delay q, troposphere gradients).
  FALSIFIED: the posture re-estimates the wet delay every epoch (the q arm is byte-identical),
  gradients worsen the persistence, and set changes add no walk; untested: phase-centre and
  long-period multipath errors.
  ANNOTATED (card SFR): the remainder is mostly the slow tail of the stations' coloured phase error.
- [WUP](WUP_windup_accumulated_promotion_2026-10-01.md): closed, 2026-10-01: pre-registered (the outside
  review's decision after card FWS): accumulated phase wind-up for the field lineages by the
  promotion path. The restart response it needs walked the live ambiguity map and so never reached a
  re-entering arc; fixed first (the drain reaches it through the leveling carry), then gates G0 to
  G3 (byte-identity off, the fix on the program posture, the golden pair, the multi-site census).
  ARMED: every gate passed; 27 of 28 field restarts answered, the staircase gone, the census with
  accumulating consumers 2.6 cm better at the median site (the consumers' own mode); baseline pair
  `runs/_rf0hv12_{field,sim}6`.
- [SFR](SFR_sim_fixture_coloured_phase_2026-10-01.md): closed, 2026-10-01: pre-registered (the outside
  review's decision after card FWS): the sim fixture re-based with the field's coloured phase error.
  The field's post-fit ionosphere-free phase residual is about 90 % a Gauss-Markov process (5 to 7 mm,
  tau 80 to 120 s) over 2 mm of white noise, the sim's about 80 % white (14 mm); a band-common
  per-line-of-sight Gauss-Markov term in the simulator, calibrated per station class on the post-fit
  instrument, and the white noise cut, in one regeneration (C1 to C4).
  RE-BASED: six calibration rungs (the colour capped at 20 degrees of elevation is what matches); the
  sim golden reads `runs/simnet60_quiet_col_24h` (baseline `runs/_rf0hv13_sim6`); old sim numbers superseded.
  The colour also carries the field's walk-share persistence out to an hour on the sim (6 h).
- [I2S](I2S_second_order_iono_sign_2026-10-01.md): closed, 2026-10-01: the sign of the engine's
  second-order ionospheric term (`model_corrections.enable_higher_order_ionosphere`, default off),
  left unadjudicated by card SOI: the engine and POINT project B on the receiver-to-satellite line of
  sight with a positive code coefficient, card SOI section 1 derives the opposite sign from the
  signal's polarization. Checked against first principles, the literature, a wind-up handedness
  anchor and independent implementations.
  FIXED: the propagation direction (satellite to receiver) is correct; a cold-plasma eigenmode solve
  (32 of 32 checks), IERS 2010 eqs. 9.19 to 9.46, Moore and Morton 2011, Petrie et al. 2011, the
  field-validated wind-up model, Ginan and PRIDE PPP-AR agree; POINT carries the same inversion with
  nothing compensating it. Engine and oracle fixed together, the four I2 goldens flip sign, a direct
  sign test added; no network result depends on the term.
  ANNOTATED (section 9): the knob cannot act on the network posture (the network dispatch precedes
  the GIM, the real-time consumer skips the term); left off on the goldens by the author's decision.
- [LGM](LGM_los_phase_nuisance_2026-10-01.md): open, 2026-10-01: pre-registered (the ranking recorded
  after cards SOI, I2S, LSR, SFR and WUP): a band-common, phase-only Gauss-Markov state per station and
  satellite, carried epoch to epoch, so that the stations' coloured phase error (90 % of the field's
  post-fit residual, card SFR) is modelled instead of stated white; built through card PRM's
  registry as its first new kind, truth-graded on the sim fixture's own colour realization.
  C0 READ: the phase sigma times 2.2 is not a substitute (hour 2 worse on both beds, the residual
  still coloured, the persistence moved in opposite directions on the two beds).
  BUILT AND READ (sections 4.1, 5.1 to 5.3), default off, not armed: no-harm with the posture's white
  sigma (field golden consumer hour 1 0.559 to 0.440 m) but starved; whitening with the white sigma
  at the white level and a sim consumer cost. The carried ambiguity moves 3 to 7 mm per epoch per
  line of sight, so the colour cannot be separated from it: open behind card AIL.
- [AIL](AIL_ambiguity_information_loss_2026-10-01.md): open, 2026-10-01: pre-registered (card LGM section
  5.3): the float vehicle's carried ionosphere-free ambiguity moves 3 to 7 mm per epoch per line of
  sight instead of being phase-stiff, which keeps the colour state from carrying the colour. A
  trace-class instrument reads the ambiguity's variance before and after each epoch to place the
  loss at the inject or in the solve.
  READ: the golden posture discards a young arc's information every epoch (card CTU's root) and
  loses little at maturity; the exact update loses 2 to 3 mm per epoch steadily (the anchored
  clock's walk through each row's regression). Owed: the anchor-q-0 read; a joint, unconditioned
  carry of the border and the station's ambiguities as a design question.
- [RPD](RPD_replay_station_prior_2026-10-01.md): closed, 2026-10-01: the within-epoch RAIM replay
  (`resolve_dropping`) re-added each station without its pre-PD datum rows
  (`network.station_prior_before_pd`, card RF-PD-fix), so no replay carried a receiver-DCB datum row.
  FIXED: a station positive-definite only with the row (the first assessed epoch, a station returning
  without a carry) aborted the replay and its restore (30 consecutive epochs on a forced field arm
  with a station outage, the estimator holding 7 of 57 stations, none of it in the run-end counters);
  elsewhere the culprit-free replay missed the solve it replays by up to 7.9 mm (field) and 10.6 mm
  (sim) in phase bias. The same prior now passes at all three re-add sites; byte-identical where the
  tier does not adapt (the goldens), the unit suite passes.
  RESTORE PATH HARDENED (section 7): a failed re-solve is undone exactly to the last landed state,
  attributed to a station and counted; the undo leaves out a station it cannot re-add (the primary's
  own rule) and only an undo that still cannot complete skips the epoch. Against the card's own
  defect: 30 epochs on a partly rebuilt estimator unhardened, 80 withheld by a skip-only first
  build, 2 failures and 240 of 240 epochs emitted as committed.
- [EXL](EXL_exact_update_lah_promotion_2026-10-01.md): open, 2026-10-01: pre-registered (the author's
  decision after cards LGM and AIL): card CTU's exact coupling update and its four companions armed on
  the goldens together with card LAH's phase-clock hold as an interim hold, by card WUP's promotion
  path. Fresh 2 h evidence: under the plain hold the exact update costs the field consumer 3.3 m in
  hour 1; under the phase-clock hold it reads 0.108 / 0.024 m. Gates in section 3 (G4 and G8 re-stated
  relative, G5 disclosed).

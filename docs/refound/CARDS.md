# The current program: open and parked cards

The cards of the network re-founding program
([NETWORK_REFOUNDING_2026-09-14.md](NETWORK_REFOUNDING_2026-09-14.md)) that are open or parked,
one entry per card in pre-registration order. A new card is pre-registered here (rule 3 of
[the index](../INDEX.md)); when it closes, `python scripts/docs_record.py close <card>` moves the
card into the record and its entry to [the record's card list](../record/refound/CARDS_part2.md).
The closed cards of the program are listed in [record/refound/CARDS.md](../record/refound/CARDS.md)
and [record/refound/CARDS_part2.md](../record/refound/CARDS_part2.md).

- [AB1](AB1_sh_consumer_product_2026-09-20.md): parked, 2026-09-20: SH VTEC add-back: frozen rule FAILS, consumer constraint KEEP_OFF, product specified only.
- [SCU](SCU_station_coordinates_unknowns_2026-09-22.md): parked, 2026-09-22: station coordinates: MIZU00JPN 1.6 m off its data, DROPPED; constrained states PARKED.
- [AB3](AB3_lanes_2026-09-20.md): open, 2026-09-20: lanes add-back: field wide-lane gap +0.283 (bar 0.10), old layer refused; SEQUENCED after L5, design owed.
- [BCL](BCL_previous_border_on_clones_2026-10-08.md): parked, 2026-10-08: the whole previous border on clones (card SCL's
  clone extended to the phase and code biases, BCL-a, and to every key card CTU re-attributes, BCL-d). BCL-a nil at +40 %
  wall time (only the code biases pass the screen); BCL-d exact at events but FAILED G2 on the held-out consumer
  (+15 / +17 mm), census neutral. Both default off, nothing armed.
- [L5B](L5B_band5_receiver_delay_2026-10-10.md): open, 2026-10-10: L5 steps R-1b (the receiver band-5 code delay
  estimated: band-5 offsets 18 to 3 stations) and R-1c (co-resumed signals share the re-entry verdict), both default off;
  the field contamination is one event at MAJU00MHL moving its station level direction; consumer cost = Block IIF L5
  phase. 2026-10-13: a station-local attribution (section 9) shows MAJU's own rows carry centimetres; the move
  comes through the shared solution. L5 SEQUENCED: R-1d (robust ignition of receiver code states) is the author's design call.
  2026-10-23 (sections 10 to 14): GPS L5 and every sim channel stable on the GPS + Galileo sim golden; on the field
  Galileo E5b clean at 2 h and GPS L5 still the MAJU00MHL event; the jump is in the anchored solve, the station's
  receiver code states are set by the shared feedback, and the DCB-only datum barely holds the GPS level. Successor: R1D.
- [L5B6](L5B6_l5_six_hour_read_2026-11-10.md): open, 2026-11-10: continuation of L5B (pre-registered): the floor
  `network.amb_float_drop_var_m2` 100 with and without GPS L5 over 6 h on field DOY 129 and 132 and the sim, the 2 h
  result repeated on main `b6bbea7`; then L5 at the user (measure only). Run keys only, nothing armed.
  Stage A (2026-11-10): 2 h holds; at 6 h the floor removes the event, dual and 16 sites neutral; three-band still
  fails one clause per field day (tracked clock wire 1.17x DOY 129; user hours 1-2 DOY 132), users -7 to -11 mm. SEQUENCED.
  Corrected 2026-11-11 (section 5): the 1.17x was a Galileo row; GPS 0.56x, so three-band with the floor passes DOY 129
  and fails DOY 132 only. Stage B (2026-11-11): the three-band loss sits at the user, from IIF L5 (site-common drift the
  corrections do not carry); without IIF L5, 15 of 16 sites equal two-band; roof drops L5 (no G05 for LEIAR25). SEQUENCED.
- [IRD](IRD_integer_recovering_datum_2026-10-13.md): open, 2026-10-13: the integer-recovering datum on the eq34
  chain (a frame-preserving event algebra on the float vehicle, observatory first, proven end to end on the sim);
  S-basis derivation, event table, bundle A0, every bar and decision rule frozen. IRD-0 STOPPED by its negative
  control: golden chord WL and 6 h wire E_L1 up to 0.029 cycle off the null (tolerance 0.02); graders, truth-wire
  positive control and the E_b slope pass. Re-registered by the author's ruling (IRD-0r: above 0.15 cycle and
  within max(0.05, the null's 99 % band)); PASSES in 44 of 44 cells; re-based onto `a02ccfb`. Results from IRD-1 on
  in IRDB, IRDC and IRDD; IRD-2's stop closed by the author's ruling (2026-11-02); IRD-3 landed (IRDC section 5);
  the certification rule closed as a float defect by the author's ruling (IRDC sections 6 and 7); IRD-4's kept row
  landed default off, I-b and the r3c consumer FAILED (IRDC section 8); the epoch-88 divergence is the zero-target
  cold pin's integer, C6 (IRDD section 2); the returning row landed default off, the return shock removed and
  I-b FAILED (IRDD section 3); the rehost row landed default off, the double-difference damage removed and
  the float consumer FAILED (IRDD section 4); the root row landed default off, the handover's step removed and
  I-b FAILED on the fractional bar (IRDD section 5); the slip row landed default off, the slipped edge out of the
  tree at the slip epoch and I-b FAILED (IRDE); the merge and split row landed default off, a split no longer steps the frame and
  I-b FAILED; the receiver floor is traced as the cause of A1's wet-delay state and of most of the DCB spread (IRDE section 9).
- [IRDB](IRDB_integer_recovering_datum_continued_2026-10-30.md): open, 2026-10-30: card IRD's continuation, from IRD-1
  on. IRD-1 LANDED, measure-only and default off (the chain's event table, edge and chord traces, CHAIN events,
  pre-hold and receiver phase-bias traces, the run-end frame census; the consumer's shared AR builder, observe-mode
  integer search, fix traces and extended gate); every gate passes (`rt_seam_verify.py` retired, the retired lineage
  graded same-build). A0 on rung 3c: 144 re-hosts in 2 h, 128 stepped by a fraction, none certifiable; RPB NEES 2.58.
  Truth wires: 0 wrong of 2 506 settled integers per bed; the golden wire admits nothing. IRD-2 STOPPED by the
  static-algebra stop rule (2026-10-31): microscope chords event-free for 30 min at 0.22 cycle (bar 0.02), because the
  double differences under bundle A0 drift off the integers after half an hour (R1 exact); certification and lattice
  rules fail, D0 keeps the floor (GF-datum defect), seed-round not adopted, D-N fires; nothing from IRD-3 on until the
  cause is closed. The bisection (section 4) puts the cause in the chain's event handling, not its static algebra: A0
  is R1 plus the chain on the microscope, and with the floor off the chain tracks R1 until the first satellite return;
  the author's ruling was owed. Its event causes are traced in IRDC, and the author ruled the stop closed (2026-11-02).
- [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md): open, 2026-11-02: card IRDB's continuation, the trace of
  the IRD-2 stop's event causes. The first satellite return's shock is traced: the chain reads a returning satellite
  as a first sighting with a known zero step (extend-only can never keep a tree edge: a pinned arc carries no leveling
  entry), so its old-frame carries survive, and the float vehicle folds them at a birth variance the chain makes as
  tight as 7.8 mm against a 2.1 m frame step. One knob (`phase_leveling_max_gap_s` 600) prunes them and removes the
  shock (end-of-run reference-free residual 0.257 to 0.036 cycle; R1 unchanged); the stop rule still fails (0.090
  cycle). Every kept re-pin is a stale-carry re-pin. The remaining drift is card IRD C8 damage from the cold re-host
  at epoch 88 and a flipped kept edge at epoch 210, which the election keys on parent identity (C7) and declares an
  unknown step; every frame step that damages the microscope's double differences (RPB floor off) is traced, none
  in the static algebra; the fixes are IRD-4's event rows. The author ruled the stop closed (2026-11-02, section 4):
  IRD-3 onward may be built in card IRD section 11's order. IRD-3 (section 5) LANDED default off: the pair family
  pins one tree on both bands at full rank and reproduces A0 byte for byte on r3c and the golden sim (every arc on
  both bands); P-c FAILED on four clauses, as A0's. A one-band fixture runs the band extension on data. The
  certification defect re-measured (section 6): the certifier is sound (no wrong integer on a float its covariance
  describes); every wrong would-be certification is a float the events or the RPB floor damaged, so no sigma scale
  fixes it. The author ruled the rule closed so (section 7): IRD-4's certified re-hosts are held until they grade
  0 wrong. IRD-4's kept row (section 8) LANDED default off: every kept edge held with no unknown or whole-cycle step,
  the flips' re-references gone; I-b FAILED on every bed and the float consumer FAILED on r3c in hour 2, where a
  reversed subtree takes its parent re-host's estimated step. Continued in IRDD.
- [IRDD](IRDD_integer_recovering_datum_rows_2026-11-03.md): open, 2026-11-03: card IRDC's continuation, IRD-4's event
  rows from the trace of the epoch-88 divergence on. The divergence is card IRD C6, not the kept row: at the only
  cold re-host after convergence the zero target meets the old frame that the subtree's surviving chords keep, so
  the solve carries the new arc's whole old-frame float (-22 / +31 cycles as built, +42 / -87 with the row: exactly
  the integer the row's frozen targets left, fraction 0.02 cycle). A scratch diagnostic that changes only that
  pin's target swaps every reading with the integer; rounding it away makes the event free (double differences
  integer-clean from 70 min in both arms), and the epoch-210 re-host is its aftermath too. The `returning`
  row (section 3) LANDED default off: every returning edge takes the cut release with its identities and
  declared keys; at the golden's 900 s horizon the floor-off microscope's wire equals the 600 s horizon arm's
  byte for byte (the return shock removed); I-b FAILED on every bed; the consumer holds its bound. The
  `rehost` row (section 4) LANDED default off with its certified branch held: every re-host takes the cut
  release, which removes the C6 and C8 double-difference damage (the search's wrong would-be fixes 3 282 to 20
  on the floor-off microscope); the float consumer FAILED in hour 2 on every bed, about twice the previous
  arm, and I-b FAILED. The author chose to continue in table order with the hold as built. The `root` row
  (section 5) LANDED default off: a new root's receiver phase bias is pinned at its carried value, so the
  satellites on its side keep their frame and carries (undeclared integer steps at the handover 0 against 235
  to 262) and the consumer holds its bound; I-b FAILED on the fractional bar, at the size the same pairs reach
  at quiet epochs. Continued in IRDE.
- [IRDE](IRDE_integer_recovering_datum_slip_2026-11-06.md): open, 2026-11-06: card IRDD's continuation, from IRD-4's `slip` row on. The row
  LANDED default off: an arc in the free-epoch latch leaves the tree at the slip epoch, the fragment re-attaches through
  a new edge under the rehost row, and a slipped arc no other route can replace takes the cut release. As built the cycle
  stays in the pinned arc's phase row and the target can fall to 0 (+57 and -91 cycles on ird_t1). The class's as-built
  steps fall to 0, both edges carry identities, and the consumer and W5 pass, a weak test here because the hold keeps
  the emitted bias. I-b FAILED: undeclared integer steps 508 to 224 on ird_t1 and 213 to 146 on ird_tr, 0 on the
  microscope twins, and the fractional bar missed. With the receiver floor off, ird_tr passes I-b. The `merge_split` row LANDED
  default off on a cut fixture built for it: a split piece's new root is pinned at its carried bias, so the frame does not step (433 to 47
  undeclared integer steps on `ird_ms`, 0 against a control; 1 360 to 733 on `ird_msr`), and a merge is a declared cut release of the side the
  votes absorb. I-b FAILED. Section 9, at the author's word: the receiver floor is a zero-mean prior on a bias holding integer path sums. It moves T by
  metres (predicted with nothing fitted, at 3 m and 10 m before the arms ran) and the DCB one for one, and the undeclared satellite steps are wet-delay differences. `ird_tr`
  passes I-b at sigma 3 m, 10 m or off. Neither golden reaches it; an archived field arm with the chain on reads up to 29 m. D0 and D-N are reported to the author.
- [PIV](PIV_pivot_election_coverage_2026-10-17.md): open, 2026-10-18: the clock pivot elected by predicted station coverage, broadcast health and shadow (`network.pivot_election: coverage`, with `pivot_election_horizon_s` and `pivot_election_step_s`); runs at a constellation's first epoch and at a forced handover only, ranks the registered healthy satellites by the roster stations the SP3 orbit product predicts above the mask over the horizon and passes over one that enters the Earth's umbra in it, keeps a registered healthy incumbent and holds a pick no emitted product has adopted yet (`ConstellationState::elected_pivot`); built default off, never armed; unit gate and Release smoke in; offline cross-check on DOY 129 at the run start: GPS G22 27.6 and G14 27.5 stations on average first, Galileo E04, G01 passed over as unhealthy; the 6 h check (section 8, 2026-10-18) holds on G0 (knob absent identical to main except the declared lines, G5 gate 23 arms, user side 16 arms, real time), G1 (field G22 and E04, sim G32 and E36, no handover against two in each control), G3 (field hour-2 common drift 0.051 against 0.192 m/h), G4 and G5 (held-out consumer and 16-site census unchanged at the median) and within 2.4 mm on G2 (clock product, marginally not "no worse"); the sum-to-zero competitor is graded for the record; the forced-handover branch was not exercised. 2026-10-28 to 2026-10-30 (sections 9 to 11): the forced-handover branch runs on a 24 h day; ARHT00ATA's change is a response, not noise (card NSP), and over five 6 h days (DOY 128 to 132) +7.6, -5.8, +40.6, +3.0 and +25.9 mm: past the 1 cm clause on two, recurring but not systematic; no other site worse; not armed.
- [SMX](SMX_scenario_matrix_2026-10-24.md): open, 2026-10-24: the standing fault-scenario matrix (18 rows on the sim golden's three clock-level postures, event minus null against bars frozen before the run; four injector shapes built default off). Version 1 run: 22 of 55 cells pass; the satellite clock step and the slip storm fail on every posture, the clock drift on the golden only, a 1 m station step on the self-contained regime; identity and control gates pass. Nothing armed.
- [NRF](NRF_network_ring_fixing_2026-11-14.md): open, 2026-11-14: integer fixing in the network, ring by ring from
  the chain's root, on the simulator (the author's instruction of 2026-11-14 lifts the do-not-build verdict for this
  work). Pre-registered: chord integers decided by the gated rounding of card IRDU 12.4 (b) behind a damage gate, held
  in the solve by the oracle pin's row, released at every event with the variance re-opened, audited against truth;
  stages N1 (bed without re-seats), N2 (r3c and event beds), N3 (6 h bed); serves IRD-5, IRD-8 and card ARL's rungs.
- [NETVIEW](../cards/tools/NETVIEW-network-view.md): open, 2026-11-15: an interactive page of one network run, made on request from its traces (`scripts/netview/netview.py build`): the tree of each signal as rings of hops, held and float links, each link's whole numbers and float distances with sigma, satellites' correction-level jumps, receivers' wet delay and clock, an event timeline and summaries; `arm` writes a config copy with the traces on, `compare` links two pages in time. No C++ change; branch `viz/network-view`.
- [DRF](DRF_docs_record_folder_2026-11-09.md): open, 2026-11-09: the docs working set and one record folder: `scripts/docs_record.py` moves closed cards and uncited documents into `docs/record/` (old layout mirrored, links re-pointed, nothing rewritten); 145 of 602 files stay; rehearsed on branch `docs/record-folder`, every docs check passes; landing and the rule text wait for the author.
- [BER](BER_bad_epoch_response_2026-11-15.md): open, 2026-11-15: the network's response to a bad epoch, on the
  author's order (the network detects a bad epoch but does not know how to respond). Stage E1: one flagged L1 slip of
  288 cycles on the simulator, then the same event observed at MRO100AUS on DOY 144; bars on wet delays, emitted values,
  emission, fixing and default identity frozen by the requesting session before the runs. Built default off:
  `network.amb_float_drop_jump_k` (the drop of a slipped arc covers the band's measured step) and
  `network.zwd_event_inflate_m` (left off).
- [CHA](CHA_chain_audit_2026-11-18.md): open, 2026-11-18: an audit of the chain and the ring fix with no engine code (the audit card NRF section 112.3 (d) set aside, ruled needed by the author): the map of every function in epoch order, the keys classified as a station's arc, a satellite band's state or both, the rules as one table (seven with more than one owner), the defects in priority order (the held entry's step decided in three places; a re-host pinned through one link; a kept re-host entering the tree without the entry check; the record behind a trace key), a refactor plan of eleven byte-identical steps and what it enables (a family's members keeping their own arc names); the control reproduces at 2fdfc29 and 72b7b3b.
- [CFG](CFG_asked_vs_in_effect_2026-11-18.md): open, 2026-11-18: An audit of every configuration key, asked against in effect: four items differ from the file today at the held-out station (in process and in replay), three keys in every reference configuration were never read, and neither loader checks unknown keys. A plan of message-first steps follows, with its coverage narrowed by the corrections.
- [CRT](CRT_run_start_table_2026-11-18.md): open, 2026-11-18: A design for one record per configuration decision and a run-start table printing, per key and engine, the value asked beside the value in effect, with an opt-in strict mode and the network loader split into twelve concern files. Design only; the measurements hold after the check against the code, with eight statements corrected.
- [NCQ](NCQ_near_constant_quantities_2026-11-18.md): open, 2026-11-18: one table of every near-constant quantity of the network engine with its stiffness, response, unit test and arming, the untested cells ranked by risk, the valve table re-dated and a plan in eight steps; only the satellite clock among satellite-side quantities is tested against the data with a loosening response, and no detector acts on a slow ramp or a constant per-link code offset.
- [CSC](CSC_correction_stream_contract_2026-11-18.md): open, 2026-11-18: The correction stream read as a contract, record by record, with the rules of the two integer flags and three discontinuity counters and every mismatch between writer and reader ranked by its cost to a user. A held user integer is released only by a counter step, a hold-generation change or a generation-id change, never by a flag falling, and under the phase-bias hold the user ignores every phase counter step.
- [UFA](UFA_user_fixing_audit_2026-11-18.md): open, 2026-11-18: An audit of user integer fixing (decision rules, holds and their release, defects by priority, a split plan and unit-test targets); the observed-data users run with partial fixing off, and the self-confirming hold (P1), the code-only Galileo wide-lane admission (P2) and the lost release lists of every failed attempt (P3) stand after the check against the code.
- [CGL](CGL_causal_group_delay_latch_2026-11-18.md): open, 2026-11-18: A design, default off, for holding each satellite's broadcast group delay from the records available by transmission time at its first use, with an inventory of every read past the served epoch. The held value has to extend the GD line that step S7 (b) already saves in the chain memory section; nothing was built or run.
- [P3L](P3L_level_at_link_end_2026-11-18.md): open, 2026-11-18: A design study of the host choice at a released re-host (the host nearest the circular medians of a satellite's converged links, orphans only, integer target, no extra move) that concludes no build now. A build waits on the five-day read of the adopted single Galileo network and a simulator bed that produces the case.
- [SDM](SDM_slip_detector_map_2026-11-18.md): open, 2026-11-18: a map of the slip detectors by tracking type and signal pair at the network and the real-time user, with the ways a held integer pair goes unwatched or unacted (E1 to E7, U1 to U5); no gap, MW or IF-TD verdict on the held pair is a release input and non-zero LLI bits other than bit 0 blind the network MW, so a held-pair row read directly by the release, behind one default-off key, is proposed.
- [URE](URE_user_reaction_edges_2026-11-18.md): open, 2026-11-18: The 28 user-engine reactions to correction-stream events with both edges, the S-record tokens from writer to reader, gaps by user harm and nineteen default-off fix steps; the generation change is the least instrumented event and AR eligibility follows the BIA file, not the stream. The users of the 2026-11-18 runs fix integers (ar_mode 1), so the integer gaps concern users in use.
- [UHT](UHT_user_hold_trace_2026-11-18.md): open, 2026-11-18: Design of a default-off, per-decision trace of the user engine's integer fixing to explain the three short wrong holds of NRF section 118.1; nothing is built. After the check against the code the three leads stand (mode-1 hold is the hold_variance write-back, at least five silent skips of the hold check, the integer search's runner-up discarded), while the key-off identity, column count and column naming need revision.
- [NSF](NSF_native_signal_family_2026-11-18.md): open, 2026-11-18: A design memo, with nothing built, for the native Galileo phase family (members keep their own signal names, one satellite phase bias and code bias per band) and a modelled code column per (satellite, receiver make), Trimble first, separable through receivers without a member column. Order, scope, datum form, noise and the user side are left to the author.
- [JNM](JNM_joint_network_mathematics_2026-11-18.md): reference, 2026-11-18: The rank defects, S-basis and estimable and integer combinations of one network over several systems, bands and channels, checked by an exact rank computation, against what the network code fixes. Nothing hard holds the receiver geometry-free direction (one per receiver and system), only a 10 m DCB prior and carries; ten statements on scope and wording are corrected in the last section.
- [NSM](NSM_network_setup_phase_map_2026-11-18.md): open, 2026-11-18: A phase map of Application::networkSetup_ with the locals that fix the cut order, the fallbacks, the window-dependent reads and a nine-step identical-output split plan (not carried out). The coverage gates are tallied over the parse span, not the processing window, and count the held-out station; seven decisions are put to the author.
- [HDR](HDR_header_fanout_2026-11-18.md): open, 2026-11-18: Include fan-out of the config and run-state headers with a split design and an include-only step list; today's rebuild counts reproduce on the include graph, every count after a change is a simulation on that graph (no build). Corrected: the ring-fix, closed-loop and slip-QC structs live inside NetworkConfig, and the counter-leaf step costs more than a verbatim move.
- [CNN](CNN_counted_not_named_2026-11-18.md): open, 2026-11-18: A ranked list of 30 report lines of the network and user engines that count fallbacks, drops or skips without naming the entity or cause, each with a log-only fix, and a plan of eight log-only steps; checked against the code, two details corrected and the per-signal Galileo headline not shown on the adopted configuration.
- [BRV](BRV_branch_since_main_review_2026-11-18.md): open, 2026-11-18: A review of the 68 code, test and build files changed from e5a7e44 to 7fb0737, with three HIGH findings (restored pin lives against a cold arc ledger; one-epoch gap evidence letting a held integer cross an unmeasured break through a non-returning cut or a kept re-host; a held link without a chord record released only by the monitor). The saved-state keys are not ready to grade, and eight decisions are put to the author.
- [SCW2](SCW2_satellite_code_level_watch_2026-11-18.md): open, 2026-11-18: design of a default-off watch on one satellite's code column (signed 2 h pooled code-residual mean, pooled Melbourne-Wubbena confirmation, bounded response by column type). At the adopted threshold the 2 h detectable step is at least about 23 cm at 10 stations and 13 cm at 30 under correlated multipath, and the confirmation rarely reaches its bar, so the monitor comes first and the response waits on an inventory of free columns.
- [EVL](EVL_event_library_2026-11-18.md): open, 2026-11-18: Eight injected events on the simulator bed pre-registered against one simulator control and one ring-fixing control, with ten 2 h runs listed and no run made. Several expected responses are corrected in the last section, and no event may run before those corrections are worked into the pre-registrations.
- [XBD](XBD_extra_band_datum_2026-11-19.md): open, 2026-11-19: The datum of a third frequency band, derived and proven by exact rank: one DCB row per (receiver, constellation) at every receiver and one level row per component of each extra code signal complete it as a pure datum. The receivers' decimetre third-band drift is estimable, not a datum defect; the keys repair the refolded receiver DCB prior, and the receiver direction at the chain roots (both on dual frequency too). network.rx_gf_datum built default off (network.extra_band_level_datum withdrawn: no extra-band graph splits on the weighted rows); L5 SEQUENCED.
- [XRS](XRS_exact_resume_2026-11-19.md): open, 2026-11-19: decision paper for rule change 12: what an exact warm resume must carry (the wind-up accumulator, the slip detectors' history, the epoch clocks continued), its size (an estimated 21 kB per station for the discrete state) and options A, B and C; recommendation: A in stages, the chain keys off until it lands, B no safe interim.
- [CPW](CPW_single_station_code_pin_2026-11-19.md): open, 2026-11-19: the single-station code pin. A satellite whose pinned code pair loses one signal to a single station is pinned by an all-ones row at target 0, so its surviving code column walks toward zero (17 to 30 cm on Galileo E14, E18, E32; 1 to 2 m on the simulator) and the IF code stays off after the return. Key `network.code_pin_single_leg_hold` (second form b22ce97: the parked signal's single-station row keeps its satellite code bias) passes its bar on DOY 146 and 147 and is ARMED in both reference files (5207fdc, 2026-11-20). Owed: the simulator's one-station trigger, the degraded declaration. Filed 2026-11-20 from the package records.
- [PSM](../cards/perf/PSM_perf_scaling_300_2026-11-19.md): open, 2026-11-19: scaling model of the network engine to 300 stations at 1 s (cost per stage tied to code at e7bc098, calibrated on loaded runs, extrapolated on three borders and two postures): at 1 s every case breaks, leveling, the chain chord read-outs and assembly share first place, 1 s by optimization alone stays closed; next: the stage profiler, then the chord read-out package, gates written before measurement. Filed 2026-11-20 from the package draft.
- [CRP](CRP_config_rulings_paper_2026-11-19.md): open, 2026-11-19: decision paper for eleven configuration and user-posture rulings (unread keys, the ionosphere request with no supply, the derived user pins, the signal rule, the float reference user, the .BIA coverage flag and others); ten can be settled with no engine run. The rulings are the author's. Filed 2026-11-21 from the package.
- [SRP](SRP_stream_rulings_paper_2026-11-19.md): open, 2026-11-19: decision paper for six rulings on the correction stream (one owner per flag rule, `wide_lane_integer`, the phase-bias hold with the ring fix, the file contract, `cmp=`, the format owner; CSC decisions 1, 2, 3, 7, 8, 10): the users follow the network's counters only through `network.serialize_disc_counters`. The rulings are the author's. Filed 2026-11-21 from the package (part 2: the contract body and the independent check).
- [THI](../cards/tooling/THI_threshold_inventory_2026-11-19.md): open, 2026-11-19: inventory of 622 fixed thresholds in the decision paths (328 live, 70 % with no source, 122 live and absolute; 45 live integer rows, 26 with no source), each with the error class its statistic can see and the data-driven form; second stage not released. Filed 2026-11-21 from the package.
- [URS](../cards/user-ar/URS-USER-RESOLVER-STAGES.md): open, 2026-11-19: the user resolver split into named stages at identical output; steps 1 and 2 in main (af91bf5, merged 2026-11-20), steps 3 to 6 and the reference switching drafted, paused until the UFQ work lands. Filed 2026-11-21 from the package.
- [QHO](../cards/user-ar/QHO-QUIN-HEIGHT-OFFSET.md): parked, 2026-11-21: the height offset of the user at QUIN00USA. No defect in the user's path: at one elevation mask the user equals standard PPP; the reported +3.43 against +0.05 cm compared 15 with 10 degrees. QUIN carries a receiver-side term that changes sign with elevation (antenna pattern or near-field reflections under the snow radome, GPS-only calibration); next step: residuals against elevation compared with KOKB00USA.
- [IOF](../cards/user-ar/IOF-USER-IONOSPHERE-FROM-NETWORK.md): open, 2026-11-21: the WTZZ00DEU user with the network's slant ionosphere read from the network run's trace (stage 1), as between-satellite pseudo-observations before the integer search, links with network integers only. Ionosphere-fixed from the two receivers at the site (sigma 0.2 to 0.6 cm), ionosphere-weighted from a plane over nine regional receivers (2.8 to 5.6 cm), both measured on the five poster networks. Bar pre-registered: no wrong integer on three pairs and sooner to 10 and 5 cm on each of five days. No result yet.

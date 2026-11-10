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
- [SCR](SCR_sat_clock_step_response_2026-10-26.md): open, 2026-10-26: builds card SCK decisions 1 and 3, default off. A satellite flagged by the IF-TD pool at most of its stations with one magnitude keeps its arcs and has its clock anchor tripped and re-solved; a tripped clock opens a phase-bias hold generation so the consumer refloats the satellite. Pre-registered bars: SMX row sat_clk passes on g, c and n, no other cell changes verdict, knobs-off identity with smx1.
- [PES](PES_parallel_epoch_stages_2026-10-29.md): open, 2026-10-29: BUILT, GATED and ARMED, merged into main 2026-11-02 (branch `perf/pes`).
  The station normal formation, the Schur reduce (split by border columns, not by stations) and the redundancy census run
  in parallel behind `network.parallel_station_normals`, `parallel_schur_columns` and `parallel_redundancy_census`
  (loader default off; ARMED and pinned in both goldens at the author's decision): key on IDENTICAL to off on both
  goldens for each key and all three, off IDENTICAL to main; loop -27.9 % (field), -20.3 % (sim), -39.7 % at 305
  stations after card CVF. The baseline pair stands. Continued 2026-11-06, branch `perf/pes-next`, ARMED 2026-11-07 (section 9):
  `network.parallel_carry_harvest`, `parallel_iono_traces` and `parallel_coupling_prepare` (each IDENTICAL to off, off IDENTICAL to main), PASS A rejected on its timing, one OpenMP team size in every region. Loop -28.9 % (field), -25.7 % (sim) against main; all six stages -48.7 % against none at 305 stations over 2 h.
  2026-11-07: a fourth key, `network.sbasis_values_first` (the S-basis completion on eigenvalues first, the rank gate reading
  them), identical; main against all four keys -41.5 % (field), -34.8 % (sim).
- [DRF](DRF_docs_record_folder_2026-11-09.md): open, 2026-11-09: the docs working set and one record folder: `scripts/docs_record.py` moves closed cards and uncited documents into `docs/record/` (old layout mirrored, links re-pointed, nothing rewritten); 145 of 602 files stay; rehearsed on branch `docs/record-folder`, every docs check passes; landing and the rule text wait for the author.

# IRM: the integer-fixing configuration made the main reference (2026-11-19)

**STATUS**

- **id:** IRM
- **topic:** config-ledger
- **status:** open
- **summary:** The author's ruling of 2026-11-19 ("Yes, make it main") applied key by key through the ledger and the generator, in form B. The configuration of the five-day read (the eq34 chain as the phase datum, network integer fixing ring by ring, the single Galileo network, the bad-epoch response, exact epoch time, GPS band 1 on L1C first; the held-basis datum and the phase-bias hold off) becomes `golden_field_refound` and `golden_sim_refound`; the earlier posture stays generated as `golden_field_rf0hv18` and `golden_sim_rf0hv18`; the BeiDou lineage is re-based on the frozen field file and does not change. 66 ledger rows changed, two posture log rows, no loader default. Pre-registered: a day health check (10-epoch smokes, three 2 h runs) and a night 6 h set. The baseline is owed until the night set has run.
- **source:** the author's ruling of 2026-11-19 on the question "Make the integer-fixing setup the main reference configuration?"; the analyst's inventory `D:/GnssProcessorSupport/gates/day_program_2026-11-19/adoption/ADOPTION_inventory.md` (checked row by row against the ledger, the generator and the record); the five-day read `D:/GnssProcessorSupport/gates/nrf_reads_2026-11-18/NRF_five_days.md`; the configuration `D:/GnssProcessorSupport/runs_nom/_rap_ad_n147/arm.json`; branch `adopt/integer-reference-2026-11-19`, cut from `int/2026-11-19` at `cf983dc`.

**Outcome.** Pre-registered 2026-11-19. The primary reference files take the integer-fixing configuration of the five-day read, key for key apart from run settings and two measure-only traces (the near-null monitor and the live record); the earlier posture is kept generated, equal in content to the files of that morning. Every new value comes through a ledger row with a three-part note and the manifest; no loader default moves, so no snapshot fingerprint moves, and the new files are fresh convergence only. The configuration had never run on these two beds: the health check below decides whether the change stands.

## 1. What changes

The ruling covers the whole posture, under the author's rule of 2026-11-07 as restated on 2026-11-15 and 2026-11-19
(an improvement that passes its bars is adopted with a three-part note in its ledger row; measured harm stays off with
the reason; never a blanket flip of loader defaults). The configuration is the one the five-day read ran
(`_rap_ad_n143` to `_rap_ad_n147`), against the reference field file of `int/2026-11-19`:

- **The chain datum.** `network.phase_datum_eq34` with the pair family, the six event rows (kept, returning, rehost,
  root, slip, merge_split), the carry shift, the receiver phase bias in the station block with the rank floor at 0
  (decision D0), the complete re-base and the chain trace; the fresh-pin softener, the seed round, its re-host frame
  and the time-linked pin pinned at their defaults (off).
- **The ring fix** (card NRF). Gated rule, odds gate, certain order, held-first re-attachment, wide-lane bound 0.25
  cycle, 12 decisions per epoch, keep-carry re-host, entry re-validation, the pairs E:L1C/L5Q and G:L1C/L2W, the
  settled flags of the largest tied set after 10 emissions, unsettled steps declared on held and unheld signals, the
  step-class declaration of four pairs, and the measure-only near-null monitor (not in the five-day file; products
  identical, card NRF section 113.1).
- **The single Galileo network.** `network.phase_families` E:L1X>L1C and E:L5X>L5Q with the member codes.
- **The bad-epoch response** (card BER). A slipped band's drop covers its measured step, the free gap cover, the
  wet-delay inflation of 0.02 m.
- **Time and signals.** `processing.exact_epoch_time`; GPS band 1 on L1C before L1W.
- **Slip layer, stream and traces.** The GF arc-jump detector at k 6, the discontinuity counters on the C/B/P records,
  the wind-up arc trace, the suspect-epoch window of 10.
- **Off with the chain.** The held basis with its cold release, cold mark and continuous handover; the prioritized
  datum; the anchor carry re-reference; the gate drop erase (minimum stations back to 1); the phase-bias hold with its
  phase-clock transfer, trip generation and the consumer's presence flip. Card IRDU stage B: the stream keeps an
  integer frame only with the chain on and the hold off, since the hold freezes the cold estimate.
- **Simulator only.** The ring fix's truth audit (`network.ring_fix.truth_root`, the fixture's own truth, measure-only).

The held-out consumer of each reference stays float and undifferenced: its type is one of the six items the
inventory leaves to the author.

## 2. Form B

The two entries of the morning are renamed in the manifest to `golden_sim_rf0hv18` and `golden_field_rf0hv18` with
their base, pins, unset list and exceptions unchanged, so their generated files equal the files of that morning but for
the output path and the provenance comment. They stay generated and verified while open work compares against
`_rf0hv18`. The primary entries `golden_sim_refound` and `golden_field_refound` take the frozen output as their base and
pin the change of section 1; their `serialize_disc_counters` exception is dropped (armed). `golden_field_bds_refound`
extends `golden_field_rf0hv18.json` in place of the field golden, so it does not change: BeiDou has no ring-fix pair, and
under `unsettled_steps_unheld` every BeiDou phase counter would step at each emission; BeiDou under the chain waits for
the author's ruling.

## 3. The ledger and the loader's posture rows

- To ARMED (pinned in the reference files): `phase_datum_eq34` (from KEEP_OFF), `phase_datum_eq34_family`,
  `phase_datum_eq34_event_rows`, `phase_datum_eq34_carry_shift`, `phase_datum_eq34_rpb_in_block`,
  `carry_rebase_complete` (from KEEP_OFF), `ring_fix.enable`, `ring_fix.rule`, `ring_fix.gate`, `ring_fix.order`,
  `ring_fix.held_first`, `ring_fix.wl_frac_max`, `cycle_slip.gf_arc_jump`.
- `trace_windup_arcs` from UNVALIDATED to TRACE. PARAM and TRACE rows pinned with a value: the rank floor 0,
  `gf_arc_jump_k` 6, `trace_chain_datum`, `suspect_epochs` 10, `ring_fix.nn_monitor`.
- The 19 RECOMMEND_SCOPED rows of the ring fix's regime keep verdict and regime and take the dated note;
  `serialize_disc_counters` likewise.
- The rows the chain turns off keep their verdict (no measured harm: the chain replaces them) with a note that names
  the frozen lineages where they stay armed. `network.emit_pb_hold`'s regime gains `not on("network.phase_datum_eq34")`;
  `network.ambiguity_vehicle`'s armed test accepts the float vehicle with the chain. The two matching rows of
  `src/core/config/GnssConfigNetworkPosture.cpp` change the run-start POSTURE lines only (log-only).
- Stay off, the reason recorded in the row: `ring_fix.nn_guard`, `ring_fix.nn_guard_release`, `resume_chain_restore`,
  `state_snapshot_chain_capture`, `resume_chain_dump`, `resume_slip_warmup_ep`, `phase_datum_eq34_family_pairs`,
  `rx_extra_band_delay_q`, and `ring_fix.held_first_by_life` (left to the author).
- Not touched: the user-side rows (the users' fixing rules wait for the author's ruling on the derived user posture,
  plan item 7; the receiver-satellite code bias family, `extra_signals`, `primary_bands`, `wl_branch_persist_s`).

## 4. Health check (pre-registered 2026-11-19, before any run)

Release engine of the branch, run as a renamed copy, through the machine's slot queue; field DOY 129 of 2024 (58
stations), sim from 00:30:15 (`network.start_offset_hours` 0.504167).

Day:

1. 10-epoch smokes of the new field file, the new sim file and the BeiDou lineage. Read: exit 0, no refusal at load, the
   POSTURE block (the counts the offline check gives: field ON 24 of 24 and SCOPED 72 of 73 armed, the one gap the
   continuity exception), the ring-fix and chain arming lines, every epoch emitted.
2. Three 2 h runs: the new field file; the new sim file with the truth audit; the frozen field file on the same engine
   (control). Read: every epoch emitted (240 of 240), the POSTURE block, the ring fix's run-end counters (decisions and
   held link-epochs non-zero) and the chain's events non-zero, the held-out user by hour against the control at metre
   level only (hours 1 and 2 are ignition), and on the sim the wrong held link-epochs against truth (bar: 0) and the
   held share by family.

Stop rule: a crash, a refusal, an epoch not emitted, the held-out user's hourly median above 1 m in hour 2 where the
control is below 0.2 m, or any wrong held link-epoch on the sim stops the work: the observation is reported and nothing
further is changed.

Night (specified, not run by day): `D:/GnssProcessorSupport/gates/day_program_2026-11-19/adoption/NIGHT_health_set.md`.

## 5. Results

Results are appended below under dated headings.

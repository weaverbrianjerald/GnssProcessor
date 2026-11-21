# IRM: the integer-fixing configuration made the main reference (2026-11-19)

**STATUS**

- **id:** IRM
- **topic:** config-ledger
- **status:** open
- **summary:** The author's ruling of 2026-11-19 ("Yes, make it main") applied key by key through the ledger and the generator, in form B. The configuration of the five-day read (the eq34 chain as the phase datum, network integer fixing ring by ring, the single Galileo network, the bad-epoch response, exact epoch time, GPS band 1 on L1C first; the held-basis datum and the phase-bias hold off) becomes `golden_field_refound` and `golden_sim_refound`; the earlier posture stays generated as `golden_field_rf0hv18` and `golden_sim_rf0hv18`; the BeiDou lineage is re-based on the frozen field file and does not change. 66 ledger rows changed, two posture log rows, no loader default. Day health check 2026-11-19 (engine f50e95d27304f1fb): three smokes and three 2 h runs exit 0 with every epoch emitted; the sim truth audit 0 wrong of 95 810 held link-epochs; held share in hour 2 field 76 % and 72 %, sim 92 % and 89 % (Galileo, GPS); the built-in float user metre-level in hour 1 (field 2.30 m, control 0.058 m), below the stop bar. By the author's ruling of that evening ('Yes, fixing user') the built-in user of the two reference files is the fixing user of the adopted days (user keys only; network products byte-identical; 6 h observed data 2.31 0.09 0.01 0.03 0.04 0.01 m, simulator 0.68 0.10 0.03 0.03 0.02 0.02 m by hour). The baseline is owed until the night 6 h set has run.
- **source:** the author's ruling of 2026-11-19 on the question "Make the integer-fixing setup the main reference configuration?"; the analyst's inventory `D:/GnssProcessorSupport/gates/day_program_2026-11-19/adoption/ADOPTION_inventory.md` (checked row by row against the ledger, the generator and the record); the five-day read `D:/GnssProcessorSupport/gates/nrf_reads_2026-11-18/NRF_five_days.md`; the configuration `D:/GnssProcessorSupport/runs_nom/_rap_ad_n147/arm.json`; branch `adopt/integer-reference-2026-11-19`, cut from `int/2026-11-19` at `cf983dc`.

**Outcome.** Pre-registered 2026-11-19. The primary reference files take the integer-fixing configuration of the five-day read, key for key apart from run settings and two measure-only traces (the near-null monitor and the live record); the earlier posture is kept generated, equal in content to the files of that morning. Every new value comes through a ledger row with a three-part note and the manifest; no loader default moves, so no snapshot fingerprint moves, and the new files are fresh convergence only. The configuration had never run on these two beds; the day health check passed its bars (every epoch emitted, no wrong hold on the simulator), with the built-in float user metre-level in hour 1 as the item to look at; that evening the built-in user became the fixing user, by the author's ruling.

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

### 2026-11-19: the day health check

Engine `D:/GnssProcessorSupport/worktrees/adopt1119/build/x64-Release/gnss_processor_irm.exe`, sha256[:16]
`f50e95d27304f1fb` (branch tip `6569e55`); runs under `D:/GnssProcessorSupport/runs_irm/`; full tables in
`D:/GnssProcessorSupport/gates/day_program_2026-11-19/adoption/CHECKPOINT_2.md`.

Smokes (10 epochs nominal): the field file (`_rap_irm_f_smk`), the sim file (`_rap_irm_s_smk`) and the BeiDou file
(`_rap_irm_b_smk`) exit 0 and emit every epoch (9, 9 and 10) with no error line and no refusal. The POSTURE tallies equal
the offline check: field ON 24 of 24 and SCOPED 72 of 73, sim 24 of 24 and 68 of 70, BeiDou 25 of 25 and 53 of 55, the
gaps being the documented exceptions. The ring fix and the chain arm as configured on the field and sim files; the
BeiDou file runs the float posture with the phase-bias hold, unchanged.

2 h runs: all exit 0 and emit 240 of 240 epochs, none withheld.

| run | user 3D median, hour 1 / hour 2 | ring fix | held share in hour 2 (Galileo, GPS) |
|---|---|---|---|
| field, new file (`_rap_irm_f129_2`) | 2.297 / 0.320 m | 694 decided, 552 held at the end, 62 100 held link-epochs | 76.0 %, 71.6 % |
| field, frozen file (`_rap_irmc_f129_2`, control) | 0.058 / 0.013 m | not armed | |
| sim, new file (`_rap_irm_s_2`) | 0.793 / 0.298 m | 857 decided, 675 held at the end, 95 810 held link-epochs | 91.8 %, 88.7 % |

The control equals the `_rf0hv18` baseline's first two hours to the millimetre. Sim truth audit: held link-epochs right
95 810, WRONG 0; decisions WRONG 0 (the bar). The chain's frame census shows no fractional realized step on either bed.

The stop rule is not met: no crash, no refusal, every epoch emitted, the user below 1 m in hour 2, no wrong hold on the
sim. Look here: the built-in user is float and undifferenced, and this posture runs the phase-bias hold off, so it is
metre-level in hour 1 (field 2.30 m, rising to 2.65 m at 20 to 30 min and below 0.5 m from 70 min; the same
configuration's built-in user read 0.65 to 1.08 m in hour 1 on the five-day days, DOY 143 to 147 of 2026). The
reference's user numbers are therefore not comparable with `_rf0hv18`, and the built-in user's type is the author's
decision. The baseline stays owed until the night set.

### 2026-11-19 (evening): the built-in user becomes the fixing user

The author's ruling of 2026-11-19 (about 20:50), in answer to the question "Make the new reference's built-in user the
fixing user, configured like the adopted days' users?": "Yes, fixing user". It settles the look-here item of the day
health check above: with the phase-bias hold off, the float undifferenced user of the reference was metre-level in
hour 1 and decimetre-level after.

**The change.** On `golden_field_refound` and `golden_sim_refound` only, through the manifest, the ledger and the
generator, the user keys of the adopted days' users (`runs_nom/_rap_nw_obsadx_146_utqi/arm.json`):

- `network.closed_loop`: `ar_mode` 1, `consumer_undifferenced` false (the differenced user), `ar_wl_only` false,
  `pb_hold_presence_flip` true;
- `network.require_signal_integer_for_ar` true;
- the `estimation.ambiguity_resolution` object of that file: `mode` 1, `elevation_mask` 20 degrees,
  `enable_partial_ar` false, `min_epochs` 10, `ratio_threshold` 5 (`ratio_upper_bound` 10 000),
  `max_position_sigma_m` 0.05, `hold_variance` 1e-7, `hold_release_cyc` 0.25, `nl_iono_free_row`,
  `nl_sigma_floor_cyc` 0.04, `wl_from_float`, `wl_branch_check`, `refloat_partner_signal`;
- `estimation.cycle_slip.lli_slip_mask` 3.

The frozen lineages and the BeiDou lineage are not regenerated and do not change. Ledger: 11 rows take the dated
three-part note. `require_signal_integer_for_ar` and the five user fixing rows (`wl_from_float`, `wl_branch_check`,
`refloat_partner_signal`, `nl_sigma_floor_cyc`, `hold_release_cyc`) go from UNVALIDATED to ARMED; `ar_mode` and
`ar_wl_only` stay PARAM with the new values; `consumer_undifferenced` keeps ARMED (false in the reference, true in the
frozen lineages and BeiDou); `pb_hold_presence_flip` and `lli_slip_mask` keep RECOMMEND_SCOPED and their regimes
(the presence flip is outside its regime as written, since the hold is off, so the posture check does not require
it). Nine keys of `estimation.ambiguity_resolution` have no ledger row (`mode`, `elevation_mask`,
`enable_partial_ar`, `min_epochs`, `ratio_threshold`, `ratio_upper_bound`, `max_position_sigma_m`, `hold_variance`,
`nl_iono_free_row`) and are pinned through the manifest only. No loader default and no engine source changes.

**The 6 h runs of the new files.** Each regenerated file equals the `_cx` arm of its bed in every key but the run
settings (flattened key-by-key comparison: `network.product_output_path` on both beds, and on the simulator
`network.start_offset_hours` 0.504167, the documented grading start). So `runs_irm/_rap_irm_f129_6_cx` and
`runs_irm/_rap_irm_s_6_cx` (engine `gnss_processor_irm.exe`, sha256[:16] `f50e95d27304f1fb`, run that night) are the
6 h runs of the new files:

| run | built-in user 3D median, hours 1 to 6 (m) | epochs with an accepted fix |
|---|---|---|
| observed DOY 129, new file (`_rap_irm_f129_6_cx`) | 2.31 0.09 0.01 0.03 0.04 0.01 | 17 of 720 |
| observed, float user (`_rap_irm_f129_6`) | 2.30 0.32 0.41 0.35 0.24 0.06 | |
| observed, frozen control (`_rap_irmc_f129_6`) | 0.06 0.01 0.02 0.01 0.02 0.01 | |
| simulator, new file (`_rap_irm_s_6_cx`) | 0.68 0.10 0.03 0.03 0.02 0.02 | 22 of 720 |
| simulator, float user (`_rap_irm_s_6`) | 0.79 0.30 0.12 0.08 0.10 0.02 | |
| simulator, frozen control (`_rap_irmc_s_6`) | 0.10 0.03 0.04 0.02 0.02 0.01 | |

The network products are byte-identical to the float-user runs on both beds: the change is user side only. The
user's estimation keys alone change nothing (`_rap_irm_f129_6_uf` and `_ux`, identical to the digit), because the
built-in user is configured under `network.closed_loop`. Scores and product hashes as read by the managing session;
the fix counts from `closed_loop_trace.csv` (`ar_success`).

**Checks of the regenerated files.** 10-epoch smokes through the slot queue, same engine, arms made with
`rapid_arm.py make --hours 0.075` (the sim with `network.start_offset_hours` 0.504167): `_rap_irmu_f_smk` and
`_rap_irmu_s_smk` exit 0 and emit 9 of 9 epochs each, no error line, POSTURE ON 24 of 24 and SCOPED 72 of 73 (field)
and 68 of 70 (sim), the same tallies as the day check; the user starts in AR mode 1 with `wl_only` off and runs its
search on every epoch (no fix in 9 epochs, as expected with `min_epochs` 10). `make_golden_config.py --verify`
(five lineages, 0 failures), `ledger_render.py --check`, `config_posture_check.py --verify-loader` (0 coverage gaps),
`tests/support/gen_config_dump.py --check` and `docs_record.py verify` pass. Debug `gnss_tests` (unchanged since
`15275de`; the files are read at run time), the suites of the 68 test files that name the reference files (90 suites, 87 of them in
`gnss_tests`; the other 3 belong to the simulator's self-test): 805 cases in 87 suites, 798 passed, 7 skipped, 0 failed.

**What stays open.** Hours 1 and 2 are still slow with the network and the user starting cold together (observed
data, hour 1: 2.31 m against the frozen control's 0.06 m); no truth audit of the user's own holds exists on the
simulator; wrong user holds were seen at receivers outside the network on the adopted days; one day per bed so far.
The baseline paragraph of `CLAUDE.md` follows the night's remaining health runs.

### 2026-11-20: the 6 h baseline of the reference files

Runs `D:/GnssProcessorSupport/runs_irm/_rap_n20_{f129,f130,f132,s,b}_6` (6 h; the reference files of 8109a64 with the
fixing built-in user; exe `gnss_processor_int1120.exe`, sha256[:16] de17cb83802a20c2) against same-exe frozen controls
of `golden_{field,sim}_rf0hv18` (`_rap_n20c_*`); census users `_rap_n20_<doy>_<site>`; grades
`D:/GnssProcessorSupport/runs_nom/_irdu_grades/reads_2026-11-20/n20_raw.txt` (read again for this section). Every run
exit 0 with 720 epochs emitted.

| run | built-in user 3D median by hour 1 to 6 (m) | frozen control |
| --- | --- | --- |
| field DOY 129 (WTZZ00DEU) | 2.31 0.09 0.01 0.03 0.04 0.01 | 0.06 0.01 0.02 0.01 0.02 0.01 |
| field DOY 130 | 2.06 0.11 0.02 0.04 0.04 0.04 | 0.09 0.02 0.03 0.04 0.06 0.05 |
| field DOY 132 | 0.60 0.05 0.03 0.03 0.02 0.01 | 0.07 0.02 0.02 0.02 0.02 0.01 |
| sim (SZZ000SIM) | 0.68 0.10 0.03 0.03 0.02 0.02 | 0.10 0.03 0.04 0.02 0.02 0.01 |
| BeiDou lineage | 0.03 0.02 0.02 0.02 0.02 0.02 | (frozen base) |

Simulator truth audit: 406,237 held link-epochs right, 0 wrong, 0 wrong decisions. Held share from hour 3: DOY 129
GPS 80.7 to 84.0 %, Galileo 81.3 to 86.7 %; DOY 130 GPS 80.2 to 81.3 %, Galileo 69.9 to 79.9 %; DOY 132 GPS 76.3 to
79.9 %, Galileo 73.6 to 82.6 %; simulator 89 to 92 %. Census of 16 sites, hours 2 to 6 paired against the frozen
control: +1.4 cm (DOY 129; 1 site worse by more than 5 cm), +1.9 cm (DOY 130; 3 sites), +1.4 cm (DOY 132; 2 sites);
hour 1 +0.50 to +0.61 m. Reading: from hour 3 the reference equals the frozen one at the built-in user; hours 1 and 2
are slower (network and user start cold together). DOY 132's hour 2 (1.35 m with the earlier float built-in user)
reads 0.05 m. The two keys armed on 2026-11-20 (`network.ring_fix.resume_response`, `network.code_pin_single_leg_hold`)
leave these numbers unchanged where no event occurs: `runs_irm/_rap_la4_{f129,s}_6` and `_rap_cp1_{f129,s}_6` are
identical in products and every trace to `_rap_n20_{f129,s}_6`. This is the baseline paragraph of `CLAUDE.md` from
2026-11-20 (branch `docs/consolidate-2026-11-20`, for the author's review). Still owed: the DOY 130 Galileo held share,
the warm-network user start.

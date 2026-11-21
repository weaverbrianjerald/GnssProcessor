# Stream rulings: flag owner, file contract, wide-lane flag, hold, cmp=, format owner (2026-11-19)

**STATUS**

- **id:** SRP
- **topic:** stream
- **status:** open
- **summary:** A paper that prepares six of the author's rulings on the correction stream (CSC decisions 1, 2, 3, 7, 8 and 10, with night report items 15 and 10): one owner per flag rule, `wide_lane_integer`, the phase-bias hold with the ring fix, the file contract, `cmp=`, one owner of the written format and the bias between the two Galileo signal types. Each section gives the ruling asked for, the options, the evidence on disk, the cost, the check against invariants 1 and 7 of CLAUDE.md and a recommendation for the ruling. Nothing is adopted, built or run by it.
- **source:** package stream-rulings-paper of the day program 2026-11-19 (`D:/GnssProcessorSupport/gates/day_program_2026-11-19/program.json`); six parts under `parts/` (flag-counter-evidence, replay-disagreement-classes, arming-scan, writer-code-costs, reader-code-costs, galileo-type-bias-evidence) and the independent check `SKEPTIC_CHECK_2026-11-19.md`; CHAIN `docs/refound/CSC_correction_stream_contract_2026-11-18.md`. Code read at CHAIN e7bc098 (`D:/GnssProcessorSupport/worktrees/chain`, branch chain/audit), MAIN 5ddc422 (`C:/GnssProcessor`, committed content) and rx_sat_bias 8ca1a20 (the tree of the users' exe `gnss_processor_rscb6.exe`, sha256[:16] a578a32f4d447c74). Run folders under `D:/GnssProcessorSupport/runs_nom/`: network `_rap_ad_n143` to `_rap_ad_n147`, `_rap_adr_n146`, `_rap_n46c_n146`, `_rap_n46cr_n146`; users `_rap_nw_obsad{x,f}_14[3-7]_<site>` (210) and `_rap_nw_obsn46{x,f}_146_<site>` (42). No engine was run.

**Status line.** A paper for the author's rulings. Nothing is adopted, built or run by it. Every recommendation below is a recommendation for a ruling, not a decision.

**Outcome.** Every user of the five-day read replays the network's product file, and the replay's own counter recompute disagrees with the file at 33 to 41 % of the epochs of each day. The users follow the network's counters only because the adopted configuration arms `network.serialize_disc_counters`, a key that is off by default and excluded from the reference configurations. On the adopted files no flag ever falls without a counter step, `wide_lane_integer` is always 1, member records always equal their identities, and no configuration combines the hold with the settled flags. The recommendations: one emitter owner for both flags and the step, retire `wide_lane_integer`, refuse the hold with the settled flags, serialize the counters, write `cmp=` behind the IRD-7 key, one reference page for the format, and declared copies for the Galileo members.

The numbers are tables under `tables/` (R = `D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/stream-rulings-paper/`), each regenerated from the run folders by `python scripts/make_all_tables.py`. Code claims carry path:line and tree; the ids in brackets (P01 to P80, W, U) are rows of `tables/code_citations.csv`, where `scripts/make_code_citations.py` checks each paper citation at its tree. "(inferred)" marks what no single line or number shows.

Regeneration check (2026-11-19, `scripts/make_all_tables.log`): the flag table is identical to the part's, the independent check's stream counts are identical, the Galileo verdict, group and post hoc tables are identical, the counter-stripped copy regenerates with the same sha256, and every citation P01 to P80 holds at its tree. Only the arming scan moves, because new run folders appear during the day; its regenerated counts are quoted with the part's.

## 0. The finding, stated plainly

1. **Every user replays the file.** All 210 users of the five-day read point `network.closed_loop.replay_products_path` at their day's `_rap_ad_n<doy>/products.txt` (210 of 210 by JSON load and by plain text, SKEPTIC claim 1). Each replays every record from the file's first epoch (first trace epoch equal to the file's first E epoch in 210 of 210; `tables/replay_emulation_vs_stdout.csv`, column users_starting_at_first_epoch). The replay loop is rx_sat_bias 8ca1a20 `src/core/app/ApplicationConsumerReplay.cpp:519-527` [P54].
2. **The replay's recompute disagrees with the file at about a third of the epochs.** Epochs with an RTA-4 warning, out of 2880 per day:

   | DOY | 143 | 144 | 145 | 146 | 147 | five days |
   |---|---|---|---|---|---|---|
   | disagreeing epochs | 959 | 1153 | 1088 | 1181 | 1026 | 5407 of 14 400 |
   | share | 33.3 % | 40.0 % | 37.8 % | 41.0 % | 35.6 % | 37.5 % |

   Every user of a day logs the same count (42 of 42 per day, `tables/replay_emulation_vs_stdout.csv`). A Python emulation of the users' replay reproduces every logged epoch, count and first key at 100 % on all five days (bar written first: 99 %; same table). Every disagreement is on the phase channel. On DOY 146, 15 891 of 16 183 disagreeing values (98.2 %) are steps of the ring fix's unsettled-step rule and their member copies, which the users' replay emitter does not have (`tables/replay_disagreement_by_class.csv`).
3. **The users follow the network's counters only through `network.serialize_disc_counters`.** The reader compares and adopts only when the file carries counters (rx_sat_bias 8ca1a20 `NetworkProductIO.cpp:911`, adoption `:946` [P55, P56]; the same rule at CHAIN e7bc098 and MAIN 5ddc422 `NetworkProductIO.cpp:920`, `:946-956` [P26, P27]). The key is true in all 8 network runs read. Its loader default is false (CHAIN e7bc098 `src/core/config/GnssConfigLoadNetwork.cpp:11780-11782` [P38]); service mode forces it on (`:11928-11929` [P39]); the three reference configurations leave it out by a manifest exception (`config/golden/golden_manifest.json:200, 329, 355` [P44 to P46]); its ledger row is RECOMMEND_SCOPED (`docs/CONFIG_promotion_ledger.json:676` [P47]).
4. **What the numbers carry about the users, and what they do not.** With the file's counters, 82.6 to 87.2 % of the users' `reset_disc` events fall at a (tow, satellite, signal) where the file stepped and a counter-less replay would not (`tables/user_reaction_by_class.csv`, column file_only_share_of_reset_disc_pct). The numbers show that the users follow the network's counters. They do not show that the users are right: F6 of `D:/GnssProcessorSupport/gates/nrf_reads_2026-11-18/NRF_five_days.md` records wrong holds at combined-mode users on four of five days (24, 10, 58 and 16 epochs; pilot-type users hold none). What a user would do without the counters is inferred. The night run of section 8 (the 21 users of DOY 146 on the counter-stripped copy `scratch/replay-disagreement-classes/products_n146_nocounters.txt`) measures it.

## 1. Facts every section uses

- **The adopted stream (8 files).** Flags and counters per day: `tables/flag_events_by_day.csv`.
  - A flag never falls without a counter step in the same emission: 0 falls without a step in 8 of 8 files, gaps, vanishes and returns included (SKEPTIC claim 11). All six signals fall 48, 60, 54, 48, 48 times on DOY 143 to 147 (50 on adr; column all_signal_falls), against 339 908 to 343 740 held key-epochs whose previous emission had the flag up (column held_prev_si1).
  - On the five root days a held flag falls only when a satellite returns after a gap (40 to 50 falls per day, all after a gap of 6 to 116 E blocks). Each WTZR00DEU run adds 2 consecutive falls.
  - A flagged key can leave the stream while flagged: 48 to 60 per root day (column vanish_flagged). It falls only at its return, with a step. A user sees the key vanish first.
  - Every rise is consecutive and carries a step. While a held signal or its partner is unflagged, both counters step at every emission (13 704 of 13 704 key-epochs on DOY 146), equal to the run-end count of `stdout.txt` to the unit.
  - `wide_lane_integer` is 1 on every P record of every file (column P_records; SKEPTIC claim 15).
  - Member records (E L1X, L5X, C1X, C5X) equal their identities in bias, flag and counter in 100 % of pairs, and appear exactly when their identities appear (columns P_member_neq, B_member_neq, member_without_identity).
  - No datum_changed epoch, no counter decrease and no continuity-guard key in any file; no record carries `gen=` (the token appears only in the legend) and the S line carries no `pbhold`. Record kinds: S, H, E, C, B, P, U, R; no G, I or N; no token on any C, B or P record (`tables/record_inventory_by_day.csv`).
- **The engine that wrote the files** is the 91f36cf build `gnss_processor_cha_fp1.exe`. Five later commits touch the flag or stream code (8d71321, 12ecc70, 5e033d8, 7fb0737, 1de8d40); none changes a stream writer, a counter rule or the text format, and the stream files are unchanged from 7fb0737 to e7bc098 (SKEPTIC claims 20 to 22). 1de8d40 moved the settled-set rule into CHAIN e7bc098 `src/core/network/ar/RingFixDecide.cpp:832-924` [P40] and is declared identical on its own controls only; 12ecc70 can change the settled set where a keep refusal fires, not measured on the five-day configuration.
- **What releases a held user integer** (CSC correction 2, MAIN 5ddc422): a phase counter step on the plain path (`src/core/realtime/RealTimeEngineSsrBreaks.cpp:303-312` [P62]), a hold-generation change (`:686-687` [P64]) or a generation-id change (`:189-198` [P60]). A flag falling only removes the ambiguity from the next search (`src/core/filter/AmbiguityResolver.cpp:432` [P68]); the held value stays in x and P (`:3564` [P67]) until `resetAmbiguity` (`src/core/filter/StateManager.cpp:501-505` [P66]).
- **The users' keys** (`tables/arming_scan_users.csv`): 160 of 210 five-day users arm `network.require_signal_integer_for_ar` (all 105 fixing users, 55 of 105 float users; the 50 others are the same ten sites each day); 0 arm `network.closed_loop.ar_observe`; 0 arm carry or translate; 599 148 reset lines, 0 hold lines and 0 generation-change lines over the 210 stdout files (SKEPTIC claim 39).

## D1. One owner per flag rule (CSC decision 1; night item 15)

**Ruling asked for.** Which single place guarantees that `signal_integer` never falls without the counter step that releases a user.

**IRD section 11 increment.** IRD-7 (emission and declarations) for D1(a) and D1(b); IRD-8 (the user proof) for D1(c).

**Options.**
- **D1(a), a single emitter function.** One function sets both flags and emits the step that must accompany every fall; the other seven writers (CSC 2.1) propose values to it.
- **D1(b), an emission-time invariant.** After the member copy, a key whose flag fell since its previous emission and whose counter did not step is stepped and counted.
- **D1(c), a user-side release on a 1 to 0 edge** (URE F12, key `network.closed_loop.signal_integer_release_hold`, default false): the user resets the ambiguity when the flag of a key falls.

**Evidence on disk.**
- `tables/flag_events_by_day.csv` (runs `_rap_ad_n143` to `_rap_ad_n147`, `_rap_adr_n146`, `_rap_n46c_n146`, `_rap_n46cr_n146`): 0 falls without a step in 8 of 8 files; 48 to 60 flagged keys vanish per root day and fall only at their return.
- The writers at CHAIN e7bc098: the default `signal_integer = (system != GLONASS)` (`src/core/network/solve/Estimator.cpp:1576` [P01]); the settled-flags stamp (`src/core/network/ar/RingFix.cpp:77-78` [P02, P03], called at `src/core/network/emit/SolPrep.cpp:1371-1372` [P04]); the member copy in the solution (`src/core/network/emit/EmitStore.cpp:60-79` [P05]) and in the emitter (`src/core/products/NetworkProductEmitter.cpp:984`, `:985`, `:987` [P16 to P18]); the guard (`:126-158`, `:160-165` [P19, P20]). The step that today accompanies every fall comes from a different rule, the unsettled steps (`NetworkProductEmitter.cpp:652-678` [P09]), behind `network.ring_fix.unsettled_steps`.
- The gap the step rule leaves: the loader requires the settled flags for the unsettled steps but not the reverse (`src/core/config/GnssConfigLoadNetwork.cpp:1579-1583` [P36]; CSC A2). 4 network runs carry the settled flags without the unsettled steps, all older trials (`_rap_f10_n144`, `_rap_f11_n144`, `_rap_f12_n144`, `_rap_f12_n145`; `tables/arming_scan_networks.csv`).
- User side: 258 one-to-zero edges over the five days, all with a step (part reader-code-costs); the URE F12 key and its counters do not exist in either tree (two searches, part reader-code-costs).

**Cost** (`tables/option_costs.csv`).

| option | files | existing tests touched | new tests | output move, adopted / reference | ledger, fingerprint |
|---|---|---|---|---|---|
| D1(a) | 8 (CHAIN) | 15 cases (RingFixSettledFlags 4, UnsettledSteps 4, ContinuityGuard 6, phase family 1) plus 9 StreamFlagRoundTrip cases on the unmerged branch tests/network-2026-11-18 | 4 | 0 on the 8 files / 0 | 0 new rows, dated notes on ledger rows 52 to 55; chain memory section version possibly bumped (inferred) |
| D1(b) | 2 to 3 (CHAIN) | none break | 3 | 0 forced steps expected (inferred; proof is a zero count on a run) / 0 | 1 row and 1 token if keyed, else 0 |
| D1(c) | 7 (MAIN) plus a ledger row | branch case level_ephemeris :420 stays green; skip :471 `ASignalIntegerEdgeHasNoCounter` (T15) turns green | 3 to 4 (T15, T16, return edge, config) | 0 for the 210 users with the key on or off | 1 row |

The zero output move of D1(a) and D1(b) holds on the eight files (no fall lacks a step); for other configurations it is inferred until a night run counts forced steps (SKEPTIC recommendation).

**Invariants 1 and 7.**
- D1(a) and D1(b) instrument the decoupling edge (the fall, then the step). The re-coupling edge is the settling step (`NetworkProductEmitter.cpp:652-678` [P09]). Both give the user's held integer a release at every fall, also when the unsettled steps are off (closes CSC A2 by construction).
- D1(c) counts the user's decoupling edge; its re-coupling edge is a fresh float. On today's streams it changes nothing, and it fires only at a key's return, never at its vanishing (section 1).

**Recommendation for the ruling.** D1(a), with the D1(b) count built into the owner function (a fall it had to step is counted on a run-end line), so the rule "a fall implies a step" is structural and measured. D1(c) is not recommended as the owner: it duplicates a release the stream already carries, and it would see the fall late. URE F12's measure-only counters (`signal_integer_drops`, `signal_integer_returns`) remain useful instrumentation. CSC decision 4 (settled flags imply the unsettled steps) becomes unnecessary under D1(a).

**Proceeds before the ruling.** The evidence and the contract body; a test that pins today's behaviour (a fall steps with the unsettled steps on and does not with them off, CSC section 7 test 1 written to today's rule); URE F12's counters, default off.

## D2. `wide_lane_integer` (CSC decision 2)

**Ruling asked for.** Keep `wide_lane_integer` with a reader and a definition, or retire it.

**IRD section 11 increment.** IRD-7 (emission and declarations).

**Options.**
- **D2(a), keep it:** give it a meaning, a user reader in MAIN, a range check in the text reader, and a rule that lowers it on a guard trip.
- **D2(b), retire it:** write 0 and drop the stamp line `src/core/network/ar/RingFix.cpp:78` (CHAIN e7bc098 [P03]).

**Evidence on disk.**
- 1 on every P record of all 8 files (`tables/flag_events_by_day.csv`, column P_records; 526 278 records on DOY 146).
- No engine reader in either tree: three searches (identifier grep at both commits, member-access grep, alias grep `wide_?lane_?int`, `wl_int`; parts writer-code-costs and reader-code-costs, SKEPTIC claim 25). Analysis scripts read it (CSC correction 6).
- Three writers, all copying the held-family stamp: the stamp [P03], the member copy (`EmitStore.cpp:60-79` [P05]), the store record (`NetworkProductEmitter.cpp:952-962` [P15]). The ring-fix meaning ("held family") differs from the IGS-SSR meaning of the entry (CSC section 5).

**Cost** (`tables/option_costs.csv`).
- D2(a): 3 network files plus a user reader in MAIN (not costed); the 0 to 3 range check breaks three -2 round trips (`tests/network/test_network_product_io.cpp:202, 1673, 1722`); 2 to 3 new tests; no output move unless the meaning changes.
- D2(b): 5 files and 1 script (`RingFix.cpp:78`, the log text in `RingFixSeams.cpp`, `GnssConfigTypes.hpp:1947-1950`, `RingFix.hpp`, the legend note; `scripts/nrf_settled_flags.py`); 1 existing case and the branch's stamp and guard cases rewritten; 1 new test. Output move: entry 6 of every P record moves 1 to 0 under the adopted configuration (526 278 of 526 278 lines on DOY 146); 0 under the reference configurations (already 0). No ledger row, no fingerprint token.

**Invariants 1 and 7.** Neither option touches an edge today. A kept entry with a reader becomes a new decoupling input and needs its own step on every fall (invariant 1); D2(a) must include that rule.

**Recommendation for the ruling.** D2(b), retire it, landed in the same change as D1(a) so the owner function sets only `signal_integer`. The entry carries no information on the adopted stream and has no reader; keeping it would add a second flag that needs its own release rule.

**Proceeds before the ruling.** The contract body states the entry's as-built meaning (1 for every held-family signal, settled or not). Nothing else.

## D3. The phase-bias hold with the ring fix (CSC decision 3)

**Ruling asked for.** How the configuration prevents a ring-fix stream from running with `network.emit_pb_hold`, under which a user ignores every phase counter step at a held key (CSC A1).

**IRD section 11 increment.** IRD-7 (the hold questions H1 and H2 of the emission plan) for D3(a); IRD-8 (the user proof) for D3(b).

**Options.**
- **D3(a), loader refusal** of `network.emit_pb_hold` with `network.ring_fix.settled_flags`, and the posture check's scoped-gap entry for the hold removed under the ring fix.
- **D3(b), the user hold path honours a counter step** on a key whose flag is down, and the posture entry is removed as in (a).

**Evidence on disk.**
- No configuration carries the combination today: 0 of 1738 network runs at the part's scan (10:10), 0 of 1753 at the independent check and 0 of 1788 at the last regeneration of the tables (0 of 404 in the newest 1500 folders), by JSON and by text (`tables/arming_scan_networks.csv`, row TOTAL; SKEPTIC claim 29). The hold and the settled flags live in separate configurations: the reference configurations arm the hold (713 runs with both hold keys at the last regeneration) and no ring fix; the adopted form arms the ring fix and serialized counters and leaves the hold off (218 runs).
- The posture check reads the adopted configuration's scoped tier as 72 of 74, the two gaps being `network.emit_pb_hold` and `network.trace_dd_fractional_ref` (CHAIN e7bc098 `docs/refound/CHA_chain_audit_2026-11-18_part3.md:102` [P43]); its hold row has no ring-fix exclusion (`src/core/config/GnssConfigNetworkPosture.cpp:825-828` [P42]). So the tooling recommends the hold for the ring-fix configuration.
- The mechanism: the user claims a held key only when `hold_generation != 0` (MAIN 5ddc422 `RealTimeEngineSsrBreaks.cpp:677` [P63]), skips the plain path for it (`:301-302` [P61]) and resets only on a generation change (`:686-687` [P64]).
- The users' copies carry `network.emit_pb_hold` true (210 of 210) while replaying streams written without the hold; in a replay user the key sets loader state and log lines only (part reader-code-costs, MAIN `GnssConfigLoadNetwork.cpp:2878-2900`, `ApplicationNetwork.cpp:203-208`). No record of the five files carries `gen=`.

**Cost** (`tables/option_costs.csv`).
- D3(a): 3 files (`GnssConfigLoadNetwork.cpp` rule after both parses, `GnssConfigNetworkPosture.cpp:825-826` regime, ledger row 175 regime and its rendered view); no existing test arms both keys; 2 new tests; 0 product move; run-start posture lines move (scoped tier count); 1 ledger row; 0 fingerprint tokens. CHA part3:102 takes an appended dated note (the record is not rewritten). File count is a design estimate (SKEPTIC, unconfirmed).
- D3(b): 2 MAIN files (6 behind a key); `test_pb_emit_hold.cpp` (9 cases, its closure count changes) and `test_pb_hold_presence.cpp` (4 cases); 2 to 3 new tests; 0 output move for the 210 users (no `gen=` on any record).

**Invariants 1 and 7.**
- D3(a) restores invariant 1 by exclusion: the posture in which every counter step at a held key is ignored can no longer be configured. It instruments no edge; it removes one that cannot be honoured.
- D3(b) keeps the combination and makes the hold re-couple: the integrity steps at a held key reach the reset again (invariant 7, the re-coupling edge of the hold).

**Recommendation for the ruling.** D3(a) now: it is cheap, it fails closed, it moves no product, and it removes a recommendation that contradicts the mechanism. D3(b) only if a held stream with settled flags is wanted later (for example under IRD-7 H2, the hold by chain generations), where it would be costed with that design.

**Proceeds before the ruling.** Nothing needs to: the arming scan shows no configuration at risk. The posture note can be drafted.

## D7. The file contract: setter rules as S keys, or the counters always serialized (CSC decision 7)

**Ruling asked for.** How a file user obtains the counters the network emits: by S keys that let the replay emitter apply the setter-armed rules, or by serializing the counters outside service mode.

**IRD section 11 increment.** IRD-7: its gate includes "a Q5 replay reproduces the disc counters and generations byte for byte" (CHAIN e7bc098 `docs/refound/IRD_integer_recovering_datum_2026-10-13.md:185`).

**Options.**
- **D7(a), S keys:** tokens for the step class, the members, the unsettled steps, the unheld form and the guard, read by `makeReplayEmitter`.
- **D7(b), always serialize:** `network.serialize_disc_counters` armed in the reference configurations through the manifest (the manifest exception removed), loader default kept off as CLAUDE.md requires; or the key deleted with a tombstone ("always").

**Evidence on disk.**
- Section 0 items 2 and 3: 33 to 41 % of epochs disagree on a full-file replay (`tables/replay_emulation_vs_stdout.csv`), 98.2 % of the DOY 146 values from the unsettled-step rule and its member copies; the remainder is 269 replay-only values in 12 epochs where the network's step-class verdict suppressed the value test, 11 file-only values in 3 epochs no rule in the file explains (release marks, inferred), and their member copies (`tables/replay_disagreement_by_class.csv`). Clock, code, datum mark, retro mark and plain value test: 0 on all seven files.
- The cause in code: `makeReplayEmitter` forwards constructor parameters only (CHAIN e7bc098 `NetworkProductIO.cpp:1132-1144` [P28]; seven at rx_sat_bias 8ca1a20 `:1118-1129` [P57]); the setter rules are armed only at network setup (`src/core/network/driver/ApplicationNetworkSetup.cpp:1582, 1600, 1603, 1610, 1614-1616` [P29 to P33]); the setup comment that every construction parameter travels in the contract holds for the constructor only (`:1570-1571` [P34]). `setPhaseFamilyMembers` does not exist at MAIN 5ddc422 (SKEPTIC claim 44).
- Release increments come from the chain record (`EmitStore.cpp:472-473` [P07]) and are not serialized; the file shows 2 to 20 steps per day that no rule carried in the file explains (part replay-disagreement-classes; 11 to 12 per day in the independent check's recount). So S keys alone cannot meet the IRD-7 bar.
- Who depends on the counters: at the part's scan all 1146 user replays in the newest 1500 folders replayed a stream whose P records carry the counter, all 210 five-day users included; at the last regeneration 1092 of 1096 do, the other 4 being recent users (`_rap_rs_*`) on an older stream without settled flags (`_rap_nrf7_hf_g6`). The 7907 replays without counters sit on 313 streams; none of the 285 streams with an arm.json arms the settled flags, and 28 have no arm.json (`tables/arming_scan_users.csv`).
- Without counters the reader neither compares nor warns (rx_sat_bias 8ca1a20 `NetworkProductIO.cpp:911` [P55]), so a file user outside service mode diverges silently (SKEPTIC addition B).

**Cost** (`tables/option_costs.csv`; the network and user rows of D7 share `NetworkProductIO.cpp` and must not be added).
- D7(a): 4 network files (`NetworkProductIO.cpp` S writer, parser, legend, `makeReplayEmitter`, `checkContract`; `NetworkProductIO.hpp` `SbasisContract`; `ApplicationNetworkSetup.cpp` pending contract) and the reader side (2 files at CHAIN, 3 at MAIN, which also lacks `setPhaseFamilyMembers`); existing cases extended (`SbasisContractRoundTripAndCheck`, `RepeatedSLinesUpdateContractPositionally`, `HeaderLegendDeclaresEveryEmittedField`; 51 cases of `test_network_product_io.cpp` re-read); about 5 new tests; output move: the S line only; 0 under the reference configurations; no ledger row, no fingerprint token.
- D7(b): 3 to 4 files (`GnssConfigLoadNetwork.cpp:11780-11782` only if "always", ledger row 676, `config/golden/golden_manifest.json:200, 329, 355`, the posture text); 1 new test; output move: 0 under the adopted configuration; every C, B and P record of the reference configurations gains its counter entry (the C drift written as 0); 1 ledger row; 0 fingerprint tokens (the row says not fingerprinted).

**Invariants 1 and 7.**
- D7(b) carries both edges of every rule to file users through the adopted counters: the decoupling edge (unsettled, guard and release steps) and the re-coupling edge (the settling step).
- D7(a) carries them partly: release marks stay unserialized (CSC A5), so a file user can keep an integer the in-process user would release (inferred).

**Recommendation for the ruling.** D7(b) through the manifest: arm `network.serialize_disc_counters` in the three reference configurations by removing the manifest exception, keep the loader default off, and state in the format text that a file without counters misses the setter-armed rules. D7(a) is not needed for correctness; its only gain would be a recompute check of the network's rules, and it cannot reproduce the release increments. A cheap addition the author may take: one reader warning when a file carries no counters, which ends the silent divergence of addition B.

**Proceeds before the ruling.** CSC section 7 test 3 (file equals store) written to pin today's gap, with and without serialized counters; the night run of section 8, which sizes what a counter-less user loses.

## D8. `cmp=` (CSC decision 8)

**Ruling asked for.** Write the chain component on P records, or drop the component requirement from the user's observe gate.

**IRD section 11 increment.** IRD-7: the emission plan names the key `network.emit_wire_chain_tokens` (`cmp=`, `genc=`) and extends `network.require_signal_integer_for_ar` to amb_datum 2 and equal `cmp=` (CHAIN e7bc098 `docs/refound/IRD_integer_recovering_datum_2026-10-13.md:119, 121`).

**Options.**
- **D8(a), write it** where the chain component is known, behind `network.emit_wire_chain_tokens` (default off).
- **D8(b), drop it** from the observe gate (MAIN 5ddc422 `src/core/realtime/RealTimeEngineArObserve.cpp:121-123` [P72]).

**Evidence on disk.**
- No C++ writer (CHAIN e7bc098 `include/products/NetworkProductEmitter.hpp:88-96` [P52]; a test pins it, `tests/test_network_product_chain_token.cpp:118` [P53]); two scripts write it (`scripts/simnet_truth_wire.py`, `scripts/irdu_hybrid_products.py`), and 10 constructed product files carry it (`tables/pbhold_newest_products.csv`, column n_P_cmp; SKEPTIC claim 26).
- The gate acts only inside the observe search (`RealTimeEngineArObserve.cpp:37` [P71]) and only with the veto key on (CSC correction 1 and 3); cmp comes only from the parsed token (`src/core/realtime/ArObserve.cpp:38` [P73]; parse CHAIN `NetworkProductIO.cpp:264-266` [P22]).
- 0 users arm `ar_observe`; the key is true in 7 network arms only (`tables/arming_scan_users.csv`; SKEPTIC claim 28).

**Cost** (`tables/option_costs.csv`).
- D8(a): 4 to 5 network files (`SolPrep.cpp` stamp from the chain edges, `ChainFrame.hpp:261-274`; the P writer and legend; the header doc; the loader key); 2 existing cases break (`TheWriterNeverEmitsTheComponent`; the branch case `ChainComponentIsReadButNeverWritten`); 3 new tests; output move with the key on: a token on every P record in a component (up to 526 278 lines on DOY 146), off: 0; 1 ledger row; 0 fingerprint tokens.
- D8(b): 1 MAIN file; `test_ar_observe_engine.cpp:553` inverts; 1 new test; 0 output move (no user arms the search).

**Invariants 1 and 7.** `cmp=` carries the re-coupling identity of invariant 7 (which satellites share an integer frame). Under D8(a), a satellite that changes component with its flag up needs a step (inferred; no such step exists today). D8(b) writes no state and admits candidates by observation only, so it touches no edge.

**Recommendation for the ruling.** D8(a), behind the IRD-7 key, sequenced after D1(a) so that the owner function also steps a component change. The IRD-7 plan already names it, and the component is the identity a user needs to pair satellites. D8(b) would remove the only user check of a shared frame.

**Proceeds before the ruling.** Nothing; no user uses the observe search today.

## D10. One owner of the written format, and the Galileo signal-type bias (CSC decision 10; night item 10)

### D10. The format owner

**Ruling asked for.** Which one document owns the written definition of the correction stream (flag meanings, `pbhold` values, member records, the eight replay parameters, the U record).

**IRD section 11 increment.** n/a (documentation); the declarations it writes down serve IRD-7.

**Options.** The legend written in every file (CHAIN e7bc098 `NetworkProductIO.cpp:659-680`), `docs/report/network_wire.tex`, or a new reference page.

**Evidence on disk (gaps per candidate; part writer-code-costs, CSC section 6).**
- Legend: lacks the flag meanings, `pbhold=2` and `3` (it shows `pbhold=1` only, `NetworkProductIO.cpp:675` [P25]; the writer emits 1 to 3, `:603-605` [P23]; the reader accepts 4, `:366-369` [P24]), the member records, the setter rules as a stream property, `cmp=`. It is the only candidate under a test, which checks key names only (`tests/network/test_network_product_io.cpp:1068-1071`).
- `network_wire.tex:730-764`: puts the URA in C, names seven replay parameters (discpv missing), lacks G, U, N, sigma=, sigc=, sigcc=, rho=, isc=, lvl=, iode=, agek=, hsig=, pdr= and the member records.
- Measured occurrence of `pbhold` in the 1500 newest product files at the last regeneration: absent 888, 2 in 428, 3 in 184; 1 and 4 never (`tables/pbhold_newest_products.csv`; the part's scan read 884, 432 and 184). Among the product files of the newest 1500 folders, 41 carry `pbhold=2` (the scout's 39 is not reproducible; the folder set moves).

**Cost.** One page plus the other candidates trimmed to pointers; `HeaderLegendDeclaresEveryEmittedField` and the branch's `StreamLegendTokens` cases if the legend owns it; 0 to 1 new test; no output move unless the legend changes; no ledger row (`tables/option_costs.csv`).

**Invariants 1 and 7.** n/a: documentation instruments no edge.

**Recommendation for the ruling.** A new reference page as the single owner of meanings, seeded from the contract body of this package (`STREAM_CONTRACT_BODY_2026-11-19.md`) once the rulings land; the legend keeps names and allowed values only, and CSC section 7 test 4 asserts that every emitted value is declared; `network_wire.tex` points to the page.

**Proceeds before the ruling.** The contract body (written, ruling-independent).

### Item 10. Whether users receive the bias between the two Galileo signal types

**Ruling asked for.** Whether combined-mode users (E L1X, L5X) keep receiving exact copies of the pilot records, or receive a term or record of their own.

**IRD section 11 increment.** n/a (a systems question; the network term is NSF's design).

**Options.** (a) Copies as today; (b) a network-only member term per NSF (one ionosphere-free code column per satellite and receiver make, CHAIN `docs/refound/NSF_native_signal_family_2026-11-18.md`), whose records still repeat the identity's values, so users see no change; (c) a per-(satellite, band, class) record on the stream.

**Evidence on disk** (`tables/galileo_type_bias_by_satellite.csv`; float users `_rap_nw_obsadf_14[3-7]_<site>`).
- 110 of 210 users (11 of 21 sites) track E L1X/L5X and receive exact copies of L1C, L5Q, C1C and C5Q: value, both flags and counter equal in 100 % of about 86 000 pairs per signal per day (stream check of the part; copy at CHAIN e7bc098 `EmitStore.cpp:60-79` [P05]). Phase never falls back to the other signal type (MAIN 5ddc422 `src/core/products/SsrCorrectionStore.cpp:396-400` [P74]); the code band fallback is off by default (`src/core/config/GnssConfigLoad.cpp:899-900` [P75]) and unset in all 210. No armed user term can absorb a bias between the types (part reader-code-costs).
- The pre-registered code-residual statistic could not be computed: the residual columns are blank because the users run between-satellite rows and the trace skips pair-labelled rows (MAIN `src/core/realtime/RealTimeEngineSsrDiag.cpp:205-207` [P77]; checked in 3 of 210 traces). The same rule was applied to the float users' between-satellite wide-lane fraction (Amendments A and B, written before any fraction was examined; the date of the first section of the pre-registration cannot be shown, SKEPTIC claim 33).
- Pre-registered verdict: one persistent signal-type offset, E12 in the Javad group (called on 5 of 5 days, mean -0.063 cycle, -47 mm). The combined group's E12 was ruled on 4 days and called on 2. Trimble: none on 4 or more days. Expected false persistent calls from the null: about 1e-4 over 30 satellites.
- Post hoc (not pre-registered): combined-minus-pilot difference rms 0.030 cycle over 30 satellites against a receiver-label permutation null of median 0.013 and 95th percentile 0.022 (p < 0.002, 500 permutations); largest E12 -0.092 cycle (69 mm).
- Wrong holds are not traced to it: UTQI E18-E06 (DOY 146) sits at the 81st percentile of UTQI's own pairs, E34-E23 (DOY 143) at the 87th, and the errors were whole wide-lane cycles (inferred that 0.15 to 0.19 cycle offsets do not explain them).

**Cost.** (a) none. (b) the NSF build (network only; no user change). (c) 4 to 5 MAIN files (`SsrCorrectionStore`, the N parse, the declared roster) plus network writer changes, 3 to 4 new tests; 110 users move if received values change, 0 if the rule only reads a declaration (inferred; `tables/option_costs.csv`).

**Invariants 1 and 7.** A member that gains its own term or record needs its own counter (invariant 1); a change of member declaration is an event at both edges (invariant 7). Copies, as today, carry the identity's edges exactly.

**Recommendation for the ruling.** (a) copies, declared on the stream (a member note the reader parses, under the D10 owner), and no term or record yet. The offset is reproducible but small (0.03 cycle rms), no wrong hold is traced to it, and the data cannot tell whether a class is the tracking mode or the receiver make, nor split it by band or into code and phase. (b) and (c) wait for a differenced code-residual trace (night item) and NSF's rank check.

**Proceeds before the ruling.** The NSF rank check; a trace of the users' differenced rows.

## 7. What the paper does not claim

- That the users are right: they follow the network's counters; F6 records wrong holds at combined-mode users on four of five days.
- What a user without counters would hold or where it would be: inferred from the classes; the night run measures it.
- Zero output moves for D1(a) and D1(b) beyond the eight files read.
- That 12ecc70 or 1de8d40 leave the flags of the five-day configuration unchanged (declared identical on their own controls only).
- The cause of the 2 consecutive falls on the WTZR00DEU runs (G03, G14) and the split of the 22 to 105 steps per day on settled held keys.
- Any grade effect of an option: costs are files, tests and output moves read from the code, not measured.
- The size of a phase or code bias between the Galileo signal types beyond the wide-lane fraction.

## 8. Runs a later check would need (none run)

1. **Counter-less users, DOY 146.** The 21 user configurations `_rap_nw_obsadx_146_<site>` (and optionally `_rap_nw_obsadf_146_<site>`), copies with only `network.closed_loop.replay_products_path` changed to `R/scratch/replay-disagreement-classes/products_n146_nocounters.txt` (sha256 2f1abba0086dde1253c7b3beedaed07cb8bb832b5d3f4b7825242416695199cc), 24 h, same exe `gnss_processor_rscb6.exe` (sha256[:16] a578a32f4d447c74), against the existing users on the original `_rap_ad_n146/products.txt`. Compare `reset_disc` counts, held integers and wrong holds (F6 rule). Prediction written before the run: 0 RTA-4 lines; `reset_disc` about 7 400 against 37 964 (combined group) and about 5 800 against 32 281 (pilot group). Sizes D7.
2. **CSC section 8** (sizes the option named): a ring-fix stream with `emit_pb_hold` on, read in process (D3); settled flags without unsettled steps (D1); a file replay against the in-process user outside service mode (D7); users with and without `require_signal_integer_for_ar` on DOY 144 and 145 (the ledger statement behind D1 and D8); a CLK_STEP trace on the ring-fix configuration (CSC A7).
3. **Engine-build check, optional.** `_rap_ad_n146/arm.json` on an e7bc098 Release build, 24 h, products compared with `_rap_ad_n146` on P entries 5 and 7; settles whether 1de8d40 or 12ecc70 move a flag on the adopted configuration.

## 9. Code citations

`tables/code_citations.csv`: 80 paper citations (P01 to P80), each checked at its tree by `scripts/make_code_citations.py` (80 of 80 verified), plus the 58 rows of part writer-code-costs (W) and the 30 rows of part reader-code-costs (U) with their verdicts and the independent check's corrections (member copy lines 984, 985, 987; `cmp=` writers in scripts).

## Decisions for the author

1. **D1, one owner per flag rule.** Recommendation: D1(a), a single emitter function for both flags and the step, with the D1(b) count inside it; URE F12 counters as instrumentation only.
2. **D2, `wide_lane_integer`.** Recommendation: retire it (write 0, drop `RingFix.cpp:78`), in the same change as D1(a).
3. **D3, the hold with the ring fix.** Recommendation: loader refusal plus the posture entry removed; the user-side hold change only with a later held-stream design.
4. **D7, the file contract.** Recommendation: serialize the counters in the reference configurations through the manifest; state the limit in the format text; optionally one reader warning for files without counters.
5. **D8, `cmp=`.** Recommendation: write it behind `network.emit_wire_chain_tokens`, after D1(a).
6. **D10, the format owner.** Recommendation: a new reference page seeded from the contract body; the legend keeps names and values under an extended test.
7. **Item 10, the Galileo signal types.** Recommendation: declared copies; no term or record until a differenced code-residual trace and NSF's rank check exist.

## Not checked

- Every option's file count is a design estimate read from the code (D3 and D7 explicitly unconfirmed by the independent check).
- The residual columns beyond 3 of 210 user traces.
- The date of the first section of the Galileo pre-registration.
- Whether any user code path other than `require_signal_integer_for_ar` and the observe gate reads `signal_integer`.
- Why 50 float users lack the veto key, and whether the derived user configuration's signal-rule pin changes anything for receivers that track both Galileo types.
- The sibling package's replay CSV (`results/stream_replay/`) had not landed as a class table when the parts were written; the classes are this package's own.
- The manager's note for this package was not found as a file; the task text and the program record were used.

## 2026-11-19: corrections from the independent check

An independent check (read-only, no engine) recomputed the numbers from the run folders with scripts written from the
definitions, opened the cited lines at their trees and diffed the team's tables: 44 claims checked, 34 confirmed, 7
corrected, 3 unconfirmed. The corrections win over the text above. Tables and scripts:
`scratch/skeptic/`; record: `SKEPTIC_CHECK_2026-11-19.md`.

1. **"Users are correct."** The numbers show only that the 210 users follow the network's counters (adoption at RX
   8ca1a20 `NetworkProductIO.cpp:946`). F6 of `NRF_five_days.md` records wrong holds at combined-mode users on four of
   five days (24, 10, 58 and 16 epochs). What a user would do without counters is inferred; the night run on
   `products_n146_nocounters.txt` shows it.
2. **Commits between the engine and e7bc098.** Five, not four: 1de8d40 (CHA S10) also touches the flag rules and moved
   the settled-set rule into `RingFixDecide.cpp:832-924`, the lines cited. It is declared identical on its controls,
   not on the five-day configuration. The engine is the 91f36cf build: 12ecc70's log text is absent from the exe and
   from the adopted stdout.
3. **Member copy lines.** CHAIN e7bc098 `NetworkProductEmitter.cpp:984` (counter), `:985` (flag), `:987`
   (`disc_counter_`).
4. **`cmp=` writers.** None in the C++ engine; `scripts/simnet_truth_wire.py` and `scripts/irdu_hybrid_products.py`
   write the token, and 10 constructed product files carry it.
5. **Release increments.** "94 raised by the release alone" (DOY 146 stdout) counts steps that were neither forced nor a
   value jump (`NetworkProductEmitter.cpp:927`); unsettled steps are not excluded. The file shows 11 to 12 such steps
   per day that no rule carried in the file explains. The conclusion that D7(a) alone cannot reproduce the counters
   stands.
6. **Galileo, combined group.** E12 was ruled on 4 days and called on 2, not "2 of 5".
7. **Wording.** "Field" for the observed-data reference configuration is replaced by "observed data"; a record position
   is an "entry".

Unconfirmed: the date of the first section of the Galileo pre-registration (its file was last written after the first
extraction script); the residual columns beyond 3 of 210 traces; the D3 and D7 file counts (estimates; the writer and
reader costs of D7 share `NetworkProductIO.cpp`).

Added, not in the text above: (A) 48 to 60 flagged keys per root day leave the stream while flagged and fall only at
their return, so a release on a 1 to 0 edge fires at the return; (B) without serialized counters the reader writes no
warning (RX `NetworkProductIO.cpp:911`), so the divergence from the network's counters is silent.

Confirmed with own numbers: 210 of 210 users replay their day's file from its first epoch; 959, 1153, 1088, 1181 and
1026 disagreeing epochs, reproduced at 100 % by an independent emulation of the users' tree; 0 flag falls without a
step in 8 of 8 files; `wide_lane_integer` 1 on every P record of 8 files; 0 member records unequal to their identity
(P and B); the unsettled and member counts equal the run-end lines; 0 networks arming the hold with the settled
flags; 160 five-day users with the veto key, 0 with the observe search.

## Filing note (2026-11-21)

Filed on 2026-11-21, after the fact, from the package `stream-rulings-paper` of the day program of 2026-11-19 (`D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/stream-rulings-paper/`: `STREAM_RULINGS_PAPER_2026-11-19.md`, `STREAM_CONTRACT_BODY_2026-11-19.md`, `SKEPTIC_CHECK_2026-11-19.md`); its parts, tables and scripts stay there. The text below is the package text, unchanged apart from heading levels. The id SRP is given at filing. The contract body and the independent check are in [part 2](SRP_stream_rulings_paper_2026-11-19_part2.md). The rulings are still the author's.

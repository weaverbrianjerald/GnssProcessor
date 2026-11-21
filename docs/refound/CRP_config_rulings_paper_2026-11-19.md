# CRP: configuration and user-posture rulings, with the evidence (2026-11-19)

**STATUS**

- **id:** CRP
- **topic:** config-ledger
- **status:** open
- **summary:** Decision paper for eleven configuration and user-posture rulings (unread keys, the ionosphere request with no supply, the derived user pins, the signal rule, the float reference user, the .BIA coverage flag, a strict level, no-reader keys, dependents without their master, ledger rows, log order), each option with its evidence and cost. No engine was run; no run behaviour is claimed.
- **source:** day program 2026-11-19, package config-rulings-paper (four parts and the independent check, `parts/` beside this file); code at CHAIN e7bc098 (branch `chain/audit`), MAIN 5ddc422 (branch `main`) and the users' build 8ca1a20 (worktree `rx_sat_bias`); run folders `_rap_ad_n143` to `_rap_ad_n147`, `_rap_adr_n146`, `_rap_nw_obsad{x,f}_<doy>_<site>` (210), `_rap_nw_obsn46{x,f}_{146,147}_*` (84), `_rap_nw_fxbgwl_14{6,7}_wtzz`; records CFG, CRT, URE, UFA (CHAIN `docs/refound/`), the T17 note, the night report of 2026-11-19 (items 7, 13, 16).

**Outcome.** Ten of the eleven rulings can be settled with no engine run. The three unread keys and the ionosphere request leave the reference configurations through the manifest, in the same regeneration as the adopted posture, and no output moves. The values the derived user configuration replaces are what every graded user ran (300 of 300 graded runs ask at least four replaced values); they should be declared and printed, not changed. The eleventh, the .BIA coverage flag, changes values and needs night replays, as would any change to a pin. One count of the parts was wrong and is corrected (section 11).

## Header

**What was read.** Every JSON file under `config/` of both trees (1 351 per tree; 1 349 parse; 1 272 engine configurations; 1 241 in network mode after `extends`), every `arm.json` of `D:/GnssProcessorSupport/runs_nom/` (scan: 17 210 files, 16 157 distinct; second collection: 16 168 distinct, 14 985 in network mode), the loaders (`src/core/config/GnssConfigLoad.cpp`, written LD, and `GnssConfigLoadNetwork.cpp`, written LN), `Knobs.cpp`, the user engine, the ledger (785 rows at CHAIN, 784 at MAIN), `scripts/loader_keys.py`, `scripts/make_golden_config.py`, the manifest, the logs and traces of the graded users, and `adoption/ADOPTION_inventory.md`.

**Trees.** CHAIN e7bc098 for network code, MAIN 5ddc422 for user-engine code, 8ca1a20 for the build the graded users ran (`gnss_processor_rscb6.exe`, sha256[:16] a578a32f4d447c74; its derivation text equals 8ca1a20 and its branch is an ancestor of 5ddc422). Every line is cited with its tree. The three files `config/test_suite/*.jsonc` are outside the JSON population by its rule.

**Rules before cases.** Each part wrote its classification rule before reading the cases: `scratch/scan/README_rules.md` (three later rules marked with their reason), `scratch/posture/pins.md`, `scratch/rules-code/r8_rule.md` (saved 09:16, counts 10:42), `scratch/ruling-tables/RULES.md` (one marked amendment). Three rule files were last saved after their tables; the marked amendments fit that, but file times cannot prove the unmarked rules came first.

**Method rules followed.** Counts carry their base. Absence is shown by two different searches. Datum and integer quantities are not graded here; nothing is graded from the simulator. Numbers a conclusion rests on are taken from the tables in `tables/`, recomputed from run folders where a document was the source. Integer and chain work: none of the rulings is an increment of record IRD section 11; the derived pins touch the user engine of the held-out station and the replay users only. "(inferred)" marks a claim no single line or number shows.

**What this paper does not claim.** No run behaviour. The effect of the derived pins, of the .BIA flag or of a rebuilt float reference user on user integers needs a paired replay that only a ruling would order (section 10).

## Summary of the rulings

| § | Ruling asked | Recommendation | Outputs that move if adopted |
|---|---|---|---|
| 1 | Remove the three unread keys, retire them, or give them readers? | Unset in the manifest (option i), in the adoption's regeneration | none |
| 2 | `closed_loop.consume_network_iono` true with no supply: unset, pin false, or keep? | Unset (default false equals the ledger row) | one info line, one replay warning, one member-dump line |
| 3 | Are the derived pins the declared posture of graded users? | Yes: declare and print them (a), then name them (b); no value change without a paired replay | (a) log lines; (b) none |
| 3b | The signal-rule difference of the two derivations: which is right? | By design; correct the comment (A) and add a named key, default count (B) | none |
| 3c | Float reference user: keep, `refloat_partner_signal` off, or no resolver? | Keep, labelled "float under the same refloat rule" | none |
| 3d | Arm `bias_coverage_requires_apply` in the user-integer posture? | Arm, only after 29 + 29 replays and one held-out day | 42 phase rows, 2 314 code rows per user group |
| 4 | Strict level: default and known-unread list? | Warn by default now; strict on unknown keys as opt-in after the combined tree | warnings only (strict: 893 of 16 157 distinct runs abort) |
| 4b | Alias column (CRT 1) | Warning period recording `alias_used`, then remove | none (0 of 16 157 runs use an alias) |
| 5 | The 21 no-reader keys and one knob: retire, correct echoes, or give readers? | Correct the echoes; retire the never-set keys; keep the 4 by design | log text |
| 6 | One policy for a dependent set without its master? | Uniform warn from a declared ledger column; ring fix keeps refusing | log text (2 silent pairs in the runs) |
| 7 | Ledger rows for 24 network leaf keys and the two KEEP_OFF bundle rows | Add the rows; SH elevation cutoff to PARAM | none |
| 8 | Does log line order belong to "identical"? | Products, traces, manifests in order; logs ordered outside parallel regions, multiset inside, until the audit lines are station-ordered | none |

**What each section settles.** Night report item 7: section 3 (the first question of the item; the user-fixing defect order is not settled here). Item 13: sections 1, 2 and 4 (the run-start table is owned by package config-in-effect-table). Item 16: section 8. CFG decisions: 1 in §1, 2 in §2, 3 and 11 in §3, 4 in §4, 5 in §5, 6 in §6; 7, 8, 9 and 10 are not settled here (§10 says who needs what). CRT decisions: 1 in §4b, 3 in §3 and §4, 4 in §1, 6 in §3, 7 in §6. UFA decision 7: §3c. URE decision 1: §3d.

## 1. The three unread keys (CFG 1, CRT 4, night report item 13)

**Ruling asked.** Should `network.redundancy_census`, `network.min_stations` and `network.max_duration_s` be removed from the reference configurations, retired, or given readers?

**Evidence that nothing reads them.**
- Search A (the full dotted key) and search B (the quoted leaf and the member forms) at CHAIN, MAIN and the test branch 511bf3e (`scratch/rules-code/r1_search.out`) and the independent check's own git grep: no JSON read of any of the three. `redundancy_census` appears only in the comment CHAIN `Knobs.cpp:586` ("there is no `network.redundancy_census` bool to read") and in `tests/test_block_baarda.cpp:1245`.
- The redundancy check is armed by `k.redun_census_on = nc.redundancy_census_stride > 0 || nc.redundancy_escalate_sigma > 0.0` (CHAIN `Knobs.cpp:519-521`). Its dependents: MDB :534 (also armable from RAIM :532-534), VCF :539, VCF apply :541, block Baarda :588, congruence :606.
- `"max_duration_s"` exists only as the top-level alias of `processing.max_duration_s` (CHAIN LD:582, MAIN LD:583). `jval` looks an alias up at the top level only (CHAIN `GnssConfigJsonHelpers.hpp:127-133`, `root.contains(legacyKey)` at :131), so a key under `network` never matches it. Every reader of the run length goes through `getMaxDuration()`, including the RINEX parse clip (CHAIN `ApplicationNetworkSetup.cpp:295-306`); the comment at CHAIN `scripts/ce_make_arms.py:119` ("only clips the RINEX parse") is wrong.
- `"min_stations"` has 0 hits as a quoted leaf; `network.streaming.min_stations` (CHAIN LN:2220-2221) and `network.parallel_min_stations` (LN:2098-2099, a threading threshold) are other keys.
- None of the three is a term of either snapshot fingerprint.
- The textual key universe misjudges one of them: CHAIN `scripts/loader_keys.py:197-203` tiles the unrelated literals `"network"` (an orbit_source value, CHAIN LD:1152) and `"max_duration_s"` (LD:582) of `loadProcessingSection_` and calls `network.max_duration_s` read (`scratch/rules-code/lk_probe.py`, confirmed by the independent check). A key universe generated from `loader_keys.py` (CRT 4.2) would not flag it. All counts below use the corrected universe.

**Values on disk** (`tables/three_keys.csv`, `tables/r1_counts.csv`).
- `redundancy_census`: set in 273 of 1 241 network config files, all true; set in 14 985 of 14 985 network arms (15 960 folders), all true. 0 files in any population set it false.
- `min_stations`: set in 372 network config files (8 not 10) and in 14 985 of 14 985 arms (all 10). No roster is shorter than its `min_stations` (0 of 372 config files, 0 of 14 974 distinct runs at the scan).
- `network.max_duration_s` differs from `processing.max_duration_s` in 36 of 586 config files that set it, and in 24 of 14 985 network arms (2026-11-13 to 2026-11-19). In each of the 24 the processing value is shorter (300 s or 7200 s against 21 600 or 86 400 s), so those runs ran the shorter length. The scan found 18; the other 6 (`_rap_sfx_{base,fix}_{arht,ascg,utqi}`) were created after its folder list.
- The reference configurations set the three at `config/golden/golden_field_refound.json:229, :367, :373` (CHAIN), the simulator file at :224, :362, :368, the BeiDou file at :236, :374, :380. The six adopted runs carry them (true, 10, 86 400).

**Options and cost** (`tables/ruling_options.csv` rows 1 to 3).
- **(i) Unset in the manifest.** Add the three keys to `goldens[].unset` of the observed-data and simulator entries of `config/golden/golden_manifest.json` (the BeiDou entry inherits). `make_golden_config.py` applies the unset at :117-118 and checks absence at :173-176. Files: 4. Outputs: none (inferred from the two searches). Fingerprints: none. Tests: none fail; TB 511bf3e `test_config_asked_vs_in_effect_network.cpp:572, :595, :609` (`RedundancyCensusIsUnread`, `NetworkMaxDurationIsUnread`, `NetworkMinStationsIsUnread`) add the key themselves (inferred to still pass). Runs: none; a 10-epoch smoke pair is optional. `scripts/loader_keys.py:423` may drop two entries.
- **(ii) Retire through `RetiredConfigKeys`.** Needs (i) first: `tests/test_retired_config_keys.cpp:355-358` (CHAIN) forbids a retired key in the current reference files. Also changes :301 (150 rows), :311-312 (every family is RKD, 2026-10-14) and :326-337 (every row has a flat spelling; these keys never had one); a new family constructor beside `retiredOn20261014` (`RetiredConfigKeys.hpp:70`). Files: 6. Output: one warning per load (network mode only, LN:244 CHAIN) in 273, 372 and 586 config files and in any re-run of the 14 985 arms.
- **(iii) Give them readers.** `redundancy_census` as a master switch with default true moves no file today (0 set it false); default false would turn the check off in every file that omits the key. The loader host checks (CHAIN LN:7556, :7629, :7695, :7857) would need the same predicate. `min_stations` has no meaning left to give. A `network.max_duration_s` reader would change the run length of the 36 config files and 24 arms. Each reader adds a member, a getter, a ledger row and a test; a 2 h identity pair for the redundancy switch.

**Recommendation.** (i), in the step-3 regeneration of the adoption (form B primary entries); the frozen `*_rf0hv18` entries may keep the keys. (ii) later, only if the author wants old files to warn. Correct the comment at `ce_make_arms.py:119` and the tiling rule of `loader_keys.py` in a code session.

## 2. `closed_loop.consume_network_iono` without supply (CFG 2, night report item 13)

**Ruling asked.** The key is true in every reference configuration while the ledger says KEEP_OFF and the network engine builds no supply: unset it, pin it false, or keep it?

**Evidence** (CHAIN, MAIN in parentheses).
- Ledger row KEEP_OFF, default false (`docs/CONFIG_promotion_ledger.json:703`). Parsed at LN:13402 (13170).
- The supply exists only under `EmitStore.cpp:663` (625), which needs `consume_network_products`, `consume_network_iono` and `sh_model`; `sh_model` is built only with `estimate_iono_sh` (`ApplicationNetworkSetup.cpp:642`, 598). The only push into the slant channel is `EmitStore.cpp:811` (773).
- The flag reaches the user engine only at `ApplicationNetworkSetup.cpp:1904` (1778); `setConsumeNetworkIono` has no other caller, so every replay user holds false and warns that the request is inert (`ApplicationConsumerReplay.cpp:139-144`, both trees). The warning appears in 210 of 210 graded users (`main_log.log`, recomputed by the ruling-tables part, `tables/repro.csv`).
- With no slant record, `SsrCorrectionStore::getSlantIono` returns nullopt (MAIN `SsrCorrectionStore.cpp:351-360`), so the prior block of MAIN `RealTimeEngineEstimate.cpp:168-230` adds 0 rows.
- The user-engine fingerprint carries the flag (`route:..:..:iono`, MAIN `RealTimeEngineStateSnap.cpp:96-98`), used only when `realtime.state_snapshot_path` is set; 0 of 219 adopted and user `arm.json` files set it.

**Counts** (`tables/r1_counts.csv`). 308 network config files set it true: 263 also set `estimate_iono_sh` (old SH lineages, supply built) and 45 do not. 14 981 of 14 985 network arms set it true; 0 build the supply.

**Options and cost** (`tables/ruling_options.csv` rows 4 to 6).
- **(a) Unset in the primary manifest entries.** 4 files. Moves the info line (`ApplicationNetworkSetup.cpp:1912` CHAIN), the replay warning and one member-dump line (TB `tests/support/ConfigDumpGenerated.inc:927`). No state, product or trace moves; no test on CHAIN or MAIN names the key.
- **(b) Pin false:** as (a), the pin read back at `make_golden_config.py:169-172`. **(c) Keep:** no cost; the 210 users keep warning.

**Recommendation.** (a), in the same regeneration as section 1. The 263 SH files are left alone in every option.

## 3. The user posture: the derived pins (CFG 3 and 11, CRT 3 and 6, night report item 7)

**Ruling asked.** Are the values the derived user configuration replaces the declared posture of graded users, and how are they shown?

**Who runs the derivation** (`tables/user_pins_summary.csv`, `tables/user_pins_by_run.csv`). P1, the in-process held-out station: 1 741 run folders. P2, replay users: 14 208 folders; replay starts on a non-empty `replay_products_path` alone (MAIN `ApplicationNetwork.cpp:207-208`) and derives at `ApplicationConsumerReplay.cpp:359`. P3, the standalone user: 0 in every population (three searches); nothing in the loader stops one. Graded runs: 300 (210 five-day users, 84 users of NRF section 118, 6 held-out days).

**Graded runs asking a replaced value** (count of 300 unless stated; from `tables/user_pins_by_run.csv`): processing mode 300; signal rule 300; no-bias code weight 300; bias age gate (left unset, armed) 300; second code channel and its scale 300; between-satellite differencing 294 (the replay users; 0 of 6 held-out days). The other modelled pins differ in 0 of 300. `estimate_ifb` (row 21) is not modelled by the scan; it is graded from the code and the Toggles line.

**The pin table** (`tables/pins.csv`, 26 pins and 10 overlays; LN lines; "fp" = in the MAIN user fingerprint, `RealTimeEngineStateSnap.cpp:80-101`).

| # | member (file key) | MAIN 5ddc422 | CHAIN e7bc098 | rscb6 8ca1a20 | verdict | fp |
|---|---|---|---|---|---|---|
| 1 | `processing_mode_` to PPP_RTK | 13497 | 13729 | 13153 | graded, effective | no |
| 2 | satellite clock and phase-bias states off | 13501-13502 | 13733-13734 | 13157-13158 | graded (reversion of the network force) | no |
| 3 | `promote_sat_code_bias_`, network code-bias states off | 13503-13504 | 13735-13736 | 13159-13160 | graded, no reader at the user | no |
| 4, 5 | SH ionosphere off; `estimate_sat_biases_` | 13505-13506, 13518 | 13737-13738, 13750 | 13161-13162, 13174 | latent | no |
| 6 | `no_bia_code_downweight_` 100 (file 1.0) | 13524 | 13756 | 13180 | graded, zero reach | no |
| 7 | `enable_between_satellite_sd_` true at P2 | 13556 / 13535 | 13788 / 13767 | 13212 / 13191 | graded at P2, latent at P1 | no |
| 8 | `ar_wl_only_` | 13541 / 13728 | 13773 / 13960 | 13197 / 13377 | latent | yes |
| 9 | `ar_max_position_sigma_m_` cap | 13546-13547 | 13778-13779 | 13202-13203 | latent | no |
| 10 | `enable_ssr_bias_max_age_` armed when unset | 13580-13582 | 13812-13814 | 13236-13238 | graded, effective | yes |
| 11 | `channel_mode_` | 13622-13623 | 13854-13855 | 13272-13273 | latent | no |
| 12 | `canonical_signal_rule_` to Count | 13684 | 13916 | 13334 | graded, effective | no |
| 13-16 | signal-rule station scope; code follows phase; extra-band code, phase scales | 13690, 13696, 13703, 13704 | 13922, 13928, 13935, 13936 | 13340, 13346, 13353, absent | latent | no |
| 17 | second code channel false, scale 1 | 13711-13712 | 13943-13944 | 13360-13361 | graded, zero reach | no |
| 18 | second phase channel | 13719-13720 | 13951-13952 | 13368-13369 | latent | no |
| 19 | `ar_mode_` from `closed_loop.ar_mode` | 13724 | 13956 | 13373 | latent | yes |
| 20 | `ar_active_systems_` | 13725-13726 | 13957-13958 | 13374-13375 | latent | no |
| 21 | `estimate_ifb_` true, inherited from the W6 force (file false) | 3869 | 3945 | 3642 | graded, effective | yes |
| 22, 23 | W6 `enable_code_phase_bias`; `estimate_phase_clock_` | 3895, 3898 | 3971, 3974 | 3668, 3671 | latent | no |
| 24 | `consume_network_iono` (dead request, §2) | `ApplicationConsumerReplay.cpp:139-144` | | | graded, inert | route bit |
| 25 | P3 profile `estimate_ifb_` | 13742-13743 | 13974-13975 | 13391-13392 | latent (no P3 run) | yes |
| 26 | P3 profile age gate | 13755-13756 | 13987-13988 | 13404-13405 | latent (no P3 run) | yes |

Overlays O1 to O10 (MAIN LN:13587-13675): only `pb_hold_presence_flip` is armed (P2 true, P1 false), inert on a holds-off stream (inferred: 0 `gen=` records in `_rap_ad_n146/products.txt`, `emit_pb_hold*` false in the arm). `drop_unrecorded` and its phase-bias twin do not exist in the rscb6 build.

**What the graded users saw in their logs.**
- Signal rule: `main_log.log` names it ("Canonical latch ... rule=count") in 210 of 210 users (recomputed, `tables/repro.csv`), while `gnss_config.log` prints the file's "priority". The rule is therefore not silent at the latch.
- Age gate: silent, by two searches (the logs of `_rap_nw_obsadx_146_utqi` for max_age, bias age, age gate, STALE; MAIN code for a log statement naming it). The comment at CHAIN LN:13810-13811 cites a configuration echo that does not exist.
- `estimate_ifb`: the Toggles line prints `ifb=on`. The comment at MAIN `RealTimeEngineSsrBreaks.cpp:861` (the closed-loop user inherits `estimate_ifb` true from the network engine) matches the code; CFG decision 11 is answered: the station was graded with the inherited true.
- Differencing at P2: the replay line says "model BSSD" while the base Toggles line says `bssd=off`. It is not in the user fingerprint although it changes the state shape (inferred gap).
- No-bias weight 100: 0 rows reached. `uncovered_rows.py` finds 2 314 of 9 548 423 code rows per user group without a stream code bias, all on (satellite, signal) entries of the loaded .BIA, so none is weighted (`tables/uncovered_rows.csv`); P1: 0 of 536 714.
- Second code channel: its reader (MAIN `PppModel.cpp:1512-1513`) needs the mode row 1 removes.

**Options and cost** (`tables/ruling_options.csv` rows 7 to 11).
- **(a) Print asked beside in effect** in a run-start table. 1 to 2 files, log text only, no fingerprint. The 12 design-pin skips of TB `ConfigAskedVsInEffectDerived` become passes once a line names each key (inferred). A 10-epoch smoke.
- **(b) A named key per pin**, defaulting to today's value. One member, getter and ledger row per pin; output identical; no fingerprint unless added. Tests: TB `test_config_asked_vs_in_effect_derived.cpp:218-505` and `MembersChangedOnTheReferenceConfiguration`. One byte-identity pair (an adx and an adf replay) and a 2 h held-out run.
- **(c) Value changes.** (c1) age gate off when unset: every replay user moves; term `agegate` moves (it matters only with a snapshot path; none today); night: 210 replays, the F6 re-grade, 6 held-out days. (c2) `estimate_ifb` to false: state shape changes, term `ifb` moves, same night cost, comment at `RealTimeEngineSsrBreaks.cpp:861` changes. (c3) weight 1.0: no move today, 2 314 rows per group with §3d.
- **Test gap.** The 52-row `MembersChangedOnTheReferenceConfiguration` compares the loaded configuration with the derived one, so the W6 force of row 21 (file false, loaded true) is invisible to it.

**Recommendation.** Yes, the pins are the posture every graded user ran, and they become declared posture: (a) now in the run-start table, (b) in the code session (one ledger row each), with `estimate_ifb` inherited true and differencing at P2 added to the declared list (CRT 3: designed decisions; CRT 6: each replacement prints a line). No value change (c) without a paired replay ordered by a ruling.

### 3b. The signal-rule difference of the two derivations

**Ruling asked.** The derived configuration pins the signal rule to Count (CHAIN LN:13916) while the standalone profile (`applyNetworkConsumerProfile`, CHAIN LN:13964) does not: which is right?

**Evidence.**
- The pin's own reason (CHAIN LN:13906-13915): the derived configuration is a full copy of the network engine's file, so without the pin an arm of `processing.canonical_signal_rule` would also move the held-out station's latch, a separately graded variable. The test-isolation reading is supported by T17 note lines 94-100 and 131-136 (the test case was deleted in af7c950).
- The comment on `applyNetworkConsumerCouplings` (CHAIN LN:13718-13725) says both paths call it "so ... cannot diverge". Both paths do call it (:13967 and the derivation); only its scope claim overreaches, since every closed-loop pin (signal rule :13916, second code channel :13943-13944, age gate :13812-13814) diverges by construction.
- The "162 → 80 epochs below a decimetre" of the comment comes from CHAIN `docs/record/refound/CPA_canonical_pair_admission_2026-09-20.md:102, 128` (arms `gD_dual`, `kC_consL1C`, 2026-09-20). The run folders are not on disk (two name searches): the number is from the record only.
- Five-day read: with the files' lists (C1C before C1W, C2W before C2L), priority latches the same signal as count in 1 728 of 1 728 band latches (`tables/latch_rule_check.csv`). Only the top two candidates are printed, so a third candidate is not excluded.
- Where the pin did matter: `_rap_nw_fxbgwl_146_wtzz` and `_rap_nw_fxbgwl_147_wtzz` list C1W first and latch `L1C=C1C(13) over C1W(12)` under count, where priority would take C1W/L1W, the family of the L1W stream they replayed (recomputed from `main_log.log` and `arm.json`). That this caused the failed starts of ARN section 14.2 is the record's attribution, not shown here.
- `golden_field_refound.json` at CHAIN lists GPS C1W first; `_rap_ad_n147` lists C1C first (confirmed by the independent check).

**Options and cost** (`tables/ruling_options.csv` rows 12 to 14).
- **(A)** Correct the comments (:13718-13725 and :13810-13811): 1 file, no output.
- **(B)** Named key `network.closed_loop.signal_rule`, default count: 4 to 6 files, identical output, one member and ledger row; TB cases `SignalRuleAskedPriority` (:218), `SignalRuleStationScopeNamingTheHeldOutStation` (:231) and the third SignalRule case assert the key; a 2 h identity pair.
- **(C)** The user takes the file's rule: expected identical on the 216 five-day and held-out runs; different wherever a list puts C1W first (the frozen `*_rf0hv18` lineage, the ARN runs). Two replays and one 2 h held-out run on a C1W-first list.

**Which is right.** The derivation's pin is right for a copy of the network engine's file; the standalone profile is right not to pin, since a standalone user file is its own. The divergence is by design; the comment overstates its scope.

**Recommendation.** (A) now and (B) in the code session. (C) is a separate ruling and must not travel with the regeneration: on the frozen lineage it would move the held-out station.

### 3c. The float reference user (UFA decision 7)

**Ruling asked.** Keep the float reference user with the resolver on, switch `refloat_partner_signal` off for it, or build it with no resolver?

**Evidence** (`tables/partner_releases.csv`, 302 374 epochs per group). Partner refloats: 806 in the fixing users (adx, 1 612 keys), 800 in the float users (adf, 1 600 keys); 47 runs in each group have at least one; 104 of 105 pairs are equal (the exception: STPM DOY 146, 7 against 1). UTQI (407) and WUH2 (261) hold 83 % of the adx total. The engine's run total (`RealTimeEngineEstimate.cpp:290-294` at 8ca1a20) is used, since in 10 runs the resolver log rotated away its PARTNERREL lines. Recomputed from `rt_engine.log` by the ruling-tables part and the independent check.

**Options and cost** (`tables/ruling_options.csv` rows 15 to 17).
- **Keep:** no cost. The comparison is fixing against float under the same refloat rule.
- **`refloat_partner_signal` off at adf:** 105 replays (about 6 min each; about 2 h at 6 jobs, at night) and the F6 re-grade for five days; output expected to move at 47 runs and stay identical at 58 (inferred).
- **`ar_mode` 0:** no resolver is built (`RealTimeEngineInit.cpp:298`), so no CLFRAC lines exist; the F6 grader (`_irdu_grades/scripts_s20/user_fix_vs_float_free2.py`) reads CLFRAC from the float user and needs a new float source (for example `amb_m` of the satellite trace), plus the 105 replays.

**Recommendation.** Keep, and name it "float under the same refloat rule" in every grade. The refloat counts are equal in 104 of 105 pairs, so the rule acts on both sides alike and the comparison isolates the fixing decision. A resolver-free float answers a different question (the refloat rule's own effect) and needs its own ruling. Whether the held pair-epoch grade counts rejected re-fixes is not settled here.

### 3d. `bias_coverage_requires_apply` in the user-integer posture (URE decision 1)

**Ruling asked.** Should the user-integer posture arm `model_corrections.bias_coverage_requires_apply`?

**The .BIA question, answered by two searches.**
- Search 1, code (MAIN, same lines at 8ca1a20 where checked): `PathResolver.cpp:356` globs `.bia` in the observation day's products with no key gating it; `ProductParser.cpp:1451-1458` parses it unconditionally; `GnssCorrections.cpp:93` and :1812-1875 build the presence index. `biaEntryApplies` returns true whenever the flag is false (`MeasurementModelRow.cpp:70-78`; default false, `GnssConfigLoad.cpp:2573`), and the AR gate then uses raw .BIA presence (`MeasurementModelRow.cpp:132-136`, `MeasurementModelStates.cpp:784-788`).
- Search 2, disk and run folders: `data/2026-146/products/COD0MGXFIN_20261460000_01D_01D_OSB.BIA` (and DOY 145, 147) is on disk; it covers G C1C, C1W, C2W, L1C, L2W (32 satellites) and E C1C, C1X, C5Q, C5X, L1C, L1X, L5Q, L5X (30). The arms use `data_source` internal with no bias-path key and set `enable_code_phase_bias` false. The trace scan agrees with a loaded .BIA: no uncovered code row falls outside it (`tables/uncovered_rows.csv`).
- The logs cannot show it: `corrections.log`, `product_cache.log`, `product_parser.log` and `resolver.log` are 0 bytes because those loggers are off, so their silence proves nothing. "Resolved 10 product files" in `main_log.log` matching a list that includes the .BIA is inferred. The load rests on code and disk, not on a log line.

**Consequence.** AR eligibility today follows a file that corrects nothing in these users.

**Options and cost** (`tables/ruling_options.csv` row 18). Arm: 42 phase rows become ineligible (all at `_rap_nw_obsad{x,f}_144_quin`), and 2 314 code rows per user group (29 runs) plus 75 at P1 move to the weight 100. Not in the user fingerprint; no ledger row exists today. CHAIN `tests/network/test_codebias_network_config.cpp:123-167` and `tests/test_seeds.cpp:414, 1566-1573` already cover the flag. Night: 29 + 29 replays and one held-out day. Leave: no cost.

**Recommendation.** Arm it in the user-integer posture, but only after the night replays: it is a value change, not an adoption of what was graded, and goes beyond the adoption inventory. Its ledger row goes with the inventory's step 1(g).

## 4. The strict level and the known-unread list (CFG 4, CRT 3, night report item 13)

**Ruling asked.** Add a strict check for unknown keys and requests with no effect, with which default, and which keys go on a known-unread (warn-only) list?

**Today.** No such check exists: `strict_config` and `config_key_check` have 0 hits in `src/` and `include/` of both trees.

**Candidates** (cumulative, defined in `scratch/ruling-tables/RULES.md` before counting): (i) unknown keys against the union reader set of both trees, `_`/`//` and `extends` exempt, the three §1 keys warn-only; (ii) the three keys abort too, except in the three reference files taken as regenerated; (iii) plus the 17 no-reader keys of §5; (iv) plus a derived pin the file asks and the derivation replaces, and the W6 forcing (lower bound: knob resolutions not modelled).

**Cost** (`tables/strict_cost.csv`; a file that aborts under strict warns under warn).

| population (base) | (i) | (ii) | (iii) | (iv) |
|---|---|---|---|---|
| config/ engine files (1 272) | 5 | 921 | 1 269 | 1 269 |
| distinct runs (16 157; 17 210 folders) | 893 | 15 145 (16 120 folders) | all | all |
| reference configurations (3) | 0 | 0 | 3 | 3 |
| adopted day runs (6) | 0 | 6 | 6 | 6 |
| graded users (210) | 0 | 210 | 210 | 210 |

- Since 2026-11-10: (i) 774 runs, (ii) 12 350. 17 203 of 17 210 folders date from November, so the date split says little.
- The 893 under (i) are keys no tree reads, from unmerged branches: `realtime.iono_map.*`, `files.ionex_*`, `amb_float_drop_recentre`, `emit_slant_iono`.
- The 5 config files under (i): `logging.level` (4 files) and five `estimation.corrections_ready_gate.*` keys (1 file, `config/_rtseam_sbf_bssd.json`) (inferred pairing). Those five are a misspelling: the loader reads `estimation.defer_until_corrections_ready.*` into the member `corrections_ready_gate_` (CHAIN LD:1725-1736), so that file believes it arms a gate that stays off.
- (iii) is driven by three keys only: `satellite_interpolation_mode`, `leap_seconds_tai` and `logging.levels.network`; the other 14 strict no-reader keys reach 16 config files and 0 runs.
- **Tree dependence** (`tables/strict_cost_by_tree.csv`): against MAIN alone, (i) aborts 6 of 6 adopted runs (`phase_families`, `phase_family_codes`, `ring_fix.entry_revalidate` unread there) and 1 029 runs; against CHAIN alone, 10 169 distinct runs (MAIN's user-engine keys); `network.trace_live_record` is unread at CHAIN. One reader list for the combined tree is needed (inferred: `int/2026-11-19` reads all four), from the exact read set, not `loader_keys.py`.
- **Sample** (`tables/sample_check.csv`): 60 keys drawn as unread (seed 20261119), 0 wrongly classed; 8 named keys, 1 wrongly classed (`network.max_duration_s`). The independent check: 20 unread and 20 read keys (seed 4242), 0 wrongly classed.

**Known-unread list per default.** Off: none. Warn: none needed to load. Strict on unknown keys (i): the prefix rule (`_`, `//`) and `extends`, plus the three §1 keys unless §1 (i) lands, plus the four tree-divergence keys on a tree that lacks them. Strict on inert and changed (iii, iv): all that plus a designed list per engine; on every reference and adopted file at least 2 W6 forcings (CHAIN LN:3943, :3945), 2 derivation pins (LN:13756, :13916), 3 no-reader keys and 4 to 6 knob resolutions (`Knobs.cpp:445-448`, :326-327; LN:415-422, :12308-12313) fire. Its length is not countable until the ConfigDecision rows exist.

**Code cost.** Warn: a generated key universe and a walk after the load (108 absence assertions in 46 test files, CRT correction 9); a 10-epoch smoke. Strict (i): an abort and the exemption table, a test per exemption. Strict (iii, iv): the ConfigDecision rows (CRT stages 1 to 3); 18 tests on TB load a reference file.

**Recommendation.** Default warn now. Strict on unknown keys, candidate (i), as an opt-in key (`processing.config_key_check`) after the combined tree, from the exact read set. Candidates (iii) and (iv) wait for the designed list (CRT decision 3).

### 4b. Aliases (CRT decision 1)

**Evidence** (`tables/alias_use.csv`). 0 of 16 157 distinct run `arm.json` use a flat alias. In `config/`, one engine file does: `config/archive/test_suite/_golden_sim_tri_w6_smoke.json` (top-level `max_duration_s` 600 beside `processing.max_duration_s` 600, equal). The fallback is read only when the dotted key is absent (CHAIN `GnssConfigJsonHelpers.hpp:127-134`). The alias list omits the top-level `rhs_attrib_columns`, read by a direct `json.contains` (CHAIN LN:2355, MAIN :2294); no file uses it.

**Recommendation.** A warning period that records `alias_used`, then removal. No current run moves. The alias list names `rhs_attrib_columns` before it is called complete.

## 5. The no-reader keys of CFG section 4.3 (CFG 5)

**Ruling asked.** Retire, give a reader to, or correct the echo of each key the loader parses but no code uses?

**Evidence** (`tables/noreader_keys.csv`, 22 rows: 21 keys and one knob member; two searches each, getter and member, at CHAIN and MAIN, `scratch/rules-code/r5_search.out`).
- No code reader, echoed as if in effect: `satellite_interpolation_mode` (echo CHAIN LD:506), `leap_seconds_tai` (LD:3641), `baseline.use_doppler` (LD:3479), `realtime.output_type` (LD:3255-3261), the non-causal slip bridge and its three tuning keys (toggle line LD:3656, :3682; `SlipBridgeKernel.hpp:18` says the key is parsed and not read).
- No code reader, no echo: `promote_sat_code_bias` (written only, LN:3636, :3944, :13735; named by the W6 line LN:3975-3980), `realtime.output_path`, `realtime.max_epoch_buffer`, `logging.enable_if_slip_trace`, `logging.enable_gf_slip_trace`, `receiver.antenna_orientation.elevation_deg` (only the azimuth is read, `Antenna.cpp:960`), `logging.levels.network` (no logger has that name). The knob `prires_s0g_redun` is written at CHAIN `Knobs.cpp:579` and never read.
- Read but never applied: `logging.enable_smoother` and `logging.enable_turbo_edit_resolver` (mapped at `GnssConfig.cpp:30, :37`; neither name is in `all_loggers`, CHAIN `Application.cpp:84-95`).
- By design or read through a default (4): `estimation.enable_if_slip_detector` (default of `cycle_slip.if`, LD:1291; the toggle line LD:3661 prints this legacy flag, not the detector in effect), `network.brdc_select` (fingerprint term, `SnapshotFingerprint.cpp:417`), `network.apriori_clock_track_phase_clock_hold` (relaxes the refusal at LN:320-343; run-end line `BrdcWire.cpp:126`), `filter.process_noise.clock_bias_q` (read by the user engine, `StateManagerProcessNoise.cpp:45`).

**Values** (`tables/known_noreader_values.csv`). `satellite_interpolation_mode`: 1 265 config files and 16 157 of 16 157 runs, all "lagrange". `leap_seconds_tai`: 1 269 config files and 16 157 runs, all 37. `clock_bias_q`: 1 241 network config files and 14 974 runs, all 1000 against the loader default 1e4. `logging.levels.network`: 364 config files, 14 974 runs. `brdc_select`: 1 482 runs. The phase-clock hold: 76 runs. Never set in any population: `promote_sat_code_bias`, `baseline.use_doppler`, `realtime.output_path`, `realtime.max_epoch_buffer`, the bridge and its three tuning keys, `enable_if_slip_detector`.

**Options and cost** (`tables/ruling_options.csv` rows 23 to 25).
- **(a) Retire the 17 keys with no code reader, keep the 4.** 8 or more files. One warning per load (network mode only) in about 1 269 config files and every run, unless the four keys the reference files set (`satellite_interpolation_mode`, `leap_seconds_tai`, `clock_bias_q`, `logging.levels.network`) are unset in the manifest. Retired-key tests as §1 (ii); retiring `brdc_select` or the phase-clock hold would also delete ledger rows (`test_retired_config_keys.cpp:340`) and a fingerprint term. Six test files name a key or getter on both trees, more on TB (listed in `tables/noreader_keys.csv`).
- **(b) Correct the echoes** (the echo says "fixed" or "not applied"): 1 to 2 files, log text.
- **(c) Give the two logger keys a reader** (add to `all_loggers`): 1 file; a false request then silences that logger.

**Recommendation.** (b) now. Retire the keys set in no file (zero files warn): the bridge and its three keys, `promote_sat_code_bias` (and drop the word from the W6 line), `baseline.use_doppler`, `realtime.output_path`, `realtime.max_epoch_buffer`; remove the knob `prires_s0g_redun`. Retire `satellite_interpolation_mode`, `leap_seconds_tai` and `logging.levels.network` only together with their manifest unsets in the regeneration. Keep the four by design and note them in the run-start table.

## 6. One policy for a dependent key without its master (CFG 6, CFG R8, CRT 7)

**Ruling asked.** When a file sets a key whose every reader is gated on another key that is off, should the load refuse, warn and disarm, warn, or stay silent, uniformly?

**Rule and catalogue.** The rule (`scratch/rules-code/r8_rule.md`, written before the catalogue): a dependent is a key whose every reader sits in a branch gated on another key; response classes come from the loader's own lines. The catalogue (`scratch/rules-code/r8_catalogue.csv`, CHAIN, script-extracted, a candidate list) holds 473 pairs: 159 from loader lines (88 refuse, 60 warn, 7 warn only when the value differs from its default, in the ring-fix list LN:11262-11301, 4 warn and disarm, for example `sd_ewl.fix_enable` LN:1981 and `suspect_epochs` LN:1352-1355) and 314 from the ledger "parent" column (227 with no loader line naming both, 87 named together). The ring fix refuses its inner dependents (LN:1514-1711) and itself without eq34 (LN:11237-11240). The extraction misses the warn form "set but X is OFF" (92 lines) and misreads non-boolean masters; hand sample: 7 of 8 silent candidates hold.

**Counts** (`tables/r8_pairs.csv`, `tables/r8_counts.csv`, corrected, section 11 item 1).
- Runs (14 985 network arms): 2 silent pairs carry a non-default value with the master off: `cb_kaula_sigma_m` with `cb_kaula_prior` off (3.0 against 10.0; 14 985 arms, 15 960 folders; read only in `BiasAdmission.cpp:172, :215, :263` under the prior) and `cycle_slip.oracle_truth_root` with `oracle_relevel` off (349 arms, 448 folders).
- Config files (1 241 network files, each tree): 7 silent pairs, 384 file-pairs with a non-default value.
- Refuse-class hits in the runs (15 026) are extraction artefacts: `sat_gf_datum` against `w6_iono_constraint` "none", the required value, not an off value (LN:4058-4070), and `phase_frame_hold_level_vote`, whose master is a disjunction (LN:352-360).
- CRT 7: LN refuses by throw (189) and by error plus `return false` (85); LD by `return false` only (CRT counts, not recounted here).

**Options and cost** (`tables/ruling_options.csv` rows 26 to 28, corrected).
- **Status quo:** no cost; the ring fix stays the one refusing family.
- **Uniform warn** from a declared ledger "requires" column (CFG step C7): 3 or more files; log text only (`cb_kaula_sigma_m` warns in 14 985 of 14 985 arms, `oracle_truth_root` in 349); a table test.
- **Uniform refusal:** every arm of the reference lineage stops until the manifest unsets `cb_kaula_sigma_m`.

**Recommendation.** Uniform warn from the declared column, with the ring fix keeping its refusals; unset `cb_kaula_sigma_m` in the regeneration if the author wants the reference files clean. CRT 7: one refusal mechanism behind the row's `on_range: refuse`, chosen in the code session (inferred output-neutral, since both stop the load).

## 7. Ledger rows for the network leaf keys and the two KEEP_OFF bundle rows (CFG section 4.4 and correction 6)

**Ruling asked.** Should the 24 network-loader leaf keys without a ledger row get rows, with which verdicts, and what happens to the two KEEP_OFF rows of the SH posture bundle?

**Evidence** (`tables/ledger_rows_proposed.csv`; set script `scratch/rules-code/sets_e7bc098.py` at both trees). 22 leaf keys plus 2 scalar arrays read by range-for (`rhs_attrib_columns`, CHAIN LN:2355-2368; `closed_loop.ar_active_systems`, LN:13509-13512): the 11 `network.process_noise` classes, 5 legacy keys the network loader probes (`filter.process_noise.sat_clock_q`, `sat_phase_bias_q`, `sat_code_bias_q`, `estimation.global_vtec_sh.q_per_degree`, `layer2_q_per_degree`), `position_ecef` under `stations[]` and `closed_loop.station`, `tow`, `east_m`, `north_m`, `up_m` under `inject.events[]`, and the two arrays. All 24 are absent from both ledgers (confirmed by the independent check). None overlaps the inventory's step-1 rows.

**Bundle rows.** CHAIN `Posture` lines 1346-1348 require `ionosphere_weighting.enable` and `elevation_cutoff_deg` of 25 or more inside the SH bundle, armed only under `estimate_iono_sh` (KEEP_OFF), so the conflict is latent on every reference file.

**Cost.** 2 files (the ledger and its rendered view). No output, no fingerprint, no test. At CHAIN, `ledger_render.py --check` prints "matches the JSON", `make_golden_config.py --verify` passes (84, 80 and 77 recommended knobs armed), `config_posture_check.py --verify-loader` reports 134 of 134 and 0 gaps; new PARAM, KEEP_OFF, TRACE or CORE rows trigger none of their failure conditions. A JSON edit needs a re-render.

**Recommendation.** Add the 24 rows with the proposed verdicts (PARAM for thresholds and process noise, KEEP_OFF for SH-only values, CORE for coordinates, TRACE for the attribution columns). `elevation_cutoff_deg` becomes PARAM; `ionosphere_weighting.enable` stays KEEP_OFF with a regime note that it is a required member of the SH bundle. One commit with the inventory's step 1.

## 8. Log line order under "identical" (night report item 16)

**Ruling asked.** Does the order of log lines belong to "identical"?

**Evidence.** The paper of package chain-exact-resume-paper (`packages/chain-exact-resume-paper/XRS_exact_resume_2026-11-19.md`, section 6): the restored-signal audit lines (120 on m1, 108 on n147 per run) are equal as multisets between two repeats of one seed; their order follows the OpenMP schedule of `passAScanStations` (CHAIN `PassASlipScan.cpp:106`). Its recommendation 4 orders them per station with its option A. The comparison tools differ: `rt_seam_compare.py` compares masked logs as ordered lists; `compare_network_run.py` compares `run.log` in order and leaves other `.log` content out by default because parallel regions give a nondeterministic order (:117-121); `check_solve_identity.py` hashes `products.txt` records in order; `irdu_build_identity.py` compares two logs byte for byte. URE decision 6 asks whether traces count as output.

**Options and cost.** (a) A definition only: 0 files. (b) Buffer the audit lines per station and flush in station order: 1 file, log order only; tools that compare logs in order (`irdu_build_identity.py`) see the change; a 2 h identity pair.

**Recommendation.** "Identical" means products, traces and manifests byte-identical with records in order; each log equal after masks, as an ordered list for lines written outside parallel regions and as a multiset inside one, until those lines are station-ordered by (b), which travels with the exact-resume work. Decided together with the author's ruling on the exact-resume paper.

## 9. Fit with the adoption inventory

From `tables/adoption_fit.csv` (corrected row for ruling 6); the inventory is not repeated.
- **Same regeneration (inventory step 3, form B primary entries):** §1 (i) unsets, §2 (a) unset, §5 unsets if those keys are retired, and the `cb_kaula_sigma_m` unset if §6 ends in refusal. The generator verify checks each unset (`make_golden_config.py:173-176`). The member-dump baseline on TB is regenerated once, with step 5 (inferred).
- **Same ledger commit (step 1):** §7's 24 rows and the two bundle rows; the `bias_coverage_requires_apply` row (§3d), with its value only after the night replays.
- **A gap in the user rule set:** `network.closed_loop.ar_wl_only` is false in 210 of 210 graded users (`scripts/writer/ar_wl_only_check.py`) while its loader default is true (CHAIN LN:13399; ledger PARAM, default true). A user rule set generated without it would fix wide-lane only on the differenced branch (inferred). Adding it carries the graded value, so it is an adoption, not a value change.
- **Conflicts:** §3b option (C) would move the held-out station on the frozen `*_rf0hv18` lineage (C1W first); keep it out of the regeneration. A strict level built on MAIN alone aborts 6 of 6 adopted runs; it comes after the combined tree. A uniform R8 refusal needs `cb_kaula_sigma_m` unset first.
- **Agree:** `max_position_sigma_m` 0.05 sits below the 0.20 cap (CHAIN LN:13778-13779); the GPS code order with C1C first makes priority and count latch the same signal on the adopted configuration; `pb_hold_presence_flip` is inert on the holds-off stream, so the inventory's open item on it does not change the five-day numbers (inferred).
- **Fingerprints:** no loader default changes in any recommended option, so no snapshot fingerprint moves, in line with inventory section 5.

## 10. What this read-only team could not do, and who needs what

- **Code changes the rulings choose** (retiring keys into `RetiredConfigKeys`, readers, the derivation reporting its own rows, the strict flag `processing.config_key_check`, the named signal-rule key, the comment corrections at CHAIN LN:13718-13725 and :13810-13811, the `loader_keys.py` tiling rule, the misspelled gate keys of `config/_rtseam_sbf_bssd.json`): package config-in-effect-table or a later code session. Needs: the ruled option per item, the known-unread list of §4, the pin table of §3, the test names in `tables/ruling_options.csv`.
- **Manifest unsets** (§1, §2, §5 if chosen) and regeneration with `make_golden_config.py --verify`: the adoption session, in the form B regeneration. Needs: `tables/adoption_fit.csv` and the ruled options.
- **Ledger edits** (§7 rows, the two bundle rows, the user keys including `bias_coverage_requires_apply` and the `ar_wl_only` value), `ledger_render.py`, `config_posture_check.py --verify-loader`, `docs_record.py verify`: the adoption session or chain-audit. Needs: `tables/ledger_rows_proposed.csv`, deduplicated against inventory step 1.
- **Night runs, only on a ruling:** a paired user replay with the derived pins removed or named; the .BIA flag armed (29 + 29 replays, one held-out day); a rebuilt float reference user (105 replays). Needs: the run lists in `tables/user_pins_by_run.csv` and `tables/partner_releases.csv`, and the stream products of `_rap_ad_n143` to `_rap_ad_n147`.
- **Filing the results** (dated results appended to CFG and CRT, a new record if wanted): chain-audit, docs only, with `docs_record.py verify` passing.
- **Log-order definition** (§8): with package chain-exact-resume-paper; any tool change follows the author's ruling.
- **Not settled by this paper:** CFG 7 (the Galileo pair family; the inventory keeps `phase_datum_eq34_family_pairs` off, section 3), CFG 8 (partial fixing in the user engine: off at every graded user; its on/off evidence is a memory note, not checked), CFG 9 (`service_mode` ordering; no reference file sets it), CFG 10 (user-engine keys spelled `network.*`; it bears on the single reader list of §4).

## 11. Corrections from the independent check

The independent check (`parts/skeptic.md`) ran on the four parts before this paper existed; style of this paper was therefore not checked by it. Each item: holds, corrected, or unverified. The body above carries the corrected values; nothing was changed silently.

1. **R8, `carry_invariant_innov_k` "14 985 runs non-default": corrected.** Every network arm holds 4.0, the loader default (CHAIN LN:8753-8755) and the ledger default "4.0"; `scratch/rules-code/scan_analyse.py:90-97` turned 4.0 into "4" and compared strings. Repaired here: `scripts/writer/r8_recount_numeric.py` keeps every other rule, reproduces the saved tables exactly with `--old`, and with the numeric comparison gives 0 of 14 985 for this pair. The runs now hold 2 silent non-default pairs, not 3. The config figure "11 silent pairs, 518 file-pairs" becomes 7 and 384: besides `innov_k` (126 to 0), `state_snapshot_interval_s` (13 to 8), `sd_ewl.fix_sigma_floor_m`, `redundancy_hot_sigma` and `redundancy_pivot_eps` (1 to 0 each) fell; `rank_gate_tol` (63) is unchanged. `tables/r8_counts.csv`, `tables/r8_pairs.csv`, three cells of `tables/ruling_options.csv` and one row of `tables/adoption_fit.csv` are corrected by `scripts/writer/apply_corrections.py`; the as-found files are kept as `*_as_found.csv`. The ruling-tables reproduction of 14 985 had re-evaluated the same saved output and inherited the defect.
2. **"The comment at LN:13720-13725 is wrong": corrected** to "by design; the comment overstates its scope" (§3b). The comment's subject, `applyNetworkConsumerCouplings`, is called by both paths; only "cannot diverge" overreaches.
3. **"A standalone run of the same file cannot occur" (CFG V5): corrected** to "0 standalone users in any population"; nothing in the loader stops one (§3).
4. **Duration spellings, 18 against 24: holds as 24** (§1).
5. **Three unread keys, strict-level counts, aliases, `consume_network_iono`, pins of the graded users, the .BIA load, partner refloats, retired-key test lines, ledger rows: hold** (the check's own numbers). Its additions (the misspelled gate keys, the `.jsonc` files, `rhs_attrib_columns`, the wrong comment at `ce_make_arms.py:119`) are in the body.
6. **Rule files saved after their tables: holds as a caveat** (Header).
7. **`ar_wl_only` "goes beyond the inventory as a value change": disagreed.** The 210 graded users set `network.closed_loop.ar_wl_only` false (recomputed, 210 of 210), so carrying it adopts the graded value. The point stands for `bias_coverage_requires_apply` (§3d, §9).
8. **Completeness of the 24 ledger rows over other read forms: unverified.**
9. **The R8 catalogue beyond its hand sample: unverified** (missed warn form, non-boolean masters, seam-side gates not read).

**Writer's reruns.** Every analysis script was rerun on the saved collections (sandbox copies under `scratch/writer/`) and reproduced its table byte for byte, except two supporting files named in "Not checked". The collectors (`scan_collect.py`, `scan_runs.py`) were not rerun: `runs_nom` grows during the day and would move the bases.

## Decisions for the author

1. **Three unread keys:** unset in the manifest in the form B regeneration. Recommendation: yes (§1 i).
2. **`consume_network_iono`:** unset in the primary entries. Recommendation: yes (§2 a).
3. **Derived pins:** declare them as graded posture, print asked beside in effect, then name each; add `estimate_ifb` inherited true and differencing at P2. Recommendation: yes; no value change without a paired replay (§3).
4. **Signal rule:** by design. Recommendation: correct the comments (A) and add `network.closed_loop.signal_rule`, default count (B); (C) only as its own ruling (§3b).
5. **Float reference user (UFA 7):** Recommendation: keep, labelled "float under the same refloat rule" (§3c).
6. **`bias_coverage_requires_apply` (URE 1):** Recommendation: order 29 + 29 night replays and one held-out day, then arm (§3d).
7. **Strict level:** Recommendation: warn by default now; strict on unknown keys as opt-in after the combined tree (§4).
8. **Aliases (CRT 1):** Recommendation: a warning period recording `alias_used`, then removal (§4b).
9. **No-reader keys:** Recommendation: correct the echoes; retire the never-set keys and the dead knob; keep the four by design (§5).
10. **Dependent without master (R8, CRT 7):** Recommendation: uniform warn from a declared ledger column, the ring fix keeps refusing, one refusal mechanism (§6).
11. **Ledger rows:** Recommendation: add the 24 rows, SH elevation cutoff to PARAM, with inventory step 1 (§7).
12. **Log order:** Recommendation: adopt the definition of §8; order the audit lines with the exact-resume work.
13. **User rule set:** Recommendation: carry `network.closed_loop.ar_wl_only` false, the graded value, in step 1(g) (§9).

## Not checked

- No engine was run; no claim about run behaviour is made. The output moves of §3b (C), §3c and §3d are expected, not measured.
- The age-gate and `estimate_ifb` effects: no on/off pair of runs exists.
- Whether a third canonical candidate earlier in a priority list exists anywhere (only two candidates are printed); the "162 → 80" runs (not on disk); the ARN failed starts' cause (the record's attribution).
- Whether the held-out runs' build (`gnss_processor_cha_fp1.exe`) was cut from exactly the e7bc098 derivation (build time only).
- The test line numbers on TB 511bf3e, which is being merged and may shift.
- Loader forcings before the derivation (CHAIN LN:3942-3946) are modelled only for `estimate_ifb` and `estimate_sat_biases`; "asked" means the file's value. Empty parents of retired families (1 run each) count as unread. The section 118 users are taken as `_rap_nw_obsn46{x,f}_{146,147}_*` (inferred from names).
- The R8 catalogue beyond its hand sample; the 24 ledger rows over other read forms.
- `ionosphere_weighting.enable` and `enable_code_phase_bias` stay unchanged on every file only if `w6_iono_constraint` is "none" (CHAIN LN:3964): not checked file by file.
- The method difference against CFG's 343 and 6 (as written, no `extends`): this paper finds 343 as written for `min_stations` and 8 as written for the duration difference; which 6 CFG counted is not settled.
- `sample_check2.csv` was written by hand from `sample_hits2.txt`; `strict_cost_keys.csv` (scratch only) breaks ties among its top 15 keys by hash order, so a rerun may name another key of count 1.

## Filing note (2026-11-21)

Filed on 2026-11-21, after the fact, from the package `config-rulings-paper` of the day program of 2026-11-19 (`D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/config-rulings-paper/`: `CONFIG_RULINGS_PAPER_2026-11-19.md`); its parts, tables and scripts stay there. The text below is the package text, unchanged apart from heading levels. The rulings are still the author's.

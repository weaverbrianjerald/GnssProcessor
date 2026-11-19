# NSM: a phase map of the network setup function (2026-11-18)

**STATUS**

- **id:** NSM
- **topic:** network-vehicle
- **status:** open
- **summary:** A phase table of `Application::networkSetup_` (`src/core/network/driver/ApplicationNetworkSetup.cpp:182-1924`), the locals that cross phases and the cut order they fix, the fallbacks and silent changes, the reads that depend on the processing window or the whole file, and a nine-step plan for a later identical-output split, none of it carried out. The coverage gates are tallied over the parse span, not over the processing window, and count the held-out station and the stations the tier-2 rule drops later; the effect on admitted channels is read from the code, not run. Seven decisions are put to the author; the corrections of 2026-11-19 narrow decision 6 and correct the phase order of section 3.3.
- **source:** plan package 12 of post_reset_program_2026-11-18, "Phase map of the 1600-line network setup function"; network code read at 03dfcbc of branch chain/audit and checked at 7fb0737 (`ApplicationNetworkSetup.cpp` is byte-identical between the two); main 5ddc422 for user-engine code (not needed by the check). Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** `Application::networkSetup_` runs from line 182 to 1924 (03dfcbc; the file is byte-identical at 7fb0737) and falls into 18 phases before the station-building lambda, an 8-part lambda (963-1259) and 10 phases after it. Two kinds of locals block a clean split: the two coverage allow-lists (835-836) and the lambda's collectors and counters (926-959). The coverage gates are tallied over the parse span, not over the processing window [t0, t1), and count the held-out station and stations the tier-2 rule drops later. The group-delay latch and the S contract names read [t0, t1). A nine-step identical-output split is planned; none is carried out.

Read at commit 03dfcbc of branch chain/audit (worktree D:/GnssProcessorSupport/worktrees/chain). `src/core/network/driver/ApplicationNetworkSetup.cpp` has no uncommitted edits; every line number below is at 03dfcbc. External line numbers marked "(stage 1)" come from the first mapping pass and were only partly re-read (see "Not checked").

## 1. Scope and how the passes were merged

Two mappers split the function at line 990; one reviewer re-read the cited lines. The merge keeps what the review confirmed, corrects what it refuted and says so in place. Four corrections change the first-stage text:

| Item | First stage | Verdict | Basis |
|---|---|---|---|
| Boundary of the halves | 990 | 963: the lambda starts there (963) and is first called at 1260; 990 is inside its body | ApplicationNetworkSetup.cpp:963, 1260 |
| Navigation parses per setup | up to four (382, 506, 598, 704) | at most three: 506 (`brdc_rts`) and 598 (`sp3_brdc_clock`) are two values of one enum and exclude each other | GnssConfig.hpp:2225-2226; ApplicationNetworkSetup.cpp:227-228 |
| A chain restore mixes two windows | reported, citing ChainMemory.cpp | dropped from this map: `src/core/network/state/ChainMemory.cpp` is not in commit 03dfcbc (`git show 03dfcbc:` fails); it belongs to the paused step and is to be checked against that design | git show exit 128 |
| A failed user engine "turns the closed loop off" | reported | imprecise: 1848-1851 only reset `consumer`; `setup.closed_loop_on` stays true, and readers re-test the pair | ApplicationNetworkSetup.cpp:1848-1851 |

Two smaller corrections: the reader "1956" of the marker-to-ARP counters reads `ctx.counters` (1957-1960), reached only through the setup copy (1352-1391) and `networkMarkerArpCensusCopy` (1968-1981); and the first-half fallback rows 986-996 and 1005-1013 lie inside the lambda.


## 2. Phase table

[corrected 2026-11-19: see Corrections, item 8] Prefix: A to R before the lambda; S1a to S1h the lambda body; S2 to S11 after it; F2 and F3 are the two other functions of the file.

| Phase | Lines | What it does | Writes | Exit on failure |
|---|---|---|---|---|
| A | 182-188 | Binds the logger and the network configuration; builds `product_parser_` if missing | `product_parser_` (member) | none |
| B | 190-209 | Sync roster: the clock-datum station as reference, the others as rovers | `setup.roster`, `pivot_found` | return 1 (205-208) |
| C | 211-234 | Held-out station flag, a priori source flag; adds the held-out station to the roster (231) | `setup.closed_loop_on`, `setup.brdc_rts`, `setup.roster` | none |
| D | 236-238 | Constructs `MultiRinexSync` | `setup.sync` | none |
| E | 240-281 | Phase-family intake: code renames per member, installed on each network station (266); refuses streaming | sync rename table | throw (250-253) |
| F | 283-307 | Clips the RINEX parse to start offset + `max_duration_s` + 60 s when `max_duration_s` > 0 (295-298) | sync parse window | none |
| G | 309-322 | `sync.load()` (non-fatal), `sync.synchronize()` (fatal) | synced epochs (inside `sync`) | return 1 |
| H | 323-344 | Removes systems absent from `processing.active_systems` (332) | synced epochs (inside `sync`) | none |
| I | 345-351 | Binds `synced` | `synced` (reference) | return 1 if empty |
| J | 352-361 | Processing window: t0 = first synced epoch + start offset; t1 = t0 + `max_duration_s`, or last synced epoch + 1 s | `setup.window_t0_s`, `setup.window_t1_s` | none |
| K | 363-451 | Broadcast health series; flagged spans logged over [t0, t1); optional removal in exclude mode (434-440) | `setup.sat_health`, `setup.sat_health_removed`; synced epochs | none |
| L | 453-461 | Loads products; binds the `ProductCache` singleton | `product_cache` | none |
| M | 462-562 | `brdc_rts`: parses, sorts and stages navigation records; refuses precise products | `setup.brdc_nav_staged`, `brdc_nav_cursor` | return 1 |
| N | 563-623 | `sp3_brdc_clock`: refuses CLK and systems other than GPS and Galileo; builds `BrdcClockApriori` over the window | `setup.brdc_apriori` | return 1 |
| O | 625-790 | Ionosphere and code-bias datum inputs: SH model, or the no-constraint posture with the group-delay latch (684-763), or the GIM | `setup.sh_model`, `sh_model2`, `setup.sat_gf_target_m`, `gim_` (member) | return 1 |
| P | 792-833 | Log only: layout flags that apply to the held-out station, not to network stations | none | none |
| Q | 835-919 | Coverage tally over every synced epoch (845) and the two allow-lists, with the KEEP/GATE log | `signal_allowlist`, `phase_bias_allowlist` (locals) | none |
| R | 921-962 | Station-build preparation: collectors, marker-to-ARP counters, `StationBuild` enum | about 15 locals (926-959) | none |
| S1a | 963-968 | Lambda `buildStation` (captures `[&]`): station parser lookup | none | returns Failed |
| S1b | 969-1015 | ANTENNA: DELTA H/E/N; `markerToArp` when armed | `st.ecef`, seven eccentricity counters | none |
| S1c | 1016-1061 | Parameters, `PppModel`, row inputs (allow-lists 1040, 1059-1061; broadcast gate 1048; broadcast clock a priori 1052) | `st.params`, `st.model`, `st.rows` | none |
| S1d | 1063-1113 | Receiver ATX match, tier-2 exclusion, radome fallback | three antenna lists | returns Excluded |
| S1e | 1115-1162 | ATX frequency route of the two lowest priority bands per system | `atx_src_by_sys` | none |
| S1f | 1164-1203 | MW detector and TurboEdit resolver | `st.mw_slip`, `st.turbo_edit` | none |
| S1g | 1205-1252 | IF-TD detector, reference gate | `st.if_slip` | none |
| S1h | 1254-1258 | Seeds the station state, pushes the station | `stations` | none |
| S2 | 1260-1293 | Builds the pivot, then the rovers, skipping the held-out station | `stations` | return 1 (1270, 1275) |
| S3 | 1294-1350 | Antenna summaries | none (log) | none |
| S4 | 1352-1391 | Copies the marker-to-ARP counters into setup; prints the posture | `setup.n_marker_arp_*`, `marker_arp_max_h(_station)` | none |
| S5 | 1393-1462 | Phase families as armed: identity, member or both per receiver; refuses receivers declaring both | `st.phase_tracking` | throw (1458) |
| S6 | 1464-1470 | Station roster CSV | file in `log_dir_` | none |
| S7 | 1472-1561 | Thread budget, parallel seam, carry threading, logger prewarm | `setup.n_sta_total`, `parallel_on`, `n_threads`, `seam_threads`, `carry_par` | none |
| S8 | 1563-1623 | Emitter: step class, phase-family members, ring-fix unsettled steps, continuity guard | `setup.emitter` | none |
| S9 | 1625-1809 | Product path and the v2 S contract; removes a stale product file | `setup.prod_path`, `pending_contract`, `contract_pending` | none |
| S10 | 1811-1921 | Closed-loop user engine: truth reduced to the ARP, `RealTimeEngine` built and routed | `setup.consumer_config`, `cl_truth_arp`, `cl_truth_hen`, `consumer` | none (failure resets `consumer` only) |
| S11 | 1923 | return 0 | none | none |
| F2 | 1929-1963 | `networkMarkerArpRunEnd` (run-end line from `ctx.counters`) | none | none |
| F3 | 1968-1981 | `networkMarkerArpCensusCopy` (setup to `ctx.counters`) | `ctx.counters.marker_arp_*` | none |

[corrected 2026-11-19: see Corrections, item 8] The function is 1743 lines, and the file holds four functions (networkSetup_, the lambda inside it, F2 and F3), not two as the program text says.

## 3. Locals that cross phases

### 3.1 Plain locals (these fix the cut order)

| Local | Written | Read | Lives in setup? |
|---|---|---|---|
| `signal_allowlist` | Q (835, filled 843-879) | S1c 1040 | no |
| `phase_bias_allowlist` | Q (836) | S1c 1059-1061 | no |
| `marker_to_arp` | R (952) | [corrected 2026-11-19: see Corrections, item 10] S1b 1005 | config copy |
| `n_ecc_stations`, `n_ecc_reduced`, `n_ecc_over_1cm`, `ecc_max_h`, `ecc_max_abs`, `ecc_max_h_station`, `no_ecc_record_stations` | R (953-959), S1b | S4 1356-1378 | no (copied into setup at S4) |
| `unmatched_antenna_stations`, `radome_fallback_stations`, `excluded_unmodeled_antenna_stations` | R (930-938), S1d | S3 1294-1303, 1331-1336, 1342-1345 | no |
| `atx_src_on`, `atx_src_by_sys` | R (944-945), S1e | S1e 1119, 1152; S3 1313 | no |
| `gal_mask`, `flagged_sats`, `n_edges` | K | K only | no (phase-local) |
| `sig_cover_min`, `pb_cover_min`, `chan_all_tally`, `sta_tracks`, `sys_n`, `code_n` | Q | Q only | no (phase-local) |
| `pivot_found` | B | B 205 only | no (phase-local) |

### 3.2 Values that cross through setup, configuration or members (do not block a cut)

| Value | Written | Readers (inside; outside) |
|---|---|---|
| `setup.roster` | B, C | D 236; S2 1260, 1277 |
| `setup.closed_loop_on`, `cl` (configuration) | C (216) | E 265; S2 1278; S6 1470; S10 1823; ApplicationNetwork.cpp:238 |
| `setup.brdc_rts` | C (227-228) | M 472; O 770; S1c 1048; S9 1715; EpochAdmission.cpp:60, SolPrep.cpp:88 (stage 1) |
| `setup.sync` | D | E 266; F 298; G to I; K 434; S1a 964; S5 1402; S10 1828; Run.cpp:287, BiasAdmission.cpp:292 (stage 1) |
| `synced` | I (reference into `sync`) | J 357-361; Q 845; re-derivable from `setup.sync` |
| `setup.window_t0_s`, `window_t1_s` | J | K 394-395, 438; N 605-607; O 710-711; S9 1707, 1728; Inject.cpp:320 (stage 1) |
| `setup.sat_health`, `sat_health_removed` | K | ClockDatum.cpp:196, HandoverStransform.cpp:306, 637 (stage 1) |
| `setup.brdc_nav_staged`, `brdc_nav_cursor` | M | EpochAdmission.cpp:62-105 (stage 1) |
| `setup.brdc_apriori` | N | S1c 1052; EpochAdmission.cpp:111, BrdcWire.cpp:39-103 (stage 1) |
| `setup.sh_model`, `sh_model2` | O | ApplicationNetwork.cpp:239, 280 |
| `setup.sat_gf_target_m` | O (725) | O 753; CodeBiasDatum.cpp:396 into Sbasis.cpp:101-108 |
| `product_parser_`, `gim_` | A, O | L 454-456; ApplicationNetworkAssembly.cpp:799 (stage 1) |
| `product_cache` | L | [corrected 2026-11-19: see Corrections, item 11] M 548, 557; N 574; no read after 574 |
| `setup.pending_contract` | S9 | S10 |
| `setup.network_store` | S8 region (1564) | S10 |

### 3.3 State carried by the `sync` object (no local shows it)

[corrected 2026-11-19: see Corrections, item 2] `retainSystems` (332, phase H) and the exclude-mode removal (434, phase K) both edit the synced epochs that phase Q tallies (845). t0 (357, phase J) is read after H and before K. The synced schedule is driven by the clock-datum station: epochs exist only where that station has data (MultiRinexSync.cpp:341-344), so t0 is that station's first epoch plus the offset. Phases H, J, K and Q are therefore order-coupled even though no local passes between them.

## 4. Cut order

A cut is safe when every value crossing it lives in `NetworkSetupResult`, the configuration, a member or a singleton. Phases before 921 can still `return 1`, so each extracted phase returns an int or bool; that is a mechanism, not a crossing local.

| Cut | Verdict | Why |
|---|---|---|
| [corrected 2026-11-19: see Corrections, item 9] A\|B through P\|Q (17 cuts) | safe | [corrected 2026-11-19: see Corrections, item 2] crossing values are setup members, configuration, `synced` (re-derivable) or the singleton; the order of 3.3 still binds H, J, K and Q |
| Q\|R | not safe as is | the two allow-lists are plain locals read at 1040 and 1061; they move into setup or a returned struct first |
| 990 | not a cut | inside the lambda body |
| R\|S1 and lambda\|S2 (1259\|1260) | not safe as is | the lambda closes over about 15 locals: allow-lists, `marker_to_arp`, seven counters, three antenna lists, the ATX route pair, `stations`; it also reads `sync`, `brdc_rts` and `setup.brdc_apriori`; it reads neither `synced`, the window nor `nc` (configuration through `config_.getNetworkConfig()`, 1230) |
| S2\|S3, S3\|S4 | not safe as is | S3 and S4 read the lambda's collectors and counters |
| S4\|S5 to S10\|S11 | safe | S5 reads `stations`, `sync` and configuration; S6 reads `nc`, `log_dir_`, `stations`, `closed_loop_on`, `cl`; S7 reads `stations.size()`; S8 and S9 work on setup references; S10 reads setup and configuration only, but shares the ambiguity-datum rule with S9 |

[corrected 2026-11-19: see Corrections, item 2] The order fixed by the locals: (1) lift the allow-lists; (2) lift the lambda's collectors into one tally struct and extract the lambda; (3) only then cut R, S1, S2, S3 and S4 apart. All other cuts are free, subject to the H, J, K, Q order.

A second constraint is not a local: `networkSetup_` runs before `NetworkRun` exists (`setup` at ApplicationNetwork.cpp:233-234; `NetworkRun run(...)` at 271). A builder of the form `networkX(NetworkRun&, ...)` cannot exist for these phases. The available shape is `networkX(NetworkSetupResult&, const GnssConfig&, ...)`, with `product_parser_`, `gim_`, `log_dir_` and `log_manager_` passed as arguments because they are `Application` members (decision 1).

## 5. Fallbacks and silent changes

"Logged" means a line names the change; "echo" means only a value appears; "none" means no line.

| Lines | Behaviour | Log | Review |
|---|---|---|---|
| 205-208 | Clock-datum station absent: return 1 | logged | confirmed |
| 216 | `closed_loop.enable` with an empty station name turns the held-out station off | [corrected 2026-11-19: see Corrections, item 4] none | confirmed |
| 250-253 | Phase families with streaming: throws | logged | stage 1 |
| 256-261, 272-279 | Member phase, Doppler, SNR (and code under `phase_family_codes`) read under the identity's names | warn | stage 1 |
| 295-307 | Parse clipped when `max_duration_s` > 0 | logged | confirmed |
| 309-318 | Stations that fail to parse are dropped; the run continues | warn | stage 1 |
| 330-344 | Observations of inactive systems removed | logged | stage 1 |
| 386 | Galileo health mask 0x1FF when no Galileo band is named | echo (418-419) | confirmed |
| 429-432 | Clock pivot unhealthy in the window; declaration kept | warn | stage 1 |
| 434-449 | Exclude mode removes at every time; counts only inside [t0, t1) (438) | both counts logged | confirmed |
| 548-556 | `brdc_rts` with SP3 or CLK: refused | logged | stage 1 |
| 574-590 | `sp3_brdc_clock` refuses CLK and systems other than GPS and Galileo | logged | stage 1 |
| 599 | `sp3_brdc_clock` keeps only Galileo F/NAV records | in the 609 summary | stage 1 |
| 658-663 | a00 band warning disabled when low >= high | logged | stage 1 |
| 710-762 | Latch fallbacks: the healthy record in force at the window start when a satellite has none in the window; all records when it has no healthy record | counts at 741-752 | stage 1 |
| 770-775 | GIM path under `brdc_rts`: refused | logged | stage 1 |
| 843, 900-901 | Gate threshold 0: empty allow-list, every tracked channel admitted | 913-918 | stage 1 |
| 986-996 | No eccentricity record: coordinate used as given | warn | confirmed |
| 1005-1013 | Eccentricities read in both states, applied only when armed | logged | stage 1 |
| 1073-1080 | Unmatched receiver antenna: PCO/PCV zero | warn | stage 1 |
| 1088-1097, 1264-1270 | Tier-2 exclusion; on the pivot, abort | logged | confirmed (1270) |
| 1109-1111 | Radome replaced by NONE | logged | stage 1 |
| 1147-1161 | Band routed same_system, cross_system or none (none = zero correction) | logged | stage 1 |
| 1180-1184 | MW or TurboEdit requested without phase leveling: no detector | [corrected 2026-11-19: see Corrections, item 5] none | confirmed |
| 1213-1214 | IF-TD not built on the pivot under the legacy datum | none | confirmed |
| 1286-1289 | A rover that fails to load is skipped | logged | stage 1 |
| 1492-1495, 1509, 1523 | Parallel stations, seam and carry off on small networks or one thread | only "SERIAL" (1539) | confirmed |
| 1498-1499 | `parallel_max_threads` capped at the hardware count | [corrected 2026-11-19: see Corrections, item 6] none | confirmed |
| 1521-1522 | `carry_par` relaxes byte identity, so output can depend on station count and hardware | logged at 1526 (inferred that output moves) | stage 1 |
| 1578, 1768 | `disc_mark_scope` other than "channel" read as not per-channel | none here | loader not checked |
| 1673-1688 | Broadcast-anchored or sum-to-zero datum ignores the configured satellite clock pin | none here; comment says warned at load | loader not checked |
| 1698-1735 | A priori ID replaced by the files in force in the window; all merged files named when names do not parse | logged | stage 1 |
| 1710-1711 | "+?" CLK half dropped under broadcast clock | none | stage 1 |
| 1754 | `orbit_ref` hard-coded "CoM" in every a priori mode, `brdc_rts` included | none (comment 1750-1753) | confirmed; correctness under broadcast orbits not settled |
| 1799-1804 | Stale product file removed | warn only on failure | stage 1 |
| 1838-1840 | No parser for the held-out station: truth used as configured | logged | stage 1 |
| 1848-1851 | User engine `initialize()` fails: `consumer` reset; `closed_loop_on` stays true | logged | corrected (section 1) |
| 1884-1888 | Undifferenced NL gate clamped to 0.20 m at load | logged | stage 1 |
| GnssConfigLoadNetwork.cpp about 13773-13775 | Derived user configuration arms `enable_ssr_bias_max_age` when it reads products and no value was given | none | approximate line (stage 1) |
| same function, ar_observe block | Unknown `ar_search` value falls back to "full" | none | stage 1 |

[corrected 2026-11-19: see Corrections, item 7] Failure exits are not uniform: return 1 (205, 1270, 1275 and the early phases), throw (250-253, 1458), and a reset that leaves setup returning 0 (1848-1851, 1923). The closed-loop "off" state has two flags and no single owner: ApplicationNetwork.cpp:280, EmitStore.cpp:650 and ClosedLoopRunEnd.cpp:56 test `closed_loop_on && consumer`; EmitStore.cpp:153 tests the flag alone (an LLH only).

## 6. Window-dependent and whole-file reads (input to the read-ahead inventory of package 5)

| Lines | Read | Span | Causal? |
|---|---|---|---|
| 295-298 | start offset + `max_duration_s` + 60 s | parse span; sets the synced set, hence Q and the fallback t1 | no (run length) |
| 357-361 | `max_duration_s`, or last synced epoch + 1 s | defines t1 | no (run length or file end) |
| 394-412 | [t0, t1) | flagged spans, edge count: log only | log only |
| 438 | [t0, t1) | `sat_health_removed` counter (HandoverStransform.cpp:637) | counter only |
| 434-440 | every time | exclude-mode removal of observations | removal uses the whole series (whether `unhealthyAt` holds a first step before its time is not checked) |
| 605-607 | day origin of t0; t1 | `BrdcClockApriori` table origin and end | no (window) |
| 710-711 | [t0, t1) | group-delay latch (NRF section 124; CHA part 3) | no (window) |
| 845-878 | every synced epoch (the parse span, not [t0, t1)) | channel admission (`signal_allowlist`, `phase_bias_allowlist`) | no (run length, file end and the skipped start-offset span) |
| 1706-1707, 1727-1728 | [t0, t1) | S contract product names (`aprioriIdentityForWindow`) | no (window), declaration only |
| 1490-1502 | `n_sta_total` | thread posture fixed for the run | static |
| 982-983, 1414-1418, 1829 | RINEX header (DELTA H/E/N, obs types, applied renames) | whole-file declaration | static |

The navigation records staged at M (522) cover the whole file but are released by availability time (EpochAdmission.cpp:102-105, stage 1), so they are causal.

**Coverage gates depend on run length.** The tally runs over every synced epoch (845), clipped at parse time to [file start, start offset + `max_duration_s` + 60 s] (295-298), not to [t0, t1). A short and a long run of one configuration can admit different channels; with `max_duration_s` <= 0 the tally covers the whole file. It has the shape of the latch defect of NRF section 124 (inferred: no run compared).

[corrected 2026-11-19: see Corrections, item 3] **The held-out station enters the tally.** It joins the sync roster (231); `MultiRinexSync` writes every roster station into `by_station` (MultiRinexSync.cpp:344, 358); the tally reads `by_station` with no skip (846). The held-out station's channels, read without the phase-family renames (265 skips it), and the channels of stations later dropped by the tier-2 exclusion (1092-1097) all count toward the admission fractions (`sys_n`, `code_n`, 862-869). Arming the held-out station can therefore change which channels the network stations admit (inferred, not run). Both points are order effects of a gate on which information reaches which states (invariant 7) and are fixes, not refactor steps.

## 7. Readability and ownership findings

| Finding | Lines | Verdict |
|---|---|---|
| The navigation file is parsed by up to three owners per setup (health 382; `brdc_rts` 506 or `sp3_brdc_clock` 598; latch 704-705); no single owner reads navigation records | 382, 506, 598, 704 | confirmed (corrected from four) |
| The phase-family table is recomputed three times and the `phase_family_codes` rule written twice | 246, 1398, 1594; 260-261, 1597-1598 | confirmed |
| The ambiguity-datum choice has two owners here (a third in SolveGlue per the comment at 1659-1660) | 1670-1672, 1873-1875 | confirmed (SolveGlue not re-read) |
| Emitter arguments and the contract fields that must match them are two hand-kept lists | 1572-1579; 1755, 1763-1770 | confirmed |
| `brdc_rts` is computed inside the held-out-station block | 219-229 | stage 1 |
| One concept, two names: `setup.closed_loop_on` / `nc.closed_loop.station.name` against the locals `closed_loop_on` / `cl` | 265, 277 | confirmed at 265 |
| The coverage-gate comment sits above the unrelated layout notices | 792-809, 810-833 | stage 1 |
| `buildStation` is a 297-line lambda doing eight jobs | 963-1259 | confirmed |
| The audit's line citation drifted: CHA_chain_audit_2026-11-18.md:163 cites 1585-1586 for `phase_family_codes`; the code is at 1597-1598 | | confirmed |

## 8. Step plan for a later split (not carried out)

Every step is a structural move with identical outputs, gated by a same-build comparison (section 9). Logging text stays byte for byte. Behaviour fixes (section 6 findings, new log lines for the silent changes of section 5) are kept out of the split and listed as decisions. The split waits for the native signal family step, which rewrites 240-282 (program package 12, "waits"), and for the chain-audit session to release the files of its paused step.

| Step | Move | Target file (CLAUDE.md "setup step" rule) | Unlocks |
|---|---|---|---|
| 1 | Move `signal_allowlist` and `phase_bias_allowlist` into a `NetworkCoverageGates` member of `NetworkSetupResult` | network/driver/SetupResult.hpp | cut Q\|R |
| 2 | Collect the lambda's counters, antenna lists and ATX routes into one tally struct (`NetworkStationBuildTally`) declared at R | network/assembly/StationBuild.hpp | S3, S4 read one struct |
| 3 | Extract `buildStation` as a free builder taking the station configuration, the role, `NetworkSetupResult&` and the tally; split its body into the eight S1 parts as short functions | network/assembly (station build) | cuts R\|S1, S2\|S3, S3\|S4 |
| 4 | Extract the tail phases S10, S9, S8, S7, S6, S5 as builders, last first | S10, S9, S8: driver/ApplicationNetworkSetup (sinks, user engine); S7: driver; S6: diag/RunTraces; S5: assembly/SignalRoster | short tail |
| 5 | One owner for the ambiguity-datum rule, called by S9, S10 and (after a check) SolveGlue | network/solve (datum) | removes the triple rule |
| 6 | One owner for the phase-family table and the `phase_family_codes` rule, computed once and passed to E, S5 and S8 | assembly/SignalRoster | removes 3 recomputations |
| 7 | [corrected 2026-11-19: see Corrections, item 2] Extract the head phases as builders: B and C (roster), D to I (sync load and system admission), J (`networkProcessingWindow`, the single owner of [t0, t1)), K (health), L to N (a priori), O (ionosphere and code-bias datum inputs, with the latch handed to the package 5 design), Q (coverage gates); keep H, J, K, Q in that order and say so in the spine's banner | driver/ApplicationNetworkSetup for validation; the others in the consuming subsystem's file | head of the spine |
| 8 | One navigation reader: parse once, hand records to K, M or N, and O | products or driver (decision 4) | removes 3 parsers |
| 9 | `networkSetup_` becomes a spine of builder calls with one return-code convention (decision 3) | driver/ApplicationNetworkSetup | readable setup |

Steps 1 to 3 are the only ordered ones; 4 to 8 can run in any order after 3, except that step 7's O waits on the package 5 design and step 7's E waits on the native signal family step.

## 9. Runs needed later (listed; none requested)

1. Per step: same-exe Release comparison of the three reference configurations in config/golden/ (observed data, simulated data, BeiDou) on 2 h runs, products and run-start log byte-identical against the pre-step build.
2. Branches the reference configurations do not reach, as 10-epoch smoke comparisons: `apriori_source` `brdc_rts`; `sp3_brdc_clock`; health exclude mode; phase families armed; held-out station disabled (`closed_loop.enable` false); a one-thread or small-network run (serial path).
3. Run-length check of the coverage gates: one configuration with a short and a long `max_duration_s`, comparing the KEEP/GATE lines (913-918). Setup-only output, so a smoke per arm suffices.
4. Held-out station check: one configuration with the held-out station on and off, comparing the same lines.
5. Exclude-mode order check: health exclude mode on and off, comparing the same lines.

## Decisions for the author

1. Builder shape. `networkSetup_` runs before `NetworkRun` exists (ApplicationNetwork.cpp:233-234 against 271), so `networkX(NetworkRun&, ...)` cannot apply to its phases. Options: builders over `NetworkSetupResult&` with the `Application` members passed in, or construct `NetworkRun` earlier (a larger move).
2. Coverage gates over the window. Restrict the tally to [t0, t1) (or a causal running form) instead of every parsed epoch, as a default-off key with a ledger row, after check 3.
3. Held-out station out of the tally, and tally after or before the tier-2 exclusion: decide which stations count, after check 4.
4. One navigation reader: where it lives (products or driver) and whether it holds the records for the run.
5. One return-code convention for setup failures (return 1, throw, or reset), and one owner of the closed-loop off state instead of the `closed_loop_on && consumer` pair.
6. [corrected 2026-11-19: see Corrections, item 4] [corrected 2026-11-19: see Corrections, item 5] [corrected 2026-11-19: see Corrections, item 6] Silent changes to log: the empty held-out station name (216), the Galileo mask fallback (386), the missing MW detector without leveling (1180-1184), the pivot IF-TD skip (1213-1214), the thread cap (1498-1499), the dropped "+?" CLK half (1710-1711). Adding lines changes the run log, so each is a separate step after the split.
7. `orbit_ref` "CoM" under `brdc_rts` (1754): settle whether broadcast orbits need another declaration.

## Not checked

- Whether the loader validates `disc_mark_scope`, warns about the ignored satellite clock pin (1673-1688), or refuses `sat_gf_datum` broadcast_tgd outside the no-constraint posture (GnssConfigLoadNetwork.cpp, comment near 3698); without that refusal the latch would be empty there (Sbasis.cpp:108, `Missing`).
- Whether `unhealthyAt` (ClockDatum.cpp:196, HandoverStransform.cpp:306) holds the first step before its time, as the log at 405 does; `satHealthSeries` not read.
- Whether the held-out-station skip at 265 ever fires: it matters only if that station is also listed in `nc.stations`, and whether the loader allows that was not checked.
- Whether `ProductParser::loadProducts` (454) or `ProductCache` read `max_duration_s`.
- The SolveGlue copy of the ambiguity-datum rule (named in the comment at 1659-1660).
- External line numbers marked "(stage 1)" in section 3.2 and the approximate GnssConfigLoadNetwork.cpp lines; they were not re-read for this document.
- No run supports the coverage-gate findings of section 6; they are readings of the code.
- The chain-restore window concern rests on an uncommitted file and was not assessed.

## 2026-11-19: corrections from the check against the code

The check read the network code at 7fb0737 (`D:/GnssProcessorSupport/worktrees/read_7fb0737`); `src/core/network/driver/ApplicationNetworkSetup.cpp` ("ANS" below) is byte-identical at 03dfcbc and 7fb0737, so every line number of the body inside that file holds at 7fb0737. Paths are under `src/core/`; "GCLN" is `config/GnssConfigLoadNetwork.cpp`. Of 49 claims checked, 37 are confirmed, 11 wrong and 1 has a moved line; the main finding holds. The full errata is at `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/_critical_read/SETUP_errata.md`.

1. The Outcome said that the group-delay latch and the S contract names depend on run length "not on the processing window". The code reads [t0, t1) for both: ANS:710-711, 1706-1707, 1727-1728 (tree 7fb0737), as section 6 of the body says. The Outcome above is rewritten accordingly; the body carries no marker for this item.
2. Section 3.3 (and sections 4 and 8 after it) says that phases H, J, K and Q are order-coupled. `retainSystems` and `removeObservationsIf` erase observations, never epochs (`rinex/MultiRinexSync.cpp:396-415, 417-431`, tree 7fb0737), so t0 and t1 (ANS:357-361) do not depend on H. The binding order is J before K (ANS:394-395), and H and K before Q.
3. Section 6 says that the held-out station is read without the phase-family renames because "265 skips it". The renames loop iterates `nc.stations` (ANS:264-268, tree 7fb0737); the held-out station is configured as `closed_loop.station`, so it gets no renames whether or not 265 fires.
4. Section 5 (row 216) and decision 6 say that an empty held-out station name turns the station off with no line. The loader logs an error and sets `enable` false (GCLN:13505-13510, tree 7fb0737); ANS:216 is a second guard.
5. Section 5 (row 1180-1184) and decision 6 say that a requested MW or TurboEdit detector without phase leveling is dropped with no line. `turbo_edit` without `phase_leveling` is warned at load (GCLN:12176-12179, tree 7fb0737); for `mw_detect` alone no line was found.
6. Section 5 (row 1498-1499) and decision 6 say that the cap of `parallel_max_threads` at the hardware count prints nothing. The capped count is echoed in the line at ANS:1535 (tree 7fb0737).
7. Section 5, closing paragraph, says that only EmitStore.cpp:153 tests the closed-loop flag alone. `network/runend/ClosedLoopRunEnd.cpp:49` also tests it alone (tree 7fb0737); the pair tests at ApplicationNetwork.cpp:280, EmitStore.cpp:650 and ClosedLoopRunEnd.cpp:56 are confirmed.
8. Section 2 says that the file holds four functions (`networkSetup_`, the lambda, F2 and F3) and that F2 and F3 are the two other functions. The file holds four functions: `networkValidateInputs_` (ANS:160-180, outside this map), `networkSetup_`, F2 and F3; `networkSetup_` holds four local lambdas: `consumerOnly` (817), `reportGate` (880), `buildStation` (963) and `joinNames` (1306) (tree 7fb0737).
9. Section 4 counts "A\|B through P\|Q (17 cuts)". The 17 phases A to Q give 16 cuts.
10. Section 3.1 gives the only reader of `marker_to_arp` as S1b 1005. It is also read at ANS:1013 and in S4 at ANS:1362 (tree 7fb0737).
11. Section 3.2 says that `product_cache` has no read after 574. It is read at ANS:579 as a log argument (tree 7fb0737).
12. Section 1 drops the chain-restore concern because `src/core/network/state/ChainMemory.cpp` is not in 03dfcbc. That holds at 03dfcbc but is stale: the file is committed at 7fb0737 and reads `setup.window_t0_s` / `window_t1_s` (`network/state/ChainMemory.cpp:57, 126`), a reader missing from the window row of section 3.2; it is to be checked with package 5.
13. Section 4 omits one crossing name: `joinNames`, a stateless local lambda of S3 (ANS:1306), is read in S4 (1391) and S5 (1445, 1447); it moves with them or into a helper.
14. Two points the body leaves open are settled. `disc_mark_scope` (section 5, row 1578, 1768): the loader warns on an unknown value and falls back to "sat" (GCLN:2892-2897). `unhealthyAt` (section 6, row 434-440, and the second "Not checked" bullet): it applies the first record's status before its time (`network/solve/Sbasis.cpp:208-210`), so an unhealthy first record removes the satellite from the start of the file.

### Line numbers that moved

- GCLN, derived user configuration arming `enable_ssr_bias_max_age` (section 5): "about 13773-13775" is 13804-13806 at 7fb0737 (13775 at 03dfcbc).
- Section 3.1, readers of the seven eccentricity counters: "S4 1356-1378" runs to 1385 (ANS:1356-1385).
- Every other line number inside ANS holds at 7fb0737 (the file is unchanged). The drift of the chain audit's citation (`CHA_chain_audit_2026-11-18.md:163` cites 1585-1586 for `phase_family_codes`, the code is at 1597-1598) is confirmed.

### What stands

The main finding stands on the code: the coverage tally (ANS:845-869) runs over every synced epoch, including the skipped start-offset span and the 60 s margin, and it counts the held-out station (added to the roster at 231 and written into `by_station` by `MultiRinexSync`) and the stations the tier-2 rule drops later. The effect on which channels the network stations admit is inferred, not run. The blocking locals (the allow-lists and the lambda's collectors) are confirmed; the only omission is the stateless `joinNames` (item 13). The cut order of section 4 stands with the phase order of item 2. The decisions put to the author after correction:

1. Builder shape: builders over `NetworkSetupResult&` with the `Application` members passed in, or `NetworkRun` constructed earlier. Stands as framed.
2. Coverage gates over the window, as a default-off key with a ledger row, after check 3. Stands as framed.
3. Which stations count in the tally (the held-out station; before or after the tier-2 exclusion), after check 4. Stands as framed.
4. One navigation reader, where it lives and whether it holds the records for the run. Stands as framed.
5. One return-code convention for setup failures and one owner of the closed-loop off state (the flag is tested alone at EmitStore.cpp:153 and ClosedLoopRunEnd.cpp:49). Stands as framed.
6. Silent changes to log, narrowed: the empty held-out station name (216) is removed (logged at load); the MW item becomes "the missing MW detector under `mw_detect` without leveling (1180-1184)"; the thread cap (1498-1499) is an echo, kept only if a named line is wanted. Remaining: the Galileo mask fallback (386), the pivot IF-TD skip (1213-1214), the dropped "+?" CLK half (1710-1711).
7. `orbit_ref` "CoM" under `brdc_rts` (ANS:1749-1754). Stands as framed.

### Left unconfirmed

- `NetworkSbasis::latchBroadcastGroupDelays`: the in-force and all-records fallbacks are taken from the log text at ANS:741-752, not from the rule.
- Whether `ProductParser::loadProducts` or `ProductCache::loadProducts` (ANS:454-456) read the navigation file too (the message at 457 names "broadcast nav"), which would make a fourth navigation reader; no `RinexNavParser::parse` call was found there.
- The SolveGlue copy of the ambiguity-datum rule; the loader handling of `sat_gf_datum` broadcast_tgd outside the no-constraint posture; the sum-to-zero pin warning.
- External "(stage 1)" lines of section 3.2 and section 5 other than those re-read for items 7 and 12 and the `setup.closed_loop_on`, `brdc_rts`, `setup.sync`, window and `sat_gf_target_m` rows; `aprioriIdentityForWindow`.
- No run supports the run-length and held-out station effects.

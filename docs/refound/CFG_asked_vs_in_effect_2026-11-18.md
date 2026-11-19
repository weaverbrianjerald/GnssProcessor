# CFG: what a configuration asks and what is in effect (2026-11-18)

**STATUS**

- **id:** CFG
- **topic:** config-ledger
- **status:** open
- **summary:** An audit, with no build and no engine run, of every configuration key: what the file asks and what the run holds after the loader, its validations, the service profile, the derived user-engine configuration of the held-out station and the knob snapshot. Four items differ from the file today at the held-out station (two requests replaced, one unset key armed, one request kept with no supply), in process and, through the same derivation, in replay; three keys set in every reference configuration were never read, and neither loader checks unknown keys. Section 6 plans message-first steps; the corrections narrow their coverage.
- **source:** the post-reset program of 2026-11-18, plan package 1 ("Configuration keys: asked versus in effect, full map"); commits read: 7fb0737 (branch `chain/audit`) for network code and main 5ddc422 for user-engine code; the chain audit [CHA](CHA_chain_audit_2026-11-18.md) for the form and the known Galileo pair-tree case; the key map `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/CFG_key_map.json` and the proposed change set `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/CFG_proposed_change_set.diff` (both outside the repository). Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** Of the 25 silent items the verifier re-read, 23 hold and 2 are reported by an existing line. Four differ from the file today at the held-out station: the signal rule and the no-bias code weight are replaced, the bias age gate is armed when unset, and the network ionosphere request is kept with no supply. The same derivation runs in replay, so every graded user run carries them. Two knob resolutions (V17, V19) also differ on the adopted configuration. Three keys set in every reference configuration were never read. Neither loader checks unknown keys, and no run-start line prints a resolved knob.

## Commits read and method

- **7fb0737** (`D:/GnssProcessorSupport/worktrees/read_7fb0737`, branch `chain/audit`, detached): the network loader `src/core/config/GnssConfigLoadNetwork.cpp` (LN, 14 340 lines), the posture table `src/core/config/GnssConfigNetworkPosture.cpp` (Posture), `src/core/network/driver/Knobs.cpp` (Knobs), the ledger `docs/CONFIG_promotion_ledger.json`, and all network code.
- **main 5ddc422** (`C:/GnssProcessor`): the non-network loader `src/core/config/GnssConfigLoad.cpp` (LD, 3 842 lines) and the user engine (`src/core/realtime`, `src/core/filter`, `src/core/slip`, `src/core/corrections`). The two trees diverged after their merge base e5a7e44.

Line numbers are of 7fb0737 for LN, Posture and Knobs, and of main for LD, unless the other tree is named. "The five" are the configurations of `keys_in_use.json` (356 keys set): the three reference configurations (observed data, simulator, BeiDou: written "R" below), the adopted integer-fixing day run `adopted_ad_n146` ("A") and one user-engine example `user_example` ("U"). The user-engine example runs in network mode.

Method. A scout script listed loader functions and `jval` calls by regex (`_scout/`). Sixteen readers each took one slice, opened every read site and every interlock by hand, and wrote one JSON row per key or interlock with its class (refuse, warn_change, silent_change, silent_ignore, late_override, no reader). A set comparison script matched every read form against the ledger, the posture table, the retired table and the five configurations, in both trees, and a 50-row hand sample checked it. A verifier collected the 632 silent rows, ranked them by tier (set by R or A first), merged them by mechanism and re-read the top 25 at the deciding line. This document keeps what the verifier confirmed, marks what it refuted, and labels everything below its cut as unverified. "(inferred)" marks a claim no single line shows.

## 1. The map

### 1.1 How a request travels

| stage | where | what it does to a request |
| --- | --- | --- |
| extends | LD:186-200 (resolved before any section) | merges base files; a diamond is refused with the message "CYCLE" because the seen list is never popped (LD:111-116) |
| section order | `loadFromJson` LD:202-217 | processing, estimation, filter, model corrections, receiver, files, imu, data download, app mode and real time, baseline (only in a baseline mode, LD:211), network (only in network mode, LD:214-215), logging |
| jval | `include/config/GnssConfigJsonHelpers.hpp:127-134` | the dotted key first; the flat alias only when the dotted key is absent; neither case logs; a wrong JSON type throws and the load stops |
| other read forms | LN and LD | `.value(`, `.contains(`, `operator[]`, `jhas`, `jsection`, `resolvePath`, range-for over arrays (section 5) |
| member | `GnssConfig` private members and the `NetworkConfig` sub-struct | the request as stored, possibly clamped or forced at the read site |
| validations | `validateNetwork*_` (LN:1757-1940, 10201-11765, 12083-13268) and the interlocks in `loadNetworkSection_` (LN:301-953) | refuse, warn and disarm, auto-arm, or nothing |
| service profile | `applyNetworkServiceProfile_` LN:11824-12082, called at LN:285 | arms a bundle of keys after most parse helpers and most validations |
| finalize | `finalizeNetworkProcessNoise_` LN:13649-13709, called at LN:295 | settles the process-noise provenance; values are not moved (inferred, n12) |
| posture table | `logNetworkPosture_` Posture:1383-1476, called at LN:955 | reads members as the loader left them: after every loader forcing, before the knob snapshot |
| cross-section checks | LD:228-259, then `logConfigurationSummary_` LD:262 | summary lines print stored requests |
| derived user-engine configuration | `deriveClosedLoopConsumerConfig` LN:13784-13955 (copies `*this` at LN:13785, then `applyNetworkConsumerCouplings` LN:13710-13783); called at setup, `ApplicationNetworkSetup.cpp:1819`, and in replay, `ApplicationConsumerReplay.cpp:359` | rewrites members for the held-out station; the function has no logger (comment LN:13802-13803) |
| standalone profile | `applyNetworkConsumerProfile` LN:13956-14020, called at main `ApplicationRealtime.cpp:357` | [corrected 2026-11-19: see Corrections, item 1] the same couplings for a standalone real-time user engine, with info lines (LN:13966-13981) |
| knob snapshot | `NetworkKnobs::snapshot` Knobs.cpp:24, built once in the `RunContext` constructor (`include/network/state/RunContext.hpp:2580`) | 361 assignments, 154 of them not a plain copy: resolves a request to 0 or off against a companion key, the vehicle or a parent switch |
| readers | network seams through `ctx.knobs`; the user engine through getters | the value in effect |

Only one resolved knob, `pl_local_block`, reaches the run manifest (`src/core/network/diag/RunTraces.cpp:1655-2450`, p01). No run-start line prints a resolved knob.

### 1.2 Call order of `loadNetworkSection_` (LN)

237 refuse if the section is missing; 244 `warnRetiredConfigKeys`; 257 `service_mode` read; 264 process noise; 265 roster and datum; 266 network-engine AR; 267 lane layers; 268 output and parallel; 269 streaming; 270 observability and snapshot; 271 shared-state forcings; 272 a priori and GLONASS; 273 carries; 274 slip detectors; 275 QC tiers; 276 clock model and channels; 277 datum signalling; 278 re-attach hygiene; 279 signal integrity; 280 emission filters; 281 `validateNetworkDatumCoherence_`; 282 `validateNetworkLaneLayers_`; 283 `validateNetworkPhaseDatum_`; 284 contract hardening; 285 `applyNetworkServiceProfile_`; 286 `validateNetworkCarryPosture_`; 287 `logNetworkRxExtraBandDelay_`; 288 `loadNetworkClosedLoop_`; 289 `validateNetworkUserLayoutFlags_`; 293 `loadNetworkInject_`; 294 `logNetworkSummary_`; 295 `finalizeNetworkProcessNoise_`; 301-378 refusals (smoothed hold, `sp3_brdc_clock` holds, tracked a priori, phase frame, quorum); 383 `validateNetworkFixedCycles_`; 399-953 interlocks that warn, disarm or auto-arm; 955 `logNetworkPosture_`.

Order facts the readers established:

- The interlocks at 399-953 run after `logNetworkSummary_` (294). Neither 293 nor 294 reads a flag those interlocks change (n01: grep of LN:13557-13648 and 14021-14341), so no stale summary results. The comment at LN:290-292 ("cross-knob warnings read final values") is not true of the flags settled after it.
- The streaming helper (269) tests `closed_loop.enable` (LN:2290) before `loadNetworkClosedLoop_` (288) parses it (LN:13371-13374): that warning can never fire (n02, n12).
- The service profile (285) arms keys that earlier checks already read: `phase_leveling` (LN:11887, after the slip-detector auto-arm at LN:6770), `clock_model` (LN:11885, after the notices at LN:8085, 8293 and the check at 11404), `emit_clock_disc` (LN:11879, after the rule at LN:10527-10534), `scoped_handover_response` (LN:11916, after the disarm at LN:10618-10625) (n07, n08, n10, n11).
- `validateNetworkCarryPosture_` (286) clamps `channel_mode` (LN:12683-12694) after the second-channel interlocks have tested the raw value (LN:12526, 12600) (n11).
- Within the slip helper, the corroboration check (LN:6679) reads `mw_detect` before its auto-arm (LN:6770) (n06).

### 1.3 Loader functions

The scout's literal `jval` count understates the keys of a function that reads through other forms; the reader key counts are in the last column.

**7fb0737, LN**

| lines | function | jval (scout) | keys (reader) |
| --- | --- | --- | --- |
| 234-975 | `loadNetworkSection_` (call order and interlocks) | 1 | n01: 34 keys, 34 interlocks over 234-1313 |
| 976-1103 | `loadNetworkProcessNoise_` | 0 | n01 (table-driven, 11 classes) |
| 1104-1313 | `loadNetworkRosterAndDatum_` | 7 | n01 |
| 1314-1756 | `loadNetworkProducerAr_` | 55 | n02: 118 keys, 26 interlocks over 1314-2323 |
| 1757-1781 | `validateNetworkUserLayoutFlags_` | 0 | n02 |
| 1782-1940 | `validateNetworkFixedCycles_` | 0 | n02 |
| 1941-2074 | `loadNetworkLaneLayers_` | 10 | n02 |
| 2075-2206 | `loadNetworkOutputAndParallel_` | 30 | n02 |
| 2207-2323 | `loadNetworkStreaming_` | 9 | n02 |
| 2324-3604 | `loadNetworkObservabilityAndSnapshot_` | 90 | n03: 91 keys, 25 interlocks |
| 3605-4351 | `loadNetworkSharedStateForcings_` | 23 | n04: 31 keys, 19 interlocks over 3605-4658 |
| 4352-4658 | `loadNetworkAprioriAndGlonass_` | 6 | n04 |
| 4659-5982 | `loadNetworkCarries_` | 102 | n05: 102 keys, 19 interlocks |
| 5983-6830 | `loadNetworkSlipDetectors_` | 82 | n06: 82 keys, 32 interlocks |
| 6831-8043 | `loadNetworkQcTiers_` | 96 | n07: 96 keys, 37 interlocks |
| 8044-9257 | clock model and channels, datum signalling, re-attach hygiene, signal integrity (four helpers) | 25 + 47 + 7 + 7 | n08: 88 keys, 37 interlocks |
| 9258-10200 | `loadNetworkEmissionFilters_` (ends at 10184 by n09; 10186-10200 is the head of the next function) | 64 | n09: 64 keys, 13 interlocks |
| 10201-11765 | `validateNetworkDatumCoherence_`, `validateNetworkLaneLayers_`, `validateNetworkPhaseDatum_` | 0 | n10: 0 keys, 85 interlocks |
| 11766-13268 | contract hardening, service profile, carry posture | 4 + 1 + 1 | n11: 23 key rows, 90 interlocks |
| 13269-14341 | extra-band log, closed loop, summary, finalize, couplings, derivation, standalone profile, inject | 0 (all `.value()` / `operator[]`) | n12: 59 keys, 27 interlocks |
| Posture:1383-1476 and Knobs.cpp | posture table, knob snapshot | 0 | p01: 134 posture rows, 154 non-plain knobs, 6 interlocks |

**main, LD**

| lines | function | jval (scout) | keys (reader) |
| --- | --- | --- | --- |
| 173-273 | `loadFromJson` | 0 | g01 |
| 274-1218 | `loadProcessingSection_` (identical to 7fb0737 272-1217, shifted one line) | 91 | g01: 96 keys, 9 interlocks |
| 1219-1257 | `loadRxSatCodeBiasKeys_` (main only) | 2 | g02 |
| 1258-2185 | `loadEstimationSection_` | 137 | g02: 145 keys, 17 interlocks |
| 2186-3842 | filter, model corrections, receiver, files, imu, data download, app mode and real time, baseline, logging, summary | 3 + 0 + 9 + 10 + 8 + 0 + 0 + 1 + 0 + 0 | g03: 281 keys, 21 interlocks |

## 2. Identities

A key has up to seven identities. The table names each, its owner, and what the readers found about it.

| identity | owner | finding |
| --- | --- | --- |
| dotted key | the `jval` (or other) call in LN or LD | 1 326 distinct keys read in 7fb0737, 1 325 in main; 1 243 read for a value in both (set comparison) |
| flat alias | the same call | three styles: bare suffix (`carry_zwd`, `mdb`, `frame_id`), `network_<name>`, `network_cycle_slip_<name>`; the fixed-cycle keys have none, nor `estimate_tropo_gradients` (on purpose, LN:3768). No alias equals another key or serves two keys. Precedence is silent (section 3, row R2) |
| member | `GnssConfig` / `NetworkConfig` | several member names differ from the key: `gap_reentry_relevel` fills `gap_reentry`; `mw_lli_*` fill `lli_*`; `if_phase_td_ref_*` fill `ref_*`; `emit_pb_hold_trip_generation_trigger` fills `emit_pb_hold_trip_trigger`; `rhs_attrib_columns` fills `network_rhs_attrib_columns_` |
| getter | `include/config/GnssConfig.hpp` | the getter can be the only reader of a value that has no effect: `getLeapSeconds` has no caller; `enableIfSlipDetector` is never called |
| ledger row | `docs/CONFIG_promotion_ledger.json` (784 rows) | 528 loader keys have no row in either tree, 24 of them leaf keys of the network loader (the set comparison said 22; the sample found 2 more, section 5). Every non-retired row is read by some loader line |
| posture row | `GnssConfigNetworkPosture.cpp` (7fb0737: 25 ON, 109 SCOPED, 8 bundle members) | reads the member before the knob snapshot, so a row can count as armed a knob the snapshot resolves off; prints only a failing row |
| knob member | `NetworkKnobs` (`include/network/state/RunContext.hpp`), filled in Knobs.cpp | the value in effect for network seams; owned by Knobs; reported nowhere except `pl_local_block` |

Where one request has two deciders (one key, two owners of its value in effect):

- **Loader and knob snapshot.** `phase_datum_prioritized` (Knobs.cpp:401 forces true under the eq34 chain), `amb_float_drop_jump_k`, `_gap_cover`, `_declared_var_m2`, `_decouple_always` (Knobs.cpp:58, 72-74), `zwd_event_inflate_m` (Knobs.cpp:174-179), `phase_datum_handover_max_var_m2` (Knobs.cpp:445-448), `suspect_epochs` (Knobs.cpp:326-327), `raim_pool_data_rows_only` (Knobs.cpp:371-372 ORs it with the prior channel).
- **Loader and derived user-engine configuration.** `canonical_signal_rule`, `no_bia_code_downweight`, `enable_ssr_bias_max_age`, `channel_mode`, `between_satellite_sd.enabled`, `ambiguity_resolution.mode`, `.wl_only`, `.active_systems`, `ar_ignore_mw_wl` (LN:13742-13950; `ApplicationNetworkSetup.cpp:1856`).
- **Two reads of one key.** `network.apriori_source` (LN:4373 and 3022), `carry_coupling_exact_update` (LN:4916 and a re-read in n03's slice), `rhs_attrib_on_trip` (LN:2383 for an info text, member at 2507), `ura_staleness_q2` (LN:8256 and, under service mode only, 12038 with an expression default), `network.estimate_glonass` (LN:4497, and a peek in every mode at LD:1534/1575), `processing.mw_signals` (twice in LD). All pairs carry the same default text (set comparison).
- **Two loaders in two modes.** User-engine keys spelled `network.*` (`consume_ura`, `consume_phase_bias_sigma`, `ura_staleness_q2`, `require_signal_integer_for_ar`, `trace_ssr_consumer`, `iono_supply_formal_sigma`) are parsed only inside `loadNetworkSection_`, which runs only in network mode (LD:213-214), while their readers sit in the user engine (n03, n08).

## 3. The rule table

One row per interlock pattern. Class: a refused loudly, b changed loudly, c changed silently, d ignored silently, e overridden later without a line. Classes c, d and e are the defects of section 4.

| # | condition | action | message, level | class |
| --- | --- | --- | --- | --- |
| R1 | network mode without a `network` object | load refused | LN:238, error | a |
| R2 | dotted key and flat alias both present | dotted wins | none (`GnssConfigJsonHelpers.hpp:127-134`) | c |
| R3 | a key no loader line reads (misspelt, set-but-unread, or from the other tree) | ignored | none; only retired keys are checked (LN:244) | d |
| R4 | a retired key present | ignored | `warnRetiredConfigKeys`, warn (150 retired rows) | b |
| R5 | unknown `network.process_noise` class name | load refused | LN:1052, error | a |
| R6 | `network.process_noise` class set while its gate is off | stored, no effect; manifest prints it as `[table]` | none (gate column `GnssConfigTypes.hpp:1612-1622` unused) | d |
| R7 | out-of-range number | most: clamped with a warn; some: clamped or replaced with no line (LN:1336-1338, 2150, 2156, 2190, 6856, 7311, 7350-7353, 7567, 7734, 7746, 7870, 8917-8918, 9084; LD:1303, 1317, 1627, 1636, 1646, 1697, 1720, 1724, 1768, 1772, 1810-1817, 1843-1845, 1890-1935; Knobs.cpp:93, 536-643) | warn, or none | b or c |
| R8 | dependent key without its master switch | ring fix: refused (LN:1533-1712); sd_ewl and a few others: disarmed with a warn (LN:1352, 1975); most: stored and inert | error / warn / none | a, b or d |
| R9 | W6 code-bias estimation (`estimate_sat_code_bias`) | forces `estimate_ifb`, `estimate_sat_biases`, ionosphere weighting, `enable_code_phase_bias`, `promote_sat_code_bias` (LN:3943-3971) and `estimate_phase_clock` (LN:3974) | info LN:3975 names all but `estimate_phase_clock` | b; c for `estimate_phase_clock` |
| R10 | `service_mode` | arms a bundle; explicit false loses with a warn (LN:11866); absent is armed with only the bundle summary (LN:12054); explicit non-zero numerics win (LN:11872) | warn / info | b; e for checks that ran before it (section 1.2) |
| R11 | closed loop enabled | the held-out station's configuration is derived from the file and rewritten (LN:13710-13955) | [corrected 2026-11-19: see Corrections, item 1] none (no logger); the standalone path logs the same arming (LN:13966-13981) | c, e |
| R12 | knob snapshot resolves a request to off or 0 | Knobs.cpp:58, 72-74, 179, 220, 254-257, 265, 298, 326, 374-378, 383-385, 401, 414, 434-437, 447-448, 471-477, 502-503 | none at resolution; some loader lines state the effect unconditionally (LN:4759, 4780, 4795, 5166) | e |
| R13 | posture regime and armed test | reads members before R12; prints a failing row only (Posture:1417-1418) | warn (ON) / info (SCOPED) | b; misses e |
| R14 | `channel_mode: all` on a posture the clamp refuses | clamped to single after the second-channel interlocks ran on the raw value (LN:12526, 12600, 12683-12694) | error for the clamp only (LN:12687) | c |
| R15 | `mw_detect` without `phase_leveling` | detector never built (`ApplicationNetworkSetup.cpp:1180-1187`) | warn only for `turbo_edit` (LN:12176) | d |
| R16 | an inert-warning tests "differs from the default" instead of presence | silent when the request equals the default (LN:12300-12302, 11249) | none | d |
| R17 | enum string not matched | three behaviours: error and fallback (LN:3703, 3730, 4705, 7253, 9601), throw (LN:9816), warn and fallback (LN:9792); exact-match fallback with no line (LD:3150-3155 `app_mode`, LD:3036-3042 `imu.coupling_mode`, `ApplicationRealtime.cpp:477` `realtime.input_format`) | error / throw / warn / none | a, b or c |
| R18 | a non-array or non-object where one is expected | dropped (LN:1008, 1041, 1144, 13501; LD:400, 1087, 1107, 1122, 1230, 2149, 2969-2971) | none | c or d |
| R19 | a run-time fallback (GPT3 grid, OTL source, resolved date) | fallback taken | on a component logger that all five switch off after the load (main `Application.cpp:84-99`, `LogManager.cpp:326-335`) | d |
| R20 | a user-engine key spelled `network.*` in a non-network run | never parsed (LD:213-214) | none | d |
| R21 | an error-level check that does not stop the load | `clock_anchor_q <= 0` (LN:9664), the anchor under `clock_model` (LN:11979-11980; Knobs.cpp:265 resolves it off) | error | b |

## 4. Defects by priority

### 4.1 Confirmed by the verifier

Numbers V1 to V25 are the verifier's. "Today" means the request differs from what is in effect on at least one of the five. Set by: R = all three reference configurations, A = adopted, U = user example.

**Priority 1: requests changed today on the held-out station (rule R11).** Every arm is graded on this station.

| id | key | deciding path:line | failing scenario | set by |
| --- | --- | --- | --- | --- |
| V4 | `network.closed_loop.consume_network_iono` (true) | 7fb0737 `src/core/network/emit/EmitStore.cpp:663` needs `sh_model`, built only under `network.estimate_iono_sh` (`ApplicationNetworkSetup.cpp:642`); the info line at `ApplicationNetworkSetup.cpp:1912` appends "+ SH-VTEC slant iono" | a file asking the network ionosphere on the held-out station gets a user engine that floats its own ionosphere, and its log suggests the supply is present; the ledger row is KEEP_OFF, default false | R, A, U |
| V5 | `processing.canonical_signal_rule` ("priority") | LN:13908 pins Count, LN:13914 clears the station scope, LN:13920 pins code-follows-phase | a file asking priority gets priority in the network engine and count at the held-out station; a standalone user engine of the same file runs priority (`PppModel.cpp:615`), so the two user-engine paths diverge although the comment at LN:13712-13714 says they cannot | R, A, U |
| V6 | `processing.enable_ssr_bias_max_age` (unset) | LN:13804-13806 arms it; reader main `MeasurementModelRow.cpp:64`; the configuration echo the comment cites does not exist | a file leaving the key unset gets a 30 s bias age gate with a 2x hold at the held-out station, and only the user-engine state fingerprint shows it | none set it (all five consume network products) |
| V7 | `estimation.no_bia_code_downweight` (1.0) | LN:13748 forces 100; with `estimate_sat_biases` false (LN:13742), main `MeasurementModelVariance.cpp:222-237` applies it | a file asking 1.0 gets 100 at the held-out station, and nothing says so | R, A, U |

**Priority 2: the known Galileo pair-tree case.**

| id | key | deciding path:line | failing scenario | set by |
| --- | --- | --- | --- | --- |
| V11 | `network.phase_datum_eq34_family` "pair" with `network.ring_fix.pairs` | 7fb0737 `ChainDatum.cpp:557-564` elects per signal when a constellation tracks more than two phase signals and no family pair is listed; a per-constellation line exists only when `phase_datum_eq34_family_pairs` is set (`:603`, `:622`); otherwise one aggregate count at run end (`:3116-3117`); the only cross-check (LN:11034-11043) runs one way, chain pairs against ring pairs | a file asking the pair family with `E:L1C/L5Q` in `ring_fix.pairs` gets Galileo elected per signal, and no line names the constellation | A |

This is the owed line of the chain audit, part 2 (note of 2026-11-18, lines 169-175), confirmed where it falls (n02, n08, n10, verifier), not a new finding. Whether `phase_families` in the adopted run leaves Galileo with two signals was not settled: no run was read. The chain audit part 2 reports Galileo elected per signal in the adopted configuration.

**Priority 3: keys set in every reference configuration and read by no loader line (rule R3).**

| id | key | deciding path:line | failing scenario | set by |
| --- | --- | --- | --- | --- |
| V1 | `network.redundancy_census` (true) | Knobs.cpp:520 arms the check on `redundancy_census_stride > 0` or `redundancy_escalate_sigma > 0` only; not read, not retired | a file asking false with stride 10 keeps the check and its dependents (MDB, VCF including `vcf_apply`, which moves weights, block-Baarda, congruence; Knobs.cpp:534-606) on; `scripts/loader_keys.py:423` already lists it inert | R, A, U |
| V2 | `network.max_duration_s` (21600 or 86400) | only `processing.max_duration_s` is read (LD:582 in 7fb0737) | a file changing only the network spelling changes nothing; today both spellings hold equal values | R, A, U |
| V3 | `network.min_stations` (10) | no reader in either tree; only `streaming.min_stations` (LN:2220) and `parallel_min_stations` (LN:2099) | a file asking a 10-station floor gets none | R, A, U |

All three come from the base configuration `config/refound/field_gps_dual_rf0hv_golden_2026-09-22.json` (lines 285, 441, 447).

**Priority 4: stored requests with no reader, echoed as if in effect (rule R3, no-reader form).**

| id | key | deciding path:line | failing scenario | set by |
| --- | --- | --- | --- | --- |
| V13 | `processing.satellite_interpolation_mode` | getter used only in the config sources; LD:507 echoes it | a file asking hermite gets the fixed SP3 interpolation, and the log prints hermite | R, A, U ("lagrange") |
| V14 | `processing.leap_seconds_tai` | `getLeapSeconds` has no caller; time uses `TimeUtils::LEAP_SECONDS` (`TimeUtils.hpp:598`) or `realtime.leap_seconds` (main `RealTimeEngineInit.cpp:40`); LD:3790 echoes it | a file asking 38 gets 37, and the summary prints 38 | R, A, U (37) |
| V15 | `filter.process_noise.clock_bias_q` in the network engine | Knobs.cpp:26-28 reads only the satellite q values; comments at LN:5185 and `GnssConfig.hpp:3042` say it is inert | a file asking a receiver-clock q gets a free per-epoch clock column in the network engine; it acts in the user engine | R, A, U (1000) |
| V16 | `logging.levels.network` | LD:3768-3772 accepts any logger name; no logger named "network" exists | a file setting this level changes nothing | R, A, U |

**Priority 5: announced before resolved (rules R12, R16).** Cosmetic or latent on the five.

| id | key | deciding path:line | failing scenario | set by |
| --- | --- | --- | --- | --- |
| V17 | `network.phase_datum_handover_max_var_m2` (0.01) | Knobs.cpp:445-448 zeroes it without the continuous handover; the ratio sibling warns (LN:11338), this key does not | a file asking a 0.01 m^2 ceiling under the eq34 chain gets none | R, A, U |
| V18 | `phase_anchor_min_age_ep`, `phase_anchor_flag_quiet_ep` | LN:12300-12302 warns only when values differ from 10 / 5; read only under the held basis (`PhaseAnchorEpoch.cpp:215-217`) | a file asking these gates without the held basis, at the default values, gets nothing | A |
| V19 | `network.emit_pb_hold_trip_generation_trigger` | stored LN:2949-2958, resolved Knobs.cpp:254-257; the only warn (LN:2984) needs trip generation on | a file asking `clock_event` with the hold off gets no trigger | R, A, U |
| V20 | `network.cb_kaula_sigma_m` (3.0, prior off) | checked only with the prior on (LN:10001); readers under the prior switch (`BiasAdmission.cpp:166, 213, 255`) | the value reads as active and is not; any value passes | R, A, U |
| V21 | `network.zwd_event_inflate_m` (0.02) | the warn at LN:5165-5168 states the effect unconditionally; Knobs.cpp:174-179 zeroes it without the in-block wet delay, the live block, conditional coupling and float_kf | a file asking 0.02 without the in-block wet delay keeps the erase response while the log says the wet delay is kept with inflated sigma; the posture row (Posture:438) counts it armed on `ring_fix.enable` alone | A |
| V22 | `amb_float_drop_jump_k`, `_gap_cover`, `_declared_var_m2`, `_decouple_always` | Knobs.cpp:58, 72-74 resolve them off unless float_kf; a second gate at `PassASlipScan.cpp:775-780`; the warns at LN:4759, 4780, 4795 state the effect unconditionally; posture rows 343, 350 count them armed | a ring-fix file on the level vehicle (the ring fix needs eq34, not float_kf, LN:11228) gets neither cover while the loader and the posture count say it is in force | A (jump_k 1.0, gap_cover free); R, A, U (decouple_always true) |

**Priority 6: couplings across sections, and other latent rows.**

| id | key | deciding path:line | failing scenario | set by |
| --- | --- | --- | --- | --- |
| V9 | `estimation.estimate_phase_clock` | LN:3974 forces false; the W6 line LN:3975 omits it; the refusal at LN:1757-1762 returns early under closed loop and runs after the force | a closed-loop file asking true gets no phase clock, and nothing says so | R, A, U (false) |
| V10 | `processing.channel_mode` at the held-out station | LN:13846 replaces it with `network.closed_loop.channel_mode` | a file asking "all" gets "single" at the held-out station unless the closed-loop key also says "all" | R, A, U ("single") |
| V23 | `filter.initial_std.sat_clock` (1000) | `GnssConfig.hpp:2312-2317` makes it the sp3 clock-anchor sigma when `apriori_clock_anchor_sigma_m` is 0; below 100 m `ClockAnchor.cpp:1357` folds a per-satellite prior | a file tightening the initial satellite-clock sigma below 100 m also gets a per-epoch clock anchor | R, A, U |
| V24 | `data_download.year`/`doy` against `files.observation_path` | main `PathResolver.cpp:260-278`: a pinned path wins; the only line is on the "resolver" logger, which is off (rule R19) | a file asking DOY 146 with a path pinned to 2024-129 gets DOY 129 | R, A, U |
| V25 | `network.cycle_slip.mw_detect` without `phase_leveling` | `ApplicationNetworkSetup.cpp:1180-1187`; the off branch warns only for `turbo_edit` (LN:12176); six rider interlocks test the member, not the detector | a file asking `mw_detect` with `phase_leveling` false gets no MW or LLI slip detection at any station | none (all five arm `phase_leveling`) |

### 4.2 Refuted by the verifier

- **V8, `estimation.estimate_ifb` at the held-out station** (ranked silent by n04 row 1 and g02 row 3). NOT SILENT: the info line at LN:3975-3976 names the forced `estimate_ifb`; the derivation copies the member (LN:13785) as LN:13961-13963 intends. The disagreement stands in part: the line speaks of the network engine, and no line says the held-out station inherits it. A comment at main `RealTimeEngineSsrBreaks.cpp:861` says the closed-loop user engine inherits true, which matches; n12 reads the file value (false) as inherited, which does not (inferred mismatch, unresolved).
- **V12, `network.phase_datum_prioritized` false under eq34** (ranked silent by n08 row 2 and p01 row 4). NOT SILENT: the unconditional eq34 banner (LN:10981-10983, text checked) says "SUPERSEDES network.phase_datum_prioritized". n08's reading that the SUPERSEDES line at LN:11120 prints only when the key is true is correct for that line, but the banner covers the case. The posture rows that test the raw member (`anchor_slip_handover` Posture:140, `trace_phase_anchor`, `phase_datum_min_stations`) still fall out of regime on the adopted configuration without a line (p01).

### 4.3 No reader (confirmed by grep in both trees, verifier section 3, unless marked)

- `estimation.promote_sat_code_bias`: the member is only written (LN:3636, 3944, 13727); the W6 line (LN:3976) announces a force with no effect.
- `baseline.use_doppler`: LD:3625 says Doppler double differences will be formed.
- `realtime.output_type`, `realtime.output_path` (and `realtime.max_epoch_buffer`, g03, not re-checked).
- `logging.enable_if_slip_trace`, `logging.enable_gf_slip_trace`: the network IF-slip trace opens on `network.trace_slip_events` (`RunTraces.cpp:232`).
- `NetworkKnobs::prires_s0g_redun`: written at Knobs.cpp:579, never read.
- From the reader rows, not re-checked by the verifier: `processing.enable_noncausal_slip_bridge` and its three tuning keys (the summary prints `nc_bridge=on`, LD:3802); `estimation.enable_if_slip_detector` (getter never called; survives as the default of `cycle_slip.if`, LD:1332); `receiver.antenna_orientation.elevation_deg` (only azimuth is read, `Antenna.cpp:960`); `logging.enable_smoother`, `logging.enable_turbo_edit_resolver` (read, not applied: missing from `all_loggers`, main `Application.cpp:84-92`, so a request of false leaves the logger on); `network.brdc_select` (one valid value; read only by the snapshot fingerprint); `network.apriori_clock_track_phase_clock_hold` (a load-time relaxation and a run-end line only).
- By design: `network.use_sif_coordinates` (applied in the loader, LN:1191-1263), `network.coord_age_warn_years` (load-time advisory), `network.ambiguity_resolution.fixed_cycles.emit_measurement_only` (an acknowledgement the fold check requires, LN:1862).

### 4.4 Silent rows below the verifier's cut (unverified)

588 silent rows lie below the cut: 85 set by a reference or the adopted configuration (mostly knob derivations that change no request, and temporal-anchor sub-keys that are in effect because the anchor is armed on all five), 76 integer-fixing and chain keys, 1 set by the user example only, 426 set by none. The rows below were ranked high by their readers; none was re-read by the verifier. None fires on the five unless stated.

| key or mechanism | path:line (reader) | failing scenario |
| --- | --- | --- |
| `signal_min_station_fraction`, `phase_bias_min_station_fraction` above 1 | LN:6858-6861; `ApplicationNetworkSetup.cpp:873-904, 1040`; `PppModel.hpp:536-541` (n07) | a file asking 50 (a percentage) gets every channel admitted while the gate's line prints every code under GATE |
| streaming with closed loop | LN:2290 against LN:288; `StreamFrontEnd.cpp:149` (n02, n12) | the warning cannot fire and `closed_loop.station.transport` is never parsed, so the held-out station is absent from every streamed epoch (inferred) |
| `second_phase_channel` with `channel_mode: all` clamped | LN:12600, 12615, 12683-12694; `GnssConfig.hpp:4589` (n11) | a zero-q level per added arc with no event response (invariant 1 of CLAUDE.md), only the clamp error appears |
| `second_code_channel`, `second_code_system_aware`, same clamp | LN:12526-12580 (n11) | skipped interlocks and a false "IGNORED" line; R, A, U set both keys, no harm on the five |
| `service_mode` order | LN:11879, 11885, 11887, 11916 against LN:6770, 8293, 10527-10534, 10618-10625 (n07, n10, n11) | a zero-q carry with no slip detector; clock bounds skipped; the T3c clock-disc rule bypassed; a transform left off while its prerequisite is armed |
| `raim_we_code_iono` forces `raim`, `raim_attribution` | LN:7201-7204 (n07) | a file asking `raim` false gets it true; the warn names only `raim_within_epoch` |
| `network.process_noise.<class>` with its gate off | LN:1036; manifest `RunTraces.cpp:2440` (n01) | the class is reported `[table]` as if in effect |
| `estimation.rx_sat_code_bias_systems` one-letter names (main only) | main LD:1230-1254; `MeasurementModelStates.cpp:1089-1090` (g02) | `["G","E"]` matches no satellite while the info line prints "systems G, E" |
| `estimation.ambiguity_resolution.active_systems` names | main LD:2147-2153; `AmbiguityResolver.cpp:439-442` (g02) | `["G","E"]` excludes the named constellations from fixing |
| `ratio_threshold` above `ratio_upper_bound` | main `IlsCore.cpp:326, 406-409` (g02) | no ratio can pass; U sets 1e9 against 1e4 (perhaps deliberate) |
| `estimation.ambiguity_resolution.enable_partial_ar` | main LD:2026; `AmbiguityResolver.cpp:861` (g02) | partial fixing, ruled off, is stored and run with only a build-time info line (loud, not guarded) |
| `stuck_code_bias.enabled` without `residual_history.enabled` | main `ResidualHistoryTracker.cpp:211`; LD:1725-1730 (g02) | the default-on detector never runs while the log says it is on |
| an active system with no priority list (other than Galileo, GLONASS) | LN:4517, 4592; `PppModel.cpp:568` (n04) | BeiDou without a list is dropped |
| `state_snapshot_require` / `_keep_history` without a path | LN:3414, 3418; `Run.cpp:312` (n03) | the run starts cold while the log says resume is required, or history is on |
| `trace_postfit_sigma0` without `trace_epoch_status` | `RunTraces.cpp:451, 1266` (n03) | no sigma0_pf output; R, A, U arm both today |
| `user-engine network.* keys` outside network mode | LD:213-214 (n03, n08) | a standalone real-time file asking `consume_ura` gets the default |
| `inject.events[].station`, `.signal` on `sat_clock_step`, `anchor_handover` | LN:14165, 14193; `Inject.cpp:522-525, 643-647` (n12) | a step meant for one station and signal hits all of them |
| closed-loop tuning keys of the held-out station | LN:13854-13898 (n12, g01) | an unknown mode string falls back to "act" (writes state) with no line; processing tuning silently replaced |
| `network.stations[].position_ecef` malformed under SIF | LN:1113, 1256 (n01) | the SIF coordinate is used and nothing says the explicit one was rejected |
| `ambiguity_resolution.block_search` without `decorrelated_partial` | LN:1341; `ProducerAr.cpp:893, 1034` (n02) | the joint search runs |
| `trace_dd_fractional_max_pairs` below 0 | LN:2150 (n02) | -1 becomes 0, which means unlimited |
| `clock_frame_leash_m` at or below 0 | `ClockEmitRebase.cpp:77`; LN:9442 (n09) | no leash at all while the banner describes one |
| `phase_datum_eq34_receiver` matching no station | `ChainDatum.cpp:808-811` (n08) | the elected root is used |
| `signal_integrity.resume_warmup_vet`, `eq34_rpb_prior_birth_only` | Knobs.cpp:374-378, 502-503; LN:8487, 9182 (n08) | resolved off while an info line says on |
| `carry_vce_components` misspelt | LN:5789, 5916 (n05) | the monitor drops the family |
| `processing.canonical_signal_priority_stations` as a string | LD:400 (g01) | a narrowing request becomes all stations |
| `files.product_list_path` | main `Application.cpp:170-175`; `PathResolver.cpp:335-337` (g03) | a curated list is dropped whenever the resolver finds any product |
| `app_mode`, `imu.coupling_mode`, `realtime.input_format` miscased | rule R17 (g03) | "RealTime" runs post-processing; "SBF" runs RTCM3 |
| `processing.gnss_rate_s` in a network file | LD:575-582; LN:2188 (g01) | the log says epochs will be decimated; they are not |

Loud but worth naming: the adopted configuration asks `cycle_slip.anchor_slip_handover` true with eq34 and `phase_datum_prioritized` false, so it is disarmed with a warn (LN:415-422); it asks `ambiguity_resolution.suspect_epochs` 10 without the search, warned INERT (LN:1352-1353) and zeroed (Knobs.cpp:326-327). The posture row for `ambiguity_vehicle` (Posture:1051) counts armed only with float_kf and the held basis, so the adopted run prints "POSTURE OFF [ON]" at warn while the float vehicle runs (inferred). A warn line in LN is used both for a statement of effect and for an inert notice (n05), so a warn proves neither.

### 4.5 Patterns behind the confirmed rows

1. **The derived user-engine configuration rewrites the file without a logger** (V4 to V7, V9, V10; rule R11). V4 is the reverse case: a request carried to a station whose supply never exists.
2. **A key is announced before it is resolved** (V17 to V22, V25; rules R12, R13, R15, R16). The posture table runs before the knob snapshot, and no run-start line prints a resolved knob.
3. **Neither loader checks unknown or unread keys** (V1 to V3, V13 to V16, section 4.3; rule R3).
4. **Run-time fallbacks log on loggers the configurations switch off** (V24; GPT3 and OTL fallbacks, g03; rule R19).

## 5. The set comparison

Script `_scratch/sets/sets.py`, result `sets.json`, findings `sets.md`; hand sample `sets_sample.md`.

| count | 7fb0737 | main |
| --- | --- | --- |
| loader read sites | 1 527 | 1 526 |
| of them jval / jhas / jsection / resolvePath | 933 / 39 / 30 / 23 | 931 / 38 / 30 / 23 |
| of them `.value` / `.contains` / `operator[]` / `.at` / `.find` | 319 / 75 / 76 / 4 / 2 | 321 / 75 / 76 / 4 / 2 |
| distinct keys read / read for a value | 1 326 / 1 243 | 1 325 / 1 243 |
| ledger rows / retired rows | 784 / 150 | 784 / 150 |
| loader keys with no ledger row | 528 | 528 |
| of them network-loader leaf keys (corrected by the sample) | 24 | 24 |
| ledger rows read by no loader line (not retired) | 0 | 0 |
| ledger default differs from the loader default (over 769 comparable rows) | 0 | 0 |
| aliases colliding | 0 | 0 |
| keys read in only one tree (corrected: array forms folded) | 14 | 14 |
| retired keys still read, or set by a configuration | 0 | 0 |
| set by a configuration, read by no loader line, not retired | 7 | 6 |
| of them ignored in the tree that owns them | 3 | 3 |

**Main rows.** main does not read three keys the adopted configuration sets: `network.phase_families`, `network.phase_family_codes`, `network.ring_fix.entry_revalidate`; a main build given the adopted configuration drops all three without a line. 7fb0737 does not read four user-engine keys the user example sets (`estimation.ambiguity_resolution.hold_release_cyc`, `nl_sigma_floor_cyc`, `refloat_partner_signal`, `wl_from_float`), nor `network.trace_live_record`, which main's reference configurations pin (main `config/golden/golden_field_refound.json:956`). Other one-tree keys: 7fb0737 has the eq34 family pairs, chain restore and capture, `rx_extra_band_delay_q` and eight `ring_fix.nn_*` / `entry_revalidate` / `held_first_by_life` keys; main has eight AR keys and the receiver-satellite code-bias keys. `loadProcessingSection_` is identical in both trees.

[corrected 2026-11-19: see Corrections, item 6] **Other rows.** The 22 (now 24) network leaf keys without a ledger row are the 11 classes of the `network.process_noise` table (the ledger holds only their legacy spellings), five legacy keys the network loader probes, the station and closed-loop `position_ecef`, the four inject event coordinates, and (added by the sample) `rhs_attrib_columns` and `closed_loop.ar_active_systems`. Five RECOMMEND_SCOPED rows have no posture row; each has regime "False", exempt by `scripts/config_posture_check.py:341-346`, not a defect. Two posture rows of the SH bundle (`ionosphere_weighting.enable`, `.elevation_cutoff_deg`, Posture:1346-1347) carry ledger verdict KEEP_OFF: a run that arms SH is told to arm a KEEP_OFF key. Sixteen ledger rows have a blank default, all outside the network section.

**The sample's error rate.** 50 rows drawn with seed 20261118 across every class, small classes whole: 50 of 50 confirmed at row level. Four systematic defects of the script: D1 a scalar array read by range-for is filed as a container (the leaf count is 24, not 22); D2 15 ledger rows (arrays and objects, `processing.active_systems`, the per-constellation priority rows) carry no comparable default, so "no default differs" holds for 769 of 784 rows and `network.ring_fix.pairs`, which carries the Galileo case, is among the 15; D3 array-element forms counted as keys (14 one-tree keys in each tree, not 16 and 15); D4 a ledger row covered only by a wildcard read is not told apart from an exact read.

**The script's limits.** Unresolved read sites: 0; three C++ container lookups excluded; nine (main ten) wildcard reads whose children cannot be listed from the text. Not covered: reads outside the two loader files (`GnssConfig.cpp`, `RetiredConfigKeys.cpp`, the simulator's configuration), anything after the read (classes c to e), the extends merge, array elements below the first level, wrong-type values. Matching is textual. The set comparison cannot see the Galileo pair-tree case: `ring_fix.pairs` is read and stored.

## 6. Plan in identical-output steps

[corrected 2026-11-19: see Corrections, item 2] Each step fixes a pattern, not a row. Products and traces stay byte-identical for every configuration that did not rely on a silent behaviour; only log text grows. The proof needs no engine run: plan package 17, a dump of every member and every resolved knob for every configuration file in `config/` (and the five), compared between the build before and after the step, plus a diff of the load log that may add lines only. A step that would change a member for any file stops and goes to the author.

| step | what changes | why output is identical | proof | ruling |
| --- | --- | --- | --- | --- |
| C1 | [corrected 2026-11-19: see Corrections, item 1] The derived user-engine configuration reports itself: `deriveClosedLoopConsumerConfig` and `applyNetworkConsumerCouplings` return a list of (key, asked, in effect, reason) that setup and replay log at info, the same list the standalone profile already logs | messages only; members unchanged | member dump of the derived configuration identical; log adds lines | no |
| C2 | Asked against in effect for the knob snapshot: after `NetworkKnobs::snapshot`, one line per non-plain knob whose resolved value differs from its request; the posture table gains an "in effect" reading (or runs after the snapshot); the run manifest writes both | messages and a manifest column; knobs unchanged | knob dump identical; manifest column added | no (manifest format: author informed) |
| C3 | Unknown and unread keys: a load-end walk of the file against the generated set of read keys (the set comparison's output, kept as a checked table), one warn per key no loader line reads; a `processing.strict_config` flag, default off, turns the warns into a refusal | warn only by default | [corrected 2026-11-19: see Corrections, item 7] member dump identical; the five produce exactly the warns for V1 to V3 and the tree-divergence keys | yes: the flag and its default |
| C4 | Keys with no reader become retired keys (warned by the existing table) or gain their reader; the echo lines of V13, V14, `nc_bridge`, `use_doppler` stop printing the request as in effect | a retired key has no reader by definition | member dump identical except removed members with no reader | yes: retire or give a reader, per key |
| C5 | Alias precedence: `jval` records when the dotted key and the alias are both present and differ; the loader logs the list once | message only | identical members | no |
| C6 | One helper for clamps and fallbacks (`clampLogged`, `enumLogged`) replacing every silent clamp and silent enum fallback of rule R7, R17 and R18 with a warn naming the key, the request and the value kept | messages only; the clamp and fallback values are unchanged | identical members; a unit test per helper | no |
| C7 | Companion requirements as data: a declared table (key, requires, inert when) that the loader evaluates after the last forcing step, emitting one inert warn per row; generated from new ledger columns; it replaces the hand-written inert lists (LN:11257-11287 and the like) and the default-value tests (LN:12300, 11249) | messages only | identical members; table checked by `scripts/docs_record.py verify` | yes: ledger schema |
| C8 | Order: every check that reads a flag a later step changes (section 1.2) is re-evaluated after the service profile and the channel clamp, as warns first | messages only in C8a | identical members on every file not setting `service_mode` or a clamped `channel_mode` | C8b (refuse, or move the profile earlier, which changes members for such files): yes |
| C9 | Run-time fallbacks (GPT3, OTL source, resolved date) write one line to the main log in addition to their component logger | message only | identical members and products | no |
| C10 | Ledger rows for the 24 network leaf keys, for the members the derived configuration pins (as a "derived" area), and a fix of the two KEEP_OFF bundle rows | ledger text only | [corrected 2026-11-19: see Corrections, item 8] `ledger_render.py --check`, `make_golden_config.py --verify` | yes (ledger rows are the author's) |

[corrected 2026-11-19: see Corrections, item 2] C1, C2, C3 (warn only), C5, C6 and C9 need no ruling and remove every confirmed silent row from the silent classes (they become class b). C4, C7, C8b and C10 wait for the author. The step that changes no member but most improves what a reader of the log sees is C1, because it covers every confirmed row that differs today.

## Decisions for the author

1. **The three unread keys in the reference configurations** (`network.redundancy_census`, `network.max_duration_s`, `network.min_stations`): delete them from the base configuration and regenerate, or give them meaning. Giving `redundancy_census` a reader would turn off the check and its weight-moving dependents in any file that sets it false.
2. **`closed_loop.consume_network_iono` true in every reference configuration** while the ledger says KEEP_OFF and the network engine builds no supply: set it false through the manifest, or keep it and accept that it does nothing.
3. **The pins of the held-out station** (signal rule Count, no-bias code weight 100, bias age gate armed, channel mode from the closed-loop key, AR mode and wide-lane flag from the closed-loop keys): keep them as the derived posture and announce them (C1), or let the file's values stand. The standalone and in-process paths should then agree.
4. **A strict flag** for unknown keys and requests with no effect (C3): whether to add it, and its default.
5. **Retire, or give a reader to,** each no-reader key of section 4.3 (C4).
6. **One policy for a dependent without its master switch:** the ring fix refuses, the rest warns and disarms or says nothing (R8).
7. **The Galileo pair family:** list `phase_datum_eq34_family_pairs` in the adopted configuration, or add a per-constellation line when a requested pair family falls back to per-signal trees (the chain audit's owed line).
8. **Partial fixing in the user engine,** ruled off, is accepted and run: refuse it, warn, or leave it.
9. **`service_mode` ordering** (C8b): move the profile before the validations or re-validate after it; this changes members only for files that set `service_mode` (none of the five).
10. **User-engine keys spelled `network.*`:** parse them in every mode (a member change for standalone real-time files that set them) or warn.
11. **`estimate_ifb` at the held-out station:** confirm that inheriting the forced true is the posture the station was graded with, and correct either the comment at main `RealTimeEngineSsrBreaks.cpp:861` or the derivation.

## Not checked

- **Slices no reader covered:** reads outside the two loader files (`GnssConfig.cpp`, `RetiredConfigKeys.cpp`, the simulator's configuration); the non-network loader of 7fb0737 beyond `loadProcessingSection_` (compared only by the set script); the network loader and posture table of main (set script only); the extends merge beyond its order and the diamond message; array elements below the first level; wrong-type values (they throw); whether a JSON float given to an int key truncates.
- **The 588 silent rows below the verifier's cut**, including every row of section 4.4.
- **Values in effect at run time:** no build, run or log was read. The adopted run's Galileo signal count after `phase_families`, whether GPS also falls back on epochs with more than two phase signals, and which tree's executable ran the user example are open.
- **Inferred, not shown by one line:** the streaming closed-loop absence; `rx_extra_band_delay_q` inert without the extra-band estimate; `gap_cover: free` needing a positive MW gate; `n3c_bia_pin_pair` with no clock-reference pairs; `cb_excursion_reinit` with no pivot; the `ambiguity_vehicle` posture warn on the adopted run.
- **Not traced:** duplicate station names past `MultiRinexSync.cpp:43`; a malformed `clock_datum_satellite`; whether the decorrelated search reads `enable_partial_ar`; AR modes outside 0 to 3 and unknown names in `ar_active_systems`; whether the network engine reaches `MeasurementModelVariance` (the network half of V7); `phase_datum_pref_elev_deg` under eq34; `sat_health: pivot` under a pivot-free clock datum; which `realtime.*` keys reach the held-out station.
- **Reader counts** are grep counts over member names and getters; shared names inflate them, and tests were not counted. Every zero was checked by hand.
- **Per-key detail** (every row of the sixteen readers, with its tree, line, member, getter, ledger and posture identity, classes and the configurations that set it) is in `CFG_key_map.json`, which another agent writes; it was not present when this document was written.

## 2026-11-19: corrections from the check against the code

Two independent reads checked this document on 2026-11-19 in 7fb0737 and main 5ddc422. LN is the network loader, LD the non-network loader. Most important items first; the full errata is at `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/_critical_read/CFG-defects_errata.md` and `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/_critical_read/CFG-sets-and-plan_errata.md`.

1. **The standalone profile does not log "the same" list** (section 1.1, rule R11, step C1). Its info lines (7fb0737 LN:13966-13981, APPLIED line LN:13987-13996) name the bias age gate, `estimate_ifb`, `estimate_sat_biases`, `ar_wl_only` and `ar_ignore_mw_wl`, not the no-bias code weight forced to 100 (LN:13748), the processing mode or the satellite-state flags. The shared couplings (LN:13710-13783) log nothing on either path.
2. **Plan coverage** (section 6, opening and closing paragraphs). The no-ruling steps remove V1 to V7, V10, V17, V19, V21, V22 and V24 from the silent classes, not every confirmed row: V9 is a loader force (LN:3974), V11 needs a run-time line, V13 to V15 are read keys C3 does not flag, V16 sits under a wildcard read, V23 and V25 fall to C7. C1 covers V4 to V7; V17 (Knobs.cpp:445-448) and V19 also differ today on the adopted configuration and belong to C2. More than log text grows: C2 adds a manifest column, C3 one member, C4 removes members.
3. **The draft's Outcome count** (rewritten above). V5 and V7 replace requests, V6 arms an unset key, V4 changes no member (stored as asked, LN:13394) but has no supply; V17 and V19 were not counted.
4. **V11 on the adopted configuration is not shown** (section 4.1, priority 2). The adopted file sets `phase_families` (E:L1X>L1C, E:L5X>L5Q) and `phase_family_codes`; a member shares its identity's chain group (`include/network/assembly/PhaseFamily.hpp:7-9`) and the Galileo priorities list only L1C, L1X, L5Q, L5X, so two Galileo groups are expected. GPS with several L1 or L2 signals is the likelier per-signal case (inferred). No run was read. Change-set line B7 needs `phase_datum_eq34_family_pairs`, unset on the adopted configuration, so it does not close V11.
5. **Replay** (section 4.1, priority 1, V4). The same derivation runs in replay (main `ApplicationConsumerReplay.cpp:359`): the user runs `_rap_nw_obsadx_143_quin` and `_rap_nw_obsadf_144_badg` ask priority, no age key and 1.0, and get Count, the gate and 100. In replay a warn names the ionosphere request INERT (`ApplicationConsumerReplay.cpp:139-144`); V4 is silent in process only.
6. **Blank ledger defaults** (section 5). 57 rows have an empty default (35 network rows); 16, all outside the network section, are blank where the loader default is not; the two second-channel allow-lists (rows 745-746) are blank against a non-empty member default.
7. **C3 proof.** The five also warn for `_comment`, set by all five and read by no loader line; the claim holds once `_comment` is exempt.
8. **C10 proof.** `ledger_render.py --check` fails until the ledger view is re-rendered; Group C of the change set fails `config_posture_check.py --verify-loader` with 12 "MISSING from ledger" rows (`config_posture_check.py:356-363`) until those keys get RECOMMEND_SCOPED rows (one, `carry_rebase_complete`, is KEEP_OFF). `make_golden_config.py --verify` passes.
9. **Scope of C8b and decision 9.** `config/ppp_rtk_network_brdc_rts.json:168` sets `service_mode` true; refusing a clamped `channel_mode` instead would stop 656 network configurations in `config/` from loading.
10. **V7 is narrower:** main `MeasurementModelVariance.cpp:231-236` applies the weight only to code rows that neither .BIA nor the stream covers.
11. **V1 to V3 were never read** (`git log -S`, both spellings). The redundancy check has been armed by its stride since 2026-08-05 (commit 2862318; comment now Knobs.cpp:586). V1 and V3 are listed inert at `scripts/loader_keys.py:423`. V2 is equal on the five, not in `config/`: six files differ.
12. **Counts and wording.** The 1 326 / 1 325 keys include 17 / 16 array-element forms (1 309 without them). "No default differs": 752 rows equal, 16 blank, 1 equal expression, 15 not compared. The key map holds 2 098 rows; its silent flag marks 560 (510 unruled, 101 set by R or A) against the 632 / 588 / 85 of the method and section 4.4, by a different definition. Change-set B4 "folded every epoch" omits `!clock_brdc_anchored` (`ClockAnchor.cpp:1357`).

### Line numbers that moved

V11 run end `ChainDatum.cpp:3107-3122`; V9 LN:3974-3980, refusal LN:1758; V10 LN:13846-13847; V21 Posture:438-440; V22 Posture:343-345, 350-352, `PassASlipScan.cpp:778-779`; V1 Knobs.cpp:519-521; V2 LD:583 in main; ring-fix inert lists LN:11253-11292; the derivation in main LN:13486-13731.

### What stands

V1 to V7, V9, V10, V21 and V22 hold at the lines (V21, V22 latent: on the adopted configuration the knobs resolve to the requests). The set comparison stands with items 6 and 12. The change set is message-only, but Group C needs ledger verdict changes. The plan's order stands, its coverage sentences do not. Decisions: 1 stands, stronger (never read; a reader for `network.max_duration_s` changes six files, `network.min_stations` is set in 343); 2 stands (replay warns, in process does not); 3 is stronger (the pins reach every graded user run); 7 is weakened (the line is owed, but the adopted run's Galileo election should be settled from its eq34 run-end block first); 9 stands with item 9; 4 to 6, 8, 10 and 11 are unaffected.

### Left unconfirmed

V8, V12 to V20 and V23 to V25 beyond the verifier; whether C2 covers V18 and V20; the section 6 proofs and hunk compilation; configurations reached only through `extends`; any run log (the adopted run's Galileo and GPS family counts); the network half of V7.

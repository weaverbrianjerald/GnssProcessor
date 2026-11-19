# CRT: a run-start table of asked beside in effect, and the network loader split (2026-11-18)

**STATUS**

- **id:** CRT
- **topic:** config-ledger
- **status:** open
- **summary:** A design for one plain record per configuration decision (ConfigDecision), a closed list of reason codes and one run-start table that prints, for each key and each engine that reads it, the value asked beside the value in effect after the last owner of that value; an opt-in strict mode with an unknown-key check; and the network configuration loader split into twelve concern files and one internal header, with declared key rows and one rule table per file run at the present call points. Design only: no code changed and no run made. The measurements of the two loaders and the load-bearing code claims hold after the check against the code; eight statements are corrected (counts in the Outcome, line ranges, an incomplete strict-mode list, the steps that change log text) and one test count is left unconfirmed.
- **source:** plan package 19 (2026-11-18), the run-start table of asked beside in effect and the loader split (two measurements of the loaders, one design sketch and one review, merged; companion document `CFG_asked_vs_in_effect_2026-11-18.md`); network code, the network loader and the run record read at 7fb0737 (branch chain/audit), user-engine code, the non-network loader and the dashboard scripts at main 5ddc422. Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** The two loaders read 1 104 distinct keys at 1 243 read sites and take 1 417 further decisions (refuse, change or leave inert), 393 of them with no log line; no run-start output states, for any key, the value asked beside the value in effect. The knob snapshot resolves silently and the derived user configuration pins values with no line. The design records every decision as a plain row, reads the value in effect from the last owner and prints one table before the run manifest. The network loader splits into twelve concern files and one internal header. Each migration step is to be proved by a member dump comparison (the tool is not yet written); four steps (S1, S3, S8, S9) change log text.

Read at: commit 7fb0737 (branch chain/audit, checkout D:/GnssProcessorSupport/worktrees/read_7fb0737) for network code, the network loader and the record; main at 5ddc422 (C:/GnssProcessor) for user-engine code, the non-network loader and the dashboard scripts. Abbreviations: LN = src/core/config/GnssConfigLoadNetwork.cpp (7fb0737, 14 340 lines); LD = src/core/config/GnssConfigLoad.cpp (main, 3 841 lines); Knobs = src/core/network/driver/Knobs.cpp (7fb0737); Posture = src/core/config/GnssConfigNetworkPosture.cpp (7fb0737). Measurement scripts and outputs: D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/CRT_run_start_table_2026-11-18_parts/_scratch/ (measure-plain-keys/, measure-interlocks-and-messages/).

How the stages were merged. The two measurements are kept as measured. The design sketch is kept where the review confirmed it, and replaced where the review showed it wrong: the key row (2.1 of the review), the single global rule table (2.2), the strict rule (2.3), the completeness claim of the readback (2.4), the second owner of the knob rules (2.5), the placement of the table outside network runs (2.6), the reason list (2.7), and the identity claims of steps S3, S6, S8 and H1 (2.8). Each replacement is stated where it applies, marked "[review]".

---

## 1. The measurements

### 1.1 Read sites and plain keys

A read site is one `jval<T>(json, path, alias, default)` call or one `section.value("k", default)` call.

| | LN (7fb0737) | LD (main) |
| --- | --- | --- |
| read sites | 744 (678 jval, 66 .value) | 499 (261 jval, 238 .value) |
| distinct literal keys | 660 | 444 |
| functions | 34, median 210 lines | 14, median 135 lines |
| throws | 189 | 0 (refusal only through `return false`) |

Seven LN keys are read at two sites, among them `network.ura_staleness_q2` (LN:8255-8257 and again under service mode at LN:12036-12039, both times with the default `network_clock_model_q2_`) and `network.carry_coupling_exact_update`, tested bare inside a refusal of `emit_pb_hold_smoothed` at LN:3045-3046 before its own read site at LN:4915. The audit key map (CFG_key_map.json) holds 1 209 network rows; every measured key is in it, and the 549 extra rows are interlock and forcing rows, not reads. Arrays and maps read by subscript and `get<>` (about 13 in LN, about 18 in LD) are not counted as sites.

Shape of each read site (one class per site, the first that matches):

| class | LN | LD |
| --- | --- | --- |
| plain: member = read, literal key and default, no condition | 367 | 257 |
| plain plus a log line naming the value | 90 | 106 |
| plain plus a single-key range check that refuses | 69 | 1 |
| plain plus a single-key check that replaces the value | 75 (33 silent) | 63 (15 silent) |
| string converted to an enum | 19 | 18 |
| range check or replacement reading a second key | 58 | 3 |
| prefix helper (`pre + "fold"`, LN:139-175) | 11 | 0 |
| array element or per-element lambda | 28 | 3 |
| interlock at read time (inside an if on another key, or default from another member) | 15 | 11 |
| read arms other state | 6 | 4 |
| string grammar parsed by a helper | 3 | 1 |
| local then free logic, wrapped right-hand side, read inside an expression | 2 | 31 |
| computed key from a table loop | 1 | 1 |

Coverage of a declarative row, cumulative over both loaders: plain plus announce 820 of 1 243 (66 %); adding a refusing range 70.7 % (LN) and 72.9 % (LD); adding a replacing range 80.8 % and 85.6 %; adding enum names 83.3 % and 89.2 %; adding the fixed-cycle keys written out in full, LN 84.8 %. Together 1 076 of 1 243 sites (86.6 %). The strictly plain class alone is 624 sites (50 %). [review] The coverage holds only for a row that carries the range action, the enum names and the announce text (section 5.2); the sketch's narrower row covers the 50 %.

### 1.2 Legacy aliases

The alias is the third jval argument, a top-level key tried when the dotted path is absent (include/config/GnssConfigJsonHelpers.hpp:126-133 in 7fb0737; same text in main). A present key is always converted, so a wrong JSON type throws.

- LN: 665 of 678 jval sites carry an alias (337 the flattened path, 311 the last segment, 17 prefixed `network_`). LD: 260 of 261.
- Use over the 1 351 JSON files under config/ in each tree (14 parsed after comment stripping): LN aliases used 0 times; LD aliases used once (`max_duration_s` at top level of config/archive/test_suite/_golden_sim_tri_w6_smoke.json, read at LD:583). The ten section aliases of `jsection` are used 0 times. No alias string is shared by two keys.
- [corrected 2026-11-19: see Corrections, item 4] An alias column therefore carries about 925 strings that no configuration in either tree uses. Copies of configurations inside run folders were not scanned.

### 1.3 Interlocks by reason code

Source: the 1 472 rows of CFG_key_map.json with a non-empty `changes` array, giving 1 510 decision sites; 1 417 lie in the loaders and Knobs. Each site keeps the audit's class and receives a cause by ordered text rules. A hand check of 40 random sites agreed on about 85 %, so each count is good to roughly 10 %.

| code | meaning | sites | refuse | changed with a line | silent | example |
| --- | --- | --- | --- | --- | --- | --- |
| COMPANION_MISSING | needs a key that is off: refused, disarmed or left inert | 485 | 143 | 239 | 103 | LN:348 refusal, emit_phase_frame_hold requires emit_pb_hold |
| COMPANION_ARMED | arms the companion it needs | 29 | 0 | 26 | 3 | LN:7179, raim_within_epoch arms raim and raim_attribution |
| MODE_FORCES | a selector forces or supersedes a value | 108 | 6 | 92 | 10 | LN:3942-3946, the network code-bias mode sets estimate_sat_biases_ true |
| EXCLUSIVE | two requested keys exclude each other | 26 | 22 | 2 | 2 | Knobs:425, the pin bridge drops the eq34 pair family |
| PROFILE_OVERRIDE | service_mode arms or overrides | 39 | 3 | 26 | 10 | LN:11866-11878: explicit false loses with a warning; explicit non-zero numeric wins silently |
| RANGE | out of range: clamped, replaced or refused | 326 | 108 | 161 | 57 | LN:12105-12109, start_offset_hours below 0 clamped |
| BAD_VALUE | unknown enum, wrong shape, name matching nothing | 110 | 33 | 66 | 11 | LN:149 refusal, fixed_cycles.fold not off/observe/emit |
| RETIRED | key retired or renamed | 4 | 0 | 4 | 0 | 7fb0737 src/core/config/RetiredConfigKeys.cpp:93 |
| NOT_BUILT | mode or feature absent from this build | 2 (+3 outside) | 1 | 1 | 0 | LD:2737, gmf read as NMF |
| UNREAD | a set key no line reads | 26 | 0 | 0 | 26 | LD:507, satellite_interpolation_mode echoed, no reader |
| ALIAS_SHADOWED | dotted key and alias both present, dotted wins | 1 helper site | 0 | 0 | 1 | GnssConfigJsonHelpers.hpp:127-133 |
| USER_DERIVED | the derived user configuration pins or replaces | 61 | 3 | 7 | 51 | LN:13748, no_bia_code_downweight_ = 100.0 |
| KNOB_RESOLVED | the knob snapshot resolves the request | 118 audited (155 statements) | 0 | 4 | 114 | Knobs:72-73, drop jump and gap cover off without float_kf |
| unclassified by rule | | 83 | 30 | 47 | 6 | 20 hand-read: each falls in COMPANION_MISSING, MODE_FORCES, RANGE or BAD_VALUE (inferred for the rest) |

Totals: refuse 349, changed with a line 675, silent 393 (silent ignore 143, late override 137, silent change 113). The silence sits mostly in KNOB_RESOLVED (114), COMPANION_MISSING (103), RANGE (57) and USER_DERIVED (51). Outside the loaders, 93 run-time fallback sites (GPT3 grid, ocean loading, path date) would need a further code; they are not loader decisions and were not read.

### 1.4 Messages by level and form

| file | throw | error | warn | info | debug | total |
| --- | --- | --- | --- | --- | --- | --- |
| LN | 189 | 128 | 418 | 318 | 0 | 1 053 |
| LD | 0 | 13 | 123 | 130 | 1 | 267 |
| Posture | 0 | 0 | 4 | 5 | 0 | 9 |
| RetiredConfigKeys.cpp:93 | 0 | 0 | 1 | 0 | 0 | 1 |

By action (LN / LD): refuse 274 / 13 (LN by two mechanisms: 189 throws, 85 error lines followed by `return false`); change 326 / 123; warn only 447 / 129; unconditional echo 6 / 2.

The level does not encode the action: LN has 43 error lines that do not stop the load (for example LN:9665) and 118 info lines that accompany a change; 135 LN lines use the word INERT for a request left in place with no effect.

Form, by rule over the literal text ("names key": a dotted key or the refuse(leaf) form; "asked": the requested value; "in effect": the value the run holds):

| file, action | n | names key | asked | in effect | all three | none |
| --- | --- | --- | --- | --- | --- | --- |
| LN refuse | 274 | 260 | 83 | 32 | 16 | 6 |
| LN change | 326 | 251 | 189 | 214 | 123 | 29 |
| LN warn only | 447 | 284 | 278 | 243 | 141 | 56 |
| LD refuse | 13 | 3 | 7 | 3 | 0 | 4 |
| LD change | 123 | 22 | 70 | 64 | 6 | 27 |
| LD warn only | 129 | 30 | 66 | 46 | 13 | 36 |

Of the 449 change sites, 129 (29 %) state key, asked and in effect together. None carries a reason code or the deciding key in a fixed place. The block-action rule can count a block assigning only a local as a change: level counts are exact, the action split approximate.

### 1.5 The stages of change

A value can move at five stages after the file is read. The last owner of each value is what the run uses.

1. **Loader.** The 21 network section helpers in their load-bearing order (LN:259-290: "a later helper reads what an earlier one resolved"), the service profile (LN:285), the carry posture validator (LN:286), the closing refusals and interlocks (LN:295-953), then the posture report (LN:955). LD's section helpers for the other modes.
2. **Knob snapshot.** `NetworkKnobs::snapshot` (Knobs:24-720), built in the `NetworkRunContext` constructor (7fb0737 src/core/network/driver/ApplicationNetwork.cpp:249). 362 assignments; 155 are not plain copies: 89 companion or parent gates, 10 ambiguity-vehicle gates (Knobs:46, 52, 58, 70-74, 84, 178, 717), 28 clamps or fallbacks with no line, 10 unions arming one feature from another key, 1 exclusion (Knobs:425), about 10 lines parsing family lists and class masks, 13 derivations that change no request.
3. **Derived user configuration.** `deriveClosedLoopConsumerConfig` (LN:13784-13953, 37 writes and one list clear, no logger: comment LN:13800-13803) and `applyNetworkConsumerCouplings` (LN:13710-13782, 13 writes). Called in network setup (7fb0737 src/core/network/driver/ApplicationNetworkSetup.cpp:1819, before ApplicationNetwork.cpp:249) and again in the user-engine replay path (7fb0737 src/core/app/ApplicationConsumerReplay.cpp:359).
4. **Standalone user profile.** `applyNetworkConsumerProfile` (LN:13956-13998), called in the real-time path (7fb0737 src/core/app/ApplicationRealtime.cpp:357); each write has an info line, and the closing line reports four of its values.
5. **Path resolution.** PathResolver fills empty paths after the load (main src/core/app/Application.cpp:151-201).

What run start reports today:
- The run manifest records 41 knobs (7fb0737 src/core/network/diag/RunTraces.cpp:1663-1703); the only non-plain one is pl_local_block (RunTraces.cpp:1682). No log line prints any of the 155 resolved knob values at start. Some appear later: at run end (src/core/network/ar/AmbFloat.cpp:552-562, src/core/network/solve/ClockAnchor.cpp:1198), at the first bridge fold (src/core/network/state/BorderPrior.cpp:216), at a warm-resume seam (src/core/network/state/WarmResume.cpp:304, 1156).
- For the held-out user station, setup prints only ar_mode, ar_wl_only and, for the undifferenced model, the capped sigma gate (ApplicationNetworkSetup.cpp:1880-1918). Never printed: the canonical signal rule, no_bia_code_downweight, estimate_sat_biases, the bias age gate, channel_mode (replaced unconditionally at LN:13846), the second channels, the extra-band scales.

### 1.6 Members written by two or more functions

LN: 63 member names (58 leaf members); of the plain or enum read sites, 51 fill a member another function rewrites later. LD: 6 (run_smoother_, enable_neq_export_, enable_noncausal_slip_bridge_, estimate_velocity_, download_config_.year and .doy). Examples in section 6. The two-writer count is a lower bound: writes through references and lambdas were resolved only for `auto& x = member;` bindings.

---

## 2. The row type ConfigDecision, the recorder and the reason codes

### 2.1 ConfigDecision

A plain value type in a new leaf header include/config/ConfigDecision.hpp. No behaviour.

| member | type | content |
| --- | --- | --- |
| key | string | dotted key as the key row declares it |
| alias_used | bool | the flat alias supplied the value |
| asked | string (JSON text) | the file's value; empty when the file is silent |
| source | string | the file of the `extends` chain that set it, or `default` (needs provenance through the merge, step S1b) |
| in_effect | string (JSON text) | the value the named engine holds after the last owner |
| engine | enum Engine | Network, HeldOutUser, StandaloneUser, Single (non-network modes) |
| stage | enum Stage | Load, ServiceProfile, Validate, Finalize, Snapshot, DerivedUser, StandaloneProfile, PathResolve |
| action | enum Action | AsAsked, Changed, Inert, Refused |
| reason | enum Reason | the closed list of 2.3 |
| by | string | the deciding key or mode (empty for RANGE and BAD_VALUE) |
| designed | bool | the decision is a declared design (W6 forcing, user-engine default); strict mode ignores it |
| detail | string | the text the existing message prints |

[review] The engine column is new. One key can hold two values in effect at once: `estimation.no_bia_code_downweight` is 1.0 in all three reference configurations and in the network engine, and 100 in the held-out user engine (LN:13748). A single in_effect column cannot show both; the table writes one row per engine that reads the key.

[review] The sketch's "tier" column (Refused, Changed, Ignored, AsAsked) is kept as `action`, with Ignored renamed Inert to match the 135 INERT lines.

### 2.2 The recorder

`ConfigDecisionLog` holds the rows and a logger. One call appends a row and emits the existing text at the existing level:

```
rec.note(stage, key, asked, in_effect, action, reason, by, level, text);
```

An existing message becomes a record by replacing `log->warn(fmt, args...)` with `rec.note(..., Level::Warn, fmt::format(fmt, args...))`. The member assignment beside it is not touched, the recorder only reads values, and level and text stay identical.

Ownership [review]. The sketch put a `std::shared_ptr<ConfigDecisionLog>` in GnssConfig. That changes copy semantics: `GnssConfig c = *this;` in the derivation (LN:13785) and the replay copy (ApplicationConsumerReplay.cpp:359) would share one log. The design instead keeps the log outside GnssConfig: `loadFromJson` takes an optional `ConfigDecisionLog*` (null keeps today's behaviour), the helpers receive it as a parameter beside `log`, and each later stage receives the log of its caller. GnssConfig gains no member for it (inferred alternative of the review; the helper signatures are private declarations in GnssConfig.hpp, so this lands in step H1).

Precedent for knowing "asked": `enable_ssr_bias_max_age_explicit_` (7fb0737 include/config/GnssConfig.hpp:10063), read by the derivation at LN:13804-13806; the profile's `userWrote` presence test (LN:11861-11863).

### 2.3 The closed list of reason codes

Reason codes name causes; the stage is its own column. [review] The measured list is adopted over the sketch's 18 codes, which had no code for an exclusion, a feature absent from the build, a refusal for range or bad value, or a request left inert, and had a code (ChannelClamp) with no measured class.

| code | cause | measured sites (1.3) |
| --- | --- | --- |
| AS_ASKED | no decision | (every other key) |
| COMPANION_MISSING | needs a key that is off | 485 |
| COMPANION_ARMED | arms the companion it needs | 29 |
| MODE_FORCES | a selector forces or supersedes a value | 108 |
| EXCLUSIVE | two requested keys exclude each other | 26 |
| PROFILE_OVERRIDE | service_mode arms or overrides | 39 |
| RANGE | out of range: clamped, replaced or refused | 326 |
| BAD_VALUE | unknown enum, wrong shape, unmatched name | 110 |
| RETIRED | retired or renamed key | 4 |
| NOT_BUILT | mode or feature absent from this build | 2 (+3) |
| UNREAD | set key with no reader in this mode | 26 |
| ALIAS_SHADOWED | dotted key and alias both present | 1 helper |
| ENGINE_DERIVED | a derived engine configuration pins or replaces the value | 61 (measured as USER_DERIVED) |
| UNKNOWN_KEY | key in no declared universe (section 4) | not measured |
| UNATTRIBUTED | a member differs from the file with no record (2.4) | target 0 |

Disagreement, stated plainly. The measurement keeps KNOB_RESOLVED as a code; the review notes that it names a stage, not a cause, and hides the 28 silent clamps, 10 unions and one exclusion of the snapshot. This design follows the review: a snapshot resolution is a row with stage Snapshot and the cause it has (a clamp is RANGE, a union COMPANION_ARMED, Knobs:425 EXCLUSIVE, a parent gate COMPANION_MISSING, the vehicle gate MODE_FORCES). USER_DERIVED is renamed ENGINE_DERIVED for the same reason and keeps its own code, because a user-engine default restored by the derivation (LN:13742, LN:13748) has no other cause.

The list closes against the measured sites only to the sample's accuracy (83 sites were unclassified by rule; 20 of them were hand-placed). A code added later is a ruling, and the enum lives in the leaf header, so adding one rebuilds only its includers.

### 2.4 The readback and what it does not catch

At the end of each stage the recorder reads every key row's member back and compares it with the file's value (or with the previous stage's value). A difference with no row for that key and stage becomes an UNATTRIBUTED record. A unit test over every file in config/ requires zero UNATTRIBUTED rows, so a new silent change fails a test, not a run.

[review] The readback catches value changes only: the late overrides (137) and silent changes (113). It does not catch the largest silent class, silent ignore (143 sites, where the request stays in its member and has no effect), nor UNREAD (26). Those become records only when the code that decides them calls `rec.note` (step S4 for the INERT lines, the unknown-key walk for UNREAD). A silent ignore that prints nothing today stays invisible until its site is migrated; the zero-UNATTRIBUTED test does not prove it.

[review] The 51 rewritten LN members (1.6) need, per key row, a declared list of later writers; otherwise their rows are UNATTRIBUTED by design until every writer records.

---

## 3. The run-start table

### 3.1 Columns and rows

Columns: key | engine | asked | source | in effect | stage | action | reason | by | detail.

Rows: every key the file sets; every key whose value in effect differs from its default although the file is silent on it (the bias age gate armed by the derivation, LN:13804-13806; a profile arming); every UNKNOWN_KEY. One row per engine that reads the key.

### 3.2 Where the values come from

[review] Values in effect are read from the last owner, never recomputed:
- network engine: the built `NetworkKnobs` through one declared map from key to knob member (5.4); keys with no knob read the GnssConfig member after the loader;
- held-out user engine: the derived GnssConfig (`setup.consumer_config`, ApplicationNetworkSetup.cpp:1819), compared member by member with the base configuration;
- standalone user engine: the configuration after `applyNetworkConsumerProfile` (ApplicationRealtime.cpp:357);
- every mode: paths after PathResolver (main Application.cpp:151-201).

### 3.3 Where it is printed

- Network run: once, immediately before `networkRunManifestStartup(run, config_path_)` (ApplicationNetwork.cpp:290), after the setup arms of lines 270-288, which may resolve further (inferred, not traced). [review] The sketch placed it between lines 249 and 290 without the arms.
- Real-time and post-processing runs: after the standalone profile and PathResolver respectively, not after `loadFromJson`. [review]
- User-engine replay: after the derivation at ApplicationConsumerReplay.cpp:359.
- A refusal prints the rows recorded up to and including the refusing row before the load returns false or throws.

Log form: a header line `=== Configuration: asked and in effect ===`; each row whose action is not AsAsked on one line in the fixed form `KEY engine=E asked=A in_effect=B reason=CODE by=KEY2 stage=S` (Refused and Changed at warn, Inert at info); one count line for the AsAsked rows. The full table goes to the manifest.

### 3.4 The manifest and the dashboard

- Manifest: one JSONL record `{"record":"config_decisions","rows":[...]}` through `appendRunManifestRecord` (RunTraces.cpp:2452-2456), which already gates on `write_run_manifest` and appends one object per line.
- Dashboard: the manifest reader keeps only known record names (main scripts/netview/layers/epoch.py:181: param_model, param_dispositions, param_responses, complete, startup); `config_decisions` is added there and rendered as one sortable table (main scripts/netview/nv_tables.py, inferred placement).
- Non-network modes have no network manifest; the record goes to logs/config_decisions.jsonl (inferred need; author decision).

### 3.5 Relation to the posture table

The posture report (`logNetworkPosture_`, Posture:1383-1473, called at LN:955) answers another question: does the run arm what the configuration ledger recommends in this regime. It prints only the regime-matched rows that are not armed (`POSTURE OFF [ON]` at warn, `[SCOPED]` at info, Posture:1417-1418), a warm-resume chain note, the engaged bundles and a count; it prints no value. It reads each member as the loader left it, before the knob snapshot and the derivation, so a row can count as armed what the snapshot resolves off: the rows at Posture:343-352 test `nc.amb_float_drop_jump_k == 1.0` and the gap cover, which Knobs:72-73 resolve to 0 and Off without the float vehicle; Posture:436-440 tests `zwd_event_inflate_m > 0.0`, which Knobs:178-179 zero without the in-block route.

Design: two blocks, one source. The posture report stays a separate block and reads its "armed" test from the in_effect column; it moves from LN:955 to just after the decision table. [review] This changes the posture verdicts as well as their position: a row that counted the raw member as armed prints POSTURE OFF. That is the intent, but it is an output change and is announced as one (step S8). Moving the block out of the loader also removes it from every path that loads without `runNetwork` (the user-engine replay path, the Python module; inferred); the design prints it there after the derivation, or not at all, by author decision.

---

## 4. Strict mode and the unknown-key check

### 4.1 Strict mode

Key `processing.strict_config`, default false, following the toggle convention: `bool strict_config_{false};`, a Doxygen getter `strictConfig()`, read first in `loadFromJson`. [review] No legacy alias: aliases are unused (1.2) and new keys take none.

Rule [review]: abort when a key the file sets explicitly is Changed or Inert inside the engine that reads it, by a reason not marked `designed`. Not counted: rows of an engine that does not read the key; designed decisions (the network code-bias forcing of estimate_sat_biases at LN:3942-3946, the user-engine defaults of the derivation); keys of another mode's section (UNREAD with `by` = the app mode). The sketch's rule ("Changed or Ignored, whatever the stage") aborts every reference configuration.

Checkpoints: the end of the load, and the table's print point (3.3), before the first epoch.

[corrected 2026-11-19: see Corrections, item 6] Validation against the three reference configurations (config/golden/golden_field_refound.json, golden_sim_refound.json, golden_field_bds_refound.json at 7fb0737) before strict mode is offered:
- `estimate_sat_biases: false` (lines 148, 146, 155) is forced true by the code-bias mode (LN:3942-3946): passes only if that forcing is declared designed;
- [corrected 2026-11-19: see Corrections, item 7] `no_bia_code_downweight: 1.0` (lines 149, 147, 156) is the network engine's value, pinned to 100 only in the held-out user engine: passes with the engine column;
- [corrected 2026-11-19: see Corrections, item 8] `min_stations: 10` (lines 229, 224, 236) and `redundancy_census: true` (lines 373, 368, 380) are read by nothing (7fb0737 scripts/loader_keys.py:416-420 lists both as inert): they fail strict mode as UNREAD until the manifest stops carrying them (a generator change, author decision).

### 4.2 The unknown-key check

A walk of the merged document's leaves at the end of the Load stage, against a declared universe:
- the key rows of every concern file;
- the keys read by code that is not a row, declared per file;
- the retired table (7fb0737 include/config/RetiredConfigKeys.hpp:165; reason RETIRED, already warned);
- declared wildcard subtrees: `network.process_noise.<class>` from `kNetworkQClassTable`, `logging.levels.<logger>`, the signal priority lists, `network.stations[]`, `network.inject.events[]`, the ring-fix and family lists.

Not flagged: keys beginning with `_` or `//` (in config/, `_comment` about 830 times and `//`-keys about 300 each, by script over 7fb0737 config/); `extends` (erased before any section, main LD:149). Keys of another engine's section are in the universe and recorded UNREAD at info. A miss is UNKNOWN_KEY at warn (strict mode: abort).

Until the rows exist, the universe is generated by 7fb0737 scripts/loader_keys.py (which already resolves prefix joins and the class table) into one generated include read by one source file.

[corrected 2026-11-19: see Corrections, item 9] [review] About 55 tests assert that a substring is absent from the captured load log (for example 7fb0737 tests/network/test_network_gross_los_cold_bound.cpp:367 "INERT"; tests/network/test_second_band_admission.cpp:534). An unknown-key warning inside the load, or a table repeating detail text, can trip them. The walk therefore logs to its own block after the load, and the table is printed outside `loadFromJson`; both steps re-run those tests.

---

## 5. The split loader

### 5.1 File map

New folder src/core/config/network/, named after the network folders. Each file holds its key rows, its rule table and the function bodies that stay code. Line ranges are 7fb0737 LN; boundaries inside 2324-3604 and the placement of the eq34, signal-integrity and contract keys are inferred from key names and need a per-key pass.

| file | contents (LN lines) |
| --- | --- |
| Driver.cpp | [corrected 2026-11-19: see Corrections, item 10] `loadNetworkSection_` call order (234-300) and its closing refusals (301-378); `loadNetworkStreaming_` (2207-2323); `loadNetworkInject_` (14021-14340) |
| Assembly.cpp | roster part of `loadNetworkRosterAndDatum_` (1104-1288); `loadNetworkSharedStateForcings_` (3605-4351); `loadNetworkAprioriAndGlonass_` (4352-4658) |
| Carry.cpp | `loadNetworkProcessNoise_` (976-1103); `loadNetworkCarries_` (4659-5982); `validateNetworkCarryPosture_` (12083-13268) |
| Slip.cpp | `loadNetworkSlipDetectors_` (5983-6830) |
| Qc.cpp | `loadNetworkQcTiers_` (6831-8043); `loadNetworkSignalIntegrity_` (9050-9257) |
| Solve.cpp | roster datum keys (1289-1313); `loadNetworkClockModelAndChannels_` (8044-8324); `loadNetworkDatumSignalling_` (8325-8878); `loadNetworkReattachHygiene_` (8879-9049); `validateNetworkDatumCoherence_` (10201-10928); `validateNetworkPhaseDatum_` (10968-11765) |
| Ar.cpp | `loadFixedCycleKnobs` (139-175); `loadNetworkProducerAr_` (1314-1756); `validateNetworkFixedCycles_` (1782-1940); `loadNetworkLaneLayers_` (1941-2074); `validateNetworkLaneLayers_` (10929-10967) |
| Emit.cpp | `loadNetworkOutputAndParallel_` (2075-2206); `loadNetworkEmissionFilters_` (9258-10200); `loadNetworkContractHardening_` (11766-11823) |
| Diag.cpp | trace and manifest keys of `loadNetworkObservabilityAndSnapshot_` (2324-3604 less snapshot keys); `logNetworkRxExtraBandDelay_` (13269-13363); `logNetworkSummary_` (13557-13648) |
| State.cpp | the `state_snapshot_*` keys out of 2324-3604 |
| Profiles.cpp | `applyNetworkServiceProfile_` (11824-12082); `finalizeNetworkProcessNoise_` (13649-13709) |
| UserDerivation.cpp | `loadNetworkClosedLoop_` (13364-13556); `validateNetworkUserLayoutFlags_` (1757-1781); `applyNetworkConsumerCouplings`, `deriveClosedLoopConsumerConfig`, `applyNetworkConsumerProfile` (13710-14020) |
| NetworkLoader.hpp | internal header of the folder: the row types, `struct GnssConfigLoader` with static functions, befriended once by GnssConfig so row tables can form member pointers to private members |

The posture tables stay in Posture. LD (main) is split in place into its existing section helpers with rows; its two largest functions, `loadProcessingSection_` (945 lines, 91 sites, 52 warnings) and `loadEstimationSection_` (928 lines, 137 sites), take most of the rows.

Size target [review] (inferred, author decision): no concern file over about 2 000 lines and no function over about 150 lines after the split. The reads alone do not reach it: rows remove roughly one line per plain read, while most of LN's bulk sits in functions with one read (`loadNetworkSection_` 742 lines and 1 site; `validateNetworkCarryPosture_` 1 186 lines and 1 site; `loadNetworkSharedStateForcings_` 747 lines and 23 sites). The readability gain must come from the rule tables.

### 5.2 The declarative key row

[review] The measured row, plus a read-form column, replaces the sketch's narrower row.

| column | content |
| --- | --- |
| key | full dotted path; the fixed-cycle prefix written out, so the prefix helper disappears |
| alias | legacy top-level key, empty by default (one key in main uses one, 1.2) |
| read_form | `jval` or `section_value` (nlohmann `value()` under a held section, no alias lookup, conversion type fixed by the default literal) |
| type | bool, int, double, string, enum: the type of the read, not of the member (`tj.value("reconnect_delay_s", 5)` at LN:1138 and `cl.value("ar_mode", 1)` at LN:13390 convert through int) |
| default | a literal, or a reference to a process-noise class (6 sites: LN:4671, 5151, 8058, 8061, 8474, 8717) |
| range | lower and upper bound, open or closed ends, finite flag |
| on_range | `refuse` (LN style, 69 single-key refusals) or `replace_with_log` (LD style, 63 replacements; the 48 silent ones gain a line, an announced change) |
| enum_names | (name, value) pairs and `on_unknown`: `refuse` (LN:4765-4774) or `fallback_with_error` (LN:4697-4714) |
| member | pointer to the GnssConfig member or NetworkConfig sub-member |
| announce | text printed when the value differs from the default (196 sites) |
| later_writers | the functions that may rewrite the member after the read (1.6, section 6) |

The precedent is `NetworkQDesc` / `kNetworkQClassTable` (7fb0737 include/config/GnssConfigTypes.hpp:1592-1623), one row per process-noise class, iterated by the loader (LN:1004-1036). A grouping column taken from the configuration ledger, checked against docs/CONFIG_promotion_ledger.json by a test, is an option (inferred design, not measured; author decision).

What stays code: the 58 cross-key checks, the 15 read-time interlocks, arrays of objects (stations LN:1109-1138, inject events LN:14057-14099), per-constellation maps (main LD:1106-1123, 2086-2094), string grammars (`network.prefit_event_classes`, LN:2492, 2527-2528), the frequency-mode fallback chain (main LD:301-334), reads inside expressions (main LD:2775), the process-noise loop. Each records through `rec.note`.

### 5.3 The rule tables

[review] The sketch's single Interlocks.cpp, "one ordered table, same order", is replaced. The cross-key forcings are interleaved with reads across 21 helpers by design (LN:259-262); one table run as a block would read members at other times than today, and the read-time interlocks and member-default reads would change value (`ura_staleness_q2` takes `network_clock_model_q2_` at read time, LN:8257, and again after the profile, LN:12038). One table per concern file, each executed at the present call point of the code it replaces, keeps every member identical.

Rule row: condition (a small pure function of the configuration), action (refuse, set member, leave inert), keys touched, `by` key, reason code, `designed` flag, level, text. Rows run in declaration order; the order is the present execution order, and reordering is a ruling, not part of the split.

### 5.4 The knob map

[review] The sketch's KnobDecisions.cpp re-evaluated the 155 snapshot conditions beside Knobs.cpp: two owners of one rule, which drift, and a new resolution would go unrecorded with no test failing. Replaced by one declared map, in src/core/network/driver/, from key to knob member, with the reason code and `by` key each resolution has (for example `network.amb_float_drop_jump_k` to `amb_drop_jump_k`, MODE_FORCES by `network.ambiguity_vehicle`, Knobs:72). The value in effect is read from the built `NetworkKnobs`. A knob that differs from its key with no map entry is UNATTRIBUTED and fails the same test. Knobs.cpp is not edited. How many of the 155 statements map to one key each was not measured; unions and parses that combine several keys need one entry per contributing key (inferred).

### 5.5 Convention change asked of the author

A new plain key is a member, its Doxygen getter and one row in its concern's table (in place of a hand-written jval line and summary toggle-log line); a new cross-key rule is a row of its concern's rule table with its reason code; a new non-plain knob resolution is an entry of the knob map. The JSON section rule (processing or estimation for engine toggles, network for network keys) is unchanged.

---

## 6. Members written by two functions, and the order questions

### 6.1 Examples of second writers

| member | read | later writer |
| --- | --- | --- |
| network_cycle_slip_relevel_free_epoch_ | LN:6083 | forced false under carry_invariant inside `loadNetworkSection_` (LN:536-542) and again at LN:6197 |
| network_carry_zwd_in_block_ | LN:5157 | forced false in `validateNetworkCarryPosture_` (LN:12780-12802) |
| network_clock_model_q1_ | LN:8057 | set at LN:8246, again in `applyNetworkServiceProfile_` (LN:12023) |
| network_ura_staleness_q2_ | LN:8255-8257 | re-read under service mode (LN:12036-12039) |
| estimate_sat_biases_ (and promote_sat_code_bias_, estimate_ifb_) | estimation section | forced by the network code-bias mode (LN:3942-3946); on the derived copy set to `!consume_clock` (LN:13742) |
| no_bia_code_downweight_ | estimation section | 100 on the derived copy (LN:13748) |
| filter process-noise members | filter section | network process-noise write-through (LN:1061-1070, from the measured lead; not re-opened) |
| network_start_offset_hours_ | none in a load helper | read and clamped inside `validateNetworkCarryPosture_` (LN:12103-12109) |
| run_smoother_, enable_neq_export_, enable_noncausal_slip_bridge_, estimate_velocity_, download_config_.year/.doy | LD | a second LD function (main; not re-opened per member) |

### 6.2 Order questions

The design records these; it changes none of them. Each is listed for a ruling.

1. [corrected 2026-11-19: see Corrections, item 11] **Profile after validators.** `applyNetworkServiceProfile_` (LN:285) runs after `validateNetworkDatumCoherence_`, `validateNetworkLaneLayers_` and `validateNetworkPhaseDatum_` (LN:282-284), which may read flags the profile arms (the audit's lead 1.2; not re-traced per flag). The table records each arming as PROFILE_OVERRIDE, which makes the order visible.
2. **Bare read before the read site.** `network.carry_coupling_exact_update` is tested at LN:3045 inside a refusal, before its member is read at LN:4915. A row cannot express this; the refusal reads the file, not the member, and stays code.
3. **Keys read in a validator.** `network.start_offset_hours` in `validateNetworkCarryPosture_` (LN:12103-12109). The row belongs in Driver.cpp or Carry.cpp at the same call point.
4. **Member defaults.** Six LN reads default to a process-noise class member and `ura_staleness_q2` to `network_clock_model_q2_`; main `processing.ssr_bias_max_age_s` defaults to `ssr_max_age_s_` (LD:622-623). A row's default can name a member only if that member is final at the row's position.
5. **Posture before the last owners.** Section 3.5.
6. [corrected 2026-11-19: see Corrections, item 11] **The inject helper last.** `loadNetworkInject_` loads last so its warnings read final values (comment at LN:287-289). Its rows keep that position.
7. **Exception order.** A wrong JSON type throws at the read (GnssConfigJsonHelpers.hpp:116-118). Reordering reads changes which key's exception is reported first; 21 test files capture loader text through an ostream sink (for example tests/test_fixed_cycle_config.cpp:162, from the review; not re-opened) and assert on substrings.

---

## 7. Migration steps

[corrected 2026-11-19: see Corrections, item 9] Proof for every step. A member dump tool, generated by a script from the member lists of GnssConfig, NetworkConfig and NetworkKnobs (so it is not hand-maintained), loads each file of config/ and the five audit configurations, builds the knob snapshot and the derived user configuration, and prints every member. The dumps before and after a step, from one build configuration, must be equal. The load log is compared too; the rule "the log may only gain lines" [review] is not enough, because about 55 tests assert absence of a substring: every step re-runs the loader tests, and each step that changes text says so. No engine run belongs to any step.

Wide headers: GnssConfig.hpp (10 601 lines in 7fb0737; 396 including files there, from the review) and GnssConfigTypes.hpp. Only H1 and H2 touch GnssConfig.hpp; no step touches GnssConfigTypes.hpp.

| step | changes | why every member stays identical | dump proof | wide header |
| --- | --- | --- | --- | --- |
| H1 | in GnssConfig.hpp, all at once: `friend struct GnssConfigLoader;`, the friend of the dump tool, `strict_config_` with its getter, the optional `ConfigDecisionLog*` parameter on the private helper declarations (default null), and every private declaration later moves need | no read or write changes; the new member equals its default | dump of the old member set equal; new member at default | GnssConfig.hpp |
| S1 | ConfigDecision.hpp, ConfigDecisionLog, a recording jval that notes ALIAS_SHADOWED; the strict key read (no behaviour yet) | the recording jval returns the same value by the same path | equal; log gains alias records only where both spellings exist (none in config/, 1.2) | none |
| S1b | provenance through the `extends` merge (main LD:149-160 merges plain JSON with no origin): a parallel origin map | the merged document is unchanged | equal | none |
| S2 | verbatim moves of member-function bodies into src/core/config/network/*.cpp; CMakeLists.txt lists each file explicitly | definitions move between source files; text identical | equal; log byte-identical | none |
| S3 (per file) | plain reads become key rows, executed at the original position of each read; read_form and read type preserved | same key, same alias, same conversion, same order | equal; log identical except where a silent replacement gains a line (announced) | none |
| S4 (per file) | messages become `rec.note` with the same text and level, including the INERT lines | the assignment beside each message is untouched | equal; log identical | none |
| S5 | readback per stage, UNATTRIBUTED records, the zero-UNATTRIBUTED test with declared later writers | reads only | equal | none |
| S6 (per file) | cross-key rules become the file's rule table, run at the original call point, in the original order | same conditions, same order, same writes | equal; log identical | none |
| S7 | DerivedUser and StandaloneProfile records by member comparison; the knob map with its test | reads only; Knobs.cpp untouched | equal | none |
| S8 | the run-start table, the manifest record, the dashboard table; posture reads in_effect and moves after the table | no member written | equal; announced output change: posture verdicts move for rows that tested raw members (Posture:343-352, 436-440), and posture leaves paths without `runNetwork` | none |
| S9 | the unknown-key walk at warn in its own block; then strict behaviour behind the default-off key | default off: no member written, no abort | [corrected 2026-11-19: see Corrections, item 8] equal with the key off; log gains UNKNOWN_KEY lines only for files carrying unknown keys | none |
| H2 | old private helper declarations leave GnssConfig.hpp once each helper is gone or a thin forwarder; scheduled with step S5 of the header-fanout draft (HDR_header_fanout.md and its errata), so the header is rebuilt twice in total | declarations only | equal | GnssConfig.hpp |

Timing. LN is edited by most network work, and the network loader on 7fb0737 and main have diverged since e5a7e44. The split (S2 onward) needs the trees merged and a quiet window on the loader files. S1 and the recorder can land earlier on either tree.

Runs owed later (none requested now): a same-build member dump over config/ for every step (a tool run, not an engine run); after S6 and after S8, the network reference configuration and the simulator reference configuration for 2 h each on one build, to confirm unchanged results and to read the first table; strict mode against the three reference configurations before it is offered.

---

## Decisions for the author

1. **Alias column.** Keep it during a warning period that records `alias_used`, then rule on removal (one file in main uses one alias; run-folder copies not scanned). Or drop it at S3 for the network loader, where no file uses one.
2. **Reason list.** Adopt the closed list of 2.3, with KNOB_RESOLVED folded into causes at stage Snapshot (the review's position) rather than kept as a code (the measurement's). A further code for the 93 run-time fallbacks outside the loaders, or leave them out of scope.
3. **Designed decisions.** Which forcings are declared `designed` (the network code-bias forcing of estimate_sat_biases at LN:3942-3946; the user-engine defaults of the derivation at LN:13742-13748; the channel_mode replacement at LN:13846). Only undeclared ones count in strict mode.
4. **The two unread keys** in the reference configurations (`network.min_stations`, `network.redundancy_census`): remove them through the generator manifest, or declare them RETIRED.
5. **Posture report.** Accept the verdict change from reading in_effect, and say whether the user-engine replay path and the Python module print it after the derivation or not at all.
6. **Silent replacements gaining a line.** The 48 silent single-key replacements (33 LN, 15 LD) print a line under `replace_with_log`; accept the log change, or keep them silent and recorded only in the table.
7. **Refusal mechanism.** LN refuses by throw (189) and by error plus `return false` (85); LD by `return false` only. Unify on one under the row's `on_range: refuse`, or keep both.
8. **Order rulings.** The profile after the validators (6.2 item 1) and the posture position are recorded, not changed; any reorder is a separate ruling with its own dump.
9. **Size target.** About 2 000 lines per concern file and 150 lines per function, or another bar.
10. **Non-network output.** logs/config_decisions.jsonl for modes without a network manifest, or the log block only.
11. **Ledger grouping column** on key rows, checked against the configuration ledger, or none.
12. **Convention change** of 5.5, replacing the summary toggle-log line.

## Not checked

- Per-code counts of 1.3 come from text rules over the audit's descriptions; agreement with a hand read was about 85 % on 40 sites. The list's closure rests on a 20-site sample of the unclassified residue.
- The read-site classifier is lexical (a check matched within 45 lines of its read); about 10 sites either way are uncertain. Writes through references and lambdas were resolved only for direct bindings, so the 63 and 6 second-writer counts are lower bounds; not every second writer was re-opened (the filter write-through at LN:1061-1070 and the six LD members were taken from the measurement).
- Alias use was scanned over config/ in both trees only, not over configuration copies in run folders.
- Whether the setup arms at ApplicationNetwork.cpp:270-288 resolve further values was not traced.
- [corrected 2026-11-19: see Corrections, item 11] Whether the validators at LN:282-284 read flags the service profile arms was taken from the audit draft, not re-traced flag by flag.
- How many of the 155 non-plain knob statements map to one key each.
- Which key rows are fingerprinted for warm resume.
- Whether the 51 rewritten LN members change the value in effect on the reference configurations: this needs the table itself, or a run; none was made.
- The test counts quoted from the review (about 55 absence assertions, 21 files capturing loader text) and the include count of GnssConfig.hpp (396) were not re-counted; one example of each was opened.
- The boundary of the trace, manifest and snapshot keys inside LN:2324-3604, and the placement of the eq34, signal-integrity and contract keys, are inferred from key names.
- The user-engine echo lines at initialization (main src/core/realtime/RealTimeEngineInit.cpp:301; RealTimeEngineArObserve.cpp:55) depend on logger levels after the load; not checked per configuration.
- The dashboard table file (nv_tables.py) placement is inferred; only the record filter at epoch.py:181 was opened.

## 2026-11-19: corrections from the check against the code

Trees: N = 7fb0737 (checkout D:/GnssProcessorSupport/worktrees/read_7fb0737), M = main 5ddc422. LN, LD, Knobs and Posture as abbreviated in the body. Items 1 to 3 concern the draft's Outcome, which the revised header replaces; they carry no marker in the body. Full errata: `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/_critical_read/CRT_errata.md`.

1. **Keys read.** The draft's Outcome said the loaders "read 1 243 keys at their read sites". The count is of read sites: 1 104 distinct keys (660 in LN, 444 in LD, table 1.1) at 1 243 read sites.
2. **Concern files.** The draft's Outcome said "thirteen concern files". Section 5.1 lists twelve .cpp concern files plus one internal header (NetworkLoader.hpp).
3. **Steps that change log text.** The draft's Outcome said every migration step "is proved by a member dump comparison; two steps change log text". The dump tool is not yet written, so each step is to be proved by it. Four steps change log text: S1 (alias records), S3 (silent replacements gain lines), S8 (posture) and S9 (UNKNOWN_KEY warnings, since all three reference configurations carry `network.min_stations` and `network.redundancy_census`, which are in no declared universe).
4. **Alias strings (1.2).** "About 925 strings that no configuration in either tree uses" is wrong in detail: 925 counts the alias-bearing sites (665 in LN plus 260 in LD); they hold 916 distinct strings, and one of them (`max_duration_s`) is used by one archived file (config/archive/test_suite/_golden_sim_tri_w6_smoke.json, read at M LD:583).
5. **Throws (1.1, 1.4).** "189 throws" is confirmed as a count but relabelled: 189 throwing sites, namely 164 `throw` statements and 25 calls of the throwing `refuse(...)` lambdas defined at N LN:142 and :1793. Ruled confirmed with a relabel, so the body carries no marker.
6. **Strict-mode validation list incomplete (4.1).** The list of forced keys omits `estimate_ifb: false` (golden_field_refound.json line 147, golden_sim_refound.json line 145, golden_field_bds_refound.json line 154), which the same block forces true (N LN:3945). It passes strict mode only if that forcing is declared designed.
7. **no_bia_code_downweight (4.1).** "Passes with the engine column" is wrong in its reasoning. Under the rule of 4.1, the held-out user row (asked 1.0, in effect 100, N LN:13748) is Changed in an engine that reads the key. The key needs the engine column and passes only if the derivation default at LN:13748 is declared designed (Decision 3).
8. **The two unread keys (4.1, step S9).** The citation `scripts/loader_keys.py:416-420` is N scripts/loader_keys.py:423; no reader exists in src/ or include/. Under 4.2 these keys are in no declared universe, so they fail strict mode as UNKNOWN_KEY, not UNREAD. For step S9: the three reference configurations gain two UNKNOWN_KEY warnings each until the generator drops these keys.
9. **Absence assertions (4.2, 7).** "About 55 tests assert a substring absent from the load log" is not re-counted: N tests/ holds 108 `EXPECT_EQ(x.find(..), npos)` sites in 46 files, aimed at load logs, run-end logs and product files alike. Both examples are wrong: tests/network/test_network_gross_los_cold_bound.cpp asserts on the run-end report `networkGrossLosColdBoundRunEnd` (:365) and tests/network/test_second_band_admission.cpp on `networkSecondBandAdmissionReport` (:529), not on the load. Real load-log absences exist, for example tests/test_fixed_cycle_config.cpp:261 and :263.
10. **Driver.cpp row of the file map (5.1).** "Closing refusals (301-378)" is wrong. `loadNetworkSection_` runs N LN:234-975 (the next function starts at :976); its closing refusals run from :296 to about :954, with the posture call at :955. The row becomes "closing refusals and posture call (296-975)". As drafted, LN:379-975 (including the forcing at :536-542) fall in no row of the file map.
11. **Validator and inject-comment lines (6.2, Not checked).** The three validators sit at N LN:281-283, not LN:282-284; the inject-helper comment sits at N LN:290-292, not LN:287-289. The profile at LN:285 and the carry validator at LN:286 are cited correctly.

### Line numbers that moved

- `scripts/loader_keys.py:416-420` is :423 (N).
- Validators `LN:282-284` are LN:281-283 (N); the inject comment `LN:287-289` is LN:290-292 (N).
- Closing refusals of `loadNetworkSection_`: about LN:296-954 with the posture call at :955 and the function's end at :975 (N), against 301-378 in 5.1 and 295-953 in 1.5.
- Test examples: the run-end assertions sit at test_network_gross_los_cold_bound.cpp:365 and test_second_band_admission.cpp:529, not :367 and :534.
- Knob snapshot count (1.5): an independent count gives 367 `k.x =` statements, 160 not a bare getter or member copy; the difference from 362 / 155 is the six family-parse `prires_x = true` lines, the `prires_on = false` initialiser (Knobs:551-560) and `sig_integrity_on = si.census` (:492). Within the counting convention, confirmed.

### What stands

- The substance of the Outcome stands: no run-start output prints asked beside in effect. The snapshot resolves silently (Knobs.cpp has no logger call) and the derivation pins user values with no line (no logger call in LN:13784-13953 or in `applyNetworkConsumerCouplings`, LN:13710-13782).
- Confirmed as written: the 28 silent clamps or fallbacks, the 10 vehicle gates, the one exclusion (Knobs:425), the 41 manifest knobs (the same record also writes raw members whose knob is resolved, for example `amb_float_drop_var_m2`), the posture rows that count raw members as armed (Posture:343-352, :438-441), the 13 writes of `applyNetworkConsumerCouplings`, the double read of `ura_staleness_q2`, the alias counts, the message counts, the totals of 1.3 and the call points of the snapshot, the derivation and the profile.
- Against the adopted configuration (D:/GnssProcessorSupport/runs_nom/_rap_ad_n146/arm.json: ambiguity_vehicle float_kf, ring fix on, amb_float_drop_jump_k 1.0, gap cover free, zwd_event_inflate_m 0.02, ZWD carried in block): the three posture rows of 3.5 count as armed and print nothing, and the snapshot holds 1.0, Free and 0.02, so asked and in effect agree. The posture defect is real but latent for this configuration, and the posture verdict change of S8 would not alter its output on these rows. The adopted configuration does differ on `estimate_sat_biases` and `estimate_ifb` (asked false, in effect true through LN:3942-3946), and posture reports no values.
- Decisions 1, 2 and 5 to 12 stand as put. Decision 3 must also name the `estimate_ifb` and `promote_sat_code_bias` writes of the same block (LN:3942-3946). Decision 4 stands, with the class of the two keys corrected to UNKNOWN_KEY.
- Steps: H1, S1b, S5 and S7 change no member, default, asserted message or validation order. S2 has two caveats: the anonymous-namespace helpers (LN:84-175, for example `parseCoordEpochYear`, `loadFixedCycleKnobs`) cannot move verbatim into several files and need the internal header, and the change of the public `loadFromJson` signature (2.2) is missing from the list of H1. S3 keeps every member only if the alias is kept: the option of Decision 1 to drop it at S3 changes behaviour for any file using one (copies in run folders not scanned); S3 also adds lines that load-log absence tests may catch. S4 is text-identical. S6 is identical only if early returns and throws inside the replaced blocks keep their short-circuit position. S8 and S9 change output (item 3). No test under N tests/ asserts on POSTURE text. No step reorders the profile against the validators.
- Project rules the design does not discuss: `ConfigDecisionLog` (a class holding rows and emitting log lines) and a befriended `GnssConfigLoader` forming member pointers to private members, against the rule of plain value types and tables with no behaviour classes; the ledger row the new key `processing.strict_config` needs in the same commit; and the invariant that new work is default-off and byte-identical when off, since the S8 table and the posture move are always-on output changes, admissible only as an explained refactor move. The toggle convention change is asked openly (5.5, Decision 12).

### Left unconfirmed

- The per-code counts of 1.3 and the read-site classes of 1.1 beyond their arithmetic; the 63 and 6 two-writer counts; the double-read keys other than `ura_staleness_q2` and `carry_coupling_exact_update`.
- The 21 test files capturing loader text and the 396 files including GnssConfig.hpp.
- Whether the setup arms at ApplicationNetwork.cpp:276-288 resolve further values; whether the validators read flags the profile arms.
- The `_comment` and `//` key counts of 4.2 (the check needed comment stripping for 24 files against the draft's 14; not load-bearing).
- Fingerprinted keys; the user-engine echo lines at initialization.

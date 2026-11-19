# URE: user-engine reactions to correction-stream events at both edges (2026-11-18)

**STATUS**

- **id:** URE
- **topic:** consumer
- **status:** open
- **summary:** A table of the 28 user-engine reactions to correction-stream events, each with its trigger, key, state touched, both edges, bound and test; the S-record tokens traced from writer to parser to reader; gaps ranked by user harm; nineteen default-off fix steps and a test list for plan package 16. The generation change is the least instrumented event (a warn line, no counter, no engine test, partial scope), and under defaults AR eligibility follows the BIA file, not the stream. Integers enter the user filter at every ar_mode other than 0, and the users of the 2026-11-18 runs resolve and hold integers (ar_mode 1), so the integer gaps concern users in use, not only a future configuration (corrections, item 2).
- **source:** plan package 8 of the post-reset program 2026-11-18, "User-side reactions as events at both edges"; commits read: `7fb0737` (branch chain/audit) for network code and the S-record writer, main `5ddc422` for user-engine code. Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** The tables list 28 user-engine reactions to correction-stream events. The phase-bias hold refloat, the freshness flip and the ephemeris jump carry counters, identities and engine tests at both edges; the generation change has a warn line, no counter, no engine test and partial scope. Under defaults, AR eligibility follows the BIA file, not the stream; code-bias discontinuities, flips and declared level steps leave held integers in place; an undecided ephemeris change is unanswered; a standalone user applies biases of any age. The reference configuration (ar_mode 0) leaves the integer gaps latent, but the users of the 2026-11-18 runs resolve integers (ar_mode 1) and are exposed to G2, G3, G7 and part of G13. Nineteen fix steps are proposed.

**Read at.** User-engine code on main `5ddc422` (C:/GnssProcessor). Network code and the
S-record writer on `7fb0737` (D:/GnssProcessorSupport/worktrees/read_7fb0737). Unmarked paths are
main. `SSB` = `src/core/realtime/RealTimeEngineSsrBreaks.cpp`. "(inferred)" marks a claim no single
line shows.

## 0. Posture that sets every severity

The reference configuration's user engine (`config/golden/golden_field_refound.json:281-300`) runs
`closed_loop.ar_mode 0` (no integer resolution), `amb_level_check false`, `pb_step_translate false`,
`pb_hold_presence_flip true`, and `trace_consumer_sat true` (:274), with the network engine
emitting the phase-bias hold. Consequences:

- [corrected 2026-11-19: see Corrections, item 1] Every "held pair kept" gap is unreachable on the reference user. Holding exists only with
  `ar_mode 3` (`src/core/filter/AmbiguityResolver.cpp:3506`; default 0, `src/core/config/GnssConfigLoad.cpp:1984`).
  Those gaps are ranked for a user-integer posture, stated per row.
- Keys are held, so the three disc-counter bodies are bypassed on held keys (SSB:301-302). The live
  phase reactions are the hold refloat (SSB:663-695) and the A5 freshness-flip reset.
- `trace_consumer_sat` feeds `setConsumerSatTrace` on the replay path
  (`src/core/app/ApplicationConsumerReplay.cpp:382`), so trace-only identities exist on the reference
  replay. The in-process closed loop and a standalone user engine were not traced (Not checked).
- The reference file sets none of `ephemeris_jump_reseed`, `ssr_level_step_inflate`,
  `roster_latch`, `ssr_drop_unrecorded`, `bias_coverage_requires_apply`,
  `require_signal_integer_for_ar` (grep); it sets `clock_disc_isb_reinflate_std 10.0` (:155).
  Whether the network-user derivation arms the unset ones beyond what the parts read is Not checked.

## 1. Reaction table

[corrected 2026-11-19: see Corrections, item 7] Order inside `processEpoch` (`src/core/realtime/RealTimeEngine.cpp:481-578`): `refreshDeclaredRoster_`
(:481), `dropBroadcastGrade` and `dropUnrecorded` (:499-500), `computeModels` (re-seeds land here),
predict, slip detection, then the break reactions in the order `reactToGenerationChange`,
`logProducerStatus`, `resetOnPhaseBiasDisc`, `reinflateOnCodeBiasDisc`, `reinflateOnClockDisc`,
`reactToDeclaredLevelSteps_`, `reactToEphemerisJumps_` (:543-549), then update, constraints, AR, and
`checkAmbiguityLevel_` (:578). Inside `resetOnPhaseBiasDisc`: `reactToPbHold_` first, then exactly
one of translate, carry, reset (SSB:264-275). Every break reaction iterates `ctx.modeled_obs`
only (SSB:291, 398, 512, 670). No earlier reaction suppresses a later one.

Columns: Trigger | Key (default) | State touched | Decoupling edge (counter; identity) |
Re-coupling edge (re-validation; counter; identity) | Bound between | Test.

### 1A. Admission and ingest (`RealTimeEngineAdmit.cpp`, `RealTimeEngineIngest.cpp`, `RealTimeEnginePrepare.cpp`)

| # | Reaction | Trigger | Key (default) | State | Decoupling | Re-coupling | Bound | Test |
|---|---|---|---|---|---|---|---|---|
| A1 | Observation/correction sync hold | newest header epoch of any record kind behind the observation (`SsrCorrectionStore.cpp:563-574`) | `processing.enable_obs_ssr_sync` (false), hold 30 s (`GnssConfigLoad.cpp:909-915`) | epoch deferred; keep-latest overwrites the held epoch (`RealTimeEngineAdmit.cpp:94-103`) | debug line every 30th epoch, no counter; outage latch: one warning (:105-110) | `sync_outage_` cleared with no log, no counter (:85-88); cold restart of ambiguities, unnamed (inferred) | hold 30 s | none |
| A2 | Skip to latest | observation lag over 5 s | `processing.enable_obs_skip_to_latest` (false) (:596-603) | skipped epochs; next dt spans them, may set `gap_detected` (Admit:199-200) | debug line only | none | none | none |
| A3 | Duplicate epoch | same epoch again | none | untouched (Admit:174-191) | debug line | n/a | n/a | `tests/test_realtime_defects.cpp:177` |
| A4 | Corrections-ready gate | store not ready at start | `estimation.defer_until_corrections_ready.enabled` (false) (:1766-1777) | estimation start deferred; one-way (`estimation_started_`, Admit:323) | log every 10 epochs with store summary | never re-arms on a later loss | none | none |
| A5d | Drop broadcast-grade | satellite broadcast-grade this epoch | `drop_broadcast_grade` (false) | satellite erased from the epoch (`RealTimeEnginePrepare.cpp:41-50`) | debug count, no identity | silent return | none | none |
| A6 | Unrecorded-satellite gate | no applied record for a satellite | `processing.ssr_drop_unrecorded` (false) (:655-656) | satellite dropped (Prepare:76-157) | DROP line with satellite; run-end counters | READMIT line, flags first sighting vs return | n/a | `tests/test_unrecorded_sat_gate.cpp:31-159` (11 cases, both edges) |
| A7 | Ingest accept policy | record kind not accepted | `corrections.accept` (all) (`src/core/app/ApplicationRealtime.cpp:491`) | record dropped | per-frame counts at trace level (Ingest:28-59, 81-87) | n/a | n/a | `tests/test_realtime_seams.cpp:514-617` |

### 1B. Store readers and correction application (`SsrCorrectionStore`, `GnssCorrections`, `PppModel`, `MeasurementModel*`)

| # | Reaction | Trigger | Key (default) | State | Decoupling | Re-coupling | Bound | Test |
|---|---|---|---|---|---|---|---|---|
| B1 | Orbit/clock older than horizon | `freshEnough` fails on wall-clock `received_time` (`SsrCorrectionStore.cpp:27-43, 187, 220, 233`) | `processing.ssr_max_age_s` (30 s); `ssr_clock_stale_hold_factor` (0) (`GnssConfigLoad.cpp:606-611, 879-896`) | broadcast clock, broadcast-grade (`GnssCorrections.cpp:393-396`); every phase observable erased (`PppModel.cpp:1111-1119`); ambiguities pruned (`MeasurementModelStates.cpp:128`, `StateManager.cpp:643-663`) | one info line per satellite per run (`sats_phase_gated_` only inserted, `PppModel.cpp:1120`); "Pruning N ambiguities" without identities (`StateManager.cpp:661`); per-epoch `clk-applied n/total` count without identity (`RealTimeEngineSsrDiag.cpp:38-48`) | phase returns, ambiguities added fresh; not counted, not named | prune makes the gap a cold restart; broadcast clock error on code rows at normal weight (inferred, unconfirmed) | store window only (`test_realtime_seams.cpp:65, 77, 639, 664`) |
| B2 | Bias older than horizon | gated getter misses (`max_age x stale_hold_factor`) | `processing.enable_ssr_bias_max_age` (false), factor 2.0 (`GnssConfigLoad.cpp:613-640`); both network-user derivations arm it (`GnssConfigLoadNetwork.cpp:13580-13582, 13755-13756`) | gate on: applied term 0, edges handled by C2 and C8; gate off: last record applied forever (`SsrCorrectionStore.cpp:287-305, 432-446`) | gate off: none | gate off: none | gate off: none | gate on: `tests/test_pb_hold_presence.cpp`, `tests/test_consumer_disc_carry.cpp:293`; gate off: none |
| B3 | Record missing for a signal | no record | none; `processing.ssr_drop_unrecorded_phase_bias` (false) (:665-666) | phase bias 0, ambiguity absorbs; code bias 0 | sibling-attribute miss counted 'L'/'C' (`SsrCorrectionStore.cpp:300-302, 377-379, 398-399`); plain absence silent | silent | none | `tests/test_ssr_clock_orbit_pairing.cpp`, `test_realtime_seams.cpp:399, 444` (orbit side) |
| B4 | IOD cutover | broadcast record cuts over while SSR names old IODE | none | baseline pinned to the SSR IODE (`GnssCorrections.cpp:225-242`; `ModelGeometry.cpp:1042, 1382`); no match: SSR clock withheld (:357-358), B1 path | trace line only (:385-389) | B1 return, not counted | B1 | `test_realtime_seams.cpp:176-299`; `tests/test_ssr_store_injection.cpp` |
| B5 | AR coverage gate | `phaseBiasCoveredForAr` false | `model_corrections.bias_coverage_requires_apply` (false) (`GnssConfigLoad.cpp:2573`) | key marked unresolvable each epoch (`MeasurementModelStates.cpp:775-788`); default gate reads BIA-file presence only (`GnssCorrections.cpp:1849, 1873-1875`); key on: follows the age-gated store (`MeasurementModelRow.cpp:86-100, 131-146`) | trace line only | eligible again next search; no re-validation | n/a | not found |
| B6 | Integer flag | `signal_integer` 1->0 or 0->1 | `network.require_signal_integer_for_ar` (false) (`GnssConfigLoadNetwork.cpp:8090-8092`) | key unresolvable (`MeasurementModelStates.cpp:729, 777-788`; `MeasurementModelRow.cpp:117-130`); dropped by AR observe (`RealTimeEngineArObserve.cpp:121-123`); hold not released (inferred) | trace only, no counter | eligible again, no refloat, no counter | none | `tests/test_ar_observe_engine.cpp:195-215`, `tests/test_eq34_consumer_integer.cpp` (no mid-arc flip) |

### 1C. Datum-break reactions (SSB, in call order)

| # | Reaction | Trigger | Key (default) | State | Decoupling | Re-coupling | Bound | Test |
|---|---|---|---|---|---|---|---|---|
| C1 | `refreshDeclaredRoster_` (SSB:52) | declared roster differs (SSB:54-55) | `network.closed_loop.roster_latch` (false) (`GnssConfigLoadNetwork.cpp:13206, 13615-13616`) | band latch in `PppModel` (SSB:65-67; `PppModel.cpp:640-656, 780-864`); no filter state | `decl_changes` (SSB:58), no identity of what changed | none: a decided constellation is not re-latched (`PppModel.cpp:703`, inferred) | n/a | latch logic `tests/test_ledger_emit.cpp:663-700, 751, 789`; no mid-run change |
| C2 | `reactToGenerationChange` (SSB:183) | `getGenerationId()` differs from a nonzero last id (SSB:187-189) | none | [corrected 2026-11-19: see Corrections, item 4] every ambiguity reset to `getAmbiguityStd` (SSB:194-198); receiver code-bias variances to max(var, 0.5 m^2) (SSB:209-216). Untouched: ISBs, satellite biases, ionosphere, RSCB states (separate map, `include/filter/StateManager.hpp:1379`), level-step memory, every reaction memory | one warn line naming both ids (SSB:190-193), no counter; per-key `reset_gen` events under the satellite trace (SSB:202-208); code-bias re-inflations in `reinflateCounters` (`StateManager.cpp:928-929`) | re-seed at next `computeModels` (debug log, uncounted, `MeasurementModelStates.cpp:470-479`); AR arc restart (`AmbiguityResolver.cpp:217`); AmbLevelCheck window restarts through the generation bump (`StateManager.cpp:507, 1883-1886`; `RealTimeEngineEstimate.cpp:528-536`) | reset variance | none at engine level; store id only (`tests/network/test_network_emit.cpp:685-695`), snapshot (`tests/test_rt_state_snapshot.cpp:38, 81`) |
| C3 | `logProducerStatus` (SSB:228) | status state changes (SSB:233) | none | none; sigma0 and withheld list change no trust (SSB:242-245) | log line | log line | none | store parsing only (`tests/test_h_heartbeat_rider.cpp:54`) |
| C4 | `reactToPbHold_` (SSB:663) | nonzero `hold_generation` differing from `PbHoldLedger` memory (`PbHoldLedger.hpp:95-117`) | none | reset on Refloat (SSB:686-693); disc changes on held keys ignored (SSB:683-685, 301-302) | cells, keys, first_sight, held, refloats, disc_ignored with closure (SSB:698-710); `reset_hold_gen` event and info line with both generations (SSB:688-692) | re-seed as C2; generation memory not in the snapshot (covered by `state_snapshot_seam_refloat`, true, `GnssConfigLoad.cpp:3239-3240`) | reset variance | `tests/test_pb_emit_hold.cpp:279-327`; engine `test_pb_hold_presence.cpp:215` |
| C5 | Reset body (SSB:277-362) | disc counter (ungated read) changed against memory (SSB:304-306; `StateManager.cpp:448-459`), or A5 flip | none (default body) | `resetAmbiguity` zeroes row and column, cuts any hold (`StateManager.cpp:499-505`) | info line per key (SSB:310-311); `reset_disc` / `reset_flip` events; A5 flips through `noteFreshnessFlip_` (flips, kind, direction, four attribution classes, capped ARC rows, SSB:92-136); no run-end count of disc resets | re-seed (uncounted); AR arc restart; AmbLevelCheck restart (generation) | reset variance; a bump while the satellite is absent is caught on return (`StateManager.cpp:449-457`) | A5 both edges driven through the engine (`test_pb_hold_presence.cpp:177-195`); memory `tests/test_filter_core.cpp:88-92`; roster-suppression branch and unheld disc reset not driven |
| C5r | Roster suppression of an A5 flip | flip classified Undeclared | `roster_latch` (false) | ambiguity left alone (SSB:337-349) | `flip_suppressed` counter, ARC row | none | none | classifier only (`test_ledger_emit.cpp:669-689`) |
| C6 | `carryOnPhaseBiasDisc_` (SSB:385) | bump or A5 flip | `processing.ssr_phase_bias_disc_carry` (false) (:673-674) | `shiftAmbiguity` by -(applied_now - applied_prev), covariance kept (`PbDiscCarryLedger.cpp:58-64`; `StateManager.cpp:543-546`); refusals take the reset (:41-56); a vanish moves the whole bias into the ambiguity (`test_pb_hold_presence.cpp:198-212`, 92.4 m) | events, disc_bumps, flips, carried, four reset classes with closure (SSB:734-745); `carry_disc` / `carry_flip` events (SSB:442-452); no `noteFreshnessFlip_` (SSB:425-430): no ARC row, no attribution | no re-seed; generation bumped (SSB:433-435), AmbLevelCheck restarts after the shift (only with `amb_level_check`, false) | covariance unchanged | ledger `tests/test_consumer_disc_carry.cpp:190-430`; engine only through the held fixture |
| C7 | `translatePhaseBiasStep_` (SSB:498) | abs(applied step) > 0.005 m on any key (SSB:555-556) | `processing.ssr_phase_bias_step_translate` (false) (:685-689); precedes the carry (SSB:272) | shift by -Delta; declared without pair or refused: reset; undeclared refused: counted no-op (`PbStepTranslateLedger.cpp:112-157`); every A5 flip is ResetFirstSight (:129-134) | counters with closure (SSB:606-621); events `translate_*` (SSB:572-587); RefusedUndeclared has no log line; flips merged into declared_seen / reset_first_sight | as C6 | as C6 | ledger `tests/test_pb_step_translate.cpp:142-448`; no engine translation |
| C8 | `countPbHoldPresenceEdge_` (SSB:712) | A5 flip on a held key | `processing.ssr_pb_hold_presence_flip` (false; inert without the age gate, :718-730); true in the reference user | lets the active body's A5 reaction run on a held key | appear / vanish / suppressed (SSB:715-732); identity from the body | as the body | as the body | `test_pb_hold_presence.cpp:163-231` (reset, carry; not translate) |
| C9 | Code-bias disc (`reinflateOnCodeBiasDisc`) | `getCodeBiasDiscontinuity(sat, code)` changes against key `CB\|sat\|code` (SSB:766-783) | [corrected 2026-11-19: see Corrections, item 5] `estimation.estimate_ifb` (false) (`GnssConfigLoad.cpp:1270-1273`); derivation arms it when biases are consumed (`GnssConfigLoadNetwork.cpp:13742-13747`); off: one warning (SSB:857-883) | every receiver code-bias state of the constellation: var max(var, 0.25 m^2), row and column zeroed, value kept (SSB:848-856; `StateManager.cpp:887-942`) | info line (sat, code); `reinflate_disc` event under the trace (SSB:779-781); state-level raised/kept counts only (`StateManager.cpp:1009-1011`) | none; held pair kept | 0.5 m | primitives `tests/test_reinflate_never_shrinks.cpp:76, 97` |
| C9f | Code-bias freshness flip | gated `hasCodeBias` differs from `ssr_code_bias_applied_` (SSB:792-828) | `enable_ssr_bias_max_age` | as C9, unless roster-suppressed (SSB:803-825) | `noteFreshnessFlip_` counters and ARC rows; memory in the snapshot (`RealTimeEngineStateSnap.cpp:261, 493`) | both directions events; constellation-wide only | 0.5 m | attribution only (`test_ledger_emit.cpp:674-687`) |
| C10 | `reinflateOnClockDisc` | `getClockDiscontinuity(sat)` changes against `SC\|sat` (SSB:908-909) | `estimation.clock_disc_isb_reinflate_std` (0.0; reference 10.0) (`GnssConfigLoad.cpp:1274-1276`) | ISB of the constellation; GPS step: every ISB (SSB:1179-1189); var max(var, std^2), value kept; receiver clock untouched on a white-noise premise (SSB:918-920, unverified) | info line; `reinflate_isb` event recorded at SSB:915 before the `isb_std > 0` test at :927 (mislabels a no-op); ISB raised/kept only | none; held pair kept | ISB std | helper only (`test_reinflate_never_shrinks.cpp:111, 251-259`) |
| C11 | `reactToDeclaredLevelSteps_` | accumulated `lvl=` totals of C and B records differ from `level_steps_seen_` by > 1e-4 m (SSB:997-1016; `SsrCorrectionStore.hpp:555-579`) | `processing.ssr_level_step_inflate` (false), k 1.0 (:735-743); derivation key `network.closed_loop.level_step_inflate` | P += (k sigma)^2 v v^T, v = iono +1, ambiguities +f1^2/f^2 (SSB:1023-1049); refused without iono state, on a frozen pivot, or without ambiguity (:1018-1036) | events, inflated, refused, sigma sum and max at run end (SSB:971-979); identity only via the trace event (:1042); refused events unnamed | none; no generation change; held integers kept; the IF ambiguity is unchanged by v | k sigma per event | store only (`tests/test_clock_level_batch2.cpp:23, 42`) |
| C12 | `reactToEphemerisJumps_` | broadcast record selected as Geometry does changes in IOD or any member, and abs(u.dpos - c dclk) > 10 m (`EphemerisJumpLedger.cpp:13-88`; SSB:1071-1101) | `processing.ephemeris_jump_reseed` (false), 10 m (:746-754) | per target: iono variance jump^2, iono rate decorrelated, satellite-bias variances raised, every ambiguity reset (SSB:1137-1154); pivot jump: every other satellite of the constellation (:1156-1164) | ledger counters with closure (`EphemerisJumpLedger.cpp:69-87`); engine counts (SSB:1166-1177); EPH-JUMP warn line with satellite, sources, IODs, d rho, targets (SSB:1127-1133) | re-seed; held pair released; AmbLevelCheck restarts (`RealTimeEngineEstimate.cpp:528-540`). Undecided change: EPH-CHANGE info line, no response (SSB:1084-1090) | max(ambiguity_std, abs(jump)) | [corrected 2026-11-19: see Corrections, item 6] `tests/test_ephemeris_jump.cpp:126-236, 339-395` (no hold, no undecided case) |

### 1D. Re-validation available to all reactions

| # | Reaction | Trigger | Key (default) | State | Decoupling | Re-coupling | Test |
|---|---|---|---|---|---|---|---|
| D1 | `checkAmbiguityLevel_` (AmbLevelCheck) | windowed mean of WL (cycles) and IF (m) level minus state, k sigma (`AmbLevelCheck.cpp:54-94`; `RealTimeEngineEstimate.cpp:484-555`) | `processing.amb_level_check` (false), window 20, k 4, act (:766-788); false in the reference | act: both ambiguities reset (releases a hold) | `trip`, `refloat` events with identity, run-end counters (Estimate:564-604) | window restarts on any generation change (C2, C4, C5, C6, C7, C12); not on C9, C10, C11; inert on between-satellite rows (Estimate:461-472) | `tests/test_amb_level_check.cpp:131-435`, `tests/test_amb_level_arc.cpp:180-278`, `test_amb_level_statistics.cpp`, `test_amb_level_band_scope.cpp` |

## 2. S-record tokens: writer, parser, readers

[corrected 2026-11-19: see Corrections, item 8] Parser and members: `src/core/products/NetworkProductIO.cpp` and `include/products/NetworkProductIO.hpp`
(identical lines 1-1200 in both trees). Two user-side checks run `checkContract`: the real-time
tail (`ApplicationRealtime.cpp:534-597`, call :576, per S receipt :687-689) and file replay
(`ApplicationConsumerReplay.cpp:215-243`, call :221). Both only compare and warn ("Continuing anyway",
:592-595; replay :236-242). Real time takes the first receipt as the expectation (:539), clears the
pivots (:540), lets four typed pins overwrite (`expect_reference_receiver`, `expect_apriori_product_id`,
`expect_frame_id`, `expect_atx_id`, `expect_orbit_ref`, :541-554; `GnssConfigLoad.cpp:1164-1181`), and
pins only the wind-up yaw id (:565, `include/network/solve/Sbasis.hpp:108`) and wind-up mode (:572)
to the binary and the user configuration. Replay takes the file's last receipt, pins only the
wind-up pair and does not apply the `expect_*` pins (:216-219). The real-time fingerprint
`contractFp` (:513-533) omits `cbpin`, `gfpin`, `sat_gf_datum`, `pbhold*`, `discfrac`, `discpv`,
`discscope`: a receipt changing only these returns at :536 unchecked. No file in `config/` of either
tree sets `expect_frame_id`, `expect_atx_id` or `expect_orbit_ref`. The in-process closed loop runs
no contract check (inferred from the comment at `ApplicationConsumerReplay.cpp:210-212`). The RTCM
SSR path carries no S record.

| Token | Writer (7fb0737) | Parser | Readers | Disagreement | Effective? |
|---|---|---|---|---|---|
| `wumode`, `windup_id` | `ApplicationNetworkSetup.cpp:1779`; serialized :613-614 | :357-362 | `checkContract` :980-994; RT :532, :565, :572, :582-585; replay :190-191, :219, :226-229 | warn; absent mode: info "unverified" | yes (only pair pinned to the user side), warn only |
| `cbpin` | `src/core/network/emit/SolPrep.cpp:1156-1165` per emit; :559-560 | :372-379 | `checkContract` :1019-1026 against the first receipt; outside the fingerprint | nothing; the user's own code pair never compared; a mid-stream move never seen | no |
| `gfpin` | none (SolPrep.cpp:1122-1125); rewritten only from an old stream (:571-572) | :380-390 | `checkContract` :1033-1041 | nothing | no (dormant) |
| `sat_gf_datum` | SolPrep.cpp:1173-1179 (`broadcast_tgd` in the reference); :580-581 | :391-398 | **none** in either tree; not compared (hpp:351) | nothing | no reader |
| `frame` | ApplicationNetworkSetup.cpp:1746 (`network.frame_id`, empty default); :511-512 | :339-340 | `checkContract` :1111-1113 if expectation non-empty; RT :530, :549-550 | warn only with a typed pin, real time only | no |
| `cepoch` | ApplicationNetworkSetup.cpp:1747 (SIF year default, `GnssConfigLoadNetwork.cpp:1248-1251`); :513-514 | :341-342 | `checkContract` :1114-1116; RT fingerprint :530; no pin key | mid-stream change only | no |
| `atx` | ApplicationNetworkSetup.cpp:1748; :515-516 | :343-344 | `checkContract` :1117-1119; RT :530, :551-552 | user ATX path never compared | no |
| `oref` | ApplicationNetworkSetup.cpp:1754, fixed "CoM"; :517-518 | :345-346 | `checkContract` :1120-1122; RT :553-554. PCO choice comes from the RTCM dialect tag instead (`RtcmSsrDecoder.cpp:461, 589` -> `SsrCorrectionStore.cpp:215, 228` -> `ModelGeometry.cpp:1057, 1390`) | warn with a typed pin | no |
| `pbhold` (1, 2, 3) | :603-605 (legend at :670-676 declares only `pbhold=1`) | :363-369 | `checkContract` :1083-1093 against itself; outside the fingerprint | nothing | no |
| `bias_sign`, `isb_datum`, `emit_cb`, `cb_datum` | (writer not re-traced) | (parser) | `checkContract` :977-1000 and fingerprint, against itself | mid-stream change only | no |
| `pdr` | SolPrep.cpp:1193; EmitStore.cpp:554 | :347-348 | none | n/a | no reader |
| `hsig` | ApplicationNetworkSetup.cpp:1791 | :370-371 | none | n/a | no reader |
| `agek` | ApplicationNetworkSetup.cpp:1785 | :349-350 | **adapts**: `store.setCombinedSigmaAgeCoeff` (NetworkProductIO.cpp:1368; `SsrCorrectionStore.cpp:531`) | n/a | yes |
| `amb_datum` | (writer not re-traced) | (parser) | replay only, measure-only gate (`ApplicationConsumerReplay.cpp:388-390` -> `RealTimeEngineArObserve.cpp:121, 131`) | n/a | yes, replay |
| `disc_thresh_m`, `emit_clock_disc`, `stale`, `cbjump`, `clkset`, `discfrac`, `discpv`, `discscope` | (not re-traced) | (parser) | adapt: `makeReplayEmitter` (:1132-1144), real time only before the first replayed epoch (:1329-1333); replay :363 | n/a | yes |
| `ref_rx`, `apriori_id`, `gid` | (not re-traced) | (parser) | compared, optional pins in real time; a-priori id steers replay products (:271, :375) | warn | partly |
| clock pivots | (not re-traced) | (parser) | cleared in both expectations | n/a | no |

Tokens nothing reads: `sat_gf_datum`, `pdr`, `hsig`. Tokens read only in a self-comparison: `cbpin`,
`gfpin`, `cepoch`, `pbhold`, `bias_sign`, `isb_datum`, `emit_cb`, `cb_datum`, clock pivots; `frame`,
`atx`, `oref` unless a pin is typed in. No user-engine folder (`src/core/realtime`, `model`, `filter`,
`corrections`) reads any convention member (grep for `frame_id`, `coord_epoch`, `atx_id`,
`orbit_ref`, `windup_mode`, `cb_pin_pair`, `gf_pin_pair`, `sat_gf_datum`).

Silent harm by token: `sat_gf_datum` (c*T_GD on every code bias between `zero` and `broadcast_tgd`,
NetworkProductIO.cpp:573-579; no user-side T_GD term was found on main, so no double count today,
inferred from one grep); `cbpin` (mixed-pair penalty, :1013-1018); `atx` (satellite- and
nadir-dependent, partly absorbed); `frame` / `cepoch` (coordinate offset and plate-motion drift,
invisible to residuals); `oref` (harmless while orbits are SP3; a PCO double-apply once an
APC-referred source lands, ApplicationNetworkSetup.cpp:1749-1753); `pbhold` (a user misreading a
bump without `gen=`).

## 3. Gaps by priority

[corrected 2026-11-19: see Corrections, item 2] Each gap names the posture where it is live: R = reference user (no integer resolution), S =
standalone user engine (no network-user derivation), I = user integers (`ar_mode 3` with defaults:
`hold_release_cyc 0`, `amb_level_check false`, `bias_coverage_requires_apply false`).

**G1. Generation change is the least instrumented whole-state event (R, S, I).** SSB:183-216.
No counter (warn line only, SSB:190-193); identity per key only under the satellite trace; no
engine test. Scope partial: ISBs re-inflated only if the clock counter moves (a re-seed from 0 to 0
does not, `RealTimeEngine.hpp:1100-1105`), RSCB states untouched (`StateManager.hpp:1379`),
satellite biases, ionosphere and every reaction memory (`ssr_phase_bias_applied_`, carry and
translate ledgers, hold generations, disc memory, `level_steps_seen_`) kept. The per-signal bodies
then react again in the same epoch; a translate or carry shift of a just-reset key is overwritten by
the re-seed (`StateManager.cpp:501-502`) but counted as a translation. User suffering: slower
re-convergence after a network restart (ISB and code-bias levels carried across a re-realization),
misleading run-end counters; no wrong integer (the skeptic's correction; the first stage claimed
one).

**G2. AR eligibility does not follow the correction stream under defaults (I).**
`MeasurementModelStates.cpp:775-788` with `bias_coverage_requires_apply` false
(`GnssConfigLoad.cpp:2573`): the gate reads BIA-file presence (`GnssCorrections.cpp:1849, 1873-1875`).
With a BIA file loaded, a key stays an AR candidate through any stream absence of its phase bias;
without one, every key is unresolvable (inferred). Combined with the carry (C6), a vanished bias
leaves the whole bias in an AR-eligible ambiguity at hold variance. User suffering: a wrong fixed
integer or a fractional DD offset held. Replaces the first stage's "no candidate gate" claim, which
the code refutes.

**G3. A code-level change on one satellite keeps a held pair (I).** C9, C9f, C11: none releases,
shifts or re-tests the hold; C11 loosens only along v (the IF ambiguity is unchanged) and does not
bump the generation, so D1 is not restarted. Per-satellite steps land on the satellite's ionosphere
and wide lane (inferred, SSB:840-847; `RealTimeEngine.hpp:1164-1175`). C10 has the same exposure for
a clock step not common to the constellation. User suffering: a wide-lane error held under
hold variance until the arc ends.

**G4. Undecided ephemeris change unanswered (any posture with `ephemeris_jump_reseed`).**
SSB:1084-1090: an EPH-CHANGE line and a count, every state and hold kept through a broadcast change
of unknown size. Also the jump test reads the broadcast record selected by the SSR IOD
(SSB:1078-1082), not broadcast plus correction: a compensated switch may re-seed and a corrected
step at a matched cutover may pass (both inferred; one simulator case settles both).

**G5. No bias age limit for a standalone user (S).** `enable_ssr_bias_max_age` false: biases of any
age applied (`SsrCorrectionStore.cpp:287-305, 432-446`); no A5 edge; a joint clock-and-bias level
change reaches the ambiguity as an undeclared step that the frozen counter cannot declare. Both
network-user derivations arm the gate, so R is not exposed.

**G6. Roster suppression of an Undeclared flip with a record on the stream (R if `roster_latch` is
armed by the derivation, I).** `SignalDeclaration.cpp:139-142` returns Undeclared before testing
`on_wire`; SSB:337-349 suppresses. A fresh-to-stale flip is a real step of the applied term left in
the ambiguity. Reachability on a real stream unconfirmed.

**G7. Integer-flag edges are not events (I with `require_signal_integer_for_ar`).** C6/B6: 1->0
releases no hold and is not counted; 0->1 re-admits without re-validation; under the hold the
paired counter step is ignored (SSB:301-302; network pairing at 7fb0737
`src/core/products/NetworkProductEmitter.cpp:652-678`), so a revoked integer stays imposed.
`wide_lane_integer` is decoded (`RtcmSsrDecoder.cpp:412`) and never read by the user engine.

**G8. Broadcast-grade departure and return are unnamed (R, S, I).** B1/B4: one log per satellite
per run, a prune count without identities, a per-epoch applied count without identities; return
uncounted. The named form exists only under `ssr_drop_unrecorded` (A6). Code-row weight during the
gap not traced.

**G9. Admission edges silent and untested (R, S).** A1 recovery unlogged and uncounted, overwritten
held epochs lost silently, sync horizon shared across record kinds (a bias push can release the hold
while clocks are old, inferred); A2 uncounted; A4 one-way; A5d no identities. No engine test of A1,
A2, A4, A5d.

**G10. Reaction memories lost on a restore.** `level_steps_seen_` (only SSB:1001-1003) and the
ephemeris ledger (by design, `RealTimeEngine.hpp:1471-1474`) absent from the snapshot; hold
generations absent (covered while `state_snapshot_seam_refloat` stays true). User suffering: a step
across the outage read as a first sighting.

**G11. Misnamed or missing event records (R, S).** `reinflate_isb` recorded before the `isb_std`
test (SSB:915 vs :927); translate folds A5 flips into declared_seen / reset_first_sight; carry and
translate never call `noteFreshnessFlip_` (SSB:425-430); RefusedUndeclared has no log line; no
run-end count of disc resets (C5), code-bias disc events (C9) or clock disc events (C10); re-seeds
uncounted; refused level steps unnamed; C1 names nothing.

**G12. S-record conventions are warn-only and mostly self-compared (R, S).** Section 2. The
fingerprint omission hides every mid-stream `cbpin` move that the network engine logs (7fb0737
SolPrep.cpp:1156-1165).

**G13. Ledger hygiene (I, inferred).** The carry and translate ledgers never forget a key
(`PbDiscCarryLedger.hpp:133`, `PbStepTranslateLedger.hpp:153`): a returning satellite seeded with
the current bias may be shifted by the Delta accumulated over the gap (bounded by the initial
variance; the seed term not traced). `shiftAmbiguity` bumps the generation, so under
`refloat_partner_signal` (false, `GnssConfigLoad.cpp:2127-2128`) a carried signal refloats the
whole satellite and held pairs are erased with no log line (`AmbiguityResolver.cpp:250-289`).

**G14. Observation without reaction (R, S).** C1 after the latch, C3 status transitions. Coverage
line ages records at 60 s against the 30 s apply limit (`RealTimeEngineSsrDiag.cpp:46`;
`SsrCorrectionStore.hpp:716-717`), visible on the same line as the applied count.

## 4. Proposed fixes

M = measure-only (counter, identity, log; identical output by construction: no state, no
observation set changes). B = behaviour behind a new key, default off, identical output when off.
Each step names its unit case; cases are listed in section 5.

| Step | Gap | Kind | Change | Unit case |
|---|---|---|---|---|
| F1 | G1 | M | `generation_changes` counter, ambiguity-reset count and a run-end line; `GEN-CHANGE` info line naming old id, new id and per-constellation reset counts, independent of the trace | T01, T02 |
| F2 | G1, G11 | M | Counter `reseeds_done` in the re-seed path (`MeasurementModelStates.cpp:470-479`) keyed by cause (generation, disc, flip, hold, ephemeris, level check) | T03 |
| F3 | G1 | M | Per-signal bodies skip counting a key already reset by the generation change in the same epoch (counter attribution only; the value path unchanged) | T04 |
| F4 | G1 | B | `processing.ssr_generation_full_scope` (false): on a generation change also re-inflate ISBs, RSCB states, satellite biases, and clear the reaction memories (applied flags, carry and translate ledgers, level-step memory) | T05, T06 |
| F5 | G2 | M | Counter and identity of AR-eligible keys whose phase bias is not applied this epoch (`eligible_without_applied_pb`), per satellite and signal | T07 |
| F6 | G2 | B | Arm `bias_coverage_requires_apply` in the user-integer posture (existing key; author decision, no code) or a new key making the coverage gate follow the gated store when the BIA file is absent | T08 |
| F7 | G3 | M | Counter `held_pairs_through_code_step` when C9, C9f, C10 or C11 fires on a satellite with a held pair; identity line | T09 |
| F8 | G3 | B | `processing.code_step_release_hold` (false): C9f and C11 bump the satellite's ambiguity generation (restarting D1 and releasing held pairs through the existing release path) | T10, T11 |
| F9 | G4 | M | `undecided_changes` already counted; add satellite identity to the run-end line and a counter of states left untouched | T12 |
| F10 | G4 | B | `processing.ephemeris_undecided_reseed` (false): treat an undecided change as a jump of size `ambiguity_std` | T13 |
| F11 | G6 | B | `processing.roster_suppress_requires_off_wire` (false): suppress only when the record is absent from the stream (test `on_wire` before the declaration) | T14 |
| F12 | G7 | M+B | M: counters `signal_integer_drops`, `signal_integer_returns` with identity. B: `network.closed_loop.signal_integer_release_hold` (false): a 1->0 edge resets the ambiguity | T15, T16 |
| F13 | G8 | M | Per-satellite DEPART / RETURN lines and counters for broadcast-grade transitions (reuse the A6 record format, without dropping) | T17, T18 |
| F14 | G9 | M | Counters and lines: sync deferrals, overwritten held epochs, outage recovery, skipped epochs, broadcast-grade drops with identities | T19, T20, T21 |
| F15 | G10 | B | `processing.snapshot_reaction_memory` (false): write `level_steps_seen_` and hold generations to the snapshot; restore them | T22 |
| F16 | G11 | M | Move the `reinflate_isb` event after the `isb_std > 0` test (record a `reinflate_isb_skipped` event otherwise); run-end event counts for C5, C9, C10; RefusedUndeclared log line; carry and translate call `noteFreshnessFlip_` for A5 flips; translate counts flips apart from first sightings | T23, T24, T25, T26 |
| F17 | G12 | M | Add `cbpin`, `gfpin`, `sat_gf_datum`, `pbhold*`, `disc*` to `contractFp`; warn on `sat_gf_datum` when the user applies its own group delay (no such term today); log `frame`, `cepoch`, `atx`, `oref` once at the first receipt beside the user's own ATX basename | T27, T28 |
| F18 | G13 | M | Counter `ledger_cross_arc_shift` when a carry or translate shifts a key whose ambiguity was added after the ledger's last entry; log line when held pairs are erased by a partner refloat | T29, T30 |
| F19 | G14 | M | Coverage line ages at `ssr_max_age_s`; C1 logs the declaration delta (families added and removed) | T31 |

Note on "identical output": the F16 event move changes trace content (a no-op event becomes a
skipped event). Trace files differ; solution output does not. The author decides whether trace
content counts as output (Decisions).

## 5. Test list handed to plan package 16

One line per case: reaction, edge, synthetic input, expected counter.

- T01. C2 generation change, decoupling: store generation id 7 -> 8 with two ambiguities held; `generation_changes == 1`, `amb_reset_by_generation == 2`.
- T02. C2, first receipt: id 0 -> 7; `generation_changes == 0`.
- T03. C2 re-coupling: next epoch after T01; `reseeds_done[generation] == 2`.
- T04. C2 plus C6 same epoch: generation change and disc bump on one key with carry armed; `carried == 0`, `reset_by_generation == 1`.
- T05. F4 off: generation change; ISB variances unchanged, carry ledger unchanged (byte-identical state to today).
- T06. F4 on: generation change; ISB and RSCB variances raised, carry ledger empty, `level_steps_seen_` empty.
- T07. B5 default gate: BIA file present, stream phase bias aged out, `ar_mode 3`; `eligible_without_applied_pb == 1`.
- T08. B5 key on: same input; key marked unresolvable, `eligible_without_applied_pb == 0`.
- T09. C11 with a held pair: declared `lvl=` step of 0.3 m on one satellite with ar_mode 3 and a hold; `held_pairs_through_code_step == 1`, `level_step_inflated == 1`.
- T10. F8 off: same input; ambiguity generation unchanged.
- T11. F8 on: same input; ambiguity generation bumped, `held_pairs_` no longer contains the pair after the release check.
- T12. C12 undecided: old record outside its fit window, new IOD; `undecided == 1`, identity line names the satellite.
- T13. F10 on: same input; `ambiguity_resets == n` for the satellite's ambiguities.
- T14. C5r roster suppression: declaration without the signal, record present, freshness fresh -> stale; today `flip_suppressed == 1`; with F11 on, `flip_suppressed == 0` and the key reset.
- T15. B6 1->0: `signal_integer` drops on a held key; `signal_integer_drops == 1`, hold kept with F12 off.
- T16. B6 with F12 on: same input; ambiguity reset, generation bumped.
- T17. B1 departure: clock record ages past `ssr_max_age_s`; `bg_depart == 1` with the satellite named, ambiguities pruned.
- T18. B1 return: fresh record next epoch; `bg_return == 1`, return flagged as not a first sighting.
- T19. A1 outage and recovery: sync armed, corrections stop for 40 s, then resume; `sync_outage == 1`, `sync_recovery == 1`.
- T20. A1 keep-latest: two epochs held while waiting; `held_epoch_overwritten == 1`.
- T21. A2 skip: observation lag 8 s; `skipped_epochs == k`.
- T22. F15 on: level step recorded, snapshot written and restored, second step; `level_step_events == 2` (today 1 plus a first sighting).
- T23. C10 with `isb_std == 0`: clock disc bump; no `reinflate_isb` event, `reinflate_isb_skipped == 1`.
- T24. C5 unheld reset: disc counter step on an unheld key; `disc_resets == 1` at run end.
- T25. C6 A5 vanish with carry: freshness fresh -> stale; ARC row written, `flips == 1`, attribution class recorded.
- T26. C7 A5 flip with translate: freshness flip; `translate_flips == 1`, `reset_first_sight == 0` for the flip.
- T27. S record: second receipt changing only `cbpin`; `contract_mismatch == 1` (today 0, skipped at :536).
- T28. S record: first receipt; one log line naming `frame`, `cepoch`, `atx`, `oref` beside the user's ATX basename.
- T29. C6 cross-arc: key carried, satellite absent, new ambiguity added, bias moved meanwhile, bump on return; `ledger_cross_arc_shift == 1`.
- T30. G13 partner refloat: carry shift with `refloat_partner_signal` on; held-pair erase line written, counter 1.
- T31. Coverage line: record aged 45 s with `ssr_max_age_s 30`; line reports it as not applied.
- T32. C9 code-bias disc through the engine (no test today): counter step on one (satellite, code); receiver code-bias variance raised for the constellation, `cb_disc_events == 1`.
- T33. C9f code-bias flip through the engine: fresh -> stale and back; two flips, two ARC rows.
- T34. C11 refused: level step without ionosphere state; `level_step_refused == 1`, identity line written.
- T35. C12 held pair: jump of 20 m with ar_mode 3 hold; ambiguities reset, `held_pairs_` released.

## 6. Merge record: first stage against the skeptic

Kept as confirmed by the skeptic: generation change warn-only with partial scope (RSCB separate
map); `reinflate_isb` ordering; level step leaves the IF ambiguity unchanged and restarts nothing;
undecided ephemeris change unanswered; `sats_phase_gated_` insert-only; bias age gate off by default
for a standalone user; `sat_gf_datum` has no reader; four tokens outside the fingerprint; Undeclared
returned before `on_wire` (spot-checked here at `SignalDeclaration.cpp:139-142`).

Dropped or corrected:
- "No level re-validation after a generation change, carry or translate": wrong. Generation bumps
  restart AmbLevelCheck (`StateManager.cpp:507, 1883-1886`, spot-checked; Estimate:528-536).
- "Wrong integer at a restart" (generation change): unsupported; the re-seed overwrites the shift.
  Kept as a counter misattribution (G1).
- "No candidate gate on an applied phase bias": wrong as worded; the gate exists
  (`MeasurementModelStates.cpp:775-788`, spot-checked) but defaults to BIA-file presence (G2).
- "No engine test of the A5 flip": wrong; `test_pb_hold_presence.cpp:177` drives both edges.
- "`agek` has no reader": wrong; it adapts (`NetworkProductIO.cpp:1368`, spot-checked).
- "No counter of broadcast-grade clocks": partly wrong; `clk-applied n/total` exists without
  identity.
- `sat_gf_datum` double count: not reachable today (no user T_GD term found).
- Presence-flip "knob off" gap: the reference user arms the knob; the gap applies to other postures.

Disagreements left open: the ephemeris cutover (spurious re-seed against an unanswered corrected
step) has one cause and needs one simulator case of a matched and an unmatched IOD cutover.
Reachability of G6 needs one stream with a record on an undeclared signal. Both are listed, not
requested.

Runs needed later (none requested): one simulator case of IOD cutovers with and without the SSR
naming the new IOD (G4); one simulator case of a carried vanish followed by AR with ar_mode 3 (G2);
one injected generation change on the simulator replay to measure re-convergence with F4 off and on
(G1); one injected per-satellite `lvl=` step with ar_mode 3 (G3).

## Decisions for the author

1. Whether the user-integer posture arms `bias_coverage_requires_apply` (F6). Without it, AR
   eligibility ignores the correction stream (G2).
2. Whether a generation change should clear the reaction memories and re-inflate ISBs, RSCB states
   and satellite biases (F4), or stay scoped to ambiguities and receiver code biases.
3. Whether a per-satellite code-level change (declared or by flip) should release a held pair
   (F8), given that receiver code-bias states are per constellation.
4. Whether an undecided ephemeris change is answered as a jump of nominal size (F10).
5. Whether roster suppression should require the record to be absent from the stream (F11).
6. Whether trace content counts as output for the identical-output rule (F16 changes a mislabelled
   trace event).
7. Whether S-record conventions stay warn-only or a mismatch against the user's own configuration
   (ATX, frame, wind-up mode) refuses the stream behind a key.
8. Whether `sat_gf_datum` gets a user reader now or when a user group-delay term lands.
9. Priority between the measure-only batch (F1, F2, F5, F7, F9, F13, F14, F16, F17) and the keyed
   behaviours; the measure-only batch is proposed first.

## Not checked

- The in-process closed loop: whether `trace_consumer_sat` reaches `setConsumerSatTrace` there, and
  that it runs no contract check (comment only).
- The network-user derivation functions beyond the lines cited (which unset keys they arm,
  including `roster_latch` and `ephemeris_jump_reseed`).
- Whether the user receiver clock is white noise in every configuration (premise of C10).
- Code-row weight of broadcast-grade satellites (only `broadcast_only` was searched).
- Whether the ambiguity seed includes the SSR phase bias (G13 premise).
- Whether a network-engine code-bias discontinuity and a declared level step move in one epoch
  (double response C9 plus C11).
- Whether every AR DD reference is within one constellation (C10 cancellation argument).
- Whether the latch is ever re-decided after a roster change (not every writer of
  `constellation_canonical_decided_` traced).
- Whether streams of the reference configuration carry `cepoch=` (depends on SIF resolution).
- Writers of `bias_sign`, `isb_datum`, `emit_cb`, `cb_datum`, `amb_datum`, the `disc*` tokens,
  `ref_rx`, `apriori_id`, `gid` (marked "not re-traced" in section 2).
- `estimate_ifb false` at line 147 of the reference file: which section it belongs to (network
  engine or user engine) was not read.
- Line numbers in this document were taken from the first-stage parts; nine were spot-checked
  (SSB:186-198, :913-928, :1084-1091; `SignalDeclaration.cpp:136-143`;
  `NetworkProductIO.cpp:1366-1369`; `StateManager.cpp:499-508, 1880-1887`;
  `MeasurementModelStates.cpp:775-788`; `GnssConfigLoad.cpp:2571-2574`; reference file :270-301;
  `test_pb_hold_presence.cpp` case lines). The rest are unverified by the writer.

## 2026-11-19: corrections from the check against the code

The check read user-engine code on main `5ddc422`, the network writer and S-record checks on both trees, and the `arm.json` of two user runs of 2026-11-18 (`_rap_nw_obsadx_143_quin`, `_rap_nw_obsadf_144_badg`). Its verdict: usable with corrections; the six top findings hold in the code, and the configuration that sets their severity is wrong for the users of those runs. Items marked in the body carry the item number below; the original words stand above.

1. **Section 0, bullet 1: "Holding exists only with `ar_mode 3`".** The code shows that integers enter the filter at every ar_mode other than 0: a successful fix installs the fixed state and covariance (`src/core/realtime/RealTimeEngineEstimate.cpp:279-282`, main), as a Kalman pseudo-observation with R = hold_variance (`src/core/filter/AmbiguityResolver.cpp:2440-2442`, main); held-pair memory fills whenever `hold_release_cyc > 0` (`AmbiguityResolver.cpp:511-514`, main); ar_mode 3 only adds `holdAmbiguities` (`AmbiguityResolver.cpp:3506`, main).
2. **Section 3, posture I ("`ar_mode 3` with defaults") and the severity frame of section 0 and the draft's Outcome.** I is better defined as user integers at ar_mode 1 or higher. The users of the 2026-11-18 runs (both `arm.json`) are I: `ambiguity_resolution.mode 1`, `closed_loop.ar_mode 1` (the derived user takes `cl.ar_mode`, `src/core/config/GnssConfigLoadNetwork.cpp:13724`, main), `hold_release_cyc 0.25`, `hold_variance 1e-07`, `refloat_partner_signal true`, `require_signal_integer_for_ar true`, between-satellite user (`consumer_undifferenced false`).
3. **The draft's Outcome: "On the reference user engine (no integer resolution) the integer gaps are latent."** True only for the reference configuration (ar_mode 0); the users of the 2026-11-18 runs resolve integers (ar_mode 1) and are exposed to G2, G3, G7 and the partner-refloat part of G13. The Outcome above is rewritten accordingly.
4. **C2, State: "receiver code-bias variances to max(var, 0.5 m^2)".** The value 0.5 m is a standard deviation; the variance floor is 0.25 m^2 (SSB:209-210; `src/core/config/GnssConfigLoad.cpp:1272-1273`, main).
5. **C9, Key: "derivation arms it when biases are consumed (`GnssConfigLoadNetwork.cpp:13742-13747`)".** Those lines are the standalone profile only; the closed-loop user copies the network configuration, where `estimate_sat_code_bias` arms `estimate_ifb` (`GnssConfigLoadNetwork.cpp:3866-3869`, main).
6. **C12, Test: "(no hold, no undecided case)".** An undecided case exists at ledger level, `OldRecordBeyondItsFitWindowIsUndecided` (`tests/test_ephemeris_jump.cpp:187-202`, main); no engine-level undecided case and no hold case exist.
7. **Section 1, call order: roster, drops, then `computeModels`.** `computeModels` (`src/core/realtime/RealTimeEngine.cpp:483`, main) precedes `dropBroadcastGrade` and `dropUnrecorded` (:499-500). The order is `refreshDeclaredRoster_` (:481), `computeModels` (:483), `dropBroadcastGrade` and `dropUnrecorded` (:499-500).
8. **Section 2: "lets four typed pins overwrite".** Five pins are listed and coded (`src/core/app/ApplicationRealtime.cpp:541-554`, main).
9. **Section 0, last bullet (addition, not a refutation).** The keys are unset in the reference file as stated; the `arm.json` of the 2026-11-18 users sets `network.require_signal_integer_for_ar` true, leaves the other five unset and sets `clock_disc_isb_reinflate_std` 10.0.
10. **G2: "without one, every key is unresolvable (inferred)" (refinement).** Confirmed by code, not inferred, outside the NlWl basis: `src/core/measurement/MeasurementModelRow.cpp:132-135` with `MeasurementModelStates.cpp:783-786` (the NlWl basis returns early at :738), main.
11. **Decision 9: measure-only batch "(F1, F2, F5, F7, F9, F13, F14, F16, F17)".** F3, F18 and F19 are also measure-only (section 4) and are missing from the list; either they join it or the reason for leaving them out is stated.

### Line numbers that moved

- C2 and section 6: `StateManager.cpp:507` is a log line; the generation bump is at :505 (`markAmbiguityPendingReseed`), reaching :1885 and then :341 (`StateManager.cpp:505, 1883-1886, 339-343`, main). The claim itself is confirmed.
- G6 and section 6: the Undeclared return before `on_wire` is at `src/core/products/SignalDeclaration.cpp:141-142` (main), inside the cited 139-142.
- G13: the partner refloat logs PARTNERREL at `AmbiguityResolver.cpp:277` and erases held pairs silently at :283-290 (main), inside the cited 250-289.
- C9f, C10 and G11: `reinflate_*` labels appear in tests only as trace labels (`tests/test_consumer_sat_trace.cpp:51, 94`, main); no test calls `getCodeBiasDiscontinuity` or `getClockDiscontinuity` outside network tests.

### What stands

The top findings stand as written: the generation change has a warn line and no counter or engine test, with partial scope (ambiguities and receiver code-bias states only; ISB, RSCB, satellite biases, ionosphere and reaction memories untouched); the default coverage gate reads the BIA file (`bias_coverage_requires_apply` false); code-bias discontinuities, flips and declared level steps leave integers in place; an undecided ephemeris change is unanswered; a standalone user has no bias age limit by default (both network-user derivations arm it); the `reinflate_isb` event is recorded before its test; `sat_gf_datum`, `pdr` and `hsig` have no reader; `agek` adapts; integer-flag 1->0 is a veto only, with no reset and no counter; the counts (28 reactions, 19 fix steps: 13 measure-only and 7 keyed, one both) are right.

What does not stand is the severity frame. The users of the 2026-11-18 runs run ar_mode 1 with integers installed in the filter, so the decisions read as follows after correction:

1. Arming `bias_coverage_requires_apply` for users that fix integers (F6): concerns users already in use (G2).
2. Scope of a generation change (F4): stands as written.
3. Release of a held pair on a per-satellite code-level change (F8): concerns users already in use (G3).
4. Answer to an undecided ephemeris change as a jump of nominal size (F10): stands.
5. Roster suppression only when the record is absent from the stream (F11): stands.
6. Whether trace content counts as output for the identical-output rule (F16): stands.
7. S-record conventions warn-only or refusing behind a key: stands.
8. A user reader for `sat_gf_datum` now or with a user group-delay term: stands.
9. Priority of the measure-only batch over the keyed behaviours: stands, with the list completed by F3, F18 and F19 or the reason for leaving them out (item 11).

The keyed behaviour of F12 (a 1->0 integer-flag edge resets the ambiguity) also concerns users in use: the 2026-11-18 users arm `require_signal_integer_for_ar`.

### Left unconfirmed

- The fingerprint omission list (`contractFp`, `ApplicationRealtime.cpp:513-533`) and the `checkContract` line numbers.
- Whether the 2026-11-18 users load a BIA file (no bias path in `arm.json`; PathResolver may supply one), which decides whether their AR runs under the default gate at all.
- Whether `network.estimate_sat_code_bias` maps to the member tested at `GnssConfigLoadNetwork.cpp:3866`.
- Line numbers of rows A1 to A7, B1, B3, B4, C1, C3 to C8, and the test list of section 5.
- Release behaviour of held pairs at `hold_release_cyc 0.25` after a code step: "held until the arc ends" in G3 may be too strong for these users.

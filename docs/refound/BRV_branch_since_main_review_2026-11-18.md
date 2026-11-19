# BRV: review of the chain/audit branch changes since main (2026-11-18)

**STATUS**

- **id:** BRV
- **topic:** network-vehicle
- **status:** open
- **summary:** A single-pass review of the 68 code, test and build files changed from the merge base e5a7e44 to 7fb0737 (50 commits, 85 files), in five reviewer groups with one refutation pass: three HIGH findings, thirteen MEDIUM, twenty LOW, one refuted and one partly refuted. The HIGH findings are restored pin lives judged against a cold arc ledger (R1, confirmed), one-epoch gap evidence that lets a held integer cross an unmeasured break through a cut not of the returning row or a kept re-host (R2, confirmed and narrowed), and a held link without a chord record released only by the monitor (R3, code path confirmed, harm not shown, older than the branch). The saved-state findings (R1, R4, R9) need keys that the adopted configuration does not set.
- **source:** plan package 24 of the post-reset program (2026-11-18), a review of the branch diff from e5a7e44 to 7fb0737; commits read: 7fb0737 (`chain/audit`) for network code, the record and the tests, and main 5ddc422 for user-engine code; the first-stage parts and the refutation pass are under `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/BRV_branch_since_main_review_2026-11-18_parts/` (outside the repository). Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** The recorded gate runs of the chain audit's steps S1 to S9 show no number moved on a configuration claimed identical, except S7 (b), which has no recorded gate; its off identity rests on four gates read in the code. Every new key is identical when off, apart from one declared move (R5) that no checked configuration reaches. Three HIGH findings stand: restored pin lives meet a cold arc ledger (R1); one-epoch gap evidence lets a held integer cross an unmeasured break through a non-returning cut or a kept re-host (R2); a held link without a chord record waits for the monitor (R3, harm not shown). The saved-state keys are not ready to grade.

Read at: network code, the record and the tests at 7fb0737 (frozen checkout D:/GnssProcessorSupport/worktrees/read_7fb0737); user-engine code at main 5ddc422 (C:/GnssProcessor). Every path:line below is tree 7fb0737 unless "main" is written. Nothing was built, run or tested; no repository file was edited.

## 1. Scope

- [corrected 2026-11-19: see Corrections, item 7] Merge base: e5a7e44 (NRF 107 pre-registration). Tip: 7fb0737 (CHA step S7 (b), the chain memory section of the saved state). 47 commits, 85 files, +6833/-649 (`git diff --shortstat e5a7e44..7fb0737`).
- [corrected 2026-11-19: see Corrections, item 7] Reviewed: the 65 files under src/, include/ and tests/, scripts/event_leg.py, and CMakeLists.txt for its source list (it adds src/core/network/state/ChainMemory.cpp and all eight new test files; checked).
- Five reviewer groups, each reading the diff and the changed functions whole:
  1. election: the chain election (ChainDatum.cpp, ChainFrame, ChainVertex, ArcGraph, PhaseAnchorEpoch, Knobs, RunContext);
  2. ring fix: RingFix.cpp/.hpp, RingFixSeams.cpp, EpochObservers, the three ring-fix tests;
  3. loader, types, ledger: GnssConfigLoadNetwork.cpp, GnssConfigNetworkPosture.cpp, GnssConfig.hpp, GnssConfigTypes.hpp, Epoch/RunTypes headers, docs/CONFIG_promotion_ledger.json;
  4. family intake and emit: PhaseFamily.hpp, SignalRoster, ApplicationNetworkSetup, RinexParser, MultiRinexSync, RawObservables, PassASlipScan, EmitStore, SolPrep, NetworkProductEmitter, RxCodeBiasCarry, ParamRegistry;
  5. saved state: ChainMemory, Entities, RunFields, SnapshotFingerprint, StateSnapshotWrite, WarmResume, NetworkStateSnapshot, Run.cpp, scripts/event_leg.py.
- Refutation pass: every HIGH finding, every MEDIUM finding that claims an output change with a key off, and the one finding two groups share. It read the adopted configuration from runs_nom/_rap_ad_n146/arm.json.
- Not reviewed: the 15 documentation files (docs/refound/CHA and NRF parts 13 to 18, CARDS.md, INDEX.md, onboarding pages, the ledger render), scripts/nom_epoch_lint.py and scripts/nom_n4_verify.py. The ledger JSON was read only for the rows of the keys in section 3.

## 2. Findings

Ranked by severity, then by confidence. "Verdict" is the refutation pass: CONFIRMED, PLAUSIBLE (code holds, harm not shown reachable), REFUTED, or "not tested" (outside the pass's scope; the group's reading stands). "Predates" marks a defect present at e5a7e44.

### HIGH

**R1. Restored pin lives are judged against a cold arc ledger.** Group 5 (H1). Verdict: CONFIRMED.
- Where: the kept row tests `arcPinnedLifeContinues(*at_pin, now)` (src/core/network/solve/ChainDatum.cpp:1522-1524), which compares flag_ep, adm_generation and adm_run_start (src/core/network/ar/ArcLife.cpp:115-122). adm_run_start is the ledger's epoch index (ArcLife.cpp:32-38, 64-67), drawn from `rs.epochs++` (src/core/network/assembly/SignalRoster.cpp:1011). The arc ledger and amb_flag_ep are ColdStart (src/core/network/state/Entities.cpp:1237, 881), and no restore writes rs.epochs (grep).
- Scenario: run U pins an edge on an arc admitted at roster epoch 118. The CPL record saves {gen 1, run_start 118, flag -1}. The resumed run admits the same arc at its epoch 0: {1, 0, -1}. The life "ended" (counted only as eq34_kept_not_ended), the edge falls to the as-built rule and may take a different pin target. Arcs admitted at U's epoch 0 pass by coincidence; any arc flagged in U fails on flag_ep.
- [corrected 2026-11-19: see Corrections, item 2] Effect: the restore is wrong by construction for the kept row on its first election, and section 125's bar B2b (tree edges and pin targets at the first resumed epoch) would fail because of it. Proof at no run cost: a unit case restoring that stamp against a ledger admission at epoch 0, expecting the edge held; it fails today (inferred).
- Direction: re-base eq34_pin_life against the resumed ledger at restore, or save the ledger records and amb_flag_ep with the pins.

**R2. The entry check's gap evidence lasts one epoch; a held integer crosses an unmeasured tracking break.** Group 2 (F2). Verdict: CONFIRMED for the row-level unmeasured case; the per-signal part is REFUTED (X2).
- Where: resumptions are cleared every epoch (include/network/slip/RawObservables.hpp:414). The cut and the keep-carry path read only `qc.epoch.resumptions` (src/core/network/ar/RingFixSeams.cpp:725-732, 77-85).
- [corrected 2026-11-19: see Corrections, item 1] Scenario: under the adopted configuration `network.cycle_slip.gap_reentry_relevel_unmeasurable` is unset (loader default false, GnssConfigLoadNetwork.cpp:6354-6356), so an unmeasurable row-level resumption with no loss-of-lock flag is bridged, not latched (src/core/network/slip/SlipQc.cpp:2657-2665). The link is neither released by the Slip cause nor relevelled. If it enters the tree at k+1 or later (a cut or a kept re-host), `resumed` is false and the verdict is Known (src/core/network/ar/RingFix.cpp:719-726); at epoch k it would have been GapUnchecked. The held integer then becomes the step that re-expresses the crossing held links.
- Shared root with R3: the release step has no gap-resumption input (include/network/ar/RingFix.hpp:583-592). This is the confirmed path by which a held integer crosses an unmeasured break in the adopted configuration (CLAUDE.md invariant 1). No reviewer group checked the interaction of that loader default with holds; the refutation pass raised it.
- No unit case covers evidence older than one epoch. Frequency unknown.

**R3. A held link whose arc resumes without a carried level is released at the resume only if the chord record lists it.** Group 2 (F1). Verdict: PLAUSIBLE. Predates.
- Where: the Refloat cause is `o.present && relevelled && !L.rebase_stamps` (RingFix.cpp:604; read at 7fb0737). `present` is a chord record on both bands (RingFixSeams.cpp:895-896), which holds only basis cycles of the band graph built from usable keys in the candidate union (ChainDatum.cpp:484-487, 2271-2303). The decision rows take every non-tree ambiguity column (RingFixSeams.cpp:1123-1124), and the hold table writes a row for every held link (RingFixSeams.cpp:999-1003). The two seams define presence differently.
- Scenario (group 2): the arc is a column but has no chord record; no latch; `present` false, so line 604 is skipped; the absence clock starts and allows 900 s (RingFix.cpp:611-613); PASS B folds the held integer onto the fresh arc until the monitor trips (4 epochs above 0.03 m, RingFix.cpp:472-477).
- Refutation pass: a real route to "column without chord record" exists in the adopted configuration, the anchor hygiene's cold hold-out (AnchorHygiene.cpp:257, 299-321, 364) with phase_anchor_min_age_ep 10, but that is a whole-satellite return. The harmful branch (relevelled, no latch, not present, absent under 900 s) was not shown reachable: a float-vehicle drop comes from the latch, so Slip fires first; the carry's gap cliff and erase lie past 900 s (LevelingCarry.cpp:338), where the Gap cause also fires; a clean resumption bridges the carry, so `relevelled` stays false. Group 2's further point (the drop's stamp seen one epoch late, ApplicationNetworkEpoch.cpp:388 then 392) is moot for latched arcs.
- Disagreement: group 2 holds HIGH with high confidence on the code path; the refutation pass holds the code path but not its reachability. Kept HIGH by consequence, ranked below R1 and R2 by confidence. Settle by reading the events of the released link between the resume and the Monitor release in the run that showed the case (a reattach_blocked_cold or pin_bridge row would show the hold-out), or one simulator event with a satellite-wide gap of about 300 s on a satellite with held links.

### MEDIUM

**R4. The saved-state fingerprint is asymmetric: a mismatched capture/restore pair loses the whole warm resume.** Groups 3 (F1) and 5 (M1). Verdict: CONFIRMED.
- Where: the chainmem tokens are appended only when the LIVE configuration has `network.state_snapshot_chain_capture` (src/core/network/state/SnapshotFingerprint.cpp:104-123); the fingerprint is compared whole (src/core/network/state/WarmResume.cpp:185-200). The loader requires resume_datum_restore with the restore key but not the capture key (GnssConfigLoadNetwork.cpp:5346-5351).
- Scenario: a run with restore on and capture off, resumed from a captured seed, cold-starts entirely (aborts under state_snapshot_require; silent otherwise). A run with capture on, resumed from a seed without the section, also cold-starts, which contradicts "Inert, and says so, on a snapshot without the section" (include/network/state/RunContext.hpp:507-512). The INERT warning (WarmResume.cpp:664-668) is reachable only with the capture off. The tokens are added with the chain and the ring fix off.
- The ledger row says `"fingerprinted": false` for the capture key (checked), so scripts/config_posture_check.py:496 never warns. scripts/event_leg.py is safe because each resumed segment inherits the seed's capture key (event_leg.py:325, 510).
- Direction: require the capture key with the restore key, or move the chainmem tokens out of the whole-resume fingerprint and gate only the CHAIN section on them.

**R5. The chain record is armed on every election; output moves with network.trace_chain_datum off.** Groups 1 (F1) and 3 (F3). Verdict: CONFIRMED; unreachable in every checked configuration.
- Where: `chain_obs.armed = true` unconditionally (ChainDatum.cpp:3042-3044; checked), against e5a7e44 ChainDatum.cpp:2861-2866 (trace only). EmitStore.cpp:472-473 marks released signals whenever the record is armed; PbHold.cpp:515, 524 fill the pre-hold trace's chain columns.
- Scenario: phase_datum_eq34 on, a releasing event row armed, disc_step_class_pairs set, trace off, ring fix off: released satellites are now declared and the step-class counters change; with the pre-hold trace open, its chain columns go from empty to filled.
- [corrected 2026-11-19: see Corrections, item 5] Reach: the adopted configuration has the trace on; the three reference configurations have phase_datum_eq34 false and no disc_step_class_pairs; no file under config/ sets disc_step_class_pairs. It is the declared repair of defect P4 (step S1, commit 0ab87eb), whose identity claim covers only configurations with the trace or without the chain. State it once in the merge note.

**R6. An adopted key was widened in place.** Group 3 (F4). Verdict: not tested.
- `network.ring_fix.entry_revalidate` is RECOMMEND_SCOPED on section 111.1's bars (ledger, checked). Step S8 (b) made it also govern the keep-carry path (ChainDatum.cpp:1660-1665; RingFixSeams.cpp:71, 1826) with no new key. The ledger row still lists section 122's bars as to do; the record shows one observed control, identical in products (CHA part 3 line 20: 172 kept re-hosts checked, 0 refused).
- The adopted configuration therefore runs a behaviour whose own bars are open, against "new behaviour is default off". Confidence high.

**R7. A cascade keep skips the entry check.** Group 2 (F4). Verdict: not tested.
- [corrected 2026-11-19: see Corrections, item 8] ChainDatum.cpp:1756-1764: with `network.ring_fix.cascade_rehost_keep`, a re-host of a third band onto a held cascade link is kept by `chainCascadeHeldInteger` / `ringFixCascadeKeepInteger`, which read the latch only. No gap or signature evidence is asked, so defect P1 stays open on cascade links. Confidence high on the code.

**R8. Copied code-bias records are not declared on the correction stream.** Group 4 (F1). Verdict: not tested.
- With `network.phase_family_codes`, `networkPhaseFamilyMemberRecords` copies C1C into C1X and C5Q into C5X (EmitStore.cpp:60-80) and the emitter copies their counters (NetworkProductEmitter.cpp:844-854). The legend note names only the P records (SolPrep.cpp:1201-1210); the run log is the only declaration (ApplicationNetworkSetup.cpp:272-280).
- [corrected 2026-11-19: see Corrections, item 6] Scenario: a user tracking C1X receives the C1C satellite code bias with no statement that it is a copy; the satellite part of C1C-C1X is ignored in the network and at the user. The record measures 1 to 3 mm at the users (NRF part 15 line 134). Confidence high.

**R9. Two ring-fix window inputs stay cold after a resume.** Group 5 (H2). Verdict: CONFIRMED mechanism; severity lowered from HIGH to MEDIUM.
- (a) SatelliteState::code_ref_event_s is ColdStart (Entities.cpp:565), read for the code-event window (RingFixSeams.cpp:1200-1201). (b) Restored carries have relevel_s 0 ("unstamped"), because the L record has no stamp (src/core/products/NetworkStateSnapshot.cpp:497-508; include/network/driver/RunTypes.hpp:692, 830-838), and ringFixWindowOk skips a zero stamp (RingFix.cpp:280-281).
- Scenario: a code-reference move or a re-level within 600 s before the snapshot; U refuses the link for the window, the resumed run decides it at its first eligible epoch.
- Why MEDIUM: both saved-state keys are needed (default off, UNVALIDATED); the other window inputs (chord_since_s, released_s, last_tree_s) are restored with the links, and the gate and persistence still apply (inferred). The gap re-entry evidence is cold by design (NRF 125.1, rule change 12).

**R10. A phase-family row joining two carriers of one band digit is accepted.** Group 4 (F2). Verdict: not tested.
- The loader (GnssConfigLoadNetwork.cpp:1488-1489) and signalRosterPhaseFamilies (SignalRoster.cpp:877) compare only the band digit. BeiDou digit 1 names B1I 1561.098 MHz and B1C 1575.42 MHz (include/common/SignalBand.hpp:64-65). "C:L1I>L1P" passes both checks, and B1I cycles would be read under a B1C name with the identity's wavelength. Comparing SignalId::band() closes it. Confidence high on acceptance; the damage is inferred. GPS "G:L1X>L1C" is accepted and would share one code bias across two ranging codes under the codes-shared form (inferred).

**R11. The group-delay latch is replaced wholesale at restore.** Group 5 (M2). Verdict: not tested.
- [corrected 2026-11-19: see Corrections, item 11] `setup.sat_gf_target_m = latch` (src/core/network/state/ChainMemory.cpp:381): a satellite in the resumed run's latch but absent from the saved one becomes SatGfTargetKind::Missing (src/core/network/solve/Sbasis.cpp:107-110). The log counts differences only over saved satellites (ChainMemory.cpp:372-379), so losses go unreported. window_t0/t1 are restored into the log only. A same-window identity test is unaffected; a restart over a later window is. Confidence high.

**R12. The restore is not an event with counters and identities (invariant 7).** Group 5 (M3). Verdict: not tested.
- [corrected 2026-11-19: see Corrections, item 12] One info line and one warning (ChainMemory.cpp:386-398); no run-end counters, no identities of the restored held links or their integers, no stale or mismatch class. absent_since_s is not seeded from the snapshot epoch, so the time between the snapshot and the first live epoch does not count toward max_gap_s (RingFix.cpp:611-613); the carry's gap drop is the only bound (inferred). Confidence high.

**R13. The saved-state unit cases fall short of the pre-registration.** Group 5 (M4). Verdict: not tested.
- [corrected 2026-11-19: see Corrections, item 9] NRF 125 names "the fingerprint's tokens"; no test touches SnapshotFingerprint, networkChainMemoryWrite / networkChainMemoryRestore (latch replacement, tag dispatch, staged consumption) or the loader interlocks. The round trips (tests/network/test_chain_memory.cpp:62-105) cannot see a member missing from both writer and reader, nor two swapped members left at equal defaults (RL: absent_since_s and released_s, rebase_stamps, oracle and log_decided; RK: streak, streak_n3, monitor_bad, decided_s, absent_since_s). No static guard (a member count on RingFixLink). Confidence high.

**R14. The root and merge_split event rows are kept on the write flag, not the read flag.** Group 3 (F2). Verdict: not tested.
- GnssConfigLoadNetwork.cpp:8651-8675 keeps 'root' (and so 'merge_split') whenever a snapshot path is set with the capture on; the root row's pinned targets come back only through `network.resume_chain_restore` (CRP lines, Entities.cpp:1312, 1354). The documented matched null (event_leg.py --no-chain-restore, docs/onboarding/event_legs.md:8) has the capture on and the restore off, so it keeps the root row on an empty target memory, the hazard the loader's own warning names (8659-8661). The new warnings for 'returning', 'slip' and 'kept' (8677-8697) test the same flag.
- [corrected 2026-11-19: see Corrections, item 10] Disagreement: group 5 (L5) calls that null fair because it keeps the same root row and fingerprint as the restored run. Both readings hold: the null matches the fingerprint and the row set, but carries a root-row behaviour on an empty memory that a lineage without the capture would not run. The root row's action on an empty memory was inferred from the loader comment, not traced in ChainDatum.cpp. Confidence medium.

**R15. The cut's usability test reads the latch on the cut's band only.** Group 2 (F5). Verdict: not tested.
- RingFixSeams.cpp:715-716 against keep-carry's two bands (ChainDatum.cpp:377-380). Defect P10's open item ("a rule question for S8", CHA part 2) remains: S8 (a) and (b) gave the verdict one owner but left the usability rules apart. A link whose other band slipped is a known entry on the cut and unusable on keep. The deferral is not triaged blocking or inert with data (invariant 3).

**R16. A tripped monitor is dropped when the link becomes a tree edge.** Group 2 (F3). Verdict: not tested.
- Step 3 (RingFixSeams.cpp:857-866) runs before the release step; `ringFixUnholdLink(ToTree)` clears monitor_trip (RingFix.cpp:385-389, 401-403). No Monitor release is counted, no carry re-opened, and the to_tree event does not say the link was tripped. The chain excludes tripped links only from held-first and keep (ChainDatum.cpp:377-380, 967). Confidence medium-low on reach.

### LOW

**R17. Phase-family members accepted in pair lists; the adopted configuration carries one.** Groups 1 (F3), 3 (F7b), refutation pass (item 8). The adopted configuration lists E:L1X/L5X in disc_step_class_pairs under a table that renames L1X and L5X; the entry has no own-signal source and its counters are overwritten by the member rule (NetworkProductEmitter.cpp:976, as reported). `phase_datum_eq34_family_pairs` (GnssConfigLoadNetwork.cpp:8577-8608) and disc_step_class_pairs (2900-2914) accept member entries while ring_fix.pairs refuses them (1549-1556). The ledger's armed_when for disc_step_class_pairs is `v != []` (checked), while ring_fix.pairs drops E:L1X/L5X when the table is set, so the posture check cannot see it. The run-end warning (ChainDatum.cpp:3138) fires only when no configured pair formed.

**R18. With family_pairs set, a constellation tracking exactly two signals ignores the list.** Group 1 (F2). ChainDatum.cpp:553-557 runs the two-signal rule before the configured branch (:558). On an epoch without L5Q, Galileo's L1C pairs with L7Q, and returns to the configured pair when L5Q returns. Intended (pinned by a unit case), but the switch is named only while eq34_family_lines < 40 (:590), has no switch counter and no identity.

**R19. held_first_by_life: election wiring untested; no counter separates a by-life choice.** Group 1 (F4). Dropping `|| el.held_by_life` at ChainDatum.cpp:687 zeroes every durability and passes every unit case (tests/network/test_held_first_by_life.cpp sets ArcRow::durability by hand; inferred). held_first_kept counts the outcome only (invariant 4).

**R20. S9's previous-target hand-off has no unit case.** Group 1 (F5). ChainDatum.cpp:1220-1225, 2909-2911, 2960, 3489; Entities.cpp:1200-1203. Traced as equivalent to e5a7e44; no test sets eq34_seed_realized, so reverting chainPrevTarget passes the suite.

**R21. Ledger "fingerprinted" disagrees with the code; two meaning-setting keys are not fingerprinted.** Groups 3 (F5), 5 (L1). phase_families and phase_family_codes are fingerprinted (SnapshotFingerprint.cpp:99-100) with no ledger member; rx_extra_band_delay_q likewise (:365); the capture key is marked false (R4). disc_step_class_pairs is absent from the fingerprint although the emitter's step-class baselines are keyed by family index (include/products/NetworkProductEmitter.hpp:924), and phase_datum_eq34_seed_round shapes EQT (Entities.cpp:429-430) without a token. Whether the emitter's restoreState rejects a reordered list was not settled.

**R22. Wrong ledger parent for two parameters.** Group 3 (F6). nn_guard_min_epochs and nn_guard_max_if_m have parent nn_guard (KEEP_OFF; checked) but are read by the adopted entry_revalidate and by nn_monitor (RingFix.cpp:480-486; RingFixSeams.cpp:495-497, 515-520, 1216).

**R23. Chained, cyclic and many-to-one family tables are accepted.** Groups 3 (F7a), 4 (F3). Only a duplicate member is refused (GnssConfigLoadNetwork.cpp:1492). "E:L1X>L1C","E:L1C>L1B" splits the network again silently (RinexParser.cpp:381-396); "E:L1X>L1C","E:L1Z>L1C" on a file declaring both renames both to L1C, the later column overwrites the earlier, and the setup refusal does not fire (ApplicationNetworkSetup.cpp:1411-1434). The adopted table hits neither.

**R24. Under phase_family_codes the copy can overwrite an estimated value.** Group 4 (F7). A file declaring C1C, C1X and L1X is not refused (the refusal tests phase codes only, ApplicationNetworkSetup.cpp:1418-1436); a C1X bias estimated from it is overwritten by the C1C copy (EmitStore.cpp:71-74). The code form of defect P6. Confidence medium.

**R25. A member's SYS / PHASE SHIFT is not applied.** Group 4 (F4). Stored under the declared code (RinexParser.cpp:632, 642), looked up under the renamed one (798-818). Only with model_corrections.apply_phase_shifts on, which no reference configuration sets.

**R26. The signal declaration carries no statement about members.** Group 4 (F5). networkLedgerEmitBuildDeclaration sees identity names only. In main the user engine's band latch follows the declaration under network.closed_loop.roster_latch (main src/core/realtime/RealTimeEngineSsrBreaks.cpp:51-66), so an L1X-only user could be steered to L1C (inferred; latch rule not read). Latent: network.ledger_emit defaults off (GnssConfigLoadNetwork.cpp:6941).

**R27. Monitor gaps.** Group 2 (F6). Predates. An epoch without a formed residual resets monitor_bad (RingFix.cpp:472-477); a held link outside the decision's rows gets no monitor update while its hold row is folded (RingFixSeams.cpp:999-1003).

**R28. The code residual is keyed by (satellite, band digit), first row wins; raw string parsing.** Group 2 (F7). RingFixSeams.cpp:1216-1236 (checked: `emplace` on `tag.sat.str()` and `code[1]`); under channel_mode all another channel's code can feed the signature. `sat[0]` / `sig[1]` parsing at RingFixSeams.cpp:54-63, 1224, 1228 is against the typed-identity rule (defect P14).

**R29. rx_extra_band_delay_q validated only inside the carry block.** Groups 3 (F7c), 4. GnssConfigLoadNetwork.cpp:12391-12403: with the carry off any value passes with no INERT line; `>= 0` admits +inf. The run-start parameter table keeps q = rx_cb_q for the family and names the IFB key only in its source text (ParamRegistry.cpp:251, 260).

**R30. Loader coverage.** Group 3 (F7d, F8). nn_guard parameters set with the ring fix off are missing from the inert list (11255-11292). No loader case for the phase-family table forms, the ring_fix.pairs member refusal, the nn_guard ranges, the release and by-life interlocks, the capture and restore interlocks, or a negative extra-band q.

**R31. The arc/band key split (S6) is unreachable in this build.** Group 1 (F6). ChainDatum.cpp:385-405, 417, 498: usable_keys builds both keys from one SatSignalId (PhaseDatum.cpp:157) and the intake renames members, so the two keys are always equal and the member loop never runs. Untested code a newcomer must still follow; it prepares the native phase-family build.

**R32. Text that disagrees with the code.** Groups 2 (F8), 3 (F3), 4 (F6), 5 (L2, L3).
- "Averaged over its last N epochs" (RingFixSeams.cpp near 1127, 1137): exponential with weight 1/N after N epochs (RingFix.cpp:482-485).
- keep_evidence comment and CHA part 3 line 26 say the cut reads this epoch's signature after the solve; the cut runs before the solve (ApplicationNetworkEpoch.cpp:388 against 473, 509).
- Roster arm line "its codes and code biases stay its own" under CODES SHARED (SignalRoster.cpp:897-903); run-end "member phase-bias record(s)" counts code records too (SignalRoster.cpp:2463; EmitStore.cpp:476-478); the intake's station count counts calls (ApplicationNetworkSetup.cpp:265-267).
- The disposition tables still declare the ring-fix memory and the chain members ColdStart, "not in the snapshot" (RunFields.cpp:161, 170, 603-618; Entities.cpp:762-770, 897, 919-923, 1042, 1231, 1235), and the fingerprint exemption says they are "never serialized" (SnapshotFingerprint.cpp:616-630).
- The documented section order puts CHAIN after EEND (include/products/NetworkStateSnapshot.hpp:112-114); the serializer writes it before EMITTER (NetworkStateSnapshot.cpp:316-329). Harmless.
- The ledger text of network.ring_fix.enable still says it needs trace_chain_datum.

**R33. The guard's release form is counted as a Monitor release.** Group 2 (F9). RingFixSeams.cpp:1657 sets monitor_trip; its nn_reopen sets rebase_stamps (798-806), blocking the Refloat cause until a carry exists. The guard failed and stays off (KEEP_OFF).

**R34. The run-end hold-table line changed format in every ring-fix run.** Group 2 (F10). RingFixSeams.cpp:1742-1749; announced in the record, products and traces identical (CHA part 3).

**R35. The chain grade of scripts/event_leg.py does not always compare like with like.** Group 5 (L5). The edge key (system, band, edge_sta, edge_sat) omits the family column that exists (RunTraces.cpp:1060), so pair families sharing a band collide; `_transitions` counts the seed's transitions at the first resumed epoch but never the resumed run's own (prev=None), biasing holds_releases_not_matched. The product columns and the ring-fix, edge and wet-delay headers check out (NetworkProductIO.cpp:808-811; RunTraces.cpp:1011, 1060, 1076).

**R36. One receiver declaring both a member and its identity aborts the run.** Group 4 (F8). ApplicationNetworkSetup.cpp:1452-1460 throws rather than excluding the station. A design choice; the message names the station.

### REFUTED (struck from the ranking, kept)

**X1. Snapshot bytes change with both chain keys off under seed round.** Group 5 (L4). REFUTED: the e5a7e44 pin audit wrote realized targets straight into eq34_prev_target (e5a7e44 ChainDatum.cpp:3283-3285; checked), so the old EQT already held them; the new merge (Entities.cpp:1200-1203) restores those bytes. The latch is dropped only at the next election (ChainDatum.cpp:2960), after the end-of-epoch snapshot.

**X2. Per-signal re-entries escape the entry check (part of R2).** Group 2 (F2). REFUTED for shifted or loss-of-lock per-signal re-entries: the per-signal tier (SlipQc.cpp:3001 onward; gap_reentry_per_signal armed) latches the arc and releases the link at k by the Slip cause. Only unmeasured per-signal re-entries remain, inside R2.

## 3. Configuration keys added or changed since the merge base

Default and verdict from the loader and docs/CONFIG_promotion_ledger.json (checked). "Posture row" = a row in src/core/config/GnssConfigNetworkPosture.cpp (grep). "Off-identical" = no output change at the default.

| key | default | off-identical | reaches code | ledger verdict | posture row | unit case |
|---|---|---|---|---|---|---|
| network.phase_families | [] | yes | yes (intake, parser, slip pairing, emit, legend, fingerprint) | RECOMMEND_SCOPED; no fingerprinted member (R21) | yes | test_phase_family.cpp; no loader or refusal case |
| network.phase_family_codes | false; refused without the table | yes | yes | RECOMMEND_SCOPED, parent phase_families | yes | test_phase_family_codes.cpp |
| network.ring_fix.nn_guard | false | yes | yes | KEEP_OFF | no | test_ring_fix_nn_guard.cpp (verdicts; move and release: none) |
| network.ring_fix.nn_guard_min_epochs | 60, range [10, 2880] | yes | yes, also via entry_revalidate and nn_monitor | PARAM, parent nn_guard (R22) | no | none |
| network.ring_fix.nn_guard_tol_gf_m | 0.12, range (0, 0.22) | yes | yes | PARAM | no | none |
| network.ring_fix.nn_guard_max_if_m | 0.30, > 0 | yes | yes, also via entry_revalidate | PARAM, parent nn_guard (R22) | no | none |
| network.ring_fix.nn_guard_release | false; refused without nn_guard | yes | yes | KEEP_OFF | no | none |
| network.ring_fix.entry_revalidate | false | yes | yes (cut; keep-carry since S8 (b)) | RECOMMEND_SCOPED, widened (R6) | yes | test_ring_fix_entry_revalidate.cpp (verdicts, gathering; seam and keep counting: none) |
| network.ring_fix.nn_monitor | false | yes | yes (observer row) | TRACE | no | test_ring_fix_nn_monitor.cpp (tally; seam: none) |
| network.ring_fix.held_first_by_life | false; refused without held_first | yes | yes | UNVALIDATED | no | test_held_first_by_life.cpp (ArcGraph only; R19) |
| network.state_snapshot_chain_capture | false; refused without the datum capture | yes | yes | UNVALIDATED; fingerprinted false, wrong (R4) | no | test_chain_memory.cpp (round trip only; R13) |
| network.resume_chain_restore | false; refused without resume_datum_restore | yes | yes | UNVALIDATED | no | test_chain_memory.cpp (partial; R13) |
| network.rx_extra_band_delay_q | 0 | yes | yes (5 sites, RxCodeBiasCarry.cpp:209, 351, 463, 635, 650) | UNVALIDATED, parent carry_rx_code_bias; no fingerprinted member | no | test_rx_extra_band_delay_q.cpp (selection rule only) |
| network.phase_datum_eq34_family_pairs | [] | yes | yes | UNVALIDATED, parent phase_datum_eq34_family | no | three oracle cases; four loader refusals |
| network.ring_fix.pairs (changed: refuses a table member) | [] | yes | yes | RECOMMEND_SCOPED | yes | none for the refusal |
| network.ring_fix.enable (changed: trace no longer required) | false | yes for this key | yes | UNVALIDATED; stale text (R32) | used as regime | none |
| network.trace_chain_datum (changed: arms the observatory only) | false | no, on R5's combination | yes | TRACE | no | none with the trace off |
| network.phase_datum_eq34_event_rows (changed: root kept with the capture) | [] | yes | yes | unchanged | no | none (R14) |
| network.ring_fix.reattach, cascade_tree (counters moved) | false | yes; cascade pair count moves only with both on (announced) | yes | unchanged | no | none new |

## 4. New and changed test files

All eight new files are in CMakeLists.txt (checked). None was compiled or run.

| file | status | what it asserts | can it fail |
|---|---|---|---|
| tests/network/test_chain_memory.cpp | new, 248 lines | write, read, write and compare text for RL, RK, the tree, root, pin-life, anchor and topology records; non-shortest number text, malformed lines and a cut section refused | yes, for a member dropped on one side or a malformed line; not for a member missing on both sides, swapped members at equal defaults, the fingerprint, the restore functions or the loader interlocks (R13) |
| tests/network/test_held_first_by_life.cpp | new | first held arc without the rule; most durable with it; a tie keeps walk order; the first kept when most durable | yes (ignoring held_by_life fails case 2; ">=" for ">" fails case 3); election wiring untested (R19) |
| tests/network/test_ring_fix_entry_revalidate.cpp | new | NotHeld when unusable; Known when off; GapUnchecked, GapShifted, clean gap Known; Signature refused unless large IF deviation or k = 0; gap verdict first; six names; chainEntryStep keeps each mechanism's rule; gathering takes held links and the link's own pair | yes, each case pins a distinct verdict; not one-epoch evidence (R2), the returning-row string, or the counters |
| tests/network/test_ring_fix_nn_guard.cpp | new | k = +1 and -2 flagged; 0.6 m IF deviation refused; under half a step not flagged; satellite-wide shift not flagged; small groups give no verdict | yes (`acted == 1` catches false flags) |
| tests/network/test_ring_fix_nn_monitor.cpp | new | step epochs, episodes, edges, sums and times; large IF deviation not a step; missing verdict keeps state | yes (exact counts and edges); the seam is untested |
| tests/network/test_phase_family.cpp | new | code mapping; intake renames L, D, S and keeps C1X; a file with both keeps both; slip pairing with the receiver's own code (and no row without the table); member records follow the identity's value, flag and counter | yes; the L5X release assertion fails without the override; the setup refusal, appliedObsCodeRenames and the continuity-guard part are untested |
| tests/network/test_phase_family_codes.cpp | new | C1X and C5X read as C1C and C5Q; member code value and counter follow the identity, first sighting takes counter 1 | yes |
| tests/network/test_rx_extra_band_delay_q.cpp | new | IFB key takes the extra-band q when set; DCB key, q = 0 and an unparsable key keep rx_cb_q | yes, if RxIfbRule::governs or rxCbCarryQ change; the five carry sites and the loader check are not exercised |
| tests/network/test_network_arc_graph_oracles.cpp | changed, +459 | three family-pair cases (hand cases; 16 lineages x 40 epochs on mirrored two-mode Galileo; band-only arcs); helpers perturbChainCarry, handChainEpoch keep the old draws' order | yes through family and fallback counts and n_pair_p; the shared-edge and equality checks alone cannot fail on full or mirrored graphs, and n_pair_epochs > 1500 is the test's own count |
| tests/network/test_loader_message_conditions.cpp | changed, +60 | four family_pairs refusals (malformed, unknown constellation, one band, shared signal); armed, signal-family and pin-bridge messages | yes; the unknown-constellation case passes on either branch of the same message (systemFromLetter('X') not checked) |
| tests/network/test_network_epoch_contract.cpp | changed, +2 | static_assert that networkRingFixNearNullMonitor has the observer signature | yes, at compile time |
| tests/network/test_network_epoch_observers.cpp | changed, +2 | pins the observer row (RosterTail slot) | yes, if the row moves or is removed |

## 5. State members of the saved state

From group 5; "tested" means a unit round trip.

| member | owner | captured | restored | tested |
|---|---|---|---|---|
| RingFixLink (30 members) | NetworkRingFix | yes, RL | yes | round trip, partial value checks |
| cascade_links (12 members) | NetworkRingFix | yes, RK | yes | round trip |
| t0_s, last_tree_s, certified_edges, prev_targets | NetworkRingFix | yes | yes | yes |
| settled_incumbent, settled_run | NetworkRingFix | yes | yes | yes |
| untied_since_s, moves, moved_s, last_move_s | NetworkRingFix | yes | yes | yes |
| nn_reopen | NetworkRingFix | yes | yes | count only |
| counters, cascade_counters, nn_mon | NetworkRingFix | no (by design) | no | n/a |
| eq34_tree_seen (satellite and receiver bands) | entities | yes, CTS/CTR | yes | yes |
| eq34_tree_root | entities | yes, CRS/CRR | yes | yes |
| eq34_root_rpb_target | entities | yes, CRP | yes | yes |
| eq34_pin_life | entities (arc) | yes, CPL | yes, judged against a cold ledger (R1) | yes |
| anchor_arc_age | entities (arc) | yes, CAA | yes (overridden by the cold ledger under ledger_anchor) | yes |
| emitted_anchor, emitted_anchor_s | entities | yes, CEA/CES | yes | yes |
| topo_prev_tie, topo_prev_kind | entities | yes, CTT/CTK | yes | yes |
| topo_seeded, topo_prev_cols | carry | yes, CTZ/CTC | yes | no |
| eq34_sat_root, rx_parent, prev_el, prev_target, retained_edge | entities | datum capture (EQS/EQP/EQE/EQT/EQR) | datum restore (WarmResume.cpp:447-450) | outside this diff |
| eq34_seed_realized | entities | folded into EQT | as prev_target | no |
| sat_gf_target_m and window | setup | yes, GW/GD | latch wholesale (R11); window into the log only | parse only |
| emitter held-value maps | emitter | yes, E* | yes; cg_lowered_last_ rebuilt each emission | yes |
| arc ledger records | entities (arc) | no | no (R1) | n/a |
| amb_flag_ep | entities (station) | no | no (R1) | n/a |
| code_ref_event_s | entities (satellite) | no | no (R9) | n/a |
| LocalCarry relevel_s, birth_ep, dropped_ep | entities (arc) | no (L record lacks them) | 0 or sentinel (R9) | n/a |

[corrected 2026-11-19: see Corrections, item 13] Order is sound: the ring fix is armed (ApplicationNetwork.cpp:279) before networkRunArm (:315); the restore runs after the warm resume (Run.cpp:334); the snapshot is written after the decision and the emit (ApplicationNetworkEpoch.cpp:509, 523, 627). Both keys off: write gated (StateSnapshotWrite.cpp:391), serialize gated on non-empty (NetworkStateSnapshot.cpp:318), tokens gated (SnapshotFingerprint.cpp:104), restore a no-op on empty staging (ChainMemory.cpp:332).

## 6. What a merge into main would still need (listed, not requested)

1. A three-way merge of the 17 files changed on both sides since e5a7e44 (main added the network view and the live record): CMakeLists.txt, docs/CONFIG_promotion_ledger.json and .md, docs/onboarding/README.md, docs/refound/CARDS.md, include/config/GnssConfig.hpp, include/config/GnssConfigTypes.hpp, include/network/driver/EpochObservers.hpp, include/rinex/RinexParser.hpp, src/core/config/GnssConfigLoadNetwork.cpp, src/core/config/GnssConfigNetworkPosture.cpp, src/core/network/diag/RunTraces.cpp, src/core/network/driver/EpochObservers.cpp, src/core/network/emit/EmitStore.cpp, src/core/rinex/RinexParser.cpp, tests/network/test_network_epoch_contract.cpp, tests/network/test_network_epoch_observers.cpp. Both sides add observer rows, so the pinned table needs both.
2. Gates on the merged tree: Debug build and the full gnss_tests suite; gnss_simulator_selftest; scripts/nom_epoch_lint.py and scripts/nom_n4_verify.py (the stale dispositions of R32 may need declaration edits); python scripts/docs_record.py verify (it runs make_golden_config.py --verify and ledger_render.py --check); scripts/config_posture_check.py on the adopted configuration.
3. Same-build identity runs (Release, one exe) of the three reference configurations against main 5ddc422 built the same way: products and traces identical, 2 h first, then 6 h per the adoption rule.
4. The adopted chain configuration (the runs_nom/_rap_ad_n146 arm) on the merged exe, identical in products to the 7fb0737 control.
5. For R5, optionally one arm with phase_datum_eq34 on, a releasing row, disc_step_class_pairs set and the trace off, to show the declared move and nothing else.
6. [corrected 2026-11-19: see Corrections, item 2] Before the saved-state keys are graded: the R1 and R9 repairs, then NRF 125's runs (B0 to B4); grading B2b before R1 is fixed fails by construction.
7. For R2 and R3: a simulator event of an unmeasured row-level gap on a held link, and one of a satellite-wide gap of about 300 s, graded on release causes against the hold rows; observed data after.
8. Before arming network.ledger_emit with phase families: a reading of the user engine's roster latch in main (R26).

## Decisions for the author

1. Whether the R1 and R9 repairs (re-base the pin life at restore or save the ledger; carry the relevel stamp in the L record) come before any NRF 125 run, as the refutation pass recommends.
2. The fingerprint rule for the capture and restore pair (R4): require the capture key with the restore key, or keep the chain-memory tokens out of the whole-resume fingerprint.
3. Whether a held link releases or re-validates on an unmeasured or unclean resumption of its own (station, satellite) (the shared root of R2 and R3), and whether gap_reentry_relevel_unmeasurable should stay off under holds.
4. Whether the keep-carry reach of entry_revalidate (R6) gets its own key or stays under the adopted key until section 122's bars are met; whether cascade keeps (R7) and the cut's one-band latch (R15) join the entry check.
5. Whether copied code-bias records are declared on the correction stream (R8), and whether phase_family_codes stays in the adopted configuration until then.
6. Whether E:L1X/L5X leaves disc_step_class_pairs under the table, and whether the loader refuses member entries in family_pairs and disc_step_class_pairs as it does in ring_fix.pairs (R17).
7. Whether the phase-family check compares the signal band rather than the band digit (R10) and refuses chains and many-to-one rows (R23).
8. Whether to merge the network code now with the saved-state keys marked UNVALIDATED and off, or hold the merge until R1, R4 and R9 are repaired.

## Not checked

- No build, test or engine run; the eight new test files were not compiled. The S1 to S9 identity claims rest on the recorded gate runs, which were not reopened.
- The 15 documentation files, scripts/nom_epoch_lint.py and scripts/nom_n4_verify.py.
- [corrected 2026-11-19: see Corrections, item 3] That rs.epochs and the arc ledger restart at 0 in a resumed run (R1): shown only by the absence of a restore.
- Which mechanism removed the resumed arc from the chord record in the observed case (R3); no run folder was opened by the reviewer groups.
- The root row's action on an empty target memory after a resume (R14).
- Whether the emitter's restoreState rejects a reordered disc_step_class_pairs (R21); whether sat_gf_datum is fingerprinted under another token.
- Whether applyRehostRow can run twice per pin in one epoch (duplicate keep_checked counts).
- Whether a re-host kept on a held link carries a satellite and receiver identity outside the chain trace.
- The user engine's roster latch in main (R26); whether n_mark_pb counts member keys twice.
- systemFromLetter('X'); the CTC label format (a comma in a label would be refused); the ledger render.
- Whether any run configuration outside git combined the chain, a releasing row and disc_step_class_pairs without the chain trace (R5).
- How often R16's path is reached on the adopted configuration; the size of the Galileo C1C-C1X and C5Q-C5X satellite differences beyond the record's 1 to 3 mm (R8).

## 2026-11-19: corrections from the check against the code

The check read tree 7fb0737 (the frozen checkout `D:/GnssProcessorSupport/worktrees/read_7fb0737`), tree e5a7e44 through `git show` in that checkout, and the adopted configuration `D:/GnssProcessorSupport/runs_nom/_rap_ad_n146/arm.json`. Its verdict: usable with corrections; the three HIGH findings and the MEDIUM findings checked hold on the code. Paths below are tree 7fb0737 unless e5a7e44 is named.

1. R2, Scenario. The document says that an entry into the tree at k+1 or later (a cut or a kept re-host) has `resumed` false and is judged Known. The code shows that a cut of the returning row sets `resumed` itself (`src/core/network/ar/RingFixSeams.cpp:724`) and is refused as GapUnchecked (`src/core/network/ar/RingFix.cpp:719-722`). The finding holds for a cut not of the returning row and for a kept re-host (`RingFixSeams.cpp:77-85`). Ruled WRONG in part; R2 stands, narrowed.
2. R1, Effect, and section 6 item 6. The document says section 125's bar B2b "would fail" because of R1, and that grading B2b before the repair "fails by construction". The code shows only that each such kept edge falls to the as-built rule (`src/core/network/solve/ChainDatum.cpp:1524`), which may pick another target; a different target is possible, not shown. Ruled overstated: B2b is at risk, and grading it before R1 is repaired tests a known defect.
3. R1, Not checked. The bullet that rs.epochs and the arc ledger restart at 0 in a resumed run is settled: the arc ledger and amb_flag_ep are ColdStart with no reader (`src/core/network/state/Entities.cpp:1237`, `:881`), `epochs{0}` (`include/network/assembly/SignalRoster.hpp:453`; `src/core/network/assembly/SignalRoster.cpp:1011-1014`), no write of the roster, the arc ledger or the flags in `WarmResume.cpp`, `ChainMemory.cpp` or `NetworkStateSnapshot.cpp` (grep), and the CPL reader writes eq34_pin_life only (`Entities.cpp:1358-1367`). R1's open part is confirmed.
4. Outcome (the draft's, replaced in the header above). The draft said no refactor step S1 to S9 was found to move a number on a configuration it claims identical. The document has no evidence of its own: the rows are the gate runs recorded in CHA part 2 (lines 140-142, 181-183: S1 to S6) and part 3 (lines 19-20, 56, 65: S8 (a), S8 (b), S9, S7 (a)). S7 (b), the tip 7fb0737, has no recorded gate: its bar B0 (NRF part 18 line 107) is pre-registered and not run (commit message of 7fb0737); its off identity is a reading of four gates in the code (section 5).
5. R5, Reach. Addition: R17 does not make R5 reachable in the adopted configuration, whose trace is on. With the trace off, its ring fix forms a combination that e5a7e44 refused (e5a7e44 `src/core/config/GnssConfigLoadNetwork.cpp:11051-11056`), so no identity claim covers it.
6. R8. The document says the record measures 1 to 3 mm at the users (NRF part 15 line 134). That line attributes user moves of 1 to 3 mm in some hours to the relabel's renamed codes; the share of the copied code-bias records is not measured. Ruled overstated; R8 stands with its size unmeasured.
7. Counts (the draft's STATUS summary and section 1). The document says 47 commits, "the 65 files under src/, include/ and tests/" and "67 code, test and build files". `git rev-list --count e5a7e44..7fb0737` gives 50 (no merges); 66 files lie under src/, include/ and tests/, and with CMakeLists.txt and scripts/event_leg.py 68. The 85 files and +6833/-649 are correct. Ruled WRONG.
8. R7, wording. The document says the cascade keep functions read the latch only. They read the hold, its monitor trip and the latch (`RingFix.cpp:120-122`; `ChainDatum.cpp:1760-1763`, `1675-1692`). The finding (no gap or signature evidence asked) stands.
9. R13, wording. The document says no test touches SnapshotFingerprint. `tests/network/test_network_layout_flags.cpp:373` calls networkSnapshotFingerprint without the capture key; no test reaches the chain-memory tokens, and `tests/network/test_chain_memory.cpp` never calls networkChainMemoryWrite or networkChainMemoryRestore. The finding stands as reworded.
10. R14, Disagreement. Decided: the matched null is fair for its stated purpose (the same run segment without the memory, `scripts/event_leg.py:507-510`). Without the restore, a root that stays a root finds no saved target and gets none (`ChainDatum.cpp:1925-1931`); a handed-over root still pins from its carry (`ChainDatum.cpp:1962-1971`). The null therefore includes one root-row step on an empty memory that a lineage without the capture never runs; this is to be stated when the null is graded. Severity low.
11. R11, line numbers. The document cites `ChainMemory.cpp:381` and `:372-379`; the code is at `src/core/network/state/ChainMemory.cpp:127` and `:116-125`. Content confirmed (`src/core/network/solve/Sbasis.cpp:107-110`).
12. R12, line numbers. The document cites `ChainMemory.cpp:386-398`; the lines are `:132-145`.
13. Section 5, line number. The restore no-op on empty staging is `ChainMemory.cpp:78`, not `:332`. The other three both-keys-off gates are confirmed (`src/core/network/state/StateSnapshotWrite.cpp:391`, `src/core/products/NetworkStateSnapshot.cpp:318`, `src/core/network/state/SnapshotFingerprint.cpp:104`; `src/core/network/driver/Run.cpp:334`).

### Line numbers that moved

All in `src/core/network/state/ChainMemory.cpp`, tree 7fb0737: `:381` is `:127` and `:372-379` is `:116-125` (R11); `:386-398` is `:132-145` (R12); `:332` is `:78` (section 5).

### What stands

- R1, R4, R9 stand, with R1's open part now settled. All three touch only the saved-state keys; the adopted configuration sets no state_snapshot_path, so none reaches it.
- R2 stands, narrowed to cuts not of the returning row and to kept re-hosts. R3 stands as a code path with harm not shown.
- R5 stands and does not reach the adopted configuration, with or without R17. R6, R7, R8, R10 stand; R8's size is not measured.
- The draft's sentence on S1 to S9 stands only as corrected: S7 (b) is not covered by a recorded gate.
- Decisions put to the author, after correction (all eight stand):
  1. Whether the R1 and R9 repairs (re-base the pin life at restore or save the ledger; carry the relevel stamp in the L record) come before any NRF 125 run.
  2. The fingerprint rule for the capture and restore pair (R4): require the capture key with the restore key, or keep the chain-memory tokens out of the whole-resume fingerprint.
  3. Whether a held link releases or re-validates on an unmeasured or unclean resumption of its own (station, satellite) (R2, narrowed, and R3), and whether gap_reentry_relevel_unmeasurable stays off under holds.
  4. Whether the keep-carry reach of entry_revalidate (R6) gets its own key; whether cascade keeps (R7) and the cut's one-band latch (R15) join the entry check.
  5. Whether copied code-bias records are declared on the correction stream (R8, size unmeasured), and whether phase_family_codes stays in the adopted configuration until then.
  6. Whether E:L1X/L5X leaves disc_step_class_pairs under the table, and whether the loader refuses member entries in family_pairs and disc_step_class_pairs (R17).
  7. Whether the phase-family check compares the signal band rather than the band digit (R10) and refuses chains and many-to-one rows (R23).
  8. Whether to merge the network code now with the saved-state keys marked UNVALIDATED and off, or hold the merge until R1, R4 and R9 are repaired.

### Left unconfirmed

- B2b failing because of R1 (item 2): a different pin target is possible, not shown.
- The size of the copied records' effect at the users (item 6).
- S7 (b) identity on a run: no recorded gate (item 4).
- Not checked by the independent read: R12, R15, R19, R20 and R23 to R36 beyond the lines cited; `PbHold.cpp:515, 524`; the X1 and X2 refutations; the test-file table of section 4 beyond `test_chain_memory.cpp`; the root row's effect at an as-built pin with no target (whether it pins at 0); how often R2 or R3 occurs on observed data.

The full errata is at `D:/GnssProcessorSupport/gates/post_reset_program_2026-11-18/results/_critical_read/REV_errata.md`.

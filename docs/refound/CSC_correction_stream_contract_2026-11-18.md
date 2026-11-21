# CSC: what the correction stream promises the user (2026-11-18)

**STATUS**

- **id:** CSC
- **topic:** wire-contract
- **status:** open
- **summary:** A read of the network's correction stream as a contract: every record kind with each entry's promised meaning, the code that writes it, the user code that reads it and the tests that touch it; the rules of the two integer flags on a phase-bias record (`signal_integer`, `wide_lane_integer`) and of the three discontinuity counters (phase, code, clock), with every mismatch between writer and reader ranked by its cost to a user. The network releases a held user integer only through a counter step, a hold-generation change or a generation-id change, never through a flag falling, and the observe search reads the flag only with `network.require_signal_integer_for_ar` on. Network code read at commit `7fb0737` (frozen checkout `D:/GnssProcessorSupport/worktrees/read_7fb0737`), user-engine code at main `5ddc422`; nothing was changed, built or run.
- **source:** plan package 3 of the post-reset program, "Correction stream: what each record and flag promises, writer against reader" (2026-11-18); commits read: `7fb0737` (branch `chain/audit`) for network code and main `5ddc422` for user-engine code; four read-only first-stage reads and one skeptical re-check; the chain audit `CHA_chain_audit_2026-11-18.md` with parts 2 and 3; `docs/CONFIG_promotion_ledger.json`. The id SCC is already taken by `docs/record/refound/SCC_second_code_channel_2026-09-21.md`, so this document carries the id CSC. Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** The stream's two integer flags and three counters have many writers and few readers. The network releases a held user integer only by a counter step, a hold-generation change or a generation-id change, never by a flag falling, yet the settled flags can be armed without the unsettled steps (a departure from the adopted package). Under the phase-bias hold the user ignores every phase counter step, and the posture check recommends that hold for the ring-fix configuration. The only resolver gate on `signal_integer` is off by default and the ledger describes it two ways. `wide_lane_integer` has no reader in the engine code. A file reader outside service mode misses the setter-armed counter rules.

## Scope and method

Two trees are cited, each named on every citation:

- **W** = `D:/GnssProcessorSupport/worktrees/chain` at `7fb0737` (network code, the text format, the ledger, the reference configurations).
- **M** = `C:/GnssProcessor` at `5ddc422` (user-engine code: `src/core/realtime`, `filter`, `slip`, `corrections`, `measurement`, `products/SsrCorrectionStore`, the RTCM decoder, their headers and tests). Where a file is identical in both trees only one is cited.

Four first-stage reads mapped the flags, the counters, the record formats and the user's reading. A skeptical pass re-checked their claims against the code; its verdicts are carried as **confirmed**, **corrected**, **partly confirmed** or **unconfirmed** below. Claims that no single line shows carry "(inferred)". No run folder was opened; the ledger text is the only source for what the DOY 144 and 145 user replays were configured with.

The network's flags and counters reach a user by two paths only: the in-process store (`SsrCorrectionStore`, filled by `NetworkProductEmitter`) and the text file written by `src/core/products/NetworkProductIO.cpp` (W). There is no RTCM encoder; the RTCM decoder (M `src/core/rtcm/RtcmSsrDecoder.cpp`) serves third-party streams only. Abbreviations: **IO** = W `src/core/products/NetworkProductIO.cpp`; **NPE** = W `src/core/products/NetworkProductEmitter.cpp`; **SSB** = M `src/core/realtime/RealTimeEngineSsrBreaks.cpp`; **SCS** = M `src/core/products/SsrCorrectionStore.cpp`.

## 1. The record kinds

Entries are numbered 1-based, the tag included. "opt" marks an entry written only under its key. The writer of the text is IO:691-838 and the reader IO:131-425 unless a row says otherwise. Every line beginning with `#` is skipped by the reader (IO:135). The header is `# gnss_network_product v1` (no contract) or `v2` followed by an S line (IO:639-646), then the legend (IO:659-680).

### 1.1 E (epoch; starts a block)

| entry | meaning promised | writers (W) | readers (M) | tests |
|---|---|---|---|---|
| 2 week, 3 tow | GPS week and second of the content reference epoch | IO:691-705 | store epoch; the observation/correction sync hold (M `src/core/realtime/RealTimeEngineAdmit.cpp:79-110`) | M `tests/network/test_network_product_io.cpp` |
| 4 iod_ssr | per-epoch IOD-SSR | IO | clock pairing and orbit echo (SCS:226, 233-234) | M `tests/test_ssr_clock_orbit_pairing.cpp` |
| 5 provider_id, 6 solution_id | IGS-SSR identities | IO | none (header entries never read, section 6) | none found |
| 7 datum_changed | 0/1: clock-pivot or S-basis re-declaration; always written, optional on read (IO:156) | IO; marks from W `src/core/network/emit/SolPrep.cpp:634-641` | replay re-applies it as a constellation mark (IO:898) | IO round trips |
| 8 opt datum_consts | constellation letters with no separator (IO:704-705); empty = global mark on replay | IO | replay only | IO round trips |

### 1.2 G, I, R (optional epoch riders)

| record | meaning promised | writers (W) | readers (M) | tests |
|---|---|---|---|---|
| G (`network.emit_generation_time`) | wall-clock time of generation, week and tow (IO:712-714) | IO | none found in the user engine | IO round trips |
| I (`network.emit_generation_id`) | uint32 generation id from first-emit time, week*604800+floor(tow) (W `include/products/NetworkProductEmitter.hpp:183-193`); a change of a present id is a restart | NPE:336 | `reactToGenerationChange`: resets every ambiguity, re-inflates every receiver code-bias state (SSB:183-220) | network tests only (`setGenerationId`); no engine-level test found |
| R | satellites flagged for a per-satellite discontinuity (retro mark), variadic (IO:725-729) | IO | replay re-marks every channel of the satellite (IO:901-906), since R carries no channel set | IO round trips |

### 1.3 C (satellite clock; only when the solution has a clock)

| entry | meaning promised | writers (W) | readers (M) | tests |
|---|---|---|---|---|
| 2 sat | RINEX satellite id | IO | store key | M `tests/test_realtime_seams.cpp` |
| 3 clock_c0_m | m, estimable clock correction against the a priori named in S entry 11 | IO | `getSatelliteClockCorrection` (M `src/core/corrections/GnssCorrections.cpp:138-406`); applied only with a finite broadcast baseline, no echo withhold and matching IODE (:357-363) | `RealtimeSsrIodeGateTest`, `SsrClockStaleHoldTest` (M `tests/test_realtime_seams.cpp`) |
| 4 opt clock_c1_m_s | m/s drift; written as 0 when counters are serialized | IO | read as c1 | as above |
| 5 opt clock_disc | post-emit clock counter (`network.serialize_disc_counters`) | IO; counter rules NPE:743-760 | `reinflateOnClockDisc` (SSB:893-964), read ungated (SSB:908) | IO round trips; no engine-level test of the re-inflation found |
| iode=, oiod=, oepoch= | broadcast IODE anchor and orbit echo (`brdc_rts` only; oiod and oepoch only after iode) | IO | clock_iode (SCS:261), echo (SCS:262-263) | `SsrConsumerSeamTest` (orbit echo) |
| sigcc=, rho= | m, combined code-correction sigma; correlation with the combined phase (rho only beside sigcc) | IO | combined_code_sigma_m (SCS:269) | M `tests/network/test_combined_sigma_scope_warning.cpp` |
| lvl= | m, level step since the previous emission; the writer feature is removed, still read from older files (W `include/products/NetworkProductEmitter.hpp:117-127`) | none | level-step inflation under `ssr_level_step_inflate` (SSB:982-1050) | none found |

Positional parsing stops at the first entry containing `=` (IO:198-202).

### 1.4 B (satellite code bias)

| entry | meaning promised | writers (W) | readers (M) | tests |
|---|---|---|---|---|
| 2 sat, 3 signal | RINEX satellite and code | IO | exact-code match; a same-band sibling only under `ssr_bias_band_fallback` (SCS:287-305, 363-382) | `SsrBiasAttrMismatchTest` (M `tests/test_rtcm.cpp`) |
| 4 code_bias_m | m, a known constant on the row | IO; a phase-family member repeats its identity's value under `network.phase_family_codes` (W `src/core/network/emit/EmitStore.cpp:71-75`) | `addSsrBias` (M `src/core/measurement/ObservationEquation.cpp:288-306`); skipped in network mode (:290); iono-free keys never match, so IF rows get 0 | M `tests/test_observation_equation.cpp`; no test drives the age-gated read through `addSsrBias` (only `tests/test_rtcm.cpp` calls the four-argument getters) |
| 5 opt code_disc | post-emit code counter | IO; counter rules NPE:787-854 | `reinflateOnCodeBiasDisc`, only with `estimation.estimate_ifb` (SSB:753-856); otherwise one warning (SSB:857-883); no counter for a sibling key (SCS:322-331, 403-415) | no test of `reinflateOnCodeBiasDisc` found |
| isc=obs | inter-signal correction estimated from data (`network.sat_isc_state`) | IO | none found | none found |
| lvl= | legacy follow step | none | as C | none found |

An absent code bias is applied as zero and the satellite is kept (M `src/core/realtime/RealTimeEnginePrepare.cpp:66-67`); a satellite-bias state (M `src/core/measurement/MeasurementModelStates.cpp:1020-1039`) or a code down-weight (M `src/core/measurement/MeasurementModelVariance.cpp:221-237`) compensates. A plain absence with no sibling is not counted.

### 1.5 P (satellite phase bias)

| entry | meaning promised | writers (W) | readers (M) | tests |
|---|---|---|---|---|
| 2 sat, 3 signal | RINEX satellite and phase code | IO | exact-code match; no sibling fallback (SCS:398-399 counts a sibling miss) | `RtcmSsrPhaseBiasTest` (M `tests/test_rtcm_ssr_phase_bias.cpp`) |
| 4 bias_m | m; under a hold, a constant of the hold generation | IO; hold W `src/core/network/emit/PbHold.cpp` | row constant (M `ObservationEquation.cpp:296-299`); absent: 0, the ambiguity absorbs it | M `tests/test_pb_hold_presence.cpp`, `tests/test_observation_equation.cpp` |
| 5 signal_integer | 0/1. Default: 1 on every non-GLONASS estimated column. Ring fix: the signal is in a held family and its satellite is settled (section 2) | eight writers (section 2.1) | [corrected 2026-11-19: see Corrections, item 1] AR veto behind `network.require_signal_integer_for_ar` (M `src/core/measurement/MeasurementModelRow.cpp:117-131`); observe gate (M `src/core/realtime/RealTimeEngineArObserve.cpp:121-123`) | `test_ring_fix`, `test_continuity_guard`, `test_unsettled_steps`, `test_phase_family` (named, not read); M `tests/test_ar_observe_engine.cpp` |
| 6 wide_lane_integer | int. Ring fix: 1 for every signal of a held family, settled or not (W `src/core/network/ar/RingFix.cpp:78`) | ring-fix stamp; copies | **none** | IO round trip of -2 (M `tests/network/test_network_product_io.cpp:202, 1673`) |
| 7 opt phase_disc | post-emit phase counter | IO; rules NPE:652-990 (section 3.1) | `resetOnPhaseBiasDisc` and the hold, carry and translate paths (SSB:256-604, 663-696), read ungated | M `tests/test_pb_step_translate.cpp`, `tests/test_consumer_disc_carry.cpp`, `tests/test_pb_hold_presence.cpp` |
| sigma= | m, 1-sigma formal error of the phase bias | IO:816-817 | variance under `consume_phase_bias_sigma` (M `MeasurementModelVariance.cpp:195-214`) | not traced |
| sigc= | m, 1-sigma of clock and phase bias combined | IO | variance under `consume_combined_sigma` (M `MeasurementModelVariance.cpp:79-88`) | M `tests/network/test_combined_sigma_scope_warning.cpp` |
| gen= | hold generation; a change is the re-float declaration | IO; W `PbHold.cpp:193-248, 443` | hold path (SSB:676-693); AR candidates (M `RealTimeEngineArObserve.cpp:115-118`) | M `tests/test_pb_hold_presence.cpp` |
| cmp= (read only) | chain component | **none** (IO:807-833 has no cmp=; W `NetworkProductEmitter.hpp:88-96` says so); scripts only (W `scripts/simnet_truth_wire.py:106`, `scripts/ird_frame_census.py:1480`) | IO:264-266 sets `chain_component`; the observe gate needs it nonzero (M `src/core/realtime/ArObserve.cpp:38`) | not traced |

A phase-family member (Galileo L1X, L5X) repeats its identity's record, flags included (W `EmitStore.cpp:60-79`), and the emitter copies the counter (NPE:976-990). The only declaration is one `#` note in the header (W `src/core/network/emit/SolPrep.cpp:1203-1209`), which the reader skips and an S re-broadcast does not repeat.

### 1.6 U, H, N

| record | meaning promised | writers (W) | readers (M) | tests |
|---|---|---|---|---|
| U (`network.emit_ura`; after the satellite's P records) | 2 sat, 3 clock_ura_m: m, 1-sigma, = max(1, mean sigma0) x formal clock sigma (W `docs/report/network_wire.tex:583`) | IO:835-836 | clock-window variance, only under `network.consume_ura` or an armed clock hold (M `MeasurementModelVariance.cpp:129-183`) | not traced |
| H (`network.emit_epoch_status_record`) | 2 week, 3 tow, 4 status {ok, partial, withheld_sigma0, withheld_gate, withheld_sh (parsed only)}, 5 sigma0 (`%.4f`, 0 = unavailable), 6 n_sats_published, 7 withheld (`-`, `*`, or letters joined by `;`); attaches to the next E only on an exact time match (IO:163-171, 295-300) | IO:623-637; the pre-open heartbeats are flushed (W `SolPrep.cpp:1215-1219`) | one log line per transition, gates nothing (SSB:228-248; M `include/products/SsrCorrectionStore.hpp:44-45`) | M `tests/test_h_heartbeat_rider.cpp` |
| N (`network.ledger_emit`) | 2 constellation, 3 band digit, ph= / co= / comp= in station-support order, dph= / dco= the pinned signals (W `include/products/SignalDeclaration.hpp:28-29`); a contiguous block replaces the previous declaration (IO:141-142, 181-186) | IO | declared roster: band latch and flip attribution (SSB:52-90, 92-139) | not traced |

### 1.7 S (the contract; header and re-broadcast, IO:471-616)

| entry | meaning promised | readers (M) |
|---|---|---|
| 2 ref_rx | pivot receiver | replay |
| 3 bias_sign | +1 | replay |
| 4 windup_id | wind-up construction, e.g. `nominal-wu93` | replay check |
| 5 amb_datum | 0 pivot receiver, 1 sum-to-zero (removed), 2 single-receiver chain (W `include/network/solve/Sbasis.hpp:153`) | observe gate (chain datum) |
| 6 isb_datum | 0 satellite clock pin, 1 sum-to-zero, 2 broadcast-anchored (Sbasis.hpp:200) | replay |
| 7 emit_cb, 8 cb_datum | 0/1; 0 sum-to-zero (only value) | replay |
| 9 disc_thresh_m | m, value-jump bar | replay emitter |
| 10 emit_clock_disc | the clock counter channel | replay emitter |
| 11 apriori_id | a priori orbit and clock; prefix `sp3+brdc:` = broadcast clock (W `include/products/NetworkProductIO.hpp:862`) | replay |
| 12+ `<const>:<pivot_sat>` | realized pivots | replay |
| keys | gid, stale, cbjump, clkset, frame, cepoch, atx, oref, pdr, agek, discfrac, discpv, hsig, cbpin, gfpin (retired, read only), sat_gf_datum, discscope, pbhold, wumode (write order IO:501-614) | replay |

The replay emitter takes eight parameters: entries 9 and 10 and the keys stale, cbjump, clkset, discfrac, discscope, discpv (IO:1132-1144). The legend shows `pbhold=1` only (IO:675); the writer emits 2 (phase-clock hold) and 3 (smoothed hold) (IO:603-605) and the reader also accepts 4 (IO:369). The reference configurations arm `emit_pb_hold_phase_clock` (W `config/golden/golden_field_refound.json:951`), so their products carry `pbhold=2`, a value the legend does not show. A stream with no S line is refused unless `network.replay_allow_undeclared_wire` is set (IO:1146-1168).

### 1.8 Records the user reads only in process

- **Slant ionosphere:** iono_m and sigma_m as a pseudo-observation on the ionosphere state (M `src/core/realtime/RealTimeEngineEstimate.cpp:192-204`), aged at half the epoch interval else 30 s (:177); missing or sigma 0 skips the satellite without a count. Test: M `tests/test_slant_ionosphere_presence.cpp`.
- **Declared level steps:** under `ssr_level_step_inflate` (SSB:982-1050).
- **Orbit:** delta_rac, delta_rac_dot, iode, t0 from epoch_time and update_interval_s, iod_ssr, reference_point (SCS:170-285); applied only when the broadcast IODE matches (M `src/core/model/ModelGeometry.cpp:1042-1056, 1382-1390`). A missing or old orbit leaves the broadcast orbit unchanged, with no per-satellite count.

### 1.9 Age rules (user side)

One rule ages every record (SCS:27-43): a record never stamped is never fresh; a record more than max_age ahead of the observation is refused. The limits differ by kind:

- orbit and clock: `processing.ssr_max_age_s`, default 30 s (M `src/core/config/GnssConfigLoad.cpp:606-611`), widened for the clock by `ssr_clock_stale_hold_factor` (default 0, off);
- code and phase bias: aged only under `processing.enable_ssr_bias_max_age` (default false, :613-614); otherwise the last record applies indefinitely (SCS:287-305, 432-446). The network-user profile arms it when the key is absent (:615-621; not traced per posture);
- the per-epoch coverage line uses the store default of 60 s (M `src/core/realtime/RealTimeEngineSsrDiag.cpp:46`), so it can show a clock "in store" that the apply path has refused.

Every push of every record kind except slant ionosphere overwrites the satellite's header (SCS:106-164; per satellite, `by_sat_`, not global). `getSsrEpoch` then serves as the clock's content age in the URA zero-order-hold term and the combined-sigma age inflation (M `MeasurementModelVariance.cpp:100-106, 147-160`). The cost is variance only, and near zero in process where every kind is pushed at one epoch (inferred).

## 2. The two integer flags

### 2.1 Writers, in order within one emission (tree W)

1. **Default**, W `src/core/network/solve/Estimator.cpp:1576`: `signal_integer = (system != GLONASS)` on every estimated phase-bias column, with no datum, fixing or settled condition (the comment at 1563-1575 asserts the integer property under every datum). `wide_lane_integer` stays at the struct default 0.
2. **Per-signal fill**, W `SolPrep.cpp:1361-1367, 1415-1429`, behind `network.n3c_wire_per_signal`: copies a source record, flags included, onto a modelled signal with no column (source chosen by band digit and channel family, 1397-1406). Not re-verified by the skeptical pass.
3. **Settled-flags stamp**, W `RingFix.cpp:64-81` (checked by the writer), called from `SolPrep.cpp:1371-1372`, behind `network.ring_fix.settled_flags` (default false; needs `ring_fix.pairs`, W `GnssConfigLoadNetwork.cpp:1574-1578`). It overwrites both flags on every record: `signal_integer = held_family && settled` (77), `wide_lane_integer = held_family ? 1 : 0` (78). A record of no held family gets 0/0. The settled set is the satellites reached from the datum receiver through held links of this epoch plus tree edges that entered as held links (W `src/core/network/ar/RingFixSeams.cpp:1555-1608`); cascade families combine by AND (1597-1600).
4. **Phase-family member copy**, W `EmitStore.cpp:60-79`, called at 476-478. Cannot conflict with writer 3: the loader refuses a pair naming a member (W `GnssConfigLoadNetwork.cpp:1549-1554`).
5. **Emitter store record**, NPE:952-962: copies both flags, then lowers `signal_integer` on any key the continuity guard holds (955-961).
6. **Guard trip**, NPE:126-158, behind `ring_fix.continuity_guard_cyc` > 0 (W `src/core/network/driver/ApplicationNetworkSetup.cpp:1614-1616`): lowers `signal_integer` on both family signals for the hold and steps both counters. `wide_lane_integer` stays 1 (NPE:147-149, 164).
7. **Emitter member copy**, NPE:976-990: copies the identity's `signal_integer` and counter, mirrors the lowered set (988-989).
8. **File mirror**, W `EmitStore.cpp:482` calling NPE:160-165: lowers the same keys in the solution so the file (IO:809) matches the store.
9. **RTCM decoder**, M `src/core/rtcm/RtcmSsrDecoder.cpp:401-412`: third-party streams only; `wide_lane_integer` is 2 bits unsigned (402).

Writers 1 and 3 disagree (a constant against the settled rule); when armed, writer 3 wins because it runs later. Writers 5 to 8 only lower or copy; the member copy raises a member to its identity's value, which is intended.

### 2.2 Readers (tree M)

- `phaseBiasCoveredForAr` (M `MeasurementModelRow.cpp:117-131`), behind `network.require_signal_integer_for_ar` (default false, M `src/core/config/GnssConfigLoadNetwork.cpp:8090-8092`): a bias present with the flag false makes the ambiguity unresolvable; an absent bias never vetoes (123); for an iono-free key either component vetoes (126). It feeds `markUnresolvableAmbiguities` (M `MeasurementModelStates.cpp:765, 784`), which clears and re-marks every epoch (729); the resolver skips marked ambiguities (M `src/core/filter/AmbiguityResolver.cpp:432`).
- [corrected 2026-11-19: see Corrections, item 1] Observe gate (M `RealTimeEngineArObserve.cpp:115-123`), behind `network.closed_loop.ar_observe` (default false, measure-only): admits a bias only with the chain datum declared, a nonzero cmp and the flag 1.
- `wide_lane_integer`: no reader in either tree. The resolver's wide-lane integers are locals from Melbourne-Wubbena (M `AmbiguityResolver.cpp:1851, 2383, 2440`).
- The network's own station builder calls the user gate on the process-wide store (W `src/core/network/assembly/LocalBirth.cpp:233, 244`).

### 2.3 What releases a held user integer

[corrected 2026-11-19: see Corrections, item 2] **Confirmed (the skeptical pass, joining two first-stage reads).** A flag falling only removes the ambiguity from the next search (M `AmbiguityResolver.cpp:432`). Fix-and-hold writes into x and P by a pseudo-update (M `AmbiguityResolver.cpp:3563-3567`) and stays until `resetAmbiguity` zeroes the row and column (M `src/core/filter/StateManager.cpp:499-505`). Only a counter step on the plain path triggers that reset (SSB:303-312). The counter, not the flag, is the release; every rule that lowers a flag must step the counter in the same emission to release a user.

## 3. The discontinuity counters

### 3.1 Writer rules (tree W)

**Phase counter** (P, per satellite and signal), stepping on a key with a baseline (NPE:938) when any of these holds:

1. a constellation datum mark, including on a stale return (NPE:875, 426-428): clock-pivot handover (SolPrep.cpp:634-641), re-anchor of the held clock frame or its resume seam (EmitStore.cpp:295-321), the warm-resume phase-bias seam over `kResumePbSeamMarkFraction` (387-414), re-anchor of the phase frame hold (427-431); pending until the constellation is emitted (NPE:1025-1050);
2. a satellite mark on the phase channel (NPE:876, 439-463): URA retro, reference-satellite slip, datum-station slip, held-basis anchor handover, gross-LOS exclusion (SolPrep.cpp:715-745), ephemeris cutover with an evicted prior (763-783), IF-pin re-selection except a continued pair (885-948); under the channel scope a mark not naming the phase channel is suppressed and logged as MARK_SCOPE;
3. a value jump on a fresh baseline, |Δpb − level_step| > bar (NPE:884-895), replaced by the phase-visible verdict when `network.disc_phase_visible` is armed (900-907) and by the step-class verdict for classified keys (910-918), which declares nothing for quiet or ionosphere-like steps (533-584);
4. a release mark from the chain (NPE:920-929), only when a step-class family is set (204-205) and the chain record is armed (EmitStore.cpp:472-473), which hangs on a trace key (CHA part 2, P4);
5. unsettled steps: a held family's key at every emission where the two flags are not both up, and once at settling (NPE:652-678); with the unheld form, every signal of no held family at every emission (679-697);
6. a continuity-guard trip (NPE:141-155);
7. a member copies its identity's counter (NPE:976-990).

**Code counter** (B): forced by a constellation mark or a code-channel satellite mark (NPE:805-806, 834; code-bias excursion re-initialisation SolPrep.cpp:801-817, re-seed 985-988); on a fresh baseline, a change of the (constellation, signal) set, of the satellite's band tuple (787-789), or an opt-in value jump (`code_bias_disc_jump_test`, true in the reference configurations; 816-827); member copy (846-854).

**Clock counter** (C, per satellite; only with `emit_clock_disc`, true in the reference configurations): forced by a constellation mark, a clock-channel satellite mark (CLK-R step test EmitStore.cpp:337-342 at `clock_disc_jump_m` 1.5 m; temporal clock-anchor decoupling SolPrep.cpp:848-854; re-seed 985-988) or a set change (`clock_disc_on_set_change`, off) (NPE:743-760). No magnitude or staleness test.

### 3.2 Reader rules (tree M; order M `src/core/realtime/RealTimeEngine.cpp:543-549`)

1. `reactToGenerationChange` (SSB:183-220).
2. Phase: the hold path first (SSB:264; checked by the writer at 663-696: a held key's counter is consumed by `noteSsrDiscontinuity` and only a generation change resets), then translate (272) or carry (275), else the plain reset (303-312). `noteSsrDiscontinuity` reacts to a change in either direction and only records at first sight (M `StateManager.cpp:448-459`).
3. Code: `reinflateOnCodeBiasDisc`, behind `estimate_ifb` (SSB:848-856).
4. Clock: `reinflateOnClockDisc` (SSB:893-964), effective only when `estimation.clock_disc_isb_reinflate_std` > 0 (default 0, M `GnssConfigLoad.cpp:1274-1276`); the trace logs "reinflate_isb" before the test (SSB:915 against 927).

Only modelled signals are read, so a step on an untracked signal is seen at re-acquisition.

## 4. Mismatches ranked by cost to a user

### A. Can leave a wrong integer held

**A1. Hold plus ring fix: every phase counter step is ignored at a held key. Confirmed (checked by the writer).** With `network.emit_pb_hold`, every emitted phase key carries `hold_generation` >= 1 (W `PbHold.cpp:193-248, 443`). The user claims held keys first, consumes the counter only to update its memory, and resets only on a generation change (SSB:677-693; the summary at 707 prints "disc-counter changes IGNORED"); downstream reactions skip the claimed key (SSB:301-302, 406-407, 520-521). Unsettled steps, guard trips, release marks, forced marks and value jumps therefore do nothing at a held key, and with 2.3 the ring fix's integrity steps cannot release a user. The adopted ring-fix configuration has the hold off, but the posture check lists `network.emit_pb_hold` as a scoped gap of that configuration (W `docs/refound/CHA_chain_audit_2026-11-18_part3.md:102`), so the tooling recommends turning it on, and no loader rule refuses the combination (no hit under W `src/core/config`). The recommendation and the mechanism contradict each other; this ranks first.

**A2. The settled flags can be armed without the unsettled steps. Confirmed (checked by the writer); missed by the first-stage reads.** The loader requires `settled_flags` for `unsettled_steps` (W `GnssConfigLoadNetwork.cpp:1581-1583`) but not the reverse. The stamp lowers the flag with no counter step (W `RingFix.cpp:77`). A satellite leaving the settled set loses its flag while a user that already fixed it keeps the integer (2.3). Both keys are RECOMMEND_SCOPED in the ledger.

**A3. Without the veto key, a default user fixes unsettled signals. Confirmed.** The only resolver gate on the flag sits behind `network.require_signal_integer_for_ar`, default false (M `GnssConfigLoadNetwork.cpp:8090-8092`); no user profile or derived configuration arms it (the parse line is the only hit), and no configuration under M `config/` sets it. The ledger says two things: the key's own row calls it "default off, never graded" (W `docs/CONFIG_promotion_ledger.json:546`), while the row of `network.ring_fix.settled_flags` (line 52, adopted 2026-11-16) describes the DOY 144 and 145 user replays as validated with the key on. The settled flags thus change no user fix unless the user arms a key the ledger calls ungraded.

**A4. The carry and translate readers keep an integer the network declared invalid. Confirmed.** Under `processing.ssr_phase_bias_disc_carry` a step shifts the ambiguity by the applied Δ (even Δ = 0) and keeps its covariance (M `src/core/realtime/PbDiscCarryLedger.cpp:60-64`; SSB:432-445); under `ssr_phase_bias_step_translate` a step with |Δ| <= eps writes nothing (M `src/core/realtime/PbStepTranslateLedger.cpp:75-82`). Unsettled steps, release marks, guard trips and forced marks on a continuous value all mean "invalid, no value move" and are lost. Both keys default false (M `GnssConfigLoad.cpp:674, 686`), as does `network.closed_loop.pb_disc_carry` (M `GnssConfigLoadNetwork.cpp:13188`).

**A5. A file reader outside service mode loses the setter-armed counter rules. Confirmed, with a correction.** `makeReplayEmitter` forwards only the eight constructor parameters (IO:1137-1143). Step-class families, unsettled steps and their unheld form, the continuity guard and the member table are set only at network setup (W `ApplicationNetworkSetup.cpp:1581-1623`), have no S token (IO:673-675), and release marks are not serialized. The setup comment "Every construction parameter also travels in the serialized contract" (W `ApplicationNetworkSetup.cpp:1570-1571`) holds for the constructor only. `network.serialize_disc_counters` defaults false (W `GnssConfigLoadNetwork.cpp:11772-11774`) and is excluded from the reference configurations by a manifest exception (W `config/golden/golden_manifest.json:200`). **Correction to both first-stage reads:** the service-mode bundle forces the key on (W `GnssConfigLoadNetwork.cpp:11920-11921`, checked by the writer), so a service-mode file carries the counters. The guard's lowered flags reach the file (W `EmitStore.cpp:480-482`); its counter steps do not. A replay of a channel-scoped stream also over-marks (IO:901-906). With 2.3, a file user can keep an integer the in-process user would release (inferred).

**A6. The plain reset is a no-op under the NlWl basis. Confirmed.** The plain path resets `makeAmbiguityKey(sat, code)` (SSB:307); under NlWl the states are NL and WL keys (M `MeasurementModelStates.cpp:248-251`) and `resetAmbiguity` on an unknown key returns false at debug level (M `StateManager.cpp:473-489`), uncounted. The carry and translate paths count this case (`reset_basis`). Applies only when NlWl is chosen.

**A7. Clock-only marks step the clock counter, which never touches an ambiguity. Partly confirmed.** The CLK-R test, the clock-anchor decoupling (whose comment records a phase-bias move of at most 0.011 m) and the re-seed step only the clock channel. The legacy phase test looks at the phase bias only (NPE:889-895), so (clock minus phase bias) can step with no phase counter step, absorbed by a held user ambiguity (inferred; size unconfirmed, no run opened). Under the phase-clock hold the clock step is transferred into the held bias (W `PbHold.cpp:258-261, 412-438`), so the case applies to postures without it, including the ring-fix configuration (inferred). `network.disc_phase_visible` closes it but is off in the reference configurations. The ISB re-inflation that would react defaults to 0; the posture table recommends 10 m in scope (M `src/core/config/GnssConfigNetworkPosture.cpp:637-641`).

**A8. Drift below the bar steps no counter. Unconfirmed by the skeptical pass.** Value tests compare against the previous emission (NPE:942, 839); the bar is 0.25 λ in the reference configurations (NPE:186-197) or 0.05 m flat (W `NetworkProductEmitter.hpp:210`). Repeated 0.2-cycle moves accumulate without a step. The only integrated test, the continuity guard, covers held families while both flags are up and is off by default.

### B. Flags without effect, or a user reading the wrong thing

**B1. Without the settled flags, `signal_integer` is a constant. Confirmed.** Writer 1 sets 1 on every CDMA column (W `Estimator.cpp:1576`); the network configuration has no GLONASS, so the veto of A3 never fires even when armed. "Integer-preserving on every CDMA record" is a stronger promise than the float chain guarantees per epoch (inferred). A per-signal fill copies that 1 onto a bias that is another signal's value (writer 2; not re-verified).

**B2. The observe gate never admits a candidate from a stream the network writes. Confirmed; missed by the first-stage reads.** It requires cmp != 0 (M `RealTimeEngineArObserve.cpp:121-123`); cmp comes only from cmp= (M `ArObserve.cpp:38`, M `src/core/products/NetworkProductIO.cpp:265`), which the writer never emits. The ledger row admits that an untokenized stream admits nothing (W ledger line 546).

**B3. The network can veto its own ambiguities. Confirmed as a path; reach unconfirmed.** If the network configuration set `network.require_signal_integer_for_ar`, LocalBirth would apply the user gate to flags the network emitted earlier (W `LocalBirth.cpp:233, 244`). Whether the network search reads that slot is not established.

**B4. Bias IOD is never compared with the clock IOD. Confirmed.** The iod_ssr of code-bias, phase-bias, URA and slant records is never compared; `getSsrIod` is called only in tests (M `tests/test_rtcm.cpp:1261-1267`). A bias set of one solution can be applied over a clock of another, untested and uncounted.

**B5. Smaller mismatches (unconfirmed by the skeptical pass).**
- A code bias falls back to a band sibling (SCS:377-379) but its counter getter has none (SCS:322-331, 403-415).
- A (constellation, signal) set change steps every satellite's code counter on that channel (NPE:386-393, 835), justified by a sum-to-zero code datum (365-370); with the satellite GF code bias pinned at broadcast T_GD these steps may declare re-references that did not happen (inferred).
- A member's own counter rules are overwritten by its identity's (NPE:976-990, 844-854): consistent while a member is a repeat, wrong once it carries its own term.
- Under a clock hold factor > 1 a decoded clock can pair with an orbit aged between max_age and the hold window (SCS:220, 233-234) while the geometry applies no orbit at max_age (M `ModelGeometry.cpp:1042, 1382`): a corrected clock on an uncorrected orbit (off by default).

### C. Variance only, or documentation

- **C1.** The header overwrite of 1.9 resets the clock's age in two variance terms. Confirmed; variance only.
- **C2.** The legend under-declares `pbhold` (1.7). Confirmed. `HeaderLegendDeclaresEveryEmittedField` checks key names, not values, and arms no hold (W `tests/network/test_network_product_io.cpp:1068-1071`).
- **C3.** Code comments that disagree with the code: IO:130 names an F line that does not exist (IO:419 rejects unknown tags); IO:1044-1045 says seven parameters are compared where eight are (W `NetworkProductIO.hpp:620`); NPE:964 says sigma has no record, but sigma= is written (IO:816-817). The banner of W `NetworkProductIO.hpp:28-84` omits G, U, R, N and fifteen keys the legend carries.
- **C4.** `wide_lane_integer` range: the text reader parses with `std::stoi` and no range check (IO:250) and a test round-trips -2; the RTCM entry is 2 bits unsigned.

## 5. Where the reads disagree

- **What releases a held integer.** The flag read left it open; the counter read said the reset removes the hold. Both hold, and 2.3 joins them: the counter is the only release.
- **`wide_lane_integer`.** The flag read called its value misleading ("held family", not "this satellite's wide lane fixed"); the records read called it undocumented. **Corrected by the skeptical pass:** "1 for the whole held family" is the declared contract in code (W `include/config/GnssConfigTypes.hpp:1947-1950`, W `include/network/ar/RingFix.hpp:1498-1505`, W `RingFixSeams.cpp:402-404`). The defect is the missing reader and the clash with the IGS-SSR meaning of the entry, not the code's own documentation. The format text and the active documents still do not define it (section 6).
- **A2** was missed by all four reads; the flag read stated only the protective direction (unsettled steps need settled flags; the guard needs the unsettled steps, NPE:168-169), which still holds.
- **Service mode** (A5): both reads said the file never carries counters by default; service mode does.

## 6. Entries with no reader, and entries with no written definition

**Written, never read by the user engine:**
- P `wide_lane_integer` (both trees);
- E provider_id and solution_id, and the header's multi_message, system, dispersive_bias_consistency and mw_consistency;
- phase-bias yaw_angle_deg, yaw_rate_deg_per_s, dispersive_bias_consistency, mw_consistency (decoded only);
- `code_phase_corr`, copied into the merged correction (SCS:270) and unused (M `include/rtcm/RtcmTypes.hpp:212-214`);
- the iod_ssr of every bias, URA and slant record (B4);
- `SsrHighRateClock::update_interval_s` (the pairing uses the clock's interval, SCS:63);
- G (generation time) and B isc=obs: no user reader found;
- a sibling code's counter (documented limitation, M `SsrCorrectionStore.hpp:853-856`).

**Read, never written by the network:** P cmp= (scripts only); C/B lvl= (removed writer); H `withheld_sh` (the gate was deleted on 2026-10-14; parsed only, SCS:619); S gfpin= (retired).

**No written definition outside code comments:**
- the ring-fix meanings of both flags, beyond one sentence of NRF part 6 (W `docs/refound/NRF_network_ring_fixing_2026-11-14_part6.md:185`); the legend names the entries only (IO:664);
- `pbhold=2/3/4`;
- the phase-family member records and the member B copy;
- the setter-armed counter rules as a property of the stream (A5);
- lvl= and isc=obs in any active document (closed records only);
- the H pre-open flush.

The one consolidated description, W `docs/report/network_wire.tex:730-764`, puts the URA in the C record (it is U), counts seven replay parameters (eight; discpv= missing) and omits G, U, N, sigma=, sigc=, sigcc=, rho=, isc=, lvl=, iode=/oiod=/oepoch=, agek=, hsig=, pdr=, gfpin= and the member records. W `docs/rt_service_operations.md:52-71, 88-92` presents `withheld_sh` as new with an active gate, and says epochs withheld before the first emission leave no record, which the flush no longer makes true.

## 7. Contract tests writable without an engine run

Each uses the emitter, the text IO, the store or a user ledger directly, as the existing suites do (feasibility inferred from the cited types). Where a test pins today's gap it is written to the rule the author chooses (section "Decisions for the author").

1. **Flag fall steps the counter.** An emitter with `settled_flags` on: a key whose `signal_integer` falls from 1 to 0 steps `phase_disc` in the same emission, with and without `unsettled_steps` (A2).
2. **Hold and ring fix.** Either the loader refuses `emit_pb_hold` with `ring_fix.settled_flags`, or the user hold path resets a held key whose counter steps while its flag is down (A1).
3. **File equals store.** Emit a sequence with the setter rules armed, write the file, replay through `makeReplayEmitter`, and compare the phase counters key by key; a second arm with `serialize_disc_counters` (A5).
4. **Legend declares values.** Extend `HeaderLegendDeclaresEveryEmittedField` with a hold armed, asserting every emitted `pbhold` value is declared (C2).
5. **cmp= round trip and the observe gate.** A record written by the network admits a candidate in `RealTimeEngineArObserve`, or the test pins that it cannot (B2).
6. **Zero-Δ step with the flag down.** `PbDiscCarryLedger` and `PbStepTranslateLedger` reset (or count) a key whose counter steps with Δ = 0 while `signal_integer` is 0 (A4).
7. **NlWl plain reset.** A phase counter step under the NlWl basis resets the NL and WL keys of that satellite, or is counted (A6).
8. **Veto reach.** With `require_signal_integer_for_ar` on, a present bias with flag 0 marks the ambiguity unresolvable and an absent one does not; a resolver already holding that integer keeps it until a counter step (2.3).
9. **Sibling counter.** A code bias served by the band fallback returns the sibling's counter (B5).
10. **IOD pairing.** A bias record of another iod_ssr than the clock is refused or counted (B4).
11. **Header age.** Pushing a bias or URA record does not change the age `getSsrEpoch` reports for the clock (C1).
12. **`wide_lane_integer` range.** The text reader accepts only the RTCM range 0 to 3, or the test documents the wider text contract (C4).
13. **Clock-only mark.** An emitter step in clock with no phase-bias move: with `disc_phase_visible` off no phase counter steps; on, it does (A7).
14. **Sub-bar drift.** A run of 0.2-cycle moves steps no counter; with the guard armed it trips at its threshold (A8).
15. **Generation change and status.** An engine-level test of `reactToGenerationChange` and of the status log (none found).

## 8. Runs a later validation would need (none requested now)

- A ring-fix stream with `emit_pb_hold` on, read in process, counting unsettled steps and guard trips the user ignored (A1).
- A ring-fix stream with `settled_flags` on and `unsettled_steps` off, counting user integers held past a flag fall (A2).
- A file replay of a ring-fix stream against the in-process user, comparing release counts, outside service mode (A5).
- A user replay with and without `require_signal_integer_for_ar` on DOY 144 and 145, to settle the ledger statement (A3).
- A trace of a CLK_STEP or CLK_ANCHOR event on the ring-fix configuration, for the size of A7.

## Decisions for the author

1. **One owner per flag rule.** `signal_integer` is written in eight places across five files (section 2.1) and released only through a counter step written elsewhere. Name one function that sets both flags and emits the counter step that must accompany every fall, so that a flag can never fall without releasing a user.
2. **`wide_lane_integer`: keep it with a reader or retire it.** It has no reader in either tree, its ring-fix meaning ("held family") differs from the IGS-SSR meaning, its text range exceeds the RTCM range, and a guard trip leaves it at 1. Keeping it needs a user rule that reads it and a definition in the format text; retiring it means writing 0 and dropping the stamp line.
3. **The hold and the ring fix.** Refuse `emit_pb_hold` together with `ring_fix.settled_flags` in the loader, or make the user hold path honour a counter step on a key whose flag is down; and remove `emit_pb_hold` from the scoped gaps the posture check reports for the ring-fix configuration until then (A1).
4. **Settled flags without unsettled steps.** Make `settled_flags` imply `unsettled_steps`, or have the stamp step the counter on every fall (A2).
5. **The veto key.** Resolve the ledger's two statements on `network.require_signal_integer_for_ar`; if the settled flags are kept, declare them on the stream (an S key) and arm the veto in the user profile when the stream declares them (A3).
6. **Carry and translate semantics.** Decide whether a counter step on a key whose flag is down is a reset under the carry and translate paths (A4).
7. **The file contract.** Carry the setter-armed counter rules as S keys (so the replay emitter can apply them), or serialize the counters outside service mode; until then, state in the format text that a file user outside service mode misses them (A5).
8. **cmp=.** Write it, or drop the cmp requirement from the observe gate (B2).
9. **Clock-only marks.** Decide whether the ring-fix configuration arms `network.disc_phase_visible` (A7).
10. **One description of the format.** Choose one owner for the written definition (the legend, `network_wire.tex` or a new reference page) and bring it to the code: flag meanings, `pbhold` values, member records, the eight replay parameters, the U record.

## Not checked

- Whether the resolver path in the network search reads the `unresolvable` slot that LocalBirth sets (B3).
- Whether `channelFamilyOf` keeps every per-signal fill off a source with a non-integer inter-signal offset, and the fill's flag copy itself (writer 2, B1).
- That no reader checks two flagged satellites share a component (the observe gate requires a component; the resolver does not).
- The size of A7, A8 and A5 in numbers; every one needs a run.
- Which configuration the DOY 144 and 145 users carried; only the ledger text speaks to it.
- Whether the adopted ring-fix configuration arms `serialize_disc_counters`, `disc_step_class_pairs` or `disc_phase_visible` (the adopted configuration file was not read).
- Whether the user wide lane takes network code biases, which sets the reach of A8 and the code set-change item of B5.
- Whether the N declaration lists member signals, and whether the replay driver warns when unserialized phase counters disagree (it warns only when serialized counters exist, IO:920-957).
- The intended user response to an ionosphere-like step that the step class leaves undeclared.
- The flag tests `test_ring_fix`, `test_continuity_guard`, `test_unsettled_steps` and `test_phase_family` (named only), and the U, N and sigma= tests.
- `docs/record/archive/` was not searched per token; "no written definition" covers the active set and the record outside the archive.
- Whether the network-user profile arms `enable_ssr_bias_max_age` on every posture in use.

## 2026-11-19: corrections from the check against the code

An independent check opened every cited line: 40 claims checked, 37 confirmed, 2 wrong, 1 line moved. Trees: **F** = frozen checkout `D:/GnssProcessorSupport/worktrees/read_7fb0737` at `7fb0737` (network code and record); **M** = `C:/GnssProcessor` at `5ddc422` (user-engine code). The body's tree **W** (`D:/GnssProcessorSupport/worktrees/chain`) is read as F: every network line checked at F matched the body's line numbers. Between F and M the reader files `SsrCorrectionStore.cpp`, `GnssCorrections.cpp`, `PppModel.cpp`, `RealTimeEngineSsrBreaks.cpp`, `MeasurementModelRow.cpp`, `RealTimeEngineArObserve.cpp` and `NetworkProductIO.cpp` are identical; `AmbiguityResolver.cpp`, `StateManager.cpp` and `GnssConfigLoad.cpp` differ, and the body cites M for them.

1. **The observe gate (section 2.2; section 1.5, row 5).** The body says the observe gate on `signal_integer` sits behind `network.closed_loop.ar_observe`. The code shows that `network.closed_loop.ar_observe` (default false, measure-only) arms the observe search, and the flag test inside it is behind `config_.networkRequireSignalIntegerForAr()`: M `src/core/realtime/RealTimeEngineArObserve.cpp:37` (ar_observe), `:47` and `:121` (veto key). Corrected reading: the gate sits inside the observe search and acts only with `network.require_signal_integer_for_ar` on; both keys are needed.
2. **What releases a held user integer (section 2.3; the draft's Outcome).** The body says "Only a counter step on the plain path triggers that reset (SSB:303-312)". This is overstated: a hold-generation change resets (M `src/core/realtime/RealTimeEngineSsrBreaks.cpp:686-687`), a generation-id change resets every ambiguity (M `RealTimeEngineSsrBreaks.cpp:189-198`), and carry and translate refusals reset (M `src/core/realtime/PbDiscCarryLedger.cpp:41-55`). Corrected reading: of the network's flags and counters, only a counter step (plain path, SSB:303-312), a hold-generation change (SSB:686-687) or a generation-id change (SSB:189-198) triggers that reset; a flag never does. The conclusion that a flag falling never releases a user stands.
3. **B2 holds only with the veto key on.** The body's title says the observe gate never admits a candidate from a stream the network writes. With `network.require_signal_integer_for_ar` off, the search admits every candidate whatever its cmp (M `RealTimeEngineArObserve.cpp:121-123`). Corrected title: with the veto key on, the observe gate never admits a candidate from a stream the network writes.
4. **A2 is a departure from the adopted package.** The adopted package arms the unsettled steps (ledger row of `network.ring_fix.settled_flags`, F `docs/CONFIG_promotion_ledger.json:52`), so the loader gap of A2 matters for a configuration that departs from it, not for the adopted posture.
5. **A1 is wider than the body states.** The reference configurations arm `network.emit_pb_hold` (F `config/golden/golden_field_refound.json:922`), so a ring-fix configuration built from them carries the combination of A1 unless the hold is removed.
6. **`wide_lane_integer` readers (sections 2.2 and 6).** "No reader in either tree" holds for the engine code only: analysis scripts read it (for example F `scripts/nrf_settled_flags.py`). The C++ grep of `src` and `include` in F and M finds writes, the text parse and the RTCM decode only.
7. **Tree of the network code (Scope and method).** The body names the worktree `D:/GnssProcessorSupport/worktrees/chain`. Read: network code at commit `7fb0737` (frozen checkout `D:/GnssProcessorSupport/worktrees/read_7fb0737`); the same for the definition of **W**.

### Line numbers that moved

- Section 3.2, reader order: M `src/core/realtime/RealTimeEngine.cpp:543-549` is `:542-546` at M.

### What stands

- The Outcome stands with correction 2 (the header above carries it).
- A1 (the hold ignores every counter step) is the strongest finding and is strengthened by correction 5.
- A3, A4, A5 (with its service-mode correction in the body), A6, A7 and B4 stand as written.
- A2 stands as a loader gap, scoped by correction 4.
- B2 stands only with the veto key on, which the ledger says the DOY 144 and 145 user replays used.
- Decisions 1 to 10 all stand. Decision 4 guards against a departure from the adopted package; decision 8 matters only when the veto key is on.

### Left unconfirmed

- The settled-set rule details (`RingFixSeams.cpp:1555-1608`) beyond the AND of cascade families; the adopted rule L with a wait of 10 emissions is not mentioned in section 2.1.
- The per-signal fill's family rule (writer 2).
- The rows of sections 1.1, 1.2, 1.6 and 1.8 (E, G, I, R, H, N, slant, orbit).
- The arming of the age rules by profile; the statements taken from `network_wire.tex` and `rt_service_operations.md`; the RTCM decoder range; the B5 items; every test cited as "not traced".
- The opt-in phase-bias drop rule `processing.ssr_drop_unrecorded_phase_bias` (default false, M `src/core/config/GnssConfigLoad.cpp:665-666`) is not mentioned beside "absent: 0" in section 1.5.

## 2026-11-21: the real-file replay tests and the stream fixes

Filed on 2026-11-21 from the package `stream-real-file-replay-fixes` of 2026-11-19 in [part 2](CSC_correction_stream_contract_2026-11-18_part2.md).

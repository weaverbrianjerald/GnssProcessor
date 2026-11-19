# SDM: which slip detector watches which signal pair (2026-11-18)

**STATUS**

- **id:** SDM
- **topic:** slip
- **status:** open
- **summary:** A map of the slip detectors by receiver tracking type and signal pair, at the network and at the real-time user: how each pair is chosen, what each flag frees, the blind spots with the record's cases, and every way a held integer pair can go unwatched or unacted (E1 to E7 at the network, U1 to U5 at the user). At the network no gap, MW or IF-TD verdict on the held pair is an input of the ring fix's release, and LLI bits other than bit 0 blind the network MW. A held-pair row read directly by the release, behind one default-off key, is proposed; the check against the code found five statements wrong and three unconfirmed (last section), none of which overturns the central finding.
- **source:** plan package 7 (which slip detector watches which signal pair, network and user); read only, nothing changed; network code and record read at 7fb0737 (frozen checkout D:/GnssProcessorSupport/worktrees/read_7fb0737, branch chain/audit), user-engine code at main 5ddc422 (C:/GnssProcessor). Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** At the network, "a held pair is always watched" is false, and a watched pair can be judged and still kept. The only slip input of the ring fix's release is the free-epoch latch, written when the slip layer finds a carry to protect and matched on exact signal strings; the release also fires on a missing, created or freed carry, the monitor, a root change and an absence beyond 900 s, but never reads a gap, MW or IF-TD verdict on the held pair. At the user the statement nearly holds; the blind case is a slip equal on both bands. Proposed: one held-pair row read directly by the release, behind one default-off key.

Read at worktree commit 7fb0737 (network) and main commit 5ddc422 (user engine). Tree tags in citations:
**W** = the worktree, **M** = the main checkout. The files `src/core/slip/MwSlipDetector*.cpp` and
`src/core/slip/IfPhaseSlipDetector.cpp` are byte-identical in the two trees, so an M citation of them holds
for W. Every claim below was confirmed at the cited line by a first-stage reader and re-checked by an
independent skeptical reader; where the two disagreed, the disagreement is stated. "(inferred)" marks what
no single line shows.

## 1. Two pair selectors and one held pair, never exchanged

The network has three independent choices of "the pair" for one (station, satellite):

| Choice | Owner | Rule | Citation |
|---|---|---|---|
| Held pair | ring fix, `network.ring_fix.pairs` | per system, from the posture: G L1C/L2W, E L1C/L5Q with the phase-family table; without the table E L1X/L5X is held as well | W src/core/config/GnssConfigNetworkPosture.cpp:410-415, :424-425 |
| Tracker pair (gap test, LLI corroboration, GF arc jump, CS-GF reset) | `RawObservables` | a phase enters only with its same-name code, else the family member's code; the two lowest distinct band digits; within a band the latched (Modelled) signal when `tracker_modeled_first`, else the lexically lowest | W src/core/network/slip/RawObservables.cpp:99-128, :148-169 |
| MW pairs | `MwSlipDetector` | from `obs.observed` (mirror mode): one phase per band, the filter-chosen signal winning; the same-channel code, else the band's first code | M src/core/slip/MwSlipDetectorPairs.cpp:124-239, :280-290 |
| IF-TD pairs | `IfPhaseSlipDetector` | phases of `obs.observed` with a modelled value, no code; with `if_phase_td_pairs = all`, one chain per distinct band pair | M src/core/slip/IfPhaseSlipDetector.cpp:144-218 |

`tracker_modeled_first` is armed in the reference configuration (W config/golden/golden_field_refound.json:253).
It decides only which signal of each band the tracker carries; the bands stay the two lowest digits with a
code (W RawObservables.cpp:129-131, read at this commit).

No line passes the held set to either selector. The release step reads, per held link: presence of both
chords, the chain root, the latch on `sig1` or `sig2` by exact string, the two carries and their re-level
stamps (W src/core/network/ar/RingFixSeams.cpp:884-906; latch at :898-899, read at this commit). The release
rule takes chain, latch, refloat (present and re-levelled, not rebasing), monitor, then absence beyond
`max_gap_s` (W src/core/network/ar/RingFix.cpp:583-625, order :600-606, absence :611-613; `max_gap_s` 900 s at
W config/golden/golden_field_refound.json:365).

Disagreement, settled: the first-stage statement "no path takes a slip verdict on the held pair as input"
is overstated. The keep-carry and the cut-entry checks do read resumption verdicts on the held pair
(W RingFixSeams.cpp:78-85, :723-739). The release step does not. The finding holds for release.

## 2. The network matrix

### 2.1 Detector by detector

| Detector | Observable | Pair | Gate (key) | Threshold | Frees | Event names the signals |
|---|---|---|---|---|---|---|
| LLI drain | RINEX LLI bit 0 on any phase | none: each flagged phase separately | `network.cycle_slip.mw_lli_corroborate` (armed) | acts on MW or GF corroboration on the tracker pair (k 5, floors 0.5 wide-lane cycles and 0.04 m) or while the station is LLI-credible (EWMA, hysteresis 0.6 / 0.3) | `refloat_signal` per flagged signal, live columns only | no: `lli_slip` detail is "LLI;mw_step\|gf_step" |
| MW | two phases, two codes | MW pairs above; `mw_detect_pairs` default all | `network.cycle_slip.mw_detect` | adaptive MAD, elevation-aware; admission needs integer fraction <= 0.15, or evidence (ratio >= 3, >= 10 arc epochs; `mw_evidence_refloat` armed) | both bands of the pair, less bands kept by the common-band scope or the re-flag skip | yes, as "sig1;sig2" (also when a band was kept, inferred) |
| IF-TD | two phases, no code | every distinct band pair (`pairs = all`) | `network.cycle_slip.if_phase_td` | max(k_mad 8 x s_eff x elevation map, 1.0 x lambda_IF), min pool 30; reference-satellite gate ref_k 6 armed | [corrected 2026-11-19: see Corrections, item 2] `refloat_pair`: both bands, less common-band refusals | yes: `if_slip`, `refsat_reset` |
| Gap re-entry, row tier | dMW or dGF across an absence of at least one epoch; any nonzero LLI on the tracker band | tracker pair | `gap_reentry_relevel` (armed) | qualify gates | tracker pair's two signals plus extra-channel cells of those bands, only when a carry exists | yes: `reentry_relevel` per arc |
| Gap re-entry, per-signal tier | GF level against a partner that tracked through; any nonzero LLI on that signal | every phase with a code | `gap_reentry_per_signal` (armed) | max(5 s_gf, 0.04 m); unmeasurable counts as a return (default true) | that one signal | yes: `sig_resume` |
| GF arc jump | GF on the tracker pair | tracker pair | `network.cycle_slip.gf_arc_jump` (off) | | tracker pair | |
| TurboEdit | MW + GF integer repair | | `turbo_edit` (off at the network) | | | |
| RAIM / gross LOS / wind-up restart | post-fit residuals | none | armed | raim_k | every carried ambiguity of the satellite at the station, with its slant iono and phase-colour carries | no: `attrib_arc` / `attrib_sat` name station and satellite |

Citations. LLI drain: W src/core/network/slip/SlipQc.cpp:789-922 (bit 0 at :800, :884; per signal :883-893;
live columns :370-375; event :910-917, suppressed :856-863); corroboration defaults
W src/core/config/GnssConfigLoadNetwork.cpp:6319-6345, W RawObservables.cpp:411-421, credibility :444-473.
MW: admission W src/core/network/slip/PassASlipScan.cpp:181-270; response W SlipQc.cpp:659-788 (bands
:718-732, event :775-781); threshold M src/core/slip/MwSlipDetectorSample.cpp:654-657; gates
W GnssConfigLoadNetwork.cpp:5989-5990, :6003-6017, :6122-6127. IF-TD: W SlipQc.cpp:1016-1803 (threshold
:1068-1092, response :1127-1199, events :1525-1532, :1759-1766); defaults W GnssConfigLoadNetwork.cpp:6029-6037.
Gap tiers: W RawObservables.cpp:475-611; row response W SlipQc.cpp:2215-2329, :2700-2758; per-signal
W SlipQc.cpp:3047-3064, W GnssConfigLoadNetwork.cpp:6364. GF arc jump: W src/core/network/slip/GfArcJump.cpp:141-184,
key W GnssConfigLoadNetwork.cpp:6383-6388. TurboEdit gate: W src/core/network/driver/ApplicationNetworkSetup.cpp:1199-1202.
RAIM: W src/core/network/qc/Raim.cpp:915-939, :1155-1161; gross LOS W src/core/network/qc/GrossLos.cpp:986;
wind-up W SlipQc.cpp:1805-1815. `GapResumeCensus` is instrumentation only
(W include/network/slip/GapResumeCensus.hpp:5-12). The user-side GF detector is not compiled.

Every free lands in `relevel->free_ambs` with a cause bit (Mw, Lli, IfTd, Gap, GfJump, Windup) and feeds the
slip funnel (W include/network/slip/SlipFunnel.hpp:13-20). The common-band scope narrows the MW, LLI and
IF-TD frees to the implicated bands of one verdict per (station, satellite, epoch)
(W include/network/slip/CommonBandScope.hpp:21-60). On a dual-frequency posture there is one MW pair per
satellite, so the scope has nothing to narrow there (inferred, unconfirmed).

### 2.2 Three readings of the LLI byte

| Reading | Readers | Citation |
|---|---|---|
| bit 0 only | LLI drain, credibility fraction, common-band evidence | W SlipQc.cpp:800, :884; W RawObservables.cpp:36-40; W PassASlipScan.cpp:315-318 |
| `estimation.cycle_slip.lli_slip_mask`, default 7 (any bit) | the MW detector the network instantiates | W src/core/config/GnssConfigLoad.cpp:1326-1327; M MwSlipDetectorPairs.cpp:90-95 |
| any nonzero value | both gap tiers | W SlipQc.cpp:2457, :2656, :3047 |

No configuration or script on this branch sets `lli_slip_mask` (only tests do). On the uncombined path, an
epoch with any LLI on any phase withholds every MW sample of the satellite (M src/core/slip/MwSlipDetectorEpoch.cpp:90,
:109-111, read at this commit), and the network discards the detector's pair-less LLI results
(W PassASlipScan.cpp:245). The record's sentence that the network's slip scan reads bit 0 only (NRF section
87, W docs/refound/NRF_network_ring_fixing_2026-11-14_part10.md:102) is true of the drain only. At a
receiver that sets bit 2 on every Galileo L1C phase (the record names HKSL, REYK, WTZR, WROC), the network's
MW therefore has no Galileo sample at any epoch (inferred from the code; no run read). An epoch with only bit
1 or bit 2 set blinds MW for that satellite while the drain does nothing.

### 2.3 By receiver tracking type

[corrected 2026-11-19: see Corrections, item 3] Assumed posture: dual frequency, single channel, phase families E:L1X>L1C and E:L5X>L5Q with codes keeping
their names (CHA part 3 line 100). The intake renames a member's phase, Doppler and signal-strength codes
unless the file also declares the identity (W src/core/rinex/RinexParser.cpp:379-394;
W ApplicationNetworkSetup.cpp:245-262).

| Receiver | Held pair | LLI drain | MW | IF-TD | Tracker (row gap, corroboration) | Per-signal gap tier |
|---|---|---|---|---|---|---|
| Galileo pilot (L1C C1C, L5Q C5Q) | L1C/L5Q | bit 0 on L1C or L5Q frees that signal | L1C+C1C / L5Q+C5Q | L1C/L5Q | L1C/L5Q | L1C, L5Q, and E5b/E6 phases with codes (unheld end as no carry) |
| Galileo combined (L1X C1X, L5X C5X), families on | L1C/L5Q (renamed phases) | reaches the held arc (flags stored under the renamed code, inferred) | [corrected 2026-11-19: see Corrections, item 3] L1C+C1X / L5Q+C5X via code fallback | L1C/L5Q | [corrected 2026-11-19: see Corrections, item 3] L1C+C1X via family pairing (:104-111); agrees with MW | L1C, L5Q |
| Galileo combined, families off | L1X/L5X (and L1C/L5Q held if tracked) | native | L1X/L5X | L1X/L5X | L1X/L5X | native |
| Galileo, file declares both families | L1C/L5Q (priority list, inferred) | LLI on L1X/L5X frees nothing, emits nothing, yet withholds the L1C/L5Q MW sample (mask 7) | L1C/L5Q | L1C/L5Q | L1C/L5Q | sees L1X/L5X returns, finds no carry |
| GPS, L1C and L2W tracked | L1C/L2W | bit 0 on L1C or L2W frees; LLI on L1W or L5Q frees nothing and blinds MW | follows the filter's band-1 signal (C1W excluded as second code companion, M MwSlipDetectorPairs.cpp:173-177) | L1C/L2W | L1C/L2W | each phase with a code |
| GPS, L1L/L2L only | none of L1C/L2W | | | | [corrected 2026-11-19: see Corrections, item 4] lexical L1L/L2L; carried arcs get `gap_reentry_unreachable` unless `network.n3c_band_signal` drops the band | |
| BeiDou lineage (B1I/B3I, bands 2 and 6) | B1I/B3I | not examined | | | two lowest digits need not be 2 and 6 | |

Open point on GPS (raised by the skeptic, not settled). The reference configuration lists L1W ahead of L1C
in the GPS priorities (W config/golden/golden_field_refound.json:53-54, read at this commit). If the filter
carries L1W, mirror-mode MW watches L1W/L2W, not the held L1C/L2W. The record says the integer-fixing
configuration moved GPS to L1C; that configuration file was not found under config/ and was not read.

## 3. The user matrix

Tree M throughout.

The user that fixes integers on the network's corrections is the real-time engine, also when replayed
(src/core/app/ApplicationConsumerReplay.cpp:365, :563). It builds no `CycleSlipManager`
(src/core/realtime/RealTimeEngineInit.cpp:297). GRAPHIC, EWL and the ionospheric variability monitor exist
only on the post-processing path; the IF-phase detector is network-only; the GF detector is not compiled.

| Detector | Path | Default | Observable | Pair | Threshold | Flag frees |
|---|---|---|---|---|---|---|
| MW | real time and post-processing | always built in real time; the `mw` toggle is not read there | MW from raw_observed | mirror: the filter's bands, one signal per band (filter's signal wins), every band pair, descending frequency; same-channel code else band's first code | 8 x max(sigma_MAD, 1 cm) mature; warm-up 0.4 cycle plus cross-satellite MAD cascade | non-LLI: both keys of the lane; LLI (mask 7): every phase key of the satellite, all bands, MW arc kept |
| TurboEdit | real time when armed; post-processing only with MW | off; armed in three real-time configs | GF TD2 + MW integer | every cross-band pair of raw_observed phases, every channel | GF-only kσ 6; split 0.2 cycle; IVI 0.06 m; cooldown 3 | additive repair x += dN lambda, or reset; code-event veto keeps phase |
| Soft-LLI bridge | real time | off (needs TurboEdit) | | satellite | gap <= 15 s | snapshot + dN lambda, or reset |
| GRAPHIC | post-processing | off | (P + lambda L)/2 per signal | first code and phase per band, lexical (L2L before L2W) | kσ 6 | that key, skipped if MW acted |
| EWL | post-processing | off | | first signals of bands 2 to 9, every pair (BeiDou B1I/B3I forms an "EWL", inferred) | kσ 5 | both keys |
| AR hold-release test | real time, `hold_release_cyc > 0` | 0.25 in the user integer recipe | float DD against held WL and IF integers | the held DD pair itself | hold_release_cyc | releases the held pair |
| Residual screens | real time | on | innovations, post-fit | key | sub-cycle floor 0.25 lambda (default on) | reset key and detector arcs |

Citations. MW build and call: src/core/realtime/RealTimeEngineInit.cpp:209, src/core/realtime/RealTimeEngineSlipQc.cpp:34;
toggle read only at src/core/slip/SlipDetectorRegistry.cpp:73 and a log line (CycleSlipManager.cpp:73).
Pairs: src/core/slip/MwSlipDetectorPairs.cpp:98-307, modes src/core/config/GnssConfigLoad.cpp:423-465.
Thresholds: include/config/GnssConfigTypes.hpp:5073, include/slip/MwArcState.hpp:93, include/slip/MwSlipDetector.hpp:579-641.
Flags: MwSlipDetectorEpoch.cpp:155-238, :868-869; RealTimeEngineSlipQc.cpp:224-277. TurboEdit:
RealTimeEngineInit.cpp:214-238, TurboEditResolver.cpp:119-147, include/slip/TurboEditResolver.hpp:650-668,
RealTimeEngineSlipQc.cpp:89-218; armed in config/realtime_ppp_rtk.json:137, config/realtime_igs_ntrip.json:182,
config/sbf_replay_covl145.json:80. Bridge: RealTimeEngineSlipQc.cpp:113-128, :262-274. GRAPHIC:
GraphicSlipDetector.cpp:92-104, CycleSlipNonMwPasses.cpp:107-153. EWL: EwlSlipDetector.cpp:90-120.
Hold-release: src/core/filter/AmbiguityResolver.cpp:1767-1838. Screens: RealTimeEngineEstimate.cpp:97-130,
RealTimeEngineScreen.cpp:197-212, KalmanPostFitQc.cpp:1277-1297, GnssConfigLoad.cpp:1625-1626, :1679-1681.

What watches a held pair (sat_i, sat_ref, sig_hi, sig_lo) at the real-time user, default modes:
1. MW lanes `sat_i_sig_hi_sig_lo` and `sat_ref_sig_hi_sig_lo`, one per satellite, not on the DD (the held
   signals are the filter's band signals because AR reads the state keys, inferred);
2. with extra signals, the lanes to the third band;
3. TurboEdit GF TD2 on the same signals per satellite, if armed;
4. the DD hold-release test, when `hold_release_cyc > 0`.

The hold record (`HeldPairFix`, AmbiguityResolver.hpp:155-168) is kept only when `hold_release_cyc > 0`
(AmbiguityResolver.cpp:510-513) and is forgotten when any ambiguity of either satellite begins or ends
(:283-289). In AR mode 1 the fix is a separate solution; only mode 3 writes the hold into the filter
(:2328-2329, :3506).

A corrections discontinuity on one signal of a held pair (order: slip detection, generation change, phase-bias
discontinuity; RealTimeEngine.cpp:537-545) resets that one signal (RealTimeEngineSsrBreaks.cpp:303-311) or
shifts it under `ssr_phase_bias_disc_carry` (:385-475). The partner and the MW and TurboEdit arcs are not
touched. The bumped generation erases the hold memory of every pair with that satellite; with
`refloat_partner_signal` every signal of the satellite is refloated (AmbiguityResolver.cpp:236-289, :515-518).
A carried value-only shift then also refloats the partner, so the carry's kept covariance is lost for the
satellite (inferred; not seen on data).

Departures from the project's slip rules at the user:
- TurboEdit is not rate-gated: `kHighRateMaxIntervalS` 1.0 is advisory (TurboEditResolver.hpp:310-315,
  :665-668), and the real-time engine takes the rate from `processing.gnss_rate_s` else 1 s
  (RealTimeEngineInit.cpp:224); nothing refuses a 30 s stream.
- "Reset, not repair" does not hold when TurboEdit or the bridge is armed (RealTimeEngineSlipQc.cpp:113-151),
  nor for the post-processing culprit repair (CycleSlipAdjudication.cpp:380-431, gate only a consistent
  integer at :36-60).
- Sub-cycle suppression is on by default (`enable_subcycle_slip_suppression`, GnssConfigLoad.cpp:1679-1681):
  a phase outlier below 0.25 lambda keeps its ambiguity.
- The real-time MW detector ignores its toggle (an absence finding: no read under src/core/realtime).

## 4. Blind spots with the record's cases

| Case | Record | What should have fired | Why it did not | State now |
|---|---|---|---|---|
| 1. Phases renamed without codes | NRF 107.6, 107.9: ULAB00MNG E14 E1/E5a, dGF -1.22 m at 08:09:30 | gap test, MW, IF-TD on E1/E5a | renamed L1C/L5Q had no C1C/C5Q, left the tracker (W RawObservables.cpp:99-114); at 28 receivers the row became L6X/L7X, L7X/L8X or one band; only E5b/E5 arcs freed, no latch on L1C/L5Q, hold kept; held-first re-attachment took the stale link as a known step | repaired for renames inside the family table only (:104-111, NRF 112, 118); the entry check fails safe for entries (W RingFixSeams.cpp:723-739), never for release |
| 2. MW flags the configuration does not act on | NRF 110.1: band-scope rows "mw(L1X+L5X) flag; iftd silent"; 302 such flags held through on DOY 147 | MW on the held pair | a flag failing the rounding gate is dropped (W PassASlipScan.cpp:244-268) but still feeds common-band evidence (CommonBandScope.cpp:107-110, :554-568); no drain, no latch, hold kept; under `primary` a non-pin pair is not formed (:205-232) | open; whether any of the 302 was a real slip is unknown; the count was not re-verified |
| 3. Dropped `lli_slip` flags in the grading | NRF 118.1, 118.4: 159 to 259 a day dropped by the grading parse | engine acted: `refloat_signal` latches (W SlipQc.cpp:365-424, latch :407) | the engine path is correct only when the carry is warm and the re-level state exists (:402-410); none was held through | engine-side exceptions remain: suppression at a non-credible station (:838, :849-865), off-pair LLI uncorroborated (:812-829), pruned column unmatched (:370-375), cold carry no latch (:402) |
| 4. Held through a resumed arc | five-day read F5: BOGT00COL G09 DOY 143, L2W lost at TOW 572250, absent 16 epochs, resumed `T0_gap arc_resume shifted_fresh` on L1C and L2W (dGF -1.02 m, pl=0), held 4 epochs, monitor released at 572850 (D:/GnssProcessorSupport/gates/nrf_reads_2026-11-18/NRF_five_days.md:107-114) | gap test fired (shifted); the response did not | the response requires `pl_bridged` or `carry_invariant` (W SlipQc.cpp:2242-2248) and returns with no latch otherwise (:2258-2302); the release does not read resumptions, only clears its absence timer on return (W RingFix.cpp:606) and releases on absence beyond 900 s (:611-613); in `d24_n143` the carry was found and the latch released | open; a breach of invariant 7 at the re-coupling edge. Why pl=0 inside the 900 s horizon is not settled: one path consistent with the code is a cut that set `rebase_stamps` (RingFix.cpp:930-933, :594-598), marked inferred by both readers |

## 5. Every way a held pair can go unwatched or unacted (network)

The skeptic's exception list (E1 to E7) is kept as the frame; the first-stage enumeration is folded into it.

**Not watched**
- **E1. A held phase with no same-name code and no family entry** leaves the tracker (W RawObservables.cpp:106-113): no row gap test, no LLI corroboration, no GF arc jump. Case 1. Open for any rename or channel outside the table.
- **E2. A held band that is not one of the two lowest digits with a code.** A missing E1 or E5a code moves the Galileo row to E5a/E6, E1/E6 or E1/E5b (:119-128, inferred). After a band loss the row moves to the next band, so detections and latches in the outage name other signals (inferred).
- **E3. Without the family table two Galileo pairs are held on the same bands** (GnssConfigNetworkPosture.cpp:411). MW and the tracker keep one signal per band (M MwSlipDetectorPairs.cpp:232-238); the second held pair is watched only by the bit-0 drain and the per-signal gap tier. IF-TD per band pair is inferred to watch one of the two (not traced). Closed in the adopted single Galileo network (NRF 123); open whenever the table is off.
- **E4. MW blinded by any bit-1 or bit-2 LLI on any phase of the satellite**, unheld phases included (section 2.2). Open.
- Within a band, with `tracker_modeled_first` off or a latched signal not reported this epoch, the tracker takes the lexical pick (L2L or L2S before L2W) (:137-140). The knob is armed in the reference configuration; its state in the adopted integer-fixing configuration was not read.
- The MW pair set comes from `obs.observed`; readmission can withhold a returning arc's phase from it (W RawObservables.cpp:58-68); IF-TD under `first` watches only the first two bands (W SlipQc.cpp:1094-1103); MW under `primary` drops non-pin pairs.
- The GPS band-1 signal in `obs.observed` may be L1W (section 2.3, open).

**Watched but not acted on**
- [corrected 2026-11-19: see Corrections, item 5] **E5.** Dropped MW flags (PassASlipScan.cpp:258-262); suppressed LLI (SlipQc.cpp:849-865); a cold carry or a missing carry entry (:402, :2258-2302); a pruned column (:370-375); the datum station path (`datum_slip_detect`, :672-678, :869-877); MW rounding gate and re-flag skip decide on the detector's integer, not the held one (:720-732). In each case no latch, so the hold stays (RingFixSeams.cpp:898-899; RingFix.cpp:603).
- The common-band scope can exonerate both held bands through silent pairs that include a third band (W SlipQc.cpp:718-732; CommonBandScope.cpp:120-209); the hold is then as right as that evidence.

**Not a release input**
- **E6. A return after an absence.** The release reads latch, stamps, monitor and absence beyond 900 s only (RingFix.cpp:600-613). The entry check's gap evidence is used at cuts and for keep-carry, never at release, and matches a resumption only when its pair equals the held pair (RingFixSeams.cpp:78-85, :725-731).
- **E7. A latch on a sibling channel** of a held band does not release (exact strings, :898-899). The reverse (tracker on L2L, carry on L2W) is counted `gap_reentry_unreachable` and not acted on (W SlipQc.cpp:2285-2300).

**Identity at the edges (invariant 7).** `mw_wl_slip`, `if_slip`, `refsat_reset`, `reentry_relevel`, `sig_resume`
and `arc_reentry` name the freed signals. `lli_slip` frees per signal but names none; RAIM `attrib_arc` /
`attrib_sat` free the whole line of sight and name no signal.

## 6. User exceptions

- **U1.** A slip equal on both bands (dN1 = dN2) is invisible to MW; covered only by TurboEdit (off by default) and the residual screens, with the sub-cycle floor keeping residuals below 0.25 lambda.
- **U2.** An LLI epoch withholds the MW sample but resets every phase key of the satellite: no slip passes, at the cost of over-resetting at bit-2 receivers. Not a blind spot.
- **U3.** A phase-bias discontinuity on one signal resets that signal alone; the partner is refloated only under `refloat_partner_signal`. A corrections event, not a slip-watch gap.
- **U4.** Unconfirmed: whether an SSR stream ever feeds satellite biases to the user's MW. No real-time writer into `GnssCorrections` was found, so MW lanes run undifferenced on a stream (inferred).
- **U5.** The hold-release test exists only with `hold_release_cyc > 0`; with 0 nothing is keyed on the held DD pair.

## 7. The smallest structural change that makes a held pair always watched

Both first-stage network readers and the skeptic agree on the shape. One change set, one key (for example
`network.ring_fix.held_pair_watch`), default off, identical output when off:

1. **One owner of the held set as a slip input.** At the start of each epoch the ring fix publishes its held
   set (station, satellite, `SignalId` sig1, `SignalId` sig2). The raw-observable pass forms one extra row on
   exactly each held pair, the code taken from the roster (same name, else the family member's), beside the
   tracker's own pair. The gap test, MW, GF step and LLI read on that row. This closes E1, E2, E3 and the
   within-band and GPS band-1 cases at once, because the watched pair is the held pair by construction.
2. **The release reads that row's verdicts directly**, not through the carry latch: gap resumed (measured,
   shifted), MW step tested against the held wide lane (n1 - n2) rather than the detector's rounding, GF and
   IF-TD flags, and any LLI bit on either held band whatever the station's credibility. Suppression and
   rounding keep governing the carry refloat, not the hold. Closes E5 for held links. A release does not
   refloat the carry: it costs one re-decision.
3. **A return after any absence is an event judged by `chainEntryStep` at release**: kept only when the
   held-pair row measured the gap and found no shift, otherwise released with an event. One owner for "a held
   integer across a gap" at the cut, at keep-carry and at release. Closes E6.
4. **Latch matching by band through the typed identity** (`SignalId` from include/common/GnssIdentity.hpp),
   not by string. Closes E7.
5. **`held_unwatched` counter, release and event**: a present held link with no row on its own pair is
   released and named. A held link is then watched or not held.
6. Two telemetry and detector points from the network-detectors reader, endorsed by the skeptic, each its own
   default-off key: the network MW detector reads the LLI byte as bit 0 (closes E4); the `lli_slip` event names
   the freed signals.

At the user no structural change is proposed: the held signals are already MW lanes in mirror mode. U1 is a
detector-coverage question (a compiled GF test on the held pair, or TurboEdit's rate gate made real), not a
pair-choice one.

Runs this would need later (none requested now): the two-mode slip bed (NRF 112's `f40_s_on` and `m1s_c`)
with injected slips also on GPS L2W after a band loss; a band-loss bed (a held band blanked 10 to 20 epochs,
then stepped); the five-day set graded with the F5 rule and `lli_slip` flags counted; 2 h identity controls
with the key off; a Leica bit-2 station pair to size E4 before and after the bit-0 key.

## Decisions for the author

1. Adopt the held-pair row and a release that reads its verdicts (section 7, items 1 to 5) as one change
   behind one default-off key, or keep the latch as the only release input.
2. On a held link, test an MW step against the held wide lane instead of the detector's rounding gate, so
   that a flag the carry ignores can still release the hold.
3. Release a hold on any LLI bit on a held band regardless of station credibility, or keep the credibility
   suppression for holds as well.
4. Make the network MW detector read the LLI byte as bit 0 (a network-only key), to match the drain and the
   record's statement.
5. Name the freed signals in `lli_slip` and decide whether RAIM attribution events should list the freed arcs.
6. At the user: make the real-time MW toggle effective or remove it; gate TurboEdit on the stream rate; decide
   whether sub-cycle suppression stays on by default against the slip rule.
7. Whether a carried (value-only) phase-bias shift should exempt the partner from `refloat_partner_signal`.

## Not checked

- Why BOGT00COL G09 had pl=0 inside the 900 s horizon on DOY 143 (needs the run's ambiguity-state and
  ring-fix traces; run folders were out of bounds).
- The GPS band-1 signal under the integer-fixing configuration (L1C per the record) against the reference
  list (L1W first); that configuration file was not read.
- The state of `tracker_modeled_first`, `network.ledger_reentry` and `gap_reentry_coresumed_share` in the
  adopted integer-fixing configuration.
- That LLI flags of a renamed column carry the identity name (RinexParser.cpp:840 with :379-394, inferred).
- The size of the bit-2 MW blindness at the four Leica stations (no run read).
- Whether any of the 302 MW flags of NRF 110.1 was a real slip; the 302 and the 159 to 259 counts were not
  re-verified.
- The common-band scope having nothing to narrow on a dual posture (inferred).
- IF-TD coverage of the second held Galileo pair with the family table off.
- The BeiDou lineage: whether the tracker's two lowest digits match the held B1I/B3I.
- At the user: whether SSR biases reach `GnssCorrections`; whether any real-time run sets
  `gnss_rate_s` above 1 s with TurboEdit armed; the user's lanes under the single Galileo network for a
  receiver tracking E1X; which `wl_from_float` path the integer-fixing users take.

## 2026-11-19: corrections from the check against the code

The check read 46 claims: 38 confirmed, 5 wrong, 3 unconfirmed. Trees: W = frozen checkout D:/GnssProcessorSupport/worktrees/read_7fb0737 (7fb0737), network code and record; M = C:/GnssProcessor (5ddc422), user-engine code. The MwSlipDetector*.cpp and IfPhaseSlipDetector.cpp files are byte-identical in the two trees, as the body states. Items 1 to 5 are the statements ruled wrong; items 6 to 9 are smaller corrections.

1. **Outcome of the draft: a hold is released "only through a free-epoch latch".** Overstated. A present link is also released when either carry is missing, created or freed (re-level stamp set to now), and by the monitor, a chain root change and an absence beyond `max_gap_s` (W src/core/network/ar/RingFix.cpp:601-606; W include/network/driver/RunTypes.hpp:905-907). The latch is the only slip input of the release; the release never reads a gap, MW or IF-TD verdict on the held pair. The Outcome above is rewritten accordingly.
2. **Section 2.1, IF-TD row: frees both bands less common-band refusals.** Incomplete. An IF-TD flag matching a satellite clock step is kept with no refloat (`sat_clock_keep`, armed at W config/golden/golden_field_refound.json:255); on the reference-satellite path `refsat_clock_keep` and `refsat_centre_keep` keep likewise (W src/core/network/slip/SlipQc.cpp:1487-1499, :1510-1520). The same keep is a further item of E5 (watched but not acted on).
3. **Section 2.3 premise: family codes keep their names.** Wrong premise. The ring-fix posture prescribes `network.phase_family_codes`, which renames the codes too (W src/core/config/GnssConfigNetworkPosture.cpp:428-432; W src/core/network/driver/ApplicationNetworkSetup.cpp:259-261). Under it the Galileo combined row (families on) pairs L1C+C1C and L5Q+C5Q, not C1X / C5X, in the MW and tracker columns.
4. **Section 2.3, GPS L1L/L2L-only row: carried arcs get `gap_reentry_unreachable`.** Wrong. No L1C/L2W carry exists at such a receiver, and a carried signal not tracked this epoch is skipped, not counted unreachable; the code comment names this very receiver (W SlipQc.cpp:2269-2277, :2287; present set :2696-2699).
5. **Section 5, E5: a missing carry entry leaves no latch, so the hold stays.** Wrong. With the link present, a missing carry gives `carry_ok` false and a refloat release unless `rebase_stamps` is pending (W RingFix.cpp:601, :604). E5 keeps the cold carry; "or a missing carry entry" does not belong to it.
6. **Section 2.3, GPS open point (reference configuration lists L1W before L1C).** True of W golden_field_refound.json:53-54, but the ring-fix posture check requires C1C/L1C before C1W/L1W (W GnssConfigNetworkPosture.cpp:283-291, :433-437). The point is settled for the adopted posture: the reference configuration's L1W-first list is out of posture for integer fixing. The same applies to the second item of "Not checked".
7. **Section 1, `max_gap_s` 900 s at golden_field_refound.json:365.** True in substance: line 365 of the reference configuration is `phase_leveling_max_gap_s`, copied into the ring fix as `max_gap_s` (W src/core/network/ar/RingFixSeams.cpp:407; W include/network/ar/RingFix.hpp:794).
8. **Section 1, held-pair row.** The loader default for `network.ring_fix.pairs` is an empty list, meaning every family (W src/core/config/GnssConfigLoadNetwork.cpp:1536-1559); the pairs in the table are the posture readout.
9. **Source of the draft.** The draft names the worktree D:/GnssProcessorSupport/worktrees/chain; that worktree is being edited, and the read applies to the frozen checkout D:/GnssProcessorSupport/worktrees/read_7fb0737 (commit 7fb0737, detached). The W tag of the body means that checkout.

### Line numbers that moved

The substance holds at each:
- W PassASlipScan.cpp:245 (pair-less MW results discarded) is now :246.
- M SlipDetectorRegistry.cpp:73 (MW toggle read) is :72.
- The IF-TD threshold text is at W SlipQc.cpp:1050-1078 and the flag branch at :1500-1535.
- The cause bits of `relevel->free_ambs` are in W include/network/driver/RunTypes.hpp:991-998, not SlipFunnel.hpp:13-20.
- M config/sbf_replay_covl145.json:80 (TurboEdit armed) is the uncommitted working copy; the line is 79 at 5ddc422.

### What stands

- The central finding: no gap, MW or IF-TD verdict on the held pair is a release input; a resumption across a gap is read only at cuts and at keep-carry; LLI on a bit other than 0 blinds the network MW (from the code; the intake stores the raw LLI digit, W src/core/rinex/RinexParser.cpp:839-840; no run read).
- E5 loses its "missing carry entry" item and gains the satellite-clock keep of IF-TD.
- Case 4: the pl=0 path stays inferred; the code supports it, since a missing or newly created carry would have released the link as a refloat unless `rebase_stamps` was pending.
- The section 7 proposal stays sound; its claim to close E3 rests on a case not shown to exist.
- Decisions 1, 2, 3, 5, 6 and 7 stand as framed.
- Decision 4 (and section 7, item 6) stands, but may need only a configuration value: the existing `estimation.cycle_slip.lli_slip_mask` (1 or 3) already makes the network MW read bit 0 per run configuration (W src/core/config/GnssConfigLoad.cpp:1324-1327); a separate network-only key is needed only to keep a user engine on 7 under one configuration.

### Left unconfirmed

- E3: that a second held Galileo pair at one station is watched only by the drain and the per-signal tier. Doubtful: a held link needs chords on both signals, that is modelled ambiguities, and the single-channel network models one phase per band (canonical latch); no station carrying both pairs is shown (W RingFixSeams.cpp:895-896; W PassASlipScan.cpp:735-747).
- Decision 4: the need for a new network-only key (see What stands).
- U1: "the sub-cycle floor keeping residuals below 0.25 lambda" is overstated: a whole-cycle slip on an uncombined row is 1 lambda, above the floor (M GnssConfigLoad.cpp:1625-1626); how much the ionosphere and clock states absorb was not settled.
- Not read by the check: the record cases' counts (302 MW flags, 159 to 259 dropped events) and the ULAB00MNG E14 details; the CommonBandScope.cpp citations; the GapResumeCensus header; the RinexParser renaming of LLI keys; the GRAPHIC and EWL pair rules; the AR mode 1 against mode 3 lines; the SSR bias path to `GnssCorrections` (U4); the RealTimeEngineSsrBreaks.cpp lines; the BeiDou rows; the gap tiers' qualify gates.

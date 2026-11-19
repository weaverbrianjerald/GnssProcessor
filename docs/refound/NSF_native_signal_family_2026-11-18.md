# NSF: a native signal family with a modelled member bias (2026-11-18)

**STATUS**

- **id:** NSF
- **topic:** network-vehicle
- **status:** open
- **summary:** A design memo, with nothing built, for the native build of the Galileo phase family (members keep their own signal names, one satellite phase bias and one satellite code bias per band) and, behind its own key, a modelled code term per (satellite, receiver make). The term is proposed as one ionosphere-free code column per (satellite, make), Trimble first, separable through receivers that carry no member column (the reference group); a toy rank computation finds no free direction with one zero-sum per class while a reference receiver sees each satellite, and the rank check on the real datum rows is owed. Order, scope, datum form, noise and the user side are left to the author.
- **source:** plan package 10 (a design memo: the native signal family and the member code term); network code read at commit 7fb0737 of branch `chain/audit` (the draft read 03dfcbc; the cited files are unchanged at 7fb0737 except RingFix, the emitter and the snapshot fingerprint, whose cited lines still hold), user-engine code and ledger at main 5ddc422; [CHA](CHA_chain_audit_2026-11-18.md) parts 2 and 3; NRF parts 14, 16 and 18 (sections 107.8, 112.5 to 112.7, 123). Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** The adopted single Galileo network renames combined-mode signals at the intake, so the member arcs never exist. The native build keeps them under their own names in one family per band and repeats the identity's records on the stream; it is graded against the adopted form, not diffed. The member bias is proposed to be carried as one ionosphere-free code column per (satellite, receiver make), Trimble first, separable only through receivers without a member column in view of the same satellite. Its datum and noise must not repeat the stiff, wrong column of NRF 112.6, so a column-motion trace is a bar. Order and scope are the author's decisions.

Read at commit 03dfcbc (worktree D:/GnssProcessorSupport/worktrees/chain). Every path:line below is at 03dfcbc unless stated otherwise. This memo merges a first-stage state-of-play, a first-stage design and an independent review of that design; where the review refuted the design, the design is changed here and the disagreement is stated in section 8.

## 1. Where the adopted rename form stands

1. **Adopted.** NRF section 123 adopts `network.phase_families` `["E:L1X>L1C","E:L5X>L5Q"]` with `network.phase_family_codes` true, scoped to the integer-fixing configuration on chain/audit; the main reference configurations do not change and nothing goes to main (NRF part18 lines 18-20). Both keys are RECOMMEND_SCOPED. The run-start posture table has rows for both, marked fingerprinted (src/core/config/GnssConfigNetworkPosture.cpp:423-431), and the pairs row reads `["E:L1C/L5Q","G:L1C/L2W"]` as armed when a table is set (GnssConfigNetworkPosture.cpp:406-415). Section 123's ledger paragraph records these rows as missing; the posture step (CHA part3, commit b98f983) added them afterwards.
2. **The intake renames.** The table is typed once and empty by default (src/core/network/assembly/SignalRoster.cpp:871-899). At every network station except the held-out user, a member's phase, Doppler and signal-strength codes, and under `phase_family_codes` its pseudorange code, are mapped to the identity's names (src/core/network/driver/ApplicationNetworkSetup.cpp:245-282, rename map 255-262). The parser rewrites the header's code list before any data is stored and records each rename (src/core/rinex/RinexParser.cpp:379-401). A file declaring both a member and its identity is refused at run start (ApplicationNetworkSetup.cpp:1449-1460); each station's real tracking is kept for the stations trace as `phase_tracking` (ApplicationNetworkSetup.cpp:1393-1448). A streamed network run with the table is refused (ApplicationNetworkSetup.cpp:250-253): NRF 123 owed item (e), blocking for a real-time network.
3. **Consequence.** The estimator sees one phase signal and, with codes shared, one code signal per band. The combined-mode arcs do not exist under their own names.
4. **The header.** include/network/assembly/PhaseFamily.hpp:1-16 documents the rename form; `phaseFamilyOf` (lines 43-47) and the text bridge `phaseFamilyCodeOf` (lines 57-63).
5. **The slip layer** pairs a family phase with the receiver's own reported code (include/network/slip/RawObservables.hpp:1145-1161). Under shared codes both are renamed, so the pair is still the receiver's own.
6. **The stream.** `networkPhaseFamilyMemberRecords` copies each identity's satellite phase bias, and under shared codes its code bias, to the member's name (src/core/network/emit/EmitStore.cpp:60-79), after every writer of the identity records and before the store push (EmitStore.cpp:474-478); released signals are widened to the members (EmitStore.cpp:81-92, 473). A member's phase record takes the identity's discontinuity counter, signal-integer flag and continuity-guard lowering (src/core/products/NetworkProductEmitter.cpp:976-990); its code record the identity's code counter (NetworkProductEmitter.cpp:844-855). The member map is built at ApplicationNetworkSetup.cpp:1591-1601. A run-end line counts member records (SignalRoster.cpp:2455-2465). NRF 118.1 C2 found 0 unequal identity and member records; the native build must meet the same contract.
7. **Known costs, untraced** (NRF 123, owed (b) and (c)): the users' paired difference exceeds 1 mm in some hour in 6 of 8 groups, means of hourly medians +0.09 to +0.65 mm; three short runs of holds wrong by the float check (UTQI E18-E06, also on today's stream; QUIN G10-G02 and ASCG G20-G23, new). With codes kept apart the record shows the same cost (112.4: means +0.15 and +0.14 mm). Neither cause is traced, so no part of this memo can claim the native build or the member term removes them.

## 2. What the chain cleanup prepared, and what remains

- S4 (5a98185) gave the chain vertex one owner; S5 (e1bdc05) typed the band (`ChainGroupKey`) and the receiver's band cell in the election. Both byte-identical.
- S6 (0783ad1, byte-identical over DOY 147) split arc keys from band keys: `chainEdgeKeys` gives a tree edge its arc key ("E05_L1X") and its band key ("E05_L1C") from one owner (src/core/network/solve/ChainDatum.cpp:386-406); `chainBandSignalOf` maps arc to band through `phaseFamilyCodeOf` (ChainDatum.cpp:408-412), `chainArcSignalsOf` is its inverse (ChainDatum.cpp:414-424); `eq34ShiftCarry` takes the table (include/network/solve/ChainDatum.hpp:109-118). Under the rename the map is the identity on every run (inferred from the rename at parse; S6's gate shows no output change).
- Left by S6: the election's maps and accessors keyed by text at the boundary; the ring fix's settled, untied and certified-edge maps keyed by the band's signal and the family text; `pairConsidered` keyed by (letter string, sig1, sig2) (include/network/ar/RingFix.hpp:859-864); the receiver phase bias of a member receiver not keyed by the band's identity (CHA part2 section 6).

## 3. The parameter chosen and why

### 3.1 Evidence

NRF 107.8 (part14 lines 102-112), DOY 143 to 147, classes by make and mode from the file headers:
- rms over satellites of the five-day constants: Trimble combined 0.026 cycle (consecutive-day correlation +0.71 to +0.78; E12 -0.073, E16 +0.055, E04 +0.043); Javad combined 0.012, at its scatter; Septentrio pilot 0.010; Leica pilot 0.029, partly repeating at a larger scatter;
- Javad against Trimble r -0.02 over 27 satellites: the term follows the make, not the mode;
- over 543 satellite-class-day means: 0.026 cycle with no term, 0.022 with one constant per satellite and mode, 0.017 with one per satellite and class;
- the section's reading: "a term per mode is not worth building"; start with none and add one constant per satellite for the Trimble class later.

On the Galileo E1/E5a wide lane (0.751 m) the term is about 2 cm rms, at most about 5 cm (arithmetic).

### 3.2 The parameter

**One code column per (satellite, receiver make) for the combined-mode class, ionosphere-free by construction, Trimble class first.** It enters the code rows of both bands of a receiver of that class with the same coefficient (sign `kSatBiasSign`), so its geometry-free part is zero without a constraint row.

Why each part:
- **Make, not mode:** 107.8 above. A per-mode or per-signal column spans the codes-apart form (B_X = B_C + d), which NRF 112.5 to 112.7 rejected; it is not offered as a setting.
- **Per satellite:** the term varies by satellite within a make. A per-receiver constant would duplicate the receiver code bias (the ISC form, include/common/ParamRef.hpp:122).
- **Code, not phase:** in float a per-(satellite, class) phase term is absorbed by that class's ambiguities. 107.8 measured a Melbourne-Wubbena term (wide-lane phase minus narrow-lane code); the record treats it as a code term (NRF part18 line 35, "the per-make code term of section 107.8"; part16 line 72 reads it from the satellite code-bias states). The split into code and phase is not settled (section 9).
- **One column, not one per band:** with the geometry-free part held at zero, two band columns plus a tight pin row carry one free value per (satellite, class). One column says the same with no stiff row and is easier to read. A split by band waits for the author's decision on releasing the geometry-free part.
- **Phase stays shared:** the satellite phase bias is the family's single band state; no phase member term in the first build.
- **Border, not station:** shared by every station of the class, so it is an `EstimatedBorder` column (include/network/assembly/ParamInventory.hpp:137), born in `networkAdmitSharedStates` (src/core/network/assembly/SharedAdmission.cpp:77), the shared-state route of docs/onboarding/08_adding_a_network_parameter.md:57-61.

### 3.3 Estimability and its datum

Code row of receiver r of class k on member signal j:
P = rho + c dt_r + b_r,j - c dt^s - B^s_j + mu_j I_r^s - d^s_k,
where B^s_j is the identity's shared satellite code bias and d^s_k the member column (same for both bands). Three directions leave the data unchanged (confirmed by the review):
1. **Per class, over satellites:** d^s_k + c_k for every satellite with b_r,j + c_k for every receiver of class k. Removed by one zero-sum over satellites per class.
2. **Geometry-free, per (satellite, class):** d + alpha mu_j with I + alpha at that class's receivers, the phase change absorbed by float ambiguities. Removed by construction (one column, equal coefficients, so no geometry-free part exists).
3. [corrected 2026-11-19: see Corrections, item 1] **Against the satellite clock and B^s:** the ionosphere-free part is separable only because receivers tracking the identity signals observe the same satellite (B^s is theirs). Phase adds nothing in float. So estimability needs the reference group in view of the satellite, not only receivers of the class.

The datum therefore is: reference group carries no column; one zero-sum per class over satellites. No interaction with the existing B^s datum is intended (its per-signal sum-to-zero and the broadcast T_GD geometry-free pin act on the identity columns; the member column has no geometry-free part). That no gauge is fixed twice is inferred and must be a unit-tested rank check.

**The zero-sum form.** The first-stage design made it time-linked, as `network.cb_time_linked` does (src/core/network/solve/CodeBiasDatum.cpp:339-348, rows in `NetworkSbasis::buildCodeBiasDatumRows`). NRF 112.7 (part16 line 111) names the time-linked code datum, a tight birth prior, the time-linked geometry-free pin and q 1e-10 m2/s as what made E11's combined-mode column stiff; 112.6 (part16 line 72) shows it moving 0.20 cycle (about 15 cm) over hours with a formal sigma of 0.00 to 0.02 m. The new column has fewer receivers (13 Trimble against 28 combined-mode). The memo drops the time-linked form as the default. Two candidates, for the author: (i) a soft zero-sum over every carried column of the class, in view or not (out-of-view columns carried with growing variance), so a satellite setting does not move the others (inferred to be sufficient); (ii) the time-linked form re-levelled at each class change, with the drift statistic of 3.5. No birth prior ties a new column to the veterans.

### 3.4 Process noise

The first-stage design took prior sigma 0.15 m and q 1e-10 m2/s from the user-side receiver-satellite code bias, calling them measured. They are not: the ledger rows at main 5ddc422, `filter.initial_std.rx_sat_code_bias` and `filter.process_noise.rx_sat_code_bias_q` (docs/CONFIG_promotion_ledger.json:765-766), are UNVALIDATED and say "Built, unit-tested, never run on data". The q here is derived instead from 107.8's day-to-day scatter of the Trimble day means, 0.012 cycle, about 9 mm: q at most about (0.009 m)^2 / 86 400 s, near 1e-9 m2/s (arithmetic; an upper bound, since the scatter includes noise). The prior sigma is the term's own scale (at most 5 cm) plus margin. The value is the author's.

### 3.5 Response when the truth moves (invariants 1 and 7)

Each edge has a counter and an event carrying the typed identity (section 5).
- **Drift and step test, windowed.** A per-epoch k-sigma test on the class's mean code innovation cannot see the term: with code noise of a few decimetres per row, the mean over about 13 receivers has sigma near 0.08 m (inferred), above the term's whole size, and 112.7 shows a slow wander of 0.3 mm per epoch passing the 0.05 m emission jump test. The response is a cumulative statistic over hours (CUSUM of the class-mean innovation per satellite), or a re-validation of the column against the class's Melbourne-Wubbena arc means (107.8's method). On a trip the column is refloated to its prior, never re-seeded stiff from one epoch.
- **Satellite events.** A satellite code-bias re-seed (`this_epoch_cb_reseed`, CodeBiasDatum.cpp:339) or a satellite clock-step trip refloats that satellite's member columns.
- **Class change.** A receiver type or firmware change at a file boundary or restart moves the station's rows from the old class (decoupling edge) to the new one (re-coupling edge), and refloats its receiver code bias, which absorbed the old class's constant.
- **Coverage, counted on both groups.** The column is estimable only while at least `min_receivers` of the class and at least one reference-group receiver see the satellite. While either is missing the column is carried with variance growing by q times the gap, refloated past the gap horizon, re-validated on return. This is not an `AbsorbedReason`: that enum is defined as "another parameter of the station takes the withheld term up" (ParamInventory.hpp:173-177), and no station parameter can take up a per-satellite term (the slant ionosphere and residuals would, inferred). It gets its own counters in the run-end report.

### 3.6 Bar on the column itself

Before any adoption, a 112.6-style trace: per (satellite, class) the column's motion against its formal sigma, every hour; no move above 3 sigma lasting longer than an hour. This bar exists because nothing in the design proves the new datum avoids 112.6.

## 4. Whether it goes to users

**Not by default.** The code record is keyed by satellite and signal (`rec.bias_per_signal[signal]`, src/core/products/NetworkProductEmitter.cpp:842) with no make field. The member records keep repeating the identity's values, flags and counters (EmitStore.cpp:60-72), so users see no change from the term.

- Indirect gain (inferred, no bar ties it): the network's satellite clocks, code biases and wide-lane decisions on Trimble links stop absorbing the term.
- A Trimble user keeps its own residual (up to 0.07 cycle, NRF 109.1 point 6, part15 line 51). The user-side receiver-satellite code bias on main could remove it but has never run on data.
- The open question of CHA part3 line 136 stands: a per-(satellite, band, class) stream record is the alternative.
- A `signal_mean` emission (identity plus the receiver-weighted mean over makes) would change every C1X and C5X record; that it helps any user is unconfirmed. It is not in the first build.

## 5. Typed identities

From include/common/GnssIdentity.hpp (`SatId`, `SignalId`, `System`, `FrequencyBand`), new:
- **The native family table, typed:** identity `SignalId` and member `SignalId` per band, replacing the text bridge `phaseFamilyCodeOf` at the boundaries left by S6 (election accessors, ring-fix maps, `pairConsidered`, the receiver phase bias of a member receiver keyed by the band's identity).
- `enum class ReceiverMake`, parsed once by the roster from `REC # / TYPE / VERS` (src/core/rinex/RinexParser.cpp:284), with its roster site counter.
- `TrackingClassId {System sys; ReceiverMake make;}` (the combined-mode class of a constellation).
- `MemberBiasId {SatId sat; ReceiverMake make;}`.
- A roster-owned class table printed at run start: class, member signals, make, receivers.

Recipe steps (docs/onboarding/08_adding_a_network_parameter.md):
- `NetworkParamKind::MemberCodeBias` appended, count 13 to 14 (ParamRef.hpp:73-95); a `ReceiverMake make{Unknown}` field on ParamRef (ParamRef.hpp:385-397; equality covers all fields, so every existing reference is unchanged with the default); a factory `memberCodeBiasRef(SatId, ReceiverMake)`; a `shared.member_code_bias` map keyed by `MemberBiasId`, as `shared.sat_code_bias` (CodeBiasDatum.cpp:85).
- The term in `addSatelliteCodeBias` (src/core/measurement/ObservationEquation.cpp:217-235), as `addSatellitePhaseBias` attaches the receiver phase bias (lines 240-261); the row's class from a new ObsInput field; the user view finds no column and leaves the term out. Whether it gets its own ObsEffect is open: under the existing `SatelliteCodeBias` effect the per-effect attribution and traces would mix it with B^s (inferred); a separate effect reads more clearly.
- [corrected 2026-11-19: see Corrections, item 3] Declared treatment (registry block, inventory label, `kCodeRow` contribution); carry as a `FieldDecl` map on the satellite-signal entity, `Persist::Record`, snapshot round-trip test; fingerprint token when armed, as the phase-family tokens (src/core/network/state/SnapshotFingerprint.cpp:99-100).
- Keys, all default off, ledger rows UNVALIDATED: `network.member_code_bias`, `_makes` (default `["Trimble"]`), `_q_m2_s`, `_prior_sigma_m`, `_min_receivers`, the drift test's window and threshold. No `classes: signal` and no `_emit` key in the first build. Run-end line: columns estimated, refloats by cause, coverage gaps, class changes, INERT warning when armed and nothing estimated.

Identical with the keys off: no class table, no make on any ParamRef, no class on any row, no token, no datum row; products and traces byte-identical. The one declared text change is the new kind's line in the run-start parameter-model table.

## 6. Places that assume two signals or read digits and letters

In scope for the Galileo native build:
- `chainStationChordRows` names the family `c|s1+s2` from the station's own signals (src/core/network/solve/ChainChordTable.cpp:69-76): a combined-mode station yields "E|L1X+L5X", a pilot station "E|L1C+L5Q". The ring fix must key the family by the band pair (native step 4). The `chain_signals` check `c|s1` must admit members.
- `pairConsidered` keyed by letter string and text signals (RingFix.hpp:862-864); `ringFixCascadeReference` compares digits (src/core/network/ar/RingFix.cpp:108-115).
- The near-null guard requires `sat[0]=='E'`, `sig1[1]=='1'`, `sig2[1]=='5'` (src/core/network/ar/RingFixSeams.cpp:57-58); it holds for both Galileo kinds but never tests GPS, BeiDou, QZSS or other Galileo pairs. Its code residuals are keyed by (satellite, digit) with `emplace` (RingFixSeams.cpp:1132-1135, 1216-1229): with two codes on one digit at one receiver the first wins. Harmless for Galileo while the both-declared refusal stands.
- The emitter's member map keyed by `s.sat[0]` (NetworkProductEmitter.cpp:848, 980): a text boundary, correct for any letter.

Out of scope for Galileo, blocking for other systems (one network for all is the end state):
- `chainPairFamilies` takes band 1 as any signal with `s[1]=='1'` and pairs it with the lowest other digit (ChainChordTable.cpp:47-56). BeiDou in its reference configuration (B1I/B3I as L2I/L6I, config/golden/golden_field_bds_refound.json:128-133) forms no family; the RINEX 3.02 spelling (L1I, L6I, L7I) pairs L1I with L6I by digit order; GPS L1C+L1W+L2W gives two families sharing L2W; QZSS L1C and L1X both read as band 1. Configured pairs keyed by `sat[0]` can supply a pair (ChainChordTable.cpp:33-44); the default path cannot.
- `chainWavelengthM` (ChainChordTable.cpp:24-31) and `makeChainBand` (ChainDatum.cpp:505-523) take the wavelength from `nominalWavelengthOfDigitM(sys, digit)`. The identity header says the 3.02 spelling of B1I is told apart by its attribute in `SignalId::band`, never by the digit (GnssIdentity.hpp:86-91, 178-180, 250-262), so L1I gets B1C's wavelength here (inferred; whether `fromObsCode` rewrites the digit is not traced).
- [corrected 2026-11-19: see Corrections, item 2] `chainFamiliesOf` forms a pair only for exactly two signals on different digits (ChainDatum.cpp:551-565); GPS L1C with L1W falls to per-signal election.
- Roster band cells read `r.signal[1]` with band 1 as digit '1' (SignalRoster.cpp:153-158, 175, 205; inferred: BeiDou B1I never takes the band-1 rules); SecondPhaseScope.cpp:42 and SecondCodeScope.cpp:34 require digit '1'; ExtraBandScope.cpp:43-52, ChannelAll.cpp:36, 113, 120 and SolPrep.cpp:1397-1403 read digits.
- GPS families where every receiver tracks both signals (L1C with L1W) need the both-declared refusal (ApplicationNetworkSetup.cpp:1449-1460) to give way to two arcs of one family at one receiver; there the ISC bridge rule already determines the per-receiver offset (CodeBiasDatum.cpp:64-100).

## 7. Step plan

**Place.** The later entry of the chain audit (CHA part3 line 134) governs: the native build does not start after S7 (b); after the usage reset, two design packages give it its basis (this one and the joint network across systems, bands and channels), then the author decides; after S7 (b) the order is S10, S11, milestone D, S12. NRF 123 owed item (d) (part18 line 35) and the heading of the CHA design note (part3 line 79) say "after S7 (b), before S10"; they are superseded by that entry. This memo is for the session that builds after the saved-state step (NRF 125).

Native family, without the term:
| step | content | identical or graded |
|---|---|---|
| N1 | typed family table and identities; S6's remaining text keys typed | identical on the adopted form and on the main references |
| N2 | native intake mode (no rename), roster band role maps both kinds to the band, behind a key default off | identical with the key off |
| N3 | candidate grouping: a member arc's ambiguity paired with its band's satellite phase-bias key | identical with the key off |
| N4 | ring fix re-keyed by typed band pair (chord table, links, settled, untied, certified maps, `pairConsidered`) | identical on the adopted form (the map is the identity there) |
| N5 | estimator: one satellite phase-bias and one code-bias column per band; member rows use the identity's columns | identical with the key off |
| N6 | emitter: member records repeat the identity's values, flags, counters | identical with the key off |
| N7 | slip layer unchanged; checked | unit test |
| G-b | native mode on, against the adopted form | graded (bar b) |

Bar b (CHA part3 line 79 onwards, kept): beds m1 and h40, then DOY 146 and 147 at 24 h (both roots on DOY 146) with the 21 users: NRF 123's F1 to F6 (part18 lines 39-44), the two-run audit 0 failing Galileo checks, lost satellite-samples not above the adopted form's, identity records equal to the adopted form's within solve noise. Graded, not diffed: the arcs differ, so the summation order differs.

Member term:
| step | content | identical or graded |
|---|---|---|
| T1 | identities, class table, kind, slot, treatment, term, keys, all off | identical apart from the declared parameter-model line; unit tests for header parsing, partial and sign, no term for the reference group, the zero-sum rows, the rank check against the B^s datum, the fingerprint |
| T2 | simulator: two-mode bed, injected per-(satellite, make) term at the observed size (0.026 cycle rms, at most 0.07) and one step of that size | graded: term recovered against truth; step detected by the windowed test and refloated with identities; no held integer wrong against truth; run-end counters non-zero; the 3.6 column-motion bar |
| T3 | observed data against the native family without the term | graded (bar c) |

Bar c: Trimble chord wide-lane fractions narrower (107.8's 0.026 toward 0.017 cycle), estimates matching 107.8's constants in sign and size, no user hold wrong by the float check, the 3.6 column-motion bar, the users' millimetre cost measured by hour. Unit tests cover the class-change and coverage edges.

**Runs needed later (none requested now):** N1 to N6 off-identity pairs on 2 h configurations (adopted form and main references); bar b on m1, h40, then DOY 146 (two roots) and 147 at 24 h with controls and 21 users; T1 one off-identity pair; T2 bed control plus arm; T3 2 h pairs on DOY 146 and 147, then 24 h on DOY 143 to 147 plus DOY 146 from the second root, with controls and 21 users per day.

## 8. Differences from the chain cleanup's design note, and from the first-stage design

Against the design note (CHA part3 lines 79-134):
- Agreed: members keep their arcs; one tree and one satellite phase bias per band; a satellite-side code term per make within the combined mode, zero mean over each class's satellites, network-only, own key default off, Trimble first; bars a to c.
- The note says per (satellite, band, class); this memo carries one column per (satellite, class) with the geometry-free part zero by construction, and leaves a split by band to the author.
- The note's step 5 says member records are "the identity's value plus the modelled term", which contradicts its own statement that no per-make record exists. This memo repeats the identity's values.
- This memo adds the gauge analysis, the coverage rule on both groups, the response edges with a windowed test, and the column-motion bar.
- The note's heading places the build after S7 (b); its later entry (line 134) does not, and governs.

First-stage design against the review (kept where the review confirmed, changed where it refuted):
- Confirmed and kept: the code anchors; class by make; the three gauge directions; border column; network-only by default with the member-record contract kept.
- Refuted and changed: the time-linked zero-sum, the tight geometry-free row and q 1e-10, the same ingredients 112.7 names for the failed column (changed: one column, no pin row, non-time-linked or re-levelled zero-sum, q from the 0.012 cycle bound); "measured" support for q (the ledger rows were never run on data); the per-epoch jump test (replaced by a windowed statistic, and T2's step at the term's size instead of sd 0.05); the `classes: signal` option and its equivalence step M3 (dropped: it spans the rejected codes-apart form); every make at once (Trimble first); coverage counted on the class alone and filed as an `AbsorbedReason` (changed); the place after S7 (b) (changed).
- Marked unconfirmed, not dropped: that the ionosphere-free part is learned from float data within hours at 13 receivers; that `signal_mean` helps any user; that one effect for both terms is harmless to the traces.
- Disagreement left open: the review prefers a non-time-linked zero-sum; the time-linked form re-levelled at class changes is also kept as a candidate, since neither is shown to avoid 112.6.

## 9. Not settled

- How 107.8's Melbourne-Wubbena term splits into code and phase, and into ionosphere-free and geometry-free parts; T3 measures what the ionosphere-free-only column leaves.
- Whether the term is constant within a day (within-day scatter 0.030 cycle, about the noise).
- Where a streamed network gets the receiver class (RTCM 1033 or the station roster, inferred); the streamed ingest refusal (owed (e)) stands regardless.
- How the term interacts with the near-null guard: a constant code offset and a one-step error look alike in a link's code residuals (NRF 108.1); removing the class offset may sharpen the guard (inferred).
- Whether the native build or the term changes the users' millimetre cost or the two wrong GPS runs; their causes are untraced.

## Decisions for the author

1. Whether to build the member term at all before the five-day read and before bar b of the native family; 107.8 judged the term small against the decision's margin, while 109.1 point 6 states its cost to a Trimble user.
2. Class scope: Trimble only first, or every make; granularity make, model, or make plus firmware; whether the Leica pilot group (0.029 rms) is split too, which needs a fixed-weight zero-sum across groups instead of a reference group.
3. The geometry-free part: zero by construction (one column, this memo's first build), released through held wide lanes (an event at the hold and release edges), or seeded from a table built from the observation files.
4. The zero-sum form: over every carried column of the class, or time-linked and re-levelled at each class change.
5. The value of q (at most about 1e-9 m2/s from the day-to-day scatter) and the prior sigma.
6. Users: network-only, or a per-(satellite, band, class) record on the stream (the open question of the chain audit's later entry); whether `signal_mean` is ever wanted.
7. The native intake as a new value of the table key or a separate key.
8. GPS: lifting the refusal of files that declare both a member and its identity, for families where every receiver tracks both signals.
9. The order: confirm the chain audit's later entry (S10, S11, milestone D, S12 after S7 (b), the native build after the author's decision) over NRF 123 item (d).

## Not checked

- The working copies of the files in the paused step (RingFix, emitter, snapshot, fingerprint and others); everything above is from 03dfcbc and may differ from the paused step.
- Whether any BeiDou station on the observed days writes the RINEX 3.02 spelling, and whether `SignalId::fromObsCode` normalises the digit of L1I before `makeChainBand`.
- Whether an ObsEffect value exists that could carry the term separately; the effect list was not read.
- The rank of the proposed datum against the existing B^s sum-to-zero and T_GD pin: argued, not computed.
- The 0.08 m sigma of the class-mean code innovation: an estimate from typical code noise, not read from a run.
- The line numbers of SharedAdmission.cpp, ObservationEquation.cpp, CodeBiasDatum.cpp, SnapshotFingerprint.cpp and RinexParser.cpp were confirmed by the review, not re-read here; ChainChordTable.cpp, RingFixSeams.cpp:50-66, ParamInventory.hpp:170-195, GnssIdentity.hpp, the ledger rows at 5ddc422 and the record passages cited were re-read.
- No engine, test or build was run.

## 2026-11-19: corrections from the check against the code

Trees: network code and record at 7fb0737 (`D:/GnssProcessorSupport/worktrees/read_7fb0737`), user-engine ledger at main 5ddc422. Items 1 to 3 are ruled wrong and are marked in the body; items 4 to 9 refine wording the check confirmed or settled, and are not marked.

1. **Separability of the member column (Outcome, section 3.3 point 3).** The document says the ionosphere-free part is separable only through receivers that track the identity signals. A toy rank computation (`_critical_read/_scratch/NSF/rank_exact.py`) shows that any receiver without a member column separates it: in a Trimble-first build the Javad combined-mode receivers, which track member signals, are reference receivers. The document's own datum (3.3) and coverage rule (3.5) already use "reference group". Corrected reading of 3.3 point 3: separable only because receivers without a member column observe the same satellite (B^s enters every receiver's rows; only those rows lack d).
2. **`chainFamiliesOf` (section 6).** The document says a pair forms only for exactly two signals on different digits. The code also forms families for more than two signals from the configured pairs `network.phase_datum_eq34_family_pairs` (`src/core/network/solve/ChainDatum.cpp:557-560`, 7fb0737; UNVALIDATED, off in the adopted configuration); otherwise each signal is elected alone, so GPS L1C with L1W falls to per-signal election.
3. **Carry of the member column (section 5).** The document carries it as a `FieldDecl` map on the satellite-signal entity. The column has no signal (one column for both bands); a satellite entity exists (`include/network/state/Entities.hpp:297`, 7fb0737). Corrected design: a `FieldDecl` map keyed by make on the satellite entity (`SatelliteState`).
4. **Outcome wording.** "best carried as" becomes "proposed to be carried as", since section 9 states the code and phase split of the measured term is not settled.
5. **Wide-lane size (section 3.1).** "at most about 5 cm" is at most about 5.5 cm (E12, 0.073 cycle; 0.751 m x 0.073).
6. **Rank against the B^s datum (section 3.3, last paragraph; Not checked).** A toy rank computation (float ambiguities, free slant ionosphere, per-signal sum-to-zero and a per-satellite geometry-free pin on B^s; `_critical_read/_scratch/NSF/rank_bdatum.py`) finds the zero-sum row independent of the B^s datum and no free direction left while a reference receiver sees each satellite. The unit-tested rank check on the real datum rows is still owed. "argued, not computed" reads "computed on a toy, not on the real datum rows".
7. **ObsEffect (section 5; Not checked).** No existing ObsEffect carries the term (`include/measurement/ObservationEquation.hpp:110-128`, 15 effects, 7fb0737); a separate effect is a new enumerator, which reads more clearly. The Not checked item is settled.
8. **Wavelength of the 3.02 spelling (section 6; Not checked).** No longer inferred: `SignalId::fromObsCode` keeps the digit as written (`src/core/common/GnssIdentity.cpp:132-137`, 7fb0737) and `makeChainBand`'s own comment says so (`ChainDatum.cpp:510-514`). The Not checked item on `fromObsCode` is settled.
9. **Wording (sections 4 and 5).** "with no make field", "a `ReceiverMake make{Unknown}` field" and "a new ObsInput field" read "with no make entry", "a `ReceiverMake make{Unknown}` member" and "a new ObsInput member".

### Line numbers that moved

At 7fb0737 against the draft's 03dfcbc (content unchanged):
- `ApplicationNetworkSetup.cpp`: intake rename 245-282 is 253-271 (map 255-261, held-out skip 264); streamed refusal 250-253 is 249-252; both-declared refusal with `phase_tracking` 1393-1460 is 1393-1458.
- `RinexParser.cpp`: 379-401 is 377-401.
- `GnssConfigNetworkPosture.cpp`: pairs row 406-415 is 409-415.
- `EmitStore.cpp`: store push 474-478 is 473-479.
- `SignalRoster.cpp`: run-end line 2455-2465 is 2456-2464; band cells 153-158 are 154-158.
- `ChainDatum.cpp`: `chainEdgeKeys` 386-406 starts at 387; `makeChainBand` 505-523 is 505-525.
- `RawObservables.hpp`: 1145-1161 is 1145-1149.
- `RingFix.hpp`: `pairConsidered` 862-864 (the draft also cites 859-864).
- `RingFix.cpp`: cascade reference 108-115 is 110-116.
- `RingFixSeams.cpp`: code residuals 1132-1135 and 1216-1229 are 1129-1135 and 1222-1225.
- `ParamRef.hpp`: the kind count sits at line 98.
- `SharedAdmission.cpp`: 77 is 76.
- `CodeBiasDatum.cpp`: `cb_time_linked` 339-348 is 338-343.

### What stands

- The adopted form renames at the intake (confirmed in code); every other code claim of sections 1, 2, 4 and 6 holds at 7fb0737 apart from items 2 and 8.
- One equal-coefficient code column per (satellite, make) with one zero-sum per class is supported by computation over the per-band form with a per-band zero mean of the chain audit's design note: the note's form leaves S-1 free geometry-free directions per class; the single column leaves none while a reference receiver sees each satellite, and one free direction per satellite the reference group does not see, which the coverage rule of 3.5 handles.
- The evidence numbers of NRF 107.8 and the q bound (about 9.4e-10 m2/s) are arithmetic-correct.
- Decisions 1 to 9 stand as questions: (1) whether to build the member term before the five-day read and bar b; (2) class scope and granularity, and the Leica pilot group; (3) the geometry-free part (zero by construction, released through held wide lanes, or seeded from a table), framed correctly by the rank result; (4) the zero-sum form (over every carried column of the class, or time-linked and re-levelled at each class change), framed correctly by the rank result; (5) the value of q and the prior sigma; (6) users: network-only or a per-(satellite, band, class) record, and `signal_mean`; (7) the native intake as a new value of the table key or a separate key; (8) GPS: lifting the both-declared refusal; (9) the order, which reads the record correctly. At 7fb0737 step S7 (b) has landed (commit 7fb0737), so the later entry's sequence (S10, S11, milestone D, S12, the native build after the author's decision) is now live.

### Left unconfirmed

NRF 112.4 and 112.5 (that a per-mode column spans the rejected codes-apart form); NRF 109.1 point 6 (part15 line 51, the 0.07 cycle user residual); the ledger paragraph of section 123; the real B^s datum rows (ionosphere-free pin election, time-linking) against the new zero-sum; the 0.08 m class-mean innovation estimate; whether the ionosphere-free part is learnable within hours at 13 receivers; the paused working copies; any run output.

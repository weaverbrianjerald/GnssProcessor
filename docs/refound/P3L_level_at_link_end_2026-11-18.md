# P3L: a satellite's level when its tree link ends (2026-11-18)

**STATUS**

- **id:** P3L
- **topic:** network-vehicle
- **status:** open
- **summary:** A design study of the host choice at a released re-host (chain audit defect P3): the satellite's level moves by the new host link's rounding residual, and nothing compares it with the satellite's other links. The design is fixed in shape (the host nearest the circular medians of the satellite's converged links, orphaned satellites only, an integer target, no extra move) and concludes no build now. Under the adopted single Galileo network the per-band form is gone; the rate and size of the remaining single-link re-hosts are measured only for two traced satellites on one day and await the five-day read of that network.
- **source:** plan package 6, a design study that concludes no build now (the level of a satellite at the end of its tree link); network code read at 7fb0737 (the draft was written at 03dfcbc; the cited files are unchanged or moved by a few lines) and user-engine code at main 5ddc422 (none cited); [CHA part 2](CHA_chain_audit_2026-11-18_part2.md) section 4; [NRF part 12](NRF_network_ring_fixing_2026-11-14_part12.md) to [part 18](NRF_network_ring_fixing_2026-11-14_part18.md) (sections 96 to 125); [IRD](IRD_integer_recovering_datum_2026-10-13.md) sections 5, 9 and 11. Written by an agent team in the night of 2026-11-18 and checked against the code by an independent agent; the corrections are the last section.

**Outcome.** At a released re-host the satellite's level moves by the new host link's rounding residual, and nothing compares that level with the satellite's other links. Under the adopted single Galileo network the per-band form is gone (8, 4 and 7 lost satellite-samples a day against 495, 431 and 415, NRF 118.1; 0 split samples of 8 239 in n46c_n147, NRF 120). The single-link form remains: 0.02 to 0.08 wide-lane cycle at converged hosts, one move of −0.18, young hosts 0.17 to 0.49 (per-band configuration). The design picks the host nearest the converged links' medians, orphans only, with an integer target and no extra move. No build until the five-day read counts these re-hosts and a bed produces the case.

Read at commit 03dfcbc. Files of the paused step were read only as committed (`git show 03dfcbc:<path>`). All code citations are path:line at 03dfcbc. Nothing was built or run.

## 1. Evidence in short

**What the code does.**
- The pin target of a satellite's tree edge on one band comes from `draftPinTarget` (src/core/network/solve/ChainDatum.cpp:1289-1307). With an anchor incumbent and a carry on the arc, the target is λ·round of the new host link's own carry (:1299). Otherwise, with the seed round on, it is λ·round of the link's state value (`draftSeedRoundTarget`, :1232-1274, rounding at :1261), frozen per (station, ambiguity) once realized (:1239-1253).
- The re-host row (`applyRehostRow`, :1743-1781) keeps that drafted target, makes the step unknown (:1778), marks the release row Rehost (:1776) and releases the moved side. Its comment states that the integer of a chord the previous epoch certified "is not read, so every re-host takes this rule" (:1725-1728).
- The draft runs once per band (:2132), so each band's level is set by that band's host link independently.
- The one exception is keep-carry (`network.ring_fix.rehost_keep_carry`, default false at include/config/GnssConfigTypes.hpp:1942, RECOMMEND_SCOPED in docs/CONFIG_promotion_ledger.json:50): a usable held link whose carry is within 1e-4 cycle of its held integer takes that integer at a step of 0 (ChainDatum.cpp:1697-1714, 1756-1759). `chainEntryStep` never makes a returning row known (src/core/network/ar/RingFix.cpp:737-744).
- The event table has a RehostCertified row, "the chord's integer from the previous chord table" (src/core/network/solve/ChainFrame.cpp:50-53), but only the observatory's `certifyRehost` (:371-395) classifies with it. The election never acts on it; every released re-host is RehostUncertified (:54-57).

**What is not measured.** The re-host counters (`eq34_rh_released`, `eq34_rh_asbuilt_*`, `eq34_rh_carry/cold`, ChainDatum.cpp:1766-1772) and the keep outcomes (RingFix.cpp:747-763) record nothing about the distance between the new level and the satellite's converged links. The chain audit says the same (CHA part 2, section 4, P3). The audit's line numbers refer to an earlier revision; the equivalents at 03dfcbc are those above.

[corrected 2026-11-19: see Corrections, item 1] **What remains under the single network.** Galileo is one pair family under the adopted single network (NRF 123; `formChainFamilies`, ChainDatum.cpp:601-624), so a satellite's two band edges move onto one link together. In NRF 121.3 the lost class fell from 52 to 0 and the split child-epochs from 2 440 and 2 028 to 0. What stays is the single-link form: the satellite's level, modulo whole cycles, carries the one new host link's own distance from the other links (inferred from the arithmetic of NRF 120).

Its observed size:
- NRF 120, six moves onto DJIG00DJI on one day in the per-band configuration: 0.02 to 0.08 wide-lane cycle from the other links' mean. Larger off that receiver: E25 onto GOP600CZE at 8.967 h, −0.18 over 10 links. Young hosts with 1 or 2 other links (+0.08, +0.06) say little.
- Rounding residuals of the target: −0.370 to +0.453 cycle on 12 unheld re-hosts, 0.000 on 7 held ones (NRF 120).
- GPS, always a pair family: the unflagged class with at least 3 unheld converged links had 20 and 33 samples on DOY 147 and 146, median offset 0.057 and 0.072, none above 0.2 (NRF 120 addendum).

**Setting satellites.** On simb_mode1z the clock datum's satellite E02 was re-hosted three times in six minutes (NRF 106.5): 39 phase-bias steps over 2 cm against 15, flagged on 67 of 120 epochs in hour 5. `held_first_by_life` cut its host changes from 3 to 2 per band but left the flags at 67 of 120, because the first change (4.12 h) went onto a link that was not held (NRF 116, 116.1). The two sections list different hosts for the same satellite (106.5: S56000SIM 5.54 h, S40000SIM 5.61 h, S29000SIM 5.65 h; 116: S11000SIM 4.12 h, S29000SIM 5.36 h, S37000SIM 5.65 h).

[corrected 2026-11-19: see Corrections, item 2] **Why `network.ring_fix.reattach` failed (not repeated here).** It moved live tree edges onto the median candidate, each move an ordinary uncertified re-host (lead rank 1, ChainDatum.cpp:2574). The first version moved converging satellites again every 11 min (NRF 102.1). The amended version failed S2 and S4: each move cost 10 to 15 min of window and persistence, +21 to +28 unflagged epochs per moved satellite against a bar of 10 (NRF 102.3). The ledger row (docs/CONFIG_promotion_ledger.json:58) is KEEP_OFF, parked by the author on 2026-11-17 with no further runs or tuning, and its to-do names the form pre-registered here: "the pin-at-re-host form ... adding no move and no window restart".

**Simulator limits.** The beds derived from runs/simnet60_quiet_col_24h produce the single-link form through an injected host code error (h40 E09, d1 E25 and E14) and setting-satellite churn (simb_mode1z). They do not produce a lost satellite from a code bias at the root's link (102.1), untied satellites lasting hours (102.3), a quarter-cycle re-host onto a young link (99.1), or split band edges (120 addendum).

## 2. The judged comparison

Two first-stage designs were weighed against an independent check of the code and the record.

Points common to both and confirmed:
- The only freedom at a release is which link becomes host. The target must stay an integer (IRD bar I-a: 0 non-integer targets, 0 fractional known shifts under the claim), so a median of the links cannot enter as a target, only as the choice of the host whose residual lies nearest it. This follows from ChainDatum.cpp:1299 and the bar.
- Re-seeding from code is the failure mechanism itself (code sets the wide lane through the slant ionosphere at about 0.87 cycle per m, NRF 103). Dropping the pin is impossible (one pin per tree vertex). Both are rejected.
- `ringFixReattachChoice` (RingFix.cpp:316-345) can supply the agreement test once its minimum-offset condition (:332-335) is a parameter, keeping one owner for the choice.

| Point | Design A (consensus preference) | Design B (tiers and provisional state) | Verdict |
|---|---|---|---|
| Which satellites get a preference | every satellite of a held family | orphans only (incumbent arc not live, or latched) | **B.** A preference on a satellite whose own edge is alive but whose host receiver sits in a cut-off fragment can attach the satellite first and reverse the edge (ArcGraph.cpp:608-646). A's "adds no re-host" holds only when the incumbent path to the root survives. |
| Tie-break | most durable candidate | most remaining life (`chainArcLife`) | [corrected 2026-11-19: see Corrections, item 6] **Neither as written.** Durability is 0.0 unless longevity or `held_first_by_life` is armed (ChainDatum.cpp:1288), so A's break falls to candidate order. B's extra `chainArcLife` calls each increment `eq34_slip_demoted`, whose contract is one read per arc, band and epoch (:450-462). Chosen: nearest the medians; durability only where the walk already computed it. |
| Held links | keep-carry path unchanged | tier H bundles `held_first_by_life` and changes `chainHeldArcs` | **A.** `held_first_by_life` is UNVALIDATED and parked; on the 40 % bed X/X wrong held link-epochs went from 229 to 1 521 (ledger line 66, NRF 116.2). It stays out of this key. |
| Re-coupling edge | measured only | acts: one confirmation re-host per provisional episode | **A.** B's confirmation is the failed re-attachment move with the same offset floors (RingFix.hpp:100-103), and the record predicts it fails S4 (NRF 102.3; 108.1 and 115.1: tests run every epoch on code-error beds act on right states). |
| Fewer than 3 agreeing candidates | today's rule | C0 (keep a settled level) and P (provisional) | **A.** C0 and P add state and tiers for a case with no measured size. B's preference for the other band's host in a one-signal family is moot under the single network. |
| Falsifier bed | inject on a re-host target read from the off run | `simb_hand`: host ends, held links gapped, a fresh arc with a code error | **B's bed with A's placement rule.** A's named target S56000SIM 5.54 h is unreliable (it was a held link, and NRF 116 lists different hosts); B's bed makes the case directly. |
| Simplicity | one store for one epoch, one walk member, one shared choice | four tiers, provisional columns in snapshot and fingerprint | **A.** |

Disagreements stated plainly:
- **The benefit is narrower than either design claims.** A host nearer the consensus shrinks the level move, about 0.08 cycle (roughly 1.5 cm on E1) at the sizes of NRF 120. It does not shorten the unflagged time of a satellite whose level was already right: every released re-host still restarts window and persistence and erases carries (ChainDatum.cpp:1776-1780). That time, and the user resets that follow it (NRF 106.5), shrinks only with a known step: keep-carry, or the RehostCertified row. Neither design builds the second route, which waits on P11 (two chord tables).
- **The dominant wrong-integer route on the simulator is not this one.** The 130 wrong decisions of h40 came through an outlier on a root's tree edge, a birth pin (NRF 103.2). A median cannot see an error past half a cycle, which wraps (NRF 105). The benefit of this design on the beds that exist is therefore unknown.
- [corrected 2026-11-19: see Corrections, item 4] **Frame equivalence is inferred.** The candidates are t−1 chord floats read as ionosphere-free and wide-lane fractions (src/core/network/ar/RingFixSeams.cpp:303-317); the pin rounds each band's arc carry at t (ChainDatum.cpp:1295-1299). The two fractions fix both bands' fractions in principle, but that the t−1 chord fraction equals the t carry residual in the chain frame is not shown.
- **102.1 against 103.2.** 102.1 says a code bias on the root's links never reaches the satellite's level; 103.2 traces 130 wrong decisions to an outlier on the root's own tree edge. Both can hold (the level is unaffected while the chords through that edge are not, inferred), but the record does not reconcile them.

## 3. The chosen design, and why no build is justified yet

**Decision: no build now.** The shape below is fixed so that a build, when justified, starts from it.

Reasons:
1. The per-band form is gone under the adopted single network. The remaining single-link form was observed at 0.02 to 0.08 wide-lane cycle on six moves of one day in the per-band configuration; GPS, a pair family throughout, shows a median 0.057 to 0.072 with none above 0.2.
2. The adopted configuration has not run as a whole; its five days are pre-registered and nothing more is adopted before the author reads them (NRF 123).
3. How often re-hosts onto unheld links still happen under that configuration, at what distance, and with how many agreeing candidates, is unknown.
4. No bed produces the case, so the simulator bars cannot be evaluated.
5. [corrected 2026-11-19: see Corrections, item 3] Every file the build touches in the chain datum and ring fix overlaps the paused step's uncommitted files (chain audit S7 (b), NRF 125).

**What would justify a build** (both needed):
- the five-day read shows, per day, re-hosts onto unheld links with a wide-lane distance of at least 0.10 cycle from three or more agreeing converged links, at a rate that matters (a working threshold of more than 5 such re-hosts a day, or any one followed by a satellite unflagged for more than 30 min; the threshold is a decision for the author); and
- the `simb_hand` bed of section 4 is built, its `verify` passes, and its key-off run shows the predicted offset.

**What would close P3 as inert on observed data:** the five-day read shows only a few such re-hosts a day at no more than about 0.08 cycle. P3 is then recorded as inert under the single network, with that read as its evidence (invariant 3: a deferral triaged inert carries data).

**The design (key `network.ring_fix.rehost_consensus`).**
1. *Evidence at t−1.* After the settled flags of epoch t−1, for every satellite of each held pair family's tree, gather its candidate links with the filters of today's untied-satellite reader (RingFixSeams.cpp:301-320), moved into one shared function: receiver in the flagged piece and not below the satellite in its tree; a float that has passed the window (`ringFixWindowOk`); ionosphere-free sigma at most `kRingFixMaxSigmaCyc` 0.10 (include/network/ar/RingFix.hpp:68); not latched; held links included. Each candidate is read as its ionosphere-free and wide-lane fractions in the chain frame.
2. *Agreement and pick.* The candidates agree when there are at least 3 and their circular standard deviations are at most 0.08 cycle ionosphere-free and 0.10 wide lane (RingFix.hpp:105-109, the reattach constants, reused). The pick is the candidate with the smallest |d_if|/0.08 + |d_wl|/0.10 from the two circular medians. Ties go to the walk's durability only where it is already computed; otherwise to candidate order. No call to `chainArcLife` is added. The function is `ringFixReattachChoice` with the minimum offset as a parameter (0 here).
3. *Orphans only.* At election t a preference is registered only for a satellite vertex whose incumbent tree arc is not live or is latched. The pick is a declared ring-fix value for one epoch, recomputed every epoch.
4. *The walk.* A new member `ForestRules::preferred_arcs` lists the picked arc. It is tried after the held arcs and before the walk's own new edges, with the held arcs' filters. It is not a lead rank (lead arcs come before every incumbent edge, ArcGraph.cpp:607; that was P13's path). If the preferred receiver is not attached in time, the walk keeps its own order and counts a miss.
5. *The pin is unchanged.* The re-host row, an unknown step, the moved side released (ChainDatum.cpp:1743-1781). Keep-carry keeps precedence where it applies. The level moves by the chosen host's residual, which lies within the candidates' spread of their median: near 0 for a satellite whose level is right, onto the piece for a lost one.

**Counters and identities at both edges (invariant 7).** Every row carries the satellite (`SatId`), the pair family, the new host's arc (`ArcId`), and the old and new host receivers.
- *Selection, t−1:* event `RINGFIX rehost_pref` (candidate count, both medians and spreads, picked receiver); outcomes `rc_few` (under 3) and `rc_spread` (disagree), both leaving today's rule in force.
- *Decoupling edge, the election at t:* event `RINGFIX rehost_consensus` per band (actual host, taken or missed, the host's carry residual, its distance to the median); counters `rc_taken`, `rc_missed`, `rc_none` (no evidence: first epoch after a start or resume), `rc_reexpressed` (taken while |median| ≥ 0.10 ionosphere-free or ≥ 0.15 wide lane).
- *Re-coupling edge, measured only:* 600 s after a taken move, the converged links' median in the new frame gives `rc_level_agrees` (under 0.10 and 0.15) or `rc_level_off`; within 1 800 s, `rc_flagged_after` or `rc_unflagged_after`, as the re-attachment's watch does (RingFixSeams.cpp:256-268). No action is taken at this edge: the existing release already refloats what it reconnects.
- *Run end:* one line in the ring fix's own report with each outcome, the largest level move and the re-host count; a run-start line names the rule.

**Cases it cannot cover.**
- Fewer than 3 agreeing candidates (a rising satellite, a small family, E04 at 7.875 h with two others): today's rule applies.
- An error common to all candidates (a satellite-side code term such as E11's, NRF 112.6; the per-make code term, NRF 107.8): the median agrees with the wrong level.
- A host error that wraps past half a cycle: made unlikely, not detected (NRF 105).
- A host chosen at birth with a lasting code error: no re-host happens (NRF 103.1, 103.2).
- Receiver-vertex re-hosts.
- The window and persistence cost of a re-host whose level was already right.

## 4. The simulator falsifier (written, not run)

**Bed `simb_hand`**, built from the adopted single-network bed (the bed of `m1_c`) by a new injector mode of `scripts/nrf_inject_code_bed.py`, with its own `verify` required to pass:
- choose one settled Galileo satellite with at least 8 links whose tree host, read from the key-off run's chain-edge trace, is a long-lived link around 3.0 h (placement rule: at a re-host, never at a birth, since a first sighting pins at 0, NRF 102.1);
- at 3.0 h end its observations at that host receiver;
- put a 300 s gap through 3.0 h at every receiver where it is held, so that keep-carry is refused by the entry check;
- [corrected 2026-11-19: see Corrections, item 5] start one fresh arc at 2.85 h at the receiver the walk's own order would take, with a constant +0.30 m code error on C1C and C5Q (about +0.35 wide-lane cycle at −0.423·b1 − 0.758·b5, NRF 103).

**Clean bed:** `m1_c` without injection, for inertness.

**Prediction, key off:** 30 min after the re-host the satellite's other links show a wide-lane offset of at least 0.15 cycle and the satellite stays unflagged (inferred from NRF 100.1). If the key-off run does not show this, the bed fails to produce the case and the falsifier is void, not passed.

**The rule is falsified if, with the key on:**
- F1. the injected arc becomes host while at least 3 agreeing candidates existed;
- F2. the satellite's other links show a wide-lane offset of at least 0.15 cycle at +30 min, or the satellite is flagged later than with the key off;
- F3. the truth audit finds any wrong held link-epoch, or the frame-free count (`PYTHONHASHSEED=0`) rises (NRF 103.3);
- F4. the re-host count differs from the key-off run by more than 5 % on either bed (the rule adds no move);
- F5. on the clean bed, held link-epochs move more than 0.2 % from key-off, or any satellite is unflagged on more than 10 epochs above key-off (NRF 102.2 item 6).

## 5. Unit tests (one new suite, `RingFixRehostConsensus`, plus the walk's own file)

1. Five agreeing candidates plus one at +0.35 wide lane: the outlier is never chosen.
2. Fewer than 3 candidates gives `rc_few`; disagreeing candidates give `rc_spread`.
3. Fractions straddling ±0.5 give the correct circular medians and pick.
4. Tie-break: with durability not computed, equal scores fall to candidate order and no `chainArcLife` call is made (`eq34_slip_demoted` unchanged).
5. Walk: when every incumbent survives, the forest is identical with and without `preferred_arcs`.
6. Walk, fragment case: a satellite with a live edge whose host receiver sits in a cut-off fragment gets no preference (orphans only), and the forest is identical to the key-off forest.
7. Walk order: a held arc beats a preferred arc; a preferred arc beats a new edge; a slipped preferred arc is refused in pass 1; an unattached preferred receiver gives `rc_missed` and the walk's own order.
8. Pair family: one preference gives identical edge sets on both bands.
9. Keep-carry precedence: a usable held link on its integer is kept at a step of 0 whatever the preference.
10. Off: null `preferred_arcs` reproduces the arc-graph oracle cases.
11. Evidence older than one epoch is ignored; after a resume the outcome is `rc_none`.
12. Re-coupling: a median of 0.02 gives `rc_level_agrees`; 0.30 gives `rc_level_off`.
13. Frame check: on a two-epoch fixture, the t−1 chord fractions of a candidate equal the residual of the rounded carry at t in the chain frame (the inferred equivalence of section 2, made a test).
14. Loader refusals, one test each: without `network.ring_fix.enable`, `held_first`, `settled_flags` or `phase_datum_eq34`; together with `network.ring_fix.reattach`.

## 6. Key and ledger row

Key `network.ring_fix.rehost_consensus`, bool, default false; parsed in the network loader with the config toggle pattern of CLAUDE.md; a `NetworkKnobs` snapshot; a snapshot fingerprint token (it changes which link pins a level). Off, the gathering's first statement is the gate and `preferred_arcs` stays null, so output is byte-identical.

[corrected 2026-11-19: see Corrections, item 2] Ledger row (docs/CONFIG_promotion_ledger.json, added in the same commit as the key):

```json
{"knob": "network.ring_fix.rehost_consensus", "verdict": "UNVALIDATED", "area": "ProducerAr", "default": "false",
 "why": "The host choice at a released re-host (chain audit defect P3; NRF sections 100.1, 102.3, 106.5, 120): when a satellite vertex of a held pair family's tree loses its tree edge, the walk prefers, after the held arcs and before its own new edges, the candidate link nearest the circular medians of the satellite's converged links (at least 3, window passed, ionosphere-free sigma at most 0.10 cycle, circular standard deviations at most 0.08 cycle ionosphere-free and 0.10 cycle wide lane, gathered at the previous epoch's settled flags). The pin is unchanged: the new host link's rounded carry, an unknown step, the moved side released; no integer is decided and no move is added. RINGFIX rehost_pref and rehost_consensus events; rc_* run-end counters. False is off and byte-identical; needs network.ring_fix.enable, held_first, settled_flags and phase_datum_eq34; refused with network.ring_fix.reattach. Pre-registered 2026-11-18 (P3L), the pin-at-re-host form named in network.ring_fix.reattach's to-do. Verified by: none. To do: the five-day read of the adopted single network (re-hosts onto unheld links, their distance, the share with three agreeing candidates); the simb_hand bed and its bars F1 to F5; observed arms only after both.",
 "card": "<the record path of P3L once filed>", "parent": "network.ring_fix.enable", "fingerprinted": true}
```

## 7. Step of the integer-recovering datum plan and its bars

**IRD-4, the re-host row** (docs/refound/IRD_integer_recovering_datum_2026-10-13.md, section 5). The rule changes the row's host, not its integer rule. It certifies nothing, so IRD-5 (lattice certification) is untouched, as NRF 102 argued for the re-attachment.

Bars reused, unchanged:
- IRD-4: off byte-identical; the class reaches n ≥ 1 on its own bed (`simb_hand`), an unexercised class reading as failed; I-b for the class (undeclared truth steps at most 0.02 cycle per band, 0.05 on the wide lane); both edges counted with identities; the float user by hour within max(+10 %, +5 mm) and W5 no higher; one loader refusal per interlock with a unit test.
- I-a: 0 non-integer targets, 0 fractional known shifts under the claim.
- I-c: 0 wrong certified re-hosts.
- The truth audit and the frame-free count (NRF 103.3).
- The clean-bed inertness bar of NRF 102.2 item 6 (F5 above).
- Held link-epochs not below 98 % of the control (NRF 115, S4); every epoch emitted and the lost class not above the control (NRF 121.3).

## 8. Validation runs needed later (listed; none requested)

1. The five-day read of the adopted single network, once its pre-registered runs exist (NRF 123): offline, with the method of `scripts/sat_pin_trace.py`, count re-hosts onto unheld links, their wide-lane and ionosphere-free distance from the other converged links, and the share with three or more agreeing candidates. A read, no new engine run.
2. `simb_hand` built and verified (an injector mode, `verify` PASS).
3. Key-off identity on the controls of CHA part 2 section 5.1, after the build.
4. `simb_hand`, key off and on, 6 h from 00:30:15 (bars F1 to F4).
5. The clean bed `m1_c`, key off and on, 6 h (F5).
6. The code-error beds d1, d31 and h40, key off and on, 6 h: truth audit and frame-free count (F3), since the record shows rules acting every epoch there can turn right chords wrong (NRF 108.1, 115.1).
7. Only after 4 to 6 pass: the adopted configuration's days with users, key off and on, at NRF 123's bars F1 to F6.

## Decisions for the author

1. Whether to accept "no build now", with the five-day read and the `simb_hand` bed as the two conditions for a build, and the threshold that turns the read into a build (proposed: more than 5 re-hosts a day at 0.10 cycle or more from three agreeing links, or any one followed by more than 30 min unflagged).
2. [corrected 2026-11-19: see Corrections, item 2] Whether this pin-at-re-host form falls under the parking of `network.ring_fix.reattach` (2026-11-17, no further runs or tuning). The ledger's to-do for that key names this form, and this design adds no move, but it reuses the same agreement constants.
3. Whether, if the read shows P3 small, to record it as inert under the single network and close it with that read as evidence.
4. Whether the stronger remedy, the RehostCertified row (the chord's integer from the previous chord table, a known step that also removes the window cost), should be scheduled ahead of this rule once P11 is resolved. This rule shrinks only the level move; that route shortens the unflagged time.
5. Whether the tie-break should use remaining life at all. Computing it without longevity or `held_first_by_life` armed needs a second reader of arc life that does not count slip demotions, and `held_first_by_life` stays parked.
6. Whether the bed should also cover the birth-pin route of NRF 103.2 (a lasting host code error on a root's tree edge), which this rule does not reach and which drives the simulator's wrong integers.

## Not checked

- The share of observed re-hosts with three or more agreeing candidates, and how often `rc_missed` would fire. Not read from any trace.
- [corrected 2026-11-19: see Corrections, item 4] Whether the t−1 chord fraction equals the t carry residual in the chain frame (made unit test 13; inferred, not shown).
- Whether NRF 106.5's re-hosts were refused by keep-carry or fell outside it; the record does not say.
- Why E04's and E25's L5Q vertices left their trees in NRF 120.
- Whether a pair family's extension edges (a vertex one band tracks, 199 in NRF 121.3) can recreate a per-band split when the second band appears through a different link (inferred possibility, not traced).
- Whether the band-direction distances (0.02 to 0.22 cycle) matter once both bands come from one link, given the free per-LOS ionosphere (inferred absorbed, not verified).
- The reconciliation of NRF 102.1 and 103.2 on code bias at the root's link.
- `ArcGraph.cpp:608-646` was read only at its opening lines (600-612) for the fragment case; the reversal path is taken from the independent check, not re-traced line by line.
- The exact rows of NRF 120's table and of sections 102.3, 106.5 and 116 were taken from the first-stage summaries and the independent check; only the ledger rows, the IRD bars and the cited ChainDatum.cpp, RingFix.hpp and ArcGraph.cpp lines were re-read for this text.

## 2026-11-19: corrections from the check against the code

The check read the network code and the record at 7fb0737 (D:/GnssProcessorSupport/worktrees/read_7fb0737). Between 03dfcbc and 7fb0737, `ChainDatum.cpp`, `ChainFrame.cpp`, `RingFixSeams.cpp` and `ArcGraph.cpp` are unchanged; `RingFix.cpp` moved by +3 lines after line 20; the insertions in `RingFix.hpp`, `GnssConfigTypes.hpp` and the ledger lie after the cited lines. Items 1 to 6 are the six claims ruled wrong and carry markers in the body; items 7 to 15 are imprecisions without markers.

1. **The 52 to 0 result is attributed to the wrong configuration.** The document (Outcome, section 1 "What remains") places the fall of the lost class from 52 to 0 and of the split child-epochs from 2 440 and 2 028 to 0 under the adopted single network. The record shows it under stage R of the family-pairs key, which was not adopted (NRF part 17 lines 208, 212, 221; part 18 lines 23, 29; tree 7fb0737). The evidence for the adopted network is 8, 4 and 7 lost satellite-samples against 495, 431 and 415 (NRF 118.1, part 16 line 139) and 0 split samples of 8 239 in n46c_n147 (NRF 120, part 16 lines 213-214). Also from NRF 120: in that codes-shared run every E04 and E25 re-host goes onto a held link or is a cold keep, offsets 0.00 to 0.02, 32 unflagged.
2. **The design is not the form named in the reattach key's to-do.** The document (section 1, section 6 ledger row, Decision 2) calls this design the pin-at-re-host form of that to-do. The to-do names a median in place of one new host link's rounded float, with no window restart (`docs/CONFIG_promotion_ledger.json:58`, tree 7fb0737). The document's own section 2 rejects a median as a target and concedes that the window restarts. This design keeps an integer target and the window restart, so it is a narrower relative of that form; the ledger row's sentence and Decision 2 are to be read so.
3. **Reason 5 is stale.** The document says every file the build touches overlaps the paused step's uncommitted files. Step S7 (b) is commit 7fb0737; it touches `RingFix.cpp` and `RingFix.hpp`, the configuration types, the loader and the ledger, not `ChainDatum.cpp`, `ArcGraph.cpp` or `RingFixSeams.cpp` (tree 7fb0737). A build rebases onto it; the reason no longer supports the decision.
4. **Frame equivalence is shown, not only inferred.** The document (section 2, "Not checked") says that the t−1 chord fraction equals the t carry residual in the chain frame is not shown. NRF 120 states that the chord floats equal the carries in the chain frame at all 14 moves whose host had a chord row (NRF part 16 line 161, tree 7fb0737). The equivalence is shown at the epoch before on 14 moves; unit test 13 keeps it.
5. **Sign of the injected wide-lane error.** The document (section 4) gives about +0.35 wide-lane cycle for +0.30 m on C1C and C5Q at −0.423·b1 − 0.758·b5. The formula gives −0.354 cycle on the injected link; the other links read about +0.35 relative to it (NRF part 16 line 227).
6. **Line of the durability rule, and the slip-demotion count.** The document (section 2, tie-break row) cites `ChainDatum.cpp:1288` for durability being 0.0 unless longevity or `held_first_by_life` is armed; line 1288 is a comment of `draftPinTarget`, and the rule is at `src/core/network/solve/ChainDatum.cpp:687` (tree 7fb0737). "Each extra `chainArcLife` call increments `eq34_slip_demoted`" is overstated: only a call on a slipped arc increments it (`ChainDatum.cpp:460-462`; the contract at 449-451 is confirmed).
7. **Size under the single network called unmeasured (overstated).** The summary and reason 3 call the single-link size under the single network unmeasured. NRF part 16 lines 213-214 report no unheld re-host of E04 or E25 in n46c_n147 (0 split of 8 239 samples, 32 unflagged). The size remains unmeasured for other satellites and days.
8. **The single-link size 0.02 to 0.08 is selective.** It holds for converged hosts. In the per-band configuration young hosts carried wide-lane fractions 0.17 to 0.49 (NRF 100.1, part 12 lines 123-126), and one move was −0.18 (E25 onto GOP600CZE). The proposed inert bar of section 3 ("no more than about 0.08 cycle") rests on converged hosts only.
9. **"No bed produces the case" (tension).** Section 1 says simb_mode1z produces the single-link form, and u2_z re-hosts E02 onto an unheld link at 4.12 h (NRF part 16 line 39); its offset was not read. Reason 4 reads better as: no bed is known to produce the case; u2_z's 4.12 h re-host of E02 onto an unheld link (NRF 116) is unread.
10. **Release erasing carries is miscited.** `ChainDatum.cpp:1776-1780` only set flags. The erasure and window restart are shown by `ChainDatum.cpp:1728-1730`, `RingFix.cpp:283-284` and NRF 102.3 (part 12 line 205).
11. **"About 0.87 cycle per m" is imprecise.** 0.87 is the X/X E5a fit; the model gives 0.42 per m of E1 code and 0.76 per m of E5a code (NRF part 13 line 21).
12. **"About 0.08 cycle (roughly 1.5 cm on E1)" mixes units.** 0.08 is a wide-lane cycle (0.751 m, about 6 cm); 1.5 cm holds only for an E1 cycle.
13. **The GPS medians 0.057 and 0.072 are a different quantity.** They are the unflagged class's common offset (NRF part 16 line 229), not a re-host distance; they support reason 1 only indirectly.
14. **IRD-4's gate.** Section 7 cites IRD section 5 for IRD-4; its gate is IRD section 11 (IRD lines 139-141, 179). The bars I-a, I-b, I-c and IRD-4 are confirmed.
15. **The 199 extension edges** of "Not checked" are confirmed, but under the family-pairs form, not the adopted network (NRF part 17 lines 205, 208).

### Line numbers that moved

At tree 7fb0737: `RingFix.cpp` 737-744 is now 740-747 (`chainEntryStep`), 747-763 is 750-766 (keep outcomes), 316-345 is 319-348 (`ringFixReattachChoice`), 332-335 is 335-338 (minimum-offset test); `ChainDatum.cpp` 2574 is 2579 (reattach lead rank 1); `RingFixSeams.cpp` 303-317 is 305-320 (candidate fractions); `ChainFrame.cpp` `certifyRehost` reads 370-395 (with a use at 426). The body's "Read at commit 03dfcbc" names the draft's tree; the check's tree is 7fb0737. Every other cited line holds.

### What stands

- **"No build now" stands.** Reasons 2 and 3 hold; NRF 120's codes-shared run (no unheld re-host of the two traced satellites, 32 unflagged of 8 239) adds evidence the draft did not use. Reason 1 rests on NRF 118.1 and NRF 120, not on the 52 to 0 result (item 1). Reason 4 is restated (item 9); reason 5 no longer applies (item 3).
- **The code description of the re-host path is accurate:** a rounded single-link target, an unknown step, keep-carry as the only exception, RehostCertified unused by the election. The design's elements (host choice only, an integer target, the reattach filters and constants) are described as the code allows.
- **Decision 1 stands** (accept no build now, with the five-day read and the `simb_hand` bed as the two conditions, and the proposed threshold). The proposed inert bar of about 0.08 cycle rests only on converged hosts; the record shows larger single-link errors from young hosts and one move of 0.18 (item 8).
- **Decision 2 is restated:** whether this narrower relative of the to-do's form (integer target, window restart kept, no added move, the same agreement constants) falls under the parking of `network.ring_fix.reattach` of 2026-11-17. It is not the to-do's form (item 2).
- **Decisions 3 to 6 stand as framed:** recording P3 inert if the read shows it small; scheduling the RehostCertified row ahead of this rule once P11 is resolved; whether the tie-break uses remaining life at all; whether the bed also covers the birth-pin route of NRF 103.2.

### Left unconfirmed

- The fragment case of section 2 (a preference reversing an edge): `ArcGraph.cpp:608-646` attaches held arcs only when the incumbent queue is empty; the reversal was not traced, nor the path past line 646.
- "One pin per tree vertex" in the code.
- NRF 112.6 (E11), 107.8, 99.1 and 103.1 beyond the lines cited; the bed name and run of NRF 116.2.
- The runs and traces themselves: no run folder was opened.
- Whether the five-day runs of NRF 123 exist yet.

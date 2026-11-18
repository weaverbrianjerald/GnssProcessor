# NRF part 17: one pair family per configured pair (sections 121 onwards of card NRF)

**STATUS**

- **id:** NRF-part17
- **topic:** network-vehicle
- **status:** open
- **summary:** Sections 121 onwards of card NRF (2026-11-18), continued from [part 16](NRF_network_ring_fixing_2026-11-14_part16.md). Section 121 pre-registers one pair family per configured pair (`network.phase_datum_eq34_family_pairs`, card IRD's step IRD-3 with the two Galileo families kept separate), built on the author's word of 2026-11-18: the arm that tells the two-band tree apart from the single network of section 118 on that section's bars, in two stages (a rapid stage R of 2 h beds and one short observed run, then stage A, the adoption comparison).
- **source:** the manager's build order of 2026-11-18 12:58 and its amendment of 14:05 (the author's word on run length); [part 16](NRF_network_ring_fixing_2026-11-14_part16.md), sections 118.1, 118.2 and 120; [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 11 (IRD-3); [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md) section 5; [CHA part 2](CHA_chain_audit_2026-11-18_part2.md) (the note of milestone B on the pair family).

**Outcome.** Pre-registered, not yet run. In the adopted configuration the chain elects Galileo per signal (four phase signals while the two Galileo families stay separate), and section 120 ties the lost satellites to those per-signal trees; section 118's single network removed them but changed the network as well. The key of section 121 forms one pair tree per configured Galileo family with the families kept separate, so that the two effects separate on the same bars. The adoption of either form stays the author's (section 118.2).

### 121. 2026-11-18: one pair family per configured pair (card IRD's step IRD-3 with the two Galileo families kept separate; pre-registered)

**Why.** Section 118's runs change two things against today's runs at once: one tree over both bands (with the table, Galileo tracks two phase signals, so `formChainFamilies` forms its pair family) and one network for both signal types. Section 120 ties the lost satellites to the per-signal trees that the separate families impose on Galileo, which tracks four phase signals there (card CHA, part 2, the note of milestone B). This arm keeps the two families separate and gives each its pair tree, so the two effects are told apart on the same bars before long runs are made on either form. The author's word (2026-11-18, about 12:50): build the two-band tree per Galileo signal type as a switch; the adoption of the single network is held until this form is graded (section 118.2).

**The key.** `network.phase_datum_eq34_family_pairs`: an array of `"<constellation>:<signal>/<signal>"` entries (the form of `network.ring_fix.pairs`), default empty, which is off and byte-identical. With `network.phase_datum_eq34_family` "pair", a constellation that tracks more than two phase signals in an epoch forms one pair family per listed entry whose two signals it tracks, its bands in signal-string order, in place of the per-signal fallback; a signal in no listed entry is elected per signal, and its constellation-epoch counts as a fallback (`eq34_pair_fallbacks`). A constellation that tracks exactly two signals keeps today's rule. The loader refuses an entry that is malformed, that names two signals of one band, or that names a signal another entry names; it warns that the list is INERT without the pair family (no chain, family "signal", or the pin bridge, which gives the pair family up). A run-start line names the list as armed; a line names each constellation's families whenever they change, with its per-signal fallback and the signals that caused it (today only a run-end counter says it); the run-end block counts the epochs of each family. The ledger row is UNVALIDATED. The key is not fingerprinted, like the family key and the other election knobs: it chooses which tree the next election grows, and a frame step that a new tree puts on a carry goes through the same re-reference and shift responses as any other re-host.

**Where the pairs come from.** The chain's own list, not `network.ring_fix.pairs`: the chain runs without the ring fix (a configuration with the ring fix off keeps its tree shape, and step S2 of card CHA names every ring-fix key set while the ring fix is off as INERT), and the two lists answer different questions (which signals share one tree; which chords the ring fix decides). A run-start line says when the two lists differ while the ring fix is armed. The arms list the same Galileo pairs in both: `"network.phase_datum_eq34_family_pairs": ["E:L1C/L5Q", "E:L1X/L5X"]`.

**Checked in the build** (its section reports what was found): every place that might assume at most one pair family per constellation: the family's remembered datum receiver, the root votes, the chord table and `form_configured_pairs`, `pairConsidered`, the emitter's families, the settled sets and the run-end block.

**Exe and controls.** The key's exe is frozen under a name given in the hand-off. The controls are today's runs (`_rap_d24_n146`, `_rap_d24r_n146`, `_rap_d24_n147`, exe nrf32) and the 2 h references of the base engine (`_rap_chaR_*`, engine cha2 at `72b7b3b`), tied to the key's exe by milestone B's whole-day identity run (card CHA, part 2: the branch reproduces `_rap_d24_n147` over 24 h) and by T1.

**Bars** (as the manager gave them, staged as amended at 14:05).
- T1. Off identical: the gate of card CHA section 5.1 for an identical-output step, as amended: the 2 h controls n147, f40sc and h40 with `nom_td_gate.py --whole --mask-time` (products, every trace and the logs identical), the chain unit cases and the two lints.
- T2. Structure, IRD-3's gate per Galileo pair family: identical band-1 and band-2 tree edge sets every epoch, pins per band equal to the gauge count, pin audit COMPLETE, mismatch 0, every edge-set difference attributed to a band-only arc; no satellite with its two band edges at different receivers inside a family other than through such an arc. Its unit part (oracle cases with two configured pair families on one constellation, and the formation rule) passes before the hand-off; its data part is read on the runs of stage R.

**Stage R, rapid.**
- T3 at 2 h: the arms of m1, h40 and d1 with only the key, against the base engine's 2 h references of those beds (`_rap_chaR_m1`, `_rap_chaR_h40`, `_rap_chaR_d1`), graded from 00:30:15. The clean two-mode bed (m1): 0 wrong held link-epochs by the truth audit, held link-epochs not below 99 % of the control's, every epoch emitted, the held-out user by hour within max(+10 %, +5 mm) of the control. The code-error beds (h40, d1): wrong held link-epochs and the frame-free count (fixed hash seed) not above the control's.
- One short observed run of DOY 147 from 00:00, control and arm on the key's exe, its length set by the second session from today's day run as the shortest that shows the lost class after hour 2 (expected 3 to 6 h; the length is reported before the run), with six users (three of each signal type) evaluated from hour 2. Bars: T2 on the data, every epoch emitted, no user hold wrong by the float check, and the lost-class samples (unflagged satellites with at least 3 unheld converged links, every 5 min from hour 2) at most one half of the control's.

**Stage A, the adoption comparison**, once and only if stage R passes.
- T3's three beds at 6 h, on the same bars, against `m1_off37`, `h40_off` and `d1_off`.
- T4. Observed: the configurations of `d24_n146`, `d24r_n146` and `d24_n147` with only the key, 24 h; `entry_revalidate` off as in section 118. Section 118's bars C4 as written there, against today's runs: GPS held share, RAIM rows, the two-run audit of DOY 146 with 0 failing Galileo checks, bar 7, and the users of section 118 (the same 21 receivers, fixing and float, both days): no hold wrong by the float check, the paired per-receiver difference not worse by more than 1 mm in any hour from 3.
- T5. The question: lost satellite-samples (band-1 tree satellites not flagged, every 30 min) at most one half of today's in each run (today 495, 431 and 415; section 118.1's 8, 4 and 7 beside them). Reported beside it, no bar: the Galileo held share by family and hour, the users' Galileo held pair-epochs and reference changes, against today's and section 118.1's.

**Who runs what.** The build, T1, T2's unit part and a 10-epoch smoke of the on arm: the chain audit's session, before the hand-off of the frozen exe. The arms of stages R and A, their runs and their grading: the second session ("Runs and reads") on that exe; its result file is appended to this card by the chain audit's session.

**Reading prepared.** Whatever the result, no adoption by any session: the form goes to the author on one table beside today's runs and section 118.1's single network, and the author chooses between the forms (section 118.2).

### 121.1 2026-11-18: the build (commit `91f36cf`, engine `gnss_processor_cha_fp1.exe`)

**Built.** Commit `91f36cf` on `chain/audit` (after step S6 of card CHA, `0783ad1`, and the test fix `bcf3bfe`), the key as pre-registered.
- Loader: the four refusals (a malformed entry, an unknown constellation, two signals of one band, a signal that two entries name) and the INERT warnings (family "signal" or no chain; the pin bridge); the run-start line "eq34 FAMILY PAIRS armed [...]"; a line naming the entries that `network.ring_fix.pairs` lacks while the ring fix is armed.
- Election: `chainFamiliesOf` gives one constellation's families (today's rule for two signals, the configured pairs for more than two, per signal otherwise) through `chainConfiguredFamilies`; with the list empty, the families, their order and the counters are today's. With the list armed, the families are counted one by one (`eq34_family_epochs`), each constellation's families are named in the log when they change (at most 40 lines), and a run-end line counts them (a warning when no configured pair formed).
- The knob `eq34_family_pairs`, by constellation, is empty unless the pair family is on (so under the pin bridge too); not fingerprinted (the exemption comment names it). Ledger row UNVALIDATED.

**Checked for an assumption of one pair family per constellation; none found.**
- The family's remembered datum receiver is kept per band (`ConstellationState::channels[signal].datum_rx`), read band by band and written for each band of the family.
- The root votes run over the family's own bands' satellites.
- The chord table and `form_configured_pairs` form families per arc (`chainArcFamilies`: each arc's own signals, then the configured families).
- `pairConsidered` and the settled sets are keyed by the family's text (`"E:L1C/L5Q"`); today's configurations already list both Galileo families.
- The emitter's families (unsettled steps, step classes) are configured lists.
- The run-end block counted the pair families only in total; the per-family counts are new.
- One effect beside them: with the key, the observatory records Galileo's groups in family order (L1C, L5Q, L1X, L5X) instead of signal order; each group's own rows keep their order.

**Unit cases** (Release; 313 chain and loader cases pass).
- The formation rule on a hand-built Galileo epoch: four signals and two pairs, a pair without its partner, one pair listed, the list empty, two signals; each against the per-signal election, with one datum receiver and a spanning tree per band, and a pair's two bands pinned on the same edges.
- The oracle on a two-mode Galileo world whose arcs carry both bands of their mode (640 epochs, 610 with both modes): the configured pairs equal the per-signal election's outputs and its record group by group, every group at full rank, no fallback; 1 865 pair families against 610 per-signal fallbacks without the list; 11 278 Galileo tree edges pinned on both bands of a mode.
- The oracle with band-only arcs (640 epochs, 1 879 pair family-epochs): every group at full rank, an arc that both bands of a configured pair carry pinned on both or on neither, and the band extension's work equal to the arcs' own count over every pair family formed; 2 024 extension edges, 29 band-only components.
- Four loader refusals and the armed and INERT lines.
- Found while writing the oracle: with the shift off, a step's zero or not decides a re-reference, so the test world's perturbation must give a Galileo mode's two bands one carry value and one previous target; drawn apart, the per-signal election itself diverges between a mode's two bands.

**T1** (key off, engine `gnss_processor_fp1.exe` of the build, against the base engine's references `_rap_chaR_h40`, `_rap_chaR_f40sc` and `_rap_cha_ctl2_n147`): pass. Products and every trace are identical to the references on the three controls (exit 0; the logs differ only in the two lines that steps S1 and S2 of card CHA announced), and the key's off runs are identical in full to step S6's runs of the same controls (`_rap_cs6_*`, logs included).

**The on arm's smoke** (`_rap_fp_on_smoke`: the 2 h observed control's configuration of DOY 147 with only the key, 9 epochs): exit 0. Galileo formed both configured pair families at every epoch (`E:L1C/L5Q` 9, `E:L1X/L5X` 9, GPS's `G:L1C/L2W` 9: 27 pair families, 0 fallbacks, 9 extension edges); the run-start, change and run-end lines read as specified.

**Frozen exe for stages R and A:** `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_cha_fp1.exe`, sha256[:16] `da240107c091aa8c` (the build of `91f36cf`; the folder's DLLs are identical to the build's).

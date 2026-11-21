# NRF part 19: the saved state's reading and the states the seam lacks (sections 125.6 onwards of card NRF)

**STATUS**

- **id:** NRF-part19
- **topic:** network-vehicle
- **status:** open
- **summary:** Sections 125.6 onwards of card NRF (2026-11-19), continued from [part 18](NRF_network_ring_fixing_2026-11-14_part18.md), which reached its size. Section 125.6: the manager's reading of section 125.5 (the purpose of step S7 (b) not reached; no bar on integers read on a resumed leg in place of an uninterrupted run until the author rules on the exact resume), what B4 gives, a diagnosis with the resume slip warm-up armed (the differing held integers reduce to the whole-turn class alone), and the items for the author. Section 125.7: B2a read as registered on m1 with the S10 engine and the dump (6 276 of 6 277 lines equal; the window line, which the restore does not install, differs). Section 125.8: the manager's reading, B2a not met as registered by that one line, a near miss for the author, and two more items for the author. Section 126: the failing two-run check of DOY 146 attributed to exact epoch time, the only difference that changes any output on that day (a note; nothing adopted or withdrawn). Sections 127 to 132: card XRS filed and line B started (127); the warm resume, the snapshot write and the fingerprint cut into groups with identical output, and its gate (128); the independent review's annotations of the night commits (129); the resume response of a held link pre-registered (130); G17's return on DOY 144 read and one evidence run of the re-attachment rule pre-registered (131); a trace-only windowed residual monitor for held links pre-registered (132). Section 130.1: the resume response read on its bars and armed at `release` in the two primary reference files (2026-11-20). Section 132.1: the planted hold and the measure-only monitor built; bars 2 and 4 on the planted runs.
- **source:** the manager's message of 2026-11-19 after checkpoint 3 (decision (1)); [part 18](NRF_network_ring_fixing_2026-11-14_part18.md), sections 125 to 125.5; the runs and grading outputs in `D:/GnssProcessorSupport/runs_nom/b2_s7br_grade/`.

**Outcome.** The manager read section 125.5 on 2026-11-19: the purpose of step S7 (b), replaying from a converged state in place of two hours of convergence, is not reached, and until the author rules on the exact resume no bar on integers is read on a resumed leg in place of an uninterrupted run. A diagnosis with the resume slip warm-up armed leaves only the whole-turn class of differing held integers (328, 331 and 496 link-epochs on m1, h40 and n147). The phase wind-up accumulator is the state the seam lacks for the integers; the slip detectors' history is the one it lacks for the seam's edges. With the dump, B2a reads as registered on m1: every installed record crosses the seam whole, and the window line differs, so B2a is not met as registered by one line (a near miss for the author). On the author's ruling of 2026-11-19, sections 130 to 132 pre-register the resume response of a held link (bar B6 at `release`), one evidence run of the re-attachment rule on DOY 144, and a trace-only windowed residual monitor whose scale is taken from the data. On 2026-11-20 the resume response met its bars B1 to B4 and B6 and was armed at `release` in the two primary reference files; the monitor was built measure only and met bars 2 and 4 on planted simulator runs, whose first plants were caught at once by the prior-residual check.

---

### 125.6 2026-11-19: the manager's reading of section 125.5, a diagnosis with the slip warm-up, and items for the author

The purpose of step S7 (b), replaying from a converged state in place of two hours of convergence, is not reached: until the author rules on the exact resume, no bar on integers is read on a resumed leg in place of an uninterrupted run.

**The bars as they stand** (section 125.5, not read again). B2a not read as registered; B2b met on m1 and h40 and missed on n147 (4 of 393 edges); B2c missed on the three beds; B3 met; B4 met on products and every trace and not on the whole logs (the located line order). Both keys stay off and UNVALIDATED. The manager's decision after section 125.5: step S10 next, with the dump of section 125.4 in its engine (`network.resume_chain_dump`, commit `8110c4c`), and the wind-up accumulator to the author as part of the exact-resume design (card CHA part 3, rule change 12). Saving and restoring it holds a choice (save the accumulator, or answer the restart on the held links and carries, as the restart refloat does for an accumulator's own restart) and touches the wind-up convention the reference configurations share.

**What B4 gives.** A leg is repeatable: two legs from one seed agree in products and every trace on m1 and n147. A leg-against-leg identity check from one seed is therefore possible as a quick pre-check of an identical-output step; the author's rule (three 2 h controls per step) stays the gate. An option for the author, not adopted.

**The diagnosis, no bar.** The legs of section 125.5 again, from the same seeds and snapshots, with only `network.resume_slip_warmup_ep` 5 added (the ledger's value: UNVALIDATED, "the sim leg at K=5 held 146 false seam slips, 0 acted"), L and N on m1, h40 and n147 (`evleg_s7bw_<bed>_Lw` and `_Nw`; outputs `evgrade_s7bw_<bed>.txt` and `b2c_shifts_s7bw_<bed>_Lw.txt` in `D:/GnssProcessorSupport/runs_nom/b2_s7br_grade/`). Links held by both with other integers: 328, 331 and 496 (epoch, link) pairs, against 329, 714 and 498 without the warm-up. Every remaining pair is a shift equal on both bands (one turn), so what remains is the whole-turn class alone; the 384 one-band pairs of h40 (the E25 target one cycle apart) went with the burst. B2b reads as before (the geometry-free jump test is not under the warm-up). Holds and releases unmatched between L and U: 249, 261 and 355 (without the warm-up 394, 398 and 351). The largest phase-bias distance at the second epoch on the sim beds stays (0.61 and 0.69 m), so it does not come from the burst. In L's own frame the truth audit grades 86 (m1) and 89 (h40) of the remaining pairs wrong where U's are right (G11 at S22000SIM and S30000SIM, G17 at S44000SIM, E21 at S20000SIM): beside holding restored integers a turn off, L also decides integers a turn off in its own frame.

**Items for the author** (no code change).
1. Two states the seam lacks. The accumulated phase wind-up: at the leg's first epoch 119 of 619 held links on m1 and 97 of 606 on h40 carry integers wrong in the leg's frame (section 125.5), and with the warm-up armed the differing held integers are the whole-turn class alone (above). The slip detectors' history: the geometry-free jump test's reference (4 edges at CHUR00CAN on n147, section 125.5) and the wide-lane reference (the burst at the second epoch, which `network.resume_slip_warmup_ep` holds back; the leg harness does not arm it). Both belong to the exact-resume design.
2. The truth audit grades each run in its own frame (it opens a wind-up branch at a run's first sighting), which let B2b pass on m1 and h40 while the restored integers were off in the leg's frame. A bar on integers across a resume needs the audit in both frames, or a frame-free count.
3. Whether the order of log lines belongs to "identical" (B4: the restored-signal audit lines, section 125.4).
4. The leg-against-leg pre-check of an identical-output step (above).

### 125.7 2026-11-19: B2a read as registered on m1, with the S10 engine

**The runs.** Engine `gnss_processor_cha_s10.exe` (sha256[:16] `e8a6792dd42cc884`; card CHA step S10, identical to the S7 (b) engine on its five controls), which carries the dump of section 125.4 (`network.resume_chain_dump`, commit `8110c4c`). Seed `evseed_s10_m1` (m1, 3 h, a snapshot every 240 emitted epochs) and the leg `evleg_s10_m1_Ld` from its snapshot at epoch 241 with the chain restore and the dump (119 epochs, exit 0). Compared line by line by a short script (outputs in `D:/GnssProcessorSupport/runs_nom/s10_gate/`).

**B2a as registered (a dump of the restored memory against the seed's section).** The seed's section at the snapshot epoch and the leg's dump hold 6 277 lines each; 6 276 are identical and in the same order, the 110 latched group delays among them. The one line that differs is the window line: `GW,1399163445.0012,1399174245.0012` in the seed's section and `GW,1399170675,1399174275` in the dump, the leg's processing window. The restore reads the saved window only for its run-start line and does not install it (section 125.3; filed with the latch's single owner, section 125.4). Every record the restore installs equals the seed's; whether the window line misses the bar is the manager's reading.

### 125.8 2026-11-19: the manager's reading of B2a as registered, and two items for the author

**B2a: not met as registered, by one line** (the manager's reading of section 125.7, recorded in the manager's words; it replaces the open verdict of section 125.7, whose text stands as written). "B2a as registered (the dump of the restored memory equal to the seed's section): 6 276 of 6 277 lines identical and in order, the 110 latch values among them. The one line that differs is the window line, which the restore does not install by design (the resumed run keeps its own processing window). Read to the letter the bar is missed by that line; every installed record crosses the seam whole. Recommendation to the author: accept it as met in substance, or re-register the bar without the window line. The window line belongs with the latch's single owner (the latch design's item)." A near miss: it is not for the manager or this session to call it met.

**Two items for the author, added to the list of section 125.6.**
5. B2a's near miss by the window line, with the recommendation above (accept it as met in substance, or re-register the bar without the window line), beside item 3 (log line order under "identical").
6. To item 1 (the states the seam lacks): restoring the held integers is not enough; the wind-up accumulator itself must cross the seam. With the slip warm-up armed, the resumed run also decides integers a turn off in its own frame (86 pairs on m1 and 89 on h40, section 125.6), not only restores them.

### 126. 2026-11-19: the failing two-run check of DOY 146, attributed (a note; nothing adopted or withdrawn)

**Sources.** The filed read [F4R](../record/refound/F4R_two_run_disagreement_read_2026-11-18.md); the manager's attribution `D:/GnssProcessorSupport/gates/manager_2026-11-19/ATTRIBUTION_two_run_check.md` (the hashes in its section 6); the second session's read `D:/GnssProcessorSupport/gates/nrf_reads_2026-11-18/NRF_five_days_attribution.md`. The manager and the second session (runs-reads) checked the content by byte comparison; the four groups and the residuals at GUAM00GUM below were checked again here from the runs' own files (`D:/GnssProcessorSupport/runs_nom/_rap_<run>`).

**Four groups of byte-identical runs on DOY 146** (sha256 of `logs/network_ring_fix.csv`, `products.txt` and `logs/network_solution.csv`): {`n46c_n146`, `v2_n146`, `v3_n146`}, {`n46cr_n146`, `v2r_n146`, `v3r_n146`}, {`ad_n146`, `v1_n146`}, {`adr_n146`, `v1r_n146`}. `n46c` and `n46cr` are the morning pair and `ad` and `adr` the adopted pair (section 123); `v1` is the adopted configuration without the entry check, `v2` the adopted configuration without exact epoch time, and `v3` the morning configuration with the adopted pairs list on the new engine. So `processing.exact_epoch_time` is the only one of the differences between the morning pair and the adopted pair that changes any output on DOY 146; `network.ring_fix.entry_revalidate` (no refusal on that day), the engine build and the dropped pair `E:L1X/L5X` change nothing.

**The adopted switches did not create the disagreement.** By distinct link the adopted pair disagrees on two links, GUAM00GUM with E29 and LMMF00MTQ with E06 (the check counts more, because its walk carries one link's disagreement to the links that share its receiver or satellite). At GUAM00GUM the run that departs from the other three, `ad_n146`, is the one whose held residual is centred on zero (+1.3 mm, 273 of 457 epochs positive); the other three, both morning runs among them, hold a one-signed -12 mm. A hold one cycle off leaves 12 to 18 mm on the link, below the held-link monitor's 30 mm on four consecutive epochs. On DOY 143 (`v1_n143`) the four-epoch hold through the resumed arc at BOGT00COL with G09 is the same with and without the entry check, so the release rule is its cause, not the entry check.

**Status.** Nothing in this note adopts or withdraws anything. The population count (42 of 31 795 holds of an hour or more with a one-signed mean of 8 mm or more) and the windowed monitor's estimate are the manager's offline numbers (the attribution document, sections 4 and 5), put to the author as a design question.

**A note on wording** (the manager's check of 2026-11-19, `D:/GnssProcessorSupport/gates/manager_2026-11-19/masked_pair_check.py` and its `.txt`). "Identical in full" in sections 125.3 to 125.7 and in card CHA's records of S10, S11 and milestone D means identical under the time mask of `nom_td_gate.py`: per pair, four files are equal only once their wall-clock values are removed (`network_datum_graph.csv`, `network_run_summary.csv`, `network_service_health.csv` and the run manifest), and every other file is byte-identical, with no file unexplained and none on one side only.

### 2026-11-19: the configuration of the five-day read made the main reference (card IRM; a result)

By the author's ruling of 2026-11-19 ("Yes, make it main"), the configuration of section 123's five-day read is the
main reference since card IRM: `config/golden/golden_field_refound.json` and `golden_sim_refound.json` carry it key for
key apart from run settings, with the measure-only near-null monitor (section 113.1) added and, on the simulator, the
ring fix's truth audit. The earlier float-vehicle posture is the frozen lineage `golden_{field,sim}_rf0hv18`. The ring
fix's ARMED and scoped ledger rows carry the three-part note of that date. First runs of the configuration on the
references' own beds (engine `gnss_processor_irm.exe`, sha256[:16] `f50e95d27304f1fb`, branch
`adopt/integer-reference-2026-11-19`; runs `D:/GnssProcessorSupport/runs_irm/`), 2 h each:

- Simulator (golden fixture from 00:30:15, `_rap_irm_s_2`): 240 of 240 epochs emitted; 857 links decided over rings 2 to
  7, 675 held at the end, 114 released (all at gaps), 68 to the tree, 136 re-hosts kept with their carries; truth audit
  held link-epochs right 95 810, WRONG 0, decisions WRONG 0; held share in hour 2 Galileo L1C/L5Q 91.8 %, GPS L1C/L2W
  88.7 %.
- Observed DOY 129 of 2024 (`_rap_irm_f129_2`, 58 receivers): 240 of 240 emitted; 694 decided, 552 held at the end, 83
  released (slip 17, gap 66), 59 to the tree, 108 re-hosts kept and 6 released off the integer; 62 100 held link-epochs;
  held share in hour 2 Galileo 76.0 %, GPS 71.6 %; 2 RAIM rows against 1 on the same-engine control of the frozen file.
- The built-in float user (undifferenced, the references' type, unchanged): WTZZ00DEU 2.30 and 0.32 m in hours 1 and 2
  (rising to 2.65 m at 20 to 30 min, below 0.5 m from 70 min), against 0.058 and 0.013 m on the control; SZZ000SIM 0.79
  and 0.30 m. The same configuration's built-in user read 0.65 to 1.08 m in hour 1 on the five-day days. Read: a float
  user of this posture (the phase-bias hold off) is metre-level in hour 1; the author decides the built-in user's type.

Owed: the night 6 h set of card IRM (DOY 129, 130 and 132 and the simulator, with same-engine controls of the frozen
lineage), from which the new baseline is taken.

### 127. 2026-11-19: the exact resume's decision paper filed as card XRS; line B started

The paper prepared for rule change 12 is filed as [XRS](XRS_exact_resume_2026-11-19.md), with its two reader reports, its independent review and its scripts kept under `D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/chain-exact-resume-paper/`. The review reproduced every count of sections 125.5 and 125.6 from the runs' own files (the matched null's 21 271, 21 275 and 28 960 differing held link-epochs; the restored legs' 329, 714 and 498) and corrected the paper's first reading of option B: releasing the held links alone still lets integers be decided a turn off from the restored carries, so B is no safe interim. The manager accepted the paper on 2026-11-19 as the basis of the build: line B (branch `chain/line-b-2026-11-19`, cut from the combined branch at `cf983dc`) first refactors the warm resume into ordered restore groups with output unchanged, then builds option A in the paper's stages, both new keys default off; both chain keys stay off until A lands, and arming is the author's ruling.

### 128. 2026-11-19: line B, the warm resume, the snapshot write and the fingerprint cut into groups with identical output

Package chain-resume-refactor of the day program, on branch `chain/line-b-2026-11-19` (base cf983dc, the combined branch `int/2026-11-19`; its resume, snapshot and fingerprint files are those of e7bc098). The long functions of the resume path are cut into named groups: bodies are moved verbatim, every loop and expression keeps its order, and the comments move with their code.

- `networkSnapshotFingerprint` (724 lines) is a declared table of 107 token rows (key, condition, text), walked in the old order; the twin without the switch-on tokens is kept.
- `networkTryWarmResume` (about 1 320 lines) is a short ordered list of 25 restore groups (`src/core/network/state/WarmResume.cpp`), each opening with its arming gate and then its bindings. The file's banner gives each group's key and the order dependencies found: the gate before any restore; the emitter first, all or nothing; the shared carry rows before every group that reads them; the frame before the datum, emit, hold and station groups (the loader requires the frame restore with each); the hold after the emit layer; the chain memory, canonical-signal and code-bias rows staged for the driver, which applies them before the first epoch; the dense border after the coupling regressors and the first-fold census after both; the carry blob after the producer counters; the report last.
- `networkWriteStateSnapshot` is one capture function per group in the old order (16), then the file write and the history copy.
- `networkInjectD2CarryPrior` and `networkCouplingExactPrepare` (`BorderPrior.cpp`) are named stages (`d2*`, `cxu*`).
- Six `kEntityWriters` rows replace the old resume row. The onboarding page `docs/onboarding/event_legs.md` gains the resume's order of operations.
- The new unit test `tests/network/test_snapshot_fingerprint_pins.cpp` pins the fingerprints of seven configurations, captured on the base build before any edit: the three goldens, and `_rap_ad_n146`, `_rap_m1_off37`, `_rap_h40_off` and `_rap_d24c32_n147` (skipped when absent). On the test's fixed three-station roster the BeiDou golden pins the field golden's value (`ec139101e59f8ba3`) and m1 pins h40's (`e133cb5d927f7347`). The base code behaves so; whether a resume across a change of the active systems is refused through the roster is not shown by the pins and stays open.

**Gate: pass on every path the seeds exercise.** Base engine `gnss_processor_lb0.exe` (sha256[:16] `46a6e70b3ab9c2f5`) against the refactored `gnss_processor_lb1.exe` (`ef6751c84c128c69`), both Release, runs under `D:/GnssProcessorSupport/runs_nom/`:

1. Unit and lint: the seven pins green; the pins with the resume, snapshot, border-prior, coupling, chain-memory, layout and epoch-contract suites by filter, 378 tests in 46 suites, 353 passed, 25 skipped (fixture-gated), 0 failed; `nom_epoch_lint.py` and `nom_n4_verify.py --whole` pass.
2. Three 2 h controls without a snapshot path (n147, h40, n147e; `_rap_lb0_*` against `_rap_lb1_*`): identical in full under the time mask (`nom_td_gate.py --whole --mask-time`).
3. Three 1 h resumed runs from `evseed_s7r_gs` (emitted epoch 242), `evseed_s7r_gf` (241) and `evseed_s7b_m1` (241; the chain memory restored, 4 113 entity lines, 388 pinned lives continuing): products, every trace and the leg's own snapshot identical (the write time masked). The logs hold the same lines under the gate's masks, every line but the canonical-signal audit lines in the same order and those at the same positions; only their order within the block differs. That order follows the threads of the slip scan's parallel loop (section 125.4), so it also differs between two runs of one engine.
4. The write path: a 0.25 h seed on m1 with a snapshot every 10 emitted epochs (`evseed_lb0_wp` against `evseed_lb1_wp`): identical in full, the four snapshot files identical but for the write time.

Not exercised by any seed or leg, so uncovered and not claimed: the clock-level group (LB rows), the code-bias linearization group (every seed sets `rx_code_bias_datum`), the slip warm-up, the carry blob (`state_snapshot_carry`), the standalone border restore, the smoothed-hold clone queue, and the refusal and cold-start branches; the eq34 half of the datum group is inert on the gs and gf seeds, and the hold restore on m1. Still owed: the night set (eight 2 h controls on both engines) and an independent reading of the diff hunk by hunk.

A note, no bar: the base engine against the S11 engine's runs of chain/audit (`_rap_cs11_*`) gives products and every trace identical on the three controls; the log differs by the merge (the posture lines, and the stream consumer's backlog warnings, which carry wall-clock values).

### 129. 2026-11-19: annotations from the independent review of the night commits (b7f7edd^ to e7bc098)

- The answer of section 125.5 is narrowed to: no third missing state found; 55 one-band pairs unsettled. The text of section 125.5 stands as written.
- The review confirms its finding F1 (no wind-up state in the snapshot) as rule change 12 (card [XRS](XRS_exact_resume_2026-11-19.md)): `network.resume_chain_restore` is not runnable on observed data until the exact resume lands.
- The review finds no output change on any input the engine produces today: 234 of 240 old function bodies token-equal, the other six splits or the changed walk driver, both lints passing, and 14 of 15 of the B2 numbers recomputed and matching (the m1 truth audit, 221 and 108, not recomputed). Its findings and their failure scenarios: `D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/tooling-review-chain-night/`.

### 130. 2026-11-19: a held link answers its own tracking break (`network.ring_fix.resume_response`; pre-registered)

Filed on `chain/line-b-2026-11-19` from the line A package held-resume-release (`nrf_preregistration_draft.md`; its paper `paper_decision3_release_or_revalidate.md` and its evidence `evidence_held_resumptions.md` in `D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/held-resume-release/`). Paths and line numbers are at e7bc098.

**Why.** The release step of a held ring-fix link has no input for a gap resumption of the link's own (station, satellite) (`include/network/ar/RingFix.hpp:583-592`; `src/core/network/ar/RingFix.cpp:583-623`; `src/core/network/ar/RingFixHold.cpp:308-345`, all at e7bc098). The resumption records of the epoch are final after PASS A (`src/core/network/driver/ApplicationNetworkEpoch.cpp:284`) and cleared each epoch (`include/network/slip/RawObservables.hpp:409-419`); the hold epoch runs later (`:388`) and reads them only for a tree entry. On the five adopted days (`runs_nom/_rap_ad_n143` to `_rap_ad_n147`) five holds were folded across a break: BOGT00COL G09 on DOY 143 (shifted, its carry erased by a chain re-host 30 s before the return, four epochs at -0.65 to -0.57 m, monitor release), POTS00DEU G13 on DOY 144 (unmeasured, -27.36 m), and KIT300UZB G29 (DOY 145, +5.04 m; DOY 147, -0.010 and -0.033 m) and G10 (DOY 147, -0.137 and -0.133 m), all unmeasured and released 30 to 60 s later by other detectors. Every other shifted or loss-of-lock resumption on a held link was released at the return. A frozen-engine bed of an unmeasurable break with a one-cycle return shift held each shifted link one epoch wrong. This is BRV decision 3 (BRV, decisions, item 3; corrections, item 3 of the decisions after correction); the paper and its evidence are filed with this section's source. Invariant 1: a held integer is a stiff time-linked quantity whose break event has no wired response. IRD increment: IRD-6, with IRD-4's per-row bars.

**The key.** `network.ring_fix.resume_response`, a string: `off` (default; the build is byte-identical), `release`, `revalidate`. Inert without `network.ring_fix.enable`. An unknown value is refused by the loader with the list of values. Ledger row UNVALIDATED with the build; posture row as the loader convention of the build requires.

**Which resumptions respond (frozen here).** For a held link (station s, satellite k, signals a and b), from the epoch's `qc.epoch.resumptions` and `qc.epoch.sig_resumptions`, considering only records of (s, k) that name a or b; first match wins:
- shifted: a row record with `shifted`, or a signal record of a or b with `shifted`: respond;
- unmeasured: a row record not `measurable`, or measured on a pair that is not {a, b}; or a signal record of a or b not `measurable`: respond;
- clean: a row record measured on {a, b} and not shifted, or a measured unshifted signal record of a or b: the hold is kept under both values;
- none: nothing.
A record of (s, k) that names neither a nor b does not respond. A link the release step releases this epoch for chain or slip keeps that cause. Carry state (bridged or fresh), loss of lock and dGF are written in the event detail and split in the counters; they are not classes.

**`release`.** A responding held link is released at the resumption epoch, before PASS B, with the new cause `resume` (`RingFixRelease::Resume`, `kRingFixReleaseCount` 6); the carried entries are re-opened as for every release. Event RINGFIX `resume_release` (station, satellite, signals, held integers, class, carry, LLI, dGF, gap epochs).

**`revalidate`.** A responding held link is suspended at the resumption epoch (not held, carried entries re-opened) with a crossing record on the link (time, class, the held integers, an agreement count). The record is cleared as `revalidated` when the link's float accepts the record's integers at 10 consecutive decision epochs (the frozen persistence count of the ring fix), the link being held again at those integers; as `contradicted` when the float decides other integers; as `expired` past 600 s (the frozen decision window); as `ended` at a slip latch, a chain event touching the link or a second resumption. Constants frozen here: test level 1e-3 (chi-square, two degrees of freedom, on the float's formal covariance times `sigma_scale` squared), persistence 10, expiry 600 s. While a record is open, the cut entry check and keep-carry judge a later entry of the link with the record as resumed and unchecked (GapUnchecked). The record is written in the ring fix's memory section only when open; no second carried state exists. Events `resume_suspend` and `resume_clear` (with the verdict).

**Counters and lines.** `resume_released`, `resume_suspended`, `resume_revalidated`, `resume_contradicted`, `resume_expired`, `resume_ended`, `resume_kept_clean`, by class and carry; a run-start line when not off; run-end lines printed only when not off, with the closures `resume_released` = `released[resume]` and `resume_suspended` = revalidated + contradicted + expired + ended + open at the end.

**Bars** (fixed before any build run).
- B1. Off identity. With the key absent: the three 2 h controls n147, f40sc and h40 identical in full under `scripts/nom_td_gate.py --whole --mask-time` against the line A exe before this build; the unit suites `RingFix*`, `ChainNamedRows`, `NetworkChainFrame`, `NetworkArcGraphOracle` and the new `RingFixResume` pass on the Debug build; `scripts/nom_epoch_lint.py` and `scripts/nom_n4_verify.py` pass.
- B2. Unit cases (`tests/network/test_ring_fix_resume.cpp`): each class under each value; off identity of `ringFixReleaseStep`; the re-base route of DOY 143 (held, `rebase_stamps` set, carry missing, not latched, resumed shifted) released (or suspended) at the resumption; a link not present at the resumption answered the same way; a later entry of a suspended link judged GapUnchecked; the record survives the memory write and read, and no record line exists when none is open; clean classes keep the hold; an off-pair record does not respond; the five observed cases as fixtures.
- B3. The two frozen-engine beds of the paper, re-run on the new exe (configurations `runs_nom/_rap_day19_hlb_a` and `_rap_day19_hlb_b`, 2 h from 00:30:15), at `release` and `revalidate` against the key-off run of the same exe (6 runs): every epoch emitted; truth-wrong held link-epochs (`nrf_audit.py --fixture` with the bed's truth overlay) 0 on bed (a) at both values; the frame-free count (`PYTHONHASHSEED=0`, with the injected deltas) not above the key-off run; GPS and Galileo held link-epochs not below 98 % of the key-off run; S06000SIM E11 and E10 answered at 265200 and 266400 with their event rows. Reported beside it: E12 and E36 answered at their returns (unshifted, the cost), and bed (b), predicted identical to its key-off run in products and ring-fix trace (every held E10 link is released by the returning cut before the resume step).
- B4. Three injected simulator events (`network.inject` outage, `delta_cycles` for the return shift, `flag_lli` false), 2 h from 00:30:15 on m1 at links the key-off run holds, each at `release` and `revalidate` against a key-off run of the same exe (9 runs): (a) a 300 s outage of one (station, satellite) with a one-cycle L1 return shift; (b) a one-epoch L2 outage with a return shift; (c) a satellite-wide 300 s gap with a kept re-host across it. Bars: those of section 122 (truth-wrong held link-epochs not above the key-off run, frame-free count not above, every epoch emitted, held link-epochs not below 98 %), plus the injected link released or suspended at the injected return with its event row and never folded above 0.03 m. The truth audit is given the injected cycles (a truth overlay) or excludes the injected link, stated in the report.
- B5. Night, the five simulator beds m1, d1, d31, h1 and h40 at 6 h from 00:30:15, at `release` (and `revalidate` unless the author has ruled), each against a key-off run of the same frozen exe (15 or 10 runs): the bars of B4 and 0 holds folded above 0.03 m at a shifted or unmeasured resumption.
- B6. Night, after the ruling, the six adopted days (`_rap_ad_n143` to `_rap_ad_n147`, `_rap_adr_n146`, arm.json with only the key and the run paths changed) at the ruled value, 24 h, each against a key-off run of the same frozen exe (12 runs; off identity to the adopted runs' exe is not expected, since the line A exe carries later network commits): F1 every epoch emitted; F2 not below the key-off run by more than 0.5 point in any hour from 3, by system; BOGT00COL G09 answered at TOW 572730 on DOY 143, POTS00DEU G13 at 18930 on DOY 144, KIT300UZB G29 at 119220 on DOY 145, KIT300UZB G29 at 291450 and G10 at 309600 on DOY 147; 0 holds folded above 0.03 m at a shifted or unmeasured resumption of the link's signals; products identical to the key-off run before the first resume event of each run (and in full on a run with none). Truth-free check: holds crossing a shifted or unmeasured resumption of their own signals, 0 under `release`; under `revalidate` each has a `resume_suspend` and a `resume_clear` row and none is held again without `revalidated`.
- Night set 1 (a measurement, no pass or fail): the two break shapes of the frozen-engine beds placed by the same rules on the configurations of `_rap_cs11_h40` and `_rap_mD_d1`, 6 h from 00:30:15, on the frozen `gnss_processor_cha_s11.exe` with no key (4 runs). Reported: held integers crossing a break wrong by the truth audit, each with its release cause and delay. Its condition, a wrong crossing in a 2 h day bed, was met on 2026-11-19 (bed (a), two links one epoch each).

**Who runs what.** The build, B1, B2 and the 10-epoch smoke: the line A session (`chain/line-a-2026-11-19`). B3 and B4: the line A session on a frozen copy of its exe, at most three short engines by day through the slot wrapper. B5, B6 and night set 1: the night queue, on frozen exes named with their sha256.

**Reading prepared.** B1 to B4 met: the key goes to the night runs; the ledger row stays UNVALIDATED until B5 and B6 are read. B6 met at the ruled value: the author decides the ledger verdict. A bar of B3 to B6 missed: the key stays off, the miss is written here with its runs, and the author decides.

**The ruling.** The author, 2026-11-19 at about 15:35, in reply to the manager's list of decisions, where this item read "Value at a tracking break: release": "Yes, let's proceed with your recomomendations on the decisions." The package's three recommendations therefore stand: this section is filed before any data run of the key; B6's value is `release`; bed (b) of B3 stays a reported prediction, not a bar (a miss is a finding about the order of the hold stages, not of the key). `revalidate` is built and graded on the simulator only (B3 to B5); no observed day runs it.

**State at filing.** The build is the line A builder's commit e728e00 on `chain/line-a-2026-11-19` (unmerged). With the key off, six 2 h controls are identical (the builder's report, not re-graded here). No data run of the key exists: the builder's smoke and day beds wait for this section.

### 131. 2026-11-19: G17 after its return on DOY 144, read; one evidence run of the re-attachment rule pre-registered (`_rap_nt19_reattach_n144`)

From the read-only package held-returning-satellite-read (2026-11-19): `held_returning_read.md` and `night_run_spec.json` with their tables and scripts, copied to `D:/GnssProcessorSupport/gates/day_program_2026-11-19/results/held_returning/`. Runs read: `_rap_ad_n143` to `_rap_ad_n147` and `_rap_adr_n146`, on `gnss_processor_cha_fp1.exe` (sha256[:16] `da240107c091aa8c`, its build stamp equal to the run manifest's); code at e7bc098. IRD-4, the returning row and the rehost row. The read sets no bar.

**What happened to G17.** G17 returned on DOY 144 at TOW 75930 after a 1 950 s gap at every station. Its level was pinned on young links while every G17 arc was fresh: the returning row at CUSV00THA, then at 76710 a re-host onto the unheld, outlying, 690 s old KIT300UZB link, forced by a loss-of-lock flag with no step (two searches find no step in the wide lane or the geometry-free combination). The converged G17 links then agreed with each other at a common offset of -0.35 to -0.47 cycle from that level (circular median -0.472 at 78450 on 6 links, -0.354 at 86370 on 23, circular sd 0.036 to 0.058), while the other GPS satellites' links at the same stations and epochs sat at a median |d_if| of 0.000; the offset belongs to G17, equal in cycles on L1 and L2. The 0.20 fraction bound refused the consensus (5 937 fraction refusals from 75930). Only a link at the edge of the bound passed: BOR100POL, |d_if| 0.189, decided at 81300, which tied the satellite at the level its other links disagree with; flag 1 from 81600. G17 was first decided 5 370 s after its return. In hour 24 the GPS held share is 78.83 % with G17 and 83.96 % without it (bar F2 of the five-day read fails on that hour). The case is 1 of 172 returns not re-held within 3 600 s; the same shape without a return occurs on G04 on DOY 146, which recovers after about 3 h. The independent skeptic's qualifications: "the re-host caused it" is not shown (the links already drifted in the CUSV00THA frame), and the offset decays by about 0.05 cycle per hour, unexplained.

**The re-attachment rule, emulated offline.** Had `network.ring_fix.reattach` been armed, it would have requested a move of G17's tree edge at TOW 78450 (2 520 s after the return; the 1 800 s dwell ended at 77730, no link had sigma_if <= 0.10 until 78210, the wide-lane spread exceeded 0.10 until 78420) onto MBAR00UGA in both bands (6 candidates, med_if -0.472, sd_if 0.036, med_wl +0.023, sd_wl 0.092); the independent check reproduced the epoch, the station and the statistics. By the rule's own timing the earliest decision after the move is 79350, 6 epochs inside the 60 min bar (which ends at 79530); a flag within 60 min is not reachable (about 79650). After the BOR100POL tie at 81300 the rule cannot act (`RingFixDecide.cpp:180-183` skips a tied satellite). The rule admits candidates only at sigma_if <= 0.10, while 68.9 % of the decided holds of `_rap_ad_n144` lie above 0.10 under the gate in force; that bound delayed the move by at least 480 s. The emulation cannot see the feedback after a move: only a run shows it.

**The lost satellite-samples of bar F3.** Galileo 4, 3, 5, 6, 2 and 7 over the six runs (27 of 8 244 tree satellite-samples, 0.33 %): short waits after E14 and E18 return (17) or in the first 1.5 h of a run (10), none of the earlier configuration's long re-host shape. GPS 14, 18, 14, 23, 18 and 13 (100 of 8 717, 1.15 %), mostly returns, with the long shape twice: G17 on DOY 144 (3 samples) and G04 on DOY 146 (5 and 6 samples, after a Birth row).

**The ruling.** Section 102.3 found the rule a delay on the simulator (bars S2 and S4 failed) and left its use on observed data to the author. The author, 2026-11-19 at about 15:35, in reply to the manager's list where the item read "Run the re-attachment rule once on observed day 144 tonight, as evidence only" (recommendation "yes, one run"): "Yes, let's proceed with your recomomendations on the decisions." The run is evidence: nothing is adopted by it, and a ledger row follows only a ruling. The package's decisions on the rule's sigma bound under gate odds and on a satellite tied by a single disagreeing hold are not ruled here.

**The run (pre-registered).** `_rap_nt19_reattach_n144` (tag `nt19_reattach_n144`), 24 h, count 1, Release. Base `D:/GnssProcessorSupport/runs_nom/_rap_ad_n144/arm.json` with exactly two keys changed: `network.ring_fix.reattach` true and `network.product_output_path` re-pointed at the new folder's `products.txt` (the loader accepts the key because `network.ring_fix.settled_flags` is true in the base). Exe `gnss_processor_cha_fp1.exe` (`da240107c091aa8c`), run as the renamed copy `gnss_processor_cha_fp1_nt19.exe`; control `_rap_ad_n144` (same exe, the rule off). The make and launch lines are those of `night_run_spec.json`.

Bars of the record, as written:
1. "G17 held again within 60 min of its return"
2. "GPS held share in hour 24 at least 80 %"
3. "no new one-signed hold, scanner D:/GnssProcessorSupport/gates/manager_2026-11-19/onesigned_scan.py against _rap_ad_n144"
4. "no new wrong user hold against _rap_ad_n144, users of DOY 144 re-derived on the new stream with D:/GnssProcessorSupport/runs_nom/_irdu_grades/scripts_s20/derive_users_days.py and graded with user_fix_vs_float_free2.py"

Supplementary bar, measured from the return: G17 decided and held earlier than in the control, that is before TOW 81300 (5 370 s after the return), and flagged before TOW 81600 (5 670 s after the return). The record's 60 min bar stays as written; the supplementary bar separates a rule that shortens the wait from one that does not when the 60 min bar is missed by a few epochs.

Baselines (the control, rule off): return 75930, the 60 min bar ending at 79530; first decision 81300 (BOR100POL), first held row 81330, flag from 81600; hour-24 GPS held share 37 084 of 47 042 rows (78.83 %; 550 more held rows reach 80 %; G17 has 2 905 rows in that hour); users: REYK alone holds G17 after 81600, 34 held epochs, 0 wrong by the float check (6 right, 28 unverified).

Reported beside the bars: every RINGFIX `untied_move`, `untied_applied` and `untied_missed` row of `network_events.csv`; the run-end untied counters (requests, held_off, few, spread, converging, flagged after); the time from G17's return to its first decided hold, its first held row and its first flag; the predicted moves for `_rap_ad_n144` (G15 at TOW 1800 onto CPVG00CPV, G17 at 78450 onto MBAR00UGA) against the moves made, with the epoch and station of each; the cost case of section 102.3 (G15's unflagged epochs in hour 1 against `_rap_ad_n144`); G17's converged links relative to the new pinned level after the move (its fraction refusals from the move to the end of the day against 5 937 from 75930 in the control).

Unproven until the run: every effect of a move (whether the MBAR00UGA arc is taken, how the floats move after the re-level, whether persistence runs unbroken), and whether the run reaches G17's state at 78450 after the predicted G15 move at 1800, from which the run is a different run.

### 132. 2026-11-19: a windowed residual monitor for held links, trace only (`network.ring_fix.window_monitor`; pre-registered)

From the package held-signature-evidence: `PAPER_item5_held_window_monitor.md` (section 3, decisions 1 and 2) and `ADDENDUM_scaled_statistic_2026-11-19.md` (the ruling, the statistic in the data's own scale, the bars of the measure-only stage, the order of work), in `D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/held-signature-evidence/`. Numbers are the packages' offline replays. Paths and line numbers are at e7bc098. IRD increment: IRD-6 (chord rungs 0 wrong, the adversarial arm reported).

**Why.** The present monitor releases a held link after 4 consecutive held epochs with a post-fit ionosphere-free residual above 0.03 m (`include/network/ar/RingFix.hpp:81`, `:83`; `src/core/network/ar/RingFix.cpp:472-477`). Holds of an hour or more sit at 8 to 16 mm, one-signed, under that bound. Over eight observed day runs, 42 of the 31 795 holds of an hour or more are signature holds (an hour or more, |mean residual| at least 8 mm, at least 90 % of the rows on one side; 0.13 %; 21 GPS and 21 Galileo), and two of them carry direct evidence of a one-cycle error. The author's rule of 2026-11-19 on data-driven values: "I thought it is better to use data-driven values rather than fixed threshold when possible. Imagine something like a smartphone network, would 30 mm fixed threshold be reasonable?"

**The ruling.** The author, 2026-11-19, in reply to the report that listed this decision as "Trace-only residual test for held links: yes, with the scale taken from the data": "Yes, let's proceed with your recomomendations on the decisions." Its scope: decisions 1 and 2 of the paper (pre-register the monitor, measure-only first; the planted wrong hold before any release is armed), with the statistic stated below. Decisions 3, 4 and 5 of the paper are followed as working conventions (the paper's class 4 is named "quiet, undetermined"; `share_sta` and `share_sat` are event columns, not a release qualifier; the decision-time stationarity condition is not built); decision 6 is not ruled.

**The key.** `network.ring_fix.window_monitor`, default off and identical when off. With the key on nothing is released and no product changes. The release stage is a later ruling: no level is frozen for release here.

**The scaled statistic.** For a held link with post-fit ionosphere-free residual r(t), in class c:
- Class c: receiver class x system x signal pair. The receiver class is the station itself where the station has enough other held links of that system and pair; otherwise the network-wide set of the same receiver type, system and pair.
- Scale s_c: the robust scale (1.4826 x median absolute deviation about the median) of the held residuals of the other links of the class over the trailing hour. The tested link's own residuals never enter its scale (a scale from the tested link grows with the fault: 28 of 38 one-signed holds caught with it, 38 of 38 with the class scale).
- Reference d_c: half the residual a one-cycle error on both bands leaves on a held link of the pair (13.5 to 15.9 mm for the full footprint on observed data); the planted runs measure it on each bed. It is a physical size of the error sought, not a noise level.
- Sums: S+(t) = max(0, S+(t-1) + r(t)/s_c - d_c/s_c) and S-(t) = max(0, S-(t-1) - r(t)/s_c - d_c/s_c), both zero at the hold's first row; a held row without a residual leaves both unchanged.
- Trip: S+ or S- above h, where h is set by a stated false-trip rate on clean held link-hours, not by a size. The reference rate is today's rule's own, 1.21 per 1 000 clean held link-hours, which gives h = 105 with the network class and 95 with the station class in the offline replay.
- Episode: it opens at the first trip and closes at the hold's end or at a release. While any link of the class is in an episode, s_c is frozen at its value before the episode.
- No floor: there is no fixed minimum for s_c. Where neither the station class nor the network class has enough other held links, the statistic is not formed for that link and epoch, and the link-epoch is counted as unscaled.
By construction the false-trip rate does not depend on the class's noise: a noisier class reaches the trip later, because each epoch carries less evidence, not more often.

**What the measure-only stage records.** For every held link and epoch: (1) the scaled sums with both class choices (station, network), the scale used and the count of links behind it; (2) the paper's fixed forms beside them: O2b (k = 0.006 m and h = 0.4 m-epochs, one statistic per sign, both reset to 0 at the hold's first row, a row without a residual leaving them unchanged), O1 (a 40 min window, |mean| at least 0.008 m, evaluated once the window lies inside the hold and holds at least 80 % of its 30 s epochs) and O2a (40 consecutive held epochs of one sign with |r| at least 0.004 m), with today's rule O0 (0.03 m on 4 consecutive held epochs) unchanged; (3) `share_sta` and `share_sat`. One RINGFIX event at the open and one at the close of each episode, per form, with station, satellite, signals, held integers, the statistic's value, the scale and the two shares; counters at both edges per form, and the unscaled count. Unit tests in `tests/network/test_ring_fix_window.cpp`. The paper's O2b is 2.5 scales and 167 scale-epochs at the observed scale of 2.39 mm: the fixed and the scaled forms agree in shape.

**Bars of the measure-only stage.**
1. Key off: identical to the build before the key, in full.
2. Key on: every output identical to the key-off control on the same engine except the monitor's own events, trace columns and counters.
3. Counters non-zero at both edges on every observed day.
4. Engine equals offline: the offline replay of the scaled statistic on the run's own `network_ring_fix.csv` reproduces the engine's episodes of every form, episode by episode. This bar carries no tuned number.
5. Reported, not barred: episodes per day of each form against the offline references (O2b 11 [4, 31] other releases a day plus the day's one-signed holds; the scaled form at its matched rate), the delay to the first trip on the one-signed holds, the unscaled share.

**Order of work.**
1. The resume response of section 130 (chain line A) comes first.
2. The simulation-only `ring_fix_plant` injector and the four planted 2 h runs of the paper (beds m1 and h40, from 00:30:15, monitor off and measure-only): they give the residual a planted one-cycle hold leaves on each pair and bed (d_c) and the delay of every form.
3. The measure-only stage on the six adopted days at night, with same-engine key-off controls (the paper's set N2), read against the bars above.
4. A release stage is a later ruling; its bars are proposed from the planted runs and the night runs, in the statistic's own units. The paper's two release bars (each planted hold caught within 60 min on both beds; false trips on right segments within the clean rate's upper Wilson bound of 0.1 %) are a proposal, not frozen.

**Cautions carried.** The simulator cannot calibrate the catch: its natural wrong holds are near-null (3, 4) errors that leave 0.04 to 0.20 mm on the held link, which belong to the near-null monitor. Levels do not transfer between beds (the simulator's scale 4.9 mm with serial correlation 0.63 to 0.86, the observed one 2.4 mm with 0.37 to 0.42); the scaled form uses each bed's own scale, and the false-trip rate that sets h is read on observed clean holds. The catch of the one-signed holds rests on holds selected as steady offsets: of the 42, 4 links point to an integer error and 25 to a station effect. A hold that ends as a tree edge leaves the held set without a release (9 of the 42) and cannot be watched further. The single 27 m residual on POTS00DEU G13 at TOW 18930 on DOY 144 is a tracking break (a held link kept through an unmeasured return), answered by the resume response of section 130 at its own epoch, not by this monitor.

### 130.1 2026-11-20: the resume response read on its bars, and armed at `release` in the two primary reference files

Line A, on the builder's commit e728e00 (exe `gnss_processor_linea2.exe`, sha256 21d421f8cbbcf664...). Checkpoints 3 and
5 of the package (`D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/held-resume-release/`), with the
placements and predictions written before the runs (`stage6/PLACEMENTS.md`).

- B1 (off identity): DOY 143 at 2 h identical in full to the line A exe before the key; five simulator and observed
  controls identical apart from the announced lines. B2: suite `RingFixResume` (16 cases) passes.
- B3, the two frozen-engine break beds, at both values against the key-off run of the same exe: bed (a), truth-wrong
  held link-epochs 2 with the key off (S06000SIM E11 and E10, one epoch each, 0.121 and 0.166 m), 0 at `release` and
  `revalidate`; every epoch emitted; frame-free count 0; held link-epochs 99.86 % or more. E11 and E10 answered at 265200
  and 266400 with their rows; the unshifted E12 and E36 released too (the cost); under `revalidate` E11 and E10 cleared
  as contradicted at their true integers, E36 held again, E12 expired. Bed (b) identical to its key-off run, as
  predicted.
- B4, three injected events on m1 (placed on Galileo links so that the frame-free count sees them): identical at all
  three values. Each injected return was measured and released at its epoch by a cause the response yields to: (a) a
  300 s gap with a one-cycle E1 shift, cause slip; (b) one epoch of E5a lost with a shift, cause slip (the signal tier);
  (c) a satellite-wide gap of E08, cause chain (the returning row; the kept re-host of the key-off run did not recur).
  Bars met; the response not driven (0 releases by it).
- A bed of the observed POTS00DEU G13 shape, asked by the manager: one epoch with L1C only (L2W and the simulator's L5Q
  dropped; the observed G13 has no L5), one epoch absent, the return with +1 cycle on L1C and no LLI, on S02000SIM G01.
  The return is unmeasured and bridged (`unmeas_bridged`, gap of one epoch). Key off: held at (3, -32) for four epochs at
  0.318 to 0.196 m, released by the monitor, decided again at (3, -33) (wrong) and held four more epochs: truth-wrong
  held link-epochs 8, wrong decisions 1. `release`: released at 264810, cause resume (counter 1), decided at (4, -32) at
  265680; wrong 0. `revalidate`: suspended at 264810, contradicted at (4, -32); never held again at (3, -32); wrong 0.
  A first attempt that dropped L2W alone was measured (the row before the gap formed on L1C/L5Q) and is recorded in the
  package.
- B6, the six adopted days at `release` against same-engine key-off runs (24 h, the night of 2026-11-19, read by the run
  session): every epoch emitted; held share identical by hour (largest difference +0.1 point); the five recorded cases
  released at their return epochs with cause resume; no hold folded above 0.03 m at a shifted or unmeasured resumption,
  against 5 with the key off; products identical to the key-off run before the first resume event on every day (on
  DOY 146 the only resume, WROC00POL E18 at 223470, takes the place of a refloat release of the same epoch, and the
  products are identical in full).
- B5, the 6 h simulator beds, is not run.

**The arming (2026-11-20).** On the author's arm rule (arm improvements unless measured harmful) and his words of
2026-11-19 22:10 "this is a good time for adoption, merge, and longer runs", announced to him twice by the manager on
2026-11-20 without objection, and on the manager's instruction: `network.ring_fix.resume_response` = `release` pinned
on `golden_sim_refound` and `golden_field_refound` through the ledger (RECOMMEND_SCOPED, regime the ring fix on, not
fingerprinted) and `make_golden_config.py`; the frozen rf0hv18 lineages and the BeiDou lineage keep it off and are left
byte for byte. Branch `chain/line-a-2026-11-19`: the combined reference merged in (5147f15), the arming 058aabd. The
verifies pass; the suites that read the reference files pass (245 cases), and the snapshot fingerprint pins do not move
(the key is not a token of the fingerprint). A 10-epoch smoke of each regenerated file runs (the run-start line names
the response).

The two 6 h reference runs on the armed files, exe `gnss_processor_linea4.exe` (sha256 a4689d81ab62ecbb..., the combined
engine with the window monitor and the planted hold, both off), against the baseline candidates `runs_irm/_rap_n20_f129_6`
and `_rap_n20_s_6` (combined engine): products, every trace and the manifest identical on both beds. In these six hours
no held link meets a shifted or unmeasured resumption of its own (observed day 129: 12 clean resumptions kept, 0
released; simulator: none), so the armed key changes no output there. The logs differ only by the posture count (one more
scoped knob armed), the response's run-start and run-end lines, the release line's `resume 0`, and timing lines.

To check: `revalidate` on the simulator only; B5 at night; days beyond the six adopted ones.

### 132.1 2026-11-20: the planted hold and the measure-only window monitor built; bars 2 and 4 on the planted runs

Line A, commit 3681795 (exe `gnss_processor_linea3.exe`, sha256 bbff461fd97731ad...). Checkpoint 4 of the package.

- `network.inject` kind `ring_fix_plant` (simulation only): both bands' held integers of a held link moved by a whole
  number of cycles, in a hold stage after every release and before the hold table. `network.ring_fix.window_monitor`:
  the scaled sums with the station and network receiver classes, O2b, O1, O2a, share_sta and share_sat on every held
  link, `network_ring_fix_window.csv`, RINGFIX window_open and window_close, counters at both edges. Suites
  `RingFixPlant` and `RingFixWindow`.
- Off identity: two 2 h controls (m1, h40) identical in full to the line A exe.
- Planted runs (m1 and h40, plants on S05000SIM G26 and S09000SIM E11, (+1, +1) at 263700): bar 2 met (with the key on,
  products, every trace and the manifest identical to the key-off run but the monitor's own file and two log lines); bar
  4 met (the offline replay of `network_ring_fix.csv` reproduces every row of the monitor's file, 91 368 and 89 449 rows,
  16 columns, and every episode; 0 = 0). The clean holds stay under every trip level (largest scaled sum 50 against 95 and
  105; O2b 0.28 m-epochs against 0.4; O2a 23 epochs against 40; O1 7 mm against 8 mm; network scale 4.6 mm).
- The plants did not persist: a whole-cycle step against an established hold disagrees with the carried ambiguity; the
  prior-residual check read 2 700 to 3 800 against 4 in the plant epoch, the RAIM attribution refloated both arcs, and
  both links were released one epoch later. No reference d and no delay came from them. The manager chose shape 1 (the
  link's carried entries move with the integers); its runs are reported below when read.

The results from 2026-11-20 on are in [part 20](NRF_network_ring_fixing_2026-11-14_part20.md).

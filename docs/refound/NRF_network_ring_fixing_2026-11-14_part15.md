# NRF part 15: the single Galileo network with the slip layer on the held pair, and the engine build (sections 107.9 onwards of card NRF)

**STATUS**

- **id:** NRF-part15
- **topic:** network-vehicle
- **status:** open
- **summary:** Sections 107.9 onwards of card NRF (2026-11-17), continued from [part 14](NRF_network_ring_fixing_2026-11-14_part14.md). Section 110.1: bar 7 of section 110 (no link held through a slip that today's run flags) loses the band-scope events from its flag list before any result, after the tool's self-check showed them diagnostic; with the rest, each of today's runs releases every link it flags. Section 107.9 (the managing session's question B): section 107's single-network runs hold 334, 420 and 593 of today's slip flags through, every one at a combined-mode receiver (mostly the Antarctic receivers, MBAR00UGA, GCGO00USA and YKRO00CIV), on 4.5 to 6.4 % of those receivers' held link-epochs; none of the six links of the two-run audit is held through a flag itself; 3 of MBAR00UGA's 11 Galileo RAIM rows on each day sit on such a link. The pilot receivers' bar 1, bar 2 and the near-null column stand; the combined-mode share, bars 3 and 4 and the users' accuracy are withdrawn until section 110. Section 107.10 (question D): a wet-delay change over 5 cm within an hour is rare outside the ULAB event (0.3 to 1.0 % of station-hours in today's runs, at the same few stations), and the event crosses it 7.5 min after the slip. Section 108.2: the managing session's reading of the guard's failure and its decision on section 91.3. Section 109.1: the build of section 109 approved with six amendments.
- **source:** [part 14](NRF_network_ring_fixing_2026-11-14_part14.md) sections 107.2 to 110; the managing session's messages of 2026-11-17 night (decisions A to D, the approval of section 109 with amendments, the reading of section 108.1).

**Outcome.** Section 107's single network kept links held through hundreds of slips that today's run caught, all at the combined-mode receivers whose slip checks watched other bands, so several of its failures cannot be read as the single network's until the re-run of section 110, now running with the slip checks on the held pair. The two-run audit's six links were not among them. A one-hour wet-delay change over 5 cm is rare enough to be a candidate instrument. The build of section 109 is approved with amendments: a declared table, two keys, flags and counters on both signal types, a declared state treatment, a slip bar on the simulator, and no term.

### 110.1 2026-11-17: bar 7's flags amended before any result

Before any result of section 110's arms was read, the tool's self-check (`slip_hold_check.py` with today's DOY 147 run as both the flagging and the tested run) counted 302 flags that today's run itself held through, every one a T0_band_scope `common_band` all event. Such an event follows a Melbourne-Wübbena flag that today's configuration logs as diagnostic and does not act on (its basis reads "mw(L1X+L5X) flag; iftd silent"), so it is not a slip action and leaves bar 7's list. With the remaining actions each of today's three runs releases every link it flags while holding it: 180 (`d24_n146`), 221 (`d24r_n146`) and 257 (`d24_n147`) flags on held links, none held through. The flag list of bar 7 is otherwise as section 110 states it.

### 107.9 2026-11-17: how far the relabel's slip blind spot reaches into section 107's numbers (the managing session's question B)

**Method.** `slip_hold_check.py` (bar 7's rule with section 110.1's list), today's runs flagging and section 107's runs tested; `slip_hold_spans.py` for the span each link stays held after a held-through flag (merged per link), the Galileo RAIM rows that fall inside such spans, and the links section 107.2 names. Outputs in `_irdu_grades/grades_family_if/`: `slip_hold_s1*_{all,x28}.txt`, `slip_hold_s1*.jsonl`, `slip_spans_s1*.txt`.

**Results.**
- Held through: `s1_n146` 334 of today's flags, `s1r_n146` 420, `s1_n147` 593, two of them into the tree as known steps (ULAB00MNG–E14 at the return traced in section 107.6, and one at CUSV00THA). Every one is at a combined-mode receiver; no pilot receiver's link is held through a flag. By receiver (the three runs): CAS100ATA 95, 101, 192; SCTB00ATA 63, 95, 197; SYOG00ATA 60, 61, 82; MBAR00UGA 69, 113, 49; GCGO00USA 21, 23, 21; YKRO00CIV 12, 11, 30; 1 to 6 at each of 13 others.
- Spans: after those flags the links stay held for 18 290, 20 103 and 26 675 link-epochs, 4.5, 4.8 and 6.4 % of the combined-mode receivers' held link-epochs (by hour 2 to 7 %, 1 to 9 % and 1 to 12 %, the most in hours 5 to 9 of DOY 147).
- The six links of the two-run audit (MBAR00UGA–E05, BOGT00COL–E33, MRC100USA–E19, LMMF00MTQ–E33, MBAR00UGA–E25, KOKV00USA–E33): none is held through a flag on itself in either run of DOY 146. MBAR00UGA's other links are, in hours 17 and 19 to 22 (the audit's failures fall 86 % in hours 16 to 23), and other links of E33 in hours 9, 10 and 21.
- Galileo RAIM rows on a link inside a held-through span: 3 of 14 in `s1_n146`, 5 of 23 in `s1r_n146`, 4 of 17 in `s1_n147`; MBAR00UGA's 11 rows on each day include 3 such.

**Which of section 107's numbers stand.** Stand: bar 1 over the pilot receivers' links (none held through a flag); bar 2 as measured; bar 6's near-null column (the steps the network holds on the pilot receivers' own links, none held through a flag); the traces of sections 107.4 to 107.8 (section 107.6 is itself the held-through event into the tree; sections 107.7 and 107.8 rest on today's runs and the observation files). Withdrawn until section 110: bar 1's share over the combined-mode receivers' links; bar 3 (GPS lower in hours 9 to 18 of DOY 147, the ULAB event); bar 4 (the Galileo RAIM rows are all at combined-mode receivers, MBAR00UGA's among the receivers with the most held-through flags); bar 6's accuracy (HKSL from the ULAB event; the Leica receivers' and JPLM's millimetres on DOY 146 untested). Bar 5: its six links are not held through a flag themselves, so the root-dependent step is not shown to be the blind spot's; section 110's bar 5 measures it again.

### 107.10 2026-11-17: wet-delay changes within an hour (the managing session's question D, a count)

**Method.** `zwd_jumps.py`: per station |ZWD(t) − ZWD(t − 1 h)| from `network_zwd.csv`, from hour 2 (the hour back then lies outside the ignition hour); a station-hour counts once if any epoch in it exceeds 5 cm. Output `zwd_jumps.txt`.

**Results.** Today's runs: 12 and 12 station-hours on DOY 146 (`d24_n146`, `d24r_n146`; 0.99 % of 1 209) and 4 on DOY 147 (0.33 % of 1 204), at 3 to 6 stations, the same ones in every run (GUAM00GUM, KIT300UZB, MBAR00UGA, LMMF00MTQ, LCK400IND, DJIG00DJI; MAJU00MHL and VOIM00MDG on DOY 147). Section 107's runs: 12 and 16 on DOY 146, 21 on DOY 147, of which ULAB00MNG holds 9 (hours 8 to 16); its first exceedance comes 7.5 min after the slip epoch (5 min after the re-attachment).

**Reading.** Outside the event a wet-delay change over 5 cm within an hour is rare (under about 1 % of station-hours, recurring at a few low-latitude stations), and the ULAB event crosses it within minutes and for nine hours. A candidate measure-only instrument for later; nothing built.

### 108.2 2026-11-17: the managing session's reading of section 108.1 and its decision on section 91.3

The managing session accepts section 108.1 as recorded: the guard is not adopted, its key stays KEEP_OFF, the six prepared observed arms and the guarded users are not run, and no further guard work follows tonight. Its reading: a mover is unsafe wherever a link's own lasting code offset reaches the step's band, because on two frequencies the code is the only wide-lane information and cannot tell a right link with an offset from a link one step off, the limit section 103.3 found for the consensus. On two frequencies the near-null is therefore bounded (5.6 mm on a level, section 103.3) but not closable link by link: a third band, on which the phase itself sees the pair, or a second independent route to a level closes it, and meanwhile what limits it is that no single link sets a level unchecked (section 109's point 4 and the single network). Checked against the beds: the twelve right links the guard moved carried offsets of 0.33 to 0.38 m, about three quarters of a step and inside the guard's band; a rounding rule without the band would move any offset beyond half a step. Its recommendation to the author: the signature as a measure-only monitor, with the run-to-run difference as its sharp form; the release form (a flagged held link released, never moved) as a later candidate with its own bars on the same beds; and closure with the third frequency.

Its decision on section 107.7, made before section 108.1's result: the users' stricter admission of section 91.3 is no longer queued, the guard being the repair, and the remaining 174 pair-epochs are read once guarded streams exist. With the guard not adopted that premise is gone; the decision is recorded as made and left to the author.

### 109.1 2026-11-17: the build of section 109 approved with six amendments (the managing session)

The build starts behind keys, off by default and byte-identical off, on this branch; its adoption is a separate question with its own bars.
1. A declared table, not a Galileo case: point 1 as a table owned by the roster, per constellation and band the family's identity signal and the member signals whose phase shares it (first and only entry: Galileo E1 L1C with L1X, E5a L5Q with L5X), empty by default; identities through `common/GnssIdentity.hpp` and the roster with its site counter, no new string parsing. The same table is the route for other constellations and channels.
2. Two keys, two arms: points 1 to 3 under one key; point 4 under its own, graded first as a one-change arm on today's separate-family configuration, since it is general. Point 4 also refuses the entry link as a known step when the near-null signature flags it (the tree edges the guard does not move are the promoted links), and the two paths that disagreed in section 107.6 get one owner of the known-or-unknown decision.
3. The stream: the L1X and L5X records carry the same settled flags and discontinuity counters as L1C and L5Q at every emission, with a unit test on the emitter for the values, flags and counters; the legend line declares it.
4. State and resume: whatever column identity changes (the satellite phase bias keyed by the family's identity signal) has its declared treatment in the parameter inventory, and the key is fingerprinted so that a snapshot of a two-family run is never resumed into a one-family run.
5. A slip bar on the simulator, section 107.6's regression test: on the two-mode bed with true slips on the combined-mode receivers' held pairs, no link held through a true slip, against the truth files; the slip layer's watched signals printed at run start for one receiver of each mode; beside the prepared bars (off identical, held integers right, the relabel arms' gains reproduced).
6. No term (section 107.8): a combined-mode Trimble user's wide lane then carries up to 0.07 cycle of the make's per-satellite term.

Order: point 4's key first, then the table and the family tie, then the simulator bars with the slip bar; observed arms of the native build wait for the morning.

### 108.3 2026-11-17: the spread of each held arc's code signature, observed days against the beds (the managing session's optional read)

**Method.** `arc_signature_spread.py`: per held Galileo E1/E5a link-hour, the mean of r(C1) − r(C5) over the hour's postfit code rows (the link held at least 90 % of the hour, 20 code epochs or more), centred on the median of that satellite's held link-hours of the same tracking mode in the hour (four or more); consecutive hours of a link form an arc (two hours or more), whose mean is the mean of its hourly deviations. Output `arc_signature_spread.txt`.

**Results** (|arc mean|; arcs at or beyond half a step, 0.224 m):

| run | arcs | median | 95 % | 99 % | beyond 0.15 m | at or beyond half a step |
|---|---|---|---|---|---|---|
| `d24_n146` (DOY 146) | 782 | 0.038 m | 0.156 | 0.268 | 5.9 % | 1.5 % |
| `d24_n147` (DOY 147) | 863 | 0.037 | 0.157 | 0.261 | 6.1 % | 1.5 % |
| `m1_off37` (clean bed) | 351 | 0.027 | 0.130 | 0.194 | 3.4 % | 0.6 % |
| `h1_off` | 343 | 0.055 | 0.224 | 0.395 | 18.4 % | 5.0 % |
| `d1_off` | 243 | 0.088 | 0.350 | 0.474 | 31.7 % | 16.9 % |

**Reading.** The observed days sit between the clean bed and h1, nearer the clean bed: a release of every held arc whose signature reaches half a step would release about 1.5 % of the observed held link-arcs (the real steps among them), against 5 % on h1 and 17 % on d1, where the guard's moves failed. The centring here is by satellite and mode only, so the observed percentages include each receiver's own code bias and are upper bounds.

### 110.2 2026-11-17: same-exe controls for section 110 (and 111)

Today's runs `d24_n146`, `d24r_n146` and `d24_n147` were made before exe nrf37 existed (they ended on 2026-11-16 at 20:35 and 20:39 and on 2026-11-17 at 00:15; nrf37 was built at 10:21), so they are not same-exe controls of the nrf37 arms, as section 107 states they are. Controls `c37_n146`, `c37r_n146` and `c37_n147` (their configurations, exe nrf37, 24 h) run beside section 110's arms; section 110's bars are graded against them, and their difference from today's runs is reported (identical output means section 107's comparison stands as made).

### 111. 2026-11-17: point 4, the re-validation of a held entry to the tree (pre-registered)

**Why.** Section 107.6: a held-first re-attachment took a held entry link's integer as a known step across a satellite's absence, on a pair an unseen slip had made wrong, while the chain's returning row declares that step unknown; the two paths disagreed. Section 109's point 4 with the managing session's amendment 2 (section 109.1): a held link that enters the tree is a re-coupling (standing invariant 7) and is re-validated before its integer is taken as a known step, and one function owns that decision.

**The build** (`network.ring_fix.entry_revalidate`, default off, byte-identical off). At each cut whose entry link (the edge that carries the moved side now) is a usable held link, `ringFixEntryVerdict` (`RingFix.cpp`) decides. An entry link whose arcs came back across a gap this epoch, or that enters a returning satellite's tree, is a known step only when the gap re-entry test measured its own held pair across the gap (`GapResumption` of the epoch's raw slip record with the held pair's two signals) and found no shift; otherwise it is refused (`gap_unchecked`, `gap_shifted`). A Galileo E1/E5a entry whose near-null signature (its code residuals averaged over `nn_guard_min_epochs`, centred on its satellite's family and then its receiver, as the guard computes them) sits at a whole step, at least half a step from zero, with the ionosphere-free deviation within `nn_guard_max_if_m`, is refused as well (`signature`). A refused entry's step is unknown: the chain's first-sighting target stands and the held links crossing the cut are released, as at any unknown step. The entry's float is not used: at the cut, before the epoch's solve, the last float predates the gap, and the gap test is the measurement across it. The chain's returning row documents that the ring fix's verdict is the one owner. One event per refused entry (RINGFIX `entry_refused`, with the reason and the evidence), counters and a run-end line; unit tests in suite RingFixEntryRevalidate (8 cases). Release build nrf39 (`cfcd136b1c470185`): the ring-fix tests pass (69 of 69, the 8 new among them) and the configuration tests (26 of 26); `nom_epoch_lint.py` and `nom_n4_verify.py --whole` pass; the smoke `e24_n147_smoke` (10 epochs) exits 0 with the arming line and the run-end line (no cut in ten epochs).

**Arms** (exe nrf39, the build of this section; the key as the one change):
- Simulator, 6 h from 00:30:15: `m1_ev`, `d1_ev`, `d31_ev`, `h1_ev`, `h40_ev` on the five beds of section 108, against the guard-off runs of exe nrf37 (`m1_off37`, `d1_off`, `d31_off`, `h1_off`, `h40_off`), with `h40_offe` (nrf39, key off) as the off check.
- Observed, today's configuration, 24 h: `e24_n146`, `e24r_n146`, `e24_n147` against the same-exe controls of section 110.2.
- Regression, the relabel run of section 107: `e1_n147` against `s1_n147`, the configuration in which section 107.6's event happened.

**Bars.**
- R1. Off: `h40_offe` reproduces `h40_off` in every data line.
- R2. Simulator, every bed: the truth audit's wrong held link-epochs and wrong decisions not above the control's; the frame-free count (summed over the sampled epochs, each family) not above the control's; every epoch emitted; on the clean bed the held link-epochs not more than 1 % below the control's.
- R3. Observed: every epoch emitted; RAIM rows not above the control's on any day; the GPS and the Galileo held shares not lowered by more than 1 point in any hour from 3; the two-run audit of DOY 146 (`e24_n146` against `e24r_n146`) with 0 failing Galileo checks.
- R4 (a read, not an adoption bar). In `e1_n147` the entry ULAB00MNG–E14 at TOW 288720 is refused (`gap_unchecked`), and ULAB00MNG has no hour with a wet-delay change over 5 cm in hours 8 to 16 (`s1_n147`: all nine).

**Reading prepared.** R1 to R3 passing: the key is adopted scoped (ledger RECOMMEND_SCOPED, its scope the integer-fixing chain configuration; the managing session's rule for the night), with its three-part note; the refusals are listed by reason. R2 or R3 failing: not adopted, the refusals that cost are listed. R4 says whether the check removes section 107.6's event where it happened.

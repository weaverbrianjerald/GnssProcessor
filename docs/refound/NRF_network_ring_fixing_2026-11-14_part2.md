# NRF part 2: stage N2, held integers through the chain's events (section 13 of card NRF)

**STATUS**

- **id:** NRF-part2
- **topic:** network-vehicle
- **status:** open
- **summary:** Section 13 of card NRF (2026-11-15): the results of stage N2, pre-registered in section 12 of [NRF](NRF_network_ring_fixing_2026-11-14.md). The ring fix with the order free and the odds gate (N1b-OL), the cut algebra of section 12 and a truth audit in every epoch's frame, on r3c (re-seats, returns, one true slip) and seven event beds: 0 wrong held integers on every cold-start arm (28 000 to 31 000 held link-epochs per bed). The ceiling (every link held at its true integer all run) takes 0 wrong. The steady-state harness (truth for 30 min, then the rule alone) FAILS: 68 wrong decisions after the cut-over. Bar I-b fails on every bed with and without the ring fix, at similar counts. Section 13.1 records the requesting session's re-grade; section 14 pre-registers stage N2b (held-first re-attachment, the oracle's epoch rule, the log rule, true slips on held links). Section 15 (N2b): 0 wrong held integers on every arm (890 063 held link-epochs); held first keeps the held integers through re-seats wherever a held link is present (0 released at a cut on six beds against 160 to 188 as built; the 26 released on the two split beds were absent at their cut) and lifts the held share of the last 30 min to 62 to 84 % (30 to 61 % as built); the ceiling and the steady-state harness pass once a released link's carried entry is re-opened (the oracle's conflict was a stale entry with the hold's variance); true slips on held links are released at their epoch and held again at the right integer 14.5 to 15.5 min later. Section 16 pre-registers stage N3.
- **source:** [NRF](NRF_network_ring_fixing_2026-11-14.md) sections 4, 10 to 12; [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 9 (bar I-b); [IRDU part 3](../record/refound/IRDU_user_integers_on_the_chain_2026-11-14_part3.md) section 12.5 (Q4)

**Outcome.** Question: do held integers survive the chain's events, and does a network that is integer stay integer by itself? From a cold start, on r3c and on seven event beds, the ring fix takes 0 wrong integers; every held link that crosses a cut with an unknown step is released at the cut, and re-hosts onto held links keep the frame. A network warmed by truth for 30 min and then left to the rule takes 68 wrong decisions within the next hour, for a reason not yet traced. Stage N2b (section 15) found the cause (a released link's carried entry kept the hold's variance), after which the warm-started network takes 0 wrong decisions, and held first keeps held integers through re-seats. Standing: default off; N3 next.

Sections 1 to 12 are in [NRF](NRF_network_ring_fixing_2026-11-14.md). Results of this section are appended below.

## 13. 2026-11-15: stage N2 results

**What ran.** Commit `65a7d42` with one fix after its first runs (below), exe `gnss_processor_irdu_nrf5.exe` (sha256[:16] `ee72f922a4b65126`). On r3c: `_rap_nrf5_olt_r3c` (N2-r3c: N1b-OL with the truth audit), `_rap_nrf5_olc_r3c` (N2-ceiling: the oracle phase all run), `_rap_nrf5_ols_r3c` (N2-steady: the oracle phase for 30 min from the first decision epoch, cut-over at TOW 263010, then N1b-OL), `_rap_nrf5off_r3c` (the plain chain arm). Event beds, N1b-OL with the truth audit and the bed's plain chain arm (exe nrf5): `_rap_nrf_olt_<bed>` and `_rap_nrf4off_<bed>` for `ird_t2`, `ird_tr`, `ird_t3`, `ird_rv16`, `ird_rg`, `ird_ms`, `ird_msx`. Gates: `_rap_nrf5off_zw_nr3`, `_rap_nrf5_ol_nr3`, `_rap_nrf5_olt_nr3`. Every run exit 0, 240 of 240 epochs. Grades `runs_nom/_irdu_grades/nrf_n2_*`; the steady-state reads `scripts/nrf_steady.py`.

**The fix after the first runs.** On the first build (exe nrf4, `20b7c5c914d9c00c`; runs `_rap_nrf_olc_r3c`, `_rap_nrf_ols_r3c`, `_rap_nrf_olt_r3c`, kept as the record) the truth model did not count the fixture's one true slip (S43000SIM G22 L1C +25 at TOW 261930) at its own epoch: the epoch's clock sits a few microseconds before the stamp. The oracle then held that link 25 cycles off for one epoch, and that arm took 1 wrong held link-epoch. The slip now counts with half a second of slack. The cold-start arm, which held nothing at that epoch, is unchanged by the fix.

**Gates: PASS.** The key off against the previous build (pv1): IDENTICAL whole. N1b-OL on the bed without re-seats against section 11's run: IDENTICAL whole (no cut occurs there). The truth model against the bed's frame file: 87 048 chord-band-epochs compared, 0 differ; on that bed the truth audit reads 12 194 held link-epochs right, 0 wrong.

**N2-B1, 0 wrong holds.**

| arm | links decided | held link-epochs | wrong (truth audit) | held at the end | releases: chain, gap, slip, refloat, monitor | became tree edges |
| --- | --- | --- | --- | --- | --- | --- |
| N2-r3c | 413 | 31 200 | 0 | 200 | 167, 28, 0, 0, 0 | 18 |
| ird_t2 (slip, large subtree) | 423 | 28 266 | 0 | 198 | 182, 27, 0, 0, 0 | 16 |
| ird_tr (slip on the root's edge) | 409 | 30 358 | 0 | 175 | 188, 29, 0, 0, 0 | 17 |
| ird_t3 (slip with LLI) | 415 | 30 735 | 0 | 200 | 168, 28, 0, 0, 0 | 19 |
| ird_rv16 (returning vertex) | 404 | 29 814 | 0 | 199 | 160, 28, 0, 0, 0 | 17 |
| ird_rg (root handover) | 378 | 19 795 | 0 | 175 | 171, 14, 1, 0, 0 | 17 |
| ird_ms (split and merge) | 351 | 27 611 | 0 | 176 | 128, 28, 1, 0, 0 | 16 |
| ird_msx (abrupt cut-off) | 366 | 27 972 | 0 | 187 | 130, 25, 0, 0, 0 | 22 |
| N2-ceiling | 1 068 (oracle) | 92 110 | 0 | 414 | 68, 134, 5, 383, 0 | 64 |
| **N2-steady** | 834 (628 oracle) | 63 467 | **2 728** (68 decisions) | 190 | 207, 82, 3, 184, 125 | 43 |

The independent re-grade with the frame algebra of `scripts/ird_frame_census.py` (`nrf_audit.py --fixture`) agrees with the code's audit to the count on the three r3c arms (N2-r3c 31 200 and 0, N2-ceiling 92 110 and 0, N2-steady 63 467 and 2 728 held link-epochs with 68 wrong decisions). Every cold-start arm and the ceiling take 0 wrong. **N2-steady FAILS the bar** (below).

**The cuts.** On r3c 148 cuts on a band; 36 had a known step, every one 0 (a re-host onto a held link: the chain's target equals the held integer, as card IRDU Q4 found), so no held integer was ever re-expressed by a non-zero step; 112 had an unknown step and released the 167 held links crossing them. No old host was inherited on any bed: the old host's arc had ended or crossed a cut with an unknown step. On the root handover bed the root row pinned the new root at its carried bias and 77 held links kept their integers, 0 wrong. Decided = released + became tree edges + held at the end on every arm (413 = 195 + 18 + 200 on N2-r3c; the run-end line of the first build left the tree transitions out of the sum and printed NOT CLOSED; it counts them now).

**N2-B2: holds on the events exercised.** Every held link crossing a cut with an unknown step is released at the cut's epoch, with an event row naming the cut's row and `step=unknown` (167 on r3c; 128 to 188 per event bed). The IRD slips fall on tree edges (`ird_t2`, `ird_tr`) or on an arc that was a tree edge at the slip (`ird_t3`), so no held link sat on a slipped arc there; their slips release the held links that cross the moved side, at the slip's epoch. A held link on a slipped arc was released at the slip's epoch on r3c's ceiling and steady arms (the fixture's slip, cause slip) and in N1-P (three).

**N2-B3, card IRD's I-b: FAILS on every bed, with and without the ring fix.** The bed's plain chain arm fails I-b too, with thousands of undeclared integer steps per class (r3c: cut releases 5 017 and 5 070 undeclared integer steps without and with the ring fix, kept edges 5 720 and 5 277, non-event epochs 16 035 and 14 672). The ring fix does not change the counts materially; the steps are the chain's own (card IRD's I-b reading of the as-built chain).

**N2-B4: PASSES.** 240 of 240 epochs on every arm.

**The steady state.** After the cut-over (TOW 263010), from `nrf_steady.py`:

| arm | chord link-epochs held after the cut-over | wrong | minutes from birth to hold, p50 (p90), rings 2 / 3 / 4 / 5 |
| --- | --- | --- | --- |
| N2-ceiling | 99.5 % | 0 | (truth at once) |
| N2-steady | 58.1 % | 2 728 | 17.0 (27.0) / 22.5 (43.0) / 31.5 (56.0) / 40.5 (63.0) |
| N2-r3c (cold start, same split) | 44.3 % | 0 | 17.0 (34.5) / 24.0 (43.5) / 41.0 (65.5) / 40.5 (63.0) |

A link born, re-seated or released after the cut-over is held 17 to 41 min later at the median by ring, with or without the warm start: the event window (600 s) and the persistence count (10 epochs) set the floor at 15 min, the floats the rest.

**Why N2-steady fails, as far as traced.** Its first wrong decisions come 16 min after the cut-over (TOW 263970: S19000SIM-G11 and S14000SIM-G19, ring 5), each 1 cycle off truth on both bands (the wide lane right), and the producer's lattice agrees with the wrong integers. Those links were held at truth by the oracle until the cut-over and released by the monitor within 30 to 60 s after it (IF residual above 3 cm at 4 epochs; 125 monitor releases over the run, none on any cold-start arm). After the release the float converged to an integer 1 below truth, and later to 2 and 3 below (S19000SIM-G11 decided 76, 75 and 74 in turn while truth read 77), so its frame drifted by about one cycle per 20 to 40 min on both bands. Holding every link at truth also makes RAIM attribute 408 times on the ceiling and 370 on N2-steady, mostly at the slipped station S43000SIM and two others, where the plain and cold-start arms have none. The candidate: the truth integers conflict with what the producer estimates at some links (around the slipped station first), the oracle phase bends the network to them, and after the cut-over the bent state relaxes while the rule decides. Not traced: the source of the conflict (the slip's response at S43000SIM, a model term the simulator has and the producer lacks, or the oracle's own forcing). The cold-start arm with the same rule on the same bed takes 0 wrong.

**Verdict.** From a cold start the ring fix takes 0 wrong integers through r3c's re-seats and on all seven event beds, releasing what crosses a cut it cannot follow and keeping the frame through re-hosts onto held links and through a pinned root handover. The steady-state harness as built does not answer whether a network that is integer stays integer: its warm start (truth for 30 min) is not a state the network reaches by itself, and after it the rule takes 68 wrong decisions. N2-B1 holds on every cold-start arm and fails on N2-steady; N2-B2 holds where exercised; N2-B3 fails for the chain with and without the ring fix; N2-B4 holds.

**What it points to, for the author's decision.** (a) A steady-state harness warmed by the network's own holds: a cold-start run continued past its first two hours (the 6 h bed of N3 does this), with the reads after hour 2; (b) the conflict between truth and the producer on the ceiling (RAIM's attributions around the slipped station) traced before truth is used as a warm start again; (c) the inheritance of old hosts never fired: re-hosts on these beds follow arcs that end, so the old host is gone; (d) N3 from a cold start, as section 6 plans. N3 waits for the requesting session's word, since N2-steady failed.

**Not verified.** The steady-state failure's cause; a held link on a slipped arc on an IRD slip bed (none occurred); a non-zero known step (none occurred); users on the N2 streams (not replayed: the stage's verdict did not need them, and the steady-state arm failed first).

### 13.1 2026-11-15: re-grade by the requesting session

Re-graded from the runs of section 13 and section 11 by the requesting session; recorded here as received.

- N1b against `config/sim/irdu/fpin_nr3_all.csv` with `scripts/nrf_regrade.py`: N1b-O 1 036 held link-epochs, 0 wrong; N1b-OL 12 194, 0 wrong.
- N2 counts reproduce: N2-r3c 31 200 held link-epochs right; the ceiling 92 110; N2-steady 60 739 right and 2 728 wrong; 3 329 decisions without a true integer agree with the frame algebra.
- The stream on r3c, NL offset p50 over all depths in 15-min bins (cycles): fixing from cold 0.247 0.166 0.106 0.060 0.047 0.023 0.017 0.022; plain 0.247 0.191 0.132 0.138 0.149 0.133 0.105 0.117; truth-held 0.023 0.248 0.260 0.155 0.040 0.020 0.014 0.014. Held share of the chords by 15 min (cold start): 0 4 12 25 43 59 71 51 %.
- Findings. (a) Re-seats discard held integers that could be kept: the re-host at 104.8 min released 74 held links and the one at 109.2 min 47; the held share fell from 71 % to 51 % and the depth-5 NL offset rose from 0.023 to 0.085 cycle. (b) The truth-held arms are not valid after the slip's epoch: `network_raim.csv` has 0 rows on the cold and plain arms and 8 255 rows (7 755 refloats) on the ceiling, 8 080 (7 598) on N2-steady, at 44 stations over TOW 261930 to 263790; S43000SIM-G22 is released and held again every epoch from 261990; at 261930 the largest s is 278.7 at S43000SIM, the congruence 75.6 m up and the gross-LOS residual 12.5 m on G19; S19000SIM-G11 carries a residual of -4.9 cm while held at truth at 262920. Neither "an integer network does not stay integer" nor its reverse follows from that arm. (c) A held link under a true slip was not exercised from a cold start (a standing-invariant-1 item, which blocks observed data). (d) The wrong pairs of S40000SIM fall in its first half hour.

## 14. 2026-11-15: stage N2b pre-registered (held-first re-attachment, the oracle's epoch rule, the log rule, true slips on held links)

**Question.** Do held integers survive the chain's re-seats when the moved side re-attaches through a held link, and does a held link survive a true slip from a cold start? Stage N2b answers the four requests of the requesting session after section 13.1, before the 6 h bed of stage N3.

**Code (exe `gnss_processor_irdu_nrf6.exe`, sha256[:16] `f89c2de2dde740f7`; default off; the ledger rows UNVALIDATED).**

1. *Held-first re-attachment*, `network.ring_fix.held_first` (N2b-1). In the chain's walk, whenever the incumbent queue is empty, a held arc (a link the ring fix holds, not latched this epoch, its monitor not tripped) from an attached vertex to an unattached one is taken first; among several, the first in the walk's own order (the attached vertices in attachment order, each one's candidates in the chain's order). The incumbent queue then re-attaches the rest of the fragment through its old edges. With no held arc the walk takes its own new edges, one at a time, so that the fragment each one reaches is re-attached and its held arcs are tried before the next. The chain's rehost row pins the new edge at its rounded carry, which is the held integer, so the cut's step is known (0) and the held links that cross the cut keep their integers. One `RINGFIX reattach` event per re-attachment names the link and the number of held links kept. Off, or with no held arc in a family's graph, the walk is the one as built.
2. *Two corrections of N2's code*, found while building N2b and in effect on every ring-fix arm from this build:
   - *Nested cuts.* The chain lists each vertex of a moved side once, in the innermost release. N2's algebra crossed a link with the listed vertices only, so a link from a nested side to the outside of its outer side took the nested step and not the outer one (kept across an unknown outer step). A cut now takes its released vertex's whole subtree in the epoch's tree, and the steps add. Held-first makes a known cut nested below an unknown one common; in N2 every known step was 0 and no wrong integer followed. Counted at run end: cuts with a nested release.
   - *The release floor.* A released link's carried entry is re-opened to at least 1 m^2 at every release cause, the slip and the refloat included (their drop already ran, but restores the birth variance).
3. *The oracle's epoch rule* (N2b-2 (i)): the oracle puts no hold into an epoch in which the link was released, its arc latched, re-levelled (a refloat or a drop) or its carried entry erased (a cut); it holds from the next epoch.
4. *The log rule*, `network.ring_fix.rule` = `log` (N2b-2 (ii)): the frozen rule evaluated and traced (`would_hold` rows and events; run-end counts of links, link-epochs and changed integers; in simulation, against truth), nothing held.

**Why the truth hold conflicted with the data (N2b-2 (i), traced in N2's runs).** At the fixture's slip (S43000SIM G22 L1C +25 at TOW 261930) the oracle held the link. The slip latch released it, and the float vehicle's drop restored the carried entry's variance to the largest of its birth variance, its first variance and the floors. The link had been held from its first stash, so its birth variance was the hold's own (1e-8 m^2): the entry stayed at the old level (45.0088 cycles, variance 2.76e-7 cycle^2), and the solve of the slip's epoch put the 25-cycle step into the station's other states. From the next epoch the oracle held the link at its new true integer (70) against that stale, tight entry; the two met halfway (57.5) and RAIM refloated every epoch from then on. The truth itself does not conflict with the data; the stale entry does. An entry re-created under a hold after a cut erased it has the same birth variance, so the cold-start arms carry the same latent defect wherever such a link is later released by a slip or a refloat. The release floor (item 2) and the epoch rule (item 3) are the response.

**Arms (Release, 2 h, arm A1krhRsmh, the bed's knob group, exe nrf6).**

| stage | runs (`_rap_<tag>`) |
| --- | --- |
| gates | `nrf6off_r3c` against `nrf5off_r3c` (key off); `nrf6_log_r3c` against `nrf6off_r3c` (the log rule); `nrf6_ol_nr3` against `nrf5_ol_nr3` (N1b-OL, no cut and no release on that bed) |
| N2b-1 | `nrf6_hf_<bed>` (N1b-OL, truth, held-first) and `nrf6_olt_<bed>` (N1b-OL, truth, the control on this exe) for r3c, ird_t2, ird_tr, ird_t3, ird_rv16, ird_rg, ird_ms, ird_msx; the plain arms of section 13 |
| N2b-2 | `nrf6_olc_r3c` (the ceiling), `nrf6_ols_r3c` (N2-steady, otherwise unchanged) |
| N2b-3 | on each slip bed nrf_s1, nrf_s2, nrf_s3: `nrf6off_<bed>` (key off), `nrf6_olt_<bed>`, `nrf6_hf_<bed>` |

The slip beds (`scripts/nrf_slip_bed.py`, `D:/GnssProcessorSupport/runs_nom/simnrf_s1..s3`) are rung 3c with one scripted, unflagged slip at TOW 265830 (80 min), its truth row added to the station's `truth_slips.csv`; `nrf_slip_bed.py verify` proves that nothing else differs from `runs/simarl_r3c`. The links are named from N2-r3c's trace: s1, L1C +25 on S05000SIM-G26 (a chord held from TOW 262110, 62 min at the slip, held to the end); s2, L1C +1 and L2W +1 on the same link; s3, L1C +1 and L2W +1 on S03000SIM-G25, a tree edge in both bands from the first epoch to TOW 267540, with 15 vertices below it and 38 held links across it at the slip.

**Bars (frozen; an arm with a wrong hold is rejected, not adjusted).**

- N2b-1. B1: 0 wrong held integers at every epoch on r3c and the seven event beds (the code's truth audit and `nrf_audit.py --fixture` agree). B2: 0 held links released by a cut that a held link crossed (read: usable held links released across an unknown step). B3: byte-identical when off (the key-off gate).
- N2b-2 (i). The ceiling's `network_raim.csv` has no more rows than the cold arm's (0), and the stream stays at its first bin's level. N2-steady: 0 wrong decisions; a wrong one is traced and reported at once, before anything below.
- N2b-2 (ii). The log rule's products are byte-identical to the key off's.
- N2b-3. B1: the held link released at the slip's epoch when the slip is flagged, within the monitor's 4 epochs otherwise. B2: no wrong held integer at any epoch after that release. B3: RAIM refloats in the 10 min after the slip at most the key-off bed's + 10. B4: every epoch emitted.

**Reads.** N2b-1: releases by cause; cuts by kind (held first, other known step, unknown step); held share by 15 min; the stream by depth (`scripts/irdu_stream_depth.py --fixture`, 15-min bins); I-b counts by class beside the plain arm (`scripts/simnet_chain_frame_grade.py`). N2b-3: what released the link; how many other holds went; the stream's step at the satellite and at the receiver; minutes until the link is held again, and whether at the right integer.

**Prediction (N2b-1).** On r3c with held-first, the held share in the last 30 min at least 85 % (61 % as built) and the depth-5 NL offset p50 at most 0.03 cycle in the last bin.

**Not in this stage.** Tracing S40000SIM; old-host inheritance; I-b beyond its counts.

## 15. 2026-11-15: stage N2b results

**What ran.** Commit `bbeaa15` (the code and section 14, before any run), exe `gnss_processor_irdu_nrf6.exe` (sha256[:16] `f89c2de2dde740f7`), 30 runs of section 14's table under `D:/GnssProcessorSupport/runs_nom` (`_rap_nrf6*`); every run exit 0, 240 of 240 epochs emitted, `network_raim.csv` with 0 rows on every ring-fix arm. Grades in `runs_nom/_irdu_grades/nrf_n2b_*`. Reads: `scripts/nrf_reads.py` (new: held share by 15 min, releases by cause, re-attachments, the run-end lines, RAIM rows, epochs; it reproduces section 13.1's held share of N2-r3c, 0 4 12 25 43 59 71 51 %), `nrf_audit.py --fixture`, `irdu_stream_depth.py --fixture`, `simnet_chain_frame_grade.py`.

**Gates: PASS.** The key off against N2's build (`nrf6off_r3c` against `nrf5off_r3c`): IDENTICAL whole. N1b-OL on the bed without re-seats (`nrf6_ol_nr3` against `nrf5_ol_nr3`): IDENTICAL whole. The log rule (`nrf6_log_r3c` against `nrf6off_r3c`): `products.txt` IDENTICAL; only the arm file, the RINGFIX rows of `network_events.csv` and the run-end lines differ. The log rule would have decided 198 links at 12 661 link-epochs, all at their true integers; 127 of those link-epochs carry integers other than the link's first (with nothing held, the frame moves at every cut with an unknown step).

**The two corrections on N2's arms.** The control arms on this exe (`nrf6_olt_<bed>`) reproduce section 13's counts on every bed (decided and held link-epochs equal, for example 413 and 31 200 on r3c, 423 and 28 266 on ird_t2): the as-built walk made no nested cut on r3c and 4 on ird_ms, none of which changed a held link, and the release floor never bound there.

**N2b-1, held-first re-attachment.**

| bed | arm | decided | held link-epochs, wrong | releases: chain, gap, slip | cuts: held first (step 0), other known, unknown | re-attachments (held links kept) | usable held links released at a cut | held share, last 30 min |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| r3c | control | 413 | 31 200, 0 | 167, 28, 0 | 0, 36, 112 | 0 | 167 | 61.0 % |
| r3c | held first | 423 | 38 241, 0 | 0, 33, 2 | 62 (62), 0, 72 | 31 (414) | 0 | 83.7 % |
| ird_t2 | control | 423 | 28 266, 0 | 182, 27, 0 | 0, 32, 116 | 0 | 182 | 53.0 % |
| ird_t2 | held first | 414 | 37 154, 0 | 0, 31, 1 | 64 (64), 0, 72 | 32 (479) | 0 | 79.7 % |
| ird_tr | control | 409 | 30 358, 0 | 188, 29, 0 | 0, 34, 118 | 0 | 188 | 57.4 % |
| ird_tr | held first | 410 | 36 879, 0 | 0, 33, 1 | 64 (64), 0, 72 | 32 (474) | 0 | 78.4 % |
| ird_t3 | control | 415 | 30 735, 0 | 168, 28, 0 | 0, 38, 112 | 0 | 168 | 58.9 % |
| ird_t3 | held first | 412 | 37 102, 0 | 0, 33, 1 | 64 (64), 0, 72 | 32 (444) | 0 | 78.9 % |
| ird_rv16 | control | 404 | 29 814, 0 | 160, 28, 0 | 0, 34, 124 | 0 | 160 | 57.1 % |
| ird_rv16 | held first | 418 | 36 949, 0 | 0, 30, 1 | 64 (64), 0, 76 | 32 (355) | 0 | 79.7 % |
| ird_rg | control | 378 | 19 795, 0 | 171, 14, 1 | 0, 34, 142 | 0 | 170 | 29.9 % |
| ird_rg | held first | 400 | 34 141, 0 | 0, 29, 1 | 72 (72), 0, 74 | 36 (686) | 0 | 71.0 % |
| ird_ms | control | 351 | 27 611, 0 | 130, 28, 1 | 0, 32, 144 | 0 | 128 | 52.0 % |
| ird_ms | held first | 348 | 31 297, 0 | 7, 29, 0 | 68 (68), 0, 92 | 34 (273) | 7 | 62.3 % |
| ird_msx | control | 366 | 27 972, 0 | 132, 25, 0 | 0, 44, 128 | 0 | 130 | 50.7 % |
| ird_msx | held first | 365 | 32 357, 0 | 25, 29, 0 | 70 (70), 0, 82 | 35 (312) | 19 | 62.3 % |

- **B1 (0 wrong held integers): PASS** on r3c and the seven event beds. `nrf_audit.py --fixture` agrees with the code's audit to the count on the r3c arms (held first 38 241 and 0; control 31 200 and 0; ceiling 91 807 and 0; steady 72 018 and 0).
- **B2 (0 held links released by a cut that a held link crossed): PASS on six beds, FAILED as read on ird_ms (7) and ird_msx (19).** Every one of those 26 links was absent at its cut: on ird_ms they are the crossing arcs the bed blanks at the thinning (TOW 264900), on ird_msx the crossing arcs blanked at the abrupt cut-off (TOW 265740), so no held link was present to carry the moved side. Counted on present links, the read is 0 on every bed; the frozen read counts absent links, and the record keeps it as FAILED. Six more links on ird_msx were released at the merge (TOW 266340) by N2's root-change rule, not by a cut: links inside the side that re-joins through an unknown step, whose integers the cut algebra keeps (both ends on the moved side).
- **B3 (byte-identical when off): PASS** (the key-off gate).
- Every held-first cut that the walk took first had a step of 0 (528 of 528 over the eight beds); 2 to 8 cuts per bed with a nested release on ird_rv16, ird_rg and ird_ms; the release floor bound 0 to 4 times per arm.
- The slip releases (1 or 2 per held-first arm) are the producer's latch on S35000SIM G05 and G19 near the end of the runs (no true slip at that station in the fixtures): released, never held wrong.
- **Prediction on r3c: half met.** The held share over the last 30 min is 83.7 % (predicted at least 85 %; 85 % in the last 15-min bin); the depth-5 NL offset p50 in the last bin is 0.013 cycle (predicted at most 0.03).
- **The stream on r3c, NL offset p50 by 15 min (cycles):**

| arm | all depths | depth 5 |
| --- | --- | --- |
| plain chain | 0.247 0.191 0.132 0.138 0.149 0.133 0.105 0.117 | 0.289 0.265 0.202 0.187 0.195 0.156 0.120 0.111 |
| control (N1b-OL) | 0.247 0.166 0.106 0.060 0.047 0.023 0.017 0.022 | 0.289 0.238 0.174 0.112 0.120 0.030 0.023 0.085 |
| held first | 0.247 0.166 0.060 0.046 0.035 0.016 0.013 0.012 | 0.289 0.238 0.085 0.068 0.056 0.019 0.017 0.013 |
| ceiling (new build) | 0.024 0.018 0.016 0.016 0.016 0.014 0.014 0.014 | 0.017 0.015 0.017 0.017 0.018 0.016 0.015 0.015 |

  The wide lane over all depths with held first: 0.179 0.109 0.078 0.073 0.041 0.035 0.035 0.033 (control 0.179 0.109 0.070 0.073 0.051 0.035 0.045 0.078).
- **I-b (card IRD) by class:** still FAILED on every bed. Held first lowers the undeclared integer steps of every class against the plain arm (r3c, band 1: cut release 2 070 to 1 948, kept 2 247 to 1 640, non-event 6 856 to 4 914, rehost 2 019 to 1 896, returning 102 to 52); the steps are the chain's own, as section 13 found.

**N2b-2 (i), the oracle's conflict: the cause found, and the bars PASS.** The cause is the one section 14 states (the stale entry with the hold's birth variance after a release). With the release floor and the epoch rule the ceiling (`nrf6_olc_r3c`) has 0 RAIM rows (N2: 8 255), 91 807 held link-epochs, 0 wrong, and its stream stays at its first bin's level (NL p50 over all depths 0.024 in the first bin, 0.014 to 0.018 after; N2: up to 0.260); 687 oracle holds were put off by one epoch and the floor re-opened 10 entries. N2-steady re-run (`nrf6_ols_r3c`): **0 wrong decisions** (N2: 68), 72 018 held link-epochs right, 0 monitor releases (N2: 125), 0 RAIM rows (N2: 8 080); after the cut-over 70.7 % of the chord link-epochs held (N2: 58.1 %), and a link born after the cut-over is held 17.0, 20.5, 24.0, 28.5 and 27.0 min later at the median on rings 2 to 6. Its depth-5 stream climbs to 0.096 in the last bin, where re-seats release held links (the steady arm has no held first).

**N2b-2 (ii), the log rule: PASS** (the gate above).

**N2b-3, true slips on held links from a cold start.**

| bed | arm | the slipped link | other holds released, slip epoch and 4 after | RAIM rows, 10 min after (key off) | held again | stream's persistent step, satellite / receiver (cycles, L1 and L2) |
| --- | --- | --- | --- | --- | --- | --- |
| s1, L1 +25 on held S05000SIM-G26 | control, held first | released at the slip's epoch, cause slip (flagged) | 0 (one gap release elsewhere) | 0, 0 (1) | 15.5 min later, right | -0.03, -0.03 / -0.07, +0.06 (noise 0.03 to 0.05 per epoch) |
| s2, L1 +1 and L2 +1 on held S05000SIM-G26 | control, held first | released at the slip's epoch, cause slip (flagged) | 0 | 0, 0 (0) | 14.5 min later, right | -0.03, -0.03 / -0.06, +0.05 |
| s3, L1 +1 and L2 +1 on the tree edge S03000SIM-G25 | control | a tree edge, not held; de-seated at the slip's epoch | 38 (the held links across the moved side, unknown step) | 0 (0) | as a chord, 17.0 min later, right | +0.38, +0.76 / -0.37, -0.41 |
| s3 | held first | as above; the moved side re-attached through the held link S01000SIM-G28 at step 0 | 0 (42 held links kept) | 0 (0) | as a chord, 20.5 min later, right | +0.09, +0.12 / -0.37, -0.41 |

- **B1: PASS** (the held link released at the slip's epoch on s1 and s2, both slips flagged by the producer's latch; s3's slipped arc was a tree edge, and the held links across it were kept with held first and released at the slip's epoch without). **B2: PASS** (0 wrong held link-epochs after the release on every arm). **B3: PASS** (0 RAIM rows in the 10 min after the slip on every ring-fix arm; the key-off beds 1, 0 and 0). **B4: PASS** (240 of 240).
- The release floor bound at the slip on s1 and s2 (2 entries, both bands of the link, on both arms; 0 on the control without the slip): the vehicle's drop alone would have left the link's carried entry tighter than 1 m^2. Why its birth variance was below 1 m^2 is not traced.
- On s3 every arm, the key off included, leaves a fractional persistent step at the receiver S03000SIM (about -0.37 cycle on both bands) and at the satellite G25 (key off +0.37 and +0.77), with a one-epoch transient of about -3 cycles at the slip's epoch on the key off and the control (+0.40 and +0.51 with held first). G25 is declared at that epoch on every arm. A 1/1 slip on a tree edge therefore moves the producer's own estimates by a fraction whatever the ring fix does; the receiver's bias is not in the stream.

**Verdict.** No safety bar of N2b failed: 0 wrong held integers on every arm (890 063 held link-epochs over the 25 runs that hold). Held first keeps the held integers through the chain's re-seats wherever a held link is present (0 released at a cut on six beds, against 160 to 188 as built), lifts the held share of the last 30 min from 30 to 61 % to 62 to 84 %, and brings the r3c stream's NL to 0.012 cycle over all depths and 0.013 at depth 5 in the last bin. The ceiling and the steady-state harness are valid now: the oracle's conflict was the stale entry after a release, not the truth. A true slip on a held link is released at its epoch and the link is held again at the right integer 14.5 to 15.5 min later. Stage N3 runs with all three arms.

**For the author's decision.** (a) B2 as read counts absent links; whether the bar means present links (read 0 on every bed) is the author's call. (b) The root-change rule releases the links inside a side that re-joins through an unknown step (6 on ird_msx); the cut algebra already keeps them. (c) The fractional persistent step at the receiver after a 1/1 slip on a tree edge (all arms, key off included) is a producer question outside the ring fix. (d) I-b remains failed for the chain with and without the ring fix.

**Not verified.** A held link on a slipped arc whose slip is not flagged (both slips of N2b-3 were flagged, so the monitor's 4-epoch path stayed unexercised); users on the N2b streams (N3 replays them); observed data.

## 16. 2026-11-15: stage N3 pre-registered (the 6 h golden sim bed from a cold start)

**Question.** Over 6 h on the reference bed, from a cold start, does the ring fix stay at 0 wrong integers, how much of the network does it hold by hour, and what do users get from the stream?

**Bed.** The golden sim bed of card IRD-8: 60 stations of `runs/simnet60_quiet_col_24h`, GPS and Galileo with the chain on GPS (Galileo unclaimed, card IRD section 13.4), the held-out consumer SZZ000SIM, from 00:30:15 (`start_offset_hours` 0.504167) for 6 h (720 epochs). The fixture is also the truth of the audit (`network.ring_fix.truth_root`).

**Arms (exe nrf6, arm A1krhRsmh, 6 h; `scripts/ird_make_arms.py` groups `nrf_olt_golden` and `nrf_hf_golden`).** `nrf6off_g6`, the plain chain; `nrf6_olt_g6`, N1b-OL with the truth audit; `nrf6_hf_g6`, the same with held first; and `nrf6_G_g6`, arm G (the golden unchanged, card IRD's same-build reference for its P bars), which has no ring fix.

**Users.** The users' build (`gnss_processor_irdu_users_rscb3.exe`), five users (SZZ000SIM, S48000SIM, S40000SIM, S27000SIM, S32000SIM), started 30 min, 2 h, 3 h and 4 h after the network's first epoch (`start_offset_hours` 1.004167, 2.504167, 3.504167, 4.504167; `scripts/irdu_users.py` now adds the network's own start offset and takes the fixture from the consumer's RINEX when the arm names none), float (obs) and fixing (fix), on the three ring-fix and plain streams; graded with `scripts/irdu_user_grade.py`.

**Bars (frozen; an arm with a wrong hold is rejected, not adjusted).**

- N3-B1: 0 wrong held integers over 6 h (the code's truth audit and `nrf_audit.py --fixture` agree).
- N3-B2: every release named (a cause on every RINGFIX release row; the run-end closure, decided = released + tree edges + held at the end, closed).
- N3-B3: every epoch emitted (720 of 720).
- U1: no wrong pair integer on a fixing stream for the users started at 2 h or later (WRONG = 0; the plain stream's count beside).
- U2: HELD above the plain stream's for those starts.

**Reads by hour.** Held share, decisions, releases by cause, minutes from birth to hold, cuts by kind, the stream by depth in 15-min bins beside the plain chain, declared and undeclared steps (I-b counts by class). Users: 3D by 15 min, HELD, WRONG, UNRESOLVED; card IRD-8's K-a (wrong among the settled determined integers, with the 95 % Wilson bound) and K-c where the user runs give them; P-a (the held-out consumer's float 3D against arm G).

**Prediction (the requesting session's).** With held first, a held share of at least 90 % in hours 3 to 6, the NL offset p50 at most 0.03 cycle at every depth, and the users started at 3 h HELD at least half the time.

### 16.1 2026-11-15: a defect found in the first N3 runs, fixed before the re-runs

**The defect.** The cut algebra matched a cut's band to a held link's band by the signal string alone, and GPS L1 and Galileo E1 are both `L1C`. On the GPS and Galileo bed a GPS cut whose moved side held a receiver therefore also crossed that receiver's Galileo links: with an unknown step it released them, and with a known step it would have re-expressed them by the other constellation's step; a Galileo cut did the same to the GPS links. On the held-first arm all 63 held links released at cuts were present at their cut: 51 Galileo and 12 GPS links, 60 of them at epochs with cuts of the other constellation only, 3 at an epoch with cuts of both. Every known step of the first runs was 0, so no held integer was moved and the truth audit reads 0 wrong on both arms; the releases were spurious. The same matching was in N2's code; on the GPS-only beds of N1 to N2b it cannot occur.

**The fix (commit below, exe `gnss_processor_irdu_nrf7.exe`).** A cut carries its band's constellation (from the release's group key, `<constellation>|<signal>`), and `ringFixCutBand` matches a link's band only when both the constellation and the signal agree; the old-host inheritance uses the same test. Unit test `RingFixCutsN2b.ACutOnOneConstellationLeavesTheOtherConstellationsLinks`. `scripts/nrf_audit.py --fixture` now reads the trees of both constellations: before, it listed only GPS links and left the Galileo links "not listed" (155 403 of the held-first arm's 332 377 held link-epochs).

**Re-runs.** The two ring-fix arms of the 6 h bed on exe nrf7 (`nrf7_olt_g6`, `nrf7_hf_g6`, arm files identical to the first runs' but for the run folder) and their users; the plain stream (`nrf6off_g6`) and arm G need no re-run (the ring fix is off there). Gates: `nrf7off_r3c` against `nrf6off_r3c` and `nrf7_hf_r3c` against `nrf6_hf_r3c` IDENTICAL expected (one constellation). The bars of section 16 apply unchanged to the re-runs; the first runs (`nrf6_olt_g6`, `nrf6_hf_g6`) are kept as the record and graded beside them.

**The users' starts, corrected.** A user's `start_offset_hours` counts from the product stream's first epoch (the replay anchors it there), not from the start of the observation files. Section 16's offsets (1.004167 to 4.504167, the network's 00:30:15 added) started the first user replays 30 min late (at 1 h, 2.5 h, 3.5 h and 4.5 h); those runs are discarded, `scripts/irdu_users.py` no longer adds the network's offset, and the users run with `start_offset_hours` 0.5, 2, 3 and 4 (started 30 min, 2 h, 3 h and 4 h after the stream's first epoch, as section 16 intends) on the plain stream and on the two re-run streams.

Sections 17 onwards (the requesting session's re-grade of N2b, stage N3's results, stages N3b, N2c and O1) are in [part 3](NRF_network_ring_fixing_2026-11-14_part3.md).

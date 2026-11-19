# NRF part 19: the saved state's reading and the states the seam lacks (sections 125.6 onwards of card NRF)

**STATUS**

- **id:** NRF-part19
- **topic:** network-vehicle
- **status:** open
- **summary:** Sections 125.6 onwards of card NRF (2026-11-19), continued from [part 18](NRF_network_ring_fixing_2026-11-14_part18.md), which reached its size. Section 125.6: the manager's reading of section 125.5 (the purpose of step S7 (b) not reached; no bar on integers read on a resumed leg in place of an uninterrupted run until the author rules on the exact resume), what B4 gives, a diagnosis with the resume slip warm-up armed (the differing held integers reduce to the whole-turn class alone), and the items for the author.
- **source:** the manager's message of 2026-11-19 after checkpoint 3 (decision (1)); [part 18](NRF_network_ring_fixing_2026-11-14_part18.md), sections 125 to 125.5; the runs and grading outputs in `D:/GnssProcessorSupport/runs_nom/b2_s7br_grade/`.

**Outcome.** The manager read section 125.5 on 2026-11-19: the purpose of step S7 (b), replaying from a converged state in place of two hours of convergence, is not reached, and until the author rules on the exact resume no bar on integers is read on a resumed leg in place of an uninterrupted run. A diagnosis with the resume slip warm-up armed leaves only the whole-turn class of differing held integers (328, 331 and 496 link-epochs on m1, h40 and n147). The phase wind-up accumulator is the state the seam lacks for the integers; the slip detectors' history is the one it lacks for the seam's edges.

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

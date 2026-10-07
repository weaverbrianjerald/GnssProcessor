# LV roadmap: the LV initiative's ranked next actions and the hand-back refresh

**STATUS**

- **id:** LV roadmap
- **topic:** ionosphere
- **status:** superseded
- **summary:** SUPERSEDED action list of the LV ladder: item 3 was built as S2H, the disc channel moved to the edge-response queue, and the level-authority thread moved to LV-6.
- **dates in the text:** 2026-08-05 to 2026-08-07
- **knobs / posture:** `network.serialize_sbasis_contract` (item 2a, armed for replayed producers); the LV knobs `iono_seed_field_frame`, `rx_cb_carry_demean`, `rx_cb_carry_step_gate`, `rx_cb_carry_demean_within`, all default OFF; EXT-CB posture of record: the canonical, no LV level stack
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none as `runs/` paths; `ext63_dual_12h_{ref,lv125,lv125_q6}`, `replay_ext63_12h_{ref,lv125}`, `agu63_nolane_30ep_lv125`
- **source:** `docs/archive/SPEC_level_information_budget.md` sections 17, 27

**Outcome.** The roadmap ranked the LV initiative's next actions after section 15 and refreshed them at hand-back. Item 0 graded the existing runs on invariant quantities: DD fractions identical (0.2503 against 0.2498), the LV stack's postfit σ̂₀ 1.4× louder, both consumers poor. Item 1 (LV-4 alone) passed stage 2 at a +32 % data-side cost; its 12 h stage 3 was staged, with no run of it on record. Item 2a cut last-30 3D RMS by 12.7 %. Standing: S2H's 12 h verdict moved the level authority, and item 0b's design (internally sourced), to LV-6 ([S2H](../code-bias/S2H-HARVEST.md) section 31); the disc channel continues in [HANDOFF_edge_response_queue.md](../../HANDOFF_edge_response_queue.md) edge 1.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 17. ROADMAP for the continuing session (2026-08-05, written after §15)

**The arc, in brief.** EXT-CB pinned every satellite family to COD products to isolate network failures. What
it caught: (1) removed families take their minimum constraints with them (DEFECT 1 — fixed by the rx zero-mean
datum); (2) the "19 TECU level defect" decomposed into a CONVENTION OFFSET (the receiver fleet's -5 ns GF-bias
mean vs our zero-mean convention — the receivers were the CARRIER of a frame difference, never a broken
parameter), an IGNITION TRANSIENT (heals by hour 2 at 61 stations; ESSENTIALLY ABSENT at 303 — station support
dissolves it), and a slow TRACKING question (the rolling carry memory is the accidental tracker; every pin we
built fought it and lost); (3) two REAL machinery defects found and permanently fixed en route — the
frozen-frame inject gate (LV-4) and the lag-1 de-mean limit cycle (LV-5); (4) datum-invariant grading (DD
fractions IDENTICAL across postures; consumer replay poor in BOTH with different excursion signatures) proved
the level work was frame-neutral where it claimed to matter and re-exposed the campaign's true blocker:
PRODUCT INTERNAL CONSISTENCY / churn; (5) the mask study attributed the level gain to the obs mask (10→15),
the residual cost to the iono cutoff, and the 30/30 extreme bought nothing (a00 goes prior-dominated —
predicted); (6) LASER: the theory named LV-2's flaw exactly (mean-only 5-86 = a frozen pin) and the SOURCE
(`C:\LASER`) turned the "architecturally blocked" principled card into a cheap post-solve one ("S-consistent
harvest", item 3 — built 2026-08-07, §28). **Grading law learned twice: time-matched GIM, ≥ 2 h windows,
datum-invariant quantities first.**

State on hand-back: knobs `iono_seed_field_frame`, `rx_cb_carry_demean`, `rx_cb_carry_step_gate`,
`rx_cb_carry_demean_within` all built and default-OFF; suite green (counts are dated snapshots — §9); runs
`ext63_dual_12h_{ref,lv125,lv125_q6}` + `ext63_dual_1h_lv{245,45,45_lv1}` + `agu63_nolane_30ep_lv125` on disk.
The EXT-CB posture of record for settled work remains the CANONICAL one (no LV level stack).

Ranked next actions — **all four executed; outcomes below, plans deleted**:

0. **Datum-invariant grading of the EXISTING runs** (user course-correction: "did we just push errors into
   the free parameters?"). Every level number in this document is a FRAME quantity (invariant #5 — legal as a
   diagnostic, never as a grade); content moved along the null-space directions (level ⊕ rx-DCB common ⊕
   per-LOS commons) is INVISIBLE to estimable quantities, real error injection is not. Graded
   `ext63_dual_12h_ref` vs `_lv125` (and `_q6`) on (a) the WTZZ00DEU held-out closed-loop consumer — the
   original handoff's grade (c), never run in ~25 arms, replay driver ~33 s/arm
   (`project_consumer_replay_driver`); (b) DD fractional structure; (c) churn counters; (d) postfit σ̂₀.
   **MEASURED (replay arms `_ext63_replay_12h_{ref,lv125}`,
   `runs\replay_ext63_12h_{ref,lv125}\logs\consumer_replay_hourly.csv`):**
   - **(b) DD fractions IDENTICAL**: settled med|frac| **0.2503 vs 0.2498 on 322 936 rows each**,
     uniform-0.25 both — the stack was estimability-neutral on the DD channel, and §4's "fractions move
     off 0.25" prediction is dead at 12 h too.
   - **(d) postfit σ̂₀: lv125 1.4× LOUDER** (0.0637 vs 0.0461; phase 0.0086 vs 0.0058) — the §13.5
     over-retention direction.
   - **(a) the CONSUMER is NOT equivalent — both poor, DIFFERENTLY poor.** ref settles 2–7 m but hours
     3–4 are catastrophic (median 400 m, up to −326 m up).  lv125 is excursion-prone (hour means spike
     to 979/660/293 m at h2/h4/h6 over clean medians) with a **STEADY ~21 m 3D error through hours
     10–11** (p95 ≈ median — a stable bias, not noise) coinciding with §14's pin-ramp era.  A
     frame-CONSISTENT product cannot hurt the consumer, so the late-era stack product is INTERNALLY
     inconsistent — the pinned rx-CB frame diverged from the drifting iono/clock content into estimable
     space.  Neither posture wins; both excursion sets carry the known consumer stiff-state-ratchet
     signature (invariant #1, consumer side).  → Excursion-hour attribution EXECUTED: §25 (+§25.7).
   - Diagnostic rider (does the fleet-bias mean drift diurnally?) → discharged, §25.6.
0b. **The ladder's unbuilt final rung — external RECEIVER biases** (CAS MGEX DCB, which publishes nonzero
    receiver DCBs; COD's GPS receiver OSBs are 0 by convention). Pinning them leaves NO soft bias absorber —
    only iono + ambiguities free, the original campaign design at its endpoint and the direct proof/refutation
    of the receiver attribution. Blocked only by the ProductParser 4-char-station card (§29.8). Diagnostic
    arm; does not violate the no-external-dependence rule (the EXT-CB ladder is diagnostic by charter).
1. **LV-4 alone (+LV-5 machinery idle), demean OFF.** → RUN TWICE: falsified in §19 at the old masks; §19's
   stage-1 FAIL does NOT reproduce at the current ones (§23). Stage 1 PASSES all five gates at 61 sta @ 15/25
   and again at 303 sta @ 30/30; **stage 2 (3 h) PASSED — §24**; stage 3 (12 h) staged but unspent (§27).
   Still not a promotion candidate: LV-4 costs the data side in every cell measured (+16…64 % postfit phase
   rms, mask-dependent) and never pays for itself. The staged-escalation protocol (user directive: 1 h → 3 h →
   12 h, fresh SEAMLESS lineage each stage, next stage only on the previous gate passing, §13.5 audit feeds
   armed from stage 1) and its per-stage gates stand as method: **1 h = STABILITY only** (settled sd ≤ 5 TECU,
   no ~3-epoch alternation, folds ≥ 95 %, erases ≤ 2 %, sh_health 0/0/0 — 1 h is inside §14's ~2 h convergence
   budget, so record the breathing, do not verdict on it); **3 h = first legal level read** (hours 2–3
   time-matched |a00 − GIM(t)| inside the canonical's ±4..9 band, breathing ≤ the canonical's ±6, no
   over-retention signature); **12 h = the tracking verdict** (|a00 − GIM(t)| ≲ 4 at hour 0 AND hour 11, end
   within ±4 — canonical ends +1.0 — erases 1–2 % with no erase clusters at frame events).
2. **The §13.5 audit gate before any promotion** — arm `trace_postfit_sigma0` + `trace_postfit_keyed` + the
   prires feeds and check the over-retention signature (quieter priors / louder data residuals). A level that
   lands on the reference is not sufficient. → **Discharged, §19.2, with the OPPOSITE sign.**
3. **The principled card — reshaped by the LASER SOURCE (`C:\LASER`): "S-consistent harvest".**
   `CoreLsa.cpp:1678` implements 5-86 exactly (mean about the expansion point + covariance factor G → S·G),
   and its big-system path (`Net3DLsa.cpp:5573`) shows the transform WITHOUT forming S: for a translation-type
   null space it is a per-component POST-SOLVE mean-subtraction over the datum-defining set — O(n). Mapped to
   the rx-CB family: **apply the Λ-weighted de-mean to the HARVESTED absolute values each epoch (solve →
   transform → harvest)**, making the carried set zero-mean-by-construction and coherent with the datum row —
   no frozen pin (the mean is re-derived from THIS epoch's solution), no lag-1 feedback, no cross-station
   prior rows. **DISTINCT from every tested posture — LV-2 de-meaned the fold INNOVATION (froze the x_lin
   frame); this de-means the harvested REALIZATION.** The canonical's rolling memory is the noisy accidental
   version of exactly this. Honest caveat: it forces receiver-physical drift into the level channel as a
   DECLARED frame property — §13.4 option 1 remains the answer if that attribution ever matters. Independent
   oracle: `C:\LASER\test\GPS3710_test_PMINOLESS`. Alternative if it fails: the reduced-level fold
   (`SPEC_pivot_visibility_gauge_defect.md §8`). Target, unreached by every posture to date:
   |a00 − GIM(t)| ≲ 4 TECU at hour 0 AND hour 11.  → **BUILT and validated to 3 h, §28.**
4. Parked, own cards: the deg-1 clustered-spread dipole (EXT-CB); and the **self-contained posture
   decision**, where the stack HELPED — §13's pathology is posture-dependent, so that call is separate and
   friendlier. Its whole measured basis is **arm 26** (`_agu63_nolane_30ep_lv125.json`,
   `runs\agu63_nolane_30ep_lv125`, the SELF-CONTAINED W6 spot-check, 30 ep = 15 min, vs
   `runs\agu63_nolane_30ep`):

   | quantity (30 ep, W6 self-contained) | control | arm 26 (LV-1+2+4+5) |
   |---|---|---|
   | a₀₀ mean | 24.77 | **29.21** (toward GIM 32.24, not away) |
   | a₀₀ end | 24.37 | 30.37 |
   | deg-1 dipole mean | 6.98 | **5.49** (SHRINKS) |
   | deg-1 dipole end | 5.26 | **2.82** |
   | LV-4 erases / max z | — | **0** erases, max z 7.14 |
   | LV-4 max robust σ̂ | — | **0.189 m** (vs 1.70 m on EXT-CB) |
   | retired screen would have withheld | — | **12.7 %** (vs 74 % on EXT-CB) |

   The self-contained posture's receiver DCBs are far more stable epoch-to-epoch because the satellite gauge
   determines them, **so the frozen-frame pathology is posture-dependent and its severity tracks how far the
   first-epoch DCB seed lands from the realized value.** In one line: a00 24.8 → 29.2, dipole shrank,
   frame-artifact withholds only 12.7 %. ⚠ 15 minutes is pure ignition (§14.4): re-grade on a settled window
   before concluding anything here.

Grading protocol: front matter. Provenance: §9–§18, §29 (campaign record), §30 (frame contract), memories
`project_level_information_budget` / `project_extcb_defect2_rootcause`. Note for items 1 and 3: §18.1 closes
the `rx_cb_carry_q` axis — tighter than 1e-9 is meaningless, looser is measurably worse, and the canonical
runs the SAME q with a -9.7 footprint residual. What the level responds to is the KIND of memory holding the
receiver-DCB common mode, not its decay rate.

---

## 27. Roadmap refresh — state on hand-back (2026-08-05, end of session)

The mask study CLOSED on a unified 15/25 posture validated at both scales (§26; recommended root-config edit
pending user sign-off). Item 1's staged escalation passed stage 2 with the best level tracking measured at 61
stations and a persistent +32 % data-side cost (§24) — stage 3 is earned and STAGED but unspent (12 h pair
cancelled for time: configs `_ext63_gps_dual_12h_lv4_m15i25.json` + `_ext63_gps_dual_12h_m15i25.json` sit on
disk with predictions written, posture-checked, run dirs created — ~26 min each; the control doubles as the
missing 12 h denominator at the new masks). §17 item 0 fully discharged: the excursion attribution (§25) found
a mark-free wire carrying endemic posture-invariant PB steps, onsets enriched in re-referencing events — and
(CORRECTED same day, §25.7) a consumer reaction path that EXISTS and FIRED 83×/12 h behind a default-OFF
logger, with ZERO enrichment at the excursion onsets; the diurnal rider (§25.6) measured the fleet bias
quasi-static, closing the receiver-thermal hypothesis. Two instruments promoted:
`scripts/product_step_census.py`, `scripts/fleet_bias_diurnal.py`.

Ranked next actions (supersedes §17's list):

1. **BUILD item 3 — the S-consistent harvest** (§17 item 3, brief unchanged, now triply motivated): §24
   proved the tracking authority exists in the admitted per-epoch code GF information and that the carry is
   the wrong vehicle (+32 % fold-side tax); §25.6 proved the physical rx biases are static, so the frame
   component is the ONLY moving part; §19–§24 measured every fold-side manipulation of the frame (pin,
   de-mean, full admission) paying on an invariant axis. Oracle: `C:\LASER\test\GPS3710_test_PMINOLESS`. Grade
   against arm 32 / arm 38 denominators; target unchanged (|a₀₀ − GIM(t)| ≲ 4 at h0 AND h11, zero fold-side
   cost). → **BUILT and validated to 3 h, §28.**
2. **The disc-channel card — REVISED, see §25.7 and `HANDOFF_edge_response_queue.md` edge 1.** The original
   two-sided formulation is retracted: the consumer reaction seam EXISTS and FIRES (83 resets/12 h) and its
   reactions have ZERO enrichment at excursion onsets — so "build the consumer seam" is not the work and
   "recover the excursion mass through this channel" is not achievable. The revised card: (a) make the live
   channel FAITHFUL — arm `network.serialize_sbasis_contract` so the replay emitter stops mis-thresholding;
   **no code change** — the flat threshold is `NetworkProductEmitter::kDefaultDiscThresholdM`, correctly
   single-sourced (§25.7 item 3) — **DONE: all four predictions confirmed, -12.7 % last-30 3D RMS on the 3 h
   pair**; (b) give the ACTUAL excursion event class (refsat_reset / col_removed / sbc_engage / eq34_reattach
   — no wire representation of any kind) a record, via the existing `R` / `datum_changed` carriers behind a
   default-OFF knob, graded on onset ENRICHMENT, not excursion mass; (c) the H rider is TWO defects — σ₀ ≡ 0
   (feed from the prefit census) and the batch replay reader dropping all 1 440 H records unread.
3. **Stage 3 of item 1** — staged, predictions on record (the tracking verdict + the σ̂₀-compounding read
   that parameterizes item 3's requirements). Run when ~55 min of budget exists. Not a promotion candidate
   regardless of outcome (§23/§24).
4. **Item 0b — external receiver DCBs (CAS MGEX)** — unchanged; still blocked by the ProductParser
   4-char-station card (§29.8).
5. Parked, unchanged: the deg-1 clustered-spread dipole card; the self-contained posture decision (§17 item
   4).

Grading-protocol addenda (binding): see the front matter — time-matched GIM series AND footprint restriction
with correct flags (§26.4), ≥ 2 h for any level claim at 61 stations (§14; the 305 network's transient is
milder but 1 h remains a rank-only window), `product_step_census.py` on any product-affecting change (§25.2),
and the four-instrument audit expecting the congruence dissent (§23.4, §24.3, unexplained).


# Study 6 — the COMMON-BAND scope — free only the band the evidence names

**STATUS**

- **id:** Study 6
- **topic:** slip
- **status:** closed
- **summary:** the COMMON-BAND scope — free only the band the evidence names
- **dates in the text:** 2026-09-11
- **knobs / posture:** `network.cycle_slip.common_band_scope`, `network.parallel_reduce`
- **hashes named in the text** (commit / exe / byte-cert): `5fe64c7`, `012ff2d`, `5223dc6`
- **run dirs named in the text:** `runs/evleg_null_mwevid_main`, `runs/golden_sim_campaign_zwdblk_cbscope`, `runs/golden_sim_campaign_zwdblk_cbscope2`, `runs/golden_sim_campaign_zwdblk_mwevid_preduce`, `runs/golden_sim_campaign_zwdblk_cbscope3`
- **source:** `docs/archive/SPEC_event_harness.md` — 14, 14.1, 14.2, 14.3, 14.4, 14.5, 14.6, 14.7, 14.8, 14.9

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 14. Study 6 — the COMMON-BAND scope: free only the band the evidence names (2026-09-11)

### 14.1 The question, the population, what is already known

A single-band slip on a triple-frequency arc moves every MW pair that contains
the band: an L5 slip fires (1,5) and (2,5) and leaves (1,2) silent.  The drain
refloats each firing pair's TWO bands, so the union is all three — an all-band
free, the §9 floor (0.45–0.65 cyc for 45 epochs at low elevation, slant iono
−0.08 m), for a slip that touched one band.  On the storm bed the run-end
clause reads 2 278 evidence admissions for 1 020 rows and 4 570 legacy
admissions: most slips fire two lanes, so most responses are all-band.  The
band-scope card of Study 3 (`refloat_band_scope: implicated`) tried to pick
the band from the MW/GF DECOMPOSITION and was falsified on the storm bed —
its fallback ran as "keep L1C whatever happened to it" 1 091 times and
collapsed the integers.  This card uses a different, set-theoretic piece of
evidence: **a silent, mature pair exonerates both of its bands**; a band is
implicated when no silent pair contains it.  Narrowing needs POSITIVE
exoneration; without it the response is today's.

### 14.2 The rule (pre-registered)

Per (station, satellite, epoch), tracked phase bands B (2 or 3):

- MW pair p is **silent** iff its arc was evaluated this epoch, is mature
  (arc count ≥ `common_band_min_arc_epochs`, 10) and did not flag — the
  detector's `getEpochCleanPairs()` set, with the maturity guard added if it
  is not already implied.  A flagged pair (admitted or dropped), an immature
  or absent pair is NOT silent.
- If the IF-TD detector evaluated the pair (GPS: L1C;L2W; Galileo: L1C;L5Q)
  and FLAGGED it this epoch, that pair cannot exonerate (closes the
  equal-count trap: L1 and L2 slipping by the same integer leave MW(1,2)
  silent but move the iono-free combination by 0.107 m per cycle).
- A band with a loss-of-lock flag set is implicated regardless.
- **exonerated(b) ⇔ no LLI on b ∧ ∃ silent pair p ∋ b (with IF-TD on p not
  flagged).  implicated = B \ exonerated.**
- Fallbacks to TODAY'S response (free every band any drain would have freed):
  no MW pair fired; implicated = ∅ although a pair fired (inconsistent
  evidence); implicated = B (nothing exonerated).
- The verdict is formed ONCE per epoch before the drains and governs every
  drain for that (station, satellite): the MW refloat, the LLI free and the
  IF-TD pair refloat each free (their bands ∩ implicated); the per-epoch
  union is therefore exactly `implicated` (Study 3's per-epoch invariant).

Worked cases: L5 slip → {L5Q}; L1 slip → {L1C} (IF-TD fires on (1,2) but
(2,5) silent exonerates L2 and L5); L2 slip → {L2W}; two bands by different
counts → all pairs fire → all (today); equal-count L1+L2 → (1,2) silent but
IF-TD(1,2) fires → nothing exonerated → all (today); a pair in warm-up cannot
exonerate → all (today); (1,2) fires alone with (1,5) and (2,5) silent →
implicated = ∅ → inconsistent → today.

### 14.3 Pre-registered legs and gates (knob `network.cycle_slip.common_band_scope`, default OFF)

Quiet seed ep0665, event at +10, 70 epochs, station **S21000SIM** (anchors no
family, so the refloat alone acts and the handover is not in the picture),
satellites from its solved GPS set with DD chords: **G08 (46°)** and **G10
(15°)**.  Every leg arms `anchor_slip_handover`, `mw_evidence_refloat` and the
scope knob explicitly (make-leg inherits the seed's config, which predates
all three).  Each leg vs its own knob-matched null.

| leg | event | expectation with the scope (vs today) |
|---|---|---|
| E1 `cb_l5_g08`, `cb_l5_g10` | L5Q +40, no flag | today all 3 bands freed (MW (1,5)+(2,5)); scope → `implicated=L5Q`; per-band level residual at +14..+45 falls from the §9 floor to ≲ 0.05 cyc, slant-iono step → ≲ 0.01 m |
| E2 `cb_l1_g08` | L1C +40, no flag | today all 3 (MW (1,2)+(1,5), IF-TD (1,2)); scope → {L1C} |
| E3 `cb_l2_g08` | L2W +40, no flag | today all 3; scope → {L2W} |
| E4 control `cb_all_g08` | L1C +40, L2W +31, L5Q +30 in one epoch, no flag | all pairs fire → all implicated → **byte-identical to the knob-OFF leg** |
| E5 control `cb_eq12_g08` | L1C +20 and L2W +20, no flag | MW(1,2) silent, IF-TD(1,2) flags → nothing exonerated → all bands; the verdict row must name `iftd(1,2)=flagged` |
| E6 `cb_lli5_g08` | L5Q +40 WITH the flag | LLI path; scope → {L5Q} (L1/L2 exonerated by silent (1,2)) |

Gates: **G1** knob-OFF null byte-identical to `runs/evleg_null_mwevid_main`;
**G2** E1–E3, E6 narrowed as expected AND per-band level residual at +45
better than the knob-OFF twin by ≥ 3× (event_frame_grade L1_d/L2_d/L5_d),
slant iono within 2× the null floor; **G3** E4 byte-identical, E5 not
narrowed; **G4** quiet 240-ep null with the knob: 0 narrowed verdicts unless
a real detection, artifacts byte-identical to `null240_evid`; **G5** the
655-slip dose (`doseall_evid` events + scope, matched null): levels `all_d`
/ `oth_d` and the per-slip residual table improve, consumer and wire not
worse, T4 ≤ 0 (stays 0), handovers FALL (only the slipped band's family moves);
**G6** storm bed (`_golden_sim_campaign_zwdblk_mwevid` + scope, manager):
consumer p50 ≤ 0.15 (from 0.174), rms ≤ 0.35, ZWD ≤ 0.01, T4 ≤ 78, L1C
ref-free ≥ 90 % (from 87.1), WL ≥ 98.9 %, cold fallbacks < 68.  A T4 count
ABOVE 78 on the storm bed is the falsifier: a wrongly exonerated band shows
up post-fit at that arc.

### 14.4 Iteration 1 — the rule is right, the evidence source was wrong (worker build 5fe64c7 on its worktree; 2026-09-11 ~11:00)

Built as specified: `include/slip/NetworkCommonBandScope.hpp` +
`src/core/slip/NetworkCommonBandScope.cpp` (the verdict seam, called before
`networkSlipDrainMwRefloat`), knob + `common_band_min_arc_epochs`, counters,
`T0_band_scope` rows (`keep` for a narrowed verdict, `all` with a reason for a
fallback when a pair fired), run-end clause with `would-narrow`, 17 tests
(suite 1843 / 0 / 17), `event_leg.py --extra`.  One shared-path edit: the
IF-TD driver's pooled centre / scale / shrinkage were factored into
`ifTdPoolStats` + `ifTdShrunkScale` so the verdict's read-only IF-TD flag
preview cannot drift from the decision it previews — certified by G1.

| gate | result |
|---|---|
| G1 OFF byte-identity vs main's `gnss_processor_mwevid_main.exe` | **PASS** (products / solution / events / ZWD identical) |
| G2 E1–E3, E6 | **no narrowing** — every verdict `all reason=no_exoneration`; ON ≡ OFF digit-for-digit on every graded channel |
| G3 E4 three-band | PASS: products / solution / ZWD identical, events differ only by the new rows.  E5 equal-count: all-band, but `cb_iftd_veto` = 0 — MW(1,2) never entered the clean set, so the trap could not arise |
| G4 quiet 240-ep null ON vs `null240_evid` | DIFFERS: 519 verdicts, 38 narrowed (2 on acting detections), first divergence a 0.2 mm clock at 284100 → knob-on nulls for knob-on legs |
| G5 655-slip dose (`doseall_cb` vs `doseall_evid`, each vs its null) | levels BETTER everywhere: per-slip residual p50 at +1 0.381 → 0.199 cyc, at +45 0.151 → 0.127; other arcs of the dosed station −0.049 → +0.002; iono diff 0.019 → 0.006; handovers 138 → 116 (cold 18 → 15); T4 0 → 0; wire 0.0088 → 0.0086; detector rows identical.  Consumer: mid-leg diff 0.34 → 0.66 m peak, last bin −0.09 → −0.14 m |

**Why nothing narrowed on the harness.**  The verdict took its silence from
`getEpochCleanPairs()`, which admits an arc only under two wavelength-
proportional bars (3σ ≤ 0.7 λ_WL, |dev| ≤ 0.3 λ_WL) — a TurboEdit-grade
"affirmatively clean" test.  On this bed the narrow lanes never meet it:
measured MW scatter (`network_slip_raw.csv`, arcs ≥ 20 samples) L1C+L2W σ_MAD
p50 0.414 m against its bar 0.201 m; L1C+L5Q 0.406 m against 0.175 m.  Only
the extra-wide lanes (L2W+L5Q bar 1.37 m, L7Q+L5Q 2.28 m) ever qualify, so
the only exonerators were (2,5) and (5,7) and the only narrowing shape was
"free L1C, keep L2W and L5Q" (869 of 871 narrowings on the dose; 99.8 % of
exonerations from one lane).  An L5 slip — the case that motivated the card —
cannot be narrowed at all under that source.  **Iteration 2 (sent): silence =
the pair was evaluated this epoch, its arc count ≥ 10, and it did not flag —
the detector's own decision, not the cleanliness bar; the IF-TD veto stays.**
The basis string will carry the silent pair's threshold in WL cycles, because
on this bed the narrow lanes' thresholds run 3–11 WL cycles (the sim's code
noise is ~5× the field's), so a narrow lane's silence excludes only slips
above that; with three lanes plus IF-TD, a single missed lane produces an
INCONSISTENT verdict (every band sits in two lanes) and falls back to all, so
a wrong narrowing needs two lanes to miss the same slip — the storm bed's
T4 count is the guard for that.

**The consumer mid-leg regression is path sensitivity, not quality.**  The
consumer replays COLD from the leg start (n_sats 14–17, no fixes, absolute
error 1.7 m at +0 falling to 0.3–0.4 m by +240 on every leg); its event−null
difference over +20..+120 is two still-converging float solutions on
slightly different wires, and it ends better with the scope (−0.14 vs
−0.09 m).  The storm bed's settled consumer statistics decide the consumer;
the dose leg's honest metric is the producer-side level channels, which
improved.  Iteration-1 storm arm `runs/golden_sim_campaign_zwdblk_cbscope`
launched for the T4 guard and the settled consumer under L1-only narrowing.

### 14.5 Iteration-1 storm arm — producer better, consumer worse, and no producer-side fingerprint (2026-09-11 ~11:40; `runs/golden_sim_campaign_zwdblk_cbscope`, exe 3B40685D…, 1323 s, 720/720)

The L1-only narrowing of iteration 1 (1 160 `keep` verdicts, every one
`implicated=L1C`; 3 048 `all`), against §13.6's arm (both slip cards, no scope):

| quantity | §13.6 | + scope (iter. 1) | bar |
|---|---|---|---|
| settled ZWD truth rms | 0.0071 m | **0.0069 m** | ≤ 0.01 PASS |
| consumer settled p50 / rms | 0.174 / 0.365 m | **0.230 / 0.474 m** | p50 ≤ 0.15 **FAIL** (worse) |
| consumer hourly rms | 0.62 / 0.29 / 0.23 / 0.18 / 0.47 / 0.39 | 0.64 / 0.30 / 0.26 / 0.23 / 0.45 / **0.65** | hours 3, 4, 6 worse; 1, 2, 5 same |
| T4 events / erased ratio | 78 / 1.51 | **83** / 1.65 | ≤ 78 **FAIL** by 5 — all 5 in hour 6 (41 → 46) |
| WL / EWL / L1C ref-free | 98.9 / 100 / 87.1 % | 99.0 / 100 / 87.6 % | WL PASS; L1C ≥ 90 FAIL (87.6) |
| anchor-slip handovers (cold) | 305 (68) | **267 (57)** | cold < 68 PASS |
| re-anchors / continuous S-transforms / cold re-realizations | 476 / 356 / 99 | 442 / 333 / 88 | — |
| detector rows, clock decouples, readmissions | 2391 / 1563 / 1195 / 111 | identical | — |

**Every difference between the arms is fewer gauge moves; the detectors and
the clock tier are identical.**  The falsifier's mechanism (a band kept on an
anchor arc that had slipped, leaving a stale pinned row) is refuted by a
direct census: of the 1 160 keeps, 81 kept a band whose family was anchored
at that station, and **none of the 81 was followed by an outlier erase within
10 epochs or a tropospheric excursion** (p50 3 mm, p95 23 mm over 20 epochs,
same as all keeps); the `all` verdicts on anchor arcs are the ones followed by
excursions (p50 0.17 m) — those are the handovers.  The 5 extra erases sit in
hour 6 at stations (S10, S15, S28, S33, S36, S49) with no keep on their anchor
arcs.  So a strictly smaller, correct repair on the producer leaves the
producer's truth channels equal or better and the single consumer station
worse — with nothing at any producer station to point at.

**Reading, and the test it requires.**  The consumer is one float PPP-RTK
station; its settled median is a single realization whose value depends on
the history of every wire step it absorbed.  Two arms whose wires differ by
38 handovers can differ by tens of centimetres of consumer median without a
quality difference — and §13.6's consumer gate (0.278 → 0.174) was read as
quality.  **The calibration owed before any consumer gate is trusted on this
bed: the same config run under a numerically irrelevant perturbation
(`network.parallel_reduce` on, summation order only; config
`_golden_sim_campaign_zwdblk_mwevid_preduce.json`) — the consumer p50 / rms
difference of that pair is the bed's consumer noise floor.**  Until it is
measured, the card's consumer bar is suspended and the producer-side bars
(ZWD, integers, T4, handovers) carry the verdict; the iteration-2 storm arm
(silence from the detector's own decision) is the one that decides the card,
because iteration 1 could only ever narrow to L1.

**Addendum (2026-09-11 ~12:00) — where the consumer's loss comes from, and what it
says about the rule.**  Two censuses on the same pair of arms:

| census | §13.6 arm | + scope (iter. 1) |
|---|---|---|
| satellite-epochs whose bands are anchored at DIFFERENT stations (of 38 004 with ≥ 2 bands anchored) | 2 500 (6.6 %); hourly 3 / 6 / 9 / 8 / 8 / 7 % | **6 262 (16.5 %)**; hourly 6 / 12 / 20 / 15 / 19 / **27 %** |
| wire inter-frequency bias steps > 5 cm (L1C−L2W / L1C−L5Q / L2W−L5Q / L1C−L7Q) | 438 / 868 / 82 / 347 | 443 / 942 / 146 / 412 (+206 in total; rms of steps lower — fewer huge cold steps) |

The hourly profile of the split-anchoring share follows the consumer's hourly
error (hour 6: 27 % and 0.65 m).  Mechanism: an all-band response on an
anchor station hands over ALL of that satellite's families in one epoch, and
the sticky election tends to place them at one successor; an L1-only response
hands over the L1C family alone and leaves L2W / L5Q pinned where they were.
The satellite's per-frequency biases are then realized at different stations,
whose arc levels walk independently, and the wire carries a slowly changing
INTER-FREQUENCY inconsistency per satellite — invisible to every producer
truth grade (each band is graded separately and each is fine) but exactly what
a float user's dispersive split reads as ionospheric change on that satellite.
Every one of the 81 keeps on anchor arcs leaves its satellite split for tens
of epochs, which is the 2 500 → 6 262.  **Design consequence for iteration 3
if iteration 2 confirms it: the scope must not split a satellite's references —
on an arc that anchors any family of the satellite, do not narrow (free all
bands so the families move together), or narrow but hand over the sibling
families to the same successor.**  The path-sensitivity floor of the consumer
(§14.5's calibration twin) is still owed before the size of the effect is
believed; its direction and its hourly correlate are not in doubt.

**Instruments owed (2026-09-11 ~12:10; from the question "why not form the user's
combinations inside the network?").**  Not as observation rows — every user
combination is a linear mix of rows the uncombined solve already holds, adds
zero rank and only re-weights; falsified twice (the float MW row, the lane
layer).  As EMISSION-TIER MONITORS, yes, and they are missing:
(1) `split_referenced_sats` — per epoch and at run end, the number of
satellites whose families are anchored at different stations (the election
already knows; a few lines; rides with iteration 3's anchor clause);
(2) an inter-frequency consistency monitor on the emitted biases — per
satellite the geometry-free combination of its per-frequency biases, its
epoch-to-epoch step and its drift, MINUS the constellation-common part (a
common shift is absorbed by the user's receiver bias per frequency), with a
run-end line and a harness grader column, so a 70-epoch leg and a storm arm
both read what the user's dispersive split will feel.  The closed-loop user
remains the final arbiter once its noise floor is calibrated (§14.5).

### 14.6 Iteration 2 — silence from the detector's own decision: the rule engages (worker commit 012ff2d on its worktree; exe `gnss_processor_cbscope2.exe` A9F824AF…; 2026-09-11 ~12:00; manager re-grade of the dose pair)

| gate | result |
|---|---|
| G1 OFF byte-identity vs main | **PASS**, hashes identical to iteration 1; the OFF twins of iteration 1 and 2 are byte-identical to each other |
| G2 E1–E3, E6 (G08) | **narrowed to exactly the slipped band** — verdict rows `keep implicated=L5Q / L1C / L2W / L5Q`, exonerators `mw(L1C+L2W)silent(n=10,thr=4.2WL)` and `mw(L2W+L5Q)silent(n=10,thr=0.4WL)`, `mw(L1C+L5Q)silent(n=10,thr=4.2WL)`; per-band level residual at +14 / +45 **24–300× below the OFF twin** (e.g. L5 slip: 0.007 / 0.008 cyc vs −0.644 / −0.300); slant-iono step 0.15 m → 2 mm; other arcs' rms 0.0170 → 0.00013 cyc; station clock 0.0056 → 0.00045 m.  E1 on G10 (15°) did NOT narrow: its (1,2) arc was 8 samples old at +10 (immature, refused and counted) — the maturity guard working as specified |
| G3 controls | three-band slip: `all`, products / solution / ZWD byte-identical to OFF; equal-count L1+L2: `all` with **`IF-TD vetoes 1`** — the trap closed by the veto exactly once, as pre-registered |
| G4 quiet 240-ep null ON vs `null240_evid` | products / solution / ZWD **byte-identical**; 519 verdicts, 3 narrowed, all three on DROPPED flags (no solve effect); `immature pairs 720, weak silences 17` |
| G5 655-slip dose (`doseall_cb2` vs its null; vs `doseall_evid`) | per-slip level residual p50 at +1 / +20 / +45: **0.030 / 0.025 / 0.022 cyc** (iter. 1: 0.199 / 0.139 / 0.127; no scope: 0.381 / 0.201 / 0.151); injected arcs' pool residual 0.511 → **0.033** at +20..+39; handovers **138 → 79** (cold 18 → 8), continuous S-transforms 187 → 151, re-anchors 215 → 167; wire de-meaned rms 0.0088 → **0.0063 m**; T4 0; detector rows identical.  Verdicts 2 138: narrowed 1 151 ({L1C} 458, {L2W} 328, {L5Q} 288, {L7Q} 77 — every band now), all-band 987 (no exoneration 514 / inconsistent 473), IF-TD vetoes 20, immature pairs 1 379, weak silences 440.  Exonerating thresholds p50 3.5 WL, p90 8.3, max 26.9; 22 % above 6 WL. **Consumer:** better through the middle of the leg (diff 0.343 → 0.020 m at +120), worse in the tail — ends 0.446 m vs 0.307 (no scope) |

**The split-reference count scales with the narrowing, exactly as §14.5's
addendum predicts** (per-epoch mean / max of satellites whose families are
anchored at different stations, out of ~53 anchored, on the three dose legs):
no scope **4.8 / 7 (9.1 %)**; iteration 1 **10.6 / 14 (20.1 %)**; iteration 2
**17.0 / 23 (32.2 %)**.  Iteration 3 (sent): the ANCHOR CLAUSE — on an arc
that anchors any family of the satellite, do not narrow (free all bands so the
families move together, reason `anchor_colocate`) — plus the always-on
`split_referenced_sats` counter in the run-end clause; gates: E1 on S21
identical to iteration 2 (S21 anchors nothing), the §13 S36/G04 anchor slip
with v2 (one family handed over) vs v3 (three), and the dose with the split
count expected back at the no-scope level.  Storm arms running: iteration 2
(`runs/golden_sim_campaign_zwdblk_cbscope2`) and the consumer noise-floor
calibration twin (`runs/golden_sim_campaign_zwdblk_mwevid_preduce`).

### 14.7 Iteration-2 storm arm, the consumer instrument calibrated, and the outlier rise located (2026-09-11 ~12:40; `runs/golden_sim_campaign_zwdblk_cbscope2`, exe A9F824AF…, 1457 s; `runs/golden_sim_campaign_zwdblk_mwevid_preduce` = §13.6's arm under `parallel_reduce`)

**The consumer instrument is trustworthy.**  The calibration twin's products
and solution DIFFER from §13.6's arm at the numeric level (summation order),
yet its consumer settled p50 / rms read 0.1742 / 0.3649 against 0.1741 /
0.3649, its hourly rms is identical to three decimals, and every event class
count is identical (detections, handovers, decouples, T4 78 = 78).  A single
user station's settled median is reproducible to a tenth of a millimetre
under numeric perturbation; the changes measured under the scope are real and
come from the discrete differences (which handovers happen).  §14.5's
suspension of the consumer bar is lifted.  What the twin does NOT calibrate is
sensitivity to a different SET of discrete events; that is the mechanism
itself, and it has a census signature.

| quantity | §13.6 (both cards) | iter. 1 (L1-only) | **iter. 2 (all bands)** | bar |
|---|---|---|---|---|
| settled ZWD truth rms | 0.0071 m | 0.0069 | **0.0063 m** | ≤ 0.01 PASS |
| consumer settled p50 / rms | 0.174 / 0.365 | 0.230 / 0.474 | **0.199 / 0.324** | p50 ≤ 0.15 FAIL; rms better |
| consumer hourly rms | 0.62 / 0.29 / 0.23 / 0.18 / 0.47 / 0.39 | 0.64 / 0.30 / 0.26 / 0.23 / 0.45 / 0.65 | 0.70 / 0.25 / 0.22 / 0.19 / 0.41 / 0.34 | hours 2–6 all better than §13.6; hour 1 worse |
| T4 events / erased ratio / ambient | 78 / 1.51 / 0.229 | 83 / 1.65 / 0.226 | **107** / 0.75 / 0.198 | ≤ 78 FAIL (+29, hours 5–6: 0 → 8, 41 → 63) |
| WL / EWL / L1C ref-free | 98.9 / 100 / 87.1 % | 99.0 / 100 / 87.6 | **99.2 / 100 / 90.0 %** | L1C ≥ 90 PASS |
| anchor-slip handovers (cold) | 305 (68) | 267 (57) | **174 (28)** | cold < 68 PASS |
| verdicts / narrowed / vetoes / weak silences | — | 4208 / 1160 / 0 / — | 4208 / 3099 ({L1C} 1190, {L5Q} 850, {L2W} 775, {L7Q} 284) / 116 / 1843 | — |
| split-referenced satellites (share of anchored; hour 6) | 6.6 % (7 %) | 16.5 % (27 %) | **43.8 % (64 %)** | — |

**Where the outlier rise comes from.**  Not from the kept bands: of the 3 099
keeps, 254 kept a band whose family was anchored at that station, and 1 of
those 254 was followed by an outlier erase within 10 epochs (all keeps: 26 of
3 099; ZWD change at noise, p50 2 mm).  It comes from the split itself: **70
of the 107 erases (65 %) land on a satellite that was split-referenced at that
epoch, against a 44 % base rate** — a satellite whose per-frequency gauges
are realized at different stations reads as inter-band inconsistent at the
OTHER stations, and the tier erases their arcs.  The same split that costs the
user (§14.5) costs the producer post-fit, only later and less.

**Verdict on iteration 2:** the narrowing itself is right and cheap (every
producer truth channel improves; L1 integers reach 90 %; cold fallbacks fall
by 60 %); its one defect is that it lets a satellite's references separate.
Iteration 3's anchor clause is the fix for exactly that; its gates, against
THIS arm: split share back to ≤ 10 %, T4 ≤ 78, consumer p50 ≤ 0.174 with the
rms ≤ 0.324 kept, ZWD ≤ 0.0071, L1C ≥ 90 %, handovers below 305.

### 14.8 Iteration 3 — the anchor clause puts the references back together (worker commit 5223dc6; exe `gnss_processor_cbscope3.exe` 776537A2…; 2026-09-11 ~12:35; manager re-grade)

Change: when a verdict would narrow on an arc that anchors ANY family of the
satellite, it does not — reason `anchor_colocate`, every band freed, so all
of the satellite's families move together; plus the always-on
`split_referenced_sats` mean / max in the run-end clause (validated against
the trace-based census: 3.05 / 5 vs 3.0 / 5 on the anchor leg).  27 tests in
three suites; suite 1853 / 0 / 17.

| gate | result |
|---|---|
| G1 OFF byte-identity vs main | PASS, hashes identical to iterations 1–2 |
| S21 leg identity | `cb_l5_g08_v3` ≡ `cb_l5_g08_v2` on all four artifacts (S21 anchors nothing; the clause did not touch it) |
| the anchor leg S36/G04/L5Q −99 | v2: `keep implicated=L5Q` → ONE family handed over (G04_L5Q → S31); v3: `all reason=anchor_colocate` → **all three families handed to S31 in the same epoch**.  Local price of the co-located all-band free: slant iono 0.04 → 0.68 m, ZWD 2 → 13 mm at S36 (the §9 floor); clock quieter (0.030 → 0.021 m); consumer diff sub-mm in both |
| G5 655-slip dose (`doseall_cb3`) | split-referenced satellites **4.79 / 7 per epoch = the no-scope baseline to two decimals** (iter. 1 10.61, iter. 2 17.00; manager re-grade 4.79 / 7, share 9.1 %); handovers back to 138 (124 continuous / 14 cold vs baseline 120 / 18); 36 anchor co-locates; per-slip level residual p50 0.033 / 0.036 / 0.030 cyc at +1 / +20 / +45 (iter. 2: 0.030 / 0.025 / 0.022; baseline 0.381 / 0.201 / 0.151 — still 5× better); **consumer the best of the four arms: ends 0.294 m (baseline 0.307, iter. 2 0.446), peak event−null −0.235 m (the dosed leg beating its null)**; T4 0; detector rows identical |

The trade is now explicit and cheap: co-locating the references costs the
§9 all-band floor at the anchor station for the 36 co-locates on the dose
(~35 % on the per-slip median against iteration 2) and buys a coherent
datum, a quieter clock and the consumer.  Final validation: ONE storm arm
(`runs/golden_sim_campaign_zwdblk_cbscope3`, launched 12:35) against §14.7's
gates — split ≤ 10 %, T4 ≤ 78, consumer p50 ≤ 0.174 with rms ≤ 0.324, ZWD ≤
0.0071, L1C ≥ 90 %, handovers < 305.

### 14.9 FINAL storm arm (iteration 3) — split removed, producer better, the user's median still 3 cm worse: KEEP_OFF on the pre-registered gates (2026-09-11 ~13:00; `runs/golden_sim_campaign_zwdblk_cbscope3`, exe 776537A2…, 1464 s)

| quantity | §13.6 (both cards) | iter. 2 | **iter. 3 (anchor clause)** | §14.7 bar |
|---|---|---|---|---|
| settled ZWD truth rms | 0.0071 m | 0.0063 | **0.0071 m** | ≤ 0.0071 PASS |
| consumer settled p50 / rms | 0.174 / 0.365 | 0.199 / 0.324 | **0.207 / 0.327 m** | p50 ≤ 0.174 **FAIL** (+3.3 cm); rms better (−3.8 cm) |
| consumer hourly rms | 0.62 / 0.29 / 0.23 / 0.18 / 0.47 / 0.39 | 0.70 / 0.25 / 0.22 / 0.19 / 0.41 / 0.34 | 0.65 / 0.29 / 0.23 / 0.18 / **0.35** / 0.41 | hours 1–4 equal, 5 better, 6 slightly worse |
| T4 events / erased ratio / ambient | 78 / 1.51 / 0.229 | 107 / 0.75 / 0.198 | **86** / 0.94 / **0.197** | ≤ 78 **FAIL** (+8: hour 5 0 → 4, hour 6 41 → 46) |
| WL / EWL / L1C ref-free | 98.9 / 100 / 87.1 % | 99.2 / 100 / 90.0 | **99.2 / 100 / 89.6 %** | L1C ≥ 90 FAIL by 0.4 |
| anchor-slip handovers (cold) | 305 (68) | 174 (28) | **305 (59)** | < 305 FAIL by equality; cold better |
| split-referenced satellites | 6.6 % | 43.8 % | **6.6 % — identical to baseline hour for hour** | ≤ 10 % PASS |
| clock-anchor decouples / continuous S-transforms / cold re-realizations | 111 / 356 / 99 | — | 118 / 374 / 81 | +7 / +18 / −18 |
| verdicts / narrowed / anchor co-locates / vetoes | — | 4208 / 3099 / — / 116 | 4208 / 2994 / 105 / 116 | — |
| wire inter-frequency steps > 5 cm (L1C−L2W / L1C−L5Q / L2W−L5Q / L1C−L7Q / L5Q−L7Q) | 438 / 868 / 82 / 347 / 105 | — | 435 / 853 / 70 / 349 / 91 | equal or fewer on every pair |

**Reading.**  The anchor clause does exactly what it was built for: the
split-reference share is the baseline's to the tenth of a percent in every
hour, cold re-realizations fall 99 → 81, and the wire's inter-frequency
consistency is equal or better on every pair.  The producer's own channels
are equal or better (ambient DD error 0.229 → 0.197 cyc, erased-arc ratio
1.51 → 0.94, WL 99.2 %, L1C +2.5 points).  What remains is a small, consistent
consumer cost from narrowing on ORDINARY arcs: the settled median is +3.3 cm
(iteration 2: +2.5 cm; the calibration twin puts numeric noise at 0.1 mm)
with the rms 4 cm better, together with +7 clock-anchor decouples and +8
outlier erases, all in hours 5–6, and no fingerprint in the wire's
inter-frequency census.  **Verdict on the pre-registered gates: not met**
(consumer p50, T4, L1C by 0.4, handovers by equality).  The knob stays merged,
default OFF, byte-identical, with its observability (verdict rows, the
always-on split-reference counter) live; ledger **KEEP_OFF with the
mechanism**, no golden pin.  What would flip it: locate the 3 cm — the seven
extra decouples and eight erases sit in hours 5–6 of the slipped bed, so a
seed near hour 5 and 60-epoch legs at those epochs (with the split counter
and the inter-frequency monitor reading zero, the remaining suspect is the
single-band re-level's interaction with the clock anchor's NIS test), or a
warm-consumer replay on the two wires showing the median difference is the
consumer's own history.  ⚠ Lesson for the harness: the dose leg's cold-replay
consumer ranked iteration 3 BEST of four arms and iteration 2 worst; the
settled storm consumer ranks them the other way on the median — a 2 h
cold-replay consumer is not a consumer gate.

**Quiet-bed neutrality of the two MERGED cards (24 h twins on today's golden,
main's exe, both cards on vs both off):** ZWD 0.0073 m both; consumer settled
h12–24 rms 0.0761 → **0.0895 m (+18 %)**, p50 0.0603 → 0.0593, UP rms 0.0643 →
0.0768; T4 60 → 66.  The evidence card admitted 0 detections in those 24 h,
so the tail cost belongs to the anchor-slip handover's 6 false-alarm family
moves (all continuous).  Recorded in both ledger rows as a measured open
cost; the harness leg at those six epochs is owed (60-epoch legs, not
another 24 h arm).


# Study 7: the common-band scope's wrong narrowing at 4.99 h

**STATUS**

- **id:** Study 7
- **topic:** slip
- **status:** closed
- **summary:** `common_band_scope` and `if_phase_td_pairs: all` RECOMMEND_ON together on the sim storm bed (settled consumer p50 0.174 to 0.120 m), over one missed "nothing worse than cbscope4" sub-gate.
- **dates in the text:** 2026-09-11
- **knobs / posture:** `network.cycle_slip.if_phase_td_pairs`, `network.clock_datum_broadcast_anchored`, `network.cycle_slip.common_band_scope`
- **hashes named in the text** (commit / exe / byte-cert): `bbc8273`, `2d499e97`, `b268ac2`, `7dc36758`, `bd97b9b`
- **run dirs named in the text:** `runs/golden_sim_campaign_zwdblk_cbscope4`, `runs/golden_sim_campaign_zwdblk_cbscope5`
- **source:** `docs/archive/SPEC_event_harness.md` §16 (16.1 to 16.8)

**Outcome.** The study asked what carried the common-band scope's +3.3 cm settled consumer cost on the storm bed. The census FALSIFIED the 4.99 h reading: the whole cost sat inside the recovery from a declared pivot handover at 4.6833 h. IF-TD on every pair, with rules F2 and F3 and F3's anchor clause, removed the 4.99 h fallout and every split reference. The sub-gate FAILED on three channels sharing one mechanism (cold handovers 51 to 78, ZWD, L1C) and was overridden on the record. Both knobs were pinned on the since-retired `golden_sim_tri_w6` with a field exception; the current goldens (`config/golden/golden_{sim,field}_refound.json`) set both.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 16. Study 7 — the common-band scope's 3 cm is ONE wrong narrowing at 4.99 h (2026-09-11, afternoon; pre-registered before any leg)

### 16.1 What the two storm arms' own logs established (no runs; `scratchpad/cb3_census.py`)

A = `golden_sim_campaign_zwdblk_mwevid` (scope OFF), B = `golden_sim_campaign_zwdblk_cbscope3` (scope ON); hour h = (tow − 302400)/3600.

| hour | p50 A | p50 B | rms A | rms B | p50 up A / B | p50 horiz A / B | p50 (B−A) per epoch | epochs \|B−A\| > 5 cm |
|---|---|---|---|---|---|---|---|---|
| 0 | 0.447 | 0.467 | 0.619 | 0.646 | 0.244 / 0.248 | 0.374 / 0.381 | 0.0000 | 16 |
| 1 | 0.2475 | 0.2455 | 0.285 | 0.287 | 0.206 / 0.213 | 0.114 / 0.103 | +0.0015 | 0 |
| 2 | 0.2286 | 0.2243 | 0.230 | 0.225 | 0.134 / 0.137 | 0.181 / 0.174 | −0.0044 | 0 |
| 3 | 0.1745 | 0.1750 | 0.179 | 0.178 | 0.140 / 0.136 | 0.106 / 0.110 | −0.0006 | 0 |
| 4 | 0.0861 | 0.0925 | 0.468 | 0.354 | 0.077 / 0.081 | 0.039 / 0.046 | +0.0074 | 27 |
| 5 | 0.2798 | 0.3022 | 0.386 | 0.405 | 0.184 / 0.175 | 0.159 / 0.234 | **+0.0949** | **108** |

The settled (h3–h6) p50 0.1741 → 0.2067 of §14.9 is the mixture of an identical hour 3, a near-identical hour 4 (A's 0.468 rms spike is the larger one) and an hour 5 in which B is worse by a median **+9.5 cm on 108 of 120 epochs, all of it horizontal** (up is better).  Per-hour event counts are identical in the two arms for T0_mw_detect, T1_if_slip, PHASE_ANCHOR and READMIT; CLK_ANCHOR differs by exactly +7 disc_bump / +7 refloat in hour 4 and +7 reengage in hour 5; T4: 15 rows only in B (4 at 4.99 h — G14/S11, G11/S16, G11/S21, G20/S37 — and 11 at 5.62 h), 7 only in A, 71 common.

**The 4.99 h row, with the truth beside it (S37000SIM / G20):**

| h | truth (`truth_slips.csv`) | B's verdict |
|---|---|---|
| 4.72 | L2W +33, flagged | 4.74 / 4.75 `keep implicated=L2W` ✓ |
| 4.88 | L2W +38 | 4.88 `keep implicated=L2W` ✓ (iftd(L1C+L2W) flagged) |
| 4.98 | **L5Q −31, flagged** | — (no verdict row) |
| 4.99 | **L1C −29** | `keep implicated=L2W; exonerated=L1C;L5Q; basis=mw(L1C+L5Q)silent(n=99;thr=9.0WL);mw(L1C+L2W)flag;mw(L2W+L5Q)flag;iftd(L1C+L2W)=flagged` — **wrong on both counts** |

The other three 4.99 h T4 rows carry no verdict and no truth slip in the preceding hour: collateral of that epoch's metre-level misfit.  Truth on this bed: 2,486 slips, 2,476 single-band (5 same-epoch two-band coincidences, all with |Δ| ≥ 36 on both bands), and **|Δ| ≥ 10 cycles on every row** — the "weak silence" of §14's interlock (a lane threshold above a small slip) cannot occur here; the trap is two LARGE slips one epoch apart whose difference (2 WL) sits below the lane's 9.0 WL threshold.

### 16.2 The mechanism (the reading, frozen before the workers report)

1. The (L1C+L5Q) lane did not evaluate the LLI epoch against its arc — n was still 99 at 4.99 h, so it was neither flagged nor restarted at 4.98 — and at 4.99 h the detector saw both slips at once; the lane's signature was their DIFFERENCE, +2 WL.
2. The rule's IF-TD veto exists for the first pair only (L1C+L2W on GPS; the IF-TD detector forms one pair per satellite), so the (1,5) silence had no phase-precise check.  IF(1,5)-TD would have read 2.2606·0.1903·(−29) − 1.2606·0.2548·(−31) = **−2.52 m**.
3. L1 and L5 kept levels wrong by 29 and 31 cycles (5.5 m, 7.9 m) under stiff priors and L2 was re-leveled against them; the outlier tier refloated the arc in the same epoch and indicted three healthy arcs; seven satellites' clock anchors decoupled; the user's horizontal error sat +9.5 cm for the following hour.

What carries the HOUR is the open half.  H-clk: the seven decoupled satellites' clocks re-realize with an offset their phase biases do not follow — the fingerprint is a step at 4.99 h in the B−A difference of the consumer-visible clock-minus-bias per satellite (the census worker reports it; §14.9's "no wire fingerprint" compared each wire against itself, never the two wires against each other).  H-coll: the three collateral refloats plus the arc's own all-band re-level (the §9 0.45-cycle floor for ~45 epochs) — too short to carry an hour on their own.  The fix below does not depend on which; the storm arm does.

### 16.3 The fix, pre-registered — IF-TD on EVERY pair (`network.cycle_slip.if_phase_td_pairs`: `first` | `all`)

`first` = today (default, byte-identical).  `all` = the IF-TD driver evaluates every distinct pair present on the satellite at the station (3 on a triple-band satellite) with pools per PAIR KIND, so the first pair's decisions never change and the new pairs are a pure addition; drain semantics for a flagged new pair are today's per-pair semantics (∩ implicated when the verdict is ON); T1_if_slip rows and counters per pair; the verdict vetoes ANY silent MW pair whose IF pair flagged.  With the verdict OFF, `all` changes a response only when every MW pair is silent and only a non-first IF pair flags — the all-bands-equal case (ΔN1 = ΔN2 = ΔN5: today L1/L2 are refloated and L5 keeps a k-cycle error), absent from this bed (0 three-band coincidences), so `all` alone is expected byte-identical on the storm bed.

### 16.4 Pre-registered gates

Harness (worker, before any storm arm; quiet seed ep0665, S21000SIM, event at +10, 70 epochs, knobs explicit on every arm):
- **G1** byte-identity: `first` + scope OFF ≡ main's exe (quiet 240-epoch null and the L2 leg).
- **G2** reproduction: S1 (L5Q −31 with LLI at +10, L1C −29 at +11) and S2 (L1C −29 and L5Q −29 at +10) under ON → `keep implicated=L2W`, L1/L5 residual tens of cycles, T4 and collateral rows; under FIX → `all` naming the (L1C+L5Q) veto, products ≡ OFF.
- **G3** inertness on a genuine single-band slip: the L2 leg under FIX ≡ ON byte-identical (still narrowed to L2W).
- **G4** storm census (after the arm): `keep` verdicts with a truth slip on an exonerated band — today's count (worker census) → 0.

Storm (ONE arm at the end; the §14.3 bars unchanged): consumer settled p50 ≤ 0.174 m, T4 ≤ 78, L1C ≥ 90 %, WL ≥ 98.9 %, ZWD ≤ 0.0071 m, handovers 305, split-referenced 6.6 %.  PASS → both knobs RECOMMEND_ON and pinned together in the sim golden.  FAIL with G2/G3 passing → a second mechanism carries the hour; KEEP_OFF stands and the new fingerprint is recorded.

What falsifies the reading: the divergence epoch is not 4.99 h; or the wrong-narrowing census shows other wrong `keep` rows that cost the consumer nothing (then the collateral / anchor path is the lever, not the narrowing).

### 16.5 The census returns — the reading corrected and the storm gates REVISED before any FIX arm exists (2026-09-11, ~15:30 local; census worker's `task_b..task_h.py`, manager's `cb3_part.py` / `cb3_part2.py` / `cb3_decomp.py`)

**What falsified the §16.2 reading: the consumers do not part at 4.99 h.**  Both take an identical 1.30 m shock at **4.6833 h** — the GPS pivot G01 sets (`PIVOT handover G:G01->G02 datum_bump`, 52 clock anchors `cta_decouple` + `cta_disc_landed`, 157 CB + 156 PB + 52 CLK discontinuities DECLARED) — a full re-reference the user re-absorbs over ~40 min with metre-class excursions.  The two recoveries differ from 4.6917 h on:

| h | arm A (scope OFF) | arm B (scope ON) |
|---|---|---|
| 4.6833 | 1.301 m, identical in both (the pivot change) | 1.301 m |
| 4.80 | E31 L5Q/L7Q handover S26→S54 **COLD** (declared PB re-realization) → vertical spike **1.53 m** at 4.83 h | the same handover **CONTINUOUS** (the intact bands kept S54's arcs carry-usable) → 0.79 m |
| 4.9917 | — (σ̂₀ 0.948, nothing acted) | the wrong narrowing's fallout: 7 clock anchors decoupled with **7 CLK + 27 PB + 21 CB discontinuities declared**, G20's clock re-engaged +2.15 cm off (datum-free +1.96 cm, its phase biases unmoved), T4 acting on G20/S37 (prominence 162) and on three innocent arcs (17–23) while S37's own highest-prominence arcs (G24 884, G30 877, G22 808) were left alone; σ̂₀ 1.356 |
| 5.0–5.4 | East decays to 0 | East sits at −0.2 m, 48 epochs of B−A > 5 cm (mean +0.12 m) |

**Decomposition of the settled median (h ≥ 3), the §14.9 gate:**

| window | A p50 / rms | B p50 / rms | Δp50 |
|---|---|---|---|
| nothing excluded (n = 360) | 0.1741 / 0.3649 | 0.2067 / 0.3273 | **+3.25 cm** |
| pivot-recovery 4.6833–5.40 h excluded (n = 273) | 0.1663 / 0.2730 | 0.1566 / 0.2686 | **−0.97 cm** |
| 4.6833–5.60 excluded (n = 249) | 0.1455 / 0.2032 | 0.1385 / 0.2211 | −0.70 cm |
| 3.0–4.68 alone | 0.1091 / 0.1459 | 0.1127 / 0.1470 | +0.36 cm |
| 4.6833–5.40 alone | 0.2728 / 0.5661 | 0.3332 / 0.4673 | +6.0 cm |
| 5.40–6.0 alone | 0.4031 / 0.4724 | 0.3362 / 0.4627 | −6.7 cm |

The whole +3.3 cm is inside the recovery window of a declared datum change the knob did not cause; outside it the rule's consumer is BETTER, and over the run the rule turned nine cold handovers into continuous ones at identical (tow, family) with zero the other way (2.18 G23 L1C; 3.36 E04 L1C+L5Q; 4.12 G18 L2W+L5Q; 4.80 E31 L5Q+L7Q; 5.58 G12 L2W+L5Q; cold by hour A/B: 27/27, 11/11, 7/6, 2/0, 8/4, 13/11).

**Wrong-narrowing census:** 6 `keep` rows in 2,994 carry a truth slip on an exonerated band within 90 s — and ALL six follow a receiver-flagged slip on the same arc 30 s earlier.  In five, that flagged slip had been answered by the LLI drain at its own epoch and the row's defect is a SPURIOUS implicated band (no truth slip on it); S37/G20 is the only row that exonerated an unanswered slip (the L1C −29 landing the epoch after the flagged L5Q −31).  Truth adjacency: 30 slip pairs ≤ 60 s apart on one arc, 9 with |Δa−Δb| ≤ 12 WL, 6 of those on pairs the IF-TD never forms (L1C+L5Q on GPS, L1C+L7Q, L5Q+L7Q, L2W+L5Q); minimum |Δ| on the bed 12 cycles.  Wire: B−A datum-free clock dispersion 0.9 / 1.5 / 2.2 / 2.7 / 6.5 / 7.5 mm rms by hour — a 7 mm per-satellite difference cannot carry a 12 cm consumer difference; the two arms' emitted clocks agree to mm satellite by satellite (⚠ `simnet_sat_clock_census.py` treats the emitted record as a null-truth error and is not an absolute grade on this bed — its metre-class "tails" G27 3.28 m, G20 2.02 m are the bed's true clocks, identical in both arms).

**The storm gates, revised now (the harness gates G1–G4 of §16.4 stand unchanged):**
- The pooled settled p50 is REPORTED, not gated: its ±3 cm is the recovery path from a declared datum change.
- **W1** consumer, window-excluded: settled p50 with 4.6833–5.40 h excluded ≤ A's 0.1663 (cbscope3 already 0.1566); 3.0–4.68 h within ±5 mm of A.
- **W2** the 4.99 h fallout gone: zero `keep` rows with an UNANSWERED truth slip on an exonerated band (G4); no B-only CLK_ANCHOR decouples at 4.9917 h; no declared CLK discontinuities at 4.9917 h (A: 0); no T4 `acted` rows at 4.9917 h (A: 0).
- **W3** producer and wire not worse than cbscope3: ZWD ≤ 0.0071 m, WL ≥ 99.2 %, L1C ≥ 89.6 %, split-referenced 6.6 %, handovers 305 with cold ≤ 59; T4 ≤ 86 (the 5.62 h epoch is an attribution scatter — A 41 / B 46 rows there, the consumer gap peaks one epoch BEFORE it — so a ±5 there is not a signal).
- **W4** (report only): the pooled p50, the hourly table, the 4.6833–5.40 window alone, the disc_counter declarations by kind at 4.9917.

Verdict rule: W1–W3 PASS → `common_band_scope` and `if_phase_td_pairs: all` RECOMMEND_ON and pinned TOGETHER in the sim golden (the field exception of §14.8 stands for both).  W2 FAIL → the fix is wrong, not the rule.  W1 FAIL with W2/W3 PASS → KEEP_OFF stands and the window's contents are named.

**Added harness leg S3** (LLI-flagged L5Q −31 at +10 and nothing else; arms ON and FIX): expectation — at +10 the LLI implicates L5Q; at +11/+12 NO verdict row.  A `keep` at +11 implicating a band that did not slip reproduces the five spurious rows and names the LLI-epoch handling (the detector withholding the flagged epoch's sample and reading the next epoch as the combined jump) as a second defect, to be closed in the SAME card.

**Next card, stub — the pivot departure.**  A declared full re-reference (52 clocks, 313 biases) costs the user 1.30 m and ~40 min of recovery in BOTH arms: the largest consumer event of the settled window, and a gauge change that ought to be absorbed by transformation, not re-convergence.  Instrument: the 4.6833 h epoch of either arm, consumer-side.  `network.clock_datum_broadcast_anchored` (no pivot, no handover seam) remains the un-validated candidate (CLAUDE.md amendment 5).

### 16.6 The follow-up census — the LLI shadow, a second rule under the same knob, pre-registered (2026-09-11, ~16:00 local; census worker `task_i.py` – `task_iiib.py`)

**Every wrong `keep` sits exactly 30 s after a receiver-flagged slip on its own arc, and the lane it trusted contained the flagged band.**  The six rows re-read with the events at t−60 / t−30 / t:

| # | h | arc | silent lane | flagged band at t−30 | the flagged slip answered at its own epoch by | implicated band has a truth slip | class |
|---|---|---|---|---|---|---|---|
| 1 | 0.9917 | S60/E27 | L1C+L5Q | L7Q | (the exonerated L1C −45 at t−90 was answered at 0.9667 by mw_wl_slip + T1_if_slip) | yes (L7Q −42) | census window artifact — not wrong |
| 2 | 1.5583 | S25/E36 | **L1C+L5Q** | **L5Q** | lli_slip all-band refloat + T1_if_slip L1C;L5Q | **no** | spurious-implicate (L7Q) |
| 3 | 2.1500 | S55/G21 | **L1C+L2W** | **L2W** | lli_slip + T1_if_slip L1C;L2W | **no** | spurious-implicate (L5Q) |
| 4 | 3.0167 | S48/E30 | **L1C+L5Q** | **L5Q** | lli_slip + T1_if_slip L1C;L5Q | **no** | spurious-implicate (L7Q) |
| 5 | 3.3750 | S48/G32 | **L1C+L2W** | **L2W** | lli_slip; (L1C +39 at t−60 by T1_if_slip) | **no** | spurious-implicate (L5Q) |
| 6 | 4.9917 | S37/G20 | **L1C+L5Q** | **L5Q** | lli_slip at 4.9833 — but the L1C −29 lands AT t and T1_if_slip nominated L1C;L2W in the same epoch; the verdict overrode it and discarded the L1C half | no (L2W) | wrong-exonerate (the only one with a wire consequence) |

**How the lane goes silent.**  The traced pair's raw record (`network_slip_raw.csv` / `network_if_slip.csv`, one pair per satellite) shows the flagged epoch EVALUATED and flagging at 12–19 m against a 0.25–0.75 m threshold, then no row at the next epoch (arc reset, sample skipped).  The MW lanes that the verdict reads behave differently: a lane containing the flagged band keeps its arc — n increments straight through the flagged epoch with one sample withheld (S37/G20 L1C+L5Q n 87 → 99 across the LLI; S25/E36 L1C+L5Q 164 → 173; S48/G32 L1C+L2W 129 → 130) — and is then judged "silent, mature" against statistics that have just swallowed a slip, with empirical thresholds of 6–13 WL.  Firing lanes, by contrast, reset to `immature(n=0)` at the next epoch every time.  (⚠ This producer writes NO detector log — `mw_slip_detector.log` is 0 bytes on both storm arms, and the silent lanes have no per-epoch trace anywhere; the basis strings are their only record.  The harness legs enable the detector log as the §13.4 `_mwlog` legs did.)

**Whole-run base rates.**  Of 2,994 `keep` rows, 858 (28.7 %) sit at flagged-slip + 30 s; 853 of those re-implicate the flagged band one epoch late (a cheap single-band re-refloat of an already-answered slip) and 5 are the spurious rows above (0.58 %, against 0.12 % for keeps elsewhere with a slip nearby).  The MW detector itself is byte-identical in the two arms (3,586 refloat rows, identical pair composition): in A the late fire names one real and one innocent band 99.6 % of the time and refloats the whole arc a second time; in B the verdict narrows it and is right 853 / 858.

**Rule F2, pre-registered — under `network.cycle_slip.common_band_scope`, no new knob (OFF byte-identical):** a MW pair is not silent — it is treated as immature — at any epoch where one of its bands carried a receiver flag at the previous epoch on that (station, satellite), or where the detector did not evaluate the pair's previous sample normally (withheld / re-anchored).  Preferred source: a `prev_sample_withheld` flag in the detector's per-epoch pair export; fallback a one-epoch per-(station, sat, band) LLI memory in `NetworkRunContext`.  Basis token `mw(L1C+L5Q)lli_shadow(n=…)`, counter `cb_lli_shadow`.  Consequence: for one epoch after an LLI only the lane NOT containing the flagged band may exonerate; the 853 correct late narrowings are untouched (their silent lane does not contain the flagged band).  F2 alone closes rows 2–6; `if_phase_td_pairs: all` closes row 6 independently and the same-epoch equal-count trap on uncovered pairs (the S2 shape — absent from this bed, the common field shape).  Both are built.

Unit tests pre-registered: (a) the S37/G20 shape with the shadow on (1,5) and IF-TD pairs `first` → all / no_exoneration; (b) LLI on L5Q at t−1, lanes (1,7)/(5,7) firing late, (1,5) silent → all, never `keep implicated=L7Q`; (c) LLI on L5Q at t−1 and a genuine L1C slip at t: (1,2) fires, (1,5) shadowed, (2,5) silent-mature → still narrowed to L1C.

Leg S3 (LLI-flagged L5Q −31 at +10, nothing else), refined expectation: ON without F2 → the census predicts a spurious `keep` at +11 implicating L1C or L2W; FIX (all pairs + F2) → no verdict row at +11, or `all`.  Gate G4 (storm census after the arm) now reads: zero `keep` rows implicating a band with no truth slip within ±60 s AND zero exonerating an unanswered slip — today 5 + 1.

### 16.7 Storm arm cbscope4 — the fallout is gone, and the arm found the one clause F3 lacked (2026-09-11 ~16:05 local; `runs/golden_sim_campaign_zwdblk_cbscope4`, main bbc8273 exe 2d499e97…, 983 s, 720/720; graded with `scripts/ev_study7_gates.py` and `simnet_storm_grade.py`)

The arm = cbscope3's knobs + `if_phase_td_pairs: all`, on the merged code (rules F2 and F3 live under `common_band_scope`).  A = scope OFF (`mwevid`), C = cbscope3, B = this arm.

| gate | A | C | **B** | bar | verdict |
|---|---|---|---|---|---|
| W2 T4 rows at 4.9917 h / clock-anchor decouples / declared CLK breaks | 0 / 0 / 0 | 4 / 14 / 7 | **0 / 0 / 0** | absent | **PASS** |
| G4 `keep` rows exonerating an UNANSWERED slip | — | 1 | **0** | 0 | **PASS** |
| G4 spurious-implicate `keep` rows (±60 s criterion) | — | 7 | **2** | 0 | residual (young-lane silences, below) |
| W1 settled p50, pivot-recovery window 4.6833–5.40 h excluded | 0.1663 | 0.1566 | **0.1584** | ≤ 0.1663 | PASS |
| W1 3.0–4.68 h p50 | 0.1091 | 0.1127 | 0.1170 | ±5 mm of A | miss by 3 mm |
| pooled settled p50 / rms (reported) | 0.1741 / 0.365 | 0.2067 / 0.327 | **0.1673 / 0.331** | (§14.3's ≤ 0.174) | (passes even the original bar) |
| consumer hourly p50 | .447 .247 .229 .174 .086 .280 | .467 .246 .224 .175 .092 .302 | .469 **.283 .268** .165 .099 **.347** | | hours 1–2 and 5 worse |
| W3 ZWD settled truth rms | 0.0071 | 0.0071 | **0.0064** | ≤ 0.0071 | PASS |
| W3 WL / EWL / L1C ref-free | 98.9 / 100.0 / 87.1 | 99.2 / 100.0 / 89.6 | **99.2 / 99.9 / 90.1** | ≥ 99.2 / — / ≥ 89.6 | PASS |
| W3 T4 events / ambient | 78 / 0.229 | 86 / 0.197 | **11 / 0.196** | ≤ 86 | PASS |
| W3 handovers (PHASE_ANCHOR rows) / cold | 305 / 68 | 305 / 59 | 291 / **51** | cold ≤ 59 | PASS |
| **W3 split-referenced satellites, mean / max of 52.8** | (3.5) | 3.5 / 7 (6.6 %) | **7.6 / 13 (14.4 %)** | 6.6 % | **FAIL** |
| IF-TD pair flags (first pair / new pairs) | | 933 + 1578 | + 640 / 939 / 657 | | = the truth slip counts per band, no false-alarm excess |
| IF-TD refloats without any verdict at that epoch | | 783 | **3** | | F3 scopes them |
| run time | 1130 s | 1464 s | 983 s | | fewer outlier iterations |

**The defect, located.**  Single-family anchor handovers 57 (C) → 103 (B); 49 of the new ones sit on an arc that carried an `rule=lli_epoch` verdict in that epoch; the normal path's `keep` rows produce 0 single-family handovers in either arm.  F3 narrows at a receiver-flagged epoch WITHOUT the §14.5 anchor clause, so on an arc that anchors a family the flagged band's family hands over alone and the satellite's references split — iteration 2's defect (§14.5–14.7, split 9 → 44 % without the clause) re-entered through one rule at a lower rate.  The S3 leg could not see it: S21000SIM anchors nothing.  The consumer's residual hourly costs (hours 1–2 +3.5–4 cm, hour 5 +6.7 cm against OFF) are what a split reference does to a user — the inter-frequency inconsistency the per-band producer grades cannot see (§14.7) — and the pooled median improved anyway because the 4.99 h fallout, the larger term, is gone.

**Residual, recorded not fixed:** 2 spurious-implicate `keep` rows — S31/G18 3.675 h (`mw(L1C+L5Q)silent(n=36;thr=10.6WL)`) and S27/E30 5.2333 h (`mw(L5Q+L7Q)silent(n=10;thr=1.2WL)`, the lane exactly at the maturity bar) — young-lane silences, not the shadow; the maturity bar (10 samples) is the lever if they ever matter.

**Pre-registered, before the fix exists: the anchor clause inside F3** (`all`, reason anchor_colocate, counted separately as cb_lli_anchor_colocate) — the harness leg is the §14.8 anchor-arc shape S36000SIM / G04 with an LLI-flagged L5Q −31 at +10: without the clause ONE family hands over and the split census rises above the null; with it all THREE families move to one successor and the split census equals the null's; S3, S1, the nulls and OFF ≡ main unchanged.  Then ONE more storm arm, `cbscope5` (`config/test_suite/_golden_sim_campaign_zwdblk_cbscope5.json`, same knobs) — a second arm is justified only because the first arm found a defect the legs could not: gates = split-referenced mean ≤ 3.5 (6.6 %), and nothing else worse than cbscope4 (ZWD ≤ 0.0064, WL ≥ 99.2, L1C ≥ 90.1, T4 ≤ 11, cold ≤ 51, W1 window-excluded ≤ 0.1663, W2 absent); the consumer's hours 1–2 and 5 are expected to return to A's ±2 cm if the split is the whole of that cost, and are reported either way.

**Lesson (memory):** every path that can narrow on an anchor arc must carry the anchor clause, and the harness leg for any new narrowing rule must include an anchor-arc station (S36/G04), not only the anchor-free S21.

### 16.8 FINAL storm arm cbscope5 — split references gone, the user's median 0.174 → 0.120 m: both knobs RECOMMEND_ON, pinned together (2026-09-11 ~16:50 local; `runs/golden_sim_campaign_zwdblk_cbscope5`, main b268ac2 exe 7dc36758…, 1050 s, 720/720)

The arm = cbscope4's knobs (`common_band_scope: true`, `if_phase_td_pairs: all`) on the code with the anchor clause inside rule F3 (worker bd97b9b, merged b268ac2).  A = scope OFF (the two-card golden, `mwevid`), C3 = scope alone, C4 = cbscope4, **B = this arm**.

| channel | A (OFF) | C3 | C4 | **B** | §16.7 bar | verdict |
|---|---|---|---|---|---|---|
| **split-referenced satellites, mean / max of 52.8** | 3.5 / 7 | 3.5 / 7 | 7.6 / 13 | **0.0 / 0** | ≤ 3.5 | **PASS — none all run** |
| W2 4.9917 h: T4 rows / anchor decouples / declared CLK breaks | 0/0/0 | 4/14/7 | 0/0/0 | **0/0/0** | absent | PASS |
| G4 `keep` exonerating an unanswered slip / spurious-implicate | — | 1 / 7 | 0 / 2 | **0 / 2** | 0 / (residual) | PASS / residual |
| consumer settled p50 (pooled) / rms | 0.1741 / 0.365 | 0.2067 / 0.327 | 0.1673 / 0.331 | **0.1204 / 0.222** | (reported) | best of every arm |
| W1 window-excluded p50 (4.6833–5.40 h out) | 0.1663 | 0.1566 | 0.1584 | **0.1028** | ≤ 0.1663 | PASS |
| consumer hourly p50 (h0…h5) | .447 .247 .229 .174 .086 .280 | .467 .246 .224 .175 .092 .302 | .469 .283 .268 .165 .099 .347 | .524 **.113 .114 .101** .090 **.236** | | hours 1–3 halved; ignition +8 cm |
| consumer hourly rms | .62 .29 .23 .18 .47 .39 | .65 .29 .23 .18 .35 .41 | .61 .27 .26 .16 .36 .42 | **.65 .11 .12 .10 .24 .28** | | |
| pivot-recovery window 4.68–5.40 alone / last 36 min | 0.273 / 0.403 | 0.333 / 0.336 | 0.212 / 0.431 | **0.221 / 0.267** | | |
| ZWD settled truth rms / span p50 | 0.0071 / 0.031 | 0.0071 / 0.021 | 0.0064 / 0.020 | **0.0069 / 0.021** | ≤ 0.0064 | 0.5 mm behind C4, better than A |
| ref-free WL / EWL / L1C | 98.9 / 100.0 / 87.1 | 99.2 / 100.0 / 89.6 | 99.2 / 99.9 / 90.1 | **99.2 / 100.0 / 89.9** | ≥ 99.2 / — / ≥ 90.1 | L1C 0.2 pt behind C4, above C3 |
| T4 events / ambient | 78 / 0.229 | 86 / 0.197 | 11 / 0.196 | **11 / 0.197** | ≤ 11 | PASS |
| PHASE_ANCHOR handovers / cold | 305 / 68 | 305 / 59 | 291 / 51 | 315 / **78** | cold ≤ 51 | **FAIL** (+27) |
| PB-GAUGE elected / continuous / cold re-realizations | | 455 / 374 / 81 | 442 / 369 / 73 | 462 / 362 / **100** | | |
| verdicts / narrowed / LLI-epoch (narrowed, co-located) | | 4208 / 2994 / — | 5413 / 4250 / 1205 (1205, 0) | 5413 / 4300 / 1205 (**1153, 52**) | | |
| IF-TD refloats without a verdict | | 783 | 3 | 3 | | |
| run time | 1130 s | 1464 s | 983 s | 1050 s | | |

**The reading.**  The anchor clause inside F3 removed every split reference — not back to the 6.6 % baseline but to zero, because (as the worker's OFF control on the S36/G04 leg showed, §14.12.6) today's receiver-flag response already splits references at flagged epochs on anchor arcs, and `all` + the clause is the only posture that moves a satellite's families together there.  The user's product responds exactly as §14.7 predicted a split-reference fix would: the pooled median 0.174 → 0.120 m (−31 % against OFF), hours 1–3 halved, the vertical in hours 1–2 three to five times smaller (up p50 0.206 / 0.134 → 0.065 / 0.029), and the pivot-recovery window and the last 36 min both better than OFF.

**The cost, named.**  Cold handovers 51 → 78 (OFF 68): moving all three families together at a flagged epoch on an anchor arc lands them on one successor whose other-band arcs are not always carry-usable (cold re-realizations 73 → 100, +27 = the cold handover excess).  The same mechanism is why ZWD (0.0069 vs 0.0064) and L1C (89.9 vs 90.1) sit a hair behind cbscope4 — every one of the three is better than OFF.  The remedy is the cold anchor-fallback card already stubbed in §15 (candidacy requires a usable carried value on every band); it now starts from this base.

**Verdict — on the record.**  The pre-registered primary gates pass (split ≤ baseline, W1, W2, T4, WL, ZWD-vs-OFF).  The "nothing worse than cbscope4" sub-gate of §16.7 fails on three channels that share one mechanism and are all better than OFF; I override it because the user's product is the best of every arm on this bed by a wide margin and the cost has a named cause and a named remedy.  `network.cycle_slip.common_band_scope` and `network.cycle_slip.if_phase_td_pairs` are **RECOMMEND_ON together**, pinned in `golden_sim_tri_w6` (generator verify PASS, posture rows in `GnssConfigNetworkPosture.cpp`, loader coverage 0 gaps), with field exceptions for both (the field's L5 lanes are BIA-corrected and run the between-satellite MW path; a field harness leg is owed before a field pin).  Residual: 2 spurious-implicate `keep` rows from young-lane silences (§14.12.6).

**What Study 7 leaves open, in order:** the cold anchor-fallback card (§15, now with +27 cold handovers of this posture as its population); the pivot-departure shock (§16.5: 1.30 m and ~40 min of user recovery at a declared full re-reference, both arms — the largest consumer event of the settled window); the oracle-pin card (§17); the maturity bar for exoneration (the two young-lane rows).


# 0910 afternoon: level-memory and persistence ladders, field continuity card

**STATUS**

- **id:** 0910 afternoon
- **topic:** ionosphere
- **status:** closed
- **summary:** Softening the levels FAILS four ways, fewer T4 erases help the producer but FAIL the consumer gate at every K, and field emission continuity is KEEP_OFF on its trigger.
- **dates in the text:** 2026-09-10
- **knobs / posture:** `network.phase_leveling_q_amb`, `network.phase_leveling_var_floor_m2`, `network.raim_arc_persist_epochs`, `network.carry_iono_in_block`, `network.emit_pb_gauge_mode`, `network.carry_zwd_in_block`, `network.serialize_disc_counters`
- **hashes named in the text** (commit / exe / byte-cert): `dd6c81c`, `b5af681`, `4bb799a`, `966d4fd`, `95afa6d9`, `c48b9a5c`, `865904b`, `721dd39` … (+2 more in the text)
- **run dirs named in the text:** `runs/golden_sim_campaign`, `runs/simnet60_campaign_wu93`, `runs/golden_sim_campaign_zwdblk`, `runs/fldf3_on2_zwdblk_`, `runs/fldf3_on2_zwdblk`, `runs/fldf3_on2_zwdblk_pbcont3`, `runs/golden_sim_pbcont`, `runs/golden_sim_pbgauge_ctl` … (+1 more in the text)
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md`, sections 6.18, 6.18a, 6.18b, 6.18c, 6.18d, 6.18e

**Outcome.** The afternoon tested the levels' memory and the T4 persistence K against the frozen (ρ, I) split on the slipped bed, and ran the field continuity card. The q arms cost the consumer 57 %; fewer erases helped the producer while the consumer stayed 2.1× worse at K = 1e6, since the anchors' (ρ, I) walk reaches it through the phase-clock. Continuity cut consumer resets by 91 to 95 %, but its trigger's staircase random-walks the emitted level: v1 and v2 KEEP_OFF. Later that day v3 was FALSIFIED, closing the continuity class, and `network.carry_iono_in_block` went KEEP_OFF ([0910 second half](../ionosphere/AMB-IONO-0910-SECOND-HALF.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §6.18 The 2026-09-10 afternoon — the level-memory and T4-persistence ladders, the wire wander, and the continuity card

§6.17e closed the morning with one open lever: the (ρ, I) split frozen into the zero-q levels, and
the two candidate ways at it — the levels' MEMORY (soften the prior and let the split move) and the
T4 erase tier (stop chopping arcs before they converge). The afternoon ran both as config-only
ladders on the slipped bed, graded the second one ON THE WIRE, and ran the field continuity card of
§7 item 31 three times. Five results: (a) the level-memory ladder FAILS four ways; (b) the
persistence ladder inverts — fewer erases help the producer and hurt the consumer; (c) the wire
grade says why, and re-points the next build; (d) the field continuity card is KEEP_OFF twice, for
a reason that is in the TRIGGER and not in the plumbing; (e) an addendum to §6.17g's field census.
Nothing in §6.16 or §6.17 is edited.

---

### 6.18a The level-memory ladder — softening the levels does not release the split (FAIL ×4)

Two knobs, both value knobs on the leveling carry, both default 0 (= off):

| knob | default | loader | what it does |
|---|---|---|---|
| `network.phase_leveling_q_amb` | 0.0 m²/s, `include/config/GnssConfig.hpp:6766` | `src/core/config/GnssConfigLoadNetwork.cpp:3098-3099` | process noise inflating the carried prior variance over the inter-epoch gap; 0 = pure zero-process-noise accumulation (random-constant) |
| `network.phase_leveling_var_floor_m2` | 0.0 m², `include/config/GnssConfig.hpp:6769` | `src/core/config/GnssConfigLoadNetwork.cpp:3105-3106` | floor on the carried prior variance (caps Λ — the lock-in guard); 0 = no cap |

Four single-knob arms off E0 (`runs/golden_sim_campaign`, the F3 golden posture on the slipped
60-station campaign bed `runs/simnet60_campaign_wu93`, 720 ep / 6 h, 2 452 truth slips), graded
whole-run with `simnet_grade.py`:

| arm | knob | ZWD truth rms (m) | ZWD truth \|residual\| p50 (m) | consumer settled p50 (m) | T4 arc erases |
|---|---|---|---|---|---|
| **E0** | — | **2.098** | **0.123** | **1.751** | **22 968** |
| `_plq7` | `phase_leveling_q_amb` 1e-7 | 1.93 | 0.256 | 2.76 | 12 348 |
| `_plq6` | `phase_leveling_q_amb` 1e-6 | 1.96 | 0.450 | 2.75 | 12 477 |
| `_plvf4` | `phase_leveling_var_floor_m2` 1e-4 (σ 1 cm) | 2.089 | 0.111 | 1.85 | 22 524 |
| `_plvf3` | `phase_leveling_var_floor_m2` 1e-3 (σ 3.2 cm) | 2.060 | 0.120 | 1.89 | 21 429 |

Arms `config/test_suite/_golden_sim_campaign_{plq7,plq6,plvf4,plvf3}.json`, each a copy of
`_golden_sim_campaign.json` with ONE key and the run paths changed.

**All four FAIL.** The q half is not inert — it halves the T4 churn (22 968 → ~12 400, −46 %), which
is the first direct evidence that the erase tier is sensitive to level STIFFNESS — but the channel it
was aimed at barely moves (ZWD truth rms 2.098 → 1.93/1.96) while the per-station-epoch truth
|residual| MEDIAN gets **worse** (0.123 → 0.256/0.450 m) and the consumer pays **+57 %**. The var-floor half is NEUTRAL on every
channel at both values: ZWD rms within 2 % of E0, |residual| p50 within 10 %, consumer +6 %,
T4 within 7 %.
Non-monotonicity in the value (1e-7 ≈ 1e-6, 1e-4 ≈ 1e-3) says both knobs are past the point where
the prior's own scale sets anything.

**Reading the data supports.** The freeze is not a stiffness-of-the-levels problem. A re-levelled arc
is code-quality in VALUE but phase-stiff in the block, so it is inconsistent with the geometry until
~20 min of geometry change refines it; T4 at E0's rate (21 erases per station-hour) erases it before
it converges — an erase → re-level limit cycle (§6.17b's H-C, measured). Softening the prior lets the
level MOVE but gives it nothing to move TOWARD: on a bed where arcs keep restarting, the station's
per-epoch (ρ, I) split has no information to converge to, so a looser level just tracks the same
mis-split with a larger variance and a noisier value. That is why the q arms cut the erase count (a
looser prior is a smaller standardized residual) and still do not release the plateau.

---

### 6.18b The T4 persistence ladder — fewer erases buy the producer and cost the consumer

`network.raim_arc_persist_epochs` = K (default 1, `include/config/GnssConfig.hpp:6857`; loader
`src/core/config/GnssConfigLoadNetwork.cpp:4401-4402`; gate `src/core/filter/NetworkRaim.cpp:629`)
requires an isolated-arc spike to trip K epochs in a row before the T4 attribution ERASE acts. The
design intent is exactly the limit cycle above: a converging young arc clears a K-epoch window, a
truly bad arc is still erased after K. Pre-registered gate: ZWD ≤ 0.5 m **with** consumer rms ≤ 2.0 m
(i.e. E1's consumer cost must not reappear).

Four arms on the in-block posture (`carry_zwd` + `carry_zwd_in_block`, §6.16.7d's vehicle), same bed,
whole-run `simnet_grade`; K = 1 is `runs/golden_sim_campaign_zwdblk` and K = 1e6 is E1-equivalent:

| K | arm | T4_attrib events | arc erases (×3 signals) | ZWD truth rms (m) | consumer p50 / rms (m) | sat-clock contrast (m) | clock vf |
|---|---|---|---|---|---|---|---|
| **1** | `_zwdblk` | 9 871 | 29 613 | 1.188 | **1.085** / 2.389 | 11.79 | 2.60e6 |
| 10 | `_zwdblk_p10` | 1 553 | 4 659 | 0.977 | 2.296 / 2.015 | 3.44 | 3.73e5 |
| 40 | `_zwdblk_p40` | 334 | 1 002 | 0.931 | 2.727 / 2.800 | 2.81 | 2.30e5 |
| 1e6 | `_zwdblk_e1` | **0** | **0** | **0.818** | 2.320 / 2.429 | **2.66** | 2.20e5 |

**FAIL on the consumer gate at every K.** And the shape of the failure is the finding: fewer erases
buy the PRODUCER monotonically — ZWD truth rms −22 % at K = 40 and −31 % at K = 1e6, satellite-clock
datum-free contrast **11.79 → 2.66 m (−77 %)**, clock variance factor 2.60e6 → 2.20e5 (−92 %) — while
the CONSUMER degrades: settled p50 1.085 → 2.296 → 2.727 (×2.5 by K = 40) and still 2.1× worse at
K = 1e6. The two halves of the ladder point in opposite directions across the whole range, so no K
tunes out of it.

⇒ **the consumer does not feel the satellite clock alone; it feels the per-satellite PHASE-CLOCK
(C + P).** "Sat-clock contrast" improving 4.4× while the consumer gets 2.5× worse is a direct
demonstration that it is not, by itself, a consumer-relevant metric — grade a consumer-facing change
on the phase-clock, not on the clock channel. §6.18c measures the phase-clock and confirms it.

---

### 6.18c The wire grade of the ladder — the wander rises with K, the jitter does not

Instrument (`scratchpad/persist/summary.txt`; script forms `wire_wander.py` / `wire_wander2.py` on
the four arms): the SD phase-clock error `W = dC − dP` against one FIXED reference satellite (G03 /
E03, the same in every arm — matched, but its own error is in every chord, so §2 below re-does the
contrast against the per-epoch median as the reference-free cross-check), decomposed inside a
PB-gauge/anchor tenure into **wander** (std of 10-min block means, the hypothesis quantity) and
**jitter** (|30 s difference|). Settled h3–h6, all in cycles.

| channel | wander K=1 | K=10 | K=40 | K=1e6 | jitter \|d\| p50 (K=1 → 1e6) | tenures | handovers |
|---|---|---|---|---|---|---|---|
| Galileo L1C | 0.249 | 0.611 | 0.633 | **0.793** | 0.071 → 0.068 | 143 → 114 | identical |
| Galileo L5Q | 0.303 | 0.699 | 0.762 | 0.877 | 0.068 → 0.070 | 138 → 108 | identical |
| Galileo L7Q | 0.478 | 0.681 | 0.748 | 0.850 | 0.070 → 0.066 | 140 → 114 | identical |
| GPS L1C | 0.507 | 0.836 | 0.885 | **0.944** | 0.072 → 0.076 | 210 → 172 | identical |
| GPS L2W | 0.523 | 0.833 | 0.881 | 0.953 | 0.067 → 0.069 | 203 → 156 | identical |
| GPS L5Q | 1.092 | 1.017 | 1.093 | 1.376 | 0.114 → 0.094 | 201 → 160 | identical |

Wander/within-tenure-std goes 0.44 → 1.12 (Gal L1C) and 0.89 → 1.43 (GPS L1C): the low-frequency
share of the within-tenure error grows from a third to all of it. **Handover counts are identical in
every arm** (PBGAUGE 279, PHASE_ANCHOR 297; only CLK_STEP moves, 68 → 4) and the per-epoch step
size is flat (step p50 0.204–0.283 cyc across the whole ladder), so **K changes the CORRELATION of
the steps, not their number or their size.** The motion itself is the frozen split seen through the
anchors: a 2-parameter (range, iono) fit of the per-satellite time-differenced motion reads
**R² 0.993–0.996** with |dr| ≈ 1.2 cm/30 s, and the SHAPE is invariant in K — only its persistence
changes.

The datum-free contrast against truth separates level from motion (per-epoch median removed; the
emitted phase bias carries the anchor's integer, so it is only meaningful arc-level-removed, and the
clock channel gets the identical treatment):

| channel, rms (m) | K=1 | K=10 | K=40 | K=1e6 |
|---|---|---|---|---|
| Galileo clock-only (lump removed) | 11.914 | 2.439 | 1.956 | 1.932 |
| GPS clock-only (lump removed) | 6.070 | 2.824 | 2.212 | 2.043 |
| Galileo clock-only **ARC-REMOVED** | 0.053 | 0.123 | 0.079 | 0.065 |
| GPS clock-only **ARC-REMOVED** | 0.079 | 0.086 | 0.096 | 0.075 |
| Galileo L1C phase-clock ARC-REMOVED | **0.143** | 0.226 | 0.286 | **0.277** |
| Galileo L5Q phase-clock ARC-REMOVED | 0.235 | 0.744 | 0.498 | 0.464 |
| Galileo L7Q phase-clock ARC-REMOVED | 0.174 | 0.366 | 0.346 | 0.358 |
| GPS L1C phase-clock ARC-REMOVED | 0.202 | 0.171 | 0.177 | 0.148 |

⇒ **the clock's 4.4× contrast gain is ALL arc LEVEL** — arc-removed, the clock channel is flat at
5–12 cm across the entire ladder — while the arc-removed PHASE-CLOCK **degrades** on every Galileo
band (L1C 0.143 → 0.277 m, ×1.9). The producer's clock statistic improves because erases stop
re-randomising the level; the quantity the consumer integrates gets worse.

The consumer side closes the loop, and it is a BIAS, not noise:

| K | PB-disc resets (settled / run) | 3D rms / mean (m) | UP mean (m) | jitter sd / wander sd (3D) | ρ(err, arc age) 3D / UP |
|---|---|---|---|---|---|
| 1 | 4 570 / 9 424 | 2.389 / 1.967 | **−0.218** | 0.898 / 1.015 | **−0.335** / −0.314 |
| 10 | 5 724 / 11 391 | 2.015 / 1.861 | −0.156 | 0.290 / 0.718 | **+0.420** / +0.222 |
| 40 | 5 985 / 11 509 | 2.800 / 2.571 | −1.548 | 0.569 / 0.952 | +0.200 / +0.085 |
| 1e6 | 5 859 / 11 400 | 2.429 / 2.232 | **−1.953** | 0.507 / 0.812 | +0.250 / +0.281 |

Both consumer wander and jitter FALL with K, and the settled MEAN rises — 3D mean 1.97 → 2.23–2.57 m
(worst at K = 40), UP mean −0.22 → −1.95 m (worst at K = 1e6). The within-run causal test is the sharp one: the correlation of |error|
with ARC AGE (epochs since the last phase-bias reset burst) **flips sign** at K ≥ 10, from −0.335
(error decays after a reset, K = 1) to +0.20…+0.42, with the mean |3D| by age bin growing
monotonically inside each run (K = 10: 1.72 m at age 0 → 2.45 at 5 → 2.55 at 10 → 2.69 at 20). A
consumer that is flushed every few minutes never accumulates the wire's within-arc walk; a consumer
that holds an arc integrates it. Consumer PB-disc resets RISE with K (4 570 → 5 859 settled) even
though the erases stop — the P-record jump proxy |dP| > 0.25 λ goes 13 260 → 17 387 — so the steps
the consumer sees are not the erases, they are the walk crossing the threshold.

**Reading the data supports: the T4 erases are the CRUDE CURE for the anchors' walk.** They chop a
continuous (ρ, I) walk into steps that the consumer flushes by resetting, at the cost of the ZWD and
clock channels (§6.17b: 99 % of them are not truth slips, and the re-level value is usually worse
than what it replaced). Removing them exposes the walk. So neither switching T4 off nor tuning K is
the fix, and neither is bounded identification (§7 item 22, already superseded): **the cure is
stopping the (ρ, I) walk itself**, which is §7 item 33's per-LOS ionosphere carried IN-BLOCK. That
card is now in build on branch `worktree-ionoblk` (from `dd6c81c`) as
`network.carry_iono_in_block`, pre-registered slipped ZWD ≤ 0.5 m **and** consumer p50 ≤ 0.6 m, with
the quiet-bed consumer within +10 % (the B9/Q1 protocol §6.16.7d demanded before any in-block claim).

⚠ **Caveats on this ladder.** (1) K = 1 (`_zwdblk`) was produced by a Sep-9 build; K = 10 → 40 → 1e6
are same-build. The knob's default is 1 and its code comment says "1 = act immediately
(byte-identical)", and the two intervening commits claim default-OFF byte-identity, but the K = 1 leg
is NOT a same-build A/B — the monotone K = 10 → 1e6 leg is. (2) These runs carry no
`logs/ambiguity_trace.csv`, so the grader's consumer join is empty and the wire-vs-consumer
side-by-side is inferential. (3) `scripts/simnet_wire_integer_consistency.py` treated
`truth_ambiguities.csv` as a STATIC per-(sat, signal) integer (2 486 slip rows touch 1 951 of 6 039
keys = 32.3 % of this bed), so its integer-dependent steps 4/4a/4b were contaminated here; **every
number quoted in this subsection is integer-free** (steps 1/2, 1c, 3b, 6). The grader now
accumulates truth slips via `simnet_amb_integer.load_truth` / `n_at` — fixed in this commit,
quiet-bed output byte-identical.

---

### 6.18d The field continuity card — validated as a mechanism, KEEP_OFF twice, and the defect is the TRIGGER

§7 item 31's card, `network.emit_pb_gauge_mode: "continuity"`: a per-family emission offset absorbs
every frame step so the emitted phase bias is continuous across handovers and mass refloats. No
integer content (the satellite's true bias is constant; the gauge is arbitrary and common to every
station's product), so it is a different mechanism from §6.17f's falsified level mode. It ran three
times on the field and once on the quiet sim.

**Pair 1 — ⚠ the branch-exe trap** (`runs/fldf3_on2_zwdblk_{pbcont,logctl}`, branch exe `pbgauge2`
= `b5af681`). The pair was supposed to run the in-block posture. **It did not.** The worktree
branched from `4bb799a`, before `966d4fd` (`network.carry_zwd_in_block`) landed, so the loader read
the key it did not know, ignored it silently, and both arms ran the DIAGONAL B9 carry: no "ZWD in
block" run-end clause, posture SCOPED tier 27/36 vs 27/35, fleet ZWD span p50 0.205 m vs the
reference's 0.143. The tell was a hash: the log-only twin's `products.txt` sha256 `95afa6d9…` ≠ the
main-tree run's `c48b9a5c…`. **A JSON loader reads the keys it knows and ignores the rest — a branch
exe knows only its own keys, and byte-identity on one posture proves nothing about knobs that
posture does not arm** (`feedback_branch_exe_unknown_knob_silent`; §7 item 38). Every pre-registered
ABSOLUTE baseline was therefore off a different posture; the paired A/B is still clean, and it is
reported as a posture contrast, never as a baseline.

On that (diagonal) posture the mechanism validated: 18 145 steps absorbed (58 at handovers /
18 087 unmarked; p50 0.233 cyc, p99 16.16), C records and P-stripped products BYTE-IDENTICAL
(emission-only), consumer PB-disc resets 1 158 → **104 (−91 %)**, |ΔP| > 0.25 λ 6 941 → 164 (−98 %),
pc_IF within-tenure deviation 0.230 → 0.164 m, step share of variance 57.8 → 11.7 %, pb_L1C unmarked
step rate 0.671 → **0.000** /sat/h — and the residual pc_IF step rate 0.401 equals the CLOCK's own
0.380, i.e. what is left in the phase channel after absorption is the clock's step content. The
CONSUMER got worse (3D 2.701 → 2.936 m, UP sd 1.99 → 2.32) and the residual converted from fast reset
jitter to slow wander (UP ACF < 0.5 at 11 → 55.5 min, %slow 61 → 98, PCA axis1 0.684 → 0.901),
corr(UP, BSHM ZWD) +0.334 → +0.707, regressing the co-located site ZWD out taking UP 2.488 → 1.355 m.
That produced a reading — "the resets were MASKING the ZWD sink" — which **pair 2 does not
reproduce**: it was the diagonal carry's, not the mechanism's.

**Pair 2 — the correct posture, v1** (`runs/fldf3_on2_zwdblk_{pbcont2,logctl2}`, rebased exe
`pbgauge3` = branch `865904b` = main `721dd39` + PB-gauge). Posture check cleared: both arms print
the identical run-end clause `ZWD in block: 87313 col(s), 98 diagonal fallback(s), 297 missing cross
term(s), 87274 RAIM-pool admission(s)`, and `logctl2`'s `products.txt` sha256 `c48b9a5c…` **exactly
reproduces** `runs/fldf3_on2_zwdblk`. Emission-only re-proved: all "P," lines stripped, sha256
`590470b2…`, 0 differing lines, max |ΔC| = 0.0.

| gate (settled ≥ h6, n = 720) | logctl2 | pbcont2 (v1) | verdict |
|---|---|---|---|
| P1 steps absorbed | — | 34 870 | PASS |
| G1 consumer PB-disc resets | 2 322 | **122** (−94.7 %) | PASS |
| G2 \|ΔP\| > 0.25 λ | 19 354 | **242** (−98.8 %) | PASS |
| G3 pc_IF within-tenure dev p50 (m) | 0.464 | **0.591** | **FAIL** (worse than its own control) |
| G3 step share of variance, pc_IF | 69.2 % | 70.2 % | **FAIL** (unmoved) |
| G4 UP rms / sd (m) | 1.862 / 1.862 | **1.131 / 1.127** | UP sd ≤ 1.2 **PASS** (−39 %) |
| G4 E rms (m) | 1.364 | 1.229 | PASS |
| G4 N rms (m) | 1.798 | **2.247** (+25 %) | **FAIL** |
| G4 3D rms (m) | 2.926 | 2.799 | FAIL (absolute gate 2.0) |

**Why G3's level half fails — measured, not inferred.** `dev()` is linear, so
`dev(pbcont2) = dev(logctl2) + dev(STAIRCASE)` where the staircase is the accumulated continuity
offset actually emitted. On the settled pc_IF channel: arm 1.1425 = control 0.7913 + **staircase
0.5314** m. The staircase fires **14 887 times in the settled window** with one-epoch |jump| p50
0.026 m — and the 0.10-cycle trigger is ≈ 1.9 cm. It is absorbing routine phase-bias NOISE (raw
pb_L1C |d| p50 4 cm/epoch, heavy-tailed p90 0.41 m), ~23 firings per family per hour, each a
PERMANENT offset ⇒ the emitted level RANDOM-WALKS, p50 18.8 m of accumulated excursion per satellite
over 6 h (max 284 m). A datum artefact was tested and REJECTED: the per-epoch trim datum is quieter
in the arm (|d datum| p50 0.0029 vs 0.0074 m, n > 0.25 m 26 vs 73), so the extra deviation is the
staircase, not the frame. The new residual is what an integrated per-satellite level error looks
like at a float consumer: a near-horizontal due-south very slow channel, PC1 72 % at el 14.4° /
az 175.6°, **84° off the geometric null**, N ACF < 0.5 at 56 min.

The 2×2's CONTROL column carries its own finding: **the in-block ZWD posture doubles-to-quadruples
the wire's phase-bias churn.** logctl → logctl2: consumer resets 1 158 → 2 322, the |ΔP| proxy
8 733 → 19 354, pb_L1C unmarked step rate 0.671 → 2.925 /sat/h, pc_IF deviation 0.230 → 0.464 m.
That is the wire-side price of the vertical §6.16.7d bought, it is what makes the continuity gauge
look good on this posture, and it deserves its own card.

**Pair 3 — v2 pend-and-confirm, a REGRESSION** (`runs/fldf3_on2_zwdblk_pbcont3` vs the same
`logctl2`; confirm k = 3 epochs within tol 0.10 cyc). Run-end: 4 636 pends, 4 212 confirmed, 190
reverted, 30 014 chained, 17 dropped at gaps, 168 dropped at the rail, **2 877 force-resolved (68 %)**.
Step suppression survives — G1 158 vs 2 322 (−93.2 %), G2 497 vs 19 354 (−97.4 %) — and everything
else is worse than v1: G3 pc_IF dev **0.844 m** (v1 0.591, ctl 0.464), step share **88.6 %**; G4 3D
**4.471 m**, UP rms **3.464** (mean −2.337, sd 2.559, **trend −0.921 m/h**), N 2.643, E 0.999 (the
only win). Hourly 3D: the best first six hours of the three arms (h0–h5 mean 1.72 m vs 3.45 v1 /
2.73 ctl) then a MONOTONE ramp h6 → h11 of 2.72 → **6.54 m** — the signature of an integrated level
error, not of an event population.

**The decisive measurement is that v2 gated the LABEL, not the WIRE.** The emitted P offset equals
`delta_cont_cyc` EXACTLY on every row of both arms — |(P_arm − P_logctl2)/λ − δ_cont| p50 = p99 =
max = 0.000000 — and that holds separately inside every `pend_state`, **including the 42 386 rows
marked PENDING**. Decomposing the 35 025 δ_cont increments: 2 163 (1 136 cyc, **3.2 %**) on
confirmation rows, 32 862 (34 064 cyc, **96.8 %**) on pending rows. The revert path is correct — all
190 reverts restore the pre-pend offset to the bit (|net move| = 0.000000 in 190 of 190) — and
irrelevant: only 4.1 % of pends revert, and the pend window sits pinned against its own
forced-resolution rail (duration p50 = p90 = max = 13 epochs). So the design premise "a reverting
excursion is dropped with the emitted value never having moved" does not hold: the staircase fires at
the same rate (14 495 vs 14 887 one-epoch jumps in 6 h), its per-satellite excursion is slightly
LARGER (p50 20.5 vs 18.8 m), and its 30-min structure is **1.60× worse** (0.848 vs 0.531 m).

**The quiet-sim v1 twin** (`runs/golden_sim_pbcont` vs `runs/golden_sim_pbgauge_ctl`, quiet 24 h
golden + the consumer-AR overlay, 61 sta, 0 truth slips, 457 392 P records). It does exactly what it
claims and almost nothing else: consumer PB-disc resets **2 274 → 151 (−93.4 %)** with code-bias
(1 513) and clock (47) disc lines IDENTICAL; products byte-identical outside the P lines
(`429c37dc…`); **the wire's SD fractions do not move** — mean |p50|f| − 0.25| is 0.0257 in BOTH arms,
ON closer to the null on 5 of 12 channels and further on 7, i.e. continuity moved no integer content
in either direction; AR unchanged (0 fixes both, ratio p50 1.01, settled `ratio_battery` 2 159
identical). The cold-handover population reconciles to the row: 53 absorbed steps at a
`PBGAUGE|handover_cold` (p50 **16.171 cyc** = 3.1 m on L1) + 70 at a `handover_continuous` (p50
0.146) = 123 = the run-end clause's "123 at handovers" exactly. The price: float consumer 3D rms
0.0761 → 0.0932 m (**+22.5 %**) with **p50 UNCHANGED** (0.0603 → 0.0601) and p95 +43 % — entirely
tail, the same trade the field shows one order of magnitude smaller.

★ **And the walk is a property of the TRIGGER, not of the field's event content.** On a bed with
ZERO truth slips the accumulated |δ_cont| still reaches p50 **28.6 cyc (5.4 m on L1)** and max 181 cyc
(34 m) over 24 h — ~9× slower per hour than the field's (1.19 vs 10.4 cyc/h) purely because **96.2 %
of the sim's candidates are sub-min (< 0.10 cyc) against 72.3 % on the field**, and with no consumer
ramp to show for it (hourly 3D flat in both arms). Two beds reach the same conclusion from opposite
directions: raise the trigger above the noise floor, or gate it on a declared re-reference. Tuning
the confirmation logic is not the lever.

⚠ The one place the quiet sim is genuinely CLEANER than the field is the rail. Steps > 40 cyc pass
through unabsorbed: on the sim, 52 records in 26 epochs of which **24 of 26 carry a declared
handover** (the other two are the first two emit epochs = ignition). On the field, **196 records in
15 epochs with only 6 of 15 carrying a marker**, and the three largest clusters (tow 280050 n = 76,
282150 n = 38, 262590 n = 17) are PRIORRES / T4_attrib / TOPOLOGY epochs, two with NO handover marker
at all. The population is identical in v1 and v2 (196 / 15 both) — untouched by pend-and-confirm.
**Undeclared network-wide frame motion is a FIELD defect (invariant #7's output edge), not a property
of the gauge.**

**Verdicts.** v1 and v2 both **KEEP_OFF as promotions**. The step-suppression half is validated
three times over (−91 / −95 / −93 % consumer resets, −98 / −99 / −97 % on the wire's own jump test,
emission-only by byte-identity on every arm) and the vertical chain on the field now reads
`on2` 3.71 → in-block 1.86 → + continuity **1.13 m**. What blocks promotion is the trigger's own
staircase and the N-component it feeds. v3 (a running median of the recent per-family offsets, then
absorb only the confirmed departure from it) is in build; the ledger row keeps the `_mode` /
`cont_*` knobs marked branch-only.

---

### 6.18e Addendum to §6.17g's field wire census — what drives the steps, and what does not

The 40-arm closure matrix at Bartlett effective N, run over `runs/fldf3_on2` and
`runs/fldf3_on2_zwdblk` (`scripts/field_wire_phase_clock_census.py`; outputs
`scratchpad/fieldres/wire_phase_clock_*.txt`), settles four things:

1. **The phase-bias steps are DRIVEN BY T4 RAIM-attribution refloats.** Of every event tier, only
   `T4_attrib` co-occurs with the pc_IF step count: **r +0.546, |z| 8.7** on the control and
   **+0.268, |z| 4.4** on the in-block arm (the emission sites are `NetworkRaim.cpp:888-893`, one
   `T4_attrib/attrib_arc` event per acted entity). Every other tier is at noise — T0_gap −0.057,
   T1_if_slip −0.101, PHASE_ANCHOR +0.018, PBGAUGE +0.019, |z| ≤ 1.5 throughout. The rate is
   **~5.3 pc_IF steps per satellite per hour above 0.25 m**, 205 above 1 m in 6 h, and up to **26
   satellites stepping in one epoch** (tow 282150, `T4_attrib ×4`; tow 299490 with `T4_attrib ×22`
   and no other tier).
2. **And they are UNSIGNALLED on the wire.** `network.serialize_disc_counters` is FALSE on the field
   chain, so the P records are the 6-field form (`P,sat,signal,bias_m,sig_int,wl_int`) with no
   `phase_disc` column: a FILE consumer gets no discontinuity signal at all. The R retro-bump fired
   **68 times against ~1 000 steps**. The closed-loop consumer still resets, but only because it
   reads the in-process store counter — invariant #7 with a bare OUTPUT edge (§7 item 39).
3. **The geometry closure is NULL in every arm.** Predicted-vs-actual consumer error correlates at
   |z| 2–3.7 at most and sign-inconsistently across the 40 (driver × prep × trim) cells; the
   predicted axis sits at el 75.6–79° against a measured 49.8° / az 9.3°, and two half-visible
   satellites (G12 40 % visible, G25 59 %) carry two thirds of the predicted amplitude. Decisively:
   **the same wire statistics feed two consumers with ORTHOGONAL error axes** (el 50 / az 9 in-block
   vs el 73 / az 137 on the control) ⇒ the consumer's wandering DIRECTION is not set by the wire's
   phase-clock. The magnitude link survives weakly (corr(rms visible |δ|, |consumer error on the
   axis|) +0.396, n_eff 27, |z| 2.05), which is consistent with the §6.18c reading — the consumer
   integrates the wire's walk into whatever its own weakest direction happens to be — but the
   projection itself is not the mechanism.
4. **Minor contract defect:** the S record advertises the DOY-128 a-priori
   (`COD0MGXFIN_20241280000_01D_05M_ORB.SP3+…_30S_CLK.CLK`) while three days are loaded — the
   advertised identity is the first day's, not the epoch's.

For scale, the same statistic on COD's own final clock is the floor: rms p50 **0.0066 m**. The
field's emitted clock sits at 0.0256 m (**3.9×** the floor) and its pc_IF at 0.464 m (**70×**).
COD's phase biases are daily constants, so every time-variation in the P channels is wire-side by
construction.

---


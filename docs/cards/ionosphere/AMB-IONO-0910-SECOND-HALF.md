# 0910 second half: probe ledger, false-alarm isolation, continuity closure

**STATUS**

- **id:** 0910 second half
- **topic:** ionosphere
- **status:** closed
- **summary:** Eleven slipped-bed probes all FAIL with one shape (the station frame walks under arc churn); emission continuity is FALSIFIED as a class and producer AR is NO-GO for Phase 3.
- **dates in the text:** 2026-09-10
- **knobs / posture:** `network.carry_iono_in_block`, `network.carry_zwd`, `network.carry_zwd_in_block`, `network.emit_pb_gauge_level`, `network.serialize_disc_counters`, `network.carry_rx_clock_in_block`, `network.ambiguity_resolution`, `network.carry_aware_covariance`, `network.inject`
- **hashes named in the text** (commit / exe / byte-cert): `bba9a7f2865070bc`, `7311ad0`, `2bf6821`, `3dd1284`, `c48b9a5c`, `525e85c`, `ad0b1d0`, `721dd39` … (+1 more in the text)
- **run dirs named in the text:** `runs/simnet60_campaign_wu93`, `runs/golden_sim_campaign_zwdblk`, `runs/golden_sim_campaign`, `runs/golden_sim_zwdblk_ionoblk_quiet`, `runs/golden_sim_zwdblk_quiet`, `runs/golden_sim_campaign_zwdblk_orafa`, `runs/fldf3_on2_zwdblk_`, `runs/fldf3_on2_zwdblk` … (+3 more in the text)
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md`, sections 6.19, 6.19a, 6.19b, 6.19c, 6.19d, 6.19e, 6.19f

**Outcome.** The second half ran `network.carry_iono_in_block` and every remaining config-only probe on the slipped bed, closed field emission continuity and graded Phase-2 producer AR. No probe met its gate pair: each response that reduces re-levelling worsens the consumer, and keeping the detectors' false alarms made it 2.7× worse. Iono-in-block is KEEP_OFF (quiet-bed NEUTRAL), `filter.process_noise.clock_bias_q` is INERT on the producer (its receiver clock has no temporal prior), and 14.48 % of producer-AR fixes were wrong (gate 1 %). Development moved to the [event harness](../harness/HARNESS-REF.md), where the receiver-clock carry later closed KEEP_OFF ([Cards A+B](../harness/CARD-AB-require-and-rx-clock-step.md)); the carry-aware Phase-2 re-grade is item 44 of the [open-question register](../ionosphere/AMB-IONO-OPEN-QUESTIONS.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §6.19 The 2026-09-10 second half — the slipped-bed probe ledger, the continuity closure, the false-alarm isolation, and the inert receiver clock

§6.18 ended with one lever named (`network.carry_iono_in_block`, §7 item 37) and one reading
untested: that the plateau is a frozen (ρ, I) split sustained by arc churn.  The afternoon and
evening ran that lever plus every remaining config-only probe on the same bed, closed the field
emission-continuity class, and found that the producer's receiver clock has no temporal prior at all.
The result is a ledger of eleven failures with ONE shape, and a change of development method.

Nothing above this section is edited.

---

### 6.19a The slipped-bed probe ledger — eleven probes, one shape

Bed: `runs/simnet60_campaign_wu93` (60 stations, GPS + Galileo triple, 2 486 truth slips in 6 h,
wu93 frame).  **Control for this section is the IN-BLOCK posture** `runs/golden_sim_campaign_zwdblk`
(the golden + `network.carry_zwd` + `network.carry_zwd_in_block`, §6.16.7c) — not E0.  Arms that ran
on the E0 posture (`runs/golden_sim_campaign`, ZWD truth rms 2.098 m, consumer settled p50 1.751 m /
rms 1.80, T4 22 968 arc erases) are marked ⁽ᴱ⁰⁾ and must not be read across the two control columns.
Every number is `scripts/simnet_grade.py <fixture> --run <run> --products <run>/products.txt`
whole-run, except the T4 column, which is the run-end counter
(`ONE-SOLVE REFLOATS: <n> T4 attribution`, invariant #4).

| probe | arm (`runs/…`) | ZWD rms | ZWD p50 | consumer p50 | consumer rms | sat-clk contrast | T4 arc erases | WL / integers |
|---|---|---|---|---|---|---|---|---|
| **IN-BLOCK CONTROL** | `golden_sim_campaign_zwdblk` | **1.188** | **0.048** | **1.085** | **2.389** | **11.79** | **29 613** | WL ref-free **86.2 %**, L1C med\|e\| 1.77 |
| oracle exact repair ⁽ᴱ⁰⁾ | `_campaign_oracle` | 1.779 | — | 1.74 ⁽ᴱ⁰⁾ | — | — | 40 686 | WL 81.5 % flat; ambient DD \|e\| 1.65 → **2.43** |
| oracle exact repair | `_campaign_zwdblk_oracle` | — | — | **2.23** | — | — | — | — |
| oracle + E1 (T4 starved) ⁽ᴱ⁰⁾ | `_campaign_oracle_e1` | 1.819 | — | 3.78 | — | — | **0** | WL 35 %; ambient \|e\| **6.09** |
| oracle + E1 (T4 starved) | `_campaign_zwdblk_oracle_e1` | 1.239 | — | 3.33 | — | — | — | ambient \|e\| 5.66 |
| level q ×2 ⁽ᴱ⁰⁾ (`phase_leveling_q_amb` 1e-7 / 1e-6) | `_campaign_plq7` / `_plq6` | 1.93 / 1.96 | 0.256 / 0.450 | 2.76 / 2.75 | — | — | 12 348 / 12 477 | — |
| var floor ×2 ⁽ᴱ⁰⁾ (`phase_leveling_var_floor_m2` 1e-4 / 1e-3) | `_campaign_plvf4` / `_plvf3` | 2.089 / 2.060 | 0.111 / 0.120 | 1.85 / 1.89 | — | — | 22 524 / 21 429 | — |
| persistence K = 10 / 40 / 1e6 (`raim_arc_persist_epochs`) | `_zwdblk_p10` / `_p40` / `_e1` | 0.977 / 0.931 / 0.818 | — | **2.296 / 2.729 / 2.320** | 2.015 / 2.800 / — | 3.44 / — / 2.66 | 4 659 / 1 002 / **0** | wire wander ↑ monotone in K |
| k_sigma 4 (`raim_k_sigma` 6 → 4) | `_zwdblk_k4` | 0.972 | 0.055 | **1.317** | **3.26** | 14.4 | **46 575** | L1C med\|e\| 1.67 |
| iono-in-block (`carry_iono_in_block`) | `_zwdblk_ionoblk` | 1.133 | 0.042 | **1.592** | 2.24 | 11.08 | 28 509 | L1C med\|e\| **1.32** (−26 %), round = 0 21 → 25 % |
| young-arc inflate V0 0.09 m² | `_zwdblk_yng` | **0.823** | 0.290 | 1.73 | **7.91** | 18.6 | 26 076 | — |
| young-arc inflate V0 0.01 m² | `_zwdblk_yng03` | 1.175 | — | 1.60 | 2.17 | 19.6 | 31 956 | — |
| false-alarm-only KEEP (`oracle_true_slip: free`) | `_zwdblk_orafa` | 1.066 | 0.104 | **2.92** | **4.46** | — | **48 078** | — |
| rx-clock q 1000 → 0.09 (`filter.process_noise.clock_bias_q`) | `_zwdblk_rxq` | — | — | — | — | — | 29 613 | **INERT — `products.txt` byte-identical** (`bba9a7f2865070bc…`) |

**Every probe FAILS its pre-registration.**  The two gates the ladder was run against were slipped-bed
ZWD truth rms ≤ 0.5 m *and* consumer settled p50 ≤ 0.6 m (§7 item 37); the best ZWD in the table
(0.818 at K = 1e6, 0.823 at V0 0.09) buys a consumer at 2.3–7.9, and the best consumer in the table is
the CONTROL.

Two riders on reading it.  (a) The T4 opportunity denominator is **identical** in every slipped arm
(`attribution ARC (T4): 645 406 opp`), so the erase column is directly comparable across rows — quote
counts with opportunities (the §6.16.7d rate-vs-count trap).  (b) `simnet_grade` p50 and rms answer
different questions on this bed: the young-arc arm has the second-best ZWD **rms** in the table and the
worst ZWD **p50** and consumer **rms** — its failure is a tail, and a p50-only ladder would have read
it as a win.

**The unified reading.**  The station frame — its stiff zero-q phase levels together with its
per-epoch ZWD, receiver clock and slant iono — **WALKS under arc churn**.  Arcs are levelled at
different epochs against different values of the frozen (ρ, I) split, so the station's compromise
drifts as its arc population turns over; the wire grade measures the walk directly at 1.2 cm/30 s in
the range-like direction with the iono-like direction riding it at R² 0.993–0.996 (§6.18c).  An arc is
consistent with that frame only if it is periodically re-levelled INTO the current frame.  That single
sentence predicts the whole table:

- **every response that REDUCES re-levelling makes the consumer worse** — exact oracle repair
  (+105 % consumer p50 in-block), keeping the false alarms (+169 %), starving the erases at any K
  (+112…+151 %), softening the levels (+57 % ⁽ᴱ⁰⁾).  A restored-to-truth or preserved arc is
  *inconsistent with its own station's corrupted states*, and the corrupted majority then rejects it
  (§6.17e: detector re-levels −85 % while T4 grew +77 %);
- **more erasing is not the other end of a lever, it is just noise** — `k_sigma` 4 adds 57 % more
  erases, improves ZWD 18 % and costs the consumer 21 % on p50 and 36 % on rms.  With the persistence
  ladder this **brackets the erase-rate family: the shipped k = 6 / K = 1 corner sits at the consumer
  optimum of that family**, and the erase rate is exhausted as a knob;
- **making young arcs honestly weak relocates the tension rather than dissolving it** — the in-block
  young-arc inflation moves it into the satellite clocks (contrast 11.8 → 18.6 / 19.6 m) and into the
  consumer's tail (rms 7.91 at V0 0.09);
- **carrying the I half of the split does not release it either.**  `carry_iono_in_block` is the one
  probe whose *integers* improve (L1C settled ref-free med\|e\| 1.77 → 1.32 cyc, −26 %; the share
  rounding to the right integer 21 → 25 %) and its ZWD moves −5 %, but the consumer regresses +47 % on
  p50.  Mechanism live and clean: 671 535 iono columns carried, **0 diagonal fallbacks**;
- **the only lever that ever helped is a TEMPORAL PRIOR ON THE ρ DIRECTION** — ZWD-in-block, −43 % on
  the ZWD channel (2.098 → 1.188) and the only knob that removed the field consumer's vertical DC bias
  (§6.16.7d).  Its cost is a clock relocation, and it is still 2.4× off its own gate.

The list of things that do NOT work is now long enough to be informative: level stiffness (q, floor),
erase rate (both directions), erase persistence, the slip response's VALUE (exact repair), the
false-alarm population, young-arc variance, and the split's I half in the block.  What has never been
tried is a prior that makes the frame itself observable epoch-to-epoch in more than one direction —
which is why §6.19d's receiver-clock finding matters and why the next probes belong on an EVENT
harness (§6.19f), not on a storm bed.

#### 6.19a.1 The quiet-bed neutrality of `carry_iono_in_block` — the B9/Q1 protocol, passed

§7 item 37 made the quiet-bed test mandatory before any slipped-bed claim, because
`project_iono_carry_8b_campaign` falsified a DIAGONAL per-LOS iono row four times and §6.16.7d's
finding was that the diagonal vehicle IS the double count.  `runs/golden_sim_zwdblk_ionoblk_quiet`
(24 h quiet golden + in-block ZWD + in-block iono) vs `runs/golden_sim_zwdblk_quiet`, settled h12–24:

| | in-block ZWD only | + in-block iono |
|---|---|---|
| consumer 3D rms | 0.0875 m | 0.0890 m (+1.7 %) |
| consumer 3D p50 | 0.0525 | 0.0555 |
| consumer UP rms | 0.0759 | 0.0768 |
| ZWD truth rms | 5.6 mm | 5.6 mm |
| iono columns carried | — | 2 750 000, **0 fallbacks** |
| T4 arc erases | 159 | 408 |

**NEUTRAL: no double-count regression** (contrast the diagonal ZWD carry's 0.204 → 0.723 m on this same
bed) **and no gain.**  The in-block vehicle is sound — the four B9 falsifications were the diagonal row's
over-count, exactly as the ZWD card found — and the mechanism is simply not where the information is.
The 2.6× rise in T4 erases on a bed with ZERO truth slips (159 → 408) is the one thing worth carrying
forward: an extra carried direction per LOS gives the post-fit consensus more to flag.

Ledger verdict: **`network.carry_iono_in_block` KEEP_OFF** — mechanism validated on both beds, benefit
absent on both.  Branch `worktree-ionoblk` (`7311ad0`, plus `2bf6821` for the young-arc inflation)
stays UNMERGED; §7 item 37 closes.

---

### 6.19b The false-alarm isolation — the re-levels the detectors get WRONG are load-bearing

Two independent measurements, and they point the same way.

**1. Attribution (`scripts/simnet_slip_census.py` on the in-block arm, post-Card-R goldens).**
Of the bed's 2 486 truth slips, **2 337 (94.0 %) are DETECTED** — by tier MW 2 006 / IF-TD 331 / T4 59 /
raim 16 — and **74 (3 %) are SILENT**, mostly ≥ 90-cycle jumps at el ≈ 10°.  Detection is not the
problem.  The other side of the ledger is:

> **slip-tier events with NO truth slip anywhere in the window: 2 371 of 4 708 — MW 1 132, IF-TD 1 239.**

That is ≈ **6.4 false re-levels per station-hour against 6.8 true slips per station-hour**: after Card R
fixed the IF-TD reference gate on the QUIET bed (§6.14.6), BOTH detectors still fire as often on nothing
as on something on the SLIPPED bed.  Consistent with §6.14.3 — the fixture's phase noise is ~5× the
field's and the MW thresholds are field-empirical, so a fixed physical threshold does not port between
beds (`feedback_mw_thresholds_empirical`).
⚠ **Counting basis.**  The census counts EVENTS per (station, satellite); the oracle below counts
per-signal ACTUATIONS.  The earlier "13 485 actuations" figure was the per-signal offer count.  The
honest per-event false-alarm count on this bed is **~2 300 per 6 h**; the honest per-signal actuation
count is 6 804.  Never mix the two bases in a rate.

**2. The false-alarm-only oracle (`runs/golden_sim_campaign_zwdblk_orafa`, commit `3dd1284`).**  The
2×2 of the oracle's two policies has a corner that isolates exactly this population:
`oracle_true_slip: free` + `oracle_false_alarm: keep` — every REAL slip pays today's shipped response,
and only actuations with no truth slip in the window keep their level.  **6 804 false alarms kept**
(MW 4 156 / IF-TD 2 131 / LLI 517), 3 409 true slips freed as today.

| | in-block control | false alarms KEPT |
|---|---|---|
| ZWD truth rms | 1.188 m | 1.066 (−10 %) |
| ZWD truth p50 | 0.048 | **0.104** (2.2× worse) |
| consumer settled p50 | 1.085 m | **2.92** |
| consumer settled rms | 2.389 | **4.46 (2.7×)** |
| T4 arc erases | 29 613 | **48 078 (+62 %)** |

**FAIL, and decisive in the opposite direction from the one it was built to test.**  NOT re-levelling
an arc that had no slip makes the consumer 2.7× worse and forces 62 % more T4 erases.  ⇒ **the
false-alarm re-levels are a RESYNC mechanism**: their job on this bed is not slip repair at all, it is
dragging a drifting arc back into its station's current frame, and the detectors' 50 % false-alarm rate
is doing useful work by accident.  This kills "tighten the detector thresholds on this bed" as a card —
a better detector would remove the resyncs and leave the walk.

The corollary for §7 item 22 is now complete: **bounded identification is the wrong lever** (§6.17b:
T4 mostly re-erases arcs its own previous re-level damaged) **and so is better detection** (here).
Both are attempts to fire the response more accurately; the finding is that the response's ACCURACY is
not what the consumer is buying from it.

---

### 6.19c EMISSION-SIDE CONTINUITY — CLOSED, falsified as a CLASS

§7 items 31 / 36 kept the field emission-continuity card alive through two KEEP_OFFs on the grounds
that the mechanism was right and the TRIGGER was wrong.  A third variant closes the class.

All three run on the in-block field posture, `runs/fldf3_on2_zwdblk_*` vs `logctl2`
(DOY-129, 63 sta, 12 h, WTZZ closed-loop consumer; `logctl2`'s `products.txt` = `c48b9a5c…`, identical
to `runs/fldf3_on2_zwdblk` — the §7 item 38 hash rule, so the branch exe is certified on this posture).
Settled ≥ h6:

| | control `logctl2` | v1 absorb-every-step `pbcont2` | v2 pend-and-confirm `pbcont3` (`525e85c`) | v3 running-median k = 5 then absorb `pbcont4` (`ad0b1d0`) |
|---|---|---|---|---|
| consumer 3D rms | 2.93 m | **2.80** | 4.47 | **8.26** |
| consumer UP rms | 1.86 | **1.13** | 3.46 | 7.34 |
| consumer N rms | 1.80 | 2.25 | 2.64 | 3.57 |
| consumer E rms | 1.36 | 1.23 | — | — |
| accumulated \|offset\| p50 | — | **125 cyc** | 132 cyc | 124 cyc |
| absorptions (settled) | — | 34 870 | 42 386 pending rows moved | **33 403** |

**v3 is the decisive arm.**  It was built on the reading that v1's staircase absorbs routine noise, so a
running median over k = 5 epochs should pass a genuine step and reject a spike.  It absorbed **33 403
steps against v1's 34 870 — 96 % of them.**  The median filter changed essentially nothing, because
there is no flat level for a spike to stand out from:

> the field's per-epoch phase-bias motion is a **RANDOM WALK** (raw \|Δ\| p50 4 cm/epoch, heavy-tailed
> p90 0.41 m), not spikes over a flat level, and **a median cannot separate a walk from steps**.

Its consumer is the worst of the four (3D 8.26 m, UP mean −6.3 m, corr(UP, BSHM ZWD) −0.70) and its
accumulated offset lands at the same p50 124 cyc as v1's 125 and v2's 132 — three different triggers,
one common outcome, which is the signature of the SOURCE rather than of any trigger.

The quiet-sim twins say the same on a bed with ZERO truth slips: v1 cuts consumer PB-disc resets
**−93 %** and costs the float consumer **+22 %** rms (a tail), with accumulated offsets already at
28.6 cyc median over 24 h; v2 confirmed 2 622 of 4 048 pends and still walked.

**READING.**  The wire's biases random-walk because **the PRODUCER's phase-bias state walks** — the same
frozen (ρ, I) split of §6.19a, seen through the anchors (§6.18c measured it: within-tenure wander rises
with K, jitter flat, step size flat, handover counts identical).  Any emission-side absorber converts
walk-steps into offset-steps and hands the consumer an *integrated* error; the consumer, given the
choice, prefers the resets, because a reset flushes the accumulated error and an offset does not.
**The fix belongs in the estimation, not at emission.**

**VERDICT: the class is CLOSED.**  `network.emit_pb_gauge_level` stays KEEP_OFF — level mode falsified
(§6.17f), all three continuity modes falsified here.  Branch
`worktree-agent-a7e2586003a605548` (HEAD `ad0b1d0`, rebased on main `721dd39`) stays **UNMERGED as the
record**; nothing from it is proposed for main.

Two durable field lessons survive the closure, and both are cheap:

1. **The in-block ZWD posture itself doubles-to-quadruples the wire's phase-bias churn** (pb_L1C
   unmarked steps 0.67 → 2.9 per sat-hour; consumer resets 1 158 → 2 322 on the 2×2's control column).
   That is its price for the vertical it buys, and it must be quoted with the −27 % consumer 3D.
2. **`network.serialize_disc_counters` must be armed on the field template** (§7 item 39) — a FILE
   consumer of this product gets no discontinuity signal at all while the wire steps ~5.3 times per
   satellite per hour above 0.25 m.

---

### 6.19d The receiver clock has NO temporal prior on the network producer — and the sim's truth is a walk

The unified reading of §6.19a says the only thing that ever helped was a temporal prior on the ρ
direction.  The station frame has three other members with no prior at all.  The cheapest to test was
the receiver clock, because a knob for it appears to exist.

**It does not.**  `runs/golden_sim_campaign_zwdblk_rxq` = the in-block control with
`filter.process_noise.clock_bias_q` 1000 → 0.09 m²/epoch.  Its `products.txt` is
**byte-identical** to the control's (`bba9a7f2865070bc…`, verified by `sha256sum`), and its run-end T4
counter is identical to the digit (29 613 erases / 9 871 events / 645 406 opportunities).

> ⚠ **`filter.process_noise.clock_bias_q` is INERT on the network producer.**  The per-station receiver
> clock is a **per-epoch LOCAL parameter** — estimated fresh, marginalized, never carried — so a
> process-noise value for it has nothing to act on.  The knob is meaningful on the PPP / baseline /
> real-time paths only.  An arm that sets it on a network config is armed-and-inert (invariant #4): no
> counter moves, no clause appears, and the products are identical.

And the thing it would have modelled is real.  The simulator's own receiver-clock truth
(`truth_clock.csv`, e.g. `runs/simnet60_campaign_wu93/S01000SIM`, 720 epochs) is a **deterministic
drift plus a white increment**: mean step **+2.997e-08 s / 30 s = +8.99 m per epoch** (≈ +0.2995 m/s)
with the step's standard deviation **1.0055e-09 s = 0.301 m**, and a second-difference sd of
1.436e-09 s (= √2 × the step sd, the signature of white increments on a linear ramp).  So the truth is
exactly the object a random-walk prior describes — after the drift is removed.

**CARD (pre-registered, building on `worktree-ionoblk`): `network.carry_rx_clock_in_block`** — the
station's receiver clock carried INSIDE the F3 whitened local block with q ≈ 0.09 m²/epoch, the ZWD
card's sibling and the third member of the (amb, iono, ZWD, clock) block.  It inherits the ZWD card's
whole contract: in-block or nothing (a diagonal row would be the same station-common double count that
falsified `carry_zwd` and the four B9 iono arms), a counted diagonal fallback, the loader interlocks on
`phase_leveling` / `phase_leveling_local_block` / `carry_invariant`, fingerprinted, OFF byte-identical,
and the B9/Q1 quiet-bed neutrality test BEFORE any slipped-bed claim.

⚠ **Its one design risk is the drift.**  A plain RW on the absolute clock will be fighting a 9 m/epoch
ramp — three decades above the 0.30 m increment it is trying to model — so the carry must be on the
RESIDUAL about a per-epoch re-linearized `x_lin`, or the state must become a (bias, drift) pair.  Get
that wrong and the card is a stiff prior carrying a stale value, which is standing invariant #1.

⚠ **And validate it on the EVENT HARNESS first** (§6.19f), not on a storm bed: one injected slip on one
arc of a converged station, and the question is whether the station frame holds.  That question is
unanswerable in the presence of 2 486 interacting slips, which is precisely why eleven storm-bed probes
produced one shape and no mechanism.

---

### 6.19e The Phase-2 producer-AR diagnostic, graded against truth

*(§7 item 44; the instrument is `dd6c81c`, the design-doc note of 2026-09-10 and
`scripts/simnet_producer_ar_grade.py`.)*

**The run.** `runs/golden_sim_netar` = `config/golden/golden_sim_tri_w6.json` VERBATIM plus
`network.ambiguity_resolution.{enable, trace, ils_max_dim 200, min_epochs 10, enable_partial_ar,
min_success_rate 0.9999, ratio_threshold 3.0, nl_max_per_pair_frac 0.10}` and the run paths — quiet
60-station truth bed `runs/simnet60_quiet_wu93_24h`, 24 h, 2 879 epochs, exit 0.  Phase 2 is READ-ONLY
(no re-pin, no re-solve), so the emitted product is the golden's.  Run-end clause, verbatim:

```
Network (producer AR, Phase 2 log-only): 2869 epochs searched, 298 DD p50 / 200 searched p50,
fixed epochs 2238 (78.0%), DD fixed total 29828, ratio p50 12.60, p_succ p50 1.0000,
med_DD_sigma p50 0.3170 cyc; carry-blind; trace on
```

**Cost:** `netar` = **2 152 ms of a 3 984 ms epoch (54 %)** at 60 stations, median over the settled
half — the joint conditional covariance and the 200-dim MLAMBDA, not the CSV.  A 24 h arm costs
~1.7 h of extra wall on this box.

**Grade** (`scripts/simnet_producer_ar_grade.py runs/simnet60_quiet_wu93_24h --run runs/golden_sim_netar
--settle-h 12`), 852 260 trace rows joined to truth, **0 unjoinable**:

| pre-registered gate (`_golden_sim_netar.json` `_comment_arm`) | measured, settled h12–24 | |
|---|---|---|
| searched-subset \|e\| p50 ≤ 0.10 cyc | **0.198 cyc** (wrapped \|frac\| p50 0.147) | **FAIL** (2.0×) |
| fixed epochs > 50 % | **86.8 %** (1 240 / 1 429; h0–12 69.3 %) | **PASS** |
| CORRECT-fix rate among kept ≥ 99 % | **85.52 %** (13 328 / 15 585) | **FAIL** |
| wrong fixes, with \|e\| | **2 257** — see below | reported |
| P1: trace rows > 0 | **852 260 rows, 0 unjoinable** | **PASS** |

**VERDICT: NO-GO for Phase 3.**  The pre-registration named this exact outcome — *"a wrong-fix rate
> 1 % means the covariance must become carry-aware first"* — and the measured rate is **14.48 %,
fourteen times the threshold.**

**What the wrong fixes look like, and why it is the covariance.**  Every one of the 200 largest-\|e\|
wrong fixes in the settled window is off by **exactly ±1 cycle** (offset histogram −1: 91, +1: 109),
with \|e\| between 0.90 and 1.10 cyc at a formal σ whose median is **0.091 cyc** — i.e. a **median
\|e\|/σ of 10.4**, accepted at ratio p50 14.0 and `p_succ` p50 **1.0000**.  The float sat a whole
cycle away and the ILS snapped, confidently, to the neighbouring integer.  The battery never had a
chance to refuse: its inputs are the covariance's own ratio and success probability, and the
covariance does not know what it does not carry.  The run-end clause says so in one word —
`carry-blind`.

**The tell that makes it diagnostic rather than merely bad: correctness gets WORSE as the run
settles.**

| window | ratio p50 (fix epochs) | correct-fix rate | searched \|e\| p50 | ceiling (\|e\| < 0.25) |
|---|---|---|---|---|
| h0–12 | 11.31 | **93.28 %** | 0.217 cyc | 67.1 % |
| settled h12–24 | **14.01** | **85.52 %** | 0.198 cyc | 72.4 % |

The gate's confidence rises 24 % while its accuracy falls 8 points.  A covariance that shrinks faster
than the error it describes is exactly what a carry-blind conditional block does on this vehicle: the
filter accumulates and the formal σ contracts, but the DD float carries an error inherited through the
phase-leveling carry that the conditional covariance never sees.  Both windows agree the search is
**not being refused by its gates** — it is fixing enthusiastically onto a float that has not arrived.

**The float, and the ceiling.**  Settled, the searched subset's wrapped \|frac\| p50 is 0.147 with
50.9 % under 0.15 and **72.4 % under 0.25** — so plain rounding would already be wrong on **27.6 %** of
the very population the `ils_max_dim` cap picked as its tightest.  Per band, settled subset: E:L1C
0.128 / ceiling 78.1 %, G:L1C 0.151 / 70.2 %, E:L5Q 0.151 / 73.0 %, G:L2W 0.161 / 68.9 %.  Correct-fix
rate splits the same way — **GPS 90.4 / 90.3 %, Galileo 79.6 / 81.7 %.**

⚠ **The graded population rotates** (§6.12's rule, and the grader reports it because of it): over the
2 869 epochs the star topology's reference SATELLITE switches 213 times on Galileo and 261 on GPS, and
the reference STATION 242 / 313 times across 52 / 46 distinct stations.  Every DD in this file inherits
its reference pair's own error, so a per-band or per-station split off this trace compares rotating
populations.  The whole-population rates above are the safe readings.

**What is NOT claimed.**  This does not say producer AR cannot work; it says the Phase-2 search
**cannot be promoted to Phase 3 pins on a carry-blind covariance**, because the gate that would decide
which pins to trust is provably not a discriminator here (85.5 % correct at p_succ 1.0000).  Nor does
it re-open the DO-NOT-BUILD verdict on producer DD-*fixing* (M5) — Phase 2 is a diagnostic search plus
its grader, and it stays log-only.  ⇒ **The next step on the AR track is `network.carry_aware_covariance`
applied to the Phase-2 DD block, re-graded with this same instrument, before any Phase-3 build.**  The
83 % ceiling that §6.16.6 measured on the estimator's own DDs is real, but it is not reachable through a
covariance that cannot see the carry.

---

### 6.19f THE EVENT HARNESS — the development loop changes

**USER DIRECTIVE, 2026-09-10** (`feedback_event_harness_over_daylong_runs`), after a day of 6 h and
24 h arms:

> "Why do the runs take so long?  I thought our simulated network would create rapid development and
> fast iteration.  Let's say a pivot handover is evaluated.  We simply run the network for 1 h, or
> however long to reach convergence, then inject the simulated handover, continue running for a few
> more epochs to study the effects.  This approach seems much faster and easier to isolate problems
> than these full-day runs that are frankly taking way too long!"

**The day's own record is the argument for it.**  Almost every discriminator in §6.19a is a
SINGLE-EVENT question — what a re-level's value error does over the next 40 epochs, what T4 does to a
young arc, what a handover steps on the wire and how the consumer answers, whether an age-inflated arc
converges.  The storm beds answered all of them only STATISTICALLY, at 23–90 min per arm, with events
interacting so thoroughly that eleven probes returned eleven aggregate numbers and no mechanism.  The
one time this section isolates a mechanism cleanly (§6.19c's v3), it is because the *trigger* was
varied against a fixed bed — an event-level experiment done the expensive way.

**Design (branch `worktree-evharness`, building; nothing merged).**  Four pieces, three of which
already half-exist:

1. **A converged snapshot library.**  Quiet sim at 6 h and 12 h, field at 6 h.  The vehicle is the
   RTB-7 warm-resume snapshot, and `project_seam_continuity_card` closed the correctness question in
   2026-09: with `resume_frame_restore` + `resume_coupling_restore` on a QK/QP-bearing snapshot, a warm
   restart's consumer converges like a cold consumer on the continuous wire.  ⚠ Grade every seam card
   with a COLD-CONSUMER REPLAY, never the seam-epoch diff.
2. **A producer-side fault injector, `network.inject.*`** — default OFF, byte-identical when unarmed,
   each event scheduled at an absolute TOW and counted at both edges (invariant #7):
   `slip` (station, satellite, signal(s), Δcycles — the simulator's scripted-slip content, but injected
   at RESUME rather than baked into the fixture), `outage` (a tracking gap of N epochs — the class the
   simulator has NEVER had, which is why the gap-poison card of §12 is field-only),
   `anchor_handover` (force a PHASE_ANCHOR / PBGAUGE handover at a chosen epoch),
   `arc_erase` (force a T4-style one-solve refloat on a named arc, so the erase can be studied without
   waiting for RAIM to choose one), and `sat_clock_step`.
3. **A leg generator** — resume from a snapshot, arm one injection, run 40–60 epochs.  At 60 stations
   that is 1–2 minutes, against 23–90 for a storm arm.
4. **A per-event grader** — the arc's level trajectory against truth, the station's ZWD / iono / clock
   response, the emitted C and P step at the event epoch and after, and the consumer's reset and
   re-convergence.  Every one of these instruments exists; what is missing is the per-event framing.

**The rule going forward.**  Storm beds are for FINAL validation of a fix whose event-level behaviour is
already understood (they are the only place emergent plateaus live), never for the development loop; and
never more than 2–3 concurrent 60-station arms — the box was oversubscribed at 5–6 today and everything
ran 2–3× slow.  §6.19d's `carry_rx_clock_in_block` is the first card to be built this way.

---


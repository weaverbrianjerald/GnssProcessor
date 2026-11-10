# T3 field: collateral defects, the field check, the detection gate, R2 + R4

**STATUS**

- **id:** T3 field
- **topic:** raim-qc
- **status:** closed
- **summary:** The field reproduces the station structure but stratification buys only 1.5× with σ̂₀ pinned at its floor; the detection gate PASSED; R2 + R4 BUILT and VALIDATED (corr +0.96).
- **dates in the text:** 2026-08-12 to 2026-08-21
- **knobs / posture:** `network.raim_carry_invariant`, `network.trace_raim_redundancy`, `network.raim_station_factor`, `network.raim_rate_hygiene` (R2 and R4: measure-only, default-OFF)
- **hashes named in the text** (commit / exe / byte-cert): `65ba362` (fld63 OFF byte-identity references)
- **run dirs named in the text:** `runs/pareref_on`, `runs/t3fault`, `runs/t3trace305_smoke{,2}`, `runs/staf_{ctl,on}`
- **source:** `docs/archive/SPEC_t3_null_derivation.md` §11 (11.1, 11.2), §12 (12.1, 12.1b, 12.2, 12.3), §13 (13.1), §14 (14.1 to 14.3), §15

**Outcome.** On the field σ̂₀ sat at its 1.0 floor on 88 to 96 % of epochs, and at 303 stations a once-fitted station scale made the flag rate worse (9.57 % to 10.31 %): the slow factor must track, and a field B2 unclamp arm is owed. Fault-fixture detection held at 20/24 with 3.9× fewer flags. The prefit census's phase column reads 1.0e5 σ on clean data (own card owed); at 305 stations post-fit RAIM screened nothing under `carry_invariant` while logging ENABLED (loader fixed). Owed work sits in [QPR 4](../queues/QPR-ITEM-4-T3-NULLS.md); the T1 flag-rate gap was fixed as tier T1r ([AMB-IONO open](../ionosphere/AMB-IONO-OPEN-QUESTIONS.md) item 5).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_t3_null_derivation.md -->
## 11. The T3 class, and two collateral defects found while inventorying it

A parallel inventory pass swept the network path for statistics that compare a non-Gaussian-null
quantity against a fixed Gaussian/heuristic quantile.  Its ranking, condensed — **the first four are
free riders on this card** (same σ̂₀, same `raim_k`, same file), so a stratified denominator fixes them
together:

| tier | statistic | null-arm excess | acts? |
|---|---|---|---|
| 1 | **T3 post-fit RAIM cell** | 1.86e7× | yes — refloats carriers |
| 1 | **T3c consensus** | 2.85e6× (4/712) | yes; also needs a *binomial* null, not a Gaussian one (§1) |
| 1 | **T4a attribution ARC** | 4.89e5× (21/21788) | yes, **at any σ̂₀** |
| 1 | T4 attribution SAT | 0 flags — σ̂₀-trigger-conjoined, structurally quiet | free rider |
| 2 | **rx-CB inject gate** (the QUEUE's named sibling) | 26.7–28 % trip rate | yes — **highest measured rate in the inventory** |

⚠ **The rx-CB sibling does NOT generalize from this card** and should not be planned as if it does:
its numerator is a *frame offset* (`value_abs − x_lin`), not a residual, so it has no null to derive.
The inventory's reading is that its principled replacement is already built — LV-4's
frame-invariant, peer-relative, median-removed `rxCbStepGate` — and the card is "arm LV-4 and grade
it", blocked by `SPEC_level_information_budget.md` §19.3 (+64 % postfit phase rms, ~15 TECU level
move) and by the absence of any truth-clean fixture that arms `carry_rx_code_bias`.  **I have not
independently verified that chain**; treat it as a pointer, not a finding.

### 11.1 Collateral defect — the prefit census's PHASE statistic is degenerate

Verified directly on `runs/pareref_on/logs_v2/network_epoch_status.csv`, all 120 epochs,
**truth-clean fixture**:

| column | median | max |
|---|---|---|
| `pc_code_max_s` | 75.1 σ | 103.9 σ |
| `pc_code_p99_s` | 48.1 σ | 58.4 σ |
| `pc_code_n1k` | **0** | 0 |
| `pc_phase_max_s` | **1.024e5 σ** | 2.865e5 σ |
| `pc_phase_p99_s` | **5.001e4 σ** | 2.518e5 σ |
| `pc_phase_n1k` | **112 rows/epoch** | 265 |

The code class is sane; the phase class is **four to five orders of magnitude out** on data with no
faults in it.  `trace_prefit_census` is in the standard armed set (CLAUDE.md: "the ONLY always-on
PRE-solve screen"), so this column is being published network-wide.  It is **operationally inert only
by accident** — `prefit_event_classes` defaults to `code,sh,other`
(`GnssConfigLoadNetwork.cpp:1410-1412`), so phase is outside the event mask and 0 PREFIT events
fired.  **Anyone who adds `phase` to that mask, or arms `prefit_fuse_classes` with phase, gets ~112
fuse events per epoch on clean data.**  The column is unusable as published.  (Independently
consistent with the 305-station note in `project_slant_triple_handoff` — census phase p99 diverging
to 3.1e6 σ by epoch 120.)  Own card owed.

### 11.2 Collateral defect — T1's flag rate excludes an acting sub-path

`network_flag_rates.csv` reports `T1, IF-phase slip, 26482 opp, 0 flags, rate 0` while
`network_events.csv` on the same run carries **13 `T1_if_slip / refsat_reset` events**.  `fr_if_slip`
is incremented only inside the per-pair loop (`NetworkSlipQc.cpp:530`); the reference-satellite
common-mode path refloats and emits with **no counter and no flag-rate tier**.  This is exactly the
§9.7.10 error class that the T4a line was added to fix — one tier over, unfixed.  Standing invariant
#4: an instrument that does not record its acting sub-path reads as "nothing happened".

---

## 12. Field check — the structure reproduces, the remedy's leverage does not

Run on `net_matrix_2024-129/net50_1h` (DOY-129 **real data**, 49 stations, GPS+Galileo, 115 epochs,
76 099 cells) from the 2026-08-12 archive.

⚠ **Provenance and scope.**  These are archived pre-VS2026 runs that CLAUDE.md flags as not trusted
for accuracy — *regenerate rather than re-grade*.  Nothing here is an accuracy verdict.  What is read
is the **structure of the residual population**, a property of the trace itself.  ⚠ And the field has
**no truth**, so its 27.3 % flag rate is **not** a false-alarm rate: an unknown share is real slips.
That asymmetry is exactly why the derivation fits on the sim.

**The structural claim generalizes, and gets stronger:**

| | sim (`pareref_on`, 24 sta) | field (`net50_1h`, 49 sta) |
|---|---|---|
| station scale spread | 4.7× | **10.5×** |
| corr(log station scale, half 1 vs 2) | +0.847 (23 sta) | **+0.873 (49 sta)** |
| per-satellite scale spread | — | 6.0× |

**But the remedy's leverage collapses:**

| scoring | sim | field |
|---|---|---|
| as shipped, held-out half | 4.4625 % | **27.58 %** |
| ÷ OOS scale(station × epoch) | **0.9736 %** (4.6×) | **18.48 %** (only **1.5×**) |
| implied c, k=3 → k=10, after stratification | 1.97 → 2.39 | **3.07 → 6.02** (still steep) |

### 12.1 Why — the σ̂₀ floor binds on the field, and that voids §2's premise

| run | σ̂₀ median | epochs **pinned at the 1.0 floor** |
|---|---|---|
| sim `pareref_on` (24 sta) | 1.247 | 38/114 = 33 % |
| field `net50_1h` (49 sta) | **1.0000** | **110/115 = 95.7 %** |
| field `h3_guard` (305 sta) | **1.0000** | **312/354 = 88.1 %** |

**On the field the denominator is a constant almost all the time.**  §2's invariance theorem holds
only while the floor is *not* binding; when it binds, $S_c$ degenerates to the bare
$\max_{i\in c}|z_i|$ compared against a fixed 6σ bar — no normalization at all.  Stratifying the pool
cannot buy much when the statistic is discarding the pool's scale by clamp.  **The field runs almost
entirely in the regime the sim barely visits.**

**Why the field pool sits below the floor is an OPEN question, and the obvious answer is contradicted
by the configs.**  The tempting explanation is redundancy shrinkage ($E[w_i v_i^2] = r_i < 1$, so
$1.4826\cdot\operatorname{med}|v|\sqrt{w}$ lands under 1 and the missing $\sqrt{r_i}$ pushes it under
the clamp).  But **the field posture assumes `phase_std_m = 0.002 m`, SMALLER than the sim's 0.003**
(both `stochastic_model: elevation`), which pushes $|v|\sqrt{w}$ *up*, not down.  Inverting the two σ̂₀
values gives the elevation-scaled post-fit phase misfit:

$$\operatorname{med}(|v|/f) \;\approx\; 1.35\ \text{mm (field)} \quad\text{vs}\quad 2.52\ \text{mm (sim)}$$

**The sim's post-fit phase residuals are ~1.9× LARGER than real DOY-129 data's**, on the same
estimator.  So the field is not "noisier and clamped"; the *sim* is the loud one, and the clamp is what
a well-behaved pool looks like under this floor.  Redundancy shrinkage, a genuinely quieter field
posture, or an unrealistically noisy sim (§7 lead 1) — unresolved, and now the sharpest question in the
card, because the answer decides whether "unclamp the field" or "quiet the sim" is the next move.

### 12.1b ⚠ At 305 stations the remedy stops working — and the reason is drift

`pdp305_pinaware/h3_guard` (DOY-129 field, **303 stations present**, 354 epochs, 1 016 566 cells,
806 794 settled over 293 epochs).  The leverage trend across all three arms:

| | sim 24-sta | field 49-sta | **field 303-sta** |
|---|---|---|---|
| station scale spread | 4.7× | 10.5× | 7.1× |
| corr(log scale, half 1 vs 2) | +0.847 | +0.873 | **+0.782** |
| as shipped, held-out half | 4.46 % | 27.58 % | 9.57 % |
| ÷ OOS scale(station) | — | — | **10.31 % — WORSE than shipped** |
| ÷ OOS scale(station × epoch) | **0.97 %** | 18.48 % | **8.42 %** |
| net leverage | **4.6×** | 1.5× | **1.14×** |
| implied c, k=3 → k=10, after stratification | 1.97 → 2.39 | 3.07 → 6.02 | **2.55 → 4.78** |

**Out-of-sample station stratification makes the 305-station arm WORSE** (9.57 % → 10.31 %); adding
the fast epoch factor recovers it only to 8.42 %, a 1.14× net win, and the implied-c curve is
essentially unmoved (2.68 → 4.79 shipped vs 2.55 → 4.78 stratified).

**The mechanism is drift, visible in the transferability column.**  The station factor's
half-to-half correlation *falls* with window length: +0.847 (1 h sim), +0.873 (1 h field), **+0.782
(3 h field)** — not a sample-size effect, since each 305-station half carries ~1300 cells per station
against the 24-station arm's ~200.  **The per-station scale is not stationary over 3 h on real
data.**  A factor fitted on the first half is stale by the second, and applying a stale scale is
worse than applying none.

⚠ **This qualifies R1's premise directly.**  §4.1's "+0.847, STABLE, transferable" was measured on a
1-hour window and does not survive extension.  The slow factor must therefore **track** — an EWMA
with a real time constant — rather than be fitted once, and **that time constant is unmeasured**.
§6's slow/fast split survives (it is forced by the noise budget); what does not survive is the
assumption that "slow" can mean "constant over the run".

⚠ **Instrument limitation, found here:** `scripts/t3_null_fit.py` is pure-Python by design and
**timed out after 50 minutes** on this run's 1 M cells; the numbers above come from a numpy
re-implementation of the same decomposition.  A field-scale run is exactly where the tool matters, so
this is a defect in the tool, not in the fixture.

### 12.2 ⚠ This BOUNDS the §5 falsification — reopen B2 for the field

§5 measured `raim_redundancy_normalize` as a null **on the sim, where the floor rarely binds**.  On
the field it would lift the pool by ≈ ×1.45 and **unpin σ̂₀ on most epochs** — not a rescaling that
cancels but a change of *regime*.  The sim result therefore **does not transfer**, and B5's "do not
arm it" must be read as sim-scoped.  A field B2 arm is now an owed measurement, not a closed question.

### 12.3 Falsified in passing

The **constellation axis is a null**: GPS median `s_raw` 1.917 vs Galileo 1.831, a **1.05×** ratio,
with flag rates 27.23 % vs 27.48 %.  A GPS/Galileo mixture is *not* a third heterogeneity axis.

The field tail is genuinely extreme and is not a calibration artifact: q99 = 60.7, q999 = 879.5,
**max 38 934.8**, with 312 cells (0.66 %) above 100 and **40 above 1000**.  The sim null arm's max is
26.1.  Those are events, not miscalibration.

## 13. The gate — detection power under stratification (PASSED)

Fault arm `runs/t3fault`, config `_simnet24_inj_t3fault.json`: the `_simnet24_inj_fault` fixture (24
injected slips, ±{1,2,3,5,10,50} cycles, 2 per rung, GPS, per-band) run in the **validated null
posture** (`phase_datum_prioritized` + `phase_anchor_carry_reref` + the cell trace), same exe
snapshot.  **The station scale is fitted on the NULL arm and applied to the fault arm** — fitting it
on the arm under test would let the fault population set its own denominator.

Run-level: T3 4.186e-02 vs the null's 3.663e-02 — **the 24 injected slips add 0.52 percentage points
on top of a 3.66 % false-alarm floor.**  That ratio is the case for this card in one number.

At each statistic's own k = 6:

| statistic | detected | per-rung (1/2/3/5/10/50 cy) | cells flagged |
|---|---|---|---|
| as shipped | **20/24** | 4/4 · 3/4 · 4/4 · 4/4 · 3/4 · 2/4 | **912** |
| ÷ station scale (fitted on the null) | **20/24** | **identical** | **235** |
| ÷ station × epoch | **20/24** | **identical** | 315 |

**Detection is byte-identical — every rung, every event — while false alarms fall 3.9×.**
Matched-budget (detections achievable without exceeding a total-flag budget): at 200 flags, shipped
reaches 17/24 where station-stratified reaches **20/24**; at 300, 19 vs 20.  Below ~100 flags all
three collapse together and the comparison stops being informative.

### 13.1 Cross-tier accounting of the four T3 misses

Three of the four are **not misses of the system** — an earlier tier caught them, and T3's residual is
≈ 0 precisely *because* the arc was already refloated:

| event | Δcyc | elev | T3 `s_norm` | caught by |
|---|---|---|---|---|
| S02 G11 L5Q @304890 | −50 | 60.7° | 0.01 | T0 MW, T2 within-epoch |
| S15 G23 L2W @304740 | +10 | 10.3° | 0.07 | T1 IF-slip |
| S03 G03 L5Q @304080 | +2 | 62.1° | 0.00 | T0 MW |
| **S05 G20 L2W @305400** | **+50** | **50.1°** | **0.00** | **NOTHING — no tier fired** |

**One event is invisible to every tier**: a +50-cycle L2W slip at 50° elevation, the largest rung and
clean geometry.  A genuine detection gap, unrelated to this card, deserving its own look.

Note also the shape: **T3 catches the whole small end** (±1, ±3, ±5 all 4/4) while the ±10 and ±50
rungs are grabbed upstream by MW/IF-slip.  T3 is the small-slip tier — which is why its false-alarm
burden is worth paying down, and why a remedy that preserved detection only for large slips would
have been useless.

---

## 14. The 305-station trace smoke — it found a defect, not a cost

Two 20-epoch runs at 305 stations (GPS+Galileo, `_golden0729` lineage: `carry_invariant` + eq34 +
SH), configs `_t3trace305_20ep_smoke{,2}.json`, arms `runs/t3trace305_smoke{,2}`.  Purpose was to
measure the new traces' cost at scale before anyone armed them on a multi-hour run.  ⚠ Deliberately
NOT a grading run: 20 epochs is 10 minutes and sits entirely inside ignition, where r_i ramps
0.0000 → ~0.52 (§7.4) — no flag rate, variance factor or iono error may be read off it.

### 14.1 Round 1 — post-fit RAIM screened NOTHING while the loader logged "ENABLED"

`EXIT=0`, 20/20 epochs emitted, and:

```
T3,post-fit RAIM cell (T3),0,0,0.000000e+00
network_raim_redundancy.csv      0 rows   (header only)
network_station_residuals.csv    0 rows   (header only)
```

**Mechanism.**  `ApplicationNetworkAssembly.cpp:2603-2605` gates the RAIM pool fill on

```cpp
const bool do_amb_carry = (phase_datum_prioritized || !st.is_pivot) && !carry_invariant;
```

so under `carry_invariant` **`R.pl_amb` is empty for every station**, `ambPool()` hands
`networkPostFitRaim` an empty pool, and `if (pool.empty()) continue;` skips all 305.  `phase_leveling`
does **not** rescue it — it satisfies the `carry_warm` CALL-SITE gate only.  The alternative pool
`qc_amb` is filled solely under `raim_carry_invariant`, which this lineage does not arm.

**And the loader's guard ladder had a hole that hid it.**  `GnssConfigLoadNetwork.cpp` opened with

```cpp
if (network_phase_leveling_ && !network_raim_carry_invariant_)   //  ← took this branch
    log->info("... post-fit RAIM ... ENABLED ...");
else if (network_raim_carry_invariant_) ...
else if (network_carry_invariant_)      log->warn("... INERT ... screens NOTHING ...");
```

With `phase_leveling` **and** `carry_invariant` both on, the run takes the *first* branch and the
INERT warning is unreachable — exactly the configuration where it is needed.  **Fixed**:
`!network_carry_invariant_` added to the first branch, and the carry_invariant warning rewritten to
name the pool as the prerequisite and to say explicitly that phase_leveling does not rescue it.

This is the "reports ARMED and screens NOTHING" class (standing invariant #4) *inside the ladder
built to catch it*.  The loader already carries a comment 400 lines earlier saying the same lesson
was applied to the `carry_vce_apply` family "before the run instead of after it … that costs a whole
field run to learn".  Here it cost 32 minutes instead of a field run.

### 14.2 Round 2 — the census armed, and the cost is 2× the estimate

Re-run with `network.raim_carry_invariant: true` (the pool re-host).  ⚠ That knob is
**solve-affecting** — the loader warns that it makes σ̂₀ real on this lineage for the first time, so an
armed `raim_withhold_sigma0` can begin withholding and the emitted URA moves with the σ̂₀ EWMA.  On
this arm nothing was withheld (`n_gate_withheld` = `n_sigma0_withheld` = 0).

| trace | rows/epoch | 20 ep | **3 h extrapolation** |
|---|---|---|---|
| `network_raim_redundancy.csv` | **14 397** | 287 948 rows / 28.2 MB | **5.18 M rows / 508 MB** |
| `network_ionosphere.csv` | 4 494 | 89 879 rows / 6.5 MB | 1.62 M rows / 116 MB |
| `network_station_residuals.csv` | 3 876 | 77 523 rows / 3.8 MB | 1.40 M rows / 69 MB |

| | round 1 | round 2 |
|---|---|---|
| epoch wall | **93.7 s** | **199.6 s (2.1×)** |
| total, 20 epochs | 32 min | 68 min |

Census alive and correct: 303 distinct stations, 5 signals (L1C 65 578, L2W 47 481, L5Q 28 794,
L7Q 17 999, L5X 17 254).

⚠ **Two of my own estimates were wrong.**  (1) I extrapolated ~2.6 M rows / 275 MB from the
24-station GPS-only arm; the true figure is **5.2 M rows / 508 MB**, because the field carries **5
signals, not 3**.  (2) I predicted ~27 s/epoch from CLAUDE.md's lane-off figure and measured
**93.7 s** (round 1) — 3.5× optimistic — and the re-host plus census doubles it again.

⚠⚠ **The correction landed in only ONE of the two ledgers, and this section used to claim both.**
`network.trace_raim_redundancy` IS present in
`src/core/config/GnssConfigNetworkPosture.cpp:449-450` (Tier::SCOPED) and in
`docs/CONFIG_promotion_ledger.json:1549`, but there is **no `### network.trace_raim_redundancy` entry
in `docs/CONFIG_promotion_ledger.md`** — the human-readable ledger and its machine-readable source
have drifted (the .md even names the gap in its own header note at `:118`).  So the corrected
row-count and cost figures above are **not reachable from the ledger a reader would open**.  Fix by
regenerating the .md from the .json, or by adding the row.

### 14.3 What this does NOT establish

The T3 numbers this arm produced (cell rate 20.7 %, T3c 93.1 %) are **ignition measurements on a
20-epoch window** and must not be quoted.  They are listed only to record that the tier is live.  A
field-scale T3 reading needs a settled window, and §12.1 already shows the field runs mostly in the
σ̂₀-floor-pinned regime where the statistic degenerates anyway.

## §15 R2 + R4 BUILT AND VALIDATED (2026-08-21) — the declared station factor reproduces the fitted heterogeneity at corr +0.96

**Built** (both measure-only, default-OFF, byte-identical OFF — recertified on
the fld63 pair against the `65ba362` references, products + five CSVs
identical with every new knob absent):

- **R2 `network.raim_station_factor`** (+ `_tau_s` 1800, `_ratio` 6.0): the
  §7.5 trimmed per-station variance component σ̂²_sta = Σw·v²/Σr_i, computed in
  Pass 1 of `networkPostFitRaim` with its own row de-dup mask (the MDB/s0g
  column-keyed-loop lesson), fed ahead of the uncheckable `continue` (the s0g
  E[w·v²]=r_i argument), trimmed at k·σ̂₀ from BOTH sums, r_i via the
  solve-neutral rowRedundancy read (the census disjunct gains `staf_on`).
  EWMA-tracked slow channel (freezes across withholds by construction — the
  seam is post-acceptance); DECLARED only: `network_station_factor.csv`,
  both-edge `T3_stafactor/depart|return` events at ratio 6.0 / 0.8×
  hysteresis with a 10-update warmup, counters + teardown line.  Feeds NO
  denominator (§7.8.2's R1 stays HELD).  Kernel unit-pinned
  (`StationFactorKernel` ×5: the identity, trim-both-sums, the negative
  "unavailable" sentinel, low-r_i immunity).
- **R4 `network.raim_rate_hygiene`**: structurally-untestable cells leave the
  T3 opportunity count (counted separately) and T3c's opportunity becomes the
  UNCONDITIONAL ≥min_sta-cells population.

**Validated** (`runs/staf_{ctl,on}`, `_staf_{ctl,on}.json` — knob-verbatim
JSON-round-trip clones of `_simnet24_inj_pareref_on.json`, product paths
redirected; exe MD5 3979C756…, suite 1491/208 green):

- **Measure-only proven by intervention**: armed vs control solve CSVs +
  products byte-identical.
- **The §4 structure reproduces on the fresh exe**: station-factor spread
  4.7× (0.948–4.452), OOS station×epoch 0.9736 %, corr(half1, half2) +0.8468
  — the §4.1 numbers to the digit.
- **R2 gate PASS**: corr(log fitted station scale, log run-end EWMA factor)
  = **+0.9589** over all 24 stations (gate ≥ +0.9); ranking agrees at both
  ends (S01/S07/S20/S06 top, S03 bottom); log-log slope 1.55 vs the naive
  2.0 — the trim compressing the heavy-tail stations, the §7.5 "robust
  variant is not cosmetic" effect, measured.
- **Deviation recorded, not tuned**: pre-registration expected 0 depart
  events (the 4.7× model-error spread < the 6.0 bar); the arm produced ONE
  depart/return pair (run-max ratio 6.45) — an EWMA convergence transient
  crossing the bar at τ=1800 on a 1 h window, closed by the hysteresis
  exactly as designed.  Both edges evented; cost-free on a measure-only
  channel.  A settled-window declaration study belongs to the R2 field arm,
  not to a threshold tune.
- **R4 finding — the §4 "1.67 %" was a FORMATTING ARTIFACT**: the L13 trace
  prints s_raw at 3 decimals, and 355 of the 364 "exactly 0" cells are
  formatted zeros with real sub-mm rows (testable in principle); only **9
  cells (0.04 %)** are structurally row-less.  The producer-side exact test
  is the correct population; the §4 mechanism claim ("no phase row") was
  97 % wrong about its own population while the hygiene concern survives at
  40× smaller magnitude.  T3c: opp 712 (conditional) → 3622 (unconditional),
  rate 5.62e-3 → 1.10e-3 — the implied-α comparison is now like-for-like.

**Still owed** (unchanged): the §8-step-2 different-seed null verify; the
field B2 unclamp arm (§12.2) before any FIELD grading of the declared factor;
a station-fault bench extension for the depart-event power claim; R1 stays
HELD, R3 unbuilt.

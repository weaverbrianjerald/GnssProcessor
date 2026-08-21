# SPEC — clock predictability: the two measured NO-GOs (`network.emit_clock_rate` C1, and `network.clock_model`)

> # ⛔ STOP — BOTH MECHANISMS IN THIS DOCUMENT ARE MEASURED NO-GOs. DO NOT REBUILD THEM.
>
> **1. `network.emit_clock_rate` (C1, the rate product) — NET-HARMFUL. ZOH is the MMSE
> predictor.**  Raw finite-difference C1 is **1.63× WORSE than ZOH at 30 s** and **4.74×
> worse at 2 h**.  The emitted clock is a **random walk of 0.157 m per 30 s** and no rate
> product can extrapolate a random walk.  Even a **non-causal ORACLE** (cheats with future
> epochs; unbuildable) peaks at 0.55× at 10 min and **still loses at 2 h**.  An independent
> second method fits the optimal shrinkage weight on the rate term at **α\* = 0.00 at every
> window and every horizon** — the best linear use of this C1 is to discard it.  §3.
>
> **2. `network.clock_model` (the Zucca–Tavella two-state carry) — NO-GO, premise
> FALSIFIED.**  Its RWFM premise needs Hadamard slope **p = 3**; measured **p ≈ 1**
> everywhere (network GPS 1.02 / Gal 1.05; intrinsic GPS 1.50 / Gal 0.88 / BDS 0.76 / GLO
> 0.94).  The advertised "15× more clock information" mechanism does not exist.  The
> deterministic RATE is real and large on the INTRINSIC clock (16–17× Galileo/BeiDou) but
> **~1.0× on the network residual** the model actually carries.  §10.
>
> **Both fall to ONE measured property**: the emitted network clock's epoch-to-epoch motion
> is **~98 % producer frame churn, not clock physics** — measured four independent ways
> (Hadamard slope p ≈ 1; this card's structure function σ_w = 0.157 m/30 s and `dt^0.581`;
> FC-1's shadow corr ≈ 0 with a 2.3 %→0.7 % headroom ceiling; Q3 §7's structure function
> q_phys ≈ 2.9e-8 m²/s = 0.94 mm/30 s).
>
> **The prerequisite for BOTH is Card Q3's guarded temporal anchor**
> (`docs/SPEC_clock_temporal_anchor.md`), not the reverse.  Neither card is deleted: both
> are correct, cheap, inert when off, and become estimable the moment the producer's clock
> acquires temporal stiffness.
>
> **Before any future clock-model / clock-rate arming, RUN
> `scripts/clock_hadamard_calib.py` and check the noise-type slope p first — if p ≈ 1 a
> drift state is inert.**  (Full tables: `docs/CLOCK_HADAMARD_CALIB_2026-07-14.md`.)

*C1 ladder written 2026-07-27 (Opus 5) against the built, default-OFF, never-validated C1
clock-rate card; it pre-registers what a validation run must show.  Absorbed 2026-08-15:
`docs/HANDOFF_clock_model_revival.md` (now `docs/attic/`) — the `clock_model` mechanism
catalog, its falsification and the debts a revival owes are **§10**; the loader interlocks
common to both knobs are **§11**; repaired companion pointers are **§12**.  The two docs
are ONE investigation: the handoff's banner terminates by PIVOTING into the C1 rate product
that this card then falsified.*

**Convention:** `[M]` measured and re-verified · `[E]` estimated/extrapolated · `[?]`
hypothesis.  Line numbers in §1–§9 were re-grepped 2026-07-27 and refreshed 2026-08-15;
they drift — verify by symbol.  Anchor-drift ledger (older notes → current, verified
2026-08-15): driver seam calls `ApplicationNetwork.cpp:1534-1585` → `:2082` (SolPrep) …
`:2166` (ClockQc), `ApplicationNetwork.cpp:1585` → `:2166`; `NetworkSolPrep.cpp:209-217` →
`:220-229` (the `clock_model` R11 gate); `NetworkClockQc.cpp:288-292` → `:287-289` (the
estimator body), `:290` → `:288` (the gap gate).
⚠ Every `runs/…` path resolves under `D:\GnssProcessor_archive\2026-08-12\`.

---

## 0. Executive summary — read this before queuing a single run

The card is **present, complete, and default-OFF** (§1), never built into a validated run
and never graded.  This spec's central contribution is that **the decisive validation was
run offline, on data already on disk, before any producer run** (§3).  Emulating the exact
implemented estimator against the DOY-129 reference stream:

> **The C1 product as implemented makes a consumer's clock reconstruction WORSE at every
> horizon tested, from 30 s to 2 h.** Raw finite-difference C1 is **1.63× worse than ZOH at
> 30 s** and **4.74× worse at 2 h** `[M]`.

Structural, not a tuning miss: the emitted clock's epoch-to-epoch motion is a **random walk
of 0.157 m per 30 s** `[M]`, and *no rate product can extrapolate a random walk*.  Even a
**non-causal oracle** — cheating by fitting across future epochs — only buys 31–45 % at
5–30 min and **still loses at 2 h** (§3.5) `[M]`.

**Recommendation: do not queue the field-validation runs as written in
~~`docs/HANDOFF_c1_clock_rate_producer.md` §4.5~~** (⚠ **NOT IN TREE** — the prohibition
stands, the referent is unreadable; §12).  Queue the **D0 decision gate** (§4) instead — a
zero-cost offline re-measurement against the guarded stream.  The full ladder (§6) is
specified and ready but should only be spent if D0 inverts the verdict.

The card is not wasted work: correct, cheap, decoupled, and valuable the moment the
producer's clock acquires temporal stiffness.  That is Q3's card, not this one (§5).
**C1's prerequisite is Q3, not the reverse.**

---

## 1. Status correction — the card is IN TREE (the memory was right)

`project_network_clock_rate_c1` records this card as *"BUILT (default-OFF, UNBUILT
binary)"*.  Accurate, with three updates:

1. **It survived the network decomposition.**  The Jul-14 implementation lived in
   `ApplicationNetwork.cpp`, which INC-0..12 rewrote from 8,903 → ~1.6k lines; the card was
   *relocated into the seams*, correctly, per the anti-regrowth decision tree:

   | concern | seam | anchor |
   |---|---|---|
   | config parse + interaction log | `GnssConfigLoadNetwork.cpp` | `:1696-1702` |
   | getter / member / Doxygen | `GnssConfig.hpp` | `:3404-3424`, `:4764` |
   | run knob | `NetworkRunContext.hpp` | `:130` `emit_clock_rate_on` |
   | carried rate state | `NetworkRunContext.hpp` | `:285-289` `clock_rate_est` |
   | the estimator update pass | `NetworkClockQc.cpp` | `:257-295` |
   | the emit-time C1 stamp | `NetworkSolPrep.cpp` | `:222-250` |
   | warm-resume record | `NetworkStateSnapshot.hpp` | `:65` (`O,` record), `:165` |
   | snapshot restore | `NetworkRunContext.cpp` | `:479-485` |
   | driver aliases | `ApplicationNetwork.cpp` | `:315`, `:359` |
   | config round-trip test | `test_codebias_network_config.cpp` | `:701`, `:770-791` |

2. **"UNBUILT binary" is stale.**  Every binary compiled since contains it, compiled and
   inert.  There is nothing to build.  `[M]`
3. **⚠ CORRECTED 2026-08-15 — the card is COMMITTED, not working-tree state.**
   `network_emit_clock_rate_` is at `include/config/GnssConfig.hpp:6043` in HEAD (getter
   `networkEmitClockRate()` `:4431`), the estimator at
   `src/core/filter/NetworkClockQc.cpp:267-292`, tree clean.  The original text's inference
   — that a `git log --all -S` miss was correct *because the card was uncommitted* — no
   longer holds.

**Method warning, paid for and still valid.** The `Grep` tool returned *"No matches found"*
for `emit_clock_rate` / `clock_rate_est` / `clock_c1_m_s` against this working tree — a
false negative; shell `grep -rn` returns dozens of hits.  **Confirm any "symbol is absent"
conclusion with a second tool before acting on it.**

### 1.1 The estimator, verbatim

`NetworkClockQc.cpp:287-289` — the whole thing:

```cpp
const double sat_dt = (r.init && r.t_last > 0.0) ? (pl_now_s - r.t_last) : 0.0;
r.x1 = (sat_dt > 0.0 && sat_dt <= clk_model_max_gap) ? (z - r.x0) / sat_dt : 0.0;
r.x0 = z;
```

A **raw single-epoch backward finite difference** of the posterior solved clock `z`.  No
smoothing, no window, no weighting.  The handoff (§5) filed smoothing as a *"known
follow-up, not blocking."*  §3 shows that is exactly backwards: **the raw difference is the
worst member of the estimator family, and smoothing is on the critical path.**

### 1.2 The A/B configs are sound

`config/test_suite/_emit_c1_{off,on}.json` differ in exactly one field
(`network.emit_clock_rate`, line 2908 in both) `[M]`; the toggle parses
(`GnssConfigLoadNetwork.cpp:1696`).  A genuine A/B pair, not the A/A trap suspected while
operating on the false-negative grep.  Usable as-is.

---

## 2. Scope correction — C1 is not the Q3 fix

`docs/SPEC_day129_frame_guards.md` Part A4 answered Q3: the quiet-hour wander **is**
satellite-clock wander (clock alone 2.83 m/hr median hour-to-hour, 16.5 m hour-pair span
`[M]`), and prescribed *"give the clock product a guarded temporal anchor (clock model / C1
clock-rate product with event response)."*  That parenthetical conflates two mechanisms:

| | what it changes | effect on producer wander |
|---|---|---|
| **temporal anchor** (clock model carry) | injects a prior; the solve is **not** byte-identical | reduces the wander |
| **C1 emission** (this card) | injects **no** prior, drives **no** refloat; solve **byte-identical** by design (`NetworkClockQc.cpp:259-261`) | **none — zero, by construction** |

**A byte-identical solution cannot reduce producer wander.**  C1 is a *consumer coasting*
product; the anchor is a *producer stiffness* product.  Grading C1 against Q3 would be a
category error.  §3 shows the dependency runs the other way: **the wander is what defeats
C1.**

---

## 3. The offline verdict — measured, no run required

### 3.1 Method

Source `runs/day129_cont/seg01/logs/network_solution.csv` — the continuous 305-station
DOY-129 reference run (2160 epochs, 06:00→23:59:30, one segment, no seams), CLK rows only.
Grading is **datum-free per standing invariant #5**: every statistic is computed on
**between-satellite pair differences** `d_ij(t) = c_i(t) − c_j(t)`, invariant to the clock
datum (including the two pivot handovers) and to the epoch-common constant a user clock
absorbs.  Per-satellite figures are pair figures ÷ √2.  Scale is **robust** (1.4826 × MAD),
so the three SH-VTEC excursions do not drive the result.  Quiet window 06:00–18:00, 24
satellites present on ≥99.5 % of epochs, 276 pairs `[M]`.

### 3.2 The emitted clock is a random walk, not a noisy smooth signal

Structure function `D(n) = robust rms[ c₀(t+nΔt) − c₀(t) ]`, per satellite, datum-free `[M]`:

| lag | 30 s | 60 s | 120 s | 240 s | 480 s | 960 s | 1920 s | 3840 s | 7680 s | 15360 s |
|---|---|---|---|---|---|---|---|---|---|---|
| D | 0.157 | 0.216 | 0.318 | 0.521 | 0.888 | 1.535 | 2.728 | 4.994 | 9.070 | 17.02 |
| ÷ √n scaling | 1.00 | 0.98 | 1.02 | 1.18 | 1.42 | 1.73 | 2.18 | 2.82 | 3.62 | 4.81 |

- **n ≤ 4 (≤ 2 min): pure √n** — a **random walk** with step **σ_w = 0.157 m per 30 s**.
  Confirmed independently: the robust lag-1 autocorrelation of the increment series is
  **−0.024** `[M]`; white measurement noise on `c₀` would give **−0.5**, a random walk 0.
- **n ≥ 8: super-√n** — a coherent drift.  Solving `D(n)² = σ_w²·n + (r·nΔt)²` across
  n = 8…512 gives a **strikingly stable** `r_rms ≈ 1.1–1.3 × 10⁻³ m/s ≈ 4.0–4.7 m/hr` `[M]`
  (cross-checks Q3's independently measured 2.83 m/hr ✓).

Crossover: `n* = σ_w²/(r·Δt)² ≈ 19 epochs ≈ **9.5 min**` `[M]`.

**Why this is producer churn, not satellite physics.**  A GPS Rb clock at
σ_y(30 s) ≈ 2 × 10⁻¹³ contributes ≈ **1.8 mm** per 30 s; a-priori CLK interpolation error
adds at most a few cm `[E]`.  The measured **157 mm** is ~2 orders above the physics — the
producer's own frame churn, exactly as Q3's structural reading predicts (*"nothing carries
absolute per-sat clock information across epochs"*).

### 3.3 The consequence, stated once

```
        ZOH:  MSE = (r·T)²           +  σ_w²·(T/Δt)
   perfect C1: MSE =                    σ_w²·(T/Δt)      ← the RW term is IRREDUCIBLE
 estimated C1: MSE = ((r − r̂)·T)²    +  σ_w²·(T/Δt)
```

The random-walk term is **common to every arm** and cannot be predicted by any rate
product.  C1's entire achievable benefit is removing `(r·T)²`, and it only helps at all if
`|r − r̂| < |r|`, i.e. if the estimator beats "assume zero".

### 3.4 C1 vs ZOH — the implemented estimator, emulated exactly

Per pair series, at each epoch: fit the trailing-window OLS slope `r̂` over `N` epochs
(**N = 2 is the implemented raw finite difference**), predict `d(t+T) = d(t) + r̂·T`,
compare against ZOH (`r̂ ≡ 0`).  Robust rms of the residual, per satellite, quiet window
`[M]`:

| lag ↓ / window N → | **2 (as built)** | 5 | 11 | 21 | 61 | 121 |
|---|---|---|---|---|---|---|
| 0.5 min | **1.63×** | 1.20× | 1.06× | 1.03× | 1.00× | 0.99× |
| 1 min | **1.93×** | 1.26× | 1.09× | 1.02× | 0.99× | 0.99× |
| 5 min | **2.72×** | 1.43× | 1.12× | 1.02× | **0.93×** | 0.96× |
| 10 min | **3.04×** | 1.51× | 1.16× | 1.05× | **0.94×** | 0.95× |
| 20 min | **3.39×** | 1.65× | 1.24× | 1.07× | 0.96× | 0.97× |
| 60 min | **4.05×** | 1.95× | 1.46× | 1.24× | 1.07× | 1.10× |
| 120 min | **4.74×** | 2.34× | 1.80× | 1.57× | 1.40× | 1.37× |

*(ratio = C1 error ÷ ZOH error; **> 1.0 means C1 is worse than doing nothing**)*

**The emulation is slightly generous to the card.**  The stamp at `NetworkSolPrep.cpp:230`
runs *before* that epoch's estimator update at `NetworkClockQc.cpp:288` (driver order
`ApplicationNetwork.cpp:2082` SolPrep then `:2166` ClockQc), so the shipped `c₁` is the
slope through epoch **k−1** while the emulation uses the slope through **k**.  The real
product carries one extra epoch of lag and is marginally **worse** than every number above.

Readings: (1) **the implemented N = 2 estimator loses everywhere, by 1.6× to 4.7×** — no
operating point ships a win.  (2) **Smoothing is necessary but not sufficient**: the best
cell in the table is N = 61 (30 min trailing window) at 5–10 min horizon, **0.93–0.94×, a
~6 % improvement** — the *whole* payoff envelope of a correctly-smoothed C1 on this
producer.  (3) **Beyond ~30 min every window loses**; at the 1–2 h horizons a withhold era
creates (§5.2), C1 is strictly harmful.

### 3.5 The oracle ceiling — how good could any C1 ever be?

Replacing the causal trailing fit with a **centered, non-causal** fit (uses future epochs;
**no producer can build this**) bounds what any estimator could achieve `[M]`:

| lag | quiet 06–18 | evening 18–24 |
|---|---|---|
| 5 min | 0.69× | 0.89× |
| 10 min | **0.55×** | 0.71× |
| 30 min | 0.66× | 1.03× |
| 60 min | 0.85× | 1.48× |
| 120 min | **1.23×** | 2.55× |

Even cheating, the ceiling is a 45 % improvement at 10 min in the *quietest* hours — and
the oracle **still loses at 2 h** and is roughly break-even or worse across the whole
evening.  The realizable causal gain (~6 %) sits far below even this ceiling.

### 3.6 Limits of this analysis — what it does and does not establish

- **Reference:** grades reconstruction of the producer's own later-epoch `c₀` — the right
  target (the correction the consumer would have received had emission continued), but
  *not* absolute clock truth.  Producer error common to both arms differences out.
- **Scope:** DOY-129, GPS only, one config, one producer.  Not shown to generalize.
- **Regime:** the reference run had **no guards armed** (`raim`, `emit_ura`,
  `phase_leveling` all false).  A guarded producer could in principle have a quieter clock
  — **exactly what D0 tests** (§4).
- **Independently reproduced by a second method** `[M]`: a parallel read-only fan-out
  graded the same stream using **p90 of datum-reduced `c0`** (per-epoch median removed)
  instead of robust rms of pair differences, plus a differencing-domain datum as a third
  variant.  Same verdict, sharper at long lag: **1.61× worse at 30 s, 4.01× at 450 s,
  7.24× at 1 h, 10.21× at 2 h.**  Two further results:
  - ZOH p90 error grows as **`dt^0.581`** — the `dt^0.5` signature of a random-walk /
    mean-reverting process, not the `dt^1.0` of a deterministic ramp.  For such a process
    the conditional expectation of the future value *is* the current value, so **ZOH is the
    MMSE predictor and any nonzero c1 strictly adds variance** — §3.3 independently.
  - Fitting an optimal shrinkage weight `α` on the rate term gives **`α* = 0.00` at every
    window and every horizon tested**.  Measured residual rate `mean 0.0246 m/s`, confirming
    the ~cm/s prediction: the problem was never the SIZE of the signal, it is that the
    signal is not PERSISTENT.
- **⚠ Not covered: `dt < 30 s` — the card's only surviving ground.**  The product cadence is
  30 s, so neither method can measure sub-epoch interpolation, the card's *stated* target
  (the SSR saw-tooth between user and product epochs).  The 30 s result (1.6× worse) is
  strong evidence against but does not strictly close it; D0 should be extended with a
  sub-cadence arm.
- **⚠ Not tested: `brdc_rts` mode**, where the correction-vs-broadcast rate is far larger
  and the drift term may dominate the random walk.  Called the real payoff regime; remains
  **`[?]` untested**.  D0-c covers it.
- **Confounded observation, reported but NOT claimed:** at matched evening hours
  (334770–338100) the guarded leg shows D(30 s) = 0.501 m vs the reference's 0.389 m `[M]`.
  **Not** evidence guards hurt — the guarded leg seeded cold at 13:00 (1.8 h carry age vs
  14.8 h) and had just exited a 2 h withheld era.  Do not cite it as a guards contrast; D0
  replaces it with a clean one.

---

## 4. **D0 — the decision gate. Run this before anything else.**

**Cost: zero producer runs.**  Re-run the §3 analysis against streams the reference run
could not provide:

| arm | source | question |
|---|---|---|
| D0-a | guards chain `runs/day129_guards/leg01` (quiet emitted blocks) | does an **armed** producer have a smaller σ_w? |
| D0-b | same, matched-hour vs reference | a **clean** guarded/unguarded contrast at equal carry age |
| D0-c | any `brdc_rts` stream (`runs/card8_brdc_rts`) | does the **broadcast-apriori** regime invert the verdict? |

**Pre-registered pass criterion.**  D0 passes iff, for some causally realizable window N,
`C1/ZOH ratio ≤ 0.80` at a horizon ≥ 5 min, on ≥ 2 independent windows.  Rationale for
0.80: a 6 % gain (§3.4) does not justify a product, a fingerprint entry, a consumer branch
and a staleness policy; a 20 % gain does.

**If D0 fails** (the expected outcome, pre-registered `[?]`): **do not queue §6.**  Record
the falsification, leave the card in-tree and default-OFF, and reprioritise onto Q3's
temporal anchor (§5.1).  Re-open C1 automatically once the anchor lands, because the anchor
is what makes C1 estimable.  **If D0 passes:** proceed to §6, and **first replace the N = 2
estimator with the winning window** (§6, G0) — validating the raw finite difference after
§3.4 would spend a field run to re-confirm a known loss.

---

## 5. The withhold era — why it is not a C1 payoff case

### 5.1 What tonight's chain actually produced `[M]`

`runs/day129_guards/leg01`, 507 epochs so far:

| disposition | tow range | n | duration |
|---|---|---|---|
| **sh_gate_withheld** | 327600 → 334740 | **239** | **1.99 h** |
| emitted | 334770 → 338100 | 112 | 0.93 h |
| withheld | 338130 → 338520 | 14 | 0.12 h |
| emitted | 338550 → 338820 | 10 | 0.08 h |
| withheld | 338850 → 339390 | 19 | 0.16 h |
| emitted | 339420 → 340290 | 30 | 0.25 h |
| withheld / emitted (short) | 340320 → 342780 | 2 / 16 / 4 / 61 | — |

The leg **resumed at 19:00 into an already-out-of-band SH state** and withheld continuously
for **1.99 h**, releasing 720 s *after* the 20:47:00 (tow 334020) excursion it was armed to
catch.  278 of 507 epochs withheld (54.8 %).

### 5.2 Why C1 does not help across it — and what does

> **C1 is only useful paired with a fresh `c₀`.**  After a 2 h withhold the consumer's `c₀`
> anchor is itself 2 h stale, so `c₀ + c₁·dt` extrapolates from a stale base — and §3.4 says
> the `c₁` term makes it *worse*, by 1.4–4.7×.  When emission resumes the consumer receives
> a fresh `c₀` and the extrapolation problem vanishes.

**The withhold era is a staleness-horizon and integrity-signalling problem, not a
clock-rate problem.**  It belongs to the companion consumer-gap audit (⚠ **NOT IN TREE** —
§12), not to this spec.

**The R11 gap bound looks like it covers this. It does not — see §5.3.**  The tempting
reading is that `sat_dt` exceeding `clk_model_max_gap` (default = `phase_leveling_max_gap_s`
= 900 s) forces `x1 = 0` (`NetworkClockQc.cpp:290`) and ships C1 = 0 with a
`T5_clock_rate / c1_zoh_suppress` event (`NetworkSolPrep.cpp:238-242`), so a 1.99 h era —
7.9× the bound — would ZOH safely on resume.  **That reasoning is wrong**, because `sat_dt`
never grows: the estimator advances on solved epochs, which continue through the era.

### 5.3 **V1 — a latent defect, resolved this session: the R11 gate measures the wrong clock**

**Seam order, read from the driver `[M]`** (anchors refreshed 2026-08-15 against the
2,359-line driver; the ORDER is the load-bearing claim and is unchanged):

```
:2035  SB_SOLVEGLUE   networkSolveGlue      ← SH-gate verdict computed here (post-solve)
:2082  SB_SOLPREP     networkSolPrep        ← early-returns on sh_gate_withheld; STAMPS C1
:2101  SB_EMITSTORE   networkEmitStore      ← early-returns on sh_gate_withheld
:2166  SB_CLOCKUPD    networkClockQc        ← UNCONDITIONAL; UPDATES the rate estimator
```

The withhold early-returns live **inside** `networkSolPrep` / `networkEmitStore`; the
driver's `networkClockQc` call is **not guarded**, so its rate pass runs on **every** epoch,
withheld or not.  Tracing the 1.99 h era: `clock_rate_est[sat].t_last` advances every 30 s
straight through it, so on the first emitted epoch after it the stamp
(`NetworkSolPrep.cpp:242`, which runs *before* that epoch's update) reads `sat_dt` = **30 s,
not 1.99 h**, the R11 gap gate **passes**, and C1 ships a rate finite-differenced **between
the last two withheld epochs** — across the very frame the SH gate declared out-of-band and
refused to publish.

**The precise defect: R11 gates on *solve* continuity, but the consumer experiences
*emission* continuity.**  With no withhold mechanism these were the same quantity, so the
gate was correct as written.  The Phase-2.2 SH gate — which withholds emission while solve,
carry and QC all continue — decouples them, and the staleness guard silently passes across a
consumer-visible 2 h gap.  Standing invariant #1 in its exact form: a time-linked quantity
whose event response does not fire on the event that actually occurred.

**It would have fired on this exact leg.**  `leg01` is a **warm resume** with
`n_emitted = 720` restored (`NetworkRunContext.cpp:521`; `stdout.log` "WARM RESUME …
n_emitted 720") `[M]`, and `n_emitted` is a monotone latch that never decrements, so
`emit_rate_warm` (`NetworkSolveGlue.cpp:578-580`) would have been **true from epoch 1**: the
finite difference would have run every 30 s through all 239 withheld epochs and a **nonzero
C1 would have been published on the first emitted epoch (tow 334770)** — `sat_dt = 30 s ≤
900 s`, gate passed — carrying a rate differenced from inside the era the gate had just
declared unpublishable.  The hazard is not "C1 goes quiet across the gap"; it is **"a
corrupt rate is published at release."**

**Severity: latent, not live.**  `network.emit_clock_rate` is OFF (default and in every
current config; `leg01/snap.txt` contains zero `O,` and zero `K,` records `[M]`, so both C1
mechanisms are dormant and a `leg02` resume inherits no rate state; re-measured 2026-08-21 —
exactly one config arms `emit_clock_rate`, `config/archive/_emit_c1_on.json`, and it is
archived, so no live lineage is affected).  `network.clock_model` shares the same gate at
`NetworkSolPrep.cpp:220-229` and is **not** universally off: **3** configs resolve it true,
two of them LIVE — `config/ppp_rtk_network_brdc_rts.json` and `config/_rtseam_cl.json`
(the third, `config/archive/_net305_clockmodel_1h.json`, is archived).  **No FIELD run on the
`_ft63`/`_golden` lineage is affected**, but the "likewise off" claim was wrong: those two
configs do reach the C1 mechanism, so the fix below is a prerequisite for them, not merely
for a future arming.  *(Counts are `extends`-resolved via `scripts/gnss_config_chain.py`; a
literal grep undercounts — see `docs/SPEC_raim_pool_rehost.md` §1.2.)*  But the fix is a prerequisite for arming either toggle: the card needs an
**emitted-epoch** staleness clock (a `t_last_emitted` advanced only on actual emission)
alongside the solved-epoch one, and must ZOH on the emission gap.  Prefer that over gating
on `sh_gate_engaged` — it also covers `WithheldGate` and `WithheldSigma0`.  Fold into §6 G0.

*Structural note.*  This is **one missing guard, not a two-halves design fork.**  The
advance is lexically nested inside `if (emit_rate_warm)` (`NetworkClockQc.cpp:267`) and
`n_emitted` never un-arms, so solved-epoch and emitted-epoch keying cannot disagree
post-warmup.  What is missing is an emission-continuity term on the R11 test.

---

## 6. The full validation ladder — **specified, gated on D0**

Pre-registered so that if D0 passes there is no design left to invent.  **Do not spend these
runs unless D0 passes.**

**G0 — estimator replacement + the §5.3 emission-continuity guard.**  Swap the N = 2 raw
difference for the D0-winning trailing window and add the `t_last_emitted` staleness clock.
Keep the R11 gap gate, the datum-wipe reseed (`NetworkClockQc.cpp:270-280`) and the
warm-resume `O,` record.  Charge the lag cost: a causal N-window carries `(N−1)Δt/2`
(N = 61 → 15 min).  If a second snapshot record is needed the free tags are **F, G, P, Q,
T** — taken: `{A,C,D,GG,I,J,K,L,M,N,O,R,SV,U,V,W,X,Y,Z}`; avoid B/H/S (they read as
carry-blob tags).
**Cadence traps to check before arming** `[M]`: the toggle is inert when `clock_model` is on
(`NetworkRunContext.cpp:106`), and inert when
`gnss_rate_s × epoch_decimation_n > clock_model_max_gap_s`
(`GnssConfigLoadNetwork.cpp:1707-1723`) — the latter check is skipped entirely when
`gnss_rate_s = 0`, which is tonight's configuration.  At 30 s vs 900 s it would not fire.

**G1 — unit.**  `gnss_tests --gtest_filter=NetworkEmitClockRate.*:NetworkProductIO.*`, then
the full suite; plus a pure-helper test for the new window estimator (known ramp → exact
slope; short history → 0; gap → 0).

**G2 — OFF byte-identity** (invariant #6).  Same-build golden A/B, `_emit_c1_off.json` vs
the frozen binary: `products.txt`, `network_solution.csv`, `network_trace.csv`,
`network_epoch_status.csv`, `network_events.csv` identical by SHA-256.  Same build config
only — never Debug vs Release.

**G3 — ON decoupling proof.**  `_emit_c1_{off,on}.json` (10-ep smoke first).  Assert every
non-C1 column of `network_solution.csv` is **byte-identical** across arms and `_on`'s C
records carry a nonzero field 4.

**G4 — estimator quality (THE decisive gate).**  On the ON arm, re-run §3.4 *against the
emitted C1 itself* rather than an emulation: does the shipped `c₁` beat ZOH at the
D0-identified operating point, by the D0 margin?  **G5 without G4 is theatre.**

**G5 — consumer replay A/B.**  `Application::runConsumerReplay`
(`network.closed_loop.replay_products_path`, ~33 s/arm), arms = the G3 product streams,
graded on `logs/consumer_replay_hourly.csv` position.  ⚠️ **Pin
`processing.signal_priorities` as explicit ARRAYS** — attribute strings do not bind the
signal and will silently give you two arms that differ in L1 instead of in C1.

**G6 — event response** (invariant #1).  Force-fire: a datum re-reference reseeds without a
spurious rate spike; a > R11 return ships C1 = 0 for one epoch with the `T5_clock_rate`
event; a warm resume restores `clock_rate_est` exactly via the `O,` record.

**G7 — run-end counters** (invariant #4).  Report epochs stamped with nonzero C1, ZOH
suppressions and reseeds, with an explicit WARN on the all-zero inert case.  Verify the
counters are non-zero before believing any ON-path claim.

---

## 7. What not to do

- **Do not queue ~~`HANDOFF_c1_clock_rate_producer.md` §4.5~~ as written** (NOT IN TREE —
  §12).  Its premise — that a low-latency consumer is saw-tooth-limited by ZOH — is
  **falsified for this producer** at every horizon from 30 s to 2 h (§3.4).
- **Do not smooth blind.**  The handoff says *"validate raw first."*  §3.4 already validated
  raw: it loses by 1.6–4.7×.  D0 selects the window empirically.
- **Do not grade C1 on Q3's wander** (§2) — byte-identical by construction, cannot move it.
- **Do not treat the withhold era as a C1 test bed** (§5.2).
- **Do not delete the card.**  Correct, decoupled, inert when off, and the natural second
  half of the Q3 anchor work.

---

## 8. What I could not self-verify

1. **`brdc_rts` regime `[?]`** — the one place the card's premise might survive.  D0-c.
2. ~~V1: the `NetworkClockQc`-vs-withhold seam order~~ — **RESOLVED** by reading the driver;
   a latent defect, written up in §5.3.  Only its *magnitude* if the toggle were ever armed
   remains unverified.
3. **Guarded σ_w `[?]`** — D0-a/b; the §3.6 observation is confounded and is not evidence.
4. **Generalisation `[?]`** — one day, one constellation, one producer.
5. **Nothing here was run through a compiler** — all analysis is offline over existing CSV
   artifacts.
6. **Sub-30 s interpolation** (§3.6) — the card's stated target, unmeasurable at 30 s cadence.

---

## 9. Method note

§1, §2, §5 and the §3 measurements were established first-hand, then cross-checked by a
77-agent read-only fan-out (adversarial refutation on every load-bearing claim; 38
confirmed, 33 corrected).  Corrections that changed this document: the §5.2
self-contradiction (the R11 bound does **not** cover the era — §5.3 was right and §5.2 as
first written was wrong); the warm-resume sharpening in §5.3; the "one guard, not a fork"
framing; and the §3.6 independent reproduction.  ⚠ The two documents that backed it —
~~`docs/REPORT_consumer_gap_dossier.md`~~ (the full dossier) and
~~`docs/AUDIT_consumer_product_gap.md`~~ (the companion consumer audit) — are **NOT IN
TREE** (§12).  The 38/33 counts are the surviving record.

---

## 10. The predecessor mechanism — `network.clock_model`, the two-state carry (NO-GO)

*Absorbed from `docs/HANDOFF_clock_model_revival.md` (2026-07-14, Fable/Opus).  This card is
the PIVOT OUT of that one: its banner terminated by handing off into the C1 rate product,
which §3 above then falsified.  Kept as a separate top-level section because it is a
DIFFERENT KNOB with its own contract — do not blur the two.*

### 10.1 ⛔ Verdict

**`network.clock_model` is a NO-GO**, on two independent findings:

- **RWFM is absent everywhere (the premise is false).**  Hadamard slope **p ≈ 1** (white-FM)
  for BOTH the emitted network clock (**GPS 1.02 / Gal 1.05**) AND the intrinsic CODE MGEX
  precise clock (**GPS 1.50 / Gal 0.88 / BDS 0.76 / GLO 0.94**) — against the **p = 3** the
  two-state premise needs.  q2 upper bounds: network **GPS ≤ 8.7e-9 / Gal ≤ 2.7e-8** (~17×
  below the documented 1.5e-7); intrinsic **GPS ≤ 5.8e-12 / Gal ≤ 1.7e-13** (4–6 orders
  below).  Expected clock physics: WFM/flicker dominates 30 s–30 min, RWFM only emerges at
  hour+ averaging.  **The "q2·dt³/3 = 3.7 cm" tightness mechanism does not exist.**  (The
  1.5e-7 measured the orbit/datum-contaminated network *residual*.)
- **The real asset is the DETERMINISTIC RATE, and the residual carry cannot see it.**
  Measured as RMS(Δx)/RMS(Δ²x), a two-state drift state beats the scalar carry by **16–17×
  on the INTRINSIC clock** (Galileo/BeiDou), **1.7× GPS** — the satellite's predictable
  frequency.  On the **network residual** — what `clock_model` actually carries — the
  advantage is **~1.0× (Gal 0.9× / GPS 1.1×), i.e. nothing**: the deterministic rate has
  already cancelled against the precise a-priori, leaving ~0.2 m/epoch estimation + datum
  noise.  *(Intrinsic small-τ carries a phase-interpolation caveat — the CODE 30 s values
  are phase-interpolated from a 300 s estimate — so treat the multiplier as
  order-of-magnitude and the ranking Gal ≫ GPS ≫ GLONASS as real.)*

Also disqualifying: **arming at q2 = 1.5e-7 would NIS-storm**, and the gain is moot on a
dense net where the clock is strongly observed every epoch.

**Full tables (per-constellation and 256 per-satellite fits, incl. Gate A's five flagged
>10×-block-median outliers E21 / E36 / G03 / G25×2):
`docs/CLOCK_HADAMARD_CALIB_2026-07-14.md`.**  That file is REGENERATED by
`scripts/clock_hadamard_calib.py` — never hand-write card prose into it.

### 10.2 The mechanism, catalogued (this exists nowhere else in prose)

A per-satellite **Zucca–Tavella two-state filter** `x = [offset (m), drift (m/s)]` on the
ESTIMABLE clock:

```
Q(dt) = [[ q1·dt + q2·dt³/3 ,  q2·dt²/2 ],
         [ q2·dt²/2         ,  q2·dt    ]]
```

- **measurement** = the epoch's network-solved offset, `R = clock_model_meas_std_m² =
  (0.05 m)²`;
- **per-sat true-elapsed-time gaps** (eclipse-safe);
- **stale reseed** at `clock_model_max_gap_s` — the **T5** event;
- **NIS fault flag** at `clock_model_k_sigma = 6`, per-constellation datum common mode
  removed;
- **when armed**, the shared clock column's carry prior becomes the two-state prediction
  (`info = 1/p₀₀`) instead of the scalar `var + sat_clock_q·dt`.

**Source pointers:** `clockOscPredict` and `struct ClockOsc`
(`include/filter/NetworkRunTypes.hpp:701` / `:655`), `stale_prior_reseed` (T5), the `datum
common-mode removed` path, and the `networkClockModel*` getter block in `GnssConfig.hpp` —
whose `networkClockModelQ2()` Doxygen (`:4376`, member `:5958`) holds the calibration
provenance.

### 10.3 The falsified arithmetic that motivated the revival

The claim: at 30 s the prediction σ is `q2·dt³/3 ≈` **3.7 cm/epoch vs ~55 cm for the scalar
carry ≈ 15× more clock information**, plus lock-ins becoming detectable NIS faults instead
of silent wander.  §10.1 falsifies the premise it rests on.  **The dt³ consequence survives
as a standing rule:** at 300 s the prediction σ is **~1.16 m**, so the leverage is a
**HIGH-RATE asset** — **never evaluate this card at the 300 s dev tier.**

### 10.4 History to respect — the q provenance and the one negative data point

- **The shipped defaults were EMPIRICAL, not datasheet:** `q2 = 1.5e-7 m²/s³` (GPS ≈ 1.1e-7
  / Galileo ≈ 1.8e-7), from an Allan/Hadamard inversion of the network Δδtˢ residual,
  recorded in the `networkClockModelQ2()` Doxygen.  §10.1 is therefore a **re-measurement**,
  not a correction of sloppiness.
- **The one negative field data point, read correctly:** the Jul-3 closed-loop consumer
  campaign set `clock_model_q2 = 1e-8` and it tested **WORSE**.  That was a hand override
  **~15× stiffer than the measured value** — datasheet-thinking applied to an estimable
  containing BRDC clock-polynomial error plus radial orbit error.  **It falsified the wrong
  q, not the model.**
- **Recalibrate-first rationale:** everything datum-side changed after the original
  calibration (sum-to-zero clock gauge, eq34 chain datum, `carry_invariant`, pin-aware PD,
  the parallel seam), so the q had to be re-derived before any arming.  That recalibration
  produced §10.1.

### 10.5 The Phase-A instrument (reusable, and the standing pre-check)

`scripts/clock_hadamard_calib.py` — overlapping **Hadamard** (3rd-difference,
drift-insensitive) primary with **Allan** (2nd-difference) cross-check; synthetic self-test
**recovers a known q2 to ratio 0.996** and is drift-immune where Allan is drift-biased;
`--clk` mode ingests a RINEX CLK product to strip SP3 orbit error and datum wander.  Model
fitted: `Var(Δ³x) = 20·σ_ε² + 6·q1·τ + q2·τ³` over τ ∈ {30, 60, 120, 300, 600, 1800} s.

- **Input requirement:** **≥3 h of 30 s epochs per satellite is sufficient for
  τ ≤ 1800 s.**  Sibling trap already carried in the script (`:77`, `:952`): do NOT
  calibrate from a 300 s run (τ floor too high, D5 churn contaminates).
- Calibration input for the 2026-07-14 tables:
  `data/2024-129/products/COD0MGXFIN_20241290000_01D_30S_CLK.CLK`.
- **Unbuilt deliverable, recorded because the knob names exist nowhere else:** if
  intra-constellation block spread is > 3×, propose per-constellation q₂ override keys
  (`clock_model_q2_gps`, `clock_model_q2_gal`; a tiny `GnssConfigLoad` addition, default =
  the global key ⇒ byte-identical).  Never built; NOT IN TREE.

> **STANDING REUSE INSTRUCTION:** run it and check the noise-type slope **p** BEFORE any
> future clock-model / clock-rate arming.  **If p ≈ 1 a drift state is inert.**

### 10.6 ⚠ The unpaid debt any revival still owes — the invariant-#1 pre-flight

Phase B was **never run** (Gate A failed by measurement first), so this was never discharged
and it does not expire.  The two-state carry is a **stiffer time-linked prior**, so before
any graded run every event response must be **FORCE-FIRED once in a smoke** (the
test-cadence rule): (1) clock-monitor flag → soft large-q reinit via the
`clk_mon_reinit_var` path; (2) **T5** gap reseed; (3) IODE cutover Δ_eph re-base, with **no
spurious disc**.  All three paths exist; **the gate is proving they fire under the CURRENT
datum machinery** — sum-to-zero, eq34 and `carry_invariant` were all built *after* this
model.

**The Phase-B watch list, kept as the grading recipe a revival inherits** (counters +
graded, invariants #4/#5): NIS flag rate (clock-monitor storms = residual miscalibration; a
healthy arm flags rarely and on real events); **T5 stale-reseed events ≈ 0** (max-gap sane
at 30 s); **dd-frac hold-or-improve** vs control (the datum-invariant grade); clock
prediction residuals against the 3.7 cm/epoch expectation; and the **clock prior INFO
FRACTION** from the B7 trace, the quantified form of *"using stability to our advantage"*.
Stretch: closed-loop consumer vertical vs control.  Report honestly if the sum-to-zero datum
interacts badly with the common-mode removal — **that interaction is the untested seam**
(anchor: `datum common-mode removed`).

### 10.7 Non-goals (carried forward)

- **Datasheet q's. Never again** — q comes from §10.5 or not at all.
- **Never arm at 300 s** (the dt³ argument, §10.3).
- **Do not touch `sat_clock_q`** — the scalar path stays the default and byte-identical.
- An **emission-frame clock continuity filter** (the clock twin of the PB filter) is **NOT
  this card**; do not scope-creep into it.  *(It later became Card CLK-R,
  `docs/SPEC_clock_emit_rebase.md` — stage 1 VALIDATED, stage 2 mechanism validated / value
  not graduated.)*

---

## 11. Standing traps — the loader interlocks for this family

Catalogued per-knob in `docs/CONFIG_promotion_ledger.md`; pointers only here, so there is
one owner of the knob metadata.

- **`network.service_mode` FORCE-ARMS `clock_model`** — it force-arms the measured NO-GO of
  §10.
- **`clock_model` ON silently makes `filter.sat_clock_q` a NO-OP** (the two-state prediction
  replaces the scalar carry).
- **`clock_model` ON makes `emit_clock_rate` INERT** — `clock_osc` already supplies C1 —
  **while the load log still names `emit_clock_rate` as the active mechanism**
  (`NetworkRunContext.cpp` knob resolution).  Check the counters, not the log line.
- **`emit_clock_rate` is also INERT when epoch spacing exceeds `clock_model_max_gap_s`**:
  `gnss_rate_s × epoch_decimation_n > max_gap` fails the loader check — and **that check is
  skipped entirely when `gnss_rate_s = 0`**.  At 30 s vs the 120 s default it would not fire.
- Getter/member anchors (2026-08-15): `networkEmitClockRate()` `GnssConfig.hpp:4431` /
  member `:6043`; `networkClockModelMaxGapS()` `:4409` / member `:6042` (loader defaults it
  to `network_phase_leveling_max_gap_s_`); `networkClockModelQ2()` `:4376` / member `:5958`.

---

## 12. Companion pointers, repaired (2026-08-15)

| cited as | status | read instead |
|---|---|---|
| `docs/HANDOFF_c1_clock_rate_producer.md` (§0, §7 — "do not queue its §4.5") | **NOT IN TREE** | The prohibition stands on its own: §3.4 falsifies the premise. No surviving referent. |
| `docs/AUDIT_consumer_product_gap.md` (§5.2, §9 — the companion consumer audit) | **NOT IN TREE** | §5.1's withhold inventory is the surviving record of the staleness problem. |
| `docs/REPORT_consumer_gap_dossier.md` (§9 — the 77-agent dossier) | **NOT IN TREE** | §9's 38-confirmed / 33-corrected counts are the surviving record. |
| `docs/HANDOFF_clock_model_revival.md` | **ABSORBED** → `docs/attic/` | §10 of this document; tables in `docs/CLOCK_HADAMARD_CALIB_2026-07-14.md`. |

**Successor chain, so nobody re-walks it:** `clock_model` (§10, NO-GO — no RWFM) → **C1
`emit_clock_rate`** (§3, NET-HARMFUL — ZOH is MMSE) → **FC-1**
(`docs/SPEC_fast_clock_split_cadence.md`, NO-GO — 1.00× against a 5× bar, 2.3 % headroom
ceiling) → **Card Q3** (`docs/SPEC_clock_temporal_anchor.md`, the estimator-side guarded
temporal anchor that all three named as their prerequisite).  The emission-side half is
**Card CLK-R** (`docs/SPEC_clock_emit_rebase.md`), whose own §2 independently falsified
per-key value smoothing — the fourth face of the same root cause.

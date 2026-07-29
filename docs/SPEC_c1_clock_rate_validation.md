# SPEC — C1 satellite clock-rate product (`network.emit_clock_rate`): validation ladder

*Written 2026-07-27 (Opus 5). Target: the built, default-OFF, never-validated C1
clock-rate card. Runs are queued for later; this spec pre-registers what they must show.*

**Convention:** `[M]` measured and re-verified this session · `[E]` estimated/extrapolated ·
`[?]` hypothesis. Every line number was re-grepped 2026-07-27 and will drift.

---

## 0. Executive summary — read this before queuing a single run

The card is **present, complete, and default-OFF** (§1). It was never built into a
validated run and never graded.

This spec's central contribution is that **the decisive validation was run offline, on
data already on disk, before any producer run** (§3). Emulating the exact implemented
estimator against the DOY-129 reference stream gives an unambiguous verdict:

> **The C1 product as implemented makes a consumer's clock reconstruction WORSE at every
> horizon tested, from 30 s to 2 h.** Raw finite-difference C1 is **1.63× worse than ZOH
> at 30 s** and **4.74× worse at 2 h** `[M]`.

This is not a tuning miss. It is structural: the emitted clock's epoch-to-epoch motion is a
**random walk of 0.157 m per 30 s** `[M]`, and *no rate product can extrapolate a random
walk*. Even a **non-causal oracle** estimator — one that cheats by fitting across future
epochs, which no producer can build — only buys 31–45 % at 5–30 min and **still loses at
2 h** (§3.5) `[M]`.

**Recommendation: do not queue the field-validation runs as written in
`docs/HANDOFF_c1_clock_rate_producer.md` §4.5.** Queue the **D0 decision gate** (§4) instead
— a zero-cost offline re-measurement against the guarded stream. The full ladder (§6) is
specified and ready, but it should only be spent if D0 inverts the verdict.

The card is not wasted work: it is correct, cheap, decoupled, and it becomes valuable the
moment the producer's clock acquires temporal stiffness. That is Q3's card, not this one
(§5). **C1's prerequisite is Q3, not the reverse.**

---

## 1. Status correction — the card is IN TREE (the memory was right)

`project_network_clock_rate_c1` records this card as *"BUILT (default-OFF, UNBUILT
binary)"*. That is accurate, with two updates:

1. **It survived the network decomposition.** The Jul-14 implementation lived in
   `ApplicationNetwork.cpp`, which INC-0..12 rewrote from 8,903 → ~1.6k lines. The card was
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

2. **"UNBUILT binary" is stale.** The card is in the tree, so every binary compiled since —
   including `gnss_processor_p2chain.exe` running tonight's guards chain — contains it,
   compiled and inert. There is nothing to build. `[M]`

**Method warning, paid for this session.** The `Grep` tool returned *"No matches found"* for
`emit_clock_rate` / `clock_rate_est` / `clock_c1_m_s` against this working tree — a false
negative. Shell `grep -rn` over the same patterns returns dozens of hits. `git log --all -S`
also finds nothing, but that is *correct*: the card is **uncommitted working-tree state**.
**Confirm any "symbol is absent" conclusion with a second tool before acting on it.**

### 1.1 The estimator, verbatim

`NetworkClockQc.cpp:288-292` — this is the whole thing:

```cpp
const double sat_dt = (r.init && r.t_last > 0.0) ? (pl_now_s - r.t_last) : 0.0;
r.x1 = (sat_dt > 0.0 && sat_dt <= clk_model_max_gap) ? (z - r.x0) / sat_dt : 0.0;
r.x0 = z;
```

A **raw single-epoch backward finite difference** of the posterior solved clock `z`. No
smoothing, no window, no weighting. The handoff (§5) filed smoothing as a *"known follow-up,
not blocking."* §3 shows that is exactly backwards: **the raw difference is the worst
member of the estimator family, and smoothing is on the critical path.**

### 1.2 The A/B configs are sound

`config/test_suite/_emit_c1_{off,on}.json` differ in exactly one field
(`network.emit_clock_rate`, line 2908 in both) `[M]`. The toggle parses
(`GnssConfigLoadNetwork.cpp:1696`), so this is a genuine A/B pair, not the A/A trap I
suspected while operating on the false-negative grep. They are usable as-is.

---

## 2. Scope correction — C1 is not the Q3 fix

`REPORT_night305_day129_investigation.md` §4 answered Q3: the quiet-hour wander **is**
satellite-clock wander (clock alone 2.83 m/hr median hour-to-hour, 16.5 m hour-pair span
`[M]`), and prescribed *"give the clock product a guarded temporal anchor (clock model / C1
clock-rate product with event response)."*

That parenthetical conflates two different mechanisms, and the distinction is the whole
scoping question:

| | what it changes | effect on producer wander |
|---|---|---|
| **temporal anchor** (clock model carry) | injects a prior; the solve is **not** byte-identical | reduces the wander |
| **C1 emission** (this card) | injects **no** prior, drives **no** refloat; solve is **byte-identical** by design (`NetworkClockQc.cpp:259-261`) | **none — zero, by construction** |

**A byte-identical solution cannot reduce producer wander.** C1 is a *consumer coasting*
product; the anchor is a *producer stiffness* product. Grading C1 against Q3 would be a
category error. §3 shows the dependency runs the other way: **the wander is what defeats
C1.**

---

## 3. The offline verdict — measured, no run required

### 3.1 Method

Source: `runs/day129_cont/seg01/logs/network_solution.csv` — the continuous 305-station
DOY-129 reference run (2160 epochs, 06:00→23:59:30, one segment, no seams). CLK rows only.

Grading is **datum-free per standing invariant #5**: every statistic is computed on
**between-satellite pair differences** `d_ij(t) = c_i(t) − c_j(t)`, which are invariant to
the clock datum (including the two pivot handovers) and to the epoch-common constant a user
clock absorbs. Per-satellite figures are pair figures ÷ √2. Scale is **robust**
(1.4826 × MAD), so the three SH-VTEC excursions do not drive the result. Quiet window
06:00–18:00, 24 satellites present on ≥99.5 % of epochs, 276 pairs `[M]`.

### 3.2 The emitted clock is a random walk, not a noisy smooth signal

Structure function `D(n) = robust rms[ c₀(t+nΔt) − c₀(t) ]`, per satellite, datum-free `[M]`:

| lag | 30 s | 60 s | 120 s | 240 s | 480 s | 960 s | 1920 s | 3840 s | 7680 s | 15360 s |
|---|---|---|---|---|---|---|---|---|---|---|
| D | 0.157 | 0.216 | 0.318 | 0.521 | 0.888 | 1.535 | 2.728 | 4.994 | 9.070 | 17.02 |
| ÷ √n scaling | 1.00 | 0.98 | 1.02 | 1.18 | 1.42 | 1.73 | 2.18 | 2.82 | 3.62 | 4.81 |

Two regimes, cleanly separated:

- **n ≤ 4 (≤ 2 min): pure √n** — a **random walk** with step **σ_w = 0.157 m per 30 s**.
  Confirmed independently: the robust lag-1 autocorrelation of the increment series is
  **−0.024** `[M]`. White measurement noise on `c₀` would give **−0.5**; a random walk gives
  **0**. The data says random walk, decisively.
- **n ≥ 8: super-√n** — a coherent drift emerges. Solving
  `D(n)² = σ_w²·n + (r·nΔt)²` across n = 8…512 gives a **strikingly stable**
  `r_rms ≈ 1.1–1.3 × 10⁻³ m/s ≈ 4.0–4.7 m/hr` `[M]` (cross-checks Q3's independently
  measured 2.83 m/hr, a different statistic on the same phenomenon ✓).

Crossover, where the coherent term overtakes the random walk:
`n* = σ_w²/(r·Δt)² ≈ 19 epochs ≈ **9.5 min**` `[M]`.

**Why this is producer churn, not satellite physics.** A GPS Rb clock at
σ_y(30 s) ≈ 2 × 10⁻¹³ contributes ≈ **1.8 mm** per 30 s; a-priori CLK interpolation error
adds at most a few cm `[E]`. The measured **157 mm** is ~2 orders of magnitude above the
physics — it is the producer's own frame churn, exactly as Q3's structural reading predicts
(*"nothing carries absolute per-sat clock information across epochs"*).

### 3.3 The consequence, stated once

The error a coasting consumer accumulates over lag `T` decomposes as

```
        ZOH:  MSE = (r·T)²           +  σ_w²·(T/Δt)
   perfect C1: MSE =                    σ_w²·(T/Δt)      ← the RW term is IRREDUCIBLE
 estimated C1: MSE = ((r − r̂)·T)²    +  σ_w²·(T/Δt)
```

The random-walk term is **common to every arm** and cannot be predicted by any rate product.
C1's entire achievable benefit is removing `(r·T)²` — and it only helps at all if
`|r − r̂| < |r|`, i.e. if the estimator beats "assume zero".

### 3.4 C1 vs ZOH — the implemented estimator, emulated exactly

For each pair series, at each epoch: fit the trailing-window OLS slope `r̂` over `N` epochs
(**N = 2 is the implemented raw finite difference**), predict `d(t+T) = d(t) + r̂·T`, and
compare against ZOH (`r̂ ≡ 0`). Robust rms of the residual, per satellite, quiet window `[M]`:

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

**The emulation is slightly generous to the card.** The stamp at `NetworkSolPrep.cpp:230` runs
*before* that epoch's estimator update at `NetworkClockQc.cpp:288` (driver order `:1534` then
`:1585`), so the shipped `c₁` is the slope through epoch **k−1**, while the emulation uses the
slope through **k**. The real product carries one extra epoch of lag and is therefore
marginally **worse** than every number above. The conclusion is not sensitive to this.

Readings:

1. **The implemented N = 2 estimator loses everywhere, by 1.6× to 4.7×.** There is no
   operating point at which shipping it helps.
2. **Smoothing is necessary but not sufficient.** The best cell in the entire table is
   N = 61 (30 min trailing window) at 5–10 min horizon: **0.93–0.94×, a ~6 % improvement.**
   That is the *whole* payoff envelope of a correctly-smoothed C1 on this producer.
3. **Beyond ~30 min every window loses.** At the 1–2 h horizons tonight's withhold era
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

Even cheating, the ceiling is a 45 % improvement at 10 min in the *quietest* hours — and the
oracle **still loses at 2 h**, and is roughly break-even or worse across the whole evening.
The realizable causal gain (~6 %) sits far below even this ceiling.

### 3.6 Limits of this analysis — what it does and does not establish

- **Reference:** grades reconstruction of the producer's own later-epoch `c₀`. That is the
  right target — it is precisely the correction the consumer would have received had
  emission continued — but it is *not* absolute clock truth. Producer error common to both
  arms differences out, which is the point.
- **Scope:** DOY-129, GPS only, one config, one producer. Not shown to generalize.
- **Regime:** the reference run had **no guards armed** (`raim`, `emit_ura`,
  `phase_leveling` all false). A guarded producer could in principle have a quieter clock —
  **this is exactly what D0 tests** (§4).
- **Independently reproduced by a second method** `[M]`. A parallel read-only fan-out graded
  the same reference stream using **p90 of datum-reduced `c0`** (per-epoch median removed)
  rather than robust rms of pair differences, and a differencing-domain datum as a third
  variant. Same verdict, and sharper at long lag: **1.61× worse at 30 s, 4.01× at 450 s,
  7.24× at 1 h, 10.21× at 2 h.** Two further results worth having:
  - ZOH p90 error grows as **`dt^0.581`** — the `dt^0.5` signature of a random-walk /
    mean-reverting process, not the `dt^1.0` of a deterministic ramp. For such a process the
    conditional expectation of the future value *is* the current value, so **ZOH is the MMSE
    predictor and any nonzero c1 strictly adds variance.** That is the §3.3 argument arrived
    at independently.
  - Fitting an optimal shrinkage weight `α` on the rate term gives **`α* = 0.00` at every
    window and every horizon tested** — the best linear use of this C1 is to discard it.
  Measured residual rate `mean 0.0246 m/s`, confirming the handoff's ~cm/s prediction; the
  problem was never the size of the signal, it is that the signal is not persistent.
- **Not covered: `dt < 30 s`.** The product cadence is 30 s, so neither method can measure
  sub-epoch interpolation — which is the card's *stated* target (the SSR saw-tooth between
  user and product epochs). The 30 s result (1.6× worse) is strong evidence against, but it
  does not strictly close the sub-30 s case. If anyone wants to defend the card, that is the
  only surviving ground, and D0 should be extended with a sub-cadence arm to test it.
- **Not tested:** `brdc_rts` mode, where the correction-vs-broadcast rate is far larger and
  the drift term may dominate the random walk. The handoff called this the real payoff
  regime and it remains **`[?]` untested**. D0 covers it.
- **Confounded observation, reported but not claimed:** at matched evening hours
  (334770–338100) the guarded leg shows D(30 s) = 0.501 m vs the reference's 0.389 m `[M]`.
  That is **not** evidence guards hurt — the guarded leg seeded cold at 13:00 (1.8 h carry
  age vs 14.8 h) and had just exited a 2 h withheld era. Do not cite it as a guards
  contrast; D0 replaces it with a clean one.

---

## 4. **D0 — the decision gate. Run this before anything else.**

**Cost: zero producer runs.** Everything needed is on disk or arrives with the guards chain.

Re-run the §3 analysis against streams the reference run could not provide:

| arm | source | question |
|---|---|---|
| D0-a | guards chain `runs/day129_guards/leg01` (quiet emitted blocks) | does an **armed** producer have a smaller σ_w? |
| D0-b | same, matched-hour vs reference | a **clean** guarded/unguarded contrast at equal carry age |
| D0-c | any `brdc_rts` stream (`runs/card8_brdc_rts`) | does the **broadcast-apriori** regime invert the verdict? |

**Pre-registered pass criterion.** D0 passes iff, for some causally realizable window N,

> `C1/ZOH ratio ≤ 0.80` at a horizon ≥ 5 min, on ≥ 2 independent windows.

Rationale for 0.80: a 6 % gain (§3.4) does not justify a product, a fingerprint entry, a
consumer branch, and a staleness policy. A 20 % gain does.

**If D0 fails** (the expected outcome, pre-registered `[?]`): **do not queue §6.** Record the
falsification, leave the card in-tree and default-OFF — it is correct and costs nothing
inert — and reprioritise onto Q3's temporal anchor (§5.1). Re-open C1 automatically once the
anchor lands, because the anchor is what makes C1 estimable.

**If D0 passes:** proceed to §6, and **first replace the N = 2 estimator with the winning
window** (§6, G0). Validating the raw finite difference after §3.4 would be spending a field
run to re-confirm a known loss.

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
catch. 278 of 507 epochs withheld (54.8 %).

### 5.2 Why C1 does not help across it — and what does

A 2 h gap is not a C1 problem, for a reason that is easy to miss:

> **C1 is only useful paired with a fresh `c₀`.** After a 2 h withhold the consumer's `c₀`
> anchor is itself 2 h stale, so `c₀ + c₁·dt` extrapolates from a stale base — and §3.4 says
> the `c₁` term makes it *worse*, by 1.4–4.7×. When emission resumes the consumer receives a
> fresh `c₀` and the extrapolation problem vanishes.

**The withhold era is a staleness-horizon and integrity-signalling problem, not a
clock-rate problem.** It belongs to the companion consumer-gap audit, not to this spec.

**The R11 gap bound looks like it covers this. It does not — see §5.3.** The tempting reading
is that `sat_dt` exceeding `clk_model_max_gap` (default = `phase_leveling_max_gap_s` = 900 s)
forces `x1 = 0` (`NetworkClockQc.cpp:290`) and ships C1 = 0 with a `T5_clock_rate /
c1_zoh_suppress` event (`NetworkSolPrep.cpp:238-242`), so a 1.99 h era — 7.9× the bound —
would ZOH safely on resume. **That reasoning is wrong**, because `sat_dt` never grows: the
estimator advances on solved epochs, which continue through the era. §5.3 has the mechanism.

### 5.3 **V1 — a latent defect, resolved this session: the R11 gate measures the wrong clock**

**Seam order, read from the driver `[M]`:**

```
:1487  SB_SOLVEGLUE   networkSolveGlue      ← SH-gate verdict computed here (post-solve)
:1534  SB_SOLPREP     networkSolPrep        ← early-returns on sh_gate_withheld; STAMPS C1
:1545  SB_EMITSTORE   networkEmitStore      ← early-returns on sh_gate_withheld
:1585  SB_CLOCKUPD    networkClockQc        ← UNCONDITIONAL; UPDATES the rate estimator
```

The withhold early-returns live **inside** `networkSolPrep` / `networkEmitStore`. The driver
call at `ApplicationNetwork.cpp:1585` is **not guarded** — so `networkClockQc`'s rate pass
runs on **every** epoch, withheld or not.

Consequence, tracing the 1.99 h era:

- `clock_rate_est[sat].t_last` advances every 30 s **straight through the withheld era**.
- On the first emitted epoch after it, the stamp (`NetworkSolPrep.cpp:230`, which runs
  *before* that epoch's update) reads `sat_dt` = **30 s, not 1.99 h**. The R11 gap gate
  **passes**.
- C1 therefore ships a rate finite-differenced **between the last two withheld epochs** —
  i.e. across the very frame the SH gate declared out-of-band and refused to publish.

**The precise defect: R11 gates on *solve* continuity, but the consumer experiences *emission*
continuity.** With no withhold mechanism these were the same quantity, so the gate was
correct as written. The Phase-2.2 SH gate — which withholds emission while the solve, carry
and QC all continue — decouples them, and the staleness guard silently passes across a
consumer-visible 2 h gap. This is standing invariant #1 in its exact form: a time-linked
quantity whose event response does not fire on the event that actually occurred.

**It would have fired on this exact leg.** `leg01` is a **warm resume** with `n_emitted = 720`
restored (`NetworkRunContext.cpp:521`; `stdout.log` "WARM RESUME … n_emitted 720") `[M]`, and
`n_emitted` is a monotone latch that never decrements. So `emit_rate_warm`
(`NetworkSolveGlue.cpp:578-580`) would have been **true from epoch 1**: the finite difference
would have run every 30 s through all 239 withheld epochs, and a **nonzero C1 would have been
published on the first emitted epoch (tow 334770)** — `sat_dt = 30 s ≤ 900 s`, gate passed —
carrying a rate differenced from inside the era the gate had just declared unpublishable. The
hazard is not "C1 goes quiet across the gap"; it is **"a corrupt rate is published at
release."**

**Severity: latent, not live.** `network.emit_clock_rate` is OFF (default and in every current
config; `leg01/snap.txt` contains zero `O,` and zero `K,` records `[M]`, so both C1 mechanisms
are dormant and a `leg02` resume inherits no rate state). `network.clock_model` shares the same
gate at `NetworkSolPrep.cpp:209-217` and is likewise off. **No run is affected.** But the fix
is a prerequisite for arming either toggle: the card needs an **emitted-epoch** staleness clock
(a `t_last_emitted` advanced only on actual emission) alongside the solved-epoch one, and must
ZOH on the emission gap. Prefer that over gating on `sh_gate_engaged` — it also covers
`WithheldGate` and `WithheldSigma0`. Fold into §6 G0.

*Structural note.* This is **one missing guard, not a two-halves design fork.** The advance is
lexically nested inside `if (emit_rate_warm)` (`NetworkClockQc.cpp:267`) and `n_emitted` never
un-arms, so the solved-epoch and emitted-epoch keying cannot disagree post-warmup. What is
missing is an emission-continuity term on the R11 test.

---

## 6. The full validation ladder — **specified, gated on D0**

Pre-registered so that if D0 passes there is no design left to invent. **Do not spend these
runs unless D0 passes.**

**G0 — estimator replacement + the §5.3 emission-continuity guard.** Swap the N = 2 raw
difference for the D0-winning trailing window, and add the `t_last_emitted` staleness clock.
Keep the R11 gap gate, the datum-wipe reseed (`NetworkClockQc.cpp:270-280`) and the
warm-resume `O,` record. Note the lag cost: a causal N-window carries `(N−1)Δt/2` of lag
(N = 61 → 15 min), which must be charged against the gain, not ignored. If a second
snapshot record is needed, the free tags are **F, G, P, Q, T** — taken:
`{A,C,D,GG,I,J,K,L,M,N,O,R,SV,U,V,W,X,Y,Z}`; avoid B/H/S (they read as carry-blob tags).

**Cadence traps to check before arming** `[M]`: the toggle is inert when `clock_model` is on
(`NetworkRunContext.cpp:106`), and inert when
`gnss_rate_s × epoch_decimation_n > clock_model_max_gap_s`
(`GnssConfigLoadNetwork.cpp:1707-1723`) — the latter check is skipped entirely when
`gnss_rate_s = 0`, which is tonight's configuration. At 30 s vs 900 s it would not fire.

**G1 — unit.** `gnss_tests --gtest_filter=NetworkEmitClockRate.*:NetworkProductIO.*`, then
the full suite. Add a pure-helper test for the new window estimator (known ramp → exact
slope; short history → 0; gap → 0).

**G2 — OFF byte-identity** (invariant #6). Same-build golden A/B, `_emit_c1_off.json` vs the
frozen binary: `products.txt`, `network_solution.csv`, `network_trace.csv`,
`network_epoch_status.csv`, `network_events.csv` identical by SHA-256. Same build config
only — never Debug vs Release.

**G3 — ON decoupling proof.** `_emit_c1_{off,on}.json` (10-ep smoke first, per standing
cadence). Assert: every non-C1 column of `network_solution.csv` **byte-identical** across
arms — the decoupling claim — and `_on`'s C records carry a nonzero field 4.

**G4 — estimator quality (the decisive gate).** On the ON arm, re-run §3.4 *against the
emitted C1 itself* rather than an emulation: does the shipped `c₁` beat ZOH at the
D0-identified operating point, by the D0 margin? **This is the gate. G5 without G4 is
theatre.**

**G5 — consumer replay A/B.** `Application::runConsumerReplay`
(`network.closed_loop.replay_products_path`, ~33 s/arm — [[project-consumer-replay-driver]]),
arms = the G3 product streams, graded on `logs/consumer_replay_hourly.csv` position.
⚠️ Pin `processing.signal_priorities` as **explicit arrays** — attribute strings do not bind
the signal and will silently give you two arms that differ in L1 instead of in C1.

**G6 — event response** (invariant #1). Force-fire: a datum re-reference reseeds without a
spurious rate spike; a > R11 return ships C1 = 0 for one epoch with the `T5_clock_rate` event;
a warm resume restores `clock_rate_est` exactly via the `O,` record.

**G7 — run-end counters** (invariant #4). The card must report epochs stamped with nonzero
C1, ZOH suppressions, and reseeds, with an explicit WARN on the all-zero inert case. Verify
the counters are non-zero before believing any ON-path claim.

---

## 7. What not to do

- **Do not queue `HANDOFF_c1_clock_rate_producer.md` §4.5 as written.** Its premise — that a
  low-latency consumer is saw-tooth-limited by ZOH — is **falsified for this producer** at
  every horizon from 30 s to 2 h (§3.4). The saw-tooth is a random walk, and rate products
  do not fix random walks.
- **Do not smooth blind.** The handoff says *"validate raw first."* §3.4 already validated
  raw: it loses by 1.6–4.7×. Skip it; D0 selects the window empirically.
- **Do not grade C1 on Q3's wander** (§2) — it is byte-identical by construction and cannot
  move it.
- **Do not treat the withhold era as a C1 test bed** (§5.2).
- **Do not delete the card.** It is correct, decoupled, inert when off, and it is the natural
  second half of the Q3 anchor work.

---

## 8. What I could not self-verify

1. **`brdc_rts` regime `[?]`** — the one place the card's premise might survive. D0-c.
2. ~~V1: the `NetworkClockQc`-vs-withhold seam order~~ — **RESOLVED** this session by
   reading `ApplicationNetwork.cpp:1534-1585`; it is a latent defect, written up in §5.3.
   What remains unverified is only its *magnitude* if the toggle were ever armed.
3. **Guarded σ_w `[?]`** — D0-a/b; the §3.6 observation is confounded and is not evidence.
4. **Generalisation `[?]`** — one day, one constellation, one producer.
5. **Nothing here was run through a compiler.** No code was modified this session; all
   analysis is offline over existing CSV artifacts.
6. **Sub-30 s interpolation** (§3.6) — the card's stated target, unmeasurable at 30 s cadence.

---

## 9. Method note

§1, §2, §5 and the §3 measurements were established first-hand. They were then cross-checked
by a 77-agent read-only fan-out (adversarial refutation on every load-bearing claim; 38
confirmed, 33 corrected). Corrections that changed this document: the §5.2 self-contradiction
(the R11 bound does **not** cover the era — §5.3 was right and §5.2 as first written was
wrong); the warm-resume sharpening in §5.3; the "one guard, not a fork" structural framing;
and the §3.6 independent reproduction. The full dossier is at `docs/REPORT_consumer_gap_dossier.md`; the
companion consumer audit is `docs/AUDIT_consumer_product_gap.md`.

# NIGHTRUN REPORT 2026-07-15 (Opus, unattended) — RESULTS

Executes `docs/NIGHTRUN_2026-07-14.md` REV 2. All runs `network.parallel_carry: true`
except the R2 OFF control. **n_threads = 20** (config `eigen_threads: 20`; per-thread-
count reproducible, per the pcarry contract). Build: Release rebuilt clean (90/90) with
the step-0c edits @ ~22:44; runs used `gnss_processor_opus_night.exe`.

**Nothing committed.** Staged working-tree edits + generated configs are listed in §Staged.

## Headline verdicts

| Run | What | Verdict |
|-----|------|---------|
| **Step 0c** | load-time parallel_carry/parallel_reduce guard | done; ⚠ flips both-armed resolution (see §Staged) |
| **R1 (G3)** | pb-gate-v2 @300s, v1 vs v2 | **PASS** |
| **R2** | pcarry ON-vs-OFF equivalence | **PASS — equivalent (counters identical, grades exactly equal); speedup 2.13×** |
| **R3 (G4/G5)** | pb-gate-v2 @30s, v1 vs v2 | **PASS — v2 disc floor 33× below v1; k=4 transfers** |
| **R4** | 3h dev-tier D5 cold-vs-steady | steady-state + **growing** → raise tier k (recommend) |
| **R5** | golden deciders em25d15 vs em10d8 | **close/mixed** — em25d15 marginally leads dd-frac+calib; both uniform/not-AR-ready |
| **R5b** | em25d15 6h hr-4 cliff | DD-step churn **accelerates** (3.9→4.6 cyc/ep); wire stays clean |
| **R6** | M5 no-refloat-arc discriminator | **third state** — churn-dominated/not-converged; M5 prereq = DD convergence |

---

## Step 0c — parallel_carry/parallel_reduce guard (staged)

Fable lean-review finding implemented:
- `src/core/config/GnssConfigLoad.cpp` `loadNetworkSection_`: added a **load-time** guard —
  when `parallel_reduce && parallel_carry` are both set, warn + force `parallel_carry = false`
  (rationale: `est.solve()` is itself Eigen-threaded, so the two nest OpenMP × Eigen inside
  `reduce()`).
- `src/core/app/ApplicationNetwork.cpp`: replaced the now-stale runtime guard comment/warn
  (which claimed "they don't nest / carry wins the tie") with a note that the load-time guard
  pre-empts it; amended the solve-site `CarryEigenScope` comment ("the whole `est.solve()` is
  Eigen-threaded").
- `include/config/GnssConfigTypes.hpp`: header doc updated ("forces parallel_carry OFF" was
  "forces parallel_reduce OFF").

⚠ **FLAG FOR REVIEW:** this **flips** the both-armed resolution from *carry-wins* (prior
runtime guard) to *reduce-wins* (new load guard forces carry off). **Inert tonight** — no
config arms `parallel_reduce`, and every run confirmed `parallel_reduce=None`. Confirm the
flip is intended (the prior design kept carry because it is the ~2× lever; the new guard
kills carry when both are set).

Build after edits: clean, 90/90, exit 0 (ApplicationNetwork.cpp + config loader both compiled).

---

## R1 — pb-gate-v2 G3 @300s (two 11-ep smokes, pcarry ON both)

`run_night_smoke_v1` (v1: k=8/σ=0.1) vs `run_night_smoke_pbv2` (v2: gate_v2, k=4/σ=0.05).
Both 11 epochs, exit 0, ~22.8 s/ep.

| metric | v1 | v2 |
|---|---|---|
| count_disc_bumps PB/ep | 16.9 | 15.1 |
| CB bumps | 34 | 34 |
| PB filter resets | 98 (98 ref-path, 0 innov) | 100 (98 ref-path, 2 innov) |
| smoothed key-ep | 2013 | 2011 |
| σ̂_step (G) | — | **0.0809 m (8.1 cm)** |
| mean posterior σ | 0.0238 m | 0.0232 m |

**G3 = PASS:** v2 innovation resets ≈ 0 (2/2011); PB disc/ep ≈ ref-path floor (98/100 resets
ref-path), both ≪ pre-fix ~180/ep; σ̂_step 8.1 cm in the 5–10 cm band (censoring-free feed,
2013 rows); smoothed v2≈v1. (Absolute ~15–17/ep is above the ~10/ep hand-baseline, but that
baseline is a different build+threading — indicative only per the queue; the in-pair v2≈v1,
slightly lower, is the gate.)

---

## R2 — parallel_carry equivalence control (OFF) + P0b split

`run_night_smoke_off` = unmodified `_net305_3card_dev300s_gps_d8_em10_smoke.json` (pcarry
absent/false). 11 ep, exit 0.

**Equivalence vs R1-1 (ON) = PASS** (equivalent within grading precision — NOT a
solution-level byte-identity claim; the pcarry contract explicitly relaxes serial≡parallel
to ~1e-4 over a run, and quantized grades/integer counters on an 11-ep smoke cannot
distinguish ULP-class drift.  Graded verdicts still come from OFF runs per the header's
usage rule; this gate is what licenses tonight's ON A/Bs):
- All run-end counters IDENTICAL (0% delta, ≤1% required): carry 7834 DD / 16249 births /
  6290 refloats; D5 5090 flags/re-taps; deweight 27 stations; re-base 110 comp / 53433 cyc /
  1932 keys; PB 2013 smoothed / 98 reset.
- `grade_ddfrac` OFF == ON exactly (0% delta, ≤2% required): mean|f|=0.252, DD step
  13.544 cyc/ep, calib 2.10×, a00 31.1±3.1 TECU. (Smoke DD-frac is uniform/cold by design —
  equivalence check only, not a quality grade; golden grading stays 30s per tier rules.)

**pcarry speedup = OFF/ON wall = 534215/250319 ms = 2.13×** (48565 → 22756 ms/ep). Doc
claimed ~2.4×; measured 2.13× at 20 threads.

**P0b foldDdPriors split (C2 decision input, from the ON run — only the parallel_carry path
emits it):** per-station-loop **23.5%** | M-inverse **13.2%** | GMGᵀ **1.4%** of loop wall.
⇒ validates INC-C2 (factored-Pz Woodbury): the fold cost is the per-station reduction + the
O(K³) middle inverse; GMGᵀ is already negligible.

**Consequence:** equivalence is MEASURED, not assumed ⇒ every pcarry-ON A/B tonight is
trustworthy and the morning OFF-confirmation contingency is **not** needed.

---

## R3 — pb-gate-v2 G4/G5 @30s (two 60-ep runs, pcarry ON both)

`run_night_30m_v1` (v1: k=4 default) vs `run_night_30m_v2` (v2: +gate_v2). Both 60 ep,
exit 0, ~14.2 s/ep (30s tier faster: 4826 carried DD vs 7834 at 300s).

| metric | v1 | v2 |
|---|---|---|
| count_disc_bumps PB/ep | **85.5** | **2.6** |
| PB filter resets | 5772 (65 ref-path, **5707 innov**) | 97 (65 ref-path, **32 innov**) |
| smoothed key-ep | 6219 | 11894 |
| σ̂_step (G) | — | 0.2012 m |
| grade_ddfrac | n=34314, DD step 2.654, calib 0.35× | **identical to v1** |

**G4 = PASS (dramatic):** v2 disc floor 2.6/ep vs v1 85.5/ep (**33× cut**, ratio 0.031);
innovation resets 5707→32 (178×); ref-path counts equal (65=65); **no laundering** — all 29
v1 large-step (≥0.5 m) discs present in v2 (100%, 0 laundered; 4 v2-only informational);
solve **byte-identical** between arms (dd-frac exactly equal) ⇒ gate_v2 is outputs-only /
S-transparent.

**G5 = PASS:** one **k=4** transfers across cadences — 300s (G3: σ̂_step 8.1 cm, 2 innov) and
30s (G4: σ̂_step 20 cm, 32 vs v1 5707). The v1 per-cadence hand-tuning (k=8/σ=0.1 @300s) is
retired. **Morning tier-config edit staged** — see §Staged.

The v1 arm exposes exactly the death-spiral gate_v2 cures: at 30s the k·σ̂_step gate over-fires
(5707 innovation resets, 85.5 disc/ep); gate_v2's censoring-free σ̂_step + model-scaled
k·√(Ppred+R) collapses it to ref-path-only.

---

## R4 — 3h dev tier D5 cold-vs-steady (`run_night_dev3h`)

36 ep (300s tier), exit 0. PB filter healthy: 6811 smoothed / 448 reset (437 ref-path, 0
innov). Last-10-ep PB disc tail ~9–27/ep. a00 tail 43.3±1.8 TECU (diurnal rise).

**D5 = 77789 flags/re-taps = 2161/ep** — ≫ smoke 463/ep and ≫ the 200/ep cold-start bar.
Decomposition (R4 ep 1–11 are byte-identical to the smoke): ep 1–11 = 463/ep →
**ep 12–36 = 2908/ep** ⇒ the D5 re-tap rate is **steady-state and GROWING**, not cold-start.
LS-VCE σ̂₀ = **4.42×** (gate threshold k·σ̂₀ = 17.7 formal-σ), yet 2161/ep still exceed it ⇒
the stiff zero-q_z carry's formal covariance is deeply optimistic (σ-incest signature).

Cross-tier D5: smoke 300s 463/ep · R4 300s 2161/ep (growing) · R3 **30s 126/ep** (golden
cadence far calmer — the big-gap 300s tier over-stresses D5).

**Verdict:** steady-state + growing ⇒ **recommend raising `carry_invariant_innov_k` for the
300s TIER configs only** (the 30s golden cadence is unaffected). No change made tonight.

---

## R5 — golden deciders (em25d15 vs em10d8, 3h 30s, pcarry ON, gate_v2)

Both 360 ep, exit 0. `run_night_gold_em25d15` (mask25/deg15, 14.1 s/ep, 84 min) vs
`run_night_gold_em10d8` (mask10/deg8, ~27 s/ep, 162 min — low-el sats: 7824 carried DD vs
4730). iono_weighting cutoff stays 25 in both.

| metric (last 1h) | em25d15 | em10d8 | better |
|---|---|---|---|
| mean\|f\| / circR | 0.248 / 0.008 | 0.250 / 0.004 | tied (both **uniform**) |
| DD step med (cyc/ep) | **3.748** | 4.112 | em25d15 |
| calib | **0.78** | 0.63 | em25d15 (nearer 1.0) |
| formal σ (cyc) | **3.407** | 4.595 | em25d15 |
| LOS>3σ frac | 0.762 | **0.703** | em10d8 |
| a00 tail (TECU) | 45.1±4.9 | **41.3±3.2** | em10d8 |
| count_disc_bumps PB/ep | **1.2** | 2.2 | em25d15 |
| D5 re-taps/ep | **392** | 2094 | em25d15 |
| PB filter innov resets | 56/360ep | 90/360ep | both low (gate_v2 holds 3h) |

**Both arms' PB emission is clean** (gate_v2 holds over 3h: disc 1.2–2.2/ep, innov resets
≤90/360ep). **But the DD ambiguities do not converge** in either arm (mean|f|≈0.25 uniform;
DD step 3.7–4.1 cyc/ep, ~44× golden). Note the em25d15 DD step **grew** 2.654 (R3, 30 min) →
3.748 (3 h tail) — the hr-4 cliff signature (R5b probes this).

**Recommendation (close call):** tonight's stated criteria (dd-frac windows + calib nearer
1.0 + no new pathology) **marginally favor em25d15**, which does *not* confirm the prior
em10/deg-8 preference. em10d8's edges are LOS-outlier fraction and a00 stability (from its
low-el coverage), at ~2× wall cost. Since **both arms are churn-dominated / not AR-ready
(R6)**, the mask/degree choice is second-order to the DD-convergence problem — I'd keep
em10/deg-8 only if the a00/coverage robustness is the priority, else em25d15 for the cleaner
DD-frac + calib + 2× speed. **Not a decisive golden-v2 capture on dd-frac grounds** — the
arms are near-tied on the primary window.

---

## R6 — M5 no-refloat-arc discriminator (VERDICT)

Analysis-only (Python) on the Jul-14 attribution runs (3h/6h all-on, 12h GPS-only) **and**
tonight's R5 golden arms. Scripts: `r6_norefloat_arcs.py`, `r6_arcmean.py` (scratchpad).

**Finding (robust across every substrate):** the network float DD ambiguities are
**uniformly distributed with no integer information on any signal**.
- 12h GPS-only 6h-tail (1.44M samples): mean|f|=0.250, P(<0.1)=0.200, circR=0.001.
- Golden em25d15: per-signal arc-mean|frac| = 0.24–0.28 (uniform) on **every** signal
  (L1C, L2W, L2S, L5…); L1C/L2W churn so hard that even the arc-*mean* can't be pinned to
  ±0.15 cyc; the only arcs stable enough to average (L2S) land **non-integer** (~half-cycle,
  the semi-codeless artifact). Golden em10d8: even churnier (fewer stable arcs; the loose-
  threshold set is uniform, circR 0.14).
- **DD step 3.7–4.1 cyc/ep** (golden ~0.085 → 18–48× worse) ⇒ almost no no-refloat arc of
  ≥15–20 epochs survives; the arc-averaging test finds no hidden integers.

**Verdict:** the "stably integer ⇒ churn was the cause / stably non-integer ⇒ per-arc model
bias" binary resolves to a **THIRD state — churn-dominated / not-converged**. There are *no*
stably-integer arcs (churn destroys them) and the stable subset (L2S) is non-integer. So:
(1) churn is real and dominant — the Card-4/PB-filter stack has been attacking the right
thing (directionally validated); (2) **but M5 self-AR is NOT yet feasible** on the producer
output — the missing prerequisite is **DD-ambiguity convergence** (drive DD churn from
~3.7 cyc/ep + uniform toward tight integers — i.e. the iono↔ambiguity cliff and the D5
re-tap load), **not** emission continuity, which gate_v2 has already solved. This leans
toward the "per-arc model bias / not-converged" horn rather than "clean integer corrupted by
churn." Method self-validated: it detects clustering when present (stable L2S arcs show
circR=0.40 clustered at a non-integer 0.34).

Substrate caveat (logged, not hidden): the Jul-14 attribution runs are churnier than golden
(DD step 1.5–2.6 cyc/ep) and multi-GNSS/noisy-signal heavy; the golden em25d15/em10d8 arms
are the fair test and agree.

## R5b — em25d15 6h (hr-4 cliff readout)

`run_night_gold_em25d15_6h`, 720 ep, exit 0, ~15 s/ep (~3 h wall). Extends the dd-frac winner
to 6 h (both arms finished 04:16 < 05:00 → licensed; em25d15 the only time-feasible 6h arm).

**Cliff confirmed — DD-step churn grows monotonically with window recency:**

| window | whole 6h | last 3h | last 1h | last 30 min |
|---|---|---|---|---|
| DD step med (cyc/ep) | 3.888 | 4.051 | 4.309 | **4.595** |

The most-recent window is the churniest — the DD instability **accelerates** with run time (the
3h arm's last-1h was 3.748; the 6h arm's last-1h is 4.309 — worse), i.e. the hr-4 cliff
continues and deepens past 6 h. **The PB emission wire stays clean the whole 6 h** (gate_v2
holds: count_disc_bumps 1.9/ep; PB filter 144839 smoothed / 938 reset, ~107 innov = 0.15/ep) —
D5 re-taps 719/ep (grew from 392/ep @3h), a00 47.4±3.0 TECU (diurnal), LOS>3σ 0.739.

**Reinforces R6:** the DDs *diverge* (churn grows), they do not converge — so the M5
DD-convergence prerequisite is further off and the trend is adverse over the run window.

---

## Operational notes

- **Nothing crashed / nothing cut.** All 9 producer runs (R1×2, R2, R3×2, R4, R5×2, R5b)
  exited 0 with the expected epoch counts. R5b (6h) was run in full (both R5 arms finished
  before 05:00, licensing it).
- **n_threads = 20** for every run (pcarry results are reproducible per thread count).
- Grading tools used: `scripts/count_disc_bumps.py`, `scripts/grade_ddfrac.py`, and three
  session helpers left in the scratchpad (`g4_compare.py` no-laundering; `r6_norefloat_arcs.py`,
  `r6_arcmean.py` for R6) — promote to `scripts/` if you want them tracked.
- Memory updated: `project_dev300s_tier`, `project_pb_gate_v2`, `project_network_ar_producer_scope`,
  and the `MEMORY.md` hook.

## Morning to-review checklist

1. **Step 0c semantic flip** (⚠ above) — confirm forcing `parallel_carry` OFF (not
   `parallel_reduce`) when both are armed is intended; it reverses the prior carry-wins design.
2. **R4** — apply the `carry_invariant_innov_k` raise to the 300 s tier configs (recommended,
   not done).
3. **R5 golden-v2** — decide em25d15 vs em10d8 given the near-tie (report leans em25d15 on
   dd-frac+calib+speed; em10d8 on a00/LOS/coverage). Neither is AR-ready.
4. **M5** — R6 says the prerequisite is DD-ambiguity convergence (the hr-4 cliff + D5 re-tap
   load), not emission continuity. Hold WL/self-AR.
5. Review the staged edits (`git diff`) before any commit.

---

## Staged (working-tree edits + configs; NOT committed)

Source edits (step 0c): `src/core/config/GnssConfigLoad.cpp`,
`src/core/app/ApplicationNetwork.cpp`, `include/config/GnssConfigTypes.hpp`.

Generated night configs (`config/test_suite/_night_*.json`): smoke_v1, smoke_pbv2, 30m_pbv1,
30m_pbv2, dev3h, golden_em25d15_3h, golden_em10d8_3h, golden_em25d15_6h.

**G5 tier-config edit — STAGED (done):** in `_net305_3card_dev300s_gps_d8_em10_{smoke,3h}.json`
removed `emit_pb_filter_jump_k` (8.0) / `emit_pb_filter_meas_sigma_m` (0.1), added
`"emit_pb_filter_gate_v2": true`. Verified valid JSON. (The per-cadence hand-tuning is retired
now that G5 proved one k=4 transfers 30 s ↔ 300 s.)

Run dirs left in place under `build/x64-Release/run_night_*`. **Nothing committed.**

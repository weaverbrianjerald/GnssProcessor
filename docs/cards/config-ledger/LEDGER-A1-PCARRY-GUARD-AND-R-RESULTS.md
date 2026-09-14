# A1 — the load-time parallel_carry / parallel_reduce guard, and results R1–R6

**STATUS**

- **id:** A1
- **topic:** config-ledger
- **status:** closed
- **summary:** the load-time parallel_carry / parallel_reduce guard, and results R1–R6
- **dates in the text:** 2026-08-07 → 2026-08-15
- **knobs / posture:** `network.parallel_carry`, `network.epoch_decimation_n`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/CONFIG_promotion_ledger.md` — A1

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: CONFIG_promotion_ledger.md -->
## A1 Step 0c — the load-time `parallel_carry` / `parallel_reduce` guard (LANDED)

*What it does:* when both are set, warn + force `parallel_carry = false` at load, because
`est.solve()` is itself Eigen-threaded and the two would nest OpenMP × Eigen inside `reduce()`.
⚠ **It FLIPS the both-armed resolution from carry-wins (the prior runtime guard) to reduce-wins** (the
new load guard kills carry, which is the ~2× lever). Flagged for review at the time; **inert that
night** — no config armed `parallel_reduce` and every run confirmed `parallel_reduce=None`. Now
documented behaviour: see the `network.parallel_carry` interlock above
(`GnssConfigLoadNetwork.cpp:540-546`, "forcing parallel_carry OFF") and memory
`project_pcarry_reduce_mutex`. Three files touched: `src/core/config/GnssConfigLoad.cpp`
`loadNetworkSection_` (the guard); `src/core/app/ApplicationNetwork.cpp` (replaced the now-stale
runtime-guard comment/warn that claimed "they don't nest / carry wins the tie", and amended the
solve-site `CarryEigenScope` comment — "the whole `est.solve()` is Eigen-threaded");
`include/config/GnssConfigTypes.hpp` (header doc said **"forces parallel_reduce OFF"**, corrected to
**"forces parallel_carry OFF"**).

## A1 R1 — pb-gate-v2 gate G3 @300 s (two 11-ep smokes, pcarry ON both) — PASS

*What it tests:* does gate_v2 hold at the 300 s dev cadence. `run_night_smoke_v1` (v1: k=8/σ=0.1) vs
`run_night_smoke_pbv2` (v2: gate_v2, k=4/σ=0.05); both 11 epochs, exit 0, ~22.8 s/ep.

| metric | v1 | v2 |
|---|---|---|
| count_disc_bumps PB/ep | 16.9 | 15.1 |
| CB bumps | 34 | 34 |
| PB filter resets | 98 (98 ref-path, 0 innov) | 100 (98 ref-path, 2 innov) |
| smoothed key-ep | 2013 | 2011 |
| σ̂_step (G) | — | **0.0809 m (8.1 cm)** |
| mean posterior σ | 0.0238 m | 0.0232 m |

**G3 = PASS:** v2 innovation resets ≈ 0 (2/2011); PB disc/ep ≈ the ref-path floor (98/100 resets
ref-path), both ≪ the pre-fix ~180/ep; σ̂_step 8.1 cm in the 5-10 cm band (censoring-free feed, 2013
rows); smoothed v2 ≈ v1. The absolute ~15-17/ep sits above the ~10/ep hand-baseline, but that
baseline is a different build + threading — indicative only; the in-pair v2 ≈ v1 is the gate.

## A1 R2 — `parallel_carry` equivalence control (OFF) + the P0b fold split — PASS

*What it tests:* whether pcarry-ON A/Bs are trustworthy at all. `run_night_smoke_off` = unmodified
`_net305_3card_dev300s_gps_d8_em10_smoke.json` (pcarry absent/false), 11 ep, exit 0, vs R1's ON arm.

- **All run-end counters IDENTICAL** (0 % delta, ≤1 % required): carry **7834 DD / 16249 births /
  6290 refloats**; D5 **5090** flags/re-taps; deweight **27 stations**; re-base **110 comp / 53433 cyc
  / 1932 keys**; PB **2013 smoothed / 98 reset**.
- **`grade_ddfrac` OFF == ON exactly** (0 % delta, ≤2 % required): mean|f| **0.252**, DD step
  **13.544 cyc/ep**, calib **2.10×**, a00 **31.1 ± 3.1 TECU**. (Smoke DD-frac is uniform/cold by
  design — an equivalence check, not a quality grade; golden grading stays 30 s per tier rules.)
- **Speedup: OFF/ON wall = 534215 / 250319 ms = 2.13×** (48565 → 22756 ms/ep). The doc claimed ~2.4×;
  **measured 2.13× at 20 threads.**

⚠ **This is explicitly NOT a byte-identity claim.** The pcarry contract relaxes serial ≡ parallel to
~1e-4 over a run, and an 11-ep smoke's quantized grades and integer counters cannot resolve ULP-class
drift. The equivalence licence is scoped to **this config family at 20 threads** and must not be
generalised to a new config, a new build, or a different `n_threads`. What it bought: equivalence is
MEASURED, not assumed, so every pcarry-ON A/B that night is trustworthy and the morning
OFF-confirmation contingency was not needed.

**P0b — `foldDdPriors` cost split** (emitted ONLY by the parallel_carry path, from the ON run):
per-station loop **23.5 %** | M-inverse **13.2 %** | GMGᵀ **1.4 %** of loop wall. ⇒ this is the
measurement that validated **INC-C2** (factored-Pz Woodbury): the fold cost is the per-station
reduction plus the O(K³) middle inverse; GMGᵀ is already negligible.

## A1 R3 — pb-gate-v2 gates G4 / G5 @30 s (two 60-ep runs, pcarry ON both) — PASS

*What it tests:* the golden 30 s cadence, where v1's hand tuning was known to over-fire.
`run_night_30m_v1` (v1: k=4 default) vs `run_night_30m_v2` (v2: +gate_v2); both 60 ep, exit 0,
~14.2 s/ep (the 30 s tier is faster: 4826 carried DD vs 7834 at 300 s).

| metric | v1 | v2 |
|---|---|---|
| count_disc_bumps PB/ep | **85.5** | **2.6** |
| PB filter resets | 5772 (65 ref-path, **5707 innov**) | 97 (65 ref-path, **32 innov**) |
| smoothed key-ep | 6219 | 11894 |
| σ̂_step (G) | — | 0.2012 m |
| grade_ddfrac | n=34314, DD step 2.654, calib 0.35× | **identical to v1** |

**G4 = PASS (dramatic):** disc floor **2.6/ep vs 85.5/ep (33×, ratio 0.031)**; innovation resets
**5707 → 32 (178×)**; ref-path counts EQUAL (65 = 65); **no laundering** — all 29 v1 large-step
(≥0.5 m) discs present in v2 (100 %, 0 laundered, 4 v2-only informational); **solve BYTE-IDENTICAL
between arms (dd-frac exactly equal) ⇒ gate_v2 is outputs-only / S-transparent.** The v1 arm is the
death spiral gate_v2 cures: at 30 s the k·σ̂_step gate over-fires; gate_v2's censoring-free σ̂_step +
model-scaled k·√(Ppred+R) collapses it to ref-path-only.
**G5 = PASS:** one **k = 4** transfers across cadences — 8.1 cm @300 s (G3) and 20 cm @30 s (G4). The
v1 per-cadence hand tuning (k=8 / σ=0.1 @300 s) is RETIRED. **The G5 tier-config edit as executed:**
in `_net305_3card_dev300s_gps_d8_em10_{smoke,3h}.json`, removed `emit_pb_filter_jump_k` (8.0) and
`emit_pb_filter_meas_sigma_m` (0.1), added `"emit_pb_filter_gate_v2": true` (valid JSON verified).

## A1 R4 — the 300 s dev tier's D5 load, cold vs steady (`run_night_dev3h`)

*What it tests:* whether the dev tier's D5 re-tap rate is a cold-start artefact. 36 ep (300 s tier),
exit 0. PB filter healthy: 6811 smoothed / 448 reset (437 ref-path, 0 innov); last-10-ep PB disc tail
~9-27/ep; a00 tail 43.3 ± 1.8 TECU (diurnal rise).

**D5 = 77789 flags/re-taps = 2161/ep.** Decomposition (R4 ep 1-11 are byte-identical to the smoke):
ep 1-11 = **463/ep** → ep 12-36 = **2908/ep** ⇒ the rate is **steady-state AND GROWING**, not
cold-start. LS-VCE **σ̂₀ = 4.42×**, i.e. a gate threshold k·σ̂₀ = **17.7 formal-σ** that 2161/ep still
exceed ⇒ the stiff zero-q_z carry's formal covariance is deeply optimistic (the σ-incest signature).
Cross-tier: 300 s smoke **463/ep** · 300 s 3 h **2161/ep (growing)** · **30 s golden 126/ep** — the
golden cadence is far calmer; the big-gap 300 s tier over-stresses D5. Recommendation at the time:
raise `carry_invariant_innov_k` **for the 300 s TIER ONLY**; no change was made that night. ⚠ **That
remedy was subsequently TESTED AND FALSIFIED** — k=8 gave MORE D5 flags, **2616/ep vs 2161** (see the
`network.epoch_decimation_n` entry above).

## A1 R5 — golden deciders, em25d15 vs em10d8 (3 h, 30 s, pcarry ON, gate_v2)

*What it tests:* which mask/degree shape captures the golden config. Both 360 ep, exit 0.
`run_night_gold_em25d15` (mask 25 / deg 15, 14.1 s/ep, 84 min) vs `run_night_gold_em10d8` (mask 10 /
deg 8, ~27 s/ep, 162 min — low-el sats: 7824 carried DD vs 4730). iono_weighting cutoff stays 25 in
both.

| metric (last 1 h) | em25d15 | em10d8 | better |
|---|---|---|---|
| mean\|f\| / circR | 0.248 / 0.008 | 0.250 / 0.004 | tied (both **uniform**) |
| DD step med (cyc/ep) | **3.748** | 4.112 | em25d15 |
| calib | **0.78** | 0.63 | em25d15 (nearer 1.0) |
| formal σ (cyc) | **3.407** | 4.595 | em25d15 |
| LOS>3σ frac | 0.762 | **0.703** | em10d8 |
| a00 tail (TECU) | 45.1 ± 4.9 | **41.3 ± 3.2** | em10d8 |
| count_disc_bumps PB/ep | **1.2** | 2.2 | em25d15 |
| D5 re-taps/ep | **392** | 2094 | em25d15 |
| PB filter innov resets | 56 / 360 ep | 90 / 360 ep | both low (gate_v2 holds 3 h) |

**Verdict: close/mixed.** Both arms' PB emission is clean, but the DD ambiguities do not converge in
either (mean|f| ≈ 0.25 uniform; DD step 3.7-4.1 cyc/ep, ~44× golden), and em25d15's DD step GREW
2.654 (R3, 30 min) → 3.748 (3 h tail). The stated criteria marginally favour em25d15 on dd-frac +
calib + 2× speed and **do NOT confirm the prior em10/deg-8 preference**; em10d8's edges are
LOS-outlier fraction and a00 stability (from its low-el coverage), at ~2× wall. **Second-order either
way** — both arms are churn-dominated / not AR-ready (R6), so the mask/degree choice sits below the
DD-convergence problem.

## A1 R5b — em25d15 at 6 h: the hr-4 cliff readout

`run_night_gold_em25d15_6h`, 720 ep, exit 0, ~15 s/ep (~3 h wall). *Licensing condition for running a
6 h arm at all: both R5 arms finished 04:16 < 05:00 → licensed, and em25d15 was the only
time-feasible 6 h arm.*

| window | whole 6 h | last 3 h | last 1 h | last 30 min |
|---|---|---|---|---|
| DD step med (cyc/ep) | 3.888 | 4.051 | 4.309 | **4.595** |

**Churn ACCELERATES with run time** — the 3 h arm's last-1h was 3.748, the 6 h arm's is 4.309, so the
hr-4 cliff continues and deepens past 6 h. **The PB emission wire stays clean the whole 6 h**
(gate_v2 holds: count_disc_bumps 1.9/ep; 144839 smoothed / 938 reset, ~107 innov = 0.15/ep). D5
re-taps **719/ep** (from 392/ep at 3 h); a00 47.4 ± 3.0 TECU; LOS>3σ 0.739. Reinforces R6: the DDs
*diverge*, they do not converge, and the trend is adverse over the run window.

## A1 R6 — the M5 no-refloat-arc discriminator (VERDICT)

*What it tests:* the pre-registered binary "stably integer ⇒ churn was the cause / stably non-integer
⇒ per-arc model bias". Analysis-only (Python) over the Jul-14 attribution runs (3 h/6 h all-on, 12 h
GPS-only) **and** the R5 golden arms. **It resolves to a THIRD STATE — churn-dominated /
not-converged.** The network float DD ambiguities are uniformly distributed with **no integer
information on any signal**:

- 12 h GPS-only, 6 h tail, **1.44 M samples**: mean|f| **0.250**, P(<0.1) **0.200**, circR **0.001**.
- Golden em25d15: per-signal arc-mean|frac| **0.24–0.28 uniform on EVERY signal** (L1C, L2W, L2S, L5).
  L1C/L2W churn so hard the arc-MEAN cannot be pinned to ±0.15 cyc; the only arcs stable enough to
  average (L2S) land **NON-integer** at ~half-cycle (the semi-codeless artefact). Golden em10d8 is
  churnier still (fewer stable arcs; the loose-threshold set is uniform, circR 0.14).
- DD step 3.7-4.1 cyc/ep (golden ~0.085 ⇒ 18-48× worse) ⇒ almost no no-refloat arc of ≥15-20 epochs
  survives; the arc-averaging test finds no hidden integers.

**Method self-validated:** it DOES detect clustering when present — stable L2S arcs show circR
**0.40** clustered at a non-integer **0.34**. **Consequence:** churn is real and dominant (the Card-4
/ PB-filter stack has been attacking the right thing, directionally validated), **but M5 self-AR is
NOT yet feasible on the producer output — the missing prerequisite is DD-ambiguity CONVERGENCE, not
emission continuity**, which gate_v2 has already solved. This leans toward the "per-arc model bias /
not-converged" horn. Substrate caveat logged, not hidden: the Jul-14 attribution runs are churnier
(DD step 1.5-2.6 cyc/ep) and multi-GNSS/noisy-signal heavy; the golden arms are the fair test and
agree. R6's negative was later re-established on a *working* fold at 6 h by
`docs/RUNQUEUE_pdfix_reask.md` P6(b) (med|frac| flat 0.246-0.254 over six hours), and M5 itself is
closed **DO NOT BUILD** (memory `project_network_ar_producer_scope`).

---

# Appendix A2 — Campaign record: overnight 2026-08-07/08 (Fable)

*Absorbed 2026-08-15 from `docs/SESSION_overnight_2026-08-07.md` (now `docs/attic/`). Cite as
`CONFIG_promotion_ledger.md §A2 F<n>`; the `F0`…`F16` and `Q4` labels are the originals, preserved
verbatim. The source file's F-numbering was chronological (F11 landed last); findings are ordered
numerically here.*

⚠ **Run-path provenance: every `runs/<name>` below resolves under
`D:\GnssProcessor_archive\2026-08-12\runs\`, not the working tree.** **Binaries:** producer A/Bs all
on Release `gnss_processor_s2h.exe` md5 **82B4450703C551109DCEFB0B6C212559**; the Q4 replays stayed on
`gnss_processor_edge2.exe` (`5E749970…`), the original pair's binary. Charter constraints honoured:
≤ ~10 min wall per run (63 sta → ≤ ~1 h data; 305 sta → ≤ ~15 min data), Release-only on a renamed
hashed exe, runs strictly serialized (shared spdlog dir), pair on build AND resolved config chain, new
work default-OFF byte-identical. **The staged run matrix itself was F5**: batch A = 4× Q4 power-fix
replays (MAJU/WIND, marks/nomarks, edge2 bin) + the H-rider replay byte-identity gate
(`_ext63_replay_1h_pl_marks_s2hbin`) + the OFF-path byte-identity anchor (`_ext63_1h_evt_off_s2hbin`)
+ the three 20-ep smokes; batch B (gated on A) = 3 h control + 1 h S2H stability arm; batch C (gated
on B's 1 h gates) = 3 h S2H arm; 12 h verdicts were DEFERRED at the time per the run budget and ran
later as F12.


# HANDOFF — Supply network ionosphere to the closed-loop consumer (defect-2 root-cause fix)

*Created 2026-07-09. Kickoff for a fresh session. Companion: `docs/ssystem_theory_reference_2026-07-09.md`
(the S-system theory map) and memory `project_ssystem_theory_reference` (the full investigation trail).*

---

## Why (the root cause, established this session)

A deep S-system-theory pass on the open **consumer vertical bug (defect 2)** — the 305-station
closed-loop consumer (ZIM200CHE) float solution diverging vertically with **0 integer fixes** —
**systematically eliminated** the datum/covariance/clock family of cures, each with theory + a
rapid empirical check:

- **Datum choice** (s2z / bca / signal-hold) → ruled out by the datum-invariance theorem (estimable
  functions are invariant in value *and* variance under any admissible S-basis).
- **Product-covariance carry** (Lever B / Tienstra) → structurally mismatched: defect 2 is *temporal*;
  the consumer's per-arc float ambiguity absorbs the per-epoch phase-bias σ (TK-2015 Eq 88). Measured:
  phase-bias σ ≈ 1–3 m, dominated by the ambiguity-absorbed level.
- **Decoupled phase clock** (Lever C / DC-IRC-DSC) → moot: the phase is *already* implicitly decoupled.
  The position-driving phase feels only ~9.5 cm within-arc (vs the 0.37 m raw-clock metric) because
  the phase bias absorbs the clock's code-noise wander within each arc.

They all failed because they were treating a **float solution that structurally cannot fix
ambiguities.** The reason it can't: **the consumer is never fed the ionosphere.**

- The closed-loop consumer consumes **clock + biases only** —
  `consumer->setSsrStore(&network_store, cl.consume_network_clock, /*bias=*/true)`
  (`src/core/app/ApplicationNetwork.cpp:1198`). The `closed_loop` config block has **no iono field**
  (keys: `enable, station, consume_network_products, consume_network_clock, consumer_undifferenced,
  ar_mode, ar_wl_only`). There is **no iono emit→consume path** anywhere (no `SsrVtec` / `pushVtec` /
  `getVtec` / `consumeNetworkIono`; `network_vtec_sh.csv` is trace-only).
- So the consumer estimates its **own slant iono** as a loose random walk (`filter.process_noise.
  ionosphere_q = 0.001`). That is the **iono-float model**, which Odijk (2012) and Teunissen–
  Khodabandeh (2015) name *"the bottleneck for fast AR."* TK-2015 Eq 73 (BLUP): feed the user the
  network iono with a distance-scaled prior and model strength climbs toward an ionosphere-fixed
  short baseline; withhold it and you need long spans to fix.
- The network *does* estimate a full SH-VTEC ionosphere (`estimation.global_vtec_sh`, degree 15,
  layer 450 km, sun-fixed) — it keeps the iono sharp, which is the entire point of the uncombined
  model, and then discards the advantage at the consumer.

**Conclusion:** defect 2 is downstream of 0 fixes, which is downstream of no-iono-supply.
(Code biases are *not* the bug — they are estimated per-signal, up to 10/Galileo sat, ≥2-station
gated at `ApplicationNetwork.cpp:3554`, datum'd correctly with a per-sat IF-pin + per-(const,signal)
sum-to-zero at `ApplicationNetwork.cpp:4099`; over-parameterized, contributing to a 0.92 m code-clock
wander, but not the root cause.)

## Goal

Wire an **in-process ionosphere supply** from the network producer to the closed-loop consumer,
turning the consumer from iono-float into iono-corrected (strong model), and test whether the
consumer goes from **0 fixes to > 0 fixes.**

Feasible without the blocked dense-data *file* pipeline (see memory `project_network_atmosphere_readiness`)
because the closed loop is in-process and the SH-VTEC coefficients are live in the estimator
(`est.vtecCoeffColumn(n,m,trig)`).

## Design

1. **Producer side (evaluate SH-VTEC at the consumer's geometry).** After the network solve, for each
   satellite the consumer tracks, compute the ionospheric piercing point at the SH-VTEC layer height
   (450 km), evaluate vertical TEC from the SH coefficients (sun-fixed frame), map to slant via the
   obliquity factor, convert to metres on L1. **Reuse the producer's own SH-VTEC→slant machinery** —
   the network already does exactly this for its stations (find `F_ion` / the obliquity mapping
   `M_r = diag[f^s_r]` and the SH evaluation in the ionosphere model). Emit the per-(sat) slant iono
   **undifferenced** (Odijk Table 11: a vertical→slant mapping intervenes, so it must NOT be between-
   satellite differenced).
2. **Consumer side (apply + tight prior = BLUP / ionosphere-weighted).** Instead of floating the
   consumer slant iono with `ionosphere_q`, constrain each per-(sat) slant iono state to the network
   prediction with a **tight, distance/quality-scaled prior** (the residual iono prediction error).
   Mirror the producer's `estimation.ionosphere_weighting` knobs (`common_mode_sigma_m=50`,
   `floor_m=0.05`, `std_scale`). This is TK-2015 Eq 73 in practice.
3. **Toggle.** `closed_loop.consume_network_iono` (or `network.consume_network_iono`), **default false,
   byte-identical when off.** Follow the existing config-toggle pattern (getter + member + `jval`
   parse; see `network.consume_ura` / the B1b `network.consume_phase_bias_sigma` added this session).

**Integer-preservation note (Odijk 2016 §6.5–6.6, Eqs 44–45):** matching the network/user iono
parameterization is the clean way to keep user ambiguities integer; we already supply DCBs (the
alternative fix), so adding iono supply is double insurance and directly strengthens AR.

## Key code locations

- Closed-loop consumer wiring: `src/core/app/ApplicationNetwork.cpp` — cl config `519–531`; consumer
  setup + `setSsrStore` `1167–1219` (the iono-consume hook goes near `:1198`); per-epoch
  `processEpoch` `~6146`; AR/fix verdict + `sum_num_fixed` summary `~7854+`.
- Producer SH-VTEC: `est.vtecCoeffColumn(n,m,trig)` / `SharedParamKind::VtecSh`
  (`NetworkEstimator.hpp/.cpp`); SH registration + layout `ApplicationNetwork.cpp:3576+`; the SH
  evaluation / obliquity mapping in the ionosphere model (`include/atmosphere/Ionosphere.hpp` +
  the network `F_ion` assembly — grep `vtec`, `obliquity`, `piercing`, `f^s_r`).
- Consumer iono state + measurement: `include/measurement/MeasurementModel.hpp` + `.cpp` (the iono
  term in `buildJacobianRow`, ~`:1419`+); `StateManager` per-sat iono state + prior;
  `filter.process_noise.ionosphere_q`.
- Correction transport (if a store channel is wanted rather than direct in-process handoff):
  `include/products/SsrCorrectionStore.hpp` — mirror the `pushUra`/`getUra` pattern (`:104`, `:254`)
  with a per-(sat) slant-iono channel. For a first cut, a direct in-process handoff (the consumer
  reads the live SH-VTEC coeffs / a per-sat slant-iono map the producer fills each epoch) is simplest.

## Test harness (the decisive metric = fixes)

- **Config:** branch from the canonical baseline `runs/net_matrix_2024-129/configs/net305_val30min_stable.json`
  (305-sta, GPS+Gal, em25, SH-VTEC, ZIM200CHE consumer, 30 min). For rapid iteration use a 50-station
  10–30-epoch variant (see `net50_rapid_biassigma.json` for the slicing recipe: first 50 stations +
  `max_duration_s`). Note AR needs enough epochs to develop — a 10-epoch smoke confirms mechanics; use
  ≥ a few hundred epochs (or the full 30 min) to see fixes.
- **Build (Release — Debug is 5–10× slower, a 305-sta run would blow past the ~1.5 h crash horizon):**
  ```
  $vcvars="C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
  cmd /c "`"$vcvars`" >nul && cmake --build C:\GnssProcessor\build\x64-Release --target gnss_processor 2>&1"
  ```
- **Run:** `cd <rundir> && "C:/GnssProcessor/build/x64-Release/gnss_processor.exe" "<abs config>" > run.log 2>&1`
  — writes `logs/closed_loop_trace.csv` + `logs/network_solution.csv` (log_dir is `cwd/logs`).
- **Grade:** `python scripts/grade_codeloose.py <rundir>`. **Watch `sum_num_fixed` (the `num_fixed`
  column of `closed_loop_trace.csv`) — currently 0.** Secondary: `up_median` / `up_std` (defect-2
  vertical). Baseline to beat: stable = medU −1.13 m, 0/55 fixes, clock wander 0.472 m, churn 8672.
- **Byte-identity gate (toggle OFF):** `gnss_tests.exe` must stay **590 pass / 1 known skip**.

## Conventions (CLAUDE.md + memory)

- Default-OFF, byte-identical-when-off; explicit CMake source list (add new `.cpp` manually); heavy
  Doxygen on public API; config toggles in `processing`/`estimation`/`network` sections.
- Non-negotiable (`feedback_network_nonnegotiable_configs`): SH-VTEC always on; zero-q requires a slip
  detector; never grade quality on stripped configs.
- Prefer the correct structural fix over an expedient one (`feedback_correctness_over_expedient`).

## First steps

1. Recall memory `project_ssystem_theory_reference` and skim `docs/ssystem_theory_reference_2026-07-09.md`
   §4 (what the network transmits) + §7 (iono must be undifferenced per-sat; integer preservation).
2. Read the producer's SH-VTEC evaluation/obliquity code and the closed-loop consumer setup
   (`ApplicationNetwork.cpp:1167–1219`) to find the exact seam for the iono handoff.
3. Implement producer→consumer slant-iono handoff + consumer tight-prior application behind
   `consume_network_iono` (default OFF). Build Release, confirm `gnss_tests` 590/591.
4. Run the harness; **check `sum_num_fixed` first.** 0 → still starved (look at model strength /
   phase-bias stability); > 0 → root cause confirmed, then measure `up_median` improvement.

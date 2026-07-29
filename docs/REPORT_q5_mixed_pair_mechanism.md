# REPORT — Q5 mixed-pair mechanism test: §1.5 [H] falsified, ratchet relocated

**Lineage:** `docs/REPORT_day129_frame_guards_phase01.md` §1.5 → this.
Executed 2026-07-27.  Tags: **[M]** measured here; **[H]** hypothesis.
All runs are single-station consumer replays of the DOY-129 reference product
stream (`runs/day129_cont/seg01/products.txt`) through
`Application::runConsumerReplay` — consumer NRMG00NCL, float, BSSD, the §1.5
protocol unchanged except where stated.  **No producer run; no producer
artifact touched.**

**Truth + banding (updated Jul-28):** every hourly figure below is from a
**re-run on the ITRF2020 / IGS20 monument coordinate** (−5743538.2169,
1380503.7952, −2397895.8332 @ 2024:129, σ ≈ 2 mm) with the hour-banding
boundary defect fixed — `runs/q5/itrf/`.  The original runs were graded against
the RINEX header `APPROX POSITION XYZ`, a ~2012-epoch value stale by 0.632 m of
Australian-plate motion, and banded 121/119/120 instead of 120/120/120.  See
`docs/REPORT_day129_frame_guards_phase01.md` §1.0 for the derivation, the fix,
and the validation.  Combined effect: every median moves by ≤ +0.6 m.  **It
changes no conclusion in this report** — the whole argument is made from
*contrasts between arms*, where a common constant cancels exactly.  The
original figures are kept in parentheses so the two can be matched against the
artifacts on disk.

## 0. Headline

1. **§1.5's mechanism [H] — "the consumer's receiver code-bias state must
   absorb the between-channel differential, and at `code_bias_q = 1e-4` it
   tracks slowly" — is FALSIFIED, on state-existence grounds** [M].  A 10⁴×
   sweep of `code_bias_q` (1e-4 / 1e-2 / 1e0) on the explicit-array mixed arm
   produced **bit-identical hourly tables to 4 decimals** across all three
   arms: the knob touches nothing live.  The receiver code-bias state family
   does not exist in this consumer configuration (§2).
2. **The mixed-pair penalty is a consumer-side STIFF-STATE RATCHET** [M for
   the family; H for the algebra].  Giving the filter either fast-forgetting
   channel — near-white per-sat iono (`ionosphere_q` 0.001→100) or soft
   ambiguities (`ambiguity_q` 1e-12→1e-6) — kills the monotone 64→114 m
   fault era outright (hours 16–17 return to the ~7–11 m quiet floor) while
   the hour-15 transient survives at ~22–25 m.  The between-frame
   differential is not mis-tracked by a bias state; it is **integrated by the
   near-static states** (per-band float ambiguities q = 1e-12, per-sat iono
   q = 1e-3) into a persistent position/vertical fault — the consumer-side
   analogue of the producer's Q2 pivot-CB ratchet, standing invariant #1 one
   level down the chain.
3. **The explicit-array mixed arm reproduces the accidental §1.5 arm to the
   decimal** [M] — hours 15/16/17 median 3D 63.984 / 80.399 / 113.930 m
   (original 63.397 / 80.167 / 113.663), **bit-for-bit identical to the §1.5
   arm in every hourly column**; h17 mean vertical −79.81 (original −79.844).
   The arm definition equivalence (attribute-string vs explicit array),
   verified for arm A in §1.1, now holds for the mixed arm too.
4. **A replay arm costs ~33 s wall, not ~35 min** [M] — all five arms of this
   card ran in under 4 minutes total (2160 epochs each, exit 0, full-day
   grading).  The phase-0 "~35 min per arm" figure is stale; budget replay
   studies accordingly.

## 1. Arms [M]

Generator: `scripts/q5_make_replay_arm.py` — new arm `M` =
`["C1C","L1C","C2W","L2W"]` (the §1.5 mixed pair, deliberate) and a new
`--code-bias-q` override.  Realized signals verified from
`measurement_residuals.csv` before grading (the count-first canonical trap):
GPS uses exactly C1C, C2W, L1C, L2W in every arm.

| arm | knob changed (vs leg inheritance) | hours 15/16/17 median 3D [m] | *(original)* |
|---|---|---|---|
| `armM_q1em4` | none (`code_bias_q` = 1e-4 baseline) | 63.98 / 80.40 / 113.93 | *(63.40 / 80.17 / 113.66)* |
| `armM_q1em2` | `code_bias_q` = 1e-2 | **identical to 4 decimals** | |
| `armM_q1e0`  | `code_bias_q` = 1.0  | **identical to 4 decimals** | |
| `armM_ionoq100` | `ionosphere_q` 0.001 → 100 | 24.81 / **10.53** / **9.87** | *(24.27 / 9.86 / 9.29)* |
| `armM_ambq1em6` | `ambiguity_q` 1e-12 → 1e-6 | 22.12 / **7.65** / **7.39** | *(21.89 / 7.30 / 6.83)* |

The `code_bias_q` bit-identity survives the re-run, as it must — the truth is a
grading anchor, not an engine input, and the banding change is common to every
arm.  (Re-verified on the new artifacts, not assumed.)

Quiet hours (2–12) sit at 4–11 m in every arm — the probes do not buy their
fault-era improvement with a degraded baseline.  One structured exception:
the `ambq1em6` arm degrades in hours 13–14 (18.1 / 20.4 m vs baseline
9.5 / 8.4) — soft ambiguities pass more of the 19:21:30 / 20:47:00 excursion
epochs' code noise through to position.  Diagnostic arms, not operating
points.

## 2. Why `code_bias_q` is inert — the state does not exist

The bit-identity is the empirical proof (any live state consuming
`process_noise_.code_bias` would have altered every epoch's gain under a
10⁴× q change).  The code chain explains it:

- The knob arrives: `deriveClosedLoopConsumerConfig` starts from a full copy
  of the arm's config (`GnssConfig c = *this`,
  `GnssConfigLoadNetwork.cpp:3520-3521`) and no consumer coupling touches
  process noise — so the consumer KF really ran with 1e-4 / 1e-2 / 1e0.
- Where it would apply: `StateManager.cpp:701-704` adds `pn.code_bias·dt` to
  every entry of `code_bias_indices_` — **when that map is non-empty**.
- Why it is empty here: receiver code-bias states (`<sys>_DCB` /
  `<sys>_IFB<band>`) are created only when the observation's system has
  clock-reference signals from a **.BIA product**
  (`StateInitializer.cpp:134-136` — `getClockRefSignals()` miss ⇒ early
  return, no state key ⇒ `MeasurementModel.cpp:864` never adds the state).
  The consumer coupling deliberately runs with the .BIA **off** so the
  network OSB is the sole code-bias source
  (`GnssConfigLoadNetwork.cpp:3467-3470`); and with the network clock
  consumed it also forces `estimate_sat_biases_ = false` (`:3478`), so there
  is no residual per-(sat,signal) satellite-bias state either.
- Independently, BSSD makes a per-band receiver-common code bias cancel
  between same-signal satellite pairs — the state would be unobservable even
  if created.

So the §1.5 consumer's absorbing-state inventory is: position, troposphere,
per-sat slant iono (q = 1e-3 m²/s), per-(sat,band) float ambiguities
(q = 1e-12).  **No bias state of any kind stands between the mixed-frame
OSB differential and the geometry.**

## 3. The refined mechanism [H′]

The producer's frame guarantees are per-frame: IF(C1C,C2L) is pinned per
satellite; each (constellation,signal) channel is sum-to-zero-disciplined.
A consumer forming IF(C1C,C2W) inherits the **difference of two frames'
per-satellite wander** — a satellite-specific, slowly-varying,
dispersively-weighted pseudorange error with no dedicated absorber (§2).
The float filter distributes it across per-sat iono, per-band ambiguities
and position; the near-static members of that set (**ambiguities at
q = 1e-12, iono at q = 1e-3**) integrate it and cannot release it — the
monotone growth to end-of-day that §1.5 measured, where the
correction-domain fault itself was *decaying* (τ = 3.79 h).  Both probes
open a release valve, and the persistent damage collapses to a ~1-hour
transient in both [M].

What the probes do NOT separate: the iono-vs-ambiguity split (the two
states are coupled through the uncombined model; each probe perturbs the
whole equilibrium).  The family-level claim — stiff-state integration, not
bias-state mis-tracking — is what the three-way evidence (inert q sweep ×
two effective release valves) establishes.

## 4. Consequences

- **Consumer-side q tuning is NOT the mitigation.**  The effective probes
  are diagnostic settings that trade away iono estimation / phase precision
  (visible in `ambq1em6`'s hours 13–14).  The §1.5 practical consequence
  stands and sharpens: **stay inside one frame** — which requires the
  product to declare its frame: the `cbpin=` S-contract field
  (`docs/SPEC_product_cbpin_contract.md`, format layer landed 2026-07-27).
- **Standing invariant #1 gains a consumer-side instance:** a near-static
  state with no event response converts a persistent input bias into a
  ratcheting solution fault.  The producer fix class (event-responsive
  reinit) has a consumer analogue if mixed-frame operation must ever be
  tolerated — but declaring/checking the frame is structurally cheaper than
  guarding the ratchet.
- The frame-guards report §1.5 should be annotated: mechanism [H] falsified;
  replaced by the stiff-state ratchet [H′] (this report).

## 5. Reproduce

```bash
python C:\GnssProcessor\scripts\q5_make_replay_arm.py --arm M --outdir runs/q5/armM_q1em2 --code-bias-q 1e-2
```
then run `build/x64-Release/gnss_processor_fable_run.exe config.json` with
working directory `runs/q5/<arm>`.  Probe arms post-edit
`filter.process_noise.{ionosphere_q, ambiguity_q}` in the written
`config.json` (`runs/q5/armM_ionoq100`, `runs/q5/armM_ambq1em6`).
Artifacts per arm: `logs/consumer_replay_hourly.csv`,
`logs/closed_loop_trace.csv` (2161 rows), `logs/measurement_residuals.csv`,
`run_console.log`.

The tables above are the ITRF2020-anchored re-runs in `runs/q5/itrf/<arm>/`
(same configs, only `position_ecef` replaced; ~29 s each).  Arms generated after
Jul-28 carry the ITRF2020 truth already.  To re-grade an existing arm onto
another truth without re-running:

```bash
python C:\GnssProcessor\scripts\q5_reanchor_hourly.py runs/q5/armM_* --selfcheck
```

The tool reads each arm's own configured truth, so mixing pre- and post-upgrade
arms is safe.

## 6. What I would not claim from this

- Single realization, one station, float consumer, GPS-only pairs — same
  §1.5 caveats.  (The truth-coordinate caveat is now retired: the anchor is
  the ITRF2020 monument value, σ ≈ 2 mm.)
- The iono/ambiguity split within the stiff-state family is not resolved;
  neither is the exact aliasing algebra (μ-weighted iono share vs per-band
  ambiguity share).  Separating them needs arms that decouple the two (e.g.
  iono-weighted-constraint variants), not just q sweeps.
- Nothing here grades AR behavior under mixed-frame faults (`ar_mode 0`
  throughout).
- The probes' quiet-hour equivalence is a fortunate property of this data
  day, not a general license to run white iono or soft ambiguities.

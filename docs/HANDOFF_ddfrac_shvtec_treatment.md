# HANDOFF — DD-fraction root-cause treatment + audit punch list (2026-07-09)

*Implementation cards for Opus sessions, derived from the Jul-9 audit. Source of truth:
memories `project_network_ddfrac_rootcause_shvtec` (root cause + sequence) and
`project_opus_ssystem_audit_jul9` (component scorecard); theory map
`docs/ssystem_theory_reference_2026-07-09.md`. Execute cards IN ORDER within each tier;
one card per session unless marked as a pair. Every card: default-OFF toggle unless
stated, byte-identical when OFF, suite stays 592 pass / 1 skip, build via the vcvars64
pattern (CLAUDE.md). Re-grep every cited line number before editing — they drift.*

## Context in three sentences

The network DD float ambiguities carry ZERO integer information (|frac| uniform, floats
~63 cycles off, drifting) because the co-estimated SH-VTEC field is unphysical
(a00 +296→−91 TECU) and its per-(station,sat) misfit is squeezed dispersively into the
ambiguities (μ_j band-ratio signature proven: E L5Q/L6C/L7Q = 1.789/1.504/1.709 vs theory
1.793/1.518/1.703). Formal σ collapses anyway because the SH pseudo-obs is re-applied as
WHITE every epoch and the zero-q leveling carry re-injects marginals as independent
priors. Consumer AR has never actually run (σ-gate 0.20 m cap fires first), so no fix may
be graded on `num_fixed` until the definitive experiment (T4) is possible.

---

## TIER 1 — root-cause instrumentation + brakes [OPUS-READY]

**T1a. SH-field health trace + divergence probes.** Add to the existing
`estimation.global_vtec_sh.trace` output (network_vtec_sh.csv): per-epoch a00,
coefficient RMS by degree, ratio-to-Kaula-prior, mean/max |slant-iono innovation| per
station, and count of stations whose iono residual exceeds 3σ. Add a startup+per-epoch
WARN when |a00| leaves a configurable physical band (default 2–150 TECU). Goal: make the
divergence visible and attributable (suspects: GF code-bias lump structure a smooth SH
surface cannot represent; weakly-observed a00). Pure logging, no state change.
Validate: byte-identical solution CSVs, trace present, WARN fires on the 305 config.

**T1b. Temporal treatment of the SH constraint (the white-noise fix).** New toggle
`estimation.global_vtec_sh.row_decimation_epochs` (default 0 = off = current behavior):
apply the per-station SH pseudo-obs rows only every N epochs (they are quasi-static in
the sun-fixed frame; re-applying white every epoch is the σ-incest channel — see
ApplicationNetwork.cpp:2196-2291, re-grep `appendShIonoRows`). Alternative accepted if
cleaner: inflate the SH row σ by sqrt(epochs-since-last-application). Validate: 50-sta
1h A/B — formal DD σ (after T2a exists) grows toward empirical scatter; solution stable.

**T1c. Arm the lock-in brakes (queue card B4, sized).** Ship nonzero floors in the ARMED
validation configs (not defaults): `phase_leveling_var_floor_m2` ≈ (0.05·λ)² ≈ 1e-4,
shared-carry PB/CB floors cm²-scale, per the B4 card in
`project_network_enhancement_queue`. One-line config edits + a regression run. Validate:
1h run — carried-ambiguity σ bounded below by floor; no churn explosion.

**T1d. Carry warmup gated on field convergence.** New toggle
`network.phase_leveling_warmup_on_convergence` (default false = current 6-epoch count):
the leveling carry only turns warm when the SH field is stationary (e.g. epoch-over-epoch
coefficient RMS delta < threshold AND a00 in the physical band from T1a) — today it locks
the wrong iono/ambiguity split at epoch 6 while a00 is ~296 TECU. Include a one-shot
"refloat all carried ambiguities" at the convergence event as a config alternative
(`..._refloat_at_convergence`). Validate: 50-sta 1h A/B — post-convergence DD fractions
(T2a meter) must leave the uniform regime (P(|f|<0.1) ≫ 0.2) if the theory holds; if they
do NOT, that is a decisive negative worth reporting, not a failure.

## TIER 2 — measurement + diagnostics [OPUS-READY]

**T2a. Fix the B5 DD-fraction meter (pair with T2b).** In the `trace_dd_fractional`
tracer (ApplicationNetwork.cpp ~6563-6649): (1) add a `dd_sigma_cyc` column from
`jointConditionalCovariance` propagated through both differencing steps (the joint call
already includes cross-station border covariance — verified); (2) make
`scripts/view_network_dd_fractional.py` (or the grader) print the full |frac| histogram,
the uniform benchmark (mean|frac| 0.25 / P(<0.1) 0.20), circular resultant R, and
per-series time-stability (std of dd_cyc per (sat,pair,signal)); never report mean|frac|
alone. Validate: run on runs/net305_10ep_audit CSV — must reproduce mean|frac|=0.247,
P(<0.1)=0.209.

**T2b. Tracer performance — CONFIRMED, HARD PREREQUISITE FOR T4.** Measured twice: the
tracer costs ~20-40 min/epoch at 305 stations (runs/net305_10ep_audit 4 epochs in 2.7 h;
dedicated timing probe runs/net305_4ep_timing 1 epoch in ~20 min; untraced baseline
~1 min/epoch). Hotspot not yet isolated by inspection (reconstructStation is a cached-LDLT
solve; the write loop is ~4k rows) — instrument coarse per-section timers around the
:6563-6649 block first, then fix. Add `trace_dd_fractional_max_pairs`
(default e.g. 2000, random-but-seeded subsample per epoch) and/or reuse per-station
reconstructions across pairs. Validate: 305-sta epoch wall time < 2 min with tracing on;
distribution statistics match the full-pair run on a 50-sta config.

**T2c. Consumer AR gate attribution.** In closed_loop_trace.csv log per epoch WHICH gate
zeroed AR (min_epochs / position-σ-gate with its value / candidate emptiness / ratio) —
AmbiguityResolver.cpp:292/321/367-375 (σ-gate sentinel ratio 0.00). Also surface the
force-capped 0.20 m undiff σ-gate (GnssConfigLoad.cpp:4730) as a visible config echo.
Validate: rerun any closed-loop config; every epoch has an attribution.

**T2d. Refloat-churn maturity gate.** RAIM-attribution arc isolations grow 2→1063/epoch
(self-sustaining loop; memory root-cause §Other blockers). Gate isolated-arc refloats on
filter maturity (suppress or k-inflate until innovation variance ratios stabilize —
mirror the warmup URA ramp pattern), toggle default-OFF, and add the per-epoch refloat
count to the epoch-status heartbeat. Validate: 1h run — churn decays instead of growing;
clean-window flag counts sane.

## TIER 3 — product-contract honesty [OPUS-READY, small]

**T3a.** `signal_integer` derived from datum: force false when the phase datum is the
ensemble (thread `decl.amb_datum` into `NetworkEstimator::toSolutionEpoch` /
NetworkEstimator.cpp:552-560); consumer must gate AR-eligibility on it.
**T3b.** Serialize the ensemble AmbDatum: `pending_contract.decl.amb_datum` is never set
(ApplicationNetwork.cpp:1060-1132) — on-disk contract mis-declares as pivot-receiver.
Set it, add a round-trip test asserting the S line carries it.
**T3c.** Auto-arm (or fail loud without) `clock_disc_on_set_change` when
`clock_datum_sum_to_zero && emit_clock_disc`; bump per-sat clock disc on a
`clock_signal_basis_hold` re-latch.
**T3d.** B1b σ on the wire: optional trailing `sigma=` token on the P record (mirror the
`cbjump=`/`stale=` pattern, byte-identical when off) + one-shot consumer warning when
`consume_phase_bias_sigma`/`consume_network_iono` is configured on a profile that cannot
receive it. Under the ensemble datum emit the between-satellite-SD σ (already computed
diagnostically in the same block) instead of the zeroed gauge column.
**T3e.** Use the age-gated `getPhaseBias` overload at the R-inflation site
(MeasurementModel.cpp:1832-1837) to match the apply site.
**T3f.** Iono-supply σ: RSS the SH-coefficient formal term (aᵀQ_sh a through the mapping-
scaled basis row) onto the representativeness floor; enforce a floor after `std_scale`;
skip/inflate the supply for satellites without fresh OSB coverage this epoch.
**T3g.** Scope the RT code-bias flip + B1 retro-bump reactions per-satellite instead of
per-constellation (RealTimeEngine.cpp:1783-1817, 1876-1913).
(Antenna WARN + deterministic IF-pin sort already exist as spawned chips — skip if done.)

## TIER 2e — convergence-run affordability [OPUS-READY, before T4]

**T2e. Epoch decimation + trace stride.** (1) `network.epoch_decimation_n` (default 1 =
off): process every Nth synced epoch in the network loop (N=2 → 60 s effective rate,
halves convergence wall time; the convergence horizon is physical-time-driven, so a 60 s
rate loses little). Note: no gnss_rate_s equivalent exists in ApplicationNetwork today.
(2) `network.trace_dd_fractional_epoch_stride` (default 1): write the DD trace only every
Nth processed epoch — fraction statistics don't need every epoch. (3) Verify the RTB-7
warm-resume config-match fingerprint tolerates arming TRACE-ONLY toggles on resume
(trace_dd_fractional etc. must not invalidate a snapshot taken untraced) — if it does
invalidate, relax the match to estimation-relevant fields only. Validate: byte-identical
at defaults; a decimated run's per-epoch products match the undecimated run at the
common epochs to solver precision.

## TIER 4 — the definitive experiment [RUN, not build — after T1/T2]

Two-stage design (convergence cost paid once or not at all):

**T4a (iteration loop, 50 stations).** The uniform-fraction phenomenon reproduces at 12
and 50 stations — the fraction-collapse falsifier is station-count-independent. Run the
T1 A/Bs on a 50-station, ≥3 h window (≈30-60 min wall each): treated vs untreated, grade
only the final 60 min with the T2a meter (stride per T2e). Iterate HERE, not at 305.

**T4b (confirmation, 305 stations, ONE overnight run).** 305-station producer +
ZIM200CHE consumer, ≥2 h window, UNTRACED, with `network.state_snapshot_path` armed
(RTB-7, default 10 s interval) → ~4 h wall at the measured ~55 s/epoch. Then WARM-RESUME
from the ~90-min snapshot with diagnostics armed (T2a σ column + T2b subsampling + T2e
stride, AR armed, T2c attribution) and run only the final 30 min — repeatable in
~30-60 min without re-paying convergence. The snapshot cadence is also crash insurance
for the OPEN ~1.5 h 305-sta non-PD crash (memory `project_network_305_longrun_crash`,
evidence runs/net305_qc_3h_crash — predates the pin-aware guard; re-check, resume past
it if it still fires). **Grade ONLY t > 90 min:** (a) |frac| distribution vs uniform
benchmark; (b) dd_sigma_cyc calibration vs empirical scatter (χ²); (c) churn trend;
(d) phase-bias product step RMS (< 0.05 cyc/epoch sustained is the AR-grade bar);
(e) AR gate attribution — did MLAMBDA ever score.
Success for the Tier-1 treatment = fractions leave the uniform regime and σ calibrates.

## FABLE-GATE (do NOT hand to Opus without a Fable-written spec)

- Carry-incest structural fix: extend the C1 dense carry to the shared-local cross block
  for carried ambiguities (cross-cutting covariance seam — the class Opus mis-traded
  before, see queue P0-1 history).
- TASK 2 full inter-gauge B⁻¹ fold (NetworkNormalEquations).
- Eq-34 integer datum: per-(receiver,signal) phase-bias states + single-receiver datum,
  or de Jonge chain pivoting (StateManager/estimability change).
- TASK 1 hard init-then-fix datum (HANDOFF_phase_ensemble_warmup_lock.md — see its 2026-07-09
  scope correction: stability goal only) — borderline; needs the lock-epoch criterion designed.
- Consumer ±537 m one-epoch L1C excursion (clfrac_probe TOW 259620/259650): INVESTIGATE
  card — instrument per-signal applied-correction deltas first; root cause unknown.

## Verification contract (what the follow-up Fable pass will check)

1. Re-grep every marker this doc cites (concurrent-session discipline).
2. Suite 592/1 (or higher with new tests); byte-identity OFF hash-proven per card
   (network_solution.csv / network_products.txt vs runs/net305_10ep_repro baseline,
   SHA 497d7dc5… for the solution CSV).
3. Per-card falsifiers above actually run and reported honestly (a negative A/B is a
   finding, not a failure — do not quietly re-scope).
4. No estimation-behavior change hidden behind a default-ON path.

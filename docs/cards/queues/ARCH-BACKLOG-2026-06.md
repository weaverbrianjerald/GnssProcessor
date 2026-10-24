# ARCH-BACKLOG: architecture opportunity audit of 2026-06-15 and the Apr-Jun 2026 PPP optimization record

**STATUS**

- **id:** ARCH-BACKLOG
- **topic:** queues
- **status:** reference
- **summary:** Ranked P0/P1/P2 refactor backlog of the 2026-06-15 audit (33 candidates, 8 killed) with the killed and rejected lists, and the settled tuning, traps and rejected proposals of the Apr-Jun 2026 PPP / PPP-RTK work.
- **dates in the text:** 2026-04 to 2026-10-07
- **source:** agent memory notes `project_arch_opportunity_backlog` and `project_optimization_state`, copied 2026-10-24

**Outcome.** Two records of the user-side code base with no other home. The audit (6 hunters and a YAGNI skeptic) found that the highest-value wins are consolidating confirmed duplication, cheap fail-fast guards and pure-kernel tests; its order is P0 #23/#24, then #1, then P1 by appetite. The optimization record keeps the author's settled values (AR gates, phase thresholds), the traps and the rejected proposals, which are not to be re-proposed. The status note lists what landed since (card UOM) and what was not re-verified.

> Results are appended TO THIS FILE, below the existing text.

---

## 2026-10-24: architecture opportunity audit of 2026-06-15 (ranked backlog, killed and rejected lists)

These results were recorded only in the agent memory note `project_arch_opportunity_backlog` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: since the 2026-10-07 recheck, #12 (pivot-slip MAD classifier, `kalman::kPivotDetectors`, `tests/test_kalman_pivot_detectors.cpp`) and #31 (`validatePostFitResiduals`, staged) landed with card UOM U2c and U2d, #11 landed in part (`KalmanKernels.cpp`) and #1 in part (`common/GnssIdentity.hpp`, a single `systemToIsbName`). `StateManager::setCovariance` throws on non-finite P while `setStateVector` only logs. The P2 items and P0 #4 were not re-verified.

Audited 2026-06-15 (6 hunters + YAGNI skeptic; 33 candidates, 8 killed). No repo document holds it.
Rechecked 2026-10-07: the P0/P1 refactors have NOT landed; some pure-kernel tests now exist (below).
Line numbers drift: re-grep before acting.

**Key finding:** the highest-value wins are not new abstractions but consolidating CONFIRMED duplication,
cheap fail-fast guards, and pure-kernel tests. Order: P0 #23/#24, then #1, then P1 by appetite. Respect
memory note `feedback_prefer_fallback_over_drop` (fail-fast only for silent NUMERICAL corruption) and
memory note `feedback_correctness_over_expedient`.

**P0**
- #23/#24: `KalmanFilter::update()` and `applyPseudoObservation` commit x/P with NO `allFinite` guard;
  `updateFromPrior` has one (still true 2026-10-07). Propagate it; have `josephPseudoUpdate` report
  `ldlt.info()`.
- #1 System enum helpers: `enum class System` (`GnssTypes.hpp`) ships none; about 17 files re-roll the
  switch (23 occurrences); `systemToIsbName` is byte-identical in `StateManager.cpp` and
  `StateInitializer.cpp`; also `KalmanFilter.cpp` `consPrefix`, `MeasurementModel.cpp` `ddConstellationOf`. Add free
  `toConstellationName / toIsbName(GPS="") / toClockRefChar / systemFromChar /
  systemFromConstellationName` in `common/`. Add IRNSS to the enum (half-present in char switches, absent
  from the enum, still absent 2026-10-07). Reconcile the divergent default cases ("" vs "Unknown" vs
  raw char) deliberately: they change log/key strings.
- #30 `makeArConfig()`: the `AmbiguityResolverConfig` block is repeated in `Application.cpp`,
  `ApplicationBaseline.cpp` and `RealTimeEngineInit.cpp` (2026-10-07).
- #4 active-system filter predicate duplicated between `Application.cpp` and the RT engine.

**P1**
- #11 extract the pure Joseph update kernel (S/K/dx/P/Sr -> `KfUpdateResult`) from `KalmanFilter.cpp`
  (3 callers; the most load-bearing, currently untestable numerics); widen `KalmanFilterTestAccess`; folds in #16 (standardizeResiduals, Sr clamped to R).
- #12 extract the BSSD pivot-slip MAD classifier (computed twice) from `validatePostFitResiduals`;
  mirror `StochasticModelTestAccess`; pins flip-flop / exclude-set boundaries tuned blind across 3+ June
  fixes; first slice of #31.
- #18/#17/#21 pure physics tests: GnssCorrections Sagnac / Shapiro still have NONE (2026-10-07);
  `CoordinatesTest` (6), `TimeUtilsTest` (11), `InterpolateTest` (12), `SolidEarthTideTest` (1),
  `PlanetarySunFrame` now exist. GnssCorrections is the vertical-bug locus
  (memory note `project_vertical_instability_solidtide`). Seam idiom: memory note `project_test_suite_initiative`.
- #6 `IByteStreamSource` port, the ONE justified new port: NtripClient / TcpClient / FileReplay are a real
  Rule-of-Three (`TcpClient.hpp` "Mirrors NtripClient"); collapses the `Kind`-tagged switch and the parallel
  `openPrimary` / `openAuxRtcmStream` in `ApplicationRealtime.cpp`; TLS (`ntrip_use_tls`) is a 4th-variant pressure.

**P2:** #13 RTS smoother-step kernel; #14 `buildJacobianRow` behind a JacobianContext; #15
resetFilterCovariance seeding kernel; #19 Saastamoinen/GMF tests; #22 Ewl/Graphic detector TestAccess
(computeMadSigma dedup already done in `RobustStats.hpp`); #25 LAMBDA back-transform unimodularity check;
#26 `setImuState` finiteness guard; #27 `computeAmbiguitySeed` swallows all exceptions -> 0.0; #28
smoother write-back guard; #29 incremental shared-EpochProcessor (proven-shared pieces only); #31 split
`validatePostFitResiduals` incrementally; #2/#3/#7 RT transport tidy (subsumed by #6).

**KILLED as YAGNI, do not re-propose:** #5 ProcessingMode strategy (numeric state handling, fights
StateManager); #8 `IObservationProvider` (no offline SBF/RTCM replay path); #9 DataDownloader
ITransport/IDecompressor (one of each); #10 ITraceSink (single CSV medium; DRY-merge the 3 loggers at
most); #32 PipelineBuilder / EpochProcessor lift (`EpochUpdateSupport` drains it); #33 GnssConfig breakup
(Pimpl rejected, memory note `project_optimization_state`); #29-full EpochProcessor (drivers necessarily differ).

Cousin: memory note `project_nav_readiness_enhancements`.

## 2026-10-24: Apr-Jun 2026 PPP / PPP-RTK optimization record: settled knobs, traps, rejected proposals

These results were recorded only in the agent memory note `project_optimization_state` until 2026-10-24, and are copied here before the note was shortened. The text is the note's; the dates inside it are the original dates.

Status note, 2026-10-24: `phase_always_reset_cycles`, named as a proposal below, is absent from the tree. The AR gate values were verified against `config/ppp_rtk_internal_AR.json` on 2026-10-07.

Landed Apr-Jun 2026 on the PPP / PPP-RTK (user-side) path. Mechanisms are in git; this keeps the settled
tuning, the traps and the rejected proposals. Do not re-propose or re-derive.

## Settled knobs (the author's values; do not "improve" without data)
- AR gates (verified 2026-10-07 in `config/ppp_rtk_internal_AR.json`, mode 0 + `wl_only`):
  `ratio_threshold 3.0` (loader default 3.0), `ratio_upper_bound 1e4`, `min_success_rate 0.999`
  (bootstrap FFR, Π erf(1/(2√(2·d_i))) over the post-reduction LDL diagonal), `nl_max_per_pair_frac
  0.15`, `wl_max_frac 0.40`, `min_epochs 1`, `max_position_sigma_m 10.0`, `hold_variance 0.001`,
  `active_systems ["GPS"]`. The no-argv default `ppp_rtk_internal.json` runs mode 1 with ratio 5.0,
  min_epochs 10, max_position_sigma_m 0.05. (The AR config moved 5.0 → 3.0 on 2026-05-29, `232015a`;
  the 2026-08-15 memory still quoted 5.0: corrected.) Galileo stays out of AR while its bias model
  is unmodeled (ATX PCV gaps, wind-up convention, per-attribute bias suspected): memory note `project_galileo_c5c7_code_bias`. AR mode 1 is the
  recommended path; mode 3 works under the pseudo-obs scheme.
- Phase: `pivot_slip_min_cycles 0.25` (λ/4 floor on both the pivot-slip detector and the per-row
  phase-outlier path; sub-cycle offsets are multipath/PCV/dynamics, never slips; IF combos exempt),
  `persistent_bias_min_cycles 0.10` (EWMA, about 19 mm on L1, needs n ≥ 20),
  `phase_huber_sres_threshold 6.0` (raised from 3.0 to match the outlier-reset threshold, so sres 3-6
  rows keep full formal-R weight), `phase_elevation_power 1.0` (raised from 0.5 on 2026-07-31; every `test_suite` bench before then is confounded, `docs/topics/build-infra.md`). Clean post-fit phase < 15 mm.
- MW WL: `kWlMaxFrac = 0.30` cap on raw |frac| at lock AND at AR exposure (stops a wrong-integer lock
  when σ_mean is tiny but the running mean carries a real bias); adaptive lock
  |frac| + 3·σ_mean/λ_WL < 0.5 replaces a fixed 5-epoch wait. ⚠ MW gap tolerance = 1.5 × effective
  interval, never the nominal cadence (float drift trips a strict `>` and resets every arc). Thresholds are empirical only
  (memory note `feedback_mw_thresholds_empirical`).
- `filter.satellite_block.{block_scale_factors, freq_band_scale, tracking_attr_scale}` are JSON-tunable;
  the defaults live in `include/config/GnssConfigTypes.hpp`, re-based on modern IGS receivers (block
  spread 3:1 compressed to about 1.2:1) and re-tuned again on 2026-06-10 (`49a6b88`: X 0.85, GPS C/A
  1.15, W 1.40, band 8 0.25, verified 2026-10-07), so read the table rather than quoting values.
  Lasting lesson: combined I+Q (X) is cleaner than I or Q alone (the original X 1.5 was inverted).

## Numerics traps
- Huber post-fit R-inflation converges only if sres is floored at the WORKING (inflated) `R(i,i)`, not
  formal `R_v` (formal pins |sres| and the loop diverges). Queue `R·(|sres|/k)²` in
  `pending_r_inflations_`, apply at next update start, clear in `beginEpoch`.
- LAMBDA units: convert metres/λ to cycles before mlambda and back (it once rounded integer METRES).
  WL-constrained NL float is non-integer for WL ≠ 0: shift by +f2/(f1+f2)·WL_int.
- AR fixes enter as a Kalman pseudo-obs with R = `hold_variance` (Joseph), not the closed form; this
  fixed the mode-3 destabilization.
- `StateManager::resetAmbiguity` must NOT create unknown keys (detectors iterate all RINEX-tracked
  signals and would bypass the band filter).
- BSSD groups by full RINEX-3 signal (`"GPS|L1W"`), not band (band grouping SD'd the pivot against
  itself, 30 m residuals under `channel_mode: all`).
- Constellation canonicals are decided once per run (`decideConstellationCanonicals`), not per epoch
  (leaked L1C/L2X states at low elevation).
- Forced pivot handover runs in the SAME epoch as detection (`hasPendingForcedHandover()`,
  `forced_only=true`); deferring caused a kinematic position spike.
- MW arcs S-transform on pivot handover (new_int = old_int(sat) − old_int(new_pivot)); erase only if the
  new pivot was not integer-locked.
- `relinearize_geometry` pass 2 is a prior-anchored IEKF on the POST-augmentation (x, P) stashed by
  `update()`; TC-IMU keeps the naive pass.

## Structure
`FrequencyMode` (bands; JSON `frequency_mode`, legacy `signal_mode` warns) is orthogonal to
`ChannelMode` (tracking attributes; All ⇒ one ambiguity per sat × tracking mode); the MW detector's
own modes have a `Mirror` sentinel. `GnssConfig.hpp` uses `json_fwd`, enums/PODs live in
`config/GnssConfigTypes.hpp`, direct-member defaults in the ctor: tuning one recompiles only
`GnssConfig.cpp`, but struct-internal defaults or a new member + getter still rebuild the ~38-TU cone.

## REJECTED, do not re-propose
- Full Pimpl of GnssConfig: about 730 edits, needs deep copy (held BY VALUE in
  `MultiRinexSync::StationData`, `Application::config_`), still rebuilds the cone on an ADDED toggle
  (it only firewalls default/layout churn). Defaults-in-ctor captured the common case at about 1/4
  the risk.
- Sequential / scalar KF update: the biggest remaining KF lever (Joseph about 60 % of KF time), but the
  author has never asked for it. Do not push.
- Do not delete staged detectors: `GfSlipDetector` is uncompiled/staged; `IfPhaseSlipDetector` is built
  and active on the NETWORK path, staged for user/kinematic (corrected 2026-10-07 from "both staged").

## Open / deferred
- `state_ambiguity_basis: nl_wl` is built, default `per_band`, PARKED: field-exercised 2026-08-11 (guarded =
  safe, not working AR; corrected 2026-10-07 from "never data-validated"); no slip path, non-MW reset keys are
  silent no-ops (memory note `project_nl_wl_basis`).
- `phase_always_reset_cycles` (absolute gate at |innov| ≥ λ/2): discussed, not implemented.
- Linearizing I_h around estimated I₁ in the Jacobian (removes GIM dependence for absolute STEC):
  deferred; the author chose "pick the right STEC source" first.

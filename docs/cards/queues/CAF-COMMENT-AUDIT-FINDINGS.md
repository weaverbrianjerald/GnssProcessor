# CAF: findings from the whole-repository comment audit

**STATUS**

- **id:** CAF
- **topic:** queues
- **status:** open
- **summary:** code defects, unverified suspicions, author checks, wrong CLAUDE.md claims and dead code found while every comment in the repository was checked against its code; a checklist, one row per finding
- **dates in the text:** 2026-09-22 → 2026-09-28
- **knobs / posture:** `processing.enable_noncausal_slip_bridge`, `model_corrections.apply_phase_shifts`, `network.gross_los_emit_exclude`
- **hashes named in the text** (commit / exe / byte-cert): `b3a8e48`, `ae53603`, `335f640`
- **run dirs named in the text:** none
- **source:** the comment rewrite (commit `b3a8e48`); every comment and docstring was rewritten with its claims checked against the code

> Results are appended TO THIS FILE. An item is closed by ticking its box and adding the commit or the reason.

---

## 1. Scope and how to use this card

The comment rewrite of commit `b3a8e48` read every comment in `src/`, `include/`, `tests/`,
`scripts/`, `pyviz/`, `python/` and `tools/`, and checked each behavioural claim against the
code it describes. Comments the code contradicted were corrected in that commit. The code was
not changed; what the audit found in the code is listed here.

Each row gives the location, the finding and the action owed. Line numbers were re-checked on
2026-09-28 against `ae53603` and drift as the files change; the function name is the stable
reference. The classes are:

- **Verified:** read in the code and confirmed.
- **Unverified:** plausible from the code, not confirmed by a test or a run.
- **Author check:** a fact outside the code (a standard, an intended value) that only the
  author can settle.

## 2. Verified defects, open

- [ ] **ProductCache error path deadlocks.** `ProductCache::loadProducts` holds `load_mutex_`
  (`src/core/products/ProductCache.cpp:214`); its catch block calls `clearCache()` (`:265`),
  which locks the same non-recursive mutex again (`:271`). Undefined behaviour, in practice a
  deadlock, on any exception during a product load. Fix: an unlocked `clearCacheLocked_()`
  called from both.
- [ ] **GRAPHIC detector threshold ignores the geometry trend.** `GraphicSlipDetector`
  compares the raw epoch difference `|dG|`, which still holds the range-rate and clock change,
  against a median-centred MAD threshold (`src/core/slip/GraphicSlipDetector.cpp:219`). Once
  the window fills, a moving satellite trips it. Inert unless the "graphic" detector is
  selected. Fix: compare `|dG - median|`, or difference against a fitted trend.
- [ ] **Bootstrap downloads cannot authenticate to CDDIS.**
  `scripts/bootstrap_internal_data.py` `download_file` uses plain `urllib.request.urlopen`
  with no authentication handler, so `.netrc` is never read and CDDIS returns 401 (docstring
  at `:181` now says so). Fix: an `HTTPBasicAuthHandler` fed from `netrc.netrc()`, with the
  Earthdata redirect handled.
- [ ] **PPP fix-and-hold is a no-op under the default mask.** `holdAmbiguities` calls
  `buildDdPairs(state_manager, {})` with no observations
  (`src/core/filter/AmbiguityResolver.cpp:2795`), so every pair's elevation is 0 and the
  default 20° `elevation_mask_ar` drops every pair. `ar_mode` 3 holds nothing.
- [ ] **GL-2 stand-down discontinuity bumps can be lost.** The gross-LOS stand-down queues
  satellites into `ura_retro_sats` (`src/core/filter/NetworkGrossLos.cpp:589`), but the drain
  runs only when one of five other knobs is on (`src/core/filter/NetworkSolPrep.cpp:779`). With
  GL-2 alone the bumps never land. Latent: the goldens arm `network.gross_los_emit_exclude`,
  one of the five.
- [ ] **`gim_` is destroyed before `ppp_model_`.** `gim_` is declared after `ppp_model_`
  (`include/app/Application.hpp:903`, `:912`), so it is destroyed first and `PppModel` holds a
  dangling non-owning pointer during teardown. Harmless unless `PppModel`'s destructor uses it;
  the comment in `Application.cpp` that claimed the opposite order is corrected. Fix: declare
  `gim_` before `ppp_model_`.
- [ ] **Configuration summary mislabels PPP-RTK modes.** `logConfigurationSummary_` prints
  `PPP_RTK_Network` and `PPP_RTK_User` runs as "Baseline-Multi"
  (`src/core/config/GnssConfigLoad.cpp:3378`); the label list at `:293` is right. Log output
  only.
- [ ] **Dead counter.** `n_depose_unusable` (`include/filter/NetworkPhaseDatum.hpp:266`) is
  summed into the run-end readout (`ApplicationNetwork.cpp:2029`) but never incremented.
- [ ] **Dead field.** `NetworkStateSnapshot::format_version`
  (`include/products/NetworkStateSnapshot.hpp:189`) is never read or written by
  `serialize()` / `parse()`.
- [ ] **`Gpt3::load` keeps `loaded_` after a failed reload.** `loaded_` is set on success
  (`src/core/atmosphere/Gpt3.cpp:109`) and never reset: a failed second load clears
  `cells_` (`:105`) but leaves `loaded_` true, so the object reports loaded with an empty
  grid.
- [ ] **`computeKlobucharDelay` has no size check.** It indexes the coefficient vector, which
  is empty or 8 long, without checking (`src/core/atmosphere/Ionosphere.cpp:163`).

## 3. Verified defects, fixed

- [x] **`KalmanFilter::update` wrote through `const_cast`** into its `const` copies of `x` and
  `P` (undefined behaviour); fixed in `ae53603`.
- [x] **Unreachable post-fit residual summary** in `KalmanFilter::update` (the outlier branch
  returned first); removed in `ae53603`.
- [x] **Persistent-bias warm-up hard-coded to 20** instead of
  `estimation.residual_history.warmup_n`; fixed in `ae53603`.
- [x] **RINEX 3 "SYS / PHASE SHIFT" count read one column left**, so per-satellite shifts
  were applied system-wide under `model_corrections.apply_phase_shifts`; fixed in `ae53603`.
- [x] **`finalizeNetworkProcessNoise_` re-read 11 of the 12 process-noise classes**, leaving
  the iono-carry source label stale; fixed in `335f640`.

## 4. Unverified suspicions

- [ ] `NetworkNormalEquations::ddPriorPosteriorMean`
  (`src/core/filter/NetworkNormalEquations.cpp:1262`) has no guard for a chordless block,
  unlike its sibling functions; a 0×0 `G` times an n×1 vector would assert in Debug. Callers in
  the carry driver do not check. Whether a chordless block reaches it is unknown.
- [ ] VMF3 mapping with a standard-atmosphere a priori takes the ZHD pressure from GPT3
  silently (`Troposphere.cpp`). Confirm whether the mix is intended.
- [ ] `applyStagedStateSnapshot_` with scope "full" detects an unknown dynamic state kind in
  the middle of the replay and cold-starts after a partial mutation
  (`RealTimeEngineStateSnap.cpp`). Validate before mutating.
- [ ] `NetworkProductIO` `checkContract` compares 6 of the 8 emitter parameters (`cbjump` and
  `clkset` are not compared). Confirm whether the omission is intended.
- [ ] Gross-LOS reason bits in the arc ledger may be cleared by `kArcReasonLateMask` at the
  next epoch (`ArcLedger`).
- [ ] `MwSlipDetector::arcZdMean` returns `bia_corrected = false` for a group's pivot arc,
  which is BIA-subtracted; if so, every cycle containing a pivot arc counts
  `skip_mixed_frame`.
- [ ] Carry driver: an arc present but without a resolvable column stays in the old-arc set,
  and a returning chord through it gets a zero-mean, zero-variance carried prior. The
  production caller filters such arcs (`NetworkDdPriorBuild.cpp`); only direct callers (tests)
  reach it.
- [ ] The B9 carry-install gate in `NetworkSolveGlue` lacks the `R.opin_rows > 0` term of the
  primary gate. Equivalent today; diverges if the within-epoch tier is decoupled from
  `carry_warm`.
- [ ] `NetworkRawObservables.cpp:92` orders same-band signals with `std::sort` on the band
  digit only; lexical order holds in practice for short lists, but only `std::stable_sort`
  guarantees it.
- [ ] `RtcmMsmDecoder` SBAS `formatSat` may produce `S120` where `S20` is expected.
- [ ] `ApplicationNetworkSetup.cpp:1506` hard-codes `pending_contract.orbit_ref = "CoM"` in
  every a-priori mode; the reasoning covers SP3 only, `brdc_rts` is unchecked.
- [ ] `TcpClient`'s receive timeout bounds shutdown only; it does not detect a silent peer.
- [ ] `ApplicationBaseline.cpp` early abort (near the `n_geom_ok` check) can never fire,
  because `n_geom_ok` is incremented together with `n_with_dd`.
- [ ] `TimeUtils` leap-second lookup compares a GPS Julian date against UTC entries, so the
  switch happens 18 s early at an insertion.
- [ ] `OceanTideModel` applies no nodal corrections (f, u), and its GMST uses GPS time rather
  than UT1 (about 0.15° on M2).
- [ ] `NetworkStashStatus`: parked re-admission seeds folded by the D2 inject are skipped by
  the VCE and census mirrors (a possible census coverage gap).
- [ ] `NetworkAmbFloat`: the drop floor is not elevation-mapped unless the birth floor is
  armed.
- [ ] `IfPhaseSlipDetector`: the 0.5 m position gate is up to 4.7 ionosphere-free cycles, so
  the "conservative margin" rationale is weak; its "88 %" variance share is unreconciled with
  the 67 to 70 % quoted elsewhere.
- [ ] `pyviz/gnssviz/viewers/ambiguity_locktime.py` `_load_slip_events` keeps "lli" over
  "slip" on a shared epoch, so a coincident detector slip is drawn as soft.
- [ ] `MeasurementModel.cpp`: the band-count warning names an extra-band ambiguity, but the
  state stays in the filter.

## 5. Author checks

- [ ] **Clock-jump threshold.** `RealTimeEnginePrepare.cpp:71` (`kJumpThresh`) and
  `Application.cpp:513` (`kClockJumpThreshSec`) both use 5e-5 s (50 µs, about 15 km); the old
  comments said 0.5 ms. The comments now match the code. If 0.5 ms was intended, both
  constants are 10 times too small.
- [ ] **RTCM DF numbers.** `RtcmTypes.hpp` cites DF490 / DF491 for the SSR phase-bias
  dispersive and MW consistency bits; RTCM 10403.3 is believed to use DF486 / DF487.
- [ ] **GLONASS SSR epoch.** Labelled DF389 in `RtcmSsrDecoder.cpp`, `TimeUtils.hpp` and
  `ssr_corrections.tex`; DF386 is believed correct.
- [ ] **Almanac health.** `RtcmNavDecoder.cpp` cites "DF040 almanac health"; DF104 is likely.
- [ ] **Lane-slip response direction.** A test comment in
  `tests/test_gap_reentry_relevel.cpp` said refloat-always measured 16 points worse than the
  evidence-gated keep; CLAUDE.md (wu93 amendment 2) gives refloat-always 70.2 % against keep
  54.0 %, better. The comment now points to `SPEC_lane_slip_response.md` without a direction.
- [ ] **Rank-gate spectrum figures.** "Gauge σ about 1e-10 of σmax" and "a 7-decade gap"
  (`NetworkSolveGlue.cpp`, `NetworkEstimator.hpp`) do not reconcile with each other.

## 6. CLAUDE.md claims the code contradicts

CLAUDE.md's older sections are record, so these are the author's to correct.

- [ ] "File management": `IGS_Pasadena` is named the default `baseline_fixture`; the loader
  defaults to `Custom` (`src/core/config/GnssConfigLoad.cpp:3104`).
- [ ] "CDDIS authentication": `DataDownloader` is said to resolve `.netrc` at startup; it
  resolves it on the first download attempt (`DataDownloader::resolveNetrcPath`,
  `src/core/products/DataDownloader.cpp:115`).
- [ ] The simulator writes a 15-column `truth_atmo` file; `pyviz/conftest.py` builds a
  14-column fixture (no `dstec_turb_tecu`).

## 7. Dead or unused code

- [ ] `SlipBridgeKernel` has no production caller (only `tests/test_slip_bridge_kernel.cpp`),
  and `processing.enable_noncausal_slip_bridge` is parsed but never read.
- [ ] `Ionosphere::getTroposphereMapping` is declared (`include/atmosphere/Ionosphere.hpp:51`)
  and never defined; `Troposphere::getTroposphereMapping` is the one in use.
- [ ] `PppModel::rescaleGlonassObservation` and `PppModel::computeIonosphericDelay` have no
  callers.
- [ ] `ProductCache` antenna helpers are declared and never defined.
- [ ] `IfPhaseSlipDetector` writes `prev_los` and never reads it.
- [ ] No callers: `IonoVariabilityMonitor::resetSatellite`, `NetworkPbEmitFilter::reset`.
- [ ] Unused members or parameters: `NetworkLedgerEmit` `declare_wire` / `decl_wire`,
  `NetworkSdEwlAr` `now_s`, `NetworkOraclePin` `lambda_csv_m`, `LogManager`
  `gps_utc_offset_s_`, the constellation argument of `signalAttributePriority`, and `config_` in
  `NetworkFrameDiag`, `NetworkClockAnchor`.
- [ ] Unused includes: `GPS_Lx_WAVELENGTH` / `kDefaultDiscThresholdM` headers in
  `GnssConfigLoad.cpp`; `IlsCore`, `NetworkCarryVce`, `<random>`, `<deque>` in
  `ApplicationNetworkSetup.cpp`.
- [ ] `StateInitializer`, `DdPivotManager` and `MeasurementModel` build state keys by string
  concatenation instead of the `StateKeySchema` helpers the header requires. The formats match
  today.
- [ ] `scripts/event_leg.py` `_leg_common` has an unreachable `print` after `return 0`.
- [ ] `pyviz` `network_discontinuities.py` never reads `clock_model_max_gap_s`.
- [ ] `NetworkPhaseDatum.cpp` about lines 814 to 816: statements mis-indented inside
  `if (cit != cand.end())` (layout only).

## 8. Output strings and tables that disagree with the code

These are program output or data, so the comment rewrite left them unchanged.

- [ ] `scripts/simnet_make_roster.py`: the `--rx-bias-bloc-sd-tecu` help text says default
  8.4; the code default is 6.0 (`:201`).
- [ ] `scripts/sbf_probe.py` `NAMES` maps 4110 to "Meas3Doppler" and 4111 to "Meas3PP";
  `Meas3Decoder.hpp` numbers 4110 as Meas3CN0HiRes and 4111 as Meas3Doppler.
- [ ] `scripts/fetch_global_obs.py` accepts `--crx2rnx` and never uses it (decompression
  goes through the `hatanaka` library).
- [ ] `MeasurementModel.cpp:1758`: the `CODE-DECOMP` trace field `bia_cbias` prints the
  receiver code-bias state term, not the .BIA correction.
- [ ] The simulator's scenario summary hard-codes its signals line to 4 per system.

## 9. References a reader cannot follow

- [ ] `AmbiguityResolver.cpp` (near line 432) points to CLAUDE.md notes on a
  "WL-fix-with-tight-coupling milestone" that do not exist.
- [ ] `AmbiguityResolver.cpp` and `NetworkPriorResidual.hpp` cite a private assistant memory
  file (`project_m1_card4_d5_innovation`) as if it were a repository document.

## 2026-09-28: findings from the LaTeX report prose pass, run 1

The prose pass over `docs/report/` (slip_detection, ambiguity_resolution, imu_aiding,
ssr_corrections, realtime_engine) checked the report's claims against the code and
corrected the report. What it found in the code or the mathematics:

- [ ] **Verified defect: GLONASS ephemeris time anchored on the host clock.**
  `decodeGlonass1020` promotes `toe` with `TimeUtils::nowContinuousGpsSeconds()`
  (`src/core/rtcm/RtcmNavDecoder.cpp:327`), so a file replay of historical data puts the
  ephemeris on the wrong day and GLONASS broadcast orbits are propagated across days.
- [ ] **Author check: `eq:imu-tc-jac` (docs/report/imu_aiding.tex).** The Doppler (m/s)
  partials with respect to attitude and accelerometer bias carry acceleration units; an
  interval factor appears missing. The code implements the same expression.
- [ ] **Stale comment:** `ImuCouplingMode::LooseCoupled` in
  `include/config/GnssConfigTypes.hpp` says GNSS position and velocity solutions update the
  INS; both coupling modes use raw observations.
- [ ] **Stale comment:** `src/core/config/GnssConfigLoad.cpp` (near line 2719) cites
  `RealTimeEngine::run`, which does not exist; the aided prediction runs in
  `RealTimeEngine::processEpoch`.

## 2026-09-28: findings from the LaTeX report prose pass, run 2

Run 2 (network, network_example, network_carry, pipeline_diagrams, state_vector):

- [ ] **Verified defect (upgrades the section 4 suspicion):** a chordless (K = 0) block's
  slot is stored with a default-constructed 0x0 `G` (`foldReducedDdPriors`,
  `src/core/filter/NetworkNormalEquations.cpp`), so `ddPriorPosteriorMean` forms
  `G.transpose() * x_s` with mismatched dimensions: an Eigen assertion in Debug, undefined
  behaviour in Release. The header promises an empty result, and the sibling functions
  (`ddPriorPosteriorCovariance`, `ddPriorInnovation`) return early for K = 0.
- [ ] **Stale comment:** `StateKeySchema.hpp` section 4b says the GLONASS CDMA bands keep the
  per-band IFB key; `resolveReceiverBias` returns none for them.
- [ ] **Stale comment:** `StateManager.cpp` gives the baseline DD key as `GPS|G05|L1C|G01`,
  without the `DD|` prefix the code uses.
- [ ] **Figures to redraw (docs/report/pipeline_diagrams.tex; the captions are corrected):**
  the SSR decode box lists message types 1057 to 1068 only (1240 to 1270 are also accepted);
  the real-time front-end figure draws the observation-to-SSR hold as a buffer before
  `processEpoch`, while the hold is the first admission gate inside it; the network figure
  omits the station-modelling pass (`networkPassAHead_`) before registration.
- [ ] **Table to update (docs/report/network.tex, seam table; the prose is corrected):** it
  calls the prefit census always-on (it runs only when `trace_prefit_census`,
  `trace_topology` or `seed_birth_check_sigma` is armed) and omits `networkBuildSignalRoster`,
  `networkAmbFloatApplyDrops`, `networkGrossLosScreen` and `networkClockLevelRebase`.

## 2026-09-28: findings from the LaTeX report prose pass, run 3

Run 3 (notation, foundations, observation_equations, bssd_dd). No code defects found.
Two questions about the mathematics, left unchanged for the author:

- [ ] **Author check: emission-time error estimate (docs/report/observation_equations.tex).**
  The estimate includes the ionosphere and quotes 30 to 60 ns; the code's seed pseudorange
  is ionosphere-free whenever the clock-reference signals exist
  (`Geometry::extractPseudorange`), so the dominant error is troposphere, antenna and noise
  (tens of ns).
- [ ] **Author check: `eq:noise-model`.** The equation shows the SNR factor multiplying the
  baseline variance; under the `snr` strategy the code's model replaces the baseline
  variance (`StochasticModelWeight::computeSnr`).

# Aiding-source / `IAidingSource` abstraction — design

**Status: LANDED** — steps 0-6 of the §10 migration table all shipped (verified 2026-08-14; anchors
re-grepped 2026-08-15). Design date 2026-06-12; roadmap item nav-readiness #1. §16 records every
deviation, including the anchor drift in §1/§6-§9/§12 and the one step-5 hop that is NOT wired.
Grounded in a code-level map of the four IMU touch-points plus the two idioms it mirrors, then put
through a three-lens adversarial review; §15 records what the review changed, and the body below is the
**corrected** design.

> ⚠ The interface / value-type / registry code in §3-§5 and §13 was *proposal* code. The shipped headers
> (`include/aiding/{IAidingSource,AidingTypes,AidingSourceRegistry,AidingSourceManager,ImuAidingSource,
> ZuptAidingSource}.hpp`) are authoritative; those sections state the CONTRACT and its rulings rather
> than reprinting source.

---

## 1. Goal

An *aiding source* is anything contributing navigation information beyond GNSS pseudorange / phase /
Doppler — IMU, ZUPT, NHC, wheel odometry, baro height, vision. Pre-migration the IMU was welded into
**four** sites (predict branch; TC update Jacobian in the filter loop; TC update Jacobian in the naive
IEKF relin; integrator construction/lifecycle + member) plus latent `enableImu()` queries in
`classifyDynamicsMode` / `handleGap` / the MW gap tolerance — any new modality was four-file surgery.
The §9 table carries all eight pointers with their pre-migration line pins.

The fix: each source **composes** by contributing three things, each at the exact seam the IMU injects
that thing today — **(a) state**, its own block owned *through* `StateManager` (no sibling state owners);
**(b) predict**, a Φ/Q sub-block (or mechanization hook); **(c) measurement**, rows into the update (or
column re-parameterization). With no source registered every seam degrades to today's GNSS-only call, so
the GNSS-only path is **byte-identical**.

---

## 2. Idioms mirrored (verified against the tree)

1. **`ISlipDetector` + `SlipDetectorRegistry` + `CycleSlipManager`** — pure-virtual contract; an enum
   (`SlipDetectorKind`) tripling as registry key / provenance tag / config token; a stateless
   Meyers-singleton creator table; the owner holds `vector<unique_ptr<…>>` **plus one typed handle** for
   the privileged member recovered by a no-RTTI down-cast (`CycleSlipManager::mw_`, design pin
   [CycleSlipManager.hpp:368](include/slip/CycleSlipManager.hpp) — §16 has the current sites, and notes
   that `asMelbourneWubbena()` is declared on `ISlipDetector`, not the manager).
2. **`MeasurementBuilder` strategy** — thin stateless orchestrators receive the host `MeasurementModel&`
   and call back into its **private** numerics via a *tight* `friend` list; the delicate arithmetic
   **never leaves the host**.

```
IAidingSource          ← ISlipDetector            (polymorphic contract)
AidingSourceRegistry   ← SlipDetectorRegistry     (Meyers creator table, config-driven)
AidingSourceManager    ← CycleSlipManager         (owns vector<> + typed ImuAidingSource* imu_)
appendAidingRow sink   ← MeasurementBuilder friend principle (numerics stay home)
```

---

## 3. The interface

Shipped as `include/aiding/IAidingSource.hpp` — read it for signatures. The rulings that shaped it:

- **Three contributions, each at the IMU's own seam.** **STATE** — `reserveStates(StateManager&) -> optional<StateBlockSpec>`: the source NAMES (tier-1) or RESERVES (tier-2) its block THROUGH `StateManager`, never as a sibling owner; error-state sources keep their NOMINAL state off-vector in the designated single owner (the KF's `C_b_e_`), never inside the source. **PREDICT** — `contributePredict(const AidingEpochContext&) -> PredictContribution`: a Φ/Q sub-block + optional nominal write-back that `KalmanFilter` splices over the `StateManager` baseline (**assignment, not `+=`**). **MEASUREMENT** — `contributeMeasurement(...) -> MeasurementRows`: independent rows (H/z/R), OR re-parameterization of existing GNSS rows via extra Jacobian columns (the TC-IMU pattern).
- **Mirrors `ISlipDetector`:** uniform `(const GnssConfig&, LogManager&)` ctor so one registry `Creator` works; narrow base; rich source-specific API kept OFF the base and recovered via the no-RTTI typed down-cast `asImu()`.
- **Opt-out is the NATURAL empty/invalid return, not a bool predicate.** `contributePredict` defaults to `.valid==false` (the KF skips it, leaving predict byte-identical to GNSS-only for measurement-only sources); `contributeMeasurement` defaults empty. *Changed vs. the draft:* the `contributesPredict()`/`contributesMeasurement()` predicates were dropped as redundant with those signals — the same signal the no-source fast path keys on, and the same way `ISlipDetector` expresses non-participation (empty `detect()`).
- **`reparameterizesGnssRows()` stays** (default false) — the one capability that CANNOT be inferred from a return value, because it selects the column-write seam (TC-IMU) over the row-append seam.
- **`reserveStates` is contractually two flavors** (§6): tier-1 `describeCoreBlock` names pre-existing offsets with no alloc/P-init; tier-2 `reserveDynamicBlock` allocates + seeds. The returned `StateBlockSpec` is a one-shot RECEIPT, **not a durable index cache** — per-epoch index resolution MUST go through a LIVE `StateManager` lookup, because `removeStateAtIndex` shifts block indices on every lower prune.
- **Lifecycle:** `ingestSample(AidingSample)` (real-time push, no-op in replay), `hasData()` (pure virtual, mirrors `ImuIntegrator::hasData()`), `bridgesGaps()` (INS dead-reckon), `reset()` (decorrelate + reinflate the owned block on dropout/reacquire), `enableTracing(path)`/`isTracingEnabled()`.

---

## 4. Value types

Shipped as `include/aiding/AidingTypes.hpp`. `AidingSourceKind {Imu, Zupt, Nhc, WheelOdom, Baro, Vision}`
triples as registry key / provenance tag / config token, with `toString` / `aidingKindFromString`.
Load-bearing field semantics:

- **`StateBlockSpec`** (`owner`, `key_prefix` e.g. `"AID|IMU"` tier-2 only, `start_index`, `size`, `layout` labels): a one-shot reservation RECEIPT; `start_index` is a **snapshot — re-resolve live each epoch.**
- **`PredictContribution`** — generalizes the IMU `ins_map` injection ([KalmanFilter.cpp:559-588](src/core/filter/KalmanFilter.cpp)): `indices` (full-state indices owned = the ins_map), `Phi`/`Q` n×n over those indices, `nominal_indices`/`nominal_values` (segment overwritten in `x`), **`dt_total` = the INTEGRATED span, which drives the baseline Q interval** (not the GNSS `dt`), `num_samples`, **`requires_naive_relinearization` set ONLY for TC coupling**, and **`valid{false}` ⇒ the KF skips this contribution.**
- **`AidingMeasurementRow`** (`jacobian` over the full state, `innovation`, `variance`, `label`, `signal`) + `using MeasurementRows = std::vector<AidingMeasurementRow>`. **`label` MUST be unique within the epoch** (e.g. `ZUPT_vx`/`_vy`/`_vz`) so KF outlier/exclusion keys never collide; `signal` is the secondary token (e.g. `"AID"`).
- **`AidingSample`** (streaming push): `kind`, `time`, `accel`, `gyro`, and a `scalar` for baro height / wheel speed by kind.
- **`AidingEpochContext`** (handed uniformly to `contribute*`, mirroring `SlipEpochContext`): `t_prev`/`t_now`, a **reference** to the current nominal `x`, `state_size` (== H cols), and `first_epoch`, which **forces GNSS-only predict on epoch 0**.

---

## 5. Registry + manager

`AidingSourceRegistry` is a stateless static factory over a process-wide Meyers creator table — a
near-exact copy of `SlipDetectorRegistry`: `Creator = function<unique_ptr<IAidingSource>(const
GnssConfig&, LogManager&)>`, public `create` / `isAvailable` / `availableKinds` / `createEnabled`, private
Meyers-singleton `creators()`, **no `TestAccess` friend** (§13). `createEnabled` builds the
config-selected set **IMU FIRST** (the privileged primary), then Zupt/Nhc/WheelOdom/Baro/Vision;
enabled-but-unavailable (staged Vision) is **warn-and-skipped**, the staged GF/IF slip precedent.

`AidingSourceManager` is the `CycleSlipManager` analog, owned by **both** `Application` and
`RealTimeEngine`: `vector<unique_ptr<IAidingSource>> sources_` (the herd) plus a non-owning typed
`ImuAidingSource* imu_{nullptr}`, set in the ctor by walking `sources_` for the first `s->asImu()`.
Lifecycle (`reset` / `ingestSample`) fans out polymorphically; the rich IMU-only calls (TC Jacobian
inputs, attitude) route through the null-guarded `imu_` handle — the "polymorphic herd + one typed
handle" split `CycleSlipManager` uses for `mw_`.

**Config** (house convention, `estimation` section — these add state-vector error states):
`estimation.aiding.{imu,zupt,nhc,wheel,baro,vision}` → `GnssConfig::getAidingConfig()`, with
`imu = jval<bool>(json,"estimation.aiding.imu","aiding_imu", enableImu())` — back-compat default sourced
from the existing flag, mirroring `cycle_slip.ifp` defaulting to `enable_if_slip_detector_` (design pin
[GnssConfigLoad.cpp:556](src/core/config/GnssConfigLoad.cpp); current sites in §16).

---

## 6. State seam — `StateManager` stays the single layout authority

**The nominal attitude has exactly one owner.** Error-state filters split *nominal* (off-vector) from
*error* (in-vector) state: the state vector holds the 3-state attitude **error** `dpsi`; the nominal
body→ECEF rotation it perturbs is the 3×3 `C_b_e_`, held off-vector as a `KalmanFilter` member
([KalmanFilter.hpp:431](include/filter/KalmanFilter.hpp), via `getAttitude()`/`setAttitude()`). **The
source must NOT relocate `C_b_e_` into itself** — the sibling-state-owner anti-pattern CLAUDE.md forbids,
and the KF still needs `C_b_e_` for the attitude-error reset
([KalmanFilter.cpp:542](src/core/filter/KalmanFilter.cpp)) and `buildJacobianRow`. `ImuAidingSource`
*reads/writes* the nominal through its single owner (the KF today; optionally promote to an explicit
`StateManager::getNominalAttitude/setNominalAttitude` later), exactly as it reads `getAttitudeIndex()`
rather than holding the error states.

**Tier-1 (IMU, the first client): no new state, no new API.** The 9 IMU error states
(att/accel-bias/gyro-bias) are already carved into `core_state_size_` under the `enableImu()` gate
([StateManager.cpp:287](src/core/filter/StateManager.cpp)) and P-seeded from `ImuConfig`
([StateManager.cpp:382](src/core/filter/StateManager.cpp)); `ImuAidingSource::reserveStates` *describes*
those offsets and reserves nothing — **bit-for-bit identical state layout.**

**Tier-2 (variable cardinality — vision landmarks, multi-anchor) is DEFERRED** and must **not** naively append into the churning ambiguity tail: `appendState` always appends at `getStateSize()` ([StateManager.cpp:114](src/core/filter/StateManager.cpp)), so a tier-2 block placed there interleaves with the ambiguity/iono/bias maps that grow and shrink every epoch. Verdict: allocate tier-2 blocks as **core offsets** under a config gate (like IMU), *or* at a stable tail **below all GNSS dynamic states** with a hard contiguity invariant. Either way three invariants become acceptance criteria of the tier-2 milestone (not the IMU milestone): (1) extend the `removeStateAtIndex` decrement loop ([StateManager.cpp:160](src/core/filter/StateManager.cpp)) — today it fixes only the four existing maps — to shift `aiding_block_indices_` ranges; (2) extend `getStateSize()` ([StateManager.cpp:482](src/core/filter/StateManager.cpp)) to add summed aiding-block sizes; (3) per-epoch indices resolved **live** via `getBlockRange(key_prefix)`, never a cached `StateBlockSpec.start_index`. The `StateKeySchema` `makeAidingBlockKey`/`parseAidingBlockKey` addition (`"AID|IMU|…"`, anchored on a leading `AID` tag so it cannot be mis-tokenized against sat-bias/DD pipe-keys) is **deferred to the first tier-2 source** — the IMU client adds zero keys.

---

## 7. Predict seam — invariance by *delegation*, not reimplementation

Where the adversarial review changed the design most. The core rule is
`void KalmanFilter::predictAided(double dt, const std::vector<PredictContribution>& contributions);`
(`dt` = the GNSS interval; contributions may be empty):

- **Empty contributions ⇒ literal delegation.** `if (contributions.empty()) { predict(dt); return; }`, which inherits `x = F*x`, the BSSD freeze-state surgery ([KalmanFilter.cpp:462-484](src/core/filter/KalmanFilter.cpp)) and the `dt<=0` guard **for free and byte-for-byte**. No reimplementation of `predict()`'s body inside `predictAided`.
- **INS contribution present ⇒ the `predictImu` kernel verbatim.** Keep the **Identity seed** for `F` (so non-INS state means — clock, clock-rate, tropo, ambiguities — are **not** advanced, matching `predictImu` today), Q from **`pred.dt_total`** (the integrated IMU span, not the GNSS `dt`), assignment (`=`, not `+=`) over the ins_map, single `P = F·P·Fᵀ + Q`, then the attitude reset. **Do not** switch the baseline to `getTransitionMatrix(dt)`.
- **Why that switch was rejected** (two lenses converged): `getTransitionMatrix` carries the `clock ← clock_rate` coupling at [StateManager.cpp:756](src/core/filter/StateManager.cpp), and the INS ins_map touches only pos/vel/att/ba/bg — never clock or clock-rate rows — so seeding `F` from it would *start* propagating the clock mean via `x=F*x`, which `predictImu`'s Identity seed does not. That is a behavior change for any clock-rate-estimating TC-IMU config, not a free refinement. The refinement (open question #1) is **deferred** until a non-INS predict-contributing source lands, then adopted as a deliberately validated, gated, trace-diffed correction.
- **`ImuAidingSource::contributePredict`** is a thin wrapper over its owned `ImuIntegrator::integrate(...)`: map the returned 15×15 Φ/Q onto the ins_map (pos/vel core offsets + the 9 IMU offsets), set `dt_total` from the integrator, set `requires_naive_relinearization = config_.tightlyCoupledImu()` (**coupling-mode gated** — a *loosely*-coupled IMU keeps the proper prior-anchored IEKF, §8 touch-point 3), `valid = pred.valid`. On `!pred.valid` the contribution is dropped and (the IMU being the only contributor) `predictAided` falls back to `predict(dt)` — identical to `predictImu`'s fallback.
- **Ordering / multi-source safety:** `predictAided` snapshots `x` once at entry, applies all nominal write-backs against the snapshot and commits once, so a later contribution cannot read an earlier one's nominal write (removes float-order coupling); disjoint owned-index sets are asserted (debug); and it completes before `capturePriorSnapshot` ([Application.cpp:1697](src/core/app/Application.cpp)) so the smoother a-priori is unchanged.

---

## 8. Measurement seam — numerics stay home

Two paths, selected by `reparameterizesGnssRows()`.

**Row-contributing (ZUPT, NHC, wheel, baro, vision-as-position)** append *independent* rows through a **new public sink** on `MeasurementModel`, `appendAidingRow(EpochMeasurement&, const AidingMeasurementRow&)`, which keeps sources OFF the friend list (the tight three-name BSSD/DD grant is preserved). It grows innovations/R_v and H (rows+1, cols == state_size) and push_backs label/signal into `satellite_ids`/`signal_types` so the dimension invariant holds and KF outlier keys never collide; it **HARD-ERRORS if `EpochMeasurement.R_full` is populated** (BSSD/DD active), because a diagonal append after `R_full` is set silently drops SD cross-covariance via the `R_v` fallback ([KalmanFilter.cpp:655-659](src/core/filter/KalmanFilter.cpp)); and it asserts `(label,signal)` is not already present this epoch. Inserted after `differencing_->applyDifferencing` ([MeasurementModel.cpp:1393](src/core/measurement/MeasurementModel.cpp)). `KalmanFilter::update` consumes the rows uniformly — it only requires `H.cols()==state_size` ([KalmanFilter.cpp:648](src/core/filter/KalmanFilter.cpp)). First release restricts row sources to **undifferenced PPP + loosely-coupled GNSS+IMU**; the block-diagonal `R_full` augmentation (`appendAidingRowBlock`) is deferred until a source must coexist with active BSSD/DD (open question #2). The `R_full` case is a **runtime guard**, not a comment.

**Column-contributing (TC-IMU)** re-parameterizes existing GNSS rows — adds Jacobian *columns*, gated on `tightlyCoupledImu() && hasImuBiases()` ([MeasurementModel.cpp:995](src/core/measurement/MeasurementModel.cpp)). This branch stays **byte-identical**: `buildJacobianRow` partials, `setImuState` and the `tc_C_b_e_`/`tc_f_b_` stash ([MeasurementModel.hpp:300](include/measurement/MeasurementModel.hpp)) are unchanged. **Only the caller of `setImuState` moves** — from the two scattered `Application` sites into `aiding_manager_->applyTcJacobianInputs(...)`, which asks the typed `imu_` handle for `C = getAttitude()` and `f_b = Cᵀ·(−g_e) − accel_bias` (the gravity/specific-force math relocated into `ImuAidingSource`, its natural owner) and calls the unchanged `setImuState`.

---

## 9. The four touch-points it replaces

Line pins are **pre-migration**; §16 item 3 maps them to current sites.

| # | Today | Becomes |
|---|-------|---------|
| **1** predict | `if (imu_integrator_ && hasData() && !first_epoch_) predictImu(...); else predict(dt);` ([Application.cpp:1682](src/core/app/Application.cpp)) | `auto c = aiding_manager_->collectPredict(ctx); if (!c.empty()) kf_->predictAided(dt,c); else kf_->predict(dt);` |
| **2** TC Jacobian (filter loop) | extract bias, gravity, `C=getAttitude()`, `f_b=…`, `setImuState(C,f_b)` ([Application.cpp:711](src/core/app/Application.cpp)) | `aiding_manager_->applyTcJacobianInputs(*meas_model_,*kf_)` (same arithmetic, relocated; gated on `imu_ && tightlyCoupledImu()`) |
| **3** TC Jacobian (naive IEKF) | duplicate `setImuState(C,f_b)` ([Application.cpp:1058](src/core/app/Application.cpp)) | same `applyTcJacobianInputs(...)` (de-duplicates sites 2&3); naive-vs-proper dispatch ([Application.cpp:924](src/core/app/Application.cpp)) keys off `anySourceRequiresNaiveRelinearization()` — **true exactly when `tightlyCoupledImu()`**, so routing is unchanged |
| **4** construction | `if (enableImu()) { imu_integrator_ = make_unique<ImuIntegrator>(...); loadData(); }` + member ([Application.cpp:1422](src/core/app/Application.cpp), [Application.hpp:440](include/app/Application.hpp)) | `aiding_manager_ = make_unique<AidingSourceManager>(config_,*log_)`; `ImuAidingSource` owns+loads its integrator; load failure ⇒ `hasData()==false` ⇒ silent GNSS-only fallback |
| **bonus** gap/dynamics | `enableImu()` in `classifyDynamicsMode` ([Application.cpp:389](src/core/app/Application.cpp)), `handleGap` ([Application.cpp:419](src/core/app/Application.cpp)), MW gap tolerance ([Application.cpp:1394](src/core/app/Application.cpp)) | `aiding_manager_->anySourceBridgesGaps()` (IMU yes, baro/ZUPT no) |

`RealTimeEngine` gains the **same** `aiding_manager_` member, the **same** `predictAided` branch at
[RealTimeEngine.cpp:763](src/core/realtime/RealTimeEngine.cpp), and `applyTcJacobianInputs` before
[RealTimeEngine.cpp:1095](src/core/realtime/RealTimeEngine.cpp).

---

## 10. IMU migration plan (numerically inert until proven)

| Step | Action | Gate |
|------|--------|------|
| 0 | Land `AidingTypes`/`IAidingSource`/`AidingSourceRegistry`/`ImuAidingSource`/`AidingSourceManager` headers+cpp; `AidingConfig` (`imu` defaults to `enableImu()`); register only `Kind::Imu`; add cpps to CMake. Nothing calls the manager. | `--target gnss_core` builds & links |
| 1 | `ImuAidingSource` **wraps** (not replaces) `ImuIntegrator`; ctor loads from `getImuConfig().imu_file_path`; `reserveStates`→`describeCoreBlock`; `contributePredict` maps Φ/Q via ins_map, sets `dt_total`, coupling-gated `requires_naive_relinearization`; `asImu()` returns `this`; exposes TC inputs + attitude. | unit-test the Φ/Q projection in isolation (§11/§13) |
| 2 | Add `KalmanFilter::predictAided` — empty⇒`predict(dt)`, INS⇒`predictImu` kernel verbatim (Identity seed, Q from `dt_total`). Keep `predictImu` in place. *(⚠ keep-in-place was completed then reversed — §16)* | **Golden test: P AND x bit-identical** to legacy `predictImu` over the same interval/state, across (a) a clock+clock_rate config and (b) a BSSD config *(⚠ realized as a trace-diff gate, not a unit test — §16)* |
| 3 | Wire `AidingSourceManager` into `Application` — replace touch-points 1–4. | builds |
| 4 | **Regression gate (no numeric test suite — CLAUDE.md):** user runs the IMU PPP config and the GNSS-only config; diff position/clock/ambiguity traces vs pre-migration baselines. GNSS-only **must** be bit-identical; TC-IMU **must** be bit-identical. | user trace inspection |
| 5 | Wire the **same** manager into `RealTimeEngine` (`predictAided` at :763, `applyTcJacobianInputs` before :1095) + `feedImuSample` streaming ingest in the `ApplicationRealtime` driver loop ([ApplicationRealtime.cpp:355](src/core/app/ApplicationRealtime.cpp)). | **Real-time IMU unlock** *(⚠ driver-loop branch NOT wired — §16)* |
| 6 | Add a second client (ZUPT or baro) as a row-contributing source to *prove* composition — register kind, add config bool, implement `contributeMeasurement` + `appendAidingRow`. No KF/StateManager change. | composition demonstrated |

---

## 11. Numerical-invariance argument (corrected)

**GNSS-only — bit-for-bit identical.** `createEnabled` with `imu=false` returns empty `sources_`; `collectPredict` returns empty; dispatch takes `else predict(dt)` — the exact pre-migration call, so `x=F*x`, the BSSD freeze block and the `dt` guard are all inherited (§7). `collectMeasurement` empty ⇒ `appendAidingRow` never called ⇒ `EpochMeasurement` untouched. `StateManager` reserves nothing (core IMU offsets stay gated on `enableImu()`). No matrix is touched. **The crucial correction:** this holds *because* the empty path is a literal delegation to `predict(dt)` — never a reimplementation that could omit `x=F*x` or the BSSD freeze surgery.

**TC-IMU — bit-for-bit identical** because every numeric stays home, per §7 and §8: same Identity-seeded `F`, same baseline `Q = getProcessNoiseMatrix(pred.dt_total)`, same Φ/Q assigned over the same ins_map, same single `P=F·P·Fᵀ+Q`; the `ImuIntegrator::integrate` call is unchanged (same gravity, same `2·Ω_ie` Coriolis, same noise densities) and `ImuAidingSource` only forwards arguments and reshapes the result; TC partials, `setImuState` and the `tc_C_b_e_`/`tc_f_b_` stash are **not moved**, only `setImuState`'s *caller*; naive-vs-proper IEKF dispatch stays keyed on `anySourceRequiresNaiveRelinearization()` = `tightlyCoupledImu()`; contributions apply after `getProcessNoiseMatrix` / before P propagation, and `predictAided` completes before `capturePriorSnapshot`.

**Loosely-coupled IMU also invariant** because `requires_naive_relinearization` is coupling-gated — an LC
config keeps `tightlyCoupledImu()==false` and so keeps the proper prior-anchored IEKF, exactly as today.

---

## 12. Real-time IMU unlock

**Landed as step 5 apart from the driver-loop ingest branch — §16.** Pre-migration, `RealTimeEngine`
ingested GNSS byte frames only (zero IMU references, verified) and called only `predict(dt)` at
[RealTimeEngine.cpp:763](src/core/realtime/RealTimeEngine.cpp). Three additions unlock RT-IMU, none
touching the GNSS frame path:

1. **Shared predict seam** — `predictAided` is consumed by both `Application::run` and `RealTimeEngine::processEpoch`; the engine branches exactly like Application.cpp:1682. `prev_epoch_time_` already exists ([RealTimeEngine.hpp:196](include/realtime/RealTimeEngine.hpp)), so the integration interval needs no new bookkeeping.
2. **Streaming ingest** (the one genuinely new RT capability) — the deliberate split of `ingestSample(AidingSample)` from `contributePredict` exists *because* a real-time IMU is not a pre-loaded CSV: samples arrive asynchronously and buffer into the owned `ImuIntegrator` until the next GNSS epoch boundary defines the interval. `RealTimeEngine::feedImuSample(...)` + an IMU branch in the `ApplicationRealtime` driver alongside `feedSbfFrame`/`feedRtcmFrame`.
3. **State readiness** — the engine runs with velocity + IMU error states enabled (`AidingConfig` + the source's `describeCoreBlock` arrange this); gap policy queries `anySourceBridgesGaps()`.

Decisive point: post-proc and RT share **one** manager and **one** `predictAided`, so the predict/update
divergence the codebase already fought (the reason `EpochUpdateSupport` was factored as a shared helper)
cannot recur for aiding.

---

## 13. Test seam

Follow the verified `XxxTestAccess` free-struct friend-shim idiom — declared under `private:` with a
"Test seam:" comment, **defined only in the test cpp**. The review's key correction: in this tree
`TestAccess` shims exist **only on numeric kernels** (`MlambdaTestAccess`, `WlsEstimatorTestAccess`,
`StochasticModelTestAccess`) — `SlipDetectorRegistry`/`MeasurementBuilderFactory` have **no** shim because
their selection API is already public. So **drop** `AidingSourceRegistryTestAccess`
(`createEnabled`/`create`/`isAvailable`/`availableKinds` are public; the registry tests need no private
access) and **keep** `ImuAidingSourceTestAccess` — `mapPredictionToContribution` (pure Φ/Q projection) and
`computeTcSpecificForce` (relocated gravity/`f_b`) are bona-fide private kernels in the
`WlsEstimator::predictObs` mold, exposed by a free struct in `namespace GnssProcessor` above the anon
namespace in `tests/test_aiding_source.cpp`. ⚠ **AS BUILT the shim set is three, not two** — §16.

Example cases (plain `TEST`/`TEST_F`, hand-listed in the explicit `add_executable(gnss_tests …)`):
registry ordering (`sources[0]->kind()==Imu`) via **public** `createEnabled`; staged-Vision warn-skip via
public `create`; `PredictMapMatchesIntegrator` (placement at mapped indices, zero elsewhere);
`NoSamplesInvalidContribution` (`.valid==false`). Fixture-backed IMU-CSV tests gate on
`GNSS_TEST_DATA_DIR` with `GTEST_SKIP()`.

---

## 14. Open questions — resolutions

Build-shaping forks (resolved 2026-06-12):

1. **Predict baseline F.** ✅ **RESOLVED: keep the Identity seed** when an INS source is present (preserves bit-identity — §7 has the clock-rate-coupling reason). The `getTransitionMatrix(dt)` "refinement" is deferred until a non-INS predict source lands, then adopted as a separately-validated, gated, trace-diffed correction — never as "free".
2. **Aiding rows under BSSD/DD.** ✅ **RESOLVED: defer the `R_full` block** (§8). First release restricts row-contributing sources (ZUPT/baro/NHC) to undifferenced PPP + LC GNSS+IMU; `appendAidingRow` **hard-errors** when `R_full` is populated. `appendAidingRowBlock` (block-diagonal augmentation) is built only when a source must coexist with active BSSD/DD.
6. **Smoother contract for aiding states.** ✅ **RESOLVED: document the exclusion.** Aiding error states stay excluded from RTS smoothing (matching today's IMU — `collectCommonIndices`, [Smoother.cpp:90](src/core/filter/Smoother.cpp), omits att/bias). Acceptance criterion: the smoothed GNSS subspace stays consistent when an aiding source perturbs the forward prior. (Registering resets as `is_discontinuity` segment boundaries is a later option, not v1.)
7. **Config section.** ✅ **RESOLVED: `estimation.aiding.{…}`** (state-vector toggles, matching the `cycle_slip` precedent); `imu` defaults to the existing `enableImu()` value for back-compat.

Tier-2 / deferred forks (recommended defaults stand; revisit when the first variable-cardinality source
lands — none affects the IMU first client). **3. Tier-2 Q/F ownership:** `StateManager`'s builders stay
ignorant of aiding dynamics; sources always *supply* Q/F via the predict hook (IMU precedent), even for
slow random-walk states. **4. Error-state vs total-state sources:** contributions stay error-state-shaped
(Φ/Q over owned indices + optional nominal write-back), with `requires_naive_relinearization` letting a
future total-state source opt into the proper IEKF. **5. IMU core states stay hardwired** (current
`enableImu()` carve-out) rather than migrating to `reserveDynamicBlock` — avoids touching state layout and
the `removeStateAtIndex` invariant for zero benefit.

---

## 15. Adversarial-review log

| Lens | Verdict | Material change to the design |
|------|---------|-------------------------------|
| State-vector discipline | needs-rework | (blocker) Draft relocated nominal `C_b_e_` into the source → reverted: nominal has one off-vector owner, source reads/writes through it. (blocker) Tier-2 `reserveBlock` would interleave with the ambiguity tail → tier-2 deferred; core-offset/stable-tail placement + 3 explicit invariants. (blocker) Smoother silently drops aiding blocks → §14 Q6 contract. (major) `reserveStates` double-duty → split `describeCoreBlock`/`reserveDynamicBlock`. (major) naive-IEKF gate → coupling-mode gated. |
| Numerical invariance | sound-with-fixes | (blocker×3) `getTransitionMatrix` baseline breaks clock-rate coupling; `x=F*x` unspecified; BSSD freeze block omitted → **all resolved by: empty⇒delegate `predict(dt)`, INS⇒`predictImu` kernel verbatim (Identity seed)**. (major) baseline-Q interval must be `dt_total`, not GNSS `dt`. (major) unique row labels + assert. (minor) snapshot `x` once. Golden test upgraded to **P *and* x**. |
| Idiom fidelity | sound-with-fixes | (major) `getTransitionMatrix` "provably invariant" framing removed. (major) **Drop `AidingSourceRegistryTestAccess`** — factories get no shim in this tree. (minor) **Drop `contributesPredict`/`contributesMeasurement` bool predicates** — redundant with empty/invalid return; keep `reparameterizesGnssRows`. (minor) `StateBlockSpec` documented as one-shot receipt. Registry/manager/config/Doxygen fidelity all verified ✓. |

---

## 16. As built (verified in tree 2026-08-14; anchors re-grepped 2026-08-15)

Steps 0-6 of the §10 migration table all landed. Deviations:

**1. `predictImu` was RETIRED from the live path, not kept in place** (§10 step 2 staged "keep
`predictImu` in place"). `include/aiding/ImuAidingSource.hpp:25`: "the contributions implemented here are
now the live path via `predictAided` / `collectPredict`; the legacy `predictImu` is retired." Precisely:
`KalmanFilter::predictImu` is still *declared* (`include/filter/KalmanFilter.hpp:106`) and its kernel is
what `predictAided` reproduces, but nothing on the `Application` or `RealTimeEngine` path calls it.
**Do not delete the declaration on the strength of the word "retired".**

**2. A third test shim exists that §13 never names: `KalmanFilterTestAccess`**, declared under `private:`
at `include/filter/KalmanFilter.hpp:472-476`, defined at `tests/test_aiding_source.cpp:56`. It exposes the
private static assembly kernel `KalmanFilter::spliceAndPropagate` (`KalmanFilter.hpp:606`) because **the
full `KalmanFilter` cannot be unit-constructed** — it needs a `PppModel` + parser chain (see
`test_filter_core.cpp`). Consequence, recorded in that test's own header: **end-to-end
`predictImu`-vs-`predictAided` bit-identity is the Step-4 trace-diff gate, not a unit test** — a real
deviation from §10 step 2's "Golden test: P AND x bit-identical". The kernel test pins the contract
instead: Identity seed, ASSIGNMENT (not `+=`) of the contribution Q, nominal write-backs, sentinel
skipping, symmetrization.

**3. Anchor drift — every line pin in §1/§6-§9/§12 was written against the pre-migration tree.** Current
sites, re-grepped 2026-08-15:

| Design pin | Current | What it is |
|---|---|---|
| Application.cpp:1422 / Application.hpp:440 | `Application.cpp:1643-1644` / `Application.hpp:863` | manager construction + `reserveAllStates`; the member is documented as replacing the bespoke `ImuIntegrator` |
| Application.cpp:711 and :1058 | `Application.cpp:905` and `:1253` | the de-duplicated `applyTcJacobianInputs(*meas_model_)` (touch-points 2 & 3) |
| Application.cpp:924 | `Application.cpp:1118` | `anySourceRequiresNaiveRelinearization()` dispatch |
| — (new) | `Application.cpp:920-928` | `collectMeasurement` + `MeasurementModel::appendAidingRow` |
| RealTimeEngine.cpp:763 | `RealTimeEngine.cpp:459-466` | the `predictAided` branch, integrating over `(ctx.aiding_t_prev, ctx.current_time]` |
| RealTimeEngine.hpp:196 | `:676` (`prev_epoch_time_`), `:457` (`ctx.aiding_t_prev`), `:208` (`feedImuSample` decl; defined `RealTimeEngine.cpp:321`) | RT timing / ingest surface |
| StateManager.cpp:287 / :382 | `:292` / `:387` | the `enableImu()` 9-state carve-out and its P-seed |
| StateManager.cpp:482 / :756 | `:486` / `:815` | `getStateSize()` / `getTransitionMatrix` (the `clock ← clock_rate` coupling) |
| StateManager.cpp:114 / :160 | unchanged | `appendState` / the `removeStateAtIndex` decrement loop |
| KalmanFilter.hpp:431 | `:490` | the `C_b_e_` member (accessors at `:145`/`:151`) |
| KalmanFilter.cpp:542 | `:550` (nominal write-back after `integrate`), `:1118-1122` (attitude-error reset + re-orthonormalize) | the attitude reset the KF still needs `C_b_e_` for |
| KalmanFilter.cpp:559-588 / :462-484 | `:571+` / unchanged | the ins_map injection / the BSSD freeze-state surgery |
| KalmanFilter.cpp:648 / :655-659 | `:733` / `:740-742` and `:1185-1186` | the `H.cols() != state_size` guard / the `R_full`→`R_` adoption whose absence is the `R_v` fallback |
| MeasurementModel.cpp:1393 / :995 | `:2113` / `:1388` | the `applyDifferencing` insertion point / the `tightlyCoupledImu() && hasImuBiases()` TC gate |
| MeasurementModel.hpp:300 | `:481` | the `tc_C_b_e_` stash |
| GnssConfigLoad.cpp:556 | `:979` (`cycle_slip.ifp` ← `estimation.cycle_slip.if`), `:2491-2509` (the aiding block) | the back-compat-default precedent and the shipped config site |
| Smoother.cpp:90 | `:78` | `collectCommonIndices` (att/bias exclusion) |

**4. Step 6 landed as `ZuptAidingSource`** (`include/aiding/ZuptAidingSource.hpp`,
`src/core/aiding/ZuptAidingSource.cpp`) — the row-contributing second client, proving composition with no
KF/StateManager change. It carries a knob the design never named: `estimation.aiding.zupt_sigma_mps`.

**5. Two shipped-shape deviations from §8 and §12.** (a) `MeasurementModel::appendAidingRow` is
**`static`** (`MeasurementModel.hpp:286`, defined at `MeasurementModel.cpp:135`), not the `const` member
§8 sketched; both hard-errors it specifies are live (`R_full` populated at `:142`, jacobian/state-dimension
mismatch at `:153`). (b) ⚠ **§12 item 2's driver-loop branch does NOT exist.**
`RealTimeEngine::feedImuSample` is public and implemented, but `grep -rn feedImuSample src/ include/`
returns no caller in `ApplicationRealtime.cpp` (only the definition, the `:460` comment and a
`RealTimeEngineInit.cpp:188` reference). RT-IMU is unlocked at the engine API, **not** end-to-end from the
driver — read §12 as design intent for that last hop. Trap for whoever wires it: `ctx.aiding_t_prev` lives
in `RtEpochContext` (`RealTimeEngine.hpp:457`), and CLAUDE.md's RT seam contract warns that "a value that
must survive to the next epoch is a member, and putting it in the context silently cold-starts it every
epoch" — it is re-seeded per epoch from `prev_epoch_time_` by design, so do not treat the context slot as
the carrier.

**Design decisions verified unchanged:** `C_b_e_` stayed the KF's; tier-2 is still deferred (no
`makeAidingBlockKey` in `StateKeySchema`); the `getTransitionMatrix` baseline was NOT adopted.

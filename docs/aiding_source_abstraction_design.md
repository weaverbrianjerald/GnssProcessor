# Aiding-source / `IAidingSource` abstraction — design

**Status:** proposal, awaiting review — *no code yet.*
**Roadmap item:** nav-readiness #1 (measurement-source / aiding abstraction).
**Date:** 2026-06-12.

> Grounded in a code-level map of the four current IMU touch-points and the two
> idioms it mirrors, then put through a three-lens adversarial review
> (state-vector discipline, numerical invariance, idiom fidelity). The
> "Adversarial-review log" at the end records what the review changed; the body
> below is the **corrected** design, not the first draft.

---

## 1. Goal

An *aiding source* is anything that contributes navigation information beyond
GNSS pseudorange / phase / Doppler — IMU, ZUPT, NHC, wheel odometry, baro
height, vision. Today the IMU is welded into four sites; adding any modality is
four-file surgery:

| # | Site | File:line |
|---|------|-----------|
| 1 | predict branch | [Application.cpp:1682](src/core/app/Application.cpp) |
| 2 | TC update Jacobian (filter loop) | [Application.cpp:711](src/core/app/Application.cpp) |
| 3 | TC update Jacobian (naive IEKF relin) | [Application.cpp:1058](src/core/app/Application.cpp) |
| 4 | integrator construction / lifecycle | [Application.cpp:1422](src/core/app/Application.cpp) + member [Application.hpp:440](include/app/Application.hpp) |

Plus the latent `enableImu()` queries in `classifyDynamicsMode` / `handleGap` /
the MW gap tolerance.

The fix is an interface where each source **composes** by contributing three
things, each at the exact seam where the IMU injects that thing today:

- **(a) state** — its own block, owned *through* `StateManager` (state-vector
  discipline: no sibling state owners);
- **(b) predict** — a Φ/Q sub-block (or mechanization hook);
- **(c) measurement** — rows into the update (or column re-parameterization).

When no source is registered, every seam degrades to today's GNSS-only call, so
the GNSS-only path is **byte-identical**.

---

## 2. Idioms mirrored (verified against the tree)

1. **`ISlipDetector` + `SlipDetectorRegistry` + `CycleSlipManager`** — pure-virtual
   contract; an enum (`SlipDetectorKind`) that triples as registry key /
   provenance tag / config token; a stateless Meyers-singleton creator table;
   the owner holds `vector<unique_ptr<…>>` **plus one typed handle** for the
   privileged member recovered by a no-RTTI down-cast
   (`CycleSlipManager::mw_` via `asMelbourneWubbena()`,
   [CycleSlipManager.hpp:368](include/slip/CycleSlipManager.hpp)).
2. **`MeasurementBuilder` strategy** — thin stateless orchestrators receive the
   host `MeasurementModel&` and call back into its **private** numerics via a
   *tight* `friend` list; the delicate arithmetic **never leaves the host**.

The aiding layer maps onto these one-for-one:

```
IAidingSource          ← ISlipDetector            (polymorphic contract)
AidingSourceRegistry   ← SlipDetectorRegistry     (Meyers creator table, config-driven)
AidingSourceManager    ← CycleSlipManager         (owns vector<> + typed ImuAidingSource* imu_)
appendAidingRow sink   ← MeasurementBuilder friend principle (numerics stay home)
```

---

## 3. The interface

```cpp
// include/aiding/IAidingSource.hpp
#pragma once
/**
 * @file IAidingSource.hpp
 * @brief Polymorphic contract shared by every navigation aiding source.
 *
 * A source contributes up to three things, each at the seam where the IMU
 * injects that thing today:
 *   - STATE: it NAMES (tier-1) or RESERVES (tier-2) its block THROUGH
 *     StateManager — never a sibling owner. Error-state sources keep their
 *     NOMINAL state off-vector but in the designated single owner (the KF's
 *     C_b_e_), never inside the source.
 *   - PREDICT: it returns a Phi/Q sub-block + optional nominal write-back that
 *     KalmanFilter splices over the StateManager baseline (assignment, not +=).
 *   - MEASUREMENT: it appends independent rows (H/z/R), OR re-parameterizes
 *     existing GNSS rows via extra Jacobian columns (the TC-IMU pattern).
 *
 * Mirrors ISlipDetector: uniform (const GnssConfig&, LogManager&) ctor so one
 * registry Creator works; narrow base; rich source-specific API kept OFF the
 * base and recovered via a no-RTTI typed down-cast (asImu()). A source opts out
 * of a phase by the NATURAL empty/invalid return — no separate bool predicate
 * (the same signal the no-source fast path already keys on).
 */
#include "aiding/AidingTypes.hpp"
#include "logging/LogManager.hpp"
#include <Eigen/Core>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

class StateManager;     // state reservation host (single layout authority)
class MeasurementModel; // measurement row sink host
class ImuAidingSource;  // typed down-cast target (the privileged source)

class IAidingSource {
public:
    virtual ~IAidingSource() = default;

    // ---- identity --------------------------------------------------------
    /// @brief Modality tag (registry key / provenance tag / config token).
    virtual AidingSourceKind kind() const = 0;
    /// @brief Human-readable name (logs/traces).
    virtual std::string name() const = 0;

    // ---- (a) STATE -------------------------------------------------------
    /**
     * @brief Establish this source's state block THROUGH StateManager.
     *
     * Tier-1 sources (IMU core error states; ZUPT/NHC/baro own NO new states)
     * DESCRIBE pre-existing core offsets — no allocation, no P-init (the core
     * path already seeded P). Tier-2 sources (variable cardinality, e.g.
     * per-landmark vision) RESERVE+seed via StateManager::reserveDynamicBlock.
     * The returned StateBlockSpec is a one-shot RECEIPT, not a durable index
     * cache: per-epoch index resolution MUST go through a LIVE StateManager
     * lookup (getBlockRange / getAttitudeIndex-style getters), because
     * removeStateAtIndex shifts block indices on every lower prune.
     *
     * @param sm The single source of truth for state layout.
     * @return The block descriptor, or std::nullopt if the source owns none.
     */
    virtual std::optional<StateBlockSpec> reserveStates(StateManager& sm) = 0;

    // ---- (b) PREDICT -----------------------------------------------------
    /**
     * @brief Predict-time contribution over [ctx.t_prev, ctx.t_now].
     *
     * Returns the FULL-state indices it owns (resolved live from StateManager),
     * a square Phi/Q sub-block over those indices (KalmanFilter ASSIGNS, never
     * accumulates), and an optional nominal segment to write into x. Default
     * returns .valid==false, which KalmanFilter treats as "skip me" — leaving
     * the predict path byte-identical to GNSS-only for measurement-only sources.
     */
    virtual PredictContribution contributePredict(const AidingEpochContext& ctx) {
        (void)ctx; return PredictContribution{};  // .valid defaults false
    }

    // ---- (c) MEASUREMENT -------------------------------------------------
    /**
     * @brief Independent measurement rows for this epoch (H/z/R + label).
     *
     * Each row's jacobian is over the full state and writes ONLY into
     * StateManager-owned columns. Labels MUST be unique within the epoch
     * (e.g. ZUPT_vx / ZUPT_vy / ZUPT_vz) so KF outlier/exclusion keys never
     * collide. Default empty — a predict-only source leaves the update untouched.
     */
    virtual MeasurementRows contributeMeasurement(const AidingEpochContext& ctx) {
        (void)ctx; return MeasurementRows{};
    }

    /**
     * @brief True if this source RE-PARAMETERIZES existing GNSS rows (adds
     *        Jacobian COLUMNS) instead of appending its own rows.
     *
     * The one capability that cannot be inferred from an empty return — it
     * selects the column-write seam (TC-IMU) over the row-append seam.
     */
    virtual bool reparameterizesGnssRows() const { return false; }

    // ---- lifecycle -------------------------------------------------------
    /// @brief Ingest one streaming sample (real-time push); no-op in replay.
    virtual void ingestSample(const AidingSample& sample) { (void)sample; }
    /// @brief Whether data is loaded/buffered (mirrors ImuIntegrator::hasData()).
    virtual bool hasData() const = 0;
    /// @brief Whether this source bridges full GNSS outages (INS dead-reckon).
    virtual bool bridgesGaps() const { return false; }
    /// @brief Soft reset (decorrelate + reinflate owned block) on dropout/reacquire.
    virtual void reset() {}
    /// @brief Enable per-source CSV tracing.
    virtual bool enableTracing(const std::filesystem::path& log_dir) { (void)log_dir; return false; }
    virtual bool isTracingEnabled() const { return false; }

    /**
     * @brief Typed access to the IMU source (this for ImuAidingSource, else
     *        nullptr) — recovers the rich API (TC Jacobian inputs, attitude)
     *        without RTTI. Exactly ISlipDetector::asMelbourneWubbena().
     */
    virtual ImuAidingSource* asImu() { return nullptr; }
};

} // namespace GnssProcessor
```

**What changed vs. the draft:** dropped the `contributesPredict()` /
`contributesMeasurement()` bool predicates — they were redundant with the
`.valid==false` / empty-vector signals the no-source fast path already keys on,
and `ISlipDetector` expresses "I don't participate" the same way (empty
`detect()`), so the base now matches that minimalism. `reparameterizesGnssRows()`
stays — it routes to a *different seam* and can't be inferred from a return value.

---

## 4. Value types

```cpp
// include/aiding/AidingTypes.hpp (excerpt)
enum class AidingSourceKind { Imu, Zupt, Nhc, WheelOdom, Baro, Vision };
std::string                    toString(AidingSourceKind);          // "imu","zupt",…
std::optional<AidingSourceKind> aidingKindFromString(const std::string&);

/// One-shot reservation RECEIPT (NOT a durable index cache — see note).
struct StateBlockSpec {
    AidingSourceKind owner{AidingSourceKind::Imu};
    std::string      key_prefix;     ///< e.g. "AID|IMU" (tier-2 only)
    size_t           start_index{0}; ///< snapshot — re-resolve live each epoch
    size_t           size{0};
    std::vector<std::string> layout; ///< per-state labels (size == .size)
};

/// Generalizes the IMU ins_map injection (KalmanFilter.cpp:559-588).
struct PredictContribution {
    AidingSourceKind     source{AidingSourceKind::Imu};
    std::vector<size_t>  indices;          ///< full-state indices owned (the ins_map)
    Eigen::MatrixXd      Phi;              ///< n×n over indices (n==indices.size())
    Eigen::MatrixXd      Q;               ///< n×n process noise over indices
    std::vector<size_t>  nominal_indices; ///< full-state indices to overwrite in x
    Eigen::VectorXd      nominal_values;
    double               dt_total{0.0};    ///< INTEGRATED span — drives baseline Q interval
    size_t               num_samples{0};
    bool                 requires_naive_relinearization{false}; ///< set ONLY for TC coupling
    bool                 valid{false};     ///< false => KF skips this contribution
};

struct AidingMeasurementRow {
    Eigen::VectorXd jacobian;   ///< (state_size) design-matrix row
    double          innovation{0.0};
    double          variance{0.0};
    std::string     label;      ///< UNIQUE within epoch (e.g. "ZUPT_vx")
    std::string     signal;     ///< secondary token (e.g. "AID")
};
using MeasurementRows = std::vector<AidingMeasurementRow>;

struct AidingSample {           ///< streaming push (real-time)
    AidingSourceKind kind{AidingSourceKind::Imu};
    GnssTime time;
    Eigen::Vector3d accel{Eigen::Vector3d::Zero()};
    Eigen::Vector3d gyro{Eigen::Vector3d::Zero()};
    double scalar{0.0};         ///< baro height / wheel speed (by kind)
};

struct AidingEpochContext {     ///< handed uniformly to contribute* (mirrors SlipEpochContext)
    GnssTime t_prev, t_now;
    const Eigen::VectorXd& state;   ///< current nominal x
    size_t   state_size{0};         ///< == H cols
    bool     first_epoch{false};    ///< forces GNSS-only predict on epoch 0
    explicit AidingEpochContext(const Eigen::VectorXd& x)
        : state(x), state_size(static_cast<size_t>(x.size())) {}
};
```

---

## 5. Registry + manager

`AidingSourceRegistry` is a stateless static factory over a process-wide Meyers
creator table — a near-exact copy of `SlipDetectorRegistry`:

```cpp
class AidingSourceRegistry {
public:
    using Creator = std::function<std::unique_ptr<IAidingSource>(
        const GnssConfig&, LogManager&)>;
    static std::unique_ptr<IAidingSource> create(AidingSourceKind, const GnssConfig&, LogManager&);
    static bool                            isAvailable(AidingSourceKind);
    static std::vector<AidingSourceKind>   availableKinds();
    /// Build the config-selected set, IMU FIRST (the privileged primary), then
    /// Zupt/Nhc/WheelOdom/Baro/Vision. Enabled-but-unavailable (staged Vision)
    /// is warn-and-skipped — exactly the staged GF/IF slip precedent.
    static std::vector<std::unique_ptr<IAidingSource>> createEnabled(const GnssConfig&, LogManager&);
private:
    static const std::map<AidingSourceKind, Creator>& creators(); // Meyers singleton
    // NO TestAccess friend here — see §10.
};
```

`AidingSourceManager` is the `CycleSlipManager` analog, owned by **both**
`Application` and `RealTimeEngine`:

```cpp
std::vector<std::unique_ptr<IAidingSource>> sources_; // owns the herd
ImuAidingSource* imu_{nullptr};                       // non-owning typed handle
// ctor: sources_ = AidingSourceRegistry::createEnabled(cfg, log);
//       for (auto& s : sources_) if (auto* i = s->asImu()) { imu_ = i; break; }
```

Lifecycle (`reset` / `ingestSample`) fans out polymorphically; the rich IMU-only
calls (TC Jacobian inputs, attitude) route through the null-guarded `imu_`
handle — the exact "polymorphic herd + one typed handle" split
`CycleSlipManager` uses for `mw_`.

**Config** (house convention, `estimation` section because these add
state-vector error states):
`estimation.aiding.{imu,zupt,nhc,wheel,baro,vision}` →
`GnssConfig::getAidingConfig()`, with
`imu = jval<bool>(json,"estimation.aiding.imu","aiding_imu", enableImu())`
(back-compat default sourced from the existing flag, mirroring `cycle_slip.ifp`
defaulting to `enable_if_slip_detector_`, [GnssConfigLoad.cpp:556](src/core/config/GnssConfigLoad.cpp)).

---

## 6. State seam — `StateManager` stays the single layout authority

**The nominal attitude has exactly one owner.** Error-state filters split
*nominal* (off-vector) from *error* (in-vector) state. The state vector holds the
3-state attitude **error** `dpsi`; the nominal body→ECEF rotation it perturbs is
the 3×3 `C_b_e_`, held off-vector as a `KalmanFilter` member
([KalmanFilter.hpp:431](include/filter/KalmanFilter.hpp), via
`getAttitude()`/`setAttitude()`). **The source must NOT relocate `C_b_e_` into
itself** — that is the sibling-state-owner anti-pattern CLAUDE.md forbids, and
the KF still needs `C_b_e_` for the attitude-error reset (KalmanFilter.cpp:542)
and `buildJacobianRow`. `ImuAidingSource` *reads/writes* the nominal through its
single owner (the KF today; optionally promote to an explicit
`StateManager::getNominalAttitude/setNominalAttitude` later), exactly as it reads
`getAttitudeIndex()` rather than holding the error states.

**Tier-1 (IMU, the first client): no new state, no new API.** The 9 IMU error
states (att/accel-bias/gyro-bias) are already carved into `core_state_size_`
under the `enableImu()` gate ([StateManager.cpp:287](src/core/filter/StateManager.cpp)),
P-seeded from `ImuConfig` ([StateManager.cpp:382](src/core/filter/StateManager.cpp)).
`ImuAidingSource::reserveStates` *describes* those offsets and reserves nothing —
**bit-for-bit identical state layout.** To remove the draft's "9-state spec that
allocates nothing" ambiguity, `reserveStates` is contractually two flavors:

- tier-1 → `describeCoreBlock(sm)`: names pre-existing offsets, no alloc/seed;
- tier-2 → `reserveDynamicBlock(sm)`: allocates + supplies `init_vals`/`init_var_diag`.

**Tier-2 (variable cardinality — vision landmarks, multi-anchor) is DEFERRED**
and, when it lands, must **not** naively append into the churning ambiguity tail.
`appendState` always appends at `getStateSize()`
([StateManager.cpp:114](src/core/filter/StateManager.cpp)), so a tier-2 block
placed there interleaves with the ambiguity/iono/bias maps that grow and shrink
every epoch. The state-discipline review's verdict: allocate tier-2 blocks as
**core offsets** under a config gate (like IMU), *or* at a stable tail **below
all GNSS dynamic states** with a hard contiguity invariant. Either way, three
invariants become explicit acceptance criteria of the tier-2 milestone (not the
IMU milestone):

1. extend the `removeStateAtIndex` decrement loop
   ([StateManager.cpp:160](src/core/filter/StateManager.cpp)) — today it fixes
   only the four existing maps — to shift `aiding_block_indices_` ranges;
2. extend `getStateSize()` ([StateManager.cpp:482](src/core/filter/StateManager.cpp))
   to add summed aiding-block sizes;
3. per-epoch indices resolved **live** via `getBlockRange(key_prefix)`, never a
   cached `StateBlockSpec.start_index`.

The `StateKeySchema` `makeAidingBlockKey`/`parseAidingBlockKey` addition
(`"AID|IMU|…"`, anchored on a leading `AID` tag so it can't be mis-tokenized
against sat-bias/DD pipe-keys) is **deferred to the first tier-2 source** — the
IMU client adds zero keys.

---

## 7. Predict seam — invariance by *delegation*, not reimplementation

This is where the adversarial review changed the design most. The core rule:

```cpp
/**
 * @brief GNSS predict augmented by aiding-source predict contributions.
 * @param dt            GNSS interval.
 * @param contributions Predict contributions this epoch (may be empty).
 */
void KalmanFilter::predictAided(double dt, const std::vector<PredictContribution>& contributions);
```

- **Empty contributions ⇒ literal delegation.** `if (contributions.empty()) {
  predict(dt); return; }`. This inherits `x = F*x`, the BSSD freeze-state
  surgery ([KalmanFilter.cpp:462-484](src/core/filter/KalmanFilter.cpp)), and the
  `dt<=0` guard **for free and byte-for-byte**. The no-source path is the exact
  pre-migration call — no reimplementation of `predict()`'s body inside
  `predictAided`.
- **INS contribution present ⇒ the `predictImu` kernel verbatim.** Keep the
  **Identity seed** for `F` (so non-INS state *means* — clock, clock-rate, tropo,
  ambiguities — are **not** advanced, matching `predictImu` today), Q from
  **`pred.dt_total`** (the integrated IMU span, *not* the GNSS `dt`), assignment
  (`=`, not `+=`) over the ins_map, single `P = F·P·Fᵀ + Q`, then the attitude
  reset. **Do not** switch the baseline to `getTransitionMatrix(dt)`.

Why the baseline switch was rejected (two lenses converged here):
`getTransitionMatrix` carries the `clock ← clock_rate` coupling at
[StateManager.cpp:756](src/core/filter/StateManager.cpp). The INS ins_map only
touches pos/vel/att/ba/bg — never the clock or clock-rate rows — so seeding `F`
from `getTransitionMatrix` would *start* propagating the clock mean via `x=F*x`,
which `predictImu`'s Identity seed does **not** do today. That is a behavior
change for any clock-rate-estimating TC-IMU config, not a free refinement. The
`getTransitionMatrix` "refinement" (open question #1) is **deferred** until a
non-INS predict-contributing source actually lands, at which point it is adopted
as a deliberately validated correction, gated and trace-diffed.

`ImuAidingSource::contributePredict` is a thin wrapper over its owned
`ImuIntegrator::integrate(...)`: map the returned 15×15 Φ/Q onto the ins_map
(pos/vel core offsets + the 9 IMU offsets), set `dt_total` from the integrator,
set `requires_naive_relinearization = config_.tightlyCoupledImu()` (**coupling-mode
gated** — a *loosely*-coupled IMU keeps the proper prior-anchored IEKF, see §8
touch-point 3), `valid = pred.valid`. On `!pred.valid` the contribution is
dropped and (since the IMU was the only contributor) `predictAided` falls back to
`predict(dt)` — identical to `predictImu`'s fallback.

**Ordering / multi-source safety:** `predictAided` snapshots `x` once at entry and
applies all nominal write-backs against the snapshot, committing once — so a later
contribution can't read an earlier one's nominal write (removes float-order
coupling). Disjoint owned-index sets are asserted (debug); contributions may not
read another's nominal write. `predictAided` completes before
`capturePriorSnapshot` ([Application.cpp:1697](src/core/app/Application.cpp)) so
the smoother a-priori is unchanged.

---

## 8. Measurement seam — numerics stay home

Two paths, selected by `reparameterizesGnssRows()`:

**Row-contributing (ZUPT, NHC, wheel, baro, vision-as-position)** append
*independent* rows through a **new public sink** on `MeasurementModel` — this
keeps sources OFF the friend list (the tight three-name BSSD/DD grant is
preserved):

```cpp
/**
 * @brief Append one independent aiding row into a finalized EpochMeasurement.
 *
 * Grows innovations/R_v and H (rows+1, cols == state_size), push_backs
 * label/signal into satellite_ids/signal_types so the dimension invariant
 * holds and KF outlier keys never collide. HARD-ERRORS if EpochMeasurement.R_full
 * is populated (BSSD/DD active): a diagonal append after R_full is set silently
 * drops SD cross-covariance via the R_v fallback (KalmanFilter.cpp:655-659).
 * Asserts (label,signal) is not already present this epoch.
 */
void appendAidingRow(EpochMeasurement& update, const AidingMeasurementRow& row) const;
```

Inserted after `differencing_->applyDifferencing`
([MeasurementModel.cpp:1393](src/core/measurement/MeasurementModel.cpp)).
`KalmanFilter::update` consumes the rows uniformly (it only requires
`H.cols()==state_size`, [KalmanFilter.cpp:648](src/core/filter/KalmanFilter.cpp)).
First release restricts row sources to **undifferenced PPP + loosely-coupled
GNSS+IMU**; the block-diagonal `R_full` augmentation (`appendAidingRowBlock`) is
deferred until a source must coexist with active BSSD/DD (open question #2). The
R_full case is a **runtime guard**, not a comment.

**Column-contributing (TC-IMU)** re-parameterizes existing GNSS rows — adds
Jacobian *columns*, gated on `tightlyCoupledImu() && hasImuBiases()`
([MeasurementModel.cpp:995](src/core/measurement/MeasurementModel.cpp)). This
branch stays **byte-identical**: `buildJacobianRow` partials, `setImuState`, and
the `tc_C_b_e_`/`tc_f_b_` stash ([MeasurementModel.hpp:300](include/measurement/MeasurementModel.hpp))
are unchanged. **Only the caller of `setImuState` moves** — from the two scattered
`Application` sites into `aiding_manager_->applyTcJacobianInputs(*meas_model_,
*kalman_filter_)`, which asks the typed `imu_` handle for `C = getAttitude()` and
`f_b = Cᵀ·(−g_e) − accel_bias` (the gravity/specific-force math relocated into
`ImuAidingSource`, its natural owner) and calls the unchanged `setImuState`.

---

## 9. The four touch-points it replaces

| # | Today | Becomes |
|---|-------|---------|
| **1** predict | `if (imu_integrator_ && hasData() && !first_epoch_) predictImu(...); else predict(dt);` ([Application.cpp:1682](src/core/app/Application.cpp)) | `auto c = aiding_manager_->collectPredict(ctx); if (!c.empty()) kf_->predictAided(dt,c); else kf_->predict(dt);` |
| **2** TC Jacobian (filter loop) | extract bias, gravity, `C=getAttitude()`, `f_b=…`, `setImuState(C,f_b)` ([Application.cpp:711](src/core/app/Application.cpp)) | `aiding_manager_->applyTcJacobianInputs(*meas_model_,*kf_)` (same arithmetic, relocated; gated on `imu_ && tightlyCoupledImu()`) |
| **3** TC Jacobian (naive IEKF) | duplicate `setImuState(C,f_b)` ([Application.cpp:1058](src/core/app/Application.cpp)) | same `applyTcJacobianInputs(...)` (de-duplicates sites 2&3); naive-vs-proper dispatch ([Application.cpp:924](src/core/app/Application.cpp)) keys off `aiding_manager_->anySourceRequiresNaiveRelinearization()` — **true exactly when `tightlyCoupledImu()`** so routing is unchanged |
| **4** construction | `if (enableImu()) { imu_integrator_ = make_unique<ImuIntegrator>(...); loadData(); }` + member ([Application.cpp:1422](src/core/app/Application.cpp), [Application.hpp:440](include/app/Application.hpp)) | `aiding_manager_ = make_unique<AidingSourceManager>(config_,*log_)`; `ImuAidingSource` owns+loads its integrator; load failure ⇒ `hasData()==false` ⇒ silent GNSS-only fallback (existing graceful behavior) |
| **bonus** gap/dynamics | `enableImu()` in `classifyDynamicsMode` ([Application.cpp:389](src/core/app/Application.cpp)), `handleGap` ([Application.cpp:419](src/core/app/Application.cpp)), MW gap tolerance ([Application.cpp:1394](src/core/app/Application.cpp)) | `aiding_manager_->anySourceBridgesGaps()` (IMU yes, baro/ZUPT no) |

`RealTimeEngine` gains the **same** `aiding_manager_` member, the **same**
`predictAided` branch at [RealTimeEngine.cpp:763](src/core/realtime/RealTimeEngine.cpp),
and `applyTcJacobianInputs` before [RealTimeEngine.cpp:1095](src/core/realtime/RealTimeEngine.cpp).

---

## 10. IMU migration plan (numerically inert until proven)

| Step | Action | Gate |
|------|--------|------|
| 0 | Land `AidingTypes`/`IAidingSource`/`AidingSourceRegistry`/`ImuAidingSource`/`AidingSourceManager` headers+cpp; `AidingConfig` (`imu` defaults to `enableImu()`); register only `Kind::Imu`; add cpps to CMake. Nothing calls the manager. | `--target gnss_core` builds & links |
| 1 | `ImuAidingSource` **wraps** (not replaces) `ImuIntegrator`; ctor loads from `getImuConfig().imu_file_path`; `reserveStates`→`describeCoreBlock`; `contributePredict` maps Φ/Q via ins_map, sets `dt_total`, coupling-gated `requires_naive_relinearization`; `asImu()` returns `this`; exposes TC inputs + attitude. | Unit-test the Φ/Q projection in isolation (see §11) |
| 2 | Add `KalmanFilter::predictAided` — empty⇒`predict(dt)`, INS⇒`predictImu` kernel verbatim (Identity seed, Q from `dt_total`). Keep `predictImu` in place. | **Golden test: P AND x bit-identical** to legacy `predictImu` over the same interval/state, across (a) a clock+clock_rate config and (b) a BSSD config |
| 3 | Wire `AidingSourceManager` into `Application` — replace touch-points 1–4. | builds |
| 4 | **Regression gate (no numeric test suite — CLAUDE.md):** user runs the IMU PPP config and the GNSS-only config; diff position/clock/ambiguity traces vs pre-migration baselines. GNSS-only **must** be bit-identical; TC-IMU **must** be bit-identical. | user trace inspection |
| 5 | Wire the **same** manager into `RealTimeEngine` (`predictAided` at :763, `applyTcJacobianInputs` before :1095) + `feedImuSample` streaming ingest in the `ApplicationRealtime` driver loop ([ApplicationRealtime.cpp:355](src/core/app/ApplicationRealtime.cpp)). | **Real-time IMU unlock** |
| 6 | Add a second client (ZUPT or baro) as a row-contributing source to *prove* composition — register kind, add config bool, implement `contributeMeasurement` + `appendAidingRow`. No KF/StateManager change. | composition demonstrated |

---

## 11. Numerical-invariance argument (corrected)

**GNSS-only — bit-for-bit identical.** `createEnabled` with `imu=false` returns
empty `sources_`; `collectPredict` returns empty; dispatch takes
`else predict(dt)` — the exact pre-migration call (so `x=F*x`, the BSSD freeze
block, and the `dt` guard are all inherited). `collectMeasurement` empty ⇒
`appendAidingRow` never called ⇒ `EpochMeasurement` untouched. `StateManager`
reserves nothing (core IMU offsets stay gated on `enableImu()`). No matrix is
touched. **The crucial correction:** this holds *because* the empty path is a
literal delegation to `predict(dt)` — never a reimplementation that could omit
`x=F*x` or the BSSD freeze surgery.

**TC-IMU — bit-for-bit identical** because every numeric stays in its home:
- *predict:* same Identity-seeded `F`, same baseline `Q = getProcessNoiseMatrix(pred.dt_total)`,
  same Φ/Q assigned over the same ins_map, same single `P=F·P·Fᵀ+Q`. The
  `ImuIntegrator::integrate` call is unchanged (same gravity, same `2·Ω_ie`
  Coriolis, same noise densities). `ImuAidingSource` only forwards arguments and
  re-shapes the result.
- *measurement:* TC partials in `buildJacobianRow`, `setImuState`, and the
  `tc_C_b_e_`/`tc_f_b_` stash are **not moved**; only the *caller* of
  `setImuState` relocates, computing the same `C` and `f_b`.
- *IEKF dispatch:* naive-vs-proper preserved, keyed on
  `anySourceRequiresNaiveRelinearization()` = `tightlyCoupledImu()` today.
- *ordering:* contributions applied after `getProcessNoiseMatrix`/before P
  propagation; `predictAided` completes before `capturePriorSnapshot`.

**Loosely-coupled IMU also invariant** because `requires_naive_relinearization`
is coupling-gated — an LC config keeps `tightlyCoupledImu()==false` and so keeps
the proper prior-anchored IEKF, exactly as today.

---

## 12. Real-time IMU unlock

`RealTimeEngine` ingests GNSS byte frames only today (zero IMU references,
verified) and calls only `predict(dt)` at
[RealTimeEngine.cpp:763](src/core/realtime/RealTimeEngine.cpp). The abstraction
unlocks RT-IMU with three additions, none touching the GNSS frame path:

1. **Shared predict seam** — `predictAided` is consumed by both `Application::run`
   and `RealTimeEngine::processEpoch`; the engine branches at :763 exactly like
   Application.cpp:1682. `prev_epoch_time_` already exists
   ([RealTimeEngine.hpp:196](include/realtime/RealTimeEngine.hpp)), so the
   integration interval is available with no new bookkeeping.
2. **Streaming ingest** (the one genuinely new RT capability) — the deliberate
   split of `ingestSample(AidingSample)` from `contributePredict` exists *because*
   a real-time IMU is not a pre-loaded CSV: samples arrive asynchronously,
   buffer into the owned `ImuIntegrator` until the next GNSS epoch boundary
   defines the interval. New `RealTimeEngine::feedImuSample(...)` + an IMU branch
   in the `ApplicationRealtime` driver alongside `feedSbfFrame`/`feedRtcmFrame`.
3. **State readiness** — the engine runs with velocity + IMU error states
   enabled (the `AidingConfig` + the source's `describeCoreBlock` arrange this);
   gap policy queries `anySourceBridgesGaps()`.

Decisive point: post-proc and RT share **one** manager and **one**
`predictAided`, so the predict/update divergence the codebase already fought
(the reason `EpochUpdateSupport` was factored as a shared helper) cannot recur
for aiding.

---

## 13. Test seam

Follow the verified `XxxTestAccess` free-struct friend-shim idiom — declared
under `private:` with a "Test seam:" comment, **defined only in the test cpp**.
The review's key correction: in this tree, `TestAccess` shims exist **only on
numeric kernels** (`MlambdaTestAccess`, `WlsEstimatorTestAccess`,
`StochasticModelTestAccess`) — `SlipDetectorRegistry`/`MeasurementBuilderFactory`
have **no** shim because their selection API is already public. So:

- **Drop** `AidingSourceRegistryTestAccess` — `createEnabled`/`create`/
  `isAvailable`/`availableKinds` are public; the registry tests need no private
  access.
- **Keep** `ImuAidingSourceTestAccess` — `mapPredictionToContribution` (pure Φ/Q
  projection) and `computeTcSpecificForce` (relocated gravity/`f_b`) are bona-fide
  private kernels in the `WlsEstimator::predictObs` mold:

```cpp
// tests/test_aiding_source.cpp (namespace GnssProcessor, above anon namespace)
struct ImuAidingSourceTestAccess {
    static PredictContribution mapPrediction(const ImuAidingSource& s,
                                             const ImuPrediction& p,
                                             const std::vector<size_t>& ins_map) {
        return s.mapPredictionToContribution(p, ins_map);
    }
    static AidingMeasurementRow tcInputs(const ImuAidingSource& s,
                                         const Eigen::Vector3d& pos_ecef) {
        return s.computeTcSpecificForce(pos_ecef);
    }
};
```

Example cases (plain `TEST`/`TEST_F`, hand-listed in the explicit
`add_executable(gnss_tests …)`): registry ordering (`sources[0]->kind()==Imu`)
via **public** `createEnabled`; staged-Vision warn-skip via public `create`;
`PredictMapMatchesIntegrator` (placement at mapped indices, zero elsewhere);
`NoSamplesInvalidContribution` (`.valid==false`). Fixture-backed IMU-CSV tests
gate on `GNSS_TEST_DATA_DIR` with `GTEST_SKIP()`.

---

## 14. Open questions — resolutions

Build-shaping forks (resolved 2026-06-12):

1. **Predict baseline F.** ✅ **RESOLVED: keep the Identity seed** when an INS
   source is present (preserves bit-identity). The `getTransitionMatrix(dt)`
   "refinement" is deferred until a non-INS predict source lands, then adopted as
   a separately-validated, gated, trace-diffed correction — never as "free".
2. **Aiding rows under BSSD/DD.** ✅ **RESOLVED: defer the `R_full` block.** First
   release restricts row-contributing sources (ZUPT/baro/NHC) to undifferenced
   PPP + LC GNSS+IMU; `appendAidingRow` **hard-errors** when `R_full` is
   populated. `appendAidingRowBlock` (block-diagonal augmentation) is built only
   when a source must coexist with active BSSD/DD.
6. **Smoother contract for aiding states.** ✅ **RESOLVED: document the
   exclusion.** Aiding error states stay excluded from RTS smoothing (matching
   today's IMU — `collectCommonIndices`, [Smoother.cpp:90](src/core/filter/Smoother.cpp),
   omits att/bias). Acceptance criterion: verify the smoothed GNSS subspace stays
   consistent when an aiding source perturbs the forward prior. (Registering
   resets as `is_discontinuity` segment boundaries is a later option, not v1.)
7. **Config section.** ✅ **RESOLVED: `estimation.aiding.{…}`** (state-vector
   toggles, matching the `cycle_slip` precedent); `imu` defaults to the existing
   `enableImu()` value for back-compat.

Tier-2 / deferred forks (recommended defaults stand; revisit when the first
variable-cardinality source lands — they do not affect the IMU first client):

3. **Tier-2 Q/F ownership.** *Default:* `StateManager`'s builders stay ignorant of
   aiding dynamics; sources always *supply* their Q/F via the predict hook (IMU
   precedent), even for slow random-walk states.
4. **Error-state vs total-state sources.** *Default:* contributions stay
   error-state-shaped (Φ/Q over owned indices + optional nominal write-back), with
   `requires_naive_relinearization` letting a future total-state source opt into
   the proper IEKF.
5. **IMU core states stay hardwired** (current `enableImu()` carve-out) rather than
   migrating to `reserveDynamicBlock` — avoids touching state layout and the
   `removeStateAtIndex` invariant for zero benefit.

---

## 15. Adversarial-review log

Three lenses reviewed the first draft; what they changed:

| Lens | Verdict | Material change to the design |
|------|---------|-------------------------------|
| State-vector discipline | needs-rework | (blocker) Draft relocated nominal `C_b_e_` into the source → reverted: nominal has one off-vector owner, source reads/writes through it. (blocker) Tier-2 `reserveBlock` would interleave with the ambiguity tail → tier-2 deferred; core-offset/stable-tail placement + 3 explicit invariants. (blocker) Smoother silently drops aiding blocks → §14 Q6 contract. (major) `reserveStates` double-duty → split `describeCoreBlock`/`reserveDynamicBlock`. (major) naive-IEKF gate → coupling-mode gated. |
| Numerical invariance | sound-with-fixes | (blocker×3) `getTransitionMatrix` baseline breaks clock-rate coupling; `x=F*x` unspecified; BSSD freeze block omitted → **all resolved by: empty⇒delegate `predict(dt)`, INS⇒`predictImu` kernel verbatim (Identity seed)**. (major) baseline-Q interval must be `dt_total`, not GNSS `dt`. (major) unique row labels + assert. (minor) snapshot `x` once. Golden test upgraded to **P *and* x**. |
| Idiom fidelity | sound-with-fixes | (major) `getTransitionMatrix` "provably invariant" framing removed. (major) **Drop `AidingSourceRegistryTestAccess`** — factories get no shim in this tree. (minor) **Drop `contributesPredict`/`contributesMeasurement` bool predicates** — redundant with empty/invalid return; keep `reparameterizesGnssRows`. (minor) `StateBlockSpec` documented as one-shot receipt. Registry/manager/config/Doxygen fidelity all verified ✓. |

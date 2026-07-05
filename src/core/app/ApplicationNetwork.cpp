/**
 * @file ApplicationNetwork.cpp
 * @brief Implementation of Application::runNetwork (minimum-constraint
 *        S-system PPP-RTK NETWORK producer).
 *
 * Extracted from Application.cpp to keep the per-mode processing pipelines in
 * dedicated translation units (mirrors ApplicationBaseline.cpp /
 * ApplicationRealtime.cpp).  The network producer is a joint multi-station
 * estimator: per-station local normals reduced onto a shared satellite-
 * parameter block via an exact Schur complement, estimating the estimable
 * satellite clock + satellite phase bias under an explicitly declared
 * minimum-constraint S-basis, emitted in IGS-SSR convention for the companion
 * PPP_RTK_User consumer.
 *
 * See docs/ssystem_conformance_2026-06-15.md (§7) and the approved design plan
 * for the full architecture and the staged build-out (W1..W8).
 *
 * ===========================================================================
 * W4-S2b — the live epoch driver
 * ===========================================================================
 *
 * Per solution epoch the driver runs the arrowhead estimator end to end:
 *
 *   PASS A (layout)  for each present station: PppModel::computeModels →
 *       MeasurementModel::ensureNetworkSharedStates pre-allocates the shared
 *       satellite states (per-sat clock + per-(sat,signal) phase bias) using the
 *       SAME usability gate the measurement build uses.  The DATUM (pivot)
 *       station is processed first so its usable phase-signal set restricts the
 *       phase-bias columns to ones backed by a pivot-receiver ambiguity pin.
 *       The estimator's shared column layout is then registered (clock for every
 *       satellite any station sees; phase bias for the pivot's signal set).
 *
 *   PASS B (assembly)  for each present station: MeasurementModel::compute-
 *       MeasurementUpdate produces an undifferenced H/innovations/R block (no
 *       receiver-side differencing — DifferencingMode::None).  Its columns are
 *       split: the satellite clock (−1) and phase-bias (+1) columns scatter into
 *       the shared border A_s; the 3 receiver-position columns are DROPPED
 *       (coordinates held fixed); the rest become the station's private nuisance
 *       block A_r.  The pivot station additionally folds tight-prior pins on its
 *       own ambiguities (the LOCAL half of the S-basis — the emitted phase bias
 *       then carries that pivot-receiver ambiguity lump, Odijk Eq. 4).
 *
 *   SOLVE  NetworkSbasis builds the SHARED minimum-constraint datum (one
 *       satellite-clock pin per constellation), the null-space rank gate
 *       verifies the reduced shared system carries exactly that deficiency, then
 *       NetworkEstimator::solve produces the shared estimate.  NetworkProduct-
 *       Emitter serializes it into an OWNED SsrCorrectionStore (closed-loop
 *       isolation — never the process-wide singleton the user side reads).
 *
 *   D2 (between-epoch)  the shared posterior (value, variance) is carried
 *       forward as a diagonal prior on the next epoch's shared border, inflated
 *       by the per-parameter process noise over the epoch gap.
 *
 * Scope (V1): estimable satellite CLOCK + PHASE BIAS, coordinates/orbits fixed,
 * GPS + Galileo, per-band uncombined, float (no network AR).  Satellite code
 * bias, atmosphere products, real-time branch, GLONASS FCN bias, and network QC
 * are later slices (W6/W7/Phase 2).  Each station's coordinates MUST be known
 * (held fixed) and its observation RINEX path MUST be explicit in the config —
 * the PathResolver network gate is deferred, so resolveData() here serves only
 * products + broadcast nav.
 */

#include "app/Application.hpp"

#include "rinex/MultiRinexSync.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/NetworkStateSnapshot.hpp"   // RTB-7: atomic state snapshot + warm resume
#include "products/LiveEphemerisStore.hpp"     // Card 5 (RTF-1): broadcast a-priori store for brdc_rts
#include "products/RinexNavParser.hpp"         // Card 5 (RTF-1): RINEX-nav → LiveEphemerisStore loader
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkPivotHandover.hpp"   // A1/A2: pivot-handover datum-change detector
#include "filter/StateKeySchema.hpp"
#include "logging/CsvRotator.hpp"     // P1: size-based rotation of the unbounded service CSVs
#include "slip/MwSlipDetector.hpp"      // T0 phase-leveling slip repair (Increment 2)
#include "slip/TurboEditResolver.hpp"   // MW+GF per-signal integer repair (Increment 2)
#include "slip/IfPhaseSlipDetector.hpp" // IF-phase between-sat/between-epoch slip detection
#include "atmosphere/SphericalHarmonicIonosphere.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"        // B5: signal wavelengths for the DD fractional-cycle diagnostic
#include "realtime/RealTimeEngine.hpp"
#include "math/TimeUtils.hpp"
#include "math/Coordinates.hpp"

#include <Eigen/Dense>
#include <Eigen/Core>          // Eigen::setNbThreads / nbThreads (pin Eigen to 1 thread inside our per-station OpenMP region — avoids nested oversubscription and keeps the result deterministic)
#include <fmt/format.h>

#ifdef _OPENMP
#include <omp.h>               // per-station PASS A/B parallelism (gated on station count); pragmas degrade to no-ops when the toolchain lacks /openmp
#endif

#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
#include <deque>
#include <limits>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace GnssProcessor {

namespace {

/// Per-station runtime bundle: an independent single-receiver StateManager /
/// PppModel / MeasurementModel (single-receiver invariants intact — the network
/// couples stations only through the shared satellite border, never by aliasing
/// their core indices).  Coordinates are held fixed at @c ecef.
struct NetStation {
    std::string                       name;
    Eigen::Vector3d                   ecef{Eigen::Vector3d::Zero()};
    bool                              is_pivot{false};
    std::unique_ptr<StateManager>     sm;
    std::unique_ptr<PppModel>         model;
    std::unique_ptr<MeasurementModel> mm;
    PppState                          pstate;
    // MW cycle-slip arc state: persistent per-station.  Constructed for NON-pivot
    // stations when network.phase_leveling && (network.cycle_slip.turbo_edit ||
    // network.cycle_slip.mw_detect); null otherwise (no MW detection runs).  The
    // turbo_edit RESOLVER (MW+GF integer REPAIR, 1 s data) is built only for
    // turbo_edit; under mw_detect the same detector drives a detect→refloat with
    // no resolver (null turbo_edit ⇒ the refloat branch in PASS A).
    std::unique_ptr<MwSlipDetector>   mw_slip;
    std::unique_ptr<TurboEditResolver> turbo_edit;
    // IF-phase triple-difference slip DETECTION (this increment): persistent
    // per-station between-satellite/between-epoch IF-phase OMC state.  Constructed
    // for NON-pivot stations when network.cycle_slip.if_phase_td; null otherwise
    // (no detection runs).  Station-local — parallel-safe, no shared write.
    std::unique_ptr<IfPhaseSlipDetector> if_slip;
    // IF-phase slip thresholding (this increment): per-signal-pair EWMA of the
    // elevation-normalized robust noise scale for THIS station, with a clean-sample
    // counter driving the pool→per-station shrinkage.  Persists across epochs; keyed
    // by the IF pair "sig1_sig2" (e.g. "L1W_L2W").  See the PASS-A IF-phase block.
    struct IfPairScale {
        double ewma_scale{0.0};  ///< EWMA of 1.4826·median(|z|), z = residual·sin(el) (m, zenith-equiv).
        long   count{0};         ///< Clean samples folded in (shrinkage weight vs the network pool).
    };
    std::map<std::string, IfPairScale> if_pair_scale;
    // Prioritized phase-bias anchor (network.phase_datum_prioritized): the
    // "sat_signal" ambiguity keys THIS station is the elected gauge anchor for
    // THIS epoch — each such ambiguity gets the tight-prior (1e10) pin folded into
    // this station's OWN local block (the phase bias then absorbs THIS receiver's
    // ambiguity lump).  Cleared + rebuilt every epoch by the serial anchor
    // election (PASS A tail).  EMPTY on the legacy single-pivot path — there the
    // pivot pins the whole shared set via is_pivot, so no station carries this.  A
    // station anchors SOME satellites and is an ordinary carry-eligible station for
    // the rest: membership is per-(sat,signal), never a station-level role.
    std::set<std::string> pinned_amb_keys;
};

/// One shared parameter carried across epochs (D2): its value and posterior
/// variance, re-keyed by (kind, sat, signal) identity so it survives layout
/// changes (a satellite blinking in/out).
struct CarryPrior {
    SharedParamKind kind;
    std::string     sat;
    std::string     signal;
    int             sh_n{-1};    ///< VtecSh degree (VtecSh only)
    int             sh_m{-1};    ///< VtecSh order  (VtecSh only)
    char            sh_trig{0};  ///< VtecSh 'C'/'S' (VtecSh only)
    double          value;
    double          var;
    bool            inflate_q{false};  ///< Clock-monitor flagged this satellite: fold NEXT epoch with the large reinit variance (a soft large-q reinitialization that lets a frozen clock walk toward truth while retaining a weak anchor) instead of the stiff process noise.
};

/// Identity key of a dense-carried shared parameter (C1) — mirrors
/// NetworkEstimator's internal key construction so the stash and fold agree.
/// SH-VTEC never enters the dense block (its carry is the diagonal pre-gate
/// fold), so the (n,m,trig) family needs no key here.
static std::string sharedCarryKey(SharedParamKind kind, const std::string& sat,
                                  const std::string& signal) {
    switch (kind) {
        case SharedParamKind::Clock:     return sat + "|CLK";
        case SharedParamKind::CodeBias:  return sat + "|CB|" + signal;
        case SharedParamKind::PhaseBias: return sat + "|PB|" + signal;
        default:                         return sat;  // unreachable for the dense carry
    }
}

/// C1 (network.shared_carry_full_cov): the dense posterior covariance block
/// over the D2-carried Clock/PhaseBias/CodeBias entries of @c prev_carry,
/// stashed beside it at emit and folded next epoch as ONE information block
/// Λ = (P_prev + Q)⁻¹ (@ref NetworkEstimator::addSharedPriorDense) — the
/// information-form Kalman time update on the shared border.  Identity-keyed
/// (row lookup by the same (kind,sat,signal) key as the estimator layout) so
/// it survives cross-epoch layout changes and every existing @c prev_carry
/// value mutation (eph-cutover re-level) without re-indexing: @c prev_carry
/// stays the single source of truth for carried values and flags, this is a
/// passive covariance side-car.  Empty ⇒ the fold degrades per-key to the
/// legacy diagonal path (toggle OFF, warm resume from the v1 snapshot, or a
/// stash-side non-finite screen).
struct SharedCarryCov {
    std::map<std::string, std::size_t> row;  ///< identity key → row/col in P
    Eigen::MatrixXd P;                       ///< dense posterior covariance (m²)
    void clear() { row.clear(); P.resize(0, 0); }
};

/// One carried-forward ambiguity (phase-leveling, Increment 1): the float
/// ambiguity's ABSOLUTE value, its posterior conditional variance, and the
/// continuous-GPS-second time it was last seen (for the inter-epoch gap
/// inflation / stale-carry drop).  Keyed in @c local_carry by (station index,
/// satellite, signal) so it survives a satellite blinking in/out.
struct LocalCarry {
    double value_abs;     ///< Carried absolute ambiguity estimate (m).
    double var;           ///< Posterior conditional variance from the last solve (m²).
    double last_seen_s;   ///< Continuous-GPS seconds when last stashed (gap inflation).
};

/// One present, carry-eligible (non-pivot) ambiguity this epoch, recorded in
/// PASS B for the post-solve harvest/stash: its A_r column, identity, and the
/// linearization point used this epoch (so the absolute value is
/// @c x_lin + x_r(col) and a carried prior targets @c value_abs − x_lin).
struct PlAmb {
    Eigen::Index col;     ///< A_r column of this ambiguity (= h2local[amb_idx]).
    std::string  sat;     ///< RINEX satellite id.
    std::string  signal;  ///< RINEX signal / IF combo.
    double       x_lin;   ///< This epoch's ambiguity linearization point (state value).
    std::string  amb_key; ///< Full ambiguity key (clears a one-shot IF-phase refloat mark post-stash).
};

/// One present station ZWD column this epoch (B9): its A_r column and the ZWD
/// linearization point, for the post-solve harvest/stash (mirrors @ref PlAmb).
struct PlZwd {
    Eigen::Index col;     ///< A_r column of the ZWD state (= h2local[tropo_idx]).
    double       x_lin;   ///< This epoch's ZWD linearization point (state value).
};

/// PASS B produce-phase result for one station: the assembled local Jacobian
/// split plus the disposition flags the serial consume phase needs.  Each slot
/// is written by exactly one thread (indexed, never push_back) and consumed
/// serially in station order, so est.addStation sees identical inputs in an
/// identical order regardless of thread count — the determinism guarantee.
struct StationAssembly {
    bool             ok{false};            ///< Produced a usable H/A_s/A_r block.
    bool             empty_innov{false};   ///< computeMeasurementUpdate yielded no rows.
    bool             state_mismatch{false};///< H column count != state size.
    bool             threw{false};         ///< computeMeasurementUpdate / splitStation threw.
    Eigen::MatrixXd  A_s, A_r;             ///< Shared-border + local-nuisance Jacobian.
    Eigen::VectorXd  w, y;                 ///< Per-row information weights + innovations.
    int              np{0};                ///< Pivot ambiguity pins folded in (pivot only).
    std::map<int,int> h2local;             ///< H-column -> A_r-column map (back-substitute the slant-iono nuisance for the viz trace).
    std::string      err;                  ///< Diagnostic (mismatch detail or exception text).
    // Phase-leveling carry (Increment 1) — built in PASS B, consumed serially.
    std::vector<Eigen::Index> carry_cols;  ///< A_r columns receiving a carried prior (ASCENDING — determinism).
    Eigen::VectorXd           carry_dNrr;  ///< Per-column information increment Λ = 1/var_eff.
    Eigen::VectorXd           carry_dur;   ///< Per-column RHS increment η = Λ·(value_abs − x_lin).
    std::vector<PlAmb>        pl_amb;       ///< Present non-pivot ambiguities, for the post-solve stash.
    bool                      pl_zwd_present{false};  ///< This station had a carry-eligible ZWD column this epoch (B9).
    bool                      pl_zwd_injected{false}; ///< L7: a ZWD carry prior was actually injected this epoch (true "carried" flag for network_zwd.csv, distinct from carry-eligible).
    PlZwd                     pl_zwd{};               ///< The ZWD present-record for the post-solve stash (B9).
};

/// Elevation mapping for the IF-phase slip stochastic model: the multiplicative
/// factor by which between-epoch double-differenced carrier-phase noise inflates
/// toward the horizon.  A residual at elevation @p el_deg is normalized to a
/// zenith-equivalent by DIVIDING by this factor (≡ multiplying by sin el); the
/// empirical scale (pooled or per-station EWMA) is re-inflated by it per satellite
/// when thresholding, so ONE scale is valid across the whole sky.  1/sin(el) is the
/// standard first-order phase-noise shape; clamped at 5° so it cannot blow up at the
/// (already elevation-masked) horizon.  A future refinement is a fitted a+b/sin(el).
inline double ifElevMap(double el_deg) {
    constexpr double kMinElDeg = 5.0;
    const double el_rad = std::max(el_deg, kMinElDeg) * (Constants::PI / 180.0);
    const double s = std::sin(el_rad);
    return (s > 1e-3) ? (1.0 / s) : 1.0;
}

/// Median of a copy (upper-middle for even counts, matching this file's convention).
inline double medianCopy(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

/// Two-state (offset + drift) stochastic clock model for a carried satellite clock — the
/// Zucca–Tavella oscillator model, x = [offset Δδtˢ (m), drift (m/s)], driven by white-FM
/// (@c q1, m²/s) and random-walk-FM (@c q2, m²/s³) noise whose diffusion coefficients come
/// from the clock's Allan / Hadamard deviation.  Over a step @c dt the transition is
/// Φ = [[1, dt], [0, 1]] and the process-noise covariance is
///   Q(dt) = [[q1·dt + q2·dt³/3,  q2·dt²/2], [q2·dt²/2,  q2·dt]].
/// Carried across epochs in the driver (the @c clock_osc map).  When network.clock_model is
/// on it REPLACES the scalar random-walk clock carry (var_eff = var + q_clock·dt): the
/// physically-tight, drift-following prediction turns a self-consistent lock-in into a large
/// NORMALIZED innovation (a detectable dynamics fault) instead of an invisible magnitude, and
/// its predicted variance sizes the detection band per satellite/epoch rather than by a
/// hand-tuned metre threshold.
struct ClockOsc {
    double x0{0.0};    ///< clock offset Δδtˢ (m)
    double x1{0.0};    ///< clock drift (m/s)
    double p00{0.0};   ///< cov(offset, offset)
    double p01{0.0};   ///< cov(offset, drift)
    double p11{0.0};   ///< cov(drift, drift)
    double t_last{0.0};///< P0-3: continuous-GPS-seconds of the last update for THIS satellite (0 = never); the prediction + drift finite-difference use the true per-sat elapsed time, so a satellite that vanishes (eclipse) and returns predicts over its real gap — Q inflated over the true dt, not one epoch — instead of a falsely-tight hours-old state.
    bool   init{false};///< false until the first stash seeds it
};

/// One-step predicted ClockOsc (the carry-prior mean/variance) — Φ·x and Φ·P·Φᵀ + Q(dt).
struct ClockOscPred { double x0, x1, p00, p01, p11; };

/// Predict a ClockOsc forward by @p dt with white-FM @p q1 and random-walk-FM @p q2.
inline ClockOscPred clockOscPredict(const ClockOsc& s, double dt, double q1, double q2) {
    const double q11 = q1 * dt + q2 * dt * dt * dt / 3.0;
    const double q12 = q2 * dt * dt / 2.0;
    const double q22 = q2 * dt;
    ClockOscPred p;
    p.x0  = s.x0 + s.x1 * dt;
    p.x1  = s.x1;
    p.p00 = s.p00 + 2.0 * dt * s.p01 + dt * dt * s.p11 + q11;
    p.p01 = s.p01 + dt * s.p11 + q12;
    p.p11 = s.p11 + q22;
    return p;
}

/// Measurement update of @p s from prediction @p pr with a network-solved offset @p z (var
/// @p r) whose datum-common-mode has already been removed via @p innov (z − predicted, minus
/// the per-constellation median innovation).  H = [1 0].  Returns S = innovation variance so
/// the caller can form the normalized innovation innov/√S (the dynamics-fault statistic).
/// @note Retained for reference; the clock-model carry now sets the osc state DIRECTLY from the
///       joint-solve posterior (B3 data-incest fix), so this scalar KF update is no longer wired.
[[maybe_unused]] inline double clockOscUpdate(ClockOsc& s, const ClockOscPred& pr, double z, double r) {
    const double S  = pr.p00 + r;
    const double dz = z - pr.x0;                 // raw innovation (carry follows the raw clock)
    const double K0 = (S > 0.0) ? pr.p00 / S : 0.0;
    const double K1 = (S > 0.0) ? pr.p01 / S : 0.0;
    s.x0  = pr.x0 + K0 * dz;
    s.x1  = pr.x1 + K1 * dz;
    s.p00 = (1.0 - K0) * pr.p00;
    s.p01 = (1.0 - K0) * pr.p01;
    s.p11 = pr.p11 - K1 * pr.p01;
    s.init = true;
    return S;
}

/// Robust prominence of each entry in @p eff: how many robust-sigmas its effect stands
/// above the dimension's own median (z = (effect − median) / (1.4826·MAD)).  This is the
/// "peak stands above the surrounding terrain" measure used to attribute a network
/// residual spike to a bad satellite / station / signal.
template <class KeyT>
std::map<KeyT, double> robustProminence(const std::map<KeyT, double>& eff) {
    std::vector<double> vals;
    vals.reserve(eff.size());
    for (const auto& kv : eff) vals.push_back(kv.second);
    const double m = medianCopy(vals);
    std::vector<double> ad;
    ad.reserve(vals.size());
    for (double v : vals) ad.push_back(std::abs(v - m));
    double mad = 1.4826 * medianCopy(ad);
    if (!(mad > 0.0)) mad = 1.0;
    std::map<KeyT, double> out;
    for (const auto& kv : eff) out[kv.first] = (kv.second - m) / mad;
    return out;
}

/// Result of a Tukey 2-way median polish: value ≈ overall + row_eff[row] + col_eff[col]
/// + interaction (parallel to the input cell order).
struct MedianPolish {
    double overall{0.0};
    std::map<int, double> row_eff;          ///< per-row (station) effect
    std::map<std::string, double> col_eff;  ///< per-column (satellite) effect
    std::vector<double> interaction;        ///< leftover per cell (isolated arc anomaly)
};

/// Robust additive decomposition of a sparse (row=int, col=string) → value table by
/// alternately sweeping out row and column medians (Tukey median polish).  Medians make
/// it robust: a minority of huge cells (the real outliers) do not distort the additive
/// row/column effects — which is the whole point (separate a bad satellite that lights up
/// a whole column from a bad station that lights up a whole row, and leave the isolated
/// (station,sat) arc slips in the interaction term).  O(iters · cells); no re-solve.
inline MedianPolish medianPolish2Way(
        const std::vector<std::tuple<int, std::string, double>>& cells, int iters = 5) {
    MedianPolish mp;
    std::vector<double> R(cells.size());
    std::map<int, std::vector<std::size_t>> by_row;
    std::map<std::string, std::vector<std::size_t>> by_col;
    for (std::size_t k = 0; k < cells.size(); ++k) {
        R[k] = std::get<2>(cells[k]);
        by_row[std::get<0>(cells[k])].push_back(k);
        by_col[std::get<1>(cells[k])].push_back(k);
    }
    for (const auto& kv : by_row) mp.row_eff[kv.first] = 0.0;
    for (const auto& kv : by_col) mp.col_eff[kv.first] = 0.0;
    auto med_idx = [&](const std::vector<std::size_t>& idx) {
        std::vector<double> v;
        v.reserve(idx.size());
        for (std::size_t k : idx) v.push_back(R[k]);
        return medianCopy(v);
    };
    for (int it = 0; it < iters; ++it) {
        for (const auto& [row, idx] : by_row) {
            const double rm = med_idx(idx);
            for (std::size_t k : idx) R[k] -= rm;
            mp.row_eff[row] += rm;
        }
        {
            std::vector<double> re;
            for (const auto& kv : mp.row_eff) re.push_back(kv.second);
            const double m = medianCopy(re);
            mp.overall += m;
            for (auto& kv : mp.row_eff) kv.second -= m;
        }
        for (const auto& [col, idx] : by_col) {
            const double cm = med_idx(idx);
            for (std::size_t k : idx) R[k] -= cm;
            mp.col_eff[col] += cm;
        }
        {
            std::vector<double> ce;
            for (const auto& kv : mp.col_eff) ce.push_back(kv.second);
            const double m = medianCopy(ce);
            mp.overall += m;
            for (auto& kv : mp.col_eff) kv.second -= m;
        }
    }
    mp.interaction = std::move(R);
    return mp;
}

/// Parse an IfPhaseSlipDetector reset key ("G01_L1C_L2W") into the satellite and
/// the two band signals of the IF pair it was formed from.  Returns false on a
/// malformed key.  The network carries per-band ambiguities, so a flagged IF pair
/// must refloat BOTH bands — the IF combination cannot localize which one slipped.
inline bool parseIfPairResetKey(const std::string& key, std::string& sat,
                                std::string& sig1, std::string& sig2) {
    const auto p1 = key.find('_');
    if (p1 == std::string::npos || p1 == 0) return false;
    const auto p2 = key.find('_', p1 + 1);
    if (p2 == std::string::npos || p2 == p1 + 1 || p2 + 1 >= key.size()) return false;
    sat  = key.substr(0, p1);
    sig1 = key.substr(p1 + 1, p2 - p1 - 1);
    sig2 = key.substr(p2 + 1);
    return !sat.empty() && !sig1.empty() && !sig2.empty();
}

} // namespace

int Application::runNetwork() {
    auto log = log_manager_.getLogger("main_log");
    const auto& nc = config_.getNetworkConfig();

    log->info("PPP-RTK Network producer (V1): {} stations, clock datum station '{}'{}",
              nc.stations.size(), nc.clock_datum_station,
              nc.clock_datum_satellite.empty()
                  ? std::string{}
                  : (" + datum sat '" + nc.clock_datum_satellite + "'"));

    // --- Phase 0: validate inputs (explicit RINEX + fixed coordinates) ------
    // V1 requires an explicit observation RINEX path per station: the
    // PathResolver network gate (filling paths from the internal data tree by
    // station name) is deferred to avoid editing the shared Application.cpp
    // resolve path while concurrent sessions are in flight.  Coordinates are
    // validated by loadNetworkSection_ (every station needs a known position).
    for (const auto& s : nc.stations) {
        if (s.rinex_path.empty()) {
            log->error("Network: station '{}' has no observation rinex_path. V1 "
                       "requires an explicit network.stations[].rinex_path for "
                       "every station (PathResolver network auto-discovery is "
                       "deferred).", s.name);
            return 1;
        }
    }

    // --- Phase 1: resolve products + broadcast nav --------------------------
    // Station RINEX is explicit, so resolveData() here only fills product /
    // navigation paths (orbits + clocks needed for per-station geometry).  A
    // failure is non-fatal: the product-cache load below surfaces a precise
    // "no orbits" diagnostic if products are genuinely missing.
    if (!resolveData()) {
        log->warn("Network: resolveData() failed; relying on explicit product / "
                  "navigation paths in the config (product-cache load will report "
                  "if orbits are missing).");
    }

    if (!product_parser_) {
        product_parser_ = std::make_unique<ProductParser>(config_, log_manager_);
    }

    // --- Phase 2: multi-station RINEX ingestion + epoch alignment -----------
    // MultiRinexSync is driven by a BaselineConfig roster; adapt the network
    // stations into it with the clock-datum station as the "reference" so the
    // synced schedule walks its epochs (by_station includes the reference).
    BaselineConfig roster;
    roster.epoch_sync_tolerance_s = nc.epoch_sync_tolerance_s;
    bool pivot_found = false;
    for (const auto& s : nc.stations) {
        if (s.name == nc.clock_datum_station) {
            roster.reference = s;
            pivot_found = true;
        } else {
            roster.rovers.push_back(s);
        }
    }
    if (!pivot_found) {
        log->error("Network: clock_datum_station '{}' not in stations list",
                   nc.clock_datum_station);
        return 1;
    }

    // Closed-loop integer-recovery harness: a HELD-OUT consumer station, added to
    // the sync roster ONLY so its observations are time-aligned with the network
    // schedule — it is deliberately kept OUT of the joint estimator stations
    // (built below) and instead consumes the producer's emitted SSR products.
    const ClosedLoopConfig& cl = nc.closed_loop;
    const bool closed_loop_on = cl.enable && !cl.station.name.empty();

    // Card 5 (RTF-1): a-priori orbit/clock source.  sp3 (default) = the precise
    // SP3/CLK cache is the a-priori (today's behaviour, byte-identical).  brdc_rts =
    // the a-priori is BROADCAST ephemeris (LiveEphemerisStore) — the self-sufficient
    // real-time service mode.  Threaded through Phase 3 (nav load + SP3-error lift),
    // the producer station gate-suppression (D1), the anchor-σ re-base (D6), the
    // clock-IODE stamp, and the apriori_product_id.  Every use is guarded on this
    // flag so the sp3 path is textually unchanged.
    const bool brdc_rts =
        (config_.networkAprioriSource() == NetworkAprioriSource::BrdcRts);
    if (closed_loop_on) {
        roster.rovers.push_back(cl.station);
        log->info("Network: closed-loop consumer '{}' added to the sync roster "
                  "(held OUT of the joint solve)", cl.station.name);
    }

    MultiRinexSync sync(config_, roster, log_manager_, *product_parser_);

    // Clip each station's parse to the processing window.  The epoch loop below
    // is bounded by processing.max_duration_s (anchored on the first synced
    // epoch); without this, load() would parse — and synchronize() would copy —
    // the FULL 24h of every station even when only the first minutes are used,
    // which dominates wall-time at ~50 full-day stations.  A small margin covers
    // per-station first-epoch jitter; max_duration_s <= 0 keeps full-file parsing.
    //
    // The parse window is anchored at the file start, but network.start_offset_hours
    // fast-forwards the epoch loop PAST that offset before the max_duration cutoff
    // begins counting (the cutoff anchors on the first PROCESSED epoch).  So the
    // window must cover [0, start_offset + max_duration + margin]; clipping to only
    // max_duration would strip out the offset window and leave zero epochs to solve.
    if (config_.getMaxDuration() > 0.0) {
        const double start_offset_s = config_.networkStartOffsetHours() * 3600.0;
        const double span_s = start_offset_s + config_.getMaxDuration() + 60.0;
        sync.setParseWindowSeconds(span_s);
        if (start_offset_s > 0.0)
            log->info("Network: clipping RINEX parse to the first {:.0f} s of each station "
                      "(start_offset {:.0f} s + max_duration_s {:.0f} s + 60 s margin)",
                      span_s, start_offset_s, config_.getMaxDuration());
        else
            log->info("Network: clipping RINEX parse to the first {:.0f} s of each "
                      "station (max_duration_s {:.0f} s + 60 s margin)",
                      span_s, config_.getMaxDuration());
    }

    if (!sync.load()) {
        // Non-fatal: a station that failed to parse is excluded from the aligned
        // schedule (synchronize() skips it; the datum/pivot is guarded below in
        // buildStation).  This lets a large roster proceed when some stations'
        // observation files are missing or corrupt — the expected case when
        // fetching ~50 stations from a public archive.
        log->warn("Network: one or more station RINEX files failed to parse; "
                  "proceeding with the stations that loaded (the datum station "
                  "is required and is checked next).");
    }
    if (!sync.synchronize()) {
        log->error("Network: failed to synchronise epochs across stations");
        return 1;
    }
    const auto& synced = sync.getSyncedEpochs();
    if (synced.empty()) {
        log->error("Network: zero synchronised epochs — check epoch_sync_tolerance_s "
                   "and whether the station files overlap in time");
        return 1;
    }
    log->info("Network: multi-station sync complete — {} synchronised epochs", synced.size());

    // --- Phase 3: products + cache for geometry -----------------------------
    product_parser_->loadProducts();
    auto& product_cache = ProductCache::getInstance();
    if (!product_cache.loadProducts(*product_parser_, log_manager_)) {
        log->error("Network: failed to initialise product cache — no broadcast nav / orbits");
        return 1;
    }
    log->info("Network: product cache loaded — {} orbits, {} clocks",
              product_cache.getOrbitCount(), product_cache.getClockCount());
    // Card 8 (replay parity): staged broadcast nav records + feed cursor for the
    // progressive LiveEphemerisStore insertion in the epoch loop (brdc_rts only;
    // empty otherwise).  Availability = GPS-continuous-scale toc (BDS toc is
    // native BDT → shifted to the GPS scale, mirroring LiveEphemerisStore's
    // toe convention).
    auto navAvailabilityGpsSeconds = [](const BroadcastEphemeris& e) {
        double c = static_cast<double>(e.kepler.toc.week) * 604800.0 + e.kepler.toc.tow;
        if (e.system == System::BeiDou) c += 1356.0 * 604800.0 + 14.0;   // BDT week 0 = GPS week 1356, BDT = GPS − 14 s
        return c;
    };
    std::vector<BroadcastEphemeris> brdc_nav_staged;
    std::size_t brdc_nav_cursor = 0;
    if (brdc_rts) {
        // Card 5 (RTF-1): the a-priori is BROADCAST ephemeris, not SP3/CLK.  Parse the
        // RINEX navigation file into the shared LiveEphemerisStore — the SAME store the
        // real-time engine fills from RTCM 1019/1042/1044/1046 — so replay resolves
        // satellite orbit+clock through the identical code path as a live run (frame
        // consistency, conventions §1 / plan D4).  Geometry::getSatellitePV falls back
        // to this store when no SP3 is loaded, and GnssCorrections::getSatelliteClock-
        // Correction supplies the broadcast SV clock (marked broadcast-grade) when no
        // precise CLK covers the epoch — exactly the broadcast a-priori this mode needs.
        const std::filesystem::path nav_path = config_.getNavigationPath();
        if (nav_path.empty()) {
            log->error("Network (brdc_rts): network.apriori_source = brdc_rts requires a "
                       "broadcast navigation file, but files.navigation_path is empty. "
                       "Set it, or let PathResolver auto-resolve a BRDC / _MN.rnx file "
                       "covering the observation day.");
            return 1;
        }
        // Parse first so a per-constellation loaded-sat summary is available.
        //
        // Card 8 fix (replay parity, plan D4): do NOT bulk-insert the whole day.
        // LiveEphemerisStore retains only CURRENT+PREVIOUS per satellite (live
        // streaming semantics — the retention Cards 3/6/7 pin against), so a
        // full-file load leaves each satellite holding its LAST two records of
        // the day; at any processing epoch more than a fit window (~4 h GPS,
        // ~2 h GAL/BDS) before end-of-day the staleness guard then rejects
        // EVERY satellite and the producer degenerates to n_present = 0.
        // Instead STAGE the records sorted by availability and feed them to
        // the store progressively as the epoch loop advances, so the store's
        // current/previous evolve exactly as they would receiving a live nav
        // stream.  Availability proxy = GPS-scale toc: the file carries no
        // receipt stamp (RinexNavParser sets received_time = {0,0}), and toc
        // is the closest file-derivable stand-in for "a live receiver holds
        // this record" (live GPS broadcasts a data set from ~toe−2h, so the
        // toc proxy is conservative by up to one fit half-window — records
        // roll slightly LATER than live, never prematurely).
        std::vector<BroadcastEphemeris> nav = RinexNavParser::parse(nav_path.string());
        std::map<char, std::set<std::string>> nav_by_const;
        for (const auto& e : nav)
            if (!e.satellite.empty()) nav_by_const[e.satellite[0]].insert(e.satellite);
        if (nav.empty()) {
            log->error("Network (brdc_rts): parsed 0 usable broadcast ephemerides from '{}'. "
                       "Cannot form a broadcast a-priori — check the file is a "
                       "RINEX 3/4 navigation file covering the observation day.",
                       nav_path.string());
            return 1;
        }
        std::stable_sort(nav.begin(), nav.end(),
                         [&](const BroadcastEphemeris& a, const BroadcastEphemeris& b) {
                             return navAvailabilityGpsSeconds(a) < navAvailabilityGpsSeconds(b);
                         });
        brdc_nav_staged = std::move(nav);
        {
            std::size_t n_sats = 0;
            std::string summary;
            for (const auto& kv : nav_by_const) {
                n_sats += kv.second.size();
                summary += fmt::format("{}{}={}", summary.empty() ? "" : " ",
                                       kv.first, kv.second.size());
            }
            log->info("Network (brdc_rts): broadcast a-priori staged from '{}' — {} records, "
                      "{} satellites [{}]; fed to LiveEphemerisStore progressively by toc "
                      "(live-arrival replay).", nav_path.filename().string(),
                      brdc_nav_staged.size(), n_sats, summary);
        }
        // ANY precise product present in brdc_rts is an unsupported combination — refuse
        // rather than mask it (revisit when the RTS-orbit seam, plan D9, lands).  BOTH the
        // orbit AND the clock cache must be empty: a precise SP3 shadows the broadcast
        // a-priori via getSatellitePV node interpolation, and a precise CLK shadows the
        // broadcast clock in getSatelliteClockCorrection (interpolateClock wins over the
        // broadcast fallback for covered epochs → used_broadcast=false, NOT broadcast-grade).
        // Either alone is a silent orbit/clock frame split: the estimated C0 becomes a delta
        // against the wrong baseline while its stamped IODE still names the broadcast eph, so
        // the consumer reconstructs brdc(iode)+C0 and is metre-level wrong.  Gate on orbit OR
        // clock count (getOrbitCount alone missed a CLK-without-SP3 product set).
        if (product_cache.getOrbitCount() > 0 || product_cache.getClockCount() > 0) {
            log->error("Network (brdc_rts): precise products are loaded alongside "
                       "apriori_source = brdc_rts ({} SP3 orbit node(s), {} CLK sample(s)). This "
                       "combination is not supported — a precise orbit/clock would shadow the "
                       "broadcast a-priori (a silent orbit/clock frame split). Remove the SP3/CLK "
                       "product paths, or use apriori_source = sp3.",
                       product_cache.getOrbitCount(), product_cache.getClockCount());
            return 1;
        }
    } else if (product_cache.getOrbitCount() == 0) {
        // sp3 mode (default): the precise orbit cache IS the a-priori — unchanged.
        log->error("Network: no orbit data available; cannot compute satellite geometry. "
                   "Set navigation_path / products to cover the observation day.");
        return 1;
    }

    // --- Phase 3b: GIM for satellite code-bias estimation (W6) ---------------
    // The satellite DCB (geometry-free code bias) is co-linear with each station's
    // free per-(rx,sat) slant ionosphere until an external constraint imposes
    // between-satellite structure.  When network.estimate_sat_code_bias is on,
    // load the IONEX GIM and fold a common-mode-free pseudo-observation per station
    // (see appendGimIonoRows); without it the satellite DCBs stay in the null space
    // and the rank gate withholds every epoch, so this is a hard prerequisite.
    // The satellite-DCB iono constraint comes from ONE of two mutually-exclusive
    // sources: the external GIM (legacy W6) or the co-estimated SH-VTEC field
    // (estimate_iono_sh).  They are never both applied — when the network estimates
    // its own field it does NOT load a GIM.
    std::unique_ptr<SphericalHarmonicIonosphere> sh_model;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    if (config_.estimateNetworkIonoSh()) {
        sh_model = std::make_unique<SphericalHarmonicIonosphere>(shc.max_degree);
        log->info("Network (SH-iono): co-estimating own global SH-VTEC field — degree {} "
                  "({} coeffs), shell {:.0f} km, {} longitude; external GIM NOT loaded.",
                  sh_model->maxDegree(), sh_model->numCoeffs(), shc.layer_height_km,
                  shc.sun_fixed ? "sun-fixed" : "geographic");
    } else if (config_.estimateNetworkSatCodeBias()) {
        // Card 5 belt-and-braces: the external-GIM code-bias path is post-processing-only
        // (a GIM is a daily product, unavailable in the self-sufficient brdc_rts service).
        // Card 1's load-time validation rejects brdc_rts + estimate_sat_code_bias +
        // !estimate_iono_sh as a hard error, so reaching this branch in brdc_rts means the
        // guard was bypassed — refuse rather than silently loading a GIM the service can't have.
        if (brdc_rts) {
            log->error("Network (brdc_rts): reached the external-GIM code-bias path, which is "
                       "post-processing-only. Set network.estimate_iono_sh = true to co-estimate "
                       "the ionosphere field, or disable network.estimate_sat_code_bias "
                       "(this combination should have been rejected at config load).");
            return 1;
        }
        gim_ = std::make_unique<GlobalIonosphereMap>(
            config_.getProductPath("INX").string(), log_manager_);
        if (!gim_->isLoaded()) {
            log->error("Network (W6): network.estimate_sat_code_bias is on but no usable IONEX "
                       "GIM was resolved (looked for a .INX under the date's products/). The "
                       "satellite code bias is unobservable without the GIM ionosphere "
                       "constraint — supply a GIM (same analysis centre as the orbits/clocks for "
                       "bias consistency), enable network.estimate_iono_sh to co-estimate the "
                       "field instead, or disable estimate_sat_code_bias.");
            return 1;
        }
        log->info("Network (W6): GIM loaded for the satellite code-bias ionosphere constraint "
                  "(common-mode-free per-constellation weighting).");
    }

    // --- Phase 4: per-station Kalman resources (pivot at index 0) -----------
    // Each station: a single-receiver StateManager (coords fixed at the
    // configured ECEF), PppModel bound to its RinexParser, and a MeasurementModel.
    // Pivot first so PASS A can seed its phase-signal set before the others.
    std::vector<NetStation> stations;
    auto buildStation = [&](const StationConfig& cfg, bool is_pivot) -> bool {
        const RinexParser* parser = sync.parserOf(cfg.name);
        if (!parser) {
            log->error("Network: station '{}' parser unavailable", cfg.name);
            return false;
        }
        NetStation st;
        st.name     = cfg.name;
        st.ecef      = Eigen::Vector3d(cfg.position_ecef.data());
        st.is_pivot  = is_pivot;
        st.sm        = std::make_unique<StateManager>(5, log_manager_, config_);  // pos(3)+clk(1)+tropo(1)
        Eigen::VectorXd x0 = st.sm->getStateVector();
        x0.head<3>() = st.ecef;
        st.sm->setStateVector(x0);
        st.sm->setStateEpochTime(synced.front().time);
        st.model = std::make_unique<PppModel>(
            config_, *product_parser_, log_manager_, *parser, *st.sm);
        // Card 5 (RTF-1 / plan D1): in brdc_rts the a-priori satellite clock is
        // broadcast-grade, but the network ESTIMATES the clock (Δδtˢ vs that
        // a-priori), so this PRODUCER station's carrier phase is informative and
        // must be kept.  Suppress the code-only broadcast-phase gate on producer
        // models only — buildStation never builds the held-out closed-loop consumer
        // (the roster loop below skips it), so the consumer keeps the default gate.
        // sp3 mode passes false ⇒ byte-identical.
        st.model->setBroadcastPhaseGateSuppressed(brdc_rts);
        st.mm = std::make_unique<MeasurementModel>(
            *st.sm, *st.model, config_, log_manager_);
        // Seed this station's measurement-model frequency helper with its own
        // RINEX-header GLONASS FCN slots, so the GLONASS iono coefficient and
        // carrier wavelength in the Jacobian use the actual channel (not the
        // compiled-in default table).  Harmless for GPS/Galileo-only stations
        // (empty map).  Matters only under enableGlonassNetwork().
        st.mm->setGlonassSlots(parser->getGlonassFreqSlots());

        // Phase-leveling T0 (Increment 2): a persistent per-station MW + GF
        // TurboEdit detector pair for cycle-slip REPAIR of the carried ambiguities.
        // NON-pivot only on the legacy path — the single pivot's ambiguities are
        // ALL gauge-pinned and a repair there needs Increment 4 (design §6).  Under
        // the prioritized datum EVERY station (incl. the former pivot) carries the
        // arcs it does not anchor, so it MUST slip-detect them — build for all; the
        // PASS-A refloat still no-ops on a station's anchored (uncarried) arcs.
        // Constructed only when the carry AND a detector are on, so OFF is
        // byte-identical.  setCorrections(&this station's GnssCorrections) engages
        // the BIA-corrected wide-lane path (integer dN) where biases are available;
        // arc state lives on the detector and is station-local (parallel-safe).
        if ((config_.networkPhaseDatumPrioritized() || !is_pivot)
            && config_.networkPhaseLeveling()
            && (config_.networkCycleSlipTurboEdit() || config_.networkCycleSlipMwDetect())) {
            st.mw_slip = std::make_unique<MwSlipDetector>(config_, log_manager_);
            st.mw_slip->setCorrections(&st.model->getCorrections());
            // The TurboEdit MW+GF integer REPAIR resolver is built ONLY for turbo_edit
            // (higher-rate 1 s data, where the geometry-free ionospheric drift over the
            // epoch gap is small enough to trust an integer).  mw_detect uses the SAME MW
            // detector as a pure DETECTOR → refloat, so it needs no resolver — and the null
            // st.turbo_edit is exactly what steers the PASS-A branch to the refloat path.
            if (config_.networkCycleSlipTurboEdit()) {
                st.turbo_edit = std::make_unique<TurboEditResolver>(config_, log_manager_);
                st.turbo_edit->setCorrections(&st.model->getCorrections());
            }
        }

        // IF-phase triple-difference slip DETECTION (this increment): a persistent
        // per-station between-satellite/between-epoch ionosphere-free phase OMC
        // detector.  NON-pivot only on the legacy path; under the prioritized datum
        // build for EVERY station (the former pivot carries non-anchored arcs that
        // need detection).  Constructed whenever network.cycle_slip.if_phase_td is
        // on — independent of phase-leveling, so it can run as a pure diagnostic in
        // the absolute solve; the refloat ACTION it drives is gated on the carry
        // being active at the call site.  Station-local detector state (parallel-safe).
        if ((config_.networkPhaseDatumPrioritized() || !is_pivot)
            && config_.networkCycleSlipIfPhaseTd()) {
            st.if_slip = std::make_unique<IfPhaseSlipDetector>(config_, log_manager_);
            st.if_slip->setDeferFlagging(true);  // network pools per-pair MAD; the driver flags
        }

        st.pstate.position        = st.ecef;
        st.pstate.clock_bias      = 0.0;
        st.pstate.troposphere_zwd = 0.0;
        stations.push_back(std::move(st));
        return true;
    };
    if (!buildStation(roster.reference, /*is_pivot=*/true)) {
        log->error("Network: clock datum station '{}' failed to load; the network "
                   "cannot be built without its datum. Aborting.",
                   roster.reference.name);
        return 1;
    }
    for (const auto& rv : roster.rovers) {
        if (closed_loop_on && rv.name == cl.station.name) continue;  // held-out consumer
        if (!buildStation(rv, /*is_pivot=*/false)) {
            log->warn("Network: station '{}' failed to load; skipping it (the "
                      "network proceeds with the remaining stations).", rv.name);
            continue;
        }
    }
    log->info("Network: {} station estimators constructed (pivot '{}')",
              stations.size(), stations.front().name);

    // --- Station roster (viz) -----------------------------------------------
    // A tiny static CSV of the stations that actually loaded (id, role, ECEF) so
    // the pyviz worldmap reflects the real run, not just the config.  The
    // held-out closed-loop consumer (if any) is listed with role "held_out".
    if (nc.trace_stations) {
        std::error_code ec;
        std::filesystem::create_directories(log_dir_, ec);
        const std::filesystem::path roster_path = log_dir_ / "network_stations.csv";
        std::ofstream roster(roster_path);
        if (roster) {
            roster << "station,role,ecef_x,ecef_y,ecef_z\n";
            for (const auto& st : stations)
                roster << st.name << ',' << (st.is_pivot ? "datum" : "network") << ','
                       << fmt::format("{:.4f}", st.ecef.x()) << ','
                       << fmt::format("{:.4f}", st.ecef.y()) << ','
                       << fmt::format("{:.4f}", st.ecef.z()) << '\n';
            if (closed_loop_on) {
                const auto& p = cl.station.position_ecef;
                roster << cl.station.name << ",held_out,"
                       << fmt::format("{:.4f}", p[0]) << ','
                       << fmt::format("{:.4f}", p[1]) << ','
                       << fmt::format("{:.4f}", p[2]) << '\n';
            }
            log->info("Network: wrote station roster {}", roster_path.string());
        } else {
            log->warn("Network: failed to open station roster {}", roster_path.string());
        }
    }

    // === Per-station compute parallelism ====================================
    // PASS A (geometry) and the produce half of PASS B (measurement assembly) are
    // embarrassingly parallel across stations — each owns an independent
    // StateManager / PppModel / MeasurementModel, and the audited call trees
    // touch no shared mutable state (product stores are const post-load; the
    // per-instance model caches are thread-private; the live SSR/ephemeris
    // singletons are mutex-guarded and empty in post-processing).  Only the
    // shared-estimator scatter (est.addStation) and everything downstream of it
    // stays serial, in deterministic station order, so the float solution is
    // BIT-IDENTICAL to a serial run regardless of thread count.  Activated only
    // above the configured station-count threshold (small networks run serially —
    // no fork-join cost, and the internal 3-station fixture is unchanged).
    int hw_max_threads = 1;
#ifdef _OPENMP
    hw_max_threads = omp_get_max_threads();
#endif
    const int n_sta_total = static_cast<int>(stations.size());
    const bool parallel_on =
        nc.parallel_stations
        && n_sta_total > nc.parallel_min_stations
        && hw_max_threads > 1;
    const int n_threads = parallel_on
        ? (nc.parallel_max_threads > 0
               ? std::min(nc.parallel_max_threads, hw_max_threads)
               : hw_max_threads)
        : 1;
#ifndef _OPENMP
    if (nc.parallel_stations && n_sta_total > nc.parallel_min_stations)
        log->warn("Network: parallel_stations requested but this build has no OpenMP "
                  "support; per-station compute will run serially.");
#endif
    if (parallel_on)
        log->info("Network: per-station compute PARALLEL — {} stations across {} threads "
                  "(shared-estimator scatter stays serial; solution is thread-count-invariant)",
                  n_sta_total, n_threads);
    else
        log->info("Network: per-station compute SERIAL ({} stations)", n_sta_total);

    // Prewarm every logger the per-station model path can touch.  getLogger()'s
    // first-touch CREATE path mutates a shared std::map; fetching an existing
    // logger is lock-free and data-race-safe.  Creating each one now (single-
    // threaded) guarantees no insert ever races a concurrent fetch inside the
    // parallel region — the only shared-mutable hazard the thread-safety audit
    // found in the per-station call tree.
    if (parallel_on) {
        for (const char* lname : {
                "ppp_model", "geometry", "antenna", "antenna_model", "corrections",
                "planetary_model", "ocean_tide", "ppp_utils", "ionosphere",
                "troposphere", "stochastic_model", "measurement_model",
                "state_manager", "state_initializer", "bssd_transform",
                "ambiguity_resolver", "main_log" })
            (void)log_manager_.getLogger(lname);
    }

    // --- Owned product sink (closed-loop isolation) -------------------------
    SsrCorrectionStore network_store;
    // A2: pass the clock-disc toggle so the emitter maintains + stamps the per-sat clock
    // discontinuity counter.  R9: the phase-bias disc threshold is the emitter's own default
    // constant (single source of truth shared with the serialized contract's disc_threshold_m).
    // RTA-11: the staleness horizon (product-time seconds; 0 = off) makes a key returning
    // after a longer gap a first sighting (no spurious disc bump against an hours-stale
    // baseline); like the other two parameters it travels in the serialized contract.
    NetworkProductEmitter emitter(NetworkProductEmitter::kDefaultDiscThresholdM,
                                  config_.networkEmitClockDisc(),
                                  config_.networkEmitterStalenessHorizonS());

    // --- Optional on-disk product serialization -----------------------------
    // A reusable per-epoch dump of the emitted solution (the same pre-emit
    // NetworkSolutionEpoch the in-memory closed loop consumes, plus this epoch's
    // datum-change flag).  Lets consumer-side experiments — notably the consumer
    // AR-frame work — replay the products WITHOUT re-running the multi-station
    // network: NetworkProductReader::replay reproduces this store byte-for-byte
    // (discontinuity counters included).
    std::unique_ptr<NetworkProductWriter> product_writer;
    std::filesystem::path prod_path;
    // B6 (P1-1): when the S-basis contract is serialized, DEFER opening the writer until
    // the first emit so the realized per-constellation clock pivots (auto-picked, unknown
    // until the first solve) can be stamped into the v2 header — not just the config
    // intent.  pending_contract holds everything known up front; realized pivots merge in
    // at the first emit.  contract_pending gates that one-shot construction.
    SbasisContract pending_contract;
    bool           contract_pending = false;
    if (!nc.product_output_path.empty()) {
        // A relative path lands under the network trace dir, alongside
        // network_solution.csv etc.; an absolute path is used verbatim.
        prod_path = nc.product_output_path;
        if (prod_path.is_relative()) prod_path = log_dir_ / prod_path;
        if (nc.serialize_sbasis_contract) {
            // B6: stamp the S-basis contract into a v2 header.  Declaration semantics are
            // the A3-corrected per-constellation sat-clock-pin basis.
            pending_contract.decl.reference_receiver_id = nc.clock_datum_station;
            pending_contract.decl.emit_code_bias        = config_.estimateNetworkSatCodeBias();
            if (config_.networkClockDatumSumToZero()) {
                // Anchor-free clock datum: declare the sum-to-zero basis and leave
                // clock_pivot_sat empty (there is no pivot; a config pin is ignored).
                // (Local clock_sum_to_zero is declared further down; the getter is
                // cheap and this one-shot startup block precedes it.)
                pending_contract.decl.isb_datum =
                    SbasisDeclaration::IsbDatum::SumToZeroPerConstellation;
            } else if (!nc.clock_datum_satellite.empty()) {
                pending_contract.decl.clock_pivot_sat[NetworkSbasis::constellationOf(nc.clock_datum_satellite)]
                    = nc.clock_datum_satellite;
            }
            // P1-1: resolve the a-priori orbit/clock identity to the ACTUAL resolved
            // SP3 + CLK file basenames (the baseline the emitted clock C0 is a delta
            // against).  Config intent alone is "CODFIN00000000" under auto-detect.
            const auto& dl = config_.getDownloadConfig();
            std::string apriori_id =
                fmt::format("{}{}{:04d}{:03d}", dl.product_source, dl.product_type, dl.year, dl.doy);
            if (brdc_rts) {
                // Card 5 (plan D10): the a-priori is broadcast ephemeris, so the identity
                // is the nav-file basename ("brdc:<file>").  Once an RTS-orbit source is
                // attached (plan D9 seam / RTF-3), this becomes "brdc+rts:<mount>".
                const std::string nav = config_.getNavigationPath().filename().string();
                apriori_id = "brdc:" + (nav.empty() ? std::string("?") : nav);
            } else {
                const std::string sp3 = config_.getProductPath("SP3").filename().string();
                const std::string clk = config_.getProductPath("CLK").filename().string();
                if (!sp3.empty() || !clk.empty())
                    apriori_id = (sp3.empty() ? std::string("?") : sp3) + "+"
                               + (clk.empty() ? std::string("?") : clk);
            }
            pending_contract.apriori_product_id = apriori_id;
            // RTA-12: contract identity fields (each serialized as an optional
            // key=value S-line field ONLY when non-empty).
            //  - frame_id / coord_epoch are config DECLARATIONS (network.frame_id /
            //    network.coord_epoch; the SIF-coordinate path defaults coord_epoch
            //    to the propagation date) — no auto-detection.
            //  - atx_id is the RESOLVED antenna-calibration file this run actually
            //    loaded (basename of the merged product path).  Failure mode: when
            //    no .atx resolved, getProductPath returns an empty path and the
            //    atx= field is simply absent — a consumer that pins expect_atx_id
            //    then flags "file '' vs expected ...".
            pending_contract.frame_id    = nc.frame_id;
            pending_contract.coord_epoch = nc.coord_epoch;
            pending_contract.atx_id      = config_.getProductPath("ATX").filename().string();
            //  - orbit_ref: SP3 orbits + ATX satellite PCO/PCV mean the emitted
            //    clock C0 is referred to the satellite CENTER OF MASS.  Hardcoded
            //    until an SSR orbit source (APC-referred, per the RTCM SSR
            //    convention) lands, at which point this becomes a variable of the
            //    orbit path selection.
            pending_contract.orbit_ref   = "CoM";
            pending_contract.disc_threshold_m   = NetworkProductEmitter::kDefaultDiscThresholdM;  // R9: same constant as the emitter above
            // The replay-emitter construction parameters (disc threshold above + the
            // clock-disc channel flag + the RTA-11 staleness horizon) travel in the
            // contract so a consumer can replay the file with byte-identical
            // discontinuity counters and no out-of-band knowledge of the producer's
            // config.  The horizon serializes as an optional stale= field only when
            // non-zero, so an OFF run's v2 header is byte-identical.
            pending_contract.emit_clock_disc    = config_.networkEmitClockDisc();
            pending_contract.staleness_horizon_s = config_.networkEmitterStalenessHorizonS();
            pending_contract.present            = true;
            contract_pending = true;
            // The deferred writer only opens (and truncates) at the FIRST emit; a run
            // that emits zero epochs would otherwise leave a PREVIOUS run's complete
            // product file at this path for a downstream consumer to silently replay.
            // Match the v1 branch's truncate-at-startup by removing any existing file.
            std::error_code stale_ec;
            std::filesystem::remove(prod_path, stale_ec);
            if (stale_ec && std::filesystem::exists(prod_path))
                log->warn("Network: could not remove a stale product file at '{}' ({}) — "
                          "if this run emits no epochs, a downstream consumer would replay "
                          "the PREVIOUS run's products from that path.",
                          prod_path.string(), stale_ec.message());
            log->info("Network: will serialize per-epoch products with a v2 S-basis contract "
                      "(realized pivots stamped at first emit) to '{}'", prod_path.string());
        } else {
            product_writer = std::make_unique<NetworkProductWriter>(prod_path.string());
            if (product_writer->good()) {
                log->info("Network: serializing per-epoch products to '{}'", prod_path.string());
            } else {
                log->error("Network: failed to open product_output_path '{}' for writing; "
                           "disabling on-disk serialization for this run.", prod_path.string());
                product_writer.reset();
            }
        }
    }

    // --- Closed-loop consumer (held-out station) ----------------------------
    // The held-out station runs the user-side PPP-RTK + AR pipeline through a
    // RealTimeEngine fed epoch-by-epoch from RINEX replay, with its SSR
    // consumption repointed at the OWNED network_store (never the singleton).
    // Each epoch: the producer emits into network_store, then the consumer reads
    // that fresh epoch — closing the loop "does our own user engine recover
    // integers from our own products on an independent station?".
    GnssConfig consumer_config = config_.deriveClosedLoopConsumerConfig();
    std::unique_ptr<RealTimeEngine> consumer;
    if (closed_loop_on) {
        // Point the consumer's RinexParser at the held-out station so its header
        // (antenna type, approximate position) seeds the consumer correctly; the
        // per-epoch observations themselves come from the synced roster below.
        consumer_config.setObservationPath(cl.station.rinex_path);
        consumer = std::make_unique<RealTimeEngine>(consumer_config, log_manager_);
        if (!consumer->initialize()) {
            log->error("Network closed-loop: consumer engine initialize() failed; "
                       "disabling the harness for this run.");
            consumer.reset();
        } else {
            // Direct LAMBDA on the network-bias-corrected float must NOT also take
            // the MW WL (it is in the .BIA frame; see setArIgnoreMwWl).  wl_only AR
            // keeps the MW WL by definition.
            consumer->setArIgnoreMwWl(!consumer_config.getArWlOnly());
            const bool undiff = cl.consumer_undifferenced;
            if (undiff)
                log->info("Network closed-loop: UNDIFFERENCED user model — direct per-band "
                          "LAMBDA on the network-bias float (no BSSD, no WL/NL cascade); the "
                          "resolver forms the between-satellite difference internally. AR fires "
                          "once the float σ converges below the gate ({:.2f} m).",
                          consumer_config.getArMaxPositionSigma());
            else if (!consumer_config.getArWlOnly())
                log->warn("Network closed-loop: BSSD consumer with ar_wl_only=false runs PURE "
                          "full NL LAMBDA on the float — EXPERIMENTAL and slow on the v1 product "
                          "(the WL-float-level position makes the NL search ill-conditioned).  "
                          "Prefer ar_wl_only=true, or consumer_undifferenced=true.");
            if (cl.consume_network_products) {
                // Consume OUR products (never the singleton).  consume_network_clock
                // gates the clock overlay: false keeps precise SP3/CLK and consumes
                // only the network phase bias (the clean integer-recovery test).
                consumer->setSsrStore(&network_store, cl.consume_network_clock, /*bias=*/true);
                log->info("Network closed-loop: consumer '{}' initialized — AR mode {}, "
                          "wl_only {} — reading the OWNED network store ({}).",
                          cl.station.name, consumer_config.getArMode(),
                          consumer_config.getArWlOnly() ? "on" : "off",
                          cl.consume_network_clock
                              ? "full: network clock + phase bias"
                              : "bias-only: precise clock + network phase bias");
            } else {
                log->info("Network closed-loop: consumer '{}' initialized — AR mode {}, "
                          "wl_only {} — BASELINE mode (precise products only, network "
                          "store NOT injected).",
                          cl.station.name, consumer_config.getArMode(),
                          consumer_config.getArWlOnly() ? "on" : "off");
            }
        }
    }

    // Closed-loop metrics + trace.
    const Eigen::Vector3d cl_truth(cl.station.position_ecef.data());
    const Coordinates::Llh cl_truth_llh =
        closed_loop_on ? Coordinates::ecefToLlh(cl_truth) : Coordinates::Llh{};
    std::size_t cl_epochs = 0, cl_ar_ran = 0, cl_ar_fixed = 0;
    long long   cl_sum_fixed = 0;
    double cl_first_fix_tow = -1.0; int cl_first_fix_week = -1;
    std::deque<double> cl_recent_err3d;   // last-N 3D errors → converged-segment RMS
    constexpr std::size_t kClRecentWindow = 30;
    std::ofstream cl_trace;

    // D2 process noise (variance growth per second) for the carry-forward prior.
    const double q_clock = config_.getProcessNoise().sat_clock;        // m²/s
    const double q_pb    = config_.getProcessNoise().sat_phase_bias;   // m²/s
    const double q_cb    = config_.getProcessNoise().sat_code_bias;    // m²/s (W6 OSB carry-forward)

    // --- Trace --------------------------------------------------------------
    // P1: the three UNBOUNDED service CSVs (always-on network_trace.csv +
    // network_solution.csv, and the RTB-6 health heartbeat) rotate size-based
    // behind network.trace_rotate_mb.  Their header lines are hoisted into named
    // constants so the rotation reopen re-writes the IDENTICAL header the
    // startup open wrote — every rolled generation parses standalone.  Rotated
    // .1….keep suffixes are NOT auto-discovered by the pyviz viewers (they read
    // only the live file name); the operator concatenates the chain or points a
    // viewer at one generation — see docs/rt_service_operations.md.
    std::ofstream trace;
    const std::filesystem::path trace_path = log_dir_ / "network_trace.csv";
    static constexpr const char* kTraceHeader =
        "week,tow,n_present,n_clock_sats,n_phase_bias,declared_dim,"
        "gate_ok,solved,rms_clock_m,max_clock_m,n_pivot_pins\n";
    // Per-satellite product trace: the actual emitted solution (long format, one
    // row per shared parameter per epoch) so the producer's output is inspectable
    // — per-sat clock-Δ continuity, phase-bias stability, and discontinuity bumps.
    std::ofstream soln;
    const std::filesystem::path soln_path = log_dir_ / "network_solution.csv";
    static constexpr const char* kSolnHeader = "week,tow,sat,kind,signal,value_m,disc_counter\n";
    // Per-station, per-satellite back-substituted slant ionosphere (viz): one row
    // per (station, sat) each emitted epoch, with the measured elevation/azimuth
    // so the pyviz viewer can map each estimate to its ionospheric pierce point.
    std::ofstream iono_trace;
    const std::filesystem::path iono_path = log_dir_ / "network_ionosphere.csv";
    // Per-epoch co-estimated SH-VTEC coefficient trace (estimate_iono_sh +
    // global_vtec_sh.trace): one row per (n,m,trig) coefficient with its value and
    // formal std (TECU), so the estimated field is inspectable / comparable to a GIM.
    std::ofstream vtec_sh_trace;
    const std::filesystem::path vtec_sh_path = log_dir_ / "network_vtec_sh.csv";
    // Deep-investigation slip/RAIM diagnostics (network.trace_slip_events): the IF-phase
    // pre-detector's per-(station,sat,pair) ΔΔφ_IF and full threshold decision each epoch,
    // and RAIM's per-(sat,station) post-fit standardized residual + cross-station consensus.
    // The pair of files that make an IF-slip "need to reset" and a RAIM trigger inspectable.
    std::ofstream if_slip_trace;
    const std::filesystem::path if_slip_trace_path = log_dir_ / "network_if_slip.csv";
    std::ofstream raim_trace;
    const std::filesystem::path raim_trace_path = log_dir_ / "network_raim.csv";
    std::ofstream raim_attrib_trace;
    const std::filesystem::path raim_attrib_trace_path = log_dir_ / "network_raim_attrib.csv";
    // Per-satellite clock-monitor diagnostics (network.clock_monitor + trace_slip_events):
    // one row per satellite near or over the refloat bound each epoch, so the abs/jump
    // decision and the resulting reinit are inspectable against the emitted clock series.
    std::ofstream clk_mon_trace;
    const std::filesystem::path clk_mon_trace_path = log_dir_ / "network_clock_monitor.csv";
    // B7: per-satellite a-priori-clock-anchor information fraction (anchor_info /
    // reduced-normals total_info) each emitted epoch, so validation can certify the
    // estimable clock is DATA-dominated, not defined by the a-priori CODE product it
    // is softly anchored to.  Pure diagnostic (network.trace_anchor_info).
    std::ofstream anchor_info_trace;
    const std::filesystem::path anchor_info_trace_path = log_dir_ / "network_anchor_info.csv";
    // B5: cross-station between-satellite DD float ambiguities (from reconstructStation),
    // per (const,signal), with their fractional-cycle residual — a sign-convention
    // self-check + empirical <0.1-cycle-support evidence for user AR (gate for producer
    // AR).  Pure diagnostic (network.trace_dd_fractional).
    std::ofstream dd_frac_trace;
    const std::filesystem::path dd_frac_trace_path = log_dir_ / "network_dd_fractional.csv";
    std::unique_ptr<PppUtils> dd_ppp_utils;   // B5: wavelength provider (built only when the DD trace is on)
    // RAW cycle-slip observables per (station,sat) every epoch (network.trace_slip_raw):
    // Melbourne-Wübbena, geometry-free L1−L2, first-signal phase-minus-code (all m) and the
    // per-band RINEX LLI — computed from raw_observed, INDEPENDENT of any detector being on.
    // The always-available "was there a slip on ANY sat here?" table; in particular the only
    // network CSV that surfaces the receiver LLI ground truth.  Epoch diffs are formed offline.
    std::ofstream slip_raw_trace;
    const std::filesystem::path slip_raw_trace_path = log_dir_ / "network_slip_raw.csv";
    std::unique_ptr<PppUtils> slip_raw_ppp_utils;  // wavelength provider (built only when the raw-slip trace is on)
    // L1: per-epoch disposition + σ̂0 + clock-stats trace (network.trace_epoch_status).
    // ONE row per processed epoch (emitted OR skipped) — the disposition timeline the
    // V1/V3/V6 validation cards are judged on.  Off by default (byte-identical when OFF).
    std::ofstream epoch_status_trace;
    const std::filesystem::path epoch_status_trace_path = log_dir_ / "network_epoch_status.csv";
    // RTB-6: operator health heartbeat (network.health_interval_s > 0) — a periodic
    // machine-readable @c network_service_health.csv appended every N s of CONTENT
    // time so a long-running service is observable without waiting for the exit
    // summary.  Named "service_health" because the pyviz viewer called
    // network_health reads network_trace.csv — a DIFFERENT surface (per-epoch
    // estimability, not service liveness).  sigma0_ewma reports −1 ("unavailable")
    // unless its feeding tiers (emit_ura + raim + phase_leveling) are all armed —
    // the EWMA only updates inside the warm post-fit RAIM block, so without them it
    // would silently freeze at its 1.0 init.  Off by default (byte-identical when
    // OFF: no file opened, no rows written).  Column semantics:
    // docs/rt_service_operations.md.
    std::ofstream health_trace;
    const std::filesystem::path health_trace_path = log_dir_ / "network_service_health.csv";
    static constexpr const char* kHealthHeader =
        "week,tow,n_emitted,n_skipped,n_gate_withheld,n_sigma0_withheld,"
        "n_runaway_epochs,sigma0_ewma,n_present,n_clock_sats,"
        "ms_per_epoch_window,last_emit_age_s,pivot,cold_start_grade_s\n";
    // L2: structured per-event QC log (network.trace_events).  One LONG-format row per
    // discrete QC action across every tier — the machine-readable replacement for
    // spdlog regex-scraping.  Off by default (byte-identical when OFF).
    std::ofstream events_trace;
    const std::filesystem::path events_trace_path = log_dir_ / "network_events.csv";
    // L7: per-station ZWD trace (network.trace_zwd) — estimated ZWD + datum-conditioned
    // formal σ + carried flag, per present station each emitted epoch (V7 instrument).
    std::ofstream zwd_trace;
    const std::filesystem::path zwd_trace_path = log_dir_ / "network_zwd.csv";
    // L13: per-station residual field (network.trace_station_residuals) — EVERY post-fit RAIM
    // cell (non-pivot station x sat) with s_raw / s_norm / σ̂0.  Heavy opt-in deep trace.
    std::ofstream station_res_trace;
    const std::filesystem::path station_res_trace_path = log_dir_ / "network_station_residuals.csv";
    {
        std::error_code ec;
        std::filesystem::create_directories(log_dir_, ec);
        trace.open(trace_path);
        if (trace) {
            trace << kTraceHeader;
        } else {
            log->warn("Network: failed to open trace file {}", trace_path.string());
        }
        soln.open(soln_path);
        if (soln) soln << kSolnHeader;
        else log->warn("Network: failed to open solution trace {}", soln_path.string());

        if (nc.trace_ionosphere) {
            iono_trace.open(iono_path);
            if (iono_trace)
                iono_trace << "week,tow,station,sat,iono_m,elevation_deg,azimuth_deg\n";
            else
                log->warn("Network: failed to open ionosphere trace {}", iono_path.string());
        }

        if (config_.estimateNetworkIonoSh() && shc.trace) {
            vtec_sh_trace.open(vtec_sh_path);
            if (vtec_sh_trace)
                vtec_sh_trace << "week,tow,n,m,trig,value_tecu,std_tecu\n";
            else
                log->warn("Network: failed to open SH-VTEC trace {}", vtec_sh_path.string());
        }

        if (closed_loop_on && consumer) {
            const std::filesystem::path cl_path = log_dir_ / "closed_loop_trace.csv";
            cl_trace.open(cl_path);
            if (cl_trace)
                cl_trace << "week,tow,n_sats,ar_ran,ar_success,num_fixed,num_candidates,"
                            "ratio,pos_err_3d_m,east_m,north_m,up_m\n";
            else
                log->warn("Network: failed to open closed-loop trace {}", cl_path.string());
        }

        if (nc.trace_slip_events) {
            if_slip_trace.open(if_slip_trace_path);
            if (if_slip_trace)
                // dd = between-sat/between-epoch IF ΔΔφ (m); med = network pool median; s_pool
                // = elevation-normalized pooled scale; ewma_scale/count = this station-pair's
                // own EWMA scale + clean-sample count (decision-time, pre-update); w/s_eff =
                // shrinkage weight + effective scale; elev_map = 1/sin el; thr/floor = gate;
                // abs_dev = |dd−med|; flagged/carry_warm = decision + whether it refloated.
                if_slip_trace << "week,tow,station,sat,sig1,sig2,elev_deg,dd_m,med_m,"
                                 "s_pool_m,ewma_scale_m,count,w_shrink,s_eff_m,elev_map,"
                                 "thr_m,floor_m,abs_dev_m,flagged,carry_warm\n";
            else
                log->warn("Network: failed to open IF-slip trace {}", if_slip_trace_path.string());
            raim_trace.open(raim_trace_path);
            if (raim_trace)
                // One row per (sat, station) whose normalized post-fit residual spiked, or any
                // station of a refloated sat: s_max_raw = v·√w (measurement-standardized), s_max_norm
                // = that ÷ the robust a-posteriori variance factor sigma0, vs raim_k; the per-sat
                // spiked-station count vs the min_sta consensus; and whether the sat was refloated.
                raim_trace << "week,tow,sat,station,s_max_raw,s_max_norm,sigma0,raim_k,spiked,"
                              "n_sta_spiked,min_sta,refloated\n";
            else
                log->warn("Network: failed to open RAIM trace {}", raim_trace_path.string());
            raim_attrib_trace.open(raim_attrib_trace_path);
            if (raim_attrib_trace)
                // Prominence attribution (LOG-ONLY, does not change the refloat action):
                // robust 2-way median polish of the (station × sat) post-fit residual table
                // each RAIM epoch → per-dimension effect + robust-z prominence.  dim ∈
                // {SAT, STA, SIG, ARC}; acted = whether today's per-sat consensus already
                // refloats that entity (SAT/ARC only).  Only entities above the report
                // prominence are emitted, so quiet epochs cost nothing.
                raim_attrib_trace << "week,tow,dim,entity,effect,prominence,n_cells,overall,"
                                     "sigma0,acted\n";
            else
                log->warn("Network: failed to open RAIM attribution trace {}",
                          raim_attrib_trace_path.string());
        }
        if (nc.trace_slip_events && (config_.networkClockMonitor() || config_.networkClockModel())) {
            clk_mon_trace.open(clk_mon_trace_path);
            if (clk_mon_trace)
                // clk = solved Δδtˢ (m); median = network median (gauge); dev = clk−median;
                // prev_clk/jump = previous adjacent-epoch value + step (blank if unavailable);
                // flag_abs/flag_jump = this monitor's own bounds; flag_ext = already flagged by
                // the within-epoch / post-fit RAIM; refloated = carried clock reinitialized.
                clk_mon_trace << "week,tow,sat,clk_m,median_m,dev_m,prev_clk_m,jump_m,"
                                 "abs_max_m,jump_max_m,flag_abs,flag_jump,flag_ext,refloated,"
                                 "nis,mode\n";   // L4: nis,mode appended (trailing-column format break)
            else
                log->warn("Network: failed to open clock-monitor trace {}",
                          clk_mon_trace_path.string());
        }
        if (nc.trace_anchor_info) {
            anchor_info_trace.open(anchor_info_trace_path);
            if (anchor_info_trace)
                // anchor_info = constant 1/initial_std.sat_clock² (m⁻²), the a-priori
                // CODE-product Tikhonov pull; total_info = reduced shared-normals diagonal
                // on this sat's clock column (data + carry prior + datum pin for the pivot);
                // carry_info = the carried clock-prior info folded this epoch (scalar D2 or
                // the two-state model); data_info = total − carry − anchor (pivot pin still
                // in it, hence is_pivot); anchor_frac_data = anchor/(anchor+data) — the
                // HONEST data-vs-anchor fraction, INDEPENDENT of carry stiffness (P1-4);
                // carry_frac = carry/total exposes a carry-dominated lock-in (high ⇒ the
                // clock is defined by its own past, not by data); frac = the legacy
                // anchor/total (kept for continuity; misleads for a locked-in clock).
                anchor_info_trace << "week,tow,sat,anchor_info,carry_info,data_info,total_info,"
                                     "anchor_frac_data,carry_frac,frac,is_pivot\n";
            else
                log->warn("Network: failed to open anchor-info trace {}",
                          anchor_info_trace_path.string());
        }
        if (nc.trace_dd_fractional) {
            dd_frac_trace.open(dd_frac_trace_path);
            if (dd_frac_trace) {
                // Between-station DD of undifferenced float ambiguities (m): sat_i minus the
                // per-(const,signal) reference sat, differenced across station pair (sta_a,
                // sta_b).  P1-2: lambda_m / dd_cyc / frac_cyc are now computed here (frac_cyc =
                // dd_cyc − round(dd_cyc)) so the CSV is directly a fractional-cycle histogram —
                // the go/no-go instrument for float-bias user AR.  GLONASS (FDMA) and the pivot
                // station (pinned ambiguities) are excluded from the pool below.
                dd_frac_trace << "week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,"
                                 "dd_m,lambda_m,dd_cyc,frac_cyc\n";
                dd_ppp_utils = std::make_unique<PppUtils>(log_manager_);
            } else {
                log->warn("Network: failed to open DD-fractional trace {}",
                          dd_frac_trace_path.string());
            }
        }
        if (nc.trace_slip_raw) {
            slip_raw_trace.open(slip_raw_trace_path);
            if (slip_raw_trace) {
                // Per-(station,sat) RAW slip observables for the primary dual-freq pair, every
                // epoch, computed straight from raw_observed (phase in cycles, code in metres) —
                // NO dependence on any detector being armed.  mw_m/gf_m/l1_c1_m are absolute (the
                // WL slip step is λ_WL≈0.86 m/cyc in mw_m, an L1/L2 slip jumps gf_m); lli1/lli2
                // are the RINEX loss-of-lock flags for sig1/sig2 (bit 0 = slip).  Diff adjacent
                // epochs offline for the slip signature.  Pivot station included (diagnostic).
                slip_raw_trace << "week,tow,station,sat,sig1,sig2,elev_deg,"
                                  "mw_m,gf_m,l1_c1_m,lli1,lli2\n";
                slip_raw_ppp_utils = std::make_unique<PppUtils>(log_manager_);
            } else {
                log->warn("Network: failed to open raw-slip trace {}",
                          slip_raw_trace_path.string());
            }
        }
        if (nc.trace_epoch_status) {
            epoch_status_trace.open(epoch_status_trace_path);
            if (epoch_status_trace)
                // ONE row per PROCESSED epoch, written before each `continue` (skip) and at
                // the emit site.  disposition classifies WHY the epoch was skipped; sigma0 /
                // n_adapt / n_refloat_sats / n_retro_applied / datum_changed capture the QC
                // actions; clock_median_m/std_m/worst_sat/worst_dev_m are the clock-Δ spread
                // (median+std+worst-sat convention).  BLANK (not 0) fields = not-in-scope at
                // that disposition site.  sigma0_trigger/withhold_ceiling are config echoes so
                // viewers draw honest threshold lines.  Note: fast-forward (start_offset_hours)
                // and max-duration epochs are uncounted by design and write NO row.
                epoch_status_trace << "week,tow,disposition,sigma0,n_adapt,n_present,"
                                      "n_clock_sats,clock_median_m,clock_std_m,worst_sat,"
                                      "worst_dev_m,n_refloat_sats,n_retro_applied,datum_changed,"
                                      "sigma0_trigger,withhold_ceiling\n";
            else
                log->warn("Network: failed to open epoch-status trace {}",
                          epoch_status_trace_path.string());
        }
        if (nc.health_interval_s > 0.0) {
            // RTB-6: periodic operator health heartbeat.  Cumulative epoch tallies +
            // σ̂0 EWMA + windowed ms/epoch + last-emit age + stations/clock-sats +
            // realized pivots, appended every network.health_interval_s of content
            // time.  A service supervisor (or a human) polls this instead of the
            // exit-only summary.
            health_trace.open(health_trace_path);
            if (health_trace)
                health_trace << kHealthHeader;
            else
                log->warn("Network: failed to open service-health trace {}",
                          health_trace_path.string());
            // P4(a): the σ̂0 EWMA updates ONLY inside the warm post-fit RAIM block
            // (emit_ura && raim && phase_leveling); with any of them unarmed the
            // column would silently report its frozen 1.0 init.  It emits the −1
            // "unavailable" sentinel instead — warn once so the operator knows the
            // quality channel of this heartbeat is dark, not clean.
            if (!(config_.networkEmitUra() && config_.networkRaim()
                  && config_.networkPhaseLeveling()))
                log->warn("Network (RTB-6): health heartbeat is armed but the σ̂0 EWMA's "
                          "feeding tiers are not (needs network.emit_ura + network.raim + "
                          "network.phase_leveling; ura={}, raim={}, phase_leveling={}) — "
                          "the sigma0_ewma column will report the -1 'unavailable' "
                          "sentinel for this run.",
                          config_.networkEmitUra(), config_.networkRaim(),
                          config_.networkPhaseLeveling());
        }
        if (nc.trace_events) {
            events_trace.open(events_trace_path);
            if (events_trace)
                // Long format: one row per discrete QC action.  tier in {T1_if_slip,
                // T2_within_epoch, T3_raim, T4_attrib, T5_clock_model, T6_clock_monitor,
                // GATE, SIGMA0, PIVOT, RETRO}.  statistic/threshold blank where not in
                // scope; detail is comma-sanitized (',' -> ';').  Rows are written even on
                // epochs that are subsequently WITHHELD — that is the disposition story.
                events_trace << "week,tow,tier,event,sat,station,statistic,threshold,"
                                "action,detail\n";
            else
                log->warn("Network: failed to open events trace {}", events_trace_path.string());
        }
        if (nc.trace_zwd) {
            zwd_trace.open(zwd_trace_path);
            if (zwd_trace)
                // zwd_m = a-priori state + reconstructed correction (the estimated correction
                // about the modeled a-priori); std_m = datum-conditioned formal σ; carried = a
                // ZWD carry prior was actually injected this epoch.
                zwd_trace << "week,tow,station,zwd_m,std_m,carried\n";
            else
                log->warn("Network: failed to open ZWD trace {}", zwd_trace_path.string());
        }
        if (nc.trace_station_residuals) {
            station_res_trace.open(station_res_trace_path);
            if (station_res_trace)
                // One row per post-fit RAIM cell: non-pivot station (pivot never appears — it is
                // skipped in the cell loop) x carried-ambiguity sat.  s_raw = measurement-
                // standardized residual (B2-normalized when raim_redundancy_normalize on),
                // s_norm = s_raw / σ̂0, sigma0 = the epoch robust variance factor.  s values are
                // ABSOLUTE (non-negative).  Rows exist only for RAIM epochs (raim && carry_warm).
                station_res_trace << "week,tow,station,sat,s_raw,s_norm,sigma0\n";
            else
                log->warn("Network: failed to open station-residual trace {}",
                          station_res_trace_path.string());
        }
    }

    // L1: one network_epoch_status.csv row per processed epoch.  Fields not yet in
    // scope at a given disposition site are left BLANK (std::nullopt), NOT zero — the
    // pyviz timeline viewers (L5) distinguish "unavailable" from a real 0.
    struct EpochStatusRow {
        std::optional<double> sigma0;
        std::optional<int>    n_adapt;
        std::optional<int>    n_present;
        std::optional<int>    n_clock_sats;
        std::optional<double> clock_median_m;
        std::optional<double> clock_std_m;
        std::string           worst_sat;        // empty string => blank
        std::optional<double> worst_dev_m;
        std::optional<int>    n_refloat_sats;
        std::optional<int>    n_retro_applied;
        std::optional<int>    datum_changed;
    };
    const double es_sigma0_trigger   = config_.networkRaimSigma0Trigger();   // config echo
    const double es_withhold_ceiling = config_.networkRaimWithholdSigma0();  // config echo

    // === Epoch loop =========================================================
    std::vector<CarryPrior> prev_carry;                 // D2 stash from the last solve
    SharedCarryCov prev_carry_cov;                      // C1: dense covariance side-car of prev_carry (empty when OFF)
    std::map<std::string, std::string> prev_pivot;      // constellation -> pivot sat (handover detect)
    std::set<std::string> c1_zoh_active;                 // L2/T5: sats currently emitting c1=0 (ZOH) — edge-trigger for the R6 event
    // A1: persistent last-EVER pivot per constellation; MERGE-updated (never
    // wholesale-overwritten) so a constellation that vanishes for an epoch and
    // returns with a DIFFERENT pivot still raises datum_changed.  Read only when
    // network.pivot_vanish_return_signal is on (otherwise inert).
    std::map<std::string, std::string> last_pivot;
    // Prioritized phase-bias anchor (network.phase_datum_prioritized) — all three
    // keyed by the "sat_signal" ambiguity form, all empty/unread on the legacy path:
    //  - anchor_incumbent: the STICKY incumbent anchor STATION NAME per (sat,signal),
    //    read when electing (kept while it still tracks the sat), advanced at election.
    //  - this_epoch_anchor: the anchor STATION NAME elected THIS epoch, captured at
    //    election and read at the emit site (decouples emit from the PASS-A scratch).
    //  - emitted_anchor: the anchor NAME of the LAST EMITTED product per (sat,signal),
    //    advanced ONLY at emit (mirrors prev_pivot/last_pivot); a change vs this drives
    //    the per-satellite handover discontinuity.  Never erased (a returning key is a
    //    first sighting, not a fault — matches the last_pivot never-erase policy).
    std::map<std::string, std::string> anchor_incumbent;
    std::map<std::string, std::string> this_epoch_anchor;
    std::map<std::string, std::string> emitted_anchor;
    // Per-satellite clock sanity monitor (network.clock_monitor): the previous emitted
    // clock value + the continuous-GPS second it was stashed, per satellite, for the
    // adjacent-epoch jump test.  Erased on refloat so the post-reinit clock re-baselines.
    std::map<std::string, std::pair<double, double>> prev_clk;  // sat -> {Δδtˢ (m), seen_s}
    // Two-state oscillator clock state per satellite (network.clock_model): the carried
    // [offset, drift] + 2×2 covariance, persisted across epochs.  Empty / inert unless clk_model_on.
    std::map<std::string, ClockOsc> clock_osc;   // per-sat carried [offset,drift]+cov+t_last (P0-3: t_last drives per-sat prediction/drift-FD across gaps)
    // Card 6 (RTF-2, network.apriori_source = brdc_rts): the broadcast-ephemeris IODE
    // that each satellite's CARRIED clock state (clock_osc.x0 / the D2 clock carry /
    // the clock-monitor jump baseline) is currently referenced to.  Advanced at the
    // eph-cutover RE-LEVEL (before the solve), NOT deferred to the emit site like
    // prev_pivot — the re-level MUTATES the carried state whether or not this epoch
    // emits (the post-emit carry updates are skipped on a withhold/solve-fail, so the
    // shifted values persist), so deferring the advance would re-detect the same roll
    // next epoch and DOUBLE-shift.  Empty / inert unless brdc_rts.  (The plan called
    // this last_emitted_iode_; "referenced-to" is the honest name since it advances on
    // withheld epochs too.)
    std::map<std::string, int> relevel_iode;
    // Card 6: sats whose eph rolled but whose PRIOR version was already evicted from
    // the store, so Δ_eph is incomputable — a forced per-sat discontinuity is queued
    // and lands at the sat's next actual emit (present/pending split like
    // ura_retro_sats).  Effectively unreachable in steady state (per-epoch checking
    // makes two rolls between consecutive checks impossible); a defensive belt for a
    // snapshot gap / two-roll corner.
    std::set<std::string> eph_cutover_disc_pending;
    double start_anchor_s = -1.0;                        // first-epoch time anchor for network.start_offset_hours
    // Phase-leveling per-station ambiguity carry (Increment 1): persistent across
    // epochs, keyed by (station index, sat, signal); ORDERED so the inject order
    // (which feeds rank-1 LDLᵀ updates) is byte-identical serial≡parallel.  Inert
    // unless network.phase_leveling.
    std::map<std::tuple<int, std::string, std::string>, LocalCarry> local_carry;
    // Per-station ZWD carry (B9): persistent across epochs, keyed by station index
    // (ZWD is one-per-station).  Inert unless network.carry_zwd.
    std::map<int, LocalCarry> zwd_carry;
    // B1 URA quality channel carried state (inert unless network.emit_ura).
    double                       ura_sigma0_ewma = 1.0;   ///< network-wide σ̂0 EWMA scaling the emitted URA (one-epoch lag).
    std::map<std::string, int>   ura_refloat_horizon;     ///< sat -> epochs remaining of post-refloat URA inflation.
    // B1 retro-bump (P0-2/P1-3): per-SATELLITE, tier-independent, edge-triggered.
    // ura_retro_sats = sats a post-emit QC tier flagged LAST epoch, applied as a
    // per-sat discontinuity at the sat's NEXT ACTUAL emit (markSatDiscontinuity):
    // a flagged sat absent from an emitted epoch (RT0-5 quarantine, vanish) stays
    // pending here rather than being consumed without landing.  retro_active =
    // sats currently flagged and already bumped, so a sat that stays flagged during
    // recovery is bumped ONCE (on its rising edge) — no per-constellation reset storm.
    std::set<std::string>        ura_retro_sats;
    std::set<std::string>        retro_active;
    // RTB-7(b/c): startup quality-ramp state.  The DEGRADED URA floor
    // (nc.warmup_ura_floor_m) applies until ramp_lifted; the lift rule is
    // networkWarmupRampLifted (emitted-epoch horizon AND σ̂0-EWMA update count,
    // the count waived when the EWMA is structurally unavailable).  All inert
    // (never read) while the floor is 0 — byte-identical.  On an RTB-7(a) WARM
    // resume these restore from the snapshot: a warm-resumed producer is NOT
    // degraded and reports grade 0 ("warm").
    size_t ura_ewma_updates     = 0;     ///< σ̂0 EWMA updates received (post-fit RAIM tier).
    bool   ramp_lifted          = false; ///< Warmup URA floor lifted (product grade reached).
    double first_emit_content_s = -1.0;  ///< Content time of the FIRST emit (grade anchor).
    double grade_metric_s       = -1.0;  ///< RTB-7(c) cold-start-to-product-grade (s); −1 before lift / ramp off; 0 = warm resume.
    const bool   pl_on         = config_.networkPhaseLeveling();
    const double pl_q_amb      = config_.networkPhaseLevelingQAmb();
    const int    pl_warmup     = config_.networkPhaseLevelingWarmupEpochs();
    const double pl_max_gap_s  = config_.networkPhaseLevelingMaxGapS();
    const double pl_var_floor  = config_.networkPhaseLevelingVarFloorM2();  // lock-in guard; 0 = off
    const double pb_var_floor  = config_.networkSharedCarryVarFloorPbM2();  // B4: D2 phase-bias carry floor; 0 = off
    const double cb_var_floor  = config_.networkSharedCarryVarFloorCbM2();  // B4: D2 code-bias carry floor;  0 = off
    const bool   fullcov_on    = config_.networkSharedCarryFullCov();       // C1: dense (P_prev+Q)⁻¹ shared carry
    const bool   zwd_on        = config_.networkCarryZwd();                 // B9: carry per-station ZWD across epochs
    const double zwd_q         = config_.networkZwdCarryQ();                // B9: ZWD carry random-walk process noise (m²/s)
    const bool   ura_on        = config_.networkEmitUra();                  // B1: emit per-sat URA quality channel
    const double ura_alpha     = config_.networkUraEwmaAlpha();             // B1: σ̂0 EWMA smoothing
    const bool   ura_retro     = config_.networkUraRetroBump();             // B1: retro-bump flagged SATELLITE(s) on next emit
    const double ura_refloat_inflate = config_.networkUraRefloatInflate(); // B1: URA multiplier post-refloat
    const bool   raim_on       = config_.networkRaim();            // post-fit network RAIM (Increment 3, T2)
    const double raim_k        = config_.networkRaimKSigma();      // standardized post-fit residual flag threshold
    const int    raim_min_sta  = config_.networkRaimMinStations(); // cross-station consensus count
    const double raim_withhold = config_.networkRaimWithholdSigma0(); // A4: >0 withholds (not emits) an epoch whose post-adaptation σ̂0 exceeds this ceiling; 0 = off
    const bool   raim_redun    = config_.networkRaimRedundancyNormalize(); // B2: Baarda per-row r_i normalization of the RAIM residual
    const bool   clk_mon_on    = config_.networkClockMonitor();       // per-sat carried-clock sanity monitor + refloat
    const double clk_mon_abs   = config_.networkClockMonitorAbsMaxM();  // |Δδtˢ − median| refloat bound (m)
    const double clk_mon_jump  = config_.networkClockMonitorJumpMaxM(); // one-epoch clock jump refloat bound (m)
    const bool   clk_mon_couple = clk_mon_on && config_.networkClockMonitorCoupleRaim();  // also refloat RAIM-flagged sats' clocks (opt-in)
    const double clk_mon_reinit_sig = config_.networkClockMonitorReinitSigmaM();  // >0: inflate-q to this σ; ≤0: hard skip-stash refloat
    const double clk_mon_reinit_var = clk_mon_reinit_sig * clk_mon_reinit_sig;   // reinit variance folded next epoch for a flagged sat
    const bool   pivot_vr_signal = config_.networkPivotVanishReturnSignal();  // A1: also signal a vanish-return pivot change
    const bool   emit_clock_disc = config_.networkEmitClockDisc();            // A2: clock disc channel + per-constellation bump scoping
    const bool   sticky_pivot    = config_.networkStickyPivot();              // RTA-1: keep the incumbent clock pivot while it stays registered
    const bool   clock_sum_to_zero = config_.networkClockDatumSumToZero();    // anchor-free per-constellation clock gauge (Σδtˢ=0) instead of a pivot pin — no pivot/handover machinery
    const bool   scoped_handover = config_.networkScopedHandoverResponse();   // RTA-2: per-constellation scoping of the INTERNAL handover response
    const bool   emit_gen_time   = config_.networkEmitGenerationTime();       // RT0-4: stamp wall-clock time-of-generation per emitted epoch
    const bool   emit_gen_id     = config_.networkEmitGenerationId();         // RTA-3: stamp a per-run solution-generation id (first-emit epoch time)
    const bool   ser_disc_ctrs   = config_.networkSerializeDiscCounters();    // RTA-4: serialize post-emit disc-counter VALUES on C/B/P records
    const double contract_rebroadcast_s = config_.networkContractRebroadcastS(); // RTA-5: S-contract re-broadcast period (product time, s; 0 = off)
    const bool   clk_runaway_on  = config_.networkClockRunawayCheck();        // RT0-5: per-constellation clock-runaway quarantine
    const double clk_runaway_mag = config_.networkClockRunawayMagM();         // RT0-5: median |clock−global median| ceiling (m)
    const double clk_runaway_rate= config_.networkClockRunawayRateMps();      // RT0-5: median |drift| ceiling (m/s)
    const bool   perconst_on       = config_.networkPerConstWithhold();          // RTA-10: per-constellation carve-out of the A4 σ̂0 withhold
    const int    perconst_min_rows = config_.networkPerConstWithholdMinRows();   // RTA-10: min σ̂0-pool rows for a constellation to be judgeable
    const bool   emit_epoch_status = config_.networkEmitEpochStatusRecord();     // RTA-10: per-epoch H heartbeat in the product file (incl. withheld epochs)
    // Two-state (offset+drift) oscillator clock model (network.clock_model): when on, the
    // satellite clock is carried via the Allan-derived Q model in clock_osc (below) instead of
    // the scalar prev_carry random walk, and a satellite is flagged by the normalized-innovation
    // dynamics-fault test rather than the absolute/jump bounds.
    const bool   clk_model_on   = config_.networkClockModel();
    const double clk_model_q1   = config_.networkClockModelQ1();
    const double clk_model_q2   = config_.networkClockModelQ2();
    const double clk_model_k    = config_.networkClockModelKSigma();
    const double clk_model_r =   // B3: σ_floor² lower bound on the clock-model fault variance S
        config_.networkClockModelMeasStdM() * config_.networkClockModelMeasStdM();
    const double clk_model_max_gap = config_.networkClockModelMaxGapS();  // R11: clock-model carry reseed / drift-reset / C1-ZOH bound (defaults to pl_max_gap_s)
    const double start_offset_s = config_.networkStartOffsetHours() * 3600.0;  // testing fast-forward (0 = off)

    // L15: run-provenance manifest (network.write_run_manifest).  JSONL — a startup record
    // here (config hash + build + full toggle/knob dump) plus an end-of-run record appended
    // after the completion line.  Hand-emitted (no JSON lib); does NOT read git state (the
    // tree is shared across concurrent sessions — config hash + toggle dump is the honest
    // provenance).  create_directories(log_dir_) already ran in the trace-open scope above.
    const std::filesystem::path manifest_path = log_dir_ / "network_run_manifest.json";
    if (nc.write_run_manifest) {
        auto jesc = [](const std::string& s) {
            std::string o; o.reserve(s.size() + 8);
            for (char c : s) {
                if (c == '\\' || c == '"') { o.push_back('\\'); o.push_back(c); }
                else if (c == '\n') o += "\\n";
                else if (c == '\r') o += "\\r";
                else o.push_back(c);
            }
            return o;
        };
        uint64_t cfg_hash = 1469598103934665603ULL;   // FNV-1a-64 over the raw config bytes
        {
            std::ifstream cf(config_path_, std::ios::binary);
            char buf[4096];
            while (cf && (cf.read(buf, sizeof buf) || cf.gcount())) {
                const std::streamsize n = cf.gcount();
                for (std::streamsize k = 0; k < n; ++k)
                    cfg_hash = (cfg_hash ^ static_cast<unsigned char>(buf[k])) * 1099511628211ULL;
                if (n < static_cast<std::streamsize>(sizeof buf)) break;
            }
        }
        char utc[32] = "unknown";
        {
            std::time_t t = std::time(nullptr);
            std::tm tmv{};
#ifdef _WIN32
            gmtime_s(&tmv, &t);
#else
            gmtime_r(&t, &tmv);
#endif
            std::strftime(utc, sizeof utc, "%Y-%m-%dT%H:%M:%SZ", &tmv);
        }
        std::ofstream mf(manifest_path, std::ios::trunc);
        if (mf) {
            mf << "{\"record\":\"startup\","
               << "\"written_utc\":\"" << utc << "\","
               << "\"config_path\":\"" << jesc(config_path_.string()) << "\","
               << fmt::format("\"config_hash_fnv1a64\":\"{:016x}\",", cfg_hash)
               << "\"build\":{\"compiled\":\"" << __DATE__ " " __TIME__ << "\"},"
               << "\"network\":{"
               // RTB-8/P5: bundle provenance — records that the run's toggle set
               // came from the service profile rather than hand-assembly.
               << "\"service_mode\":" << (config_.networkServiceMode() ? "true" : "false") << ','
               << "\"stations\":" << stations.size() << ','
               << "\"clock_datum_station\":\"" << jesc(nc.clock_datum_station) << "\","
               << "\"clock_datum_satellite\":\"" << jesc(nc.clock_datum_satellite) << "\","
               << fmt::format("\"epoch_sync_tolerance_s\":{},", nc.epoch_sync_tolerance_s)
               << "\"product_output_path\":\"" << jesc(nc.product_output_path) << "\","
               << "\"product_flush_per_epoch\":" << (nc.product_flush_per_epoch ? "true" : "false") << ','
               << "\"parallel_stations\":" << (nc.parallel_stations ? "true" : "false") << ','
               << "\"parallel_min_stations\":" << nc.parallel_min_stations << ','
               << "\"parallel_max_threads\":" << nc.parallel_max_threads << ','
               << "\"parallel_reduce\":" << (nc.parallel_reduce ? "true" : "false") << ','
               << "\"n_threads\":" << n_threads << ','
               << "\"trace_stations\":" << (nc.trace_stations ? "true" : "false") << ','
               << "\"trace_ionosphere\":" << (nc.trace_ionosphere ? "true" : "false") << ','
               << "\"trace_estimability\":" << (nc.trace_estimability ? "true" : "false") << ','
               << "\"trace_slip_events\":" << (nc.trace_slip_events ? "true" : "false") << ','
               << "\"trace_anchor_info\":" << (nc.trace_anchor_info ? "true" : "false") << ','
               << "\"trace_dd_fractional\":" << (nc.trace_dd_fractional ? "true" : "false") << ','
               << "\"trace_flag_rates\":" << (nc.trace_flag_rates ? "true" : "false") << ','
               << "\"trace_epoch_status\":" << (nc.trace_epoch_status ? "true" : "false") << ','
               << "\"trace_events\":" << (nc.trace_events ? "true" : "false") << ','
               << "\"trace_zwd\":" << (nc.trace_zwd ? "true" : "false") << ','
               << "\"trace_clock_full\":" << (nc.trace_clock_full ? "true" : "false") << ','
               << "\"trace_station_residuals\":" << (nc.trace_station_residuals ? "true" : "false") << ','
               // RTB-6/P1: service-observability knobs (health heartbeat + trace rotation).
               << fmt::format("\"health_interval_s\":{},", nc.health_interval_s)
               << fmt::format("\"trace_rotate_mb\":{},", nc.trace_rotate_mb)
               << "\"trace_rotate_keep\":" << nc.trace_rotate_keep << ','
               // RTB-7: startup quality ramp + state snapshot / warm resume.
               << fmt::format("\"warmup_ura_floor_m\":{},", nc.warmup_ura_floor_m)
               << "\"warmup_epochs\":" << nc.warmup_epochs << ','
               << "\"state_snapshot_path\":\"" << jesc(nc.state_snapshot_path) << "\","
               << fmt::format("\"state_snapshot_interval_s\":{},", nc.state_snapshot_interval_s)
               << "\"serialize_sbasis_contract\":" << (nc.serialize_sbasis_contract ? "true" : "false") << ','
               << "\"estimate_sat_code_bias\":" << (config_.estimateNetworkSatCodeBias() ? "true" : "false") << ','
               << "\"estimate_iono_sh\":" << (config_.estimateNetworkIonoSh() ? "true" : "false") << ','
               << "\"phase_leveling\":" << (pl_on ? "true" : "false") << ','
               << fmt::format("\"phase_leveling_q_amb\":{},", pl_q_amb)
               << "\"phase_leveling_warmup_epochs\":" << pl_warmup << ','
               << fmt::format("\"phase_leveling_max_gap_s\":{},", pl_max_gap_s)
               << fmt::format("\"phase_leveling_var_floor_m2\":{},", pl_var_floor)
               << fmt::format("\"shared_carry_var_floor_pb_m2\":{},", pb_var_floor)
               << fmt::format("\"shared_carry_var_floor_cb_m2\":{},", cb_var_floor)
               << "\"carry_zwd\":" << (zwd_on ? "true" : "false") << ','
               << fmt::format("\"zwd_carry_q\":{},", zwd_q)
               << "\"raim\":" << (raim_on ? "true" : "false") << ','
               << fmt::format("\"raim_k_sigma\":{},", raim_k)
               << "\"raim_min_stations\":" << raim_min_sta << ','
               << fmt::format("\"raim_sigma0_trigger\":{},", es_sigma0_trigger)
               << fmt::format("\"raim_withhold_sigma0\":{},", raim_withhold)
               << "\"per_const_withhold\":" << (perconst_on ? "true" : "false") << ','
               << "\"per_const_withhold_min_rows\":" << perconst_min_rows << ','
               << "\"raim_redundancy_normalize\":" << (raim_redun ? "true" : "false") << ','
               << "\"raim_attribution\":" << (config_.networkRaimAttribution() ? "true" : "false") << ','
               << "\"raim_within_epoch\":" << (config_.networkRaimWithinEpoch() ? "true" : "false") << ','
               << "\"clock_monitor\":" << (clk_mon_on ? "true" : "false") << ','
               << fmt::format("\"clock_monitor_abs_max_m\":{},", clk_mon_abs)
               << fmt::format("\"clock_monitor_jump_max_m\":{},", clk_mon_jump)
               << "\"clock_monitor_couple_raim\":" << (config_.networkClockMonitorCoupleRaim() ? "true" : "false") << ','
               << fmt::format("\"clock_monitor_reinit_sigma_m\":{},", clk_mon_reinit_sig)
               << "\"clock_model\":" << (clk_model_on ? "true" : "false") << ','
               << fmt::format("\"clock_model_q1\":{},", clk_model_q1)
               << fmt::format("\"clock_model_q2\":{},", clk_model_q2)
               << fmt::format("\"clock_model_k_sigma\":{},", clk_model_k)
               << fmt::format("\"clock_model_meas_std_m\":{},", config_.networkClockModelMeasStdM())
               << fmt::format("\"clock_model_max_gap_s\":{},", clk_model_max_gap)
               << "\"emit_ura\":" << (ura_on ? "true" : "false") << ','
               << fmt::format("\"ura_ewma_alpha\":{},", ura_alpha)
               << "\"consume_ura\":" << (config_.networkConsumeUra() ? "true" : "false") << ','
               << "\"ura_retro_bump\":" << (ura_retro ? "true" : "false") << ','
               << fmt::format("\"ura_refloat_inflate\":{},", ura_refloat_inflate)
               << fmt::format("\"ura_staleness_q2\":{},", config_.networkUraStalenessQ2())
               << "\"pivot_vanish_return_signal\":" << (pivot_vr_signal ? "true" : "false") << ','
               << "\"emit_clock_disc\":" << (emit_clock_disc ? "true" : "false") << ','
               << "\"sticky_pivot\":" << (sticky_pivot ? "true" : "false") << ','
               << "\"scoped_handover_response\":" << (scoped_handover ? "true" : "false") << ','
               << "\"emit_generation_id\":" << (emit_gen_id ? "true" : "false") << ','
               << "\"serialize_disc_counters\":" << (ser_disc_ctrs ? "true" : "false") << ','
               << "\"emit_epoch_status_record\":" << (emit_epoch_status ? "true" : "false") << ','
               << fmt::format("\"contract_rebroadcast_s\":{},", contract_rebroadcast_s)
               << fmt::format("\"emitter_staleness_horizon_s\":{},",
                              config_.networkEmitterStalenessHorizonS())
               << fmt::format("\"start_offset_hours\":{},", config_.networkStartOffsetHours())
               << fmt::format("\"process_noise_sat_clock\":{},", q_clock)
               << fmt::format("\"process_noise_sat_phase_bias\":{},", q_pb)
               << fmt::format("\"process_noise_sat_code_bias\":{}", q_cb)
               << "}}\n";
            mf.close();
        } else {
            log->warn("Network: failed to open run-manifest {}", manifest_path.string());
        }
    }
    double prev_tow = -1.0;
    int    prev_week = -1;
    int    iod = 0;
    // RTA-3: per-run solution-generation id, minted at the FIRST emit from that
    // epoch's GPS time (deterministic under replay — the id is serialized per
    // epoch; different across live restarts, which first-emit later).  0 = unset.
    uint32_t generation_id = 0;
    // RTA-5: product-time (s) of the last written S contract line (the deferred v2
    // header counts as the first); -1 until the contract writer opens.
    double last_contract_emit_s = -1.0;
    size_t n_emitted = 0, n_skipped = 0, n_gate_withheld = 0, n_runaway_epochs = 0;
    // RTB-6: operator health-record state (network.health_interval_s > 0).
    double last_health_write_s = -1.0;   // content time of the last health row (s)
    double last_emit_content_s = -1.0;   // content time of the last EMITTED epoch (s)
    int    last_n_present      = 0;      // stations present in the most recent processed epoch
    int    last_n_clock_sats   = 0;      // registered clock sats in the most recent processed epoch (P4b)
    // P4(b): ms_per_epoch_window anchors — wall ms + loop-epoch count as of the
    // PREVIOUS health row, so each row reports the mean over ITS OWN window (the
    // operationally useful "is it slowing down NOW" signal), not a cumulative
    // run mean mislabeled "rolling".
    double health_win_ms_anchor     = 0.0;
    size_t health_win_epochs_anchor = 0;
    size_t n_sigma0_withheld = 0;   // A4/R8: epochs skipped specifically by the σ̂0 withhold ceiling (subset of n_skipped)
    // Prioritized-datum drop-and-heal observability: epochs where the gate threw and the
    // near-null phase-bias soft-anchor recovered it, and total columns anchored.  A run
    // that heals every epoch "succeeds" silently otherwise (the heal-success log is debug).
    size_t n_healed_epochs = 0, n_healed_cols = 0;
    // Coverage-threshold union gate (network.phase_datum_min_stations): epochs on
    // which >=1 thin column was excluded, and the cumulative column-epoch count.
    size_t n_gated_epochs = 0, n_gated_key_epochs = 0;
    size_t n_perconst_partial_epochs = 0;     // RTA-10: epochs published PARTIALLY (σ̂0 carve-out pruned >=1 constellation)
    size_t n_perconst_diffuse_published = 0;  // RTA-10: epochs published in FULL although the global σ̂0 tripped (every constellation individually clean — the diffuse cross-constellation edge of the per-const approximation)

    // --- B8 empirical-alpha flag-rate counters (network.trace_flag_rates) -----
    // Pure observation: {opportunities, flags} per QC tier, summarized at end of run
    // as realized rate vs the implied one-sided Gaussian α for the configured k.  Never
    // fed back into any decision; every increment is guarded so OFF is byte-identical.
    const bool trace_flag_rates = nc.trace_flag_rates;
    struct FlagRate { long opp = 0, flag = 0; };
    FlagRate fr_if_slip;    // T1 per (station,sat,pair) IF-phase sample   k=if_phase_td_k_mad
    FlagRate fr_we_trigger; // T2 per warm within-epoch epoch: σ̂0_first>trig k=sigma0_trigger
    FlagRate fr_raim_cell;  // T3 per (sat,station) standardized residual   k=raim_k
    FlagRate fr_raim_sat;   // T3 consensus per spiked sat (>=min_sta)       k=raim_k
    FlagRate fr_attrib;     // T4 per median-polish SAT column: eff/σ̂0>k & σ̂0>trigger (B8/P1-4)
    FlagRate fr_clk_model;  // T5 per sat clock-model NIS (own, !ext)        k=clock_model_k
    FlagRate fr_clk_mon;    // T6 per sat clock-monitor abs/jump (own, !ext) absolute bounds
    double max_dur_anchor_s = 0.0;
    bool   max_dur_anchored = false;
    bool   spectrum_diag_done = false;  // one-shot estimability spectrum dump

    // Single source of truth for "is (sat,signal) at station i a gauge-pinned
    // ambiguity this epoch?" — the predicate that replaces the scattered is_pivot
    // gauge tests.  Legacy (network.phase_datum_prioritized OFF): membership ==
    // station is THE pivot, which pins the whole shared_phase_keys set, so this
    // returns exactly is_pivot and every rewritten site is byte-identical.
    // Prioritized: membership == this "sat_signal" key is in station i's elected
    // pinned_amb_keys (the anchor for that gauge freedom, its pin folded into its
    // OWN local block).  Key form matches the pin fold (parts->satellite + "_" +
    // parts->signal) and the ambiguity-state key.
    const bool phase_datum_prioritized = config_.networkPhaseDatumPrioritized();
    auto isGaugePinned = [&](size_t i, const std::string& sat,
                             const std::string& signal) -> bool {
        if (!phase_datum_prioritized) return stations[i].is_pivot;
        return stations[i].pinned_amb_keys.count(sat + "_" + signal) != 0;
    };

    // Column-split helper: split a station's H into the shared border A_s
    // (clock/phase-bias columns) and the local nuisance block A_r (everything
    // except the 3 fixed-position columns and the shared columns).  Returns the
    // H-column → A_r-column map (used for the pivot ambiguity pins).
    auto splitStation = [&](NetStation& st, const NetworkEstimator& est,
                            const EpochMeasurement& em,
                            Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                            Eigen::VectorXd& w, Eigen::VectorXd& y,
                            std::map<int, int>& h2local) {
        const Eigen::Index m       = em.H.rows();
        const Eigen::Index nstate  = em.H.cols();
        const Eigen::Index nshared = est.numShared();
        A_s = Eigen::MatrixXd::Zero(m, nshared);

        std::set<int> shared_cols;  // H-column indices that map to the shared border
        for (const auto& kv : st.sm->getSatClockStates()) {
            const std::string sat = StateKeys::satelliteFromSatClockKey(kv.first);
            Eigen::Index col;
            try { col = est.clockColumn(sat); } catch (...) { continue; }
            A_s.col(col) = em.H.col(static_cast<Eigen::Index>(kv.second));
            shared_cols.insert(static_cast<int>(kv.second));
        }
        for (const auto& kv : st.sm->getSatPhaseBiasStates()) {
            const auto parts = StateKeys::parseSatPhaseBiasKey(kv.first);
            if (!parts) continue;
            Eigen::Index col;
            try { col = est.phaseBiasColumn(parts->satellite, parts->signal); }
            catch (...) { continue; }
            A_s.col(col) = em.H.col(static_cast<Eigen::Index>(kv.second));
            shared_cols.insert(static_cast<int>(kv.second));
        }
        // Satellite CODE bias (W6): scatter each per-(sat,signal) sat-bias column
        // into the shared border.  In network mode getSatBiasStates() holds ONLY
        // the network-owned satellite code biases (the single-receiver fallback is
        // gated off in ensureRequiredStates); GLONASS gets none (code-only scope).
        for (const auto& kv : st.sm->getSatBiasStates()) {
            const auto parts = StateKeys::parseSatBiasKey(kv.first);
            if (!parts) continue;
            Eigen::Index col;
            try { col = est.codeBiasColumn(parts->satellite, parts->signal); }
            catch (...) { continue; }
            A_s.col(col) = em.H.col(static_cast<Eigen::Index>(kv.second));
            shared_cols.insert(static_cast<int>(kv.second));
        }

        // Receiver inter-frequency-bias (IFB) columns on NON-primary bands: when
        // estimating the satellite code bias, drop the receiver IFB on every band
        // outside the iono-free reference pair (the "sys_IFB<band>" states).  An
        // EXPLICIT non-primary receiver IFB drains the code information that
        // otherwise separates that band's satellite clock from its phase bias —
        // regressing the L5/E5b/... phase-bias product to a near-gauge (verified:
        // keeping it drives the per-constellation L5Q phase-bias common mode below
        // the null-space cutoff and withholds the whole epoch).  Dropping it folds
        // the receiver's non-primary code delay into the per-(rx,constellation)
        // slant-iono common mode the GIM weighting deliberately leaves free (the
        // L1-referenced convention); the SATELLITE code bias on that band stays
        // observable through the GIM's BETWEEN-satellite iono structure (a per-
        // satellite quantity, so the per-receiver delay averages out across the
        // network), not through an explicit receiver counterpart.  The two primary-
        // band receiver DCB ("sys_DCB") is kept, and the GLONASS per-slot IFB (a
        // different state family) is untouched.
        std::set<int> drop_cols;
        if (config_.estimateNetworkSatCodeBias()) {
            for (const auto& kv : st.sm->getCodeBiasStates()) {
                if (StateKeys::isGlonassPerSlotIfbKey(kv.first)) continue;
                if (kv.first.find("_IFB") != std::string::npos)
                    drop_cols.insert(static_cast<int>(kv.second));
            }
        }

        const int pos = static_cast<int>(st.sm->getPositionIndex());  // 0
        std::vector<int> local_cols;
        local_cols.reserve(static_cast<size_t>(nstate));
        for (int c = 0; c < static_cast<int>(nstate); ++c) {
            if (c == pos || c == pos + 1 || c == pos + 2) continue;  // coords fixed
            if (shared_cols.count(c)) continue;                      // shared border
            if (drop_cols.count(c)) continue;                        // non-primary rx IFB (dropped)
            // Drop a structurally-unobservable local state (all-zero H column this
            // epoch).  The usual case: the Galileo inter-system-bias state of a
            // station that tracks no Galileo this epoch (the StateManager allocates
            // an ISB per active non-GPS system regardless of per-station, per-epoch
            // visibility); also a stale ambiguity/iono column for a satellite the
            // cross-station observability gate just removed.  Keeping the dead
            // column makes the local normal block N_rr singular, and the kernel
            // then rejects the WHOLE station — throwing away its perfectly good GPS
            // observations (this is why GPS-only NVSK00RUS / MDVJ00RUS were dropped
            // wholesale).  Excluding ONLY the dead column lets the station
            // contribute every observable state it has; its receiver clock absorbs
            // its GPS rows, so the GPS clock common-mode gauge stays intact.
            if (em.H.col(c).cwiseAbs().maxCoeff() == 0.0) continue;
            h2local[c] = static_cast<int>(local_cols.size());
            local_cols.push_back(c);
        }
        // --- Linearity invariant (A6, insurance) --------------------------------
        // Every column retained in local_cols becomes a column of A_r, i.e. a
        // parameter the Schur kernel treats as LINEAR in the observations.  That is
        // valid ONLY because the receiver position is held fixed at its known ECEF:
        // the 3 position columns (pos..pos+2) are dropped above ("coords fixed"), so
        // A_r contains only genuinely-linear nuisances (rx clock, tropo ZWD, per-sat
        // slant iono, ambiguities, code/phase biases, ISBs).  A position column is
        // NONLINEAR in the geometry (its H entry is the line-of-sight unit vector,
        // re-linearized each epoch about a moving point) and must never enter A_r for
        // a fixed-coordinate station.  Should a future float-coordinate (non-fiducial)
        // station ever survive here, the fixed-linear-block assumption is violated and
        // the estimate would be silently wrong — hard-fail instead.  This scan never
        // fires on the current fixed-coord path (the loop above already `continue`s on
        // pos..pos+2), so it is byte-identical; the throw rides the produce-phase
        // try/catch (per-station, thread-safe) that dispositions a failed station.
        for (int lc : local_cols) {
            if (lc == pos || lc == pos + 1 || lc == pos + 2) {
                throw std::runtime_error(fmt::format(
                    "Network splitStation: position column {} of station '{}' survived "
                    "into the linear local block A_r. The network holds all station "
                    "coordinates fixed; a position column here breaks the "
                    "linear-in-observation invariant of A_r (is_pivot={}).",
                    lc, st.name, st.is_pivot));
            }
        }
        A_r.resize(m, static_cast<Eigen::Index>(local_cols.size()));
        for (size_t j = 0; j < local_cols.size(); ++j)
            A_r.col(static_cast<Eigen::Index>(j)) = em.H.col(local_cols[j]);

        w = em.R_v.cwiseInverse();   // per-obs information = 1/σ²
        y = em.innovations;
    };

    // GIM ionosphere pseudo-observation (W6 satellite code-bias prerequisite).
    // Ports Application::applyGimIonoUpdate's common-mode-free R into tight-prior
    // rows folded into a station's OWN block: one row per GPS/Galileo satellite
    // with a usable slant-iono column and a valid GIM value, with a per-
    // constellation R = diag(var) + σ_c²·11ᵀ.  The all-ones block leaves the
    // per-constellation common mode (the receiver iono-datum level) UNCONSTRAINED,
    // imposing only the GIM's between-satellite structure — exactly what separates
    // the satellite DCB from the slant iono while leaving the constellation-wide
    // DCB level for the explicit sum-to-zero datum.  The correlated R is whitened
    // per constellation (L⁻¹ from LLᵀ=R) into unit-information rows so it stays in
    // the station's diagonal-weight block.  GLONASS is handled by its own loose
    // anchors above (no satellite code bias).  Built into the station's own block —
    // no shared write, safe in the parallel produce phase (GIM query is const).
    auto appendGimIonoRows = [&](NetStation& st,
                                 const std::vector<PppObservation>& sta_obs,
                                 const std::map<int, int>& h2local, const GnssTime& t,
                                 Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                 Eigen::VectorXd& w, Eigen::VectorXd& y) {
        if (!gim_ || !gim_->isLoaded()) return;
        const auto& iw = config_.getIonosphereWeightingConfig();
        if (!iw.enabled) return;
        const Eigen::VectorXd x = st.sm->getStateVector();

        struct GRow { int col; double innov; double var; };
        std::map<char, std::vector<GRow>> by_const;   // constellation char -> rows
        for (const auto& o : sta_obs) {
            if (o.satellite.empty() || o.satellite.front() == 'R') continue;  // GPS/Galileo only
            if (o.elevation < iw.elevation_cutoff_deg) continue;
            const int iono_idx = st.sm->getIonosphereIndex(o.satellite);
            if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
            const auto hit = h2local.find(iono_idx);
            if (hit == h2local.end()) continue;   // iono not in this station's local block this epoch
            const GimQuery q =
                gim_->slantIonoDelay(t, st.ecef, o.sat_pos, Constants::GPS_L1_FREQ);
            if (!q.valid) continue;
            const double sigma = std::max(iw.floor_m, iw.std_scale * q.std_m);
            by_const[o.satellite.front()].push_back(
                GRow{hit->second, q.delay_m - x(iono_idx), sigma * sigma});
        }

        int total = 0;
        for (const auto& kv : by_const) total += static_cast<int>(kv.second.size());
        if (total == 0) return;

        const double common_var = iw.common_mode_sigma_m * iw.common_mode_sigma_m;
        const Eigen::Index m0     = A_s.rows();
        const Eigen::Index ncol_r = A_r.cols();
        A_s.conservativeResize(m0 + total, Eigen::NoChange);
        A_s.bottomRows(total).setZero();
        A_r.conservativeResize(m0 + total, Eigen::NoChange);
        A_r.bottomRows(total).setZero();
        w.conservativeResize(m0 + total);
        y.conservativeResize(m0 + total);

        Eigen::Index out = m0;
        for (auto& kv : by_const) {
            const std::vector<GRow>& rws = kv.second;
            const int mc = static_cast<int>(rws.size());
            Eigen::MatrixXd R = Eigen::MatrixXd::Constant(mc, mc, common_var);
            for (int k = 0; k < mc; ++k) R(k, k) += rws[k].var;
            Eigen::MatrixXd Hc = Eigen::MatrixXd::Zero(mc, ncol_r);
            Eigen::VectorXd zc(mc);
            for (int k = 0; k < mc; ++k) { Hc(k, rws[k].col) = 1.0; zc(k) = rws[k].innov; }
            // Whiten by L⁻¹ (LLᵀ = R) → unit-information rows that contribute exactly
            // Hᵀ R⁻¹ H / Hᵀ R⁻¹ z to the station normals.
            const Eigen::LLT<Eigen::MatrixXd> llt(R);
            A_r.middleRows(out, mc) = llt.matrixL().solve(Hc);
            y.segment(out, mc)      = llt.matrixL().solve(zc);
            w.segment(out, mc).setOnes();
            out += mc;
        }
    };

    // Co-estimated SH-VTEC ionosphere pseudo-observation (the design's hinge —
    // replaces the external-GIM target of appendGimIonoRows with the network's OWN
    // spherical-harmonic field).  Per GPS/Galileo satellite with a usable slant-iono
    // column it folds one row asserting  I_local(rx,sat) = M(el)·VTEC_SH(IPP) :
    //   H:  +1 on the local slant-iono column (A_r, exactly as the GIM row), and
    //      −M(el)·tecu_to_L1m·Ȳ_nm(IPP) on each SHARED SH coefficient column (A_s) —
    //      the ONE new write that turns the prior into a joint estimate.
    // The innovation is −x(iono_idx) (the field at coeff 0 is 0; absolute solve).
    // The common-mode-free per-constellation R = diag(var)+σ_c²·11ᵀ is REUSED
    // unchanged: its all-ones block leaves the per-(rx,const) iono datum level FREE
    // (the receiver DCB rides in it), so the field constrains only the between-
    // satellite slant-iono STRUCTURE — a₀₀ (global mean) is observable only weakly,
    // through the mapping-function spread, and is otherwise held by its loose Kaula
    // prior (data-determined mean, not clamped to an external value).  GLONASS is
    // excluded from the field (FDMA per-slot IFB would leak into the global field;
    // see design §8).  Built into the station's own block + the shared border; the
    // SH column lookups are const, safe in the parallel produce phase.
    auto appendShIonoRows = [&](NetStation& st, const NetworkEstimator& est,
                                const std::vector<PppObservation>& sta_obs,
                                const std::map<int, int>& h2local, const GnssTime& t,
                                Eigen::MatrixXd& A_s, Eigen::MatrixXd& A_r,
                                Eigen::VectorXd& w, Eigen::VectorXd& y) {
        if (!sh_model) return;
        const auto& iw = config_.getIonosphereWeightingConfig();
        if (!iw.enabled) return;
        const Eigen::VectorXd x = st.sm->getStateVector();
        const double Re = shc.earth_radius_km * 1000.0;
        const double H  = shc.layer_height_km * 1000.0;
        const double tecu_to_L1m =
            40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
        const double lon_sun = shc.sun_fixed ? meanSunLongitude(t) : 0.0;
        const int K = sh_model->numCoeffs();

        struct SRow {
            int    local_col;        ///< A_r column of this satellite's slant-iono state.
            double innov;            ///< −x(iono_idx).
            double var;              ///< per-LOS representativeness variance (m²).
            double map_tecu;         ///< M(el)·tecu_to_L1m (the STEC scale).
            std::vector<double> basis;  ///< Ȳ_nm(IPP) in coeff order.
        };
        std::map<char, std::vector<SRow>> by_const;
        std::vector<double> bvals;
        for (const auto& o : sta_obs) {
            if (o.satellite.empty() || o.satellite.front() == 'R') continue;  // GPS/Galileo only
            if (o.elevation < iw.elevation_cutoff_deg) continue;
            const int iono_idx = st.sm->getIonosphereIndex(o.satellite);
            if (iono_idx < 0 || iono_idx >= static_cast<int>(x.size())) continue;
            const auto hit = h2local.find(iono_idx);
            if (hit == h2local.end()) continue;   // iono not in this station's local block this epoch
            const IppGeometry g = computeIppGeometry(st.ecef, o.sat_pos, Re, H);
            if (!g.valid) continue;
            const double lon_sf = g.ipp_lon_rad - lon_sun;
            sh_model->basis(g.ipp_lat_rad, lon_sf, bvals);
            const double sigma =
                std::max(iw.floor_m, shc.representation_sigma_tecu * g.mapping * tecu_to_L1m);
            SRow r;
            r.local_col = hit->second;
            r.innov     = -x(iono_idx);
            r.var       = sigma * sigma;
            r.map_tecu  = g.mapping * tecu_to_L1m;
            r.basis     = bvals;
            by_const[o.satellite.front()].push_back(std::move(r));
        }

        int total = 0;
        for (const auto& kv : by_const) total += static_cast<int>(kv.second.size());
        if (total == 0) return;

        // SH column indices (shared border), looked up once per call.
        std::vector<Eigen::Index> sh_cols(static_cast<std::size_t>(K));
        for (int k = 0; k < K; ++k) {
            const auto& c = sh_model->coeffs()[static_cast<std::size_t>(k)];
            sh_cols[static_cast<std::size_t>(k)] = est.vtecCoeffColumn(c.n, c.m, c.trig);
        }

        const double common_var = iw.common_mode_sigma_m * iw.common_mode_sigma_m;
        const Eigen::Index m0      = A_s.rows();
        const Eigen::Index ncol_r  = A_r.cols();
        const Eigen::Index nshared = A_s.cols();
        A_s.conservativeResize(m0 + total, Eigen::NoChange);
        A_s.bottomRows(total).setZero();
        A_r.conservativeResize(m0 + total, Eigen::NoChange);
        A_r.bottomRows(total).setZero();
        w.conservativeResize(m0 + total);
        y.conservativeResize(m0 + total);

        Eigen::Index out = m0;
        for (auto& kv : by_const) {
            const std::vector<SRow>& rws = kv.second;
            const int mc = static_cast<int>(rws.size());
            Eigen::MatrixXd R = Eigen::MatrixXd::Constant(mc, mc, common_var);
            for (int k = 0; k < mc; ++k) R(k, k) += rws[k].var;
            Eigen::MatrixXd Hr = Eigen::MatrixXd::Zero(mc, ncol_r);    // local +1 (slant iono)
            Eigen::MatrixXd Hs = Eigen::MatrixXd::Zero(mc, nshared);   // −M·tecu·basis (SH cols)
            Eigen::VectorXd zc(mc);
            for (int k = 0; k < mc; ++k) {
                Hr(k, rws[k].local_col) = 1.0;
                for (int j = 0; j < K; ++j)
                    Hs(k, sh_cols[static_cast<std::size_t>(j)]) =
                        -rws[k].map_tecu * rws[k].basis[static_cast<std::size_t>(j)];
                zc(k) = rws[k].innov;
            }
            // Whiten by L⁻¹ (LLᵀ = R) → unit-information rows; whiten the local +1,
            // the SH partials, AND the innovation by the SAME factor so the common-
            // mode-free structure carries through to both blocks identically.
            const Eigen::LLT<Eigen::MatrixXd> llt(R);
            A_r.middleRows(out, mc) = llt.matrixL().solve(Hr);
            A_s.middleRows(out, mc) = llt.matrixL().solve(Hs);
            y.segment(out, mc)      = llt.matrixL().solve(zc);
            w.segment(out, mc).setOnes();
            out += mc;
        }
    };

    // Pin Eigen to a single thread for the duration of the (parallelized) epoch
    // loop.  _OPENMP is defined project-wide so Eigen threads its own dense
    // kernels; nesting those inside our per-station OpenMP region would cause
    // thread oversubscription AND non-deterministic reduction order in the
    // per-station matmuls.  Single-threaded Eigen inside each station thread is
    // fully thread-safe (separate matrices) and deterministic; restored after the
    // loop.  Only touched when parallelism is active, so the serial path (and the
    // small-network case) is byte-identical to before.
    const int eigen_threads_saved = Eigen::nbThreads();
    if (parallel_on) Eigen::setNbThreads(1);

    // [TIMING] Wall-clock of just the per-epoch loop (excludes ~startup: product
    // cache load + per-station RINEX parse).  Isolates the per-epoch compute that
    // scales with station count — the scaling metric.  Counts only epochs that
    // actually entered processing (past the max-duration cutoff).
    const auto t_loop_begin = std::chrono::steady_clock::now();
    size_t n_loop_epochs = 0;

    // The joint estimator is HOISTED out of the epoch loop and reset()/reused each
    // epoch.  Constructing a fresh NetworkEstimator per epoch reallocated one dense
    // Schur block PER STATION every epoch; the process heap retained that churn as
    // committed memory, saturating at ~0.5 GB/station (≈146 GB at 305 stations over
    // a 121-epoch run → exhausted RAM).  Reusing the estimator's station-block pool
    // (NetworkNormalEquations::reset) holds the footprint to one epoch's working set.
    NetworkEstimator est;
    // OPT-IN fan-out of the Schur reduction.  Kept SEPARATE from the build-phase
    // parallelism: the build's serial-ordered scatter is byte-identical to a serial
    // run, but the reduce fan-out reassociates the per-station summation, so it is
    // reproducible per thread count yet ULP-different from the serial reduce (the
    // network's ill-conditioning amplifies that to ~1e-5 in the products — physically
    // null, but it breaks serial≡parallel byte-identity).  Default OFF: the reduce
    // stays serial (byte-identical serial≡parallel) while the build still threads and
    // the deterministic serial-tail wins (eigensolver gate, no double-reduce, factor
    // reuse) still apply.  Enable network.parallel_reduce for max-speed runs.
    const int reduce_threads = (parallel_on && nc.parallel_reduce) ? n_threads : 1;
    est.setReduceThreads(reduce_threads);
    // Pin-aware station PD guard (network.station_pd_pin_aware): median-|pivot|
    // reference scale for the local-block rank test — the prioritized datum's
    // in-block 1e10 anchor pins otherwise eject marginal stations every epoch.
    est.setStationPdMedianScale(config_.networkStationPdPinAware());
    if (parallel_on && nc.parallel_reduce)
        log->info("Network: Schur reduce PARALLEL across {} threads "
                  "(serial≡parallel byte-identity intentionally relaxed to ~1e-5)",
                  n_threads);

    // === RTB-7(a): state snapshot + warm resume ==============================
    // Entirely OUTSIDE the product-file replay path: the snapshot restores the
    // producer's in-memory cross-epoch state; NetworkProductIO replay still
    // reproduces every discontinuity counter with no snapshot involved.  Path
    // empty (default) ⇒ nothing below runs — byte-identical.
    std::filesystem::path snap_path;
    if (!nc.state_snapshot_path.empty()) {
        snap_path = nc.state_snapshot_path;
        if (snap_path.is_relative()) snap_path = log_dir_ / snap_path;
    }
    double   last_snapshot_write_s = -1.0;   // product time of the last snapshot write
    uint64_t live_fingerprint      = 0;
    if (!snap_path.empty()) {
        // Config fingerprint over the DATUM-relevant fields: resuming under a
        // changed S-basis/pivot pin, station roster (order defines the carry's
        // station indices), carry q's / gap bounds / floors, clock-model
        // parameters, or emitter channel config must COLD-start — the carried
        // state would be interpreted in the wrong frame or under the wrong rules.
        std::string fp;
        auto add = [&fp](const std::string& v) { fp += v; fp += '\x1f'; };
        for (const auto& st : stations) { add(st.name); add(st.is_pivot ? "P" : "-"); }
        add(nc.clock_datum_station); add(nc.clock_datum_satellite);
        // The phase-bias gauge realization (single-pivot vs prioritized anchor)
        // defines which station's ambiguities carry the datum; a resume across a
        // toggle change would interpret the carried anchors in the wrong frame.
        add(fmt::format("{}", phase_datum_prioritized));
        add(fmt::format("{}", pl_on));           add(fmt::format("{}", pl_q_amb));
        add(fmt::format("{}", pl_warmup));       add(fmt::format("{}", pl_max_gap_s));
        add(fmt::format("{}", pl_var_floor));    add(fmt::format("{}", pb_var_floor));
        add(fmt::format("{}", cb_var_floor));    add(fmt::format("{}", zwd_on));
        add(fmt::format("{}", zwd_q));           add(fmt::format("{}", clk_model_on));
        add(fmt::format("{}", clk_model_q1));    add(fmt::format("{}", clk_model_q2));
        add(fmt::format("{}", clk_model_k));     add(fmt::format("{}", clk_model_max_gap));
        add(fmt::format("{}", ura_on));          add(fmt::format("{}", ura_alpha));
        add(fmt::format("{}", ura_retro));       add(fmt::format("{}", ura_refloat_inflate));
        add(fmt::format("{}", emit_clock_disc));
        add(fmt::format("{}", config_.networkEmitterStalenessHorizonS()));
        add(fmt::format("{}", sticky_pivot));    add(fmt::format("{}", scoped_handover));
        add(fmt::format("{}", emit_gen_id));
        add(fmt::format("{}", config_.estimateNetworkSatCodeBias()));
        add(fmt::format("{}", config_.estimateNetworkIonoSh()));
        add(fmt::format("{}", q_clock)); add(fmt::format("{}", q_pb)); add(fmt::format("{}", q_cb));
        add(fmt::format("{}", nc.warmup_ura_floor_m)); add(fmt::format("{}", nc.warmup_epochs));
        // Card 6: apriori_source + anchor σ are datum-relevant.  The carried clock
        // frame is Δδtˢ-vs-broadcast in brdc_rts but Δδtˢ-vs-precise in sp3 (differ by
        // ~metres), and only brdc_rts carries the relevel_iode baseline — so a warm
        // resume must NOT cross an apriori_source change or it would fold a clock carry
        // in the wrong frame and re-level against a stale baseline.
        add(fmt::format("{}", static_cast<int>(config_.networkAprioriSource())));
        add(fmt::format("{}", config_.networkAprioriClockAnchorSigmaM()));
        live_fingerprint = NetworkStateSnapshotIO::fnv1a64(fp);
    }
    if (!snap_path.empty() && std::filesystem::exists(snap_path)) {
        NetworkStateSnapshot snap;
        std::string err;
        if (!NetworkStateSnapshotIO::read(snap_path, snap, &err)) {
            log->warn("Network (RTB-7): state snapshot '{}' failed to parse ({}) — COLD START "
                      "(a new generation id is minted at first emit as usual).",
                      snap_path.string(), err);
        } else {
            // First epoch this run will actually process (honouring the
            // start_offset_hours fast-forward) — the age reference of the gate.
            double first_live_s = -1.0;
            if (!synced.empty()) {
                const double anchor_s = TimeUtils::continuousGpsSeconds(synced.front().time);
                for (const auto& sf : synced) {
                    const double t_s = TimeUtils::continuousGpsSeconds(sf.time);
                    if (start_offset_s <= 0.0 || (t_s - anchor_s) >= start_offset_s) {
                        first_live_s = t_s;
                        break;
                    }
                }
            }
            // Carry-validity horizon = max of the per-key gap bounds.  Past it
            // every restored key would be dropped/reseeded downstream anyway;
            // WITHIN it, per-key staleness after restore is handled by the
            // EXISTING machinery (local-carry max-gap drop, D2 q·dt inflation
            // over the true gap via the restored prev_week/prev_tow, clock-model
            // per-sat t_last gap logic) — restore everything, let the
            // established per-key logic judge each key.
            const double horizon_s = std::max(pl_max_gap_s, clk_model_max_gap);
            std::string gate;
            if (first_live_s < 0.0) {
                log->warn("Network (RTB-7): state snapshot '{}' found but there are no live "
                          "epochs to resume against — COLD START.", snap_path.string());
            } else if (!NetworkStateSnapshotIO::warmResumeAllowed(
                           snap, live_fingerprint, first_live_s, horizon_s, &gate)) {
                log->warn("Network (RTB-7): state snapshot '{}' NOT warm-resumable — {} — "
                          "COLD START (a new generation id is minted at first emit as usual).",
                          snap_path.string(), gate);
            } else {
                // Emitter layer FIRST (all-or-nothing): a failed emitter restore
                // discards the whole warm resume — restoring the driver carries
                // without the emitter's baselines/pending flags would emit WRONG
                // discontinuity counters (the silent consumer datum break).
                std::istringstream es(snap.emitter_state);
                if (!emitter.restoreState(es)) {
                    log->warn("Network (RTB-7): state snapshot '{}' passed the gate but its "
                              "EMITTER section failed to restore (corrupt or parameter "
                              "mismatch) — COLD START; driver state deliberately NOT "
                              "restored (the two layers only make sense together).",
                              snap_path.string());
                } else {
                    // --- Driver layer -----------------------------------------
                    prev_carry.clear();
                    prev_carry.reserve(snap.shared_carry.size());
                    for (const auto& sc : snap.shared_carry) {
                        CarryPrior cp;
                        cp.kind      = static_cast<SharedParamKind>(sc.kind);
                        cp.sat       = sc.sat;
                        cp.signal    = sc.signal;
                        cp.sh_n      = sc.sh_n;
                        cp.sh_m      = sc.sh_m;
                        cp.sh_trig   = static_cast<char>(sc.sh_trig);
                        cp.value     = sc.value;
                        cp.var       = sc.var;
                        cp.inflate_q = sc.inflate_q;
                        prev_carry.push_back(std::move(cp));
                    }
                    // C1: the v1 snapshot serializes per-key (value, var) only — the
                    // dense covariance block deliberately is not persisted.  The first
                    // post-resume fold therefore runs the diagonal path (identical
                    // legacy semantics); the dense block re-seeds at the first emit.
                    if (fullcov_on && !prev_carry.empty())
                        log->info("Network (C1): warm resume restored {} diagonal carries — "
                                  "the full-covariance carry re-seeds at the first emitted "
                                  "epoch.", prev_carry.size());
                    prev_pivot    = snap.prev_pivot;
                    last_pivot    = snap.last_pivot;
                    c1_zoh_active = snap.c1_zoh_active;
                    prev_clk      = snap.prev_clk;
                    clock_osc.clear();
                    for (const auto& [sat, o] : snap.clock_osc) {
                        ClockOsc c;
                        c.x0 = o.x0; c.x1 = o.x1;
                        c.p00 = o.p00; c.p01 = o.p01; c.p11 = o.p11;
                        c.t_last = o.t_last; c.init = o.init;
                        clock_osc[sat] = c;
                    }
                    local_carry.clear();
                    for (const auto& [key, l] : snap.local_carry)
                        local_carry[key] = LocalCarry{l.value_abs, l.var, l.last_seen_s};
                    zwd_carry.clear();
                    for (const auto& [idx, l] : snap.zwd_carry)
                        zwd_carry[idx] = LocalCarry{l.value_abs, l.var, l.last_seen_s};
                    ura_sigma0_ewma     = snap.ura_sigma0_ewma;
                    ura_refloat_horizon = snap.ura_refloat_horizon;
                    ura_retro_sats      = snap.ura_retro_sats;
                    retro_active        = snap.retro_active;
                    // Card 6: restore the eph-cutover re-level baseline + any pending
                    // forced disc so a warm resume does not re-level against a stale
                    // baseline (or lose a pending forced bump).  Empty in an sp3 or
                    // pre-Card-6 snapshot ⇒ the first brdc_rts epoch re-seeds cleanly.
                    relevel_iode             = snap.relevel_iode;
                    eph_cutover_disc_pending = snap.eph_cutover_disc_pending;
                    // RTA-3 seam (the critical one): PRESERVE the snapshot's
                    // generation id.  The first-emit minting site only mints when
                    // generation_id == 0, so a restored non-zero id SUPPRESSES the
                    // mint — consumers keep the same product datum and never
                    // hard-reset.  (Same-config guarantee: emit_gen_id is in the
                    // fingerprint, so an id-less snapshot only warm-resumes an
                    // id-less run, where 0 is correct.)
                    generation_id = snap.generation_id;
                    iod           = snap.iod;
                    prev_week     = snap.prev_week;
                    prev_tow      = snap.prev_tow;
                    n_emitted           = static_cast<size_t>(snap.n_emitted);
                    n_skipped           = static_cast<size_t>(snap.n_skipped);
                    n_gate_withheld     = static_cast<size_t>(snap.n_gate_withheld);
                    n_sigma0_withheld   = static_cast<size_t>(snap.n_sigma0_withheld);
                    n_runaway_epochs    = static_cast<size_t>(snap.n_runaway_epochs);
                    n_perconst_partial_epochs    = static_cast<size_t>(snap.n_perconst_partial);
                    n_perconst_diffuse_published = static_cast<size_t>(snap.n_perconst_diffuse);
                    last_emit_content_s = snap.last_emit_content_s;
                    last_health_write_s = snap.last_health_write_s;
                    // RTB-7(b): ramp state restores — a warm-resumed producer is
                    // NOT degraded.  Already-lifted ⇒ grade 0 ("warm"); mid-ramp
                    // ⇒ the ramp CONTINUES from the restored counts (never
                    // restarts), with the grade anchored at the original first
                    // emit (honest total product time to grade).
                    ura_ewma_updates     = static_cast<size_t>(snap.ura_ewma_updates);
                    ramp_lifted          = snap.ramp_lifted;
                    first_emit_content_s = snap.first_emit_content_s;
                    if (nc.warmup_ura_floor_m > 0.0 && ura_on && ramp_lifted) {
                        grade_metric_s = 0.0;
                        log->info("Network (RTB-7): cold-start-to-product-grade: 0 s (warm "
                                  "resume — ramp already lifted before the bounce).");
                    }
                    const double snap_s =
                        static_cast<double>(snap.snap_week) * 604800.0 + snap.snap_tow;
                    log->info("Network (RTB-7): WARM RESUME from '{}' (written {}) — snapshot "
                              "age {:.1f} s vs first live epoch (horizon {:.1f} s); generation "
                              "id {} PRESERVED (first-emit minting suppressed).  Restored: {} "
                              "D2 shared-carry, {} ambiguity-carry, {} ZWD-carry, {} clock-"
                              "oscillator, {} clock-monitor baseline, {}/{} pivot baseline "
                              "(prev/last-ever), {} pending retro-bump, {} refloat-horizon "
                              "entries; emitter stream state (counters, baselines, first-"
                              "sighting sets, pending bumps, last-seen stamps) restored; "
                              "n_emitted {} (carry stays warm).",
                              snap_path.string(), snap.wall_utc, first_live_s - snap_s,
                              horizon_s, generation_id, prev_carry.size(), local_carry.size(),
                              zwd_carry.size(), clock_osc.size(), prev_clk.size(),
                              prev_pivot.size(), last_pivot.size(), ura_retro_sats.size(),
                              ura_refloat_horizon.size(), n_emitted);
                }
            }
        }
    }

    // Card 5 (RTF-1 / plan D6): resolve the a-priori satellite-clock anchor σ ONCE
    // (loop-invariant, config-only) so the anchor fold and its B7 information-fraction
    // diagnostic below share one value.  The resolution policy (explicit > 0 wins;
    // brdc_rts auto → 3.0 m; sp3 auto → initial_std.sat_clock, unchanged) lives in
    // GnssConfig so it is a single source of truth and unit-testable in isolation.
    // The < 100 m binding gate at the fold site is unchanged and stays in the driver.
    const double clk_anchor_sigma = config_.resolveNetworkAprioriClockAnchorSigmaM();

    for (const auto& se : synced) {
        // Optional testing fast-forward (network.start_offset_hours): skip epochs until the
        // elapsed data span from the first synced epoch reaches the offset, then cold-start the
        // solve there.  Skipped epochs are not counted, modeled, solved, or stashed, so the
        // carry begins empty at the offset — reproduces an externally-driven late-hour event
        // (e.g. the ~4.3 h lock-in) after a fresh warm-up, not the exact accumulated history.
        if (start_offset_s > 0.0) {
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            if (start_anchor_s < 0.0) start_anchor_s = now_s;
            if (now_s - start_anchor_s < start_offset_s) continue;
        }
        LogManager::setCurrentEpochTime(se.time);

        // Card 8 (replay parity): feed the staged broadcast nav records whose
        // availability (toc, GPS scale) this epoch has reached.  The store's
        // current/previous retention then evolves exactly as under live
        // arrival — IODE rolls happen at their true times (Card 6 re-levels
        // fire), and the fit-window guard sees fresh records instead of the
        // end-of-day pair a bulk load would leave.  The first processed epoch
        // catches up over any start_offset span in one sorted batch, which
        // ends in the identical store state as epoch-by-epoch feeding.
        if (brdc_rts) {
            auto& eph_store = LiveEphemerisStore::getInstance();
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            while (brdc_nav_cursor < brdc_nav_staged.size()
                   && navAvailabilityGpsSeconds(brdc_nav_staged[brdc_nav_cursor]) <= now_s) {
                eph_store.insert(brdc_nav_staged[brdc_nav_cursor], /*from_igs=*/true);
                ++brdc_nav_cursor;
            }
        }

        // Optional max-duration cutoff (processing.max_duration_s): anchor on the
        // first synced epoch, stop once the elapsed data span reaches the limit.
        if (config_.getMaxDuration() > 0.0) {
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            if (!max_dur_anchored) { max_dur_anchor_s = now_s; max_dur_anchored = true; }
            else if (now_s - max_dur_anchor_s >= config_.getMaxDuration()) {
                log->info("Network: reached max processing duration ({:.1f} s of data); stopping",
                          config_.getMaxDuration());
                break;
            }
        }
        ++n_loop_epochs;  // [TIMING]

        // P1: size-based rotation of the unbounded service CSVs
        // (network.trace_rotate_mb > 0).  Checked once per processed epoch,
        // BEFORE this epoch's rows land, so a file overshoots by at most one
        // epoch of rows.  Each roll shifts file.csv.k → file.csv.(k+1) (oldest
        // deleted), retires the live file to file.csv.1, and reopens fresh with
        // the header re-written — every rolled generation parses standalone.
        // OFF (default 0) ⇒ maybeRotateCsv touches nothing (byte-identical).
        if (nc.trace_rotate_mb > 0.0) {
            const struct {
                std::ofstream*               os;
                const std::filesystem::path* path;
                const char*                  header;
            } rotating[] = {
                {&trace,        &trace_path,        kTraceHeader},
                {&soln,         &soln_path,         kSolnHeader},
                {&health_trace, &health_trace_path, kHealthHeader},
            };
            for (const auto& r : rotating)
                if (maybeRotateCsv(*r.os, *r.path, r.header,
                                   nc.trace_rotate_mb, nc.trace_rotate_keep))
                    log->info("Network (P1): rotated {} (live file exceeded {:.1f} MiB; "
                              "keeping {} rolled generation(s)).",
                              r.path->filename().string(), nc.trace_rotate_mb,
                              nc.trace_rotate_keep);
        }

        // RTB-6: operator health heartbeat every network.health_interval_s of content
        // time.  Reports cumulative state THROUGH the previous epoch (this epoch's
        // per-present/emit outcomes are computed below), which is exactly the "is the
        // service alive, emitting, and how contaminated/fast" signal a supervisor
        // polls.  Flushed per write so a live tail sees it immediately.  Guarded ⇒
        // OFF (default) writes nothing (byte-identical).
        if (health_trace.is_open()) {
            const double now_s = TimeUtils::continuousGpsSeconds(se.time);
            if (last_health_write_s < 0.0
                || (now_s - last_health_write_s) >= nc.health_interval_s) {
                const double loop_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_loop_begin).count();
                // P4(b): mean wall ms per loop epoch over the window since the
                // PREVIOUS health row (first row: since loop start) — a windowed
                // "is it slowing down NOW" signal, not a cumulative run mean.
                const size_t win_epochs = n_loop_epochs - health_win_epochs_anchor;
                const double ms_per_epoch_window =
                    win_epochs > 0
                        ? (loop_ms - health_win_ms_anchor) / static_cast<double>(win_epochs)
                        : 0.0;
                const double emit_age =
                    last_emit_content_s >= 0.0 ? (now_s - last_emit_content_s) : -1.0;
                // P4(a): σ̂0-EWMA availability — the EWMA updates only inside the
                // warm post-fit RAIM block (emit_ura && raim && phase_leveling);
                // with any of them unarmed the column emits the −1 "unavailable"
                // sentinel, never the frozen 1.0 init.  (During the phase-leveling
                // warmup it still reads 1.0 — updates begin once carry_warm.)
                const bool ewma_avail = ura_on && raim_on && pl_on;
                health_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_emitted << ',' << n_skipped << ',' << n_gate_withheld
                             << ',' << n_sigma0_withheld << ',' << n_runaway_epochs << ','
                             << (ewma_avail ? fmt::format("{:.4f}", ura_sigma0_ewma)
                                            : std::string("-1"))
                             << ',' << last_n_present << ',' << last_n_clock_sats
                             << ',' << fmt::format("{:.3f}", ms_per_epoch_window)
                             << ',' << fmt::format("{:.3f}", emit_age)
                             // P2: realized per-constellation pivots of the last EMITTED
                             // product (the emit-baseline last_pivot map, merge-updated
                             // only at the emit site) — empty before the first emit.
                             << ',' << formatPivotMap(last_pivot)
                             // RTB-7(c): cold-start-to-product-grade (s of product time
                             // from first emit to the quality-ramp lift).  −1 before the
                             // lift or when the ramp is off; 0 = warm resume.
                             << ',' << (grade_metric_s >= 0.0
                                            ? fmt::format("{:.1f}", grade_metric_s)
                                            : std::string("-1"))
                             << '\n';
                health_trace.flush();   // a supervisor tails this live
                last_health_write_s      = now_s;
                health_win_ms_anchor     = loop_ms;
                health_win_epochs_anchor = n_loop_epochs;
            }
        }

        // L1: write one network_epoch_status.csv row for this epoch (guarded; no-op when OFF).
        auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
            if (!epoch_status_trace.is_open()) return;
            auto& o = epoch_status_trace;
            o << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
              << disposition << ',';
            if (r.sigma0)          o << fmt::format("{:.4f}", *r.sigma0);
            o << ',';
            if (r.n_adapt)         o << *r.n_adapt;
            o << ',';
            if (r.n_present)       o << *r.n_present;
            o << ',';
            if (r.n_clock_sats)    o << *r.n_clock_sats;
            o << ',';
            if (r.clock_median_m)  o << fmt::format("{:.4f}", *r.clock_median_m);
            o << ',';
            if (r.clock_std_m)     o << fmt::format("{:.4f}", *r.clock_std_m);
            o << ',';
            o << r.worst_sat << ',';
            if (r.worst_dev_m)     o << fmt::format("{:.4f}", *r.worst_dev_m);
            o << ',';
            if (r.n_refloat_sats)  o << *r.n_refloat_sats;
            o << ',';
            if (r.n_retro_applied) o << *r.n_retro_applied;
            o << ',';
            if (r.datum_changed)   o << *r.datum_changed;
            o << ',';
            o << fmt::format("{:.4f}", es_sigma0_trigger) << ','
              << fmt::format("{:.4f}", es_withhold_ceiling) << '\n';
        };

        // L2: write one network_events.csv row for a discrete QC action (guarded; no-op OFF).
        auto emitEvent = [&](const char* tier, const char* event,
                             const std::string& sat, const std::string& station,
                             std::optional<double> statistic, std::optional<double> threshold,
                             const char* action, const std::string& detail) {
            if (!events_trace.is_open()) return;
            std::string d = detail;
            for (char& c : d) if (c == ',') c = ';';   // sanitize: keep the CSV single-field
            auto& o = events_trace;
            o << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
              << tier << ',' << event << ',' << sat << ',' << station << ',';
            if (statistic) o << fmt::format("{:.6f}", *statistic);
            o << ',';
            if (threshold) o << fmt::format("{:.6f}", *threshold);
            o << ',' << action << ',' << d << '\n';
        };

        const double dt = (prev_tow < 0.0) ? 0.0
            : ((se.time.week - prev_week) * 604800.0 + (se.time.tow - prev_tow));

        // Phase-leveling carry timing (Increment 1): continuous-GPS seconds of this
        // epoch (for the per-ambiguity inter-epoch gap) and whether the cold-start
        // warmup has elapsed.  Count EMITTED product epochs (not raw loop epochs) so
        // the carry waits for the clock/SH convergence horizon — locking an
        // unconverged ambiguity onto a contaminated cold-start value would propagate
        // it (design §3.7).  n_emitted here excludes the current epoch (not yet emitted).
        const double pl_now_s   = TimeUtils::continuousGpsSeconds(se.time);
        const bool   carry_warm = pl_on && (static_cast<int>(n_emitted) >= pl_warmup);

        // --- PASS A: per-station geometry + shared-state layout -------------
        std::vector<std::vector<PppObservation>> modeled(stations.size());
        std::vector<bool> present(stations.size(), false);

        // Legacy single-pivot datum: model the pivot FIRST so its usable phase
        // signals restrict the shared phase-bias columns to ones with a pivot-
        // receiver ambiguity pin, and an absent pivot sinks the epoch (no datum).
        // Prioritized mode treats the pivot as an ordinary station — modeled in the
        // tail loop from i_lo=0, no special skip — and elects the per-(sat,signal)
        // anchors AFTER the cross-station prune, so this block is skipped and the
        // pivot blinking out no longer costs the whole epoch (the SPOF cure).
        const int i_lo = phase_datum_prioritized ? 0 : 1;
        if (!phase_datum_prioritized) {
            NetStation& piv = stations.front();
            auto it = se.by_station.find(piv.name);
            if (it == se.by_station.end() || it->second.empty()) {
                // No datum station this epoch: no clean phase-bias gauge — skip.
                ++n_skipped;
                if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                                 << ",0,0,0,0,0,0,0,0,0\n";
                emitEpochStatus("sync_skip", {});   // only week/tow available
                continue;
            }
            piv.pstate.epoch_time = se.time;
            if (!piv.model->computeModels(it->second, piv.pstate, modeled.front())
                || modeled.front().empty()) {
                ++n_skipped;
                if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                                 << ",0,0,0,0,0,0,0,0,0\n";
                emitEpochStatus("sync_skip", {});   // only week/tow available
                continue;
            }
            piv.mm->ensureNetworkSharedStates(modeled.front());  // allow all pivot phase signals
            present.front() = true;
        }

        // Legacy: the pivot's allocated phase-bias keys ARE the shared set.
        // Prioritized: seeded empty here; the anchor election after the cross-
        // station observability prune rebuilds it as the anchored UNION over
        // present stations (each key backed by exactly one elected pin).
        std::set<std::string> shared_phase_keys;
        if (!phase_datum_prioritized)
            for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
                shared_phase_keys.insert(kv.first);

        // PASS A tail (stations[1..]) — PARALLEL over stations when enabled.
        // computeModels + ensureNetworkSharedStates touch ONLY station i's own
        // sm/model/mm/pstate; shared_phase_keys is read-only here and modeled[i]
        // is a distinct slot per i — no shared write.  Each iteration catches its
        // own exceptions: an exception must never escape an OpenMP structured
        // block (UB).  present[] is std::vector<bool> (packed bits → concurrent
        // writes to adjacent indices race), so threads record into a per-index
        // char buffer that is folded into present[] serially afterwards.
        {
            std::vector<char>        a_ok(stations.size(), 0);
            std::vector<std::string> a_err(stations.size());
            // Phase-leveling T0 (Increment 2): per-station integer slip repairs,
            // produced in the parallel loop into a private slot, applied to the
            // shared local_carry in a SERIAL station-ordered drain below.
            std::vector<std::vector<TurboEditResolver::SignalRepair>>
                sta_repairs(stations.size());
            // IF-phase slip detector output: per-station flagged IF-pair reset keys
            // ("G01_L1C_L2W"), produced station-local in the parallel loop, consumed
            // in a serial station-ordered marking pass below (deterministic).
            std::vector<std::vector<std::string>> sta_if_resets(stations.size());
            // MW DETECTOR (mw_detect) output: confident wide-lane integer slips to REFLOAT,
            // as (satellite, sig1, sig2) tuples produced station-local in the parallel loop
            // and consumed in the serial MW-detect drain below.  Empty when turbo_edit runs
            // the integer-repair path instead, or when mw_detect is off.
            std::vector<std::vector<std::tuple<std::string, std::string, std::string>>>
                sta_mw_refloat(stations.size());

            #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(parallel_on)
            for (int i = i_lo; i < n_sta_total; ++i) {
                try {
                    NetStation& st = stations[i];
                    auto it = se.by_station.find(st.name);
                    if (it == se.by_station.end() || it->second.empty()) continue;
                    st.pstate.epoch_time = se.time;
                    if (!st.model->computeModels(it->second, st.pstate, modeled[i])
                        || modeled[i].empty())
                        continue;
                    // Prioritized: every station (incl. the former pivot, i=0)
                    // allocates ALL its own trackable phase signals — the anchored
                    // shared set is the post-election union, not a pivot restriction.
                    if (phase_datum_prioritized)
                        st.mm->ensureNetworkSharedStates(modeled[i]);
                    else
                        st.mm->ensureNetworkSharedStates(modeled[i], shared_phase_keys);
                    a_ok[i] = 1;

                    // T0 slip detection: roll this station's MW + GF arcs forward and
                    // collect per-(sat,signal) integer repairs.  Station-local detector
                    // state — no shared write, safe in the parallel region.  Run EVERY
                    // epoch (the resolver MUST advance its GF arcs even with no slips)
                    // and regardless of the carry warmup, so the adaptive noise floors
                    // are warm when the carry activates.  A detection failure costs only
                    // this station's repairs this epoch, never the station itself.
                    if (st.mw_slip) {
                        try {
                            const auto mwr = st.mw_slip->detectLargeMwResets(modeled[i]);
                            if (st.turbo_edit) {
                                // turbo_edit (1 s data): MW+GF → integer REPAIR of the carry.
                                auto er = st.turbo_edit->resolve(
                                    mwr, st.mw_slip->getEpochCleanPairs(), modeled[i], dt,
                                    st.mw_slip->getAcrossGapWlEvidence(pl_now_s));
                                sta_repairs[i] = std::move(er.repairs);
                            } else {
                                // mw_detect (any rate): detect-only → REFLOAT confident
                                // wide-lane integer jumps (no GF, no integer applied).  frac
                                // gates rounding cleanliness — a BIA-less float re-init rounds
                                // to a large spurious integer but with a large frac (LEFT); a
                                // KNOWN non-zero integer with small frac is a real WL slip.
                                // LLI loss-of-lock is handled directly in the serial drain.
                                const double frac_max = config_.networkCycleSlipTurboEditFracMax();
                                for (const auto& r : mwr) {
                                    if (r.slip_wl_known && r.slip_wl_cycles != 0
                                        && r.slip_frac <= frac_max
                                        && !r.sig1.empty() && !r.sig2.empty())
                                        sta_mw_refloat[i].emplace_back(r.satellite, r.sig1, r.sig2);
                                }
                            }
                        } catch (...) { /* best-effort: no MW action this epoch */ }
                    }

                    // IF-phase triple-difference slip detection: between-satellite,
                    // between-epoch IF-phase OMC.  Station positions are fixed at
                    // near-truth, so position_sigma_3d = 0 disables the pre-convergence
                    // gate (the sole reason this detector was staged) and st.ecef makes
                    // the inter-epoch geometry correction a no-op (Δpos ≡ 0).  Detector
                    // state is station-local — no shared write, safe here.  Best-effort:
                    // a failure costs only this station's flags this epoch.
                    if (st.if_slip) {
                        try {
                            sta_if_resets[i] = st.if_slip->detect(modeled[i], 0.0, st.ecef);
                        } catch (...) { /* best-effort: no IF-phase flags this epoch */ }
                    }
                } catch (const std::exception& ex) {
                    a_err[i] = ex.what();
                } catch (...) {
                    a_err[i] = "unknown exception";
                }
            }

            // Serial drain: commit present[] + log diagnostics in station order.
            // i_lo folds in station 0 too under the prioritized datum (it is an
            // ordinary station there; on the legacy path i_lo==1 and station 0's
            // present was already set by the pivot-first block above).
            for (int i = i_lo; i < n_sta_total; ++i) {
                if (a_ok[i]) present[i] = true;
                else if (!a_err[i].empty())
                    log->warn("Network: station '{}' PASS A geometry threw ({}); "
                              "skipping it this epoch", stations[i].name, a_err[i]);
            }

            // Phase-leveling T0 (serial, station-ordered → deterministic), applied
            // BEFORE PASS-B reads the carry to build the prior.
            //   • REPAIR  — a confident, sane-magnitude integer (frac ≤ frac_max AND
            //     |dN| ≤ dn_max): shift the carried value continuously
            //     (value_abs += dN·λ, variance unchanged, covariance-preserving).
            //   • LEAVE   — a less-confident emitted repair (frac/magnitude reject):
            //     do nothing.  This is the data-driven choice for the BIA-less network:
            //     the float wide-lane's emitted-but-low-confidence repairs are
            //     predominantly FALSE positives (e.g. a re-initialising arc reporting
            //     dN=675 at frac≈0.27), so acting on them — REFLOAT or DROP — discards
            //     a GOOD carry and measurably degrades the products, whereas leaving
            //     them matches carry-only.  The residual lock-in risk on a RARE
            //     real-but-rejected slip is bounded by phase_leveling_var_floor_m2 and
            //     the max-gap drop, and is properly resolved by Increment 3's
            //     cross-station RAIM consensus (which CAN tell a real slip from a
            //     single-station false positive — something a per-station T0 cannot).
            //     Wiring external BIA into the network MW would make the wide-lane
            //     integer trustworthy and let this become a REFLOAT.
            // The resolver's `reset_sats` is likewise NOT acted on (BIA-less, it is a
            // liberal catch-all of tens of sats/epoch).  The pivot (stations[0]) is
            // never touched (its gauge-pinned ambiguities are Increment-4 territory).
            int n_t0_repairs = 0, n_t0_left = 0;
            std::string t0_detail;
            const double t0_frac_max = config_.networkCycleSlipTurboEditFracMax();
            const int    t0_dn_max   = config_.networkCycleSlipTurboEditDnMax();
            for (int i = i_lo; i < n_sta_total; ++i) {
                if (!present[i]) continue;
                for (const auto& r : sta_repairs[i]) {
                    if (r.dN == 0 || !(std::isfinite(r.lambda) && r.lambda > 0.0)) continue;
                    auto it = local_carry.find(std::make_tuple(i, r.satellite, r.signal));
                    if (it == local_carry.end()) continue;
                    const int adn = (r.dN < 0) ? -r.dN : r.dN;
                    if (r.frac <= t0_frac_max && adn <= t0_dn_max) {
                        it->second.value_abs += static_cast<double>(r.dN) * r.lambda;  // REPAIR
                        ++n_t0_repairs;
                        if (n_t0_repairs <= 12)
                            t0_detail += fmt::format("{}{}/{}/{} dN={} f={:.2f}",
                                                     t0_detail.empty() ? "" : ", ",
                                                     stations[i].name, r.satellite, r.signal,
                                                     r.dN, r.frac);
                    } else {
                        ++n_t0_left;  // LEAVE (untrusted integer — likely a float-MW false positive)
                        if (n_t0_left <= 6)
                            log->debug("Network[phase-leveling T0]: TOW {:.1f} LEFT low-confidence "
                                       "{}/{}/{} dN={} frac={:.2f} (untrusted; carry kept)",
                                       se.time.tow, stations[i].name, r.satellite, r.signal,
                                       r.dN, r.frac);
                    }
                }
            }
            if (n_t0_repairs > 0 || n_t0_left > 0)
                log->info("Network[phase-leveling T0]: TOW {:.1f} applied {} repair(s) [{}]; "
                          "left {} low-confidence detection(s) untouched",
                          se.time.tow, n_t0_repairs, t0_detail, n_t0_left);

            // MW DETECTOR (mw_detect) → refloat: the iono-safe complement to IF-phase TD.
            // Two signals, neither of which uses the geometry-free ionosphere (so both stay
            // valid at the 30 s product rate, unlike the TurboEdit MW+GF integer REPAIR):
            //   (a) a CONFIDENT wide-lane integer jump collected in PASS A (sta_mw_refloat);
            //   (b) the receiver's own LLI loss-of-lock flag (bit 0) on a carried signal —
            //       the ground truth a receiver sets on a cycle slip / re-acquisition (this
            //       is what a real slip like G23@GAMB carries when MW/IF can't see it).
            // Either REFLOATS the affected ambiguity (marks it pending-reseed so PASS B
            // re-estimates it from this epoch's data) — never applying an integer, so a wrong
            // wide-lane integer can never bias the gauge.  Serial, station-ordered →
            // deterministic.  The refloat is meaningful only when the carry is warm; cold, the
            // detection is diagnostic (the free per-epoch estimate needs no reseed).  Pivot
            // (stations[0]) is never touched (its ambiguities are gauge-pinned).
            if (config_.networkCycleSlipMwDetect()) {
                auto refloat_signal = [&](int i, const std::string& sat,
                                          const std::string& signal) -> bool {
                    if (!carry_warm) return false;
                    bool any = false;
                    StateManager& sm = *stations[i].sm;
                    for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                        const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                        if (!parts || parts->satellite != sat) continue;
                        if (parts->signal == signal) {
                            sm.markAmbiguityPendingReseed(amb_key);
                            any = true;
                        }
                    }
                    return any;
                };
                int n_mw = 0, n_lli = 0;
                std::string mw_detail;
                for (int i = i_lo; i < n_sta_total; ++i) {
                    if (!present[i]) continue;
                    // (a) confident wide-lane integer slips detected in PASS A.  A satellite
                    // can slip on several signal-pairs at once (multi-freq); refloat every
                    // band but count / log / emit ONCE per (station, satellite).
                    std::set<std::string> mw_done;
                    for (const auto& [sat, sig1, sig2] : sta_mw_refloat[i]) {
                        const bool a = refloat_signal(i, sat, sig1);
                        const bool b = refloat_signal(i, sat, sig2);
                        if ((a || b) && mw_done.insert(sat).second) {
                            emitEvent("T0_mw_detect", "mw_wl_slip", sat, stations[i].name,
                                      std::nullopt, std::nullopt, "refloat", sig1 + ";" + sig2);
                            if (++n_mw <= 12)
                                mw_detail += fmt::format("{}{}/{}", mw_detail.empty() ? "" : ", ",
                                                         stations[i].name, sat);
                        }
                    }
                    // (b) receiver LLI loss-of-lock (bit 0) on any carried phase signal.
                    // modeled[i] holds one entry per satellite; refloat every LLI'd band but
                    // count / emit ONCE per satellite (an LLI on both L1 and L2 is one slip).
                    for (const auto& obs : modeled[i]) {
                        bool refloated = false;
                        for (const auto& [code, lli] : obs.lli_flags) {
                            if (code.empty() || code[0] != 'L' || !(lli & 1)) continue;
                            if (refloat_signal(i, obs.satellite, code)) refloated = true;
                        }
                        if (refloated) {
                            emitEvent("T0_mw_detect", "lli_slip", obs.satellite,
                                      stations[i].name, std::nullopt, std::nullopt,
                                      "refloat", "LLI");
                            ++n_lli;
                        }
                    }
                }
                if (n_mw > 0 || n_lli > 0)
                    log->info("Network[MW detect]: TOW {:.1f} refloated {} wide-lane slip(s){} + "
                              "{} LLI loss-of-lock(s)", se.time.tow, n_mw,
                              mw_detail.empty() ? std::string{} : (" [" + mw_detail + "]"), n_lli);
            }

            // IF-phase triple-difference slip detection + refloat (this increment).
            // Each non-pivot station's detector ran in DEFERRED mode above: it computed
            // the between-satellite, between-epoch ionosphere-free phase double
            // differences (ΔΔφ_IF) per (sat, signal-pair) and cached them, but did NOT
            // flag.  The driver decides, elevation-aware and per (station, signal-pair):
            //
            //  1. CENTER — pool ΔΔφ_IF across ALL present non-pivot stations BY SIGNAL
            //     PAIR, take the median `med` (raw m): the network common-mode (frame /
            //     datum wobble), which is elevation-independent, so it is removed in raw
            //     space.
            //  2. SCALE — normalize each residual to a zenith-equivalent, z = (dd−med)·
            //     sin(el) = (dd−med)/ifElevMap, so satellites at every elevation share one
            //     scale.  The pooled s_pool = 1.4826·median(|z|) is the warmup PRIOR; each
            //     (station, pair) also keeps an EWMA of its OWN z-scale (if_pair_scale).
            //     The effective scale shrinks from the pool toward the station's EWMA as
            //     that station-pair accumulates clean samples (empirical-Bayes, weight
            //     count/(count+κ)) — so a quiet reference station and a noisy modernized-
            //     signal station each get their OWN envelope, not one pooled threshold set
            //     by the majority (the fix for the L5Q/L2L over-firing).
            //  3. THRESHOLD sat i — max(k·s_eff·ifElevMap(el_i), floor_frac·λ_IF(1,1)):
            //     the empirical term re-inflated to sat i's elevation, the physical
            //     (1,1)-integer floor kept in raw m so a real slip is caught at ANY
            //     elevation.  Flag if |dd_i − med| exceeds it.
            //  4. A flagged arc has its detector gradient reset (fresh arc next epoch —
            //     arc hygiene, every epoch) and, when the carry is active, BOTH band
            //     ambiguities marked pending-reseed so PASS B refloats them.  Clean
            //     survivors then update the station-pair EWMA (slips excluded, so the
            //     scale never absorbs the event it must catch).
            //
            // The detector's own reference-satellite common-mode slip keys (which the
            // per-pair pool cannot see) are folded in too.  Thresholds are deliberately
            // loose; the sharp pass is a future post-estimation phase validation, not yet
            // built.  Serial, station-ordered → deterministic.  Pivot has no detector.
            if (config_.networkCycleSlipIfPhaseTd()) {
                const double k_mad        = config_.networkCycleSlipIfPhaseTdKMad();
                const double floor_frac   = config_.networkCycleSlipIfPhaseTdFloorFrac();
                const int    min_pool     = config_.networkCycleSlipIfPhaseTdMinPool();
                const double ewma_alpha   = config_.networkCycleSlipIfPhaseTdEwmaAlpha();
                const double shrink_kappa = config_.networkCycleSlipIfPhaseTdShrinkKappa();

                // Refloat both band ambiguities of (sat, sig1, sig2) at station i (only
                // when the carry is active; cold, the detection is diagnostic only).
                auto refloat_pair = [&](int i, const std::string& sat,
                                        const std::string& sig1, const std::string& sig2) {
                    if (!carry_warm) return;
                    StateManager& sm = *stations[i].sm;
                    for (const auto& [amb_key, sidx] : sm.getAmbiguities()) {
                        const auto parts = StateKeys::parseAmbiguityKey(amb_key);
                        if (!parts || parts->satellite != sat) continue;
                        if (parts->signal == sig1 || parts->signal == sig2)
                            sm.markAmbiguityPendingReseed(amb_key);
                    }
                };

                // Pool this epoch's ΔΔφ_IF by signal pair across present non-pivot stations.
                // Carry elevation (for the stochastic normalization) and the per-sat (1,1)
                // IF wavelength (for the physical floor — exact even for GLONASS FDMA).
                struct PairSample { int sta; std::string sat, sig1, sig2; double dd, el, lambda; };
                std::map<std::string, std::vector<PairSample>> by_pair;
                for (int i = i_lo; i < n_sta_total; ++i) {
                    if (!present[i] || !stations[i].if_slip) continue;
                    for (const auto& [sat, jp] : stations[i].if_slip->getLastIfJumps()) {
                        if (!jp.valid || !std::isfinite(jp.delta_grad_m)) continue;
                        if (jp.sig1.empty() || jp.sig2.empty()) continue;
                        const std::string pair = jp.sig1 + "_" + jp.sig2;
                        by_pair[pair].push_back({i, sat, jp.sig1, jp.sig2,
                                                 jp.delta_grad_m, jp.elevation_deg, jp.lambda_if_m});
                    }
                }

                int n_if_arcs = 0, n_sparse = 0, n_warm_sp = 0;
                std::string if_detail;
                for (auto& [pair, samples] : by_pair) {
                    // (1) Network common-mode center: median of the raw DDs (m).
                    std::vector<double> raw;
                    raw.reserve(samples.size());
                    for (const auto& s : samples) raw.push_back(s.dd);
                    std::nth_element(raw.begin(), raw.begin() + raw.size() / 2, raw.end());
                    const double med = raw[raw.size() / 2];

                    // (2) Pooled elevation-normalized robust scale (zenith-equiv m):
                    //     s_pool = 1.4826·median(|z|), z = (dd−med)/ifElevMap(el).
                    std::vector<double> zabs;
                    zabs.reserve(samples.size());
                    for (const auto& s : samples)
                        zabs.push_back(std::abs((s.dd - med) / ifElevMap(s.el)));
                    std::nth_element(zabs.begin(), zabs.begin() + zabs.size() / 2, zabs.end());
                    const double s_pool = 1.4826 * zabs[zabs.size() / 2];
                    if (static_cast<int>(samples.size()) < min_pool) ++n_sparse;

                    // Group the pair's samples by station (ordered → deterministic) so each
                    // station-pair thresholds with its OWN shrunk scale, then updates its
                    // OWN EWMA once this epoch's flags are known.
                    std::map<int, std::vector<const PairSample*>> by_sta;
                    for (const auto& s : samples) by_sta[s.sta].push_back(&s);

                    for (auto& [sta, plist] : by_sta) {
                        auto& sc = stations[sta].if_pair_scale[pair];

                        // Empirical-Bayes shrinkage: pool prior (cold) → station EWMA (warm).
                        const double w = static_cast<double>(sc.count) /
                                         (static_cast<double>(sc.count) + shrink_kappa);
                        double s_eff = (sc.count > 0) ? (w * sc.ewma_scale + (1.0 - w) * s_pool)
                                                      : s_pool;
                        if (!(s_eff > 0.0)) s_eff = s_pool;  // degenerate all-equal pool
                        if (sc.count >= static_cast<long>(shrink_kappa)) ++n_warm_sp;

                        std::vector<double> clean_zabs;
                        for (const PairSample* sp : plist) {
                            const double emap    = ifElevMap(sp->el);
                            const double floor_m = floor_frac * sp->lambda;
                            const double thr     = std::max(k_mad * s_eff * emap, floor_m);
                            const double abs_dev = std::abs(sp->dd - med);
                            const bool   flagged = (abs_dev > thr);
                            if (trace_flag_rates) { ++fr_if_slip.opp; if (flagged) ++fr_if_slip.flag; }  // B8/T1
                            if (flagged) {
                                stations[sp->sta].if_slip->resetSatellite(sp->sat);  // restart arc
                                refloat_pair(sp->sta, sp->sat, sp->sig1, sp->sig2);
                                emitEvent("T1_if_slip", "if_slip", sp->sat,
                                          stations[sp->sta].name, abs_dev, thr,
                                          carry_warm ? "refloat" : "diagnostic",
                                          sp->sig1 + ";" + sp->sig2);
                                ++n_if_arcs;
                                if (n_if_arcs <= 12)
                                    if_detail += fmt::format("{}{}/{}_{}_{}", if_detail.empty() ? "" : ", ",
                                                             stations[sp->sta].name, sp->sat, sp->sig1, sp->sig2);
                            } else {
                                clean_zabs.push_back(abs_dev / emap);
                            }
                            // Deep-investigation row: the full decision context for this
                            // (station, sat, pair) sample.  sc.ewma_scale / sc.count are the
                            // decision-time (pre-update) values used to form s_eff.
                            if (if_slip_trace.is_open())
                                if_slip_trace
                                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                                    << stations[sp->sta].name << ',' << sp->sat << ','
                                    << sp->sig1 << ',' << sp->sig2 << ','
                                    << fmt::format("{:.2f}", sp->el) << ','
                                    << fmt::format("{:.6f}", sp->dd) << ',' << fmt::format("{:.6f}", med) << ','
                                    << fmt::format("{:.6f}", s_pool) << ',' << fmt::format("{:.6f}", sc.ewma_scale) << ','
                                    << sc.count << ',' << fmt::format("{:.3f}", w) << ','
                                    << fmt::format("{:.6f}", s_eff) << ',' << fmt::format("{:.3f}", emap) << ','
                                    << fmt::format("{:.6f}", thr) << ',' << fmt::format("{:.6f}", floor_m) << ','
                                    << fmt::format("{:.6f}", abs_dev) << ',' << (flagged ? 1 : 0) << ','
                                    << (carry_warm ? 1 : 0) << '\n';
                        }

                        // (4) Fold clean survivors into the station-pair EWMA (slips excluded).
                        if (!clean_zabs.empty()) {
                            std::nth_element(clean_zabs.begin(),
                                             clean_zabs.begin() + clean_zabs.size() / 2, clean_zabs.end());
                            const double s_epoch = 1.4826 * clean_zabs[clean_zabs.size() / 2];
                            sc.ewma_scale = (sc.count == 0)
                                ? s_epoch
                                : (1.0 - ewma_alpha) * sc.ewma_scale + ewma_alpha * s_epoch;
                            sc.count += static_cast<long>(clean_zabs.size());
                        }
                    }
                }

                // Reference-satellite common-mode slips (per station; pool can't see them).
                for (int i = i_lo; i < n_sta_total; ++i) {
                    if (!present[i]) continue;
                    for (const auto& rkey : sta_if_resets[i]) {
                        std::string sat, sig1, sig2;
                        if (!parseIfPairResetKey(rkey, sat, sig1, sig2)) continue;
                        refloat_pair(i, sat, sig1, sig2);
                        emitEvent("T1_if_slip", "refsat_reset", sat, stations[i].name,
                                  std::nullopt, std::nullopt,
                                  carry_warm ? "refloat" : "diagnostic", sig1 + ";" + sig2);
                        ++n_if_arcs;
                        if (n_if_arcs <= 12)
                            if_detail += fmt::format("{}{}/{} (refsat)", if_detail.empty() ? "" : ", ",
                                                     stations[i].name, rkey);
                    }
                }

                if (n_if_arcs > 0)
                    log->info("Network[IF-phase slip]: TOW {:.1f} flagged {} arc(s){} across {} pair(s) "
                              "({} station-pair(s) warm){}{}",
                              se.time.tow, n_if_arcs,
                              carry_warm ? " for refloat" : " (carry cold; diagnostic)",
                              by_pair.size(), n_warm_sp,
                              n_sparse > 0 ? fmt::format(", {} pair(s) under min_pool({})", n_sparse, min_pool)
                                           : std::string{},
                              if_detail.empty() ? std::string{} : (" [" + if_detail + "]"));
            }
        }

        // RAW cycle-slip observables (network.trace_slip_raw), computed straight from
        // raw_observed — INDEPENDENT of whether any slip detector is armed.  Written BEFORE
        // the observability pre-filter so a satellite about to be dropped is still captured.
        // For each present station+satellite pick the primary dual-frequency pair (two lowest
        // DISTINCT bands whose phase AND matching code "C"+sig[1:] are present) and emit MW,
        // GF (=L1−L2), first-signal phase-minus-code (all m) plus the per-band RINEX LLI.  The
        // factor c cancels out of MW/GF, so wavelengths alone (FDMA-correct via the satellite)
        // suffice.  Absolute values — diff adjacent epochs offline for the slip signature.
        if (slip_raw_trace.is_open() && slip_raw_ppp_utils) {
            for (int i = 0; i < n_sta_total; ++i) {
                if (!present[i]) continue;
                for (const auto& obs : modeled[i]) {
                    std::vector<std::string> ph;  // phase signals with a matching code
                    for (const auto& kv : obs.raw_observed) {
                        const std::string& k = kv.first;
                        if (k.size() < 2 || k[0] != 'L') continue;
                        if (obs.raw_observed.count("C" + k.substr(1))) ph.push_back(k);
                    }
                    if (ph.empty()) continue;
                    std::sort(ph.begin(), ph.end(),
                              [](const std::string& a, const std::string& b) { return a[1] < b[1]; });
                    const std::string s1 = ph.front();
                    std::string s2;
                    for (const auto& k : ph) if (k[1] != s1[1]) { s2 = k; break; }  // distinct band

                    const double lam1 = slip_raw_ppp_utils->getWavelength(obs.system, s1, obs.satellite);
                    if (!(std::isfinite(lam1) && lam1 > 0.0)) continue;
                    const double L1_cyc = obs.raw_observed.at(s1);
                    const double P1_m   = obs.raw_observed.at("C" + s1.substr(1));
                    const double L1_m   = L1_cyc * lam1;
                    const double l1c1   = L1_m - P1_m;
                    const int    lli1   = obs.lli_flags.count(s1) ? obs.lli_flags.at(s1) : 0;

                    bool dual = !s2.empty();
                    double mw = 0.0, gf = 0.0;
                    int    lli2 = 0;
                    if (dual) {
                        const double lam2 = slip_raw_ppp_utils->getWavelength(obs.system, s2, obs.satellite);
                        if (std::isfinite(lam2) && lam2 > 0.0) {
                            const double inv1 = 1.0 / lam1, inv2 = 1.0 / lam2;
                            const double L2_cyc = obs.raw_observed.at(s2);
                            const double P2_m   = obs.raw_observed.at("C" + s2.substr(1));
                            gf = L1_m - L2_cyc * lam2;
                            mw = (L1_cyc - L2_cyc) / (inv1 - inv2)
                               - (P1_m * inv1 + P2_m * inv2) / (inv1 + inv2);
                            lli2 = obs.lli_flags.count(s2) ? obs.lli_flags.at(s2) : 0;
                        } else {
                            dual = false;
                        }
                    }
                    slip_raw_trace
                        << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                        << stations[i].name << ',' << obs.satellite << ','
                        << s1 << ',' << (dual ? s2 : std::string{}) << ','
                        << fmt::format("{:.2f}", obs.elevation) << ','
                        << (dual ? fmt::format("{:.4f}", mw) : std::string{}) << ','
                        << (dual ? fmt::format("{:.4f}", gf) : std::string{}) << ','
                        << fmt::format("{:.4f}", l1c1) << ',' << lli1 << ',' << lli2 << '\n';
                }
            }
        }

        // --- Cross-station observability pre-filter -------------------------
        // A satellite clock is only well-determined for a network product when at
        // least kMinStationsPerSat stations observe it: a single-station satellite
        // clock is tied to just that one receiver clock and, at marginal geometry,
        // becomes the extra near-zero direction that trips the rank gate.  Drop
        // such satellites from the network ENTIRELY this epoch — both the shared
        // clock/phase-bias states AND the underlying observations.  Excluding the
        // observations is essential: a satellite kept in the design with NO shared
        // clock column would pin its station's receiver clock absolutely and so
        // erase the very clock gauge the datum is meant to pin (declared > observed
        // → the gate would then reject every epoch).  Dropping the whole satellite
        // lets one weak satellite no longer force the WHOLE epoch to be withheld;
        // the gate stays as the final safety net for a genuine residual deficiency.
        constexpr int kMinStationsPerSat = 2;
        int n_dropped_sats = 0;
        {
            std::map<std::string, int> sat_station_count;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                for (const auto& kv : stations[i].sm->getSatClockStates())
                    ++sat_station_count[StateKeys::satelliteFromSatClockKey(kv.first)];
            }
            std::set<std::string> dropped_sats;
            for (const auto& kv : sat_station_count)
                if (kv.second < kMinStationsPerSat) { dropped_sats.insert(kv.first); ++n_dropped_sats; }

            if (!dropped_sats.empty()) {
                for (size_t i = 0; i < stations.size(); ++i) {
                    if (!present[i]) continue;
                    // Remove the dropped satellites' observations so their rows are
                    // not built (and so cannot pin the receiver clock).
                    auto& obs = modeled[i];
                    obs.erase(std::remove_if(obs.begin(), obs.end(),
                                  [&](const PppObservation& o) { return dropped_sats.count(o.satellite) != 0; }),
                              obs.end());
                    // Prune the now-orphaned shared states.
                    StateManager& sm = *stations[i].sm;
                    std::set<std::string> keep_clk, keep_pb;
                    for (const auto& kv : sm.getSatClockStates())
                        if (!dropped_sats.count(StateKeys::satelliteFromSatClockKey(kv.first)))
                            keep_clk.insert(kv.first);
                    for (const auto& kv : sm.getSatPhaseBiasStates()) {
                        const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                        if (p && !dropped_sats.count(p->satellite)) keep_pb.insert(kv.first);
                    }
                    sm.pruneSatClockStatesExcept(keep_clk);
                    sm.pruneSatPhaseBiasStatesExcept(keep_pb);
                }
            }
        }
        // The final shared phase-bias set + which station anchors (gauge-pins) each
        // (sat,signal).  Two realizations of the SAME minimum constraint (exactly
        // one ambiguity pin per phase-bias freedom):
        //   Legacy (network.phase_datum_prioritized OFF): the single pivot's pruned
        //     phase-bias states ARE the shared set (each backed by a pivot-receiver
        //     pin).  Byte-identical.
        //   Prioritized: elect, per (sat,signal), the highest-coverage receiver that
        //     tracks it with a live ambiguity as its anchor — decoupled from the
        //     clock pivot, so a satellite low/unseen at the clock pivot keeps a clean
        //     gauge at another station instead of jumping.
        if (!phase_datum_prioritized) {
            shared_phase_keys.clear();
            for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
                shared_phase_keys.insert(kv.first);
        } else {
            const double pref_el = config_.networkPhaseDatumPrefElevDeg();
            const bool   sticky  = config_.networkPhaseDatumSticky();
            const int    pdp_min_sta = config_.networkPhaseDatumMinStations();

            // (a) Per present station: satellite elevation (from the pruned obs) and
            //     the ANCHORABLE keys — the per-(sat,signal) phase-bias STATES this
            //     station allocated in PASS A.  NOTE the timing: the ambiguity states
            //     are allocated later, in PASS B's computeMeasurementUpdate, so we
            //     canNOT test getAmbiguityIndex here (it is -1 for everything at this
            //     point).  ensureNetworkSharedStates allocates a phase-bias state via
            //     the SAME usability gate as the row build, so a live phase-bias state
            //     ⟹ a phase row ⟹ a pinnable ambiguity in PASS B; the pin fold's own
            //     getAmbiguityIndex + h2local check is the final guard (a miss orphans
            //     the key, caught by the null-space gate → withhold + heal, rare).
            //     usable_keys[i]: "sat_signal" -> "sat|signal|PB".
            std::vector<std::map<std::string, double>>      sat_el(stations.size());
            std::vector<std::map<std::string, std::string>> usable_keys(stations.size());
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                for (const auto& o : modeled[i]) sat_el[i][o.satellite] = o.elevation;
                StateManager& sm = *stations[i].sm;
                for (const auto& kv : sm.getSatPhaseBiasStates()) {
                    const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                    if (!p) continue;
                    usable_keys[i].emplace(p->satellite + "_" + p->signal, kv.first);
                }
            }

            // (b) Per-constellation candidate receivers, ranked by DESCENDING
            //     coverage (# satellites tracked at >= pref_el) then lexical station
            //     name (determinism).  A station is a candidate for a constellation
            //     iff it can anchor >=1 of its keys; a low-elevation-only tracker
            //     stays a (last-ranked) candidate so it can still anchor a setting
            //     satellite nobody else sees.
            std::map<char, std::map<size_t, int>> cover;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                for (const auto& [sat, el] : sat_el[i])
                    if (!sat.empty() && el >= pref_el) ++cover[sat.front()][i];
            }
            auto cov = [&](char c, size_t i) -> int {
                auto it = cover.find(c); if (it == cover.end()) return 0;
                auto jt = it->second.find(i); return jt == it->second.end() ? 0 : jt->second;
            };
            std::map<char, std::set<size_t>> cand_set;
            std::map<std::string, std::string> union_keys;   // "sat_signal" -> "sat|signal|PB"
            for (size_t i = 0; i < stations.size(); ++i)
                for (const auto& [amb, pb] : usable_keys[i]) {
                    if (!amb.empty()) cand_set[amb.front()].insert(i);
                    union_keys.emplace(amb, pb);
                }

            // Coverage-threshold gating of the anchored union
            // (network.phase_datum_min_stations; 1 = off, byte-identical): drop
            // every (sat,signal) tracked by fewer than k present stations BEFORE
            // the election.  A one/two-tracker column is exactly the pathology
            // class the pdp validation surfaced: it (a) ejects a marginal
            // tracking station as non-PD (its local block barely separates the
            // column from that station's own ambiguity — the TWTF/VOIM symptom),
            // (b) re-levels by a full ambiguity lump when its one-station anchor
            // pool thins, and (c) leaks its anchor lump into the clock↔phase-bias
            // split.  A gated key's PHASE rows still serve the satellite clock:
            // each station's float ambiguity absorbs the bias, exactly as legacy
            // non-shared signals (and GLONASS network phase) always have.  The
            // tracking stations' allocated phase-bias states are pruned so the
            // per-station column sets stay consistent with the gated union (the
            // kMinStationsPerSat prune pattern above); the emitter sees a clean
            // per-key absence (staleness / first-sighting semantics on return).
            if (pdp_min_sta > 1) {
                std::map<std::string, int> n_trk;
                for (size_t i = 0; i < stations.size(); ++i)
                    for (const auto& kv : usable_keys[i]) ++n_trk[kv.first];
                std::set<std::string> gated_amb;   // "sat_signal" excluded this epoch
                for (auto it = union_keys.begin(); it != union_keys.end();) {
                    if (n_trk[it->first] < pdp_min_sta) {
                        gated_amb.insert(it->first);
                        it = union_keys.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (!gated_amb.empty()) {
                    ++n_gated_epochs;
                    n_gated_key_epochs += gated_amb.size();
                    for (size_t i = 0; i < stations.size(); ++i) {
                        if (!present[i]) continue;
                        StateManager& sm = *stations[i].sm;
                        std::set<std::string> keep_pb;
                        bool drop_any = false;
                        for (const auto& kv : sm.getSatPhaseBiasStates()) {
                            const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
                            if (p && gated_amb.count(p->satellite + "_" + p->signal)) {
                                drop_any = true;
                                continue;
                            }
                            keep_pb.insert(kv.first);
                        }
                        if (drop_any) sm.pruneSatPhaseBiasStatesExcept(keep_pb);
                    }
                }
            }

            std::map<char, std::vector<size_t>> cand;
            for (auto& [c, s] : cand_set) {
                std::vector<size_t> v(s.begin(), s.end());
                std::stable_sort(v.begin(), v.end(), [&](size_t a, size_t b) {
                    const int ca = cov(c, a), cb = cov(c, b);
                    if (ca != cb) return ca > cb;
                    return stations[a].name < stations[b].name;
                });
                cand[c] = std::move(v);
            }

            // (c) Elect one anchor per (sat,signal): keep the STICKY incumbent while
            //     it still tracks the key (fewest handovers), else the highest-ranked
            //     candidate that tracks it — concentrating most of a constellation on
            //     one receiver (frame-consistent) with graceful per-satellite
            //     fallback.  Record the pin in the anchor's OWN pinned_amb_keys (its
            //     local block — the load-bearing invariant), the shared column, and
            //     this_epoch_anchor (read at the emit site for handover signalling).
            for (auto& st : stations) st.pinned_amb_keys.clear();
            this_epoch_anchor.clear();
            shared_phase_keys.clear();
            std::map<std::string, std::string> new_incumbent;
            for (const auto& [amb, pbkey] : union_keys) {
                const char c = amb.empty() ? char(0) : amb.front();
                size_t anchor = static_cast<size_t>(-1);
                if (sticky) {
                    auto it = anchor_incumbent.find(amb);
                    if (it != anchor_incumbent.end())
                        for (size_t i = 0; i < stations.size(); ++i)
                            if (present[i] && stations[i].name == it->second
                                && usable_keys[i].count(amb)) { anchor = i; break; }
                }
                if (anchor == static_cast<size_t>(-1)) {
                    auto cit = cand.find(c);
                    if (cit != cand.end())
                        for (size_t i : cit->second)
                            if (usable_keys[i].count(amb)) { anchor = i; break; }
                }
                if (anchor == static_cast<size_t>(-1)) continue;   // no tracker (unreachable)
                stations[anchor].pinned_amb_keys.insert(amb);
                this_epoch_anchor[amb] = stations[anchor].name;
                new_incumbent[amb]     = stations[anchor].name;
                shared_phase_keys.insert(pbkey);
            }
            anchor_incumbent.swap(new_incumbent);
        }

        // --- Register the shared column layout (reset the hoisted estimator) ---
        est.reset();
        std::vector<std::string> registered_clock_sats;
        std::set<std::string> clk_seen;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i]) continue;
            for (const auto& kv : stations[i].sm->getSatClockStates()) {
                const std::string sat = StateKeys::satelliteFromSatClockKey(kv.first);
                est.registerClock(sat);
                if (clk_seen.insert(sat).second) registered_clock_sats.push_back(sat);
            }
        }
        for (const auto& pk : shared_phase_keys) {
            const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
            if (parts) est.registerPhaseBias(parts->satellite, parts->signal);
        }
        // Register the satellite CODE-bias (OSB) columns (W6, full multi-band):
        // EVERY (sat,signal) — every frequency band AND every tracking channel
        // (attribute) — that at least two present stations carry an OSB state for.
        // Collected (deduplicated, first-seen order) for the code-bias datum
        // constraints, the D2 carry-forward, and the solution trace.  Empty unless
        // estimate_sat_code_bias.
        //
        // The ONLY admission gate is >=2-station coverage: a single-station code
        // bias is perfectly confounded with that one station's receiver code bias
        // and cannot be separated, so it stays a local nuisance (its sat-bias state
        // is pruned below, leaving its raw code row to fold into the receiver
        // clock / slant iono — the pre-W6 path).  Everything with >=2 stations is a
        // first-class shared OSB column and is datumed below: the GIM between-
        // satellite iono structure makes the inter-band DCBs observable, while
        // same-band channels (C1C vs C1W, C2W vs C2L, ...) need no iono at all
        // (equal frequency → the receiver and ionosphere terms cancel in the raw
        // between-channel difference, leaving the satellite inter-channel bias).
        // The earlier slice confined the OSB to the two lowest bands and so emitted
        // nothing on a 3rd band; the datum below now pins every registered column.
        std::vector<std::pair<std::string, std::string>> registered_code_keys;
        if (config_.estimateNetworkSatCodeBias()) {
            std::map<std::string, int> cb_station_count;   // "sat|signal" -> #stations
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                for (const auto& kv : stations[i].sm->getSatBiasStates())
                    ++cb_station_count[kv.first];
            }
            std::set<std::string> keep_keys;
            for (const auto& [key, cnt] : cb_station_count)
                if (cnt >= 2) keep_keys.insert(key);

            std::set<std::string> cb_seen;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (!present[i]) continue;
                StateManager& sm = *stations[i].sm;
                std::set<std::string> station_keep;
                for (const auto& kv : sm.getSatBiasStates()) {
                    if (!keep_keys.count(kv.first)) continue;
                    station_keep.insert(kv.first);
                    const auto parts = StateKeys::parseSatBiasKey(kv.first);
                    if (!parts) continue;
                    est.registerCodeBias(parts->satellite, parts->signal);
                    if (cb_seen.insert(kv.first).second)
                        registered_code_keys.emplace_back(parts->satellite, parts->signal);
                }
                sm.pruneSatBiasStatesExcept(station_keep);
            }
        }

        // Register the global SH-VTEC coefficient columns (estimate_iono_sh).  The
        // layout is FIXED — all (N+1)² coefficients every epoch, independent of the
        // satellite set — so the field is continuous across epochs and the Kaula
        // prior can keep every coefficient soft-pinned (the rank gate then stays at
        // #constellations regardless of coverage; design §6.1).
        if (config_.estimateNetworkIonoSh() && sh_model) {
            for (const auto& c : sh_model->coeffs())
                est.registerVtecCoeff(c.n, c.m, c.trig);
        }

        const int n_clock = static_cast<int>(registered_clock_sats.size());
        const int n_pb    = static_cast<int>(shared_phase_keys.size());
        const int n_cb    = static_cast<int>(registered_code_keys.size());
        const int n_present = static_cast<int>(std::count(present.begin(), present.end(), true));
        last_n_present    = n_present;   // RTB-6: most-recent station-presence count for the health heartbeat
        last_n_clock_sats = n_clock;     // RTB-6 (P4b): most-recent registered-clock-sat count for the health heartbeat

        // Column -> human descriptor map (W6 estimability diagnostic): lets the
        // one-shot spectrum dump name which shared parameters span the near-zero
        // eigen-directions (unpinned gauges) instead of guessing.  Populated only
        // when the (default-OFF) estimability trace is armed — it feeds nothing but
        // that dump, so a production run skips the string-building entirely.
        std::vector<std::string> col_desc(static_cast<size_t>(est.numShared()));
        if (!spectrum_diag_done && nc.trace_estimability) {
            for (const std::string& sat : registered_clock_sats)
                col_desc[static_cast<size_t>(est.clockColumn(sat))] = "CLK|" + sat;
            for (const auto& pk : shared_phase_keys) {
                const auto p = StateKeys::parseSatPhaseBiasKey(pk);
                if (p) col_desc[static_cast<size_t>(est.phaseBiasColumn(p->satellite, p->signal))]
                           = "PB|" + p->satellite + "|" + p->signal;
            }
            for (const auto& kv : registered_code_keys)
                col_desc[static_cast<size_t>(est.codeBiasColumn(kv.first, kv.second))]
                    = "CB|" + kv.first + "|" + kv.second;
            // Name the SH-VTEC columns — a HARD requirement (design §6.4): otherwise
            // a missing SH datum prints as an anonymous colNNN in the spectrum dump,
            // indistinguishable from a clock-datum fault.
            if (config_.estimateNetworkIonoSh() && sh_model) {
                for (const auto& c : sh_model->coeffs())
                    col_desc[static_cast<size_t>(est.vtecCoeffColumn(c.n, c.m, c.trig))] =
                        fmt::format("SH|{}|{}|{}", c.n, c.m, c.trig);
            }
        }

        if (est.numShared() == 0) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ",0,0,0,0,0,0,0,0\n";
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              emitEpochStatus("sync_skip", es); }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        est.beginEpoch();

        // Capture the shared-border folds (code-bias gauge constraint + all carried /
        // anchor priors) applied to THIS epoch's arrowhead, so the within-epoch
        // adaptation (drop a culprit → re-solve) can replay the identical shared system
        // over a fresh beginEpoch().  addSharedPrior is additive, so the three prior
        // sets concatenate into one replayable vector.  Populated only when the
        // adaptation is armed; otherwise the appends are cheap no-ops.
        const bool within_epoch_armed = config_.networkRaimWithinEpoch();
        Eigen::MatrixXd cap_H;
        Eigen::VectorXd cap_z;
        double cap_constraint_weight = 0.0;
        bool   cap_has_constraint = false;
        std::vector<NetworkEstimator::SharedPrior> cap_priors;
        // C1: the dense D2 fold is captured separately (keys + effective covariance)
        // and replayed via addSharedPriorDense — deterministic, since the layout is
        // frozen for the epoch.  Non-empty only when the dense fold SUCCEEDED (its
        // fallback routes through cap_priors like every diagonal prior).
        std::vector<NetworkEstimator::SharedPrior> cap_dense_keys;
        Eigen::MatrixXd cap_dense_cov;

        // --- PASS B: per-station assembly, then SERIAL scatter -------------
        // Split into a PARALLEL produce phase (pure per-station compute into
        // pre-sized slots — no shared write) and a SERIAL consume phase that
        // scatters each block into the shared arrowhead in deterministic station
        // order.  computeMeasurementUpdate + splitStation + the pivot pin fold
        // touch only station i's own state and CONST queries on `est` (its column
        // layout is frozen after registration; addStation is the only mutation
        // and runs serially below) — so the produced blocks and their add order
        // are identical for any thread count.
        std::vector<StationAssembly> asm_res(stations.size());

        #pragma omp parallel for schedule(dynamic) num_threads(n_threads) if(parallel_on)
        for (int i = 0; i < n_sta_total; ++i) {
            if (!present[i]) continue;
            StationAssembly& R = asm_res[i];
            try {
                NetStation& st = stations[i];

                EpochMeasurement em = st.mm->computeMeasurementUpdate(
                    modeled[i], st.sm->getStateVector());
                if (em.innovations.size() == 0) { R.empty_innov = true; continue; }
                if (em.H.cols() != static_cast<Eigen::Index>(st.sm->getStateSize())) {
                    R.state_mismatch = true;
                    R.err = fmt::format("{} vs {}", em.H.cols(), st.sm->getStateSize());
                    continue;
                }

                // Alias into the assembly slot so the H->A_r column map survives
                // for the post-solve slant-iono back-substitution (viz trace).
                std::map<int, int>& h2local = R.h2local;
                splitStation(st, est, em, R.A_s, R.A_r, R.w, R.y, h2local);

                // The anchor station(s) fold tight-prior pins on the ambiguities
                // they gauge (the LOCAL half of the S-basis): drive each pinned
                // ambiguity's ABSOLUTE value to 0 so the estimable phase bias
                // absorbs THIS receiver's ambiguity lump (Odijk Eq. 4).  Built into
                // the anchor's OWN block — no shared write.  Legacy: the single
                // pivot pins the whole shared_phase_keys set.  Prioritized: each
                // station pins exactly its elected pinned_amb_keys (the load-bearing
                // invariant — the pin must live in the block whose local ambiguity
                // column it constrains, else the β^s+λN^s confound is not removed).
                const bool do_fold = phase_datum_prioritized ? !st.pinned_amb_keys.empty()
                                                             : st.is_pivot;
                if (do_fold) {
                    const Eigen::VectorXd piv_x = st.sm->getStateVector();
                    std::vector<int> pin_col;       // A_r column of each pinned ambiguity
                    std::vector<double> pin_y;       // innovation = -current absolute value
                    auto foldAmb = [&](const std::string& amb_key) {
                        const int amb_idx = st.sm->getAmbiguityIndex(amb_key);
                        if (amb_idx < 0) return;   // no usable phase row for it this epoch
                        const auto hit = h2local.find(amb_idx);
                        if (hit == h2local.end()) return;  // column dropped (all-zero H)
                        pin_col.push_back(hit->second);
                        pin_y.push_back(-piv_x(amb_idx));
                    };
                    if (phase_datum_prioritized) {
                        for (const auto& amb_key : st.pinned_amb_keys) foldAmb(amb_key);
                    } else {
                        for (const auto& pk : shared_phase_keys) {
                            const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
                            if (!parts) continue;
                            foldAmb(parts->satellite + "_" + parts->signal);
                        }
                    }
                    const int np = static_cast<int>(pin_col.size());
                    if (np > 0) {
                        const Eigen::Index m0 = R.A_s.rows();
                        R.A_s.conservativeResize(m0 + np, Eigen::NoChange);
                        R.A_s.bottomRows(np).setZero();
                        R.A_r.conservativeResize(m0 + np, Eigen::NoChange);
                        R.A_r.bottomRows(np).setZero();
                        R.w.conservativeResize(m0 + np);
                        R.y.conservativeResize(m0 + np);
                        for (int k = 0; k < np; ++k) {
                            R.A_r(m0 + k, pin_col[k]) = 1.0;
                            R.w(m0 + k) = 1e10;   // tight prior (exact gauge)
                            R.y(m0 + k) = pin_y[k];
                        }
                        R.np = np;
                    }
                }

                // GLONASS local-nuisance anchors (code-only network scope).
                // The per-epoch ABSOLUTE network solve carries no Kalman prior on the
                // local nuisances, and GLONASS — unlike GPS/Galileo — has a per-FCN-
                // slot receiver IFB on EVERY band, so there is no IFB-free reference-
                // band code to anchor the slant iono.  Two unobservable families make
                // the local block N_rr non-positive-definite (then the kernel drops
                // the WHOLE station, losing its good GPS/Galileo rows too):
                //   (1) each GLONASS satellite's slant iono I_s — fully confounded
                //       with that satellite's IFB(band,k) + ambiguity (the code rows
                //       determine only the SUM IFB + alpha*I, leaving I_s free);
                //   (2) the {IFB(k)} <-> GLONASS-ISB common mode — unanchored unless a
                //       central-channel k=0 satellite is up (GLONASS has only ~2-3).
                // Append one LOOSE zero-mean prior row per GLONASS per-slot IFB column
                // AND per GLONASS per-satellite iono column to regularize exactly
                // those directions.  PRODUCT-SAFE: the emitted satellite clock depends
                // only on the data-determined IFB+alpha*I sum (and the cross-station
                // consensus), never on how this unobservable split is resolved, so
                // anchoring the split toward zero does not perturb the shared clock.
                // Weight 1.0 (sigma 1 m) sits well above the kernel's relative PD floor
                // (1e-12 * d_max, d_max ~ the 1e10 pivot ambiguity pins), so even the
                // pivot block stays PD.  Built into the station's OWN block (no shared
                // write); inert for GPS/Galileo-only stations and when the flag is off.
                if (config_.enableGlonassNetwork()) {
                    std::vector<int> anchor_cols;
                    for (const auto& [key, sidx] : st.sm->getCodeBiasStates()) {
                        if (!StateKeys::isGlonassPerSlotIfbKey(key)) continue;
                        const auto hit = h2local.find(static_cast<int>(sidx));
                        if (hit != h2local.end()) anchor_cols.push_back(hit->second);
                    }
                    for (const auto& [sat, sidx] : st.sm->getIonosphereStates()) {
                        if (sat.empty() || sat.front() != 'R') continue;  // GLONASS iono only
                        const auto hit = h2local.find(static_cast<int>(sidx));
                        if (hit != h2local.end()) anchor_cols.push_back(hit->second);
                    }
                    const int ni = static_cast<int>(anchor_cols.size());
                    if (ni > 0) {
                        constexpr double kGloAnchorWeight = 1.0;  // 1/(1 m)^2
                        const Eigen::Index m0 = R.A_s.rows();
                        R.A_s.conservativeResize(m0 + ni, Eigen::NoChange);
                        R.A_s.bottomRows(ni).setZero();
                        R.A_r.conservativeResize(m0 + ni, Eigen::NoChange);
                        R.A_r.bottomRows(ni).setZero();
                        R.w.conservativeResize(m0 + ni);
                        R.y.conservativeResize(m0 + ni);
                        for (int k = 0; k < ni; ++k) {
                            R.A_r(m0 + k, anchor_cols[k]) = 1.0;
                            R.w(m0 + k) = kGloAnchorWeight;
                            R.y(m0 + k) = 0.0;  // toward 0 (absolute solve, lin pt 0)
                        }
                    }
                }

                // Ionosphere constraint (W6): the between-satellite structure that
                // makes the satellite DCB observable against the free slant iono.
                // Co-estimated SH-VTEC field (network's own iono) when enabled, else
                // the external GIM.  Folded into the station's OWN block (+ the shared
                // SH border); inert when neither is on.
                if (config_.estimateNetworkIonoSh())
                    appendShIonoRows(st, est, modeled[i], h2local, se.time,
                                     R.A_s, R.A_r, R.w, R.y);
                else if (config_.estimateNetworkSatCodeBias())
                    appendGimIonoRows(st, modeled[i], h2local, se.time,
                                      R.A_s, R.A_r, R.w, R.y);

                // --- Phase-leveling carry: record present ambiguities + build the
                //     diagonal carried prior (Increment 1) ----------------------
                // For every present NON-pivot ambiguity: record it for the
                // post-solve stash, and — if a converged carry exists and is not
                // stale — build its diagonal prior (Λ=1/var_eff, η=Λ·(value_abs−
                // x_lin)) to inject into this station's local block AFTER addStation,
                // BEFORE reduce.  Pivot ambiguities are SKIPPED: their 1e10 pins
                // define the phase-bias gauge exactly, so a carried diagonal there is
                // redundant.  Pure per-station compute + read-only local_carry lookup
                // — no shared write, safe in the parallel produce phase.
                if (carry_warm && (phase_datum_prioritized || !st.is_pivot || zwd_on)) {
                    const Eigen::VectorXd x_lin = st.sm->getStateVector();
                    std::vector<std::tuple<Eigen::Index, double, double>> inj;  // (col, Λ, η)
                    // Ambiguity carry.  Legacy: the whole pivot station is skipped (all
                    // its ambiguities are gauge pins).  Prioritized: every station
                    // carries the arcs it does NOT anchor, so the skip is PER-(sat,
                    // signal) — an anchored ambiguity's 1e10 pin already defines the
                    // gauge, a carried diagonal there is redundant (isGaugePinned).
                    // Inject and harvest partition on the SAME predicate so a pinned
                    // amb never gets a spurious carry that fights its pin.  B9 (P1-4)
                    // keeps the ZWD carry below OUT of any pivot/anchor exclusion.
                    const bool do_amb_carry = phase_datum_prioritized || !st.is_pivot;
                    if (do_amb_carry)
                    for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
                        const auto parts = StateKeys::parseAmbiguityKey(key);
                        if (!parts) continue;
                        if (isGaugePinned(i, parts->satellite, parts->signal))
                            continue;  // anchored (gauge-pinned) — never carried
                        const int amb_idx = static_cast<int>(sidx);
                        if (amb_idx < 0 || amb_idx >= x_lin.size()) continue;
                        const auto hit = h2local.find(amb_idx);
                        if (hit == h2local.end()) continue;  // ambiguity column not in A_r this epoch
                        const Eigen::Index col = static_cast<Eigen::Index>(hit->second);
                        R.pl_amb.push_back(PlAmb{col, parts->satellite, parts->signal,
                                                 x_lin(amb_idx), key});

                        // A pending re-seed marks the prior arc value as no longer
                        // trustworthy (the network's analogue of a slip): re-float from
                        // data this epoch rather than clamp to the stale carry.  The
                        // stash below then captures the fresh value.  (Slip-driven
                        // REFLOAT/DROP proper is Increment 2+; this is the cheap guard
                        // for the only reset trigger the network can raise today.)
                        if (st.sm->isAmbiguityPendingReseed(key)) continue;
                        if (!std::isfinite(x_lin(amb_idx))) continue;  // non-finite state → don't inject

                        const auto cit = local_carry.find(
                            std::make_tuple(i, parts->satellite, parts->signal));
                        if (cit == local_carry.end()) continue;       // no carry yet (new arc)
                        const double gap = pl_now_s - cit->second.last_seen_s;
                        if (gap > pl_max_gap_s) continue;             // stale — housekeeping will drop it
                        // var_eff = var + q·gap, floored by the optional lock-in guard
                        // (var_floor>0 caps Λ so a slowly-biasing ambiguity can never
                        // become a rigid prior the data cannot override).  Only present
                        // (data-bearing) columns are injected, so a tiny Λ never leaves a
                        // column PD-held by the prior alone — the §3.4 floor is not
                        // load-bearing here; this guards the OPPOSITE (lock-in) direction.
                        double var_eff = cit->second.var + pl_q_amb * std::max(0.0, gap);
                        if (pl_var_floor > 0.0) var_eff = std::max(var_eff, pl_var_floor);
                        if (!(var_eff > 0.0)) continue;
                        const double lambda = 1.0 / var_eff;
                        const double eta = lambda * (cit->second.value_abs - x_lin(amb_idx));
                        if (!std::isfinite(eta)) continue;            // belt-and-suspenders: never poison u_r
                        inj.emplace_back(col, lambda, eta);
                    }
                    // --- ZWD carry (B9): treat the tropo ZWD column like one more carried
                    //     local state (random WALK, not random constant → positive q).  The
                    //     prior tuple joins `inj` so it is ordered by the same ascending sort
                    //     and folded by the single addStationPrior inject (byte-identical OFF).
                    if (zwd_on) {
                        const size_t tropo_idx_sz = st.sm->getTropoIndex();
                        if (tropo_idx_sz != std::numeric_limits<size_t>::max()) {
                            const int tropo_idx = static_cast<int>(tropo_idx_sz);
                            if (tropo_idx >= 0 && tropo_idx < x_lin.size()) {
                                const auto zhit = h2local.find(tropo_idx);
                                if (zhit != h2local.end() && std::isfinite(x_lin(tropo_idx))) {
                                    const Eigen::Index zcol = static_cast<Eigen::Index>(zhit->second);
                                    R.pl_zwd_present = true;   // record for the post-solve stash
                                    R.pl_zwd = PlZwd{zcol, x_lin(tropo_idx)};
                                    const auto zit = zwd_carry.find(static_cast<int>(i));
                                    if (zit != zwd_carry.end()) {
                                        const double zgap = pl_now_s - zit->second.last_seen_s;
                                        if (zgap <= pl_max_gap_s) {
                                            const double zvar_eff =
                                                zit->second.var + zwd_q * std::max(0.0, zgap);
                                            if (zvar_eff > 0.0) {
                                                const double zlambda = 1.0 / zvar_eff;
                                                const double zeta =
                                                    zlambda * (zit->second.value_abs - x_lin(tropo_idx));
                                                if (std::isfinite(zeta)) {
                                                    inj.emplace_back(zcol, zlambda, zeta);
                                                    R.pl_zwd_injected = true;   // L7: a carry was actually folded
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // ASCENDING column order → deterministic rank-1 update sequence.
                    std::sort(inj.begin(), inj.end(),
                              [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
                    const Eigen::Index ninj = static_cast<Eigen::Index>(inj.size());
                    R.carry_cols.resize(static_cast<size_t>(ninj));
                    R.carry_dNrr.resize(ninj);
                    R.carry_dur.resize(ninj);
                    for (Eigen::Index k = 0; k < ninj; ++k) {
                        R.carry_cols[static_cast<size_t>(k)] = std::get<0>(inj[static_cast<size_t>(k)]);
                        R.carry_dNrr(k) = std::get<1>(inj[static_cast<size_t>(k)]);
                        R.carry_dur(k)  = std::get<2>(inj[static_cast<size_t>(k)]);
                    }
                }
                R.ok = true;
            } catch (const std::exception& ex) {
                R.threw = true; R.err = ex.what();
            } catch (...) {
                R.threw = true; R.err = "unknown exception";
            }
        }

        // Serial consume: scatter each station's block into the shared arrowhead
        // in deterministic station order, preserving the exact pivot-aborts-epoch
        // / drop-non-pivot disposition of the original single-pass loop.
        int  total_pins = 0;
        bool epoch_ok   = true;
        // est-station index per station (the order addStation was called) so the
        // post-solve back-substitution can recover each present station's local
        // slant-iono block; -1 for stations not added this epoch.
        std::vector<int> est_index_of(stations.size(), -1);
        for (size_t i = 0; i < stations.size() && epoch_ok; ++i) {
            if (!present[i]) continue;
            NetStation&      st = stations[i];
            StationAssembly& R  = asm_res[i];

            if (R.empty_innov) { present[i] = false; continue; }
            if (R.state_mismatch) {
                log->warn("Network: station '{}' H/state mismatch ({})", st.name, R.err);
                // Legacy: the single pivot carries the whole datum, so its loss sinks
                // the epoch.  Prioritized: the gauge is distributed across anchors, so
                // drop this station like any other — any keys it anchored are pruned
                // by the null-space gate (drop-and-heal; the epoch self-heals next).
                if (!phase_datum_prioritized && st.is_pivot) { epoch_ok = false; break; }
                present[i] = false;
                continue;
            }
            if (R.threw || !R.ok) {
                // computeMeasurementUpdate / splitStation threw.  The original
                // single-pass loop did not guard these (an exception escaped the
                // run); under threads they MUST be caught, so their disposition is
                // decided here, matching the addStation policy below: a failed
                // pivot sinks the epoch, any other station is dropped.
                const std::string why = R.err.empty() ? "no usable block" : R.err;
                if (!phase_datum_prioritized && st.is_pivot) {
                    log->warn("Network: PIVOT '{}' assembly failed ({}); skipping epoch",
                              st.name, why);
                    epoch_ok = false;
                    break;
                }
                log->warn("Network: station '{}' assembly failed ({}); dropping it this epoch",
                          st.name, why);
                present[i] = false;
                continue;
            }

            total_pins += R.np;

            try {
                est_index_of[i] = static_cast<int>(est.addStation(R.A_s, R.A_r, R.w, R.y));
            } catch (const std::exception& ex) {
                // One station's rank-deficient / ill-conditioned local block must
                // not sink the whole epoch in a large heterogeneous network: drop
                // it and keep the rest (the null-space gate still guards the
                // result).  On the legacy path the PIVOT is the exception — it
                // carries the S-basis, so without it the epoch has no datum and must
                // be skipped.  Prioritized: the gauge is distributed, so drop it too
                // (an anchor loss is caught by the null-space gate → withhold + heal).
                if (!phase_datum_prioritized && st.is_pivot) {
                    log->warn("Network: PIVOT '{}' addStation rejected ({}); skipping epoch",
                              st.name, ex.what());
                    epoch_ok = false;
                    break;
                }
                log->warn("Network: station '{}' addStation rejected ({}); dropping it this epoch",
                          st.name, ex.what());
                present[i] = false;
                continue;
            }

            // Phase-leveling carry: fold this station's diagonal carried prior into
            // its local block NOW (after addStation, before the gate/solve trigger
            // reduce) so the single arrowhead solve already reflects it.  The pivot's
            // ambiguity columns are gauge-pinned (never carried), but its ZWD column
            // may be (B9), so gate on a non-empty carry set — not on !is_pivot: for the
            // pivot R.carry_cols holds at most the ZWD column, and is empty (no call,
            // byte-identical) unless network.carry_zwd is on.  A reject here drops only
            // this station's carry, not the station — its data is already in the system.
            if (carry_warm && !R.carry_cols.empty()) {
                try {
                    est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                        R.carry_cols, R.carry_dNrr, R.carry_dur);
                } catch (const std::exception& ex) {
                    log->warn("Network[phase-leveling]: station '{}' carry inject rejected "
                              "({}); proceeding without its carry this epoch", st.name, ex.what());
                }
            }
        }

        if (!epoch_ok || est.numShared() == 0) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ',' << n_clock << ',' << n_pb
                             << ',' << 0 << ",0,0,0,0," << total_pins << '\n';
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              emitEpochStatus("sync_skip", es); }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // --- Satellite code-bias datum constraints (W6, full multi-band) -----
        // Each shared per-(sat,signal) code bias opens gauge freedoms the data
        // cannot resolve; fold them here — BEFORE the null-space gate — as pure-
        // gauge rows so the reduced shared system keeps only the per-constellation
        // clock deficiency the explicit clock datum pins (declared_dimension
        // unchanged).  Generalised to ALL bands and tracking channels, the gauges:
        //   (1) per-satellite CLOCK-COMMON direction — a constant added to every
        //       one of a satellite's code biases is absorbed by its estimable
        //       clock.  Pinned by an ionosphere-free pin on the two lowest distinct-
        //       frequency signals (a1+a2=1), keeping the emitted OSB pure-DCB with
        //       its IF lump in the clock (else clock and OSB double-count it); and
        //   (2) per-(CONSTELLATION, SIGNAL) absolute level — each registered code
        //       signal's constellation-wide common mode is a gauge the GIM common-
        //       mode-free iono weighting leaves free.  Pinned by a sum-to-zero over
        //       the satellites carrying that signal — one row per (constellation,
        //       signal).  This adopts the standard IGS/AC OSB convention that every
        //       channel's per-constellation mean is zero (cross-checked against the
        //       COD .BIA: same-band inter-channel means are ~0 — exactly 0 for
        //       Galileo and GPS L2, <=0.14 m only on the handful of GPS L1C sats).
        //       The earlier two-band slice pinned only the lowest band, so every 3rd
        //       band (GPS L5, Galileo E5b/E6/E5) left an unpinned exact gauge that
        //       withheld the whole epoch.
        //       Why per-SIGNAL, not per-band: pinning one representative channel per
        //       band leaves a *non-co-tracked* same-band channel (e.g. Galileo C5Q
        //       vs C5X, seen by disjoint receivers so their inter-channel bias is
        //       unobservable) an unpinned gauge that withholds the epoch at network
        //       scale.  Per-signal pins every channel's level robustly.  For the
        //       minority of co-tracked same-band channels whose inter-channel mean IS
        //       observable, this also fixes that mean to the conventional zero (a
        //       sub-decimetre effect, per the .BIA cross-check); a connected-component
        //       datum that pins only genuinely-unobservable levels is future work.
        // These are pure gauge directions (no data information), so any positive
        // weight fixes them without biasing the estimable (between-satellite) DCB
        // structure the GIM makes observable.  Over-covering one level with both a
        // sum-to-zero and the IF pin is harmless: redundant positive-semidefinite
        // information cannot create a new near-zero direction (Weyl), so generous
        // coverage cannot trip the gate — only an UNpinned gauge can.  If the GIM
        // iono constraint is absent the between-satellite DCBs stay confounded with
        // the slant iono and the gate trips ("under-specified") — the safety net.
        if (config_.estimateNetworkSatCodeBias() && !registered_code_keys.empty()) {
            constexpr double kCodeBiasGaugeWeight = 1.0e3;  // » gate cutoff (1e-9·σmax), « σmax → no spectral inflation
            const Eigen::Index nshared = est.numShared();

            // Carrier frequency of a (sat,signal) from any present station's model.
            auto freqOf = [&](const std::string& sat, const std::string& sig) -> double {
                for (size_t i = 0; i < stations.size(); ++i)
                    if (present[i]) {
                        const double f = stations[i].mm->carrierFrequency(sig, sat);
                        if (std::isfinite(f) && f > 0.0) return f;
                    }
                return 0.0;
            };
            auto bandDigit = [](const std::string& sig) -> char {
                return sig.size() >= 2 ? sig[1] : '\0';   // RINEX band digit ("C1W" -> '1')
            };

            std::map<std::string, std::vector<std::string>> sat_signals;  // sat -> code signals
            std::vector<std::string> sat_order;
            for (const auto& kv : registered_code_keys) {
                auto& v = sat_signals[kv.first];
                if (v.empty()) sat_order.push_back(kv.first);
                v.push_back(kv.second);
            }

            std::vector<Eigen::VectorXd> rows;                 // length-nshared constraint Jacobians

            // (2) Per-(constellation, signal) sum-to-zero — one row per registered
            // signal, summed over the satellites that carry it (keyed
            // "<constellation>|<signal>").  Pins every channel's absolute level; see
            // the block header for why this is per-SIGNAL (robust to non-co-tracked
            // same-band channels at network scale) rather than per-band.
            std::map<std::string, Eigen::VectorXd> st0_row;
            for (const auto& kv : registered_code_keys) {
                Eigen::Index col;
                try { col = est.codeBiasColumn(kv.first, kv.second); } catch (...) { continue; }
                const std::string skey =
                    NetworkSbasis::constellationOf(kv.first) + "|" + kv.second;
                Eigen::VectorXd& srow = st0_row[skey];
                if (srow.size() == 0) srow = Eigen::VectorXd::Zero(nshared);
                srow(col) += 1.0;
            }

            // (1) Per-satellite ionosphere-free pin (clock-common gauge).
            for (const std::string& sat : sat_order) {
                std::vector<std::string>& sigs = sat_signals[sat];
                // Sort by RINEX band digit so the primary dual-frequency pair
                // (GPS L1/L2, Galileo E1/E5a) is the first two distinct bands.
                std::sort(sigs.begin(), sigs.end(),
                          [&](const std::string& a, const std::string& b) {
                              return bandDigit(a) < bandDigit(b);
                          });
                // IF pin: two lowest DISTINCT-frequency code signals → a1+a2=1.
                const double f1 = freqOf(sat, sigs.front());
                std::string sig2; double f2 = 0.0;
                for (size_t k = 1; k < sigs.size(); ++k) {
                    const double fk = freqOf(sat, sigs[k]);
                    if (fk > 0.0 && std::abs(fk - f1) > 1.0) { sig2 = sigs[k]; f2 = fk; break; }
                }
                Eigen::VectorXd row = Eigen::VectorXd::Zero(nshared);
                bool row_ok = false;
                if (f1 > 0.0 && f2 > 0.0) {
                    const double den = f1 * f1 - f2 * f2;
                    const double a1 =  f1 * f1 / den;     // ionosphere-free (clock-preserving) weights
                    const double a2 = -f2 * f2 / den;
                    try {
                        row(est.codeBiasColumn(sat, sigs.front())) = a1;
                        row(est.codeBiasColumn(sat, sig2))         = a2;
                        row_ok = true;
                    } catch (...) { row.setZero(); row_ok = false; }
                } else {
                    // Single distinct frequency (degenerate): pin the all-ones gauge
                    // so the satellite's code-bias common mode is still fixed.
                    for (const std::string& s : sigs)
                        try { row(est.codeBiasColumn(sat, s)) = 1.0; row_ok = true; } catch (...) {}
                }
                if (row_ok && row.cwiseAbs().sum() > 0.0) rows.push_back(std::move(row));
            }
            for (auto& kv : st0_row)
                if (kv.second.size() == nshared && kv.second.cwiseAbs().sum() > 0.0)
                    rows.push_back(std::move(kv.second));

            if (!rows.empty()) {
                Eigen::MatrixXd H(static_cast<Eigen::Index>(rows.size()), nshared);
                for (size_t r = 0; r < rows.size(); ++r)
                    H.row(static_cast<Eigen::Index>(r)) = rows[r];
                const Eigen::VectorXd z =
                    Eigen::VectorXd::Zero(static_cast<Eigen::Index>(rows.size()));
                est.addSharedConstraintRows(H, z, kCodeBiasGaugeWeight);
                if (within_epoch_armed) {
                    cap_H = H; cap_z = z;
                    cap_constraint_weight = kCodeBiasGaugeWeight;
                    cap_has_constraint = true;
                }
            }
            if (!spectrum_diag_done)   // one-shot (spectrum block sets the flag below)
                log->warn("Network[W6-cb]: nshared={}, n_cb={}, folded {} code-bias gauge "
                          "rows (IF pin for {} sats + sum-to-zero for {} (const,signal) levels)",
                          est.numShared(), n_cb, rows.size(), sat_order.size(), st0_row.size());
        }

        // --- Datum + null-space gate + solve --------------------------------
        NetworkSbasis sb;
        SbasisDeclaration decl;
        decl.reference_receiver_id = nc.clock_datum_station;
        decl.emit_code_bias = config_.estimateNetworkSatCodeBias();  // W6: self-describe the OSB datum
        if (clock_sum_to_zero) {
            // Anchor-free clock gauge: buildSharedDatum branches on this to emit
            // per-constellation sum-to-zero constraint rows (no pivot, empty datum).
            decl.isb_datum = SbasisDeclaration::IsbDatum::SumToZeroPerConstellation;
        } else if (!nc.clock_datum_satellite.empty()) {
            decl.clock_pivot_sat[NetworkSbasis::constellationOf(nc.clock_datum_satellite)]
                = nc.clock_datum_satellite;
        }

        NetworkSbasis::Plan plan;
        try {
            // RTA-1: in sticky mode the incumbent baseline is last_pivot, NOT
            // prev_pivot — prev_pivot is wholesale-replaced at each emit, so a
            // constellation absent from the last emitted epoch drops out of it,
            // and its return would spuriously re-select (and, with the
            // vanish-return arm off, SILENTLY re-reference the datum).
            // last_pivot is merge-updated at the same emit site, so per
            // constellation it holds the pivot of the last emitted product that
            // CONTAINED it — exactly the datum a consumer of that constellation
            // last adopted.  Both maps advance only at the emit site (P0-1), so
            // stickiness tracks the datum of the last EMITTED product: a
            // handover epoch that is subsequently withheld leaves the incumbent
            // unchanged, and if the incumbent returns before any new-datum epoch
            // emits, the datum simply never moves.
            plan = sb.buildSharedDatum(est, registered_clock_sats, decl,
                                       sticky_pivot ? &last_pivot : nullptr);
        } catch (const std::exception& ex) {
            log->warn("Network: buildSharedDatum failed ({}); skipping epoch", ex.what());
            ++n_skipped;
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              emitEpochStatus("datum_failed", es); }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // Sum-to-zero clock datum: fold the per-constellation Σδtˢ=0 gauge rows onto
        // the shared border BEFORE the null-space gate — anchor-free, so plan carries
        // constraint rows instead of hard pins and declared_dimension is 0.  Folded
        // exactly like the code-bias gauge (additive information; the within-epoch
        // re-solve re-applies them from plan, which is in scope there).  Empty (0
        // rows) in PIN mode → this is a no-op, byte-identical.
        if (plan.clock_constraint_H.rows() > 0)
            est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                        plan.clock_constraint_weight);

        // Pivot handover: the previous epoch's shared estimate lives in the OLD
        // datum, so a changed clock pivot invalidates the D2 carry-forward (and
        // forces a discontinuity bump downstream).
        // A1/A2: detect a clock-pivot datum re-reference and record WHICH
        // constellations changed (for the A2 per-constellation emit-bump scoping).
        // Arm (b) (vanish-return) is gated on network.pivot_vanish_return_signal;
        // when off this reproduces the legacy adjacent-epoch-only detection.
        std::set<char> changed_consts;
        const bool datum_changed = pivotHandoverSignalled(
            plan.clock_pivot, prev_pivot, last_pivot, pivot_vr_signal, changed_consts);
        // RTA-2: does THIS key's carried state belong to a retired datum?  With
        // network.scoped_handover_response off this is exactly datum_changed (the
        // legacy global response, byte-identical); when on, only keys of the
        // constellation(s) whose pivot changed — the SAME changed_consts set the
        // A2 emit bump consumes (single source of truth) — are invalidated.
        const auto handover_affects = [&](const std::string& sat) {
            return datum_changed
                && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
        };

        // --- SH-VTEC prior (Kaula + temporal), folded BEFORE the gate ----------
        // The SH coefficients are SHARED columns in the reduced normals; without a
        // bound on every one, a coefficient whose IPP support rotated over an
        // ocean/polar gap is near-null and trips the all-or-nothing rank gate
        // (design §6.1).  A degree-dependent Kaula prior (σ_n = kaula_scale/n; a₀₀
        // a LOOSE climatology prior so the mean VTEC stays data-determined and is
        // NOT clamped to an external value, §6.2) keeps every coefficient PSD-bounded
        // → declared_dimension stays = #constellations regardless of coverage.  The
        // SAME prior carries the field forward in time as a bounded random walk
        // (anchor the previous estimate, inflate by the per-degree q, but never
        // exceed the Kaula bound).  Folded via addSharedPrior BEFORE the gate (so the
        // gate sees the regularized SH block) — unlike the clock/bias D2 carry-
        // forward, which must stay AFTER the gate (it would otherwise fill the clock
        // gauge the datum pins).
        if (config_.estimateNetworkIonoSh() && sh_model && est.numShared() > 0) {
            std::map<std::tuple<int, int, char>, const CarryPrior*> sh_carry;
            // RTA-2 scope decision: the SH-VTEC carry skip deliberately stays
            // GLOBAL even under network.scoped_handover_response.  The SH
            // coefficients are constellation-LESS shared columns, so no scoped
            // subset exists for them; and although the pure clock-gauge
            // S-transform leaves the iono columns invariant in exact arithmetic,
            // the carried SH posterior was estimated JOINTLY with the old-datum
            // clock/bias states, so the conservative (existing) response is kept:
            // on ANY datum change the RW-toward-previous term is dropped for one
            // epoch and the coefficients fall back to the always-on Kaula
            // climatology anchor below (still PSD-bounded, gate-safe) — a cheap,
            // bounded cost that avoids scoping a column that belongs to no
            // constellation.
            if (!datum_changed)
                for (const CarryPrior& cp : prev_carry)
                    if (cp.kind == SharedParamKind::VtecSh)
                        sh_carry.emplace(std::make_tuple(cp.sh_n, cp.sh_m, cp.sh_trig), &cp);

            std::vector<NetworkEstimator::SharedPrior> sh_priors;
            sh_priors.reserve(sh_model->coeffs().size());
            for (const auto& c : sh_model->coeffs()) {
                const double kaula_sigma = (c.n == 0)
                    ? shc.mean_vtec_prior_sigma_tecu
                    : shc.kaula_scale_tecu / static_cast<double>(c.n);
                const double kaula_var = kaula_sigma * kaula_sigma;
                const double clim = (c.n == 0 && c.m == 0 && c.trig == 'C')
                    ? shc.mean_vtec_prior_tecu : 0.0;   // a₀₀ → mean VTEC; others → 0

                // Mean-reverting (first-order Gauss-Markov) prior: ALWAYS apply the
                // Kaula climatology anchor (toward clim, σ = kaula_sigma) AND, when a
                // posterior was carried forward, a random-walk anchor (toward the
                // previous estimate, inflated by the per-degree q).  Combining them in
                // information space pulls every coefficient back toward climatology
                // each epoch (so the field cannot random-walk away — the design's
                // "unobserved coefficients are pulled to their prior", §6.1) while the
                // RW term preserves temporal continuity for well-observed ones.  A
                // pure RW-toward-previous (no climatology re-assertion) let the global
                // field drift to unphysical magnitudes; this is the bound.
                double info  = 1.0 / kaula_var;       // climatology Tikhonov (always on)
                double sumiv = info * clim;
                const auto it = sh_carry.find(std::make_tuple(c.n, c.m, c.trig));
                if (it != sh_carry.end() && std::isfinite(it->second->value)
                    && it->second->var > 0.0) {
                    const double q_deg = (c.n == 0)
                        ? shc.q_per_degree
                        : shc.q_per_degree / static_cast<double>(c.n * c.n);
                    const double var_rw = it->second->var + q_deg * std::max(0.0, dt);
                    if (var_rw > 0.0) {
                        const double info_rw = 1.0 / var_rw;
                        info  += info_rw;
                        sumiv += info_rw * it->second->value;
                    }
                }
                if (!(info > 0.0)) continue;
                NetworkEstimator::SharedPrior p;
                p.kind = SharedParamKind::VtecSh;
                p.sh_n = c.n; p.sh_m = c.m; p.sh_trig = c.trig;
                p.value = sumiv / info; p.info = info;
                sh_priors.push_back(p);
            }
            est.addSharedPrior(sh_priors);
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), sh_priors.begin(), sh_priors.end());
        }

        // Relative eigenvalue tolerance for the null-space rank gate (see the gate
        // block below for the derivation of 1e-9); hoisted here so the prioritized
        // drop-and-heal uses the SAME cutoff as the gate it is protecting.
        constexpr double kNullSpaceTol = 1e-9;

        // --- Prioritized phase-bias datum: spectrum-based drop-and-heal helper ----
        // The prioritized union registers a phase-bias column for every (sat,signal)
        // that any two stations track.  Two things can leave such a column below the
        // rank-gate cutoff and would withhold the WHOLE epoch (every well-anchored
        // satellite lost to one bad column): (1) its ONLY ambiguity pin lived in an
        // anchor station that was DROPPED in the serial consume (a rank-deficient
        // local block) — a pure near-null orphan; (2) the joint W6 code-bias
        // estimation leaves a marginally-tracked satellite's phase bias weakly
        // separated from its clock — a near-null-but-real column.  This helper
        // identifies every near-null direction of the reduced shared normals spanned
        // DOMINANTLY by phase-bias columns (NOT the per-constellation clock gauges —
        // those have ~zero phase-bias weight) and soft-anchors exactly those columns
        // toward 0 (σ = 1 m: far above the cutoff so the residual gauge is resolved,
        // negligible against the 1e10 ambiguity pins so well-anchored columns are
        // untouched), returning the count healed.  A soft-anchored column is a
        // bounded, signalled degradation for one epoch (the emitter bias-jump disc
        // bumps when its proper ambiguity anchor returns).
        //
        // ★ PERF: this does a FULL eigenVECTOR decomposition of the reduced normals
        // (O(n_shared³) — ~1156³ at 305 stations), so it is invoked ONLY from the
        // gate-failure path below, never every epoch.  A healthy network passes the
        // cheap eigenvalue-only gate on the first try and never pays this cost (the
        // earlier unconditional placement made every 305 epoch grind on a needless
        // eigensolve).
        auto healNearNullPhaseBias = [&]() -> int {
            if (!phase_datum_prioritized || est.numShared() == 0 || shared_phase_keys.empty())
                return 0;
            const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
            Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Nss, Eigen::ComputeEigenvectors);
            if (es.info() != Eigen::Success) return 0;
            const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
            const double smax = ev.size() ? ev(ev.size() - 1) : 0.0;
            const double cutoff = kNullSpaceTol * std::max(smax, 1.0);
            std::map<int, std::pair<std::string, std::string>> pbcol;
            for (const auto& pk : shared_phase_keys) {
                const auto p = StateKeys::parseSatPhaseBiasKey(pk);
                if (p) pbcol[static_cast<int>(
                    est.phaseBiasColumn(p->satellite, p->signal))] = {p->satellite, p->signal};
            }
            // Every phase-bias column meaningfully spanning a near-null direction
            // (|component| > 0.3) is weakly/un-observable → heal it.  A clock-gauge
            // direction has ~zero phase-bias weight, so its minor PB participants
            // stay below 0.3 and are left alone.
            constexpr double kSpanFrac = 0.3;
            std::set<int> heal_cols;
            for (Eigen::Index j = 0; j < ev.size(); ++j) {
                if (ev(j) >= cutoff) continue;                 // observable direction
                const Eigen::VectorXd v = es.eigenvectors().col(j).cwiseAbs();
                for (const auto& [c, ss] : pbcol)
                    if (c < v.size() && v(c) > kSpanFrac) heal_cols.insert(c);
            }
            if (heal_cols.empty()) return 0;
            std::vector<NetworkEstimator::SharedPrior> hp;
            hp.reserve(heal_cols.size());
            for (int c : heal_cols) {
                NetworkEstimator::SharedPrior p;
                p.kind   = SharedParamKind::PhaseBias;
                p.sat    = pbcol[c].first;
                p.signal = pbcol[c].second;
                p.value  = 0.0;
                p.info   = 1.0;   // σ = 1 m
                hp.push_back(p);
            }
            est.addSharedPrior(hp);
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), hp.begin(), hp.end());
            return static_cast<int>(hp.size());
        };

        // Null-space rank gate.  The reduced shared normals are deliberately
        // ill-conditioned: the phase rows pin the satellite-clock / phase-bias
        // SUM to ~σ_phase precision (weight ~1e5), while CODE alone separates the
        // two (the clock-vs-bias split), so that split is orders of magnitude
        // weaker — yet still firmly non-zero.  Empirically the spectrum shows a
        // clean ~7-decade gap between the true gauge freedoms (the per-
        // constellation clock datum, σ ~ 1e-10 of σ_max) and the weakest REAL
        // direction (σ ~ 1e-2).  The default 1e-6 relative tolerance sits above
        // the weak-but-real directions and miscounts them as deficiencies; a
        // 1e-9 relative tolerance lands squarely in the gap, counting only the
        // genuine gauges while still catching a truly unpinned datum.
        // (kNullSpaceTol is declared just above the drop-and-heal block, which
        // shares this cutoff.)

        // ---- One-shot estimability diagnostic (default-OFF) ---------------
        // On the FIRST epoch that reaches the gate, optionally dump the tail of
        // the reduced shared-normal eigen-spectrum, so we can see WHERE the null
        // space sits relative to the declared datum dimension: a clean ~7-decade
        // gap at exactly declared_dim ⇒ the 1e-9 tolerance is mis-set; one or more
        // EXTRA tiny eigenvalues above the gap ⇒ a genuinely unpinned direction (a
        // missing datum constraint).
        //
        // The reduced shared normals are SYMMETRIC positive-SEMIdefinite (a Schur
        // complement of weighted Gram normals — see NetworkSbasis), so their
        // eigenvalues EQUAL their singular values: a self-adjoint eigensolve yields
        // the exact same spectrum the rank gate below tests, but at a small fraction
        // of a 1156×1156 JacobiSVD(ComputeFullV)'s cost — that one-shot SVD added
        // ~90 s to EVERY 305-station epoch-1.  Naming the gauge parameters still
        // needs the eigenVECTORs, so this stays a full decomposition and is gated
        // behind network.trace_estimability (default OFF); a production run pays
        // nothing.  The one-shot LATCH is always taken so the co-located W6-cb
        // one-shot log above still fires exactly once.  Eigenvalues come back
        // ASCENDING, so the largest is the LAST entry and the gauges are the FIRST.
        if (!spectrum_diag_done) {
            spectrum_diag_done = true;
            if (nc.trace_estimability) {
                const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
                Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(
                    Nss, Eigen::ComputeEigenvectors);
                if (es.info() == Eigen::Success) {
                    const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
                    const Eigen::Index k = ev.size();
                    const double smax = (k > 0) ? ev(k - 1) : 0.0;  // largest = last
                    const double cutoff = kNullSpaceTol * std::max(smax, 1.0);
                    int below = 0;
                    for (Eigen::Index i = 0; i < k; ++i) if (ev(i) < cutoff) ++below;
                    // Smallest ntail eigenvalues, printed largest-first so the very
                    // smallest (the gauge freedoms) sit at the end of the list.
                    const Eigen::Index ntail = std::min<Eigen::Index>(14, k);
                    std::string tail;
                    for (Eigen::Index i = ntail - 1; i >= 0; --i)
                        tail += fmt::format("{}{:.3e}", (i == ntail - 1 ? "" : ", "), ev(i));
                    log->warn("Network[estimability]: TOW {:.1f} reduced_N_ss {}x{}, "
                              "declared_dim={}, ev_max={:.3e}, cutoff={:.3e}, {} ev<cutoff; "
                              "smallest {} ev = [{}]",
                              se.time.tow, Nss.rows(), Nss.cols(), plan.declared_dimension,
                              smax, cutoff, below, ntail, tail);
                    // Name the parameters spanning each near-zero (unpinned-gauge)
                    // direction, printed largest-first to mirror the tail line above.
                    const Eigen::MatrixXd& V = es.eigenvectors();
                    const Eigen::Index ndump = std::min<Eigen::Index>(below + 2, k);
                    for (Eigen::Index j = ndump - 1; j >= 0; --j) {
                        const Eigen::VectorXd v = V.col(j).cwiseAbs();
                        std::vector<Eigen::Index> idx(v.size());
                        for (Eigen::Index t = 0; t < v.size(); ++t) idx[t] = t;
                        std::partial_sort(idx.begin(),
                                          idx.begin() + std::min<Eigen::Index>(5, v.size()), idx.end(),
                                          [&](Eigen::Index a, Eigen::Index b){ return v(a) > v(b); });
                        std::string top;
                        for (int t = 0; t < 5 && t < v.size(); ++t) {
                            const Eigen::Index c = idx[t];
                            top += fmt::format("{}{}={:.2f}", (t ? ", " : ""),
                                               (c < static_cast<Eigen::Index>(col_desc.size())
                                                && !col_desc[c].empty()) ? col_desc[c]
                                                                         : fmt::format("col{}", c),
                                               v(c));
                        }
                        log->warn("Network[estimability]:   ev={:.3e} dir spanned by [{}]", ev(j), top);
                    }
                }
            }
        }

        bool gate_ok = true;
        try {
            sb.assertNullSpaceRank(est.reducedSharedNormals(), plan.declared_dimension,
                                   kNullSpaceTol);
        } catch (const std::exception& ex) {
            // The reduced shared system carries MORE gauge freedoms than the
            // declared datum can pin — typically a satellite that is momentarily
            // unobservable (seen by a single station at marginal geometry), or —
            // under the prioritized datum — a phase-bias column whose anchor was
            // dropped / whose separation is weak.  For the prioritized case, try to
            // soft-anchor exactly those near-null phase-bias columns and RE-GATE
            // before withholding (the eigenVECTOR heal runs ONLY here, not every
            // epoch).  If the datum is still deficient after healing (or on the
            // legacy path), the product would be a meaningless least-norm split, so
            // WITHHOLD (the plan's "gate emission until convergence" safeguard).
            bool recovered = false;
            try {
                const int healed = healNearNullPhaseBias();   // 0 unless prioritized + near-null PB
                if (healed > 0) {
                    sb.assertNullSpaceRank(est.reducedSharedNormals(),
                                           plan.declared_dimension, kNullSpaceTol);
                    recovered = true;
                    ++n_healed_epochs;
                    n_healed_cols += static_cast<size_t>(healed);
                    log->debug("Network: TOW {:.1f} soft-anchored {} near-null phase-bias "
                               "column(s) (dropped anchor / weak separation) — gate recovered.",
                               se.time.tow, healed);
                }
            } catch (const std::exception& ex2) {
                // The heal (eigensolve / addSharedPrior) or the re-gate reported a
                // deficiency it could not resolve — withhold rather than crash.
                log->warn("Network: TOW {:.1f} drop-and-heal failed ({}); withholding.",
                          se.time.tow, ex2.what());
            }
            if (!recovered) {
                gate_ok = false;
                ++n_gate_withheld;
                log->debug("Network: observability gate at TOW {:.1f} withheld product ({})",
                           se.time.tow, ex.what());
                emitEvent("GATE", "gate_withheld", "", "", std::nullopt, std::nullopt,
                          "skip_epoch", ex.what());
            }
        }
        if (!gate_ok) {
            ++n_skipped;
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ',' << n_clock << ',' << n_pb
                             << ',' << plan.declared_dimension << ",0,0,0,0," << total_pins << '\n';
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              es.datum_changed = datum_changed ? 1 : 0;
              emitEpochStatus("gate_withheld", es); }
            // RTA-10: product-level heartbeat for a WITHHELD epoch — an H line with NO
            // E line, so a live tail can tell "gate withheld" from "producer died".
            // Only once the deferred product writer has opened (the first emit): earlier
            // withheld epochs precede the product file's existence by construction.
            // σ̂0 is unavailable pre-solve (0 per the H grammar); withheld="*" = whole epoch.
            if (emit_epoch_status && product_writer) {
                NetworkProducerStatus hst;
                hst.time.week = se.time.week;
                hst.time.tow  = se.time.tow;
                hst.state     = NetworkProducerStatus::State::WithheldGate;
                hst.withheld  = "*";
                product_writer->writeEpochStatus(hst);
                if (nc.product_flush_per_epoch) product_writer->flush();
            }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // Two-state clock model warmup gate (mirrors carry_warm): seed/predict only after the
        // cold-start convergence horizon.  Its per-epoch predictions (for the post-solve update
        // + normalized-innovation test) are stashed here.
        const bool clk_model_warm = clk_model_on && (static_cast<int>(n_emitted) >= pl_warmup);
        std::map<std::string, ClockOscPred> clk_pred;
        // B7 (P1-4): the carried clock-prior information folded onto each clock column this
        // epoch (0 = none) — either the scalar D2 carry (1/var_eff) or the two-state model
        // prior (1/p00).  The anchor-fraction diagnostic subtracts this from the reduced-normals
        // diagonal so it certifies DATA dominance, not "prior (carry) dominance" masquerading as
        // data.  Populated only when the diagnostic trace is open; inert otherwise.
        std::map<std::string, double> clk_prior_info;

        // --- Card 6 (RTF-2 / plan D3): ephemeris-cutover re-level ------------------------------
        // The producer's estimable per-sat clock is Δδtˢ = (true sat clock) − (broadcast
        // a-priori clock of the version this epoch's geometry uses).  When that broadcast
        // version rolls (old IODE → new IODE) the a-priori jumps by
        // Δ_eph(t) = c·(brdc_new(t) − brdc_old(t)); the TRUE clock is continuous, so the
        // estimable Δδtˢ must jump by −Δ_eph to keep representing the same satellite.  Left
        // alone, the carried clock prior — the TIGHT two-state clock_osc prediction, or the
        // D2 q_clock carry — is a full Δ_eph out of frame and FIGHTS the new-frame
        // measurements: a slow transient that also trips the two-state NIS / clock-monitor
        // jump test as a spurious per-sat fault.  So we deterministically re-base the CARRIED
        // clock state by −Δ_eph, and the solve runs entirely in the new frame — emitted C0 is
        // continuous-in-reconstruction (the consumer forms brdc(new_iode)+C0, Card 7) with NO
        // discontinuity bump.
        //
        // ORDERING (deliberate deviation from the plan's "after the solve, before the C1
        // pass"): the re-base MUST precede the prior injection (below) and the solve.  The
        // plan's post-solve placement anchored to where Card 5 STAMPS clock_iode (after
        // toSolutionEpoch); but by then x_s was already solved against the OLD-frame prior, so
        // a post-hoc x_s shift is exact ONLY when the prior dominates (tight) and OVER-corrects
        // a data-dominated (loose) solve.  Shifting the PRIOR is unconditionally exact for any
        // prior weight AND — being a deterministic function of the broadcast ephemeris alone —
        // is independent of the solve, so it applies identically on every disposition (emit /
        // σ̂0-withhold / solve-fail).  The IODE this epoch uses is read straight from the
        // shared LiveEphemerisStore (Card 5's own equivalence note: toe granularity ≫
        // intra-epoch light-travel, so the emit-site query lands on the version the emission
        // path used), so no dependence on the post-solve stamp remains.
        //
        // Placed after the observability-gate `continue`: a gate-withheld epoch leaves the
        // carried clock state untouched (its post-emit update never runs), so deferring the
        // re-level to the next non-gate-withheld epoch keeps relevel_iode and the carried
        // state in lock-step.  On the σ̂0/solve-fail paths the block DID run, the shift landed,
        // and the (skipped) post-emit updates leave it in place — no double-shift either way.
        if (brdc_rts) {
            auto& eph_store = LiveEphemerisStore::getInstance();
            for (const std::string& sat : registered_clock_sats) {
                if (sat.empty() || sat[0] == 'R') continue;          // GLONASS out of scope (plan D8)
                const auto now = eph_store.computeSatelliteClock(sat, se.time);
                if (!now) continue;                                  // no eph held → nothing to track
                const int new_iode = now->iode;
                auto bit = relevel_iode.find(sat);
                if (bit == relevel_iode.end()) {                     // first sighting: seed, no shift
                    relevel_iode[sat] = new_iode;
                    continue;
                }
                if (bit->second == new_iode) continue;               // no roll — the common case
                const int old_iode = bit->second;
                const auto shift = eph_store.ephCutoverShiftMetres(sat, se.time, old_iode);
                relevel_iode[sat] = new_iode;                        // baseline advances regardless
                if (shift) {
                    // Re-base the carried clock state by −Δ_eph.  Offset / level term ONLY —
                    // the drift (c1) and the 2×2 covariance are frame-invariant and untouched.
                    const double d = *shift;
                    if (auto oit = clock_osc.find(sat); oit != clock_osc.end() && oit->second.init)
                        oit->second.x0 -= d;                          // two-state offset (clock_model)
                    for (CarryPrior& cp : prev_carry)                 // D2 scalar clock carry (model off)
                        if (cp.kind == SharedParamKind::Clock && cp.sat == sat) cp.value -= d;
                    if (auto jit = prev_clk.find(sat); jit != prev_clk.end())
                        jit->second.first -= d;                       // clock-monitor jump baseline
                    log->info("Network (Card 6): {} eph cutover IODE {}->{} — re-leveled carried "
                              "clock by {:.4f} m (−Δ_eph); no discontinuity bump.",
                              sat, old_iode, new_iode, -d);
                    emitEvent("RELEVEL", "eph_cutover_relevel", sat, "", -d, std::nullopt,
                              "relevel", fmt::format("iode={}->{}", old_iode, new_iode));
                } else {
                    // Prior version no longer retained → Δ_eph incomputable → the clock stepped
                    // by an unknown amount.  Force a per-sat discontinuity (consumer resets this
                    // sat's network-corrected ambiguity across the step); it lands at the sat's
                    // next actual emit (below).  Effectively unreachable with per-epoch checking.
                    eph_cutover_disc_pending.insert(sat);
                    log->warn("Network (Card 6): {} eph cutover IODE {}->{} but the prior version "
                              "is no longer held — forcing a per-sat discontinuity (Δ_eph "
                              "incomputable).", sat, old_iode, new_iode);
                    emitEvent("RELEVEL", "eph_cutover_disc", sat, "", std::nullopt, std::nullopt,
                              "disc", fmt::format("iode={}->{}", old_iode, new_iode));
                }
            }
        }

        // --- D2 carry-forward prior (diagonal, inflated over the gap) -------
        // Applied AFTER the gate (which must see the DATA-only gauge structure)
        // and BEFORE the solve.  The prior anchors each non-datum shared
        // parameter to its previous-epoch estimate; it is skipped on a pivot
        // handover, when the carried values belong to a now-retired datum.
        // RTA-2 (network.scoped_handover_response): only the CHANGED
        // constellation(s)' datum retired — a per-constellation clock gauge is
        // independent of its siblings, so carries of unaffected constellations
        // remain valid and fold normally; the per-key skip below drops exactly
        // the retired-datum keys.  OFF reproduces the legacy all-or-nothing
        // skip byte-identically (the outer gate collapses to !datum_changed and
        // the per-key skip can never trigger inside it).
        if ((!datum_changed || scoped_handover) && !prev_carry.empty() && !fullcov_on) {
            std::vector<NetworkEstimator::SharedPrior> priors;
            priors.reserve(prev_carry.size());
            for (const CarryPrior& cp : prev_carry) {
                if (cp.kind == SharedParamKind::VtecSh) continue;  // SH prior is folded PRE-gate
                if (handover_affects(cp.sat)) continue;  // RTA-2: retired-datum key (scoped mode only)
                if (clk_model_on && cp.kind == SharedParamKind::Clock) continue;  // clock is on clock_osc
                const double q = (cp.kind == SharedParamKind::Clock)    ? q_clock
                               : (cp.kind == SharedParamKind::CodeBias) ? q_cb
                                                                        : q_pb;
                // A clock-monitor-flagged satellite folds with the large REINIT variance (a soft
                // large-q reinitialization): the weak prior still anchors the parameter at its
                // previous value — curbing the collective datum drift a hard refloat causes — but
                // is loose enough for the data to walk a frozen clock back toward truth.
                double var_eff = cp.inflate_q ? clk_mon_reinit_var
                                              : cp.var + q * std::max(0.0, dt);
                // B4: floor the D2 phase-bias / code-bias carry variance (caps Λ so a stiff
                // zero-q carry cannot lock a biased product in), mirroring the ambiguity-carry
                // floor.  Only on the non-refloat path (a clk-monitor refloat is deliberately
                // loose); clock is out of scope (own two-state model / monitor).  0 = off.
                if (!cp.inflate_q) {
                    const double floor = (cp.kind == SharedParamKind::PhaseBias) ? pb_var_floor
                                       : (cp.kind == SharedParamKind::CodeBias)  ? cb_var_floor
                                                                                 : 0.0;
                    if (floor > 0.0) var_eff = std::max(var_eff, floor);
                }
                if (!(var_eff > 0.0)) continue;
                NetworkEstimator::SharedPrior p;
                p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
                p.value = cp.value; p.info = 1.0 / var_eff;
                if (anchor_info_trace.is_open() && cp.kind == SharedParamKind::Clock)
                    clk_prior_info[cp.sat] = p.info;   // B7: scalar clock carry info on this column
                priors.push_back(p);
            }
            est.addSharedPrior(priors);  // skips params absent from this epoch's layout
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), priors.begin(), priors.end());
        } else if ((!datum_changed || scoped_handover) && !prev_carry.empty()) {
            // C1 (network.shared_carry_full_cov): fold the carried block as ONE dense
            // information prior Λ = (P_prev + Q)⁻¹ — the information-form KF time
            // update on the shared border, recovering the cross-covariances (most
            // critically clock↔phase-bias) the diagonal stash above discards.  Entry
            // selection, per-key skip rules, and the per-key var_eff surgery
            // (inflate_q reinit, q·dt inflation, B4 floors) are IDENTICAL to the
            // diagonal branch; the only new content is the retained off-diagonals.
            // Keys with no matrix row (first fold after a warm resume, or after a
            // stash-side non-finite screen) fold through the legacy diagonal path —
            // the mode degrades per-key, it never breaks.  Keys skipped by rule are
            // MARGINALIZED (their rows/cols of P are simply not selected), which is
            // the exact treatment for a departed/retired parameter.
            std::vector<NetworkEstimator::SharedPrior> dkeys;     // dense-eligible set
            std::vector<std::size_t> drow;                        // row in prev_carry_cov.P
            std::vector<double> dvar_eff;                         // per-key var_eff (diag + fallback)
            std::vector<char> dinflate;                           // inflate_q per dense key
            std::vector<NetworkEstimator::SharedPrior> leftovers; // no matrix row → diagonal fold
            dkeys.reserve(prev_carry.size());
            drow.reserve(prev_carry.size());
            dvar_eff.reserve(prev_carry.size());
            dinflate.reserve(prev_carry.size());
            for (const CarryPrior& cp : prev_carry) {
                if (cp.kind == SharedParamKind::VtecSh) continue;  // SH prior is folded PRE-gate
                if (handover_affects(cp.sat)) continue;  // RTA-2: retired-datum key (scoped mode only)
                if (clk_model_on && cp.kind == SharedParamKind::Clock) continue;  // clock is on clock_osc
                const double q = (cp.kind == SharedParamKind::Clock)    ? q_clock
                               : (cp.kind == SharedParamKind::CodeBias) ? q_cb
                                                                        : q_pb;
                double var_eff = cp.inflate_q ? clk_mon_reinit_var
                                              : cp.var + q * std::max(0.0, dt);
                if (!cp.inflate_q) {
                    const double floor = (cp.kind == SharedParamKind::PhaseBias) ? pb_var_floor
                                       : (cp.kind == SharedParamKind::CodeBias)  ? cb_var_floor
                                                                                 : 0.0;
                    if (floor > 0.0) var_eff = std::max(var_eff, floor);
                }
                if (!(var_eff > 0.0)) continue;
                NetworkEstimator::SharedPrior p;
                p.kind = cp.kind; p.sat = cp.sat; p.signal = cp.signal;
                p.value = cp.value; p.info = 1.0 / var_eff;  // used on the diagonal paths only
                const auto rit = prev_carry_cov.row.find(sharedCarryKey(cp.kind, cp.sat, cp.signal));
                if (rit == prev_carry_cov.row.end()) {
                    if (anchor_info_trace.is_open() && cp.kind == SharedParamKind::Clock)
                        clk_prior_info[cp.sat] = p.info;   // B7: diagonal-carry info (no dense row)
                    leftovers.push_back(p);
                    continue;
                }
                dkeys.push_back(p);
                drow.push_back(rit->second);
                dvar_eff.push_back(var_eff);
                dinflate.push_back(cp.inflate_q ? 1 : 0);
            }
            const std::size_t kd = dkeys.size();
            if (kd > 0) {
                // P_eff over the dense set: subset of the stashed posterior covariance;
                // diagonal := var_eff (the stashed diagonal equals cp.var by
                // construction, so this IS the legacy q·dt inflation + B4 floor); a
                // clock-monitor-flagged key is DECORRELATED before its diagonal becomes
                // the reinit variance (its old cross-covariances are stale once the
                // variance is replaced — and a swapped diagonal under live off-diagonals
                // could make P_eff indefinite).
                Eigen::MatrixXd cov_eff(kd, kd);
                for (std::size_t a = 0; a < kd; ++a)
                    for (std::size_t b = 0; b < kd; ++b)
                        cov_eff(a, b) = prev_carry_cov.P(drow[a], drow[b]);
                for (std::size_t a = 0; a < kd; ++a) {
                    if (dinflate[a]) {
                        cov_eff.row(static_cast<Eigen::Index>(a)).setZero();
                        cov_eff.col(static_cast<Eigen::Index>(a)).setZero();
                    }
                    cov_eff(a, a) = dvar_eff[a];
                }
                try {
                    Eigen::VectorXd lam;
                    est.addSharedPriorDense(dkeys, cov_eff,
                                            anchor_info_trace.is_open() ? &lam : nullptr);
                    if (anchor_info_trace.is_open())
                        for (std::size_t a = 0; a < kd; ++a)
                            if (dkeys[a].kind == SharedParamKind::Clock)
                                clk_prior_info[dkeys[a].sat] = lam(static_cast<Eigen::Index>(a));  // B7: folded Λ(c,c)
                    if (within_epoch_armed) {
                        cap_dense_keys = dkeys;      // replayed in resolve_dropping
                        cap_dense_cov  = cov_eff;
                    }
                } catch (const std::exception& e) {
                    // Numerical fallback: fold the SAME keys as independent diagonal
                    // priors with 1/var_eff — exactly the legacy semantics for this
                    // epoch.  Self-healing: the next emit re-stashes a fresh block.
                    log->warn("Network (C1): dense carried-covariance fold failed ({}) — "
                              "falling back to the diagonal carry for this epoch.",
                              e.what());
                    emitEvent("D2_DENSE", "fallback_diagonal", "", "",
                              static_cast<double>(kd), std::nullopt, "fallback",
                              e.what());   // non-PD / non-finite / conditioning budget — emitEvent sanitizes commas
                    for (std::size_t a = 0; a < kd; ++a) {
                        if (anchor_info_trace.is_open() &&
                            dkeys[a].kind == SharedParamKind::Clock)
                            clk_prior_info[dkeys[a].sat] = dkeys[a].info;
                        leftovers.push_back(dkeys[a]);
                    }
                }
            }
            est.addSharedPrior(leftovers);  // no-op when empty; skips absent keys
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), leftovers.begin(), leftovers.end());
        }

        // --- Two-state oscillator clock carry-forward prior (network.clock_model) ------------
        // The satellite clock's prior comes from the physical clock model: predict [offset,drift]
        // forward and fold the offset (mean = drift-following prediction, info = 1/predicted-var).
        // Because the prediction is tight (~cm), the post-solve innovation exposes a lock-in that
        // the scalar random walk would freeze.  Skipped on a pivot handover (retired datum) and
        // for the first warm epoch of each satellite (no prediction until it is seeded).
        // RTA-2 (network.scoped_handover_response): a handover retires only the changed
        // constellation(s)' datum, so under scoping their satellites alone skip the prediction
        // (they re-seed from this epoch's new-datum solve in the post-solve update); unaffected
        // constellations' oscillators predict normally.  OFF is byte-identical (outer gate
        // collapses to !datum_changed; the per-sat skip can never trigger inside it).
        if (clk_model_warm && (!datum_changed || scoped_handover)) {
            std::vector<NetworkEstimator::SharedPrior> clkp;
            clkp.reserve(clock_osc.size());
            for (const std::string& sat : registered_clock_sats) {
                if (handover_affects(sat)) continue;  // RTA-2: retired-datum sat (scoped mode only)
                auto it = clock_osc.find(sat);
                if (it == clock_osc.end() || !it->second.init) continue;
                // P0-3: predict over THIS satellite's true elapsed time, not the global
                // epoch dt.  A satellite absent for several epochs (eclipse exclusion is
                // ON in the clock-model config) would otherwise get an hours-old state
                // predicted over ONE dt of Q — a falsely-tight prior.  Past a gap bound
                // its carried state is stale: drop the prior entirely (reseed) so the
                // clock re-acquires from data + the a-priori anchor rather than trusting
                // an ancient value.  (Within the bound the Q·dt inflation over the true
                // gap naturally de-weights the prior — the honest, self-limiting behavior.)
                const double sat_dt = (it->second.t_last > 0.0)
                                          ? (pl_now_s - it->second.t_last) : std::max(0.0, dt);
                if (!(sat_dt >= 0.0) || sat_dt > clk_model_max_gap) {
                    // T5 (V2 blind spot): the stale-carry reseed is otherwise SILENT.  Naturally
                    // edge-triggered — the update reseeds t_last, so next epoch is fresh.
                    emitEvent("T5_clock_model", "stale_prior_reseed", sat, "", sat_dt,
                              clk_model_max_gap, "reseed", "carry past gap");
                    continue;   // R11: stale → no prior (reseed)
                }
                const ClockOscPred pr = clockOscPredict(it->second, sat_dt,
                                                        clk_model_q1, clk_model_q2);
                if (!(pr.p00 > 0.0) || !std::isfinite(pr.x0)) continue;
                clk_pred[sat] = pr;
                NetworkEstimator::SharedPrior p;
                p.kind = SharedParamKind::Clock; p.sat = sat; p.signal = "";
                p.value = pr.x0; p.info = 1.0 / pr.p00;
                if (anchor_info_trace.is_open()) clk_prior_info[sat] = p.info;  // B7: two-state clock-model prior info
                clkp.push_back(p);
            }
            est.addSharedPrior(clkp);
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), clkp.begin(), clkp.end());
        }

        // --- A-priori satellite-clock anchor (cold-start conditioning) ------
        // The shared clock column is Δδtˢ relative to the a-priori PRECISE product
        // clock, so it is genuinely small (sub-metre for precise post-processing).
        // With no absolute anchor the per-epoch joint solve floats it by hundreds
        // of metres at cold start to absorb the clock↔iono↔bias confusion (no
        // carry-forward exists yet at epoch 1, and epoch 2+ then anchors to that
        // floated value).  A Tikhonov prior pulling each Δδtˢ toward 0 with
        // σ = initial_std.sat_clock (the documented "Δδtˢ vs a-priori product clock"
        // quality: sub-m precise, looser for RT broadcast) regularizes ONLY the
        // ill-conditioned cold-start direction: the per-constellation clock DATUM
        // stays fixed by the explicit Datum (a large-weight pin that dominates this
        // soft prior), so the few-metre datum offset the data determines survives.
        // Applied POST-gate (like the carry-forward) so the null-space rank gate
        // still sees the clock datum as a gauge, and it combines in information space
        // with the carry-forward (anchor-toward-0 + RW-toward-previous).  Gated to a
        // BINDING std (< 100 m): the legacy loose default (1000 m) leaves the anchor
        // off, so every existing network config is byte-identical.
        //
        // SCOPE (measured, 305-sta GPS+Gal, 2026-06-30; see [[project_global_sh_iono]]):
        // σ=0.05 m collapses cold-start clock-Δ rms 56→2.9 m for ~6 epochs, then the
        // W6 clock↔bias↔ambiguity systematic (data wants ~80 m, info ≫ the anchor)
        // reasserts.  This improves cold-start CLOCK-PRODUCT quality only — the
        // co-estimated SH-VTEC field divergence is a SEPARATE, DECOUPLED cold-start
        // estimability defect (its large-scale level is unobservable under the
        // common-mode-free iono R); pinning the clock does NOT move the field.
        {
            const double clk_sigma = clk_anchor_sigma;  // m (D6-resolved; sp3 auto == initial_std.sat_clock)
            if (std::isfinite(clk_sigma) && clk_sigma > 0.0 && clk_sigma < 100.0) {
                const double clk_info = 1.0 / (clk_sigma * clk_sigma);
                std::vector<NetworkEstimator::SharedPrior> clk_anchor;
                clk_anchor.reserve(registered_clock_sats.size());
                for (const std::string& sat : registered_clock_sats) {
                    NetworkEstimator::SharedPrior p;
                    p.kind  = SharedParamKind::Clock;
                    p.sat   = sat;
                    p.value = 0.0;          // anchor Δδtˢ toward the a-priori clock
                    p.info  = clk_info;
                    clk_anchor.push_back(p);
                }
                est.addSharedPrior(clk_anchor);
                if (within_epoch_armed)
                    cap_priors.insert(cap_priors.end(), clk_anchor.begin(), clk_anchor.end());
            }
        }

        // A clock-pivot handover re-references the whole shared solution; the
        // forced bump steps BOTH phase and code discontinuity counters next
        // emit (the code per-(const,signal) sum-to-zero and IF-pin re-references
        // are detected by the emitter from the emitted epoch itself — see
        // NetworkProductEmitter; only this clock-pivot signal is driver-known).
        // Both the emitter datum bump (markDatumChange) AND the prev_pivot /
        // last_pivot baseline advance are DEFERRED to the emit site: the maps
        // must describe the datum of the last EMITTED product (the consumer's
        // view).  Advancing them here — before the solve — would make a handover
        // epoch that is subsequently skipped (solve failure, σ̂0 withhold)
        // compare equal next epoch: the discontinuity signal would be lost
        // forever AND the old-datum prev_carry/clock_osc would fold as priors
        // into the new-datum solve.  With the baseline frozen across skipped
        // epochs, every skip path re-detects the handover until one epoch
        // actually emits in the new datum.

        Eigen::VectorXd x_s;
        try {
            x_s = est.solve(plan.shared_datum);
        } catch (const std::exception& ex) {
            log->warn("Network: solve failed at TOW {:.1f} ({}); skipping epoch",
                      se.time.tow, ex.what());
            ++n_skipped;
            { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
              es.datum_changed = datum_changed ? 1 : 0;
              emitEpochStatus("solve_failed", es); }   // x_s empty — no clock stats
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // Satellites whose carried SHARED state (clock + coupled phase/code bias) is to be
        // REFLOATED at the end-of-epoch stash: the per-satellite clock monitor (below, the
        // detector for a lone lock-in the σ̂0 RAIM misses) plus — only when
        // clock_monitor_couple_raim is opted in — every satellite the within-epoch / post-fit
        // RAIM already flags (so a slip that leaked into the shared clock reinitializes the
        // clock too, not just the ambiguity).  Empty unless network.clock_monitor, so the stash
        // stays byte-identical when off.
        std::set<std::string> clock_refloat_sats;
        // B1 retro-bump / URA-inflate feed (P0-2): the union of satellites ANY QC tier
        // flags this epoch, independent of clock_monitor_couple_raim.  clock_refloat_sats
        // only captures the RAIM tiers under the couple opt-in (it drives the CARRY
        // refloat, a numeric action), so a separate accumulator is needed so the
        // retro-bump reaches every tier without forcing a carry refloat.  Populated only
        // when a URA feature that consumes it is on; inert (byte-identical) otherwise.
        std::set<std::string> retro_flag_sats;
        const bool retro_feed = ura_retro || ura_refloat_inflate > 1.0;

        // --- Within-epoch adaptation: drop the dominant culprit(s) and re-solve --------
        // When σ̂0 (the robust a-posteriori variance factor) shows the joint solve is
        // contaminated, peel the single most-prominent culprit — a whole SATELLITE (its
        // rows de-weighted network-wide) or a whole STATION (excluded) — re-solve THIS epoch
        // over a fresh beginEpoch() replay of the captured shared system, and repeat until
        // σ̂0 falls below the trigger or a cap.  Only the (rare, already-contaminated) trigger
        // epochs pay the re-solve; try/catch reverts to the last good solve on any failure.
        // Uses the same robust median-polish attribution as the RAIM refloat.
        int  n_adapt = 0;
        bool we_adapt_attempted = false;   // a within-epoch re-solve was launched (est layout was rebuilt)
        double sigma0_adapt = 0.0;         // A4: final within-epoch σ̂0 (0 = adaptation did not run this epoch)
        // RTA-10 (network.per_const_withhold): per-CONSTELLATION partition of the LAST
        // assess()'s standardized-residual pool — the very rows whose median is the global
        // σ̂0 the A4 withhold compares, keyed by the ambiguity's satellite constellation
        // (pa.sat[0]).  Cleared and refilled on every assess() so, like sigma0_adapt, it
        // describes the FINAL within-epoch solve.  Populated only when the carve-out is
        // armed (empty otherwise — no cost, no behavior change on the default path).
        std::map<char, std::vector<double>> we_sigma0_pool_c;
        // R12: the B8/T2 flag-rate (first-solve σ̂0 > trigger) is DECIDED here but COMMITTED at
        // the emit site, so a subsequently-withheld epoch is excluded from T2's denominator —
        // matching T3–T6, which only accrue on emitted epochs.  Without this, T2 counted withheld
        // (contaminated) epochs while the post-fit tiers did not, giving incomparable alphas.
        bool we_t2_opp = false, we_t2_flag = false;
        if (within_epoch_armed && carry_warm) {
            const double trig      = config_.networkRaimSigma0Trigger();
            const bool   surgical  = config_.networkRaimSurgicalStation();
            const double sta_bias  = surgical ? config_.networkRaimStationDropBias() : 1.0;
            constexpr int    kMaxAdapt   = 5;

            std::vector<char> orig_added(stations.size(), 0);
            for (size_t i = 0; i < stations.size(); ++i)
                orig_added[i] = (est_index_of[i] >= 0) ? 1 : 0;

            // Culprit sets.  A bad AMBIGUITY (cycle slip — the dominant contamination the
            // carried-phase RAIM sees) needs REINITIALIZATION, not measurement de-weighting: the
            // re-solve REFLOATS the culprit's ambiguities (withholds their stale carry prior) so
            // they re-estimate from full-weight data and absorb the slip locally.  A whole bad
            // STATION (a data-quality problem, not a slip) is EXCLUDED — the de-weight/exclude
            // tool, which is the right one for a bad measurement.
            std::set<std::string> drop_sats;                       // refloat that sat's ambiguities network-wide
            std::set<int>         drop_stations;                   // excluded entirely (bad-data case)
            std::map<int, std::set<std::string>> drop_arcs;        // (station → sats) refloat at that station ONLY

            // σ̂0 + median-polish effects of the CURRENT shared solution.  The (station × sat)
            // residual table (carried-ambiguity PHASE rows only) decomposes additively into a
            // per-satellite column effect, a per-station row effect, and an isolated-arc
            // interaction.  Computed over EVERY present ambiguity — a refloated ambiguity fits
            // its own data (its residual collapses after the re-solve), so σ̂0 stays an HONEST
            // measure of the REMAINING contamination without special-casing; the culprit is kept
            // out of the next pick by the candidate-set exclusion, not by hiding it here.
            auto assess = [&](const Eigen::VectorXd& xs, double& sigma0_out,
                              std::map<std::string, double>& sat_eff,
                              std::map<int, double>& sta_eff,
                              std::map<std::pair<int, std::string>, double>& arc_eff) {
                std::vector<double> r_pool;
                if (perconst_on) we_sigma0_pool_c.clear();   // RTA-10: re-partition per assess
                std::map<std::pair<int, std::string>, double> cell_max;
                for (size_t i = 0; i < stations.size(); ++i) {
                    // Prioritized: keep the former pivot in the σ̂0 pool — it now
                    // carries fault-testable non-anchored arcs; its anchored (pinned)
                    // keys are already absent from R.pl_amb (per-(sat,signal) partition).
                    if (!orig_added[i] || est_index_of[i] < 0
                        || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
                    const StationAssembly& R = asm_res[i];
                    if (R.pl_amb.empty() || R.A_s.cols() != xs.size()) continue;
                    Eigen::VectorXd x_r;
                    try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), xs); }
                    catch (...) { continue; }
                    if (x_r.size() != R.A_r.cols()) continue;
                    const Eigen::VectorXd resid = R.y - R.A_s * xs - R.A_r * x_r;
                    Eigen::VectorXd row_r;   // B2: per-row Baarda redundancy (empty when off)
                    if (raim_redun) {
                        try {
                            row_r = est.rowRedundancy(static_cast<std::size_t>(est_index_of[i]),
                                                      R.A_s, R.A_r, R.w, plan.shared_datum);
                        } catch (...) { row_r.resize(0); }
                    }
                    for (const auto& pa : R.pl_amb) {
                        if (pa.col < 0 || pa.col >= R.A_r.cols()) continue;
                        double r_max = 0.0;
                        for (Eigen::Index row = 0; row < R.A_r.rows(); ++row) {
                            if (std::abs(R.A_r(row, pa.col)) <= 0.5) continue;
                            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
                            double r = std::abs(resid(row)) * std::sqrt(std::max(0.0, w));
                            if (raim_redun && row < row_r.size()) {   // B2: standardize v/(σ·√r_i)
                                constexpr double kRedFloor = 1e-6;
                                const double ri = row_r(row);
                                if (ri <= kRedFloor) continue;   // uncheckable row: exclude from σ̂0 pool + r_max
                                r /= std::sqrt(ri);
                            }
                            r_pool.push_back(r);
                            // RTA-10: the SAME standardized row (weighting and B2 √r_i
                            // normalization included; redundancy-floored rows excluded by
                            // the continue above), partitioned by constellation.
                            if (perconst_on && !pa.sat.empty())
                                we_sigma0_pool_c[pa.sat[0]].push_back(r);
                            if (r > r_max) r_max = r;
                        }
                        const auto key = std::make_pair(static_cast<int>(i), pa.sat);
                        auto it = cell_max.find(key);
                        if (it == cell_max.end() || r_max > it->second) cell_max[key] = r_max;
                    }
                }
                sigma0_out = 1.0;
                if (!r_pool.empty()) {
                    std::nth_element(r_pool.begin(), r_pool.begin() + r_pool.size() / 2, r_pool.end());
                    sigma0_out = std::max(1.0, 1.4826 * r_pool[r_pool.size() / 2]);
                }
                std::vector<std::tuple<int, std::string, double>> tbl;
                tbl.reserve(cell_max.size());
                for (const auto& [k, v] : cell_max) tbl.emplace_back(k.first, k.second, v);
                const MedianPolish mp = medianPolish2Way(tbl);
                sat_eff = mp.col_eff;
                sta_eff = mp.row_eff;
                arc_eff.clear();
                for (std::size_t k = 0; k < tbl.size(); ++k)
                    arc_eff[std::make_pair(std::get<0>(tbl[k]), std::get<1>(tbl[k]))] = mp.interaction[k];
            };

            // Replay the epoch over a fresh arrowhead (beginEpoch reopens the frozen layout;
            // only reset() would clear it), REFLOATING the culprit ambiguities.  Rebuilds
            // est_index_of so reconstructStation / harvest stay valid on the final solve.  The
            // measurements stay at FULL weight — de-weighting is the tool for a bad measurement,
            // whereas a slipped ambiguity needs a new VALUE.  A satellite culprit refloats its
            // ambiguities at every station; an arc culprit at one station only; a whole-station
            // culprit is excluded (its data, not a single ambiguity, is bad).  A refloated
            // ambiguity keeps its full-weight data (so its local block stays positive-definite —
            // no drop-induced singularity) but WITHHOLDS its carry prior, so the re-solve lets it
            // find its new value and absorb the slip locally; the harvest then re-stashes that
            // corrected value (self-contained reinit — no reliance on the post-fit refloat).
            auto resolve_dropping = [&]() -> Eigen::VectorXd {
                est.beginEpoch();
                for (size_t i = 0; i < stations.size(); ++i) {
                    if (!orig_added[i] || drop_stations.count(static_cast<int>(i))) {
                        est_index_of[i] = -1;
                        continue;
                    }
                    const StationAssembly& R = asm_res[i];
                    // A_r columns of THIS station's ambiguities to refloat (withhold their prior).
                    // A SATELLITE culprit refloats its ambiguities at every station tracking it: a
                    // satellite-level contamination pulls the shared clock at ALL its stations, and
                    // its arcs are contaminated to varying degrees — a strict per-arc gate on the
                    // (inflated) σ̂0 would miss the moderately-slipped ones and leave a worse
                    // mixed state, so the whole satellite is reinitialized consistently.  An ARC
                    // culprit refloats at ONE station only (surgical, isolated slip).
                    std::set<Eigen::Index> refloat_cols;
                    const auto dai = drop_arcs.find(static_cast<int>(i));
                    for (const auto& pa : R.pl_amb)
                        if (drop_sats.count(pa.sat) ||
                            (dai != drop_arcs.end() && dai->second.count(pa.sat)))
                            refloat_cols.insert(pa.col);
                    est_index_of[i] = static_cast<int>(est.addStation(R.A_s, R.A_r, R.w, R.y));
                    // B9: re-inject the pivot's ZWD carry too (its R.carry_cols is empty
                    // unless network.carry_zwd), so the within-epoch re-solve matches the
                    // primary solve.  refloat_cols only ever names ambiguity columns, so a
                    // pivot's lone ZWD column is never in it (empty → the fast path).
                    if (carry_warm && !R.carry_cols.empty()) {
                        if (refloat_cols.empty()) {
                            est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                                R.carry_cols, R.carry_dNrr, R.carry_dur);
                        } else {
                            // Inject the carry prior for every carried ambiguity EXCEPT the
                            // refloated ones (those are left free → re-estimated from data).
                            std::vector<Eigen::Index> cc;
                            Eigen::VectorXd cdn(R.carry_cols.size()), cdu(R.carry_cols.size());
                            cc.reserve(R.carry_cols.size());
                            Eigen::Index m = 0;
                            for (std::size_t k = 0; k < R.carry_cols.size(); ++k)
                                if (!refloat_cols.count(R.carry_cols[k])) {
                                    cc.push_back(R.carry_cols[k]);
                                    cdn(m) = R.carry_dNrr(static_cast<Eigen::Index>(k));
                                    cdu(m) = R.carry_dur(static_cast<Eigen::Index>(k));
                                    ++m;
                                }
                            if (!cc.empty()) {
                                cdn.conservativeResize(m);
                                cdu.conservativeResize(m);
                                est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                                    cc, cdn, cdu);
                            }
                        }
                    }
                }
                if (cap_has_constraint) est.addSharedConstraintRows(cap_H, cap_z, cap_constraint_weight);
                // Re-apply the sum-to-zero clock gauge on the fresh arrowhead (plan is
                // in scope; its rows are layout-frozen for the epoch so they match the
                // primary solve).  No-op / byte-identical in PIN mode (0 rows).
                if (plan.clock_constraint_H.rows() > 0)
                    est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                                plan.clock_constraint_weight);
                if (!cap_priors.empty()) est.addSharedPrior(cap_priors);
                // C1: replay the dense D2 fold on the fresh arrowhead.  The layout is
                // frozen for the epoch, so the key→column mapping and the LDLT are
                // bit-reproducible — the re-solve sees the identical shared system.
                // Non-empty only when the primary dense fold succeeded (its fallback
                // routed through cap_priors above), so this cannot throw where the
                // primary did not.
                if (!cap_dense_keys.empty())
                    est.addSharedPriorDense(cap_dense_keys, cap_dense_cov);
                return est.solve(plan.shared_datum);
            };

            double sigma0 = 1.0;
            std::map<std::string, double> sat_eff;
            std::map<int, double> sta_eff;
            std::map<std::pair<int, std::string>, double> arc_eff;
            assess(x_s, sigma0, sat_eff, sta_eff, arc_eff);
            const double sigma0_first = sigma0;
            if (trace_flag_rates) { we_t2_opp = true; we_t2_flag = (sigma0_first > trig); }  // B8/T2 (R12: committed at emit, below)
            std::string adapt_detail;
            for (int iter = 0; iter < kMaxAdapt && sigma0 > trig; ++iter) {
                // Best NEW culprit per granularity (all gated on effect/σ̂0 > raim_k).  An
                // isolated ARC is the finest, least-destructive peel; a whole STATION is the
                // coarsest (it discards good arcs too) and is de-rated by sta_bias so it is
                // only chosen when it dominates the best satellite/arc by that margin.
                // Preference on equal score is ARC > SAT > STA (surgical-first tie-break).
                std::string best_arc_sat; int best_arc_sta = -1; double best_arc_eff = 0.0;
                if (surgical)
                    for (const auto& [key, eff] : arc_eff) {
                        const int s = key.first; const std::string& sat = key.second;
                        if (drop_stations.count(s) || drop_sats.count(sat)) continue;
                        const auto it = drop_arcs.find(s);
                        if (it != drop_arcs.end() && it->second.count(sat)) continue;
                        if (eff > best_arc_eff) { best_arc_eff = eff; best_arc_sta = s; best_arc_sat = sat; }
                    }
                std::string best_sat; double best_sat_eff = 0.0;
                for (const auto& [sat, eff] : sat_eff)
                    if (!drop_sats.count(sat) && eff > best_sat_eff) { best_sat_eff = eff; best_sat = sat; }
                int best_sta = -1; double best_sta_eff = 0.0;
                for (const auto& [sta, eff] : sta_eff)
                    if (!drop_stations.count(sta) && eff > best_sta_eff) { best_sta_eff = eff; best_sta = sta; }

                // Consensus escalation: a satellite with ≥ raim_min_sta arcs currently over the
                // gate is a NETWORK-WIDE problem (its shared clock/phase-bias is corrupted), not a
                // stack of isolated arcs — reinitialize the whole satellite in ONE iteration (one
                // re-solve) rather than peeling arc by arc.  When the best arc belongs to such a
                // satellite, redirect to the satellite branch.  Reuses the RAIM cross-station
                // consensus count (§ post-fit RAIM).
                if (surgical && best_arc_sta >= 0 && !drop_sats.count(best_arc_sat)) {
                    int n_bad = 0;
                    for (const auto& [key, eff] : arc_eff) {
                        if (key.second != best_arc_sat || drop_stations.count(key.first)) continue;
                        const auto it = drop_arcs.find(key.first);
                        if (it != drop_arcs.end() && it->second.count(key.second)) continue;
                        if (eff / sigma0 > raim_k) ++n_bad;
                    }
                    if (n_bad >= raim_min_sta) {
                        if (best_arc_eff > best_sat_eff) { best_sat = best_arc_sat; best_sat_eff = best_arc_eff; }
                        best_arc_sta = -1;   // drop the whole satellite instead of this one arc
                    }
                }

                enum class Kind { None, Arc, Sat, Sta } kind = Kind::None;
                double best_score = -1.0;   // strict '>' below → earlier (more surgical) wins ties
                if (best_arc_sta >= 0 && best_arc_eff / sigma0 > raim_k && best_arc_eff > best_score) {
                    best_score = best_arc_eff; kind = Kind::Arc;
                }
                if (!best_sat.empty() && best_sat_eff / sigma0 > raim_k && best_sat_eff > best_score) {
                    best_score = best_sat_eff; kind = Kind::Sat;
                }
                if (best_sta >= 0 && best_sta_eff / sigma0 > raim_k &&
                    (best_sta_eff / sta_bias) > best_score) {
                    best_score = best_sta_eff / sta_bias; kind = Kind::Sta;
                }
                if (kind == Kind::None) break;  // nothing bad enough left to drop

                if (kind == Kind::Arc) {
                    drop_arcs[best_arc_sta].insert(best_arc_sat);
                    adapt_detail += fmt::format("{}arc:{}/{}", adapt_detail.empty() ? "" : ",",
                                                stations[best_arc_sta].name, best_arc_sat);
                } else if (kind == Kind::Sat) {
                    drop_sats.insert(best_sat);
                    adapt_detail += fmt::format("{}sat:{}", adapt_detail.empty() ? "" : ",", best_sat);
                } else {
                    drop_stations.insert(best_sta);
                    adapt_detail += fmt::format("{}sta:{}", adapt_detail.empty() ? "" : ",",
                                                stations[best_sta].name);
                }

                Eigen::VectorXd x_try;
                we_adapt_attempted = true;
                try { x_try = resolve_dropping(); }
                catch (const std::exception& ex) {
                    // Should be unreachable now (the per-station full-weight retry uses the
                    // known-PD original assembly).  Last resort: restore the clean no-drop solve
                    // so the emit / harvest run on a consistent estimator rather than a
                    // half-rebuilt one (the old "keep prior solve" left est mid-rebuild).
                    log->warn("Network[RAIM within-epoch]: re-solve failed ({}); restoring clean solve.",
                              ex.what());
                    drop_sats.clear(); drop_stations.clear(); drop_arcs.clear();
                    try { x_s = resolve_dropping(); } catch (...) {}
                    break;
                }
                x_s = x_try;
                ++n_adapt;
                assess(x_s, sigma0, sat_eff, sta_eff, arc_eff);
            }
            if (n_adapt > 0) {
                log->info("Network[RAIM within-epoch]: TOW {:.1f} re-conditioned in {} re-solve(s) — "
                          "refloated {} sat / {} arc, {} sta excluded [{}]; σ̂0 {:.2f} → {:.2f} (trigger {:.2f}).",
                          se.time.tow, n_adapt, drop_sats.size(),
                          [&] { size_t n = 0; for (const auto& kv : drop_arcs) n += kv.second.size(); return n; }(),
                          drop_stations.size(), adapt_detail, sigma0_first, sigma0, trig);
                // T2: one event per settled culprit (whole-sat drop, per-(sta,sat) arc drop,
                // excluded station).  statistic=final σ̂0, threshold=trigger, detail=first->final.
                if (events_trace.is_open()) {
                    const std::string span = fmt::format("{:.2f}->{:.2f}", sigma0_first, sigma0);
                    for (const auto& s : drop_sats)
                        emitEvent("T2_within_epoch", "we_refloat_sat", s, "", sigma0, trig,
                                  "refloat", span);
                    for (const auto& kv : drop_arcs)
                        for (const auto& s : kv.second)
                            emitEvent("T2_within_epoch", "we_refloat_arc", s,
                                      stations[kv.first].name, sigma0, trig, "refloat", span);
                    for (int sta : drop_stations)
                        emitEvent("T2_within_epoch", "we_exclude_station", "",
                                  stations[sta].name, sigma0, trig, "exclude", span);
                }
            }

            // A within-epoch satellite refloat reinitialized that sat's AMBIGUITY at every
            // station; its coupled shared clock is just as suspect, so refloat the carried
            // clock too (the stash below skips it).  Opt-in (clock_monitor_couple_raim).
            if (clk_mon_couple)
                for (const auto& s : drop_sats) clock_refloat_sats.insert(s);
            // P0-2: a within-epoch whole-satellite drop contaminated that sat's emitted
            // correction — feed the retro-bump regardless of the (numeric) couple opt-in.
            if (retro_feed)
                for (const auto& s : drop_sats) retro_flag_sats.insert(s);

            sigma0_adapt = sigma0;   // A4: publish the final within-epoch σ̂0 for the withhold gate
        }

        // Reduce-once invariant (design §3.5): the carried prior is injected
        // pre-gate, so the single gate reduce() must be the ONLY per-station Schur
        // recompute this epoch — UNLESS the within-epoch adaptation deliberately re-solved
        // (each drop→re-solve is another reduce()).  A second recompute otherwise means a
        // stray post-gate local-block change silently doubled the serial path.
        if (pl_on && !we_adapt_attempted && est.reduceRecomputeCount() != 1) {
            log->error("Network[phase-leveling]: reduce() recomputed {} times this epoch "
                       "(expected exactly 1) — a stray post-gate local-block change doubled "
                       "the serial Schur path.", est.reduceRecomputeCount());
        }

        // --- Within-epoch escape-hatch: withhold rather than emit a still-contaminated epoch
        // The capped adaptation (drop culprit → re-solve, kMaxAdapt passes) may not fully
        // isolate the fault; if σ̂0 STILL exceeds the withhold ceiling the shared solution
        // is too corrupted to publish.  Reuse the observability-gate disposition (no emit,
        // no carry stash, epoch skipped — the contaminated solve never enters the store or
        // the carry) instead of emitting it with only a log warning.  OFF (raim_withhold ≤ 0)
        // is byte-identical: the epoch emits exactly as today.
        //
        // P0-4: gate on σ̂0 ALONE, NOT on we_adapt_attempted.  A DIFFUSE contamination
        // that leaves NO single dominant culprit (kind == Kind::None) never launches a
        // re-solve, so we_adapt_attempted stays false — yet σ̂0 is high, and that is
        // exactly the case the withhold ceiling should catch.  sigma0_adapt is set to the
        // assessed σ̂0 whenever the within-epoch RAIM block runs (whether or not a culprit
        // was dropped) and stays 0 otherwise, so a clean/OFF epoch (sigma0_adapt == 0)
        // still can never trip it.
        // RTA-10 (network.per_const_withhold): per-CONSTELLATION carve-out of the σ̂0
        // withhold.  When the ceiling trips, judge each constellation by its OWN σ̂0_c —
        //
        //   σ̂0_c = max(1, 1.4826 · median{ r_i : i ∈ constellation c's rows })
        //
        // over EXACTLY the rows the global σ̂0 pools (carried-ambiguity PHASE rows of
        // every present non-pivot station, r_i = |v_i|·√w_i, divided by √(redundancy r̄_i)
        // under B2 with its uncheckable-row exclusion), partitioned by the ambiguity's
        // satellite constellation.  Same statistic, same ceiling, restricted support.
        //
        // APPROXIMATION (stated, not hidden): the residuals are evaluated at the JOINT
        // solution — a contamination in constellation A perturbs shared per-station
        // nuisances (receiver clock, ZWD, iono) that leak into B's residuals, and each
        // row's B2 redundancy is its share of the JOINT system's redundancy, not of a
        // per-constellation solve.  σ̂0_c therefore under-separates cross-constellation
        // coupling; the "every constellation individually clean while the global σ̂0
        // tripped" outcome below is the honest edge of that approximation and is
        // published-with-warning + counted (n_perconst_diffuse_published), not silently
        // absorbed.  A constellation also needs >= per_const_withhold_min_rows rows to
        // be JUDGEABLE — fewer cannot certify cleanliness under a contaminated epoch.
        //
        // The RANK/OBSERVABILITY gate above keeps whole-epoch semantics untouched: a
        // rank deficiency means the declared datum itself is invalid — the product
        // would be a meaningless least-norm split, so no "healthy subset" exists to
        // carve out.  This carve-out applies ONLY to the σ̂0 (contamination) withhold.
        bool           perconst_active = false;   // a publishable subset exists this epoch
        std::set<char> perconst_publish;          // constellations to keep in the emission
        std::set<char> perconst_dec_withhold;     // constellations the decision withheld
        std::map<char, NetworkEstimator::PerConstSigma0> perconst_stats;  // σ̂0_c + rows (events/log)
        if (perconst_on && raim_withhold > 0.0 && sigma0_adapt > raim_withhold) {
            for (auto& [cst, pool] : we_sigma0_pool_c) {
                NetworkEstimator::PerConstSigma0 st;
                st.rows = static_cast<int>(pool.size());
                if (!pool.empty()) {   // same median × 1.4826, floor 1 as the global σ̂0
                    std::nth_element(pool.begin(), pool.begin() + pool.size() / 2, pool.end());
                    st.sigma0_c = std::max(1.0, 1.4826 * pool[pool.size() / 2]);
                }
                perconst_stats[cst] = st;
            }
            const NetworkEstimator::PerConstWithholdDecision dec =
                NetworkEstimator::decidePerConstWithhold(perconst_stats, raim_withhold,
                                                         perconst_min_rows);
            if (!dec.publish.empty()) {
                perconst_active       = true;
                perconst_publish      = dec.publish;
                perconst_dec_withhold = dec.withhold;
                std::string detail;
                for (const auto& [cst, st] : perconst_stats)
                    detail += fmt::format("{}{}:{:.2f}/{}{}", detail.empty() ? "" : " ", cst,
                                          st.sigma0_c, st.rows,
                                          dec.publish.count(cst) ? "" : "(withheld)");
                log->info("Network (RTA-10): TOW {:.1f} global σ̂0={:.2f} > ceiling {:.2f} — "
                          "per-constellation carve-out publishes {} of {} judged "
                          "constellation(s) [σ̂0_c/rows: {}].",
                          se.time.tow, sigma0_adapt, raim_withhold, dec.publish.size(),
                          perconst_stats.size(), detail);
            }
            // dec.publish empty ⇒ NO constellation is individually publishable — fall
            // through to the whole-epoch withhold below, exactly as without the toggle.
        }
        if (raim_withhold > 0.0 && sigma0_adapt > raim_withhold && !perconst_active) {
            log->warn("Network[RAIM within-epoch]: TOW {:.1f} σ̂0={:.2f} > withhold ceiling {:.2f} "
                      "after {} re-solve(s) — WITHHOLDING epoch (too contaminated to publish).",
                      se.time.tow, sigma0_adapt, raim_withhold, n_adapt);
            emitEvent("SIGMA0", "withhold", "", "", sigma0_adapt, raim_withhold,
                      "skip_epoch", fmt::format("{} re-solve(s)", n_adapt));
            ++n_skipped;
            ++n_sigma0_withheld;   // R8: dedicated count of σ̂0-ceiling withholds (reported in the summary)
            if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                             << ',' << n_present << ',' << n_clock << ',' << n_pb
                             << ',' << plan.declared_dimension << ",1,0,0,0," << total_pins << '\n';
            { EpochStatusRow es; es.sigma0 = sigma0_adapt; es.n_adapt = n_adapt;
              es.n_present = n_present; es.n_clock_sats = n_clock;
              es.datum_changed = datum_changed ? 1 : 0;
              emitEpochStatus("sigma0_withheld", es); }
            // RTA-10: product-level heartbeat for the WITHHELD epoch (H line, no E line)
            // — a live tail can tell "σ̂0 withheld" from "producer died".  Only once the
            // deferred product writer has opened (the first emit); earlier withheld
            // epochs precede the product file itself.  withheld="*" = whole epoch.
            if (emit_epoch_status && product_writer) {
                NetworkProducerStatus hst;
                hst.time.week = se.time.week;
                hst.time.tow  = se.time.tow;
                hst.state     = NetworkProducerStatus::State::WithheldSigma0;
                hst.sigma0    = sigma0_adapt;
                hst.withheld  = "*";
                product_writer->writeEpochStatus(hst);
                if (nc.product_flush_per_epoch) product_writer->flush();
            }
            prev_week = se.time.week; prev_tow = se.time.tow;
            continue;
        }

        // --- Emit -----------------------------------------------------------
        NetworkSolutionEpoch sol = est.toSolutionEpoch(x_s, se.time, iod++);
        // Card 5 (RTF-2): stamp the broadcast-ephemeris VERSION the producer's clock-C0
        // baseline used, per emitted satellite, so the consumer applies C0 against the
        // SAME broadcast clock (getSatelliteClockCorrection pinned to clock_iode, Card 7)
        // and the eph-cutover re-level (Card 6) can detect a version roll.  The broadcast
        // clock is station-independent (one shared LiveEphemerisStore), so the version the
        // per-station emission path (computeEmissionState) selected this epoch is exactly
        // what computeSatelliteClock reports here — toe granularity (hours) dwarfs the
        // intra-epoch light-travel spread, so pick() lands on the same record.  Querying
        // the store directly at the emit site is a deliberate, equivalent simplification of
        // the plan's "thread the IODE out of the emission path" (no per-station out-param
        // plumbing needed).  GLONASS ('R') stays -1 (plan D8).  sp3 mode: the store is empty
        // ⇒ nullopt ⇒ clock_iode stays -1 everywhere ⇒ Card-4 writes no iode= token ⇒
        // byte-identical.
        if (brdc_rts) {
            auto& eph_store = LiveEphemerisStore::getInstance();
            for (auto& s : sol.sats) {
                if (!s.has_clock || s.sat.empty() || s.sat[0] == 'R') continue;
                if (auto bc = eph_store.computeSatelliteClock(s.sat, se.time))
                    s.clock_iode = bc->iode;
            }
        }
        // RT0-4: stamp the wall-clock TIME-OF-GENERATION (distinct from the content
        // epoch se.time) so a consumer can measure product latency.  Only when armed
        // — otherwise generation_time stays {0,0} and no G record is serialized
        // (byte-identical).  Non-deterministic (wall clock), hence gated.
        if (emit_gen_time) sol.generation_time = TimeUtils::nowGpsTime();
        // RTA-3: stamp the per-run solution-generation id, minted once from the
        // FIRST EMITTED epoch's GPS time (week·604800 + ⌊tow⌋).  Deterministic under
        // file replay (the id is serialized per epoch as an I record and comes back
        // out of the file); a restarted live producer first-emits at a later epoch
        // and therefore mints a DIFFERENT id — the consumer treats the change as a
        // global datum break.  0 is reserved for "unset", so an armed run clamps to
        // 1 in the (never-in-practice) week-0 corner.  OFF ⇒ sol.generation_id stays
        // 0 ⇒ no I record, store generation unset (byte-identical).
        // RTB-7 seam: an RTB-7(a) WARM RESUME restores the previous run's non-zero
        // id BEFORE the epoch loop, so the generation_id == 0 guard below suppresses
        // this mint — re-minting on a warm resume would signal a global datum break
        // and force a spurious consumer-wide hard reset.  Only a genuine cold start
        // (no/stale/mismatched snapshot) reaches the mint.
        if (emit_gen_id) {
            if (generation_id == 0) {
                generation_id = static_cast<uint32_t>(se.time.week) * 604800u
                              + static_cast<uint32_t>(se.time.tow);
                if (generation_id == 0) generation_id = 1;
                log->info("Network (RTA-3): solution-generation id {} minted from the first "
                          "emitted epoch (week {}, tow {:.1f}).",
                          generation_id, se.time.week, se.time.tow);
            }
            sol.generation_id = generation_id;
        }
        // B3b: publish the two-state clock-model drift as C1 (consumer extrapolates
        // c0 + c1·dt).  clock_osc[sat].x1 is the drift carried INTO this epoch (from the
        // previous epoch's posterior sequence — the correct extrapolation basis; the
        // clock-model update for THIS epoch runs after the emit).  0 when clock_model off.
        if (clk_model_on)
            for (auto& s : sol.sats) {
                const auto oit = clock_osc.find(s.sat);
                if (oit == clock_osc.end() || !oit->second.init) continue;
                // R6: a satellite that vanished (eclipse) and returns still carries its PRE-GAP
                // drift here — the clock-model update that reseeds x1 to 0 runs after this emit
                // — so shipping oit->second.x1 unconditionally would emit an hours-old rate as C1
                // for one epoch.  Gate on the carried state's freshness against the SAME staleness
                // bound the prediction / drift finite-difference use (R11): past the gap the drift
                // is not trustworthy → leave c1 at 0 (ZOH), matching the reseed the update will do.
                const double sat_dt = (oit->second.t_last > 0.0) ? (pl_now_s - oit->second.t_last)
                                                                 : 0.0;
                if (sat_dt >= 0.0 && sat_dt <= clk_model_max_gap) {
                    s.clock_c1_m_s = oit->second.x1;
                    c1_zoh_active.erase(s.sat);
                } else {
                    // T5/R6 (V2 blind spot): a vanish-return past the gap ships c1=0 (ZOH) for one
                    // epoch instead of an hours-old rate.  Edge-triggered (c1_zoh_active) so a
                    // persistently-stale sat logs ONCE, not every emit.
                    if (events_trace.is_open() && c1_zoh_active.insert(s.sat).second)
                        emitEvent("T5_clock_model", "c1_zoh_suppress", s.sat, "", sat_dt,
                                  clk_model_max_gap, "c1_zero", "vanish_return");
                }
            }
        // RTA-10: constellations withheld from THIS epoch's emission — filled by the
        // RT0-5 runaway quarantine and/or the per-constellation σ̂0 carve-out below;
        // feeds the H heartbeat's status (ok vs partial) and its withheld field.
        std::set<char> epoch_withheld_consts;
        // RT0-5: per-constellation clock-runaway quarantine.  Group this epoch's clock
        // solutions by constellation; if a constellation's median |clock − global median|
        // exceeds the magnitude ceiling (or median |drift| exceeds the rate ceiling), DROP
        // that constellation's satellites from the emission and flag it — a runaway (e.g.
        // the open BeiDou clock divergence) then degrades SCOPE, not the whole product.
        // OFF ⇒ sol untouched (byte-identical).  Placed AFTER the C1 drift-stamp pass so
        // the rate ceiling sees this epoch's clock_c1_m_s (0 when clock_model off ⇒ only
        // the magnitude ceiling applies), and BEFORE the URA pass so a withheld
        // constellation costs no URA work.
        //
        // HAZARD (documented, not defended): the magnitude test is referenced to the
        // GLOBAL median across ALL clock sats.  If the runaway constellation supplies
        // half or more of the epoch's clock satellites, the global median lands inside
        // the runaway cluster and the comparison INVERTS — the healthy minority trips
        // the ceiling and is quarantined while the runaway constellation passes.  The
        // check is sized for its intended regime (a minority constellation, e.g. the
        // open BeiDou divergence, walking off by ~km against a multi-constellation
        // majority); do not arm it in a run dominated by the suspect constellation.
        // The single-constellation degenerate case is warned at config load.
        if (clk_runaway_on && !sol.sats.empty()) {
            std::vector<double> all_c;
            for (const auto& s : sol.sats) if (s.has_clock) all_c.push_back(s.clock_c0_m);
            std::set<char> runaway;
            if (!all_c.empty()) {
                std::nth_element(all_c.begin(), all_c.begin() + all_c.size() / 2, all_c.end());
                const double gmed = all_c[all_c.size() / 2];   // common-mode datum
                std::map<char, std::vector<double>> dev, rate;
                for (const auto& s : sol.sats) {
                    if (s.sat.empty() || !s.has_clock) continue;
                    dev[s.sat[0]].push_back(std::abs(s.clock_c0_m - gmed));
                    if (s.clock_c1_m_s != 0.0) rate[s.sat[0]].push_back(std::abs(s.clock_c1_m_s));
                }
                auto medianOf = [](std::vector<double>& v) {
                    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                    return v[v.size() / 2];
                };
                for (auto& [c, d] : dev) {
                    const double dm = medianOf(d);
                    double       rm = 0.0;
                    bool         bad = dm > clk_runaway_mag;
                    if (!bad) {
                        const auto rit = rate.find(c);
                        if (rit != rate.end() && !rit->second.empty()) {
                            rm  = medianOf(rit->second);
                            bad = rm > clk_runaway_rate;
                        }
                    }
                    if (bad) {
                        runaway.insert(c);
                        const auto n_drop = std::count_if(sol.sats.begin(), sol.sats.end(),
                            [c](const NetworkSatSolution& s){ return !s.sat.empty() && s.sat[0] == c; });
                        log->warn("Network (RT0-5): constellation '{}' clock runaway — median |Δclk|="
                                  "{:.1f} m (ceil {:.0f}), median |drift|={:.3f} m/s (ceil {:.2f}); "
                                  "WITHHOLDING its {} sats this epoch.",
                                  c, dm, clk_runaway_mag, rm, clk_runaway_rate, n_drop);
                        emitEvent("RT0-5_runaway", "constellation_withhold", std::string(1, c), "",
                                  dm, clk_runaway_mag, "drop_const", fmt::format("drift={:.3f}", rm));
                    }
                }
            }
            if (!runaway.empty()) {
                std::vector<NetworkSatSolution> kept;
                kept.reserve(sol.sats.size());
                for (auto& s : sol.sats)
                    if (s.sat.empty() || runaway.count(s.sat[0]) == 0) kept.push_back(std::move(s));
                sol.sats.swap(kept);
                ++n_runaway_epochs;
                epoch_withheld_consts.insert(runaway.begin(), runaway.end());  // RTA-10: H heartbeat
            }
        }
        // RTA-10 (network.per_const_withhold): apply the per-constellation σ̂0 carve-out
        // decided at the withhold site above — prune the non-publishable constellations'
        // satellites from the emission EXACTLY the way the RT0-5 runaway quarantine does
        // (adjacent code, same swap-prune pattern).  Placed AFTER the C1 drift-stamp pass
        // (unlike RT0-5 nothing here reads clock_c1_m_s — a pruned sat merely had its C1
        // stamped needlessly, which is harmless) and BEFORE the URA pass, so a withheld
        // constellation costs no URA work — the same seam RT0-5 occupies.  A satellite
        // whose constellation is NOT in the publish set is pruned; that includes any
        // constellation ABSENT from the σ̂0 pools entirely (zero rows ⇒ unjudgeable ⇒
        // not publishable under a contaminated epoch).  The downstream emitter seams —
        // presence-aware pending datum bumps, the post-prune retro split, the rise/set
        // code-datum detection — all key on the post-prune sol.sats, so they behave for
        // this prune exactly as they do for the (tested) RT0-5 quarantine.
        if (perconst_active) {
            std::set<char> pruned;
            std::vector<NetworkSatSolution> kept;
            kept.reserve(sol.sats.size());
            for (auto& s : sol.sats) {
                if (!s.sat.empty() && perconst_publish.count(s.sat[0]) == 0)
                    pruned.insert(s.sat[0]);
                else
                    kept.push_back(std::move(s));
            }
            if (!pruned.empty()) {
                sol.sats.swap(kept);
                ++n_perconst_partial_epochs;
                epoch_withheld_consts.insert(pruned.begin(), pruned.end());
                for (char cst : pruned) {
                    const auto sit    = perconst_stats.find(cst);
                    const bool judged = sit != perconst_stats.end();
                    log->warn("Network (RTA-10): TOW {:.1f} constellation '{}' WITHHELD by the "
                              "per-constellation σ̂0 carve-out — σ̂0_c={} over {} row(s) "
                              "(ceiling {:.2f}, min rows {}); its sats are pruned from this "
                              "epoch's emission.",
                              se.time.tow, cst,
                              judged ? fmt::format("{:.2f}", sit->second.sigma0_c)
                                     : std::string("n/a"),
                              judged ? sit->second.rows : 0, raim_withhold, perconst_min_rows);
                    emitEvent("RTA-10_perconst", "constellation_withhold", std::string(1, cst),
                              "",
                              judged ? std::optional<double>(sit->second.sigma0_c) : std::nullopt,
                              raim_withhold, "drop_const",
                              fmt::format("rows={};global_sigma0={:.2f}",
                                          judged ? sit->second.rows : 0, sigma0_adapt));
                }
            } else if (perconst_dec_withhold.empty()) {
                // The honest edge of the per-constellation approximation: the GLOBAL σ̂0
                // exceeded the ceiling, yet EVERY judged constellation individually passed
                // (cross-constellation coupling — shared per-station nuisances — that no
                // per-constellation partial captures).  Publish everything, but say so
                // loudly and count it; a rising count says the carve-out's separability
                // assumption is failing on this network.
                ++n_perconst_diffuse_published;
                log->warn("Network (RTA-10): TOW {:.1f} DIFFUSE contamination published — global "
                          "σ̂0={:.2f} > ceiling {:.2f} while every judged constellation passed "
                          "individually (the per-constellation statistic disagreed with the "
                          "global one).",
                          se.time.tow, sigma0_adapt, raim_withhold);
                emitEvent("RTA-10_perconst", "diffuse_published", "", "", sigma0_adapt,
                          raim_withhold, "publish_all",
                          fmt::format("{} const(s) individually clean", perconst_stats.size()));
            }
            // else: the decision withheld constellation(s) that are not in this epoch's
            // emission anyway (RT0-5 already quarantined them, or they carry no solution
            // sats) — nothing pruned here, and not the diffuse edge either.
        }
        // B1: per-sat URA = EWMA(σ̂0) × formal clock σ (shared-covariance diagonal),
        // skipping datum-pinned clock columns (their variance is the artificial pin,
        // not a physical σ) and inflating a recently-refloated sat for its warmup horizon.
        // Uses the PREVIOUS epoch's σ̂0 EWMA (post-emit QC ordering — a one-epoch lag).
        if (ura_on) {
            // RTB-7(b): evaluate the quality-ramp LIFT rule (counts through the
            // previous epoch, matching the floor's indexing below): n_emitted ≥
            // max(warmup_epochs, phase_leveling_warmup_epochs) AND the σ̂0 EWMA
            // has received ≥ warmup_epochs updates — OR the EWMA is structurally
            // unavailable (the RTB-6 sentinel condition: emit_ura+raim+
            // phase_leveling not all armed; here ura_on holds, so raim_on&&pl_on
            // decides), in which case the epoch horizon alone lifts.  Logs the
            // RTB-7(c) cold-start-to-product-grade metric at the lift.  Guarded
            // by the floor ⇒ OFF (0) is byte-identical.
            if (nc.warmup_ura_floor_m > 0.0 && !ramp_lifted) {
                const bool ewma_structural = raim_on && pl_on;   // && ura_on (this block)
                if (networkWarmupRampLifted(static_cast<long long>(n_emitted),
                                            nc.warmup_epochs, pl_warmup,
                                            static_cast<long long>(ura_ewma_updates),
                                            ewma_structural)) {
                    ramp_lifted    = true;
                    grade_metric_s = (first_emit_content_s >= 0.0)
                                         ? (pl_now_s - first_emit_content_s)
                                         : 0.0;
                    log->info("Network (RTB-7): cold-start-to-product-grade: {:.1f} s / {} "
                              "epochs — warmup URA floor {:.1f} m LIFTED (n_emitted {} >= "
                              "max(warmup_epochs {}, pl_warmup {}); sigma0-EWMA updates {}{}).",
                              grade_metric_s, n_emitted, nc.warmup_ura_floor_m, n_emitted,
                              nc.warmup_epochs, pl_warmup, ura_ewma_updates,
                              ewma_structural ? "" : " — EWMA structurally unavailable, "
                                                     "update count waived");
                }
            }
            const Eigen::MatrixXd& scov = est.sharedCovariance();
            std::set<Eigen::Index> pinned;
            for (const auto& pin : plan.shared_datum) pinned.insert(pin.column);
            const double s0 = std::max(1.0, ura_sigma0_ewma);
            for (auto& s : sol.sats) {
                if (!s.has_clock) continue;
                Eigen::Index col;
                try { col = est.clockColumn(s.sat); } catch (...) { continue; }
                if (pinned.count(col)) continue;   // datum pivot — bogus pin variance, no honest URA
                if (col < 0 || col >= scov.rows() || col >= scov.cols()) continue;
                const double var = scov(col, col);
                if (!(var > 0.0) || !std::isfinite(var)) continue;
                double ura = s0 * std::sqrt(var);
                const auto hit = ura_refloat_horizon.find(s.sat);
                if (ura_refloat_inflate > 1.0 && hit != ura_refloat_horizon.end() && hit->second > 0)
                    ura *= ura_refloat_inflate;   // honest surrogate for the post-refloat DIA penalty
                // RTB-7(b): cold-start quality ramp — a freshly (re)started producer
                // advertises DEGRADED URA until the ramp LIFTS (the
                // networkWarmupRampLifted rule evaluated above: emitted-epoch
                // horizon AND σ̂0-EWMA stabilization, not a bare epoch count), so a
                // consumer never trusts a not-yet-converged network solution.  A
                // warm-resumed producer restores ramp_lifted from the snapshot and
                // is not degraded.  Off (floor 0) ⇒ byte-identical.
                if (nc.warmup_ura_floor_m > 0.0 && !ramp_lifted)
                    ura = std::max(ura, nc.warmup_ura_floor_m);
                s.clock_ura_m = ura;
                s.has_ura     = true;
            }
        }
        // Datum bump + serialization, applied HERE (after a successful solve so a
        // withheld/failed epoch never leaks a pending bump).  The A1/A2 clock-pivot
        // handover (this epoch) is a per-CONSTELLATION datum re-reference (E-line
        // field-8); the B1 retro-bump (a post-emit QC tier flagged these SATELLITES
        // last epoch) is a per-SATELLITE discontinuity (R record).  Both must
        // reproduce on replay from the serialized flags.  A GLOBAL datum bump
        // (emit_clock_disc off) serializes an EMPTY constellation set so replay
        // re-applies the global bump.
        bool           global_bump = false;
        std::set<char> serialized_consts;
        if (datum_changed) {
            if (emit_clock_disc) {
                emitter.markDatumChange(changed_consts);
                serialized_consts.insert(changed_consts.begin(), changed_consts.end());
            } else {
                emitter.markDatumChange();   // global (pre-A2 byte-identical)
                global_bump = true;
            }
            // PIVOT (V1/V2): the datum re-reference is otherwise UNLOGGED.  One row per
            // changed constellation, sat=new pivot, detail=<const>:old->new + arm.  Arm is
            // derived here (pivotHandoverSignalled does not return it): prev_pivot still holds
            // the key ⇒ adjacent handover; else ⇒ vanish_return (old pivot from last_pivot).
            // Emitted BEFORE prev_pivot advances below.
            if (events_trace.is_open()) {
                for (char c : changed_consts) {
                    const std::string key(1, c);
                    auto np = plan.clock_pivot.find(key);
                    const std::string new_p = (np != plan.clock_pivot.end()) ? np->second
                                                                             : std::string();
                    std::string old_p;
                    const char* arm;
                    auto pp = prev_pivot.find(key);
                    if (pp != prev_pivot.end()) { old_p = pp->second; arm = "adjacent"; }
                    else {
                        auto lp = last_pivot.find(key);
                        old_p = (lp != last_pivot.end()) ? lp->second : std::string("?");
                        arm = "vanish_return";
                    }
                    emitEvent("PIVOT", "handover", new_p, "", std::nullopt, std::nullopt,
                              "datum_bump", fmt::format("{}:{}->{};{}", key, old_p, new_p, arm));
                }
            }
        }
        std::set<std::string> retro_emit_sats;   // B1 retro-bump applied this emit (serialized as an R record)
        if (ura_retro && !ura_retro_sats.empty()) {
            // RT0-5 seam (P0-1: the R record must describe bumps that LANDED in the
            // emitted product): a flagged sat ABSENT from this epoch's emission —
            // its constellation quarantined above, or the sat itself vanished —
            // must not be consumed here.  The emitter would clear its mark without
            // any of its keys stepping, and the R record would overstate a bump
            // that never reached the product (replay would then diverge from
            // intent, not from the file).  Keep it PENDING so it is marked AND
            // serialized at its next actual emit; only sats present in the
            // (post-quarantine) sol.sats are consumed now, so mark == land and
            // replay re-marks exactly the landed sats on exactly those epochs.
            // Edge-triggered semantics preserved: a pending sat is already in
            // retro_active, so it is not re-queued while it waits.
            std::set<std::string> present_sats;
            for (const auto& s : sol.sats) present_sats.insert(s.sat);
            std::set<std::string> retro_pending;
            for (const auto& sat : ura_retro_sats)
                (present_sats.count(sat) ? retro_emit_sats : retro_pending).insert(sat);
            ura_retro_sats = std::move(retro_pending);   // absent sats wait for their next emit
            if (!retro_emit_sats.empty())
                emitter.markSatDiscontinuity(retro_emit_sats);   // per-sat, not constellation-wide
            // RETRO (V4): one row per per-sat discontinuity bump APPLIED this emit.  The QC
            // tier that flagged the sat last epoch is lost at the union, so detail=union
            // (honest — do not fabricate provenance).
            if (events_trace.is_open())
                for (const auto& s : retro_emit_sats)
                    emitEvent("RETRO", "retro_bump", s, "", std::nullopt, std::nullopt,
                              "disc_bump", "union");
        }
        // Card 6: land the forced per-sat discontinuities for eph cutovers whose prior
        // version was evicted before Δ_eph could be computed (an unknown clock step → the
        // consumer must reset that sat's network-corrected ambiguity).  Independent of
        // ura_retro_bump — this is a genuine datum step, not a QC retro-bump — but reuses
        // the same present/pending split: a flagged sat ABSENT from this emission waits for
        // its next actual emit, so the bump lands with the sat present (replay-faithful).
        if (!eph_cutover_disc_pending.empty()) {
            std::set<std::string> present, mark, still_pending;
            for (const auto& s : sol.sats) present.insert(s.sat);
            for (const auto& sat : eph_cutover_disc_pending)
                (present.count(sat) ? mark : still_pending).insert(sat);
            eph_cutover_disc_pending = std::move(still_pending);
            if (!mark.empty()) {
                emitter.markSatDiscontinuity(mark);
                if (events_trace.is_open())
                    for (const auto& s : mark)
                        emitEvent("RELEVEL", "eph_cutover_disc_landed", s, "", std::nullopt,
                                  std::nullopt, "disc_bump", "prior_eph_version_evicted");
            }
        }
        // The handover-detection baseline advances ONLY here, with the bump it
        // pairs with: prev_pivot/last_pivot describe the datum of the last
        // EMITTED product.  A handover epoch that never reaches this point
        // (gate withhold, solve failure, σ̂0 withhold) leaves the baseline
        // untouched, so the change re-detects — and re-skips the old-datum
        // carries — every epoch until one emits in the new datum.
        prev_pivot = plan.clock_pivot;
        // A1: merge-update the persistent last-ever pivot map (never erase a
        // constellation) so a later vanish-return with a different pivot is caught.
        for (const auto& kv : plan.clock_pivot) last_pivot[kv.first] = kv.second;
        // Prioritized phase-bias anchor: record per-(sat,signal) anchor HANDOVERS
        // for observability, and advance the emit-baseline (emitted_anchor).  The
        // consumer-facing DISCONTINUITY is already handled — replay-safely — by the
        // emitter's per-(sat,signal) bias-jump test: a handover steps the emitted
        // bias by a different receiver's integer ambiguity lump (≥ ~1 cycle, well
        // above disc_threshold_m), so emitToStore below bumps that key's counter and
        // the consumer resets the ambiguity.  We deliberately do NOT issue a separate
        // markSatDiscontinuity here: it is not reconstructable from the serialized
        // NetworkSolutionEpoch (replay does not re-run the election), so it would
        // make the product's disc counters diverge on NetworkProductIO replay.  The
        // baseline advances ONLY at emit (a withheld epoch re-detects next emit),
        // never erasing an absent key (matches the last_pivot policy).
        if (phase_datum_prioritized) {
            int n_handover = 0;
            for (const auto& [amb, sta] : this_epoch_anchor) {
                auto it = emitted_anchor.find(amb);
                if (it != emitted_anchor.end() && it->second != sta) {
                    emitEvent("PHASE_ANCHOR", "anchor_handover",
                              amb.substr(0, amb.find('_')), sta,
                              std::nullopt, std::nullopt, "reanchor",
                              it->second + "->" + sta);
                    ++n_handover;
                }
                emitted_anchor[amb] = sta;
            }
            if (n_handover > 0)
                log->debug("Network: {} per-(sat,signal) phase-bias anchor handover(s) at "
                           "TOW {:.1f} (disc handled by the emitter bias-jump test).",
                           n_handover, se.time.tow);
        }
        // B6 (P1-1): open the deferred contract writer at the FIRST emit, stamping the
        // realized per-constellation clock pivots (this epoch's plan) into the v2 header.
        if (contract_pending) {
            contract_pending = false;
            for (const auto& [cst, sat] : plan.clock_pivot)
                pending_contract.decl.clock_pivot_sat[cst] = sat;   // realized auto-picked pivots
            // RTA-3: the generation id (minted above at this same first emit) travels
            // in the contract as an optional gid= field — 0 (unset) writes nothing.
            pending_contract.generation_id = sol.generation_id;
            product_writer = std::make_unique<NetworkProductWriter>(prod_path.string(), pending_contract);
            if (product_writer->good()) {
                log->info("Network: product serialization open — v2 S-basis contract, "
                          "{} realized clock pivot(s).", pending_contract.decl.clock_pivot_sat.size());
                last_contract_emit_s = pl_now_s;   // RTA-5: the header S line is the first broadcast
            } else {
                log->error("Network: failed to open product_output_path '{}' for writing; "
                           "disabling on-disk serialization for this run.", prod_path.string());
                product_writer.reset();
            }
        }
        emitter.emitToStore(sol, network_store);
        // RTA-5: periodic S-contract re-broadcast — a CURRENT S line (refreshed
        // realized pivots) every N seconds of PRODUCT time, and additionally on any
        // epoch whose emit carried a datum change (markDatumChange fired above), so a
        // late joiner sees the contract and a post-handover reader sees the pivots the
        // product is NOW realized in.  Pivots refresh from last_pivot — the merged
        // emit-baseline map that advanced just above, paired with this epoch's bump:
        // it holds this epoch's realized pivot for every present constellation and the
        // last-EMITTED pivot for an absent one, i.e. exactly the datum of the product
        // as of this emit.  pending_contract is refreshed in place so the end-of-run
        // round-trip self-check compares against the LAST written contract.  Written
        // BEFORE the epoch block so the S line positionally precedes the epochs it
        // describes; the elapsed>0 guard keeps the first-emit epoch single-S (the
        // header line just written).  OFF (0) ⇒ no extra S lines (byte-identical).
        if (contract_rebroadcast_s > 0.0 && product_writer && nc.serialize_sbasis_contract
            && last_contract_emit_s >= 0.0 && pl_now_s > last_contract_emit_s) {
            const bool due = (pl_now_s - last_contract_emit_s) >= contract_rebroadcast_s;
            if (due || datum_changed) {
                for (const auto& [cst, sat] : last_pivot)
                    pending_contract.decl.clock_pivot_sat[cst] = sat;   // refresh realized pivots
                product_writer->writeContract(pending_contract);
                last_contract_emit_s = pl_now_s;
            }
        }
        if (product_writer) {
            // RTA-10: the emitted epoch's heartbeat, written immediately BEFORE its epoch
            // block (the S-line positional precedent — an H describes what follows).
            // status=partial iff any constellation was withheld from this emission (the
            // RT0-5 runaway quarantine and/or the per-constellation σ̂0 carve-out);
            // sigma0 = the global within-epoch σ̂0 (0 when the within-epoch RAIM did not
            // run this epoch — "unavailable" per the H grammar); n_sats counts the
            // POST-prune emission.  Never feeds replay; flushed with the epoch below.
            if (emit_epoch_status) {
                NetworkProducerStatus hst;
                hst.time.week        = se.time.week;
                hst.time.tow         = se.time.tow;
                hst.state            = epoch_withheld_consts.empty()
                                           ? NetworkProducerStatus::State::Ok
                                           : NetworkProducerStatus::State::Partial;
                hst.sigma0           = sigma0_adapt;
                hst.n_sats_published = static_cast<int>(sol.sats.size());
                if (epoch_withheld_consts.empty()) {
                    hst.withheld = "-";
                } else {
                    hst.withheld.clear();
                    for (char cst : epoch_withheld_consts) {
                        if (!hst.withheld.empty()) hst.withheld += ';';
                        hst.withheld += cst;
                    }
                }
                product_writer->writeEpochStatus(hst);
            }
            const bool any_bump = global_bump || !serialized_consts.empty();
            product_writer->write(sol, any_bump, global_bump ? std::set<char>{} : serialized_consts,
                                  retro_emit_sats,
                                  // RTA-4: post-emit counter source — emitToStore ran
                                  // above, so the emitter's counters ARE this epoch's
                                  // store-record values.
                                  ser_disc_ctrs ? &emitter : nullptr);
            // RTA-6: per-epoch flush so a live tail consumer (NetworkProductTail)
            // sees this epoch block now, not at an ofstream-buffer boundary.
            // Covers the RTA-5 S re-broadcast written above too (same stream).
            // Changes no bytes; default-off toggle discipline.
            if (nc.product_flush_per_epoch) product_writer->flush();
        }
        if (n_emitted == 0) first_emit_content_s = pl_now_s;   // RTB-7(c): grade-metric anchor
        ++n_emitted;
        last_emit_content_s = pl_now_s;   // RTB-6: content time of this emit (health last-emit age)
        // R12: commit the B8/T2 within-epoch-trigger flag-rate now that the epoch has EMITTED
        // (withheld/skipped epochs never reach here, so T2's denominator = emitted epochs, the
        // same population T3–T6 count over).  Guarded by trace_flag_rates ⇒ OFF byte-identical.
        if (trace_flag_rates && we_t2_opp) {
            ++fr_we_trigger.opp;
            if (we_t2_flag) ++fr_we_trigger.flag;
        }

        // --- Closed-loop consume: held-out station vs the fresh network store -
        // Run the consumer ONLY now that this epoch's products are in the store,
        // so its SSR clock + phase bias reflect the just-solved network solution.
        if (closed_loop_on && consumer) {
            auto cit = se.by_station.find(cl.station.name);
            if (cit != se.by_station.end() && !cit->second.empty()) {
                consumer->processEpoch(cit->second);
                ++cl_epochs;

                const Eigen::Vector3d pos = consumer->getPosition();
                const Eigen::Vector3d d   = pos - cl_truth;
                const Eigen::Vector3d enu = Coordinates::ecefToEnu(d, cl_truth_llh);
                const double err3d = d.norm();

                const bool ar_ran = consumer->lastArRan();
                const AmbiguityResolution& ar = consumer->lastArResult();
                const bool fixed = ar_ran && ar.success;
                if (ar_ran) ++cl_ar_ran;
                if (fixed) {
                    ++cl_ar_fixed;
                    cl_sum_fixed += ar.num_fixed;
                    if (cl_first_fix_tow < 0.0) {
                        cl_first_fix_tow = se.time.tow; cl_first_fix_week = se.time.week;
                    }
                }
                // Converged-segment position RMS: rolling window over the most
                // recent epochs (the float transient + any early wrong-fix
                // excursions are not a fair measure of steady-state quality).
                cl_recent_err3d.push_back(err3d);
                if (cl_recent_err3d.size() > kClRecentWindow) cl_recent_err3d.pop_front();

                if (cl_trace)
                    cl_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                             << cit->second.size() << ',' << (ar_ran ? 1 : 0) << ','
                             << (fixed ? 1 : 0) << ',' << (ar_ran ? ar.num_fixed : 0) << ','
                             << (ar_ran ? ar.num_candidates : 0) << ','
                             << fmt::format("{:.2f}", ar_ran ? ar.ratio : 0.0) << ','
                             << fmt::format("{:.4f}", err3d) << ','
                             << fmt::format("{:.4f}", enu.x()) << ','
                             << fmt::format("{:.4f}", enu.y()) << ','
                             << fmt::format("{:.4f}", enu.z()) << '\n';
            }
        }

        // Per-satellite product trace (after emit, so discontinuity counters are current).
        if (soln) {
            for (const auto& s : sol.sats) {
                if (s.has_clock)
                    soln << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                         << s.sat << ",CLK,," << fmt::format("{:.4f}", s.clock_c0_m)
                         << ',' << emitter.clockDiscontinuityCounter(s.sat) << '\n';  // A2: 0 when off (byte-identical)
                for (const auto& [signal, pb] : s.phase_bias)
                    soln << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                         << s.sat << ",PB," << signal << ',' << fmt::format("{:.4f}", pb.bias_m)
                         << ',' << emitter.discontinuityCounter(s.sat, signal) << '\n';
                for (const auto& [signal, cb] : s.code_bias_m)   // W6 satellite code bias (OSB)
                    soln << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                         << s.sat << ",CB," << signal << ',' << fmt::format("{:.4f}", cb)
                         << ',' << emitter.codeBiasDiscontinuityCounter(s.sat, signal) << '\n';
            }
        }

        // --- Network-estimated slant ionosphere (viz trace) -----------------
        // Back-substitute each present station's local nuisance block (the slant
        // iono lives there, an absolute per-epoch estimate with linearization
        // point 0) and emit one row per (station, satellite) at the measured
        // elevation/azimuth, so the pyviz viewer can map each value to its
        // ionospheric pierce point.  Gated by network.trace_ionosphere.
        if (iono_trace.is_open()) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;   // not added to this epoch's solve
                Eigen::VectorXd x_r;
                try {
                    x_r = est.reconstructStation(
                        static_cast<std::size_t>(est_index_of[i]), x_s);
                } catch (...) { continue; }
                // Elevation/azimuth per satellite from the modeled observations
                // (first observation of each satellite carries the geometry).
                std::map<std::string, std::pair<double, double>> elaz;
                for (const auto& o : modeled[i])
                    elaz.emplace(o.satellite, std::make_pair(o.elevation, o.azimuth));
                const auto& h2l = asm_res[i].h2local;
                for (const auto& kv : stations[i].sm->getIonosphereStates()) {
                    const auto hit = h2l.find(static_cast<int>(kv.second));
                    if (hit == h2l.end()) continue;
                    if (hit->second < 0
                        || static_cast<Eigen::Index>(hit->second) >= x_r.size())
                        continue;
                    const auto e = elaz.find(kv.first);
                    if (e == elaz.end()) continue;
                    iono_trace << se.time.week << ','
                               << fmt::format("{:.3f}", se.time.tow) << ','
                               << stations[i].name << ',' << kv.first << ','
                               << fmt::format("{:.4f}", x_r(hit->second)) << ','
                               << fmt::format("{:.2f}", e->second.first) << ','
                               << fmt::format("{:.2f}", e->second.second) << '\n';
                }
            }
        }

        // --- Per-station ZWD trace (L7) -------------------------------------
        // One row per present station each emitted epoch: the estimated ZWD (a-priori state +
        // reconstructed correction) and its datum-conditioned formal σ (the V7 border-sharpening
        // instrument).  Independent of the carry — locate the ZWD column via getTropoIndex /
        // h2local, NOT R.pl_zwd_present (that is only set under carry_warm && zwd_on, so run A of
        // the V7 A/B and warmup would otherwise emit no rows).
        if (zwd_trace.is_open()) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;               // not in this epoch's solve
                const size_t tropo_idx_sz = stations[i].sm->getTropoIndex();
                if (tropo_idx_sz == std::numeric_limits<size_t>::max()) continue;  // no ZWD state
                const int tropo_idx = static_cast<int>(tropo_idx_sz);
                const auto& h2l = asm_res[i].h2local;
                const auto zhit = h2l.find(tropo_idx);
                if (zhit == h2l.end() || zhit->second < 0) continue;   // ZWD not in A_r this epoch
                const Eigen::Index zcol = static_cast<Eigen::Index>(zhit->second);
                Eigen::VectorXd x_r, vard;
                try {
                    x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s);
                    vard = est.conditionalLocalVarianceDiag(
                        static_cast<std::size_t>(est_index_of[i]), {zcol}, plan.shared_datum);
                } catch (...) { continue; }
                if (zcol >= x_r.size()) continue;
                const double x_lin = stations[i].sm->getStateVector()(tropo_idx);  // ≡0.0 today; kept for robustness
                const double zwd_m = x_lin + x_r(zcol);
                const double var   = (vard.size() > 0) ? vard(0) : -1.0;
                if (!(var > 0.0) || !std::isfinite(var) || !std::isfinite(zwd_m)) continue;
                zwd_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                          << stations[i].name << ',' << fmt::format("{:.6f}", zwd_m) << ','
                          << fmt::format("{:.6e}", std::sqrt(var)) << ','
                          << (asm_res[i].pl_zwd_injected ? 1 : 0) << '\n';
            }
        }

        // --- A-priori-clock-anchor information-fraction diagnostic (B7) ------
        // Certify the estimable network clock is DATA-dominated, not defined by the
        // a-priori CODE product it is softly anchored to.  anchor_info is the constant
        // Tikhonov pull (1/σ²); total_info is the reduced-normals diagonal on the clock
        // column (data + carry prior + datum pin).  P1-4: the legacy anchor/total fraction
        // is MISLEADING for a locked-in (carry-dominated) clock — its huge carry info
        // shrinks the fraction and certifies "data-dominated" for a column that is really
        // defined by its own past.  Decompose the denominator: subtract the carried
        // clock-prior info (clk_prior_info, folded this epoch) and the anchor, leaving
        // data_info; report anchor_frac_data = anchor/(anchor+data_info) — independent of
        // carry stiffness — plus carry_frac = carry/total, which lights up on a lock-in.
        // Read-only; the anchor fold itself is unchanged.
        if (anchor_info_trace.is_open()) {
            const double clk_sigma_a = clk_anchor_sigma;  // D6-resolved (matches the fold site above)
            const bool anchor_active =
                std::isfinite(clk_sigma_a) && clk_sigma_a > 0.0 && clk_sigma_a < 100.0;
            const double anchor_info =
                anchor_active ? 1.0 / (clk_sigma_a * clk_sigma_a) : 0.0;
            const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
            for (const std::string& sat : registered_clock_sats) {
                Eigen::Index col;
                try { col = est.clockColumn(sat); } catch (...) { continue; }
                if (col < 0 || col >= Nss.rows()) continue;
                const double total_info = Nss(col, col);
                const auto cpi = clk_prior_info.find(sat);
                const double carry_info = (cpi != clk_prior_info.end()) ? cpi->second : 0.0;
                // Data information = everything not the carry prior or the anchor.  For a
                // pivot the datum pin is ALSO in total_info (is_pivot flags it — its
                // decomposition is pin-dominated and not a data-dominance statement).
                const double data_info = std::max(0.0, total_info - carry_info - anchor_info);
                const double denom_ad  = anchor_info + data_info;
                const double anchor_frac_data = (denom_ad > 0.0) ? (anchor_info / denom_ad) : 0.0;
                const double carry_frac = (total_info > 0.0) ? (carry_info / total_info) : 0.0;
                const double frac = (total_info > 0.0) ? (anchor_info / total_info) : 0.0;
                const auto pit = plan.clock_pivot.find(NetworkSbasis::constellationOf(sat));
                const bool is_pivot = (pit != plan.clock_pivot.end() && pit->second == sat);
                anchor_info_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << sat << ',' << fmt::format("{:.6e}", anchor_info) << ','
                    << fmt::format("{:.6e}", carry_info) << ','
                    << fmt::format("{:.6e}", data_info) << ','
                    << fmt::format("{:.6e}", total_info) << ','
                    << fmt::format("{:.6f}", anchor_frac_data) << ','
                    << fmt::format("{:.6f}", carry_frac) << ','
                    << fmt::format("{:.6f}", frac) << ',' << (is_pivot ? 1 : 0) << '\n';
            }
        }

        // --- Cross-station DD fractional-residual (B5) ----------------------
        // For each present station, back-substitute its local block and read each
        // ABSOLUTE float ambiguity (x_lin + x_r(col), m) per (sat,signal).  Within a
        // (const,signal), difference each sat against a reference sat (between-sat SD),
        // then difference that across station pairs → the between-station DD.  A DD near
        // an integer #cycles is evidence the float biases support user AR.  P1-2: the
        // fractional cycle (dd_cyc = dd_m/λ, frac_cyc = dd_cyc − round) is computed HERE
        // so the CSV is a ready fractional histogram.  EXCLUDED from the pool: the pivot
        // station (its ambiguities are pinned to zero — a degenerate DD) and GLONASS
        // (FDMA: no common λ per signal, between-sat SD not integer).  Read-only; gated.
        if (dd_frac_trace.is_open()) {
            auto sysOfChar = [](char c) -> System {
                switch (c) {
                    case 'G': return System::GPS;      case 'E': return System::Galileo;
                    case 'C': return System::BeiDou;   case 'J': return System::QZSS;
                    case 'S': return System::SBAS;     default:  return System::Unknown;
                }
            };
            struct AmbAbs { double value_m; };
            // (const,signal) -> sat -> station index -> absolute float ambiguity (m).
            std::map<std::pair<std::string, std::string>,
                     std::map<std::string, std::map<size_t, AmbAbs>>> pool;
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                // Legacy: skip the whole pivot (all its ambiguities are pinned to
                // zero → degenerate DD).  Prioritized: keep the station and skip only
                // its per-(sat,signal) anchored (pinned) ambiguities below.
                if (!phase_datum_prioritized && stations[i].is_pivot) continue;
                Eigen::VectorXd x_r;
                try {
                    x_r = est.reconstructStation(
                        static_cast<std::size_t>(est_index_of[i]), x_s);
                } catch (...) { continue; }
                const auto& h2l = asm_res[i].h2local;
                const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
                for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                    const auto parts = StateKeys::parseAmbiguityKey(key);
                    if (!parts) continue;
                    if (isGaugePinned(i, parts->satellite, parts->signal))
                        continue;   // anchored (pinned to 0) → degenerate DD
                    const std::string cst = NetworkSbasis::constellationOf(parts->satellite);
                    if (cst == "R") continue;   // GLONASS FDMA: excluded (no common λ / integer DD)
                    const int amb_idx = static_cast<int>(sidx);
                    if (amb_idx < 0 || amb_idx >= x_lin.size()) continue;
                    const auto hit = h2l.find(amb_idx);
                    if (hit == h2l.end()) continue;   // ambiguity column not in A_r this epoch
                    if (hit->second < 0
                        || static_cast<Eigen::Index>(hit->second) >= x_r.size()) continue;
                    const double value_m = x_lin(amb_idx) + x_r(hit->second);
                    if (!std::isfinite(value_m)) continue;
                    pool[{cst, parts->signal}][parts->satellite][i] = AmbAbs{value_m};
                }
            }
            for (auto& [cs, satmap] : pool) {
                std::string ref;   // reference sat = lexically-lowest sat present at ≥2 stations
                for (const auto& [sat, stamap] : satmap)
                    if (stamap.size() >= 2) { ref = sat; break; }
                if (ref.empty()) continue;
                // λ for this (const,signal).  GLONASS is already excluded, so the nominal
                // per-band wavelength is satellite-independent; 0 (unknown signal) leaves the
                // cycle columns 0 so the reader falls back to the metre column.
                double lambda = 0.0;
                if (dd_ppp_utils) {
                    const System sys = sysOfChar(cs.first.empty() ? '?' : cs.first[0]);
                    if (sys != System::Unknown)
                        lambda = dd_ppp_utils->getWavelength(sys, cs.second, ref);
                }
                const auto& rmap = satmap[ref];
                for (const auto& [sat, stamap] : satmap) {
                    if (sat == ref) continue;
                    for (auto ia = stamap.begin(); ia != stamap.end(); ++ia) {
                        const auto ra = rmap.find(ia->first);
                        if (ra == rmap.end()) continue;
                        const double sd_a = ia->second.value_m - ra->second.value_m;
                        auto ib = ia; ++ib;
                        for (; ib != stamap.end(); ++ib) {
                            const auto rb = rmap.find(ib->first);
                            if (rb == rmap.end()) continue;
                            const double sd_b = ib->second.value_m - rb->second.value_m;
                            const double dd_m = sd_a - sd_b;   // between-station DD (m)
                            const double dd_cyc = (lambda > 0.0) ? (dd_m / lambda) : 0.0;
                            const double frac_cyc =
                                (lambda > 0.0) ? (dd_cyc - std::round(dd_cyc)) : 0.0;
                            dd_frac_trace
                                << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                                << cs.first << ',' << cs.second << ','
                                << sat << ',' << ref << ','
                                << stations[ia->first].name << ',' << stations[ib->first].name << ','
                                << fmt::format("{:.4f}", dd_m) << ','
                                << fmt::format("{:.6f}", lambda) << ','
                                << fmt::format("{:.4f}", dd_cyc) << ','
                                << fmt::format("{:.4f}", frac_cyc) << '\n';
                        }
                    }
                }
            }
        }

        // --- Co-estimated SH-VTEC coefficient trace -------------------------
        if (vtec_sh_trace.is_open() && sh_model) {
            const Eigen::MatrixXd& shcov = est.sharedCovariance();
            for (const auto& c : sh_model->coeffs()) {
                Eigen::Index col;
                try { col = est.vtecCoeffColumn(c.n, c.m, c.trig); } catch (...) { continue; }
                const double val = x_s(col);
                const double v   = (col < shcov.rows() && col < shcov.cols())
                                       ? shcov(col, col) : 0.0;
                vtec_sh_trace << se.time.week << ','
                              << fmt::format("{:.3f}", se.time.tow) << ','
                              << c.n << ',' << c.m << ',' << c.trig << ','
                              << fmt::format("{:.4f}", val) << ','
                              << fmt::format("{:.4f}", (v > 0.0) ? std::sqrt(v) : 0.0) << '\n';
            }
        }

        // --- Phase-leveling carry: harvest posterior + stash (Increment 1) ----
        // For each present non-pivot station, recover its ambiguities' ABSOLUTE
        // values (x_lin + back-substituted Δx) and their conditional variances
        // (the cheap per-column diagonal harvest, NOT the full per-station inverse)
        // and refresh the persistent carry.  Then drop entries unseen beyond the
        // max gap (a stale-carry / map-growth guard — distinct from the slip-driven
        // REFLOAT/DROP of later increments).
        if (carry_warm) {
            for (size_t i = 0; i < stations.size(); ++i) {
                if (est_index_of[i] < 0) continue;
                // Prioritized: harvest the former pivot too — its non-anchored arcs
                // are in R.pl_amb; anchored (pinned) keys were excluded at build, so
                // inject and harvest partition on the SAME per-(sat,signal) predicate.
                if (!phase_datum_prioritized && stations[i].is_pivot) continue;  // legacy: pivot gauge-pinned, never carried
                const StationAssembly& R = asm_res[i];
                if (R.pl_amb.empty()) continue;
                std::vector<Eigen::Index> cols;
                cols.reserve(R.pl_amb.size());
                for (const auto& a : R.pl_amb) cols.push_back(a.col);
                Eigen::VectorXd xr, vard;
                try {
                    xr = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s);
                    vard = est.conditionalLocalVarianceDiag(
                        static_cast<std::size_t>(est_index_of[i]), cols, plan.shared_datum);
                } catch (...) { continue; }
                for (size_t a = 0; a < R.pl_amb.size(); ++a) {
                    const PlAmb& pa = R.pl_amb[a];
                    if (pa.col < 0 || pa.col >= xr.size()) continue;
                    const double value_abs = pa.x_lin + xr(pa.col);
                    const double var = (a < static_cast<size_t>(vard.size())) ? vard(a) : -1.0;
                    if (!std::isfinite(value_abs) || !(var > 0.0) || !std::isfinite(var)) continue;
                    local_carry[std::make_tuple(static_cast<int>(i), pa.sat, pa.signal)] =
                        LocalCarry{value_abs, var, pl_now_s};
                    // One-shot IF-phase refloat: the fresh post-slip value is now
                    // stashed, so drop any pending-reseed mark and let the next epoch
                    // resume normal carry on the new arc.  Cleared only after a
                    // SUCCESSFUL stash — a skipped non-finite value keeps the mark so
                    // the stale pre-slip carry is never re-trusted.  No-op when unmarked.
                    stations[i].sm->clearPendingReseed(pa.amb_key);
                }
            }
            for (auto it = local_carry.begin(); it != local_carry.end();) {
                if (pl_now_s - it->second.last_seen_s > pl_max_gap_s) it = local_carry.erase(it);
                else ++it;
            }
            // --- ZWD carry (B9): harvest posterior ZWD + stash (mirrors the ambiguity
            //     harvest; independent loop so it works regardless of ambiguity presence).
            if (zwd_on) {
                for (size_t i = 0; i < stations.size(); ++i) {
                    // B9: harvest the pivot's ZWD too — R.pl_zwd_present is set for the
                    // pivot now that the carry inject above runs for it.
                    if (est_index_of[i] < 0) continue;
                    const StationAssembly& R = asm_res[i];
                    if (!R.pl_zwd_present) continue;
                    Eigen::VectorXd xr, vard;
                    try {
                        xr = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s);
                        vard = est.conditionalLocalVarianceDiag(
                            static_cast<std::size_t>(est_index_of[i]), {R.pl_zwd.col},
                            plan.shared_datum);
                    } catch (...) { continue; }
                    if (R.pl_zwd.col < 0 || R.pl_zwd.col >= xr.size()) continue;
                    const double value_abs = R.pl_zwd.x_lin + xr(R.pl_zwd.col);
                    const double var = (vard.size() > 0) ? vard(0) : -1.0;
                    if (!std::isfinite(value_abs) || !(var > 0.0) || !std::isfinite(var)) continue;
                    zwd_carry[static_cast<int>(i)] = LocalCarry{value_abs, var, pl_now_s};
                }
                for (auto it = zwd_carry.begin(); it != zwd_carry.end();) {
                    if (pl_now_s - it->second.last_seen_s > pl_max_gap_s) it = zwd_carry.erase(it);
                    else ++it;
                }
            }
        }

        // --- Post-fit network RAIM (Increment 3, tier T2) -------------------
        // The safety net for the zero-process-noise carry.  The per-station slip
        // pre-processors (MW/GF TurboEdit, IF-phase triple-difference) are
        // threshold/pool-limited, so a real slip on a satellite that is SETTING (few
        // tracking stations, under the pooled-MAD count) passes undetected — and with
        // zero process noise the stale carry then freezes into the products forever.
        //
        // This runs AFTER the joint solve and spends the network redundancy the
        // per-station detectors cannot see.  A bad carried ambiguity leaks its error
        // into the SHARED satellite clock/phase-bias; the guilty station HIDES it
        // there (its own residual stays small), but every OTHER station tracking that
        // satellite is pinned to its own (correct) carry and CANNOT absorb the shared
        // shift, so its post-fit phase residual spikes.  Each carried ambiguity's phase
        // residual is a Baarda-style normalized residual: v·√w divided by a ROBUST
        // a-posteriori variance factor σ̂0 = max(1, 1.4826·median|v·√w|) pooled network-wide
        // this epoch, so a satellite is judged an outlier relative to the network's CURRENT
        // residual level, not to the mm measurement noise (which would trip on ordinary cm
        // model misfit).  A cross-station consensus on the normalized residual (aggregated
        // per satellite) therefore fires exactly when the contamination reached the PRODUCTS
        // (the shared clock actually moved ⇒ many innocent stations spike) and stays quiet
        // when a slip stayed local and harmless — robust WHILE THE SLIPPED FRACTION IS SMALL
        // (an all-move-together epoch inflates σ̂0 itself and is a separate diagnosis).  It
        // refloats the flagged satellite's
        // carry at EVERY station (marks pending-reseed, so the next epoch re-estimates
        // from data — the IF-phase detector's one-shot lifecycle).  Placed after the
        // harvest so the just-completed stash's clearPendingReseed does not wipe the
        // mark.  OFF (default) = byte-identical (no marks, no state change).
        if (raim_on && carry_warm) {
            // Pass 1: reconstruct each station once, form its post-fit phase residuals, and
            // record per (station, satellite) the largest v·√w (measurement-standardized),
            // pooling every phase-row value for the robust a-posteriori variance factor.
            struct AmbStat { std::string sat; int sta; double r_max; };
            std::vector<AmbStat> amb_stats;
            std::vector<double>  r_pool;
            // Prominence-attribution input (collected iff attribution is LOGGED or DRIVES the
            // action): one cell per (station, satellite, signal) carried-ambiguity residual.
            const bool attrib_on   = raim_attrib_trace.is_open();       // log the decomposition
            const bool attrib_act  = config_.networkRaimAttribution();  // let it drive the refloat
            // B8/T4 (P1-4): also build the attribution table under trace_flag_rates so the T4
            // flag-rate is MEASURABLE on a clean baseline with ONLY network.trace_flag_rates on —
            // the documented empirical-α workflow — without needing raim_attribution or the
            // attribution trace.  The median polish then runs read-only (no refloat unless
            // attrib_act, no trace unless attrib_on), feeding only the fr_attrib counter; byte-
            // identical when trace_flag_rates is off (this disjunct is then absent).
            const bool need_attrib = attrib_on || attrib_act || trace_flag_rates;
            struct Cell { int sta; std::string sat, sig; double r; };
            std::vector<Cell> cells;
            for (size_t i = 0; i < stations.size(); ++i) {
                // Prioritized: keep the former pivot — its non-anchored arcs are in
                // R.pl_amb (anchored/pinned keys already excluded at carry build).
                if (est_index_of[i] < 0
                    || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
                const StationAssembly& R = asm_res[i];
                if (R.pl_amb.empty() || R.A_s.cols() != x_s.size()) continue;
                Eigen::VectorXd x_r;
                try {
                    x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s);
                } catch (...) { continue; }
                if (x_r.size() != R.A_r.cols()) continue;
                const Eigen::VectorXd resid = R.y - R.A_s * x_s - R.A_r * x_r;  // post-fit O − C
                Eigen::VectorXd row_r;   // B2: per-row Baarda redundancy (empty when off)
                if (raim_redun) {
                    try {
                        row_r = est.rowRedundancy(static_cast<std::size_t>(est_index_of[i]),
                                                  R.A_s, R.A_r, R.w, plan.shared_datum);
                    } catch (...) { row_r.resize(0); }
                }
                // Largest measurement-standardized post-fit phase residual PER SATELLITE at this
                // station (a carried ambiguity's phase rows carry +1 on its A_r column; uncombined).
                std::map<std::string, double> sat_rmax;
                for (const auto& pa : R.pl_amb) {
                    if (pa.col < 0 || pa.col >= R.A_r.cols()) continue;
                    double r_max = 0.0;
                    for (Eigen::Index row = 0; row < R.A_r.rows(); ++row) {
                        if (std::abs(R.A_r(row, pa.col)) <= 0.5) continue;  // not a phase row of this ambiguity
                        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
                        double r = std::abs(resid(row)) * std::sqrt(std::max(0.0, w));
                        if (raim_redun && row < row_r.size()) {   // B2: standardize v/(σ·√r_i)
                            constexpr double kRedFloor = 1e-6;
                            const double ri = row_r(row);
                            if (ri <= kRedFloor) continue;   // uncheckable row: exclude from σ̂0 pool + r_max
                            r /= std::sqrt(ri);
                        }
                        r_pool.push_back(r);
                        if (r > r_max) r_max = r;
                    }
                    double& cur = sat_rmax[pa.sat];
                    if (r_max > cur) cur = r_max;
                    if (need_attrib) cells.push_back({static_cast<int>(i), pa.sat, pa.signal, r_max});
                }
                for (const auto& [sat, rmax] : sat_rmax)
                    amb_stats.push_back({sat, static_cast<int>(i), rmax});
            }

            // Robust a-posteriori variance factor for this epoch, network-wide.  Floored at 1
            // so a clean network is no more sensitive than the nominal stochastic model
            // (σ̂0→1 ⇒ identical to the plain standardized residual); when the whole network
            // misfits at cm, σ̂0 rises and the normalized residuals stay O(1) — so RAIM stops
            // firing on ambient misfit and flags only what stands out ABOVE it.
            double sigma0 = 1.0;
            if (!r_pool.empty()) {
                std::nth_element(r_pool.begin(), r_pool.begin() + r_pool.size() / 2, r_pool.end());
                sigma0 = std::max(1.0, 1.4826 * r_pool[r_pool.size() / 2]);
            }
            // B1: fold the network-wide σ̂0 into the URA EWMA (scales next epoch's emitted
            // URA — the post-emit ordering means the scale lands one epoch later).
            // RTB-7(b): the update COUNT feeds the quality-ramp lift rule (the
            // "EWMA has stabilized" half); read only while the warmup floor is armed.
            if (ura_on) {
                ura_sigma0_ewma = ura_alpha * sigma0 + (1.0 - ura_alpha) * ura_sigma0_ewma;
                ++ura_ewma_updates;
            }

            // Pass 2: normalize by σ̂0 and take the cross-station consensus per satellite.
            std::map<std::string, int> sat_flag_stations;  // satellite -> #stations whose normalized residual spiked
            struct RaimDetail { std::string sat; int sta; double s_raw, s_norm; bool spiked; };
            std::vector<RaimDetail> raim_details;           // per (station, sat); built only when tracing
            for (const auto& a : amb_stats) {
                const double s_norm = a.r_max / sigma0;
                const bool   spiked = (s_norm > raim_k);
                if (trace_flag_rates) { ++fr_raim_cell.opp; if (spiked) ++fr_raim_cell.flag; }  // B8/T3 cell
                if (station_res_trace.is_open())   // L13: every cell (not just spikes)
                    station_res_trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                                      << ',' << stations[a.sta].name << ',' << a.sat << ','
                                      << fmt::format("{:.3f}", a.r_max) << ','
                                      << fmt::format("{:.3f}", s_norm) << ','
                                      << fmt::format("{:.3f}", sigma0) << '\n';
                if (spiked) ++sat_flag_stations[a.sat];
                if (raim_trace.is_open())
                    raim_details.push_back({a.sat, a.sta, a.r_max, s_norm, spiked});
            }
            std::vector<std::string> flagged;
            for (const auto& kv : sat_flag_stations)
                if (kv.second >= raim_min_sta) flagged.push_back(kv.first);
            const std::set<std::string> fset(flagged.begin(), flagged.end());
            if (trace_flag_rates) {   // B8/T3 consensus: rate of consensus among sats that spiked ≥1 station
                fr_raim_sat.opp  += static_cast<long>(sat_flag_stations.size());
                fr_raim_sat.flag += static_cast<long>(flagged.size());
            }

            // A consensus-flagged satellite's slip leaked into its SHARED clock (that is why the
            // innocent stations spiked) — refloat the carried clock alongside the ambiguity.
            if (clk_mon_couple)
                for (const auto& s : fset) clock_refloat_sats.insert(s);
            // P0-2: consensus-flagged sats contaminated the emitted correction — feed the
            // retro-bump independent of the couple opt-in.
            if (retro_feed)
                for (const auto& s : fset) retro_flag_sats.insert(s);

            // Trace the consensus context: every station of a refloated sat (to see the
            // guilty-hidden / innocent-spike pattern) plus any spiked near-miss.  Quiet
            // epochs emit nothing.  σ̂0 is logged so the all-move-together epochs (where σ̂0
            // itself spikes) are diagnosable.
            if (raim_trace.is_open()) {
                for (const auto& d : raim_details) {
                    if (!(fset.count(d.sat) || d.spiked)) continue;
                    raim_trace
                        << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                        << d.sat << ',' << stations[d.sta].name << ','
                        << fmt::format("{:.3f}", d.s_raw) << ',' << fmt::format("{:.3f}", d.s_norm) << ','
                        << fmt::format("{:.3f}", sigma0) << ',' << fmt::format("{:.1f}", raim_k) << ','
                        << (d.spiked ? 1 : 0) << ',' << sat_flag_stations[d.sat] << ','
                        << raim_min_sta << ',' << (fset.count(d.sat) ? 1 : 0) << '\n';
                }
            }

            // --- Prominence attribution + optional attribution-driven refloat -------------
            // Robust 2-way median polish of the (station × satellite) standardized-residual
            // table separates a bad SATELLITE (whole column lights up), a bad STATION (whole
            // row), and an isolated ARC (interaction); a per-signal marginal on the polished
            // leftover flags a bad SIGNAL.  One pass over the residual cells already in hand —
            // no re-solve, unlike the user-side iterative snoop.  Always LOGGED when the
            // attribution trace is open; when network.raim_attribution is on it also DRIVES
            // the refloat (bad sat → network-wide, bad station → that station's arcs, isolated
            // arc → just that (station,sat)) instead of the per-satellite consensus.
            bool attrib_acted = false;
            if (need_attrib && !cells.empty()) {
                constexpr double kAttribReportProm = 4.0;  // robust-z above which an entity is logged

                // (station × sat) table: cell = max standardized residual over that
                // station-sat's signals.
                std::map<std::pair<int, std::string>, double> cell_max;
                for (const auto& c : cells) {
                    const auto key = std::make_pair(c.sta, c.sat);
                    auto it = cell_max.find(key);
                    if (it == cell_max.end() || c.r > it->second) cell_max[key] = c.r;
                }
                std::vector<std::tuple<int, std::string, double>> tbl;
                tbl.reserve(cell_max.size());
                for (const auto& [k, v] : cell_max) tbl.emplace_back(k.first, k.second, v);
                const MedianPolish mp = medianPolish2Way(tbl);

                // B8/T4 (P1-4): attribution flag-rate on the per-satellite median-polish
                // column effect, using the SAME criterion the acting path (bad_sats) uses —
                // σ̂0 above the within-epoch trigger AND effect/σ̂0 above raim_k — so the rate
                // is measurable on a clean baseline whether or not raim_attribution is acting.
                if (trace_flag_rates) {
                    const double trig_a = config_.networkRaimSigma0Trigger();
                    for (const auto& [sat, eff] : mp.col_eff) {
                        (void)sat;
                        ++fr_attrib.opp;
                        if (sigma0 > trig_a && sigma0 > 0.0 && eff / sigma0 > raim_k) ++fr_attrib.flag;
                    }
                }

                const auto sat_prom = robustProminence(mp.col_eff);
                const auto sta_prom = robustProminence(mp.row_eff);

                // Signal marginal on the polished leftover (net of overall + station + sat).
                std::map<std::string, std::vector<double>> sig_left;
                for (const auto& c : cells)
                    sig_left[c.sig].push_back(
                        c.r - mp.overall - mp.row_eff.at(c.sta) - mp.col_eff.at(c.sat));
                std::map<std::string, double> sig_eff;
                for (auto& [sig, v] : sig_left) sig_eff[sig] = medianCopy(v);
                const auto sig_prom = robustProminence(sig_eff);

                // Isolated-arc interaction scale.
                std::vector<double> inter_abs;
                inter_abs.reserve(mp.interaction.size());
                for (double v : mp.interaction) inter_abs.push_back(std::abs(v));
                const double inter_mad = std::max(1e-9, 1.4826 * medianCopy(inter_abs));

                // Attribution-driven refloat set (scale-consistent: an effect is "bad" when it
                // stands raim_k robust baselines (σ̂0) above the additive model).  A bad
                // satellite/station is caught wholesale; an isolated arc counts only if its
                // sat/station were NOT already caught (no double attribution).
                std::set<std::string> bad_sats;
                std::set<int>         bad_stations;   // deliberately never populated — see below
                std::vector<std::pair<int, std::string>> bad_arcs;
                if (attrib_act) {
                    // Wholesale SATELLITE refloat only when the network is GENUINELY contaminated
                    // (σ̂0 above the within-epoch trigger).  At a robust-clean epoch (σ̂0 ≈ 1) a high
                    // column effect is a median-polish artifact of a satellite visible across a
                    // regional station cluster, NOT a network-wide slip — refloating its carriers
                    // everywhere would wipe good accumulated precision and destabilize (the root of
                    // the 305 24h ~4 h lock-in).  The isolated-arc path below still refloats real
                    // single slips at ANY σ̂0.
                    if (sigma0 > config_.networkRaimSigma0Trigger())
                        for (const auto& [sat, eff] : mp.col_eff)
                            if (eff / sigma0 > raim_k) bad_sats.insert(sat);
                    // Refloat the carried clock of an attribution-flagged (network-wide) satellite
                    // too — an isolated ARC is a local slip and does NOT implicate the shared clock,
                    // so only whole-satellite culprits feed the clock refloat.
                    if (clk_mon_couple)
                        for (const auto& s : bad_sats) clock_refloat_sats.insert(s);
                    // P0-2: attribution whole-satellite culprits contaminated the emitted
                    // correction — feed the retro-bump independent of the couple opt-in.
                    // (Isolated arcs are LOCAL slips that do not implicate the shared per-sat
                    //  correction, so they are excluded — mirroring the clock-refloat scoping.)
                    if (retro_feed)
                        for (const auto& s : bad_sats) retro_flag_sats.insert(s);
                    // Bad-STATION wholesale refloat is DELIBERATELY DISABLED: a regional cluster of
                    // stations lighting up on ONE shared satellite event inflates their median-polish
                    // ROW effects, so this path mass-refloated dozens of innocent stations at once
                    // (78 stations / 3977 carriers at TOW 273150 in the 24 h run) → cascade →
                    // divergence.  A genuinely bad station is handled by the within-epoch station-
                    // exclude (σ̂0 > trigger) or surfaces as isolated arcs; a whole-station carrier
                    // wipe is never worth the mis-attribution risk.  bad_stations stays empty.
                    for (std::size_t k = 0; k < tbl.size(); ++k) {
                        if (mp.interaction[k] / sigma0 <= raim_k) continue;
                        const int sta = std::get<0>(tbl[k]);
                        const std::string& sat = std::get<1>(tbl[k]);
                        if (!bad_sats.count(sat))
                            bad_arcs.emplace_back(sta, sat);
                    }
                }

                if (attrib_on) {
                    std::map<std::string, int> sat_n, sig_n;
                    std::map<int, int> sta_n;
                    for (const auto& c : cells) { ++sat_n[c.sat]; ++sig_n[c.sig]; ++sta_n[c.sta]; }
                    auto emit = [&](const char* dim, const std::string& entity, double effect,
                                    double prom, int n, int acted) {
                        raim_attrib_trace
                            << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                            << dim << ',' << entity << ','
                            << fmt::format("{:.4f}", effect) << ',' << fmt::format("{:.2f}", prom) << ','
                            << n << ',' << fmt::format("{:.4f}", mp.overall) << ','
                            << fmt::format("{:.3f}", sigma0) << ',' << acted << '\n';
                    };
                    for (const auto& [sat, pr] : sat_prom)
                        if (pr > kAttribReportProm)
                            emit("SAT", sat, mp.col_eff.at(sat), pr, sat_n[sat],
                                 (attrib_act ? bad_sats.count(sat) : fset.count(sat)) ? 1 : 0);
                    for (const auto& [sta, pr] : sta_prom)
                        if (pr > kAttribReportProm)
                            emit("STA", stations[sta].name, mp.row_eff.at(sta), pr, sta_n[sta],
                                 (attrib_act && bad_stations.count(sta)) ? 1 : 0);
                    for (const auto& [sig, pr] : sig_prom)
                        if (pr > kAttribReportProm)
                            emit("SIG", sig, sig_eff.at(sig), pr, sig_n[sig], 0);
                    std::vector<std::size_t> order(mp.interaction.size());
                    for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
                    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                        return std::abs(mp.interaction[a]) > std::abs(mp.interaction[b]);
                    });
                    int arc_logged = 0;
                    for (std::size_t k : order) {
                        const double pr = std::abs(mp.interaction[k]) / inter_mad;
                        if (pr <= kAttribReportProm || arc_logged >= 20) break;
                        const int sta = std::get<0>(tbl[k]);
                        const std::string& sat = std::get<1>(tbl[k]);
                        const int acted = attrib_act
                            ? ((mp.interaction[k] / sigma0 > raim_k || bad_sats.count(sat)
                                || bad_stations.count(sta)) ? 1 : 0)
                            : (fset.count(sat) ? 1 : 0);
                        emit("ARC", stations[sta].name + "/" + sat, mp.interaction[k], pr, 1, acted);
                        ++arc_logged;
                    }
                }

                // Execute the attribution-driven refloat (surgical: whole sat / whole station /
                // single arc, per the decomposition).  Erasing local_carry + pending-reseed is
                // the same one-shot refloat the consensus path uses.
                if (attrib_act && (!bad_sats.empty() || !bad_stations.empty() || !bad_arcs.empty())) {
                    auto refloat_sat_at = [&](int i, const std::string& sat) -> int {
                        int n = 0;
                        for (const auto& pa : asm_res[i].pl_amb)
                            if (pa.sat == sat) {
                                local_carry.erase(std::make_tuple(i, pa.sat, pa.signal));
                                stations[i].sm->markAmbiguityPendingReseed(pa.amb_key);
                                ++n;
                            }
                        return n;
                    };
                    int n_ref = 0;
                    for (const auto& sat : bad_sats)
                        for (size_t i = 0; i < stations.size(); ++i)
                            if (present[i] && est_index_of[i] >= 0
                                && (phase_datum_prioritized || !stations[i].is_pivot))
                                n_ref += refloat_sat_at(static_cast<int>(i), sat);
                    // (bad_stations is intentionally empty — the whole-station refloat path is
                    //  disabled to avoid the regional mis-attribution mass-refloat.)
                    for (const auto& [sta, sat] : bad_arcs) n_ref += refloat_sat_at(sta, sat);
                    attrib_acted = true;

                    std::string detail;
                    for (const auto& s : bad_sats)
                        detail += (detail.empty() ? "" : ",") + std::string("sat:") + s;
                    for (int s : bad_stations)
                        detail += (detail.empty() ? "" : ",") + std::string("sta:") + stations[s].name;
                    if (!bad_arcs.empty())
                        detail += fmt::format("{}{} arc", detail.empty() ? "" : ",", bad_arcs.size());
                    log->info("Network[RAIM-attrib]: TOW {:.1f} refloated {} carried ambiguity(ies) — "
                              "{} bad sat(s), {} bad station(s), {} isolated arc(s) "
                              "(σ̂0={:.2f}, gate {:.1f}) [{}]",
                              se.time.tow, n_ref, bad_sats.size(), bad_stations.size(),
                              bad_arcs.size(), sigma0, raim_k, detail);
                    // T4: one event per ACTED entity (SAT with its column prominence; ARC keyed
                    // by station).  Full per-entity attribution (incl. arc prominence) is in
                    // network_raim_attrib.csv; here we log only the entities that were refloated.
                    if (events_trace.is_open()) {
                        const std::string sd = fmt::format("sigma0={:.2f}", sigma0);
                        for (const auto& s : bad_sats)
                            emitEvent("T4_attrib", "attrib_sat", s, "",
                                      sat_prom.count(s) ? std::optional<double>(sat_prom.at(s))
                                                        : std::nullopt,
                                      raim_k, "refloat", sd);
                        for (const auto& [sta, sat] : bad_arcs)
                            emitEvent("T4_attrib", "attrib_arc", sat, stations[sta].name,
                                      std::nullopt, raim_k, "refloat", sd);
                    }
                }
            }

            // --- "So bad" within-epoch adaptation trigger (detection + log) ---------------
            // When σ̂0 exceeds the trigger the joint solve is contaminated enough that emitting
            // it would be wrong.  The full adaptation drops the dominant culprit(s) and RE-SOLVES
            // this epoch (a beginEpoch replay of the scatter minus the culprit) to re-condition;
            // that core-solve change is staged.  For now we DETECT + arm it so the trigger rate
            // and the drop targets can be validated against the attribution first.
            if (config_.networkRaimWithinEpoch() && sigma0 > config_.networkRaimSigma0Trigger()) {
                log->warn("Network[RAIM within-epoch]: TOW {:.1f} σ̂0={:.2f} STILL > trigger {:.2f} after "
                          "adaptation — residual contamination the culprit-drop could not isolate "
                          "(emitted anyway; inspect network_raim_attrib.csv).",
                          se.time.tow, sigma0, config_.networkRaimSigma0Trigger());
                emitEvent("T2_within_epoch", "emitted_contaminated", "", "", sigma0,
                          config_.networkRaimSigma0Trigger(), "emit_anyway", "");
            }

            // Per-satellite consensus refloat — only when attribution is NOT driving the action.
            if (!attrib_act && !flagged.empty()) {
                int n_refloat = 0;
                for (size_t i = 0; i < stations.size(); ++i) {
                    if (est_index_of[i] < 0
                        || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
                    for (const auto& pa : asm_res[i].pl_amb)
                        if (fset.count(pa.sat)) {
                            // Drop the contaminated carry OUTRIGHT.  The harvest above
                            // just re-stashed this ambiguity's (contaminated) value into
                            // local_carry; erasing it means the next epoch finds no prior
                            // to inject (cit == end → no addStationPrior), so the ambiguity
                            // re-floats from data — a direct, unambiguous refloat that does
                            // not depend on the deferred pending-reseed skip path.  The mark
                            // is also set (belt-and-suspenders / cleared by the next stash).
                            local_carry.erase(std::make_tuple(static_cast<int>(i), pa.sat, pa.signal));
                            stations[i].sm->markAmbiguityPendingReseed(pa.amb_key);
                            ++n_refloat;
                        }
                }
                std::string flist;
                for (size_t e = 0; e < flagged.size(); ++e) { if (e) flist += ","; flist += flagged[e]; }
                log->info("Network[RAIM]: TOW {:.1f} refloated {} carried ambiguity(ies) on {} "
                          "satellite(s) [{}] — normalized post-fit residual > {:.1f} at >= {} "
                          "stations (σ̂0={:.2f}; a pre-processor-missed slip leaked into the shared "
                          "clock, caught by network redundancy).",
                          se.time.tow, n_refloat, flagged.size(), flist, raim_k, raim_min_sta, sigma0);
                // T3: one event per consensus-refloated sat.  statistic = the per-sat max
                // normalized post-fit residual (max r_max / σ̂0 over its stations), from amb_stats.
                if (events_trace.is_open()) {
                    std::map<std::string, double> t3_norm;
                    for (const auto& a : amb_stats) {
                        const double sn = (sigma0 > 0.0) ? a.r_max / sigma0 : a.r_max;
                        double& m = t3_norm[a.sat];
                        m = std::max(m, sn);
                    }
                    for (const auto& s : flagged)
                        emitEvent("T3_raim", "consensus_refloat", s, "",
                                  t3_norm.count(s) ? std::optional<double>(t3_norm.at(s))
                                                   : std::nullopt,
                                  raim_k, "refloat",
                                  fmt::format("sigma0={:.2f};min_sta={}", sigma0, raim_min_sta));
                }
            }
            (void)attrib_acted;
        }

        // --- Two-state oscillator clock: measurement update + normalized-innovation fault test
        //     (network.clock_model) ------------------------------------------------------------
        // Update each satellite's clock model with this epoch's solved offset and test the
        // normalized innovation against the model's own tight prediction band.  A healthy clock
        // tracks its prediction (|innov|/√S ~ 1); a self-consistent lock-in jumps hundreds of σ —
        // the detectable dynamics fault the scalar random-walk carry hides.  The per-constellation
        // MEDIAN innovation (the datum common-mode) is removed first, so a whole-constellation
        // datum re-reference is gauged out and only per-satellite faults flag.  A flagged sat is
        // added to clock_refloat_sats (its phase/code bias reinitialize in the stash) and its
        // clock model is loosened (large-q reinit) so it re-acquires toward the a-priori anchor
        // instead of freezing.  On a pivot handover the models are re-seeded (the datum moved).
        if (clk_model_warm) {
            if (datum_changed && !scoped_handover) {
                clock_osc.clear();   // datum moved: every carried offset is in the retired frame (t_last dropped with it)
                // T5 (V2 blind spot): the datum-change oscillator wipe is otherwise SILENT.
                emitEvent("T5_clock_model", "datum_osc_wipe", "", "", std::nullopt, std::nullopt,
                          "clear_osc", "datum re-reference");
            } else {
                // RTA-2 (network.scoped_handover_response): wipe ONLY the changed
                // constellation(s)' oscillators — their carried offsets are in the
                // retired frame; unaffected constellations' models survive and update
                // normally below.  A wiped satellite carries no prediction this epoch
                // (the scoped prior skip above), so it falls into the seed branch and
                // re-seeds from THIS epoch's already-new-datum solve — one epoch
                // earlier than the global wipe, which skips the whole update block.
                if (datum_changed) {
                    for (auto oit = clock_osc.begin(); oit != clock_osc.end();) {
                        if (datumChangeAffectsKey(oit->first, changed_consts, /*scoped=*/true))
                            oit = clock_osc.erase(oit);
                        else
                            ++oit;
                    }
                    std::string consts;
                    for (char c : changed_consts) {
                        if (!consts.empty()) consts += ',';
                        consts += c;
                    }
                    emitEvent("T5_clock_model", "datum_osc_wipe", "", "", std::nullopt,
                              std::nullopt, "clear_osc",
                              fmt::format("datum re-reference (scoped: {})", consts));
                }
                std::map<std::string, double> z_of, innov_of, S_of, p_post_of;
                std::map<char, std::vector<double>> innov_by_const;
                const Eigen::MatrixXd& shcov = est.sharedCovariance();   // posterior formal cov
                for (const std::string& sat : registered_clock_sats) {
                    Eigen::Index col;
                    try { col = est.clockColumn(sat); } catch (...) { continue; }
                    const double z = x_s(col);
                    if (!std::isfinite(z) || sat.empty()) continue;
                    z_of[sat] = z;
                    p_post_of[sat] = (col < shcov.rows() && col < shcov.cols())
                                         ? shcov(col, col) : 0.0;   // posterior formal variance
                    auto pit = clk_pred.find(sat);
                    if (pit == clk_pred.end()) continue;      // freshly seeded / gap — no prediction
                    innov_of[sat] = z - pit->second.x0;
                    // B3 (data-incest fix): the fault statistic uses the info-consistent
                    // prior-minus-posterior variance (p00_pred − p00_post), NOT the old incest
                    // R-add (pit->p00 + clk_model_r).  The posterior x_s already FOLDS the model
                    // prior, so its innovation vs the prediction is the shrunken residual; its
                    // correct variance is the information the data removed, p00_pred − p00_post.
                    // Using +R deflated S in exactly the weak-data regime the NIS is meant to police.
                    // Floor S with the measurement-noise scale (σ_floor² = clk_model_r), NOT 1e-12, so
                    // a weak-data epoch (p00_post → p00_pred ⇒ p00_pred − p00_post → 0, and dm → 0)
                    // yields NIS ≈ 0 rather than a 0/0 blow-up that would false-flag the lowest-
                    // confidence satellites.  This is a LOWER BOUND, not the incest of adding R to a
                    // posterior-consistent innovation.
                    S_of[sat]     = std::max(std::max(1e-12, clk_model_r),
                                             pit->second.p00 - p_post_of[sat]);
                    innov_by_const[sat[0]].push_back(innov_of[sat]);
                }
                std::map<char, double> med_innov;
                for (auto& [c, vv] : innov_by_const) {
                    std::nth_element(vv.begin(), vv.begin() + vv.size() / 2, vv.end());
                    med_innov[c] = vv[vv.size() / 2];
                }
                int n_flag = 0;
                std::string detail;
                for (const std::string& sat : registered_clock_sats) {
                    auto zit = z_of.find(sat);
                    if (zit == z_of.end()) continue;
                    const double z   = zit->second;
                    ClockOsc&    osc = clock_osc[sat];
                    auto pit = clk_pred.find(sat);
                    if (pit == clk_pred.end() || !osc.init) {
                        // Seed (first warm epoch of this satellite, OR its carried state was
                        // dropped as stale in the prediction above — a vanish-return): start
                        // from the solved value, zero drift, loose covariance; no fault test
                        // until it has a prediction.  Stamp t_last so the NEXT epoch's drift
                        // finite-difference and prediction use the true per-sat interval.
                        osc.x0 = z; osc.x1 = 0.0;
                        osc.p00 = 1.0; osc.p01 = 0.0; osc.p11 = 1e-6;
                        osc.t_last = pl_now_s; osc.init = true;
                        continue;
                    }
                    const double S   = std::max(1e-12, S_of[sat]);
                    const double dm  = innov_of[sat] - med_innov[sat[0]];  // datum common-mode removed
                    const double nis = dm / std::sqrt(S);
                    const bool   ext = clock_refloat_sats.count(sat) > 0;  // within-epoch/RAIM (couple)
                    const bool   flagged = (std::abs(nis) > clk_model_k) || ext;
                    if (trace_flag_rates) { ++fr_clk_model.opp; if (std::abs(nis) > clk_model_k) ++fr_clk_model.flag; }  // B8/T5 own detection (excl. ext)
                    if (flagged && !ext) { ++n_flag; detail += (detail.empty() ? "" : ",") + sat;
                        emitEvent("T5_clock_model", "nis_fault", sat, "", nis, clk_model_k,
                                  "refloat", fmt::format("innov={:.4f}", dm)); }
                    if (flagged) clock_refloat_sats.insert(sat);
                    // B3 (data-incest fix): set the carried state DIRECTLY from the joint-solve
                    // posterior — NO scalar KF measurement update (that folded the model prior a
                    // second time, making the osc covariance self-confirmingly optimistic).  The
                    // offset + its variance ARE the solve posterior; the drift is finite-differenced
                    // from the offset sequence.  On a fault, reinit the variance (loosen) so a frozen
                    // clock can re-acquire rather than lock onto the bad value; the a-priori anchor
                    // pulls it back to truth.
                    const double p_post   = std::max(1e-12, p_post_of[sat]);
                    const double x0_prev  = osc.x0;
                    // B3b/P0-3: finite-difference the drift over THIS satellite's true elapsed
                    // time (pl_now_s − osc.t_last), NOT the global epoch dt.  A satellite whose
                    // last clock-model value came several epochs ago — because intervening epochs
                    // were SKIPPED (gate/σ̂0 withhold, solve failure) or the sat was absent —
                    // would otherwise divide a multi-interval Δoffset by one dt and emit a drift
                    // ~N× too large as C1 next epoch.  Past a gap bound the pair is not usable
                    // (reset drift to 0); within it the true interval gives the correct rate.
                    const double sat_dt   = (osc.t_last > 0.0) ? (pl_now_s - osc.t_last) : dt;
                    const bool contiguous = (osc.t_last > 0.0) && sat_dt > 0.0
                                            && sat_dt <= clk_model_max_gap;   // R11
                    osc.x1  = contiguous ? (z - x0_prev) / sat_dt : 0.0;
                    osc.x0  = z;
                    osc.p00 = flagged ? ((clk_mon_reinit_var > 0.0) ? clk_mon_reinit_var : (30.0 * 30.0))
                                      : p_post;                            // epoch formal variance
                    osc.p11 = flagged ? (osc.p11 + 1e-2) : osc.p11;        // reopen drift on fault
                    osc.p01 = 0.0;   // offset/drift cross term not tracked in the direct set
                    osc.t_last = pl_now_s;   // P0-3: stamp this sat's update time
                    osc.init = true;
                    // Trace (network.trace_slip_events): the clk_mon columns are remapped for the
                    // model — clk_m=solved, median_m=predicted, dev_m=de-meaned innov, prev_clk_m=
                    // drift, jump_m=raw innov, abs_max_m=band(√S), jump_max_m=k, flag_abs=(nis>k).
                    if (clk_mon_trace.is_open()
                        && (nc.trace_clock_full || flagged || std::abs(nis) > 0.3 * clk_model_k))
                        clk_mon_trace
                            << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                            << sat << ',' << fmt::format("{:.4f}", z) << ','
                            << fmt::format("{:.4f}", pit->second.x0) << ','
                            << fmt::format("{:.4f}", dm) << ','
                            << fmt::format("{:.6f}", osc.x1) << ','
                            << fmt::format("{:.4f}", innov_of[sat]) << ','
                            << fmt::format("{:.4f}", std::sqrt(S)) << ','
                            << fmt::format("{:.1f}", clk_model_k) << ','
                            << (std::abs(nis) > clk_model_k ? 1 : 0) << ",0,"
                            << (ext ? 1 : 0) << ',' << (flagged ? 1 : 0) << ','
                            << fmt::format("{:.4f}", nis) << ",model\n";   // L4: nis + mode
                }
                if (n_flag > 0)
                    log->warn("Network[clock-model]: TOW {:.1f} flagged {} satellite clock(s) [{}] — "
                              "normalized innovation > {:.1f}σ against the two-state prediction band "
                              "(dynamics fault); model loosened + carried state reinitialized.",
                              se.time.tow, n_flag, detail, clk_model_k);
                // P0-3: per-satellite osc.t_last (stamped above) now records each sat's last
                // update time directly, so the contiguity / gap decision is exact even across
                // skipped epochs — the old whole-set prev_osc_sats marker is retired.
            }
        }

        // --- Per-satellite carried-clock sanity monitor (network.clock_monitor) --------------
        // The σ̂0 RAIM above flags contamination that MOVES the shared residual level (many
        // innocent stations spike at once).  A lone satellite whose carried clock has locked
        // into a physically impossible value is INVISIBLE to it — the clock absorbs the error
        // self-consistently, σ̂0 stays ≈1, and the stiff sat_clock_q carry then FREEZES the
        // wrong value for the rest of the run (the multi-hour single-satellite lock-in).  This
        // is the missing detector for that mode: on the FINAL (post-adaptation) solution, flag
        // any satellite whose DE-MEANED clock exceeds clk_mon_abs or whose one-epoch jump
        // exceeds clk_mon_jump, and refloat its carried shared state (the stash below skips it)
        // so the next epoch injects no stale prior and the clock re-estimates freely toward its
        // data / a-priori-anchor value.  De-meaning by the network median removes the per-epoch
        // datum gauge (a few metres, not error); the jump test runs only across adjacent
        // stable-datum epochs.  clock_refloat_sats may already hold within-epoch / post-fit RAIM
        // sats (only under clock_monitor_couple_raim); those are refloated here too (ext).
        //
        // Gated on the emitted-epoch warmup (NOT carry_warm): the D2 shared clock carry runs
        // independently of phase leveling, so the monitor must too; it only needs to be past the
        // cold-start systematic (the same pl_warmup horizon the ambiguity carry uses).  Superseded
        // by the two-state normalized-innovation test above when network.clock_model is on.
        if (clk_mon_on && !clk_model_on && static_cast<int>(n_emitted) >= pl_warmup) {
            std::map<std::string, double> clk_of;
            std::map<char, std::vector<double>> clk_by_const;  // per-constellation, for the datum median
            for (const std::string& sat : registered_clock_sats) {
                Eigen::Index col;
                try { col = est.clockColumn(sat); } catch (...) { continue; }
                const double v = x_s(col);
                if (!std::isfinite(v) || sat.empty()) continue;
                clk_of[sat] = v;
                clk_by_const[sat[0]].push_back(v);
            }
            // De-mean PER CONSTELLATION.  Each constellation has its OWN clock datum (a distinct
            // pivot satellite), so a whole-constellation offset — e.g. a Galileo-wide re-reference
            // when its pivot hands over — is a GAUGE artifact, not per-satellite error.  A single
            // global median would subtract the GPS datum from the Galileo clocks and flag the
            // entire constellation; the per-constellation median cancels that and leaves only the
            // genuine per-satellite outliers within each system.
            std::map<char, double> med_by_const;
            for (auto& [c, vv] : clk_by_const) {
                std::nth_element(vv.begin(), vv.begin() + vv.size() / 2, vv.end());
                med_by_const[c] = vv[vv.size() / 2];
            }
            int n_new = 0;
            std::string detail;
            for (const auto& [sat, v] : clk_of) {
                const double med      = med_by_const[sat[0]];
                const double dev      = v - med;
                const bool   flag_abs = std::abs(dev) > clk_mon_abs;
                // Jump-test reference frame.  PIN mode: the raw clock v (the pivot pin
                // anchors the raw per-constellation frame across epochs, so v−prev_v is a
                // genuine per-sat step — byte-identical to the legacy path).  SUM-TO-ZERO
                // mode: the per-constellation DE-MEANED clock dev, because the raw
                // common-mode floats each epoch (Σδtˢ=0 is re-imposed on the current
                // satellite set, so a membership change redistributes ~Δ/N onto EVERY
                // survivor's raw clock — a datum artifact, not a per-sat jump).  De-meaning
                // by the constellation median removes that float, mirroring the two-state
                // clock-model NIS path's `innov − median`.
                const double jref     = clock_sum_to_zero ? dev : v;
                bool   have_jump  = false;
                double jump       = 0.0;
                bool   flag_jump  = false;
                const auto pit = prev_clk.find(sat);
                // Jump only across ADJACENT epochs of a stable datum: a handover shifts every
                // clock of the re-referenced constellation (common-mode, not a per-sat fault)
                // and a long gap makes the step meaningless — both fall back to the absolute
                // bound.  RTA-2: under network.scoped_handover_response the datum is stable
                // for constellations whose pivot did NOT change, so their jump test keeps
                // running through a sibling's handover; OFF, handover_affects == datum_changed
                // and the legacy all-constellation suppression is byte-identical.
                if (pit != prev_clk.end() && !handover_affects(sat) && dt > 0.0
                    && (pl_now_s - pit->second.second) <= 1.5 * dt) {
                    have_jump = true;
                    jump      = jref - pit->second.first;   // datum-invariant under sum-to-zero (jref=dev), raw under pin (jref=v)
                    flag_jump = std::abs(jump) > clk_mon_jump;
                }
                const bool ext       = clock_refloat_sats.count(sat) > 0;  // within-epoch / post-fit RAIM
                const bool refloated = flag_abs || flag_jump || ext;
                if (trace_flag_rates) { ++fr_clk_mon.opp; if (flag_abs || flag_jump) ++fr_clk_mon.flag; }  // B8/T6 own bounds (excl. ext)
                if (refloated && !ext) { ++n_new; detail += (detail.empty() ? "" : ",") + sat;
                    emitEvent("T6_clock_monitor", "clk_monitor_refloat", sat, "", dev, clk_mon_abs,
                              "refloat",
                              have_jump ? fmt::format("jump={:.4f};jmax={:.2f}", jump, clk_mon_jump)
                                        : std::string("abs")); }
                if (refloated) clock_refloat_sats.insert(sat);

                if (clk_mon_trace.is_open()
                    && (nc.trace_clock_full || refloated || std::abs(dev) > 0.5 * clk_mon_abs
                        || (have_jump && std::abs(jump) > 0.5 * clk_mon_jump))) {
                    clk_mon_trace
                        << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                        << sat << ',' << fmt::format("{:.4f}", v) << ','
                        << fmt::format("{:.4f}", med) << ',' << fmt::format("{:.4f}", dev) << ',';
                    if (have_jump)
                        clk_mon_trace << fmt::format("{:.4f}", pit->second.first) << ','
                                      << fmt::format("{:.4f}", jump) << ',';
                    else
                        clk_mon_trace << ",,";
                    clk_mon_trace
                        << fmt::format("{:.2f}", clk_mon_abs) << ','
                        << fmt::format("{:.2f}", clk_mon_jump) << ','
                        << (flag_abs ? 1 : 0) << ',' << (flag_jump ? 1 : 0) << ','
                        << (ext ? 1 : 0) << ',' << (refloated ? 1 : 0) << ",,monitor\n";  // L4: empty nis + mode
                }

                // Re-baseline the jump test: a refloated sat's clock is about to be
                // reinitialized, so drop its history (next epoch's recovery must not read as a
                // fresh jump); otherwise record this epoch's jump reference (jref) + time as the
                // next adjacent baseline (raw v in pin mode; de-meaned dev in sum-to-zero mode).
                if (refloated) prev_clk.erase(sat);
                else           prev_clk[sat] = {jref, pl_now_s};
            }
            if (n_new > 0)
                log->warn("Network[clock-monitor]: TOW {:.1f} reinitialized {} satellite clock(s) [{}] "
                          "exceeding |Δδtˢ−median| > {:.1f} m or |jump| > {:.1f} m/epoch — carried "
                          "shared state {} to break the single-satellite lock-in.",
                          se.time.tow, n_new, detail, clk_mon_abs, clk_mon_jump,
                          (clk_mon_reinit_sig > 0.0)
                              ? fmt::format("loosened to σ={:.0f} m (soft large-q reinit)", clk_mon_reinit_sig)
                              : std::string("withheld from the stash (hard refloat)"));
        }

        // B1: after all post-emit QC tiers, record the flagged sats for the NEXT emit's
        // retro-bump (per-SATELLITE disc bump) + post-refloat URA inflation horizon.
        // The flag set is the union of clock_refloat_sats (clock-model / clock-monitor,
        // plus the coupled RAIM tiers) and retro_flag_sats (the tier-independent P0-2
        // accumulator) — so the retro-bump reaches every QC tier regardless of the
        // clock_monitor_couple_raim opt-in.
        if (retro_feed) {
            std::set<std::string> flagged_now;
            for (const std::string& sat : clock_refloat_sats)
                if (!sat.empty()) flagged_now.insert(sat);
            for (const std::string& sat : retro_flag_sats)
                if (!sat.empty()) flagged_now.insert(sat);

            if (ura_refloat_inflate > 1.0 && pl_warmup > 0)
                for (const std::string& sat : flagged_now) ura_refloat_horizon[sat] = pl_warmup;

            // Retro-bump is EDGE-triggered (P1-3): queue only a sat that just became
            // flagged (not already in retro_active), so a satellite that stays flagged
            // through several recovery epochs is reset ONCE at the consumer, not every
            // emit.  A sat that clears drops out of retro_active, so a later re-flag
            // re-arms the bump.  ura_retro_sats is consumed at the next emit.
            if (ura_retro) {
                for (const std::string& sat : flagged_now)
                    if (!retro_active.count(sat)) ura_retro_sats.insert(sat);
                retro_active = std::move(flagged_now);
            }

            // Age out the post-refloat inflation horizon one epoch at a time.
            if (ura_refloat_inflate > 1.0)
                for (auto it = ura_refloat_horizon.begin(); it != ura_refloat_horizon.end();) {
                    if (--it->second <= 0) it = ura_refloat_horizon.erase(it);
                    else ++it;
                }
        }

        // --- D2 stash for the next epoch ------------------------------------
        prev_carry.clear();
        prev_carry_cov.clear();   // C1: rebuilt below when the dense carry is on
        std::vector<Eigen::Index> fc_cols;  // C1: shared-border column per dense row
        const Eigen::MatrixXd& cov = est.sharedCovariance();
        std::set<Eigen::Index> pinned_cols;
        for (const auto& p : plan.shared_datum) pinned_cols.insert(p.column);
        auto stash = [&](SharedParamKind kind, const std::string& sat,
                         const std::string& signal, Eigen::Index col, bool inflate) {
            // Skip the datum-pinned columns.  Their solve covariance is 1/pin_scale
            // (not ~0), but the explicit datum re-pins them every epoch (its
            // Dirichlet elimination overrides any prior on them), and carrying one
            // forward would wrongly anchor a retired pivot to its old datum value
            // after a handover.
            if (pinned_cols.count(col)) return;
            const double var = cov(col, col);
            if (!(var > 0.0)) return;
            CarryPrior cp;
            cp.kind = kind; cp.sat = sat; cp.signal = signal;
            cp.value = x_s(col); cp.var = var; cp.inflate_q = inflate;
            prev_carry.push_back(cp);
            if (fullcov_on) {   // C1: register this entry's dense row (same skip rules)
                prev_carry_cov.row.emplace(sharedCarryKey(kind, sat, signal),
                                           fc_cols.size());
                fc_cols.push_back(col);
            }
        };
        // A satellite flagged by the clock monitor (or, opt-in, the within-epoch / post-fit RAIM)
        // has its carried SHARED state REINITIALIZED rather than frozen.  Two modes: with
        // clk_mon_reinit_sig > 0 (default) it is STILL stashed but marked inflate_q, so next epoch
        // folds it with the loose reinit variance (a weak anchor that lets a frozen clock walk to
        // truth while curbing the datum drift a hard refloat causes); with clk_mon_reinit_sig ≤ 0
        // it is WITHHELD entirely (hard skip-stash refloat — no prior).  clock_refloat_sats is
        // empty unless network.clock_monitor, so the stash is byte-identical when the monitor is
        // off (nothing flagged → inflate=false → the ordinary stiff-carry stash).
        const bool full_refloat = !(clk_mon_reinit_sig > 0.0);   // reinit_sigma ≤ 0 → hard skip-stash
        // The satellite clock is stashed to prev_carry ONLY in scalar-carry mode.  Under
        // network.clock_model the clock is carried by clock_osc (updated above), so it is not
        // stashed here — the two-state model is its sole cross-epoch memory.
        if (!clk_model_on)
            for (const std::string& sat : registered_clock_sats) {
                const bool flagged = clock_refloat_sats.count(sat) > 0;
                if (flagged && full_refloat) continue;
                stash(SharedParamKind::Clock, sat, "", est.clockColumn(sat), flagged);
            }
        for (const auto& pk : shared_phase_keys) {
            const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
            if (!parts) continue;
            const bool flagged = clock_refloat_sats.count(parts->satellite) > 0;
            if (flagged && full_refloat) continue;
            stash(SharedParamKind::PhaseBias, parts->satellite, parts->signal,
                  est.phaseBiasColumn(parts->satellite, parts->signal), flagged);
        }
        for (const auto& kv : registered_code_keys) {  // W6 satellite code bias (OSB)
            const bool flagged = clock_refloat_sats.count(kv.first) > 0;
            if (flagged && full_refloat) continue;
            stash(SharedParamKind::CodeBias, kv.first, kv.second,
                  est.codeBiasColumn(kv.first, kv.second), flagged);
        }
        // C1: stash the dense covariance block over the clock/phase/code entries
        // just recorded (the SH block below keeps its own diagonal stash — its
        // carry is the separate pre-gate fold).  One O(k²) copy from the solve's
        // posterior.  A single non-finite entry clears the whole block — the next
        // fold then degrades per-key to the diagonal path instead of poisoning
        // the next epoch's normals (the exact crash mode the SH guard below
        // screens per-entry).
        if (fullcov_on && !fc_cols.empty()) {
            const Eigen::Index kfc = static_cast<Eigen::Index>(fc_cols.size());
            prev_carry_cov.P.resize(kfc, kfc);
            for (Eigen::Index a = 0; a < kfc; ++a)
                for (Eigen::Index b = 0; b < kfc; ++b)
                    prev_carry_cov.P(a, b) = cov(fc_cols[static_cast<std::size_t>(a)],
                                                 fc_cols[static_cast<std::size_t>(b)]);
            if (!prev_carry_cov.P.allFinite()) {
                log->warn("Network (C1): non-finite entries in the stashed shared-"
                          "covariance block ({} keys) — dense carry cleared; the next "
                          "epoch folds the diagonal carry.", fc_cols.size());
                prev_carry_cov.clear();
            }
        }
        if (config_.estimateNetworkIonoSh() && sh_model) {   // SH-VTEC coefficients
            for (const auto& c : sh_model->coeffs()) {
                const Eigen::Index col = est.vtecCoeffColumn(c.n, c.m, c.trig);
                if (pinned_cols.count(col)) continue;   // SH cols are never datum-pinned (safety)
                const double var = cov(col, col);
                const double val = x_s(col);
                // Never carry a non-finite value/variance forward — it would poison
                // the next epoch's normals and hard-crash the solve.  The mean-
                // reverting climatology prior re-seeds the coefficient cleanly.
                if (!(var > 0.0) || !std::isfinite(val) || !std::isfinite(var)) continue;
                CarryPrior cp;
                cp.kind = SharedParamKind::VtecSh;
                cp.sh_n = c.n; cp.sh_m = c.m; cp.sh_trig = c.trig;
                cp.value = val; cp.var = var;
                prev_carry.push_back(cp);
            }
        }

        // --- Trace ----------------------------------------------------------
        double sumsq = 0.0, maxabs = 0.0;
        for (const std::string& sat : registered_clock_sats) {
            const double v = x_s(est.clockColumn(sat));
            sumsq += v * v;
            maxabs = std::max(maxabs, std::abs(v));
        }
        const double rms = (n_clock > 0) ? std::sqrt(sumsq / n_clock) : 0.0;
        if (trace) {
            trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                  << n_present << ',' << n_clock << ',' << n_pb << ','
                  << plan.declared_dimension << ",1,1,"
                  << fmt::format("{:.4f}", rms) << ',' << fmt::format("{:.4f}", maxabs)
                  << ',' << total_pins << '\n';
        }
        // L1: the emitted-epoch disposition row — the median+std+worst-sat clock spread
        // (over the SAME registered_clock_sats / x_s(clockColumn) series as the rms above)
        // plus the QC action counts.  Computed only when the trace is open.
        if (epoch_status_trace.is_open()) {
            EpochStatusRow es;
            es.sigma0          = sigma0_adapt;   // 0.0 unless within-epoch RAIM ran this epoch
            es.n_adapt         = n_adapt;
            es.n_present       = n_present;
            es.n_clock_sats    = n_clock;
            es.n_refloat_sats  = static_cast<int>(clock_refloat_sats.size());
            es.n_retro_applied = static_cast<int>(retro_emit_sats.size());   // bumps APPLIED this emit
            es.datum_changed   = datum_changed ? 1 : 0;
            std::vector<double> cvals; cvals.reserve(registered_clock_sats.size());
            for (const std::string& sat : registered_clock_sats)
                cvals.push_back(x_s(est.clockColumn(sat)));
            if (!cvals.empty()) {
                std::vector<double> sorted = cvals;
                std::sort(sorted.begin(), sorted.end());
                const size_t m = sorted.size();
                const double median = (m % 2) ? sorted[m / 2]
                                              : 0.5 * (sorted[m / 2 - 1] + sorted[m / 2]);
                double ss = 0.0, worst = 0.0;
                std::string worst_sat_name;
                for (size_t k = 0; k < cvals.size(); ++k) {
                    const double d = cvals[k] - median;
                    ss += d * d;
                    if (std::abs(d) > worst) { worst = std::abs(d); worst_sat_name = registered_clock_sats[k]; }
                }
                es.clock_median_m = median;
                es.clock_std_m    = std::sqrt(ss / static_cast<double>(m));
                es.worst_sat      = worst_sat_name;
                es.worst_dev_m    = worst;
            }
            emitEpochStatus("emitted", es);
        }

        if (n_emitted == 1 || n_emitted % 60 == 0) {
            log->info("Network: TOW {:.1f} emitted epoch {} — {} stations, {} sat clocks, "
                      "{} phase biases, {} code biases ({} single-station sat(s) dropped), "
                      "datum dim {}, clock-Δ rms {:.3f} m",
                      se.time.tow, n_emitted, n_present, n_clock, n_pb, n_cb, n_dropped_sats,
                      plan.declared_dimension, rms);
            if (config_.estimateNetworkIonoSh() && sh_model) {
                double a00 = 0.0;
                try { a00 = x_s(est.vtecCoeffColumn(0, 0, 'C')); } catch (...) {}
                log->info("Network[SH-iono]:   mean VTEC (a00) = {:.2f} TECU over {} coeffs",
                          a00, sh_model->numCoeffs());
            }
        }

        prev_week = se.time.week; prev_tow = se.time.tow;

        // --- RTB-7(a): periodic atomic state snapshot -------------------------
        // End-of-epoch, EMITTED epochs only (skipped epochs `continue` above and
        // mutate no carry state), every state_snapshot_interval_s of PRODUCT
        // time.  Written AFTER this epoch's product emit + flush, to the
        // snapshot's own temp file + rename — the product writer is never
        // flushed or reordered by this block (emission is unperturbed).  Path
        // empty (default) ⇒ byte-identical.
        if (!snap_path.empty()
            && (last_snapshot_write_s < 0.0
                || pl_now_s - last_snapshot_write_s >= nc.state_snapshot_interval_s)) {
            // RTB-7 cold-state exclusions (in-tree record of a decision previously
            // documented only in memory/report): this snapshot carries ONLY shared,
            // network-level producer state — the cross-epoch carries (phase-level /
            // iono / ZWD), clock oscillators, pivots, generation id, URA/ramp metrics
            // and the emitter's disc-counter / baseline / first-sighting bookkeeping.
            // Per-STATION filter state is DELIBERATELY NOT snapshotted: per-station
            // cycle-slip arcs and slip-detector history, per-station StateManager
            // ambiguity / iono / receiver-bias marks, and per-station covariance.  On
            // a warm resume those layers cold-start and re-warm within a few epochs,
            // and the post-fit RAIM safety net absorbs the transient — serializing the
            // large per-station covariance blocks would buy nothing the re-warm does
            // not already provide.  That is the recovery-budget trade RTB-7 accepts.
            NetworkStateSnapshot snap;
            snap.snap_week          = se.time.week;
            snap.snap_tow           = se.time.tow;
            snap.wall_utc           = NetworkStateSnapshotIO::utcNowIso();
            snap.generation_id      = generation_id;
            snap.config_fingerprint = live_fingerprint;
            snap.n_emitted          = n_emitted;
            snap.n_skipped          = n_skipped;
            snap.n_gate_withheld    = n_gate_withheld;
            snap.n_sigma0_withheld  = n_sigma0_withheld;
            snap.n_runaway_epochs   = n_runaway_epochs;
            snap.n_perconst_partial = n_perconst_partial_epochs;
            snap.n_perconst_diffuse = n_perconst_diffuse_published;
            snap.iod                = iod;
            snap.prev_week          = prev_week;
            snap.prev_tow           = prev_tow;
            snap.last_emit_content_s = last_emit_content_s;
            snap.last_health_write_s = last_health_write_s;
            snap.ura_sigma0_ewma      = ura_sigma0_ewma;
            snap.ura_ewma_updates     = ura_ewma_updates;
            snap.ramp_lifted          = ramp_lifted;
            snap.first_emit_content_s = first_emit_content_s;
            snap.shared_carry.reserve(prev_carry.size());
            for (const CarryPrior& cp : prev_carry) {
                NetworkStateSnapshot::SharedCarry sc;
                sc.kind      = static_cast<int>(cp.kind);
                sc.sat       = cp.sat;
                sc.signal    = cp.signal;
                sc.sh_n      = cp.sh_n;
                sc.sh_m      = cp.sh_m;
                sc.sh_trig   = static_cast<int>(cp.sh_trig);
                sc.value     = cp.value;
                sc.var       = cp.var;
                sc.inflate_q = cp.inflate_q;
                snap.shared_carry.push_back(std::move(sc));
            }
            snap.prev_pivot    = prev_pivot;
            snap.last_pivot    = last_pivot;
            snap.c1_zoh_active = c1_zoh_active;
            snap.prev_clk      = prev_clk;
            for (const auto& [sat, o] : clock_osc) {
                NetworkStateSnapshot::Osc so;
                so.x0 = o.x0; so.x1 = o.x1;
                so.p00 = o.p00; so.p01 = o.p01; so.p11 = o.p11;
                so.t_last = o.t_last; so.init = o.init;
                snap.clock_osc[sat] = so;
            }
            for (const auto& [key, lc] : local_carry)
                snap.local_carry[key] =
                    NetworkStateSnapshot::Local{lc.value_abs, lc.var, lc.last_seen_s};
            for (const auto& [idx, lc] : zwd_carry)
                snap.zwd_carry[idx] =
                    NetworkStateSnapshot::Local{lc.value_abs, lc.var, lc.last_seen_s};
            snap.ura_refloat_horizon = ura_refloat_horizon;
            snap.ura_retro_sats      = ura_retro_sats;
            snap.retro_active        = retro_active;
            snap.relevel_iode              = relevel_iode;              // Card 6
            snap.eph_cutover_disc_pending  = eph_cutover_disc_pending;  // Card 6
            {
                std::ostringstream es;
                emitter.serializeState(es);
                snap.emitter_state = es.str();
            }
            std::string werr;
            if (!NetworkStateSnapshotIO::write(snap_path, snap, &werr))
                log->warn("Network (RTB-7): state-snapshot write to '{}' failed ({}) — the "
                          "previous good snapshot (if any) is left intact.",
                          snap_path.string(), werr);
            last_snapshot_write_s = pl_now_s;
        }
    }

    if (parallel_on) Eigen::setNbThreads(eigen_threads_saved);  // restore Eigen's own threading

    double loop_ms = 0.0;   // L3/L15: hoisted out of the timing block so the run-summary CSV / manifest can read it
    {  // [TIMING] per-epoch scaling measurement (loop wall only; startup excluded)
        loop_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_loop_begin).count();
        log->info("Network[timing]: epoch-loop wall {:.1f} ms over {} epochs = {:.2f} ms/epoch "
                  "| {} stations | {} | threads {}",
                  loop_ms, n_loop_epochs,
                  n_loop_epochs ? loop_ms / static_cast<double>(n_loop_epochs) : 0.0,
                  stations.size(), parallel_on ? "PARALLEL" : "SERIAL", n_threads);
    }

    if (trace) trace.close();
    if (soln) soln.close();
    if (iono_trace.is_open()) iono_trace.close();
    if (vtec_sh_trace.is_open()) vtec_sh_trace.close();
    if (cl_trace) cl_trace.close();
    if (if_slip_trace.is_open()) if_slip_trace.close();
    if (raim_trace.is_open()) raim_trace.close();
    if (raim_attrib_trace.is_open()) raim_attrib_trace.close();
    if (epoch_status_trace.is_open()) epoch_status_trace.close();
    if (health_trace.is_open()) health_trace.close();
    if (events_trace.is_open()) events_trace.close();
    if (zwd_trace.is_open()) zwd_trace.close();
    if (station_res_trace.is_open()) station_res_trace.close();

    log->info("Network producer complete: {} epochs emitted, {} skipped "
              "({} of those withheld by the observability gate, {} by the A4 σ̂0 ceiling) — "
              "of {} synced.",
              n_emitted, n_skipped, n_gate_withheld, n_sigma0_withheld, synced.size());
    if (clk_runaway_on && n_runaway_epochs > 0)
        log->warn("Network (RT0-5): {} epoch(s) dropped at least one constellation for clock "
                  "runaway (magnitude ceiling {:.0f} m / rate ceiling {:.2f} m/s).",
                  n_runaway_epochs, clk_runaway_mag, clk_runaway_rate);
    if (n_healed_epochs > 0)
        log->info("Network: prioritized-datum drop-and-heal recovered the gate on {} epoch(s) "
                  "({} near-null phase-bias column(s) soft-anchored).",
                  n_healed_epochs, n_healed_cols);
    if (n_gated_epochs > 0)
        log->info("Network: anchored-union coverage gate (< {} trackers) excluded thin shared "
                  "phase-bias column(s) on {} epoch(s) ({} column-epoch(s) total; their phase "
                  "rows served the satellite clock through the local blocks).",
                  config_.networkPhaseDatumMinStations(), n_gated_epochs, n_gated_key_epochs);
    if (perconst_on && (n_perconst_partial_epochs > 0 || n_perconst_diffuse_published > 0))
        log->info("Network (RTA-10): σ̂0 carve-out — {} epoch(s) published PARTIALLY (>=1 "
                  "constellation pruned), {} DIFFUSE epoch(s) published in full (every judged "
                  "constellation individually clean while the global σ̂0 tripped).",
                  n_perconst_partial_epochs, n_perconst_diffuse_published);

    // L15: append the end-of-run manifest record (JSONL — a second object on its own line).
    // Reopen-append (never rewrite) so a crashed run keeps the startup record (itself diagnostic).
    if (nc.write_run_manifest) {
        std::ofstream mf(manifest_path, std::ios::app);
        if (mf)
            mf << "{\"record\":\"complete\","
               << "\"n_synced\":" << synced.size() << ','
               << "\"n_emitted\":" << n_emitted << ','
               << "\"n_skipped\":" << n_skipped << ','
               << "\"n_gate_withheld\":" << n_gate_withheld << ','
               << "\"n_sigma0_withheld\":" << n_sigma0_withheld << ','
               << "\"n_perconst_partial_epochs\":" << n_perconst_partial_epochs << ','
               << "\"n_perconst_diffuse_published\":" << n_perconst_diffuse_published << ','
               << fmt::format("\"loop_ms\":{:.1f}", loop_ms)
               << "}\n";
    }

    // --- B6 (P1-1): serialized-contract round-trip self-check --------------------
    // Close the writer (flush the header), read it back, and cross-check the parsed
    // S-basis contract against what we serialized.  This is a LIVE checkContract call
    // site (previously dead code) that guards the write→read→consume replay path a
    // real product consumer would take: a header serialization/parse regression fails
    // here loudly instead of silently mis-declaring the datum to a downstream user.
    if (contract_pending && !prod_path.empty()) {
        // Zero epochs emitted: the deferred writer never opened, and the round-trip
        // self-check below is skipped.  The stale previous file was removed at
        // staging — unless that removal failed (locked/read-only), in which case a
        // PREVIOUS run's products still sit at the path: check, don't assert.
        if (std::filesystem::exists(prod_path))
            log->warn("Network[B6]: no epochs were emitted AND a stale product file from a "
                      "PREVIOUS run remains at '{}' (removal failed at startup) — do NOT "
                      "consume it; it is not this run's output.", prod_path.string());
        else
            log->warn("Network[B6]: no epochs were emitted — the deferred contract writer never "
                      "opened; no product file exists at '{}' (round-trip self-check skipped).",
                      prod_path.string());
        // L14: record the distinguishable "writer never opened" outcome so an absent
        // contract-check CSV unambiguously means "the check never ran".
        if (nc.serialize_sbasis_contract) {
            std::ofstream cc(log_dir_ / "network_contract_check.csv");
            if (cc) cc << "check,status,detail\n"
                          "overall,FAIL,writer never opened (zero epochs emitted)\n";
        }
    }
    if (nc.serialize_sbasis_contract && product_writer && !prod_path.empty()) {
        product_writer.reset();   // flush + close so the reader sees the complete header
        SbasisContract parsed;
        bool           rok = false;
        const std::vector<NetworkProductRecord> check_recs =
            NetworkProductReader::read(prod_path.string(), &parsed, &rok);
        // RTA-5: with re-broadcast armed, run checkContract on EVERY S receipt (each
        // record's contract_refresh), not once — the per-receipt behaviour a late-
        // joining consumer relies on.  The per-receipt expectation drops the pivot
        // map (pivots legitimately evolve across re-broadcasts; the FINAL pivot map
        // is checked separately below against the last-written contract) but keeps
        // every run-constant convention field AND the run's single generation id.
        // Gated on the toggle so an OFF run's check output is byte-identical.
        std::size_t s_receipts = 0, s_receipt_fails = 0;
        std::vector<std::string> s_receipt_mism;
        if (contract_rebroadcast_s > 0.0) {
            SbasisContract receipt_expect = pending_contract;
            receipt_expect.decl.clock_pivot_sat.clear();
            for (std::size_t ri = 0; ri < check_recs.size(); ++ri) {
                if (!check_recs[ri].contract_refresh.present) continue;
                ++s_receipts;
                std::vector<std::string> rm;
                if (!NetworkProductReader::checkContract(check_recs[ri].contract_refresh,
                                                         receipt_expect, &rm)) {
                    ++s_receipt_fails;
                    for (const auto& m : rm)
                        s_receipt_mism.push_back(fmt::format("receipt {} (epoch {}): {}",
                                                             s_receipts, ri, m));
                }
            }
            if (s_receipt_fails > 0) {
                log->warn("Network[RTA-5]: {} of {} re-broadcast S receipt(s) FAILED the "
                          "per-receipt contract check:", s_receipt_fails, s_receipts);
                for (const auto& m : s_receipt_mism) log->warn("    - {}", m);
            } else {
                log->info("Network[RTA-5]: {} S-contract receipt(s) parsed positionally; every "
                          "one passed the per-receipt contract check.", s_receipts);
            }
        }
        std::vector<std::string> mism;
        if (!parsed.present) {
            log->warn("Network[B6]: the serialized S-basis contract did not parse back from '{}' — "
                      "header round-trip FAILED.", prod_path.string());
        } else if (!NetworkProductReader::checkContract(parsed, pending_contract, &mism)) {
            log->warn("Network[B6]: serialized S-basis contract does not round-trip cleanly "
                      "({} mismatch(es)):", mism.size());
            for (const auto& m : mism) log->warn("    - {}", m);
        } else if (parsed.decl.clock_pivot_sat != pending_contract.decl.clock_pivot_sat) {
            log->warn("Network[B6]: the realized clock-pivot map did not round-trip through the header.");
        } else {
            log->info("Network[B6]: S-basis contract round-trips cleanly "
                      "(write → read → checkContract), {} realized pivot(s) serialized.",
                      parsed.decl.clock_pivot_sat.size());
        }
        // L14: machine-readable check result.  The pivot-map round-trip is checked SEPARATELY
        // (checkContract only compares the expected-side pivots, so a realized-pivot mismatch is
        // NOT in `mism`) and must fail 'overall' too.  Written on PASS as well — an ABSENT file
        // means the check never ran (crash / zero epochs), distinct from a recorded FAIL.
        const bool pivot_ok = parsed.present
            && (parsed.decl.clock_pivot_sat == pending_contract.decl.clock_pivot_sat);
        const bool overall = rok && parsed.present && mism.empty() && pivot_ok
            && (contract_rebroadcast_s <= 0.0 || s_receipt_fails == 0);
        const std::filesystem::path cc_path = log_dir_ / "network_contract_check.csv";
        std::ofstream cc(cc_path);
        if (cc) {
            cc << "check,status,detail\n";
            cc << "file_readable," << (rok ? "PASS" : "FAIL") << ",\n";
            cc << "contract_present," << (parsed.present ? "PASS" : "FAIL") << ",\n";
            for (const auto& m : mism)
                cc << "field_mismatch,FAIL," << NetworkProductReader::csvSanitize(m) << '\n';
            // RTA-5 (toggle-gated so an OFF run's CSV is byte-identical): per-receipt
            // contract-check summary across every positional S line in the file.
            if (contract_rebroadcast_s > 0.0) {
                cc << "s_receipts," << (s_receipt_fails == 0 ? "PASS" : "FAIL") << ','
                   << s_receipts << " receipt(s); " << s_receipt_fails << " failed\n";
                for (const auto& m : s_receipt_mism)
                    cc << "s_receipt_mismatch,FAIL," << NetworkProductReader::csvSanitize(m) << '\n';
            }
            cc << "pivot_map_roundtrip," << (pivot_ok ? "PASS" : "FAIL") << ",\n";
            cc << "overall," << (overall ? "PASS" : "FAIL") << ','
               << parsed.decl.clock_pivot_sat.size() << " realized pivot(s) serialized\n";
            cc.close();
        } else {
            log->warn("Network[L14]: failed to open contract-check CSV {}", cc_path.string());
        }
    }

    // --- B8: empirical false-alarm-rate summary per QC tier ------------------
    // Realized flag rate vs the implied one-sided Gaussian α for the configured k
    // (magnitude gates are two-sided, so α = P(|Z|>k) = erfc(k/√2)).  Run this on a
    // CLEAN baseline to read each tier's realized false-alarm rate.  Measurement only.
    if (trace_flag_rates) {
        auto gaussAlpha = [](double k) { return std::erfc(k / std::sqrt(2.0)); };  // P(|Z|>k)
        // L3: also stream a machine-readable network_flag_rates.csv alongside the log lines
        // (both sinks gated on the ONE existing trace_flag_rates toggle).  The two log format
        // strings below are byte-identical to before — the CSV row is appended after log->info.
        const std::filesystem::path fr_csv_path = log_dir_ / "network_flag_rates.csv";
        std::ofstream fr_csv(fr_csv_path);
        if (fr_csv)
            fr_csv << "tier,label,opportunities,flags,rate,k,implied_alpha,has_alpha\n";
        else
            log->warn("Network: failed to open flag-rate CSV {}", fr_csv_path.string());
        auto emit = [&](const char* tier, const char* label, const FlagRate& fr, double k,
                        bool has_alpha) {
            const double rate = fr.opp ? static_cast<double>(fr.flag) /
                                         static_cast<double>(fr.opp) : 0.0;
            if (has_alpha)
                log->info("  {:<24}: {:>10} opp, {:>8} flag, rate {:.3e}  (k={:.1f} => implied "
                          "Gaussian a {:.3e})", label, fr.opp, fr.flag, rate, k, gaussAlpha(k));
            else
                log->info("  {:<24}: {:>10} opp, {:>8} flag, rate {:.3e}  (absolute bound; no "
                          "Gaussian a)", label, fr.opp, fr.flag, rate);
            if (fr_csv) {
                fr_csv << tier << ',' << label << ',' << fr.opp << ',' << fr.flag << ','
                       << fmt::format("{:.6e}", rate) << ',' << fmt::format("{:.1f}", k) << ',';
                if (has_alpha) fr_csv << fmt::format("{:.6e}", gaussAlpha(k));
                fr_csv << ',' << (has_alpha ? 1 : 0) << '\n';
            }
        };
        log->info("==== Network flag-rate empirical-alpha summary (network.trace_flag_rates) ====");
        emit("T1",  "IF-phase slip (T1)",      fr_if_slip,    config_.networkCycleSlipIfPhaseTdKMad(), true);
        emit("T2",  "within-ep trigger (T2)",  fr_we_trigger, config_.networkRaimSigma0Trigger(),      true);
        emit("T3",  "post-fit RAIM cell (T3)", fr_raim_cell,  config_.networkRaimKSigma(),             true);
        emit("T3c", "RAIM consensus (T3)",     fr_raim_sat,   config_.networkRaimKSigma(),             true);
        emit("T4",  "attribution SAT (T4)",    fr_attrib,     config_.networkRaimKSigma(),             true);
        emit("T5",  "clock-model NIS (T5)",    fr_clk_model,  config_.networkClockModelKSigma(),       true);
        emit("T6",  "clock-monitor (T6)",      fr_clk_mon,    0.0,                                     false);
        log->info("=============================================================================");
        if (fr_csv) fr_csv.close();

        // L3: run-summary CSV (same toggle).  ms_per_epoch uses n_loop_epochs (loop epochs,
        // NOT n_synced — the fast-forward / max-duration epochs are excluded from the loop).
        const std::filesystem::path rs_csv_path = log_dir_ / "network_run_summary.csv";
        std::ofstream rs_csv(rs_csv_path);
        if (rs_csv) {
            rs_csv << "n_synced,n_emitted,n_skipped,n_gate_withheld,n_sigma0_withheld,loop_ms,"
                      "ms_per_epoch,n_stations,n_threads,parallel\n";
            const double ms_per_epoch = n_loop_epochs
                ? loop_ms / static_cast<double>(n_loop_epochs) : 0.0;
            rs_csv << synced.size() << ',' << n_emitted << ',' << n_skipped << ','
                   << n_gate_withheld << ',' << n_sigma0_withheld << ','
                   << fmt::format("{:.1f}", loop_ms) << ',' << fmt::format("{:.3f}", ms_per_epoch) << ','
                   << stations.size() << ',' << n_threads << ',' << (parallel_on ? 1 : 0) << '\n';
            rs_csv.close();
        } else {
            log->warn("Network: failed to open run-summary CSV {}", rs_csv_path.string());
        }
    }

    log->info("Network: shared satellite product accumulated in an owned "
              "SsrCorrectionStore (closed-loop sink). Trace: {}", trace_path.string());

    // --- Closed-loop integer-recovery summary -------------------------------
    if (closed_loop_on && consumer) {
        const double fix_rate = cl_ar_ran ? (100.0 * static_cast<double>(cl_ar_fixed)
                                             / static_cast<double>(cl_ar_ran)) : 0.0;
        const double mean_fixed = cl_ar_fixed ? (static_cast<double>(cl_sum_fixed)
                                                 / static_cast<double>(cl_ar_fixed)) : 0.0;
        // Converged-window robust stats: median (typical accuracy, spike-immune)
        // + max (surfaces wrong-fix excursions the RMS would smear).
        double conv_med = 0.0, conv_max = 0.0;
        if (!cl_recent_err3d.empty()) {
            std::vector<double> w(cl_recent_err3d.begin(), cl_recent_err3d.end());
            std::sort(w.begin(), w.end());
            conv_med = w[w.size() / 2];
            conv_max = w.back();
        }
        const Eigen::Vector3d final_err = consumer->getPosition() - cl_truth;
        const Eigen::Vector3d final_enu = Coordinates::ecefToEnu(final_err, cl_truth_llh);

        const char* product_label = !cl.consume_network_products
            ? "BASELINE (precise products only — network store NOT consumed)"
            : (cl.consume_network_clock ? "FULL (network clock + phase bias)"
                                        : "BIAS-ONLY (precise clock + network phase bias)");
        const std::string mode_label =
            std::string(cl.consumer_undifferenced ? "UNDIFFERENCED" : "BSSD")
            + " user / " + product_label;

        // Honest, fix-rate-led verdict.  WL-only AR does not fix the narrow lane,
        // so the post-fix position stays at the WL-float level (~m) even on a
        // healthy product — the WL fix RATE, not the position, is the recovery
        // metric.  Flag a marginal result and point at the A/B baseline knob so a
        // reader never reads a degraded run as a clean pass.
        std::string verdict;
        if (cl_ar_fixed == 0)
            verdict = "NO integers recovered — inspect closed_loop_trace.csv / SSR coverage";
        else if (fix_rate >= 50.0)
            verdict = fmt::format("RECOVERED INTEGERS — {:.0f}% WL fix rate from our own "
                                  "network products", fix_rate);
        else
            verdict = fmt::format("PARTIAL — {:.0f}% WL fix rate (marginal); compare with the "
                                  "precise-products baseline (network.closed_loop."
                                  "consume_network_products=false)", fix_rate);

        log->info("==== Closed-loop integer-recovery harness — held-out '{}' ====",
                  cl.station.name);
        log->info("  mode                      : {}", mode_label);
        log->info("  consumer epochs processed : {}", cl_epochs);
        log->info("  AR invoked / accepted     : {} / {}  ({:.1f}% fix rate)",
                  cl_ar_ran, cl_ar_fixed, fix_rate);
        log->info("  mean WL integers per fix  : {:.1f}", mean_fixed);
        if (cl_first_fix_tow >= 0.0)
            log->info("  first integer fix at      : week {} tow {:.1f}",
                      cl_first_fix_week, cl_first_fix_tow);
        else
            log->info("  first integer fix         : NONE — no epoch recovered integers");
        log->info("  position error (final)    : {:.3f} m 3D  (E {:.3f}, N {:.3f}, U {:.3f})",
                  final_err.norm(), final_enu.x(), final_enu.y(), final_enu.z());
        log->info("  position (last {} ep)      : median {:.3f} m, max {:.3f} m 3D "
                  "(max >> median ⇒ wrong-fix excursions)",
                  cl_recent_err3d.size(), conv_med, conv_max);
        log->info("  VERDICT                   : {}", verdict);
        log->info("  closed-loop trace         : {}",
                  (log_dir_ / "closed_loop_trace.csv").string());
        log->info("==============================================================");
    }
    if (n_emitted == 0) {
        log->error("Network: produced ZERO solution epochs. Common causes: stations "
                   "do not overlap in time; the local nuisance block is rank-"
                   "deficient (disable receiver code-bias / phase-clock estimation "
                   "for the network config); or the datum is under-specified "
                   "(check active_systems = GPS+Galileo and ≥2 overlapping stations).");
        return 1;
    }
    return 0;
}

} // namespace GnssProcessor

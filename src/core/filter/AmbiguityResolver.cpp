/**
 * @file AmbiguityResolver.cpp
 * @brief MLAMBDA integer ambiguity resolution implementation.
 *
 * Implements the Modified LAMBDA method following Chang et al. (2005)
 * and integrates it with the GnssProcessor Kalman filter for PPP-RTK
 * integer ambiguity resolution and fix-and-hold.
 */

#include "filter/AmbiguityResolver.hpp"
#include "filter/IlsCore.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/KalmanFilter.hpp"
#include "common/Constants.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>
#include <set>
#include <fmt/format.h>

namespace GnssProcessor {

// ===========================================================================
// Construction
// ===========================================================================

AmbiguityResolver::AmbiguityResolver(const AmbiguityResolverConfig& config,
                                     LogManager& log_manager)
    : config_(config), log_manager_(log_manager), ppp_utils_(log_manager) {
}

// ===========================================================================
// LDL^T decomposition
// ===========================================================================

// The MLAMBDA numerical kernels (ldlDecompose, reduction, search, mlambda,
// computeBootstrapSuccessRate) live in filter/IlsCore.hpp as pure free
// functions shared with the PPP-RTK network producer path.  These members are
// thin forwarders, so the test seam (MlambdaTestAccess) and the
// StateManager-coupled entry points below keep their call sites.  Behavior is
// bit-identical to the kernels before the extraction.
bool AmbiguityResolver::ldlDecompose(const Eigen::MatrixXd& Q,
                                     Eigen::MatrixXd& L,
                                     Eigen::VectorXd& d) {
    return ils::ldlDecompose(Q, L, d);
}

// ===========================================================================
// MLAMBDA lattice reduction (decorrelation)
// ===========================================================================

void AmbiguityResolver::reduction(Eigen::MatrixXd& L, Eigen::VectorXd& d,
                                  Eigen::MatrixXd& Z) {
    ils::reduction(L, d, Z);
}

// ===========================================================================
// Integer least-squares search (modified Schnorr-Euchner enumeration)
// ===========================================================================

int AmbiguityResolver::search(const Eigen::MatrixXd& L,
                              const Eigen::VectorXd& d,
                              const Eigen::VectorXd& zs,
                              Eigen::MatrixXd& zn,
                              Eigen::VectorXd& sq,
                              int m) {
    return ils::search(L, d, zs, zn, sq, m);
}

// ===========================================================================
// Full MLAMBDA
// ===========================================================================

bool AmbiguityResolver::mlambda(const Eigen::VectorXd& a,
                                const Eigen::MatrixXd& Q,
                                Eigen::VectorXd& afix,
                                Eigen::VectorXd& s,
                                Eigen::VectorXd* d_out) {
    return ils::mlambda(a, Q, afix, s, d_out);
}

// ===========================================================================
// Bootstrap-rounding success rate
// ===========================================================================

double AmbiguityResolver::computeBootstrapSuccessRate(const Eigen::VectorXd& d) {
    return ils::computeBootstrapSuccessRate(d);
}

// ===========================================================================
// Partial ambiguity resolution (greedy backward elimination)
// ===========================================================================

AmbiguityResolver::PartialFix AmbiguityResolver::selectFixableSubset_(
    const Eigen::VectorXd& y_cycles,
    const Eigen::MatrixXd& Q_cycles) const {

    // The backward-elimination loop lives in filter/IlsCore.hpp as
    // ils::selectFixableSubset, shared with the network producer's ilsFix.
    // This member is a thin adapter that maps config_ → IlsGateConfig and the
    // returned ils::PartialFix → AmbiguityResolver::PartialFix.  Bit-identical
    // to the pre-extraction loop (verbatim port, same gate values).
    const ils::IlsGateConfig gates{
        config_.ratio_threshold,
        config_.ratio_upper_bound,
        config_.min_success_rate,
        config_.nl_max_per_pair_frac,
        config_.enable_partial_ar,
    };
    const ils::PartialFix pf = ils::selectFixableSubset(y_cycles, Q_cycles, gates);

    PartialFix out;
    out.success     = pf.success;
    out.kept        = pf.kept;
    out.afix_cycles = pf.afix_cycles;
    out.ratio       = pf.ratio;
    out.p_s         = pf.p_s;
    out.max_frac    = pf.max_frac;
    return out;
}

// ===========================================================================
// Diagnostic: conditioning of the DD ambiguity covariance
//
// qbCondition() reports the eigenvalue spread of the ambiguity covariance in
// the cycle domain, the matrix MLAMBDA searches (the DD block, or the NL-space
// matrix on the WL/NL path).  The fix sites evaluate it before the fix and on
// the block rebuilt from the fixed covariance, which shows whether applying
// the integer fix as a Joseph-form pseudo-observation leaves Qb well-conditioned
// for the next epoch (the intent stated at the fix sites) or collapses it as
// the back-substitution form does.  Motivation: a smaller hold_variance
// (1e-3 to 1e-7) gave fewer fixed epochs and a divergent kinematic solution,
// both signatures of a fix that drives the covariance singular.
//
// Read-only: the eigenvalues are computed on a copy.  The probe is not gated by
// log level: the pre-fix matrix is decomposed before every search, the rebuilt
// matrix after every successful fix, and each fix logs one QBCOND line at info
// level.
// ===========================================================================
namespace {

struct QbCond {
    double cond   = 0.0;   ///< lambda_max / lambda_min of Qb in cycles^2
    double lo     = 0.0;   ///< smallest eigenvalue (cycles^2)
    double hi     = 0.0;   ///< largest eigenvalue  (cycles^2)
    double sig_lo = 0.0;   ///< sqrt(lo): sigma of the tightest DD direction (cycles)
    double sig_hi = 0.0;   ///< sqrt(hi): sigma of the loosest DD direction (cycles)
};

/// Eigen-decompose the (symmetric) cycle-domain DD ambiguity covariance.
/// Returns all zeros on an empty or non-finite matrix.
QbCond qbCondition(const Eigen::MatrixXd& Qb_cycles) {
    QbCond c;
    const Eigen::Index n = Qb_cycles.rows();
    if (n <= 0 || !Qb_cycles.allFinite()) return c;
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(
        0.5 * (Qb_cycles + Qb_cycles.transpose()), Eigen::EigenvaluesOnly);
    if (es.info() != Eigen::Success) return c;
    const Eigen::VectorXd ev = es.eigenvalues();
    c.lo = ev.minCoeff();
    c.hi = ev.maxCoeff();
    c.cond   = (c.lo > 0.0) ? (c.hi / c.lo) : std::numeric_limits<double>::infinity();
    c.sig_lo = (c.lo > 0.0) ? std::sqrt(c.lo) : 0.0;
    c.sig_hi = (c.hi > 0.0) ? std::sqrt(c.hi) : 0.0;
    return c;
}

/// Rebuild the cycle-domain DD ambiguity covariance from a full state
/// covariance with the differencing and lambda scaling that resolve() uses.
/// The post-fix Qb comes from here: the fix updates the full P, so the DD block
/// is re-extracted to compare with the pre-fix matrix.
template <typename PairVec>
Eigen::MatrixXd rebuildQbCycles(const Eigen::MatrixXd& P,
                                const PairVec& pairs,
                                const std::vector<double>& pair_lambda) {
    const int nb = static_cast<int>(pairs.size());
    Eigen::MatrixXd Q(nb, nb);
    for (int i = 0; i < nb; ++i) {
        for (int j = 0; j < nb; ++j) {
            const int ii = pairs[i].state_idx_i,  ir = pairs[i].state_idx_ref;
            const int ji = pairs[j].state_idx_i,  jr = pairs[j].state_idx_ref;
            Q(i, j) = (P(ii, ji) - P(ii, jr) - P(ir, ji) + P(ir, jr))
                      / (pair_lambda[i] * pair_lambda[j]);
        }
    }
    return 0.5 * (Q + Q.transpose());
}

}  // namespace

// ===========================================================================
// Per-ambiguity arc age and information (maturity gate and diagnostics)
// ===========================================================================

void AmbiguityResolver::refreshArcAges_(const StateManager& state_manager,
                                        const std::vector<PppObservation>& observations) {
    // satellite -> elevation in degrees (the same field the AR elevation mask uses).
    std::map<std::string, double> sat_elev;
    for (const auto& obs : observations) sat_elev[obs.satellite] = obs.elevation;

    const auto& amb = state_manager.getAmbiguities();
    std::set<std::string> present;
    for (const auto& [key, idx] : amb) {
        (void)idx;
        const auto parts = StateKeys::parseAmbiguityKey(key);
        if (!parts) continue;
        const std::string k = parts->satellite + "|" + parts->signal;
        present.insert(k);
        ArcState& a = arcs_[k];

        // A refloat (resetAmbiguity()) re-inflates the variance but keeps the key,
        // so counting presence alone would carry the pre-slip age across the
        // discontinuity and report a new arc as mature.  The pending-reseed mark
        // is the only in-place signal of a refloat, so the arc restarts on it.
        if (state_manager.isAmbiguityPendingReseed(key)) a = ArcState{};

        ++a.epochs;
        const auto it = sat_elev.find(parts->satellite);
        if (it != sat_elev.end() && it->second > 0.0) {
            // Same sin(el) form as the elevation stochastic model
            // (StochasticModel.cpp:127), so `info` is the accumulated
            // unit-weight code information under the weighting in use.
            const double s = std::sin(it->second * Constants::PI / 180.0);
            a.info += s * s;
        }
    }
    // Keys that left the state vector are dropped: a re-acquired ambiguity
    // starts a new arc instead of resuming its pre-gap count.
    for (auto it = arcs_.begin(); it != arcs_.end();)
        it = present.count(it->first) ? std::next(it) : arcs_.erase(it);
}

int AmbiguityResolver::arcAgeOf_(const std::string& sat,
                                 const std::string& signal) const {
    const auto it = arcs_.find(sat + "|" + signal);
    return (it == arcs_.end()) ? 0 : it->second.epochs;
}

double AmbiguityResolver::arcInfoOf_(const std::string& sat,
                                     const std::string& signal) const {
    const auto it = arcs_.find(sat + "|" + signal);
    return (it == arcs_.end()) ? 0.0 : it->second.info;
}

// ===========================================================================
// DD pair construction
// ===========================================================================

std::vector<AmbiguityResolver::DdPair> AmbiguityResolver::buildDdPairs(
    const StateManager& state_manager,
    const std::vector<PppObservation>& observations) const {

    // Build satellite -> elevation map
    std::map<std::string, double> sat_elev;
    for (const auto& obs : observations) {
        sat_elev[obs.satellite] = obs.elevation;
    }

    // Get all ambiguity keys and group by (constellation, signal)
    const auto& amb_map = state_manager.getAmbiguities();

    // Parse ambiguity key format: "G01_L1C" -> satellite="G01", signal="L1C"
    struct AmbInfo {
        std::string key;
        std::string satellite;
        std::string signal;
        std::string constellation; // first char of satellite
        int state_idx;
        double elevation;
    };
    std::vector<AmbInfo> ambs;

    // Hoist the full-state copies out of the per-ambiguity loop.  Both
    // getStateVector and getCovariance return by value; doing them once
    // per ambiguity is O(n) and O(n²) per call respectively, which adds
    // up on a state with many ambiguities even though AR only runs once
    // per epoch.
    const Eigen::VectorXd sv = state_manager.getStateVector();
    const Eigen::MatrixXd P  = state_manager.getCovariance();

    for (const auto& [key, idx] : amb_map) {
        // Skip any non-PPP keys that may share the ambiguity map (e.g.
        // baseline DD entries in mixed configurations).  The PPP parser
        // splits on the first underscore, so iono-free combinations
        // ("L1C_L5Q") stay whole in the signal field.
        const auto parsed = StateKeys::parseAmbiguityKey(key);
        if (!parsed) continue;

        AmbInfo info;
        info.key = key;
        info.satellite = parsed->satellite;
        info.signal = parsed->signal;
        info.constellation = info.satellite.substr(0, 1);
        info.state_idx = static_cast<int>(idx);

        // Two separate filters with documented intent:
        //   (1) value == 0  → uninitialized seed.  MeasurementModel::ambiguitySeed()
        //       returns 0 when the obs/modeled maps don't have the signal,
        //       so the float ambiguity hasn't yet been informed by data.
        //       LAMBDA candidate would be undefined.
        //   (2) variance == 0 → frozen BSSD pivot.  The pivot's row/col
        //       are zeroed by PivotManager::freezePivotAmbiguityCovariance
        //       and the value is set to exactly 0 by the SD reference.  A
        //       state with zero variance can't be decorrelated; LAMBDA
        //       would divide by zero or produce a degenerate Q matrix.
        // Both are robust under exact float arithmetic: the values are
        // assigned via setZero() and don't drift (KF gain·residual is
        // zero when the covariance row/col are zero).  Listing both checks
        // documents intent and survives future changes that might leave
        // one or the other non-zero.
        if (info.state_idx >= sv.size()) continue;
        if (sv(info.state_idx) == 0.0) continue;          // uninitialized seed
        if (P(info.state_idx, info.state_idx) == 0.0) continue;  // frozen pivot

        auto it = sat_elev.find(info.satellite);
        info.elevation = (it != sat_elev.end()) ? it->second : 0.0;

        // Skip below AR elevation mask
        if (info.elevation < config_.elevation_mask_ar) continue;

        // Skip unresolvable ambiguities: missing BIA/ATX coverage means
        // the float value absorbs a time-constant bias and cannot be
        // resolved to the correct integer.
        if (state_manager.isAmbiguityUnresolvable(key)) continue;

        // Per-constellation AR filter.  When @c ar_active_systems is
        // non-empty, ambiguities outside the listed systems are skipped.
        // They remain in the float Kalman filter; they're just excluded
        // from the LAMBDA candidate set.
        if (!config_.ar_active_systems.empty()) {
            const std::string sys = PppUtils::getSystemString(info.satellite);
            if (std::find(config_.ar_active_systems.begin(),
                          config_.ar_active_systems.end(),
                          sys) == config_.ar_active_systems.end()) {
                continue;
            }
        }

        ambs.push_back(std::move(info));
    }

    // Group by (constellation, signal)
    std::map<std::string, std::vector<size_t>> groups;
    for (size_t i = 0; i < ambs.size(); ++i) {
        groups[ambs[i].constellation + "|" + ambs[i].signal].push_back(i);
    }

    // Build DD pairs: for each group, select highest-elevation as reference
    std::vector<DdPair> pairs;
    for (const auto& [gk, indices] : groups) {
        if (indices.size() < 2) continue;

        // Find pivot (highest elevation)
        size_t ref_idx = indices[0];
        for (size_t idx : indices) {
            if (ambs[idx].elevation > ambs[ref_idx].elevation) {
                ref_idx = idx;
            }
        }

        // Create DD pairs: each non-pivot vs pivot
        for (size_t idx : indices) {
            if (idx == ref_idx) continue;
            DdPair pair;
            pair.state_idx_i = ambs[idx].state_idx;
            pair.state_idx_ref = ambs[ref_idx].state_idx;
            pair.sat_i = ambs[idx].satellite;
            pair.sat_ref = ambs[ref_idx].satellite;
            pair.signal = ambs[idx].signal;
            pairs.push_back(pair);
        }
    }

    return pairs;
}

// ===========================================================================
// Resolve without a WL constraint (forwards to the overload with an empty map)
// ===========================================================================

AmbiguityResolution AmbiguityResolver::resolve(
    const StateManager& state_manager,
    const std::vector<PppObservation>& observations) {
    return resolve(state_manager, observations, {});
}

// ===========================================================================
// Resolve (with optional WL constraint)
// ===========================================================================

AmbiguityResolution AmbiguityResolver::resolve(
    const StateManager& state_manager,
    const std::vector<PppObservation>& observations,
    const std::map<std::string, int>& wl_ambiguities,
    const std::map<std::string, double>& wl_fractions,
    const std::map<std::string, WlArcQuality>& wl_quality) {

    auto log = log_manager_.getLogger("ambiguity_resolver");
    AmbiguityResolution result;

    // Update arc ages before the ar_mode gate, so a float-only run (ar_mode 0)
    // keeps the same age series as an AR run and an AR-on/AR-off A/B can be
    // compared on it.
    refreshArcAges_(state_manager, observations);

    if (config_.ar_mode == 0) {
        log->debug("AR disabled (ar_mode=0)");
        result.gate_reason = 1;   // disabled (T2c)
        return result;
    }

    // Convergence guard 1: minimum-epochs gate.  AR runs LAMBDA on the float
    // covariance.  A marginal fix accepted before that covariance has settled
    // tends to lock onto wrong integers; once a wrong integer is constrained
    // into the state, the next LAMBDA call sees a degenerate covariance
    // direction and returns inflated, meaningless ratios.  Skip the first
    // @c min_epochs invocations.
    ++epoch_count_;
    if (epoch_count_ <= config_.min_epochs) {
        log->debug("AR skipped: epoch {}/{} (min_epochs convergence guard)",
                   epoch_count_, config_.min_epochs);
        result.gate_reason = 2;   // min_epochs guard (T2c)
        return result;
    }

    // NlWl basis: skip the per-band DD-pair and LAMBDA pipeline.  The state
    // vector holds NL/WL ambiguities directly, so there are no per-(sat, signal)
    // DD pairs to enumerate.  This path applies MW-resolved WL integers as
    // 1-row pseudo-observations and stops there; a full LAMBDA NL search with
    // the WL pre-constrained is not implemented (see the CLAUDE.md notes on the
    // WL-fix-with-tight-coupling milestone).
    if (config_.state_ambiguity_basis == StateAmbiguityBasis::NlWl) {
        const Eigen::VectorXd x = state_manager.getStateVector();
        const Eigen::MatrixXd P = state_manager.getCovariance();
        if (P.rows() >= 3) {
            const double sigma_3d = std::sqrt(
                std::max(P(0, 0), 0.0) + std::max(P(1, 1), 0.0) + std::max(P(2, 2), 0.0));
            if (sigma_3d > config_.max_position_sigma_m) {
                log->debug("AR (NlWl) skipped: 3D position σ={:.4f} m > {:.4f} m (sigma gate)",
                           sigma_3d, config_.max_position_sigma_m);
                result.gate_reason = 3;   // position-σ gate (T2c)
                result.gate_sigma_3d = sigma_3d;
                return result;
            }
        }
        return applyMwWlConstraintsNlWl_(state_manager, x, P,
                                          wl_ambiguities, wl_fractions, observations,
                                          wl_quality);
    }

    // Build DD pairs
    auto pairs = buildDdPairs(state_manager, observations);
    result.num_candidates = static_cast<int>(pairs.size());

    // --- Closed-loop frame diagnostic.  Runs only when the ambiguity_resolver
    // logger is at debug level, like the debug dumps below, so output is
    // byte-identical otherwise.  Logs the between-satellite SD float ambiguity
    // fraction (cycles) for every pair here, ahead of the position-σ gate: that
    // gate returns before the metre-to-cycle conversion, so without this block
    // the half-cycle clustering test never runs on a float that has not
    // converged.  If the injected network satellite phase-bias frame is off by
    // half a cycle, these fractions cluster near ±0.5 instead of 0.
    // See docs / project_closedloop_consumer_vertical_bug.
    if (!pairs.empty() && log->should_log(spdlog::level::debug)) {
        const Eigen::VectorXd x_dbg = state_manager.getStateVector();
        std::map<std::string, const PppObservation*> obs_dbg;
        for (const auto& o : observations) obs_dbg[o.satellite] = &o;
        double sum_abs = 0.0; int n_dbg = 0;
        for (const auto& pr : pairs) {
            auto it = obs_dbg.find(pr.sat_i);
            if (it == obs_dbg.end()) continue;
            const double lam = ppp_utils_.getWavelength(it->second->system,
                                                        pr.signal, pr.sat_i);
            if (!(std::isfinite(lam) && lam > 0.0)) continue;
            if (pr.state_idx_i >= x_dbg.size() || pr.state_idx_ref >= x_dbg.size()) continue;
            const double sd_cyc = (x_dbg(pr.state_idx_i) - x_dbg(pr.state_idx_ref)) / lam;
            const double frac   = sd_cyc - std::round(sd_cyc);
            log->debug("CLFRAC sat_i={} sat_ref={} sig={} sd_cyc={:.4f} frac={:+.4f}",
                       pr.sat_i, pr.sat_ref, pr.signal, sd_cyc, frac);
            sum_abs += std::abs(frac); ++n_dbg;
        }
        if (n_dbg > 0) {
            log->debug("CLFRAC_SUMMARY n={} mean_abs_frac={:.4f}", n_dbg, sum_abs / n_dbg);
        }
    }

    if (pairs.empty()) {
        log->debug("No DD ambiguity pairs available for AR");
        result.gate_reason = 4;   // no candidates (T2c)
        return result;
    }

    const Eigen::VectorXd x = state_manager.getStateVector();
    const Eigen::MatrixXd P = state_manager.getCovariance();

    // Convergence guard 2: formal position-σ gate.  Complements the
    // min-epochs gate with the filter's own confidence instead of an epoch
    // count.  When the 3D position σ is wide, the ambiguity covariance is
    // usually still wide too and the integer search is not reliable.
    if (P.rows() >= 3) {
        const double sigma_3d = std::sqrt(
            std::max(P(0, 0), 0.0) + std::max(P(1, 1), 0.0) + std::max(P(2, 2), 0.0));
        if (sigma_3d > config_.max_position_sigma_m) {
            log->debug("AR skipped: 3D position σ={:.4f} m > {:.4f} m (sigma gate)",
                       sigma_3d, config_.max_position_sigma_m);
            result.gate_reason = 3;   // position-σ gate (T2c)
            result.gate_sigma_3d = sigma_3d;
            return result;
        }
    }

    // WL-only path: apply MW-resolved WL integers as Kalman pseudo-observations
    // and return without any LAMBDA NL search.  Intended as a conservative
    // first stage that checks the WL pipeline before full WL+NL LAMBDA fixes
    // are enabled.  With this mode on, the LAMBDA paths are never reached.
    if (config_.wl_only) {
        if (wl_ambiguities.empty()) {
            log->debug("WL-only mode: no MW-resolved WL integers available this epoch");
            result.gate_reason = 4;   // no candidates: no WL integers (T2c)
            return result;
        }
        auto wl_only_result = resolveWlOnly(pairs, x, P, wl_ambiguities,
                                            wl_fractions, observations, wl_quality);
        wl_only_result.num_candidates = result.num_candidates;
        return wl_only_result;
    }

    // Try WL-constrained NL resolution first when WL integers are available
    if (!wl_ambiguities.empty()) {
        auto wl_result = resolveWlConstrained(pairs, x, P, wl_ambiguities, observations);
        if (wl_result.success) {
            // Upper-bound ratio check: a ratio above this indicates a
            // degenerate covariance direction (often from a previous wrong-
            // integer hold).  The fix looks ultra-confident but is suspect.
            if (wl_result.ratio > config_.ratio_upper_bound) {
                log->warn("AR rejecting WL-constrained fix: ratio={:.3e} > upper bound {:.1e} "
                          "(degenerate covariance — staying float).",
                          wl_result.ratio, config_.ratio_upper_bound);
                wl_result.success = false;
            }
            wl_result.num_candidates = result.num_candidates;
            return wl_result;
        }
        // WL-constrained AR failed: stay float.
        //
        // Do not fall through to the unconstrained LAMBDA fallback below.  That
        // fallback searches the full per-band DD set, which includes DDs whose
        // WL is not in @c wl_ambiguities (no MW lock, |frac| above the cap, or a
        // BIA gap on the WL pair-mate).  Searching those DDs lets LAMBDA pick
        // integer combinations whose implied (N_1 − N_2) contradicts the WL that
        // MW locked, and a wrong-WL fix is worse than no fix.
        //
        // The WL is the easier integer to fix (long λ_WL, robust under BSSD).  A
        // satellite whose WL is not locked cannot have its NL fixed reliably
        // either, because both depend on the same models (BIA, antenna,
        // wind-up).  The rule is therefore no NL AR for satellites whose WL is
        // float, and the unconstrained LAMBDA would break it.
        log->debug("WL-constrained AR failed; skipping unconstrained LAMBDA "
                   "fallback (MW WL info available — won't second-guess it)");
        return result;
    }

    const int nb = static_cast<int>(pairs.size());
    const int na = static_cast<int>(x.size()); // full state size

    // Extract DD ambiguity float vector: y_dd = x[i] - x[ref]
    Eigen::VectorXd y_dd(nb);
    for (int k = 0; k < nb; ++k) {
        y_dd(k) = x(pairs[k].state_idx_i) - x(pairs[k].state_idx_ref);
    }

    // Extract DD ambiguity covariance: Qb = D * P_amb * D^T
    // where D selects (row_i - row_ref) from the ambiguity block
    Eigen::MatrixXd Qb(nb, nb);
    for (int i = 0; i < nb; ++i) {
        for (int j = 0; j < nb; ++j) {
            int ii = pairs[i].state_idx_i;
            int ir = pairs[i].state_idx_ref;
            int ji = pairs[j].state_idx_i;
            int jr = pairs[j].state_idx_ref;
            Qb(i, j) = P(ii, ji) - P(ii, jr) - P(ir, ji) + P(ir, jr);
        }
    }

    // Ensure Qb is symmetric positive definite
    Qb = 0.5 * (Qb + Qb.transpose());

    // Cross-covariance between non-ambiguity states and DD ambiguities
    // Qab(k, dd) = P(k, i) - P(k, ref)  for k in [0, na)
    Eigen::MatrixXd Qab(na, nb);
    for (int k = 0; k < na; ++k) {
        for (int dd = 0; dd < nb; ++dd) {
            Qab(k, dd) = P(k, pairs[dd].state_idx_i) - P(k, pairs[dd].state_idx_ref);
        }
    }

    // Convert meters → cycles for MLAMBDA.  Integer ambiguities are
    // integer-cycle quantities; running MLAMBDA on the meter-domain
    // floats rounds to integer-meter values that have no carrier-phase
    // semantics and produces ratios near 1.0 regardless of how well-
    // converged the float is.  Per-pair λ is looked up from the
    // observation's system + signal so GLONASS FDMA's per-satellite
    // wavelength is handled correctly.
    std::map<std::string, const PppObservation*> obs_map;
    for (const auto& obs : observations) obs_map[obs.satellite] = &obs;

    std::vector<double> pair_lambda(nb);
    for (int k = 0; k < nb; ++k) {
        auto it = obs_map.find(pairs[k].sat_i);
        if (it == obs_map.end()) {
            log->debug("MLAMBDA: no obs for sat {}, skipping AR", pairs[k].sat_i);
            return result;
        }
        const double lam = ppp_utils_.getWavelength(it->second->system,
                                                     pairs[k].signal,
                                                     pairs[k].sat_i);
        if (!(std::isfinite(lam) && lam > 0.0)) {
            log->debug("MLAMBDA: invalid λ for {} {}, skipping AR",
                       pairs[k].sat_i, pairs[k].signal);
            return result;
        }
        pair_lambda[k] = lam;
    }

    Eigen::VectorXd y_cycles(nb);
    Eigen::MatrixXd Qb_cycles(nb, nb);
    for (int i = 0; i < nb; ++i) {
        y_cycles(i) = y_dd(i) / pair_lambda[i];
        for (int j = 0; j < nb; ++j) {
            Qb_cycles(i, j) = Qb(i, j) / (pair_lambda[i] * pair_lambda[j]);
        }
    }

    // Diagnostic: pre-fix conditioning of the matrix MLAMBDA is about to search.
    const QbCond qb_pre = qbCondition(Qb_cycles);

    // Run MLAMBDA on cycle-domain inputs.
    Eigen::VectorXd afix_cycles;
    Eigen::VectorXd s;
    Eigen::VectorXd d_lambda;
    if (!mlambda(y_cycles, Qb_cycles, afix_cycles, s, &d_lambda)) {
        log->debug("MLAMBDA failed (matrix not positive definite or search failed)");
        return result;
    }

    // Ratio test
    if (s(0) <= 0.0 || s(1) <= 0.0) {
        log->debug("MLAMBDA: invalid squared distances s1={:.6f} s2={:.6f}", s(0), s(1));
        return result;
    }

    result.ratio = s(1) / s(0);
    log->info("MLAMBDA: {} DD ambs, ratio={:.2f} (threshold={:.1f}), s1={:.4f} s2={:.4f}",
              nb, result.ratio, config_.ratio_threshold, s(0), s(1));

    // Full-set acceptance battery.  Each failing gate logs its reason and sets
    // full_ok=false; on failure the resolver tries a partial-subset fix (when
    // enabled) or stays float.  p_s and max_dd_frac are hoisted so the
    // full-set success log below can reuse them.
    double p_s         = 0.0;
    double max_dd_frac = 0.0;
    int    worst_dd_k  = -1;
    bool   full_ok     = true;

    if (result.ratio < config_.ratio_threshold) {
        log->info("AR ratio test failed: {:.2f} < {:.1f}", result.ratio, config_.ratio_threshold);
        full_ok = false;
    } else if (result.ratio > config_.ratio_upper_bound) {
        log->warn("AR rejecting fix: ratio={:.3e} > upper bound {:.1e} "
                  "(degenerate covariance — staying float).",
                  result.ratio, config_.ratio_upper_bound);
        full_ok = false;
    } else {
        // FFR gate: bootstrap success rate.
        p_s = computeBootstrapSuccessRate(d_lambda);
        if (p_s < config_.min_success_rate) {
            log->info("AR rejecting fix despite passing ratio ({:.2f}): "
                      "bootstrap P_s={:.5f} < min_success_rate={:.4f}",
                      result.ratio, p_s, config_.min_success_rate);
            full_ok = false;
        } else {
            // Per-DD residual cap: refuse the fix if any individual DD's
            // |float − fixed| (in cycles) exceeds the cap.
            for (int k = 0; k < nb; ++k) {
                const double per_dd = std::abs(y_cycles(k) - afix_cycles(k));
                if (per_dd > max_dd_frac) {
                    max_dd_frac = per_dd;
                    worst_dd_k  = k;
                }
            }
            if (max_dd_frac > config_.nl_max_per_pair_frac) {
                log->info("AR rejecting fix despite passing ratio ({:.2f}) and P_s={:.5f}: "
                          "DD {}-{} {} has |frac|={:.3f} cyc > nl_max_per_pair_frac={:.3f}",
                          result.ratio, p_s,
                          pairs[worst_dd_k].sat_i, pairs[worst_dd_k].sat_ref,
                          pairs[worst_dd_k].signal,
                          max_dd_frac, config_.nl_max_per_pair_frac);
                full_ok = false;
            }
        }
    }

    if (!full_ok) {
        if (!config_.enable_partial_ar) return result;

        // Partial AR: the full set failed its gates, so retry on the first
        // subset, in greedy backward elimination, that clears the same battery.
        // The survivors map back to DD pairs and are applied as SD
        // pseudo-observations as in the full-set path below, with fewer rows.
        PartialFix pf = selectFixableSubset_(y_cycles, Qb_cycles);
        if (!pf.success) {
            log->info("Partial AR: no fixable subset found ({} DD candidates); staying float", nb);
            // Diagnostic: conditioning at rejection.  Read with the accept-path
            // lines, it shows whether rejections correlate with a collapsed Qb.
            log->info("QBCOND path=reject nb={} nfix=0 R={:.3e} "
                      "pre_cond={:.4e} pre_sig_lo={:.4e} pre_sig_hi={:.4e} "
                      "post_cond=0 post_sig_lo=0 post_sig_hi=0 "
                      "cond_ratio=0 siglo_ratio=0 ratio=0",
                      nb, config_.hold_variance,
                      qb_pre.cond, qb_pre.sig_lo, qb_pre.sig_hi);
            return result;
        }

        const int m = static_cast<int>(pf.kept.size());
        Eigen::MatrixXd H = Eigen::MatrixXd::Zero(m, na);
        Eigen::VectorXd v = Eigen::VectorXd::Zero(m);
        for (int a = 0; a < m; ++a) {
            const int k = pf.kept[a];
            const double afix_m = pf.afix_cycles(a) * pair_lambda[k];
            if (pairs[k].state_idx_i   >= 0 && pairs[k].state_idx_i   < na) H(a, pairs[k].state_idx_i)   = +1.0;
            if (pairs[k].state_idx_ref >= 0 && pairs[k].state_idx_ref < na) H(a, pairs[k].state_idx_ref) = -1.0;
            v(a) = afix_m - y_dd(k);
            result.fixed_dd_rows.push_back({ pairs[k].state_idx_i, pairs[k].state_idx_ref });
            if (log->should_log(spdlog::level::debug)) {
                log->debug("  [PAR] DD {}-{} {}: float={:.3f} cyc fixed={} cyc diff={:+.4f} cyc (λ={:.4f} m)",
                           pairs[k].sat_i, pairs[k].sat_ref, pairs[k].signal,
                           y_cycles(k), static_cast<int>(std::round(pf.afix_cycles(a))),
                           y_cycles(k) - pf.afix_cycles(a), pair_lambda[k]);
            }
        }
        Eigen::MatrixXd R = Eigen::MatrixXd::Identity(m, m) * config_.hold_variance;
        KalmanFilter::josephPseudoUpdate(x, P, H, v, R,
                                         result.fixed_state, result.fixed_covariance);
        result.success   = true;
        result.num_fixed = m;
        result.ratio     = pf.ratio;
        result.p_succ    = pf.p_s;
        log->info("Partial AR succeeded (Kalman pseudo-obs, R={:.4f} m²): {}/{} ambiguities fixed, "
                  "ratio={:.2f}, P_s={:.5f}, max|frac|={:.3f} cyc",
                  config_.hold_variance, m, nb, pf.ratio, pf.p_s, pf.max_frac);
        {   // Diagnostic: Qb conditioning across the fix (partial-AR path)
            const QbCond qb_post =
                qbCondition(rebuildQbCycles(result.fixed_covariance, pairs, pair_lambda));
            log->info("QBCOND path=partial nb={} nfix={} R={:.3e} "
                      "pre_cond={:.4e} pre_sig_lo={:.4e} pre_sig_hi={:.4e} "
                      "post_cond={:.4e} post_sig_lo={:.4e} post_sig_hi={:.4e} "
                      "cond_ratio={:.4e} siglo_ratio={:.4e} ratio={:.2f}",
                      nb, m, config_.hold_variance,
                      qb_pre.cond, qb_pre.sig_lo, qb_pre.sig_hi,
                      qb_post.cond, qb_post.sig_lo, qb_post.sig_hi,
                      (qb_pre.cond > 0.0 ? qb_post.cond / qb_pre.cond : 0.0),
                      (qb_pre.sig_lo > 0.0 ? qb_post.sig_lo / qb_pre.sig_lo : 0.0),
                      pf.ratio);
        }
        return result;
    }

    // Convert the integer fix back to meters for the state-space Kalman
    // pseudo-observation update (state is in meters; afix_cycles came
    // from the cycle-domain MLAMBDA above).
    Eigen::VectorXd afix(nb);
    for (int k = 0; k < nb; ++k) {
        afix(k) = afix_cycles(k) * pair_lambda[k];
    }

    if (log->should_log(spdlog::level::debug)) {
        for (int k = 0; k < nb; ++k) {
            log->debug("  DD {}-{} {}: float={:.3f} cyc fixed={} cyc diff={:+.4f} cyc (λ={:.4f} m)",
                       pairs[k].sat_i, pairs[k].sat_ref, pairs[k].signal,
                       y_cycles(k), static_cast<int>(std::round(afix_cycles(k))),
                       y_cycles(k) - afix_cycles(k), pair_lambda[k]);
        }
    }

    // Fix succeeded: apply the integer fix as a Kalman pseudo-observation
    // update rather than by back-substitution.
    //
    // The back-substitution form
    //     x_fixed = x − Qab · Qb⁻¹ · (y_dd − afix)
    //     P_fixed = P  − Qab · Qb⁻¹ · Qab'
    // is a perfect-measurement (R = 0) update.  After it, the ambiguity
    // rows of P_fixed have near-zero variance, so the next epoch's LAMBDA
    // call sees a near-singular Qb in the integer search and returns
    // meaningless ratios that keep the filter in the upper-bound rejection
    // guard.  A Joseph-form update with R = hold_variance keeps the
    // covariance well-conditioned and lets later epochs either re-confirm
    // the fix (converging toward the integer) or detect a slip through the
    // residual.
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(nb, na);
    Eigen::VectorXd v = Eigen::VectorXd::Zero(nb);
    for (int k = 0; k < nb; ++k) {
        if (pairs[k].state_idx_i   >= 0 && pairs[k].state_idx_i   < na) H(k, pairs[k].state_idx_i)   = +1.0;
        if (pairs[k].state_idx_ref >= 0 && pairs[k].state_idx_ref < na) H(k, pairs[k].state_idx_ref) = -1.0;
        v(k) = afix(k) - y_dd(k);
        result.fixed_dd_rows.push_back({ pairs[k].state_idx_i, pairs[k].state_idx_ref });
    }

    Eigen::MatrixXd R   = Eigen::MatrixXd::Identity(nb, nb) * config_.hold_variance;
    KalmanFilter::josephPseudoUpdate(x, P, H, v, R,
                                     result.fixed_state, result.fixed_covariance);

    result.success = true;
    result.num_fixed = nb;
    result.p_succ  = p_s;

    log->info("AR succeeded (Kalman pseudo-obs, R={:.4f} m²): {} ambiguities fixed, "
              "ratio={:.2f}, P_s={:.5f}, max|frac|={:.3f} cyc",
              config_.hold_variance, nb, result.ratio, p_s, max_dd_frac);

    {   // Diagnostic: Qb conditioning across the fix (full-set path)
        const QbCond qb_post =
            qbCondition(rebuildQbCycles(result.fixed_covariance, pairs, pair_lambda));
        log->info("QBCOND path=full nb={} nfix={} R={:.3e} "
                  "pre_cond={:.4e} pre_sig_lo={:.4e} pre_sig_hi={:.4e} "
                  "post_cond={:.4e} post_sig_lo={:.4e} post_sig_hi={:.4e} "
                  "cond_ratio={:.4e} siglo_ratio={:.4e} ratio={:.2f}",
                  nb, nb, config_.hold_variance,
                  qb_pre.cond, qb_pre.sig_lo, qb_pre.sig_hi,
                  qb_post.cond, qb_post.sig_lo, qb_post.sig_hi,
                  (qb_pre.cond > 0.0 ? qb_post.cond / qb_pre.cond : 0.0),
                  (qb_pre.sig_lo > 0.0 ? qb_post.sig_lo / qb_pre.sig_lo : 0.0),
                  result.ratio);
    }

    return result;
}

// ===========================================================================
// Resolve (baseline / DD path)
//
// The state vector already holds DD ambiguity floats keyed
// "DD|cons|sat_i|rov_code|pivot".  The MLAMBDA core expects integer-
// valued targets, so meter-valued floats are converted to cycles by
// dividing by the per-pair carrier wavelength before the search, and
// the integer fix is multiplied back to meters for the Kalman
// pseudo-observation update.  No SD subtraction is applied: the state is
// already DD.
// ===========================================================================

namespace {

/// Constellation string ("GPS", "Galileo", ...) → System enum.
/// Mirrors PppUtils::getSystemString(System) inversely.  Unknown returns
/// System::Unknown so the caller can skip the row.
GnssProcessor::System ddStringToSystem(const std::string& cons) {
    using GnssProcessor::System;
    if (cons == "GPS")     return System::GPS;
    if (cons == "Galileo") return System::Galileo;
    if (cons == "GLONASS") return System::GLONASS;
    if (cons == "BeiDou")  return System::BeiDou;
    if (cons == "QZSS")    return System::QZSS;
    if (cons == "SBAS")    return System::SBAS;
    return System::Unknown;
}

} // namespace

AmbiguityResolution AmbiguityResolver::resolveDd(
    const StateManager& state_manager,
    const std::vector<PppObservation>& observations,
    const std::set<std::string>& exclude_keys) {

    auto log = log_manager_.getLogger("ambiguity_resolver");
    AmbiguityResolution result;

    if (config_.ar_mode == 0) {
        log->debug("AR (DD) disabled (ar_mode=0)");
        return result;
    }

    // Convergence gates, as on the SD path (rationale in resolve()).
    ++epoch_count_;
    if (epoch_count_ <= config_.min_epochs) {
        log->debug("AR (DD) skipped: epoch {}/{} (min_epochs convergence guard)",
                   epoch_count_, config_.min_epochs);
        return result;
    }

    {
        const Eigen::MatrixXd P_chk = state_manager.getCovariance();
        if (P_chk.rows() >= 3) {
            const double sigma_3d = std::sqrt(
                std::max(P_chk(0, 0), 0.0) + std::max(P_chk(1, 1), 0.0) + std::max(P_chk(2, 2), 0.0));
            if (sigma_3d > config_.max_position_sigma_m) {
                log->debug("AR (DD) skipped: 3D position σ={:.4f} m > {:.4f} m (sigma gate)",
                           sigma_3d, config_.max_position_sigma_m);
                return result;
            }
        }
    }

    // sat -> elevation lookup for the elevation mask.
    std::map<std::string, double> sat_elev;
    for (const auto& obs : observations) {
        sat_elev[obs.satellite] = obs.elevation;
    }

    // Per-DD-pair info captured during key parsing.  Stored in a local
    // struct rather than reusing DdPair because DD-mode pairs only need
    // a single state index (the DD itself) plus the per-pair wavelength.
    struct DdEntry {
        int state_idx;        ///< Index of the DD ambiguity float in the full state vector.
        std::string sat_i;    ///< Non-pivot satellite (for diagnostics).
        std::string sat_p;    ///< Pivot satellite (for diagnostics).
        std::string rov_code; ///< Rover RINEX code (drives wavelength lookup).
        double      lambda;   ///< Carrier wavelength of @c rov_code on this satellite (m).
    };
    std::vector<DdEntry> entries;

    const auto& amb_map = state_manager.getAmbiguities();
    const Eigen::VectorXd x_full = state_manager.getStateVector();

    int n_skipped_excluded  = 0;  // non-integer-capable per MeasurementModel
    int n_skipped_elevation = 0;
    int n_skipped_unknown   = 0;
    int n_skipped_no_obs    = 0;

    for (const auto& [key, idx] : amb_map) {
        if (key.compare(0, 3, "DD|") != 0) continue;

        // Parse "DD|<cons>|<sat_i>|<rov_code>|<pivot>" by walking '|'.
        // Skip any malformed key: the writer guarantees this format, so a
        // mismatch most likely means stale state from another mode.
        const auto p1 = key.find('|', 0);              // after "DD"
        const auto p2 = key.find('|', p1 + 1);
        const auto p3 = key.find('|', p2 + 1);
        const auto p4 = key.find('|', p3 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos
         || p3 == std::string::npos || p4 == std::string::npos) {
            ++n_skipped_unknown;
            continue;
        }
        const std::string cons     = key.substr(p1 + 1, p2 - p1 - 1);
        const std::string sat_i    = key.substr(p2 + 1, p3 - p2 - 1);
        const std::string rov_code = key.substr(p3 + 1, p4 - p3 - 1);
        const std::string pivot    = key.substr(p4 + 1);

        if (exclude_keys.count(key)) { ++n_skipped_excluded; continue; }

        // Per-constellation AR filter (mirrors the SD path).
        if (!config_.ar_active_systems.empty()
            && std::find(config_.ar_active_systems.begin(),
                          config_.ar_active_systems.end(),
                          cons) == config_.ar_active_systems.end()) {
            continue;
        }

        auto e_it = sat_elev.find(sat_i);
        if (e_it == sat_elev.end()) { ++n_skipped_no_obs; continue; }
        if (e_it->second < config_.elevation_mask_ar) {
            ++n_skipped_elevation;
            continue;
        }

        const System sys = ddStringToSystem(cons);
        if (sys == System::Unknown) { ++n_skipped_unknown; continue; }

        const double lam = ppp_utils_.getWavelength(sys, rov_code, sat_i);
        if (!(std::isfinite(lam) && lam > 0.0)) { ++n_skipped_unknown; continue; }

        const int sidx = static_cast<int>(idx);
        if (sidx < 0 || sidx >= x_full.size()) { ++n_skipped_unknown; continue; }

        // A DD ambiguity that's still at exactly its initial seed has
        // had no measurement update applied yet; including it would
        // distort the search and the ratio test.
        if (x_full(sidx) == 0.0) { ++n_skipped_unknown; continue; }

        entries.push_back({ sidx, sat_i, pivot, rov_code, lam });
    }

    result.num_candidates = static_cast<int>(entries.size());

    log->debug("AR (DD) candidate set: {} eligible, skipped: non_integer_capable={}, "
               "elev<mask={}, no_obs={}, malformed/unknown={}",
               entries.size(), n_skipped_excluded, n_skipped_elevation,
               n_skipped_no_obs, n_skipped_unknown);

    if (entries.size() < 2) {
        log->debug("AR (DD): need at least 2 candidates, have {}", entries.size());
        return result;
    }

    const Eigen::MatrixXd P_full = state_manager.getCovariance();
    const int nb = static_cast<int>(entries.size());
    const int na = static_cast<int>(x_full.size());

    // Float vector and covariance in meters (the state's native units).
    Eigen::VectorXd y_meters(nb);
    Eigen::MatrixXd Qb_meters(nb, nb);
    for (int i = 0; i < nb; ++i) {
        y_meters(i) = x_full(entries[i].state_idx);
        for (int j = 0; j < nb; ++j) {
            Qb_meters(i, j) = P_full(entries[i].state_idx, entries[j].state_idx);
        }
    }
    Qb_meters = 0.5 * (Qb_meters + Qb_meters.transpose());

    // Convert to cycles for MLAMBDA.  diag(D) = 1/lambda is the
    // meters→cycles transform, applied symmetrically to the covariance.
    Eigen::VectorXd y_cycles(nb);
    Eigen::MatrixXd Qb_cycles(nb, nb);
    for (int i = 0; i < nb; ++i) {
        y_cycles(i) = y_meters(i) / entries[i].lambda;
        for (int j = 0; j < nb; ++j) {
            Qb_cycles(i, j) = Qb_meters(i, j)
                            / (entries[i].lambda * entries[j].lambda);
        }
    }

    Eigen::VectorXd afix_cycles;
    Eigen::VectorXd s;
    Eigen::VectorXd d_lambda;
    if (!mlambda(y_cycles, Qb_cycles, afix_cycles, s, &d_lambda)) {
        log->debug("MLAMBDA (DD) failed (covariance not positive definite or search failed)");
        return result;
    }

    if (s(0) <= 0.0 || s(1) <= 0.0) {
        log->debug("MLAMBDA (DD): invalid squared distances s1={:.6f} s2={:.6f}", s(0), s(1));
        return result;
    }

    result.ratio = s(1) / s(0);
    log->info("MLAMBDA (DD): {} ambs, ratio={:.2f} (threshold={:.1f}), s1={:.4f} s2={:.4f}",
              nb, result.ratio, config_.ratio_threshold, s(0), s(1));

    // Full-set acceptance battery (see resolve() for the rationale of the
    // hoisted p_s / max_dd_frac and the partial-AR fallback).
    double p_s         = 0.0;
    double max_dd_frac = 0.0;
    int    worst_dd_k  = -1;
    bool   full_ok     = true;

    if (result.ratio < config_.ratio_threshold) {
        log->info("AR (DD) ratio test failed: {:.2f} < {:.1f}",
                  result.ratio, config_.ratio_threshold);
        full_ok = false;
    } else if (result.ratio > config_.ratio_upper_bound) {
        log->warn("AR (DD) rejecting fix: ratio={:.3e} > upper bound {:.1e} "
                  "(degenerate covariance — staying float).",
                  result.ratio, config_.ratio_upper_bound);
        full_ok = false;
    } else {
        // FFR gate.
        p_s = computeBootstrapSuccessRate(d_lambda);
        if (p_s < config_.min_success_rate) {
            log->info("AR (DD) rejecting fix despite passing ratio ({:.2f}): "
                      "bootstrap P_s={:.5f} < min_success_rate={:.4f}",
                      result.ratio, p_s, config_.min_success_rate);
            full_ok = false;
        } else {
            // Per-DD residual cap.
            for (int k = 0; k < nb; ++k) {
                const double per_dd = std::abs(y_cycles(k) - afix_cycles(k));
                if (per_dd > max_dd_frac) {
                    max_dd_frac = per_dd;
                    worst_dd_k  = k;
                }
            }
            if (max_dd_frac > config_.nl_max_per_pair_frac) {
                log->info("AR (DD) rejecting fix despite passing ratio ({:.2f}) and P_s={:.5f}: "
                          "DD {}-{} {} has |frac|={:.3f} cyc > nl_max_per_pair_frac={:.3f}",
                          result.ratio, p_s,
                          entries[worst_dd_k].sat_i, entries[worst_dd_k].sat_p,
                          entries[worst_dd_k].rov_code,
                          max_dd_frac, config_.nl_max_per_pair_frac);
                full_ok = false;
            }
        }
    }

    if (!full_ok) {
        if (!config_.enable_partial_ar) return result;

        // Partial AR: retry on the first subset, in greedy backward elimination,
        // that clears the battery.  Baseline DD rows are single-column because
        // the state is the DD ambiguity.
        PartialFix pf = selectFixableSubset_(y_cycles, Qb_cycles);
        if (!pf.success) {
            log->info("Partial AR (DD): no fixable subset found ({} DD candidates); staying float", nb);
            return result;
        }

        const int m = static_cast<int>(pf.kept.size());
        Eigen::MatrixXd H = Eigen::MatrixXd::Zero(m, na);
        Eigen::VectorXd v = Eigen::VectorXd::Zero(m);
        for (int a = 0; a < m; ++a) {
            const int k = pf.kept[a];
            const double afix_m = pf.afix_cycles(a) * entries[k].lambda;
            if (entries[k].state_idx >= 0 && entries[k].state_idx < na)
                H(a, entries[k].state_idx) = +1.0;
            v(a) = afix_m - x_full(entries[k].state_idx);
            if (log->should_log(spdlog::level::debug)) {
                log->debug("  [PAR] DD {}-{} {}: float={:.4f} m fixed={:.4f} m diff={:.4f} m (lambda={:.4f} m)",
                           entries[k].sat_i, entries[k].sat_p, entries[k].rov_code,
                           x_full(entries[k].state_idx), afix_m, v(a), entries[k].lambda);
            }
        }
        Eigen::MatrixXd R = Eigen::MatrixXd::Identity(m, m) * config_.hold_variance;
        KalmanFilter::josephPseudoUpdate(x_full, P_full, H, v, R,
                                         result.fixed_state, result.fixed_covariance);
        result.success   = true;
        result.num_fixed = m;
        result.ratio     = pf.ratio;
        result.p_succ    = pf.p_s;
        log->info("Partial AR (DD) succeeded (Kalman pseudo-obs, R={:.4f} m²): {}/{} ambiguities fixed, "
                  "ratio={:.2f}, P_s={:.5f}, max|frac|={:.3f} cyc",
                  config_.hold_variance, m, nb, pf.ratio, pf.p_s, pf.max_frac);
        return result;
    }

    // Convert the integer fix back to meters for the constraint.
    Eigen::VectorXd afix_meters(nb);
    for (int i = 0; i < nb; ++i) {
        afix_meters(i) = afix_cycles(i) * entries[i].lambda;
    }

    // Apply the integer fix as a Kalman pseudo-observation update
    // rather than by back-substitution: the R = 0 back-substitution form
    // makes the next epoch's LAMBDA degenerate (rationale in the matching
    // comment blocks in resolve() and resolveWlConstrained()).
    //
    // Baseline DD: each row is a single-column constraint, since the state
    // itself is the DD ambiguity and no SD subtraction applies.
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(nb, na);
    Eigen::VectorXd v = Eigen::VectorXd::Zero(nb);
    for (int k = 0; k < nb; ++k) {
        if (entries[k].state_idx >= 0 && entries[k].state_idx < na)
            H(k, entries[k].state_idx) = +1.0;
        v(k) = afix_meters(k) - x_full(entries[k].state_idx);
    }

    Eigen::MatrixXd R   = Eigen::MatrixXd::Identity(nb, nb) * config_.hold_variance;
    KalmanFilter::josephPseudoUpdate(x_full, P_full, H, v, R,
                                     result.fixed_state, result.fixed_covariance);

    result.success   = true;
    result.num_fixed = nb;
    result.p_succ    = p_s;

    log->info("AR (DD) succeeded (Kalman pseudo-obs, R={:.4f} m²): {} ambiguities fixed, "
              "ratio={:.2f}, P_s={:.5f}, max|frac|={:.3f} cyc",
              config_.hold_variance, nb, result.ratio, p_s, max_dd_frac);

    if (log->should_log(spdlog::level::debug)) {
        for (int k = 0; k < nb; ++k) {
            log->debug("  DD {}-{} {}: float={:.4f} m fixed={:.4f} m diff={:.4f} m (lambda={:.4f} m)",
                       entries[k].sat_i, entries[k].sat_p, entries[k].rov_code,
                       y_meters(k), afix_meters(k), -v(k), entries[k].lambda);
        }
    }

    return result;
}

// ===========================================================================
// WL-constrained NL resolution (two-step approach)
// ===========================================================================

AmbiguityResolver::NlRowCoefficients AmbiguityResolver::nlRowCoefficients(
    double f1, double f2, double lam1, double lam2, bool iono_free) {

    // Both rows estimate the same integer N_1 once the widelane WL = N_1 − N_2 is
    // known; they differ in what they are sensitive to.  A slant-iono error moves
    // the metre-domain ambiguities (λ1·N_1, λ2·N_2) along α = (1, (f1/f2)²).
    //
    //   Narrowlane (legacy, default):  a_NL = (f1, f2)/(f1+f2)
    //     y = (f1·N_1 + f2·N_2)/(f1+f2) = N_1 − [f2/(f1+f2)]·WL
    //     Picks up 0.959 cycles per TECU of slant-iono error, so the default
    //     nl_max_per_pair_frac = 0.15 cycles is an implicit 0.156 TECU demand.
    //
    //   Iono-free (nl_iono_free_row):  c_IF = (f1², −f2²)/(f1²−f2²)
    //     B_IF = c_IF·(λ1·N_1, λ2·N_2) = λ_NL·(N_1 + [f2/(f1−f2)]·WL),
    //     since c_IF1·λ1 + c_IF2·λ2 = c(f1−f2)/(f1²−f2²) = c/(f1+f2) = λ_NL.
    //     c_IF·α = 0 exactly, so the row does not see slant ionosphere at all.
    //
    // The cost is the factor (f1+f2)/(f1−f2) = 8.06 for GPS L1/L2, which scales
    // both the row norm (about 65× in variance) and the widelane sensitivity
    // (3.53 cycles of N_1 per cycle of WL error, against 0.438).  The iono-free
    // row trades an ionospheric bias for amplified widelane error and noise.
    NlRowCoefficients c{};
    if (iono_free) {
        // Rows in [1/m] so T·(DD metres) lands in NL cycles:
        //   t = c_IF / λ_NL  with λ_NL = c/(f1+f2)  ⇒  t_1 = f1²/((f1−f2)·c)
        const double f_dif   = f1 - f2;
        const double c_light = Constants::SPEED_OF_LIGHT;
        c.t_1   =  f1 * f1 / (f_dif * c_light);
        c.t_2   = -(f2 * f2) / (f_dif * c_light);
        c.shift = -f2 / f_dif;
        c.bt_2  = -f1 / f_dif;
    } else {
        // Legacy narrowlane row.  The expressions are kept verbatim from the
        // original in-line form so the off path stays bit-identical, not merely
        // algebraically equal (standing invariant #6).
        const double f_sum = f1 + f2;
        c.t_1   =  f1 / (lam1 * f_sum);
        c.t_2   =  f2 / (lam2 * f_sum);
        c.shift =  f2 / (f1 + f2);
        c.bt_2  = -f1 / f_sum;
    }
    return c;
}

AmbiguityResolution AmbiguityResolver::resolveWlConstrained(
    const std::vector<DdPair>& pairs,
    const Eigen::VectorXd& x,
    const Eigen::MatrixXd& P,
    const std::map<std::string, int>& wl_ambiguities,
    const std::vector<PppObservation>& observations) {

    auto log = log_manager_.getLogger("ambiguity_resolver");
    AmbiguityResolution result;

    // Build satellite -> observation map for system/frequency lookup
    std::map<std::string, const PppObservation*> obs_map;
    for (const auto& obs : observations)
        obs_map[obs.satellite] = &obs;

    // Group DD pairs by satellite pair key (sat_i + "|" + sat_ref).
    // We need exactly two signals per satellite pair to form the WL/NL transform.
    struct DdPairInfo {
        int pair_idx;        // index into pairs[]
        std::string signal;
        double freq;         // frequency in Hz
        double wavelength;   // wavelength in meters
    };
    // key: "sat_i|sat_ref" -> vector of DdPairInfo
    std::map<std::string, std::vector<DdPairInfo>> sat_pair_groups;

    for (int k = 0; k < static_cast<int>(pairs.size()); ++k) {
        const auto& p = pairs[k];
        auto obs_it = obs_map.find(p.sat_i);
        if (obs_it == obs_map.end()) continue;
        const auto& obs = *obs_it->second;

        const double freq = ppp_utils_.getFrequency(p.signal, obs.system, obs.satellite);
        const double wl   = ppp_utils_.getWavelength(obs.system, p.signal, obs.satellite);
        if (!(std::isfinite(freq) && freq > 0.0 && std::isfinite(wl) && wl > 0.0)) continue;

        const std::string spkey = p.sat_i + "|" + p.sat_ref;
        sat_pair_groups[spkey].push_back({ k, p.signal, freq, wl });
    }

    // Identify dual-frequency pairs with known DD WL integers
    struct NlCandidate {
        int dd_idx_1;         // index of L1 DD in pairs[]
        int dd_idx_2;         // index of L2 DD in pairs[]
        double f1, f2;        // frequencies (Hz)
        double lam1, lam2;    // wavelengths (m)
        int dd_wl;            // DD WL integer (known)
        double lambda_nl;     // NL wavelength (m)
        // --- Search-functional coefficients (see the T-matrix block below) ---
        // Filled per candidate so every downstream expression keeps one form
        // whichever row (narrowlane or iono-free) is in force.
        double t_coef_1;      // T row entry on dd_idx_1 [1/m]: DD metres → search cycles
        double t_coef_2;      // T row entry on dd_idx_2 [1/m]
        double shift_coef;    // y_for_mlambda = y_reduced + shift_coef·WL  (≡ N_1)
                              // and, identically, N_1 = n_reduced_fixed + shift_coef·WL
        double bt_coef_2;     // N_2 = n_reduced_fixed + bt_coef_2·WL  (= shift_coef − 1)
    };
    std::vector<NlCandidate> nl_candidates;
    // Track which DD indices are consumed by NL pairs
    std::set<int> consumed_dd;
    int n_skipped_immature = 0;   ///< candidates dropped by the min_arc_info gate

    for (const auto& [spkey, infos] : sat_pair_groups) {
        if (infos.size() != 2) continue;  // need exactly two frequencies

        const auto& a = infos[0];
        const auto& b = infos[1];
        const auto& p_a = pairs[a.pair_idx];
        const auto& p_b = pairs[b.pair_idx];

        // Determine which is f1 (higher) and f2 (lower)
        const bool a_is_f1 = a.freq > b.freq;
        const auto& hi = a_is_f1 ? a : b;
        const auto& lo = a_is_f1 ? b : a;

        // Look up the DD WL integer from the MW detector's resolved WL map.
        // WL key format is "SAT_SIG1_SIG2" where SIG1 is on the higher freq.
        // DD_WL = SD_WL(sat_i) - SD_WL(sat_ref)
        const std::string wl_key_i   = p_a.sat_i   + "_" + hi.signal + "_" + lo.signal;
        const std::string wl_key_ref = p_a.sat_ref  + "_" + hi.signal + "_" + lo.signal;

        auto it_i   = wl_ambiguities.find(wl_key_i);
        auto it_ref = wl_ambiguities.find(wl_key_ref);
        if (it_i == wl_ambiguities.end() || it_ref == wl_ambiguities.end()) continue;

        // Maturity gate: both constituent arcs (sat_i and sat_ref on the higher
        // frequency) must have accumulated enough elevation-weighted code
        // information.  A single immature arc fails the ratio test for the whole
        // set, so the gate acts per candidate, not per set.
        if (config_.min_arc_info > 0.0) {
            const double info_i = arcInfoOf_(p_a.sat_i,   hi.signal);
            const double info_r = arcInfoOf_(p_a.sat_ref, hi.signal);
            if (std::min(info_i, info_r) < config_.min_arc_info) {
                ++n_skipped_immature;
                continue;
            }
        }

        const int dd_wl = it_i->second - it_ref->second;

        // Diagnostic: log the MW-derived widelane integers (WLDUMP) so they can be
        // scored against the estimator-free census (scripts/mw_census_allpairs.py);
        // tow lets the comparison window-match the census series.  The N1 search
        // is anchored on dd_wl: a 1-cycle WL error shifts the N1 search target by
        // |shift_coef|, which is f2/(f1+f2) = 0.438 cycles on the narrowlane row
        // and f2/(f1−f2) = 3.53 cycles on the iono-free row (GPS L1/L2).  Either
        // way a wrong WL yields a wrong N1.
        if (!observations.empty()) {
            log->info("WLDUMP tow={:.1f} sat_i={} sat_ref={} sig_hi={} sig_lo={} "
                      "wl_i={} wl_ref={} dd_wl={}",
                      observations.front().time.tow, p_a.sat_i, p_a.sat_ref,
                      hi.signal, lo.signal, it_i->second, it_ref->second, dd_wl);
        }

        const double f1 = hi.freq;
        const double f2 = lo.freq;
        const double f_sum = f1 + f2;
        const double f_dif = f1 - f2;
        const double lambda_nl = Constants::SPEED_OF_LIGHT / (f1 + f2);

        // A near-degenerate frequency pair makes the iono-free row blow up: the
        // amplification is (f1+f2)/(f1−f2), which is 6.9–9.7 for the usual
        // dual-frequency pairs (GPS L1/L2 8.06, L1/L5 6.90, Galileo E1/E5b 7.56,
        // E1/E6 9.62, BeiDou B1I/B3I 9.67) but 77.7 for an E5a/E5b pair, where a
        // single wrong widelane cycle displaces N_1 by about 38 cycles.  Such a
        // candidate is dropped rather than fixed wrong, and the drop is logged
        // (invariant #4).  This check runs before nlRowCoefficients() and guards
        // that function's f1−f2 divisor.
        if (config_.nl_iono_free_row) {
            constexpr double kIfRowMaxWlAmplification = 20.0;
            const double amplification = (f_dif > 0.0)
                ? f_sum / f_dif
                : std::numeric_limits<double>::infinity();
            if (!(amplification < kIfRowMaxWlAmplification)) {
                log->warn("AR iono-free row: dropping {}-{} {}/{} — WL amplification "
                          "{:.1f}x exceeds {:.1f}x (frequency pair too close; a 1-cycle "
                          "WL error would move N_1 by {:.1f} cyc)",
                          p_a.sat_i, p_a.sat_ref, hi.signal, lo.signal,
                          amplification, kIfRowMaxWlAmplification,
                          std::abs(f2 / f_dif));
                continue;
            }
        }

        // Search functional: which row of the (L1,L2) plane observes N_1.
        // Both rows target the same integer; see nlRowCoefficients().
        const NlRowCoefficients row =
            nlRowCoefficients(f1, f2, hi.wavelength, lo.wavelength,
                              config_.nl_iono_free_row);

        nl_candidates.push_back({
            hi.pair_idx, lo.pair_idx,
            f1, f2, hi.wavelength, lo.wavelength,
            dd_wl, lambda_nl,
            row.t_1, row.t_2, row.shift, row.bt_2
        });
        consumed_dd.insert(hi.pair_idx);
        consumed_dd.insert(lo.pair_idx);
    }

    if (n_skipped_immature > 0) {
        // An active gate logs what it removed (invariant #4); otherwise a
        // shrinking candidate set reads as thin data rather than as the gate working.
        log->info("AR maturity gate: {} NL candidate(s) below min_arc_info={:.1f}, "
                  "{} remain", n_skipped_immature, config_.min_arc_info,
                  nl_candidates.size());
    }
    if (nl_candidates.empty()) {
        log->debug("WL-constrained AR: no dual-frequency pairs with known WL");
        return result;  // empty; the caller stays float (no unconstrained fallback)
    }

    // Build the reduced NL ambiguity vector and covariance.
    //
    // For each NL candidate on the default narrowlane row, the float DD NL
    // ambiguity (in cycles) is:
    //   n_NL = (f1 * DD_1/lam1 + f2 * DD_2/lam2) / (f1+f2)
    //
    // where DD_1, DD_2 are the float DD ambiguities in meters.  The iono-free
    // row uses c_IF/λ_NL instead (see nlRowCoefficients()).
    //
    // Only NL candidates enter the LAMBDA search.  Unpaired DD ambiguities
    // (single-signal DDs without a matching second frequency or WL integer)
    // are excluded: they cannot be properly resolved in the WL/NL
    // framework and would contaminate the ratio test.
    const int n_nl  = static_cast<int>(nl_candidates.size());

    if (n_nl <= 0) {
        log->debug("WL-constrained AR: no NL candidates");
        return result;
    }

    // Build transformation matrix T (n_nl x pairs.size()) that maps original
    // DD vector to the NL vector.
    const int n_orig = static_cast<int>(pairs.size());
    Eigen::MatrixXd T = Eigen::MatrixXd::Zero(n_nl, n_orig);

    for (int k = 0; k < n_nl; ++k) {
        const auto& nlc = nl_candidates[k];
        // Row entries are in [1/m]: y_dd carries DD ambiguities in metres
        // (λ_i·N_i), so T·y_dd lands in NL cycles.  The row (narrowlane or
        // exact iono-free) was chosen per candidate above.
        T(k, nlc.dd_idx_1) = nlc.t_coef_1;
        T(k, nlc.dd_idx_2) = nlc.t_coef_2;
    }

    // Extract original DD float vector (meters)
    Eigen::VectorXd y_dd(n_orig);
    for (int k = 0; k < n_orig; ++k)
        y_dd(k) = x(pairs[k].state_idx_i) - x(pairs[k].state_idx_ref);

    // Original DD covariance (meters^2)
    Eigen::MatrixXd Qdd(n_orig, n_orig);
    for (int i = 0; i < n_orig; ++i) {
        for (int j = 0; j < n_orig; ++j) {
            int ii = pairs[i].state_idx_i;
            int ir = pairs[i].state_idx_ref;
            int ji = pairs[j].state_idx_i;
            int jr = pairs[j].state_idx_ref;
            Qdd(i, j) = P(ii, ji) - P(ii, jr) - P(ir, ji) + P(ir, jr);
        }
    }

    // Transform to reduced NL space (cycles)
    Eigen::VectorXd y_reduced = T * y_dd;           // [n_NL cycles]
    Eigen::MatrixXd Q_reduced = T * Qdd * T.transpose();
    Q_reduced = 0.5 * (Q_reduced + Q_reduced.transpose());

    // Diagnostic: pre-fix conditioning of the NL-space matrix MLAMBDA searches.
    // Compared with the post-fix rebuild at each fix site below, it shows
    // whether the Joseph-form pseudo-observation keeps Qb usable for the next
    // epoch or collapses it (the back-substitution failure mode).
    const QbCond qb_pre = qbCondition(Q_reduced);

    // Diagnostic: per-candidate NL sigma against arc age.  Tests whether the
    // sigma_hi floor (about 0.35 cycles, insensitive to both run length and
    // ionosphere_q) is the youngest arc in the set rather than a convergence
    // limit.  One line per candidate per epoch: the candidate's own marginal
    // sigma sqrt(Q_reduced(k,k)) against the age of its two constituent arcs.
    if (!observations.empty()) {
        for (int k = 0; k < n_nl; ++k) {
            const auto& nlc = nl_candidates[k];
            const auto& p1  = pairs[nlc.dd_idx_1];
            const int    age_i  = arcAgeOf_(p1.sat_i,   p1.signal);
            const int    age_r  = arcAgeOf_(p1.sat_ref, p1.signal);
            const double info_i = arcInfoOf_(p1.sat_i,   p1.signal);
            const double info_r = arcInfoOf_(p1.sat_ref, p1.signal);
            log->info("ARCAGE tow={:.1f} sat_i={} sat_ref={} sig={} "
                      "age_i={} age_ref={} age_min={} "
                      "info_i={:.2f} info_ref={:.2f} info_min={:.2f} "
                      "sig_k={:.4f} n_nl={}",
                      observations.front().time.tow, p1.sat_i, p1.sat_ref, p1.signal,
                      age_i, age_r, std::min(age_i, age_r),
                      info_i, info_r, std::min(info_i, info_r),
                      std::sqrt(std::max(Q_reduced(k, k), 0.0)), n_nl);
        }
    }
    // Rebuilds Q_reduced from an updated full covariance with the same T and pairs.
    auto rebuildQnl = [&](const Eigen::MatrixXd& P_new) {
        Eigen::MatrixXd Qdd_new(n_orig, n_orig);
        for (int i = 0; i < n_orig; ++i)
            for (int j = 0; j < n_orig; ++j) {
                const int ii = pairs[i].state_idx_i,  ir = pairs[i].state_idx_ref;
                const int ji = pairs[j].state_idx_i,  jr = pairs[j].state_idx_ref;
                Qdd_new(i, j) = P_new(ii, ji) - P_new(ii, jr) - P_new(ir, ji) + P_new(ir, jr);
            }
        Eigen::MatrixXd Q = T * Qdd_new * T.transpose();
        return Eigen::MatrixXd(0.5 * (Q + Q.transpose()));
    };

    // Run MLAMBDA on the NL-only set
    // Shift y_reduced so MLAMBDA's integer search aligns with integer N_1.
    //
    // Neither reduced combination is integer-valued on its own; each is N_1
    // displaced by a known multiple of the fixed widelane:
    //
    //   narrowlane   y_reduced(k) = N_1 − [f2/(f1+f2)]·WL   (−0.438·WL, L1/L2)
    //   iono-free    y_reduced(k) = N_1 + [f2/(f1−f2)]·WL   (+3.529·WL, L1/L2)
    //
    // Left unshifted, the narrowlane form with WL=1 sits at N_1 − 0.438, where
    // neither integer is meaningfully closer and the ratio test sees s2/s1 ≈ 1.6
    // regardless of float quality.  Adding the known-WL offset back gives a
    // search variable that is integer in expectation, in both forms:
    //
    //   y_for_mlambda(k) = y_reduced(k) + shift_coef·WL_int  =  N_1 + noise
    //
    // afix_n1 is then shifted back to the reduced form so the back-transform
    // expressions below keep one form.  Q_reduced is unchanged: a constant
    // per-row shift moves the mean, not the covariance.
    Eigen::VectorXd nl_shift(n_nl);
    for (int k = 0; k < n_nl; ++k) {
        const auto& nlc = nl_candidates[k];
        nl_shift(k) = nlc.shift_coef * static_cast<double>(nlc.dd_wl);
    }
    const Eigen::VectorXd y_for_mlambda = y_reduced + nl_shift;

    Eigen::VectorXd afix_n1;       // integer N_1 candidates (cycles)
    Eigen::VectorXd s;
    Eigen::VectorXd d_lambda;      // post-reduction LDL diagonal (for bootstrap P_s)
    if (!mlambda(y_for_mlambda, Q_reduced, afix_n1, s, &d_lambda)) {
        log->debug("WL-constrained AR: MLAMBDA failed on NL problem");
        return result;
    }
    // Convert back to the reduced representation expected by the back-transform
    // code below: afix_reduced = N_1 − shift_coef · WL_int, which is
    // N_1 − [f2/(f1+f2)] · WL_int on the narrowlane row.
    Eigen::VectorXd afix_reduced = afix_n1 - nl_shift;

    if (s(0) <= 0.0 || s(1) <= 0.0) {
        log->debug("WL-constrained AR: invalid distances s1={:.6f} s2={:.6f}", s(0), s(1));
        return result;
    }

    result.ratio = s(1) / s(0);
    log->info("WL-constrained AR: {} NL ambs, ratio={:.2f} (threshold={:.1f})",
              n_nl, result.ratio, config_.ratio_threshold);

    // Full-set acceptance battery (rationale in resolve()).  p_s and
    // max_pair_frac are hoisted for the success log; on failure the resolver
    // tries a partial-subset NL fix (when enabled) or stays float.  The
    // bootstrap success rate must reach the configured min_success_rate (e.g.
    // 0.999 for a 0.1% probability of accepting a wrong-integer fix); the
    // per-pair frac cap catches a borderline integer set where one pair sits
    // near a half-cycle boundary.
    double p_s           = 0.0;
    double max_pair_frac = 0.0;
    int    worst_pair_k  = -1;
    bool   full_ok       = true;

    if (result.ratio < config_.ratio_threshold) {
        log->info("WL-constrained AR ratio test failed: {:.2f} < {:.1f}",
                  result.ratio, config_.ratio_threshold);
        full_ok = false;
    } else if (result.ratio > config_.ratio_upper_bound) {
        log->warn("WL-constrained AR rejecting fix: ratio={:.3e} > upper bound {:.1e} "
                  "(degenerate covariance — staying float).",
                  result.ratio, config_.ratio_upper_bound);
        full_ok = false;
    } else {
        p_s = computeBootstrapSuccessRate(d_lambda);
        if (p_s < config_.min_success_rate) {
            log->info("WL-constrained AR rejecting fix despite passing ratio "
                      "({:.2f}): bootstrap P_s={:.5f} < min_success_rate={:.4f}",
                      result.ratio, p_s, config_.min_success_rate);
            full_ok = false;
        } else {
            for (int k = 0; k < n_nl; ++k) {
                const double per_pair = std::abs(y_for_mlambda(k) - afix_n1(k));
                if (per_pair > max_pair_frac) {
                    max_pair_frac = per_pair;
                    worst_pair_k  = k;
                }
            }
            if (max_pair_frac > config_.nl_max_per_pair_frac) {
                const auto& nlc = nl_candidates[worst_pair_k];
                log->info("WL-constrained AR rejecting fix despite passing ratio "
                          "({:.2f}) and P_s={:.5f}: pair {}-{} has |frac|={:.3f} cyc > "
                          "nl_max_per_pair_frac={:.3f}",
                          result.ratio, p_s,
                          pairs[nlc.dd_idx_1].sat_i, pairs[nlc.dd_idx_1].sat_ref,
                          max_pair_frac, config_.nl_max_per_pair_frac);
                full_ok = false;
            }
        }
    }

    if (!full_ok) {
        if (!config_.enable_partial_ar) return result;

        // Partial AR: retry on the first subset of NL candidates, in greedy
        // backward elimination, that clears the battery.  The search runs in
        // the N_1-aligned domain (y_for_mlambda); each kept candidate
        // back-transforms to its two signal-domain DD rows as the full-set path
        // below does.
        PartialFix pf = selectFixableSubset_(y_for_mlambda, Q_reduced);
        if (!pf.success) {
            log->info("Partial AR (WL/NL): no fixable subset found ({} NL candidates); staying float", n_nl);
            log->info("QBCOND path=reject n_nl={} nfix=0 R={:.3e} "
                      "pre_cond={:.4e} pre_sig_lo={:.4e} pre_sig_hi={:.4e} "
                      "post_cond=0 post_sig_lo=0 post_sig_hi=0 cond_ratio=0 siglo_ratio=0 ratio={:.2f}",
                      n_nl, config_.hold_variance,
                      qb_pre.cond, qb_pre.sig_lo, qb_pre.sig_hi, result.ratio);
            return result;
        }

        const int nx = static_cast<int>(x.size());
        const int m  = static_cast<int>(pf.kept.size());
        Eigen::MatrixXd H = Eigen::MatrixXd::Zero(2 * m, nx);
        Eigen::VectorXd v = Eigen::VectorXd::Zero(2 * m);
        for (int a = 0; a < m; ++a) {
            const auto&  nlc        = nl_candidates[pf.kept[a]];
            const double n_nl_fixed = pf.afix_cycles(a) - nl_shift(pf.kept[a]);  // N_1 → reduced
            const double dd1 = (n_nl_fixed + nlc.shift_coef * nlc.dd_wl) * nlc.lam1;
            const double dd2 = (n_nl_fixed + nlc.bt_coef_2 * nlc.dd_wl) * nlc.lam2;
            const auto& p1 = pairs[nlc.dd_idx_1];
            const auto& p2 = pairs[nlc.dd_idx_2];
            if (p1.state_idx_i   >= 0 && p1.state_idx_i   < nx) H(2 * a,     p1.state_idx_i)   = +1.0;
            if (p1.state_idx_ref >= 0 && p1.state_idx_ref < nx) H(2 * a,     p1.state_idx_ref) = -1.0;
            v(2 * a)     = dd1 - (x(p1.state_idx_i) - x(p1.state_idx_ref));
            if (p2.state_idx_i   >= 0 && p2.state_idx_i   < nx) H(2 * a + 1, p2.state_idx_i)   = +1.0;
            if (p2.state_idx_ref >= 0 && p2.state_idx_ref < nx) H(2 * a + 1, p2.state_idx_ref) = -1.0;
            v(2 * a + 1) = dd2 - (x(p2.state_idx_i) - x(p2.state_idx_ref));
            result.fixed_dd_rows.push_back({ p1.state_idx_i, p1.state_idx_ref });
            result.fixed_dd_rows.push_back({ p2.state_idx_i, p2.state_idx_ref });
            if (log->should_log(spdlog::level::debug)) {
                log->debug("  [PAR] NL pair {}-{} {}/{}: N_1_fixed={} dd_wl={}, DD1={:.3f}m DD2={:.3f}m",
                           p1.sat_i, p1.sat_ref, p1.signal, p2.signal,
                           static_cast<int>(std::lround(pf.afix_cycles(a))), nlc.dd_wl, dd1, dd2);
            }
        }
        Eigen::MatrixXd R = Eigen::MatrixXd::Identity(2 * m, 2 * m) * config_.hold_variance;
        KalmanFilter::josephPseudoUpdate(x, P, H, v, R,
                                         result.fixed_state, result.fixed_covariance);
        result.success   = true;
        result.num_fixed = m;
        result.ratio     = pf.ratio;
        result.p_succ    = pf.p_s;
        log->info("Partial AR (WL/NL) succeeded (Kalman pseudo-obs, R={:.4f} m²): {}/{} NL pairs fixed, "
                  "ratio={:.2f}, P_s={:.5f}, max|frac|={:.3f} cyc",
                  config_.hold_variance, m, n_nl, pf.ratio, pf.p_s, pf.max_frac);
        {   // Diagnostic: Qb conditioning across the fix (partial WL/NL path)
            const QbCond qb_post = qbCondition(rebuildQnl(result.fixed_covariance));
            log->info("QBCOND path=partial n_nl={} nfix={} R={:.3e} "
                      "pre_cond={:.4e} pre_sig_lo={:.4e} pre_sig_hi={:.4e} "
                      "post_cond={:.4e} post_sig_lo={:.4e} post_sig_hi={:.4e} "
                      "cond_ratio={:.4e} siglo_ratio={:.4e} ratio={:.2f}",
                      n_nl, m, config_.hold_variance,
                      qb_pre.cond, qb_pre.sig_lo, qb_pre.sig_hi,
                      qb_post.cond, qb_post.sig_lo, qb_post.sig_hi,
                      (qb_pre.cond   > 0.0 ? qb_post.cond   / qb_pre.cond   : 0.0),
                      (qb_pre.sig_lo > 0.0 ? qb_post.sig_lo / qb_pre.sig_lo : 0.0),
                      pf.ratio);
        }
        return result;
    }

    // Back-transform: recover fixed DD ambiguities in meters.  By construction
    // n_reduced_fixed + shift_coef·WL = N_1 and + bt_coef_2·WL = N_2 = N_1 − WL,
    // for either search row:
    //   DD_1 [m] = N_1 · lam1
    //   DD_2 [m] = N_2 · lam2
    Eigen::VectorXd afix_orig(n_orig);
    afix_orig.setZero();

    for (int k = 0; k < n_nl; ++k) {
        const auto& nlc = nl_candidates[k];
        const double n_nl_fixed = afix_reduced(k);

        afix_orig(nlc.dd_idx_1) = (n_nl_fixed + nlc.shift_coef * nlc.dd_wl) * nlc.lam1;
        afix_orig(nlc.dd_idx_2) = (n_nl_fixed + nlc.bt_coef_2 * nlc.dd_wl) * nlc.lam2;

        const double n1_float = y_for_mlambda(k);   // searched as N_1 + noise
        const int    n1_int   = static_cast<int>(std::lround(afix_n1(k)));
        log->debug("  NL pair {}-{} {}/{}: N_1_float={:.3f} N_1_fixed={} dd_wl={}, "
                   "DD1={:.3f}m DD2={:.3f}m",
                   pairs[nlc.dd_idx_1].sat_i, pairs[nlc.dd_idx_1].sat_ref,
                   pairs[nlc.dd_idx_1].signal, pairs[nlc.dd_idx_2].signal,
                   n1_float, n1_int, nlc.dd_wl,
                   afix_orig(nlc.dd_idx_1), afix_orig(nlc.dd_idx_2));
    }

    // Apply the fixed integer DD ambiguities as Kalman pseudo-observations
    // rather than by direct state/covariance overwrite.
    //
    // The back-substitution form
    //     x_fixed = x − Qab · Qbb⁻¹ · (y_dd − a_int)
    //     P_fixed = P  − Qab · Qbb⁻¹ · Qab'
    // is mathematically equivalent to a perfect (R = 0) measurement of
    // the integer.  After it, the ambiguity rows of P_fixed have
    // near-zero variance and the next epoch's LAMBDA call sees a
    // near-singular Qb in the integer search, which produces
    // meaningless ratios (s2 ≈ s1 with both huge, or ratios of order
    // 1e13 that trip the upper-bound guard).
    //
    // The Kalman pseudo-observation form below applies the same
    // integer values through a finite-variance measurement update.  Each
    // fixed DD becomes a 2-state row (+1 on sat_i, −1 on sat_ref)
    // with z = integer-fixed DD value in meters and R = hold_variance.
    // The Joseph form keeps the covariance positive-definite, so the
    // ambiguity columns retain the small but nonzero variance set by
    // R.  The next epoch's AR sees a well-conditioned Qb and can
    // either re-confirm the same fix (tightening further, converging
    // toward the integer) or detect a slip through the residual.
    const int n_fixed_dd = 2 * n_nl;
    const int nx = static_cast<int>(x.size());

    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(n_fixed_dd, nx);
    Eigen::VectorXd v = Eigen::VectorXd::Zero(n_fixed_dd);

    for (int k = 0; k < n_nl; ++k) {
        const auto& nlc = nl_candidates[k];
        const auto& p1 = pairs[nlc.dd_idx_1];
        const auto& p2 = pairs[nlc.dd_idx_2];

        // Row 2k: DD_1 = a_i_1 - a_ref_1 = afix_orig(dd_idx_1)
        if (p1.state_idx_i   >= 0 && p1.state_idx_i   < nx) H(2 * k,     p1.state_idx_i)   = +1.0;
        if (p1.state_idx_ref >= 0 && p1.state_idx_ref < nx) H(2 * k,     p1.state_idx_ref) = -1.0;
        v(2 * k)     = afix_orig(nlc.dd_idx_1) - (x(p1.state_idx_i) - x(p1.state_idx_ref));

        // Row 2k+1: DD_2 constraint
        if (p2.state_idx_i   >= 0 && p2.state_idx_i   < nx) H(2 * k + 1, p2.state_idx_i)   = +1.0;
        if (p2.state_idx_ref >= 0 && p2.state_idx_ref < nx) H(2 * k + 1, p2.state_idx_ref) = -1.0;
        v(2 * k + 1) = afix_orig(nlc.dd_idx_2) - (x(p2.state_idx_i) - x(p2.state_idx_ref));
        result.fixed_dd_rows.push_back({ p1.state_idx_i, p1.state_idx_ref });
        result.fixed_dd_rows.push_back({ p2.state_idx_i, p2.state_idx_ref });
    }

    Eigen::MatrixXd R   = Eigen::MatrixXd::Identity(n_fixed_dd, n_fixed_dd) * config_.hold_variance;
    KalmanFilter::josephPseudoUpdate(x, P, H, v, R,
                                     result.fixed_state, result.fixed_covariance);

    result.success = true;
    result.num_fixed = n_nl;
    result.p_succ  = p_s;

    log->info("WL-constrained AR succeeded (Kalman pseudo-obs, R={:.4f} m²): "
              "{} NL pairs fixed, ratio={:.2f}, P_s={:.5f}, max|frac|={:.3f} cyc",
              config_.hold_variance, n_nl, result.ratio, p_s, max_pair_frac);

    {   // Diagnostic: Qb conditioning across the fix (full WL/NL path)
        const QbCond qb_post = qbCondition(rebuildQnl(result.fixed_covariance));
        log->info("QBCOND path=full n_nl={} nfix={} R={:.3e} "
                  "pre_cond={:.4e} pre_sig_lo={:.4e} pre_sig_hi={:.4e} "
                  "post_cond={:.4e} post_sig_lo={:.4e} post_sig_hi={:.4e} "
                  "cond_ratio={:.4e} siglo_ratio={:.4e} ratio={:.2f}",
                  n_nl, n_nl, config_.hold_variance,
                  qb_pre.cond, qb_pre.sig_lo, qb_pre.sig_hi,
                  qb_post.cond, qb_post.sig_lo, qb_post.sig_hi,
                  (qb_pre.cond   > 0.0 ? qb_post.cond   / qb_pre.cond   : 0.0),
                  (qb_pre.sig_lo > 0.0 ? qb_post.sig_lo / qb_pre.sig_lo : 0.0),
                  result.ratio);
    }

    return result;
}

// ===========================================================================
// WL-only resolution
//
// Conservative AR mode: take the MW-resolved DD WL integers as-is and
// apply each one as a Kalman pseudo-observation that constrains the
// cycle-domain combination
//     (DD_L1 / λ_L1) − (DD_L2 / λ_L2) = WL_int
// The state's L1 and L2 SD ambiguities (still floats) get pulled into
// consistency with the WL integer without any LAMBDA NL search.  No
// ratio test runs; the MW detector's own quality checks (consistent
// across epochs, near-integer Hatch mean, threshold-pass) gate which
// WL integers enter @c wl_ambiguities in the first place.  This path
// then applies the wl_max_frac point test and, when armed (both are on
// by default), the FFR gate (wl_only_ffr) and the innovation screen
// (wl_only_max_innov_cyc).
//
// Useful as a first-stage AR validation before turning on the full
// WL+NL LAMBDA pipeline.  WL has a long wavelength (~86 cm GPS), so
// MW resolution is robust even when the float NL covariance hasn't
// converged enough for safe integer rounding.
// ===========================================================================

double AmbiguityResolver::wlArcSuccessRate_(
    const std::set<std::string>& arc_keys,
    const std::map<std::string, WlArcQuality>& wl_quality,
    std::string* worst_key) const {

    if (worst_key) worst_key->clear();
    if (arc_keys.empty()) return 0.0;

    // 1/(2√2): the per-component bootstrap term is erf(1/(2√2·σ)) = P(|N(0,σ²)| < ½),
    // the same constant and the same functional ils::computeBootstrapSuccessRate
    // applies to the LAMBDA tier's conditional variances.  Sharing the formula is
    // what makes the two tiers' p_succ one comparable number.
    constexpr double kInv2Sqrt2 = 0.353553390593273762200422181052424519;

    double p_total = 1.0;
    double p_worst = 2.0;                       // above any admissible value
    for (const std::string& key : arc_keys) {
        double p_arc = 0.0;                     // fail-safe default: unknown ⇒ zero
        const auto it = wl_quality.find(key);
        if (it != wl_quality.end()) {
            const WlArcQuality& q = it->second;
            if (q.exact) {
                p_arc = 1.0;                    // SD-vs-self pivot: exact by construction
            } else if (q.count >= static_cast<std::size_t>(
                                      std::max(0, config_.wl_only_min_arc_count))
                       && std::isfinite(q.sigma_cycles) && q.sigma_cycles >= 0.0) {
                // σ → 0 on a mature arc means the integer is decided, so the
                // term saturates at 1 rather than dividing by zero.  This is safe
                // because an immature arc (count below wl_only_min_arc_count)
                // never reaches this branch and keeps p_arc = 0.
                p_arc = (q.sigma_cycles > 0.0)
                            ? std::erf(kInv2Sqrt2 / q.sigma_cycles)
                            : 1.0;
            }
        }
        if (p_arc < p_worst) { p_worst = p_arc; if (worst_key) *worst_key = key; }
        p_total *= p_arc;
        if (p_total <= 0.0) {
            // Keep scanning: the caller needs the weakest arc to eliminate, and
            // an early return would report the weakest arc seen so far rather
            // than the weakest overall.
            p_total = 0.0;
        }
    }
    return p_total;
}

AmbiguityResolution AmbiguityResolver::resolveWlOnly(
    const std::vector<DdPair>& pairs,
    const Eigen::VectorXd& x,
    const Eigen::MatrixXd& P,
    const std::map<std::string, int>& wl_ambiguities,
    const std::map<std::string, double>& wl_fractions,
    const std::vector<PppObservation>& observations,
    const std::map<std::string, WlArcQuality>& wl_quality) {

    auto log = log_manager_.getLogger("ambiguity_resolver");
    AmbiguityResolution result;
    result.fixed_state      = x;
    result.fixed_covariance = P;

    // Build sat -> obs map for system/frequency lookup
    std::map<std::string, const PppObservation*> obs_map;
    for (const auto& obs : observations)
        obs_map[obs.satellite] = &obs;

    // Group DD pairs by satellite pair, mirroring resolveWlConstrained
    struct InfoRow {
        int idx_i, idx_ref;        // SD ambiguity state indices (non-pivot, pivot)
        std::string signal;
        double freq;
        double wavelength;
    };
    std::map<std::string, std::vector<InfoRow>> sat_pair_groups;
    for (const auto& p : pairs) {
        auto obs_it = obs_map.find(p.sat_i);
        if (obs_it == obs_map.end()) continue;
        const auto& obs = *obs_it->second;
        const double freq = ppp_utils_.getFrequency(p.signal, obs.system, obs.satellite);
        const double wl   = ppp_utils_.getWavelength(obs.system, p.signal, obs.satellite);
        if (!(std::isfinite(freq) && freq > 0.0 && std::isfinite(wl) && wl > 0.0)) continue;

        const std::string spkey = p.sat_i + "|" + p.sat_ref;
        sat_pair_groups[spkey].push_back({ p.state_idx_i, p.state_idx_ref, p.signal, freq, wl });
    }

    // Collect WL constraints: one per (sat_pair, dual-frequency) combo with
    // a known DD WL integer.
    struct WlConstraint {
        int idx_L1_i, idx_L1_ref, idx_L2_i, idx_L2_ref;
        double inv_lam_L1, inv_lam_L2;
        int wl_int;
        std::string sat_i, sat_ref, sig_hi, sig_lo;
        // The two MW arcs this DD integer is built from (N_i - N_ref).  Carried
        // so the FFR gate can take its product over distinct arcs: many DDs
        // share the pivot arc, and a wrong pivot makes all of them wrong at once.
        std::string arc_key_i, arc_key_ref;
    };
    std::vector<WlConstraint> constraints;
    for (const auto& [spkey, infos] : sat_pair_groups) {
        if (infos.size() != 2) continue;  // need exactly two frequencies

        const auto& a = infos[0];
        const auto& b = infos[1];
        const bool a_is_hi = a.freq > b.freq;
        const auto& hi = a_is_hi ? a : b;
        const auto& lo = a_is_hi ? b : a;

        // Recover the satellite IDs from the spkey.
        const auto sep = spkey.find('|');
        if (sep == std::string::npos) continue;
        const std::string sat_i   = spkey.substr(0, sep);
        const std::string sat_ref = spkey.substr(sep + 1);

        const std::string wl_key_i   = sat_i   + "_" + hi.signal + "_" + lo.signal;
        const std::string wl_key_ref = sat_ref + "_" + hi.signal + "_" + lo.signal;
        auto it_i   = wl_ambiguities.find(wl_key_i);
        auto it_ref = wl_ambiguities.find(wl_key_ref);
        if (it_i == wl_ambiguities.end() || it_ref == wl_ambiguities.end()) continue;

        // Per-fix WL confidence gate (as in applyMwWlConstraintsNlWl_).  The DD
        // WL integer combines two zero-difference MW arcs (N_i − N_ref).  If
        // either arc's running fractional residual exceeds wl_max_frac (locked
        // too early, bias-contaminated, or near a half-cycle boundary), the DD
        // WL is suspect, and the pair stays float for this epoch rather than
        // committing a questionable integer to the pseudo-obs update, where a
        // wrong WL biases the whole SD ambiguity block.  Both arcs are gated;
        // the NlWl gate tests one arc at a time, but every arc there, pivots
        // included, passes it before DD constraints are formed.
        const auto frac_of = [&](const std::string& k) {
            auto it = wl_fractions.find(k);
            return it != wl_fractions.end() ? it->second : 0.0;
        };
        const double frac_i   = frac_of(wl_key_i);
        const double frac_ref = frac_of(wl_key_ref);
        if (frac_i > config_.wl_max_frac || frac_ref > config_.wl_max_frac) {
            log->debug("WL-only AR: skip DD {}-{} {}/{}: frac_i={:.3f}, frac_ref={:.3f} cyc "
                       "> wl_max_frac={:.3f}",
                       sat_i, sat_ref, hi.signal, lo.signal,
                       frac_i, frac_ref, config_.wl_max_frac);
            continue;
        }

        const int dd_wl = it_i->second - it_ref->second;
        constraints.push_back({
            hi.idx_i, hi.idx_ref, lo.idx_i, lo.idx_ref,
            1.0 / hi.wavelength, 1.0 / lo.wavelength,
            dd_wl, sat_i, sat_ref, hi.signal, lo.signal,
            wl_key_i, wl_key_ref
        });
    }

    if (constraints.empty()) {
        log->debug("WL-only AR: no dual-frequency pairs with known DD WL integers");
        return result;
    }

    // ------------------------------------------------------------------
    // FFR acceptance gate (AmbiguityResolverConfig::wl_only_ffr).
    //
    // With the gate off, the WL-only path applies every MW integer that clears
    // `wl_max_frac`, a point test on the running residual that cannot bound
    // P(the integer is wrong), and its `ratio` is only the "+inf = no LAMBDA
    // ran" sentinel.  An ungated 5-constraint fix put the held-out consumer
    // 35 m off (docs/SPEC_clock_temporal_anchor.md §8.11).
    //
    // The gate applies the bootstrap functional of the LAMBDA tier over the
    // distinct arcs backing the set and eliminates the weakest arc until the
    // survivors reach `min_success_rate`.  Backward elimination rather than
    // all-or-nothing follows selectFixableSubset: a confidently known subset is
    // worth applying even when one arc is not ready.
    // wl_p_succ here and wl_max_innov below are computed on every call and acted
    // on only when their gate is armed.  A disarmed run (legacy reproduction)
    // therefore still reports what the gate would have seen, instead of a 0 that
    // misreads the same way `ratio = +inf` misreads as a perfect ratio.  Both
    // cost a few flops per arc.
    double wl_p_succ = 0.0;
    {
        std::set<std::string> arcs0;
        for (const auto& c : constraints) {
            arcs0.insert(c.arc_key_i);
            arcs0.insert(c.arc_key_ref);
        }
        wl_p_succ = wlArcSuccessRate_(arcs0, wl_quality, nullptr);
    }
    if (config_.wl_only_ffr) {
        while (!constraints.empty()) {
            std::set<std::string> arcs;
            for (const auto& c : constraints) {
                arcs.insert(c.arc_key_i);
                arcs.insert(c.arc_key_ref);
            }
            std::string worst;
            wl_p_succ = wlArcSuccessRate_(arcs, wl_quality, &worst);
            if (wl_p_succ >= config_.min_success_rate) break;
            if (worst.empty()) { constraints.clear(); break; }
            const std::size_t before = constraints.size();
            constraints.erase(
                std::remove_if(constraints.begin(), constraints.end(),
                               [&](const WlConstraint& c) {
                                   return c.arc_key_i == worst || c.arc_key_ref == worst;
                               }),
                constraints.end());
            log->debug("WL-only AR (FFR): P_s={:.6f} < {:.6f}; dropping arc '{}' "
                       "({} -> {} constraint(s))",
                       wl_p_succ, config_.min_success_rate, worst,
                       before, constraints.size());
            // Dropping the pivot arc removes every DD that referenced it, so the
            // loop can empty the set in one step.  That is the intended outcome,
            // not a degenerate case: a set whose common reference is unreliable
            // has no trustworthy member.
        }
        if (constraints.empty()) {
            log->info("WL-only AR: no MW WL constraint set reaches the bootstrap "
                      "success rate {:.4f} (FFR gate) — staying FLOAT this epoch.",
                      config_.min_success_rate);
            result.gate_reason = 5;   // the guards rejected the set; MLAMBDA never ran
            return result;
        }
    }

    // ------------------------------------------------------------------
    // Innovation screen (AmbiguityResolverConfig::wl_only_max_innov_cyc): the
    // counterpart of the LAMBDA tier's per-entry nl_max_per_pair_frac.
    //
    // The FFR gate above bounds the random failure of rounding each arc, and on
    // a mature arc it correctly reports certainty.  It cannot see a systematic
    // disagreement between the asserted integer and the filter's own frame,
    // because nothing in the arc's dispersion depends on that frame.  In the
    // recorded failure every arc had p_succ = 1.000000 while the asserted
    // widelanes contradicted the float enough to move the position 33 m in one
    // epoch (SPEC_clock_temporal_anchor §8.11).
    //
    // A constraint whose innovation exceeds half a widelane (the 0.5-cycle
    // default) is not a rounding question; it is dropped rather than letting
    // a σ = 0.01 cycle pseudo-obs drag the state across the gap.
    double wl_max_innov = 0.0;
    {   // measured on every arm, armed or not (see the note above)
        const int nx_p = static_cast<int>(x.size());
        for (const auto& c : constraints) {
            if (c.idx_L1_i < 0 || c.idx_L1_i >= nx_p || c.idx_L1_ref < 0 || c.idx_L1_ref >= nx_p
             || c.idx_L2_i < 0 || c.idx_L2_i >= nx_p || c.idx_L2_ref < 0 || c.idx_L2_ref >= nx_p)
                continue;
            const double pred = (x(c.idx_L1_i) - x(c.idx_L1_ref)) * c.inv_lam_L1
                              - (x(c.idx_L2_i) - x(c.idx_L2_ref)) * c.inv_lam_L2;
            wl_max_innov = std::max(wl_max_innov,
                                    std::abs(static_cast<double>(c.wl_int) - pred));
        }
    }
    if (config_.wl_only_max_innov_cyc > 0.0) {
        const int nx_pred = static_cast<int>(x.size());
        const std::size_t before = constraints.size();
        int n_over = 0;
        double worst_over = 0.0;
        std::string worst_id;
        constraints.erase(
            std::remove_if(constraints.begin(), constraints.end(),
                           [&](const WlConstraint& c) {
                               if (c.idx_L1_i   < 0 || c.idx_L1_i   >= nx_pred ||
                                   c.idx_L1_ref < 0 || c.idx_L1_ref >= nx_pred ||
                                   c.idx_L2_i   < 0 || c.idx_L2_i   >= nx_pred ||
                                   c.idx_L2_ref < 0 || c.idx_L2_ref >= nx_pred)
                                   return true;   // unusable indices: drop, never assert
                               const double pred =
                                   (x(c.idx_L1_i) - x(c.idx_L1_ref)) * c.inv_lam_L1
                                 - (x(c.idx_L2_i) - x(c.idx_L2_ref)) * c.inv_lam_L2;
                               const double innov =
                                   std::abs(static_cast<double>(c.wl_int) - pred);
                               if (innov > wl_max_innov) wl_max_innov = innov;
                               if (innov > config_.wl_only_max_innov_cyc) {
                                   ++n_over;
                                   if (innov > worst_over) {
                                       worst_over = innov;
                                       worst_id = c.sat_i + "-" + c.sat_ref + " "
                                                + c.sig_hi + "/" + c.sig_lo;
                                   }
                                   return true;
                               }
                               return false;
                           }),
            constraints.end());
        if (n_over > 0) {
            log->warn("WL-only AR: dropped {} of {} MW WL constraint(s) whose asserted "
                      "integer disagrees with the float by more than {:.2f} cyc (worst "
                      "{:.3f} cyc on {}).  An innovation this large is a FRAME conflict, "
                      "not a rounding question — the MW wide-lane is in the .BIA frame "
                      "and a network-bias consumer's ambiguities are not.",
                      n_over, before, config_.wl_only_max_innov_cyc, worst_over, worst_id);
        }
        if (constraints.empty()) {
            log->info("WL-only AR: every MW WL constraint failed the innovation screen "
                      "— staying FLOAT this epoch.");
            result.gate_reason = 5;   // the guards rejected the set; MLAMBDA never ran
            return result;
        }
    }

    // Detect WL integer changes since the last call.  A change implies a slip
    // on L1, L2 or both for the affected satellite: the MW Hatch mean re-locked
    // at a different integer.  Applying the new constraint with the existing
    // covariance (tight from earlier epochs) gives a small gain against an
    // integer-cycle innovation, so the state would drift toward the new integer
    // over many epochs instead of snapping in one.  The covariance of the
    // affected L1 and L2 SD ambiguity states is therefore reset to
    // kAmbResetVar, so the new constraint takes full effect at once and later
    // measurement updates refine from there.  handlePhaseOutlier zeroes the
    // row and column the same way but resets to getAmbiguityStd()².
    constexpr double kAmbResetVar = 100.0;  // (10 m)²

    Eigen::MatrixXd P_local = P;  // mutable working copy
    int n_reset = 0;
    auto inflate_state = [&](int idx) {
        if (idx < 0 || idx >= P_local.rows()) return;
        P_local.row(idx).setZero();
        P_local.col(idx).setZero();
        P_local(idx, idx) = kAmbResetVar;
    };

    for (const auto& c : constraints) {
        const std::string key = c.sat_i + "_" + c.sat_ref + "_" + c.sig_hi + "_" + c.sig_lo;
        auto prev_it = previous_dd_wl_.find(key);
        if (prev_it == previous_dd_wl_.end()) continue;     // first time: no reset needed
        if (prev_it->second == c.wl_int)     continue;      // same integer: no change

        log->warn("WL integer changed for DD {}-{} {}/{}: {} → {} cycles. "
                  "Inflating L1/L2 SD ambiguity covariance to allow snap.",
                  c.sat_i, c.sat_ref, c.sig_hi, c.sig_lo,
                  prev_it->second, c.wl_int);

        // Reset the sat_i SD ambiguity states on both bands; the reference
        // entries (idx_L1_ref, idx_L2_ref) are left as they are.  The reference
        // is not a BSSD-frozen pivot: buildDdPairs skips zero-value and
        // zero-variance ambiguities, so it always has a live state.
        inflate_state(c.idx_L1_i);
        inflate_state(c.idx_L2_i);
        ++n_reset;
    }

    // Build pseudo-observation update.  H is in cycle-domain (so R is also
    // in cycles²); each row enforces (DD_L1/λ_L1) − (DD_L2/λ_L2) = WL_int.
    const int nx = static_cast<int>(x.size());
    const int nv = static_cast<int>(constraints.size());
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(nv, nx);
    Eigen::VectorXd v = Eigen::VectorXd::Zero(nv);

    for (int k = 0; k < nv; ++k) {
        const auto& c = constraints[k];
        if (c.idx_L1_i   >= 0 && c.idx_L1_i   < nx) H(k, c.idx_L1_i)   =  c.inv_lam_L1;
        if (c.idx_L1_ref >= 0 && c.idx_L1_ref < nx) H(k, c.idx_L1_ref) = -c.inv_lam_L1;
        if (c.idx_L2_i   >= 0 && c.idx_L2_i   < nx) H(k, c.idx_L2_i)   = -c.inv_lam_L2;
        if (c.idx_L2_ref >= 0 && c.idx_L2_ref < nx) H(k, c.idx_L2_ref) =  c.inv_lam_L2;

        const double dd_wl_pred = (x(c.idx_L1_i) - x(c.idx_L1_ref)) * c.inv_lam_L1
                                - (x(c.idx_L2_i) - x(c.idx_L2_ref)) * c.inv_lam_L2;
        v(k) = static_cast<double>(c.wl_int) - dd_wl_pred;
    }

    // Kalman update with cycle-domain pseudo-obs variance.  P_local has
    // inflated diagonals for any rows whose WL integer just changed so the
    // gain on those is high and the state snaps to the new integer.
    Eigen::MatrixXd R = Eigen::MatrixXd::Identity(nv, nv) * config_.wl_only_pseudo_var;
    KalmanFilter::josephPseudoUpdate(x, P_local, H, v, R,
                                     result.fixed_state, result.fixed_covariance);

    result.success     = true;
    result.num_fixed   = nv;
    result.ratio       = std::numeric_limits<double>::infinity();  // sentinel: no LAMBDA ratio test ran
    // The sentinel above says only that no ratio test ran; p_succ is the figure
    // this fix is graded on, and wl_only_fix lets a trace say which machinery
    // fixed rather than inferring it from an infinity.
    result.p_succ      = wl_p_succ;
    result.wl_only_fix = true;

    // Record the applied WL integers so the next call can detect changes.
    for (const auto& c : constraints) {
        const std::string key = c.sat_i + "_" + c.sat_ref + "_" + c.sig_hi + "_" + c.sig_lo;
        previous_dd_wl_[key] = c.wl_int;
    }

    log->info("WL-only AR: applied {} DD WL constraint(s){} (no LAMBDA NL search); "
              "P_s={:.6f}, max|innov| seen (PRE-screen) {:.4f} cyc",
              nv, n_reset > 0 ? fmt::format(", {} with reset", n_reset) : "",
              wl_p_succ, wl_max_innov);
    if (log->should_log(spdlog::level::debug)) {
        for (const auto& c : constraints) {
            log->debug("  WL DD {}-{} {}/{}: WL_int={} cyc, residual={:+.4f} cyc",
                       c.sat_i, c.sat_ref, c.sig_hi, c.sig_lo, c.wl_int,
                       static_cast<double>(c.wl_int) - (
                           (x(c.idx_L1_i) - x(c.idx_L1_ref)) * c.inv_lam_L1
                         - (x(c.idx_L2_i) - x(c.idx_L2_ref)) * c.inv_lam_L2));
        }
    }
    return result;
}

// ===========================================================================
// MW WL constraints in the NlWl basis
// ---------------------------------------------------------------------------
// Counterpart to resolveWlOnly() above.  The per-band path forms 2-row
// pseudo-observations on (L_hi_i, L_hi_ref, L_lo_i, L_lo_ref) per
// (sat_i, sat_ref) DD; the NlWl path expresses the same constraint as a
// linear combination of NL/WL state columns.
//
// State convention (per ensureRequiredStates): one NL state per sat
// (NL_R = N_<ref_band>) plus one WL state per non-ref band (canonical
// key WL_<min(R,b)>_<max(R,b)> stores N_<min> - N_<max> in cycles).  For
// any band b on a satellite with reference band R:
//
//     N_b = NL_R + alpha(R, b) * WL_<canonical>_state
//
// where alpha(R, b) = -1 when R < b (the canonical WL key has R first,
// so state = N_R - N_b → N_b = NL_R - state) and alpha(R, b) = +1 when
// R > b (canonical key has b first, so state = N_b - N_R → N_b = NL_R +
// state); alpha(R, R) = 0 (the band is the ref, no WL term).
//
// MW(hi, lo) returns the SD WL integer N_hi - N_lo in cycles.  Substituting:
//
//     MW = (NL_R + alpha(R,hi) * W_hi) - (NL_R + alpha(R,lo) * W_lo)
//        = alpha(R,hi) * W_hi - alpha(R,lo) * W_lo
//
// The NL_R term cancels.  After BSSD differencing against an AR pivot
// satellite, the structure is the same on both sats and the DD is:
//
//   alpha(R,hi) * (W_hi_i - W_hi_pivot) - alpha(R,lo) * (W_lo_i - W_lo_pivot)
//   = MW_int_i - MW_int_pivot = DD_WL_int
//
// This handles every MW arc the detector resolves:
//
//   - MW(R, b) for b != R: alpha(hi)=0 or alpha(lo)=0, so the row collapses
//     to a single non-zero coefficient on WL_<canonical>.  These are the
//     classic "L1/L2" and "L1/L5" cases when ref_band = 1.
//
//   - MW(b1, b2) where neither equals R (e.g. GPS L2/L5 EWL with R=1, or
//     Galileo E5a/E5b EWL with R=1): two non-zero coefficients across
//     two distinct WL state columns per sat, giving a 4-state DD row.  It
//     applies EWL information that the per-band path's ref-relative
//     enumeration drops without notice.
//
// Coincidence: when an AR-internal pivot equals the BSSD pivot, that
// pivot's NL/WL columns are zero-covariance / zero-valued and their
// contribution drops out of the DD; the row reduces to the sat_i side
// only.  The Kalman update handles this naturally.
// ===========================================================================

AmbiguityResolution AmbiguityResolver::applyMwWlConstraintsNlWl_(
    const StateManager& state_manager,
    const Eigen::VectorXd& x,
    const Eigen::MatrixXd& P,
    const std::map<std::string, int>& wl_ambiguities,
    const std::map<std::string, double>& wl_fractions,
    const std::vector<PppObservation>& observations,
    const std::map<std::string, WlArcQuality>& wl_quality) {

    auto log = log_manager_.getLogger("ambiguity_resolver");
    AmbiguityResolution result;
    result.fixed_state      = x;
    result.fixed_covariance = P;

    if (wl_ambiguities.empty()) {
        log->debug("WL-NlWl: no MW-resolved WL integers this epoch");
        return result;
    }

    // Per-satellite lookups.
    std::map<std::string, double> sat_elev;
    std::map<std::string, const PppObservation*> obs_map;
    for (const auto& obs : observations) {
        sat_elev[obs.satellite] = obs.elevation;
        obs_map[obs.satellite] = &obs;
    }

    // Reference band per sat (from existing NL state keys).  It stays fixed
    // for the lifetime of the satellite's arc (Phase 3b).
    std::map<std::string, char> ref_band_per_sat;
    for (const auto& [key, idx] : state_manager.getAmbiguities()) {
        if (auto p = StateKeys::parseNlAmbiguityKey(key)) {
            ref_band_per_sat[p->satellite] = p->band;
        }
    }

    // wl_state_idx[sat][other_band] = state-vector index of the canonical
    // WL state coupling (ref_band, other_band).  Keyed by the band that is
    // not the ref, so callers can look up by the carrier they need,
    // regardless of canonical min/max ordering.
    std::map<std::string, std::map<char, int>> wl_state_idx;
    for (const auto& [key, idx] : state_manager.getAmbiguities()) {
        auto p = StateKeys::parseWlAmbiguityKey(key);
        if (!p) continue;
        auto rbi = ref_band_per_sat.find(p->satellite);
        if (rbi == ref_band_per_sat.end()) continue;
        const char R = rbi->second;
        char other = 0;
        if (p->band_a == R)      other = p->band_b;
        else if (p->band_b == R) other = p->band_a;
        else                     continue;  // non-ref-relative WL; Phase 3b never creates these.
        wl_state_idx[p->satellite][other] = static_cast<int>(idx);
    }

    // Per-MW-arc constraint metadata, built by parsing the keys of the
    // wl_ambiguities map (e.g. "G01_L1W_L2W") rather than by enumerating
    // state WL columns.  That lets the EWL through: the L2/L5 MW arc is in
    // this map even though the state has no WL_2_5.
    struct ArcConstraint {
        std::string sat;
        char        band_hi;     ///< Higher carrier-frequency band of the pair.
        char        band_lo;     ///< Lower carrier-frequency band of the pair.
        char        canon_a;     ///< Canonical-ascending min(band_hi, band_lo).
        char        canon_b;     ///< Canonical-ascending max(band_hi, band_lo).
        std::string sig_hi;      ///< Higher-freq signal name (for slip-snap key).
        std::string sig_lo;
        int         mw_int;      ///< MW SD-WL integer (cycles, N_hi - N_lo form).
        double      elevation;
        /// The MW detector's own stats key for this arc, carried verbatim.
        /// Do not reconstruct it from sat + sig_hi + sig_lo: sig_hi/sig_lo are
        /// frequency-ordered while the key keeps its original order, so the
        /// two disagree whenever f_a < f_b.  A reconstructed key that misses
        /// turns "unknown arc" into a silent gate bypass.
        std::string stats_key;
    };
    std::map<std::string, std::vector<ArcConstraint>> groups;  // "<cons>|<canon_a>_<canon_b>"

    for (const auto& [stats_key, mw_int] : wl_ambiguities) {
        const auto u1 = stats_key.find('_');
        if (u1 == std::string::npos) continue;
        const auto u2 = stats_key.find('_', u1 + 1);
        if (u2 == std::string::npos) continue;
        const std::string sat   = stats_key.substr(0, u1);
        const std::string sig_a = stats_key.substr(u1 + 1, u2 - u1 - 1);
        const std::string sig_b = stats_key.substr(u2 + 1);
        if (sig_a.size() < 2 || sig_b.size() < 2) continue;

        auto eit = sat_elev.find(sat);
        if (eit == sat_elev.end()) continue;
        if (eit->second < config_.elevation_mask_ar) continue;

        // Per-constellation AR filter.  Skip arcs from constellations
        // not in @c ar_active_systems.  The arc continues to be tracked
        // in the float Kalman filter (its NL/WL state is unaffected);
        // it just doesn't contribute to the AR pseudo-obs update.
        if (!config_.ar_active_systems.empty()) {
            const std::string sys = PppUtils::getSystemString(sat);
            if (std::find(config_.ar_active_systems.begin(),
                          config_.ar_active_systems.end(),
                          sys) == config_.ar_active_systems.end()) {
                continue;
            }
        }

        // Per-fix WL confidence gate.  An MW arc whose running mean sits far
        // from its locked integer (frac above wl_max_frac) is suspect,
        // typically because the arc locked too early, carries unmodeled bias,
        // or sits near a half-cycle boundary.  Rather than committing the
        // questionable integer to the AR pseudo-obs update, where a wrong WL
        // would bias every NL fix that depends on it, the arc stays float for
        // this epoch.  Pivot arcs pass the same test, since the pivot is picked
        // later from the surviving arcs.
        auto fit = wl_fractions.find(stats_key);
        const double frac = (fit != wl_fractions.end()) ? fit->second : 0.0;
        if (frac > config_.wl_max_frac) {
            log->debug("WL-NlWl: skip {} {}/{}: frac={:.3f} cyc > wl_max_frac={:.3f}",
                       sat, sig_a, sig_b, frac, config_.wl_max_frac);
            continue;
        }

        auto rbi = ref_band_per_sat.find(sat);
        if (rbi == ref_band_per_sat.end()) continue;
        const char R = rbi->second;

        auto oit = obs_map.find(sat);
        if (oit == obs_map.end()) continue;
        const auto& obs = *oit->second;

        const double f_a = ppp_utils_.getFrequency(sig_a, obs.system, sat);
        const double f_b = ppp_utils_.getFrequency(sig_b, obs.system, sat);
        if (!(std::isfinite(f_a) && f_a > 0.0
           && std::isfinite(f_b) && f_b > 0.0)) continue;

        const char b_a = sig_a[1];
        const char b_b = sig_b[1];

        ArcConstraint c;
        c.sat = sat;
        if (f_a > f_b) { c.band_hi = b_a; c.band_lo = b_b; c.sig_hi = sig_a; c.sig_lo = sig_b; }
        else           { c.band_hi = b_b; c.band_lo = b_a; c.sig_hi = sig_b; c.sig_lo = sig_a; }
        c.canon_a   = std::min(b_a, b_b);
        c.canon_b   = std::max(b_a, b_b);
        c.mw_int    = mw_int;
        c.elevation = eit->second;
        c.stats_key = stats_key;

        // Skip arcs for which the satellite lacks the WL state(s) needed
        // to express the constraint as a state linear combination.
        auto wsi = wl_state_idx.find(sat);
        if (wsi == wl_state_idx.end()) continue;
        auto hasWl = [&](char band) { return wsi->second.count(band) > 0; };
        const bool need_hi = (c.band_hi != R);
        const bool need_lo = (c.band_lo != R);
        if (need_hi && !hasWl(c.band_hi)) continue;
        if (need_lo && !hasWl(c.band_lo)) continue;

        const std::string cons = PppUtils::getSystemString(sat);
        const std::string gk =
            cons + "|" + std::string{ c.canon_a } + "_" + std::string{ c.canon_b };
        groups[gk].push_back(std::move(c));
    }

    if (groups.empty()) {
        log->debug("WL-NlWl: no MW arcs with state-expressible coefficients");
        return result;
    }

    // alpha(R, b): coefficient relating N_b to (NL_R, WL_<canonical>_state).
    auto alpha_coeff = [](char R, char b) -> int {
        if (R == b)  return 0;
        return (R < b) ? -1 : +1;
    };

    // Per-group AR pivot pick (highest elevation), then form DD constraints.
    // Coefficients are signed integers (∈ {-1, 0, +1}); H rows are 1, 2,
    // 3, or 4 non-zero entries depending on (band_hi, band_lo, R) and
    // whether the AR pivot's columns are BSSD-frozen.
    struct WlConstraint {
        std::vector<std::pair<int, double>> coeffs;  // (state_idx, coefficient)
        int          dd_wl_int;
        double       predicted_dd;
        std::string  sat_i;
        std::string  sat_ref;
        std::string  sig_hi;
        std::string  sig_lo;
        std::vector<int> non_pivot_wl_indices;  // for slip-snap inflation
        // The two MW arcs this DD integer is built from (a.mw_int - pivot.mw_int).
        // Carried so the FFR gate can take its product over distinct arcs: every
        // DD in a group shares the pivot arc, so a wrong pivot makes all of them
        // wrong at once and a per-DD product would count that failure once per DD.
        std::string  arc_key_i, arc_key_ref;
    };
    std::vector<WlConstraint> constraints;

    for (const auto& [gk, arcs] : groups) {
        if (arcs.size() < 2) continue;

        size_t pivot_idx = 0;
        for (size_t i = 1; i < arcs.size(); ++i) {
            if (arcs[i].elevation > arcs[pivot_idx].elevation) pivot_idx = i;
        }
        const auto& pivot = arcs[pivot_idx];

        for (size_t i = 0; i < arcs.size(); ++i) {
            if (i == pivot_idx) continue;
            const auto& a = arcs[i];

            // ref_band must match between the two sats for the
            // ref-relative state mapping to be consistent.  In a normal
            // BSSD configuration all sats in a constellation share the
            // ref_band; defensive skip otherwise.
            const char R_i   = ref_band_per_sat.at(a.sat);
            const char R_ref = ref_band_per_sat.at(pivot.sat);
            if (R_i != R_ref) {
                log->debug("WL-NlWl: skip {} vs {} on {}/{}: ref_band mismatch ({} vs {})",
                           a.sat, pivot.sat, a.sig_hi, a.sig_lo, R_i, R_ref);
                continue;
            }
            const char R = R_i;

            const double a_hi = static_cast<double>(alpha_coeff(R, a.band_hi));
            const double a_lo = static_cast<double>(alpha_coeff(R, a.band_lo));

            auto getIdx = [&](const std::string& sat, char band) -> int {
                if (band == R) return -1;       // alpha is 0 anyway
                auto sit = wl_state_idx.find(sat);
                if (sit == wl_state_idx.end()) return -1;
                auto bit = sit->second.find(band);
                return (bit == sit->second.end()) ? -1 : bit->second;
            };
            const int idx_hi_i   = getIdx(a.sat,    a.band_hi);
            const int idx_lo_i   = getIdx(a.sat,    a.band_lo);
            const int idx_hi_ref = getIdx(pivot.sat, a.band_hi);
            const int idx_lo_ref = getIdx(pivot.sat, a.band_lo);

            WlConstraint wc;
            wc.dd_wl_int = a.mw_int - pivot.mw_int;
            wc.sat_i  = a.sat;       wc.sat_ref = pivot.sat;
            wc.sig_hi = a.sig_hi;    wc.sig_lo  = a.sig_lo;
            wc.arc_key_i = a.stats_key;  wc.arc_key_ref = pivot.stats_key;

            // Coefficients per the derivation in the section header:
            //   sat_i:    +alpha(hi) on W_hi_i,    -alpha(lo) on W_lo_i
            //   sat_ref:  -alpha(hi) on W_hi_ref,  +alpha(lo) on W_lo_ref
            if (a_hi != 0.0 && idx_hi_i   >= 0) {
                wc.coeffs.push_back({ idx_hi_i,   +a_hi });
                wc.non_pivot_wl_indices.push_back(idx_hi_i);
            }
            if (a_lo != 0.0 && idx_lo_i   >= 0) {
                wc.coeffs.push_back({ idx_lo_i,   -a_lo });
                wc.non_pivot_wl_indices.push_back(idx_lo_i);
            }
            if (a_hi != 0.0 && idx_hi_ref >= 0) {
                wc.coeffs.push_back({ idx_hi_ref, -a_hi });
            }
            if (a_lo != 0.0 && idx_lo_ref >= 0) {
                wc.coeffs.push_back({ idx_lo_ref, +a_lo });
            }
            if (wc.coeffs.empty()) continue;  // safety: shouldn't happen.

            double pred = 0.0;
            for (const auto& [idx, coeff] : wc.coeffs) pred += coeff * x(idx);
            wc.predicted_dd = pred;

            constraints.push_back(std::move(wc));
        }
    }

    if (constraints.empty()) {
        log->debug("WL-NlWl: no usable DD WL constraints");
        return result;
    }

    // ------------------------------------------------------------------
    // FFR acceptance gate and innovation screen: the two guards resolveWlOnly
    // carries, for the same reason (SPEC_clock_temporal_anchor §8.11).  This
    // path is the NlWl counterpart of the per-band path: it also asserts MW
    // integers with no LAMBDA search, so its `ratio` is the "+inf = no ratio
    // test ran" sentinel, and without these guards only the per-arc
    // `wl_max_frac` point test above stands behind it, which cannot bound
    // P(integer is wrong).
    //
    // Order matters: FFR first (drop arcs that cannot be bounded), innovation
    // second (drop assertions the float contradicts).  They answer different
    // questions, random rounding failure against systematic frame
    // disagreement, and the recorded failure passed the first with
    // p_succ = 1.0 and was caught only by the second.
    // Both are computed on every call and acted on only when armed (see
    // resolveWlOnly).
    double nlwl_p_succ = 0.0;
    {
        std::set<std::string> arcs0;
        for (const auto& c : constraints) {
            arcs0.insert(c.arc_key_i);
            arcs0.insert(c.arc_key_ref);
        }
        nlwl_p_succ = wlArcSuccessRate_(arcs0, wl_quality, nullptr);
    }
    if (config_.wl_only_ffr) {
        while (!constraints.empty()) {
            std::set<std::string> arcs;
            for (const auto& c : constraints) {
                arcs.insert(c.arc_key_i);
                arcs.insert(c.arc_key_ref);
            }
            std::string worst;
            nlwl_p_succ = wlArcSuccessRate_(arcs, wl_quality, &worst);
            if (nlwl_p_succ >= config_.min_success_rate) break;
            if (worst.empty()) { constraints.clear(); break; }
            const std::size_t before = constraints.size();
            constraints.erase(
                std::remove_if(constraints.begin(), constraints.end(),
                               [&](const WlConstraint& c) {
                                   return c.arc_key_i == worst || c.arc_key_ref == worst;
                               }),
                constraints.end());
            log->debug("WL-NlWl (FFR): P_s={:.6f} < {:.6f}; dropping arc '{}' ({} -> {})",
                       nlwl_p_succ, config_.min_success_rate, worst,
                       before, constraints.size());
        }
        if (constraints.empty()) {
            log->info("WL-NlWl: no MW WL constraint set reaches the bootstrap success "
                      "rate {:.4f} (FFR gate) — staying FLOAT this epoch.",
                      config_.min_success_rate);
            result.gate_reason = 5;   // the guards rejected the set; MLAMBDA never ran
            return result;
        }
    }

    double nlwl_max_innov = 0.0;
    for (const auto& c : constraints)      // measured on every arm, armed or not
        nlwl_max_innov = std::max(nlwl_max_innov,
                                  std::abs(static_cast<double>(c.dd_wl_int) - c.predicted_dd));
    if (config_.wl_only_max_innov_cyc > 0.0) {
        const std::size_t before = constraints.size();
        int n_over = 0;
        double worst_over = 0.0;
        std::string worst_id;
        constraints.erase(
            std::remove_if(constraints.begin(), constraints.end(),
                           [&](const WlConstraint& c) {
                               // predicted_dd was formed from x above with the
                               // coefficients the H row uses, so this is the
                               // innovation the update applies.
                               const double innov =
                                   std::abs(static_cast<double>(c.dd_wl_int) - c.predicted_dd);
                               if (innov > nlwl_max_innov) nlwl_max_innov = innov;
                               if (innov > config_.wl_only_max_innov_cyc) {
                                   ++n_over;
                                   if (innov > worst_over) {
                                       worst_over = innov;
                                       worst_id = c.sat_i + "-" + c.sat_ref + " "
                                                + c.sig_hi + "/" + c.sig_lo;
                                   }
                                   return true;
                               }
                               return false;
                           }),
            constraints.end());
        if (n_over > 0) {
            log->warn("WL-NlWl: dropped {} of {} MW WL constraint(s) whose asserted integer "
                      "disagrees with the float by more than {:.2f} cyc (worst {:.3f} cyc on "
                      "{}).  An innovation this large is a FRAME conflict, not a rounding "
                      "question.",
                      n_over, before, config_.wl_only_max_innov_cyc, worst_over, worst_id);
        }
        if (constraints.empty()) {
            log->info("WL-NlWl: every MW WL constraint failed the innovation screen — "
                      "staying FLOAT this epoch.");
            result.gate_reason = 5;   // the guards rejected the set; MLAMBDA never ran
            return result;
        }
    }

    // Cross-frequency DD WL parity (consistency) check.
    //
    // For triple-frequency satellites, the three separately resolved WL DDs
    // covering bands (a, b), (a, c), (b, c) must satisfy
    //
    //     dd_canon(a,b) − dd_canon(a,c) + dd_canon(b,c) = 0     (exactly)
    //
    // by construction (telescoping cycle counts), where dd_canon(a,b) is
    // the SD WL value with N_<min_band> − N_<max_band> sign convention.
    // A non-zero parity at integer scale means at least one of the three
    // MW arcs has the wrong integer for this sat-pair this epoch.  Which
    // one cannot be told without more information, so all three are
    // rejected.
    //
    // The MW dd_wl_int is stored with sign convention "N_hi_freq − N_lo_freq"
    // (whichever band has higher carrier frequency).  It is converted to
    // canonical before the parity check by flipping the sign when the
    // higher-frequency band is not the lower-numbered band (e.g. Galileo
    // E5a/E5b: E5b at band 7 is higher frequency than E5a at band 5).
    {
        struct PairKey {
            std::string sat_i, sat_ref;
            bool operator<(const PairKey& o) const {
                return std::tie(sat_i, sat_ref) < std::tie(o.sat_i, o.sat_ref);
            }
        };
        struct CanonEntry {
            std::size_t constraint_idx;
            int         canon_dd;        // N_<lower_band> − N_<higher_band>
        };
        // Per (sat_i, sat_ref): map of (canon_a, canon_b) → entry
        std::map<PairKey, std::map<std::pair<char, char>, CanonEntry>> per_pair;
        for (std::size_t k = 0; k < constraints.size(); ++k) {
            const auto& c = constraints[k];
            if (c.sig_hi.size() < 2 || c.sig_lo.size() < 2) continue;
            const char b_hi = c.sig_hi[1];
            const char b_lo = c.sig_lo[1];
            const char a_min = std::min(b_hi, b_lo);
            const char a_max = std::max(b_hi, b_lo);
            // canon_dd uses min-band first; if hi-freq is the min band the
            // sign matches MW directly, otherwise flip.
            const int sign  = (b_hi == a_min) ? +1 : -1;
            per_pair[{ c.sat_i, c.sat_ref }]
                    [{ a_min, a_max }] = { k, sign * c.dd_wl_int };
        }

        std::set<std::size_t> rejected_idx;
        int n_parity_rejections = 0;
        for (const auto& [pk, band_map] : per_pair) {
            // Need at least 3 entries to even attempt parity.
            if (band_map.size() < 3) continue;

            // Collect distinct bands present for this sat-pair.
            std::set<char> bands;
            for (const auto& [bp, _e] : band_map) {
                bands.insert(bp.first);
                bands.insert(bp.second);
            }
            std::vector<char> band_list(bands.begin(), bands.end());

            // For each triplet (a < b < c), check if all three pairs
            // (a,b), (a,c), (b,c) are present and verify parity.
            for (std::size_t i = 0; i < band_list.size(); ++i)
            for (std::size_t j = i + 1; j < band_list.size(); ++j)
            for (std::size_t l = j + 1; l < band_list.size(); ++l) {
                const char a = band_list[i];
                const char b = band_list[j];
                const char d = band_list[l];
                auto it_ab = band_map.find({ a, b });
                auto it_ad = band_map.find({ a, d });
                auto it_bd = band_map.find({ b, d });
                if (it_ab == band_map.end() || it_ad == band_map.end()
                    || it_bd == band_map.end()) continue;
                const int parity = it_ab->second.canon_dd
                                 - it_ad->second.canon_dd
                                 + it_bd->second.canon_dd;
                if (parity != 0) {
                    log->warn("WL-NlWl: parity mismatch for {}-{} bands {}/{}/{}: "
                              "DD({},{})={} − DD({},{})={} + DD({},{})={} = {} cyc; "
                              "rejecting all three constraints this epoch",
                              pk.sat_i, pk.sat_ref, a, b, d,
                              a, b, it_ab->second.canon_dd,
                              a, d, it_ad->second.canon_dd,
                              b, d, it_bd->second.canon_dd, parity);
                    rejected_idx.insert(it_ab->second.constraint_idx);
                    rejected_idx.insert(it_ad->second.constraint_idx);
                    rejected_idx.insert(it_bd->second.constraint_idx);
                    ++n_parity_rejections;
                }
            }
        }

        if (!rejected_idx.empty()) {
            std::vector<WlConstraint> kept;
            kept.reserve(constraints.size() - rejected_idx.size());
            for (std::size_t k = 0; k < constraints.size(); ++k) {
                if (!rejected_idx.count(k)) kept.push_back(std::move(constraints[k]));
            }
            constraints = std::move(kept);
            log->info("WL-NlWl: dropped {} constraints across {} parity-mismatched triplets",
                      rejected_idx.size(), n_parity_rejections);
        }
    }

    if (constraints.empty()) {
        log->debug("WL-NlWl: all DD WL constraints rejected by parity check");
        return result;
    }

    // Slip-snap covariance inflation.  When a DD WL integer changes since
    // the last call, MW has re-locked at a different value, almost
    // certainly because of a slip on one of the two carriers.  Inflate
    // every non-pivot WL state column that participates in the new
    // constraint so the Kalman gain on the affected states is large
    // enough to snap to the new integer in a single epoch.  The AR pivot's
    // columns are left alone; they are BSSD-frozen when the AR pivot is
    // also the BSSD pivot.
    constexpr double kAmbResetVarCycles = 144.0;  // (12 cyc)², order of the L1/L2 reset
    Eigen::MatrixXd P_local = P;
    int n_reset = 0;
    auto inflate_state = [&](int idx) {
        if (idx < 0 || idx >= P_local.rows()) return;
        P_local.row(idx).setZero();
        P_local.col(idx).setZero();
        P_local(idx, idx) = kAmbResetVarCycles;
    };
    for (const auto& c : constraints) {
        const std::string key =
            c.sat_i + "_" + c.sat_ref + "_" + c.sig_hi + "_" + c.sig_lo;
        auto prev_it = previous_dd_wl_.find(key);
        if (prev_it == previous_dd_wl_.end()) continue;
        if (prev_it->second == c.dd_wl_int) continue;
        log->warn("WL integer changed for DD {}-{} {}/{}: {} → {} cycles. "
                  "Inflating non-pivot WL state columns to allow snap.",
                  c.sat_i, c.sat_ref, c.sig_hi, c.sig_lo,
                  prev_it->second, c.dd_wl_int);
        for (int idx : c.non_pivot_wl_indices) inflate_state(idx);
        ++n_reset;
    }

    // Build the pseudo-observation Kalman update.  Each row contains
    // 1–4 non-zero coefficients depending on the (band_hi, band_lo, R)
    // pattern and whether the AR-pivot's state columns are BSSD-frozen.
    const int nx = static_cast<int>(x.size());
    const int nv = static_cast<int>(constraints.size());
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(nv, nx);
    Eigen::VectorXd v = Eigen::VectorXd::Zero(nv);
    for (int k = 0; k < nv; ++k) {
        const auto& c = constraints[k];
        for (const auto& [idx, coeff] : c.coeffs) {
            if (idx >= 0 && idx < nx) H(k, idx) += coeff;
        }
        v(k) = static_cast<double>(c.dd_wl_int) - c.predicted_dd;
    }

    Eigen::MatrixXd R   = Eigen::MatrixXd::Identity(nv, nv) * config_.wl_only_pseudo_var;
    KalmanFilter::josephPseudoUpdate(x, P_local, H, v, R,
                                     result.fixed_state, result.fixed_covariance);

    result.success     = true;
    result.num_fixed   = nv;
    result.ratio       = std::numeric_limits<double>::infinity();  // sentinel: no LAMBDA ratio test ran
    result.wl_only_fix = true;
    result.p_succ      = nlwl_p_succ;
    // Both guards stand behind this path, as behind resolveWlOnly (SPEC §8.11.6
    // gap 1).  p_succ carries the realized bootstrap rate, so the trace has a
    // confidence figure besides the sentinel.

    for (const auto& c : constraints) {
        const std::string key =
            c.sat_i + "_" + c.sat_ref + "_" + c.sig_hi + "_" + c.sig_lo;
        previous_dd_wl_[key] = c.dd_wl_int;
    }

    // Bookkeeping for the log: count the constraints that exercise an
    // EWL-style multi-state coefficient pattern (no band == ref_band on
    // the non-pivot side, so both alpha(hi) and alpha(lo) are non-zero).
    int n_ewl = 0;
    for (const auto& c : constraints) {
        if (c.non_pivot_wl_indices.size() >= 2) ++n_ewl;
    }
    log->info("WL-NlWl: applied {} DD WL constraint(s) ({} EWL-style multi-state){}; "
              "P_s={:.6f}, max|innov| seen (PRE-screen) {:.4f} cyc",
              nv, n_ewl,
              n_reset > 0 ? fmt::format(", {} with reset", n_reset) : "",
              nlwl_p_succ, nlwl_max_innov);
    return result;
}

// ===========================================================================
// Fix-and-hold
// ===========================================================================

void AmbiguityResolver::holdAmbiguities(StateManager& state_manager,
                                        const AmbiguityResolution& resolution) {
    auto log = log_manager_.getLogger("ambiguity_resolver");

    if (!resolution.success || config_.ar_mode != 3) return;

    // NlWl basis: fix-and-hold needs LAMBDA-fixed integers from an NL search,
    // which is not implemented.  The MW-WL pseudo-observation already applies
    // the WL constraint inside resolve(), and a separate hold pass would only
    // re-apply the same information.  This stays a no-op until a LAMBDA NL
    // search exists alongside @c applyMwWlConstraintsNlWl_.
    if (config_.state_ambiguity_basis == StateAmbiguityBasis::NlWl) {
        log->debug("holdAmbiguities skipped: NlWl basis (LAMBDA NL fix not yet implemented)");
        return;
    }

    // The held set is the rows the fix constrained.  Rebuilding the DD pairs here
    // from the state alone has no observations, so every satellite would get
    // elevation 0 and the AR elevation mask would drop each pair; the fix itself
    // already holds the epoch's candidate set, pivots and mask.  A fix that
    // constrained no single-signal DD (WL-only) leaves the rows empty.
    const auto& rows = resolution.fixed_dd_rows;
    if (rows.empty()) {
        log->debug("Fix-and-hold skipped: the accepted fix constrained no single-signal DD ambiguity");
        return;
    }

    const int nb = static_cast<int>(rows.size());
    const int nx = static_cast<int>(state_manager.getStateSize());
    Eigen::VectorXd x = state_manager.getStateVector();
    Eigen::MatrixXd P = state_manager.getCovariance();

    // Build pseudo-observation: v = (xa[i]-xa[ref]) - (x[i]-x[ref])
    // where xa is the fixed state
    Eigen::VectorXd v = Eigen::VectorXd::Zero(nb);
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(nb, nx);
    int nv = 0;

    for (int k = 0; k < nb; ++k) {
        int idx_i = rows[k].state_idx_i;
        int idx_r = rows[k].state_idx_ref;

        if (idx_i < 0 || idx_r < 0 || idx_i >= nx || idx_r >= nx) continue;
        if (idx_i >= resolution.fixed_state.size() ||
            idx_r >= resolution.fixed_state.size()) continue;

        double dd_fixed = resolution.fixed_state(idx_i) - resolution.fixed_state(idx_r);
        double dd_float = x(idx_i) - x(idx_r);

        v(nv) = dd_fixed - dd_float;
        H(nv, idx_i) = 1.0;
        H(nv, idx_r) = -1.0;
        ++nv;
    }

    if (nv == 0) return;

    v.conservativeResize(nv);
    H.conservativeResize(nv, nx);

    // Kalman update with hold variance
    Eigen::MatrixXd R = Eigen::MatrixXd::Identity(nv, nv) * config_.hold_variance;
    KalmanFilter::josephPseudoUpdate(x, P, H, v, R, x, P);

    state_manager.setStateVector(x);
    state_manager.setCovariance(P);

    log->info("Fix-and-hold applied: {} DD constraints, hold_var={:.4f}", nv, config_.hold_variance);
}

// ===========================================================================
// Fix-and-hold for baseline (DD) state
//
// Each ambiguity row is a single selector H(k, dd_idx)=1 instead of the
// (i, ref) row used by the SD path, because the state value already is the DD.
// ===========================================================================

void AmbiguityResolver::holdAmbiguitiesDd(StateManager& state_manager,
                                          const AmbiguityResolution& resolution) {
    auto log = log_manager_.getLogger("ambiguity_resolver");

    if (!resolution.success || config_.ar_mode != 3) return;

    // Collect every DD-keyed ambiguity index that is in range of the
    // resolution's fixed_state.  Only the range is checked: an ambiguity added
    // between resolveDd() and this call is skipped when its index falls outside
    // fixed_state, since its row would reference a non-existent fixed value.
    std::vector<int> dd_indices;
    const auto& amb_map = state_manager.getAmbiguities();
    const Eigen::Index n_fixed = resolution.fixed_state.size();
    for (const auto& [key, idx] : amb_map) {
        if (key.compare(0, 3, "DD|") != 0) continue;
        const Eigen::Index ei = static_cast<Eigen::Index>(idx);
        if (ei < 0 || ei >= n_fixed) continue;
        dd_indices.push_back(static_cast<int>(idx));
    }

    if (dd_indices.empty()) {
        log->debug("Fix-and-hold (DD): no DD ambiguities in state, skipping");
        return;
    }

    const int nb = static_cast<int>(dd_indices.size());
    const int nx = static_cast<int>(state_manager.getStateSize());
    Eigen::VectorXd x = state_manager.getStateVector();
    Eigen::MatrixXd P = state_manager.getCovariance();

    // Pseudo-observation v(k) = fixed_state(dd_k) - x(dd_k).
    // H(k, dd_k) = 1: a selector rather than a difference.
    Eigen::VectorXd v(nb);
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(nb, nx);
    for (int k = 0; k < nb; ++k) {
        const int idx = dd_indices[k];
        v(k) = resolution.fixed_state(idx) - x(idx);
        H(k, idx) = 1.0;
    }

    Eigen::MatrixXd R = Eigen::MatrixXd::Identity(nb, nb) * config_.hold_variance;
    KalmanFilter::josephPseudoUpdate(x, P, H, v, R, x, P);

    state_manager.setStateVector(x);
    state_manager.setCovariance(P);

    log->info("Fix-and-hold (DD) applied: {} ambiguities, hold_var={:.4f}",
              nb, config_.hold_variance);
}

} // namespace GnssProcessor

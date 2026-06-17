/**
 * @file AmbiguityResolver.cpp
 * @brief MLAMBDA integer ambiguity resolution implementation.
 *
 * Implements the Modified LAMBDA method following Chang et al. (2005)
 * and integrates it with the GnssProcessor Kalman filter for PPP-RTK
 * integer ambiguity resolution and fix-and-hold.
 */

#include "filter/AmbiguityResolver.hpp"
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

bool AmbiguityResolver::ldlDecompose(const Eigen::MatrixXd& Q,
                                     Eigen::MatrixXd& L,
                                     Eigen::VectorXd& d) {
    const int n = static_cast<int>(Q.rows());
    L = Eigen::MatrixXd::Zero(n, n);
    d = Eigen::VectorXd::Zero(n);

    Eigen::MatrixXd A = Q;

    for (int i = n - 1; i >= 0; --i) {
        d(i) = A(i, i);
        if (d(i) <= 0.0) return false; // not positive definite

        const double sqrt_d = std::sqrt(d(i));
        for (int j = 0; j <= i; ++j) {
            L(i, j) = A(i, j) / sqrt_d;
        }
        for (int j = 0; j < i; ++j) {
            for (int k = 0; k <= j; ++k) {
                A(j, k) -= L(i, k) * L(i, j);
            }
        }
        for (int j = 0; j <= i; ++j) {
            L(i, j) /= L(i, i);
        }
    }

    return true;
}

// ===========================================================================
// MLAMBDA lattice reduction (decorrelation)
// ===========================================================================

void AmbiguityResolver::reduction(Eigen::MatrixXd& L, Eigen::VectorXd& d,
                                  Eigen::MatrixXd& Z) {
    const int n = static_cast<int>(d.size());
    Z = Eigen::MatrixXd::Identity(n, n);

    int j = n - 2;
    int sw = n - 2;

    while (j >= 0) {
        // Gauss transformations
        if (j <= sw) {
            for (int i = j + 1; i < n; ++i) {
                double mu = std::round(L(i, j));
                if (mu != 0.0) {
                    for (int k = i; k < n; ++k) {
                        L(k, j) -= mu * L(k, i);
                    }
                    for (int k = 0; k < n; ++k) {
                        Z(k, j) -= mu * Z(k, i);
                    }
                }
            }
        }

        // Permutation test
        double delta = d(j) + L(j + 1, j) * L(j + 1, j) * d(j + 1);
        if (delta + 1e-6 < d(j + 1)) {
            // Permute columns j and j+1
            double eta = d(j) / delta;
            double lam = d(j + 1) * L(j + 1, j) / delta;

            d(j) = eta * d(j + 1);
            d(j + 1) = delta;

            // Update L rows j and j+1 for columns 0..j-1
            Eigen::RowVectorXd Lj_row = L.row(j).head(j).eval();
            Eigen::RowVectorXd Lj1_row = L.row(j + 1).head(j).eval();
            L.row(j).head(j) = -L(j + 1, j) * Lj_row + Lj1_row;
            L.row(j + 1).head(j) = eta * Lj_row + lam * Lj1_row;

            L(j + 1, j) = lam;

            // Swap L columns j,j+1 for rows j+2..n-1
            for (int k = j + 2; k < n; ++k) {
                std::swap(L(k, j), L(k, j + 1));
            }
            // Swap Z columns j,j+1
            Z.col(j).swap(Z.col(j + 1));

            sw = j;
            j = n - 2;
        } else {
            --j;
        }
    }
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
    const int n = static_cast<int>(d.size());
    int nn = 0;
    int imax = 0;
    double Chi2 = 1e18;

    Eigen::MatrixXd S = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd dist = Eigen::VectorXd::Zero(n);
    Eigen::VectorXd zb = Eigen::VectorXd::Zero(n);
    Eigen::VectorXd z = Eigen::VectorXd::Zero(n);
    Eigen::VectorXd step = Eigen::VectorXd::Zero(n);
    zn = Eigen::MatrixXd::Zero(n, m);
    sq = Eigen::VectorXd::Constant(m, 0.0);

    int k = n - 1;
    zb(k) = zs(k);
    z(k) = std::round(zb(k));
    double y = zb(k) - z(k);
    step(k) = (y >= 0.0) ? 1.0 : -1.0;

    constexpr int kMaxIter = 10000;
    for (int iter = 0; iter < kMaxIter; ++iter) {
        double newdist = dist(k) + y * y / d(k);

        if (newdist < Chi2) {
            if (k != 0) {
                --k;
                dist(k) = newdist;
                for (int i = 0; i <= k; ++i) {
                    S(k, i) = S(k + 1, i) + (z(k + 1) - zb(k + 1)) * L(k + 1, i);
                }
                zb(k) = zs(k) + S(k, k);
                z(k) = std::round(zb(k));
                y = zb(k) - z(k);
                step(k) = (y >= 0.0) ? 1.0 : -1.0;
            } else {
                // Found a candidate
                if (nn < m) {
                    if (nn == 0 || newdist > sq(imax)) {
                        imax = nn;
                    }
                    zn.col(nn) = z;
                    sq(nn) = newdist;
                    ++nn;
                } else {
                    if (newdist < sq(imax)) {
                        zn.col(imax) = z;
                        sq(imax) = newdist;
                        imax = 0;
                        for (int i = 1; i < m; ++i) {
                            if (sq(i) > sq(imax)) imax = i;
                        }
                    }
                    Chi2 = sq(imax);
                }
                z(0) += step(0);
                y = zb(0) - z(0);
                step(0) = -step(0) - ((step(0) >= 0.0) ? 1.0 : -1.0);
            }
        } else {
            if (k == n - 1) break;
            ++k;
            z(k) += step(k);
            y = zb(k) - z(k);
            step(k) = -step(k) - ((step(k) >= 0.0) ? 1.0 : -1.0);
        }
    }

    // Sort by ascending squared distance
    if (nn >= 2) {
        std::vector<int> order(nn);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(),
                  [&sq](int a, int b) { return sq(a) < sq(b); });
        Eigen::MatrixXd zn_sorted = zn;
        Eigen::VectorXd sq_sorted = sq;
        for (int i = 0; i < nn; ++i) {
            zn_sorted.col(i) = zn.col(order[i]);
            sq_sorted(i) = sq(order[i]);
        }
        zn = zn_sorted;
        sq = sq_sorted;
    }

    return nn;
}

// ===========================================================================
// Full MLAMBDA
// ===========================================================================

bool AmbiguityResolver::mlambda(const Eigen::VectorXd& a,
                                const Eigen::MatrixXd& Q,
                                Eigen::VectorXd& afix,
                                Eigen::VectorXd& s,
                                Eigen::VectorXd* d_out) {
    Eigen::MatrixXd L;
    Eigen::VectorXd d;
    if (!ldlDecompose(Q, L, d)) return false;

    Eigen::MatrixXd Z;
    reduction(L, d, Z);

    // Z-transform the float ambiguities
    Eigen::VectorXd z_float = Z.transpose() * a;

    // ILS search for 2 best candidates
    Eigen::MatrixXd E;
    Eigen::VectorXd sq;
    int found = search(L, d, z_float, E, sq, 2);
    if (found < 2) return false;

    s = sq.head(2);
    if (d_out) *d_out = d;  // post-reduction diagonal — caller may compute P_s,boot

    // Back-transform best candidate: afix = inv(Z^T) * E(:,0)
    // Since Z is integer, inv(Z^T) = round(inv(Z^T))
    Eigen::MatrixXd Zt_inv = Z.transpose().inverse();
    // Round to nearest integer to clean up numerical noise
    for (int i = 0; i < Zt_inv.rows(); ++i) {
        for (int j = 0; j < Zt_inv.cols(); ++j) {
            Zt_inv(i, j) = std::round(Zt_inv(i, j));
        }
    }
    afix = Zt_inv * E.col(0);

    return true;
}

// ===========================================================================
// Bootstrap-rounding success rate
// ===========================================================================

double AmbiguityResolver::computeBootstrapSuccessRate(const Eigen::VectorXd& d) {
    // P_s,boot = Π_i  erf( 1 / (2·√(2·d_i)) )
    // Per-dimension: P(|N(0, d_i)| < 0.5) = erf( 0.5 / (√d_i · √2) )
    if (d.size() == 0) return 0.0;
    constexpr double kInv2Sqrt2 = 0.353553390593273762200422181052424519;  // 1/(2√2)
    double p = 1.0;
    for (int i = 0; i < d.size(); ++i) {
        if (d(i) <= 0.0) return 0.0;
        p *= std::erf(kInv2Sqrt2 / std::sqrt(d(i)));
        if (p <= 0.0) return 0.0;
    }
    return p;
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
        // splits on the FIRST underscore, preserving iono-free combos
        // ("L1C_L5Q") in the signal field unchanged.
        const auto parsed = StateKeys::parseAmbiguityKey(key);
        if (!parsed) continue;

        AmbInfo info;
        info.key = key;
        info.satellite = parsed->satellite;
        info.signal = parsed->signal;
        info.constellation = info.satellite.substr(0, 1);
        info.state_idx = static_cast<int>(idx);

        // Two separate filters with documented intent:
        //   (1) value == 0  → uninitialized seed.  computeAmbiguitySeed()
        //       returns 0 when the obs/modeled maps don't have the signal,
        //       so the float ambiguity hasn't yet been informed by data.
        //       LAMBDA candidate would be undefined.
        //   (2) variance == 0 → frozen BSSD pivot.  The pivot's row/col
        //       are zeroed by BssdPivotManager::freezePivotAmbiguityCovariance
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

        // Skip unresolvable ambiguities � missing BIA/ATX coverage means
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
// Resolve (no WL constraint � forwards to constrained with empty map)
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
    const std::map<std::string, double>& wl_fractions) {

    auto log = log_manager_.getLogger("ambiguity_resolver");
    AmbiguityResolution result;

    if (config_.ar_mode == 0) {
        log->debug("AR disabled (ar_mode=0)");
        return result;
    }

    // Convergence guard 1 — minimum-epochs gate.  AR runs LAMBDA on the
    // float covariance; a marginal fix accepted before the float covariance
    // has settled tends to lock onto wrong integers, and once a wrong
    // integer is constrained into the state the next LAMBDA call sees a
    // degenerate covariance direction and produces inflated "ultra-confident"
    // ratios that are mathematically pathological.  Skip the first
    // @c min_epochs invocations.
    ++epoch_count_;
    if (epoch_count_ <= config_.min_epochs) {
        log->debug("AR skipped: epoch {}/{} (min_epochs convergence guard)",
                   epoch_count_, config_.min_epochs);
        return result;
    }

    // NlWl basis: skip the per-band DD-pair / LAMBDA pipeline entirely.
    // The state vector holds NL/WL ambiguities directly, so there are no
    // per-(sat, signal) DD pairs to enumerate.  Today we apply MW-resolved
    // WL integers as 1-row pseudo-observations and stop there — full
    // LAMBDA NL search with WL pre-constrained is future work (see CLAUDE.md
    // notes on the WL-fix-with-tight-coupling milestone).
    if (config_.state_ambiguity_basis == StateAmbiguityBasis::NlWl) {
        const Eigen::VectorXd x = state_manager.getStateVector();
        const Eigen::MatrixXd P = state_manager.getCovariance();
        if (P.rows() >= 3) {
            const double sigma_3d = std::sqrt(
                std::max(P(0, 0), 0.0) + std::max(P(1, 1), 0.0) + std::max(P(2, 2), 0.0));
            if (sigma_3d > config_.max_position_sigma_m) {
                log->debug("AR (NlWl) skipped: 3D position σ={:.4f} m > {:.4f} m (sigma gate)",
                           sigma_3d, config_.max_position_sigma_m);
                return result;
            }
        }
        return applyMwWlConstraintsNlWl_(state_manager, x, P,
                                          wl_ambiguities, wl_fractions, observations);
    }

    // Build DD pairs
    auto pairs = buildDdPairs(state_manager, observations);
    result.num_candidates = static_cast<int>(pairs.size());

    if (pairs.empty()) {
        log->debug("No DD ambiguity pairs available for AR");
        return result;
    }

    const Eigen::VectorXd x = state_manager.getStateVector();
    const Eigen::MatrixXd P = state_manager.getCovariance();

    // Convergence guard 2 — formal position-σ gate.  Complements the
    // min-epochs gate by using actual filter confidence rather than wall
    // time.  When the 3D position σ is wide, the ambiguity covariance is
    // also typically still wide and the integer search isn't reliable.
    if (P.rows() >= 3) {
        const double sigma_3d = std::sqrt(
            std::max(P(0, 0), 0.0) + std::max(P(1, 1), 0.0) + std::max(P(2, 2), 0.0));
        if (sigma_3d > config_.max_position_sigma_m) {
            log->debug("AR skipped: 3D position σ={:.4f} m > {:.4f} m (sigma gate)",
                       sigma_3d, config_.max_position_sigma_m);
            return result;
        }
    }

    // WL-only path: apply MW-resolved WL integers as Kalman pseudo-observations
    // and return without running any LAMBDA NL search.  Use this as a
    // conservative first stage to verify the WL pipeline is healthy before
    // enabling full WL+NL LAMBDA fixes.  No fall-through to LAMBDA paths
    // when this mode is on — staying WL-only is the whole point.
    if (config_.wl_only) {
        if (wl_ambiguities.empty()) {
            log->debug("WL-only mode: no MW-resolved WL integers available this epoch");
            return result;
        }
        auto wl_only_result = resolveWlOnly(pairs, x, P, wl_ambiguities,
                                            wl_fractions, observations);
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
        // WL-constrained AR failed — stay float.
        //
        // Don't fall through to the unconstrained LAMBDA fallback below.
        // The fallback would search over the full per-band DD set, which
        // includes DDs whose corresponding WL is NOT in @c wl_ambiguities
        // (no MW lock, or |frac| above the cap, or BIA gap on the WL
        // pair-mate).  Searching over those DDs lets LAMBDA pick integer
        // combinations whose implied (N_1 − N_2) contradicts MW's locked
        // WL — a wrong-WL fix is worse than no fix.
        //
        // The hierarchy is: WL is the easier integer to fix (long λ_WL,
        // robust under BSSD).  If a sat's WL hasn't been locked, its NL
        // can't be reliably fixed either — same model dependencies (BIA,
        // antenna, wind-up).  So "no NL AR for sats whose WL is float" is
        // the right enforcement, and the unconstrained LAMBDA breaks it.
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

    if (result.ratio < config_.ratio_threshold) {
        log->info("AR ratio test failed: {:.2f} < {:.1f}", result.ratio, config_.ratio_threshold);
        return result;
    }
    if (result.ratio > config_.ratio_upper_bound) {
        log->warn("AR rejecting fix: ratio={:.3e} > upper bound {:.1e} "
                  "(degenerate covariance — staying float).",
                  result.ratio, config_.ratio_upper_bound);
        return result;
    }

    // FFR gate: bootstrap success rate.
    const double p_s = computeBootstrapSuccessRate(d_lambda);
    if (p_s < config_.min_success_rate) {
        log->info("AR rejecting fix despite passing ratio ({:.2f}): "
                  "bootstrap P_s={:.5f} < min_success_rate={:.4f}",
                  result.ratio, p_s, config_.min_success_rate);
        return result;
    }

    // Per-DD residual cap: refuse the fix if any individual DD's
    // |float − fixed| (in cycles) exceeds the cap.
    double max_dd_frac = 0.0;
    int    worst_dd_k  = -1;
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

    // Fix succeeded — apply the integer fix as a Kalman pseudo-observation
    // update rather than via back-substitution.
    //
    // The legacy back-sub form
    //     x_fixed = x − Qab · Qb⁻¹ · (y_dd − afix)
    //     P_fixed = P  − Qab · Qb⁻¹ · Qab'
    // is a perfect-measurement (R = 0) update.  Once it succeeds, the
    // ambiguity rows of P_fixed have ~zero variance and the next epoch's
    // LAMBDA call sees a near-singular Qb in the integer search,
    // producing meaningless ratios that drive the filter into the
    // upper-bound rejection guard from there on.  Joseph-form Kalman
    // with R = hold_variance keeps the covariance well-conditioned
    // and lets subsequent epochs either re-confirm the fix (asymptoting
    // toward the integer) or detect a slip via the residual.
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(nb, na);
    Eigen::VectorXd v = Eigen::VectorXd::Zero(nb);
    for (int k = 0; k < nb; ++k) {
        if (pairs[k].state_idx_i   >= 0 && pairs[k].state_idx_i   < na) H(k, pairs[k].state_idx_i)   = +1.0;
        if (pairs[k].state_idx_ref >= 0 && pairs[k].state_idx_ref < na) H(k, pairs[k].state_idx_ref) = -1.0;
        v(k) = afix(k) - y_dd(k);
    }

    Eigen::MatrixXd R   = Eigen::MatrixXd::Identity(nb, nb) * config_.hold_variance;
    KalmanFilter::josephPseudoUpdate(x, P, H, v, R,
                                     result.fixed_state, result.fixed_covariance);

    result.success = true;
    result.num_fixed = nb;

    log->info("AR succeeded (Kalman pseudo-obs, R={:.4f} m²): {} ambiguities fixed, "
              "ratio={:.2f}, P_s={:.5f}, max|frac|={:.3f} cyc",
              config_.hold_variance, nb, result.ratio, p_s, max_dd_frac);

    return result;
}

// ===========================================================================
// Resolve (baseline / DD path)
//
// The state vector already holds DD ambiguity floats keyed
// "DD|cons|sat_i|rov_code|pivot".  The MLAMBDA core expects integer-
// valued targets, so meter-valued floats are converted to cycles by
// dividing by the per-pair carrier wavelength before the search and
// the integer fix is multiplied back to meters for the back-substitution
// step.  No SD subtraction is applied — the state is already DD.
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

    // Convergence gates (mirror the SD path — see resolve() for rationale).
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

    // Build sat -> elevation lookup so we can apply the elevation mask.
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
        // Bail on any malformed key — the writer side guarantees this
        // format, so a mismatch likely means stale state from another
        // mode and should be ignored.
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

    // Float vector and covariance in METERS (the state's native units).
    Eigen::VectorXd y_meters(nb);
    Eigen::MatrixXd Qb_meters(nb, nb);
    for (int i = 0; i < nb; ++i) {
        y_meters(i) = x_full(entries[i].state_idx);
        for (int j = 0; j < nb; ++j) {
            Qb_meters(i, j) = P_full(entries[i].state_idx, entries[j].state_idx);
        }
    }
    Qb_meters = 0.5 * (Qb_meters + Qb_meters.transpose());

    // Convert to CYCLES for MLAMBDA.  diag(D) = 1/lambda is the
    // meters→cycles transform; symmetric scaling for the covariance.
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

    if (result.ratio < config_.ratio_threshold) {
        log->info("AR (DD) ratio test failed: {:.2f} < {:.1f}",
                  result.ratio, config_.ratio_threshold);
        return result;
    }
    if (result.ratio > config_.ratio_upper_bound) {
        log->warn("AR (DD) rejecting fix: ratio={:.3e} > upper bound {:.1e} "
                  "(degenerate covariance — staying float).",
                  result.ratio, config_.ratio_upper_bound);
        return result;
    }

    // FFR gate.
    const double p_s = computeBootstrapSuccessRate(d_lambda);
    if (p_s < config_.min_success_rate) {
        log->info("AR (DD) rejecting fix despite passing ratio ({:.2f}): "
                  "bootstrap P_s={:.5f} < min_success_rate={:.4f}",
                  result.ratio, p_s, config_.min_success_rate);
        return result;
    }

    // Per-DD residual cap.
    double max_dd_frac = 0.0;
    int    worst_dd_k  = -1;
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
        return result;
    }

    // Convert the integer fix back to METERS for the constraint.
    Eigen::VectorXd afix_meters(nb);
    for (int i = 0; i < nb; ++i) {
        afix_meters(i) = afix_cycles(i) * entries[i].lambda;
    }

    // Apply the integer fix as a Kalman pseudo-observation update
    // rather than via back-substitution.  See the equivalent comment
    // block in resolve() and resolveWlConstrained() for rationale —
    // the legacy R = 0 form makes the next epoch's LAMBDA degenerate.
    //
    // Baseline DD: each row is a single-column constraint (the state
    // itself IS the DD ambiguity, no SD subtraction).
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
    };
    std::vector<NlCandidate> nl_candidates;
    // Track which DD indices are consumed by NL pairs
    std::set<int> consumed_dd;

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

        const int dd_wl = it_i->second - it_ref->second;

        const double f1 = hi.freq;
        const double f2 = lo.freq;
        const double lambda_nl = Constants::SPEED_OF_LIGHT / (f1 + f2);

        nl_candidates.push_back({
            hi.pair_idx, lo.pair_idx,
            f1, f2, hi.wavelength, lo.wavelength,
            dd_wl, lambda_nl
        });
        consumed_dd.insert(hi.pair_idx);
        consumed_dd.insert(lo.pair_idx);
    }

    if (nl_candidates.empty()) {
        log->debug("WL-constrained AR: no dual-frequency pairs with known WL");
        return result;  // empty, caller falls through to standard LAMBDA
    }

    // Build the reduced NL ambiguity vector and covariance.
    //
    // For each NL candidate, the float DD NL ambiguity (in cycles) is:
    //   n_NL = (f1 * DD_1/lam1 + f2 * DD_2/lam2) / (f1+f2)
    //
    // where DD_1, DD_2 are the float DD ambiguities in meters.
    //
    // Only NL candidates enter the LAMBDA search.  Unpaired DD ambiguities
    // (single-signal DDs without a matching second frequency or WL integer)
    // are excluded � they cannot be properly resolved in the WL/NL
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
    // Also build the WL correction vector (for back-transformation)
    Eigen::VectorXd wl_correction = Eigen::VectorXd::Zero(n_orig);

    for (int k = 0; k < n_nl; ++k) {
        const auto& nlc = nl_candidates[k];
        const double f_sum = nlc.f1 + nlc.f2;

        // n_NL = (f1/lam1 * DD_1 + f2/lam2 * DD_2) / (f1+f2)
        // In the NL row, we want the result in NL cycles, so:
        //   row[dd_idx_1] = f1 / (lam1 * f_sum)
        //   row[dd_idx_2] = f2 / (lam2 * f_sum)
        T(k, nlc.dd_idx_1) = nlc.f1 / (nlc.lam1 * f_sum);
        T(k, nlc.dd_idx_2) = nlc.f2 / (nlc.lam2 * f_sum);

        // WL correction for back-transformation:
        // DD_1 [m] = (n_NL + f2/(f1+f2) * dd_wl) * lam1
        // DD_2 [m] = (n_NL - f1/(f1+f2) * dd_wl) * lam2
        wl_correction(nlc.dd_idx_1) = nlc.f2 / f_sum * nlc.dd_wl * nlc.lam1;
        wl_correction(nlc.dd_idx_2) = -nlc.f1 / f_sum * nlc.dd_wl * nlc.lam2;
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

    // Run MLAMBDA on the NL-only set
    // Shift y_reduced so MLAMBDA's integer search aligns with integer N_1.
    //
    // The IF-NL combination satisfies
    //   y_reduced(k) = (f1·N_1 + f2·N_2) / (f1+f2)
    //                = N_1 − [f2/(f1+f2)] · WL          (substituting N_2 = N_1−WL)
    //
    // which is NOT integer-valued in general — for GPS L1/L2 with WL=1 it
    // sits at N_1 − 0.4379, neither integer is meaningfully closer, and
    // the ratio test sees s2/s1 ≈ 1.6 regardless of float quality.  By
    // adding the known-WL offset back in we get a search variable that
    // IS integer in expectation:
    //
    //   y_for_mlambda(k) = y_reduced(k) + [f2/(f1+f2)] · WL_int  =  N_1 + noise
    //
    // We then back-shift afix_n1 to the IF-NL form so the existing
    // back-transform expression (which expects IF-NL) continues to work.
    // Q_reduced is unchanged: a constant per-row shift doesn't affect the
    // covariance, only the mean.
    Eigen::VectorXd nl_shift(n_nl);
    for (int k = 0; k < n_nl; ++k) {
        const auto& nlc = nl_candidates[k];
        nl_shift(k) = nlc.f2 / (nlc.f1 + nlc.f2) * static_cast<double>(nlc.dd_wl);
    }
    const Eigen::VectorXd y_for_mlambda = y_reduced + nl_shift;

    Eigen::VectorXd afix_n1;       // integer N_1 candidates (cycles)
    Eigen::VectorXd s;
    Eigen::VectorXd d_lambda;      // post-reduction LDL diagonal (for bootstrap P_s)
    if (!mlambda(y_for_mlambda, Q_reduced, afix_n1, s, &d_lambda)) {
        log->debug("WL-constrained AR: MLAMBDA failed on NL problem");
        return result;
    }
    // Convert back to the IF-NL representation expected by the
    // back-transform code below: afix_reduced = N_1 − [f2/(f1+f2)] · WL_int.
    Eigen::VectorXd afix_reduced = afix_n1 - nl_shift;

    if (s(0) <= 0.0 || s(1) <= 0.0) {
        log->debug("WL-constrained AR: invalid distances s1={:.6f} s2={:.6f}", s(0), s(1));
        return result;
    }

    result.ratio = s(1) / s(0);
    log->info("WL-constrained AR: {} NL ambs, ratio={:.2f} (threshold={:.1f})",
              n_nl, result.ratio, config_.ratio_threshold);

    if (result.ratio < config_.ratio_threshold) {
        log->info("WL-constrained AR ratio test failed: {:.2f} < {:.1f}",
                  result.ratio, config_.ratio_threshold);
        return result;
    }
    if (result.ratio > config_.ratio_upper_bound) {
        log->warn("WL-constrained AR rejecting fix: ratio={:.3e} > upper bound {:.1e} "
                  "(degenerate covariance — staying float).",
                  result.ratio, config_.ratio_upper_bound);
        return result;
    }

    // FFR gate: bootstrap success rate must exceed the configured target
    // (e.g. 0.999 for 0.1% probability of accepting a wrong-integer fix).
    // Self-tuning: hard to pass when the search is high-dimensional or
    // poorly conditioned; easy to pass when the float is well-converged
    // and the decorrelation is clean.
    const double p_s = computeBootstrapSuccessRate(d_lambda);
    if (p_s < config_.min_success_rate) {
        log->info("WL-constrained AR rejecting fix despite passing ratio "
                  "({:.2f}): bootstrap P_s={:.5f} < min_success_rate={:.4f}",
                  result.ratio, p_s, config_.min_success_rate);
        return result;
    }

    // Per-NL-pair residual cap: refuse the fix if any individual pair's
    // |float − fixed| (in cycles) exceeds the cap.  The joint ratio test
    // can pass on a borderline integer set where one or two pairs are
    // sitting near a half-cycle boundary; those marginal pairs cause the
    // ratio to collapse on the next epoch as the state drifts back
    // toward the "actual" integer.  Catching them up front is the right
    // defense.
    double max_pair_frac = 0.0;
    int    worst_pair_k  = -1;
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
        return result;
    }

    // Back-transform: recover fixed DD ambiguities in meters.
    // For NL-paired DDs:
    //   DD_1 [m] = (n_NL_fixed + f2/(f1+f2) * dd_wl) * lam1
    //   DD_2 [m] = (n_NL_fixed - f1/(f1+f2) * dd_wl) * lam2
    Eigen::VectorXd afix_orig(n_orig);
    afix_orig.setZero();

    for (int k = 0; k < n_nl; ++k) {
        const auto& nlc = nl_candidates[k];
        const double f_sum = nlc.f1 + nlc.f2;
        const double n_nl_fixed = afix_reduced(k);

        afix_orig(nlc.dd_idx_1) = (n_nl_fixed + nlc.f2 / f_sum * nlc.dd_wl) * nlc.lam1;
        afix_orig(nlc.dd_idx_2) = (n_nl_fixed - nlc.f1 / f_sum * nlc.dd_wl) * nlc.lam2;

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
    // rather than via direct state/covariance overwrite.
    //
    // The original implementation used the back-substitution form
    //     x_fixed = x − Qab · Qbb⁻¹ · (y_dd − a_int)
    //     P_fixed = P  − Qab · Qbb⁻¹ · Qab'
    // which is mathematically equivalent to a "perfect" measurement
    // (R = 0) of the integer.  Once successfully fixed, the
    // ambiguity rows of P_fixed have ~zero variance and the next
    // epoch's LAMBDA call sees a near-singular Qb in the integer
    // search, producing meaningless ratios (s2 ≈ s1, both huge —
    // the failure mode that triggers the upper-bound guard with
    // 1e13-scale ratios).
    //
    // The Kalman pseudo-observation form below applies the same
    // integer values via finite-variance measurement update.  Each
    // fixed DD becomes a 2-state row (+1 on sat_i, −1 on sat_ref)
    // with z = integer-fixed DD value in meters and R = hold_variance.
    // Joseph form keeps the covariance positive-definite, so the
    // ambiguity columns retain the small but nonzero variance set by
    // R.  The next epoch's AR sees a well-conditioned Qb and can
    // either re-confirm the same fix (further tightening, asymptoting
    // toward the integer) or detect a slip via the residual.
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
    }

    Eigen::MatrixXd R   = Eigen::MatrixXd::Identity(n_fixed_dd, n_fixed_dd) * config_.hold_variance;
    KalmanFilter::josephPseudoUpdate(x, P, H, v, R,
                                     result.fixed_state, result.fixed_covariance);

    result.success = true;
    result.num_fixed = n_nl;

    log->info("WL-constrained AR succeeded (Kalman pseudo-obs, R={:.4f} m²): "
              "{} NL pairs fixed, ratio={:.2f}, P_s={:.5f}, max|frac|={:.3f} cyc",
              config_.hold_variance, n_nl, result.ratio, p_s, max_pair_frac);

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
// WL integers enter @c wl_ambiguities in the first place.
//
// Useful as a first-stage AR validation before turning on the full
// WL+NL LAMBDA pipeline.  WL has a long wavelength (~86 cm GPS), so
// MW resolution is robust even when the float NL covariance hasn't
// converged enough for safe integer rounding.
// ===========================================================================

AmbiguityResolution AmbiguityResolver::resolveWlOnly(
    const std::vector<DdPair>& pairs,
    const Eigen::VectorXd& x,
    const Eigen::MatrixXd& P,
    const std::map<std::string, int>& wl_ambiguities,
    const std::map<std::string, double>& wl_fractions,
    const std::vector<PppObservation>& observations) {

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

        // Per-fix WL confidence gate (mirrors applyMwWlConstraintsNlWl_).  The
        // DD WL integer combines two zero-difference MW arcs (N_i − N_ref); if
        // EITHER arc's running fractional residual sits far from its locked
        // integer (frac ≫ 0 — locked too early, bias-contaminated, or near a
        // half-cycle boundary) the DD WL is suspicious.  Rather than committing
        // the questionable integer to the pseudo-obs update (where a wrong WL
        // biases the whole SD ambiguity block), stay float for this pair this
        // epoch.  Gating both arcs makes this stronger than the NlWl gate,
        // which only sees one arc per constraint.
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
            dd_wl, sat_i, sat_ref, hi.signal, lo.signal
        });
    }

    if (constraints.empty()) {
        log->debug("WL-only AR: no dual-frequency pairs with known DD WL integers");
        return result;
    }

    // Detect WL integer changes since the last invocation.  A change implies
    // a slip happened on L1 or L2 (or both) for the affected satellite —
    // MW's Hatch-mean re-locked at a different integer.  If we just apply
    // the new constraint with the existing (tight from previous epochs)
    // covariance, the gain is small relative to the integer-cycle innovation
    // and the state would drift toward the new integer over many epochs
    // instead of snapping in one.  Force a covariance reset on the affected
    // L1 and L2 SD ambiguity states so the new constraint takes full effect
    // immediately and any future measurement updates can refine from there.
    constexpr double kAmbResetVar = 100.0;  // (10 m)² — matches handlePhaseOutlier convention.

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
        if (prev_it == previous_dd_wl_.end()) continue;     // first time — no reset needed
        if (prev_it->second == c.wl_int)     continue;      // same integer — no change

        log->warn("WL integer changed for DD {}-{} {}/{}: {} → {} cycles. "
                  "Inflating L1/L2 SD ambiguity covariance to allow snap.",
                  c.sat_i, c.sat_ref, c.sig_hi, c.sig_lo,
                  prev_it->second, c.wl_int);

        // Reset the non-pivot SD ambiguity states on both bands.  The pivot
        // entries (idx_L1_ref, idx_L2_ref) are frozen at zero by BSSD and
        // shouldn't be inflated — touching them would unfreeze the pivot.
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

    result.success   = true;
    result.num_fixed = nv;
    result.ratio     = std::numeric_limits<double>::infinity();  // sentinel: no LAMBDA ratio test ran

    // Record the WL integers we just applied so the next call can detect changes.
    for (const auto& c : constraints) {
        const std::string key = c.sat_i + "_" + c.sat_ref + "_" + c.sig_hi + "_" + c.sig_lo;
        previous_dd_wl_[key] = c.wl_int;
    }

    log->info("WL-only AR: applied {} DD WL constraints{} (no LAMBDA NL search)",
              nv, n_reset > 0 ? fmt::format(", {} with reset", n_reset) : "");
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
// state); alpha(R, R) = 0 (the band IS the ref, no WL term).
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
//   - MW(R, b) for b != R: alpha(hi)=0 or alpha(lo)=0 — collapses to a
//     single non-zero coefficient on WL_<canonical>.  The classic "L1/L2"
//     and "L1/L5" cases when ref_band = 1.
//
//   - MW(b1, b2) where neither equals R (e.g. GPS L2/L5 EWL with R=1, or
//     Galileo E5a/E5b EWL with R=1): two non-zero coefficients across
//     two distinct WL state columns per sat — a 4-state DD row, which
//     correctly applies the EWL information that the per-band path's
//     ref-relative-only enumeration would silently drop.
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
    const std::vector<PppObservation>& observations) {

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

    // Reference band per sat (from existing NL state keys).  Phase 3b
    // freezes this for the lifetime of the satellite's arc.
    std::map<std::string, char> ref_band_per_sat;
    for (const auto& [key, idx] : state_manager.getAmbiguities()) {
        if (auto p = StateKeys::parseNlAmbiguityKey(key)) {
            ref_band_per_sat[p->satellite] = p->band;
        }
    }

    // wl_state_idx[sat][other_band] = state-vector index of the canonical
    // WL state coupling (ref_band, other_band).  We map by "the band that
    // is NOT the ref" so callers can look up by the carrier they care
    // about, regardless of canonical-min/max ordering.
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
        else                     continue;  // non-ref-relative WL — Phase 3b never creates these.
        wl_state_idx[p->satellite][other] = static_cast<int>(idx);
    }

    // Per-MW-arc constraint metadata.  Built by parsing the keys of the
    // wl_ambiguities map (e.g. "G01_L1W_L2W") rather than by enumerating
    // state WL columns — this is what lets the EWL flow through: the
    // L2/L5 MW arc is in this map even though the state has no WL_2_5.
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

        // Per-fix WL confidence gate.  An MW arc whose running mean is
        // far from its locked integer (frac ≫ 0) is suspicious — typically
        // because the arc locked too early, has unmodeled bias contamination,
        // or sits near a half-cycle boundary.  Rather than committing the
        // questionable integer to the AR pseudo-obs update (where a wrong
        // WL would bias every NL fix that depends on it), we stay float
        // for this arc this epoch.
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

    // Cross-frequency DD WL parity (consistency) check.
    //
    // For triple-frequency satellites, the three independent WL DDs
    // covering bands (a, b), (a, c), (b, c) must satisfy
    //
    //     dd_canon(a,b) − dd_canon(a,c) + dd_canon(b,c) = 0     (mod 0)
    //
    // by construction (telescoping cycle counts), where dd_canon(a,b) is
    // the SD WL value with N_<min_band> − N_<max_band> sign convention.
    // A non-zero parity at integer scale means at least one of the three
    // MW arcs has the wrong integer for this sat-pair this epoch.  We
    // can't tell which one without more information, so we conservatively
    // reject all three.
    //
    // The MW dd_wl_int is stored with sign convention "N_hi_freq − N_lo_freq"
    // (whichever band has higher carrier frequency).  Convert to canonical
    // before checking parity by flipping sign when the higher-frequency
    // band is NOT the lower-numbered band (e.g. Galileo E5a/E5b: E5b at
    // band 7 is higher frequency than E5a at band 5).
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
    // the last invocation, MW has re-locked at a different value — almost
    // certainly because of a slip on one of the two carriers.  Inflate
    // every non-pivot WL state column that participates in the new
    // constraint so the Kalman gain on the affected states is large
    // enough to snap to the new integer in a single epoch.  The pivot
    // columns are BSSD-frozen and are left alone.
    constexpr double kAmbResetVarCycles = 144.0;  // (12 cyc)² — order of magnitude L1/L2 reset.
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

    result.success   = true;
    result.num_fixed = nv;
    result.ratio     = std::numeric_limits<double>::infinity();

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
    log->info("WL-NlWl: applied {} DD WL constraints ({} EWL-style multi-state){}",
              nv, n_ewl,
              n_reset > 0 ? fmt::format(", {} with reset", n_reset) : "");
    return result;
}

// ===========================================================================
// Fix-and-hold
// ===========================================================================

void AmbiguityResolver::holdAmbiguities(StateManager& state_manager,
                                        const AmbiguityResolution& resolution) {
    auto log = log_manager_.getLogger("ambiguity_resolver");

    if (!resolution.success || config_.ar_mode != 3) return;

    // NlWl basis: fix-and-hold requires LAMBDA-fixed integers from the
    // (still future) NL search.  Today's MW-WL pseudo-observation already
    // applies the WL constraint as part of resolve(); a separate hold pass
    // would just re-apply the same information.  No-op until LAMBDA NL is
    // wired through @c applyMwWlConstraintsNlWl_'s big sibling.
    if (config_.state_ambiguity_basis == StateAmbiguityBasis::NlWl) {
        log->debug("holdAmbiguities skipped: NlWl basis (LAMBDA NL fix not yet implemented)");
        return;
    }

    auto pairs = buildDdPairs(state_manager, {});
    if (pairs.empty()) return;

    const int nb = static_cast<int>(pairs.size());
    const int nx = static_cast<int>(state_manager.getStateSize());
    Eigen::VectorXd x = state_manager.getStateVector();
    Eigen::MatrixXd P = state_manager.getCovariance();

    // Build pseudo-observation: v = (xa[i]-xa[ref]) - (x[i]-x[ref])
    // where xa is the fixed state
    Eigen::VectorXd v = Eigen::VectorXd::Zero(nb);
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(nb, nx);
    int nv = 0;

    for (int k = 0; k < nb; ++k) {
        int idx_i = pairs[k].state_idx_i;
        int idx_r = pairs[k].state_idx_ref;

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
// (i, ref) row used by the SD path — the state value already IS the DD.
// ===========================================================================

void AmbiguityResolver::holdAmbiguitiesDd(StateManager& state_manager,
                                          const AmbiguityResolution& resolution) {
    auto log = log_manager_.getLogger("ambiguity_resolver");

    if (!resolution.success || config_.ar_mode != 3) return;

    // Collect every DD-keyed ambiguity index that is in range of the
    // resolution's fixed_state.  An ambiguity that was added or removed
    // between resolveDd() and this call is silently skipped — its row
    // would reference a stale or non-existent fixed value.
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
    // H(k, dd_k) = 1 — selector rather than difference.
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

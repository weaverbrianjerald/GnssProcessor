/**
 * @file AmbiguityResolver.cpp
 * @brief MLAMBDA integer ambiguity resolution implementation.
 *
 * Implements the Modified LAMBDA method following Chang et al. (2005)
 * and integrates it with the GnssProcessor Kalman filter for PPP-RTK
 * integer ambiguity resolution and fix-and-hold.
 */

#include "AmbiguityResolver.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>
#include <fmt/format.h>

namespace GnssProcessor {

// ===========================================================================
// Construction
// ===========================================================================

AmbiguityResolver::AmbiguityResolver(const AmbiguityResolverConfig& config,
                                     LogManager& log_manager)
    : config_(config), log_manager_(log_manager) {
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
                                Eigen::VectorXd& s) {
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

    for (const auto& [key, idx] : amb_map) {
        auto sep = key.find('_');
        if (sep == std::string::npos) continue;

        AmbInfo info;
        info.key = key;
        info.satellite = key.substr(0, sep);
        info.signal = key.substr(sep + 1);
        info.constellation = info.satellite.substr(0, 1);
        info.state_idx = static_cast<int>(idx);

        // Check that ambiguity state is initialized (non-zero)
        const Eigen::VectorXd sv = state_manager.getStateVector();
        if (info.state_idx >= sv.size()) continue;
        if (sv(info.state_idx) == 0.0) continue;

        auto it = sat_elev.find(info.satellite);
        info.elevation = (it != sat_elev.end()) ? it->second : 0.0;

        // Skip below AR elevation mask
        if (info.elevation < config_.elevation_mask_ar) continue;

        // Skip relaxed ambiguities (non-zero process noise) — they track
        // a time-varying bias and should not be resolved to integers.
        if (state_manager.isAmbiguityRelaxed(key)) continue;

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
// Resolve
// ===========================================================================

AmbiguityResolution AmbiguityResolver::resolve(
    const StateManager& state_manager,
    const std::vector<PppObservation>& observations) {

    auto log = log_manager_.getLogger("ambiguity_resolver");
    AmbiguityResolution result;

    if (config_.ar_mode == 0) {
        log->debug("AR disabled (ar_mode=0)");
        return result;
    }

    // Build DD pairs
    auto pairs = buildDdPairs(state_manager, observations);
    result.num_candidates = static_cast<int>(pairs.size());

    if (pairs.empty()) {
        log->debug("No DD ambiguity pairs available for AR");
        return result;
    }

    const int nb = static_cast<int>(pairs.size());
    const Eigen::VectorXd x = state_manager.getStateVector();
    const Eigen::MatrixXd P = state_manager.getCovariance();
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

    // Run MLAMBDA
    Eigen::VectorXd afix;
    Eigen::VectorXd s;
    if (!mlambda(y_dd, Qb, afix, s)) {
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

    // Fix succeeded — compute fixed state and covariance
    // x_fixed = x - Qab * inv(Qb) * (y_dd - afix)
    Eigen::VectorXd residual = y_dd - afix;
    Eigen::MatrixXd Qb_inv = Qb.ldlt().solve(Eigen::MatrixXd::Identity(nb, nb));
    Eigen::MatrixXd K_fix = Qab * Qb_inv;

    result.fixed_state = x - K_fix * residual;
    result.fixed_covariance = P - K_fix * Qab.transpose();
    result.fixed_covariance = 0.5 * (result.fixed_covariance + result.fixed_covariance.transpose());

    result.success = true;
    result.num_fixed = nb;

    log->info("AR succeeded: {} ambiguities fixed, ratio={:.2f}", nb, result.ratio);

    // Log individual fixes
    for (int k = 0; k < nb; ++k) {
        log->debug("  DD {}-{} {}: float={:.3f} fixed={:.0f} diff={:.3f}",
                   pairs[k].sat_i, pairs[k].sat_ref, pairs[k].signal,
                   y_dd(k), afix(k), y_dd(k) - afix(k));
    }

    return result;
}

// ===========================================================================
// Fix-and-hold
// ===========================================================================

void AmbiguityResolver::holdAmbiguities(StateManager& state_manager,
                                        const AmbiguityResolution& resolution) {
    auto log = log_manager_.getLogger("ambiguity_resolver");

    if (!resolution.success || config_.ar_mode != 3) return;

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

    Eigen::MatrixXd PHt = P * H.transpose();
    Eigen::MatrixXd S = H * PHt + R;
    Eigen::MatrixXd K = PHt * S.ldlt().solve(Eigen::MatrixXd::Identity(nv, nv));

    x = x + K * v;
    Eigen::MatrixXd IKH = Eigen::MatrixXd::Identity(nx, nx) - K * H;
    P = IKH * P * IKH.transpose() + K * R * K.transpose();
    P = 0.5 * (P + P.transpose());

    state_manager.setStateVector(x);
    state_manager.setCovariance(P);

    log->info("Fix-and-hold applied: {} DD constraints, hold_var={:.4f}", nv, config_.hold_variance);
}

} // namespace GnssProcessor

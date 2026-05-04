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
    const std::map<std::string, int>& wl_ambiguities) {

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

    const Eigen::VectorXd x = state_manager.getStateVector();
    const Eigen::MatrixXd P = state_manager.getCovariance();

    // Try WL-constrained NL resolution first when WL integers are available
    if (!wl_ambiguities.empty()) {
        auto wl_result = resolveWlConstrained(pairs, x, P, wl_ambiguities, observations);
        if (wl_result.success) {
            wl_result.num_candidates = result.num_candidates;
            return wl_result;
        }
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

    // Fix succeeded � compute fixed state and covariance
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
    if (!mlambda(y_cycles, Qb_cycles, afix_cycles, s)) {
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

    // Convert the integer fix back to METERS for the back-substitution.
    Eigen::VectorXd afix_meters(nb);
    for (int i = 0; i < nb; ++i) {
        afix_meters(i) = afix_cycles(i) * entries[i].lambda;
    }

    // Cross-covariance between full state and DD floats (no SD subtraction
    // — we're picking columns directly because the state IS the DD).
    Eigen::MatrixXd Qab_meters(na, nb);
    for (int k = 0; k < na; ++k) {
        for (int dd = 0; dd < nb; ++dd) {
            Qab_meters(k, dd) = P_full(k, entries[dd].state_idx);
        }
    }

    // Standard fix-back: x_fixed = x - Qab * Qb^-1 * (y - afix).
    const Eigen::VectorXd residual = y_meters - afix_meters;
    const Eigen::MatrixXd Qb_inv =
        Qb_meters.ldlt().solve(Eigen::MatrixXd::Identity(nb, nb));
    const Eigen::MatrixXd K_fix = Qab_meters * Qb_inv;

    result.fixed_state      = x_full - K_fix * residual;
    result.fixed_covariance = P_full - K_fix * Qab_meters.transpose();
    result.fixed_covariance =
        0.5 * (result.fixed_covariance + result.fixed_covariance.transpose());

    result.success   = true;
    result.num_fixed = nb;

    log->info("AR (DD) succeeded: {} ambiguities fixed, ratio={:.2f}", nb, result.ratio);

    if (log->should_log(spdlog::level::debug)) {
        for (int k = 0; k < nb; ++k) {
            log->debug("  DD {}-{} {}: float={:.4f} m fixed={:.4f} m diff={:.4f} m (lambda={:.4f} m)",
                       entries[k].sat_i, entries[k].sat_p, entries[k].rov_code,
                       y_meters(k), afix_meters(k), residual(k), entries[k].lambda);
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
    Eigen::VectorXd afix_reduced;
    Eigen::VectorXd s;
    if (!mlambda(y_reduced, Q_reduced, afix_reduced, s)) {
        log->debug("WL-constrained AR: MLAMBDA failed on NL problem");
        return result;
    }

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

        log->debug("  NL pair {}-{} {}/{}: n_NL_float={:.3f} n_NL_fixed={:.0f} dd_wl={}, "
                   "DD1={:.3f}m DD2={:.3f}m",
                   pairs[nlc.dd_idx_1].sat_i, pairs[nlc.dd_idx_1].sat_ref,
                   pairs[nlc.dd_idx_1].signal, pairs[nlc.dd_idx_2].signal,
                   y_reduced(k), n_nl_fixed, nlc.dd_wl,
                   afix_orig(nlc.dd_idx_1), afix_orig(nlc.dd_idx_2));
    }

    // Compute fixed state using only the NL-paired DD ambiguities.
    // Build the cross-covariance and DD covariance for the consumed DDs only.
    const int n_fixed_dd = 2 * n_nl;
    std::vector<int> fixed_dd_indices;
    fixed_dd_indices.reserve(n_fixed_dd);
    for (int k = 0; k < n_nl; ++k) {
        fixed_dd_indices.push_back(nl_candidates[k].dd_idx_1);
        fixed_dd_indices.push_back(nl_candidates[k].dd_idx_2);
    }

    const int na = static_cast<int>(x.size());
    Eigen::MatrixXd Qab(na, n_fixed_dd);
    Eigen::MatrixXd Qbb(n_fixed_dd, n_fixed_dd);
    Eigen::VectorXd y_fixed(n_fixed_dd);
    Eigen::VectorXd a_fixed(n_fixed_dd);

    for (int c = 0; c < n_fixed_dd; ++c) {
        const int dd_c = fixed_dd_indices[c];
        y_fixed(c) = y_dd(dd_c);
        a_fixed(c) = afix_orig(dd_c);
        for (int k = 0; k < na; ++k) {
            Qab(k, c) = P(k, pairs[dd_c].state_idx_i) - P(k, pairs[dd_c].state_idx_ref);
        }
        for (int r = 0; r < n_fixed_dd; ++r) {
            const int dd_r = fixed_dd_indices[r];
            int ii = pairs[dd_r].state_idx_i;
            int ir = pairs[dd_r].state_idx_ref;
            int ji = pairs[dd_c].state_idx_i;
            int jr = pairs[dd_c].state_idx_ref;
            Qbb(r, c) = P(ii, ji) - P(ii, jr) - P(ir, ji) + P(ir, jr);
        }
    }

    Qbb = 0.5 * (Qbb + Qbb.transpose());
    Eigen::VectorXd residual = y_fixed - a_fixed;
    Eigen::MatrixXd Qbb_inv = Qbb.ldlt().solve(Eigen::MatrixXd::Identity(n_fixed_dd, n_fixed_dd));
    Eigen::MatrixXd K_fix = Qab * Qbb_inv;

    result.fixed_state = x - K_fix * residual;
    result.fixed_covariance = P - K_fix * Qab.transpose();
    result.fixed_covariance = 0.5 * (result.fixed_covariance + result.fixed_covariance.transpose());

    result.success = true;
    result.num_fixed = n_nl;

    log->info("WL-constrained AR succeeded: {} NL pairs fixed, ratio={:.2f}",
              n_nl, result.ratio);

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

    Eigen::MatrixXd PHt = P * H.transpose();
    Eigen::MatrixXd S   = H * PHt + R;
    Eigen::MatrixXd K   = PHt * S.ldlt().solve(Eigen::MatrixXd::Identity(nb, nb));

    x = x + K * v;
    Eigen::MatrixXd IKH = Eigen::MatrixXd::Identity(nx, nx) - K * H;
    P = IKH * P * IKH.transpose() + K * R * K.transpose();
    P = 0.5 * (P + P.transpose());

    state_manager.setStateVector(x);
    state_manager.setCovariance(P);

    log->info("Fix-and-hold (DD) applied: {} ambiguities, hold_var={:.4f}",
              nb, config_.hold_variance);
}

} // namespace GnssProcessor

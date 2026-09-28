/**
 * @file IlsCore.cpp
 * @brief Pure integer-least-squares (MLAMBDA) core implementation.
 *
 * The MLAMBDA kernels (Chang et al. 2005) and the acceptance battery, shared
 * by the single-station user path (AmbiguityResolver) and the PPP-RTK network
 * producer path.  No StateManager, Kalman, config or logging dependencies;
 * see IlsCore.hpp.
 */

#include "filter/IlsCore.hpp"

#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>

namespace GnssProcessor {
namespace ils {

// ===========================================================================
// Q = L^T D L factorization (unit lower-triangular L, eliminated from the last row up)
// ===========================================================================

bool ldlDecompose(const Eigen::MatrixXd& Q,
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

void reduction(Eigen::MatrixXd& L, Eigen::VectorXd& d,
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

int search(const Eigen::MatrixXd& L,
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

bool mlambda(const Eigen::VectorXd& a,
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
    if (d_out) *d_out = d;  // post-reduction diagonal, for the caller's P_s,boot

    // Back-transform the best candidate: afix = inv(Z^T) * E(:,0).
    // Z is unimodular, so inv(Z^T) is an integer matrix and rounding it is exact.
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

double computeBootstrapSuccessRate(const Eigen::VectorXd& d) {
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
// Partial ambiguity resolution (greedy backward elimination)
//
// AmbiguityResolver::selectFixableSubset_ forwards here with its config copied
// into @p gates; the gate thresholds are read from @p gates.
// ===========================================================================

PartialFix selectFixableSubset(const Eigen::VectorXd& y_cycles,
                               const Eigen::MatrixXd& Q_cycles,
                               const IlsGateConfig& gates) {
    PartialFix out;
    const int nb = static_cast<int>(y_cycles.size());
    if (nb <= 0) return out;

    // Active = indices (into the caller's full set) still in contention.
    std::vector<int> active(nb);
    std::iota(active.begin(), active.end(), 0);

    // Each iteration either returns or shrinks `active` by one, so the loop
    // terminates.  The subset may shrink to a single entry: the ratio,
    // bootstrap and frac gates remain meaningful for one ambiguity and reject
    // all but the most confident lone fixes.
    while (static_cast<int>(active.size()) >= 1) {
        const int m = static_cast<int>(active.size());

        Eigen::VectorXd y_sub(m);
        Eigen::MatrixXd Q_sub(m, m);
        for (int a = 0; a < m; ++a) {
            y_sub(a) = y_cycles(active[a]);
            for (int b = 0; b < m; ++b) {
                Q_sub(a, b) = Q_cycles(active[a], active[b]);
            }
        }
        // No-op on the already-symmetric inputs (bit-identical), but keeps the
        // sub-block robust to any caller that passes a not-quite-symmetric Q.
        Q_sub = 0.5 * (Q_sub + Q_sub.transpose());

        // Drop the entry with the largest formal float variance (cycles²): the
        // least precise ambiguity, whose removal is expected to improve the
        // bootstrap success rate and search conditioning most.
        auto drop_worst_variance = [&]() {
            int worst = 0;
            double worst_var = -1.0;
            for (int a = 0; a < m; ++a) {
                if (Q_sub(a, a) > worst_var) { worst_var = Q_sub(a, a); worst = a; }
            }
            active.erase(active.begin() + worst);
        };

        Eigen::VectorXd afix_sub, s_sub, d_sub;
        if (!mlambda(y_sub, Q_sub, afix_sub, s_sub, &d_sub)) { drop_worst_variance(); continue; }
        if (s_sub(0) <= 0.0 || s_sub(1) <= 0.0)               { drop_worst_variance(); continue; }

        const double ratio = s_sub(1) / s_sub(0);
        if (ratio < gates.ratio_threshold || ratio > gates.ratio_upper_bound) {
            drop_worst_variance();
            continue;
        }

        const double p_s = computeBootstrapSuccessRate(d_sub);
        if (p_s < gates.min_success_rate) { drop_worst_variance(); continue; }

        // Per-entry fractional cap: when this is the failing gate, drop the
        // entry with the largest |float − fixed| rather than the
        // highest-variance one.
        double max_frac = 0.0;
        int    worst_frac = 0;
        for (int a = 0; a < m; ++a) {
            const double f = std::abs(y_sub(a) - afix_sub(a));
            if (f > max_frac) { max_frac = f; worst_frac = a; }
        }
        if (max_frac > gates.nl_max_per_pair_frac) {
            active.erase(active.begin() + worst_frac);
            continue;
        }

        // The subset passes the full acceptance battery: accept it.
        out.success     = true;
        out.kept        = active;
        out.afix_cycles = afix_sub;
        out.ratio       = ratio;
        out.p_s         = p_s;
        out.max_frac    = max_frac;
        return out;
    }

    return out;  // success == false: no subset down to size 1 cleared the gates
}

// ===========================================================================
// Full-set fix + acceptance battery (+ optional partial-AR fallback)
//
// Mirrors the inline battery in AmbiguityResolver::resolve / resolveDd: the
// gate ordering (ratio-lower → ratio-upper → bootstrap FFR → per-entry frac),
// the "no partial fallback when MLAMBDA itself fails or distances are bad"
// behavior, and the partial-AR path are identical.  Logging is left to the
// caller (this routine is pure); the IlsResult carries every diagnostic the
// existing log lines reference.
// ===========================================================================

IlsResult ilsFix(const Eigen::VectorXd& a_hat_cycles,
                 const Eigen::MatrixXd& Q_cycles,
                 const IlsGateConfig& gates) {
    IlsResult res;
    const int nb = static_cast<int>(a_hat_cycles.size());
    if (nb <= 0) {
        res.full_fail = IlsFailReason::MlambdaFailed;
        return res;
    }

    Eigen::VectorXd afix_cycles, s, d_lambda;
    if (!mlambda(a_hat_cycles, Q_cycles, afix_cycles, s, &d_lambda)) {
        // Covariance not PD or <2 candidates: stay float, no partial fallback
        // (matches resolve()/resolveDd() returning before the battery).
        res.full_fail = IlsFailReason::MlambdaFailed;
        return res;
    }

    res.full_s1 = s(0);
    res.full_s2 = s(1);
    if (s(0) <= 0.0 || s(1) <= 0.0) {
        // Invalid squared distances: stay float, no partial fallback.
        res.full_fail = IlsFailReason::BadDistances;
        return res;
    }

    const double ratio = s(1) / s(0);
    res.full_ratio = ratio;

    IlsFailReason fail = IlsFailReason::None;
    double p_s = 0.0;
    double max_frac = 0.0;
    int    worst_idx = -1;

    if (ratio < gates.ratio_threshold) {
        fail = IlsFailReason::RatioLow;
    } else if (ratio > gates.ratio_upper_bound) {
        fail = IlsFailReason::RatioHigh;
    } else {
        p_s = computeBootstrapSuccessRate(d_lambda);
        res.full_p_succ = p_s;
        if (p_s < gates.min_success_rate) {
            fail = IlsFailReason::Bootstrap;
        } else {
            for (int k = 0; k < nb; ++k) {
                const double per = std::abs(a_hat_cycles(k) - afix_cycles(k));
                if (per > max_frac) { max_frac = per; worst_idx = k; }
            }
            res.full_max_frac  = max_frac;
            res.full_worst_idx = worst_idx;
            if (max_frac > gates.nl_max_per_pair_frac) {
                fail = IlsFailReason::FracCap;
            }
        }
    }

    res.full_fail = fail;

    if (fail == IlsFailReason::None) {
        // Full-set fix accepted.
        res.ok       = true;
        res.partial  = false;
        res.kept.resize(nb);
        std::iota(res.kept.begin(), res.kept.end(), 0);
        res.afix     = afix_cycles;
        res.ratio    = ratio;
        res.p_succ   = p_s;
        res.max_frac = max_frac;
        return res;
    }

    // Full set failed a gate.  Without partial AR, stay float.
    if (!gates.enable_partial_ar) {
        return res;  // ok == false; full_fail records the reason for caller logging
    }

    // Retry on the largest subset that clears the same battery.
    PartialFix pf = selectFixableSubset(a_hat_cycles, Q_cycles, gates);
    if (!pf.success) {
        // Partial AR found nothing; full_fail (set above) stays as the reason.
        return res;  // ok == false
    }

    res.ok       = true;
    res.partial  = true;
    res.kept     = pf.kept;
    res.afix     = pf.afix_cycles;
    res.ratio    = pf.ratio;
    res.p_succ   = pf.p_s;
    res.max_frac = pf.max_frac;
    return res;
}

} // namespace ils
} // namespace GnssProcessor

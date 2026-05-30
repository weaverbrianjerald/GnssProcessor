/*!
 * \file Smoother.cpp
 * \brief Backward RTS smoother implementation.
 *
 * Runs a Rauch-Tung-Striebel recursion over forward-pass snapshots.  The
 * recursion is performed in a common-key subspace at each step so that
 * state-vector dimension changes between adjacent epochs (new/removed
 * ambiguities, per-satellite ionosphere states, etc.) are handled without
 * resizing the stored forward solution.  States that only exist at one
 * epoch in a pair, and ambiguities whose satellite was reset, are left at
 * their forward a-posteriori values.
 */

#include "filter/Smoother.hpp"
#include "logging/LogManager.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <limits>
#include <utility>

namespace GnssProcessor {

    namespace {

        constexpr size_t kNone = std::numeric_limits<size_t>::max();

        /// True if \p amb_key's satellite prefix matches any entry in
        /// \p reset_satellites.  The satellite prefix is taken as the
        /// substring up to the first '_' (ambiguity keys look like
        /// "G01_L1W_L2W" or "G01-G05_L1W_L2W" for BSSD).
        bool ambSatWasReset(const std::string& amb_key,
                            const std::set<std::string>& reset_satellites) {
            if (reset_satellites.empty()) return false;
            const auto us = amb_key.find('_');
            const std::string sat_prefix = (us != std::string::npos)
                                             ? amb_key.substr(0, us)
                                             : amb_key;
            if (reset_satellites.count(sat_prefix)) return true;
            // Handle BSSD reset sets that may contain a bare sat while the
            // amb key is a differenced pair, or vice-versa.
            for (const auto& r : reset_satellites) {
                if (!r.empty() && amb_key.find(r) != std::string::npos) return true;
            }
            return false;
        }

        /// Intersect two key->index maps and append matching pairs to the
        /// parallel output index arrays.
        void intersectMaps(const std::map<std::string, size_t>& a,
                           const std::map<std::string, size_t>& b,
                           std::vector<size_t>& idx_k,
                           std::vector<size_t>& idx_k1) {
            for (const auto& [key, ia] : a) {
                auto it = b.find(key);
                if (it != b.end()) {
                    idx_k.push_back(ia);
                    idx_k1.push_back(it->second);
                }
            }
        }

    } // anonymous namespace

    // ==================================================================
    // Construction
    // ==================================================================

    Smoother::Smoother(std::vector<SmootherEpochSnapshot> snapshots, LogManager& log_manager)
        : snapshots_(std::move(snapshots)), log_manager_(log_manager) {}

    // ==================================================================
    // Common-index collection
    // ==================================================================

    void Smoother::collectCommonIndices(const SmootherEpochSnapshot& sk,
                                        const SmootherEpochSnapshot& sk1,
                                        std::vector<size_t>& idx_k,
                                        std::vector<size_t>& idx_k1) const {
        idx_k.clear();
        idx_k1.clear();

        // Always-present core states: position (3) and tropo (1).
        for (size_t j = 0; j < 3; ++j) {
            idx_k.push_back(sk.pos_idx + j);
            idx_k1.push_back(sk1.pos_idx + j);
        }
        idx_k.push_back(sk.tropo_idx);   idx_k1.push_back(sk1.tropo_idx);

        // Optional core states — included only if present at BOTH epochs.
        auto both_scalar = [&](size_t a, size_t b) {
            if (a != kNone && b != kNone) {
                idx_k.push_back(a);
                idx_k1.push_back(b);
            }
        };

        // Receiver clock — present in non-BSSD PPP / PPP-RTK only.  Skipped
        // under BSSD because no clock state is allocated; tropo therefore
        // ends up at index 3 instead of 4, but consumers locate it through
        // sk.tropo_idx so the relative shift is invisible here.
        both_scalar(sk.clock_idx, sk1.clock_idx);
        if (sk.velocity_idx != kNone && sk1.velocity_idx != kNone) {
            for (size_t j = 0; j < 3; ++j) {
                idx_k.push_back(sk.velocity_idx + j);
                idx_k1.push_back(sk1.velocity_idx + j);
            }
        }
        both_scalar(sk.phase_clock_idx, sk1.phase_clock_idx);
        both_scalar(sk.gradient_n_idx,  sk1.gradient_n_idx);
        both_scalar(sk.gradient_e_idx,  sk1.gradient_e_idx);

        // Dynamic-key intersections.  Ambiguities additionally exclude
        // satellites whose states were reset during the update at k+1.
        for (const auto& [amb_key, ia] : sk.ambiguities) {
            if (ambSatWasReset(amb_key, sk1.reset_satellites)) continue;
            auto it = sk1.ambiguities.find(amb_key);
            if (it != sk1.ambiguities.end()) {
                idx_k.push_back(ia);
                idx_k1.push_back(it->second);
            }
        }

        intersectMaps(sk.ionospheres, sk1.ionospheres, idx_k, idx_k1);
        intersectMaps(sk.code_biases, sk1.code_biases, idx_k, idx_k1);
        intersectMaps(sk.sat_biases,  sk1.sat_biases,  idx_k, idx_k1);
        intersectMaps(sk.sys_offsets, sk1.sys_offsets, idx_k, idx_k1);
    }

    // ==================================================================
    // Backward RTS recursion
    // ==================================================================

    bool Smoother::run() {
        auto log = log_manager_.getLogger("smoother");
        if (snapshots_.empty()) {
            if (log) log->warn("Smoother: empty snapshot buffer, nothing to smooth");
            return false;
        }

        const size_t N = snapshots_.size();
        smoothed_.resize(N);

        // Copy core-state indices from snapshot to smoothed result.
        auto copyCoreIndices = [](SmoothedEpoch& s, const SmootherEpochSnapshot& snap) {
            s.pos_idx         = snap.pos_idx;
            s.clock_idx       = snap.clock_idx;
            s.tropo_idx       = snap.tropo_idx;
            s.velocity_idx    = snap.velocity_idx;
            s.phase_clock_idx = snap.phase_clock_idx;
            s.gradient_n_idx  = snap.gradient_n_idx;
            s.gradient_e_idx  = snap.gradient_e_idx;
        };

        // Initialize the last epoch's smoothed result = a-posteriori.
        smoothed_[N - 1].t = snapshots_[N - 1].t;
        smoothed_[N - 1].x = snapshots_[N - 1].x_post;
        smoothed_[N - 1].P = snapshots_[N - 1].P_post;
        copyCoreIndices(smoothed_[N - 1], snapshots_[N - 1]);
        smoothed_[N - 1].smoothed = snapshots_[N - 1].has_posterior;

        size_t num_smoothed = smoothed_[N - 1].smoothed ? 1 : 0;
        size_t num_boundaries = 0;
        size_t num_inversion_fallbacks = 0;

        for (size_t i = N - 1; i > 0; --i) {
            const size_t k = i - 1;
            const auto& sk  = snapshots_[k];
            const auto& sk1 = snapshots_[i];
            const auto& smk1 = smoothed_[i];
            auto& smk = smoothed_[k];

            // Start with the forward a-posteriori.
            smk.t = sk.t;
            smk.x = sk.x_post;
            smk.P = sk.P_post;
            copyCoreIndices(smk, sk);
            smk.smoothed = sk.has_posterior;

            // Segment boundary: complete reset at k+1 stops backward flow.
            if (sk1.is_discontinuity || !sk1.has_posterior) {
                ++num_boundaries;
                continue;
            }

            // Build parallel index arrays for the common subspace.
            std::vector<size_t> idx_k, idx_k1;
            collectCommonIndices(sk, sk1, idx_k, idx_k1);
            const size_t C = idx_k.size();
            if (C == 0) continue;

            // Extract subvectors / submatrices in the common subspace.
            // Note: x_pred and F at k+1 are sized for epoch k's layout
            // (predict does not grow the state), so idx_k indexes them.
            Eigen::VectorXd x_post_k_C(C), x_pred_k1_C(C), x_s_k1_C(C);
            Eigen::MatrixXd P_post_k_CC(C, C), P_pred_k1_CC(C, C),
                            P_s_k1_CC(C, C),  F_CC(C, C);

            for (size_t a = 0; a < C; ++a) {
                x_post_k_C(static_cast<Eigen::Index>(a))  = sk.x_post[idx_k[a]];
                x_pred_k1_C(static_cast<Eigen::Index>(a)) = sk1.x_pred[idx_k[a]];
                x_s_k1_C(static_cast<Eigen::Index>(a))    = smk1.x[idx_k1[a]];
                for (size_t b = 0; b < C; ++b) {
                    P_post_k_CC(a, b)  = sk.P_post(idx_k[a],  idx_k[b]);
                    P_pred_k1_CC(a, b) = sk1.P_pred(idx_k[a], idx_k[b]);
                    P_s_k1_CC(a, b)    = smk1.P(idx_k1[a],    idx_k1[b]);
                    F_CC(a, b)         = sk1.F(idx_k[a],      idx_k[b]);
                }
            }

            // Symmetrise P_pred_{k+1} before inversion (numerical hygiene).
            P_pred_k1_CC = 0.5 * (P_pred_k1_CC + P_pred_k1_CC.transpose());

            Eigen::LDLT<Eigen::MatrixXd> ldlt(P_pred_k1_CC);
            if (ldlt.info() != Eigen::Success) {
                ++num_inversion_fallbacks;
                if (log) log->warn("Epoch k={} week={} tow={:.3f}: P_pred inversion failed (C={}); leaving forward solution",
                                   k, sk.t.week, sk.t.tow, C);
                continue;
            }

            // G_k = P_post_k * F^T * inv(P_pred_{k+1})
            Eigen::MatrixXd G_C =
                ldlt.solve((P_post_k_CC * F_CC.transpose()).transpose()).transpose();

            // Smoothed state and covariance (in common subspace).
            Eigen::VectorXd x_s_k_C = x_post_k_C + G_C * (x_s_k1_C - x_pred_k1_C);
            Eigen::MatrixXd P_s_k_CC = P_post_k_CC
                                     + G_C * (P_s_k1_CC - P_pred_k1_CC) * G_C.transpose();
            // Symmetrise the smoothed block.
            P_s_k_CC = 0.5 * (P_s_k_CC + P_s_k_CC.transpose());

            // Write the common-subspace smoothed block back into the full
            // smoothed state/cov at epoch k.  Non-common rows and columns
            // retain their forward a-posteriori values.
            for (size_t a = 0; a < C; ++a) {
                smk.x[idx_k[a]] = x_s_k_C(static_cast<Eigen::Index>(a));
                for (size_t b = 0; b < C; ++b) {
                    smk.P(idx_k[a], idx_k[b]) = P_s_k_CC(a, b);
                }
            }

            smk.smoothed = true;
            ++num_smoothed;
        }

        if (log) {
            log->info("RTS smoother complete: {} epochs, {} smoothed, {} segment boundaries, {} inversion fallbacks",
                      N, num_smoothed, num_boundaries, num_inversion_fallbacks);
        }
        return true;
    }

} // namespace GnssProcessor

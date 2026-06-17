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
#include "common/PppUtils.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

    namespace {

        constexpr size_t kNone = std::numeric_limits<size_t>::max();

        /// True if \p amb_key's satellite prefix EXACTLY matches an entry in
        /// \p reset_satellites.  The prefix is the substring up to the first
        /// '_' (ambiguity keys look like "G01_L1W" or "G01_L1C_L2W").  An exact
        /// prefix match — not a substring search — so a reset of "G1" does not
        /// spuriously segment "G12_..." (audit R7).
        bool ambSatWasReset(const std::string& amb_key,
                            const std::set<std::string>& reset_satellites) {
            if (reset_satellites.empty()) return false;
            const auto us = amb_key.find('_');
            const std::string sat_prefix = (us != std::string::npos)
                                             ? amb_key.substr(0, us)
                                             : amb_key;
            return reset_satellites.count(sat_prefix) > 0;
        }

        /// Intersect two key->index maps and append matching pairs to the
        /// parallel output index arrays, skipping any key in \p exclude_keys
        /// (the handover-segmentation set).
        void intersectMaps(const std::map<std::string, size_t>& a,
                           const std::map<std::string, size_t>& b,
                           std::vector<size_t>& idx_k,
                           std::vector<size_t>& idx_k1,
                           const std::set<std::string>& exclude_keys) {
            for (const auto& [key, ia] : a) {
                if (exclude_keys.count(key)) continue;
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
                                        std::vector<size_t>& idx_k1,
                                        const std::set<std::string>& exclude_keys) const {
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
        // satellites whose states were reset during the update at k+1, and any
        // key in the handover-segmentation set (constellations whose SD frame
        // changed and cannot be inverse-referenced, plus orphaned signal groups).
        for (const auto& [amb_key, ia] : sk.ambiguities) {
            if (ambSatWasReset(amb_key, sk1.reset_satellites)) continue;
            if (exclude_keys.count(amb_key)) continue;
            auto it = sk1.ambiguities.find(amb_key);
            if (it != sk1.ambiguities.end()) {
                idx_k.push_back(ia);
                idx_k1.push_back(it->second);
            }
        }

        intersectMaps(sk.ionospheres, sk1.ionospheres, idx_k, idx_k1, exclude_keys);
        intersectMaps(sk.code_biases, sk1.code_biases, idx_k, idx_k1, exclude_keys);
        intersectMaps(sk.sat_biases,  sk1.sat_biases,  idx_k, idx_k1, exclude_keys);
        intersectMaps(sk.sys_offsets, sk1.sys_offsets, idx_k, idx_k1, exclude_keys);
    }

    // ==================================================================
    // BSSD handover frame reconciliation
    // ==================================================================

    void Smoother::inverseReReferenceGroup(
        const std::vector<Eigen::Index>& group_indices,
        Eigen::Index datum,
        Eigen::VectorXd& x,
        Eigen::MatrixXd& P) {
        // State subtract (datum value captured before any member is modified;
        // the datum is not modified by the loop, so reading x(datum) each
        // iteration would also be safe).
        const double dv = x(datum);
        for (const Eigen::Index gi : group_indices)
            if (gi != datum) x(gi) -= dv;
        x(datum) = 0.0;
        // Covariance T·P·Tᵀ on FULL rows/cols (the forward re-reference coupled
        // the group to every other state; the inverse must too — audit R1).
        // Row ops then col ops, datum frozen — identical pattern to
        // PivotManager::applyCovarianceReReference, anchored on the old pivot.
        for (const Eigen::Index gi : group_indices)
            if (gi != datum) P.row(gi) -= P.row(datum);
        for (const Eigen::Index gi : group_indices)
            if (gi != datum) P.col(gi) -= P.col(datum);
        P.row(datum).setZero();
        P.col(datum).setZero();
    }

    std::set<std::string> Smoother::prepareHandoverFrame(
        const SmootherEpochSnapshot& sk,
        const SmootherEpochSnapshot& sk1,
        Eigen::VectorXd& x_adj,
        Eigen::MatrixXd& P_adj) const {

        std::set<std::string> exclude_keys;

        // Constellations present in either epoch's pivot map.
        std::set<std::string> cons_set;
        for (const auto& [c, s] : sk.pivot_satellites)  cons_set.insert(c);
        for (const auto& [c, s] : sk1.pivot_satellites) cons_set.insert(c);

        for (const auto& cons : cons_set) {
            auto it0 = sk.pivot_satellites.find(cons);
            auto it1 = sk1.pivot_satellites.find(cons);
            const bool has0 = it0 != sk.pivot_satellites.end();
            const bool has1 = it1 != sk1.pivot_satellites.end();
            if (has0 && has1 && it0->second == it1->second) continue;   // no frame change

            const std::string p0 = has0 ? it0->second : std::string();
            const std::string p1 = has1 ? it1->second : std::string();

            // Always segment this constellation's ionosphere and sat-bias states:
            // their re-references are not inverted here, so the k+1 estimate is in
            // the new frame and must fall back to the forward solution.
            for (const auto& [sat, idx] : sk1.ionospheres)
                if (PppUtils::getSystemString(sat) == cons) exclude_keys.insert(sat);
            for (const auto& [sb_key, idx] : sk1.sat_biases) {
                const auto bar = sb_key.find('|');
                const std::string sat = (bar != std::string::npos)
                                            ? sb_key.substr(0, bar) : sb_key;
                if (PppUtils::getSystemString(sat) == cons) exclude_keys.insert(sb_key);
            }

            // CLEAN, invertible handover?  (recorded by PivotManager, old/new match)
            bool clean = false;
            if (has0 && has1) {
                auto ch = sk1.clean_handovers.find(cons);
                if (ch != sk1.clean_handovers.end()
                    && ch->second.first == p0 && ch->second.second == p1)
                    clean = true;
            }

            if (!clean) {
                // Disturbed / forced / appeared-or-vanished: segment all of this
                // constellation's ambiguities (the old pivot is discontinuous or
                // there is no well-defined inverse datum).
                for (const auto& [amb_key, idx] : sk1.ambiguities) {
                    const auto us = amb_key.find('_');
                    const std::string sat = (us != std::string::npos)
                                                ? amb_key.substr(0, us) : amb_key;
                    if (PppUtils::getSystemString(sat) == cons)
                        exclude_keys.insert(amb_key);
                }
                continue;
            }

            // CLEAN handover: inverse-reference each ambiguity SIGNAL group back
            // to the old pivot p0.  Group by signal-suffix (after the first '_'),
            // mirroring PivotManager's grouping.
            std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>> groups;
            for (const auto& [amb_key, idx] : sk1.ambiguities) {
                const auto us = amb_key.find('_');
                if (us == std::string::npos) continue;
                const std::string sat = amb_key.substr(0, us);
                if (PppUtils::getSystemString(sat) != cons) continue;
                const std::string sig = amb_key.substr(us + 1);
                groups[sig].emplace_back(amb_key, static_cast<Eigen::Index>(idx));
            }

            // Per-signal groups have DISJOINT member and datum index sets, so
            // their re-reference operators commute — the inverse order is
            // irrelevant to the result (verified numerically).  Iterate in
            // descending order purely for symmetry with PivotManager's ascending
            // forward loop.  (Iono / sat-bias are segmented above, so only the
            // ambiguity blocks need inverting here.)
            for (auto git = groups.rbegin(); git != groups.rend(); ++git) {
                const std::string& sig = git->first;
                const auto& members    = git->second;
                const std::string datum_old_key = p0 + "_" + sig;  // inverse datum
                const std::string datum_new_key = p1 + "_" + sig;  // forward datum
                auto d_old = sk1.ambiguities.find(datum_old_key);
                // Invertible only if the forward leg re-referenced this signal to
                // p1 (so p1 had a state on it) AND the old pivot has a live state
                // on it to anchor the inverse.  Otherwise the signal was orphan-
                // reset (new pivot lacked it) or has no inverse datum — segment.
                const bool invertible = d_old != sk1.ambiguities.end()
                                        && sk1.ambiguities.count(datum_new_key) > 0;
                if (!invertible) {
                    for (const auto& [amb_key, idx] : members)
                        exclude_keys.insert(amb_key);
                    continue;
                }
                std::vector<Eigen::Index> member_idx;
                member_idx.reserve(members.size());
                for (const auto& [amb_key, idx] : members) member_idx.push_back(idx);
                inverseReReferenceGroup(member_idx,
                                        static_cast<Eigen::Index>(d_old->second),
                                        x_adj, P_adj);
            }
        }

        return exclude_keys;
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

            // Dimension guard / segment boundary: a long-gap teardown
            // (Application::handleGap) prunes ambiguity / iono / sat-bias states
            // BEFORE predict, so this epoch's a-priori (x_pred / P_pred / F) is
            // captured at a SMALLER dimension than epoch k's posterior.  The
            // epoch-k indices used below to read the k+1 a-priori would then be
            // out of bounds.  Predict never grows the state and pre-predict
            // changes can only shrink it, so any size change is a teardown — a
            // forward discontinuity that backward information must not cross.
            if (sk1.x_pred.size() != sk.x_post.size()) {
                ++num_boundaries;
                continue;
            }

            // BSSD handover reconciliation.  If the pivot map changed between k
            // and k+1, a pivot handover re-referenced one or more constellations
            // in place: x_post(k+1) / x_s(k+1) are in the NEW frame while
            // x_pred(k+1) (captured pre-handover, epoch-k layout) is still in the
            // OLD frame.  For CLEAN handovers we inverse-reference k+1's smoothed
            // estimate back to the old pivot so the whole step stays consistently
            // in epoch k's frame; disturbed/forced handovers (and iono/sat-bias
            // groups) are segmented.  This runs on COPIES so the reported k+1
            // result is left in its own frame (audit P2-1).  The copies are taken
            // only when the pivot map actually changed (handovers are rare).
            std::set<std::string> exclude_keys;
            bool frame_adjusted = false;
            Eigen::VectorXd x_s1_adj;
            Eigen::MatrixXd P_s1_adj;
            if (sk.pivot_satellites != sk1.pivot_satellites) {
                x_s1_adj = smk1.x;
                P_s1_adj = smk1.P;
                exclude_keys = prepareHandoverFrame(sk, sk1, x_s1_adj, P_s1_adj);
                frame_adjusted = true;
            }
            const Eigen::VectorXd& x_s1_use = frame_adjusted ? x_s1_adj : smk1.x;
            const Eigen::MatrixXd& P_s1_use = frame_adjusted ? P_s1_adj : smk1.P;

            // Build parallel index arrays for the common subspace.
            std::vector<size_t> idx_k, idx_k1;
            collectCommonIndices(sk, sk1, idx_k, idx_k1, exclude_keys);
            const size_t C = idx_k.size();
            if (C == 0) continue;

            // Extract subvectors / submatrices in the common subspace.
            // Note: x_pred and F at k+1 are sized for epoch k's layout
            // (predict does not grow the state), so idx_k indexes them.  The
            // smoothed k+1 estimate is read from the (possibly frame-adjusted)
            // copies so it shares epoch k's BSSD frame.
            Eigen::VectorXd x_post_k_C(C), x_pred_k1_C(C), x_s_k1_C(C);
            Eigen::MatrixXd P_post_k_CC(C, C), P_pred_k1_CC(C, C),
                            P_s_k1_CC(C, C),  F_CC(C, C);

            for (size_t a = 0; a < C; ++a) {
                x_post_k_C(static_cast<Eigen::Index>(a))  = sk.x_post[idx_k[a]];
                x_pred_k1_C(static_cast<Eigen::Index>(a)) = sk1.x_pred[idx_k[a]];
                x_s_k1_C(static_cast<Eigen::Index>(a))    = x_s1_use[idx_k1[a]];
                for (size_t b = 0; b < C; ++b) {
                    P_post_k_CC(a, b)  = sk.P_post(idx_k[a],  idx_k[b]);
                    P_pred_k1_CC(a, b) = sk1.P_pred(idx_k[a], idx_k[b]);
                    P_s_k1_CC(a, b)    = P_s1_use(idx_k1[a],  idx_k1[b]);
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

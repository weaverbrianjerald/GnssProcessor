/**
 * @file DdPivotManager.cpp
 * @brief Implementation of DD baseline pivot manager.
 *
 * Sticky-or-rerank selection plus the DD pivot-handover S-transform,
 * extracted from @c MeasurementModel.  The handover math is unchanged
 * from the previous inline implementation; only its home moved.
 */

#include "filter/DdPivotManager.hpp"
#include "filter/StateKeySchema.hpp"
#include <Eigen/Dense>
#include <Eigen/Cholesky>
#include <algorithm>
#include <cmath>
#include <vector>

namespace GnssProcessor {

    DdPivotManager::DdPivotManager(const GnssConfig& config, LogManager& log_manager)
        : config_(config), log_manager_(log_manager) {
    }

    // ===================================================================
    // selectPivots — sticky-or-rerank, with optional handover S-transform.
    // ===================================================================
    std::map<std::string, std::string> DdPivotManager::selectPivots(
        const CandidateMap& candidates,
        StateManager& state_manager)
    {
        // Snapshot the previous epoch's sticky map so we can detect
        // changes after this epoch's selection runs.
        const std::map<std::string, std::string> previous_pivots = sticky_;

        std::map<std::string, std::string> pivots;
        for (const auto& [cons, sats] : candidates) {
            std::string chosen;
            auto sticky_it = sticky_.find(cons);
            if (sticky_it != sticky_.end()
             && sats.find(sticky_it->second) != sats.end()) {
                // Incumbent is still in the candidate set — keep it.
                chosen = sticky_it->second;
            } else {
                // Re-rank by SNR + small tiebreak on signal count.
                // Same scoring as the legacy inline implementation.
                double best = -1.0;
                for (const auto& [sat, info] : sats) {
                    const double score = info.avg_snr
                                       + 0.01 * static_cast<double>(info.num_sigs);
                    if (score > best) { best = score; chosen = sat; }
                }
                if (!chosen.empty() && sticky_it != sticky_.end()) {
                    log_manager_.getLogger("main_log")->info(
                        "DD pivot handover for '{}': '{}' -> '{}'",
                        cons, sticky_it->second, chosen);
                }
            }
            if (!chosen.empty()) pivots[cons] = chosen;
        }
        sticky_ = pivots;

        // Apply S-transform on any pivot that changed, so existing DD
        // ambiguities are re-referenced rather than reset and reseeded.
        if (config_.getBaselineConfig().enable_pivot_handover) {
            handleHandover(state_manager, previous_pivots, pivots);
        }

        return pivots;
    }

    // ===================================================================
    // handleHandover — DD S-transformation for any pivot that changed.
    // ===================================================================
    void DdPivotManager::handleHandover(
        StateManager& state_manager,
        const std::map<std::string, std::string>& previous_pivots,
        const std::map<std::string, std::string>& new_pivots)
    {
        auto klog = log_manager_.getLogger("measurement_model");

        // Per (cons, code) group with a usable transform: the global
        // state indices of all DD ambiguities in the group, and the
        // position within that list of the row for the new pivot.
        // Also retain the rename pairs so we can re-key after the math.
        struct GroupTransform {
            std::string cons;
            std::string code;
            std::string old_pivot;
            std::string new_pivot;
            std::vector<int>            indices;
            std::vector<std::string>    sat_i;
            int                         new_pivot_pos{ -1 };
        };
        std::vector<GroupTransform> transforms;

        const auto& amb_map = state_manager.getAmbiguities();

        for (const auto& [cons, new_pivot] : new_pivots) {
            auto prev_it = previous_pivots.find(cons);
            if (prev_it == previous_pivots.end()) continue;       // first-time pivot
            const std::string& old_pivot = prev_it->second;
            if (old_pivot == new_pivot) continue;                 // unchanged

            // Bucket old-pivot-suffixed DD keys by their RINEX code.
            // Each bucket forms an independent (cons, code) sub-system.
            struct Entry { std::string sat_i; int idx; };
            std::map<std::string, std::vector<Entry>> by_code;

            for (const auto& [key, idx] : amb_map) {
                // Format documented in StateKeySchema.hpp:
                // "DD|<cons>|<sat_i>|<code>|<pivot>".
                const auto parts = StateKeys::parseDdAmbiguityKey(key);
                if (!parts) continue;
                if (parts->cons != cons || parts->pivot != old_pivot) continue;
                by_code[parts->code].push_back({ parts->sat_i, static_cast<int>(idx) });
            }

            for (const auto& [code, entries] : by_code) {
                int new_pivot_pos = -1;
                for (size_t k = 0; k < entries.size(); ++k) {
                    if (entries[k].sat_i == new_pivot) {
                        new_pivot_pos = static_cast<int>(k);
                        break;
                    }
                }
                if (new_pivot_pos < 0) {
                    // No DD ambiguity for the new pivot in this group's
                    // old basis — can't transform.  Fall through; the
                    // expected-keys prune will erase the stale rows and
                    // the row builder will re-seed on the new pivot.
                    klog->info("DD pivot handover for {}|{} ({}-> {}): "
                               "no existing ambiguity for new pivot in this group; "
                               "falling back to reset-and-reseed",
                               cons, code, old_pivot, new_pivot);
                    continue;
                }

                GroupTransform gt;
                gt.cons          = cons;
                gt.code          = code;
                gt.old_pivot     = old_pivot;
                gt.new_pivot     = new_pivot;
                gt.new_pivot_pos = new_pivot_pos;
                gt.indices.reserve(entries.size());
                gt.sat_i.reserve(entries.size());
                for (const auto& e : entries) {
                    gt.indices.push_back(e.idx);
                    gt.sat_i.push_back(e.sat_i);
                }
                transforms.push_back(std::move(gt));
            }
        }

        if (transforms.empty()) return;

        // Apply x_new = T·x and P_new = T·P·Tᵀ for the S-transform without
        // materializing the full state_size × state_size T.  T is identity
        // outside the per-group blocks; for each group, with new-pivot row
        // j and other group rows i_k, the only non-identity entries of T
        // are T(j, j) = -1 and T(i_k, j) = -1.  Equivalently:
        //   x'(j) = -x(j); x'(i) = x(i) - x(j)
        //   P_1.row(j) = -P.row(j); P_1.row(i) = P.row(i) - P.row(j)
        //   P_2.col(j) = -P_1.col(j); P_2.col(i) = P_1.col(i) - P_1.col(j)
        // Groups have disjoint row sets (each ambiguity belongs to exactly
        // one (cons, code, pivot) group) so the in-place updates compose
        // without interference.  Saves an O(n³) full matrix multiply per
        // handover, which scales badly when the state has many ambiguities.
        Eigen::VectorXd x = state_manager.getStateVector();
        Eigen::MatrixXd P = state_manager.getCovariance();

        // Pass 1: row updates of P (and x) — produces P_1 = T·P.
        for (const auto& gt : transforms) {
            const int j = gt.indices[gt.new_pivot_pos];
            const Eigen::RowVectorXd row_j = P.row(j);  // copy
            const double x_j = x(j);
            for (size_t k = 0; k < gt.indices.size(); ++k) {
                const int i = gt.indices[k];
                if (k == static_cast<size_t>(gt.new_pivot_pos)) {
                    x(i) = -x_j;
                    P.row(i) = -row_j;
                } else {
                    x(i) -= x_j;
                    P.row(i) -= row_j;
                }
            }
        }

        // Pass 2: column updates of P_1 — produces P_2 = P_1·Tᵀ = T·P·Tᵀ.
        for (const auto& gt : transforms) {
            const int j = gt.indices[gt.new_pivot_pos];
            const Eigen::VectorXd col_j = P.col(j);  // copy after Pass 1
            for (size_t k = 0; k < gt.indices.size(); ++k) {
                const int i = gt.indices[k];
                if (k == static_cast<size_t>(gt.new_pivot_pos)) {
                    P.col(i) = -col_j;
                } else {
                    P.col(i) -= col_j;
                }
            }
        }

        // Symmetrize defensively, then check PSD via LDLT.  Symmetrization
        // alone fixes asymmetry from FP rounding; it does NOT guarantee
        // positive-semi-definiteness if P was already ill-conditioned or
        // a near-rank-deficient pivot row crept through.  Log when the
        // LDLT reports any non-positive pivot so the issue surfaces in
        // logs rather than poisoning the filter silently.
        P = 0.5 * (P + P.transpose());
        {
            Eigen::LDLT<Eigen::MatrixXd> ldlt(P);
            const auto& d = ldlt.vectorD();
            double min_d = d.minCoeff();
            if (ldlt.info() != Eigen::Success || min_d <= 0.0) {
                log_manager_.getLogger("main_log")->warn(
                    "DD pivot handover: P not PSD after T·P·Tᵀ (min LDLT "
                    "diagonal = {:.3e}); applying epsilon regularization",
                    min_d);
                const double eps = std::max(1e-12, std::abs(min_d) + 1e-9);
                P.diagonal().array() += eps;
            }
        }

        state_manager.setStateVector(x);
        state_manager.setCovariance(P);

        // Rename ambiguity keys.  State indices are unchanged — only
        // the key strings move.
        size_t total_renamed = 0;
        for (const auto& gt : transforms) {
            for (size_t k = 0; k < gt.indices.size(); ++k) {
                const std::string& sat_i = gt.sat_i[k];
                const std::string old_key =
                    "DD|" + gt.cons + "|" + sat_i + "|" + gt.code + "|" + gt.old_pivot;
                std::string new_key;
                if (k == static_cast<size_t>(gt.new_pivot_pos)) {
                    new_key = "DD|" + gt.cons + "|" + gt.old_pivot + "|"
                            + gt.code + "|" + gt.new_pivot;
                } else {
                    new_key = "DD|" + gt.cons + "|" + sat_i + "|"
                            + gt.code + "|" + gt.new_pivot;
                }
                if (state_manager.renameAmbiguity(old_key, new_key)) {
                    ++total_renamed;
                }
            }
            log_manager_.getLogger("main_log")->info(
                "DD pivot handover for {}|{}: {} -> {}, transformed {} ambiguity row(s) "
                "(S-transform applied; integer cycles preserved)",
                gt.cons, gt.code, gt.old_pivot, gt.new_pivot, gt.indices.size());
        }
        klog->debug("DD pivot handover: {} group(s), {} ambiguity key(s) renamed",
                    transforms.size(), total_renamed);
    }

} // namespace GnssProcessor

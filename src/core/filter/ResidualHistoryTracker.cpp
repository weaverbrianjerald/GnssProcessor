/**
 * @file ResidualHistoryTracker.cpp
 * @brief Implementation of @c ResidualHistoryTracker.  See header for
 *        the EWMA-Welford recurrence and the reset semantics.
 */

#include "filter/ResidualHistoryTracker.hpp"
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace GnssProcessor {

    // -----------------------------------------------------------------
    // Construction
    // -----------------------------------------------------------------

    ResidualHistoryTracker::ResidualHistoryTracker(const Config& cfg, LogManager& log_mgr)
        : cfg_(cfg), log_manager_(log_mgr) {

        if (cfg_.enabled) {
            log_manager_.getLogger("kalman_filter")->info(
                "ResidualHistoryTracker enabled: alpha={:.3f} (~{} epochs effective), "
                "warmup_n={}, z_threshold={:.2f}, fraction_threshold={:.2f}",
                cfg_.alpha,
                cfg_.alpha > 0 ? static_cast<int>(std::round(1.0 / cfg_.alpha)) : 0,
                cfg_.warmup_n, cfg_.z_threshold, cfg_.fraction_threshold);
        }
    }

    // -----------------------------------------------------------------
    // Key construction
    // -----------------------------------------------------------------

    std::string ResidualHistoryTracker::makeKey(const std::string& sat,
                                                  const std::string& signal) {
        std::string key;
        key.reserve(sat.size() + 1 + signal.size());
        key.append(sat);
        key.push_back('_');
        key.append(signal);
        return key;
    }

    // -----------------------------------------------------------------
    // record(): EWMA-Welford update
    // -----------------------------------------------------------------
    //
    // The forgetting-factor form (Finch 2009, "Incremental Calculation
    // of Weighted Mean and Variance"):
    //
    //     δ      = r_k − μ_{k-1}
    //     μ_k    = μ_{k-1} + α · δ
    //     σ²_k   = (1 − α) · (σ²_{k-1} + α · δ²)
    //
    // The variance recurrence has both a forgetting term ((1-α)·σ²)
    // and a fresh contribution ((1-α)·α·δ²) so it tracks shifts in
    // residual scale at the same rate as the mean tracks shifts in
    // residual centre.  At @c n = 1 the variance is initialised to
    // 0; the warm-up guard in evaluate() suppresses queries until
    // @c n ≥ @c warmup_n, so the σ → 0 cold-start case never
    // produces a spurious infinite z-score.
    void ResidualHistoryTracker::record(const std::string& sat,
                                         const std::string& signal,
                                         double residual_m) {
        if (!cfg_.enabled) return;
        if (!std::isfinite(residual_m)) return;

        const std::string key = makeKey(sat, signal);
        Entry& e = entries_[key];

        if (e.n == 0) {
            e.mean = residual_m;
            e.var  = 0.0;
            e.n    = 1;
            return;
        }

        const double delta = residual_m - e.mean;
        e.mean = e.mean + cfg_.alpha * delta;
        e.var  = (1.0 - cfg_.alpha) * (e.var + cfg_.alpha * delta * delta);
        ++e.n;
    }

    // -----------------------------------------------------------------
    // evaluate(): z-score against current EWMA statistics
    // -----------------------------------------------------------------
    ResidualHistoryTracker::Evaluation
    ResidualHistoryTracker::evaluate(const std::string& sat,
                                      const std::string& signal,
                                      double residual_m) const {
        Evaluation ev{};
        if (!cfg_.enabled) return ev;
        if (!std::isfinite(residual_m)) return ev;

        const std::string key = makeKey(sat, signal);
        const auto it = entries_.find(key);
        if (it == entries_.end()) return ev;

        const Entry& e = it->second;
        if (e.n < cfg_.warmup_n) return ev;

        // Floor the variance so a momentarily-tiny σ doesn't blow up
        // the z-score.  10^-12 m² ≈ 1 µm σ — below carrier-tracking
        // noise, an effective floor that prevents pathologies.
        const double sigma = std::sqrt(std::max(e.var, 1e-12));
        ev.warmed_up = true;
        ev.z_score   = (residual_m - e.mean) / sigma;
        ev.is_outlier = std::abs(ev.z_score) > cfg_.z_threshold;
        return ev;
    }

    // -----------------------------------------------------------------
    // Resets
    // -----------------------------------------------------------------

    void ResidualHistoryTracker::resetSatSignal(const std::string& sat,
                                                  const std::string& signal) {
        if (!cfg_.enabled) return;
        entries_.erase(makeKey(sat, signal));
    }

    void ResidualHistoryTracker::resetSatellite(const std::string& sat) {
        if (!cfg_.enabled) return;
        const std::string prefix = sat + "_";
        // map keys are sorted; iterate the prefix range and drop matches.
        auto it = entries_.lower_bound(prefix);
        while (it != entries_.end() && it->first.compare(0, prefix.size(), prefix) == 0) {
            it = entries_.erase(it);
        }
    }

    void ResidualHistoryTracker::resetConstellationSignal(char constellation_prefix,
                                                           const std::string& signal) {
        if (!cfg_.enabled) return;
        // Keys have the form "<sat>_<signal>" where <sat> starts with
        // the constellation prefix character.  Without a prefix-aware
        // multimap we walk the whole map; entries_ is small (~10² in
        // typical multi-constellation runs) so the linear scan is fine.
        for (auto it = entries_.begin(); it != entries_.end(); ) {
            const std::string& key = it->first;
            const bool cons_match = !key.empty() && key[0] == constellation_prefix;
            bool sig_match = signal.empty();
            if (!sig_match) {
                // Compare the key's signal half to @p signal.
                const auto sep = key.find('_');
                if (sep != std::string::npos) {
                    sig_match = key.compare(sep + 1, std::string::npos, signal) == 0;
                }
            }
            if (cons_match && sig_match) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ResidualHistoryTracker::resetAll() {
        entries_.clear();
    }

    // -----------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------

    std::optional<ResidualHistoryTracker::Stats>
    ResidualHistoryTracker::getStats(const std::string& sat,
                                       const std::string& signal) const {
        const auto it = entries_.find(makeKey(sat, signal));
        if (it == entries_.end()) return std::nullopt;
        const Entry& e = it->second;
        Stats s;
        s.n = e.n;
        s.mean = e.mean;
        s.std_dev = (e.n >= 2) ? std::sqrt(std::max(e.var, 0.0)) : 0.0;
        return s;
    }

    ResidualHistoryTracker::Summary ResidualHistoryTracker::summarize() const {
        Summary out;
        out.entry_count = entries_.size();
        if (out.entry_count == 0) return out;

        std::vector<double> sigmas;
        sigmas.reserve(entries_.size());
        for (const auto& [_key, e] : entries_) {
            if (e.n >= cfg_.warmup_n) ++out.warmed_up;
            const double s = (e.n >= 2) ? std::sqrt(std::max(e.var, 0.0)) : 0.0;
            sigmas.push_back(s);
            if (s > out.max_std) out.max_std = s;
        }
        // Median via nth_element so the digest cost stays O(n) rather
        // than O(n log n) for large entry counts (rare but cheap to
        // get right).
        const auto mid = sigmas.begin() + sigmas.size() / 2;
        std::nth_element(sigmas.begin(), mid, sigmas.end());
        out.median_std = (sigmas.size() % 2 != 0)
            ? *mid
            : 0.5 * (*mid + *std::max_element(sigmas.begin(), mid));
        return out;
    }

} // namespace GnssProcessor

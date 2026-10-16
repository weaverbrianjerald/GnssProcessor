/**
 * @file MwSlipDetectorReset.cpp
 * @brief The MW detector's external resets and the pivot hint.
 *
 * Per satellite, per (satellite, signal), the hard-gap restart of every arc, and the
 * one-shot hint that keeps a slipped KalmanFilter pivot out of the next MW pivot
 * election.
 *
 * Part of MwSlipDetector (slip/MwSlipDetector.hpp lists every file of the class).
 */

#include "slip/MwSlipDetector.hpp"
#include <limits>

namespace GnssProcessor {

    using namespace mw;  // the constants table and the per-arc state (slip/MwArcState.hpp)

    // =================================================================
    // External reset
    // =================================================================

    void MwSlipDetector::resetSatellite(const std::string& satellite) {
        auto logger = log_manager_.getLogger("mw_slip_detector");

        // Helper: roll back one arc using its pre-epoch snapshot if
        // present (preserves accumulated noise statistics, breaks the
        // lock), or full-erase if no snapshot exists (legacy behavior).
        // Returns true if the arc was rolled back, false if erased.
        auto rollback_or_erase = [&](decltype(mw_stats_.begin())& cit) -> bool {
            const std::string key = cit->first;
            const size_t arc_n   = cit->second.count;
            auto snap_it = arc_snapshots_.find(key);
            if (snap_it != arc_snapshots_.end()) {
                cit->second = snap_it->second;
                cit->second.wl_locked  = false;
                cit->second.rounded_wl = 0;
                arc_snapshots_.erase(snap_it);
                logger->debug("Rolled back MW arc {} to pre-epoch snapshot "
                              "(external reset, arc_n was {}, post-rollback {}; "
                              "lock broken)",
                              key, arc_n, cit->second.count);
                ++cit;
                return true;
            }
            logger->debug("Cleared MW arc history for {} "
                          "(external reset, arc_n={}; no snapshot available)",
                          key, arc_n);
            cit = mw_stats_.erase(cit);
            return false;
        };

        auto it = mw_stats_.find(satellite);
        if (it != mw_stats_.end()) {
            rollback_or_erase(it);
        }

        const std::string prefix = satellite + "_";
        for (auto cit = mw_stats_.begin(); cit != mw_stats_.end(); ) {
            if (cit->first.rfind(prefix, 0) == 0) {
                rollback_or_erase(cit);
            } else {
                ++cit;
            }
        }

        // An external reset invalidates any demoted-level stash for the
        // sat: a rolled-back arc's mean reverts to the pre-event level, so
        // (mean − stash) would read ~0 with full rollback confidence, a
        // convincing but wrong "no slip" settle estimate.
        demoted_levels_.erase(satellite);
        for (auto dit = demoted_levels_.begin(); dit != demoted_levels_.end(); )
            if (dit->first.rfind(prefix, 0) == 0) dit = demoted_levels_.erase(dit);
            else ++dit;

        // If this satellite was an SD pivot, remove the pivot entry so the
        // pivot is re-selected next epoch.  The pivot-change handling in
        // detectLargeMwResets (S-transform or erase of the group's SD arcs)
        // needs a stored previous pivot, so it does not run for this group
        // next epoch; the dependent SD arcs are not reset here.
        for (auto pit = sd_pivot_sat_.begin(); pit != sd_pivot_sat_.end(); )
            if (pit->second == satellite) {
                logger->debug("Cleared SD pivot {} for group {} (external satellite reset)",
                    satellite, pit->first);
                pit = sd_pivot_sat_.erase(pit);
            } else {
                ++pit;
            }
    }

    void MwSlipDetector::resetSatelliteSignal(const std::string& satellite,
                                                const std::string& signal) {
        if (satellite.empty() || signal.empty()) return;
        auto logger = log_manager_.getLogger("mw_slip_detector");

        // Arc keys are formatted "<sat>_<sig1>_<sig2>".  Rollback (via
        // pre-epoch snapshot) or erase (fallback) entries where either
        // signal in the pair matches the slipped signal; preserve
        // entries on signal pairs that don't involve it.
        const std::string prefix = satellite + "_";
        int n_rolled = 0, n_erased = 0;
        for (auto cit = mw_stats_.begin(); cit != mw_stats_.end(); ) {
            if (cit->first.rfind(prefix, 0) != 0) { ++cit; continue; }
            const std::string rest = cit->first.substr(prefix.size());
            const auto us = rest.find('_');
            bool involves = false;
            if (us != std::string::npos) {
                const std::string sig1 = rest.substr(0, us);
                const std::string sig2 = rest.substr(us + 1);
                involves = (sig1 == signal || sig2 == signal);
            } else {
                // No "<sig1>_<sig2>" structure (e.g. raw signal key): match exactly.
                involves = (rest == signal);
            }
            if (involves) {
                const std::string key = cit->first;
                const size_t arc_n   = cit->second.count;
                auto snap_it = arc_snapshots_.find(key);
                if (snap_it != arc_snapshots_.end()) {
                    cit->second = snap_it->second;
                    cit->second.wl_locked  = false;
                    cit->second.rounded_wl = 0;
                    arc_snapshots_.erase(snap_it);
                    logger->debug("Rolled back MW arc {} to pre-epoch snapshot "
                                  "(per-signal reset, signal={}, arc_n was {}, "
                                  "post-rollback {}; lock broken)",
                                  key, signal, arc_n, cit->second.count);
                    ++cit;
                    ++n_rolled;
                } else {
                    logger->debug("Cleared MW arc history for {} (per-signal reset, "
                                  "signal={}, arc_n={}; no snapshot available)",
                                  key, signal, arc_n);
                    cit = mw_stats_.erase(cit);
                    ++n_erased;
                }
            } else {
                ++cit;
            }
        }

        if (n_rolled + n_erased > 0) {
            logger->info("MW per-signal reset {} {}: rolled back {} arc(s), "
                         "erased {} (no snapshot); arcs not involving this "
                         "signal preserved",
                         satellite, signal, n_rolled, n_erased);
        }

        // Invalidate demoted-level stashes touching this signal (same
        // rationale as resetSatellite: a rollback corrupts the settle
        // estimate's post-restart mean).
        for (auto dit = demoted_levels_.begin(); dit != demoted_levels_.end(); ) {
            bool involves = false;
            if (dit->first.rfind(prefix, 0) == 0) {
                const std::string rest = dit->first.substr(prefix.size());
                const auto us = rest.find('_');
                involves = (us != std::string::npos)
                    ? (rest.substr(0, us) == signal || rest.substr(us + 1) == signal)
                    : (rest == signal);
            }
            if (involves) dit = demoted_levels_.erase(dit);
            else ++dit;
        }
    }

    void MwSlipDetector::resetAll() {
        auto logger = log_manager_.getLogger("mw_slip_detector");
        const size_t n_arcs   = mw_stats_.size();
        const size_t n_pivots = sd_pivot_sat_.size();
        const size_t n_biases = sat_pair_bias_.size();
        const size_t n_xsat   = cross_sat_sigma_m_.size();
        const size_t n_snaps  = arc_snapshots_.size();
        mw_stats_.clear();
        sd_pivot_sat_.clear();
        sat_pair_bias_.clear();
        cross_sat_sigma_m_.clear();
        cross_sat_sigma_cons_.clear();
        cross_sat_sigma_global_ = std::numeric_limits<double>::quiet_NaN();
        cross_sat_devs_.clear();
        arc_snapshots_.clear();
        demoted_levels_.clear();
        if (n_arcs > 0 || n_pivots > 0 || n_biases > 0 || n_xsat > 0 || n_snaps > 0) {
            logger->info("MW detector resetAll: cleared {} arc(s), {} SD pivot(s), {} stored bias(es), {} cross-sat σ entries, {} arc snapshot(s)",
                         n_arcs, n_pivots, n_biases, n_xsat, n_snaps);
        }
    }

    void MwSlipDetector::notifyPivotForcedHandover(const std::string& satellite,
                                                    const std::string& reason) {
        if (satellite.empty()) return;
        auto logger = log_manager_.getLogger("mw_slip_detector");
        const auto [it, inserted] = deprioritized_pivot_sats_.insert(satellite);
        if (inserted) {
            logger->info("MW pivot deprioritization: {} ({}); next pivot "
                         "pick will avoid this satellite if alternatives exist.",
                         satellite, reason);
        }
    }

} // namespace GnssProcessor

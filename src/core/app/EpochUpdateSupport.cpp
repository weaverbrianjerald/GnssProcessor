/**
 * @file EpochUpdateSupport.cpp
 * @brief Implementation of the shared per-epoch measurement-rebuild helpers.
 *        See EpochUpdateSupport.hpp for the rationale (one implementation of
 *        the algorithm steps shared by the post-processing and real-time
 *        drivers).
 */

#include "app/EpochUpdateSupport.hpp"

#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "filter/KalmanFilter.hpp"
#include "filter/PivotManager.hpp"
#include "filter/ResidualHistoryTracker.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "measurement/MeasurementModel.hpp"
#include "slip/MwSlipDetector.hpp"

#include <algorithm>
#include <map>

namespace GnssProcessor {

int computeRebuildPassBudget(const StateManager& state_manager,
                             const GnssConfig& config,
                             const std::vector<PppObservation>& modeled_obs) {
    // Hard ceiling that guarantees loop termination regardless of measurement
    // count.  The computed budget below stays far under it for any physical
    // epoch, so it clamps only a pathological observation set.
    constexpr int kMaxRebuildPassesHardCap = 256;

    size_t n_meas_start = 0;
    for (const auto& obs : modeled_obs) n_meas_start += obs.observed.size();

    const size_t reset_budget = static_cast<size_t>(
        config.getBadMeasPercent() * static_cast<double>(n_meas_start)) + 3;

    return std::min<int>(
        kMaxRebuildPassesHardCap,
        static_cast<int>(std::max<size_t>({
            static_cast<size_t>(10),
            state_manager.getAmbiguities().size() + 3,
            reset_budget })));
}

void forwardKfPivotHandovers(KalmanFilter& kalman_filter,
                             MeasurementModel& meas_model,
                             MwSlipDetector* mw_detector,
                             LogManager& log_manager,
                             const std::string& epoch_prefix,
                             std::set<std::string>& carry_excluded) {
    // Take-and-clear: a request must not re-fire on a later pass unless the
    // detector fires again.  Drain the flagged constellations first and return
    // early when there are none, which is the common case.
    auto handover_cons = kalman_filter.takeRequestedPivotHandovers();
    if (handover_cons.empty()) return;

    auto handover_excludes  = kalman_filter.takeRequestedPivotHandoverExcludeSats();
    auto handover_reinflate = kalman_filter.takeRequestedPivotHandoverIonoReinflateSats();

    auto& res_hist  = kalman_filter.getResidualHistoryTracker();
    auto& pivot_mgr = meas_model.getPivotManager();
    // The current (pre-handover) pivot map, for the MW notify.  The
    // requestForcedHandover calls below only queue requests and do not move the
    // pivot, so this reference stays valid across the loop.
    const auto& current_pivots = pivot_mgr.getPivotSatellites();

    auto main_log = log_manager.getLogger("main_log");
    static const std::set<std::string> kNone;

    for (const auto& cons : handover_cons) {
        // Replacement-pivot exclusions (offset pivot + co-disturbed outliers)
        // so the forced handover lands on a consensus satellite rather than
        // bouncing to another offset sat (the handover flip-flop).
        const auto ex_it = handover_excludes.find(cons);
        const std::set<std::string>& ex_sats =
            ex_it != handover_excludes.end() ? ex_it->second : kNone;

        // Iono/sat-bias re-inflation targets for this constellation (code-
        // detected contamination only; empty otherwise).
        const auto rf_it = handover_reinflate.find(cons);
        const std::set<std::string>& rf_sats =
            rf_it != handover_reinflate.end() ? rf_it->second : kNone;

        if (!ex_sats.empty()) {
            std::string joined;
            for (const auto& s : ex_sats) {
                if (!joined.empty()) joined += ",";
                joined += s;
            }
            main_log->info(
                "{}Forwarding KF-detected pivot slip on {} to PivotManager "
                "for handover next pass (exclude replacements: {})",
                epoch_prefix, cons, joined);
        } else {
            main_log->info(
                "{}Forwarding KF-detected pivot slip on {} to PivotManager for handover next pass",
                epoch_prefix, cons);
        }

        const char* kHandoverReason =
            "KF post-fit residual detector flagged coherent pivot offset";
        pivot_mgr.requestForcedHandover(cons, kHandoverReason, ex_sats, rf_sats);

        // Propagate the handover to the MW pivot picker so it doesn't re-elect
        // the slipped sat next epoch.  Skipped when no MW detector is wired.
        if (mw_detector) {
            auto piv_it = current_pivots.find(cons);
            if (piv_it != current_pivots.end() && !piv_it->second.empty())
                mw_detector->notifyPivotForcedHandover(piv_it->second, kHandoverReason);
        }

        // After re-referencing to a new pivot the BSSD residuals are not
        // comparable to the pre-handover EWMA statistics, so clear them to force
        // a fresh warm-up.  No-op when the tracker is disabled or the
        // constellation name has no prefix letter below.
        const char cons_prefix =
            cons == "GPS"     ? 'G' :
            cons == "Galileo" ? 'E' :
            cons == "BeiDou"  ? 'C' :
            cons == "GLONASS" ? 'R' :
            cons == "QZSS"    ? 'J' :
            cons == "SBAS"    ? 'S' : '\0';
        if (cons_prefix != '\0' && res_hist.isEnabled())
            res_hist.resetConstellationSignal(cons_prefix, /*signal=*/"");

        // Release this constellation's carried exclusions so the rebuild pass
        // re-forms its SD rows against the new pivot.  Otherwise every row the
        // spatial detector swept into its group stays suppressed, and the
        // per-row dispatch cannot catch an independent slip that coincided with
        // the pivot's.
        if (cons_prefix != '\0') {
            size_t cleared = 0;
            for (auto it = carry_excluded.begin(); it != carry_excluded.end(); ) {
                if (!it->empty() && (*it)[0] == cons_prefix) {
                    it = carry_excluded.erase(it);
                    ++cleared;
                } else {
                    ++it;
                }
            }
            if (cleared > 0)
                main_log->debug(
                    "{}Released {} carry-excluded measurement(s) for {} so the "
                    "post-handover rebuild pass re-includes its SD rows and "
                    "re-runs outlier checks",
                    epoch_prefix, cleared, cons);
        }
    }
}

void reinflateResetSatStates(const KalmanFilter& kalman_filter,
                             StateManager& state_manager,
                             const MeasurementModel& meas_model,
                             const GnssConfig& config) {
    // A complete filter reset re-inflates the whole covariance itself, so
    // per-sat reinflation is redundant there and is skipped.  The gate sits here
    // rather than at each call site, so the post-processing and real-time callers
    // behave identically.
    if (kalman_filter.wasCompleteReset()) return;

    const auto& sr_cfg = config.getSlipStateReinflationConfig();
    if (!sr_cfg.enabled) return;
    const double io_std = sr_cfg.iono_std_m;
    const double sb_std = sr_cfg.sat_bias_std_m;
    if (io_std <= 0.0 && sb_std <= 0.0) return;   // both branches disabled

    // A per-(sat, signal) reset does not justify sat-wide reinflation: one
    // slipped signal leaves the rest of the satellite's phase lock, and its
    // (iono, sat-bias) partition, intact.  Only a full-satellite ambiguity reset
    // does, i.e. a sat in getLastResetSatellites() with no per-signal entry
    // (every phase ambiguity reseeded, so the partition would be locked in).
    std::set<std::string> per_signal_sats;
    for (const auto& kv : kalman_filter.getLastResetSignals())
        per_signal_sats.insert(kv.first);

    // BSSD pivots carry frozen (0, 0) rows; reinflating them is a StateManager-
    // guarded no-op, but skipping here avoids a per-pivot warn line.
    const std::map<std::string, std::string>* pivots =
        config.enableBetweenSatelliteSd() ? &meas_model.getBssdPivotSatellites()
                                          : nullptr;

    for (const auto& sat_id : kalman_filter.getLastResetSatellites()) {
        if (per_signal_sats.count(sat_id)) continue;   // per-signal reset, not sat-wide

        if (pivots) {
            const auto pit = pivots->find(PppUtils::getSystemString(sat_id));
            if (pit != pivots->end() && pit->second == sat_id) continue;
        }

        // Values are preserved (slant iono / hardware delays don't change just
        // because phase lock was lost); only the diagonal variance is broken so
        // the filter re-partitions cleanly over the next several epochs.
        if (io_std > 0.0) {
            state_manager.reinflateIonosphereVariance(sat_id, io_std * io_std);
            // Constant-velocity model: decorrelate the companion rate too
            // (no-op unless enableIonoRate; 0.0 => initial_std.ionosphere_rate default).
            state_manager.reinflateIonosphereRateVariance(sat_id, 0.0);
        }
        if (sb_std > 0.0)
            state_manager.reinflateSatBiasVariancesForSat(sat_id, sb_std * sb_std);
    }
}

} // namespace GnssProcessor

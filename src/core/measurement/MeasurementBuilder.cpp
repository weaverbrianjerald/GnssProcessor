/**
 * @file MeasurementBuilder.cpp
 * @brief Measurement-differencing strategy implementations + factory.
 *
 * The strategies are thin orchestrators: they call back into the (private)
 * building blocks of @c MeasurementModel, which declares them @c friend.  All
 * delicate measurement numerics remain owned by @c MeasurementModel; this file
 * only encodes WHICH steps run for each differencing regime.
 */

#include "measurement/MeasurementBuilder.hpp"
#include "measurement/MeasurementModel.hpp"
#include "common/PppUtils.hpp"

namespace GnssProcessor {

DifferencingMode differencingModeFor(const GnssConfig& config) {
    if (config.isBaselineMode())          return DifferencingMode::DoubleDifference;
    if (config.enableBetweenSatelliteSd()) return DifferencingMode::BetweenSatellite;
    return DifferencingMode::None;
}

// ===================================================================
// BssdMeasurementBuilder (PPP-RTK between-satellite single difference)
// ===================================================================

void BssdMeasurementBuilder::preparePivots(MeasurementModel& model,
                                           const std::vector<PppObservation>& observations,
                                           const std::string& epoch_prefix,
                                           const GnssTime& epoch) {
    // --- Pivot handover ---
    // First pass of each epoch runs handover normally (forced and
    // disturbance-detection paths both active).  Rebuild passes within the
    // same epoch keep the pivot fixed (so SD ambiguities stay consistent),
    // except when a forced-handover request is pending from the KF post-fit
    // residual detector — then re-enter in forced_only mode so the same
    // epoch's rebuild runs against the new pivot without risking a cascade.
    const bool first_pass = !(epoch == model.last_pivot_epoch_);
    const bool has_forced = model.pivot_manager_.hasPendingForcedHandover();
    if (first_pass || has_forced) {
        const bool forced_only = !first_pass && has_forced;
        model.handleBssdPivotHandover(observations, epoch_prefix, forced_only);
        model.last_pivot_epoch_ = epoch;
    }

    // --- Pivot signal-coverage map ---
    // Only signals tracked by the pivot satellite may enter the SD update —
    // BssdTransform groups SD pairs per full signal type, so a non-pivot's
    // L1C with no L1C row on the pivot would become an undifferenced singleton.
    const auto& pivot_sats = model.pivot_manager_.getPivotSatellites();
    for (const auto& obs : observations) {
        const std::string cons = PppUtils::getSystemString(obs.satellite);
        auto piv_it = pivot_sats.find(cons);
        if (piv_it == pivot_sats.end() || piv_it->second != obs.satellite)
            continue;
        auto& sigs = model.bssd_pivot_signals_[cons];
        for (const auto& sig_val : obs.observed) {
            if (sig_val.first.size() >= 2) sigs.insert(sig_val.first);
        }
    }
}

void BssdMeasurementBuilder::freezePivotCovariances(MeasurementModel& model) {
    model.freezePivotAmbiguityCovariance();
    model.freezePivotIonoAndBiasCovariance();
}

void BssdMeasurementBuilder::applyDifferencing(MeasurementModel& model,
                                               EpochMeasurement& update,
                                               const std::string& epoch_prefix) {
    model.bssd_transform_.apply(update, epoch_prefix);
}

// ===================================================================
// DdMeasurementBuilder (baseline double difference)
// ===================================================================

EpochMeasurement DdMeasurementBuilder::buildDoubleDifference(
    MeasurementModel& model,
    const std::vector<PppObservation>& rov_modeled,
    const std::vector<PppObservation>& ref_modeled,
    const Eigen::VectorXd& current_state,
    const std::set<std::string>& excluded) {
    return model.buildDdMeasurement(rov_modeled, ref_modeled, current_state, excluded);
}

// ===================================================================
// Factory
// ===================================================================

std::unique_ptr<MeasurementDifferencingStrategy> MeasurementBuilderFactory::create(
    DifferencingMode mode, LogManager& log_manager) {
    auto log = log_manager.getLogger("measurement_model");
    switch (mode) {
        case DifferencingMode::None:
            log->info("Measurement builder: undifferenced (PPP)");
            return std::make_unique<UndifferencedMeasurementBuilder>();
        case DifferencingMode::BetweenSatellite:
            log->info("Measurement builder: between-satellite single difference (PPP-RTK)");
            return std::make_unique<BssdMeasurementBuilder>();
        case DifferencingMode::DoubleDifference:
            log->info("Measurement builder: double difference (baseline)");
            return std::make_unique<DdMeasurementBuilder>();
    }
    log->warn("Unknown differencing mode; defaulting to undifferenced.");
    return std::make_unique<UndifferencedMeasurementBuilder>();
}

} // namespace GnssProcessor

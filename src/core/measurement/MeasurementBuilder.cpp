/**
 * @file MeasurementBuilder.cpp
 * @brief Measurement-differencing strategy implementations + factory.
 *
 * The strategies are thin orchestrators: they call back into the (private)
 * building blocks of @c MeasurementModel, which declares them @c friend.  The
 * measurement numerics remain owned by @c MeasurementModel; this file encodes
 * only which steps run for each differencing regime.
 */

#include "measurement/MeasurementBuilder.hpp"
#include "measurement/MeasurementModel.hpp"
#include "common/PppUtils.hpp"

namespace GnssProcessor {

DifferencingMode differencingModeFor(const GnssConfig& config) {
    // The network producer runs on undifferenced, uncombined rows and estimates the
    // satellite-side bias lumps explicitly, so it never applies receiver-side BSSD,
    // even when a between_satellite_sd toggle is present in its config.
    if (config.getProcessingMode() == ProcessingMode::PPP_RTK_Network)
                                          return DifferencingMode::None;
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
    // The first pass of each epoch runs the handover with both the forced and the
    // disturbance-detection paths active.  Rebuild passes within the same epoch
    // keep the pivot fixed, so the SD ambiguities stay consistent, unless a
    // forced-handover request is pending (for example from the KF post-fit
    // residual detector).  In that case the handover re-enters in forced_only
    // mode, so the rebuild of the same epoch runs against the new pivot and the
    // disturbance path cannot trigger a further handover.
    const bool first_pass = !(epoch == model.last_pivot_epoch_);
    const bool has_forced = model.pivot_manager_.hasPendingForcedHandover();
    if (first_pass || has_forced) {
        const bool forced_only = !first_pass && has_forced;
        model.handleBssdPivotHandover(observations, epoch_prefix, forced_only);
        model.last_pivot_epoch_ = epoch;
    }

    // --- Pivot signal-coverage map ---
    // Records the signals tracked by each constellation's pivot satellite; only
    // those may enter the SD update.  BssdTransform groups SD pairs per full signal
    // type, so a non-pivot L1C row with no L1C row on the pivot would become an
    // undifferenced singleton.
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

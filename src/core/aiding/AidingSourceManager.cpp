/**
 * @file AidingSourceManager.cpp
 * @brief Owns the config-selected aiding-source set (CycleSlipManager analog).
 *
 * @see docs/aiding_source_abstraction_design.md
 */

#include "aiding/AidingSourceManager.hpp"
#include "aiding/AidingSourceRegistry.hpp"
#include "aiding/ImuAidingSource.hpp"
#include <iterator>

namespace GnssProcessor {

AidingSourceManager::AidingSourceManager(const GnssConfig& config,
                                         LogManager& log_manager) {
    sources_ = AidingSourceRegistry::createEnabled(config, log_manager);

    // Recover the typed IMU handle — "polymorphic herd + one typed handle for
    // the privileged source", exactly CycleSlipManager's asMelbourneWubbena().
    for (const auto& s : sources_) {
        if (auto* i = s->asImu()) { imu_ = i; break; }
    }
}

void AidingSourceManager::reserveAllStates(StateManager& sm) {
    for (auto& s : sources_) s->reserveStates(sm);
}

void AidingSourceManager::ingestSample(const AidingSample& sample) {
    for (auto& s : sources_) s->ingestSample(sample);
}

MeasurementRows AidingSourceManager::collectMeasurement(const AidingEpochContext& ctx) {
    MeasurementRows rows;
    for (auto& s : sources_) {
        if (s->reparameterizesGnssRows()) continue;  // column sources (TC-IMU) don't append rows
        MeasurementRows r = s->contributeMeasurement(ctx);
        rows.insert(rows.end(),
                    std::make_move_iterator(r.begin()),
                    std::make_move_iterator(r.end()));
    }
    return rows;
}

bool AidingSourceManager::anySourceBridgesGaps() const {
    for (const auto& s : sources_)
        if (s->bridgesGaps()) return true;
    return false;
}

std::vector<PredictContribution>
AidingSourceManager::collectPredict(const AidingEpochContext& ctx) {
    std::vector<PredictContribution> contributions;
    contributions.reserve(sources_.size());
    for (auto& s : sources_) {
        PredictContribution c = s->contributePredict(ctx);
        if (c.valid) contributions.push_back(std::move(c));
    }
    return contributions;
}

void AidingSourceManager::applyTcJacobianInputs(MeasurementModel& model) const {
    // Only the IMU re-parameterizes GNSS rows today; it self-guards on the
    // coupling mode / data availability.
    if (imu_) imu_->applyTcJacobianInputs(model);
}

bool AidingSourceManager::anySourceRequiresNaiveRelinearization() const {
    for (const auto& s : sources_)
        if (s->requiresNaiveRelinearization()) return true;
    return false;
}

} // namespace GnssProcessor

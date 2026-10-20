/**
 * @file StationRows.cpp
 * @brief The data rows of one network station (see network/assembly/StationRows.hpp).
 */

#include "network/assembly/StationRows.hpp"

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "measurement/RowRules.hpp"
#include "measurement/RowTrace.hpp"
#include "model/PppModel.hpp"
#include "products/ProductCache.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <cmath>
#include <exception>

namespace GnssProcessor {

namespace {

/// The NL/WL reference bands of a satellite: none.  A network station estimates one ambiguity
/// per band, so its inputs name the per-band basis and read no reference band.
const std::map<std::string, char>& noNlWlReferenceBands() {
    static const std::map<std::string, char> none;
    return none;
}

/// The configuration, the station's model tables and the live SSR store the rules read.
RowProducts productsOf(const StationRowContext& c) {
    return RowProducts{c.config, c.model, SsrCorrectionStore::getInstance()};
}

} // namespace

ParamSlot NetView::find(const ParamRef& ref) const {
    const int k = findParam(set_, ref);
    if (k < 0) return ParamSlot{};
    return ParamSlot{k, set_.slots[static_cast<std::size_t>(k)].x_lin};
}

StationRowInputs::StationRowInputs(const GnssConfig& config, const PppModel& model,
                                   LogManager& log_manager,
                                   const std::map<std::string, int>& glonass_slots,
                                   std::map<std::string, std::set<std::string>> allowlist)
    : freq(log_manager),
      rx_bias(config, model, log_manager),
      phase_bias_allowlist(std::move(allowlist)),
      log_manager(&log_manager),
      clock_refs(&ProductCache::getInstance().getClockRefSignals()) {
    freq.setGlonassSlots(glonass_slots);
}

ObsInput rowObsInput(const StationRowContext& c) {
    return ObsInput{productsOf(c), c.inputs.freq, &c.inputs.rx_bias, noNlWlReferenceBands(),
                    kNetworkAmbiguityBasis, *c.inputs.clock_refs};
}

ObsInput residualSeedObsInput(const StationRowContext& c) {
    return ObsInput{productsOf(c), c.inputs.rx_bias.frequencyHelper(), &c.inputs.rx_bias,
                    noNlWlReferenceBands(), kNetworkAmbiguityBasis, *c.inputs.clock_refs};
}

ObsInput ambiguitySeedObsInput(const StationRowContext& c) {
    return ObsInput{productsOf(c), c.inputs.freq, &c.inputs.rx_bias, noNlWlReferenceBands(),
                    kNetworkAmbiguityBasis, *c.inputs.clock_refs, FdmaChannel::Zero};
}

EpochMeasurement networkStationRows(const StationRowContext& c, const StationParams& set,
                                    const std::vector<PppObservation>& observations) {
    EpochMeasurement update;
    update.time = observations.empty() ? GnssTime{0, 0.0} : observations[0].time;
    const std::string epoch_prefix;   // the log pattern carries the timestamp
    auto log = c.log.getLogger("measurement_model");

    if (observations.empty()) {
        log->error("No observations provided for measurement update, mode=PPP-RTK");
        return update;
    }

    const PivotSignalMap no_pivot_filter;
    const std::set<std::string> no_exclusions;
    const auto usable = [&](const PppObservation& obs, const std::string& ref_type) {
        return isUsableMeasurement(c.config, c.log, no_pivot_filter, obs, ref_type, no_exclusions,
                                   epoch_prefix);
    };

    // The rows to build: every usable signal, the ones with invalid stored geometry included
    // (they are counted here and dropped below, which the log line names).
    std::size_t num_obs = 0;
    for (const PppObservation& obs : observations)
        for (const auto& entry : obs.observed)
            if (usable(obs, entry.first)) ++num_obs;
    if (num_obs == 0) {
        log->error("No valid observations found in residuals, mode=PPP-RTK");
        return update;
    }

    const std::size_t state_size = set.slots.size();
    update.innovations.resize(static_cast<Eigen::Index>(num_obs));
    update.R_v.resize(static_cast<Eigen::Index>(num_obs));
    update.H.resize(static_cast<Eigen::Index>(num_obs), static_cast<Eigen::Index>(state_size));
    update.H.setZero();
    update.satellite_ids.reserve(num_obs);
    update.signal_types.reserve(num_obs);

    const NetView view(set);
    const ParamLookup lookup(view);
    const ObsInput input = rowObsInput(c);
    const RowProducts products = productsOf(c);

    std::size_t row = 0;
    for (const PppObservation& obs : observations) {
        if (!hasValidGeometry(c.config, c.log, obs, epoch_prefix)) continue;
        for (const auto& entry : obs.observed) {
            const std::string& ref_type = entry.first;
            if (!usable(obs, ref_type)) continue;
            try {
                const ObservationEquation eq = observationEquation(input, obs, ref_type, lookup);
                if (!eq.usable) continue;
                ObsTermValues terms;
                const bool decomposed = wantsRowDecomposition(c.log, obs);
                const double predicted = evaluate(eq, decomposed ? &terms : nullptr);
                Eigen::VectorXd jacobian = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(state_size));
                scatterDense(eq, jacobian);
                if (decomposed) traceRowDecomposition(c.log, obs, ref_type, predicted, terms);

                const auto r = static_cast<Eigen::Index>(row);
                update.innovations(r) = entry.second - predicted;
                update.R_v(r)         = rowVariance(products, obs, ref_type);
                update.H.row(r)       = jacobian;
                update.satellite_ids.push_back(obs.satellite);
                update.signal_types.push_back(ref_type);
                if (!std::isfinite(update.innovations(r)) || !std::isfinite(update.R_v(r))) {
                    update.innovations(r) = 0.0;
                    update.R_v(r)         = 1e6;
                }
                log->trace("Meas {}/{}: obs={:.3f}, mod={:.3f}, pred={:.3f}, innov={:.3f}",
                           obs.satellite, ref_type, entry.second, obs.modeled.at(ref_type), predicted,
                           update.innovations(r));
                ++row;
            } catch (const std::exception&) {
                continue;
            }
        }
    }

    if (row == 0) {
        log->error("No valid measurement updates computed, mode=PPP-RTK");
        update.innovations.resize(0);
        update.R_v.resize(0);
        update.H.resize(0, static_cast<Eigen::Index>(state_size));
        update.satellite_ids.clear();
        update.signal_types.clear();
        return update;
    }
    if (row < num_obs) {
        log->warn("Processed {} observations out of {} expected, mode=PPP-RTK", row, num_obs);
        update.innovations.conservativeResize(static_cast<Eigen::Index>(row));
        update.R_v.conservativeResize(static_cast<Eigen::Index>(row));
        update.H.conservativeResize(static_cast<Eigen::Index>(row), Eigen::NoChange);
        update.satellite_ids.resize(row);
        update.signal_types.resize(row);
    }
    update.bad_residual_count = 0;
    log->info("Computed measurement update: {} valid innovations, H ({}x{}), mode=PPP-RTK", row,
              update.H.rows(), update.H.cols());
    return update;
}

} // namespace GnssProcessor

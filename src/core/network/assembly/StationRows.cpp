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

#include <algorithm>
#include <cmath>
#include <exception>

namespace GnssProcessor {

namespace {

/// The Jacobian entries of one row: one per part of the equation, in the order of its terms and
/// parts.  A slot that two parts name keeps the later value, as an assignment into a dense row
/// would (measurement/ObservationEquation.hpp, scatterDense).
void rowPartialsOf(const ObservationEquation& eq, std::vector<RowPartial>& out) {
    out.clear();
    for (std::uint8_t i = 0; i < eq.n; ++i) {
        const ObsAddend& a = eq.addend[i];
        for (std::uint8_t k = 0; k < a.n_part; ++k) {
            const RowPartial entry{a.part[k].column, a.scale * a.part[k].partial};
            const auto same = std::find_if(out.begin(), out.end(), [&](const RowPartial& p) {
                return p.slot == entry.slot;
            });
            if (same != out.end()) same->value = entry.value;
            else                   out.push_back(entry);
        }
    }
}

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
    // The full-equation seeds take the rows' frequency helper (the station's own GLONASS
    // channels); the legacy seeds keep the resolver's default channel table.
    const PppUtils& freq = c.config.seedFullEquation() ? c.inputs.freq
                                                       : c.inputs.rx_bias.frequencyHelper();
    return ObsInput{productsOf(c), freq, &c.inputs.rx_bias, noNlWlReferenceBands(),
                    kNetworkAmbiguityBasis, *c.inputs.clock_refs};
}

ObsInput ambiguitySeedObsInput(const StationRowContext& c) {
    // The full-equation seeds take the satellite's own channel, as the rows do; the legacy
    // ambiguity seed keeps channel 0.
    const FdmaChannel fdma = c.config.seedFullEquation() ? FdmaChannel::Satellite : FdmaChannel::Zero;
    return ObsInput{productsOf(c), c.inputs.freq, &c.inputs.rx_bias, noNlWlReferenceBands(),
                    kNetworkAmbiguityBasis, *c.inputs.clock_refs, fdma};
}

double rowPartial(const StationDataRows& rows, Eigen::Index row, int slot) {
    const auto r = static_cast<std::size_t>(row);
    for (std::size_t e = rows.row_start[r]; e < rows.row_start[r + 1]; ++e)
        if (rows.partials[e].slot == slot) return rows.partials[e].value;
    return 0.0;
}

Eigen::MatrixXd slotColumns(const StationDataRows& rows, int first_slot, int count) {
    Eigen::MatrixXd block = Eigen::MatrixXd::Zero(rows.rows(), count);
    for (Eigen::Index r = 0; r < rows.rows(); ++r) {
        const auto ru = static_cast<std::size_t>(r);
        for (std::size_t e = rows.row_start[ru]; e < rows.row_start[ru + 1]; ++e) {
            const int k = rows.partials[e].slot - first_slot;
            if (k >= 0 && k < count) block(r, k) = rows.partials[e].value;
        }
    }
    return block;
}

Eigen::MatrixXd denseJacobian(const StationDataRows& rows) {
    return slotColumns(rows, 0, static_cast<int>(rows.n_slots));
}

StationDataRows networkStationRows(const StationRowContext& c, const StationParams& set,
                                   const std::vector<PppObservation>& observations) {
    StationDataRows rows;
    rows.n_slots = set.slots.size();
    const std::string epoch_prefix;   // the log pattern carries the timestamp
    auto log = c.log.getLogger("measurement_model");

    if (observations.empty()) {
        log->error("No observations provided for measurement update, mode=PPP-RTK");
        return rows;
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
        return rows;
    }

    rows.innovations.resize(static_cast<Eigen::Index>(num_obs));
    rows.R_v.resize(static_cast<Eigen::Index>(num_obs));
    rows.satellite_ids.reserve(num_obs);
    rows.signal_types.reserve(num_obs);
    rows.row_start.reserve(num_obs + 1);

    const NetView view(set);
    const ParamLookup lookup(view);
    const ObsInput input = rowObsInput(c);
    const RowProducts products = productsOf(c);

    std::vector<RowPartial> entries;   // one row's entries, kept once the row is complete
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
                rowPartialsOf(eq, entries);
                if (decomposed) traceRowDecomposition(c.log, obs, ref_type, predicted, terms);

                const auto r = static_cast<Eigen::Index>(row);
                rows.innovations(r) = entry.second - predicted;
                rows.R_v(r)         = rowVariance(products, obs, ref_type);
                rows.partials.insert(rows.partials.end(), entries.begin(), entries.end());
                rows.row_start.push_back(rows.partials.size());
                rows.satellite_ids.push_back(obs.satellite);
                rows.signal_types.push_back(ref_type);
                if (!std::isfinite(rows.innovations(r)) || !std::isfinite(rows.R_v(r))) {
                    rows.innovations(r) = 0.0;
                    rows.R_v(r)         = 1e6;
                }
                log->trace("Meas {}/{}: obs={:.3f}, mod={:.3f}, pred={:.3f}, innov={:.3f}",
                           obs.satellite, ref_type, entry.second, obs.modeled.at(ref_type), predicted,
                           rows.innovations(r));
                ++row;
            } catch (const std::exception&) {
                continue;
            }
        }
    }

    if (row == 0) {
        log->error("No valid measurement updates computed, mode=PPP-RTK");
        rows.innovations.resize(0);
        rows.R_v.resize(0);
        rows.partials.clear();
        rows.row_start.assign(1, 0);
        rows.satellite_ids.clear();
        rows.signal_types.clear();
        return rows;
    }
    if (row < num_obs) {
        log->warn("Processed {} observations out of {} expected, mode=PPP-RTK", row, num_obs);
        rows.innovations.conservativeResize(static_cast<Eigen::Index>(row));
        rows.R_v.conservativeResize(static_cast<Eigen::Index>(row));
    }
    // The Jacobian's shape is printed as before: rows by slots of the set.
    log->info("Computed measurement update: {} valid innovations, H ({}x{}), mode=PPP-RTK", row,
              row, rows.n_slots);
    return rows;
}

} // namespace GnssProcessor

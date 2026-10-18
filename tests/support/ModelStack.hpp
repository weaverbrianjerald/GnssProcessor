#pragma once
/**
 * @file ModelStack.hpp
 * @brief A single-receiver model stack over a configuration given as JSON text: the
 *        configuration, an isolated SSR store, the product and RINEX parsers, the state
 *        manager, the PPP model and the measurement model, built in the order the filter builds
 *        them.
 *
 * The tests of the observation equation and of the seeds build one per case and drive it with
 * synthetic observations.
 */

#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "measurement/MeasurementModel.hpp"
#include "measurement/ObservationEquation.hpp"
#include "measurement/UserParamView.hpp"
#include "model/PppModel.hpp"
#include "products/ProductParser.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "rinex/RinexParser.hpp"
#include "support/TestTempDir.hpp"

#include <Eigen/Dense>

#include <filesystem>
#include <fstream>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace gnss_test {

using namespace GnssProcessor;

/// One satellite with the given signals.  Each phase is observed at the modeled value plus an
/// offset (the ambiguity, in metres, distinct per signal); each code at the modeled value.
/// @param sat       Satellite id ("G12").
/// @param sys       Constellation.
/// @param signals   The signals ("C1W", "L1W", ...; an iono-free pair "C1W_C2W" for combined rows).
/// @param epoch     Epoch counter (the time of the observation advances 30 s per epoch).
/// @param azimuth   Azimuth (deg).
/// @param elevation Elevation (deg).
/// @param modeled   The modeled value of every signal (m).
inline PppObservation syntheticObservation(const std::string& sat, System sys,
                                           const std::vector<std::string>& signals, int epoch,
                                           double azimuth = 45.0, double elevation = 45.0,
                                           double modeled = 2.2e7) {
    PppObservation o{};
    o.satellite = sat;
    o.system = sys;
    o.time = GnssTime{2418, 1000.0 + 30.0 * epoch};
    o.rx_pos_linearization = Eigen::Vector3d(6378137.0, 0.0, 0.0);
    o.los = Eigen::Vector3d(1.0, 1.0, 1.0).normalized();
    o.range = 2.2e7;
    o.sat_pos = o.rx_pos_linearization + o.range * o.los;
    o.elevation = elevation;
    o.azimuth = azimuth;
    o.mf_w = 1.0 / std::sin(elevation * 3.14159265358979323846 / 180.0);
    o.mf_d = o.mf_w;
    double offset = 3.0;
    for (const std::string& signal : signals) {
        const bool phase = signal[0] == 'L';
        o.modeled[signal] = modeled;
        o.observed[signal] = modeled + (phase ? offset : 0.0);
        o.variances[signal] = phase ? 0.000025 : 0.25;
        if (phase) offset += 4.0;
    }
    return o;
}

/// A configuration and the model stack around it.
struct ModelStack {
    GnssConfig config;
    SsrCorrectionStore store;  // empty, and isolated from the process-wide store
    std::unique_ptr<ProductParser> products;
    std::unique_ptr<RinexParser> rinex;
    std::unique_ptr<StateManager> states;
    std::unique_ptr<PppModel> ppp;
    std::unique_ptr<MeasurementModel> model;

    /// Loads a configuration and builds the stack around it.
    /// @param mode        processing.mode ("ppp-rtk" is uncombined; "ppp" with uncombined_enabled false is iono-free).
    /// @param processing  extra members of the processing object (each starting with a comma).
    /// @param estimation  extra members of the estimation object (each starting with a comma).
    /// @param bssd        estimation.between_satellite_sd.enabled.
    /// @param check       processing.obs_equation_check.
    /// @param top         extra top-level members (each starting with a comma).
    bool load(LogManager& log, const std::string& tag, const std::string& mode,
              const std::string& processing, const std::string& estimation, bool bssd,
              bool check, const std::string& top = "") {
        const auto path = gnss_test::tempDir() / (std::string("obs_equation_") + tag + ".json");
        {
            std::ofstream out(path);
            out << R"({
                "processing": {"mode": ")" << mode << R"(", "active_systems": ["GPS", "Galileo"],
                               "obs_equation_check": )" << (check ? "true" : "false")
                << processing << R"(},
                "estimation": {
                    "estimate_velocity": false,
                    "estimate_phase_clock": false,
                    "estimate_ifb": false,
                    "estimate_sat_biases": false,
                    "no_bia_code_downweight": 100.0,
                    "ambiguity_resolution": {"mode": 0},
                    "between_satellite_sd": {"enabled": )" << (bssd ? "true" : "false") << R"(})"
                << estimation << R"(
                },
                "filter": {"initial_std": {
                    "position": 1.0, "clock": 1.0, "troposphere": 0.1,
                    "ionosphere": 1.0, "ambiguity": 100.0
                }})" << top << R"(
            })";
        }
        return build(log, path);
    }

    /// Loads a configuration given as JSON text and builds the stack around it.
    bool loadText(LogManager& log, const std::string& tag, const std::string& text) {
        const auto path = gnss_test::tempDir() / (std::string("obs_equation_") + tag + ".json");
        {
            std::ofstream out(path);
            out << text;
        }
        return build(log, path);
    }

    /// Loads the file (and removes it) and builds the model stack around the configuration.
    bool build(LogManager& log, const std::filesystem::path& path) {
        const bool loaded = config.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (!loaded) return false;
        products = std::make_unique<ProductParser>(config, log);
        rinex = std::make_unique<RinexParser>(config, log, *products);
        states = std::make_unique<StateManager>(5, log, config);
        ppp = std::make_unique<PppModel>(config, *products, log, *rinex);
        model = std::make_unique<MeasurementModel>(*states, *ppp, config, log);
        model->setSsrStore(&store);
        auto x = states->getStateVector();
        x.segment<3>(states->getPositionIndex()) = Eigen::Vector3d(6378137.0, 0.0, 0.0);
        states->setStateVector(x);
        return true;
    }

    /// Moves every state off its seed by a deterministic, unequal amount, so that a wrong
    /// column or a wrong grouping of the sum changes the bits.
    void perturbStates() {
        Eigen::VectorXd x = states->getStateVector();
        for (Eigen::Index i = 3; i < x.size(); ++i)
            x(i) += 0.0173 * static_cast<double>(i + 1) + 0.00031 * static_cast<double>(i * i);
        x.segment<3>(states->getPositionIndex()) += Eigen::Vector3d(0.0131, -0.0272, 0.0409);
        states->setStateVector(x);
    }

    /// The term list of one row at the current state of the stack.
    ObservationEquation equation(LogManager& log, const PppObservation& o,
                                 const std::string& signal,
                                 const std::map<std::string, char>& nlwl = {}) const {
        const Eigen::VectorXd x = states->getStateVector();
        const UserParamView view(*states, x, states->getStateSize());
        const PppUtils utils(log);
        const ObsInput in{RowProducts{config, *ppp, store}, utils, nullptr, nlwl};
        return observationEquation(in, o, signal, ParamLookup(view));
    }
};

} // namespace gnss_test

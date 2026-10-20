#pragma once
/**
 * @file StationBirthFixture.hpp
 * @brief One network station over a model stack: a loaded network configuration, the station's
 *        row inputs and a fresh parameter set, with synthetic observations to drive its births,
 *        its shared-parameter admission and its rows.
 *
 * The model stack (tests/support/ModelStack.hpp) supplies the configuration and the PPP model the
 * station's inputs refer to; the station itself reads no state manager.  The bias product's
 * clock-reference map is the fixture's own (empty: no .BIA product), so a test that needs the
 * receiver code biases names the reference pair it wants.
 */

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/StationRows.hpp"
#include "network/state/StationParams.hpp"
#include "ModelStack.hpp"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace gnss_test {

/// What a station fixture's configuration contains beyond the network posture every case shares.
struct BirthOptions {
    /// The processing.active_systems array.
    std::string systems = R"(["GPS", "Galileo"])";
    /// The processing.signal_priorities object.
    std::string priorities = R"({"GPS": "WC", "Galileo": "QX"})";
    /// Extra members of the network object, each starting with a comma.
    std::string network_extra;
    /// Extra members of the estimation object, each starting with a comma.
    std::string estimation_extra;
    /// estimation.estimate_ifb (the receiver code biases).
    bool estimate_ifb = false;
};

/// A network station over a configuration given by @ref BirthOptions.
struct BirthFixture {
    GnssProcessor::LogManager                   log;
    ModelStack                                  stack;
    GnssProcessor::ClockRefMap                  clock_refs;   ///< The bias product's clock-reference pairs; empty.
    std::unique_ptr<GnssProcessor::StationRowInputs> inputs;
    GnssProcessor::StationParams                set;

    /// A station over a GPS and Galileo network posture with the given extra members.
    explicit BirthFixture(const std::string& network_extra = "", const std::string& estimation_extra = "")
        : BirthFixture(options(network_extra, estimation_extra)) {}

    /// A station over the configuration the options describe.
    explicit BirthFixture(const BirthOptions& o,
                          const std::map<std::string, int>& glonass_slots = {},
                          std::map<std::string, std::set<std::string>> allowlist = {}) {
        const std::string text = std::string(R"({
            "processing": {"mode": "ppp-rtk-network", "active_systems": )") + o.systems
            + R"(, "signal_priorities": )" + o.priorities + R"(},
            "estimation": {"estimate_ifb": )" + (o.estimate_ifb ? "true" : "false") + o.estimation_extra
            + R"(},
            "network": {"clock_datum_station": "AAAA",
                        "stations": [{"name": "AAAA", "position_ecef": [6378137.0, 0.0, 0.0]},
                                     {"name": "BBBB", "position_ecef": [0.0, 6378137.0, 0.0]}])"
            + o.network_extra + R"(}
        })";
        EXPECT_TRUE(stack.loadText(log, "births", text));
        if (!glonass_slots.empty()) stack.ppp->setGlonassSlots(glonass_slots);
        inputs = std::make_unique<GnssProcessor::StationRowInputs>(stack.config, *stack.ppp, log,
                                                                   glonass_slots, std::move(allowlist));
        inputs->clock_refs = &clock_refs;
        set = GnssProcessor::startStationParams(stack.config, Eigen::Vector3d(6378137.0, 0.0, 0.0));
    }

    /// The station's references.
    GnssProcessor::StationRowContext context() {
        return GnssProcessor::StationRowContext(stack.config, *stack.ppp, *inputs);
    }

    /// One epoch of observations: GPS satellites with L1C and L2W, one Galileo satellite.
    std::vector<GnssProcessor::PppObservation> epoch(int k, bool with_g12 = true) const {
        using GnssProcessor::System;
        std::vector<GnssProcessor::PppObservation> obs;
        obs.push_back(syntheticObservation("G05", System::GPS, {"C1C", "L1C", "C2W", "L2W"}, k, 40.0, 50.0));
        if (with_g12)
            obs.push_back(syntheticObservation("G12", System::GPS, {"C1C", "L1C", "C2W", "L2W"}, k, 130.0, 35.0));
        obs.push_back(syntheticObservation("E24", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, k, 220.0, 25.0));
        return obs;
    }

private:
    static BirthOptions options(const std::string& network_extra, const std::string& estimation_extra) {
        BirthOptions o;
        o.network_extra    = network_extra;
        o.estimation_extra = estimation_extra;
        return o;
    }
};

}  // namespace gnss_test

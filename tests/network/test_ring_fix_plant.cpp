/**
 * @file test_ring_fix_plant.cpp
 * @brief The simulation-only planted wrong hold (network.inject kind ring_fix_plant): the hold stage that applies it
 *        and the loader's refusals.
 *
 * RingFixPlant: a staged plant shifts both bands' held integers of the first held link of its station and satellite
 * once, writes one INJECT row and closes its ledger entry; a link not held leaves it staged until it is held; a plant
 * of an unknown station never fires; no staged plant and no ring fix leave everything as it was.  The loader refuses
 * the kind without the ring fix's truth (simulation only) and without a non-zero integer delta.
 */
#include "network/ar/RingFix.hpp"
#include "network/ar/RingFixHold.hpp"
#include "network/carry/Inject.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "math/TimeUtils.hpp"
#include "../support/NetworkTestRun.hpp"
#include "../support/TestTempDir.hpp"
#include "ConfigLoadCapture.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

const RingFixLinkId kLink{0, "G05", "L1C", "L2W"};
const RingFixLinkId kOther{1, "G05", "L1C", "L2W"};

/// Two stations, a held link of G05 at each, an armed injector with one ring_fix_plant event on the first station.
struct PlantRun {
    TestSupport::LoadedConfig        loaded;
    fs::path                         trace_dir;
    std::unique_ptr<NetworkTestRun>  fx;

    PlantRun(const std::string& tag, const std::string& station, long long delta)
        : loaded(TestSupport::loadNetworkConfigCapturing("rfpl_" + tag, "    \"trace_events\": true,\n")) {
        EXPECT_TRUE(loaded.ok) << loaded.text;
        const auto log = std::make_shared<spdlog::logger>("rfpl_" + tag, std::make_shared<spdlog::sinks::null_sink_mt>());
        trace_dir = gnss_test::tempDir() / ("rfpl_" + tag);
        fs::create_directories(trace_dir);
        fx = std::make_unique<NetworkTestRun>(loaded.config, log, trace_dir);
        fx->traces().openAll(loaded.config, loaded.config.getNetworkConfig(), loaded.config.getGlobalVtecShConfig(),
                             /*sh_model=*/nullptr, /*cl_trace_armed=*/false, log);
        fx->setup().stations.resize(2);
        fx->setup().stations[0].name = "AAAA00SIM";
        fx->setup().stations[1].name = "BBBB00SIM";
        fx->newEpoch(GnssTime{2313, 259230.0});
        fx->ctx().carry.ring_fix = std::make_unique<NetworkRingFix>();
        for (const RingFixLinkId& id : {kLink, kOther}) {
            RingFixLink& L = rf().links[id];
            L.held = true;
            L.n1 = 10;
            L.n2 = 7;
            L.ring = 2;
        }
        NetworkInjectState& inj = fx->ctx().inject;
        NetworkInjectEvent ev;
        ev.kind = "ring_fix_plant";
        ev.tow = 259200.0;
        ev.station = station;
        ev.sat = "G05";
        ev.delta_cycles = static_cast<double>(delta);
        inj.armed = true;
        inj.events = {ev};
        inj.n_applied = {0};
        inj.onset_logged = {false};
        inj.release_logged = {false};
        stage();
    }
    ~PlantRun() {
        fx->traces().closeAll();
        std::error_code ec;
        fs::remove_all(trace_dir, ec);
    }
    NetworkRingFix& rf() { return *fx->ctx().carry.ring_fix; }
    NetworkInjectState& inj() { return fx->ctx().inject; }
    /// Stage the plant as the injector does at the top of every epoch until it fires.
    void stage() {
        inj().ring_fix_plant_now.clear();
        const NetworkInjectEvent& e = inj().events[0];
        if (!inj().onset_logged[0])
            inj().ring_fix_plant_now.push_back(InjectRingFixPlant{e.station, e.sat, std::llround(e.delta_cycles), 0});
    }
    /// The INJECT ring_fix_plant rows of the event trace.
    int injectRows() {
        fx->traces().closeAll();
        std::ifstream f(trace_dir / "network_events.csv");
        int n = 0;
        for (std::string line; std::getline(f, line);)
            n += (line.find("INJECT") != std::string::npos && line.find("ring_fix_plant") != std::string::npos) ? 1 : 0;
        return n;
    }
};

}  // namespace

// A staged plant moves both bands of its own link once, closes its ledger entry and writes one row; the other
// station's link of the same satellite is untouched; a second epoch with the entry closed stages nothing.
TEST(RingFixPlant, ShiftsBothBandsOfTheHeldLinkOnce) {
    PlantRun r("once", "AAAA00SIM", 1);
    networkRingFixHoldPlant(r.fx->run(), r.fx->ep());
    EXPECT_TRUE(r.rf().links.at(kLink).held);
    EXPECT_EQ(r.rf().links.at(kLink).n1, 11);
    EXPECT_EQ(r.rf().links.at(kLink).n2, 8);
    EXPECT_EQ(r.rf().links.at(kOther).n1, 10);
    EXPECT_EQ(r.rf().links.at(kOther).n2, 7);
    EXPECT_EQ(r.inj().n_applied[0], 1);
    EXPECT_TRUE(r.inj().onset_logged[0]);
    EXPECT_EQ(r.inj().fired_by_kind["ring_fix_plant"], 1);
    r.stage();
    EXPECT_TRUE(r.inj().ring_fix_plant_now.empty());
    networkRingFixHoldPlant(r.fx->run(), r.fx->ep());
    EXPECT_EQ(r.rf().links.at(kLink).n1, 11);
    EXPECT_EQ(r.rf().links.at(kLink).n2, 8);
    EXPECT_EQ(r.injectRows(), 1);
}

// A negative delta moves both bands down.
TEST(RingFixPlant, NegativeDeltaMovesBothBandsDown) {
    PlantRun r("neg", "BBBB00SIM", -2);
    networkRingFixHoldPlant(r.fx->run(), r.fx->ep());
    EXPECT_EQ(r.rf().links.at(kOther).n1, 8);
    EXPECT_EQ(r.rf().links.at(kOther).n2, 5);
    EXPECT_EQ(r.rf().links.at(kLink).n1, 10);
}

// A link not held leaves the plant staged and unfired; it fires at the first epoch the link is held.
TEST(RingFixPlant, WaitsUntilTheLinkIsHeld) {
    PlantRun r("wait", "AAAA00SIM", 1);
    r.rf().links.at(kLink).held = false;
    networkRingFixHoldPlant(r.fx->run(), r.fx->ep());
    EXPECT_EQ(r.rf().links.at(kLink).n1, 10);
    EXPECT_EQ(r.inj().n_applied[0], 0);
    EXPECT_FALSE(r.inj().onset_logged[0]);
    r.stage();
    ASSERT_EQ(r.inj().ring_fix_plant_now.size(), 1u);
    r.rf().links.at(kLink).held = true;
    networkRingFixHoldPlant(r.fx->run(), r.fx->ep());
    EXPECT_EQ(r.rf().links.at(kLink).n1, 11);
    EXPECT_EQ(r.inj().n_applied[0], 1);
}

// A station outside the roster never fires (the injector's run end then fails the run by name).
TEST(RingFixPlant, UnknownStationNeverFires) {
    PlantRun r("unknown", "ZZZZ00SIM", 1);
    networkRingFixHoldPlant(r.fx->run(), r.fx->ep());
    EXPECT_EQ(r.inj().n_applied[0], 0);
    EXPECT_EQ(r.rf().links.at(kLink).n1, 10);
    EXPECT_EQ(r.rf().links.at(kOther).n1, 10);
    EXPECT_EQ(r.injectRows(), 0);
}

// No staged plant, or no ring fix: nothing is read or written.
TEST(RingFixPlant, NoPlantOrNoRingFixIsANoOp) {
    PlantRun r("noop", "AAAA00SIM", 1);
    r.inj().ring_fix_plant_now.clear();
    networkRingFixHoldPlant(r.fx->run(), r.fx->ep());
    EXPECT_EQ(r.rf().links.at(kLink).n1, 10);
    EXPECT_EQ(r.inj().n_applied[0], 0);
    r.stage();
    r.fx->ctx().carry.ring_fix.reset();
    networkRingFixHoldPlant(r.fx->run(), r.fx->ep());
    EXPECT_EQ(r.inj().n_applied[0], 0);
    EXPECT_FALSE(r.inj().onset_logged[0]);
}

// The loader: the kind needs the ring fix's truth (simulation only) and a non-zero integer delta.  The accepting path
// needs the whole ring-fix posture (network.phase_datum_eq34 and its prerequisites), which no unit config carries; it
// is shown by the planted runs of the package held-resume-release (plant/), whose two events load and fire.
TEST(RingFixPlant, LoaderRefusesWithoutTruthOrIntegerDelta) {
    const std::string ev = "    \"inject\": { \"events\": [ { \"kind\": \"ring_fix_plant\", \"tow\": 259230, "
                           "\"station\": \"AAAA\", \"sat\": \"G05\", \"delta_cycles\": DELTA } ] },\n";
    auto with = [&](const std::string& delta) {
        std::string s = ev;
        s.replace(s.find("DELTA"), 5, delta);
        return s;
    };
    const TestSupport::LoadedConfig no_truth = TestSupport::loadNetworkConfigCapturing("rfpl_nt", with("1"));
    EXPECT_FALSE(no_truth.ok);
    EXPECT_NE(no_truth.text.find("simulation only"), std::string::npos) << no_truth.text;
    const TestSupport::LoadedConfig frac = TestSupport::loadNetworkConfigCapturing("rfpl_fr", with("0.5"));
    EXPECT_FALSE(frac.ok);
    EXPECT_NE(frac.text.find("non-zero integer delta_cycles"), std::string::npos) << frac.text;
    const TestSupport::LoadedConfig zero = TestSupport::loadNetworkConfigCapturing("rfpl_z", with("0"));
    EXPECT_FALSE(zero.ok);
    EXPECT_NE(zero.text.find("non-zero integer delta_cycles"), std::string::npos) << zero.text;
}

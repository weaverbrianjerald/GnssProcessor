/**
 * @file test_ring_fix_hold_stages.cpp
 * @brief Today's behaviour of three hold stages of the ring fix, pinned before line A changes their response (review
 *        finding F2), and the gate of the stages line A touches (finding F6).
 *
 * RingFixHoldStages: (a) a cut release across an unknown step on a latched held link; (b) a cut entry refused for a
 * shifted gap resumption under network.ring_fix.entry_revalidate; (c) a refloat release.  The carried entries sit at
 * the release floor's variance or above and re-level between the link's two stamps, so each entry is re-opened
 * against its own band's stamp (a stamp-1-for-both mutant re-opens the wrong one).
 * RingFixStageGates: each touched stage returns at once on a run without the ring fix.
 */
#include "network/ar/RingFix.hpp"
#include "network/ar/RingFixDecide.hpp"
#include "network/ar/RingFixHold.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"
#include "network/driver/Run.hpp"
#include "network/driver/RunGroups.hpp"
#include "network/slip/RawObservables.hpp"
#include "network/solve/ChainFrame.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "common/GnssIdentity.hpp"
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
#include <sstream>
#include <string>
#include <vector>

using namespace GnssProcessor;
namespace fs = std::filesystem;

namespace {

constexpr double kL1 = 0.19029367279836487;   // GPS L1 (m)
constexpr double kL2 = 0.24421021342456825;   // GPS L2 (m)
const RingFixLinkId kLink{0, "G05", "L1C", "L2W"};

/// One station, a held link with distinct stamps, both bands' carried entries re-levelled between them, and the
/// event trace open.
struct HoldStageRun {
    TestSupport::LoadedConfig        loaded;
    fs::path                         trace_dir;
    std::unique_ptr<NetworkTestRun>  fx;
    double                           now_s{0.0};

    explicit HoldStageRun(const std::string& tag)
        : loaded(TestSupport::loadNetworkConfigCapturing("rfhs_" + tag, "    \"trace_events\": true,\n")) {
        EXPECT_TRUE(loaded.ok) << loaded.text;
        const auto log = std::make_shared<spdlog::logger>("rfhs_" + tag, std::make_shared<spdlog::sinks::null_sink_mt>());
        trace_dir = gnss_test::tempDir() / ("rfhs_" + tag);
        fs::create_directories(trace_dir);
        fx = std::make_unique<NetworkTestRun>(loaded.config, log, trace_dir);
        fx->traces().openAll(loaded.config, loaded.config.getNetworkConfig(), loaded.config.getGlobalVtecShConfig(),
                             /*sh_model=*/nullptr, /*cl_trace_armed=*/false, log);
        fx->setup().stations.resize(2);
        fx->setup().stations[0].name = "AAAA00SIM";
        fx->setup().stations[1].name = "BBBB00SIM";
        NetworkEpoch& e = fx->newEpoch(GnssTime{2313, 259230.0});
        now_s = TimeUtils::continuousGpsSeconds(e.se.time);
        e.clock = EpochClock{30.0, now_s, true};
        fx->ctx().carry.ring_fix = std::make_unique<NetworkRingFix>();
        RingFixLink& L = rf().links[kLink];
        L.lambda1 = kL1;
        L.lambda2 = kL2;
        L.held = true;
        L.n1 = 10;
        L.n2 = 7;
        L.ring = 2;
        L.decided_s = now_s - 3000.0;
        L.stamp1 = now_s - 2000.0;   // band 1's carry re-levelled after its stamp: not stale
        L.stamp2 = now_s - 500.0;    // band 2's carry re-levelled before its stamp: stale
        for (const char* sig : {"L1C", "L2W"}) {
            LocalCarry lc;
            lc.value_abs = 1.0;
            lc.var = 2.0 * kRingFixReleaseFloorM2;   // at the floor or above: only a stale entry is re-opened
            lc.var_first = lc.var;
            lc.var_birth = lc.var;
            lc.last_seen_s = now_s - 30.0;
            lc.relevel_s = now_s - 1000.0;           // between the two stamps
            net::put(fx->ctx().entities.arcs, arcId(sig), &net::ArcState::amb) = lc;
        }
    }
    ~HoldStageRun() {
        fx->traces().closeAll();
        std::error_code ec;
        fs::remove_all(trace_dir, ec);
    }

    NetworkRingFix& rf() { return *fx->ctx().carry.ring_fix; }
    static ArcId arcId(const char* sig) {
        return ArcId{StationId{0}, *SatId::parse("G05"), *SignalId::fromObsCode(System::GPS, sig)};
    }
    bool carried(const char* sig) {
        return net::getArc(fx->ctx().entities.arcs, net::ArcTuple{0, "G05", sig}, &net::ArcState::amb) != nullptr;
    }
    /// The RINGFIX rows of the event trace whose text holds every one of @p parts.
    int events(const std::vector<std::string>& parts) {
        fx->traces().closeAll();
        std::ifstream f(trace_dir / "network_events.csv");
        int n = 0;
        for (std::string line; std::getline(f, line);) {
            if (line.find("RINGFIX") == std::string::npos) continue;
            bool all = true;
            for (const std::string& p : parts) all = all && line.find(p) != std::string::npos;
            n += all ? 1 : 0;
        }
        return n;
    }
};

}  // namespace

// (a) A cut across an unknown step releases a latched held link: not usable, one release row with an unknown step,
//     each band's entry re-opened against its own stamp.
TEST(RingFixHoldStages, ACutAcrossAnUnknownStepReleasesALatchedHeldLink) {
    HoldStageRun r("cut");
    RingFixCut cut;
    cut.sys = "G";
    cut.signal = "L1C";
    cut.stations = {0};     // the link's receiver on the moved side, its satellite not
    cut.known = false;
    cut.row = "rehost";
    cut.entry_station = 1;
    cut.entry_sat = "G07";
    const std::set<net::ArcTuple> latched{{0, "G05", "L1C"}};
    networkRingFixHoldThroughCuts(r.fx->run(), r.fx->ep(), latched, {cut});
    const RingFixCounters& C = r.rf().counters;
    EXPECT_FALSE(r.rf().links.at(kLink).held);
    EXPECT_EQ(C.cut_released, 1);
    EXPECT_EQ(C.released[static_cast<std::size_t>(RingFixRelease::Chain)], 1);
    EXPECT_EQ(C.cut_released_usable, 0);   // latched: not usable before the cut
    EXPECT_EQ(C.carry_reopened, 1);        // band 2 only
    EXPECT_TRUE(r.carried("L1C"));
    EXPECT_FALSE(r.carried("L2W"));
    EXPECT_EQ(r.events({",release,", "step=unknown", "cut=rehost"}), 1);
}

// (b) A cut whose entry link is held is refused under the entry check when the gap re-entry test found its pair shifted.
TEST(RingFixHoldStages, AShiftedGapResumptionRefusesTheEntryUnderTheEntryCheck) {
    HoldStageRun r("entry");
    r.rf().entry_revalidate = true;
    auto rec = std::make_shared<ChainEpochObs>();
    rec->armed = true;
    ChainReleaseObs rel;
    rel.row = ChainReleaseRow::Rehost;
    rel.group = "G|L1C";
    rel.signal = "L1C";
    rel.station = 0;
    rel.sat = "G05";
    rel.sat_is_child = true;
    rel.vertex = "S:G05";
    rel.sats = {"G05"};
    rec->releases.push_back(rel);
    r.fx->ep().events.phase_datum.chain = rec;
    GapResumption g;
    g.station = 0;
    g.satellite = "G05";
    g.sig1 = "L1C";
    g.sig2 = "L2W";
    g.gap_epochs = 6;
    g.measurable = true;
    g.shifted = true;
    g.d_gf = -1.02;
    r.fx->run().engines.slip_qc.epoch.resumptions.push_back(g);
    const std::vector<RingFixCut> cuts = networkRingFixHoldCuts(r.fx->run(), r.fx->ep(), {});
    ASSERT_EQ(cuts.size(), 1u);
    EXPECT_FALSE(cuts[0].known);
    const RingFixCounters& C = r.rf().counters;
    EXPECT_EQ(C.entry_checked, 1);
    EXPECT_EQ(C.entry_known, 0);
    EXPECT_EQ(C.entry_refused_shifted, 1);
    EXPECT_EQ(C.entry_refused_unchecked, 0);
    EXPECT_EQ(r.events({",entry_refused,", "gap_shifted"}), 1);
}

// (c) A present held link whose carry was re-levelled since the decision is released as a refloat, each band's entry
//     re-opened against its own stamp.
TEST(RingFixHoldStages, ARefloatReleasesAPresentHeldLinkReLevelledSinceItsDecision) {
    HoldStageRun r("refloat");
    RingFixChainFacts facts;
    facts.chords = {{0, "G05", "L1C"}, {0, "G05", "L2W"}};
    networkRingFixHoldReleases(r.fx->run(), r.fx->ep(), facts, {});
    const RingFixCounters& C = r.rf().counters;
    EXPECT_FALSE(r.rf().links.at(kLink).held);
    EXPECT_EQ(C.released[static_cast<std::size_t>(RingFixRelease::Refloat)], 1);
    EXPECT_EQ(C.carry_reopened, 1);
    EXPECT_TRUE(r.carried("L1C"));
    EXPECT_FALSE(r.carried("L2W"));
    EXPECT_EQ(r.events({",release,", ",refloat,"}), 1);
}

// ---- The resume step inside the release stage (network.ring_fix.resume_response) ----

// A release at the resumption acts on that epoch's solve: the stage releases the link before PASS B, so the hold table
// the produce reads has no row for it.  Off, the same epoch keeps today's cause (refloat, case (c) above).
TEST(RingFixResumeStage, AReleaseAtTheResumptionLeavesNoHoldRowInThatEpoch) {
    HoldStageRun r("resume");
    r.rf().resume_response = RingFixResumeResponse::Release;
    GapResumption g;
    g.station = 0;
    g.satellite = "G05";
    g.sig1 = "L1C";
    g.sig2 = "L2W";
    g.gap_epochs = 15;
    g.measurable = true;
    g.shifted = true;
    g.d_gf = -1.0204;
    g.lli1 = g.lli2 = 1;
    r.fx->run().engines.slip_qc.epoch.resumptions.push_back(g);
    RingFixChainFacts facts;
    facts.chords = {{0, "G05", "L1C"}, {0, "G05", "L2W"}};
    networkRingFixHoldReleases(r.fx->run(), r.fx->ep(), facts, {});
    networkRingFixHoldTable(r.fx->run());
    const RingFixCounters& C = r.rf().counters;
    EXPECT_FALSE(r.rf().links.at(kLink).held);
    EXPECT_EQ(C.released[static_cast<std::size_t>(RingFixRelease::Resume)], 1);
    EXPECT_EQ(C.released[static_cast<std::size_t>(RingFixRelease::Refloat)], 0);   // the resume step came first
    EXPECT_EQ(C.resume_released[static_cast<std::size_t>(RingFixResumeClass::Shifted)], 1);
    EXPECT_EQ(r.rf().table.count({0, "G05", "L1C"}), 0u);
    EXPECT_EQ(r.rf().table.count({0, "G05", "L2W"}), 0u);
    EXPECT_EQ(C.carry_reopened, 1);   // each entry against its own stamp
    EXPECT_EQ(r.events({",release,", ",resume,", "class=shifted", "carry=bridged", "lli=1"}), 1);
}

// ---- The gate of the touched stages ----

TEST(RingFixStageGates, TheTouchedStagesReturnAtOnceWithoutTheRingFix) {
    NetworkTestRun fx;
    ASSERT_FALSE(fx.ctx().carry.ring_fix);
    NetworkEpoch& e = fx.newEpoch(GnssTime{2313, 259230.0});
    EXPECT_TRUE(networkRingFixHoldCuts(fx.run(), e, {}).empty());
    networkRingFixHoldThroughCuts(fx.run(), e, {}, {});
    networkRingFixHoldReleases(fx.run(), e, RingFixChainFacts{}, {});
    EXPECT_TRUE(networkRingFixDecideLinks(fx.run(), e, RingFixDecideReadings{}).empty());
    EXPECT_FALSE(fx.ctx().carry.ring_fix);
}

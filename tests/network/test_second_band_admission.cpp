/**
 * @file test_second_band_admission.cpp
 * @brief The second-band admission: a constellation other than the time reference enters
 *        the network path at a station only while the station holds it on two modelled
 *        bands.
 *
 * The defect the rule answers: a station that tracks BeiDou on B1I alone (no B3I) leaves the
 * inter-system bias, the slant ionosphere and the ambiguities of its BeiDou lines of sight
 * collinear, the addStation positive-definiteness guard rejects the whole station at every
 * epoch, and the station's GPS and Galileo rows leave the solve with it. The cases below pin
 * each decision of the rule that has no other observable:
 *
 *   1. the candidate table and the band count (pure): physical bands, code and phase both
 *      required, two signals of one carrier counted once, non-candidate signals ignored;
 *   2. the unit state machine (pure): both edges where they are crossed and none where they
 *      are not;
 *   3. the seam, station scope: the single-band station loses BeiDou and keeps GPS; a
 *      single-band satellite among dual-band peers is kept; the threshold; the re-admission
 *      when the second band appears; the memory across an absent constellation;
 *   4. the seam, satellite scope: each satellite is its own unit;
 *   5. what is outside the rule: the time reference constellation, a constellation without a
 *      priority list, an unarmed run (structurally inert: the same epoch object comes back,
 *      no counter, no entity row);
 *   6. the loader: defaults, an armed value, the scope names and the threshold clamp.
 *
 * Structural only: the rule's effect on the network is graded by the arms its card records.
 *
 * Suites: SecondBandPure, SecondBandSeam, SecondBandConfig (one file owns all three).
 */
#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "common/SignalBand.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/SecondBandAdmission.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/state/RunContext.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "rinex/RinexParser.hpp"
#include "../support/NetworkTestRun.hpp"
#include "../support/TestTempDir.hpp"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// One satellite record of the given constellation carrying the listed observation codes.
RinexObservation rec(const std::string& sat, System sys,
                     std::initializer_list<const char*> codes) {
    RinexObservation o;
    o.satellite = sat;
    o.system = sys;
    for (const char* c : codes) o.measurements[c] = (c[0] == 'L') ? 1.0e5 : 2.2e7;
    return o;
}

/// A BeiDou record on one band (B1I) and one on two (B1I and B3I).
RinexObservation bdsSingle(const std::string& sat) {
    return rec(sat, System::BeiDou, {"C2I", "L2I", "C7I", "L7I"});   // B2I is not a candidate
}
RinexObservation bdsDual(const std::string& sat) {
    return rec(sat, System::BeiDou, {"C2I", "L2I", "C6I", "L6I"});
}
RinexObservation gps(const std::string& sat) {
    return rec(sat, System::GPS, {"C1C", "L1C"});   // one band: the reference constellation is exempt
}

const std::vector<std::string> kBdsPriority = {"C2I", "L2I", "C6I", "L6I", "D2I", "S2I"};

GnssTime epochAt(double tow) {
    GnssTime t;
    t.week = 2314;
    t.tow = tow;
    return t;
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("secband_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    return p;
}

/// A loadable network config with @p net_extra spliced into the network block.  GPS and
/// BeiDou are active; Galileo is active without a priority list (outside the rule).
std::string networkConfig(const std::string& net_extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\", \"Galileo\", \"BeiDou\" ],\n"
          "    \"signal_priorities\": {\n"
          "      \"GPS\": [ \"C1C\", \"L1C\", \"C2W\", \"L2W\" ],\n"
          "      \"BeiDou\": [ \"C2I\", \"L2I\", \"C6I\", \"L6I\" ]\n"
          "    }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
        + net_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

std::unique_ptr<GnssConfig> loadCfg(LogManager& log, const std::string& tag,
                                    const std::string& net_extra) {
    auto cfg = std::make_unique<GnssConfig>();
    const fs::path p = writeTempConfig(tag, networkConfig(net_extra));
    const bool ok = cfg->loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    EXPECT_TRUE(ok) << "loadFromJson failed for tag " << tag;
    return cfg;
}

std::string armed(const std::string& scope = "station", int min_sats = 1) {
    return "    \"second_band_admission\": true,\n    \"second_band_scope\": \"" + scope
         + "\",\n    \"second_band_min_sats\": " + std::to_string(min_sats) + ",\n";
}

/// A logger with no sink: the default logger can be null after another suite's LogManager has
/// been torn down, and the seam writes an info line at each edge.
std::shared_ptr<spdlog::logger> quietLogger() {
    return std::make_shared<spdlog::logger>("secband_quiet",
                                            std::make_shared<spdlog::sinks::null_sink_mt>());
}

/// The seam's run: two stations (no model, no state manager: the seam reads neither), one epoch
/// at a time.  The epoch handed to the seam is built by the test, so the original is kept to
/// show that it is never mutated.
struct Rig {
    std::unique_ptr<NetworkTestRun> fx;
    long long n = 0;

    explicit Rig(const GnssConfig& cfg) : fx(std::make_unique<NetworkTestRun>(cfg, quietLogger())) {
        auto& st = fx->setup().stations;
        st.resize(2);
        st[0].name = "AAAA";
        st[1].name = "BBBB";
    }

    /// One epoch: station AAAA holds @p a, station BBBB holds @p b.
    SyncedEpoch epoch(std::vector<RinexObservation> a, std::vector<RinexObservation> b = {}) {
        SyncedEpoch se;
        se.time = epochAt(1000.0 + 30.0 * static_cast<double>(n++));
        if (!a.empty()) se.by_station["AAAA"] = std::move(a);
        if (!b.empty()) se.by_station["BBBB"] = std::move(b);
        return se;
    }

    const SyncedEpoch& run(const SyncedEpoch& in, SyncedEpoch& scratch) {
        return networkSecondBandAdmission(fx->run(), in, scratch);
    }

    const NetworkSecondBandCounters& ctr() { return fx->ctx().counters.sba; }

    /// Station AAAA's withheld units.
    std::set<std::string> withheld(int station = 0) {
        std::set<std::string> out;
        const auto it = fx->ctx().entities.stations.find(StationId{station});
        if (it == fx->ctx().entities.stations.end()) return out;
        for (const auto& kv : it->second.sba_since) out.insert(kv.first);
        return out;
    }
};

std::set<std::string> sats(const SyncedEpoch& se, const std::string& station) {
    std::set<std::string> out;
    const auto it = se.by_station.find(station);
    if (it == se.by_station.end()) return out;
    for (const auto& o : it->second) out.insert(o.satellite);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. The candidate table and the band count
// ---------------------------------------------------------------------------
TEST(SecondBandPure, CandidatesAreTheCodeAndPhaseSignalsOfTheListWithTheirBands) {
    const SecondBandCandidates c = secondBandCandidates(System::BeiDou, kBdsPriority, nullptr);
    EXPECT_EQ(c.code.size(), 2u);    // C2I, C6I
    EXPECT_EQ(c.phase.size(), 2u);   // L2I, L6I
    EXPECT_EQ(c.code.at("C2I"), FrequencyBand::BdsB1I);
    EXPECT_EQ(c.phase.at("L2I"), FrequencyBand::BdsB1I);
    EXPECT_EQ(c.code.at("C6I"), FrequencyBand::BdsB3);
    EXPECT_EQ(c.phase.at("L6I"), FrequencyBand::BdsB3);
    EXPECT_EQ(c.code.count("D2I"), 0u) << "a Doppler entry is not a modelled signal";
    EXPECT_EQ(c.code.count("S2I"), 0u) << "a signal-strength entry is not a modelled signal";
}

TEST(SecondBandPure, TheCoverageGateRemovesWhatItExcludes) {
    const std::set<std::string> admitted = {"C2I", "L2I", "L6I"};   // C6I excluded network wide
    const SecondBandCandidates c = secondBandCandidates(System::BeiDou, kBdsPriority, &admitted);
    EXPECT_EQ(c.code.count("C6I"), 0u);
    EXPECT_EQ(c.phase.count("L6I"), 1u);
    // The band is then never modelled with both a code and a phase.
    EXPECT_EQ(secondBandCount(bdsDual("C06"), c), 1);
}

TEST(SecondBandPure, ASignalOfAnUnknownBandIsDropped) {
    const SecondBandCandidates c =
        secondBandCandidates(System::BeiDou, {"C9Z", "L9Z", "C2I", "L2I"}, nullptr);
    EXPECT_EQ(c.code.count("C9Z"), 0u);
    EXPECT_EQ(c.phase.count("L9Z"), 0u);
    EXPECT_EQ(c.code.count("C2I"), 1u);
}

TEST(SecondBandPure, ABandCountsOnlyWithBothACodeAndAPhase) {
    const SecondBandCandidates c = secondBandCandidates(System::BeiDou, kBdsPriority, nullptr);
    EXPECT_EQ(secondBandCount(bdsDual("C06"), c), 2);
    EXPECT_EQ(secondBandCount(bdsSingle("C07"), c), 1)
        << "B2I signals are not candidates, so the record is on B1I alone";
    EXPECT_EQ(secondBandCount(rec("C08", System::BeiDou, {"C2I", "L2I", "C6I"}), c), 1)
        << "a code without its phase does not make a band";
    EXPECT_EQ(secondBandCount(rec("C09", System::BeiDou, {"C2I", "L2I", "L6I"}), c), 1)
        << "a phase without its code does not make a band";
    EXPECT_EQ(secondBandCount(rec("C10", System::BeiDou, {"D2I"}), c), 0);
}

TEST(SecondBandPure, TwoSignalsOfOneCarrierCountOnce) {
    // C1I / L1I are B1I too (RINEX 3.02 spelling): listing both spellings must not make two bands.
    const SecondBandCandidates c =
        secondBandCandidates(System::BeiDou, {"C2I", "L2I", "C1I", "L1I"}, nullptr);
    EXPECT_EQ(secondBandCount(rec("C11", System::BeiDou, {"C2I", "L2I", "C1I", "L1I"}), c), 1);
}

// ---------------------------------------------------------------------------
// 2. The unit state machine
// ---------------------------------------------------------------------------
TEST(SecondBandPure, TheStepRaisesEachEdgeOnceAndNoneWhileTheStateHolds) {
    std::map<std::string, long long> since;
    // epochs:           0      1      2      3      4      5
    const bool seq[] = {true, false, false, false, true, true};
    int withhold = 0, readmit = 0;
    long long gap = -1;
    for (long long e = 0; e < 6; ++e) {
        const SecondBandStep s = secondBandStep(since, "BeiDou", seq[e], e);
        withhold += s.withhold_edge ? 1 : 0;
        readmit += s.readmit_edge ? 1 : 0;
        if (s.readmit_edge) gap = s.gap_ep;
        if (e == 2 || e == 3) EXPECT_FALSE(s.withhold_edge) << "a continuing withhold is no edge";
        if (e == 5) EXPECT_FALSE(s.readmit_edge) << "a continuing admission is no edge";
    }
    EXPECT_EQ(withhold, 1);
    EXPECT_EQ(readmit, 1);
    EXPECT_EQ(gap, 3) << "withheld at epochs 1 to 3, back at 4";
    EXPECT_TRUE(since.empty()) << "the memory holds a unit exactly while it is withheld";
}

TEST(SecondBandPure, AUnitWithheldFromTheFirstEpochRaisesTheEdgeThere) {
    std::map<std::string, long long> since;
    const SecondBandStep s = secondBandStep(since, "BeiDou", false, 0);
    EXPECT_TRUE(s.withhold_edge);
    EXPECT_EQ(since.at("BeiDou"), 0);
}

// ---------------------------------------------------------------------------
// 3. The seam, station scope
// ---------------------------------------------------------------------------
TEST(SecondBandSeam, TheSingleBandStationLosesBeiDouAndKeepsItsOtherConstellations) {
    LogManager lm;
    auto cfg = loadCfg(lm, "station", armed());
    Rig r(*cfg);
    const SyncedEpoch in = r.epoch({gps("G01"), gps("G02"), bdsSingle("C06"), bdsSingle("C07")},
                                   {gps("G01"), bdsDual("C06"), bdsDual("C07")});
    SyncedEpoch scratch;
    const SyncedEpoch& out = r.run(in, scratch);

    EXPECT_EQ(&out, &scratch);
    EXPECT_EQ(sats(out, "AAAA"), (std::set<std::string>{"G01", "G02"}))
        << "BeiDou leaves the single-band station; GPS stays";
    EXPECT_EQ(sats(out, "BBBB"), (std::set<std::string>{"G01", "C06", "C07"}))
        << "the dual-band station is untouched";
    EXPECT_EQ(sats(in, "AAAA").size(), 4u) << "the caller's epoch is never mutated";

    const auto& c = r.ctr();
    EXPECT_EQ(c.epochs, 1);
    EXPECT_EQ(c.records_screened, 4) << "four BeiDou records, GPS is not screened";
    EXPECT_EQ(c.units_screened, 2) << "one unit per station and constellation";
    EXPECT_EQ(c.units_withheld, 1);
    EXPECT_EQ(c.records_withheld, 2);
    EXPECT_EQ(c.withhold_edges, 1);
    EXPECT_EQ(c.readmit_edges, 0);
    EXPECT_EQ(c.epochs_copied, 1);
    EXPECT_EQ(c.affected.at("BeiDou"), (std::set<std::string>{"AAAA"}));
    EXPECT_EQ(r.withheld(0), (std::set<std::string>{"BeiDou"}));
    EXPECT_TRUE(r.withheld(1).empty());
}

TEST(SecondBandSeam, ASingleBandSatelliteAmongDualBandPeersIsKept) {
    LogManager lm;
    auto cfg = loadCfg(lm, "peers", armed());
    Rig r(*cfg);
    const SyncedEpoch in = r.epoch({gps("G01"), bdsDual("C06"), bdsSingle("C07"), bdsSingle("C08")});
    SyncedEpoch scratch;
    const SyncedEpoch& out = r.run(in, scratch);
    EXPECT_EQ(&out, &in) << "nothing withheld: no copy is taken";
    EXPECT_EQ(r.ctr().units_withheld, 0);
    EXPECT_EQ(r.ctr().epochs_copied, 0);
    EXPECT_TRUE(r.withheld(0).empty());
    EXPECT_EQ(r.fx->ctx().entities.stations.count(StationId{0}), 0u)
        << "a station that never has a withheld unit owns no row";
}

TEST(SecondBandSeam, TheThresholdCountsDualBandSatellites) {
    LogManager lm;
    auto cfg = loadCfg(lm, "min2", armed("station", 2));
    Rig r(*cfg);
    SyncedEpoch scratch;
    {
        const SyncedEpoch in = r.epoch({bdsDual("C06"), bdsSingle("C07"), bdsSingle("C08")});
        const SyncedEpoch& out = r.run(in, scratch);
        EXPECT_TRUE(sats(out, "AAAA").empty()) << "one dual-band satellite is below a threshold of 2";
        EXPECT_EQ(r.ctr().withhold_edges, 1);
    }
    {
        const SyncedEpoch in = r.epoch({bdsDual("C06"), bdsDual("C07"), bdsSingle("C08")});
        const SyncedEpoch& out = r.run(in, scratch);
        EXPECT_EQ(sats(out, "AAAA").size(), 3u) << "two dual-band satellites meet it";
        EXPECT_EQ(r.ctr().readmit_edges, 1);
        EXPECT_EQ(r.ctr().max_gap_ep, 1);
    }
}

TEST(SecondBandSeam, TheConstellationReturnsWhenASecondBandAppears) {
    LogManager lm;
    auto cfg = loadCfg(lm, "return", armed());
    Rig r(*cfg);
    SyncedEpoch scratch;
    for (int k = 0; k < 3; ++k) {
        const SyncedEpoch in = r.epoch({gps("G01"), bdsSingle("C06")});
        const SyncedEpoch& out = r.run(in, scratch);
        EXPECT_EQ(sats(out, "AAAA"), (std::set<std::string>{"G01"}));
    }
    EXPECT_EQ(r.ctr().withhold_edges, 1) << "three withheld epochs raise one decoupling edge";
    EXPECT_EQ(r.ctr().units_withheld, 3);
    EXPECT_EQ(r.ctr().readmit_edges, 0);

    const SyncedEpoch in = r.epoch({gps("G01"), bdsDual("C06")});
    const SyncedEpoch& out = r.run(in, scratch);
    EXPECT_EQ(&out, &in) << "admitted: the epoch comes back as it was";
    EXPECT_EQ(sats(out, "AAAA"), (std::set<std::string>{"G01", "C06"}));
    EXPECT_EQ(r.ctr().readmit_edges, 1);
    EXPECT_EQ(r.ctr().max_gap_ep, 3);
    EXPECT_TRUE(r.withheld(0).empty()) << "the memory is erased at the re-coupling edge";
}

TEST(SecondBandSeam, AConstellationAbsentForAWhileKeepsItsWithheldMemory) {
    LogManager lm;
    auto cfg = loadCfg(lm, "absent", armed());
    Rig r(*cfg);
    SyncedEpoch scratch;
    { const SyncedEpoch in = r.epoch({gps("G01"), bdsSingle("C06")}); r.run(in, scratch); }
    { const SyncedEpoch in = r.epoch({gps("G01")}); r.run(in, scratch); }   // BeiDou not tracked
    { const SyncedEpoch in = r.epoch({gps("G01")}); r.run(in, scratch); }
    EXPECT_EQ(r.ctr().withhold_edges, 1);
    EXPECT_EQ(r.ctr().readmit_edges, 0) << "an absent constellation is not a re-admission";
    { const SyncedEpoch in = r.epoch({gps("G01"), bdsSingle("C06")}); r.run(in, scratch); }
    EXPECT_EQ(r.ctr().withhold_edges, 1) << "returning on one band is still withheld, not a new edge";
    { const SyncedEpoch in = r.epoch({gps("G01"), bdsDual("C06")}); r.run(in, scratch); }
    EXPECT_EQ(r.ctr().readmit_edges, 1);
    EXPECT_EQ(r.ctr().max_gap_ep, 4);
}

// ---------------------------------------------------------------------------
// 4. The seam, satellite scope
// ---------------------------------------------------------------------------
TEST(SecondBandSeam, UnderSatelliteScopeEachSatelliteIsItsOwnUnit) {
    LogManager lm;
    auto cfg = loadCfg(lm, "sat", armed("satellite"));
    Rig r(*cfg);
    SyncedEpoch scratch;
    const SyncedEpoch in = r.epoch({gps("G01"), bdsDual("C06"), bdsSingle("C07"), bdsSingle("C08")});
    const SyncedEpoch& out = r.run(in, scratch);
    EXPECT_EQ(sats(out, "AAAA"), (std::set<std::string>{"G01", "C06"}));
    EXPECT_EQ(r.ctr().units_screened, 3);
    EXPECT_EQ(r.ctr().units_withheld, 2);
    EXPECT_EQ(r.ctr().records_withheld, 2);
    EXPECT_EQ(r.ctr().withhold_edges, 2);
    EXPECT_EQ(r.withheld(0), (std::set<std::string>{"C07", "C08"}));

    // C07 gains its second band; C08 stays on one.
    const SyncedEpoch in2 = r.epoch({gps("G01"), bdsDual("C06"), bdsDual("C07"), bdsSingle("C08")});
    const SyncedEpoch& out2 = r.run(in2, scratch);
    EXPECT_EQ(sats(out2, "AAAA"), (std::set<std::string>{"G01", "C06", "C07"}));
    EXPECT_EQ(r.ctr().readmit_edges, 1);
    EXPECT_EQ(r.ctr().withhold_edges, 2) << "C08 continues withheld, no second edge";
    EXPECT_EQ(r.withheld(0), (std::set<std::string>{"C08"}));
}

// ---------------------------------------------------------------------------
// 5. What the rule does not touch
// ---------------------------------------------------------------------------
TEST(SecondBandSeam, TheTimeReferenceAndAConstellationWithoutAListAreOutsideTheRule) {
    LogManager lm;
    auto cfg = loadCfg(lm, "outside", armed());
    Rig r(*cfg);
    SyncedEpoch scratch;
    // Single-band GPS (the reference) and Galileo (active, no priority list).
    const SyncedEpoch in = r.epoch({gps("G01"), gps("G02"), rec("E11", System::Galileo, {"C1C", "L1C"})});
    const SyncedEpoch& out = r.run(in, scratch);
    EXPECT_EQ(&out, &in);
    EXPECT_EQ(r.ctr().records_screened, 0);
    EXPECT_EQ(r.ctr().units_withheld, 0);
}

TEST(SecondBandSeam, ARecordOnNoModelledBandIsLeftAloneAndCountedApart) {
    // B2I only (C7I / L7I): the priority list names neither, so the producer forms no row from
    // the record whatever the rule does.  The station that spells B1I as C1I / L1I is this case.
    LogManager lm;
    auto cfg = loadCfg(lm, "unmodelled", armed());
    Rig r(*cfg);
    SyncedEpoch scratch;
    const SyncedEpoch in = r.epoch({gps("G01"), rec("C06", System::BeiDou, {"C7I", "L7I"}),
                                    rec("C07", System::BeiDou, {"C1I", "L1I"})});
    const SyncedEpoch& out = r.run(in, scratch);
    EXPECT_EQ(&out, &in);
    EXPECT_EQ(r.ctr().records_unmodelled, 2);
    EXPECT_EQ(r.ctr().records_screened, 0);
    EXPECT_EQ(r.ctr().units_screened, 0);
    EXPECT_EQ(r.ctr().withhold_edges, 0);
    EXPECT_TRUE(r.fx->ctx().entities.stations.empty());
}

TEST(SecondBandSeam, AnEpochWithNoModelledBandDoesNotReadmitAWithheldConstellation) {
    LogManager lm;
    auto cfg = loadCfg(lm, "unmodelled_hold", armed());
    Rig r(*cfg);
    SyncedEpoch scratch;
    { const SyncedEpoch in = r.epoch({gps("G01"), bdsSingle("C06")}); r.run(in, scratch); }
    EXPECT_EQ(r.ctr().withhold_edges, 1);
    {
        const SyncedEpoch in = r.epoch({gps("G01"), rec("C06", System::BeiDou, {"C7I", "L7I"})});
        const SyncedEpoch& out = r.run(in, scratch);
        EXPECT_EQ(&out, &in);
        EXPECT_EQ(r.ctr().readmit_edges, 0) << "no modelled band is no evidence of a second one";
        EXPECT_EQ(r.withheld(0), (std::set<std::string>{"BeiDou"}));
    }
}

TEST(SecondBandSeam, UnderSatelliteScopeARecordOnNoModelledBandIsLeftAlone) {
    LogManager lm;
    auto cfg = loadCfg(lm, "unmodelled_sat", armed("satellite"));
    Rig r(*cfg);
    SyncedEpoch scratch;
    const SyncedEpoch in = r.epoch({gps("G01"), rec("C06", System::BeiDou, {"C7I", "L7I"}),
                                    bdsSingle("C07")});
    const SyncedEpoch& out = r.run(in, scratch);
    EXPECT_EQ(sats(out, "AAAA"), (std::set<std::string>{"G01", "C06"}))
        << "only the one-band satellite is withheld";
    EXPECT_EQ(r.ctr().records_unmodelled, 1);
    EXPECT_EQ(r.ctr().units_screened, 1);
}

TEST(SecondBandSeam, AStationWithNoBeiDouRecordsRaisesNothing) {
    LogManager lm;
    auto cfg = loadCfg(lm, "none", armed());
    Rig r(*cfg);
    SyncedEpoch scratch;
    const SyncedEpoch in = r.epoch({gps("G01")}, {gps("G01"), bdsDual("C06")});
    const SyncedEpoch& out = r.run(in, scratch);
    EXPECT_EQ(&out, &in);
    EXPECT_EQ(r.ctr().units_screened, 1);
    EXPECT_EQ(r.ctr().withhold_edges, 0);
}

TEST(SecondBandSeam, UnarmedTheSeamIsStructurallyInert) {
    LogManager lm;
    auto cfg = loadCfg(lm, "off", "");
    Rig r(*cfg);
    SyncedEpoch scratch;
    const SyncedEpoch in = r.epoch({gps("G01"), bdsSingle("C06")});
    const SyncedEpoch& out = r.run(in, scratch);
    EXPECT_EQ(&out, &in) << "the same epoch object comes back: no copy";
    EXPECT_TRUE(scratch.by_station.empty());
    const auto& c = r.ctr();
    EXPECT_EQ(c.epochs, 0) << "not even the epoch clock advances";
    EXPECT_EQ(c.records_screened, 0);
    EXPECT_EQ(c.units_screened, 0);
    EXPECT_TRUE(c.affected.empty());
    EXPECT_TRUE(r.fx->ctx().entities.stations.empty()) << "no entity row is created";
}

TEST(SecondBandSeam, TheHeldOutConsumerIsNeverScreened) {
    // The seam walks the producer roster only: a station that is not on it is not touched.
    LogManager lm;
    auto cfg = loadCfg(lm, "consumer", armed());
    Rig r(*cfg);
    SyncedEpoch scratch;
    SyncedEpoch in = r.epoch({gps("G01")});
    in.by_station["WTZZ"] = {gps("G01"), bdsSingle("C06")};
    const SyncedEpoch& out = r.run(in, scratch);
    EXPECT_EQ(&out, &in);
    EXPECT_EQ(sats(out, "WTZZ").size(), 2u);
}

TEST(SecondBandSeam, TheRunEndReportNamesTheStationsAndTheEdges) {
    LogManager lm;
    auto cfg = loadCfg(lm, "report", armed());
    Rig r(*cfg);
    SyncedEpoch scratch;
    for (int k = 0; k < 2; ++k) {
        const SyncedEpoch in = r.epoch({gps("G01"), bdsSingle("C06")}, {gps("G01"), bdsDual("C06")});
        r.run(in, scratch);
    }
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto lg = std::make_shared<spdlog::logger>("secband_report", sink);
    networkSecondBandAdmissionReport(r.fx->ctx(), lg);
    const std::string text = oss.str();
    EXPECT_NE(text.find("SECOND-BAND ADMISSION, scope station"), std::string::npos) << text;
    EXPECT_NE(text.find("1 withhold, 0 readmit"), std::string::npos) << text;
    EXPECT_NE(text.find("BeiDou withheld at 1 station(s) (AAAA)"), std::string::npos) << text;
    EXPECT_EQ(text.find("ARMED AND INERT"), std::string::npos) << "the rule acted";
}

TEST(SecondBandSeam, AnArmedRuleThatWithheldNothingReportsItselfInert) {
    LogManager lm;
    auto cfg = loadCfg(lm, "inert", armed());
    Rig r(*cfg);
    SyncedEpoch scratch;
    const SyncedEpoch in = r.epoch({gps("G01"), bdsDual("C06")});
    r.run(in, scratch);
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto lg = std::make_shared<spdlog::logger>("secband_report", sink);
    networkSecondBandAdmissionReport(r.fx->ctx(), lg);
    EXPECT_NE(oss.str().find("ARMED AND INERT"), std::string::npos) << oss.str();
}

TEST(SecondBandSeam, UnarmedTheRunEndReportIsSilent) {
    LogManager lm;
    auto cfg = loadCfg(lm, "silent", "");
    Rig r(*cfg);
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto lg = std::make_shared<spdlog::logger>("secband_report", sink);
    networkSecondBandAdmissionReport(r.fx->ctx(), lg);
    EXPECT_TRUE(oss.str().empty());
}

// ---------------------------------------------------------------------------
// 6. The loader
// ---------------------------------------------------------------------------
TEST(SecondBandConfig, DefaultsAreOffStationScopeThresholdOne) {
    LogManager lm;
    auto cfg = loadCfg(lm, "defaults", "");
    EXPECT_FALSE(cfg->networkSecondBandAdmission());
    EXPECT_EQ(cfg->networkSecondBandScope(), SecondBandScope::Station);
    EXPECT_EQ(cfg->networkSecondBandMinSats(), 1);
}

TEST(SecondBandConfig, ArmedValuesAreRead) {
    LogManager lm;
    auto cfg = loadCfg(lm, "armed", armed("satellite", 3));
    EXPECT_TRUE(cfg->networkSecondBandAdmission());
    EXPECT_EQ(cfg->networkSecondBandScope(), SecondBandScope::Satellite);
    EXPECT_EQ(cfg->networkSecondBandMinSats(), 3);
}

TEST(SecondBandConfig, ScopeNamesAreCaseInsensitiveAndAnUnknownNameFallsBackToStation) {
    LogManager lm;
    EXPECT_EQ(loadCfg(lm, "caps", armed("Satellite"))->networkSecondBandScope(),
              SecondBandScope::Satellite);
    EXPECT_EQ(loadCfg(lm, "bad", armed("constellation"))->networkSecondBandScope(),
              SecondBandScope::Station);
}

TEST(SecondBandConfig, AThresholdBelowOneIsClampedToOne) {
    LogManager lm;
    EXPECT_EQ(loadCfg(lm, "zero", armed("station", 0))->networkSecondBandMinSats(), 1);
    EXPECT_EQ(loadCfg(lm, "neg", armed("station", -4))->networkSecondBandMinSats(), 1);
}

TEST(SecondBandConfig, TheKnobsReachTheRunContext) {
    LogManager lm;
    auto cfg = loadCfg(lm, "knobs", armed("satellite", 2));
    NetworkTestRun fx(*cfg);
    EXPECT_TRUE(fx.ctx().knobs.second_band_admission);
    EXPECT_EQ(fx.ctx().knobs.second_band_scope, SecondBandScope::Satellite);
    EXPECT_EQ(fx.ctx().knobs.second_band_min_sats, 2);
}

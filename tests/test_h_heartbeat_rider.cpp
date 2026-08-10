/**
 * @file test_h_heartbeat_rider.cpp
 * @brief The H-heartbeat rider (docs/HANDOFF_edge_response_queue.md §1.6 item 3):
 *        (a) the batch product read surfaces EVERY H record — before this,
 *        1440/12 h were written and dropped unread (only the live tail saw
 *        them), so the replay consumer's producer-status channel was
 *        structurally blind; (b) the census-fed σ̂₀'s wire self-description
 *        (S-line hsig= token) and its loader contract.
 *
 * What is pinned here:
 *   1. The 4-arg read returns every H in file order, INCLUDING the standalone
 *      H of a fully WITHHELD epoch — which completes no product record and is
 *      therefore invisible to both the returned records and the per-record
 *      epoch_status field.  That standalone case is the whole point: withheld
 *      stretches are exactly when a consumer must hear from the producer.
 *   2. The pre-existing surfaces are untouched: matching-H attach to the next
 *      E record still works, and the 3-arg read still parses the same file.
 *   3. The hsig= contract token round-trips, and its absence round-trips
 *      (byte-identical S line when the feed is off).
 *   4. Loader contract (invariant #4): default OFF; INERT-safe without the H
 *      records or without the census walk it reads; hard error on an empty
 *      class set (a permanent 0.0000 masquerading as a measurement).
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/SsrCorrectionStore.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempFile(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("h_rider_" + tag);
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

// ===========================================================================
// (a) the batch read surfaces every H
// ===========================================================================

TEST(HHeartbeatRider, BatchReadSurfacesEveryHeartbeatIncludingStandaloneWithheld) {
    // Three heartbeats: an emitted epoch's H (attaches to its E), a STANDALONE
    // withheld-gate H (no E — the previously invisible case), and a second
    // emitted epoch's H.  E-line field counts match the live writer.
    const fs::path p = writeTempFile("read.txt",
        "# gnss_network_product v1\n"
        "H,2313,259200,ok,1.2345,32,-\n"
        "E,2313,259200,9,0,0,0\n"
        "H,2313,259230,withheld_gate,0.0000,0,*\n"
        "H,2313,259260,partial,2.5,31,G\n"
        "E,2313,259260,9,0,0,0\n");

    std::vector<NetworkProducerStatus> statuses;
    bool ok = false;
    const auto records = NetworkProductReader::read(p.string(), nullptr, &ok, &statuses);

    ASSERT_EQ(records.size(), 2u);
    ASSERT_EQ(statuses.size(), 3u) << "the standalone withheld H must surface";

    EXPECT_EQ(statuses[0].state, NetworkProducerStatus::State::Ok);
    EXPECT_NEAR(statuses[0].sigma0, 1.2345, 1e-9);
    EXPECT_EQ(statuses[0].n_sats_published, 32);
    EXPECT_DOUBLE_EQ(statuses[0].time.tow, 259200.0);

    EXPECT_EQ(statuses[1].state, NetworkProducerStatus::State::WithheldGate);
    EXPECT_EQ(statuses[1].withheld, "*");
    EXPECT_DOUBLE_EQ(statuses[1].time.tow, 259230.0);

    EXPECT_EQ(statuses[2].state, NetworkProducerStatus::State::Partial);
    EXPECT_NEAR(statuses[2].sigma0, 2.5, 1e-9);
    EXPECT_EQ(statuses[2].withheld, "G");

    // The pre-existing attach surface is untouched: the emitted epochs carry
    // their own H; the withheld H attached to nothing.
    ASSERT_TRUE(records[0].epoch_status.has_value());
    EXPECT_EQ(records[0].epoch_status->state, NetworkProducerStatus::State::Ok);
    ASSERT_TRUE(records[1].epoch_status.has_value());
    EXPECT_EQ(records[1].epoch_status->state, NetworkProducerStatus::State::Partial);

    // The 3-arg read still parses the same file (source compatibility).
    bool ok3 = false;
    const auto records3 = NetworkProductReader::read(p.string(), nullptr, &ok3);
    EXPECT_EQ(records3.size(), 2u);

    std::error_code ec; fs::remove(p, ec);
}

// ===========================================================================
// (b) the hsig= contract token
// ===========================================================================

TEST(HHeartbeatRider, ContractHsigTokenRoundTrips) {
    SbasisContract c;
    c.decl.reference_receiver_id = "TEST00XYZ";
    c.apriori_product_id = "CODTEST";
    c.h_sigma0_source = "pc";
    c.present = true;

    const fs::path p = fs::temp_directory_path() / "h_rider_contract.txt";
    { NetworkProductWriter w(p.string(), c); ASSERT_TRUE(w.good()); }

    SbasisContract back;
    bool ok = false;
    (void)NetworkProductReader::read(p.string(), &back, &ok);
    ASSERT_TRUE(back.present);
    EXPECT_EQ(back.h_sigma0_source, "pc");
    std::error_code ec; fs::remove(p, ec);
}

TEST(HHeartbeatRider, ContractWithoutHsigStaysEmpty) {
    SbasisContract c;
    c.decl.reference_receiver_id = "TEST00XYZ";
    c.apriori_product_id = "CODTEST";
    c.present = true;

    const fs::path p = fs::temp_directory_path() / "h_rider_nohsig.txt";
    { NetworkProductWriter w(p.string(), c); ASSERT_TRUE(w.good()); }

    // Byte-level: the token must be absent, not empty-valued.
    std::ifstream f(p);
    std::string line, s_line;
    while (std::getline(f, line))
        if (!line.empty() && line[0] == 'S') { s_line = line; break; }
    ASSERT_FALSE(s_line.empty());
    EXPECT_EQ(s_line.find("hsig="), std::string::npos);

    SbasisContract back;
    bool ok = false;
    (void)NetworkProductReader::read(p.string(), &back, &ok);
    ASSERT_TRUE(back.present);
    EXPECT_TRUE(back.h_sigma0_source.empty());
    std::error_code ec; fs::remove(p, ec);
}

// ===========================================================================
// loader contract
// ===========================================================================

std::string networkConfig(const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"estimation\": {\n"
        "    \"estimate_ifb\": true\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    return writeTempFile("cfg_" + tag + ".json", body);
}

TEST(HHeartbeatRider, CensusFeedDefaultsOff) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig(""));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkHSigma0PrefitCensus());
    EXPECT_EQ(cfg.networkHSigma0Classes(), "code,sh,other");
    std::error_code ec; fs::remove(p, ec);
}

// No H records ⇒ nothing to feed.
TEST(HHeartbeatRider, CensusFeedForcedOffWithoutHeartbeat) {
    LogManager log;
    const fs::path p = writeTempConfig("noh",
        networkConfig("    \"trace_prefit_census\": true,\n"
                      "    \"h_sigma0_prefit_census\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkHSigma0PrefitCensus());
    std::error_code ec; fs::remove(p, ec);
}

// No census walk ⇒ no presence map ⇒ births would pollute the statistic.
TEST(HHeartbeatRider, CensusFeedForcedOffWithoutTheCensusWalk) {
    LogManager log;
    const fs::path p = writeTempConfig("nocensus",
        networkConfig("    \"emit_epoch_status_record\": true,\n"
                      "    \"h_sigma0_prefit_census\": true,\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(cfg.networkHSigma0PrefitCensus());
    std::error_code ec; fs::remove(p, ec);
}

TEST(HHeartbeatRider, CensusFeedSurvivesFullyArmed) {
    LogManager log;
    const fs::path p = writeTempConfig("armed",
        networkConfig("    \"emit_epoch_status_record\": true,\n"
                      "    \"trace_prefit_census\": true,\n"
                      "    \"h_sigma0_prefit_census\": true,\n"
                      "    \"h_sigma0_classes\": \"code,sh\",\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(cfg.networkHSigma0PrefitCensus());
    EXPECT_EQ(cfg.networkHSigma0Classes(), "code,sh");
    std::error_code ec; fs::remove(p, ec);
}

// An empty class set is a permanent 0.0000 masquerading as a measurement.
TEST(HHeartbeatRider, CensusFeedRejectsAnEmptyClassSet) {
    LogManager log;
    const fs::path p = writeTempConfig("emptymask",
        networkConfig("    \"emit_epoch_status_record\": true,\n"
                      "    \"trace_prefit_census\": true,\n"
                      "    \"h_sigma0_prefit_census\": true,\n"
                      "    \"h_sigma0_classes\": \"none\",\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

}  // namespace

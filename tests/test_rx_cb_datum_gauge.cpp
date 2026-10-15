/**
 * @file test_rx_cb_datum_gauge.cpp
 * @brief The run-end gauge counter of the zero-mean receiver-DCB datum
 *        (network.rx_code_bias_datum, RXCB-DATUM).
 *
 * `n_rxcb_gauges` is the only instrument that distinguishes a live
 * zero-mean-receiver-DCB datum from an armed but inert one, which the run-end
 * RXCB-DATUM line in networkSbasisDatumRunEnd asserts under standing invariant #4.  The
 * datum and the rx-CB carry are independent knobs, and datum without carry is the
 * canonical posture, so the accumulation (networkRxCbDatumGaugeCensus, called by
 * networkHarvestLocalCarries) sits outside the carry bracket
 * (`if (ctx.knobs.rx_cb_on)`).  Inside it, a datum-only run would report zero folds while the datum folds, and the run-end line would
 * raise its invariant-#4 alarm against a working gauge.
 *
 * What is pinned here:
 *
 *  1. **The datum-only posture counts.**  Datum armed, carry absent, folds
 *     present ⇒ the counter reports them.
 *  2. **The counter follows the datum knob, not the carry knob.**  Datum on with
 *     carry off counts; datum off counts nothing.
 *  3. **Both armed counts once.**  A second accumulation site (inside the carry
 *     bracket) would double the count; this is the case that catches it.
 *  4. **Stations absent from the estimator are skipped**, matching every other
 *     per-station walk in the harvest (`est_index_of[i] < 0`).
 *  5. Loader contract: the two knobs parse independently, and the datum does not
 *     require the carry.
 *
 * The harvest is called through the test run fixture with `carry_warm = false`
 * and no armed carry knobs, so every walk ahead of the census is gated off and neither the NetworkEstimator
 * nor any station's StateManager is dereferenced; the toy fixture below needs
 * no solvable network.
 */

#include <gtest/gtest.h>

#include "app/NetworkLocalCarryHarvest.hpp"   // networkHarvestLocalCarries (card NOM3 step HV1)
#include "app/NetworkRunContext.hpp"
#include "support/NetworkTestRun.hpp"   // the run and epoch the harvest's epoch form takes
#include "config/GnssConfig.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkPriorResidual.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/NetworkSbasis.hpp"
#include "logging/LogManager.hpp"

#include <Eigen/Dense>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& network_extra) {
    const std::string body = std::string("{\n") +
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
    const fs::path p = fs::temp_directory_path() / ("rxcbdatum_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    return p;
}

struct Loaded {
    LogManager log;
    GnssConfig cfg;
    fs::path   path;
    explicit Loaded(const std::string& tag, const std::string& extra) {
        path = writeTempConfig(tag, extra);
        EXPECT_TRUE(cfg.loadFromJson(path, log));
    }
    ~Loaded() { std::error_code ec; fs::remove(path, ec); }
};

/// Datum armed, carry not armed: the canonical RXCB-DATUM posture, and the one
/// a counter inside the carry bracket would miss.
const char* const kDatumOnly =
    "    \"rx_code_bias_datum\": true,\n";

/// Datum + carry.  carry_rx_code_bias is force-disabled at load unless
/// code_datum_time_linked is on (the loader says so), so arming it takes both.
const char* const kDatumAndCarry =
    "    \"rx_code_bias_datum\": true,\n"
    "    \"code_datum_time_linked\": true,\n"
    "    \"carry_rx_code_bias\": true,\n";

/// Neither knob: the byte-identical baseline.
const char* const kNeither = "";

/// Drive the census with `n` stations whose per-station fold counts are `rows`.
/// A negative entry in `est_idx` marks a station the estimator never admitted.
std::size_t runCensus(const GnssConfig& cfg,
                      const std::vector<int>& rows,
                      const std::vector<int>& est_idx) {
    // The run and the epoch the harvest's epoch form takes (card NOM3 step HV1; the
    // fixture builds the context, the engines and an empty setup from the configuration).
    NetworkTestRun fx(cfg);
    fx.setup().stations.resize(rows.size());
    NetworkEpoch& e = fx.newEpoch(GnssTime{});
    e.assembly.asm_res.resize(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        fx.setup().stations[i].name = "S" + std::to_string(i);
        e.assembly.asm_res[i].pl_cb_datum_rows = rows[i];
    }
    e.assembly.est_index_of = est_idx;
    e.clock = EpochClock{};   // carry_warm false, pl_now_s 0

    // carry_warm = false, and no carry knob is armed, so every walk ahead of the
    // census short-circuits before touching the estimator or any station's StateManager.
    networkHarvestLocalCarries(fx.run(), e);
    return fx.ctx().counters.n_rxcb_gauges;
}

}  // namespace

// ===========================================================================
// 1: a datum-only run reports its folds
// ===========================================================================

// With the counter inside the carry bracket this returns 0 and the run-end line
// declares a working gauge inert.  Two stations folding 3 and 4 prior rows = 7.
TEST(RxCbDatumGauge, DatumOnlyPostureCountsItsFolds) {
    Loaded L("datum_only", kDatumOnly);
    ASSERT_TRUE(L.cfg.networkRxCodeBiasDatum());
    ASSERT_FALSE(L.cfg.networkCarryRxCodeBias());   // the posture under test

    EXPECT_EQ(runCensus(L.cfg, {3, 4}, {0, 1}), 7u);
}

// ===========================================================================
// 2: the counter follows the datum knob (datum off here; datum on in test 1)
// ===========================================================================

// Datum off ⇒ no census, whatever the assembly left in the field.  In production
// pl_cb_datum_rows is structurally 0 here (the row build is itself gated on
// rx_cb_datum), so this pins the gate rather than a reachable state; the gate is
// what keeps the off path byte-identical.
TEST(RxCbDatumGauge, DatumOffCountsNothing) {
    Loaded L("neither", kNeither);
    ASSERT_FALSE(L.cfg.networkRxCodeBiasDatum());

    EXPECT_EQ(runCensus(L.cfg, {3, 4}, {0, 1}), 0u);
}

// ===========================================================================
// 3: both armed counts once (guards against a second accumulation site)
// ===========================================================================

TEST(RxCbDatumGauge, DatumAndCarryTogetherCountExactlyOnce) {
    Loaded L("both", kDatumAndCarry);
    ASSERT_TRUE(L.cfg.networkRxCodeBiasDatum());
    ASSERT_TRUE(L.cfg.networkCarryRxCodeBias());    // both armed: the double-count case

    // 7, not 14: the census is the only accumulation site.
    EXPECT_EQ(runCensus(L.cfg, {3, 4}, {0, 1}), 7u);
}

// ===========================================================================
// 4: stations the estimator never admitted are skipped
// ===========================================================================

TEST(RxCbDatumGauge, SkipsStationsAbsentFromTheEstimator) {
    Loaded L("absent", kDatumOnly);

    // The middle station has est_index_of < 0, so its 99 rows are not counted.
    EXPECT_EQ(runCensus(L.cfg, {3, 99, 4}, {0, -1, 1}), 7u);
}

// ===========================================================================
// 5: loader contract: the two knobs are independent
// ===========================================================================

// The datum does not require the carry.  This is the posture the run-end line
// describes, and the reason its counter cannot sit behind the carry's knob.
TEST(RxCbDatumGauge, DatumDoesNotRequireTheCarry) {
    Loaded L("independent", kDatumOnly);
    EXPECT_TRUE(L.cfg.networkRxCodeBiasDatum());
    EXPECT_FALSE(L.cfg.networkCarryRxCodeBias());
    EXPECT_GT(L.cfg.networkRxCodeBiasDatumSigmaM(), 0.0);   // a usable prior sigma

    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    EXPECT_TRUE(ctx.knobs.rx_cb_datum);    // the census gate
    EXPECT_FALSE(ctx.knobs.rx_cb_on);      // the carry gate, independent of the census
}

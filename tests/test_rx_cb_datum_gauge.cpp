/**
 * @file test_rx_cb_datum_gauge.cpp
 * @brief RXCB-DATUM (network.rx_code_bias_datum) — the run-end gauge counter.
 *
 * `n_rxcb_gauges` is the ONLY instrument that can distinguish a live
 * zero-mean-receiver-DCB datum from an armed-but-inert one, which is what the
 * run-end RXCB-DATUM line in NetworkTeardown asserts under standing invariant #4.
 * Its accumulation used to sit INSIDE the rx-CB CARRY bracket
 * (`if (ctx.knobs.rx_cb_on)`) in networkHarvestLocalCarries, even though the datum
 * and the carry are independent knobs and datum-WITHOUT-carry is the canonical
 * posture.  Every such run therefore reported ZERO folds while the datum was in
 * fact folding, and the run-end line fired its invariant-#4 alarm against a
 * healthy gauge — a FALSE NEGATIVE on the one channel that exists to catch a
 * false negative.
 *
 * What is pinned here:
 *
 *  1. **The datum-only posture counts.**  The regression proper: datum armed,
 *     carry absent, folds present ⇒ the counter reports them.
 *  2. **The counter follows the DATUM knob, not the carry knob.**  Both
 *     directions, so a future refactor cannot re-couple them silently.
 *  3. **Armed-both counts exactly ONCE.**  Hoisting an accumulation out of a
 *     bracket invites a double count if the original site survives; this is the
 *     case that would catch it.
 *  4. **Stations absent from the estimator are skipped**, matching every other
 *     per-station walk in the harvest (`est_index_of[i] < 0`).
 *  5. Loader contract: the two knobs parse independently, and the datum does not
 *     require the carry.
 *
 * The harvest is called with `carry_warm = false` and no armed carry knobs, so
 * every walk ahead of the census is gated off and neither the NetworkEstimator
 * nor any station's StateManager is dereferenced — the toy fixture below needs
 * no solvable network.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
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

/// Datum armed, carry NOT armed — the canonical RXCB-DATUM posture and the one
/// the defect silenced.
const char* const kDatumOnly =
    "    \"rx_code_bias_datum\": true,\n";

/// Datum + carry.  carry_rx_code_bias is force-disabled at load unless
/// code_datum_time_linked is on (the loader says so), so arming it takes both.
const char* const kDatumAndCarry =
    "    \"rx_code_bias_datum\": true,\n"
    "    \"code_datum_time_linked\": true,\n"
    "    \"carry_rx_code_bias\": true,\n";

/// Neither knob — the byte-identical baseline.
const char* const kNeither = "";

/// Drive the census with `n` stations whose per-station fold counts are `rows`.
/// A negative entry in `est_idx` marks a station the estimator never admitted.
std::size_t runCensus(const GnssConfig& cfg,
                      const std::vector<int>& rows,
                      const std::vector<int>& est_idx) {
    NetworkRunContext ctx(cfg, cfg.getNetworkConfig());

    std::vector<NetStation> stations(rows.size());
    std::vector<StationAssembly> asm_res(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        stations[i].name = "S" + std::to_string(i);
        asm_res[i].pl_cb_datum_rows = rows[i];
    }

    NetworkEstimator      est;
    Eigen::VectorXd       x_s;
    NetworkSbasis::Plan   plan;
    // Neither engine has a default ctor; both are inert here (their feed seams are
    // carry_warm-gated) so a minimal no-family instance is enough.
    NetworkCarryVce       carry_vce({}, /*window_epochs=*/1.0);
    NetworkPriorResidual  prires({}, /*window_epochs=*/1.0);
    const std::vector<std::string>                          clock_sats;
    const std::set<std::string>                             phase_keys;
    const std::vector<std::pair<std::string, std::string>>  code_keys;

    // carry_warm = false, and no carry knob is armed, so every walk ahead of the
    // census short-circuits before touching `est` or any station's StateManager.
    networkHarvestLocalCarries(ctx, cfg, est, x_s, stations, est_idx, asm_res,
                               plan, nullptr, nullptr, carry_vce, prires,
                               /*carry_warm=*/false, /*pl_now_s=*/0.0,
                               clock_sats, phase_keys, code_keys);
    return ctx.counters.n_rxcb_gauges;
}

}  // namespace

// ===========================================================================
// 1: the regression — a datum-only run must report its folds
// ===========================================================================

// THE DEFECT.  Before the fix this returned 0 and the run-end line declared a
// healthy gauge inert.  Two stations folding 3 and 4 prior rows = 7.
TEST(RxCbDatumGauge, DatumOnlyPostureCountsItsFolds) {
    Loaded L("datum_only", kDatumOnly);
    ASSERT_TRUE(L.cfg.networkRxCodeBiasDatum());
    ASSERT_FALSE(L.cfg.networkCarryRxCodeBias());   // the posture under test

    EXPECT_EQ(runCensus(L.cfg, {3, 4}, {0, 1}), 7u);
}

// ===========================================================================
// 2: the counter follows the DATUM knob — both directions
// ===========================================================================

// Datum OFF ⇒ no census, whatever the assembly left in the field.  In production
// pl_cb_datum_rows is structurally 0 here (the row build is itself gated on
// rx_cb_datum), so this pins the gate rather than a reachable state — which is
// exactly what makes the OFF path byte-identical.
TEST(RxCbDatumGauge, DatumOffCountsNothing) {
    Loaded L("neither", kNeither);
    ASSERT_FALSE(L.cfg.networkRxCodeBiasDatum());

    EXPECT_EQ(runCensus(L.cfg, {3, 4}, {0, 1}), 0u);
}

// ===========================================================================
// 3: armed-both counts exactly once (the hoist's double-count hazard)
// ===========================================================================

TEST(RxCbDatumGauge, DatumAndCarryTogetherCountExactlyOnce) {
    Loaded L("both", kDatumAndCarry);
    ASSERT_TRUE(L.cfg.networkRxCodeBiasDatum());
    ASSERT_TRUE(L.cfg.networkCarryRxCodeBias());    // both armed — the hazard case

    // 7, not 14: the census is the SOLE accumulation site.
    EXPECT_EQ(runCensus(L.cfg, {3, 4}, {0, 1}), 7u);
}

// ===========================================================================
// 4: stations the estimator never admitted are skipped
// ===========================================================================

TEST(RxCbDatumGauge, SkipsStationsAbsentFromTheEstimator) {
    Loaded L("absent", kDatumOnly);

    // Middle station has est_index_of < 0 — its 99 rows never existed.
    EXPECT_EQ(runCensus(L.cfg, {3, 99, 4}, {0, -1, 1}), 7u);
}

// ===========================================================================
// 5: loader contract — the two knobs are independent
// ===========================================================================

// The datum does NOT require the carry.  This is the posture the run-end line
// describes, and the reason its counter cannot live behind the carry's knob.
TEST(RxCbDatumGauge, DatumDoesNotRequireTheCarry) {
    Loaded L("independent", kDatumOnly);
    EXPECT_TRUE(L.cfg.networkRxCodeBiasDatum());
    EXPECT_FALSE(L.cfg.networkCarryRxCodeBias());
    EXPECT_GT(L.cfg.networkRxCodeBiasDatumSigmaM(), 0.0);   // a usable prior sigma

    NetworkRunContext ctx(L.cfg, L.cfg.getNetworkConfig());
    EXPECT_TRUE(ctx.knobs.rx_cb_datum);    // the census gate
    EXPECT_FALSE(ctx.knobs.rx_cb_on);      // the carry gate it used to hide behind
}

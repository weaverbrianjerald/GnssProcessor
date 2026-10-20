/**
 * @file test_galileo_network.cpp
 * @brief GPS and Galileo dual frequency on the network path
 *        (docs/refound/GAL_gps_galileo_dual_2026-09-22.md).
 *
 * What the Galileo network path builds and these cases pin:
 *
 *  - the broadcast group delay rule of the RF-0c geometry-free pin, per
 *    constellation: T_GD on a GPS (L1, L2) pair, BGD(E1, E5a) on a Galileo
 *    (E1, E5a) pair, zero with its own counter on a pair no latched group delay
 *    describes;
 *  - the time-linked extra-channel anchor, which asserts the target its pin pair was
 *    pinned at (a Galileo C5X column is anchored at the same value as its (C1C, C5Q)
 *    pair, c*BGD or zero);
 *  - the pin rows of a Galileo satellite tracked by both channel families: the
 *    ionosphere-free and geometry-free pins on (C1C, C5Q), C5X cross-band at the same
 *    target, C1X free with its channel level row;
 *  - the per-constellation split of the pin counters;
 *  - the loader's Galileo admission consistency check;
 *  - the receiver inter-system bias allocation per station;
 *  - the products wire on Galileo C, B and P records;
 *  - the E1 / E5a band mapping;
 *  - the tabulated satellite-clock validity bound: a Galileo clock of several
 *    milliseconds is a real value (E05 sits at +4.75 ms on DOY-129), not a bad sample.
 *
 * Suite name @c GalileoNetworkTest is owned by this file alone (one suite name, one
 * file).
 */

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "common/GnssTypes.hpp"
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "common/SignalBand.hpp"
#include "config/GnssConfig.hpp"
#include "config/GnssConfigTypes.hpp"
#include "corrections/GnssCorrections.hpp"
#include "network/solve/Estimator.hpp"
#include "network/assembly/MultiGnss.hpp"
#include "network/solve/Sbasis.hpp"
#include "network/state/StationParams.hpp"
#include "logging/LogManager.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkProductIO.hpp"
#include "products/ProductTypes.hpp"
#include "products/SsrCorrectionStore.hpp"
#include "../support/TestTempDir.hpp"   // per-process, per-test scratch directory

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr double kF1 = 1575.42e6;   // GPS L1 == Galileo E1
constexpr double kF2 = 1227.60e6;   // GPS L2
constexpr double kF5 = 1176.45e6;   // GPS L5 == Galileo E5a
constexpr double kF7 = 1207.14e6;   // Galileo E5b

/// Carrier frequency by (satellite, signal), in the builder's oracle shape: GPS bands 1,
/// 2, 5; Galileo bands 1, 5, 7.  Unknown band -> 0.
double codeFreq(const std::string& sat, const std::string& sig) {
    if (sig.size() < 2 || sat.empty()) return 0.0;
    if (sat.front() == 'E') {
        switch (sig[1]) {
            case '1': return kF1;
            case '5': return kF5;
            case '7': return kF7;
            default:  return 0.0;
        }
    }
    switch (sig[1]) {
        case '1': return kF1;
        case '2': return kF2;
        case '5': return kF5;
        default:  return 0.0;
    }
}

/// Owns the builder's side channels, cleared before each call as the driver contract
/// requires.  The posture defaults are the RF-0hv bases': time-linked code datum,
/// broadcast_tgd with the geometry-free time link.
struct Harness {
    using Pair = std::pair<std::string, std::string>;

    bool cb_time_linked{true};
    NetworkSatGfDatum sat_gf_datum{NetworkSatGfDatum::BroadcastTgd};
    std::map<std::string, double> sat_gf_target_m;
    bool sat_gf_datum_time_linked{true};

    std::map<std::string, std::map<std::string, double>> code_bias_prev;
    std::map<std::string, Pair>                          code_bias_ifpin_pair;
    std::set<std::string>                                code_bias_sat_seen;

    std::map<std::string, Pair> if_pin_sel, if_pin_real, cb_ifpin;
    std::set<std::string>       cb_reseed;

    NetworkSbasis::CodeBiasDatumRows build(const NetworkEstimator& est,
                                           const std::vector<Pair>& keys) {
        if_pin_sel.clear(); if_pin_real.clear(); cb_ifpin.clear(); cb_reseed.clear();
        const std::set<std::string> none;
        return NetworkSbasis::buildCodeBiasDatumRows(
            est, keys, codeFreq, cb_time_linked, /*emit_clock_disc=*/false,
            sat_gf_datum, sat_gf_target_m, sat_gf_datum_time_linked,
            /*sat_isc_state=*/false, none,
            /*sat_gf_extra_band_free=*/false, none,
            code_bias_prev, code_bias_ifpin_pair,
            code_bias_sat_seen, /*code_bias_datum_resumed=*/false,
            if_pin_sel, if_pin_real, cb_ifpin, cb_reseed);
    }
};

std::vector<Harness::Pair> registerRoster(NetworkEstimator& est,
                                          const std::vector<std::string>& sats,
                                          const std::vector<std::string>& sigs) {
    std::vector<Harness::Pair> keys;
    for (const auto& sat : sats)
        for (const auto& s : sigs) {
            est.registerCodeBias(sat, s);
            keys.emplace_back(sat, s);
        }
    return keys;
}

/// Rows that touch column @p col, with their coefficient.
std::vector<std::pair<Eigen::Index, double>> rowsOn(const Eigen::MatrixXd& H, Eigen::Index col) {
    std::vector<std::pair<Eigen::Index, double>> out;
    for (Eigen::Index r = 0; r < H.rows(); ++r)
        if (std::abs(H(r, col)) > 1e-12) out.emplace_back(r, H(r, col));
    return out;
}

/// The one row whose only nonzero columns are @p a and @p b (a two-entry pin row).
Eigen::Index pinRow(const Eigen::MatrixXd& H, Eigen::Index a, Eigen::Index b) {
    for (Eigen::Index r = 0; r < H.rows(); ++r) {
        int nz = 0;
        for (Eigen::Index c = 0; c < H.cols(); ++c)
            if (std::abs(H(r, c)) > 1e-12) ++nz;
        if (nz == 2 && std::abs(H(r, a)) > 1e-12 && std::abs(H(r, b)) > 1e-12) return r;
    }
    return -1;
}

double mu(double fa, double fb) { return (fa * fa) / (fb * fb); }

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = gnss_test::tempDir() / ("gal_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config with the given processing systems.
std::string networkConfig(const std::string& active, const std::string& priorities) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": " + active + ",\n"
        "    \"signal_priorities\": " + priorities + "\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

const char* kGpsGalPriorities =
    "{ \"GPS\": [\"C1C\", \"L1C\", \"C2W\", \"L2W\"], "
    "\"Galileo\": [\"C1C\", \"C1X\", \"L1C\", \"L1X\", \"C5Q\", \"C5X\", \"L5Q\", \"L5X\"] }";

}  // namespace

// ===========================================================================
// The broadcast group delay rule, per constellation (R3 (i))
// ===========================================================================

// The rule is the navigation message's: T_GD describes a GPS (L1, L2) pair, and
// BGD(E1, E5a) describes a Galileo (E1, E5a) pair, whichever channel family carries it.
// A Galileo (E1, E5b) pair would need BGD(E1, E5b), which the setup does not latch.
TEST(GalileoNetworkTest, GroupDelayDescribesTheRightPairPerConstellation) {
    EXPECT_TRUE(NetworkSbasis::broadcastGroupDelayDescribesPair("G05", "C1C", "C2W"));
    EXPECT_TRUE(NetworkSbasis::broadcastGroupDelayDescribesPair("G05", "C1W", "C2W"));
    EXPECT_FALSE(NetworkSbasis::broadcastGroupDelayDescribesPair("G05", "C1C", "C5Q"));
    EXPECT_TRUE(NetworkSbasis::broadcastGroupDelayDescribesPair("E11", "C1C", "C5Q"));
    EXPECT_TRUE(NetworkSbasis::broadcastGroupDelayDescribesPair("E11", "C1X", "C5X"));
    EXPECT_FALSE(NetworkSbasis::broadcastGroupDelayDescribesPair("E11", "C1C", "C7Q"));
    EXPECT_FALSE(NetworkSbasis::broadcastGroupDelayDescribesPair("E11", "C5Q", "C7Q"));
    // Unchanged for the other constellations: the RF-0c band-digit rule.
    EXPECT_TRUE(NetworkSbasis::broadcastGroupDelayDescribesPair("J02", "C1C", "C2L"));
    EXPECT_FALSE(NetworkSbasis::broadcastGroupDelayDescribesPair("J02", "C1C", "C5Q"));
    EXPECT_FALSE(NetworkSbasis::broadcastGroupDelayDescribesPair("E11", "C", "C5Q"));
}

// The four cases, in the RF-0c order: the pair rule first, then the navigation record,
// then a broadcast value of exactly zero.
TEST(GalileoNetworkTest, SatGfTargetFourCases) {
    const std::map<std::string, double> t{ { "E11", 1.25 }, { "E12", 0.0 }, { "G05", -2.0 } };
    using K = NetworkSbasis::SatGfTargetKind;

    const auto hit = NetworkSbasis::satGfBroadcastTarget("E11", "C1C", "C5Q", t);
    EXPECT_EQ(hit.kind, K::BroadcastHit);
    EXPECT_DOUBLE_EQ(hit.target_m, 1.25);

    const auto x = NetworkSbasis::satGfBroadcastTarget("E11", "C1X", "C5X", t);
    EXPECT_EQ(x.kind, K::BroadcastHit);
    EXPECT_DOUBLE_EQ(x.target_m, 1.25);

    const auto zero = NetworkSbasis::satGfBroadcastTarget("E12", "C1C", "C5Q", t);
    EXPECT_EQ(zero.kind, K::BroadcastZero);
    EXPECT_DOUBLE_EQ(zero.target_m, 0.0);

    const auto missing = NetworkSbasis::satGfBroadcastTarget("E24", "C1C", "C5Q", t);
    EXPECT_EQ(missing.kind, K::Missing);
    EXPECT_DOUBLE_EQ(missing.target_m, 0.0);

    const auto nogd = NetworkSbasis::satGfBroadcastTarget("E11", "C1C", "C7Q", t);
    EXPECT_EQ(nogd.kind, K::NoGroupDelay);
    EXPECT_DOUBLE_EQ(nogd.target_m, 0.0);

    const auto gps = NetworkSbasis::satGfBroadcastTarget("G05", "C1C", "C2W", t);
    EXPECT_EQ(gps.kind, K::BroadcastHit);
    EXPECT_DOUBLE_EQ(gps.target_m, -2.0);
}

// A Galileo (C1C, C5Q) pin pair is targeted at c*BGD on its geometry-free row, counted as
// a hit, never as a fallback; a Galileo pair with no group delay (C1C, C7Q) keeps zero
// and is counted on its own.  Per constellation, the split equals the global counters.
TEST(GalileoNetworkTest, GalileoPairPinnedAtTheBroadcastGroupDelay) {
    NetworkEstimator est;
    const std::vector<std::string> gal{ "E02", "E11", "E24" };
    const std::vector<std::string> gps{ "G05", "G10" };
    auto keys = registerRoster(est, gal, { "C1C", "C5Q" });
    const auto gkeys = registerRoster(est, gps, { "C1C", "C2W" });
    keys.insert(keys.end(), gkeys.begin(), gkeys.end());

    Harness h;
    h.sat_gf_target_m = { { "E02", 0.75 }, { "E11", 1.25 }, { "E24", -0.5 },
                          { "G05", -2.0 }, { "G10", 3.0 } };
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_sat_gf_rows, 5);
    EXPECT_EQ(rows.n_sat_gf_tgd_hits, 5);
    EXPECT_EQ(rows.n_sat_gf_pair_not_l1l2, 0);   // the Galileo pairs are hits, not fallbacks
    EXPECT_EQ(rows.n_sat_gf_tgd_missing, 0);
    EXPECT_EQ(rows.n_sat_gf_anchored, 5);        // first sighting: every row absolute

    const double inv = 1.0 / (mu(kF1, kF5) - 1.0);
    for (const auto& [sat, tgd] : std::vector<std::pair<std::string, double>>{
             { "E02", 0.75 }, { "E11", 1.25 }, { "E24", -0.5 } }) {
        const Eigen::Index c1 = est.codeBiasColumn(sat, "C1C");
        const Eigen::Index c5 = est.codeBiasColumn(sat, "C5Q");
        // Two two-entry rows touch the pair: the IF pin (a1, a2) and the GF pin.
        const auto on1 = rowsOn(rows.H, c1);
        bool gf_found = false;
        for (const auto& [r, coef] : on1) {
            if (std::abs(coef + inv) < 1e-9 && std::abs(rows.H(r, c5) - inv) < 1e-9) {
                gf_found = true;
                EXPECT_DOUBLE_EQ(rows.z(r), tgd) << sat;
            }
        }
        EXPECT_TRUE(gf_found) << sat;
    }

    ASSERT_EQ(rows.sat_gf_by_const.count("E"), 1u);
    ASSERT_EQ(rows.sat_gf_by_const.count("G"), 1u);
    EXPECT_EQ(rows.sat_gf_by_const.at("E").rows, 3);
    EXPECT_EQ(rows.sat_gf_by_const.at("E").tgd_hits, 3);
    EXPECT_EQ(rows.sat_gf_by_const.at("E").pair_no_gd, 0);
    EXPECT_EQ(rows.sat_gf_by_const.at("G").rows, 2);
    EXPECT_EQ(rows.sat_gf_by_const.at("G").tgd_hits, 2);

    // A Galileo satellite whose only codes are (C1C, C7Q): no latched group delay
    // describes that pair, so its target is zero and it is counted as such.
    NetworkEstimator est2;
    const auto k2 = registerRoster(est2, { "E30" }, { "C1C", "C7Q" });
    Harness h2;
    h2.sat_gf_target_m = { { "E30", 1.1 } };
    const auto r2 = h2.build(est2, k2);
    EXPECT_EQ(r2.n_sat_gf_rows, 1);
    EXPECT_EQ(r2.n_sat_gf_tgd_hits, 0);
    EXPECT_EQ(r2.n_sat_gf_pair_not_l1l2, 1);
    EXPECT_EQ(r2.sat_gf_by_const.at("E").pair_no_gd, 1);
    const Eigen::Index gf = pinRow(r2.H, est2.codeBiasColumn("E30", "C1C"),
                                   est2.codeBiasColumn("E30", "C7Q"));
    ASSERT_GE(gf, 0);
    // Both two-entry rows on the pair (IF and GF) carry a zero target here.
    for (Eigen::Index r = 0; r < r2.H.rows(); ++r) EXPECT_DOUBLE_EQ(r2.z(r), 0.0);
}

// ===========================================================================
// The extra-channel anchor asserts the pair's target (R3 (ii))
// ===========================================================================

// A Galileo satellite tracked by both families registers C1C, C5Q, C1X and C5X.  Its pin
// pair is (C1C, C5Q); C5X is a cross-band extra, pinned through its geometry-free
// combination against C1C.  At a first sighting under the time link the extra row is
// anchored, and the anchor is the target the pair was pinned at: c*BGD on both rows,
// which asserts C5X - C5Q = 0 (the counted RF-0d assertion) and nothing larger.  C1X is
// a same-band extra and gets no pin row, only its channel level row.
TEST(GalileoNetworkTest, PinRowsForASatelliteTrackedByBothFamilies) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, { "E11" }, { "C1C", "C5Q", "C1X", "C5X" });

    Harness h;
    h.sat_gf_target_m = { { "E11", 1.25 } };
    const auto rows = h.build(est, keys);

    const Eigen::Index c1c = est.codeBiasColumn("E11", "C1C");
    const Eigen::Index c5q = est.codeBiasColumn("E11", "C5Q");
    const Eigen::Index c1x = est.codeBiasColumn("E11", "C1X");
    const Eigen::Index c5x = est.codeBiasColumn("E11", "C5X");
    const double inv = 1.0 / (mu(kF1, kF5) - 1.0);
    const double den = kF1 * kF1 - kF5 * kF5;
    const double a1 = kF1 * kF1 / den, a2 = -kF5 * kF5 / den;

    // The ionosphere-free pin on (C1C, C5Q), target 0 at a first sighting.
    bool if_found = false, gf_found = false, x_found = false;
    for (Eigen::Index r = 0; r < rows.H.rows(); ++r) {
        if (std::abs(rows.H(r, c1c) - a1) < 1e-9 && std::abs(rows.H(r, c5q) - a2) < 1e-9) {
            if_found = true;
            EXPECT_DOUBLE_EQ(rows.z(r), 0.0);
        }
        if (std::abs(rows.H(r, c1c) + inv) < 1e-9 && std::abs(rows.H(r, c5q) - inv) < 1e-9) {
            gf_found = true;
            EXPECT_DOUBLE_EQ(rows.z(r), 1.25);
        }
        if (std::abs(rows.H(r, c1c) + inv) < 1e-9 && std::abs(rows.H(r, c5x) - inv) < 1e-9) {
            x_found = true;
            // The same c*BGD as the pair.
            EXPECT_DOUBLE_EQ(rows.z(r), 1.25);
        }
    }
    EXPECT_TRUE(if_found);
    EXPECT_TRUE(gf_found);
    EXPECT_TRUE(x_found);

    // C1X: one row only, its channel's sum-to-zero level (coefficient 1).
    const auto on_x = rowsOn(rows.H, c1x);
    ASSERT_EQ(on_x.size(), 1u);
    EXPECT_NEAR(on_x[0].second, 1.0, 1e-12);

    EXPECT_EQ(rows.n_sat_gf_rows, 1);
    EXPECT_EQ(rows.n_sat_gf_extra_rows, 1);
    EXPECT_EQ(rows.n_sat_gf_isc_zero, 1);        // the C5X - C5Q = 0 assertion, counted
    EXPECT_EQ(rows.n_sat_gf_sameband_free, 1);   // C1X
    EXPECT_EQ(rows.n_sat_gf_anchored, 2);        // the pair row and the extra row
    const auto& e = rows.sat_gf_by_const.at("E");
    EXPECT_EQ(e.extra_rows, 1);
    EXPECT_EQ(e.isc_zero, 1);
    EXPECT_EQ(e.sameband_free, 1);
    EXPECT_EQ(e.anchored, 2);
}

// Where the pair has no group delay, the extra channel is anchored at zero with it: the
// anchor does not read the latched value on its own, so the extra row cannot carry the
// latched value while its pair sits at zero.
TEST(GalileoNetworkTest, ExtraAnchorFollowsAPairWithoutAGroupDelay) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, { "E30" }, { "C1C", "C7Q", "C7X" });
    Harness h;
    h.sat_gf_target_m = { { "E30", 1.1 } };
    const auto rows = h.build(est, keys);
    const Eigen::Index c1c = est.codeBiasColumn("E30", "C1C");
    const Eigen::Index c7x = est.codeBiasColumn("E30", "C7X");
    const Eigen::Index r = pinRow(rows.H, c1c, c7x);
    ASSERT_GE(r, 0);
    EXPECT_DOUBLE_EQ(rows.z(r), 0.0);
}

// GPS is byte-identical: on a GPS (L1, L2) pair with a band-5 extra the anchor is c*T_GD
// exactly as before, and a satellite with no navigation record anchors at zero as before.
TEST(GalileoNetworkTest, GpsExtraAnchorIsUnchanged) {
    NetworkEstimator est;
    const auto keys = registerRoster(est, { "G05", "G10" }, { "C1C", "C2W", "C5Q" });
    Harness h;
    h.sat_gf_target_m = { { "G05", -2.0 } };   // G10 has no navigation record
    const auto rows = h.build(est, keys);
    const Eigen::Index r05 = pinRow(rows.H, est.codeBiasColumn("G05", "C1C"),
                                    est.codeBiasColumn("G05", "C5Q"));
    const Eigen::Index r10 = pinRow(rows.H, est.codeBiasColumn("G10", "C1C"),
                                    est.codeBiasColumn("G10", "C5Q"));
    ASSERT_GE(r05, 0);
    ASSERT_GE(r10, 0);
    EXPECT_DOUBLE_EQ(rows.z(r05), -2.0);
    EXPECT_DOUBLE_EQ(rows.z(r10), 0.0);
    EXPECT_EQ(rows.n_sat_gf_tgd_hits, 1);
    EXPECT_EQ(rows.n_sat_gf_tgd_missing, 1);
}

// ===========================================================================
// The loader's Galileo admission consistency check (R1)
// ===========================================================================

// The network admits observations by processing.active_systems, so a Galileo signal list
// without Galileo active loads, is ignored, and logs a warning; the warning text is
// asserted in test_active_system_admission.cpp.
TEST(GalileoNetworkTest, GalileoListWithoutGalileoActiveIsIgnored) {
    LogManager log;
    const fs::path p = writeTempConfig("ignored",
        networkConfig("[ \"GPS\" ]", kGpsGalPriorities));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(networkMultiGnssActive(cfg));   // Galileo is not admitted
    std::error_code ec; fs::remove(p, ec);
}

TEST(GalileoNetworkTest, GalileoActiveWithoutAListLoadsWithAWarning) {
    LogManager log;
    const fs::path p = writeTempConfig("warned",
        networkConfig("[ \"GPS\", \"Galileo\" ]",
                      "{ \"GPS\": [\"C1C\", \"L1C\", \"C2W\", \"L2W\"] }"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(networkMultiGnssActive(cfg));   // no list: Galileo is not admitted
    std::error_code ec; fs::remove(p, ec);
}

TEST(GalileoNetworkTest, GalileoActiveWithAListLoadsAndIsMultiGnss) {
    LogManager log;
    const fs::path p = writeTempConfig("armed",
        networkConfig("[ \"GPS\", \"Galileo\" ]", kGpsGalPriorities));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_TRUE(networkMultiGnssActive(cfg));
    std::error_code ec; fs::remove(p, ec);
}

TEST(GalileoNetworkTest, GpsOnlyIsNotMultiGnss) {
    LogManager log;
    const fs::path p = writeTempConfig("gps",
        networkConfig("[ \"GPS\" ]", "{ \"GPS\": [\"C1C\", \"L1C\", \"C2W\", \"L2W\"] }"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_FALSE(networkMultiGnssActive(cfg));
    std::error_code ec; fs::remove(p, ec);
}

// ===========================================================================
// The inter-system bias, one state per station per non-GPS active system (R2)
// ===========================================================================

TEST(GalileoNetworkTest, InterSystemBiasAllocatedPerStationForGalileoOnly) {
    LogManager log;
    const fs::path p = writeTempConfig("isb",
        networkConfig("[ \"GPS\", \"Galileo\" ]", kGpsGalPriorities));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    const StationParams a = startStationParams(cfg, Eigen::Vector3d(1.0, 2.0, 3.0));
    const StationParams b = startStationParams(cfg, Eigen::Vector3d(4.0, 5.0, 6.0));
    for (const StationParams* set : { &a, &b }) {
        ASSERT_EQ(set->isb.size(), 1u);
        EXPECT_EQ(set->isb.begin()->first, System::Galileo);
        EXPECT_LT(static_cast<std::size_t>(set->isb.begin()->second), set->slots.size());
        // The core: the position (held), the clock, the wet delay, then the inter-system bias.
        EXPECT_EQ(set->slots.size(), 6u);
        EXPECT_EQ(set->clock, 3);
        EXPECT_EQ(set->zwd, 4);
        EXPECT_EQ(set->isb.begin()->second, 5);
    }
    EXPECT_EQ(a.slots[0].x_lin, 1.0);
    EXPECT_EQ(b.slots[2].x_lin, 6.0);
    std::error_code ec; fs::remove(p, ec);

    const fs::path q = writeTempConfig("isb_gps",
        networkConfig("[ \"GPS\" ]", "{ \"GPS\": [\"C1C\", \"L1C\", \"C2W\", \"L2W\"] }"));
    GnssConfig cfg2;
    ASSERT_TRUE(cfg2.loadFromJson(q, log));
    const StationParams c = startStationParams(cfg2, Eigen::Vector3d::Zero());
    EXPECT_TRUE(c.isb.empty());
    EXPECT_EQ(c.slots.size(), 5u);
    fs::remove(q, ec);
}

// A station-epoch with Galileo rows and no GPS row has its clock and inter-system bias
// collinear; the census's predicate says so, and says nothing for a mixed epoch.
TEST(GalileoNetworkTest, NonGpsOnlyStationEpochPredicate) {
    auto ob = [](const std::string& sat, System s) {
        PppObservation o;
        o.satellite = sat;
        o.system    = s;
        return o;
    };
    EXPECT_TRUE(networkStationNonGpsOnly({ ob("E11", System::Galileo), ob("E12", System::Galileo) }));
    EXPECT_FALSE(networkStationNonGpsOnly({ ob("E11", System::Galileo), ob("G05", System::GPS) }));
    EXPECT_FALSE(networkStationNonGpsOnly({ ob("G05", System::GPS) }));
    EXPECT_FALSE(networkStationNonGpsOnly({}));
}

// ===========================================================================
// The products wire carries Galileo C, B and P records on the satellite id (R5)
// ===========================================================================

TEST(GalileoNetworkTest, ProductsRoundTripGalileoRecords) {
    const std::string path = (gnss_test::tempDir() / "gal_products_roundtrip.txt").string();
    std::error_code ec;
    fs::remove(path, ec);

    NetworkSolutionEpoch sol;
    sol.epoch_time = GnssTime{ 2313, 259230.0 };
    NetworkSatSolution e;
    e.sat = "E11";
    e.has_clock = true;
    e.clock_c0_m = 1.234;
    e.code_bias_m["C1C"] = 0.5;
    e.code_bias_m["C5Q"] = 0.9;
    e.code_bias_m["C1X"] = 0.55;
    NetworkPhaseBias pb;
    pb.bias_m = 0.02;
    e.phase_bias["L1C"] = pb;
    e.phase_bias["L5Q"] = pb;
    sol.sats.push_back(e);
    NetworkSatSolution g;
    g.sat = "G05";
    g.has_clock = true;
    g.clock_c0_m = -0.75;
    g.code_bias_m["C1C"] = -1.6;
    sol.sats.push_back(g);
    {
        NetworkProductWriter w(path);
        ASSERT_TRUE(w.good());
        w.write(sol, false, {}, {}, nullptr);
    }
    bool ok = false;
    const auto recs = NetworkProductReader::read(path, &ok);
    EXPECT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    const NetworkSatSolution* back = nullptr;
    for (const auto& s : recs[0].epoch.sats)
        if (s.sat == "E11") back = &s;
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(back->has_clock);
    EXPECT_NEAR(back->clock_c0_m, 1.234, 1e-9);
    EXPECT_NEAR(back->code_bias_m.at("C1C"), 0.5, 1e-9);
    EXPECT_NEAR(back->code_bias_m.at("C5Q"), 0.9, 1e-9);
    EXPECT_NEAR(back->code_bias_m.at("C1X"), 0.55, 1e-9);
    EXPECT_EQ(back->phase_bias.count("L1C"), 1u);
    EXPECT_EQ(back->phase_bias.count("L5Q"), 1u);

    // The emitter keys the store on the satellite id with no system filter.
    NetworkProductEmitter emit(0.05, /*emit_clock_disc=*/false);
    SsrCorrectionStore store;
    emit.emitToStore(recs[0].epoch, store);
    EXPECT_TRUE(store.hasCodeBias("E11", "C5Q"));
    EXPECT_NEAR(store.getCodeBias("E11", "C1X"), 0.55, 1e-9);
    EXPECT_TRUE(store.getPhaseBias("E11", "L5Q").has_value());
    EXPECT_TRUE(store.hasCodeBias("G05", "C1C"));
    fs::remove(path, ec);
}

// ===========================================================================
// The E1 / E5a band mapping
// ===========================================================================

TEST(GalileoNetworkTest, E1AndE5aBandMapping) {
    LogManager log;
    PppUtils u(log);
    for (const char* s : { "C1C", "C1X", "L1C", "L1X" })
        EXPECT_DOUBLE_EQ(u.getFrequency(s, System::Galileo, "E11"), kF1) << s;
    for (const char* s : { "C5Q", "C5X", "L5Q", "L5X" })
        EXPECT_DOUBLE_EQ(u.getFrequency(s, System::Galileo, "E11"), kF5) << s;
    EXPECT_DOUBLE_EQ(u.getFrequency("C7Q", System::Galileo, "E11"), kF7);
    EXPECT_EQ(classifySignal("Galileo", "C5X").band, FrequencyBand::GalE5a);
    EXPECT_EQ(classifySignal("Galileo", "C5Q").band, FrequencyBand::GalE5a);
    EXPECT_EQ(classifySignal("Galileo", "C1X").band, FrequencyBand::GalE1);
    EXPECT_EQ(classifySignal("Galileo", "L1C").band, FrequencyBand::GalE1);
    // The IF / GF weights the pins use on (E1, E5a) are GPS (L1, L5)'s: the pair shares
    // the carriers, so a1 + a2 = 1 and the geometry-free divisor is mu(E5a) - 1.
    const double den = kF1 * kF1 - kF5 * kF5;
    EXPECT_NEAR(kF1 * kF1 / den - kF5 * kF5 / den, 1.0, 1e-12);
    EXPECT_NEAR(mu(kF1, kF5) - 1.0, 0.79329, 1e-4);
}

// ===========================================================================
// The tabulated satellite-clock validity bound
// ===========================================================================

namespace {
std::vector<ClockData> clockSeries(const std::string& sat, double bias_s) {
    std::vector<ClockData> out;
    for (int k = 0; k < 4; ++k) {
        ClockData c;
        c.satellite  = sat;
        c.time       = GnssTime{ 2313, 259200.0 + 30.0 * k };
        c.clock_bias = bias_s + 1.0e-10 * k;
        out.push_back(c);
    }
    return out;
}
}  // namespace

// GPS satellite clocks are steered to within 1 ms of system time; Galileo clocks are not,
// and three DOY-129 values exceed 1 ms: E05 +4.75 ms, E11 +2.91 ms, E12 -1.03 ms.  A 1 ms
// bound refuses every node of such a satellite, so the interpolation returns 0 and the
// satellite is modelled against a zero clock.  The bound rejects only non-physical
// values (the SP3 bad-clock sentinel is about 1 s).
TEST(GalileoNetworkTest, MillisecondSatelliteClockIsInterpolated) {
    LogManager log;
    const GnssTime mid{ 2313, 259215.0 };
    for (const double bias : { 4.7459345e-3, 2.9082240e-3, -1.0283096e-3 }) {
        const auto clocks = clockSeries("E05", bias);
        GnssCorrections corr(log, clocks, std::vector<ErpData>{});
        EXPECT_NEAR(corr.interpolateClock("E05", mid, clocks), bias + 0.5e-10, 1e-15) << bias;
    }
    // A GPS-class clock is interpolated exactly as before.
    const auto gps = clockSeries("G17", 0.708008176954e-3);
    GnssCorrections corr_gps(log, gps, std::vector<ErpData>{});
    EXPECT_NEAR(corr_gps.interpolateClock("G17", mid, gps), 0.708008176954e-3 + 0.5e-10, 1e-15);
    // The SP3 bad-clock sentinel (999999.999999 microseconds) is still refused.
    const auto bad = clockSeries("E05", 0.999999999999);
    GnssCorrections corr_bad(log, bad, std::vector<ErpData>{});
    EXPECT_EQ(corr_bad.interpolateClock("E05", mid, bad), 0.0);
}

/**
 * @file test_w6_iono_constraint.cpp
 * @brief Card RF-0b — @c network.w6_iono_constraint, the self-contained W6 posture.
 *
 * Card: docs/refound/RF-0_baseline_2026-09-14.md, Finding 2.  Under GPS-only DUAL the
 * producer had no reachable posture without EXTERNAL ionosphere data: the co-estimated
 * SH field was off, @c network.code_bias_gf_datum (the only condition that switched the
 * loader's @c ionosphere_weighting force off) is refused under
 * @c processing.frequency_mode = dual, and the run then hard-errored without a `.INX`.
 * This option adds the third answer — NO ionosphere constraint at all, with the satellite
 * code-bias datum left to the rows @ref GnssProcessor::NetworkSbasis::buildCodeBiasDatumRows
 * already folds every epoch.
 *
 * Two halves, and the second is the one that matters.
 *
 * 1. THE PARSE + INTERLOCKS.  Default @c auto must be today's behaviour exactly
 *    (standing invariant #6), @c none must CLEAR the force rather than merely skip it,
 *    the two contradictory co-arms must fail loud, and a typo must never silently select
 *    a different product datum.
 * 2. THE DATUM'S REACH.  The card's premise is that the per-(constellation, signal)
 *    sum-to-zero rows can stand in for the GIM.  They cannot, and the tests below
 *    COMPUTE the shortfall rather than assert it: with the ionosphere-free pin and the
 *    sum-to-zero rows folded, exactly @c n_sat-1 of the rank-deficiency-3b directions
 *    (Odijk et al. 2016, J Geod 90:15-44) survive, because a sum-to-zero row is violated
 *    only by the CONSTELLATION-WIDE shift and is blind to every between-satellite one.
 *    That is the structural half of the RF-0b reading, locked here so nobody has to
 *    re-derive it from a field log.
 */

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "config/GnssConfig.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkSbasis.hpp"
#include "logging/LogManager.hpp"

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("w6_iono_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal but loadable PPP-RTK network config.  @p processing_extra is spliced into
/// the processing block, @p network_extra into the network block; both must end with a
/// trailing comma when non-empty (the fixed tail follows).
std::string networkConfig(const std::string& processing_extra,
                          const std::string& network_extra) {
    return std::string("{\n") +
        "  \"processing\": {\n"
        "    \"mode\": \"ppp-rtk-network\",\n"
        + processing_extra +
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
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

/// The W6 slice, armed the way every RF-0 lineage arms it — including the explicit
/// `ionosphere_weighting.enable: true` those configs carry (the loader forced it anyway,
/// so it was written down).  That explicit true is exactly what "none" has to CLEAR.
constexpr const char* kW6 = "    \"estimate_sat_code_bias\": true,\n";
constexpr const char* kIonoWeightOn =
    "    \"estimation\": { \"ionosphere_weighting\": { \"enable\": true } },\n";

/// GPS carrier frequency by RINEX band digit, in the buildCodeBiasDatumRows (sat, signal)
/// oracle shape.  Unknown band -> 0, which is how the builder is told a signal cannot
/// participate in a pin.
double gpsCodeFreq(const std::string& /*sat*/, const std::string& sig) {
    if (sig.size() < 2) return 0.0;
    switch (sig[1]) {
        case '1': return 1575.42e6;   // L1
        case '2': return 1227.60e6;   // L2
        case '5': return 1176.45e6;   // L5
        default:  return 0.0;
    }
}

/// mu_j = (f_ref / f_j)^2 — the ionospheric coefficient relative to @p base_sig.  The
/// rank-deficiency-3b direction is defined only up to a per-satellite scale (beta^s
/// absorbs it), so any consistent reference works for a dimension count.
double muRel(const std::string& base_sig, const std::string& sig) {
    const double f1 = gpsCodeFreq("", base_sig), fj = gpsCodeFreq("", sig);
    return (f1 * f1) / (fj * fj);
}

/// Owns the builder's side channels and clears the per-epoch scratch before each call,
/// exactly as the driver contract requires ("cleared by caller").  Deliberately
/// self-contained rather than shared with the other suites' harnesses: the suite
/// convention here is one owner per suite name.
struct DatumRowsHarness {
    using Pair = std::pair<std::string, std::string>;

    bool cb_time_linked{false};
    bool clock_signal_basis_hold{false};
    bool emit_clock_disc{false};
    bool gf_datum{false};              ///< RF-0b's whole point: the GF pin is NOT armed.
    std::set<std::string> gf_family_constellations;

    std::map<std::string, std::map<std::string, double>> code_bias_prev;
    std::map<std::string, Pair>                          code_bias_ifpin_pair;
    std::set<std::string>                                code_bias_sat_seen;
    bool                                                 code_bias_datum_resumed{false};

    std::map<std::string, Pair> held_clock_ref;
    std::map<std::string, Pair> this_epoch_if_pin_sel;
    std::map<std::string, Pair> this_epoch_if_pin_real;
    std::set<std::string>       this_epoch_gf_pin_real;
    std::map<std::string, Pair> this_epoch_family_gf_pin_real;
    std::map<std::string, Pair> this_epoch_cb_ifpin;
    std::set<std::string>       this_epoch_cb_reseed;
    std::set<std::string>       basis_relatch_disc_pending;

    NetworkSbasis::CodeBiasDatumRows build(
        const NetworkEstimator& est, const std::vector<Pair>& registered_code_keys) {
        this_epoch_if_pin_sel.clear();
        this_epoch_if_pin_real.clear();
        this_epoch_gf_pin_real.clear();
        this_epoch_family_gf_pin_real.clear();
        this_epoch_cb_ifpin.clear();
        this_epoch_cb_reseed.clear();
        return NetworkSbasis::buildCodeBiasDatumRows(
            est, registered_code_keys, gpsCodeFreq, cb_time_linked,
            clock_signal_basis_hold, emit_clock_disc, gf_datum,
            gf_family_constellations, code_bias_prev, code_bias_ifpin_pair,
            code_bias_sat_seen, code_bias_datum_resumed, held_clock_ref,
            this_epoch_if_pin_sel, this_epoch_if_pin_real, this_epoch_gf_pin_real,
            this_epoch_family_gf_pin_real, this_epoch_cb_ifpin, this_epoch_cb_reseed,
            basis_relatch_disc_pending,
            [](const char*, const char*, const std::string&, const std::string&,
               std::optional<double>, std::optional<double>, const char*,
               const std::string&) {});
    }
};

/// A GPS roster: @p n_sats satellites, each carrying @p sigs.
std::vector<std::string> gpsSats(int n_sats) {
    std::vector<std::string> sats;
    for (int i = 1; i <= n_sats; ++i)
        sats.push_back(i < 10 ? "G0" + std::to_string(i) : "G" + std::to_string(i));
    return sats;
}

std::vector<DatumRowsHarness::Pair> registerRoster(
    NetworkEstimator& est, const std::vector<std::string>& sats,
    const std::vector<std::string>& sigs) {
    std::vector<DatumRowsHarness::Pair> keys;
    for (const auto& sat : sats)
        for (const auto& s : sigs) {
            est.registerCodeBias(sat, s);
            keys.emplace_back(sat, s);
        }
    return keys;
}

/// How many dimensions of the rank-deficiency-3b family SURVIVE the folded rows.
///
/// The family is, per satellite s, `d^s_j -= mu_j * beta^s` on every one of that
/// satellite's code-bias columns — the shift that exactly compensates
/// `I_r^s += beta^s` at every station, and is therefore invisible to the observation
/// equations when the ionosphere is free per line of sight.  Mapping each unit `beta^s`
/// through @p H gives one column of A; the surviving dimension is `n_sats - rank(A)`.
/// Computed, not asserted: this is the card's central count.
int survivingRd3bDims(const NetworkEstimator& est, const Eigen::MatrixXd& H,
                      const std::vector<std::string>& sats,
                      const std::vector<std::string>& sigs) {
    const Eigen::Index n = static_cast<Eigen::Index>(sats.size());
    Eigen::MatrixXd A(H.rows(), n);
    A.setZero();
    for (Eigen::Index k = 0; k < n; ++k) {
        Eigen::VectorXd v = Eigen::VectorXd::Zero(est.numShared());
        for (const auto& sig : sigs) {
            Eigen::Index col = -1;
            try { col = est.codeBiasColumn(sats[static_cast<std::size_t>(k)], sig); }
            catch (...) { continue; }   // this satellite does not carry that channel
            v(col) = muRel(sigs.front(), sig);
        }
        A.col(k) = H * v;
    }
    Eigen::FullPivLU<Eigen::MatrixXd> lu(A);
    lu.setThreshold(1e-9);
    return static_cast<int>(n - lu.rank());
}

}  // namespace

// ===========================================================================
// Half 1 — the parse and the interlocks.
// ===========================================================================

// Standing invariant #6: absent knob == today's producer.  The W6 slice force-arms the
// GIM-weighted ionosphere constraint, and that must not move.
TEST(W6IonoConstraintTest, DefaultIsAutoAndKeepsTheIonoConstraintForce) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig("", kW6));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getNetworkConfig().w6_iono_constraint, NetworkW6IonoConstraint::Auto);
    EXPECT_TRUE(cfg.getIonosphereWeightingConfig().enabled);
    std::error_code ec; fs::remove(p, ec);
}

// An explicitly written "auto" is the SAME posture as an absent knob — a config that
// documents its choice must not thereby change it.
TEST(W6IonoConstraintTest, ExplicitAutoIsIdenticalToAbsent) {
    LogManager log;
    const fs::path p = writeTempConfig("auto",
        networkConfig("", std::string(kW6) + "    \"w6_iono_constraint\": \"auto\",\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getNetworkConfig().w6_iono_constraint, NetworkW6IonoConstraint::Auto);
    EXPECT_TRUE(cfg.getIonosphereWeightingConfig().enabled);
    std::error_code ec; fs::remove(p, ec);
}

// "none" must CLEAR the ionosphere-weighting force, not merely decline to apply it.
// Every RF-0 lineage writes `ionosphere_weighting.enable: true` explicitly (the loader
// forced it anyway), so a "skip the force" implementation would leave a flag reading ON
// with nothing folding behind it — standing invariant #4's exact shape.
TEST(W6IonoConstraintTest, NoneClearsAnExplicitIonoWeightingEnable) {
    LogManager log;
    const fs::path p = writeTempConfig("none",
        networkConfig(std::string(kIonoWeightOn),
                      std::string(kW6) + "    \"w6_iono_constraint\": \"none\",\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getNetworkConfig().w6_iono_constraint, NetworkW6IonoConstraint::None);
    EXPECT_FALSE(cfg.getIonosphereWeightingConfig().enabled);
    // The rest of the W6 slice is untouched — "none" changes the ionosphere posture and
    // nothing else about what the network estimates.
    EXPECT_TRUE(cfg.estimateNetworkSatCodeBias());
    EXPECT_TRUE(cfg.estimateSatBiases());
    EXPECT_TRUE(cfg.promoteSatCodeBias());
    EXPECT_TRUE(cfg.estimateIfb());
    EXPECT_FALSE(cfg.getModelCorrections().enable_code_phase_bias);
    std::error_code ec; fs::remove(p, ec);
}

// The co-estimated SH-VTEC field IS a single-layer ionosphere constraint (it is what
// arms the W6 slice at all), so "no constraint" alongside it is a contradiction.  Fail
// loud rather than letting one silently win and shipping a product whose datum nobody
// chose — the same contract network.code_bias_gf_datum has against the same knob.
TEST(W6IonoConstraintTest, NoneRefusesTheCoEstimatedShField) {
    LogManager log;
    const fs::path p = writeTempConfig("none_sh",
        networkConfig("", std::string(kW6) +
                          "    \"estimate_iono_sh\": true,\n"
                          "    \"w6_iono_constraint\": \"none\",\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

// Both remove the ionosphere constraint, so this is not a rank conflict — it is a DATUM
// ambiguity, which is worse: the run would solve and emit, with the satellite code-bias
// frame decided by branch order.  Triple frequency, so the GF datum's own dual gate
// passes and it is THIS interlock being exercised.
TEST(W6IonoConstraintTest, NoneRefusesTheGeometryFreePin) {
    LogManager log;
    const fs::path p = writeTempConfig("none_gf",
        networkConfig("    \"frequency_mode\": \"triple\",\n",
                      std::string(kW6) +
                      "    \"code_bias_gf_datum\": true,\n"
                      "    \"w6_iono_constraint\": \"none\",\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

// Without the W6 slice there are no satellite code-bias columns, hence no satellite-DCB
// rank deficiency for an ionosphere constraint to answer: the option decides nothing.
// That is a WARN, not an error — nothing is broken, it is idle — but it must load, and
// it must not reach into the ionosphere posture on its way past.
TEST(W6IonoConstraintTest, NoneWithoutTheW6SliceLoadsAndIsInert) {
    LogManager log;
    const fs::path p = writeTempConfig("none_inert",
        networkConfig("", "    \"w6_iono_constraint\": \"none\",\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getNetworkConfig().w6_iono_constraint, NetworkW6IonoConstraint::None);
    EXPECT_FALSE(cfg.estimateNetworkSatCodeBias());
    // The W6 force never ran, so the ionosphere weighting keeps whatever the config said
    // (nothing here) rather than being switched by an inert option.
    EXPECT_FALSE(cfg.getIonosphereWeightingConfig().enabled);
    std::error_code ec; fs::remove(p, ec);
}

// A typo must never select a different product datum.  Fall back to "auto" — the posture
// that needs external data and says so loudly at startup — not to "none", which would
// silently ship a differently-gauged product.
TEST(W6IonoConstraintTest, UnknownValueFallsBackToAuto) {
    LogManager log;
    const fs::path p = writeTempConfig("typo",
        networkConfig("", std::string(kW6) + "    \"w6_iono_constraint\": \"gim\",\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getNetworkConfig().w6_iono_constraint, NetworkW6IonoConstraint::Auto);
    EXPECT_TRUE(cfg.getIonosphereWeightingConfig().enabled);
    std::error_code ec; fs::remove(p, ec);
}

// ===========================================================================
// Half 2 — what the sum-to-zero datum actually reaches.
// ===========================================================================

// The builder takes no ionosphere-constraint argument at all, which is the whole reason
// RF-0b needs no new datum row: the rows the card names are already folded on every W6
// epoch, GIM or no GIM.  Lock the shape (one IF pin per satellite, one sum-to-zero row
// per (constellation, signal) channel) so a future change to the builder's arming cannot
// quietly remove the only datum this posture has.
TEST(W6IonoConstraintTest, DatumRowsAreBuiltWithoutAnyIonoConstraintInput) {
    NetworkEstimator est;
    const auto sats = gpsSats(32);
    const std::vector<std::string> sigs{"C1C", "C2W"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;                     // gf_datum = false: no geometry-free pin
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_if_pin_sats, 32);      // one ionosphere-free pin per satellite
    EXPECT_EQ(rows.n_s2z_channels, 2);      // one sum-to-zero row per (const, signal)
    EXPECT_EQ(rows.n_gf_pin_sats, 0);       // the GF pin is deliberately absent here
    EXPECT_EQ(rows.H.rows(), 34);
    EXPECT_EQ(rows.H.cols(), est.numShared());
}

// THE CARD'S CENTRAL COUNT.  With the ionosphere free per line of sight, each satellite
// owns one rank-deficiency-3b direction (Odijk 2016).  The ionosphere-free pin is
// ORTHOGONAL to it by construction (a1*mu_1 + a2*mu_2 = 0 is what "ionosphere-free"
// means), and a sum-to-zero row over signal j changes by -mu_j * sum_s beta^s, so it is
// violated ONLY by the constellation-wide shift.  Together they therefore close exactly
// ONE of the 32 directions and leave 31 open — which is what the external GIM's
// common-mode-free R was closing, and what a "none" posture must answer for by some
// other means (a prior, on today's tree network.cb_kaula_prior).
TEST(W6IonoConstraintTest, SumToZeroRowsCloseOnlyTheConstellationWideDirection) {
    NetworkEstimator est;
    const auto sats = gpsSats(32);
    const std::vector<std::string> sigs{"C1C", "C2W"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    const auto rows = h.build(est, keys);

    EXPECT_EQ(survivingRd3bDims(est, rows.H, sats, sigs), 31);
}

// The count is n_sat-1 and NOT n_signal-dependent: adding a third channel adds a third
// sum-to-zero row, but every one of them tests the same scalar sum_s beta^s, so the
// extra rows buy nothing.  (This is also the cleanest statement of why the GF datum, not
// the sum-to-zero rows, is what closes rd 3b — and why it needs a third frequency to
// leave anything estimable behind.)
TEST(W6IonoConstraintTest, ExtraChannelsDoNotCloseMoreDirections) {
    NetworkEstimator est;
    const auto sats = gpsSats(12);
    const std::vector<std::string> sigs{"C1C", "C1W", "C2W"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_s2z_channels, 3);
    EXPECT_EQ(survivingRd3bDims(est, rows.H, sats, sigs), 11);
}

// CONTRAST, so the 31 above cannot be read as "the rows are broken".  Arm the
// geometry-free pin — the OTHER answer to the same rank deficiency — and every direction
// closes.  This is the posture network.code_bias_gf_datum implements and that the loader
// refuses under dual precisely because it then leaves nothing estimable; here it is used
// only to show that the surviving-dimension instrument above responds.
TEST(W6IonoConstraintTest, TheGeometryFreePinClosesEveryDirection) {
    NetworkEstimator est;
    const auto sats = gpsSats(32);
    const std::vector<std::string> sigs{"C1C", "C2W"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    h.gf_datum = true;
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_gf_pin_sats, 32);
    EXPECT_EQ(survivingRd3bDims(est, rows.H, sats, sigs), 0);
}

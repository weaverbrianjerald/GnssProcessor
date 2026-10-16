/**
 * @file test_w6_iono_constraint.cpp
 * @brief @c network.w6_iono_constraint, the self-contained W6 posture (card RF-0b), and
 *        the datum cards built on it (RF-0c, RF-0d, RF-0e, RF-0f).
 *
 * Card: docs/refound/RF-0_baseline_2026-09-14.md, Finding 2.  Under GPS-only dual
 * frequency, without this option, the producer has no posture that avoids external
 * ionosphere data: the co-estimated SH field is off, the loader forces
 * @c ionosphere_weighting on, and the run hard-errors without a `.INX`.
 * This option adds a second posture: no ionosphere constraint at all, with the satellite
 * code-bias datum left to the rows @ref GnssProcessor::NetworkSbasis::buildCodeBiasDatumRows
 * already folds every epoch.
 *
 * The RF-0b tests come in two halves; the second carries the result.
 *
 * 1. Parse and interlocks.  The default @c auto reproduces the behaviour without the knob
 *    (standing invariant #6), @c none clears the force instead of merely skipping it,
 *    the contradictory co-arm (the SH field) fails the load, and a typo never selects a
 *    different product datum.
 * 2. The datum's reach.  The card's premise is that the per-(constellation, signal)
 *    sum-to-zero rows can stand in for the GIM.  They cannot, and the tests below
 *    compute the shortfall instead of asserting it: with the ionosphere-free pin and the
 *    sum-to-zero rows folded, exactly @c n_sat-1 of the rank-deficiency-3b directions
 *    (Odijk et al. 2016, J Geod 90:15-44) survive, because a sum-to-zero row is violated
 *    only by the constellation-wide shift and is blind to every between-satellite one.
 *    That is the structural half of the RF-0b reading.
 *
 * Later sections cover the per-satellite geometry-free datum (RF-0c), its per-signal
 * extension and S-line declaration (RF-0d), the geometry-free time link (RF-0e e2) and
 * the band-2 ISC (RF-0f).
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
#include "products/NetworkProductIO.hpp"   // the S-line declaration (RF-0d item 2)

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

/// A minimal loadable PPP-RTK network config.  @p processing_extra is spliced into the
/// processing block, @p network_extra into the network block; each needs a trailing
/// comma when non-empty (the fixed tail follows).
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

/// The W6 slice, armed as every RF-0 lineage arms it, including the explicit
/// `ionosphere_weighting.enable: true` those configs carry (the loader forces it anyway,
/// so the configs state it).  That explicit true is what "none" has to clear.
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

/// mu_j = (f_ref / f_j)^2, the ionospheric coefficient relative to @p base_sig.  The
/// rank-deficiency-3b direction is defined only up to a per-satellite scale (beta^s
/// absorbs it), so any consistent reference works for a dimension count.
double muRel(const std::string& base_sig, const std::string& sig) {
    const double f1 = gpsCodeFreq("", base_sig), fj = gpsCodeFreq("", sig);
    return (f1 * f1) / (fj * fj);
}

/// Owns the builder's side channels and clears the per-epoch scratch before each call,
/// as the driver contract requires ("cleared by caller").  Self-contained instead of
/// shared with the other suites' harnesses, following the convention of one owner per
/// suite name.
struct DatumRowsHarness {
    using Pair = std::pair<std::string, std::string>;

    bool cb_time_linked{false};
    bool emit_clock_disc{false};
    /// The hard per-satellite geometry-free datum mode and its run-latched broadcast
    /// targets (metres, `c*T_GD`) (RF-0c).  Off by default so every RF-0b case keeps
    /// measuring the un-datumed posture it was written for.
    NetworkSatGfDatum sat_gf_datum{NetworkSatGfDatum::NoneMode};
    std::map<std::string, double> sat_gf_target_m;
    /// The time link of the geometry-free rows (RF-0e e2).  Off by default so every case
    /// keeps measuring the posture it was written for.
    bool sat_gf_datum_time_linked{false};
    /// Leave the band-2 companion unpinned, and the set of (sat|signal) keys a station
    /// was found to bridge (RF-0f).  Empty means nothing is bridged, so every companion
    /// falls through to the pin, which is the unobservable case.
    bool sat_isc_state{false};
    std::set<std::string> isc_bridged;
    /// Leave an extra-band column unpinned where the data determine it, and the
    /// `"sat|signal|<b1><b2>"` determination roster the driver builds from the station
    /// state maps (B5D).  Off by default so every case keeps measuring the posture it
    /// was written for.
    bool sat_gf_extra_band_free{false};
    std::set<std::string> extra_band_determined;

    std::map<std::string, std::map<std::string, double>> code_bias_prev;
    std::map<std::string, Pair>                          code_bias_ifpin_pair;
    std::set<std::string>                                code_bias_sat_seen;
    bool                                                 code_bias_datum_resumed{false};

    std::map<std::string, Pair> this_epoch_if_pin_sel;
    std::map<std::string, Pair> this_epoch_if_pin_real;
    std::map<std::string, Pair> this_epoch_cb_ifpin;
    std::set<std::string>       this_epoch_cb_reseed;

    NetworkSbasis::CodeBiasDatumRows build(
        const NetworkEstimator& est, const std::vector<Pair>& registered_code_keys) {
        this_epoch_if_pin_sel.clear();
        this_epoch_if_pin_real.clear();
        this_epoch_cb_ifpin.clear();
        this_epoch_cb_reseed.clear();
        return NetworkSbasis::buildCodeBiasDatumRows(
            est, registered_code_keys, gpsCodeFreq, cb_time_linked, emit_clock_disc,
            sat_gf_datum, sat_gf_target_m, sat_gf_datum_time_linked,
            sat_isc_state, isc_bridged,
            sat_gf_extra_band_free, extra_band_determined,
            code_bias_prev, code_bias_ifpin_pair,
            code_bias_sat_seen, code_bias_datum_resumed,
            this_epoch_if_pin_sel, this_epoch_if_pin_real,
            this_epoch_cb_ifpin, this_epoch_cb_reseed);
    }
};

/// The smallest solution epoch the writer serializes.  The RF-0d item 2 cases only need
/// a record to exist so the S line has a file to sit in; the epoch's content is not
/// under test.
NetworkSolutionEpoch makeMinimalEpoch() {
    NetworkSolutionEpoch sol;
    sol.epoch_time  = GnssTime{2313, 259200.0};
    sol.iod_ssr     = 3;
    sol.provider_id = 42;
    sol.solution_id = 1;
    NetworkSatSolution g1;
    g1.sat                 = "G01";
    g1.has_clock           = true;
    g1.clock_c0_m          = 12.5;
    g1.code_bias_m["C1W"]  = 0.3;
    sol.sats.push_back(g1);
    return sol;
}

/// A GPS roster of @p n_sats satellite IDs, G01 upward.
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

/// How many dimensions of the rank-deficiency-3b family survive the folded rows.
///
/// The family is, per satellite s, `d^s_j -= mu_j * beta^s` on every one of that
/// satellite's code-bias columns: the shift that exactly compensates
/// `I_r^s += beta^s` at every station, and is therefore invisible to the observation
/// equations when the ionosphere is free per line of sight.  Mapping each unit `beta^s`
/// through @p H gives one column of A; the surviving dimension is `n_sats - rank(A)`.
/// The count is computed, not asserted, because it is the card's central result.
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
// Half 1: the parse and the interlocks.
// ===========================================================================

// Standing invariant #6: with the knob absent the producer is unchanged.  The W6 slice
// force-arms the GIM-weighted ionosphere constraint, and that stays so.
TEST(W6IonoConstraintTest, DefaultIsAutoAndKeepsTheIonoConstraintForce) {
    LogManager log;
    const fs::path p = writeTempConfig("default", networkConfig("", kW6));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getNetworkConfig().w6_iono_constraint, NetworkW6IonoConstraint::Auto);
    EXPECT_TRUE(cfg.getIonosphereWeightingConfig().enabled);
    std::error_code ec; fs::remove(p, ec);
}

// An explicitly written "auto" is the same posture as an absent knob: a config that
// documents its choice does not change it by doing so.
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

// "none" clears the ionosphere-weighting force, not merely declines to apply it.
// Every RF-0 lineage writes `ionosphere_weighting.enable: true` explicitly (the loader
// forces it anyway), so a "skip the force" implementation would leave a flag reading on
// with nothing folding behind it, the case standing invariant #4 describes.
TEST(W6IonoConstraintTest, NoneClearsAnExplicitIonoWeightingEnable) {
    LogManager log;
    const fs::path p = writeTempConfig("none",
        networkConfig(std::string(kIonoWeightOn),
                      std::string(kW6) + "    \"w6_iono_constraint\": \"none\",\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getNetworkConfig().w6_iono_constraint, NetworkW6IonoConstraint::None);
    EXPECT_FALSE(cfg.getIonosphereWeightingConfig().enabled);
    // The rest of the W6 slice is untouched: "none" changes the ionosphere posture and
    // nothing else about what the network estimates.
    EXPECT_TRUE(cfg.estimateNetworkSatCodeBias());
    EXPECT_TRUE(cfg.estimateSatBiases());
    EXPECT_TRUE(cfg.promoteSatCodeBias());
    EXPECT_TRUE(cfg.estimateIfb());
    EXPECT_FALSE(cfg.getModelCorrections().enable_code_phase_bias);
    std::error_code ec; fs::remove(p, ec);
}

// The co-estimated SH-VTEC field is a single-layer ionosphere constraint (it is what
// arms the W6 slice at all), so "no constraint" alongside it is a contradiction.  The
// load fails instead of letting one setting win and emitting a product whose datum was
// never chosen.
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

// Without the W6 slice there are no satellite code-bias columns, hence no satellite-DCB
// rank deficiency for an ionosphere constraint to answer: the option decides nothing.
// That is a warning, not an error (nothing is broken; the option is idle): the config
// loads, and the option leaves the ionosphere posture alone.
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

// A typo never selects a different product datum.  It falls back to "auto" (the posture
// that needs external data and reports so at startup), not to "none", which would emit
// a differently-gauged product without notice.
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
// Half 2: what the sum-to-zero datum reaches.
// ===========================================================================

// The builder takes no ionosphere-constraint argument, which is why RF-0b needs no new
// datum row: the rows the card names are already folded on every W6 epoch, with or
// without a GIM.  The shape (one IF pin per satellite, one sum-to-zero row per
// (constellation, signal) channel) is checked so a change to the builder's arming
// cannot remove the only datum this posture has.
TEST(W6IonoConstraintTest, DatumRowsAreBuiltWithoutAnyIonoConstraintInput) {
    NetworkEstimator est;
    const auto sats = gpsSats(32);
    const std::vector<std::string> sigs{"C1C", "C2W"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;                     // no geometry-free datum
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_if_pin_sats, 32);      // one ionosphere-free pin per satellite
    EXPECT_EQ(rows.n_s2z_channels, 2);      // one sum-to-zero row per (const, signal)
    EXPECT_EQ(rows.H.rows(), 34);
    EXPECT_EQ(rows.H.cols(), est.numShared());
}

// The card's central count.  With the ionosphere free per line of sight, each satellite
// owns one rank-deficiency-3b direction (Odijk 2016).  The ionosphere-free pin is
// orthogonal to it by construction (a1*mu_1 + a2*mu_2 = 0 is what "ionosphere-free"
// means), and a sum-to-zero row over signal j changes by -mu_j * sum_s beta^s, so it is
// violated only by the constellation-wide shift.  Together they therefore close exactly
// one of the 32 directions and leave 31 open.  The external GIM's common-mode-free R
// closes those 31; a "none" posture has to close them by other means (a prior, in the
// current code network.cb_kaula_prior).
TEST(W6IonoConstraintTest, SumToZeroRowsCloseOnlyTheConstellationWideDirection) {
    NetworkEstimator est;
    const auto sats = gpsSats(32);
    const std::vector<std::string> sigs{"C1C", "C2W"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    const auto rows = h.build(est, keys);

    EXPECT_EQ(survivingRd3bDims(est, rows.H, sats, sigs), 31);
}

// The count is n_sat-1 and does not depend on the number of signals: a third channel
// adds a third sum-to-zero row, but every one of them tests the same scalar
// sum_s beta^s, so the extra rows close nothing.  (This is also the clearest statement
// of why a geometry-free datum, not the sum-to-zero rows, closes rd 3b.)
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

// The n_sat-1 count holds only while every channel spans the same satellite set.  A
// channel that one satellite does not carry gives a sum-to-zero row over a different
// support, which tests a second, independent sum of the beta^s and closes one more
// direction: n_sat-2 survive.  An acceptance test that hard-codes n_sat-1 would report
// such a valid roster as a failure.  The equal-support roster is the control.
TEST(W6IonoConstraintTest, UnequalChannelSupportClosesASecondDirection) {
    const auto sats = gpsSats(3);
    const std::vector<std::string> sigs{"C1C", "C2W", "C5Q"};

    NetworkEstimator equal_est;
    const auto equal_keys = registerRoster(equal_est, sats, sigs);
    DatumRowsHarness equal_h;
    const auto equal_rows = equal_h.build(equal_est, equal_keys);
    EXPECT_EQ(equal_rows.n_s2z_channels, 3);
    EXPECT_EQ(survivingRd3bDims(equal_est, equal_rows.H, sats, sigs), 2)
        << "equal channel support leaves n_sat - 1";

    NetworkEstimator est;
    std::vector<DatumRowsHarness::Pair> keys;
    for (const auto& sat : sats)
        for (const auto& s : sigs) {
            if (sat == "G03" && s == "C5Q") continue;   // G03 does not carry L5
            est.registerCodeBias(sat, s);
            keys.emplace_back(sat, s);
        }
    DatumRowsHarness h;
    const auto rows = h.build(est, keys);
    EXPECT_EQ(rows.n_s2z_channels, 3);
    EXPECT_EQ(survivingRd3bDims(est, rows.H, sats, sigs), 1)
        << "unequal channel support closes a second direction; n_sat - 1 is not general";
}

// ===========================================================================
// The hard per-satellite geometry-free datum, network.sat_gf_datum (card RF-0c).
// ===========================================================================

// Standing invariant #6: the default builds no row at all, so an unarmed run folds
// exactly the rows RF-0b folded.
TEST(W6IonoConstraintTest, SatGfDatumDefaultBuildsNoRow) {
    LogManager log;
    const fs::path p = writeTempConfig("gf_default", networkConfig("", kW6));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getNetworkConfig().sat_gf_datum, NetworkSatGfDatum::NoneMode);
    std::error_code ec; fs::remove(p, ec);

    NetworkEstimator est;
    const auto sats = gpsSats(32);
    const std::vector<std::string> sigs{"C1C", "C2W"};
    const auto keys = registerRoster(est, sats, sigs);
    DatumRowsHarness h;                       // sat_gf_datum defaults to NoneMode
    const auto rows = h.build(est, keys);
    EXPECT_EQ(rows.n_sat_gf_rows, 0);
    EXPECT_EQ(rows.H.rows(), 34);             // 32 IF pins + 2 sum-to-zero, unchanged
}

// The card's bar, on real datum rows.  RF-0b leaves 31 of 32 rd-3b directions open with
// only the IF pin and the sum-to-zero rows; the hard geometry-free datum closes every
// one, with no prior involved, because the card replaces a prior standing in for a
// datum with an actual datum.
TEST(W6IonoConstraintTest, SatGfDatumZeroClosesEveryDirection) {
    NetworkEstimator est;
    const auto sats = gpsSats(32);
    const std::vector<std::string> sigs{"C1C", "C2W"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    h.sat_gf_datum = NetworkSatGfDatum::Zero;
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_sat_gf_rows, 32);
    // 32 IF + 32 GF.  Both sum-to-zero rows are suppressed (RF-0d): the datum pins
    // every column of both channels, so their level rows are redundant, and under
    // broadcast_tgd they would be inconsistent, so they are dropped in both modes.
    EXPECT_EQ(rows.n_s2z_suppressed, 2);
    EXPECT_EQ(rows.H.rows(), 64);
    EXPECT_EQ(rows.z.cwiseAbs().maxCoeff(), 0.0);     // every target zero in this mode
    EXPECT_EQ(survivingRd3bDims(est, rows.H, sats, sigs), 0);
}

// The same, on a roster with same-band companions (C1W beside C1C, C2X beside C2W),
// which is the field's actual shape.  The pin touches only the ionosphere-free pair, but
// the rd-3b shift moves every one of the satellite's columns by its own mu, so a single
// row on the pair still closes the whole per-satellite direction.  This has its own
// case because the opposite reading (one row needed per signal) is plausible, and it is
// the reading the L2X wire tail would suggest.
TEST(W6IonoConstraintTest, SatGfDatumClosesDirectionsWithSameBandCompanions) {
    NetworkEstimator est;
    const auto sats = gpsSats(16);
    const std::vector<std::string> sigs{"C1C", "C1W", "C2W", "C2X"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    h.sat_gf_datum = NetworkSatGfDatum::Zero;
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_sat_gf_rows, 16);
    EXPECT_EQ(rows.n_s2z_channels, 4);
    EXPECT_EQ(survivingRd3bDims(est, rows.H, sats, sigs), 0);
}

// ===========================================================================
// The per-signal extension of the geometry-free datum (card RF-0d).
// ===========================================================================

// The card's bar.  Four registered channels, the prior off.  RF-0c's pair pin already
// leaves 0 rank-deficiency-3b directions here (the mu-shift moves every column of a
// satellite together, so one row on the pair reaches it), but it does not make the extra
// channels' levels a datum, which leaves the sparse C2X wire noisy.  With RF-0d every
// cross-band column is pinned and the C2X column's own geometry-free direction against
// the reference is closed too; the same-band C1W is deliberately not pinned, because its
// difference from C1C is a pure ISC that needs no ionosphere to be observable.
TEST(W6IonoConstraintTest, SatGfDatumPerSignalPinsEveryCrossBandColumn) {
    NetworkEstimator est;
    const auto sats = gpsSats(32);
    const std::vector<std::string> sigs{"C1C", "C1W", "C2W", "C2X"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    h.sat_gf_datum = NetworkSatGfDatum::Zero;
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_sat_gf_rows, 32);          // the pair pin, one per satellite
    EXPECT_EQ(rows.n_sat_gf_extra_rows, 32);    // C2X: cross-band, pinned
    EXPECT_EQ(rows.n_sat_gf_sameband_free, 32); // C1W: same band as the reference, free
    EXPECT_EQ(rows.n_sat_gf_extra_nofreq, 0);
    // Every channel is now completely pinned except the free C1W, so three of the four
    // sum-to-zero rows are suppressed as redundant and conflicting.
    EXPECT_EQ(rows.n_s2z_suppressed, 3);
    EXPECT_EQ(rows.n_s2z_channels, 4);          // the census is over the channels present
    EXPECT_EQ(survivingRd3bDims(est, rows.H, sats, sigs), 0);
}

// The L2X column's own direction, which the card names.  It differs from the rd-3b
// family above: shift only the C2X columns by a per-satellite delta and leave every
// other column alone.  Only the stations that track C2X (a sparse set on the field)
// see it in the observation equations, so without the RF-0d rows it is weakly
// determined (a coverage problem that looks like a datum problem), and with them it is
// pinned.
TEST(W6IonoConstraintTest, SatGfDatumPerSignalClosesTheSparseL2xDirection) {
    NetworkEstimator est;
    const auto sats = gpsSats(32);
    const std::vector<std::string> sigs{"C1C", "C1W", "C2W", "C2X"};
    const auto keys = registerRoster(est, sats, sigs);

    // The direction: one unit on this satellite's C2X column, nothing else.
    const auto l2xDims = [&](const Eigen::MatrixXd& H) {
        const Eigen::Index n = static_cast<Eigen::Index>(sats.size());
        Eigen::MatrixXd A(H.rows(), n);
        A.setZero();
        for (Eigen::Index k = 0; k < n; ++k) {
            Eigen::VectorXd v = Eigen::VectorXd::Zero(est.numShared());
            v(est.codeBiasColumn(sats[static_cast<std::size_t>(k)], "C2X")) = 1.0;
            A.col(k) = H * v;
        }
        Eigen::FullPivLU<Eigen::MatrixXd> lu(A);
        lu.setThreshold(1e-9);
        return static_cast<int>(n - lu.rank());
    };

    DatumRowsHarness pair_only;                 // no GF datum, for the contrast
    pair_only.sat_gf_datum = NetworkSatGfDatum::NoneMode;
    const auto before = pair_only.build(est, keys);
    // With no geometry-free datum at all, the C2X levels are held only by their channel's
    // single sum-to-zero row: 32 directions in, 1 closed, 31 left to the data.
    EXPECT_EQ(l2xDims(before.H), 31);

    DatumRowsHarness h;
    h.sat_gf_datum = NetworkSatGfDatum::Zero;
    const auto after = h.build(est, keys);
    EXPECT_EQ(l2xDims(after.H), 0);             // every C2X column is now a datum
}

// Under broadcast_tgd the extra rows carry the same c*T_GD (the identity
// (d_k - d_ref)/(mu_k - 1) = c*T_GD holds for every band k), and each one is counted as
// an asserted zero ISC: the part of a "broadcast" datum the navigation message does not
// state.
TEST(W6IonoConstraintTest, SatGfDatumPerSignalCountsTheAssertedIscZeros) {
    NetworkEstimator est;
    const auto sats = gpsSats(3);
    const std::vector<std::string> sigs{"C1C", "C2W", "C2X"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    h.sat_gf_target_m = {{"G01", -3.1}, {"G02", 1.7}, {"G03", 2.2}};
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_sat_gf_rows, 3);
    EXPECT_EQ(rows.n_sat_gf_extra_rows, 3);
    EXPECT_EQ(rows.n_sat_gf_isc_zero, 3);
    EXPECT_EQ(rows.n_sat_gf_sameband_free, 0);
    // Row order per satellite: IF pin, pair GF, then the extra(s), so the extra row
    // carries the same target as the pair row immediately before it.
    EXPECT_DOUBLE_EQ(rows.z(1), -3.1);
    EXPECT_DOUBLE_EQ(rows.z(2), -3.1);
    EXPECT_DOUBLE_EQ(rows.z(4), 1.7);
    EXPECT_DOUBLE_EQ(rows.z(5), 1.7);
    EXPECT_EQ(rows.n_s2z_suppressed, 3);        // all three channels fully pinned
}

// broadcast_tgd pins the same row at c*T_GD.  Two things are checked: the RHS lands
// verbatim (the geometry-free combination of the IS-GPS-200 biases is exactly c*T_GD,
// with no frequency factor), and the datum still closes every direction, because a
// pin's rank is in its Jacobian and not in its target.
TEST(W6IonoConstraintTest, SatGfDatumBroadcastTgdPinsTheLatchedMetresVerbatim) {
    NetworkEstimator est;
    const auto sats = gpsSats(4);
    const std::vector<std::string> sigs{"C1C", "C2W"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    h.sat_gf_target_m = {{"G01", -3.1}, {"G02", 1.7}, {"G03", 0.0}};   // G04: no record
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_sat_gf_rows, 4);
    EXPECT_EQ(rows.n_sat_gf_tgd_hits, 2);      // G01, G02
    EXPECT_EQ(rows.n_sat_gf_tgd_zero, 1);      // G03 broadcasts exactly zero
    EXPECT_EQ(rows.n_sat_gf_tgd_missing, 1);   // G04 has no navigation record
    EXPECT_EQ(rows.n_sat_gf_pair_not_l1l2, 0);

    // The row order is interleaved, and that is the builder's contract: the
    // per-satellite loop pushes each satellite's IF pin and then its geometry-free row
    // before moving on, and any sum-to-zero rows are appended after the loop.  So
    // satellite k's GF row is at 2k+1; here both sum-to-zero rows are suppressed.
    EXPECT_EQ(rows.H.rows(), 4 * 2);        // both sum-to-zero rows suppressed (RF-0d)
    EXPECT_DOUBLE_EQ(rows.z(1), -3.1);   // G01
    EXPECT_DOUBLE_EQ(rows.z(3), 1.7);    // G02
    EXPECT_DOUBLE_EQ(rows.z(5), 0.0);    // G03: broadcast T_GD is exactly zero
    EXPECT_DOUBLE_EQ(rows.z(7), 0.0);    // G04: the fallback, counted above
    EXPECT_EQ(rows.z(0), 0.0);           // and the IF pins keep their own zero target
    EXPECT_EQ(rows.z(2), 0.0);
    EXPECT_EQ(survivingRd3bDims(est, rows.H, sats, sigs), 0);
}

// A pinned pair the navigation message never described is not given a group delay.
// Here the only two distinct frequencies are L1 and L5, so T_GD (an L1/L2 quantity) does
// not apply: the row is still folded (the datum has to be complete or the gate
// withholds), but at zero, and counted, so the run-end line can say what fraction of a
// "broadcast_tgd" product is not the physical datum.
TEST(W6IonoConstraintTest, SatGfDatumBroadcastTgdRefusesANonL1L2Pair) {
    NetworkEstimator est;
    const auto sats = gpsSats(3);
    const std::vector<std::string> sigs{"C1C", "C5Q"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    h.sat_gf_target_m = {{"G01", -3.1}, {"G02", 1.7}, {"G03", 2.2}};
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_sat_gf_rows, 3);
    EXPECT_EQ(rows.n_sat_gf_tgd_hits, 0);
    EXPECT_EQ(rows.n_sat_gf_pair_not_l1l2, 3);
    // The targets themselves are zero, not just the counters: a counted fallback that
    // still pinned the map's value would be wrong and misreported at once.
    EXPECT_EQ(rows.z.cwiseAbs().maxCoeff(), 0.0);
    EXPECT_EQ(survivingRd3bDims(est, rows.H, sats, sigs), 0);
}

// --- the interlocks ---

TEST(W6IonoConstraintTest, SatGfDatumParsesBothArmedModes) {
    LogManager log;
    for (const char* mode : {"zero", "broadcast_tgd"}) {
        const fs::path p = writeTempConfig(std::string("gf_") + mode,
            networkConfig(std::string(kIonoWeightOn),
                          std::string(kW6) +
                          "    \"w6_iono_constraint\": \"none\",\n" +
                          "    \"sat_gf_datum\": \"" + mode + "\",\n"));
        GnssConfig cfg;
        ASSERT_TRUE(cfg.loadFromJson(p, log)) << mode;
        EXPECT_EQ(cfg.getNetworkConfig().sat_gf_datum,
                  std::string(mode) == "zero" ? NetworkSatGfDatum::Zero
                                              : NetworkSatGfDatum::BroadcastTgd);
        EXPECT_FALSE(cfg.getIonosphereWeightingConfig().enabled);
        std::error_code ec; fs::remove(p, ec);
    }
}

// With an ionosphere constraint present this direction is determined by the data, so a
// pin would be a second datum conflicting with the data instead of replacing it.  The
// loader enforces the card's precondition.
TEST(W6IonoConstraintTest, SatGfDatumRequiresTheNoConstraintPosture) {
    LogManager log;
    const fs::path p = writeTempConfig("gf_needs_none",
        networkConfig("", std::string(kW6) + "    \"sat_gf_datum\": \"zero\",\n"));
    GnssConfig cfg;
    EXPECT_FALSE(cfg.loadFromJson(p, log));
    std::error_code ec; fs::remove(p, ec);
}

// A typo falls back to the posture that changes nothing, never to a datum.
TEST(W6IonoConstraintTest, SatGfDatumUnknownValueFallsBackToNone) {
    LogManager log;
    const fs::path p = writeTempConfig("gf_typo",
        networkConfig("", std::string(kW6) +
                          "    \"w6_iono_constraint\": \"none\",\n"
                          "    \"sat_gf_datum\": \"tgd\",\n"));
    GnssConfig cfg;
    ASSERT_TRUE(cfg.loadFromJson(p, log));
    EXPECT_EQ(cfg.getNetworkConfig().sat_gf_datum, NetworkSatGfDatum::NoneMode);
    std::error_code ec; fs::remove(p, ec);
}

// ===========================================================================
// The S-line declaration `sat_gf_datum=<mode>` (card RF-0d item 2).
// ===========================================================================

// A separate field, not an overload of gfpin=.  That token's payload is its presence
// ("the geometry-free direction is a datum") and a signal pair; this one states what
// presence cannot, the value the direction is pinned at, and the two possible values
// differ by c*T_GD on every emitted code bias of the satellite, with the opposite shift
// in the ionosphere the consumer estimates for itself.  It round-trips beside gfpin=,
// not inside it, and is not taken by the bare-':' realized-pivot fallback.
TEST(W6IonoConstraintTest, SatGfDatumDeclarationRoundTripsOnTheSLine) {
    const fs::path path = fs::temp_directory_path() / "w6_iono_satgfdatum_sline.txt";
    std::error_code ec; fs::remove(path, ec);

    SbasisContract c;
    c.decl.reference_receiver_id = "MBAR00UGA";
    c.disc_threshold_m           = 0.05;
    c.cb_pin_pair["G"]           = "C1C-C2W";
    c.gf_pin_pair["G"]           = "C1C-C2W";
    c.sat_gf_datum               = "broadcast_tgd";
    c.present                    = true;

    {
        NetworkProductWriter w(path.string(), c);
        ASSERT_TRUE(w.good());
        w.write(makeMinimalEpoch());
    }

    bool ok = false;
    SbasisContract parsed;
    const auto recs = NetworkProductReader::read(path.string(), &parsed, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    ASSERT_TRUE(parsed.present);
    EXPECT_EQ(parsed.sat_gf_datum, "broadcast_tgd");
    EXPECT_EQ(parsed.gf_pin_pair.at("G"), "C1C-C2W");
    EXPECT_EQ(parsed.cb_pin_pair.at("G"), "C1C-C2W");
    EXPECT_TRUE(parsed.decl.clock_pivot_sat.empty());
    fs::remove(path, ec);
}

// Byte-identity, the standing-invariant-#6 half: a producer that folds no such datum
// writes the S line exactly as it did before the field existed, so every pre-RF-0d
// product and every stored product hash still round-trips.  The armed file is the
// unarmed file plus exactly that one token, and nothing else.
TEST(W6IonoConstraintTest, SatGfDatumDeclarationAbsentWhenUnarmed) {
    const fs::path with_p    = fs::temp_directory_path() / "w6_iono_satgf_with.txt";
    const fs::path without_p = fs::temp_directory_path() / "w6_iono_satgf_without.txt";
    std::error_code ec; fs::remove(with_p, ec); fs::remove(without_p, ec);

    SbasisContract base;
    base.decl.reference_receiver_id = "MBAR00UGA";
    base.disc_threshold_m           = 0.05;
    base.gf_pin_pair["G"]           = "C1C-C2W";
    base.present                    = true;

    { NetworkProductWriter w(without_p.string(), base); ASSERT_TRUE(w.good()); }
    SbasisContract armed = base;
    armed.sat_gf_datum = "zero";
    { NetworkProductWriter w(with_p.string(), armed); ASSERT_TRUE(w.good()); }

    // Compare the S lines, not the whole files.  The '#' header legend is
    // grammar-complete by contract (HeaderLegendDeclaresEveryEmittedField enforces it),
    // so it names sat_gf_datum=<mode> in both files by design; a whole-file search
    // would find the legend and prove nothing about the record stream.
    const auto sLine = [](const fs::path& p) {
        std::ifstream f(p);
        std::string ln;
        while (std::getline(f, ln))
            if (ln.rfind("S,", 0) == 0) return ln;
        return std::string{};
    };
    const std::string a = sLine(without_p), b = sLine(with_p);
    ASSERT_FALSE(a.empty());
    ASSERT_FALSE(b.empty());
    EXPECT_EQ(a.find("sat_gf_datum="), std::string::npos);
    EXPECT_NE(b.find(",sat_gf_datum=zero"), std::string::npos);
    // The armed S line is the unarmed S line plus exactly that token, appended last.
    EXPECT_EQ(b, a + ",sat_gf_datum=zero");

    // The value survives a round-trip verbatim instead of being normalized, so a reader
    // that predates a future mode does not rewrite the product's frame.
    bool ok = false;
    SbasisContract parsed;
    NetworkProductReader::read(with_p.string(), &parsed, &ok);
    ASSERT_TRUE(ok);
    EXPECT_EQ(parsed.sat_gf_datum, "zero");
    fs::remove(with_p, ec); fs::remove(without_p, ec);
}

// ===========================================================================
// The level datum (card RF-0e), arm e2: time-linked geometry-free rows.  (Arm e1, an
// absolute per-constellation ionosphere-free sum-to-zero row, is retired.)
// ===========================================================================

// e2's bar: the change pin leaves the first-sighting anchor in force.  A satellite with
// no veteran value is still pinned at the absolute broadcast target (the only place the
// frame is asserted under this arm), while a satellite with veterans carries its own
// previous geometry-free combination instead.
TEST(W6IonoConstraintTest, SatGfTimeLinkKeepsTheFirstSightingAnchor) {
    NetworkEstimator est;
    const auto sats = gpsSats(3);
    const std::vector<std::string> sigs{"C1C", "C2W"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    h.sat_gf_datum_time_linked = true;
    h.sat_gf_target_m = {{"G01", -3.1}, {"G02", 1.7}, {"G03", 2.2}};
    // G01 and G02 have veterans; G03 is a first sighting.
    h.code_bias_prev["G|C1C"] = {{"G01", 0.40}, {"G02", -0.20}};
    h.code_bias_prev["G|C2W"] = {{"G01", 1.00}, {"G02", 0.30}};

    const auto rows = h.build(est, keys);
    EXPECT_EQ(rows.n_sat_gf_carried, 2);     // G01, G02
    EXPECT_EQ(rows.n_sat_gf_anchored, 1);    // G03 -- the anchor survives

    // Row order per satellite is IF pin then geometry-free row, so the GF rows are 1,3,5.
    const double mu = muRel("C1C", "C2W") - 1.0;
    EXPECT_NEAR(rows.z(1), (1.00 - 0.40) / mu, 1e-12);    // carried, not -3.1
    EXPECT_NEAR(rows.z(3), (0.30 + 0.20) / mu, 1e-12);    // carried, not  1.7
    EXPECT_DOUBLE_EQ(rows.z(5), 2.2);                     // anchored at c*T_GD
}

// And unarmed, e2 changes nothing: every row keeps the absolute target, so the RF-0d
// stack is reproduced exactly (standing invariant #6).
TEST(W6IonoConstraintTest, SatGfTimeLinkUnarmedKeepsTheAbsoluteTarget) {
    NetworkEstimator est;
    const auto sats = gpsSats(3);
    const std::vector<std::string> sigs{"C1C", "C2W"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    h.sat_gf_target_m = {{"G01", -3.1}, {"G02", 1.7}, {"G03", 2.2}};
    h.code_bias_prev["G|C1C"] = {{"G01", 0.40}, {"G02", -0.20}};
    h.code_bias_prev["G|C2W"] = {{"G01", 1.00}, {"G02", 0.30}};

    const auto rows = h.build(est, keys);
    EXPECT_EQ(rows.n_sat_gf_carried, 0);
    EXPECT_EQ(rows.n_sat_gf_anchored, 0);   // the counters are e2-only
    EXPECT_DOUBLE_EQ(rows.z(1), -3.1);
    EXPECT_DOUBLE_EQ(rows.z(3), 1.7);
    EXPECT_DOUBLE_EQ(rows.z(5), 2.2);
}

// ===========================================================================
// The band-2 ISC: estimated where a station bridges, pinned where none does (card RF-0f).
// ===========================================================================

// The card's first bar: estimable with one dual-tracking station.  The companion column
// gets no geometry-free row, so `b(C2X) = b(C2W) + ISC^s` with the ISC free, as it was
// before RF-0d pinned it at c*T_GD (an assertion of ISC = 0 that RINEX navigation never
// makes).  The band-1 companion stays free, and the C2W pin itself is untouched: that is
// the P-code pair the T_GD describes.
TEST(W6IonoConstraintTest, IscStateLeavesABridgedBand2CompanionFree) {
    NetworkEstimator est;
    const auto sats = gpsSats(8);
    const std::vector<std::string> sigs{"C1C", "C1W", "C2W", "C2X"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    h.sat_isc_state = true;
    for (const auto& s : sats) {
        h.sat_gf_target_m[s] = -2.0;
        h.isc_bridged.insert(s + "|C2X");     // one station tracks C2W and C2X
    }
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_isc_observed, 8);
    EXPECT_EQ(rows.n_isc_pinned, 0);
    EXPECT_EQ(rows.n_sat_gf_extra_rows, 0);      // no companion row at all
    EXPECT_EQ(rows.n_sat_gf_rows, 8);            // the P-code pair pin is unchanged
    EXPECT_EQ(rows.n_sat_gf_sameband_free, 8);   // C1W, free as before
    // The C2X column carries no geometry-free row.  Its only entry is the channel's
    // sum-to-zero level row (coefficient 1), which RF-0d's suppression logic re-enables
    // here: once the columns are not each pinned, that channel's constellation-wide
    // mean needs a datum again.  A level row is not an ISC assertion: it fixes the mean
    // and leaves every between-satellite ISC free.  Only C1C and C2W are fully pinned:
    // C1W is free (the band-1 rule) and C2X is free now, so both keep their level rows.
    EXPECT_EQ(rows.n_s2z_suppressed, 2);
    for (const auto& s : sats) {
        const Eigen::Index c = est.codeBiasColumn(s, "C2X");
        const Eigen::VectorXd col = rows.H.col(c);
        EXPECT_EQ((col.array().abs() > 1e-12).count(), 1) << s;
        EXPECT_NEAR(col.cwiseAbs().maxCoeff(), 1.0, 1e-12) << s;
    }
}

// The card's second bar: pinned without one.  No station bridges the two signals, so the
// ISC is unreachable and the column falls through to the RF-0d pin, counted so the run
// and the wire can both say which satellites' C2X is an estimate and which an assertion.
TEST(W6IonoConstraintTest, IscStatePinsAnUnbridgedBand2Companion) {
    NetworkEstimator est;
    const auto sats = gpsSats(8);
    const std::vector<std::string> sigs{"C1C", "C2W", "C2X"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness h;
    h.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    h.sat_isc_state = true;
    for (const auto& s : sats) h.sat_gf_target_m[s] = -2.0;
    h.isc_bridged.insert("G03|C2X");             // exactly one satellite is bridged
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_isc_observed, 1);           // G03
    EXPECT_EQ(rows.n_isc_pinned, 7);
    EXPECT_EQ(rows.n_sat_gf_extra_rows, 7);      // the pinned ones still get their row
    EXPECT_EQ(rows.n_sat_gf_rows, 8);            // and the P-code pin is still every satellite's
    // G03's C2X column carries only the channel sum-to-zero entry; every other
    // satellite's carries its geometry-free pin as well.  The two are told apart by the
    // row count, not by presence, because a level row is back in play for that channel.
    EXPECT_EQ((rows.H.col(est.codeBiasColumn("G03", "C2X")).array().abs() > 1e-12).count(), 1);
    EXPECT_EQ((rows.H.col(est.codeBiasColumn("G01", "C2X")).array().abs() > 1e-12).count(), 2);
}

// Standing invariant #6: unarmed, RF-0f changes nothing and its counters stay 0.  Armed
// with nothing bridged, every companion is pinned (and counted in n_isc_pinned), so the
// RF-0d stack is reproduced row for row and target for target.
TEST(W6IonoConstraintTest, IscStateUnarmedIsByteIdenticalToRf0d) {
    NetworkEstimator est;
    const auto sats = gpsSats(8);
    const std::vector<std::string> sigs{"C1C", "C2W", "C2X"};
    const auto keys = registerRoster(est, sats, sigs);

    DatumRowsHarness off;
    off.sat_gf_datum = NetworkSatGfDatum::BroadcastTgd;
    for (const auto& s : sats) off.sat_gf_target_m[s] = -2.0;
    const auto a = off.build(est, keys);

    DatumRowsHarness on = off;                   // armed, but nothing bridged...
    on.sat_isc_state = true;
    const auto b = on.build(est, keys);

    EXPECT_EQ(a.n_isc_observed, 0);
    EXPECT_EQ(a.n_isc_pinned, 0);                // the counters are RF-0f-only
    // ...so every companion is pinned and the folded stack is identical to RF-0d's.
    EXPECT_EQ(b.n_isc_pinned, 8);
    ASSERT_EQ(a.H.rows(), b.H.rows());
    EXPECT_LT((a.H - b.H).cwiseAbs().maxCoeff(), 1e-15);
    EXPECT_LT((a.z - b.z).cwiseAbs().maxCoeff(), 1e-15);
}

// The wire half: `isc=obs` is a per-record token that round-trips, and a record
// without it parses exactly as it did before the token existed.
TEST(W6IonoConstraintTest, IscObsTokenRoundTripsOnTheBRecord) {
    const fs::path path = fs::temp_directory_path() / "w6_iono_isc_token.txt";
    std::error_code ec; fs::remove(path, ec);

    SbasisContract c;
    c.decl.reference_receiver_id = "MBAR00UGA";
    c.present                    = true;

    NetworkSolutionEpoch sol = makeMinimalEpoch();
    sol.sats[0].code_bias_m["C2W"] = 1.25;
    sol.sats[0].code_bias_m["C2X"] = 1.31;
    sol.sats[0].isc_observed.insert("C2X");      // estimated; C1W/C2W carry no token
    {
        NetworkProductWriter w(path.string(), c);
        ASSERT_TRUE(w.good());
        w.write(sol);
    }

    bool ok = false;
    const auto recs = NetworkProductReader::read(path.string(), nullptr, &ok);
    ASSERT_TRUE(ok);
    ASSERT_EQ(recs.size(), 1u);
    const auto& s = recs[0].epoch.sats.at(0);
    EXPECT_EQ(s.isc_observed.count("C2X"), 1u);
    EXPECT_EQ(s.isc_observed.count("C2W"), 0u);
    EXPECT_EQ(s.isc_observed.count("C1W"), 0u);
    // The values themselves are untouched by the token.
    EXPECT_DOUBLE_EQ(s.code_bias_m.at("C2X"), 1.31);
    EXPECT_DOUBLE_EQ(s.code_bias_m.at("C2W"), 1.25);
    fs::remove(path, ec);
}

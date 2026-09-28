/**
 * @file test_family_gf_datum.cpp
 * @brief Tests for the family-GF datum: a geometry-free pin per (satellite,
 *        secondary family).
 *
 * Card: docs/SPEC_family_gf_datum.md.  Odijk 2016 rank deficiency 3b appears again per
 * receiver channel family when the station population partitions by tracking family
 * (the Galileo C/Q-vs-X split).  The primary pin is built on a satellite's two
 * lowest-ordered signals, so on a Q+X roster it is always the Q pair and no X-station
 * observation touches a pinned channel.  One mu-patterned null direction per satellite
 * remains, absorbed by the X stations' free slant ionosphere; in the field these showed
 * up as 20 undeclared near-null directions and a wire withheld at every epoch.
 *
 * The cases check three properties that are easy to get wrong and not visible in a
 * field log: the family key (it is not the RINEX attribute character), the count
 * closure (and its equal-support precondition), and the intended absence of a secondary
 * IF row (that combination is the estimable inter-family ISC, not a gauge).
 */

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkSbasis.hpp"

using namespace GnssProcessor;

namespace {

// Galileo carrier frequency by RINEX band digit, in the buildCodeBiasDatumRows
// (sat, signal) oracle shape.  E1 / E5a / E5b are the three bands the posture-C field
// arms register.  An unknown band returns 0 ("frequency unavailable"), which tells the
// builder that a signal cannot take part in a pin.
double galCodeFreq(const std::string& /*sat*/, const std::string& sig) {
    if (sig.size() < 2) return 0.0;
    switch (sig[1]) {
        case '1': return 1575.42e6;   // E1
        case '5': return 1176.45e6;   // E5a
        case '7': return 1207.14e6;   // E5b
        default:  return 0.0;
    }
}

// mu_j = (f_1 / f_j)^2, the ionospheric coefficient relative to the pair's own first
// signal, as the builder computes it; mu_1 == 1 by construction.
double muRel(const std::string& base_sig, const std::string& sig) {
    const double f1 = galCodeFreq("", base_sig), fj = galCodeFreq("", sig);
    return (f1 * f1) / (fj * fj);
}

/// The per-satellite geometry-free datum of RF-0c is outside this card: these cases
/// exercise the family extension of network.code_bias_gf_datum, so the RF-0c mode stays
/// off and its target map empty (the builder then builds no RF-0c row).
const std::map<std::string, double> kNoSatGfTargets{};
/// No companion is bridged here (RF-0f is outside this file as well).
const std::set<std::string> kNoIscBridged{};

/// Owns the builder's side channels and clears the per-epoch scratch before each call,
/// as the driver contract requires ("cleared by caller").  Separate from
/// test_network_estimator.cpp's harness, which is file-local; the suite convention is
/// one owner per suite name.
struct FamilyRowsHarness {
    using Pair = std::pair<std::string, std::string>;

    bool gf_datum{true};                    // the parent knob; the family rows are secondary to it
    bool cb_time_linked{false};
    bool clock_signal_basis_hold{false};
    bool emit_clock_disc{false};
    std::set<std::string> gf_family_constellations;   // the run-latched census verdict

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
            est, registered_code_keys, galCodeFreq, cb_time_linked,
            clock_signal_basis_hold, emit_clock_disc, gf_datum,
            NetworkSatGfDatum::NoneMode, kNoSatGfTargets,
            /*sat_if_level_datum=*/false, /*sat_gf_datum_time_linked=*/false,
            /*sat_isc_state=*/false, kNoIscBridged,
            /*sat_gf_extra_band_free=*/false, kNoIscBridged,
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

/// Register a Galileo satellite on the primary pair plus the given secondary channels.
void registerSat(NetworkEstimator& est, const std::string& sat,
                 const std::vector<std::string>& sigs) {
    for (const auto& s : sigs) est.registerCodeBias(sat, s);
}

/// The whole (sat, signal) key list in a deterministic order.
std::vector<FamilyRowsHarness::Pair> keysOf(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& roster) {
    std::vector<FamilyRowsHarness::Pair> keys;
    for (const auto& [sat, sigs] : roster)
        for (const auto& s : sigs) keys.emplace_back(sat, s);
    return keys;
}

/// Build the null-direction basis and report how many of its dimensions survive the
/// folded constraint rows.
///
/// The direction under test is, per satellite s, delta_s applied to the secondary
/// family's channels with the mu pattern: d_{s,j} = mu_j * delta_s.  Mapping each unit
/// delta through H gives one column of A; the surviving dimension is then
/// n_sats - rank(A).  This is the §1 count closure, computed rather than asserted.
int survivingNullDims(const NetworkEstimator& est, const Eigen::MatrixXd& H,
                      const std::vector<std::string>& sats,
                      const std::vector<std::string>& fam_sigs_all) {
    const Eigen::Index n = static_cast<Eigen::Index>(sats.size());
    Eigen::MatrixXd A(H.rows(), n);
    A.setZero();
    for (Eigen::Index k = 0; k < n; ++k) {
        Eigen::VectorXd v = Eigen::VectorXd::Zero(est.numShared());
        for (const auto& sig : fam_sigs_all) {
            Eigen::Index col = -1;
            try { col = est.codeBiasColumn(sats[static_cast<std::size_t>(k)], sig); }
            catch (...) { continue; }   // this satellite does not carry that channel
            v(col) = muRel(fam_sigs_all.front(), sig);
        }
        A.col(k) = H * v;
    }
    Eigen::FullPivLU<Eigen::MatrixXd> lu(A);
    lu.setThreshold(1e-9);
    return static_cast<int>(n - lu.rank());
}

}  // namespace

// ===========================================================================
// The family key, §3.1: it is not the RINEX attribute character: the Galileo
// primary family is C1C/C5Q/C7Q, whose attributes are 'C','Q','Q'.
// ===========================================================================

TEST(FamilyGfDatumTest, ChannelFamilyIsNotTheAttributeCharacter) {
    // The Galileo primary family spans different attribute characters.  Keying on the
    // character would split it in two and declare C1C a family of its own.
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C1C"),
              NetworkSbasis::ChannelFamily::Primary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C5Q"),
              NetworkSbasis::ChannelFamily::Primary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C7Q"),
              NetworkSbasis::ChannelFamily::Primary);
    // Other discrete components belong to the same family: a receiver tracking the E1
    // data component or the E5a in-phase component is not a third population.
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C1B"),
              NetworkSbasis::ChannelFamily::Primary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C5I"),
              NetworkSbasis::ChannelFamily::Primary);
    // Only combined I+Q tracking is the secondary family.
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C1X"),
              NetworkSbasis::ChannelFamily::Secondary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C5X"),
              NetworkSbasis::ChannelFamily::Secondary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("E", "C7X"),
              NetworkSbasis::ChannelFamily::Secondary);
}

TEST(FamilyGfDatumTest, GpsCodelessAndCivilChannelsAreOneFamily) {
    // Regression guard.  An attribute-character key would place C2W and C2L in different
    // families and add rows on the GPS postures, which have no family split.  Every GPS
    // channel the field arms register belongs to the primary family.
    for (const char* sig : {"C1C", "C1W", "C1P", "C1S", "C1L",
                            "C2W", "C2L", "C2S", "C2P",
                            "C5Q", "C5I"})
        EXPECT_EQ(NetworkSbasis::channelFamilyOf("G", sig),
                  NetworkSbasis::ChannelFamily::Primary)
            << "GPS " << sig << " must not be a secondary family";
}

TEST(FamilyGfDatumTest, UnrecognisedConstellationNeverPartitions) {
    // The allow-list of §7 is enforced in the mapper rather than at the call site: a
    // constellation whose family split has not been validated reports no secondary
    // family, so its census cannot partition and no row is built for it.
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("R", "C1X"),
              NetworkSbasis::ChannelFamily::Primary);
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("J", "C5X"),
              NetworkSbasis::ChannelFamily::Primary);
    // BeiDou is on the list (the expected next case), so it maps normally.
    EXPECT_EQ(NetworkSbasis::channelFamilyOf("C", "C2X"),
              NetworkSbasis::ChannelFamily::Secondary);
}

// ===========================================================================
// The census, §3.2: a bridge decides the outcome: with one, the primary pin
// reaches the secondary columns and an extra row would over-pin.
// ===========================================================================

TEST(FamilyGfDatumTest, CensusPartitionsWhenNoStationBridges) {
    const std::map<std::string, std::set<std::string>> stations{
        {"QQQ1", {"E01|C1C", "E01|C5Q", "E01|C7Q"}},
        {"QQQ2", {"E02|C1C", "E02|C5Q"}},
        {"XXX1", {"E01|C1X", "E01|C5X", "E01|C7X"}},
        {"XXX2", {"E02|C1X", "E02|C5X"}},
    };
    const auto c = NetworkSbasis::runFamilyCensus(stations);
    EXPECT_TRUE(c.latched);
    EXPECT_EQ(c.partitioned.count("E"), 1u);
    EXPECT_EQ(c.n_stations_primary, 2);
    EXPECT_EQ(c.n_stations_secondary, 2);
    EXPECT_EQ(c.n_bridges, 0);
    ASSERT_EQ(c.lines.size(), 1u);
    EXPECT_NE(c.lines[0].find("PARTITIONED"), std::string::npos);
}

TEST(FamilyGfDatumTest, CensusRefusesToPartitionWithASingleBridge) {
    // A single receiver tracking both families makes the inter-family geometry-free
    // difference observable, so the direction is no longer null and no family row may
    // be built.  A mixed-family roster takes this branch.
    std::map<std::string, std::set<std::string>> stations{
        {"QQQ1", {"E01|C1C", "E01|C5Q"}},
        {"XXX1", {"E01|C1X", "E01|C5X"}},
        {"BOTH", {"E01|C1C", "E01|C5Q", "E01|C1X", "E01|C5X"}},
    };
    const auto c = NetworkSbasis::runFamilyCensus(stations);
    EXPECT_EQ(c.partitioned.count("E"), 0u);
    EXPECT_EQ(c.n_bridges, 1);
    ASSERT_EQ(c.lines.size(), 1u);
    EXPECT_NE(c.lines[0].find("BRIDGE"), std::string::npos);
    EXPECT_EQ(c.lines[0].find("PARTITIONED"), std::string::npos);
}

TEST(FamilyGfDatumTest, CensusReportsSingleFamilyRosterAsSuch) {
    // The state of every validated posture so far.  The census still prints it
    // (standing invariant #4: an armed but inert configuration is reported as such).
    const std::map<std::string, std::set<std::string>> stations{
        {"G1", {"G01|C1C", "G01|C2W", "E01|C1C", "E01|C5Q"}},
        {"G2", {"G02|C1C", "G02|C2W", "E02|C1C", "E02|C5Q"}},
    };
    const auto c = NetworkSbasis::runFamilyCensus(stations);
    EXPECT_TRUE(c.partitioned.empty());
    EXPECT_EQ(c.n_bridges, 0);
    ASSERT_EQ(c.lines.size(), 2u);            // one per constellation, E and G
    for (const auto& l : c.lines)
        EXPECT_NE(l.find("SINGLE-FAMILY"), std::string::npos) << l;
}

// ===========================================================================
// The row, §2: weights, target, and the intended absence of an IF row.
// ===========================================================================

TEST(FamilyGfDatumTest, SecondaryFamilyGetsOneGeometryFreeRowAndNoIfRow) {
    NetworkEstimator est;
    registerSat(est, "E01", {"C1C", "C5Q", "C7Q", "C1X", "C5X", "C7X"});
    const auto keys = keysOf({{"E01", {"C1C", "C5Q", "C7Q", "C1X", "C5X", "C7X"}}});

    FamilyRowsHarness h;
    h.gf_family_constellations = {"E"};
    const auto out = h.build(est, keys);

    EXPECT_EQ(out.n_if_pin_sats, 1);
    EXPECT_EQ(out.n_gf_pin_sats, 1);          // the primary geometry-free pin
    EXPECT_EQ(out.n_family_gf_rows, 1);       // exactly one extra row for the X family
    EXPECT_EQ(out.n_family_gf_only_pair, 0);  // the X family carries C7X, a third band

    // 1 IF + 1 primary GF + 1 family GF + 6 sum-to-zero channels.
    ASSERT_EQ(out.H.rows(), 9);
    EXPECT_TRUE(out.z.isZero()) << "every datum target here is a gauge choice, not a value";

    // The family row is row 2 (per-satellite loop order: IF, primary GF, family GF).
    const Eigen::VectorXd frow = out.H.row(2);
    const double mu2_minus_mu1 = muRel("C1X", "C5X") - 1.0;
    EXPECT_NEAR(frow(est.codeBiasColumn("E01", "C1X")), -1.0 / mu2_minus_mu1, 1e-12);
    EXPECT_NEAR(frow(est.codeBiasColumn("E01", "C5X")), 1.0 / mu2_minus_mu1, 1e-12);

    // No IF row for the secondary family.  The satellite clock is shared and already
    // realized by the primary IF pin, so the X-family IF combination is the estimable
    // inter-family ISC, and pinning it would over-pin a data-determined quantity.  An IF
    // row would carry a1 = f1^2/(f1^2-f2^2) > 0 on C1X and a2 < 0 on C5X: the same two
    // columns as the GF row, with the signs reversed (the GF row is negative on C1X).
    // Require that no row in the fold has that shape on the X pair.
    const Eigen::Index c1x = est.codeBiasColumn("E01", "C1X");
    const Eigen::Index c5x = est.codeBiasColumn("E01", "C5X");
    for (Eigen::Index r = 0; r < out.H.rows(); ++r)
        EXPECT_FALSE(out.H(r, c1x) > 1e-12 && out.H(r, c5x) < -1e-12)
            << "row " << r << " is an ionosphere-free pin on the SECONDARY family";

    // The family's realized pair is recorded for the '+'-joined gfpin= declaration.
    ASSERT_EQ(h.this_epoch_family_gf_pin_real.count("E01"), 1u);
    EXPECT_EQ(h.this_epoch_family_gf_pin_real["E01"].first, "C1X");
    EXPECT_EQ(h.this_epoch_family_gf_pin_real["E01"].second, "C5X");
}

TEST(FamilyGfDatumTest, FamilyRowTargetStaysZeroUnderTimeLinking) {
    // Time linking (Card 2) re-targets the IF pin and the sum-to-zero rows to the
    // previous solved values.  The geometry-free rows (primary and family) are datum choices on a gauge
    // quantity and stay at 0 regardless (§2).
    NetworkEstimator est;
    registerSat(est, "E01", {"C1C", "C5Q", "C1X", "C5X"});
    const auto keys = keysOf({{"E01", {"C1C", "C5Q", "C1X", "C5X"}}});

    FamilyRowsHarness h;
    h.gf_family_constellations = {"E"};
    h.cb_time_linked = true;
    h.code_bias_sat_seen.insert("E01");
    h.code_bias_ifpin_pair["E01"] = {"C1C", "C5Q"};
    h.code_bias_prev["E|C1C"]["E01"] = 3.0;
    h.code_bias_prev["E|C5Q"]["E01"] = -2.0;
    h.code_bias_prev["E|C1X"]["E01"] = 7.0;
    h.code_bias_prev["E|C5X"]["E01"] = 5.0;

    const auto out = h.build(est, keys);
    ASSERT_EQ(out.n_family_gf_rows, 1);
    // Row 1 is the primary GF pin and row 2 the family GF pin; both target zero although
    // the satellite is a veteran with nonzero priors on every channel.
    EXPECT_DOUBLE_EQ(out.z(1), 0.0);
    EXPECT_DOUBLE_EQ(out.z(2), 0.0);
    EXPECT_NE(out.z(0), 0.0) << "the IF pin SHOULD be time-linked; the control is meaningless otherwise";
}

TEST(FamilyGfDatumTest, EmptyVerdictFoldsNotOneExtraRow) {
    // The byte-identity control (standing invariant #6): with the knob off or nothing
    // partitioned, the fold is unchanged.
    NetworkEstimator est;
    registerSat(est, "E01", {"C1C", "C5Q", "C1X", "C5X"});
    const auto keys = keysOf({{"E01", {"C1C", "C5Q", "C1X", "C5X"}}});

    FamilyRowsHarness on;
    on.gf_family_constellations = {"E"};
    FamilyRowsHarness off;                    // verdict empty
    const auto a = on.build(est, keys);
    const auto b = off.build(est, keys);

    EXPECT_EQ(a.H.rows(), b.H.rows() + 1);
    EXPECT_EQ(b.n_family_gf_rows, 0);
    EXPECT_TRUE(off.this_epoch_family_gf_pin_real.empty());
    // The only difference is the family row inserted at row 2; the rows before and after
    // it match the legacy rows (compared with isApprox).
    EXPECT_TRUE(a.H.topRows(2).isApprox(b.H.topRows(2)));
    EXPECT_TRUE(a.H.bottomRows(b.H.rows() - 2).isApprox(b.H.bottomRows(b.H.rows() - 2)));
}

TEST(FamilyGfDatumTest, NoFamilyRowsWithoutTheParentGfDatum) {
    // The loader rejects this combination with an error; the builder is also inert, so a
    // mis-wired call site cannot fold a secondary gauge into a posture that has no primary
    // geometry-free frame.
    NetworkEstimator est;
    registerSat(est, "E01", {"C1C", "C5Q", "C1X", "C5X"});
    const auto keys = keysOf({{"E01", {"C1C", "C5Q", "C1X", "C5X"}}});

    FamilyRowsHarness h;
    h.gf_datum = false;
    h.gf_family_constellations = {"E"};
    const auto out = h.build(est, keys);
    EXPECT_EQ(out.n_gf_pin_sats, 0);
    EXPECT_EQ(out.n_family_gf_rows, 0);
}

TEST(FamilyGfDatumTest, FamilyOnlyPairCountsTheFamilySubsetNotTheWholeSatellite) {
    // The case §2.2 names: `sigs` spans both families, so a satellite whose secondary
    // family is a bare pair still has a "third band" among its primary channels.  Counting
    // over the whole signal list would never report a family whose only estimable
    // content is the inter-family ISC.
    NetworkEstimator est;
    registerSat(est, "E01", {"C1C", "C5Q", "C7Q", "C1X", "C5X"});   // X family: two bands only
    const auto keys = keysOf({{"E01", {"C1C", "C5Q", "C7Q", "C1X", "C5X"}}});

    FamilyRowsHarness h;
    h.gf_family_constellations = {"E"};
    const auto out = h.build(est, keys);
    EXPECT_EQ(out.n_family_gf_rows, 1);
    EXPECT_EQ(out.n_family_gf_only_pair, 1);
    EXPECT_EQ(out.n_gf_only_pair, 0) << "the PRIMARY family does carry a third band (C7Q)";
}

// ===========================================================================
// The count closure (§1), computed on a small normal-equation system.
// ===========================================================================

TEST(FamilyGfDatumTest, CountClosureLeavesNSatsMinusOneWithoutTheCard) {
    // Three satellites, all carrying the full X triple.  The sum-to-zero rows span each X
    // channel over the same satellite set, so each gives mu_j * (sum of delta) = 0, the
    // same single constraint for every j.  Exactly n_sats - 1 directions survive; the
    // field estimability dump showed 21 - 1 = 20.
    NetworkEstimator est;
    const std::vector<std::string> sats{"E01", "E02", "E03"};
    std::vector<std::pair<std::string, std::vector<std::string>>> roster;
    for (const auto& s : sats) {
        registerSat(est, s, {"C1C", "C5Q", "C1X", "C5X", "C7X"});
        roster.emplace_back(s, std::vector<std::string>{"C1C", "C5Q", "C1X", "C5X", "C7X"});
    }
    const auto keys = keysOf(roster);
    const std::vector<std::string> fam{"C1X", "C5X", "C7X"};

    FamilyRowsHarness off;                    // without the card: the defect
    const auto a = off.build(est, keys);
    EXPECT_EQ(survivingNullDims(est, a.H, sats, fam), 2)
        << "expected n_sats - 1 undeclared near-null directions";

    FamilyRowsHarness on;                     // the card
    on.gf_family_constellations = {"E"};
    const auto b = on.build(est, keys);
    EXPECT_EQ(b.n_family_gf_rows, 3);                  // one per satellite
    EXPECT_EQ(b.H.rows(), a.H.rows() + 3);
    EXPECT_EQ(survivingNullDims(est, b.H, sats, fam), 0)
        << "one family GF row per satellite must close the space completely";
}

TEST(FamilyGfDatumTest, CountClosurePreconditionUnequalChannelSupport) {
    // The precondition §1 states: n_sats - 1 holds only while every family channel spans
    // the same satellite set.  Without E03's E5b (the situation of the 11 XX- stations)
    // the C7X sum-to-zero row spans a different support and imposes a second independent
    // constraint.  Only n_sats - 2 survive, so an acceptance test that hard-codes
    // n_sats - 1 would report this valid configuration as a failure.
    NetworkEstimator est;
    const std::vector<std::string> sats{"E01", "E02", "E03"};
    std::vector<std::pair<std::string, std::vector<std::string>>> roster;
    for (const auto& s : sats) {
        std::vector<std::string> sigs{"C1C", "C5Q", "C1X", "C5X"};
        if (s != "E03") sigs.push_back("C7X");
        registerSat(est, s, sigs);
        roster.emplace_back(s, sigs);
    }
    const auto keys = keysOf(roster);
    const std::vector<std::string> fam{"C1X", "C5X", "C7X"};

    FamilyRowsHarness off;
    const auto a = off.build(est, keys);
    EXPECT_EQ(survivingNullDims(est, a.H, sats, fam), 1)
        << "unequal channel support removes a second direction; n_sats - 1 is NOT general";

    FamilyRowsHarness on;
    on.gf_family_constellations = {"E"};
    const auto b = on.build(est, keys);
    EXPECT_EQ(survivingNullDims(est, b.H, sats, fam), 0);
}

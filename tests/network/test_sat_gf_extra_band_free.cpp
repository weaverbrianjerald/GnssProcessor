/**
 * @file test_sat_gf_extra_band_free.cpp
 * @brief Tests for @c network.sat_gf_extra_band_free, the band-5 code datum (card B5D).
 *
 * Card B5D: docs/refound/B5D_band5_code_datum_2026-09-20.md (requirements R1..R3, R7);
 * evidence: docs/refound/TBI_three_band_ignition_2026-09-20.md section 5.
 *
 * RF-0d pins every registered code column beyond the ionosphere-free pair through its
 * geometry-free combination at @c c*T_GD, which asserts an inter-signal correction of
 * zero.  Between same-band channels the error of that assertion is centimetres (RF-0f
 * frees the band-2 one where a station bridges it).  Across bands it is metres: the
 * estimator-free triple-code combination places the Block III satellites 4 to 5 m above
 * the Block IIF ones.  With the slant ionosphere and the clock fixed by the pinned pair,
 * the column is estimable up to one constant per (constellation, signal) family.  The
 * knob frees the column where the data determine it, and the channel's sum-to-zero row
 * supplies the datum for the constant.
 *
 * The cases exercise the pure row builder, where the datum is decided.  They check the
 * folded stack arithmetically, not through logs: the card's bars are "no assertion row
 * on a determined column", "exactly one level row per channel", "no absolute
 * broadcast-frame value in a sum-to-zero family" and "byte-identical OFF", and each is a
 * statement about @c H and @c z.
 *
 * Suite name @c SatGfExtraBandFreeTest is owned by this file alone (suite-hygiene
 * convention: one suite name, one file).  The harness below is separate from the
 * RF-0b/RF-0f suite's on purpose.
 */

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "config/GnssConfigTypes.hpp"
#include "network/solve/Estimator.hpp"
#include "network/solve/Sbasis.hpp"

using namespace GnssProcessor;

namespace {

/// GPS carrier frequency by RINEX band digit, in the builder's (sat, signal) oracle
/// shape.  An unknown band returns 0, which is how the builder is told a signal cannot
/// take part in a pin.
double gpsCodeFreq(const std::string& /*sat*/, const std::string& sig) {
    if (sig.size() < 2) return 0.0;
    switch (sig[1]) {
        case '1': return 1575.42e6;   // L1
        case '2': return 1227.60e6;   // L2
        case '5': return 1176.45e6;   // L5
        default:  return 0.0;
    }
}

/// mu_k = (f_ref / f_k)^2 relative to @p base_sig.  The geometry-free row divides by
/// `mu_k - 1`, so 1/(mu_k - 1) is the coefficient the assertion row carries.
double muRel(const std::string& base_sig, const std::string& sig) {
    const double f1 = gpsCodeFreq("", base_sig), fk = gpsCodeFreq("", sig);
    return (f1 * f1) / (fk * fk);
}

/// Owns the builder's side channels and clears the per-epoch scratch before each call,
/// as the driver contract requires ("cleared by caller").
struct Harness {
    using Pair = std::pair<std::string, std::string>;

    bool cb_time_linked{true};        ///< on in RF-0h lineages (code_datum_time_linked).
    bool emit_clock_disc{false};
    NetworkSatGfDatum sat_gf_datum{NetworkSatGfDatum::BroadcastTgd};
    std::map<std::string, double> sat_gf_target_m;
    bool sat_gf_datum_time_linked{true};   ///< and sat_gf_datum_time_linked (RF-0e e2).
    bool sat_isc_state{false};
    std::set<std::string> isc_bridged;
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

std::vector<std::string> gpsSats(int n_sats) {
    std::vector<std::string> sats;
    for (int i = 1; i <= n_sats; ++i)
        sats.push_back(i < 10 ? "G0" + std::to_string(i) : "G" + std::to_string(i));
    return sats;
}

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

/// The rows that touch one column, with their coefficients.  The card's bars are
/// statements about this list.
std::vector<std::pair<Eigen::Index, double>> rowsOn(const Eigen::MatrixXd& H,
                                                    Eigen::Index col) {
    std::vector<std::pair<Eigen::Index, double>> out;
    for (Eigen::Index r = 0; r < H.rows(); ++r)
        if (std::abs(H(r, col)) > 1e-12) out.emplace_back(r, H(r, col));
    return out;
}

/// The determination roster the driver would build for a station that carries all three
/// codes of every satellite, for the (band-1, band-2) pin pair the builder realizes here.
std::set<std::string> determinedAll(const std::vector<std::string>& sats,
                                    const std::string& extra_sig) {
    std::set<std::string> out;
    for (const auto& s : sats) out.insert(s + "|" + extra_sig + "|12");
    return out;
}

}  // namespace

// ===========================================================================
// With the knob off, the rows and targets are RF-0d's (R1).
// ===========================================================================

// Standing invariant #6.  With the knob off nothing changes: the band-5 columns keep
// their geometry-free assertion rows, the channel keeps its suppression, and the
// extra-band counters stay at zero.  The check is an exact matrix comparison, because
// the field arms rely on "byte-identical OFF".
TEST(SatGfExtraBandFreeTest, UnarmedIsIdenticalToRf0d) {
    NetworkEstimator est;
    const auto sats = gpsSats(8);
    const std::vector<std::string> sigs{"C1W", "C2W", "C5X"};
    const auto keys = registerRoster(est, sats, sigs);

    Harness off;
    for (const auto& s : sats) off.sat_gf_target_m[s] = -2.0;
    const auto a = off.build(est, keys);

    // Second build: knob still off, but with the full determination roster supplied.
    // With the knob off the roster is ignored, so the rows match the first build.
    Harness on = off;
    on.sat_gf_extra_band_free = false;
    on.extra_band_determined  = determinedAll(sats, "C5X");
    const auto b = on.build(est, keys);

    EXPECT_EQ(a.n_extra_band_free, 0);
    EXPECT_EQ(a.n_extra_band_carried, 0);
    EXPECT_EQ(a.n_extra_band_held, 0);
    EXPECT_EQ(a.n_sat_gf_extra_rows, 8);     // one assertion per satellite, as in RF-0d
    EXPECT_EQ(a.n_sat_gf_isc_zero, 8);
    ASSERT_EQ(a.H.rows(), b.H.rows());
    EXPECT_LT((a.H - b.H).cwiseAbs().maxCoeff(), 1e-15);
    EXPECT_LT((a.z - b.z).cwiseAbs().maxCoeff(), 1e-15);
    EXPECT_TRUE(a.extra_band_held.empty());
}

// ===========================================================================
// A determined extra-band column gets no assertion row and one level row (R2).
// ===========================================================================

// The card's first bar.  A column the data determine gets no geometry-free row.  The
// channel's sum-to-zero row, which RF-0d suppresses once every column of a channel is
// pinned, returns because a freed column is not in the pinned set.  There is exactly one
// such row per (constellation, signal) channel: the data do not determine the level, and
// the level needs one datum, not n.
TEST(SatGfExtraBandFreeTest, DeterminedExtraBandColumnGetsNoAssertionAndOneLevelRow) {
    NetworkEstimator est;
    const auto sats = gpsSats(8);
    const std::vector<std::string> sigs{"C1W", "C2W", "C5X"};
    const auto keys = registerRoster(est, sats, sigs);

    Harness h;
    h.sat_gf_extra_band_free = true;
    h.extra_band_determined  = determinedAll(sats, "C5X");
    for (const auto& s : sats) h.sat_gf_target_m[s] = -2.0;
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_extra_band_free, 8);
    EXPECT_EQ(rows.n_extra_band_carried, 0);
    EXPECT_EQ(rows.n_extra_band_held, 0);
    EXPECT_EQ(rows.n_sat_gf_extra_rows, 0);   // no assertion row anywhere
    EXPECT_EQ(rows.n_sat_gf_isc_zero, 0);
    EXPECT_EQ(rows.n_sat_gf_rows, 8);         // the pair pin itself is unchanged

    // The pinned pair's two channels are still fully pinned, so their sum-to-zero rows
    // stay suppressed; C5X's does not.
    EXPECT_EQ(rows.n_s2z_channels, 3);
    EXPECT_EQ(rows.n_s2z_suppressed, 2);

    // Every C5X column carries exactly one row, with coefficient +1 (the channel level),
    // not the assertion row's 1/(mu_5 - 1).
    const double assertion_coeff = 1.0 / (muRel("C1W", "C5X") - 1.0);
    int level_rows_seen = 0;
    std::set<Eigen::Index> level_row_ids;
    for (const auto& s : sats) {
        const auto on = rowsOn(rows.H, est.codeBiasColumn(s, "C5X"));
        ASSERT_EQ(on.size(), 1u) << s;
        EXPECT_NEAR(on[0].second, 1.0, 1e-12) << s;
        EXPECT_GT(std::abs(on[0].second - assertion_coeff), 1e-3) << s;
        level_row_ids.insert(on[0].first);
        ++level_rows_seen;
    }
    EXPECT_EQ(level_rows_seen, 8);
    EXPECT_EQ(level_row_ids.size(), 1u);   // one shared row: the channel's level datum

    // On a first epoch with no veterans that row's RHS is 0, the sum-to-zero seed, not
    // the broadcast target (R3's frame statement, checked on the level row).
    EXPECT_DOUBLE_EQ(rows.z(*level_row_ids.begin()), 0.0);
}

// A two-family roster (as in the field: C5Q at one group of stations, C5X at another)
// gauges to full rank with two level rows, one per family.  The family constant is per
// (constellation, signal), so two channels need two datums and neither may use the
// other's.
TEST(SatGfExtraBandFreeTest, TwoExtraBandFamiliesGaugeWithOneLevelRowEach) {
    NetworkEstimator est;
    const auto sats = gpsSats(6);
    // Half the satellites carry C5Q and half C5X, a small version of the field's split roster.
    std::vector<Harness::Pair> keys;
    for (size_t i = 0; i < sats.size(); ++i) {
        const std::string extra = (i % 2 == 0) ? "C5Q" : "C5X";
        for (const auto& s : {std::string("C1W"), std::string("C2W"), extra}) {
            est.registerCodeBias(sats[i], s);
            keys.emplace_back(sats[i], s);
        }
    }

    Harness h;
    h.sat_gf_extra_band_free = true;
    for (size_t i = 0; i < sats.size(); ++i) {
        h.sat_gf_target_m[sats[i]] = -2.0;
        h.extra_band_determined.insert(sats[i] + "|" + ((i % 2 == 0) ? "C5Q" : "C5X")
                                       + "|12");
    }
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_extra_band_free, 6);
    EXPECT_EQ(rows.n_sat_gf_extra_rows, 0);
    EXPECT_EQ(rows.n_s2z_channels, 4);     // C1W, C2W, C5Q, C5X
    EXPECT_EQ(rows.n_s2z_suppressed, 2);   // the pinned pair's two

    // Two level rows, and they are disjoint: no satellite's C5Q column appears in the
    // C5X row.  A shared row would gauge the difference between the two families, the
    // between-family constant that nothing in the data determines.
    std::set<Eigen::Index> q_rows, x_rows;
    for (size_t i = 0; i < sats.size(); ++i) {
        const std::string extra = (i % 2 == 0) ? "C5Q" : "C5X";
        const auto on = rowsOn(rows.H, est.codeBiasColumn(sats[i], extra));
        ASSERT_EQ(on.size(), 1u) << sats[i];
        EXPECT_NEAR(on[0].second, 1.0, 1e-12) << sats[i];
        (extra == "C5Q" ? q_rows : x_rows).insert(on[0].first);
    }
    ASSERT_EQ(q_rows.size(), 1u);
    ASSERT_EQ(x_rows.size(), 1u);
    EXPECT_NE(*q_rows.begin(), *x_rows.begin());
}

// ===========================================================================
// An undetermined column never brings the broadcast frame into the family (R3).
// ===========================================================================

// Undetermined with a previous value: the row carries that column's own previous
// geometry-free combination, not `c*T_GD`.  The family stands in the sum-to-zero frame;
// an absolute broadcast target on one of its columns would put two frames on the same
// family at one gauge weight (RF-0d's conflict, in the opposite direction).
TEST(SatGfExtraBandFreeTest, UndeterminedColumnWithAVeteranCarriesItsOwnCombination) {
    NetworkEstimator est;
    const auto sats = gpsSats(4);
    const std::vector<std::string> sigs{"C1W", "C2W", "C5X"};
    const auto keys = registerRoster(est, sats, sigs);

    Harness h;
    h.sat_gf_extra_band_free = true;   // the roster is empty: nothing is determined
    const double tgd = -2.0;
    for (const auto& s : sats) {
        h.sat_gf_target_m[s] = tgd;
        h.code_bias_sat_seen.insert(s);
        h.code_bias_prev["G|C1W"][s] = 0.4;
        h.code_bias_prev["G|C2W"][s] = 1.1;
        h.code_bias_prev["G|C5X"][s] = 3.7;    // a real, metre-scale band-5 bias
    }
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_extra_band_free, 0);
    EXPECT_EQ(rows.n_extra_band_carried, 4);
    EXPECT_EQ(rows.n_extra_band_held, 0);
    EXPECT_EQ(rows.n_sat_gf_extra_rows, 4);   // the rows exist; their target changed
    EXPECT_TRUE(rows.extra_band_held.empty());

    // The carried target is (b5_prev - b1_prev) / (mu_5 - 1), and it is nowhere near the
    // broadcast c*T_GD the unarmed build would have folded.
    const double mu5 = muRel("C1W", "C5X");
    const double want = (3.7 - 0.4) / (mu5 - 1.0);
    const auto on = rowsOn(rows.H, est.codeBiasColumn(sats[0], "C5X"));
    ASSERT_EQ(on.size(), 1u);
    EXPECT_NEAR(rows.z(on[0].first), want, 1e-9);
    EXPECT_GT(std::abs(want - tgd), 1.0);
}

// Undetermined at a first sighting: nothing to carry, so the column is held by a single
// row in the channel's own frame (coefficient +1, target 0), and its key is returned for
// the emit prep to withhold.  The geometry-free assertion row is not used, because it
// would put `c*T_GD` into a family that the sum-to-zero row is levelling.
TEST(SatGfExtraBandFreeTest, UndeterminedFirstSightingIsHeldInTheChannelFrameAndWithheld) {
    NetworkEstimator est;
    const auto sats = gpsSats(4);
    const std::vector<std::string> sigs{"C1W", "C2W", "C5X"};
    const auto keys = registerRoster(est, sats, sigs);

    Harness h;
    h.sat_gf_extra_band_free = true;
    for (const auto& s : sats) h.sat_gf_target_m[s] = -2.0;
    // G04 alone is determined; the other three are first sightings with nothing carried.
    h.extra_band_determined.insert("G04|C5X|12");
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_extra_band_free, 1);
    EXPECT_EQ(rows.n_extra_band_carried, 0);
    EXPECT_EQ(rows.n_extra_band_held, 3);
    EXPECT_EQ(rows.n_sat_gf_extra_rows, 0);   // no assertion row was folded for any of them
    EXPECT_EQ(rows.n_sat_gf_isc_zero, 0);
    EXPECT_EQ(rows.extra_band_held,
              (std::set<std::string>{"G01|C5X", "G02|C5X", "G03|C5X"}));

    // The channel keeps its level row (nothing pinned it), so a held column appears in
    // two rows: the shared level row and its own hold.  Both have coefficient +1 and
    // target 0, in the channel's own frame, not the broadcast one.
    for (const auto& s : {"G01", "G02", "G03"}) {
        const auto on = rowsOn(rows.H, est.codeBiasColumn(s, "C5X"));
        ASSERT_EQ(on.size(), 2u) << s;
        for (const auto& [r, c] : on) {
            EXPECT_NEAR(c, 1.0, 1e-12) << s;
            EXPECT_DOUBLE_EQ(rows.z(r), 0.0) << s;
        }
    }
    // The determined one carries the level row alone.
    EXPECT_EQ(rowsOn(rows.H, est.codeBiasColumn("G04", "C5X")).size(), 1u);
    EXPECT_EQ(rows.n_s2z_suppressed, 2);
}

// The knob affects the extra band only.  RF-0d's band-1 same-band rule and the pin
// pair's own rows are unchanged, and a same-band companion on the pin's second band is
// still decided by RF-0f's rule, not this one.  The three rules must not overlap.
TEST(SatGfExtraBandFreeTest, SameBandCompanionsAreUntouchedByTheExtraBandRule) {
    NetworkEstimator est;
    const auto sats = gpsSats(4);
    const std::vector<std::string> sigs{"C1C", "C1W", "C2W", "C2X", "C5X"};
    const auto keys = registerRoster(est, sats, sigs);

    Harness h;
    h.sat_gf_extra_band_free = true;
    h.extra_band_determined  = determinedAll(sats, "C5X");
    for (const auto& s : sats) h.sat_gf_target_m[s] = -2.0;
    const auto rows = h.build(est, keys);

    EXPECT_EQ(rows.n_extra_band_free, 4);          // C5X only
    EXPECT_EQ(rows.n_sat_gf_sameband_free, 4);     // C1W, free as RF-0d left it
    EXPECT_EQ(rows.n_sat_gf_extra_rows, 4);        // C2X still asserted (sat_isc_state off)
    EXPECT_EQ(rows.n_isc_observed, 0);
    EXPECT_EQ(rows.n_isc_pinned, 0);
    EXPECT_EQ(rows.n_sat_gf_rows, 4);              // the pair pin, one per satellite

    // C2X keeps its geometry-free assertion coefficient; C5X has only the level row.
    const double c2x_coeff = 1.0 / (muRel("C1C", "C2X") - 1.0);
    const auto c2x = rowsOn(rows.H, est.codeBiasColumn("G01", "C2X"));
    ASSERT_EQ(c2x.size(), 1u);
    EXPECT_NEAR(c2x[0].second, c2x_coeff, 1e-9);
    const auto c5x = rowsOn(rows.H, est.codeBiasColumn("G01", "C5X"));
    ASSERT_EQ(c5x.size(), 1u);
    EXPECT_NEAR(c5x[0].second, 1.0, 1e-12);
}

// ===========================================================================
// A re-reference of the freed channel's level is an event at both edges (R5).
// ===========================================================================

// The freed columns stand on the veteran-linked sum-to-zero row.  When the satellite set
// changes so that no veteran is present, that row re-seeds: the channel's level
// re-references, every satellite seen before is marked for the consumer's discontinuity
// channel, and the marks are counted.  Standing invariant #7: a mechanism that moves a
// frame is an event at both edges, and the counters separate a re-reference that did not
// happen from one that was not instrumented.
TEST(SatGfExtraBandFreeTest, LevelReseedOnASetChangeIsMarkedAndCounted) {
    NetworkEstimator est;
    const auto sats = gpsSats(3);
    const std::vector<std::string> sigs{"C1W", "C2W", "C5X"};
    const auto keys = registerRoster(est, sats, sigs);

    Harness h;
    h.sat_gf_extra_band_free = true;
    h.extra_band_determined  = determinedAll(sats, "C5X");
    for (const auto& s : sats) {
        h.sat_gf_target_m[s] = -2.0;
        h.code_bias_sat_seen.insert(s);          // all three are veterans of the run
        h.code_bias_prev["G|C1W"][s] = 0.4;
        h.code_bias_prev["G|C2W"][s] = 1.1;
        // Their ionosphere-free pin carries cleanly (same pair, both values), so the
        // Card-2 IF-pin re-seed adds no mark and the set below comes from the sum-to-zero
        // re-reference alone.
        h.code_bias_ifpin_pair[s] = {"C1W", "C2W"};
    }
    // None of them has a previous C5X value: the C5X channel's veteran link has nothing
    // to reference, so its level row re-seeds at 0.
    const auto reseeded = h.build(est, keys);
    EXPECT_EQ(reseeded.n_s2z_reseed_rows, 1);        // the C5X channel alone
    EXPECT_EQ(reseeded.n_s2z_reseed_sats, 3);
    EXPECT_EQ(h.this_epoch_cb_reseed,
              (std::set<std::string>{"G01", "G02", "G03"}));

    // With the channel's veterans present the row carries instead and nothing is marked,
    // which shows the counter does not fire unconditionally.
    Harness carried = h;
    for (const auto& s : sats) carried.code_bias_prev["G|C5X"][s] = 3.7;
    const auto rows = carried.build(est, keys);
    EXPECT_EQ(rows.n_s2z_reseed_rows, 0);
    EXPECT_EQ(rows.n_s2z_reseed_sats, 0);
    EXPECT_TRUE(carried.this_epoch_cb_reseed.empty());

    // The carried level row's RHS is the sum over the veterans, so the level is carried
    // rather than re-zeroed.
    const auto on = rowsOn(rows.H, est.codeBiasColumn("G01", "C5X"));
    ASSERT_EQ(on.size(), 1u);
    EXPECT_NEAR(rows.z(on[0].first), 3.0 * 3.7, 1e-9);
}

// ===========================================================================
// The loader: the knob's default.
// ===========================================================================

// The knob defaults to off (R1 at the config layer).  There is no getter (as with
// sat_isc_state, the network sub-struct is read directly), so the test reads the member.
TEST(SatGfExtraBandFreeTest, ConfigDefaultIsOff) {
    NetworkConfig nc;
    EXPECT_FALSE(nc.sat_gf_extra_band_free);
}

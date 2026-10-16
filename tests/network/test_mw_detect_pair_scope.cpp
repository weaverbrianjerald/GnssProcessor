/**
 * @file test_mw_detect_pair_scope.cpp
 * @brief Contract tests for `network.cycle_slip.mw_detect_pairs` (card L5P,
 *        docs/refound/L5P_l5_phase_posture_2026-09-21.md knob A).
 *
 * The network's Melbourne-Wubbena detector forms one pair per distinct band pair
 * of a satellite's arc.  On a three-band lineage (card B5D) two of the three pairs
 * carry band 5, and those pairs account for most `mw_wl_slip` detections.
 * `primary` forms only the pair whose two signals both sit on the constellation's
 * ionosphere-free pin bands; on a lineage that latches one signal per band, that
 * is the station's latched band-1 / band-2 pair.
 *
 * Contract checked here:
 *   (a) @c networkMwPairInPrimaryScope admits the pin pair in either signal order
 *       and refuses any pair carrying a band outside it;
 *   (b) an unresolved (or degenerate) pin pair admits every pair: the scope
 *       cannot be applied without the pin bands, and dropping every pair would
 *       silence the detector instead of narrowing it;
 *   (c) the tracking attribute plays no part: the scope is a band decision, so
 *       L1C/L2W and L1W/L2W are both the primary pair;
 *   (d) a Galileo-shaped pin pair (bands 1 and 5) scopes to E1/E5a and refuses
 *       the band-7 and band-2 pairs, so nothing assumes band 5 is the extra one;
 *   (e) a malformed signal code is admitted, so the scope never drops a pair it
 *       cannot parse.
 *
 * The predicate is pure (no config, logger or product cache), so these cases need
 * no fixture.  Whether a run applies it is shown by the run's
 * "card L5P, MW pair scope" run-end line and the `mw_wl_slip` event population,
 * which are graded on the run arms, not here.
 */
#include <gtest/gtest.h>

#include "network/assembly/NetworkExtraBandScope.hpp"

#include <string>

using GnssProcessor::NetworkPinBands;
using GnssProcessor::networkMwPairInPrimaryScope;

namespace {

/// The GPS pin pair of the RF-0hv posture: the BIA clock reference is C1W/C2W.
constexpr NetworkPinBands kGps{ '1', '2' };
/// A Galileo-shaped pin pair: E1/E5a.
constexpr NetworkPinBands kGal{ '1', '5' };
/// Nothing resolved (no clock-reference entry for the constellation).
constexpr NetworkPinBands kNone{};

}  // namespace

// --------------------------------------------------------------------------
// (a) the primary pair is admitted, every extra-band pair is refused
// --------------------------------------------------------------------------

TEST(MwDetectPairScope, AdmitsThePinPairInEitherOrder) {
    EXPECT_TRUE(networkMwPairInPrimaryScope("L1C", "L2W", kGps));
    EXPECT_TRUE(networkMwPairInPrimaryScope("L2W", "L1C", kGps));
}

TEST(MwDetectPairScope, RefusesEveryPairCarryingAnExtraBand) {
    // The two band-5 pairs of the three-band arc, in the signal combinations that
    // appear in detection records (L2W;L5Q, L2W;L5X, L1C;L5Q, L1W;L5X).
    EXPECT_FALSE(networkMwPairInPrimaryScope("L1C", "L5Q", kGps));
    EXPECT_FALSE(networkMwPairInPrimaryScope("L2W", "L5Q", kGps));
    EXPECT_FALSE(networkMwPairInPrimaryScope("L2W", "L5X", kGps));
    EXPECT_FALSE(networkMwPairInPrimaryScope("L1W", "L5X", kGps));
    // The extra band listed first, to show the result does not depend on order.
    EXPECT_FALSE(networkMwPairInPrimaryScope("L5Q", "L1C", kGps));
}

TEST(MwDetectPairScope, RefusesAPairWithBothSignalsOffThePinBands) {
    // Not reachable on a GPS L1/L2/L5 arc; on a constellation with four tracked
    // bands the rule still refuses it.
    EXPECT_FALSE(networkMwPairInPrimaryScope("L5Q", "L7Q", kGps));
}

// --------------------------------------------------------------------------
// (b) an unresolved pin pair changes nothing
// --------------------------------------------------------------------------

TEST(MwDetectPairScope, UnresolvedPinAdmitsEveryPair) {
    EXPECT_FALSE(kNone.valid());
    EXPECT_TRUE(networkMwPairInPrimaryScope("L1C", "L2W", kNone));
    EXPECT_TRUE(networkMwPairInPrimaryScope("L2W", "L5Q", kNone));
}

TEST(MwDetectPairScope, ADegeneratePinPairIsTreatedAsUnresolved) {
    // Two clock-reference signals on the same band do not form a pin pair, so the
    // predicate treats the pin as unresolved rather than reading band 2 as extra.
    constexpr NetworkPinBands same{ '1', '1' };
    EXPECT_FALSE(same.valid());
    EXPECT_TRUE(networkMwPairInPrimaryScope("L1C", "L2W", same));
    EXPECT_TRUE(networkMwPairInPrimaryScope("L1C", "L5Q", same));
}

// --------------------------------------------------------------------------
// (c) / (d) bands, never attributes; and no constellation is hard-coded
// --------------------------------------------------------------------------

TEST(MwDetectPairScope, TheTrackingAttributeIsNotPartOfTheTest) {
    // A station latched to L1W and one latched to L1C both have a primary pair.
    EXPECT_TRUE(networkMwPairInPrimaryScope("L1W", "L2W", kGps));
    EXPECT_TRUE(networkMwPairInPrimaryScope("L1C", "L2L", kGps));
    EXPECT_TRUE(networkMwPairInPrimaryScope("L1X", "L2X", kGps));
}

TEST(MwDetectPairScope, AGalileoShapedPinPairScopesToItsOwnBands) {
    EXPECT_TRUE(networkMwPairInPrimaryScope("L1C", "L5Q", kGal));   // E1 / E5a
    EXPECT_FALSE(networkMwPairInPrimaryScope("L1C", "L7Q", kGal));  // E1 / E5b
    EXPECT_FALSE(networkMwPairInPrimaryScope("L5Q", "L7Q", kGal));
    // Band 2 is the extra band here; a rule that hard-coded band 5 as the extra
    // band would admit this pair.
    EXPECT_FALSE(networkMwPairInPrimaryScope("L1C", "L2W", kGal));
}

// --------------------------------------------------------------------------
// (e) malformed input
// --------------------------------------------------------------------------

TEST(MwDetectPairScope, MalformedCodesAreNeverActedOn) {
    EXPECT_TRUE(networkMwPairInPrimaryScope("", "L2W", kGps));
    EXPECT_TRUE(networkMwPairInPrimaryScope("L1C", "", kGps));
    EXPECT_TRUE(networkMwPairInPrimaryScope("L", "L2W", kGps));
}

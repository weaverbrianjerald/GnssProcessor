/**
 * @file test_station_births.cpp
 * @brief The births of a network station's local parameters (network/assembly/LocalBirth.hpp),
 *        the admission of its shared satellite parameters (SharedAdmission.hpp) and the
 *        ambiguity basis of its rows (StationRows.hpp), one family at a time.
 *
 * Each case drives one station with synthetic observations and reads the station's parameter
 * set.  The cases cover what a birth or a gate does that the first-epoch births of
 * test_station_params.cpp do not arm:
 *  - the phase colour: born at 0 with the elevation of its line of sight, kept across epochs
 *    with the elevation refreshed, pruned with the satellite's phase;
 *  - the first-epoch seeds: the inter-system biases retry until the time reference has code,
 *    the cold receiver clock seeds once and then latches, a clock that is not cold is left;
 *  - the receiver code biases: one per key the bias rule names, the GLONASS network keeping
 *    only the per-channel bias, a key pruned with the signal that needed it;
 *  - the receiver code-bias seeds: the median of the band differences, the warm resume's
 *    installed points taking a covered key and counting the ones that cannot be used;
 *  - the shared admission: the coverage allow-list, the GLONASS code-only scope, the satellite
 *    code bias, the prune of a satellite that leaves, and the geometry gates that agree with
 *    the row loop;
 *  - the rows: the network's rows have one ambiguity per band whatever the configuration says
 *    about the single-receiver filter's layout.
 */

#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "common/PppUtils.hpp"
#include "network/assembly/LocalBirth.hpp"
#include "network/assembly/SharedAdmission.hpp"
#include "network/assembly/StationRows.hpp"
#include "network/state/StationParams.hpp"
#include "../support/StationBirthFixture.hpp"
#include "../support/StationParamsBuilder.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace GnssProcessor;
using gnss_test::BirthFixture;
using gnss_test::BirthOptions;
using gnss_test::dcbKey;
using gnss_test::ifbKey;
using gnss_test::satOf;
using gnss_test::signalOf;
using gnss_test::slotIfbKey;
using gnss_test::syntheticObservation;

namespace {

/// The network members that arm the per-line-of-sight phase colour.
const char* const kPhaseColourPosture = R"(,
    "ambiguity_vehicle": "float_kf", "phase_leveling": true, "carry_conditional_coupling": true,
    "shared_carry_full_cov": true, "phase_colour_state": true)";

/// The slot of a typed parameter, or -1.
int slotOf(const StationParams& set, const ParamRef& ref) { return findParam(set, ref); }

/// The linearization point of a typed parameter.
double valueOf(const StationParams& set, const ParamRef& ref) {
    const int k = findParam(set, ref);
    EXPECT_GE(k, 0) << keyText(ref);
    return k < 0 ? std::numeric_limits<double>::quiet_NaN() : slotAt(set, k).x_lin;
}

/// A GPS observation with the given codes (no phase), at epoch @p k.
PppObservation gpsCodes(const std::string& sat, const std::vector<std::string>& codes, int k = 0,
                        double azimuth = 40.0, double elevation = 50.0) {
    return syntheticObservation(sat, System::GPS, codes, k, azimuth, elevation);
}

/// The reference pair the bias product names for GPS.
ClockRefSignals gpsClockPair() { return ClockRefSignals{"C1W", "C2W"}; }

/// The phase-bias slots a set holds, as "<sat>_<signal>" texts.
std::set<std::string> phaseBiasTexts(const StationParams& set) {
    std::set<std::string> out;
    for (const auto& kv : set.shared.sat_phase_bias)
        out.insert(kv.first.sat.str() + "_" + kv.first.signal.obsCode(ObsType::Phase));
    return out;
}

/// The state-key texts of the shared parameters the gates withheld this epoch.
std::set<std::string> withheldTexts(const StationParams& set) {
    std::set<std::string> out;
    for (const AbsorbedParam& w : set.shared.withheld) out.insert(keyText(w.ref));
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The phase colour
// ---------------------------------------------------------------------------

TEST(StationBirthTest, ThePhaseColourIsBornAtZeroWithItsElevationAndFollowsThePhase) {
    BirthFixture f(kPhaseColourPosture);
    ASSERT_TRUE(f.stack.config.getNetworkConfig().phase_colour_state);
    const StationRowContext c = f.context();

    networkBirthLocalStates(c, f.set, f.epoch(0));
    ASSERT_EQ(f.set.colour.size(), 3u);
    const std::map<std::string, double> elevation = {{"G05", 50.0}, {"G12", 35.0}, {"E24", 25.0}};
    for (const auto& kv : f.set.colour) {
        const LocalSlot& slot = slotAt(f.set, kv.second);
        EXPECT_EQ(slot.ref, phaseColourRef(kv.first));
        EXPECT_EQ(slot.x_lin, 0.0) << kv.first.str();
        EXPECT_EQ(slot.elevation_deg, elevation.at(kv.first.str())) << kv.first.str();
    }
    // Born after the slant ionospheres and before the ambiguities.
    int last_iono = -1, first_colour = 1 << 30, last_colour = -1, first_amb = 1 << 30;
    for (const auto& kv : f.set.iono) last_iono = std::max(last_iono, kv.second);
    for (const auto& kv : f.set.colour) {
        first_colour = std::min(first_colour, kv.second);
        last_colour  = std::max(last_colour, kv.second);
    }
    for (const auto& kv : f.set.amb) first_amb = std::min(first_amb, kv.second);
    EXPECT_LT(last_iono, first_colour);
    EXPECT_LT(last_colour, first_amb);

    // The value is the filter's; only the elevation follows the line of sight.
    const ParamRef g05 = phaseColourRef(satOf("G05"));
    f.set.slots[static_cast<std::size_t>(slotOf(f.set, g05))].x_lin = 0.004;
    std::vector<PppObservation> next = f.epoch(1);
    next[0] = syntheticObservation("G05", System::GPS, {"C1C", "L1C", "C2W", "L2W"}, 1, 40.0, 60.0);
    networkBirthLocalStates(c, f.set, next);
    EXPECT_EQ(valueOf(f.set, g05), 0.004);
    EXPECT_EQ(slotAt(f.set, slotOf(f.set, g05)).elevation_deg, 60.0);

    // A satellite seen on code alone has no phase colour, and one that leaves takes it along.
    std::vector<PppObservation> reduced;
    reduced.push_back(next[0]);
    reduced.push_back(gpsCodes("G12", {"C1C", "C2W"}, 2, 130.0, 35.0));
    networkBirthLocalStates(c, f.set, reduced);
    ASSERT_EQ(f.set.colour.size(), 1u);
    EXPECT_EQ(f.set.colour.begin()->first, satOf("G05"));
    EXPECT_EQ(f.set.iono.size(), 2u);   // the code-only satellite keeps its ionosphere
}

TEST(StationBirthTest, WithoutThePhaseColourPostureNoColourIsBorn) {
    BirthFixture f;
    networkBirthLocalStates(f.context(), f.set, f.epoch(0));
    EXPECT_TRUE(f.set.colour.empty());
}

// ---------------------------------------------------------------------------
// The first-epoch seeds of the inter-system biases and the receiver clock
// ---------------------------------------------------------------------------

TEST(StationBirthTest, TheInterSystemBiasSeedRetriesUntilTheTimeReferenceHasCode) {
    BirthFixture f;
    const StationRowContext c = f.context();
    const std::vector<PppObservation> galileo_only = {
        syntheticObservation("E24", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 0, 220.0, 25.0)};

    networkBirthLocalStates(c, f.set, galileo_only);
    EXPECT_FALSE(f.set.seeded.isb);

    PppObservation g05 = gpsCodes("G05", {"C1C", "C2W"}, 1);
    PppObservation e24 = syntheticObservation("E24", System::Galileo, {"C1C", "C5Q"}, 1, 220.0, 25.0);
    e24.observed["C1C"] += 7.0;   // the Galileo code reads 7 m above the GPS code
    networkBirthLocalStates(c, f.set, {g05, e24});
    EXPECT_TRUE(f.set.seeded.isb);
    EXPECT_NEAR(valueOf(f.set, isbRef(System::Galileo)), 7.0, 1e-6);
}

TEST(StationBirthTest, TheColdClockIsSeededOnceFromTheTimeReferenceAndThenLatched) {
    BirthFixture f(R"(, "cold_clock_seed": true)");
    ASSERT_TRUE(f.stack.config.networkColdClockSeed());
    const StationRowContext c = f.context();
    const ParamRef clock = rxClockRef();

    // No time-reference code: nothing to seed, nothing latched.
    networkBirthLocalStates(c, f.set, {syntheticObservation("E24", System::Galileo, {"C1C", "L1C"}, 0, 220.0, 25.0)});
    EXPECT_FALSE(f.set.seeded.cold_clock);
    EXPECT_EQ(valueOf(f.set, clock), 0.0);

    // The next epoch has GPS code reading 1000 m above the model: the median becomes the clock.
    auto biased = [](double offset, int k) {
        std::vector<PppObservation> obs = {gpsCodes("G05", {"C1C", "C2W"}, k),
                                           gpsCodes("G12", {"C1C", "C2W"}, k, 130.0, 35.0)};
        for (PppObservation& o : obs)
            for (auto& entry : o.observed) entry.second += offset;
        return obs;
    };
    networkBirthLocalStates(c, f.set, biased(1000.0, 1));
    EXPECT_TRUE(f.set.seeded.cold_clock);
    EXPECT_NEAR(valueOf(f.set, clock), 1000.0, 1e-6);

    // Latched: a cold clock later (here forced to zero) is not seeded again.
    f.set.slots[static_cast<std::size_t>(f.set.clock)].x_lin = 0.0;
    networkBirthLocalStates(c, f.set, biased(2000.0, 2));
    EXPECT_EQ(valueOf(f.set, clock), 0.0);
}

TEST(StationBirthTest, AClockThatIsNotColdIsLeftAndTheSeedLatches) {
    BirthFixture f(R"(, "cold_clock_seed": true)");
    f.set.slots[static_cast<std::size_t>(f.set.clock)].x_lin = 5.0;
    std::vector<PppObservation> obs = {gpsCodes("G05", {"C1C", "C2W"})};
    for (auto& entry : obs[0].observed) entry.second += 1000.0;
    networkBirthLocalStates(f.context(), f.set, obs);
    EXPECT_TRUE(f.set.seeded.cold_clock);
    EXPECT_EQ(valueOf(f.set, rxClockRef()), 5.0);
}

TEST(StationBirthTest, TheColdClockStaysUnseededWhenTheKnobIsOff) {
    BirthFixture f;
    std::vector<PppObservation> obs = {gpsCodes("G05", {"C1C", "C2W"})};
    for (auto& entry : obs[0].observed) entry.second += 1000.0;
    networkBirthLocalStates(f.context(), f.set, obs);
    EXPECT_FALSE(f.set.seeded.cold_clock);
    EXPECT_EQ(valueOf(f.set, rxClockRef()), 0.0);
}

// ---------------------------------------------------------------------------
// The receiver code biases
// ---------------------------------------------------------------------------

namespace {

/// A GPS station with the receiver code biases estimated and a bias product that names L1 and L2
/// as the clock-reference pair.
struct CodeBiasStation {
    BirthFixture fixture;

    CodeBiasStation() : fixture(options()) { fixture.clock_refs['G'] = gpsClockPair(); }

    static BirthOptions options() {
        BirthOptions o;
        o.estimate_ifb = true;
        return o;
    }

    /// One GPS satellite on L1, L2 and L5 code, each code carrying its own bias (m).
    PppObservation satellite(const std::string& sat, int k, double b1, double b2, double b5,
                             bool with_l5 = true) const {
        PppObservation o = gpsCodes(sat, with_l5 ? std::vector<std::string>{"C1C", "C2W", "C5X"}
                                                 : std::vector<std::string>{"C1C", "C2W"}, k);
        o.observed["C1C"] += b1;
        o.observed["C2W"] += b2;
        if (with_l5) o.observed["C5X"] += b5;
        return o;
    }
};

}  // namespace

TEST(StationBirthTest, TheReceiverCodeBiasesAreOnePerKeyTheBiasRuleNamesAndArePrunedWithTheirSignal) {
    CodeBiasStation s;
    BirthFixture& f = s.fixture;
    const StationRowContext c = f.context();
    f.set.seeded.code_bias = true;   // births only; the seed is tested below

    networkBirthLocalStates(c, f.set, {s.satellite("G05", 0, 0, 0, 0), s.satellite("G12", 0, 0, 0, 0)});
    // L1 and L2 are the reference pair (one receiver DCB for both), L5 is outside it (its own IFB);
    // Galileo has no reference pair in the product and so no bias.
    ASSERT_EQ(f.set.rx_cb.size(), 2u);
    const ParamRef dcb = rxCodeBiasRef(dcbKey(System::GPS));
    const ParamRef ifb5 = rxCodeBiasRef(ifbKey(System::GPS, '5'));
    EXPECT_GE(slotOf(f.set, dcb), 0);
    EXPECT_GE(slotOf(f.set, ifb5), 0);
    EXPECT_EQ(valueOf(f.set, dcb), 0.0);
    EXPECT_EQ(valueOf(f.set, ifb5), 0.0);
    EXPECT_LT(slotOf(f.set, dcb), slotOf(f.set, ifb5));   // born in the order the codes arrive

    // The same keys again change nothing; without the L5 code the L5 bias leaves and the rest
    // keeps its order.
    const std::size_t slots = f.set.slots.size();
    networkBirthLocalStates(c, f.set, {s.satellite("G05", 1, 0, 0, 0), s.satellite("G12", 1, 0, 0, 0)});
    EXPECT_EQ(f.set.slots.size(), slots);
    networkBirthLocalStates(c, f.set, {s.satellite("G05", 2, 0, 0, 0, false), s.satellite("G12", 2, 0, 0, 0, false)});
    ASSERT_EQ(f.set.rx_cb.size(), 1u);
    EXPECT_GE(slotOf(f.set, dcb), 0);
    EXPECT_LT(slotOf(f.set, ifb5), 0);
}

TEST(StationBirthTest, WithoutEstimateIfbNoReceiverCodeBiasIsBorn) {
    BirthFixture f;
    f.clock_refs['G'] = gpsClockPair();
    networkBirthLocalStates(f.context(), f.set, {gpsCodes("G05", {"C1C", "C2W", "C5X"})});
    EXPECT_TRUE(f.set.rx_cb.empty());
}

TEST(StationBirthTest, TheGlonassNetworkKeepsOnlyThePerChannelReceiverBias) {
    BirthOptions o;
    o.systems      = R"(["GPS", "Galileo", "GLONASS"])";
    o.priorities   = R"({"GPS": "WC", "Galileo": "QX", "GLONASS": "CP"})";
    o.network_extra = R"(, "estimate_glonass": true)";
    BirthFixture f(o, {{"R07", 1}, {"R08", -4}, {"R09", 0}});
    f.clock_refs['G'] = gpsClockPair();   // GPS would have a bias in a network without GLONASS
    ASSERT_TRUE(f.stack.config.enableGlonassNetwork());
    ASSERT_TRUE(f.stack.config.estimateIfb());
    const StationRowContext c = f.context();
    f.set.seeded.code_bias = true;

    const std::vector<std::string> codes = {"C1C", "L1C", "C2C", "L2C"};
    std::vector<PppObservation> epoch = {
        gpsCodes("G05", {"C1C", "C2W", "C5X"}),
        syntheticObservation("R07", System::GLONASS, codes, 0, 90.0, 40.0),
        syntheticObservation("R08", System::GLONASS, codes, 0, 150.0, 30.0),
        syntheticObservation("R09", System::GLONASS, codes, 0, 210.0, 45.0)};
    networkBirthLocalStates(c, f.set, epoch);

    // One bias per (band, channel) of the channels that are not the central one; none for GPS.
    EXPECT_EQ(f.set.rx_cb.size(), 4u);
    for (const char band : {'1', '2'}) {
        EXPECT_GE(slotOf(f.set, rxCodeBiasRef(slotIfbKey(System::GLONASS, band, 1))), 0) << band;
        EXPECT_GE(slotOf(f.set, rxCodeBiasRef(slotIfbKey(System::GLONASS, band, -4))), 0) << band;
        EXPECT_LT(slotOf(f.set, rxCodeBiasRef(slotIfbKey(System::GLONASS, band, 0))), 0) << band;
    }
    EXPECT_LT(slotOf(f.set, rxCodeBiasRef(dcbKey(System::GPS))), 0);
    EXPECT_LT(slotOf(f.set, rxCodeBiasRef(ifbKey(System::GPS, '5'))), 0);

    // A channel whose satellite leaves is pruned.
    epoch.erase(epoch.begin() + 2);
    networkBirthLocalStates(c, f.set, epoch);
    EXPECT_EQ(f.set.rx_cb.size(), 2u);
    EXPECT_LT(slotOf(f.set, rxCodeBiasRef(slotIfbKey(System::GLONASS, '1', -4))), 0);
    EXPECT_GE(slotOf(f.set, rxCodeBiasRef(slotIfbKey(System::GLONASS, '1', 1))), 0);
}

// ---------------------------------------------------------------------------
// The receiver code-bias seeds
// ---------------------------------------------------------------------------

namespace {

/// The coefficient of an L5 bias against the L1 reference, which divides the code difference:
/// the L5 coefficient (1) less the L1 coefficient of the geometry-free ionosphere-free pair.
double l5CoefficientDifference(LogManager& log) {
    const PppUtils utils(log);
    const double f1 = utils.getFrequency("C1W", System::GPS, "G05");
    const double f2 = utils.getFrequency("C2W", System::GPS, "G05");
    const double mu2 = (f1 * f1) / (f2 * f2);
    return 1.0 - 1.0 / (1.0 - mu2);
}

}  // namespace

TEST(StationBirthTest, TheFirstEpochSeedsTheReceiverCodeBiasesFromTheBandDifferences) {
    CodeBiasStation s;
    BirthFixture& f = s.fixture;
    // Code biases of 10, 8 and 10 + 3 beta m on L1, L2 and L5: the L1 minus L2 difference is the
    // DCB (the L2 coefficient minus the L1 coefficient is -1), the L5 difference divided by its
    // coefficient difference beta is the L5 IFB.
    const double beta = l5CoefficientDifference(f.log);
    networkBirthLocalStates(f.context(), f.set,
                            {s.satellite("G05", 0, 10.0, 8.0, 10.0 + 3.0 * beta),
                             s.satellite("G12", 0, 10.0, 8.0, 10.0 + 3.0 * beta)});
    EXPECT_TRUE(f.set.seeded.code_bias);
    EXPECT_NEAR(valueOf(f.set, rxCodeBiasRef(dcbKey(System::GPS))), 2.0, 1e-6);
    EXPECT_NEAR(valueOf(f.set, rxCodeBiasRef(ifbKey(System::GPS, '5'))), 3.0, 1e-6);

    // One shot: a later epoch with other biases leaves the seeded values.
    networkBirthLocalStates(f.context(), f.set,
                            {s.satellite("G05", 1, 30.0, 8.0, 40.0), s.satellite("G12", 1, 30.0, 8.0, 40.0)});
    EXPECT_NEAR(valueOf(f.set, rxCodeBiasRef(dcbKey(System::GPS))), 2.0, 1e-6);
}

TEST(StationBirthTest, AWarmResumeInstallsTheCodeBiasPointsAndCountsTheOnesItCannotUse) {
    CodeBiasStation s;
    BirthFixture& f = s.fixture;
    const double beta = l5CoefficientDifference(f.log);
    long long applied = 0, uncovered = 0;
    // The DCB point is usable; the L5 IFB point is covered by the snapshot but not finite.
    f.set.rx_cb_restore = CodeBiasRestore{
        {{"GPS_DCB", 4.5}, {"GPS_IFB5", std::numeric_limits<double>::quiet_NaN()}}, &applied, &uncovered};
    networkBirthLocalStates(f.context(), f.set,
                            {s.satellite("G05", 0, 10.0, 8.0, 10.0 + 3.0 * beta),
                             s.satellite("G12", 0, 10.0, 8.0, 10.0 + 3.0 * beta)});
    EXPECT_EQ(valueOf(f.set, rxCodeBiasRef(dcbKey(System::GPS))), 4.5);   // the writer's point, not the median
    EXPECT_NEAR(valueOf(f.set, rxCodeBiasRef(ifbKey(System::GPS, '5'))), 3.0, 1e-6);   // the median
    EXPECT_EQ(applied, 1);
    EXPECT_EQ(uncovered, 1);
}

TEST(StationBirthTest, AKeyTheSnapshotDoesNotCarryTakesTheMedianAndIsNotCounted) {
    CodeBiasStation s;
    BirthFixture& f = s.fixture;
    const double beta = l5CoefficientDifference(f.log);
    long long applied = 0, uncovered = 0;
    f.set.rx_cb_restore = CodeBiasRestore{{{"GPS_DCB", 4.5}}, &applied, &uncovered};
    networkBirthLocalStates(f.context(), f.set,
                            {s.satellite("G05", 0, 10.0, 8.0, 10.0 + 3.0 * beta)});
    EXPECT_EQ(valueOf(f.set, rxCodeBiasRef(dcbKey(System::GPS))), 4.5);
    EXPECT_NEAR(valueOf(f.set, rxCodeBiasRef(ifbKey(System::GPS, '5'))), 3.0, 1e-6);
    EXPECT_EQ(applied, 1);
    EXPECT_EQ(uncovered, 0);
}

// ---------------------------------------------------------------------------
// The shared admission
// ---------------------------------------------------------------------------

TEST(StationBirthTest, TheCoverageAllowListWithholdsTheSignalsItDoesNotListAndIsRedrawnEachEpoch) {
    // GPS may carry a phase bias on L1C only; a constellation not in the list is unrestricted.
    BirthFixture f(BirthOptions{}, {}, {{"GPS", {"L1C"}}});
    const StationRowContext c = f.context();
    const std::vector<PppObservation> obs = f.epoch(0);

    networkAdmitSharedStates(c, f.set, obs, {}, nullptr);
    EXPECT_EQ(phaseBiasTexts(f.set), (std::set<std::string>{"E24_L1C", "E24_L5Q", "G05_L1C", "G12_L1C"}));
    ASSERT_EQ(f.set.shared.withheld.size(), 2u);
    for (const AbsorbedParam& w : f.set.shared.withheld) EXPECT_EQ(w.reason, AbsorbedReason::CoverageGate);
    EXPECT_EQ(withheldTexts(f.set), (std::set<std::string>{"G05|L2W|PB", "G12|L2W|PB"}));
    EXPECT_EQ(f.set.shared.sat_clock.size(), 3u);   // the clock is not gated by the list

    // The record is the epoch's own: an epoch with no gated signal clears it.
    networkAdmitSharedStates(c, f.set, {syntheticObservation("E24", System::Galileo, {"C1C", "L1C", "C5Q", "L5Q"}, 1, 220.0, 25.0)},
                             {}, nullptr);
    EXPECT_TRUE(f.set.shared.withheld.empty());
}

TEST(StationBirthTest, GlonassHasOnlyAClockOnTheCodeOnlyScopeAndNothingWithoutIt) {
    const std::vector<std::string> codes = {"C1C", "L1C", "C2C", "L2C"};
    const PppObservation g05 = syntheticObservation("G05", System::GPS, {"C1C", "L1C", "C2W", "L2W"}, 0, 40.0, 50.0);
    const PppObservation r07 = syntheticObservation("R07", System::GLONASS, codes, 0, 90.0, 40.0);

    // GLONASS not admitted to the network: its satellites are not part of the station.
    {
        BirthFixture f;
        networkAdmitSharedStates(f.context(), f.set, {g05, r07}, {}, nullptr);
        EXPECT_EQ(f.set.shared.sat_clock.count(satOf("R07")), 0u);
        EXPECT_TRUE(f.set.shared.withheld.empty());
        EXPECT_TRUE(f.set.shared.withheld_unnamed.empty());
    }
    // The code-only scope: a clock for the satellite, its phase biases withheld with the reason.
    {
        BirthOptions o;
        o.systems       = R"(["GPS", "Galileo", "GLONASS"])";
        o.priorities    = R"({"GPS": "WC", "Galileo": "QX", "GLONASS": "CP"})";
        o.network_extra = R"(, "estimate_glonass": true)";
        BirthFixture f(o, {{"R07", 1}});
        networkAdmitSharedStates(f.context(), f.set, {g05, r07}, {}, nullptr);
        EXPECT_EQ(f.set.shared.sat_clock.count(satOf("R07")), 1u);
        EXPECT_EQ(f.set.shared.sat_clock.count(satOf("G05")), 1u);
        EXPECT_EQ(phaseBiasTexts(f.set), (std::set<std::string>{"G05_L1C", "G05_L2W"}));
        ASSERT_EQ(f.set.shared.withheld.size(), 2u);
        for (const AbsorbedParam& w : f.set.shared.withheld) EXPECT_EQ(w.reason, AbsorbedReason::GlonassCodeOnlyScope);
        EXPECT_EQ(withheldTexts(f.set), (std::set<std::string>{"R07|L1C|PB", "R07|L2C|PB"}));
    }
}

TEST(StationBirthTest, TheSatelliteCodeBiasIsAdmittedPerCodeSignalAndPrunedWithTheSatellite) {
    BirthFixture f(R"(, "estimate_sat_code_bias": true)");
    ASSERT_TRUE(f.stack.config.estimateNetworkSatCodeBias());
    const StationRowContext c = f.context();

    networkAdmitSharedStates(c, f.set, f.epoch(0), {}, nullptr);
    // One column per (satellite, code signal), at 0.
    ASSERT_EQ(f.set.shared.sat_code_bias.size(), 6u);
    for (const char* text : {"G05_C1C", "G05_C2W", "G12_C1C", "G12_C2W", "E24_C1C", "E24_C5Q"}) {
        const std::string sat = std::string(text).substr(0, 3);
        const std::string code = std::string(text).substr(4);
        const SatId s = satOf(sat);
        EXPECT_EQ(valueOf(f.set, satCodeBiasRef(s, signalOf(s.sys, code))), 0.0) << text;
    }
    EXPECT_EQ(f.set.shared.sat_clock.size(), 3u);
    EXPECT_EQ(f.set.shared.sat_phase_bias.size(), 6u);

    // A satellite that leaves takes its clock, phase biases and code biases with it, and the
    // survivors keep their order.
    const std::size_t before = f.set.slots.size();
    networkAdmitSharedStates(c, f.set, f.epoch(1, /*with_g12=*/false), {}, nullptr);
    EXPECT_EQ(f.set.shared.sat_code_bias.size(), 4u);
    EXPECT_EQ(f.set.shared.sat_phase_bias.size(), 4u);
    EXPECT_EQ(f.set.shared.sat_clock.size(), 2u);
    EXPECT_EQ(f.set.slots.size(), before - 5u);
    EXPECT_EQ(f.set.shared.sat_clock.count(satOf("G12")), 0u);
    for (const auto& kv : f.set.shared.sat_code_bias) EXPECT_NE(kv.first.sat, satOf("G12"));
}

TEST(StationBirthTest, TheAdmissionAndTheRowLoopAgreeOnTheStoredGeometry) {
    struct Gate {
        const char* name;
        std::function<void(PppObservation&)> spoil;
    };
    const std::vector<Gate> gates = {
        {"below the elevation mask", [](PppObservation& o) { o.elevation = 5.0; }},
        {"satellite position too near", [](PppObservation& o) { o.sat_pos = Eigen::Vector3d(1.0e6, 0.0, 0.0); }},
        {"range too short", [](PppObservation& o) { o.range = 1.0e7; }},
        {"no line of sight", [](PppObservation& o) { o.los = Eigen::Vector3d::Zero(); }},
        {"no wet mapping function", [](PppObservation& o) { o.mf_w = 0.0; }},
    };
    for (const Gate& gate : gates) {
        BirthFixture f;
        const StationRowContext c = f.context();
        std::vector<PppObservation> obs = f.epoch(0);
        gate.spoil(obs[1]);   // G12
        networkBirthLocalStates(c, f.set, obs);
        networkAdmitSharedStates(c, f.set, obs, {}, nullptr);

        EXPECT_EQ(f.set.shared.sat_clock.count(satOf("G12")), 0u) << gate.name;
        EXPECT_EQ(f.set.shared.sat_clock.count(satOf("G05")), 1u) << gate.name;
        for (const auto& kv : f.set.shared.sat_phase_bias) EXPECT_NE(kv.first.sat, satOf("G12")) << gate.name;
        // The row loop drops the same satellite, so no admitted column is left without a row.
        const EpochMeasurement em = networkStationRows(c, f.set, obs);
        EXPECT_EQ(em.satellite_ids.size(), 8u) << gate.name;   // four signals of each of two satellites
        EXPECT_EQ(std::count(em.satellite_ids.begin(), em.satellite_ids.end(), std::string("G12")), 0) << gate.name;
        for (const auto& kv : f.set.shared.sat_clock) {
            bool written = false;
            for (Eigen::Index r = 0; r < em.H.rows(); ++r) written = written || em.H(r, kv.second) != 0.0;
            EXPECT_TRUE(written) << gate.name << " " << kv.first.str();
        }
    }
}

// ---------------------------------------------------------------------------
// The ambiguity basis of the rows
// ---------------------------------------------------------------------------

namespace {

/// The rows of a station, after its births and its admission, over a configuration with the
/// given members of the estimation object.
EpochMeasurement rowsUnder(const std::string& estimation_extra, StationParams* set_out = nullptr) {
    BirthOptions o;
    o.estimation_extra = estimation_extra;
    BirthFixture f(o);
    const StationRowContext c = f.context();
    const std::vector<PppObservation> obs = f.epoch(0);
    networkBirthLocalStates(c, f.set, obs);
    networkAdmitSharedStates(c, f.set, obs, {}, nullptr);
    EpochMeasurement em = networkStationRows(c, f.set, obs);
    if (set_out != nullptr) *set_out = f.set;
    return em;
}

/// Whether two row blocks are the same bit for bit.
void expectSameRows(const EpochMeasurement& a, const EpochMeasurement& b) {
    ASSERT_EQ(a.innovations.size(), b.innovations.size());
    ASSERT_EQ(a.H.rows(), b.H.rows());
    ASSERT_EQ(a.H.cols(), b.H.cols());
    for (Eigen::Index r = 0; r < a.H.rows(); ++r) {
        EXPECT_EQ(a.innovations(r), b.innovations(r)) << r;
        EXPECT_EQ(a.R_v(r), b.R_v(r)) << r;
        for (Eigen::Index k = 0; k < a.H.cols(); ++k) EXPECT_EQ(a.H(r, k), b.H(r, k)) << r << "," << k;
    }
    EXPECT_EQ(a.satellite_ids, b.satellite_ids);
    EXPECT_EQ(a.signal_types, b.signal_types);
}

}  // namespace

TEST(StationBirthTest, ANetworkStationHasOneAmbiguityPerBandWhateverTheAmbiguityBasisSays) {
    StationParams per_band_set, nlwl_set;
    const EpochMeasurement per_band = rowsUnder("", &per_band_set);
    const EpochMeasurement nlwl = rowsUnder(R"(, "state_ambiguity_basis": "nl_wl")", &nlwl_set);

    BirthOptions o;
    o.estimation_extra = R"(, "state_ambiguity_basis": "nl_wl")";
    BirthFixture probe(o);
    ASSERT_EQ(probe.stack.config.getStateAmbiguityBasis(), StateAmbiguityBasis::NlWl);

    // Every signal keeps its row: the phase rows are not dropped for want of a narrow-lane
    // reference band, and the columns are the per-band ones.
    EXPECT_EQ(nlwl.innovations.size(), 12);
    EXPECT_EQ(nlwl_set.amb.size(), 6u);
    for (const auto& kv : nlwl_set.amb) EXPECT_EQ(slotAt(nlwl_set, kv.second).ref.kind, NetworkParamKind::Ambiguity);
    expectSameRows(per_band, nlwl);
}

TEST(StationBirthTest, ANetworkStationIgnoresTheSingleReceiverPhaseClockAndGradients) {
    const EpochMeasurement plain = rowsUnder("");
    const EpochMeasurement flagged = rowsUnder(
        R"(, "estimate_phase_clock": true, "estimate_tropo_gradients": true)");
    EXPECT_EQ(flagged.innovations.size(), 12);
    expectSameRows(plain, flagged);
}

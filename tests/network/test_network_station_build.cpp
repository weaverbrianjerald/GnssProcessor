/**
 * @file test_network_station_build.cpp
 * @brief The station build's two step tables and the parallel contract as a type (card NOM3
 *        step PB1b, filter/NetworkStationBuild.hpp).
 *
 * The compile-time half: the view members of StationBuild are const references (a step
 * cannot write shared state through them), the writable members are exactly the station, its
 * slot and the measurement update, the two step function types are exact, and the table sizes
 * equal their counts.  The run-time half: the table orders that fix the bytes (rows, diagonal
 * priors, block columns, pending coupling rows) and the kinds the tables declare.
 */

#include <gtest/gtest.h>

#include "network/assembly/NetworkStationBuild.hpp"
#include "network/assembly/NetworkContributionKind.hpp"

#include <cstddef>
#include <string>
#include <type_traits>
#include <vector>

using namespace GnssProcessor;

namespace {

template <typename T>
constexpr bool kConstRef = std::is_reference_v<T> && std::is_const_v<std::remove_reference_t<T>>;

// ---- The parallel contract (compile time) -------------------------------------------------
static_assert(kConstRef<decltype(StationBuild::ctx)>, "the run context is read only in a step");
static_assert(kConstRef<decltype(StationBuild::config)>, "the configuration is read only");
static_assert(kConstRef<decltype(StationBuild::est)>, "the estimator's layout is read only");
static_assert(kConstRef<decltype(StationBuild::setup)>, "the setup (every station) is read only");
static_assert(kConstRef<decltype(StationBuild::se)>, "the epoch is read only");
static_assert(kConstRef<decltype(StationBuild::clock)>, "the epoch clock is read only");
static_assert(kConstRef<decltype(StationBuild::pass_a)>, "PASS A's layout is read only");
static_assert(kConstRef<decltype(StationBuild::gim)>, "the GIM is read only");
static_assert(std::is_same_v<decltype(StationBuild::i), const int>,
              "the roster index keeps the frozen loop variable's type (the carry keys are int)");
static_assert(std::is_same_v<decltype(StationBuild::st), NetStation&>, "the station is writable");
static_assert(std::is_same_v<decltype(StationBuild::R), StationAssembly&>, "its slot is writable");
static_assert(std::is_same_v<decltype(StationBuild::em), EpochMeasurement>,
              "the measurement update is the one value a step hands a later one");
static_assert(std::is_same_v<StationBuildFn, StationStep (*)(StationBuild&)>);
static_assert(std::is_same_v<StationCarryFn, void (*)(StationCarryFrame&)>);
static_assert(std::is_same_v<decltype(StationCarryFrame::build), StationBuild&>);
static_assert(std::tuple_size_v<std::remove_const_t<decltype(kStationBuildSteps)>> ==
              kStationBuildStepCount);
static_assert(std::tuple_size_v<std::remove_const_t<decltype(kStationCarrySteps)>> ==
              kStationCarryStepCount);

std::vector<std::string> buildNames() {
    std::vector<std::string> out;
    for (const StationBuildStep& s : kStationBuildSteps) out.emplace_back(s.name);
    return out;
}

std::vector<std::string> carryNames() {
    std::vector<std::string> out;
    for (const StationCarryStep& s : kStationCarrySteps) out.emplace_back(s.name);
    return out;
}

/// The index of the first top-level step whose kinds hold @p k (kStationBuildStepCount if none).
std::size_t firstBuildStep(ContributionKind k) {
    for (std::size_t j = 0; j < kStationBuildSteps.size(); ++j)
        if (kStationBuildSteps[j].kinds & kindBit(k)) return j;
    return kStationBuildStepCount;
}

/// The index of the first carry step whose kinds hold @p k (kStationCarryStepCount if none).
std::size_t firstCarryStep(ContributionKind k) {
    for (std::size_t j = 0; j < kStationCarrySteps.size(); ++j)
        if (kStationCarrySteps[j].kinds & kindBit(k)) return j;
    return kStationCarryStepCount;
}

std::size_t carryIndex(const std::string& name) {
    const std::vector<std::string> n = carryNames();
    for (std::size_t j = 0; j < n.size(); ++j)
        if (n[j] == name) return j;
    return n.size();
}

}  // namespace

// ---- The tables ---------------------------------------------------------------------------
TEST(NetworkStationBuildTable, TopLevelStepsInWalkOrder) {
    const std::vector<std::string> want = {
        "data_rows", "rx_ifb_recouple", "congruence_geometry", "phase_datum_pins",
        "glonass_anchors", "sim_zero_iono", "iono_constraint", "sd_ewl", "raim_columns",
        "carried_priors", "phase_colour_rows", "zwd_capture"};
    EXPECT_EQ(buildNames(), want);
}

TEST(NetworkStationBuildTable, CarryStepsInWalkOrder) {
    const std::vector<std::string> want = {
        "ambiguity", "slant_iono_block", "rx_clock_block", "rx_cb_block", "phase_colour_block",
        "local_block", "oracle_pin", "zwd", "rx_cb", "coupling_region", "prior_finalize"};
    EXPECT_EQ(carryNames(), want);
}

TEST(NetworkStationBuildTable, EveryStepHasAFunctionAndAFamilyFile) {
    for (const StationBuildStep& s : kStationBuildSteps) {
        EXPECT_NE(s.build, nullptr) << s.name;
        EXPECT_EQ(std::string(s.home).rfind("filter/Network", 0), 0u) << s.name;
    }
    for (const StationCarryStep& s : kStationCarrySteps) {
        EXPECT_NE(s.build, nullptr) << s.name;
        EXPECT_EQ(std::string(s.home).rfind("filter/Network", 0), 0u) << s.name;
    }
}

TEST(NetworkStationBuildTable, KindsCoverExactlyTheStationKinds) {
    ContributionKindSet top = 0, carry = 0, station = 0;
    for (const StationBuildStep& s : kStationBuildSteps) top |= s.kinds;
    for (const StationCarryStep& s : kStationCarrySteps) carry |= s.kinds;
    for (auto k = static_cast<unsigned>(ContributionKind::DataCode);
         k <= static_cast<unsigned>(ContributionKind::PriorRxCbDatumPrePd); ++k)
        station |= kindBit(static_cast<ContributionKind>(k));
    EXPECT_EQ(top, station);
    // carried_priors declares the union of the carry table's kinds.
    EXPECT_EQ(kStationBuildSteps[9].kinds, carry);
}

// ---- The orders that fix the bytes --------------------------------------------------------
TEST(NetworkStationBuildOrder, RowsFollowTheTableOrder) {
    using CK = ContributionKind;
    const std::vector<CK> rows = {CK::DataCode, CK::AmbiguityPin, CK::GloIfbAnchor,
                                  CK::SimZeroIono, CK::ShIonoTie, CK::SdEwl,
                                  CK::CouplingScalar, CK::PhaseColourBirth};
    for (std::size_t j = 1; j < rows.size(); ++j)
        EXPECT_LT(firstBuildStep(rows[j - 1]), firstBuildStep(rows[j])) << j;
}

TEST(NetworkStationBuildOrder, DiagonalPriorsFollowTheCarryTable) {
    using CK = ContributionKind;
    EXPECT_LT(firstCarryStep(CK::PriorAmbiguityCarry), firstCarryStep(CK::PriorAmbiguityOraclePin));
    EXPECT_LT(firstCarryStep(CK::PriorAmbiguityOraclePin), firstCarryStep(CK::PriorZwdCarry));
    EXPECT_LT(firstCarryStep(CK::PriorZwdCarry), firstCarryStep(CK::PriorRxCbCarry));
    EXPECT_LT(carryIndex("rx_cb"), carryIndex("prior_finalize"));
}

TEST(NetworkStationBuildOrder, BlockColumnsAreCollectedBeforeTheBlock) {
    // The ambiguity columns, then ionosphere, receiver clock, receiver code bias and phase
    // colour are collected in this order; the block (which pushes the wet delay after the
    // ambiguities) is built after all of them and appended by the coupling region.
    EXPECT_LT(carryIndex("ambiguity"), carryIndex("slant_iono_block"));
    EXPECT_LT(carryIndex("slant_iono_block"), carryIndex("rx_clock_block"));
    EXPECT_LT(carryIndex("rx_clock_block"), carryIndex("rx_cb_block"));
    EXPECT_LT(carryIndex("rx_cb_block"), carryIndex("phase_colour_block"));
    EXPECT_LT(carryIndex("phase_colour_block"), carryIndex("local_block"));
    EXPECT_LT(carryIndex("local_block"), carryIndex("coupling_region"));
}

TEST(NetworkStationBuildOrder, PendingCouplingRowsAmbiguityThenWetDelay) {
    // Pending coupling rows: the ambiguity rows (inline route in `ambiguity`, or the block's
    // fallbacks in `local_block`), then the wet-delay row (`zwd`), appended by
    // `coupling_region`.
    EXPECT_LT(carryIndex("ambiguity"), carryIndex("zwd"));
    EXPECT_LT(carryIndex("local_block"), carryIndex("zwd"));
    EXPECT_LT(carryIndex("zwd"), carryIndex("coupling_region"));
}

/**
 * @file test_ppp_model_signal_selection.cpp
 * @brief Characterization tables for the PPP model's per-band signal choice: which
 *        code and which phase signal a station uses on each band of GPS, Galileo and
 *        BeiDou, given the configured priority list and the signals the satellites
 *        actually report.
 *
 * The choice is made once per constellation, at the first epoch that constellation is
 * seen, and is then locked for the run (the per-band "latch").  Every later epoch takes
 * its per-band signals from that latch, so the latch is the signal selection a run
 * actually uses.  These tables pin it for the signal sets the project runs on (GPS L1C,
 * L1W, L2W, L2L, L5Q; Galileo L1C, L1X, L5Q, L5X, L7Q; BeiDou B1I and B3I, which the
 * RINEX 3 codes name C2I / L2I and C6I / L6I), together with the cases a reader most
 * needs settled:
 *   - a tie in satellite count goes to the signal listed first;
 *   - under the default count rule, the most observed signal wins over the listed order;
 *   - an absent first priority falls through to the next listed signal of the band;
 *   - a signal the receiver reports but the list does not name is never chosen;
 *   - a constellation with no priority entry at all gets no latch (the known trap: such
 *     a constellation is dropped from processing, with no error).
 *
 * The cases drive the public computeModels() entry with synthetic observations and an
 * uninitialised receiver position.  computeModels() decides the latch before it touches
 * geometry, and the zero position then rejects every satellite before any orbit or clock
 * product is read, so the cases need no product files and read the latch back through
 * exportCanonicalSignals() and the band-digit accessors.
 *
 * Not reachable this way: the per-satellite selection that follows a successful geometry
 * (selectSignals and its helpers), which needs orbit and clock products.
 */
#include <gtest/gtest.h>

#include "common/GnssTypes.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "model/PppModel.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"
#include "support/TestTempDir.hpp"

#include <Eigen/Dense>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace GnssProcessor;

namespace {

/// The GPS priority list of these tables: the signals named in the banner, code first.
const char* kGpsList =
    R"("GPS": ["C1C", "C1W", "C2W", "C2L", "C5Q", "L1C", "L1W", "L2W", "L2L", "L5Q"])";
/// The Galileo priority list of these tables.
const char* kGalList =
    R"("Galileo": ["C1C", "C1X", "C5Q", "C5X", "C7Q", "L1C", "L1X", "L5Q", "L5X", "L7Q"])";
/// The BeiDou priority list: B1I (RINEX band 2) and B3I (RINEX band 6), I then X tracking.
const char* kBdsList =
    R"("BeiDou": ["C2I", "C2X", "C6I", "C6X", "L2I", "L2X", "L6I", "L6X"])";

/// A PPP-RTK configuration with the given signal_priorities members and extra
/// processing members (each extra member starting with a comma).
std::string configText(const std::string& priorities, const std::string& processing = "") {
    return std::string(R"({
        "processing": {
            "mode": "ppp-rtk",
            "active_systems": ["GPS", "Galileo", "BeiDou"],
            "signal_priorities": {)") + priorities + R"(})" + processing + R"(
        },
        "estimation": {
            "estimate_velocity": false,
            "estimate_phase_clock": false,
            "estimate_ifb": false,
            "estimate_sat_biases": false,
            "no_bia_code_downweight": 100.0,
            "ambiguity_resolution": {"mode": 0},
            "between_satellite_sd": {"enabled": false}
        },
        "filter": {"initial_std": {
            "position": 1.0, "clock": 1.0, "troposphere": 0.1,
            "ionosphere": 1.0, "ambiguity": 100.0
        }}
    })";
}

/// The configuration, the parsers and the PPP model, built in the order the filter builds them.
struct ModelUnderTest {
    LogManager log;
    GnssConfig cfg;
    std::unique_ptr<ProductParser> products;
    std::unique_ptr<RinexParser> rinex;
    std::unique_ptr<PppModel> model;

    bool load(const std::string& text) {
        const std::filesystem::path path = gnss_test::tempDir() / "ppp_model_selection.json";
        {
            std::ofstream out(path);
            out << text;
        }
        const bool ok = cfg.loadFromJson(path, log);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        if (!ok) return false;
        products = std::make_unique<ProductParser>(cfg, log);
        rinex = std::make_unique<RinexParser>(cfg, log, *products);
        model = std::make_unique<PppModel>(cfg, *products, log, *rinex);
        return true;
    }
};

/// One satellite reporting the given signals (values are placeholders: only presence counts).
RinexObservation sat(const std::string& id, System sys, const std::vector<std::string>& signals) {
    RinexObservation o;
    o.satellite = id;
    o.system = sys;
    o.time = GnssTime{2313, 259200.0};
    double v = 2.0e7;
    for (const auto& s : signals) o.measurements[s] = (v += 1.0);
    return o;
}

/// Runs one epoch through computeModels with a zero receiver position: the latch is decided,
/// then every satellite is rejected before geometry reads any product.
void runEpoch(PppModel& m, const std::vector<RinexObservation>& epoch) {
    PppState state;
    state.position.setZero();
    std::vector<PppObservation> rows;
    m.computeModels(epoch, state, rows);
    ASSERT_TRUE(rows.empty()) << "the latch-only harness modelled a row; its premise broke";
}

/// The latch of one constellation as "band/type" -> signal (e.g. "L1/C" -> "C1C").
std::map<std::string, std::string> latch(const PppModel& m, const std::string& cons) {
    std::map<std::string, std::string> out;
    for (const auto& row : m.exportCanonicalSignals())
        if (row.constellation == cons)
            out[row.band + "/" + std::string(1, row.obs_type)] = row.signal;
    return out;
}

using Table = std::map<std::string, std::string>;

}  // namespace

// --------------------------------------------------------------------------
// GPS
// --------------------------------------------------------------------------

/// Every listed GPS signal on every satellite: each band ties, and the tie goes to the
/// signal listed first.  L5 is latched even though the default frequency mode is dual:
/// the latch covers every band of the list, and the frequency mode filters later.
TEST(PppModelSignalSelection, GpsTieGoesToTheFirstListedSignalOnEveryBand) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(kGpsList)));
    const std::vector<std::string> all{ "C1C", "C1W", "C2W", "C2L", "C5Q",
                                        "L1C", "L1W", "L2W", "L2L", "L5Q" };
    runEpoch(*t.model, { sat("G01", System::GPS, all), sat("G02", System::GPS, all),
                         sat("G03", System::GPS, all) });
    EXPECT_EQ(latch(*t.model, "GPS"), (Table{
        { "L1/C", "C1C" }, { "L1/L", "L1C" },
        { "L2/C", "C2W" }, { "L2/L", "L2W" },
        { "L5/C", "C5Q" }, { "L5/L", "L5Q" } }));
}

/// Under the default count rule the signal more satellites report wins, whatever the
/// listed order: L2L on three satellites beats L2W (listed first) on two.
TEST(PppModelSignalSelection, GpsCountRuleTakesTheMostObservedSignalOverTheListedOrder) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(kGpsList)));
    runEpoch(*t.model, {
        sat("G01", System::GPS, { "C1C", "L1C", "C2W", "L2W" }),
        sat("G02", System::GPS, { "C1C", "L1C", "C2W", "L2W", "C2L", "L2L" }),
        sat("G03", System::GPS, { "C1C", "L1C", "C2L", "L2L" }),
        sat("G04", System::GPS, { "C1C", "L1C", "C2L", "L2L" }) });
    const Table got = latch(*t.model, "GPS");
    EXPECT_EQ(got.at("L2/L"), "L2L");
    EXPECT_EQ(got.at("L2/C"), "C2L");
    EXPECT_EQ(got.at("L1/C"), "C1C");
    EXPECT_EQ(got.at("L1/L"), "L1C");
}

/// The same epoch under processing.canonical_signal_rule "priority" (coverage floor 0.5 by
/// default): L2W on two satellites clears half of the band's largest count (three), so the
/// first listed signal is kept, and its code follows the W attribute.
TEST(PppModelSignalSelection, GpsPriorityRuleKeepsTheFirstListedSignalAboveTheFloor) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(kGpsList, R"(, "canonical_signal_rule": "priority")")));
    runEpoch(*t.model, {
        sat("G01", System::GPS, { "C1C", "L1C", "C2W", "L2W" }),
        sat("G02", System::GPS, { "C1C", "L1C", "C2W", "L2W", "C2L", "L2L" }),
        sat("G03", System::GPS, { "C1C", "L1C", "C2L", "L2L" }),
        sat("G04", System::GPS, { "C1C", "L1C", "C2L", "L2L" }) });
    const Table got = latch(*t.model, "GPS");
    EXPECT_EQ(got.at("L2/L"), "L2W");
    EXPECT_EQ(got.at("L2/C"), "C2W");
}

/// The first listed L1 signals (C1C, L1C) are reported by no satellite: the band falls
/// through to the next listed signals (C1W, L1W); the band is not left empty.
TEST(PppModelSignalSelection, GpsAbsentFirstPriorityFallsThroughToTheNextListed) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(kGpsList)));
    const std::vector<std::string> semi_codeless{ "C1W", "L1W", "C2W", "L2W" };
    runEpoch(*t.model, { sat("G05", System::GPS, semi_codeless),
                         sat("G06", System::GPS, semi_codeless) });
    EXPECT_EQ(latch(*t.model, "GPS"), (Table{
        { "L1/C", "C1W" }, { "L1/L", "L1W" },
        { "L2/C", "C2W" }, { "L2/L", "L2W" } }));
}

/// C1X / L1X are reported on every satellite but are not in the list: the L1 band gets no
/// latch at all, even though L2 does.  The list, not the receiver, bounds the choice.
TEST(PppModelSignalSelection, GpsUnlistedSignalIsNeverChosen) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(kGpsList)));
    const std::vector<std::string> obs{ "C1X", "L1X", "C2W", "L2W" };
    runEpoch(*t.model, { sat("G07", System::GPS, obs), sat("G08", System::GPS, obs) });
    EXPECT_EQ(latch(*t.model, "GPS"), (Table{ { "L2/C", "C2W" }, { "L2/L", "L2W" } }));
    EXPECT_EQ(t.model->latchedCanonicalCodeOnBandDigit("GPS", '1'), "");
    EXPECT_EQ(t.model->latchedCanonicalPhaseOnBandDigit("GPS", '1'), "");
}

/// The string form of a priority entry ("WC") expands to every band of the system in
/// attribute order, so W is listed before C on each band and wins the tie; L2L is not
/// named by either attribute and is never chosen.
TEST(PppModelSignalSelection, GpsAttributeStringListsEveryBandInAttributeOrder) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(R"("GPS": "WC")")));
    const std::vector<std::string> obs{ "C1C", "C1W", "L1C", "L1W", "C2L", "L2L", "C2W", "L2W" };
    runEpoch(*t.model, { sat("G09", System::GPS, obs), sat("G10", System::GPS, obs) });
    EXPECT_EQ(latch(*t.model, "GPS"), (Table{
        { "L1/C", "C1W" }, { "L1/L", "L1W" },
        { "L2/C", "C2W" }, { "L2/L", "L2W" } }));
}

/// Doppler and signal-strength entries of a list are never latched: only code and phase
/// rows carry a per-band choice.
///
/// The outcome is held by three redundant guards: the code-or-phase type filter of the
/// tally, the band prefix table (D1C and S1C start with no listed prefix such as "C1" or
/// "L1", so no band is found for them) and the pick rule, which keeps only C and L
/// candidates.  Deleting any single one of these rules leaves the result unchanged; the
/// case fails only if all of them stop excluding these entries.  It records the end-to-end
/// behaviour, not one rule.
TEST(PppModelSignalSelection, DopplerAndSignalStrengthEntriesAreNeverChosen) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(R"("GPS": ["D1C", "S1C", "C1C", "L1C"])")));
    const std::vector<std::string> obs{ "C1C", "L1C", "D1C", "S1C" };
    runEpoch(*t.model, { sat("G11", System::GPS, obs) });
    EXPECT_EQ(latch(*t.model, "GPS"), (Table{ { "L1/C", "C1C" }, { "L1/L", "L1C" } }));
}

// --------------------------------------------------------------------------
// Galileo
// --------------------------------------------------------------------------

/// Every listed Galileo signal on every satellite: C tracking (listed first) wins the tie on
/// E1, Q on E5a, and E5b takes its only listed signal.
TEST(PppModelSignalSelection, GalileoTieGoesToTheFirstListedSignalOnEveryBand) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(kGalList)));
    const std::vector<std::string> all{ "C1C", "C1X", "C5Q", "C5X", "C7Q",
                                        "L1C", "L1X", "L5Q", "L5X", "L7Q" };
    runEpoch(*t.model, { sat("E01", System::Galileo, all), sat("E02", System::Galileo, all) });
    EXPECT_EQ(latch(*t.model, "Galileo"), (Table{
        { "E1/C", "C1C" },  { "E1/L", "L1C" },
        { "E5a/C", "C5Q" }, { "E5a/L", "L5Q" },
        { "E5b/C", "C7Q" }, { "E5b/L", "L7Q" } }));
}

/// A receiver that tracks only the combined (X) channels: the first priorities (C on E1, Q on
/// E5a) are absent, both bands fall through to X, and E5b, not tracked, has no latch.
TEST(PppModelSignalSelection, GalileoAbsentFirstPriorityFallsThroughToX) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(kGalList)));
    const std::vector<std::string> x_only{ "C1X", "L1X", "C5X", "L5X" };
    runEpoch(*t.model, { sat("E03", System::Galileo, x_only), sat("E04", System::Galileo, x_only) });
    EXPECT_EQ(latch(*t.model, "Galileo"), (Table{
        { "E1/C", "C1X" },  { "E1/L", "L1X" },
        { "E5a/C", "C5X" }, { "E5a/L", "L5X" } }));
}

// --------------------------------------------------------------------------
// BeiDou
// --------------------------------------------------------------------------

/// B1I and B3I on every satellite: B1I is RINEX band 2 (C2I / L2I) and is filed under the
/// band name "B1I"; B3I is RINEX band 6 (C6I / L6I) and is filed under "B3".
TEST(PppModelSignalSelection, BeiDouB1IAndB3IAreChosenFromBands2And6) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(kBdsList)));
    const std::vector<std::string> all{ "C2I", "C2X", "C6I", "C6X", "L2I", "L2X", "L6I", "L6X" };
    runEpoch(*t.model, { sat("C21", System::BeiDou, all), sat("C22", System::BeiDou, all) });
    EXPECT_EQ(latch(*t.model, "BeiDou"), (Table{
        { "B1I/C", "C2I" }, { "B1I/L", "L2I" },
        { "B3/C", "C6I" },  { "B3/L", "L6I" } }));
    EXPECT_EQ(t.model->latchedCanonicalCodeOnBandDigit("BeiDou", '2'), "C2I");
    EXPECT_EQ(t.model->latchedCanonicalPhaseOnBandDigit("BeiDou", '6'), "L6I");
}

/// The I channels (listed first) are absent: both bands fall through to X.
TEST(PppModelSignalSelection, BeiDouAbsentFirstPriorityFallsThroughToX) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(kBdsList)));
    const std::vector<std::string> x_only{ "C2X", "L2X", "C6X", "L6X" };
    runEpoch(*t.model, { sat("C23", System::BeiDou, x_only) });
    EXPECT_EQ(latch(*t.model, "BeiDou"), (Table{
        { "B1I/C", "C2X" }, { "B1I/L", "L2X" },
        { "B3/C", "C6X" },  { "B3/L", "L6X" } }));
}

// --------------------------------------------------------------------------
// Across constellations and epochs
// --------------------------------------------------------------------------

/// The known trap, pinned as the code behaves today: Galileo satellites are observed and
/// Galileo is an active system, but processing.signal_priorities has no Galileo entry, so
/// Galileo gets no latch at all while GPS and BeiDou in the same epoch are decided.  No
/// error is raised; downstream, the per-satellite selection then rejects every Galileo
/// satellite for want of a priority list, so the constellation drops out of the run.
TEST(PppModelSignalSelection, AConstellationWithoutAPriorityEntryIsNeverChosen) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(std::string(kGpsList) + ", " + kBdsList)));
    runEpoch(*t.model, {
        sat("G01", System::GPS, { "C1C", "L1C", "C2W", "L2W" }),
        sat("E01", System::Galileo, { "C1C", "L1C", "C5Q", "L5Q" }),
        sat("E02", System::Galileo, { "C1C", "L1C", "C5Q", "L5Q" }),
        sat("C21", System::BeiDou, { "C2I", "L2I", "C6I", "L6I" }) });
    EXPECT_TRUE(latch(*t.model, "Galileo").empty());
    EXPECT_EQ(t.model->latchedCanonicalCodeOnBandDigit("Galileo", '1'), "");
    EXPECT_EQ(t.model->latchedCanonicalPhaseOnBandDigit("Galileo", '5'), "");
    EXPECT_EQ(latch(*t.model, "GPS").size(), 4u);
    EXPECT_EQ(latch(*t.model, "BeiDou").size(), 4u);
}

/// The choice is made at the constellation's first epoch and then held: a later epoch on
/// which the first priority has vanished does not move it (the band is skipped for that
/// satellite instead), and a constellation first seen on the later epoch is decided there.
TEST(PppModelSignalSelection, TheChoiceIsMadeAtEachConstellationsFirstEpochAndHeld) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(std::string(kGpsList) + ", " + kGalList)));
    runEpoch(*t.model, { sat("G01", System::GPS, { "C1C", "C1W", "L1C", "L1W", "C2W", "L2W" }) });
    EXPECT_EQ(latch(*t.model, "GPS").at("L1/L"), "L1C");
    EXPECT_TRUE(latch(*t.model, "Galileo").empty());

    runEpoch(*t.model, {
        sat("G01", System::GPS, { "C1W", "L1W", "C2W", "L2W" }),
        sat("G02", System::GPS, { "C1W", "L1W", "C2W", "L2W" }),
        sat("E01", System::Galileo, { "C1X", "L1X", "C5X", "L5X" }) });
    EXPECT_EQ(latch(*t.model, "GPS").at("L1/C"), "C1C");
    EXPECT_EQ(latch(*t.model, "GPS").at("L1/L"), "L1C");
    EXPECT_EQ(latch(*t.model, "Galileo"), (Table{
        { "E1/C", "C1X" },  { "E1/L", "L1X" },
        { "E5a/C", "C5X" }, { "E5a/L", "L5X" } }));
}

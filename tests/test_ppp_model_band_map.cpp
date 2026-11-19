/**
 * @file test_ppp_model_band_map.cpp
 * @brief Characterization tables for the PPP model's frequency band map: which RINEX band
 *        digits each frequency mode admits per constellation, which band name each chosen
 *        signal is filed under, the carrier frequency the model assigns it, and the
 *        ionosphere-free combination built from two of those frequencies.
 *
 * Why: the band a signal is filed under decides which signals compete for one slot, and its
 * frequency enters every wavelength, every ionosphere scaling and every ionosphere-free
 * coefficient.  A wrong digit-to-band or band-to-frequency entry is silent: the run still
 * converges, on the wrong physics.  The expected frequencies are written here from the
 * published signal plans, as multiples of the 10.23 MHz fundamental, and the expected
 * combination from the first-order ionosphere law (group delay proportional to 1/f^2), not
 * copied from the code's constants.
 *
 * The band names are read from the per-band latch the model decides at a constellation's
 * first epoch (driven through the public computeModels() entry with a zero receiver
 * position, which decides the latch and then rejects every satellite before any product is
 * read), the digits from the free function getAllowedBandDigits(), and the frequencies from
 * the model's own PppUtils instance.
 */
#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "common/GnssTypes.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "model/PppModel.hpp"
#include "products/ProductParser.hpp"
#include "rinex/RinexParser.hpp"
#include "support/TestTempDir.hpp"

#include <Eigen/Dense>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <vector>

using namespace GnssProcessor;

namespace {

/// The GNSS fundamental frequency (Hz); every carrier below is an exact multiple of it.
constexpr double kF0 = 10.23e6;
/// Speed of light in vacuum (m/s), the defined value.
constexpr double kC = 299792458.0;

/// Published carrier frequencies (Hz), by constellation and band name.
const std::map<std::string, std::map<std::string, double>>& publishedBandFrequency() {
    static const std::map<std::string, std::map<std::string, double>> t = {
        { "GPS",     { { "L1", 154.0 * kF0 }, { "L2", 120.0 * kF0 }, { "L5", 115.0 * kF0 } } },
        { "Galileo", { { "E1", 154.0 * kF0 }, { "E5a", 115.0 * kF0 }, { "E5b", 118.0 * kF0 },
                       { "E6", 125.0 * kF0 }, { "E5", 116.5 * kF0 } } },
        { "BeiDou",  { { "B1C", 154.0 * kF0 }, { "B1I", 152.6 * kF0 }, { "B2a", 115.0 * kF0 },
                       { "B2b", 118.0 * kF0 }, { "B2ab", 116.5 * kF0 }, { "B3", 124.0 * kF0 } } },
    };
    return t;
}

/// A PPP-RTK configuration with the given signal_priorities members.
std::string configText(const std::string& priorities) {
    return std::string(R"({
        "processing": {
            "mode": "ppp-rtk",
            "active_systems": ["GPS", "Galileo", "BeiDou"],
            "signal_priorities": {)") + priorities + R"(}
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
        const std::filesystem::path path = gnss_test::tempDir() / "ppp_model_band_map.json";
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

/// One epoch through computeModels with a zero receiver position: the latch is decided,
/// then every satellite is rejected before geometry reads any product.
void runEpoch(PppModel& m, const std::vector<RinexObservation>& epoch) {
    PppState state;
    state.position.setZero();
    std::vector<PppObservation> rows;
    m.computeModels(epoch, state, rows);
    ASSERT_TRUE(rows.empty()) << "the latch-only harness modelled a row; its premise broke";
}

/// Every RINEX band of the three constellations, one tracking attribute per band, code and phase.
const char* kEveryBandLists =
    R"("GPS": ["C1C", "C2W", "C5Q", "L1C", "L2W", "L5Q"],
       "Galileo": ["C1C", "C5Q", "C7Q", "C6C", "C8Q", "L1C", "L5Q", "L7Q", "L6C", "L8Q"],
       "BeiDou": ["C1X", "C2I", "C5X", "C7I", "C8X", "C6I", "L1X", "L2I", "L5X", "L7I", "L8X", "L6I"])";

/// One epoch on which every signal of kEveryBandLists is reported.
std::vector<RinexObservation> everyBandEpoch() {
    return {
        sat("G01", System::GPS, { "C1C", "C2W", "C5Q", "L1C", "L2W", "L5Q" }),
        sat("E01", System::Galileo,
            { "C1C", "C5Q", "C7Q", "C6C", "C8Q", "L1C", "L5Q", "L7Q", "L6C", "L8Q" }),
        sat("C21", System::BeiDou,
            { "C1X", "C2I", "C5X", "C7I", "C8X", "C6I", "L1X", "L2I", "L5X", "L7I", "L8X", "L6I" }),
    };
}

using Digits = std::set<char>;

}  // namespace

// --------------------------------------------------------------------------
// Band digits per frequency mode
// --------------------------------------------------------------------------

/// Dual and triple frequency per constellation, as RINEX band digits.  BeiDou dual is
/// B1I + B3I (digits 2 and 6), not B1C; Galileo dual is E1 + E5a (1 and 5).
TEST(PppModelBandMap, DualAndTripleModesAdmitThePlannedBands) {
    EXPECT_EQ(getAllowedBandDigits("GPS", FrequencyMode::Dual),        (Digits{ '1', '2' }));
    EXPECT_EQ(getAllowedBandDigits("GPS", FrequencyMode::Triple),      (Digits{ '1', '2', '5' }));
    EXPECT_EQ(getAllowedBandDigits("Galileo", FrequencyMode::Dual),    (Digits{ '1', '5' }));
    EXPECT_EQ(getAllowedBandDigits("Galileo", FrequencyMode::Triple),  (Digits{ '1', '5', '7' }));
    EXPECT_EQ(getAllowedBandDigits("BeiDou", FrequencyMode::Dual),     (Digits{ '2', '6' }));
    EXPECT_EQ(getAllowedBandDigits("BeiDou", FrequencyMode::Triple),   (Digits{ '2', '5', '6' }));
    EXPECT_EQ(getAllowedBandDigits("GLONASS", FrequencyMode::Dual),    (Digits{ '1', '2' }));
    EXPECT_EQ(getAllowedBandDigits("GLONASS", FrequencyMode::Triple),  (Digits{ '1', '2', '3' }));
    EXPECT_EQ(getAllowedBandDigits("QZSS", FrequencyMode::Dual),       (Digits{ '1', '2' }));
    EXPECT_EQ(getAllowedBandDigits("SBAS", FrequencyMode::Dual),       (Digits{ '1', '5' }));
    EXPECT_EQ(getAllowedBandDigits("SBAS", FrequencyMode::Triple),     (Digits{ '1', '5' }));
}

/// The all-bands mode admits every band the model knows for the constellation, and the
/// independent mode starts from the same list (its per-satellite pruning happens later).
TEST(PppModelBandMap, AllModeAdmitsEveryKnownBandAndIndependentMatchesIt) {
    EXPECT_EQ(getAllowedBandDigits("GPS", FrequencyMode::All),     (Digits{ '1', '2', '5' }));
    EXPECT_EQ(getAllowedBandDigits("Galileo", FrequencyMode::All), (Digits{ '1', '5', '6', '7', '8' }));
    EXPECT_EQ(getAllowedBandDigits("BeiDou", FrequencyMode::All),
              (Digits{ '1', '2', '5', '6', '7', '8' }));
    EXPECT_EQ(getAllowedBandDigits("GLONASS", FrequencyMode::All), (Digits{ '1', '2', '3', '4', '6' }));
    EXPECT_EQ(getAllowedBandDigits("QZSS", FrequencyMode::All),    (Digits{ '1', '2', '5', '6' }));
    for (const char* cons : { "GPS", "Galileo", "BeiDou", "GLONASS", "QZSS", "SBAS" })
        EXPECT_EQ(getAllowedBandDigits(cons, FrequencyMode::Independent),
                  getAllowedBandDigits(cons, FrequencyMode::All)) << cons;
}

/// A name outside the table (another constellation, a different spelling, empty) admits
/// no band at all rather than a default set.
TEST(PppModelBandMap, AnUnknownConstellationNameAdmitsNoBand) {
    EXPECT_TRUE(getAllowedBandDigits("IRNSS", FrequencyMode::Dual).empty());
    EXPECT_TRUE(getAllowedBandDigits("gps", FrequencyMode::Dual).empty());
    EXPECT_TRUE(getAllowedBandDigits("Beidou", FrequencyMode::All).empty());
    EXPECT_TRUE(getAllowedBandDigits("", FrequencyMode::Triple).empty());
}

// --------------------------------------------------------------------------
// Band name per chosen signal
// --------------------------------------------------------------------------

/// Every RINEX band digit of GPS, Galileo and BeiDou is filed under its published band
/// name, for code and phase alike.  The BeiDou rows are the ones most easily confused:
/// digit 1 is B1C, digit 2 is B1I, digit 6 is B3, digit 8 is B2(a+b).
TEST(PppModelBandMap, EveryRinexBandDigitIsFiledUnderItsPublishedBandName) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(kEveryBandLists)));
    runEpoch(*t.model, everyBandEpoch());

    std::map<std::string, std::string> band_of;   // "<constellation>:<signal>" -> band
    for (const auto& row : t.model->exportCanonicalSignals())
        band_of[row.constellation + ":" + row.signal] = row.band;

    const std::map<std::string, std::string> expected = {
        { "GPS:C1C", "L1" },  { "GPS:L1C", "L1" },
        { "GPS:C2W", "L2" },  { "GPS:L2W", "L2" },
        { "GPS:C5Q", "L5" },  { "GPS:L5Q", "L5" },
        { "Galileo:C1C", "E1" },  { "Galileo:L1C", "E1" },
        { "Galileo:C5Q", "E5a" }, { "Galileo:L5Q", "E5a" },
        { "Galileo:C7Q", "E5b" }, { "Galileo:L7Q", "E5b" },
        { "Galileo:C6C", "E6" },  { "Galileo:L6C", "E6" },
        { "Galileo:C8Q", "E5" },  { "Galileo:L8Q", "E5" },
        { "BeiDou:C1X", "B1C" },  { "BeiDou:L1X", "B1C" },
        { "BeiDou:C2I", "B1I" },  { "BeiDou:L2I", "B1I" },
        { "BeiDou:C5X", "B2a" },  { "BeiDou:L5X", "B2a" },
        { "BeiDou:C7I", "B2b" },  { "BeiDou:L7I", "B2b" },
        { "BeiDou:C8X", "B2ab" }, { "BeiDou:L8X", "B2ab" },
        { "BeiDou:C6I", "B3" },   { "BeiDou:L6I", "B3" },
    };
    EXPECT_EQ(band_of, expected);
}

// --------------------------------------------------------------------------
// Frequency per chosen signal
// --------------------------------------------------------------------------

/// The carrier frequency the model assigns to each signal of the project's signal sets,
/// against the published value.  Code and phase of one band share the carrier.
TEST(PppModelBandMap, ChosenSignalsCarryThePublishedFrequencies) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(R"("GPS": ["C1C", "L1C"])")));
    const PppUtils& u = t.model->pppUtils();
    struct Row { System sys; const char* signal; double published_hz; };
    const Row rows[] = {
        { System::GPS, "C1C", 154.0 * kF0 },     { System::GPS, "L1C", 154.0 * kF0 },
        { System::GPS, "C1W", 154.0 * kF0 },     { System::GPS, "L1W", 154.0 * kF0 },
        { System::GPS, "C2W", 120.0 * kF0 },     { System::GPS, "L2W", 120.0 * kF0 },
        { System::GPS, "C2L", 120.0 * kF0 },     { System::GPS, "L2L", 120.0 * kF0 },
        { System::GPS, "C5Q", 115.0 * kF0 },     { System::GPS, "L5Q", 115.0 * kF0 },
        { System::Galileo, "C1C", 154.0 * kF0 }, { System::Galileo, "L1X", 154.0 * kF0 },
        { System::Galileo, "C5Q", 115.0 * kF0 }, { System::Galileo, "L5X", 115.0 * kF0 },
        { System::Galileo, "C7Q", 118.0 * kF0 }, { System::Galileo, "L7Q", 118.0 * kF0 },
        { System::Galileo, "L6C", 125.0 * kF0 }, { System::Galileo, "L8Q", 116.5 * kF0 },
        { System::BeiDou, "C2I", 152.6 * kF0 },  { System::BeiDou, "L2I", 152.6 * kF0 },
        { System::BeiDou, "C6I", 124.0 * kF0 },  { System::BeiDou, "L6I", 124.0 * kF0 },
        { System::BeiDou, "L1X", 154.0 * kF0 },  { System::BeiDou, "L5X", 115.0 * kF0 },
        { System::BeiDou, "L7I", 118.0 * kF0 },  { System::BeiDou, "L8X", 116.5 * kF0 },
    };
    for (const auto& r : rows)
        EXPECT_NEAR(u.getFrequency(r.signal, r.sys, ""), r.published_hz, 1.0)
            << systemName(r.sys) << " " << r.signal;
}

/// The join of the two maps: for every signal the latch filed under a band, the frequency
/// the model assigns equals the published frequency of that band name.  A band table and a
/// frequency table that drift apart (one digit renamed in only one of them) fail here.
TEST(PppModelBandMap, EveryChosenSignalsFrequencyIsThatOfItsBandName) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(kEveryBandLists)));
    runEpoch(*t.model, everyBandEpoch());
    const auto rows = t.model->exportCanonicalSignals();
    ASSERT_EQ(rows.size(), 28u);
    const PppUtils& u = t.model->pppUtils();
    for (const auto& row : rows) {
        const auto cit = publishedBandFrequency().find(row.constellation);
        ASSERT_NE(cit, publishedBandFrequency().end()) << row.constellation;
        const auto bit = cit->second.find(row.band);
        ASSERT_NE(bit, cit->second.end()) << row.constellation << " band " << row.band;
        EXPECT_NEAR(u.getFrequency(row.signal, systemFromName(row.constellation), ""),
                    bit->second, 1.0)
            << row.constellation << " " << row.signal << " filed under " << row.band;
    }
}

/// The wavelength is the speed of light over the assigned frequency (for example GPS L1
/// about 0.1903 m, L2 about 0.2442 m, BeiDou B1I about 0.1920 m).
TEST(PppModelBandMap, WavelengthIsTheSpeedOfLightOverTheFrequency) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(R"("GPS": ["C1C", "L1C"])")));
    const PppUtils& u = t.model->pppUtils();
    EXPECT_NEAR(u.getWavelength(System::GPS, "L1C"), kC / (154.0 * kF0), 1e-12);
    EXPECT_NEAR(u.getWavelength(System::GPS, "L2W"), kC / (120.0 * kF0), 1e-12);
    EXPECT_NEAR(u.getWavelength(System::GPS, "L5Q"), kC / (115.0 * kF0), 1e-12);
    EXPECT_NEAR(u.getWavelength(System::Galileo, "L5Q"), kC / (115.0 * kF0), 1e-12);
    EXPECT_NEAR(u.getWavelength(System::Galileo, "L7Q"), kC / (118.0 * kF0), 1e-12);
    EXPECT_NEAR(u.getWavelength(System::BeiDou, "L2I"), kC / (152.6 * kF0), 1e-12);
    EXPECT_NEAR(u.getWavelength(System::BeiDou, "L6I"), kC / (124.0 * kF0), 1e-12);
    EXPECT_NEAR(u.getWavelength(System::GPS, "L1C"), 0.19029367, 1e-8);
}

// --------------------------------------------------------------------------
// The ionosphere-free combination of two chosen bands
// --------------------------------------------------------------------------

/// First-order ionosphere: the group delay is 40.3 TEC / f^2, so a delay I on band 1 is
/// I f1^2 / f2^2 on band 2, and the combination (f1^2 P1 - f2^2 P2) / (f1^2 - f2^2) returns
/// the geometric range.  Pinned for the dual-frequency pair of each constellation; a
/// coefficient with the wrong sign or a swapped frequency leaves metres of ionosphere.
TEST(PppModelBandMap, IonoFreeCombinationRemovesAFirstOrderIonosphereDelay) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(R"("GPS": ["C1C", "L1C"])")));
    struct Pair { System sys; const char* s1; const char* s2; double f1; double f2; };
    const Pair pairs[] = {
        { System::GPS,     "C1W", "C2W", 154.0 * kF0, 120.0 * kF0 },
        { System::GPS,     "C1C", "C5Q", 154.0 * kF0, 115.0 * kF0 },
        { System::Galileo, "C1C", "C5Q", 154.0 * kF0, 115.0 * kF0 },
        { System::BeiDou,  "C2I", "C6I", 152.6 * kF0, 124.0 * kF0 },
    };
    const double range = 2.2e7;
    const double iono1 = 5.0;
    for (const auto& p : pairs) {
        const double iono2 = iono1 * (p.f1 * p.f1) / (p.f2 * p.f2);
        const std::map<std::string, double> m = { { p.s1, range + iono1 }, { p.s2, range + iono2 } };
        EXPECT_NEAR(t.model->computeIonoFreeCombination(m, p.sys, p.s1, p.s2), range, 1e-6)
            << systemName(p.sys) << " " << p.s1 << "/" << p.s2;
    }
}

/// The two coefficients themselves for GPS L1/L2: alpha = f1^2 / (f1^2 - f2^2), about
/// +2.546, on the first signal; beta = -f2^2 / (f1^2 - f2^2), about -1.546, on the second.
TEST(PppModelBandMap, IonoFreeCoefficientsHaveThePublishedSignsAndSizes) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(R"("GPS": ["C1C", "L1C"])")));
    const double f1 = 154.0 * kF0, f2 = 120.0 * kF0;
    const double alpha = f1 * f1 / (f1 * f1 - f2 * f2);
    const double beta = -f2 * f2 / (f1 * f1 - f2 * f2);
    EXPECT_NEAR(alpha, 2.546, 1e-3);   // the textbook value, guarding the formula above
    EXPECT_NEAR(t.model->computeIonoFreeCombination({ { "C1W", 1.0 }, { "C2W", 0.0 } },
                                                    System::GPS, "C1W", "C2W"),
                alpha, 1e-12);
    // A unit on the second signal alone: beta, negative.  A zero first value is a valid
    // measurement here (only a missing key is rejected).
    EXPECT_NEAR(t.model->computeIonoFreeCombination({ { "C1W", 0.0 }, { "C2W", 1.0 } },
                                                    System::GPS, "C1W", "C2W"),
                beta, 1e-12);
}

/// Two signals of one band have one frequency and no combination: the model returns 0, and
/// it returns 0 as well when either measurement is missing.
TEST(PppModelBandMap, IonoFreeCombinationOfOneBandOrAMissingSignalIsZero) {
    ModelUnderTest t;
    ASSERT_TRUE(t.load(configText(R"("GPS": ["C1C", "L1C"])")));
    EXPECT_EQ(t.model->computeIonoFreeCombination({ { "C1C", 2.2e7 }, { "C1W", 2.2e7 + 1.0 } },
                                                  System::GPS, "C1C", "C1W"), 0.0);
    EXPECT_EQ(t.model->computeIonoFreeCombination({ { "C1C", 2.2e7 } },
                                                  System::GPS, "C1C", "C2W"), 0.0);
}

/**
 * @file test_rx_sat_code_bias_store.cpp
 * @brief The receiver-satellite code-bias store file (filter/RxSatCodeBiasStore.hpp): the JSON round
 *        trip, the refusal of files that are not stores, the identity check that ignores a store of
 *        another receiver or bias source, the product-series identity, and the age rule.
 */

#include <gtest/gtest.h>

#include "filter/RxSatCodeBiasStore.hpp"
#include "filter/StateKeySchema.hpp"
#include "support/TestTempDir.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

using namespace GnssProcessor;

namespace {

namespace fs = std::filesystem;

/// A scratch directory for this test.
fs::path scratch() {
    const fs::path dir = gnss_test::tempDir() / "rscb_store";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

RxSatCodeBiasEntry entry(const std::string& sat, const std::string& sig, double value, double sigma,
                         int week, double tow) {
    RxSatCodeBiasEntry e;
    e.satellite = sat;
    e.signal    = sig;
    e.value_m   = value;
    e.sigma_m   = sigma;
    e.updated   = GnssTime{ week, tow };
    return e;
}

RxSatCodeBiasStore sampleStore() {
    RxSatCodeBiasStore s;
    s.identity = RxSatCodeBiasIdentity{ "TRIMBLE ALLOY", "5.56", "COD0MGXFIN" };
    for (const auto& e : { entry("G01", "C1W", 0.123456789012345, 0.0071, 2418, 86400.5),
                           entry("G01", "C2W", -0.0457, 0.0123, 2418, 86400.5),
                           entry("G32", "C1W", 0.5e-3, 0.001, 2417, 12.25) })
        s.entries[StateKeys::makeRxSatCodeBiasKey(e.satellite, e.signal)] = e;
    return s;
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream out(p);
    out << text;
}

}  // namespace

TEST(RxSatCodeBiasStoreTest, AWrittenStoreIsReadBackExactly) {
    const fs::path path = scratch() / "roundtrip.json";
    const RxSatCodeBiasStore s = sampleStore();
    ASSERT_TRUE(writeRxSatCodeBiasStore(path, s));

    std::string why;
    const auto back = readRxSatCodeBiasStore(path, &why);
    ASSERT_TRUE(back.has_value()) << why;
    EXPECT_EQ(back->identity.receiver_type, "TRIMBLE ALLOY");
    EXPECT_EQ(back->identity.receiver_version, "5.56");
    EXPECT_EQ(back->identity.bias_source, "COD0MGXFIN");
    ASSERT_EQ(back->entries.size(), s.entries.size());
    for (const auto& [key, e] : s.entries) {
        ASSERT_TRUE(back->entries.count(key)) << key;
        const RxSatCodeBiasEntry& b = back->entries.at(key);
        EXPECT_EQ(b.satellite, e.satellite) << key;
        EXPECT_EQ(b.signal, e.signal) << key;
        EXPECT_EQ(b.value_m, e.value_m) << key;       // doubles survive the text exactly
        EXPECT_EQ(b.sigma_m, e.sigma_m) << key;
        EXPECT_EQ(b.updated.week, e.updated.week) << key;
        EXPECT_EQ(b.updated.tow, e.updated.tow) << key;
    }
    // Writing again over an existing file replaces it.
    RxSatCodeBiasStore smaller = s;
    smaller.entries.erase("G32|C1W");
    ASSERT_TRUE(writeRxSatCodeBiasStore(path, smaller));
    EXPECT_EQ(readRxSatCodeBiasStore(path)->entries.size(), 2u);
}

TEST(RxSatCodeBiasStoreTest, AnEmptyStoreIsAValidFile) {
    const fs::path path = scratch() / "empty.json";
    RxSatCodeBiasStore s;
    s.identity = RxSatCodeBiasIdentity{ "SEPT POLARX5S", "", "none" };
    ASSERT_TRUE(writeRxSatCodeBiasStore(path, s));
    const auto back = readRxSatCodeBiasStore(path);
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(back->entries.empty());
    EXPECT_EQ(back->identity.receiver_version, "");
}

TEST(RxSatCodeBiasStoreTest, WritingCreatesTheDirectory) {
    const fs::path path = scratch() / "nested" / "dir" / "store.json";
    ASSERT_TRUE(writeRxSatCodeBiasStore(path, sampleStore()));
    EXPECT_TRUE(readRxSatCodeBiasStore(path).has_value());
}

TEST(RxSatCodeBiasStoreTest, FilesThatAreNoStoreAreRefusedWithAReason) {
    const fs::path dir = scratch();
    std::string why;

    EXPECT_FALSE(readRxSatCodeBiasStore(dir / "does_not_exist.json", &why).has_value());
    EXPECT_FALSE(why.empty());

    writeText(dir / "not_json.json", "this is not json");
    EXPECT_FALSE(readRxSatCodeBiasStore(dir / "not_json.json", &why).has_value());
    EXPECT_NE(why.find("JSON"), std::string::npos) << why;

    writeText(dir / "other_marker.json", R"({"marker": "something else", "schema": 1, "entries": []})");
    EXPECT_FALSE(readRxSatCodeBiasStore(dir / "other_marker.json", &why).has_value());
    EXPECT_NE(why.find("marker"), std::string::npos) << why;

    writeText(dir / "other_schema.json",
              std::string(R"({"marker": ")") + kRxSatCodeBiasStoreMarker + R"(", "schema": 99, "entries": []})");
    EXPECT_FALSE(readRxSatCodeBiasStore(dir / "other_schema.json", &why).has_value());
    EXPECT_NE(why.find("schema"), std::string::npos) << why;

    // A phase signal is no code: the entry's key is no key of the family.
    writeText(dir / "bad_key.json",
              std::string(R"({"marker": ")") + kRxSatCodeBiasStoreMarker + R"(", "schema": 1, "entries": [)"
              R"({"satellite": "G01", "signal": "L1C", "value_m": 0.1, "sigma_m": 0.01, "week": 1, "tow": 2.0}]})");
    EXPECT_FALSE(readRxSatCodeBiasStore(dir / "bad_key.json", &why).has_value());
    EXPECT_NE(why.find("key"), std::string::npos) << why;

    // A zero sigma would be a state that takes no data.
    writeText(dir / "zero_sigma.json",
              std::string(R"({"marker": ")") + kRxSatCodeBiasStoreMarker + R"(", "schema": 1, "entries": [)"
              R"({"satellite": "G01", "signal": "C1W", "value_m": 0.1, "sigma_m": 0.0, "week": 1, "tow": 2.0}]})");
    EXPECT_FALSE(readRxSatCodeBiasStore(dir / "zero_sigma.json", &why).has_value());

    // A missing member.
    writeText(dir / "missing_member.json",
              std::string(R"({"marker": ")") + kRxSatCodeBiasStoreMarker + R"(", "schema": 1, "entries": [)"
              R"({"satellite": "G01", "signal": "C1W", "value_m": 0.1}]})");
    EXPECT_FALSE(readRxSatCodeBiasStore(dir / "missing_member.json", &why).has_value());
}

TEST(RxSatCodeBiasStoreTest, AStoreOfAnotherReceiverOrBiasSourceIsNotUsable) {
    const RxSatCodeBiasStore s = sampleStore();
    RxSatCodeBiasIdentity run = s.identity;
    EXPECT_EQ(checkRxSatCodeBiasStore(s, run), RxSatCodeBiasVerdict::Usable);

    RxSatCodeBiasIdentity other_type = run;
    other_type.receiver_type = "JAVAD TRE_3 DELTA";
    EXPECT_EQ(checkRxSatCodeBiasStore(s, other_type), RxSatCodeBiasVerdict::ReceiverTypeDiffers);

    RxSatCodeBiasIdentity other_version = run;
    other_version.receiver_version = "5.57";
    EXPECT_EQ(checkRxSatCodeBiasStore(s, other_version), RxSatCodeBiasVerdict::ReceiverVersionDiffers);

    RxSatCodeBiasIdentity other_source = run;
    other_source.bias_source = "GRG0MGXFIN";
    EXPECT_EQ(checkRxSatCodeBiasStore(s, other_source), RxSatCodeBiasVerdict::BiasSourceDiffers);

    // The first field that differs is named.
    RxSatCodeBiasIdentity all = { "X", "Y", "Z" };
    EXPECT_EQ(checkRxSatCodeBiasStore(s, all), RxSatCodeBiasVerdict::ReceiverTypeDiffers);

    EXPECT_STREQ(rxSatCodeBiasVerdictText(RxSatCodeBiasVerdict::Usable), "usable");
    EXPECT_STREQ(rxSatCodeBiasVerdictText(RxSatCodeBiasVerdict::ReceiverTypeDiffers), "receiver type differs");
    EXPECT_STREQ(rxSatCodeBiasVerdictText(RxSatCodeBiasVerdict::ReceiverVersionDiffers), "receiver version differs");
    EXPECT_STREQ(rxSatCodeBiasVerdictText(RxSatCodeBiasVerdict::BiasSourceDiffers), "bias source differs");
}

TEST(RxSatCodeBiasStoreTest, TheBlankPaddingOfARinexFieldDoesNotMakeAnotherReceiver) {
    RxSatCodeBiasStore s = sampleStore();
    RxSatCodeBiasIdentity run{ "TRIMBLE ALLOY    ", "  5.56  ", "COD0MGXFIN " };
    EXPECT_EQ(checkRxSatCodeBiasStore(s, run), RxSatCodeBiasVerdict::Usable);
    EXPECT_EQ(normalizeIdentityText("  TRIMBLE ALLOY \t"), "TRIMBLE ALLOY");
    EXPECT_EQ(normalizeIdentityText("   "), "");
}

TEST(RxSatCodeBiasStoreTest, TheBiasSourceIsTheProductSeriesOfTheFileName) {
    // The daily files of one product share the series, so a store written one day serves the next.
    EXPECT_EQ(biasSourceIdentity("COD0MGXFIN_20241450000_01D_01D_OSB.BIA"), "COD0MGXFIN");
    EXPECT_EQ(biasSourceIdentity("COD0MGXFIN_20241460000_01D_01D_OSB.BIA"), "COD0MGXFIN");
    EXPECT_NE(biasSourceIdentity("GRG0MGXFIN_20241450000_01D_01D_OSB.BIA"),
              biasSourceIdentity("COD0MGXFIN_20241450000_01D_01D_OSB.BIA"));
    // Any other name stands for itself.
    EXPECT_EQ(biasSourceIdentity("my_osb.bia"), "my_osb.bia");
    EXPECT_EQ(biasSourceIdentity("COD0MGXFIN_latest.BIA"), "COD0MGXFIN_latest.BIA");
    // No file: the series "none".
    EXPECT_EQ(biasSourceIdentity(""), "none");
    EXPECT_EQ(biasSourceIdentity("  "), "none");
}

TEST(RxSatCodeBiasStoreTest, TheVarianceGrowsByTheRandomWalkOverTheAgeOfTheEntry) {
    constexpr double q = 1.0e-10;
    EXPECT_DOUBLE_EQ(ageInflatedVariance(0.01, q, 0.0), 1.0e-4);
    EXPECT_DOUBLE_EQ(ageInflatedVariance(0.01, q, 86400.0), 1.0e-4 + q * 86400.0);
    // An entry stamped after the birth is not shrunk.
    EXPECT_DOUBLE_EQ(ageInflatedVariance(0.01, q, -50.0), 1.0e-4);

    EXPECT_DOUBLE_EQ(secondsBetween(GnssTime{ 2418, 100.0 }, GnssTime{ 2418, 40.0 }), 60.0);
    EXPECT_DOUBLE_EQ(secondsBetween(GnssTime{ 2418, 10.0 }, GnssTime{ 2417, 604790.0 }), 20.0);
    EXPECT_DOUBLE_EQ(secondsBetween(GnssTime{ 2417, 604790.0 }, GnssTime{ 2418, 10.0 }), -20.0);
}

TEST(RxSatCodeBiasStoreTest, ThePriorOfABornStateIsTheEntrysValueWithItsAgedVariance) {
    const RxSatCodeBiasStore s = sampleStore();
    constexpr double q = 1.0e-10;
    // The entry G01|C1W was updated at (2418, 86400.5); one day later.
    const auto prior = storedRxSatCodeBiasPrior(s, "G01|C1W", GnssTime{ 2418, 86400.5 + 86400.0 }, q);
    ASSERT_TRUE(prior.has_value());
    EXPECT_EQ(prior->value_m, 0.123456789012345);
    EXPECT_DOUBLE_EQ(prior->variance_m2, 0.0071 * 0.0071 + q * 86400.0);
    // An entry the store lacks has no prior.
    EXPECT_FALSE(storedRxSatCodeBiasPrior(s, "G05|C1W", GnssTime{ 2418, 0.0 }, q).has_value());
}

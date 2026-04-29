/**
 * @file test_state_key_schema.cpp
 * @brief Unit tests for filter/StateKeySchema make/parse round-trips.
 *
 * StateKeySchema is the single source of truth for every dynamic state-vector
 * key format. CLAUDE.md flags it as critical: silent format drift here breaks
 * call sites in MeasurementModel, StateInitializer, StateManager,
 * AmbiguityResolver, and DdPivotManager. These tests pin down each format
 * exactly so an accidental delimiter change or empty-field bug fails fast.
 *
 * The header is intentionally header-only / all-inline, so this TU links
 * trivially without pulling any additional .cpp into gnss_tests.
 */

#include <gtest/gtest.h>
#include "filter/StateKeySchema.hpp"

#include <optional>
#include <string>

namespace GnssProcessor {
namespace {

using namespace ::GnssProcessor::StateKeys;

// ============================================================================
// 1. PPP ambiguity keys — "<satellite>_<signal>"
// ============================================================================

TEST(StateKeySchemaTest, AmbiguitySimpleRoundTrip) {
    const std::string key = makeAmbiguityKey("G01", "L1C");
    EXPECT_EQ(key, "G01_L1C");

    auto parts = parseAmbiguityKey(key);
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->satellite, "G01");
    EXPECT_EQ(parts->signal, "L1C");
}

// Iono-free combos embed the underscore inside `signal`. The parser MUST split
// only on the first '_' so the IF token survives intact.
TEST(StateKeySchemaTest, AmbiguityIonoFreeComboPreservesEmbeddedDelimiter) {
    const std::string key = makeAmbiguityKey("G01", "L1C_L5Q");
    EXPECT_EQ(key, "G01_L1C_L5Q");

    auto parts = parseAmbiguityKey(key);
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->satellite, "G01");
    EXPECT_EQ(parts->signal, "L1C_L5Q");
}

TEST(StateKeySchemaTest, AmbiguityParseRejectsMalformed) {
    EXPECT_FALSE(parseAmbiguityKey("").has_value());
    EXPECT_FALSE(parseAmbiguityKey("G01").has_value());      // No delimiter
    EXPECT_FALSE(parseAmbiguityKey("_L1C").has_value());     // Empty satellite
    EXPECT_FALSE(parseAmbiguityKey("G01_").has_value());     // Empty signal
}

// ============================================================================
// 2. DD ambiguity keys — "DD|<cons>|<sat_i>|<code>|<pivot>"
// ============================================================================

TEST(StateKeySchemaTest, DdAmbiguityRoundTrip) {
    const std::string key = makeDdAmbiguityKey("GPS", "G05", "L1", "G01");
    EXPECT_EQ(key, "DD|GPS|G05|L1|G01");

    EXPECT_TRUE(isDdAmbiguityKey(key));

    auto parts = parseDdAmbiguityKey(key);
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->cons, "GPS");
    EXPECT_EQ(parts->sat_i, "G05");
    EXPECT_EQ(parts->code, "L1");
    EXPECT_EQ(parts->pivot, "G01");
}

TEST(StateKeySchemaTest, DdAmbiguityIsDdRejectsNonDd) {
    EXPECT_FALSE(isDdAmbiguityKey(""));
    EXPECT_FALSE(isDdAmbiguityKey("DD"));            // Prefix only, no payload
    EXPECT_FALSE(isDdAmbiguityKey("DD|"));           // Empty payload (size <= prefix+1)
    EXPECT_FALSE(isDdAmbiguityKey("DDX|GPS"));       // Wrong delimiter after prefix
    EXPECT_FALSE(isDdAmbiguityKey("G01_L1C"));       // PPP ambiguity, not DD
}

TEST(StateKeySchemaTest, DdAmbiguityParseRejectsMalformed) {
    // Wrong shape (missing pipes / fields).
    EXPECT_FALSE(parseDdAmbiguityKey("DD|GPS|G05|L1").has_value());        // 3 pipes only
    EXPECT_FALSE(parseDdAmbiguityKey("DD|GPS|G05|L1|G01|extra").has_value());// 5 pipes
    EXPECT_FALSE(parseDdAmbiguityKey("DD||G05|L1|G01").has_value());       // Empty cons
    EXPECT_FALSE(parseDdAmbiguityKey("DD|GPS||L1|G01").has_value());       // Empty sat_i
    EXPECT_FALSE(parseDdAmbiguityKey("DD|GPS|G05||G01").has_value());      // Empty code
    EXPECT_FALSE(parseDdAmbiguityKey("DD|GPS|G05|L1|").has_value());       // Empty pivot
    EXPECT_FALSE(parseDdAmbiguityKey("PPP|GPS|G05|L1|G01").has_value());   // Wrong prefix
}

// ============================================================================
// 3. Per-satellite ionosphere keys — bare satellite ID
// ============================================================================

TEST(StateKeySchemaTest, IonosphereKeyIsBareSatelliteId) {
    EXPECT_EQ(makeIonosphereKey("G01"), "G01");
    EXPECT_EQ(makeIonosphereKey("E11"), "E11");
}

// ============================================================================
// 4. Receiver code-bias keys — IFB / DCB
// ============================================================================

TEST(StateKeySchemaTest, IfbKeyShape) {
    EXPECT_EQ(makeIfbKey("GPS", '2'), "GPS_IFB2");
    EXPECT_EQ(makeIfbKey("GPS", '5'), "GPS_IFB5");
    EXPECT_EQ(makeIfbKey("Galileo", '7'), "Galileo_IFB7");
}

TEST(StateKeySchemaTest, DcbKeyShape) {
    EXPECT_EQ(makeDcbKey("GPS"), "GPS_DCB");
    EXPECT_EQ(makeDcbKey("BeiDou"), "BeiDou_DCB");
}

// ============================================================================
// 5. Per-satellite signal-bias keys — "<satellite>|<signal>"
// ============================================================================

TEST(StateKeySchemaTest, SatBiasRoundTrip) {
    const std::string key = makeSatBiasKey("G01", "C1W");
    EXPECT_EQ(key, "G01|C1W");

    auto parts = parseSatBiasKey(key);
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->satellite, "G01");
    EXPECT_EQ(parts->signal, "C1W");
}

TEST(StateKeySchemaTest, SatBiasParseRejectsMalformed) {
    EXPECT_FALSE(parseSatBiasKey("").has_value());
    EXPECT_FALSE(parseSatBiasKey("G01").has_value());      // No delimiter
    EXPECT_FALSE(parseSatBiasKey("|C1W").has_value());     // Empty satellite
    EXPECT_FALSE(parseSatBiasKey("G01|").has_value());     // Empty signal
}

// satelliteFromSatBiasKey is intentionally lenient: keys without a pipe are
// treated as if the whole string were the satellite ID. This preserves the
// pre-StateKeySchema StateManager::computeProcessNoise loop's behavior.
TEST(StateKeySchemaTest, SatelliteFromSatBiasKeyHandlesLegacyKeys) {
    EXPECT_EQ(satelliteFromSatBiasKey("G01|C1W"), "G01");
    EXPECT_EQ(satelliteFromSatBiasKey("G01"), "G01");          // Legacy / malformed
    EXPECT_EQ(satelliteFromSatBiasKey("|C1W"), "");            // Pipe-prefixed
    EXPECT_EQ(satelliteFromSatBiasKey(""), "");
}

// ============================================================================
// 6. System-time-offset keys — bare system name
// ============================================================================

TEST(StateKeySchemaTest, SystemTimeOffsetKeyIsBareSystemName) {
    EXPECT_EQ(makeSystemTimeOffsetKey("Galileo"), "Galileo");
    EXPECT_EQ(makeSystemTimeOffsetKey("GLONASS"), "GLONASS");
}

// ============================================================================
// 7. BSSD pivot-seed keys — "<cons>|<band>"
// ============================================================================

TEST(StateKeySchemaTest, PivotSeedKeyShape) {
    EXPECT_EQ(makePivotSeedKey("GPS", "L1"), "GPS|L1");
    EXPECT_EQ(makePivotSeedKey("Galileo", "E5"), "Galileo|E5");
    EXPECT_EQ(makePivotSeedKey("BeiDou", "B2"), "BeiDou|B2");
}

// ============================================================================
// Cross-category sanity: a DD key must not be mistaken for a PPP ambiguity
// (and vice-versa). The parsers' first-delimiter rules handle this implicitly,
// but the assertion is worth making explicit.
// ============================================================================

TEST(StateKeySchemaTest, CategoriesAreDistinguishable) {
    const std::string ppp = makeAmbiguityKey("G01", "L1C");
    const std::string dd  = makeDdAmbiguityKey("GPS", "G05", "L1", "G01");
    const std::string sat = makeSatBiasKey("G01", "C1W");

    EXPECT_FALSE(isDdAmbiguityKey(ppp));
    EXPECT_TRUE(isDdAmbiguityKey(dd));
    EXPECT_FALSE(isDdAmbiguityKey(sat));

    // PPP ambiguity parser rejects DD keys (DD has '|' not '_' at position [2]).
    // It will, however, find an underscore because the prefix is "DD" (no '_'),
    // and DD keys contain no underscore — so parseAmbiguityKey returns nullopt.
    EXPECT_FALSE(parseAmbiguityKey(dd).has_value());

    // SatBias parser uses '|', which DD also uses; but DD has 4 pipes and the
    // SatBias parser splits on the FIRST pipe, so it interprets a DD key as
    // {satellite="DD", signal="GPS|G05|L1|G01"}. That's "valid" by SatBias
    // shape rules — assert this documented behavior so future maintainers
    // know category-ambiguous keys are caller-policed, not parser-policed.
    auto sat_from_dd = parseSatBiasKey(dd);
    ASSERT_TRUE(sat_from_dd.has_value());
    EXPECT_EQ(sat_from_dd->satellite, "DD");
    EXPECT_EQ(sat_from_dd->signal, "GPS|G05|L1|G01");
}

}  // namespace
}  // namespace GnssProcessor

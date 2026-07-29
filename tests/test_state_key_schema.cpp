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

TEST(StateKeySchemaTest, IfbKeyRoundTrip) {
    for (const std::string sys : {"GPS", "Galileo", "BeiDou"}) {
        for (const char band : {'1', '2', '5', '7'}) {
            const std::string key = makeIfbKey(sys, band);
            auto parts = parseIfbKey(key);
            ASSERT_TRUE(parts.has_value()) << key;
            EXPECT_EQ(parts->system, sys) << key;
            EXPECT_EQ(parts->band, band) << key;
        }
    }
}

TEST(StateKeySchemaTest, DcbKeyRoundTrip) {
    for (const std::string sys : {"GPS", "Galileo", "BeiDou"}) {
        auto sys_out = parseDcbKey(makeDcbKey(sys));
        ASSERT_TRUE(sys_out.has_value()) << sys;
        EXPECT_EQ(*sys_out, sys) << sys;
    }
}

// parseIfbKey / parseDcbKey select ONLY receiver-common per-band code biases —
// the columns a network common-mode pin / S-basis datum acts on.  They must
// reject the per-slot GLONASS form (its own central-channel datum) and each
// other's shape, mirroring the isGlonassPerSlotIfbKey discriminator.
TEST(StateKeySchemaTest, IfbDcbParseRejectsForeignAndMalformed) {
    EXPECT_FALSE(parseIfbKey(makeGlonassPerSlotIfbKey("GLONASS", '1', -4)).has_value());
    EXPECT_FALSE(parseIfbKey("GPS_DCB").has_value());
    EXPECT_FALSE(parseIfbKey("GPS_IFB").has_value());    // no band char
    EXPECT_FALSE(parseIfbKey("GPS_IFB25").has_value());  // multi-char band
    EXPECT_FALSE(parseIfbKey("IFB5").has_value());       // no system
    EXPECT_FALSE(parseIfbKey("").has_value());

    EXPECT_FALSE(parseDcbKey("GPS_IFB5").has_value());
    EXPECT_FALSE(parseDcbKey(makeGlonassPerSlotIfbKey("GLONASS", '2', 3)).has_value());
    EXPECT_FALSE(parseDcbKey("GPS_DCBX").has_value());   // trailing junk
    EXPECT_FALSE(parseDcbKey("DCB").has_value());        // no system
    EXPECT_FALSE(parseDcbKey("").has_value());
}

// ============================================================================
// 4b. Per-FCN-slot GLONASS IFB keys — "GLONASS_IFB<band>_k<±slot>"
// ============================================================================

TEST(StateKeySchemaTest, GlonassPerSlotIfbKeyShape) {
    EXPECT_EQ(makeGlonassPerSlotIfbKey("GLONASS", '1', 5),  "GLONASS_IFB1_k+5");
    EXPECT_EQ(makeGlonassPerSlotIfbKey("GLONASS", '2', -4), "GLONASS_IFB2_k-4");
    EXPECT_EQ(makeGlonassPerSlotIfbKey("GLONASS", '1', 0),  "GLONASS_IFB1_k+0");
}

TEST(StateKeySchemaTest, GlonassPerSlotIfbRoundTrip) {
    for (int k = -7; k <= 6; ++k) {
        for (const char band : {'1', '2'}) {
            const std::string key = makeGlonassPerSlotIfbKey("GLONASS", band, k);
            EXPECT_TRUE(isGlonassPerSlotIfbKey(key)) << key;
            auto parts = parseGlonassPerSlotIfbKey(key);
            ASSERT_TRUE(parts.has_value()) << key;
            EXPECT_EQ(parts->band, band) << key;
            EXPECT_EQ(parts->fcn_slot, k) << key;
        }
    }
}

// THE load-bearing invariant for byte-identical flag-off behavior: the per-slot
// predicate must NOT match any receiver-common code-bias key.  BssdTransform and
// StateManager use isGlonassPerSlotIfbKey() to decide which code-bias columns
// survive single differencing / keep process noise; a false positive on a
// per-band IFB or DCB key would silently change the BSSD solution for
// GPS/Galileo/BeiDou (and break the flag-off byte-identical guarantee).
TEST(StateKeySchemaTest, GlonassPerSlotIfbPredicateRejectsReceiverCommonKeys) {
    EXPECT_FALSE(isGlonassPerSlotIfbKey(makeIfbKey("GPS", '5')));       // GPS_IFB5
    EXPECT_FALSE(isGlonassPerSlotIfbKey(makeIfbKey("GPS", '2')));       // GPS_IFB2
    EXPECT_FALSE(isGlonassPerSlotIfbKey(makeIfbKey("Galileo", '7')));   // Galileo_IFB7
    EXPECT_FALSE(isGlonassPerSlotIfbKey(makeIfbKey("GLONASS", '3')));   // per-band GLONASS_IFB3
    EXPECT_FALSE(isGlonassPerSlotIfbKey(makeDcbKey("GPS")));            // GPS_DCB
    EXPECT_FALSE(isGlonassPerSlotIfbKey(makeDcbKey("GLONASS")));        // GLONASS_DCB
    // ...and it DOES match a genuine per-slot key.
    EXPECT_TRUE(isGlonassPerSlotIfbKey(makeGlonassPerSlotIfbKey("GLONASS", '1', -4)));
}

TEST(StateKeySchemaTest, GlonassPerSlotIfbParseRejectsMalformed) {
    EXPECT_FALSE(parseGlonassPerSlotIfbKey("").has_value());
    EXPECT_FALSE(parseGlonassPerSlotIfbKey("GLONASS_IFB1").has_value());      // No _k slot
    EXPECT_FALSE(parseGlonassPerSlotIfbKey("GLONASS_DCB").has_value());       // DCB, no IFB
    EXPECT_FALSE(parseGlonassPerSlotIfbKey("GLONASS_IFB12_k+5").has_value()); // Multi-char band
    EXPECT_FALSE(parseGlonassPerSlotIfbKey("GLONASS_IFB1_k").has_value());    // Empty slot
    EXPECT_FALSE(parseGlonassPerSlotIfbKey("GLONASS_IFB1_k+").has_value());   // Sign only
    EXPECT_FALSE(parseGlonassPerSlotIfbKey("GLONASS_IFB1_kx").has_value());   // Non-numeric slot
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
// 5b. Per-satellite signal PHASE-bias keys — "<satellite>|<signal>|PB"
// ============================================================================

TEST(StateKeySchemaTest, SatPhaseBiasRoundTrip) {
    const std::string key = makeSatPhaseBiasKey("G01", "L1C");
    EXPECT_EQ(key, "G01|L1C|PB");

    auto parts = parseSatPhaseBiasKey(key);
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->satellite, "G01");
    EXPECT_EQ(parts->signal, "L1C");

    EXPECT_TRUE(isSatPhaseBiasKey(key));
    EXPECT_EQ(satelliteFromSatPhaseBiasKey(key), "G01");
}

TEST(StateKeySchemaTest, SatPhaseBiasParseRejectsMalformed) {
    EXPECT_FALSE(parseSatPhaseBiasKey("").has_value());
    EXPECT_FALSE(parseSatPhaseBiasKey("G01|L1C").has_value());   // No |PB suffix (this is a CODE-bias key)
    EXPECT_FALSE(parseSatPhaseBiasKey("|L1C|PB").has_value());   // Empty satellite
    EXPECT_FALSE(parseSatPhaseBiasKey("G01||PB").has_value());   // Empty signal
    EXPECT_FALSE(parseSatPhaseBiasKey("|PB").has_value());       // No body
}

// The CODE-bias and PHASE-bias key namespaces must be provably disjoint: the
// state lives in separate StateManager maps, but the trailing "|PB" guarantees
// no helper mistakes one for the other.  In particular parseSatBiasKey is never
// invoked on a phase-bias key in production (it would read "L1C|PB" as the
// signal); this test pins that the discriminators keep them apart and that the
// satellite-prefix extractor is robust on either form.
TEST(StateKeySchemaTest, SatPhaseBiasNamespaceDisjointFromCodeBias) {
    const std::string code  = makeSatBiasKey("G01", "L1C");        // "G01|L1C"
    const std::string phase = makeSatPhaseBiasKey("G01", "L1C");   // "G01|L1C|PB"
    EXPECT_NE(code, phase);

    // Discriminator: only the phase key is a phase-bias key.
    EXPECT_TRUE(isSatPhaseBiasKey(phase));
    EXPECT_FALSE(isSatPhaseBiasKey(code));

    // A code-bias key never parses as a phase-bias key.
    EXPECT_FALSE(parseSatPhaseBiasKey(code).has_value());

    // Satellite-prefix extraction is first-pipe robust on EITHER form.
    EXPECT_EQ(satelliteFromSatBiasKey(phase), "G01");
    EXPECT_EQ(satelliteFromSatPhaseBiasKey(phase), "G01");
    EXPECT_EQ(satelliteFromSatPhaseBiasKey(code), "G01");
}

// ============================================================================
// 5c. Per-satellite CLOCK keys — "<satellite>|SC" (PPP_RTK_Network)
// ============================================================================

TEST(StateKeySchemaTest, SatClockRoundTrip) {
    const std::string key = makeSatClockKey("G01");
    EXPECT_EQ(key, "G01|SC");

    auto sat = parseSatClockKey(key);
    ASSERT_TRUE(sat.has_value());
    EXPECT_EQ(*sat, "G01");

    EXPECT_TRUE(isSatClockKey(key));
    EXPECT_EQ(satelliteFromSatClockKey(key), "G01");
}

TEST(StateKeySchemaTest, SatClockParseRejectsMalformed) {
    EXPECT_FALSE(parseSatClockKey("").has_value());
    EXPECT_FALSE(parseSatClockKey("G01").has_value());        // No |SC suffix
    EXPECT_FALSE(parseSatClockKey("|SC").has_value());        // Empty satellite
    EXPECT_FALSE(parseSatClockKey("G01|L1C|SC").has_value()); // Sat clock is per-satellite, no signal field
}

// The sat-clock key (per-satellite, "<sat>|SC") must stay disjoint from the
// per-(sat,signal) bias keys.  "|SC" and "|PB" are distinct discriminators, and
// a clock key carries no signal field, so it never parses as a bias key.
TEST(StateKeySchemaTest, SatClockNamespaceDisjointFromBiasKeys) {
    const std::string clock = makeSatClockKey("G01");           // "G01|SC"
    const std::string code  = makeSatBiasKey("G01", "L1C");     // "G01|L1C"
    const std::string phase = makeSatPhaseBiasKey("G01", "L1C");// "G01|L1C|PB"

    EXPECT_TRUE(isSatClockKey(clock));
    EXPECT_FALSE(isSatClockKey(code));
    EXPECT_FALSE(isSatClockKey(phase));
    EXPECT_FALSE(isSatPhaseBiasKey(clock));

    // A clock key never parses as a phase-bias key, and vice versa.
    EXPECT_FALSE(parseSatPhaseBiasKey(clock).has_value());
    EXPECT_FALSE(parseSatClockKey(phase).has_value());

    EXPECT_EQ(satelliteFromSatClockKey(clock), "G01");
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
// 1b. NL / WL ambiguity keys — "<sat>_NL_<band>" / "<sat>_WL_<band_a>_<band_b>"
// ============================================================================

TEST(StateKeySchemaTest, NlAmbiguityRoundTrip) {
    const std::string key = makeNlAmbiguityKey("G01", '1');
    EXPECT_EQ(key, "G01_NL_1");

    EXPECT_TRUE(isNlAmbiguityKey(key));
    auto parts = parseNlAmbiguityKey(key);
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->satellite, "G01");
    EXPECT_EQ(parts->band, '1');
}

TEST(StateKeySchemaTest, NlAmbiguityRejectsMalformed) {
    EXPECT_FALSE(isNlAmbiguityKey(""));
    EXPECT_FALSE(isNlAmbiguityKey("G01"));
    EXPECT_FALSE(isNlAmbiguityKey("G01_NL"));      // No band
    EXPECT_FALSE(isNlAmbiguityKey("G01_NL_"));     // Empty band
    EXPECT_FALSE(isNlAmbiguityKey("G01_NL_1A"));   // Trailing junk past band digit
    EXPECT_FALSE(isNlAmbiguityKey("G01_NX_1"));    // Wrong infix
    EXPECT_FALSE(isNlAmbiguityKey("_NL_1"));       // Empty satellite
    EXPECT_FALSE(parseNlAmbiguityKey("G01_L1C").has_value());  // Per-band key
}

TEST(StateKeySchemaTest, WlAmbiguityRoundTripCanonicalOrder) {
    // Caller passes bands in any order; the maker normalizes to ascending.
    const std::string key = makeWlAmbiguityKey("G01", '1', '2');
    const std::string key_swapped = makeWlAmbiguityKey("G01", '2', '1');
    EXPECT_EQ(key, "G01_WL_1_2");
    EXPECT_EQ(key, key_swapped);

    EXPECT_TRUE(isWlAmbiguityKey(key));
    auto parts = parseWlAmbiguityKey(key);
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->satellite, "G01");
    EXPECT_EQ(parts->band_a, '1');
    EXPECT_EQ(parts->band_b, '2');
}

TEST(StateKeySchemaTest, WlAmbiguityHandlesMultiCharSatId) {
    // Galileo IDs are 3 chars; the parser must not assume satellite-length 3.
    const std::string key = makeWlAmbiguityKey("E11", '1', '7');
    EXPECT_EQ(key, "E11_WL_1_7");

    auto parts = parseWlAmbiguityKey(key);
    ASSERT_TRUE(parts.has_value());
    EXPECT_EQ(parts->satellite, "E11");
    EXPECT_EQ(parts->band_a, '1');
    EXPECT_EQ(parts->band_b, '7');
}

TEST(StateKeySchemaTest, WlAmbiguityRejectsMalformed) {
    EXPECT_FALSE(isWlAmbiguityKey(""));
    EXPECT_FALSE(isWlAmbiguityKey("G01_WL_1"));      // Missing second band
    EXPECT_FALSE(isWlAmbiguityKey("G01_WL_1_"));     // Empty second band
    EXPECT_FALSE(isWlAmbiguityKey("G01_WL__2"));     // Empty first band
    EXPECT_FALSE(isWlAmbiguityKey("G01_WL_1_2_3")); // Three bands
    EXPECT_FALSE(isWlAmbiguityKey("G01_WX_1_2"));   // Wrong infix
    EXPECT_FALSE(parseWlAmbiguityKey("G01_NL_1").has_value()); // NL key
}

// NL/WL parsers must not mis-classify per-band keys, and the per-band parser
// remains a happy round-trip for legacy usage.  Tests guard against future
// schema collisions when both bases coexist transiently in tooling.
TEST(StateKeySchemaTest, NlWlVsPerBandDistinguishable) {
    const std::string nl = makeNlAmbiguityKey("G01", '1');
    const std::string wl = makeWlAmbiguityKey("G01", '1', '2');
    const std::string per_band = makeAmbiguityKey("G01", "L1C");

    EXPECT_TRUE(isNlAmbiguityKey(nl));
    EXPECT_FALSE(isNlAmbiguityKey(wl));
    EXPECT_FALSE(isNlAmbiguityKey(per_band));

    EXPECT_TRUE(isWlAmbiguityKey(wl));
    EXPECT_FALSE(isWlAmbiguityKey(nl));
    EXPECT_FALSE(isWlAmbiguityKey(per_band));

    // A signal label that happens to contain "NL" (none in real RINEX) would
    // not collide because per-band signals start with 'L' or 'C', not "NL_".
    // The per-band parser still accepts the NL/WL keys (it splits on the
    // FIRST '_'), interpreting them as {sat="G01", signal="NL_1"} etc.  This
    // is documented behavior — callers are expected to consult the basis
    // before parsing, not rely on the per-band parser to reject.
    auto pb_from_nl = parseAmbiguityKey(nl);
    ASSERT_TRUE(pb_from_nl.has_value());
    EXPECT_EQ(pb_from_nl->signal, "NL_1");
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

TEST(StateKeySchema, VtecShKeyRoundTrip) {
    using namespace StateKeys;
    EXPECT_EQ(makeVtecShKey(0, 0, 'C'), "SH|0|0|C");
    EXPECT_EQ(makeVtecShKey(4, 2, 'S'), "SH|4|2|S");
    EXPECT_EQ(makeVtecShKey(12, 11, 'C'), "SH|12|11|C");

    for (char trig : {'C', 'S'}) {
        for (int n = 0; n <= 12; ++n) {
            for (int m = 0; m <= n; ++m) {
                const std::string k = makeVtecShKey(n, m, trig);
                EXPECT_TRUE(isVtecShKey(k));
                auto p = parseVtecShKey(k);
                ASSERT_TRUE(p.has_value()) << k;
                EXPECT_EQ(p->n, n);
                EXPECT_EQ(p->m, m);
                EXPECT_EQ(p->trig, trig);
            }
        }
    }

    // Malformed / cross-namespace inputs are rejected.
    EXPECT_FALSE(parseVtecShKey("SH|1|2").has_value());        // too few fields
    EXPECT_FALSE(parseVtecShKey("SH|1|2|X").has_value());      // bad trig tag
    EXPECT_FALSE(parseVtecShKey("SH|1|2|C|extra").has_value());// extra field
    EXPECT_FALSE(parseVtecShKey("SH|a|2|C").has_value());      // non-numeric degree
    EXPECT_FALSE(parseVtecShKey("G01|SC").has_value());        // not an SH key
    EXPECT_FALSE(isVtecShKey("G01|CB|C1W"));
    EXPECT_FALSE(isVtecShKey("SH"));
}

// Iono-2 dual-layer: the second shell keys under the DISTINCT "SH2" prefix.
// Layer 0 output stays byte-identical to the pre-Iono-2 three-argument form
// (the backward-compat contract for snapshots/traces), the two layers never
// collide, and both satisfy the layer-agnostic isVtecShKey predicate.
TEST(StateKeySchema, VtecShLayerKeyRoundTrip) {
    using namespace StateKeys;
    // Layer 0 is the legacy format, unchanged.
    EXPECT_EQ(makeVtecShKey(4, 2, 'S', 0), "SH|4|2|S");
    EXPECT_EQ(makeVtecShKey(4, 2, 'S', 0), makeVtecShKey(4, 2, 'S'));
    // Layer 1 gets the distinct prefix.
    EXPECT_EQ(makeVtecShKey(0, 0, 'C', 1), "SH2|0|0|C");
    EXPECT_NE(makeVtecShKey(3, 1, 'C', 1), makeVtecShKey(3, 1, 'C', 0));

    for (char trig : {'C', 'S'}) {
        for (int n = 0; n <= 6; ++n) {
            for (int m = 0; m <= n; ++m) {
                for (int layer : {0, 1}) {
                    const std::string k = makeVtecShKey(n, m, trig, layer);
                    EXPECT_TRUE(isVtecShKey(k)) << k;
                    auto p = parseVtecShKey(k);
                    ASSERT_TRUE(p.has_value()) << k;
                    EXPECT_EQ(p->n, n);
                    EXPECT_EQ(p->m, m);
                    EXPECT_EQ(p->trig, trig);
                    EXPECT_EQ(p->layer, layer);
                }
            }
        }
    }

    // A parsed legacy key reports layer 0 (old snapshots stay valid).
    auto legacy = parseVtecShKey("SH|8|3|C");
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ(legacy->layer, 0);
    // Malformed layer-2 inputs rejected on the same rules as layer 0.
    EXPECT_FALSE(parseVtecShKey("SH2|1|2").has_value());
    EXPECT_FALSE(parseVtecShKey("SH2|1|2|X").has_value());
    EXPECT_FALSE(parseVtecShKey("SH2|1|2|C|extra").has_value());
    EXPECT_FALSE(isVtecShKey("SH2"));
}

}  // namespace
}  // namespace GnssProcessor

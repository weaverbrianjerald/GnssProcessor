/**
 * @file test_gnss_identity.cpp
 * @brief Card NOM step N0a: the identity types of common/GnssIdentity.hpp.
 *
 * The types replace string keys whose order is numerically load-bearing (the network's
 * rank-1 inject order and fold order iterate string-keyed maps), so the first contract
 * tested is that every comparator reproduces the @c std::string order of the key it will
 * replace, over every satellite of the RINEX constellations and every signal the
 * classifier knows.  Then the parse and render round trips (the blank-padded satellite
 * forms of card RXP included), the letter, name and key maps against the existing
 * helpers, and the one frequency function against @c PppUtils::getFrequency.
 *
 * The frequency comparison records, and does not fix, the places the two sources
 * disagree; step N0b decides them.  The expected list is pinned here so that a change on
 * either side is noticed.
 *
 * Step N0b's call-site helpers are tested last, each against the expression or helper it
 * replaces at the network call sites: the satellite-letter reads, the renderings of a
 * @c System, the observation-type test, and the wavelength and TECU expressions, the last
 * two bit for bit (the migration claims byte identity).
 */

#include <gtest/gtest.h>

#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"
#include "common/PppUtils.hpp"
#include "common/SignalBand.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace GnssProcessor;

namespace {

/// The six constellations with a RINEX letter and a @c System value.
const std::vector<System> kSystems = {System::GPS, System::Galileo, System::GLONASS,
                                      System::BeiDou, System::QZSS, System::SBAS};

/// Every (digit, attribute) the classifier maps to a known band for @p sys.
std::vector<SignalId> classifiedSignals(System sys) {
    std::vector<SignalId> out;
    for (char d = '0'; d <= '9'; ++d)
        for (char a = 'A'; a <= 'Z'; ++a) {
            const std::string code{'C', d, a};
            if (classifySignal(traitsOf(sys).name, code).band != FrequencyBand::Unknown)
                out.push_back(SignalId{sys, d, a});
        }
    return out;
}

/// A deterministic shuffle, so that a sort that does nothing cannot pass.
template <class T>
void shuffle(std::vector<T>& v) {
    std::mt19937 rng(20261008u);
    std::shuffle(v.begin(), v.end(), rng);
}

} // namespace

// ---------------------------------------------------------------------------
// Ordering: every comparator reproduces the string order it replaces
// ---------------------------------------------------------------------------

TEST(GnssIdentityTest, SatIdOrderMatchesStringOrder) {
    std::vector<SatId> sats;
    for (System s : kSystems)
        for (int prn = 1; prn <= 99; ++prn) sats.push_back(SatId{s, prn});
    std::vector<std::string> strs;
    for (const SatId& s : sats) strs.push_back(s.str());
    shuffle(sats);
    std::sort(sats.begin(), sats.end());
    std::sort(strs.begin(), strs.end());
    ASSERT_EQ(sats.size(), strs.size());
    for (std::size_t i = 0; i < sats.size(); ++i) EXPECT_EQ(sats[i].str(), strs[i]) << i;
    // The letter orders it, not the enum: BeiDou ('C') before GLONASS ('R').
    EXPECT_LT((SatId{System::BeiDou, 1}), (SatId{System::GLONASS, 1}));
    EXPECT_LT((SatId{System::GPS, 9}), (SatId{System::GPS, 10}));
}

TEST(GnssIdentityTest, SignalIdOrderMatchesStringOrder) {
    std::vector<SignalId> sigs;
    std::vector<std::string> strs;
    for (System s : kSystems)
        for (const SignalId& g : classifiedSignals(s)) {
            sigs.push_back(g);
            strs.push_back(std::string(1, traitsOf(s).letter) + g.str());
        }
    ASSERT_GT(sigs.size(), 100u);
    shuffle(sigs);
    std::sort(sigs.begin(), sigs.end());
    std::sort(strs.begin(), strs.end());
    for (std::size_t i = 0; i < sigs.size(); ++i)
        EXPECT_EQ(std::string(1, traitsOf(sigs[i].sys).letter) + sigs[i].str(), strs[i]) << i;

    // Within one constellation: the order of the bare phase code ("L1C" < "L1W" < "L2W").
    for (System s : kSystems) {
        std::vector<SignalId> one = classifiedSignals(s);
        std::vector<std::string> codes;
        for (const SignalId& g : one) codes.push_back(g.obsCode(ObsType::Phase));
        shuffle(one);
        std::sort(one.begin(), one.end());
        std::sort(codes.begin(), codes.end());
        for (std::size_t i = 0; i < one.size(); ++i)
            EXPECT_EQ(one[i].obsCode(ObsType::Phase), codes[i]) << traitsOf(s).name;
    }
}

TEST(GnssIdentityTest, ObservableIdOrderMatchesStringOrder) {
    for (System s : kSystems) {
        std::vector<ObservableId> obs;
        std::vector<std::string> codes;
        for (const SignalId& g : classifiedSignals(s))
            for (ObsType t : {ObsType::Code, ObsType::Phase, ObsType::Doppler, ObsType::Snr}) {
                obs.push_back(ObservableId{g, t});
                codes.push_back(g.obsCode(t));
            }
        shuffle(obs);
        std::sort(obs.begin(), obs.end());
        std::sort(codes.begin(), codes.end());
        for (std::size_t i = 0; i < obs.size(); ++i)
            EXPECT_EQ(obs[i].str(), codes[i]) << traitsOf(s).name;
    }
}

TEST(GnssIdentityTest, LosAndArcOrderMatchTheTupleKeysTheyReplace) {
    // LosId replaces std::pair<int, std::string>; ArcId the (int, sat, signal) tuple.
    std::vector<LosId> los;
    std::vector<std::pair<int, std::string>> los_keys;
    std::vector<ArcId> arcs;
    std::vector<std::tuple<int, std::string, std::string>> arc_keys;
    const std::vector<SignalId> gps = classifiedSignals(System::GPS);
    for (int sta = 0; sta < 3; ++sta)
        for (System s : {System::GPS, System::Galileo, System::BeiDou})
            for (int prn : {1, 5, 10, 33}) {
                const SatId sat{s, prn};
                los.push_back(LosId{StationId{sta}, sat});
                los_keys.emplace_back(sta, sat.str());
                if (s != System::GPS) continue;
                for (std::size_t k = 0; k < gps.size(); k += 7) {
                    arcs.push_back(ArcId{StationId{sta}, sat, gps[k]});
                    arc_keys.emplace_back(sta, sat.str(), gps[k].obsCode(ObsType::Phase));
                }
            }
    shuffle(los);
    shuffle(arcs);
    std::sort(los.begin(), los.end());
    std::sort(los_keys.begin(), los_keys.end());
    std::sort(arcs.begin(), arcs.end());
    std::sort(arc_keys.begin(), arc_keys.end());
    for (std::size_t i = 0; i < los.size(); ++i) {
        EXPECT_EQ(los[i].station.index, los_keys[i].first);
        EXPECT_EQ(los[i].sat.str(), los_keys[i].second);
    }
    for (std::size_t i = 0; i < arcs.size(); ++i) {
        EXPECT_EQ(arcs[i].station.index, std::get<0>(arc_keys[i]));
        EXPECT_EQ(arcs[i].sat.str(), std::get<1>(arc_keys[i]));
        EXPECT_EQ(arcs[i].signal.obsCode(ObsType::Phase), std::get<2>(arc_keys[i]));
    }
}

TEST(GnssIdentityTest, SatSignalIdOrderMatchesTheCompositeKeysItReplaces) {
    // SatSignalId (card NOM step N4.0) replaces the "sat|code" and "sat_code" strings and
    // the (sat, code) pairs; every key of one container carries codes of one type.
    std::vector<SatSignalId> ids;
    std::vector<std::pair<std::string, std::string>> pairs;
    std::vector<std::string> bars, unders;
    for (System s : {System::GPS, System::Galileo, System::BeiDou, System::GLONASS})
        for (int prn : {1, 5, 10, 33})
            for (const SignalId& sig : classifiedSignals(s)) {
                const SatId sat{s, prn};
                const std::string code = sig.obsCode(ObsType::Phase);
                ids.push_back(SatSignalId{sat, sig});
                pairs.emplace_back(sat.str(), code);
                bars.push_back(sat.str() + "|" + code);
                unders.push_back(sat.str() + "_" + code);
            }
    shuffle(ids);
    std::sort(ids.begin(), ids.end());
    std::sort(pairs.begin(), pairs.end());
    std::sort(bars.begin(), bars.end());
    std::sort(unders.begin(), unders.end());
    ASSERT_EQ(ids.size(), pairs.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const std::string sat = ids[i].sat.str();
        const std::string code = ids[i].signal.obsCode(ObsType::Phase);
        EXPECT_EQ(sat, pairs[i].first);
        EXPECT_EQ(code, pairs[i].second);
        EXPECT_EQ(sat + "|" + code, bars[i]);
        EXPECT_EQ(sat + "_" + code, unders[i]);
    }
    EXPECT_EQ(SatSignalId{ids.front()}, ids.front());
    EXPECT_NE(ids.front(), ids.back());
}

// ---------------------------------------------------------------------------
// Parse and render
// ---------------------------------------------------------------------------

TEST(GnssIdentityTest, SatIdParseRoundTripsAndNormalizesPaddedForms) {
    for (System s : kSystems)
        for (int prn = 1; prn <= 99; ++prn) {
            const SatId sat{s, prn};
            const auto back = SatId::parse(sat.str());
            ASSERT_TRUE(back.has_value()) << sat.str();
            EXPECT_EQ(*back, sat);
            EXPECT_TRUE(back->valid());
        }
    // The three spellings of card RXP.
    for (const char* t : {"G05", "G 5", "G5"}) {
        const auto p = SatId::parse(t);
        ASSERT_TRUE(p.has_value()) << t;
        EXPECT_EQ(p->str(), "G05") << t;
    }
    EXPECT_EQ(SatId::parse("E 1")->str(), "E01");
    EXPECT_EQ(SatId::parse("R24")->str(), "R24");
    // Everything else is refused.
    for (const char* t : {"", "G", "G005", "g05", "I05", "X05", "G00", "G0", "G 0", "GA5",
                          "G5 ", " G5", "G 05", "G-1", "05"})
        EXPECT_FALSE(SatId::parse(t).has_value()) << "'" << t << "'";
}

TEST(GnssIdentityTest, SignalParseAndRender) {
    const auto s = SignalId::fromObsCode(System::GPS, "L1C");
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->str(), "1C");
    EXPECT_EQ(s->obsCode(ObsType::Code), "C1C");
    EXPECT_EQ(s->obsCode(ObsType::Phase), "L1C");
    EXPECT_EQ(s->band(), FrequencyBand::GpsL1);
    EXPECT_EQ(*SignalId::fromObsCode(System::GPS, "C1C"), *s);   // the type is not part of the signal
    for (const char* t : {"", "L1", "L1CX", "X1C", "LAC", "L1c", "L11"})
        EXPECT_FALSE(SignalId::fromObsCode(System::GPS, t).has_value()) << t;
    EXPECT_FALSE(SignalId::fromObsCode(System::Unknown, "L1C").has_value());

    const auto o = ObservableId::parse(System::Galileo, "L5Q");
    ASSERT_TRUE(o.has_value());
    EXPECT_EQ(o->type, ObsType::Phase);
    EXPECT_EQ(o->str(), "L5Q");
    EXPECT_EQ(o->signal.band(), FrequencyBand::GalE5a);

    // band() is classifySignal, for every classified signal.
    for (System sys : kSystems)
        for (const SignalId& g : classifiedSignals(sys))
            EXPECT_EQ(g.band(),
                      classifySignal(traitsOf(sys).name, g.obsCode(ObsType::Code)).band);
}

TEST(GnssIdentityTest, SignalPairParseAndRender) {
    for (const char* t : {"C1C_C2W", "L1C_L5Q", "C1W_C2W"}) {
        const auto p = SignalPair::parse(System::GPS, t);
        ASSERT_TRUE(p.has_value()) << t;
        EXPECT_EQ(p->str(static_cast<ObsType>(t[0])), t);
    }
    const auto p = SignalPair::parse(System::GPS, "C1C_C2W");
    EXPECT_EQ(p->a, *SignalId::fromObsCode(System::GPS, "C1C"));
    EXPECT_EQ(p->b, *SignalId::fromObsCode(System::GPS, "C2W"));
    for (const char* t : {"", "C1C", "C1CC2W", "C1C_L2W", "C1C_C2", "C1C-C2W", "C1C_C2WX"})
        EXPECT_FALSE(SignalPair::parse(System::GPS, t).has_value()) << t;
    // Order (a, b).
    EXPECT_LT(*SignalPair::parse(System::GPS, "C1C_C2W"), *SignalPair::parse(System::GPS, "C1W_C2W"));
    EXPECT_LT(*SignalPair::parse(System::GPS, "C1C_C2L"), *SignalPair::parse(System::GPS, "C1C_C2W"));
}

// ---------------------------------------------------------------------------
// Letter, name and key maps against the existing helpers
// ---------------------------------------------------------------------------

TEST(GnssIdentityTest, TraitsAgreeWithTheExistingHelpers) {
    for (System s : kSystems) {
        const ConstellationTraits& t = traitsOf(s);
        EXPECT_EQ(t.sys, s);
        EXPECT_EQ(std::string(t.name), PppUtils::getSystemString(s));
        EXPECT_EQ(PppUtils::getSystemString(std::string(1, t.letter) + "01"), t.name);
        // The name doubles as the system-time-offset state key.
        EXPECT_EQ(std::string(t.name), StateKeys::makeSystemTimeOffsetKey(t.name));
        EXPECT_EQ(systemFromLetter(t.letter), s);
        EXPECT_EQ(systemFromName(t.name), s);
        EXPECT_EQ(systemFromName(std::string(1, t.letter)), s);
        EXPECT_EQ(t.fdma, s == System::GLONASS);
        // The satellite-id wrapper of the classifier reads the same letter.
        EXPECT_EQ(classifySignalForSat(std::string(1, t.letter) + "01", "C1C").band,
                  classifySignal(t.name, "C1C").band);

        // The band list is exactly the set of bands the classifier returns for the
        // constellation, each once, in non-decreasing digit order.
        std::set<FrequencyBand> listed;
        for (std::size_t i = 0; i < t.n_bands; ++i) {
            EXPECT_TRUE(listed.insert(t.bands[i].band).second) << t.name;
            if (i > 0) EXPECT_LE(t.bands[i - 1].digit, t.bands[i].digit) << t.name;
            // The digit names the band for at least one attribute.
            bool named = false;
            for (char a = 'A'; a <= 'Z' && !named; ++a)
                named = classifySignal(t.name, std::string{'C', t.bands[i].digit, a}).band
                        == t.bands[i].band;
            EXPECT_TRUE(named) << t.name << " " << bandLabel(t.bands[i].band);
        }
        std::set<FrequencyBand> classified;
        for (const SignalId& g : classifiedSignals(s)) classified.insert(g.band());
        EXPECT_EQ(listed, classified) << t.name;
    }
    // The documented letter policy: unknown letters are Unknown, never GPS.
    EXPECT_EQ(systemFromLetter('I'), System::Unknown);
    EXPECT_EQ(systemFromLetter('g'), System::Unknown);
    EXPECT_EQ(systemFromLetter(' '), System::Unknown);
    EXPECT_EQ(systemFromName("IRNSS"), System::Unknown);
    EXPECT_EQ(systemFromName("gps"), System::Unknown);
    EXPECT_EQ(systemFromName(""), System::Unknown);
    EXPECT_EQ(traitsOf(System::Unknown).letter, '?');
    EXPECT_EQ(traitsOf(System::Unknown).n_bands, 0u);
}

TEST(GnssIdentityTest, TraitsAreCompileTimeConstants) {
    // The table is an inline constexpr table in the header: the letter a SatId comparison
    // reads is a constant expression, so no comparison calls across translation units.
    static_assert(traitsOf(System::GPS).letter == 'G', "GPS letter");
    static_assert(traitsOf(System::BeiDou).letter == 'C', "BeiDou letter");
    static_assert(traitsOf(System::GLONASS).fdma, "GLONASS is FDMA");
    static_assert(SatId{System::Galileo, 5}.letter() == 'E', "SatId letter");
    static_assert(bandOfDigit(System::GPS, '5') == FrequencyBand::GpsL5, "GPS digit 5");
    SUCCEED();
}

TEST(GnssIdentityTest, BandOfDigitReadsTheTraitsTable) {
    for (System s : kSystems) {
        const ConstellationTraits& t = traitsOf(s);
        for (std::size_t i = 0; i < t.n_bands; ++i) {
            const FrequencyBand b = bandOfDigit(s, t.bands[i].digit);
            // The first band listed under the digit: BeiDou digit 7 is B2I and B2b (one
            // carrier, 1207.14 MHz), and reads as B2I.
            if (s == System::BeiDou && t.bands[i].band == FrequencyBand::BdsB2b)
                EXPECT_EQ(b, FrequencyBand::BdsB2I);
            else
                EXPECT_EQ(b, t.bands[i].band) << t.name << " " << t.bands[i].digit;
            EXPECT_EQ(nominalCarrierFrequencyHz(b), nominalCarrierFrequencyHz(t.bands[i].band));
        }
    }
    EXPECT_EQ(bandOfDigit(System::GPS, '7'), FrequencyBand::Unknown);
    EXPECT_EQ(bandOfDigit(System::Galileo, '2'), FrequencyBand::Unknown);
    EXPECT_EQ(bandOfDigit(System::Unknown, '1'), FrequencyBand::Unknown);
    // A digit alone reads BeiDou '1' as B1C; the RINEX 3.02 spelling of B1I (C1I, C1Q)
    // needs the attribute, which SignalId::band reads.
    EXPECT_EQ(bandOfDigit(System::BeiDou, '1'), FrequencyBand::BdsB1C);
    EXPECT_EQ(SignalId::fromObsCode(System::BeiDou, "C1I")->band(), FrequencyBand::BdsB1I);
}

// ---------------------------------------------------------------------------
// The frequency function against PppUtils::getFrequency
// ---------------------------------------------------------------------------

TEST(GnssIdentityTest, FrequencyAgreesWithPppUtilsExceptTheRecordedCases) {
    LogManager log;
    PppUtils pu(log);
    // A satellite per constellation; R01's default FDMA channel is +1.
    const std::map<System, std::string> sat = {
        {System::GPS, "G01"}, {System::Galileo, "E01"}, {System::GLONASS, "R01"},
        {System::BeiDou, "C01"}, {System::QZSS, "J01"}, {System::SBAS, "S20"}};
    ASSERT_EQ(pu.getGlonassChannel("R01"), 1);

    std::set<std::string> disagree;   // "<letter><code>: identity vs PppUtils"
    for (System s : kSystems)
        for (const SignalId& g : classifiedSignals(s)) {
            const std::string code = g.obsCode(ObsType::Code);
            const double mine = g.frequencyHz(s == System::GLONASS ? 1 : 0);   // R01: slot +1
            const double theirs = pu.getFrequency(code, s, sat.at(s));
            if (mine != theirs)
                disagree.insert(std::string(1, traitsOf(s).letter) + code);
        }
    // Recorded for step N0b, not fixed here: BeiDou digit 1 with an I or Q attribute
    // (the RINEX 3.02 spelling of B1I).  classifySignal and this function give B1I,
    // 1561.098 MHz; PppUtils::getFrequency matches the digit alone and gives B1C,
    // 1575.42 MHz (card NOM section 7 item 7).
    const std::set<std::string> expected = {"CC1I", "CC1Q"};
    EXPECT_EQ(disagree, expected);
    EXPECT_DOUBLE_EQ(nominalCarrierFrequencyHz(FrequencyBand::BdsB1I), 1561.098e6);
    EXPECT_DOUBLE_EQ(pu.getFrequency("C1I", System::BeiDou, "C01"), 1575.42e6);

    // Recorded for step N0b: a GLONASS FDMA satellite with no known slot.  PppUtils
    // refuses it (channel sentinel -999, frequency 0.0); carrierFrequencyHz takes the
    // slot as an argument and reads slot 0 as the central channel (1602 MHz).  With
    // the slot known the two agree, slot 0 included (R11's default channel is 0).
    EXPECT_EQ(pu.getFrequency("C1C", System::GLONASS, "R99"), 0.0);
    EXPECT_DOUBLE_EQ(carrierFrequencyHz(FrequencyBand::GloG1, 0), 1602.0e6);
    ASSERT_EQ(pu.getGlonassChannel("R11"), 0);
    EXPECT_EQ(pu.getFrequency("C1C", System::GLONASS, "R11"),
              carrierFrequencyHz(FrequencyBand::GloG1, 0));
    EXPECT_EQ(pu.getFrequency("C2P", System::GLONASS, "R02"),
              carrierFrequencyHz(FrequencyBand::GloG2, -4));

    // The nominal functions are the slot functions at slot 0: the value the
    // nominal-wavelength table of SignalBand documents for the FDMA bands.  A CDMA band
    // ignores the slot.
    for (int b = 0; b <= static_cast<int>(FrequencyBand::IrnS); ++b) {
        const auto band = static_cast<FrequencyBand>(b);
        EXPECT_DOUBLE_EQ(nominalWavelengthM(band), bandNominalWavelengthMeters(band)) << bandLabel(band);
        EXPECT_EQ(nominalCarrierFrequencyHz(band), carrierFrequencyHz(band, 0)) << bandLabel(band);
        EXPECT_EQ(nominalWavelengthM(band), wavelengthM(band, 0)) << bandLabel(band);
        if (band != FrequencyBand::GloG1 && band != FrequencyBand::GloG2)
            EXPECT_EQ(carrierFrequencyHz(band, 3), carrierFrequencyHz(band, 0)) << bandLabel(band);
    }
    EXPECT_DOUBLE_EQ(carrierFrequencyHz(FrequencyBand::GloG1, 3), 1602.0e6 + 3 * 0.5625e6);
    EXPECT_EQ(nominalCarrierFrequencyHz(FrequencyBand::Unknown), 0.0);
    EXPECT_EQ(nominalWavelengthM(FrequencyBand::Unknown), 0.0);
}

// ---------------------------------------------------------------------------
// Step N0b: the call-site helpers, each against what it replaces
// ---------------------------------------------------------------------------

TEST(GnssIdentityTest, SystemOfSatReadsTheFirstLetterOnly) {
    // Every satellite of the six constellations, in its canonical rendering.
    for (System s : kSystems)
        for (int prn = 1; prn <= 99; ++prn)
            EXPECT_EQ(systemOfSat(SatId{s, prn}.str()), s) << SatId{s, prn}.str();
    // Tolerant: the non-normalized spellings, and anything after the letter.
    for (const char* t : {"G 5", "G5", "G005", "G", "GXYZ"})
        EXPECT_EQ(systemOfSat(t), System::GPS) << "'" << t << "'";
    // The single letter policy: never GPS for an unknown letter or an empty id.
    for (const char* t : {"", " G05", "g05", "I05", "X01", "?01"})
        EXPECT_EQ(systemOfSat(t), System::Unknown) << "'" << t << "'";

    // The name it gives is the name PppUtils::getSystemString reads from the same
    // string, for every first character (the satellite overload the call sites use).
    EXPECT_EQ(std::string(systemName(systemOfSat(""))), PppUtils::getSystemString(std::string{}));
    for (int c = 1; c < 128; ++c) {
        const std::string sat = std::string(1, static_cast<char>(c)) + "01";
        EXPECT_EQ(std::string(systemName(systemOfSat(sat))), PppUtils::getSystemString(sat)) << c;
    }
}

TEST(GnssIdentityTest, LetterNameAndKeyRenderTheTraits) {
    static_assert(rinexLetter(System::BeiDou) == 'C', "BeiDou letter");
    static_assert(rinexLetter(System::Unknown) == '?', "Unknown letter");
    for (System s : kSystems) {
        EXPECT_EQ(rinexLetter(s), traitsOf(s).letter);
        EXPECT_EQ(std::string(systemName(s)), PppUtils::getSystemString(s));
        EXPECT_EQ(systemFromName(systemName(s)), s);
        // The key is the satellite id's first character, for every satellite.
        for (int prn = 1; prn <= 99; ++prn) {
            const std::string sat = SatId{s, prn}.str();
            EXPECT_EQ(constellationKey(systemOfSat(sat)), sat.substr(0, 1)) << sat;
        }
    }
    EXPECT_EQ(std::string(systemName(System::Unknown)), "Unknown");
    EXPECT_EQ(constellationKey(System::Unknown), "?");
    // Recorded: where the first character itself is not a known letter, the key is "?"
    // (the first character would have been "" or "I").
    EXPECT_EQ(constellationKey(systemOfSat("")), "?");
    EXPECT_EQ(constellationKey(systemOfSat("I05")), "?");
}

TEST(GnssIdentityTest, LetterOfSatIsTheFirstCharacterOfAKnownSatellite) {
    // The letter reads are compile-time constants (inline constexpr table reads).
    static_assert(systemFromLetter('E') == System::Galileo, "letter map");
    static_assert(systemFromLetter('I') == System::Unknown, "IRNSS has no System value");
    static_assert(systemOfSat("C19") == System::BeiDou, "satellite read");
    static_assert(systemOfSat("") == System::Unknown, "empty id");
    static_assert(letterOfSat("J02") == 'J', "satellite letter");
    static_assert(letterOfSat("") == '?', "empty id");
    // The first character of every satellite of the six constellations.
    for (System s : kSystems)
        for (int prn = 1; prn <= 99; ++prn) {
            const std::string sat = SatId{s, prn}.str();
            EXPECT_EQ(letterOfSat(sat), sat.front()) << sat;
            EXPECT_EQ(letterOfSat(sat), rinexLetter(systemOfSat(sat))) << sat;
        }
    // An empty id and every letter outside the table share the '?' group, which is
    // where letterOfSat differs from the first character it replaced.
    for (const char* t : {"", "I05", "X01", "g05", " G05"})
        EXPECT_EQ(letterOfSat(t), '?') << "'" << t << "'";
}

TEST(GnssIdentityTest, ObsTypeOfReadsTheFirstCharacter) {
    static_assert(obsTypeOf("C1C") == ObsType::Code, "code");
    static_assert(obsTypeOf("L2W") == ObsType::Phase, "phase");
    // The type of every classified observable, as the validating parser reads it.
    for (System s : kSystems)
        for (const SignalId& g : classifiedSignals(s))
            for (ObsType t : {ObsType::Code, ObsType::Phase, ObsType::Doppler, ObsType::Snr}) {
                const std::string code = g.obsCode(t);
                EXPECT_EQ(obsTypeOf(code), ObservableId::parse(s, code)->type) << code;
            }
    // The tests it replaces read the first character alone: a pair text included.
    EXPECT_EQ(obsTypeOf("C1C_C2W"), ObsType::Code);
    EXPECT_EQ(obsTypeOf("L1C_L2W"), ObsType::Phase);
    for (const char* t : {"", "X1C", "c1C", "1C"})
        EXPECT_EQ(obsTypeOf(t), ObsType::Unknown) << "'" << t << "'";
    for (int c = 1; c < 128; ++c) {
        const std::string code = std::string(1, static_cast<char>(c)) + "1C";
        EXPECT_EQ(obsTypeOf(code) == ObsType::Code, code[0] == 'C') << c;
        EXPECT_EQ(obsTypeOf(code) == ObsType::Phase, code[0] == 'L') << c;
    }
}

TEST(GnssIdentityTest, WavelengthsAgreeBitForBitWithTheFunctionsTheyReplace) {
    LogManager log;
    PppUtils pu(log);
    const std::map<System, std::string> sat = {
        {System::GPS, "G01"}, {System::Galileo, "E01"}, {System::GLONASS, "R01"},
        {System::BeiDou, "C01"}, {System::QZSS, "J01"}, {System::SBAS, "S20"}};
    ASSERT_EQ(pu.getGlonassChannel("R01"), 1);

    // SignalId::wavelengthM against PppUtils::getWavelength, exactly (EXPECT_EQ, not a
    // tolerance): a call site that swaps one for the other keeps its bytes, except for
    // the BeiDou digit-1 I and Q signals N0a records (their frequencies differ).
    std::set<std::string> disagree;
    for (System s : kSystems)
        for (const SignalId& g : classifiedSignals(s)) {
            const int slot = (s == System::GLONASS) ? 1 : 0;   // R01: slot +1
            EXPECT_EQ(g.wavelengthM(slot), wavelengthM(g.band(), slot));
            for (ObsType t : {ObsType::Code, ObsType::Phase}) {
                const std::string code = g.obsCode(t);
                if (g.wavelengthM(slot) != pu.getWavelength(s, code, sat.at(s)))
                    disagree.insert(std::string(1, rinexLetter(s)) + code);
            }
        }
    const std::set<std::string> expected = {"CC1I", "CC1Q", "CL1I", "CL1Q"};
    EXPECT_EQ(disagree, expected);

    // nominalWavelengthM against SignalBand's table, exactly, for every band: the table's
    // constexpr quotients and this function's run-time quotient are the same division.
    for (int b = 0; b <= static_cast<int>(FrequencyBand::IrnS); ++b) {
        const auto band = static_cast<FrequencyBand>(b);
        EXPECT_EQ(nominalWavelengthM(band), bandNominalWavelengthMeters(band)) << bandLabel(band);
    }
}

TEST(GnssIdentityTest, IonoDelayPerTecuReproducesTheSiteExpression) {
    // The expression the network code writes at each of its sites, bit for bit.
    const double site = 40.3 * 1.0e16 / (Constants::GPS_L1_FREQ * Constants::GPS_L1_FREQ);
    EXPECT_EQ(nominalCarrierFrequencyHz(FrequencyBand::GpsL1), Constants::GPS_L1_FREQ);
    EXPECT_EQ(ionoDelayMetresPerTecu(nominalCarrierFrequencyHz(FrequencyBand::GpsL1)), site);
    EXPECT_NEAR(site, 0.16237, 1.0e-5);
    // The first-order delay scales as 1 / f^2.
    const double l2 = ionoDelayMetresPerTecu(nominalCarrierFrequencyHz(FrequencyBand::GpsL2));
    const double ratio = Constants::GPS_L1_FREQ / Constants::GPS_L2_FREQ;
    EXPECT_NEAR(l2 / site, ratio * ratio, 1.0e-12);
}

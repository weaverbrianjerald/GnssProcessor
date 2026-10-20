/**
 * @file test_param_ref.cpp
 * @brief The identities of common/ParamRef.hpp and the order of the state keys of the
 *        station-local families.
 *
 * The network iterates a station's key maps in the @c std::string order of their keys (the
 * rank-1 update order and the fold order follow it), so a map re-keyed by a typed identity
 * must iterate in the same order.  The satellite and signal identities of GnssIdentity.hpp
 * order by RINEX letter, which is the string order of a satellite id; a key that opens with
 * the constellation NAME (the inter-system bias, the receiver phase bias, the receiver
 * code-bias keys) does not follow the letters ("GPS_..." < "Galileo_...", the letters put
 * 'E' before 'G').  Each family is tested against the keys the state-key schema builds, over
 * the signals of both golden configurations and over every signal the classifier knows:
 *  - the inter-system bias: @c System with @c SystemNameOrder;
 *  - the receiver phase bias: @c SignalId with @c SignalNameOrder;
 *  - the receiver code bias: @c RxCbKey, with @c receiverCodeBiasKey as the witness that the typed
 *    key means what the state key says;
 *  - the satellite-signal and satellite families: @c SatSignalId and @c SatId against the
 *    real key makers (the suffixes "|PB" and "|SC" included).
 * Then the facts that make the explicit comparators necessary are pinned.
 */

#include <gtest/gtest.h>

#include "common/GnssIdentity.hpp"
#include "common/ParamRef.hpp"
#include "common/SignalBand.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "network/state/StationParams.hpp"       // receiverCodeBiasKey
#include "measurement/UserParamView.hpp"          // userKey

#include <algorithm>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace GnssProcessor;
namespace K  = GnssProcessor::StateKeys;
namespace fs = std::filesystem;

namespace {

/// The six constellations with a RINEX letter and a @c System value.
const std::vector<System> kSystems = {System::GPS, System::Galileo, System::GLONASS,
                                      System::BeiDou, System::QZSS, System::SBAS};

/// The signals of a constellation, as code and as phase (the same tracking signals).
using SignalUniverse = std::map<System, std::vector<SignalId>>;

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

/// Every classified signal of the six constellations.
SignalUniverse classifiedUniverse() {
    SignalUniverse u;
    for (System s : kSystems) u[s] = classifiedSignals(s);
    return u;
}

/// The signals of the golden configurations: the union of their signal priority lists.
SignalUniverse goldenUniverse() {
    SignalUniverse u;
    for (const char* file : {"golden_field_refound.json", "golden_sim_refound.json"}) {
        LogManager log;
        GnssConfig cfg;
        const fs::path path = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / file;
        EXPECT_TRUE(cfg.loadFromJson(path, log)) << path;
        for (const auto& [name, codes] : cfg.getSignalPriorities()) {
            const System sys = systemFromName(name);
            for (const std::string& code : codes)
                if (const auto sig = SignalId::fromObsCode(sys, code)) {
                    std::vector<SignalId>& v = u[sys];
                    if (std::find(v.begin(), v.end(), *sig) == v.end()) v.push_back(*sig);
                }
        }
    }
    return u;
}

/// A deterministic shuffle, so that an order that does nothing cannot pass.
template <class T>
void shuffle(std::vector<T>& v) {
    std::mt19937 rng(20261016u);
    std::shuffle(v.begin(), v.end(), rng);
}

/// The RINEX digit of a band, found by inverting @c bandOfDigit digit by digit (independent
/// of @c digitOfBand); '\0' when no digit reads as the band.
char digitFor(System sys, FrequencyBand band) {
    for (char d = '0'; d <= '9'; ++d)
        if (bandOfDigit(sys, d) == band) return d;
    return '\0';
}

/// The distinct bands of a constellation that a digit reads back as (BeiDou B2b is read as
/// B2I by its digit, so only B2I is a band a state key can name).
std::vector<FrequencyBand> keyBands(System sys) {
    std::vector<FrequencyBand> out;
    const ConstellationTraits& t = traitsOf(sys);
    for (std::size_t i = 0; i < t.n_bands; ++i) {
        const FrequencyBand b = t.bands[i].band;
        if (bandOfDigit(sys, digitFor(sys, b)) != b) continue;
        if (std::find(out.begin(), out.end(), b) == out.end()) out.push_back(b);
    }
    return out;
}

/// The state key of a receiver code-bias key, built with the schema's own makers.
std::string renderRxCb(const RxCbKey& k) {
    const std::string name = systemName(k.sys);
    switch (k.form) {
        case ParamForm::Dcb:        return K::makeDcbKey(name);
        case ParamForm::Ifb:        return K::makeIfbKey(name, digitFor(k.sys, k.band));
        case ParamForm::GloSlotIfb: return K::makeGlonassPerSlotIfbKey(name, digitFor(k.sys, k.band),
                                                                       k.glo_slot);
        case ParamForm::Isc:        return K::makeReceiverIscKey(name, k.signal.obsCode(ObsType::Code));
        default:                    return {};
    }
}

/// Every receiver code-bias key a station of the given constellations can hold: the
/// differential bias, an inter-frequency bias per band, a per-slot bias per FDMA band and
/// slot, and an inter-signal correction per code signal.
std::vector<RxCbKey> rxCbKeys(const SignalUniverse& u) {
    std::vector<RxCbKey> out;
    for (const auto& [sys, signals] : u) {
        out.push_back(RxCbKey{sys, ParamForm::Dcb, FrequencyBand::Unknown, SignalId{}, 0});
        for (const FrequencyBand b : keyBands(sys))
            out.push_back(RxCbKey{sys, ParamForm::Ifb, b, SignalId{}, 0});
        if (traitsOf(sys).fdma)
            for (const FrequencyBand b : keyBands(sys)) {
                if (b != FrequencyBand::GloG1 && b != FrequencyBand::GloG2) continue;
                for (int slot = -7; slot <= 6; ++slot)
                    out.push_back(RxCbKey{sys, ParamForm::GloSlotIfb, b, SignalId{}, slot});
            }
        for (const SignalId& sig : signals)
            out.push_back(RxCbKey{sys, ParamForm::Isc, FrequencyBand::Unknown, sig, 0});
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// The constellation names
// ---------------------------------------------------------------------------

TEST(ParamRefTest, NoConstellationNameIsAPrefixOfAnother) {
    // The order of a key that opens with the name equals the order of the names only if no
    // name is a prefix of another (the character after a name would then decide).
    std::vector<System> all = kSystems;
    all.push_back(System::Unknown);
    for (System a : all)
        for (System b : all) {
            if (a == b) continue;
            const std::string na = systemName(a), nb = systemName(b);
            EXPECT_NE(nb.compare(0, na.size(), na), 0) << na << " is a prefix of " << nb;
        }
}

TEST(ParamRefTest, SystemNameOrderMatchesTheInterSystemBiasKeyOrder) {
    // The inter-system bias key is the constellation name itself.
    std::vector<System> systems = kSystems;
    shuffle(systems);
    std::map<System, int, SystemNameOrder> typed;
    for (System s : systems) typed[s] = 0;
    std::vector<std::string> keys;
    for (System s : kSystems) keys.push_back(K::makeSystemTimeOffsetKey(systemName(s)));
    std::sort(keys.begin(), keys.end());
    ASSERT_EQ(typed.size(), keys.size());
    std::size_t i = 0;
    for (const auto& [sys, unused] : typed) {
        EXPECT_EQ(K::makeSystemTimeOffsetKey(systemName(sys)), keys[i]);
        ++i;
    }
    // The golden pair: GPS holds no bias (it is the reference), but the order of the two
    // names is the one the keys have.
    EXPECT_TRUE(systemNameLess(System::GPS, System::Galileo));
}

TEST(ParamRefTest, TheLetterOrderOfTheNaiveKeyInvertsGpsAndGalileo) {
    // Why the explicit comparators exist.  A key typed by satellite letter (SatSignalId
    // with the constellation and no PRN) puts Galileo ('E') before GPS ('G'); the keys put
    // "GPS_RPB_..." before "Galileo_RPB_...".  The golden runs hold exactly these two.
    const SignalId gps{System::GPS, '1', 'C'};
    const SignalId gal{System::Galileo, '1', 'C'};
    const SatSignalId naive_gps{SatId{System::GPS, 0}, gps};
    const SatSignalId naive_gal{SatId{System::Galileo, 0}, gal};
    EXPECT_TRUE(naive_gal < naive_gps);
    EXPECT_FALSE(naive_gps < naive_gal);
    const std::string key_gps = K::makeReceiverPhaseBiasKey("GPS", "L1C");
    const std::string key_gal = K::makeReceiverPhaseBiasKey("Galileo", "L1C");
    EXPECT_LT(key_gps, key_gal);
    // The named order agrees with the keys.
    EXPECT_TRUE(SignalNameOrder{}(gps, gal));
    EXPECT_FALSE(SignalNameOrder{}(gal, gps));
    // The same holds for the receiver code-bias keys.
    const RxCbKey cb_gps{System::GPS, ParamForm::Isc, FrequencyBand::Unknown,
                         SignalId{System::GPS, '1', 'W'}, 0};
    const RxCbKey cb_gal{System::Galileo, ParamForm::Dcb, FrequencyBand::Unknown, SignalId{}, 0};
    EXPECT_LT(renderRxCb(cb_gps), renderRxCb(cb_gal));
    EXPECT_TRUE(cb_gps < cb_gal);
}

// ---------------------------------------------------------------------------
// The receiver phase bias
// ---------------------------------------------------------------------------

namespace {

/// The receiver phase-bias map of one universe: typed against the string keys.
void expectRxPbOrder(const SignalUniverse& u) {
    std::vector<SignalId> signals;
    std::vector<std::string> keys;
    for (const auto& [sys, sigs] : u)
        for (const SignalId& sig : sigs) {
            signals.push_back(sig);
            keys.push_back(K::makeReceiverPhaseBiasKey(systemName(sys), sig.obsCode(ObsType::Phase)));
        }
    ASSERT_FALSE(signals.empty());
    shuffle(signals);
    std::map<SignalId, int, SignalNameOrder> typed;
    for (const SignalId& s : signals) typed[s] = 0;
    std::sort(keys.begin(), keys.end());
    ASSERT_EQ(typed.size(), keys.size());
    std::size_t i = 0;
    for (const auto& [sig, unused] : typed) {
        EXPECT_EQ(K::makeReceiverPhaseBiasKey(systemName(sig.sys), sig.obsCode(ObsType::Phase)), keys[i]);
        ++i;
    }
}

} // namespace

TEST(ParamRefTest, SignalNameOrderMatchesTheReceiverPhaseBiasKeysOverTheGoldenSignals) {
    const SignalUniverse u = goldenUniverse();
    ASSERT_TRUE(u.count(System::GPS) && u.count(System::Galileo));
    expectRxPbOrder(u);
}

TEST(ParamRefTest, SignalNameOrderMatchesTheReceiverPhaseBiasKeysOverEveryClassifiedSignal) {
    expectRxPbOrder(classifiedUniverse());
}

// ---------------------------------------------------------------------------
// The receiver code bias
// ---------------------------------------------------------------------------

namespace {

/// The receiver code-bias map of one universe: the typed order against the string order,
/// and the key parser's reading of every key against the typed fields.
void expectRxCbOrder(const SignalUniverse& u) {
    std::vector<RxCbKey> keys = rxCbKeys(u);
    ASSERT_FALSE(keys.empty());
    std::vector<std::string> strings;
    for (const RxCbKey& k : keys) strings.push_back(renderRxCb(k));
    {
        // Every typed key renders to its own state key.
        std::set<std::string> distinct(strings.begin(), strings.end());
        ASSERT_EQ(distinct.size(), strings.size());
    }
    shuffle(keys);
    std::map<RxCbKey, int> typed;
    for (const RxCbKey& k : keys) typed[k] = 0;
    std::sort(strings.begin(), strings.end());
    ASSERT_EQ(typed.size(), strings.size());
    std::size_t i = 0;
    for (const auto& [key, unused] : typed) {
        EXPECT_EQ(renderRxCb(key), strings[i]) << "at position " << i;
        ++i;
    }
    // The key parser reads each state key back as the typed key.
    for (const auto& [key, unused] : typed) {
        const std::optional<RxCbKey> back = receiverCodeBiasKey(renderRxCb(key));
        ASSERT_TRUE(back.has_value()) << renderRxCb(key);
        EXPECT_EQ(*back, key) << renderRxCb(key);
    }
}

} // namespace

TEST(ParamRefTest, RxCbKeyOrderMatchesTheStateKeysOverTheGoldenSignals) {
    const SignalUniverse u = goldenUniverse();
    ASSERT_TRUE(u.count(System::GPS) && u.count(System::Galileo));
    expectRxCbOrder(u);
}

TEST(ParamRefTest, RxCbKeyOrderMatchesTheStateKeysOverEveryClassifiedSignal) {
    expectRxCbOrder(classifiedUniverse());
}

TEST(ParamRefTest, RxCbKeySlotsOrderAsTheirSignedText) {
    // "+0" ... "+6" before "-1" ... "-7", the negative slots by magnitude: not numeric order.
    std::vector<int> slots;
    for (int s = -7; s <= 6; ++s) slots.push_back(s);
    shuffle(slots);
    std::map<RxCbKey, int> typed;
    for (int s : slots)
        typed[RxCbKey{System::GLONASS, ParamForm::GloSlotIfb, FrequencyBand::GloG1, SignalId{}, s}] = s;
    std::vector<int> got;
    for (const auto& [key, slot] : typed) got.push_back(slot);
    const std::vector<int> expected = {0, 1, 2, 3, 4, 5, 6, -1, -2, -3, -4, -5, -6, -7};
    EXPECT_EQ(got, expected);
}

TEST(ParamRefTest, TheFormOrderAloneDoesNotGiveTheStateKeyOrder) {
    // Why RxCbKey orders by name first and keeps the per-slot keys among the inter-frequency
    // keys: ParamForm declares Dcb, Ifb, Isc, GloSlotIfb in that order.
    // (1) Across constellations the name decides, whatever the form.
    EXPECT_LT(std::string("GPS_ISC_C1W"), std::string("Galileo_DCB"));
    EXPECT_GT(static_cast<int>(ParamForm::Isc), static_cast<int>(ParamForm::Dcb));
    // (2) Within one constellation the per-slot key sorts ahead of the ISC keys although its
    //     form is declared after them.
    EXPECT_LT(K::makeGlonassPerSlotIfbKey("GLONASS", '1', 5), K::makeReceiverIscKey("GLONASS", "C1C"));
    EXPECT_GT(static_cast<int>(ParamForm::GloSlotIfb), static_cast<int>(ParamForm::Isc));
    // (3) The per-band key is a prefix of its per-slot keys, and a higher band digit sorts
    //     after the per-slot keys of a lower one.
    EXPECT_LT(K::makeIfbKey("GLONASS", '1'), K::makeGlonassPerSlotIfbKey("GLONASS", '1', -4));
    EXPECT_LT(K::makeGlonassPerSlotIfbKey("GLONASS", '1', -4), K::makeIfbKey("GLONASS", '2'));
    // The typed order agrees with all three.
    const auto key = [](ParamForm f, FrequencyBand b, int slot) {
        return RxCbKey{System::GLONASS, f, b, SignalId{System::GLONASS, '1', 'C'}, slot};
    };
    EXPECT_TRUE(key(ParamForm::GloSlotIfb, FrequencyBand::GloG1, 5) < key(ParamForm::Isc, FrequencyBand::Unknown, 0));
    EXPECT_TRUE(key(ParamForm::Ifb, FrequencyBand::GloG1, 0) < key(ParamForm::GloSlotIfb, FrequencyBand::GloG1, -4));
    EXPECT_TRUE(key(ParamForm::GloSlotIfb, FrequencyBand::GloG1, -4) < key(ParamForm::Ifb, FrequencyBand::GloG2, 0));
}

// ---------------------------------------------------------------------------
// The satellite and satellite-signal families
// ---------------------------------------------------------------------------

TEST(ParamRefTest, SatSignalIdOrderMatchesTheAmbiguityAndSatelliteBiasKeysOverTheGoldenSignals) {
    // Ambiguity "G05_L1C", satellite code bias "G05|C1C", satellite phase bias "G05|L1C|PB":
    // every key of one map carries codes of one type, with a fixed suffix.
    const SignalUniverse u = goldenUniverse();
    std::vector<SatSignalId> ids;
    for (const auto& [sys, signals] : u)
        for (int prn = 1; prn <= 99; ++prn)
            for (const SignalId& sig : signals) ids.push_back(SatSignalId{SatId{sys, prn}, sig});
    ASSERT_FALSE(ids.empty());
    std::vector<std::string> amb, code_bias, phase_bias;
    for (const SatSignalId& id : ids) {
        amb.push_back(K::makeAmbiguityKey(id.sat.str(), id.signal.obsCode(ObsType::Phase)));
        code_bias.push_back(K::makeSatBiasKey(id.sat.str(), id.signal.obsCode(ObsType::Code)));
        phase_bias.push_back(K::makeSatPhaseBiasKey(id.sat.str(), id.signal.obsCode(ObsType::Phase)));
    }
    shuffle(ids);
    std::sort(ids.begin(), ids.end());
    std::sort(amb.begin(), amb.end());
    std::sort(code_bias.begin(), code_bias.end());
    std::sort(phase_bias.begin(), phase_bias.end());
    ASSERT_EQ(ids.size(), amb.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const std::string sat = ids[i].sat.str();
        EXPECT_EQ(K::makeAmbiguityKey(sat, ids[i].signal.obsCode(ObsType::Phase)), amb[i]);
        EXPECT_EQ(K::makeSatBiasKey(sat, ids[i].signal.obsCode(ObsType::Code)), code_bias[i]);
        EXPECT_EQ(K::makeSatPhaseBiasKey(sat, ids[i].signal.obsCode(ObsType::Phase)), phase_bias[i]);
    }
}

TEST(ParamRefTest, SatIdOrderMatchesTheIonosphereAndClockKeys) {
    // Slant ionosphere and phase colour "G05", satellite clock "G05|SC".
    std::vector<SatId> sats;
    for (System s : kSystems)
        for (int prn = 1; prn <= 99; ++prn) sats.push_back(SatId{s, prn});
    std::vector<std::string> iono, clock;
    for (const SatId& s : sats) {
        iono.push_back(K::makeIonosphereKey(s.str()));
        clock.push_back(K::makeSatClockKey(s.str()));
    }
    shuffle(sats);
    std::sort(sats.begin(), sats.end());
    std::sort(iono.begin(), iono.end());
    std::sort(clock.begin(), clock.end());
    for (std::size_t i = 0; i < sats.size(); ++i) {
        EXPECT_EQ(K::makeIonosphereKey(sats[i].str()), iono[i]);
        EXPECT_EQ(K::makeSatClockKey(sats[i].str()), clock[i]);
    }
}

// ---------------------------------------------------------------------------
// The moved identities
// ---------------------------------------------------------------------------

TEST(ParamRefTest, DigitOfBandInvertsBandOfDigit) {
    for (System sys : kSystems) {
        const ConstellationTraits& t = traitsOf(sys);
        for (std::size_t i = 0; i < t.n_bands; ++i) {
            const FrequencyBand band = t.bands[i].band;
            const char digit = digitOfBand(sys, band);
            EXPECT_EQ(digit, t.bands[i].digit) << systemName(sys);
            // BeiDou B2b has the digit of B2I, and the digit reads back as B2I.
            if (sys == System::BeiDou && band == FrequencyBand::BdsB2b)
                EXPECT_EQ(bandOfDigit(sys, digit), FrequencyBand::BdsB2I);
            else
                EXPECT_EQ(bandOfDigit(sys, digit), band) << systemName(sys);
        }
        EXPECT_EQ(digitOfBand(sys, FrequencyBand::Unknown), '\0');
    }
    EXPECT_EQ(digitOfBand(System::GPS, FrequencyBand::GalE1), '\0');
    EXPECT_EQ(digitOfBand(System::Unknown, FrequencyBand::GpsL1), '\0');
}

TEST(ParamRefTest, TheKindsAndFormsKeepTheirTableNames) {
    EXPECT_STREQ(paramKindName(NetworkParamKind::SatClock), "sat_clock");
    EXPECT_STREQ(paramKindName(NetworkParamKind::RxPosition), "rx_position");
    EXPECT_EQ(static_cast<std::size_t>(NetworkParamKind::RxPosition) + 1, kNetworkParamKindCount);
    EXPECT_STREQ(paramFormName(ParamForm::None), "");
    EXPECT_STREQ(paramFormName(ParamForm::GloSlotIfb), "slot");
}

// ---------------------------------------------------------------------------
// Reference and instance
// ---------------------------------------------------------------------------

namespace {

/// A reference converted to its instance, and rendered as the state key it stands for.
void expectConverts(const ParamRef& ref, const std::string& key, bool renders_back = true) {
    const ParamInstanceId id = paramInstanceOf(ref);
    EXPECT_EQ(id.cls.kind, ref.kind) << key;
    if (renders_back) EXPECT_EQ(userKey(ref), key) << describe(ref);
}

} // namespace

TEST(ParamRefTest, AnInstanceConvertsToItsReferenceAndBack) {
    const SignalUniverse u = classifiedUniverse();
    for (const auto& [sys, signals] : u) {
        for (const int prn : {5, 17}) {
            const SatId sat{sys, prn};
            const std::string s = sat.str();
            expectConverts(slantIonoRef(sat), K::makeIonosphereKey(s));
            expectConverts(phaseColourRef(sat), s);
            expectConverts(satClockRef(sat), K::makeSatClockKey(s));
            for (const SignalId& sig : signals) {
                expectConverts(ambiguityRef(sat, sig), K::makeAmbiguityKey(s, sig.obsCode(ObsType::Phase)));
                expectConverts(satPhaseBiasRef(sat, sig), K::makeSatPhaseBiasKey(s, sig.obsCode(ObsType::Phase)));
                expectConverts(satCodeBiasRef(sat, sig), K::makeSatBiasKey(s, sig.obsCode(ObsType::Code)));
            }
            for (std::size_t i = 0; i + 1 < signals.size(); i += 2) {
                const SignalPair pair{signals[i], signals[i + 1]};
                expectConverts(ambiguityIfRef(sat, pair), K::makeAmbiguityKey(s, pair.str(ObsType::Phase)));
            }
            const std::vector<FrequencyBand> bands = keyBands(sys);
            for (const FrequencyBand a : bands) {
                expectConverts(narrowLaneRef(sat, a), K::makeNlAmbiguityKey(s, digitFor(sys, a)));
                for (const FrequencyBand b : bands)
                    if (a < b)
                        expectConverts(wideLaneRef(sat, a, b),
                                       K::makeWlAmbiguityKey(s, digitFor(sys, a), digitFor(sys, b)));
            }
        }
        expectConverts(isbRef(sys), K::makeSystemTimeOffsetKey(systemName(sys)), false);
        for (const SignalId& sig : signals)
            expectConverts(rxPhaseBiasRef(sys, sig),
                           K::makeReceiverPhaseBiasKey(systemName(sys), sig.obsCode(ObsType::Phase)));
    }
    for (const RxCbKey& k : rxCbKeys(u)) {
        expectConverts(rxCodeBiasRef(k), renderRxCb(k));
        // The typed key is the reference's own fields.
        EXPECT_EQ(rxCbKeyOf(rxCodeBiasRef(k)), k) << renderRxCb(k);
    }
    for (std::uint8_t axis = 0; axis < 3; ++axis) expectConverts(rxPositionRef(axis), "", false);
    expectConverts(rxClockRef(), "", false);
    expectConverts(zwdRef(), "", false);
}

TEST(ParamRefTest, TheStationOfAnInstanceTakesPartInItsEquality) {
    const ParamRef r = ambiguityRef(*SatId::parse("G05"), SignalId{System::GPS, '1', 'C'});
    const ParamInstanceId a = paramInstanceOf(r, StationId{3});
    ParamInstanceId b = a;
    EXPECT_EQ(a, b);
    b.station = StationId{4};
    EXPECT_FALSE(a == b);
    EXPECT_EQ(paramInstanceOf(r).station.index, -1);
    EXPECT_TRUE(a.cls == paramInstanceOf(r).cls);
}

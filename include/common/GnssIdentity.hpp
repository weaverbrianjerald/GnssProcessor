#pragma once

/**
 * @file GnssIdentity.hpp
 * @brief Typed identities of the GNSS entities the network producer models: the
 *        constellation's traits, a satellite, a tracking signal, an observable, a
 *        signal pair, a station, a line of sight and an arc (
 *        docs/refound/NOM_network_object_model_2026-10-08.md section 11.1).
 *
 * ### Why this header exists
 *
 * The same identity is spelled several ways across the code: a constellation as the
 * @c System enum, as a RINEX letter ('E'), as a one-letter string ("E") and as a name
 * ("Galileo"), with private converters whose defaults disagree (an unknown letter is
 * GPS in some and "Unknown" in others); a satellite as a string that one reader
 * normalizes and another does not (blank-padded ids dropped satellites below
 * G10); a band as a digit read from @c sig[1].  Every disagreement between two such
 * readers is a defect waiting for an input that separates them.
 *
 * This header gives each identity one plain value type with one parser and one
 * rendering.  It adds no second enum: @c System (common/GnssTypes.hpp) is the
 * constellation and @c FrequencyBand (common/SignalBand.hpp) the physical band.  Strings
 * stay the serialization (state keys, snapshots, the wire, traces); in memory an
 * identity is typed.
 *
 * ### Ordering contract
 *
 * Every comparator reproduces the @c std::string order of the key it will replace, so a
 * container re-keyed from a string to one of these types keeps its iteration order (the
 * network's rank-1 inject order and fold order depend on it):
 *  - @ref SatId compares (RINEX letter, PRN): with the PRN rendered as two zero-padded
 *    digits this is the lexical order of "C01" < "E05" < "G05" < "G10" < "J01" < "R01".
 *    The letter, not the @c System enum value, orders it (the enum puts GLONASS before
 *    BeiDou, the letters put 'C' before 'R').
 *  - @ref SignalId compares (letter, band digit, attribute): the order of "1C" < "1W" <
 *    "2W" < "5Q" within one constellation.
 *  - @ref ObservableId compares (letter, observation type, digit, attribute): the order of
 *    "C1C" < "C2W" < "L1C" within one constellation.
 *  - @ref StationId, @ref LosId and @ref ArcId compare member by member in declaration
 *    order.
 *
 * The comparators read the letter from @ref traitsOf, which is an inline @c constexpr
 * table in this header, so a comparison compiles to table reads in the caller's own
 * translation unit (no out-of-line call per comparison in a hot ordered map).
 *
 * ### Scope of step N0a
 *
 * New types only: no existing call site reads them yet (step N0b migrates the call sites).
 * IRNSS has no @c System value: its letter 'I' maps to @c System::Unknown here, and
 * @ref SatId::parse refuses it.  Adding IRNSS to the enum is a user-side decision that
 * is out of scope here.
 *
 * ### Step N0b additions
 *
 * The small helpers the network call sites need to read these types in one line:
 * @ref systemOfSat (the one place a satellite string's constellation is read until step
 * N4 re-keys the containers by @ref SatId) and @ref letterOfSat (its RINEX letter, the
 * key of the containers keyed by a constellation character), @ref rinexLetter and
 * @ref systemName (the two renderings of a @c System), @ref constellationKey (the
 * one-letter string key of the containers that stay string-keyed until N4),
 * @ref obsTypeOf (the observation type of a code), @ref SignalId::wavelengthM and
 * @ref ionoDelayMetresPerTecu (frequency-derived values, written so that each reproduces
 * the expression it replaces bit for bit).  The letter reads (@ref systemFromLetter,
 * @ref systemOfSat, @ref letterOfSat) are inline @c constexpr, like @ref traitsOf: they
 * run per coupling term and per carried ambiguity.
 */

#include "common/GnssTypes.hpp"    // System
#include "common/SignalBand.hpp"   // FrequencyBand, ObsType, classifySignal

#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace GnssProcessor {

    // =======================================================================
    // Constellation traits
    // =======================================================================

    /**
     * @brief One physical band of a constellation and the RINEX frequency digit that
     *        names it in an observation code.
     *
     * BeiDou reuses digits across bands (digit 7 is B2I or B2b by attribute; digit 1 is
     * B1C or, with an I or Q attribute, the RINEX 3.02 spelling of B1I): @ref digit is
     * the digit RINEX 3.04 assigns the band (B1I is '2', B1C '1', B2I and B2b both '7').
     */
    struct BandSlot {
        char          digit{'\0'};                  ///< RINEX frequency digit ('1', '2', '5', ...).
        FrequencyBand band{FrequencyBand::Unknown}; ///< The physical band.
    };

    /// Size of @ref ConstellationTraits::bands: the largest number of physical bands one
    /// constellation has (BeiDou: B1C, B1I, B2a, B3, B2I, B2b, B2ab).
    inline constexpr std::size_t kMaxConstellationBands = 7;

    /**
     * @brief The fixed properties of one constellation, one record per @c System value.
     *
     * A plain table row: read through @ref traitsOf, never built by a caller.  The name
     * doubles as the system-time-offset (inter-system bias) state key, since
     * @c StateKeys::makeSystemTimeOffsetKey returns the name unchanged.
     */
    struct ConstellationTraits {
        System      sys{System::Unknown};  ///< The constellation.
        char        letter{'?'};           ///< RINEX satellite-id letter ('G', 'E', 'R', 'C', 'J', 'S'); '?' for Unknown.
        /// The name used by @c PppUtils::getSystemString, by the configuration and as the
        /// system-time-offset state key ("GPS", "Galileo", "GLONASS", "BeiDou", "QZSS",
        /// "SBAS"; "Unknown").
        const char* name{"Unknown"};
        bool        fdma{false};           ///< True when some band's carrier depends on the satellite's frequency slot (GLONASS G1, G2).
        /// True for the one constellation that is the time reference and the inter-system-bias
        /// reference of a multi-constellation solution: its clock defines the receiver clock
        /// state, and every other constellation carries an inter-system bias (a system time
        /// offset state) relative to it, so the reference itself carries none.  Exactly one
        /// row sets it (GPS); read it through @ref isTimeReference and
        /// @ref timeReferenceSystem, never by comparing against @c System::GPS.
        bool        time_reference{false};
        /// The constellation's physical bands in RINEX digit order; the first
        /// @ref n_bands entries are valid.
        std::array<BandSlot, kMaxConstellationBands> bands{};
        std::size_t n_bands{0};            ///< Number of valid entries in @ref bands.
    };

    namespace identity_detail {

        /**
         * @brief Build one traits row (the band list in RINEX digit order).
         *
         * @c constexpr, so the table below is constant-initialized: safe to read during
         * the static initialization of any translation unit.
         *
         * @param sys    The constellation.
         * @param letter Its RINEX letter.
         * @param name   Its name.
         * @param fdma   True for an FDMA constellation.
         * @param bands  Its bands, in RINEX digit order (at most @ref kMaxConstellationBands).
         * @param time_reference True for the time and inter-system-bias reference
         *                       constellation (see @ref ConstellationTraits::time_reference).
         * @return The row.
         */
        constexpr ConstellationTraits makeTraits(System sys, char letter, const char* name, bool fdma,
                                                 std::initializer_list<BandSlot> bands,
                                                 bool time_reference = false) {
            ConstellationTraits t;
            t.sys    = sys;
            t.letter = letter;
            t.name   = name;
            t.fdma   = fdma;
            t.time_reference = time_reference;
            for (const BandSlot& b : bands) t.bands[t.n_bands++] = b;
            return t;
        }

        using FB = FrequencyBand;

        inline constexpr ConstellationTraits kGps = makeTraits(
            System::GPS, 'G', "GPS", false,
            {{'1', FB::GpsL1}, {'2', FB::GpsL2}, {'5', FB::GpsL5}},
            /*time_reference=*/true);   // the one time and inter-system-bias reference
        inline constexpr ConstellationTraits kGalileo = makeTraits(
            System::Galileo, 'E', "Galileo", false,
            {{'1', FB::GalE1}, {'5', FB::GalE5a}, {'6', FB::GalE6}, {'7', FB::GalE5b},
             {'8', FB::GalE5}});
        inline constexpr ConstellationTraits kGlonass = makeTraits(
            System::GLONASS, 'R', "GLONASS", true,
            {{'1', FB::GloG1}, {'2', FB::GloG2}, {'3', FB::GloG3}, {'4', FB::GloG1a},
             {'6', FB::GloG2a}});
        // BeiDou digit '1' is B1C (1575.42 MHz) and digit '2' is B1I (1561.098 MHz), as in
        // RINEX 3.04.  The RINEX 3.02 spelling of B1I (C1I, C1Q) is told apart by its
        // attribute in SignalId::band, never by aliasing the digit onto B1I.
        inline constexpr ConstellationTraits kBeiDou = makeTraits(
            System::BeiDou, 'C', "BeiDou", false,
            {{'1', FB::BdsB1C}, {'2', FB::BdsB1I}, {'5', FB::BdsB2a}, {'6', FB::BdsB3},
             {'7', FB::BdsB2I}, {'7', FB::BdsB2b}, {'8', FB::BdsB2ab}});
        inline constexpr ConstellationTraits kQzss = makeTraits(
            System::QZSS, 'J', "QZSS", false,
            {{'1', FB::QzsL1}, {'2', FB::QzsL2}, {'5', FB::QzsL5}, {'6', FB::QzsL6}});
        inline constexpr ConstellationTraits kSbas = makeTraits(
            System::SBAS, 'S', "SBAS", false,
            {{'1', FB::SbsL1}, {'5', FB::SbsL5}});
        inline constexpr ConstellationTraits kUnknown = makeTraits(
            System::Unknown, '?', "Unknown", false, {});

    } // namespace identity_detail

    /**
     * @brief The traits record of a constellation.
     *
     * @param sys The constellation (any value, @c System::Unknown included).
     * @return A reference into the constant table above; the record of
     *         @c System::Unknown for a value outside the table.
     */
    constexpr const ConstellationTraits& traitsOf(System sys) {
        namespace D = identity_detail;
        switch (sys) {
        case System::GPS:     return D::kGps;
        case System::Galileo: return D::kGalileo;
        case System::GLONASS: return D::kGlonass;
        case System::BeiDou:  return D::kBeiDou;
        case System::QZSS:    return D::kQzss;
        case System::SBAS:    return D::kSbas;
        case System::Unknown: return D::kUnknown;
        }
        return D::kUnknown;
    }

    /**
     * @brief Whether a constellation is the time and inter-system-bias reference.
     *
     * Reads @ref ConstellationTraits::time_reference.  The reference constellation defines
     * the receiver clock datum and holds no inter-system bias; every other constellation
     * holds one relative to it.  Declaring the role in the table keeps the choice of
     * reference in one place instead of one comparison per use.
     *
     * @param sys The constellation (any value, @c System::Unknown included).
     * @return True for the reference constellation (GPS), false for every other value.
     */
    constexpr bool isTimeReference(System sys) { return traitsOf(sys).time_reference; }

    /**
     * @brief The constellation that is the time and inter-system-bias reference.
     *
     * The row of the traits table that sets @ref ConstellationTraits::time_reference.
     *
     * @return The reference constellation (GPS).
     */
    constexpr System timeReferenceSystem() {
        constexpr System all[] = {System::GPS, System::Galileo, System::GLONASS,
                                  System::BeiDou, System::QZSS, System::SBAS};
        for (const System s : all)
            if (traitsOf(s).time_reference) return s;
        return System::Unknown;
    }

    /**
     * @brief The physical band a RINEX frequency digit names for a constellation.
     *
     * Reads the traits table: the first band of @p sys whose digit is @p digit.  Two
     * BeiDou digits name more than one band, and a digit alone cannot tell them apart
     * (only a signal's attribute can): digit '7' is B2I and B2b, which share one carrier
     * (1207.14 MHz), and reads as B2I; digit '1' reads as B1C (the RINEX 3.02 spelling of
     * B1I, C1I or C1Q, needs the attribute; see @ref SignalId::band).
     *
     * @param sys   The constellation.
     * @param digit The RINEX frequency digit ('1', '2', '5', ...).
     * @return The band, or @c FrequencyBand::Unknown for a digit the constellation does
     *         not use (and for @c System::Unknown).
     */
    constexpr FrequencyBand bandOfDigit(System sys, char digit) {
        const ConstellationTraits& t = traitsOf(sys);
        for (std::size_t i = 0; i < t.n_bands; ++i)
            if (t.bands[i].digit == digit) return t.bands[i].band;
        return FrequencyBand::Unknown;
    }

    /**
     * @brief The constellation of a RINEX satellite-id letter.
     *
     * The single letter policy: 'G', 'E', 'R', 'C', 'J', 'S' map to their constellation;
     * every other character (IRNSS 'I' included, lower case included) maps to
     * @c System::Unknown, never to GPS.
     *
     * @param letter The letter.
     * @return The constellation, or @c System::Unknown.
     */
    constexpr System systemFromLetter(char letter) {
        switch (letter) {
        case 'G': return System::GPS;
        case 'E': return System::Galileo;
        case 'R': return System::GLONASS;
        case 'C': return System::BeiDou;
        case 'J': return System::QZSS;
        case 'S': return System::SBAS;
        default:  return System::Unknown;   // IRNSS 'I' included (no System value)
        }
    }

    /**
     * @brief The constellation of a name or a letter.
     *
     * Accepts the names of @ref ConstellationTraits::name ("GPS", "Galileo", ...) and the
     * one-letter strings of @ref systemFromLetter ("G", "E", ...), case-sensitive, as
     * @c classifySignal does.
     *
     * @param name_or_letter The text.
     * @return The constellation, or @c System::Unknown for anything else.
     */
    System systemFromName(std::string_view name_or_letter);

    /**
     * @brief The constellation of a satellite id string, read from its first character.
     *
     * The one place a satellite string's constellation is read, until step N4 re-keys the
     * network's containers by @ref SatId: call sites that hold a satellite as a string
     * ("G05") call this instead of testing @c sat[0] or @c sat.front() against a letter.
     *
     * Tolerant on purpose: only the first character is read, through
     * @ref systemFromLetter (the single letter policy), so the non-normalized spellings
     * "G 5" and "G5" read as GPS as "G05" does, and the rest of the string is not
     * validated (@ref SatId::parse is the validating parser).  An empty string, a leading
     * blank, a lower-case letter and every letter outside the table (IRNSS 'I' included)
     * give @c System::Unknown, never GPS.
     *
     * @param sat The satellite id, e.g. "G05".
     * @return The constellation, or @c System::Unknown.
     */
    constexpr System systemOfSat(std::string_view sat) {
        return sat.empty() ? System::Unknown : systemFromLetter(sat.front());
    }

    /**
     * @brief The RINEX satellite-id letter of a constellation.
     *
     * @param sys The constellation.
     * @return @ref ConstellationTraits::letter: 'G', 'E', 'R', 'C', 'J', 'S'; '?' for
     *         @c System::Unknown.
     */
    constexpr char rinexLetter(System sys) { return traitsOf(sys).letter; }

    /**
     * @brief The RINEX letter of a satellite id string's constellation: the key of the
     *        containers keyed by a constellation character.
     *
     * Short for @c rinexLetter(systemOfSat(sat)), the read that replaced @c sat[0] at the
     * network call sites.  It equals the id's first character for the six constellations
     * of the traits table; an empty id and every letter outside the table (IRNSS 'I'
     * included) give '?', so all of them share one group.  A call site that kept a
     * separate value for an empty id tests @c sat.empty() itself.
     *
     * @param sat The satellite id, e.g. "G05".
     * @return 'G', 'E', 'R', 'C', 'J', 'S', or '?'.
     */
    constexpr char letterOfSat(std::string_view sat) { return rinexLetter(systemOfSat(sat)); }

    /**
     * @brief The name of a constellation, as @c PppUtils::getSystemString and the
     *        configuration spell it.
     *
     * @param sys The constellation.
     * @return @ref ConstellationTraits::name: "GPS", "Galileo", "GLONASS", "BeiDou",
     *         "QZSS", "SBAS"; "Unknown" for @c System::Unknown.  A pointer into the
     *         constant table (static storage).
     */
    constexpr const char* systemName(System sys) { return traitsOf(sys).name; }

    /**
     * @brief The one-letter string key of a constellation ("G", "E", ...).
     *
     * A bridge for the containers that stay keyed by a constellation string until step N4
     * re-keys them by @c System (maps keyed "G", composite keys such as "G|L1C").
     * Rendered from @ref rinexLetter, so @c System::Unknown gives "?".  Equal to the
     * first character of a satellite id for the six constellations of the table; for an
     * empty id or a letter outside the table, @c constellationKey(systemOfSat(sat)) is
     * "?" where the first character itself would have been "" or, for IRNSS, "I".
     *
     * @param sys The constellation.
     * @return A one-character string.
     */
    std::string constellationKey(System sys);

    // =======================================================================
    // Frequencies
    // =======================================================================

    /**
     * @brief The carrier frequency of a physical band (Hz), the one frequency function of
     *        the identity layer.
     *
     * Values from @c Constants.  The FDMA GLONASS bands G1 and G2 add the satellite's
     * slot offset (G1 = 1602 MHz + k 0.5625 MHz, G2 = 1246 MHz + k 0.4375 MHz); every
     * other band ignores @p glo_slot.  The slot has no default on purpose: an FDMA caller
     * that does not know its slot must say so (pass 0, or call
     * @ref nominalCarrierFrequencyHz) rather than get slot 0 silently.  IRNSS L5 shares
     * 1176.45 MHz.
     *
     * @param band     The physical band.
     * @param glo_slot GLONASS FDMA channel number k (read for @c GloG1 and @c GloG2 only).
     * @return Frequency in Hz; 0.0 for @c Unknown and for the IRNSS S band (no
     *         @c Constants entry, as in @c bandNominalWavelengthMeters).
     */
    double carrierFrequencyHz(FrequencyBand band, int glo_slot);

    /**
     * @brief The nominal carrier frequency of a physical band (Hz): the centre frequency,
     *        with the FDMA GLONASS bands G1 and G2 at slot 0 (1602 MHz, 1246 MHz).
     *
     * The value @c bandNominalWavelengthMeters documents for those bands; correct for
     * every CDMA band, an approximation (up to 7 slots of 0.5625 MHz) for an FDMA
     * satellite.
     *
     * @param band The physical band.
     * @return @ref carrierFrequencyHz at slot 0.
     */
    double nominalCarrierFrequencyHz(FrequencyBand band);

    /**
     * @brief The carrier wavelength of a physical band (m): speed of light over
     *        @ref carrierFrequencyHz.
     *
     * @param band     The physical band.
     * @param glo_slot GLONASS FDMA channel number k (read for @c GloG1 and @c GloG2 only).
     * @return Wavelength in metres, or 0.0 where the frequency is 0.0.
     */
    double wavelengthM(FrequencyBand band, int glo_slot);

    /**
     * @brief The nominal carrier wavelength of a physical band (m): speed of light over
     *        @ref nominalCarrierFrequencyHz (GLONASS G1 and G2 at slot 0).
     *
     * @param band The physical band.
     * @return Wavelength in metres, or 0.0 where the frequency is 0.0.
     */
    double nominalWavelengthM(FrequencyBand band);

    /**
     * @brief The nominal carrier frequency (Hz) that a RINEX frequency digit declares for a
     *        constellation, from the digit alone.
     *
     * The digit is read through @ref bandOfDigit and the band through
     * @ref nominalCarrierFrequencyHz.  For BeiDou, digit '1' is B1C (1575.42 MHz) and digit
     * '2' is B1I (1561.098 MHz); digit '1' is never aliased to B1I.  A caller that holds
     * the full signal, with its attribute, reads @ref SignalId::band instead.
     *
     * @param sys   The constellation.
     * @param digit The RINEX frequency digit ('1', '2', '5', ...).
     * @return Frequency in Hz; 0.0 for a digit the constellation does not use.
     */
    double nominalCarrierFrequencyOfDigitHz(System sys, char digit);

    /**
     * @brief The nominal carrier wavelength (m) that a RINEX frequency digit declares for a
     *        constellation: speed of light over @ref nominalCarrierFrequencyOfDigitHz.
     *
     * @param sys   The constellation.
     * @param digit The RINEX frequency digit ('1', '2', '5', ...).
     * @return Wavelength in metres, or 0.0 for a digit the constellation does not use.
     */
    double nominalWavelengthOfDigitM(System sys, char digit);

    /**
     * @brief Metres of first-order ionospheric delay per TECU on a carrier:
     *        40.3 * 1e16 / f^2.
     *
     * One TEC unit is 1e16 electrons per square metre, and the first-order group delay is
     * 40.3 TEC / f^2 metres.  The expression is evaluated term for term as the network
     * code wrote it at each of its sites, @c 40.3 * 1.0e16 / (f * f), so that a site
     * reading this function gets the same double bit for bit (the GPS L1 value,
     * f = @ref nominalCarrierFrequencyHz of @c GpsL1, is about 0.1624 m per TECU).
     *
     * @param carrier_hz Carrier frequency (Hz), e.g. from @ref carrierFrequencyHz.
     * @return Metres per TECU; not finite when @p carrier_hz is 0.0.
     */
    double ionoDelayMetresPerTecu(double carrier_hz);

    // =======================================================================
    // Satellite
    // =======================================================================

    /**
     * @brief A satellite: its constellation and PRN.
     *
     * Rendered "G05" (letter and two zero-padded digits).  Ordered by (letter, PRN), the
     * lexical order of the rendered strings.
     */
    struct SatId {
        System sys{System::Unknown};  ///< Constellation.
        int    prn{0};                ///< PRN, 1 to 99.

        /**
         * @brief Parse a RINEX satellite id.
         *
         * Normalizes the three spellings in use: "G05", the blank-padded "G 5"
         * and the unpadded "G5".  Refuses everything else: an empty or longer
         * string, an unknown letter (@ref systemFromLetter gives Unknown), a lower-case
         * letter, a non-digit, PRN 0.
         *
         * @param text The id.
         * @return The satellite, or @c std::nullopt.
         */
        static std::optional<SatId> parse(std::string_view text);

        /**
         * @brief The canonical rendering, "G05".
         * @return The id; "?00" style output for an invalid value.
         */
        std::string str() const;

        /**
         * @brief The RINEX letter of the constellation.
         * @return @ref ConstellationTraits::letter of @ref sys.
         */
        constexpr char letter() const { return traitsOf(sys).letter; }

        /**
         * @brief True for a satellite @ref parse could have produced.
         * @return Known constellation and PRN in 1..99.
         */
        constexpr bool valid() const { return sys != System::Unknown && prn >= 1 && prn <= 99; }
    };

    /**
     * @brief Equality of two satellites.
     * @param a First satellite.
     * @param b Second satellite.
     * @return True when the constellation and the PRN are equal.
     */
    inline bool operator==(const SatId& a, const SatId& b) { return a.sys == b.sys && a.prn == b.prn; }
    /**
     * @brief Inequality of two satellites.
     * @param a First satellite.
     * @param b Second satellite.
     * @return @c !(a == b).
     */
    inline bool operator!=(const SatId& a, const SatId& b) { return !(a == b); }
    /**
     * @brief Order of two satellites: (RINEX letter, PRN), the string order of "G05".
     * @param a First satellite.
     * @param b Second satellite.
     * @return True when @p a orders before @p b.
     */
    inline bool operator<(const SatId& a, const SatId& b) {
        return std::make_tuple(a.letter(), a.prn) < std::make_tuple(b.letter(), b.prn);
    }

    // =======================================================================
    // Signal, observable, signal pair
    // =======================================================================

    /**
     * @brief The observation type of a RINEX observation code, from its first character.
     *
     * Replaces the @c code[0] == 'C' and @c code[0] == 'L' tests.  Tolerant on purpose:
     * only the first character is read, as those tests read it, so the text of an
     * ionosphere-free pair ("C1C_C2W") reads as @c ObsType::Code.  A code whose
     * constellation is known and whose form must be checked is parsed with
     * @ref ObservableId::parse instead.
     *
     * @param code The code, e.g. "C1C" or "L2W".
     * @return @c Code, @c Phase, @c Doppler or @c Snr for a first character 'C', 'L',
     *         'D' or 'S'; @c ObsType::Unknown for any other character and for an empty
     *         string.
     */
    constexpr ObsType obsTypeOf(std::string_view code) {
        if (code.empty()) return ObsType::Unknown;
        switch (code.front()) {
        case 'C': return ObsType::Code;
        case 'L': return ObsType::Phase;
        case 'D': return ObsType::Doppler;
        case 'S': return ObsType::Snr;
        default:  return ObsType::Unknown;
        }
    }

    /**
     * @brief A tracking signal of one constellation, such as GPS "1C": the band digit and
     *        the tracking attribute, without the observation type.
     *
     * The observation code of a given type is @ref obsCode ("C1C", "L1C").  Ordered by
     * (constellation letter, digit, attribute).
     */
    struct SignalId {
        System sys{System::Unknown};  ///< Constellation.
        char   digit{'\0'};           ///< RINEX frequency digit ('1'..'9', '0').
        char   attribute{'\0'};       ///< RINEX tracking attribute ('C', 'W', 'Q', ...).

        /**
         * @brief The signal of a RINEX-3 observation code.
         *
         * Accepts a three-character code whose type is C, L, D or S, whose second
         * character is a digit and whose third is an upper-case letter.  The band does
         * not have to be known to @c classifySignal (see @ref band).
         *
         * @param sys  The constellation (not Unknown).
         * @param code The code, e.g. "C1C" or "L1C".
         * @return The signal, or @c std::nullopt.
         */
        static std::optional<SignalId> fromObsCode(System sys, std::string_view code);

        /**
         * @brief The physical band, through @c classifySignal (the attribute decides the
         *        BeiDou digits that name two bands).
         * @return The band, or @c FrequencyBand::Unknown when the classifier does not list
         *         the (constellation, digit, attribute).
         */
        FrequencyBand band() const;

        /**
         * @brief The observation code of one type, e.g. "L1C" for @c ObsType::Phase.
         * @param type The observation type (its character is the code's first letter).
         * @return The three-character code.
         */
        std::string obsCode(ObsType type) const;

        /**
         * @brief The carrier frequency of the signal's band (Hz).
         * @param glo_slot GLONASS FDMA channel number k (read for the FDMA bands only; no
         *                 default, see @ref carrierFrequencyHz).
         * @return @ref carrierFrequencyHz of @ref band.
         */
        double frequencyHz(int glo_slot) const { return carrierFrequencyHz(band(), glo_slot); }

        /**
         * @brief The carrier wavelength of the signal's band (m).
         *
         * Speed of light over @ref frequencyHz, the division @c PppUtils::getWavelength
         * performs, so the two agree bit for bit wherever their frequencies agree (every
         * classified signal except the BeiDou digit-1 I and Q signals, which N0a records).
         *
         * @param glo_slot GLONASS FDMA channel number k (read for the FDMA bands only; no
         *                 default, see @ref carrierFrequencyHz).
         * @return The free function @ref GnssProcessor::wavelengthM of @ref band; 0.0 for
         *         a band the classifier does not list.
         */
        double wavelengthM(int glo_slot) const;

        /**
         * @brief The two-character rendering, "1C".
         * @return Digit and attribute.
         */
        std::string str() const;
    };

    /**
     * @brief Equality of two signals.
     * @param a First signal.
     * @param b Second signal.
     * @return True when the constellation, the digit and the attribute are equal.
     */
    inline bool operator==(const SignalId& a, const SignalId& b) {
        return a.sys == b.sys && a.digit == b.digit && a.attribute == b.attribute;
    }
    /**
     * @brief Inequality of two signals.
     * @param a First signal.
     * @param b Second signal.
     * @return @c !(a == b).
     */
    inline bool operator!=(const SignalId& a, const SignalId& b) { return !(a == b); }
    /**
     * @brief Order of two signals: (constellation letter, digit, attribute).
     * @param a First signal.
     * @param b Second signal.
     * @return True when @p a orders before @p b.
     */
    inline bool operator<(const SignalId& a, const SignalId& b) {
        return std::make_tuple(traitsOf(a.sys).letter, a.digit, a.attribute)
             < std::make_tuple(traitsOf(b.sys).letter, b.digit, b.attribute);
    }

    /**
     * @brief One observable: a signal and an observation type, rendered "L1C".
     *
     * Ordered by (constellation letter, type character, digit, attribute): within one
     * constellation the string order of the code.
     */
    struct ObservableId {
        SignalId signal{};              ///< The tracking signal.
        ObsType  type{ObsType::Unknown}; ///< Code, phase, Doppler or SNR.

        /**
         * @brief Parse a RINEX-3 observation code.
         * @param sys  The constellation.
         * @param code The code, e.g. "L1C".
         * @return The observable, or @c std::nullopt (see @ref SignalId::fromObsCode).
         */
        static std::optional<ObservableId> parse(System sys, std::string_view code);

        /**
         * @brief The observation code, "L1C".
         * @return The three-character code.
         */
        std::string str() const { return signal.obsCode(type); }
    };

    /**
     * @brief Equality of two observables.
     * @param a First observable.
     * @param b Second observable.
     * @return True when the signal and the observation type are equal.
     */
    inline bool operator==(const ObservableId& a, const ObservableId& b) {
        return a.signal == b.signal && a.type == b.type;
    }
    /**
     * @brief Inequality of two observables.
     * @param a First observable.
     * @param b Second observable.
     * @return @c !(a == b).
     */
    inline bool operator!=(const ObservableId& a, const ObservableId& b) { return !(a == b); }
    /**
     * @brief Order of two observables: (letter, type, digit, attribute), the string order
     *        of the code.
     * @param a First observable.
     * @param b Second observable.
     * @return True when @p a orders before @p b.
     */
    inline bool operator<(const ObservableId& a, const ObservableId& b) {
        return std::make_tuple(traitsOf(a.signal.sys).letter, static_cast<char>(a.type),
                               a.signal.digit, a.signal.attribute)
             < std::make_tuple(traitsOf(b.signal.sys).letter, static_cast<char>(b.type),
                               b.signal.digit, b.signal.attribute);
    }

    /**
     * @brief Two signals used together: an ionosphere-free combination or a pin pair.
     *
     * Rendered "C1C_C2W" (or "L1C_L2W" for phase), the form the ionosphere-free ambiguity
     * keys already use; the members keep the order they were given.  Ordered by (a, b).
     */
    struct SignalPair {
        SignalId a{};  ///< First member.
        SignalId b{};  ///< Second member.

        /**
         * @brief Parse "C1C_C2W": two observation codes of the same type joined by '_'.
         * @param sys  The constellation of both members.
         * @param text The pair.
         * @return The pair, or @c std::nullopt (wrong length, no '_', mixed types, or a
         *         member @ref SignalId::fromObsCode refuses).
         */
        static std::optional<SignalPair> parse(System sys, std::string_view text);

        /**
         * @brief The canonical rendering for one observation type, "C1C_C2W".
         * @param type The observation type of both members.
         * @return The pair text.
         */
        std::string str(ObsType type) const;
    };

    /**
     * @brief Equality of two pairs.
     * @param x First pair.
     * @param y Second pair.
     * @return True when both members are equal, in the same order.
     */
    inline bool operator==(const SignalPair& x, const SignalPair& y) { return x.a == y.a && x.b == y.b; }
    /**
     * @brief Inequality of two pairs.
     * @param x First pair.
     * @param y Second pair.
     * @return @c !(x == y).
     */
    inline bool operator!=(const SignalPair& x, const SignalPair& y) { return !(x == y); }
    /**
     * @brief Order of two pairs: (a, b).
     * @param x First pair.
     * @param y Second pair.
     * @return True when @p x orders before @p y.
     */
    inline bool operator<(const SignalPair& x, const SignalPair& y) {
        if (x.a != y.a) return x.a < y.a;
        return x.b < y.b;
    }

    /**
     * @brief A signal of one satellite: (satellite, signal), such as G05
     *        "1C".
     *
     * Replaces the composite keys of the per-satellite-signal state: the strings
     * "G05|L1C" and "G05_L1C" and the (satellite, code) pairs.  Ordered by (satellite,
     * signal), which is the string order of each of those keys when every key of one
     * container uses observation codes of one type (within a satellite the constellation
     * is fixed, so the signal order is the order of the digit and the attribute).  A
     * container that mixes code and phase codes keys by @ref ObservableId instead.
     */
    struct SatSignalId {
        SatId    sat{};     ///< The satellite.
        SignalId signal{};  ///< The tracking signal (of the satellite's constellation).
    };
    /**
     * @brief Equality of two satellite signals.
     * @param a First satellite signal.
     * @param b Second satellite signal.
     * @return True when the satellite and the signal are equal.
     */
    inline bool operator==(const SatSignalId& a, const SatSignalId& b) {
        return a.sat == b.sat && a.signal == b.signal;
    }
    /**
     * @brief Inequality of two satellite signals.
     * @param a First satellite signal.
     * @param b Second satellite signal.
     * @return @c !(a == b).
     */
    inline bool operator!=(const SatSignalId& a, const SatSignalId& b) { return !(a == b); }
    /**
     * @brief Order of two satellite signals: (satellite, signal).
     * @param a First satellite signal.
     * @param b Second satellite signal.
     * @return True when @p a orders before @p b.
     */
    inline bool operator<(const SatSignalId& a, const SatSignalId& b) {
        if (a.sat != b.sat) return a.sat < b.sat;
        return a.signal < b.signal;
    }

    // =======================================================================
    // Station, line of sight, arc
    // =======================================================================

    /**
     * @brief A station of the run, by its index in the run's station roster.
     *
     * The network keys station state by roster index today; the identity keeps that
     * index (its order is the roster order, the order the network iterates stations).
     */
    struct StationId {
        int index{-1};  ///< Roster index; -1 when unset.

        /**
         * @brief True for an index a roster could hold.
         * @return @ref index is non-negative.
         */
        constexpr bool valid() const { return index >= 0; }
    };

    /**
     * @brief Equality of two stations.
     * @param a First station.
     * @param b Second station.
     * @return True when the roster indices are equal.
     */
    inline bool operator==(const StationId& a, const StationId& b) { return a.index == b.index; }
    /**
     * @brief Inequality of two stations.
     * @param a First station.
     * @param b Second station.
     * @return @c !(a == b).
     */
    inline bool operator!=(const StationId& a, const StationId& b) { return !(a == b); }
    /**
     * @brief Order of two stations: roster order.
     * @param a First station.
     * @param b Second station.
     * @return True when @p a has the lower roster index.
     */
    inline bool operator<(const StationId& a, const StationId& b) { return a.index < b.index; }

    /**
     * @brief A line of sight: (station, satellite).
     *
     * Replaces the @c std::pair<int, std::string> keys and orders the same way (station
     * index, then the satellite's string order).
     */
    struct LosId {
        StationId station{};  ///< The station.
        SatId     sat{};      ///< The satellite.
    };

    /**
     * @brief Equality of two lines of sight.
     * @param a First line of sight.
     * @param b Second line of sight.
     * @return True when the station and the satellite are equal.
     */
    inline bool operator==(const LosId& a, const LosId& b) { return a.station == b.station && a.sat == b.sat; }
    /**
     * @brief Inequality of two lines of sight.
     * @param a First line of sight.
     * @param b Second line of sight.
     * @return @c !(a == b).
     */
    inline bool operator!=(const LosId& a, const LosId& b) { return !(a == b); }
    /**
     * @brief Order of two lines of sight: (station, satellite).
     * @param a First line of sight.
     * @param b Second line of sight.
     * @return True when @p a orders before @p b.
     */
    inline bool operator<(const LosId& a, const LosId& b) {
        if (a.station != b.station) return a.station < b.station;
        return a.sat < b.sat;
    }

    /**
     * @brief An arc: (station, satellite, signal).
     *
     * The arc generation (the count of discontinuities that re-opened it) joins in step
     * N4.  Ordered member by member.
     */
    struct ArcId {
        StationId station{};  ///< The station.
        SatId     sat{};      ///< The satellite.
        SignalId  signal{};   ///< The tracking signal.
    };

    /**
     * @brief Equality of two arcs.
     * @param a First arc.
     * @param b Second arc.
     * @return True when the station, the satellite and the signal are equal.
     */
    inline bool operator==(const ArcId& a, const ArcId& b) {
        return a.station == b.station && a.sat == b.sat && a.signal == b.signal;
    }
    /**
     * @brief Inequality of two arcs.
     * @param a First arc.
     * @param b Second arc.
     * @return @c !(a == b).
     */
    inline bool operator!=(const ArcId& a, const ArcId& b) { return !(a == b); }
    /**
     * @brief Order of two arcs: (station, satellite, signal).
     * @param a First arc.
     * @param b Second arc.
     * @return True when @p a orders before @p b.
     */
    inline bool operator<(const ArcId& a, const ArcId& b) {
        if (a.station != b.station) return a.station < b.station;
        if (a.sat != b.sat) return a.sat < b.sat;
        return a.signal < b.signal;
    }

} // namespace GnssProcessor

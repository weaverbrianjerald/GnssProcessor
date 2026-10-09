#pragma once

/**
 * @file StateKeySchema.hpp
 * @brief Single source of truth for every dynamic state-vector key format.
 *
 * @c StateManager owns the state vector and exposes several
 * <tt>std::map<std::string, size_t></tt> indices that grow at runtime:
 * ambiguities, per-satellite ionosphere, receiver code biases (IFB / DCB),
 * per-satellite signal biases, inter-system time offsets, and (in baseline
 * mode) double-difference ambiguities.  Each map sends a caller-formatted
 * key string to a row of the state vector.
 *
 * The key categories use different delimiters (<tt>'_'</tt> and
 * <tt>'|'</tt>), and the keys are built and read in @c MeasurementModel,
 * @c StateInitializer, @c StateManager, @c AmbiguityResolver and
 * @c DdPivotManager.  An ad hoc <tt>find()</tt> + <tt>substr()</tt> parse
 * without bounds checks breaks silently when a format changes, so the
 * formats are defined here.  Every key category has:
 *   - A documented format string (verbatim, with example).
 *   - A @c make<i>Category</i>Key(...) builder.
 *   - A matching @c parse<i>Category</i>Key(...) helper that returns
 *     <tt>std::optional</tt> of a small "<tt>...Parts</tt>" struct, or
 *     @c std::nullopt on malformed input.
 *
 * Key strings stored in @c StateManager are to be built with the makers
 * here, and code that reads an existing key is to use the parsers here
 * rather than its own <tt>find('|')</tt> walk.
 *
 * @note Every function is inline, so the header needs no translation unit.
 *       A new category follows the same pattern: format comment, @c Parts
 *       struct, @c make / @c parse pair, then migration of the call sites.
 */

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace GnssProcessor {
namespace StateKeys {

// ===========================================================================
// Delimiters & prefixes
// ===========================================================================

/// Delimiter used inside ambiguity keys ("G01_L1C").
inline constexpr char kAmbiguityDelimiter = '_';

/// Delimiter used inside per-satellite signal-bias keys ("G01|C1W") and
/// inside double-difference ambiguity keys ("DD|GPS|G05|L1|G01").
inline constexpr char kPipeDelimiter = '|';

/// Delimiter used inside receiver code-bias keys ("GPS_IFB2", "GPS_DCB").
inline constexpr char kCodeBiasDelimiter = '_';

/// Tag that opens every double-difference ambiguity key.
inline constexpr std::string_view kDdAmbiguityPrefix = "DD";

/// Receiver inter-frequency-bias infix in code-bias keys ("GPS_IFB5").
inline constexpr std::string_view kIfbInfix = "IFB";

/// FCN-slot marker in per-slot GLONASS IFB keys ("GLONASS_IFB1_k-4").
inline constexpr std::string_view kGlonassFcnMarker = "_k";

/// Receiver differential code bias infix in code-bias keys ("GPS_DCB").
inline constexpr std::string_view kDcbInfix = "DCB";

/// Receiver phase-bias infix in receiver-phase-bias keys ("GPS_RPB_L1C").
/// @c makeIfbKey / @c makeDcbKey cannot produce "_RPB_" (the infix with its
/// surrounding underscores), so the receiver phase-bias keys stay disjoint
/// from the receiver code-bias keys (Card 3, §4c).
inline constexpr std::string_view kRpbInfix = "RPB";

/// Receiver inter-signal code-bias infix ("GPS_ISC_C1W", card SCC, §4d).
/// As for @c kRpbInfix, @c makeIfbKey / @c makeDcbKey cannot produce "_ISC_",
/// and "ISC" contains neither "IFB" nor "DCB", so the three receiver code-bias
/// key families (IFB, DCB, ISC) never collide.
inline constexpr std::string_view kIscInfix = "ISC";

/// Narrow-lane infix in NlWl-basis ambiguity keys ("G01_NL_1").
inline constexpr std::string_view kNlInfix = "NL";

/// Wide-lane infix in NlWl-basis ambiguity keys ("G01_WL_1_2").
inline constexpr std::string_view kWlInfix = "WL";

// ===========================================================================
// 1. PPP ambiguity keys: "<satellite>_<signal>"
// ---------------------------------------------------------------------------
// Examples: "G01_L1C", "E11_L5Q", "G01_L1C_L5Q" (iono-free combination; the
// signal field contains the delimiter, so a parser splits on the first '_').
//
// Constructed in:  MeasurementModel.cpp (single-band and IF combinations)
// Stored in:       StateManager::ambiguity_indices_
// Parsed in:       AmbiguityResolver (per-(cons,sig) grouping for MLAMBDA)
// ===========================================================================

/// Decomposed ambiguity key.  @c signal may itself contain underscores
/// for iono-free combinations (e.g. "L1C_L5Q").
struct AmbiguityKeyParts {
    std::string satellite;  ///< Full RINEX sat ID, e.g. "G01", "E11".
    std::string signal;     ///< RINEX signal code or IF combo, e.g. "L1C".
};

/// Build a PPP ambiguity key.
inline std::string makeAmbiguityKey(std::string_view satellite,
                                    std::string_view signal) {
    std::string out;
    out.reserve(satellite.size() + 1 + signal.size());
    out.append(satellite);
    out.push_back(kAmbiguityDelimiter);
    out.append(signal);
    return out;
}

/// Parse a PPP ambiguity key.  Splits on the first underscore so that an
/// iono-free signal combination stays whole in @c signal.
/// Returns @c std::nullopt if the delimiter is missing or either side
/// would be empty.
inline std::optional<AmbiguityKeyParts>
parseAmbiguityKey(std::string_view key) {
    const auto sep = key.find(kAmbiguityDelimiter);
    if (sep == std::string_view::npos) return std::nullopt;
    if (sep == 0 || sep + 1 >= key.size()) return std::nullopt;
    return AmbiguityKeyParts{
        std::string(key.substr(0, sep)),
        std::string(key.substr(sep + 1)),
    };
}

// ===========================================================================
// 1b. NL/WL ambiguity keys: "<sat>_NL_<band>" / "<sat>_WL_<band_a>_<band_b>"
// ---------------------------------------------------------------------------
// Examples: "G01_NL_1", "G01_WL_1_2", "G01_WL_1_5", "E11_WL_1_7".
//
// Selected by GnssConfig::getStateAmbiguityBasis() == StateAmbiguityBasis::NlWl
// (config knob @c estimation.state_ambiguity_basis = "nl_wl").  The basis is
// global per run, so per-band keys (above) and NL/WL keys never coexist in a
// single state vector.
//
//   sat            : RINEX satellite ID, e.g. "G01", "E11".
//   band           : single character, the RINEX band digit (i.e. signal[1]).
//                    For NL keys this is the *reference band* the satellite's
//                    NL state is tied to (typically the lowest observed band).
//   band_a, band_b : the band digits of the two carriers in a wide-lane,
//                    stored in canonical ascending order (band_a < band_b
//                    lexicographically).  The state value semantically
//                    represents N_<band_a> - N_<band_b> in cycles.  Note this
//                    matches the MW convention for the L1/L2 case (band_a=1,
//                    band_b=2 -> N_1 - N_2).
//
// Unit convention: NL/WL state values are in *cycles*, in contrast to the
// per-band keys above which store ambiguities in meters (λ × N).  This keeps
// LAMBDA / WL-fix arithmetic native to integers; the measurement Jacobian
// applies the wavelength scaling.
//
// Constructed in:  MeasurementModel (ensureRequiredStates, buildJacobianRow)
// Stored in:       StateManager::ambiguity_indices_ (same map as per-band)
// Parsed in:       MwSlipDetector (resolve WL state index for direct update),
//                  AmbiguityResolver (split state into NL vs WL components).
// ===========================================================================

/// Decomposed NL ambiguity key.
struct NlAmbiguityKeyParts {
    std::string satellite;  ///< Full RINEX sat ID, e.g. "G01".
    char        band;       ///< RINEX band digit, e.g. '1' for L1.
};

/// Decomposed WL ambiguity key.  For a key built by @c makeWlAmbiguityKey,
/// @c band_a <= @c band_b (canonical ascending order); the parser does not
/// check the order.
struct WlAmbiguityKeyParts {
    std::string satellite;  ///< Full RINEX sat ID, e.g. "G01".
    char        band_a;     ///< Lower-numbered RINEX band digit.
    char        band_b;     ///< Higher-numbered RINEX band digit.
};

/// Build an NL ambiguity key.
inline std::string makeNlAmbiguityKey(std::string_view satellite, char band) {
    std::string out;
    out.reserve(satellite.size() + 2 + kNlInfix.size() + 1);
    out.append(satellite);
    out.push_back(kAmbiguityDelimiter);
    out.append(kNlInfix);
    out.push_back(kAmbiguityDelimiter);
    out.push_back(band);
    return out;
}

/// Build a WL ambiguity key.  @p band_a and @p band_b are sorted into
/// canonical ascending order on the way in, so the caller does not need
/// to pre-order them.
inline std::string makeWlAmbiguityKey(std::string_view satellite,
                                      char band_a, char band_b) {
    if (band_a > band_b) {
        const char tmp = band_a;
        band_a = band_b;
        band_b = tmp;
    }
    std::string out;
    out.reserve(satellite.size() + 2 + kWlInfix.size() + 3);
    out.append(satellite);
    out.push_back(kAmbiguityDelimiter);
    out.append(kWlInfix);
    out.push_back(kAmbiguityDelimiter);
    out.push_back(band_a);
    out.push_back(kAmbiguityDelimiter);
    out.push_back(band_b);
    return out;
}

/// Cheap predicate: does @p key match the "<sat>_NL_<digit>" shape?
/// Lets a scan of the mixed ambiguity map skip non-NL entries quickly.
inline bool isNlAmbiguityKey(std::string_view key) {
    // Minimum: 1-char sat + "_NL_" + 1-char band = 6 chars.
    if (key.size() < 6) return false;
    if (key.back() < '0' || key.back() > '9') return false;
    // The "_NL_" tail is fixed-width: positions [size-4, size-2) hold "NL",
    // and the two surrounding chars must be the ambiguity delimiter.
    const auto n = key.size();
    return key[n - 5] == kAmbiguityDelimiter
        && key[n - 4] == kNlInfix[0]
        && key[n - 3] == kNlInfix[1]
        && key[n - 2] == kAmbiguityDelimiter;
}

/// Cheap predicate: does @p key match the "<sat>_WL_<digit>_<digit>" shape?
inline bool isWlAmbiguityKey(std::string_view key) {
    // Minimum: 1-char sat + "_WL_" + "<d>_<d>" = 8 chars.
    if (key.size() < 8) return false;
    if (key.back() < '0' || key.back() > '9') return false;
    const auto n = key.size();
    if (key[n - 2] != kAmbiguityDelimiter) return false;
    if (key[n - 3] < '0' || key[n - 3] > '9') return false;
    return key[n - 7] == kAmbiguityDelimiter
        && key[n - 6] == kWlInfix[0]
        && key[n - 5] == kWlInfix[1]
        && key[n - 4] == kAmbiguityDelimiter;
}

/// Parse an NL ambiguity key.  Returns @c std::nullopt if the shape does
/// not match @c "<sat>_NL_<digit>" or the satellite field is empty.
inline std::optional<NlAmbiguityKeyParts>
parseNlAmbiguityKey(std::string_view key) {
    if (!isNlAmbiguityKey(key)) return std::nullopt;
    const auto sat_len = key.size() - 5;  // size - "_NL_<d>"
    if (sat_len == 0) return std::nullopt;
    return NlAmbiguityKeyParts{
        std::string(key.substr(0, sat_len)),
        key.back(),
    };
}

/// Parse a WL ambiguity key.  Returns @c std::nullopt if the shape does
/// not match @c "<sat>_WL_<digit>_<digit>" or the satellite field is empty.
inline std::optional<WlAmbiguityKeyParts>
parseWlAmbiguityKey(std::string_view key) {
    if (!isWlAmbiguityKey(key)) return std::nullopt;
    const auto sat_len = key.size() - 7;  // size - "_WL_<d>_<d>"
    if (sat_len == 0) return std::nullopt;
    return WlAmbiguityKeyParts{
        std::string(key.substr(0, sat_len)),
        key[key.size() - 3],
        key.back(),
    };
}

// ===========================================================================
// 2. Double-difference ambiguity keys: "DD|<cons>|<sat_i>|<code>|<pivot>"
// ---------------------------------------------------------------------------
// Examples: "DD|GPS|G05|L1|G01", "DD|Galileo|E11|E5|E24"
//
//   cons   : constellation name string (e.g. "GPS", "Galileo", "BeiDou")
//   sat_i  : non-pivot satellite ID    (e.g. "G05", "E11")
//   code   : RINEX frequency code      (e.g. "L1", "E5")
//   pivot  : reference satellite ID    (e.g. "G01")
//
// Constructed in:  DdPivotManager (handover S-transform), MeasurementModel
// Parsed in:       DdPivotManager (handover bucketing)
// ===========================================================================

/// Decomposed DD ambiguity key.
struct DdAmbiguityKeyParts {
    std::string cons;       ///< Constellation name ("GPS", "Galileo", ...).
    std::string sat_i;      ///< Non-pivot satellite ID.
    std::string code;       ///< RINEX frequency code.
    std::string pivot;      ///< Pivot (reference) satellite ID.
};

/// Build a DD ambiguity key.
inline std::string makeDdAmbiguityKey(std::string_view cons,
                                      std::string_view sat_i,
                                      std::string_view code,
                                      std::string_view pivot) {
    std::string out;
    out.reserve(kDdAmbiguityPrefix.size() + 4 + cons.size() + sat_i.size()
                + code.size() + pivot.size());
    out.append(kDdAmbiguityPrefix);
    out.push_back(kPipeDelimiter);
    out.append(cons);
    out.push_back(kPipeDelimiter);
    out.append(sat_i);
    out.push_back(kPipeDelimiter);
    out.append(code);
    out.push_back(kPipeDelimiter);
    out.append(pivot);
    return out;
}

/// Test whether a key string is in the DD-ambiguity namespace, without
/// fully parsing it.  Cheap and intentional: code that scans a mixed
/// ambiguity map can fast-skip non-DD entries before paying for parse.
inline bool isDdAmbiguityKey(std::string_view key) {
    if (key.size() <= kDdAmbiguityPrefix.size() + 1) return false;
    return key.substr(0, kDdAmbiguityPrefix.size()) == kDdAmbiguityPrefix
        && key[kDdAmbiguityPrefix.size()] == kPipeDelimiter;
}

/// Parse a DD ambiguity key.  Returns @c std::nullopt if the prefix or
/// any of the four pipes are missing, or if any field would be empty.
inline std::optional<DdAmbiguityKeyParts>
parseDdAmbiguityKey(std::string_view key) {
    if (!isDdAmbiguityKey(key)) return std::nullopt;
    // p0 is the pipe immediately after "DD".
    const auto p0 = kDdAmbiguityPrefix.size();
    const auto p1 = key.find(kPipeDelimiter, p0 + 1);
    if (p1 == std::string_view::npos) return std::nullopt;
    const auto p2 = key.find(kPipeDelimiter, p1 + 1);
    if (p2 == std::string_view::npos) return std::nullopt;
    const auto p3 = key.find(kPipeDelimiter, p2 + 1);
    if (p3 == std::string_view::npos) return std::nullopt;
    // A fifth pipe would mean an unknown trailing field; reject it.
    if (key.find(kPipeDelimiter, p3 + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    DdAmbiguityKeyParts parts{
        std::string(key.substr(p0 + 1, p1 - p0 - 1)),
        std::string(key.substr(p1 + 1, p2 - p1 - 1)),
        std::string(key.substr(p2 + 1, p3 - p2 - 1)),
        std::string(key.substr(p3 + 1)),
    };
    if (parts.cons.empty() || parts.sat_i.empty()
        || parts.code.empty() || parts.pivot.empty()) {
        return std::nullopt;
    }
    return parts;
}

// ===========================================================================
// 3. Per-satellite ionosphere keys: "<satellite>"
// ---------------------------------------------------------------------------
// Just the bare satellite ID, no delimiter.  Present only in PPP-RTK
// uncombined mode.
//
// Constructed in:  MeasurementModel (StateManager::addIonosphereState(sat))
// Stored in:       StateManager::ionosphere_indices_
// ===========================================================================

/// Build a per-satellite ionosphere key.  Returns the satellite ID unchanged;
/// the function exists for symmetry and so that <tt>grep makeIonosphereKey</tt>
/// finds every site.
inline std::string makeIonosphereKey(std::string_view satellite) {
    return std::string(satellite);
}

// ===========================================================================
// 4. Receiver code-bias keys: "<system>_IFB<band>" or "<system>_DCB"
// ---------------------------------------------------------------------------
// Examples: "GPS_IFB2", "GPS_IFB5", "GPS_DCB", "Galileo_IFB7"
//
//   system : system name string from PppUtils::getSystemString()
//            ("GPS", "Galileo", "BeiDou", "GLONASS", "QZSS", "SBAS")
//   band   : single character, the RINEX frequency-band digit, i.e.
//            obs_signal[1] (the digit between the observation-type letter
//            C or L and the tracking-attribute suffix).
//   "DCB"  : receiver differential code bias.  StateInitializer::
//            resolveReceiverBias maps both bands of the system's clock-reference
//            signal pair to "<system>_DCB" (each with its own coefficient) and
//            gives every other band its own "<system>_IFB<band>" state.
//
// Constructed in:  StateInitializer (resolveReceiverBias)
// Stored in:       StateManager::code_bias_indices_
// ===========================================================================

/// Build an inter-frequency receiver bias key.
inline std::string makeIfbKey(std::string_view system, char band) {
    std::string out;
    out.reserve(system.size() + 1 + kIfbInfix.size() + 1);
    out.append(system);
    out.push_back(kCodeBiasDelimiter);
    out.append(kIfbInfix);
    out.push_back(band);
    return out;
}

/// Build a receiver differential code bias key.
inline std::string makeDcbKey(std::string_view system) {
    std::string out;
    out.reserve(system.size() + 1 + kDcbInfix.size());
    out.append(system);
    out.push_back(kCodeBiasDelimiter);
    out.append(kDcbInfix);
    return out;
}

// ===========================================================================
// 4b. Per-FCN-slot GLONASS receiver IFB keys: "GLONASS_IFB<band>_k<±slot>"
// ---------------------------------------------------------------------------
// Examples: "GLONASS_IFB1_k-4", "GLONASS_IFB2_k+5"
//
//   system : system name string (always "GLONASS" in practice).
//   band   : FDMA band digit, '1' (G1) or '2' (G2).  CDMA bands keep the
//            per-band makeIfbKey form (their frequency is slot-independent).
//   slot   : GLONASS FDMA channel number k ∈ [-7,+6], printed with an
//            explicit sign ("+5" / "-4").  The central channel k=0 is the
//            held datum; StateInitializer creates no key for it.
//
// Unlike the per-band IFB/DCB keys above, these are not receiver-common: two
// GLONASS satellites on different slots see different group delays, so the
// columns survive between-satellite single differencing.  BssdTransform
// (receiver-block zeroing) and StateManager (process-noise suppression)
// special-case them via isGlonassPerSlotIfbKey().
//
// Constructed in:  StateInitializer (resolveReceiverBias, GLONASS branch)
// Stored in:       StateManager::code_bias_indices_
// ===========================================================================

/// Decomposed per-slot GLONASS IFB key.
struct GlonassPerSlotIfbKeyParts {
    char band;       ///< FDMA band digit ('1' or '2').
    int  fcn_slot;   ///< GLONASS FDMA channel number k ∈ [-7,+6].
    /// The system name before "_IFB" ("GLONASS"); empty when the key does not open with
    /// "<system>_" before the infix (the parser does not require it).  Card NOM N1: the
    /// parameter inventory reads the constellation from here rather than assuming it.
    std::string system{};
};

/// Build a per-FCN-slot GLONASS receiver IFB key, e.g. "GLONASS_IFB1_k-4".
inline std::string makeGlonassPerSlotIfbKey(std::string_view system,
                                            char band, int fcn_slot) {
    std::string out;
    out.reserve(system.size() + 1 + kIfbInfix.size() + 1
                + kGlonassFcnMarker.size() + 3);
    out.append(system);
    out.push_back(kCodeBiasDelimiter);
    out.append(kIfbInfix);
    out.push_back(band);
    out.append(kGlonassFcnMarker);             // "_k"
    if (fcn_slot >= 0) out.push_back('+');     // std::to_string keeps '-' for negatives
    out.append(std::to_string(fcn_slot));
    return out;
}

/// Cheap predicate: is @p key a per-slot GLONASS IFB key (the IFB infix
/// followed somewhere by the "_k" FCN marker)?  The per-band IFB/DCB keys lack
/// "_k" and return false, which lets BssdTransform / StateManager tell the
/// SD-surviving per-slot columns from the receiver-common ones.
inline bool isGlonassPerSlotIfbKey(std::string_view key) {
    const auto ifb = key.find(kIfbInfix);
    if (ifb == std::string_view::npos) return false;
    return key.find(kGlonassFcnMarker, ifb + kIfbInfix.size())
           != std::string_view::npos;
}

/// Parse a per-slot GLONASS IFB key "<system>_IFB<band>_k<±slot>".  From the
/// first "IFB" onward the key must hold one band char, "_k", an optional sign
/// and one or more digits; otherwise returns @c std::nullopt.  The text before
/// "IFB" and the slot range are not checked.
inline std::optional<GlonassPerSlotIfbKeyParts>
parseGlonassPerSlotIfbKey(std::string_view key) {
    const auto ifb = key.find(kIfbInfix);
    if (ifb == std::string_view::npos) return std::nullopt;
    const auto band_pos = ifb + kIfbInfix.size();
    const auto kpos = key.find(kGlonassFcnMarker, band_pos);
    if (kpos == std::string_view::npos || kpos != band_pos + 1)
        return std::nullopt;                       // exactly one band char before "_k"
    const char band = key[band_pos];
    std::string_view num = key.substr(kpos + kGlonassFcnMarker.size());
    if (num.empty()) return std::nullopt;
    bool neg = false;
    if (num.front() == '+' || num.front() == '-') {
        neg = (num.front() == '-');
        num.remove_prefix(1);
    }
    if (num.empty()) return std::nullopt;
    int mag = 0;
    for (const char c : num) {
        if (c < '0' || c > '9') return std::nullopt;
        mag = mag * 10 + (c - '0');
    }
    std::string system;
    if (ifb > 0 && key[ifb - 1] == kCodeBiasDelimiter)
        system = std::string(key.substr(0, ifb - 1));
    return GlonassPerSlotIfbKeyParts{ band, neg ? -mag : mag, std::move(system) };
}

/// Decomposed receiver per-band IFB key ("<system>_IFB<band>").
struct IfbKeyParts {
    std::string system;  ///< system name string (e.g. "GPS", "Galileo")
    char        band;    ///< RINEX frequency-band digit
};

/// Parse a receiver-common per-band IFB key "<system>_IFB<band>".  Returns
/// @c std::nullopt for the per-slot GLONASS form ("<system>_IFB<band>_k<±slot>",
/// which carries its own central-channel datum) and for any key where "IFB" is
/// not preceded by '_' or not followed by exactly one band char.  Inverse of
/// @c makeIfbKey and the receiver-common counterpart to
/// @c parseGlonassPerSlotIfbKey: a caller uses it to select the receiver-common
/// code-bias columns (the ones a network common-mode pin / S-basis datum acts on)
/// and skip the SD-surviving per-slot GLONASS columns.
inline std::optional<IfbKeyParts> parseIfbKey(std::string_view key) {
    if (isGlonassPerSlotIfbKey(key)) return std::nullopt;   // per-slot, not per-band
    const auto ifb = key.find(kIfbInfix);
    if (ifb == std::string_view::npos || ifb == 0) return std::nullopt;
    if (key[ifb - 1] != kCodeBiasDelimiter) return std::nullopt;     // need "<sys>_IFB"
    const auto band_pos = ifb + kIfbInfix.size();
    if (band_pos + 1 != key.size()) return std::nullopt;             // exactly one band char
    return IfbKeyParts{ std::string(key.substr(0, ifb - 1)), key[band_pos] };
}

/// Parse a receiver DCB key "<system>_DCB".  Returns the system name string, or
/// @c std::nullopt unless @p key is exactly "<system>_DCB".  Inverse of
/// @c makeDcbKey.
inline std::optional<std::string> parseDcbKey(std::string_view key) {
    const auto dcb = key.find(kDcbInfix);
    if (dcb == std::string_view::npos || dcb == 0) return std::nullopt;
    if (key[dcb - 1] != kCodeBiasDelimiter) return std::nullopt;     // need "<sys>_DCB"
    if (dcb + kDcbInfix.size() != key.size()) return std::nullopt;   // nothing after "DCB"
    return std::string(key.substr(0, dcb - 1));
}

// ===========================================================================
// 4c. Receiver phase-bias keys: "<system>_RPB_<signal>"
// ---------------------------------------------------------------------------
// Examples: "GPS_RPB_L1C", "Galileo_RPB_L5Q"
//
//   system : system name string from PppUtils::getSystemString()
//            ("GPS", "Galileo", "BeiDou", "QZSS", ...).
//   signal : full RINEX signal code (3 chars; the network allocates these on
//            raw phase signals only, so no iono-free combination with an
//            embedded '_').
//
// The per-(receiver, system, signal) satellite-phase-bias split state
// δ_{r,σ} of the Eq-34 / de Jonge single-receiver-chain phase datum (Card 3,
// network.phase_datum_eq34).  With its own estimable state, the receiver
// fractional phase bias does not lump into the emitted satellite phase bias,
// so the emitted between-satellite reference stays integer-consistent across
// all satellites (each chain hop contributes only an integer).  These are
// local states (observed by one station), kept in the separate map
// StateManager::receiver_phase_bias_indices_.  The "_RPB_" infix keeps the
// keys disjoint from the receiver code-bias keys (§4): parseIfbKey /
// parseDcbKey look for the "IFB"/"DCB" infix and never match "RPB".
//
// Constructed in:  the network driver / MeasurementModel (eq34 branch only)
// Stored in:       StateManager::receiver_phase_bias_indices_
// Parsed in:       the network driver (RPB pin fold, prune bookkeeping)
// ===========================================================================

/// Decomposed receiver phase-bias key ("<system>_RPB_<signal>").
struct ReceiverPhaseBiasKeyParts {
    std::string system;  ///< system name string (e.g. "GPS", "Galileo").
    std::string signal;  ///< full RINEX signal code (e.g. "L1C").
};

/// Build a receiver phase-bias key "<system>_RPB_<signal>".
inline std::string makeReceiverPhaseBiasKey(std::string_view system,
                                            std::string_view signal) {
    std::string out;
    out.reserve(system.size() + 2 + kRpbInfix.size() + signal.size());
    out.append(system);
    out.push_back(kCodeBiasDelimiter);   // '_'
    out.append(kRpbInfix);
    out.push_back(kCodeBiasDelimiter);   // '_'
    out.append(signal);
    return out;
}

/// True iff @p key carries the "_RPB_" infix (cheap namespace test).
inline bool isReceiverPhaseBiasKey(std::string_view key) {
    // Need "<sys>_RPB_<sig>": at least one char of system before "_RPB_" and
    // at least one signal char after it.
    const std::string_view needle = "_RPB_";
    const auto pos = key.find(needle);
    return pos != std::string_view::npos
        && pos > 0
        && pos + needle.size() < key.size();
}

/// Parse a receiver phase-bias key.  Returns @c std::nullopt unless the key
/// has the exact form "<system>_RPB_<signal>" with non-empty system and signal.
inline std::optional<ReceiverPhaseBiasKeyParts>
parseReceiverPhaseBiasKey(std::string_view key) {
    const std::string_view needle = "_RPB_";
    const auto pos = key.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    if (pos == 0 || pos + needle.size() >= key.size()) return std::nullopt;
    return ReceiverPhaseBiasKeyParts{
        std::string(key.substr(0, pos)),
        std::string(key.substr(pos + needle.size())),
    };
}

// ===========================================================================
// 4d. Receiver inter-signal code-bias keys: "<system>_ISC_<signal>"
// ---------------------------------------------------------------------------
// Examples: "GPS_ISC_C1W", "GPS_ISC_C1X"
//
//   system : system name string from PppUtils::getSystemString()
//            ("GPS", "Galileo", "BeiDou", "QZSS", ...).
//   signal : full RINEX code-observation signal code (3 chars).
//
// Card SCC (docs/refound/SCC_second_code_channel_2026-09-21.md, R2).  With
// `network.second_code_channel` a station contributes every allow-listed band-1
// code it tracks, not only its latched canonical one (with
// `processing.channel_mode: all`, any admitted non-canonical code on any band;
// card SID phase 6b).  The receiver group delay
// of the second code differs from the canonical one by an inter-signal
// correction, and the per-band receiver key (§4, "<sys>_DCB" for a pin band)
// cannot express it: C1C and C1W both resolve to "GPS_DCB" with the same
// coefficient, so a second band-1 code would assert a zero receiver inter-signal
// bias and push the difference into the satellite column.  (Card B5D records
// what an asserted zero inter-signal bias costs on a third band.)
//
// The non-canonical code therefore gets its own per-(receiver, system, signal)
// state with unit coefficient: an absolute receiver delay in the frame the clock
// and ionosphere datum leave free, rather than the ionosphere-referenced
// difference the DCB state carries.  The one null direction this creates (every
// receiver inter-signal bias against every satellite one) is closed by the
// satellite family's own time-linked sum-to-zero row, so card SCC changes no
// datum row (R3).
//
// Key disjointness: the key must match none of parseIfbKey, parseDcbKey and
// isGlonassPerSlotIfbKey.  Application::networkSplitStation_ drops every receiver
// column whose key contains "_IFB", and the receiver code-bias zero-mean datum
// prior is gated on parseDcbKey and must not act on this state (it carries no
// zero-mean prior, R2).  "ISC" contains neither infix, so all three reject the
// key by construction; the negative parser tests in
// tests/test_network_second_code_scope.cpp pin the property.
//
// Constructed in:  StateInitializer (resolveReceiverBias, SCC branch)
// Stored in:       StateManager::code_bias_indices_ (beside the IFB/DCB states,
//                  so the receiver code-bias carry restores it, R2)
// Parsed in:       Application::networkSplitStation_ (the R5 sigma scale),
//                  the network teardown (the R7 state census)
// ===========================================================================

/// Decomposed receiver inter-signal code-bias key ("<system>_ISC_<signal>").
struct ReceiverIscKeyParts {
    std::string system;  ///< system name string (e.g. "GPS", "Galileo").
    std::string signal;  ///< full RINEX code signal (e.g. "C1W").
};

/// Build a receiver inter-signal code-bias key "<system>_ISC_<signal>".
inline std::string makeReceiverIscKey(std::string_view system,
                                      std::string_view signal) {
    std::string out;
    out.reserve(system.size() + 2 + kIscInfix.size() + signal.size());
    out.append(system);
    out.push_back(kCodeBiasDelimiter);   // '_'
    out.append(kIscInfix);
    out.push_back(kCodeBiasDelimiter);   // '_'
    out.append(signal);
    return out;
}

/// True iff @p key carries the "_ISC_" infix with a non-empty system and signal
/// on either side (cheap namespace test, the @c isReceiverPhaseBiasKey twin).
inline bool isReceiverIscKey(std::string_view key) {
    const std::string_view needle = "_ISC_";
    const auto pos = key.find(needle);
    return pos != std::string_view::npos
        && pos > 0
        && pos + needle.size() < key.size();
}

/// Parse a receiver inter-signal code-bias key.  Returns @c std::nullopt unless
/// the key has the exact form "<system>_ISC_<signal>" with non-empty system and
/// signal.  Inverse of @c makeReceiverIscKey.
inline std::optional<ReceiverIscKeyParts>
parseReceiverIscKey(std::string_view key) {
    const std::string_view needle = "_ISC_";
    const auto pos = key.find(needle);
    if (pos == std::string_view::npos) return std::nullopt;
    if (pos == 0 || pos + needle.size() >= key.size()) return std::nullopt;
    return ReceiverIscKeyParts{
        std::string(key.substr(0, pos)),
        std::string(key.substr(pos + needle.size())),
    };
}

// ===========================================================================
// 5. Per-satellite signal-bias keys: "<satellite>|<signal>"
// ---------------------------------------------------------------------------
// Examples: "G01|C1W", "E11|C5Q", "E24|C7Q"
//
//   satellite : full RINEX sat ID
//   signal    : full RINEX signal code (3 chars)
//
// Constructed in:  MeasurementModel, StateInitializer
// Stored in:       StateManager::sat_bias_indices_
// Parsed in:       StateManager::getProcessNoiseMatrix (the BSSD freeze check
//                  reads only the satellite prefix; see
//                  satelliteFromSatBiasKey below).
// ===========================================================================

/// Decomposed sat-bias key.
struct SatBiasKeyParts {
    std::string satellite;  ///< Full RINEX sat ID.
    std::string signal;     ///< Full RINEX signal code.
};

/// Build a per-satellite signal-bias key.
inline std::string makeSatBiasKey(std::string_view satellite,
                                  std::string_view signal) {
    std::string out;
    out.reserve(satellite.size() + 1 + signal.size());
    out.append(satellite);
    out.push_back(kPipeDelimiter);
    out.append(signal);
    return out;
}

/// Parse a per-satellite signal-bias key.  Returns @c std::nullopt if
/// the delimiter is missing or either side is empty.
inline std::optional<SatBiasKeyParts>
parseSatBiasKey(std::string_view key) {
    const auto bar = key.find(kPipeDelimiter);
    if (bar == std::string_view::npos) return std::nullopt;
    if (bar == 0 || bar + 1 >= key.size()) return std::nullopt;
    return SatBiasKeyParts{
        std::string(key.substr(0, bar)),
        std::string(key.substr(bar + 1)),
    };
}

/// Extract the satellite prefix from a sat-bias key.  A key with no pipe
/// (legacy or malformed) is returned whole, which keeps the lenient behavior of
/// the StateManager::getProcessNoiseMatrix loop.
inline std::string satelliteFromSatBiasKey(std::string_view key) {
    const auto bar = key.find(kPipeDelimiter);
    return std::string(bar != std::string_view::npos
                           ? key.substr(0, bar)
                           : key);
}

// ===========================================================================
// 5b. Per-satellite signal phase-bias keys: "<satellite>|<signal>|PB"
// ---------------------------------------------------------------------------
// Examples: "G01|L1C|PB", "E11|L5Q|PB"
//
// The network producer (PPP_RTK_Network) estimates an explicit per-(satellite,
// signal) satellite phase bias (Odijk Eq. 4; it carries the network pivot-
// receiver ambiguity lump).  It is stored in StateManager::sat_phase_bias_indices_,
// a map separate from sat_bias_indices_ (code bias, section 5).  The trailing
// "|PB" segment keeps the two key sets disjoint:
//   - satelliteFromSatBiasKey / satelliteFromSatPhaseBiasKey both split on the
//     first pipe, so either returns the satellite correctly;
//   - a phase-bias key is never stored in sat_bias_indices_, so parseSatBiasKey
//     (which would read "L1C|PB" as the signal) is never applied to it.
//
// Constructed in:  MeasurementModel, the network estimator
// Stored in:       StateManager::sat_phase_bias_indices_
// Parsed in:       StateManager::getProcessNoiseMatrix (the pivot-freeze check
//                  reads only the satellite prefix; see satelliteFromSatPhaseBiasKey).
// ===========================================================================

/// Trailing discriminator separating a phase-bias key from a code-bias key.
inline constexpr std::string_view kSatPhaseBiasSuffix = "|PB";

/// Build a per-satellite signal phase-bias key.
inline std::string makeSatPhaseBiasKey(std::string_view satellite,
                                       std::string_view signal) {
    std::string out;
    out.reserve(satellite.size() + 1 + signal.size() + kSatPhaseBiasSuffix.size());
    out.append(satellite);
    out.push_back(kPipeDelimiter);
    out.append(signal);
    out.append(kSatPhaseBiasSuffix);
    return out;
}

/// True iff @p key is a satellite phase-bias key (cheap trailing-suffix test).
inline bool isSatPhaseBiasKey(std::string_view key) {
    return key.size() > kSatPhaseBiasSuffix.size()
        && key.substr(key.size() - kSatPhaseBiasSuffix.size()) == kSatPhaseBiasSuffix;
}

/// Parse a per-satellite signal phase-bias key.  Returns @c std::nullopt unless
/// the key has the form "<sat>|<signal>|PB" with non-empty satellite and signal.
inline std::optional<SatBiasKeyParts>
parseSatPhaseBiasKey(std::string_view key) {
    if (!isSatPhaseBiasKey(key)) return std::nullopt;
    const std::string_view body = key.substr(0, key.size() - kSatPhaseBiasSuffix.size());
    const auto bar = body.find(kPipeDelimiter);
    if (bar == std::string_view::npos) return std::nullopt;
    if (bar == 0 || bar + 1 >= body.size()) return std::nullopt;
    return SatBiasKeyParts{
        std::string(body.substr(0, bar)),
        std::string(body.substr(bar + 1)),
    };
}

/// Extract just the satellite prefix from a sat-phase-bias key (first-pipe split,
/// matching satelliteFromSatBiasKey's lenient fallback).
inline std::string satelliteFromSatPhaseBiasKey(std::string_view key) {
    const auto bar = key.find(kPipeDelimiter);
    return std::string(bar != std::string_view::npos
                           ? key.substr(0, bar)
                           : key);
}

// ===========================================================================
// 5c. Per-satellite clock keys: "<satellite>|SC"
// ---------------------------------------------------------------------------
// Examples: "G01|SC", "E11|SC"
//
// The network producer (PPP_RTK_Network) estimates an explicit per-satellite
// clock correction (Δδtˢ in metres, relative to the a-priori precise/real-time
// product clock already removed in obs.modeled) to stay consistent with the
// chosen S-system datum.  Unlike the code/phase biases there is one clock per
// satellite, not per signal, so the key carries no signal field.  It is stored
// in StateManager::sat_clock_indices_, a map separate from the bias maps, and
// the trailing "|SC" distinguishes the keys.  The satellite orbit (position)
// stays fixed from the product; only the clock is estimated.
//
// Constructed in:  the network driver / estimator
// Stored in:       StateManager::sat_clock_indices_
// Parsed in:       StateManager::getProcessNoiseMatrix (the frozen-pivot check
//                  reads only the satellite prefix; see satelliteFromSatClockKey).
// ===========================================================================

/// Trailing discriminator marking a per-satellite clock key.
inline constexpr std::string_view kSatClockSuffix = "|SC";

/// Build a per-satellite clock key "<satellite>|SC".
inline std::string makeSatClockKey(std::string_view satellite) {
    std::string out;
    out.reserve(satellite.size() + kSatClockSuffix.size());
    out.append(satellite);
    out.append(kSatClockSuffix);
    return out;
}

/// True iff @p key is a per-satellite clock key (cheap trailing-suffix test).
inline bool isSatClockKey(std::string_view key) {
    return key.size() > kSatClockSuffix.size()
        && key.substr(key.size() - kSatClockSuffix.size()) == kSatClockSuffix;
}

/// Extract the satellite from a sat-clock key.  A key without the "|SC" suffix
/// is returned whole.
inline std::string satelliteFromSatClockKey(std::string_view key) {
    if (!isSatClockKey(key)) return std::string(key);
    return std::string(key.substr(0, key.size() - kSatClockSuffix.size()));
}

/// Parse a per-satellite clock key.  Returns the satellite, or std::nullopt
/// unless the key has the form "<sat>|SC" with a non-empty satellite that
/// contains no pipe.
inline std::optional<std::string> parseSatClockKey(std::string_view key) {
    if (!isSatClockKey(key)) return std::nullopt;
    const std::string_view sat = key.substr(0, key.size() - kSatClockSuffix.size());
    if (sat.empty() || sat.find(kPipeDelimiter) != std::string_view::npos) return std::nullopt;
    return std::string(sat);
}

// ===========================================================================
// 6. System-time-offset keys: "<system>"
// ---------------------------------------------------------------------------
// The bare system name string ("Galileo", "GLONASS", "BeiDou", "QZSS",
// "SBAS").  GPS is the reference and never appears.  Under between-satellite
// single differencing (BSSD) no system-time-offset state is allocated.
//
// Constructed in:  StateManager constructor (loop over active systems)
// Stored in:       StateManager::system_time_offset_indices_
// ===========================================================================

/// Build a system-time-offset key.  Returns the system name unchanged; the
/// function exists for symmetry.
inline std::string makeSystemTimeOffsetKey(std::string_view system) {
    return std::string(system);
}

// ===========================================================================
// 7. BSSD pivot-seed keys: "<cons>|<band>"
// ---------------------------------------------------------------------------
// Examples: "GPS|L1", "Galileo|E5", "BeiDou|B2"
//
//   cons : constellation string (currently the same string used in DD
//          ambiguity keys)
//   band : band label; the helper joins whatever string it is given.
//
// makePivotSeedKey has no production caller.  MeasurementModel (BSSD apply
// path) builds its pivot_abs_seeds keys by concatenation, with the full phase
// signal code rather than a 2-char band ("GPS|L1C"), plus "<cons>|NL" and
// "<cons>|WL|<a>_<b>" under the NL/WL basis.
// Used as:         lookup key in the pivot_abs_seeds map (no parse path).
// ===========================================================================

/// Build a BSSD pivot-seed key.
inline std::string makePivotSeedKey(std::string_view cons,
                                    std::string_view band) {
    std::string out;
    out.reserve(cons.size() + 1 + band.size());
    out.append(cons);
    out.push_back(kPipeDelimiter);
    out.append(band);
    return out;
}

// ===========================================================================
// 8. Global SH-VTEC coefficient keys: "SH|<n>|<m>|<C|S>" / "SH2|<n>|<m>|<C|S>"
// ---------------------------------------------------------------------------
// Examples: "SH|0|0|C", "SH|4|2|C", "SH|4|2|S", "SH2|1|0|C"
//
//   n    : spherical-harmonic degree (0..N).
//   m    : order (0..n).
//   trig : 'C' (cosine term, includes m=0) or 'S' (sine term, m>=1).
//   layer: shell index.  Layer 0 keeps the "SH" prefix, so single-layer keys,
//          snapshots and traces written before the second layer existed
//          (card Iono-2) stay valid.  The dual-layer field's second shell uses
//          the separate prefix "SH2": parseVtecShKey rejects extra pipe fields,
//          so a 5-field layer-extended key would be unparseable by older
//          readers, and a separate prefix is the backward-compatible extension.
//
// These name the shared spherical-harmonic VTEC coefficient columns of the
// PPP-RTK network producer's co-estimated global ionosphere
// (SharedParamKind::VtecSh).  Like the pivot-seed keys (§7), they are not
// StateManager state-vector keys: the coefficients live only in
// NetworkEstimator's shared border.  They follow the same make/parse pattern
// so the estimator's column map and the driver's col_desc/trace use identical
// names.
//
// Constructed in:  the network estimator (registerVtecCoeff), the driver.
// ===========================================================================

/// Tag that opens every layer-0 SH-VTEC coefficient key.
inline constexpr std::string_view kVtecShPrefix = "SH";
/// Tag that opens every layer-1 (second-shell) SH-VTEC coefficient key.
inline constexpr std::string_view kVtecSh2Prefix = "SH2";

/// Decomposed SH-VTEC coefficient key.
struct VtecShKeyParts {
    int  n;     ///< Degree.
    int  m;     ///< Order.
    char trig;  ///< 'C' or 'S'.
    int  layer = 0;  ///< Shell index: 0 = legacy "SH" layer, 1 = "SH2" second shell.
};

/// Build a global SH-VTEC coefficient key "SH|<n>|<m>|<C|S>" (layer 0) or
/// "SH2|<n>|<m>|<C|S>" (layer 1; any @p layer > 0 selects "SH2").  Layer 0
/// output is byte-identical to the single-layer three-argument form that
/// preceded card Iono-2.
inline std::string makeVtecShKey(int n, int m, char trig, int layer = 0) {
    std::string out;
    out.append(layer > 0 ? kVtecSh2Prefix : kVtecShPrefix);
    out.push_back(kPipeDelimiter);
    out.append(std::to_string(n));
    out.push_back(kPipeDelimiter);
    out.append(std::to_string(m));
    out.push_back(kPipeDelimiter);
    out.push_back(trig);
    return out;
}

/// Cheap predicate: does @p key open with an SH-VTEC prefix ("SH" or "SH2") and
/// a pipe?  True for both layers, so layer-agnostic call sites (kind tests,
/// trace filters) work unchanged on dual-layer keys.
inline bool isVtecShKey(std::string_view key) {
    if (key.size() > kVtecSh2Prefix.size() + 1
        && key.substr(0, kVtecSh2Prefix.size()) == kVtecSh2Prefix
        && key[kVtecSh2Prefix.size()] == kPipeDelimiter)
        return true;
    return key.size() > kVtecShPrefix.size() + 1
        && key.substr(0, kVtecShPrefix.size()) == kVtecShPrefix
        && key[kVtecShPrefix.size()] == kPipeDelimiter;
}

/// Parse a global SH-VTEC coefficient key.  Returns @c std::nullopt unless the
/// key has the exact form "SH|<n>|<m>|<C|S>" (layer 0) or "SH2|<n>|<m>|<C|S>"
/// (layer 1) with non-negative integer n, m and a 'C' or 'S' trig tag.
inline std::optional<VtecShKeyParts>
parseVtecShKey(std::string_view key) {
    if (!isVtecShKey(key)) return std::nullopt;
    const bool layer2 = key.substr(0, kVtecSh2Prefix.size()) == kVtecSh2Prefix
                        && key[kVtecSh2Prefix.size()] == kPipeDelimiter;
    const auto p0 = layer2 ? kVtecSh2Prefix.size()
                           : kVtecShPrefix.size();                // pipe after the prefix
    const auto p1 = key.find(kPipeDelimiter, p0 + 1);
    if (p1 == std::string_view::npos) return std::nullopt;
    const auto p2 = key.find(kPipeDelimiter, p1 + 1);
    if (p2 == std::string_view::npos) return std::nullopt;
    if (key.find(kPipeDelimiter, p2 + 1) != std::string_view::npos)
        return std::nullopt;                                     // no extra fields
    const std::string_view n_sv = key.substr(p0 + 1, p1 - p0 - 1);
    const std::string_view m_sv = key.substr(p1 + 1, p2 - p1 - 1);
    const std::string_view t_sv = key.substr(p2 + 1);
    if (n_sv.empty() || m_sv.empty() || t_sv.size() != 1) return std::nullopt;
    if (t_sv.front() != 'C' && t_sv.front() != 'S') return std::nullopt;
    auto toInt = [](std::string_view sv, int& out) -> bool {
        int v = 0;
        for (const char c : sv) { if (c < '0' || c > '9') return false; v = v * 10 + (c - '0'); }
        out = v;
        return true;
    };
    int n = 0, m = 0;
    if (!toInt(n_sv, n) || !toInt(m_sv, m)) return std::nullopt;
    return VtecShKeyParts{n, m, t_sv.front(), layer2 ? 1 : 0};
}

}  // namespace StateKeys
}  // namespace GnssProcessor

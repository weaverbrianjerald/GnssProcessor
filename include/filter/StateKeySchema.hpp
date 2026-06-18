#pragma once

/**
 * @file StateKeySchema.hpp
 * @brief Single source of truth for every dynamic state-vector key format.
 *
 * @c StateManager owns the state vector and exposes a handful of
 * <tt>std::map<std::string, size_t></tt> indices that grow at runtime —
 * ambiguities, per-satellite ionosphere, receiver code biases (IFB / DCB),
 * per-satellite signal biases, inter-system time offsets, and (in baseline
 * mode) double-difference ambiguities.  Each map keys a piece of opaque,
 * caller-formatted text to a row of the state vector.
 *
 * Historically the format of those key strings was implicit, with key
 * construction (string concatenation) and key parsing (<tt>find()</tt> +
 * <tt>substr()</tt>) scattered across @c MeasurementModel,
 * @c StateInitializer, @c StateManager, @c AmbiguityResolver, and
 * @c DdPivotManager.  Different categories used different delimiters
 * (<tt>'_'</tt> vs <tt>'|'</tt>), and the parsers mostly had no bounds
 * checking — small format changes silently broke distant callers.
 *
 * This header centralizes all of that.  Every key category has:
 *   - A documented format string (verbatim, with example).
 *   - A @c make<i>Category</i>Key(...) builder.
 *   - A matching @c parse<i>Category</i>Key(...) helper that returns
 *     <tt>std::optional</tt> of a small "<tt>...Parts</tt>" struct, or
 *     @c std::nullopt on malformed input.
 *
 * The makers here MUST be the only producers of the on-the-wire key
 * strings stored in @c StateManager.  Code that needs to parse an
 * existing key MUST go through the parsers here; raw <tt>find('|')</tt>
 * walks belong in a museum of unhappy bug reports.
 *
 * @note The header is intentionally lightweight — every function is
 *       inline, no extra translation unit required.  Add a new category
 *       by following the pattern: format comment -> @c Parts struct ->
 *       @c make / @c parse pair -> grep call sites and migrate.
 */

#include <optional>
#include <string>
#include <string_view>

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

/// Receiver differential code bias infix in code-bias keys ("GPS_DCB").
inline constexpr std::string_view kDcbInfix = "DCB";

/// Narrow-lane infix in NlWl-basis ambiguity keys ("G01_NL_1").
inline constexpr std::string_view kNlInfix = "NL";

/// Wide-lane infix in NlWl-basis ambiguity keys ("G01_WL_1_2").
inline constexpr std::string_view kWlInfix = "WL";

// ===========================================================================
// 1. PPP ambiguity keys — "<satellite>_<signal>"
// ---------------------------------------------------------------------------
// Examples: "G01_L1C", "E11_L5Q", "G01_L1C_L5Q" (iono-free combination,
// note the embedded delimiter — parsers must split on the FIRST '_').
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

/// Parse a PPP ambiguity key.  Splits on the FIRST underscore so that
/// iono-free signal combinations remain in @c signal as-is.
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
// 1b. NL/WL ambiguity keys — "<sat>_NL_<band>" / "<sat>_WL_<band_a>_<band_b>"
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

/// Decomposed WL ambiguity key.  @c band_a < @c band_b always (canonical
/// ascending order).
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
/// Used to fast-skip non-NL entries when scanning the mixed ambiguity map.
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
// 2. Double-difference ambiguity keys — "DD|<cons>|<sat_i>|<code>|<pivot>"
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
    // No 5th pipe allowed — that would mean an unknown trailing field.
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
// 3. Per-satellite ionosphere keys — "<satellite>"
// ---------------------------------------------------------------------------
// Just the bare satellite ID, no delimiter.  Present only in PPP-RTK
// uncombined mode.
//
// Constructed in:  MeasurementModel (StateManager::addIonosphereState(sat))
// Stored in:       StateManager::ionosphere_indices_
// ===========================================================================

/// Build a per-satellite ionosphere key.  Trivial — exists for symmetry
/// and so that <tt>grep makeIonosphereKey</tt> finds every site.
inline std::string makeIonosphereKey(std::string_view satellite) {
    return std::string(satellite);
}

// ===========================================================================
// 4. Receiver code-bias keys — "<system>_IFB<band>" or "<system>_DCB"
// ---------------------------------------------------------------------------
// Examples: "GPS_IFB2", "GPS_IFB5", "GPS_DCB", "Galileo_IFB7"
//
//   system : system name string from PppUtils::getSystemString()
//            ("GPS", "Galileo", "BeiDou", "GLONASS", "QZSS", "SBAS")
//   band   : single character — the RINEX frequency-band digit, i.e.
//            obs_signal[1] (the digit between the L/C tracking-mode
//            prefix and the attribute suffix).
//   "DCB"  : differential code bias — used when a different bias
//            structure is selected by StateInitializer::resolveReceiverBias.
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
// 5. Per-satellite signal-bias keys — "<satellite>|<signal>"
// ---------------------------------------------------------------------------
// Examples: "G01|C1W", "E11|C5Q", "E24|C7Q"
//
//   satellite : full RINEX sat ID
//   signal    : full RINEX signal code (3 chars)
//
// Constructed in:  MeasurementModel, StateInitializer
// Stored in:       StateManager::sat_bias_indices_
// Parsed in:       StateManager::computeProcessNoise (BSSD freeze check
//                  reads only the satellite prefix — see
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

/// Extract just the satellite prefix from a sat-bias key.  If the key
/// has no pipe (legacy / malformed), returns the whole key — this
/// preserves the lenient behavior of the original
/// StateManager::computeProcessNoise loop.
inline std::string satelliteFromSatBiasKey(std::string_view key) {
    const auto bar = key.find(kPipeDelimiter);
    return std::string(bar != std::string_view::npos
                           ? key.substr(0, bar)
                           : key);
}

// ===========================================================================
// 5b. Per-satellite signal PHASE-bias keys — "<satellite>|<signal>|PB"
// ---------------------------------------------------------------------------
// Examples: "G01|L1C|PB", "E11|L5Q|PB"
//
// The network producer (PPP_RTK_Network) estimates an EXPLICIT per-(satellite,
// signal) satellite phase bias (Odijk Eq. 4 — it carries the network pivot-
// receiver ambiguity lump).  Stored in StateManager::sat_phase_bias_indices_, a
// map kept DISTINCT from sat_bias_indices_ (CODE bias, section 5).  The trailing
// "|PB" segment is the discriminator that keeps the two namespaces disjoint:
//   - satelliteFromSatBiasKey / satelliteFromSatPhaseBiasKey both split on the
//     FIRST pipe, so either returns the satellite correctly;
//   - a phase-bias key is NEVER stored in sat_bias_indices_, so parseSatBiasKey
//     (which would otherwise read "L1C|PB" as the signal) is never invoked on it.
//
// Constructed in:  MeasurementModel, the network estimator
// Stored in:       StateManager::sat_phase_bias_indices_
// Parsed in:       StateManager::getProcessNoiseMatrix (pivot-freeze check reads
//                  only the satellite prefix — see satelliteFromSatPhaseBiasKey).
// ===========================================================================

/// Trailing discriminator separating a phase-bias key from a code-bias key.
inline constexpr std::string_view kSatPhaseBiasSuffix = "|PB";

/// Build a per-satellite signal PHASE-bias key.
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

/// Parse a per-satellite signal PHASE-bias key.  Returns @c std::nullopt unless
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
// 6. System-time-offset keys — "<system>"
// ---------------------------------------------------------------------------
// Just the system name string ("Galileo", "GLONASS", "BeiDou", "QZSS",
// "SBAS").  GPS is the reference and never appears.
//
// Constructed in:  StateManager constructor (loop over active systems)
// Stored in:       StateManager::system_time_offset_indices_
// ===========================================================================

/// Build a system-time-offset key.  Trivial — exists for symmetry.
inline std::string makeSystemTimeOffsetKey(std::string_view system) {
    return std::string(system);
}

// ===========================================================================
// 7. BSSD pivot-seed keys — "<cons>|<band>"
// ---------------------------------------------------------------------------
// Examples: "GPS|L1", "Galileo|E5", "BeiDou|B2"
//
//   cons : constellation string (currently the same string used in DD
//          ambiguity keys)
//   band : 2-char band slice of the RINEX signal code (signal.substr(0, 2))
//
// Constructed in:  MeasurementModel (BSSD apply path)
// Used as:         lookup key in pivot_abs_seeds map (no parse path).
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

}  // namespace StateKeys
}  // namespace GnssProcessor

#pragma once
/**
 * @file NetworkOraclePinScope.hpp
 * @brief The `oracle_pin` SCOPE grammar — which arcs a sim-only oracle
 *        ambiguity pin holds at truth.
 *
 * A LEAF header (no project includes) deliberately split out of
 * filter/NetworkOraclePin.hpp: the JSON loader
 * (src/core/config/GnssConfigLoadNetwork.cpp) must refuse a bad scope BY NAME at
 * load time, and it must do so through the SAME parser the run-scope seam uses,
 * or the grammar would live in two places and drift.  The seam's own header
 * pulls filter/NetworkRunTypes.hpp (StateManager, NetworkEstimator, PppModel,
 * MeasurementModel), which the config TU has no business including.
 *
 * See include/filter/NetworkOraclePin.hpp for the card.
 */

#include <set>
#include <string>

namespace GnssProcessor {

/**
 * @brief One parsed `scope` string of an `oracle_pin` event.
 *
 * Grammar (rejected BY NAME at load, never silently widened or emptied):
 *   - `all`                       — every carried non-anchor arc;
 *   - `band:<sig>[+<sig>…]`       — e.g. `band:L1C+L2W`;
 *   - `station:<name>`            — one roster station;
 *   - `sat:<id>`                  — one satellite, e.g. `sat:G08`;
 *   - `arc:<station>/<sat>/<sig>` — one exact arc.
 */
struct OraclePinScope {
    /// Which form this scope is.  @c Invalid never leaves @ref oraclePinParseScope
    /// on success — it is the parse-failure marker.
    enum class Kind { Invalid, All, Band, Station, Sat, Arc };
    Kind                  kind{Kind::Invalid};
    std::set<std::string> signals;   ///< @c Band: the accepted RINEX-3 phase codes.
    std::string           station;   ///< @c Station / @c Arc.
    std::string           sat;       ///< @c Sat / @c Arc.
    std::string           signal;    ///< @c Arc.

    /**
     * @brief Does this scope cover arc (@p sta_name, @p sat_id, @p sig)?
     * @return true when the arc is in scope; false for @c Kind::Invalid.
     */
    bool covers(const std::string& sta_name, const std::string& sat_id,
                const std::string& sig) const;
};

/**
 * @brief Parse one `scope` string.
 *
 * @param s         The raw scope text from the event.
 * @param[out] why  On failure, a one-sentence reason naming the offending text
 *                  (the loader prints it verbatim); untouched on success.
 * @return The parsed scope; @c kind == @c OraclePinScope::Kind::Invalid on failure.
 */
OraclePinScope oraclePinParseScope(const std::string& s, std::string& why);

} // namespace GnssProcessor

#pragma once
/**
 * @file OraclePinScope.hpp
 * @brief The `oracle_pin` scope grammar: which arcs a sim-only oracle ambiguity
 *        pin holds at truth.
 *
 * A leaf header (no project includes), split out of network/slip/OraclePin.hpp
 * on purpose.  The JSON loader (src/core/config/GnssConfigLoadNetwork.cpp) refuses
 * a bad scope by name at load time, and does so through the same parser the
 * run-scope seam uses, so the grammar exists in one place.  The seam's own header
 * includes network/driver/RunTypes.hpp (StateManager, NetworkEstimator, PppModel,
 * MeasurementModel), which the config TU should not depend on.
 *
 * The oracle-pin card is described in include/network/slip/OraclePin.hpp.
 */

#include <set>
#include <string>

namespace GnssProcessor {

/**
 * @brief One parsed `scope` string of an `oracle_pin` event.
 *
 * Grammar (an invalid scope is rejected by name at load, never silently widened
 * or emptied):
 *   - `all`: every carried arc;
 *   - `band:<sig>[+<sig>…]`: phase signal codes, each starting with `L`,
 *     e.g. `band:L1C+L2W`;
 *   - `station:<name>`: one roster station;
 *   - `sat:<id>`: one satellite, e.g. `sat:G08`;
 *   - `arc:<station>/<sat>/<sig>`: one exact arc.
 *
 * Under every scope, a family's anchor arc is never pinned (its datum pin defines
 * the family's gauge); src/core/network/slip/OraclePin.cpp applies that exclusion
 * after covers().
 */
struct OraclePinScope {
    /// Which form this scope is.  @c Invalid is the parse-failure marker and is
    /// never returned by @ref oraclePinParseScope on success.
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

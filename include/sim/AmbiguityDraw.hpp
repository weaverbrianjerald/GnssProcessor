#pragma once

#include <cstdint>
#include <string>

/**
 * @file AmbiguityDraw.hpp
 * @brief The simulator's integer carrier-ambiguity draw and its two regimes.
 *
 * The draw is a free function (called from @c runSynth, src/sim/main.cpp) so
 * that it is unit-testable on its own and not only through a full network
 * synthesis.  The regime it implements is a fixture property that changes what
 * an estimator experiment can conclude.
 *
 * ## The two regimes
 *
 * @c AmbiguityRegime::Legacy draws @f$N \in [10^5, 10^6)@f$.  It is the default,
 * so every existing fixture regenerates byte-identically.
 *
 * @c AmbiguityRegime::Field draws @f$N \in [-W, +W]@f$ with @f$W@f$ =
 * @ref AmbiguityDrawConfig::half_width_cycles.
 *
 * ## Why the field regime exists (measured)
 *
 * A geodetic receiver aligns its carrier-phase register to the pseudorange at
 * lock, so the raw ambiguity is small: of order @f$10^1@f$–@f$10^2@f$ cycles, not
 * @f$10^5@f$.  Measured on eight bundled IGS stations (DOY-129 2024, 1 h arcs,
 * Melbourne-Wubbena, so the estimate is iono-free and code noise averages down):
 *
 * | quantity                        | field (IGS)   | legacy sim | ratio    |
 * |---------------------------------|---------------|------------|----------|
 * | median @f$\|N_{WL}\|@f$         | 28.5 cyc      | 268 127    | 9 400x   |
 * | median @f$\|\nabla\Delta N\|@f$ | 29.7 cyc      | 358 244    | 12 100x  |
 *
 * The four-decade difference is not cosmetic: it decides whether a cold start
 * that linearizes about zero begins inside or outside its capture region.  On
 * the 60-station bisection ladder every eq34 arm lost to plain float under the
 * legacy regime while the matched field-regime arms passed structurally, so the
 * legacy fixture cannot grade the datum stack it was built for.
 *
 * @c half_width_cycles = 48 (the default) reproduces the measured field spread
 * closely under an i.i.d. model.  Predicted medians against the field column
 * above: zero-difference 24, wide-lane 29, double-difference 38 cycles.
 *
 * ## Properties the draw preserves in either regime
 *
 * Two structural properties are load-bearing and are easily lost by simplifying
 * this function; tests in @c tests/sim/test_sim_ambiguity_regime.cpp pin both.
 *
 * 1. **The station term (@p seed_salt) is load-bearing.**  Without it every
 *    station in a network receives numerically identical integers, so every
 *    between-station single and double difference is exactly zero: ambiguity
 *    resolution then reports a 100 % fix rate on perfect integers while
 *    measuring nothing.  Salt 0 (the single-station default) reproduces the
 *    pre-regime hash integers exactly.
 * 2. **Per-band draws are independent.**  The pre-2026 formula
 *    @c (E?700000:100000)+prn*1000+band_idx*137 made the wide-lane @f$N_1-N_2@f$
 *    the constant -137 on every satellite, so one Melbourne-Wubbena solve
 *    generalized to the whole constellation and removed the per-satellite work
 *    that makes wide-lane resolution a real exercise.
 *
 * The hash is a self-contained splitmix64 finalizer, not a draw from a shared
 * RNG stream, so changing the regime cannot shift the noise, multipath or slip
 * realizations: an A/B over the regime moves the ambiguities and nothing else.
 */

namespace GnssProcessor {
namespace sim {

/** @brief Which magnitude regime the integer ambiguities are drawn from. */
enum class AmbiguityRegime {
    Legacy, ///< @f$N \in [10^5, 10^6)@f$; the default, kept for fixture byte-identity.
    Field   ///< @f$N \in [-W, +W]@f$; matches a pseudorange-aligned phase register.
};

/** @brief Resolved ambiguity-draw knobs (see the file banner for the rationale). */
struct AmbiguityDrawConfig {
    AmbiguityRegime regime{ AmbiguityRegime::Legacy };
    /// Half-width @f$W@f$ of the @c Field regime, in cycles.  Ignored under
    /// @c Legacy, where the loader rejects an explicit value.  Default 48 is calibrated
    /// against the measured IGS spread tabulated in the file banner.
    int half_width_cycles{ 48 };
};

/**
 * @brief splitmix64 over (system letter, PRN, band index, station salt).
 *
 * Exposed separately from @ref drawAmbiguity so a test can assert the avalanche
 * and the salt dependence without also pinning the range mapping.
 *
 * @param sat       Satellite ID, e.g. "G05"; only @c sat[0] (the system letter)
 *                  enters the hash; the PRN is passed in @p prn.  Must be non-empty.
 * @param prn       Satellite PRN.
 * @param band_idx  Index into the run's band plan.  It is positional, not a
 *                  frequency identity: the same phase code can carry a different
 *                  index across systems, which is harmless because @c sat[0] is
 *                  hashed too.
 * @param seed_salt Per-station salt; 0 is the single-station legacy value and
 *                  makes its term vanish.
 * @return The finalized 64-bit hash.
 */
inline std::uint64_t ambiguityHash(const std::string& sat, int prn, int band_idx,
                                   std::uint64_t seed_salt) {
    std::uint64_t h = 0x9E3779B97F4A7C15ull
        ^ (static_cast<std::uint64_t>(static_cast<unsigned char>(sat.empty() ? 0 : sat[0])) << 56)
        ^ (static_cast<std::uint64_t>(prn) << 32)
        ^ (static_cast<std::uint64_t>(band_idx) * 0xD1B54A32D192ED03ull)
        ^ (seed_salt * 0xA24BAED4963EE407ull);
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27; h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return h;
}

/**
 * @brief Draw the arc-constant integer ambiguity for one (station, satellite, band).
 *
 * Deterministic: the same arguments always return the same integer, within and
 * across processes (the network driver synthesizes each station in its own
 * child process, so this is a hard requirement, not a convenience).
 *
 * @param cfg       Regime + half-width.
 * @param sat       Satellite ID, e.g. "G05".
 * @param prn       Satellite PRN.
 * @param band_idx  Index into the run's band plan.
 * @param seed_salt Per-station salt (0 = single-station legacy).
 * @return The integer ambiguity in cycles, in @f$[10^5,10^6)@f$ under @c Legacy
 *         or @f$[-W,+W]@f$ under @c Field.
 */
inline int drawAmbiguity(const AmbiguityDrawConfig& cfg, const std::string& sat, int prn,
                         int band_idx, std::uint64_t seed_salt) {
    const std::uint64_t h = ambiguityHash(sat, prn, band_idx, seed_salt);
    if (cfg.regime == AmbiguityRegime::Field) {
        // Uniform over the 2W+1 values [-W, +W].  The modulus is taken on the
        // finalized hash as the legacy path does, so the two regimes share one
        // entropy source and differ in the range mapping alone.
        const std::uint64_t span = 2ull * static_cast<std::uint64_t>(cfg.half_width_cycles) + 1ull;
        return static_cast<int>(static_cast<std::int64_t>(h % span)
                                - static_cast<std::int64_t>(cfg.half_width_cycles));
    }
    return static_cast<int>(100000ull + h % 900000ull);
}

} // namespace sim
} // namespace GnssProcessor

#pragma once

#include <cstdint>
#include <string>

/**
 * @file AmbiguityDraw.hpp
 * @brief The simulator's integer carrier-ambiguity draw, and its two REGIMES.
 *
 * The draw used to be a lambda inside @c runSynth (src/sim/main.cpp).  It is a
 * free function here for one reason: the regime it implements is a *fixture
 * property that changes what an estimator experiment can conclude*, so it needs
 * to be unit-testable on its own rather than only through an 85-minute
 * network synthesis.
 *
 * ## The two regimes
 *
 * @c AmbiguityRegime::Legacy draws @f$N \in [10^5, 10^6)@f$.  That is the
 * historical behaviour and remains the DEFAULT so every existing fixture
 * regenerates byte-identically.
 *
 * @c AmbiguityRegime::Field draws @f$N \in [-W, +W]@f$ with @f$W@f$ =
 * @ref AmbiguityDrawConfig::half_width_cycles.
 *
 * ## Why the field regime exists (measured, not assumed)
 *
 * A geodetic receiver aligns its carrier-phase register to the pseudorange at
 * lock, so the raw ambiguity is small — order @f$10^1@f$–@f$10^2@f$ cycles, not
 * @f$10^5@f$.  Measured on eight bundled IGS stations (DOY-129 2024, 1 h arcs,
 * Melbourne-Wubbena so the estimate is iono-free and code noise averages down):
 *
 * | quantity                        | field (IGS)   | legacy sim | ratio    |
 * |---------------------------------|---------------|------------|----------|
 * | median @f$\|N_{WL}\|@f$         | 28.5 cyc      | 268 127    | 9 400x   |
 * | median @f$\|\nabla\Delta N\|@f$ | 29.7 cyc      | 358 244    | 12 100x  |
 *
 * Four decades matters because it is not a cosmetic scale difference: it
 * decides whether a cold start that linearizes about zero begins inside or
 * outside its capture region.  On the 60-station bisection ladder every eq34
 * arm lost to plain float by 34-852x while the matched FIELD arms passed
 * structurally, i.e. the field is outside the sim's catastrophic regime and the
 * fixture could not grade the very datum stack it was built for.
 *
 * @c half_width_cycles = 48 (the default) reproduces the measured field spread
 * closely under an i.i.d. model.  Predicted medians vs. the field column above:
 * zero-difference 24, wide-lane 29, double-difference 38 cycles.
 *
 * ## What the draw must preserve, in EITHER regime
 *
 * Two structural properties are load-bearing and easy to destroy by "simplifying"
 * this function; both are pinned by tests in @c tests/sim/test_sim_ambiguity_regime.cpp.
 *
 * 1. **The station term (@p seed_salt) is load-bearing.**  Without it every
 *    station in a network receives numerically IDENTICAL integers, so every
 *    between-station single- and double-difference is EXACTLY ZERO: ambiguity
 *    resolution reports a 100 % fix rate on perfect integers while measuring
 *    nothing.  Salt 0 (the single-station default) reproduces the historical
 *    integers exactly.
 * 2. **Per-band draws are independent.**  The pre-2026 formula
 *    @c (E?700000:100000)+prn*1000+band_idx*137 made the wide-lane @f$N_1-N_2@f$
 *    the constant -137 on EVERY satellite, so one Melbourne-Wubbena solve
 *    generalized to the whole constellation and the per-satellite work that
 *    makes wide-lane resolution a real exercise was absent.
 *
 * The hash is a SELF-CONTAINED splitmix64 finalizer, not a draw from a shared
 * RNG stream, so changing the regime cannot shift the noise / multipath / slip
 * realizations — an A/B over the regime moves the ambiguities and nothing else.
 */

namespace GnssProcessor {
namespace sim {

/** @brief Which magnitude regime the integer ambiguities are drawn from. */
enum class AmbiguityRegime {
    Legacy, ///< @f$N \in [10^5, 10^6)@f$ — historical; the DEFAULT (byte-identity).
    Field   ///< @f$N \in [-W, +W]@f$ — matches a pseudorange-aligned phase register.
};

/** @brief Resolved ambiguity-draw knobs (see the file banner for the rationale). */
struct AmbiguityDrawConfig {
    AmbiguityRegime regime{ AmbiguityRegime::Legacy };
    /// Half-width @f$W@f$ of the FIELD regime, in cycles.  Ignored (and rejected
    /// by the loader if explicitly set) under @c Legacy.  Default 48 is calibrated
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
 *                  enters the hash — the PRN travels in @p prn.  Must be non-empty.
 * @param prn       Satellite PRN.
 * @param band_idx  Index into the run's band plan (positional, NOT a frequency
 *                  identity — the same phase code can carry a different index
 *                  across systems, which is harmless only because @c sat[0] is
 *                  hashed too).
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
        // finalized hash exactly as the legacy path does, so the two regimes share
        // one entropy source and differ ONLY in the range mapping.
        const std::uint64_t span = 2ull * static_cast<std::uint64_t>(cfg.half_width_cycles) + 1ull;
        return static_cast<int>(static_cast<std::int64_t>(h % span)
                                - static_cast<std::int64_t>(cfg.half_width_cycles));
    }
    return static_cast<int>(100000ull + h % 900000ull);
}

} // namespace sim
} // namespace GnssProcessor

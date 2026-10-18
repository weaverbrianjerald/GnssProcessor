#pragma once
/**
 * @file Seeds.hpp
 * @brief The values a state is born at, as pure functions of the observation equation: the
 *        residual a seed is formed from, and the seeds of the inter-system biases, the cold
 *        receiver clock, the phase clock, the receiver code biases, the slant ionosphere and the
 *        ambiguities.
 *
 * ### One equation, two masks
 *
 * A seed is the observation less the terms that are already known when the state is born.
 * Before this file the equation was written out twice for that purpose
 * (@c StateInitializer::computeRawResidual and @c StateManager::computeAmbiguitySeed), each
 * leaving out different terms.  Both now read the term list of
 * measurement/ObservationEquation.hpp, built with the effects of a @ref SeedMask:
 *
 * | effect              | residual (ISB, clock, phase clock, code bias) | ambiguity |
 * |---------------------|-----------------------------------------------|-----------|
 * | modeled value       | yes                                           | yes       |
 * | receiver clock      | yes                                           | yes (summed with the phase clock first) |
 * | phase clock         | no                                            | yes       |
 * | wet troposphere     | yes                                           | yes       |
 * | inter-system bias   | yes                                           | yes       |
 * | gradients           | yes                                           | yes       |
 * | slant ionosphere    | yes                                           | yes (phase signals) |
 * | receiver code bias  | yes                                           | no        |
 * | satellite code bias | yes                                           | no        |
 * | range, satellite phase bias, satellite clock, SSR bias, phase colour, ambiguity | no | no |
 *
 * @ref kLegacySeedMask is exactly this table; the omitted terms are zero for a network
 * producer's birth (shared states at 0, no SSR term, range correction 0) and non-zero for a
 * consumer's.  The terms are subtracted in the order of @ref kSeedOrder, which is the order the
 * two copies subtracted them in (the inter-system bias before the gradients, not after, as in
 * the equation's sum), so that the numbers are the same bit for bit.
 *
 * ### Pure
 *
 * The functions read the observations and a @ref ParamLookup and return values; they write no
 * state and log nothing.  The single-receiver filter applies the values to its @c StateManager
 * in its own call order (MeasurementModelSeeds.cpp); the network will apply them to its own
 * parameter set.  A seed for a parameter that does not exist in the lookup is not returned.
 *
 * ### The carrier frequency (open)
 *
 * The slant-ionosphere term scales by a carrier frequency, and the three places that did so
 * disagreed on GLONASS FDMA bands only (every other band is the same number, bit for bit; see
 * tests/test_seeds.cpp).  Seeds keep the number each copy used: the residuals take the
 * frequency helper of @c StateInitializer (whose GLONASS channels are the default table, never
 * the receiver's), the ambiguity seed takes channel 0.  @ref residualSeedInput and
 * @ref ambiguitySeedInput of @c MeasurementModel build those two inputs; one rule for both is
 * the open difference of the full-equation seed.
 *
 * Plain value types and free functions.
 */

#include "measurement/ObservationEquation.hpp"

#include <Eigen/Dense>

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace GnssProcessor {

class StateManager;

// =======================================================================
// The masks
// =======================================================================

/**
 * @brief Which terms of the equation a seed subtracts from the observation.
 */
struct SeedMask {
    ObsEffects effects;        ///< The terms the seed knows.
    bool       group_clocks;   ///< Add the phase clock to the receiver clock first, then subtract the sum.
};

/**
 * @brief The two forms of a seed.
 */
struct SeedMasks {
    SeedMask residual;   ///< For the inter-system bias, the cold clock, the phase clock and the receiver code biases.
    SeedMask ambiguity;  ///< For the ambiguities.
};

/**
 * @brief The terms the two seed copies subtracted before the equation was shared.
 */
inline constexpr SeedMasks kLegacySeedMask{
    SeedMask{effectsOf({ObsEffect::Modeled, ObsEffect::ReceiverClock, ObsEffect::Troposphere,
                        ObsEffect::SystemTimeOffset, ObsEffect::TropoGradient,
                        ObsEffect::Ionosphere, ObsEffect::ReceiverCodeBias,
                        ObsEffect::SatelliteCodeBias}),
             /*group_clocks=*/false},
    SeedMask{effectsOf({ObsEffect::Modeled, ObsEffect::ReceiverClock, ObsEffect::PhaseClock,
                        ObsEffect::Troposphere, ObsEffect::SystemTimeOffset,
                        ObsEffect::TropoGradient, ObsEffect::Ionosphere}),
             /*group_clocks=*/true},
};

/**
 * @brief The effects a seed can subtract, in the order it subtracts them.
 *
 * The receiver clock stands for the clock group when @ref SeedMask::group_clocks is set.  An
 * effect outside this list is never subtracted, whatever a mask says.
 */
inline constexpr std::array<ObsEffect, 8> kSeedOrder{
    ObsEffect::Modeled,          ObsEffect::ReceiverClock,    ObsEffect::Troposphere,
    ObsEffect::SystemTimeOffset, ObsEffect::TropoGradient,    ObsEffect::Ionosphere,
    ObsEffect::ReceiverCodeBias, ObsEffect::SatelliteCodeBias,
};

// =======================================================================
// The residual
// =======================================================================

/**
 * @brief The observation less the masked terms of its equation.
 *
 * The terms are subtracted left to right in @ref kSeedOrder.  A term the equation does not hold
 * (its parameter does not exist) is skipped, which is the same as subtracting zero.
 *
 * @param eq       The row's equation (usable), built with at least the effects of @p mask.
 * @param mask     The terms to subtract.
 * @param observed The observed value of the signal (m).
 * @return The residual (m).
 */
double seedResidual(const ObservationEquation& eq, const SeedMask& mask, double observed);

/**
 * @brief The raw residual of one row: the observation less the terms the residual seeds know.
 *
 * Builds the equation of @p ref_type with the residual mask and subtracts it.
 *
 * @param in       Equation inputs; use the residual input (@c MeasurementModel::residualSeedInput).
 * @param params   The parameters the terms are read from.
 * @param obs      The satellite's modeled observation.
 * @param ref_type A code or phase signal with an observed and a modeled value.
 * @return The residual (m); nothing when the row has no equation (a satellite text that is not
 *         a canonical RINEX id, or a signal that is neither code nor phase).
 */
std::optional<double> rawResidual(const ObsInput& in, const ParamLookup& params,
                                  const PppObservation& obs, const std::string& ref_type);

// =======================================================================
// The ambiguity
// =======================================================================

/**
 * @brief The float ambiguity a phase signal is born at (m): its observation less the terms the
 *        ambiguity seed knows.
 *
 * The slant ionosphere is subtracted from phase signals only.  The seed is absolute (an
 * undifferenced ambiguity); the caller makes it relative to a pivot or divides it by a
 * wavelength.
 *
 * @param in       Equation inputs; use the ambiguity input (@c MeasurementModel::ambiguitySeedInput).
 * @param params   The parameters the terms are read from.
 * @param obs      The satellite's modeled observation.
 * @param ref_type The signal.
 * @return The seed (m); 0 when the signal has no observed or modeled value or no equation.
 */
double seedAmbiguity(const ObsInput& in, const ParamLookup& params, const PppObservation& obs,
                     const std::string& ref_type);

/**
 * @brief The ambiguity seed over the single-receiver state.
 *
 * @ref seedAmbiguity with the lookup of a @c StateManager evaluated at @p x.
 *
 * @param in       Equation inputs (the ambiguity input).
 * @param states   The state layout.
 * @param x        The state vector to evaluate at.
 * @param obs      The satellite's modeled observation.
 * @param ref_type The signal.
 * @return The seed (m).
 */
double seedAmbiguityOfState(const ObsInput& in, const StateManager& states,
                            const Eigen::VectorXd& x, const PppObservation& obs,
                            const std::string& ref_type);

// =======================================================================
// The slant ionosphere
// =======================================================================

/**
 * @brief A slant ionosphere seed with the pair of codes it came from.
 */
struct IonosphereSeed {
    double      value_m{0.0};   ///< The seed: slant delay on GPS L1 (m).
    std::string code_a;         ///< The first code of the pair.
    std::string code_b;         ///< The second code of the pair.
    double      range_a_m{0.0}; ///< Observed minus modeled of the first code (m).
    double      range_b_m{0.0}; ///< Observed minus modeled of the second code (m).
    double      alpha_a{0.0};   ///< Squared frequency ratio of the first code against GPS L1.
    double      alpha_b{0.0};   ///< Squared frequency ratio of the second code against GPS L1.
};

/**
 * @brief The slant ionosphere of a satellite from its dual-frequency code.
 *
 * Only code signals with a .BIA entry (applied, when the configuration requires it) are used:
 * for an uncovered signal the observed minus modeled carries the satellite's raw code bias,
 * which would put tens of metres into the seed.  The first pair of distinct bands whose seed is
 * finite and below 200 m in magnitude is returned.  Reads no state.
 *
 * @param in  Equation inputs (the residual input: its frequency helper scales the codes).
 * @param obs The satellite's modeled observation.
 * @return The seed; nothing when no covered pair of distinct bands gives one (the caller seeds
 *         0 with an inflated variance).
 */
std::optional<IonosphereSeed> seedIonosphere(const ObsInput& in, const PppObservation& obs);

// =======================================================================
// The inter-system biases
// =======================================================================

/// The seed of one constellation's inter-system bias.
struct IsbSeed {
    System      sys{System::Unknown};  ///< The constellation.
    double      value_m{0.0};          ///< Its median code residual less the time reference's (m).
    std::size_t n_obs{0};              ///< Satellites in the median.
};

/// The inter-system bias seeds of one epoch.
struct IsbSeeds {
    bool                 settled{false};    ///< False when no time-reference code observation was available (retry next epoch).
    std::size_t          reference_obs{0};  ///< Satellites of the time reference in its median.
    std::vector<IsbSeed> seeds;             ///< One per other constellation whose bias exists, by constellation name.
};

/**
 * @brief The inter-system bias of each constellation from the first epoch's code.
 *
 * One code residual per satellite above the elevation mask; the median per constellation less
 * the time reference's median.  Only constellations whose bias parameter exists are returned.
 *
 * @param in           Equation inputs (the residual input).
 * @param params       The parameters the residuals are formed against.
 * @param observations The epoch's modeled observations.
 * @return The seeds; @c settled is false when the time reference has no code observation.
 */
IsbSeeds seedIsb(const ObsInput& in, const ParamLookup& params,
                 const std::vector<PppObservation>& observations);

// =======================================================================
// The cold receiver clock
// =======================================================================

/// How a cold-clock seed ended.
enum class ColdClockOutcome : std::uint8_t {
    NoClockState,   ///< The configuration has no receiver clock parameter: nothing to seed.
    AlreadySeeded,  ///< The clock is not cold (more than a micrometre from zero): left alone.
    NoReference,    ///< Cold, but no time-reference code observation this epoch: retry next epoch.
    Seeded,         ///< Cold and seeded.
};

/// The seed of a cold receiver clock.
struct ColdClockSeed {
    ColdClockOutcome outcome{ColdClockOutcome::NoClockState};  ///< How it ended.
    double           current_m{0.0};  ///< The clock's current value (m); read for AlreadySeeded.
    double           value_m{0.0};    ///< The seed (m); read for Seeded.
    std::size_t      n_obs{0};        ///< Time-reference satellites in the median; read for Seeded.

    /// @return False only for NoReference: the caller latches the seed otherwise.
    bool settled() const { return outcome != ColdClockOutcome::NoReference; }
};

/**
 * @brief The receiver clock from the time reference's code, while the clock is cold.
 *
 * The median code residual of the time reference's satellites, one per satellite above the
 * elevation mask; it is the time-reference clock because the inter-system biases carry the
 * other constellations' offsets.
 *
 * @param in           Equation inputs (the residual input).
 * @param params       The parameters the residuals are formed against.
 * @param observations The epoch's modeled observations.
 * @return The outcome and, for @c Seeded, the value.
 */
ColdClockSeed seedColdClock(const ObsInput& in, const ParamLookup& params,
                            const std::vector<PppObservation>& observations);

// =======================================================================
// The phase clock
// =======================================================================

/// The seed of the receiver phase-clock offset.
struct PhaseClockSeed {
    bool        found{false};   ///< False when no satellite has both a code and a phase residual.
    double      value_m{0.0};   ///< The median of code residual minus phase residual (m).
    std::size_t n_pairs{0};     ///< Satellites in the median.
};

/**
 * @brief The phase-clock offset from the code-minus-phase residual difference.
 * @param in           Equation inputs (the residual input).
 * @param params       The parameters the residuals are formed against.
 * @param observations The epoch's modeled observations.
 * @return The seed.
 */
PhaseClockSeed seedPhaseClock(const ObsInput& in, const ParamLookup& params,
                              const std::vector<PppObservation>& observations);

// =======================================================================
// The receiver code biases
// =======================================================================

/// The seed of one receiver code-bias parameter.
struct CodeBiasSeed {
    RxCbKey     key{};              ///< The parameter.
    double      value_m{0.0};       ///< The seed (m).
    std::size_t n_sats{0};          ///< Satellites in the median (0 for a restored value).
    bool        restored{false};    ///< The value is the installed linearization point, not a median.
};

/// The receiver code-bias seeds of the first epoch.
struct CodeBiasSeeds {
    std::vector<CodeBiasSeed> seeds;     ///< One per parameter that exists, in key order.
    std::size_t               refused{0};///< Keys the restore map covers whose installed value could not be used.
};

/**
 * @brief The receiver DCB and IFB parameters from the cross-band code residual differences.
 *
 * For every satellite with codes on two or more bands, the residual of each band less the
 * first clock-reference band's, less the slant ionosphere's frequency-dependent part, over the
 * difference of the receiver-bias coefficients; the median over satellites per parameter,
 * estimates above 200 m refused.  A key the @p restore map covers takes the map's value when it
 * is finite (the warm resume's linearization point) and no median is formed for it.  Only
 * parameters that exist are returned.  Reads nothing when the configuration does not estimate
 * receiver biases in uncombined mode.
 *
 * @param in           Equation inputs (the residual input).
 * @param params       The parameters the residuals are formed against.
 * @param observations The epoch's modeled observations.
 * @param restore      The installed linearization points by key text (empty: none).
 * @return The seeds.
 */
CodeBiasSeeds seedCodeBias(const ObsInput& in, const ParamLookup& params,
                           const std::vector<PppObservation>& observations,
                           const std::map<std::string, double>& restore);

} // namespace GnssProcessor

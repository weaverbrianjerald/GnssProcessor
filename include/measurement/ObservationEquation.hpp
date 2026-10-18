#pragma once
/**
 * @file ObservationEquation.hpp
 * @brief The observation equation of a code or phase row as a list of terms: what the
 *        predicted observable adds up, which parameter each term reads, and with which partial.
 *
 * ### The equation
 *
 * A row's predicted observable is the a-priori model value plus one term per estimated or
 * applied quantity:
 *
 *     predicted = modeled + range correction + receiver clock + phase clock + wet troposphere
 *               + troposphere gradients + inter-system bias + slant ionosphere
 *               + receiver code bias + satellite code bias + satellite phase bias
 *               + satellite clock + SSR bias (+ ambiguity (+ phase colour)).
 *
 * @ref observationEquation returns that sum as a list of @ref ObsAddend, in the order of the
 * sum.  A term that reads parameters carries, for each, the identity (@ref ParamRef), the
 * partial derivative, and the column and value the caller's @ref ParamLookup answered with.
 * A term that reads none (the modeled value, the SSR bias) carries its value.  The same list
 * serves every consumer of the row:
 *  - @ref evaluate adds the terms up and returns the predicted observable;
 *  - @ref scatterDense writes the partials into a dense Jacobian row.
 *
 * ### Bit-exact summation
 *
 * The filter's numbers depend on the order of the floating-point operations.  The list keeps
 * the grouping of the sum it replaces: the terms are added left to right in the order above
 * (a term that is absent is absent from the list, which is the same as adding zero); a term
 * with a factor keeps the factor (@ref ObsAddend::scale) outside the sum of its parts, as
 * @c mf_grad * (cos * x_n + sin * x_e) and @c lambda * (x_nl - s * x_wl) are written; the
 * satellite phase bias and the receiver phase bias are one term of two parts; and the range
 * correction is evaluated from its line of sight and linearization point as the dot product
 * the filter has always used.  The Jacobian entry of a part is @c scale * @c partial.
 *
 * ### Who answers for the parameters
 *
 * The caller owns the parameters.  It passes a @ref ParamLookup, which answers for one
 * @ref ParamRef with the parameter's column and linearization value, or with a negative
 * column when the parameter does not exist.  The single-receiver filter answers from its
 * state vector (measurement/UserParamView.hpp).  A term whose parameter does not exist is
 * left out of the list, as the row has always left it out.
 *
 * ### Signs between the producer and the consumer
 *
 * The network producer estimates the satellite biases and clocks as states; the closed-loop
 * consumer takes the same quantities as known terms.  The two agree only if the partial of the
 * producer's state and the multiplier of the consumer's known term are the same number.  They
 * are one constant each, @ref kSatBiasSign and @ref kSatClockPartial, used by both sides.
 *
 * Doppler rows are not part of this equation (velocity, clock rate and the IMU states are
 * columns only a single-receiver configuration has).  Plain value types and free functions.
 */

#include "common/ParamRef.hpp"
#include "common/PppObservation.hpp"
#include "measurement/RowRules.hpp"

#include <Eigen/Dense>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <string>

namespace GnssProcessor {

class PppUtils;
class StateInitializer;

// =======================================================================
// Signs shared by the producer and the consumer
// =======================================================================

/**
 * @brief The sign of a satellite signal bias in the observation: the partial of the
 *        producer's satellite code-bias and phase-bias states, and the multiplier of the
 *        consumer's known SSR bias term.
 *
 * The decoded bias is the satellite signal bias present in the observation, so it is added
 * to the clean model.  This convention can only be confirmed empirically against the
 * provider's stream: float ambiguity fractions that cluster near 0.5 instead of 0 mean the
 * constant must be flipped (and the code rebuilt).  Code and phase share it.
 */
inline constexpr double kSatBiasSign = +1.0;

/**
 * @brief The partial of the satellite clock state on every row it appears in.
 *
 * The observable carries the satellite clock as minus c times its offset, and the modeled
 * value has already removed the a-priori product clock, so the residual-clock state enters
 * with -1 on code and phase rows.  The consumer takes the clock inside the modeled value.
 */
inline constexpr double kSatClockPartial = -1.0;

// =======================================================================
// The term list
// =======================================================================

/**
 * @brief The effects of the equation, in the order of the sum.
 */
enum class ObsEffect : std::uint8_t {
    Modeled = 0,        ///< The a-priori model value (no parameter).
    RangeCorrection,    ///< Minus the line of sight dotted with the position's offset from the linearization point.
    ReceiverClock,      ///< The receiver clock (+1).
    PhaseClock,         ///< The receiver phase-clock offset (+1, phase rows).
    Troposphere,        ///< The zenith wet delay times the wet mapping function.
    TropoGradient,      ///< The troposphere gradients times the gradient mapping function.
    SystemTimeOffset,   ///< The inter-system bias of a non-reference constellation (+1).
    Ionosphere,         ///< The slant ionosphere (+alpha on code, -alpha on phase).
    ReceiverCodeBias,   ///< The receiver code bias (code rows; the resolver's coefficient).
    SatelliteCodeBias,  ///< The satellite code bias (code rows; @ref kSatBiasSign).
    SatellitePhaseBias, ///< The satellite phase bias, with the receiver phase bias (phase rows).
    SatelliteClock,     ///< The satellite clock (@ref kSatClockPartial).
    SsrBias,            ///< The SSR satellite bias as a known term (no parameter).
    Ambiguity,          ///< The float ambiguity (phase rows; NL and WL on the NL/WL basis).
    PhaseColour,        ///< The line of sight's phase-colour state (+1, phase rows).
};

/// Number of @ref ObsEffect enumerators.
inline constexpr std::size_t kObsEffectCount = 15;

/**
 * @brief A set of effects: the terms a caller asks @ref observationEquation to build.
 *
 * A plain bit set, one bit per @ref ObsEffect.  A row that enters the update asks for every
 * effect; a seed asks for the effects it knows (measurement/Seeds.hpp), so a term it leaves
 * out is neither looked up nor required (a seed for an ambiguity is formed before the
 * ambiguity exists).
 */
struct ObsEffects {
    std::uint32_t bits{0};  ///< Bit @c k set when the effect with value @c k is in the set.

    /// @return True when @p effect is in the set.
    constexpr bool has(ObsEffect effect) const {
        return ((bits >> static_cast<unsigned>(effect)) & 1u) != 0u;
    }
    /// @return The set with @p effect added.
    constexpr ObsEffects with(ObsEffect effect) const {
        return ObsEffects{bits | (1u << static_cast<unsigned>(effect))};
    }
    /// @return The set with @p effect removed.
    constexpr ObsEffects without(ObsEffect effect) const {
        return ObsEffects{bits & ~(1u << static_cast<unsigned>(effect))};
    }
};

/**
 * @brief The set of the listed effects.
 * @param list The effects.
 * @return The set.
 */
constexpr ObsEffects effectsOf(std::initializer_list<ObsEffect> list) {
    ObsEffects set;
    for (const ObsEffect effect : list) set = set.with(effect);
    return set;
}

/// Every effect: what a row that enters the update asks for.
inline constexpr ObsEffects kAllEffects{(1u << kObsEffectCount) - 1u};

/**
 * @brief The name of an effect, as the traces and the check report print it.
 * @param effect The effect.
 * @return A short lower-case name ("receiver_clock"); "?" for a value outside the enumeration.
 */
const char* obsEffectName(ObsEffect effect);

/**
 * @brief Where a parameter is and the value the row is evaluated at.
 *
 * The answer of a @ref ParamLookup for one @ref ParamRef.
 */
struct ParamSlot {
    int    column{-1};  ///< The parameter's column in the caller's row; negative when the parameter does not exist.
    double x{0.0};      ///< The value the row is evaluated at (the linearization value).
};

/**
 * @brief A non-owning reference to the caller's parameter lookup: any object with
 *        <tt>ParamSlot find(const ParamRef&) const</tt>.
 *
 * Built from the object at the call and used inside it; it holds a pointer, so the object
 * must outlive the call that receives the reference.
 */
class ParamLookup {
public:
    /**
     * @brief Refers to @p view.
     * @tparam View A type with <tt>ParamSlot find(const ParamRef&) const</tt>.
     * @param view The lookup; must outlive this reference.
     */
    template <class View>
    explicit ParamLookup(const View& view) : view_(&view), find_(&call<View>) {}

    /**
     * @brief Asks the lookup for one parameter.
     * @param ref The parameter.
     * @return Its slot.
     */
    ParamSlot find(const ParamRef& ref) const { return find_(view_, ref); }

private:
    template <class View>
    static ParamSlot call(const void* view, const ParamRef& ref) {
        return static_cast<const View*>(view)->find(ref);
    }
    const void* view_;
    ParamSlot (*find_)(const void*, const ParamRef&);
};

/// Number of parameters one term reads at most (the three components of the position).
inline constexpr std::size_t kObsMaxParts = 3;

/**
 * @brief One parameter a term reads.
 */
struct ObsPart {
    ParamRef ref{};         ///< The parameter.
    double   partial{0.0};  ///< Its partial before the term's factor.
    int      column{-1};    ///< Its column in the caller's row.
    double   x{0.0};        ///< The value the row is evaluated at.
};

/**
 * @brief One term of the sum.
 *
 * With no part the term is the number @ref constant.  With parts its value is
 * <tt>scale * (partial_0 * x_0 + partial_1 * x_1 + ...)</tt>, the parts added in order, and
 * the Jacobian entry of each part is <tt>scale * partial</tt>.  The range correction is the
 * exception: its parts are the position components with partial minus the line of sight, and
 * its value is read from the equation's @ref ObservationEquation::range_origin.
 */
struct ObsAddend {
    ObsEffect effect{ObsEffect::Modeled};      ///< What the term is.
    double    constant{0.0};                   ///< The value of a term without parts.
    double    scale{1.0};                      ///< The factor outside the sum of the parts.
    std::array<ObsPart, kObsMaxParts> part{};  ///< The parameters the term reads, in order.
    std::uint8_t n_part{0};                    ///< Number of valid entries in @ref part.
};

/**
 * @brief Why a row has no equation.
 */
enum class ObsFault : std::uint8_t {
    None = 0,         ///< The row has an equation.
    NotCodeOrPhase,   ///< The signal is neither code nor phase (a Doppler row has its own function).
    NoAmbiguity,      ///< A phase row whose ambiguity parameter does not exist.
    NlWlReference,    ///< The NL/WL basis: no reference band, no wavelength, or a missing NL or WL parameter.
    UnmappedIdentity, ///< A satellite or signal text that is not a canonical RINEX id, so no parameter can be named.
};

/**
 * @brief The terms of one row's predicted observable.
 */
struct ObservationEquation {
    std::array<ObsAddend, kObsEffectCount> addend{};  ///< The terms in the order of the sum; the first @ref n are valid.
    std::uint8_t    n{0};                             ///< Number of valid terms.
    bool            usable{false};                    ///< False when the row cannot be built (see @ref fault).
    ObsFault        fault{ObsFault::None};            ///< The reason a row is unusable.
    Eigen::Vector3d range_origin{Eigen::Vector3d::Zero()};  ///< The position the modeled value was computed at.
};

/**
 * @brief The values of the terms, indexed by @ref ObsEffect; a term that is absent is 0.
 *
 * The decomposition traces print them.
 */
struct ObsTermValues {
    std::array<double, kObsEffectCount> value{};  ///< One value per effect.

    /**
     * @brief The value of one effect.
     * @param effect The effect.
     * @return Its term's value, or 0.0 when the row has no such term.
     */
    double operator[](ObsEffect effect) const { return value[static_cast<std::size_t>(effect)]; }
};

// =======================================================================
// Building, evaluating, scattering
// =======================================================================

/**
 * @brief The GLONASS channel at which the carrier frequency of an FDMA band (G1, G2) is taken.
 *
 * Only the slant ionosphere term scales by a carrier frequency.  The row builder takes the
 * frequency of each satellite's own channel from the frequency helper.  The ambiguity seed has
 * always taken channel 0 for every satellite; @ref Zero keeps that number until the seeds'
 * frequency rule is decided.  Every other band has one frequency and does not depend on the rule.
 */
enum class FdmaChannel : std::uint8_t {
    Satellite = 0,  ///< The satellite's channel, from @ref ObsInput::ppp_utils.
    Zero,           ///< Channel 0, whatever the satellite's channel.
};

/**
 * @brief What the equation reads besides the observation and the parameters.
 *
 * Non-owning references built per call by the owner of the model.
 */
struct ObsInput {
    RowProducts                       products;       ///< The configuration, the model's tables and the SSR store.
    const PppUtils&                   ppp_utils;      ///< Frequencies and wavelengths (GLONASS slots seeded).
    const StateInitializer*           receiver_bias;  ///< Resolver of the receiver code-bias state of a code signal; null: none.
    const std::map<std::string, char>& nlwl_ref_band; ///< Per satellite, the NL/WL basis' reference band digit.
    FdmaChannel                       fdma{FdmaChannel::Satellite};  ///< Channel of the FDMA bands' frequency.
};

/**
 * @brief Builds the term list of one code or phase row.
 *
 * Reads the configuration flags that switch terms on (the sum in the file header lists the
 * terms), asks @p params for each parameter a switched-on term needs, and leaves a term out
 * when its parameter does not exist.  A phase row whose ambiguity does not exist (on the
 * NL/WL basis: whose reference band, wavelength, NL or WL parameter is missing) is unusable.
 * A satellite or signal that is not a canonical RINEX id (@c SatId::str and
 * @c SignalId::obsCode would not give the text back) cannot be named, so a row that needs the
 * name is unusable too.
 *
 * Throws @c std::out_of_range when the observation has no modeled value for @p ref_type, as
 * the row builder always has; the row loop catches it.
 *
 * With @p effects set to fewer than all effects the equation holds only those terms, and a
 * term left out is neither looked up nor able to make the row unusable.  A seed uses this
 * (measurement/Seeds.hpp).
 *
 * @param in       Configuration, tables, SSR store, frequency helper, receiver-bias resolver.
 * @param obs      The satellite's modeled observation.
 * @param ref_type The signal (a RINEX code, or an iono-free pair "a_b").
 * @param params   The caller's parameters.
 * @param effects  The terms to build (default: all).
 * @return The equation; @c usable is false (and @c fault says why) when the row cannot be built.
 */
ObservationEquation observationEquation(const ObsInput& in, const PppObservation& obs,
                                        const std::string& ref_type, const ParamLookup& params,
                                        ObsEffects effects = kAllEffects);

/**
 * @brief The term of one effect.
 * @param eq     The equation.
 * @param effect The effect.
 * @return The term, or null when the row has none (the effect is not in the equation).
 */
const ObsAddend* findTerm(const ObservationEquation& eq, ObsEffect effect);

/**
 * @brief The value of one term of an equation.
 * @param eq   The equation the term belongs to.
 * @param term One of its terms.
 * @return The term's value (m): its constant, or its factor times the sum of its parts.
 */
double termValue(const ObservationEquation& eq, const ObsAddend& term);

/**
 * @brief The predicted observable: the terms added left to right.
 * @param eq    The equation (usable).
 * @param terms Out, optional: the value of each term.
 * @return The predicted observable (m).
 */
double evaluate(const ObservationEquation& eq, ObsTermValues* terms = nullptr);

/**
 * @brief Writes the partials of the equation into a dense Jacobian row.
 *
 * For every part, <tt>row(column) = scale * partial</tt>; the other entries are not touched.
 *
 * @param eq  The equation (usable).
 * @param row The row, already sized and zeroed by the caller.
 */
void scatterDense(const ObservationEquation& eq, Eigen::VectorXd& row);

/**
 * @brief Whether a constellation carries an inter-system bias parameter: every constellation
 *        except the time reference (and an unknown one).
 * @param sys The constellation.
 * @return True when the equation has an @c SystemTimeOffset term for it.
 */
inline bool hasInterSystemBias(System sys) {
    return !isTimeReference(sys) && sys != System::Unknown;
}

} // namespace GnssProcessor

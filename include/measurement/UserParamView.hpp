#pragma once
/**
 * @file UserParamView.hpp
 * @brief The single-receiver filter's answer to "where is this parameter": a view of a
 *        @c StateManager and a state vector, and the one place a typed parameter identity
 *        becomes a user-side state key.
 *
 * The observation equation (measurement/ObservationEquation.hpp) names its parameters by
 * @ref ParamRef.  The single-receiver filter keeps them in @c StateManager under string keys
 * (filter/StateKeySchema.hpp) and at fixed columns.  @ref UserParamView maps one to the
 * other, and @ref userKey is the only function that renders a typed identity as a user-side
 * key, so a key format is written in one place on this side.
 *
 * Plain value types and free functions.
 */

#include "common/ParamRef.hpp"
#include "measurement/ObservationEquation.hpp"

#include <Eigen/Dense>

#include <cstddef>
#include <string>

namespace GnssProcessor {

class StateManager;

/**
 * @brief The state key of a receiver code-bias parameter ("GPS_DCB", "GPS_IFB5",
 *        "GLONASS_IFB1_k-4", "GPS_ISC_C1W").
 * @param key The typed key.
 * @return The text key @c StateManager stores the parameter under; empty for a key whose
 *         constellation or band the identity tables do not know.
 */
std::string userCodeBiasKey(const RxCbKey& key);

/**
 * @brief The state key a string-keyed parameter is stored under in @c StateManager.
 *
 * Covers the families keyed by text: the ambiguity (per signal, per iono-free pair, narrow
 * and wide lane), slant ionosphere, phase colour, receiver code bias, satellite code bias
 * (per signal or iono-free pair), satellite phase bias, receiver phase bias and satellite
 * clock.
 *
 * @param ref The parameter.
 * @return The key; empty for a parameter that is not string-keyed (the fixed-core columns and
 *         the inter-system bias, which have accessors), or whose identity the key formats cannot
 *         render (a band the constellation does not use).
 */
std::string userKey(const ParamRef& ref);

/**
 * @brief A @ref ParamLookup answer for one @c StateManager and the state vector a row is
 *        evaluated at.
 *
 * Fixed-core columns (position, zenith wet delay, receiver clock, phase clock, gradients)
 * answer with their column whenever the state has them.  Dynamic columns (inter-system
 * bias, ionosphere, ambiguities, biases, clocks, phase colour) answer with their column when
 * it is inside the state; everything else answers with a negative column.
 */
class UserParamView {
public:
    /**
     * @brief A view of @p states evaluated at @p x.
     * @param states     The state layout.
     * @param x          The state vector the row is evaluated at; must outlive the view.
     * @param state_size Number of state columns (a dynamic column must be below it).
     */
    UserParamView(const StateManager& states, const Eigen::VectorXd& x, std::size_t state_size)
        : states_(states), x_(x), state_size_(state_size) {}

    /**
     * @brief The slot of one parameter.
     * @param ref The parameter.
     * @return Its column and the value of @c x there; a negative column when the state does
     *         not have it.
     */
    ParamSlot find(const ParamRef& ref) const;

private:
    ParamSlot fixed(std::size_t column) const;
    ParamSlot dynamic(long long column) const;

    const StateManager&    states_;
    const Eigen::VectorXd& x_;
    std::size_t            state_size_;
};

} // namespace GnssProcessor

#pragma once
/**
 * @file StationRows.hpp
 * @brief The data rows of one network station: the lookup of its parameter set for the
 *        observation equation, the inputs the equation reads, and the row loop that turns a
 *        station's observations into innovations, variances and a dense Jacobian.
 *
 * ### One equation
 *
 * A row's predicted observable is the term list of measurement/ObservationEquation.hpp.  The
 * network producer reads it through @ref NetView, which answers for each parameter with the
 * slot of the station's set (network/state/StationParams.hpp) and its linearization point.
 * The single-receiver filter and the closed-loop consumer read the same function through the
 * lookup of their state vector, so the producer's and the consumer's rows are one equation
 * under different parameter sets.  Shared satellite parameters answer with their column and
 * the value 0: they are absolute in the network solve and are never written back.
 *
 * ### The rows
 *
 * @ref networkStationRows is the network's row loop.  It admits a satellite's signal as the
 * single-receiver model's rule does (@c isUsableMeasurement, the stored geometry), builds the
 * equation, evaluates it for the innovation, scatters its partials into a dense row and takes
 * the variance from the stochastic model.  The columns of the dense matrix are the slots of
 * the set.  A signal with no equation (a satellite or signal text that is not a canonical
 * RINEX identity) is left out.
 *
 * Plain value types and free functions; no behaviour classes.
 */

#include "common/EpochMeasurement.hpp"
#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "measurement/ObservationEquation.hpp"
#include "measurement/ReceiverBias.hpp"
#include "network/state/StationParams.hpp"
#include "products/ProductTypes.hpp"   // ClockRefMap

#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

class GnssConfig;
class LogManager;
class PppModel;

/**
 * @brief The lookup of a station's parameter set for the observation equation.
 *
 * Answers for a parameter with the index of its slot and the slot's linearization point; a
 * parameter the set does not hold (the receiver phase clock and the troposphere gradients of
 * the single-receiver filter, a satellite signal the station was not given) has a negative
 * column.
 */
class NetView {
public:
    /**
     * @brief A view of @p set.
     * @param set The station's set; must outlive the view.
     */
    explicit NetView(const StationParams& set) : set_(set) {}

    /**
     * @brief The slot of one parameter.
     * @param ref The parameter.
     * @return Its slot index and linearization point; a negative column when the set does not
     *         hold it.
     */
    ParamSlot find(const ParamRef& ref) const;

private:
    const StationParams& set_;
};

/**
 * @brief What a station's births and rows read besides its observations and its set.
 *
 * Built once per station and unchanged afterwards.
 */
struct StationRowInputs {
    /// Carrier frequencies for the row: the GLONASS channels are the station's own, from its
    /// RINEX header.
    PppUtils             freq;
    /// The receiver code-bias parameter of a code signal.  Its frequency helper keeps the
    /// default GLONASS channel table, the helper the seeds have always scaled by.
    ReceiverBiasResolver rx_bias;
    /// Per constellation name, the phase signals that may have a satellite phase-bias
    /// parameter; a constellation or an empty map admits every signal.
    std::map<std::string, std::set<std::string>> phase_bias_allowlist;
    /// Logging, for the rules that report (the stored-geometry check, the seeds).
    LogManager*          log_manager;

    /**
     * @brief Builds the inputs of one station.
     * @param config          Processing configuration.
     * @param model           The station's model (its band latches and GLONASS channels).
     * @param log_manager     Logging.
     * @param glonass_slots   The station's GLONASS satellite to channel map (RINEX header).
     * @param phase_bias_allowlist The phase-bias coverage allowlist (empty: admit all).
     */
    StationRowInputs(const GnssConfig& config, const PppModel& model, LogManager& log_manager,
                     const std::map<std::string, int>& glonass_slots,
                     std::map<std::string, std::set<std::string>> phase_bias_allowlist);
};

/**
 * @brief The references a station's births and rows are computed from.
 *
 * Non-owning, built per call from the run's configuration and the station's own model and
 * inputs.
 */
struct StationRowContext {
    const GnssConfig&      config;  ///< Processing configuration.
    const PppModel&        model;   ///< The station's model (bias and antenna tables).
    LogManager&            log;     ///< Logging.
    const StationRowInputs& inputs; ///< The station's frequency helper, resolver and allowlist.

    /**
     * @brief The references of a station.
     * @param cfg    Processing configuration.
     * @param mdl    The station's model.
     * @param in     The station's inputs (their logging is used).
     */
    StationRowContext(const GnssConfig& cfg, const PppModel& mdl, const StationRowInputs& in)
        : config(cfg), model(mdl), log(*in.log_manager), inputs(in) {}
};

/**
 * @brief The equation inputs of the data rows.
 * @param c The station's references.
 * @return Inputs whose frequency helper is the station's own.
 */
ObsInput rowObsInput(const StationRowContext& c);

/**
 * @brief The equation inputs of the residual seeds (inter-system bias, cold clock, receiver
 *        code biases, slant ionosphere): the resolver's default-table frequency helper.
 * @param c The station's references.
 * @return The inputs.
 */
ObsInput residualSeedObsInput(const StationRowContext& c);

/**
 * @brief The equation inputs of the ambiguity seed: the station's frequency helper with the
 *        GLONASS FDMA bands taken at channel 0.
 * @param c The station's references.
 * @return The inputs.
 */
ObsInput ambiguitySeedObsInput(const StationRowContext& c);

/**
 * @brief The data rows of a station's epoch.
 *
 * For each satellite with valid stored geometry and each usable signal, in the order of the
 * observations and of each satellite's signals: the innovation (observed minus predicted), the
 * variance, the dense Jacobian row over the slots of @p set and the identities.  A row whose
 * equation is unusable, or whose evaluation throws, is left out.  The result has no rows (all
 * arrays empty) when there is no observation, no usable signal, or no row could be built;
 * a row with a non-finite innovation or variance is kept with innovation 0 and variance 1e6.
 *
 * @param c            The station's references.
 * @param set          The station's set, after its births.
 * @param observations The station's modeled observations of the epoch.
 * @return The epoch's measurement update (the observations are not copied into it).
 */
EpochMeasurement networkStationRows(const StationRowContext& c, const StationParams& set,
                                    const std::vector<PppObservation>& observations);

} // namespace GnssProcessor

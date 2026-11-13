#pragma once
/**
 * @file StationRows.hpp
 * @brief The data rows of one network station: the lookup of its parameter set for the
 *        observation equation, the inputs the equation reads, and the row loop that turns a
 *        station's observations into innovations, variances and the Jacobian entries of each row.
 *
 * ### One equation
 *
 * A row's predicted observable is the term list of measurement/ObservationEquation.hpp.  The
 * network producer reads it through @ref GnssProcessor::NetView "NetView", which answers for each parameter with the
 * slot of the station's set (network/state/StationParams.hpp) and its linearization point.
 * The single-receiver filter and the closed-loop consumer read the same function through the
 * lookup of their state vector, so the producer's and the consumer's rows are one equation
 * under different parameter sets.  Shared satellite parameters answer with their column and
 * the value 0: they are absolute in the network solve and are never written back.
 *
 * ### The rows
 *
 * @ref GnssProcessor::networkStationRows "networkStationRows" is the network's row loop.  It admits a satellite's signal as the
 * single-receiver model's rule does (@c isUsableMeasurement, the stored geometry), builds the
 * equation, evaluates it for the innovation, keeps its partials as (slot, value) pairs
 * (@ref GnssProcessor::StationDataRows "StationDataRows") and takes the variance from the stochastic model.  A signal with no
 * equation (a satellite or signal text that is not a canonical RINEX identity) is left out.
 * At trace level each row also prints its decomposition (measurement/RowTrace.hpp), the line
 * the single-receiver model prints for its rows.
 *
 * ### No dense Jacobian
 *
 * The columns of a row are the slots of the set, and a row reads a handful of them.  The rows
 * therefore hold only the entries their equations write; the split
 * (network/assembly/StationSplit.hpp) decides each slot's disposition from them and writes each
 * entry straight into the shared border or the local block.  No matrix of rows by slots is
 * formed on the network (@ref GnssProcessor::denseJacobian "denseJacobian" builds one for tests and diagnostics).
 *
 * Plain value types and free functions; no behaviour classes.
 */

#include "common/PppObservation.hpp"
#include "common/PppUtils.hpp"
#include "measurement/ObservationEquation.hpp"
#include "measurement/ReceiverBias.hpp"
#include "network/state/StationParams.hpp"
#include "products/ProductTypes.hpp"   // ClockRefMap

#include <Eigen/Dense>

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

class GnssConfig;
class LogManager;
class PppModel;

/**
 * @brief One Jacobian entry of a data row: a slot of the station's set and the row's partial
 *        derivative there.
 */
struct RowPartial {
    int    slot{-1};    ///< The slot of the station's set (the entry's column).
    double value{0.0};  ///< The entry: the term's factor times the part's partial
                        ///< (measurement/ObservationEquation.hpp); a zero is kept.
};

/**
 * @brief A station's data rows of one epoch, as the row loop leaves them for the split.
 *
 * Row-aligned: the innovation, the variance, the satellite and the signal of row @c r, and its
 * Jacobian entries <tt>partials[row_start[r]]</tt> to <tt>partials[row_start[r + 1] - 1]</tt>.
 * A row holds one entry per parameter its equation reads, in the order the equation lists them;
 * a slot read twice keeps the last value, as an assignment into a dense row would.  Every other
 * entry of the row is zero.  An entry whose value is zero (a partial that evaluates to plus or
 * minus zero) is kept: the split writes it as it is, and does not count it as reading its slot.
 */
struct StationDataRows {
    Eigen::VectorXd          innovations;    ///< Observed minus predicted (m), one per row.
    Eigen::VectorXd          R_v;            ///< The variance of each row (m^2).
    std::vector<std::string> satellite_ids;  ///< The satellite of each row.
    std::vector<std::string> signal_types;   ///< The signal of each row.
    std::vector<RowPartial>  partials;       ///< The rows' Jacobian entries, row after row.
    std::vector<std::size_t> row_start{0};   ///< Where each row's entries begin; one element more than rows.
    std::size_t              n_slots{0};     ///< Slots of the set the rows were built over (the Jacobian's columns).

    /**
     * @brief Number of data rows of the epoch.
     * @return The number of rows.
     */
    Eigen::Index rows() const { return innovations.size(); }
};

/**
 * @brief The entry of one row at one slot.
 * @param rows The rows.
 * @param row  The row (in range).
 * @param slot The slot.
 * @return The row's entry at @p slot; 0 when the row reads no parameter there.
 */
double rowPartial(const StationDataRows& rows, Eigen::Index row, int slot);

/**
 * @brief Consecutive slots of every row as a dense block.
 *
 * The congruence screen reads the receiver position's three slots this way.
 *
 * @param rows       The rows.
 * @param first_slot The first slot.
 * @param count      The number of slots.
 * @return A matrix of @c rows.rows() by @p count holding the entries at slots
 *         @p first_slot to @p first_slot + @p count - 1, zero elsewhere.
 */
Eigen::MatrixXd slotColumns(const StationDataRows& rows, int first_slot, int count);

/**
 * @brief The dense Jacobian of the rows, rows by @c n_slots.
 *
 * For tests and diagnostics: the producer never forms it.
 *
 * @param rows The rows.
 * @return The matrix, with every entry the rows hold and zero elsewhere.
 */
Eigen::MatrixXd denseJacobian(const StationDataRows& rows);

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
    /// The bias product's clock-reference observables per constellation letter: the bands the
    /// receiver code biases are referenced to.  Points at the process-wide product cache's map,
    /// which is read when used (a product loaded after the station was built is seen); a test
    /// points it at its own.  Never null.
    const ClockRefMap*   clock_refs;

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
 * @brief The ambiguity parameterization of every network station: one ambiguity per (satellite,
 *        signal) or per iono-free pair.
 *
 * The producer's local block, its ambiguity carries and its integer searches are all per band.
 * The equation's inputs therefore name this basis, whatever @c estimation.state_ambiguity_basis
 * says: that key chooses the single-receiver filter's layout and has no network meaning.
 */
inline constexpr StateAmbiguityBasis kNetworkAmbiguityBasis = StateAmbiguityBasis::PerBand;

/**
 * @brief The equation inputs of the data rows.
 * @param c The station's references.
 * @return Inputs whose frequency helper is the station's own, on the per-band ambiguity basis
 *         (@ref GnssProcessor::kNetworkAmbiguityBasis "kNetworkAmbiguityBasis").
 */
ObsInput rowObsInput(const StationRowContext& c);

/**
 * @brief The equation inputs of the residual seeds (inter-system bias, cold clock, receiver
 *        code biases, slant ionosphere): the resolver's default-table frequency helper, or
 *        under @c estimation.seed_full_equation the rows' helper (the station's own GLONASS
 *        channels).
 * @param c The station's references.
 * @return The inputs.
 */
ObsInput residualSeedObsInput(const StationRowContext& c);

/**
 * @brief The equation inputs of the ambiguity seed: the station's frequency helper with the
 *        GLONASS FDMA bands taken at channel 0, or under @c estimation.seed_full_equation at
 *        the satellite's own channel, as the rows take them.
 * @param c The station's references.
 * @return The inputs.
 */
ObsInput ambiguitySeedObsInput(const StationRowContext& c);

/**
 * @brief The data rows of a station's epoch.
 *
 * For each satellite with valid stored geometry and each usable signal, in the order of the
 * observations and of each satellite's signals: the innovation (observed minus predicted), the
 * variance, the row's Jacobian entries over the slots of @p set and the identities.  A row whose
 * equation is unusable, or whose evaluation throws, is left out.  The result has no rows (all
 * arrays empty) when there is no observation, no usable signal, or no row could be built;
 * a row with a non-finite innovation or variance is kept with innovation 0 and variance 1e6.
 *
 * @param c            The station's references.
 * @param set          The station's set, after its births.
 * @param observations The station's modeled observations of the epoch.
 * @return The rows; @c n_slots is the size of @p set.
 */
StationDataRows networkStationRows(const StationRowContext& c, const StationParams& set,
                                   const std::vector<PppObservation>& observations);

} // namespace GnssProcessor

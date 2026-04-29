
#pragma once

/**
 * @file StateInitializer.hpp
 * @brief First-epoch state seeding for inter-system biases, phase clock, and code biases.
 *
 * StateInitializer encapsulates the one-shot initialization logic that runs
 * on the first epoch to seed ISB, phase clock offset, and receiver DCB/IFB
 * states from code and phase observation residuals.  Extracting this from
 * MeasurementModel keeps the per-epoch hot path readable and isolates the
 * initialization-specific helpers (computeRawResidual, resolveReceiverBias,
 * computeIonosphereSeed, computeMedian) in one place.
 */

#include "config/GnssConfig.hpp"
#include "filter/StateManager.hpp"
#include "logging/LogManager.hpp"
#include "common/PppTypes.hpp"
#include <Eigen/Dense>
#include <string>
#include <vector>

// Forward declarations
namespace GnssProcessor {
class PppModel;
}

namespace GnssProcessor {

/**
 * @class StateInitializer
 * @brief Seeds first-epoch filter states from observation residuals.
 *
 * Owns the initialization flags and provides three entry points:
 *   - initializeInterSystemBiases()
 *   - initializePhaseClockOffset()
 *   - initializeCodeBiases()
 *
 * Also exposes helpers used by MeasurementModel during the state pre-pass
 * and Jacobian assembly (computeIonosphereSeed, resolveReceiverBias,
 * computeRawResidual).
 */
class StateInitializer {
public:
    StateInitializer(const GnssConfig& config,
                     const PppModel& ppp_model,
                     StateManager& state_manager,
                     LogManager& log_manager);

    // -------------------------------------------------------------------
    // First-epoch initialization entry points
    // -------------------------------------------------------------------

    /**
     * @brief Initialize inter-system biases from first-epoch code observations.
     *
     * Computes the median code residual per system relative to GPS.
     */
    void initializeInterSystemBiases(const std::vector<PppObservation>& observations,
                                      const std::string& epoch_prefix);

    /**
     * @brief Seed the phase clock offset from the codephase residual difference.
     */
    void initializePhaseClockOffset(const std::vector<PppObservation>& observations,
                                     const std::string& epoch_prefix);

    /**
     * @brief Seed DCB/IFB states from cross-band code residual differences.
     *
     * Must be called after ensureRequiredStates() has created the code-bias
     * states and after ionosphere states have been seeded.
     */
    void initializeCodeBiases(const std::vector<PppObservation>& observations,
                               const std::string& epoch_prefix);

    // -------------------------------------------------------------------
    // Initialization flags (queried/set by MeasurementModel orchestrator)
    // -------------------------------------------------------------------

    bool isIsbInitialized()        const { return isb_initialized_; }
    bool isPhaseClockInitialized() const { return phase_clock_initialized_; }
    bool isCodeBiasInitialized()   const { return code_bias_initialized_; }

    void markIsbInitialized()        { isb_initialized_ = true; }
    void markPhaseClockInitialized() { phase_clock_initialized_ = true; }
    void markCodeBiasInitialized()   { code_bias_initialized_ = true; }

    // -------------------------------------------------------------------
    // Shared helpers (used by MeasurementModel for state pre-pass / Jacobian)
    // -------------------------------------------------------------------

    /**
     * @brief Resolved receiver-bias state for one observation signal.
     */
    struct ReceiverBiasEntry {
        std::string state_key;   /**< Key into code_bias_indices_ (e.g. "G_DCB" or "G_IFB5"). */
        double      coefficient; /**< Jacobian coefficient for the state.  NaN ? no bias modelled. */
    };

    /**
     * @brief Determine the receiver-bias state and coefficient for a signal.
     */
    ReceiverBiasEntry resolveReceiverBias(const std::string& obs_signal,
                                           System system,
                                           const std::string& satellite) const;

    /**
     * @brief Compute a "raw" residual for an observation/ref_type pair.
     *
     * residual = observed - modeled - rx_clk - mf_w*zwd - isb - tropo_grad
     *          - iono - code_bias - sat_bias
     */
    double computeRawResidual(const PppObservation& obs,
                               const std::string& ref_type,
                               const Eigen::VectorXd& x) const;

    /**
     * @brief Compute ionosphere seed from dual-frequency code pseudoranges.
     */
    double computeIonosphereSeed(const PppObservation& obs) const;

    /**
     * @brief Map a System enum to its constellation name for ISB lookup.
     */
    static std::string systemToIsbName(System system);

    /**
     * @brief Map a System enum to the single-char BIA clock reference identifier.
     */
    static char systemToClockRefChar(System system);

private:
    const GnssConfig& config_;
    const PppModel& ppp_model_;
    StateManager& state_manager_;
    LogManager& log_manager_;
    PppUtils ppp_utils_;

    bool isb_initialized_ = false;
    bool phase_clock_initialized_ = false;
    bool code_bias_initialized_ = false;
};

} // namespace GnssProcessor

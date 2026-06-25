#pragma once

/**
 * @file NeqAccumulator.hpp
 * @brief Cross-session normal-equation accumulator + export (item #13B).
 *
 * Accumulates a label-keyed least-squares normal system
 *   N += HᵀR⁻¹H,  u += HᵀR⁻¹Δz,  ltpl += ΔzᵀR⁻¹Δz
 * from the same (H, R, innovation) the Kalman filter accepts each epoch, and
 * writes a human-readable @c .asc and a compact round-trippable @c .neq at end
 * of run, with the a-posteriori variance factor σ̂₀².  It is a *sibling
 * consumer* of the measurement update — it never owns or mutates filter state
 * (State-vector discipline: StateManager remains the single source of truth).
 *
 * Parameters are keyed by the stable StateKeySchema-derived labels from
 * StateManager::getStateLabels(), so growing / pruning the state vector across
 * epochs (which shifts raw indices) maps cleanly onto one global parameter
 * set — the prerequisite for combining normal equations across sessions.
 *
 * Per-epoch *dynamic* parameters (receiver clock, clock-rate, phase-clock,
 * velocity, IMU attitude/biases, fast per-satellite ionosphere) are eliminated
 * each epoch by an exact Schur complement, so the retained system spans only
 * the (approximately) time-constant / persistent parameters: station position,
 * zenith wet delay, troposphere gradients, inter-system time/rate offsets,
 * receiver/satellite biases, and per-arc carrier-phase ambiguities.  Carrier
 * ambiguities are arc-stamped (a re-acquisition gap or a flagged slip-reset
 * starts a fresh segment) so distinct tracking arcs are distinct parameters.
 *
 * Modelling caveats (documented, intentional for a first export):
 *  - Zenith wet delay and troposphere gradients are treated as session-constant
 *    (the Kalman filter models them as a slow random walk).  Exact for a short
 *    static session; a multi-hour session would want a piecewise parameterization.
 *  - In-place ambiguity slip-resets are arc-separated only when the caller
 *    supplies the reset-satellite set for that epoch (the post-processing driver
 *    does); a continuous-arc reset with no signal would not be separated.
 */

#include "common/EpochMeasurement.hpp"

#include <Eigen/Dense>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Label-keyed normal-equation accumulator with epoch-local Schur
     *        reduction and .neq/.asc export.
     */
    class NeqAccumulator {
    public:
        NeqAccumulator() = default;

        /**
         * @brief Accumulate one accepted epoch update into the normal system.
         *
         * @param meas             Accepted measurement (H, R_v / R_full, innovations).
         * @param labels           Stable per-state-index labels
         *                         (StateManager::getStateLabels()); size must
         *                         equal @c meas.H.cols() or the epoch is skipped.
         * @param reset_satellites Primary satellite IDs whose ambiguities were
         *                         reset this epoch (starts a fresh ambiguity arc);
         *                         may be empty.
         */
        void accumulate(const EpochMeasurement& meas,
                        const std::vector<std::string>& labels,
                        const std::set<std::string>& reset_satellites);

        /**
         * @brief Solved normal-equation result.
         */
        struct Solution {
            Eigen::VectorXd          x;            //!< x̂ = N⁻¹ u over the retained labels.
            Eigen::VectorXd          sigma;        //!< Per-parameter formal σ = √(σ̂₀²·(N⁻¹)ᵢᵢ).
            std::vector<std::string> labels;       //!< Parameter labels (row order of x / N).
            double                   sigma0_sq{ 0.0 };  //!< A-posteriori variance factor σ̂₀² = WSSR/dof.
            double                   dof{ 0.0 };        //!< Degrees of freedom = n_obs − n_params − n_local.
            double                   wssr{ 0.0 };       //!< Post-fit weighted SSR = ltpl − uᵀN⁻¹u.
            std::size_t              n_obs{ 0 };        //!< Total observation rows accumulated.
            std::size_t              n_params{ 0 };     //!< Number of retained (global) parameters.
            std::size_t              n_local{ 0 };      //!< Total epoch-local parameters eliminated.
            bool                     ok{ false };       //!< True if the system solved (N positive-definite).
        };

        /**
         * @brief Solve x̂ = N⁻¹u and form σ̂₀², dof and the scaled covariance.
         * @return Solution (ok == false if the system is empty or singular).
         */
        Solution solve() const;

        /**
         * @brief Write @c <stem>.asc (human-readable) and @c <stem>.neq (compact,
         *        round-trippable) for the supplied solution.
         * @param stem Output path without extension (e.g. the navigation-solution stem).
         * @param sol  Result of solve().
         * @return True on success; false on empty system or I/O failure.
         */
        bool writeFiles(const std::filesystem::path& stem, const Solution& sol) const;

        /// @return True if no observations have been accumulated yet.
        bool empty() const { return n_obs_ == 0; }
        /// @return Number of retained (global) parameters.
        std::size_t numParameters() const { return static_cast<std::size_t>(N_.rows()); }
        /// @return Total observation rows accumulated.
        std::size_t numObservations() const { return n_obs_; }

    private:
        /// True if @p label is a per-epoch dynamic state eliminated via Schur each epoch.
        static bool isEpochLocal_(const std::string& label);
        /// True if @p label is an arc-stamped carrier-ambiguity label ("AMB:...").
        static bool isArcLabel_(const std::string& label);
        /// True if a reset-satellite matches the satellite embedded in an "AMB:" label.
        static bool resetMatchesArc_(const std::string& arc_base,
                                     const std::set<std::string>& reset_satellites);
        /// Get-or-create the growing global parameter index for @p label.
        Eigen::Index globalIndexFor_(const std::string& label);

        std::map<std::string, Eigen::Index> label_to_global_;  //!< stable label → global parameter index.
        std::vector<std::string>            global_labels_;    //!< global parameter index → label.
        Eigen::MatrixXd N_;        //!< Accumulated reduced normal matrix (global params).
        Eigen::VectorXd u_;        //!< Accumulated reduced RHS.
        double          ltpl_{ 0.0 };  //!< Accumulated reduced prefit weighted SSR.
        std::size_t     n_obs_{ 0 };   //!< Total observation rows.
        std::size_t     n_local_{ 0 }; //!< Total epoch-local parameters eliminated.

        // Arc-stamping bookkeeping.
        std::map<std::string, int> arc_counter_;     //!< AMB base label → current arc index.
        std::set<std::string>      prev_epoch_arcs_; //!< AMB base labels present last epoch (gap detection).
    };

} // namespace GnssProcessor

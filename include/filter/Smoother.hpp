#pragma once

/**
 * @file Smoother.hpp
 * @brief Backward Rauch-Tung-Striebel (RTS) smoother types.
 *
 * This header declares the per-epoch snapshot record captured during the
 * forward Kalman pass and the Smoother class that consumes those records
 * to produce a smoothed trajectory.
 */

#include "common/PppTypes.hpp"
#include <Eigen/Dense>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

    /**
     * @brief Per-epoch state/covariance snapshot used by the RTS smoother.
     *
     * Captured during the forward Kalman pass.  For each epoch we record
     * the a-priori (post-predict, pre-update) state and covariance, the
     * a-posteriori (post-update) state and covariance, the transition
     * matrix F from the previous epoch, the time step, and the dynamic
     * key->index maps that define the state-vector layout.
     *
     * The backward RTS recursion needs both the a-priori and the
     * a-posteriori at every epoch; storing them directly avoids having
     * to re-derive the a-priori from F, Q and the previous posterior
     * when state dimensions change between epochs.
     */
    struct SmootherEpochSnapshot {
        /// Epoch time (GPS scale).
        GnssTime t{};

        /// Elapsed time from the previous epoch (seconds).  Zero at the first epoch.
        double dt{ 0.0 };

        /// Transition matrix from the previous epoch to this one.
        /// Dimension matches \c x_pred / \c P_pred (i.e. the state size at
        /// the end of the previous epoch's update, which equals the state
        /// size right after this epoch's predict).
        Eigen::MatrixXd F;

        /// A-priori state vector (after predict(), before any cycle-slip
        /// resets or measurement update).
        Eigen::VectorXd x_pred;

        /// A-priori covariance matrix corresponding to \c x_pred.
        Eigen::MatrixXd P_pred;

        /// A-posteriori state vector (after the measurement update loop).
        Eigen::VectorXd x_post;

        /// A-posteriori covariance matrix corresponding to \c x_post.
        Eigen::MatrixXd P_post;

        // ---------------------------------------------------------------
        // Post-update core-state indices (reflect the x_post layout)
        // ---------------------------------------------------------------
        size_t pos_idx{ 0 };                                                ///< First position component (x).
        size_t clock_idx{ 0 };                                              ///< Receiver clock bias.
        size_t tropo_idx{ 0 };                                              ///< Tropospheric ZWD.
        size_t velocity_idx{ std::numeric_limits<size_t>::max() };          ///< First velocity component, max() if absent.
        size_t phase_clock_idx{ std::numeric_limits<size_t>::max() };       ///< Phase clock offset, max() if absent.
        size_t gradient_n_idx{ std::numeric_limits<size_t>::max() };        ///< Tropo north gradient, max() if absent.
        size_t gradient_e_idx{ std::numeric_limits<size_t>::max() };        ///< Tropo east gradient, max() if absent.

        // ---------------------------------------------------------------
        // Post-update dynamic-key index maps.  Keys identify the physical
        // quantity (e.g. ambiguity "G01_L1W_L2W", ionosphere "G01") so
        // adjacent epochs can be aligned through a common-key projection
        // even when the state vector grows/shrinks between epochs.
        // ---------------------------------------------------------------
        std::map<std::string, size_t> ambiguities;   ///< Ambiguity key -> state index.
        std::map<std::string, size_t> ionospheres;   ///< Satellite ID -> per-sat iono LEVEL state index.
        std::map<std::string, size_t> iono_rates;    ///< Satellite ID -> per-sat iono RATE (Idot) index; populated only under the constant-velocity model. Kept in the common subspace alongside the level so F_CC carries the F(level,rate)=dt coupling and the rate is smoothed jointly.
        std::map<std::string, size_t> code_biases;   ///< Code bias key -> state index.
        std::map<std::string, size_t> sat_biases;    ///< Per-sat signal bias key -> state index.
        std::map<std::string, size_t> sys_offsets;   ///< Inter-system time offset system -> state index.

        /// Satellites whose ambiguity states were reset by the update at
        /// this epoch (the IDs returned by KalmanFilter::getLastResetSatellites,
        /// accumulated across all iterations of the update loop).
        ///
        /// During the backward recursion, any ambiguity key whose satellite
        /// prefix matches one of these is excluded from the common-key set
        /// when smoothing the previous epoch, because its prior at this
        /// epoch is a re-seeded large-variance value that carries no
        /// information backward.  This is slightly broader than strictly
        /// necessary (could affect different-frequency ambiguities on the
        /// same satellite), but it errs on the side of falling back to the
        /// forward solution for those states — which is always safe.
        std::set<std::string> reset_satellites;

        /// BSSD pivot satellite map (constellation name -> satellite ID) in
        /// effect at the END of this epoch's processing (after all routine,
        /// forced-rebuild, and slip-phase handovers).  Empty when BSSD is
        /// inactive.  The backward pass compares this against the previous
        /// epoch's map: a per-constellation change means the SD datum moved
        /// (a pivot handover), so the affected ambiguity states are in a
        /// different frame than the previous epoch's a-priori.
        std::map<std::string, std::string> pivot_satellites;

        /// CLEAN (undisturbed, non-forced) pivot handovers performed during
        /// this epoch, constellation -> {old_pivot, new_pivot}, as reported by
        /// PivotManager::getCleanHandovers.  For these the SD re-reference is a
        /// lossless, invertible datum change, so the backward pass inverse-
        /// references the affected ambiguities back to the old pivot rather
        /// than segmenting them.  A constellation whose pivot changed but is
        /// absent here (disturbed / forced handover, slipped old pivot) is
        /// segmented instead.  See slip-repair audit P2-1.
        std::map<std::string, std::pair<std::string, std::string>> clean_handovers;

        /// True if the update at this epoch performed a complete filter
        /// reset.  Treated as a hard segment boundary: smoother recursion
        /// does not propagate information across this epoch.
        bool is_discontinuity{ false };

        /// False if no measurement update was applied (e.g. empty epoch,
        /// skipped update).  In that case \c x_post == \c x_pred and
        /// \c P_post == \c P_pred.
        bool has_posterior{ true };
    };

    class LogManager; // forward-declare

    /**
     * @brief Backward Rauch-Tung-Striebel smoother over forward-pass snapshots.
     *
     * Consumes a vector of SmootherEpochSnapshot records produced during
     * the forward Kalman pass and produces a smoothed state/covariance
     * trajectory.  The recursion is applied in a common-key subspace so
     * that state-vector dimension changes between adjacent epochs (from
     * ambiguity additions/removals and per-satellite ionosphere states)
     * are handled gracefully.
     *
     * Segment boundaries (complete filter resets) terminate backward
     * information flow; non-core states on either side of a boundary
     * keep their forward (a-posteriori) values.  Ambiguities on
     * satellites whose states were reset at epoch k+1 are excluded from
     * the common subspace when smoothing epoch k, preventing a re-seeded
     * large-variance prior from contaminating the backward update.
     *
     * Operates on float ambiguities only; any integer-fix corrections
     * applied in the forward pass are ignored by the smoother.
     */
    class Smoother {
    public:
        /**
         * @brief Construct a Smoother from a snapshot buffer.
         *
         * Takes ownership of the snapshot vector.
         *
         * @param snapshots Forward-pass snapshot records in chronological order.
         * @param log_manager Reference to the application LogManager.
         */
        Smoother(std::vector<SmootherEpochSnapshot> snapshots, LogManager& log_manager);

        /**
         * @brief Run the backward RTS recursion.
         *
         * Populates the internal smoothed trajectory.  Safe to call once.
         *
         * @return True on success, false if the snapshot buffer was empty.
         */
        bool run();

        /**
         * @brief Smoothed state/covariance at one epoch.
         *
         * Only the subset of states in the "common subspace" at each
         * recursion step is modified relative to the forward posterior.
         * States outside the subspace retain their forward values.
         */
        struct SmoothedEpoch {
            GnssTime t{};              ///< Epoch time.
            Eigen::VectorXd x;         ///< Smoothed state vector.
            Eigen::MatrixXd P;         ///< Smoothed covariance matrix.

            size_t pos_idx{ 0 };       ///< Index of position (x,y,z).
            size_t clock_idx{ 0 };     ///< Index of receiver clock bias.
            size_t tropo_idx{ 0 };     ///< Index of tropo ZWD.
            size_t velocity_idx{ std::numeric_limits<size_t>::max() };
            size_t phase_clock_idx{ std::numeric_limits<size_t>::max() };
            size_t gradient_n_idx{ std::numeric_limits<size_t>::max() };
            size_t gradient_e_idx{ std::numeric_limits<size_t>::max() };

            bool smoothed{ false };    ///< True if the backward recursion modified this epoch.
        };

        /**
         * @brief Access the smoothed trajectory.
         * @return Vector of smoothed epochs in chronological order (size == input snapshot count).
         */
        const std::vector<SmoothedEpoch>& results() const { return smoothed_; }

        /**
         * @brief Inverse BSSD re-reference of a group of state indices back to
         *        a datum index — the exact inverse of PivotManager's forward
         *        re-reference, anchored on the OLD pivot instead of the new one.
         *
         * Mirrors @c PivotManager::applyCovarianceReReference (plus the state
         * subtract) but with @p datum = the OLD pivot's index:
         * @code
         *   x'(i) = x(i) - x(datum)   for group members i != datum
         *   x'(datum) = 0
         *   P'(i,:) = P(i,:) - P(datum,:),  P'(:,i) = P(:,i) - P(:,datum)
         *   P'(datum,:) = 0,  P'(:,datum) = 0
         * @endcode
         * Operating on the FULL rows/columns of @p P (not a group-local block)
         * is required: the forward transform coupled the group to every other
         * state (position, troposphere, cross-constellation), so the inverse
         * must too, or the ambiguity<->position cross-covariance is dropped
         * (audit P2-1 / R1).  For a single group this exactly recovers the
         * pre-handover (old-pivot) frame; see the unit tests.
         *
         * Public and static so the backward pass and the characterization
         * tests share one implementation.
         *
         * @param group_indices All state indices in the group (incl. @p datum).
         * @param datum         The OLD pivot's state index (becomes the frozen datum).
         * @param[in,out] x     State vector (modified in place).
         * @param[in,out] P     Covariance matrix (modified in place).
         */
        static void inverseReReferenceGroup(
            const std::vector<Eigen::Index>& group_indices,
            Eigen::Index datum,
            Eigen::VectorXd& x,
            Eigen::MatrixXd& P);

    private:
        std::vector<SmootherEpochSnapshot> snapshots_;
        std::vector<SmoothedEpoch> smoothed_;
        LogManager& log_manager_;

        /**
         * @brief Collect parallel index arrays for the common subspace
         *        between adjacent epochs.
         *
         * @param sk    Epoch k snapshot (post-update).
         * @param sk1   Epoch k+1 snapshot (post-update; its \c x_pred / \c F share k's indexing).
         * @param[out] idx_k  Indices in epoch k's state space.
         * @param[out] idx_k1 Parallel indices in epoch k+1's state space.
         * @param exclude_keys Dynamic-state keys (ambiguity / ionosphere /
         *                     sat-bias) to EXCLUDE from the common subspace at
         *                     this boundary — the handover-segmentation set
         *                     computed by the backward pass (constellations
         *                     whose frame changed but cannot be inverse-
         *                     referenced, plus orphaned signal groups).
         */
        void collectCommonIndices(const SmootherEpochSnapshot& sk,
                                  const SmootherEpochSnapshot& sk1,
                                  std::vector<size_t>& idx_k,
                                  std::vector<size_t>& idx_k1,
                                  const std::set<std::string>& exclude_keys) const;

        /**
         * @brief Reconcile the BSSD frame across a handover boundary (epoch k
         *        to k+1) for the backward step.
         *
         * Compares the per-epoch pivot maps.  For each constellation whose
         * pivot changed:
         *   - CLEAN handover (recorded in @c sk1.clean_handovers, old/new
         *     pivots matching): inverse-references each re-referenced ambiguity
         *     signal group of @p x_adj / @p P_adj (copies of the k+1 smoothed
         *     estimate) back to the OLD pivot — exactly undoing the forward
         *     re-reference so the estimate re-enters epoch k's frame.  Orphaned
         *     signal groups (the new pivot lacked the signal) are segmented.
         *   - Otherwise (disturbed / forced / appeared-or-vanished): the whole
         *     constellation's ambiguities are segmented.
         * Ionosphere and satellite-bias states of any handover constellation
         * are always segmented (their re-references are not inverted here).
         *
         * @param sk    Epoch k snapshot (its pivot map is the OLD datum).
         * @param sk1   Epoch k+1 snapshot (NEW datum + clean-handover record).
         * @param[in,out] x_adj  Copy of the k+1 smoothed state (inverse-referenced in place).
         * @param[in,out] P_adj  Copy of the k+1 smoothed covariance (inverse-referenced in place).
         * @return Dynamic-state keys to exclude from the common subspace.
         */
        std::set<std::string> prepareHandoverFrame(
            const SmootherEpochSnapshot& sk,
            const SmootherEpochSnapshot& sk1,
            Eigen::VectorXd& x_adj,
            Eigen::MatrixXd& P_adj) const;
    };

} // namespace GnssProcessor

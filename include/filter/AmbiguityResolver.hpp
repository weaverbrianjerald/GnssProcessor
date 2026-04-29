/**
 * @file AmbiguityResolver.hpp
 * @brief LAMBDA/MLAMBDA integer ambiguity resolution for PPP-RTK.
 *
 * Implements the Modified LAMBDA (MLAMBDA) method for integer least-squares
 * estimation of carrier-phase ambiguities.  The resolver operates on the
 * between-satellite single-difference (SD) ambiguity sub-vector extracted
 * from the Kalman filter state, resolves integer values, and optionally
 * constrains the float solution via a fix-and-hold pseudo-observation update.
 *
 * References:
 *   [1] P.J.G. Teunissen, "The least-squares ambiguity decorrelation
 *       adjustment: a method for fast GPS ambiguity estimation",
 *       J. Geodesy, 70, 65-82, 1995.
 *   [2] X.-W. Chang, X. Yang, T. Zhou, "MLAMBDA: A modified LAMBDA method
 *       for integer least-squares estimation", J. Geodesy, 79, 552-565, 2005.
 */
#pragma once

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "filter/StateManager.hpp"
#include "common/PppTypes.hpp"
#include <Eigen/Dense>
#include <string>
#include <vector>
#include <map>
#include <set>

namespace GnssProcessor {

/**
 * @brief Result of an integer ambiguity resolution attempt.
 */
struct AmbiguityResolution {
    bool success{ false };             ///< True if ambiguities were successfully fixed.
    double ratio{ 0.0 };               ///< Ratio test value (s2/s1).  Higher = more confident.
    int num_fixed{ 0 };                ///< Number of SD ambiguities fixed.
    int num_candidates{ 0 };           ///< Number of SD ambiguity candidates.
    Eigen::VectorXd fixed_state;       ///< Full state vector with fixed ambiguities applied.
    Eigen::MatrixXd fixed_covariance;  ///< Covariance after ambiguity fixing.
};

/**
 * @brief Configuration parameters for ambiguity resolution.
 */
struct AmbiguityResolverConfig {
    double ratio_threshold{ 3.0 };     ///< Minimum ratio test threshold for acceptance.
    double elevation_mask_ar{ 20.0 };  ///< Elevation mask for AR candidates (degrees).
    double hold_variance{ 0.001 };     ///< Pseudo-observation variance for fix-and-hold (m^2).
    int    ar_mode{ 0 };               ///< 0: float-only, 1: continuous, 2: instantaneous, 3: fix-and-hold.
};

/**
 * @brief MLAMBDA integer ambiguity resolver.
 *
 * Workflow:
 *   1. Extract the SD ambiguity sub-vector and sub-covariance from the filter.
 *   2. Form DD ambiguities using a per-constellation pivot (highest-elevation).
 *   3. Apply MLAMBDA (LDL decomposition + Z-decorrelation + ILS search).
 *   4. Ratio test: accept if s2/s1 >= threshold.
 *   5. Compute the fixed-ambiguity state and covariance.
 *   6. Optionally apply fix-and-hold constraints.
 */
class AmbiguityResolver {
public:
    /**
     * @brief Construct the resolver.
     * @param config    AR configuration parameters.
     * @param log_manager  Logging facility.
     */
    AmbiguityResolver(const AmbiguityResolverConfig& config, LogManager& log_manager);

    /**
     * @brief Attempt to resolve integer ambiguities.
     *
     * @param state_manager   Current filter state (provides ambiguity indices).
     * @param observations    Modeled observations for this epoch (for elevation, satellite info).
     * @param wl_ambiguities  Resolved SD WL integers from the MW detector.
     *                        Keys are "SAT_SIG1_SIG2" (e.g. "G01_L1C_L2W"),
     *                        values are the rounded SD WL integer.  When
     *                        available for a DD pair, a two-step WL/NL approach
     *                        is used: the WL is fixed from MW, and only the NL
     *                        is searched with LAMBDA, halving the search space.
     * @return AmbiguityResolution result.
     */
    AmbiguityResolution resolve(const StateManager& state_manager,
                                const std::vector<PppObservation>& observations,
                                const std::map<std::string, int>& wl_ambiguities);

    /**
     * @brief Attempt to resolve integer ambiguities (no WL constraint).
     */
    AmbiguityResolution resolve(const StateManager& state_manager,
                                const std::vector<PppObservation>& observations);

    /**
     * @brief Attempt to resolve integer ambiguities for baseline (DD) mode.
     *
     * Sister entry point to @c resolve() that consumes ambiguity states
     * keyed in DD form (@c "DD|cons|sat_i|rov_code|pivot") directly.
     * Unlike the PPP/PPP-RTK path, no SD subtraction is applied — the
     * state values are already DD-differenced floats in meters.  The
     * meter-valued floats are converted to integer cycles using the
     * per-pair carrier wavelength before MLAMBDA, then the integer fix
     * is multiplied back to meters for the back-substitution step.
     *
     * Only DDs whose four observations (rov_i, rov_p, ref_i, ref_p)
     * all use the same RINEX code are integer-cycle.  Anything else
     * carries a residual receiver IFB or satellite DCB term that
     * prevents the float from rounding to integer.  Those DD ambiguity
     * keys must be passed in @p exclude_keys and are skipped from the
     * candidate set.
     *
     * @param state_manager  Filter state holding DD ambiguity entries.
     * @param observations   Rover-side modeled observations (used for
     *                       elevation lookups; reference-side elevations
     *                       are nearly identical on short baselines).
     * @param exclude_keys   DD ambiguity keys to skip (typically
     *                       MeasurementModel::getNonIntegerCapableDdAmbiguityKeys()).
     * @return AmbiguityResolution; @c success=false when there are too
     *         few candidates, the ratio test fails, or MLAMBDA cannot
     *         decompose the covariance.
     */
    AmbiguityResolution resolveDd(const StateManager& state_manager,
                                  const std::vector<PppObservation>& observations,
                                  const std::set<std::string>& exclude_keys);

    /**
     * @brief Apply fix-and-hold constraints to the filter.
     *
     * Feeds back the fixed DD ambiguities as pseudo-observations to
     * constrain the float solution, preventing the ambiguities from
     * drifting away from their integer values.
     *
     * @param state_manager   Filter state to update (modified in place).
     * @param resolution      Result from a previous successful resolve() call.
     */
    void holdAmbiguities(StateManager& state_manager,
                         const AmbiguityResolution& resolution);

    /**
     * @brief Baseline-mode (DD) variant of @c holdAmbiguities.
     *
     * The state ambiguities are already DD-valued, so the
     * pseudo-observation matrix is a simple selector
     * (@c H(k, dd_idx)=1) rather than the (i,-1,ref) row used by the
     * PPP / PPP-RTK SD path.  Only ambiguities whose key starts with
     * @c "DD|" are eligible for the hold update.
     *
     * @param state_manager   Filter state to update (modified in place).
     * @param resolution      Result from a previous successful @c resolveDd() call.
     */
    void holdAmbiguitiesDd(StateManager& state_manager,
                           const AmbiguityResolution& resolution);

    /// Get the current AR configuration.
    const AmbiguityResolverConfig& getConfig() const { return config_; }

    /// Update the AR configuration.
    void setConfig(const AmbiguityResolverConfig& config) { config_ = config; }

private:
    // -----------------------------------------------------------------------
    // MLAMBDA core routines
    // -----------------------------------------------------------------------

    /**
     * @brief LDL^T decomposition of a positive-definite matrix.
     * @param Q  Input covariance matrix (n x n).
     * @param L  Output lower-triangular unit matrix.
     * @param d  Output diagonal vector.
     * @return True on success, false if matrix is not positive definite.
     */
    static bool ldlDecompose(const Eigen::MatrixXd& Q,
                             Eigen::MatrixXd& L, Eigen::VectorXd& d);

    /**
     * @brief MLAMBDA lattice reduction (decorrelation).
     * @param[in,out] L  Lower-triangular matrix (modified in place).
     * @param[in,out] d  Diagonal vector (modified in place).
     * @param[out]    Z  Integer transformation matrix.
     */
    static void reduction(Eigen::MatrixXd& L, Eigen::VectorXd& d,
                          Eigen::MatrixXd& Z);

    /**
     * @brief Integer least-squares search (modified Schnorr-Euchner).
     * @param L     Reduced lower-triangular matrix.
     * @param d     Reduced diagonal vector.
     * @param zs    Float DD ambiguity in Z-transformed space.
     * @param zn    Output: best m integer candidates (n x m).
     * @param sq    Output: squared distances for each candidate.
     * @param m     Number of candidates to find (default 2 for ratio test).
     * @return Number of candidates found.
     */
    static int search(const Eigen::MatrixXd& L, const Eigen::VectorXd& d,
                      const Eigen::VectorXd& zs,
                      Eigen::MatrixXd& zn, Eigen::VectorXd& sq, int m = 2);

    /**
     * @brief Full MLAMBDA: decompose, reduce, search, back-transform.
     * @param a     Float DD ambiguity vector.
     * @param Q     DD ambiguity covariance matrix.
     * @param afix  Output: best integer candidate.
     * @param s     Output: squared distances [s1, s2] for ratio test.
     * @return True on success.
     */
    static bool mlambda(const Eigen::VectorXd& a, const Eigen::MatrixXd& Q,
                        Eigen::VectorXd& afix, Eigen::VectorXd& s);

    // -----------------------------------------------------------------------
    // DD index management
    // -----------------------------------------------------------------------

    /// Information about one DD ambiguity pair.
    struct DdPair {
        int    state_idx_i;    ///< State index of the non-pivot SD ambiguity.
        int    state_idx_ref;  ///< State index of the pivot (reference) SD ambiguity.
        std::string sat_i;     ///< Non-pivot satellite ID.
        std::string sat_ref;   ///< Pivot satellite ID.
        std::string signal;    ///< Signal type (e.g. "L1C").
    };

    /**
     * @brief Build DD ambiguity pairs from the current state.
     *
     * For each (constellation, signal) group, selects the highest-elevation
     * satellite as pivot and pairs all other ambiguities against it.
     *
     * @param state_manager  Filter state.
     * @param observations   Observations for elevation info.
     * @return Vector of DD pairs.
     */
    std::vector<DdPair> buildDdPairs(const StateManager& state_manager,
                                     const std::vector<PppObservation>& observations) const;

    AmbiguityResolverConfig config_;
    LogManager& log_manager_;
    PppUtils ppp_utils_;   ///< For frequency/wavelength lookups in NL computation.

    /**
     * @brief Two-step WL/NL resolution using known WL integers.
     *
     * For each satellite pair that has DD pairs on two frequencies AND a
     * known DD WL integer, the (L1, L2) DD pair is transformed to the
     * NL domain.  LAMBDA runs on the NL-only set � unpaired DD
     * ambiguities without a matching second frequency or WL integer are
     * excluded to avoid contaminating the ratio test.  The fixed NL
     * integers are back-transformed to the original signal domain.
     *
     * @param pairs            DD pairs (from buildDdPairs).
     * @param x                Full state vector.
     * @param P                Full covariance matrix.
     * @param wl_ambiguities   Resolved SD WL integers from MW detector.
     * @param observations     Observations (for system/frequency info).
     * @return AmbiguityResolution result, or empty if the method is not applicable.
     */
    AmbiguityResolution resolveWlConstrained(
        const std::vector<DdPair>& pairs,
        const Eigen::VectorXd& x,
        const Eigen::MatrixXd& P,
        const std::map<std::string, int>& wl_ambiguities,
        const std::vector<PppObservation>& observations);
};

} // namespace GnssProcessor

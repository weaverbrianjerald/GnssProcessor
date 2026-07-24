#pragma once

/**
 * @file OtlField.hpp
 * @brief Ocean-tidal-loading coefficients that VARY ALONG the vessel track.
 *
 * Motivation — why this class exists at all
 * ----------------------------------------
 * The processor synthesizes its OTL coefficients ONCE, at the RINEX
 * @c APPROX @c POSITION @c XYZ, and reuses that frozen 11-constituent set for the
 * whole arc (@c Model::Geometry::loadData); only the ENU->ECEF rotation follows
 * the moving receiver.  @c EphemerisProvider::loadOceanTide deliberately mirrors
 * that: it loads the SAME coefficients at the SAME point, so the injected term
 * and the modeled term are bit-identical and OTL cancels EXACTLY even though the
 * vessel moves hundreds of kilometres.  That is the right default — it keeps the
 * "reuse so it cancels" invariant that every other M7 correction relies on.
 *
 * It is also physically wrong.  Ocean loading is a FIELD: the EOT20 load grid
 * gives a different complex amplitude at every point, so a vessel under way
 * samples a continuously changing OTL signal.  A processor that freezes the
 * coefficients at the start of the arc therefore cannot model what a moving
 * receiver actually feels, and the difference is a real, unmodelled,
 * position-correlated error.
 *
 * This class synthesizes that field.  It samples the genuine EOT20 coefficients
 * at anchor points spaced along the truth trajectory and interpolates between
 * them, so @ref displacementEcef returns the loading at where the vessel IS
 * rather than at where it STARTED.  The residual left in the data is exactly
 *
 *     d_unmodelled(t) = d(x(t), t) - d(x0, t)
 *
 * which is zero at the first epoch and grows as the vessel leaves x0.
 *
 * Why interpolate, and why in the complex plane
 * ---------------------------------------------
 * @c OceanTideModel::parseEot20Load re-opens the 286 MB grid on every call, so
 * sampling it per epoch is wasteful; the load tide is also far smoother than the
 * 0.125 deg (~14 km) grid it is stored on, so anchors every few km are exact to
 * well under the noise floor.
 *
 * The blend MUST happen in the complex (FES/EOT) plane, never on the BLQ
 * (amplitude, Greenwich phase lag) pair that @c OtlStationCoefficients stores.
 * Phase is circular: blending +179 deg with -179 deg arithmetically yields 0 deg
 * — a half-cycle error, the worst possible answer, and one that appears only in
 * the handful of segments that happen to straddle the wrap.  Converting back to
 * @c z = A*exp(-i*phi) first makes the blend linear and wrap-free, and linear
 * blending of z is also the physically correct operation: the loading response
 * is linear in the complex load coefficients, so interpolating z and evaluating
 * is identical to evaluating at the anchors and interpolating the displacement.
 *
 * Evaluation reuses the processor's own @c OceanTideModel — the same Doodson
 * arguments, nodal corrections and (Up, West, South)->ECEF rotation — so the ONLY
 * thing that differs from the frozen path is the coefficient set being fed in.
 */

#include <filesystem>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "corrections/OceanTideModel.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp" // GnssTime

namespace GnssProcessor {
namespace sim {

/**
 * @brief Blend two BLQ-style coefficient sets in the complex plane.
 *
 * Converts each constituent/component from (amplitude, Greenwich phase lag) to
 * the EOT/FES complex form @c z = re + i*im with @c re = A*cos(phi),
 * @c im = -A*sin(phi) (the inverse of the @c A = |z|, @c phi = atan2(-im, re)
 * used by @c OceanTideModel::parseEot20Load), blends linearly, and converts back.
 *
 * @param a Coefficient set at fraction 0.
 * @param b Coefficient set at fraction 1.
 * @param f Blend fraction; clamped to [0, 1].
 * @return Blended coefficients.  @c f == 0 reproduces @p a and @c f == 1
 *         reproduces @p b bit-for-bit (the round trip is skipped at the
 *         endpoints, so anchors are exact).
 */
OtlStationCoefficients lerpOtlCoefficients(const OtlStationCoefficients& a,
                                           const OtlStationCoefficients& b,
                                           double f);

/**
 * @brief EOT20 ocean-loading coefficients sampled along a trajectory.
 *
 * Build once from the truth track, then query per epoch.  Not copyable (it owns
 * an evaluator bound to a @c LogManager reference).
 *
 * @note The query methods are non-const and mutate the internal evaluator and memo,
 *       so the object is neither reentrant nor thread-safe.  @c EphemerisProvider
 *       reaches it through a @c unique_ptr from a @c const method (const does not
 *       propagate through the pointer), matching how it already treats @c geometry_.
 */
class OtlField {
public:
    /** @brief One point of the truth trajectory used to place anchors. */
    struct TrackSample {
        GnssTime time{};                                 ///< Epoch (week + tow).
        Eigen::Vector3d ecef{ Eigen::Vector3d::Zero() }; ///< Truth receiver ECEF (m).
    };

    /** @brief An EOT20 sample: the coefficients synthesized at one track point. */
    struct Anchor {
        GnssTime time{};
        /// Monotone lookup key = week*604800 + tow.  Plain tow would run backwards at a
        /// week rollover mid-arc and silently clamp the whole remainder to one anchor.
        double gps_s{ 0.0 };
        Eigen::Vector3d ecef{ Eigen::Vector3d::Zero() };
        OtlStationCoefficients coeffs{};
    };

    explicit OtlField(LogManager& log);

    OtlField(const OtlField&) = delete;
    OtlField& operator=(const OtlField&) = delete;

    /**
     * @brief Sample the EOT20 grid along @p track and install the anchor set.
     *
     * Anchors are placed at the first sample, at every @p anchor_spacing_km of
     * travelled distance thereafter, and at the last sample.  A stationary track
     * therefore yields a single anchor, which reproduces the frozen behaviour
     * exactly.
     *
     * @param eot20_nc          Path to EOT20_load.nc.
     * @param track             Truth trajectory, ascending in @c tow.
     * @param anchor_spacing_km Distance between anchors (km); must be > 0.
     * @param frozen_site       The site the PROCESSOR will freeze its own
     *                          coefficients at (the RINEX APPROX POSITION).
     *                          Sampled separately so @ref frozenDisplacementEcef
     *                          reports the modeled term without assuming it
     *                          coincides with @c track.front().
     * @return True if every anchor (and the frozen site) was synthesized.
     */
    bool build(const std::filesystem::path& eot20_nc,
               const std::vector<TrackSample>& track,
               double anchor_spacing_km,
               const Eigen::Vector3d& frozen_site);

    /** @brief True if @ref build succeeded and the field can be queried. */
    bool ready() const { return !anchors_.empty() && frozen_.valid; }

    /** @brief The anchors, in track order (diagnostics / probe output). */
    const std::vector<Anchor>& anchors() const { return anchors_; }

    /** @brief The frozen coefficient set the processor will use. */
    const OtlStationCoefficients& frozenCoefficients() const { return frozen_; }

    /**
     * @brief Coefficients for the vessel at (@p t, @p rx).
     *
     * @p t brackets the anchor pair (time is monotone even where the track doubles
     * back); @p rx sets the blend fraction WITHIN that pair, since anchors are placed
     * by distance and a time fraction would misplace the vessel wherever speed is not
     * constant.  Clamped outside the anchor span.  Memoized on the epoch, so the
     * per-satellite calls within one epoch cost one blend, not one per observation.
     */
    const OtlStationCoefficients& coefficientsAt(const GnssTime& t, const Eigen::Vector3d& rx);

    /**
     * @brief Loading displacement (ECEF, m) at where the vessel actually is.
     * @param rx Truth receiver ECEF (m) — also supplies the ENU->ECEF rotation.
     * @param t  Epoch.
     */
    Eigen::Vector3d displacementEcef(const Eigen::Vector3d& rx, const GnssTime& t);

    /**
     * @brief Loading displacement (ECEF, m) from the FROZEN coefficients.
     *
     * This is what the processor models.  Same rotation point as
     * @ref displacementEcef, so the difference of the two is precisely the
     * unmodelled along-track term.
     */
    Eigen::Vector3d frozenDisplacementEcef(const Eigen::Vector3d& rx, const GnssTime& t);

private:
    /// Evaluate @p coeffs at (@p rx, @p t) through the processor's own model.
    Eigen::Vector3d evaluate(const OtlStationCoefficients& coeffs,
                             const Eigen::Vector3d& rx, const GnssTime& t);

    LogManager& log_;
    OceanTideModel model_;          ///< Scratch evaluator; coefficients pushed per query.
    std::vector<Anchor> anchors_;   ///< Ascending in tow.
    OtlStationCoefficients frozen_; ///< Coefficients at the RINEX APPROX POSITION.

    // One-entry memo for coefficientsAt(), keyed on the FULL argument pair.
    bool memo_valid_ = false;
    double memo_gps_s_ = 0.0;
    Eigen::Vector3d memo_rx_{ Eigen::Vector3d::Zero() };
    OtlStationCoefficients memo_coeffs_{};
};

} // namespace sim
} // namespace GnssProcessor

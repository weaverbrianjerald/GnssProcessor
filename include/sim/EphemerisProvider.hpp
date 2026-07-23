#pragma once

/**
 * @file EphemerisProvider.hpp
 * @brief Truth satellite orbit/clock source for the simulator (precise SP3/CLK).
 *
 * Wraps the GnssProcessor product chain validated by the M2 spike
 * (`gnss_simulator probe-orbit`): a private @c ProductParser ->
 * @c ProductCache -> @c Model::Geometry, set up exactly as
 * @c Application::loadInputData does it.  Exposes the two queries the
 * observation synthesizer needs — interpolated satellite ECEF position/velocity
 * and the precise satellite clock — at an arbitrary GPS epoch.
 *
 * Per DESIGN.md §5.4 the truth orbit/clock come from the precise SP3/CLK (the
 * same products the verifier reads), so they cancel by construction and the
 * exam difficulty lives entirely in the estimated states + noise + the storm.
 * The clock query reports @c broadcast_grade so synthesis can assert the
 * precise clock is in use (a broadcast-grade clock would make the processor
 * drop carrier phase).
 */

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp" // GnssTime

namespace GnssProcessor {

class ProductParser;
class RinexParser;
class Troposphere;
class Ionosphere;
class Antenna;
struct AntennaData;
namespace Model {
class Geometry;
}

namespace sim {

/**
 * @brief Provides truth satellite position/velocity/clock from precise products.
 *
 * Construction performs the full chain setup; check @c ok() before use.  The
 * object owns its config, logger, parser, and geometry, so it must outlive any
 * query.  Not copyable.
 */
class EphemerisProvider {
public:
    /**
     * @brief Build the provider from precise products.
     * @param setup_obs   A valid RINEX observation file used only to satisfy
     *                    @c Geometry's @c RinexParser dependency (receiver
     *                    metadata; orbit/clock queries do not consult it).
     * @param product_paths SP3 + CLK (and optionally ATX/BIA/ERP) file paths.
     * @param log_dir      Directory for the (warning-level) log.
     */
    EphemerisProvider(const std::filesystem::path& setup_obs,
                      const std::vector<std::filesystem::path>& product_paths,
                      const std::filesystem::path& log_dir,
                      const std::filesystem::path& nav_path = {});
    ~EphemerisProvider();

    EphemerisProvider(const EphemerisProvider&) = delete;
    EphemerisProvider& operator=(const EphemerisProvider&) = delete;

    /** @brief True if the product chain set up successfully. */
    bool ok() const { return ok_; }

    /** @brief Number of SP3 orbit nodes loaded into the shared cache. */
    std::size_t orbitNodeCount() const;
    /** @brief Number of CLK clock samples loaded into the shared cache. */
    std::size_t clockSampleCount() const;
    /** @brief Number of ERP (polar-motion / UT1) entries loaded into the shared cache. */
    std::size_t erpNodeCount() const;

    /**
     * @brief Interpolated satellite ECEF position/velocity at @p t.
     * @return False if no valid orbit covers @p t for @p sat.
     */
    bool positionVelocity(const std::string& sat, const GnssTime& t,
                          Eigen::Vector3d& pos, Eigen::Vector3d& vel) const;

    /**
     * @brief Precise satellite clock bias (seconds) at @p t.
     * @param broadcast Optional out-param: set true if the clock is only
     *        broadcast-grade (no precise CLK / SSR backing).
     * @return Clock bias in seconds (0 if unavailable).
     */
    double clock(const std::string& sat, const GnssTime& t, bool* broadcast = nullptr) const;

    /**
     * @brief Saastamoinen zenith hydrostatic delay (m) at the receiver.
     *
     * Uses the processor's own @c Troposphere model with the same config, so the
     * ZHD matches the value the verifier subtracts (cancels by construction).
     */
    double tropoZhd(const Eigen::Vector3d& rx, const GnssTime& t) const;

    /**
     * @brief GMF hydrostatic/wet mapping functions at a satellite elevation.
     * @param elev_deg Satellite elevation (degrees).
     */
    void tropoMapping(double elev_deg, const Eigen::Vector3d& rx, const GnssTime& t,
                      double& mf_d, double& mf_w) const;

    /** @brief True if a broadcast Klobuchar field (GPSA/GPSB) was loaded. */
    bool hasKlobuchar() const { return has_klobuchar_; }

    /**
     * @brief Broadcast Klobuchar slant ionospheric delay (m) at carrier @p freq_hz.
     *
     * Reuses the processor's own @c Ionosphere::computeKlobucharDelay with the real
     * broadcast GPSA/GPSB coefficients, so the quiet background the simulator injects
     * is exactly what a single-frequency student removes with Klobuchar (it cancels;
     * only the storm survives).  Returns 0 if no Klobuchar field is available.
     */
    double klobucharDelayMetres(const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                                const GnssTime& t, double freq_hz) const;

    // --- M7 full-correction stack (reuse-so-it-cancels; see SimConfig::Corrections).
    // Each returns the SAME metres the processor's PppModel::computeBaseModeledValue
    // uses (verified sign), so ADDING it to the synthesized code+phase and enabling
    // the matching model_corrections.enable_* flag cancels it back to the truth.

    /** @brief Relativistic periodic clock term as a range delay (m): -c*dtr, where
     *  dtr = computeRelativisticCorrection at the EMISSION-time (pre-Sagnac) state. */
    double relativityMetres(const std::string& sat, const Eigen::Vector3d& sat_pos_em,
                            const Eigen::Vector3d& sat_vel_em) const;

    /** @brief Shapiro gravitational path delay (m, >=0), reception-frame sat pos. */
    double shapiroMetres(const std::string& sat, const Eigen::Vector3d& rx,
                         const Eigen::Vector3d& sat_pos) const;

    /** @brief IERS 2010 solid-earth tide LOS displacement (m), using the geometry
     *  pipeline's OWN cached Sun/Moon so it cancels bit-for-bit. */
    double solidEarthTideMetres(const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                                const GnssTime& t) const;

    /** @brief Pole-tide site displacement projected onto the LOS (m); 0 if no ERP. */
    double poleTideMetres(const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                          const GnssTime& t) const;

    /** @brief True if an ATX was loaded and the satellite-antenna model stood up. */
    bool hasAntenna() const { return has_antenna_; }

    /**
     * @brief IGS ANTEX satellite antenna PCO/PCV correction (m) for one signal.
     *
     * Reuses the processor's own @c Antenna::getAntennaCorrection with ONLY the
     * satellite terms active (this provider's config disables receiver PCO/PCV and
     * phase wind-up — all three default @c true), so the return is exactly
     * @c (-sat_pco + sat_pcv) projected on the sat->rx LOS.  The processor ADDS the
     * identical scalar to its modeled value (@c PppModel::computeBaseModeledValue,
     * "+ antenna_corr", alongside Shapiro/solid-tide), so ADDING this to the
     * synthesized code+phase and enabling @c model_corrections.enable_satellite_pco
     * + @c enable_satellite_pcv cancels it back to truth — the same "reuse so it
     * cancels" pattern as the other M7 terms.  Satellite PCO/PCV are identical for
     * code and phase (wind-up is off), so one call per band serves both.  Returns 0
     * when no ATX was loaded.
     *
     * @param sat      Satellite id ("G05"/"E11"); its first char selects the system.
     * @param code     RINEX signal code being emitted (e.g. "C1C", "L1C", "C5Q").
     * @param rx       Receiver ECEF position (m).
     * @param sat_pos  RECEPTION-frame (Sagnac-rotated) satellite ECEF (m) — matches
     *                 the processor's @c geom.sat_pos.
     * @param sat_vel  EMISSION-frame satellite ECEF velocity (m/s), unrotated —
     *                 matches the processor's @c geom.sat_vel (used for the
     *                 yaw-steering attitude frame only).
     * @param elev_deg,az_deg Receiver topocentric elevation/azimuth (degrees).
     */
    double satelliteAntennaMetres(const std::string& sat, const std::string& code,
                                  const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                                  const Eigen::Vector3d& sat_vel,
                                  double elev_deg, double az_deg, const GnssTime& t) const;

private:
    GnssConfig config_;
    LogManager log_;
    std::unique_ptr<ProductParser> product_parser_;
    std::unique_ptr<RinexParser> rinex_;
    std::unique_ptr<Model::Geometry> geometry_;
    std::unique_ptr<Troposphere> troposphere_;
    std::unique_ptr<Ionosphere> ionosphere_;
    std::unique_ptr<Antenna> antenna_;
    // Antenna binds a RAW reference into the ProductCache singleton's current antenna
    // vector, which a later clearCache()/reload would free.  Only one provider lives
    // per process today, but this shared handle pins the data for antenna_'s lifetime
    // so the "single live provider" invariant is not silently load-bearing.
    std::shared_ptr<const std::vector<AntennaData>> antennas_hold_;
    bool ok_ = false;
    bool has_klobuchar_ = false;
    bool has_antenna_ = false;
};

} // namespace sim
} // namespace GnssProcessor

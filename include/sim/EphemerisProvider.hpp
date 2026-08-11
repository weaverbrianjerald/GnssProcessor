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
#include "sim/OtlField.hpp"   // along-track ocean-loading field (opt-in)

namespace GnssProcessor {

class ProductParser;
class RinexParser;
class Troposphere;
class Ionosphere;
class Antenna;
class GlobalIonosphereMap;
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
    /// @param rx_antenna_type RINEX "ANT # / TYPE" receiver antenna (16-char model +
    ///        4-char radome, e.g. "TRM59800.00     NONE").  EMPTY (default) leaves the
    ///        receiver PCO/PCV terms DISABLED, so @ref antennaMetres returns the
    ///        satellite half only.  When non-empty the receiver terms are enabled and
    ///        the antenna must exist in the loaded ATX.
    EphemerisProvider(const std::filesystem::path& setup_obs,
                      const std::vector<std::filesystem::path>& product_paths,
                      const std::filesystem::path& log_dir,
                      const std::filesystem::path& nav_path = {},
                      const std::string& rx_antenna_type = {});
    ~EphemerisProvider();

    EphemerisProvider(const EphemerisProvider&) = delete;
    EphemerisProvider& operator=(const EphemerisProvider&) = delete;

    /** @brief True if the product chain set up successfully. */
    bool ok() const { return ok_; }

    /**
     * @brief The provider's LogManager.
     *
     * spdlog registers loggers by process-global name, so a second LogManager in
     * the same process fails to initialize; any additional parser this process
     * stands up (e.g. spp-reference's RinexParser over the emitted hero RINEX)
     * must reuse this one instead of creating its own.
     */
    LogManager& logManager() { return log_; }

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

    /**
     * @brief Load a CODE/IGS IONEX (.INX) as the quiet-background source.
     *
     * Reuses the processor's own @c GlobalIonosphereMap, so the STEC the simulator
     * injects is bit-for-bit the value that code path computes: pierce point on the
     * IONEX shell (the map's OWN height — 450 km for CODE, not the storm field's 350),
     * bilinear VTEC in lat/lon, sun-fixed longitude rotation in time, then the map's
     * thin-shell mapping function.  Unlike the Klobuchar background this does NOT
     * cancel against a student's broadcast correction — that residual is the point.
     * @return true if the IONEX parsed and the map stood up.
     */
    bool loadGim(const std::filesystem::path& inx_path);

    /** @brief True if an IONEX was parsed and the GIM is usable. */
    bool hasGim() const { return gim_ != nullptr; }

    /**
     * @brief Slant quiet-background STEC (TECU) from the loaded GIM.
     *
     * @warning The returned STEC is ALREADY SLANT (the map applies its own obliquity
     * internally).  Callers must NOT multiply by IonoSample::obliquity or 1/sin(el) —
     * doing so inflates the background by up to ~3x at a 10 deg mask, and NO existing
     * gate catches it (the iono-free combination is blind to the background because it
     * cancels, and the Verifier never reads truth_atmo.csv).
     *
     * @param[out] ipp_lat_deg,ipp_lon_deg  GIM pierce point (deg); longitude wrapped to
     *        (-180, 180] because the map returns it unwrapped and this vessel sits on
     *        the dateline.  These are the map's GEOCENTRIC IPP on ITS shell, so they
     *        differ from IonoSample's geodetic IPP on the 350 km storm shell — both are
     *        published in truth_atmo.csv rather than reconciled.
     * @return false if no GIM is loaded or the query is invalid / non-finite.
     */
    bool gimSlantStec(const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                      const GnssTime& t,
                      double& stec_tecu, double& std_tecu,
                      double& ipp_lat_deg, double& ipp_lon_deg) const;

    /**
     * @brief IONEX map-epoch coverage [first, last], for the arc-containment guard.
     *
     * Epochs outside this span are silently CLAMPED to the endpoint map by
     * GlobalIonosphereMap — and still longitude-rotated, so an arc running past the
     * last map receives a map rotated by up to 90 deg with no warning.  Silently wrong
     * TRUTH, which is why the caller must fail rather than trust it.
     * @return false if no GIM is loaded or it carries no epochs.
     */
    bool gimEpochSpan(GnssTime& first, GnssTime& last) const;

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

    /**
     * @brief Synthesize ocean-tidal-loading coefficients from the EOT20 global load grid.
     *
     * Must be called ONCE, before any @ref oceanTideMetres call, with the SAME site the
     * processor will use: it loads coefficients at @p site_ecef exactly as
     * @c Model::Geometry::loadData does from the RINEX @c APPROX POSITION XYZ.  Because
     * both sides then evaluate the same frozen 11-constituent coefficient set, the term
     * cancels EXACTLY even for a moving vessel.
     * @return true if the grid was read and coefficients were installed.
     */
    bool loadOceanTide(const std::filesystem::path& eot20_nc, const Eigen::Vector3d& site_ecef);

    /**
     * @brief Install an ocean-loading field that VARIES ALONG the track.
     *
     * The alternative to @ref loadOceanTide: instead of freezing the coefficients at
     * @p frozen_site the way the processor does, this samples the real EOT20 field at
     * anchors along @p track and interpolates between them, so the injected loading is
     * the one at the vessel's ACTUAL position at each epoch.  The processor still
     * models the frozen set, so the term deliberately does NOT cancel — the residual
     * @c d(x(t),t) @c - @c d(x0,t) is a genuine unmodelled error that grows with
     * along-track distance.  Call at most one of the two loaders.
     *
     * @param eot20_nc          Path to EOT20_load.nc.
     * @param track             Truth trajectory (tow + ECEF), ascending in tow.
     * @param anchor_spacing_km EOT20 sampling interval along the track (km).
     * @param frozen_site       Site the processor freezes ITS coefficients at (the
     *                          RINEX APPROX POSITION), used for the truth diagnostics.
     * @return true if the field was built.
     */
    bool loadOceanTideAlongTrack(const std::filesystem::path& eot20_nc,
                                 const std::vector<OtlField::TrackSample>& track,
                                 double anchor_spacing_km,
                                 const Eigen::Vector3d& frozen_site);

    /** @brief True if ocean-tidal-loading coefficients are installed. */
    bool hasOceanTide() const { return has_ocean_tide_; }

    /** @brief True if the OTL being injected varies along the track (does NOT cancel). */
    bool hasOceanTideAlongTrack() const { return otl_field_ != nullptr; }

    /** @brief Number of EOT20 anchors in the along-track field (0 if frozen). */
    std::size_t oceanTideAnchorCount() const;

    /**
     * @brief The ocean-loading site displacement the sim is INJECTING (ECEF, m).
     *
     * Along-track when a field is installed, frozen otherwise.  Truth bookkeeping
     * only — the observation model goes through @ref oceanTideMetres.
     */
    Eigen::Vector3d oceanTideDisplacementEcef(const Eigen::Vector3d& rx, const GnssTime& t) const;

    /**
     * @brief The ocean-loading site displacement the PROCESSOR will MODEL (ECEF, m).
     *
     * Always the frozen APPROX-POSITION coefficients, rotated at @p rx.  The
     * difference from @ref oceanTideDisplacementEcef is precisely what the run leaves
     * unmodelled; it is identically zero unless an along-track field is installed.
     */
    Eigen::Vector3d oceanTideModeledEcef(const Eigen::Vector3d& rx, const GnssTime& t) const;

    /**
     * @brief Ocean tidal loading site displacement projected on the LOS (m).
     *
     * Identical in form to @ref poleTideMetres: the processor computes
     * @c otl_corr = otl_ecef.dot(-los) (ModelGeometry) and ADDS it to the modeled value
     * (@c PppModel::computeBaseModeledValue "+ geom.otl_corr"), so ADDING this to the
     * synthesized code+phase and enabling @c model_corrections.enable_ocean_tide_loading
     * cancels it back to truth.  Returns 0 when no coefficients are loaded.
     */
    double oceanTideMetres(const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                           const GnssTime& t) const;

    /** @brief True if an ATX was loaded and the satellite-antenna model stood up. */
    bool hasAntenna() const { return has_antenna_; }

    /**
     * @brief IGS ANTEX antenna PCO/PCV correction (m) for one signal — satellite
     *        always, PLUS the receiver half when a receiver antenna type was given.
     *
     * Reuses the processor's own @c Antenna::getAntennaCorrection, which composes ONE
     * scalar from the individually-gated terms:
     * @c (-sat_pco + sat_pcv) + (+rx_pco + rx_pcv), each projected on the sat->rx LOS
     * (the satellite APC is closer than the CoM so its PCO SHORTENS the range, hence
     * the minus; the receiver PCO ADDS to the modeled range).  Phase wind-up stays off.
     *
     * With an EMPTY @c rx_antenna_type this provider disables receiver PCO/PCV, so the
     * return is exactly @c (-sat_pco + sat_pcv) — the historical satellite-only
     * behaviour.  With a type set, the receiver terms are enabled and included.
     *
     * The processor ADDS the identical scalar to its modeled value
     * (@c PppModel::computeBaseModeledValue, "+ antenna_corr", alongside
     * Shapiro/solid-tide), so ADDING this to the synthesized code+phase and enabling
     * the MATCHING @c model_corrections.enable_satellite_pco/_pcv (and
     * @c enable_receiver_pco/_pcv when a receiver antenna is used) cancels it back to
     * truth — the same "reuse so it cancels" pattern as the other M7 terms.  All four
     * terms are identical for code and phase (wind-up is off), so one call per band
     * serves both.  Returns 0 when no ATX was loaded.
     *
     * @note The receiver PCV is azimuth-dependent in the ANTENNA frame.  Both sides
     *       assume a level, north-referenced antenna (the processor's rx-orientation
     *       option defaults off ⇒ raw topocentric azimuth), so a yawing hull is NOT
     *       modelled; the azimuthal term is mm-level while the dominant
     *       elevation-dependent part cancels exactly.
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
    double antennaMetres(const std::string& sat, const std::string& code,
                         const Eigen::Vector3d& rx, const Eigen::Vector3d& sat_pos,
                         const Eigen::Vector3d& sat_vel,
                         double elev_deg, double az_deg, const GnssTime& t) const;

    /** @brief True if a receiver antenna type was supplied (receiver PCO/PCV injected). */
    bool hasReceiverAntenna() const { return !rx_antenna_type_.empty(); }

    /** @brief True if a Bias-SINEX (.BIA) OSB product was loaded into the shared cache. */
    bool hasSignalBias() const { return has_signal_bias_; }

    /**
     * @brief Observable-specific signal bias (OSB) for one signal (m).
     *
     * Reuses the processor's own @c GnssCorrections::getBiasCorrection — the SAME
     * lookup @c PppModel::computeBaseModeledValue performs — so the value returned is
     * bit-for-bit the code/phase hardware bias the processor ADDS to its modeled value
     * ("+ bias_corr", alongside antenna/Shapiro/solid-tide) when
     * @c model_corrections.enable_code_phase_bias is on.  Therefore ADDING this to the
     * synthesized code (for @p code == "C1C"/"C2W"/…) and to the synthesized phase
     * (for @p code == "L1C"/"L2W"/…) makes the run config's @c enable_code_phase_bias
     * cancel it back to truth — the same "reuse so it cancels" pattern as the M7 terms.
     *
     * Unlike the antenna term the OSB is DIFFERENT for the code and phase observables of
     * the same band (e.g. C1C ≠ L1C), so it must be queried once per RINEX code, not
     * once per band.  The value is a per-(satellite, observable) constant over the
     * product's validity window (one UTC day for the COD MGX OSB), keyed on the exact
     * RINEX code string.
     *
     * PEDAGOGY: the precise satellite clock is referenced to a specific ionosphere-free
     * observable pair (COD MGX: GPS C1W/C2W, Galileo C1C/C5Q — @c BIAS/DESCRIPTION
     * @c SATELLITE_CLOCK_REFERENCE_OBSERVABLES).  Because the receiver logs GPS C1C (not
     * the clock's C1W), the C1C OSB carries a real per-satellite differential that a
     * PPP ignoring OSBs leaves in the ionosphere-free code as a ~decimetre bias; Galileo
     * emits its own clock-reference signals, so its OSB IF combination is ≈0.
     *
     * @param sat   Satellite id ("G05"/"E11"); matched verbatim against the product PRN.
     * @param code  RINEX observable code being emitted (e.g. "C1C", "L1C", "C2W", "L5Q").
     * @return The OSB in metres, or 0 when no .BIA was loaded, no @c geometry_, or the
     *         product has no entry for this (satellite, observable) in the epoch window.
     */
    double signalBiasMetres(const std::string& sat, const std::string& code,
                            const GnssTime& t) const;

    /**
     * @brief Carrier-phase wind-up (m) for one band, ACCUMULATED mode (M10 wind-up).
     *
     * Reuses the processor's own @c GnssCorrections::computePhaseWindUp on the
     * geometry pipeline's instance, with the geometry's cached Sun for the
     * yaw-steering attitude and the emission-frame @p sat_vel as fallback — the
     * exact inputs @c PppModel routes through @c Antenna::getAntennaCorrection —
     * and @c accumulate=true, so the injected angle is continuous (unwrapped)
     * within each tracked arc and the phase ambiguities remain time-constant
     * integers under a wind-up-aware estimator.  The per-satellite unwrap state
     * lives on the geometry's @c GnssCorrections instance; nothing else calls
     * @c computePhaseWindUp during synthesis, so the state is exclusively ours.
     * The angle is band-common in cycles: call once per band and the same-epoch
     * guard inside @c computePhaseWindUp reuses the first call's accumulation.
     *
     * @param sat          Satellite id (e.g. "G01"); unknown prefixes return 0.
     * @param rx           Receiver ECEF truth position (m).
     * @param sat_pos      Reception-frame satellite ECEF position (m).
     * @param sat_vel      EMISSION-frame satellite ECEF velocity (m/s).
     * @param wavelength_m Carrier wavelength for this band (m).
     * @param t            Epoch.
     * @return Wind-up in metres for this band (add to the phase observable; the
     *         processor ADDS the same value to its modeled phase when
     *         @c enable_phase_windup + @c accumulate_phase_windup are set).
     */
    double phaseWindupMetres(const std::string& sat, const Eigen::Vector3d& rx,
                             const Eigen::Vector3d& sat_pos, const Eigen::Vector3d& sat_vel,
                             double wavelength_m, const GnssTime& t) const;

private:
    GnssConfig config_;
    LogManager log_;
    std::unique_ptr<ProductParser> product_parser_;
    std::unique_ptr<RinexParser> rinex_;
    std::unique_ptr<Model::Geometry> geometry_;
    std::unique_ptr<Troposphere> troposphere_;
    std::unique_ptr<Ionosphere> ionosphere_;
    std::unique_ptr<Antenna> antenna_;
    /// Quiet-background IONEX map; null unless loadGim() succeeded.  Deliberately NOT
    /// added to the config's product_paths: that would change the provider's product
    /// set and risk perturbing existing product resolution (byte-identity risk).
    std::unique_ptr<GlobalIonosphereMap> gim_;
    // Antenna binds a RAW reference into the ProductCache singleton's current antenna
    // vector, which a later clearCache()/reload would free.  Only one provider lives
    // per process today, but this shared handle pins the data for antenna_'s lifetime
    // so the "single live provider" invariant is not silently load-bearing.
    std::shared_ptr<const std::vector<AntennaData>> antennas_hold_;
    std::string rx_antenna_type_;   ///< RINEX ANT # / TYPE; empty => receiver terms off.
    /// Along-track ocean-loading field; null = the frozen (cancelling) coefficients in
    /// geometry_'s corrections are used instead.  Declared after log_ so it is destroyed
    /// before the LogManager it holds a reference to.
    std::unique_ptr<OtlField> otl_field_;
    bool has_ocean_tide_ = false;   ///< OTL coefficients installed (EOT20).
    bool ok_ = false;
    bool has_klobuchar_ = false;
    bool has_antenna_ = false;
    bool has_signal_bias_ = false;  ///< A .BIA OSB product was parsed into the shared cache.
};

} // namespace sim
} // namespace GnssProcessor

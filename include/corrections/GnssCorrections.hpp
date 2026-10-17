#pragma once

/**
 * @file GnssCorrections.hpp
 * @brief Satellite and geodetic correction utilities used by GNSS processing.
 *
 * This header declares the GnssCorrections class which encapsulates common
 * corrections applied to GNSS measurements and navigation products:
 * - Satellite clock interpolation and relativistic corrections
 * - Sagnac and Shapiro path delays
 * - Tide and loading corrections (solid earth, pole tide, ocean, atmospheric)
 * - Bias retrieval and phase wind-up calculation
 *
 * The class is constructed with optional product data (clocks, ERPs, biases,
 * orbits) and may reference configuration and RINEX parser objects for
 * behaviour that depends on runtime settings or observation metadata.
 */

#include "logging/LogManager.hpp"
#include "math/TimeUtils.hpp"
#include "common/PppTypes.hpp"
#include "corrections/OceanTideModel.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <unordered_map>
#include <memory>
#include <vector>
#include <Eigen/Dense>
#include <string>

namespace GnssProcessor {

    // Forward declarations to avoid header cycles
    class GnssConfig;
    class RinexParser;
    class SsrCorrectionStore;
    class BrdcClockApriori;

    /**
     * @class GnssCorrections
     * @brief Compute and provide GNSS-related corrections for measurements.
     *
     * GnssCorrections provides a central place to compute corrections that are
     * frequently required when forming GNSS measurement models (e.g. for PPP).
     * Implementations typically depend on externally parsed product files such
     * as CLK (satellite clocks), ERP (Earth rotation parameters), orbit
     * products, and optional bias files. A LogManager reference is required
     * for diagnostic logging.
     */
    class GnssCorrections {

    public:
        /**
         * @brief Minimal constructor without product data.
         * @param log_mgr Reference to the LogManager used for logging.
         *
         * Use this constructor when product files are not available but logging
         * is still required.
         */
        explicit GnssCorrections(LogManager& log_mgr);

        /**
         * @brief Construct with clocks and ERPs.
         * @param log_mgr Reference to the LogManager used for logging.
         * @param clocks Vector of satellite clock samples (ClockData).
         * @param erps Vector of Earth rotation parameter samples (ErpData).
         *
         * Commonly used by product/geometry parsers which already have clocks
         * and ERP loaded at construction time.
         */
        GnssCorrections(LogManager& log_mgr,
            const std::vector<ClockData>& clocks,
            const std::vector<ErpData>& erps);

        /**
         * @brief Full constructor with optional biases and RINEX access.
         * @param log_mgr Reference to the LogManager used for logging.
         * @param config Reference to the processing configuration (may be used
         *               to control correction options).
         * @param clocks Vector of satellite clock samples (ClockData).
         * @param erps Vector of Earth rotation parameter samples (ErpData).
         * @param biases Vector of bias entries (BiasData).
         * @param rinex Reference to a RinexParser for receiver/observation metadata.
         *
         * Use when full product context is available and some corrections
         * depend on configuration or RINEX header information.
         */
        GnssCorrections(LogManager& log_mgr,
            const GnssConfig& config,
            std::shared_ptr<const std::vector<ClockData>> clocks,
            std::shared_ptr<const std::vector<ErpData>> erps,
            std::shared_ptr<const std::vector<BiasData>> biases,
            const RinexParser& rinex);

        /**
         * @brief Update clocks and ERPs after construction.
         * @param clocks Vector of satellite clock samples (ClockData).
         * @param erps Vector of Earth rotation parameter samples (ErpData).
         *
         * Allows replacing or refreshing time-dependent product containers
         * without recreating the GnssCorrections object.
         */
        void setClocksAndErps(const std::vector<ClockData>& clocks, const std::vector<ErpData>& erps);

        /**
         * @brief Inject a non-owning SSR-correction store override (dependency
         *        injection point for the PPP-RTK closed-loop harness).
         *
         * By default the SSR clock overlay in @c getSatelliteClockCorrection
         * reads the process-wide @c SsrCorrectionStore singleton.  The network
         * closed-loop consumer points this at the store the producer owns, so a
         * held-out station consumes the clock the network has just emitted
         * without writing to global state.  Pass @c nullptr (the default) to
         * restore the singleton; callers outside the harness are then
         * byte-identical.
         *
         * @param store Non-owning store pointer; must outlive this object, or
         *        @c nullptr to use the singleton.
         */
        void setSsrStore(const SsrCorrectionStore* store) { ssr_store_override_ = store; }

        /**
         * @brief Declare the store the consumer's SSR orbit overlay reads from,
         *        for the orbit-coherence echo check.
         *
         * A @c brdc_rts network clock record echoes the identity (IOD-SSR and
         * content epoch) of the RTS orbit the producer estimated it against
         * (conventions §2).  @c getSatelliteClockCorrection checks that echo
         * against the orbit record the consumer applies.  Under the RTA-8 split
         * routing that record lives in a different store from the clock: the
         * orbit on the external-RTS singleton, the clock on the injected
         * network-product store.  This setter names the orbit store.
         * @c Model::Geometry::setSsrOrbitStore and @c PppModel::setSsrOrbitStore
         * forward to it, so it follows the orbit overlay's routing.  Pass
         * @c nullptr (the default) for the process-wide singleton, which is
         * also the orbit overlay's default.
         *
         * Consulted only when a clock record carries an echo and its merge does
         * not already contain the matching orbit part (a combined record is
         * self-coherent).  A stream without echo fields never reads it.
         *
         * @param store Non-owning store pointer; must outlive this object, or
         *        @c nullptr to use the singleton.
         */
        void setOrbitEchoStore(const SsrCorrectionStore* store) { orbit_echo_store_override_ = store; }

        /**
         * @brief Take the a priori satellite clock from the native broadcast clock
         *        a priori instead of a CLK product (@c network.apriori_source
         *        @c sp3_brdc_clock; producer stations only).
         *
         * When set and no CLK sample covers the epoch (the mode refuses a loaded CLK, so
         * none does), @c getSatelliteClockCorrection returns a_s(t) = brdc_k(t) + O_s,k
         * from @p apriori and does not consult the live store or an SSR record: it is the
         * producer's declared a priori, the value the bed's CLK product carried, so it is
         * not flagged broadcast-grade (the network estimates the clock against it).  A
         * satellite without an eligible record returns 0 and is dropped, as a satellite
         * missing from a CLK product.  @c nullptr (the default) leaves the method
         * byte-identical.
         *
         * @param apriori Non-owning pointer (must outlive this object), or @c nullptr.
         */
        void setBroadcastClockApriori(const BrdcClockApriori* apriori) { brdc_apriori_ = apriori; }

        /**
         * @brief Get satellite clock correction in seconds.
         * @param satellite Satellite identifier string (e.g. "G01", "E11").
         * @param time Epoch at which the correction is requested.
         * @param[out] broadcast_grade Optional.  Set true when the returned clock
         *        came from the broadcast polynomial with neither a precise CLK
         *        sample nor an SSR clock delta, so it is only metre-level
         *        accurate.  Carrier phase is not usable against such a clock (the
         *        ambiguity cannot be separated from the product error), so callers
         *        use this flag to fall back to code-only for that satellite.  False
         *        when a precise CLK sample or a fresh, IODE-matched SSR clock delta
         *        backs the value, and on the input-error paths.
         * @return Clock correction in seconds.
         *
         * The baseline is the precise CLK product (see interpolateClock()); when
         * no CLK sample covers the epoch, the live broadcast clock is used
         * instead.  A fresh, IODE-matched SSR clock delta is added on top when
         * one is available.
         */
        double getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time,
            bool* broadcast_grade = nullptr) const;

        /**
         * @brief Interpolate clock values from clock samples.
         * @param satellite Satellite identifier string.
         * @param time Epoch for interpolation.
         * @param clocks Container of ClockData from which to interpolate.
         * @return Interpolated clock value in seconds; 0 when no valid sample
         *         covers the epoch.
         *
         * The method is set by @c processing.satellite_clock_interpolation_mode:
         * Linear (two-node bracket; the default, and the mode used when no
         * configuration is attached), Lagrange (bracket-centred polynomial) or
         * LsqSatClock (weighted LSQ over a multi-node window, which falls back to
         * a simpler method when too few valid nodes are available).
         */
        double interpolateClock(const std::string& satellite, const GnssTime& time,
            const std::vector<ClockData>& clocks) const;

        /**
         * @brief Compute relativistic clock correction due to satellite orbit.
         * @param satellite Satellite identifier string.
         * @param sat_pos Satellite ECEF position (meters).
         * @param sat_vel Satellite ECEF velocity (meters/second).
         * @return Relativistic correction in seconds.
         *
         * Accounts for the classical relativistic effect from the satellite's
         * orbital motion and eccentricity as used in GNSS clock modelling.
         */
        double computeRelativisticCorrection(const std::string& satellite, const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sat_vel) const;

        /**
         * @brief Compute the Shapiro (relativistic path) delay.
         * @param satellite Satellite identifier string (used only in log messages).
         * @param rx_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @return Shapiro delay in meters.
         *
         * The Shapiro delay is the additional path length caused by space-time
         * curvature in the Earth's gravity field:
         * (2·GM/c²)·ln((r_sat + r_rx + r_sr)/(r_sat + r_rx − r_sr)), with GM the
         * Earth's gravitational parameter.
         */
        double computeShapiroDelay(const std::string& satellite, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos) const;

        /**
         * @brief Compute Sagnac correction (rotation of the Earth).
         * @param rx_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @return Sagnac correction in meters.
         *
         * Accounts for signal path difference caused by Earth's rotation
         * between transmit and receive events.
         */
        double computeSagnacCorrection(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos) const;

        /**
         * @brief Compute pole tide correction in ECEF coordinates.
         * @param rx_pos Receiver ECEF position (meters).
         * @param time Epoch for ERP/pole values.
         * @return ECEF displacement vector (meters) due to pole tide.
         *
         * Applies pole tide displacement to station coordinates using ERP data.
         */
        Eigen::Vector3d computePoleTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        /**
         * @brief Ocean pole tide load displacement (IERS 2010 Sec. 7.1.5, Desai 2002).
         *
         * Closed-form site displacement driven by the ocean's equilibrium
         * response to the centrifugal pole-tide potential, evaluated from the
         * station's six interpolated load coefficients and the current polar
         * motion wobble.  Returned in the local @b East/@b North/@b Up frame
         * (metres), to be summed onto the Sec. 7.1.4 rotational-deformation pole
         * tide before rotation into ECEF.  Exposed as a static method so it can
         * be validated directly against an external reference vector
         * (see OceanPoleTideTest / scripts/ocean_pole_tide_reference.py)
         * independently of the grid interpolation and ERP pipeline.
         *
         * @param m1_arcsec  Polar-motion wobble m1 = xp - xp_mean   (arcseconds).
         * @param m2_arcsec  Polar-motion wobble m2 = -(yp - yp_mean) (arcseconds).
         * @param opt_coeffs Six per-station load coefficients ordered
         *        [u_n^R, u_n^I, u_e^R, u_e^I, u_r^R, u_r^I] (IERS opole grid
         *        North/East/radial real & imaginary parts).
         * @return (East, North, Up) displacement in metres.
         */
        static Eigen::Vector3d computeOceanPoleTideDisplacement(
            double m1_arcsec, double m2_arcsec, const std::array<double, 6>& opt_coeffs);

        /**
         * @brief Compute solid Earth tide correction (LOS displacement).
         * @param receiver_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @param sun_pos Sun ECEF position (meters).
         * @param moon_pos Moon ECEF position (meters).
         * @param time Epoch for Step 2 frequency-dependent corrections (GnssTime: week + tow).
         * @return LOS displacement in meters to apply to the geometric range.
         *
         * Implements the IERS 2010 solid Earth tide displacement model
         * (Section 7.1.1) producing **conventional tide-free** coordinates,
         * consistent with IGS products (SP3/CLK/ITRF). Internally a faithful
         * port of the IERS dehanttideinel routine (Step 1 in-phase degree 2/3,
         * Step 1 out-of-phase diurnal/semidiurnal + L1, Step 2 diurnal
         * Table 7.3a and long-period Table 7.3b); the permanent tide is
         * retained (not removed), matching the IGS/ITRF convention.
         */
        double computeSolidEarthTide(const Eigen::Vector3d& receiver_pos, const Eigen::Vector3d& sat_pos,
            const Eigen::Vector3d& sun_pos, const Eigen::Vector3d& moon_pos,
            const GnssTime& time) const;

        /**
         * @brief IERS 2010 solid Earth tide station displacement (ECEF vector).
         *
         * Faithful port of the IERS Conventions dehanttideinel routine,
         * returning the geocentric ECEF displacement (meters) of the station.
         * Exposed as a static method so it can be validated directly against
         * the official IERS dehanttideinel test vector (see SolidEarthTideTest)
         * independently of the ephemeris and the line-of-sight projection.
         *
         * @param xSta Station ECEF position (meters).
         * @param xSun Sun ECEF position (meters).
         * @param xMon Moon ECEF position (meters).
         * @param jc   Julian centuries from J2000 (TT≈UT at tide precision),
         *             including the intra-day fraction.
         * @param fhr  UT fractional hour of day (supplies the diurnal τ term).
         * @return Station displacement vector in ECEF (meters).
         */
        static Eigen::Vector3d computeSolidEarthTideDisplacement(
            const Eigen::Vector3d& xSta, const Eigen::Vector3d& xSun,
            const Eigen::Vector3d& xMon, double jc, double fhr);

        /**
         * @brief Compute ocean tide correction (ECEF displacement).
         * @param rx_pos Receiver ECEF position (meters).
         * @param time Epoch for tide constituents.
         * @return ECEF displacement vector (meters) caused by ocean loading/tides.
         *
         * Ocean tide correction is often modeled as frequency-dependent loading
         * using ocean tide models and load Love numbers.
         */
        Eigen::Vector3d computeOceanTideCorrection(const Eigen::Vector3d& rx_pos, const GnssTime& time) const;

        /**
         * @brief Compute the atmospheric pressure loading (APL) site displacement
         *        from the S1/S2 thermal-tide model (ECEF displacement vector).
         * @param rx_pos Receiver ECEF position (meters).
         * @param time  Epoch when the loading is evaluated.
         * @return ECEF displacement vector (meters); zero if no S1/S2 coefficients
         *         are loaded for this station.
         *
         * Models the diurnal (S1) and semidiurnal (S2) atmospheric thermal tides,
         * the sun-synchronous part of atmospheric pressure loading.  Unlike the
         * synoptic (non-tidal) component, S1/S2 do not average out over a static
         * daily PPP arc: they alias a coherent millimetre-level radial bias into
         * the daily mean height, which is why this component is implemented first.
         * The non-tidal/synoptic APL (gridded 6-hourly pressure or a precomputed
         * displacement series) is not implemented.
         *
         * Per component c in {East, North, Up}, with local-solar-time arguments
         *   theta_S1 = 2*pi*UT_frac + lambda,  theta_S2 = 2*(2*pi*UT_frac) + 2*lambda
         * (lambda = station longitude, UT_frac = fractional part of the MJD,
         * computed from GPS time):
         *   d_c = aS1_c^cos*cos(theta_S1) + aS1_c^sin*sin(theta_S1)
         *       + aS2_c^cos*cos(theta_S2) + aS2_c^sin*sin(theta_S2)
         * assembled as ENU and rotated to ECEF (mirrors OceanTideModel).
         */
        Eigen::Vector3d computeAtmosphericTideDisplacement(const Eigen::Vector3d& rx_pos,
                                                           const GnssTime& time) const;

        /**
         * @brief Load per-station S1/S2 atmospheric-tide load coefficients by
         *        bilinear interpolation of a global grid at the station.
         * @param grid_path     Path to the atmospheric-tide load grid (Ray & Ponte
         *                       / IERS Conventions 2010 Sec. 7.1 atmospheric tidal
         *                       loading; CM frame to match OTL).
         * @param station_ecef  Receiver ECEF position (meters).
         * @return True if the grid parsed and coefficients were stored.
         *
         * @warning The header-line count, column order and frame of the
         *          atmospheric-tide grid are product-specific and must be pinned
         *          to the supplied file before production use (see the provisional
         *          grid constants in the .cpp).  Off by default:
         *          model_corrections.enable_atmospheric_loading defaults to false.
         */
        bool loadAtmosphericTideFromGrid(const std::string& grid_path,
                                         const Eigen::Vector3d& station_ecef);

        /**
         * @brief Directly set the S1/S2 atmospheric-tide load coefficients.
         * @param coeffs 12 coefficients: indices 0..5 = S1, 6..11 = S2; within
         *        each harmonic [aE^cos, aE^sin, aN^cos, aN^sin, aU^cos, aU^sin] (m).
         *
         * Primarily for testing and for callers (e.g. a SIF block) that already
         * hold per-station coefficients without a grid.
         */
        void setAtmosphericTideCoefficients(const std::array<double, 12>& coeffs);

        /**
         * @brief Check whether S1/S2 atmospheric-tide coefficients are available.
         * @return True if coefficients have been set or interpolated.
         */
        bool hasAtmosphericTideCoefficients() const { return has_atm_tide_coeffs_; }

        /**
         * @brief Get bias correction for a given satellite/signal at an epoch.
         * @param satellite Satellite identifier or receiver id string.
         * @param freq_type Frequency or signal type string (e.g. "L1", "C1").
         * @param time Epoch for which the bias applies.
         * @return Bias correction in meters (or zero if no matching bias).
         *
         * Returns the first entry in product order whose satellite and signal
         * match and whose [start, end] validity interval contains @p time, so
         * with overlapping entries the earlier one wins.
         */
        double getBiasCorrection(const std::string& satellite, const std::string& freq_type,
            const GnssTime& time) const;

        /**
         * @brief Check whether the BIA product contains any entry for a satellite+signal pair.
         *
         * Looks the pair up in a set (O(log n)) built from the bias entries.  It
         * does not check time validity, only whether the pair is indexed.  The
         * index leaves out zero-valued code-bias entries and all GLONASS
         * code-bias entries, so for those pairs this returns false and the
         * per-(satellite, signal) sat-bias state absorbs the delay instead
         * (with estimate_sat_biases=true).
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @param signal    RINEX observation code (e.g. "C1W", "L5Q").
         * @return True if at least one indexed BiasData entry matches the pair.
         */
        bool hasBiasEntry(const std::string& satellite, const std::string& signal) const;

        /**
         * @brief Compute phase wind-up correction for carrier-phase.
         * @param satellite Satellite identifier string.
         * @param system Satellite system enum (System).
         * @param rx_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @param wavelength Carrier wavelength in meters.
         * @param time Epoch for any time-dependent quantities.
         * @param sun_pos Sun ECEF position (meters).  When supplied (norm above
         *        1e6 m), the satellite attitude follows the yaw-steering model
         *        (Sun direction and nadir).  When zero, the attitude is built from
         *        the velocity instead.  BeiDou-2 GEO satellites (C01-C05, C59,
         *        C60) use the orbit-normal frame in either case.
         * @param sat_vel Satellite ECEF velocity (meters/second).  Used for the
         *        attitude when sun_pos is not supplied, when the Sun direction is
         *        collinear with nadir (yaw-steering degenerate), and for the
         *        BeiDou-2 GEO orbit normal.  There is no separate eclipse handling.
         * @param accumulate When true, unwrap against the previous epoch so the
         *        applied correction is continuous (required for IAR).  When false
         *        (the loader default, estimation.accumulate_phase_windup) the raw
         *        @c atan2 angle is applied, which steps one full cycle at each
         *        half-cycle crossing; @ref enableWindUpCensus counts those steps.
         * @param max_gap_s Accumulator continuity horizon (seconds).  A gap larger
         *        than this, or an out-of-order epoch, restarts accumulation from
         *        the raw angle, which applies an exact integer-cycle step to every
         *        signal of that satellite.  This horizon is independent of
         *        @c network.phase_leveling_max_gap_s, which is longer in the golden
         *        configurations (900 s).  A gap in (max_gap_s,
         *        phase_leveling_max_gap_s] therefore steps the model while the
         *        carried ambiguity and the zero-q leveling level survive and are
         *        re-injected at their pre-gap values (standing invariant #1).  The
         *        wired response is @c network.cycle_slip.windup_restart_refloat
         *        (off by default; see @ref wuRestarts), and the census counts the
         *        gaps in that interval.
         * @param correct_velocity_frame When true, the BeiDou-2 GEO orbit normal
         *        is built from the inertial velocity instead of the ECEF velocity.
         * @return Phase wind-up correction in meters (to add to carrier range).
         *
         * Computes the phase bias caused by relative antenna orientation
         * (receiver vs satellite) and returns the equivalent length offset.
         */
        double computePhaseWindUp(const std::string& satellite, System system, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, double wavelength, const GnssTime& time,
            const Eigen::Vector3d& sun_pos = Eigen::Vector3d::Zero(),
            const Eigen::Vector3d& sat_vel = Eigen::Vector3d::Zero(),
            bool accumulate = false,
            double max_gap_s = 300.0,
            bool correct_velocity_frame = false) const;

        /**
         * @brief Enable CSV tracing of per-satellite phase wind-up diagnostics.
         *
         * Opens @c windup_trace.csv in @p log_dir.  Once enabled,
         * @c computePhaseWindUp() writes one row per (satellite, epoch) with
         * the wrapped atan2 angle, the post-accumulation angle, the implied
         * cycle count, the applied metres, the inter-epoch gap and the
         * accumulation-restart flag.  Intended for diagnosing whether the
         * accumulated (unwrapped) mode tracks geometry correctly.
         *
         * @param log_dir Directory where the trace file will be created.
         * @return True if the trace file was opened successfully.
         */
        bool enableWindUpTracing(const std::filesystem::path& log_dir);

        /**
         * @brief Check whether the phase wind-up CSV trace is active.
         * @return True if @c windup_trace.csv is open and writable.
         */
        bool isWindUpTracingEnabled() const { return windup_trace_.is_open(); }

        /**
         * @brief Set station-specific ocean tidal loading coefficients.
         * @param coeffs OTL station coefficients (11 constituents).
         *
         * When set, computeOceanTideCorrection() uses the full 11-constituent
         * model instead of the approximate fallback.
         */
        void setOceanTideCoefficients(const OtlStationCoefficients& coeffs);

        /**
         * @brief Switch the ocean tide nodal corrections and the UTC Greenwich argument.
         * @param on True to apply the nodal factors and take the Greenwich argument from
         *           UTC (see @c OceanTideModel::setNodalCorrections); false keeps the
         *           original evaluation bit for bit.
         */
        void setOceanTideNodalCorrections(bool on);

        /**
         * @brief Load OTL coefficients from a BLQ file for a station.
         * @param blq_path Path to the BLQ file.
         * @param station_name Station name to search for.
         * @return True if coefficients were found and loaded.
         */
        bool loadOceanTideFromBLQ(const std::string& blq_path, const std::string& station_name);

        /**
         * @brief Synthesize OTL coefficients from the EOT20 global load grid.
         *
         * Bilinearly interpolates the EOT20_load.nc complex height coefficients
         * at the station position and converts them to BLQ-style amplitude and
         * Greenwich phase lag for the 11 BLQ constituents. Provides station-
         * specific OTL for any location on Earth without per-station BLQ files.
         *
         * @param nc_path       Path to EOT20_load.nc.
         * @param station_ecef  Receiver ECEF position (meters).
         * @return True if synthesis succeeded; on failure, the model retains
         *         whatever coefficients (or absence thereof) were set previously.
         */
        bool loadOceanTideFromEot20(const std::string& nc_path,
                                    const Eigen::Vector3d& station_ecef);

        /**
         * @brief Check whether station-specific OTL coefficients are available.
         * @return True if OTL coefficients have been loaded.
         */
        bool hasOceanTideCoefficients() const;

        /**
         * @brief Set the six per-station ocean pole tide load coefficients.
         * @param coeffs Coefficients ordered [u_n^R, u_n^I, u_e^R, u_e^I,
         *        u_r^R, u_r^I] (North/East/radial real & imaginary parts), as
         *        produced by the IERS opole grid.
         *
         * Once set, @c computePoleTideCorrection() adds the Sec. 7.1.5 ocean
         * pole tide term on top of the Sec. 7.1.4 rotational-deformation term.
         */
        void setOceanPoleTideCoefficients(const std::array<double, 6>& coeffs);

        /**
         * @brief Interpolate ocean pole tide load coefficients from the IERS grid.
         *
         * Bilinearly interpolates the global 0.5x0.5-degree Desai opole load grid
         * (@c opoleloadcoefcmcor.txt, IERS Conventions 2010 chapter 7) at the
         * station position and stores the resulting six coefficients.  Nothing is
         * loaded unless the caller invokes this (gated upstream by
         * @c enable_ocean_pole_tide), so default runs are byte-identical.
         *
         * @param grid_path     Path to @c opoleloadcoefcmcor.txt.
         * @param station_ecef  Receiver ECEF position (metres).
         * @return True if the grid parsed and coefficients were stored.
         */
        bool loadOceanPoleTideFromGrid(const std::string& grid_path,
                                       const Eigen::Vector3d& station_ecef);

        /**
         * @brief Check whether ocean pole tide coefficients are available.
         * @return True if OPT coefficients have been set or interpolated.
         */
        bool hasOceanPoleTideCoefficients() const { return has_opt_coeffs_; }

    private:
        LogManager& log_manager_;                         ///< Logger for debugging and info
        // Shared handles to the single cached copy in ProductCache: the read-only
        // clock/ERP/bias data is referenced, not copied per instance (per-station
        // memory).  Non-null by default so dereferencing is always safe; an SSR
        // consumer that overrides clocks rebinds its own shared_ptr.
        std::shared_ptr<const std::vector<ClockData>> clocks_{std::make_shared<const std::vector<ClockData>>()};  ///< Satellite clock samples (shared).
        std::shared_ptr<const std::vector<ErpData>>   erps_{std::make_shared<const std::vector<ErpData>>()};       ///< Earth rotation parameters (shared).
        std::shared_ptr<const std::vector<BiasData>>  biases_{std::make_shared<const std::vector<BiasData>>()};    ///< Bias entries (shared).
        std::shared_ptr<const std::vector<OrbitData>> orbits_{std::make_shared<const std::vector<OrbitData>>()};   ///< Orbit records (shared).  Never populated; read only by the wind-up velocity fallback.

        // Optional external references (nullable pointers)
        const GnssConfig* config_;                        ///< Optional processing configuration (may be nullptr)
        const RinexParser* rinex_parser_;                 ///< Optional reference to a RinexParser (may be nullptr)
        const SsrCorrectionStore* ssr_store_override_{ nullptr }; ///< Non-owning SSR store override; nullptr -> process-wide singleton (see setSsrStore).
        const SsrCorrectionStore* orbit_echo_store_override_{ nullptr }; ///< Non-owning orbit-overlay store for the echo check; nullptr -> singleton (see setOrbitEchoStore).
        const BrdcClockApriori* brdc_apriori_{ nullptr }; ///< Non-owning native broadcast clock a priori; nullptr -> the CLK / live-store path (see setBroadcastClockApriori).

        /// Resolve the active SSR store: the injected override when set, else the
        /// process-wide singleton.  Defined in the .cpp where the full type is visible.
        const SsrCorrectionStore& ssrStore() const;

        /// Resolve the orbit-overlay store for the echo check: the override when
        /// set, else the singleton.  Defined in the .cpp beside ssrStore().
        const SsrCorrectionStore& orbitEchoStore() const;

        std::unordered_map<std::string, std::vector<const ClockData*>> clock_index_;
        void buildClockIndex();

        std::set<std::string> bias_signals_;   ///< Fast lookup: "satellite|signal" keys from BIA
        void buildBiasIndex();

        // Phase wind-up state per satellite (for continuity between epochs).
        // Stores the accumulated angle (radians) and the GPS seconds of the
        // last update so that stale entries can be discarded after data gaps.
        struct WindUpState {
            double angle{ 0.0 };      ///< Accumulated wind-up angle (radians).
            double epoch_s{ 0.0 };    ///< Continuous GPS seconds of last update.
        };
        // Ownership contract: the key is the satellite alone, so this map is
        // correct only while the owning GnssCorrections instance serves one
        // receiver.  Every production path satisfies this: each network station
        // owns its own PppModel (ApplicationNetworkSetup.cpp), which holds an
        // Antenna by value, which holds its GnssCorrections by value, and the
        // only caller that reaches the wind-up path is the Antenna's
        // receiver/satellite correction assembly, behind an `is_phase` gate.
        // (ModelGeometry's emission-time refinement calls the same Antenna with
        // code signals only; that gate keeps those calls out of both the
        // accumulator and the census.)  An instance shared across receivers would
        // interleave their arcs in accumulated mode, with no error raised, and
        // the unwrap reference would become another station's angle.  The
        // simulator's shared-provider fan-out runs one process per station for
        // this reason.  Re-key to (station, satellite) before sharing an instance.
        mutable std::unordered_map<std::string, WindUpState> prev_windup_;

    public:
        /**
         * @brief Per-instance (that is, per-receiver) phase wind-up census counters.
         *
         * Armed by @ref enableWindUpCensus and populated from
         * @c computePhaseWindUp in both accumulation postures, fractional and
         * accumulated.  Read-only telemetry: arming it does not change the
         * solve.  A transition is a change of processed epoch for one satellite;
         * repeated calls within an epoch are deduplicated.
         */
        struct WindUpCensus {
            long long steps{0};          ///< Forward transitions (gap > 0) between consecutive processed epochs: the denominator (@ref backwards excluded).
            /// Transitions with |Delta applied| >= 0.9 cyc across an in-horizon gap:
            /// the fractional staircase.  Fractional posture only: in accumulated
            /// mode an in-horizon transition passes through the unwrap, which by
            /// construction confines the step to [-0.5, +0.5] cyc, so this counter
            /// is identically zero there and a zero does not mean "no restarts".
            /// The restart count is @ref restarts.
            long long wraps{0};
            /// Accumulator restarts, taken from the accumulator's own decision
            /// rather than from a threshold on the applied step.
            /// Zero in fractional mode (there is no accumulator to restart).
            long long restarts{0};
            long long gap_steps{0};      ///< |Delta applied| >= 0.9 cyc across an out-of-horizon gap.
            long long backwards{0};      ///< transitions with gap < 0 (epochs out of order); excluded from @ref steps and from every bucket.  The reference still advances, so they do not corrupt the next gap.
            long long gap_lt_wu{0};      ///< transitions with gap < the wind-up horizon (accumulation continues).
            /// Out-of-horizon transitions with gap up to the later of the two
            /// horizons, [wind-up horizon, max(wind-up, carry)].  When the carry
            /// horizon exceeds the wind-up horizon, these are the gaps across
            /// which the model steps while the leveling carry survives
            /// (invariant #1).  It does on the golden configurations (900 s
            /// against 300 s) but not at the loader default
            /// (@c network.phase_leveling_max_gap_s defaults to 120 s, below the
            /// wind-up horizon); with that inverted ordering the bucket holds only
            /// a gap exactly equal to the wind-up horizon and is in practice
            /// empty.  Filled only when a carry horizon above 0 was passed to
            /// @ref enableWindUpCensus.  The teardown prints the ordering it
            /// observed and omits the exposure reading when the ordering does not
            /// hold, so a structural zero is not reported as a clean result.
            long long gap_wu_to_carry{0};
            long long gap_gt_carry{0};   ///< out-of-horizon transitions beyond the later of the two horizons (the carry is dropped too, so no invariant-#1 exposure).  Filled only when a carry horizon above 0 was passed.
            double    max_abs_step_in_cyc{0.0};  ///< largest |Delta applied| over in-horizon transitions (cycles).
            double    max_abs_step_gap_cyc{0.0}; ///< largest |Delta applied| over out-of-horizon transitions (cycles).
            double    sum_abs_step_cyc{0.0};     ///< Sigma |Delta applied| over in-horizon transitions (cycles).
        };

        /**
         * @brief Arm the wind-up census on this instance.
         *
         * @param wu_gap_s    the accumulator continuity horizon in force.
         * @param carry_gap_s the phase-leveling carry horizon (@c
         *        network.phase_leveling_max_gap_s), used only to bucket gaps so
         *        the invariant-#1 exposure between the two horizons is counted.
         *        Pass 0 to disable that bucketing (user path).
         */
        void enableWindUpCensus(double wu_gap_s, double carry_gap_s) {
            wu_census_on_ = true;
            wu_census_gap_s_ = wu_gap_s;
            wu_census_carry_gap_s_ = carry_gap_s;
        }
        /// @brief The census counters (all zero unless @ref enableWindUpCensus ran).
        const WindUpCensus& windUpCensus() const { return wu_census_; }

        /**
         * @brief One accumulator restart, recorded for the wired response
         *        (SPEC_phase_windup_sign §10.4: refloat the arcs the restart
         *        stepped).  @c gap_s is the gap that tripped the horizon
         *        (negative for out-of-order epochs, which take the same restart
         *        branch).  @c epoch_s stamps when the restart happened
         *        (continuous GPS seconds), so a consumer that missed an epoch
         *        (absent station) can refuse the stale entry instead of acting on
         *        an old restart.
         */
        struct WuRestart { double gap_s{0.0}; double epoch_s{0.0}; };
        /**
         * @brief The restart ledger: filled only in accumulated mode (fractional
         *        mode has no restart decision), keyed by satellite, overwritten
         *        per restart, and bounded by the visible constellation.  It does
         *        not change the correction itself.  The network response drain
         *        (@c network.cycle_slip.windup_restart_refloat) reads and clears
         *        it each epoch; with no consumer it holds the last restart per
         *        satellite.
         */
        const std::map<std::string, WuRestart>& wuRestarts() const { return wu_restarts_; }
        /// @brief Clear the restart ledger (called by the consumer's drain).
        void clearWuRestarts() { wu_restarts_.clear(); }

    private:
        /// Restart ledger, §10.4 (see @ref wuRestarts).  Written from the const
        /// computePhaseWindUp, hence mutable, as for the census.
        mutable std::map<std::string, WuRestart> wu_restarts_;
        bool   wu_census_on_{false};
        double wu_census_gap_s_{300.0};
        double wu_census_carry_gap_s_{0.0};
        mutable WindUpCensus wu_census_;
        /// Last applied wind-up in cycles per satellite, with its epoch.  This is
        /// the census's own state, kept separate from @c prev_windup_ so the
        /// census reads identically in both accumulation postures.  Same
        /// one-receiver-per-instance contract as @c prev_windup_ above.
        /// @c seeded is an explicit flag rather than an @c epoch_s == 0 sentinel:
        /// 0.0 is a legal continuous-GPS-second value, and overloading it would
        /// make a satellite at that epoch re-seed forever and never be counted.
        struct WuCensusState { double cycles{0.0}; double epoch_s{0.0}; bool seeded{false}; };
        mutable std::unordered_map<std::string, WuCensusState> wu_census_prev_;

        // Phase wind-up CSV diagnostic trace.  Opened on demand via
        // enableWindUpTracing(); written from the const computePhaseWindUp(),
        // hence mutable.  windup_trace_epoch_ tracks the last epoch traced per
        // satellite so multi-signal / multi-pass calls emit one row per epoch.
        mutable std::ofstream windup_trace_;
        mutable std::unordered_map<std::string, double> windup_trace_epoch_;

        OceanTideModel ocean_tide_model_;  ///< Ocean tidal loading model

        // Ocean pole tide (IERS 2010 Sec. 7.1.5) per-station load coefficients,
        // ordered [u_n^R, u_n^I, u_e^R, u_e^I, u_r^R, u_r^I].  Empty/false until
        // interpolated from the opole grid (or set directly); when absent
        // computePoleTideCorrection returns only the Sec. 7.1.4 term.
        std::array<double, 6> opt_coeffs_{};   ///< Ocean pole tide load coefficients.
        bool has_opt_coeffs_{ false };         ///< True once OPT coefficients are loaded.

        // Atmospheric pressure loading (S1/S2 thermal tide) per-station load
        // coefficients.  Indices 0..5 = S1 (diurnal), 6..11 = S2 (semidiurnal);
        // within each harmonic [aE^cos, aE^sin, aN^cos, aN^sin, aU^cos, aU^sin]
        // in metres.  Empty/false until interpolated from a grid or set directly;
        // when absent computeAtmosphericTideDisplacement returns zero.
        std::array<double, 12> atm_tide_coeffs_{};  ///< S1/S2 atmospheric-tide load coefficients.
        bool has_atm_tide_coeffs_{ false };         ///< True once S1/S2 coefficients are loaded.

    };

}
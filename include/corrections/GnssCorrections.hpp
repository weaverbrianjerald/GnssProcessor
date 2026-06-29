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
         *        injection seam for the PPP-RTK closed-loop harness).
         *
         * By default the SSR clock overlay in @c getSatelliteClockCorrection
         * reads the process-wide @c SsrCorrectionStore singleton.  The network
         * closed-loop consumer instead points this at the producer's OWNED store
         * so a held-out station consumes the network's freshly-emitted clock
         * without polluting global state.  Pass @c nullptr (the default) to
         * restore the singleton — every non-harness caller is then byte-identical.
         *
         * @param store Non-owning store pointer; must outlive this object, or
         *        @c nullptr to use the singleton.
         */
        void setSsrStore(const SsrCorrectionStore* store) { ssr_store_override_ = store; }

        /**
         * @brief Get satellite clock correction in seconds.
         * @param satellite Satellite identifier string (e.g. "G01", "E11").
         * @param time Epoch at which the correction is requested.
         * @param[out] broadcast_grade Optional. Set true when the returned clock
         *        came from the broadcast polynomial with NO precise CLK sample
         *        and NO SSR clock delta — i.e. it is only metre-level accurate.
         *        Carrier phase is meaningless against such a clock (the ambiguity
         *        cannot be separated from the product error), so callers use this
         *        to fall back to code-only for that satellite. Always false when a
         *        precise CLK product or a fresh IODE-matched SSR clock backs it.
         * @return Clock correction in seconds (bias + interpolation).
         *
         * This routine uses available CLK data to provide a best-effort clock
         * correction for the requested satellite and epoch.
         */
        double getSatelliteClockCorrection(const std::string& satellite, const GnssTime& time,
            bool* broadcast_grade = nullptr) const;

        /**
         * @brief Interpolate clock values from clock samples.
         * @param satellite Satellite identifier string.
         * @param time Epoch for interpolation.
         * @param clocks Container of ClockData from which to interpolate.
         * @return Interpolated clock value in seconds.
         *
         * Performs interpolation (or nearest-sample retrieval) using the
         * provided clock samples. Exact interpolation method (linear, spline)
         * is implementation-defined.
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
         * @param satellite Satellite identifier string (unused in pure geometric form).
         * @param rx_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @return Shapiro delay in meters.
         *
         * The Shapiro delay is the additional path length caused by the
         * curvature of spacetime near massive bodies (primarily the Sun).
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
         * synoptic (non-tidal) component, S1/S2 do NOT average out over a static
         * daily PPP arc — they alias a coherent ~mm radial bias into the daily
         * mean height, which is why this is the high-value v1 component.  The
         * non-tidal/synoptic APL (gridded 6-hourly pressure or a precomputed
         * displacement series) is deferred to a later phase.
         *
         * Per component c in {East, North, Up}, with local-solar-time arguments
         *   theta_S1 = 2*pi*UT_frac + lambda,  theta_S2 = 2*(2*pi*UT_frac) + 2*lambda
         * (lambda = station longitude, UT_frac = fractional MJD):
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
         * @warning The exact header-line count, column order, and frame of the
         *          atmospheric-tide grid are product-specific and must be pinned
         *          to the supplied file before enabling in production (see the
         *          PROVISIONAL constants in the .cpp).  Gated off by default via
         *          model_corrections.enable_atmospheric_loading.
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
         * Searches internal bias table for an applicable entry and returns the
         * bias value; behaviour on overlapping entries is implementation-defined.
         */
        double getBiasCorrection(const std::string& satellite, const std::string& freq_type,
            const GnssTime& time) const;

        /**
         * @brief Check whether the BIA product contains any entry for a satellite+signal pair.
         *
         * This is a fast O(1) lookup against a set built at construction time.
         * It does not check time validity  only whether the pair exists at all.
         *
         * @param satellite Satellite identifier (e.g. "G01").
         * @param signal    RINEX observation code (e.g. "C1W", "L5Q").
         * @return True if at least one BiasData entry matches the pair.
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
         * @param sun_pos Sun ECEF position (meters). When provided (non-zero), the
         *        satellite attitude is computed using the proper yaw-steering model
         *        (Sun direction + nadir). When zero, falls back to velocity-based attitude.
         * @param sat_vel Satellite ECEF velocity (meters/second). Used as fallback
         *        for attitude when sun_pos is not available or during eclipse/noon turn.
         * @return Phase wind-up correction in meters (to add to carrier range).
         *
         * Computes the phase bias caused by relative antenna orientation
         * (receiver vs satellite) and returns the equivalent length offset.
         */
        double computePhaseWindUp(const std::string& satellite, System system, const Eigen::Vector3d& rx_pos,
            const Eigen::Vector3d& sat_pos, double wavelength, const GnssTime& time,
            const Eigen::Vector3d& sun_pos = Eigen::Vector3d::Zero(),
            const Eigen::Vector3d& sat_vel = Eigen::Vector3d::Zero(),
            bool accumulate = false) const;

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
         * (@c opoleloadcoefcmcor.txt — IERS Conventions 2010 chapter 7) at the
         * station position and stores the resulting six coefficients.  Default
         * runs stay byte-identical: nothing is loaded unless the caller invokes
         * this (gated upstream by @c enable_ocean_pole_tide).
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
        // Shared handles to the single cached copy (ProductCache) — read-only
        // clock/ERP/bias data is REFERENCED, not duplicated per instance (the
        // per-station memory fix).  Non-null by default so deref is always safe;
        // an SSR consumer that overrides clocks rebinds its own shared_ptr.
        std::shared_ptr<const std::vector<ClockData>> clocks_{std::make_shared<const std::vector<ClockData>>()};  ///< Satellite clock samples (shared).
        std::shared_ptr<const std::vector<ErpData>>   erps_{std::make_shared<const std::vector<ErpData>>()};       ///< Earth rotation parameters (shared).
        std::shared_ptr<const std::vector<BiasData>>  biases_{std::make_shared<const std::vector<BiasData>>()};    ///< Bias entries (shared).
        std::shared_ptr<const std::vector<OrbitData>> orbits_{std::make_shared<const std::vector<OrbitData>>()};   ///< Orbit records (shared; currently unused/empty).

        // Optional external references (nullable pointers)
        const GnssConfig* config_;                        ///< Optional processing configuration (may be nullptr)
        const RinexParser* rinex_parser_;                 ///< Optional reference to a RinexParser (may be nullptr)
        const SsrCorrectionStore* ssr_store_override_{ nullptr }; ///< Non-owning SSR store override; nullptr -> process-wide singleton (see setSsrStore).

        /// Resolve the active SSR store: the injected override when set, else the
        /// process-wide singleton.  Defined in the .cpp where the full type is visible.
        const SsrCorrectionStore& ssrStore() const;

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
        mutable std::unordered_map<std::string, WindUpState> prev_windup_;

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
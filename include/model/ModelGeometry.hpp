#pragma once

/**
 * @file ModelGeometry.hpp
 * @brief Satellite/receiver geometry helper and GNSS geometric corrections.
 *
 * Declares the `GnssProcessor::Model::Geometry` class which computes satellite
 * geometry (position, velocity, line-of-sight, elevation/azimuth) and
 * related corrections used throughout the PPP processing stack.
 *
 * This header contains only declarations; implementation details live in
 * `src/core/model/ModelGeometry.cpp`.
 */

#include <Eigen/Dense>
#include "common/PppTypes.hpp"
#include "common/Constants.hpp"
#include "logging/LogManager.hpp"
#include "config/GnssConfig.hpp"
#include "products/ProductCache.hpp"
#include "rinex/RinexParser.hpp"
#include "corrections/GnssCorrections.hpp"
#include "model/Planetary.hpp"
#include "atmosphere/Troposphere.hpp"
#include <map>
#include <set>
#include <vector>
#include <string>
#include <unordered_map>

namespace GnssProcessor {

    // Forward declaration — Antenna lives at the GnssProcessor namespace
    // scope and is owned externally (typically by PppModel).  Geometry
    // holds only a non-owning pointer for emission-time pseudorange
    // refinement; it never constructs or destroys the Antenna instance.
    class Antenna;
    class SsrCorrectionStore;

    namespace Model {

    /**
     * @brief Simple container for topocentric coordinates.
     *
     * Elevation is given in degrees in the range [0, 90], azimuth in degrees
     * in the range [0, 360).
     */
    struct Topocentric {
        double elevation_deg = 0.0;   /**< Elevation angle in degrees (0..90). */
        double azimuth_deg   = 0.0;   /**< Azimuth angle in degrees (0..360). */
    };

    /**
     * @class Geometry
     * @brief Computes satellite/receiver geometry and auxiliary corrections.
     *
     * `Geometry` encapsulates the geometry computations required by the PPP
     * model: satellite position/velocity interpolation, line-of-sight vectors,
     * elevation/azimuth, geometric range and standard corrections such as:
     * - tropospheric delay (mapping + ZWD)
     * - satellite clock correction
     * - Shapiro and Sagnac delays
     * - solid Earth tide and atmospheric loading
     *
     * The class holds product-derived containers (orbits/clocks/antennas/ERPs)
     * and exposes thread-safe const accessors for geometry used by the
     * measurement modelling code.
     */
    class Geometry {

    public:
        /**
         * @brief Construct a Geometry helper.
         * @param config Read-only processing configuration.
         * @param cache ProductCache singleton (or application cache) providing parsed products.
         * @param rinex Reference to parsed RINEX observation/header metadata.
         * @param log_mgr Logger used for diagnostic messages.
         *
         * Construction defers heavy loading to `loadData()` implemented in the
         * translation unit to avoid inline-initialization ordering issues.
         */
        Geometry(const GnssConfig& config, ProductCache& cache, const RinexParser& rinex, LogManager& log_mgr);

        /** @brief Default virtual destructor. */
        ~Geometry() = default;

        /**
         * @brief Compute satellite geometry and common corrections for an observation.
         *
         * This routine performs the end-to-end geometry computation needed to
         * model a single satellite observation: it obtains (or interpolates)
         * the satellite position/velocity, computes receiver-satellite line-of-sight,
         * elevation/azimuth, geometric range and applies the standard corrections
         * used by the PPP observation model.
         *
         * @param[in] obs RINEX observation record (contains satellite id, signal, epoch).
         * @param[in] state Current PPP state (receiver position, troposphere, etc.).
         * @param[out] sat_pos Satellite ECEF position (meters).
         * @param[out] rx_pos Receiver ECEF position (meters).
         * @param[out] elevation Satellite elevation angle at the receiver (radians or degrees? documented below).
         * @param[out] azimuth Satellite azimuth at the receiver (radians or degrees? documented below).
         * @param[out] los Line-of-sight unit vector from receiver to satellite (ECEF).
         * @param[out] range Geometric range (meters) between receiver and satellite.
         * @param[out] tropo_delay Total tropospheric delay (meters).
         * @param[out] sat_clock Satellite clock correction (seconds).
         * @param[out] shapiro_delay Shapiro relativistic path delay (meters).
         * @param[out] sagnac_corr Sagnac correction (meters).
         * @param[out] otl_corr Ocean tidal loading site-displacement projection onto -LOS (meters).
         * @param[out] apl_corr Atmospheric pressure loading (S1/S2 thermal tide) site-displacement projection onto -LOS (meters).
         * @param[out] relativistic_corr Relativistic orbit-clock correction (meters or seconds depending on caller convention).
         * @param[out] solid_tide Solid Earth tide radial correction (meters).
         * @param[out] mf_w Wide-lane Melbourne-Wübbena combination factor (if applicable).
         * @param[out] mf_d Narrow-lane / dual-frequency mapping factor (if applicable).
         * @param[out] sat_vel Satellite ECEF velocity (meters/second).
         * @param[out] broadcast_only  Set true when the satellite range model is
         *             "broadcast grade" — the clock came from the broadcast
         *             polynomial with no precise CLK and no SSR clock (metre
         *             level).  The caller drops carrier phase for such satellites
         *             (phase at mm precision is meaningless against a metre-level
         *             clock).  False for precise-product or SSR-corrected runs.
         * @param[out] drop_reason  On success, set to @c DropReason::None.  On
         *             failure, set to the specific reason (BelowMask /
         *             NoProduct / NoModel) so callers can surface it in the
         *             satellite-status trace.  Defaults to a sink if the
         *             caller doesn't want it.
         * @return True on success, false if geometry/corrections could not be computed.
         */
        bool computeSatelliteGeometry(const RinexObservation& obs, const PppState& state,
            Eigen::Vector3d& sat_pos, Eigen::Vector3d& rx_pos, double& elevation,
            double& azimuth, Eigen::Vector3d& los, double& range, double& tropo_delay,
            double& sat_clock, double& shapiro_delay, double& sagnac_corr,
            double& otl_corr, double& apl_corr, double& relativistic_corr, double& solid_tide,
            double& mf_w, double& mf_d, Eigen::Vector3d& sat_vel,
            bool& broadcast_only,
            DropReason& drop_reason) const;

        /**
         * @brief Topocentric elevation/azimuth of a satellite as seen from a
         *        receiver, both given in ECEF.
         *
         * Pure geometry (no product/orbit state): elevation in [0, 90]°,
         * azimuth in [0, 360)° measured clockwise from north.  Degenerate
         * inputs (range < 1 m, or near-zero receiver magnitude) yield {0, 0}.
         *
         * @param rx_pos_ecef  Receiver ECEF position (meters).
         * @param sat_pos_ecef Satellite ECEF position (meters).
         * @return Topocentric { elevation_deg, azimuth_deg }.
         */
        static Topocentric computeTopocentric(const Eigen::Vector3d& rx_pos_ecef, const Eigen::Vector3d& sat_pos_ecef);

        /**
         * @brief Get interpolated satellite ECEF position from available orbit products.
         * @param satellite Satellite identifier string (e.g. "G01", "E11").
         * @param time Epoch at which the position is requested.
         * @return Satellite ECEF position in meters.
         *
         * If precise orbit (SP3) or propagated ephemeris data are available the
         * position is interpolated from `orbits_`, otherwise an empty/zero
         * vector may be returned (caller must check validity).
         */
        std::pair<Eigen::Vector3d, Eigen::Vector3d> getSatellitePV(
            const std::string& satellite, const GnssTime& time) const;

        /**
         * @brief Compute geometric range (meters) between receiver and satellite.
         * @param rx_pos Receiver ECEF position (meters).
         * @param sat_pos Satellite ECEF position (meters).
         * @return Euclidean distance in meters.
         */
        static double computeGeometricRange(const Eigen::Vector3d& rx_pos, const Eigen::Vector3d& sat_pos);

        /**
         * @brief Get mutable reference to the corrections object.
         * @return Reference to the internal GnssCorrections instance.
         *
         * Allows external components to access correction methods directly.
         */
        GnssCorrections& getCorrections() { return corrections_; }

        /**
         * @brief Get const reference to the corrections object.
         * @return Const reference to the internal GnssCorrections instance.
         */
        const GnssCorrections& getCorrections() const { return corrections_; }

        /**
         * @brief Get the cached precise Sun position (ECEF, meters).
         *
         * Returns the most recently computed rigorous Sun position from the
         * IAU 1976/1980 precession/nutation model. The cache is populated by
         * computeSatelliteGeometry().
         *
         * @return Cached Sun ECEF position (meters). May be zero if no epoch has been processed yet.
         */
        const Eigen::Vector3d& getCachedSunPosition() const { return cached_sun_pos_; }

        /**
         * @brief Wire in an externally-owned Antenna model for emission-time
         *        ATX-based pseudorange refinement.
         *
         * Without an antenna model, the second-pass refinement in
         * computeSatelliteGeometry only subtracts the slant tropospheric
         * ZHD from the pseudorange.  With the model set, it additionally
         * subtracts the IF-combined satellite + receiver PCO/PCV LOS
         * projection — driving the t_em residual from tens of ns down to
         * the code-noise floor (~ns / μm of satellite displacement).
         *
         * The pointer is non-owning; the caller (typically PppModel)
         * retains ownership and must outlive this Geometry instance.
         *
         * @param antenna  Reference to a constructed Antenna model.
         */
        void setAntennaModel(const Antenna& antenna) { antenna_ = &antenna; }

        /**
         * @brief Inject a non-owning SSR-correction store override (DI seam for
         *        the PPP-RTK closed-loop harness).
         *
         * Forwards to the internal @c GnssCorrections (which applies the SSR
         * clock overlay) and also overrides the SSR ORBIT overlay this class
         * applies in @c getSatellitePV.  Pass @c nullptr (the default) to use
         * the process-wide singleton — every non-harness caller stays
         * byte-identical.  The network closed-loop consumer points this at the
         * producer's OWNED store so a held-out station consumes the network's
         * freshly-emitted corrections without touching global state.
         *
         * Delegates to @ref setSsrOrbitStore + @ref setSsrClockStore (RTA-8:
         * the two overlays are independently routable); this coupled form is
         * retained for every pre-split caller and behaves exactly as before.
         *
         * @param store Non-owning store pointer (must outlive this object) or
         *        @c nullptr for the singleton.
         */
        void setSsrStore(const SsrCorrectionStore* store) {
            setSsrOrbitStore(store);
            setSsrClockStore(store);
        }

        /**
         * @brief RTA-8: route ONLY the SSR ORBIT overlay (@c getSatellitePV's
         *        RAC delta application) at @p store.
         *
         * Leaves the clock path (the internal @c GnssCorrections) untouched, so
         * a consumer can read orbit corrections from the external RTS stream
         * (the singleton, via @c nullptr here) while its clocks come from an
         * injected network-product store — the split the standalone
         * network-consumer wiring uses (@c processing.network_consumer.orbit_source).
         *
         * Also forwards to the internal @c GnssCorrections' Card-7 echo seam
         * (@c setOrbitEchoStore) so the orbit-coherence echo check always
         * compares against the store THIS overlay actually applies from —
         * the two routings cannot drift apart.
         *
         * @param store Non-owning store pointer (must outlive this object) or
         *        @c nullptr for the singleton.
         */
        void setSsrOrbitStore(const SsrCorrectionStore* store) {
            ssr_store_override_ = store;
            corrections_.setOrbitEchoStore(store);   // Card 7: echo check tracks the orbit routing
        }

        /**
         * @brief RTA-8: route ONLY the SSR CLOCK overlay (the internal
         *        @c GnssCorrections satellite-clock lookup) at @p store.
         *
         * Counterpart of @ref setSsrOrbitStore; the orbit overlay keeps its
         * current routing.
         *
         * @param store Non-owning store pointer (must outlive this object) or
         *        @c nullptr for the singleton.
         */
        void setSsrClockStore(const SsrCorrectionStore* store) {
            corrections_.setSsrStore(store);
        }

        /**
         * @brief RTB-5(c): was the most recent SSR orbit overlay APPLIED to
         *        @p satellite an APC-referenced (legacy-SSR) correction?
         *
         * Recorded at the two orbit-overlay apply sites in
         * @c getSatellitePV (live-ephemeris fallback and SP3 branch) — true
         * only when a fresh, IODE-matched orbit delta tagged
         * @c SsrReferencePoint::APC was actually subtracted for the most
         * recent query of this satellite; false when no overlay applied or
         * the record was CoM-referenced.  @c PppModel reads it to skip the
         * satellite-PCO range term (the APC double-count guard).
         *
         * Only maintained while @c realtime.honor_ssr_reference_point is
         * armed — with the toggle off nothing is recorded and this always
         * returns false, keeping the default path byte-identical and
         * allocation-free.
         */
        bool ssrOrbitApcApplied(const std::string& satellite) const {
            const auto it = ssr_orbit_apc_applied_.find(satellite);
            return it != ssr_orbit_apc_applied_.end() && it->second;
        }

        std::unordered_map<std::string, std::vector<const OrbitData*>> orbit_index_;

    private:
        GnssCorrections corrections_;                     /**< GNSS correction models. */
        const GnssConfig& config_;                        /**< Processing configuration. */
        Planetary planetary_;                             /**< Planetary (sun and moon) models. */
        Troposphere troposphere_;                         /**< Troposphere model instance. */
        ProductCache& product_cache_;                     /**< Product cache reference (replaces ProductParser). */
        LogManager& log_manager_;                         /**< Logger for diagnostics. */
        const RinexParser& rinex_parser_;                 /**< RINEX data parser (header/metadata). */
        const double elevation_threshold_;                /**< Elevation mask in degrees. */
        // Shared handles to the single cached copy (ProductCache) — read-only
        // product data referenced, NOT duplicated per Geometry instance (the
        // per-station memory fix; ~0.5 GB/station of copies otherwise).
        std::shared_ptr<const std::vector<OrbitData>>   orbits_;    /**< Satellite orbit records (shared). */
        std::shared_ptr<const std::vector<ClockData>>   clocks_;    /**< Satellite clock records (shared). */
        std::shared_ptr<const std::vector<AntennaData>> antennas_;  /**< Antenna correction models (shared). */
        std::shared_ptr<const std::vector<BiasData>>    biases_;     /**< Receiver/satellite bias entries (shared). */
        std::shared_ptr<const std::vector<ErpData>>     erps_;       /**< Earth rotation parameter samples (shared). */
        std::vector<double> klobuchar_coeffs_;            /**< Klobuchar ionospheric coefficients. */
        const std::map<std::string, std::vector<std::string>> signal_priorities_; /**< Signal priorities per GNSS system. */

        /// Non-owning pointer to the Antenna model used for ATX-based
        /// emission-time pseudorange refinement.  Set via setAntennaModel().
        /// May be nullptr (then the refinement only applies slant tropo).
        const Antenna* antenna_ = nullptr;

        /// Non-owning SSR store override; nullptr -> process-wide singleton.
        /// Set via setSsrStore() for the closed-loop consumer (see header).
        const SsrCorrectionStore* ssr_store_override_ = nullptr;

        /// Resolve the active SSR store (override when set, else the singleton).
        /// Defined in the .cpp where the full type is visible.
        const SsrCorrectionStore& ssrStore() const;

        /** @brief Load product and auxiliary data into internal containers. */
        void loadData();

        /**
         * @brief Ensure the Sun/Moon cache is current for the given epoch.
         *
         * Evaluates the rigorous IAU 1976/1980 model only when the epoch has
         * changed (tolerance 1 ms). Called by computeSatelliteGeometry() to
         * avoid redundant evaluations.
         */
        void ensureSunMoonCached(const GnssTime& time) const;

        /**
         * @brief Extract the best available code pseudorange from an
         *        observation and report the signals used.
         *
         * Tries (in order): clock-reference-aligned IF pair, bias-corrected
         * IF pair, best-effort IF pair, single-frequency fallback.
         * Reports the chosen signal codes via @p sig1_out and @p sig2_out
         * so that downstream emission-time refinement can apply matching
         * ATX corrections on the same pair before subtracting them from
         * the pseudorange.
         *
         * @param[in]  obs           RINEX observation containing raw measurements.
         * @param[out] pseudorange   Extracted (or IF-combined) pseudorange in meters.
         * @param[out] sig1_out      First code signal used (e.g. "C1W").
         * @param[out] sig2_out      Second code signal used; empty string
         *                           when the single-frequency fallback fired.
         * @return True if a valid pseudorange was obtained.
         */
        bool extractPseudorange(const RinexObservation& obs,
                                double& pseudorange,
                                std::string& sig1_out,
                                std::string& sig2_out) const;

        /**
         * @brief Compute satellite state at signal transmission time.
         *
         * Performs the single-pass emission-time computation: seeds TOF from
         * the pseudorange, applies the satellite clock, and evaluates orbit
         * and clock at the corrected emission epoch.
         *
         * @param obs          RINEX observation (satellite ID + reception time).
         * @param rx_pos       Receiver ECEF position (meters).
         * @param pseudorange  Code pseudorange used as TOF seed (meters).
         * @param[out] sat_pos_em Satellite ECEF position at emission time (meters).
         * @param[out] sat_vel_em Satellite ECEF velocity at emission time (m/s).
         * @param[out] sat_clock  Satellite clock correction at emission time (seconds).
         * @param[out] emission_time  GPS time of signal transmission.
         * @return True on success; false if orbit/clock data are unavailable or invalid.
         */
        bool computeEmissionState(const RinexObservation& obs,
            const Eigen::Vector3d& rx_pos, double pseudorange,
            Eigen::Vector3d& sat_pos_em, Eigen::Vector3d& sat_vel_em,
            double& sat_clock, GnssTime& emission_time,
            bool& broadcast_grade,
            DropReason& drop_reason) const;

        // Per-epoch cache for Sun/Moon ECEF positions.
        // The rigorous IAU 1976/1980 nutation model is epoch-dependent only
        // (not satellite-dependent), so caching avoids redundant 106-term
        // series evaluations across satellites within the same epoch.
        mutable GnssTime cached_sun_moon_epoch_{0, -1.0};  /**< Epoch of cached Sun/Moon. */
        mutable Eigen::Vector3d cached_sun_pos_{Eigen::Vector3d::Zero()};   /**< Cached Sun ECEF (m). */
        mutable Eigen::Vector3d cached_moon_pos_{Eigen::Vector3d::Zero()};  /**< Cached Moon ECEF (m). */

        /// Satellites already warned about for missing/invalid clock products.
        /// Without this set the warning fires once per (call site × satellite ×
        /// epoch) and dominates geometry.log — typically 4 sats × 2 calls/epoch
        /// × thousands of epochs. We warn once per satellite per session.
        mutable std::set<std::string> sats_warned_no_clock_;

        /// RTB-5(c): per-satellite record of whether the most recent
        /// getSatellitePV query APPLIED an APC-referenced SSR orbit delta
        /// (see ssrOrbitApcApplied).  Written at the overlay apply sites only
        /// while realtime.honor_ssr_reference_point is armed; mutable because
        /// getSatellitePV is const (same pattern as the Sun/Moon cache).
        mutable std::unordered_map<std::string, bool> ssr_orbit_apc_applied_;
    };

    } // namespace Model

} // namespace GnssProcessor
/*!
 * \file GnssConfig.cpp
 * \brief Non-loader bits of GnssConfig and LoggingConfig.
 *
 * Organization:
 *  - LoggingConfig::isLoggerEnabled
 *  - GnssConfig::getProductPath
 *
 * The bulk of this file used to be a 900-line @c loadFromJson — that
 * function has been split per top-level JSON section into private
 * helpers defined in @c GnssConfigLoad.cpp.  The shared JSON-reading
 * primitives (resolvePath / jval / jsection) live in
 * @c include/common/GnssConfigJsonHelpers.hpp.
 */

#include "config/GnssConfig.hpp"
#include "math/TimeUtils.hpp"   // GnssTime definition + mjdToDecimalYear / gpsTimeToUtcMjd

namespace GnssProcessor {

    // =================================================================
    // LoggingConfig helpers
    // =================================================================

    bool LoggingConfig::isLoggerEnabled(const std::string& name) const {
        if (name == "main_log")              return enable_main_log;
        if (name == "gnss_config")           return enable_gnss_config;
        if (name == "kalman_filter")         return enable_kalman_filter;
        if (name == "smoother")              return enable_smoother;
        if (name == "state_manager")         return enable_state_manager;
        if (name == "ppp_model")             return enable_ppp_model;
        if (name == "measurement_model")     return enable_measurement_model;
        if (name == "mw_slip_detector")      return enable_mw_slip_detector;
        if (name == "if_slip_detector")      return enable_if_slip_detector;
        if (name == "gf_slip_detector")      return enable_gf_slip_detector;
        if (name == "turbo_edit_resolver")   return enable_turbo_edit_resolver;
        if (name == "cycle_slip_manager")    return enable_cycle_slip_manager;
        if (name == "ambiguity_resolver")    return enable_ambiguity_resolver;
        if (name == "corrections")           return enable_corrections;
        if (name == "geometry")              return enable_geometry;
        if (name == "troposphere")           return enable_troposphere;
        if (name == "ionosphere")            return enable_ionosphere;
        if (name == "antenna")               return enable_antenna;
        if (name == "antenna_model")         return enable_antenna_model;
        if (name == "ocean_tide")            return enable_ocean_tide;
        if (name == "planetary_model")       return enable_planetary_model;
        if (name == "stochastic_model")      return enable_stochastic_model;
        if (name == "rinex_parser")          return enable_rinex_parser;
        if (name == "product_parser")        return enable_product_parser;
        if (name == "product_cache")         return enable_product_cache;
        if (name == "resolver")              return enable_resolver;
        if (name == "state_initializer")     return enable_state_initializer;
        if (name == "bssd_pivot_manager")    return enable_bssd_pivot_manager;
        if (name == "bssd_transform")        return enable_bssd_transform;
        if (name == "ppp_utils")             return enable_ppp_utils;
        if (name == "downloader")            return enable_downloader;
        if (name == "ntrip")                 return enable_ntrip;
        if (name == "rt_engine")             return enable_rt_engine;
        if (name == "imu")                   return enable_imu;
        return true; // unknown loggers default to enabled
    }

    // =================================================================
    // Product path lookup
    // =================================================================

    std::filesystem::path GnssConfig::getProductPath(const std::string& type) const {
        for (const auto& path : product_paths_) {
            std::string ext = path.extension().string();
            if ((type == "SP3" && ext == ".SP3") ||
                (type == "CLK" && ext == ".CLK") ||
                (type == "ATX" && ext == ".atx") ||
                (type == "BIA" && ext == ".BIA") ||
                (type == "ERP" && ext == ".ERP") ||
                (type == "INX" && (ext == ".INX" || ext == ".inx")) ||
                (type == "NC"  && ext == ".nc"))
                return path;
        }
        if (type == "RNX") return navigation_path_;
        return {};
    }

    // =================================================================
    // Construction (defaults relocated from the header member block)
    // =================================================================
    // Defaults relocated out of the GnssConfig.hpp member block so that
    // tuning a default no longer recompiles every includer of the header.
    // Generated from the former in-class initializers (values verbatim).
    GnssConfig::GnssConfig()
        : processing_mode_{ ProcessingMode::PPP }
        , frequency_mode_{ FrequencyMode::Dual }
        , channel_mode_{ ChannelMode::Single }
        , mw_frequency_mode_{ MwFrequencyMode::Mirror }
        , mw_channel_mode_{ MwChannelMode::Mirror }
        , enable_inhouse_bias_estimation_{ false }
        , mw_init_frac_threshold_{ 0.4 }
        , mw_cross_sat_ewma_alpha_{ 0.1 }
        , state_ambiguity_basis_{ StateAmbiguityBasis::PerBand }
        , satellite_interpolation_mode_{ SatelliteInterpolationMode::Lagrange }
        , satellite_clock_interpolation_mode_{ SatelliteClockInterpolationMode::Linear }
        , uncombined_enabled_{ false }
        , elevation_mask_deg_{ 10.0 }
        , exclude_eclipsing_satellites_{ false }
        , eclipse_shadow_scale_{ 1.0 }
        , data_gap_tolerance_s_{ 0.0 }
        , long_gap_tolerance_s_{ 300.0 }
        , gnss_rate_s_{ 0.0 }
        , max_duration_s_{ 0.0 }
        , rinex_types_{ "CL" }
        , active_systems_{ "GPS", "Galileo", "GLONASS", "BeiDou", "QZSS", "SBAS" }
        , zhd_pressure_mbar{ 1013.25 }
        , zhd_temp_kelvin{ 288.15 }
        , tropo_apriori_source_{ TroposphereAprioriSource::StandardAtmosphere }
        , tropo_mapping_function_{ TroposphereMappingFunction::Gmf }
        , leap_seconds_tai_{ 37.0 }
        , residual_threshold_{ 6.0 }
        , bad_meas_percent_{ 0.5 }
        , ambiguity_std_{ 1e3 }
        , code_noise_amplify_{ 1e4 }
        , estimate_tropo_gradients_{ false }
        , relinearize_geometry_{ false }
        , relinearize_dpos_threshold_phase_m_{ 1.0 }
        , relinearize_dpos_threshold_code_only_m_{ 100.0 }
        , relinearize_max_count_{ -1 }
        , eigen_threads_{ 1 }
        , run_smoother_{ false }
        , enable_noncausal_slip_bridge_{ false }
        , slip_bridge_half_window_epochs_{ 30 }
        , pre_bridge_float_tol_cycles_{ 0.5 }
        , pre_bridge_conv_var_m2_{ 0.0025 }
        , enable_auto_report_{ false }
        , report_backend_{ "matplotlib" }
        , report_python_{ "" }
        , report_viewers_{}
        , report_async_{ false }
        , estimate_phase_clock_{ false }
        , enable_if_slip_detector_{ false }
        , accumulate_phase_windup_{ false }
        , enable_between_satellite_sd_{ false }
        , enable_glonass_bssd_{ false }
        , enable_glonass_ifb_{ false }
        , bssd_pivot_min_elevation_deg_{ 45.0 }
        , pivot_slip_min_cycles_{ 0.25 }
        , persistent_bias_min_cycles_{ 0.10 }
        , phase_huber_sres_threshold_{ 6.0 }
        , enable_huber_innovation_{ false }
        , enable_huber_r_inflation_{ true }
        , enable_subcycle_slip_suppression_{ true }
        , estimate_ifb_{ false }
        , lock_signals_to_clock_ref_{ false }
        , require_bia_for_signals_{ false }
        , estimate_sat_biases_{ false }
        , sat_bias_cache_warm_age_s_{ 300.0 }
        , sat_bias_cache_max_age_s_{ 3600.0 }
        , no_bia_code_downweight_{ 100.0 }
        , estimate_velocity_{ false }
        , has_reference_position_{ false }
        , reference_position_ecef_{ 0.0, 0.0, 0.0 }
        , stochastic_model_{ StochasticModelType::Elevation }
        , code_elevation_power_{ 1.0 }
        , phase_elevation_power_{ 0.5 }
        , data_source_{ DataSource::Internal }
        , app_mode_{ AppMode::PostProcessing }
        , ar_mode_{ 0 }
        , ar_ratio_threshold_{ 3.0 }
        , ar_elevation_mask_{ 20.0 }
        , ar_hold_variance_{ 0.001 }
        , ar_min_epochs_{ 10 }
        , ar_max_position_sigma_m_{ 0.05 }
        , ar_ratio_upper_bound_{ 1e4 }
        , ar_wl_only_{ false }
        , ar_wl_only_pseudo_var_{ 1e-4 }
        , ar_wl_max_frac_{ 0.40 }
        , ar_min_success_rate_{ 0.999 }
        , ar_nl_max_per_pair_frac_{ 0.15 }
        , ar_enable_partial_ar_{ false }
    {
    }

    // =================================================================
    //  ITRF velocity propagation of the fixed a-priori reference position
    // =================================================================
    std::array<double, 3> GnssConfig::getReferencePositionEcefAtEpoch(const GnssTime& t) const {
        // No propagation unless explicitly enabled with a velocity and a
        // reference epoch, and a valid observation epoch to propagate to.
        if (!enable_itrf_velocity_ || !has_reference_velocity_ ||
            reference_position_epoch_year_ == 0.0 || t.week <= 0) {
            return reference_position_ecef_;
        }
        // Linear plate motion: r(t) = r0 + v·(year(t) − t0).  Sub-day precision
        // in the decimal year is irrelevant at cm/yr station velocities.
        const double year = TimeUtils::mjdToDecimalYear(TimeUtils::gpsTimeToUtcMjd(t));
        const double dt   = year - reference_position_epoch_year_;  // years
        return {
            reference_position_ecef_[0] + reference_velocity_ecef_[0] * dt,
            reference_position_ecef_[1] + reference_velocity_ecef_[1] * dt,
            reference_position_ecef_[2] + reference_velocity_ecef_[2] * dt,
        };
    }

} // namespace GnssProcessor

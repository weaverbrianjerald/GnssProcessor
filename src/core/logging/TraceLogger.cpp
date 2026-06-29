/**
 * @file TraceLogger.cpp
 * @brief Implementation of TraceLogger for writing epoch-level CSV traces.
 *
 * The TraceLogger writes four CSV trace files:
 * - state_trace.csv          : receiver state vector and per-state std.dev
 * - ambiguity_trace.csv      : ambiguity estimates and standard deviations
 * - measurement_residuals.csv: per-observation innovations/residuals/standardized residuals
 * - wls_trace.csv            : WLS estimation and seeding diagnostics
 *
 * Streams are opened in the constructor and written from logically-const methods;
 * stream members are therefore declared mutable in the header to allow writes
 * from const methods.
 */

#include "logging/TraceLogger.hpp"
#include "filter/StateKeySchema.hpp"
#include <iomanip>
#include <limits>
#include <Eigen/Dense>

namespace GnssProcessor {

TraceLogger::TraceLogger(LogManager& log_manager, const std::filesystem::path& log_dir, bool is_ppp_rtk,
                     const LoggingConfig& logging_config)
: log_manager_(log_manager), is_ppp_rtk_(is_ppp_rtk) {
std::error_code ec;
if (!std::filesystem::exists(log_dir)) {
    std::filesystem::create_directories(log_dir, ec);
    if (ec) {
        try {
            log_manager_.getLogger("main_log")->warn("Failed to create log directory '{}': {}", log_dir.string(), ec.message());
        } catch (...) { /* best-effort logging only */ }
    }
}

const auto state_path = log_dir / "state_trace.csv";
const auto amb_path = log_dir / "ambiguity_trace.csv";
const auto iono_path = log_dir / "ionosphere_trace.csv";
const auto sat_bias_path = log_dir / "sat_bias_trace.csv";
const auto code_bias_path = log_dir / "code_bias_trace.csv";
const auto sat_status_path = log_dir / "satellite_status_trace.csv";
const auto dop_path = log_dir / "dop_trace.csv";
const auto residual_path = log_dir / "measurement_residuals.csv";
const auto wls_path = log_dir / "wls_trace.csv";

if (logging_config.enable_state_trace) {
    state_trace_.open(state_path, std::ios::out | std::ios::trunc);
    if (!state_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open state trace file '{}'", state_path.string()); }
        catch (...) {}
    }
}

if (logging_config.enable_ambiguity_trace) {
    ambiguity_trace_.open(amb_path, std::ios::out | std::ios::trunc);
    if (!ambiguity_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open ambiguity trace file '{}'", amb_path.string()); }
        catch (...) {}
    }
}

if (logging_config.enable_ionosphere_trace) {
    ionosphere_trace_.open(iono_path, std::ios::out | std::ios::trunc);
    if (!ionosphere_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open ionosphere trace file '{}'", iono_path.string()); }
        catch (...) {}
    }
}

if (logging_config.enable_sat_bias_trace) {
    sat_bias_trace_.open(sat_bias_path, std::ios::out | std::ios::trunc);
    if (!sat_bias_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open sat-bias trace file '{}'", sat_bias_path.string()); }
        catch (...) {}
    }
}

if (logging_config.enable_code_bias_trace) {
    code_bias_trace_.open(code_bias_path, std::ios::out | std::ios::trunc);
    if (!code_bias_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open code-bias trace file '{}'", code_bias_path.string()); }
        catch (...) {}
    } else {
        code_bias_trace_ << "week,tow_s,key,value_m,std_m\n";
    }
}

if (logging_config.enable_sat_status_trace) {
    sat_status_trace_.open(sat_status_path, std::ios::out | std::ios::trunc);
    if (!sat_status_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open sat-status trace file '{}'", sat_status_path.string()); }
        catch (...) {}
    }
}

if (logging_config.enable_dop_trace) {
    dop_trace_.open(dop_path, std::ios::out | std::ios::trunc);
    if (!dop_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open DOP trace file '{}'", dop_path.string()); }
        catch (...) {}
    }
}

if (logging_config.enable_residual_trace) {
    residual_trace_.open(residual_path, std::ios::out | std::ios::trunc);
    if (!residual_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open residual trace file '{}'", residual_path.string()); }
        catch (...) {}
    }
}

if (logging_config.enable_wls_trace) {
    wls_trace_.open(wls_path, std::ios::out | std::ios::trunc);
    if (!wls_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open WLS trace file '{}'", wls_path.string()); }
        catch (...) {}
    }
}

    // Write base header to state trace file but do NOT terminate with newline.
    // The clock columns, phase clock, gradient and per-system time offset
    // columns are appended on the first call to logState() based on what
    // the StateManager has actually allocated (e.g. clock is omitted under
    // BSSD where it isn't a state).
    if (state_trace_.is_open()) {
        state_trace_ << "week,tow_s,x_m,y_m,z_m,x_std_m,y_std_m,z_std_m,tropo_m,tropo_std_m";
        if (is_ppp_rtk_) state_trace_ << ",iono_mean_m,iono_count";
    }

    if (ambiguity_trace_.is_open()) {
        ambiguity_trace_ << "week,tow_s,key,value_m,std_m\n";
    }

    if (ionosphere_trace_.is_open()) {
        ionosphere_trace_ << "week,tow_s,satellite,system,iono_m,std_m,elevation_deg,azimuth_deg,"
                             "mapping_factor,vtec_m,is_pivot,pivot_satellite\n";
    }

    if (sat_bias_trace_.is_open()) {
        sat_bias_trace_ << "week,tow_s,satellite,signal,system,value_m,std_m,elevation_deg,"
                           "is_pivot,pivot_satellite\n";
    }

    if (sat_status_trace_.is_open()) {
        sat_status_trace_ << "week,tow_s,satellite,system,status,status_code,"
                            "elevation_deg,azimuth_deg,n_signals_used,n_signals_total\n";
    }

    if (dop_trace_.is_open()) {
        dop_trace_ << "week,tow_s,n_used_total,"
                      "n_used_gps,n_used_galileo,n_used_glonass,"
                      "n_used_beidou,n_used_qzss,n_used_sbas,"
                      "pdop,hdop,vdop,tdop,gdop,"
                      "adop_m,n_ambiguities\n";
    }

    if (residual_trace_.is_open()) {
        // The trailing redundancy/w_test/mdb_m columns carry the Baarda
        // data-snooping QC (item #9); they are populated only when
        // estimation.enable_data_snooping is on, and written as nan otherwise.
        residual_trace_ << "week,tow_s,satellite,signal,innovation_m,residual_m,residual_std_m,standardized_residual,redundancy,w_test,mdb_m\n";
    }

    if (wls_trace_.is_open()) {
        wls_trace_ << "week,tow_s,iterations,obs_count,x_m,y_m,z_m,clock_m,tropo_m,rms_m,max_abs_m,"
                   << "wls_clock_m,kf_clock_m,delta_clock_m,jump_detected,should_seed\n";
    }

    // Consider the logger initialized if at least one file is writable.
    initialized_ = state_trace_.is_open() || ambiguity_trace_.is_open() || ionosphere_trace_.is_open()
                || sat_bias_trace_.is_open() || code_bias_trace_.is_open() || sat_status_trace_.is_open()
                || dop_trace_.is_open() || residual_trace_.is_open() || wls_trace_.is_open();

    if (!initialized_) {
        try { log_manager_.getLogger("main_log")->error("TraceLogger failed to open any trace files in '{}'", log_dir.string()); }
        catch (...) {}
    }
}

bool TraceLogger::isInitialized() const {
    return initialized_;
}

void TraceLogger::flush() const {
    // Per-record writers use '\n' (no implicit flush); push buffered tail
    // records to disk so an external reader (the end-of-run report) sees
    // complete traces.
    for (std::ofstream* s : { &state_trace_, &ambiguity_trace_, &residual_trace_,
                              &ionosphere_trace_, &sat_bias_trace_, &code_bias_trace_,
                              &sat_status_trace_, &dop_trace_, &wls_trace_ }) {
        if (s->is_open()) s->flush();
    }
}

void TraceLogger::logState(const GnssTime& time, const StateManager& state_manager) const {
if (!state_trace_.is_open()) return;

// On first call, complete the header line with dynamic columns.  Clock
// is dynamic because BSSD strips the receiver clock state — its column
// is omitted entirely rather than emitted as zeros.
if (!state_header_written_) {
    if (state_manager.hasClock()) {
        state_trace_ << ",clock_m,clock_std_m";
    }
    if (state_manager.hasPhaseClock()) {
        state_trace_ << ",phase_clock_m,phase_clock_std_m";
    }
    if (state_manager.hasTropoGradients()) {
        state_trace_ << ",gn_m,gn_std_m,ge_m,ge_std_m";
    }
    const auto& time_offset_systems = state_manager.getSystemsWithTimeOffsets();
    for (const auto& sys : time_offset_systems) {
        state_trace_ << "," << sys << "_m," << sys << "_std_m";
    }
    state_trace_ << "\n";
    state_trace_.flush();
    state_header_written_ = true;
}

const Eigen::MatrixXd cov = state_manager.getCovariance();
auto std_at = [&cov](size_t idx) -> double {
    if (idx < static_cast<size_t>(cov.rows()) && idx < static_cast<size_t>(cov.cols())) {
        const double var = cov(static_cast<Eigen::Index>(idx), static_cast<Eigen::Index>(idx));
        return std::sqrt(std::max(0.0, var));
    }
    return std::numeric_limits<double>::quiet_NaN();
};

    const Eigen::VectorXd sv = state_manager.getStateVector();
    const size_t pos_idx = state_manager.getPositionIndex();
    const size_t trop_idx = state_manager.getTropoIndex();
    const bool has_pos = sv.size() >= pos_idx + 3;
    const bool has_trop = trop_idx < static_cast<size_t>(sv.size());
    const bool has_iono = is_ppp_rtk_ && !state_manager.getIonosphereStates().empty();

    if (!(has_pos && has_trop)) {
        try { log_manager_.getLogger("main_log")->warn(
            "[{} {:.3f}] Skipped state trace row: state size {} insufficient (pos_idx={}, trop_idx={})",
            time.week, time.tow, sv.size(), pos_idx, trop_idx); }
        catch (...) {}
        return;
    }

    state_trace_ << std::fixed << std::setprecision(3)
                 << time.week << "," << time.tow << ",";
    state_trace_ << std::setprecision(6)
                 << sv(static_cast<Eigen::Index>(pos_idx)) << ","
                 << sv(static_cast<Eigen::Index>(pos_idx + 1)) << ","
                 << sv(static_cast<Eigen::Index>(pos_idx + 2)) << ",";
    state_trace_ << std_at(pos_idx) << ","
                 << std_at(pos_idx + 1) << ","
                 << std_at(pos_idx + 2) << ",";
    state_trace_ << sv(static_cast<Eigen::Index>(trop_idx)) << ","
                 << std_at(trop_idx);
    if (is_ppp_rtk_) {
        // Log the number of per-satellite iono states and mean value
        const auto& iono_states = state_manager.getIonosphereStates();
        double iono_mean = 0.0;
        for (const auto& [sat, idx] : iono_states) {
            if (idx < static_cast<size_t>(sv.size())) {
                iono_mean += sv(static_cast<Eigen::Index>(idx));
            }
        }
        if (!iono_states.empty()) iono_mean /= static_cast<double>(iono_states.size());
        state_trace_ << ","
                     << iono_mean << ","
                     << static_cast<int>(iono_states.size());
    }

    // Receiver clock — omitted under BSSD (no state allocated).
    if (state_manager.hasClock()) {
        const size_t clk_idx = state_manager.getClockIndex();
        state_trace_ << ","
                     << sv(static_cast<Eigen::Index>(clk_idx)) << ","
                     << std_at(clk_idx);
    }

    // Phase clock offset state
    if (state_manager.hasPhaseClock()) {
        const size_t pc_idx = state_manager.getPhaseClockIndex();
        state_trace_ << ","
                     << sv(static_cast<Eigen::Index>(pc_idx)) << ","
                     << std_at(pc_idx);
    }

    // Tropospheric gradient states (Gn, Ge)
    if (state_manager.hasTropoGradients()) {
        const size_t gn_idx = state_manager.getTropoGradientNorthIndex();
        const size_t ge_idx = state_manager.getTropoGradientEastIndex();
        state_trace_ << "," << sv(static_cast<Eigen::Index>(gn_idx)) << "," << std_at(gn_idx)
                     << "," << sv(static_cast<Eigen::Index>(ge_idx)) << "," << std_at(ge_idx);
    }
    
    // Write inter-system time offset values (one pair of columns per system)
    const auto& time_offset_systems = state_manager.getSystemsWithTimeOffsets();
    for (const auto& sys : time_offset_systems) {
        size_t idx = state_manager.getSystemTimeOffsetIndex(sys);
        if (idx < static_cast<size_t>(sv.size())) {
            state_trace_ << "," << std::setprecision(6) << sv(static_cast<Eigen::Index>(idx))
                         << "," << std::setprecision(6) << std_at(idx);
        } else {
            // Keep CSV column alignment when index is invalid
            state_trace_ << ",,";
        }
    }
    
    state_trace_ << "\n";
}

void TraceLogger::logAmbiguities(const GnssTime& time, const StateManager& state_manager) const {
    if (!ambiguity_trace_.is_open()) return;

    const Eigen::MatrixXd cov = state_manager.getCovariance();
    auto std_at = [&cov](size_t idx) -> double {
        if (idx < static_cast<size_t>(cov.rows()) && idx < static_cast<size_t>(cov.cols())) {
            const double var = cov(static_cast<Eigen::Index>(idx), static_cast<Eigen::Index>(idx));
            return std::sqrt(std::max(0.0, var));
        }
        return std::numeric_limits<double>::quiet_NaN();
    };

    const auto& amb_map = state_manager.getAmbiguities();
    if (amb_map.empty()) return;

    const Eigen::VectorXd sv = state_manager.getStateVector();

    for (const auto& [key, idx] : amb_map) {
        if (idx >= static_cast<size_t>(sv.size())) continue;
        const double val = sv(static_cast<Eigen::Index>(idx));
        const double std = std_at(idx);
        ambiguity_trace_ << std::fixed << std::setprecision(3)
                         << time.week << "," << time.tow << ",";
        ambiguity_trace_ << key << ",";
        ambiguity_trace_ << std::setprecision(6) << val << "," << std << "\n";
    }
}

void TraceLogger::logReceiverCodeBiases(const GnssTime& time, const StateManager& state_manager) const {
    if (!code_bias_trace_.is_open()) return;

    const auto& cb_map = state_manager.getCodeBiasStates();
    if (cb_map.empty()) return;

    const Eigen::VectorXd sv = state_manager.getStateVector();
    const Eigen::MatrixXd cov = state_manager.getCovariance();
    auto std_at = [&cov](size_t idx) -> double {
        if (idx < static_cast<size_t>(cov.rows()) && idx < static_cast<size_t>(cov.cols())) {
            const double var = cov(static_cast<Eigen::Index>(idx), static_cast<Eigen::Index>(idx));
            return std::sqrt(std::max(0.0, var));
        }
        return std::numeric_limits<double>::quiet_NaN();
    };

    for (const auto& [key, idx] : cb_map) {
        if (idx >= static_cast<size_t>(sv.size())) continue;
        code_bias_trace_ << std::fixed << std::setprecision(3)
                         << time.week << "," << time.tow << ",";
        code_bias_trace_ << key << ",";
        code_bias_trace_ << std::setprecision(6)
                         << sv(static_cast<Eigen::Index>(idx)) << ","
                         << std_at(idx) << "\n";
    }
}

void TraceLogger::logIonosphere(const GnssTime& time, const StateManager& state_manager) const {
    if (!ionosphere_trace_.is_open()) return;

    const auto& iono_map = state_manager.getIonosphereStates();
    if (iono_map.empty()) return;

    const Eigen::VectorXd sv = state_manager.getStateVector();
    const Eigen::MatrixXd cov = state_manager.getCovariance();
    const auto& elev_map = state_manager.getIonosphereElevations();
    const auto& az_map   = state_manager.getIonosphereAzimuths();
    const auto& pivots   = state_manager.getFrozenPivotSatellites();

    auto std_at = [&cov](size_t idx) -> double {
        if (idx < static_cast<size_t>(cov.rows()) && idx < static_cast<size_t>(cov.cols())) {
            const double var = cov(static_cast<Eigen::Index>(idx), static_cast<Eigen::Index>(idx));
            return std::sqrt(std::max(0.0, var));
        }
        return std::numeric_limits<double>::quiet_NaN();
    };

    // Map satellite-system prefix to a human-readable constellation name and
    // to that constellation's BSSD pivot (if any).  The frozen-pivot set is a
    // flat set of sat IDs; we bucket by leading character to recover the
    // per-constellation pivot without plumbing PivotManager through to the
    // logger.
    auto systemFor = [](char c) -> const char* {
        switch (c) {
            case 'G': return "GPS";
            case 'E': return "Galileo";
            case 'R': return "GLONASS";
            case 'C': return "BeiDou";
            case 'J': return "QZSS";
            case 'S': return "SBAS";
            default:  return "Unknown";
        }
    };
    std::map<char, std::string> pivot_by_constellation;
    for (const auto& sat : pivots) {
        if (!sat.empty()) pivot_by_constellation[sat[0]] = sat;
    }

    // Thin-shell single-layer mapping factor: same Earth radius / shell
    // height (450 km) that GlobalIonosphereMap uses for VTEC<->STEC, so the
    // vtec_m column is comparable across the codebase.
    auto mappingFactor = [](double el_deg) -> double {
        static constexpr double kEarthRadius_m = Constants::EARTH_RADIUS;
        static constexpr double kIonoShell_m   = 450.0e3;
        if (!std::isfinite(el_deg) || el_deg <= 0.0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const double el_rad = el_deg * Constants::PI / 180.0;
        const double sin_zp = (kEarthRadius_m / (kEarthRadius_m + kIonoShell_m)) * std::cos(el_rad);
        const double cos_zp = std::sqrt(std::max(0.0, 1.0 - sin_zp * sin_zp));
        if (cos_zp <= 1.0e-6) return std::numeric_limits<double>::quiet_NaN();
        return 1.0 / cos_zp;
    };

    for (const auto& [sat, idx] : iono_map) {
        if (idx >= static_cast<size_t>(sv.size())) continue;
        const double val = sv(static_cast<Eigen::Index>(idx));
        const double sigma = std_at(idx);
        double elev = std::numeric_limits<double>::quiet_NaN();
        double az   = std::numeric_limits<double>::quiet_NaN();
        auto el_it = elev_map.find(sat);
        if (el_it != elev_map.end()) elev = el_it->second;
        auto az_it = az_map.find(sat);
        if (az_it != az_map.end()) az = az_it->second;

        const double mf      = mappingFactor(elev);
        const double vtec_m  = std::isfinite(mf) ? (val / mf) : std::numeric_limits<double>::quiet_NaN();
        const char  sys_prefix = sat.empty() ? '?' : sat.front();
        const char* sys_name = systemFor(sys_prefix);
        const bool is_pivot  = pivots.count(sat) != 0;
        auto pv_it = pivot_by_constellation.find(sys_prefix);
        const std::string pivot_sat = (pv_it != pivot_by_constellation.end()) ? pv_it->second : std::string();

        ionosphere_trace_ << std::fixed
                          << time.week << "," << std::setprecision(3) << time.tow << ","
                          << sat << "," << sys_name << ","
                          << std::setprecision(6) << val << "," << sigma << ","
                          << std::setprecision(3) << elev << "," << az << ","
                          << std::setprecision(6) << mf << "," << vtec_m << ","
                          << (is_pivot ? 1 : 0) << "," << pivot_sat << "\n";
    }
}

void TraceLogger::logSatBiases(const GnssTime& time, const StateManager& state_manager) const {
    if (!sat_bias_trace_.is_open()) return;

    const auto& bias_map = state_manager.getSatBiasStates();
    if (bias_map.empty()) return;

    const Eigen::VectorXd sv  = state_manager.getStateVector();
    const Eigen::MatrixXd cov = state_manager.getCovariance();
    const auto& elev_map = state_manager.getIonosphereElevations();  // PPP-RTK only; absent in PPP.
    const auto& pivots   = state_manager.getFrozenPivotSatellites();

    auto std_at = [&cov](size_t idx) -> double {
        if (idx < static_cast<size_t>(cov.rows()) && idx < static_cast<size_t>(cov.cols())) {
            const double var = cov(static_cast<Eigen::Index>(idx), static_cast<Eigen::Index>(idx));
            return std::sqrt(std::max(0.0, var));
        }
        return std::numeric_limits<double>::quiet_NaN();
    };

    auto systemFor = [](char c) -> const char* {
        switch (c) {
            case 'G': return "GPS";
            case 'E': return "Galileo";
            case 'R': return "GLONASS";
            case 'C': return "BeiDou";
            case 'J': return "QZSS";
            case 'S': return "SBAS";
            default:  return "Unknown";
        }
    };
    std::map<char, std::string> pivot_by_constellation;
    for (const auto& sat : pivots) {
        if (!sat.empty()) pivot_by_constellation[sat[0]] = sat;
    }

    for (const auto& [key, idx] : bias_map) {
        if (idx >= static_cast<size_t>(sv.size())) continue;

        // "<sat>|<signal>" -- parseSatBiasKey returns nullopt on a malformed
        // key (no pipe); skip those rather than logging an unparseable row.
        const auto parts = StateKeys::parseSatBiasKey(key);
        if (!parts) continue;
        const std::string& sat    = parts->satellite;
        const std::string& signal = parts->signal;

        const double val   = sv(static_cast<Eigen::Index>(idx));
        const double sigma = std_at(idx);

        double elev = std::numeric_limits<double>::quiet_NaN();
        auto el_it = elev_map.find(sat);
        if (el_it != elev_map.end()) elev = el_it->second;

        const char sys_prefix = sat.empty() ? '?' : sat.front();
        const char* sys_name  = systemFor(sys_prefix);
        const bool is_pivot   = pivots.count(sat) != 0;
        auto pv_it = pivot_by_constellation.find(sys_prefix);
        const std::string pivot_sat = (pv_it != pivot_by_constellation.end()) ? pv_it->second : std::string();

        sat_bias_trace_ << std::fixed
                        << time.week << "," << std::setprecision(3) << time.tow << ","
                        << sat << "," << signal << "," << sys_name << ","
                        << std::setprecision(6) << val << "," << sigma << ","
                        << std::setprecision(3) << elev << ","
                        << (is_pivot ? 1 : 0) << "," << pivot_sat << "\n";
    }
}

void TraceLogger::logSatelliteStatus(const GnssTime& time,
                                     const std::vector<RinexObservation>& epoch_obs,
                                     const std::vector<PppObservation>& modeled_obs,
                                     const std::map<std::string, DropReason>& drop_reasons,
                                     const EpochMeasurement& applied_meas,
                                     bool have_applied,
                                     const StateManager& state_manager,
                                     const std::set<std::string>& slipped_sats,
                                     const std::set<std::string>& reset_sats,
                                     double elevation_mask_deg) const {
    (void)elevation_mask_deg;  // currently informational only; mask check
                               // happens upstream in ModelGeometry and is
                               // detected here via "in epoch_obs, absent
                               // from modeled_obs".
    if (!sat_status_trace_.is_open()) return;
    if (epoch_obs.empty() && modeled_obs.empty()) return;

    const auto& pivots = state_manager.getFrozenPivotSatellites();

    // -------------------------------------------------------------------
    // Derive per-sat "used signals" + "code outlier" sets from the applied
    // measurement.  applied_meas may be empty if @c have_applied is false
    // (e.g. epoch was skipped due to insufficient observations); in that
    // case every tracked sat collapses to BelowMask or NotUsed based on
    // elevation alone.
    // -------------------------------------------------------------------
    std::map<std::string, int> n_signals_used;       // sat -> count
    std::set<std::string> sats_with_code_outlier;
    if (have_applied) {
        for (size_t i = 0; i < applied_meas.satellite_ids.size(); ++i) {
            const std::string& sat_full = applied_meas.satellite_ids[i];
            // BSSD compound IDs are "<primary>-<pivot>" (see
            // KalmanFilter::splitBssdSatId).  Strip the suffix so a non-
            // pivot BSSD row like "G05-G03" collapses to "G05" and the
            // count matches the primary sat in @c per_sat.
            const auto dash = sat_full.find('-');
            const std::string sat = (dash == std::string::npos)
                                       ? sat_full
                                       : sat_full.substr(0, dash);
            ++n_signals_used[sat];
        }
        // excluded_measurements keys: "<primary_sat>_<signal>".  The
        // primary sat is already extracted before insertion (see
        // KalmanFilter::handleCodeOutlier), so the leading token before
        // the first underscore is the bare sat ID.  Be defensive about
        // any compound that may slip through.
        for (const auto& key : applied_meas.excluded_measurements) {
            const auto us = key.find('_');
            if (us == std::string::npos) continue;
            std::string sat = key.substr(0, us);
            const auto dash = sat.find('-');
            if (dash != std::string::npos) sat = sat.substr(0, dash);
            sats_with_code_outlier.insert(std::move(sat));
        }
    }

    auto systemFor = [](char c) -> const char* {
        switch (c) {
            case 'G': return "GPS";
            case 'E': return "Galileo";
            case 'R': return "GLONASS";
            case 'C': return "BeiDou";
            case 'J': return "QZSS";
            case 'S': return "SBAS";
            default:  return "Unknown";
        }
    };

    // Deduplicate modeled observations by satellite (multiple PppObservation
    // rows can share a satellite for different signals; we want one status
    // row per (epoch, sat), with n_signals_total counting tracked signals).
    struct SatObsSummary {
        double elevation{0.0};
        double azimuth{0.0};
        int    n_signals_total{0};
    };
    std::map<std::string, SatObsSummary> per_sat;
    for (const auto& obs : modeled_obs) {
        if (obs.satellite.empty()) continue;
        auto& s = per_sat[obs.satellite];
        s.elevation = obs.elevation;   // last observation wins (all signals share geometry)
        s.azimuth   = obs.azimuth;
        // Count distinct signal types tracked for this sat at this epoch.
        s.n_signals_total += static_cast<int>(obs.observed.size());
    }

    // Raw-RINEX-only sats: in epoch_obs but absent from modeled_obs.  These
    // were dropped by ModelGeometry (most often "elevation < mask", but
    // also missing ephemeris / unsupported system / nan-position) and so
    // never reach the iono / sat-bias / Kalman update.  We surface them as
    // "BelowMask" rows so the strip chart shows the FULL constellation,
    // not just the geometry-resolved subset.  Deduplicated by sat across
    // any per-signal duplicates in epoch_obs.
    std::map<std::string, int> per_sat_raw_signal_count;
    for (const auto& obs : epoch_obs) {
        if (obs.satellite.empty()) continue;
        per_sat_raw_signal_count[obs.satellite] +=
            static_cast<int>(obs.measurements.size());
    }

    auto emitRow = [&](const std::string& sat, int code, const char* label,
                       double elev, double az, int n_used, int n_total) {
        const char sys_prefix = sat.empty() ? '?' : sat.front();
        const char* sys_name  = systemFor(sys_prefix);
        sat_status_trace_ << std::fixed
                          << time.week << "," << std::setprecision(3) << time.tow << ","
                          << sat << "," << sys_name << ","
                          << label << "," << code << ","
                          << std::setprecision(3) << elev << ","
                          << az << ","
                          << n_used << "," << n_total << "\n";
    };

    // -------------------------------------------------------------------
    // Pass 1: geometry-resolved sats (in modeled_obs).  Status precedence
    // (most actionable wins): Slip > CodeOutlier > Pivot > Clean.  Sats
    // that made it through geometry but aren't in applied_meas (rare
    // edge case) fall through to Clean with n_used=0 -- if the upstream
    // taxonomy is correct, this means a downstream filter accepted the
    // sat with zero rows, which is a tripwire worth surfacing.
    // -------------------------------------------------------------------
    for (const auto& [sat, s] : per_sat) {
        const int n_used = [&]() {
            auto it = n_signals_used.find(sat);
            return it != n_signals_used.end() ? it->second : 0;
        }();

        int code = -1;
        const char* label = nullptr;
        const bool is_pivot        = pivots.count(sat) != 0;
        const bool is_slipped      = slipped_sats.count(sat) != 0
                                  || reset_sats.count(sat) != 0;
        const bool is_code_outlier = sats_with_code_outlier.count(sat) != 0;

        if (is_slipped)              { code = 2; label = "Slip"; }
        else if (is_code_outlier)    { code = 3; label = "CodeOutlier"; }
        else if (is_pivot)           { code = 1; label = "Pivot"; }
        else                         { code = 0; label = "Clean"; }

        emitRow(sat, code, label, s.elevation, s.azimuth,
                n_used, s.n_signals_total);
    }

    // -------------------------------------------------------------------
    // Pass 2: tracked-but-dropped sats (in epoch_obs, absent from
    // modeled_obs).  Use drop_reasons (populated by PppModel) to split
    // into BelowMask / NoProduct / NoModel.  Elevation is NaN -- the
    // geometry drop path doesn't preserve it.  If the reason is missing
    // from the map (rare: every drop site is annotated), default to
    // NoModel so the row is still rendered with a "this needs attention"
    // colour instead of disappearing entirely.
    // -------------------------------------------------------------------
    const double kNaN = std::numeric_limits<double>::quiet_NaN();
    for (const auto& [sat, n_total_raw] : per_sat_raw_signal_count) {
        if (per_sat.count(sat) != 0) continue;   // already emitted in pass 1

        DropReason reason = DropReason::NoModel;   // fallback
        auto it = drop_reasons.find(sat);
        if (it != drop_reasons.end() && it->second != DropReason::None) {
            reason = it->second;
        }

        int code;
        const char* label;
        switch (reason) {
            case DropReason::BelowMask: code = 4; label = "BelowMask"; break;
            case DropReason::NoProduct: code = 6; label = "NoProduct"; break;
            case DropReason::Eclipsing: code = 8; label = "Eclipsing"; break;
            case DropReason::NoModel:
            default:                    code = 7; label = "NoModel";   break;
        }
        emitRow(sat, code, label, kNaN, kNaN, 0, n_total_raw);
    }
}

void TraceLogger::logDops(const GnssTime& time,
                          const std::vector<PppObservation>& modeled_obs,
                          const EpochMeasurement& applied_meas,
                          bool have_applied,
                          const StateManager& state_manager) const {
    if (!dop_trace_.is_open()) return;

    const double kNaN = std::numeric_limits<double>::quiet_NaN();

    // -------------------------------------------------------------------
    // Build the "used sat" set: anything that contributed to the update.
    // For BSSD this is the non-pivot rows in applied_meas plus the pivot
    // (which contributes via the S-transform but doesn't appear as an
    // observation row).  For PPP the BSSD-pivot set is empty and only
    // applied_meas.satellite_ids matter.
    // -------------------------------------------------------------------
    std::set<std::string> used_sats;
    if (have_applied) {
        for (const auto& sat_full : applied_meas.satellite_ids) {
            const auto dash = sat_full.find('-');   // BSSD compound: "primary-pivot"
            std::string sat = (dash == std::string::npos)
                                  ? sat_full
                                  : sat_full.substr(0, dash);
            used_sats.insert(std::move(sat));
        }
    }
    for (const auto& sat : state_manager.getFrozenPivotSatellites()) {
        used_sats.insert(sat);
    }

    // -------------------------------------------------------------------
    // Per-system counts + geometry collection (one entry per *used* sat,
    // deduplicating multi-signal rows in modeled_obs).
    // -------------------------------------------------------------------
    auto systemFor = [](char c) -> const char* {
        switch (c) {
            case 'G': return "GPS";
            case 'E': return "Galileo";
            case 'R': return "GLONASS";
            case 'C': return "BeiDou";
            case 'J': return "QZSS";
            case 'S': return "SBAS";
            default:  return "Unknown";
        }
    };
    std::map<std::string, int> per_sys;
    auto count_of = [&](const char* sys) -> int {
        auto it = per_sys.find(sys);
        return it != per_sys.end() ? it->second : 0;
    };

    struct DopSat { double elev_rad; double az_rad; };
    std::vector<DopSat> dop_sats;
    dop_sats.reserve(used_sats.size());

    std::set<std::string> seen;
    int n_total = 0;
    for (const auto& obs : modeled_obs) {
        if (obs.satellite.empty()) continue;
        if (!used_sats.count(obs.satellite)) continue;
        if (!seen.insert(obs.satellite).second) continue;  // dedupe across signals

        ++per_sys[systemFor(obs.satellite[0])];
        ++n_total;

        const double el_rad = obs.elevation * Constants::PI / 180.0;
        const double az_rad = obs.azimuth   * Constants::PI / 180.0;
        if (std::isfinite(el_rad) && std::isfinite(az_rad) && el_rad > 0.0) {
            dop_sats.push_back({el_rad, az_rad});
        }
    }

    // -------------------------------------------------------------------
    // Position DOPs: geometry-only H built from used sats' (el, az) with
    // a single clock column.  Multi-system ISB columns are intentionally
    // not modelled here -- the standard "geometric DOP" reported by GNSS
    // receivers is the single-system version that ignores ISBs.
    //
    // Need at least 4 sats for a non-singular HᵀH.  Otherwise emit NaN
    // DOPs so the MATLAB plot shows a gap rather than a misleading value.
    // -------------------------------------------------------------------
    double pdop = kNaN, hdop = kNaN, vdop = kNaN, tdop = kNaN, gdop = kNaN;
    if (dop_sats.size() >= 4) {
        Eigen::MatrixXd H(static_cast<Eigen::Index>(dop_sats.size()), 4);
        for (size_t i = 0; i < dop_sats.size(); ++i) {
            const double ce = std::cos(dop_sats[i].elev_rad);
            const double se = std::sin(dop_sats[i].elev_rad);
            const double ca = std::cos(dop_sats[i].az_rad);
            const double sa = std::sin(dop_sats[i].az_rad);
            // LOS in ENU: (sa·ce, ca·ce, se).  Observation equation
            // signs put -LOS in the position columns and +1 in the
            // clock column.
            H(static_cast<Eigen::Index>(i), 0) = -sa * ce;
            H(static_cast<Eigen::Index>(i), 1) = -ca * ce;
            H(static_cast<Eigen::Index>(i), 2) = -se;
            H(static_cast<Eigen::Index>(i), 3) =  1.0;
        }
        Eigen::Matrix4d HtH = H.transpose() * H;
        Eigen::Matrix4d Q;
        bool invertible = false;
        HtH.computeInverseWithCheck(Q, invertible);
        if (invertible) {
            const double q_e = Q(0,0), q_n = Q(1,1), q_u = Q(2,2), q_t = Q(3,3);
            hdop = std::sqrt(std::max(0.0, q_e + q_n));
            vdop = std::sqrt(std::max(0.0, q_u));
            pdop = std::sqrt(std::max(0.0, q_e + q_n + q_u));
            tdop = std::sqrt(std::max(0.0, q_t));
            gdop = std::sqrt(std::max(0.0, q_e + q_n + q_u + q_t));
        }
    }

    // -------------------------------------------------------------------
    // ADOP: Teunissen ambiguity DOP = (det Q_a)^(1/(2n)), in meters.
    //
    // Q_a is the ambiguity-subblock of the filter covariance.  Frozen
    // pivot ambiguities are EXCLUDED -- their variance is zero by BSSD
    // construction (S-transform freezes them to value=0, var=0) and
    // including them would force det(Q_a) -> 0 -> ADOP -> 0 regardless
    // of actual ambiguity precision.
    //
    // Computation: eigenvalue decomposition of Q_a, then
    //   log_det = sum(log(eigenvalue_i)) over the positive eigenvalues
    //   adop_m = exp(log_det / (2 * n_eff))
    // Eigenvalue decomposition is O(n³) but n is the ambiguity count
    // (~50-100), and the alternative (Cholesky) breaks down on near-
    // singular Q_a.
    // -------------------------------------------------------------------
    double adop_m = kNaN;
    int n_amb_for_adop = 0;
    {
        const auto& amb_map = state_manager.getAmbiguities();
        if (!amb_map.empty()) {
            const Eigen::MatrixXd cov = state_manager.getCovariance();
            const auto& pivots = state_manager.getFrozenPivotSatellites();

            std::vector<Eigen::Index> amb_idx;
            amb_idx.reserve(amb_map.size());
            for (const auto& [key, idx] : amb_map) {
                // Skip frozen-pivot ambiguities.  Key format starts with
                // the satellite ID (UC: "<sat>_<sig>", IF: "<sat>_<sig1>_<sig2>",
                // DD: "DD|<cons>|<sat>|<rov>|<pivot>"); for DD ambiguities
                // the leading "DD|" prefix means no satellite-name pivot
                // collision, so the pivots check naturally misses them.
                const auto u1 = key.find('_');
                std::string sat = (u1 == std::string::npos) ? key : key.substr(0, u1);
                if (pivots.count(sat)) continue;

                if (idx >= static_cast<size_t>(cov.rows())) continue;
                const double var = cov(static_cast<Eigen::Index>(idx),
                                       static_cast<Eigen::Index>(idx));
                if (!std::isfinite(var) || var <= 1.0e-15) continue;

                amb_idx.push_back(static_cast<Eigen::Index>(idx));
            }

            if (!amb_idx.empty()) {
                const int n = static_cast<int>(amb_idx.size());
                Eigen::MatrixXd Qa(n, n);
                for (int i = 0; i < n; ++i) {
                    for (int j = 0; j < n; ++j) {
                        Qa(i, j) = cov(amb_idx[i], amb_idx[j]);
                    }
                }
                Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eig(Qa);
                if (eig.info() == Eigen::Success) {
                    const auto& evals = eig.eigenvalues();
                    double log_det_sum = 0.0;
                    int n_eff = 0;
                    for (int i = 0; i < evals.size(); ++i) {
                        const double lam = evals(i);
                        if (std::isfinite(lam) && lam > 1.0e-15) {
                            log_det_sum += std::log(lam);
                            ++n_eff;
                        }
                    }
                    if (n_eff > 0) {
                        adop_m = std::exp(log_det_sum / (2.0 * n_eff));
                        n_amb_for_adop = n_eff;
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------------
    // Write row.  Format: counts as ints, DOPs at 4 sig figs, ADOP at 6.
    // -------------------------------------------------------------------
    dop_trace_ << std::fixed
               << time.week << "," << std::setprecision(3) << time.tow << ","
               << n_total << ","
               << count_of("GPS")     << ","
               << count_of("Galileo") << ","
               << count_of("GLONASS") << ","
               << count_of("BeiDou")  << ","
               << count_of("QZSS")    << ","
               << count_of("SBAS")    << ","
               << std::setprecision(4)
               << pdop << "," << hdop << "," << vdop << "," << tdop << "," << gdop << ","
               << std::setprecision(6) << adop_m << ","
               << n_amb_for_adop << "\n";
}

void TraceLogger::logResiduals(const GnssTime& time, const EpochMeasurement& m) const {
    if (!residual_trace_.is_open()) return;

    const Eigen::VectorXd& innov = m.innovations;
    const Eigen::VectorXd& res = m.residuals;
    const Eigen::VectorXd& sres = m.residuals_standardized;
    const Eigen::MatrixXd& Sr = m.residuals_covariance;

    const size_t n = std::min({ static_cast<size_t>(innov.size()),
                                static_cast<size_t>(res.size()),
                                static_cast<size_t>(sres.size()),
                                m.satellite_ids.size(),
                                m.signal_types.size() });

    auto diag_std = [&Sr](size_t i) -> double {
        if (i < static_cast<size_t>(Sr.rows()) && i < static_cast<size_t>(Sr.cols())) {
            const double v = Sr(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i));
            return std::sqrt(std::max(0.0, v));
        }
        return std::numeric_limits<double>::quiet_NaN();
    };

    // Baarda QC columns (item #9).  These vectors are empty unless
    // estimation.enable_data_snooping is on; emit NaN per row when absent so
    // the CSV column count stays constant.
    auto qc = [](const Eigen::VectorXd& v, size_t i) -> double {
        return (i < static_cast<size_t>(v.size()))
            ? v(static_cast<Eigen::Index>(i))
            : std::numeric_limits<double>::quiet_NaN();
    };

    for (size_t i = 0; i < n; ++i) {
        residual_trace_ << std::fixed << std::setprecision(3)
                        << time.week << "," << time.tow << ",";
        residual_trace_ << m.satellite_ids[i] << "," << m.signal_types[i] << ",";
        residual_trace_ << std::setprecision(6)
                        << innov(static_cast<Eigen::Index>(i)) << ","
                        << res(static_cast<Eigen::Index>(i)) << ","
                        << diag_std(i) << ","
                        << sres(static_cast<Eigen::Index>(i)) << ","
                        << qc(m.residual_redundancy, i) << ","
                        << qc(m.residual_wtest, i) << ","
                        << qc(m.residual_mdb, i) << "\n";
    }
}

void TraceLogger::logWls(const GnssTime& time,
                         const WlsResult& wls,
                         double wls_clock_bias_m,
                         double kf_clock_bias_m,
                         double delta_clock_m,
                         bool jump_detected,
                         bool should_seed) const {
    if (!wls_trace_.is_open() || !wls.success() || wls.state.size() < 5) return;

    wls_trace_ << std::fixed << std::setprecision(3)
               << time.week << "," << time.tow << ",";
    wls_trace_ << wls.iterations << "," << wls.observation_count << ",";
    wls_trace_ << std::setprecision(6)
               << wls.state(0) << "," << wls.state(1) << "," << wls.state(2) << ","
               << wls.state(3) << "," << wls.state(4) << ",";
    wls_trace_ << wls.rms << "," << wls.max_abs << ",";
    wls_trace_ << wls_clock_bias_m << "," << kf_clock_bias_m << "," << delta_clock_m << ",";
    wls_trace_ << (jump_detected ? 1 : 0) << ","
               << (should_seed ? 1 : 0) << "\n";
}

} // namespace GnssProcessor
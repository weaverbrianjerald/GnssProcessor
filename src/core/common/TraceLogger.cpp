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

#include "TraceLogger.hpp"
#include <iomanip>
#include <limits>
#include <Eigen/Dense>

namespace GnssProcessor {

TraceLogger::TraceLogger(LogManager& log_manager, const std::filesystem::path& log_dir, bool is_ppp_rtk)
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
    const auto residual_path = log_dir / "measurement_residuals.csv";
    const auto wls_path = log_dir / "wls_trace.csv";

    state_trace_.open(state_path, std::ios::out | std::ios::trunc);
    if (!state_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open state trace file '{}'", state_path.string()); }
        catch (...) {}
    }

    ambiguity_trace_.open(amb_path, std::ios::out | std::ios::trunc);
    if (!ambiguity_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open ambiguity trace file '{}'", amb_path.string()); }
        catch (...) {}
    }

    residual_trace_.open(residual_path, std::ios::out | std::ios::trunc);
    if (!residual_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open residual trace file '{}'", residual_path.string()); }
        catch (...) {}
    }

    wls_trace_.open(wls_path, std::ios::out | std::ios::trunc);
    if (!wls_trace_.is_open()) {
        try { log_manager_.getLogger("main_log")->warn("Could not open WLS trace file '{}'", wls_path.string()); }
        catch (...) {}
    }

    // Write base header to state trace file but do NOT terminate with newline.
    // Dynamic time-offset columns are appended on the first call to logState().
    if (state_trace_.is_open()) {
        state_trace_ << "week,tow_s,x_m,y_m,z_m,x_std_m,y_std_m,z_std_m,clock_m,clock_std_m,tropo_m,tropo_std_m";
        if (is_ppp_rtk_) state_trace_ << ",iono_m,iono_std_m";
        // Do NOT write newline here - dynamic time-offset columns will be appended in logState()
    }

    if (ambiguity_trace_.is_open()) {
        ambiguity_trace_ << "week,tow_s,key,value_m,std_m\n";
    }

    if (residual_trace_.is_open()) {
        residual_trace_ << "week,tow_s,satellite,signal,innovation_m,residual_m,residual_std_m,standardized_residual\n";
    }

    if (wls_trace_.is_open()) {
        wls_trace_ << "week,tow_s,iterations,obs_count,x_m,y_m,z_m,clock_m,tropo_m,rms_m,max_abs_m,"
                   << "wls_clock_m,kf_clock_m,delta_clock_m,jump_detected,should_seed\n";
    }

    // Consider the logger initialized if at least one file is writable.
    initialized_ = state_trace_.is_open() || ambiguity_trace_.is_open() || residual_trace_.is_open() || wls_trace_.is_open();

    if (!initialized_) {
        try { log_manager_.getLogger("main_log")->error("TraceLogger failed to open any trace files in '{}'", log_dir.string()); }
        catch (...) {}
    }
}

bool TraceLogger::isInitialized() const {
    return initialized_;
}

void TraceLogger::logState(const GnssTime& time, const StateManager& state_manager) const {
if (!state_trace_.is_open()) return;

// On first call, complete the header line with dynamic time-offset columns
if (!state_header_written_) {
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
    const size_t clk_idx = state_manager.getClockIndex();
    const size_t trop_idx = state_manager.getTropoIndex();
    const bool has_pos = sv.size() >= pos_idx + 3;
    const bool has_clk = clk_idx < static_cast<size_t>(sv.size());
    const bool has_trop = trop_idx < static_cast<size_t>(sv.size());
    const bool has_iono = is_ppp_rtk_ && state_manager.getIonoIndex() < static_cast<size_t>(sv.size());

    if (!(has_pos && has_clk && has_trop && (!is_ppp_rtk_ || has_iono))) {
        try { log_manager_.getLogger("main_log")->warn(
            "[{} {:.3f}] Skipped state trace row: state size {} insufficient (pos_idx={}, clk_idx={}, trop_idx={})",
            time.week, time.tow, sv.size(), pos_idx, clk_idx, trop_idx); }
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
    state_trace_ << sv(static_cast<Eigen::Index>(clk_idx)) << ","
                 << std_at(clk_idx) << ",";
    state_trace_ << sv(static_cast<Eigen::Index>(trop_idx)) << ","
                 << std_at(trop_idx);
    if (is_ppp_rtk_) {
        const size_t iono_idx = state_manager.getIonoIndex();
        state_trace_ << ","
                     << sv(static_cast<Eigen::Index>(iono_idx)) << ","
                     << std_at(iono_idx);
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

    for (size_t i = 0; i < n; ++i) {
        residual_trace_ << std::fixed << std::setprecision(3)
                        << time.week << "," << time.tow << ",";
        residual_trace_ << m.satellite_ids[i] << "," << m.signal_types[i] << ",";
        residual_trace_ << std::setprecision(6)
                        << innov(static_cast<Eigen::Index>(i)) << ","
                        << res(static_cast<Eigen::Index>(i)) << ","
                        << diag_std(i) << ","
                        << sres(static_cast<Eigen::Index>(i)) << "\n";
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
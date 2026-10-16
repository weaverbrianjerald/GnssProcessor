/**
 * @file MwSlipDetectorTrace.cpp
 * @brief The MW detector's CSV trace (mwwl_trace.csv).
 *
 * Opening the trace with its header row, and writing one row per processed sample.
 *
 * Part of MwSlipDetector (slip/MwSlipDetector.hpp lists every file of the class).
 */

#include "slip/MwSlipDetector.hpp"
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>

namespace GnssProcessor {

    using namespace mw;  // the constants table and the per-arc state (slip/MwArcState.hpp)

    // =================================================================
    // Tracing (CSV output)
    // =================================================================

    bool MwSlipDetector::enableTracing(const std::filesystem::path& log_dir) {
        auto logger = log_manager_.getLogger("mw_slip_detector");

        std::error_code ec;
        if (!std::filesystem::exists(log_dir)) {
            std::filesystem::create_directories(log_dir, ec);
            if (ec) {
                logger->warn("Failed to create log directory '{}': {}", log_dir.string(), ec.message());
                return false;
            }
        }

        const auto trace_path = log_dir / "mwwl_trace.csv";
        mwwl_trace_.open(trace_path, std::ios::out | std::ios::trunc);
        if (!mwwl_trace_.is_open()) {
            logger->warn("Could not open MWWL trace file '{}'", trace_path.string());
            return false;
        }

        mwwl_trace_ << "week,tow_s,satellite,system,elevation_deg,"
                    << "sig1,sig2,mw_m,mw_cycles,"
                    << "mw_mean_m,mw_mean_cycles,"
                    << "dev_from_mean_m,dev_from_mean_cycles,"
                    << "mw_sigma_m,mw_sigma_cycles,"
                    << "arc_count,"
                    << "lambda_wl_m,threshold_m,threshold_cycles,"
                    << "cross_sat_floor_m,cross_sat_floor_cycles,"
                    << "is_slip,is_lli,"
                    << "bia_corrected,sat_mw_bias_m,rounded_wl,wl_locked,"
                    << "mw_pivot,"
                    << "local_ref_m,local_ref_cycles,"
                    << "local_sigma_m,local_sigma_cycles,"
                    << "detect_mode,"
                    << "active_dev_m,active_dev_cycles\n";

        logger->info("MWWL tracing enabled: {}", trace_path.string());
        return true;
    }

    void MwSlipDetector::writeTraceRecord(const PppObservation& obs,
                                           const std::string& sig1,
                                           const std::string& sig2,
                                           double mw_m,
                                           double mw_mean,
                                           double dev_from_mean,
                                           double mw_sigma,
                                           size_t arc_count,
                                           double lambda_wl,
                                           double thresh_m,
                                           double cross_sat_floor_m,
                                           bool is_slip,
                                           bool is_lli,
                                           bool bia_corrected,
                                           double sat_mw_bias_m,
                                           int rounded_wl,
                                           bool wl_locked,
                                           const std::string& mw_pivot,
                                           double local_ref_m,
                                           double local_sigma_m,
                                           int detect_mode,
                                           double active_dev_m) const {
        if (!mwwl_trace_.is_open()) return;

        const auto to_cyc = [&](double val_m) -> double {
            return (lambda_wl > 0.0 && std::isfinite(val_m)) ? val_m / lambda_wl
                                                             : std::numeric_limits<double>::quiet_NaN();
        };

        const double elev_deg = std::isfinite(obs.elevation) ? obs.elevation
                                                             : std::numeric_limits<double>::quiet_NaN();

        mwwl_trace_ << std::fixed << std::setprecision(3)
                    << current_epoch_.week << "," << current_epoch_.tow << ","
                    << obs.satellite << ","
                    << PppUtils::getSystemString(obs.system) << ","
                    << std::setprecision(2) << elev_deg << ","
                    << sig1 << "," << sig2 << ","
                    << std::setprecision(6) << mw_m << "," << to_cyc(mw_m) << ",";

        auto csv_pair = [&](double val_m) {
            if (std::isfinite(val_m))
                mwwl_trace_ << val_m << "," << to_cyc(val_m) << ",";
            else
                mwwl_trace_ << ",,";
        };
        csv_pair(mw_mean);
        csv_pair(dev_from_mean);
        csv_pair(mw_sigma);

        mwwl_trace_ << arc_count << ","
                    << lambda_wl << "," << thresh_m << "," << to_cyc(thresh_m) << ",";
        csv_pair(cross_sat_floor_m);   // diagnostic: the cross-sat warmup floor (m + cycles)
        mwwl_trace_ << (is_slip ? 1 : 0) << "," << (is_lli ? 1 : 0) << ","
                    << (bia_corrected ? 1 : 0) << ","
                    << std::setprecision(6) << sat_mw_bias_m << ","
                    << rounded_wl << "," << (wl_locked ? 1 : 0) << ","
                    << mw_pivot << ",";
        csv_pair(local_ref_m);
        csv_pair(local_sigma_m);
        mwwl_trace_ << detect_mode << ",";
        if (std::isfinite(active_dev_m))
            mwwl_trace_ << active_dev_m << "," << to_cyc(active_dev_m) << "\n";
        else
            mwwl_trace_ << ",\n";
    }

} // namespace GnssProcessor

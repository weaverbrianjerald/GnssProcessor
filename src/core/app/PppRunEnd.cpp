/**
 * @file PppRunEnd.cpp
 * @brief The run end of the PPP / PPP-RTK pipeline as a declared table of reports.
 *
 * When the epoch loop has ended, Application::runPpp builds one PppRunEndView and calls
 * reportPppRunEnd_, which walks kPppRunEndReports top to bottom.  A row is one report: a
 * private member that reads the Application's components and the view, gates itself on
 * the knob that arms its subsystem, and prints its lines (or, for the smoother, runs the
 * backward pass and writes its trace).  The table is the run end's table of contents;
 * each report is one block of the former single Application::run, moved unchanged
 *.
 *
 * The order is the former run end's: the processing-time line measures the wall from the
 * start of the epoch loop to the moment its row runs, so the smoother and the NEQ export
 * stay before it.  The HTML report (Application::generateReport_) runs after the table,
 * from runPpp, once every trace is written.
 *
 * Adding a report: write the member here, declare it with its Doxygen in
 * app/Application.hpp beside the others, and add one row where its lines belong.
 */

#include "app/Application.hpp"
#include "app/PppPipelineWork.hpp"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <fmt/format.h>

namespace GnssProcessor {


// ---------------------------------------------------------------------------
// The run-end table and its walk
// ---------------------------------------------------------------------------

void Application::reportPppRunEnd_(const PppRunEndView& v) {
    using Report = void (Application::*)(const PppRunEndView&);
    struct Row {
        const char* name;      // what the report prints
        const char* armed_by;  // the knob that arms it
        Report      report;    // the report
    };
    static const Row kPppRunEndReports[] = {
        { "wind-up census",             "estimation.trace_windup_census", &Application::reportWindUpCensus_ },
        { "RTS smoother and its trace", "processing.run_smoother",        &Application::runSmootherPass_ },
        { "normal-equation export",     "processing.enable_neq_export",   &Application::exportNormalEquations_ },
        { "processing time",            "always",                         &Application::reportProcessingTime_ },
        { "state re-inflations",        "always",                         &Application::reportReinflations_ },
        { "Kalman filter timings",      "always",                         &Application::reportKalmanFilterRunEnd_ },
        { "cycle-slip counters",        "always",                         &Application::reportCycleSlipRunEnd_ },
    };

    for (const Row& row : kPppRunEndReports) (this->*row.report)(v);
}

// ---------------------------------------------------------------------------
// Report: the wind-up census (estimation.trace_windup_census)
// ---------------------------------------------------------------------------

void Application::reportWindUpCensus_(const PppRunEndView&) {
    // --- Wind-up census readout (estimation.trace_windup_census) ------------
    // Single-receiver counterpart of the network teardown's roster sum.  The
    // per-day step rate in SPEC_phase_windup_sign §7 comes from this path, so a
    // PPP run with the census on cross-checks the network census calibration.
    if (config_.traceWindUpCensus() && ppp_model_) {
        const auto& w = ppp_model_->windUpCensus();
        auto wu_log = log_manager_.getLogger("ppp_model");
        const bool acc = config_.accumulatePhaseWindUp();
        if (wu_log) {
            wu_log->info("Wind-up census (mode {}, horizon {:.0f} s): {} forward "
                         "transitions ({} out-of-order, excluded).  IN-horizon: {} "
                         "transitions, {} step(s) >= 0.9 cyc, mean |step| {:.4f} cyc, "
                         "max {:.3f}.  OUT-of-horizon: {} transitions, {} stepped "
                         ">= 0.9 cyc (max {:.3f}).  Accumulator RESTARTS (its own "
                         "verdict, not a threshold): {}.",
                         acc ? "ACCUMULATED" : "fractional",
                         config_.phaseWindUpMaxGapS(), w.steps, w.backwards,
                         w.gap_lt_wu, w.wraps,
                         w.gap_lt_wu > 0
                             ? w.sum_abs_step_cyc / static_cast<double>(w.gap_lt_wu)
                             : 0.0,
                         w.max_abs_step_in_cyc,
                         w.steps - w.gap_lt_wu, w.gap_steps, w.max_abs_step_gap_cyc,
                         w.restarts);
            wu_log->info("Wind-up census READING: {}  0 transitions on an armed run = "
                         "dead wiring (invariant #4).",
                         acc ? "`wraps` is 0 by CONSTRUCTION in accumulated mode — every "
                               "in-horizon transition is unwrapped into ±0.5 cyc, so a "
                               "threshold on the applied step is blind; the restart "
                               "population is the count reported above, and each restart "
                               "steps the model by an exact integer cycle."
                             : "the in-horizon steps >= 0.9 cyc ARE the one-cycle "
                               "staircase (SPEC_phase_windup_sign §7); this is the "
                               "single-receiver cross-check on the network census, and "
                               "the path §7's own 43/day figure came from.");
        }
    }
}

// ---------------------------------------------------------------------------
// Report: the backward RTS smoother (processing.run_smoother)
// ---------------------------------------------------------------------------

void Application::runSmootherPass_(const PppRunEndView&) {
    // --- Phase 5: Optional backward RTS smoother pass ---
    if (config_.runSmoother() && !smoother_buffer_.empty()) {
        auto smooth_log = log_manager_.getLogger("smoother");
        if (smooth_log) smooth_log->info("Running backward RTS smoother over {} epochs...",
                                         smoother_buffer_.size());

        Smoother smoother(std::move(smoother_buffer_), log_manager_);
        if (smoother.run() && config_.getLoggingConfig().enable_smoother_trace) {
            writeSmootherTrace_(smoother);
        }
    }
}

// ---------------------------------------------------------------------------
// The smoother trace (logging.enable_smoother_trace)
// ---------------------------------------------------------------------------

void Application::writeSmootherTrace_(const Smoother& smoother) {
    const std::filesystem::path out_path = log_dir_ / "smoother_trace.csv";
    std::ofstream ofs(out_path);
    if (!ofs) {
        log_manager_.getLogger("main_log")->error(
            "Failed to open smoother trace file: {}", out_path.string());
    } else {
        ofs << "week,tow,x,y,z,sigma_x,sigma_y,sigma_z,"
               "clock_m,sigma_clock_m,tropo_zwd_m,sigma_tropo_m,smoothed\n";
        const auto& results = smoother.results();
        auto sig = [](const Eigen::MatrixXd& P, size_t i) {
            const double v = P(static_cast<Eigen::Index>(i),
                               static_cast<Eigen::Index>(i));
            return (std::isfinite(v) && v >= 0.0) ? std::sqrt(v) : 0.0;
        };
        // A state the layout does not hold (the receiver clock under BSSD
        // and in baseline mode: its index is the max() sentinel) is written
        // as NaN, as the other traces do; indexing x with the sentinel read
        // outside the vector.
        constexpr double kAbsent = std::numeric_limits<double>::quiet_NaN();
        auto value_or_nan = [&](const Eigen::VectorXd& x, size_t i) {
            return i < static_cast<size_t>(x.size()) ? x[static_cast<Eigen::Index>(i)] : kAbsent;
        };
        auto sig_or_nan = [&](const Eigen::MatrixXd& P, size_t i) {
            return i < static_cast<size_t>(P.rows()) ? sig(P, i) : kAbsent;
        };
        for (const auto& s : results) {
            const size_t px = s.pos_idx;
            ofs << s.t.week << ","
                << fmt::format("{:.3f}", s.t.tow) << ","
                << fmt::format("{:.4f}", s.x[static_cast<Eigen::Index>(px)])     << ","
                << fmt::format("{:.4f}", s.x[static_cast<Eigen::Index>(px + 1)]) << ","
                << fmt::format("{:.4f}", s.x[static_cast<Eigen::Index>(px + 2)]) << ","
                << fmt::format("{:.4f}", sig(s.P, px))     << ","
                << fmt::format("{:.4f}", sig(s.P, px + 1)) << ","
                << fmt::format("{:.4f}", sig(s.P, px + 2)) << ","
                << fmt::format("{:.4f}", value_or_nan(s.x, s.clock_idx)) << ","
                << fmt::format("{:.4f}", sig_or_nan(s.P, s.clock_idx)) << ","
                << fmt::format("{:.4f}", s.x[static_cast<Eigen::Index>(s.tropo_idx)]) << ","
                << fmt::format("{:.4f}", sig(s.P, s.tropo_idx)) << ","
                << (s.smoothed ? 1 : 0) << "\n";
        }
        log_manager_.getLogger("main_log")->info(
            "Smoother trace written: {} ({} epochs)", out_path.string(), results.size());
    }
}

// ---------------------------------------------------------------------------
// Report: the normal-equation export (processing.enable_neq_export)
// ---------------------------------------------------------------------------

void Application::exportNormalEquations_(const PppRunEndView&) {
    // --- Phase 5b: Cross-session normal-equation export (item #13B) ---
    // The accumulator mirrored each accepted update's (H, R, innovation) into a
    // label-keyed normal system with the epoch-local states Schur-eliminated.
    // x̂ = N⁻¹u is a Gauss-Newton increment about the per-epoch operating points,
    // not an absolute solution (≈0 for a converged static run, which confirms
    // the filter solution is a stationary point of the batch system); absolute
    // coordinates are the final filter state plus that increment.  N, u, the
    // labels and σ̂₀² form the combinable product written to .neq/.asc.
    if (neq_accumulator_ && !neq_accumulator_->empty()) {
        const NeqAccumulator::Solution sol = neq_accumulator_->solve();
        const std::filesystem::path stem = log_dir_ / "normal_equations";
        const bool wrote = neq_accumulator_->writeFiles(stem, sol);
        auto neq_log = log_manager_.getLogger("main_log");
        if (sol.ok) {
            double dx = 0.0, dy = 0.0, dz = 0.0;
            for (std::size_t i = 0; i < sol.labels.size(); ++i) {
                const auto& lab = sol.labels[i];
                const double xi = sol.x(static_cast<Eigen::Index>(i));
                if (lab == "POS_X") dx = xi;
                else if (lab == "POS_Y") dy = xi;
                else if (lab == "POS_Z") dz = xi;
            }
            const Eigen::Vector3d final_pos =
                state_manager_->getStateVector().head<3>();
            const double incr = std::sqrt(dx * dx + dy * dy + dz * dz);
            neq_log->info(
                "NEQ export: {} params, {} obs, {} local eliminated, dof={:.1f}, "
                "sigma0^2={:.3f}",
                sol.n_params, sol.n_obs, sol.n_local, sol.dof, sol.sigma0_sq);
            neq_log->info(
                "NEQ position: increment |dx_pos|={:.4f} m, "
                "absolute=({:.4f}, {:.4f}, {:.4f}) m",
                incr, final_pos.x() + dx, final_pos.y() + dy, final_pos.z() + dz);
        } else {
            neq_log->warn(
                "NEQ export: system not solvable ({} params, {} obs); "
                "matrices still written for inspection",
                sol.n_params, sol.n_obs);
        }
        if (wrote)
            neq_log->info("Normal equations written: {}.neq / {}.asc",
                          stem.string(), stem.string());
        else
            neq_log->error("Failed to write normal-equation export to {}.*",
                           stem.string());
    }
}

// ---------------------------------------------------------------------------
// Report: the processing time
// ---------------------------------------------------------------------------

void Application::reportProcessingTime_(const PppRunEndView& v) {
    const auto& wall_start = v.wall_start;
    const bool is_ppp_rtk = v.is_ppp_rtk;

    auto wall_end = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_end - wall_start).count();
    log_manager_.getLogger("main_log")->info("Processing completed in {} ms, mode={}",
        duration_ms, is_ppp_rtk ? "PPP-RTK" : "PPP");
}

// ---------------------------------------------------------------------------
// Report: the state re-inflation counters
// ---------------------------------------------------------------------------

void Application::reportReinflations_(const PppRunEndView& v) {
    const bool is_ppp_rtk = v.is_ppp_rtk;

    // Re-inflation counters from the state manager.  A re-inflation
    // never lowers a variance: when the requested value is below the current one,
    // the current one is kept, and each such case is a "kept" count.
    if (state_manager_)
        log_manager_.getLogger("main_log")->info(
            "card OWD re-inflations ({}): {}", is_ppp_rtk ? "PPP-RTK" : "PPP",
            StateManager::reinflateSummary(state_manager_->reinflateCounters()));
}

// ---------------------------------------------------------------------------
// Report: the Kalman filter's timings and QC counters
// ---------------------------------------------------------------------------

void Application::reportKalmanFilterRunEnd_(const PppRunEndView&) {
    if (kalman_filter_) kalman_filter_->logKalmanFilterTimings();
}

// ---------------------------------------------------------------------------
// Report: the cycle-slip counters
// ---------------------------------------------------------------------------

void Application::reportCycleSlipRunEnd_(const PppRunEndView&) {
    if (cycle_slip_manager_) cycle_slip_manager_->logRunEndCounters();
}

} // namespace GnssProcessor

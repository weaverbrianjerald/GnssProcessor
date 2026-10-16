/**
 * @file NetworkTeardown.cpp
 * @brief Implementation of @ref networkTeardown, the network producer's run end: the walk of
 *        the declared run-end reports (filter/NetworkRunEnd.hpp) and the exit code.
 *
 * Until card NOM step TD1b this file held the whole run end, one function of 7 189 lines;
 * its sections now live beside the subsystems they report, and this function only orders
 * them.  The function below is written by scripts/nom_td_cut.py, and
 * scripts/nom_td_verify.py proves its zero-emit guard against the frozen function.
 */

#include "network/runend/NetworkTeardown.hpp"

#include "network/state/NetworkRunContext.hpp"
#include "network/runend/NetworkRunEnd.hpp"
#include "network/driver/NetworkRunGroups.hpp"

#include <spdlog/spdlog.h>

#include <chrono>

namespace GnssProcessor {

// ---- Written by scripts/nom_td_cut.py (card NOM step TD1b); the row call edited by hand in step TD2 ----

int networkTeardown(const NetworkRunContext& ctx,
                    const GnssConfig& config_,
                    const std::shared_ptr<spdlog::logger>& log,
                    NetworkRunTraces& traces,
                    NetworkSetupResult& setup,
                    const NetworkRunEndInputs& run) {
    // The epoch-loop wall, from run.timing.loop_begin (taken just before the estimator is
    // built), measured once before any report runs.  The timing report, the manifest's
    // complete record and the run-summary CSV read this one value.
    const double loop_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - run.timing.loop_begin).count();
    const NetworkRunEndView view{ctx, config_, log, traces, setup, run, loop_ms};

    // Every report, in the declared order.  A report returns 0, or a verdict for the exit
    // code; a later non-zero verdict replaces an earlier one, so the table's order is the
    // precedence (the injection verdict over the joint-block PD-fallback bar).
    int verdict = 0;
    for (const NetworkRunEndRow& row : kNetworkRunEndReports) {
        const int rc = networkRunEndCallRow(row, view);   // with its heading line (TD2)
        if (rc != 0) verdict = rc;
    }

    // A run that emitted nothing failed, whatever the reports returned.  Checked after the
    // walk, so the injection clause, the canonical-pair census, the roster and the other
    // tail reports are on record for a run that produced nothing.
    if (ctx.counters.n_emitted == 0) {
        log->error("Network: produced ZERO solution epochs. Common causes: stations "
                   "do not overlap in time; the local nuisance block is rank-"
                   "deficient (disable receiver code-bias / phase-clock estimation "
                   "for the network config); or the shared datum is under-specified — "
                   "a near-null W6 code-bias direction withholds every epoch when sparse "
                   "(often BeiDou) satellites register OSB columns with < 2 IF-pinnable "
                   "signals.  Arm the run-latched admission census (network.cb_admission; "
                   "tune network.cb_admission_min_stations / _census_epochs) so those "
                   "pairs stay station-local sponges, and confirm >= 2 overlapping stations.");
        return 1;
    }
    return verdict;
}

// ---- End of the generated region ----

} // namespace GnssProcessor

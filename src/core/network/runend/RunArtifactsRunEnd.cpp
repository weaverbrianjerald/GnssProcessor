/**
 * @file RunArtifactsRunEnd.cpp
 * @brief Run-end reports of the run's artifacts: the trace streams' close, the run manifest's complete record, the flag-rate and run-summary CSVs.
 *
 * The writers are NetworkRunTraces's (network/diag/RunTraces.cpp); these reports hand
 * them the estimator's tolerance, the synced epoch count and the station detectors' flag
 * rates, so the writers stay free of the estimator and the station detectors.
 *
 * The table that orders the reports is network/runend/RunEnd.cpp.
 */

#include "network/runend/RunArtifactsRunEnd.hpp"

#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/solve/Estimator.hpp"
#include "network/assembly/MultiGnss.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/driver/RunGroups.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/diag/RunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "slip/IfPhaseSlipDetector.hpp"

#include <memory>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

// ---- Run-end reports ----

// Run-end report: trace close: the estimability RUN row, then the trace streams closed.
int networkTraceCloseRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    NetworkRunTraces& traces = v.traces;
    const NetworkEstimator& est = v.run.engines.est;

    // The run-wide rank-gate telemetry goes into network_estimability.csv, the one
    // artifact a trace-armed run adds, and not into a new run-end log line: golden
    // comparisons byte-compare the log text, and an unarmed control run has to keep
    // identical text.  No-op with the trace off.
    traces.writeEstimabilityRunRow(ctx.counters, est.rankGateTol());
    traces.closeAll();   // closes the streams listed in NetworkRunTraces::closeAll; the rest close in its destructor
    return 0;
}

// Run-end report: the run manifest's complete record.
int networkRunManifestCompleteRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    NetworkRunTraces& traces = v.traces;
    const double loop_ms = v.loop_ms;
    const NetworkConfig& nc = v.config.getNetworkConfig();
    const std::vector<SyncedEpoch>& synced = v.setup.sync->getSyncedEpochs();   // for its size: the batch epoch list

    // End-of-run manifest record; the body is in NetworkRunTraces.
    traces.writeRunManifestComplete(nc, ctx.counters, synced.size(), loop_ms,
                                    networkMultiGnssManifestJson(ctx, config_));
    return 0;
}

// Run-end report: flag rates, the run summary and the shared-product line.
int networkRunSummaryRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    NetworkRunTraces& traces = v.traces;
    const double loop_ms = v.loop_ms;
    const std::vector<NetStation>& stations = v.setup.stations;
    const std::vector<SyncedEpoch>& synced = v.setup.sync->getSyncedEpochs();   // for its size: the batch epoch list
    const bool parallel_on = v.setup.parallel_on;
    const int n_threads = v.setup.n_threads;
    const auto& trace_path = v.traces.trace_path;

    // Collect the IF-TD reference arm's own opportunities and flags from the per-station
    // detectors (flag-rate tier T1r).  Not gated on nc.slip_qc.ref_gate: the legacy
    // reference condition runs and acts whether or not the hardened gate is armed, so
    // gating this on the hardening toggle would leave the tier reading 0 on the runs
    // where it is the only reference screen.
    FlagRate fr_if_refgate;   // the IF-TD reference arm's flag rate, summed below for the writer
    if (ctx.knobs.trace_flag_rates) {
        auto& fr = fr_if_refgate;
        fr.opp = fr.flag = 0;
        for (const auto& st : stations) {
            if (!st.if_slip) continue;
            const auto& c = st.if_slip->refGateCounters();
            fr.opp  += static_cast<long>(c.gate_opportunities);
            fr.flag += static_cast<long>(c.gate_flags);
        }
    }

    // Flag-rate empirical-alpha summary and run-summary CSV; the bodies are in
    // NetworkRunTraces, and one trace_flag_rates gate covers both.
    traces.writeFlagRateAndRunSummary(config_, ctx.knobs, ctx.counters, fr_if_refgate, synced.size(),
                                      stations.size(), n_threads, parallel_on, loop_ms, log);

    log->info("Network: shared satellite product accumulated in an owned "
              "SsrCorrectionStore (closed-loop sink). Trace: {}", trace_path.string());
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor

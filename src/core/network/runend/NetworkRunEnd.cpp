/**
 * @file NetworkRunEnd.cpp
 * @brief The run-end table, kNetworkRunEndReports: every run-end report of the network
 *        producer in print order, with what it reports and its home file (card NOM step
 *        TD1b), one row per subsystem (card NOM2 step TD2), and the walk's two helpers.
 *        network/runend/NetworkRunEnd.hpp states how to add a row and the order constraints.
 *
 * The rows were written by scripts/nom_td_cut.py (TD1b) and regrouped by subsystem by
 * scripts/nom_td2_cut.py (TD2, proved by scripts/nom_td2_verify.py); since TD2 the table is
 * edited by hand under the rules of the header.  Each row's report is declared in the header
 * of its home file.
 */

#include "network/runend/NetworkRunEnd.hpp"

#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include <string>
#include <vector>

// ---- The table (TD1b's generated region, regrouped by TD2; edited by hand since) ----

#include "network/runend/NetworkClosedLoopRunEnd.hpp"
#include "network/runend/NetworkReadmitRunEnd.hpp"
#include "network/runend/NetworkResumeRunEnd.hpp"
#include "network/runend/NetworkRunArtifactsRunEnd.hpp"
#include "network/runend/NetworkRunTimingRunEnd.hpp"
#include "network/driver/NetworkSetupResult.hpp"
#include "network/runend/NetworkSolveStationsRunEnd.hpp"
#include "network/assembly/SignalRoster.hpp"
#include "network/ar/NetworkAmbFloat.hpp"
#include "network/diag/NetworkAmbStateTrace.hpp"
#include "network/solve/NetworkAnchorHygiene.hpp"
#include "network/ar/NetworkArLattice.hpp"
#include "network/ar/NetworkArSuspect.hpp"
#include "network/assembly/NetworkBiasAdmission.hpp"
#include "network/qc/NetworkBlockBaarda.hpp"
#include "network/emit/NetworkBrdcWire.hpp"
#include "network/assembly/NetworkCanonicalPairAdmission.hpp"
#include "network/state/NetworkCarryDisposition.hpp"
#include "network/state/NetworkCarrySites.hpp"
#include "network/carry/NetworkCarryVceRunEnd.hpp"
#include "network/solve/NetworkChainDatum.hpp"
#include "network/assembly/NetworkChannelAll.hpp"
#include "network/solve/NetworkClockAnchor.hpp"
#include "network/solve/NetworkClockLevel.hpp"
#include "network/qc/NetworkClockQc.hpp"
#include "network/qc/NetworkCongruence.hpp"
#include "network/assembly/NetworkContributions.hpp"
#include "network/carry/NetworkCoupling.hpp"
#include "network/carry/NetworkCouplingExact.hpp"
#include "network/solve/NetworkDatumGraph.hpp"
#include "network/diag/NetworkDdFrac.hpp"
#include "network/emit/NetworkEmitStore.hpp"
#include "network/solve/NetworkEstimator.hpp"
#include "network/assembly/NetworkExtraBandScope.hpp"
#include "network/ar/NetworkFixedCycles.hpp"
#include "network/diag/NetworkFrameDiag.hpp"
#include "network/qc/NetworkGrossLos.hpp"
#include "network/solve/NetworkHandoverStransform.hpp"
#include "network/carry/NetworkInject.hpp"
#include "network/carry/NetworkInvariantCarryDriver.hpp"
#include "network/emit/NetworkLedgerEmit.hpp"
#include "network/carry/NetworkLevelAuthority.hpp"
#include "network/carry/NetworkLevelingCarryRunEnd.hpp"
#include "network/qc/NetworkMdb.hpp"
#include "network/assembly/NetworkMeasurementAdmission.hpp"
#include "network/assembly/NetworkMultiGnss.hpp"
#include "network/slip/NetworkOraclePin.hpp"
#include "network/assembly/NetworkParamInventory.hpp"
#include "network/assembly/NetworkParamRegistry.hpp"
#include "network/carry/NetworkPhaseColourCarry.hpp"
#include "network/solve/NetworkPhaseDatum.hpp"
#include "network/diag/NetworkPhaseVisibleDiag.hpp"
#include "network/qc/NetworkPrefitCensus.hpp"
#include "network/qc/NetworkPriorResidual.hpp"
#include "network/ar/NetworkProducerAr.hpp"
#include "network/qc/NetworkRaim.hpp"
#include "network/qc/NetworkRaimWithinEpoch.hpp"
#include "network/solve/NetworkRankGate.hpp"
#include "network/qc/NetworkRedundancyCensus.hpp"
#include "network/assembly/NetworkResponses.hpp"
#include "network/carry/NetworkRxClockCarry.hpp"
#include "network/carry/NetworkRxCodeBiasCarryRunEnd.hpp"
#include "network/solve/NetworkSbasis.hpp"
#include "network/solve/NetworkSbasisCompletion.hpp"
#include "network/ar/NetworkSdEwlAr.hpp"
#include "network/assembly/NetworkSecondCodeScope.hpp"
#include "network/assembly/NetworkSecondPhaseScope.hpp"
#include "network/diag/NetworkShDevVtec.hpp"
#include "network/assembly/NetworkSignalIntegrity.hpp"
#include "network/emit/NetworkSolPrep.hpp"
#include "network/diag/NetworkStationBlock.hpp"
#include "network/qc/NetworkWithinEpochCulprits.hpp"
#include "network/carry/NetworkZwdCarry.hpp"
#include "network/emit/NetworkClockEmitRebase.hpp"
#include "network/emit/NetworkEmitRebase.hpp"
#include "network/slip/NetworkCommonBandScope.hpp"
#include "network/state/NetworkEntities.hpp"
#include "network/slip/NetworkGfArcJump.hpp"
#include "network/slip/NetworkSlipOracle.hpp"
#include "network/slip/NetworkSlipQcRunEnd.hpp"

namespace GnssProcessor {

// The reports in print order: {what it reports, its home file, the function}.
const std::array<NetworkRunEndRow, kNetworkRunEndReportCount> kNetworkRunEndReports = {{
    {"trace close: the estimability RUN row, then every trace stream closed",
     "app/NetworkRunArtifactsRunEnd.cpp", networkTraceCloseRunEnd},
    {"run timing: the epoch-loop wall, the D0 and D0b section timers",
     "app/NetworkRunTimingRunEnd.cpp", networkRunTimingRunEnd},
    {"the emit store: epochs emitted, skipped and withheld; heartbeats of a writer that never opened; the product contract round trip (B6)",
     "filter/NetworkEmitStore.cpp", networkEmitStoreRunEnd},
    {"stations in the solve and the addStation PD rejections",
     "app/NetworkSolveStationsRunEnd.cpp", networkSolveStationsRunEnd},
    {"the observability rank gate and the datum-direction ratio",
     "filter/NetworkRankGate.cpp", networkRankGateRunEnd},
    {"S-basis completion (SBC)",
     "filter/NetworkSbasisCompletion.cpp", networkSbasisCompletionRunEnd},
    {"the datum graph census",
     "filter/NetworkDatumGraph.cpp", networkDatumGraphRunEnd},
    {"within-epoch RAIM: the WE-CI screen and the T2 responses",
     "filter/NetworkRaimWithinEpoch.cpp", networkWithinEpochRaimRunEnd},
    {"the T2 culprit withhold on the DD carry: both ends of the hand-off (card WCF)",
     "filter/NetworkWithinEpochCulprits.cpp", networkWithinEpochCulpritWithholdRunEnd},
    {"the KP prior and the CB-ADM code-pair admission",
     "filter/NetworkBiasAdmission.cpp", networkBiasAdmissionRunEnd},
    {"phase-anchor hygiene: handover, cold fallback, the anchor re-reference, eq34",
     "filter/NetworkAnchorHygiene.cpp", networkAnchorHygieneRunEnd},
    {"RAIM: the carried-phase pools and the re-host, the withhold era, the R2 station factor, the R4 rate hygiene",
     "filter/NetworkRaim.cpp", networkRaimRunEnd},
    {"the gross line-of-sight screen and its cold-start bound",
     "filter/NetworkGrossLos.cpp", networkGrossLosRunEnd},
    {"the flicker and gap signal integrity",
     "filter/NetworkSignalIntegrity.cpp", networkSignalIntegrityRunEnd},
    {"the prior-residual census",
     "filter/NetworkPriorResidual.cpp", networkPriorResidualRunEnd},
    {"the redundancy and testability census",
     "filter/NetworkRedundancyCensus.cpp", networkRedundancyCensusRunEnd},
    {"Baarda MDB reliability",
     "filter/NetworkMdb.cpp", networkMdbRunEnd},
    {"the BlockFormal LOS-block census",
     "filter/NetworkBlockBaarda.cpp", networkBlockBaardaRunEnd},
    {"the station congruence screen",
     "filter/NetworkCongruence.cpp", networkCongruenceRunEnd},
    {"the prefit tier: census, fuse, seed births, topology, recorder, attribution",
     "filter/NetworkPrefitCensus.cpp", networkPrefitCensusRunEnd},
    {"the product-referenced admission of measurements",
     "filter/NetworkMeasurementAdmission.cpp", networkMeasurementAdmissionRunEnd},
    {"the invariant carry driver: the PD-safe DD fold, the card 4/M1 carry, the D5 gate and its map misses",
     "filter/NetworkInvariantCarryDriver.cpp", networkInvariantCarryDriverRunEnd},
    {"the producer ambiguity-resolution search",
     "filter/NetworkProducerAr.cpp", networkProducerArRunEnd},
    {"the producer AR search's suspect-corner mask (card ARL)",
     "filter/NetworkArSuspect.cpp", networkArSuspectRunEnd},
    {"the producer AR search's lattice observatory (card AGT)",
     "filter/NetworkArLattice.cpp", networkArLatticeRunEnd},
    {"the fixed-cycle fold (card FCF)",
     "filter/NetworkFixedCycles.cpp", networkFixedCyclesRunEnd},
    {"the carry-aware covariance mode",
     "filter/NetworkEstimator.cpp", networkCarryAwareCovarianceRunEnd},
    {"the SD pre-reduce EWL rows and integer fix",
     "filter/NetworkSdEwlAr.cpp", networkSdEwlRunEnd},
    {"slip QC: MW and LLI detections, re-level, wind-up, anchor-slip handovers, MW admission, IF-TD pairs and re-seed, datum stations, gaps, re-entry and the IF-TD reference gate",
     "slip/NetworkSlipQcRunEnd.cpp", networkSlipQcRunEnd},
    {"the phase-leveling carry: retirement families [A] to [D], the F3 local block, the response",
     "filter/NetworkLevelingCarryRunEnd.cpp", networkLevelingCarryRunEnd},
    {"the receiver-clock carry's jump gate (F3 block)",
     "filter/NetworkRxClockCarry.cpp", networkRxClockCarryRunEnd},
    {"warm resume: restores (frame, coupling regressors, dense border), then the resume and state-capture events (RTB-7, RS-2 to RS-9, the coupling capture)",
     "app/NetworkResumeRunEnd.cpp", networkResumeRunEnd},
    {"re-admission continuity",
     "app/NetworkReadmitRunEnd.cpp", networkReadmitRunEnd},
    {"the DD fractional-cycle trace's reference identity",
     "filter/NetworkDdFrac.cpp", networkDdFracReferenceRunEnd},
    {"the seam-scoped ZWD re-seed",
     "filter/NetworkZwdCarry.cpp", networkZwdSeamRunEnd},
    {"the extra-band code deweight",
     "filter/NetworkExtraBandScope.cpp", networkExtraBandScopeRunEnd},
    {"the second code channel",
     "filter/NetworkSecondCodeScope.cpp", networkSecondCodeScopeRunEnd},
    {"the second phase channel",
     "filter/NetworkSecondPhaseScope.cpp", networkSecondPhaseScopeRunEnd},
    {"the common-band scope verdicts",
     "slip/NetworkCommonBandScope.cpp", networkCommonBandScopeRunEnd},
    {"slip QC: the arc-scoped GF jump trigger (card ARL)",
     "slip/NetworkGfArcJump.cpp", networkGfArcJumpRunEnd},
    {"the LV-6 level-authority census",
     "filter/NetworkLevelAuthority.cpp", networkLevelAuthorityRunEnd},
    {"the sim-only oracle ambiguity pin",
     "filter/NetworkOraclePin.cpp", networkOraclePinRunEnd},
    {"the sim-only oracle slip response",
     "slip/NetworkSlipOracle.cpp", networkSlipOracleRunEnd},
    {"the eq34 chain datum: re-references, pin audit, seed round, slip-aware election",
     "filter/NetworkChainDatum.cpp", networkChainDatumRunEnd},
    {"the Iono-2 SH deviation carry",
     "filter/NetworkShDevVtec.cpp", networkShDevCarryRunEnd},
    {"the carried receiver code biases and their family",
     "filter/NetworkRxCodeBiasCarryRunEnd.cpp", networkRxCodeBiasCarryRunEnd},
    {"the S-basis datum arms and card GFC's code-datum arms",
     "filter/NetworkSbasis.cpp", networkSbasisRunEnd},
    {"the CB-excursion guard (Phase 2.1)",
     "filter/NetworkClockQc.cpp", networkCbExcursionRunEnd},
    {"frame diagnostics: the post-fit sigma0 and rows, the shared-normal spectrum trace",
     "filter/NetworkFrameDiag.cpp", networkFrameDiagRunEnd},
    {"the ZP station-block trace",
     "filter/NetworkStationBlock.cpp", networkStationBlockRunEnd},
    {"the ambiguity vehicle and the joint-block PD-fallback bar",
     "filter/NetworkAmbFloat.cpp", networkAmbVehicleRunEnd},
    {"the phase datum: the held S-basis (RF-2), the prioritized drop and heal, the anchored-union gate and its drop edge",
     "filter/NetworkPhaseDatum.cpp", networkPhaseDatumRunEnd},
    {"the ambiguity-state trace",
     "filter/NetworkAmbStateTrace.cpp", networkAmbStateTraceRunEnd},
    {"the emit-layer frame re-base",
     "products/NetworkEmitRebase.cpp", networkEmitRebaseRunEnd},
    {"the clock emission re-base and frame hold",
     "products/NetworkClockEmitRebase.cpp", networkClockEmitRebaseRunEnd},
    {"satellite health (HLT) and clock-pivot handovers with their S-transform",
     "filter/NetworkHandoverStransform.cpp", networkHandoverStransformRunEnd},
    {"the Q3 guarded temporal anchor of the clock level",
     "filter/NetworkClockAnchor.cpp", networkClockAnchorRunEnd},
    {"solution prep: the clock-runaway quarantine (RT0-5), the per-constellation sigma0 carve-out (RTA-10)",
     "filter/NetworkSolPrep.cpp", networkSolPrepRunEnd},
    {"the coupling carry (cards 1, 1d), reinit sources (1g) and gauge projection (1f)",
     "filter/NetworkCoupling.cpp", networkCouplingRunEnd},
    {"the CTU level monitor",
     "filter/NetworkClockLevel.cpp", networkClockLevelMonitorRunEnd},
    {"the native broadcast clock a priori (NBP)",
     "filter/NetworkBrdcWire.cpp", networkBrdcWireRunEnd},
    {"the phase-visible consistency (PVN)",
     "filter/NetworkPhaseVisibleDiag.cpp", networkPhaseVisibleRunEnd},
    {"the exact coupling time update (CTU, WSX, SCL, SCL2)",
     "filter/NetworkCouplingExact.cpp", networkCouplingExactRunEnd},
    {"the LS-VCE carry-variance monitor and factor",
     "filter/NetworkCarryVceRunEnd.cpp", networkCarryVceRunEnd},
    {"the marker to ARP roster reduction (UPO)",
     "app/ApplicationNetworkSetup.cpp", networkMarkerArpRunEnd},
    {"the parameter model as armed",
     "filter/NetworkParamRegistry.cpp", networkParamModelRunEnd},
    {"the receiver extra-band code delay (card L5B) and parameters as armed (NOM N1)",
     "filter/NetworkParamInventory.cpp", networkParamInventoryRunEnd},
    {"contributions as armed (NOM N2a)",
     "filter/NetworkContributions.cpp", networkContributionsRunEnd},
    {"the signal roster: signals as planned (NOM N3a), the roster and the arc ledger (SID phase 1)",
     "app/SignalRoster.cpp", networkSignalRosterRunEnd},
    {"the roster's site census (NOM N3b)",
     "app/SignalRosterSites.cpp", networkSignalRosterSitesRunEnd},
    {"the pivot-frame clock noise (PFQ) and the fold's dispositions of the carried border (PRM 1b)",
     "filter/NetworkCarryDisposition.cpp", networkCarryDispositionRunEnd},
    {"the carried references per site (PRM 2)",
     "filter/NetworkCarrySites.cpp", networkCarrySitesRunEnd},
    {"the declared responses of the carried instances (PRM 3a)",
     "filter/NetworkResponses.cpp", networkResponsesRunEnd},
    {"the phase-colour state (LGM)",
     "filter/NetworkPhaseColourCarry.cpp", networkPhaseColourRunEnd},
    {"the run manifest's complete record",
     "app/NetworkRunArtifactsRunEnd.cpp", networkRunManifestCompleteRunEnd},
    {"flag rates, the run summary and the shared-product line",
     "app/NetworkRunArtifactsRunEnd.cpp", networkRunSummaryRunEnd},
    {"the closed-loop integer-recovery harness",
     "app/NetworkClosedLoopRunEnd.cpp", networkClosedLoopRunEnd},
    {"entities as carried: the reach table of the entity tables (card NOM step N4)",
     "app/NetworkEntities.cpp", net::networkEntitiesRunEnd},
    {"the event harness (fault injection)",
     "filter/NetworkInject.cpp", networkInjectRunEnd},
    {"the canonical-pair admission (CPA)",
     "filter/NetworkCanonicalPairAdmission.cpp", networkCanonicalPairRunEnd},
    {"the all-channel census (SID phase 6b)",
     "filter/NetworkChannelAll.cpp", networkChannelAllRunEnd},
    {"the second constellation (GAL)",
     "filter/NetworkMultiGnss.cpp", networkMultiGnssRunEnd},
    {"the ledger-driven emission (SID phase 5a)",
     "filter/NetworkLedgerEmit.cpp", networkLedgerEmitRunEnd},
}};

} // namespace GnssProcessor

// ---- End of the table ----

namespace GnssProcessor {

int networkRunEndSections(const NetworkRunEndView& v,
                          std::initializer_list<NetworkRunEndReport> sections) {
    int verdict = 0;
    for (NetworkRunEndReport section : sections) {
        const int rc = section(v);
        if (rc != 0) verdict = rc;
    }
    return verdict;
}

namespace {

/// Holds the main-log messages one report writes, whole (time, thread, level, text), so
/// the walk can put the report's heading line ahead of them and then write each to the
/// logger's own sinks exactly as the logger would have.
class HeldRunEndLines final : public spdlog::sinks::sink {
public:
    void log(const spdlog::details::log_msg& msg) override { held.emplace_back(msg); }
    void flush() override {}
    void set_pattern(const std::string&) override {}
    void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

    std::vector<spdlog::details::log_msg_buffer> held;
};

/// Writes one message to the logger's sinks, as spdlog::logger::sink_it_ does (the level
/// test of each sink, then the logger's flush policy).
void writeToSinks(const spdlog::logger& lg, const spdlog::details::log_msg& msg) {
    for (const spdlog::sink_ptr& sink : lg.sinks())
        if (sink->should_log(msg.level)) sink->log(msg);
    if (msg.level != spdlog::level::off && msg.level >= lg.flush_level())
        for (const spdlog::sink_ptr& sink : lg.sinks()) sink->flush();
}

/// The held lines, after the heading when the logger prints at info.
void writeHeld(const spdlog::logger& lg, const char* name,
               const std::vector<spdlog::details::log_msg_buffer>& held) {
    if (held.empty()) return;
    if (lg.should_log(spdlog::level::info)) {
        const std::string text = std::string("Network[run-end] ") + name;
        spdlog::details::log_msg heading(lg.name(), spdlog::level::info, text);
        heading.time = held.front().time;
        heading.thread_id = held.front().thread_id;
        writeToSinks(lg, heading);
    }
    for (const spdlog::details::log_msg_buffer& msg : held) writeToSinks(lg, msg);
}

} // namespace

int networkRunEndWithHeading(spdlog::logger& lg, const char* name,
                             const std::function<int()>& report) {
    auto holder = std::make_shared<HeldRunEndLines>();
    std::vector<spdlog::sink_ptr> sinks{holder};
    lg.sinks().swap(sinks);                  // the report writes to the holder
    int rc = 0;
    try {
        rc = report();
    } catch (...) {
        lg.sinks().swap(sinks);              // the logger's own sinks again
        writeHeld(lg, name, holder->held);
        throw;
    }
    lg.sinks().swap(sinks);
    writeHeld(lg, name, holder->held);
    return rc;
}

int networkRunEndCallRow(const NetworkRunEndRow& row, const NetworkRunEndView& v) {
    return networkRunEndWithHeading(*v.log, row.name, [&] { return row.report(v); });
}

} // namespace GnssProcessor

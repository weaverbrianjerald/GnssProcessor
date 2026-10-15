/**
 * @file NetworkRunEnd.cpp
 * @brief The run-end table, kNetworkRunEndReports: every run-end report of the network
 *        producer in print order, with what it reports and its home file (card NOM step
 *        TD1b), one row per subsystem (card NOM2 step TD2), and the walk's two helpers.
 *        filter/NetworkRunEnd.hpp states how to add a row and the order constraints.
 *
 * The rows were written by scripts/nom_td_cut.py (TD1b) and regrouped by subsystem by
 * scripts/nom_td2_cut.py (TD2, proved by scripts/nom_td2_verify.py); since TD2 the table is
 * edited by hand under the rules of the header.  Each row's report is declared in the header
 * of its home file.
 */

#include "filter/NetworkRunEnd.hpp"

#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include <string>
#include <vector>

// ---- The table (TD1b's generated region, regrouped by TD2; edited by hand since) ----

#include "app/NetworkClosedLoopRunEnd.hpp"
#include "app/NetworkReadmitRunEnd.hpp"
#include "app/NetworkResumeRunEnd.hpp"
#include "app/NetworkRunArtifactsRunEnd.hpp"
#include "app/NetworkRunTimingRunEnd.hpp"
#include "app/NetworkSetupResult.hpp"
#include "app/NetworkSolveStationsRunEnd.hpp"
#include "app/SignalRoster.hpp"
#include "filter/NetworkAmbFloat.hpp"
#include "filter/NetworkAmbStateTrace.hpp"
#include "filter/NetworkAnchorHygiene.hpp"
#include "filter/NetworkArLattice.hpp"
#include "filter/NetworkArSuspect.hpp"
#include "filter/NetworkBiasAdmission.hpp"
#include "filter/NetworkBlockBaarda.hpp"
#include "filter/NetworkBrdcWire.hpp"
#include "filter/NetworkCanonicalPairAdmission.hpp"
#include "filter/NetworkCarryDisposition.hpp"
#include "filter/NetworkCarrySites.hpp"
#include "filter/NetworkCarryVceRunEnd.hpp"
#include "filter/NetworkChainDatum.hpp"
#include "filter/NetworkChannelAll.hpp"
#include "filter/NetworkClockAnchor.hpp"
#include "filter/NetworkClockLevel.hpp"
#include "filter/NetworkClockQc.hpp"
#include "filter/NetworkCongruence.hpp"
#include "filter/NetworkContributions.hpp"
#include "filter/NetworkCoupling.hpp"
#include "filter/NetworkCouplingExact.hpp"
#include "filter/NetworkDatumGraph.hpp"
#include "filter/NetworkDdFrac.hpp"
#include "filter/NetworkEmitStore.hpp"
#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkExtraBandScope.hpp"
#include "filter/NetworkFixedCycles.hpp"
#include "filter/NetworkFrameDiag.hpp"
#include "filter/NetworkGrossLos.hpp"
#include "filter/NetworkHandoverStransform.hpp"
#include "filter/NetworkInject.hpp"
#include "filter/NetworkInvariantCarryDriver.hpp"
#include "filter/NetworkLedgerEmit.hpp"
#include "filter/NetworkLevelAuthority.hpp"
#include "filter/NetworkLevelingCarryRunEnd.hpp"
#include "filter/NetworkMdb.hpp"
#include "filter/NetworkMeasurementAdmission.hpp"
#include "filter/NetworkMultiGnss.hpp"
#include "filter/NetworkOraclePin.hpp"
#include "filter/NetworkParamInventory.hpp"
#include "filter/NetworkParamRegistry.hpp"
#include "filter/NetworkPhaseColourCarry.hpp"
#include "filter/NetworkPhaseDatum.hpp"
#include "filter/NetworkPhaseVisibleDiag.hpp"
#include "filter/NetworkPrefitCensus.hpp"
#include "filter/NetworkPriorResidual.hpp"
#include "filter/NetworkProducerAr.hpp"
#include "filter/NetworkRaim.hpp"
#include "filter/NetworkRaimWithinEpoch.hpp"
#include "filter/NetworkRankGate.hpp"
#include "filter/NetworkRedundancyCensus.hpp"
#include "filter/NetworkResponses.hpp"
#include "filter/NetworkRxClockCarry.hpp"
#include "filter/NetworkRxCodeBiasCarryRunEnd.hpp"
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkSbasisCompletion.hpp"
#include "filter/NetworkSdEwlAr.hpp"
#include "filter/NetworkSecondCodeScope.hpp"
#include "filter/NetworkSecondPhaseScope.hpp"
#include "filter/NetworkShDevVtec.hpp"
#include "filter/NetworkSignalIntegrity.hpp"
#include "filter/NetworkSolPrep.hpp"
#include "filter/NetworkStationBlock.hpp"
#include "filter/NetworkWithinEpochCulprits.hpp"
#include "filter/NetworkZwdCarry.hpp"
#include "products/NetworkClockEmitRebase.hpp"
#include "products/NetworkEmitRebase.hpp"
#include "slip/NetworkCommonBandScope.hpp"
#include "slip/NetworkGfArcJump.hpp"
#include "slip/NetworkSlipOracle.hpp"
#include "slip/NetworkSlipQcRunEnd.hpp"

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
    {"warm resume: restores (frame, coupling regressors, dense border), then resume and snapshot (RTB-7, RS-2 to RS-9, the coupling capture)",
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

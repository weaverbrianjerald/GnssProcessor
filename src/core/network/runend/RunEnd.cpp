/**
 * @file RunEnd.cpp
 * @brief The run-end table, kNetworkRunEndReports: every run-end report of the network
 *        producer in print order, with what it reports and its home file (card NOM step
 *        TD1b), one row per subsystem (card NOM2 step TD2), and the walk's two helpers.
 *        network/runend/RunEnd.hpp states how to add a row and the order constraints.
 *
 * The rows were written by scripts/nom_td_cut.py (TD1b) and regrouped by subsystem by
 * scripts/nom_td2_cut.py (TD2, proved by scripts/nom_td2_verify.py); since TD2 the table is
 * edited by hand under the rules of the header.  Each row's report is declared in the header
 * of its home file.
 */

#include "network/runend/RunEnd.hpp"

#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include <string>
#include <vector>

// ---- The table (TD1b's generated region, regrouped by TD2; edited by hand since) ----

#include "network/runend/ClosedLoopRunEnd.hpp"
#include "network/runend/ReadmitRunEnd.hpp"
#include "network/runend/ResumeRunEnd.hpp"
#include "network/runend/RunArtifactsRunEnd.hpp"
#include "network/runend/RunTimingRunEnd.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/runend/SolveStationsRunEnd.hpp"
#include "network/assembly/SignalRoster.hpp"
#include "network/ar/AmbFloat.hpp"
#include "network/diag/AmbStateTrace.hpp"
#include "network/solve/AnchorHygiene.hpp"
#include "network/ar/ArLattice.hpp"
#include "network/ar/ArSuspect.hpp"
#include "network/assembly/BiasAdmission.hpp"
#include "network/qc/BlockBaarda.hpp"
#include "network/emit/BrdcWire.hpp"
#include "network/assembly/CanonicalPairAdmission.hpp"
#include "network/state/CarryDisposition.hpp"
#include "network/state/CarrySites.hpp"
#include "network/carry/CarryVceRunEnd.hpp"
#include "network/solve/ChainDatum.hpp"
#include "network/assembly/ChannelAll.hpp"
#include "network/solve/ClockAnchor.hpp"
#include "network/solve/ClockLevel.hpp"
#include "network/qc/ClockQc.hpp"
#include "network/qc/Congruence.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/carry/Coupling.hpp"
#include "network/carry/CouplingExact.hpp"
#include "network/solve/DatumGraph.hpp"
#include "network/diag/DdFrac.hpp"
#include "network/emit/EmitStore.hpp"
#include "network/solve/Estimator.hpp"
#include "network/assembly/ExtraBandScope.hpp"
#include "network/ar/FixedCycles.hpp"
#include "network/diag/FrameDiag.hpp"
#include "network/qc/GrossLos.hpp"
#include "network/solve/HandoverStransform.hpp"
#include "network/carry/Inject.hpp"
#include "network/carry/InvariantCarryDriver.hpp"
#include "network/emit/LedgerEmit.hpp"
#include "network/carry/LevelAuthority.hpp"
#include "network/carry/LevelingCarryRunEnd.hpp"
#include "network/qc/Mdb.hpp"
#include "network/assembly/MeasurementAdmission.hpp"
#include "network/assembly/MultiGnss.hpp"
#include "network/slip/OraclePin.hpp"
#include "network/assembly/ParamInventory.hpp"
#include "network/assembly/ParamRegistry.hpp"
#include "network/carry/PhaseColourCarry.hpp"
#include "network/solve/PhaseDatum.hpp"
#include "network/diag/PhaseVisibleDiag.hpp"
#include "network/qc/PrefitCensus.hpp"
#include "network/qc/PriorResidual.hpp"
#include "network/ar/ProducerAr.hpp"
#include "network/qc/Raim.hpp"
#include "network/qc/RaimWithinEpoch.hpp"
#include "network/solve/RankGate.hpp"
#include "network/qc/RedundancyCensus.hpp"
#include "network/assembly/Responses.hpp"
#include "network/carry/RxClockCarry.hpp"
#include "network/carry/RxCodeBiasCarryRunEnd.hpp"
#include "network/solve/Sbasis.hpp"
#include "network/solve/SbasisCompletion.hpp"
#include "network/ar/SdEwlAr.hpp"
#include "network/assembly/SecondCodeScope.hpp"
#include "network/assembly/SecondPhaseScope.hpp"
#include "network/diag/ShDevVtec.hpp"
#include "network/assembly/SignalIntegrity.hpp"
#include "network/emit/SolPrep.hpp"
#include "network/diag/StationBlock.hpp"
#include "network/qc/WithinEpochCulprits.hpp"
#include "network/carry/ZwdCarry.hpp"
#include "network/emit/ClockEmitRebase.hpp"
#include "network/emit/EmitRebase.hpp"
#include "network/slip/CommonBandScope.hpp"
#include "network/state/Entities.hpp"
#include "network/slip/GfArcJump.hpp"
#include "network/slip/SlipOracle.hpp"
#include "network/slip/SlipQcRunEnd.hpp"

namespace GnssProcessor {

// The reports in print order: {what it reports, its home file, the function}.
const std::array<NetworkRunEndRow, kNetworkRunEndReportCount> kNetworkRunEndReports = {{
    {"trace close: the estimability RUN row, then every trace stream closed",
     "app/RunArtifactsRunEnd.cpp", networkTraceCloseRunEnd},
    {"run timing: the epoch-loop wall, the D0 and D0b section timers",
     "app/RunTimingRunEnd.cpp", networkRunTimingRunEnd},
    {"the emit store: epochs emitted, skipped and withheld; heartbeats of a writer that never opened; the product contract round trip (B6)",
     "filter/EmitStore.cpp", networkEmitStoreRunEnd},
    {"stations in the solve and the addStation PD rejections",
     "app/SolveStationsRunEnd.cpp", networkSolveStationsRunEnd},
    {"the observability rank gate and the datum-direction ratio",
     "filter/RankGate.cpp", networkRankGateRunEnd},
    {"S-basis completion (SBC)",
     "filter/SbasisCompletion.cpp", networkSbasisCompletionRunEnd},
    {"the datum graph census",
     "filter/DatumGraph.cpp", networkDatumGraphRunEnd},
    {"within-epoch RAIM: the WE-CI screen and the T2 responses",
     "filter/RaimWithinEpoch.cpp", networkWithinEpochRaimRunEnd},
    {"the T2 culprit withhold on the DD carry: both ends of the hand-off (card WCF)",
     "filter/WithinEpochCulprits.cpp", networkWithinEpochCulpritWithholdRunEnd},
    {"the KP prior and the CB-ADM code-pair admission",
     "filter/BiasAdmission.cpp", networkBiasAdmissionRunEnd},
    {"phase-anchor hygiene: handover, cold fallback, the anchor re-reference, eq34",
     "filter/AnchorHygiene.cpp", networkAnchorHygieneRunEnd},
    {"RAIM: the carried-phase pools and the re-host, the withhold era, the R2 station factor, the R4 rate hygiene",
     "filter/Raim.cpp", networkRaimRunEnd},
    {"the gross line-of-sight screen and its cold-start bound",
     "filter/GrossLos.cpp", networkGrossLosRunEnd},
    {"the flicker and gap signal integrity",
     "filter/SignalIntegrity.cpp", networkSignalIntegrityRunEnd},
    {"the prior-residual census",
     "filter/PriorResidual.cpp", networkPriorResidualRunEnd},
    {"the redundancy and testability census",
     "filter/RedundancyCensus.cpp", networkRedundancyCensusRunEnd},
    {"Baarda MDB reliability",
     "filter/Mdb.cpp", networkMdbRunEnd},
    {"the BlockFormal LOS-block census",
     "filter/BlockBaarda.cpp", networkBlockBaardaRunEnd},
    {"the station congruence screen",
     "filter/Congruence.cpp", networkCongruenceRunEnd},
    {"the prefit tier: census, fuse, seed births, topology, recorder, attribution",
     "filter/PrefitCensus.cpp", networkPrefitCensusRunEnd},
    {"the product-referenced admission of measurements",
     "filter/MeasurementAdmission.cpp", networkMeasurementAdmissionRunEnd},
    {"the invariant carry driver: the PD-safe DD fold, the card 4/M1 carry, the D5 gate and its map misses",
     "filter/InvariantCarryDriver.cpp", networkInvariantCarryDriverRunEnd},
    {"the producer ambiguity-resolution search",
     "filter/ProducerAr.cpp", networkProducerArRunEnd},
    {"the producer AR search's suspect-corner mask (card ARL)",
     "filter/ArSuspect.cpp", networkArSuspectRunEnd},
    {"the producer AR search's lattice observatory (card AGT)",
     "filter/ArLattice.cpp", networkArLatticeRunEnd},
    {"the fixed-cycle fold (card FCF)",
     "filter/FixedCycles.cpp", networkFixedCyclesRunEnd},
    {"the carry-aware covariance mode",
     "filter/Estimator.cpp", networkCarryAwareCovarianceRunEnd},
    {"the SD pre-reduce EWL rows and integer fix",
     "filter/SdEwlAr.cpp", networkSdEwlRunEnd},
    {"slip QC: MW and LLI detections, re-level, wind-up, anchor-slip handovers, MW admission, IF-TD pairs and re-seed, datum stations, gaps, re-entry and the IF-TD reference gate",
     "slip/SlipQcRunEnd.cpp", networkSlipQcRunEnd},
    {"the phase-leveling carry: retirement families [A] to [D], the F3 local block, the response",
     "filter/LevelingCarryRunEnd.cpp", networkLevelingCarryRunEnd},
    {"the receiver-clock carry's jump gate (F3 block)",
     "filter/RxClockCarry.cpp", networkRxClockCarryRunEnd},
    {"warm resume: restores (frame, coupling regressors, dense border), then the resume and state-capture events (RTB-7, RS-2 to RS-9, the coupling capture)",
     "app/ResumeRunEnd.cpp", networkResumeRunEnd},
    {"re-admission continuity",
     "app/ReadmitRunEnd.cpp", networkReadmitRunEnd},
    {"the DD fractional-cycle trace's reference identity",
     "filter/DdFrac.cpp", networkDdFracReferenceRunEnd},
    {"the seam-scoped ZWD re-seed",
     "filter/ZwdCarry.cpp", networkZwdSeamRunEnd},
    {"the extra-band code deweight",
     "filter/ExtraBandScope.cpp", networkExtraBandScopeRunEnd},
    {"the second code channel",
     "filter/SecondCodeScope.cpp", networkSecondCodeScopeRunEnd},
    {"the second phase channel",
     "filter/SecondPhaseScope.cpp", networkSecondPhaseScopeRunEnd},
    {"the common-band scope verdicts",
     "slip/CommonBandScope.cpp", networkCommonBandScopeRunEnd},
    {"slip QC: the arc-scoped GF jump trigger (card ARL)",
     "slip/GfArcJump.cpp", networkGfArcJumpRunEnd},
    {"the LV-6 level-authority census",
     "filter/LevelAuthority.cpp", networkLevelAuthorityRunEnd},
    {"the sim-only oracle ambiguity pin",
     "filter/OraclePin.cpp", networkOraclePinRunEnd},
    {"the sim-only oracle slip response",
     "slip/SlipOracle.cpp", networkSlipOracleRunEnd},
    {"the eq34 chain datum: re-references, pin audit, seed round, slip-aware election",
     "filter/ChainDatum.cpp", networkChainDatumRunEnd},
    {"the Iono-2 SH deviation carry",
     "filter/ShDevVtec.cpp", networkShDevCarryRunEnd},
    {"the carried receiver code biases and their family",
     "filter/RxCodeBiasCarryRunEnd.cpp", networkRxCodeBiasCarryRunEnd},
    {"the S-basis datum arms and card GFC's code-datum arms",
     "filter/Sbasis.cpp", networkSbasisRunEnd},
    {"the CB-excursion guard (Phase 2.1)",
     "filter/ClockQc.cpp", networkCbExcursionRunEnd},
    {"frame diagnostics: the post-fit sigma0 and rows, the shared-normal spectrum trace",
     "filter/FrameDiag.cpp", networkFrameDiagRunEnd},
    {"the ZP station-block trace",
     "filter/StationBlock.cpp", networkStationBlockRunEnd},
    {"the ambiguity vehicle and the joint-block PD-fallback bar",
     "filter/AmbFloat.cpp", networkAmbVehicleRunEnd},
    {"the phase datum: the held S-basis (RF-2), the prioritized drop and heal, the anchored-union gate and its drop edge",
     "filter/PhaseDatum.cpp", networkPhaseDatumRunEnd},
    {"the ambiguity-state trace",
     "filter/AmbStateTrace.cpp", networkAmbStateTraceRunEnd},
    {"the emit-layer frame re-base",
     "products/EmitRebase.cpp", networkEmitRebaseRunEnd},
    {"the clock emission re-base and frame hold",
     "products/ClockEmitRebase.cpp", networkClockEmitRebaseRunEnd},
    {"satellite health (HLT) and clock-pivot handovers with their S-transform",
     "filter/HandoverStransform.cpp", networkHandoverStransformRunEnd},
    {"the Q3 guarded temporal anchor of the clock level",
     "filter/ClockAnchor.cpp", networkClockAnchorRunEnd},
    {"solution prep: the clock-runaway quarantine (RT0-5), the per-constellation sigma0 carve-out (RTA-10)",
     "filter/SolPrep.cpp", networkSolPrepRunEnd},
    {"the coupling carry (cards 1, 1d), reinit sources (1g) and gauge projection (1f)",
     "filter/Coupling.cpp", networkCouplingRunEnd},
    {"the CTU level monitor",
     "filter/ClockLevel.cpp", networkClockLevelMonitorRunEnd},
    {"the native broadcast clock a priori (NBP)",
     "filter/BrdcWire.cpp", networkBrdcWireRunEnd},
    {"the phase-visible consistency (PVN)",
     "filter/PhaseVisibleDiag.cpp", networkPhaseVisibleRunEnd},
    {"the exact coupling time update (CTU, WSX, SCL, SCL2)",
     "filter/CouplingExact.cpp", networkCouplingExactRunEnd},
    {"the LS-VCE carry-variance monitor and factor",
     "filter/CarryVceRunEnd.cpp", networkCarryVceRunEnd},
    {"the marker to ARP roster reduction (UPO)",
     "app/ApplicationNetworkSetup.cpp", networkMarkerArpRunEnd},
    {"the parameter model as armed",
     "filter/ParamRegistry.cpp", networkParamModelRunEnd},
    {"the receiver extra-band code delay (card L5B) and parameters as armed (NOM N1)",
     "filter/ParamInventory.cpp", networkParamInventoryRunEnd},
    {"contributions as armed (NOM N2a)",
     "filter/Contributions.cpp", networkContributionsRunEnd},
    {"the signal roster: signals as planned (NOM N3a), the roster and the arc ledger (SID phase 1)",
     "app/SignalRoster.cpp", networkSignalRosterRunEnd},
    {"the roster's site census (NOM N3b)",
     "app/SignalRosterSites.cpp", networkSignalRosterSitesRunEnd},
    {"the pivot-frame clock noise (PFQ) and the fold's dispositions of the carried border (PRM 1b)",
     "filter/CarryDisposition.cpp", networkCarryDispositionRunEnd},
    {"the carried references per site (PRM 2)",
     "filter/CarrySites.cpp", networkCarrySitesRunEnd},
    {"the declared responses of the carried instances (PRM 3a)",
     "filter/Responses.cpp", networkResponsesRunEnd},
    {"the phase-colour state (LGM)",
     "filter/PhaseColourCarry.cpp", networkPhaseColourRunEnd},
    {"the run manifest's complete record",
     "app/RunArtifactsRunEnd.cpp", networkRunManifestCompleteRunEnd},
    {"flag rates, the run summary and the shared-product line",
     "app/RunArtifactsRunEnd.cpp", networkRunSummaryRunEnd},
    {"the closed-loop integer-recovery harness",
     "app/ClosedLoopRunEnd.cpp", networkClosedLoopRunEnd},
    {"entities as carried: the reach table of the entity tables (card NOM step N4)",
     "app/Entities.cpp", net::networkEntitiesRunEnd},
    {"the event harness (fault injection)",
     "filter/Inject.cpp", networkInjectRunEnd},
    {"the canonical-pair admission (CPA)",
     "filter/CanonicalPairAdmission.cpp", networkCanonicalPairRunEnd},
    {"the all-channel census (SID phase 6b)",
     "filter/ChannelAll.cpp", networkChannelAllRunEnd},
    {"the second constellation (GAL)",
     "filter/MultiGnss.cpp", networkMultiGnssRunEnd},
    {"the ledger-driven emission (SID phase 5a)",
     "filter/LedgerEmit.cpp", networkLedgerEmitRunEnd},
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

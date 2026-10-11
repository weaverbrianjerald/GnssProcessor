/**
 * @file NetworkRunEnd.cpp
 * @brief The run-end table, kNetworkRunEndReports: every run-end report of the network
 *        producer in print order, with what it reports and its home file (card NOM step
 *        TD1b).  filter/NetworkRunEnd.hpp states how to add a row and the order constraints.
 *
 * The rows below are written by scripts/nom_td_cut.py together with the reports themselves,
 * so the table and the cut cannot disagree.  Each row's report is declared in the header of
 * its home file.
 */

#include "filter/NetworkRunEnd.hpp"

// ---- Written by scripts/nom_td_cut.py (card NOM step TD1b); change the generator, not this region ----

#include "app/NetworkClosedLoopRunEnd.hpp"
#include "app/NetworkReadmitRunEnd.hpp"
#include "app/NetworkResumeRunEnd.hpp"
#include "app/NetworkRunArtifactsRunEnd.hpp"
#include "app/NetworkRunTimingRunEnd.hpp"
#include "app/NetworkSetupResult.hpp"
#include "app/NetworkSolveStationsRunEnd.hpp"
#include "app/NetworkStationIonoRunEnd.hpp"
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
#include "filter/NetworkFastClock.hpp"
#include "filter/NetworkFrameDiag.hpp"
#include "filter/NetworkGrossLos.hpp"
#include "filter/NetworkHandoverStransform.hpp"
#include "filter/NetworkInject.hpp"
#include "filter/NetworkInvariantCarryDriver.hpp"
#include "filter/NetworkIonoDwRelease.hpp"
#include "filter/NetworkLedgerEmit.hpp"
#include "filter/NetworkLevelAuthority.hpp"
#include "filter/NetworkLevelingCarryRunEnd.hpp"
#include "filter/NetworkMdb.hpp"
#include "filter/NetworkMeasurementAdmission.hpp"
#include "filter/NetworkMultiGnss.hpp"
#include "filter/NetworkMwWlLayer.hpp"
#include "filter/NetworkOraclePin.hpp"
#include "filter/NetworkParamInventory.hpp"
#include "filter/NetworkParamRegistry.hpp"
#include "filter/NetworkPhaseDatum.hpp"
#include "filter/NetworkPhaseVisibleDiag.hpp"
#include "filter/NetworkPrefitCensus.hpp"
#include "filter/NetworkPriorResidual.hpp"
#include "filter/NetworkProducerAr.hpp"
#include "filter/NetworkRaim.hpp"
#include "filter/NetworkRedundancyCensus.hpp"
#include "filter/NetworkResponses.hpp"
#include "filter/NetworkRxCodeBiasCarryRunEnd.hpp"
#include "filter/NetworkSbasis.hpp"
#include "filter/NetworkSdEwlAr.hpp"
#include "filter/NetworkSecondCodeScope.hpp"
#include "filter/NetworkSecondPhaseScope.hpp"
#include "filter/NetworkShDevVtec.hpp"
#include "filter/NetworkShHealth.hpp"
#include "filter/NetworkSignalIntegrity.hpp"
#include "filter/NetworkSolPrep.hpp"
#include "filter/NetworkStationBlock.hpp"
#include "products/NetworkClockEmitRebase.hpp"
#include "products/NetworkEmitRebase.hpp"
#include "products/NetworkPbEmitFilter.hpp"
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
    {"producer complete: epochs emitted, skipped and withheld",
     "filter/NetworkEmitStore.cpp", networkEmitOutcomeRunEnd},
    {"stations in the solve and the addStation PD rejections",
     "app/NetworkSolveStationsRunEnd.cpp", networkSolveStationsRunEnd},
    {"heartbeats of a deferred product writer that never opened",
     "filter/NetworkEmitStore.cpp", networkDeferredHeartbeatsRunEnd},
    {"the observability rank gate and the datum-direction ratio",
     "filter/NetworkEstimator.cpp", networkRankGateRunEnd},
    {"S-basis completion (SBC)",
     "filter/NetworkSbasis.cpp", networkSbasisCompletionRunEnd},
    {"the datum graph census",
     "filter/NetworkDatumGraph.cpp", networkDatumGraphRunEnd},
    {"within-epoch RAIM: the WE-CI screen and the T2 responses",
     "filter/NetworkRaim.cpp", networkWithinEpochRaimRunEnd},
    {"the KP prior and the CB-ADM code-pair admission",
     "filter/NetworkBiasAdmission.cpp", networkBiasAdmissionRunEnd},
    {"the adaptive ionosphere process noise",
     "app/NetworkStationIonoRunEnd.cpp", networkAdaptiveIonoQRunEnd},
    {"phase-anchor hygiene: handover, cold fallback, PHASE_ANCHOR re-reference, eq34",
     "filter/NetworkAnchorHygiene.cpp", networkAnchorHygieneRunEnd},
    {"the carried-phase RAIM pools: admissions and the re-host",
     "filter/NetworkRaim.cpp", networkRaimPoolRunEnd},
    {"the gross line-of-sight screen",
     "filter/NetworkGrossLos.cpp", networkGrossLosRunEnd},
    {"the gross line-of-sight screen's cold-start bound",
     "filter/NetworkGrossLos.cpp", networkGrossLosColdBoundRunEnd},
    {"the ionosphere-deweight release hygiene",
     "filter/NetworkIonoDwRelease.cpp", networkDwReleaseHygieneRunEnd},
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
    {"the product-referenced measurement admission",
     "filter/NetworkMeasurementAdmission.cpp", networkMeasurementAdmissionRunEnd},
    {"the PD-safe invariant-DD fold",
     "filter/NetworkInvariantCarryDriver.cpp", networkCarryDdFoldRunEnd},
    {"the producer ambiguity-resolution search",
     "filter/NetworkProducerAr.cpp", networkProducerArRunEnd},
    {"the producer AR search's suspect-corner mask (card ARL)",
     "filter/NetworkArSuspect.cpp", networkArSuspectRunEnd},
    {"the producer AR search's lattice observatory (card AGT)",
     "filter/NetworkArLattice.cpp", networkArLatticeRunEnd},
    {"the carry-aware covariance mode",
     "filter/NetworkEstimator.cpp", networkCarryAwareCovarianceRunEnd},
    {"the SD pre-reduce EWL rows and integer fix",
     "filter/NetworkSdEwlAr.cpp", networkSdEwlRunEnd},
    {"slip QC: MW and LLI detections, the re-level response, the statistical retap",
     "slip/NetworkSlipQcRunEnd.cpp", networkSlipMwRelevelRunEnd},
    {"leveling-carry retirement census [B] and [A]; F1, F3, the clock gate, young arcs",
     "filter/NetworkLevelingCarryRunEnd.cpp", networkLevelingRetirementRunEnd},
    {"warm-resume restores: frame, coupling regressors, dense border",
     "app/NetworkResumeRunEnd.cpp", networkResumeRestoreRunEnd},
    {"re-admission continuity",
     "app/NetworkReadmitRunEnd.cpp", networkReadmitRunEnd},
    {"the DD fractional-cycle trace's reference identity",
     "filter/NetworkDdFrac.cpp", networkDdFracReferenceRunEnd},
    {"the seam-scoped ZWD re-seed",
     "app/NetworkResumeRunEnd.cpp", networkZwdSeamRunEnd},
    {"leveling-carry retirement census [C], [D] and the response",
     "filter/NetworkLevelingCarryRunEnd.cpp", networkLevelingResponseRunEnd},
    {"slip QC: wind-up census and restart, anchor-slip handover, MW admission and scope",
     "slip/NetworkSlipQcRunEnd.cpp", networkSlipWindupMwRunEnd},
    {"the extra-band code deweight",
     "filter/NetworkExtraBandScope.cpp", networkExtraBandScopeRunEnd},
    {"the second code channel",
     "filter/NetworkSecondCodeScope.cpp", networkSecondCodeScopeRunEnd},
    {"the second phase channel",
     "filter/NetworkSecondPhaseScope.cpp", networkSecondPhaseScopeRunEnd},
    {"the common-band scope verdicts",
     "slip/NetworkCommonBandScope.cpp", networkCommonBandScopeRunEnd},
    {"slip QC: IF-TD pairs, band-scoped responses, the lane observatory, retap corroboration",
     "slip/NetworkSlipQcRunEnd.cpp", networkSlipBandResponsesRunEnd},
    {"slip QC: the IF-TD post-flag re-seed (card ARL)",
     "slip/NetworkSlipQcRunEnd.cpp", networkIfTdReseedRunEnd},
    {"slip QC: the arc-scoped GF jump trigger (card ARL)",
     "slip/NetworkGfArcJump.cpp", networkGfArcJumpRunEnd},
    {"the slant-ionosphere carry",
     "app/NetworkStationIonoRunEnd.cpp", networkIonoCarryRunEnd},
    {"the withhold era, the R2 station factor, the R4 rate hygiene",
     "filter/NetworkRaim.cpp", networkRaimWithholdRunEnd},
    {"the LV-6 level-authority census",
     "filter/NetworkLevelAuthority.cpp", networkLevelAuthorityRunEnd},
    {"slip QC: datum-station detection and LLI corroboration",
     "slip/NetworkSlipQcRunEnd.cpp", networkSlipDatumStationRunEnd},
    {"the sim-only oracle ambiguity pin",
     "filter/NetworkOraclePin.cpp", networkOraclePinRunEnd},
    {"the sim-only oracle slip response",
     "slip/NetworkSlipOracle.cpp", networkSlipOracleRunEnd},
    {"slip QC: the gap instrument, re-entry, GRS, the IF-TD reference gate",
     "slip/NetworkSlipQcRunEnd.cpp", networkSlipGapRefGateRunEnd},
    {"the eq34 chain datum: re-references, pin audit, seed round, slip-aware election",
     "filter/NetworkChainDatum.cpp", networkChainDatumRunEnd},
    {"the card 4/M1 invariant carry",
     "filter/NetworkInvariantCarryDriver.cpp", networkInvariantCarryRunEnd},
    {"the FC-1 split-cadence fast clock",
     "filter/NetworkFastClock.cpp", networkFastClockRunEnd},
    {"resume and snapshot: RTB-7, RS-2 to RS-9, the coupling capture",
     "app/NetworkResumeRunEnd.cpp", networkResumeSnapshotRunEnd},
    {"the Iono-2 SH deviation carry",
     "filter/NetworkShDevVtec.cpp", networkShDevCarryRunEnd},
    {"the receiver code-bias carry family",
     "filter/NetworkRxCodeBiasCarryRunEnd.cpp", networkRxCodeBiasCarryRunEnd},
    {"the S-basis datum arms",
     "filter/NetworkSbasis.cpp", networkSbasisDatumRunEnd},
    {"the MW/WL lanes: WL-1, EWL, the WL inject, UWL, WL-3",
     "filter/NetworkMwWlLayer.cpp", networkLanesRunEnd},
    {"the D5 fine-slip innovation gate and its variants",
     "filter/NetworkInvariantCarryDriver.cpp", networkD5GateRunEnd},
    {"the station-adaptive SH-VTEC constraint deweight",
     "filter/NetworkIonoDwRelease.cpp", networkIonoDeweightRunEnd},
    {"the CB-excursion guard (Phase 2.1)",
     "filter/NetworkClockQc.cpp", networkCbExcursionRunEnd},
    {"the SH health gate (Phase 2.2)",
     "filter/NetworkShHealth.cpp", networkShHealthRunEnd},
    {"the post-fit sigma0 and the post-fit rows",
     "filter/NetworkFrameDiag.cpp", networkPostfitRunEnd},
    {"the producer's state-manager re-inflations",
     "filter/NetworkIonoDwRelease.cpp", networkReinflateRunEnd},
    {"the ZP station-block trace",
     "filter/NetworkStationBlock.cpp", networkStationBlockRunEnd},
    {"the ambiguity vehicle and the joint-block PD-fallback bar",
     "filter/NetworkAmbFloat.cpp", networkAmbVehicleRunEnd},
    {"the held S-basis (RF-2)",
     "filter/NetworkPhaseDatum.cpp", networkHeldSbasisRunEnd},
    {"the ambiguity-state trace",
     "filter/NetworkAmbStateTrace.cpp", networkAmbStateTraceRunEnd},
    {"the shared-normal spectrum trace",
     "filter/NetworkFrameDiag.cpp", networkShSpectrumRunEnd},
    {"D5 flags missing from the carry map",
     "filter/NetworkInvariantCarryDriver.cpp", networkD5MapMissRunEnd},
    {"the emit-layer frame re-base",
     "products/NetworkEmitRebase.cpp", networkEmitRebaseRunEnd},
    {"the phase-bias emission-continuity filter",
     "products/NetworkPbEmitFilter.cpp", networkPbEmitFilterRunEnd},
    {"the clock emission re-base and frame hold",
     "products/NetworkClockEmitRebase.cpp", networkClockEmitRebaseRunEnd},
    {"the clock level re-base (CLV)",
     "filter/NetworkClockLevel.cpp", networkClockLevelRunEnd},
    {"broadcast satellite health (HLT)",
     "filter/NetworkHandoverStransform.cpp", networkSatHealthRunEnd},
    {"the Q3 guarded temporal clock anchor",
     "filter/NetworkClockAnchor.cpp", networkClockAnchorRunEnd},
    {"clock-pivot handovers and their S-transform",
     "filter/NetworkHandoverStransform.cpp", networkPivotHandoverRunEnd},
    {"the clock-runaway quarantine (RT0-5)",
     "filter/NetworkSolPrep.cpp", networkClockRunawayRunEnd},
    {"the prioritized-datum drop-and-heal",
     "filter/NetworkPhaseDatum.cpp", networkDropHealRunEnd},
    {"the coupling carry (cards 1, 1b, 1d)",
     "filter/NetworkCoupling.cpp", networkCouplingRunEnd},
    {"the CTU level monitor",
     "filter/NetworkClockLevel.cpp", networkClockLevelMonitorRunEnd},
    {"the native broadcast clock a priori (NBP)",
     "filter/NetworkBrdcWire.cpp", networkBrdcWireRunEnd},
    {"the phase-visible consistency (PVN)",
     "filter/NetworkPhaseVisibleDiag.cpp", networkPhaseVisibleRunEnd},
    {"the pivot-frame clock noise (PFQ)",
     "filter/NetworkCarryDisposition.cpp", networkPivotFrameQRunEnd},
    {"the exact coupling time update (CTU, WSX, SCL, SCL2)",
     "filter/NetworkCouplingExact.cpp", networkCouplingExactRunEnd},
    {"the gauge-free shared carry (C-GF)",
     "filter/NetworkClockAnchor.cpp", networkGaugeFreeCarryRunEnd},
    {"coupling reinit sources (1g) and gauge projection (1f)",
     "filter/NetworkCoupling.cpp", networkCouplingProjectionRunEnd},
    {"the LS-VCE carry-variance monitor and factor",
     "filter/NetworkCarryVceRunEnd.cpp", networkCarryVceRunEnd},
    {"the anchored-union coverage gate and its drop edge",
     "filter/NetworkPhaseDatum.cpp", networkPhaseDatumGatesRunEnd},
    {"the marker to ARP roster reduction (UPO)",
     "app/ApplicationNetworkSetup.cpp", networkMarkerArpRunEnd},
    {"the per-constellation sigma0 carve-out (RTA-10)",
     "filter/NetworkSolPrep.cpp", networkPerConstellationRunEnd},
    {"the parameter model as armed",
     "filter/NetworkParamRegistry.cpp", networkParamModelRunEnd},
    {"parameters as armed (NOM N1)",
     "filter/NetworkParamInventory.cpp", networkParamInventoryRunEnd},
    {"contributions as armed (NOM N2a)",
     "filter/NetworkContributions.cpp", networkContributionsRunEnd},
    {"signals as planned (NOM N3a)",
     "app/SignalRoster.cpp", networkSignalRosterPlanRunEnd},
    {"the fold's dispositions of the carried border (PRM 1b)",
     "filter/NetworkCarryDisposition.cpp", networkCarryDispositionRunEnd},
    {"the carried references per site (PRM 2)",
     "filter/NetworkCarrySites.cpp", networkCarrySitesRunEnd},
    {"the declared responses of the carried instances (PRM 3a)",
     "filter/NetworkResponses.cpp", networkResponsesRunEnd},
    {"the phase-colour state (LGM)",
     "filter/NetworkLevelingCarryRunEnd.cpp", networkPhaseColourRunEnd},
    {"the run manifest's complete record",
     "app/NetworkRunArtifactsRunEnd.cpp", networkRunManifestCompleteRunEnd},
    {"the product contract round trip (B6)",
     "filter/NetworkEmitStore.cpp", networkProductContractRunEnd},
    {"flag rates, the run summary and the shared-product line",
     "app/NetworkRunArtifactsRunEnd.cpp", networkRunSummaryRunEnd},
    {"the closed-loop integer-recovery harness",
     "app/NetworkClosedLoopRunEnd.cpp", networkClosedLoopRunEnd},
    {"the event harness (fault injection)",
     "filter/NetworkInject.cpp", networkInjectRunEnd},
    {"the canonical-pair admission (CPA)",
     "filter/NetworkCanonicalPairAdmission.cpp", networkCanonicalPairRunEnd},
    {"the signal roster and the arc ledger (SID phase 1)",
     "app/SignalRoster.cpp", networkSignalRosterRunEnd},
    {"the all-channel census (SID phase 6b)",
     "filter/NetworkChannelAll.cpp", networkChannelAllRunEnd},
    {"the second constellation (GAL)",
     "filter/NetworkMultiGnss.cpp", networkMultiGnssRunEnd},
    {"the ledger-driven emission (SID phase 5a)",
     "filter/NetworkLedgerEmit.cpp", networkLedgerEmitRunEnd},
}};

} // namespace GnssProcessor

// ---- End of the generated region ----

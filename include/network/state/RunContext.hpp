#pragma once
/**
 * @file RunContext.hpp
 * @brief @internal Tier-1 run state of Application::runNetwork: the const
 *        config-knob snapshot (NetworkKnobs), the cross-epoch carry state
 *        (NetworkCarryState), and the service counters
 *        (NetworkServiceCounters), bundled as NetworkRunContext.
 *
 * The members keep the names and types of the run-loop locals they replaced
 * (decomposition INC-4; see docs/network_decomposition_plan.md).  The epoch function
 * (ApplicationNetworkEpoch.cpp) and the preamble of each seam in its epoch form bind
 * `auto& X = ctx.carry.X;` / `const T X = ctx.knobs.X;` aliases under those names (the
 * run loop in ApplicationNetwork.cpp did until the run took ownership), so the use sites read
 * as before.
 *
 * Three-place rule: a NetworkCarryState field marked snapshot-member must be
 * added to the fingerprint, the restore and the snapshot write, in
 * src/core/network/state/SnapshotFingerprint.cpp, WarmResume.cpp and
 * StateSnapshotWrite.cpp.  A new cross-epoch field goes into
 * NetworkCarryState under that rule unless it is explicitly exempted; the
 * per-field "not snapshotted" notes below are the exemption record.
 * Kept out of this header on purpose: the engine objects (NetworkEstimator,
 * NetworkInvariantCarryDriver, NetworkCarryVce, NetworkPriorResidual,
 * NetworkEmitRebase), the D0/D0b section-timer instrumentation, and the closed-loop
 * consumer metrics.  The run owns them (network/driver/Run.hpp; they
 * were locals of the run TU before) in the groups of network/driver/RunGroups.hpp
 * (engines, emit engines, run timing, closed-loop tally); a per-epoch seam in its epoch
 * form receives the run, NetworkRun&.
 */

#include "network/driver/RunTypes.hpp"
#include "network/state/Entities.hpp"         // net::NetworkEntities: the entity tables
#include "network/ar/ArSuspect.hpp"      // ArSuspectLedger / ArSuspectCounters
#include "network/ar/ArCycleBasis.hpp"   // ArBasisMemory / ArCycleBasisCounters
#include "network/ar/ArLattice.hpp"      // ArLatticeState / ArLatticeCounters
#include "network/ar/FixedCycles.hpp"    // FixedCycleCounters
#include "network/assembly/ParamRegistry.hpp" // NetworkParamRegistry: one declared property set per parameter kind
#include "network/assembly/ParamInventory.hpp" // NetworkParamInventory: every parameter instance with its disposition
#include "network/state/CarrySites.hpp"     // CarrySite / CarryMechanism: the carried references of the border
#include "network/assembly/Responses.hpp"      // ResponseSite: the declared responses of the carried instances
#include "network/carry/CouplingExact.hpp"  // CouplingExactEpoch / CouplingExactCounters
#include "network/carry/PivotFrameQ.hpp"    // PivotFrameCounters
#include "network/emit/PbHoldClone.hpp"    // PbHoldClone
#include "network/carry/ClockClone.hpp"     // ClockCloneEpoch / ClockCloneCounters
#include "network/carry/SharedHistory.hpp"  // SharedHistoryState / SharedHistoryCounters
#include "network/solve/AnchorHygiene.hpp" // NetworkAnchorTallies (eq34 reattach hygiene)
#include "network/assembly/SignalIntegrity.hpp" // SignalIntegrityArc + tallies (flicker/gap signal integrity)
#include "network/qc/Raim.hpp"            // NetworkRaimCiTallies (RAIM pool re-host), RaimPoolAdmissionTally
#include "network/qc/WithinEpochCulprits.hpp" // WithinEpochCulpritTally
#include "network/qc/GrossLos.hpp"        // NetworkGrossLosTallies / GrossLosState
#include "network/qc/PrefitCensus.hpp"    // NetworkPrefitCensusTallies (prefit census / fuse / topology / recorder)
#include "network/carry/CarryVceApply.hpp"   // NetworkCarryVceApply (M2 v2 carry variance-factor state)
#include "network/qc/Mdb.hpp"             // NetworkMdbEpoch + NetworkMdbTallies (Baarda MDB reliability)
#include "network/qc/BlockBaarda.hpp"     // NetworkBlockBaarda + BlockBaardaHit + tallies (BlockFormal LOS block census)
#include "network/qc/Congruence.hpp"      // NetworkCongruence + CongruenceHit + tallies (station congruence screen)
#include "network/solve/DatumGraph.hpp"      // NetDatumGraphSpectral + NetworkDatumGraphTallies (datum graph census)
#include "network/assembly/MeasurementAdmission.hpp" // MeasurementAdmissionArc + tallies (product-referenced admission)
#include "network/qc/RhsAttrib.hpp"       // NetworkAttribFrame (INC-5 flight-recorder retention slot)
#include "network/ar/ZStore.hpp"     // ArcKey (D5 inject/hold tag sets)
#include "network/slip/RawObservables.hpp" // IonoCsState, the CS-GF authority (successor to QUEUE 8b)
#include "network/slip/SlipOracle.hpp"  // NetworkSlipOracle (sim-only oracle slip response)
#include "network/slip/OraclePin.hpp"  // NetworkOraclePin (sim-only oracle ambiguity pin)
#include "network/solve/Sbasis.hpp"     // NetworkSbasis::Plan (lifecycle-fn params)
#include "network/assembly/BiasAdmission.hpp" // latched W6 code-bias admission census, ctx.admission (CB-ADM)
#include "network/carry/Inject.hpp"        // NetworkInjectState: the event-harness fault schedule and ledger
#include "network/assembly/CanonicalPairAdmission.hpp" // canonical-pair admission state and counters
#include "network/assembly/SignalRoster.hpp"            // SignalRoster and its counters
#include "network/assembly/ChannelAll.hpp"  // the all-channel census
#include "network/assembly/MultiGnss.hpp"   // the second-constellation telemetry
#include "network/state/ArcLedger.hpp"            // the arc lifecycle machine
#include "network/solve/AnchorReentry.hpp" // the anchor drop memo and its read
#include "network/emit/LedgerEmit.hpp"    // the emitter read and the declared roster
#include "network/diag/RunTraces.hpp" // FlagRate (B8 counters)
#include "network/solve/SolveStations.hpp"                   // SolveStationsCounters
#include "network/solve/ClockAnchor.hpp"                     // ClockAnchorCounters
#include "network/solve/ClockDatum.hpp"                      // ClockDatumCounters
#include "network/state/StateSnapshotCounters.hpp"           // StateSnapshotCounters
#include "network/solve/HandoverStransformCounters.hpp"      // HandoverStransformCounters
#include "network/state/BorderCounters.hpp"                  // BorderCounters
#include "network/ar/SdEwlAr.hpp"                            // SdEwlArCounters
#include "network/solve/ChainDatum.hpp"                      // ChainDatumCounters
#include "network/emit/SolPrep.hpp"                          // SolPrepCounters
#include "network/qc/RaimWithinEpoch.hpp"                    // RaimWithinEpochCounters
#include "network/carry/LevelingCarry.hpp"                   // LevelingCarryCounters
#include "network/assembly/StationBuild.hpp"                 // StationBuildCounters
#include "network/ar/AmbFloat.hpp"                           // AmbFloatCounters
#include "network/solve/PhaseAnchorEpoch.hpp"                // PhaseAnchorEpochCounters
#include "network/diag/AmbStateTrace.hpp"                    // AmbStateTraceCounters
#include "network/carry/SlantIonoCarry.hpp"                  // SlantIonoCarryCounters
#include "network/carry/RxCodeBiasCarry.hpp"                 // RxCodeBiasCarryCounters
#include "network/state/WarmResumeCounters.hpp"              // WarmResumeCounters
#include "network/carry/RxClockCarry.hpp"                    // RxClockCarryCounters
#include "network/carry/ZwdCarry.hpp"                        // ZwdCarryCounters
#include "network/carry/PhaseColourCarry.hpp"                // PhaseColourCarryCounters
#include "network/emit/EmitStore.hpp"                        // EmitStoreCounters
#include "network/slip/PassASlipScanCounters.hpp"            // PassASlipScanCounters
#include "network/carry/InstallCarry.hpp"                    // InstallCarryCounters
#include "network/diag/DdFrac.hpp"                           // DdFracCounters
#include "network/carry/LevelAuthority.hpp"                  // LevelAuthorityCounters
#include "network/emit/PbHold.hpp"                           // PbHoldCounters
#include "network/solve/RankGate.hpp"                        // RankGateCounters
#include "network/solve/CodeBiasDatum.hpp"                   // CodeBiasDatumCounters
#include "network/carry/Coupling.hpp"                        // CouplingCounters
#include "network/state/CarryDisposition.hpp"                // CarryDispositionCounters
#include "network/solve/ClockLevel.hpp"                      // ClockLevelCounters
#include "network/emit/BrdcWire.hpp"                         // BrdcWireCounters
#include "network/diag/PhaseVisibleDiag.hpp"                 // PhaseVisibleDiagCounters
#include "network/assembly/SecondCodeScope.hpp"              // SecondCodeScopeCounters
#include "network/assembly/SecondPhaseScope.hpp"             // SecondPhaseScopeCounters
#include "network/slip/CommonBandScope.hpp"                  // CommonBandScopeCounters
#include "network/solve/SolveGlue.hpp"                       // SolveGlueCounters
#include "network/solve/PhaseDatum.hpp"                      // PhaseDatumCounters
#include "network/driver/EpochAdmission.hpp"                 // EpochAdmissionCounters
#include "network/solve/SbasisCompletion.hpp"                // SbasisCompletionCounters
#include "network/diag/FrameDiag.hpp"                        // FrameDiagCounters
#include "network/qc/RedundancyCensus.hpp"                   // RedundancyCensusCounters
#include "network/qc/PriorResidual.hpp"                      // PriorResidualCounters
#include "network/carry/InvariantCarryDriver.hpp"            // InvariantCarryDriverCounters
#include "network/ar/ProducerAr.hpp"                         // ProducerArCounters
#include "network/carry/DdPriorBuild.hpp"                    // DdPriorBuildCounters
#include "network/diag/StationBlock.hpp"                     // StationBlockCounters
#include "network/qc/ClockQc.hpp"                            // ClockQcCounters
#include "network/diag/ShDevVtec.hpp"                        // ShDevVtecCounters

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

    class RealTimeEngine;   // the closed-loop consumer handed to networkWriteStateSnapshot (part B)

    class GnssConfig;
    struct NetworkConfig;
    class MultiRinexSync;
    class NetworkProductEmitter;
    class NetworkInvariantCarryDriver;
    class NetworkCarryVce;
    class NetworkPriorResidual;   ///< network/qc/PriorResidual.hpp; an engine the run owns (run.engines.prires), like NetworkCarryVce.
    class SphericalHarmonicIonosphere;
    struct GnssTime;

    /// @internal Const snapshot of every run-loop config knob (decomposition
    /// INC-4).  snapshot() reads the getters in the original order; members are
    /// value copies under the original local names and types.  The invariant-carry gate
    /// and the emit-layer on/off flags joined it later.
    struct NetworkKnobs {
        double q_clock{};
        double q_pb{};
        double q_cb{};
        /// network.ledger_reentry: the gap re-entry response takes its population from
        /// the arc ledger's admitted lifecycle.  Read here only by the
        /// run-end table; the drains take the switch through @c SlipQcParams.
        bool ledger_reentry{};
        /// network.ledger_screens (the reason-mask writers and the anchor read) and
        /// network.ledger_anchor (the restore that acts on that read).
        /// Snapshotted for the run-end readout, which has no config; it reads
        /// @ref ledger_anchor.  The driver reads both when it arms the election's anchor
        /// read (@c AnchorReentry::observe and @c restore); the reason-mask
        /// writers take their switch from @c ArcReasonEpoch::armed.
        bool ledger_screens{};
        bool ledger_anchor{};
        bool pl_on{};
        double pl_q_amb{};
        int pl_warmup{};
        double pl_max_gap_s{};
        double pl_var_floor{};
        /// network.phase_leveling_local_block (F3): carry each station's injectable
        /// carried ambiguity set as one jointly whitened local block (the harvested
        /// conditional cross covariances (N_rr⁻¹)_{c_i c_j} on Σ's off-diagonals;
        /// non-admitted columns marginalized) instead of independent per-column rows.
        /// Given the border, the block is exact in every local-local direction: the
        /// per-LOS iono direction and the station-common rx clock, ZWD and rx-DCB
        /// directions alike.  Live only with phase_leveling on, carry_invariant off
        /// and carry_cpl_cond on; the loader warns that the knob is inert otherwise.
        /// The knob is off by default (byte-identical).
        /// This member is the resolved live predicate, not the knob:
        /// @c network.phase_leveling_local_block OR the @c float_kf vehicle, under which
        /// the joint block is structural (design §3.2: the carried covariance folded as a
        /// proper KF prior is F3, and under the vehicle it is the definition of the fold
        /// rather than an option).  @ref pl_blk_from_vehicle says which of the two
        /// supplied it, so run-end telemetry can name the source (RF-1 step 3).
        bool pl_local_block{};
        /// True when the joint block is live because the vehicle is @c float_kf, not
        /// because the F3 knob is armed (the loader refuses the knob under the vehicle).
        /// Read only by telemetry and by the PD-fallback failure bar.  The fold reads
        /// @ref pl_local_block and, by design, cannot tell the two sources apart
        /// (RF-1 step 3).
        bool pl_blk_from_vehicle{};
        /// network.ambiguity_vehicle == "float_kf": the float state with a carried joint
        /// covariance, a floored birth prior, and a bounded drop as the slip response
        /// (docs/refound/RF-1_float_ambiguities_design.md §7.1).  Read by the slip
        /// response (@c networkAmbFloatApplyDrops drops a flagged arc's carried prior to
        /// its birth variance, and the inject folds that arc instead of skipping it), by
        /// the RAIM attribution path (a bounded drop instead of an erase), by the declared
        /// anchor handover, and by the run-end telemetry.
        bool amb_float_kf{};
        /// network.amb_float_birth_var_m2: the floor (m²) applied to the arc's first-stash
        /// variance when the harvest records @ref LocalCarry::var_birth.  0.0 (default) =
        /// unfloored, so var_birth == var_first to the ULP (RF-1 §3.3(b)).
        double amb_birth_var_floor{};
        /// network.amb_float_drop_var_m2: the floor (m²) applied at a drop only, so the
        /// restored prior is max(var_birth, var_first, birth floor, this floor), while the
        /// birth stash reads @ref amb_birth_var_floor alone.  Resolved to 0 off the
        /// float_kf vehicle.  0.0 (default) adds no drop floor (RF-3a).
        double amb_drop_var_floor{};
        bool   amb_drop_decouple_always{};   ///< network.amb_float_drop_decouple_always: a drop clears the arc's regression and block row even at birth strength
        // The per-line-of-sight phase nuisance state.  pc_on is the
        // knob as the loader validated it (float vehicle, phase_leveling, conditional
        // coupling; no carry_invariant), so every site reads it alone.
        bool   pc_on{};              ///< network.phase_colour_state
        double pc_sigma_zen{};       ///< network.phase_colour_sigma_zen_m (m)
        double pc_tau_s{};           ///< network.phase_colour_tau_s (s)
        double pc_floor_deg{};       ///< network.phase_colour_elev_floor_deg (deg)
        double pc_max_gap_s{};       ///< network.phase_colour_max_gap_s (s)
        bool   pc_trace{};           ///< network.trace_phase_colour (and pc_on)
        bool   amb_if_trace{};       ///< network.trace_amb_if (trace class)
        /// The state's stationary variance at elevation @p elev_deg:
        /// (sigma_zen / sin(max(e, floor)))^2.  An unknown elevation (NaN) takes the floor.
        double pcStationaryVar(double elev_deg) const {
            const double e = (elev_deg == elev_deg) ? std::max(elev_deg, pc_floor_deg) : pc_floor_deg;
            const double s = std::sin(e * 3.14159265358979323846 / 180.0);
            const double sig = (s > 1e-6) ? pc_sigma_zen / s : pc_sigma_zen * 1e6;
            return sig * sig;
        }
        /// network.amb_float_drop_floor_after_ep: the processed epoch (@c n_loop_epochs)
        /// from which @ref amb_drop_var_floor is in force; 0 = no gate.  Resolved to 0 when
        /// the floor itself is unarmed, so every site reads one pair of numbers and a
        /// miswired arm cannot half-act (RF-3c).
        int amb_drop_floor_after_ep{};
        /// network.amb_float_declared_handover: the anchor handover as a declared, exact
        /// carry S-transformation instead of a network-wide erase (RF-1 §3.6, step 5).
        /// Acts only under float_kf (@c networkAmbFloatDeclaredHandover).
        bool amb_float_declared_handover{};
        /// network.phase_datum_held_basis (RF-2 item 1).  The loader interlocks it with
        /// float_kf and phase_datum_prioritized; snapshot() also requires the vehicle.
        bool pd_held_basis{};
        /// network.phase_datum_held_cold_release, already ANDed with pd_held_basis (RF-2d).
        bool pd_held_cold_release{};
        /// network.phase_datum_held_mark_cold, already ANDed with pd_held_basis (RF-2d).
        bool pd_held_mark_cold{};
        /// network.phase_anchor_min_age_ep (0 = no age gate; RF-2 item 3).
        int  pa_min_age_ep{};
        /// network.phase_anchor_flag_quiet_ep (0 = no quiet-window gate; RF-2 item 3).
        int  pa_flag_quiet_ep{};
        /// network.trace_amb_state: the per-arc carry-state trace
        /// (@c network_amb_state.csv).  Measure-only; off by default (no file, no rows).
        /// RF-1 §7.4.
        bool trace_amb_state{};
        /// network.trace_amb_state_every: epoch stride of the trajectory rows (>= 1;
        /// snapshot() clamps it).  Event rows (birth, drop, erase) ignore it (RF-1 §7.4).
        int  trace_amb_state_every{1};
        double pb_var_floor{};
        double cb_var_floor{};
        bool fullcov_on{};       ///< network.shared_carry_full_cov: the dense (P_prev+Q)⁻¹ shared carry (C1).
        bool carry_cpl_cond{};   ///< network.carry_conditional_coupling: the conditional local carry on the coupling rows; requires fullcov_on, enforced at load.
        bool carry_cpl_tu{};
        /// network.carry_coupling_exact_update: the exact conditional time update
        /// of the coupling rows in place of the time update's scalar kappa, on every row harvested
        /// at the stash the dense border block belongs to.
        bool carry_cpl_exact{};
        /// network.carry_coupling_exact_cross: the exact update's bracket between
        /// two rows of a station, added in the whitened local block.
        bool carry_cpl_exact_cross{};
        /// network.clock_pivot_frame_q: the carried clocks' process noise in the
        /// pivot frame (network/carry/PivotFrameQ.hpp).
        bool pfq_on{};
        /// network.carry_coupling_clock_clone: the previous epoch's satellite
        /// clocks as one-epoch clone columns in the shared border (filter/NetworkClockClone).
        bool carry_cpl_clone{};
        /// network.carry_coupling_shared_history: the clones' shared
        /// conditional carried as history columns (filter/NetworkSharedHistory).
        bool carry_cpl_shx{};
        /// network.carry_coupling_bias_clone: the phase and code biases on
        /// one-epoch clones with walk factors (filter/NetworkClockClone).
        bool carry_cpl_bclone{};
        /// network.carry_coupling_departed_clone: the keys the coupling time update
        /// re-attributes on one-epoch clones with the stashed prior (filter/NetworkClockClone).
        bool carry_cpl_dclone{};
        /// network.carry_coupling_shared_history_rank: history columns kept
        /// per cloned clock (1 to 8).
        int  shx_rank{1};
        bool clm_on{};      ///< network.clock_level_monitor: the level monitor, measure-only
        bool clm_raise{};   ///< network.clock_level_bound_raise: raise the stashed block to the level bound
        int  clm_restore_credit_ep{};   ///< network.clock_level_restore_credit_ep: epochs of code information the restored level is credited with (0 = all the snapshot's)
        bool clm_restore_raise{};       ///< network.clock_level_restore_raise: raise the restored border block along each satellite's level to its code-only variance, once, at the first leg epoch
        int  clm_innov_window_ep{60};   ///< network.clock_level_innov_window_ep: the level innovation instrument's window, epochs
        bool pv_on{};                   ///< network.phase_visible_monitor: the phase-visible consistency instrument, measure-only
        bool nbp_on{};                  ///< network.apriori_source sp3_brdc_clock: the native broadcast clock a priori; the wire refers to the stamped ephemeris
        int  pv_window_ep{60};          ///< network.phase_visible_window_ep: its window, epochs
        /// network.carry_coupling_gauge_project: hold each (constellation, kind)
        /// coefficient sum of a truncated coupling row at its harvest value, so a departed
        /// column does not turn the row into a high-weight observation of the current datum
        /// gauge pinned at the old realization.  Without it, a few departures in one epoch
        /// can lift the smallest null eigenvalue decades above the rank-gate cutoff and
        /// cause spurious over-specified withholds that clear only when pl_max_gap_s
        /// expires the carry.
        bool carry_cpl_gauge{};
        /// network.carry_coupling_reinit_sources: honour the reinit history with
        /// the κ time update off, and feed it from every realization-step source
        /// (phase-anchor re-reference, eq34 chain re-reference, datum-slip PB
        /// re-realization) rather than from the clock-QC sources alone.  A re-realized
        /// column carries an unknown frame step in its stashed ŝ_prev; folding it as a
        /// full-weight present term would inject that step as a row bias at
        /// conditional-variance (mm) confidence.
        bool carry_cpl_reinit{};
        bool zwd_on{};
        /// network.carry_iono_in_block (I-in-block): the station's present per-LOS iono
        /// columns join its F3 whitened local block instead of a diagonal row (the
        /// retired network.carry_iono, FALSIFIED).  The harvest appends them to `cols` between
        /// the ambiguities and the ZWD, so `PlHarvestBlock::C` is (n_amb+n_iono+[1])²; each
        /// entry stashes with the same `blk`/`blk_idx` as an ambiguity; PASS B pushes one
        /// more `LocalBlockCol` per LOS.  A diagonal per-LOS row asserts the iono direction
        /// that the station's B carried band priors already assert (the B-fold over-count
        /// of SPEC_amb_iono_split_overcount §7 item 17); inside the block the harvested
        /// cross terms subtract it exactly.  The loader guarantees phase_leveling,
        /// pl_local_block and carry_cpl_cond with carry_invariant off.  Off by default
        /// (byte-identical).
        bool iono_blk{};
        double iono_blk_q_m2{};      ///< I-in-block RW process noise, m² of L1 slant delay per epoch, not per second (see GnssConfig::networkCarryIonoInBlockQM2).
        double iono_blk_max_gap_s{}; ///< I-in-block staleness bound (s); the loader has already resolved the "= phase_leveling_max_gap_s" sentinel.
        /// network.carry_rx_clock_in_block (RX-CLK-in-block): the station's receiver clock
        /// column (and, under @c rxclk_isb, its per-system ISB columns) join its F3 whitened
        /// local block.  Otherwise the clock is a free per-epoch local column with no
        /// temporal prior on this path, and it is the rho-direction sink into which the ZWD
        /// and iono terms push their residual error.  It is carried as a random walk about a
        /// carried drift, because @c x_lin(clock) is a run constant here (the producer never
        /// writes its solution back): the whole clock ramp (about 0.3 m/s) lives in x_r, and
        /// a bare RW prior would be about 30 sigma wrong every epoch.  The loader guarantees
        /// phase_leveling, pl_local_block and carry_cpl_cond with carry_invariant off.  Off
        /// by default (byte-identical).
        bool rxclk_blk{};
        double rxclk_q_m2{};        ///< Carried clock-bias random-walk variance, m^2 per epoch, not per second (see GnssConfig::networkCarryRxClockQM2).
        double rxclk_max_gap_s{};   ///< Staleness bound (s); the loader has already resolved the "= phase_leveling_max_gap_s" sentinel.
        double rxclk_rate_var0{};   ///< Initial variance of the carried drift state, m^2/s^2; large on purpose, so an unknown drift injects a near-free prior.
        double rxclk_rate_q{};      ///< Carried drift random-walk variance, m^2/s^2 per epoch.
        double rxclk_jump_k{};      ///< Clock-jump gate (sigma) on the one-step prediction innovation at harvest; exceeding it erases the carry (the wired response of invariant #1).
        bool rxclk_isb{};           ///< Carry the per-system ISB columns too (pure RW, no drift state).
        bool rx_cb_on{};      ///< rx-CB carry (SPEC_rx_cb_carry); loader guarantees code_datum_time_linked.
        bool rxcb_blk{};      ///< RXCB-IN-BLOCK (network.carry_rx_code_bias_in_block): the rx-CB columns fold inside the station's whitened local block; loader guarantees rx_cb_on + coupling + a local block.
        /// The live F3 block predicate: @ref pl_local_block and @ref carry_cpl_cond (the block's
        /// rows are coupling rows).  Read by networkPassB_ (the produce's block route and the
        /// tally's F3 telemetry) and, through the four predicates below, by the harvest
        /// (it was derived in both).
        bool pl_blk_on{};
        /// The carried ZWD column joins the F3 block: @ref zwd_in_block and @ref pl_blk_on.
        /// Read by networkPassB_ and networkLocalCarryHarvest.
        bool zwd_blk_on{};
        /// The carried slant-iono columns join the F3 block: @ref iono_blk and @ref pl_blk_on.
        /// Read by networkPassB_ and networkLocalCarryHarvest.
        bool iono_blk_on{};
        /// The receiver clock (and ISB) columns join the F3 block: @ref rxclk_blk and
        /// @ref pl_blk_on.  Read by networkPassB_ and networkLocalCarryHarvest.
        bool rxclk_blk_on{};
        /// The receiver code-bias columns join the F3 block: @ref rxcb_blk, @ref pl_blk_on and
        /// @ref rx_cb_on.  Read by networkPassB_ and networkLocalCarryHarvest.
        bool rxcb_blk_on{};
        /// network.cycle_slip.relevel_free_epoch: the produce consults the latched free-epoch
        /// set and the tally folds its skips (SPEC_slip_relevel_walk).  Read by networkPassB_.
        bool relevel_free_on{};
        bool rx_cb_datum{};   ///< Zero-mean-over-receivers DCB datum (network.rx_code_bias_datum).
        double rx_cb_datum_sigma{};  ///< Loose physical-bound sigma (m) of that datum (network.rx_code_bias_datum_sigma_m).
        bool sta_prior_pre_pd{};  ///< Fold the station's datum rows before the addStation PD test (network.station_prior_before_pd).
        double rx_cb_q{};   ///< rx-CB carry RW process noise (m²/s).
        double zwd_q{};
        /// network.carry_zwd_in_block (B9-in-block): the carried ZWD column joins its
        /// station's F3 whitened local block instead of folding as a diagonal coupling row
        /// outside the block.  The harvest appends it to `cols`, so `PlHarvestBlock::C` is
        /// (n+1)²; the entry stashes with the same `blk`/`blk_idx`; PASS B pushes it as one
        /// more `LocalBlockCol`.  A diagonal ZWD row asserts a station-common direction that
        /// every carried ambiguity row already asserts: the ~n_LOS over-count named in
        /// SPEC_amb_iono_split_overcount §7 item 17, and the reason B9 was falsified on the
        /// quiet bed.  The loader requires zwd_on and pl_local_block and disables the knob
        /// otherwise.  Off by default (byte-identical).
        bool zwd_in_block{};
        bool zwd_seam_on{};    ///< network.zwd_seam_reseed: post-resume ZWD re-seed window (SPEC_synthetic_minimum_network §17 finding 3).  Not fingerprinted (forward-only resume response policy).
        int  zwd_seam_epochs{};   ///< network.zwd_seam_reseed_epochs: seam-window horizon in emitted epochs.
        /// network.zwd_seam_reseed_q (RS-10b, SPEC_event_harness §21.21): the seam window's
        /// own RW process noise (m²/s).  Negative (the -1 default) = unset: the seam fold
        /// uses @ref zwd_q, byte-identical to the build without this knob.  Not
        /// fingerprinted, by design: @ref zwd_q is fingerprinted, so without this knob the
        /// response's only weight could not be varied on a resume leg.  Read through
        /// @ref zwdQEff, never directly at a fold site.
        double zwd_seam_q{};
        /**
         * @brief The ZWD carry's effective RW process noise (m²/s) for one fold.
         *
         * The one place the RS-10b override is resolved, so every ZWD fold site agrees:
         * the B9 diagonal row, the B9-in-block column, and the prior-residual census's
         * @c fold_var, which must mirror what was folded or the `zwd` family's σ̂₀ is
         * computed against a variance no row used.
         *
         * @param seam_live  True iff the post-resume seam window is open this epoch
         *                   (@c ctx.carry.zwd_seam_ep_left > 0).
         * @return @ref zwd_seam_q when the window is live and the knob is armed (> 0);
         *         otherwise @ref zwd_q (@c network.zwd_carry_q, the continuous carry's
         *         fingerprinted parameter).
         */
        double zwdQEff(bool seam_live) const {
            return (seam_live && zwd_seam_q > 0.0) ? zwd_seam_q : zwd_q;
        }
        /// Snapshot-armed run (state_snapshot_path non-empty): record and harvest the
        /// per-station ZWD posterior every processed epoch, so every snapshot carries Z
        /// rows.  Capture only: no inject, no effect on the solve.
        bool zwd_capture{};
        /// network.resume_frame_restore (SPEC_synthetic_minimum_network §18.10): at a warm
        /// resume, restore the serialized frame state (phase-anchor incumbents and sticky
        /// pin targets, the Q3 clock-anchor per-sat state, the CLK-R held frame), so the
        /// first post-resume election, fold and emit continue the pre-seam realization
        /// instead of re-pinning at cold seeds.  Not fingerprinted (forward-only resume
        /// response policy).  Off = byte-identical resume (the rows are parsed into maps
        /// nothing reads).
        bool   resume_frame_restore{};
        /// network.resume_emit_restore (RS-5, SPEC_event_harness §21.10): at a warm resume,
        /// restore the outputs-only emit engine's emission frame (@c NetworkEmitRebase, the
        /// snapshot's ER rows) before the first emit.  The emitted phase biases then
        /// continue the writer's emission level instead of stepping to the raw solve
        /// frame.  Inert, and says so in the log, when its rows are absent.  The loader requires
        /// resume_frame_restore.  Not fingerprinted (forward-only resume response policy).
        /// Off = byte-identical resume (rows parsed into containers nothing reads).
        bool   resume_emit_restore{};
        /// network.resume_datum_restore (RS-3, SPEC_event_harness §21.7): at a warm resume,
        /// restore the eq34 chain datum's last-epoch spanning tree (sticky datum receiver,
        /// receiver parent edges, satellite component roots, pin targets, longevity
        /// elevations, gap-retained edges), so the election's sticky seed continues the
        /// writer's tree instead of re-electing from the cold arc registry; also restore the
        /// time-linked code-bias previous-value map and ever-seen set, with
        /// @c code_bias_datum_resumed_ left false.  The loader requires
        /// resume_frame_restore (anchor_incumbent is the satellite side of the same tree).
        /// Not fingerprinted (forward-only resume response policy).  Off = byte-identical
        /// resume (rows parsed into containers nothing reads).
        bool   resume_datum_restore{};
        /// network.resume_station_restore (RS-6, SPEC_event_harness §21.13): at a warm
        /// resume, restore the two station-layer memories before the first epoch: the rx-CB
        /// carry (the snapshot's RC rows, through @c makeRestoredLocalCarry, so every entry
        /// carries the §15.4.1 restored-arc sentinel) and the T5 / Iono-2 per-LOS SH
        /// deviation carry (the SD rows, as written).  @c ShDevCarry's own cold test is
        /// @c t_s <= 0, so the restored harvest stamp makes the entry read warm, and the
        /// epoch's aging pre-pass ages it over the true gap.  Each half is inert, and says
        /// so in the log, when its rows are absent.  The loader requires
        /// resume_frame_restore.  Not fingerprinted (forward-only resume response policy).
        /// Off = byte-identical resume (rows parsed into containers nothing reads).
        bool   resume_station_restore{};
        bool   resume_station_rxcb{};     ///< network.resume_station_restore_rxcb (default true): diagnostic sub-switch of @c resume_station_restore, so one §21.13 attribution leg can restore the T5 deviation half alone.
        /// network.resume_signal_canon_restore (RS-8, SPEC_event_harness §21.16): at a warm
        /// resume, restore each station's per-(constellation, band, C|L) canonical-signal
        /// latch (the snapshot's @c CN rows) into its @c PppModel before the first epoch, so
        /// @c PppModel::decideConstellationCanonicals treats the station as already decided
        /// instead of re-deciding the latch from the resume epoch's satellite counts.  A
        /// re-decided latch can switch a station's tracker (L2X and L2W), so an eq34
        /// component loses its datum receiver and its phase-bias families step at the first
        /// post-resume epoch.  Inert, and says so in the log, on a snapshot with no @c CN
        /// rows.  The loader requires resume_frame_restore.  Not fingerprinted (forward-only
        /// resume response policy).  Off = byte-identical resume (rows parsed into a
        /// container nothing reads).
        bool   resume_signal_canon_restore{};
        /// network.resume_rxcb_lin_restore (RS-9, SPEC_event_harness §21.18): at a warm
        /// resume, stash the snapshot's @c RL rows (the writer's code-bias @c x_lin per
        /// (station roster index, CB state key)) and seed each station's code-bias state
        /// from them at its first-epoch @c StateInitializer::initializeCodeBiases, instead
        /// of from one epoch of the resume epoch's code.  RS-6 restores the rx-CB carry's
        /// numerator; this restores the reference its inject gate differences against, the
        /// other edge of the same seam (invariant #7).  Solve-invariant by construction: an
        /// @c x_lin shift is an exact solve no-op for a state that enters linearly, and the
        /// gate is its only consumer.  For that reason the loader refuses
        /// @c network.rx_code_bias_datum with it (that datum's RHS reads @c x_lin).
        /// Requires @c resume_station_restore and its rxcb half.  Not fingerprinted.  Off =
        /// byte-identical resume (rows parsed into a container nothing reads).
        bool   resume_rxcb_lin_restore{};
        /// network.resume_border_restore (RS-7, SPEC_event_harness §21.14): at a warm
        /// resume, rebuild the C1 dense shared-carry covariance (@c SharedCarryCov) from the
        /// snapshot's QK/QP records for every key that also has a restored D row, so the
        /// first warm fold runs the dense @f$(P_{prev}+Q)^{-1}@f$ path instead of the
        /// per-key diagonal a resume otherwise falls back to.  Kept separate from
        /// @c resume_coupling_restore on purpose: that knob also restores the coupling rows
        /// (QL/QZ regressors), whose truncation breaks the family identity
        /// @f$M^Tg_s=-g_r(c)@f$ and therefore needs the gauge projection; a covariance block carries no
        /// regression and no gauge identity, so it is legal on a @c carry_invariant lineage
        /// where 1f is out of regime.  The loader requires @c resume_frame_restore and
        /// disarms the knob, with a warning, without @c shared_carry_full_cov.  Not
        /// fingerprinted (forward-only resume response policy).  Off = byte-identical
        /// resume.
        bool   resume_border_restore{};
        /// network.resume_pb_seam_mark (RS-4, SPEC_event_harness §21.9): at the first emit
        /// after a warm resume, markDatumChange any constellation for which more than
        /// kResumePbSeamMarkFraction of the fresh-baseline phase-bias keys moved past
        /// discThresholdM against their restored emitter baselines.  This is the
        /// declaration the CLK-R resume_seam rule misses, because that rule fires only on a
        /// cold held-frame engine.  Not fingerprinted (forward-only resume response policy).
        /// Off = byte-identical (the census is never taken).
        bool   resume_pb_seam_mark{};
        /// network.resume_coupling_restore (SPEC_synthetic_minimum_network §18.12): at a
        /// warm resume, rebuild each restored leveling / ZWD carry's coupling regression on
        /// the free shared border from the v3 snapshot's QS dictionary and QL/QZ rows
        /// (verbatim, in harvest order), and seed tu_reinit_hist from the columns the write
        /// epoch was re-drawing.  The first PASS-B inject then builds a conditional row
        /// (A_s = M_r, shared-neutral) instead of the plain diagonal prior asserted at the
        /// pure conditional variance, which out-votes the Q3 anchored clock priors at the
        /// first fold (§18.11).  The loader requires resume_frame_restore,
        /// carry_conditional_coupling and carry_coupling_gauge_project.  Not fingerprinted
        /// (forward-only resume response policy).  Off = byte-identical resume (LocalCarry::coupling stays empty, as the
        /// aggregate init leaves it).
        bool   resume_coupling_restore{};
        /// network.readmit_seed_horizon_s: seed-first re-admission (E2), for continuity
        /// across a satellite's absence (SPEC_synthetic_minimum_network §18.4,
        /// SPEC_amb_iono_split_overcount §7, first criterion).  A departed satellite's D2
        /// Clock and CodeBias carry is parked for this many seconds and seeds its
        /// re-registered columns at var + q·gap; the Q3 anchor folds anchored over the true
        /// gap (bounded by this horizon instead of clock_anchor_max_gap_s), and its NIS
        /// guard is the mark-if-moved.  0 = off (byte-identical: nothing is ever parked).
        /// Not fingerprinted: parking adds entries without changing the meaning of any
        /// carried value, and the snapshot never carries a parked entry.
        double readmit_seed_horizon_s{};
        /// network.readmit_coupling_marg_ep: the block/row re-coupling edge at satellite
        /// re-admission.  For this many epochs after a satellite's clock column returns from
        /// an absence of >= kReadmitMinGapEp, every coupling term on its shared columns
        /// takes the departed route.  0 = off.  Requires the coupling rows (checked
        /// by the loader).
        int    readmit_marg_ep{};
        /// network.readmit_seed_code_bias: park and seed the per-signal code-bias columns
        /// too (true, the default) or the clock alone (false).  Inert without
        /// readmit_seed_horizon_s.
        bool   readmit_seed_cb{};
        /// network.readmit_seed_mirrors: the carry LS-VCE monitor and the prior-residual
        /// census snapshots take a parked seed the D2 fold applied, instead of skipping
        /// every parked entry.  Read by networkStashStatus.  Inert without
        /// readmit_seed_horizon_s.
        bool   readmit_seed_mirrors{};
        /// network.resume_d5_warmup_ep (RS-2b, SPEC_event_harness §21.6): processed epochs
        /// after a warm resume during which the D5 innovation gate is diagnostic
        /// only: a tripping chord is counted but not re-tapped, not marked slipped, and its
        /// block is not refolded.  A measurement knob: it holds the restored branch through
        /// the seam so the re-tap cannot destroy the hypothesis it would test.  0 = off.
        /// Not fingerprinted.
        int    resume_d5_warmup_ep{};
        /// network.resume_slip_warmup_ep: epochs after a warm resume during which the
        /// MW/LLI (T0) drain is diagnostic only, because its cold Welford reference (two
        /// samples at the second warm epoch) declares false WL slips and re-levels restored
        /// arcs (measured on the §18.9 bed).  The IF-TD (T1) and wind-up drains stay live,
        /// so an in-gap slip still has a detector.  0 = off.  Not fingerprinted.
        int    resume_slip_warmup_ep{};
        bool carry_vce{};
        bool vce_apply_on{};   ///< LASER 5-95 variance-factor apply (M2 v2, docs/SPEC_carry_vce_apply.md).  Affects the solve; loader guarantees carry_vce.
        bool ura_on{};
        double ura_alpha{};
        bool ura_retro{};
        bool mark_refsat_reset_disc{};   ///< network.mark_refsat_reset_disc (Edge-1, piece 2).
        /// Datum-station slip detection (§9.7.10), resolved for the run: armed and on the
        /// legacy pivot datum (false under any no-pivot datum).  The third producer into
        /// the refsat/retro mark queue; it joins the SolPrep drain condition.
        bool datum_station_detect{};
        double ura_refloat_inflate{};
        /// The retro-bump / URA-inflate feed is armed (B1, P0-2): ura_retro or an inflate
        /// factor above 1.  The within-epoch tier and the post-fit tiers then fill the feed
        /// (before it, a local of the solve).
        bool retro_feed{};
        bool raim_on{};
        double raim_k{};
        double raim_s0_floor{};
        int raim_min_sta{};
        int raim_arc_persist{};
        bool raim_maturity_gate{};
        int raim_maturity_epochs{};
        double raim_withhold{};
        /// Withhold-era release: escalated within-epoch adaptation budget on epochs whose
        /// predecessor was A4-withheld; 0 = off (stock budget everywhere).  The loader
        /// guarantees 0 or > 5, and 0 without an armed A4 ceiling.
        int wh_escalate{};
        /// Erase detector-freed leveling carries at the A4 withhold branch, so the free
        /// re-level survives the discarded epoch (§3b).  The loader guarantees
        /// relevel_free_epoch and an armed ceiling.
        bool wh_free_erase{};
        bool raim_redun{};
        /// Emit the per-row Baarda redundancy census (`network.trace_raim_redundancy`;
        /// SPEC_t3_null_derivation §7.1).  Diagnostic only, and independent of
        /// @ref raim_redun on purpose: it computes `row_r` without using it, so the census
        /// is solve-neutral where arming `raim_redundancy_normalize` is not.  The open
        /// question it serves is whether r_i depends on the signal: the RAIM cell is a max
        /// over signals, so no existing trace separates L1C from L2W/L5Q.
        bool raim_redun_trace{};
        /// RAIM pool re-host, resolved for the run: the post-fit RAIM screen runs on the
        /// invariant-carry lineage against the carry-neutral `qc_amb` census.
        /// Already ANDed with `raim_on` and `carry_invariant`, so the driver, the assembly
        /// and the seam all read one bool and cannot disagree about whether the census
        /// exists.
        bool raim_ci_on{};
        /// The carried-phase RAIM pools admit observation phase rows only, each to the
        /// pool of the ambiguity column its tag names (@c network.raim_pool_data_rows_only).
        /// Read by both pool walks (the post-fit pass in network/qc/Raim.cpp
        /// and the within-epoch assess in network/solve/SolveGlue.cpp) through
        /// @c raimPoolAdmitsRow, and by the run-end admission lines.  The loader guarantees
        /// @c network.raim.  Off = today's declared rule (byte-identical).  Not
        /// fingerprinted (a QC knob).
        bool raim_pool_data_rows_only{};
        /// The gross line-of-sight screen is armed for this run (
        /// docs/SPEC_event_harness.md §24.18).  Resolved once from
        /// @c network.gross_los_screen so the driver, the assembly, the seam and the
        /// teardown all read one bool and cannot disagree about whether the state exists.
        /// It affects the solve.  Off: the seam is never called, no LOS state is
        /// allocated, @c NetStation::gl_code_sigma_scale is never populated, and the run
        /// is byte-identical.
        bool   gl_on{};
        /// The σ multiplier at which a declared line of sight's code rows fold
        /// (@c network.gross_los_deweight): the assembly divides the row's
        /// information weight by its square.  1.0 when the screen is off.  Held in the
        /// knobs, not re-read from config at the assembly, so the publish half and the fold
        /// half cannot disagree mid-run.
        double gl_deweight{ 1.0 };
        /// The whole-satellite stand-down is armed for this run (GL-2, §24.19).  Already
        /// ANDed with @ref gl_on: the satellite rule sets the scope of the gross-LOS rule
        /// and cannot fire without it.  Off: no tally, no state, byte-identical.
        bool   gl2_on{};
        /// network.gross_los_min_rows_rule == available (RF-3b B1).  ANDed with @ref gl_on.
        bool   gl_min_rows_available{};
        /// network.gross_los_bound (RF-3b B2): the declared LOS's freed states are held at
        /// their pre-declaration values with zero process noise instead of being erased.
        /// ANDed with @ref gl_on.
        bool   gl_bound{};
        /// network.gross_los_emit_exclude (RF-3b B3): the screen's decision half runs ahead
        /// of the epoch's emission, and the declared line of sight leaves its satellite's
        /// contributor set for that epoch.  ANDed with @ref gl_on.
        bool   gl_emit_exclude{};
        /// network.gross_los_station_tier (RF-3b B4): the whole-station guard becomes a
        /// station-level declaration with its own two edges.  ANDed with @ref gl_on.
        bool   gl_station_tier{};
        /// network.gross_los_cold_bound_m: the cold-start absolute
        /// bound on a line of sight's post-fit code |v|, metres.  Resolved to 0 (off)
        /// unless @ref gl_on and not @ref gl_bound, so the seam, the act half and the
        /// run-end readout read one number and cannot disagree about whether the cold
        /// path exists.
        double gl_cold_bound_m{ 0.0 };
        /// Declared per-station variance factor (QUEUE item 4 R2, SPEC_t3_null_derivation
        /// §6/§7.5).  Measure-only: it feeds no denominator.
        bool   staf_on{};
        double staf_tau_s{};
        double staf_ratio{};
        /// T3/T3c opportunity-accounting hygiene, telemetry only (QUEUE item 4 R4).
        bool rate_hygiene{};
        /// The per-epoch fleet CS-GF level census; measure-only.
        bool lvlauth_on{};
        int  lvlauth_min_n{};
        int  lvlauth_min_los{};
        bool lvlauth_los_dump{};   ///< Dump the census's own per-LOS pierce points (LV-6 G2).
        bool clk_mon_on{};
        double clk_mon_abs{};
        double clk_mon_jump{};
        bool clk_mon_couple{};
        double clk_mon_reinit_sig{};
        double clk_mon_reinit_var{};
        bool pivot_vr_signal{};
        bool emit_clock_disc{};
        bool sticky_pivot{};
        bool clock_sum_to_zero{};
        bool clock_brdc_anchored{};
        bool cb_time_linked{};
        bool scoped_handover{};
        /// network.handover_stransform (queue item 1(b)): at a pivot handover, re-express the
        /// changed constellation's carry along the structural clock null direction and fold
        /// it, instead of skipping it for one epoch.  The loader forces it false without
        /// scoped_handover and under an anchor-free clock datum.
        bool handover_stransform{};
        /// network.handover_stransform_before_rows: the handover's
        /// S-transform runs before the fold decision and PASS B, through the traversal of
        /// carried references, so the coupling rows read the re-expressed carry.  The loader
        /// forces it false without handover_stransform.
        bool handover_stransform_before_rows{};
        /// network.handover_stransform_dense: the handover's S-transform
        /// re-expresses the carried values and the dense block together by S P S^T
        /// (applyClockStransformDense), the diagonal kernel the fallback.  The loader forces it
        /// false without handover_stransform or shared_carry_full_cov.
        bool handover_stransform_dense{};
        /// network.clock_anchor_datum_keep: the Q3 guard keeps an anchored
        /// clock engaged at a handover the dense kernel re-expressed exactly.  Resolved with
        /// the anchor (cta_on); the loader forces it false without handover_stransform_dense.
        bool cta_datum_keep{};
        /// network.carry_rebase_complete: each carry re-base
        /// mechanism's walk (the ephemeris re-level, the eq34 D4 shift, the declared
        /// anchor handover) also moves the sites its exemption table
        /// marks as owed (carryMechanismOwedSites; the phase hold's held value excepted).
        /// The loader forces it false when none of those mechanisms is armed.
        bool rebase_complete{};
        bool emit_gen_time{};
        bool emit_gen_id{};
        bool ser_disc_ctrs{};
        double contract_rebroadcast_s{};
        bool clk_runaway_on{};
        double clk_runaway_mag{};
        double clk_runaway_rate{};
        bool perconst_on{};
        int perconst_min_rows{};
        bool emit_epoch_status{};
        /// Emit satellite clocks in the per-constellation held frame
        /// (network.emit_clock_frame_hold).  The engine is a run-TU
        /// local (see the file header), like NetworkEmitRebase.
        bool clk_frame_hold{};
        bool clk_jump_test{};   ///< Mark per-sat common-removed clock steps through the R-record retro-bump (network.clock_disc_jump_test).
        bool   emit_pb_hold{};             ///< Hold each (sat, signal) emitted phase bias at the first value of its generation (network.emit_pb_hold); state in NetworkCarryState::pb_hold.
        double emit_pb_hold_max_gap_s{};   ///< Arc gap (s) beyond which a returning key starts a new generation (network.emit_pb_hold_max_gap_s).  Inert unless emit_pb_hold.
        bool   emit_pb_hold_phase_clock{}; ///< Hold the phase-visible clock instead of the phase bias (network.emit_pb_hold_phase_clock).  Inert unless emit_pb_hold.
        bool   emit_pb_hold_smoothed{};    ///< The smoothed hold (network.emit_pb_hold_smoothed): the held phase bias follows the clock clone c(t0|t); LAH's transfer after a clone dies.  Inert unless emit_pb_hold.
        // --- Guarded temporal clock anchor ---
        // Solve-affecting and fingerprinted.  `cta_on` is the one predicate every site reads
        // for "does this run touch the clock carry at all", so an off run never allocates the
        // per-sat state and never diverges from the legacy fold.
        bool   cta_on{};             ///< network.clock_temporal_anchor, resolved (off under network.clock_model)
        double cta_q{};              ///< network.clock_anchor_q (m²/s): the anchor's RW rate; emitted walk = √(q·Δt)
        double cta_var_floor{};      ///< network.clock_anchor_var_floor_m2: deviation-#1 consistency floor (clock analogue of the B4 pb/cb floors); 0 = off
        double cta_max_gap_s{};      ///< network.clock_anchor_max_gap_s: per-sat staleness bound; beyond it the satellite decouples instead of folding a stale value
        double cta_k{};              ///< network.clock_anchor_k: |w| trip threshold on the standardized predicted residual (empirical, §3)
        double cta_abs_m{};          ///< network.clock_anchor_abs_m: floor-independent absolute backstop on |ê| (m); 0 = off
        double cta_sigma_floor_m{};  ///< network.clock_anchor_sigma_floor_m: √S floor, so a weak-data epoch gives w ≈ 0 instead of 0/0
        int    cta_space{};          ///< network.clock_anchor_guard_space: 0 clock, 1 phase_shadow, 2 phase
        double cta_phi_floor_m{};    ///< network.clock_anchor_phase_sigma_floor_m: sqrt(S) floor of the phase-space statistic (m)
        int    cta_recouple_ep{};    ///< network.clock_anchor_recouple_epochs: consecutive in-band epochs a loose satellite must serve before the anchor re-engages (the re-validation of invariant #7)
        bool   cta_mark{};           ///< network.clock_anchor_mark: wire-mark the decoupling edge (per-sat disc through the B1 retro-bump chain)
        /// The one resolved gauge-free predicate that every fold, withdraw and replay site
        /// reads: @c cta_on && cta_gauge_free.  Never re-derive it at a call site.  Fold
        /// and withdraw must agree exactly: a fold that downdated, paired with a withdraw
        /// that did not, leaves the negative-semidefinite residue −ΛU(UᵀΛU)⁻¹UᵀΛ in the
        /// shared normals, which are then not PSD along the gauge.  The constrained solve
        /// then either throws on its pivot guard (a persistent epoch skip, since the stash
        /// never refreshes) or factorizes into a saddle and returns a wrong x_s with no
        /// error.
        bool   carry_gauge_free{};
        /// network.clock_anchor_gauge_free: marginalize the
        /// per-constellation satellite-clock gauge direction out of the folded clock prior,
        /// so the anchor constrains only the estimable between-satellite differences and
        /// leaves the common mode to the datum.  Without it the carry does part of the
        /// gauge's work, which shows as a reduced drained Σ|μ| at CLK-R.
        bool   cta_gauge_free{};
        bool   cta_resolve{};        ///< network.clock_anchor_resolve: adapt and emit; withdraw the tripped prior and re-solve the epoch before emission
        // network.clock_anchor_gauge_free_stat, resolved once (auto follows the fold), so
        // the guard and the census read predicates instead of re-deriving the mapping (the
        // same rule as the clock_model interlock on `cta_on` above).  These select only the
        // guard's denominator.  The withdraw path must keep reading `cta_gauge_free`,
        // because what it un-folds has to be what was folded.
        bool   cta_stat_proj_post{};  ///< P⁺ = the gauge-projected posterior variance of the common-removed contrast, not the raw marginal cov(c,c)
        bool   cta_stat_proj_prior{}; ///< P⁻ = the gauge-projected prior variance of that same contrast, not the raw per-satellite var_eff.  Set only by 'proj', the one consistent pairing.
        // --- Broadcast satellite health on the network path.  SOLVE-AFFECTING and
        // FINGERPRINTED.
        int    sat_health{};         ///< network.sat_health: 0 off, 1 "pivot" (the election passes over an unhealthy satellite), 2 "exclude" (the setup door removed its observations)
        // --- Clock pivot election by predicted station coverage.  SOLVE-AFFECTING and
        // FINGERPRINTED.
        int    pivot_election{};              ///< network.pivot_election: 0 off, 1 "coverage" (ignition and forced handovers elect by predicted roster coverage)
        double pivot_election_horizon_s{};    ///< network.pivot_election_horizon_s: look-ahead of the coverage count and the umbra test (s)
        double pivot_election_step_s{};       ///< network.pivot_election_step_s: sample step inside the horizon (s)
        /// network.clock_model: the two-state (offset and drift) oscillator clock model.  When
        /// on, the satellite clock is carried through the Allan-derived Q model in
        /// NetworkCarryState::clock_osc instead of the scalar prev_carry random walk, and a
        /// satellite is flagged by the normalized-innovation dynamics-fault test rather than
        /// by the absolute and jump bounds.
        bool clk_model_on{};
        double clk_model_q1{};
        double clk_model_q2{};
        double clk_model_k{};
        double clk_model_r{};
        double clk_model_max_gap{};
        double start_offset_s{};
        /// network.trace_flag_rates: the empirical-alpha flag-rate counters.  Observation
        /// only: {opportunities, flags} per QC tier, reported at the end of the run as the
        /// realized rate against the implied one-sided Gaussian alpha for the configured k.
        /// Never fed back into a decision; every increment is guarded, so with the knob off
        /// the output is byte-identical.
        bool trace_flag_rates{};
        bool phase_datum_eq34{};
        bool carry_invariant{};
        /// @c network.carry_invariant_pd_safe, resolved against its @c carry_invariant
        /// prerequisite (docs/SPEC_pd_safe_dd_fold.md).  Fingerprinted: the DD z-store a
        /// pd_safe run writes comes from a different (repaired, product-form) recursion
        /// whose blocks are PD by construction, so a resume across the flip must
        /// cold-start rather than mix the two arithmetics in one lineage.
        bool carry_invariant_pd_safe{};
        /// @c network.carry_aware_covariance, resolved against its @c carry_invariant
        /// prerequisite (docs/SPEC_carry_aware_covariance.md).  Not fingerprinted: its only
        /// consumers are @c dd_sigma_cyc and the log-only AR probe, which feed nothing
        /// back, so on and off agree byte for byte on the solve artifacts and differ in
        /// exactly two trace files.  A solve-affecting successor would need its own
        /// fingerprinted knob.
        bool carry_aware_covariance{};
        /// @c network.ambiguity_resolution.suspect_epochs, resolved against
        /// @c ambiguity_resolution.enable (0 when the search is off): the window, in
        /// searches, of the search's suspect-corner mask (
        /// network/ar/ArSuspect.hpp).  0 = off: no site writes the ledger and the search
        /// is byte-identical.  Not fingerprinted: the ledger is never serialized and the
        /// search it feeds is log-only (its time-only release makes it field-blocking as
        /// the guard of a solve-affecting search).
        int ar_suspect_epochs{};
        double eq34_rpb_prior_sigma{};
        double eq34_rpb_prior_w{};
        double eq34_rpb_q{};
        bool eq34_shift_on{};
        bool eq34_longevity{};
        bool eq34_extend_only{};
        bool eq34_slip_aware{};   ///< network.phase_datum_eq34_slip_aware_election: keep slipped arcs out of the spanning tree, so a slip costs one chord instead of every cycle through a tree edge.
        /// network.phase_datum_eq34_seed_round (docs/SPEC_eq34_seed_round.md): round
        /// otherwise-zero pin targets to λ·round(x_state/λ), so |innovation| ≤ λ/2 by
        /// construction.  Not gated on !carry_invariant: the invariant-carry zero-target rationale
        /// is re-pin continuity, while this knob targets ignition.
        bool eq34_seed_round{};
        /// network.phase_datum_eq34_seed_round_rehost_frame (SPEC §8.5 INC-2): a pin that
        /// re-hosts takes its frame step from the new host's own ambiguity state (the exact
        /// pre-pin float) and stays shiftable, instead of taking an unknown step that
        /// re-references the (sat, σ).  Resolved to false unless eq34_seed_round.
        bool eq34_seed_round_rehost{};
        bool phase_datum_prioritized{};
        // --- eq34 reattach hygiene ---
        // All four are resolved against the datum mode: the loader already disarms them
        // outside an anchored election, and `anchor_hygiene_on` is the one predicate every
        // site reads for "does this run allocate the arc-age map at all", so an off run
        // never touches the new state.
        bool   anchor_hygiene_on{};   ///< Any increment armed on an anchored phase datum: the master gate for the arc-age map and the hygiene seam.
        /// Two-edge continuous handover, resolved for the run: an anchor handover pins the
        /// incoming anchor at its own absolute ambiguity estimate in the outgoing frame
        /// instead of at 0, which makes the gauge change an S-transform.  Already ANDed with
        /// `anchored && !phase_datum_eq34`, so the census fill, the post-solve harvest and
        /// the election all read one bool and cannot disagree about whether the ledger
        /// exists.
        bool   pd_handover_continuous{};
        /// Conditional-variance ceiling (m²) that a handover ledger value must beat before
        /// it may be pinned: the re-coupling re-validation of invariant #7.  0.0 when the
        /// mechanism is off.
        double pd_handover_max_var{};
        /// network.phase_datum_handover_max_var_ratio: the σ-scale ceiling.  A ledger value
        /// is usable only if var <= ratio × its arc's first-stash conditional variance (an
        /// unknown var_first is refused).  The absolute pd_handover_max_var stays as a
        /// rail.  0.0 = off (absolute ceiling only); resolved with pd_handover_continuous.
        double pd_handover_max_var_ratio{};
        /// PHASE_ANCHOR carry re-reference, resolved for the run (queue item 1(d);
        /// docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1).  On an anchor handover,
        /// the (sat, signal) family's shared PB carry is marginalized for one epoch and its
        /// phase-leveling local carries are erased at every station: the eq34_reref
        /// treatment applied to the anchored election, which closes the carry-side edge
        /// whose tension the satellite clock otherwise absorbs.  Already ANDed with
        /// `anchored && !phase_datum_eq34` (the eq34 chain owns its own re-reference),
        /// resolved the same way as pd_handover_continuous.
        bool   pa_carry_reref{};
        /// Cold anchor fallback, resolved for the run (network.phase_datum_candidacy_usable,
        /// docs/SPEC_event_harness.md §15): at a handover the successor is chosen from the
        /// coverage-ranked candidates whose carry is usable by the continuous branch's own
        /// predicate, instead of from the first tracker of the key.  Already ANDed with
        /// `pd_handover_continuous` (with no ledger there is nothing to screen against),
        /// which is itself ANDed with `anchored && !phase_datum_eq34`, so every site reads
        /// one bool, as with the two knobs above.
        bool   pd_candidacy_usable{};
        /// network.phase_datum_candidacy_min_age_ep: the maturity bar in loop epochs
        /// (0 = no age test); 0 when the mechanism is off.
        int    pd_candidacy_min_age_ep{};
        /// Cold anchor fallback, iteration 2 (SPEC_event_harness §15.8): resolve a
        /// satellite's handover jointly, so its families cannot elect different hosts.
        /// Already ANDed with pd_candidacy_usable; false means iteration 1's per-family
        /// screen, so the A/B against iteration 1 changes a single knob.
        bool   pd_candidacy_satellite{};
        int    eq34_min_arc_ep{};     ///< Eligibility threshold in consecutive-present epochs (0 = no gate; INC-1).
        double prior_realization_max_gap_s{};  ///< Warm conditioning (INC-1): the block applies only while the key's last emitted realization is younger than this (s); <= 0 disables the conditioning.
        bool   pin_bridge_on{};       ///< Time-linked phase datum armed (INC-2).
        double pin_bridge_sigma{};    ///< Bridge-row sigma, m (INC-2).
        double pin_bridge_info{};     ///< Bridge-row information 1/sigma^2; 0 when disarmed (INC-2).
        int    pin_bridge_max_ep{};   ///< Maximum consecutive folds of one bridge (INC-2).
        double fresh_pin_scale{};     ///< Maximum sigma multiplier on a fresh-host datum pin; 1.0 = off (INC-3).
        int    fresh_pin_ramp_ep{};   ///< Ramp length in epochs; 0 = off (INC-3).
        // --- S-basis completion, SBC (network.sbasis_complete) ---
        // Resolved once so the pb_level_prev harvest gate and the solve-glue block read the
        // same predicate.  The completion reuses the harvest the INC-2 bridge added: under
        // carry_invariant the shared PhaseBias carry is not in prev_carry, so the
        // completion's continuity targets need the same alternate source as the bridge.
        bool   sbasis_complete_on{};  ///< SBC armed: constrain realized extra null directions instead of withholding.
        int    sbasis_complete_max{}; ///< SBC per-epoch direction cap (beyond it: refuse and withhold); ≤0 = uncapped.
        // --- flicker/gap signal integrity ---
        // All resolved against the master arm: the loader has already disarmed every
        // action when `census` is off, and `sig_integrity_on` is the one predicate every
        // site reads for "does this run allocate the band-state map at all", so an off run
        // never touches the new state.
        bool   sig_integrity_on{};    ///< Master: the band-state classifier runs this epoch (network.signal_integrity.census).
        double sig_unvetted_scale{};  ///< Sigma multiplier on an unvetted LOS's phase rows; 1.0 = no deweight (INC-2).
        bool   sig_withhold{};        ///< Alternative to the deweight: remove the unvetted LOS's phase observables (INC-2).
        int    sig_revet_ep{};        ///< Consecutive full-band epochs required to return to the @c Stable state (INC-3(b)).
        int    sig_flicker_max_ep{};  ///< Maximum band outage (epochs) still classed as a flicker (INC-1).
        bool   sig_refloat_regain{};  ///< Refloat the LOS's ambiguities on all bands when a flickered band returns (INC-3(a)).
        bool   sig_resume_warmup{};   ///< Vet every arc for sig_revet_ep epochs after a warm resume, because tight carries are restored and detector state is not (addendum).
        // --- redundancy / testability census ---
        // `redun_census_on` is the one predicate the driver reads for "does this run enter
        // the census seam at all", so an off run never calls into the translation unit and
        // pays only this bool test.  The census is measure-only, so there is no estimation
        // precondition to resolve against, only the two arming knobs.
        bool   redun_census_on{};     ///< Master: the stride or the escalation trigger is armed.
        // --- Baarda reliability numbers / MDB ---
        // `mdb_on` is the one predicate both feed sites and the flush read for "does this
        // run compute an MDB at all", so an off run pays one bool test per site.  The two
        // source flags are resolved here, not at the sites, because the run-end
        // invariant-#4 lines must say which feed was armed when a source contributed zero
        // rows: "the RAIM pool never ran warm" and "no sweep completed" need different
        // fixes.
        bool   mdb_on{};              ///< Master: network.mdb and at least one live r_i source.
        bool   mdb_from_raim{};       ///< The RAIM-pool feed site is armed (raim and raim_redundancy_normalize).
        bool   mdb_from_census{};     ///< The redundancy-census feed site is armed (mirrors @ref redun_census_on).
        double mdb_event_m{};         ///< Reporting threshold, metres (0 = no events).  Nothing acts on it.
        long long mdb_event_holdoff{};///< Per-cell MDB event rate limit, in flush passes.
        // --- prior-residual census ---
        // `prires_on` is the one predicate every feed site reads for "does this run touch
        // the census at all", so an off run pays one bool test per site.  The per-family
        // arms are resolved here (the families CSV is parsed once at run start, never per
        // row), so a feed site in the serial carry harvest tests a bool instead of
        // searching a vector.
        bool   prires_on{};           ///< Master: the census is armed and at least one known family was requested.
        bool   prires_pl{};           ///< Phase-leveling ambiguity carry family armed.
        bool   prires_pb{};           ///< Satellite phase-bias carry family armed.
        bool   prires_cb{};           ///< Satellite code-bias carry family armed.
        bool   prires_zwd{};          ///< ZWD (B9) carry family armed (INC-2).
        bool   prires_rxcb{};         ///< Receiver code-bias carry family armed (INC-2).
        /// Satellite-clock carry family armed.  Unlike every other family,
        /// its ê is common-mode removed per constellation before standardizing: the clock is
        /// the one carry with a free per-epoch gauge, and the raw ê would report that gauge
        /// on every satellite (standing invariant #5).  Trace-only; this family is the
        /// empirical instrument from which the anchor's q and k are derived (§3).
        bool   prires_clk{};
        bool   prires_corrob{};       ///< Raw-observable corroboration join armed (INC-3).
        double prires_report_sigma{}; ///< Per-row trace threshold in σ̂₀ units (nothing acts on it).
        int    prires_event_holdoff{};///< Per-cell PRIORRES event rate limit, in census epochs.
        // --- constraint-aware global σ̂₀
        bool   prires_s0g{};          ///< The global variance-component rider is armed (implies @ref prires_on).
        bool   prires_s0g_redun{};    ///< A per-row Baarda r_i exists for the data side (`raim` and `raim_redundancy_normalize`).  False: the r ≡ 1 fallback, flagged per epoch.
        // --- BlockFormal LOS block census ---
        // `bb_on` is the one predicate for "does this run sweep blocks at all", read at one
        // site (inside the redundancy census's swept branch) and by the emission seam.  It
        // requires both the master arm and a live host: the census has no scheduler and
        // rides the redundancy census's swept stations, so with that census off there is
        // nothing to ride and the sweep must not be entered.  The loader fails on that
        // combination; this repeats the check in the one place a knob may be computed
        // (ctx.knobs is const thereafter).
        bool   bb_on{};               ///< Master: network.block_baarda and a live redundancy-census sweep.
        bool   bb_corrob{};           ///< Raw-observable corroboration join armed (implies @ref bb_on; INC-2).
        bool   bb_estimable{};        ///< The sweep tests the estimable part of each block (implies @ref bb_on); network.block_baarda_estimable.
        double bb_estimable_eps{};    ///< Floor on a kept direction's redundancy number; read only with @ref bb_estimable.
        double bb_report_sigma{};     ///< Trace/event threshold on t_B.  A variance ratio (E = 1 under H0), not σ units; see BlockBaarda.hpp.
        long long bb_event_holdoff{}; ///< Per-(station, satellite) BLOCKBAARDA event rate limit, in census epochs.
        // --- Station congruence screen ---
        // `cg_on` is the one predicate for "does this run screen coordinates at all".  It is
        // read at three sites (the assembly's INC-0 geometry retention, the redundancy
        // census's swept branch, and the emission seam) and, like `bb_on`, requires both
        // the master arm and a live host: the screen has no scheduler and rides the
        // redundancy census's swept stations, so with that census off there is nothing to
        // ride.  The loader fails on that combination; this repeats the check in the one
        // place a knob may be computed.
        bool   cg_on{};               ///< Master: network.congruence_screen and a live redundancy-census sweep.
        double cg_report_sigma{};     ///< Trace/event threshold on t_i.  A variance ratio (E = 1 under H0 for every observable dimension), not σ units; see Congruence.hpp.
        long long cg_event_holdoff{}; ///< Per-station CONGRUENCE event rate limit, in census epochs.
        // --- prefit census / fuse / birth check / topology / flight recorder ---
        // `pc_walk_on` is the one predicate the driver reads for "does this run walk the
        // row stacks at all", so an off run never enters the seam.  It is the union of the
        // census, the birth check and the topology ledger on purpose: INC-3 and INC-4 ride
        // the same walk, so arming either implies the walk even with the INC-1 columns off
        // (the loader's arming line says so).  The fuse is separate: it runs at the
        // pre-addStation assembly site, not in this walk.
        bool   pc_walk_on{};          ///< Master for the post-solve walk (census, birth check or topology).
        bool   pc_census_on{};        ///< Columns and PREFIT exceedance events (INC-1).
        double pc_event_sigma{};      ///< Event threshold on the standardized deviation (INC-1).
        unsigned pc_event_mask{};     ///< Event domain (INC-1): bit (1 << NetRowClass) per data class allowed to raise an exceedance event (the columns always cover all four).
        long long pc_event_holdoff{}; ///< Per-cell event rate limit, in processed epochs (INC-1/2).
        double pc_fuse_sigma{};       ///< Weight cap (0 = off; INC-2), the one solve-affecting knob of the mechanism.
        unsigned pc_fuse_mask{};      ///< Cap domain (INC-2): bit (1 << NetRowClass) per data class the cap may act on.
        double pc_birth_sigma{};      ///< Seed-birth tripwire threshold (0 = off; INC-3).
        bool   pc_topo_on{};          ///< TOPOLOGY events (INC-4).
        bool   rec_on{};              ///< Flight recorder armed: trip latch and retention (INC-5).
        bool   h_sigma0_census{};     ///< H-rider: feed the H heartbeat's sigma0 from the pooled census σ̂₀ (network.h_sigma0_prefit_census).  Loader guarantees trace_prefit_census and emit_epoch_status_record.
        unsigned h_sigma0_class_mask{};  ///< H-rider: data classes pooled into the H σ̂₀ (network.h_sigma0_classes; default code+sh+other, phase excluded per census deviation 2c).
        // --- datum graph census ---
        // `dg_on` is the one predicate for "does this run predict the S-basis dimension at
        // all", read at three sites: the driver's census call, the top-of-epoch clear of the
        // spectral hand-off, and the solve glue's write of it.  All three must agree or the
        // census would reconcile against a stale epoch, which is why it is one resolved
        // knob and not three independent config reads.
        bool      dg_on{};            ///< Master: network.datum_graph_census.
        bool      dg_x_family{};      ///< network.n3c_channel_family: the census groups by "X against the rest".
        long long dg_event_holdoff{}; ///< Per-identity DATUMGRAPH event rate limit, in processed epochs.
        // --- product-referenced measurement admission ---
        // Nothing to resolve against the estimation configuration: the screen reads
        // `observed` / `modeled` and removes observables, so the two arming knobs are the
        // predicate, and the loader has already clamped the thresholds.  `adm_on` is the
        // one test the driver reads for "does this run enter the seam at all"; it is the
        // union of the response and the census, since the census rides the same pass and
        // arming either implies the pass (the loader's arming line says so).  Screening
        // implies the census columns, per spec §5.
        bool   adm_on{};              ///< Master for the screen pass (response or census).
        bool   adm_screen{};          ///< The response (INC-1/INC-2), the one solve-affecting knob of the mechanism.
        bool   adm_trace{};           ///< Census columns and ADMISSION events, implied by the response (INC-3).
        double adm_bound_m{};         ///< |z| rejection bound (m).
        int    adm_min_sats{};        ///< median-validity floor in distinct satellites.
        int    adm_heal_ep{};         ///< consecutive in-bound epochs before a rejected channel is readmitted.
        double adm_max_frac{};        ///< station-guard fraction on the median's trustworthiness.
        // Re-entry vet (INC-4).  `adm_revet` is already ANDed with `adm_screen` by the
        // loader's inert guard, and it is re-imposed here so the seam's predicate cannot
        // depend on load order.
        bool   adm_revet{};           ///< Re-screen an arc re-entry as a birth instead of inheriting its pre-gap verdict (INC-4).
        int    adm_revet_gap_ep{};    ///< absent screened epochs before a return counts as a re-entry.
        int    adm_revet_ep{};        ///< screened epochs the vet window stays open after a re-entry.
        double adm_revet_bound_m{};   ///< the tighter |z| bound (m) a re-entering / vetting channel answers to; == adm_bound_m when the scale is 1.0.  Resolved to metres here so the seam never re-multiplies.
        bool   adm_revet_census{};    ///< consult the previous epoch's PREFIT obs_code exceedance flags inside a vet window.
        bool   adm_revet_hold{};      ///< strict posture: withhold a clean re-entry until the adm_heal_ep path readmits it.
        // --- invariant-carry gate and emit layers ---
        // Resolved here, with the driver's former conjunctions unchanged, so the driver, the
        // DD-prior build, the install, the emit store and the run end read one bool each
        // instead of each receiving it as a parameter.  Every one is false unless
        // carry_invariant.  Read from the configuration, never written; the snapshot
        // fingerprint does not read them (it reads the configuration getters it needs).
        bool carry_innov_gate{};         ///< D5 estimation-side fine-slip innovation gate: carry_invariant and network.carry_invariant_innov_gate.
        bool emit_rebase_on{};           ///< Emit-layer frame re-base of the emitted phase biases: carry_invariant and network.carry_invariant_emit_rebase.
        // --- the epoch driver's configuration-derived values ---
        // Each is a value runNetwork computed from the configuration, once in its setup or
        // again on every epoch, and read in its epoch body or passed to a seam.  Resolved here
        // with the driver's expression unchanged, so the driver and those seams read one field
        // each.  Constant for the run: the configuration is not written after load, and setup
        // builds the SH model exactly when network.estimate_iono_sh is set (the sh_dev_* rows
        // read that flag where the driver tested the model).  The
        // snapshot fingerprint reads none of them.
        bool   sec_timers_on{};    ///< network.trace_section_timers: the D0 / D0b per-epoch section timers.  Read by the driver (the estimator and carry-driver timers, the networkSubBegin / networkSubEnd brackets, the per-epoch readout) and by networkPassB_ (the D0 assembly wall).
        /// The a-priori satellite-clock anchor sigma (m), resolved once by
        /// GnssConfig::resolveNetworkAprioriClockAnchorSigmaM from
        /// network.apriori_clock_anchor_sigma_m (an explicit value > 0 wins; brdc_rts auto
        /// 3.0 m; sp3 auto initial_std.sat_clock), so the anchor fold and its
        /// information-fraction diagnostic share one value.  The < 100 m binding gate stays at
        /// the fold site (SolveGlue.cpp).  Read by networkSolveGlue (the clock datum's
        /// declaration and the anchor fold), networkHandoverStransformBeforeRows (the
        /// declaration) and networkIonoDiag (the B7 anchor-information diagnostic).
        double clk_anchor_sigma{};
        /// The SH-deviation carry's correlation time (s):
        /// estimation.global_vtec_sh.rep_error_tau_s with a co-estimated SH field
        /// (network.estimate_iono_sh), else 0.  Read by networkPassB_ (the per-LOS aging).
        double sh_dev_tau{};
        /// The SH-deviation carry is live: @ref sh_dev_tau > 0.  The one derivation of this
        /// predicate: read by networkPassB_, networkShDevVtec and its run-end report
        /// (networkShDevCarryRunEnd).
        bool   sh_dev_on{};
        bool   sh_dev_demean{};    ///< Anti-ratchet de-mean (Iono-2 v3): sh_dev_on and estimation.global_vtec_sh.rep_error_demean.  Read by networkPassB_.
        bool   sh_dev_d5_keep{};   ///< Erase-policy split (Iono-2 v3k): sh_dev_on and estimation.global_vtec_sh.rep_error_d5_keep.  Read by networkPassB_.
        bool   vce_wants_pb{};     ///< The carry LS-VCE monitor (network.carry_vce) lists the "pb" family in network.carry_vce_components.  Read by networkStashStatus.
        bool   vce_wants_cb{};     ///< The same for the "cb" family.  Read by networkStashStatus.
        bool   streaming_on{};     ///< network.streaming.enable: the RTF-3 streaming front-end is the epoch source.  Read by the driver (the epoch source, and the code-bias admission census, which is refused under streaming).
        /// network.raim_within_epoch: the within-epoch RAIM tier (T2) is armed, so the solve
        /// glue captures the shared-border folds into the epoch workspace's replay channel
        /// (EpochWorkspace) and may re-solve the epoch without a culprit.  Read by
        /// networkSolveGlue.
        bool   within_epoch_armed{};
        bool   pd_sticky{};        ///< network.phase_datum_sticky: the prioritized election keeps a (satellite, signal) anchor while it still tracks the satellite with a usable ambiguity.  Read by the driver (the election's first argument).
        bool   anc_slip_handover{};///< network.cycle_slip.anchor_slip_handover (SPEC_event_harness §12.7): a family whose anchor arc a slip just freed is handed to a successor before the election.  Read by the driver.
        bool   pd_gate_drop_erase{}; ///< network.phase_datum_gate_drop_erase: the coverage gate's drop edge erases the family's carried ambiguities.  Read by the driver (PhaseGateDrop::on).
        int    pd_min_stations{};  ///< network.phase_datum_min_stations: the phase datum's coverage gate.  Read by the driver as the threshold column of the gate-drop event row.
        double max_duration_s{};   ///< processing.max_duration_s: the data span after which the run stops (0 = no limit).  Read by the driver's max-duration cutoff.
        std::size_t epoch_decimation_n{1}; ///< network.epoch_decimation_n clamped to >= 1: only every Nth synced epoch is processed (T2e; 1 = every epoch).  Read by the driver's decimation gate.
        double rhs_attrib_tow{-1.0}; ///< network.rhs_attrib_tow: the first TOW of the reduced-RHS attribution window (< 0 = off).  Read, with @ref rec_on, where the driver arms the estimator's capture recorder; the window test itself stays per epoch.
        int    rhs_attrib_n_ep{1}; ///< network.rhs_attrib_n_ep clamped to >= 1: the window's length in processed epochs.  Read with @ref rhs_attrib_tow.
        bool   trace_postfit_on{}; ///< network.trace_postfit_sigma0, trace_postfit_keyed or trace_postfit_rows: the post-fit residual pass (networkPostfitSigma0) runs.  Read by the driver.
        bool   trace_sh_spectrum{};  ///< network.trace_sh_spectrum: networkShSpectrum runs.  Read by the driver.
        bool   trace_station_block{};///< network.trace_station_block: networkStationBlock runs.  Read by the driver.
        bool   amb_birth_floor_on{}; ///< The float vehicle's birth variance floor is armed: @ref amb_float_kf and @ref amb_birth_var_floor > 0.  Read by the driver (this epoch's elevation map for the bounded drop) and by networkPassB_ (the inject).

        /// One-shot factory: the knob-snapshot statements, in the original read order
        /// (parameter names match the former use sites).
        /// Defined in src/core/network/driver/Knobs.cpp.
        static NetworkKnobs snapshot(const GnssConfig& config_, const NetworkConfig& nc);
    };

    /**
     * @brief The warm resume's hand-offs to the first leg epoch: each is
     *        set only by @ref networkTryWarmResume and read, then cleared or left to expire,
     *        by its consumer at the first post-resume epoch.  Never serialized: each resume
     *        is its own seam.  Persist::ResumeHandoff (net::kRunFields).
     */
    struct ResumeHandoff {
        /// A warm resume restored LB rows and the raise waits for the first leg epoch.
        /// Was @c NetworkCarryState::clm_restore_raise_pending.
        bool clm_raise{false};
        /// One-shot latch set by @ref networkTryWarmResume beside
        /// @ref NetworkCarryState::resume_datum_first_ep and consumed by the first
        /// @c networkCodeDatumFamilyKeptEpoch call.  With
        /// @ref NetworkCarryState::last_if_pin_holds not snapshotted, the first kept epoch after a resume
        /// cannot tell a hold that begins there from one the writer's last kept epoch
        /// carried, so the holds it finds are declared "hold_resumed" and counted apart
        /// from the holds begun.  Telemetry only: nothing reads it when no hold exists.
        /// Never serialized.
        /// Was @c NetworkCarryState::if_pin_holds_resume_pending.
        bool if_pin_holds{false};
        /// This run resumed from a snapshot, so each station's first screened epoch labels a
        /// first-sighting failure `adm_reject_resume` (a restored corrupt arc) rather than
        /// `adm_reject_birth`.  Written unconditionally by a successful warm resume; inert
        /// unless the screen is armed, and byte-identical either way, like
        /// @c anchor_age_resume_seed.
        ///
        /// It is not a one-shot consumed by the first seam pass.  The seam screens only the
        /// stations that are present and above the satellite floor, so a station that is
        /// absent, thin, or whose satellite the two-station observability gate erased on
        /// that epoch would have the flag consumed on its behalf, and every later first
        /// sighting there would read as a birth.  The response does not depend on the
        /// distinction (the refloat is unconditional), but the event name does.
        /// Was @c NetworkCarryState::adm_resume_pending.
        bool adm{false};
        /// One-shot phase-bias seam latch (RS-4, SPEC_event_harness §21.9).  Set by
        /// @ref networkTryWarmResume beside @ref NetworkCarryState::resume_datum_first_ep on every warm resume
        /// and consumed by the first emit in NetworkEmitStore, where the emitter's restored
        /// per-key baselines are still the previous run's values.  Read only under
        /// @c knobs.resume_pb_seam_mark, so an unarmed run is byte-identical.  Never
        /// serialized: each resume is its own seam, and a leg that resumes twice has two
        /// seams.
        /// Was @c NetworkCarryState::resume_pb_seam_pending.
        bool pb_seam{false};
        /// Latch for the first-post-resume harvest census and fold split (§18.12).
        /// Was @c NetworkCarryState::resume_cpl_census_pending.
        bool cpl_census{false};
    };

    /// @internal Tier-1 cross-epoch carry and run state: the persistent declarations of
    /// runNetwork(), relocated with their comments.  The per-field comments are the
    /// authoritative docs and the snapshot-exemption record.
    struct NetworkCarryState {
        /// The warm resume's hand-offs to the first leg epoch.
        ResumeHandoff resume;
        std::vector<CarryPrior> prev_carry;                 // the D2 stash from the last solve
        // nbp_prev_iode: moved to net::SatelliteState::nbp_prev_iode (network/state/Entities.hpp).
        // Per-satellite guarded temporal clock anchor.  Empty and untouched unless
        // network.clock_temporal_anchor.  Not snapshotted by default: a warm resume is an
        // invariant-#1 event, so every satellite re-earns the anchor from the loose state
        // and the stiffness does not outlive the evidence that justified it.  The frame
        // pair is the exception: network.state_snapshot_frame_capture writes the per-sat
        // state of folded satellites, and network.resume_frame_restore restores it
        // (SPEC_synthetic_minimum_network §18.10).  The knob is in the resume fingerprint,
        // so a toggle change cold-starts.
        ClockAnchorState clk_anchor;
        // Satellites whose anchor decoupled and whose per-sat wire mark has not yet landed
        // in an emitted product.  Drained by networkSolPrep with the same present/pending
        // split that every other forced-bump queue uses (mark == land), so a replay
        // reproduces the marks from the serialized R records.
        // clk_anchor_disc_pending: moved to net::SatelliteState::disc_pending (net::Disc::ClockAnchor) (network/state/Entities.hpp).
        // prev_clock_refloat_sats: moved to net::SatelliteState::prev_clock_refloat (network/state/Entities.hpp).
        // tu_reinit_hist: moved to net::SatSignalState::tu_reinit (network/state/Entities.hpp).
        SharedCarryCov prev_carry_cov;                      // dense covariance side-car of prev_carry, empty when off (C1)
        // Exact update (network.carry_coupling_exact_update).  The stash records when the dense
        // block was taken and which clock the datum pinned then; PASS B's serial preamble
        // builds the epoch (the fold mirror, S/D split and the factor of P_SS + Q_S), which
        // the parallel produce reads and the D2 fold checks.  Not snapshotted: after a warm
        // resume the stash stamp is invalid until the first kept stash, and the rows take
        // the plain time-update route for that one epoch (counted).  Empty and unread when off.
        double cxu_stash_s{0.0};                            ///< continuous-GPS s of the stash prev_carry_cov belongs to
        bool   cxu_stash_valid{false};                      ///< the stash was taken this run with the dense block present
        // cxu_pivot, pfq_pivot: moved to net::ConstellationState (network/state/Entities.hpp).
        CouplingExactEpoch cxu_epoch;                       ///< this epoch's inputs; ok = false when off or unbuilt
        ClockCloneEpoch    ccl;                             ///< This epoch's clock clones and the rows' copy of cxu_epoch (filter/NetworkClockClone); rebuilt every armed epoch, empty off
        SharedHistoryState shx;                             ///< The held compression and this epoch's history columns (filter/NetworkSharedHistory); not snapshotted, empty off
        // clm_info: moved to net::SatelliteState::clm_info (network/state/Entities.hpp).
        // clm_n: moved to net::SatelliteState::clm_n (network/state/Entities.hpp).
        // clm_info_c: moved to net::SatelliteState::clm_info_c (network/state/Entities.hpp).
        std::map<std::string, double> clm_seam_q;           ///< Satellite -> level process noise of this epoch's time update (m^2), set at a switch-on resume, cleared after the fold
        // clm_restore_raise_pending: moved to NetworkCarryState::resume.clm_raise (ResumeHandoff).
        // Level innovation instrument, measure-only.
        // clm_prev_clk: moved to net::SatelliteState::clm_prev_clk (network/state/Entities.hpp).
        Eigen::MatrixXd               clm_prev_pclk;        ///< previous stash's clock-clock block (m^2), the innovation's prior variance
        // clm_prev_pidx: moved to net::SatelliteState::clm_prev_pidx (network/state/Entities.hpp).
        double                        clm_prev_s{-1.0};     ///< continuous GPS seconds of the previous monitored epoch
        // clm_win: moved to net::SatelliteState::clm_win (network/state/Entities.hpp).
        // clm_win_ep: moved to NetworkServiceCounters (decision D6).
        // pv_win_ep, pv_win_nis_sum, pv_win_nis_n, pv_win_e2_sum, pv_win_su_sum: moved to
        // NetworkServiceCounters (decision D6).
        // Readmit continuity: E2 seed-first re-admission and the block re-coupling edge
        // (network.readmit_seed_horizon_s / readmit_coupling_marg_ep).  Not snapshotted
        // (forward-only response policy): a seam during a satellite's absence loses its
        // seed, which gives the ordinary cold return, and the marginalization windows
        // restart empty.  All empty or zero, and unread, when both knobs are off.
        double readmit_now_s{0.0};          ///< continuous-GPS s of the current epoch, stamped at the D2 install (the stash has no clock of its own)
        double readmit_prev_stash_s{0.0};   ///< continuous-GPS s of the previous kept stash: the park stamp a departing entry receives
        // readmit_last_seen_ep: moved to net::SatelliteState::readmit_last_seen_ep (network/state/Entities.hpp).
        // readmit_marg: moved to net::SatelliteState::readmit_marg (network/state/Entities.hpp).
        // readmit_seeded_s: moved to net::SatelliteState::readmit_seeded_s (network/state/Entities.hpp).
        // readmit_seed_check: moved to EpochEvents::Solve (network/driver/Epoch.hpp).
        // ddfrac_ref_age, ddfrac_ref_prev: moved to net::ChannelState (network/state/Entities.hpp).
        // prev_pivot: moved to net::ConstellationState::prev_pivot (network/state/Entities.hpp).
        // c1_zoh_active: moved to net::SatelliteState::c1_zoh_active (network/state/Entities.hpp).
        // last_pivot: moved to net::ConstellationState::last_pivot (network/state/Entities.hpp).
        // Deterministic IF-pin selection discontinuity.  The pin is re-derived from every
        // epoch's coverage; the deterministic NetworkSbasis::selectIfPinPair makes it a
        // pure function of the registered signal set, so a change between emitted products
        // means a channel entered or left the ≥2-station set.  That steps the emitted
        // (δtˢ, dˢ) split by the satellite's inter-channel code bias, a per-sat datum move
        // the consumer must reset for.  Two maps mirror the phase-anchor discipline
        // (this_epoch_anchor / emitted_anchor), so a transient pin move on a withheld
        // epoch cannot fabricate a bump:
        //  - this_epoch_if_pin_sel: the (sig1, sig2) selected this epoch in the main pass
        //    (scratch, cleared each epoch, decoupled from the emit decision);
        //  - last_if_pin_sel: the pair of the last emitted product per satellite, compared
        //    and advanced only at the emit site (like emitted_anchor / prev_pivot), so a
        //    withheld epoch leaves it untouched and its transient pair is dropped.
        // Both are populated only under emit_clock_disc, so the output is byte-identical
        // otherwise.  Not snapshotted: a warm resume has no prior for its first emit, so
        // it must not fabricate a disc it cannot verify.
        // this_epoch_if_pin_sel: moved to EpochEvents::CodeDatum (network/driver/Epoch.hpp).
        // last_if_pin_sel: moved to net::SatelliteState::last_if_pin_sel (network/state/Entities.hpp).
        // this_epoch_if_pin_changes: moved to EpochEvents::CodeDatum (network/driver/Epoch.hpp).
        // this_epoch_if_pin_holds: moved to EpochEvents::CodeDatum (network/driver/Epoch.hpp).
        // last_if_pin_holds: moved to net::SatelliteState::last_if_pin_hold (network/state/Entities.hpp).
        // if_pin_holds_resume_pending: moved to NetworkCarryState::resume.if_pin_holds (ResumeHandoff).
        // this_epoch_cb_family_rows: moved to EpochEvents::CodeDatum (network/driver/Epoch.hpp).
        // this_epoch_cb_family_births: moved to EpochEvents::CodeDatum (network/driver/Epoch.hpp).
        // this_epoch_cb_family_birth_skips: moved to EpochEvents::CodeDatum (network/driver/Epoch.hpp).
        // last_cb_family_partner: moved to net::ChannelState::last_cb_family_partner (network/state/Entities.hpp).
        // S-contract §1.5 declaration scratch: the realized code-bias datum pair per
        // satellite, recorded unconditionally (the only guard is a real dual-frequency pin,
        // f1, f2 > 0) and read only by the cbpin= fill in NetworkSolPrep.  It is kept apart
        // from this_epoch_if_pin_sel on purpose.  That map is the disc-compare scratch,
        // gated on emit_clock_disc: reading it would make a product-contract declaration
        // depend on a QC-channel knob (emit_clock_disc defaults false, so a posture without
        // it would declare no pin), and widening it would change which satellites get a
        // discontinuity bump, a wire change.  This map records the pair the pin realized
        // (after the family pair hold, when armed), so the declaration tracks the datum the
        // run realized.  Cleared each epoch; declaration-only, so the solve and every
        // emitted numeric value are untouched.
        // this_epoch_if_pin_real: moved to EpochEvents::CodeDatum (network/driver/Epoch.hpp).
        /// This epoch's per-(satellite, companion signal) inter-signal-correction verdicts,
        /// keyed `"sat|signal"` (RF-0f).  A key in @ref this_epoch_isc_obs means some
        /// present station tracked both that companion and the T_GD pin's band-2 signal on
        /// that satellite, so `ISC^s = b(companion) - b(pinned)` is determined by data and
        /// the bias is emitted carrying `isc=obs`.  A key in @ref this_epoch_isc_pinned
        /// means none did: the column was pinned at 0, and the bias is withheld from the
        /// wire, because a consumer cannot tell an asserted zero from an estimate by value.
        /// Cleared and refilled each epoch by the solve glue, read by the emit prep; both
        /// empty unless `network.sat_isc_state` is armed.
        std::set<std::string> this_epoch_isc_obs;
        std::set<std::string> this_epoch_isc_pinned;
        // this_epoch_extra_band_held: moved to EpochEvents::CodeDatum (network/driver/Epoch.hpp).
        // Prioritized phase-bias anchor (network.phase_datum_prioritized).  All three maps
        // are keyed by the "sat_signal" ambiguity form and are empty and unread on the
        // legacy path:
        //  - anchor_incumbent: the sticky incumbent anchor station name per (sat, signal),
        //    read when electing (kept while it still tracks the sat), advanced at election.
        //  - this_epoch_anchor: the anchor station name elected this epoch, captured at
        //    election and read at the emit site (decouples emit from the PASS-A scratch).
        //  - emitted_anchor: the anchor name of the last emitted product per (sat, signal),
        //    advanced only at emit (mirrors prev_pivot / last_pivot); a change against it
        //    drives the per-satellite handover discontinuity.  Never erased: a returning
        //    key is a first sighting, not a fault (the never-erase policy of last_pivot).
        // anchor_incumbent is written to the snapshot under
        // network.state_snapshot_frame_capture and restored under
        // network.resume_frame_restore.
        // anchor_incumbent: moved to net::SatSignalState::anchor_incumbent (network/state/Entities.hpp).
        // this_epoch_anchor: moved to EpochEvents::PhaseDatum (network/driver/Epoch.hpp).
        // anchor_pin_target: moved to net::SatSignalState::anchor_pin_target (network/state/Entities.hpp).
        /// Two-edge continuous handover ledger: each anchor-candidate station's absolute
        /// ambiguity estimate from the previous epoch's solve, so a handover can re-pin the
        /// successor in the outgoing frame instead of re-zeroing it.  Rebuilt wholesale by
        /// the post-solve harvest each epoch; the one-epoch lifetime bounds what a
        /// decoupled gauge may carry (invariant #7).  Empty unless
        /// `knobs.pd_handover_continuous`.  Not serialized into the snapshot, so a resume's
        /// first handover is counted as cold rather than pinned against a stale frame;
        /// under network.resume_frame_restore the resume primes the ledger from the
        /// restored leveling carries for the first live epoch instead.
        AnchorHandoverLedger handover;
        // emitted_anchor: moved to net::SatSignalState::emitted_anchor (network/state/Entities.hpp).
        // eq34 reattach hygiene, warm conditioning (spec addendum): the continuous-GPS
        // second at which each key's datum was last realized and emitted, i.e. the age of
        // the warm memory a cold pin would have to fight.  Advanced at the same emit site
        // as emitted_anchor, and read only by filter/NetworkAnchorHygiene.  Past
        // eq34_prior_realization_max_gap_s there is nothing left to shock (the carried
        // state has expired through its own staleness machinery), so the eligibility block
        // lifts and the key converges the legacy way.  This keeps a network restart from
        // extending its own phase-bias outage by min_arc_ep epochs.  Empty and unread
        // unless the mechanism is armed; not snapshotted (a resumed leg re-earns its baselines
        // on the first emitted epoch).
        // emitted_anchor_s: moved to net::SatSignalState::emitted_anchor_s (network/state/Entities.hpp).
        // Chain election of the eq34 datum (network.phase_datum_eq34); all empty and unread
        // otherwise:
        //  - eq34_datum_rx: the sticky datum receiver r1 station name per (constellation |
        //    signal) component, keyed "c|σ".  Kept while the incumbent is still present in
        //    the component (fewest datum-receiver handovers); advanced at election (mirrors
        //    anchor_incumbent's per-election stickiness).
        //  - anchor_incumbent / this_epoch_anchor / emitted_anchor are reused for the tree
        //    bookkeeping and hold each satellite's parent-edge station (unique per
        //    "sat_signal" key).  Receiver-side parent edges live in eq34_rx_parent, not
        //    here, so an interior satellite's several tree edges never collide in one map
        //    slot.  The existing emit-site handover machinery (jump-test disc and trace)
        //    applies unchanged: a parent-edge reattach is an anchor change.
        //  - eq34_rx_parent: each receiver's parent-edge satellite in last epoch's spanning
        //    tree, keyed "<station>|c|σ".  anchor_incumbent (satellite side) and
        //    eq34_rx_parent (receiver side) together are last epoch's tree: each edge lives
        //    in exactly one map, and both are replaced wholesale per election, so the
        //    still-live subset seeded into the sticky rebuild is always a forest.
        //  - eq34_sat_root: each satellite's component datum receiver r1 last epoch, keyed
        //    "sat_signal".  A root change (r1 handover, component migration) shifts the
        //    whole component frame by a common fractional part plus per-sat integers, so it
        //    re-references every satellite even where all tree edges survived.
        // eq34_datum_rx: moved to net::ChannelState::datum_rx (network/state/Entities.hpp).
        // eq34_rx_parent: moved to net::StationSignalState::eq34_rx_parent (network/state/Entities.hpp).
        // eq34_sat_root: moved to net::SatSignalState::eq34_sat_root (network/state/Entities.hpp).
        // eq34_prev_el: moved to net::LosState::eq34_prev_el (network/state/Entities.hpp).
        // The datum receiver r1 (pdr=) of the last emitted contract, so a root handover
        // triggers an S-line re-broadcast (advanced only at emit, like the pivot
        // baselines).  Empty on every non-eq34 run.
        std::string eq34_emitted_pdr;
        // eq34_reref_total, eq34_erase_total, eq34_shift_total, eq34_cpl_sprev_total,
        // eq34_gap_repin_total, eq34_mismatch_epochs: moved to NetworkServiceCounters (
        // decision D6).
        // Chain-pin audit of the eq34 datum (docs/SPEC_synthetic_minimum_network.md §14 T1; measured
        // record §12.8): this epoch's declared chain-gauge dimension per (constellation,
        // signal) group, keyed "<cons>|<signal>" (the election's dr_key), value Σ over the
        // group's components of (n_c + m_c).  eq34_epoch_bridged counts the hygiene INC-2
        // bridge pins per group, which are realized on the shared side (a bridge row, not
        // a foldAmb pin), so the station-side fold expectation is declared − bridged.  Both
        // are replaced wholesale at each eq34 election and are empty on every non-eq34 run.
        // After PASS B's serial consume they are compared against the pins that landed
        // (folded by foldAmb/foldRpb and on a station that est.addStation accepted), which
        // covers the two silent loss paths that standing invariant #4 warns about.
        // eq34_epoch_declared: moved to EpochEvents::PhaseDatum (network/driver/Epoch.hpp).
        // eq34_epoch_bridged: moved to EpochEvents::PhaseDatum (network/driver/Epoch.hpp).
        // Tree edges retained across the current gap because their local_carry is still
        // alive: {receiver index, "sat_signal"}.  Rebuilt at each election's sticky-map
        // update; read at the next election's recordEdge to count a gap re-pin.  Empty
        // unless network.phase_datum_eq34_extend_only.
        // eq34_retained_edges: moved to net::ArcState::eq34_retained_edge (network/state/Entities.hpp).
        // Last epoch's pin target per (station index, "sat_signal") for the eq34 datum: the exact
        // same-station pinmove baseline for the shift walk.  It must be keyed per station:
        // an interior satellite carries several pins (its parent edge and one per
        // child-receiver edge), each at a different station with its own target.  An
        // amb-only key collides, and the resulting wrong pinmoves feed back through the
        // carry shifts into the next epoch's rounded targets, which then diverge.
        // Replaced wholesale each election.
        // eq34_prev_target: moved to net::ArcState::eq34_prev_target (network/state/Entities.hpp).
        // Per-epoch re-reference set (sat, signal) of the eq34 datum: cleared and rebuilt at each
        // eq34 election, and read (1) by the local-carry erase right after the election and
        // (2) by the D2/C1 shared-carry consume, which must marginalize the re-referenced
        // PB keys for that epoch.  Their carried priors belong to the retired frame, and
        // folding them pulls the new frame by up to the full re-level with no signal on the
        // wire.  Always empty when eq34 is off.
        // eq34_reref: moved to EpochEvents::PhaseDatum (network/driver/Epoch.hpp).
        // PHASE_ANCHOR carry re-reference (queue item 1(d);
        // docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1).  Per-epoch (sat, signal)
        // set of the anchor election's handovers: cleared and rebuilt at each anchor
        // election, and read (1) by the local-carry erase right after the election and (2)
        // by the D2/C1 shared-carry consume, which marginalizes the re-referenced PB keys
        // for that epoch.  This is the eq34_reref treatment applied to the anchored
        // (non-eq34) election.  Without it, a handover re-realizes the datum on the design
        // matrix only, while the carry is folded forward in the retired frame at a much
        // stiffer q_pb than the satellite clock's q, so the clock absorbs the whole frame
        // step.  Always empty unless ctx.knobs.pa_carry_reref.
        std::set<std::pair<std::string, std::string>> phase_anchor_reref;
        // pa_reref_total, pa_reref_erase_total: moved to NetworkServiceCounters (
        // decision D6).

        // --- eq34 reattach hygiene ---
        // Every member below stays empty unless ctx.knobs.anchor_hygiene_on, so an off run
        // allocates nothing and the election is structurally unchanged.  None of it is
        // serialized: the warm-resume rule is a seed (see anchor_age_resume_seed), not a
        // restore.  Adding an age map to the snapshot would change the format for every
        // run, armed or not (as with the sd_ewl exemption), and sub-min_arc_ep fidelity
        // across a resume is deferred.
        //
        // Arc age per (station index, "sat_signal"): consecutive epochs this arc has been
        // present with an anchorable phase-bias state.  Rebuilt wholesale each election (an
        // absent arc drops out, so absence resets the age, the conservative direction), and
        // erased on a slip-funnel hit or a pending-reseed flag for the arc, so a refloated
        // ambiguity never presents itself as a mature host (standing invariant #1).
        // anchor_arc_age: moved to net::ArcState::anchor_arc_age (network/state/Entities.hpp).
        // Warm-resume seeding latch (spec §2 INC-1, "warm-resume rule").  Consumed by the
        // first hygiene pass of a resumed run, which seeds every present arc at
        // eq34_anchor_min_arc_ep instead of 1; otherwise a resumed leg would block every
        // reattach for min_arc_ep epochs.  Seeded from presence rather than from carried
        // keys on purpose: stacks that run with phase_leveling off have an empty
        // local_carry, so a carry-keyed seed would seed nothing.
        bool anchor_age_resume_seed = false;
        // pb_level_prev: moved to net::SatSignalState::pb_level_prev (network/state/Entities.hpp).
        // PinBridge: moved to net::PinBridge (network/state/Entities.hpp).
        // pin_bridge: moved to net::SatSignalState::pin_bridge (network/state/Entities.hpp).
        // Engage/release identity of the S-basis completion (SBC, network.sbasis_complete):
        // the satellites participating (|eigenvector component| >=
        // NetworkSbasis::kSbasisAttrSpan) in a direction constrained last epoch.  Under
        // standing invariant #7 the constraint is a decoupling at its engage edge and a
        // re-coupling at its release edge, and both must be events: a satellite present
        // here but in no direction this epoch has been re-observed (the data re-determine
        // the held combination from its carried level) and emits sbc_release.  Kept per
        // satellite rather than per direction on purpose: the eigenvectors of two weak
        // satellites of the same scale mix, so a direction-shaped identity would flicker
        // with the rotation while the participating-satellite set stays stable.
        // sbc_active_sats: moved to net::SatelliteState::sbc_active (network/state/Entities.hpp).
        // Declared datum directions currently breached (R >= 1: some pre-gate fold is
        // pinning a direction the datum owns), for invariant #7.  Persistent across epochs
        // only so the breach and clear edges can be emitted: a direction that frees itself
        // must produce a clear edge rather than read as a first sighting the next time it
        // breaches (the re-coupling half of invariant #7).  The identity is the direction
        // label ("CLK|G").
        std::set<std::string> ddi_breached;
        // Keys currently held out of the anchored set because no candidate host has
        // converged (INC-1 with the bridge disarmed or unavailable).  Persistent, and
        // required: a held-out key is absent from this_epoch_anchor, so it also drops out
        // of anchor_incumbent at the end of the election.  Without this set it would read
        // as a first sighting the next epoch, pass the gate (first sightings are never
        // gated) and be pinned on the same still-cold host one epoch later.  The hold lasts
        // until an eligible host appears, which is guaranteed: a held-out key's arcs keep
        // ageing (its phase-bias states are re-allocated by the next epoch's PASS A, so it
        // stays in usable_keys), so the hold clears itself after at most
        // eq34_anchor_min_arc_ep epochs of continuous presence.
        std::set<std::string> anchor_blocked;
        // si_arc: moved to net::LosState::si_arc (network/state/Entities.hpp).
        // Resume warm-up countdown (spec addendum).  Set to `sig_revet_ep` by a successful
        // warm resume and decremented by each classifier pass; while it is positive, every
        // present line of sight is demoted regardless of its own band state.  A resume
        // restores tight carries but no detector state (nothing in the snapshot
        // deserializes MW Welford counts or IF-TD arc history), so the first epochs have no
        // working detector anywhere in the network against fully warm state.  That is the
        // flicker hazard at scale, and the one case the per-arc rule cannot see, because
        // every arc looks like a first sighting to an empty si_arc.  0 on every cold start,
        // so cold starts are exempt.
        int si_resume_warmup_ep = 0;

        // --- product-referenced measurement admission ---
        // Snapshot: exempt.  Per-(station index, "<sat>|<band digit>") admission record:
        // whether the channel is currently in the model set, and how many consecutive
        // in-bound epochs a rejected one has accumulated toward readmission.  Keyed per
        // band rather than per signal, because the response covers both observables of a
        // band and a receiver fault that displaces a channel does not respect RINEX
        // attribute letters.
        //
        // Empty unless ctx.knobs.adm_screen, so an off (or measure-only) run allocates
        // nothing.  Not serialized, and the exemption is required: the mechanism is not
        // fingerprinted so that an off/on pair can share one snapshot, and persisting an
        // observer's memory would make a forward-only admission policy
        // fingerprint-relevant.  A resumed leg's first pass therefore sees every channel as
        // unknown, which is the case @c resume.adm labels: the arc's state was
        // restored (possibly carrying the corrupt seed) while this observer's memory was
        // not.
        // adm_arc: moved to net::StationState::adm_arc (network/state/Entities.hpp).
        // adm_resume_pending: moved to NetworkCarryState::resume.adm (ResumeHandoff).
        // adm_resume_seen: moved to net::StationState::adm_resume_seen (network/state/Entities.hpp).
        // adm_event_last: moved to net::StationState::adm_event_last (network/state/Entities.hpp).
        // Per-epoch L1 scalars (INC-3).  @c adm_ep_tow is the stamp the epoch-status writer
        // compares against, so an epoch the screen did not run on leaves the columns blank
        // instead of repeating stale numbers (the redun_ep_tow pattern).
        // adm_ep_tow: moved to EpochEvents::Admission (network/driver/Epoch.hpp).
        // ep_adm_sta_worst_z: moved to EpochEvents::Admission (network/driver/Epoch.hpp).
        // ep_adm_net_worst_z: moved to EpochEvents::Admission (network/driver/Epoch.hpp).

        // Whether the last processed epoch moved the clock datum.  A clock-pivot handover
        // re-references every satellite phase bias by the pivot's clock (the phase
        // observable ties δtˢ + b_s), so a bridge value harvested before it belongs to the
        // retired frame.  Written where datum_changed is computed (solve glue), read by the
        // next epoch's hygiene pass (bridge drop, reason=datum).
        bool last_datum_changed = false;
        // sts_consts: moved to EpochEvents::Handover (network/driver/Epoch.hpp).
        // sts_c0: moved to EpochEvents::Handover (network/driver/Epoch.hpp).
        // sts_dense: moved to EpochEvents::Handover (network/driver/Epoch.hpp).
        // sts_pre: moved to EpochEvents::Handover (network/driver/Epoch.hpp).
        /// Per constellation, the transform the preview applied to the
        /// carry (the incoming pivot, c0, var_q), held until the next stash, which re-expresses
        /// the carry in the solve's datum and clears the record.  A skipped epoch leaves it set:
        /// the next preview keeps the carry when it elects the same pivot (the handover is
        /// released without a second transform) and undoes the transform when it elects
        /// another (the outgoing pivot again, or a third one, which is then transformed to from
        /// the outgoing datum).  Cold-started on a warm resume (not snapshotted): a snapshot is
        /// written after the stash, which has cleared it.
        struct StsCarryDatum {
            std::string pivot;     ///< the pivot whose datum the carry is in
            double      c0{0.0};   ///< the coefficient the transform applied (m)
            double      var_q{0.0};///< the variance it added to every clock (m^2)
        };
        std::map<char, StsCarryDatum> sts_carry_pivot;

        // prev_clk: moved to net::SatelliteState::prev_clk (network/state/Entities.hpp).
        // clock_osc: moved to net::SatelliteState::clock_osc (network/state/Entities.hpp).
        // The broadcast-ephemeris IODE to which each satellite's carried clock state
        // (clock_osc.x0, the D2 clock carry, the clock-monitor jump baseline) is currently
        // referenced.  Advanced at the eph-cutover re-level (before the solve), not
        // deferred to the emit site like prev_pivot: the re-level mutates the carried state
        // whether or not this epoch emits (the post-emit carry updates are skipped on a
        // withhold or solve failure, so the shifted values persist), and deferring the
        // advance would re-detect the same roll next epoch and shift twice.  Empty and
        // inert unless brdc_rts.  Named for the reference rather than the emit because it
        // also advances on withheld epochs.
        // relevel_iode: moved to net::SatelliteState::relevel_iode (network/state/Entities.hpp).
        // Sats whose ephemeris rolled but whose prior version was already evicted from the
        // store, so Δ_eph cannot be computed: a forced per-sat discontinuity is queued and
        // lands at the satellite's next actual emit (present/pending split, like
        // ura_retro_sats).  Unreachable in steady state (per-epoch checking makes two rolls
        // between consecutive checks impossible); a defensive path for a snapshot gap or a
        // two-roll corner case.
        // eph_cutover_disc_pending: moved to net::SatelliteState::disc_pending (net::Disc::EphCutover) (network/state/Entities.hpp).
        // code_bias_prev_: moved to net::ChannelState::code_bias_prev (network/state/Entities.hpp).
        // The IF-pin reference pair (sat -> {sig1, sig2}) that produced the values in
        // code_bias_prev_, with the same kept-epoch lifecycle (rebuilt at the D2 harvest
        // from the scratch below).  A pair change between kept epochs re-references the
        // emitted clock↔code split by the satellite's inter-channel bias (the band-1
        // C1C↔C1W flip), so the IF-pin RHS re-seeds and the disc bumps even when both of
        // the new pair's signals have previous values.  Snapshotted and restored with
        // code_bias_prev_ (RS-3 datum capture and restore only).
        // code_bias_ifpin_pair_: moved to net::SatelliteState::code_bias_ifpin_pair (network/state/Entities.hpp).
        // This epoch's selected IF-pin pair per satellite (valid pairs only), captured in
        // the main pass and copied into code_bias_ifpin_pair_ at the harvest (kept epochs
        // only, as for code_bias_prev_, so a pair recorded on a withheld epoch never
        // advances the comparison baseline).  Cleared each epoch.
        // this_epoch_cb_ifpin: moved to EpochEvents::CodeDatum (network/driver/Epoch.hpp).
        // Satellites that re-seed this epoch (IF-pin split or a cold sum-to-zero channel).
        // A per-epoch scratch (cleared each epoch), populated in the main pass and merged
        // into cb_reseed_disc_pending only at the emit site, which every whole-epoch
        // withhold path `continue`s before reaching.  A transient re-seed on an epoch that
        // is then withheld (the frame reverts by the next kept epoch) therefore never
        // enters the pending set and cannot fire a bump with no emitted value step (the
        // emit-only discipline of this_epoch_if_pin_sel / last_if_pin_sel), and no erase is
        // needed that would destroy a bump legitimately deferred for a satellite re-seeded
        // while pruned from the emission.
        // this_epoch_cb_reseed: moved to EpochEvents::CodeDatum (network/driver/Epoch.hpp).
        // The channels ("<constellation letter>|<signal>") whose sum-to-zero row re-seeded
        // this epoch (CodeBiasDatumRows::reseed_channels), cleared with
        // this_epoch_cb_reseed.  Read only by the band-5 receiver-delay harvest
        // (network.estimate_rx_extra_band_code_delay), which erases the receiver IFB carry
        // entries of a re-seeded family.
        // this_epoch_cb_reseed_channels: moved to EpochEvents::CodeDatum (network/driver/Epoch.hpp).
        // Satellites whose code-bias split has ever been harvested: a later re-seed of one
        // of these is a return (disc bump), not a first appearance (seeded silently).
        // Accumulates for the whole process.  Snapshotted and restored with
        // code_bias_prev_ (RS-3 datum capture and restore only).
        // code_bias_sat_seen_: moved to net::SatelliteState::code_bias_seen (network/state/Entities.hpp).
        // A warm resume without the RS-3 datum restore starts code_bias_prev_ and the seen
        // set empty, but the pre-snapshot product was emitted, so every first post-resume
        // re-seed is a re-reference the consumer must be told about.  The resume block sets
        // this to true in that case, and re-seeds then bump regardless of
        // code_bias_sat_seen_ for the rest of the resumed run (conservative; harmless on a
        // new satellite's first emit).  It stays false when network.resume_datum_restore
        // brought the map back: the datum then continues across the seam, and the restored
        // seen set keeps the return/first-appearance distinction.  Not snapshotted.
        bool code_bias_datum_resumed_ = false;
        // Satellites to disc-bump for a time-linked datum re-seed (a no-veteran sum-to-zero
        // channel or an IF-pin split forced to the 0 seed, so the emitted value stepped).
        // Fed at the emit site from this_epoch_cb_reseed (kept epochs only), then flushed
        // with the same present/pending split as eph_cutover_disc_pending, so the bump
        // lands with the satellite present (emit-only, replay-faithful).  Populated only
        // under code_datum_time_linked, so it is empty (a byte-identical no-op) otherwise.
        // cb_reseed_disc_pending: moved to net::SatelliteState::disc_pending (net::Disc::CbReseed) (network/state/Entities.hpp).
        double start_anchor_s = -1.0;                        // first-epoch time anchor for network.start_offset_hours
        // Phase-leveling per-station ambiguity carry (increment 1): persistent across
        // epochs, keyed by (station index, sat, signal).  Ordered, so the inject order
        // (which feeds rank-1 LDLᵀ updates) is byte-identical serial and parallel.  Inert
        // unless network.phase_leveling.
        // local_carry: moved to net::ArcState::amb (network/state/Entities.hpp).
        /// Continuous-GPS-seconds stamp of the previous harvest, i.e. the value
        /// @c LocalCarry::last_seen_s carries for an entry stashed one processed epoch
        /// ago.  Written at the end of every harvest; -1 before the first.  Readers
        /// difference it against this epoch's stamp for the observed inter-epoch spacing.
        double pl_prev_now_s{-1.0};
        /// The previous receiver-clock harvest's stamp (s), for RX-CLK-in-block.  A second
        /// field rather than a read of @c pl_prev_now_s, because that one has already been
        /// advanced to this epoch by the time the clock harvest runs.  Used only to convert
        /// a sighting gap into epochs for the per-epoch q.
        double rxclk_prev_now_s{-1.0};
        /// This epoch's @c network_amb_state.csv rows (@c network.trace_amb_state; RF-1
        /// §7.4), buffered here and drained serially by @ref networkAmbStateTrace at the
        /// end of the epoch (as @c sd_ewl_trace does, so the file order does not depend on
        /// the thread count).
        ///
        /// A buffer rather than a direct write because the row sources are spread across
        /// the epoch and some run after the harvest: the trajectory rows come from the
        /// leveling harvest (SB_LEVELING), while the `erase:` rows come from the gap prune
        /// (also SB_LEVELING, after the harvest refreshed last_seen_s), the T4 attribution
        /// and T3 consensus refloats (SB_RAIM), and the phase-anchor carry re-reference
        /// (the datum block).  Cleared by the drain, so an epoch that never reaches it
        /// cannot leak rows into the next one.  Empty on every unarmed run.
        std::vector<AmbStateRow> amb_state_rows;
        // phase_basis: moved to net::SatSignalState::phase_basis (network/state/Entities.hpp).
        // amb_flag_ep: moved to net::StationState::amb_flag_ep (network/state/Entities.hpp).
        /// Sim-only oracle slip response (network.cycle_slip.oracle_relevel,
        /// include/network/slip/SlipOracle.hpp): the run-scope truth table, the per-arc
        /// application ledger and the run-end counters.  Null on every run where the knob
        /// is off: the four detector actuation sites take a `NetworkSlipOracle*` and never
        /// reach a call, so the off path is structurally byte-identical (standing invariant
        /// #6).  Owns a raw pointer back to @c local_carry (a sibling member of this
        /// struct, so it outlives the oracle); set at construction.  Not snapshotted: a
        /// diagnostic, and a resumed leg re-reads the fixture anyway.
        std::unique_ptr<NetworkSlipOracle> slip_oracle;
        /// Sim-only oracle ambiguity pin (`network.inject.events[] kind
        /// "oracle_pin"`, include/network/slip/OraclePin.hpp): the run-scope truth table,
        /// the parsed scopes, this epoch's gauge-consistent pin table and the run-end
        /// counters.  Null on every run where no `oracle_pin` event is armed; the one
        /// serial call site and the PASS-B row both test the pointer first, so the off
        /// path is structurally byte-identical (standing invariant #6).  Lives beside
        /// @ref slip_oracle because both are truth-fed diagnostics keyed on (station index,
        /// satellite, signal).  Not snapshotted: a diagnostic, and a resumed leg re-reads
        /// the fixture anyway.
        std::unique_ptr<NetworkOraclePin> oracle_pin;
        // zwd_carry: moved to net::StationState::zwd (network/state/Entities.hpp).
        /// Seam-scoped ZWD re-seed window (network.zwd_seam_reseed): emitted epochs
        /// remaining in the post-resume inject window.  Armed by networkTryWarmResume when
        /// the knob is on and the snapshot restored >= 1 Z row; decremented once per
        /// processed epoch at the serial harvest; 0 = window closed (the retire, or
        /// decoupling, edge, logged).  Read-only in the parallel produce phase.  Not
        /// snapshotted: each resume is its own seam and re-arms a full horizon.
        int zwd_seam_ep_left{0};
        /// network.resume_slip_warmup_ep countdown (SPEC_synthetic_minimum_network
        /// §18.10): epochs remaining during which the MW/LLI drain is diagnostic only after
        /// a warm resume.  Armed by networkTryWarmResume, decremented once per processed
        /// epoch at the PASS-A drain dispatch, never serialized (each resume is its own
        /// seam).  0 on every continuous epoch.
        int resume_slip_warmup_left{0};
        /// network.resume_d5_warmup_ep countdown (RS-2b, SPEC_event_harness §21.6):
        /// processed epochs remaining during which the D5 innovation gate is
        /// diagnostic only after a warm resume.  Armed by @ref networkTryWarmResume and
        /// decremented once per epoch at the install seam, the one site where the gate
        /// lives, so a run that never installs a posterior never uses up the window.  Never
        /// serialized (each resume is its own seam).  0 on every continuous epoch, which
        /// keeps the off path byte-identical.
        int resume_d5_warmup_left{0};
        /// First-post-resume-epoch measurement latch (RS-3, SPEC_event_harness §21.7).  Set
        /// by @ref networkTryWarmResume on every warm resume, armed or not, so an A/B's
        /// control leg reports the same three figures from the same instrument; cleared at
        /// the emit site in NetworkSolPrep once the counts have been taken.  Telemetry
        /// only: nothing downstream reads it, so an off run is byte-identical (the shape of
        /// @c anchor_age_resume_seed / @c resume.adm).  If the first processed
        /// epochs are withheld, the window extends to the first emitted epoch, the right
        /// boundary for a consumer-facing readout, and the rerefs accumulate rather than
        /// being overwritten.  Never serialized: each resume is its own seam.
        bool resume_datum_first_ep{false};
        // resume_pb_seam_pending: moved to NetworkCarryState::resume.pb_seam (ResumeHandoff).
        // PbHoldKey: moved to net::PbHoldKey (network/state/Entities.hpp).
        // pb_hold: moved to net::SatSignalState::pb_hold (network/state/Entities.hpp).
        /// |estimate - held| (m) over every held emission: the cost of the
        /// decoupling edge, measured rather than asserted (invariant #7).  Read at run end
        /// for p50 / p90 / max; empty on an off run.
        std::vector<double> pb_hold_dev_abs_m;
        /// The de-commoned satellite clock step (m) at every epoch where a mark naming both
        /// the clock and the phase-bias channel reaches a held key.  This is the
        /// one move the hold does not transfer, on purpose, so its size is the measured
        /// cost of the identity-passing contract; empty on an off run.
        std::vector<double> pb_hold_coupled_step_m;
        // pb_hold_prev_clock_m: moved to net::SatelliteState::pb_hold_prev_clock_m (network/state/Entities.hpp).
        // pb_hold_frz_M: moved to net::ConstellationState::pb_hold_frz_M (network/state/Entities.hpp).
        // pb_hold_frz_prev_clk: moved to net::SatelliteState::pb_hold_frz_prev_clk (network/state/Entities.hpp).
        /// |transfer step| (m) per held emission, the per-epoch move the mode
        /// writes on top of the held value (invariant #7, the decoupling edge's rate).
        std::vector<double> pb_hold_frz_step_abs_m;
        /// |transfer| (m) per held emission, the accumulated move since the
        /// generation start.
        std::vector<double> pb_hold_frz_total_abs_m;
        // smh_clone: moved to net::SatSignalState::smh_clone (network/state/Entities.hpp).
        // smh_pending: moved to net::SatSignalState::smh_pending (network/state/Entities.hpp).
        /// Continuous GPS seconds of the first held emission (the hour basis).
        double smh_t_first_s{-1.0};
        // smh_block: moved to net::SatSignalState::smh_block (network/state/Entities.hpp).
        // smh_block_pending: moved to net::SatSignalState::smh_block_pending (network/state/Entities.hpp).
        // iono_blk_carry: moved to net::LosState::iono_blk (network/state/Entities.hpp).
        // gl_state: moved to net::LosState::gross_los (network/state/Entities.hpp).
        // gl2_sat_down: moved to net::SatelliteState::gl2_down (network/state/Entities.hpp).
        // gl2_sat_since: moved to net::SatelliteState::gl2_since (network/state/Entities.hpp).
        // gl2_wire_pending: moved to net::SatelliteState::wire_prune_pending (network/state/Entities.hpp).
        // gl_event_last: moved to net::LosState::gl_event_last (network/state/Entities.hpp).
        // gl_seq: moved to NetworkServiceCounters (decision D6).
        // gl_hold_amb: moved to net::ArcState::gl_hold_amb (network/state/Entities.hpp).
        // gl_hold_iono_blk: moved to net::LosState::gl_hold_iono_blk (network/state/Entities.hpp).
        // gl_sta_state: moved to net::StationState::gross_los (network/state/Entities.hpp).
        // gl_emit_withhold: moved to EpochEvents::Gross (network/driver/Epoch.hpp).
        // gl_emit_disc: moved to EpochEvents::Gross (network/driver/Epoch.hpp).
        // gl_act_declare: moved to EpochEvents::Gross (network/driver/Epoch.hpp).
        // gl_act_release: moved to EpochEvents::Gross (network/driver/Epoch.hpp).
        // gl_act_station: moved to EpochEvents::Gross (network/driver/Epoch.hpp).
        // gl_act_cold_refloat: moved to EpochEvents::Gross (network/driver/Epoch.hpp).
        // rx_clk_carry: moved to net::StationSignalState::rx_clk (network/state/Entities.hpp).
        // rx_clk_rate: moved to net::StationSignalState::rx_clk_rate (network/state/Entities.hpp).
        // iono_cs: moved to net::LosState::iono_cs (network/state/Entities.hpp).
        // rpb_carry: moved to net::StationSignalState::rpb (network/state/Entities.hpp).
        // rx_cb_carry: moved to net::StationSignalState::rx_cb (network/state/Entities.hpp).
        // pc_carry: moved to net::LosState::pc (network/state/Entities.hpp).
        // pc_trace_rows: moved to EpochEvents::Harvest (network/driver/Epoch.hpp).
        // amb_if_rows: moved to EpochEvents::Harvest (network/driver/Epoch.hpp).
        // rxcb_lin_restore: moved to net::StationState::rxcb_lin_restore (network/state/Entities.hpp).
        /// Per-station out-slots for the two RS-9 override tallies, sized once by
        /// @c networkApplyRxCbLinRestore and never resized: each station's
        /// @c StateInitializer holds a raw pointer to its own element, so the counting is
        /// race-free inside the OpenMP parallel produce phase (distinct objects, no
        /// atomics).  {applied, uncovered}; summed at the run-end readout.
        std::vector<std::pair<long long, long long>> rxcb_lin_sta_counts;
        // URA quality channel carried state, B1 (inert unless network.emit_ura).
        double                       ura_sigma0_ewma = 1.0;   ///< network-wide σ̂0 EWMA scaling the emitted URA (one-epoch lag).
        // ura_refloat_horizon: moved to net::SatelliteState::ura_refloat_horizon (network/state/Entities.hpp).
        // arc_spike_streak: moved to net::LosState::arc_spike_streak (network/state/Entities.hpp).
        // ura_retro_sats: moved to net::SatelliteState::ura_retro_sats (network/state/Entities.hpp).
        // retro_active: moved to net::SatelliteState::retro_active (network/state/Entities.hpp).
        // Satellites whose reference-path arc was reset by IF-TD slip QC this epoch,
        // queued for a per-satellite discontinuity mark at the sat's next actual emit
        // (Edge-1 piece 2, network.mark_refsat_reset_disc; off by default, and then this
        // set is never written, so the output is byte-identical).  Drained by
        // NetworkSolPrep through the same mark-and-serialize path as ura_retro_sats,
        // including the RT0-5 "mark == land" rule (a sat absent from this epoch's emission
        // stays pending), so the R record always describes bumps that reached the product
        // and replay re-marks exactly the landed sats on exactly those epochs.  Marking is
        // needed although the emitter's jump test catches most ref-path slips: SPEC §25.3
        // found excursion onsets enriched in refsat resets with no PB value step, so the
        // onset-relevant subset re-references with no value step and is invisible to the
        // jump test.
        std::set<std::string>        refsat_mark_sats;
        // Datum-station slip re-reference queues (network.cycle_slip.datum_station_detect;
        // off by default, and then never written, so the output is byte-identical).  A
        // slip detected at the gauge-pinned datum receiver queues the satellite in both
        // sets:
        //   * pb set: consumed (and cleared) at this epoch's stash as a hard skip-stash of
        //     the sat's shared PhaseBias carry, so the next kept epoch re-seeds the emitted
        //     bias at the new pivot level (reset over repair; the pivot's pin re-anchors it
        //     in one solve).
        //   * amb set: consumed at the first drain after that skip-stash (per-sat guard: a
        //     sat still pending in the pb set is deferred, so a withheld epoch cannot
        //     decouple the two edges) as a network-wide ambiguity reseed, so every
        //     station's integer re-references in the same solve in which the bias jumps.
        // Invariant #7: both edges are evented (datum_slip at detection, datum_reref at
        // application) with run-end counters.  Not serialized into the snapshot: the same
        // transient class as refsat_mark_sats; a warm resume across a pending re-reference
        // drops it, and the slip then surfaces to post-fit RAIM instead.
        // datum_reref_pb_sats: moved to net::SatelliteState::datum_reref_pb (network/state/Entities.hpp).
        // datum_reref_amb_sats: moved to net::SatelliteState::datum_reref_amb (network/state/Entities.hpp).
        // relevel: moved to EpochEvents::Slip (network/driver/Epoch.hpp).
        /// The producer AR search's suspect-corner ledger (
        /// network/ar/ArSuspect.hpp): per (station, satellite), the stamp of each slip
        /// cause's last event (the loop epoch of the first search that can act on it), and
        /// the corners awaiting their release back into the search.  Written in PASS A (MW
        /// unknown drops, GF jump flags), at SB_NETAR (the free-epoch latch and the carry
        /// stamps, the search's masking set) and at SB_RAIM (T4 / T3, stamped for the next
        /// epoch), read by the search.  Empty and untouched unless
        /// knobs.ar_suspect_epochs > 0.  Not snapshotted (a resumed leg starts with every
        /// corner clean).
        ArSuspectLedger              ar_suspect;
        // ar_basis: moved to net::ConstellationState::ar_basis (network/state/Entities.hpp).
        /// The ledger of fixed cycles (see
        /// network/ar/ArLattice.hpp): the lattice of fixed cycles, the streaks of fixes
        /// waiting to enter it and its last step, stepped once per search epoch by the
        /// fixed-cycle seam at SB_SOLPREP.  Empty and untouched unless
        /// network.ambiguity_resolution.trace_lattice or a fixed-cycle fold with source
        /// lattice.  Not snapshotted and not fingerprinted (measure-only): a resumed leg starts
        /// with an empty ledger.
        ArLatticeState               ar_lattice;
        /// The fixed-cycle fold's run state (network/ar/FixedCycles.hpp):
        /// the units folded at the last step, for the fold's edges.  Empty and untouched unless
        /// the fold runs with source lattice.  Not snapshotted.
        FixedCycleState              fixed_cycles;
        // relevel_pend: moved to net::LosState::relevel_pend (network/state/Entities.hpp).
        // sid_reentry_outcome: moved to EpochEvents::Slip (network/driver/Epoch.hpp).
        /// The anchor election's own datum history per (satellite, signal) family:
        /// what each coverage-gate drop edge left behind (the incumbent,
        /// the epoch, the per-station admitted generations) and the last epoch the family
        /// held an incumbent.  The election itself keeps neither, which is why a returning
        /// family otherwise reads as a first sighting.  Not snapshotted and not
        /// fingerprinted: a resumed run has no drop memo, so its first post-resume election
        /// is a real first sighting (the same exemption the roster and the ledger take).
        /// Untouched unless network.ledger_screens.
        AnchorReentryState           sid_anchor_reentry;
        // sid_carry_prune_deaths: moved to EpochEvents::Harvest (network/driver/Epoch.hpp).
        // Startup quality-ramp state (RTB-7(b/c)).  The degraded URA floor
        // (nc.warmup_ura_floor_m) applies until ramp_lifted; the lift rule is
        // networkWarmupRampLifted (emitted-epoch horizon and σ̂0-EWMA update count, the
        // count waived when the EWMA is structurally unavailable).  All inert (never read)
        // while the floor is 0, so the output is byte-identical.  On an RTB-7(a) warm
        // resume these restore from the snapshot: a warm-resumed producer is not degraded
        // and reports grade 0 ("warm").
        size_t ura_ewma_updates     = 0;     ///< σ̂0 EWMA updates received (post-fit RAIM tier).
        bool   ramp_lifted          = false; ///< Warmup URA floor lifted (product grade reached).
        double first_emit_content_s = -1.0;  ///< Content time of the first emit (grade anchor).
        double grade_metric_s       = -1.0;  ///< cold-start-to-product-grade time (s), RTB-7(c); −1 before lift / ramp off; 0 = warm resume.

        // --- Invariant-carry graph identities (decomposition INC-4: plain-data tier-1
        // entities) ---
        // carry_sat_vertex: moved to net::SatelliteState::carry_vertex (network/state/Entities.hpp).
        // carry_group_id: moved to net::ChannelState::group_id (network/state/Entities.hpp).
        // carry_glonass_warned: moved to NetworkServiceCounters (decision D6).
        double carry_prev_tow = std::numeric_limits<double>::quiet_NaN();
        // carry_epochs, carry_born_total, carry_marg_total, carry_merges_total, carry_splits_total,
        // carry_swaps_total, carry_zdim_last, carry_slipkills_total, carry_dropped_total: moved to
        // NetworkServiceCounters (decision D6).
        // Arcs the innovation gate flagged in the previous epoch, to be treated as slipped
        // this epoch (the estimation-side fine-slip re-tap runs a post-solve detection, so
        // it feeds the next epoch's slip funnel).  Persists across epochs (the per-epoch
        // carry_slipped_ambs is rebuilt each epoch).  Empty unless carry_innov_gate.
        std::set<std::pair<int, std::string>> carry_innov_pending;
        // carry_innov_flags_total, carry_innov_retaps_total, carry_innov_map_miss: moved to
        // NetworkServiceCounters (decision D6).
        // The subset of carry_innov_pending whose arc the raw observables measured as
        // continuous at flag time (D5 v4, network.carry_invariant_innov_retap_only).
        // Same tri-state as the v3 corroboration gate: stepped and unmeasurable arcs are
        // absent on purpose, so
        // they keep the refloat (standing invariant #1).  Drained one epoch later into
        // @ref carry_innov_flat_now, in lockstep with carry_innov_pending.
        std::set<std::pair<int, std::string>> carry_innov_pending_flat;
        // The drained view the DD-prior build reads this epoch: a member of this set that
        // is not also in the epoch's raw funnel subset keeps its carried DD instead of
        // being refloated.  Rebuilt (cleared and filled) at the head of every epoch.
        // carry_innov_flat_now: moved to EpochEvents::Slip (network/driver/Epoch.hpp).
        // carry_innov_kill_suppressed: moved to NetworkServiceCounters (decision D6).

        // --- Per-epoch carry churn (SPEC_day129_frame_guards P1-1) ---
        // The invariant-carry counters above are run-cumulative: they mirror
        // carry_driver.counters(), which survive the mid-run carry reset() that the
        // install seam's exception handler can fire (reset() clears the carry, not the run
        // instrument; only restoreCarry, which installs a foreign carry, zeroes the
        // tallies).  Without per-epoch deltas the churn is therefore visible only in the
        // run-end log line.  That survival matters here: these mirrors are assigned from
        // the cumulative totals and differenced against the previous epoch's snapshot, so
        // a counter that restarted at zero mid-run would drive the per-epoch deltas below
        // negative.  The per-epoch series shows whether the re-tap count spikes at a
        // collapse epoch.  NetworkInstallCarry differences the cumulative totals against
        // the previous epoch's snapshot; the emit site copies the deltas into the
        // epoch-status row.  Trace only: nothing reads them back.
        long long carry_prev_born = 0, carry_prev_slipkills = 0,
                  carry_prev_innov_flags = 0, carry_prev_innov_retaps = 0;
        // ep_chord_births: moved to EpochEvents::Install (network/driver/Epoch.hpp).
        // ep_slip_refloats: moved to EpochEvents::Install (network/driver/Epoch.hpp).
        // ep_d5_flags: moved to EpochEvents::Install (network/driver/Epoch.hpp).
        // ep_d5_retaps: moved to EpochEvents::Install (network/driver/Epoch.hpp).
        // Per-epoch observability of the within-epoch RAIM tier (R0), parked here between
        // the solve glue (writer) and the epoch-status emit (reader).  It puts a null
        // sample of σ̂₀_first on disk (a trip event's detail span carries it only on
        // trips), which a corrected quantile needs to be fitted against (QUEUE item 4).
        // It also makes the within-epoch satellite refloats visible in the epoch-status
        // row, where es.n_refloat_sats counts only the clock-couple set, which is empty
        // unless clock_monitor_couple_raim.  Sentinels: < 0 = the tier did not assess this
        // epoch, and the columns stay blank ("unavailable is not 0").
        // ep_sigma0_first: moved to EpochEvents::Solve (network/driver/Epoch.hpp).
        // ep_we_refloat_sats: moved to EpochEvents::Solve (network/driver/Epoch.hpp).
        // ep_we_clk_move_max: moved to EpochEvents::Solve (network/driver/Epoch.hpp).
        // ep_we_clk_move_sat: moved to EpochEvents::Solve (network/driver/Epoch.hpp).

        // --- Withhold-era continuity (A4).  Advanced only at epochs that reach the A4
        // decision; continuity uses the census freshness idiom
        // (`wh_last_loop_ep == n_loop_epochs − 1`), so any intervening bypass epoch
        // (datum_failed, gate_withheld, solve_failed, PASS-A-head skip, decimation tick)
        // breaks the era (strictly consecutive, C5/C7; an interrupted era is exited with
        // detail "interrupted" at the next decide).  Distinct, on purpose, from the flight
        // recorder's ceiling-breach era (`networkRhsTripEdge` "sigma0"), which does not
        // depend on the carve-out: two co-located eras with different edges.
        int       wh_era_len      = 0;  ///< consecutive A4-withheld decides (0 = no era)
        long long wh_last_loop_ep = -1; ///< n_loop_epochs at the last withheld decide

        // staf_ewma: moved to net::StationState::staf_ewma (network/state/Entities.hpp).
        // staf_updates: moved to net::StationState::staf_updates (network/state/Entities.hpp).
        // staf_departed: moved to net::StationState::staf_departed (network/state/Entities.hpp).
        double                   staf_prev_tow = -1.0; ///< dt source for the EWMA alpha

        // --- Cross-epoch state of the PD-safe DD fold (INC-0).
        //
        // There is no `ddfold_ep_tow` stamp here, on purpose, unlike the redundancy and
        // admission censuses.  Those seams run pre-assembly, so their numbers must be
        // parked until the epoch-status writer runs; the DD fold's numbers are held by the
        // kernel (@ref NetworkEstimator::ddFoldStats, reset once per epoch by beginEpoch),
        // and both writers read them from it directly.  A stamp here would be wrong in two
        // ways: the install seam that would write it runs after networkSolveGlue and only
        // on an accepted epoch, so every withheld row would blank the columns it was meant
        // to fill; and a stamp written on a chordless epoch reports the default-constructed
        // min_pivot_rel of 1.0, a perfectly conditioned factorization that was never
        // computed and cannot be told apart from a real single-chord block.  Only the
        // cross-epoch ledger below needs to live here.
        /// The blocks currently in the degenerate state, keyed "<group>:<chord0.u>-<chord0.v>".
        /// Diffed epoch to epoch so the warning fires on the transition into the degenerate
        /// state and not once per epoch: logging only transitions keeps a long outage to two
        /// lines instead of one per epoch.
        std::set<std::string> ddfold_degen_prev;

        /// σ̂₀ clamp visibility for D5 v2: the @c NetworkInvariantCarryDriver::InflationState
        /// tag reported at the previous install, so a state change is logged once rather
        /// than every epoch (as for @ref ddfold_degen_prev).  Empty = nothing seen yet, so
        /// the first install always announces the state it starts in.
        std::string d5_calib_state_prev;

        // --- Measure-only post-fit σ̂0 (SPEC_day129_frame_guards P1-4) ---
        // Filled by networkPostfitSigma0 (filter/NetworkFrameDiag) under
        // network.trace_postfit_sigma0; ep_pf_rows == 0 means "not computed this epoch",
        // and the trace columns stay blank.  Nothing reads these back: the A4 withhold
        // ceiling still uses the within-epoch RAIM's sigma0_adapt.
        double    ep_sigma0_pf       = 0.0;
        double    ep_sigma0_pf_code  = 0.0;
        double    ep_sigma0_pf_phase = 0.0;
        long long ep_pf_rows         = 0;
        long long ep_pf_code_rows    = 0;   ///< …of which code rows
        long long ep_pf_phase_rows   = 0;   ///< …of which phase rows (residual ≡ 0 under free ambiguities)
        // pf_epochs_total, pf_empty_logged: moved to NetworkServiceCounters (decision
        // D6).
        // pf_rows_written, pf_rows_code, pf_rows_phase, pf_rows_no_elev, pf_rows_unattributed,
        // pf_rows_no_signal: moved to NetworkServiceCounters (decision D6).
        // sb_rows_written, sb_epochs_written, sb_stations_skipped, sb_decomp_failed, sb_last_emitted:
        // moved to NetworkServiceCounters (decision D6).

        // --- PREFIT census, per-epoch scalars ---
        // Filled by networkPrefitCensus under network.trace_prefit_census, and copied into
        // the epoch-status row by NetworkStashStatus.  ep_pc_rows == 0 means "not computed
        // this epoch", and the census columns stay blank (the "unavailable is not 0"
        // convention of the P1-2/P1-4 columns).  Indexed by the data-class order of
        // NetRowClass (obs_code, obs_phase, obs_other, iono_sh_row), which is also the
        // trace column order.  Nothing reads them back: the census is a tripwire, not an
        // input.
        // ep_pc_max_s: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        // ep_pc_p99_s: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        // ep_pc_n_gt_10: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        // ep_pc_n_gt_1000: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        // ep_pc_rows: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        /// H-rider (edge-queue §1.6 item 3a): this epoch's pooled robust prefit σ̂₀ for the
        /// H heartbeat (networkPrefitSigma0, computed pre-solve at the top of
        /// networkSolveGlue).  0.0 = unavailable (empty pool, or the knob is off), the
        /// sentinel the H grammar already defines.
        double    ep_h_sigma0_census = 0.0;
        // h_sig0_census_nonzero: moved to NetworkServiceCounters (decision D6).

        // --- Reduced-RHS class attribution (filter/NetworkRhsAttrib) ---
        int rhs_attrib_done = 0;   ///< armed target epochs already dumped (caps network.rhs_attrib_n_ep)
        /// The station-local labels of network.rhs_attrib_columns (and the no-x_s condition of
        /// a trip dump) already named in the log, so each is named once per run.
        std::set<std::string> rhs_attrib_loc_warned;

        // sh_spectrum_epochs, sh_spectrum_fail_logged: moved to NetworkServiceCounters (
        // decision D6).

        // --- CB-carry event response (SPEC_day129_frame_guards Phase 2.1) ---
        // Not snapshotted (three-place-rule exemption): every member is either a one-epoch
        // handoff (cb_excursion_sats is consumed by the same epoch's stash) or run
        // telemetry.  A cb_excursion_pending_bump that crosses a resume boundary unbumped
        // is the conservative failure: a resumed run re-detects the excursion if it is
        // still present, and a missed bump costs a consumer nothing it did not already
        // live without (the counter is silent by default).  cb_exc_prev_clk rebuilds from
        // the first post-resume epoch, which correctly suppresses a spurious trip on the
        // resume seam itself (no baseline, so no delta and no trip).
        // cb_exc_prev_clk: moved to net::SatelliteState::cb_exc_prev_clk (network/state/Entities.hpp).
        // cb_excursion_sats: moved to EpochEvents::Qc (network/driver/Epoch.hpp).
        // cb_excursion_pending_bump: moved to net::SatelliteState::disc_pending (net::Disc::CbExcursion) (network/state/Entities.hpp).
        // cb_exc_trips, cb_exc_reinits, cb_exc_bumps, cb_exc_nonpivot_would, cb_exc_max_median,
        // cb_exc_max_median_tow: moved to NetworkServiceCounters (decision D6).

        // sh_dev_carry: moved to net::LosState::sh_dev (network/state/Entities.hpp).
        // shdev_warm_total, shdev_cold_total, shdev_slip_resets_total, shdev_nodata_total,
        // shdev_demean_epochs_total, shdev_demean_sta_total, shdev_demean_abs_g_sum, shdev_d5_keeps_total:
        // moved to NetworkServiceCounters (decision D6).

        // --- SH-field warning edge state (T1a) ---
        // Persistent state for the a₀₀ physical-band divergence warning (T1a).  It starts
        // "in band", so the first out-of-band epoch is always reported (transition edge);
        // later reports are throttled.
        bool sh_a00_was_in_band = true;

        // --- SD EWL Route-B integer decision (increment 2a, filter/NetworkSdEwlAr) ---
        // None of the three is snapshotted, on purpose (the exemption record of the RTB-7
        // three-place rule):
        //  * sd_ewl_fix is re-derived from scratch every epoch: it is this epoch's
        //    publication to the PASS-B produce loop, not cross-epoch state;
        //  * sd_ewl_r0 is a datum choice that can be re-elected from any epoch's data.
        //    Keeping it sticky only avoids a needless re-frame; a fresh election after a
        //    warm resume costs one counted sd_ref_reelected.  Adding it to the
        //    fingerprint/restore/write chain would change the snapshot format for every
        //    run, including runs with this feature off, and invalidate every stored
        //    snapshot, for no estimation benefit;
        //  * sd_ewl_prev exists only to veto a flipped integer.  Empty after a resume, it
        //    makes the first resumed epoch charge no flip and fold normally, which is what
        //    keeps the S2 single-epoch virtual fold non-inert.  Snapshotting it would make
        //    a paired A/B sensitive to the build format.
        // Storing a decision here and reusing it instead of re-deriving it would be a
        // zero-q hold in disguise (increment 2c) and would pull 2c's slip-response rules
        // forward; see the invariant #1 discussion in SdEwlAr.hpp.
        // sd_ewl_fix: moved to net::StationState::sd_ewl_fix (network/state/Entities.hpp).
        // sd_ewl_r0: moved to net::ConstellationState::sd_ewl_r0 (network/state/Entities.hpp).
        // sd_ewl_prev: moved to net::LosState::sd_ewl_prev (network/state/Entities.hpp).

        // --- State-snapshot channel, RTB-7(a) (path resolved at the run-TU site;
        // fingerprint, restore and write live in SnapshotFingerprint.cpp,
        // WarmResume.cpp and StateSnapshotWrite.cpp) ---
        std::filesystem::path snap_path;
        double   last_snapshot_write_s = -1.0;   // product time of the last snapshot write
        uint64_t live_fingerprint      = 0;
        uint64_t live_fingerprint_ctu_off = 0;   ///< the live fingerprint without the clock-level switch-on tokens (0 when none is armed)
        bool     ctu_switch_on_resume = false;   ///< this run resumed from a snapshot written without the clock-level knobs
        // snap_writes, snap_carry_bytes_last, snap_carry_diagonal: moved to NetworkServiceCounters (decision D6).
        // snap_cpl_dict_last, snap_cpl_rows_last, snap_cpl_zrows_last, snap_cpl_terms_last,
        // snap_cpl_rows_dropped, snap_datum_rows_last, snap_emit_rows_last, snap_station_rows_last,
        // snap_canon_rows_last: moved to NetworkServiceCounters (decision D6).
        // resume_canon_rows: moved to net::StationState::resume_canon_rows (network/state/Entities.hpp).
        // snap_rxcb_lin_rows_last: moved to NetworkServiceCounters (decision D6).
        // resume_cpl_census_pending: moved to NetworkCarryState::resume.cpl_census (ResumeHandoff).
        std::vector<double> resume_cpl_census_rel;    // per-row max|M − M_d| / max|M_d| at the first harvest (§18.12)
        std::vector<double> resume_cpl_census_rhs;    // per-row |Δ Σ M·ŝ_prev| (m) at the first harvest (§18.12)

        // --- redundancy / testability census ---
        // Snapshot: exempt.  Every field here is diagnostic state of a measure-only
        // instrument.  Restoring it would make a resumed leg's stride phase, event
        // rate-limit memory and unobservable-column edges depend on the writer's history,
        // i.e. it would make a diagnostic fingerprint-relevant, and keeping it out is what
        // keeps the census A/B-legal on a shared snapshot.  A resumed run re-sweeps on its
        // own phase and re-events each still-unobservable column once, which is the right
        // behaviour for a fresh observer.
        // redun_epochs: moved to NetworkServiceCounters (decision D6).
        std::set<std::size_t> redun_escalate_pending;   ///< stations the free tier flagged this epoch, swept at the next one (one-epoch latch).
        // redun_event_last: moved to net::LosState::events (LosEventState::redun_event_last) (network/state/Entities.hpp).
        // redun_unobs_active: moved to net::StationState::redun_unobs_active (network/state/Entities.hpp).
        // Per-epoch L1 scalars.  redun_ep_tow is the stamp the epoch-status writer
        // compares against: a non-sweep epoch leaves the columns blank instead of
        // repeating a stale sweep's numbers (the "unavailable is not 0" convention).
        // redun_ep_tow: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        // ep_redun_rows: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        // ep_uncheckable_rows: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        // ep_uncheckable_max_s: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        // ep_unobservable_local: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        // redun_cov_warned, redun_closure_warned: moved to NetworkServiceCounters (
        // decision D6).

        // --- Baarda MDB ---
        // Snapshot: exempt; observer state of a measure-only, forward-only instrument, the
        // same class as the two censuses above.  Snapshotting an event-holdoff clock would
        // make a diagnostic fingerprint-relevant, and keeping it out is what keeps the
        // instrument A/B-legal on a shared snapshot.
        //
        // @c mdb is stamped, not cleared by the driver: the RAIM pool feeds it in SB_RAIM
        // and the census sweep in SB_FRAMEDIAG, so a feed whose TOW differs from the stamp
        // resets it first.  That is what keeps an epoch which fed rows and never reached
        // the flush (a withhold, a failed solve) from leaking into the next epoch's
        // aggregates.  Empty and unread unless ctx.knobs.mdb_on, so an off run allocates
        // nothing.
        NetworkMdbEpoch mdb;                   ///< this epoch's per-(constellation x class) accumulation.
        // mdb_seq: moved to NetworkServiceCounters (decision D6).
        // mdb_event_last: moved to net::LosState::events (LosEventState::mdb_event_last) (network/state/Entities.hpp).
        // mdb_join_warned: moved to NetworkServiceCounters (decision D6).

        // --- Carry LS-VCE monitor, the hourly trace flush (M2) ---
        // Snapshot: exempt; observer state.  The pair only schedules the hourly flush of
        // network_carry_vce.csv by content hour, and the monitor it flushes (NetworkCarryVce,
        // run.engines.carry_vce; a run-TU local formerly) is not snapshotted
        // either, so a warm-resumed leg re-anchors its
        // content hour at its first monitored epoch.  They were driver locals until the run took ownership,
        // with the same initial values and the same resume behaviour.  Written and
        // read only by networkShHealth, under ctx.knobs.carry_vce.
        int    last_vce_hour = -1;   ///< content-hour bucket of the last LS-VCE monitor flush (-1 = none yet).
        double first_vce_s   = -1.0; ///< content time of the first monitored epoch, the hour anchor (s; -1 = not yet anchored).

        // --- Carry variance factor (M2 v2) ---
        // Snapshot: exempt, and the exemption is enforced by refusal rather than free.  α²
        // is real cross-epoch state that changes the solve, so leaving it out of the
        // snapshot is not the harmless observer exemption the censuses get.  Spec §8
        // declares it: α² restarting at 1 mid-chain would move the solve across the seam,
        // so v1 is fresh-convergence-only, and networkTryWarmResume throws when a resume
        // meets an armed apply (the fingerprint alone cannot catch it: both legs of an
        // apply-on chain fingerprint identically).  Relaxing it needs a v2 with two parts,
        // NetworkCarryVce::seedRobustFloor for the monitor and an α² field in the
        // snapshot; until both exist, the exemption stays enforced by the refusal.
        //
        // Inert (unity factor, no allocation beyond the fixed table) unless
        // ctx.knobs.vce_apply_on, so an off run is byte-identical.
        NetworkCarryVceApply vce_apply;         ///< per-(family x constellation) α², its clamps, freezes and tallies.
        std::vector<NetworkCarryVceApply::Edge> vce_apply_edges;  ///< the last step's clamp/freeze edges, drained by the emitting seam.
        int  vce_apply_trace_hour = -1;         ///< content-hour bucket of the last apply-trace flush (durability).

        // --- prior-residual census ---
        // Snapshot: exempt; observer state of a measure-only, forward-only instrument.
        // Restoring an event-holdoff clock would make a diagnostic fingerprint-relevant,
        // and keeping it out is what keeps the census A/B-legal on a shared snapshot.  §7 of
        // the spec also declares the σ̂₀ accumulator un-snapshotted: a resumed run restarts
        // it empty and reports raw formal values for the first window, and the output says
        // so per row (NetworkCarryVce::seedRobustFloor is the v2 path if that ever
        // matters).  Empty and unread unless ctx.knobs.prires_on, so an off run allocates
        // nothing.
        // prires_event_last: moved to net::LosState::events (LosEventState::prires_event_last) (network/state/Entities.hpp).
        // prires_ep_tow: moved to EpochEvents::Status (network/driver/Epoch.hpp).
        // ep_prires_rows: moved to EpochEvents::Status (network/driver/Epoch.hpp).
        // ep_prires_resp: moved to EpochEvents::Status (network/driver/Epoch.hpp).
        // ep_prires_frame: moved to EpochEvents::Status (network/driver/Epoch.hpp).
        // ep_prires_reported: moved to EpochEvents::Status (network/driver/Epoch.hpp).
        // ep_prires_max_t: moved to EpochEvents::Status (network/driver/Epoch.hpp).
        // prires_trunc_warned: moved to NetworkServiceCounters (decision D6).
        int       prires_last_flush_hour = -1; ///< content-hour bucket of the last ladder flush (durability: only health_trace flushes on its own).
        // ep_s0g_formed: moved to EpochEvents::Status (network/driver/Epoch.hpp).
        // ep_s0g_sigma0: moved to EpochEvents::Status (network/driver/Epoch.hpp).
        // ep_s0g_tension_def: moved to EpochEvents::Status (network/driver/Epoch.hpp).
        // ep_s0g_tension: moved to EpochEvents::Status (network/driver/Epoch.hpp).
        // ep_s0g_flags: moved to EpochEvents::Status (network/driver/Epoch.hpp).

        // --- BlockFormal LOS block census ---
        // Snapshot: exempt; observer state of a measure-only, forward-only instrument, the
        // same class as the three censuses above.  Restoring the σ̂₀ accumulator or the
        // event-holdoff clock would make a diagnostic fingerprint-relevant and cost the
        // census its A/B legality on a shared snapshot; a resumed leg restarts the
        // calibration empty and reports raw formal values for its first window, and every
        // trace row and log line says so.
        //
        // The accumulator lives here rather than as a run-TU local (the prior-residual
        // census's shape) because its feed site is inside the redundancy-census seam,
        // which took ctx but not a run-TU reference formerly (the seam
        // takes the run now, so the reason no longer holds; moving the accumulator into
        // the run's engines is a later step).  Default-constructed with no
        // families, so enabled() is false and every entry point is a single bool test; an
        // unarmed run allocates nothing beyond the empty vectors.
        NetworkBlockBaarda bb{{}, 0.0};        ///< the run accumulator; re-seated from config once at run start.
        // bb_hits: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        // bb_event_last: moved to net::LosState::bb_event_last (network/state/Entities.hpp).
        // This census adds no network_epoch_status.csv columns, unlike the three censuses
        // above.  Its own network_block_baarda.csv already carries n_blocks / n_reported /
        // max_t per (family x label) per epoch, more than three pooled scalars could say,
        // so an epoch-status column would duplicate a number nobody would read there.
        // Per-epoch state that nothing consumes is not carried.

        // --- Station congruence screen ---
        // Snapshot: exempt; the same class and reasoning as the block census above,
        // observer state of a measure-only, forward-only instrument.  A resumed leg
        // restarts the σ̂₀ calibration empty and reports raw formal values for its first
        // window, and every trace row and log line says so.
        //
        // The exemption has a cost here: the per-station δ̂ / MDB samples are not restored
        // either, so a resumed leg's run median covers only that leg, not the lineage.
        // SPEC §7.2 grades a run median against a perturbed arm, so both arms must run as
        // one continuous lineage; otherwise the two medians cover different windows.
        //
        // Lives here rather than as a run-TU local for the block census's reason: its feed
        // site is inside the redundancy-census seam, which took ctx but not a run-TU
        // reference for now (a later step moves it with the block
        // census).  Default-constructed unarmed, so enabled() is false and every entry
        // point is a single bool test.
        NetworkCongruence cg{false};           ///< the run accumulator; re-seated from config once at run start.
        // cg_hits: moved to EpochEvents::Census (network/driver/Epoch.hpp).
        // cg_event_last: moved to net::StationState::cg_event_last (network/state/Entities.hpp).

        // s0g_approx_warned, s0g_nodata_warned: moved to NetworkServiceCounters (
        // decision D6).

        // --- prefit census / fuse / birth check / topology / flight recorder ---
        // Snapshot: exempt; every member is observer state of a forward-only instrument
        // (except the fuse, which reads none of it across epochs).  Restoring a presence
        // map, a topology baseline or an event-holdoff clock would make a diagnostic
        // fingerprint-relevant, and keeping them out is what keeps the census A/B-legal on
        // a shared snapshot.  The resumed behaviour is the right one for a fresh observer:
        // the first walked epoch seeds (no births, no topology deltas, since a first
        // sighting is not a change) and every later epoch is judged against it.
        //
        // Empty and unread unless ctx.knobs.pc_walk_on (or, for the event clock,
        // pc_fuse_sigma > 0), so an off run allocates nothing.
        // prefit_event_last: moved to net::LosState::events (LosEventState::prefit_event_last) (network/state/Entities.hpp).
        // prefit_present_prev: moved to net::LosState::prefit_present_prev (network/state/Entities.hpp).
        bool prefit_walk_seeded = false;   ///< the presence map holds a real epoch; before that, every LOS would read as born (INC-3).
        // prefit_empty_logged: moved to NetworkServiceCounters (decision D6).
        // prefit_code_flag: moved to net::LosState::prefit_code_flag (network/state/Entities.hpp).
        /// The `n_loop_epochs` value at which @ref prefit_code_flag was last written (-1 =
        /// never).  The vet honours flags from the immediately preceding loop epoch only;
        /// anything older is stale and is counted as an absent consultation rather than
        /// believed.  The same staleness discipline as the `adm_ep_tow` / `redun_ep_tow`
        /// column stamps.
        long long prefit_code_flag_ep = -1;
        // topo_prev_tie: moved to net::StationState::topo_prev_tie (network/state/Entities.hpp).
        // topo_prev_kind: moved to net::LosState::topo_prev_kind (network/state/Entities.hpp).
        std::set<std::string> topo_prev_cols;  ///< last epoch's shared-column label set, for registry deltas (INC-4).
        bool topo_seeded = false;              ///< the baselines above hold a real epoch (INC-4).

        // --- Flight recorder (INC-5) ---
        // rhs_trip_eras is the era membership, not a fire log: a kind is inserted on its
        // rising edge and erased when its condition clears, so one era trips exactly once
        // however long it lasts.  rhs_trip_pending is the one-epoch handoff from a trigger
        // site to the SB_FRAMEDIAG consumer (the A4 withhold path consumes its own, since
        // that epoch never reaches SB_FRAMEDIAG).
        std::set<std::string> rhs_trip_eras;   ///< QC eras currently active (latched, one trip each).
        // rhs_trip_pending: moved to EpochEvents::Qc (network/driver/Epoch.hpp).
        std::unique_ptr<NetworkAttribFrame> rhs_prev_frame;  ///< the retained previous epoch (null unless retention is on and sized in).
        // rhs_prev_oversize_logged: moved to NetworkServiceCounters (decision D6).

        // --- datum graph census ---
        // Not restored from a state snapshot, on purpose and for the same reason as the
        // prefit census's observer state: this is forward-only observer state of a
        // measure-only instrument, and persisting it would make a diagnostic
        // fingerprint-relevant; keeping it out is what keeps the instrument A/B-legal on a
        // shared snapshot and legal on a warm resume.  Empty and unread unless
        // ctx.knobs.dg_on, so an off run allocates nothing.
        //
        // `dg_spec` is a one-epoch hand-off written by the solve glue and read by the
        // SB_FRAMEDIAG census.  It is cleared at the top of every epoch, so an epoch that
        // skips, withholds or throws before the gate can never hand the previous epoch's
        // spectrum to the next census pass; otherwise the census would reconcile this
        // epoch's topology against last epoch's eigenvalues without saying so.
        NetDatumGraphSpectral dg_spec;
        std::map<std::string, long long> dg_event_last;  ///< "kind|identity" -> processed-epoch index of its last DATUMGRAPH event (per-identity rate limit).
        /// `predicted only` identities already reported in the current era.  Cleared on a
        /// datum re-reference (the same boundary at which SBC re-seeds its continuity
        /// targets), so the KP-load-bearing roster is reported once per identity per era
        /// rather than once per run, which would make its stability across the day
        /// (spec §6.4) unreadable.
        std::set<std::string> dg_pred_only_era;

        // --- Canonical-pair admission ---
        // The per-(station, satellite) admission memory of
        // docs/refound/CPA_canonical_pair_admission_2026-09-20.md, parked here rather
        // than in the seam because the rule's whole content is a value that must survive
        // to the next epoch (the INC-12 rule: a seam owns no state).  Not snapshotted and
        // not in the warm-resume fingerprint, the same forward-only posture as the other
        // observer states on this path: a resumed leg re-earns each pair's first good
        // epoch.  Empty and untouched unless network.canonical_pair_admission.
        NetworkCanonicalPairState cpa;
    };

    /// @brief Run-scope counters of the Application drivers, a base of NetworkServiceCounters (network/state/RunContext.hpp).
    struct GeneralCounters {
        int    last_n_present      = 0;      // stations present in the most recent processed epoch
        int    last_n_clock_sats   = 0;      // registered clock sats in the most recent processed epoch (P4b)
        // The marker → antenna-reference-point roster census, copied out of
        // NetworkSetupResult once the context exists (it is a setup fact, not an epoch
        // counter; it is here only because the run-end readout and the run manifest both
        // read `counters`).  Populated in both states of network.station_marker_to_arp: with
        // the knob off the eccentricities are still read, so the run states the size of the
        // class (standing invariant #4).  See NetworkSetupResult for the per-field meanings.
        bool   marker_arp_armed       = false;   ///< network.station_marker_to_arp as the run read it
        int    n_marker_arp_ecc       = 0;
        int    n_marker_arp_reduced   = 0;
        int    n_marker_arp_over_1cm  = 0;
        int    n_marker_arp_no_record = 0;
        double marker_arp_max_h       = 0.0;
        std::string marker_arp_max_h_station;

        // The measurable cost of the deliberate CB-ADM granularity seam.  The phase co-gate
        // lives at state allocation and keys off the run-latched census-admitted set, while
        // the code column follows per-epoch >=2-station coverage; per-epoch phase-state
        // allocation churn was rejected, because it would thrash eq34, the carry and the
        // snapshot and would violate the anti-flap design.  The residue is a transient
        // orphan window: an admitted code key that drops below 2 present stations for an
        // epoch keeps its shared phase column while its code column is withheld.  This
        // counts those (key, epoch) pairs.  At 305-station density it measures zero (every
        // admitted satellite has >= 2 present stations); a sparse run where it is nonzero
        // and coincides with withholds is the data that would justify revisiting the seam.
        std::size_t n_cbadm_phase_orphan_key_epochs = 0;
    };

    /// @internal Service counters, health-window anchors and B8 flag-rate tallies,
    /// relocated from the run loop.
    ///
    /// The members are declared by owner: each base is an aggregate in the header of the
    /// subsystem that writes its members (in a light Counters header beside it where the
    /// owner header includes this file), so `ctx.counters.x` is spelled as it always was.
    /// A new run-scope counter goes to the counter struct of the subsystem that writes it;
    /// a new struct is one more base below.  Bases and the header declaring each:
    ///   SolveStationsCounters              network/solve/SolveStations.hpp
    ///   RunTracesCounters                  network/diag/RunTraces.hpp
    ///   ClockAnchorCounters                network/solve/ClockAnchor.hpp
    ///   ClockDatumCounters                 network/solve/ClockDatum.hpp
    ///   StateSnapshotCounters              network/state/StateSnapshotCounters.hpp
    ///   HandoverStransformCounters         network/solve/HandoverStransformCounters.hpp
    ///   BorderCounters                     network/state/BorderCounters.hpp
    ///   SdEwlArCounters                    network/ar/SdEwlAr.hpp
    ///   ChainDatumCounters                 network/solve/ChainDatum.hpp
    ///   AnchorHygieneCounters              network/solve/AnchorHygiene.hpp
    ///   SignalIntegrityCounters            network/assembly/SignalIntegrity.hpp
    ///   RaimCounters                       network/qc/Raim.hpp
    ///   GrossLosCounters                   network/qc/GrossLos.hpp
    ///   MeasurementAdmissionCounters       network/assembly/MeasurementAdmission.hpp
    ///   SolPrepCounters                    network/emit/SolPrep.hpp
    ///   GeneralCounters                    network/state/RunContext.hpp
    ///   RaimWithinEpochCounters            network/qc/RaimWithinEpoch.hpp
    ///   LevelingCarryCounters              network/carry/LevelingCarry.hpp
    ///   StationBuildCounters               network/assembly/StationBuild.hpp
    ///   AmbFloatCounters                   network/ar/AmbFloat.hpp
    ///   PhaseAnchorEpochCounters           network/solve/PhaseAnchorEpoch.hpp
    ///   AmbStateTraceCounters              network/diag/AmbStateTrace.hpp
    ///   SlantIonoCarryCounters             network/carry/SlantIonoCarry.hpp
    ///   RxCodeBiasCarryCounters            network/carry/RxCodeBiasCarry.hpp
    ///   WarmResumeCounters                 network/state/WarmResumeCounters.hpp
    ///   RxClockCarryCounters               network/carry/RxClockCarry.hpp
    ///   ZwdCarryCounters                   network/carry/ZwdCarry.hpp
    ///   PhaseColourCarryCounters           network/carry/PhaseColourCarry.hpp
    ///   EmitStoreCounters                  network/emit/EmitStore.hpp
    ///   PassASlipScanCounters              network/slip/PassASlipScanCounters.hpp
    ///   InstallCarryCounters               network/carry/InstallCarry.hpp
    ///   DdFracCounters                     network/diag/DdFrac.hpp
    ///   LevelAuthorityCounters             network/carry/LevelAuthority.hpp
    ///   PbHoldCounters                     network/emit/PbHold.hpp
    ///   PbHoldCloneCounters                network/emit/PbHoldClone.hpp
    ///   RankGateCounters                   network/solve/RankGate.hpp
    ///   BiasAdmissionCounters              network/assembly/BiasAdmission.hpp
    ///   CodeBiasDatumCounters              network/solve/CodeBiasDatum.hpp
    ///   SbasisCounters                     network/solve/Sbasis.hpp
    ///   CouplingCounters                   network/carry/Coupling.hpp
    ///   CouplingExactRunCounters           network/carry/CouplingExact.hpp
    ///   PivotFrameQCounters                network/carry/PivotFrameQ.hpp
    ///   ClockCloneRunCounters              network/carry/ClockClone.hpp
    ///   SharedHistoryRunCounters           network/carry/SharedHistory.hpp
    ///   CarryDispositionCounters           network/state/CarryDisposition.hpp
    ///   CarrySitesCounters                 network/state/CarrySites.hpp
    ///   ResponsesCounters                  network/assembly/Responses.hpp
    ///   ClockLevelCounters                 network/solve/ClockLevel.hpp
    ///   BrdcWireCounters                   network/emit/BrdcWire.hpp
    ///   PhaseVisibleDiagCounters           network/diag/PhaseVisibleDiag.hpp
    ///   SecondCodeScopeCounters            network/assembly/SecondCodeScope.hpp
    ///   SecondPhaseScopeCounters           network/assembly/SecondPhaseScope.hpp
    ///   ChannelAllCounters                 network/assembly/ChannelAll.hpp
    ///   MultiGnssCounters                  network/assembly/MultiGnss.hpp
    ///   RawObservablesCounters             network/slip/RawObservables.hpp
    ///   CommonBandScopeCounters            network/slip/CommonBandScope.hpp
    ///   SolveGlueCounters                  network/solve/SolveGlue.hpp
    ///   PhaseDatumCounters                 network/solve/PhaseDatum.hpp
    ///   EpochAdmissionCounters             network/driver/EpochAdmission.hpp
    ///   SbasisCompletionCounters           network/solve/SbasisCompletion.hpp
    ///   FrameDiagCounters                  network/diag/FrameDiag.hpp
    ///   WithinEpochCulpritsCounters        network/qc/WithinEpochCulprits.hpp
    ///   RedundancyCensusCounters           network/qc/RedundancyCensus.hpp
    ///   MdbCounters                        network/qc/Mdb.hpp
    ///   BlockBaardaCounters                network/qc/BlockBaarda.hpp
    ///   CongruenceCounters                 network/qc/Congruence.hpp
    ///   PriorResidualCounters              network/qc/PriorResidual.hpp
    ///   PrefitCensusCounters               network/qc/PrefitCensus.hpp
    ///   DatumGraphCounters                 network/solve/DatumGraph.hpp
    ///   InvariantCarryDriverCounters       network/carry/InvariantCarryDriver.hpp
    ///   ProducerArCounters                 network/ar/ProducerAr.hpp
    ///   ArSuspectRunCounters               network/ar/ArSuspect.hpp
    ///   ArCycleBasisRunCounters            network/ar/ArCycleBasis.hpp
    ///   ArLatticeRunCounters               network/ar/ArLattice.hpp
    ///   FixedCyclesCounters                network/ar/FixedCycles.hpp
    ///   CanonicalPairAdmissionCounters     network/assembly/CanonicalPairAdmission.hpp
    ///   SignalRosterCounters               network/assembly/SignalRoster.hpp
    ///   LedgerEmitCounters                 network/emit/LedgerEmit.hpp
    ///   DdPriorBuildCounters               network/carry/DdPriorBuild.hpp
    ///   StationBlockCounters               network/diag/StationBlock.hpp
    ///   ClockQcCounters                    network/qc/ClockQc.hpp
    ///   ShDevVtecCounters                  network/diag/ShDevVtec.hpp
    ///   RhsAttribCounters                  network/qc/RhsAttrib.hpp
    struct NetworkServiceCounters
        : SolveStationsCounters,
          RunTracesCounters,
          ClockAnchorCounters,
          ClockDatumCounters,
          StateSnapshotCounters,
          HandoverStransformCounters,
          BorderCounters,
          SdEwlArCounters,
          ChainDatumCounters,
          AnchorHygieneCounters,
          SignalIntegrityCounters,
          RaimCounters,
          GrossLosCounters,
          MeasurementAdmissionCounters,
          SolPrepCounters,
          GeneralCounters,
          RaimWithinEpochCounters,
          LevelingCarryCounters,
          StationBuildCounters,
          AmbFloatCounters,
          PhaseAnchorEpochCounters,
          AmbStateTraceCounters,
          SlantIonoCarryCounters,
          RxCodeBiasCarryCounters,
          WarmResumeCounters,
          RxClockCarryCounters,
          ZwdCarryCounters,
          PhaseColourCarryCounters,
          EmitStoreCounters,
          PassASlipScanCounters,
          InstallCarryCounters,
          DdFracCounters,
          LevelAuthorityCounters,
          PbHoldCounters,
          PbHoldCloneCounters,
          RankGateCounters,
          BiasAdmissionCounters,
          CodeBiasDatumCounters,
          SbasisCounters,
          CouplingCounters,
          CouplingExactRunCounters,
          PivotFrameQCounters,
          ClockCloneRunCounters,
          SharedHistoryRunCounters,
          CarryDispositionCounters,
          CarrySitesCounters,
          ResponsesCounters,
          ClockLevelCounters,
          BrdcWireCounters,
          PhaseVisibleDiagCounters,
          SecondCodeScopeCounters,
          SecondPhaseScopeCounters,
          ChannelAllCounters,
          MultiGnssCounters,
          RawObservablesCounters,
          CommonBandScopeCounters,
          SolveGlueCounters,
          PhaseDatumCounters,
          EpochAdmissionCounters,
          SbasisCompletionCounters,
          FrameDiagCounters,
          WithinEpochCulpritsCounters,
          RedundancyCensusCounters,
          MdbCounters,
          BlockBaardaCounters,
          CongruenceCounters,
          PriorResidualCounters,
          PrefitCensusCounters,
          DatumGraphCounters,
          InvariantCarryDriverCounters,
          ProducerArCounters,
          ArSuspectRunCounters,
          ArCycleBasisRunCounters,
          ArLatticeRunCounters,
          FixedCyclesCounters,
          CanonicalPairAdmissionCounters,
          SignalRosterCounters,
          LedgerEmitCounters,
          DdPriorBuildCounters,
          StationBlockCounters,
          ClockQcCounters,
          ShDevVtecCounters,
          RhsAttribCounters {};

    /// @internal The run context: one instance per runNetwork() invocation, passed by
    /// reference into the relocated lifecycle functions.  Non-copyable (it owns move-only,
    /// lazily built helpers).
    struct NetworkRunContext {
        NetworkRunContext(const GnssConfig& config, const NetworkConfig& nc)
            : knobs(NetworkKnobs::snapshot(config, nc)),
              params(NetworkParamRegistry::build(knobs, config)),
              param_inventory(networkRxIfbRule(config)) {
            // The estimability census latch is a counter, so it is seeded here (the one
            // place the context is built) rather than snapshotted into knobs; the dump
            // site decrements it (RF-NS).  The default 6 equals the former hard-coded
            // constant, so an unset key is trace-identical.
            counters.spectrum_diag_budget = nc.trace_estimability_budget;
        }
        NetworkRunContext(const NetworkRunContext&) = delete;
        NetworkRunContext& operator=(const NetworkRunContext&) = delete;

        const NetworkKnobs     knobs;
        /// The parameter registry: one declared property set per
        /// parameter kind, built once from @ref knobs (declared after it, so it is
        /// initialized after it) and read, never written, inside the epoch loop.  The
        /// single process-noise source of the border kinds (@ref NetworkParamRegistry::q).
        const NetworkParamRegistry params;
        /// The parameter inventory: the run's receiver IFB rule
        /// (@c param_inventory.rx_ifb, built here once and read by the station split), and
        /// every state column of every station as a parameter instance with its disposition,
        /// folded after PASS B.  The tallies are trace class: written by the drain, never
        /// read by the solve.  Tabled after the first emitted epoch and at run end
        /// ("parameters as armed").
        NetworkParamInventory  param_inventory;
        /// The contributions to the normal equations, per declared kind (see
        /// network/assembly/Contributions.hpp): the station side drained at the end of PASS B,
        /// the shared folds at the epoch tail.  Trace class: never read by the solve.
        /// Tabled after the first emitted epoch and at run end ("contributions as armed").
        NetworkContributionLedger contributions;
        NetworkCarryState      carry;
        /// The cross-epoch run state as entity tables (
        /// network/state/Entities.hpp): each sub-step moves one entity's members out of
        /// @ref carry into its table.
        net::NetworkEntities   entities;
        NetworkServiceCounters counters;
        NetworkBiasAdmission   admission;   // latched census result of CB-ADM (active=false: the legacy path)
        /// Event-harness scripted-fault schedule and ledger (network/carry/Inject.hpp).
        /// Kept outside the warm-resume fingerprint and the snapshot on purpose: a leg
        /// resumes a converged state and then applies its own schedule, and two legs off one
        /// snapshot are designed to differ only here.  armed == false (the default,
        /// network.inject absent) makes every consumer of this object short-circuit, and the
        /// run is byte-identical.
        NetworkInjectState     inject;
        /// The per-epoch signal roster and its run-scope caches (see
        /// docs/refound/SID_signal_identity_2026-09-22.md section 5).  Kept outside the
        /// warm-resume fingerprint and the state snapshot on purpose: nothing here is read by
        /// the solve, so a resumed run re-seats every cell at its first post-resume epoch,
        /// the right posture for a fresh observer.  Built on every network run
        /// (networkSignalRosterArm sets active); active == false, the default,
        /// is a context no run set up, where every seam returns on its first line.
        SignalRoster           roster;
        /// The arc lifecycle machine.  Its records are the @c ledger
        /// fields of @ref entities' arc rows, so it is bound to
        /// @ref entities, declared above it.  Same exemption and same reason as
        /// @ref roster; advanced by the same single call.
        ArcLedger              arc_ledger{entities};
        /// The per-epoch reason buffer that every removal screen writes and the roster's
        /// audit pass folds into the ledger (network.ledger_screens).
        /// Same exemption as @ref arc_ledger: nothing in the solve reads it, so it is
        /// neither snapshotted nor fingerprinted.  armed == false (the default) makes every
        /// writer return on its first line.
        ArcReasonEpoch         reasons;
        /// The emit-site capture, the generation memories the fold differences against, and
        /// the declared roster the wire carries (network.ledger_emit).
        /// Same exemption and same reason as @ref arc_ledger: nothing in the solve reads it,
        /// so it is neither snapshotted nor fingerprinted.  armed == false (the default)
        /// leaves every member untouched and both seams unreachable.
        LedgerEmitState        ledger_emit;
    };

    // ------------------------------------------------------------------
    // The relocated RTB-7 snapshot channel and carry lifecycle, as free functions over
    // the context (decomposition INC-4).  They are free functions rather than Application
    // member functions because none needs Application state beyond config_, which is
    // passed explicitly (parameter names match the former use sites).  Where a body opens
    // with an alias block, the block binds the driver's former local names to members of
    // the context, so the relocated code keeps those names.
    // The three-place rule: fingerprint terms live in networkSnapshotFingerprint
    // (SnapshotFingerprint.cpp), the restore in networkTryWarmResume
    // (WarmResume.cpp), and the write in networkWriteStateSnapshot
    // (StateSnapshotWrite.cpp); a new snapshot field touches all three or documents
    // its exemption at its NetworkCarryState declaration.
    //
    // The bodies, with NetworkKnobs::snapshot, are in seven files of src/core/app/, one
    // per concern; each declaration names its file.  They were split out of
    // the former src/core/app/NetworkRunContext.cpp, so a citation of that file in the
    // record resolves through this list:
    //   Knobs.cpp                the knob snapshot (NetworkKnobs::snapshot)
    //   SnapshotFingerprint.cpp  the snapshot fingerprint
    //   WarmResume.cpp           the warm resume: read, validate, restore
    //   StateSnapshotWrite.cpp   the snapshot write
    //   BorderPrior.cpp          the priors folded on the shared (border) columns,
    //                                   and the exact coupling update's inputs
    //   LocalCarryHarvest.cpp    the station-local carries: harvest, stash, prune
    //                                   (declared in
    //                                   network/carry/LocalCarryHarvest.hpp and cut by
    //                                   carried kind; its two resume steps are in
    //                                   WarmResume.cpp)
    //   BorderStash.cpp          the epoch-tail border stash and the retro-URA flags
    // ------------------------------------------------------------------

    /// @brief Form the run's configuration fingerprint (RTB-7), which a state snapshot
    ///        carries and a warm resume checks; a no-op without a snapshot path.
    /// Defined in src/core/network/state/SnapshotFingerprint.cpp.
    void networkSnapshotFingerprint(NetworkRunContext& ctx, const GnssConfig& config_,
                                    const std::vector<NetStation>& stations);

    class NetworkClockEmitRebase;   // fwd: include/network/emit/ClockEmitRebase.hpp
    class NetworkEmitRebase;        // fwd: include/network/emit/EmitRebase.hpp
    // Resume frame restore: both seams take the CLK-R held-frame engine (a runNetwork
    // local), so its A_c and per-sat baselines travel with the snapshot.  nullptr: the
    // engine's frame is neither captured nor restored.
    //
    // RS-5 (§21.10) adds the phase-bias emit re-base on the same footing (also a
    // runNetwork local, also outputs-only, also neither captured nor restored when
    // nullptr), so both cross-epoch emit layers travel with the snapshot instead of
    // cold-starting at every seam.
    /// @brief Read the state snapshot, validate it against the run, restore the carried
    ///        layers, and stage the two that need the station roster (RTB-7).
    /// Defined in src/core/network/state/WarmResume.cpp.
    void networkTryWarmResume(NetworkRunContext& ctx, const GnssConfig& config_,
                              const MultiRinexSync& sync, NetworkProductEmitter& emitter,
                              NetworkInvariantCarryDriver& carry_driver,
                              const std::shared_ptr<spdlog::logger>& log,
                              NetworkClockEmitRebase* clk_rebase = nullptr,
                              NetworkEmitRebase* emit_rebase = nullptr);

    /**
     * @brief Apply the staged canonical-signal latch to the station models (RS-8, §21.16).
     *
     * The second half of the canonical-signal restore; @ref networkTryWarmResume is the
     * first.  The resume seam parses the snapshot and stages the @c CN rows in
     * @c NetworkCarryState::resume_canon_rows (it has no @c stations and must not grow
     * one).  This seam, called from the driver immediately after it, where the roster is
     * in scope, hands each station's rows to its @c PppModel::restoreCanonicalSignals so
     * the first-epoch guard treats the station as decided.  It counts stations and
     * entries into @c ctx.counters and drains the staging buffer.
     *
     * Unarmed, unresumed, or given a snapshot with no @c CN rows, it does nothing, except
     * that in the armed, row-less case it warns once (standing invariant #4).
     *
     * Defined in src/core/network/state/WarmResume.cpp.
     *
     * @param ctx      Run context (reads the staged rows, writes the counters).
     * @param config_  Run configuration (the knob and the snapshot path).
     * @param stations The station roster; indices key the staged rows.
     * @param log      Run logger.
     */
    void networkRestoreCanonicalSignals(NetworkRunContext& ctx, const GnssConfig& config_,
                                        std::vector<NetStation>& stations,
                                        const std::shared_ptr<spdlog::logger>& log);

    struct NetworkRun;              // network/driver/Run.hpp
    struct NetworkEpoch;            // network/driver/Epoch.hpp
    struct EpochEvents;             // network/driver/Epoch.hpp

    /**
     * @brief Write the periodic atomic state snapshot (RTB-7(a)): on the first call, then
     *        once per network.state_snapshot_interval_s of product time; a no-op without a
     *        snapshot path.
     * Defined in src/core/network/state/StateSnapshotWrite.cpp.
     *
     * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx),
     *            *run.setup.emitter (emitter), run.setup.consumer.get() (consumer). Read:
     *            run.config (config_), run.engines.carry_driver (carry_driver), run.setup.stations
     *            (stations), run.log (log), &run.emit.clk_rebase (clk_rebase), &run.emit.rebase
     *            (emit_rebase).
     * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se.time (t),
     *           ep.clock.pl_now_s (pl_now_s).
     *
     * The former parameters, by binding:
     *   - @p ctx = run.ctx.
     *   - @p config_ = run.config.
     *   - @p t = ep.se.time.
     *   - @p pl_now_s = ep.clock.pl_now_s.
     *   - @p emitter = *run.setup.emitter.
     *   - @p carry_driver = run.engines.carry_driver.
     *   - @p stations = run.setup.stations: The station roster, shared by two capture groups that
     *     cannot be read out of @c ctx.carry. RS-8 (§21.16) @c CN: the canonical-signal latch
     *     lives in each station's @c PppModel. RS-9 (§21.18) @c RL: the code-bias linearization
     *     point is a property of each station's own state vector
     *     (@c stations[i].sm->getStateVector()), which no other snapshot record needs; the
     *     RC/SD/ER rows are all plain copies out of @c ctx.carry. Read-only in both cases, and
     *     each group is written only under its own knob (@c network.state_snapshot_signal_capture
     *     / @c network.state_snapshot_rxcb_lin_capture).
     *   - @p log = run.log.
     *   - @p clk_rebase = &run.emit.clk_rebase.
     *   - @p emit_rebase = &run.emit.rebase.
     *   - @p consumer = run.setup.consumer.get(): The closed-loop consumer engine (nullptr when
     *     the harness is off). Under @c network.closed_loop.state_snapshot each
     *     network history copy (@c state_snapshot_keep_history) is accompanied by the consumer's
     *     own RtStateSnapshot at @c RealTimeEngine::consumerSnapshotHistoryPath (same @c .epNNNN
     *     suffix), written only when the consumer's last processed epoch is @p t (the matching-tow
     *     invariant; a miss is counted in @c counters.cl_snap_skipped and logged). Read-only for
     *     the producer.
     */
    void networkWriteStateSnapshot(NetworkRun& run, const NetworkEpoch& ep);

    /// Install the warm resume's stashed @c RL map on each station's @c StateInitializer,
    /// so the station's one-shot first-epoch code-bias seed takes the writer's
    /// linearization point instead of a median over one epoch of the resume epoch's code
    /// (RS-9, §21.18).  Called from @c runNetwork immediately after
    /// @c networkTryWarmResume, before any station has processed an epoch, which is the
    /// only window in which the one-shot latch has not yet closed.  A no-op (not even a
    /// map allocation) unless @c ctx.carry.rxcb_lin_restore is non-empty, so an unarmed
    /// or cold run reaches no new code at all.
    /// The applied / uncovered halves are read back at the run-end readout directly from
    /// @c ctx.carry.rxcb_lin_sta_counts (the run-end report needs no station roster:
    /// the tallies are indexed, not named).
    /// Defined in src/core/network/state/WarmResume.cpp.
    void networkApplyRxCbLinRestore(NetworkRunContext& ctx, const GnssConfig& config_,
                                    std::vector<NetStation>& stations,
                                    const std::shared_ptr<spdlog::logger>& log);

    /// @brief Fold the SH-VTEC prior (Kaula bound and temporal random walk) on the SH
    ///        coefficient columns, before the null-space rank gate.
    /// Defined in src/core/network/state/BorderPrior.cpp.
    void networkInjectShVtecPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                  const GnssConfig& config_, NetworkEstimator& est,
                                  const SphericalHarmonicIonosphere* sh_model,
                                  const SphericalHarmonicIonosphere* sh_model2,
                                  bool datum_changed, double dt, bool within_epoch_armed);

    /**
     * @brief Fold the open pin-bridge rows (eq34 reattach hygiene, INC-2).
     *
     * One soft shared-border prior per open bridge (@c ctx.carry.pin_bridge), at the
     * key's held level with information 1/σ².  Folded before the null-space rank gate,
     * like the broadcast-anchored clock anchors, because for a
     * bridged key this row is the datum: the election left it without a pin on purpose
     * (it is a spanning-forest root), so the gate would otherwise count its column as an
     * unpinned gauge and withhold the epoch.
     *
     * Along the gauge direction the row realizes, the phase data carry no information
     * (that is what makes it a gauge), so a soft row fully determines it.  In every other
     * direction the same row is about 2–3 decades weaker than the data and is outvoted,
     * which is the intended degradation: a bridge whose held value has gone slightly
     * stale (for example the one-epoch exposure across a clock-datum change, dropped at
     * the next election) biases nothing measurably.  The K2-style margin of the row
     * information over the rank-gate cutoff is measured once and logged.
     *
     * Defined in src/core/network/state/BorderPrior.cpp.
     *
     * @param within_epoch_armed  append to @c ws.cap_priors so the within-epoch RAIM
     *                            re-solve replays the identical shared system.
     */
    void networkInjectPinBridgePrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                     NetworkEstimator& est, bool within_epoch_armed,
                                     const std::shared_ptr<spdlog::logger>& log);

    /// @brief Fold the border carry stashed last epoch (the D2 diagonal or the C1 dense
    ///        fold), after the null-space rank gate.
    /// @param events The epoch's events; reads handover and phase_datum, writes solve (the re-admission seeds).
    /// Defined in src/core/network/state/BorderPrior.cpp.
    void networkInjectD2CarryPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                   NetworkRunTraces& traces, const GnssTime& t,
                                   NetworkEstimator& est, bool datum_changed,
                                   const std::set<char>& changed_consts, double dt,
                                   bool within_epoch_armed,
                                   const std::shared_ptr<spdlog::logger>& log, EpochEvents& events);

    /**
     * @brief Build this epoch's inputs of the exact coupling time update (
     *        @c network.carry_coupling_exact_update) into @c ctx.carry.cxu_epoch.
     *
     * Called by the driver after the epoch's shared columns are registered and before
     * PASS B, whose parallel produce reads the result.  The coupling rows are built before
     * @ref networkInjectD2CarryPrior runs, so the inflation each stashed key will receive
     * is the fold's own decision, made once before PASS B by @ref networkCarryDecide and
     * read here and by the fold: a key the fold folds is in S at the
     * variance the decision forms on its stashed variance (@ref carryFoldVarEff, the dense
     * fold's chain), and a key the fold will not carry, or whose column left the layout,
     * goes to D with its route.  The one clause no point before PASS B can know is a
     * clock-pivot handover, decided later in the epoch; the fold counts the keys in S that
     * the handover re-expressed or retired after these rows were built.
     * Then @f$P_{SS}+Q_S@f$ is factored.  A no-op (with @c ok false) when the knob is off,
     * when no dense block was stashed, or when the stash stamp is not valid (the epoch
     * after a warm resume).
     *
     * Defined in src/core/network/state/BorderPrior.cpp.
     *
     * Epoch form (generated by scripts/nom_ep1_cut.py): the run and the epoch
     * replace the explicit parameters; the definition binds each former parameter in its preamble,
     * under its former name.
     *
     * @param run The run (network/driver/Run.hpp). Written through non-const bindings: run.ctx (ctx).
     *            Read: run.engines.est (est), run.log (log).
     * @param ep The epoch (network/driver/Epoch.hpp), read only. Bindings: ep.se.time (t), ep.clock.dt
     *           (dt).
     *
     * The former parameters, by binding:
     *   - @p ctx = run.ctx.
     *   - @p est = run.engines.est.
     *   - @p t = ep.se.time.
     *   - @p dt = ep.clock.dt: the driver's inter-epoch step, the same value the fold receives.
     *   - @p log = run.log.
     */
    void networkCouplingExactPrepare(NetworkRun& run, const NetworkEpoch& ep);

    /// @brief Fold the two-state clock-model prior (network.clock_model) on the satellite
    ///        clocks, after the null-space rank gate.
    /// Defined in src/core/network/state/BorderPrior.cpp.
    void networkInjectClockOscPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                    NetworkRunTraces& traces, const GnssTime& t,
                                    NetworkEstimator& est,
                                    const std::vector<std::string>& registered_clock_sats,
                                    bool clk_model_warm, bool datum_changed,
                                    const std::set<char>& changed_consts,
                                    double pl_now_s, double dt, bool within_epoch_armed);

    /// @brief The first post-resume harvest's check of one restored regression against this
    ///        epoch's fresh one: matched and unmatched terms, the largest relative coefficient
    ///        change and the RHS change, filed in the run's census (§18.12).  Measure-only.
    ///        Called for an ambiguity (L) row by networkHarvestAmbiguityStation and for a ZWD
    ///        (Z) row by networkHarvestZwd; the two copies were merged.
    /// @param run      The run (its context's counters and census samples).
    /// @param restored The restored carry entry (the L or Z row read from the snapshot).
    /// @param coupling This epoch's freshly derived regression of the same row.
    /// Defined in src/core/network/state/WarmResume.cpp.
    void networkResumeCouplingCensusRow(NetworkRun& run, const LocalCarry& restored,
                                        const std::vector<CoupledShared>& coupling);

    /// @brief The first post-resume fold's CTA trip count, then the first-warm-epoch
    ///        latch (resume.cpl_census) retired.  Last step of the harvest.
    /// @param run The run (its context's carry and counters).
    /// Defined in src/core/network/state/WarmResume.cpp.
    void networkResumeFirstEpochRetire(NetworkRun& run);

    /// @brief Record the satellites the QC tiers flagged, for the next emit's retro-bump
    ///        and the post-refloat URA inflation horizon.
    /// Defined in src/core/network/state/BorderStash.cpp.
    void networkRetroUraBookkeeping(NetworkRunContext& ctx,
                                    const std::set<std::string>& clock_refloat_sats,
                                    const std::set<std::string>& retro_flag_sats,
                                    bool retro_feed);

    /// @brief Stash the border carry (prev_carry, prev_carry_cov) for the next epoch's D2
    ///        fold, with the READMIT continuity bookkeeping.
    /// @param events The epoch's events, read: the IF-pin pairs, the re-admission seeds, the code-bias excursion marks.
    /// Defined in src/core/network/state/BorderStash.cpp.
    void networkStashCarry(NetworkRunContext& ctx, const GnssConfig& config_,
                           NetworkRunTraces& traces, const GnssTime& t,
                           const NetworkEstimator& est, const Eigen::VectorXd& x_s,
                           const NetworkSbasis::Plan& plan,
                           const SphericalHarmonicIonosphere* sh_model,
                           const SphericalHarmonicIonosphere* sh_model2,
                           const std::set<std::string>& clock_refloat_sats,
                           const std::vector<std::string>& registered_clock_sats,
                           const std::set<std::string>& shared_phase_keys,
                           const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
                           const EpochEvents& events,
                           const std::shared_ptr<spdlog::logger>& log);

} // namespace GnssProcessor

#pragma once
/**
 * @file NetworkRunContext.hpp
 * @brief @internal Tier-1 run state of Application::runNetwork: the const
 *        config-knob snapshot (NetworkKnobs), the cross-epoch carry state
 *        (NetworkCarryState), and the service counters
 *        (NetworkServiceCounters), bundled as NetworkRunContext.
 *
 * The members keep the names and types of the run-loop locals they replaced
 * (decomposition INC-4; see docs/network_decomposition_plan.md).  The run loop in
 * ApplicationNetwork.cpp binds `auto& X = ctx.carry.X;` / `const T X = ctx.knobs.X;`
 * aliases under those names, so the use sites read as before.
 *
 * Three-place rule: a NetworkCarryState field marked snapshot-member must be
 * added to the fingerprint, the restore and the snapshot write, all three in
 * src/core/app/NetworkRunContext.cpp.  A new cross-epoch field goes into
 * NetworkCarryState under that rule unless it is explicitly exempted; the
 * per-field "not snapshotted" notes below are the exemption record.
 * Kept out of this header on purpose (they stay locals of the run TU): the
 * engine objects (NetworkEstimator, NetworkInvariantCarryDriver, NetworkCarryVce,
 * NetworkIonoDeweightGate, NetworkEmitRebase, NetworkPbEmitFilter, the WL/UWL
 * lane machinery and its function-local types), the D0/D0b section-timer
 * instrumentation, and the closed-loop consumer metrics.
 */

#include "filter/NetworkRunTypes.hpp"
#include "filter/NetworkParamRegistry.hpp" // NetworkParamRegistry: one declared property set per parameter kind (card PRM)
#include "filter/NetworkParamInventory.hpp" // NetworkParamInventory: every parameter instance with its disposition (card NOM N1)
#include "filter/NetworkCarrySites.hpp"     // CarrySite / CarryMechanism: the carried references of the border (card PRM phase 2)
#include "filter/NetworkResponses.hpp"      // ResponseSite: the declared responses of the carried instances (card PRM phase 3a)
#include "filter/NetworkCouplingExact.hpp"  // CouplingExactEpoch / CouplingExactCounters (card CTU)
#include "filter/NetworkPivotFrameQ.hpp"    // PivotFrameCounters (card PFQ)
#include "filter/NetworkPbHoldClone.hpp"    // PbHoldClone (card SMH)
#include "filter/NetworkClockClone.hpp"     // ClockCloneEpoch / ClockCloneCounters (card SCL)
#include "filter/NetworkSharedHistory.hpp"  // SharedHistoryState / SharedHistoryCounters (card SCL2 X2)
#include "filter/NetworkYoungInflate.hpp"   // CarryPrior, SharedCarryCov, LocalCarry, ClockOsc, ShDevCarry
#include "filter/NetworkAnchorHygiene.hpp" // NetworkAnchorTallies (eq34 reattach hygiene)
#include "filter/NetworkSignalIntegrity.hpp" // SignalIntegrityArc + tallies (flicker/gap signal integrity)
#include "filter/NetworkIonoDwRelease.hpp"   // NetworkIonoDwReleaseTallies (iono-deweight release hygiene)
#include "filter/NetworkRaim.hpp"            // NetworkRaimCiTallies (RAIM pool re-host)
#include "filter/NetworkGrossLos.hpp"        // NetworkGrossLosTallies / GrossLosState (card GL)
#include "filter/NetworkPrefitCensus.hpp"    // NetworkPrefitCensusTallies (prefit census / fuse / topology / recorder)
#include "filter/NetworkCarryVceApply.hpp"   // NetworkCarryVceApply (M2 v2 carry variance-factor state)
#include "filter/NetworkMdb.hpp"             // NetworkMdbEpoch + NetworkMdbTallies (Baarda MDB reliability)
#include "filter/NetworkBlockBaarda.hpp"     // NetworkBlockBaarda + BlockBaardaHit + tallies (BlockFormal LOS block census)
#include "filter/NetworkCongruence.hpp"      // NetworkCongruence + CongruenceHit + tallies (station congruence screen)
#include "filter/NetworkDatumGraph.hpp"      // NetDatumGraphSpectral + NetworkDatumGraphTallies (datum graph census)
#include "filter/NetworkMeasurementAdmission.hpp" // MeasurementAdmissionArc + tallies (product-referenced admission)
#include "filter/NetworkRhsAttrib.hpp"       // NetworkAttribFrame (INC-5 flight-recorder retention slot)
#include "filter/NetworkZStore.hpp"     // ArcKey (D5 inject/hold tag sets)
#include "slip/NetworkRawObservables.hpp" // IonoCsState, the CS-GF authority (successor to QUEUE 8b)
#include "slip/NetworkSlipOracle.hpp"  // NetworkSlipOracle (sim-only oracle slip response)
#include "filter/NetworkOraclePin.hpp"  // NetworkOraclePin (sim-only oracle ambiguity pin)
#include "filter/NetworkSbasis.hpp"     // NetworkSbasis::Plan (lifecycle-fn params)
#include "filter/NetworkBiasAdmission.hpp" // latched W6 code-bias admission census, ctx.admission (CB-ADM)
#include "filter/NetworkInject.hpp"        // NetworkInjectState: the event-harness fault schedule and ledger
#include "filter/NetworkCanonicalPairAdmission.hpp" // canonical-pair admission state and counters (card CPA)
#include "app/SignalRoster.hpp"            // SignalRoster and its counters (card SID phase 1)
#include "filter/NetworkChannelAll.hpp"  // the all-channel census (card SID phase 6b)
#include "filter/NetworkMultiGnss.hpp"   // the second-constellation telemetry (card GAL)
#include "filter/ArcLedger.hpp"            // the arc lifecycle machine (card SID phase 1)
#include "filter/NetworkAnchorReentry.hpp" // the anchor drop memo and its read (card SID phase 4)
#include "filter/NetworkLedgerEmit.hpp"    // the emitter read and the declared roster (card SID phase 5a)
#include "logging/NetworkRunTraces.hpp" // FlagRate (B8 counters)

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

    class RealTimeEngine;   // the closed-loop consumer handed to networkWriteStateSnapshot (card CC, part B)

    class GnssConfig;
    struct NetworkConfig;
    class MultiRinexSync;
    class NetworkProductEmitter;
    class NetworkInvariantCarryDriver;
    class NetworkCarryVce;
    class NetworkPriorResidual;   ///< filter/NetworkPriorResidual.hpp; a run-TU local, like NetworkCarryVce.
    class SphericalHarmonicIonosphere;
    struct GnssTime;

    /// @internal Const snapshot of every run-loop config knob (decomposition
    /// INC-4).  snapshot() reads the getters in the original order; members are
    /// value copies under the original local names and types.  The WL/UWL-zone
    /// knob consts stay in the run TU with their machinery.
    struct NetworkKnobs {
        double q_clock{};
        double q_pb{};
        double q_cb{};
        /// network.ledger_reentry: the gap re-entry response takes its population from
        /// the arc ledger's admitted lifecycle (card SID phase 3).  Read here only by the
        /// run-end table; the drains take the switch through @c SlipQcParams.
        bool ledger_reentry{};
        /// network.ledger_screens (the reason-mask writers and the anchor read) and
        /// network.ledger_anchor (the restore that acts on that read), card SID phase 4.
        /// Snapshotted for the run-end readout, which has no config; it reads
        /// @ref ledger_anchor.  @ref ledger_screens has no reader through this struct:
        /// the writers and the anchor read take their switch from
        /// @c ArcReasonEpoch::armed and @c AnchorReentry::observe.
        bool ledger_screens{};
        bool ledger_anchor{};
        bool pl_on{};
        double pl_q_amb{};
        int pl_warmup{};
        double pl_max_gap_s{};
        double pl_var_floor{};
        /// network.phase_leveling_los_scale (F1): at the leveling-carry inject, multiply
        /// each carried ambiguity row's variance by B_eff, the number of carried ambiguity
        /// columns injected on the same (station, satellite) LOS this epoch, so the B rows
        /// assert one unit of iono-direction precision instead of B (the over-count that
        /// holds the level for K_eff = (B+1)/(B−1) epochs).  Applied after q·gap, before
        /// the variance floor and the VCE factor.  Off by default (byte-identical).
        bool pl_los_scale{};
        /// network.phase_leveling_local_block (F3): carry each station's injectable
        /// carried ambiguity set as one jointly whitened local block (the harvested
        /// conditional cross covariances (N_rr⁻¹)_{c_i c_j} on Σ's off-diagonals;
        /// non-admitted columns marginalized) instead of independent per-column rows.
        /// Given the border, the block is exact in every local-local direction: the
        /// per-LOS iono direction that F1 fixes, and the station-common rx clock, ZWD and
        /// rx-DCB directions that F1 leaves over-counted.  Supersedes pl_los_scale on
        /// whitened blocks (F1 applies only on a factorization fallback).  Live only with
        /// phase_leveling on, carry_invariant off, carry_cpl_cond on and carry_cpl_joint
        /// off; the loader warns that the knob is inert otherwise.  The knob is off by
        /// default (byte-identical).
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
        /// network.phase_leveling_local_block_min_age_ep: F3 block admission age gate.  A
        /// carried leg younger than this many loop epochs (counted from its first stash)
        /// is kept out of the station block; @ref pl_blk_young_free decides whether it is
        /// emitted as a per-column row (F1-scaled if armed) or not injected.  A leg born at
        /// a satellite's re-admission carries its code-level birth error, and the block's
        /// joint whitening would spread that error into the station's mature columns: the
        /// post-eclipse DD episodes measured in SPEC_amb_iono_split_overcount §6.9 (§6.11
        /// shows they do not come from the shared border).  0 = off (byte-identical).
        int  pl_blk_min_age_ep{};
        /// network.phase_leveling_local_block_young_mode == "free": a young leg is not
        /// injected until it matures.  false = "diagonal": per-column rows, measured
        /// harmful (SPEC_amb_iono §6.11 arm 5).
        bool pl_blk_young_free{};
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
        bool   amb_drop_decouple_always{};   ///< network.amb_float_drop_decouple_always (card CTU section 14): a drop clears the arc's regression and block row even at birth strength
        // The per-line-of-sight phase nuisance state (card LGM section 4.1).  pc_on is the
        // knob as the loader validated it (float vehicle, phase_leveling, conditional
        // coupling; no joint whitening, no carry_invariant), so every site reads it alone.
        bool   pc_on{};              ///< network.phase_colour_state
        double pc_sigma_zen{};       ///< network.phase_colour_sigma_zen_m (m)
        double pc_tau_s{};           ///< network.phase_colour_tau_s (s)
        double pc_floor_deg{};       ///< network.phase_colour_elev_floor_deg (deg)
        double pc_max_gap_s{};       ///< network.phase_colour_max_gap_s (s)
        bool   pc_trace{};           ///< network.trace_phase_colour (and pc_on)
        bool   amb_if_trace{};       ///< network.trace_amb_if (card AIL; trace class)
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
        /// network.amb_float_q_m2s: process noise on the ambiguity block under float_kf
        /// (RF-1 §3.5).  The card's posture is 0.0.  Not applied to the solve; carried so
        /// the run manifest and the run-end telemetry can state it.
        double amb_float_q_m2s{};
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
    /// Age-aware young-arc variance inflation (network.phase_leveling_young_inflate_*,
    /// SPEC_amb_iono_split_overcount §6.17–§6.18).  A carried ambiguity column whose last
    /// re-level (@ref LocalCarry::relevel_s) is recent carries a code-quality level at a
    /// phase-stiff harvested variance, so the T4 tier erases it, and the repeated
    /// erase/re-level cycle sustains a metre-class ZWD/clock plateau.  V0·exp(−age/τ) is
    /// added to the column's prior variance: inside the F3 block as a PD- and
    /// correlation-preserving congruence scaling of its row and column of Σ, and on the
    /// per-column row as a plain var_eff += var_add.  @c var_m2 == 0 = off
    /// (byte-identical); the loader zeroes it unless phase_leveling is on and
    /// carry_invariant is off.
    NetworkYoungInflate::Knobs yng{};
        bool pl_warmup_on_conv{};
        bool pl_refloat_on_conv{};
        double pl_conv_rms_thresh{};
        double pb_var_floor{};
        double cb_var_floor{};
        bool fullcov_on{};
        bool carry_cpl_cond{};
        bool carry_cpl_joint{};
        bool carry_cpl_tu{};
        /// network.carry_coupling_exact_update (card CTU): the exact conditional time update
        /// of the coupling rows in place of card 1d's scalar kappa, on every row harvested
        /// at the stash the dense border block belongs to.
        bool carry_cpl_exact{};
        /// network.carry_coupling_exact_cross (card WSX): the exact update's bracket between
        /// two rows of a station, added in the whitened local block.
        bool carry_cpl_exact_cross{};
        /// network.clock_pivot_frame_q (card PFQ): the carried clocks' process noise in the
        /// pivot frame (filter/NetworkPivotFrameQ.hpp).
        bool pfq_on{};
        /// network.carry_coupling_clock_clone (card SCL): the previous epoch's satellite
        /// clocks as one-epoch clone columns in the shared border (filter/NetworkClockClone).
        bool carry_cpl_clone{};
        /// network.carry_coupling_shared_history (card SCL2 X2): the clones' shared
        /// conditional carried as history columns (filter/NetworkSharedHistory).
        bool carry_cpl_shx{};
        /// network.carry_coupling_bias_clone (card BCL-a): the phase and code biases on
        /// one-epoch clones with walk factors (filter/NetworkClockClone).
        bool carry_cpl_bclone{};
        /// network.carry_coupling_departed_clone (card BCL-d): the keys card CTU
        /// re-attributes on one-epoch clones with the stashed prior (filter/NetworkClockClone).
        bool carry_cpl_dclone{};
        /// network.carry_coupling_shared_history_rank (card SCL2 X2): history columns kept
        /// per cloned clock (1 to 8).
        int  shx_rank{1};
        bool clm_on{};      ///< network.clock_level_monitor (card CTU): the level monitor, measure-only
        bool clm_raise{};   ///< network.clock_level_bound_raise (card CTU): raise the stashed block to the level bound
        int  clm_restore_credit_ep{};   ///< network.clock_level_restore_credit_ep (card CTU G5): epochs of code information the restored level is credited with (0 = all the snapshot's)
        bool clm_restore_raise{};       ///< network.clock_level_restore_raise (card CTU G5): raise the restored border block along each satellite's level to its code-only variance, once, at the first leg epoch
        int  clm_innov_window_ep{60};   ///< network.clock_level_innov_window_ep (card CTU section 14): the level innovation instrument's window, epochs
        bool pv_on{};                   ///< network.phase_visible_monitor (card PVN): the phase-visible consistency instrument, measure-only
        bool nbp_on{};                  ///< network.apriori_source sp3_brdc_clock (card NBP): the native broadcast clock a priori; the wire refers to the stamped ephemeris
        int  pv_window_ep{60};          ///< network.phase_visible_window_ep (card PVN): its window, epochs
        bool carry_cpl_shmarg{};
        /// network.carry_coupling_gauge_project (card 1f): hold each (constellation, kind)
        /// coefficient sum of a truncated coupling row at its harvest value, so a departed
        /// column does not turn the row into a high-weight observation of the current datum
        /// gauge pinned at the old realization.  Without it, a few departures in one epoch
        /// can lift the smallest null eigenvalue decades above the rank-gate cutoff and
        /// cause spurious over-specified withholds that clear only when pl_max_gap_s
        /// expires the carry.
        bool carry_cpl_gauge{};
        /// network.carry_coupling_reinit_sources (card 1g): honour the reinit history with
        /// the κ time update off, and feed it from every realization-step source
        /// (phase-anchor re-reference, eq34 chain re-reference, datum-slip PB
        /// re-realization) rather than from the clock-QC sources alone.  A re-realized
        /// column carries an unknown frame step in its stashed ŝ_prev; folding it as a
        /// full-weight present term would inject that step as a row bias at
        /// conditional-variance (mm) confidence.
        bool carry_cpl_reinit{};
        bool zwd_on{};
        /// Slant-iono carry (network.carry_iono; QUEUE 8b).  The loader guarantees
        /// phase_leveling and relevel_free_epoch (scope=relevel) and enforces the
        /// SH / mech-B / carry_invariant mutexes.
        bool iono_on{};
        bool iono_scope_always{};  ///< iono_carry_scope == "always" (else "relevel").
        bool iono_source_smoothed{};  ///< iono_carry_source == "smoothed_gf" (the CS-GF authority) vs "solve".
        double iono_q{};       ///< Slant-iono carry RW process noise (m²/s).
        double iono_var_floor{};   ///< Inject-side variance floor (m²), so the data can correct a wrong carry.
        double iono_max_gap_s{};   ///< Carry eviction gap (s); deliberately longer than pl_max_gap_s.
        /// network.carry_iono_in_block (I-in-block): the station's present per-LOS iono
        /// columns join its F3 whitened local block instead of the diagonal
        /// network.carry_iono row (FALSIFIED).  The harvest appends them to `cols` between
        /// the ambiguities and the ZWD, so `PlHarvestBlock::C` is (n_amb+n_iono+[1])²; each
        /// entry stashes with the same `blk`/`blk_idx` as an ambiguity; PASS B pushes one
        /// more `LocalBlockCol` per LOS.  A diagonal per-LOS row asserts the iono direction
        /// that the station's B carried band priors already assert (the B-fold over-count
        /// of SPEC_amb_iono_split_overcount §7 item 17); inside the block the harvested
        /// cross terms subtract it exactly.  The loader guarantees phase_leveling,
        /// pl_local_block and carry_cpl_cond with carry_invariant off, and refuses a
        /// co-arm with carry_iono.  Off by default (byte-identical).
        bool iono_blk{};
        double iono_blk_q_m2{};      ///< I-in-block RW process noise, m² of L1 slant delay per epoch, not per second (see GnssConfig::networkCarryIonoInBlockQM2).
        double iono_blk_max_gap_s{}; ///< I-in-block staleness bound (s); the loader has already resolved the "= phase_leveling_max_gap_s" sentinel.
        /// network.carry_rx_clock_in_block (RX-CLK-in-block): the station's receiver clock
        /// column (and, under @c rxclk_isb, its per-system ISB columns) join its F3 whitened
        /// local block.  Otherwise the clock is a free per-epoch local column with no
        /// temporal prior on this path, and it is the rho-direction sink into which the ZWD
        /// and iono cards push their residual error.  It is carried as a random walk about a
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
        bool rxcb_blk{};      ///< RXCB-IN-BLOCK (network.carry_rx_code_bias_in_block, card CTU section 14): the rx-CB columns fold inside the station's whitened local block; loader guarantees rx_cb_on + coupling + a local block.
        bool rx_cb_datum{};   ///< Zero-mean-over-receivers DCB datum (network.rx_code_bias_datum; card RXCB-DATUM).
        double rx_cb_datum_sigma{};  ///< Loose physical-bound sigma (m) of that datum (network.rx_code_bias_datum_sigma_m).
        bool sta_prior_pre_pd{};  ///< Fold the station's datum rows before the addStation PD test (network.station_prior_before_pd; card RF-PD-fix).
        double rx_cb_q{};   ///< rx-CB carry RW process noise (m²/s).
        bool rx_cb_demean{};  ///< Inject η = λ·(innov − μ_c) and gate on |innov − μ_c| (card LV-2, SPEC_level_information_budget); loader guarantees rx_cb_on.
        int rx_cb_demean_min_sta{};  ///< Contributing-station floor below which μ_c is frozen, a thin-mean guard (card LV-2).
        /// Reduce μ between PASS A and PASS B and fold against this epoch's value, with no
        /// lag (card LV-5, SPEC_level_information_budget §12).  Loader guarantees rx_cb_demean.
        bool rx_cb_demean_within{};
        /// Disable the frame-dependent inject screen and erase instead on a frame-invariant
        /// step at the harvest (card LV-4, SPEC_level_information_budget §11).  Loader
        /// guarantees rx_cb_on.
        bool rx_cb_step_gate{};
        double rx_cb_step_k{};       ///< k·σ of the invariant step screen (card LV-4).
        /// Post-solve Λ-weighted de-mean of the harvested absolute rx-CB realization, per
        /// family: the "S-consistent harvest" (card S2H, SPEC_level_information_budget §17
        /// item 3).  Loader guarantees rx_cb_on and forces LV-2/LV-5 off.
        bool rx_cb_harvest_demean{};
        double rx_cb_harvest_screen_z{};  ///< Robust peer-screen threshold for the μ vote; the loader validates > 0 (card S2H).
        bool lv1_seed_frame{};       ///< Reframe per-LOS iono seeds into the field's frame at allocation (card LV-1, SPEC_level_information_budget); loader guarantees estimate_iono_sh.
        int lv1_min_sats{};          ///< Minimum raw seeds for the per-station median offset (card LV-1).
        double zwd_q{};
        /// network.carry_zwd_in_block (B9-in-block): the carried ZWD column joins its
        /// station's F3 whitened local block instead of folding as a diagonal coupling row
        /// outside the block.  The harvest appends it to `cols`, so `PlHarvestBlock::C` is
        /// (n+1)²; the entry stashes with the same `blk`/`blk_idx`; PASS B pushes it as one
        /// more `LocalBlockCol`.  A diagonal ZWD row asserts a station-common direction that
        /// every carried ambiguity row already asserts: the ~n_LOS over-count named in
        /// SPEC_amb_iono_split_overcount §7 item 17, and the reason B9 was falsified on the
        /// quiet bed.  Exempt from the F3 young-column age gate (a ZWD is not an arc).  The
        /// loader requires zwd_on and pl_local_block and disables the knob otherwise.  Off
        /// by default (byte-identical).
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
        /// network.amb_seam_reinflate_var_m2: one-shot variance (m²) added to every
        /// restored leveling carry at a warm resume (STABILITY target 2, leg C).  Not
        /// fingerprinted (forward-only resume response policy).  0 = off.
        double amb_seam_reinflate_var{};
        /// network.resume_frame_restore (SPEC_synthetic_minimum_network §18.10): at a warm
        /// resume, restore the serialized frame state (phase-anchor incumbents and sticky
        /// pin targets, the Q3 clock-anchor per-sat state, the CLK-R held frame), so the
        /// first post-resume election, fold and emit continue the pre-seam realization
        /// instead of re-pinning at cold seeds.  Not fingerprinted (forward-only resume
        /// response policy).  Off = byte-identical resume (the rows are parsed into maps
        /// nothing reads).
        bool   resume_frame_restore{};
        /// network.resume_emit_restore (RS-5, SPEC_event_harness §21.10): at a warm resume,
        /// restore the two outputs-only emit engines before the first emit:
        /// @c NetworkEmitRebase's emission frame (the snapshot's ER rows) and
        /// @c NetworkPbEmitFilter's per-key posterior and per-constellation σ̂_step
        /// accumulators (PF / PS).  The emitted phase biases then continue the writer's
        /// emission level instead of stepping to the raw solve frame.  Each half is inert,
        /// and says so in the log, when its rows are absent.  The loader requires
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
        bool   resume_station_rxcb{};     ///< network.resume_station_restore_rxcb (default true): diagnostic sub-switch of @c resume_station_restore, so one §21.13 attribution leg can restore the rx-CB half alone.
        bool   resume_station_shdev{};    ///< network.resume_station_restore_shdev (default true): the SH-deviation twin of @c resume_station_rxcb.
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
        /// @f$M^Tg_s=-g_r(c)@f$ and therefore needs card 1f; a covariance block carries no
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
        /// warm resume, rebuild each restored leveling / ZWD carry's card-1 regression on
        /// the free shared border from the v3 snapshot's QS dictionary and QL/QZ rows
        /// (verbatim, in harvest order), and seed tu_reinit_hist from the columns the write
        /// epoch was re-drawing.  The first PASS-B inject then builds a conditional row
        /// (A_s = M_r, shared-neutral) instead of the plain diagonal prior asserted at the
        /// pure conditional variance, which out-votes the Q3 anchored clock priors at the
        /// first fold (§18.11).  The loader requires resume_frame_restore,
        /// carry_conditional_coupling and carry_coupling_gauge_project, and refuses
        /// amb_seam_reinflate_var_m2 > 0 (that response changes the row's weight; this one
        /// changes only its direction).  Not fingerprinted (forward-only resume response
        /// policy).  Off = byte-identical resume (LocalCarry::coupling stays empty, as the
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
        /// takes the departed route.  0 = off.  Requires the card-1 coupling rows (checked
        /// by the loader).
        int    readmit_marg_ep{};
        /// network.readmit_seed_code_bias: park and seed the per-signal code-bias columns
        /// too (true, the default) or the clock alone (false).  Inert without
        /// readmit_seed_horizon_s.
        bool   readmit_seed_cb{};
        /// network.resume_d5_warmup_ep (RS-2b, SPEC_event_harness §21.6): processed epochs
        /// after a warm resume during which the card-4 D5 innovation gate is diagnostic
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
        /// card-4 invariant-carry lineage against the carry-neutral `qc_amb` census.
        /// Already ANDed with `raim_on` and `carry_invariant`, so the driver, the assembly
        /// and the seam all read one bool and cannot disagree about whether the census
        /// exists.
        bool raim_ci_on{};
        /// The actuator half: a re-hosted flag is queued into `raim_refloat_pending` and
        /// drained into the C3c funnel at the next epoch's head.  ANDed with `raim_ci_on`;
        /// false = the screen measures and counts but queues nothing.
        bool raim_ci_refloat{};
        /// The gross line-of-sight screen is armed for this run (card GL,
        /// docs/SPEC_event_harness.md §24.18).  Resolved once from
        /// @c network.gross_los_screen so the driver, the assembly, the seam and the
        /// teardown all read one bool and cannot disagree about whether the state exists.
        /// It affects the solve.  Off: the seam is never called, no LOS state is
        /// allocated, @c NetStation::gl_code_sigma_scale is never populated, and the run
        /// is byte-identical.
        bool   gl_on{};
        /// The σ multiplier at which a declared line of sight's code rows fold
        /// (@c network.gross_los_deweight; card GL): the assembly divides the row's
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
        bool clock_signal_basis_hold{};
        bool cb_time_linked{};
        bool scoped_handover{};
        /// network.handover_stransform (queue item 1(b)): at a pivot handover, re-express the
        /// changed constellation's carry along the structural clock null direction and fold
        /// it, instead of skipping it for one epoch.  The loader forces it false without
        /// scoped_handover and under an anchor-free clock datum.
        bool handover_stransform{};
        /// network.handover_stransform_before_rows (card PRM phase 2): the handover's
        /// S-transform runs before the fold decision and PASS B, through the traversal of
        /// carried references, so the coupling rows read the re-expressed carry.  The loader
        /// forces it false without handover_stransform.
        bool handover_stransform_before_rows{};
        /// network.handover_stransform_dense (card SCL B2): the handover's S-transform
        /// re-expresses the carried values and the dense block together by S P S^T
        /// (applyClockStransformDense), the diagonal kernel the fallback.  The loader forces it
        /// false without handover_stransform or shared_carry_full_cov.
        bool handover_stransform_dense{};
        /// network.clock_anchor_datum_keep (card SCL B2): card Q3's guard keeps an anchored
        /// clock engaged at a handover the dense kernel re-expressed exactly.  Resolved with
        /// the anchor (cta_on); the loader forces it false without handover_stransform_dense.
        bool cta_datum_keep{};
        /// network.carry_rebase_complete (card PRM phase 3b step 2): each carry re-base
        /// mechanism's walk (card CLV's level re-base, the ephemeris re-level, the eq34 D4
        /// shift, the declared anchor handover) also moves the sites its exemption table
        /// marks as owed (carryMechanismOwedSites; the phase hold's held value excepted).
        /// The loader forces it false when none of those mechanisms is armed.
        bool rebase_complete{};
        bool pb_excl_unmatched{};
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
        /// (network.emit_clock_frame_hold; card CLK-R stage 1).  The engine is a run-TU
        /// local (see the file header), like NetworkPbEmitFilter.
        bool clk_frame_hold{};
        bool clk_jump_test{};   ///< Mark per-sat common-removed clock steps through the R-record retro-bump (network.clock_disc_jump_test; card CLK-R stage 2).
        bool   emit_pb_hold{};             ///< Hold each (sat, signal) emitted phase bias at the first value of its generation (network.emit_pb_hold; card PBH); state in NetworkCarryState::pb_hold.
        double emit_pb_hold_max_gap_s{};   ///< Arc gap (s) beyond which a returning key starts a new generation (network.emit_pb_hold_max_gap_s; card PBH).  Inert unless emit_pb_hold.
        bool   emit_pb_hold_phase_clock{}; ///< Hold the phase-visible clock instead of the phase bias (network.emit_pb_hold_phase_clock; card LAH).  Inert unless emit_pb_hold.
        bool   emit_pb_hold_smoothed{};    ///< The smoothed hold (network.emit_pb_hold_smoothed; card SMH): the held phase bias follows the clock clone c(t0|t); LAH's transfer after a clone dies.  Inert unless emit_pb_hold.
        bool   emit_pb_hold_smoothed_block{}; ///< The smoothed hold's phase-visible member (network.emit_pb_hold_smoothed_block; card SMD): the wire takes the block clone's Phi_D instead of the clock clone's Phi.  Inert unless emit_pb_hold_smoothed.
        // --- Guarded temporal clock anchor (card Q3) ---
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
        int    cta_space{};          ///< network.clock_anchor_guard_space (card CTU section 14): 0 clock, 1 phase_shadow, 2 phase
        double cta_phi_floor_m{};    ///< network.clock_anchor_phase_sigma_floor_m: sqrt(S) floor of the phase-space statistic (m)
        int    cta_recouple_ep{};    ///< network.clock_anchor_recouple_epochs: consecutive in-band epochs a loose satellite must serve before the anchor re-engages (the re-validation of invariant #7)
        bool   cta_mark{};           ///< network.clock_anchor_mark: wire-mark the decoupling edge (per-sat disc through the B1 retro-bump chain)
        /// The one resolved gauge-free predicate that every fold, withdraw and replay site
        /// reads: @c (cta_on && cta_gauge_free) || (shared_carry_full_cov &&
        /// shared_carry_gauge_free) (card C-GF).  Never re-derive it at a call site.  Fold
        /// and withdraw must agree exactly: a fold that downdated, paired with a withdraw
        /// that did not, leaves the negative-semidefinite residue −ΛU(UᵀΛU)⁻¹UᵀΛ in the
        /// shared normals, which are then not PSD along the gauge.  The constrained solve
        /// then either throws on its pivot guard (a persistent epoch skip, since the stash
        /// never refreshes) or factorizes into a saddle and returns a wrong x_s with no
        /// error.
        bool   carry_gauge_free{};
        /// Require support closure on each datum direction before downdating it (card
        /// C-GF).  True only on the carry-side arming (shared_carry_full_cov and
        /// shared_carry_gauge_free); the legacy Q3 path keeps its carried-clocks rule with
        /// a two-member floor, so it stays byte-identical.
        bool   carry_gauge_free_closure{};
        /// network.clock_anchor_gauge_free (card Q3 §2.1): marginalize the
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
        // --- Satellite clock level: a code-only re-base of the carry (card CLV) ---
        // Solve-affecting and fingerprinted.  `clv_on` is the one predicate every site
        // reads.
        bool   clv_on{};             ///< network.clock_level_carry, resolved (off under network.clock_model, whose oscillator state the re-base does not move)
        double clv_floor_m{};        ///< network.clock_level_sigma_floor_m: V_L = max(1 / I_L, floor^2), so I_L never exceeds 1 / floor^2
        double clv_horizon_s{};      ///< network.clock_level_horizon_s: I_L decays by exp(-dt / horizon)
        double clv_ignition_var{};   ///< V_L reported for a satellite whose filter is not armed: filter.initial_std.sat_clock^2 (the clock's cold-start variance)
        bool   clv_pin_pair_read{};  ///< network.clock_level_pin_pair_read (R-A): read the level only from the realized pin pair
        int    clv_cb_follow{};      ///< network.clock_level_free_cb_follow (R-A): 0 off, 1 "if" (d (1 - (f2/f1)^2)), 2 "full" (d)
        bool   clv_datum_pause{};    ///< network.clock_level_datum_pause (R-B): a datum-class decouple pauses the filter instead of disarming it
        int    clv_confirm_epochs{1};///< network.clock_level_confirm_epochs (R-B): consecutive epochs an arm or a break needs
        bool   clv_pivot_ref{};      ///< network.clock_level_pivot_ref (R-C): shifts referenced to the constellation's clock pivot
        bool   clv_free_cb_rebase{}; ///< network.clock_level_free_cb_rebase (card CLV2 R-A2): each free same-band code-bias column re-based from the same-band difference at dual-tracking stations
        // --- Card HLT, broadcast satellite health on the network path.  SOLVE-AFFECTING and
        // FINGERPRINTED.
        int    sat_health{};         ///< network.sat_health: 0 off, 1 "pivot" (the election passes over an unhealthy satellite), 2 "exclude" (the setup door removed its observations)
        bool clk_model_on{};
        double clk_model_q1{};
        double clk_model_q2{};
        double clk_model_k{};
        double clk_model_r{};
        double clk_model_max_gap{};
        bool emit_clock_rate_on{};
        double start_offset_s{};
        bool trace_flag_rates{};
        bool phase_datum_ensemble{};
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
        double eq34_rpb_prior_sigma{};
        double eq34_rpb_prior_w{};
        double eq34_rpb_q{};
        bool eq34_shift_on{};
        bool eq34_longevity{};
        bool eq34_extend_only{};
        bool eq34_slip_aware{};   ///< network.phase_datum_eq34_slip_aware_election: keep slipped arcs out of the spanning tree, so a slip costs one chord instead of every cycle through a tree edge.
        /// network.phase_datum_eq34_seed_round (docs/SPEC_eq34_seed_round.md): round
        /// otherwise-zero pin targets to λ·round(x_state/λ), so |innovation| ≤ λ/2 by
        /// construction.  Not gated on !carry_invariant: the card-4 zero-target rationale
        /// is re-pin continuity, while this knob targets ignition.
        bool eq34_seed_round{};
        /// network.phase_datum_eq34_seed_round_rehost_frame (SPEC §8.5 INC-2): a pin that
        /// re-hosts takes its frame step from the new host's own ambiguity state (the exact
        /// pre-pin float) and stays shiftable, instead of taking an unknown step that
        /// re-references the (sat, σ).  Resolved to false unless eq34_seed_round.
        bool eq34_seed_round_rehost{};
        bool phase_datum_prioritized{};
        double phase_hold_sigma{};
        bool phase_hold_on{};
        double phase_hold_var{};
        // --- eq34 reattach hygiene ---
        // All four are resolved against the datum mode: the loader already disarms them
        // outside an anchored election, and `anchor_hygiene_on` is the one predicate every
        // site reads for "does this run allocate the arc-age map at all", so an off run
        // never touches the new state.
        bool   anchor_hygiene_on{};   ///< Any increment armed on an anchored (non-ensemble) phase datum: the master gate for the arc-age map and the hygiene seam.
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
        /// `anchored && !phase_datum_eq34` (the eq34 chain owns its own re-reference; the
        /// ensemble has no anchors), resolved the same way as pd_handover_continuous.
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
        // --- iono-deweight release hygiene ---
        // Resolved against the SH field and the gate: the loader has already disarmed every
        // increment without estimate_iono_sh (and INC-1 without the adaptive gate), and
        // `dw_hygiene_on` is the one predicate the seam and the assembly read for "does
        // this run sweep at all", so an off run touches nothing.
        bool   dw_hygiene_on{};          ///< Master: any increment armed on a live SH field; the gate for the release seam and the held-row lookup.
        bool   dw_release_reseed{};      ///< Release-time slant reseed, ambiguity refloat and stash erase armed (INC-1).
        double dw_release_eps{};         ///< "Decoupled" weight threshold; the release edge is a rise above it (INC-1).
        double dw_park_bound_tecu{};     ///< While-decoupled park bound in slant TECU (0 = off; INC-2).
        double dw_release_validate_k{};  ///< Standardized deviation screen on a re-engaged tie row (0 = off; INC-3).
        double dw_admit_validate_k{};    ///< The same screen at a LOS's first tie row, the admission gate measured as the valve (0 = off; INC-3b).
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
        bool   prires_iono{};         ///< Slant-iono carry family armed (QUEUE 8b): the one instrument that can see a wrong carried iono value (T4 pools phase rows only; the prefit census is x_lin-framed).
        bool   prires_rxcb{};         ///< Receiver code-bias carry family armed (INC-2).
        /// Satellite-clock carry family armed (card Q3, INC-0).  Unlike every other family,
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
        double bb_report_sigma{};     ///< Trace/event threshold on t_B.  A variance ratio (E = 1 under H0), not σ units; see NetworkBlockBaarda.hpp.
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
        double cg_report_sigma{};     ///< Trace/event threshold on t_i.  A variance ratio (E = 1 under H0 for every observable dimension), not σ units; see NetworkCongruence.hpp.
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
        double pc_fuse_sigma{};       ///< Weight cap (0 = off; INC-2), the one solve-affecting knob of the card.
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
        bool   adm_screen{};          ///< The response (INC-1/INC-2), the one solve-affecting knob of the card.
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

        /// One-shot factory: the knob-snapshot statements, in the original read order
        /// (parameter names match the former use sites).
        static NetworkKnobs snapshot(const GnssConfig& config_, const NetworkConfig& nc);
    };

    /// @internal Tier-1 cross-epoch carry and run state: the persistent declarations of
    /// runNetwork(), relocated with their comments.  The per-field comments are the
    /// authoritative docs and the snapshot-exemption record.
    struct NetworkCarryState {
        std::vector<CarryPrior> prev_carry;                 // the D2 stash from the last solve
        /// Card NBP: each satellite's previous wire stamp (IODE), for the switch flag of
        /// the O trace and the run-end switch count.  Empty unless the native broadcast
        /// clock a priori is armed.
        std::map<std::string, int> nbp_prev_iode;
        // Per-satellite guarded temporal clock anchor.  Empty and untouched unless
        // network.clock_temporal_anchor.  Not snapshotted by default: a warm resume is an
        // invariant-#1 event, so every satellite re-earns the anchor from the loose state
        // and the stiffness does not outlive the evidence that justified it.  The frame
        // pair is the exception: network.state_snapshot_frame_capture writes the per-sat
        // state of folded satellites, and network.resume_frame_restore restores it
        // (SPEC_synthetic_minimum_network §18.10).  The knob is in the resume fingerprint,
        // so a toggle change cold-starts.
        ClockAnchorState clk_anchor;
        // Per-satellite code-only level filters and the emit-side transfer (card CLV).
        // Empty and untouched unless network.clock_level_carry.  Not snapshotted (see
        // ClockLevelState).
        ClockLevelState clk_level;
        // Satellites whose anchor decoupled and whose per-sat wire mark has not yet landed
        // in an emitted product.  Drained by networkSolPrep with the same present/pending
        // split that every other forced-bump queue uses (mark == land), so a replay
        // reproduces the marks from the serialized R records.
        std::set<std::string> clk_anchor_disc_pending;
        std::set<std::string> prev_clock_refloat_sats;      // clock-monitor flags at the last stash; hard-refloat mode skip-stashes the sat, so prev_carry cannot carry the flag (card 1d)
        // (kind, sat, signal) → content time of the last reinit fold, for multi-epoch-gap
        // coverage (cards 1d/1g).  An empty signal is the sat-wide wildcard: the clock-QC
        // feeds (inflate_q stash marks, clock refloats) mark a whole satellite kind by
        // kind, while the card-1g realization-step feeds mark one (sat, signal) family,
        // because that is the granularity at which a phase-bias datum is re-referenced.
        // buildCplRow probes the exact key, then the wildcard.
        std::map<std::tuple<SharedParamKind, std::string, std::string>, double> tu_reinit_hist;
        SharedCarryCov prev_carry_cov;                      // dense covariance side-car of prev_carry, empty when off (C1)
        // Card CTU (network.carry_coupling_exact_update).  The stash records when the dense
        // block was taken and which clock the datum pinned then; PASS B's serial preamble
        // builds the epoch (the fold mirror, S/D split and the factor of P_SS + Q_S), which
        // the parallel produce reads and the D2 fold checks.  Not snapshotted: after a warm
        // resume the stash stamp is invalid until the first kept stash, and the rows take
        // the card 1d route for that one epoch (counted).  Empty and unread when off.
        double cxu_stash_s{0.0};                            ///< continuous-GPS s of the stash prev_carry_cov belongs to
        bool   cxu_stash_valid{false};                      ///< the stash was taken this run with the dense block present
        std::map<char, std::pair<std::string, double>> cxu_pivot;  ///< constellation -> (pinned clock sat, pinned value) at the stash
        /// Card PFQ: constellation -> the clock pivot the stashed posterior was pinned on
        /// (the frame the carried clocks are in).  Recorded at every stash and restored from
        /// the snapshot's pivots; empty and unread when network.clock_pivot_frame_q is off.
        std::map<char, std::string> pfq_pivot;
        CouplingExactEpoch cxu_epoch;                       ///< this epoch's inputs; ok = false when off or unbuilt
        ClockCloneEpoch    ccl;                             ///< card SCL: this epoch's clock clones and the rows' copy of cxu_epoch (filter/NetworkClockClone); rebuilt every armed epoch, empty off
        SharedHistoryState shx;                             ///< card SCL2 X2: the held compression and this epoch's history columns (filter/NetworkSharedHistory); not snapshotted, empty off
        std::map<std::string, double> clm_info;             ///< card CTU monitor: satellite -> accumulated code-only level information I_L (1/m^2), undecayed
        std::map<std::string, int>    clm_n;                ///< card CTU monitor: satellite -> epochs of code information accumulated
        std::map<std::string, double> clm_info_c;           ///< card CTU G5: satellite -> code-only level information in the constellation contrast, 1/(s0^2 Q) summed (1/m^2)
        std::map<std::string, double> clm_seam_q;           ///< card CTU G5: satellite -> level process noise of this epoch's time update (m^2), set at a switch-on resume, cleared after the fold
        bool                          clm_restore_raise_pending{false};   ///< card CTU G5: a warm resume restored LB rows and the raise waits for the first leg epoch
        // Level innovation instrument (card CTU section 14), measure-only.
        std::map<std::string, double> clm_prev_clk;         ///< previous monitored epoch's clock per satellite (m), the innovation's prior mean
        Eigen::MatrixXd               clm_prev_pclk;        ///< previous stash's clock-clock block (m^2), the innovation's prior variance
        std::map<std::string, int>    clm_prev_pidx;        ///< satellite -> row of clm_prev_pclk
        double                        clm_prev_s{-1.0};     ///< continuous GPS seconds of the previous monitored epoch
        std::map<std::string, ClockLevelInnovWin> clm_win;  ///< per-satellite window sums
        int                           clm_win_ep{0};        ///< epochs in the current window
        // Phase-visible consistency instrument (card PVN), measure-only: the open window's sums.
        int       pv_win_ep{0};           ///< epochs in the current window
        double    pv_win_nis_sum{0.0};    ///< summed NIS in the window
        long long pv_win_nis_n{0};        ///< NIS values in the window
        double    pv_win_e2_sum{0.0};     ///< summed squared contrast update (m^2)
        double    pv_win_su_sum{0.0};     ///< summed predicted update variance S- - S+ (m^2)
        // Readmit continuity: E2 seed-first re-admission and the block re-coupling edge
        // (network.readmit_seed_horizon_s / readmit_coupling_marg_ep).  Not snapshotted
        // (forward-only response policy): a seam during a satellite's absence loses its
        // seed, which gives the ordinary cold return, and the marginalization windows
        // restart empty.  All empty or zero, and unread, when both knobs are off.
        double readmit_now_s{0.0};          ///< continuous-GPS s of the current epoch, stamped at the D2 install (the stash has no clock of its own)
        double readmit_prev_stash_s{0.0};   ///< continuous-GPS s of the previous kept stash: the park stamp a departing entry receives
        std::map<std::string, long long> readmit_last_seen_ep;  ///< sat → n_loop_epochs of its last stash with a registered clock column (the re-admission detector)
        std::map<std::string, long long> readmit_marg;          ///< sat → loop epoch at which its post-re-admission coupling marginalization expires
        std::map<std::string, double>    readmit_seeded_s;      ///< sat → continuous-GPS s of its last clock seed (the guard's "moved" attribution window)
        struct ReadmitSeed { SharedParamKind kind; std::string sat, signal; double value, var_eff; };
        std::vector<ReadmitSeed> readmit_seed_check;            ///< this epoch's seeds, checked against the solve at the stash (mark-if-moved telemetry for every seeded kind)
        // DD-trace reference age (§6.12.5): (cons, signal) -> sat -> consecutive traced
        // epochs the satellite has been present at >= 2 stations.  An absence erases the
        // entry, so the count is a continuous run, not a lifetime total.  Advanced and read
        // only inside the stride-gated dd-frac emit, so both maps stay empty on a run that
        // does not trace.  Not snapshotted: the trace is a diagnostic, and a warm resume
        // restarts its ages from zero; the first traced epoch after a seam then re-elects
        // by the tie-break, which the run-end switch count shows.
        std::map<std::pair<std::string, std::string>, std::map<std::string, long long>> ddfrac_ref_age;
        std::map<std::pair<std::string, std::string>, std::string> ddfrac_ref_prev;  ///< last reference elected per (cons, signal): the switch detector
        std::map<std::string, std::string> prev_pivot;      // constellation -> pivot sat (handover detect)
        std::set<std::string> c1_zoh_active;                 // sats currently emitting c1=0 (ZOH); edge trigger for the R6 event (L2/T5)
        // Persistent last-ever pivot per constellation (A1).  Merge-updated, never
        // overwritten wholesale, so a constellation that vanishes for an epoch and returns
        // with a different pivot still raises datum_changed.  Read only when
        // network.pivot_vanish_return_signal is on (otherwise inert).
        std::map<std::string, std::string> last_pivot;
        // network.clock_signal_basis_hold: per-satellite held ionosphere-free clock
        // reference pair (sat -> {sig1, sig2}) for the W6 IF pin.  Latched at the first
        // epoch a satellite's code-bias columns register a usable dual-frequency pair and
        // reused every later epoch while both signals stay registered, so the emitted
        // (δtˢ, dˢ) split stays anchored to one signal basis for the whole run (the
        // analogue of the IGS fixed-clock convention, which keeps the differential clock
        // time-consistent).  Re-latched to the current epoch's freshly selected pair only
        // when a held reference signal leaves the ≥2-station set.  Empty and unread unless
        // clock_signal_basis_hold (so the IF-pin datum is byte-identical when off).  Not
        // snapshotted (v1): a warm resume re-latches from first sight, one small per-sat
        // basis step that is trace-logged but not wire-signalled, like the per-epoch
        // re-selection it replaces.  The toggle is in the resume fingerprint, so a toggle
        // change cold-starts.
        std::map<std::string, std::pair<std::string, std::string>> held_clock_ref;
        // Deterministic IF-pin selection discontinuity (hold off).  With
        // clock_signal_basis_hold off, the pin is re-derived from every epoch's coverage;
        // the deterministic NetworkSbasis::selectIfPinPair makes it a pure function of the
        // registered signal set, so a change between emitted products means a channel
        // entered or left the ≥2-station set.  That steps the emitted (δtˢ, dˢ) split by
        // the satellite's inter-channel code bias, a per-sat datum move the consumer must
        // reset for.  Two maps mirror the phase-anchor discipline (this_epoch_anchor /
        // emitted_anchor), so a transient pin move on a withheld epoch cannot fabricate a
        // bump:
        //  - this_epoch_if_pin_sel: the (sig1, sig2) selected this epoch in the main pass
        //    (scratch, cleared each epoch, decoupled from the emit decision);
        //  - last_if_pin_sel: the pair of the last emitted product per satellite, compared
        //    and advanced only at the emit site (like emitted_anchor / prev_pivot), so a
        //    withheld epoch leaves it untouched and its transient pair is dropped.
        // Both are populated only under !clock_signal_basis_hold && emit_clock_disc (the
        // hold path signals its own re-latch through held_clock_ref), so the output is
        // byte-identical otherwise.  Not snapshotted: a warm resume has no prior for its
        // first emit, so it must not fabricate a disc it cannot verify.
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_if_pin_sel;
        std::map<std::string, std::pair<std::string, std::string>> last_if_pin_sel;
        // S-contract §1.5 declaration scratch: the realized code-bias datum pair per
        // satellite, recorded unconditionally (the only guard is a real dual-frequency pin,
        // f1, f2 > 0) and read only by the cbpin=/gfpin= fill in NetworkSolPrep.  It is
        // kept apart from this_epoch_if_pin_sel on purpose.  That map is the hold-off
        // disc-compare scratch, gated on `!clock_signal_basis_hold && emit_clock_disc`:
        // reading it would make a product-contract declaration depend on a QC-channel knob
        // (emit_clock_disc defaults false, so a posture without it would declare no pin),
        // and widening it would change which satellites get a discontinuity bump, a wire
        // change.  This map covers both selection paths (the hold-off per-epoch
        // re-selection and the clock_signal_basis_hold latched pair), so the declaration
        // tracks the datum the run realized.  Cleared each epoch; declaration-only, so the
        // solve and every emitted numeric value are untouched.
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_if_pin_real;
        // S-contract §1.5: the satellites that also received a geometry-free datum row this
        // epoch (network.code_bias_gf_datum, Odijk rd 3b).  The GF row is folded on the
        // same (sig1, sig2) as the IF pin, so the pair itself lives in
        // this_epoch_if_pin_real; this set records which satellites got the second row,
        // which the gfpin= declaration needs (a satellite with no valid dual-frequency pin
        // gets neither row).  Empty whenever the GF datum is off, so there is no gfpin=
        // token and the S line is byte-identical.  Cleared each epoch.
        std::set<std::string> this_epoch_gf_pin_real;
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
        /// This epoch's held extra-band code-bias columns, keyed `"sat|signal"` (card B5D):
        /// a column on a band outside the pin pair that no present station determined and
        /// that had no previous solved value to carry, so a level row in the channel's own
        /// frame holds it and its bias is withheld from the wire (the RF-0f rule: a
        /// consumer cannot tell a fabricated value from an estimate by value).  Filled by
        /// the row builder through @c CodeBiasDatumRows, cleared each epoch by the solve
        /// glue, read by the emit prep; empty unless `network.sat_gf_extra_band_free` is
        /// armed.
        std::set<std::string> this_epoch_extra_band_held;
        // Run-latched family census (FAMILY-GF).  Odijk rd 3b re-derives per receiver
        // channel family when the station population partitions (the Galileo C/Q-vs-X
        // tracking split), because the primary pin is built on the satellite's
        // lowest-ordered signals and so never touches an X-only station's columns.
        // Whether that partition exists is a property of the receiver population, not of
        // an epoch, so the verdict is computed once, on the first epoch with a usable
        // station set, and held for the run: re-deciding it per epoch would make the datum
        // topology follow coverage churn, the flapping that the CB-ADM census exists to
        // prevent.  A bridge appearing or disappearing mid-run is not re-detected in v1;
        // the null-space gate is the backstop, and the latched census line attributes any
        // such withhold.  An empty `partitioned` gives zero extra rows (byte-identical),
        // the state of every single-family posture.
        NetworkSbasis::FamilyCensus family_census;
        // The secondary family's realized GF pair per satellite this epoch (FAMILY-GF), the
        // per-family counterpart of this_epoch_if_pin_real.  Feeds the '+'-joined
        // gfpin=<const>:<primary pair>+<secondary pair> declaration, which the wire parser
        // and checkContract accept as is.  Cleared each epoch; empty unless the per-family
        // rows folded.
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_family_gf_pin_real;
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
        std::map<std::string, std::string> anchor_incumbent;
        std::map<std::string, std::string> this_epoch_anchor;
        /// Sticky continuous target (R2): the pin target at which each incumbent anchor is
        /// currently realized, per (sat, signal) "sat_signal" key.  Without it the
        /// continuous handover is a one-epoch deferral: the ledger pin reverts to 0 at T+1
        /// and the frame steps anyway (measured null in §5.0).  Entries exist only for keys
        /// whose last handover was continuous (a cold pin or a first sighting realizes the
        /// 0-frame and stores nothing); rebuilt with anchor_incumbent at each election, so
        /// a key that leaves the union drops its target with its incumbent.  Empty unless
        /// `knobs.pd_handover_continuous`.  Written to the snapshot only under
        /// network.state_snapshot_frame_capture and restored only under
        /// network.resume_frame_restore; otherwise a resume's first handover is cold.
        std::map<std::string, double> anchor_pin_target;
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
        std::map<std::string, std::string> emitted_anchor;
        // eq34 reattach hygiene, warm conditioning (spec addendum): the continuous-GPS
        // second at which each key's datum was last realized and emitted, i.e. the age of
        // the warm memory a cold pin would have to fight.  Advanced at the same emit site
        // as emitted_anchor, and read only by filter/NetworkAnchorHygiene.  Past
        // eq34_prior_realization_max_gap_s there is nothing left to shock (the carried
        // state has expired through its own staleness machinery), so the eligibility block
        // lifts and the key converges the legacy way.  This keeps a network restart from
        // extending its own phase-bias outage by min_arc_ep epochs.  Empty and unread
        // unless the card is armed; not snapshotted (a resumed leg re-earns its baselines
        // on the first emitted epoch).
        std::map<std::string, double> emitted_anchor_s;
        // Chain election of card 3 (network.phase_datum_eq34); all empty and unread
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
        std::map<std::string, std::string> eq34_datum_rx;
        std::map<std::string, std::string> eq34_rx_parent;
        std::map<std::string, std::string> eq34_sat_root;
        // Last epoch's per-(station idx, sat) arc elevation, for the rising-vs-setting test
        // that ranks new spanning-tree edges by how long they will survive.  Rebuilt each
        // epoch from sat_el; empty and unread unless
        // network.phase_datum_eq34_longevity_election.
        std::map<std::pair<size_t, std::string>, double> eq34_prev_el;
        // The datum receiver r1 (pdr=) of the last emitted contract, so a root handover
        // triggers an S-line re-broadcast (advanced only at emit, like the pivot
        // baselines).  Empty on every non-eq34 run.
        std::string eq34_emitted_pdr;
        // Run-scope observability counters of card 3, logged once in the run-end producer
        // summary (the per-epoch reref/erase lines are debug-only, so these totals are the
        // only non-debug record of the reref volume).  Untouched on non-eq34 runs.
        long long eq34_reref_total = 0, eq34_erase_total = 0;
        long long eq34_shift_total = 0;   // carry values S-transformed (v2 shift) over the run
        long long eq34_cpl_sprev_total = 0;   // coupling s_prev references re-based with those shifts; structurally 0 with coupling off, since the stashes are empty (SPEC_card1_coupling_carry §14)
        long long eq34_gap_repin_total = 0;   // tree edges resumed from a carry-alive gap, re-pinned to their pre-gap integer instead of re-leveling from 0 (I2)
        int eq34_mismatch_epochs = 0;
        // Chain-pin audit of card 3 (docs/SPEC_synthetic_minimum_network.md §14 T1; measured
        // record §12.8): this epoch's declared chain-gauge dimension per (constellation,
        // signal) group, keyed "<cons>|<signal>" (the election's dr_key), value Σ over the
        // group's components of (n_c + m_c).  eq34_epoch_bridged counts the hygiene INC-2
        // bridge pins per group, which are realized on the shared side (a bridge row, not
        // a foldAmb pin), so the station-side fold expectation is declared − bridged.  Both
        // are replaced wholesale at each eq34 election and are empty on every non-eq34 run.
        // After PASS B's serial consume they are compared against the pins that landed
        // (folded by foldAmb/foldRpb and on a station that est.addStation accepted), which
        // covers the two silent loss paths that standing invariant #4 warns about.
        std::map<std::string, int> eq34_epoch_declared;
        std::map<std::string, int> eq34_epoch_bridged;
        // Tree edges retained across the current gap because their local_carry is still
        // alive: {receiver index, "sat_signal"}.  Rebuilt at each election's sticky-map
        // update; read at the next election's recordEdge to count a gap re-pin.  Empty
        // unless network.phase_datum_eq34_extend_only.
        std::set<std::pair<int, std::string>> eq34_retained_edges;
        // Last epoch's pin target per (station index, "sat_signal") for card 3: the exact
        // same-station pinmove baseline for the shift walk.  It must be keyed per station:
        // an interior satellite carries several pins (its parent edge and one per
        // child-receiver edge), each at a different station with its own target.  An
        // amb-only key collides, and the resulting wrong pinmoves feed back through the
        // carry shifts into the next epoch's rounded targets, which then diverge.
        // Replaced wholesale each election.
        std::map<std::pair<int, std::string>, double> eq34_prev_target;
        // Per-epoch re-reference set (sat, signal) of card 3: cleared and rebuilt at each
        // eq34 election, and read (1) by the local-carry erase right after the election and
        // (2) by the D2/C1 shared-carry consume, which must marginalize the re-referenced
        // PB keys for that epoch.  Their carried priors belong to the retired frame, and
        // folding them pulls the new frame by up to the full re-level with no signal on the
        // wire.  Always empty when eq34 is off.
        std::set<std::pair<std::string, std::string>> eq34_reref;
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
        // Run-scope observability for 1(d), as for eq34_reref_total: families marginalized
        // and phase-leveling local carries erased over the run.
        long long pa_reref_total = 0, pa_reref_erase_total = 0;

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
        std::map<std::pair<int, std::string>, int> anchor_arc_age;
        // Warm-resume seeding latch (spec §2 INC-1, "warm-resume rule").  Consumed by the
        // first hygiene pass of a resumed run, which seeds every present arc at
        // eq34_anchor_min_arc_ep instead of 1; otherwise a resumed leg would block every
        // reattach for min_arc_ep epochs.  Seeded from presence rather than from carried
        // keys on purpose: stacks that run with phase_leveling off have an empty
        // local_carry, so a carry-keyed seed would seed nothing.
        bool anchor_age_resume_seed = false;
        // Bridge source (INC-2): the last solved shared phase bias per "sat_signal",
        // harvested at the D2 stash (kept epochs only, like code_bias_prev_).  Kept apart
        // from prev_carry on purpose: under carry_invariant the shared PhaseBias carry is
        // not stashed at all, so prev_carry would be an empty source there.
        std::map<std::string, double> pb_level_prev;
        /// One open INC-2 bridge: the key's gauge is realized by a soft shared row at
        /// @c value instead of a tree-edge or anchor pin.
        struct PinBridge {
            double value = 0.0;      ///< the held shared phase bias (m), from pb_level_prev at open time
            int    folds = 0;        ///< consecutive epochs this bridge has been folded (staleness bound)
            double opened_tow = 0.0; ///< TOW of the epoch it opened (trace only)
        };
        std::map<std::string, PinBridge> pin_bridge;   ///< "sat_signal" -> open bridge.
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
        std::set<std::string> sbc_active_sats;
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
        // --- flicker/gap signal integrity ---
        // Per-(station index, satellite) band-state history: the vetted reference
        // phase-signal set, which reference bands are currently missing and since when,
        // the last epoch the line of sight was present, and the re-vet counter.  Keyed per
        // line of sight rather than per (satellite, signal) arc, because across a loss of
        // lock every band's cycle counter is arbitrary at once, so trusting one band while
        // demoting the other would make the absence-of-evidence claim this mechanism
        // exists to refuse.
        //
        // Empty unless ctx.knobs.sig_integrity_on, so an off run allocates nothing.  Not
        // serialized: a resumed leg's first epoch sees every LOS as a first sighting and
        // re-vets it, which is the conservative direction, since a resume is a window in
        // which no time-difference detector can operate.  (This is the opposite resume rule
        // from the eq34 arc-age map, which seeds arcs as mature because blocking every
        // reattach for min_arc_ep epochs would do active harm; here the conservative
        // default costs revet_ep epochs of deweighted phase and nothing else.)
        std::map<std::pair<int, std::string>, SignalIntegrityArc> si_arc;
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
        // nothing.  Not serialized, and the exemption is required: the card is not
        // fingerprinted so that an off/on pair can share one snapshot, and persisting an
        // observer's memory would make a forward-only admission policy
        // fingerprint-relevant.  A resumed leg's first pass therefore sees every channel as
        // unknown, which is the case @c adm_resume_pending labels: the arc's state was
        // restored (possibly carrying the corrupt seed) while this observer's memory was
        // not.
        std::map<std::pair<int, std::string>, MeasurementAdmissionArc> adm_arc;
        // This run resumed from a snapshot, so each station's first screened epoch labels a
        // first-sighting failure `adm_reject_resume` (a restored corrupt arc) rather than
        // `adm_reject_birth`.  Written unconditionally by a successful warm resume; inert
        // unless the screen is armed, and byte-identical either way, like
        // @c anchor_age_resume_seed.
        //
        // It is not a one-shot consumed by the first seam pass.  The seam screens only the
        // stations that are present and above the satellite floor, so a station that is
        // absent, thin, or whose satellite the two-station observability gate erased on
        // that epoch would have the flag consumed on its behalf, and every later first
        // sighting there would read as a birth.  The response does not depend on the
        // distinction (the refloat is unconditional), but the event name does.
        bool adm_resume_pending = false;
        /// Station indices whose first post-resume screened epoch has already happened:
        /// the per-station half of @c adm_resume_pending.  Bounded by the station roster;
        /// empty and unread on a cold start.
        std::set<int> adm_resume_seen;
        /// "event|station|sat|band" -> the processed-epoch index of its last emitted
        /// event.  Rate-limits only the two ADMISSION streams that can repeat (the INC-3
        /// census and a chronically bad station's guard event); the INC-1 / INC-2 events
        /// are transitions and never consult it.
        std::map<std::string, long long> adm_event_last;
        // Per-epoch L1 scalars (INC-3).  @c adm_ep_tow is the stamp the epoch-status writer
        // compares against, so an epoch the screen did not run on leaves the columns blank
        // instead of repeating stale numbers (the redun_ep_tow pattern; the ep_dw_engaged
        // pattern does not fit, because its -1 sentinel is never reset and sticks after the
        // first write).
        double adm_ep_tow          = -1.0;
        double ep_adm_sta_worst_z  = 0.0;  ///< median over screened stations of each station's worst |z| (m): the typical level.
        double ep_adm_net_worst_z  = 0.0;  ///< network maximum |z| (m): the extreme.  Its identity goes to the events.

        // Whether the last processed epoch moved the clock datum.  A clock-pivot handover
        // re-references every satellite phase bias by the pivot's clock (the phase
        // observable ties δtˢ + b_s), so a bridge value harvested before it belongs to the
        // retired frame.  Written where datum_changed is computed (solve glue), read by the
        // next epoch's hygiene pass (bridge drop, reason=datum).
        bool last_datum_changed = false;
        /// The constellations whose carried shared state the handover S-transform
        /// re-expressed this epoch (queue item 1(b)).  The RTA-2 handover_affects skips that
        /// consult this set release for exactly these constellations: the prior is valid in
        /// the incoming datum, so it must fold, which is the essential half of the card
        /// (§8.4 INC-1).  Rebuilt every epoch by networkHandoverStransform; empty on every
        /// non-handover epoch and whenever the knob is off, so the off path is
        /// byte-identical.
        std::set<char> sts_consts;
        /// The coefficient applied per constellation this epoch (constellation char -> c0,
        /// metres), for the PIVOT event row and traces (queue item 1(b)).
        std::map<char, double> sts_c0;
        /// Card SCL B2 (network.handover_stransform_dense): what the dense kernel did for each
        /// constellation it re-expressed this epoch, read by card SCL's clone fold (B1) and by
        /// card Q3's anchor keep.  Rebuilt every epoch by networkHandoverStransform (cleared
        /// beside sts_c0, and only an applied dense edge writes an entry), so it is empty on
        /// every epoch without a dense edge and whenever the knob is off.
        struct StsDense {
            std::string q;                    ///< the incoming pivot
            std::string p;                    ///< the outgoing pivot (the frame the block was in)
            double      c0{0.0};              ///< the incoming pivot's carried level before (m)
            double      pqq{0.0};             ///< its block variance before, P_qq (m^2)
            std::size_t k_old{0};             ///< block size before the outgoing pivot was appended
            int         iq{-1};               ///< the incoming pivot's block row
            bool        p_added{false};       ///< the outgoing pivot was appended
            bool        selfcheck_ok{true};   ///< the kernel's self-check
            std::set<std::string> row_sats;   ///< the constellation's clocks with a block row (q included)
            std::set<std::string> clean;      ///< ...whose carried entry equalled its fold decision before the transform
        };
        std::map<char, StsDense> sts_dense;
        /// Card PRM phase 2 (network.handover_stransform_before_rows): what the handover
        /// preview before PASS B did for each changed constellation this epoch, read by the
        /// solve glue's transform for its bookkeeping.  Rebuilt every epoch by
        /// networkHandoverStransformBeforeRows; empty with the knob off.
        struct StsBeforeRows {
            std::string pivot;              ///< the incoming pivot the preview elected
            bool        already{false};     ///< the carry was already in that pivot's datum (a re-detection after a skipped epoch)
            bool        applied{false};     ///< the transform moved the carry
            double      c0{0.0};            ///< the coefficient applied (m)
            double      var_q{0.0};         ///< the incoming pivot's carried variance (m^2)
            long long   rows{0};            ///< carried clock entries re-expressed
            double      x_q_post{0.0};      ///< the incoming pivot's carried level after it (expected 0)
            bool        selfcheck_ok{true}; ///< the exactness self-check
        };
        std::map<char, StsBeforeRows> sts_pre;
        /// Card PRM phase 2: per constellation, the transform the preview applied to the
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

        // Per-satellite clock sanity monitor (network.clock_monitor): the previous emitted
        // clock value and the continuous-GPS second it was stashed, per satellite, for the
        // adjacent-epoch jump test.  Erased on refloat so the post-reinit clock re-baselines.
        std::map<std::string, std::pair<double, double>> prev_clk;  // sat -> {Δδtˢ (m), seen_s}
        // Two-state oscillator clock state per satellite (network.clock_model): the carried
        // [offset, drift] and 2×2 covariance, persisted across epochs.  Empty and inert
        // unless clk_model_on.
        std::map<std::string, ClockOsc> clock_osc;   // per-sat carried [offset,drift]+cov+t_last (P0-3: t_last drives per-sat prediction/drift-FD across gaps)
        // network.emit_clock_rate: decoupled per-sat rate state, used only to derive the
        // emitted C1 by finite-differencing the posterior solved clock (x0 = previous solved
        // offset, x1 = drift, t_last).  It never feeds a prior or a refloat, so the solution
        // is byte-identical and only clock_c1_m_s changes.  Empty and inert unless
        // emit_clock_rate_on (which is off whenever clk_model_on supplies C1 from clock_osc).
        std::map<std::string, ClockOsc> clock_rate_est;
        // Split-cadence fast-clock anchor (FC-1, network.fast_clock): per-LOS cached lumps
        // and rates and the emitted c0 at the last emitted full solve, rebuilt from scratch
        // at every emit.  Empty and inert unless fast_clock.enable.  Not snapshotted
        // (RTB-7a exemption record): a warm resume re-arms after two fresh emitted full
        // epochs, so a restored anchor never refers to a frame the resumed leg did not
        // emit itself.  The fast tick mutates nothing outside this member; that isolation
        // contract keeps full epochs byte-identical whether or not ticks ran.
        FastClockCarry fast_clock;
        // The broadcast-ephemeris IODE to which each satellite's carried clock state
        // (clock_osc.x0, the D2 clock carry, the clock-monitor jump baseline) is currently
        // referenced.  Advanced at the eph-cutover re-level (before the solve), not
        // deferred to the emit site like prev_pivot: the re-level mutates the carried state
        // whether or not this epoch emits (the post-emit carry updates are skipped on a
        // withhold or solve failure, so the shifted values persist), and deferring the
        // advance would re-detect the same roll next epoch and shift twice.  Empty and
        // inert unless brdc_rts.  Named for the reference rather than the emit because it
        // also advances on withheld epochs.
        std::map<std::string, int> relevel_iode;
        // Sats whose ephemeris rolled but whose prior version was already evicted from the
        // store, so Δ_eph cannot be computed: a forced per-sat discontinuity is queued and
        // lands at the satellite's next actual emit (present/pending split, like
        // ura_retro_sats).  Unreachable in steady state (per-epoch checking makes two rolls
        // between consecutive checks impossible); a defensive path for a snapshot gap or a
        // two-roll corner case.
        std::set<std::string> eph_cutover_disc_pending;
        // Sats whose clock_signal_basis_hold IF-pin reference pair re-latched this epoch
        // (T3c): the emitted δtˢ↔dˢ split moved (a real per-sat clock step), so a per-sat
        // disc bump is forced at the next emit (present/pending split, like
        // eph_cutover_disc_pending).  Populated only under clock_signal_basis_hold &&
        // emit_clock_disc, so the output is byte-identical otherwise.  (The hold-off
        // deterministic re-selection uses the emit-only this_epoch_if_pin_sel /
        // last_if_pin_sel path, not this queue; see the comment near held_clock_ref.)
        std::set<std::string> basis_relatch_disc_pending;
        // The code-bias datum's previous-epoch solved values, keyed
        // "<constellation>|<signal>" -> satellite -> value.  The veteran-referenced
        // sum-to-zero RHS and the IF-pin RHS read it at gauge construction; it is cleared
        // and rebuilt from the solved shared vector at the D2 harvest (kept epochs only, so
        // a departed satellite is dropped and its return re-seeds that satellite alone).
        // Snapshotted only under network.state_snapshot_datum_capture and restored only
        // under network.resume_datum_restore (RS-3); otherwise a warm resume re-seeds,
        // with a signal.
        std::map<std::string, std::map<std::string, double>> code_bias_prev_;
        // The IF-pin reference pair (sat -> {sig1, sig2}) that produced the values in
        // code_bias_prev_, with the same kept-epoch lifecycle (rebuilt at the D2 harvest
        // from the scratch below).  A pair change between kept epochs re-references the
        // emitted clock↔code split by the satellite's inter-channel bias (the band-1
        // C1C↔C1W flip), so the IF-pin RHS re-seeds and the disc bumps even when both of
        // the new pair's signals have previous values.  Snapshotted and restored with
        // code_bias_prev_ (RS-3 datum capture and restore only).
        std::map<std::string, std::pair<std::string, std::string>> code_bias_ifpin_pair_;
        // This epoch's selected IF-pin pair per satellite (valid pairs only), captured in
        // the main pass and copied into code_bias_ifpin_pair_ at the harvest (kept epochs
        // only, as for code_bias_prev_, so a pair recorded on a withheld epoch never
        // advances the comparison baseline).  Cleared each epoch.
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_cb_ifpin;
        // Satellites that re-seed this epoch (IF-pin split or a cold sum-to-zero channel).
        // A per-epoch scratch (cleared each epoch), populated in the main pass and merged
        // into cb_reseed_disc_pending only at the emit site, which every whole-epoch
        // withhold path `continue`s before reaching.  A transient re-seed on an epoch that
        // is then withheld (the frame reverts by the next kept epoch) therefore never
        // enters the pending set and cannot fire a bump with no emitted value step (the
        // emit-only discipline of this_epoch_if_pin_sel / last_if_pin_sel), and no erase is
        // needed that would destroy a bump legitimately deferred for a satellite re-seeded
        // while pruned from the emission.
        std::set<std::string> this_epoch_cb_reseed;
        // Satellites whose code-bias split has ever been harvested: a later re-seed of one
        // of these is a return (disc bump), not a first appearance (seeded silently).
        // Accumulates for the whole process.  Snapshotted and restored with
        // code_bias_prev_ (RS-3 datum capture and restore only).
        std::set<std::string> code_bias_sat_seen_;
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
        std::set<std::string> cb_reseed_disc_pending;
        double start_anchor_s = -1.0;                        // first-epoch time anchor for network.start_offset_hours
        // Phase-leveling per-station ambiguity carry (increment 1): persistent across
        // epochs, keyed by (station index, sat, signal).  Ordered, so the inject order
        // (which feeds rank-1 LDLᵀ updates) is byte-identical serial and parallel.  Inert
        // unless network.phase_leveling.
        std::map<std::tuple<int, std::string, std::string>, LocalCarry> local_carry;
        /// Continuous-GPS-seconds stamp of the previous harvest, i.e. the value
        /// @c LocalCarry::last_seen_s carries for an entry stashed one processed epoch
        /// ago.  Written at the end of every harvest; -1 before the first.  The §7.3 lane
        /// observatory uses it to require that a pre-event carry is exactly one epoch old,
        /// so a stale level is never differenced against a one-epoch ΔMW.
        double pl_prev_now_s{-1.0};
        /// The previous receiver-clock harvest's stamp (s), for RX-CLK-in-block.  A second
        /// field rather than a read of @c pl_prev_now_s, because that one has already been
        /// advanced to this epoch by the time the clock harvest runs.  Used only to convert
        /// a sighting gap into epochs for the per-epoch q.
        double rxclk_prev_now_s{-1.0};
        /// T4-side pre-erase capture for the §7.6(d) lane observatory: the
        /// @c LocalCarry::value_abs about to be erased (with its @c last_seen_s stamp and
        /// the erase-cause bit) at the RAIM attribution and consensus erase sites, keyed
        /// like @c local_carry.  Measure-only, armed with the observatory knob.  Lifetime
        /// is one epoch: filled at SB_RAIM of epoch t (post-harvest), consumed and cleared
        /// at epoch t+1's harvest (the bound invariant #1 puts on what a decoupled capture
        /// may carry; a missed epoch drops the capture rather than measuring across an
        /// unequal interval).  Not snapshotted.  Unlike the §7.3 detector path, whose pre
        /// level lives in @c local_carry and is therefore gauge-current by construction,
        /// this is a copy across one epoch boundary: the consumer must skip pairs whose
        /// (sat, σ) appears in this epoch's @c eq34_reref set (n_lane_t4_skip_gauge).
        struct LaneT4Pre {
            double value_abs{0.0};
            double stamp_s{-1.0};
            std::uint8_t cause{0};   ///< RelevelEpochState::kCauseT4Attrib / kCauseT4Consens.
        };
        std::map<std::tuple<int, std::string, std::string>, LaneT4Pre> lane_t4_pre;
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
        /// The held S-basis ledger (RF-2 item 1): ambiguity key -> the frozen datum
        /// (@ref PhaseBasisEntry).  Written once per anchor tenure by the election and read
        /// by it on every later epoch of that tenure, so the pinned value is a constant
        /// rather than something re-derived from a moving carry.  Empty on every run
        /// without network.phase_datum_held_basis.
        std::map<std::string, PhaseBasisEntry> phase_basis;
        /// (station idx, ambiguity key) -> the loop epoch of that arc's last screen flag: a
        /// slip response (the latched free set), a T4 / T3 refloat, or a gross-LOS
        /// declaration (RF-2 items 3 and 4).  The election's quiet-window gate reads it to
        /// refuse the basis to a recently flagged candidate, and the sticky branch reads it
        /// to depose a basis holder that has just been flagged, so the handover happens
        /// before the response acts on that column (item 4).  Written only when the held
        /// basis is armed.
        std::map<std::pair<int, std::string>, long long> amb_flag_ep;
        /// Sim-only oracle slip response (network.cycle_slip.oracle_relevel,
        /// include/slip/NetworkSlipOracle.hpp): the run-scope truth table, the per-arc
        /// application ledger and the run-end counters.  Null on every run where the knob
        /// is off: the four detector actuation sites take a `NetworkSlipOracle*` and never
        /// reach a call, so the off path is structurally byte-identical (standing invariant
        /// #6).  Owns a raw pointer back to @c local_carry (a sibling member of this
        /// struct, so it outlives the oracle); set at construction.  Not snapshotted: a
        /// diagnostic, and a resumed leg re-reads the fixture anyway.
        std::unique_ptr<NetworkSlipOracle> slip_oracle;
        /// Sim-only oracle ambiguity pin (`network.inject.events[] kind
        /// "oracle_pin"`, include/filter/NetworkOraclePin.hpp): the run-scope truth table,
        /// the parsed scopes, this epoch's gauge-consistent pin table and the run-end
        /// counters.  Null on every run where no `oracle_pin` event is armed; the one
        /// serial call site and the PASS-B row both test the pointer first, so the off
        /// path is structurally byte-identical (standing invariant #6).  Lives beside
        /// @ref slip_oracle because both are truth-fed diagnostics keyed on (station index,
        /// satellite, signal).  Not snapshotted: a diagnostic, and a resumed leg re-reads
        /// the fixture anyway.
        std::unique_ptr<NetworkOraclePin> oracle_pin;
        // Per-station ZWD carry (B9): persistent across epochs, keyed by station index (one
        // ZWD per station).  Solve-inert unless network.carry_zwd (every-epoch inject) or a
        // live zwd_seam_ep_left window (post-resume inject).  Also kept fresh (capture
        // only, no inject) on any snapshot-armed run (knobs.zwd_capture), so every RTB-7
        // snapshot serializes Z rows regardless of carry_zwd.
        std::map<int, LocalCarry> zwd_carry;
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
        /// processed epochs remaining during which the card-4 D5 innovation gate is
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
        /// @c anchor_age_resume_seed / @c adm_resume_pending).  If the first processed
        /// epochs are withheld, the window extends to the first emitted epoch, the right
        /// boundary for a consumer-facing readout, and the rerefs accumulate rather than
        /// being overwritten.  Never serialized: each resume is its own seam.
        bool resume_datum_first_ep{false};
        /// One-shot phase-bias seam latch (RS-4, SPEC_event_harness §21.9).  Set by
        /// @ref networkTryWarmResume beside @ref resume_datum_first_ep on every warm resume
        /// and consumed by the first emit in NetworkEmitStore, where the emitter's restored
        /// per-key baselines are still the previous run's values.  Read only under
        /// @c knobs.resume_pb_seam_mark, so an unarmed run is byte-identical.  Never
        /// serialized: each resume is its own seam, and a leg that resumes twice has two
        /// seams.
        bool resume_pb_seam_pending{false};
        /// One (satellite, signal) key's emit-side phase-bias hold state (card PBH).
        struct PbHoldKey {
            double        held_m{0.0};       ///< The value every emission of this generation writes.
            std::uint32_t generation{0};     ///< 1-based; 0 never appears on a live entry.
            double        last_emit_s{0.0};  ///< Product content time (s) of this key's last emission.
            /// This (satellite, signal)'s accumulated level shift (@c ClockLevelSat::xfer)
            /// at this generation's start (card CLV).  The held emission is
            /// @c held_m + (shift - level_ref), so the wire's phase side does not move when
            /// the level is re-based.  0 and unread unless network.clock_level_carry.
            double        level_ref{0.0};
            /// Card LAH (@c network.emit_pb_hold_phase_clock): the satellite's
            /// common-removed emitted clock @f$\tilde c = c - M@f$ at this generation's
            /// start.  The held emission is @c held_m + (c~(t) - frz_ref).  0 and unread
            /// unless the mode is armed.
            double        frz_ref{0.0};
            /// Card LAH: false when the generation started on an epoch without an emitted
            /// clock for the satellite (no reference; the key is held plainly).
            bool          frz_ok{false};
            /// Card LAH: the transfer written at this key's last held emission (m), reused
            /// on an epoch without an emitted clock for the satellite.
            double        frz_last{0.0};
            /// Card SMH: the key's clone died this generation of a cause other than the
            /// anchor's withdrawal or disengagement, and the key carries card LAH's
            /// transfer from the switch on for the rest of the generation.
            bool          smh_reverted{false};
            /// Card SMH: the transfer written at this key's last held emission (m).
            double        smh_last{0.0};
            /// Card SMH: a clone left before LAH had a reference (a generation started
            /// without an emitted clock), so LAH's reference waits for the satellite's next
            /// emitted clock and starts from the last transfer (continuity of the transfer).
            /// With a reference, leaving a clone folds its passed motion into it instead
            /// (card SMD section 7).
            bool          smh_ref_pending{false};
            /// Card SMH: the key's last held emission wrote its clone's transfer (so a
            /// clone that is not live now is a switch to the bridge).
            bool          smh_on_clone{false};
            /// Card SMH: LAH's transfer at the key's last smoothed emission (m), the frame
            /// the clone's transfer is written in (LAH's minus the passed motion), reused
            /// on an epoch without an emitted clock for the satellite (section 10).
            double        smh_lah_last{0.0};
        };
        /// "<sat>|<signal>" -> the held value and its generation (card PBH,
        /// @c network.emit_pb_hold).  Owned here rather than in the seam because it must
        /// survive the epoch loop; empty and never touched when the knob is off.  Not
        /// snapshotted: a warm resume cold-starts every key as a first sighting, which
        /// advances the generation the consumer sees and so re-floats instead of silently
        /// re-seating a stale held value (the conservative direction, as for the slant-iono
        /// carry below).
        std::map<std::string, PbHoldKey> pb_hold;
        /// |estimate - held| (m) over every held emission (card PBH): the cost of the
        /// decoupling edge, measured rather than asserted (invariant #7).  Read at run end
        /// for p50 / p90 / max; empty on an off run.
        std::vector<double> pb_hold_dev_abs_m;
        /// The de-commoned satellite clock step (m) at every epoch where a mark naming both
        /// the clock and the phase-bias channel reaches a held key (card PBH).  This is the
        /// one move the hold does not transfer, on purpose, so its size is the measured
        /// cost of the identity-passing contract; empty on an off run.
        std::vector<double> pb_hold_coupled_step_m;
        /// Satellite -> the clock (m) this producer emitted at that satellite's previous
        /// emission: the baseline the coupled-mark step above is differenced against (card
        /// PBH).  Maintained only while the hold is armed.
        std::map<std::string, double> pb_hold_prev_clock_m;
        /// Card LAH: constellation letter -> M, the accumulated common clock step (m): the
        /// sum over emitted epochs of the median, over the constellation's satellites with
        /// an emitted clock at both that epoch and the previous emitted epoch, of the
        /// emitted clock step.  Empty and untouched unless network.emit_pb_hold_phase_clock.
        std::map<char, double> pb_hold_frz_M;
        /// Card LAH: satellite -> emitted clock (m) at the previous emitted epoch, holding
        /// only that epoch's satellites (a satellite absent there contributes no step).
        std::map<std::string, double> pb_hold_frz_prev_clk;
        /// Card LAH: |transfer step| (m) per held emission, the per-epoch move the mode
        /// writes on top of the held value (invariant #7, the decoupling edge's rate).
        std::vector<double> pb_hold_frz_step_abs_m;
        /// Card LAH: |transfer| (m) per held emission, the accumulated move since the
        /// generation start.
        std::vector<double> pb_hold_frz_total_abs_m;
        /// Card SMH: hold key ("sat|signal") -> its clock clone (network.emit_pb_hold_smoothed).
        /// A clone lives for its key's generation; a dead one stays until the next
        /// generation replaces it.  Empty and untouched unless the mode is armed.
        std::map<std::string, PbHoldClone> smh_clone;
        /// Card SMH: hold keys whose generation started at this epoch's emit, born at
        /// this epoch's stash (sat, signal and generation filled in).
        std::map<std::string, PbHoldClone> smh_pending;
        /// Card SMH: continuous GPS seconds of the first held emission (the hour basis).
        double smh_t_first_s{-1.0};
        /// Card SMD: hold key ("sat|signal") -> its block clone (a clock row and the
        /// phase-visible row on one lifetime).  Carried whenever the smoothed hold is armed;
        /// the wire reads it under network.emit_pb_hold_smoothed_block.
        std::map<std::string, PbHoldClone> smh_block;
        /// Card SMD: hold keys whose block clone waits for a birth (as @c smh_pending).
        std::map<std::string, PbHoldClone> smh_block_pending;
        /// Per-LOS slant-iono carry (QUEUE 8b): persistent across epochs, keyed (station
        /// index, sat).  Diagonal only (the rx-CB rationale: no shared-border footprint;
        /// the coupling variant is deferred).  Not snapshotted: a warm resume cold-starts
        /// it, which degrades to one epoch of white-iono behaviour (the conservative
        /// direction, as for relevel_pend).  The knob is conditionally fingerprinted (it
        /// affects the solve; never flip it mid-chain).  Inert unless network.carry_iono.
        std::map<std::pair<int, std::string>, LocalCarry> iono_carry;
        /// I-in-block per-LOS slant-iono carry (@c network.carry_iono_in_block), with the
        /// same (station index, sat) key as @c iono_carry.  A separate map on purpose: the
        /// two vehicles are mutually exclusive (the loader refuses a co-arm), and a shared
        /// map would let the QUEUE-8(b) prune, prires and CS-GF paths, all gated on
        /// @c knobs.iono_on, act on in-block entries.  Unlike @c iono_carry, these entries
        /// carry the F3 bookkeeping (@c blk / @c blk_idx / @c coupling / @c birth_s),
        /// because they are folded as columns of the station's whitened block like a
        /// carried ambiguity.  The value is the absolute slant delay in metres at L1
        /// (x_lin + x_r).  Not snapshotted: a warm resume cold-starts it, which is one
        /// epoch of white-iono behaviour (as for @c iono_carry), and a restored entry would
        /// carry no block anyway.
        std::map<std::pair<int, std::string>, LocalCarry> iono_blk_carry;
        /// The gross line-of-sight dispositions (card GL, docs/SPEC_event_harness.md §24.18),
        /// keyed (station index, satellite): the same key as the iono carries above, on
        /// purpose, because a gross LOS's response reaches exactly those entries.  Holds
        /// both the streaks of every screened line of sight and the declared subset; the
        /// declared subset is what @ref networkGrossLosPublishScales turns into the
        /// assembly's code-row σ scales.  Entries are pruned on the absent-satellite edge
        /// (@c network.gross_los_max_gap_s), so the map tracks the live LOS population
        /// instead of growing monotonically.  Empty unless @c knobs.gl_on.
        ///
        /// Not snapshotted and not fingerprinted: the state is one screen's short-horizon
        /// evidence (two epochs of streak), not a carried estimate, so a warm resume
        /// re-earns it within @c gross_los_epochs epochs, and a snapshot written with the
        /// screen off stays readable with it on.
        std::map<std::pair<int, std::string>, GrossLosState> gl_state;
        /// Satellites currently stood down (GL-2, §24.19): their per-LOS declarations are
        /// refused and they are routed to the consensus-fault response instead.  Empty
        /// unless @c knobs.gl2_on.  Not snapshotted and not fingerprinted, for the same
        /// reason as @ref gl_state: it is one epoch of evidence, re-earned at once.
        std::set<std::string> gl2_sat_down;
        /// TOW at which each stood-down satellite's edge fired, for the release event's detail (GL-2).
        std::map<std::string, double> gl2_sat_since;
        /// Satellites to prune from the next emission (GL-2), drained by @c networkSolPrep
        /// where the RT0-5 runaway quarantine and the RTA-10 per-constellation carve-out
        /// prune theirs.  Rebuilt every screened epoch, so a released satellite stops being
        /// withheld in the same pass that a newly faulted one starts.  Empty when
        /// @c network.gross_los_sat_wire_withhold is off.
        std::set<std::string> gl2_wire_pending;
        /// Per-(station, satellite) last event sequence, for
        /// @c network.gross_los_event_holdoff_ep (card GL).  Empty at the default holdoff 0.
        std::map<std::string, long long> gl_event_last;
        /// Screened-epoch sequence number, the holdoff's clock (card GL).
        long long gl_seq{ 0 };
        /// The held carried ambiguity levels of the declared lines of sight (RF-3b B2,
        /// @c network.gross_los_bound), keyed (station index, satellite, signal) like
        /// @ref local_carry.  Written once, at the declaration, from the pre-declaration
        /// entry with its border coupling and joint block cleared; re-applied over
        /// @ref local_carry after every harvest for as long as the declaration stands;
        /// erased at the release or the expiry.  Empty unless @c knobs.gl_bound.
        std::map<std::tuple<int, std::string, std::string>, LocalCarry> gl_hold_amb;
        /// The held per-LOS iono carries (RF-3b B2), keyed (station index, satellite) like
        /// @ref iono_carry.  Same lifecycle as @ref gl_hold_amb.
        std::map<std::pair<int, std::string>, LocalCarry> gl_hold_iono;
        /// The held in-block per-LOS iono carries, the twin of @ref iono_blk_carry (RF-3b B2).
        std::map<std::pair<int, std::string>, LocalCarry> gl_hold_iono_blk;
        /// The station-level dispositions (RF-3b B4, @c network.gross_los_station_tier),
        /// keyed by station index.  @c gross means every one of that station's code rows
        /// folds at the deweight; @c sats is the satellite set the last screened epoch saw,
        /// which @ref networkGrossLosPublishScales turns into per-satellite scales (the
        /// assembly's map is per satellite, so a station-wide fold is expressed as "every
        /// satellite this station tracks").  Empty unless @c knobs.gl_station_tier.
        struct GrossLosStationState {
            bool   gross{ false };       ///< The station is declared.
            int    clear_streak{ 0 };    ///< Consecutive screened epochs at or under the cap while declared.
            double declared_tow{ 0.0 };  ///< TOW of the standing declaration.
            long long n_declared{ 0 };   ///< Declarations over the run for this station.
            std::set<std::string> sats;  ///< Satellites the station was last seen screening.
        };
        std::map<int, GrossLosStationState> gl_sta_state;
        /// Satellites whose records this epoch's emission must withhold because a declared
        /// line of sight took their contributing station count below
        /// @c network.gross_los_sat_min_stations (RF-3b B3, @c network.gross_los_emit_exclude).
        /// Drained by @c networkSolPrep beside the GL-2 prune.  Rebuilt every screened epoch.
        std::set<std::string> gl_emit_withhold;
        /// Satellites this epoch's emission must mark with a per-satellite discontinuity
        /// because a declared line of sight left their contributor set while enough
        /// stations remained to emit (RF-3b B3).  Drained by @c networkSolPrep.
        std::set<std::string> gl_emit_disc;
        /// The lines of sight the decision half declared this epoch, deferred to the act
        /// half, which must run after the carry harvest (RF-3b B3).  Rebuilt every screened
        /// epoch; empty unless @c knobs.gl_emit_exclude split the seam in two.
        std::vector<std::pair<int, std::string>> gl_act_declare;
        /// The lines of sight the decision half released or expired this epoch; the act
        /// half drops their hold (RF-3b B3).  Same lifecycle as @ref gl_act_declare.
        std::vector<std::pair<int, std::string>> gl_act_release;
        /// The stations the decision half declared (@c true) or released (@c false) this
        /// epoch, deferred to the act half (RF-3b B3/B4).
        std::vector<std::pair<int, bool>> gl_act_station;
        /// The carried receiver-clock-family local columns for RX-CLK-in-block
        /// (@c network.carry_rx_clock_in_block), keyed (station index, kind), with kind ""
        /// for the primary receiver clock and the system name for each ISB.  The value is
        /// the absolute clock/ISB in metres (x_lin + x_r) as of @c last_seen_s, with the
        /// F3 bookkeeping (@c blk / @c blk_idx / @c coupling) that every in-block column
        /// carries.  Not snapshotted: a warm resume cold-starts it, which is one epoch of
        /// white-clock behaviour, and a restored entry would carry no block anyway.
        std::map<std::pair<int, std::string>, LocalCarry> rx_clk_carry;
        /// The drift half of the carried (bias, drift) pair for RX-CLK-in-block, keyed like
        /// @c rx_clk_carry but populated only for the primary clock (kind ""); an ISB is a
        /// near-constant bias and takes a pure random walk.  Kept in its own map rather
        /// than added to @c LocalCarry, because @c LocalCarry is shared machinery used by
        /// the other carries, none of which has a rate.
        ///
        /// This state exists because the network producer never writes its solution back
        /// into the station StateManager: @c x_lin(clock) is set once (0, or the latched
        /// cold_clock_seed median) and is constant for the run, so @c x_r carries the whole
        /// clock including its ramp of about 0.3 m/s (9 m per 30 s epoch, about 30 sigma
        /// against a bias-only prior).  The rate is a scalar Kalman state driven by the
        /// harvested increments; it also supplies the one-step prediction whose innovation
        /// is the clock-jump detector.
        struct RxClkRate {
            double rate{0.0};          ///< Estimated drift, m/s (GPS-referenced, same frame as the clock state).
            double rate_var{-1.0};     ///< Its variance, m^2/s^2.  < 0 = never updated, so the injector uses network.carry_rx_clock_rate_var0_m2ps2.
            double prev_abs{0.0};      ///< Previous harvested absolute clock (m): the increment's left end.
            double prev_s{-1.0};       ///< Its time (s).  < 0 = no previous sighting.
            double prev_var{0.0};      ///< Its harvested variance (m^2), for the increment's measurement noise.
            long long n_incr{0};       ///< Increments folded.  0 = no rate yet, so the jump gate cannot fire (there is no prediction to test).
        };
        std::map<std::pair<int, std::string>, RxClkRate> rx_clk_rate;
        /// CS-GF authority states (successor to QUEUE 8b; iono_carry_source =
        /// smoothed_gf): per (station index, sat) carrier-smoothed GF-code slant-iono,
        /// advanced serially in the PASS-A slip scan from the qualified raw verdicts.
        /// Level-independent evidence: the T4 erase edge does not touch it, on purpose (it
        /// erases the carry, and the next harvest re-stashes from here).  Scratch that
        /// rebuilds itself; not snapshotted (the RawSlipHistory rationale).  Bounded at
        /// (n_stations × visible sats), so no pruning is needed.
        std::map<std::pair<int, std::string>, IonoCsState> iono_cs;
        // Per-(station index, "sys_RPB_σ") receiver phase-bias carry for card 3 (eq34):
        // local time-linking of δ_{r,σ} (spec D6 v2 in its minimal local form, with no
        // border promotion).  Re-estimating the RPB fresh each epoch injects
        // anti-correlated per-epoch wander into every individual ambiguity float of that
        // (station, σ): DDs cancel it exactly, but the per-key phase-leveling carry does
        // not, which damages the datum-invariant DD steps.  Read-only during the parallel
        // matrix build (fold), written at the serial post-solve stash, erased for dirty
        // receiver vertices at the election (their chain frame stepped), and pruned on gap
        // like local_carry.  Inert unless network.phase_datum_eq34 with
        // phase_datum_eq34_rpb_q_m2s > 0.
        std::map<std::pair<int, std::string>, LocalCarry> rpb_carry;
        /// rx-CB carry: carried receiver code-bias posteriors, keyed (station index, CB
        /// state key).  Harvested outside the carry_warm bracket, under its own gate, since
        /// it must work on the carry_invariant / no-leveling lineage.  Inert unless
        /// network.carry_rx_code_bias.
        std::map<std::pair<int, std::string>, LocalCarry> rx_cb_carry;
        /// The per-line-of-sight phase nuisance carry (card LGM section 4.1): (station,
        /// satellite) -> the harvested estimate (value_abs, the linearization point being
        /// 0), its conditional variance, its border regression and its row of the station's
        /// harvest block.  Folded the next epoch as a predicted copy (value and regression
        /// times phi = exp(-gap / tau), variance phi^2 var + sigma(e)^2 (1 - phi^2)).
        /// Written at the harvest, read-only in PASS B, erased by the RAIM attribution and
        /// gross line-of-sight erases, pruned past network.phase_colour_max_gap_s.  Not
        /// snapshotted: a warm resume cold-starts it (its fingerprint token refuses a
        /// resume across a flip of the knob).  Empty when the knob is off.
        std::map<std::pair<int, std::string>, LocalCarry> pc_carry;
        /// Card LGM: formatted rows of network_phase_colour.csv buffered at the harvest
        /// and written by the driver after it (empty unless network.trace_phase_colour).
        std::vector<std::string> pc_trace_rows;
        /// Card AIL: formatted rows of network_amb_if.csv buffered at the harvest and
        /// written by the driver after it (empty unless network.trace_amb_if).
        std::vector<std::string> amb_if_rows;
        // rx-CB carry run counters (armed-only run-end line).
        long long rx_cb_injected_total = 0, rx_cb_innov_skips_total = 0,
                  rx_cb_harvested_total = 0;
        /// Per-station fold/withhold tally of the rx-CB carry (RS-9 rider, §21.18), kept on
        /// every run where the carry is armed; trace-class, with byte-identical products.
        /// It shows what the pooled "N folded, M withheld" line hides.  Keyed by station
        /// name (the teardown has no station roster); value = {station-epochs with at least
        /// one rx-CB carry decision, station-epochs in which every decision was a
        /// withhold}.  A station whose second number is > 90 % of its first carried nothing
        /// for the run, while the run-end line still counted it among the "N carried" (a
        /// blind spot of the kind standing invariant #4 names, measured by the RS-7b
        /// census).  §21.18(b) gives the cause:
        /// the harvest overwrites a withheld entry unconditionally and the prune drops only
        /// entries older than 900 s, so a refused station is refreshed and refused for the
        /// whole run.  Written serially at the post-solve harvest from the produce phase's
        /// per-station StationAssembly tallies, so it is race-free like the counters beside
        /// it.
        std::map<std::string, std::pair<long long, long long>> rx_cb_sta_tally;
        /// The snapshot's RL rows for the RS-9 restore (§21.18): station roster index ->
        /// (CB state key -> the writer's @c x_lin, metres).  Stashed by
        /// @c networkTryWarmResume beside the RS-6 RC restore and consumed once, by
        /// @c networkApplyRxCbLinRestore, which installs each station's map on its own
        /// @c StateInitializer before the first epoch.  Empty on every other run.
        std::map<int, std::map<std::string, double>> rxcb_lin_restore;
        /// Per-station out-slots for the two RS-9 override tallies, sized once by
        /// @c networkApplyRxCbLinRestore and never resized: each station's
        /// @c StateInitializer holds a raw pointer to its own element, so the counting is
        /// race-free inside the OpenMP parallel produce phase (distinct objects, no
        /// atomics).  {applied, uncovered}; summed at the run-end readout.
        std::vector<std::pair<long long, long long>> rxcb_lin_sta_counts;
        /// The information-weighted mean carry innovation μ = Σλ·innov / Σλ (card LV-2),
        /// keyed by receiver code-bias state key ("GPS_DCB", "GPS_IFB5", …): the
        /// constellation is in the key, and the DCB and IFB families are kept apart
        /// because their innovations are different physical quantities.  Computed at the
        /// serial post-solve harvest from the sums the parallel produce phase accumulated,
        /// and consumed with a one-epoch lag by the next epoch's inject (the same race-free
        /// split the carry itself uses: produce reads, serial writes).  Empty until the
        /// first harvest, so epoch 1 injects without de-meaning (μ = 0), one epoch of
        /// legacy behaviour.
        std::map<std::string, double> rx_cb_demean_mu;
        /// This epoch's reduction per family, kept only so the caller can trace it (one
        /// `lv2_mu` row per family in network_events.csv, following the spec's "do not
        /// create a new stream for one scalar").  Rebuilt every armed epoch.
        struct RxCbDemeanEpoch {
            double mu{0.0};       ///< the applied μ: this epoch's under LV-5, the next epoch's under lag-1 LV-2.
            double sum_lam{0.0};  ///< Σλ over the rows of this family.
            int    n_sta{0};      ///< contributing stations.
            bool   fresh{false};  ///< false = frozen (below the thin-mean floor).
            double folded_mu{0.0};  ///< diagnostic for LV-5: the folded set's raw common mode, measured after the fact.
        };
        std::map<std::string, RxCbDemeanEpoch> rx_cb_demean_last;
        // Run counters of LV-2 (armed-only run-end line).  `mu_frozen` is the invariant-#7
        // thin-mean event: a mass re-seed or RAIM re-reduce epoch whose folded set is too
        // thin to define the common mode holds μ instead of trusting it.
        long long lv2_mu_epochs = 0, lv2_mu_frozen = 0, lv2_folds_demeaned = 0;
        // Run counters of LV-4 (armed-only run-end line).  `frame_gate_would_skip` is the
        // same-run A/B: what the retired frame-dependent screen would have withheld on
        // exactly these rows, so the comparison needs no second run.
        long long lv4_step_resets = 0, lv4_step_checks = 0,
                  lv4_frame_gate_would_skip = 0, lv4_scale_standdown = 0;
        double    lv4_max_step_z = 0.0;    ///< largest peer-relative z seen (jump-scale readout).
        double    lv4_max_scale_m = 0.0;   ///< largest robust step σ̂ (m): how big "normal" got.
        // Counters of LV-5 (within-epoch μ).  `resid` is the folded set's raw common mode
        // minus the μ that was applied to it: the direct measure of what the correction
        // left behind, and the way to tell a de-mean that works from one that only ran.
        long long lv5_within_epochs = 0, lv5_resid_n = 0;
        double    lv5_resid_sum = 0.0, lv5_max_abs_resid_m = 0.0;
        double    lv2_last_mu_m = 0.0;   ///< most recent μ (any constellation) for the run-end line.
        double    lv2_max_abs_mu_m = 0.0;
        /// This epoch's S-consistent-harvest reduction per family, kept so the caller can
        /// trace it (one `s2h_mu` row per family in network_events.csv, as for LV-2).
        /// Rebuilt every armed epoch.
        struct RxCbHarvestEpoch {
            double mu{0.0};        ///< the subtracted Λ-weighted mean (m).
            double sum_lam{0.0};   ///< Σλ over the voting members.
            int    n{0};           ///< usable family entries.
            int    n_used{0};      ///< votes after the peer screen.
            int    n_screened{0};  ///< screened from the vote (still transformed).
            bool   applied{false}; ///< false = identity transform (thin or degenerate).
        };
        std::map<std::string, RxCbHarvestEpoch> rx_cb_harvest_last;
        // S2H run counters (armed-only run-end line).  `thin_skips` is the invariant-#7
        // decoupling edge: an epoch too thin to define the frame applies nothing (the
        // identity; the set keeps the last applied frame) and is counted, never frozen
        // silently.
        long long s2h_epochs_applied = 0, s2h_thin_skips = 0,
                  s2h_screened_total = 0, s2h_mu_n = 0;
        double    s2h_last_mu_m = 0.0, s2h_max_abs_mu_m = 0.0, s2h_mu_abs_sum = 0.0;
        /// Lag-1 snapshot of the solved SH-VTEC coefficient vector, per active shell in
        /// @c shLayers order (TECU, in @c SphericalHarmonicIonosphere::coeffs() order;
        /// card LV-1).  Written serially post-solve and read by the next epoch's parallel
        /// produce phase to sample the field at each LOS's pierce point, so the seed frame
        /// is one epoch old by construction (the field is quasi-static; the LV-2 de-mean
        /// takes the same lag).  Invalid before the first solve: the reframe then falls
        /// back to the a₀₀ climatology prior mean, which is the designed level authority.
        std::vector<std::vector<double>> sh_field_lag1;
        long long lv1_field_snapshots = 0;   ///< epochs the lag-1 field was refreshed.
        // URA quality channel carried state, B1 (inert unless network.emit_ura).
        double                       ura_sigma0_ewma = 1.0;   ///< network-wide σ̂0 EWMA scaling the emitted URA (one-epoch lag).
        std::map<std::string, int>   ura_refloat_horizon;     ///< sat -> epochs remaining of post-refloat URA inflation.
        // Isolated-arc persistence gate carried state (inert unless
        // network.raim_arc_persist_epochs > 1): (station index, satellite) -> number of
        // consecutive epochs this isolated arc has tripped the attribution interaction
        // gate without yet being refloated.  Reset to 0 (dropped) on any epoch the arc is
        // not flagged, and cleared the epoch it refloats.  Not part of the warm-resume
        // snapshot: a resume re-accrues the streak (a one-time K-epoch delay after resume)
        // and never mis-refloats.
        std::map<std::pair<int, std::string>, int> arc_spike_streak;
        // Retro-bump (B1; P0-2/P1-3): per satellite, tier-independent, edge-triggered.
        // ura_retro_sats = sats a post-emit QC tier flagged last epoch, applied as a
        // per-sat discontinuity at the sat's next actual emit (markSatDiscontinuity): a
        // flagged sat absent from an emitted epoch (RT0-5 quarantine, vanish) stays
        // pending here instead of being consumed without landing.  retro_active = sats
        // currently flagged and already bumped, so a sat that stays flagged during
        // recovery is bumped once (on its rising edge), with no per-constellation reset
        // storm.
        std::set<std::string>        ura_retro_sats;
        std::set<std::string>        retro_active;
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
        std::set<std::string>        datum_reref_pb_sats;
        std::set<std::string>        datum_reref_amb_sats;
        /// Per-epoch state of the slip re-level response (SPEC_slip_relevel_walk §6): the
        /// latched free-epoch set, the deferred MW bridge candidates, and the bridged-band
        /// set for the harvest-side statistics guard.  Filled by the PASS-A serial drains,
        /// read by the PASS-B inject and the post-solve harvest, cleared at the next
        /// epoch's PASS-A entry.  Not snapshotted (one-epoch lifetime; a resumed leg's
        /// first epochs degrade to the path without the response, the conservative
        /// direction, by the same argument as the pending re-reference queues above).
        RelevelEpochState            relevel;
        /// Retap piece (relevel_retap_statistical): per-arc consecutive-retap streak
        /// (key → last continuous-GPS s, streak).  An arc that re-trips
        /// relevel_retap_streak_max times inside the 120 s window falls back to the erase
        /// (a real missed slip that the loose prior cannot absorb).  Also the set of arcs
        /// retapped between one RAIM pass and the next harvest, for the statistics guard
        /// (read and cleared by the next harvest's labeling).  Neither is snapshotted
        /// (forward-only response state; a resume degrades to the erase path, the
        /// conservative direction).
        std::map<std::tuple<int, std::string, std::string>, std::pair<double, int>>
                                     relevel_retap_streak;
        std::set<std::tuple<int, std::string, std::string>> relevel_retapped_prev;
        /// Bridge v2 pend store, keyed (station idx, sat, "sig1_sig2"); persistent across
        /// epochs (the MW integer arrives 1–2 epochs after the T1 flag), swept for expiry
        /// and replacement at the top of the MW drain.  Not snapshotted: a resume drops
        /// outstanding pends, and those slips keep their v1 free re-level (the conservative
        /// direction).
        std::map<std::tuple<int, std::string, std::string>, RelevelPend> relevel_pend;
        /// What the gap re-entry response did to each arc at this epoch (card SID phase 3,
        /// R4), recorded by the response and read by the roster's audit pass, where the
        /// admitted-side census lives.  Cleared at the build seam, so it never describes a
        /// previous epoch.  Written in both states of network.ledger_reentry (unarmed, the
        /// raw tiers record their own outcomes), and only while network.signal_roster is
        /// armed.  Observatory: nothing in the solve reads it.
        ArcReentryOutcomeMap sid_reentry_outcome;
        /// The anchor election's own datum history per (satellite, signal) family (card SID
        /// phase 4, S2 / B1): what each coverage-gate drop edge left behind (the incumbent,
        /// the epoch, the per-station admitted generations) and the last epoch the family
        /// held an incumbent.  The election itself keeps neither, which is why a returning
        /// family otherwise reads as a first sighting.  Not snapshotted and not
        /// fingerprinted: a resumed run has no drop memo, so its first post-resume election
        /// is a real first sighting (the same exemption the roster and the ledger take).
        /// Untouched unless network.ledger_screens.
        AnchorReentryState           sid_anchor_reentry;
        /// The arcs whose leveling carry the gap prune retired at
        /// network.phase_leveling_max_gap_s this epoch (card SID phase 3, R3a).  The prune
        /// runs in the harvest seam, which has neither the trace bundle nor the epoch time,
        /// so the keys are buffered here and the audit pass emits their
        /// `ARC arc_death reason=horizon` rows.  Cleared as it is drained.
        std::vector<ArcLedger::Key> sid_carry_prune_deaths;
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

        // --- WL/UWL zone-2 run-latched sets and D5 tag state (decomposition INC-4: the
        // plain-data tier-1 entities; instances with function-local types, namely the
        // WlInjectPlan/LaneTraceEntry/WlCounters/F6Capture vectors and the lane/layer
        // engines, stay in the run TU until INC-9b) ---
        std::set<std::pair<int, std::string>> d5_ewl_stasat;
        std::set<ArcKey> d5_hold_chords;
        std::set<ArcKey> d5_inject_chords_prev, d5_inject_chords_now;
        // Regroup census of WL-3 (run totals; the equivalence leg reads these).
        long long lane_pair_arcs = 0;           ///< arcs routed into pair groups.
        long long lane_resid_arcs = 0;          ///< primary-band arcs left in residual per-band groups (partner missing/unshared).
        long long lane_pair_tree_mismatch = 0;  ///< pair arcs whose two bands' eq34 tree tags disagreed (band-1's tag governs).
        std::map<std::string, int>                    carry_sat_vertex;  // stable satellite -> vertex id (disjoint high range)
        std::map<std::pair<char, std::string>, int>   carry_group_id;    // (constellation,signal) -> stable group key
        bool carry_glonass_warned = false;   // one-time WARN when a GLONASS ambiguity is excluded (FDMA)
        // Once-per-run warnings for skips that must be loud (UWL-1 §3.1): a constellation
        // whose cycles enter the EWL machinery without a lane tier (untabled under the
        // armed card, or any non-GPS constellation under the legacy digits) must fail
        // loudly, not as a silent pile of skip_missing_mw (SPEC §1 fact 3).
        std::set<char> uwl_warned_cons;
        // Census visibility for WL-3 (once per run per cons; log-only).
        std::set<char> lane_census_logged;
        // v2 (uwl_family_pairs): the run-persistent admissible pair set per constellation.
        // A pair joins when its joint-census count first reaches uwl_family_min_arcs and
        // stays admitted for the run (latched: membership flicker would refloat the family
        // group's carried chords at every flip, the WL-3 micro-pair-group hazard; a latched
        // pair whose channels vanish routes no arcs, which is harmless).
        std::map<char, std::set<std::pair<std::string, std::string>>> lane_admit_pairs;
        std::set<std::pair<char, std::string>> lane_family_logged;   // once-per-pair INFO
        double carry_prev_tow = std::numeric_limits<double>::quiet_NaN();
        long long carry_epochs = 0, carry_born_total = 0, carry_marg_total = 0,
                  carry_merges_total = 0, carry_splits_total = 0, carry_swaps_total = 0,
                  carry_zdim_last = 0, carry_slipkills_total = 0, carry_dropped_total = 0;
        // Arcs the innovation gate flagged in the previous epoch, to be treated as slipped
        // this epoch (the estimation-side fine-slip re-tap runs a post-solve detection, so
        // it feeds the next epoch's slip funnel).  Persists across epochs (the per-epoch
        // carry_slipped_ambs is rebuilt each epoch).  Empty unless carry_innov_gate.
        std::set<std::pair<int, std::string>> carry_innov_pending;
        long long carry_innov_flags_total = 0, carry_innov_retaps_total = 0;
        long long carry_innov_map_miss = 0;   // reverse-map misses from a D5 flag to its ambiguity (should stay 0)
        // The subset of carry_innov_pending whose arc the raw observables measured as
        // continuous at flag time (card-4 D5 v4, network.carry_invariant_innov_retap_only).
        // Same tri-state as the v3 corroboration gate and the network_d5_flags.csv
        // `raw_stepped` column: stepped and unmeasurable arcs are absent on purpose, so
        // they keep the refloat (standing invariant #1).  Drained one epoch later into
        // @ref carry_innov_flat_now, in lockstep with carry_innov_pending.
        std::set<std::pair<int, std::string>> carry_innov_pending_flat;
        // The drained view the DD-prior build reads this epoch: a member of this set that
        // is not also in the epoch's raw funnel subset keeps its carried DD instead of
        // being refloated.  Rebuilt (cleared and filled) at the head of every epoch.
        std::set<std::pair<int, std::string>> carry_innov_flat_now;
        // Carry arcs whose membership kill was suppressed by the above.  The tripwire of
        // invariant #4 for the knob: 0 with it armed means nothing reached it.  Counted per
        // arc per buildCarryDdPriors call, so a within-epoch RAIM re-solve counts its arcs
        // twice (the same convention as the driver's chords_born / slip_kills).
        long long carry_innov_kill_suppressed = 0;

        // --- Per-epoch carry churn (SPEC_day129_frame_guards P1-1) ---
        // The card-4 counters above are run-cumulative: they mirror
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
        long long ep_chord_births = 0, ep_slip_refloats = 0,
                  ep_d5_flags = 0, ep_d5_retaps = 0;
        // Per-epoch observability of the within-epoch RAIM tier (R0), parked here between
        // the solve glue (writer) and the epoch-status emit (reader).  It puts a null
        // sample of σ̂₀_first on disk (a trip event's detail span carries it only on
        // trips), which a corrected quantile needs to be fitted against (QUEUE item 4).
        // It also makes the within-epoch satellite refloats visible in the epoch-status
        // row, where es.n_refloat_sats counts only the clock-couple set, which is empty
        // unless clock_monitor_couple_raim.  Sentinels: < 0 = the tier did not assess this
        // epoch, and the columns stay blank ("unavailable is not 0").
        double      ep_sigma0_first    = -1.0; ///< first-assess σ̂₀ (pre-adaptation)
        int         ep_we_refloat_sats = -1;   ///< whole-sat refloats settled this epoch
        double      ep_we_clk_move_max = -1.0; ///< max |final − primary| clock move (m)
        std::string ep_we_clk_move_sat;        ///< identity of that max (invariant #7)

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

        // --- Slow channel of the declared per-station variance factor (QUEUE item 4 R2):
        // an EWMA over emitted epochs.  The seam runs only after acceptance, so the factor
        // freezes across withholds by construction.  Keyed by station index (the roster is
        // stable).  Not snapshotted: a warm resume re-converges the EWMA over about τ, the
        // conservative direction.
        std::map<int, double>    staf_ewma;     ///< EWMA of the per-epoch factor
        std::map<int, long long> staf_updates;  ///< EWMA update count (declaration warmup)
        std::set<int>            staf_departed; ///< stations currently past the depart edge
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

        // --- Per-epoch iono-deweight state (SPEC_day129_frame_guards P1-2) ---
        // Harvested at the serial NetworkShHealth advance() site (the gate's threading
        // contract forbids reading it from the parallel assembly) and copied into the emit
        // epoch's status row.  n_dw_zero counts stations the gate has driven to
        // w == floor == 0, i.e. whose entire SH block is dropped (the w_s <= 0 early return
        // at ApplicationNetworkAssembly.cpp:1190-1191) and whose per-LOS iono floats: the
        // candidate trigger for a "deweight release wave".
        int    ep_dw_engaged = -1;   // -1 = not harvested this epoch (blank in the trace)
        int    ep_dw_zero    = 0;
        double ep_dw_min_w   = 1.0;
        double ep_dw_mean_w  = 1.0;

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
        long long pf_epochs_total    = 0;   ///< run-end counter (invariant #4: 0 means inert)
        bool      pf_empty_logged    = false;  ///< one-shot "armed but pooled nothing" warn latch
        // Run-end accounting of the signed per-row dump (SPEC_event_harness §24.6,
        // network.trace_postfit_rows): rows written (code and phase), written rows with no
        // elevation, and data rows the pass visited but could attribute to neither family
        // (not written).  Invariant #4 needs all of them: "0 written under an armed trace"
        // must be visible, and a non-zero unattributed count says which rows the dump does
        // not carry.
        long long pf_rows_written      = 0;
        long long pf_rows_code         = 0;   ///< …of which kind=code
        long long pf_rows_phase        = 0;   ///< …of which kind=phase
        long long pf_rows_no_elev      = 0;   ///< …written with an empty elev_deg (station absent from the modeled set, or no satellite on the row)
        long long pf_rows_unattributed = 0;   ///< rows attributed to neither family (the row tag is not an observation: pins, local prior and anchor rows, unclassified rows); counted, not written
        long long pf_rows_no_signal    = 0;   ///< rows written with an empty signal (the row tag named no signal); 0 expected (card OWD, O5)
        // Run-end accounting of the per-station local-block audit (SPEC_event_harness
        // §24.10, network.trace_station_block, card ZP).  Invariant #4 needs all four: "0
        // rows under an armed trace" must be visible, and the two refusal counts say which
        // of the two ways the sweep can find nothing happened: no station reached the
        // audit at all, or the block itself could not be decomposed (a non-finite or
        // empty N_rr).
        long long sb_rows_written     = 0;   ///< station-epoch rows written to network_station_block.csv
        long long sb_epochs_written   = 0;   ///< traced epochs that wrote at least one row
        long long sb_stations_skipped = 0;   ///< station-epochs skipped: not in the solve, no usable assembly, or no local block
        long long sb_decomp_failed    = 0;   ///< station-epochs skipped: N_rr unavailable or its eigen-decomposition did not converge
        std::size_t sb_last_emitted   = 0;   ///< n_emitted of the last audited emission (one row set per emitted epoch, never per solve)

        // --- PREFIT census, per-epoch scalars ---
        // Filled by networkPrefitCensus under network.trace_prefit_census, and copied into
        // the epoch-status row by NetworkStashStatus.  ep_pc_rows == 0 means "not computed
        // this epoch", and the census columns stay blank (the "unavailable is not 0"
        // convention of the P1-2/P1-4 columns).  Indexed by the data-class order of
        // NetRowClass (obs_code, obs_phase, obs_other, iono_sh_row), which is also the
        // trace column order.  Nothing reads them back: the census is a tripwire, not an
        // input.
        double    ep_pc_max_s[kNetDataClasses]{};
        double    ep_pc_p99_s[kNetDataClasses]{};
        long long ep_pc_n_gt_10[kNetDataClasses]{};
        long long ep_pc_n_gt_1000[kNetDataClasses]{};
        long long ep_pc_rows         = 0;   ///< data rows in this epoch's census (0 means blank columns)
        /// H-rider (edge-queue §1.6 item 3a): this epoch's pooled robust prefit σ̂₀ for the
        /// H heartbeat (networkPrefitSigma0, computed pre-solve at the top of
        /// networkSolveGlue).  0.0 = unavailable (empty pool, or the knob is off), the
        /// sentinel the H grammar already defines.
        double    ep_h_sigma0_census = 0.0;
        long long h_sig0_census_nonzero = 0;   ///< run counter: epochs with a usable census σ̂₀ on the H wire (invariant #4)

        // --- Reduced-RHS class attribution (filter/NetworkRhsAttrib) ---
        int rhs_attrib_done = 0;   ///< armed target epochs already dumped (caps network.rhs_attrib_n_ep)

        // --- Spectrum bookkeeping (P1-3) ---
        long long sh_spectrum_epochs      = 0;      ///< traced epochs (also drives the mode-detail stride)
        bool      sh_spectrum_fail_logged = false;  ///< one-shot eigensolve-failure warn latch

        // --- CB-carry event response (SPEC_day129_frame_guards Phase 2.1) ---
        // Not snapshotted (three-place-rule exemption): every member is either a one-epoch
        // handoff (cb_excursion_sats is consumed by the same epoch's stash) or run
        // telemetry.  A cb_excursion_pending_bump that crosses a resume boundary unbumped
        // is the conservative failure: a resumed run re-detects the excursion if it is
        // still present, and a missed bump costs a consumer nothing it did not already
        // live without (the counter is silent by default).  cb_exc_prev_clk rebuilds from
        // the first post-resume epoch, which correctly suppresses a spurious trip on the
        // resume seam itself (no baseline, so no delta and no trip).
        std::map<std::string, double> cb_exc_prev_clk;      ///< sat -> previous epoch's solved clock (m)
        std::set<std::string> cb_excursion_sats;            ///< this epoch's stash: mark these sats' CB carry inflate_q
        std::set<std::string> cb_excursion_pending_bump;    ///< disc-bump these sats' code biases at the next emit
        long long cb_exc_trips           = 0;   ///< epochs the detector tripped
        long long cb_exc_reinits         = 0;   ///< (sat,epoch) CB carries marked for reinit
        long long cb_exc_bumps           = 0;   ///< code-bias discontinuity bumps landed
        long long cb_exc_nonpivot_would  = 0;   ///< (sat,epoch) non-pivot sats a per-satellite rule would have tripped (the D1 counter)
        double    cb_exc_max_median      = 0.0; ///< largest median |Δclock| observed (m): the trip statistic's run maximum
        double    cb_exc_max_median_tow  = -1.0;///< its epoch

        // --- SH-field health gate (SPEC_day129_frame_guards Phase 2.2) ---
        // Not snapshotted (three-place-rule exemption): the gate is emission-only (no
        // carried state changes meaning across it), so a resume re-accrues the out-of-band
        // run within sh_gate_min_epochs, at worst one epoch of conservative extra emission
        // and never a wrong-frame fold.  For the same reason the toggle is not in the
        // resume fingerprint (the raim / clock_runaway_check class, not the cb_excursion
        // class).  The verdict is computed post-solve in NetworkSolveGlue (the only
        // pre-emit seam holding x_s), enforced by early returns in NetworkSolPrep and
        // NetworkEmitStore, and its disposition is stamped by NetworkStashStatus.
        int       sh_gate_oob_run        = 0;     ///< consecutive out-of-band epochs (0 = in band)
        bool      sh_gate_engaged        = false; ///< this epoch's emission verdict (true = withhold)
        long long sh_gate_oob_epochs_total = 0;   ///< run total of out-of-band epochs (handoff §4.2: count epochs, not warnings)
        long long sh_gate_engagements    = 0;     ///< engage transitions over the run
        double    sh_gate_last_a00       = 0.0;   ///< last computed total a00 (TECU; engage/release log detail)
        double    sh_gate_last_rms       = 0.0;   ///< last computed total-field RMS (TECU)

        // --- SH representativeness-error deviation carry of T5/Iono-2 (struct
        // ShDevCarry: filter/NetworkRunTypes.hpp; v2 units = vertical TECU) ---
        std::map<std::string, ShDevCarry> sh_dev_carry;   // key "<station>|<sat>"
        // Iono-2 run-end counters (invariant #4: wiring that is inert shows only through
        // them): warm-prior harvests, cold seeds, C3c-funnel slip resets, and
        // no-information harvest skips (V̂ ≥ P_pred + f²).
        long long shdev_warm_total = 0, shdev_cold_total = 0,
                  shdev_slip_resets_total = 0, shdev_nodata_total = 0;
        // Iono-2 v3 de-mean: epochs the global de-mean applied, station-mean removals, and
        // Σ|g| for the run-end mean |g|.
        long long shdev_demean_epochs_total = 0, shdev_demean_sta_total = 0;
        double    shdev_demean_abs_g_sum = 0.0;
        // Iono-2 v3k: D5-only re-taps that kept the level (variance re-tap) instead of
        // erasing it.  shdev_slip_resets_total then counts actual erases only.
        long long shdev_d5_keeps_total = 0;

        // --- iono-deweight release hygiene ---
        // Every member below stays empty or at its sentinel unless
        // ctx.knobs.dw_hygiene_on, so an off run allocates nothing.  None of it is
        // serialized: the gate itself is not snapshotted either (a resumed leg starts
        // every station at w = 1 with no samples), so there is no carried release state
        // to restore.  The warm-resume shock is bounded instead by INC-2's gate-cold
        // trigger, which makes the spec §6 deferral explicit.
        //
        // INC-1 ambiguity refloats detected at the release edge (post-solve, SB_T1A),
        // queued for the next epoch's slip funnel.  The funnel `carry_slipped_ambs` is a
        // per-epoch driver local whose consumer (buildCarryDdPriors) has already run by
        // then, and a bare markAmbiguityPendingReseed issued at SB_T1A is wiped a few lines
        // later by the leveling harvest's clearPendingReseed, so the response is deferred,
        // as the card-4 D5 innovation gate defers its own post-solve detections through
        // carry_innov_pending.
        std::set<std::pair<int, std::string>> dw_release_pending;

        // --- RAIM pool re-host ---
        // (station index, ambiguity key) flagged by the re-hosted post-fit RAIM screen at
        // SB_RAIM, queued for the next epoch's slip funnel: the same deferral as
        // `dw_release_pending` above, for the same reason (`carry_slipped_ambs` is a
        // per-epoch driver local whose consumer, buildCarryDdPriors at SB_DDBUILD, has
        // already run by the time RAIM detects post-solve).
        //
        // A queue rather than the seam's own actions, because neither mutation that
        // networkPostFitRaim performs can move the solve:
        //   - `local_carry.erase` drops a phase-leveling carry entry whose only solve-side
        //     consumer is the PASS-B inject, itself under `!carry_invariant`, and on this
        //     lineage that map is empty anyway.
        //   - `markAmbiguityPendingReseed` does reach a live reader (the PASS-B state
        //     seed), but rewriting x_lin is bookkeeping here: the kernel solves
        //     corrections about the linearization point with no write-back, so
        //     b_red = u_s − N_sr N_rr⁻¹ u_r is algebraically unchanged.
        // Only the funnel actuates: it excludes the arc from A_old by membership, so its
        // chord re-taps from a diffuse prior.
        //
        // Drained into `carry_slipped_ambs`, not `carry_slipped_raw`, on purpose: raw
        // means a raw-observable step, which the deviation-erase classifier and the MW-row
        // slip-skip both read.  A post-fit residual consensus is estimation-side evidence,
        // like the D5 innovation gate, which also drains into the funnel only.
        //
        // The lifetime is bounded to exactly one epoch by an unconditional drain-and-clear
        // at the driver head (standing invariant #1); the drain must stay unconditional even
        // on a datum-change or station-drop epoch, or a stale (station index, key) pair
        // could outlive its arc.  Not serialized (as for dw_release_pending), so a warm
        // resume starts with an empty queue.
        std::set<std::pair<int, std::string>> raim_refloat_pending;
        /// "<station>|<sat>" LOS whose SH<->slant tie row is currently withheld, because its
        /// standardized deviation failed k or because it has not yet earned admission
        /// (INC-3 / INC-3b).  Rebuilt from scratch for every station the release seam
        /// sweeps (so a set or pruned LOS cannot leave a stale hold behind) and read by the
        /// assembly's tie-row build.
        std::set<std::string> dw_row_held;
        /// "<station>|<sat>" LOS whose tie row has been admitted (INC-3b).  A LOS absent from
        /// this set has its row held, which is what makes the guard act on a LOS's first
        /// tie row rather than one epoch after it (a positive admission list cannot be
        /// expressed as a hold list: at first sighting the seam has never run for that
        /// key).  Empty unless INC-3b is armed.
        std::set<std::string> dw_tie_seen;
        /// One-shot start/resume seed latch (INC-3b).  Nothing in this mechanism is
        /// serialized, so on the first epoch of a run (or of a warm-resumed leg) the
        /// admitted set is empty, and holding every tie row for one epoch would leave the
        /// whole SH field unconstrained, a shock far larger than the coupling event the
        /// guard exists to catch.  There is also nothing to guard at that point: the guard
        /// concerns a LOS coupling for the first time relative to an established field,
        /// and at start there is no established field.  So the first seam pass admits every
        /// present LOS unscreened and sets this latch; the assembly keeps the admission
        /// check inert until it is set.  The same reasoning as the eq34 card's
        /// `anchor_age_resume_seed`.
        bool dw_tie_seeded = false;
        /// Consecutive epochs a quarantined LOS has gone with no harvested deviation, keyed
        /// "<station>|<sat>".  `networkShDevVtec` skips its harvest for a station at weight
        /// <= 0 and for a LOS with no local column, so such a LOS can never produce the
        /// statistic the screen needs, and a screen that holds on missing data would
        /// withhold its tie row for the whole run.  The guard rejects on evidence, not on
        /// the absence of evidence, so after `kAdmitNoDevMaxEp` barren epochs the LOS is
        /// admitted and counted separately.  A LOS with data gets its first harvest in the
        /// same epoch its row would fold, so the bound never fails open for one the screen
        /// could judge.
        std::map<std::string, int> dw_admit_wait;
        /// Last NetworkIonoDeweightGate::advanceCount() the release seam consumed.  The
        /// producer skips the whole gate block on a datum-change epoch (there is no
        /// advance(false) caller), which leaves the gate's per-call release list holding
        /// the previous epoch's contents; without this watermark the seam would re-fire an
        /// old release on every held epoch.  -1 = nothing consumed yet.
        long long dw_gate_adv_seen = -1;

        // --- SH-field warning edge state and convergence latch (T1a/T1d) ---
        // Persistent state for the a₀₀ physical-band divergence warning (T1a).  It starts
        // "in band", so the first out-of-band epoch is always reported (transition edge);
        // later reports are throttled.
        bool sh_a00_was_in_band = true;
        // SH-field convergence latch and previous coefficient vector (T1d), for the
        // epoch-over-epoch RMS-change stationarity test that can gate the leveling-carry
        // warmup and/or the one-shot carry refloat.  Empty/false until the first emitted
        // SH epoch.
        bool sh_converged_latch = false;
        Eigen::VectorXd sh_coeff_prev;

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
        // forward; see the invariant #1 discussion in NetworkSdEwlAr.hpp.
        std::map<std::pair<std::size_t, char>, std::map<std::string, SdEwlFix>> sd_ewl_fix;
        std::map<char, std::string> sd_ewl_r0;   ///< constellation -> sticky component reference station name (A2's r0).
        /// (station index, constellation, satellite) -> (integer, the detector's lagged
        /// arc sample count).  The count separates a real flip from a legitimately changed
        /// integer on a restarted arc: a slip, gap or re-acquisition resets the MW
        /// accumulator, so the count goes down.
        std::map<std::tuple<std::size_t, char, std::string>,
                 std::pair<long long, std::size_t>> sd_ewl_prev;

        // --- State-snapshot channel, RTB-7(a) (path resolved at the run-TU site;
        // fingerprint, restore and write live in NetworkRunContext.cpp) ---
        std::filesystem::path snap_path;
        double   last_snapshot_write_s = -1.0;   // product time of the last snapshot write
        uint64_t live_fingerprint      = 0;
        uint64_t live_fingerprint_ctu_off = 0;   ///< card CTU G5: the live fingerprint without the card's switch-on tokens (0 when none is armed)
        bool     ctu_switch_on_resume = false;   ///< card CTU G5: this run resumed from a snapshot written without the card's knobs
        // Write-side telemetry (RTB-7(e), invariant #4).  The carry blob is about 99 % of
        // the snapshot, so its size and shape are the only readout that shows
        // network.state_snapshot_carry_diagonal took effect: a diagonal run otherwise looks
        // like a dense one in every log line and counter.
        long long snap_writes             = 0;      // snapshots written this run
        long long snap_carry_bytes_last   = 0;      // bytes of the last serialized carry blob
        bool      snap_carry_diagonal     = false;  // that blob was diagonal-only (v3)
        // Coupling-regressor capture (§18.12, network.state_snapshot_coupling_capture): the
        // QS/QL/QZ payload is about 37x the rest of the file, so the counts of rows, terms
        // and shared columns are the only readout that shows the capture took effect.
        // rows_dropped reports a writer-side numerical problem that is otherwise invisible:
        // a row with a non-finite field is omitted whole, never in part, because a partial
        // coefficient vector breaks the gauge identity of card 1e.
        long long snap_cpl_dict_last     = 0;   // shared columns in the last snapshot's QS dictionary (§18.12)
        long long snap_cpl_rows_last     = 0;   // QL rows in the last snapshot (§18.12)
        long long snap_cpl_zrows_last    = 0;   // QZ rows in the last snapshot (§18.12)
        long long snap_cpl_terms_last    = 0;   // regressor entries in the last snapshot (§18.12)
        long long snap_cpl_rows_dropped  = 0;   // rows omitted whole for a non-finite/negative field, run total (§18.12)
        long long snap_datum_rows_last   = 0;   // EQ*/CB* datum rows in the last snapshot; 0 under an armed capture on an eq34/time-linked-CB run means the capture is inert (RS-3 §21.7)
        long long snap_emit_rows_last    = 0;   // ER/PF/PS emit-layer rows in the last snapshot; 0 under an armed capture with the re-base or the filter running means the capture is inert (RS-5 §21.10)
        long long snap_station_rows_last = 0;   // RC/SD station-layer rows in the last snapshot; 0 under an armed capture on a run that arms carry_rx_code_bias or the T5 deviation carry means the capture is inert (RS-6 §21.13)
        long long snap_canon_rows_last   = 0;   // CN canonical-signal rows in the last snapshot; 0 under an armed capture means no station had decided a latch yet, which on a network run past epoch 1 means the capture is inert (RS-8 §21.16)
        // Snapshot: exempt (RS-8 §21.16); a resume-response staging buffer, filled by
        // networkTryWarmResume and drained by networkRestoreCanonicalSignals in the same
        // process.  The parsed CN rows, keyed by station roster index, wait here for the
        // driver to hand them to the station models (networkTryWarmResume has no
        // `stations` and must not grow one; the restore is the seam right after it).  Empty
        // on every run that does not resume with network.resume_signal_canon_restore armed.
        std::map<int, std::vector<PppModel::CanonicalSignal>> resume_canon_rows;
        long long snap_rxcb_lin_rows_last = 0;  // RL code-bias linearization-point rows in the last snapshot; 0 under an armed capture means the capture is inert, since no station holds a finite code-bias state, i.e. estimate_ifb is off (RS-9 §21.18)
        bool      resume_cpl_census_pending = false;  // latch for the first-post-resume harvest census and fold split (§18.12)
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
        long long redun_epochs = 0;            ///< census seam passes: the stride phase counter and the event-holdoff clock.
        std::set<std::size_t> redun_escalate_pending;   ///< stations the free tier flagged this epoch, swept at the next one (one-epoch latch).
        std::map<std::string, long long> redun_event_last;  ///< "station\x1fsat\x1fclass" -> redun_epochs of its last emitted hot-cell event.
        std::map<std::size_t, std::set<std::string>> redun_unobs_active;  ///< station index -> local-column keys currently flagged unobservable (the decoupling-edge set; an entry appearing is an event, an entry disappearing is a counted re-couple).
        // Per-epoch L1 scalars.  redun_ep_tow is the stamp the epoch-status writer
        // compares against: a non-sweep epoch leaves the columns blank instead of
        // repeating a stale sweep's numbers (the "unavailable is not 0" convention).
        double    redun_ep_tow           = -1.0;
        long long ep_redun_rows          = 0;  ///< the denominator: data rows swept this epoch.  A raw uncheckable count cannot be interpreted without it.
        long long ep_uncheckable_rows    = 0;
        double    ep_uncheckable_max_s   = 0.0;
        long long ep_unobservable_local  = 0;
        bool      redun_cov_warned    = false; ///< one-shot: armed with no solved covariance to read.
        bool      redun_closure_warned = false;///< one-shot: trace(H) exceeded the free-parameter bound.

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
        long long mdb_seq = 0;                 ///< flush passes: the per-cell event-holdoff clock.
        std::map<std::string, long long> mdb_event_last;  ///< "station\x1fsat\x1fclass" -> mdb_seq of its last emitted MDB event.
        bool      mdb_join_warned = false;     ///< one-shot: the uncheckable join disagreed with the census.

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
        std::map<std::string, long long> prires_event_last;  ///< "family|station|sat|signal" -> census-epoch index of its last emitted PRIORRES event.
        /// Per-epoch L1 scalars.  prires_ep_tow is the stamp the epoch-status writer
        /// compares against, so an epoch the census did not run leaves the columns blank
        /// rather than repeating stale numbers ("unavailable is not 0"; a real 0-row epoch
        /// and an un-run one say different things).
        double    prires_ep_tow      = -1.0;
        long long ep_prires_rows     = 0;   ///< the denominator: carried priors censused this epoch.
        long long ep_prires_resp     = 0;   ///< of those, labelled responded (the positive control's population).
        long long ep_prires_frame    = 0;   ///< of those, labelled frame (invariant-#7 edges).
        long long ep_prires_reported = 0;   ///< of those, |t| above the trace threshold (nothing acted on them).
        double    ep_prires_max_t    = 0.0; ///< the worst |t| this epoch, over every family and label.
        bool      prires_trunc_warned = false; ///< one-shot: the per-epoch identity buffer overflowed (ladders unaffected; the event stream became a sample).
        int       prires_last_flush_hour = -1; ///< content-hour bucket of the last ladder flush (durability: only health_trace flushes on its own).
        /// Constraint-aware global σ̂₀: the three epoch-status scalars, stamped by the same
        /// emission seam and gated by the same @ref prires_ep_tow stamp, so an epoch the
        /// rider did not drain leaves them blank rather than repeating stale numbers.
        bool      ep_s0g_formed = false;///< σ̂₀²_global was defined this epoch (Σr > 0); false means the column is blank, never 0.
        double    ep_s0g_sigma0 = 0.0;  ///< σ̂₀_global (the square root, directly comparable to the median σ̂₀ it is measured against).
        bool      ep_s0g_tension_def = false; ///< the tension ratio had a data side to divide by.
        double    ep_s0g_tension = 0.0; ///< Σw·ê² / Σw·v²: the headline column.
        int       ep_s0g_flags   = 0;   ///< @c NetworkPriorResidual::GlobalFlag bitmask: in-band degradation, never silent.

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
        // which takes ctx but not a run-TU reference.  Default-constructed with no
        // families, so enabled() is false and every entry point is a single bool test; an
        // unarmed run allocates nothing beyond the empty vectors.
        NetworkBlockBaarda bb{{}, 0.0};        ///< the run accumulator; re-seated from config once at run start.
        std::vector<BlockBaardaHit> bb_hits;   ///< this epoch's reportable blocks, drained by the emission seam.
        std::map<std::string, long long> bb_event_last;  ///< "station\x1fsat" -> census-epoch index of its last emitted BLOCKBAARDA event.
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
        // site is inside the redundancy-census seam, which takes ctx but not a run-TU
        // reference.  Default-constructed unarmed, so enabled() is false and every entry
        // point is a single bool test.
        NetworkCongruence cg{false};           ///< the run accumulator; re-seated from config once at run start.
        std::vector<CongruenceHit> cg_hits;    ///< this epoch's per-station records, drained by the emission seam.
        std::map<std::string, long long> cg_event_last;  ///< station name -> census-epoch index of its last emitted CONGRUENCE event.

        bool      s0g_approx_warned = false;  ///< one-shot: the data side took the r ≡ 1 fallback.
        bool      s0g_nodata_warned = false;  ///< one-shot: an epoch formed the number with no data rows at all.

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
        std::map<std::string, long long> prefit_event_last;  ///< "<event>|<station>|<sat>|<class>" -> processed-epoch index of its last emitted event (INC-1 and INC-2 rate limit).
        std::set<std::pair<int, std::string>> prefit_present_prev;  ///< (station index, satellite) seen in the last epoch's walk; absence here on a present LOS is the birth (INC-3).
        bool prefit_walk_seeded = false;   ///< the presence map holds a real epoch; before that, every LOS would read as born (INC-3).
        bool prefit_empty_logged = false;  ///< one-shot "armed but classified zero data rows" warn latch.
        /// (station index, satellite) whose worst @c obs_code prefit deviation exceeded
        /// @c network.prefit_event_sigma in the epoch stamped by @ref prefit_code_flag_ep.
        /// This is the one channel by which the census, a pre-solve instrument that runs
        /// later in the epoch than the admission seam, becomes evidence that the admission
        /// seam's INC-4 vet can act on at the next epoch (`network.adm_revet_census`).  The
        /// card's §1 names the census as the detection net; this set connects it to the
        /// response.
        ///
        /// Written from the exceedance, upstream of the event-class mask and the per-cell
        /// holdoff on purpose: detection must not depend on whether anyone is listening,
        /// and a trigger throttled by a diagnostic rate limiter would go quiet exactly when
        /// a cell is persistently bad.
        ///
        /// Code rows only.  A phase row's prefit innovation is arc-specific (it carries
        /// that arc's ambiguity linearization point), and its station median is not a
        /// common mode at all (608 of 665 phase events on one measured leg were
        /// non-findings), so a phase-driven trigger would feed false positives into an
        /// admission verdict.  The admission statistic itself obeys the same code-only
        /// rule.
        ///
        /// Per line of sight, not per band: the census's worst is taken over a satellite's
        /// code rows, and there is no row -> band map to resolve it further.  The flicker
        /// card's demotion map makes the same trade for the same reason (its deviation 6).
        /// It is the conservative direction (a flagged satellite refuses whichever of its
        /// bands is currently vetting), and the vet window bounds its reach.
        std::set<std::pair<int, std::string>> prefit_code_flag;
        /// The `n_loop_epochs` value at which @ref prefit_code_flag was last written (-1 =
        /// never).  The vet honours flags from the immediately preceding loop epoch only;
        /// anything older is stale and is counted as an absent consultation rather than
        /// believed.  The same staleness discipline as the `adm_ep_tow` / `redun_ep_tow`
        /// column stamps, and not the `ep_dw_engaged` pattern, whose sentinel is never
        /// reset and therefore sticks.
        long long prefit_code_flag_ep = -1;
        std::map<int, int>    topo_prev_tie;   ///< station index -> last epoch's iono_sh_row count, the deweight valve's own edge (INC-4).
        std::map<int, double> topo_prev_w;     ///< station index -> last epoch's SH-constraint deweight weight, the "w 0 -> 0.25" half of the event (INC-4).
        std::map<std::pair<int, std::string>, unsigned> topo_prev_kind;  ///< (station index, satellite) -> last epoch's data-class bitmask: band gained/lost, code-only <-> code+phase (INC-4).
        std::set<std::string> topo_prev_cols;  ///< last epoch's shared-column label set, for registry deltas (INC-4).
        bool topo_seeded = false;              ///< the baselines above hold a real epoch (INC-4).

        // --- Flight recorder (INC-5) ---
        // rhs_trip_eras is the era membership, not a fire log: a kind is inserted on its
        // rising edge and erased when its condition clears, so one era trips exactly once
        // however long it lasts.  rhs_trip_pending is the one-epoch handoff from a trigger
        // site to the SB_FRAMEDIAG consumer (the A4 withhold path consumes its own, since
        // that epoch never reaches SB_FRAMEDIAG).
        std::set<std::string> rhs_trip_eras;   ///< QC eras currently active (latched, one trip each).
        std::string rhs_trip_pending;          ///< this epoch's trip reason ("" = none); names the dump.
        std::unique_ptr<NetworkAttribFrame> rhs_prev_frame;  ///< the retained previous epoch (null unless retention is on and sized in).
        bool rhs_prev_oversize_logged = false; ///< one-shot: retention disabled for network size.

        // --- datum graph census ---
        // Not restored from a state snapshot, on purpose and for the same reason as the
        // prefit census's observer state: this is forward-only observer state of a
        // measure-only instrument, and persisting it would make a diagnostic
        // fingerprint-relevant; keeping it out is what keeps the card A/B-legal on a
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

        // --- Canonical-pair admission (card CPA) ---
        // The per-(station, satellite) admission memory of
        // docs/refound/CPA_canonical_pair_admission_2026-09-20.md, parked here rather
        // than in the seam because the rule's whole content is a value that must survive
        // to the next epoch (the INC-12 rule: a seam owns no state).  Not snapshotted and
        // not in the warm-resume fingerprint, the same forward-only posture as the other
        // observer states on this path: a resumed leg re-earns each pair's first good
        // epoch.  Empty and untouched unless network.canonical_pair_admission.
        NetworkCanonicalPairState cpa;
    };

    /// @internal Service counters, health-window anchors and B8 flag-rate tallies,
    /// relocated from the run loop.
    struct NetworkServiceCounters {
        /// --- The addStation positive-definiteness drop, counted (card RF-PD).
        /// The drop removes one station for one epoch
        /// (ApplicationNetworkAssembly.cpp: `present[i]=false`), and
        /// `network_epoch_status.csv` does not report it: `n_present` still counts the
        /// station, and otherwise the only record is a warning line in a rotating log.
        /// These are always counted (the increment costs nothing and writes no output);
        /// `network.trace_station_pd_null` arms the run-end readout and the per-rejection
        /// `sta_pd_dropped` event rows.
        long long pd_drop_epochs = 0;       ///< epochs whose serial consume loop ran (denominator)
        long long pd_drop_total = 0;        ///< station-epochs dropped by the addStation PD rejection
        long long pd_drop_named = 0;        ///< …of those, ones whose null direction was eigen-named (armed only)
        long long pd_drop_pivot_abort = 0;  ///< epochs lost because the rejected station was the legacy pivot
        int       pd_drop_max_ep = 0;       ///< largest per-epoch drop count over the run
        double    pd_drop_max_ep_tow = 0.0; ///< …its epoch
        /// Top-column family census over the named rejections (label prefix:
        /// "AMB" / "IONO" / "CBIAS" / "ZWD" / "CLK" / "RPB" / "ISB" / "other").  The read
        /// script does the per-station and persistence analysis off the event rows; this
        /// is the run-end one-liner that says which family dominates without opening a
        /// trace.
        std::map<std::string, long long> pd_drop_family;
        /// The same census weighted by mass: Σ over rejections of the null eigenvector's
        /// per-family mass Σw² (each rejection contributes 1.0 in total).  The top-column
        /// count above depends on where the top-5 cut fell; this one does not, and here
        /// that matters: the null is spread nearly evenly over a whole band of ambiguities,
        /// so the family ranked first is close to arbitrary while the mass split is stable.
        std::map<std::string, double> pd_drop_family_mass;
        /// --- The actual in-solve station count, and the re-coupling edge of invariant #7
        /// (card RF-PD-fix).  `network_epoch_status.csv`'s `n_present` is the roster that
        /// reached the consume loop, not the set that entered the arrowhead, so a
        /// PD-dropped (or assembly-dropped) station still reads as present.  The run-end
        /// tail and `network_run_summary.csv` report these instead, and they are always
        /// accumulated; the `sta_readmitted` event rows ride `network.trace_topology`, the
        /// counters do not.  The per-station return bookkeeping lives here in the run
        /// context so that no seam keeps it in a function-local (the INC-12 rule).
        struct StationSolveTrack {
            bool      ever_in_solve = false;  ///< distinguishes a return from a first sighting
            long long absent_streak = 0;      ///< consecutive consume epochs out of the solve
        };
        std::map<std::string, StationSolveTrack> sta_solve_track;
        std::vector<int> sta_in_solve_ep;        ///< per consume epoch: stations that entered the arrowhead
        long long sta_in_solve_epochs = 0;       ///< number of epochs the above was recorded for (denominator)
        int       sta_in_solve_min = -1;         ///< min over the run (−1 = never recorded)
        double    sta_in_solve_min_tow = 0.0;    ///< …its epoch
        long long sta_readmit_events = 0;        ///< stations that re-entered the solve after ≥ 1 absent epoch
        long long sta_readmit_max_gap_ep = 0;    ///< longest absence that ended in a readmission
        // --- Guarded temporal clock anchor (card Q3).
        // Standing invariant #4: `cta_folded_anchored == 0` on an armed run means the
        // mechanism is inert however clean the log reads, so every disposition is counted
        // against a denominator.  Trips are split by reason because the reasons are not
        // interchangeable evidence: a `gap` trip shows the staleness guard works, an `nis`
        // trip shows the detector works, and a run that only ever shows `resume` has
        // tested neither.
        long long cta_epochs = 0;            ///< epochs the anchor seam ran (denominator)
        long long cta_folded_anchored = 0;   ///< (sat,epoch) clock priors folded at the anchor variance
        long long cta_folded_loose = 0;      ///< (sat,epoch) clock priors folded at the legacy variance (loose)
        long long cta_floor_hits = 0;        ///< folds where the variance floor bound (the deviation-#1 cap binding)
        long long cta_trip_nis = 0;          ///< decouples: |w| over network.clock_anchor_k
        long long cta_trip_abs = 0;          ///< decouples: |ê| over network.clock_anchor_abs_m
        long long cta_trip_gap = 0;          ///< decouples: per-sat elapsed time over network.clock_anchor_max_gap_s
        long long cta_trip_datum = 0;        ///< decouples: a datum change retired this satellite's frame
        long long cta_trip_ext = 0;          ///< decouples: an external tier (clock monitor / RAIM) already refloated this satellite
        long long cta_resolves = 0;          ///< in-epoch re-solves executed (the adapt-and-emit half)
        long long cta_resolve_failed = 0;    ///< in-epoch re-solves that threw and were reverted to the anchored solution
        // --- Code-only level re-base (card CLV).  Standing invariant #4: on an armed run
        // `clv_shifts == 0` means the mechanism never moved a level, and
        // `clv_refs_shifted == 0` with shifts means the carried state never followed.
        long long clv_epochs = 0;            ///< kept epochs the re-base ran (the denominator)
        long long clv_skip_datum = 0;        ///< …of which were skipped whole: a datum change this epoch
        long long clv_twoway_failed = 0;     ///< …of which the two-way solve failed (no shift)
        long long clv_pairs = 0;             ///< (station,sat,epoch) ionosphere-free pairs entering the two-way fit
        long long clv_pairs_clipped = 0;     ///< …of which the per-satellite robust screen removed
        long long clv_sat_few = 0;           ///< (sat,epoch) with fewer than the minimum pairs (no update)
        long long clv_sat_tripped = 0;       ///< (sat,epoch) skipped: the Q3 anchor decoupled the satellite this epoch
        long long clv_sat_unarmed = 0;       ///< (sat,epoch) skipped: the anchor does not hold the clock (loose), so no level is carried to re-base
        long long clv_arms = 0;              ///< filter (re)starts when a satellite becomes eligible (first sighting or after a decouple)
        long long clv_breaks = 0;            ///< filter restarts at an innovation break (|e| over the break bar)
        long long clv_updates = 0;           ///< (sat,epoch) filter updates (a shift was computed)
        long long clv_at_floor = 0;          ///< …of which I_L was capped at 1 / floor^2
        double    clv_shift_sum_abs = 0.0;   ///< Σ |applied shift| (m)
        double    clv_shift_max_abs = 0.0;   ///< largest |applied shift| (m)
        long long clv_refs_shifted = 0;      ///< carried references moved (shared-carry keys, coupling terms, clock baselines, bridges)
        long long clv_held_moved = 0;        ///< held phase-bias emissions moved by the transfer (non-zero offset)
        // Card CLV return plan (R-A, R-B, R-C).  All zero unless the matching knob is armed.
        long long clv_pairs_offpin = 0;      ///< R-A: (station,sat,band) cells whose heaviest row was not a pin-pair signal (the rows the pin-pair read left out)
        long long clv_cb_follow_moves = 0;   ///< R-A: (sat,signal,epoch) free same-band code-bias columns moved with the level
        double    clv_cb_follow_sum_abs = 0.0; ///< R-A: Σ |code-bias follow step| (m)
        long long clv_paused = 0;            ///< R-B: (sat,epoch) skipped with the filter's information kept (datum pause)
        long long clv_pause_marks = 0;       ///< R-B: armed satellites marked paused at a datum change
        long long clv_confirm_wait = 0;      ///< R-B: (sat,epoch) arms or breaks waiting for confirmation (no shift applied)
        long long clv_confirm_reset = 0;     ///< R-B: break sequences reset by an in-band epoch or a sign change
        long long clv_pivot_ref_epochs = 0;  ///< R-C: (constellation,epoch) shifts referenced to the pivot's update
        long long clv_pivot_ref_missing = 0; ///< R-C: (constellation,epoch) the pivot had no update, so the de-mean was used
        // Card CLV2 R-A2, the free columns' own re-base.  All zero unless the knob is armed.
        long long clv_fcb_cells = 0;         ///< (station,sat,signal,epoch) same-band difference cells entering the fits
        long long clv_fcb_clipped = 0;       ///< …of which the per-satellite robust screen removed
        long long clv_fcb_few = 0;           ///< (sat,signal,epoch) with fewer than the minimum cells (no update)
        long long clv_fcb_fit_failed = 0;    ///< (constellation,signal,epoch) same-band fits that failed
        long long clv_fcb_updates = 0;       ///< (sat,signal,epoch) filter updates (a step was computed)
        long long clv_fcb_arms = 0;          ///< …of which were first arms
        long long clv_fcb_breaks = 0;        ///< …of which were restarts at an innovation break
        double    clv_fcb_sum_abs = 0.0;     ///< Σ |applied step| (m)
        double    clv_fcb_max_abs = 0.0;     ///< largest |applied step| (m)
        // Card HLT, broadcast health on the network path.  All zero unless network.sat_health.
        long long hlt_unhealthy_registered = 0; ///< (sat,epoch) registered clock satellites the broadcast flags unhealthy (processed in "pivot" mode, absent in "exclude")
        long long hlt_pivot_skips = 0;          ///< (constellation,epoch) pivot picks that passed over an unhealthy satellite
        long long hlt_pivot_fallbacks = 0;      ///< (constellation,epoch) with no healthy registered satellite, where the lexical first stood
        /// The closed-loop consumer's snapshot written beside each network history copy
        /// (`<state_snapshot_path>.consumer.epNNNN`; card CC, part B).  Invariant #4: a seed
        /// with `closed_loop.state_snapshot` armed and `cl_snap_written == 0` has no
        /// consumer warm start to offer a leg, however clean the log reads.
        long long cl_snap_written = 0;       ///< consumer snapshots written with the network copy's tow
        long long cl_snap_skipped = 0;       ///< history epochs where the consumer had not processed that tow (nothing written)
        // Handover S-transform telemetry (queue item 1(b); invariant #4): these are the
        // card's on-path counters, and transforms applied with sts_rows_retained == 0 mean
        // the fold release is mis-wired however clean the log looks.
        long long sts_edges = 0;             ///< pivot-switch (constellation,epoch) edges the transform handled
        long long sts_applied = 0;           ///< carried rows whose mean/var the transform re-expressed
        long long sts_rows_retained = 0;     ///< carry rows folded that the baseline skip would have dropped
        long long sts_noop_no_carry = 0;     ///< edges skipped: the incoming pivot had no carried clock level
        long long sts_pre_edges = 0;         ///< card PRM phase 2: edges re-expressed before the coupling rows
        long long sts_pre_redetected = 0;    ///< card PRM phase 2: re-detections after a skipped epoch, released without a second transform
        long long sts_pre_mismatch = 0;      ///< card PRM phase 2: changed constellations the solve glue elected differently from the preview (the transform ran after PASS B instead)
        long long sts_pre_undone = 0;        ///< card PRM phase 2: previewed transforms undone after a skipped epoch, the next election having chosen another pivot
        long long sts_selfcheck_fail = 0;    ///< post-transform |x_q| or re-derived c0 above tolerance (§8.4 INC-3)
        double    sts_max_abs_c0 = 0.0;      ///< largest |c0| applied over the run (m)
        double    sts_last_c0 = 0.0;         ///< the last c0 applied (m)
        // Card SCL B2 (network.handover_stransform_dense; invariant #4: armed with 0 dense edges
        // on a run with a handover means the kernel is mis-wired).
        long long sts_dense_edges = 0;          ///< edges the dense kernel re-expressed (S P S^T)
        long long sts_dense_fallback = 0;       ///< edges the dense kernel could not apply (the diagonal kernel ran)
        long long sts_dense_rows = 0;           ///< carried clocks with a block row re-expressed
        long long sts_dense_parked = 0;         ///< carried clocks without a block row moved by the diagonal rule
        long long sts_dense_p_added = 0;        ///< outgoing pivots appended to the block and the carry
        long long sts_dense_selfcheck_fail = 0; ///< dense edges whose self-check failed
        double    sts_dense_var_saved = 0.0;    ///< summed (diagonal kernel's variance - dense variance) over the re-expressed clocks (m^2)
        double    sts_dense_contrast_maxdev = 0.0; ///< largest |change| of a between-clock contrast variance over the run (m^2; rounding)
        long long cta_datum_kept = 0;           ///< card SCL B2: anchored clocks kept engaged at a dense-transformed handover (no datum decouple)
        long long cta_datum_kept_refused = 0;   ///< card SCL B2: armed datum decouples the keep rule refused (no dense edge, no row, a loose fold)
        /// The baseline that increment 0 named as missing: clock-pivot handovers per
        /// constellation over the run.  Counted whenever pivotHandoverSignalled raises the
        /// edge, transform armed or not, so an off run states the population an on run
        /// acts on.
        std::map<char, long long> pivot_handovers;
        long long cta_recouples = 0;         ///< re-coupling edges: a loose satellite re-earned the anchor
        long long cta_marks = 0;             ///< per-sat wire discontinuities queued at a decoupling edge
        long long cta_gf_keys = 0;           ///< (sat,epoch) clock priors whose information was gauge-downdated.  0 on an armed gauge-free run means the downdate never reached a group of >= 2 carried clocks: the step is inert and the anchor still constrains the gauge.
        long long cta_gf_inert = 0;          ///< epochs where the gauge-free step was armed but downdated nothing (every constellation contributed < 2 carried clocks, or UᵀΛU was singular)
        // The partial-support skip tally of card C-GF is not here: it lives run-scoped on
        // the estimator (NetworkEstimator::gaugeFreePartialSkipped) and is read once at
        // teardown, because the withdraw and within-epoch-replay folds run after any
        // per-epoch read point the driver could offer.
        double    cta_max_w = 0.0;           ///< largest |w| seen (any disposition)
        double    cta_max_w_tow = 0.0;       ///< …its epoch
        std::string cta_max_w_sat;           ///< …its satellite
        // --- The guard's own realized distribution (card Q3 §3, the k derivation).
        // `k` is read off a measured |t| ladder, and the ladder moves with q: the
        // denominator √(P⁻ − P⁺) shrinks as the anchor tightens, so a fixed k means a
        // different number of metres at every rung.  Without these, re-deriving k per
        // rung would need a second, disarmed run per rung.  Accumulated over anchored rows
        // on the first guard pass only (a later pass re-tests survivors against a moved
        // median and would count them twice).
        //
        // The population is truncated across epochs, not within one: a tripped satellite
        // goes loose and stops contributing until it re-couples, so the guard's own action
        // suppresses the upper tail.  For the untruncated null, run an arm with
        // `clock_anchor_k` beyond the top rung and `clock_anchor_abs_m: 0`; that is what
        // §3 means by "with the guard disarmed", and it is the only reading that
        // calibrates k without the truncation.
        long long cta_w_rows = 0;            ///< anchored rows contributing to the two ladders below (denominator)
        std::array<long long, 8> cta_w_ge{}; ///< |w| ≥ @ref kCtaWLadder[i]: the dimensionless ladder k is read off
        std::array<long long, 8> cta_e_ge{}; ///< |ê| ≥ @ref kCtaELadder[i] (m): the ladder the absolute backstop's rail is read off
        double    cta_sum_sqrt_s = 0.0;      ///< Σ √S over those rows, so mean √S = the metres that one unit of k buys at this rung
        double    cta_sum_abs_e = 0.0;       ///< Σ |ê| over those rows (m)
        // Card CTU section 14: the phase-space statistic (network.clock_anchor_guard_space).
        long long cta_phi_rows = 0;          ///< anchored pass-0 rows with a phase statistic
        long long cta_phi_unavail = 0;       ///< rows tested without one (no dense clock + two-band PB prior this epoch): abs only in phase mode
        long long cta_phi_floor_rows = 0;    ///< rows whose phase sqrt(S) sat at its floor
        std::array<long long, 8> cta_phi_w_ge{};   ///< |w_phi| >= kCtaWLadder[i]
        std::array<long long, 8> cta_phi_e_ge{};   ///< |e_phi| >= kCtaELadder[i] (m)
        double    cta_phi_sum_w2 = 0.0;      ///< sum w_phi^2 (the phase statistic's own sigma0)
        long long cta_x_both = 0;            ///< anchored pass-0 rows over k on both statistics
        long long cta_x_clk_only = 0;        ///< …on the clock statistic only (the level's motion)
        long long cta_x_phi_only = 0;        ///< …on the phase statistic only
        long long cta_trip_phi = 0;          ///< decouples decided by the phase statistic (phase mode)
        double    cta_sum_w2 = 0.0;          ///< Σ w², so √(Σw²/n) is the guard's own σ̂₀, a same-statistic cross-check on the census's σ̂₀_clk

        // --- Run tallies of the SD EWL Route-B integer decision (increment 2a).  Standing
        // invariant #4: on an armed run, `sdewl_dd_fixed == 0` means the decision never
        // fired, which is not the same as "the integers had no effect", so every rejection
        // path below is counted against a denominator.  The 2b row tallies live per
        // station in NetStation::SdEwlCounters (the row build is parallel).
        long long sdewl_epochs = 0;             ///< epochs the decision seam ran (the denominator for everything else).
        long long sdewl_components = 0;         ///< (constellation, connected component) instances processed.
        long long sdewl_dd_pairs_seen = 0;      ///< (station, satellite) arcs admitted to the integer graph.
        long long sdewl_dd_fix_attempts = 0;    ///< DD roundings attempted along spanning-tree / closure edges.
        long long sdewl_dd_fixed = 0;           ///< integers published usable this epoch: measured plus datum values (see sdewl_dd_datum; both are legitimately foldable, but only the measured ones went through a rounding gate).
        long long sdewl_dd_datum = 0;           ///< of those, values assigned as a datum rather than measured: the reference station's own satellites, each station's link satellite, and a satellite framed on first sighting.  A datum value carries no rounding decision, so it cannot be wrong: it defines the frame.
        long long sdewl_dd_reject_frac = 0;     ///< rejected: |fractional residue| over sd_ewl.fix_max_frac_cyc.
        long long sdewl_dd_reject_ffr = 0;      ///< rejected: rounding success probability under sd_ewl.fix_min_ffr.
        long long sdewl_dd_flip = 0;            ///< integer disagreed with the previous epoch on a continuous arc: counted and vetoed (the invariant #1 response).
        long long sdewl_dd_restart = 0;         ///< arc restarted (lagged count fell): memory replaced, no flip charged.
        long long sdewl_sd_ref_reelected = 0;   ///< the sticky component reference station r0 changed (re-datums the whole component's SD frame).
        long long sdewl_dd_skip_channel = 0;    ///< (station, satellite) excluded: its MW arc is on a different tracking-channel pair than the network-common election, so B^s would not cancel in the DD.

        // --- Run tallies of the adaptive iono q (network.adaptive_iono_q).  The feed
        // counter lives per station (NetStation::iono_var_epochs_fed, since PASS A is
        // parallel); these two are bumped in the serial T5v2 aging only.
        long long aiq_boosts_total = 0;         ///< GM deviation innovation boosts applied (multiplier > 1 LOS-epochs).
        double    aiq_max_mult = 1.0;           ///< Largest GF-TD2 multiplier applied over the run.

        // Slip-aware eq34 election of cards 3/4 (network.phase_datum_eq34_slip_aware_election).
        // All three are needed to read the mechanism, because two of them are the policy
        // working and the third is the graph refusing it:
        //   * deseeded:   slipped incumbents denied their sticky tree seat;
        //   * demoted:    arcLife rankings pushed below the clean band (inert unless the
        //                 longevity election is armed; counted per comparison, so it is a
        //                 sort-effort number, not a count of distinct arcs);
        //   * readmitted: slipped arcs that became tree edges anyway, because the
        //                 component had no other route to the vertex.  Those still
        //                 mass-refloat, and a large share here means the topology, not the
        //                 election, is the binding constraint (standing invariant #4:
        //                 report the failure mode, and do not let "deseeded > 0" read as
        //                 "no tree edge slipped").
        long long eq34_slip_deseeded   = 0;
        long long eq34_slip_demoted    = 0;
        long long eq34_slip_readmitted = 0;
        // Run tallies of the card-3 chain-pin audit (docs/SPEC_synthetic_minimum_network.md
        // §14 T1; measured record §12.8), keyed "<cons>|<signal>" (the election's dr_key).
        // A pin can leak silently from a three-stage pipeline: declared (the election's
        // per-group Σ (n_c + m_c) gauge dimension), folded (foldAmb/foldRpb built the 1e10
        // row; the lambdas return early on a missing state index or a missing live phase
        // row), and landed (the folded row sits on a station that est.addStation accepted;
        // a rejected or dropped station takes every folded pin down with it, the loss mode
        // network.station_pd_pin_aware exists to prevent).  A group whose landed <
        // declared − bridged in any epoch ran with an under-constrained datum that reads
        // like a working one (standing invariant #4); the W6+SH n-invariant L2W near-null
        // is the suspect that motivated the audit.  Accumulated serially after PASS B's
        // consume; logged once by NetworkTeardown.  All zero unless
        // network.phase_datum_eq34.
        struct Eq34PinAudit {
            std::map<std::string, long long> declared;         ///< Σ per-epoch gauge dim (n_c+m_c)
            std::map<std::string, long long> bridged;          ///< Σ hygiene INC-2 bridge pins (shared-side)
            std::map<std::string, long long> folded;           ///< pins foldAmb/foldRpb built (all producing stations)
            std::map<std::string, long long> landed;           ///< folded pins on accepted stations
            std::map<std::string, long long> miss_state;       ///< fold refused: state index absent
            std::map<std::string, long long> miss_row;         ///< fold refused: no live phase row (h2local miss)
            std::map<std::string, long long> dropped_station;  ///< folded pins lost with dropped/rejected stations
            std::map<std::string, int>       short_epochs;     ///< epochs with landed < declared − bridged
            std::map<std::string, int>       worst_short;      ///< max per-epoch shortfall
            int epochs = 0;                                    ///< audited (consumed, non-aborted) epochs
        } eq34_pin_audit;
        // Run tallies of the card-3 seed round.  The card asserts |pin innovation| ≤ λ/2
        // at all times, so a max_abs_y above λ_max/2 is a logic error.  `pins` counts
        // election-side targets (the state existed at election time); `deferred` counts
        // fold-computed targets (state allocated in PASS B: cold start and new arcs);
        // `target_steps` counts same-station target changes between epochs (the reref
        // pressure this knob generates; about 0 expected on static receivers).  All zero
        // unless network.phase_datum_eq34_seed_round.
        long long seed_round_pins         = 0;
        long long seed_round_deferred     = 0;
        long long seed_round_frozen       = 0;   ///< targets reused verbatim from eq34_prev_target, so the frame is realized once (INC-1)
        long long seed_round_target_steps = 0;
        double    seed_round_max_abs_y_m  = 0.0;
        // Re-host tallies (INC-2).  `framed` = re-hosts whose frame step was computed
        // from the new host's own state and therefore stayed shiftable (the carried PB
        // prior is S-transformed, not marginalized); `cold` = re-hosts that INC-2 could
        // not frame (no allocated state: the fold-deferred path), which keep the legacy
        // unknown-step reref.  `max_step_m` is the largest |frame step| any framed re-host
        // applied.  It is bounded by λ/2 only when the new host's float is
        // frame-consistent, so a large value signals that the solution at the new host had
        // drifted off the realized frame: read it as a diagnostic of the runaway the card
        // probes, never as a quality bound.
        long long seed_round_rehost_framed  = 0;
        long long seed_round_rehost_cold    = 0;
        double    seed_round_rehost_max_step_m = 0.0;
        // Run tallies of eq34 reattach hygiene; every field is documented on the struct.
        // Bumped only when ctx.knobs.anchor_hygiene_on, and reported by the run-end block
        // in filter/NetworkTeardown.cpp.
        NetworkAnchorTallies hyg;
        // Run tallies of flicker/gap signal integrity; every field is documented on the
        // struct.  Bumped only when ctx.knobs.sig_integrity_on, and reported by the
        // run-end block in filter/NetworkTeardown.cpp.  `demoted_rows` is summed there
        // from the per-station NetStation::si_rows_demoted counters (the fold runs inside
        // the parallel produce, so the row tally cannot live here).
        NetworkSignalIntegrityTallies sig;
        // Run tallies of iono-deweight release hygiene; every field is documented on the
        // struct.  Bumped only when ctx.knobs.dw_hygiene_on, and reported by the run-end
        // block in filter/NetworkTeardown.cpp.
        NetworkIonoDwReleaseTallies dwr;
        /// Run-end telemetry of the RAIM pool re-host.  All zero unless
        /// ctx.knobs.raim_ci_on; the denominators separate "seam never ran" from "census
        /// empty" from "nothing flagged" (standing invariant #4).
        NetworkRaimCiTallies raim_ci;
        /// Run-end telemetry of the gross line-of-sight screen (card GL,
        /// docs/SPEC_event_harness.md §24.18).  All zero unless ctx.knobs.gl_on; the
        /// denominators separate "seam never ran" from "no code row reached it" from
        /// "nothing cleared the bar" from "evidence never persisted" (standing invariant
        /// #4).
        NetworkGrossLosTallies gross_los;
        // Run tallies of product-referenced measurement admission; every field is
        // documented on the struct.  Bumped only when ctx.knobs.adm_on, and reported by
        // the run-end block in filter/NetworkTeardown.cpp.  Named `adm` and not reachable
        // as `ctx.admission` on purpose: that name belongs to the CB-ADM W6 code-bias
        // admission census (@ref NetworkBiasAdmission below), a different mechanism, and
        // the two are easy to confuse.
        NetworkMeasurementAdmissionTallies adm;
        long long sdewl_dd_skip_frame = 0;      ///< (station, satellite) excluded: BIA frame disagrees with the constellation majority.
        long long sdewl_dd_no_link = 0;         ///< (station, satellite) left unfixed: no admissible link satellite reached it from the tree/closure.
        long long sdewl_dd_no_graph = 0;        ///< stations isolated: fewer than two co-visible satellites with any neighbour, so no DD can be formed.

        double prev_tow = -1.0;
        int    prev_week = -1;
        int    iod = 0;
        // Per-run solution-generation id, minted at the first emit from that epoch's GPS
        // time: deterministic under replay (the id is serialized per epoch), and different
        // across live restarts, which make their first emit later.  0 = unset.
        uint32_t generation_id = 0;
        // Product time (s) of the last written S contract line (the deferred v2 header
        // counts as the first); -1 until the contract writer opens.
        double last_contract_emit_s = -1.0;
        size_t n_emitted = 0, n_skipped = 0, n_gate_withheld = 0, n_runaway_epochs = 0;
        // Operator health-record state (network.health_interval_s > 0).
        double last_health_write_s = -1.0;   // content time of the last health row (s)
        double last_emit_content_s = -1.0;   // content time of the last emitted epoch (s)
        int    last_n_present      = 0;      // stations present in the most recent processed epoch
        int    last_n_clock_sats   = 0;      // registered clock sats in the most recent processed epoch (P4b)
        // ms_per_epoch_window anchors (P4(b)): wall ms and loop-epoch count as of the
        // previous health row, so each row reports the mean over its own window (the
        // operationally useful "is it slowing down now" signal), not a cumulative run mean
        // labelled "rolling".
        double health_win_ms_anchor     = 0.0;
        size_t health_win_epochs_anchor = 0;
        size_t n_sigma0_withheld = 0;   // A4/R8: epochs skipped specifically by the σ̂0 withhold ceiling (subset of n_skipped)
        // Withhold-era release policy: era telemetry for both edges, per invariant #7.  All
        // zero unless one of the two knobs is armed (the era bookkeeping itself is
        // knob-gated to keep the events CSV byte-identical on off paths).
        long long n_wh_era_entries        = 0;  ///< SIGMA0/era_enter events
        long long n_wh_era_exits          = 0;  ///< SIGMA0/era_exit events (a run ending mid-era exits nowhere; max_len covers it)
        long long n_wh_era_max_len        = 0;  ///< longest era (withheld decides)
        long long n_wh_escalated_epochs   = 0;  ///< era epochs run at the escalated budget
        long long n_wh_escalate_landed    = 0;  ///< escalated epochs that landed σ̂₀ ≤ ceiling (took the accepted path)
        long long n_wh_escalate_exhausted = 0;  ///< escalated epochs that still withheld
        long long n_wh_free_erases        = 0;  ///< carry entries erased at the withhold branch (§3b)
        long long n_wh_perconst_exits     = 0;  ///< declared limitation: eras ended by the RTA-10 carve-out publishing (no escalation ran)

        // --- Leveling-carry retirement census ---
        // Every path that removes a phase-leveling carry entry is counted at its own site,
        // so a run can attribute a fleet-scale carry loss instead of inferring it.  The gap
        // prune reads only `last_seen_s`, i.e. it selects on elapsed time and nothing else;
        // these counters separate it from the four quality-driven retirement paths that run
        // beside it.  §7's promotion refusal assumes that the invariant-#1 gap cliff
        // usefully retires stressed carries, and these counters measure that.  Pure
        // telemetry (no solve effect, no CSV row), always on.
        // (A) Departures: entries physically removed and not replaced this epoch.  The gap
        //     prune runs immediately after the harvest has refreshed last_seen_s for every
        //     arc it stashed, so its candidate set is disjoint from the arcs observed this
        //     epoch: it can only retire arcs that were not harvested (set satellites, absent
        //     stations, harvest failures).  It is therefore a departure counter and not the
        //     cliff; (B) is the cliff.
        long long n_cr_gap_prune          = 0;  ///< amb-carry entries dropped by the pl_max_gap_s prune (departures; selected by time)
        long long n_cr_gap_prune_epochs   = 0;  ///< epochs on which that prune removed at least one entry
        long long n_cr_gap_prune_max      = 0;  ///< largest single-epoch prune removal
        long long n_cr_gap_prune_max_week = 0;  ///< GPS week of that epoch (attribution handle)
        double    cr_gap_prune_max_tow    = 0.0;///< TOW (s) of that epoch; joins directly to the `tow` column of every trace CSV
        long long n_cr_gap_prune_max_fleet= 0;  ///< carry size immediately before that removal

        // (B) The cliff: the inject-side stale skip, the mechanism §1.3 observed as the pl
        //     prires rows falling to 0.  The carried prior is not injected because its gap
        //     exceeded the horizon, so the arc solves free and is then re-stashed from one
        //     epoch of code.  The entry is never erased, which is why the prune above cannot
        //     see it.  Counted at the inject predicate itself.
        long long n_cr_inject_stale       = 0;  ///< carried priors skipped for gap > pl_max_gap_s (the mass re-level)
        long long n_cr_inject_stale_epochs= 0;  ///< epochs on which at least one prior was skipped stale
        long long n_cr_inject_stale_max   = 0;  ///< largest single-epoch stale-skip count
        long long n_cr_inject_stale_max_week = 0; ///< GPS week of that epoch
        double    cr_inject_stale_max_tow = 0.0;  ///< TOW (s) of that epoch: the attribution handle for a cliff
        long long n_cr_inject_stale_max_sta  = 0; ///< distinct stations in that epoch's stale set (separates a fleet-wide era from one station dropping out)

        // Ambiguity-vehicle telemetry (RF-1, docs/refound/RF-1_float_ambiguities_design.md
        //     §7.2 / §7.4).  Standing invariant #4: these counters are the on-path evidence,
        //     and design §7.4's D3 states the rule in the other direction too: a float_kf
        //     run with 0 drops is a miswire, not a quiet run.  The trace counters
        //     (@c n_amb_state_rows / @c n_amb_state_events / @c n_amb_state_epochs), the
        //     drop counters (step 4) and the S-transform counters (step 5) are all reported
        //     at run end, so an arm that arms a mechanism and moves none of them is visible
        //     at once rather than after a grade.
        long long n_amb_drop              = 0;  ///< carried priors dropped back to their birth prior, value_abs kept (RF-1 step 4).
        long long n_amb_drop_epochs       = 0;  ///< epochs on which at least one drop happened.
        long long n_amb_drop_relevel_free = 0;  ///< …of which from the detectors' latched free-epoch set (the level vehicle skips one inject instead).
        long long n_amb_drop_stale_gap    = 0;  ///< …of which from the inject-site gap cliff (the level vehicle skips one inject instead).
        long long n_amb_drop_t4           = 0;  ///< …of which from the RAIM prominence-attribution refloat (the level vehicle erases the entry instead).
        long long n_amb_drop_t3           = 0;  ///< …of which from the RAIM per-satellite consensus refloat (the level vehicle erases the entry instead).
        long long n_amb_drop_idempotent   = 0;  ///< drop calls that found the arc already at or below birth strength: two detectors on one arc in one epoch, or an arc that never converged.  The age clock still restarts; the variance does not move.
        long long n_amb_birth_floored     = 0;  ///< birth stashes whose var_birth was raised by network.amb_float_birth_var_m2 above the arc's own first-stash variance: the on-path telemetry of the floor (invariant #4: 0 under an armed floor means every arc was already born looser than it).
        // Telemetry of the drop-only floor (RF-3a,
        //     docs/refound/RF-3_code_row_qc_2026-09-22.md §4 A1/A2), maintained in both knob
        //     states so an off run reports the walk it suffers (standing invariant #4).
        long long n_amb_drop_floored      = 0;  ///< drops whose restored variance came from network.amb_float_drop_var_m2, i.e. the drop floor at this LOS exceeded max(var_birth, var_first, birth floor).  0 by construction with the knob at 0.
        long long n_amb_drop_pre_gate     = 0;  ///< drops taken before network.amb_float_drop_floor_after_ep opened, with an armed drop floor: the population the gate withheld the floor from (RF-3c).  0 with no gate, and 0 with no floor.
        /// |value change| (m) absorbed by the first fold after a drop, one entry per drop
        /// that restarted the age clock (RF-3a A2).  Measured at the harvest from the
        /// carry's value before and after that fold, so it is the size of the correction
        /// the drop's restored prior let through in one epoch.  With a stiff restored prior
        /// the first fold absorbs little and the value then walks for tens of epochs; with
        /// a stated floor the correction lands at once.  Read at run end as max and median.
        std::vector<double> amb_walk_fold1_m;
        /// The same at the second fold after the drop, i.e. the residual walk itself
        /// (RF-3a A2).  A stiff restored prior is still moving here; a floored one has
        /// settled.
        std::vector<double> amb_walk_fold2_m;
        long long n_amb_stransform        = 0;  ///< declared anchor S-transformations applied to the carry, one per (sat,signal) family per handover (RF-1 step 5).
        long long n_amb_stransform_cols   = 0;  ///< …carried columns shifted by them.
        long long n_amb_sts_identity      = 0;  ///< …families whose Δ was exactly 0 (already in the new basis: a station re-pinning itself): the transform is the identity, and the family is still owned (no erase).
        long long n_amb_sts_refused_cold  = 0;  ///< handovers refused by the transform because the election was cold (no carried level for the successor, so no new basis to express the family in): the erase stands; this is the limit of step 5 and the subject of RF-2.
        long long n_amb_sts_refused_no_level = 0; ///< …refused because the ledger said continuous but the incoming anchor held no live carry this epoch: a ledger/carry disagreement, counted apart from a cold election.
        // On-path telemetry of the held S-basis (RF-2; invariant #4).
        long long n_pb_basis_held          = 0;  ///< family-epochs re-pinned at their frozen basis value (the held property in action).
        long long n_pb_basis_elected       = 0;  ///< new basis entries written: first sightings plus handovers.
        long long n_pb_basis_cold          = 0;  ///< …of which cold (no level to inherit): the new basis is 0 and the emitted phase bias steps by the outgoing value.
        long long n_pb_basis_refuse_young  = 0;  ///< candidate-family evaluations refused by the arc-age gate (never a newborn low arc).
        long long n_pb_basis_refuse_flagged= 0;  ///< …refused by the screen-flag quiet window (item 3), which is also item 4's depose trigger on the incumbent.
        long long n_pb_basis_rank_changed  = 0;  ///< elections whose elevation rank picked a different station than the coverage order would have, plus incumbents deposed by a flag.
        long long n_pb_basis_declared      = 0;  ///< handovers declared to the consumer (markSatDiscontinuity through the retro-bump queue), continuous and cold alike (RF-2 item 2).
        long long n_pb_basis_declared_cold = 0;  ///< …of which cold, i.e. the wire's phase bias stepped.
        // Why the datum moved: one counter per deposition cause (RF-2b).
        long long n_pb_basis_refuse_unusable = 0; ///< candidate evaluations refused because their carried level is not usable as a datum (the continuous branch's ledgerVerdict), the test that RF-2's rank lacked.
        long long n_pb_basis_elev_fallback   = 0; ///< elections where no candidate was ledger-usable and the rank fell back to elevation only (a cold pin).
        long long n_pb_basis_depose_flag     = 0; ///< incumbents deposed by a screen flag on their own basis column.
        long long n_pb_basis_depose_death    = 0; ///< …deposed because the basis arc has no carry left at all.
        long long n_pb_basis_depose_unusable = 0; ///< …deposed because their own carried level stopped passing the ledger verdict.
        long long n_pb_basis_value_drift   = 0;  ///< mid-tenure basis-value changes refused: the alarm of the held property itself.  Must read 0.
        long long n_amb_sts_selfcheck_fail = 0; ///< transforms whose exactness self-check failed: the incoming anchor's carried level did not read 0 within kStsExactTolM afterwards.  A transform defect, never noise; logged as a warning with identities.
        long long n_amb_block_fallback    = 0;  ///< station-epochs whose joint block fell back to per-column rows under the float_kf vehicle (RF-1 step 3, risk #3): a vehicle change on one station-epoch, so it is counted rather than noted.  The run-end bar (on n_pl_blk_fallback) fails a run whose fallback rate exceeds 1 % of attempted station-epochs.
        long long n_amb_state_rows        = 0;  ///< network_amb_state.csv: trajectory rows written (one per carried (station,sat,signal) per traced epoch).
        long long n_amb_state_events      = 0;  ///< network_amb_state.csv: rows carrying a non-empty `event` (birth / drop / erase / stransform).
        long long n_amb_state_epochs      = 0;  ///< network_amb_state.csv: epochs that wrote at least one row.
        long long n_amb_state_erase_rows  = 0;  ///< network_amb_state.csv: `erase:<cause>` rows, entries captured at the four erase sites before deletion.

        // On-path telemetry of the inject-side per-LOS variance scale (F1,
        //     network.phase_leveling_los_scale, SPEC_amb_iono_split_overcount; invariant
        //     #4).  Counted per station in the parallel produce phase (StationAssembly) and
        //     folded here serially.  Armed on a multi-band posture with rows = 0 means the
        //     inject never saw two carried bands on one LOS: a miswire, not a quiet run.
        long long n_pl_los_scale_rows    = 0;             ///< carried ambiguity rows whose variance was multiplied by a B_eff > 1
        long long n_pl_los_scale_hist[4] = {0, 0, 0, 0};  ///< LOS groups (station, sat, epoch) by B_eff = 1 / 2 / 3 / ≥4 injected columns

        // On-path telemetry of the station-wide jointly whitened leveling-carry block (F3,
        //     network.phase_leveling_local_block, SPEC_amb_iono_split_overcount; invariant
        //     #4).  Counted per station in the parallel produce phase (StationAssembly) and
        //     folded here serially; the two refloat counters are written directly by the
        //     serial within-epoch RAIM re-solve.  Armed with blocks = 0 means the inject never
        //     built a block: a miswire (carry_cpl_cond off, carry_cpl_joint on, or no station
        //     with an injectable carried column), not a quiet run.  pair_missing ≈
        //     Σ n(n−1)/2 over the stations on the first post-resume epoch is the expected
        //     one-epoch degradation (the harvest block is not snapshotted); a steady non-zero
        //     rate afterwards means columns re-harvested at different epochs (satellites
        //     dropping in and out).  marg_cols is the decoupling edge of invariant #7: every
        //     carried entry not admitted to its station's block this epoch (departed, stale,
        //     freed, reseed); the block omits it and never conditions on it.
        long long n_pl_blk_blocks       = 0;             ///< blocks whitened (station-epochs)
        long long n_pl_blk_rows         = 0;             ///< whitened rows emitted
        long long n_pl_blk_marg_cols    = 0;             ///< carried entries not injected (marginalized out of their station's block)
        long long n_pl_blk_pair_missing = 0;             ///< (i,j) pairs zeroed: harvested at different epochs / no harvest block
        long long n_pl_blk_col_refused  = 0;             ///< block columns dropped by the scalar-row screen (non-finite RHS or non-positive variance)
        long long n_pl_blk_fallback     = 0;             ///< station-epochs whose block factorization failed → F1/diagonal rows
        long long n_pl_blk_young_cols   = 0;             ///< injectable columns kept out of their block (too young) and emitted per column (F3 age gate)
        // Invariant #4 telemetry of the age-aware young-arc inflation
        // (network.phase_leveling_young_inflate_var_m2).  An armed run with n_yng_cols == 0
        // is armed and inert (no carried column was ever stamped young at an inject), not a
        // quiet run; a mean age at the ceiling says the inflated population is nearly
        // mature and the knob buys almost nothing.  n_yng_cols counts columns for which a
        // positive var_add was computed and handed to a route; a column that the block's
        // scalar-row screen then refused (n_pl_blk_col_refused) is included.
        long long n_pl_blk_yng_scaled = 0;               ///< block columns (F3) whose row/column of Σ was congruence-scaled.  Read against n_yng_cols: a gap means columns computed young whose block never reached the scaling (refused by the scalar screen, or the station took the per-column route).
        long long n_yng_cols        = 0;                 ///< column-epochs inflated (a positive var_add computed and handed to the block congruence or the diagonal row)
        long long n_yng_skip_age    = 0;                 ///< stamped column-epochs refused because age > phase_leveling_young_inflate_max_age_ep (invariant #3: the deferral is measured).  An unstamped entry (relevel_s == 0: a warm-resume restore, or a lineage without the stamp) is treated as mature and counted in neither.
        double    yng_var_add_sum   = 0.0;               ///< Σ of the var_add handed out (m²): the total prior information given back
        double    yng_age_sum       = 0.0;               ///< Σ of the ages (loop epochs) of the inflated columns; ÷ n_yng_cols = the run's mean inflated age
        long long n_yng_stamp_create = 0;                ///< re-level stamps set at the harvest because the entry was created (first stash, or the re-stash after a T4 erase / gap prune deleted it)
        long long n_yng_stamp_free   = 0;                ///< re-level stamps set at the harvest because the arc was freed this epoch (the latched relevel.free_ambs set the PASS-B inject consulted)
        long long n_pl_blk_hist[5]      = {0, 0, 0, 0, 0}; ///< block dimension histogram <10 / 10–24 / 25–49 / 50–74 / ≥75
        long long n_pl_blk_max_dim      = 0;             ///< largest block dimension over the run
        // Invariant #4/#7 telemetry of B9-in-block (network.carry_zwd_in_block).  A run with
        // the knob armed and n_pl_blk_zwd_cols == 0 is unwired, not clean; a large
        // n_pl_blk_zwd_fallback_diag says most station-epochs degraded, without a signal,
        // to the diagonal B9 row that the in-block carry replaces.
        long long n_pl_blk_zwd_cols     = 0;             ///< ZWD columns admitted into a block build (station-epochs; a block that then failed to factorize still counts, and n_pl_blk_zwd_fallback_diag says how many of those ended up on the diagonal row instead)
        long long n_pl_blk_zwd_fallback_diag = 0;        ///< station-epochs whose ZWD fell back to the B9 diagonal row (no block for the station this epoch, or the block's factorization failed)
        long long n_pl_blk_zwd_missing_cross = 0;        ///< (ambiguity, ZWD) pairs zeroed because the harvest cross term was missing (harvest-stamp mismatch / ZWD stashed without a block); a subset of n_pl_blk_pair_missing
        // Invariant #4/#7 telemetry of I-in-block (network.carry_iono_in_block), with the
        // discipline of the ZWD split above.  An armed run with n_pl_blk_iono_cols == 0 is
        // unwired, not clean.  n_pl_blk_iono_not_folded is not a "degraded to the old row"
        // count: the in-block carry has no diagonal iono row (that is the FALSIFIED
        // QUEUE-8(b) vehicle), so those columns carried nothing that station-epoch.
        long long n_pl_blk_iono_cols    = 0;             ///< per-LOS iono columns admitted into a block build (a block that then failed to factorize still counts; n_pl_blk_iono_not_folded says how many of those did not act)
        long long n_pl_blk_iono_not_folded = 0;          ///< iono columns pushed but not folded (non-SPD block / no block): their carry did not act this station-epoch, and there is no diagonal fallback, on purpose
        long long n_pl_blk_iono_missing_cross = 0;       ///< (iono, other) pairs zeroed because the harvest cross term was missing (harvest-stamp mismatch / entry stashed without a block); a subset of n_pl_blk_pair_missing
        long long n_iono_blk_erased_t4  = 0;             ///< in-block iono carries erased by a T4/consensus arc refloat (invariant #1: the T4 statistic reads level and iono error as a sum, so the LOS re-draws both)
        long long n_iono_blk_gap_drop   = 0;             ///< in-block iono carries dropped at the harvest prune for exceeding carry_iono_in_block_max_gap_s without a sighting (the absent-satellite / eclipse edge)
        long long n_raim_pool_iono_rows = 0;             ///< post-fit RAIM pool admissions (station-epochs × rows) whose admitted row is one of the iono columns' whitened block rows: the §7 item 4 interaction measured for the I half, as n_raim_pool_zwd_rows measures it for the ρ half.  Counted only; the pool rule is unchanged.
        // Invariant #4/#7 telemetry of RX-CLK-in-block (network.carry_rx_clock_in_block),
        // with the discipline of the ZWD and iono splits.  An armed run with
        // n_pl_blk_rxclk_cols == 0 is unwired, not clean.  n_pl_blk_rxclk_not_folded is not
        // a "degraded to the old row" count: the in-block carry has no diagonal clock row
        // (a diagonal prior on the most station-common direction of all is the over-count
        // that falsified the diagonal ZWD carry), so those columns carried nothing that
        // station-epoch.
        long long n_pl_blk_rxclk_cols   = 0;             ///< receiver-clock-family columns admitted into a block build (a block that then failed to factorize still counts; n_pl_blk_rxclk_not_folded says how many did not act)
        long long n_pl_blk_rxclk_isb_cols = 0;           ///< …of which are ISB columns.  0 is not an alarm (a GPS-only / BSSD posture allocates no per-system time-offset state).
        long long n_pl_blk_rxclk_not_folded = 0;         ///< clock-family columns pushed but not folded (non-SPD block): their carry did not act, and there is no diagonal fallback, on purpose
        long long n_pl_blk_rxclk_missing_cross = 0;      ///< (clock-family, other) pairs zeroed because the harvest cross term was missing (harvest-stamp mismatch / entry stashed without a block); a subset of n_pl_blk_pair_missing
        // Invariant #4/#7 telemetry of RXCB-IN-BLOCK (network.carry_rx_code_bias_in_block).
        // An armed run with n_pl_blk_rxcb_cols == 0 is unwired, not clean; the not-folded
        // count is a "carried nothing" count (no diagonal fallback, as for the clock).
        long long n_pl_blk_rxcb_cols = 0;                ///< receiver code-bias columns admitted into a block build
        long long n_pl_blk_rxcb_not_folded = 0;          ///< receiver code-bias columns pushed but not folded (non-SPD block)
        long long n_pl_blk_rxcb_missing_cross = 0;       ///< (receiver code-bias, other) pairs zeroed for a missing harvest cross term
        long long n_rxcb_blk_harvested = 0;              ///< receiver code-bias entries stashed with a border regression and a harvest-block row
        long long n_rxcb_blk_harvest_fail = 0;           ///< receiver code-bias columns whose coupling harvest failed (not stashed; the column is cold next epoch)
        long long n_amb_drop_decoupled_idem = 0;         ///< card CTU section 14: drops at birth strength that cleared a regression / block row (network.amb_float_drop_decouple_always)
        long long cxu_resume_stash = 0;                  ///< card CTU G5: the exact update's stash stamp rebuilt from a restored dense border
        long long n_rxcb_blk_restored_skip = 0;          ///< RXCB-IN-BLOCK: carried entries without a harvest-block row (restored from a diagonal-carry snapshot) left out of the block and re-harvested
        long long n_rxcb_blk_step_reset = 0;             ///< receiver code-bias entries erased at the harvest by the frame-invariant step test (10 sigma of the two harvested variances, the walk and a 0.5 m guard): the event response of invariant #1
        long long n_rxcb_blk_no_block = 0;               ///< receiver code-bias entries stashed without a harvest-block row (cross terms to the station's other columns missing next epoch)
        long long n_rxclk_carry_refused = 0;             ///< clock carries erased by the jump gate (network.carry_rx_clock_jump_k) at harvest: |one-step prediction innovation| > k*sigma means the receiver (or the solve) stepped, so both the bias and the drift are dropped and re-born cold.  The wired response of invariant #1 for a quantity that has no other jump detector on the network path.
        long long n_rxclk_carry_gap_drop = 0;            ///< clock-family carries dropped at the harvest prune for exceeding carry_rx_clock_max_gap_s without a sighting (the absent-station edge)
        // --- The jump gate's own scale, so a leg can say what it would have caught ---
        // The gate is |innovation| > k*sqrt(var_pred), and var_pred is per station-epoch
        // (harvested variance + drift extrapolation + q), so "the gate" is a distribution,
        // not a constant.  Without this census a report can quote only k; with it, it can
        // quote the metres an event has to exceed.  Armed-only (the whole rx-clock harvest
        // block is inside `if (rxclk_blk_on)`), so an off run moves none of it.
        long long n_rxclk_gate_tests = 0;                ///< station-epochs the jump gate tested (primary clock only; an ISB has no rate)
        double    sum_rxclk_gate_m = 0.0;                ///< Σ k*sqrt(var_pred) over those tests, metres
        double    min_rxclk_gate_m = 0.0;                ///< smallest k*sqrt(var_pred) seen (0 = never tested)
        double    max_rxclk_gate_m = 0.0;                ///< largest  k*sqrt(var_pred) seen (0 = never tested)
        double    max_rxclk_innov_m = 0.0;               ///< largest |innovation| seen, metres: the largest step the gate saw
        long long n_raim_pool_rxclk_rows = 0;            ///< post-fit RAIM pool admissions (station-epochs x rows) whose admitted row is one of the clock-family columns' whitened block rows: the SPEC_amb_iono_split_overcount §7 item 4 interaction measured for the rho half, as n_raim_pool_zwd_rows / n_raim_pool_iono_rows measure it for theirs.  Counted only; the pool rule is unchanged.
        long long n_raim_pool_zwd_rows  = 0;             ///< post-fit RAIM pool admissions (station-epochs) whose admitted row is the ZWD's whitened block row: the §7 item 4 interaction, measured (the pool admits by |A_r(row, amb_col)| > 0.5, and a ZWD block row mixes into ambiguity columns through L⁻¹).  Counted only; the pool rule is unchanged.
        /// The carried-phase RAIM pools' admissions per contribution kind (card NOM step N2b,
        /// filter/NetworkRowClassify @c raimPoolAdmitsKind): the post-fit pool
        /// (NetworkRaim.cpp; one walk per epoch it runs) and the within-epoch assess pool
        /// (NetworkSolveGlue.cpp; one walk per assess call, the first and each replay's).
        /// Read at run end; the declared rule admits the observation phase rows, the
        /// coupling and block carried-prior rows and the SD-EWL constraint rows, so the
        /// non-data rows' share is on record for the science card RPA.
        RaimPoolAdmissionTally raim_pool_postfit;
        RaimPoolAdmissionTally raim_pool_within;   ///< As @c raim_pool_postfit, for the within-epoch assess pool.
        long long n_pl_blk_refloat_rewhiten = 0;         ///< within-epoch RAIM refloats that re-whitened a block over its survivors (the re-coupling edge of invariant #7, counted)
        long long n_pl_blk_refloat_fail = 0;             ///< … whose re-whiten was non-SPD: the block's rows were dropped for that re-solve (a safe degradation: the refloated arcs re-float from data)

        // (C) One-solve refloats: the entry is erased after the harvest, so the arc
        //     refloats on the next epoch.  Selected by quality.
        long long n_cr_sh_conv_flush      = 0;  ///< whole-fleet one-shot flush at SH convergence (pl_refloat_on_conv), the largest single removal in the producer.  Family (C), not (A): the arcs are still tracked and the next harvest re-stashes them, so it costs one solve, not a departure.
        long long n_cr_sh_conv_flushes    = 0;  ///< how many times that flush fired (expected 1)
        long long n_cr_raim_attrib        = 0;  ///< attribution refloat, T4 (live only when network.raim_attribution is armed)
        long long n_cr_raim_consensus     = 0;  ///< consensus refloat, T3 (live only when raim_attribution is not armed; the two are a run-constant either/or, never a split)
        long long n_cr_gross_los          = 0;  ///< level entries erased by a gross line-of-sight declaration (card GL, docs/SPEC_event_harness.md §24.18).  Family (C) like the T4 erase: the arc is still tracked and its next harvest re-stashes it.  Zero unless network.gross_los_screen.

        // (D) One-solve frame skips: the frame stepped, so the carried value is stale in the
        //     old gauge; the same epoch's harvest re-stashes any still-tracked arc, so the
        //     cost is one solve of continuity.
        long long n_cr_eq34_reref         = 0;  ///< eq34 D4 chain-datum re-reference
        long long n_cr_phase_datum_reref  = 0;  ///< phase-datum family re-reference

        // Siblings on the same gap predicate, for other carries (departures).
        long long n_cr_zwd_gap_prune      = 0;  ///< ZWD carry entries dropped by the pl_max_gap_s prune
        // Card LGM, the per-line-of-sight phase nuisance state: every edge counted.
        long long n_pc_birth_rows         = 0;  ///< columns given the birth prior row (no live carry)
        long long n_pc_backstop_rows      = 0;  ///< planned carried columns the builder refused, given a birth row instead
        long long n_pc_block_folds        = 0;  ///< carried columns folded through the whitened block
        long long n_pc_fallback_folds     = 0;  ///< carried columns folded by the per-column fallback (non-SPD block)
        long long n_pc_stash              = 0;  ///< entries harvested
        long long n_pc_stash_fail         = 0;  ///< harvest attempts refused (non-finite or a failed regression)
        long long n_pc_gap_prune          = 0;  ///< entries pruned past the maximum gap
        long long n_pc_erase_raim         = 0;  ///< entries erased by the RAIM attribution erase of their line of sight
        long long n_pc_erase_gross        = 0;  ///< entries erased by the gross line-of-sight erase
        double    pc_abs_sum              = 0.0;///< sum of |estimate| over harvested entries (m)
        double    pc_sq_sum               = 0.0;///< sum of estimate^2 over harvested entries (m^2)
        long long n_cr_rpb_gap_prune      = 0;  ///< receiver-PB carry entries dropped by the pl_max_gap_s prune

        // --- Seam-scoped ZWD re-seed (network.zwd_seam_reseed) ---
        // On-path telemetry (standing invariant #4): 0 folds under an armed knob means the
        // window never armed (no warm resume, or no restored Z rows) or every fold was
        // skipped; check the resume log line as well.
        long long n_zwd_seam_folds        = 0;  ///< station-fold count inside the post-resume window
        long long n_zwd_seam_epochs       = 0;  ///< epochs inside the window with >= 1 fold
        // Resume frame restore (network.resume_frame_restore, §18.10): the identities of the
        // re-coupling edge, i.e. what the seam reconnected, so a 0 under an armed knob reads
        // as inert (standing invariant #4).
        long long n_resume_frame_anchors     = 0;  ///< phase-anchor incumbents restored
        long long n_resume_frame_targets     = 0;  ///< sticky pin targets restored
        long long n_resume_frame_cta         = 0;  ///< clock-anchor per-sat states restored
        long long n_resume_frame_cta_anchored = 0; ///< ...of which anchored
        long long n_resume_frame_clk_keys    = 0;  ///< CLK-R held-frame per-sat keys restored
        long long n_resume_frame_ledger      = 0;  ///< handover-ledger entries primed from restored leveling rows
        // Resume datum restore (RS-3, network.resume_datum_restore, SPEC_event_harness
        // §21.7): the re-coupling edge's identities for the two datum memories, and the
        // first-post-resume-epoch event counts that say whether the datum continued.  0
        // restored under an armed knob means inert (invariant #4); the three first-epoch
        // figures are counted on every warm resume, armed or not, so the control leg of an
        // A/B reports them from the same instrument.
        long long n_resume_datum_eq34_rx      = 0;  ///< sticky datum receivers restored (EQD)
        long long n_resume_datum_eq34_edges   = 0;  ///< receiver parent edges restored (EQP)
        long long n_resume_datum_sat_roots    = 0;  ///< satellite component roots restored (EQS)
        long long n_resume_datum_prev_targets = 0;  ///< last-epoch pin targets restored (EQT)
        long long n_resume_datum_prev_el      = 0;  ///< longevity elevations restored (EQE)
        long long n_resume_datum_retained     = 0;  ///< gap-retained tree edges restored (EQR)
        long long n_resume_datum_cb_prev      = 0;  ///< code-bias previous values restored (CBP)
        long long n_resume_datum_cb_seen      = 0;  ///< code-bias ever-seen satellites restored (CBS)
        long long n_resume_datum_cb_pair      = 0;  ///< code-bias IF-pin reference pairs restored (CBI); 0 here with CBP > 0 means the IF-pin half re-seeds anyway
        long long n_resume_datum_first_reref    = 0; ///< PHASE_DATUM/eq34_reref (sat,σ) at the first post-resume epoch
        long long n_resume_datum_first_reattach = 0; ///< PHASE_DATUM/eq34_reattach (tree-edge handovers) there
        long long n_resume_datum_first_cb_disc  = 0; ///< CB_DATUM/reseed_disc_landed satellites there
        long long n_pb_seam_keys_tested = 0; ///< phase-bias keys the first post-resume emit compared with a restored baseline, the jump test's own population (RS-4)
        long long n_pb_seam_keys_over   = 0; ///< of those, keys beyond discThresholdM (RS-4)
        long long n_pb_seam_marks       = 0; ///< constellations markDatumChange'd by the seam rule; 0 with the knob off, and 0 on a cold start (RS-4)
        // Resume emit-layer restore (RS-5, network.resume_emit_restore, SPEC_event_harness
        // §21.10): the re-coupling edge's identities for the two emit engines.  0 under an
        // armed knob means inert (standing invariant #4): the snapshot carried no ER / PF /
        // PS rows (a writer without network.state_snapshot_emit_capture) or no warm resume
        // happened, and the run-end line says which.
        long long n_resume_emit_rebase_keys = 0; ///< emission-frame keys restored into NetworkEmitRebase (ER)
        long long n_resume_emit_filter_keys = 0; ///< per-key posteriors restored into NetworkPbEmitFilter (PF)
        long long n_resume_emit_step_accums = 0; ///< per-constellation step-noise accumulators restored (PS)
        // Resume station-layer restore (RS-6, network.resume_station_restore,
        // SPEC_event_harness §21.13): the re-coupling edge's identities for the two station
        // memories.  0 under an armed knob means inert (standing invariant #4): the
        // snapshot carried no RC / SD rows (a writer without
        // network.state_snapshot_station_capture), the run arms neither carry, a sub-switch
        // withheld that half, or no warm resume happened; the run-end line says which.
        long long n_resume_station_rxcb  = 0; ///< rx-CB carry entries restored (RC)
        long long n_resume_station_shdev = 0; ///< entries of the T5 per-LOS deviation carry restored (SD)
        // Resume canonical-signal restore (RS-8, network.resume_signal_canon_restore,
        // SPEC_event_harness §21.16).  0 stations under an armed knob means inert (standing
        // invariant #4): the snapshot carried no CN rows (a writer without
        // network.state_snapshot_signal_capture) or no warm resume happened, and the
        // run-end line says which.  The disagree count measures the problem directly: it is
        // the number of restored entries that this run's first epoch would have latched
        // differently, i.e. the flips the restore prevents, so a nonzero value confirms the
        // restore acted, and a zero one says the seam epoch happened to agree.
        long long n_resume_canon_stations = 0; ///< stations whose canonical latch was restored (CN)
        long long n_resume_canon_entries  = 0; ///< canonical entries restored across those stations
        long long n_resume_canon_audited  = 0; ///< restored entries the first post-resume epoch's tally could be compared against
        long long n_resume_canon_disagree = 0; ///< ...of those, the ones this run would have decided differently
        // Resume rx-CB linearization-point restore (RS-9, network.resume_rxcb_lin_restore,
        // SPEC_event_harness §21.18): the re-coupling edge's identities for the frame
        // against which the rx-CB inject gate judges the restored carry.  0 entries under an
        // armed knob means inert (invariant #4): the snapshot carried no RL rows (a writer
        // without network.state_snapshot_rxcb_lin_capture), or no warm resume happened.
        //
        // `_uncovered` is scoped to the keys for which the RL rows carry a reference.  The
        // rows pair 1:1 with the RC rows, so that is exactly the set with an rx-CB carry,
        // i.e. the set whose x_lin the inject gate reads.  A code-bias family with no carry
        // (GPS_IFB5 beside the carried GPS_DCB on a GPS L1/L2/L5 station) takes the
        // one-epoch seed and is not a miss, so it is not counted.  A nonzero `_uncovered`
        // therefore means a carried key whose restored reference could not be applied (a
        // non-finite value, or a state index this run does not hold), which is a defect.
        long long n_resume_rxcb_lin_stations  = 0; ///< stations handed an RL override map
        long long n_resume_rxcb_lin_entries   = 0; ///< RL entries installed across them
        long long n_resume_rxcb_lin_uncovered = 0; ///< carried code-bias keys whose restored reference could not be applied (expected 0)
        long long n_resume_slip_warmup_epochs = 0; ///< epochs the MW/LLI drain ran diagnostic-only post-resume
        long long n_resume_d5_warmup_epochs = 0;   ///< epochs the card-4 D5 innovation gate ran diagnostic-only post-resume (RS-2b)
        // Card-1 coupling-regressor restore (network.resume_coupling_restore, §18.12): both
        // edges of the seam (standing invariant #7).  rows/zrows/terms/dict are the
        // re-coupling edge's identities; seed_reinit is the decoupling edge (columns the
        // write epoch was re-drawing, diverted on purpose); seam_present/seam_departed are
        // what the first fold bound to.  Rows > 0 with seam_present == 0 indicates a
        // miswire, not a clean run.
        long long n_resume_cpl_rows        = 0;  ///< L leveling rows whose regression was rebuilt
        long long n_resume_cpl_zrows       = 0;  ///< Z (ZWD) rows whose regression was rebuilt
        long long n_resume_cpl_terms       = 0;  ///< regressor entries restored across both
        long long n_resume_cpl_dict        = 0;  ///< shared-column dictionary entries read
        long long n_resume_cpl_orphan      = 0;  ///< QL/QZ rows with no live carry to attach to
        long long n_resume_cpl_seed_reinit = 0;  ///< tu_reinit_hist seeds planted (loose anchor / eph cutover)
        long long n_resume_cpl_seam_present  = 0;  ///< first warm epoch: coupling terms that resolved to a live column
        long long n_resume_cpl_seam_departed = 0;  ///< ...that marginalized (departed or reinit-diverted)
        long long n_resume_cpl_diag_border   = 0;  ///< border keys (C1) that folded diagonally at the first post-resume fold (the known one-epoch card-1 asymmetry; see networkInjectD2CarryPrior in NetworkRunContext.cpp)
        // First-post-resume-harvest observatory (measure-only, byte-identical): the restored
        // M against the epoch's own freshly derived M_d for the same key.  This is the only
        // direct evidence that the restored row was frame-valid, and it needs no second
        // leg.  It also separates "the regressors were stale" from "the residual is the
        // diagonal C1 border" if the trips do not reach 0.
        long long n_resume_cpl_census_rows      = 0;  ///< rows compared against their own fresh harvest
        long long n_resume_cpl_census_terms     = 0;  ///< identities matched in both vectors
        long long n_resume_cpl_census_unmatched = 0;  ///< restored identities the fresh harvest did not produce
        // The dense D2 covariance side-car across the resume seam (§18.12 (2a)).
        long long n_resume_cpl_cov_keys    = 0;  ///< dense-carry keys restored from the snapshot's QK/QP records
        long long n_resume_cpl_cov_dropped = 0;  ///< QK keys with no restored D row (row and column dropped)
        /// Carried D rows present at a warm resume with a dense (full-cov) carry (RS-7,
        /// §21.14).  The marker that separates "no warm resume happened" from "the border
        /// came back complete": n_resume_cpl_diag_border is 0 in both cases, so the run-end
        /// readout needs its own denominator.
        long long n_resume_dense_carry_keys = 0;
        /// Clock-anchor decouples of card Q3 (nis + abs + gap + datum + ext) accumulated over
        /// the first post-resume epoch, latched when the census retires (RS-7, §21.14).  The
        /// readout of §18.11: a restored carry injected as a plain stiff prior on a border
        /// with no cross-covariance out-votes the anchored clock priors and trips them at the
        /// first fold, so "trips at the seed's level" is the check on the border.
        /// Structurally 0 on a lineage that does not arm network.clock_anchor; the run-end
        /// line says which case it is (invariant #4).
        long long n_resume_first_fold_cta_trips = 0;
        // Readmit continuity (E2), both edges (standing invariant #7): parked is the
        // decoupling edge (what left, and when); seeded is the re-coupling edge (what came
        // back and was re-validated: by the Q3 guard for the clock, by the stash-side seed
        // check for every kind); expired / dropped are what was not reconnected, and why.
        long long n_readmit_parked        = 0;  ///< entries of the D2 Clock/CodeBias carry parked at a satellite's departure
        long long n_readmit_seeded_clk    = 0;  ///< clock columns seeded from a parked entry on re-registration
        long long n_readmit_seeded_cb     = 0;  ///< code-bias columns seeded
        long long n_readmit_seeded_anch   = 0;  ///< ...of the clock seeds, folded anchored over the gap (instead of loose)
        long long n_readmit_expired       = 0;  ///< parked entries dropped: the gap exceeded the horizon
        long long n_readmit_dropped_datum = 0;  ///< parked entries dropped: a clock-datum change retired their frame
        long long n_readmit_dropped_stale = 0;  ///< parked entries dropped: the column returned but the entry was not consumed (skip-stash / pinned)
        // The DD trace's reference identity, made visible (§6.12.5).  Standing invariant #4:
        // a reference policy that never changes the elected satellite is inert, and only a
        // count shows it.  Both counters are written on every traced epoch regardless of
        // policy, so the `lowest` default reports its own switch rate too.
        long long n_ddfrac_ref_switch     = 0;  ///< (cons,signal) reference changes across traced epochs
        long long n_ddfrac_ref_epochs     = 0;  ///< (cons,signal) reference elections performed (denominator)
        long long n_ddfrac_ref_age_win    = 0;  ///< elections where `oldest` chose a different satellite than `lowest` would have (0 means the policy is inert on this run)
        long long n_readmit_events        = 0;  ///< re-admissions detected (clock column back after >= kReadmitMinGapEp loop epochs)
        long long n_readmit_seed_moved    = 0;  ///< nis/abs trips of the Q3 guard within kReadmitVerifyWindowS of a clock seed: the data rejected the seed
        long long n_readmit_seed_check    = 0;  ///< stash-side seed-vs-solve checks run (all seeded kinds)
        long long n_readmit_seed_outlier  = 0;  ///< ...of which |e|/sqrt(S) > kReadmitSeedK
        long long n_readmit_marg_windows  = 0;  ///< coupling marginalization windows opened
        long long n_cpl_readmit_diverted  = 0;  ///< coupling terms diverted to the departed route by a re-admission window
        long long n_snap_parked_excluded  = 0;  ///< parked entries excluded from written snapshots

        // --- The carry-domain lane observatory ---
        // Measure-only: it computes the lane response of the free re-level and histograms
        // it, writing nothing back, so an armed run is byte-identical on every solve
        // artifact.  The |delta| distribution decides whether an apply leg is worth
        // building at all, so the counters below are reconciled by the closure identity.
        //
        // delta = [C_a(t+)/lam_a - C_b(t+)/lam_b] - [C_a(t-)/lam_a - C_b(t-)/lam_b] - dM
        // The residual is eps_L(t+) - eps_L(t-) - eps_MW, with eps_MW = 0.351 cyc on GPS
        // L1/L2 and 0.051 cyc on L2/L5 EWL at sigma_P = 0.3 m.  The carry-frame hypothesis
        // predicts the bulk of |delta| below about 2 cyc; a bulk above 25 cyc is what a
        // frame mismatch looks like, and is the falsifier to watch for.
        //
        // Closure identity (invariant #4: a channel that does not close is a miswire, not
        // a null):
        //     n_lane_attempts == n_lane_measured
        //                      + n_lane_skip_no_row     + n_lane_skip_no_mw
        //                      + n_lane_skip_unmeasured + n_lane_skip_pair_changed
        //                      + n_lane_skip_no_freed_band
        //                      + n_lane_skip_no_pre     + n_lane_skip_stale_pre
        //                      + n_lane_skip_no_post
        //                      + n_lane_skip_bridged    + n_lane_skip_bad_lambda
        //     n_lane_measured  == sum(n_lane_hist[0..8])
        long long n_lane_attempts        = 0;  ///< (station, satellite) pairs with >= 1 freed signal this epoch: the denominator.  Counted over the freed set, not over the raw rows, so an arc the raw pass never saw is still visible (as n_lane_skip_no_row) instead of leaving the denominator unnoticed.
        long long n_lane_measured        = 0;  ///< pairs that yielded a finite delta.
        long long n_lane_skip_no_row     = 0;  ///< the freed (station, satellite) had no raw-observable row this epoch at all (satellite not tracked by the raw pass, or station absent).
        long long n_lane_skip_no_mw      = 0;  ///< a raw row existed but carried no usable second band (not dual).
        long long n_lane_skip_unmeasured = 0;  ///< raw-MW entry present but no adjacent predecessor (step not formable).
        long long n_lane_skip_pair_changed = 0;///< the adjacent raw predecessor used a different band pair, so ΔMW is not in this lane's cycles.
        long long n_lane_skip_no_freed_band = 0;///< the satellite had a freed band, but not one of this pair's two: both carries are continuous, so there is no re-level response to measure.  (Third-band frees: L5 on a triple posture, or a second channel of the same band under channel_mode=all.)
        long long n_lane_skip_no_pre     = 0;  ///< a band had no live pre-event carry (new arc, or retired last epoch).
        long long n_lane_skip_stale_pre  = 0;  ///< a pre-event carry existed but was not stashed at the immediately preceding processed epoch, so differencing it against a one-epoch ΔMW would compare unequal intervals.
        long long n_lane_skip_no_post    = 0;  ///< a band was not re-stashed this epoch.  Tested on last_seen_s, not on map presence: an un-refreshed entry survives for a full pl_max_gap_s, and accepting it would contribute the pre value as the post value, collapsing δ to a one-band code re-level (top bins) or to exactly −ΔMW (≈0.35 cyc, bottom bins).  Either direction would produce a false verdict.
        long long n_lane_skip_bridged    = 0;  ///< a band was delta-bridged: its carry is already re-based, so the lane response is the bridge's, not the free re-level's.
        long long n_lane_skip_bad_lambda = 0;  ///< non-finite / non-positive wavelength on either band.
        long long n_lane_both_freed      = 0;  ///< ...of the measured, pairs where both bands were freed (vertex-wide response).
        long long n_lane_one_freed       = 0;  ///< ...of the measured, pairs where exactly one band was freed (band-scoped response; its partner's carry is continuous).
        double    lane_abs_delta_sum_cyc = 0.0;///< Σ |delta| over measured pairs (cycles), uncapped.
        double    lane_abs_delta_max_cyc = 0.0;///< max |delta| (cycles).
        /// |delta| histogram, bins < 0.1 / 0.25 / 0.5 / 1 / 2 / 5 / 10 / 25 / >= 25 cyc.
        long long n_lane_hist[9]         = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        // --- Instrument additions of §7.6 ---
        // (b) Band-scoped-only histogram: the one_freed subset of n_lane_hist, so the
        //     vertex-wide distribution is the bin-wise difference and the two responses
        //     (the rung-2 contrast) separate.  Closure:
        //     sum(n_lane_hist_bandscoped) == n_lane_one_freed.
        long long n_lane_hist_bandscoped[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        // (c) Cause split over measured pairs, as bit counts (a pair freed by two
        //     detectors increments two of these), so they sum to >= n_lane_measured.
        long long n_lane_meas_mw   = 0;  ///< measured pairs with the MW bit.
        long long n_lane_meas_lli  = 0;  ///< measured pairs with the LLI bit.
        long long n_lane_meas_iftd = 0;  ///< measured pairs with the IF-TD bit.
        // (d) T4-side coverage: the same delta measured on pairs whose carries the RAIM
        //     attribution/consensus erase refloated, the channel that frees most arcs in
        //     the field and is structurally invisible to free_ambs.  Pre-erase values are
        //     captured at the erase site (lane_t4_pre) and measured at the next harvest.
        //     Separate closure: n_lane_t4_attempts == n_lane_t4_measured +
        //     Σ n_lane_t4_skip_*.
        long long n_lane_t4_captured     = 0;  ///< (station,sat,signal) arcs captured at the erase sites.
        long long n_lane_t4_attempts     = 0;  ///< (station,sat) pairs with >= 1 captured band last epoch.
        long long n_lane_t4_measured     = 0;  ///< pairs that yielded a finite T4 delta.
        long long n_lane_t4_skip_no_row  = 0;  ///< no raw-observable row this epoch.
        long long n_lane_t4_skip_no_mw   = 0;  ///< raw row not dual.
        long long n_lane_t4_skip_unmeasured = 0;   ///< no adjacent raw predecessor.
        long long n_lane_t4_skip_pair_changed = 0; ///< predecessor used a different band pair.
        long long n_lane_t4_skip_gauge   = 0;  ///< the pair's (sat,σ) was eq34-re-referenced this epoch, so the capture predates the frame step.  The detector path gets this guard for free because lane_pre lives in local_carry; the capture is a copy and must be interlocked.
        long long n_lane_t4_skip_no_pre  = 0;  ///< a band of the MW pair had no capture.
        long long n_lane_t4_skip_stale_pre = 0;///< capture not from the immediately preceding harvest.
        long long n_lane_t4_skip_no_post = 0;  ///< a band was not re-stashed this epoch (the same last_seen_s test as the detector path).
        long long n_lane_t4_skip_bad_lambda = 0; ///< non-finite wavelength/delta.
        double    lane_t4_abs_delta_sum_cyc = 0.0;
        double    lane_t4_abs_delta_max_cyc = 0.0;
        long long n_lane_t4_hist[9]      = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        // Declaration telemetry for both edges (invariant #7) and hygiene accounting
        // (QUEUE item 4 R2/R4).
        long long n_staf_departs          = 0;  ///< depart events (EWMA ratio > threshold; R2)
        long long n_staf_returns          = 0;  ///< return events (hysteresis 0.8×; R2)
        double    staf_max_ratio          = 0.0;///< run-max EWMA ratio to the network median
        long long n_staf_epochs           = 0;  ///< epochs the factor was computed on
        long long n_t3_untestable_cells   = 0;  ///< s_raw==0 cells excluded from the T3 opportunities (R4)
        // Invariant #4 counters of the LV-6 census.
        long long n_lvlauth_epochs   = 0;       ///< epochs with a FLEET row
        long long n_lvlauth_immature = 0;       ///< LOS skipped at the min_n screen
        double    lvlauth_sum = 0.0;            ///< Σ L_cs (teardown mean)
        double    lvlauth_min =  1.0e30;        ///< run min L_cs (TECU)
        double    lvlauth_max = -1.0e30;        ///< run max L_cs (TECU)
        // Epochs whose emission the SH health gate withheld (Phase 2.2).  Not a subset of
        // n_skipped: the solve, carry and QC all ran (the gate is emission-only), so these
        // epochs are counted nowhere else.
        size_t n_sh_gate_withheld = 0;
        // Invariant #4 counters for the per-satellite consumer readout (card CST,
        // network.trace_consumer_sat).  0 epochs on an armed run means the consumer
        // deferred every epoch (admission gates) and the file is a bare header; 0 rows
        // with non-zero epochs is impossible by construction.
        long long n_cst_epochs = 0;  ///< consumer epochs that contributed >= 1 row
        long long n_cst_rows   = 0;  ///< (satellite, raw phase signal) rows written
        // The edges of invariant #7 for the consumer's ambiguity level re-validation,
        // counted (card ALC, network.closed_loop.amb_level_check).  All four stay 0 on an
        // off run.  n_amb_level_checks == 0 on an armed run means no satellite ever filled
        // a window (the instrument never fired) however clean the run reads; trips == 0
        // with checks > 0 is the real null.  In `act` mode n_amb_level_refloats ==
        // trips_wl + trips_if by construction, and in `observe` mode it is 0, which is how
        // the mode is read back off the run.
        long long n_amb_level_checks   = 0;  ///< full-window (satellite, epoch) evaluations
        long long n_amb_level_trips_wl = 0;  ///< wide-lane test trips
        long long n_amb_level_trips_if = 0;  ///< iono-free test trips
        long long n_amb_level_refloats = 0;  ///< satellites refloated (two ambiguities each)
        // The four declaration classes of the de-commoned phase jump test (card JT,
        // network.disc_jump_test_decommon): the identities of invariant #7, counted (the
        // per-cell PB_JUMP rows carry sat / signal / raw dPB / c_s / de-commoned dPB).  All
        // four stay 0 on an off run.  `decommon_only` is the class the raw test missed (a
        // frame move hid a real per-satellite step); `raw_only` is the class it
        // fabricated (the frame moved, this satellite did not).  A run with 0 in both is a
        // knob that changed nothing, however clean the log reads.
        long long n_jt_decommon_only = 0;  ///< de-commoned declared, raw would not
        long long n_jt_both          = 0;  ///< both tests declared
        long long n_jt_raw_only      = 0;  ///< raw would have declared, de-commoned did not
        long long n_jt_fallback_raw  = 0;  ///< signal population below kDiscDecommonMinSats, so the raw result stands
        // --- Per-channel discontinuity marks (card MS, network.disc_mark_scope) ---
        // The decoupling identity of invariant #7 for a per-satellite mark: which channel
        // generations it stepped, and which it was withheld from.  All zero under the
        // default `sat` scope (nothing is counted there), so a run that reports 0/0/0 with
        // the scope armed has inert wiring however clean it reads.
        long long n_mark_clk = 0;  ///< satellite-epochs whose clock generation a mark bumped
        long long n_mark_cb  = 0;  ///< (sat, signal) code-bias cells a mark bumped
        long long n_mark_pb  = 0;  ///< (sat, signal) phase-bias cells a mark bumped
        long long n_mark_suppressed_by_scope     = 0;  ///< (sat, channel) marks the scope withheld, in total
        long long n_mark_suppressed_by_scope_clk = 0;  ///< …of which the clock channel
        long long n_mark_suppressed_by_scope_cb  = 0;  ///< …of which the code-bias channel
        long long n_mark_suppressed_by_scope_pb  = 0;  ///< …of which the phase-bias channel
        // --- The emit-side phase-bias hold (card PBH, network.emit_pb_hold) ---
        // On-path telemetry of invariant #4 and the two edges of invariant #7.  All zero on
        // an off run, so a run reporting 0 held emissions has an inert hold however clean
        // the log reads.  The cost of the decoupling edge is not a count but a distribution
        // (ctx.carry.pb_hold_dev_abs_m, |estimate - held| over every held emission) plus the
        // coupled-mark clock step (pb_hold_coupled_step_m); the re-coupling edge is
        // n_pbh_gen_first + n_pbh_gen_gap, one PB_HOLD/gen_start event each.
        long long n_pbh_keys           = 0;  ///< distinct (sat, signal) keys the hold ever held
        long long n_pbh_gen_first      = 0;  ///< generations started by a first sighting
        long long n_pbh_gen_gap        = 0;  ///< generations started by a gap > emit_pb_hold_max_gap_s
        long long n_pbh_held_emissions = 0;  ///< emissions whose value the hold overwrote
        long long n_pbh_coupled_marks  = 0;  ///< coupled clock+phase-bias marks reaching a held key
        // --- Card LAH (network.emit_pb_hold_phase_clock) ---
        long long n_pbh_frz_moved      = 0;  ///< held emissions whose value the transfer moved (non-zero)
        long long n_pbh_frz_noclock    = 0;  ///< held emissions on an epoch without the satellite's emitted clock (last transfer reused)
        long long n_pbh_frz_noref      = 0;  ///< held emissions of a generation started without a clock (held plainly)
        long long n_pbh_frz_epochs     = 0;  ///< epochs that advanced M (at least one satellite with a clock at both emissions)
        long long n_pbh_frz_coupled    = 0;  ///< coupled clock+phase-bias marks whose clock step the mode transferred
        // --- Card SMH (network.emit_pb_hold_smoothed) ---
        long long smh_births           = 0;  ///< clones born on a stashed clock
        long long smh_births_pinned    = 0;  ///< clones born on the constellation's pinned clock (a transfer of zero)
        long long smh_births_deferred  = 0;  ///< clones born at least one stash after their birth was queued (section 9)
        long long smh_rebirths         = 0;  ///< clones born after a disengagement within their generation (section 9)
        long long smh_defer_loose      = 0;  ///< stashes at which a pending birth waited because card Q3's anchor did not hold the clock
        long long smh_defer_nostash    = 0;  ///< stashes at which a pending birth waited for a stashed clock (or a dense stash)
        long long smh_pending_dropped  = 0;  ///< pending births dropped because their generation was superseded
        long long smh_bridges          = 0;  ///< switches from a live clone to the phase-clock transfer at a disengagement
        long long smh_held_bridged     = 0;  ///< held emissions on the phase-clock transfer while the key waits for a clone
        std::vector<double> smh_defer_ep;    ///< epochs waited, per deferred birth
        long long smh_updates          = 0;  ///< clone-epochs advanced by the exact route
        long long smh_deaths[static_cast<int>(PbHoldCloneDeath::kCount)]{};   ///< clone deaths by cause
        long long smh_reverts          = 0;  ///< held keys switched to LAH's transfer by their clone's death
        long long smh_held_smoothed    = 0;  ///< held emissions that wrote the clone's transfer
        long long smh_held_lah         = 0;  ///< held emissions that wrote LAH's transfer after a revert (the rest of the generation)
        long long smh_terms_s          = 0;  ///< clone terms updated by the one rule
        long long smh_terms_d          = 0;  ///< clone terms re-attributed through P
        long long smh_terms_outside    = 0;  ///< clone terms outside the dense block (kappa or marginalized)
        long long smh_deficit_orphan   = 0;  ///< clock-family deficits with no pivot (dropped)
        long long smh_epochs_restricted = 0; ///< epochs whose exact inputs were restricted to the keys the solve folded
        long long smh_keys_unfolded    = 0;  ///< S keys the solve did not fold (a withdrawn clock, a skipped handover), summed
        long long smh_w_invalid        = 0;  ///< held emissions whose W is void (the parent clock left S this generation)
        std::vector<double> smh_xfer_abs;    ///< |the clone's transfer| per smoothed held emission (m)
        std::vector<double> smh_diff_abs;    ///< |the clone's transfer - LAH's| per smoothed held emission (m)
        std::vector<double> smh_phi_abs;     ///< |Phi| per smoothed held emission (m)
        std::map<std::pair<char, int>, std::pair<double, long long>> smh_r;   ///< (constellation, hour) -> (sum r, count), r = Phi^2 / (W - V_post)
        // --- Card SMD (the block clones; carried whenever network.emit_pb_hold_smoothed) ---
        long long smh_pv_births         = 0;  ///< block clones born on a stashed clock
        long long smh_pv_births_pinned  = 0;  ///< block clones born on the constellation's pinned clock
        long long smh_pv_rebirths       = 0;  ///< block clones born again within their generation
        long long smh_pv_defer_anchor   = 0;  ///< stashes at which a pending block birth waited for the anchor
        long long smh_pv_defer_nostash  = 0;  ///< stashes at which a pending block birth waited for a stashed clock
        long long smh_pv_defer_nopair   = 0;  ///< stashes at which a pending block birth waited for phase biases on two bands
        long long smh_pv_pending_dropped = 0; ///< pending block births dropped with their generation
        long long smh_pv_updates        = 0;  ///< block-clone epochs advanced (both rows)
        long long smh_pv_deaths[static_cast<int>(PbHoldCloneDeath::kCount)]{};   ///< block-clone deaths by cause
        long long smh_pv_follow[static_cast<int>(PbHoldFollow::kCount)]{};       ///< block-clone retirements by the step followed
        long long smh_pv_reref_unmarked = 0;  ///< block clones retired at a re-reference whose satellite carried no phase-channel mark
        long long smh_pv_steps_unmarked = 0;  ///< live block-clone steps of D above the smallest emitter jump bar of the pair (passed as motion), per held key
        std::map<int, long long> smh_pv_steps_unmarked_hour;   ///< the same by hour of the run
        double    smh_pv_step_unmarked_max = 0.0;  ///< the largest such step (m)
        long long smh_pv_held           = 0;  ///< held emissions written from a block clone (the block member)
        std::map<std::string, long long> smh_pv_pairs;   ///< the pair at birth ("L1C+L2W") -> block births
        std::vector<double> smh_pv_phi_abs;   ///< |Phi_D| per live block clone at a held emission (m)
        std::vector<double> smh_pv_lvl_abs;   ///< |Phi_c - Phi_D| (the level part) per live block clone at a held emission (m)
        std::map<std::pair<char, int>, std::pair<double, long long>> smh_pv_r;   ///< (constellation, hour) -> (sum r_D, count), r_D = Phi_D^2 / (W_D - V_D)
        // Observability of the prioritized-datum drop-and-heal: epochs where the gate threw
        // and the near-null phase-bias soft anchor recovered it, and the total columns
        // anchored.  Without these a run that recovers every epoch would look successful
        // with no trace (the success log line is at debug level).
        size_t n_healed_epochs = 0, n_healed_cols = 0;
        // The same quantities counted on every fired attempt, recovered or not.  The pair
        // above counts successes only, so an attempt that fires every epoch and never
        // rescues the gate would read as "0 healed"; the withhold warning in
        // NetworkSolveGlue then carries "drop-and-heal failed" as a trailing clause.  That
        // is the inert-looking but live wiring standing invariant #4 exists to catch, so
        // the attempt is counted separately: attempts > 0 with recoveries == 0 is the
        // signature of an attempt that aims at the right columns but cannot resolve the
        // residual alone (card PA's own insufficiency criterion, PA5).
        size_t n_heal_attempt_epochs = 0, n_heal_attempt_cols = 0;
        // Epochs on which the admitted-OSB physical-bound prior was folded, and the
        // cumulative column count.  Standing invariant #4: an armed KP run with
        // n_kp_epochs == 0 means the prior never reached a solve (no registered code
        // columns, or the W6 block never ran), which clean-looking log output would not
        // reveal.
        size_t n_kp_epochs = 0, n_kp_cols = 0;
        bool   kp_margin_logged = false;   ///< one-shot latch for the K2 margin check
        // Epochs on which the satellite code-bias datum was folded with no ionosphere
        // constraint behind it (RF-0b, network.w6_iono_constraint = none), and the
        // cumulative count of sum-to-zero (constellation, signal) rows in those epochs,
        // which are the datum in that posture.  Standing invariant #4: an armed "none" run
        // with n_w6_iono_none_epochs == 0 never reached the W6 block at all (no registered
        // code columns), and one with n_w6_iono_none_s2z_rows == 0 folded the IF pin
        // against nothing; in both cases the satellite code biases have no datum at all,
        // which a clean-looking log would not reveal.
        size_t n_w6_iono_none_epochs = 0, n_w6_iono_none_s2z_rows = 0;
        // Epochs on which the hard per-satellite geometry-free datum folded (RF-0c,
        // network.sat_gf_datum), the cumulative row count, and, under broadcast_tgd, how
        // those rows were targeted: `tgd` a non-zero c*T_GD from the navigation message,
        // `tgd0` a satellite the constellation broadcasts at T_GD = 0 exactly (physical,
        // just numerically zero), `fallback` a satellite with no navigation record or
        // whose pinned pair is not (L1, L2).  The split matters because all three look
        // identical on the wire: only the fallback count says the run pinned a value the
        // navigation message never asserted.
        size_t n_sat_gf_epochs = 0, n_sat_gf_rows = 0;
        size_t n_sat_gf_tgd = 0, n_sat_gf_tgd0 = 0, n_sat_gf_fallback = 0;
        // The per-signal extension (RF-0d).  `extra` counts the rows beyond the
        // ionosphere-free pair (which make a sparse channel's level a datum), `isc0` how
        // many of those asserted a zero inter-signal correction the navigation message
        // never stated, `sameband` the columns deliberately left estimable, and
        // `s2z_suppressed` the sum-to-zero rows dropped because the datum already pins
        // every column of that channel.
        size_t n_sat_gf_extra = 0, n_sat_gf_isc0 = 0;
        size_t n_sat_gf_sameband = 0, n_sat_gf_s2z_suppressed = 0;
        // e1's absolute IF-level rows (and the satellites they sum), and e2's split of
        // geometry-free rows into carried (a change pin on the previous solved value) and
        // anchored (first sighting, the absolute broadcast target), RF-0e.  The anchored
        // count is the only place the broadcast frame is re-asserted under e2, so a run
        // with 0 of them has carried its frame from epoch 1 and never re-checked it.
        size_t n_if_level_rows = 0, n_if_level_sats = 0;
        size_t n_sat_gf_carried = 0, n_sat_gf_anchored = 0;
        // Band-2 companion columns left free because a station bridged the two signals,
        // and those pinned at 0 because none did (RF-0f).  The pinned ones are the part of a
        // C2X product that is asserted rather than estimated, and they are not emitted.
        size_t n_isc_observed = 0, n_isc_pinned = 0;
        // The three dispositions of an extra-band code-bias column under
        // `network.sat_gf_extra_band_free` (card B5D): freed, because some station's cell
        // carried the pin pair's two bands and the signal; carried at its own previous
        // solved geometry-free combination, because none did this epoch; or held by a
        // level row in the channel's own frame at a first sighting with nothing to carry
        // (and then withheld from the wire).  Standing invariant #4: 0 freed with the knob
        // armed means the run exercised nothing.  `n_s2z_reseed_*` is the decoupling edge
        // (invariant #7) on the level the freed channels now stand on: the re-seeds of the
        // veteran-linked sum-to-zero row, and the satellites they marked for the
        // consumer's discontinuity channel.
        size_t n_extra_band_free = 0, n_extra_band_carried = 0, n_extra_band_held = 0;
        size_t n_s2z_reseed_rows = 0, n_s2z_reseed_sats = 0;
        // Zero-mean receiver code-bias prior rows folded over the run, one per
        // station-epoch per constellation (card RXCB-DATUM).  0 with the toggle armed is
        // the standing invariant #4 reading: no receiver DCB column ever existed.
        size_t n_rxcb_gauges = 0;
        // Station-epochs whose joint coupling-row covariance was non-SPD after the ridge
        // retry and fell back to the card-1 per-row scalar marginalization.  Warned once at
        // run end.
        size_t n_cpl_joint_fallback = 0;
        // Observability of cards 1/1d: run-total coupling-carry volume, as harvest-side
        // stash counts (non-empty regression, empty regression degraded to a plain
        // diagonal row, outright harvest failure) and inject-side term counts (present,
        // departed, κ-shrunk).  Without these, a run whose coupling carry degrades to
        // diagonal rows without a signal (or never runs) cannot be told apart from a
        // working one, and a byte-identical A/B would hide it (the card-1b/1d trap).
        // Logged once at run end when the card is armed.
        size_t n_cpl_stash = 0, n_cpl_stash_empty = 0, n_cpl_harvest_fail = 0;
        long long n_cpl_terms_present = 0, n_cpl_terms_departed = 0, n_cpl_terms_shrunk = 0;
        // Card CTU run totals (invariant 4): the per-station row and term counts folded
        // serially after PASS B, the epoch builds, and what the fold changed after the rows
        // were built from the shared decision (card PRM phase 1b): keys in S that a
        // clock-pivot handover re-expressed (the S-transform) or retired, on the handover
        // epochs the fold counts.
        CouplingExactCounters cxu{};
        long long cxu_epochs = 0, cxu_epochs_built = 0, cxu_epochs_unfactored = 0,
                  cxu_epochs_nostash = 0;
        long long cxu_keys_s = 0, cxu_keys_d[kCouplingExactRoutes]{};
        long long cxu_q_negative = 0;          ///< row inflations below 0 (a VCE factor < 1), clamped to 0
        long long cxu_fold_handover_epochs = 0, cxu_fold_diag_fallback = 0;
        long long cxu_fold_reexpressed_in_s = 0;   ///< keys in S folded on a variance a handover re-expressed after the rows
        long long cxu_fold_retired_in_s = 0;       ///< keys in S the fold retired at a handover (the rows carried them)
        long long cxu_pivot_missing = 0;       ///< built epochs with a clock constellation and no recorded pivot
        PivotFrameCounters pfq{};              ///< card PFQ run totals (filter/NetworkPivotFrameQ.hpp)
        ClockCloneCounters ccl{};              ///< card SCL run totals (filter/NetworkClockClone)
        SharedHistoryCounters shx{};           ///< card SCL2 X2 run totals (filter/NetworkSharedHistory)
        // Card PRM phase 1b: the decided-once fold disposition (networkCarryDecide before
        // PASS B, read by the rows and the fold).  prm_disp counts key-epochs by
        // (SharedParamKind, the disposition the fold applied); prm_departed the foldable
        // ones whose column had left the layout (the estimator marginalizes them).  A key
        // re-expressed between the decision and the fold (the handover S-transform, the
        // ephemeris cut-over re-level) is counted with its largest variance change: the rows
        // were built on the decided variance.  prm_undecided_at_fold: entries the fold met
        // with no decision this epoch (0 unless a path skips the pass).
        long long prm_decide_epochs = 0;
        long long prm_disp[4][kCarryDispositions]{};
        long long prm_departed[4]{};
        long long prm_reexpressed = 0, prm_reexpressed_epochs = 0, prm_undecided_at_fold = 0;
        double    prm_reexpressed_max_dvar = 0.0;
        // Card PRM phase 2: the carried references each mechanism moved, per site, through
        // forEachCarriedReference (filter/NetworkCarrySites), and the mechanism's calls.
        CarryMovedTally prm_ref_moved{};
        long long prm_ref_calls[kCarryMechanisms]{};
        // Card PRM phase 3a: the responses of the carried instances, per site of the
        // declared table (filter/NetworkResponses.hpp; the sites whose count source is
        // Increment), and the free-epoch latch per cause (keys latched with the bit set; of
        // those, keys with a carried entry at the response), the unique latched keys, the
        // keys with more than one cause, and the ambiguities the gap prune erased in the
        // epoch the stale-gap drop dropped them.
        long long prm_resp[kResponseSites]{};
        long long prm_latch_keys[kLatchCauses]{};
        long long prm_latch_carried[kLatchCauses]{};
        long long prm_latch_unique = 0, prm_latch_unique_carried = 0, prm_latch_multi = 0;
        long long prm_latch_epochs = 0;
        long long prm_pruned_after_drop = 0;
        // Card PRM phase 3b step 2: held phase-bias emissions (pb_hold) of a (satellite,
        // signal) family that a phase-bias shift moved (the eq34 D4 shift, the declared anchor
        // handover) and that stayed in the old frame: PbHold is exempt from both shifts with or
        // without network.carry_rebase_complete (its own design is owed).  Measure-only.
        long long prm_pbhold_unmoved_at_shift = 0;
        // Card CTU level monitor and raise.  Ratios are counted for a satellite from its
        // second epoch of code information on (its first solve is the code-only batch).
        long long clm_epochs = 0, clm_skip_datum = 0, clm_rows = 0, clm_below1 = 0,
                  clm_no_clock = 0, clm_raised = 0;
        double    clm_min_ratio = 0.0, clm_raised_sum = 0.0, clm_raised_max = 0.0;
        std::vector<double> clm_ratios;        ///< every counted ratio (the run-end median)
        std::map<std::string, long long> clm_raised_by_sat;   ///< raises per satellite (invariant 7)
        // Level innovation instrument (card CTU section 14).
        long long clm_innov_rows{0};           ///< satellite-epoch innovations formed
        long long clm_innov_tripped{0};        ///< satellite-epochs left out because the anchor withdrew the clock's prior
        double    clm_innov_nis_sum{0.0};      ///< summed NIS after epoch 15
        long long clm_restore_raised{0};       ///< card CTU G5: satellites raised on restore
        double    clm_restore_raise_sum{0.0};  ///< …summed variance added (m^2)
        double    clm_restore_raise_max{0.0};  ///< …largest (m^2)
        long long clm_innov_nis_n{0};          ///< NIS values summed
        long long clm_innov_nis_gt{0};         ///< NIS values above 6.63 (chi2(1), 1 %)
        std::vector<double> clm_win_nees;      ///< per window: the split-half truth-free level NEES
        std::vector<double> clm_win_rms;       ///< per window: rms over satellites of the mean innovation (m)
        std::vector<double> clm_win_T;         ///< per window: mean over satellites of T = nubar^2 / var(nubar)
        std::vector<double> clm_win_phi;       ///< per window: split-half noise over the white prediction
        long long clm_win_Tgt{0};              ///< window satellites with T above 6.63
        // Native broadcast clock a priori (card NBP).  `stamped 0` means the wire was never
        // stamped (inert wiring); `not_held > 0` means a consumer could not reconstruct.
        long long nbp_epochs{0};               ///< emitted epochs the wire stamp ran on
        long long nbp_stamped{0};              ///< clock records stamped (iode=k, C_wire = C_est + O)
        long long nbp_unstamped{0};            ///< clock records with no selectable record (value unchanged, no iode)
        long long nbp_held{0};                 ///< stamped records the store held (current or previous) at consume
        long long nbp_not_held{0};             ///< stamped records the store did not hold
        long long nbp_switches{0};             ///< stamp changes of a satellite between its emissions (cut-overs seen on the wire)
        double    nbp_offset_min_m{0.0};       ///< smallest O on the wire (m)
        double    nbp_offset_max_m{0.0};       ///< largest O on the wire (m)
        std::set<std::pair<std::string, int>> nbp_records;   ///< distinct (satellite, IODE) stamped
        // Phase-visible consistency instrument (card PVN).  `rows 0` means inert.
        long long pv_epochs{0};                ///< epochs the instrument ran
        long long pv_epochs_nocap{0};          ///< …of which without a dense fold capture (no rows)
        long long pv_rows{0};                  ///< satellite-epoch rows formed
        long long pv_su_nonpos{0};             ///< rows whose S- - S+ was not positive (no NIS)
        double    pv_nis_sum{0.0};             ///< summed NIS after epoch 15
        long long pv_nis_n{0};                 ///< NIS values summed
        long long pv_nis_gt{0};                ///< NIS values above 6.63 (chi2(1), 1 %)
        std::vector<double> pv_abs_e;          ///< |contrast update| after epoch 15 (m), the run-end median
        std::vector<double> pv_sqrt_sp;        ///< stated posterior sd sqrt(S+) after epoch 15 (m)
        std::vector<double> pv_sqrt_su;        ///< predicted update sd sqrt(S- - S+) after epoch 15 (m)
        std::vector<double> pv_win_nis;        ///< per window: mean NIS
        std::vector<double> pv_win_ratio;      ///< per window: rms update over rms predicted update sd
        long long clm_win_Tn{0};               ///< window satellites with a T
        // On-path telemetry for the departed-column gauge defect.  `held` counts
        // (constellation, kind) families whose coefficient sum was restored to its harvest
        // value against a truncated column; `orphan` counts families whose truncated mass
        // had no present recipient, i.e. the rows the projection could not make
        // gauge-invariant.  Standing invariant #4: armed with both at 0 means no row was
        // ever truncated, so the projection is inert, not clean.
        long long n_cpl_gauge_held = 0, n_cpl_gauge_orphan = 0;
        /// Orphan attribution: run totals per (constellation, SharedParamKind), as the
        /// orphan count and the summed |truncated mass| left gauge-dirty.  Structurally a
        /// channel of two or more constellations (a row's minority family can lose all its
        /// present columns while the majority family survives); GPS-only arms measure 0.
        /// Fed by foldAndDumpCouplingRows, read at teardown.
        std::map<std::pair<char,int>, std::pair<long long,double>> cpl_orphan_fam_total;
        /// The denominator of card 1g: terms diverted to the departed route by the widened
        /// (kind, sat, signal) probe specifically.  Armed with this at 0 means the
        /// realization-step feeds never matched a live coupling term: inert, not clean.
        long long n_cpl_reinit_diverted = 0;
        /// Run total of inject-site free-epoch skips (relevel_free_epoch;
        /// SPEC_slip_relevel_walk).  Armed with this at 0 on a slip-bearing run is a
        /// miswire (invariant #4): the drains latched but the inject never consulted, or no
        /// marked arc ever had a live carry.
        long long n_relevel_free_skips = 0;
        /// Run totals of the retap piece: arcs kept (level preserved, variance re-tapped),
        /// streak escalations to the erase, and flagged arcs with no live carry.
        long long n_relevel_retap_kept = 0, n_relevel_retap_escalated = 0,
                  n_relevel_retap_miss = 0;
        /// Corroborated retap: keeps that carried positive raw-observable continuity
        /// evidence, refusals because the raw level stepped, and refusals because the arc
        /// was unmeasured (no predecessor, or the qualification pass did not run).  Both
        /// refusal counters at 0 on a slip-bearing armed run is invariant #4's "clean log,
        /// dead wiring"; check them, not the keep count.
        long long n_relevel_retap_corr_kept = 0, n_relevel_retap_corr_stepped = 0,
                  n_relevel_retap_corr_unmeasured = 0;
        /// Run totals of the slant-iono carry (QUEUE 8b), for both coupling edges
        /// (invariant #7).  injected: priors folded (scope-gated).  kept_through_slip:
        /// slip-funnel entries whose bands were freed (kept).  erased_slip: funnel entries
        /// not freed (erased).  erased_t4: the T4/RAIM arc-erase hook.  evicted: gap
        /// eviction.  floor_hits: injects clamped by iono_carry_var_floor.  refused_datum:
        /// declared zero; the code-datum re-realization edge (GF-pin re-election /
        /// cb_excursion_reinit) ships bare, so a nonzero value means that edge got wired.
        /// Armed with injected = 0 on a slip-bearing run is a miswire (invariant #4).
        long long n_iono_carry_injected = 0, n_iono_carry_kept_through_slip = 0,
                  n_iono_carry_erased_slip = 0, n_iono_carry_erased_t4 = 0,
                  n_iono_carry_evicted = 0, n_iono_carry_floor_hits = 0,
                  n_iono_carry_refused_datum = 0;
        /// CS-GF authority (iono_carry_source = smoothed_gf): recursion resets (gap /
        /// slip-gate / first sighting) and blend steps.  Armed with blends = 0 is a miswire
        /// (invariant #4).
        long long n_iono_cs_resets = 0, n_iono_cs_blends = 0;
        /// Band-scoped T4 erase (network.raim_erase_band_scope = min_keep_one;
        /// docs/SPEC_event_harness.md §Z).  cells: isolated-arc erase cells the scope
        /// adjudicated.  erased / kept: bands freed and bands left pinned (kept is one per
        /// adjudicated multi-band cell by construction).  kept_over_gate: cells where every
        /// band stood above k·σ̂₀, so the pin is the least bad band rather than a clean one.
        /// single_band: cells skipped outright because the arc had one band and none to
        /// spare.  Armed with cells = 0 on a T4-active run is a miswire (invariant #4).
        long long n_bscope_t4_cells = 0, n_bscope_t4_erased = 0,
                  n_bscope_t4_kept = 0, n_bscope_t4_kept_over_gate = 0,
                  n_bscope_t4_single_band = 0;
        /// Anchor-slip handover (network.cycle_slip.anchor_slip_handover;
        /// docs/SPEC_event_harness.md §12.7).  staged: directives the seam put up (families
        /// whose anchor arc a slip response freed).  handed: of those, the ones the election
        /// re-hosted.  continuous / cold: the successor pinned at its own carried value_abs,
        /// or pinned at 0 because no usable ledger entry survived the variance gates; a cold
        /// outcome is a gauge re-realization and is never folded into the continuous total.
        /// no_successor: the ban left no candidate, so the family went unpinned for the
        /// epoch.  inert: the directive matched no family in this epoch's union (gated,
        /// bridged or pruned column).  Armed with staged = 0 on a run whose detectors fired
        /// does not prove a quiet network; it is the first thing to check (invariant #4).
        /// staged > 0 with handed = 0 is dead wiring.
        long long n_anc_slip_staged = 0, n_anc_slip_handed = 0,
                  n_anc_slip_continuous = 0, n_anc_slip_cold = 0,
                  n_anc_slip_no_successor = 0, n_anc_slip_inert = 0,
                  n_anc_slip_epochs = 0;
        /// MW detect-only admission census (network.cycle_slip.mw_evidence_refloat).
        /// Reduced serially from PassABuffers::sta_mw_admit after the parallel scan, and
        /// collected independently of the knob, on purpose.  legacy: admitted by the
        /// integer gate.  evidence: admitted by the strength rule alone with the knob armed.
        /// would_admit: the same population with the knob off, i.e. exactly what arming
        /// would change (an off run that reports 0 here says the card is inert on that
        /// lineage, with no run needed).  drop_loose / drop_unknown: the two refused
        /// populations, a flag with a known integer whose rounding residual exceeded
        /// turbo_edit_frac_max, and a flag with no usable integer at all.
        long long n_mw_admit_legacy = 0, n_mw_evidence_admitted = 0,
                  n_mw_evidence_would_admit = 0, n_mw_drop_loose = 0,
                  n_mw_drop_unknown = 0;
        /// Detector rows the network did not form under `primary`, summed over stations
        /// (card L5P, network.cycle_slip.mw_detect_pairs).  Zero under the default `all` and
        /// on a two-band lineage; a `primary` run reporting zero on a three-band lineage is
        /// dead wiring (standing invariant #4).
        long long n_mw_pair_scope_skipped = 0;
        /// Code rows folded at the scaled sigma, and code rows whose constellation had no
        /// resolvable ionosphere-free pin pair, which keep their unscaled weight (card L5P,
        /// network.extra_band_code_sigma_scale).  Summed from the per-station NetStation
        /// counters at teardown, as the gross-LOS row counter is.
        long long n_eb_rows_deweighted = 0, n_eb_rows_no_pin = 0;
        /// Run-end telemetry of the second code channel, including both edges of standing
        /// invariant #7 (card SCC, docs/refound/SCC_second_code_channel_2026-09-21.md, R7).
        /// Everything except @c rows / @c fallback_rows is reduced in the serial post-PASS-A
        /// sweep; those two are summed from the per-station NetStation counters at
        /// teardown, as the L5P row counter is.
        ///
        /// Standing invariant #4: @c rows == 0 with the knob armed means the run exercised
        /// nothing (no station reported a second allow-listed band-1 code), and the teardown
        /// warns instead of reporting a clean run.
        struct SecondCode {
            long long cells = 0;          ///< Companion (station, satellite, signal) cells open, summed over epochs.
            long long cell_open = 0;      ///< Cell openings (the re-coupling edge): a cell that was not open last epoch.
            long long cell_close = 0;     ///< Cell closings (the decoupling edge): a cell that was open last epoch and is gone.
            long long rows = 0;           ///< Companion code rows folded into the solve, over the run.
            long long fallback_rows = 0;  ///< Rows routed through the inter-signal state that were not companions (R2's fallback case).
            long long isc_states_max = 0; ///< Receiver inter-signal states alive network-wide, maximum over epochs.
            long long isc_reseeds = 0;    ///< States created at a station that had held that state before and was not restored by the receiver code-bias carry.
            /// (satellite, signal) columns that reached the >= 2-station registration bar
            /// only through companions, i.e. that would have had fewer than two supporting
            /// stations without the second code channel.  Per epoch, summed.  It measures
            /// the card's premise directly: zero of these means the second code channel
            /// added rows without ever adding a column the wire did not already carry.
            long long cols_via_companion = 0;
        };
        SecondCode second_code;
        /// Run-end telemetry of the second phase channel, including both edges of standing
        /// invariant #7 (card SID phase 6a, docs/refound/SID_signal_identity_2026-09-22.md
        /// section 4, R7).  The cell classes and the tier census are reduced in serial
        /// sweeps; @c rows is summed from the per-station NetStation counters at teardown,
        /// as for the second code.
        ///
        /// Standing invariant #4: @c rows == 0 with the knob armed means the run exercised
        /// nothing (no station reported a second allow-listed band-1 phase), and the
        /// teardown warns instead of reporting a clean run.
        struct SecondPhase {
            long long cells = 0;          ///< Second-phase (station, satellite, signal) cells open, summed over epochs.
            long long cell_open = 0;      ///< Cell openings (the re-coupling edge): a cell that was not open last epoch.
            long long cell_close = 0;     ///< Cell closings (the decoupling edge): a cell that was open last epoch and is gone.
            long long rows = 0;           ///< Second-phase rows folded into the solve, over the run.
            /// The fallback case: an allow-listed band-1 phase this station reports for a
            /// satellite with no latched phase of that band beside it.  Those rows are not
            /// added (the latch decides what "second" means); the count is a finding about
            /// the canonical latch, not about the second phase channel.
            long long fallback_rows = 0;
            long long arcs_born = 0;      ///< Distinct (station, satellite, signal) cells the run ever opened.
            long long arcs_live_max = 0;  ///< Cells open network-wide, maximum over epochs.
            long long stations_max = 0;   ///< Stations contributing at least one second phase, maximum over epochs.
            /// Phase-datum anchor incumbencies held on a second-phase arc, summed over
            /// epochs.  R3's question as a number: a second-phase arc is a candidate like any
            /// other, and this says whether the election ever seats one.
            long long anchors = 0;
            /// (satellite, signal) columns that reached the >= 2-station registration bar
            /// only through second-phase arcs.  Per epoch, summed.  It measures the card's
            /// premise directly: zero of these means the channel added rows without ever
            /// widening a family's roster.
            long long cols_via_second = 0;
            // --- Slip census on second-phase arcs, by tier (R4) ---
            // Keyed on RelevelEpochState::kCause*, the producer's own tier vocabulary, so
            // these cannot drift from what the tiers did.  A tier that reads zero here
            // while its arcs exist does not screen the second-phase rows, which is what
            // standing invariant #1 asks about.
            long long slip_arcs = 0;      ///< Second-phase arcs freed by any tier, summed over epochs.
            long long slip_mw = 0;        ///< ... of those, with the MW wide-lane bit.
            long long slip_lli = 0;       ///< ... with the receiver LLI bit.
            long long slip_iftd = 0;      ///< ... with the IF-phase time-difference bit.
            long long slip_t4 = 0;        ///< ... with the RAIM prominence-attribution (T4) bit.
            long long slip_t3 = 0;        ///< ... with the RAIM per-satellite consensus (T3) bit.
            long long slip_windup = 0;    ///< ... with the wind-up restart bit.
            long long slip_gap = 0;       ///< ... with the gap re-entry bit (either gap tier).
            // --- The between-channel witness (R8; observatory: counts, no action) ---
            long long witness_pairs = 0;  ///< (station, satellite) pair-epochs with both band-1 phases and a previous epoch.
            long long witness_jumps = 0;  ///< ... of those, whose epoch-to-epoch change exceeded 0.5 cycles.
            double    witness_max = 0.0;  ///< The largest absolute change seen, in cycles.
            std::string witness_max_id;   ///< "<station>|<satellite>|<signal>@<tow>" of that change.
        };
        SecondPhase second_phase;
        /// The family census and cross-channel exposure of the all-channel widening (card
        /// SID phase 6b, docs/refound/SID_signal_identity_2026-09-22.md).  Folded once per
        /// epoch by @c networkChannelAllEpoch and read at teardown; every class is
        /// measure-only, and the struct stays empty unless `processing.channel_mode: all` is
        /// active on the network path.
        NetworkChannelAllCounters channel_all;
        /// The second-constellation telemetry (card GAL,
        /// docs/refound/GAL_gps_galileo_dual_2026-09-22.md): inter-system bias, clock
        /// S-basis per constellation, the station-epochs that are not GPS-only, and the
        /// anchor per constellation.  Measure-only; stays empty unless a constellation
        /// other than GPS is active on the network path.
        NetworkMultiGnssCounters mgnss;
        /// The RF-0c / RF-0d geometry-free datum counters split per constellation letter,
        /// accumulated from @c NetworkSbasis::CodeBiasDatumRows::sat_gf_by_const beside the
        /// global counters (card GAL, R3 iii).  Read only by the card-GAL run-end line.
        std::map<std::string, NetworkSbasis::SatGfConstCounts> sat_gf_by_const;
        /// Common-band scope census (network.cycle_slip.common_band_scope,
        /// SPEC_event_harness §14).  Reduced per epoch from CommonBandEpoch and, like the MW
        /// admission census above, collected independently of the knob, on purpose.
        /// verdicts: (station, satellite) pairs that had at least one MW pair fire, i.e. the
        /// population a band scope could act on.  narrowed: of those, the ones positive
        /// exoneration restricted.  bands_kept: bands a narrowed verdict spared
        /// (Σ|exonerated|), the headline number, because under the all-band union every live
        /// band would otherwise have been freed.  all_no_exoneration / all_inconsistent: the
        /// two fallbacks that return the unscoped response (nothing exonerated; every band
        /// exonerated although a pair fired).  iftd_veto: silent pairs refused as
        /// exonerators because the IF-TD detector flagged them (the equal-count trap).
        /// immature_pairs: silent pairs refused for arc age.  would_narrow: the knob-off count
        /// of verdicts that would have narrowed, i.e. exactly what arming changes; an off run
        /// reporting 0 here says the card is inert on that lineage, with no armed run needed.
        /// weak_silence: exonerating silences whose detection threshold stood above 6
        /// wide-lane cycles, a gate too loose to have excluded the slip sizes the scope
        /// responds to.  Reported, never acted on; it measures how much narrowing rests on a
        /// weak silence.
        long long n_cb_verdicts = 0, n_cb_narrowed = 0, n_cb_all_no_exoneration = 0,
                  n_cb_all_inconsistent = 0, n_cb_bands_kept = 0,
                  n_cb_iftd_veto = 0, n_cb_immature_pairs = 0,
                  n_cb_weak_silence = 0, n_cb_anchor_colocate = 0,
                  n_cb_would_narrow = 0;
        /// Silent pairs refused as exonerators because one of their bands carried a receiver
        /// LLI at the previous epoch, so the detector withheld that pair's sample and kept
        /// the arc (the count runs straight through the flagged epoch while the lane saw
        /// nothing); §14.11 rule F2.  Its own counter, not folded into n_cb_immature_pairs:
        /// the two refusals have different causes, and this one sizes the population the
        /// spurious `keep` verdicts measured in §14.11 came from.
        long long n_cb_lli_shadow = 0;
        /// Verdicts formed from the receiver's LLI bits alone, at an epoch where every MW
        /// sample of the satellite was withheld so the normal rule could not run, and how
        /// many of those restrict a drain (§14.12 rule F3).  Independent of the knob
        /// (invariant #4): an off run reports what arming would scope.  A run whose LLI drain
        /// fires with lli_verdicts 0 is dead wiring.
        long long n_cb_lli_verdicts = 0, n_cb_lli_narrowed = 0;
        /// Of those, the ones the §14.5 anchor clause refused (also counted in
        /// @ref n_cb_anchor_colocate); §14.12.5.  Its own counter because the co-locates of
        /// the two paths must be distinguishable: attributing the split-reference
        /// regression to the F3 path relies on this split.
        long long n_cb_lli_anchor_colocate = 0;
        /// Split-referenced satellite census (§14.5), always on and independent of the
        /// knob: per processed epoch, how many satellites have >= 2 anchored phase-bias
        /// families whose anchors sit at different stations.  Those satellites'
        /// per-frequency biases are realized against arc levels that walk independently,
        /// and this is the instrument that shows a split-reference regression.  It must read
        /// the same with the scope off, which is why it is not knob-gated.  Sum and epochs
        /// give the mean; the max is the worst epoch; the mean of multi_sum is the
        /// population that can split.
        long long n_cb_split_sats_sum = 0, n_cb_split_sats_max = 0,
                  n_cb_split_multi_sum = 0, n_cb_split_epochs = 0;
        /// IF-TD pair width (§14.10, network.cycle_slip.if_phase_td_pairs): the on-path
        /// telemetry of the added pairs, per pair kind: how many samples the pooled test
        /// thresholded, how many it flagged, and how many of those flags freed an
        /// ambiguity.  Collected independently of the knob: a `first` run reports the one
        /// watched pair, so the clause sizes the card before it is armed, and an `all` run
        /// whose extra pair kinds show evaluated ≫ 0 with flagged 0 over a slipped bed is
        /// dead wiring, not a quiet lane (invariant #4).
        IfTdPairTallies n_iftd_pair;
        /// The veto's per-pair-kind breakdown (§14.10): silent MW pairs the common-band
        /// scope refused as exonerators because an IF-TD pair flagged, attributed to the
        /// IF-TD pair kind that vetoed.  The card's premise is that the vetoes come from
        /// pairs the single-pair detector never watched, so the total
        /// (@ref n_cb_iftd_veto) alone cannot show it.
        std::map<std::string, long long> n_cb_iftd_veto_by_pair;
        /// The reference-arc clause (§22, network.cycle_slip.common_band_ref_arc): silent MW
        /// pairs refused as exonerators because their IF-TD sample sits on the chain's
        /// reference arc, whose double difference is identically 0, so the pooled test could
        /// not have flagged it and the silence is the absence of a test, not evidence of
        /// continuity.  Counted apart from @ref n_cb_iftd_veto on purpose: a veto is
        /// positive evidence the pair moved, this is a structural dead zone, and only the
        /// second carries the clause's named cost (a real single-band slip on a reference
        /// arc also stops narrowing).  Nonzero only with the clause armed, so an armed leg
        /// whose event sits on a known reference arc and reports 0 here is dead wiring
        /// (invariant #4); the by-pair map attributes the refusals to the chains whose
        /// reference the arc is.
        long long n_cb_iftd_ref_arc = 0;
        std::map<std::string, long long> n_cb_iftd_ref_arc_by_pair;
        int n_cpl_row_dump = 12;   // debug budget of card 1d: one-shot assembled coupling-row dump
        int n_cpl_solve_dump = 8;  // debug budget of card 1d: one-shot full-precision solve-checksum dump
        // Coverage-threshold union gate (network.phase_datum_min_stations): epochs on
        // which >=1 thin column was excluded, and the cumulative column-epoch count.
        size_t n_gated_epochs = 0, n_gated_key_epochs = 0;
        // The coverage gate's drop edge (card EV7 §6, network.phase_datum_gate_drop_erase;
        // the rule is filter/NetworkPhaseGateDrop.hpp).  Counted in both knob states, so an
        // off run reports the size of the class and an armed run reports how much of it the
        // response reached (standing invariant #4: a counter that exists only on the armed
        // path cannot say whether arming would matter).
        /// (sat,signal) families the gate excluded while they still held an anchor
        /// incumbent from the previous epoch.
        size_t n_pd_gate_drop_edges = 0;
        /// …of which the knob added to the re-reference set (= edges when armed with
        /// phase_anchor_carry_reref, 0 when off or inert).
        size_t n_pd_gate_drop_handled = 0;
        /// Per-station carried ambiguities erased because of a drop edge: the subset of the
        /// re-reference erase that the drop-edge response owns, separated from the
        /// handover-driven `n_cr_phase_datum_reref` population it shares the path with (and
        /// tagged `erase:gate_drop` in network_amb_state.csv).
        size_t n_pd_gate_drop_arcs = 0;
        // The marker → antenna-reference-point roster census (card UPO), copied out of
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
        size_t n_perconst_partial_epochs = 0;     // epochs published partially: the σ̂0 carve-out pruned >=1 constellation (RTA-10)
        size_t n_perconst_diffuse_published = 0;  // epochs published in full although the global σ̂0 tripped, every constellation being individually clean: the diffuse cross-constellation edge of the per-const approximation (RTA-10)
        FlagRate fr_if_slip;    // tier T1: per (station,sat,pair) IF-phase sample, k=if_phase_td_k_mad
        // Tier T1r: per (station, epoch, constellation) family of n>=2 non-ref DDs, the IF-TD
        // reference arm.  Its action drains through sta_if_resets/refsat_reset and
        // increments nothing in T1, so without its own tier the table reads T1 flags=0 while
        // the arm drives acting refloats (§6.14.5 defect 1).  A separate tier on purpose:
        // the opportunity denominator differs (station-epoch-constellations, not per-sat
        // samples) and so does the threshold (hardened: max(k*s_eff, floor_frac*lambda_IF);
        // legacy: a bare physical floor), and merging it into T1 would hide that asymmetry
        // again.  Aggregated at teardown from each station detector's RefGateCounters.
        FlagRate fr_if_refgate;
        FlagRate fr_we_trigger; // tier T2: per warm within-epoch epoch, σ̂0_first > trig, k=sigma0_trigger
        FlagRate fr_raim_cell;  // tier T3: per (sat,station) standardized residual, k=raim_k
        FlagRate fr_raim_sat;   // tier T3 consensus: per spiked sat (>=min_sta), k=raim_k
        FlagRate fr_attrib;     // tier T4: per median-polish sat column, eff/σ̂0 > k and σ̂0 > trigger (B8/P1-4)
        FlagRate fr_attrib_arc; // tier T4: per median-polish arc interaction, inter/σ̂0 > k (the acting sub-path's criterion, before the persistence/maturity gates; without this line the tier reads 0 flags while it refloats arcs, §9.7.10)
        FlagRate fr_clk_model;  // tier T5: per sat clock-model NIS (own, !ext), k=clock_model_k
        FlagRate fr_clk_mon;    // tier T6: per sat clock-monitor abs/jump (own, !ext), absolute bounds
        double max_dur_anchor_s = 0.0;
        bool   max_dur_anchored = false;
        int    spectrum_diag_budget = 6;    // estimability spectrum dump: the first N gate evaluations (a budget rather than one shot, because a rows epoch needs a later firing than epoch 1).  Seeded from network.trace_estimability_budget by the NetworkRunContext ctor (default 6 = this constant; RF-NS); under network.trace_estimability_deficit_only a clean epoch does not spend it.

        // Timing: the processed-epoch count and the T2e decimation counter (relocated from
        // the loop-timing cluster; t_loop_begin stays a run-TU local).
        std::size_t n_loop_epochs = 0;
        std::size_t decim_counter = 0;   // counts post-offset raw epochs for epoch_decimation_n (T2e)

        // Tallies of the W6 code-bias admission census (CB-ADM; run-end observability).
        // Standing invariant #4: inert wiring is visible only through them, so a BDS-armed
        // on run with n_cb_quarantined_census == 0 is a miswire.  n_cb_late_quarantined
        // counts distinct (sat,signal) keys first seen at the registration site after the
        // census window (implicitly quarantined).
        std::size_t n_cb_admitted = 0, n_cb_quarantined_census = 0, n_cb_late_quarantined = 0;

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

        // Rank-gate calibration telemetry.  The gate is an exact-equality test that throws
        // in both directions, so "withheld" alone hides which failure happened.
        // Under-specified (observed > declared: a missing datum constraint, as in the
        // BeiDou storm) and over-specified (observed < declared: the tolerance pushed below
        // this config's own gauge depth, a mis-calibration that withholds a sound config)
        // need opposite fixes and must be separate numbers.  min/max are over the first gate
        // evaluation of each epoch (one sample per epoch, showing what the datum looked
        // like); a post-heal re-gate is a different question and is counted in
        // n_healed_epochs.
        std::size_t n_gate_evaluated = 0;        ///< epochs on which the gate ran (denominator for the rest)
        std::size_t n_gate_withheld_under = 0;   ///< withheld with observed > declared (missing datum constraint)
        std::size_t n_gate_withheld_over  = 0;   ///< withheld with observed < declared (tolerance below the gauge depth)
        int nullspace_obs_min = -1;              ///< min observed near-null count over the run (-1 = never evaluated)
        int nullspace_obs_max = -1;              ///< max observed near-null count over the run (-1 = never evaluated)

        // Datum-direction information ratio R = (vᵀNv/vᵀv)/cutoff, for standing invariant
        // #7: one sample per (epoch × declared datum direction), on the same reduced normals
        // and the same cutoff the rank gate judges by.  The gate collapses that spectrum to
        // an integer, so a declared gauge that acquires information moves the observed count
        // by one and reads like a mis-calibrated tolerance.  A card-1 coupling row with a
        // truncated family sum can raise one clock gauge decades above the cutoff in a
        // single epoch, with over-specified withholds as the only symptom.  R ≪ 1 = still a
        // gauge; R ≥ 1 = something folded before the gate is pinning a direction the datum
        // owns.  Computed unconditionally (one k×k quadratic form per constellation, against
        // the gate's own O(n³) eigensolve), because a knob-gated version would be dark on
        // the runs that need it.
        std::size_t ddi_epochs        = 0;   ///< epochs the ratio was evaluated (denominator)
        std::size_t ddi_dirs          = 0;   ///< (epoch × direction) samples
        std::size_t ddi_breach_epochs = 0;   ///< epochs with >= 1 direction at R >= 1
        std::size_t ddi_breach_edges  = 0;   ///< invariant #7 edges emitted (breach + clear)
        std::size_t ddi_over_breach   = 0;   ///< over-specified withholds coincident with a breach: the discriminator the run-end advice branches on
        double      ddi_ratio_max     = -1.0;///< largest R over the run (-1 = never evaluated)
        double      ddi_ratio_max_tow = 0.0; ///< ...its epoch
        std::string ddi_ratio_max_dir;       ///< ...its identity ("CLK|G")
        double      ddi_ratio_min     = -1.0;///< smallest R: the floor at which a free datum sits

        // Run-end telemetry of the S-basis completion (SBC, network.sbasis_complete).
        // Standing invariant #4: an armed completion whose counters never move on a GPS+Gal
        // run means the block never saw the deficiency it was built for (a miswire), not a
        // clean run.  Every refusal path is counted separately because each needs a
        // different fix (cap: a pathological epoch or tolerance; degenerate: an orphaned
        // exact gauge, the territory of card PA; weight: re-place rank_gate_tol).
        std::size_t n_sbc_epochs = 0;        ///< epochs on which ≥1 completion row was folded
        std::size_t n_sbc_dirs = 0;          ///< completion rows folded, cumulative
        int         sbc_max_dirs = 0;        ///< max rows folded on one epoch
        std::size_t n_sbc_release = 0;       ///< sbc_release events (satellite re-observed: the re-coupling edge of invariant #7)
        std::size_t n_sbc_capped = 0;        ///< epochs refused: extras beyond sbasis_complete_max
        std::size_t n_sbc_degenerate = 0;    ///< directions skipped as unseparated from the declared-gauge cluster (machine-zero orphans, left to the drop-and-heal; not epoch-fatal)
        std::size_t n_sbc_weight_marginal = 0; ///< epochs refused: completion weight < 2 decades above the cutoff (K2)

        // Run-end telemetry of WE-CI (network.raim_we_code_iono).  Standing invariant #4: an
        // armed screening whose counters never move on an eventful window is a miswire, not
        // a quiet run.
        std::size_t we_ci_code_trips = 0;   ///< epochs whose first-assess code-pool σ̂0 exceeded the code trigger
        std::size_t we_ci_iono_trips = 0;   ///< epochs whose first-assess iono-pool σ̂0 exceeded the iono trigger
        std::size_t we_ci_sta_excl   = 0;   ///< epoch-scoped station exclusions chosen from the code/iono pools
        std::size_t we_ci_sat_dw     = 0;   ///< network-wide satellite code-row de-weights chosen from the code pool
        double we_ci_s0_code_max = 0.0;     ///< max code-pool σ̂0 observed (trigger-calibration telemetry)
        double we_ci_s0_iono_max = 0.0;     ///< max iono-pool σ̂0 observed (trigger-calibration telemetry)

        // Run-end counters for the three response classes of the within-epoch tier (R0).
        // The withhold has its own run counter (n_sigma0_withheld); without these, the
        // refloats and exclusions would exist only as per-epoch T2 event rows, and a run
        // summary could not say how often the tier acted.  The clock move is the
        // impulse-channel meter of §5.07.2: how far the within-epoch re-solves moved a
        // satellite clock from the primary solve's value, with counters and identity per
        // invariant #7.
        long long we_refloat_sat_total = 0; ///< whole-sat within-epoch refloats settled over the run
        long long we_refloat_arc_total = 0; ///< isolated-arc within-epoch refloats settled over the run
        long long we_exclude_sta_total = 0; ///< epoch-scoped station exclusions settled over the run
        double      we_clk_move_max_m   = 0.0; ///< largest |final − primary| clock move (m)
        double      we_clk_move_max_tow = 0.0; ///< TOW of that move
        std::string we_clk_move_max_sat;       ///< satellite of that move
        // Failed within-epoch re-solves (card RPD section 7).  n_adapt counts landed
        // re-solves only, so a failed one appeared in no counter before the card.  A
        // failure is undone to the last landed state; the undo drops, for the epoch, a
        // station it cannot re-add, and an undo that still cannot complete skips the
        // epoch.  The first failure's identity is kept (invariant #7).
        long long   we_resolve_failed   = 0;   ///< re-solves that failed and were undone to the last landed state
        long long   we_undo_sta_dropped = 0;   ///< station-epochs the undo could not re-add and left out of the final solve
        long long   we_restore_failed   = 0;   ///< undos that could not complete (epoch skipped)
        double      we_resolve_fail_tow = 0.0; ///< TOW of the first failed re-solve
        std::string we_resolve_fail_at;        ///< where it failed: a station name, or "shared system"

        // Split-cadence fast-clock telemetry (FC-1, network.fast_clock).  Standing
        // invariant #4 matters most here: the fast tier runs on epochs the full pipeline
        // skips, so a miswired hook produces a clean log and no evidence of itself.
        // "0 ticks_driven" on an armed run means the T2e hook never fired; "0 ticks_emitted"
        // with emit on means the product wire never saw the tier.  Both are miswires, not
        // quiet runs.
        std::size_t fc_ticks_driven   = 0;  ///< fast ticks the decimation hook ran (including shadow ticks)
        std::size_t fc_ticks_estimated = 0; ///< ticks that produced at least one satellite clock delta
        std::size_t fc_ticks_emitted  = 0;  ///< ticks whose deltas reached the product wire (INC-3)
        std::size_t fc_los_used       = 0;  ///< LOS admitted into a tick's fit, summed over ticks
        std::size_t fc_los_screened   = 0;  ///< LOS excluded by the k·σ post-fit screen
        std::size_t fc_screen_slips   = 0;  ///< screened LOS whose residual is slip-scale (|res| >= half a wide-lane)
        std::size_t fc_sat_below_floor = 0; ///< (tick, satellite) pairs that missed min_stations_per_sat
        std::size_t fc_datum_withheld_ticks = 0; ///< ticks withheld entirely because the datum station was absent/screened
        std::size_t fc_gap_withheld_ticks   = 0; ///< ticks whose emission was suppressed by the R11 emit_max_gap_s bound
        std::size_t fc_anchor_los  = 0;     ///< LOS cached by the most recent anchor harvest (a level, not a run total)
        std::size_t fc_anchor_sats = 0;     ///< satellites in the most recent anchor (likewise a level)
        std::size_t fc_anchor_rho_fail = 0; ///< run-total LOS dropped at harvest for want of a usable range / orbit sample
        double fc_wall_ms = 0.0;            ///< run-total wall time inside the tick body (perf budget: §7.3 ≤ 50 ms/tick @305)
        double fc_wall_ms_max = 0.0;        ///< slowest single tick (the budget is a max bound, not a mean)

        // --- redundancy / testability census ---
        // Standing invariant #4 in its statistical form.  The failure mode this census
        // must be able to disprove is "the sweep ran and found nothing", which cannot be
        // told apart from "the sweep never ran" without denominators, so the passes, the
        // sweeps and the rows are reported before any finding.  `redun_max_cold_*` exists
        // for the opposite reason: the prefer-fallback directive says uncheckable but quiet
        // rows (for example single-frequency rising satellites) must be counted and never
        // evented, and keeping the worst one makes that a checkable claim rather than an
        // assertion.
        long long redun_passes = 0;             ///< seam invocations (the denominator for everything else).
        long long redun_sweeps = 0;             ///< epochs on which at least one station was swept.
        long long redun_sweeps_full = 0;        ///< of those, stride/trip full sweeps.
        long long redun_sweeps_targeted = 0;    ///< of those, escalation-driven single-station sweeps.
        long long redun_sweeps_trip = 0;        ///< of the full sweeps, those forced by a flight-recorder trip epoch.
        long long redun_station_sweeps = 0;     ///< (station, epoch) pairs swept: the real cost unit.
        long long redun_escalate_flagged = 0;   ///< (station, epoch) latches raised by the free tier: the escalation's opportunity count.
        long long redun_escalations = 0;        ///< targeted sweeps taken from those latches.  Necessarily 0 under stride 1 (the scheduled sweep already covered every station), so it must never be read as "the free tier found nothing"; redun_escalate_flagged answers that.
        long long redun_rows = 0;               ///< data rows examined in sweeps.
        long long redun_rows_all = 0;           ///< all rows examined (the closure domain: pins and coupling rows carry leverage too).
        long long redun_uncheckable = 0;        ///< data rows with r_i < eps (uncheckable; counted, not acted on).
        long long redun_uncheckable_hot = 0;    ///< of those, rows that also exceeded the magnitude threshold: the cross-flag.
        long long redun_events = 0;             ///< events emitted (after the rate limit): hot cells and unobservable locals.
        long long redun_pool_skips = 0;         ///< (station, epoch, pool) centring pools skipped for population < 4.
        long long redun_r_clamped = 0;          ///< rows whose r_i hit the kernel's [0,1] clamp at 0 (leverage >= 1/w).
        double    redun_mass = 0.0;             ///< sum of r_i over data rows: the redundancy mass of what was swept.
        long long redun_pivot_cols = 0;         ///< local columns examined by the INC-2 pivot census.
        long long redun_pivot_flagged = 0;      ///< of those, columns below the relative pivot floor (column-epochs).
        long long redun_unobs_entered = 0;      ///< decoupling edges: (station, key) first appearances, each evented.
        long long redun_unobs_cleared = 0;      ///< re-coupling edges: (station, key) that regained information.
        bool      redun_closure_any = false;    ///< at least one closure sample exists.
        double    redun_closure_min_slack = 0.0;///< worst (smallest) n_free − trace(H); must stay >= 0.
        long long redun_closure_neg = 0;        ///< sweeps whose slack went negative (a covariance/row-stack defect, not a data finding).
        double    redun_max_hot_s = 0.0;        ///< largest cross-flagged centred deviation over the run.
        std::string redun_max_hot_id;           ///< its (station/sat/class @tow) identity.
        double    redun_max_cold_s = 0.0;       ///< largest centred deviation on an uncheckable row that stayed below the cross-flag.
        std::string redun_max_cold_id;          ///< its (station/sat/class @tow) identity.
        double    redun_wall_ms = 0.0;          ///< run-total wall time inside the census seam (the cost claim).

        /// Baarda reliability / MDB tallies.  Every field is documented on the struct;
        /// reported by @c networkMdbReport, which lives next to the aggregation that
        /// produced the numbers so the counters and the sentences interpreting them cannot
        /// drift apart.
        NetworkMdbTallies mdb;

        /// BlockFormal LOS block census tallies.  Named `bb`, not `block`:
        /// `ctx.counters.bb` is the run-total POD and `ctx.carry.bb` is the accumulator that
        /// produced it, two different objects spelled the same on purpose so a reader does
        /// not have to remember which mechanism owns which prefix.  It is not the redundancy
        /// census's `redun_*` block: that one counts rows, this one counts blocks, and their
        /// denominators are not interchangeable.
        NetworkBlockBaardaTallies bb;

        /// Station congruence screen tallies.  Named `cg` on the same convention as `bb`:
        /// `ctx.counters.cg` is the run-total POD and `ctx.carry.cg` is the accumulator that
        /// produced it.  It counts station-epochs, where `bb` counts blocks and the
        /// redundancy census's `redun_*` block counts rows: three denominators that are not
        /// interchangeable and must never be divided into one another.
        NetworkCongruenceTallies cg;

        // --- prior-residual census ---
        // Standing invariant #4: the ladder is the telemetry.  The per-family ladders live
        // in the accumulator (NetworkPriorResidual) and are read from it at run end; these
        // are the seam-level counters that make the ladders interpretable: how many epochs
        // the seam ran, how many rows reached it, how many identities were emitted and how
        // many were dropped to the per-epoch buffer cap.  A family with n_rows = 0 must read
        // as "ran, saw nothing", distinct from "never ran", and that distinction is
        // prires_epochs > 0 with prires_rows = 0.
        long long prires_epochs = 0;            ///< census epochs advanced (beginEpoch): the "did the wiring execute at all" denominator.
        long long prires_emit_passes = 0;       ///< emission-seam invocations (should track prires_epochs; a gap means an epoch fed rows nobody drained).
        long long prires_rows = 0;              ///< carried priors censused over the run, all families and labels.
        long long prires_reported = 0;          ///< of those, rows above the trace threshold (nothing acted on them).
        long long prires_events = 0;            ///< PRIORRES events emitted (after the per-cell holdoff).
        long long prires_hits_dropped = 0;      ///< above-threshold rows whose individual identity was dropped to the per-epoch buffer cap.  Never affects a ladder, but a nonzero value means the event stream is a sample, and a truncated stream must not read as a quiet run.
        double    prires_max_t = 0.0;           ///< the worst |t| over the whole run, any family, any label, excluding the RNEG class (see below).
        std::string prires_max_id;              ///< its (family/label/station/sat/signal @tow) identity.
        // --- The RNEG class (post_var > fold_var), partitioned out of the ladders.  Its |t|
        // is not a standardized residual: the dispersion is formed from a (prior,
        // posterior) pair that does not describe one fold, so r hits the redundancy floor
        // and |t| inflates by √((p+v)/(εp)).  Left in, this class would dominate the
        // run-end |t| extremum; the readable extremum for it is |ê| in metres.
        long long prires_rneg_rows = 0;         ///< RNEG rows over the run (a subset of prires_rows).
        double    prires_rneg_max_e = 0.0;      ///< the worst |ê| (m) over the whole run's RNEG class.
        std::string prires_rneg_max_id;         ///< its (family/label/station/sat/signal @tow) identity.
        double    prires_wall_ms = 0.0;         ///< run-total wall time inside the emission seam (the §5 cost claim; the per-row addRow cost rides the carry harvests and cannot be separated).

        // --- constraint-aware global σ̂₀ ---
        // Standing invariant #4 for the rider.  The four sums live in the accumulator (they
        // are the statistic); these are the seam-level counters that say whether the
        // wiring executed and in which degraded mode.  "0 rows on the data side" and "the
        // rider never ran" are different findings, and a run whose every epoch carried the
        // approx flag must not read like one that never needed it.
        long long s0g_epochs = 0;               ///< drained epochs the rider evaluated: the "did it execute at all" denominator.
        long long s0g_formed = 0;               ///< of those, epochs with a defined σ̂₀²_global (Σr > 0).
        long long s0g_data_rows = 0;            ///< data rows folded into Σw·v² over the run (RAIM pool, de-duplicated).
        long long s0g_prior_rows = 0;           ///< folded census rows in Σw·ê² over the run.
        long long s0g_ep_approx = 0;            ///< epochs whose data side used the r ≡ 1 fallback (Σr_data is a row count).
        long long s0g_ep_no_data = 0;           ///< epochs with zero data rows (the number is prior-only).
        long long s0g_ep_no_prior = 0;          ///< epochs with zero folded prior rows (the number is data-only, which is what the card corrects).
        long long s0g_ep_degenerate = 0;        ///< epochs where Σr ≤ 0 and no number was formed.
        double    s0g_max_tension = 0.0;        ///< the worst per-epoch Σw·ê²/Σw·v² of the run.
        double    s0g_max_tension_tow = -1.0;   ///< its GPS TOW (the cliff's timestamp, for §6 gate 3).

        /// Tallies of the prefit census, structural fuse, seed-birth tripwire, topology
        /// ledger and flight recorder.  Every field is documented on the struct; reported by
        /// the run-end block in filter/NetworkTeardown.cpp.  Standing invariant #4 applies
        /// directly: the census is a tripwire, and a tripwire that never fires looks like a
        /// tripwire that was never connected; only `epochs` / `rows` / `rec_consults`
        /// separate them.
        NetworkPrefitCensusTallies pc;

        /// Run tallies of the datum graph census.  Standing invariant #4, with a twist
        /// specific to this census: the expected reading of tier 1 on a sound global roster
        /// is k = 1 everywhere with zero predictions, which is byte for byte what a seam
        /// that never ran produces; only `epochs` against `epochs_quiet` tells the two
        /// apart, and the teardown line prints both.
        NetworkDatumGraphTallies dg;

        // --- Run tallies of the PD-safe invariant-DD fold.
        // Always on (INC-0) and solve-neutral: the counters are pure reads of a
        // factorization the fold already computed.  Without them, the branch they observe
        // (`M := 0` on a non-PD Pᶻ+B) has no counter, no log and no event, so a block that
        // goes indefinite can freeze most of the carried state for the rest of the run
        // while every downstream reading stays blind to the carry.
        //
        // Reported by the run-end block in filter/NetworkTeardown.cpp.  The per-epoch
        // halves are not parked here: both epoch-status writers read them directly from
        // @ref NetworkEstimator::ddFoldStats (see NetworkCarryState).
        long long ddfold_epochs        = 0;  ///< accepted installs whose fold reached a factorization (the denominator).
        long long ddfold_folds         = 0;  ///< Σ blocks factored over those epochs.
        long long ddfold_fold_rows     = 0;  ///< Σ K over those blocks: the carried dimension offered to the data.
        long long ddfold_degen_blocks  = 0;  ///< Σ blocks that folded inertly (the PD screen tripped, so M = 0).
        long long ddfold_degen_rows    = 0;  ///< Σ K over those blocks: the carried dimension that took no data.
        long long ddfold_degen_epochs  = 0;  ///< epochs with at least one degenerate block.
        long long ddfold_repairs       = 0;  ///< eigen-clamp repairs applied, INC-1c (0 unless pd_safe).
        long long ddfold_repair_rows   = 0;  ///< Σ K over repaired blocks.
        long long ddfold_rebirths      = 0;  ///< blocks rebirthed at install, INC-2a (0 unless pd_safe).
        long long ddfold_rebirth_rows  = 0;  ///< Σ carried rows re-seeded by those rebirths.
        long long ddfold_transitions   = 0;  ///< edges into the degenerate state observed (the decoupling edge of invariant #7).
        long long ddfold_recoveries    = 0;  ///< edges out of the degenerate state observed: the block's key left the degenerate set, because the block factors again or its component was re-formed (the key cannot tell which).
        double    ddfold_min_pivot_rel = 1.0;///< worst relative pivot d_min/d_max seen all run (≤ 0 means indefinite).
        double    ddfold_repair_worst  = 0.0;///< worst clamped |λ| / λ_max seen all run.  Values ≤ 1e-12 are numerical dust; > 1e-6 indicates a new bug and is logged as a warning.
        long long ddfold_repair_loud   = 0;  ///< folds whose worst clamp exceeded 1e-6·λ_max: the population that is not dust.  A large clamp indicates a new bug and must be reported, not absorbed.

        // --- Run tallies of producer AR, phase 2 (network.ambiguity_resolution).
        // These give the diagnostic a run-end clause; otherwise its only output would be
        // one log line per epoch.  Standing invariant #4 in its ordinary form:
        // `netar_epochs == 0` on an armed run means the search never reached the ILS
        // (min_epochs never cleared, the covariance request threw, or the basis never had
        // two undifferenced columns), which cannot be told apart from a clean quiet run
        // without a denominator.  The distributions are kept as raw per-epoch samples
        // rather than running means because the reported statistic is a median: ratio and
        // p_succ are both heavy-tailed (a degenerate epoch's ratio is 1e4 by construction),
        // and a mean of either is dominated by the epochs a grader would discard.  One
        // double per epoch per series is about 120 kB at 24 h.
        long long netar_epochs     = 0;   ///< epochs the ILS search ran (denominator)
        long long netar_fix_epochs = 0;   ///< …of those, epochs the acceptance battery passed (full or partial)
        long long netar_dd_fixed   = 0;   ///< Σ fixed DDs over the run (kept.size() summed)
        long long netar_rows       = 0;   ///< trace rows written: P1 for the trace card (0 with the trace armed = dead wiring)
        bool      netar_carry_aware = false; ///< covariance mode the search consumed (docs/SPEC_carry_aware_covariance.md §4); part of the reading, not a footnote
        std::vector<double> netar_n_dd;      ///< per-epoch full DD-basis size
        std::vector<double> netar_n_search;  ///< per-epoch searched subset size (== n_dd when uncapped)
        std::vector<double> netar_ratio;     ///< per-epoch accepted-fix ratio
        std::vector<double> netar_p_succ;    ///< per-epoch accepted-fix bootstrap success rate
        std::vector<double> netar_med_sigma; ///< per-epoch median DD formal σ (cycles): the convergence gauge

        /// --- Run-end telemetry of canonical-pair admission (card CPA).
        /// The on-path readout of the rule (standing invariant 4): a `withheld_total` of
        /// zero on an armed run is dead wiring, and the teardown line says so in those words
        /// rather than reading as a quiet run.  Untouched unless
        /// network.canonical_pair_admission.
        NetworkCanonicalPairCounters cpa;

        /// --- The signal roster / arc ledger observatory (card SID phase 1).
        /// Every disagreement class is a named counter with its own denominator (standing
        /// invariant 4), and the run-end table prints all of them.  Untouched unless
        /// network.signal_roster.
        NetworkSignalRosterCounters sid;

        /// --- The emitter's per-family bookkeeping read against the arc ledger, and the
        /// declared roster's own wire census (card SID phase 5a, network.ledger_emit).
        /// Untouched unless the knob is armed.
        NetworkLedgerEmitCounters le;
    };

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
        /// The parameter registry (card PRM phase 1): one declared property set per
        /// parameter kind, built once from @ref knobs (declared after it, so it is
        /// initialized after it) and read, never written, inside the epoch loop.  The
        /// single process-noise source of the border kinds (@ref NetworkParamRegistry::q).
        const NetworkParamRegistry params;
        /// The parameter inventory (card NOM N1): the run's receiver IFB rule
        /// (@c param_inventory.rx_ifb, built here once and read by the station split), and
        /// every state column of every station as a parameter instance with its disposition,
        /// folded after PASS B.  The tallies are trace class: written by the drain, never
        /// read by the solve.  Tabled after the first emitted epoch and at run end
        /// ("parameters as armed").
        NetworkParamInventory  param_inventory;
        /// The contributions to the normal equations, per declared kind (card NOM N2a,
        /// filter/NetworkContributions.hpp): the station side drained at the end of PASS B,
        /// the shared folds at the epoch tail.  Trace class: never read by the solve.
        /// Tabled after the first emitted epoch and at run end ("contributions as armed").
        NetworkContributionLedger contributions;
        NetworkCarryState      carry;
        NetworkServiceCounters counters;
        NetworkBiasAdmission   admission;   // latched census result of CB-ADM (active=false: the legacy path)
        /// Event-harness scripted-fault schedule and ledger (filter/NetworkInject.hpp).
        /// Kept outside the warm-resume fingerprint and the snapshot on purpose: a leg
        /// resumes a converged state and then applies its own schedule, and two legs off one
        /// snapshot are designed to differ only here.  armed == false (the default,
        /// network.inject absent) makes every consumer of this object short-circuit, and the
        /// run is byte-identical.
        NetworkInjectState     inject;
        /// The per-epoch signal roster and its run-scope caches (card SID phase 1,
        /// docs/refound/SID_signal_identity_2026-09-22.md section 5).  Kept outside the
        /// warm-resume fingerprint and the state snapshot on purpose: nothing here is read by
        /// the solve, so a resumed run re-seats every cell at its first post-resume epoch,
        /// the right posture for a fresh observer.  armed == false (the default,
        /// network.signal_roster off) leaves every member untouched and both seams
        /// unreachable.
        SignalRoster           roster;
        /// The arc lifecycle machine, one record per (station, satellite, signal) (card SID
        /// phase 1).  Same exemption and same reason as @ref roster; advanced by the same
        /// single call.
        ArcLedger              arc_ledger;
        /// The per-epoch reason buffer that every removal screen writes and the roster's
        /// audit pass folds into the ledger (card SID phase 4a, network.ledger_screens).
        /// Same exemption as @ref arc_ledger: nothing in the solve reads it, so it is
        /// neither snapshotted nor fingerprinted.  armed == false (the default) makes every
        /// writer return on its first line.
        ArcReasonEpoch         reasons;
        /// The emit-site capture, the generation memories the fold differences against, and
        /// the declared roster the wire carries (card SID phase 5a, network.ledger_emit).
        /// Same exemption and same reason as @ref arc_ledger: nothing in the solve reads it,
        /// so it is neither snapshotted nor fingerprinted.  armed == false (the default)
        /// leaves every member untouched and both seams unreachable.
        LedgerEmitState        ledger_emit;
    };

    // ------------------------------------------------------------------
    // The relocated RTB-7 snapshot channel and carry lifecycle, as free functions over
    // the context (decomposition INC-4).  The bodies are in NetworkRunContext.cpp, and
    // each opens with an alias block.  They are free functions rather than Application
    // member functions because none needs Application state beyond config_, which is
    // passed explicitly (parameter names match the former use sites).
    // The three-place rule: fingerprint terms live in networkSnapshotFingerprint, the
    // restore in networkTryWarmResume, and the write in networkWriteStateSnapshot; a new
    // snapshot field touches all three or documents its exemption at its
    // NetworkCarryState declaration.
    // ------------------------------------------------------------------

    void networkSnapshotFingerprint(NetworkRunContext& ctx, const GnssConfig& config_,
                                    const std::vector<NetStation>& stations);

    class NetworkClockEmitRebase;   // fwd: include/products/NetworkClockEmitRebase.hpp
    class NetworkEmitRebase;        // fwd: include/products/NetworkEmitRebase.hpp
    class NetworkPbEmitFilter;      // fwd: include/products/NetworkPbEmitFilter.hpp
    // Resume frame restore: both seams take the CLK-R held-frame engine (a runNetwork
    // local), so its A_c and per-sat baselines travel with the snapshot.  nullptr: the
    // engine's frame is neither captured nor restored.
    //
    // RS-5 (§21.10) adds the two phase-bias emit engines on the same footing (also
    // runNetwork locals, also outputs-only, also neither captured nor restored when
    // nullptr), so all three cross-epoch emit layers travel with the snapshot instead of
    // cold-starting at every seam.
    void networkTryWarmResume(NetworkRunContext& ctx, const GnssConfig& config_,
                              const MultiRinexSync& sync, NetworkProductEmitter& emitter,
                              NetworkInvariantCarryDriver& carry_driver,
                              const std::shared_ptr<spdlog::logger>& log,
                              NetworkClockEmitRebase* clk_rebase = nullptr,
                              NetworkEmitRebase* emit_rebase = nullptr,
                              NetworkPbEmitFilter* pb_filter = nullptr);

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
     * @param ctx      Run context (reads the staged rows, writes the counters).
     * @param config_  Run configuration (the knob and the snapshot path).
     * @param stations The station roster; indices key the staged rows.
     * @param log      Run logger.
     */
    void networkRestoreCanonicalSignals(NetworkRunContext& ctx, const GnssConfig& config_,
                                        std::vector<NetStation>& stations,
                                        const std::shared_ptr<spdlog::logger>& log);

    /// @param stations The station roster, shared by two capture groups that cannot be
    ///        read out of @c ctx.carry.  RS-8 (§21.16) @c CN: the canonical-signal latch
    ///        lives in each station's @c PppModel.  RS-9 (§21.18) @c RL: the code-bias
    ///        linearization point is a property of each station's own state vector
    ///        (@c stations[i].sm->getStateVector()), which no other snapshot record needs;
    ///        the RC/SD/ER/PF rows are all plain copies out of @c ctx.carry.  Read-only in
    ///        both cases, and each group is written only under its own knob
    ///        (@c network.state_snapshot_signal_capture /
    ///        @c network.state_snapshot_rxcb_lin_capture).
    /// @param consumer The closed-loop consumer engine (nullptr when the harness is off;
    ///        card CC, part B).  Under @c network.closed_loop.state_snapshot each network
    ///        history copy (@c state_snapshot_keep_history) is accompanied by the
    ///        consumer's own RtStateSnapshot at
    ///        @c RealTimeEngine::consumerSnapshotHistoryPath (same @c .epNNNN suffix),
    ///        written only when the consumer's last processed epoch is @p t (the
    ///        matching-tow invariant; a miss is counted in @c counters.cl_snap_skipped and
    ///        logged).  Read-only for the producer.
    void networkWriteStateSnapshot(NetworkRunContext& ctx, const GnssConfig& config_,
                                   const GnssTime& t, double pl_now_s,
                                   NetworkProductEmitter& emitter,
                                   const NetworkInvariantCarryDriver& carry_driver,
                                   const std::vector<NetStation>& stations,
                                   const std::shared_ptr<spdlog::logger>& log,
                                   const NetworkClockEmitRebase* clk_rebase = nullptr,
                                   const NetworkEmitRebase* emit_rebase = nullptr,
                                   const NetworkPbEmitFilter* pb_filter = nullptr,
                                   RealTimeEngine* consumer = nullptr);

    /// Install the warm resume's stashed @c RL map on each station's @c StateInitializer,
    /// so the station's one-shot first-epoch code-bias seed takes the writer's
    /// linearization point instead of a median over one epoch of the resume epoch's code
    /// (RS-9, §21.18).  Called from @c runNetwork immediately after
    /// @c networkTryWarmResume, before any station has processed an epoch, which is the
    /// only window in which the one-shot latch has not yet closed.  A no-op (not even a
    /// map allocation) unless @c ctx.carry.rxcb_lin_restore is non-empty, so an unarmed
    /// or cold run reaches no new code at all.
    /// The applied / uncovered halves are read back at the run-end readout directly from
    /// @c ctx.carry.rxcb_lin_sta_counts (@c networkTeardown has no station roster and
    /// needs none: the tallies are indexed, not named).
    void networkApplyRxCbLinRestore(NetworkRunContext& ctx, const GnssConfig& config_,
                                    std::vector<NetStation>& stations,
                                    const std::shared_ptr<spdlog::logger>& log);

    void networkInjectShVtecPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                  const GnssConfig& config_, NetworkEstimator& est,
                                  const SphericalHarmonicIonosphere* sh_model,
                                  const SphericalHarmonicIonosphere* sh_model2,
                                  bool datum_changed, double dt, bool within_epoch_armed);

    void networkInjectPhaseHoldPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                     NetworkEstimator& est, bool within_epoch_armed);

    /**
     * @brief Fold the open pin-bridge rows (eq34 reattach hygiene, INC-2).
     *
     * One soft shared-border prior per open bridge (@c ctx.carry.pin_bridge), at the
     * key's held level with information 1/σ².  Folded before the null-space rank gate,
     * like the broadcast-anchored clock anchors and the Fix-A phase hold, because for a
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
     * @param within_epoch_armed  append to @c ws.cap_priors so the within-epoch RAIM
     *                            re-solve replays the identical shared system.
     */
    void networkInjectPinBridgePrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                     NetworkEstimator& est, bool within_epoch_armed,
                                     const std::shared_ptr<spdlog::logger>& log);

    void networkInjectD2CarryPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                   NetworkRunTraces& traces, const GnssTime& t,
                                   NetworkEstimator& est, bool datum_changed,
                                   const std::set<char>& changed_consts, double dt,
                                   bool within_epoch_armed,
                                   const std::shared_ptr<spdlog::logger>& log);

    /**
     * @brief Build this epoch's inputs of the exact coupling time update (card CTU,
     *        @c network.carry_coupling_exact_update) into @c ctx.carry.cxu_epoch.
     *
     * Called by the driver after the epoch's shared columns are registered and before
     * PASS B, whose parallel produce reads the result.  The coupling rows are built before
     * @ref networkInjectD2CarryPrior runs, so the inflation each stashed key will receive
     * is the fold's own decision, made once before PASS B by @ref networkCarryDecide and
     * read here and by the fold (card PRM phase 1b): a key the fold folds is in S at the
     * variance the decision forms on its stashed variance (@ref carryFoldVarEff, the dense
     * fold's chain), and a key the fold will not carry, or whose column left the layout,
     * goes to D with its route.  The one clause no point before PASS B can know is a
     * clock-pivot handover, decided later in the epoch; the fold counts the keys in S that
     * the handover re-expressed or retired after these rows were built.
     * Then @f$P_{SS}+Q_S@f$ is factored.  A no-op (with @c ok false) when the knob is off,
     * when no dense block was stashed, or when the stash stamp is not valid (the epoch
     * after a warm resume).
     * @param dt the driver's inter-epoch step, the same value the fold receives.
     */
    void networkCouplingExactPrepare(NetworkRunContext& ctx, const NetworkEstimator& est,
                                     const GnssTime& t, double dt,
                                     const std::shared_ptr<spdlog::logger>& log);

    void networkInjectClockOscPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                    NetworkRunTraces& traces, const GnssTime& t,
                                    NetworkEstimator& est,
                                    const std::vector<std::string>& registered_clock_sats,
                                    bool clk_model_warm, bool datum_changed,
                                    const std::set<char>& changed_consts,
                                    double pl_now_s, double dt, bool within_epoch_armed);

    /**
     * @brief Reduce the rx-CB de-mean μ within the epoch (card LV-5).
     *
     * Runs serially between PASS A and PASS B.  Both terms of every carry innovation are
     * known at that point (the carried absolute posterior, i.e. last epoch's stash, and
     * @c x_lin(cb), the frozen linearization point), so the LV-2 mean can be formed over
     * the stations present this epoch and applied to this epoch's folds, instead of
     * arriving one epoch late.
     *
     * The lag matters because its loop gain scales with how much carry information folds:
     * while the frame screen withholds most of the priors the lag is harmless, but once
     * card LV-4 admits them all a lagged μ makes the level limit-cycle.
     *
     * The same race contract as the rest of the carry cycle applies: one serial writer,
     * and the parallel produce phase only reads @c ctx.carry.rx_cb_demean_mu.  The
     * candidate set is every present station with a carried entry inside the gap horizon;
     * the folded set can be marginally smaller (a column that drops out of the local
     * block), which the harvest's diagnostic mean measures directly.
     *
     * No-op unless @c network.rx_cb_carry_demean_within.
     *
     * @param present  PASS-A presence flags (a station with no data this epoch must
     *                 not vote on the frame it will not be folded into).
     */
    void networkRxCbDemeanPrepass(NetworkRunContext& ctx,
                                  const std::vector<NetStation>& stations,
                                  const std::vector<bool>& present,
                                  double pl_now_s);

    void networkHarvestLocalCarries(NetworkRunContext& ctx, const GnssConfig& config_,
                                    NetworkEstimator& est, const Eigen::VectorXd& x_s,
                                    std::vector<NetStation>& stations,
                                    const std::vector<int>& est_index_of,
                                    const std::vector<StationAssembly>& asm_res,
                                    const NetworkSbasis::Plan& plan,
                                    const SphericalHarmonicIonosphere* sh_model,
                                    const SphericalHarmonicIonosphere* sh_model2,
                                    NetworkCarryVce& carry_vce_est,
                                    NetworkPriorResidual& prires_est, bool carry_warm,
                                    double pl_now_s,
                                    const std::vector<std::string>& registered_clock_sats,
                                    const std::set<std::string>& shared_phase_keys,
                                    const std::vector<std::pair<std::string, std::string>>& registered_code_keys);

    void networkRetroUraBookkeeping(NetworkRunContext& ctx,
                                    const std::set<std::string>& clock_refloat_sats,
                                    const std::set<std::string>& retro_flag_sats,
                                    bool retro_feed);

    void networkStashCarry(NetworkRunContext& ctx, const GnssConfig& config_,
                           NetworkRunTraces& traces, const GnssTime& t,
                           NetworkEstimator& est, const Eigen::VectorXd& x_s,
                           const NetworkSbasis::Plan& plan,
                           const SphericalHarmonicIonosphere* sh_model,
                           const SphericalHarmonicIonosphere* sh_model2,
                           const std::set<std::string>& clock_refloat_sats,
                           const std::vector<std::string>& registered_clock_sats,
                           const std::set<std::string>& shared_phase_keys,
                           const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
                           const std::shared_ptr<spdlog::logger>& log);

} // namespace GnssProcessor

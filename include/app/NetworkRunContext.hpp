#pragma once
/**
 * @file NetworkRunContext.hpp
 * @brief @internal Tier-1 run state of Application::runNetwork — the const
 *        config-knob snapshot (NetworkKnobs), the cross-epoch carry state
 *        (NetworkCarryState), and the service counters
 *        (NetworkServiceCounters), bundled as NetworkRunContext.
 *
 * Relocated VERBATIM from the ApplicationNetwork.cpp declaration stretches
 * (decomposition INC-4, 2026-07-16; see
 * docs/network_decomposition_plan.md).  The run loop binds
 * `auto& X = ctx.carry.X;` / `const T X = ctx.knobs.X;` aliases under the
 * former local names, so every downstream use-site is textually unchanged.
 *
 * Three-place rule: a NetworkCarryState field marked snapshot-member
 * must be added to the fingerprint + restore + snapshot write — all three
 * live in src/core/app/NetworkRunContext.cpp.  Any NEW cross-epoch field
 * defaults into NetworkCarryState under that rule unless explicitly exempted
 * (the per-field "NOT snapshotted" comments below are the exemption record).
 * Deliberately NOT relocated here (stay run-TU locals): the engine objects
 * (NetworkEstimator, NetworkInvariantCarryDriver, NetworkCarryVce,
 * NetworkIonoDeweightGate, NetworkEmitRebase, NetworkPbEmitFilter, the WL/UWL
 * lane machinery incl. its function-local types), the D0/D0b section-timer
 * instrumentation, and the closed-loop consumer metrics.
 */

#include "filter/NetworkRunTypes.hpp"
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
#include "slip/NetworkRawObservables.hpp" // IonoCsState (CS-GF authority, QUEUE 8b successor)
#include "slip/NetworkSlipOracle.hpp"  // NetworkSlipOracle (sim-only oracle slip response)
#include "filter/NetworkOraclePin.hpp"  // NetworkOraclePin (sim-only oracle ambiguity pin)
#include "filter/NetworkSbasis.hpp"     // NetworkSbasis::Plan (lifecycle-fn params)
#include "filter/NetworkBiasAdmission.hpp" // CB-ADM: latched W6 code-bias admission census (ctx.admission)
#include "filter/NetworkInject.hpp"        // NetworkInjectState — the EVENT HARNESS schedule + ledger
#include "logging/NetworkRunTraces.hpp" // FlagRate (B8 counters)
#include "common/PppUtils.hpp"          // eq34_ppp_utils

#include <Eigen/Dense>

#include <array>
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

    class RealTimeEngine;   // Card CC (B): the closed-loop consumer handed to networkWriteStateSnapshot

    class GnssConfig;
    struct NetworkConfig;
    class MultiRinexSync;
    class NetworkProductEmitter;
    class NetworkInvariantCarryDriver;
    class NetworkCarryVce;
    class NetworkPriorResidual;   ///< filter/NetworkPriorResidual.hpp — a run-TU local, like NetworkCarryVce.
    class SphericalHarmonicIonosphere;
    struct GnssTime;

    /// @internal Const snapshot of every run-loop config knob (decomposition
    /// INC-4).  snapshot() preserves the original getter READ ORDER exactly;
    /// members are value copies with the original local names/types.  The
    /// WL/UWL-zone knob consts stay in the run TU with their machinery.
    struct NetworkKnobs {
        double q_clock{};
        double q_pb{};
        double q_cb{};
        bool pl_on{};
        double pl_q_amb{};
        int pl_warmup{};
        double pl_max_gap_s{};
        double pl_var_floor{};
        /// network.phase_leveling_los_scale — F1: at the
        /// leveling-carry inject, multiply each carried ambiguity row's variance by B_eff =
        /// the number of carried ambiguity columns injected on the same (station, satellite)
        /// LOS this epoch, so the B rows assert ONE unit of iono-direction precision instead
        /// of B (the over-count that froze the level at K_eff = (B+1)/(B−1) epochs).  Applied
        /// after q·gap, before the var floor and the VCE factor.  Default OFF = byte-identical.
        bool pl_los_scale{};
        /// network.phase_leveling_local_block — F3: carry
        /// each station's injectable carried ambiguity set as ONE jointly-whitened local
        /// block (the harvested conditional cross covariances (N_rr⁻¹)_{c_i c_j} on Σ's
        /// off-diagonals; non-admitted columns MARGINALIZED) instead of independent
        /// per-column rows — exact for EVERY local-local direction given the border: the
        /// per-LOS iono direction F1 fixes AND the station-common rx clock / ZWD / rx-DCB
        /// directions it leaves over-counted.  Supersedes pl_los_scale on whitened blocks
        /// (F1 applies only on a factorization fallback).  Live only with carry_cpl_cond
        /// and NOT carry_cpl_joint (loader warns INERT otherwise).  Default OFF =
        /// byte-identical.
        /// ⚠ RF-1 step 3: this is the RESOLVED LIVE PREDICATE, not the knob —
        /// @c network.phase_leveling_local_block OR the @c float_kf vehicle, which makes
        /// the joint block STRUCTURAL (design §3.2: "the carried covariance folded as a
        /// proper KF prior IS F3; what RF-1 changes is that it stops being an optional
        /// knob and becomes the vehicle's definition").  @ref pl_blk_from_vehicle says
        /// which of the two supplied it, so run-end telemetry can name the source.
        bool pl_local_block{};
        /// RF-1 step 3: the joint block is live because the VEHICLE is @c float_kf
        /// (rather than because the F3 knob is armed — which the loader refuses under the
        /// vehicle).  Read only by telemetry and by the PD-fallback failure bar, never by
        /// the fold itself: the fold reads @ref pl_local_block and cannot tell the two
        /// sources apart, which is the point.
        bool pl_blk_from_vehicle{};
        /// network.phase_leveling_local_block_min_age_ep — F3 block ADMISSION age gate: a
        /// carried leg younger than this many loop epochs (since its first stash) is kept
        /// OUT of the station block and emitted as a per-column row (F1-scaled if armed).
        /// A leg born at a satellite's re-admission carries its code-level birth error;
        /// the block's joint whitening spreads that into the station's mature columns —
        /// the post-eclipse DD episodes SPEC_amb_iono_split_overcount §6.9 measured (and
        /// §6.11 showed are NOT the shared border).  0 = OFF (byte-identical).
        int  pl_blk_min_age_ep{};
        /// network.phase_leveling_local_block_young_mode == "free": a young leg is NOT injected
        /// (no memory until it matures).  false = "diagonal" (per-column rows — measured
        /// harmful, SPEC_amb_iono §6.11 arm 5).
        bool pl_blk_young_free{};
        /// RF-1 §7.1 (docs/refound/RF-1_float_ambiguities_design.md):
        /// network.ambiguity_vehicle == "float_kf" — the float STATE with a carried joint
        /// covariance, a floored birth prior and a bounded DROP as the slip response.
        /// ⚠ RF-1 STEP 1: the vehicle is REGISTERED and carries NO mechanism, so this
        /// flag is read by the run-end telemetry and the trace's provenance line and by
        /// nothing that touches the solve — both values are byte-identical in this build.
        bool amb_float_kf{};
        /// RF-1 §3.3(b): network.amb_float_birth_var_m2 — the FLOOR (m²) applied to the
        /// arc's first-stash variance when the harvest records @ref LocalCarry::var_birth.
        /// 0.0 (default) = unfloored, so var_birth == var_first to the ULP.
        double amb_birth_var_floor{};
        /// RF-1 §3.5: network.amb_float_q_m2s — process noise on the ambiguity block
        /// under float_kf.  The card's posture is 0.0; carried here so the run manifest
        /// and the run-end telemetry can state it.  ⚠ INERT in step 1.
        double amb_float_q_m2s{};
        /// RF-1 §3.6 (step 5): network.amb_float_declared_handover — the anchor handover
        /// as a declared, exact carry S-transformation instead of a network-wide erase.
        /// ⚠ INERT in step 1.
        bool amb_float_declared_handover{};
        /// RF-1 §7.4: network.trace_amb_state — the per-arc carry-state trace
        /// (@c network_amb_state.csv).  Measure-only; default OFF ⇒ no file, no rows.
        /// RF-2 item 1: network.phase_datum_held_basis, already ANDed with float_kf and
        /// phase_datum_prioritized by the loader's interlocks.
        bool pd_held_basis{};
        /// RF-2d: network.phase_datum_held_cold_release, already ANDed with pd_held_basis.
        bool pd_held_cold_release{};
        /// RF-2d: network.phase_datum_held_mark_cold, already ANDed with pd_held_basis.
        bool pd_held_mark_cold{};
        /// RF-2 item 3: network.phase_anchor_min_age_ep (0 = no age gate).
        int  pa_min_age_ep{};
        /// RF-2 item 3: network.phase_anchor_flag_quiet_ep (0 = no quiet-window gate).
        int  pa_flag_quiet_ep{};
        bool trace_amb_state{};
        /// RF-1 §7.4: network.trace_amb_state_every — epoch stride of the TRAJECTORY rows
        /// (>= 1).  EVENT rows ignore it.
        int  trace_amb_state_every{1};
    /// AGE-AWARE young-arc variance inflation (network.phase_leveling_young_inflate_*,
    /// SPEC_amb_iono_split_overcount §6.17–§6.18): a carried ambiguity column whose LAST
    /// RE-LEVEL (@ref LocalCarry::relevel_s) is recent carries a CODE-quality level at a
    /// PHASE-stiff harvested variance, so the T4 tier erases it and the erase–re-level
    /// treadmill sustains a metre-class ZWD/clock plateau.  V0·exp(−age/τ) is added to
    /// the column's prior variance — as a PD- and correlation-preserving congruence
    /// scaling of its row/column of Σ inside the F3 block, as a plain var_eff += var_add
    /// on the per-column row without it.  @c var_m2 == 0 = OFF = byte-identical; the
    /// loader guarantees phase_leveling and !carry_invariant when it is not.
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
        bool carry_cpl_shmarg{};
        /// network.carry_coupling_gauge_project — Card 1f: hold each (constellation,kind)
        /// coefficient sum of a TRUNCATED coupling row at its harvest value, so a departed
        /// column stops turning the row into a high-weight observation of the current datum
        /// gauge pinned at the old realization.  Measured motive: 7 departures in one epoch
        /// drove the smallest null eigenvalue 1e-11 → 9.43e-3 (five decades over the gate
        /// cutoff) for 4 spurious OVER-specified withholds, self-healing only at
        /// pl_max_gap_s by blanket carry expiry.
        bool carry_cpl_gauge{};
        /// network.carry_coupling_reinit_sources — Card 1g: honour the reinit history with
        /// the κ time update OFF, and feed it from every REALIZATION-STEP source (phase-anchor
        /// re-reference, eq34 chain re-reference, datum-slip PB re-realization) rather than
        /// the clock-QC sources alone.  A re-realized column carries an unknown frame step in
        /// its stashed ŝ_prev; folding it as a full-weight present term injects that step as
        /// row bias at conditional-variance (mm) confidence.
        bool carry_cpl_reinit{};
        bool zwd_on{};
        bool iono_on{};        ///< Slant-iono carry (QUEUE 8b, network.carry_iono); loader guarantees phase_leveling + relevel_free_epoch (scope=relevel) and the SH/mech-B/carry_invariant mutexes.
        bool iono_scope_always{};  ///< iono_carry_scope == "always" (else "relevel").
        bool iono_source_smoothed{};  ///< iono_carry_source == "smoothed_gf" (the CS-GF authority) vs "solve".
        double iono_q{};       ///< Slant-iono carry RW process noise (m²/s).
        double iono_var_floor{};   ///< Inject-side variance floor (m²) — the wrong-carry healing bound.
        double iono_max_gap_s{};   ///< Carry eviction gap (s); deliberately longer than pl_max_gap_s.
        /// network.carry_iono_in_block — I-in-block: the station's present per-LOS iono
        /// columns join its F3 whitened local block (harvest appends them to `cols` between
        /// the ambiguities and the ZWD so `PlHarvestBlock::C` is (n_amb+n_iono+[1])², each
        /// entry stashes with the same `blk`/`blk_idx` as an ambiguity, and PASS B pushes
        /// one more `LocalBlockCol` per LOS) instead of the FALSIFIED diagonal
        /// network.carry_iono row.  A diagonal per-LOS row asserts the iono direction the
        /// station's B carried band priors already assert — the B-fold over-count of
        /// SPEC_amb_iono_split_overcount §7 item 17; inside the block the harvested cross
        /// terms subtract it exactly.  Loader guarantees phase_leveling + pl_local_block +
        /// carry_cpl_cond + !carry_invariant, and HARD-ERRORS on a carry_iono co-arm.
        /// OFF = byte-identical.
        bool iono_blk{};
        double iono_blk_q_m2{};      ///< I-in-block RW process noise, m² of L1 slant delay PER EPOCH (⚠ not per second — see GnssConfig::networkCarryIonoInBlockQM2).
        double iono_blk_max_gap_s{}; ///< I-in-block staleness bound (s); the loader has already resolved the "= phase_leveling_max_gap_s" sentinel.
        /// network.carry_rx_clock_in_block — RX-CLK-in-block: the station's RECEIVER
        /// CLOCK column (and, under @c rxclk_isb, its per-system ISB columns) join its F3
        /// whitened local block.  The clock is otherwise a FREE per-epoch local column
        /// with no temporal prior on this path — the rho-direction sink the ZWD and iono
        /// cards push their residual error into.  ⚠ Carried as a random walk about a
        /// CARRIED DRIFT, because @c x_lin(clock) is a run-constant here (the producer
        /// never writes its solution back), so the whole ~0.3 m/s clock ramp lives in
        /// x_r and a bare RW prior would be ~30 sigma wrong every epoch.  Loader
        /// guarantees phase_leveling + pl_local_block + carry_cpl_cond and
        /// !carry_invariant.  OFF = byte-identical.
        bool rxclk_blk{};
        double rxclk_q_m2{};        ///< Carried clock BIAS random-walk variance, m^2 PER EPOCH (⚠ not per second — see GnssConfig::networkCarryRxClockQM2).
        double rxclk_max_gap_s{};   ///< Staleness bound (s); the loader has already resolved the "= phase_leveling_max_gap_s" sentinel.
        double rxclk_rate_var0{};   ///< Initial variance of the carried DRIFT state, m^2/s^2 (deliberately large: an unknown drift must inject a near-free prior).
        double rxclk_rate_q{};      ///< Carried DRIFT random-walk variance, m^2/s^2 PER EPOCH.
        double rxclk_jump_k{};      ///< Clock-JUMP gate (sigma) on the one-step prediction innovation at harvest; exceeding it erases the carry (invariant #1's wired response).
        bool rxclk_isb{};           ///< Carry the per-system ISB columns too (pure RW, no drift state).
        bool rx_cb_on{};      ///< rx-CB carry (SPEC_rx_cb_carry); loader guarantees code_datum_time_linked.
        bool rx_cb_datum{};   ///< RXCB-DATUM: zero-mean-over-receivers DCB datum (network.rx_code_bias_datum).
        double rx_cb_datum_sigma{};  ///< RXCB-DATUM loose physical-bound sigma in m (network.rx_code_bias_datum_sigma_m).
        bool sta_prior_pre_pd{};  ///< Card RF-PD-fix: fold the station's DATUM rows before the addStation PD test (network.station_prior_before_pd).
        double rx_cb_q{};   ///< rx-CB carry RW process noise (m²/s).
        bool rx_cb_demean{};  ///< Card LV-2 (SPEC_level_information_budget): inject η = λ·(innov − μ_c) and gate on |innov − μ_c|; loader guarantees rx_cb_on.
        int rx_cb_demean_min_sta{};  ///< Card LV-2: contributing-station floor below which μ_c FREEZES (thin-mean guard).
        bool rx_cb_demean_within{};  ///< Card LV-5 (SPEC_level_information_budget §12): reduce μ between PASS A and PASS B and fold against THIS epoch's value (no lag).  Loader guarantees rx_cb_demean.
        bool rx_cb_step_gate{};      ///< Card LV-4 (SPEC_level_information_budget §11): disable the frame-DEPENDENT inject screen; erase on a frame-INVARIANT step at the harvest.  Loader guarantees rx_cb_on.
        double rx_cb_step_k{};       ///< Card LV-4: k·σ of the invariant step screen.
        bool rx_cb_harvest_demean{}; ///< Card S2H (SPEC_level_information_budget §17 item 3): post-solve Λ-weighted de-mean of the HARVESTED absolute rx-CB realization, per family — the "S-consistent harvest".  Loader guarantees rx_cb_on and forces LV-2/LV-5 OFF.
        double rx_cb_harvest_screen_z{};  ///< Card S2H: robust peer-screen threshold for the μ vote (≤ 0 never reaches the run: loader validates > 0).
        bool lv1_seed_frame{};       ///< Card LV-1 (SPEC_level_information_budget): reframe per-LOS iono seeds into the field's frame at allocation; loader guarantees estimate_iono_sh.
        int lv1_min_sats{};          ///< Card LV-1: minimum raw seeds for the per-station median offset.
        double zwd_q{};
        /// network.carry_zwd_in_block — B9-in-block: the carried ZWD column joins its
        /// station's F3 whitened local block (harvest appends it to `cols` so `PlHarvestBlock::C`
        /// is (n+1)², the entry stashes with the same `blk`/`blk_idx`, and PASS B pushes it as
        /// one more `LocalBlockCol`) instead of folding as a diagonal coupling row OUTSIDE the
        /// block.  A diagonal ZWD row asserts a station-COMMON direction that every carried
        /// ambiguity row already asserts — the ~n_LOS over-count SPEC_amb_iono_split_overcount
        /// §7 item 17 names, and the reason B9 was falsified on the quiet bed.  Exempt from the
        /// F3 young-column age gate (a ZWD is not an arc).  Loader guarantees zwd_on AND
        /// pl_local_block (it disables the knob otherwise).  OFF = byte-identical.
        bool zwd_in_block{};
        bool zwd_seam_on{};    ///< network.zwd_seam_reseed: post-resume ZWD re-seed window (SPEC_synthetic_minimum_network §17 finding 3).  NOT fingerprinted (forward-only resume response policy).
        int  zwd_seam_epochs{};   ///< network.zwd_seam_reseed_epochs: seam-window horizon in emitted epochs.
        double zwd_seam_q{};   ///< network.zwd_seam_reseed_q (RS-10b, SPEC_event_harness §21.21): the SEAM window's own RW process noise (m²/s).  Negative (the -1 default) = unset ⇒ the seam fold borrows @ref zwd_q, byte-identical to the pre-card build.  NOT fingerprinted — which is the point: @ref zwd_q is, so the response's only weight could not be varied on a resume leg.  Read through @ref zwdQEff, never directly at a fold site.
        /**
         * @brief The ZWD carry's effective RW process noise (m²/s) for one fold.
         *
         * The single place the RS-10b override is resolved, so every ZWD fold site
         * (the B9 diagonal row, the B9-in-block column, and the prior-residual
         * census's @c fold_var, which must mirror what was folded or the `zwd`
         * family's σ̂₀ is computed against a variance no row ever used) agrees.
         *
         * @param seam_live  True iff the post-resume seam window is open this epoch
         *                   (@c ctx.carry.zwd_seam_ep_left > 0).
         * @return @ref zwd_seam_q when the window is live AND the knob is armed
         *         (> 0); @ref zwd_q — @c network.zwd_carry_q, the continuous carry's
         *         fingerprinted parameter — in every other case.
         */
        double zwdQEff(bool seam_live) const {
            return (seam_live && zwd_seam_q > 0.0) ? zwd_seam_q : zwd_q;
        }
        bool zwd_capture{};    ///< Snapshot-armed run (state_snapshot_path non-empty): record + harvest the per-station ZWD posterior every processed epoch so EVERY snapshot carries Z rows — read-only w.r.t. the solve (no inject; capture only).
        double amb_seam_reinflate_var{};  ///< network.amb_seam_reinflate_var_m2: one-shot variance ADD (m²) on every restored leveling carry at a warm resume (STABILITY target 2 leg C).  NOT fingerprinted (forward-only resume response policy).  0 = OFF.
        bool   resume_frame_restore{};    ///< network.resume_frame_restore (SPEC_synthetic_minimum_network §18.10): at a warm resume, restore the serialized FRAME state — phase-anchor incumbents + sticky pin targets, the Q3 clock-anchor per-sat state, the CLK-R held frame — so the first post-resume election/fold/emit continue the pre-seam realization instead of re-pinning at cold seeds.  NOT fingerprinted (forward-only resume response policy).  OFF = byte-identical resume (rows parsed into maps nothing reads).
        bool   resume_emit_restore{};     ///< network.resume_emit_restore (RS-5, SPEC_event_harness §21.10): at a warm resume, restore the two OUTPUTS-ONLY emit engines before the first emit — @c NetworkEmitRebase's emission frame (the snapshot's ER rows) and @c NetworkPbEmitFilter's per-key posterior + per-constellation σ̂_step accumulators (PF / PS) — so the emitted phase biases continue the writer's emission LEVEL instead of stepping to the raw solve frame (17.666 m p50 per (sat,signal) measured at the field seam against 1.695 m in the estimator's own state).  Each half INERT and loudly logged when its rows are absent.  Loader hard-requires resume_frame_restore.  NOT fingerprinted (forward-only resume response policy).  OFF = byte-identical resume (rows parsed into containers nothing reads).
        bool   resume_datum_restore{};    ///< network.resume_datum_restore (RS-3, SPEC_event_harness §21.7): at a warm resume, restore the eq34 chain datum's last-epoch spanning tree (sticky datum receiver, receiver parent edges, satellite component roots, pin targets, longevity elevations, gap-retained edges) so the election's sticky seed CONTINUES the writer's tree instead of re-electing from the cold arc registry, and restore the time-linked code-bias previous-value map + ever-seen set with @c code_bias_datum_resumed_ left FALSE.  Loader hard-requires resume_frame_restore (anchor_incumbent is the same tree's satellite side).  NOT fingerprinted (forward-only resume response policy).  OFF = byte-identical resume (rows parsed into containers nothing reads).
        bool   resume_station_restore{};  ///< network.resume_station_restore (RS-6, SPEC_event_harness §21.13): at a warm resume, restore the two STATION-LAYER memories before the first epoch — the rx-CB carry (the snapshot's RC rows, through @c makeRestoredLocalCarry so every entry carries the §15.4.1 restored-arc sentinel) and the T5 / Iono-2 per-LOS SH deviation carry (the SD rows, as written: @c ShDevCarry's own cold test is @c t_s <= 0, so the restored harvest stamp is what makes the entry read WARM and the epoch's aging pre-pass ages it over the true gap).  Each half INERT and loudly logged when its rows are absent.  Loader hard-requires resume_frame_restore.  NOT fingerprinted (forward-only resume response policy).  OFF = byte-identical resume (rows parsed into containers nothing reads).
        bool   resume_station_rxcb{};     ///< network.resume_station_restore_rxcb (default true) — DIAGNOSTIC sub-switch of @c resume_station_restore, so one §21.13 attribution leg can restore the rx-CB half alone.
        bool   resume_station_shdev{};    ///< network.resume_station_restore_shdev (default true) — the @c resume_station_rxcb twin.
        bool   resume_signal_canon_restore{};  ///< network.resume_signal_canon_restore (RS-8, SPEC_event_harness §21.16): at a warm resume, restore each station's per-(constellation, band, C|L) canonical-signal latch (the snapshot's @c CN rows) into its @c PppModel before the first epoch, so @c PppModel::decideConstellationCanonicals treats the station as already decided instead of re-deciding the latch from the RESUME epoch's satellite counts (measured: GAMB00PYF L2X→L2W, CPVG00CPV L2W→L2X ⇒ the L2X tracker set changes, the eq34 L2X component loses its datum receiver and six phase-bias families step ~25 m at +1).  INERT and loudly logged on a snapshot with no @c CN rows.  Loader hard-requires resume_frame_restore.  NOT fingerprinted (forward-only resume response policy).  OFF = byte-identical resume (rows parsed into a container nothing reads).
        bool   resume_rxcb_lin_restore{}; ///< network.resume_rxcb_lin_restore (RS-9, SPEC_event_harness §21.18): at a warm resume, stash the snapshot's @c RL rows (the writer's code-bias @c x_lin per (station roster index, CB state key)) and seed each station's code-bias state from them at its first-epoch @c StateInitializer::initializeCodeBiases, instead of from ONE epoch of the resume epoch's code.  RS-6 restores the rx-CB carry's NUMERATOR; this restores the REFERENCE its inject gate differences against, which is the same seam's other edge (invariant #7).  Solve-invariant by construction — an @c x_lin shift is an exact solve no-op for a state that enters linearly and the gate is its only consumer — so the loader REFUSES @c network.rx_code_bias_datum with it (that datum's RHS reads @c x_lin).  Hard-requires @c resume_station_restore + its rxcb half.  NOT fingerprinted.  OFF = byte-identical resume (rows parsed into a container nothing reads).
        bool   resume_border_restore{};   ///< network.resume_border_restore (RS-7, SPEC_event_harness §21.14): at a warm resume, rebuild the C1 dense shared-carry covariance (@c SharedCarryCov) from the snapshot's QK/QP records for every key that also has a restored D row, so the FIRST warm fold runs the dense @f$(P_{prev}+Q)^{-1}@f$ path instead of the per-key DIAGONAL every resume has degraded to.  Split out of @c resume_coupling_restore on purpose: that knob also restores the coupling ROWS (QL/QZ regressors), whose truncation breaks the family identity @f$M^Tg_s=-g_r(c)@f$ and therefore needs Card 1f; a COVARIANCE block carries no regression and no gauge identity, so it is legal on a @c carry_invariant lineage where 1f is out of regime.  Loader hard-requires @c resume_frame_restore and warn-disarms without @c shared_carry_full_cov.  NOT fingerprinted (forward-only resume response policy).  OFF = byte-identical resume.
        bool   resume_pb_seam_mark{};     ///< network.resume_pb_seam_mark (RS-4, SPEC_event_harness §21.9): at the FIRST emit after a warm resume, markDatumChange any constellation more than kResumePbSeamMarkFraction of whose fresh-baseline phase-bias keys moved past discThresholdM against their RESTORED emitter baselines - the declaration the CLK-R resume_seam rule misses because it fires only on a COLD held-frame engine.  NOT fingerprinted (forward-only resume response policy).  OFF = byte-identical (the census is never taken).
        bool   resume_coupling_restore{}; ///< network.resume_coupling_restore (SPEC_synthetic_minimum_network §18.12): at a warm resume, rebuild every restored leveling / ZWD carry's Card-1 regression on the free shared border from the v3 snapshot's QS dictionary + QL/QZ rows (verbatim, in harvest order) and seed tu_reinit_hist from the columns the write epoch was RE-DRAWING, so the first PASS-B inject builds a CONDITIONAL row (A_s = M_r, shared-neutral) instead of the plain DIAGONAL prior asserted at the PURE CONDITIONAL variance that out-voted 33 of 52 Q3 anchored clock priors at the first fold (§18.11).  Loader hard-requires resume_frame_restore + carry_conditional_coupling + carry_coupling_gauge_project and refuses amb_seam_reinflate_var_m2 > 0 (rung 3 attacked the row's WEIGHT; this touches DIRECTION only).  NOT fingerprinted (forward-only resume response policy).  OFF = byte-identical resume (LocalCarry::coupling stays empty exactly as the aggregate init leaves it).
        /// network.readmit_seed_horizon_s — READMIT continuity (E2 seed-first re-admission,
        /// SPEC_synthetic_minimum_network §18.4 / SPEC_amb_iono_split_overcount §7 FIRST):
        /// a departed satellite's D2 Clock + CodeBias carry is PARKED for this many seconds
        /// and seeds its re-registered columns at var + q·gap; the Q3 anchor folds ANCHORED
        /// over the true gap (bounded by this horizon instead of clock_anchor_max_gap_s)
        /// and its NIS guard is the mark-if-moved.  0 = OFF (byte-identical: nothing is
        /// ever parked).  NOT fingerprinted: parking adds entries, it changes no carried
        /// value's meaning, and the snapshot never carries a parked entry.
        double readmit_seed_horizon_s{};
        /// network.readmit_coupling_marg_ep — the block/row RE-COUPLING edge at satellite
        /// re-admission: for this many epochs after a satellite's clock column returns from a
        /// >= kReadmitMinGapEp absence, every coupling term on its shared columns takes the
        /// departed route.  0 = OFF.  Requires the Card-1 coupling rows (loader-checked).
        int    readmit_marg_ep{};
        /// network.readmit_seed_code_bias — park + seed the per-signal CODE-BIAS columns too
        /// (true, the default) or the clock alone (false).  Inert without readmit_seed_horizon_s.
        bool   readmit_seed_cb{};
        int    resume_d5_warmup_ep{};     ///< network.resume_d5_warmup_ep (RS-2b, SPEC_event_harness §21.6): PROCESSED epochs after a warm resume during which the Card-4 D5 innovation gate is DIAGNOSTIC-ONLY — a tripping chord is counted but not re-tapped, not marked slipped, and its block is not refolded.  A measurement knob: it holds the restored branch through the seam so the re-tap cannot destroy the hypothesis it would test.  0 = OFF.  NOT fingerprinted.
        int    resume_slip_warmup_ep{};   ///< network.resume_slip_warmup_ep: epochs after a warm resume during which the MW/LLI (T0) drain is DIAGNOSTIC-ONLY — its cold Welford reference (2 samples at the second warm epoch) declared 146 false WL slips on the §18.9 bed and re-leveled 5 % of the restored arcs; IF-TD (T1) and wind-up drains stay live, so an in-gap slip keeps a detector.  0 = OFF.  NOT fingerprinted.
        bool carry_vce{};
        bool vce_apply_on{};   ///< M2 v2 (docs/SPEC_carry_vce_apply.md): the LASER 5-95 variance-factor apply. SOLVE-AFFECTING; loader guarantees carry_vce.
        bool ura_on{};
        double ura_alpha{};
        bool ura_retro{};
        bool mark_refsat_reset_disc{};   ///< Edge-1 piece 2 (network.mark_refsat_reset_disc).
        bool datum_station_detect{};     ///< §9.7.10 datum-station slip detect, RESOLVED for the run: armed AND legacy pivot datum (false under any no-pivot datum).  Third producer into the refsat/retro mark queue — joins the SolPrep drain condition.
        double ura_refloat_inflate{};
        bool raim_on{};
        double raim_k{};
        double raim_s0_floor{};
        int raim_min_sta{};
        int raim_arc_persist{};
        bool raim_maturity_gate{};
        int raim_maturity_epochs{};
        double raim_withhold{};
        /// Withhold-era release: escalated within-epoch
        /// adaptation budget on epochs whose predecessor was A4-withheld; 0 = off
        /// (stock budget everywhere).  Loader guarantees 0 or > 5, and 0 without an
        /// armed A4 ceiling.
        int wh_escalate{};
        /// §3b: erase detector-freed leveling carries at the A4 withhold branch so the
        /// free re-level survives the discarded epoch.  Loader guarantees
        /// relevel_free_epoch + an armed ceiling.
        bool wh_free_erase{};
        bool raim_redun{};
        /// SPEC_t3_null_derivation §7.1: emit the per-ROW Baarda redundancy census
        /// (`network.trace_raim_redundancy`).  DIAGNOSTIC ONLY and deliberately
        /// independent of @ref raim_redun — it makes `row_r` be COMPUTED without
        /// letting it be USED, so the census is solve-neutral where arming
        /// `raim_redundancy_normalize` is not.  The open question it exists to answer
        /// is whether r_i is signal-dependent: the RAIM cell is a max OVER signals, so
        /// no trace on disk can currently separate L1C from L2W/L5Q.
        bool raim_redun_trace{};
        /// POOL RE-HOST, RESOLVED for the run: the
        /// post-fit RAIM screen runs on the Card-4 invariant-carry lineage against the
        /// carry-neutral `qc_amb` census.  Already ANDed with `raim_on` and
        /// `carry_invariant`, so the driver, the assembly and the seam all read ONE bool
        /// and cannot disagree about whether the census exists.
        bool raim_ci_on{};
        /// The ACTUATOR half: a re-hosted flag is queued into `raim_refloat_pending` and
        /// drained into the C3c funnel at the next epoch's head.  ANDed with
        /// `raim_ci_on`; false = the screen measures and counts but queues nothing.
        bool raim_ci_refloat{};
        /// Card GL (docs/SPEC_event_harness.md §24.18): the GROSS LINE-OF-SIGHT screen
        /// is armed for this run.  Resolved once from @c network.gross_los_screen so the
        /// driver, the assembly, the seam and the teardown all read ONE bool and cannot
        /// disagree about whether the state exists.  The ONLY solve-affecting QC knob in
        /// this neighbourhood; OFF the seam is never called, no LOS state is allocated,
        /// @c NetStation::gl_code_sigma_scale is never populated, and the run is
        /// byte-identical.
        bool   gl_on{};
        /// Card GL: the σ multiplier a declared line of sight's CODE rows fold at
        /// (@c network.gross_los_deweight) — the assembly divides the row's information
        /// weight by its square.  Held in the knobs rather than re-read from config at
        /// the assembly because the publish half and the fold half must not be able to
        /// disagree mid-run.
        double gl_deweight{ 1.0 };
        /// GL-2 (§24.19): the WHOLE-SATELLITE stand-down is armed for this run.  Already
        /// ANDed with @ref gl_on — the satellite rule is a scope decision ON the gross-LOS
        /// rule and cannot fire without it.  OFF ⇒ no tally, no state, byte-identical.
        bool   gl2_on{};
        /// QUEUE item 4 R2 (SPEC_t3_null_derivation §6/§7.5): declared per-station
        /// variance factor — measure-only, feeds no denominator.
        bool   staf_on{};
        double staf_tau_s{};
        double staf_ratio{};
        /// QUEUE item 4 R4: T3/T3c opportunity-accounting hygiene (telemetry only).
        bool rate_hygiene{};
        /// The per-epoch fleet CS-GF level
        /// census — measure-only.
        bool lvlauth_on{};
        int  lvlauth_min_n{};
        int  lvlauth_min_los{};
        bool lvlauth_los_dump{};   ///< LV-6 G2: dump the census's own per-LOS pierce points.
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
        bool handover_stransform{};  ///< Queue 1(b) (network.handover_stransform): re-express the CHANGED constellation's carry along the structural clock null direction at a pivot handover and fold it, instead of skipping it for one epoch.  Loader forces it false without scoped_handover / under an anchor-free clock datum.
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
        bool clk_frame_hold{};  ///< Card CLK-R stage 1 (network.emit_clock_frame_hold): emit satellite clocks in the per-constellation HELD frame.  Engine = run-TU local (banner rule), like NetworkPbEmitFilter.
        bool clk_jump_test{};   ///< Card CLK-R stage 2 (network.clock_disc_jump_test): mark per-sat common-removed clock steps via the R-record retro-bump.
        // --- Card Q3, the guarded temporal clock anchor ---
        // SOLVE-AFFECTING and FINGERPRINTED.  `cta_on` is the single "does this run
        // touch the clock carry at all" predicate every site reads, so an OFF run
        // never allocates the per-sat state and never diverges from the legacy fold.
        bool   cta_on{};             ///< network.clock_temporal_anchor
        double cta_q{};              ///< network.clock_anchor_q (m²/s) — the anchor's RW rate; emitted walk = √(q·Δt)
        double cta_var_floor{};      ///< network.clock_anchor_var_floor_m2 — deviation-#1 consistency floor (clock analogue of the B4 pb/cb floors); 0 = off
        double cta_max_gap_s{};      ///< network.clock_anchor_max_gap_s — per-sat staleness bound; beyond it the satellite DECOUPLES rather than folding a stale value
        double cta_k{};              ///< network.clock_anchor_k — |w| trip threshold on the standardized predicted residual (empirical, §3)
        double cta_abs_m{};          ///< network.clock_anchor_abs_m — floor-independent absolute backstop on |ê| (m); 0 = off
        double cta_sigma_floor_m{};  ///< network.clock_anchor_sigma_floor_m — √S floor so a weak-data epoch gives w ≈ 0 instead of 0/0
        int    cta_recouple_ep{};    ///< network.clock_anchor_recouple_epochs — consecutive in-band epochs a LOOSE satellite must serve before the anchor re-engages (invariant #7 re-validation)
        bool   cta_mark{};           ///< network.clock_anchor_mark — wire-mark the DECOUPLING edge (per-sat disc via the B1 retro-bump chain)
        /// Card C-GF: the ONE resolved gauge-free predicate every fold / withdraw / replay
        /// site reads — @c (cta_on && cta_gauge_free) || (shared_carry_full_cov &&
        /// shared_carry_gauge_free).  Never re-derive this at a call site.  Fold and
        /// withdraw must agree exactly: a fold that downdated against a withdraw that did
        /// not leaves the negative-semidefinite residue −ΛU(UᵀΛU)⁻¹UᵀΛ in the shared
        /// normals, which is no longer PSD along the gauge — the constrained solve then
        /// either throws on its pivot guard (persistent epoch skip, since the stash never
        /// refreshes) or factorizes into a saddle and returns a silently wrong x_s.
        bool   carry_gauge_free{};
        /// Card C-GF: require SUPPORT CLOSURE on each datum direction before downdating
        /// it.  True only on the new carry-side arming — the legacy Q3 path keeps its
        /// carried-clocks-with-a-two-member-floor rule so it stays byte-identical.
        bool   carry_gauge_free_closure{};
        bool   cta_gauge_free{};     ///< network.clock_anchor_gauge_free — Card Q3 §2.1: marginalize the per-constellation SATELLITE-CLOCK gauge direction out of the folded clock prior, so the anchor constrains only the estimable between-satellite differences and leaves the common mode to the datum.  Measured motive: the plain anchor suppressed CLK-R's drained Σ|μ| 49.4 → 27.8 m over 3 h, i.e. the carry was doing 44 % of the gauge's work.
        bool   cta_resolve{};        ///< network.clock_anchor_resolve — adapt-and-EMIT: withdraw the tripped prior and RE-SOLVE the epoch before emission
        // network.clock_anchor_gauge_free_stat resolved ONCE (auto → follow the fold),
        // so the guard and the census read predicates rather than re-deriving the
        // mapping — the same discipline `cta_on`'s clock_model interlock follows above.
        // These select the GUARD's denominator ONLY.  The WITHDRAW path must keep
        // reading `cta_gauge_free`, because what it un-folds has to be what was folded.
        bool   cta_stat_proj_post{};  ///< P⁺ = the GAUGE-PROJECTED posterior variance of the common-removed contrast, not the raw marginal cov(c,c)
        bool   cta_stat_proj_prior{}; ///< P⁻ = the GAUGE-PROJECTED prior variance of that same contrast, not the raw per-satellite var_eff.  Set only by 'proj', the one consistent pairing.
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
        /// docs/SPEC_pd_safe_dd_fold.md — @c network.carry_invariant_pd_safe, resolved
        /// against its @c carry_invariant prerequisite.  FINGERPRINTED: the DD z-store a
        /// pd_safe run writes was produced by a different (repaired, product-form)
        /// recursion and its blocks are PD by construction, so a resume across the flip
        /// must cold-start rather than mix the two arithmetics inside one lineage.
        bool carry_invariant_pd_safe{};
        /// docs/SPEC_carry_aware_covariance.md — @c network.carry_aware_covariance,
        /// resolved against its @c carry_invariant prerequisite.  NOT fingerprinted: its
        /// only consumers are @c dd_sigma_cyc and the log-only AR probe, which feed nothing
        /// back, so ON and OFF must agree BYTE-FOR-BYTE on the solve artifacts and differ
        /// in exactly two trace files.  A solve-affecting successor would need its own,
        /// fingerprinted, knob.
        bool carry_aware_covariance{};
        double eq34_rpb_prior_sigma{};
        double eq34_rpb_prior_w{};
        double eq34_rpb_q{};
        bool eq34_shift_on{};
        bool eq34_longevity{};
        bool eq34_extend_only{};
        bool eq34_slip_aware{};   ///< network.phase_datum_eq34_slip_aware_election — keep slipped arcs out of the spanning tree so a slip costs one chord, not every cycle through a tree edge.
        bool eq34_seed_round{};   ///< network.phase_datum_eq34_seed_round (docs/SPEC_eq34_seed_round.md) — round otherwise-zero pin targets to λ·round(x_state/λ); |innovation| ≤ λ/2 by construction.  NOT gated on !carry_invariant (the Card-4 zero-target rationale is re-pin continuity; ignition is this knob's whole point).
        bool eq34_seed_round_rehost{};   ///< network.phase_datum_eq34_seed_round_rehost_frame (SPEC §8.5 INC-2) — a pin that re-hosts takes its frame step from the NEW host's own ambiguity state (exact pre-pin float) and stays shiftable, instead of an unknown step that re-references the (sat,σ).  INERT unless eq34_seed_round.
        bool phase_datum_prioritized{};
        double phase_hold_sigma{};
        bool phase_hold_on{};
        double phase_hold_var{};
        // --- eq34 reattach hygiene ---
        // All four RESOLVED against the datum mode: the loader already disarms them
        // outside an anchored election, and `anchor_hygiene_on` is the single "does
        // this run allocate the arc-age map at all" predicate every site reads, so an
        // OFF run never touches the new state.
        bool   anchor_hygiene_on{};   ///< Any increment armed on an anchored (non-ensemble) phase datum — the master gate for the arc-age map and the hygiene seam.
        /// TWO-EDGE CONTINUOUS HANDOVER, RESOLVED for the run: an anchor handover
        /// pins the
        /// INCOMING anchor at its own absolute ambiguity estimate in the OUTGOING
        /// frame instead of at 0, making the gauge change an S-transform.  Already
        /// ANDed with `anchored && !phase_datum_eq34`, so the census fill, the
        /// post-solve harvest and the election all read ONE bool and cannot disagree
        /// about whether the ledger exists.
        bool   pd_handover_continuous{};
        /// Conditional-variance ceiling (m²) a handover ledger value must beat
        /// before it may be pinned — invariant #7's re-coupling re-validation.
        /// 0.0 when the mechanism is off.
        double pd_handover_max_var{};
        /// network.phase_datum_handover_max_var_ratio — the honest-σ-scale ceiling: a ledger
        /// value is usable only if var <= ratio × its arc's FIRST-STASH conditional variance
        /// (unknown var_first ⇒ refused).  The absolute pd_handover_max_var stays a rail.
        /// 0.0 = OFF (absolute ceiling only); resolved with pd_handover_continuous.
        double pd_handover_max_var_ratio{};
        /// QUEUE item 1(d): PHASE_ANCHOR carry re-reference, RESOLVED for the run
        /// (docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1).  On an anchor
        /// handover the (sat,signal) family's shared PB carry is MARGINALIZED for
        /// one epoch and its phase-leveling local carries erased at every station
        /// — the eq34_reref treatment for the anchored election, closing the
        /// carry-side edge whose tension the satellite clock absorbed.  Already
        /// ANDed with `anchored && !phase_datum_eq34` (the eq34 chain owns its own
        /// re-reference; the ensemble has no anchors), same belt-and-braces as
        /// pd_handover_continuous above.
        bool   pa_carry_reref{};
        /// COLD ANCHOR-FALLBACK, RESOLVED for the run (network.phase_datum_candidacy_usable,
        /// docs/SPEC_event_harness.md §15): at a handover the successor is chosen from the
        /// coverage-ranked candidates whose CARRY IS USABLE by the continuous branch's own
        /// predicate, instead of from the first tracker of the key.  Already ANDed with
        /// `pd_handover_continuous` (with no ledger there is nothing to screen against),
        /// which is itself ANDed with `anchored && !phase_datum_eq34` — so, as with the two
        /// knobs above, every site reads ONE bool.
        bool   pd_candidacy_usable{};
        /// network.phase_datum_candidacy_min_age_ep — the maturity bar in loop epochs
        /// (0 = no age test); 0 when the mechanism is off.
        int    pd_candidacy_min_age_ep{};
        /// COLD ANCHOR-FALLBACK iteration 2 (SPEC_event_harness §15.8): resolve a
        /// satellite's handover JOINTLY so its families cannot elect different hosts.
        /// Already ANDed with pd_candidacy_usable; false there means iteration 1's
        /// per-family screen, which is the single-knob A/B.
        bool   pd_candidacy_satellite{};
        int    eq34_min_arc_ep{};     ///< INC-1 eligibility threshold in consecutive-present epochs (0 = no gate).
        double prior_realization_max_gap_s{};  ///< INC-1 warm conditioning: the block applies only while the key's last EMITTED realization is younger than this (s); <= 0 disables the conditioning.
        bool   pin_bridge_on{};       ///< INC-2 time-linked phase datum armed.
        double pin_bridge_sigma{};    ///< INC-2 bridge-row sigma (m).
        double pin_bridge_info{};     ///< INC-2 bridge-row information 1/sigma^2 (0 when disarmed).
        int    pin_bridge_max_ep{};   ///< INC-2 maximum consecutive folds of one bridge.
        double fresh_pin_scale{};     ///< INC-3 maximum sigma multiplier on a fresh-host datum pin (1.0 = off).
        int    fresh_pin_ramp_ep{};   ///< INC-3 ramp length in epochs (0 = off).
        // --- SBC S-basis completion (network.sbasis_complete) ---
        // Resolved once so the pb_level_prev harvest gate and the solve-glue block
        // read the same predicate; the completion also re-uses the harvest the
        // INC-2 bridge added (the shared PhaseBias carry is not in prev_carry
        // under carry_invariant — the completion's continuity targets need the
        // same alternate source the bridge did).
        bool   sbasis_complete_on{};  ///< SBC armed: constrain realized extra null directions instead of withholding.
        int    sbasis_complete_max{}; ///< SBC per-epoch direction cap (beyond ⇒ refuse + withhold); ≤0 = uncapped.
        // --- flicker/gap signal integrity ---
        // All RESOLVED against the master arm: the loader has already disarmed every
        // action when `census` is off, and `sig_integrity_on` is the single "does this
        // run allocate the band-state map at all" predicate every site reads, so an
        // OFF run never touches the new state.
        bool   sig_integrity_on{};    ///< Master: the band-state classifier runs this epoch (network.signal_integrity.census).
        double sig_unvetted_scale{};  ///< INC-2 sigma multiplier on an unvetted LOS's PHASE rows; 1.0 = no deweight.
        bool   sig_withhold{};        ///< INC-2 alternative: REMOVE the unvetted LOS's phase observables instead of deweighting.
        int    sig_revet_ep{};        ///< INC-3(b) consecutive full-band epochs required to re-earn STABLE.
        int    sig_flicker_max_ep{};  ///< INC-1 maximum band outage (epochs) still classed as a FLICKER.
        bool   sig_refloat_regain{};  ///< INC-3(a) refloat the LOS's ambiguities on ALL bands when a flickered band returns.
        bool   sig_resume_warmup{};   ///< ADDENDUM: vet EVERY arc for sig_revet_ep epochs after a warm resume (tight carries restored, detector state not).
        // --- iono-deweight RELEASE hygiene ---
        // RESOLVED against the SH field and the gate: the loader has already disarmed
        // every increment without estimate_iono_sh (and INC-1 without the adaptive
        // gate), and `dw_hygiene_on` is the single "does this run sweep at all"
        // predicate the seam and the assembly read, so an OFF run touches nothing.
        bool   dw_hygiene_on{};          ///< Master: any increment armed on a live SH field — the gate for the release seam and the held-row lookup.
        bool   dw_release_reseed{};      ///< INC-1 release-time slant reseed + ambiguity refloat + stash erase armed.
        double dw_release_eps{};         ///< INC-1 "decoupled" weight threshold; the release edge is a rise ABOVE it.
        double dw_park_bound_tecu{};     ///< INC-2 while-decoupled park bound in SLANT TECU (0 = off).
        double dw_release_validate_k{};  ///< INC-3 standardized deviation screen on a re-engaged tie row (0 = off).
        double dw_admit_validate_k{};    ///< INC-3b the same screen at a LOS's FIRST tie row — the admission gate that was the measured valve (0 = off).
        // --- redundancy / testability census ---
        // `redun_census_on` is the single "does this run enter the census seam at
        // all" predicate the driver reads, so an OFF run never calls into the
        // translation unit and pays nothing beyond this bool test.  The census is
        // MEASURE-ONLY, so there is no estimation precondition to resolve against
        // — only the two arming knobs.
        bool   redun_census_on{};     ///< Master: the stride OR the escalation trigger is armed.
        // --- Baarda reliability numbers / MDB ---
        // `mdb_on` is the single "does this run compute an MDB at all" predicate
        // that BOTH feed sites and the flush read, so an OFF run pays one bool
        // test per site and nothing else.  The two source flags are resolved here
        // (not re-derived at the sites) because the run-end invariant-#4 lines
        // need to say WHICH feed was armed when a source contributed zero rows —
        // "the RAIM pool never ran warm" and "no sweep completed" demand
        // different fixes.
        bool   mdb_on{};              ///< Master: network.mdb, AND at least one live r_i source.
        bool   mdb_from_raim{};       ///< The RAIM-pool feed site is armed (raim AND raim_redundancy_normalize).
        bool   mdb_from_census{};     ///< The redundancy-census feed site is armed (mirrors @ref redun_census_on).
        double mdb_event_m{};         ///< REPORTING threshold, metres (0 = no events).  Nothing acts on it.
        long long mdb_event_holdoff{};///< Per-cell MDB event rate limit, in flush passes.
        // --- prior-residual census -------
        // `prires_on` is the single "does this run touch the census at all"
        // predicate every feed site reads, so an OFF run pays one bool test per
        // site and nothing else.  The five per-family arms are RESOLVED here (the
        // families CSV is parsed once at run start, never per row) so a feed site
        // in the serial carry harvest tests a bool rather than searching a vector.
        bool   prires_on{};           ///< Master: the census is armed AND at least one known family was requested.
        bool   prires_pl{};           ///< Phase-leveling ambiguity carry family armed.
        bool   prires_pb{};           ///< Satellite phase-bias carry family armed.
        bool   prires_cb{};           ///< Satellite code-bias carry family armed.
        bool   prires_zwd{};          ///< INC-2 ZWD (B9) carry family armed.
        bool   prires_iono{};         ///< QUEUE 8b slant-iono carry family armed — the ONE instrument that can see a wrong carried iono value (T4 pools phase rows only; the prefit census is x_lin-framed).
        bool   prires_rxcb{};         ///< INC-2 receiver code-bias carry family armed.
        bool   prires_clk{};          ///< Card Q3 INC-0: satellite-CLOCK carry family armed.  Unlike every other family its ê is COMMON-MODE REMOVED per constellation before standardizing — the clock is the one carry with a free per-epoch gauge, and the raw ê would report that gauge on every satellite (standing invariant #5).  Trace-only; this family is the empirical instrument the anchor's q and k are derived from (§3).
        bool   prires_corrob{};       ///< INC-3 raw-observable corroboration join armed.
        double prires_report_sigma{}; ///< Per-row TRACE threshold in σ̂₀ units (nothing acts on it).
        int    prires_event_holdoff{};///< Per-cell PRIORRES event rate limit, in census epochs.
        // --- constraint-aware global σ̂₀
        bool   prires_s0g{};          ///< The global variance-component rider is armed (implies @ref prires_on).
        bool   prires_s0g_redun{};    ///< A per-row Baarda r_i actually exists for the data side (`raim` AND `raim_redundancy_normalize`).  FALSE ⇒ the r ≡ 1 fallback, flagged per epoch.
        // --- BlockFormal LOS block census ---------
        // `bb_on` is the single "does this run sweep blocks at all" predicate,
        // read at ONE site (inside the redundancy census's swept branch) and by
        // the emission seam.  It is the AND of the master arm and a LIVE HOST:
        // this card deliberately has no scheduler and rides the redundancy
        // census's swept stations, so with that census off there is nothing to
        // ride and the sweep must never be entered.  The loader hard-errors on
        // that combination; this AND is the belt-and-braces re-resolution, in the
        // one place a knob may be computed (ctx.knobs is const thereafter).
        bool   bb_on{};               ///< Master: network.block_baarda AND a live redundancy-census sweep.
        bool   bb_corrob{};           ///< INC-2 raw-observable corroboration join armed (implies @ref bb_on).
        double bb_report_sigma{};     ///< TRACE/event threshold on t_B.  A VARIANCE RATIO (E = 1 under H0), NOT σ units — see NetworkBlockBaarda.hpp.
        long long bb_event_holdoff{}; ///< Per-(station, satellite) BLOCKBAARDA event rate limit, in census epochs.
        // --- Station congruence screen -------
        // `cg_on` is the single "does this run screen coordinates at all"
        // predicate.  It is read at THREE sites — the assembly's INC-0 geometry
        // retention, the redundancy census's swept branch, and the emission seam —
        // and, exactly like `bb_on`, it is the AND of the master arm and a LIVE
        // HOST: the screen has no scheduler and rides the redundancy census's
        // swept stations, so with that census off there is nothing to ride.  The
        // loader hard-errors on that combination; this AND is the belt-and-braces
        // re-resolution, in the one place a knob may be computed.
        bool   cg_on{};               ///< Master: network.congruence_screen AND a live redundancy-census sweep.
        double cg_report_sigma{};     ///< TRACE/event threshold on t_i.  A VARIANCE RATIO (E = 1 under H0 for every observable dimension), NOT σ units — see NetworkCongruence.hpp.
        long long cg_event_holdoff{}; ///< Per-station CONGRUENCE event rate limit, in census epochs.
        // --- prefit census / fuse / birth check / topology / flight recorder ---
        // `pc_walk_on` is the single
        // "does this run walk the row stacks at all" predicate the driver reads, so
        // an OFF run never enters the seam.  It is deliberately the UNION of the
        // census, the birth check and the topology ledger: INC-3 and INC-4 ride the
        // same walk, so arming either implies the walk even with the INC-1 columns
        // off (the loader says so in its arming line).  The FUSE is separate — it
        // runs at the pre-addStation assembly site, not in this walk.
        bool   pc_walk_on{};          ///< Master for the post-solve walk (census OR birth check OR topology).
        bool   pc_census_on{};        ///< INC-1 columns + PREFIT exceedance events.
        double pc_event_sigma{};      ///< INC-1 event threshold on the standardized deviation.
        unsigned pc_event_mask{};     ///< INC-1 EVENT domain: bit (1 << NetRowClass) per DATA class allowed to raise an exceedance event (columns always cover all four).
        long long pc_event_holdoff{}; ///< INC-1/2 per-cell event rate limit (processed epochs).
        double pc_fuse_sigma{};       ///< INC-2 weight cap (0 = off) — the one SOLVE-AFFECTING knob of the card.
        unsigned pc_fuse_mask{};      ///< INC-2 domain: bit (1 << NetRowClass) per DATA class the cap may act on.
        double pc_birth_sigma{};      ///< INC-3 seed-birth tripwire threshold (0 = off).
        bool   pc_topo_on{};          ///< INC-4 TOPOLOGY events.
        bool   rec_on{};              ///< INC-5 flight recorder armed (trip latch + retention).
        bool   h_sigma0_census{};     ///< H-rider: feed the H heartbeat's sigma0 from the pooled census σ̂₀ (network.h_sigma0_prefit_census).  Loader guarantees trace_prefit_census + emit_epoch_status_record.
        unsigned h_sigma0_class_mask{};  ///< H-rider: DATA classes pooled into the H σ̂₀ (network.h_sigma0_classes; default code+sh+other — phase excluded per census deviation 2c).
        // --- datum graph census ------------
        // `dg_on` is the single "does this run predict the S-basis dimension at
        // all" predicate, read at THREE sites: the driver's census call, the
        // top-of-epoch clear of the spectral hand-off, and the solve glue's
        // write of it.  All three must agree or the census would reconcile
        // against a stale epoch — which is precisely why it is one resolved
        // knob and not three independent config reads.
        bool      dg_on{};            ///< Master: network.datum_graph_census.
        long long dg_event_holdoff{}; ///< Per-identity DATUMGRAPH event rate limit, in processed epochs.
        // --- product-referenced measurement admission ---
        //     Nothing to resolve against the
        //     estimation configuration: the screen reads `observed` / `modeled` and
        //     removes observables, so the two arming knobs ARE the predicate and the
        //     loader has already clamped the thresholds.  `adm_on` is the single
        //     "does this run enter the seam at all" test the driver reads, and it is
        //     the UNION of the response and the census — the census rides the same
        //     pass, so arming either implies the pass (the loader's arming line says
        //     so).  Screening implies the census columns, per spec §5.
        bool   adm_on{};              ///< Master for the screen pass (response OR census).
        bool   adm_screen{};          ///< INC-1/INC-2 response — the one SOLVE-AFFECTING knob of the card.
        bool   adm_trace{};           ///< INC-3 census columns + ADMISSION events (implied by the response).
        double adm_bound_m{};         ///< |z| rejection bound (m).
        int    adm_min_sats{};        ///< median-validity floor in DISTINCT satellites.
        int    adm_heal_ep{};         ///< consecutive in-bound epochs before a rejected channel is readmitted.
        double adm_max_frac{};        ///< station-guard fraction on the median's trustworthiness.
        // INC-4 re-entry vet.  `adm_revet` is already AND-ed with
        // `adm_screen` by the loader's INERT-guard, but it is re-imposed here so the
        // seam's predicate cannot depend on load order.
        bool   adm_revet{};           ///< INC-4: re-screen an arc RE-ENTRY as a birth rather than inheriting its pre-gap verdict.
        int    adm_revet_gap_ep{};    ///< absent SCREENED epochs before a return counts as a re-entry.
        int    adm_revet_ep{};        ///< screened epochs the vet window stays open after a re-entry.
        double adm_revet_bound_m{};   ///< the TIGHTER |z| bound (m) a re-entering / vetting channel answers to; == adm_bound_m when the scale is 1.0.  Resolved to metres here so the seam never re-multiplies.
        bool   adm_revet_census{};    ///< consult the previous epoch's PREFIT obs_code exceedance flags inside a vet window.
        bool   adm_revet_hold{};      ///< strict posture: withhold a CLEAN re-entry until the heal path readmits it.

        /// One-shot factory — the relocated knob-snapshot statements, original
        /// read order preserved (parameter names match the former use-sites).
        static NetworkKnobs snapshot(const GnssConfig& config_, const NetworkConfig& nc);
    };

    /// @internal Tier-1 cross-epoch carry/run state —
    /// the former runNetwork() persistent declaration stretch, verbatim
    /// (comments included; they are the authoritative per-field docs and the
    /// snapshot-exemption record).
    struct NetworkCarryState {
        std::vector<CarryPrior> prev_carry;                 // D2 stash from the last solve
        // Per-satellite guarded
        // temporal clock anchor.  Empty and untouched unless network.clock_temporal_anchor.
        // NOT snapshotted (three-place-rule exemption, deliberate): a warm resume is an
        // invariant-#1 event, so every satellite re-earns the anchor from LOOSE — the
        // stiffness must not outlive the evidence that justified it.  The knob is in the
        // resume fingerprint, so a toggle change cold-starts anyway.
        ClockAnchorState clk_anchor;
        // Satellites whose anchor DECOUPLED and whose per-sat wire mark has not
        // yet landed in an emitted product.  Drained by networkSolPrep with the same
        // present/pending split every other forced-bump queue uses (mark == land), so a
        // replay reproduces the marks from the serialized R records.
        std::set<std::string> clk_anchor_disc_pending;
        std::set<std::string> prev_clock_refloat_sats;      // Card 1d: clock-monitor flags at the last stash (hard-refloat mode skip-stashes the sat, so prev_carry cannot carry the flag)
        // Card 1d/1g: (kind, sat, signal) → content time of the last reinit FOLD
        // (multi-epoch-gap coverage).  An EMPTY signal is the SAT-WIDE WILDCARD — the
        // clock-QC feeds (inflate_q stash marks, clock refloats) mark a whole satellite
        // kind-by-kind, while the Card-1g realization-step feeds mark one (sat,signal)
        // family, because that is the granularity at which a phase-bias datum is
        // re-referenced.  buildCplRow probes the exact key and then the wildcard.
        std::map<std::tuple<SharedParamKind, std::string, std::string>, double> tu_reinit_hist;
        SharedCarryCov prev_carry_cov;                      // C1: dense covariance side-car of prev_carry (empty when OFF)
        // READMIT continuity (E2 seed-first re-admission + the block re-coupling edge;
        // network.readmit_seed_horizon_s / readmit_coupling_marg_ep).  NOT snapshotted
        // (forward-only response policy): a seam during a satellite's absence simply
        // loses its seed — today's cold return — and the marginalization windows restart
        // empty.  All empty / zero and unread when both knobs are off.
        double readmit_now_s{0.0};          ///< continuous-GPS s of the current epoch, stamped at the D2 install (the stash has no clock of its own)
        double readmit_prev_stash_s{0.0};   ///< continuous-GPS s of the previous KEPT stash — the park stamp a departing entry receives
        std::map<std::string, long long> readmit_last_seen_ep;  ///< sat → n_loop_epochs of its last stash with a registered clock column (the re-admission detector)
        std::map<std::string, long long> readmit_marg;          ///< sat → loop epoch at which its post-re-admission coupling marginalization EXPIRES
        std::map<std::string, double>    readmit_seeded_s;      ///< sat → continuous-GPS s of its last clock seed (the guard's "moved" attribution window)
        struct ReadmitSeed { SharedParamKind kind; std::string sat, signal; double value, var_eff; };
        std::vector<ReadmitSeed> readmit_seed_check;            ///< this epoch's seeds, checked against the solve at the stash (mark-if-moved telemetry for every seeded kind)
        // §6.12.5 DD-trace reference age.  (cons,signal) -> sat -> consecutive TRACED epochs the
        // satellite has been present at >= 2 stations; an absence erases the entry, so the count is
        // a CONTINUOUS run, not a lifetime total.  Advanced and read only inside the stride-gated
        // dd-frac emit, so both maps stay empty on any run that does not trace.  NOT snapshotted:
        // the trace is a diagnostic and a warm resume legitimately restarts its ages from zero —
        // the first traced epoch after a seam then re-elects by the tie-break, which the run-end
        // switch count makes visible rather than silent.
        std::map<std::pair<std::string, std::string>, std::map<std::string, long long>> ddfrac_ref_age;
        std::map<std::pair<std::string, std::string>, std::string> ddfrac_ref_prev;  ///< last reference elected per (cons,signal) — the switch detector
        std::map<std::string, std::string> prev_pivot;      // constellation -> pivot sat (handover detect)
        std::set<std::string> c1_zoh_active;                 // L2/T5: sats currently emitting c1=0 (ZOH) — edge-trigger for the R6 event
        // A1: persistent last-EVER pivot per constellation; MERGE-updated (never
        // wholesale-overwritten) so a constellation that vanishes for an epoch and
        // returns with a DIFFERENT pivot still raises datum_changed.  Read only when
        // network.pivot_vanish_return_signal is on (otherwise inert).
        std::map<std::string, std::string> last_pivot;
        // network.clock_signal_basis_hold: per-satellite HELD ionosphere-free clock
        // reference pair (sat -> {sig1, sig2}) for the W6 IF pin.  Latched the first
        // epoch a satellite's code-bias columns register a usable dual-frequency pair
        // and reused every subsequent epoch while BOTH signals stay registered, so the
        // emitted (δtˢ, dˢ) split is anchored to ONE signal basis for the whole run —
        // the IGS fixed-clock-convention analogue that keeps the differential clock
        // time-consistent.  Re-latched (to the current epoch's freshly-selected pair)
        // ONLY when a held reference signal leaves the ≥2-station set.  Empty / unread
        // unless clock_signal_basis_hold (so the IF-pin datum is byte-identical OFF).
        // Not snapshotted (v1): a warm resume re-latches from first sight after the
        // resume — one small per-sat basis step (trace-logged only, not wire-signalled,
        // exactly like the per-epoch re-selection it replaces); the toggle is in the
        // resume fingerprint so a toggle change already cold-starts.
        std::map<std::string, std::pair<std::string, std::string>> held_clock_ref;
        // Deterministic IF-pin selection discontinuity (hold OFF).  With
        // clock_signal_basis_hold OFF the pin is re-derived from every epoch's
        // coverage; the deterministic NetworkSbasis::selectIfPinPair makes it a pure
        // function of the registered signal SET, so a change between EMITTED products
        // means a channel genuinely entered/left the ≥2-station set — stepping the
        // emitted (δtˢ, dˢ) split by the satellite's inter-channel code bias, a per-sat
        // datum move the consumer must reset for.  Two maps mirror the phase-anchor
        // discipline (this_epoch_anchor / emitted_anchor) so a transient pin move on a
        // WITHHELD epoch cannot fabricate a bump:
        //  - this_epoch_if_pin_sel: the (sig1,sig2) SELECTED this epoch in the main
        //    pass (scratch, cleared each epoch, decoupled from the emit decision);
        //  - last_if_pin_sel: the pair of the LAST EMITTED product per satellite,
        //    compared + advanced ONLY at the emit site (like emitted_anchor/prev_pivot),
        //    so a withheld epoch leaves it untouched and its transient pair is dropped.
        // Both populated only under !clock_signal_basis_hold && emit_clock_disc (the
        // hold path owns its own re-latch signalling via held_clock_ref) ⇒ byte-
        // identical otherwise.  Not snapshotted: a warm resume has no prior for its
        // first emit, so it cannot — and must not — fabricate a disc it cannot verify.
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_if_pin_sel;
        std::map<std::string, std::pair<std::string, std::string>> last_if_pin_sel;
        // S-contract §1.5 DECLARATION scratch — the realized code-bias datum pair per
        // satellite, recorded UNCONDITIONALLY (the only guard is a real dual-frequency
        // pin, f1,f2 > 0) and read ONLY by the cbpin=/gfpin= fill in NetworkSolPrep.
        // Deliberately NOT this_epoch_if_pin_sel: that map is the hold-OFF disc-compare
        // scratch, gated `!clock_signal_basis_hold && emit_clock_disc`, and widening it
        // would change which satellites get a discontinuity bump — a WIRE change.  The
        // fill originally read it anyway, which silently made a product-contract
        // DECLARATION depend on a QC-channel knob: emit_clock_disc defaults false and
        // is absent from the whole ft63→pc305 field chain, so every field posture-C
        // product ever written declared no pin at all while the sim arms (which arm the
        // knob) declared fine.  Recording here covers BOTH selection paths — the
        // hold-OFF per-epoch re-selection and the clock_signal_basis_hold held/latched
        // pair — so the declaration tracks the datum the run actually realized.
        // Cleared each epoch; declaration-only, so the solve and every emitted numeric
        // value are untouched.
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_if_pin_real;
        // S-contract §1.5: the satellites that ALSO received a GEOMETRY-FREE datum row
        // this epoch (network.code_bias_gf_datum, Odijk rd 3b).  The GF row is folded on
        // the SAME (sig1,sig2) as the IF pin, so the pair itself lives in
        // this_epoch_if_pin_real; this set records WHICH satellites got the second row,
        // which is what the gfpin= declaration needs (a satellite with no valid dual-
        // frequency pin gets neither row).  Empty whenever the GF datum is off ⇒ no
        // gfpin= token ⇒ byte-identical S line.  Cleared each epoch.
        std::set<std::string> this_epoch_gf_pin_real;
        /// RF-0f: THIS epoch's per-(satellite, companion signal) inter-signal-correction
        /// verdicts, keyed `"sat|signal"`.  A key in @ref this_epoch_isc_obs means some
        /// present station tracked BOTH that companion and the T_GD pin's band-2 signal on
        /// that satellite, so `ISC^s = b(companion) - b(pinned)` is determined by data and
        /// the bias is emitted carrying `isc=obs`; a key in @ref this_epoch_isc_pinned
        /// means none did, the column was pinned at 0, and the bias is WITHHELD from the
        /// wire — a consumer cannot tell an asserted zero from an estimate by value.
        /// Cleared and refilled each epoch by the solve glue, read by the emit prep; both
        /// empty unless `network.sat_isc_state` is armed.
        std::set<std::string> this_epoch_isc_obs;
        std::set<std::string> this_epoch_isc_pinned;
        // FAMILY-GF: the RUN-LATCHED family census.
        // Odijk rd 3b re-derives PER RECEIVER CHANNEL FAMILY when the station population
        // partitions (the Galileo C/Q-vs-X tracking split), because the primary pin is
        // built on the satellite's lowest-ordered signals and therefore never touches an
        // X-only station's columns.  Whether that partition exists is a property of the
        // receiver POPULATION, not of an epoch, so the verdict is computed once — on the
        // first epoch with a usable station set — and held for the run: re-deciding it
        // per epoch would make the datum topology follow coverage churn, which is the
        // anti-flap failure the CB-ADM census was built to end.  Mid-run bridge
        // appearance/loss is deliberately NOT re-detected in v1; the null-space gate
        // is the backstop and the latched census line makes any such withhold
        // self-attributing.  Empty `partitioned` => zero extra rows => byte-identical,
        // which is the state of every single-family posture validated to date.
        NetworkSbasis::FamilyCensus family_census;
        // FAMILY-GF: the SECONDARY family's realized GF pair per satellite this epoch, the
        // per-family counterpart of this_epoch_if_pin_real.  Feeds the '+'-joined
        // gfpin=<const>:<primary pair>+<secondary pair> declaration, which the wire parser
        // and checkContract already accept verbatim.  Cleared each epoch; empty unless the
        // per-family rows actually folded.
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_family_gf_pin_real;
        // Prioritized phase-bias anchor (network.phase_datum_prioritized) — all three
        // keyed by the "sat_signal" ambiguity form, all empty/unread on the legacy path:
        //  - anchor_incumbent: the STICKY incumbent anchor STATION NAME per (sat,signal),
        //    read when electing (kept while it still tracks the sat), advanced at election.
        //  - this_epoch_anchor: the anchor STATION NAME elected THIS epoch, captured at
        //    election and read at the emit site (decouples emit from the PASS-A scratch).
        //  - emitted_anchor: the anchor NAME of the LAST EMITTED product per (sat,signal),
        //    advanced ONLY at emit (mirrors prev_pivot/last_pivot); a change vs this drives
        //    the per-satellite handover discontinuity.  Never erased (a returning key is a
        //    first sighting, not a fault — matches the last_pivot never-erase policy).
        std::map<std::string, std::string> anchor_incumbent;
        std::map<std::string, std::string> this_epoch_anchor;
        /// R2 STICKY CONTINUOUS TARGET:
        /// the pin target each incumbent anchor is currently realized at, per
        /// (sat,signal) "sat_signal" key.  Without it the continuous handover is a
        /// one-epoch deferral — the ledger pin reverts to 0 at T+1 and
        /// the frame steps anyway, which is why §5.0 measured it null.  Entries
        /// exist only for keys whose LAST handover was continuous (a cold pin or a
        /// first sighting realizes the 0-frame and stores nothing); rebuilt
        /// alongside anchor_incumbent each election, so a key that leaves the
        /// union drops its target with its incumbent.  Empty forever unless
        /// `knobs.pd_handover_continuous`; deliberately NOT snapshotted, exactly
        /// like the ledger below (a resume's first handover is honestly cold).
        std::map<std::string, double> anchor_pin_target;
        /// TWO-EDGE CONTINUOUS HANDOVER ledger:
        /// each anchor-candidate station's ABSOLUTE ambiguity estimate from the
        /// PREVIOUS epoch's solve, so a handover can re-pin the successor in the
        /// outgoing frame rather than re-zeroing it.  Rebuilt wholesale by the
        /// post-solve harvest each epoch — a one-epoch lifetime is the BOUND on what
        /// a decoupled gauge may carry (invariant #7).  Empty unless
        /// `knobs.pd_handover_continuous`, and deliberately NOT serialized into a
        /// warm-resume snapshot: a resume starts cold and its first handover is
        /// counted as such rather than pinned against a stale frame.
        AnchorHandoverLedger handover;
        std::map<std::string, std::string> emitted_anchor;
        // eq34 reattach hygiene, WARM CONDITIONING (spec addendum): the continuous-GPS
        // second at which each key's datum was last REALIZED AND EMITTED — the age of
        // the warm memory a cold pin would have to fight.  Advanced at the same emit
        // site as emitted_anchor above, and read only by filter/NetworkAnchorHygiene:
        // past eq34_prior_realization_max_gap_s there is nothing left to shock (the
        // carried state has expired through its own staleness machinery), so the
        // eligibility block lifts and the key converges the legacy way.  This is what
        // keeps a network RESTART from extending its own phase-bias outage by
        // min_arc_ep epochs.  Empty / unread unless the card is armed; not snapshotted
        // (a resumed leg simply re-earns its baselines on the first emitted epoch).
        std::map<std::string, double> emitted_anchor_s;
        // Card 3 (network.phase_datum_eq34) chain election, all empty/unread otherwise:
        //  - eq34_datum_rx: the STICKY datum receiver r1 STATION NAME per (constellation
        //    |signal) component, keyed "c|σ".  Kept while the incumbent is still present
        //    in the component (fewest datum-receiver handovers); advanced at election
        //    (mirrors anchor_incumbent's per-election stickiness).
        //  - anchor_incumbent / this_epoch_anchor / emitted_anchor are REUSED for the
        //    tree bookkeeping, holding each satellite's PARENT-EDGE station (unique per
        //    "sat_signal" key — receiver-side parent edges live in eq34_rx_parent, NOT
        //    here, so an interior satellite's several tree edges can never collide in
        //    one map slot).  The existing emit-site handover machinery (jump-test disc +
        //    trace) applies unchanged: a parent-edge reattach == an anchor change.
        //  - eq34_rx_parent: each receiver's parent-edge SATELLITE in last epoch's
        //    spanning tree, keyed "<station>|c|σ".  anchor_incumbent (sat side) +
        //    eq34_rx_parent (receiver side) together ARE last epoch's tree — each edge
        //    lives in exactly one map, both replaced wholesale per election — so the
        //    still-live subset seeded into the sticky rebuild is always a forest.
        //  - eq34_sat_root: each sat's component datum receiver r1 last epoch, keyed
        //    "sat_signal".  A root change (r1 handover, component migration) shifts the
        //    whole component frame by a common fractional + per-sat integers, so it
        //    re-references every sat even where all tree edges survived.
        std::map<std::string, std::string> eq34_datum_rx;
        std::map<std::string, std::string> eq34_rx_parent;
        std::map<std::string, std::string> eq34_sat_root;
        // Last epoch's per-(station idx, sat) arc
        // elevation, for the rising-vs-setting test that ranks NEW spanning-tree edges
        // by how long they will survive.  Rebuilt each epoch from sat_el; empty and
        // unread unless network.phase_datum_eq34_longevity_election.
        std::map<std::pair<size_t, std::string>, double> eq34_prev_el;
        // The datum receiver r1 (pdr=) of the LAST EMITTED contract, so a root
        // handover triggers an S-line re-broadcast (advanced only at emit, like the
        // pivot baselines).  Empty on every non-eq34 run.
        std::string eq34_emitted_pdr;
        // Wavelength provider for the pin-to-round re-pin target (λ·round(float/λ));
        // built lazily on first eq34 use.  Null on every non-eq34 run.
        std::unique_ptr<PppUtils> eq34_ppp_utils;
        // Card 3 run-scope observability counters, logged once in the run-end producer
        // summary (the per-epoch reref/erase lines are debug-only, which left the
        // Jul-10 field A/B blind to the reref volume).  Untouched on non-eq34 runs.
        long long eq34_reref_total = 0, eq34_erase_total = 0;
        long long eq34_shift_total = 0;   // carry values S-transformed (v2-shift) over the run
        long long eq34_cpl_sprev_total = 0;   // SPEC_card1_coupling_carry §14: coupling s_prev references re-based WITH those shifts (structurally 0 with coupling off — the stashes are empty)
        long long eq34_gap_repin_total = 0;   // I2: tree edges resumed from a carry-alive gap (re-pinned to their pre-gap integer instead of re-leveling from 0)
        int eq34_mismatch_epochs = 0;
        // Card 3 chain-pin AUDIT (docs/SPEC_synthetic_minimum_network.md §14 T1 (measured record §12.8)): THIS epoch's
        // declared chain-gauge dimension per (constellation,signal) group — key
        // "<cons>|<signal>" (the election's dr_key), value Σ over the group's
        // components of (n_c + m_c).  eq34_epoch_bridged counts the hygiene INC-2
        // bridge pins per group, which are realized on the SHARED side (a bridge
        // row, not a foldAmb pin), so the station-side fold expectation is
        // declared − bridged.  Both replaced wholesale at each eq34 election;
        // empty on every non-eq34 run.  Compared after PASS B's serial consume
        // against the pins that actually LANDED (folded by foldAmb/foldRpb AND on
        // a station est.addStation accepted) — the two silent loss paths standing
        // invariant #4 warns about.
        std::map<std::string, int> eq34_epoch_declared;
        std::map<std::string, int> eq34_epoch_bridged;
        // Tree edges retained across the current gap because
        // their local_carry is still alive — {receiver index, "sat_signal"}.  Rebuilt at
        // each election's sticky-map update; read at the next election's recordEdge to
        // count a gap re-pin.  Empty unless network.phase_datum_eq34_extend_only.
        std::set<std::pair<int, std::string>> eq34_retained_edges;
        // Card 3: last epoch's pin target per (STATION index, "sat_signal") — the
        // exact same-station pinmove baseline for the shift walk.  MUST be keyed per
        // station: an interior sat carries several pins (its parent edge + one per
        // child-receiver edge), each at a different station with its own target —
        // an amb-only key collides exactly like the original this_epoch_anchor
        // defect, and the resulting garbage pinmoves feed back through the carry
        // shifts into next epoch's rounded targets (measured: 4e8-cycle blowup).
        // Replaced wholesale each election.
        std::map<std::pair<int, std::string>, double> eq34_prev_target;
        // Card 3 per-epoch re-reference set (sat, signal): cleared + rebuilt at each
        // eq34 election, read (1) by the local-carry erase right after the election
        // and (2) by the D2/C1 shared-carry consume, which must MARGINALIZE the
        // re-referenced PB keys for that epoch — their carried priors belong to the
        // retired frame, and folding them yanks the new frame by up to the full
        // re-level (the 03:46 silent-re-level mechanism).  Always empty when eq34 off.
        std::set<std::pair<std::string, std::string>> eq34_reref;
        // QUEUE item 1(d) — the PHASE_ANCHOR carry re-reference
        // (docs/SPEC_pivot_visibility_gauge_defect.md §5.07.5 R1).  Per-epoch
        // (sat, signal) set of the anchor election's HANDOVERS: cleared + rebuilt
        // at each anchor election, read (1) by the local-carry erase right after
        // the election and (2) by the D2/C1 shared-carry consume, which
        // marginalizes the re-referenced PB keys for that epoch — exactly the
        // eq34_reref treatment, applied to the anchored (non-eq34) election whose
        // handovers previously re-realized the datum on the DESIGN MATRIX ONLY
        // while the carry was folded forward in the retired frame at q_pb = 1e-12
        // (the satellite clock, at q = 0.01, absorbed the whole frame step — 41 σ
        // measured on G21).  Always empty unless ctx.knobs.pa_carry_reref.
        std::set<std::pair<std::string, std::string>> phase_anchor_reref;
        // 1(d) run-scope observability (the eq34_reref_total precedent): families
        // marginalized and phase-leveling local carries erased over the run.
        long long pa_reref_total = 0, pa_reref_erase_total = 0;

        // --- eq34 REATTACH HYGIENE --------
        // Every member below stays EMPTY unless ctx.knobs.anchor_hygiene_on, so an
        // OFF run allocates nothing and the election is structurally unchanged.
        // None of it is serialized: the warm-resume rule is a SEED (see
        // anchor_age_resume_seed), not a restore — adding an age map to the snapshot
        // would change the format for every run, armed or not (the sd_ewl exemption
        // precedent), and sub-min_arc_ep fidelity across a resume is explicitly
        // deferred.
        //
        // Arc age per (STATION index, "sat_signal"): CONSECUTIVE epochs this arc has
        // been present with an anchorable phase-bias state.  Rebuilt wholesale each
        // election (an absent arc simply drops out — absence resets the age, which is
        // the conservative direction), and ERASED on a slip-funnel hit or a
        // pending-reseed flag for the arc, so a refloated ambiguity can never present
        // itself as a mature host (standing invariant #1).
        std::map<std::pair<int, std::string>, int> anchor_arc_age;
        // Warm-resume seeding latch (spec §2 INC-1 "warm-resume rule").  Consumed by
        // the FIRST hygiene pass of a resumed run, which seeds every present arc at
        // eq34_anchor_min_arc_ep instead of 1 — otherwise a resumed leg would block
        // every reattach for min_arc_ep epochs.  Deliberately seeded from PRESENCE
        // rather than from "carried keys": the FT63-class stacks run with
        // phase_leveling OFF, so local_carry is empty there and a carry-keyed seed
        // would silently seed nothing — the exact failure the rule exists to prevent.
        bool anchor_age_resume_seed = false;
        // INC-2 bridge source: the last SOLVED shared phase bias per "sat_signal",
        // harvested at the D2 stash (kept epochs only, like code_bias_prev_).  Kept
        // separate from prev_carry deliberately — under carry_invariant the shared
        // PhaseBias carry is NOT stashed at all, so prev_carry would be an empty
        // source on exactly the stack this card was root-caused on.
        std::map<std::string, double> pb_level_prev;
        /// One open INC-2 bridge (the key's gauge is realized by a soft shared row at
        /// @c value instead of a tree-edge / anchor pin).
        struct PinBridge {
            double value = 0.0;      ///< the held shared phase bias (m), from pb_level_prev at open time
            int    folds = 0;        ///< consecutive epochs this bridge has been folded (staleness bound)
            double opened_tow = 0.0; ///< TOW of the epoch it opened (trace only)
        };
        std::map<std::string, PinBridge> pin_bridge;   ///< "sat_signal" -> open bridge.
        // SBC (network.sbasis_complete) engage/release identity: the satellites
        // participating (|eigenvector component| >= NetworkSbasis::kSbasisAttrSpan)
        // in a direction constrained LAST epoch.  Standing invariant #7 — the
        // constraint is a decoupling at its engage edge and a re-coupling at its
        // release edge, and BOTH must be events: a satellite present here but in
        // no direction THIS epoch has re-observed (the data re-determines the
        // held combination from its carried level) and emits sbc_release.
        // Per-SATELLITE rather than per-direction on purpose: two simultaneously
        // weak satellites' eigenvectors mix (their eigenvalues are same-scale),
        // so a direction-shaped identity would flicker on the rotation while the
        // participating-satellite set stays stable.
        std::set<std::string> sbc_active_sats;
        // Invariant #7: declared datum directions currently BREACHED (R >= 1, i.e. some
        // pre-gate fold is pinning a direction the datum owns).  Persistent across epochs
        // purely so the breach/clear EDGES can be emitted — a direction that re-frees
        // itself must produce a clear edge rather than reading as a first sighting the
        // next time it breaches (the FT63 topology-ledger lesson, invariant #7's
        // re-coupling half).  Identity is the direction label ("CLK|G").
        std::set<std::string> ddi_breached;
        // Keys currently HELD OUT of the anchored set because no candidate host has
        // converged (INC-1 with the bridge disarmed or unavailable).  PERSISTENT, and
        // load-bearing: a held-out key is absent from this_epoch_anchor, so it also
        // drops out of anchor_incumbent at the end of the election — without this set
        // it would read as a FIRST SIGHTING the very next epoch, sail past the gate
        // (first sightings are deliberately never gated) and be pinned on the same
        // still-cold host one epoch later.  The hold therefore lasts until an eligible
        // host appears, which it must: a held-out key's arcs keep ageing (its
        // phase-bias states are re-allocated by the next epoch's PASS A, so it stays
        // in usable_keys), so the hold self-clears after at most
        // eq34_anchor_min_arc_ep epochs of continuous presence.
        std::set<std::string> anchor_blocked;
        // --- FLICKER/GAP SIGNAL INTEGRITY ---
        // Per-(STATION index, SATELLITE) band-state history: the vetted reference
        // phase-signal set, which reference bands are currently missing and since
        // when, the last epoch the line of sight was present, and the re-vet counter.
        // Keyed per LINE OF SIGHT rather than per (satellite, signal) arc because
        // across a loss of lock EVERY band's cycle counter is arbitrary at once (the
        // measured G21@MBAR case carries LLI=1 on both), so trusting one band while
        // demoting the other would be the same absence-of-evidence claim the card
        // exists to refuse.
        //
        // EMPTY unless ctx.knobs.sig_integrity_on, so an OFF run allocates nothing.
        // NOT serialized: a resumed leg's first epoch sees every LOS as a first
        // sighting and re-vets it, which is the CONSERVATIVE direction — a resume IS
        // a window in which no time-difference detector can operate.  (Deliberately
        // the OPPOSITE resume rule from the eq34 arc-age map, which had to SEED
        // mature because blocking every reattach for min_arc_ep epochs would have
        // been an active harm; here the conservative default costs revet_ep epochs
        // of deweighted phase and nothing else.)
        std::map<std::pair<int, std::string>, SignalIntegrityArc> si_arc;
        // RESUME WARMUP countdown (spec ADDENDUM).  Set to `sig_revet_ep` by a
        // successful warm resume and decremented by each classifier pass; while it is
        // positive EVERY present line of sight is demoted, regardless of its own band
        // state.  A resume restores TIGHT carries but no detector state (nothing in
        // the snapshot deserializes MW Welford counts or IF-TD arc history), so those
        // first epochs are network-wide detector blindness against fully-warm state —
        // the flicker hazard at scale, and the one case the per-arc rule cannot see
        // because every arc looks like a first sighting to a freshly-empty si_arc.
        // 0 on every cold start, so cold starts stay exempt by principle.
        int si_resume_warmup_ep = 0;

        // --- PRODUCT-REFERENCED MEASUREMENT ADMISSION ---
        // [snapshot: EXEMPT] Per-(STATION index, "SATELLITE|<band digit>") admission
        // record: is the channel currently in the model set, and how many
        // consecutive in-bound epochs has a rejected one accumulated toward its
        // heal.  Keyed per BAND rather than per signal because the response covers
        // both observables of a band and because a receiver fault that displaces a
        // channel by 198 km does not respect RINEX attribute letters.
        //
        // EMPTY unless ctx.knobs.adm_screen, so an OFF (and a measure-only) run
        // allocates nothing.  NOT serialized, and that is load-bearing rather than
        // merely tolerable: the card is NOT fingerprinted precisely so an OFF/ON
        // pair can share ONE snapshot, and persisting an observer's memory would
        // make a forward-only admission policy fingerprint-relevant.  A resumed
        // leg's first pass therefore sees every channel as unknown — which is
        // exactly the case @c adm_resume_pending exists to label, because the arc's
        // STATE was restored (possibly carrying the corrupt seed) even though this
        // observer's memory was not.
        std::map<std::pair<int, std::string>, MeasurementAdmissionArc> adm_arc;
        // This RUN resumed from a snapshot, so each station's first screened epoch
        // labels a first-sighting FAILURE `adm_reject_resume` (a restored corrupt
        // arc) rather than `adm_reject_birth`.  Written UNCONDITIONALLY by a
        // successful warm resume — inert unless the screen is armed, and
        // byte-identical either way, exactly like @c anchor_age_resume_seed.
        //
        // Deliberately NOT a one-shot consumed by the first seam pass.  That is what
        // it was first built as, and it was wrong: the seam screens whichever
        // stations are present AND above the satellite floor, so a station that is
        // absent, thin, or whose satellite the two-station observability gate erased
        // on that epoch would have the flag consumed on its behalf and every later
        // first sighting there would read as a birth.  The response no longer depends
        // on the distinction (the refloat is unconditional), but the EVENT NAME does.
        bool adm_resume_pending = false;
        /// Station indices whose first post-resume screened epoch has already
        /// happened — the per-station half of @c adm_resume_pending.  Bounded by the
        /// station roster; empty and unread on a cold start.
        std::set<int> adm_resume_seen;
        /// "event|STATION|SAT|band" -> the processed-epoch index of its last emitted
        /// event.  Rate-limits only the two ADMISSION streams that CAN repeat (the
        /// INC-3 census and a chronically-bad station's guard event); the INC-1 /
        /// INC-2 events are transitions and never consult it.
        std::map<std::string, long long> adm_event_last;
        // INC-3 per-epoch L1 scalars.  @c adm_ep_tow is the STAMP the epoch-status
        // writer compares against, so an epoch the screen did not run on leaves the
        // columns BLANK instead of repeating stale numbers (the redun_ep_tow
        // pattern — deliberately NOT the ep_dw_engaged one, whose -1 sentinel is
        // never reset and therefore sticks after the first write).
        double adm_ep_tow          = -1.0;
        double ep_adm_sta_worst_z  = 0.0;  ///< MEDIAN over screened stations of each station's worst |z| (m) — the healthy sea.
        double ep_adm_net_worst_z  = 0.0;  ///< network MAXIMUM |z| (m) — the extreme.  Its identity goes to the events.

        // Did the LAST processed epoch move the clock datum?  A clock-pivot handover
        // re-references every satellite phase bias by the pivot's clock (the phase
        // observable ties δtˢ + b_s), so a bridge value harvested before it belongs to
        // the retired frame.  Written where datum_changed is computed (solve glue),
        // read by the next epoch's hygiene pass (bridge drop, reason=datum).
        bool last_datum_changed = false;
        /// Queue 1(b): the constellations whose carried shared state the handover
        /// S-transform RE-EXPRESSED this epoch.  The five RTA-2 handover_affects
        /// predicates release their skip for exactly these constellations — the
        /// prior is valid in the incoming datum, so it must FOLD, which is the
        /// load-bearing half of the card (§8.4 INC-1).  Rebuilt every epoch by
        /// networkHandoverStransform (empty on every non-handover epoch and
        /// whenever the knob is off ⇒ the OFF path is byte-identical).
        std::set<char> sts_consts;
        /// Queue 1(b): the coefficient applied per constellation this epoch
        /// (constellation char -> c0, metres), for the PIVOT event row and traces.
        std::map<char, double> sts_c0;

        // Per-satellite clock sanity monitor (network.clock_monitor): the previous emitted
        // clock value + the continuous-GPS second it was stashed, per satellite, for the
        // adjacent-epoch jump test.  Erased on refloat so the post-reinit clock re-baselines.
        std::map<std::string, std::pair<double, double>> prev_clk;  // sat -> {Δδtˢ (m), seen_s}
        // Two-state oscillator clock state per satellite (network.clock_model): the carried
        // [offset, drift] + 2×2 covariance, persisted across epochs.  Empty / inert unless clk_model_on.
        std::map<std::string, ClockOsc> clock_osc;   // per-sat carried [offset,drift]+cov+t_last (P0-3: t_last drives per-sat prediction/drift-FD across gaps)
        // network.emit_clock_rate: decoupled per-sat rate state used ONLY to derive the emitted C1 by
        // finite-differencing the posterior solved clock (x0=prev solved offset, x1=drift, t_last).  Never
        // feeds a prior or a refloat ⇒ the solution is byte-identical; only clock_c1_m_s changes.  Empty /
        // inert unless emit_clock_rate_on (which is off whenever clk_model_on supplies C1 from clock_osc).
        std::map<std::string, ClockOsc> clock_rate_est;
        // FC-1 (network.fast_clock): the split-cadence fast-clock anchor — per-LOS cached
        // lumps + rates + the emitted c0 at the last EMITTED full solve, rebuilt from
        // scratch at every emit.  Empty / inert unless fast_clock.enable.  Deliberately
        // NOT snapshotted (RTB-7a exemption record): a warm resume re-arms after two fresh
        // emitted full epochs, so a restored anchor can never refer to a frame the resumed
        // leg did not itself emit.  The fast tick mutates NOTHING outside this member —
        // that is the isolation contract that keeps full epochs byte-identical whether or
        // not ticks ran.
        FastClockCarry fast_clock;
        // The broadcast-ephemeris IODE
        // that each satellite's CARRIED clock state (clock_osc.x0 / the D2 clock carry /
        // the clock-monitor jump baseline) is currently referenced to.  Advanced at the
        // eph-cutover RE-LEVEL (before the solve), NOT deferred to the emit site like
        // prev_pivot — the re-level MUTATES the carried state whether or not this epoch
        // emits (the post-emit carry updates are skipped on a withhold/solve-fail, so the
        // shifted values persist), so deferring the advance would re-detect the same roll
        // next epoch and DOUBLE-shift.  Empty / inert unless brdc_rts.  (The plan called
        // this last_emitted_iode_; "referenced-to" is the honest name since it advances on
        // withheld epochs too.)
        std::map<std::string, int> relevel_iode;
        // Sats whose eph rolled but whose PRIOR version was already evicted from
        // the store, so Δ_eph is incomputable — a forced per-sat discontinuity is queued
        // and lands at the sat's next actual emit (present/pending split like
        // ura_retro_sats).  Effectively unreachable in steady state (per-epoch checking
        // makes two rolls between consecutive checks impossible); a defensive belt for a
        // snapshot gap / two-roll corner.
        std::set<std::string> eph_cutover_disc_pending;
        // T3c: sats whose clock_signal_basis_hold IF-pin reference pair RE-LATCHED this epoch —
        // the emitted δtˢ↔dˢ split moved (a genuine per-sat clock step), so force a per-sat disc
        // bump at the next emit (present/pending split like eph_cutover_disc_pending).  Only ever
        // populated under clock_signal_basis_hold && emit_clock_disc ⇒ byte-identical otherwise.
        // (The hold-OFF deterministic re-selection uses the emit-only this_epoch_if_pin_sel /
        // last_if_pin_sel path, NOT this queue — see that comment near held_clock_ref.)
        std::set<std::string> basis_relatch_disc_pending;
        // The code-bias datum's previous-epoch
        // solved values, keyed "<constellation>|<signal>" -> satellite -> value.  The
        // veteran-referenced sum-to-zero RHS and the IF-pin RHS read this at gauge
        // construction; it is cleared + rebuilt from the solved shared vector at the D2
        // harvest (kept epochs only — so a departed satellite is dropped and its return
        // re-seeds IT alone).  NOT snapshotted (a warm resume re-seeds, signalled).
        std::map<std::string, std::map<std::string, double>> code_bias_prev_;
        // The IF-pin reference pair (sat -> {sig1,sig2}) that produced the values
        // in code_bias_prev_ — the SAME kept-epoch lifecycle (rebuilt at the D2 harvest
        // from the scratch below).  A pair CHANGE between kept epochs re-references the
        // emitted clock↔code split by the satellite's inter-channel bias (the band-1
        // C1C↔C1W flip), so the IF-pin RHS re-seeds and the disc bumps even when the new
        // pair's signals both have previous values.  NOT snapshotted.
        std::map<std::string, std::pair<std::string, std::string>> code_bias_ifpin_pair_;
        // This epoch's selected IF-pin pair per satellite (valid pairs only),
        // captured in the main pass and copied into code_bias_ifpin_pair_ at the harvest
        // (kept epochs only — mirrors code_bias_prev_, so a pair recorded on a withheld
        // epoch never advances the comparison baseline).  Cleared each epoch.
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_cb_ifpin;
        // Satellites that RE-SEED this epoch (IF-pin split or a cold sum-to-zero
        // channel).  A per-epoch scratch (cleared each epoch) populated in the main pass and
        // MERGED into cb_reseed_disc_pending only at the emit site — which every whole-epoch
        // withhold path `continue`s before reaching.  So a transient re-seed on a since-
        // withheld epoch (the frame reverts by the next kept epoch) never enters the pending
        // set and cannot fire a bump with no emitted value step (the emit-only discipline
        // this_epoch_if_pin_sel / last_if_pin_sel use), WITHOUT an erase that would destroy a
        // bump legitimately deferred for a satellite re-seeded while pruned from the emission.
        std::set<std::string> this_epoch_cb_reseed;
        // Satellites whose code-bias split has EVER been harvested — a later
        // re-seed of one of these is a RETURN (disc-bump), not a genuine first appearance
        // (seed silently).  Accumulates for the whole process; NOT snapshotted.
        std::set<std::string> code_bias_sat_seen_;
        // A warm resume starts code_bias_prev_/seen EMPTY, but the pre-snapshot
        // product was emitted, so every first post-resume re-seed IS a re-reference the
        // consumer must be told about.  Set true in the resume block; makes re-seeds bump
        // regardless of code_bias_sat_seen_ for the rest of a resumed run (conservative —
        // harmless on a genuinely-new satellite's first emit).  NOT snapshotted.
        bool code_bias_datum_resumed_ = false;
        // Satellites to disc-bump for a time-linked datum RE-SEED (a no-veteran
        // sum-to-zero channel or an IF-pin split forced to the 0 seed → the emitted value
        // stepped).  Fed at the emit site from this_epoch_cb_reseed (kept epochs only), then
        // flushed with the same present/pending split as eph_cutover_disc_pending so the bump
        // lands with the sat present (emit-only, replay-faithful).  Only ever populated under
        // code_datum_time_linked ⇒ empty (no-op, byte-identical) otherwise.
        std::set<std::string> cb_reseed_disc_pending;
        double start_anchor_s = -1.0;                        // first-epoch time anchor for network.start_offset_hours
        // Phase-leveling per-station ambiguity carry (Increment 1): persistent across
        // epochs, keyed by (station index, sat, signal); ORDERED so the inject order
        // (which feeds rank-1 LDLᵀ updates) is byte-identical serial≡parallel.  Inert
        // unless network.phase_leveling.
        std::map<std::tuple<int, std::string, std::string>, LocalCarry> local_carry;
        /// Continuous-GPS-seconds stamp of the PREVIOUS harvest, i.e. the value
        /// @c LocalCarry::last_seen_s carries for an entry stashed one processed
        /// epoch ago.  Written at the end of every harvest; -1 before the first.
        /// Used by the §7.3 lane observatory to require that a pre-event carry is
        /// exactly one epoch old, so a stale level is never differenced against a
        /// one-epoch ΔMW.
        double pl_prev_now_s{-1.0};
        /// RX-CLK-in-block: the previous RX-CLOCK HARVEST's stamp (s).  Deliberately a
        /// second field rather than a read of @c pl_prev_now_s, which has already been
        /// advanced to this epoch by the time the clock harvest runs.  Its only use is
        /// converting a sighting gap into EPOCHS for the per-epoch q.
        double rxclk_prev_now_s{-1.0};
        /// §7.6(d) lane observatory T4-side pre-erase capture: the
        /// about-to-be-erased @c LocalCarry::value_abs (+ its @c last_seen_s
        /// stamp + the erase-cause bit) at the RAIM attribution / consensus
        /// erase sites, keyed like @c local_carry.  MEASURE-ONLY, armed with
        /// the observatory knob.  Lifetime: ONE epoch — filled at SB_RAIM of
        /// epoch t (post-harvest), consumed AND cleared at epoch t+1's harvest
        /// (invariant #1's bound on what a decoupled capture may carry; a
        /// missed epoch drops the capture rather than measuring across an
        /// unequal interval).  NOT snapshotted.  ⚠ Unlike the §7.3 detector
        /// path (whose pre level lives in @c local_carry and is therefore
        /// gauge-current by construction), this is a COPY across one epoch
        /// boundary — the consumer must skip pairs whose (sat,σ) appears in
        /// this epoch's @c eq34_reref set (n_lane_t4_skip_gauge).
        struct LaneT4Pre {
            double value_abs{0.0};
            double stamp_s{-1.0};
            std::uint8_t cause{0};   ///< RelevelEpochState::kCauseT4Attrib / kCauseT4Consens.
        };
        std::map<std::tuple<int, std::string, std::string>, LaneT4Pre> lane_t4_pre;
        /// RF-1 §7.4 (@c network.trace_amb_state): THIS epoch's @c network_amb_state.csv
        /// rows, buffered here and drained serially by @ref networkAmbStateTrace at the
        /// end of the epoch (the @c sd_ewl_trace precedent, so the file order is
        /// thread-count independent).
        ///
        /// A buffer rather than a direct write because the row sources are spread across
        /// the epoch and some of them run AFTER the harvest: the trajectory rows come
        /// from the leveling harvest (SB_LEVELING), while the `erase:` rows come from the
        /// gap prune (also SB_LEVELING, after the harvest refreshed last_seen_s), the T4
        /// attribution and T3 consensus refloats (SB_RAIM) and the phase-anchor carry
        /// re-reference (the datum block).  Cleared by the drain, so an epoch that never
        /// reaches it cannot leak rows into the next one.  Empty on every unarmed run.
        std::vector<AmbStateRow> amb_state_rows;
        /// RF-2 item 1 — THE HELD S-BASIS LEDGER: ambiguity key -> the FROZEN datum
        /// (@ref PhaseBasisEntry).  Written ONCE per anchor tenure by the election and
        /// read by it on every subsequent epoch of that tenure, so the pinned value is a
        /// CONSTANT rather than something re-derived from a carry that moves.  Empty on
        /// every run without network.phase_datum_held_basis.
        std::map<std::string, PhaseBasisEntry> phase_basis;
        /// RF-2 items 3 + 4 — (station idx, ambiguity key) -> the loop epoch of that
        /// arc's LAST screen flag: a slip response (the latched free set), a T4 / T3
        /// refloat, a gross-LOS declaration.  The election's quiet-window gate reads it
        /// to refuse a recently-flagged candidate the basis, and the sticky branch reads
        /// it to DEPOSE a basis holder that has just been flagged — which is item 4's
        /// "the handover happens BEFORE the response acts on that column".  Only ever
        /// written when the held basis is armed.
        std::map<std::pair<int, std::string>, long long> amb_flag_ep;
        /// SIM-ONLY ORACLE slip response (network.cycle_slip.oracle_relevel,
        /// include/slip/NetworkSlipOracle.hpp): the run-scope truth table, the
        /// per-arc application ledger and the run-end counters.  NULL on every
        /// run where the knob is off — the four detector actuation sites take a
        /// `NetworkSlipOracle*` and never reach a call, so the OFF path is
        /// structurally byte-identical (standing invariant #6).  Owns a raw
        /// pointer back to @c local_carry (a sibling member of this struct, so
        /// it outlives the oracle); set at construction.  NOT snapshotted — a
        /// diagnostic, and a resumed leg re-reads the fixture anyway.
        std::unique_ptr<NetworkSlipOracle> slip_oracle;
        /// SIM-ONLY ORACLE AMBIGUITY PIN (`network.inject.events[] kind
        /// "oracle_pin"`, include/filter/NetworkOraclePin.hpp): the run-scope
        /// truth table, the parsed scopes, THIS epoch's gauge-consistent pin
        /// table and the run-end counters.  NULL on every run where no
        /// `oracle_pin` event is armed — the one serial call site and the PASS-B
        /// row both test the pointer first, so the OFF path is structurally
        /// byte-identical (standing invariant #6).  Lives beside
        /// @ref slip_oracle because both are truth-fed diagnostics keyed on
        /// (station index, satellite, signal).  NOT snapshotted — a diagnostic,
        /// and a resumed leg re-reads the fixture anyway.
        std::unique_ptr<NetworkOraclePin> oracle_pin;
        // Per-station ZWD carry (B9): persistent across epochs, keyed by station index
        // (ZWD is one-per-station).  Solve-inert unless network.carry_zwd (every-epoch
        // inject) or a live zwd_seam_ep_left window (post-resume inject); ALSO kept
        // fresh — capture-only, no inject — on any snapshot-armed run (knobs.zwd_capture)
        // so every RTB-7 snapshot serializes Z rows regardless of carry_zwd.
        std::map<int, LocalCarry> zwd_carry;
        /// Seam-scoped ZWD re-seed window (network.zwd_seam_reseed): emitted epochs
        /// REMAINING in the post-resume inject window.  Armed by networkTryWarmResume
        /// when the knob is on and the snapshot restored >= 1 Z row; decremented once
        /// per processed epoch at the serial harvest; 0 = window closed (the retire /
        /// decoupling edge, logged).  Read-only in the parallel produce phase.  NOT
        /// snapshotted: each resume is its own seam and re-arms a full horizon.
        int zwd_seam_ep_left{0};
        /// network.resume_slip_warmup_ep countdown (SPEC_synthetic_minimum_network
        /// §18.10): epochs REMAINING during which the MW/LLI drain acts diagnostic-
        /// only after a warm resume.  Armed by networkTryWarmResume, decremented
        /// once per processed epoch at the PASS-A drain dispatch, never serialized
        /// (each resume is its own seam).  0 on every continuous epoch.
        int resume_slip_warmup_left{0};
        /// network.resume_d5_warmup_ep countdown (RS-2b, SPEC_event_harness §21.6):
        /// PROCESSED epochs REMAINING during which the Card-4 D5 innovation gate acts
        /// diagnostic-only after a warm resume.  Armed by @ref networkTryWarmResume,
        /// decremented once per epoch at the INSTALL seam — the single site the gate
        /// lives at, so a run that never installs a posterior never burns the window —
        /// and never serialized (each resume is its own seam).  0 on every continuous
        /// epoch, which is what makes the OFF path byte-identical.
        int resume_d5_warmup_left{0};
        /// RS-3 (SPEC_event_harness §21.7) first-post-resume-epoch measurement latch.
        /// Set by @ref networkTryWarmResume on EVERY warm resume — armed or not, so an
        /// A/B's control leg reports the same three figures from the same instrument —
        /// and cleared at the emit site in NetworkSolPrep once the counts have been
        /// taken.  Pure telemetry: nothing downstream reads it, so an OFF run is
        /// byte-identical (the @c anchor_age_resume_seed / @c adm_resume_pending shape).
        /// If the first processed epochs are WITHHELD the window simply extends to the
        /// first EMITTED epoch, which is the honest boundary for a consumer-facing
        /// readout — the rerefs accumulate rather than being overwritten.  Never
        /// serialized: each resume is its own seam.
        bool resume_datum_first_ep{false};
        /// RS-4 (SPEC_event_harness §21.9) one-shot PHASE-BIAS SEAM latch.  Set by
        /// @ref networkTryWarmResume beside @ref resume_datum_first_ep on EVERY warm
        /// resume and consumed by the FIRST emit in NetworkEmitStore, which is where the
        /// emitter's restored per-key baselines are still the previous run's values.
        /// Read only under @c knobs.resume_pb_seam_mark, so an unarmed run is
        /// byte-identical.  Never serialized: each resume is its own seam, and a leg
        /// that resumes twice would be two seams.
        bool resume_pb_seam_pending{false};
        /// Per-LOS slant-iono carry (QUEUE 8b): persistent across epochs, keyed
        /// (station index, sat).  Diagonal-only (rx-CB rationale — no shared-border
        /// footprint; the coupling variant is deliberately deferred).  NOT snapshotted:
        /// a warm resume cold-starts it, degrading to one epoch of today's white-iono
        /// behavior — the conservative direction (relevel_pend precedent).  The knob IS
        /// conditionally fingerprinted (solve-affecting; never flip mid-chain).  Inert
        /// unless network.carry_iono.
        std::map<std::pair<int, std::string>, LocalCarry> iono_carry;
        /// I-in-block per-LOS slant-iono carry (@c network.carry_iono_in_block): the same
        /// (station index, sat) key as @c iono_carry above, and DELIBERATELY a separate
        /// map — the two are mutually exclusive vehicles (the loader hard-errors on a
        /// co-arm), and sharing one map would let the QUEUE-8(b) prune / prires / CS-GF
        /// paths, all gated on @c knobs.iono_on, silently act on in-block entries.  Unlike
        /// @c iono_carry these entries carry the F3 bookkeeping (@c blk / @c blk_idx /
        /// @c coupling / @c birth_s), because they are folded as columns of the station's
        /// whitened block exactly like a carried ambiguity.  Value is ABSOLUTE slant delay
        /// in metres at L1 (x_lin + x_r).  NOT snapshotted — a warm resume cold-starts it,
        /// which is one epoch of today's white-iono behaviour (the @c iono_carry
        /// precedent), and a restored entry would carry no block anyway.
        std::map<std::pair<int, std::string>, LocalCarry> iono_blk_carry;
        /// Card GL (docs/SPEC_event_harness.md §24.18): the gross line-of-sight
        /// dispositions, keyed (station index, satellite) — the same key the iono
        /// carries above use, deliberately, because a gross LOS's response reaches
        /// exactly those entries.  Holds BOTH the streaks of every screened line of
        /// sight and the declared subset; the declared subset is what
        /// @ref networkGrossLosPublishScales turns into the assembly's code-row σ
        /// scales.  Entries are pruned on the absent-satellite edge
        /// (@c network.gross_los_max_gap_s), so the map tracks the live LOS population
        /// rather than growing monotonically.  Empty unless @c knobs.gl_on.
        ///
        /// NOT snapshotted and NOT fingerprinted: the state is one screen's short-horizon
        /// evidence (two epochs of streak), not a carried estimate, so a warm resume
        /// re-earns it within @c gross_los_epochs epochs and a snapshot written with the
        /// screen off stays readable with it on.
        std::map<std::pair<int, std::string>, GrossLosState> gl_state;
        /// Card GL: per-(station, satellite) last event sequence, for
        /// @c network.gross_los_event_holdoff_ep.  Empty at the default holdoff 0.
        /// GL-2 (§24.19): satellites currently STOOD DOWN — their per-LOS declarations are
        /// refused and they are routed to the consensus-fault response instead.  Empty
        /// unless @c knobs.gl2_on.  Not snapshotted and not fingerprinted, for the same
        /// reason @ref gl_state is not: it is one epoch of evidence, re-earned instantly.
        std::set<std::string> gl2_sat_down;
        /// GL-2: TOW at which each stood-down satellite's edge fired (the release event's detail).
        std::map<std::string, double> gl2_sat_since;
        /// GL-2: satellites to prune from the NEXT emission — drained by @c networkSolPrep
        /// exactly where the RT0-5 runaway quarantine and the RTA-10 per-constellation
        /// carve-out prune theirs.  Rebuilt every screened epoch, so a released satellite
        /// stops being withheld in the same pass a newly-faulted one starts.  Empty when
        /// @c network.gross_los_sat_wire_withhold is off.
        std::set<std::string> gl2_wire_pending;
        std::map<std::string, long long> gl_event_last;
        /// Card GL: screened-epoch sequence number, the holdoff's clock.
        long long gl_seq{ 0 };
        /// RX-CLK-in-block (@c network.carry_rx_clock_in_block): the carried
        /// receiver-clock-family local columns, keyed (station index, kind) with kind ""
        /// for the PRIMARY receiver clock and the system name for each ISB.  Value is the
        /// ABSOLUTE clock/ISB in metres (x_lin + x_r) as of @c last_seen_s, with the F3
        /// bookkeeping (@c blk / @c blk_idx / @c coupling) every in-block column carries.
        /// NOT snapshotted — a warm resume cold-starts it, which is one epoch of today's
        /// white-clock behaviour, and a restored entry would carry no block anyway.
        std::map<std::pair<int, std::string>, LocalCarry> rx_clk_carry;
        /// RX-CLK-in-block: the DRIFT half of the carried (bias, drift) pair, keyed like
        /// @c rx_clk_carry but populated ONLY for the primary clock (kind "") — an ISB is
        /// a near-constant bias and takes a pure random walk.  Kept in its own map rather
        /// than bolted onto @c LocalCarry because @c LocalCarry is shared machinery used
        /// by five other carries, none of which has a rate.
        ///
        /// ⚠ This state exists because the network producer NEVER writes its solution back
        /// into the station StateManager: @c x_lin(clock) is set once (0, or the latched
        /// cold_clock_seed median) and is constant for the run, so @c x_r carries the
        /// whole clock including its ~0.3 m/s ramp — 9 m per 30 s epoch, ~30 sigma against
        /// a bias-only prior.  The rate is a scalar Kalman state driven by the harvested
        /// increments; it also supplies the one-step PREDICTION whose innovation is the
        /// clock-jump detector.
        struct RxClkRate {
            double rate{0.0};          ///< Estimated drift, m/s (GPS-referenced, same frame as the clock state).
            double rate_var{-1.0};     ///< Its variance, m^2/s^2.  < 0 = never updated ⇒ the injector uses network.carry_rx_clock_rate_var0_m2ps2.
            double prev_abs{0.0};      ///< Previous harvested ABSOLUTE clock (m) — the increment's left end.
            double prev_s{-1.0};       ///< Its time (s).  < 0 = no previous sighting.
            double prev_var{0.0};      ///< Its harvested variance (m^2), for the increment's measurement noise.
            long long n_incr{0};       ///< Increments folded.  0 ⇒ no rate yet ⇒ the jump gate cannot fire (there is no prediction to test).
        };
        std::map<std::pair<int, std::string>, RxClkRate> rx_clk_rate;
        /// CS-GF authority states (QUEUE 8b successor, iono_carry_source =
        /// smoothed_gf): per (station index, sat) carrier-smoothed GF-code
        /// slant-iono, advanced serially in the PASS-A slip scan from the
        /// qualified raw verdicts.  Level-independent evidence — the T4 erase
        /// edge deliberately does NOT touch it (it erases the carry; the next
        /// harvest re-stashes from here).  Self-healing scratch, NOT
        /// snapshotted (the RawSlipHistory rationale).  Bounded at
        /// (n_stations × visible sats) — no pruning needed.
        std::map<std::pair<int, std::string>, IonoCsState> iono_cs;
        // Card 3 (eq34): per-(station index, "sys_RPB_σ") receiver phase-bias carry —
        // LOCAL time-linking of δ_{r,σ} (spec D6 v2 in its minimal local form, no
        // border promotion).  Re-estimating the RPB fresh each epoch injects
        // anti-correlated per-epoch wander into every individual ambiguity float of
        // that (station,σ): DDs cancel it exactly, but the per-key phase-leveling
        // carry does NOT — measured Jul-10 as 10–100× datum-invariant DD-step damage.
        // Read-only during the parallel matrix build (fold), written at the serial
        // post-solve stash, erased for DIRTY receiver vertices at the election (their
        // chain frame stepped) and pruned on gap like local_carry.  Inert unless
        // network.phase_datum_eq34 with phase_datum_eq34_rpb_q_m2s > 0.
        std::map<std::pair<int, std::string>, LocalCarry> rpb_carry;
        /// rx-CB carry: carried receiver code-bias
        /// posteriors, keyed (station index, CB state key).  Harvested OUTSIDE
        /// the carry_warm bracket (own gate — must work on the
        /// carry_invariant/no-leveling lineage).  Inert unless
        /// network.carry_rx_code_bias.
        std::map<std::pair<int, std::string>, LocalCarry> rx_cb_carry;
        // rx-CB carry run counters (armed-only run-end line).
        long long rx_cb_injected_total = 0, rx_cb_innov_skips_total = 0,
                  rx_cb_harvested_total = 0;
        /// RS-9 rider (§21.18), every run the rx-CB carry is armed — trace-class, and
        /// products byte-identical: the PER-STATION fold/withhold tally the pooled
        /// "N folded, M withheld" line hides.  Keyed by station NAME (the teardown has
        /// no station roster), value = {station-epochs with at least one rx-CB carry
        /// DECISION, station-epochs in which every decision was a WITHHOLD}.  A station
        /// whose second number is > 90 % of its first carried NOTHING for the run while
        /// the run-end line still reported it among the "N carried" — the standing
        /// invariant #4 blind spot the RS-7b census had to be written to see (§21.18(b):
        /// the harvest overwrites a withheld entry unconditionally and the prune only
        /// drops > 900 s, so a refused station is refreshed and refused forever).
        /// Written SERIALLY at the post-solve harvest from the produce phase's per-station
        /// StationAssembly tallies, so it is race-free like every counter beside it.
        std::map<std::string, std::pair<long long, long long>> rx_cb_sta_tally;
        /// RS-9 restore (§21.18): the snapshot's RL rows, station roster index -> (CB
        /// state key -> the writer's @c x_lin, metres).  Stashed by
        /// @c networkTryWarmResume beside the RS-6 RC restore and consumed ONCE, by
        /// @c networkApplyRxCbLinRestore, which installs each station's map on its own
        /// @c StateInitializer before the first epoch.  Empty on every other run.
        std::map<int, std::map<std::string, double>> rxcb_lin_restore;
        /// RS-9 per-station out-slots for the two override tallies, sized ONCE by
        /// @c networkApplyRxCbLinRestore and never resized after: each station's
        /// @c StateInitializer holds a raw pointer to ITS OWN element, so the counting
        /// is race-free inside the OpenMP parallel produce phase (distinct objects, no
        /// atomics).  {applied, uncovered}; summed at the run-end readout.
        std::vector<std::pair<long long, long long>> rxcb_lin_sta_counts;
        /// Card LV-2: the information-
        /// weighted MEAN carry innovation, μ = Σλ·innov / Σλ, keyed by receiver
        /// code-bias STATE KEY ("GPS_DCB", "GPS_IFB5", …) — the constellation is in
        /// the key, and the DCB/IFB families are kept apart because their
        /// innovations are different physical quantities.  Computed at the SERIAL
        /// post-solve harvest from the sums the parallel produce phase accumulated,
        /// and consumed LAG-1 by the next epoch's inject — the same race-free split
        /// the carry itself uses (produce reads, serial writes).  Empty until the
        /// first harvest ⇒ epoch 1 injects un-demeaned (μ = 0), one epoch of legacy
        /// behaviour.
        std::map<std::string, double> rx_cb_demean_mu;
        /// THIS epoch's reduction per family, retained only so the caller can
        /// trace it (one `lv2_mu` row per family in network_events.csv — the spec's
        /// "do not create a new stream for one scalar").  Rebuilt every armed epoch.
        struct RxCbDemeanEpoch {
            double mu{0.0};       ///< the applied μ — this epoch's under LV-5, next epoch's under lag-1 LV-2.
            double sum_lam{0.0};  ///< Σλ over the rows of this family.
            int    n_sta{0};      ///< contributing stations.
            bool   fresh{false};  ///< false ⇒ FROZEN (below the thin-mean floor).
            double folded_mu{0.0};  ///< LV-5 diagnostic: the FOLDED set's raw common mode, measured after the fact.
        };
        std::map<std::string, RxCbDemeanEpoch> rx_cb_demean_last;
        // LV-2 run counters (armed-only run-end line).  `mu_frozen` is the
        // invariant-#7 thin-mean event: a mass re-seed / RAIM re-reduce epoch whose
        // folded set is too thin to define the common mode holds μ instead of
        // trusting it.
        long long lv2_mu_epochs = 0, lv2_mu_frozen = 0, lv2_folds_demeaned = 0;
        // LV-4 run counters (armed-only run-end line).  `frame_gate_would_skip` is
        // the same-run A/B: what the RETIRED frame-dependent screen would have
        // withheld on exactly these rows, so the comparison needs no second run.
        long long lv4_step_resets = 0, lv4_step_checks = 0,
                  lv4_frame_gate_would_skip = 0, lv4_scale_standdown = 0;
        double    lv4_max_step_z = 0.0;    ///< largest peer-relative z seen (jump-scale readout).
        double    lv4_max_scale_m = 0.0;   ///< largest robust step σ̂ (m) — how big "normal" got.
        // LV-5 (within-epoch μ) counters.  `resid` is the folded set's raw common
        // mode MINUS the μ that was applied to it — the direct measure of what the
        // correction left behind, and the only honest way to tell a working de-mean
        // from one that merely ran.
        long long lv5_within_epochs = 0, lv5_resid_n = 0;
        double    lv5_resid_sum = 0.0, lv5_max_abs_resid_m = 0.0;
        double    lv2_last_mu_m = 0.0;   ///< most recent μ (any constellation) for the run-end line.
        double    lv2_max_abs_mu_m = 0.0;
        /// THIS epoch's S-consistent-harvest reduction per family, retained so the
        /// caller can trace it (one `s2h_mu` row per family in
        /// network_events.csv, the LV-2 pattern).  Rebuilt every armed epoch.
        struct RxCbHarvestEpoch {
            double mu{0.0};        ///< the subtracted Λ-weighted mean (m).
            double sum_lam{0.0};   ///< Σλ over the voting members.
            int    n{0};           ///< usable family entries.
            int    n_used{0};      ///< votes after the peer screen.
            int    n_screened{0};  ///< screened from the vote (still transformed).
            bool   applied{false}; ///< false ⇒ identity transform (thin/degenerate).
        };
        std::map<std::string, RxCbHarvestEpoch> rx_cb_harvest_last;
        // S2H run counters (armed-only run-end line).  `thin_skips` is the
        // invariant-#7 decoupling edge: an epoch too thin to define the frame
        // applies NOTHING (identity — the set keeps the last applied frame)
        // and is counted, never silently frozen.
        long long s2h_epochs_applied = 0, s2h_thin_skips = 0,
                  s2h_screened_total = 0, s2h_mu_n = 0;
        double    s2h_last_mu_m = 0.0, s2h_max_abs_mu_m = 0.0, s2h_mu_abs_sum = 0.0;
        /// Card LV-1: LAG-1 snapshot of the solved SH-VTEC coefficient vector, per
        /// active shell in @c shLayers order (TECU, in @c SphericalHarmonicIonosphere
        /// ::coeffs() order).  Written SERIALLY post-solve, read by the next epoch's
        /// PARALLEL produce phase to sample the field at each LOS's pierce point —
        /// so the seed frame is one epoch old by construction (the field is
        /// quasi-static; this is the same lag the LV-2 de-mean takes).  Invalid
        /// before the first solve: the reframe then falls back to the a₀₀
        /// climatology PRIOR MEAN, which is the designed level authority.
        std::vector<std::vector<double>> sh_field_lag1;
        long long lv1_field_snapshots = 0;   ///< epochs the lag-1 field was refreshed.
        // B1 URA quality channel carried state (inert unless network.emit_ura).
        double                       ura_sigma0_ewma = 1.0;   ///< network-wide σ̂0 EWMA scaling the emitted URA (one-epoch lag).
        std::map<std::string, int>   ura_refloat_horizon;     ///< sat -> epochs remaining of post-refloat URA inflation.
        // Isolated-arc persistence gate carried state (inert unless network.raim_arc_persist_epochs > 1):
        // (station index, satellite) -> number of CONSECUTIVE epochs this isolated arc has tripped the
        // attribution interaction gate WITHOUT yet being refloated.  Reset to 0 (dropped) on any epoch the
        // arc is not flagged, and cleared the epoch it refloats.  Not part of the warm-resume snapshot — a
        // resume simply re-accrues the streak (a one-time K-epoch delay after resume), never mis-refloats.
        std::map<std::pair<int, std::string>, int> arc_spike_streak;
        // B1 retro-bump (P0-2/P1-3): per-SATELLITE, tier-independent, edge-triggered.
        // ura_retro_sats = sats a post-emit QC tier flagged LAST epoch, applied as a
        // per-sat discontinuity at the sat's NEXT ACTUAL emit (markSatDiscontinuity):
        // a flagged sat absent from an emitted epoch (RT0-5 quarantine, vanish) stays
        // pending here rather than being consumed without landing.  retro_active =
        // sats currently flagged and already bumped, so a sat that stays flagged during
        // recovery is bumped ONCE (on its rising edge) — no per-constellation reset storm.
        std::set<std::string>        ura_retro_sats;
        std::set<std::string>        retro_active;
        // Edge-1 piece 2 (network.mark_refsat_reset_disc, default OFF ⇒ this set is
        // never written ⇒ byte-identical).  Satellites whose REFERENCE-path arc was
        // reset by IF-TD slip QC this epoch, queued for a per-satellite discontinuity
        // mark at the sat's NEXT ACTUAL emit.  Drained by NetworkSolPrep through the
        // SAME mark-and-serialize path as ura_retro_sats — including the RT0-5
        // "mark == land" rule (a sat absent from this epoch's emission stays pending)
        // — so the R record always describes bumps that actually reached the product
        // and replay re-marks exactly the landed sats on exactly those epochs.
        // Rationale for marking at all (the emitter's jump test already catches most
        // ref-path slips): SPEC §25.3 measured excursion onsets as refsat_reset-
        // enriched 5.0x with PB value steps = 0, so the onset-relevant subset
        // re-references with no value step and is invisible to the jump test.
        std::set<std::string>        refsat_mark_sats;
        // Datum-station slip re-reference queues (network.cycle_slip.datum_station_detect,
        // default OFF ⇒ never written ⇒ byte-identical).  A detected slip at the
        // gauge-pinned datum receiver queues the satellite in BOTH sets:
        //   * pb set — consumed (and cleared) at this epoch's stash: HARD skip-stash of
        //     the sat's shared PhaseBias carry, so the next KEPT epoch re-seeds the
        //     emitted bias at the new pivot level (reset-over-repair; the pivot's pin
        //     re-anchors it in one solve).
        //   * amb set — consumed at the FIRST DRAIN AFTER that skip-stash (per-sat
        //     guard: a sat still pending in the pb set is deferred, so a withheld
        //     epoch cannot decouple the two edges): network-wide ambiguity reseed,
        //     every station's integer re-references in the SAME solve the bias jumps.
        // Invariant #7: both edges are evented (datum_slip at detection, datum_reref
        // at application) with run-end counters.  NOT snapshot-serialized — same
        // transient class as refsat_mark_sats above; a warm resume across a pending
        // re-reference drops it (the slip then surfaces to post-fit RAIM instead).
        std::set<std::string>        datum_reref_pb_sats;
        std::set<std::string>        datum_reref_amb_sats;
        /// SPEC_slip_relevel_walk §6: the slip re-level response's per-epoch
        /// state (free-epoch latched set, deferred MW bridge candidates, the
        /// bridged-band set for the harvest-side statistics guard).  Filled by
        /// the PASS-A serial drains, read by the PASS-B inject + the post-solve
        /// harvest, cleared at the next epoch's PASS-A entry.  NOT snapshotted
        /// (one-epoch lifetime; a resumed leg's first epochs degrade to the
        /// pre-fix path, the conservative direction — the same argument as the
        /// pending-re-reference queues above).
        RelevelEpochState            relevel;
        /// Retap piece (relevel_retap_statistical): per-arc consecutive-retap
        /// streak (key → last continuous-GPS-s, streak) — an arc that re-trips
        /// relevel_retap_streak_max times inside the 120 s window falls back to
        /// the erase (a real missed slip the loose prior cannot shake).  And the
        /// one-RAIM-to-next-harvest set of retapped arcs for the statistics
        /// guard (read + cleared by the next harvest's labeling).  Neither is
        /// snapshotted (forward-only response state; a resume degrades to the
        /// erase path, the conservative direction).
        std::map<std::tuple<int, std::string, std::string>, std::pair<double, int>>
                                     relevel_retap_streak;
        std::set<std::tuple<int, std::string, std::string>> relevel_retapped_prev;
        /// Bridge v2 pend store, keyed (station idx, sat, "sig1_sig2") —
        /// PERSISTENT across epochs (the MW integer arrives 1-2 epochs after the
        /// T1 flag), swept for expiry/replace at the top of the MW drain.  NOT
        /// snapshotted: a resume drops outstanding pends and those slips keep
        /// their v1 free re-level — the conservative direction.
        std::map<std::tuple<int, std::string, std::string>, RelevelPend> relevel_pend;
        // RTB-7(b/c): startup quality-ramp state.  The DEGRADED URA floor
        // (nc.warmup_ura_floor_m) applies until ramp_lifted; the lift rule is
        // networkWarmupRampLifted (emitted-epoch horizon AND σ̂0-EWMA update count,
        // the count waived when the EWMA is structurally unavailable).  All inert
        // (never read) while the floor is 0 — byte-identical.  On an RTB-7(a) WARM
        // resume these restore from the snapshot: a warm-resumed producer is NOT
        // degraded and reports grade 0 ("warm").
        size_t ura_ewma_updates     = 0;     ///< σ̂0 EWMA updates received (post-fit RAIM tier).
        bool   ramp_lifted          = false; ///< Warmup URA floor lifted (product grade reached).
        double first_emit_content_s = -1.0;  ///< Content time of the FIRST emit (grade anchor).
        double grade_metric_s       = -1.0;  ///< RTB-7(c) cold-start-to-product-grade (s); −1 before lift / ramp off; 0 = warm resume.

        // --- WL/UWL zone-2 run-latched sets + D5 tag state (decomposition INC-4: the
        // plain-data Tier-1 entities; instances with function-local types —
        // WlInjectPlan/LaneTraceEntry/WlCounters/F6Capture vectors and the lane/
        // layer engines — stay in the run TU until INC-9b) ---
        // (The lane-trace / D5-dump "header written" latches lived here until the two
        // dumps were given the normal trace lifecycle — a NetworkRunTraces-owned
        // stream opened truncating WITH its header in openMwWlDumps.  Nothing writes a
        // header mid-run any more, so the latches are gone.)
        std::set<std::pair<int, std::string>> d5_ewl_stasat;
        std::set<ArcKey> d5_hold_chords;
        std::set<ArcKey> d5_inject_chords_prev, d5_inject_chords_now;
        // WL-3 regroup census (run totals; the equivalence leg reads these).
        long long lane_pair_arcs = 0;           ///< arcs routed into pair groups.
        long long lane_resid_arcs = 0;          ///< primary-band arcs left in residual per-band groups (partner missing/unshared).
        long long lane_pair_tree_mismatch = 0;  ///< pair arcs whose two bands' eq34 tree tags disagreed (band-1's tag governs).
        std::map<std::string, int>                    carry_sat_vertex;  // stable satellite -> vertex id (disjoint high range)
        std::map<std::pair<char, std::string>, int>   carry_group_id;    // (constellation,signal) -> stable group key
        bool carry_glonass_warned = false;   // one-time WARN when a GLONASS ambiguity is excluded (FDMA)
        // UWL-1 §3.1: once-per-run loud-skip warns — a constellation whose cycles enter
        // the EWL machinery without a lane tier (untabled under the armed card, or any
        // non-GPS constellation under the legacy digits) must fail LOUDLY, not as a
        // silent skip_missing_mw pile (the measured Galileo death, SPEC §1 fact 3).
        std::set<char> uwl_warned_cons;
        // WL-3 census visibility (once per run per cons; log-only).
        std::set<char> lane_census_logged;
        // v2 (uwl_family_pairs): the run-persistent admissible pair set per constellation.
        // A pair joins when its joint-census count first reaches uwl_family_min_arcs and
        // stays admitted for the run (LATCHED — membership flicker would refloat the
        // family group's carried chords every flip, the WL-3 micro-pair-group hazard;
        // a latched pair whose channels vanish simply routes no arcs, harmless).
        std::map<char, std::set<std::pair<std::string, std::string>>> lane_admit_pairs;
        std::set<std::pair<char, std::string>> lane_family_logged;   // once-per-pair INFO
        double carry_prev_tow = std::numeric_limits<double>::quiet_NaN();
        long long carry_epochs = 0, carry_born_total = 0, carry_marg_total = 0,
                  carry_merges_total = 0, carry_splits_total = 0, carry_swaps_total = 0,
                  carry_zdim_last = 0, carry_slipkills_total = 0, carry_dropped_total = 0;
        // Arcs the innovation gate flagged in the PREVIOUS epoch, to be
        // treated as slipped THIS epoch (the estimation-side fine-slip re-tap runs a post-solve
        // detection ⇒ it feeds the NEXT epoch's slip funnel).  Persists across epochs (the
        // per-epoch carry_slipped_ambs is rebuilt each epoch).  Empty unless carry_innov_gate.
        std::set<std::pair<int, std::string>> carry_innov_pending;
        long long carry_innov_flags_total = 0, carry_innov_retaps_total = 0;
        long long carry_innov_map_miss = 0;   // D5 flag→ambiguity reverse-map misses (should stay 0)
        // Card 4 D5 v4 (network.carry_invariant_innov_retap_only): the subset of
        // carry_innov_pending whose arc the RAW observables MEASURED as continuous at flag
        // time.  Same tri-state as the v3 corroboration gate and the network_d5_flags.csv
        // `raw_stepped` column — stepped and unmeasurable arcs are deliberately absent, so
        // they keep today's refloat (standing invariant #1).  Drained one epoch later into
        // @ref carry_innov_flat_now, in lockstep with carry_innov_pending.
        std::set<std::pair<int, std::string>> carry_innov_pending_flat;
        // The drained view the DD-prior build reads THIS epoch: a member of this set that
        // is not also in the epoch's RAW funnel subset keeps its carried DD instead of
        // being refloated.  Rebuilt (cleared + filled) at the head of every epoch.
        std::set<std::pair<int, std::string>> carry_innov_flat_now;
        // Carry arcs whose membership kill was suppressed by the above.  Invariant #4's
        // tripwire for the knob: 0 with it armed means nothing ever reached it.  Counted
        // per ARC per buildCarryDdPriors CALL, so a within-epoch RAIM re-solve counts its
        // arcs twice — the same convention as the driver's chords_born / slip_kills.
        long long carry_innov_kill_suppressed = 0;

        // --- SPEC_day129_frame_guards P1-1: per-epoch carry churn -------------
        // The Card-4 counters above are RUN-CUMULATIVE (they mirror
        // carry_driver.counters(), which survives the mid-run carry reset() the install
        // seam's exception handler can fire — it clears the carry, not the run
        // instrument; only restoreCarry, installing a FOREIGN carry, zeroes the
        // tallies), so the run-end log line is the only place the churn is visible.
        // That survival is load-bearing HERE: these mirrors are ASSIGNED from the
        // cumulative totals and then DIFFERENCED against the previous epoch's
        // snapshot, so a counter that restarted at zero mid-run would drive the
        // per-epoch deltas below negative.  The trigger question — "did the
        // re-tap count SPIKE at the collapse epoch?" — needs the per-epoch series.
        // NetworkInstallCarry differences the cumulative totals against the
        // previous epoch's snapshot; the emit site copies the deltas into the
        // epoch-status row.  Pure trace: nothing reads them back.
        long long carry_prev_born = 0, carry_prev_slipkills = 0,
                  carry_prev_innov_flags = 0, carry_prev_innov_retaps = 0;
        long long ep_chord_births = 0, ep_slip_refloats = 0,
                  ep_d5_flags = 0, ep_d5_retaps = 0;
        // R0: the within-epoch
        // RAIM tier's per-epoch observability, parked here between the solve glue
        // (writer) and the epoch-status emit (reader).  σ̂₀_first previously existed
        // ONLY inside a trip event's detail span, so there was no null sample on
        // disk to fit a corrected quantile against (QUEUE item 4's hard blocker);
        // and the within-epoch sat refloats were invisible in the epoch-status row
        // because es.n_refloat_sats counts the clock-COUPLE set, which is empty
        // unless clock_monitor_couple_raim.  Sentinels: < 0 = the tier did not
        // assess this epoch (columns stay BLANK — "unavailable is not 0").
        double      ep_sigma0_first    = -1.0; ///< first-assess σ̂₀ (pre-adaptation)
        int         ep_we_refloat_sats = -1;   ///< whole-sat refloats settled this epoch
        double      ep_we_clk_move_max = -1.0; ///< max |final − primary| clock move (m)
        std::string ep_we_clk_move_sat;        ///< identity of that max (invariant #7)

        // --- A4 withhold-era continuity.  Advanced
        // ONLY at epochs that reach the A4 decision; continuity is the census
        // freshness idiom (`wh_last_loop_ep == n_loop_epochs − 1`), so ANY
        // intervening bypass epoch — datum_failed, gate_withheld, solve_failed,
        // PASS-A-head skip, decimation tick — breaks the era (strictly-consecutive
        // C5/C7; an interrupted era is exited with detail "interrupted" at the next
        // decide).  Deliberately distinct from the flight recorder's ceiling-BREACH
        // era (`networkRhsTripEdge` "sigma0"), which is carve-out-independent —
        // two co-located eras, different edges.
        int       wh_era_len      = 0;  ///< consecutive A4-withheld decides (0 = no era)
        long long wh_last_loop_ep = -1; ///< n_loop_epochs at the last withheld decide

        // --- QUEUE item 4 R2: the declared per-station variance factor's SLOW
        // channel (EWMA over emitted epochs — the seam only runs post-acceptance,
        // so the factor freezes across withholds by construction, documented).
        // Keyed by station index (roster stable).  Not snapshotted: a warm resume
        // re-converges the EWMA over ~τ, the conservative direction.
        std::map<int, double>    staf_ewma;     ///< EWMA of the per-epoch factor
        std::map<int, long long> staf_updates;  ///< EWMA update count (declaration warmup)
        std::set<int>            staf_departed; ///< stations currently past the depart edge
        double                   staf_prev_tow = -1.0; ///< dt source for the EWMA alpha

        // --- PD-safe DD fold INC-0 cross-epoch state.
        //
        // There is DELIBERATELY no `ddfold_ep_tow` stamp here, unlike the redundancy and
        // admission censuses.  Those seams run pre-assembly, so their numbers must be
        // parked somewhere until the epoch-status writer runs; the DD fold's do not — the
        // kernel holds them (@ref NetworkEstimator::ddFoldStats, reset once per epoch by
        // beginEpoch) and BOTH writers read them straight from it.  A stamp here would
        // have been actively wrong twice over: the install seam that would write it runs
        // AFTER networkSolveGlue and only on an ACCEPTED epoch, so every withheld row
        // would blank the columns it was meant to fill; and a stamp written on a
        // chordless epoch reports the default-constructed min_pivot_rel of 1.0 — a
        // perfectly conditioned factorization that was never computed, and indistinguishable
        // from a genuine single-chord block.  (Both caught by the card's own review,
        // 2026-08-03.)  Only the CROSS-epoch ledger below needs to live here.
        /// The blocks currently in the DEGENERATE state, keyed "<group>:<chord0.u>-<chord0.v>".
        /// Diffed epoch-to-epoch so the healthy→degenerate WARN fires on the TRANSITION and
        /// not once per epoch — a 360-epoch spam teaches readers to ignore the line, which is
        /// the failure mode this instrument exists to end.
        std::set<std::string> ddfold_degen_prev;

        /// D5 v2 σ̂₀ CLAMP VISIBILITY: the @c NetworkInvariantCarryDriver::InflationState
        /// tag reported at the previous install, so the state CHANGE is logged once rather
        /// than every epoch (same reason as @ref ddfold_degen_prev).  Empty = nothing seen
        /// yet, so the first install always announces the state it starts in.
        std::string d5_calib_state_prev;

        // --- SPEC_day129_frame_guards P1-2: per-epoch iono-deweight state -----
        // Harvested at the SERIAL NetworkShHealth advance() site (the gate's
        // threading contract forbids reading it from the parallel assembly) and
        // copied into the emit epoch's status row.  n_dw_zero counts stations the
        // gate has driven to w == floor == 0, i.e. whose ENTIRE SH block is
        // dropped (ApplicationNetworkAssembly.cpp:330-331) and whose per-LOS iono
        // is floating — the "deweight release wave" candidate trigger.
        int    ep_dw_engaged = -1;   // -1 = not harvested this epoch (blank in the trace)
        int    ep_dw_zero    = 0;
        double ep_dw_min_w   = 1.0;
        double ep_dw_mean_w  = 1.0;

        // --- SPEC_day129_frame_guards P1-4: measure-only post-fit σ̂0 ---------
        // Filled by networkPostfitSigma0 (filter/NetworkFrameDiag) under
        // network.trace_postfit_sigma0; ep_pf_rows == 0 means "not computed this
        // epoch" and the trace columns stay blank.  Nothing reads these back —
        // the A4 withhold ceiling still uses the within-epoch RAIM's sigma0_adapt.
        double    ep_sigma0_pf       = 0.0;
        double    ep_sigma0_pf_code  = 0.0;
        double    ep_sigma0_pf_phase = 0.0;
        long long ep_pf_rows         = 0;
        long long ep_pf_code_rows    = 0;   ///< …of which CODE rows
        long long ep_pf_phase_rows   = 0;   ///< …of which PHASE rows (residual ≡ 0 under free ambiguities)
        long long pf_epochs_total    = 0;   ///< run-end counter (invariant #4: 0 ⇒ inert)
        bool      pf_empty_logged    = false;  ///< one-shot "armed but pooled nothing" warn latch
        // SPEC_event_harness §24.6 (network.trace_postfit_rows): run-end accounting of
        // the SIGNED per-row dump — rows WRITTEN (code + phase), written rows with no
        // elevation, and DATA rows the pass visited but could attribute to neither
        // family (NOT written).  Invariant #4 needs all of them: "0 written under an
        // ARMED trace" must be visible, and a non-zero unattributed count says which
        // rows the dump does not carry.
        long long pf_rows_written      = 0;
        long long pf_rows_code         = 0;   ///< …of which kind=code
        long long pf_rows_phase        = 0;   ///< …of which kind=phase
        long long pf_rows_no_elev      = 0;   ///< …written with an EMPTY elev_deg (station absent from the modeled set, or no satellite on the row)
        long long pf_rows_unattributed = 0;   ///< data rows attributed to neither family (local-column-only pseudo-observations) — counted, not written
        // SPEC_event_harness §24.10 (network.trace_station_block, card ZP): run-end
        // accounting of the per-station LOCAL-block audit.  Invariant #4 needs all
        // four: "0 rows under an ARMED trace" must be visible, and the two refusal
        // counts say WHICH of the two ways the sweep can find nothing happened —
        // no station reached the audit at all, or the block itself could not be
        // decomposed (a non-finite / empty N_rr).
        long long sb_rows_written     = 0;   ///< station-epoch rows written to network_station_block.csv
        long long sb_epochs_written   = 0;   ///< TRACED epochs that wrote at least one row
        long long sb_stations_skipped = 0;   ///< station-epochs skipped: not in the solve, no usable assembly, or no local block
        long long sb_decomp_failed    = 0;   ///< station-epochs skipped: N_rr unavailable or its eigen-decomposition did not converge
        std::size_t sb_last_emitted   = 0;   ///< n_emitted of the last audited emission (one row-set per EMITTED epoch, never per solve)

        // --- PREFIT census, per-epoch scalars ---------------------------------
        // Filled by
        // networkPrefitCensus under network.trace_prefit_census, copied into the
        // epoch-status row by NetworkStashStatus.  ep_pc_rows == 0 means "not
        // computed this epoch" and the census columns stay BLANK — the same
        // "unavailable is not 0" convention the P1-2/P1-4 columns use.  Indexed
        // by the DATA-class order of NetRowClass (obs_code, obs_phase, obs_other,
        // iono_sh_row), which is also the trace column order.  Nothing reads them
        // back: the census is a tripwire, not an input.
        double    ep_pc_max_s[kNetDataClasses]{};
        double    ep_pc_p99_s[kNetDataClasses]{};
        long long ep_pc_n_gt_10[kNetDataClasses]{};
        long long ep_pc_n_gt_1000[kNetDataClasses]{};
        long long ep_pc_rows         = 0;   ///< DATA rows in this epoch's census (0 ⇒ blank columns)
        /// H-rider (edge-queue §1.6 item 3a): this epoch's pooled robust prefit
        /// σ̂₀ for the H heartbeat (networkPrefitSigma0, computed pre-solve at
        /// the top of networkSolveGlue).  0.0 = unavailable (empty pool, or the
        /// knob is off) — the same sentinel the H grammar already defines.
        double    ep_h_sigma0_census = 0.0;
        long long h_sig0_census_nonzero = 0;   ///< run counter: epochs with a usable census σ̂₀ on the H wire (invariant #4)

        // --- Reduced-RHS class attribution (filter/NetworkRhsAttrib) ----------
        int rhs_attrib_done = 0;   ///< armed target epochs already dumped (caps network.rhs_attrib_n_ep)

        // --- P1-3 spectrum bookkeeping ---------------------------------------
        long long sh_spectrum_epochs      = 0;      ///< traced epochs (also drives the mode-detail stride)
        bool      sh_spectrum_fail_logged = false;  ///< one-shot eigensolve-failure warn latch

        // --- SPEC_day129_frame_guards Phase 2.1: CB-carry event response ------
        // NOT snapshotted (three-place-rule exemption): every member is either a
        // one-epoch handoff (cb_excursion_sats is consumed by the SAME epoch's
        // stash) or run telemetry.  cb_excursion_pending_bump crossing a resume
        // boundary unbumped is the conservative failure: a resumed run re-detects
        // the excursion if it is still present, and a missed bump costs a
        // consumer nothing it was not already living without (the counter is
        // silent by default today).  cb_exc_prev_clk rebuilds from the first
        // post-resume epoch, which correctly suppresses a spurious trip on the
        // resume seam itself (no baseline ⇒ no delta ⇒ no trip).
        std::map<std::string, double> cb_exc_prev_clk;      ///< sat -> previous epoch's solved clock (m)
        std::set<std::string> cb_excursion_sats;            ///< THIS epoch's stash: mark these sats' CB carry inflate_q
        std::set<std::string> cb_excursion_pending_bump;    ///< disc-bump these sats' code biases at the NEXT emit
        long long cb_exc_trips           = 0;   ///< epochs the detector tripped
        long long cb_exc_reinits         = 0;   ///< (sat,epoch) CB carries marked for reinit
        long long cb_exc_bumps           = 0;   ///< code-bias discontinuity bumps landed
        long long cb_exc_nonpivot_would  = 0;   ///< D1 counter: (sat,epoch) NON-pivot sats a per-satellite rule would have tripped
        double    cb_exc_max_median      = 0.0; ///< largest median |Δclock| observed (m) — the trip statistic's run maximum
        double    cb_exc_max_median_tow  = -1.0;///< its epoch

        // --- SPEC_day129_frame_guards Phase 2.2: SH-field health GATE ---------
        // NOT snapshotted (three-place-rule exemption): the gate is EMISSION-ONLY
        // (no carried state changes meaning across it), so a resume simply
        // re-accrues the out-of-band run within sh_gate_min_epochs — one epoch of
        // conservative extra emission at worst, never a wrong-frame fold.  For
        // the same reason the toggle is NOT in the resume fingerprint (the
        // raim / clock_runaway_check class, not the cb_excursion class).
        // Verdict computed post-solve in NetworkSolveGlue (the only pre-emit
        // seam holding x_s); enforced by early-returns in NetworkSolPrep +
        // NetworkEmitStore; disposition stamped by NetworkStashStatus.
        int       sh_gate_oob_run        = 0;     ///< CONSECUTIVE out-of-band epochs (0 = in band)
        bool      sh_gate_engaged        = false; ///< this epoch's emission verdict (true = withhold)
        long long sh_gate_oob_epochs_total = 0;   ///< run total of out-of-band EPOCHS (handoff §4.2: count epochs, not warns)
        long long sh_gate_engagements    = 0;     ///< engage transitions over the run
        double    sh_gate_last_a00       = 0.0;   ///< last computed total a00 (TECU; engage/release log detail)
        double    sh_gate_last_rms       = 0.0;   ///< last computed total-field RMS (TECU)

        // --- T5/Iono-2 SH representativeness-error deviation carry (struct
        // ShDevCarry: filter/NetworkRunTypes.hpp; v2 units = vertical TECU) ---
        std::map<std::string, ShDevCarry> sh_dev_carry;   // key "STATION|SAT"
        // Iono-2 run-end counters (invariant #4 — inert wiring is only visible
        // through them): warm-prior harvests, cold seeds, C3c-funnel slip
        // resets, and no-information harvest skips (V̂ ≥ P_pred + f²).
        long long shdev_warm_total = 0, shdev_cold_total = 0,
                  shdev_slip_resets_total = 0, shdev_nodata_total = 0;
        // Iono-2 v3 de-mean: epochs the global de-mean
        // applied, station-mean removals, and Σ|g| for the run-end mean |g|.
        long long shdev_demean_epochs_total = 0, shdev_demean_sta_total = 0;
        double    shdev_demean_abs_g_sum = 0.0;
        // Iono-2 v3k: D5-only re-taps that KEPT the level (variance re-tap) instead
        // of erasing.  shdev_slip_resets_total then counts actual erases only.
        long long shdev_d5_keeps_total = 0;

        // --- IONO-DEWEIGHT RELEASE HYGIENE ---
        // Every member below stays EMPTY / at its sentinel unless
        // ctx.knobs.dw_hygiene_on, so an OFF run allocates nothing.  None of it is
        // serialized: the gate itself is not snapshotted either (a resumed leg starts
        // every station at w = 1 with no samples), so there is no carried release
        // state to restore — the warm-resume shock is bounded by INC-2's gate-cold
        // trigger instead, which is exactly the spec §6 deferral made explicit.
        //
        // INC-1 ambiguity refloats detected at the release edge (post-solve, SB_T1A),
        // queued for the NEXT epoch's slip funnel.  The funnel `carry_slipped_ambs` is
        // a per-epoch driver local whose load-bearing consumer (buildCarryDdPriors) has
        // already run by then, and a bare markAmbiguityPendingReseed issued at SB_T1A
        // is wiped a few lines later by the leveling harvest's clearPendingReseed — so
        // the response is deferred, exactly as the Card-4 D5 innovation gate defers its
        // own post-solve detections through carry_innov_pending.
        std::set<std::pair<int, std::string>> dw_release_pending;

        // --- RAIM POOL RE-HOST ---------------------
        // (station index, ambiguity key) flagged by the re-hosted post-fit RAIM screen
        // at SB_RAIM, queued for the NEXT epoch's slip funnel — the same deferral as
        // `dw_release_pending` above, for the same reason: `carry_slipped_ambs` is a
        // per-epoch driver local whose load-bearing consumer (buildCarryDdPriors, at
        // SB_DDBUILD) has already run by the time RAIM detects post-solve.
        //
        // A queue rather than the seam's own actions, because neither mutation
        // networkPostFitRaim performs can move the solve:
        //   - `local_carry.erase` drops a phase-leveling carry entry whose only
        //     solve-side consumer is the PASS-B inject, itself under
        //     `!carry_invariant` — and on this lineage that map is empty anyway.
        //   - `markAmbiguityPendingReseed` does reach a live reader (the PASS-B state
        //     seed), but rewriting x_lin is BOOKKEEPING here: the kernel solves
        //     corrections about the linearization point with no write-back, so
        //     b_red = u_s − N_sr N_rr⁻¹ u_r is algebraically unchanged.
        // Only the funnel actuates — it membership-excludes the arc from A_old so its
        // chord re-taps from a diffuse prior.
        //
        // Drained into `carry_slipped_ambs`, deliberately NOT `carry_slipped_raw`:
        // raw means a RAW-OBSERVABLE step, which the deviation-erase classifier and
        // the MW-row slip-skip both read.  A post-fit residual consensus is
        // estimation-side evidence, like the D5 innovation gate, which also drains
        // into the funnel only.
        //
        // Lifetime is bounded to EXACTLY one epoch by an unconditional drain-and-clear at
        // the driver head (standing invariant #1); the drain must stay unconditional even
        // on a datum-change or station-drop epoch, or a stale (station index, key) pair
        // could outlive its arc.  Not serialized — the dw-release precedent — so a warm
        // resume simply starts with an empty queue.
        std::set<std::pair<int, std::string>> raim_refloat_pending;
        /// INC-3 / INC-3b: "STATION|SAT" LOS whose SH<->slant tie row is currently
        /// WITHHELD — because its standardized deviation failed k, or because it has
        /// not yet earned admission.  Rebuilt from scratch for every station the
        /// release seam sweeps (so a set or pruned LOS cannot leave a stale hold
        /// behind) and read by the assembly's tie-row build.
        std::set<std::string> dw_row_held;
        /// INC-3b: "STATION|SAT" LOS whose tie row has been ADMITTED.  A LOS absent
        /// from this set has its row held, which is what makes the guard act on a LOS's
        /// FIRST tie row rather than one epoch after it (a positive admission list
        /// cannot be expressed as a hold list — at first sighting the seam has never
        /// run for that key).  Empty unless INC-3b is armed.
        std::set<std::string> dw_tie_seen;
        /// INC-3b one-shot start/resume SEED latch.  Nothing about this card is
        /// serialized, so on the first epoch of a run (or of a warm-resumed leg) the
        /// admitted set is empty — and holding EVERY tie row for one epoch would leave
        /// the whole SH field unconstrained, a self-inflicted shock far larger than the
        /// coupling event the guard exists to catch.  There is also nothing to guard at
        /// that point: the guard is about a LOS coupling for the FIRST time relative to
        /// an established field, and at start there is no established field.  So the
        /// first seam pass ADMITS every present LOS unscreened and latches this; the
        /// assembly keeps the admission check inert until it is set.  The eq34 card's
        /// `anchor_age_resume_seed` precedent, same reasoning.
        bool dw_tie_seeded = false;
        /// Consecutive epochs a quarantined LOS has gone with NO harvested
        /// deviation, keyed "STATION|SAT".  `networkShDevVtec` skips its harvest for a
        /// station at weight <= 0 and for a LOS with no local column, so such a LOS can
        /// NEVER produce the statistic the screen needs — and a screen that holds on
        /// missing data would withhold its tie row for the whole run (measured: 58
        /// consecutive epochs on CPVG00CPV|G30 before this bound existed).  A guard must
        /// reject on EVIDENCE, not on absence of evidence, so after
        /// `kAdmitNoDevMaxEp` barren epochs the LOS is admitted and counted apart.  A
        /// healthy LOS gets its first harvest in the SAME epoch its row would fold, so
        /// the bound never fails open for one the screen could actually judge.
        std::map<std::string, int> dw_admit_wait;
        /// Last NetworkIonoDeweightGate::advanceCount() the release seam consumed.
        /// The producer skips the whole gate block on a datum-change epoch (there is no
        /// advance(false) caller), leaving the gate's per-call release list holding the
        /// PREVIOUS epoch's contents — without this watermark the seam would re-fire an
        /// old release on every held epoch.  -1 = nothing consumed yet.
        long long dw_gate_adv_seen = -1;

        // --- T1a/T1d SH-field WARN edge state + convergence latch ---
        // T1a: persistent state for the a₀₀ physical-band divergence WARN — start "in band"
        // so the first out-of-band epoch is always reported (transition edge), then throttle.
        bool sh_a00_was_in_band = true;
        // T1d: SH-field convergence latch + previous coefficient vector for the epoch-over-
        // epoch RMS-change stationarity test that (optionally) gates the leveling-carry warmup
        // and/or the one-shot carry refloat.  Empty/false until the first emitted SH epoch.
        bool sh_converged_latch = false;
        Eigen::VectorXd sh_coeff_prev;

        // --- SD EWL Route-B integer decision (increment 2a — filter/NetworkSdEwlAr) ---
        // NOT snapshotted, all three, DELIBERATELY (the RTB-7 three-place rule's
        // exemption record):
        //  * sd_ewl_fix is re-derived from scratch every epoch — it is this epoch's
        //    publication to the PASS-B produce loop, not cross-epoch state;
        //  * sd_ewl_r0 is a DATUM choice that is re-electable from any epoch's data.
        //    Keeping it sticky only avoids a needless re-frame; a fresh election after
        //    a warm resume costs exactly one counted sd_ref_reelected.  Adding it to
        //    the fingerprint/restore/write chain would change the snapshot format for
        //    EVERY run — including ones with this feature off — and invalidate every
        //    stored snapshot, for no estimation benefit;
        //  * sd_ewl_prev exists only to VETO a flipped integer.  Empty after a resume
        //    means the first resumed epoch charges no flip and folds normally, which
        //    is precisely what makes the S2 single-epoch virtual fold non-inert.
        //    Snapshotting it would make a paired A/B build-format-sensitive.
        // Storing a decision here and REUSING it instead of re-deriving would be a
        // zero-q hold in disguise (increment 2c) and would pull 2c's slip-response
        // rules forward — see the invariant #1 discussion in NetworkSdEwlAr.hpp.
        std::map<std::pair<std::size_t, char>, std::map<std::string, SdEwlFix>> sd_ewl_fix;
        std::map<char, std::string> sd_ewl_r0;   ///< constellation -> sticky component reference station name (A2's r0).
        /// (station index, constellation, satellite) -> (integer, the detector's lagged
        /// arc sample count).  The count is what separates a genuine FLIP from a
        /// legitimately-changed integer on a restarted arc: a slip / gap / re-acquisition
        /// resets the MW accumulator, so the count goes DOWN.
        std::map<std::tuple<std::size_t, char, std::string>,
                 std::pair<long long, std::size_t>> sd_ewl_prev;

        // --- RTB-7(a) state-snapshot channel (path resolved at the run-TU site;
        // fingerprint/restore/write live in NetworkRunContext.cpp) ---
        std::filesystem::path snap_path;
        double   last_snapshot_write_s = -1.0;   // product time of the last snapshot write
        uint64_t live_fingerprint      = 0;
        // RTB-7(e) write-side telemetry (invariant #4).  The carry blob is ~99% of the
        // snapshot, so "how big was it and in which shape" is the ONLY readout that shows
        // network.state_snapshot_carry_diagonal actually took effect — a diagonal run
        // otherwise looks exactly like a dense one in every log line and counter.
        long long snap_writes             = 0;      // snapshots written this run
        long long snap_carry_bytes_last   = 0;      // bytes of the last serialized carry blob
        bool      snap_carry_diagonal     = false;  // that blob was diagonal-only (v3)
        // §18.12 coupling-regressor capture (network.state_snapshot_coupling_capture):
        // the QS/QL/QZ payload is ~37x the rest of the file, so "how many rows, how many
        // terms, over how many shared columns" is the only readout that shows the capture
        // took effect — and rows_dropped is a writer-side numerical problem that is
        // otherwise invisible (a row with a non-finite field is omitted WHOLE, never in
        // part: an amputated coefficient vector breaks Card 1e's gauge identity).
        long long snap_cpl_dict_last     = 0;   // §18.12: shared columns in the last snapshot's QS dictionary
        long long snap_cpl_rows_last     = 0;   // §18.12: QL rows in the last snapshot
        long long snap_cpl_zrows_last    = 0;   // §18.12: QZ rows in the last snapshot
        long long snap_cpl_terms_last    = 0;   // §18.12: regressor entries in the last snapshot
        long long snap_cpl_rows_dropped  = 0;   // §18.12: rows OMITTED WHOLE for a non-finite/negative field (run total)
        long long snap_datum_rows_last   = 0;   // RS-3 §21.7: EQ*/CB* datum rows in the last snapshot (0 under an ARMED capture on an eq34/time-linked-CB run is the INERT reading)
        long long snap_emit_rows_last    = 0;   // RS-5 §21.10: ER/PF/PS emit-layer rows in the last snapshot (0 under an ARMED capture with the re-base or the filter running is the INERT reading)
        long long snap_station_rows_last = 0;   // RS-6 §21.13: RC/SD station-layer rows in the last snapshot (0 under an ARMED capture on a run that arms carry_rx_code_bias or the T5 deviation carry is the INERT reading)
        long long snap_canon_rows_last   = 0;   // RS-8 §21.16: CN canonical-signal rows in the last snapshot (0 under an ARMED capture means no station had decided a latch yet — on a network run past epoch 1 that is the INERT reading)
        // RS-8 §21.16 [snapshot: EXEMPT — a resume-response staging buffer, filled by
        // networkTryWarmResume and drained by networkRestoreCanonicalSignals in the same
        // process].  The parsed CN rows, keyed by station ROSTER INDEX, waiting for the
        // driver to hand them the station models (networkTryWarmResume has no `stations`
        // and must not grow one — the restore is the seam right after it).  Empty on
        // every run that does not resume with network.resume_signal_canon_restore armed.
        std::map<int, std::vector<PppModel::CanonicalSignal>> resume_canon_rows;
        long long snap_rxcb_lin_rows_last = 0;  // RS-9 §21.18: RL code-bias linearization-point rows in the last snapshot (0 under an ARMED capture is the INERT reading — no station holds a finite code-bias state, i.e. estimate_ifb is off)
        bool      resume_cpl_census_pending = false;  // §18.12: latch the first-post-resume harvest census + fold split
        std::vector<double> resume_cpl_census_rel;    // §18.12: per-row max|M − M_d| / max|M_d| at the first harvest
        std::vector<double> resume_cpl_census_rhs;    // §18.12: per-row |Δ Σ M·ŝ_prev| (m) at the first harvest

        // --- redundancy / testability census ---
        // [snapshot: EXEMPT] Every field here is diagnostic state of a MEASURE-ONLY
        // instrument.  Restoring it would make a resumed leg's stride phase, event
        // rate-limit memory and unobservable-column edges depend on the writer's
        // history, i.e. it would make a diagnostic fingerprint-relevant — precisely
        // what keeps this card A/B-legal on a shared snapshot.  A resumed run
        // simply re-sweeps on its own phase and re-events each still-unobservable
        // column once, which is the honest behaviour for a fresh observer.
        long long redun_epochs = 0;            ///< census seam passes = the stride PHASE counter and the event-holdoff clock.
        std::set<std::size_t> redun_escalate_pending;   ///< stations the FREE tier flagged this epoch, swept at the NEXT one (one-epoch latch).
        std::map<std::string, long long> redun_event_last;  ///< "station\x1fsat\x1fclass" -> redun_epochs of its last emitted hot-cell event.
        std::map<std::size_t, std::set<std::string>> redun_unobs_active;  ///< station index -> local-column keys currently flagged unobservable (the DECOUPLING-edge set; entries appearing = event, disappearing = counted re-couple).
        // Per-epoch L1 scalars.  redun_ep_tow is the STAMP the epoch-status writer
        // compares against: a non-sweep epoch leaves the columns BLANK instead of
        // repeating a stale sweep's numbers (the "unavailable is not 0" convention).
        double    redun_ep_tow           = -1.0;
        long long ep_redun_rows          = 0;  ///< the DENOMINATOR: data rows swept this epoch.  A raw uncheckable count without it is uninterpretable — 1461 of 3300 and 1461 of 40000 are different networks.
        long long ep_uncheckable_rows    = 0;
        double    ep_uncheckable_max_s   = 0.0;
        long long ep_unobservable_local  = 0;
        bool      redun_cov_warned    = false; ///< one-shot: armed with no solved covariance to read.
        bool      redun_closure_warned = false;///< one-shot: trace(H) exceeded the free-parameter bound.

        // --- Baarda MDB ----------------
        // [snapshot: EXEMPT] — observer state of a MEASURE-ONLY, forward-only
        // instrument, same class as the two censuses above.  Snapshotting an
        // event-holdoff clock would make a diagnostic fingerprint-relevant, which
        // is exactly what keeps this card A/B-legal on a shared snapshot.
        //
        // @c mdb is STAMPED, not driver-cleared: the RAIM pool feeds it in SB_RAIM
        // and the census sweep in SB_FRAMEDIAG, so a feed whose TOW differs from
        // the stamp resets it first.  That is what makes an epoch which fed rows
        // and never reached the flush (a withhold, a failed solve) leak nothing
        // into the next epoch's aggregates.  Empty / unread unless
        // ctx.knobs.mdb_on, so an OFF run allocates nothing.
        NetworkMdbEpoch mdb;                   ///< this epoch's per-(constellation x class) accumulation.
        long long mdb_seq = 0;                 ///< flush passes — the per-cell event-holdoff clock.
        std::map<std::string, long long> mdb_event_last;  ///< "station\x1fsat\x1fclass" -> mdb_seq of its last emitted MDB event.
        bool      mdb_join_warned = false;     ///< one-shot: the uncheckable join disagreed with the census.

        // --- M2 v2 carry variance FACTOR -------
        // [snapshot: EXEMPT, and this one is BLOCKING rather than free] — α² is
        // genuine cross-epoch state that CHANGES THE SOLVE, so leaving it out of
        // the snapshot is not the harmless observer exemption the two censuses
        // below get.  Spec §8 declares it: α² restarting at 1 mid-chain would
        // move the solve across the seam, so v1 is FRESH-CONVERGENCE-ONLY and
        // networkTryWarmResume HARD-ERRORS when a resume meets an armed apply
        // (the fingerprint alone cannot catch it — both legs of an apply-on chain
        // fingerprint identically).  The v2 that would relax it is a pair:
        // NetworkCarryVce::seedRobustFloor for the monitor plus an α² field in
        // the snapshot; until both exist this stays exempt-by-refusal.
        //
        // Inert (unity factor, no allocation beyond the fixed table) unless
        // ctx.knobs.vce_apply_on, so an OFF run is byte-identical.
        NetworkCarryVceApply vce_apply;         ///< per-(family x constellation) α², its clamps, freezes and tallies.
        std::vector<NetworkCarryVceApply::Edge> vce_apply_edges;  ///< the last step's clamp/freeze edges, drained by the emitting seam.
        int  vce_apply_trace_hour = -1;         ///< content-hour bucket of the last apply-trace flush (durability).

        // --- PRIOR-RESIDUAL census --------
        // [snapshot: EXEMPT] — observer state of a MEASURE-ONLY, forward-only
        // instrument.  Restoring an event-holdoff clock would make a diagnostic
        // fingerprint-relevant, which is exactly what keeps this card A/B-legal on
        // a shared snapshot; §7 of the spec pre-declares the σ̂₀ accumulator as
        // deliberately un-snapshotted too (a resumed run restarts it empty and
        // reports RAW FORMAL for the first window, and the output says so per row;
        // NetworkCarryVce::seedRobustFloor is the v2 path if that ever matters).
        // Empty / unread unless ctx.knobs.prires_on, so an OFF run allocates nothing.
        std::map<std::string, long long> prires_event_last;  ///< "family|station|sat|signal" -> census-epoch index of its last emitted PRIORRES event.
        /// Per-epoch L1 scalars.  prires_ep_tow is the STAMP the epoch-status
        /// writer compares against, so an epoch the census did not run leaves the
        /// columns BLANK rather than repeating stale numbers ("unavailable is not
        /// 0" — and a genuine 0-row epoch and an un-run one say different things).
        double    prires_ep_tow      = -1.0;
        long long ep_prires_rows     = 0;   ///< the DENOMINATOR: carried priors censused this epoch.
        long long ep_prires_resp     = 0;   ///< of those, RESPONDED-labelled (the positive control's population).
        long long ep_prires_frame    = 0;   ///< of those, FRAME-labelled (invariant-#7 edges).
        long long ep_prires_reported = 0;   ///< of those, |t| above the TRACE threshold (nothing acted on them).
        double    ep_prires_max_t    = 0.0; ///< the worst |t| this epoch, over every family and label.
        bool      prires_trunc_warned = false; ///< one-shot: the per-epoch identity buffer overflowed (ladders unaffected; the event stream became a sample).
        int       prires_last_flush_hour = -1; ///< content-hour bucket of the last ladder flush (durability: only health_trace flushes on its own).
        /// Constraint-aware global σ̂₀ —
        /// the three epoch-status scalars, stamped by the same emission seam and
        /// gated by the same @ref prires_ep_tow stamp, so an epoch the rider did
        /// not drain leaves them BLANK rather than repeating stale numbers.
        bool      ep_s0g_formed = false;///< σ̂₀²_global was defined this epoch (Σr > 0); false ⇒ the column is blank, never 0.
        double    ep_s0g_sigma0 = 0.0;  ///< σ̂₀_global (the SQRT — directly comparable to the median σ̂₀ it is measured against).
        bool      ep_s0g_tension_def = false; ///< the tension ratio had a data side to divide by.
        double    ep_s0g_tension = 0.0; ///< Σw·ê² / Σw·v² — THE HEADLINE column.
        int       ep_s0g_flags   = 0;   ///< @c NetworkPriorResidual::GlobalFlag bitmask: in-band degradation, never silent.

        // --- BLOCKFORMAL LOS block census ---------
        // [snapshot: EXEMPT] — observer state of a MEASURE-ONLY, forward-only
        // instrument, the same class as the three censuses above.  Restoring the
        // σ̂₀ accumulator or the event-holdoff clock would make a diagnostic
        // fingerprint-relevant and cost this card its A/B legality on a shared
        // snapshot; a resumed leg simply restarts the calibration empty and
        // reports RAW FORMAL for its first window, and every trace row and log
        // line SAYS SO rather than hiding it.
        //
        // The accumulator lives HERE rather than as a run-TU local (the
        // prior-residual census's shape) because its feed site is inside the
        // redundancy-census seam, which takes ctx but not a run-TU reference.
        // Default-constructed with NO families ⇒ enabled() is false ⇒ every entry
        // point is a single bool test, so an unarmed run allocates nothing beyond
        // the empty vectors.
        NetworkBlockBaarda bb{{}, 0.0};        ///< the run accumulator; re-seated from config once at run start.
        std::vector<BlockBaardaHit> bb_hits;   ///< this epoch's reportable blocks, drained by the emission seam.
        std::map<std::string, long long> bb_event_last;  ///< "station\x1fsat" -> census-epoch index of its last emitted BLOCKBAARDA event.
        // NOTE: this card deliberately adds NO network_epoch_status.csv columns,
        // unlike the three censuses above.  Its own network_block_baarda.csv
        // already carries n_blocks / n_reported / max_t PER (family x label) per
        // epoch — strictly more than three pooled scalars could say — so an
        // epoch-status column would be a second copy of a number nobody would
        // read from there.  Carrying per-epoch state nothing consumes is the
        // "live counter over dead wiring" failure the blocks above exist to
        // prevent, so those scalars are not carried at all.

        // --- STATION CONGRUENCE screen -------
        // [snapshot: EXEMPT] — same class and same reasoning as the block census
        // above: observer state of a MEASURE-ONLY, forward-only instrument.  A
        // resumed leg restarts the σ̂₀ calibration empty and reports RAW FORMAL for
        // its first window, and every trace row and log line SAYS SO.
        //
        // ⚠ There IS a cost to that exemption here, and it is worth naming: the
        // per-station δ̂ / MDB samples are not restored either, so a resumed leg's
        // run-median covers only that LEG, not the lineage.  SPEC §7.2 grades a
        // run-median against a perturbed arm — run BOTH arms as one continuous
        // lineage, or the two medians are over different windows.
        //
        // Lives HERE rather than as a run-TU local for the block census's reason:
        // its feed site is inside the redundancy-census seam, which takes ctx but
        // not a run-TU reference.  Default-constructed UNARMED ⇒ enabled() is
        // false ⇒ every entry point is a single bool test.
        NetworkCongruence cg{false};           ///< the run accumulator; re-seated from config once at run start.
        std::vector<CongruenceHit> cg_hits;    ///< this epoch's per-station records, drained by the emission seam.
        std::map<std::string, long long> cg_event_last;  ///< station name -> census-epoch index of its last emitted CONGRUENCE event.

        bool      s0g_approx_warned = false;  ///< one-shot: the data side took the r ≡ 1 fallback.
        bool      s0g_nodata_warned = false;  ///< one-shot: an epoch formed the number with NO data rows at all.

        // --- PREFIT census / fuse / birth check / topology / flight recorder ----
        // [snapshot: EXEMPT] — every
        // member is observer state of a forward-only instrument (the fuse aside,
        // which reads none of it across epochs).  Restoring a presence map, a
        // topology baseline or an event-holdoff clock would make a diagnostic
        // fingerprint-relevant, which is exactly what keeps this card A/B-legal on
        // a shared snapshot.  The resumed behaviour is the honest one for a fresh
        // observer: the first walked epoch SEEDS (no births, no topology deltas —
        // a first sighting is not a change) and every later epoch is judged
        // against it.
        //
        // Empty / unread unless ctx.knobs.pc_walk_on (or, for the event clock,
        // pc_fuse_sigma > 0), so an OFF run allocates nothing.
        std::map<std::string, long long> prefit_event_last;  ///< "event|STATION|SAT|class" -> processed-epoch index of its last emitted event (INC-1 + INC-2 rate limit).
        std::set<std::pair<int, std::string>> prefit_present_prev;  ///< INC-3: (station index, satellite) seen in LAST epoch's walk — absence here on a present LOS IS the birth.
        bool prefit_walk_seeded = false;   ///< INC-3: the presence map holds a real epoch (before that, every LOS would read as born).
        bool prefit_empty_logged = false;  ///< one-shot "armed but classified zero data rows" warn latch.
        /// (station index, satellite) whose worst @c obs_code prefit deviation
        /// exceeded @c network.prefit_event_sigma in the epoch stamped by
        /// @ref prefit_code_flag_ep.  The one channel by which the census — a
        /// PRE-solve instrument that runs LATER in the epoch than the admission
        /// seam — becomes evidence the admission seam's INC-4 vet can act on at the
        /// NEXT epoch (`network.adm_revet_census`).  Card §1 always named the census
        /// as the detection net; this is the wire from it to the response.
        ///
        /// Written from the EXCEEDANCE, deliberately upstream of the event-class mask
        /// and the per-cell holdoff: detection must not depend on whether anyone is
        /// listening, and a trigger throttled by a diagnostic rate limiter would go
        /// quiet exactly when a cell is persistently bad.
        ///
        /// CODE rows only.  A phase row's prefit innovation is arc-specific (it
        /// carries that arc's ambiguity linearization point) and its station median is
        /// not a common mode at all — 608 of 665 phase events on one leg measured as
        /// non-findings — so a phase-driven trigger would wire a false-positive
        /// generator to an admission verdict.  It is the same CODE-only rule the
        /// admission statistic itself obeys.
        ///
        /// Per LINE OF SIGHT, not per band: the census's worst is taken over a
        /// satellite's code rows and there is no row -> band map to resolve it
        /// further.  The flicker card's demotion map made the identical trade for
        /// the identical reason (its deviation 6).  It is the conservative
        /// direction — a flagged satellite refuses whichever of its bands is
        /// currently vetting — and the blast radius is bounded by the vet window.
        std::set<std::pair<int, std::string>> prefit_code_flag;
        /// The `n_loop_epochs` value @ref prefit_code_flag was last written at
        /// (-1 = never).  The vet honours flags from the IMMEDIATELY preceding loop
        /// epoch only; anything older is stale and is counted as an absent
        /// consultation rather than silently believed.  Same staleness discipline as
        /// the `adm_ep_tow` / `redun_ep_tow` column stamps, and deliberately not the
        /// `ep_dw_engaged` pattern whose sentinel is never reset and therefore
        /// sticks.
        long long prefit_code_flag_ep = -1;
        std::map<int, int>    topo_prev_tie;   ///< INC-4: station index -> last epoch's iono_sh_row count (the deweight valve's own edge).
        std::map<int, double> topo_prev_w;     ///< INC-4: station index -> last epoch's SH-constraint deweight weight (the "w 0 -> 0.25" half of the event).
        std::map<std::pair<int, std::string>, unsigned> topo_prev_kind;  ///< INC-4: (station index, satellite) -> last epoch's DATA-class bitmask (band gained/lost, code-only <-> code+phase).
        std::set<std::string> topo_prev_cols;  ///< INC-4: last epoch's shared-column label set (registry deltas).
        bool topo_seeded = false;              ///< INC-4: the baselines above hold a real epoch.

        // --- INC-5 flight recorder --------------------------------------------
        // rhs_trip_eras is the ERA membership, not a fire log: a kind is inserted
        // on its rising edge and erased when its condition clears, so one era
        // trips exactly once however long it lasts.  rhs_trip_pending is the
        // one-epoch handoff from a trigger site to the SB_FRAMEDIAG consumer (the
        // A4 withhold path consumes its own, since that epoch never reaches
        // SB_FRAMEDIAG).
        std::set<std::string> rhs_trip_eras;   ///< QC eras currently ACTIVE (latched, one trip each).
        std::string rhs_trip_pending;          ///< this epoch's trip reason ("" = none); names the dump.
        std::unique_ptr<NetworkAttribFrame> rhs_prev_frame;  ///< the RETAINED previous epoch (null unless retention is on and sized in).
        bool rhs_prev_oversize_logged = false; ///< one-shot: retention disabled for network size.

        // --- datum graph census -------------
        // NOT restored from a state snapshot, deliberately and for the same
        // reason the prefit census's observer state is not: this is forward-only
        // observer state of a MEASURE-ONLY instrument, and persisting it would
        // make a diagnostic fingerprint-relevant — which is exactly what keeps
        // the card A/B-legal on a shared snapshot and legal on a warm resume.
        // Empty / unread unless ctx.knobs.dg_on, so an OFF run allocates nothing.
        //
        // `dg_spec` is a ONE-EPOCH hand-off written by the solve glue and read by
        // the SB_FRAMEDIAG census.  It is cleared at the TOP of every epoch, so
        // an epoch that skips, withholds or throws before the gate can never hand
        // the previous epoch's spectrum to the next census pass — the census
        // would then reconcile this epoch's topology against last epoch's
        // eigenvalues and never say so.
        NetDatumGraphSpectral dg_spec;
        std::map<std::string, long long> dg_event_last;  ///< "kind|identity" -> processed-epoch index of its last DATUMGRAPH event (per-identity rate limit).
        /// `predicted only` identities already reported in the CURRENT era.
        /// Cleared on a datum re-reference — the same boundary SBC re-seeds its
        /// continuity targets on — so the KP-load-bearing roster is reported once
        /// per identity per era rather than once per run (which would make its
        /// stability across the day, spec §6.4, unreadable).
        std::set<std::string> dg_pred_only_era;
    };

    /// @internal Service counters, health-window anchors, and B8 flag-rate
    /// tallies — verbatim relocation.
    struct NetworkServiceCounters {
        /// --- Card RF-PD: the addStation POSITIVE-DEFINITENESS drop, counted.
        /// The drop is a per-station, per-epoch amputation
        /// (ApplicationNetworkAssembly.cpp: `present[i]=false`) that
        /// `network_epoch_status.csv` does NOT report — `n_present` still counts
        /// the station.  Its only prior record was a warn line in a ROTATING log.
        /// These are ALWAYS counted (the increment is free and costs no output);
        /// the run-end readout and the per-rejection `sta_pd_dropped` event rows
        /// are what `network.trace_station_pd_null` arms.
        long long pd_drop_epochs = 0;       ///< epochs whose serial consume loop ran (DENOMINATOR)
        long long pd_drop_total = 0;        ///< station-epochs dropped by the addStation PD rejection
        long long pd_drop_named = 0;        ///< …of those, ones whose null direction was eigen-named (armed only)
        long long pd_drop_pivot_abort = 0;  ///< epochs SUNK because the rejected station was the legacy pivot
        int       pd_drop_max_ep = 0;       ///< largest per-epoch drop count over the run
        double    pd_drop_max_ep_tow = 0.0; ///< …its epoch
        /// Top-column FAMILY census over the named rejections (label prefix:
        /// "AMB" / "IONO" / "CBIAS" / "ZWD" / "CLK" / "RPB" / "ISB" / "other").
        /// The read script does the per-station / persistence analysis off the
        /// event rows; this is the run-end one-liner that says which family
        /// dominates without opening a trace.
        std::map<std::string, long long> pd_drop_family;
        /// The same census done HONESTLY: Σ over rejections of the null
        /// eigenvector's per-family mass Σw² (each rejection contributes 1.0 in
        /// total).  The top-column count above is a function of where the top-5 cut
        /// fell; this is not, and here it matters — the null is spread near-evenly
        /// over a whole band of ambiguities, so the family that ends up ranked first
        /// is close to arbitrary while the MASS split is stable.
        std::map<std::string, double> pd_drop_family_mass;
        /// --- Card RF-PD-fix: the TRUTHFUL in-solve station count + invariant #7's
        /// RE-COUPLING edge.  `network_epoch_status.csv`'s `n_present` is the roster
        /// that reached the consume loop, not the set that entered the arrowhead, so a
        /// PD-dropped (or assembly-dropped) station still reads as present.  These are
        /// what the run-end tail and `network_run_summary.csv` report instead, and they
        /// are ALWAYS accumulated — the `sta_readmitted` EVENT rows ride
        /// `network.trace_topology`, the counters do not.  The per-station return
        /// bookkeeping is here, in the run context, precisely so no seam keeps it in a
        /// function-local (the INC-12 rule).
        struct StationSolveTrack {
            bool      ever_in_solve = false;  ///< distinguishes a RETURN from a first sighting
            long long absent_streak = 0;      ///< consecutive consume epochs out of the solve
        };
        std::map<std::string, StationSolveTrack> sta_solve_track;
        std::vector<int> sta_in_solve_ep;        ///< per consume epoch: stations that entered the arrowhead
        long long sta_in_solve_epochs = 0;       ///< #epochs the above was recorded for (DENOMINATOR)
        int       sta_in_solve_min = -1;         ///< min over the run (−1 = never recorded)
        double    sta_in_solve_min_tow = 0.0;    ///< …its epoch
        long long sta_readmit_events = 0;        ///< stations that re-entered the solve after ≥ 1 absent epoch
        long long sta_readmit_max_gap_ep = 0;    ///< longest absence that ended in a readmission
        // --- Card Q3, the guarded temporal clock anchor.
        // Standing invariant #4: `cta_folded_anchored == 0` on an armed run means the
        // mechanism is INERT no matter how clean the log reads, so every disposition is
        // counted against a denominator.  Trips are split by REASON because the reasons
        // are not interchangeable evidence — a `gap` trip proves the staleness guard
        // works, an `nis` trip proves the detector works, and a run that only ever shows
        // `resume` has tested neither.
        long long cta_epochs = 0;            ///< epochs the anchor seam ran (denominator)
        long long cta_folded_anchored = 0;   ///< (sat,epoch) clock priors folded at the ANCHOR variance
        long long cta_folded_loose = 0;      ///< (sat,epoch) clock priors folded at the LEGACY variance (LOOSE)
        long long cta_floor_hits = 0;        ///< folds where the variance FLOOR bound (the deviation-#1 cap actually binding)
        long long cta_trip_nis = 0;          ///< decouples: |w| over network.clock_anchor_k
        long long cta_trip_abs = 0;          ///< decouples: |ê| over network.clock_anchor_abs_m
        long long cta_trip_gap = 0;          ///< decouples: per-sat elapsed time over network.clock_anchor_max_gap_s
        long long cta_trip_datum = 0;        ///< decouples: a datum change retired this satellite's frame
        long long cta_trip_ext = 0;          ///< decouples: an external tier (clock monitor / RAIM) already refloated this satellite
        long long cta_resolves = 0;          ///< in-epoch re-solves executed (the adapt-and-EMIT half)
        long long cta_resolve_failed = 0;    ///< in-epoch re-solves that threw and were reverted to the anchored solution
        /// Queue 1(b) handover S-transform telemetry (invariant #4 — these are the
        /// card's ON-path counters; transforms applied with sts_rows_retained == 0
        /// means the fold release is mis-wired no matter how clean the log looks).
        /// Card CC (B): the closed-loop consumer's snapshot written beside each network
        /// history copy (`<state_snapshot_path>.consumer.epNNNN`).  Invariant #4: a seed
        /// with `closed_loop.state_snapshot` armed and `cl_snap_written == 0` has NO
        /// consumer warm start to offer a leg, however clean the log reads.
        long long cl_snap_written = 0;       ///< consumer snapshots written with the network copy's tow
        long long cl_snap_skipped = 0;       ///< history epochs where the consumer had not processed that tow (nothing written)
        long long sts_edges = 0;             ///< pivot-switch (constellation,epoch) edges the transform handled
        long long sts_applied = 0;           ///< carried rows whose mean/var the transform re-expressed
        long long sts_rows_retained = 0;     ///< carry rows folded that the baseline skip would have dropped
        long long sts_noop_no_carry = 0;     ///< edges skipped: the incoming pivot had no carried clock level
        long long sts_selfcheck_fail = 0;    ///< §8.4 INC-3: post-transform |x_q| or re-derived c0 above tolerance
        double    sts_max_abs_c0 = 0.0;      ///< largest |c0| applied over the run (m)
        double    sts_last_c0 = 0.0;         ///< the last c0 applied (m)
        /// The MISSING baseline increment 0 named: clock-pivot handovers per
        /// constellation over the run.  Counted whenever pivotHandoverSignalled
        /// raises the edge, transform armed or not, so an OFF run states the
        /// population an ON run is acting on.
        std::map<char, long long> pivot_handovers;
        long long cta_recouples = 0;         ///< re-coupling edges: a LOOSE satellite re-earned the anchor
        long long cta_marks = 0;             ///< per-sat wire discontinuities QUEUED at a decoupling edge
        long long cta_gf_keys = 0;           ///< (sat,epoch) clock priors whose information was gauge-downdated.  0 on an ARMED gauge-free run means the downdate never reached a group of >= 2 carried clocks — the step is inert and the anchor is still constraining the gauge.
        long long cta_gf_inert = 0;          ///< epochs where the gauge-free step was armed but downdated NOTHING (every constellation contributed < 2 carried clocks, or UᵀΛU was singular)
        // Card C-GF's partial-support skip tally is NOT here: it lives run-scoped on the
        // estimator (NetworkEstimator::gaugeFreePartialSkipped) and is read once at
        // teardown, because the withdraw and within-epoch-replay folds run after any
        // per-epoch read point the driver could offer.
        double    cta_max_w = 0.0;           ///< largest |w| seen (any disposition)
        double    cta_max_w_tow = 0.0;       ///< …its epoch
        std::string cta_max_w_sat;           ///< …its satellite
        // --- The guard's own realized DISTRIBUTION (Card Q3 §3, the k derivation).
        // `k` is read off a measured |t| ladder, and the ladder MOVES with q: the
        // denominator √(P⁻ − P⁺) shrinks as the anchor tightens, so a FIXED k means a
        // different number of METRES at every rung.  Without these the only way to
        // re-derive k per rung is a second disarmed run per rung.  Accumulated over
        // ANCHORED rows on the FIRST guard pass only (a later pass re-tests survivors
        // against a moved median and would double-count them).
        //
        // ⚠ The population is truncated ACROSS epochs, not within one: a tripped
        // satellite goes LOOSE and stops contributing until it re-couples, so the
        // upper tail is suppressed by the guard's own action.  For the UN-truncated
        // null run an arm with `clock_anchor_k` beyond the top rung and
        // `clock_anchor_abs_m: 0` — that is what §3 means by "with the guard
        // disarmed", and it is the only reading that calibrates k honestly.
        long long cta_w_rows = 0;            ///< anchored rows contributing to the two ladders below (denominator)
        std::array<long long, 8> cta_w_ge{}; ///< |w| ≥ @ref kCtaWLadder[i] — the dimensionless ladder k is read off
        std::array<long long, 8> cta_e_ge{}; ///< |ê| ≥ @ref kCtaELadder[i] (m) — the ladder the ABSOLUTE backstop's rail is read off
        double    cta_sum_sqrt_s = 0.0;      ///< Σ √S over those rows ⇒ mean √S = the METRES one unit of k buys at this rung
        double    cta_sum_abs_e = 0.0;       ///< Σ |ê| over those rows (m)
        double    cta_sum_w2 = 0.0;          ///< Σ w² ⇒ √(Σw²/n) is the guard's own σ̂₀, a same-statistic cross-check on the census's σ̂₀_clk

        // --- SD EWL Route-B integer decision (increment 2a) run tallies.  Standing
        // invariant #4: on an armed run `sdewl_dd_fixed == 0` means the decision never
        // fired, which is NOT the same as "the integers had no effect", so every
        // rejection path below is counted against a denominator.  The 2b ROW tallies
        // live per-station in NetStation::SdEwlCounters (the row build is parallel).
        long long sdewl_epochs = 0;             ///< epochs the decision seam ran (the denominator for everything else).
        long long sdewl_components = 0;         ///< (constellation, connected component) instances processed.
        long long sdewl_dd_pairs_seen = 0;      ///< (station, satellite) arcs admitted to the integer graph.
        long long sdewl_dd_fix_attempts = 0;    ///< DD roundings attempted along spanning-tree / closure edges.
        long long sdewl_dd_fixed = 0;           ///< integers published usable this epoch — MEASURED plus DATUM values (see sdewl_dd_datum; both are legitimately foldable, but only the measured ones went through a rounding gate).
        long long sdewl_dd_datum = 0;           ///< of those, values assigned as a DATUM rather than measured: the reference station's own satellites, each station's link satellite, and a satellite framed on first sighting.  A datum value carries no rounding decision, so it can never be WRONG — it defines the frame.
        long long sdewl_dd_reject_frac = 0;     ///< rejected: |fractional residue| over sd_ewl.fix_max_frac_cyc.
        long long sdewl_dd_reject_ffr = 0;      ///< rejected: rounding success probability under sd_ewl.fix_min_ffr.
        long long sdewl_dd_flip = 0;            ///< integer disagreed with the previous epoch on a CONTINUOUS arc — counted AND vetoed (invariant #1 response).
        long long sdewl_dd_restart = 0;         ///< arc restarted (lagged count fell): memory replaced, NO flip charged.
        long long sdewl_sd_ref_reelected = 0;   ///< the sticky component reference station r0 changed (re-datums the whole component's SD frame).
        long long sdewl_dd_skip_channel = 0;    ///< (station, satellite) excluded: its MW arc is on a different tracking-channel pair than the network-common election, so B^s would NOT cancel in the DD.

        // --- Adaptive iono q (network.adaptive_iono_q) run tallies.  The feed
        // counter lives per-station (NetStation::iono_var_epochs_fed — PASS A is
        // parallel); these two are bumped in the SERIAL T5v2 aging only.
        long long aiq_boosts_total = 0;         ///< GM deviation innovation boosts applied (multiplier > 1 LOS-epochs).
        double    aiq_max_mult = 1.0;           ///< Largest GF-TD2 multiplier applied over the run.

        // Card 3/4 SLIP-AWARE eq34 election (network.phase_datum_eq34_slip_aware_election).
        // All three are needed to read the mechanism honestly, because two of them are the
        // policy WORKING and the third is the graph refusing it:
        //   * deseeded  — slipped incumbents denied their sticky tree seat;
        //   * demoted   — arcLife rankings pushed below the clean band (inert unless the
        //                 longevity election is armed; counted per COMPARISON, so it is a
        //                 sort-effort number, not a distinct-arc number);
        //   * readmitted— slipped arcs that became tree edges ANYWAY because the component
        //                 had no other route to the vertex.  Those still mass-refloat, and
        //                 a large share here means the topology, not the election, is the
        //                 binding constraint (standing invariant #4: report the failure
        //                 mode, do not let "deseeded > 0" read as "no tree edge slipped").
        long long eq34_slip_deseeded   = 0;
        long long eq34_slip_demoted    = 0;
        long long eq34_slip_readmitted = 0;
        // Card 3 chain-pin AUDIT run tallies (docs/SPEC_synthetic_minimum_network.md §14 T1 (measured record §12.8)),
        // keyed "<cons>|<signal>" (the election's dr_key).  The pipeline a pin can
        // silently leak from has three stages — DECLARED (the election's per-group
        // Σ (n_c + m_c) gauge dimension), FOLDED (foldAmb/foldRpb actually built the
        // 1e10 row; the lambdas early-return on a missing state index or a missing
        // live phase row), and LANDED (the folded row sits on a station
        // est.addStation ACCEPTED — a rejected/dropped station takes every folded
        // pin down with it, the loss mode network.station_pd_pin_aware exists to
        // prevent).  A group whose landed < declared − bridged in ANY epoch ran
        // with an under-constrained datum that reads exactly like a working one
        // (standing invariant #4); the W6+SH n-invariant L2W near-null is the
        // motivating suspect.  Accumulated SERIALLY after PASS B's consume; logged
        // once by NetworkTeardown.  All zero unless network.phase_datum_eq34.
        struct Eq34PinAudit {
            std::map<std::string, long long> declared;         ///< Σ per-epoch gauge dim (n_c+m_c)
            std::map<std::string, long long> bridged;          ///< Σ hygiene INC-2 bridge pins (shared-side)
            std::map<std::string, long long> folded;           ///< pins foldAmb/foldRpb built (all producing stations)
            std::map<std::string, long long> landed;           ///< folded pins on ACCEPTED stations
            std::map<std::string, long long> miss_state;       ///< fold refused: state index absent
            std::map<std::string, long long> miss_row;         ///< fold refused: no live phase row (h2local miss)
            std::map<std::string, long long> dropped_station;  ///< folded pins lost with dropped/rejected stations
            std::map<std::string, int>       short_epochs;     ///< epochs with landed < declared − bridged
            std::map<std::string, int>       worst_short;      ///< max per-epoch shortfall
            int epochs = 0;                                    ///< audited (consumed, non-aborted) epochs
        } eq34_pin_audit;
        // Card 3 SEED-ROUND run tallies.  The
        // card's health assertion is |pin innovation| ≤ λ/2 ALWAYS — max_abs_y
        // above λ_max/2 is a logic error, full stop.  `pins` counts election-side
        // targets (the state existed at election time); `deferred` counts
        // fold-computed targets (state allocated in PASS B — cold start and
        // genuinely-new arcs); `target_steps` counts same-station target changes
        // between epochs (the reref pressure this knob generates; ≈ 0 expected on
        // static receivers).  All zero unless network.phase_datum_eq34_seed_round.
        long long seed_round_pins         = 0;
        long long seed_round_deferred     = 0;
        long long seed_round_frozen       = 0;   ///< INC-1: targets reused verbatim from eq34_prev_target (frame realized once)
        long long seed_round_target_steps = 0;
        double    seed_round_max_abs_y_m  = 0.0;
        // INC-2 re-host tallies.  `framed` = re-hosts whose frame step was
        // computed from the new host's own state and therefore stayed SHIFTABLE (the
        // carried PB prior is S-transformed, not marginalized); `cold` = re-hosts
        // INC-2 could not frame (no allocated state — the fold-deferred path), which
        // keep the legacy unknown-step reref.  `max_step_m` is the largest |frame
        // step| any framed re-host applied: it is bounded by λ/2 ONLY when the new
        // host's float is frame-consistent, so a large value is the signal that the
        // solution at the new host had drifted off the realized frame — read it as a
        // diagnostic of the very runaway this card probes, never as a health bound.
        long long seed_round_rehost_framed  = 0;
        long long seed_round_rehost_cold    = 0;
        double    seed_round_rehost_max_step_m = 0.0;
        // eq34 reattach hygiene run tallies —
        // every field documented on the struct.  Bumped only when
        // ctx.knobs.anchor_hygiene_on, and reported by the run-end block in
        // filter/NetworkTeardown.cpp.
        NetworkAnchorTallies hyg;
        // Flicker/gap signal integrity run
        // tallies — every field documented on the struct.  Bumped only when
        // ctx.knobs.sig_integrity_on, and reported by the run-end block in
        // filter/NetworkTeardown.cpp.  `demoted_rows` is summed there from the
        // per-station NetStation::si_rows_demoted counters (the fold runs inside the
        // parallel produce, so the ROW tally cannot live here).
        NetworkSignalIntegrityTallies sig;
        // Iono-deweight release hygiene run
        // tallies — every field documented on the struct.  Bumped only when
        // ctx.knobs.dw_hygiene_on, and reported by the run-end block in
        // filter/NetworkTeardown.cpp.
        NetworkIonoDwReleaseTallies dwr;
        /// RAIM pool re-host run-end telemetry.  All-zero
        /// unless ctx.knobs.raim_ci_on; the denominators separate "seam never ran" from
        /// "census empty" from "nothing flagged" (standing invariant #4).
        NetworkRaimCiTallies raim_ci;
        /// Card GL run-end telemetry (docs/SPEC_event_harness.md §24.18).  All-zero
        /// unless ctx.knobs.gl_on; the denominators separate "seam never ran" from
        /// "no code row reached it" from "nothing cleared the bar" from "evidence
        /// never persisted" (standing invariant #4).
        NetworkGrossLosTallies gross_los;
        // Product-referenced measurement admission
        // run tallies — every field documented
        // on the struct.  Bumped only when ctx.knobs.adm_on, and reported by the
        // run-end block in filter/NetworkTeardown.cpp.  Deliberately named `adm` and
        // NOT reachable as `ctx.admission`: that name is already the CB-ADM W6
        // code-bias admission census (@ref NetworkBiasAdmission below), a different
        // card entirely, and conflating the two would be an easy and expensive
        // reading mistake.
        NetworkMeasurementAdmissionTallies adm;
        long long sdewl_dd_skip_frame = 0;      ///< (station, satellite) excluded: BIA frame disagrees with the constellation majority.
        long long sdewl_dd_no_link = 0;         ///< (station, satellite) left unfixed: no admissible link satellite reached it from the tree/closure.
        long long sdewl_dd_no_graph = 0;        ///< stations isolated: fewer than two co-visible satellites with any neighbour, so no DD can be formed.

        double prev_tow = -1.0;
        int    prev_week = -1;
        int    iod = 0;
        // Per-run solution-generation id, minted at the FIRST emit from that
        // epoch's GPS time (deterministic under replay — the id is serialized per
        // epoch; different across live restarts, which first-emit later).  0 = unset.
        uint32_t generation_id = 0;
        // Product-time (s) of the last written S contract line (the deferred v2
        // header counts as the first); -1 until the contract writer opens.
        double last_contract_emit_s = -1.0;
        size_t n_emitted = 0, n_skipped = 0, n_gate_withheld = 0, n_runaway_epochs = 0;
        // Operator health-record state (network.health_interval_s > 0).
        double last_health_write_s = -1.0;   // content time of the last health row (s)
        double last_emit_content_s = -1.0;   // content time of the last EMITTED epoch (s)
        int    last_n_present      = 0;      // stations present in the most recent processed epoch
        int    last_n_clock_sats   = 0;      // registered clock sats in the most recent processed epoch (P4b)
        // P4(b): ms_per_epoch_window anchors — wall ms + loop-epoch count as of the
        // PREVIOUS health row, so each row reports the mean over ITS OWN window (the
        // operationally useful "is it slowing down NOW" signal), not a cumulative
        // run mean mislabeled "rolling".
        double health_win_ms_anchor     = 0.0;
        size_t health_win_epochs_anchor = 0;
        size_t n_sigma0_withheld = 0;   // A4/R8: epochs skipped specifically by the σ̂0 withhold ceiling (subset of n_skipped)
        // Withhold-era release policy — both-edge era
        // telemetry per invariant #7.  All zero unless one of the two knobs is armed
        // (the era bookkeeping itself is knob-gated to keep the events CSV
        // byte-identical on OFF paths).
        long long n_wh_era_entries        = 0;  ///< SIGMA0/era_enter events
        long long n_wh_era_exits          = 0;  ///< SIGMA0/era_exit events (a run ending mid-era exits nowhere — max_len covers)
        long long n_wh_era_max_len        = 0;  ///< longest era (withheld decides)
        long long n_wh_escalated_epochs   = 0;  ///< era epochs run at the escalated budget
        long long n_wh_escalate_landed    = 0;  ///< escalated epochs that landed σ̂₀ ≤ ceiling (took the accepted path)
        long long n_wh_escalate_exhausted = 0;  ///< escalated epochs that still withheld
        long long n_wh_free_erases        = 0;  ///< §3b carry entries erased at the withhold branch
        long long n_wh_perconst_exits     = 0;  ///< declared limitation: eras ended by the RTA-10 carve-out publishing (no escalation ran)

        // --- Leveling-carry RETIREMENT census ---
        // Every path that removes a phase-leveling carry entry, counted at its
        // own site, so a run can ATTRIBUTE a fleet-scale carry loss instead of
        // inferring one.  Motivation: §7's promotion refusal rests on the claim
        // that the invariant-#1 gap CLIFF was performing useful therapy by
        // amputating stressed carries.  That claim was never measured — the
        // gap prune reads only `last_seen_s`, i.e. it selects on ELAPSED TIME
        // and on nothing else, and no instrument distinguished it from the four
        // quality-driven retirement paths that run beside it.  These counters
        // are pure telemetry (no solve effect, no CSV row) and are always on.
        // (A) DEPARTURES — entries physically removed and not replaced this epoch.
        //     ⚠ The gap prune runs immediately AFTER the harvest has refreshed
        //     last_seen_s for every arc it stashed, so its candidate set is
        //     disjoint from the arcs observed this epoch: it can only retire
        //     arcs that were NOT harvested (set satellites, absent stations,
        //     harvest failures).  It is therefore a DEPARTURE counter and NOT
        //     the cliff — see (B), which is.
        long long n_cr_gap_prune          = 0;  ///< amb-carry entries dropped by the pl_max_gap_s prune (departures; TIME-selected)
        long long n_cr_gap_prune_epochs   = 0;  ///< epochs on which that prune removed at least one entry
        long long n_cr_gap_prune_max      = 0;  ///< largest single-epoch prune removal
        long long n_cr_gap_prune_max_week = 0;  ///< GPS week of that epoch (attribution handle)
        double    cr_gap_prune_max_tow    = 0.0;///< TOW (s) of that epoch — joins directly to the `tow` column of every trace CSV
        long long n_cr_gap_prune_max_fleet= 0;  ///< carry size immediately BEFORE that removal

        // (B) THE CLIFF — the inject-side stale skip.  This is the mechanism
        //     §1.3 observed as "pl prires rows 753 -> 0": the carried prior is
        //     NOT injected because its gap exceeded the horizon, so the arc
        //     solves FREE and is then re-stashed from one epoch of code.  The
        //     entry is never erased, which is exactly why the prune above
        //     cannot see it.  Counted at the inject predicate itself.
        long long n_cr_inject_stale       = 0;  ///< carried priors skipped for gap > pl_max_gap_s (the mass re-level)
        long long n_cr_inject_stale_epochs= 0;  ///< epochs on which at least one prior was skipped stale
        long long n_cr_inject_stale_max   = 0;  ///< largest single-epoch stale-skip count
        long long n_cr_inject_stale_max_week = 0; ///< GPS week of that epoch
        double    cr_inject_stale_max_tow = 0.0;  ///< TOW (s) of that epoch — THE attribution handle for a cliff
        long long n_cr_inject_stale_max_sta  = 0; ///< distinct stations in that epoch's stale set (separates a fleet-wide era from one station dropping out)

        // RF-1 (docs/refound/RF-1_float_ambiguities_design.md §7.2 / §7.4) — the
        //     AMBIGUITY-VEHICLE telemetry.  Standing invariant #4: these counters ARE
        //     the ON-path evidence, and design §7.4's D3 states the rule in the other
        //     direction too — "a float_kf run with 0 drops is a MISWIRE, not a quiet
        //     run".  ⚠ STEP 1: only @c n_amb_state_rows / @c n_amb_state_events /
        //     @c n_amb_state_epochs can be nonzero (the trace); the drop, erase-by-drop
        //     and S-transform counters are PLACEHOLDERS for steps 4–5 and are reported
        //     at run end so an arm that arms a mechanism and moves none of them is
        //     visible immediately rather than after a grade.
        long long n_amb_drop              = 0;  ///< RF-1 step 4: carried priors DROPPED back to their birth prior (value_abs kept).
        long long n_amb_drop_epochs       = 0;  ///< epochs on which at least one drop happened.
        long long n_amb_drop_relevel_free = 0;  ///< …of which from the detectors' latched free-epoch set (was: skip one inject).
        long long n_amb_drop_stale_gap    = 0;  ///< …of which from the inject-site gap cliff (was: skip one inject).
        long long n_amb_drop_t4           = 0;  ///< …of which from the RAIM prominence-attribution refloat (was: ERASE the entry).
        long long n_amb_drop_t3           = 0;  ///< …of which from the RAIM per-satellite consensus refloat (was: ERASE the entry).
        long long n_amb_drop_idempotent   = 0;  ///< drop calls that found the arc ALREADY at or below birth strength — two detectors on one arc in one epoch, or an arc that never converged.  The age clock still restarts; the variance does not move.
        long long n_amb_birth_floored     = 0;  ///< birth stashes whose var_birth was RAISED by network.amb_float_birth_var_m2 above the arc's own first-stash variance — the ON-path telemetry of the floor (invariant #4: 0 under an armed floor means every arc was already born looser than it).
        long long n_amb_stransform        = 0;  ///< RF-1 step 5: declared anchor S-transformations applied to the carry (one per (sat,signal) family per handover).
        long long n_amb_stransform_cols   = 0;  ///< …carried columns shifted by them.
        long long n_amb_sts_identity      = 0;  ///< …families whose Δ was exactly 0 (already in the new basis — a station re-pinning itself): the transform is the identity, and the family is still OWNED (no erase).
        long long n_amb_sts_refused_cold  = 0;  ///< handovers REFUSED by the transform because the election was COLD (no carried level for the successor ⇒ no new basis to express the family in): the ERASE stands, which is the honest limit of step 5 and RF-2's subject.
        long long n_amb_sts_refused_no_level = 0; ///< …refused because the ledger said continuous but the incoming anchor held no live carry this epoch — a ledger/carry disagreement, counted apart from a cold election.
        // RF-2 — the HELD S-BASIS's ON-path telemetry (invariant #4).
        long long n_pb_basis_held          = 0;  ///< family-epochs re-pinned at their FROZEN basis value (the held property in action).
        long long n_pb_basis_elected       = 0;  ///< new basis entries written — first sightings plus handovers.
        long long n_pb_basis_cold          = 0;  ///< …of which COLD (no level to inherit): the new basis is 0 and the emitted phase bias steps by the outgoing value.
        long long n_pb_basis_refuse_young  = 0;  ///< candidate-family evaluations refused by the arc-AGE gate (never a newborn low arc).
        long long n_pb_basis_refuse_flagged= 0;  ///< …refused by the screen-flag QUIET window (item 3), which is also item 4's depose trigger on the incumbent.
        long long n_pb_basis_rank_changed  = 0;  ///< elections whose ELEVATION rank picked a different station than the coverage order would have, plus incumbents deposed by a flag.
        long long n_pb_basis_declared      = 0;  ///< RF-2 item 2: handovers DECLARED to the consumer (markSatDiscontinuity via the retro-bump queue) — continuous and cold alike.
        long long n_pb_basis_declared_cold = 0;  ///< …of which cold, i.e. the wire's phase bias actually stepped.
        // RF-2b — WHY the datum moved (one counter per deposition cause).
        long long n_pb_basis_refuse_unusable = 0; ///< candidate evaluations refused because their carried level is not usable AS A DATUM (the continuous branch's ledgerVerdict) — the test RF-2's rank was missing.
        long long n_pb_basis_elev_fallback   = 0; ///< elections where NO candidate was ledger-usable and the rank fell back to elevation-only (a COLD pin).
        long long n_pb_basis_depose_flag     = 0; ///< incumbents deposed by a screen flag on THEIR OWN basis column.
        long long n_pb_basis_depose_death    = 0; ///< …deposed because the basis arc no longer has a carry at all.
        long long n_pb_basis_depose_unusable = 0; ///< …deposed because their own carried level stopped passing the ledger verdict.
        long long n_pb_basis_value_drift   = 0;  ///< ⚠ mid-tenure basis-value changes REFUSED — the held property's own alarm.  Must read 0.
        long long n_amb_sts_selfcheck_fail = 0; ///< transforms whose exactness self-check FAILED: the incoming anchor's carried level did not read 0 within kStsExactTolM afterwards.  A transform defect, never noise — WARNed with identities.
        long long n_amb_block_fallback    = 0;  ///< RF-1 step 3 risk #3: station-epochs whose joint block fell back to per-column rows — a VEHICLE CHANGE on one station-epoch, which is why it is counted rather than noted.  0 in this build.
        long long n_amb_state_rows        = 0;  ///< network_amb_state.csv: TRAJECTORY rows written (one per carried (station,sat,signal) per traced epoch).
        long long n_amb_state_events      = 0;  ///< network_amb_state.csv: rows carrying a non-empty `event` (birth / drop / erase / stransform).
        long long n_amb_state_epochs      = 0;  ///< network_amb_state.csv: epochs that wrote at least one row.
        long long n_amb_state_erase_rows  = 0;  ///< network_amb_state.csv: `erase:<cause>` rows — entries captured at the four erase sites BEFORE deletion.

        // F1 (network.phase_leveling_los_scale, SPEC_amb_iono_split_overcount) — the
        //     inject-side per-LOS variance scale's ON-path telemetry (invariant #4).
        //     Counted per station in the PARALLEL produce phase (StationAssembly) and
        //     folded here SERIALLY.  Armed on a multi-band posture with rows = 0 means
        //     the inject never saw two carried bands on one LOS — a MISWIRE, not quiet.
        long long n_pl_los_scale_rows    = 0;             ///< carried ambiguity rows whose variance was multiplied by a B_eff > 1
        long long n_pl_los_scale_hist[4] = {0, 0, 0, 0};  ///< LOS groups (station, sat, epoch) by B_eff = 1 / 2 / 3 / ≥4 injected columns

        // F3 (network.phase_leveling_local_block, SPEC_amb_iono_split_overcount) — the
        //     station-wide jointly-whitened leveling-carry block's ON-path telemetry
        //     (invariant #4).  Counted per station in the PARALLEL produce phase
        //     (StationAssembly) and folded here SERIALLY; the two refloat counters are
        //     written by the SERIAL within-epoch RAIM re-solve directly.  Armed with
        //     blocks = 0 means the inject never built one — a MISWIRE (carry_cpl_cond
        //     off, carry_cpl_joint on, or no station with an injectable carried
        //     column), not a quiet run.  pair_missing ≈ Σ n(n−1)/2 over the stations
        //     on the first post-resume epoch is the EXPECTED one-epoch degrade (the
        //     harvest block is not snapshotted); a steady non-zero rate afterwards is
        //     columns re-harvested at different epochs (blinking satellites).
        //     marg_cols is invariant #7's decoupling edge: every carried entry NOT
        //     admitted to its station's block this epoch (departed / stale / freed /
        //     reseed) — the block simply omits it, never conditions on it.
        long long n_pl_blk_blocks       = 0;             ///< blocks whitened (station-epochs)
        long long n_pl_blk_rows         = 0;             ///< whitened rows emitted
        long long n_pl_blk_marg_cols    = 0;             ///< carried entries NOT injected (marginalized out of their station's block)
        long long n_pl_blk_pair_missing = 0;             ///< (i,j) pairs zeroed: harvested at different epochs / no harvest block
        long long n_pl_blk_col_refused  = 0;             ///< block columns dropped by the scalar-row "never poison" screen
        long long n_pl_blk_fallback     = 0;             ///< station-epochs whose block factorization failed → F1/diagonal rows
        long long n_pl_blk_young_cols   = 0;             ///< F3 age gate: injectable columns kept OUT of their block (too young) and emitted per-column
        // AGE-AWARE young-arc inflate (network.phase_leveling_young_inflate_var_m2) —
        // invariant #4 telemetry.  An armed run with n_yng_cols == 0 is ARMED AND INERT
        // (no carried column was ever stamped young at an inject), not a quiet run; a
        // mean age at the ceiling says the population being inflated is nearly mature
        // and the knob is buying almost nothing.  ⚠ n_yng_cols counts columns for which
        // a positive var_add was COMPUTED and handed to a route — a column the block's
        // scalar "never poison" screen then refused (n_pl_blk_col_refused) is included.
        long long n_pl_blk_yng_scaled = 0;               ///< F3 block columns whose row/column of Σ was actually CONGRUENCE-scaled (read against n_yng_cols: a gap = columns computed young whose block never reached the scaling — refused by the scalar screen, or the station took the per-column route)
        long long n_yng_cols        = 0;                 ///< column-epochs inflated (a positive var_add computed and handed to the block congruence or the diagonal row)
        long long n_yng_skip_age    = 0;                 ///< STAMPED column-epochs refused because age > phase_leveling_young_inflate_max_age_ep (invariant #3: the deferral is measured).  An UNSTAMPED entry (relevel_s == 0 — warm-resume restore / pre-card lineage) is treated as mature and counted in neither.
        double    yng_var_add_sum   = 0.0;               ///< Σ of the var_add actually handed out (m²) — the total prior information given back
        double    yng_age_sum       = 0.0;               ///< Σ of the ages (loop epochs) of the inflated columns; ÷ n_yng_cols = the run's mean inflated age
        long long n_yng_stamp_create = 0;                ///< re-level stamps set at the harvest because the entry was CREATED (first stash, or the re-stash after a T4 erase / gap prune deleted it)
        long long n_yng_stamp_free   = 0;                ///< re-level stamps set at the harvest because the arc was FREED this epoch (the latched relevel.free_ambs set the PASS-B inject consulted)
        long long n_pl_blk_hist[5]      = {0, 0, 0, 0, 0}; ///< block dimension histogram <10 / 10–24 / 25–49 / 50–74 / ≥75
        long long n_pl_blk_max_dim      = 0;             ///< largest block dimension over the run
        // B9-in-block (network.carry_zwd_in_block) — invariant #4/#7 telemetry.  A run with
        // the knob armed and n_pl_blk_zwd_cols == 0 is UNWIRED, not clean; a large
        // n_pl_blk_zwd_fallback_diag says most station-epochs silently degraded to the
        // diagonal B9 row this card exists to replace.
        long long n_pl_blk_zwd_cols     = 0;             ///< ZWD columns ADMITTED into a block build (station-epochs; a block that then failed to factorize still counts — n_pl_blk_zwd_fallback_diag says how many of those ended up on the diagonal row instead)
        long long n_pl_blk_zwd_fallback_diag = 0;        ///< station-epochs whose ZWD fell back to the B9 DIAGONAL row (no block for the station this epoch, or the block's factorization failed)
        long long n_pl_blk_zwd_missing_cross = 0;        ///< (ambiguity, ZWD) pairs zeroed because the harvest cross term was missing (harvest-stamp mismatch / ZWD stashed without a block) — a subset of n_pl_blk_pair_missing
        // I-in-block (network.carry_iono_in_block) — invariant #4/#7 telemetry, the same
        // discipline as the ZWD split above.  An armed run with n_pl_blk_iono_cols == 0 is
        // UNWIRED, not clean.  ⚠ n_pl_blk_iono_not_folded is NOT a "degraded to the old
        // row" count: this card has no diagonal iono row (that IS the falsified QUEUE-8(b)
        // vehicle), so those columns carried NOTHING that station-epoch.
        long long n_pl_blk_iono_cols    = 0;             ///< per-LOS iono columns ADMITTED into a block build (a block that then failed to factorize still counts — n_pl_blk_iono_not_folded says how many of those did not act)
        long long n_pl_blk_iono_not_folded = 0;          ///< iono columns pushed but NOT folded (non-SPD block / no block): their carry did not act this station-epoch, and there is deliberately no diagonal fallback
        long long n_pl_blk_iono_missing_cross = 0;       ///< (iono, other) pairs zeroed because the harvest cross term was missing (harvest-stamp mismatch / entry stashed without a block) — a subset of n_pl_blk_pair_missing
        long long n_iono_blk_erased_t4  = 0;             ///< in-block iono carries erased by a T4/consensus arc refloat (invariant #1: the T4 statistic reads level+iono error as a sum, so the LOS re-draws both)
        long long n_iono_blk_gap_drop   = 0;             ///< in-block iono carries dropped at the harvest prune for exceeding carry_iono_in_block_max_gap_s without a sighting (the absent-satellite / eclipse edge)
        long long n_raim_pool_iono_rows = 0;             ///< post-fit RAIM pool admissions (station-epochs × rows) whose admitted row IS one of the iono columns' whitened block rows — the §7 item 4 interaction MEASURED for the I half, exactly as n_raim_pool_zwd_rows measures it for the ρ half.  Counted only; the pool rule is unchanged by this card.
        // RX-CLK-in-block (network.carry_rx_clock_in_block) — invariant #4/#7 telemetry,
        // the same discipline as the ZWD and iono splits.  An armed run with
        // n_pl_blk_rxclk_cols == 0 is UNWIRED, not clean.  ⚠ n_pl_blk_rxclk_not_folded is
        // NOT a "degraded to the old row" count: this card has no diagonal clock row (a
        // diagonal prior on the most station-COMMON direction of all is exactly the
        // over-count that falsified the diagonal ZWD carry), so those columns carried
        // NOTHING that station-epoch.
        long long n_pl_blk_rxclk_cols   = 0;             ///< receiver-clock-family columns ADMITTED into a block build (a block that then failed to factorize still counts — n_pl_blk_rxclk_not_folded says how many did not act)
        long long n_pl_blk_rxclk_isb_cols = 0;           ///< …of which are ISB columns.  0 is NOT an alarm (a GPS-only / BSSD posture allocates no per-system time-offset state).
        long long n_pl_blk_rxclk_not_folded = 0;         ///< clock-family columns pushed but NOT folded (non-SPD block): their carry did not act, and there is deliberately no diagonal fallback
        long long n_pl_blk_rxclk_missing_cross = 0;      ///< (clock-family, other) pairs zeroed because the harvest cross term was missing (harvest-stamp mismatch / entry stashed without a block) — a subset of n_pl_blk_pair_missing
        long long n_rxclk_carry_refused = 0;             ///< clock carries ERASED by the jump gate (network.carry_rx_clock_jump_k) at harvest: |one-step prediction innovation| > k*sigma ⇒ the receiver (or the solve) stepped, so both the bias and the drift are dropped and re-born cold.  Invariant #1's wired response for a quantity that has no other jump detector anywhere on the network path.
        long long n_rxclk_carry_gap_drop = 0;            ///< clock-family carries dropped at the harvest prune for exceeding carry_rx_clock_max_gap_s without a sighting (the absent-station edge)
        // --- the jump gate's OWN scale, so a leg can say what it would have caught ---
        // The gate is |innovation| > k*sqrt(var_pred), and var_pred is per station-epoch
        // (harvested variance + drift extrapolation + q), so "the gate" is a DISTRIBUTION,
        // not a constant.  Without this census a report can only quote k; with it, it can
        // quote the metres an event has to exceed.  Armed-only (the whole rx-clock harvest
        // block is inside `if (rxclk_blk_on)`), so an OFF run moves none of it.
        long long n_rxclk_gate_tests = 0;                ///< station-epochs the jump gate actually tested (primary clock only; an ISB has no rate)
        double    sum_rxclk_gate_m = 0.0;                ///< Σ k*sqrt(var_pred) over those tests, metres
        double    min_rxclk_gate_m = 0.0;                ///< smallest k*sqrt(var_pred) seen (0 ⇒ never tested)
        double    max_rxclk_gate_m = 0.0;                ///< largest  k*sqrt(var_pred) seen (0 ⇒ never tested)
        double    max_rxclk_innov_m = 0.0;               ///< largest |innovation| seen, metres — the loudest step the gate DID see
        long long n_raim_pool_rxclk_rows = 0;            ///< post-fit RAIM pool admissions (station-epochs x rows) whose admitted row IS one of the clock-family columns' whitened block rows — the SPEC_amb_iono_split_overcount §7 item 4 interaction MEASURED for the rho half, exactly as n_raim_pool_zwd_rows / n_raim_pool_iono_rows measure it for theirs.  Counted only; the pool rule is unchanged by this card.
        long long n_raim_pool_zwd_rows  = 0;             ///< post-fit RAIM pool admissions (station-epochs) whose admitted row IS the ZWD's whitened block row — the §7 item 4 interaction, MEASURED (the pool admits by |A_r(row, amb_col)| > 0.5 and a ZWD block row mixes into ambiguity columns through L⁻¹).  Counted only; the pool rule is unchanged by this card.
        long long n_pl_blk_refloat_rewhiten = 0;         ///< within-epoch RAIM refloats that re-whitened a block over its survivors (invariant #7's re-coupling edge, counted)
        long long n_pl_blk_refloat_fail = 0;             ///< … whose re-whiten was non-SPD: the block's rows were dropped for that re-solve (safe degrade — the refloated arcs re-float from data)

        // (C) ONE-SOLVE REFLOATS — the entry is erased AFTER the harvest, so the
        //     arc refloats on the NEXT epoch.  Quality-selected.
        long long n_cr_sh_conv_flush      = 0;  ///< WHOLE-FLEET one-shot flush at SH convergence (pl_refloat_on_conv) — the largest single removal in the producer.  Family (C), NOT (A): the arcs are still tracked and the next harvest re-stashes them, so it costs one solve, not a departure.
        long long n_cr_sh_conv_flushes    = 0;  ///< how many times that flush fired (expected 1)
        long long n_cr_raim_attrib        = 0;  ///< T4 ATTRIBUTION refloat (live only when network.raim_attribution is armed)
        long long n_cr_raim_consensus     = 0;  ///< T3 CONSENSUS refloat (live only when raim_attribution is NOT armed — the two are a run-constant either/or, never a split)
        long long n_cr_gross_los          = 0;  ///< Card GL: level entries erased by a GROSS LINE-OF-SIGHT declaration (docs/SPEC_event_harness.md §24.18).  Family (C) like the T4 erase — the arc is still tracked and its next harvest re-stashes it.  Zero unless network.gross_los_screen.

        // (D) ONE-SOLVE FRAME SKIPS — the frame stepped, so the carried value is
        //     stale in the OLD gauge; the same epoch's harvest re-stashes any
        //     still-tracked arc, so the cost is one solve of continuity.
        long long n_cr_eq34_reref         = 0;  ///< eq34 D4 chain-datum re-reference
        long long n_cr_phase_datum_reref  = 0;  ///< phase-datum family re-reference

        // Siblings on the SAME gap predicate, different carries (departures).
        long long n_cr_zwd_gap_prune      = 0;  ///< ZWD carry entries dropped by the pl_max_gap_s prune
        long long n_cr_rpb_gap_prune      = 0;  ///< receiver-PB carry entries dropped by the pl_max_gap_s prune

        // --- Seam-scoped ZWD re-seed (network.zwd_seam_reseed) ------------------
        // The ON-path telemetry (standing invariant #4): 0 folds under an armed
        // knob means the window never armed (no warm resume / no restored Z rows)
        // or every fold was skipped — check the resume log line, not just this.
        long long n_zwd_seam_folds        = 0;  ///< station-fold count inside the post-resume window
        long long n_zwd_seam_epochs       = 0;  ///< epochs inside the window with >= 1 fold
        // Resume FRAME restore (network.resume_frame_restore, §18.10) — the re-coupling
        // edge's identities: what the seam reconnected, so a 0 under an armed knob
        // reads as INERT (standing invariant #4).
        long long n_resume_frame_anchors     = 0;  ///< phase-anchor incumbents restored
        long long n_resume_frame_targets     = 0;  ///< sticky pin targets restored
        long long n_resume_frame_cta         = 0;  ///< clock-anchor per-sat states restored
        long long n_resume_frame_cta_anchored = 0; ///< ...of which ANCHORED
        long long n_resume_frame_clk_keys    = 0;  ///< CLK-R held-frame per-sat keys restored
        long long n_resume_frame_ledger      = 0;  ///< handover-ledger entries primed from restored leveling rows
        // RS-3 resume DATUM restore (network.resume_datum_restore, SPEC_event_harness
        // §21.7) — the re-coupling edge's identities for the two datum memories, and the
        // first-post-resume-epoch event counts that say whether the datum actually
        // CONTINUED.  0 restored under an armed knob = INERT (invariant #4); the three
        // first-epoch figures are counted on EVERY warm resume, armed or not, so the
        // control leg of an A/B reports them from the same instrument.
        long long n_resume_datum_eq34_rx      = 0;  ///< sticky datum receivers restored (EQD)
        long long n_resume_datum_eq34_edges   = 0;  ///< receiver parent edges restored (EQP)
        long long n_resume_datum_sat_roots    = 0;  ///< satellite component roots restored (EQS)
        long long n_resume_datum_prev_targets = 0;  ///< last-epoch pin targets restored (EQT)
        long long n_resume_datum_prev_el      = 0;  ///< longevity elevations restored (EQE)
        long long n_resume_datum_retained     = 0;  ///< gap-retained tree edges restored (EQR)
        long long n_resume_datum_cb_prev      = 0;  ///< code-bias previous values restored (CBP)
        long long n_resume_datum_cb_seen      = 0;  ///< code-bias ever-seen satellites restored (CBS)
        long long n_resume_datum_cb_pair      = 0;  ///< code-bias IF-pin reference pairs restored (CBI) — 0 here with CBP > 0 means the IF-pin half re-seeds anyway
        long long n_resume_datum_first_reref    = 0; ///< PHASE_DATUM/eq34_reref (sat,σ) at the first post-resume epoch
        long long n_resume_datum_first_reattach = 0; ///< PHASE_DATUM/eq34_reattach (tree-edge handovers) there
        long long n_resume_datum_first_cb_disc  = 0; ///< CB_DATUM/reseed_disc_landed satellites there
        long long n_pb_seam_keys_tested = 0; ///< RS-4: phase-bias keys the first post-resume emit compared with a restored baseline (the jump test's own population)
        long long n_pb_seam_keys_over   = 0; ///< RS-4: of those, keys beyond discThresholdM
        long long n_pb_seam_marks       = 0; ///< RS-4: constellations markDatumChange'd by the seam rule (0 with the knob off, and 0 on a cold start)
        // RS-5 resume EMIT-LAYER restore (network.resume_emit_restore, SPEC_event_harness
        // §21.10) — the re-coupling edge's identities for the two emit engines.  0 under an
        // armed knob = INERT (standing invariant #4): the snapshot carried no ER / PF / PS
        // rows (a writer without network.state_snapshot_emit_capture) or no warm resume
        // happened, and the run-end line says which.
        long long n_resume_emit_rebase_keys = 0; ///< emission-frame keys restored into NetworkEmitRebase (ER)
        long long n_resume_emit_filter_keys = 0; ///< per-key posteriors restored into NetworkPbEmitFilter (PF)
        long long n_resume_emit_step_accums = 0; ///< per-constellation step-noise accumulators restored (PS)
        // RS-6 resume STATION-LAYER restore (network.resume_station_restore,
        // SPEC_event_harness §21.13) — the re-coupling edge's identities for the two
        // station memories.  0 under an armed knob = INERT (standing invariant #4): the
        // snapshot carried no RC / SD rows (a writer without
        // network.state_snapshot_station_capture), the run arms neither carry, a
        // sub-switch withheld that half, or no warm resume happened; the run-end line
        // says which.
        long long n_resume_station_rxcb  = 0; ///< rx-CB carry entries restored (RC)
        long long n_resume_station_shdev = 0; ///< T5 per-LOS deviation carry entries restored (SD)
        // RS-8 resume CANONICAL-SIGNAL restore (network.resume_signal_canon_restore,
        // SPEC_event_harness §21.16).  0 stations under an armed knob = INERT (standing
        // invariant #4): the snapshot carried no CN rows (a writer without
        // network.state_snapshot_signal_capture) or no warm resume happened, and the
        // run-end line says which.  The DISAGREE count is the defect's own instrument —
        // it is the number of restored entries this run's first epoch would have latched
        // DIFFERENTLY, i.e. exactly the flips the card prevents, so a nonzero value is a
        // success reading and a zero one says the seam epoch happened to agree.
        long long n_resume_canon_stations = 0; ///< stations whose canonical latch was restored (CN)
        long long n_resume_canon_entries  = 0; ///< canonical entries restored across those stations
        long long n_resume_canon_audited  = 0; ///< restored entries the first post-resume epoch's tally could be compared against
        long long n_resume_canon_disagree = 0; ///< ...of those, the ones this run would have decided differently
        // RS-9 resume rx-CB LINEARIZATION-POINT restore (network.resume_rxcb_lin_restore,
        // SPEC_event_harness §21.18) — the re-coupling edge's identities for the FRAME the
        // rx-CB inject gate judges the restored carry against.  0 entries under an armed
        // knob = INERT (invariant #4): the snapshot carried no RL rows (a writer without
        // network.state_snapshot_rxcb_lin_capture), or no warm resume happened.
        //
        // `_uncovered` is scoped to the keys the RL rows CARRY a reference for, which — the
        // rows pairing 1:1 with the RC rows — is exactly the set that has an rx-CB carry,
        // i.e. exactly the set whose x_lin the inject gate ever reads.  A code-bias family
        // with no carry (GPS_IFB5 beside the carried GPS_DCB on a GPS L1/L2/L5 station)
        // takes today's one-epoch seed and is NOT a miss, so it is not counted.  A NONZERO
        // `_uncovered` therefore means a CARRIED key whose restored reference could not be
        // applied — a non-finite value, or a state index this run does not hold — which is
        // a defect, not a scope note.
        long long n_resume_rxcb_lin_stations  = 0; ///< stations handed an RL override map
        long long n_resume_rxcb_lin_entries   = 0; ///< RL entries installed across them
        long long n_resume_rxcb_lin_uncovered = 0; ///< CARRIED code-bias keys whose restored reference could NOT be applied (expected 0)
        long long n_resume_slip_warmup_epochs = 0; ///< epochs the MW/LLI drain ran diagnostic-only post-resume
        long long n_resume_d5_warmup_epochs = 0;   ///< RS-2b: epochs the Card-4 D5 innovation gate ran diagnostic-only post-resume
        // Card 1 coupling-regressor restore (network.resume_coupling_restore, §18.12) —
        // BOTH edges of the seam (standing invariant #7).  rows/zrows/terms/dict are the
        // RE-COUPLING edge's identities; seed_reinit is the DECOUPLING edge (columns the
        // write epoch was re-drawing, diverted on purpose); seam_present/seam_departed are
        // what the first fold ACTUALLY bound to.  Rows > 0 with seam_present == 0 is the
        // MISWIRE reading, not a clean one.
        long long n_resume_cpl_rows        = 0;  ///< L leveling rows whose regression was rebuilt
        long long n_resume_cpl_zrows       = 0;  ///< Z (ZWD) rows whose regression was rebuilt
        long long n_resume_cpl_terms       = 0;  ///< regressor entries restored across both
        long long n_resume_cpl_dict        = 0;  ///< shared-column dictionary entries read
        long long n_resume_cpl_orphan      = 0;  ///< QL/QZ rows with no live carry to attach to
        long long n_resume_cpl_seed_reinit = 0;  ///< tu_reinit_hist seeds planted (LOOSE anchor / eph cutover)
        long long n_resume_cpl_seam_present  = 0;  ///< first warm epoch: coupling terms that RESOLVED to a live column
        long long n_resume_cpl_seam_departed = 0;  ///< ...that marginalized (departed or reinit-diverted)
        long long n_resume_cpl_diag_border   = 0;  ///< C1 border keys that folded DIAGONALLY at the first post-resume fold (the known one-epoch Card-1 asymmetry — NetworkRunContext.cpp:1163-1169)
        // First-post-resume-harvest OBSERVATORY (measure-only, byte-identical): the
        // restored M against the epoch's OWN freshly derived M_d for the same key.  This
        // is the only direct evidence the restored row was frame-valid, and it needs no
        // second leg.  It is also the instrument that separates "the regressors were
        // stale" from "the residual is the diagonal C1 border" if the trips do not reach 0.
        long long n_resume_cpl_census_rows      = 0;  ///< rows compared against their own fresh harvest
        long long n_resume_cpl_census_terms     = 0;  ///< identities matched in both vectors
        long long n_resume_cpl_census_unmatched = 0;  ///< restored identities the fresh harvest did not produce
        // §18.12 (2a): the dense D2 covariance side-car across the resume seam.
        long long n_resume_cpl_cov_keys    = 0;  ///< dense-carry keys restored from the snapshot's QK/QP records
        long long n_resume_cpl_cov_dropped = 0;  ///< QK keys with no restored D row (row + column dropped)
        long long n_resume_dense_carry_keys = 0;  ///< RS-7 (§21.14) — carried D rows present at a warm resume with a dense (full-cov) carry.  The marker that separates "no warm resume happened" from "the border came back perfectly": n_resume_cpl_diag_border is 0 in BOTH cases, so the run-end readout needs its own denominator.
        long long n_resume_first_fold_cta_trips = 0;  ///< RS-7 (§21.14) — Card-Q3 clock-anchor DECOUPLES (nis + abs + gap + datum + ext) accumulated over the FIRST post-resume epoch, latched when the census retires.  §18.11's readout: a restored carry injected as a plain stiff prior on a border with no cross-covariance out-votes the anchored clock priors and trips them at the first fold, so "trips at the seed's level" is the border's health check.  Structurally 0 on a lineage that does not arm network.clock_anchor — the run-end line says which case it is (invariant #4).
        // READMIT continuity (E2) — BOTH edges (standing invariant #7): parked = the
        // DECOUPLING edge (what left, and when); seeded = the RE-COUPLING edge (what came
        // back and was re-validated — by the Q3 guard for the clock, by the stash-side
        // seed check for every kind); expired / dropped = what was NOT reconnected, and why.
        long long n_readmit_parked        = 0;  ///< D2 Clock/CodeBias entries parked at a satellite's departure
        long long n_readmit_seeded_clk    = 0;  ///< clock columns seeded from a parked entry on re-registration
        long long n_readmit_seeded_cb     = 0;  ///< code-bias columns seeded
        long long n_readmit_seeded_anch   = 0;  ///< ...of the clock seeds, folded ANCHORED over the gap (vs LOOSE)
        long long n_readmit_expired       = 0;  ///< parked entries dropped: the gap exceeded the horizon
        long long n_readmit_dropped_datum = 0;  ///< parked entries dropped: a clock-datum change retired their frame
        long long n_readmit_dropped_stale = 0;  ///< parked entries dropped: the column returned but the entry was not consumed (skip-stash / pinned)
        // §6.12.5: the DD trace's reference identity, made visible.  Standing invariant #4 — a
        // reference policy that never changes the elected satellite is INERT, and the only way to
        // know is to count.  Both counters are written on every traced epoch regardless of policy,
        // so the `lowest` default reports its own (previously silent) switch rate too.
        long long n_ddfrac_ref_switch     = 0;  ///< (cons,signal) reference changes across traced epochs
        long long n_ddfrac_ref_epochs     = 0;  ///< (cons,signal) reference elections performed (denominator)
        long long n_ddfrac_ref_age_win    = 0;  ///< elections where `oldest` chose a DIFFERENT satellite than `lowest` would have (0 => the policy is inert on this run)
        long long n_readmit_events        = 0;  ///< re-admissions detected (clock column back after >= kReadmitMinGapEp loop epochs)
        long long n_readmit_seed_moved    = 0;  ///< Q3 nis/abs trips within kReadmitVerifyWindowS of a clock seed — the data REJECTED the seed
        long long n_readmit_seed_check    = 0;  ///< stash-side seed-vs-solve checks run (all seeded kinds)
        long long n_readmit_seed_outlier  = 0;  ///< ...of which |e|/sqrt(S) > kReadmitSeedK
        long long n_readmit_marg_windows  = 0;  ///< coupling marginalization windows opened
        long long n_cpl_readmit_diverted  = 0;  ///< coupling terms diverted to the departed route by a re-admission window
        long long n_snap_parked_excluded  = 0;  ///< parked entries excluded from written snapshots

        // --- The CARRY-DOMAIN lane observatory --------------------------------
        // Measure-only: it computes the lane response of the free re-level and
        // histograms it, writing nothing back, so an armed run is byte-identical on
        // every solve artifact.  That is deliberate — the three preceding iterations
        // all died on unreconciled counters, and the |delta| DISTRIBUTION is what
        // decides whether an apply leg is worth building at all.
        //
        // delta = [C_a(t+)/lam_a - C_b(t+)/lam_b] - [C_a(t-)/lam_a - C_b(t-)/lam_b] - dM
        // Residual is eps_L(t+) - eps_L(t-) - eps_MW, with eps_MW = 0.351 cyc on GPS
        // L1/L2 and 0.051 cyc on L2/L5 EWL at sigma_P = 0.3 m.  The carry-frame
        // hypothesis predicts the bulk of |delta| below ~2 cyc; the seed site's own
        // counters showed 97 % above 25 cyc, which is what a frame mismatch looks
        // like and is the falsifier to watch for.
        //
        // CLOSURE IDENTITY (invariant #4 — a channel that does not close is a
        // miswire, not a null):
        //     n_lane_attempts == n_lane_measured
        //                      + n_lane_skip_no_row     + n_lane_skip_no_mw
        //                      + n_lane_skip_unmeasured + n_lane_skip_pair_changed
        //                      + n_lane_skip_no_freed_band
        //                      + n_lane_skip_no_pre     + n_lane_skip_stale_pre
        //                      + n_lane_skip_no_post
        //                      + n_lane_skip_bridged    + n_lane_skip_bad_lambda
        //     n_lane_measured  == sum(n_lane_hist[0..8])
        long long n_lane_attempts        = 0;  ///< (station, satellite) pairs with >= 1 freed signal this epoch — the denominator.  Counted over the FREED set, not over the raw rows, so an arc the raw pass never saw is still visible (as n_lane_skip_no_row) instead of silently leaving the denominator.
        long long n_lane_measured        = 0;  ///< pairs that yielded a finite delta.
        long long n_lane_skip_no_row     = 0;  ///< the freed (station, satellite) had NO raw-observable row this epoch at all (satellite not tracked by the raw pass, or station absent).
        long long n_lane_skip_no_mw      = 0;  ///< a raw row existed but carried no usable second band (not dual).
        long long n_lane_skip_unmeasured = 0;  ///< raw-MW entry present but no adjacent predecessor (step not formable).
        long long n_lane_skip_pair_changed = 0;///< the adjacent raw predecessor used a DIFFERENT band pair, so ΔMW is not in this lane's cycles.
        long long n_lane_skip_no_freed_band = 0;///< the satellite had a freed band, but NOT one of this pair's two — both carries are continuous, so there is no re-level response to measure.  (Third-band frees: L5 on a triple posture, or a second channel of the same band under channel_mode=all.)
        long long n_lane_skip_no_pre     = 0;  ///< a band had no live pre-event carry (new arc, or retired last epoch).
        long long n_lane_skip_stale_pre  = 0;  ///< a pre-event carry existed but was NOT stashed at the immediately preceding processed epoch, so differencing it against a one-epoch ΔMW would compare unequal intervals.
        long long n_lane_skip_no_post    = 0;  ///< a band was not RE-STASHED this epoch.  ⚠ Tested on last_seen_s, not on map presence: an un-refreshed entry survives for a full pl_max_gap_s, and accepting it would silently contribute the PRE value as the POST value — collapsing δ to a one-band code re-level (top bins) or to exactly −ΔMW (≈0.35 cyc, bottom bins).  Both directions manufacture a verdict.
        long long n_lane_skip_bridged    = 0;  ///< a band was delta-bridged — its carry is already re-based, so the lane response is the bridge's, not the free re-level's.
        long long n_lane_skip_bad_lambda = 0;  ///< non-finite / non-positive wavelength on either band.
        long long n_lane_both_freed      = 0;  ///< ...of the measured, pairs where BOTH bands were freed (vertex-wide response).
        long long n_lane_one_freed       = 0;  ///< ...of the measured, pairs where exactly one band was freed (band-scoped response; its partner's carry is continuous).
        double    lane_abs_delta_sum_cyc = 0.0;///< Sigma |delta| over measured pairs (cycles) — UNCONDITIONAL, unlike the seed site's post-cap sum that hid its own falsification.
        double    lane_abs_delta_max_cyc = 0.0;///< max |delta| (cycles).
        /// |delta| histogram, bins < 0.1 / 0.25 / 0.5 / 1 / 2 / 5 / 10 / 25 / >= 25 cyc.
        long long n_lane_hist[9]         = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        // --- §7.6 instrument additions --------
        // (b) band-scoped-only histogram: the one_freed subset of n_lane_hist,
        //     so the vertex-wide distribution is the bin-wise difference and the
        //     two responses (rung-2 contrast) separate.  Closure:
        //     sum(n_lane_hist_bandscoped) == n_lane_one_freed.
        long long n_lane_hist_bandscoped[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        // (c) cause split over MEASURED pairs — BIT counts (a pair freed by two
        //     detectors increments two of these), so they sum to >= n_lane_measured.
        long long n_lane_meas_mw   = 0;  ///< measured pairs with the MW bit.
        long long n_lane_meas_lli  = 0;  ///< measured pairs with the LLI bit.
        long long n_lane_meas_iftd = 0;  ///< measured pairs with the IF-TD bit.
        // (d) T4-side coverage: the SAME delta measured on pairs whose carries
        //     the RAIM attribution/consensus erase refloated — the channel that
        //     is 97.3 % of all field freeing and structurally invisible to
        //     free_ambs.  Pre-erase values captured at the erase site
        //     (lane_t4_pre), measured at the NEXT harvest.  Separate closure:
        //     n_lane_t4_attempts == n_lane_t4_measured + Σ n_lane_t4_skip_*.
        long long n_lane_t4_captured     = 0;  ///< (station,sat,signal) arcs captured at the erase sites.
        long long n_lane_t4_attempts     = 0;  ///< (station,sat) pairs with >= 1 captured band last epoch.
        long long n_lane_t4_measured     = 0;  ///< pairs that yielded a finite T4 delta.
        long long n_lane_t4_skip_no_row  = 0;  ///< no raw-observable row this epoch.
        long long n_lane_t4_skip_no_mw   = 0;  ///< raw row not dual.
        long long n_lane_t4_skip_unmeasured = 0;   ///< no adjacent raw predecessor.
        long long n_lane_t4_skip_pair_changed = 0; ///< predecessor used a different band pair.
        long long n_lane_t4_skip_gauge   = 0;  ///< the pair's (sat,σ) was eq34-re-referenced THIS epoch — the capture predates the frame step (the one guard the detector path gets for free from lane_pre living in local_carry; the capture is a copy and must be interlocked).
        long long n_lane_t4_skip_no_pre  = 0;  ///< a band of the MW pair had no capture.
        long long n_lane_t4_skip_stale_pre = 0;///< capture not from the immediately preceding harvest.
        long long n_lane_t4_skip_no_post = 0;  ///< a band was not RE-STASHED this epoch (same last_seen_s test as the detector path).
        long long n_lane_t4_skip_bad_lambda = 0; ///< non-finite wavelength/delta.
        double    lane_t4_abs_delta_sum_cyc = 0.0;
        double    lane_t4_abs_delta_max_cyc = 0.0;
        long long n_lane_t4_hist[9]      = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        // QUEUE item 4 R2/R4 — both-edge declaration
        // telemetry (invariant #7) + hygiene accounting.
        long long n_staf_departs          = 0;  ///< R2 depart events (EWMA ratio > threshold)
        long long n_staf_returns          = 0;  ///< R2 return events (hysteresis 0.8×)
        double    staf_max_ratio          = 0.0;///< run-max EWMA ratio to the network median
        long long n_staf_epochs           = 0;  ///< epochs the factor was computed on
        long long n_t3_untestable_cells   = 0;  ///< R4: s_raw==0 cells excluded from T3 opp
        // LV-6 census — invariant #4 counters.
        long long n_lvlauth_epochs   = 0;       ///< epochs with a FLEET row
        long long n_lvlauth_immature = 0;       ///< LOS skipped at the min_n screen
        double    lvlauth_sum = 0.0;            ///< Σ L_cs (teardown mean)
        double    lvlauth_min =  1.0e30;        ///< run min L_cs (TECU)
        double    lvlauth_max = -1.0e30;        ///< run max L_cs (TECU)
        // Phase 2.2: epochs whose EMISSION the SH health gate withheld.  NOT a
        // subset of n_skipped — the solve/carry/QC all ran (emission-only gate),
        // so these epochs are counted nowhere else.
        size_t n_sh_gate_withheld = 0;
        // Card CST (network.trace_consumer_sat) — invariant #4 counters for the
        // per-satellite consumer readout.  0 epochs on an ARMED run means the
        // consumer deferred every epoch (admission gates) and the file is a bare
        // header; 0 rows with non-zero epochs is impossible by construction.
        long long n_cst_epochs = 0;  ///< consumer epochs that contributed >= 1 row
        long long n_cst_rows   = 0;  ///< (satellite, raw phase signal) rows written
        // Card JT (network.disc_jump_test_decommon) — the four declaration classes of
        // the DE-COMMONED phase jump test, invariant #7's identities counted (the
        // per-cell PB_JUMP rows carry sat / signal / raw dPB / c_s / de-commoned dPB).
        // All four stay 0 on an OFF run.  `decommon_only` is the class the raw test
        // MISSED (a frame move hid a real per-satellite step); `raw_only` is the class
        // it MINTED (the frame moved, this satellite did not).  A run with 0 in both
        // is a knob that changed nothing, however clean the log reads.
        long long n_jt_decommon_only = 0;  ///< de-commoned declared, raw would NOT
        long long n_jt_both          = 0;  ///< both tests declared
        long long n_jt_raw_only      = 0;  ///< raw would have declared, de-commoned did NOT
        long long n_jt_fallback_raw  = 0;  ///< signal population below kDiscDecommonMinSats ⇒ raw result stands
        // --- Card MS (network.disc_mark_scope): per-channel discontinuity marks ---
        // Invariant #7's DECOUPLING identity for a per-satellite mark: which channel
        // generations it actually stepped, and which it was withheld from.  All zero
        // under the default `sat` scope (nothing is counted there), so a run that
        // reports 0/0/0 with the scope armed has an inert wiring however clean it reads.
        long long n_mark_clk = 0;  ///< satellite-epochs whose CLOCK generation a mark bumped
        long long n_mark_cb  = 0;  ///< (sat, signal) CODE-BIAS cells a mark bumped
        long long n_mark_pb  = 0;  ///< (sat, signal) PHASE-BIAS cells a mark bumped
        long long n_mark_suppressed_by_scope     = 0;  ///< (sat, channel) marks the scope withheld — total
        long long n_mark_suppressed_by_scope_clk = 0;  ///< …of which the CLOCK channel
        long long n_mark_suppressed_by_scope_cb  = 0;  ///< …of which the CODE-BIAS channel
        long long n_mark_suppressed_by_scope_pb  = 0;  ///< …of which the PHASE-BIAS channel
        // Prioritized-datum drop-and-heal observability: epochs where the gate threw and the
        // near-null phase-bias soft-anchor recovered it, and total columns anchored.  A run
        // that heals every epoch "succeeds" silently otherwise (the heal-success log is debug).
        size_t n_healed_epochs = 0, n_healed_cols = 0;
        // The SAME quantities counted on EVERY fired heal, recovered or not.
        // The pair above is success-only, which makes a heal that fires every epoch and
        // never rescues the gate read as "0 healed" — measured on the 305 co-gated arm,
        // which logged a heal-failure withhold on all 6 epochs while reporting
        // n_healed_epochs = 0.  (That measurement predates the unified withhold WARN
        // in NetworkSolveGlue; the log text it quoted, "drop-and-heal failed", is now
        // a trailing clause of that one line rather than a message of its own.)  That is precisely the inert-looking-but-live wiring
        // standing invariant #4 exists to catch, so the attempt is counted separately:
        // attempts > 0 with recoveries == 0 is the signature of a heal that is aiming at
        // the right columns but cannot resolve the residual alone (Card PA's own
        // insufficiency criterion, PA5).
        size_t n_heal_attempt_epochs = 0, n_heal_attempt_cols = 0;
        // Epochs on which the admitted-OSB
        // physical-bound prior was folded, and the cumulative column count.
        // Standing invariant #4 — an armed KP run with n_kp_epochs == 0 means the
        // prior never reached a solve (no registered code columns, or the W6 block
        // never ran), which no amount of clean-looking log output would reveal.
        size_t n_kp_epochs = 0, n_kp_cols = 0;
        bool   kp_margin_logged = false;   ///< one-shot latch for the K2 margin check
        // RF-0b (network.w6_iono_constraint = none): epochs on which the satellite
        // code-bias datum was folded with NO ionosphere constraint behind it, and the
        // cumulative count of sum-to-zero (constellation, signal) rows in those epochs —
        // the rows that ARE the datum in that posture.  Standing invariant #4: an armed
        // "none" run with n_w6_iono_none_epochs == 0 never reached the W6 block at all
        // (no registered code columns), and one with n_w6_iono_none_s2z_rows == 0 folded
        // the IF pin against NOTHING — in both cases the satellite code biases have no
        // datum whatever, which a clean-looking log would never reveal.
        size_t n_w6_iono_none_epochs = 0, n_w6_iono_none_s2z_rows = 0;
        // RF-0c (network.sat_gf_datum): epochs on which the HARD per-satellite
        // geometry-free datum folded, the cumulative row count, and — under
        // broadcast_tgd — how those rows were targeted: `tgd` a non-zero c*T_GD from
        // the navigation message, `tgd0` a satellite the constellation broadcasts at
        // T_GD = 0 exactly (physical, just numerically zero), `fallback` a satellite
        // with no navigation record or whose pinned pair is not (L1, L2).  The split
        // matters because all three look identical on the wire: only the fallback
        // count says the run pinned a value the navigation message never asserted.
        size_t n_sat_gf_epochs = 0, n_sat_gf_rows = 0;
        size_t n_sat_gf_tgd = 0, n_sat_gf_tgd0 = 0, n_sat_gf_fallback = 0;
        // RF-0d: the per-signal extension.  `extra` counts the rows beyond the
        // ionosphere-free pair (what makes a sparse channel's level a datum),
        // `isc0` how many of those asserted a zero inter-signal correction the
        // navigation message never stated, `sameband` the columns deliberately
        // left estimable, and `s2z_suppressed` the sum-to-zero rows dropped
        // because the datum already pins every column of that channel.
        size_t n_sat_gf_extra = 0, n_sat_gf_isc0 = 0;
        size_t n_sat_gf_sameband = 0, n_sat_gf_s2z_suppressed = 0;
        // RF-0e: e1's absolute IF-level rows (and the satellites they sum), and
        // e2's split of geometry-free rows into CARRIED (a change pin on the
        // previous solved value) vs ANCHORED (first sighting, the absolute
        // broadcast target).  The anchored count is the only place the broadcast
        // frame is re-asserted under e2, so a run with 0 of them has carried its
        // frame from epoch 1 and never re-checked it.
        size_t n_if_level_rows = 0, n_if_level_sats = 0;
        size_t n_sat_gf_carried = 0, n_sat_gf_anchored = 0;
        // RF-0f: band-2 companion columns left free because a station bridged the two
        // signals, vs pinned at 0 because none did.  The pinned ones are the part of a
        // C2X product that is asserted rather than estimated, and they are not emitted.
        size_t n_isc_observed = 0, n_isc_pinned = 0;
        // #station-epochs whose joint
        // coupling-row covariance was non-SPD after the ridge retry and fell back to the
        // Card-1 per-row scalar marginalization.  Warned once at run end.
        // RXCB-DATUM: zero-mean receiver code-bias prior rows folded over the run
        // (one per station-epoch per constellation).  0 with the toggle armed is
        // the standing invariant #4 reading: no receiver DCB column ever existed.
        size_t n_rxcb_gauges = 0;
        size_t n_cpl_joint_fallback = 0;
        // Card 1/1d observability: run-total coupling-carry volume — harvest-side stash
        // counts (non-empty regression vs EMPTY = plain-diagonal degrade vs outright
        // harvest failure) and inject-side term counts (present / departed / κ-shrunk).
        // A run whose coupling carry silently degrades to diagonal rows (or never runs)
        // is indistinguishable from a healthy one without these — the Card-1b/1d
        // byte-identical-A/B trap.  Logged once at run end when the card is armed.
        size_t n_cpl_stash = 0, n_cpl_stash_empty = 0, n_cpl_harvest_fail = 0;
        long long n_cpl_terms_present = 0, n_cpl_terms_departed = 0, n_cpl_terms_shrunk = 0;
        // The ON-path telemetry for the
        // departed-column gauge defect.  `held` counts (constellation,kind) families whose
        // coefficient sum was restored to its HARVEST value against a truncated column;
        // `orphan` counts families whose truncated mass had no present recipient, i.e. the
        // rows this card could NOT make gauge-invariant.  Standing invariant #4: armed with
        // both at 0 means no row was ever truncated — the card is inert, not clean.
        long long n_cpl_gauge_held = 0, n_cpl_gauge_orphan = 0;
        /// Orphan attribution (owed by the 2026-08-19 multi-constellation arm): run totals
        /// per (constellation, SharedParamKind) — orphan count + summed |truncated mass|
        /// left gauge-dirty.  Structurally a >=2-constellation channel (a row's minority
        /// family can lose ALL present columns while the majority family survives);
        /// every GPS-only arm measured 0.  Fed by foldAndDumpCouplingRows, read at teardown.
        std::map<std::pair<char,int>, std::pair<long long,double>> cpl_orphan_fam_total;
        /// Card 1g's own denominator — terms diverted to the departed route by the WIDENED
        /// (kind,sat,signal) probe specifically.  Armed with this at 0 means the
        /// realization-step feeds never matched a live coupling term: INERT, not clean.
        long long n_cpl_reinit_diverted = 0;
        /// SPEC_slip_relevel_walk: run total of inject-site free-epoch skips
        /// (relevel_free_epoch).  Armed with this at 0 on a slip-bearing run is
        /// a MISWIRE (invariant #4) — the drains latched but the inject never
        /// consulted, or no marked arc ever had a live carry.
        long long n_relevel_free_skips = 0;
        /// Retap piece run totals: arcs KEPT (level preserved, var re-tapped),
        /// streak ESCALATIONS to the erase, and flagged arcs with no live carry.
        long long n_relevel_retap_kept = 0, n_relevel_retap_escalated = 0,
                  n_relevel_retap_miss = 0;
        /// Corroborated retap: keeps that carried
        /// positive raw-observable continuity evidence / refusals because the
        /// raw level STEPPED / refusals because the arc was UNMEASURED (no
        /// predecessor or the qualification pass did not run).  Both refusal
        /// counters landing at 0 on a slip-bearing armed run is invariant #4's
        /// "clean log, dead wiring" — check them, not the keep count.
        long long n_relevel_retap_corr_kept = 0, n_relevel_retap_corr_stepped = 0,
                  n_relevel_retap_corr_unmeasured = 0;
        /// Slant-iono carry (QUEUE 8b) run totals — BOTH coupling edges (invariant #7).
        /// injected: priors folded (scope-gated).  kept_through_slip: slip-funnel
        /// entries whose bands were freed (KEEP).  erased_slip: funnel entries not
        /// freed (ERASE).  erased_t4: T4/RAIM arc-erase hook.  evicted: gap eviction.
        /// floor_hits: injects clamped by iono_carry_var_floor.  refused_datum:
        /// DECLARED-ZERO — the code-datum re-realization edge (GF-pin re-election /
        /// cb_excursion_reinit) ships bare; nonzero means the edge got wired.  Armed
        /// with injected=0 on a slip-bearing run is a MISWIRE (invariant #4).
        long long n_iono_carry_injected = 0, n_iono_carry_kept_through_slip = 0,
                  n_iono_carry_erased_slip = 0, n_iono_carry_erased_t4 = 0,
                  n_iono_carry_evicted = 0, n_iono_carry_floor_hits = 0,
                  n_iono_carry_refused_datum = 0;
        /// CS-GF authority (iono_carry_source = smoothed_gf): recursion resets
        /// (gap / slip-gate / first sighting) and blend steps.  Armed with
        /// blends = 0 is a MISWIRE (invariant #4).
        long long n_iono_cs_resets = 0, n_iono_cs_blends = 0;
        /// §Z BAND-SCOPED T4 ERASE (network.raim_erase_band_scope = min_keep_one;
        /// docs/SPEC_event_harness.md).  cells: isolated-arc erase cells the scope
        /// actually adjudicated.  erased / kept: bands freed vs bands left pinned
        /// (kept is one per adjudicated multi-band cell by construction).
        /// kept_over_gate: cells where EVERY band stood above k·σ̂₀, so the pin is
        /// the least-BAD band rather than a clean one.  single_band: cells skipped
        /// outright because the arc had one band and there was none to spare.
        /// Armed with cells = 0 on a T4-active run is a MISWIRE (invariant #4).
        long long n_bscope_t4_cells = 0, n_bscope_t4_erased = 0,
                  n_bscope_t4_kept = 0, n_bscope_t4_kept_over_gate = 0,
                  n_bscope_t4_single_band = 0;
        /// ANCHOR-SLIP HANDOVER (network.cycle_slip.anchor_slip_handover;
        /// docs/SPEC_event_harness.md §12.7).  staged: directives the seam put up
        /// (= families whose ANCHOR arc a slip response freed).  handed: of those,
        /// the ones the election actually re-hosted.  continuous / cold: the
        /// successor pinned at its own carried value_abs vs pinned at 0 because no
        /// usable ledger entry survived the variance gates — a cold outcome IS a
        /// gauge re-realization and is never folded into the continuous total.
        /// no_successor: the ban left no candidate, so the family went unpinned for
        /// the epoch.  inert: the directive matched no family in this epoch's union
        /// (gated, bridged or pruned column).  ARMED with staged = 0 on a run whose
        /// detectors fired is not proof of a quiet network — it is the first thing
        /// to check (invariant #4); staged > 0 with handed = 0 is dead wiring.
        long long n_anc_slip_staged = 0, n_anc_slip_handed = 0,
                  n_anc_slip_continuous = 0, n_anc_slip_cold = 0,
                  n_anc_slip_no_successor = 0, n_anc_slip_inert = 0,
                  n_anc_slip_epochs = 0;
        /// MW DETECT-ONLY ADMISSION CENSUS (network.cycle_slip.mw_evidence_refloat).
        /// Reduced serially from PassABuffers::sta_mw_admit after the parallel scan,
        /// and collected KNOB-INDEPENDENTLY — that is the point.  legacy: admitted by
        /// today's integer gate.  evidence: admitted by the strength rule alone with
        /// the knob ARMED.  would_admit: the same population with the knob OFF, i.e.
        /// exactly what arming would change (an OFF run that reports 0 here says the
        /// card is inert on that lineage, no run needed).  drop_loose / drop_unknown:
        /// the two refused populations — a flag with a known integer whose rounding
        /// residual exceeded turbo_edit_frac_max, and a flag with no usable integer
        /// at all.  Before this census both were silent: no counter, no event row.
        long long n_mw_admit_legacy = 0, n_mw_evidence_admitted = 0,
                  n_mw_evidence_would_admit = 0, n_mw_drop_loose = 0,
                  n_mw_drop_unknown = 0;
        /// COMMON-BAND SCOPE CENSUS (network.cycle_slip.common_band_scope,
        /// SPEC_event_harness §14).  Reduced per epoch from CommonBandEpoch and,
        /// like the MW admission census above, collected KNOB-INDEPENDENTLY —
        /// that is the point.  verdicts: (station, satellite) pairs that had at
        /// least one MW pair FIRE, i.e. the population a band scope could act on.
        /// narrowed: of those, the ones positive exoneration restricted.
        /// bands_kept: bands a narrowed verdict SPARED (Σ|exonerated|) — the
        /// headline, because under the all-band union every live band would
        /// otherwise have been freed.  all_no_exoneration / all_inconsistent: the
        /// two fallbacks that return today's response (nothing exonerated; every
        /// band exonerated although a pair fired).  iftd_veto: silent pairs
        /// REFUSED as exonerators because the IF-TD detector flagged them (the
        /// equal-count trap).  immature_pairs: silent pairs refused for arc age.
        /// would_narrow: the knob-OFF count of verdicts that WOULD have narrowed,
        /// i.e. exactly what arming changes — an OFF run reporting 0 here says
        /// the card is inert on that lineage, no armed run needed.
        /// weak_silence: exonerating silences whose DETECTION THRESHOLD stood
        /// above 6 wide-lane cycles — a gate too loose to have excluded the slip
        /// sizes the scope responds to.  Reported, never acted on; it is the
        /// honest measure of how much narrowing rests on a weak silence.
        long long n_cb_verdicts = 0, n_cb_narrowed = 0, n_cb_all_no_exoneration = 0,
                  n_cb_all_inconsistent = 0, n_cb_bands_kept = 0,
                  n_cb_iftd_veto = 0, n_cb_immature_pairs = 0,
                  n_cb_weak_silence = 0, n_cb_anchor_colocate = 0,
                  n_cb_would_narrow = 0;
        /// §14.11 RULE F2 — silent pairs refused as exonerators because one of
        /// their bands carried a receiver LLI at the PREVIOUS epoch, so the
        /// detector withheld that pair's sample and preserved the arc (count
        /// running straight through the flagged epoch while the lane saw
        /// nothing).  Its own counter, not folded into n_cb_immature_pairs: the
        /// two refusals have different causes, and this one sizes the population
        /// the five measured spurious `keep` verdicts came from.
        long long n_cb_lli_shadow = 0;
        /// §14.12 RULE F3 — verdicts formed from the receiver's LLI bits alone,
        /// at an epoch where every MW sample of the satellite was withheld so the
        /// normal rule could not run, and how many of those actually restrict a
        /// drain.  Knob-independent (invariant #4): an OFF run reports what
        /// arming would scope.  A run whose LLI drain fires with lli_verdicts 0
        /// is dead wiring.
        long long n_cb_lli_verdicts = 0, n_cb_lli_narrowed = 0;
        /// §14.12.5: of those, the ones the §14.5 ANCHOR CLAUSE refused (also in
        /// @ref n_cb_anchor_colocate).  Its own counter because the two paths'
        /// co-locates have to be distinguishable: the storm arm attributed the
        /// split-reference regression to the F3 path with exactly this split.
        long long n_cb_lli_anchor_colocate = 0;
        /// SPLIT-REFERENCED SATELLITE CENSUS (§14.5), ALWAYS ON and
        /// knob-independent: per PROCESSED epoch, how many satellites have >= 2
        /// anchored phase-bias families whose anchors sit at DIFFERENT stations.
        /// Those satellites' per-frequency biases are realized against arc levels
        /// that walk independently, and this is the instrument that caught the
        /// iteration-1 storm regression (6.6 % -> 16.5 % of satellite-epochs,
        /// 27 % in hour 6).  It must read the same with the scope OFF, which is
        /// why it is not knob-gated.  Sum + epochs give the mean; the max is the
        /// worst epoch; multi_sum's mean is the population that CAN split.
        long long n_cb_split_sats_sum = 0, n_cb_split_sats_max = 0,
                  n_cb_split_multi_sum = 0, n_cb_split_epochs = 0;
        /// §14.10 IF-TD PAIR WIDTH (network.cycle_slip.if_phase_td_pairs), the
        /// ON-path telemetry of the new pairs: per PAIR KIND, how many samples
        /// the pooled test thresholded, how many it flagged, and how many of
        /// those flags actually freed an ambiguity.  Collected KNOB-
        /// INDEPENDENTLY — a `first` run reports the one watched pair, so the
        /// clause sizes the card before arming it, and an `all` run whose extra
        /// pair kinds show evaluated ≫ 0 with flagged 0 over a slipped bed is
        /// dead wiring, not a quiet lane (invariant #4).
        IfTdPairTallies n_iftd_pair;
        /// §14.10, the veto's per-pair-kind breakdown: silent MW pairs the
        /// common-band scope refused as exonerators because an IF-TD pair
        /// flagged, attributed to the IF-TD pair KIND that did the vetoing.  The
        /// whole point of the card is that the vetoes come from pairs the
        /// single-pair detector never watched, so the total (@ref
        /// n_cb_iftd_veto) alone cannot show it.
        std::map<std::string, long long> n_cb_iftd_veto_by_pair;
        /// §22 THE REFERENCE-ARC CLAUSE (network.cycle_slip.common_band_ref_arc):
        /// silent MW pairs refused as exonerators because their IF-TD sample sits
        /// on the chain's REFERENCE arc, whose double difference is identically 0
        /// — so the pooled test could not have flagged it and the silence is the
        /// ABSENCE of a test, not a clean bill of health.  Counted apart from
        /// @ref n_cb_iftd_veto on purpose: a veto is positive evidence the pair
        /// moved, this is a structural dead zone, and only the second one carries
        /// the clause's named cost (a genuine single-band slip on a reference arc
        /// also stops narrowing).  Nonzero only with the clause armed, so an
        /// armed leg whose event sits on a known reference arc and reports 0 here
        /// is dead wiring (invariant #4); the by-pair map attributes the
        /// refusals to the CHAINS whose reference the arc is.
        long long n_cb_iftd_ref_arc = 0;
        std::map<std::string, long long> n_cb_iftd_ref_arc_by_pair;
        int n_cpl_row_dump = 12;   // Card 1d debug: one-shot assembled coupling-row dump budget
        int n_cpl_solve_dump = 8;  // Card 1d debug: one-shot full-precision solve-checksum budget
        // Coverage-threshold union gate (network.phase_datum_min_stations): epochs on
        // which >=1 thin column was excluded, and the cumulative column-epoch count.
        size_t n_gated_epochs = 0, n_gated_key_epochs = 0;
        size_t n_perconst_partial_epochs = 0;     // RTA-10: epochs published PARTIALLY (σ̂0 carve-out pruned >=1 constellation)
        size_t n_perconst_diffuse_published = 0;  // RTA-10: epochs published in FULL although the global σ̂0 tripped (every constellation individually clean — the diffuse cross-constellation edge of the per-const approximation)
        FlagRate fr_if_slip;    // T1 per (station,sat,pair) IF-phase sample   k=if_phase_td_k_mad
        FlagRate fr_if_refgate; // T1r per (station,epoch,constellation) family of n>=2 non-ref DDs — the IF-TD REFERENCE arm, whose action drains through sta_if_resets/refsat_reset and increments NOTHING in T1 (§6.14.5 defect 1: the table read T1 flags=0 for a run the tier drove 1917 acting refloats on).  DELIBERATELY a separate tier: different opportunity denominator (station-epoch-constellations, not per-sat samples) and a different threshold (max(k*s_eff, floor_frac*lambda_IF) hardened, a bare physical floor legacy) — merging it into T1 would re-hide exactly that asymmetry.  Aggregated at teardown from each station detector's RefGateCounters.
        FlagRate fr_we_trigger; // T2 per warm within-epoch epoch: σ̂0_first>trig k=sigma0_trigger
        FlagRate fr_raim_cell;  // T3 per (sat,station) standardized residual   k=raim_k
        FlagRate fr_raim_sat;   // T3 consensus per spiked sat (>=min_sta)       k=raim_k
        FlagRate fr_attrib;     // T4 per median-polish SAT column: eff/σ̂0>k & σ̂0>trigger (B8/P1-4)
        FlagRate fr_attrib_arc; // T4 per median-polish ARC interaction: inter/σ̂0>k (the acting sub-path's criterion, pre persistence/maturity gates — §9.7.10: without this line the tier reads 0-flag while it refloats arcs)
        FlagRate fr_clk_model;  // T5 per sat clock-model NIS (own, !ext)        k=clock_model_k
        FlagRate fr_clk_mon;    // T6 per sat clock-monitor abs/jump (own, !ext) absolute bounds
        double max_dur_anchor_s = 0.0;
        bool   max_dur_anchored = false;
        int    spectrum_diag_budget = 6;    // estimability spectrum dump: first N gate evaluations (was one-shot; a rows-epoch needs a later firing than epoch 1).  RF-NS: seeded from network.trace_estimability_budget by the NetworkRunContext ctor (default 6 = this constant); under network.trace_estimability_deficit_only a clean epoch does NOT spend it.

        // [TIMING] processed-epoch count + T2e decimation counter (relocated
        // from the loop-timing cluster; t_loop_begin stays a run-TU local).
        std::size_t n_loop_epochs = 0;
        std::size_t decim_counter = 0;   // T2e: counts post-offset raw epochs for epoch_decimation_n

        // CB-ADM: W6 code-bias admission census tallies (run-end observability;
        // standing invariant #4 — inert wiring is only visible through them, so
        // a BDS-armed ON run with n_cb_quarantined_census == 0 is a MISWIRE).
        // n_cb_late_quarantined counts distinct (sat,signal) keys first seen at
        // the registration site AFTER the census window (implicitly quarantined).
        std::size_t n_cb_admitted = 0, n_cb_quarantined_census = 0, n_cb_late_quarantined = 0;

        // The measurable cost of the
        // deliberate CB-ADM granularity seam.  The phase co-gate lives at STATE
        // ALLOCATION and keys off the run-latched CENSUS-admitted set, while the
        // CODE column follows PER-EPOCH >=2-station coverage; per-epoch phase-state
        // allocation churn was rejected (it would thrash eq34/carry/snapshot and
        // violate the anti-flap design).  The residue is a transient ORPHAN WINDOW:
        // an ADMITTED code key that drops below 2 present stations for an epoch
        // keeps its shared phase column while its code column is withheld.  This
        // counts those (key, epoch) pairs.  Measured a no-op at 305-station density
        // (every admitted sat has >=2 present stations); a future SPARSE-tier run
        // showing it nonzero AND coincident with withholds is the trigger to
        // revisit the seam — with data, not by re-litigating the ruling.
        std::size_t n_cbadm_phase_orphan_key_epochs = 0;

        // Rank-gate calibration
        // telemetry.  The gate is an EXACT-equality test that throws in BOTH
        // directions, so "withheld" alone hides which failure happened:
        // UNDER-specified (observed > declared — a missing datum constraint, the
        // BeiDou storm) and OVER-specified (observed < declared — the tolerance
        // pushed BELOW this config's own gauge depth, i.e. a mis-calibration that
        // withholds a HEALTHY config) need opposite fixes and must be separate
        // numbers.  min/max are over the FIRST gate evaluation of each epoch (one
        // sample per epoch — the honest "what the datum looked like"; a post-heal
        // re-gate is a different question and is counted in n_healed_epochs).
        std::size_t n_gate_evaluated = 0;        ///< epochs on which the gate ran (denominator for the rest)
        std::size_t n_gate_withheld_under = 0;   ///< withheld with observed > declared (missing datum constraint)
        std::size_t n_gate_withheld_over  = 0;   ///< withheld with observed < declared (tolerance below the gauge depth)
        int nullspace_obs_min = -1;              ///< min observed near-null count over the run (-1 = never evaluated)
        int nullspace_obs_max = -1;              ///< max observed near-null count over the run (-1 = never evaluated)

        // STANDING INVARIANT #7 — DATUM-DIRECTION INFORMATION RATIO  R = (vᵀNv/vᵀv)/cutoff,
        // one sample per (epoch × declared datum direction), on the SAME reduced normals and
        // the SAME cutoff the rank gate judges by.  The gate collapses that spectrum to an
        // integer, so a declared gauge that ACQUIRES information moves the observed count by
        // one and reads exactly like a mis-calibrated tolerance.  Measured: a Card-1 coupling
        // row with a truncated family sum drove one clock gauge 1e-11 → 9.43e-3 in a single
        // epoch — five decades over the cutoff — and the only symptom was four OVER-specified
        // withholds.  R ≪ 1 = still gauge; R ≥ 1 = something folded PRE-gate is pinning a
        // direction the datum owns.  Computed UNCONDITIONALLY (one k×k quadratic form per
        // constellation against the gate's own O(n³) eigensolve): a knob-gated version would
        // have been dark on exactly the runs that needed it.
        std::size_t ddi_epochs        = 0;   ///< epochs the ratio was evaluated (denominator)
        std::size_t ddi_dirs          = 0;   ///< (epoch × direction) samples
        std::size_t ddi_breach_epochs = 0;   ///< epochs with >= 1 direction at R >= 1
        std::size_t ddi_breach_edges  = 0;   ///< invariant #7 edges emitted (breach + clear)
        std::size_t ddi_over_breach   = 0;   ///< OVER-specified withholds COINCIDENT with a breach — the discriminator the run-end advice branches on
        double      ddi_ratio_max     = -1.0;///< largest R over the run (-1 = never evaluated)
        double      ddi_ratio_max_tow = 0.0; ///< ...its epoch
        std::string ddi_ratio_max_dir;       ///< ...its identity ("CLK|G")
        double      ddi_ratio_min     = -1.0;///< smallest R — the healthy floor a free datum sits at

        // SBC (network.sbasis_complete) run-end telemetry — standing invariant #4:
        // an armed completion whose counters never move on a GPS+Gal run means the
        // block never saw the deficiency it was built for (miswire), not a clean
        // run; and every REFUSAL path is counted separately because each demands a
        // different fix (cap ⇒ pathological epoch / tolerance; degenerate ⇒ an
        // orphaned exact gauge, Card PA territory; weight ⇒ re-place rank_gate_tol).
        std::size_t n_sbc_epochs = 0;        ///< epochs on which ≥1 completion row was folded
        std::size_t n_sbc_dirs = 0;          ///< completion rows folded, cumulative
        int         sbc_max_dirs = 0;        ///< max rows folded on one epoch
        std::size_t n_sbc_release = 0;       ///< sbc_release events (satellite re-observed — invariant #7's re-coupling edge)
        std::size_t n_sbc_capped = 0;        ///< epochs refused: extras beyond sbasis_complete_max
        std::size_t n_sbc_degenerate = 0;    ///< DIRECTIONS skipped as unseparated from the declared-gauge cluster (machine-zero orphans — left to the drop-and-heal; NOT epoch-fatal)
        std::size_t n_sbc_weight_marginal = 0; ///< epochs refused: completion weight < 2 decades above the cutoff (K2)

        // WE-CI (network.raim_we_code_iono) run-end telemetry — standing
        // invariant #4: an armed screening whose counters never move on an
        // eventful window is a miswire, not a quiet run.
        std::size_t we_ci_code_trips = 0;   ///< epochs whose FIRST assess code-pool σ̂0 exceeded the code trigger
        std::size_t we_ci_iono_trips = 0;   ///< epochs whose FIRST assess iono-pool σ̂0 exceeded the iono trigger
        std::size_t we_ci_sta_excl   = 0;   ///< epoch-scoped station exclusions chosen from the code/iono pools
        std::size_t we_ci_sat_dw     = 0;   ///< network-wide satellite code-row de-weights chosen from the code pool
        double we_ci_s0_code_max = 0.0;     ///< max code-pool σ̂0 observed (trigger-calibration telemetry)
        double we_ci_s0_iono_max = 0.0;     ///< max iono-pool σ̂0 observed (trigger-calibration telemetry)

        // R0: run-end counters
        // for the within-epoch tier's three response classes.  Before this, only
        // the WITHHOLD had a first-class run counter (n_sigma0_withheld); the
        // refloats and exclusions existed solely as per-epoch T2 event rows, so a
        // run summary could not say how often the tier actually acted.  The clock
        // move is the §5.07.2 impulse-channel meter: how far the within-epoch
        // re-solves moved a satellite clock from the PRIMARY solve's value —
        // counters AND identity, per invariant #7.
        long long we_refloat_sat_total = 0; ///< whole-sat within-epoch refloats settled over the run
        long long we_refloat_arc_total = 0; ///< isolated-arc within-epoch refloats settled over the run
        long long we_exclude_sta_total = 0; ///< epoch-scoped station exclusions settled over the run
        double      we_clk_move_max_m   = 0.0; ///< largest |final − primary| clock move (m)
        double      we_clk_move_max_tow = 0.0; ///< TOW of that move
        std::string we_clk_move_max_sat;       ///< satellite of that move

        // FC-1 (network.fast_clock) split-cadence fast-clock telemetry — standing
        // invariant #4 in its sharpest form for this card: the fast tier runs on
        // epochs the full pipeline SKIPS, so a miswired hook produces a completely
        // clean log and zero evidence of itself.  "0 ticks_driven" on an armed run
        // means the T2e hook never fired; "0 ticks_emitted" with emit on means the
        // product wire never saw the tier.  Both are miswires, not quiet runs.
        std::size_t fc_ticks_driven   = 0;  ///< fast ticks the decimation hook actually ran (incl. shadow ticks)
        std::size_t fc_ticks_estimated = 0; ///< ticks that produced at least one satellite clock delta
        std::size_t fc_ticks_emitted  = 0;  ///< ticks whose deltas reached the product wire (INC-3)
        std::size_t fc_los_used       = 0;  ///< LOS admitted into a tick's fit, summed over ticks
        std::size_t fc_los_screened   = 0;  ///< LOS excluded by the k·σ post-fit screen
        std::size_t fc_screen_slips   = 0;  ///< screened LOS whose residual is slip-scale (|res| >= half a wide-lane)
        std::size_t fc_sat_below_floor = 0; ///< (tick, satellite) pairs that missed min_stations_per_sat
        std::size_t fc_datum_withheld_ticks = 0; ///< ticks withheld entirely because the datum station was absent/screened
        std::size_t fc_gap_withheld_ticks   = 0; ///< ticks whose emission was suppressed by the R11 emit_max_gap_s bound
        std::size_t fc_anchor_los  = 0;     ///< LOS cached by the MOST RECENT anchor harvest (a level, not a run total)
        std::size_t fc_anchor_sats = 0;     ///< satellites in the most recent anchor (likewise a level)
        std::size_t fc_anchor_rho_fail = 0; ///< run-total LOS dropped at harvest for want of a usable range / orbit sample
        double fc_wall_ms = 0.0;            ///< run-total wall inside the tick body (perf budget: §7.3 ≤ 50 ms/tick @305)
        double fc_wall_ms_max = 0.0;        ///< slowest single tick (the budget is a MAX bound, not a mean)

        // --- redundancy / testability census ---
        // Standing invariant #4 in its statistical form.  The failure mode this
        // card must be able to disprove is "the sweep ran and found nothing",
        // which is indistinguishable from "the sweep never ran" without
        // denominators — so the passes, the sweeps and the ROWS are reported
        // before any finding is.  `redun_max_cold_*` exists for the opposite
        // reason: the prefer-fallback directive says uncheckable-but-quiet rows
        // (healthy single-frequency risers) must be COUNTED and never evented,
        // and keeping the worst one makes that a checkable claim rather than an
        // assertion.
        long long redun_passes = 0;             ///< seam invocations (the denominator for everything else).
        long long redun_sweeps = 0;             ///< epochs on which at least one station was actually swept.
        long long redun_sweeps_full = 0;        ///< of those, stride/trip FULL sweeps.
        long long redun_sweeps_targeted = 0;    ///< of those, escalation-driven single-station sweeps.
        long long redun_sweeps_trip = 0;        ///< of the full sweeps, those forced by a flight-recorder trip epoch.
        long long redun_station_sweeps = 0;     ///< (station, epoch) pairs swept — the real cost unit.
        long long redun_escalate_flagged = 0;   ///< (station, epoch) latches RAISED by the free tier — the escalation's OPPORTUNITY count.
        long long redun_escalations = 0;        ///< targeted sweeps actually TAKEN from those latches.  Necessarily 0 under stride 1 (the scheduled sweep already covered every station), so it must never be read as "the free tier found nothing" — that is what redun_escalate_flagged is for.
        long long redun_rows = 0;               ///< DATA rows examined in sweeps.
        long long redun_rows_all = 0;           ///< ALL rows examined (the closure domain: pins and coupling rows carry leverage too).
        long long redun_uncheckable = 0;        ///< DATA rows with r_i < eps (uncheckable — counted, NOT acted on).
        long long redun_uncheckable_hot = 0;    ///< of those, rows that ALSO exceeded the magnitude threshold: THE CROSS-FLAG.
        long long redun_events = 0;             ///< events actually emitted (post rate-limit) — hot cells + unobservable locals.
        long long redun_pool_skips = 0;         ///< (station, epoch, pool) centring pools skipped for population < 4.
        long long redun_r_clamped = 0;          ///< rows whose r_i hit the kernel's [0,1] clamp at 0 (leverage >= 1/w).
        double    redun_mass = 0.0;             ///< sum of r_i over DATA rows — the redundancy MASS of what was swept.
        long long redun_pivot_cols = 0;         ///< local columns examined by the INC-2 pivot census.
        long long redun_pivot_flagged = 0;      ///< of those, columns below the relative pivot floor (column-epochs).
        long long redun_unobs_entered = 0;      ///< DECOUPLING edges: (station, key) first appearances, each evented.
        long long redun_unobs_cleared = 0;      ///< RE-COUPLING edges: (station, key) that regained information.
        bool      redun_closure_any = false;    ///< at least one closure sample exists.
        double    redun_closure_min_slack = 0.0;///< worst (smallest) n_free − trace(H); must stay >= 0.
        long long redun_closure_neg = 0;        ///< sweeps whose slack went negative (a covariance/row-stack defect, not a data finding).
        double    redun_max_hot_s = 0.0;        ///< largest cross-flagged centred deviation over the run.
        std::string redun_max_hot_id;           ///< its (station/sat/class @tow) identity.
        double    redun_max_cold_s = 0.0;       ///< largest centred deviation on an UNCHECKABLE row that stayed BELOW the cross-flag.
        std::string redun_max_cold_id;          ///< its (station/sat/class @tow) identity.
        double    redun_wall_ms = 0.0;          ///< run-total wall inside the census seam (the cost claim).

        /// Baarda reliability / MDB tallies.
        /// Every field documented on the struct; reported by @c networkMdbReport,
        /// which lives next to the aggregation that produced the numbers so the
        /// counters and the sentences interpreting them cannot drift apart.
        NetworkMdbTallies mdb;

        /// BlockFormal LOS block census tallies.
        /// Named `bb`, NOT `block`: `ctx.counters.bb` is the run-total POD and
        /// `ctx.carry.bb` is the accumulator that produced it — two different
        /// objects with one short name each, deliberately spelled the same so a
        /// reader never has to remember which card owns which prefix.  It is NOT
        /// the redundancy census's `redun_*` block: that one counts ROWS, this
        /// one counts BLOCKS, and their denominators are not interchangeable.
        NetworkBlockBaardaTallies bb;

        /// Station congruence screen tallies.
        /// Named `cg` on the same convention as `bb`: `ctx.counters.cg` is the
        /// run-total POD and `ctx.carry.cg` is the accumulator that produced it.
        /// It counts STATION-EPOCHS, where `bb` counts BLOCKS and the redundancy
        /// census's `redun_*` block counts ROWS — three denominators that are not
        /// interchangeable and must never be divided into one another.
        NetworkCongruenceTallies cg;

        // --- PRIOR-RESIDUAL census --------
        // Standing invariant #4: THE LADDER IS THE TELEMETRY.  The per-family
        // ladders live in the accumulator (NetworkPriorResidual) and are read
        // straight out of it at run end; these are the seam-level counters that
        // make the ladders INTERPRETABLE — how many epochs the seam ran, how many
        // rows reached it, how many identities were emitted and how many were
        // dropped to the per-epoch buffer cap.  A family with n_rows = 0 must read
        // as "ran, saw nothing", distinct from "never ran", and that distinction
        // is prires_epochs > 0 with prires_rows = 0.
        long long prires_epochs = 0;            ///< census epochs advanced (beginEpoch) — the "did the wiring execute at all" denominator.
        long long prires_emit_passes = 0;       ///< emission-seam invocations (should track prires_epochs; a gap means an epoch fed rows nobody drained).
        long long prires_rows = 0;              ///< carried priors censused over the run, ALL families and labels.
        long long prires_reported = 0;          ///< of those, rows above the TRACE threshold (nothing acted on them).
        long long prires_events = 0;            ///< PRIORRES events actually emitted (post per-cell holdoff).
        long long prires_hits_dropped = 0;      ///< above-threshold rows whose INDIVIDUAL identity was dropped to the per-epoch buffer cap.  Never affects a ladder — but a nonzero value means the event stream is a SAMPLE, and a truncated stream must not read as a quiet run.
        double    prires_max_t = 0.0;           ///< the worst |t| over the whole run, any family, any label — EXCLUDING the RNEG class (see below).
        std::string prires_max_id;              ///< its (family/label/station/sat/signal @tow) identity.
        // --- the RNEG class (post_var > fold_var), re-partitioned out of the
        // ladders.  Its |t| is not a standardized residual — the dispersion is
        // formed from a (prior, posterior) pair that does not describe one fold,
        // so r hits the redundancy floor and |t| inflates by √((p+v)/(εp)).
        // Measured on runs/fldf3_pl2: 927 of the 957 rows above |t| = 1e4 were
        // r_negative and the run-end headline read 2.5e8 off one of them.  The
        // readable extremum for this class is |ê| in METRES.
        long long prires_rneg_rows = 0;         ///< RNEG rows over the run (a subset of prires_rows).
        double    prires_rneg_max_e = 0.0;      ///< the worst |ê| (m) over the whole run's RNEG class.
        std::string prires_rneg_max_id;         ///< its (family/label/station/sat/signal @tow) identity.
        double    prires_wall_ms = 0.0;         ///< run-total wall inside the emission seam (the §5 cost claim; the per-row addRow cost rides the carry harvests and is not separable).

        // --- CONSTRAINT-AWARE GLOBAL σ̂₀ -
        // Standing invariant #4 for the rider.  The four SUMS live in the
        // accumulator (they are the statistic); these are the seam-level
        // counters that say whether the wiring executed and in which DEGRADED
        // mode — "0 rows on the data side" and "the rider never ran" are
        // different findings, and a run whose every epoch carried the APPROX
        // flag must not read like one that never needed it.
        long long s0g_epochs = 0;               ///< drained epochs the rider evaluated — the "did it execute at all" denominator.
        long long s0g_formed = 0;               ///< of those, epochs with a defined σ̂₀²_global (Σr > 0).
        long long s0g_data_rows = 0;            ///< data rows folded into Σw·v² over the run (RAIM pool, de-duplicated).
        long long s0g_prior_rows = 0;           ///< folded census rows in Σw·ê² over the run.
        long long s0g_ep_approx = 0;            ///< epochs whose data side used the r ≡ 1 fallback (Σr_data is a row count).
        long long s0g_ep_no_data = 0;           ///< epochs with ZERO data rows (the number is prior-only).
        long long s0g_ep_no_prior = 0;          ///< epochs with ZERO folded prior rows (the number is data-only — exactly what the card corrects).
        long long s0g_ep_degenerate = 0;        ///< epochs where Σr ≤ 0 and no number was formed.
        double    s0g_max_tension = 0.0;        ///< the worst per-epoch Σw·ê²/Σw·v² of the run.
        double    s0g_max_tension_tow = -1.0;   ///< its GPS TOW (the cliff's timestamp, for §6 gate 3).

        /// Prefit census / structural fuse / seed-birth tripwire / topology
        /// ledger / flight-recorder tallies.  Every field documented on
        /// the struct; reported by the run-end block in filter/NetworkTeardown.cpp.
        /// Standing invariant #4 in its sharpest form for this card: the census
        /// is a TRIPWIRE, and a tripwire that never fires looks exactly like a
        /// tripwire that was never connected — only `epochs` / `rows` /
        /// `rec_consults` separate them.
        NetworkPrefitCensusTallies pc;

        /// Datum graph census run tallies.
        /// Standing invariant #4 with an extra twist this card owns: the EXPECTED
        /// reading of tier 1 on a healthy global roster is k = 1 everywhere with
        /// zero predictions, which is byte-for-byte what a seam that never ran
        /// produces — only `epochs` vs `epochs_quiet` tells the two apart, and the
        /// teardown line prints both.
        NetworkDatumGraphTallies dg;

        // --- PD-safe invariant-DD fold run tallies.
        // INC-0 is ALWAYS-ON and solve-neutral: the counters are pure reads of a
        // factorization the fold already computed.  They exist because the branch
        // they observe — `M := 0` on a non-PD Pᶻ+B — had NO counter, NO log and NO
        // event, so a block that went indefinite between epoch 2 and epoch 122 froze
        // 78 % of the carried state FOREVER while every downstream reading stayed
        // carry-blind (measured 2026-08-02, `runs/churn_base`, 63 sta / 3 h).  Three
        // weeks of campaigns were graded on that substrate.
        //
        // Reported by the run-end block in filter/NetworkTeardown.cpp; the per-epoch
        // per-epoch halves are NOT parked here: both epoch-status writers read them
        // straight off @ref NetworkEstimator::ddFoldStats (see NetworkCarryState).
        long long ddfold_epochs        = 0;  ///< accepted installs whose fold reached a factorization (the denominator).
        long long ddfold_folds         = 0;  ///< Σ blocks factored over those epochs.
        long long ddfold_fold_rows     = 0;  ///< Σ K over those blocks — the carried dimension offered to the data.
        long long ddfold_degen_blocks  = 0;  ///< Σ blocks that folded INERTLY (PD screen tripped ⇒ M = 0).
        long long ddfold_degen_rows    = 0;  ///< Σ K over those blocks — the carried dimension that took NO data.
        long long ddfold_degen_epochs  = 0;  ///< epochs with at least one degenerate block.
        long long ddfold_repairs       = 0;  ///< INC-1c eigen-clamp repairs applied (0 unless pd_safe).
        long long ddfold_repair_rows   = 0;  ///< Σ K over repaired blocks.
        long long ddfold_rebirths      = 0;  ///< INC-2a blocks rebirthed at install (0 unless pd_safe).
        long long ddfold_rebirth_rows  = 0;  ///< Σ carried rows re-seeded by those rebirths.
        long long ddfold_transitions   = 0;  ///< healthy → degenerate edges observed (invariant #7's DECOUPLING edge).
        long long ddfold_recoveries    = 0;  ///< degenerate → healthy edges observed (the block factored again on its own).
        double    ddfold_min_pivot_rel = 1.0;///< worst relative pivot d_min/d_max seen all run (≤ 0 ⇒ indefinite).
        double    ddfold_repair_worst  = 0.0;///< worst clamped |λ| / λ_max seen all run.  Dust (≤1e-12) is hygiene; >1e-6 is a NEW bug and is WARNed.
        long long ddfold_repair_loud   = 0;  ///< FOLDS whose worst clamp exceeded 1e-6·λ_max — the "this is not dust" population.  Dust is hygiene; a large clamp is a NEW bug and must scream, not be laundered.

        // --- Producer-AR Phase-2 (network.ambiguity_resolution) run tallies.
        // The diagnostic had NO run-end clause at all: its only output was one spdlog
        // line per epoch, so a run either had to be grepped or read as "AR did
        // something".  Standing invariant #4 in its ordinary form — `netar_epochs == 0`
        // on an ARMED run means the search never reached the ILS (min_epochs never
        // cleared, the covariance request threw, or the basis never had two undiffed
        // columns), and that is indistinguishable from a clean quiet run without a
        // denominator.  The distributions are kept as raw per-epoch samples rather
        // than running means because the reported statistic is a MEDIAN: ratio and
        // p_succ are both heavy-tailed (a degenerate epoch's ratio is 1e4 by
        // construction) and a mean of either is dominated by the epochs a grader
        // would throw away.  One double per epoch per series is ~120 kB at 24 h.
        long long netar_epochs     = 0;   ///< epochs the ILS search actually ran (denominator)
        long long netar_fix_epochs = 0;   ///< …of those, epochs the acceptance battery passed (full OR partial)
        long long netar_dd_fixed   = 0;   ///< Σ fixed DDs over the run (kept.size() summed)
        long long netar_rows       = 0;   ///< trace rows written — P1 for the trace card (0 with the trace armed = dead wiring)
        bool      netar_carry_aware = false; ///< covariance mode the search consumed (docs/SPEC_carry_aware_covariance.md §4) — part of the reading, not a footnote
        std::vector<double> netar_n_dd;      ///< per-epoch full DD-basis size
        std::vector<double> netar_n_search;  ///< per-epoch searched subset size (== n_dd when uncapped)
        std::vector<double> netar_ratio;     ///< per-epoch accepted-fix ratio
        std::vector<double> netar_p_succ;    ///< per-epoch accepted-fix bootstrap success rate
        std::vector<double> netar_med_sigma; ///< per-epoch median DD formal σ (cycles) — the convergence gauge
    };

    /// @internal The run context: one instance per runNetwork() invocation,
    /// passed by reference into the relocated lifecycle functions.  Non-copyable
    /// (owns move-only lazily-built helpers).
    struct NetworkRunContext {
        NetworkRunContext(const GnssConfig& config, const NetworkConfig& nc)
            : knobs(NetworkKnobs::snapshot(config, nc)) {
            // RF-NS: the estimability census latch is a COUNTER, so it is seeded
            // here (the one place the context is built) rather than snapshotted
            // into knobs — the dump site decrements it.  Default 6 == the
            // historical hard-coded constant, so an unset key is trace-identical.
            counters.spectrum_diag_budget = nc.trace_estimability_budget;
        }
        NetworkRunContext(const NetworkRunContext&) = delete;
        NetworkRunContext& operator=(const NetworkRunContext&) = delete;

        const NetworkKnobs     knobs;
        NetworkCarryState      carry;
        NetworkServiceCounters counters;
        NetworkBiasAdmission   admission;   // CB-ADM: latched census result (active=false ⇒ legacy path)
        /// EVENT HARNESS scripted-fault schedule + ledger (filter/NetworkInject.hpp).
        /// Deliberately OUTSIDE the warm-resume fingerprint and the snapshot: a leg
        /// resumes a converged state and then applies its OWN schedule — that two
        /// legs off one snapshot differ ONLY here is the whole design.
        /// armed == false (the default, network.inject absent) ⇒ every consumer of
        /// this object short-circuits and the run is byte-identical.
        NetworkInjectState     inject;
    };

    // ------------------------------------------------------------------
    // Decomposition INC-4: the relocated RTB-7 snapshot channel + carry
    // lifecycle, as free functions over the context (bodies verbatim in
    // NetworkRunContext.cpp; each opens with an alias block).  Spec deviation
    // from the Jul-4 plan recorded in the commit: free functions instead of
    // Application member fns — none needs Application state beyond config_,
    // which is passed explicitly (parameter names match the former use-sites).
    // The three-place rule: fingerprint terms live in
    // networkSnapshotFingerprint, restore in networkTryWarmResume, and the
    // write in networkWriteStateSnapshot — a new [snapshot] field touches all
    // three or documents its exemption at its NetworkCarryState declaration.
    // ------------------------------------------------------------------

    void networkSnapshotFingerprint(NetworkRunContext& ctx, const GnssConfig& config_,
                                    const std::vector<NetStation>& stations);

    class NetworkClockEmitRebase;   // fwd: include/products/NetworkClockEmitRebase.hpp
    class NetworkEmitRebase;        // fwd: include/products/NetworkEmitRebase.hpp
    class NetworkPbEmitFilter;      // fwd: include/products/NetworkPbEmitFilter.hpp
    // Resume FRAME restore: both seams take the CLK-R held-frame engine
    // (a runNetwork local) so its A_c / per-sat baselines travel with the snapshot.
    // nullptr ⇒ the engine's frame is neither captured nor restored.
    //
    // RS-5 (§21.10) adds the two PHASE-BIAS emit engines on exactly the same footing —
    // also runNetwork locals, also outputs-only, also nullptr ⇒ neither captured nor
    // restored — so all three cross-epoch emit layers travel with the snapshot instead of
    // cold-starting at every seam.
    void networkTryWarmResume(NetworkRunContext& ctx, const GnssConfig& config_,
                              const MultiRinexSync& sync, NetworkProductEmitter& emitter,
                              NetworkInvariantCarryDriver& carry_driver,
                              const std::shared_ptr<spdlog::logger>& log,
                              NetworkClockEmitRebase* clk_rebase = nullptr,
                              NetworkEmitRebase* emit_rebase = nullptr,
                              NetworkPbEmitFilter* pb_filter = nullptr);

    /**
     * @brief RS-8 (§21.16): apply the staged canonical-signal latch to the station models.
     *
     * The second half of @ref networkTryWarmResume for this card.  The resume seam
     * parses the snapshot and stages the @c CN rows in
     * @c NetworkCarryState::resume_canon_rows (it has no @c stations and must not grow
     * one); this seam — called from the driver immediately after it, where the roster
     * IS in scope — hands each station's rows to its @c PppModel::restoreCanonicalSignals
     * so the first-epoch guard treats the station as decided.  Counts stations /
     * entries into @c ctx.counters and drains the staging buffer.
     *
     * Unarmed, unresumed, or given a snapshot with no @c CN rows, it does nothing but
     * (in the armed, row-less case) warn ONCE — standing invariant #4.
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
    ///        lives in each station's @c PppModel.  RS-9 (§21.18) @c RL: the
    ///        code-bias linearization point is a property of each station's OWN state
    ///        vector (@c stations[i].sm->getStateVector()), which no other snapshot
    ///        record needs — the RC/SD/ER/PF rows are all plain copies out of
    ///        @c ctx.carry.  Read-only in both cases, and each group is written only
    ///        under its own knob (@c network.state_snapshot_signal_capture /
    ///        @c network.state_snapshot_rxcb_lin_capture).
    /// @param consumer Card CC (B): the closed-loop consumer engine (nullptr when the
    ///        harness is off).  Under @c network.closed_loop.state_snapshot each network
    ///        HISTORY copy (@c state_snapshot_keep_history) is accompanied by the
    ///        consumer's own RtStateSnapshot at
    ///        @c RealTimeEngine::consumerSnapshotHistoryPath (same @c .epNNNN suffix),
    ///        written only when the consumer's last processed epoch IS @p t (the
    ///        matching-tow invariant; a miss is counted in
    ///        @c counters.cl_snap_skipped and logged).  Read-only for the producer.
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

    /// RS-9 (§21.18): install the warm resume's stashed @c RL map on each station's
    /// @c StateInitializer, so the station's one-shot first-epoch code-bias seed takes
    /// the writer's linearization point instead of a median over ONE epoch of the resume
    /// epoch's code.  Called from @c runNetwork immediately after
    /// @c networkTryWarmResume — before any station has processed an epoch, which is the
    /// only window in which the one-shot latch has not yet closed.  A no-op (not even a
    /// map allocation) unless @c ctx.carry.rxcb_lin_restore is non-empty, so an unarmed
    /// or cold run reaches no new code at all.
    /// The APPLIED / UNCOVERED halves are read back at the run-end readout straight out
    /// of @c ctx.carry.rxcb_lin_sta_counts (@c networkTeardown has no station roster and
    /// needs none — the tallies are indexed, not named).
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
     * @brief eq34 reattach hygiene INC-2 — fold the open PIN BRIDGE rows.
     *
     * One soft shared-border prior per open bridge (@c ctx.carry.pin_bridge), at the
     * key's held level with information 1/σ².  Folded BEFORE the null-space rank
     * gate, exactly like the broadcast-anchored clock anchors and the Fix-A phase
     * hold, because for a bridged key this row IS the datum: the election deliberately
     * left it without a pin (it is a spanning-FOREST root), so the gate would
     * otherwise count its column as an unpinned gauge and withhold the epoch.
     *
     * Along the gauge direction the row realizes, the phase data carry NO information
     * — that is what makes it a gauge — so a soft row fully determines it.  In every
     * OTHER direction the same row is ~2–3 decades weaker than the data and is simply
     * outvoted, which is exactly the desired degradation: a bridge whose held value
     * has gone slightly stale (e.g. the one-epoch exposure across a clock-datum change,
     * dropped at the next election) biases nothing measurably.  The K2-style margin of
     * the row information over the rank-gate cutoff is measured once and logged.
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

    void networkInjectClockOscPrior(NetworkRunContext& ctx, EpochWorkspace& ws,
                                    NetworkRunTraces& traces, const GnssTime& t,
                                    NetworkEstimator& est,
                                    const std::vector<std::string>& registered_clock_sats,
                                    bool clk_model_warm, bool datum_changed,
                                    const std::set<char>& changed_consts,
                                    double pl_now_s, double dt, bool within_epoch_armed);

    /**
     * @brief Card LV-5 — reduce the rx-CB de-mean μ WITHIN the epoch.
     *
     * Runs SERIALLY between PASS A and PASS B.  Both terms of every carry
     * innovation are already known at that point — the carried absolute posterior
     * (last epoch's stash) and @c x_lin(cb) (the frozen linearization point) — so
     * the LV-2 mean can be formed over the stations present THIS epoch and applied
     * to THIS epoch's folds, instead of arriving one epoch late.
     *
     * The lag matters: its loop gain scales with how much carry information folds,
     * so it was harmless while the (defective) frame screen withheld two thirds of
     * the priors and limit-cycles the level once card LV-4 admits them all.
     *
     * Same race contract as the rest of the carry cycle: one serial writer, and the
     * parallel produce phase only reads @c ctx.carry.rx_cb_demean_mu.  Candidate
     * set = every present station with a carried entry inside the gap horizon; the
     * folded set can be marginally smaller (a column that drops out of the local
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

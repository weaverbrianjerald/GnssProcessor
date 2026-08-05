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
 * docs/network_decomposition_addendum_2026-07-16.md).  The run loop binds
 * `auto& X = ctx.carry.X;` / `const T X = ctx.knobs.X;` aliases under the
 * former local names, so every downstream use-site is textually unchanged.
 *
 * Three-place rule (RTB-7): a NetworkCarryState field marked snapshot-member
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

#include "filter/NetworkRunTypes.hpp"   // CarryPrior, SharedCarryCov, LocalCarry, ClockOsc, ShDevCarry
#include "filter/NetworkAnchorHygiene.hpp" // NetworkAnchorTallies (eq34 reattach hygiene)
#include "filter/NetworkSignalIntegrity.hpp" // SignalIntegrityArc + tallies (flicker/gap signal integrity)
#include "filter/NetworkIonoDwRelease.hpp"   // NetworkIonoDwReleaseTallies (iono-deweight release hygiene)
#include "filter/NetworkPrefitCensus.hpp"    // NetworkPrefitCensusTallies (prefit census / fuse / topology / recorder)
#include "filter/NetworkCarryVceApply.hpp"   // NetworkCarryVceApply (M2 v2 carry variance-factor state)
#include "filter/NetworkMdb.hpp"             // NetworkMdbEpoch + NetworkMdbTallies (Baarda MDB reliability)
#include "filter/NetworkBlockBaarda.hpp"     // NetworkBlockBaarda + BlockBaardaHit + tallies (BlockFormal LOS block census)
#include "filter/NetworkCongruence.hpp"      // NetworkCongruence + CongruenceHit + tallies (station congruence screen)
#include "filter/NetworkDatumGraph.hpp"      // NetDatumGraphSpectral + NetworkDatumGraphTallies (datum graph census)
#include "filter/NetworkMeasurementAdmission.hpp" // MeasurementAdmissionArc + tallies (product-referenced admission)
#include "filter/NetworkRhsAttrib.hpp"       // NetworkAttribFrame (INC-5 flight-recorder retention slot)
#include "filter/NetworkZStore.hpp"     // ArcKey (D5 inject/hold tag sets)
#include "filter/NetworkSbasis.hpp"     // NetworkSbasis::Plan (lifecycle-fn params)
#include "filter/NetworkBiasAdmission.hpp" // CB-ADM: latched W6 code-bias admission census (ctx.admission)
#include "logging/NetworkRunTraces.hpp" // FlagRate (B8 counters)
#include "common/PppUtils.hpp"          // eq34_ppp_utils

#include <Eigen/Dense>

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
    /// WL/UWL-zone knob consts stay in the run TU with their machinery
    /// (INC-9b).
    struct NetworkKnobs {
        double q_clock{};
        double q_pb{};
        double q_cb{};
        bool pl_on{};
        double pl_q_amb{};
        int pl_warmup{};
        double pl_max_gap_s{};
        double pl_var_floor{};
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
        bool zwd_on{};
        bool rx_cb_on{};      ///< rx-CB carry (SPEC_rx_cb_carry); loader guarantees code_datum_time_linked.
        bool rx_cb_datum{};   ///< RXCB-DATUM: zero-mean-over-receivers DCB datum (network.rx_code_bias_datum).
        double rx_cb_datum_sigma{};  ///< RXCB-DATUM loose physical-bound sigma in m (network.rx_code_bias_datum_sigma_m).
        double rx_cb_q{};   ///< rx-CB carry RW process noise (m²/s).
        bool rx_cb_demean{};  ///< Card LV-2 (SPEC_level_information_budget): inject η = λ·(innov − μ_c) and gate on |innov − μ_c|; loader guarantees rx_cb_on.
        int rx_cb_demean_min_sta{};  ///< Card LV-2: contributing-station floor below which μ_c FREEZES (thin-mean guard).
        bool rx_cb_demean_within{};  ///< Card LV-5 (SPEC_level_information_budget §12): reduce μ between PASS A and PASS B and fold against THIS epoch's value (no lag).  Loader guarantees rx_cb_demean.
        bool rx_cb_step_gate{};      ///< Card LV-4 (SPEC_level_information_budget §11): disable the frame-DEPENDENT inject screen; erase on a frame-INVARIANT step at the harvest.  Loader guarantees rx_cb_on.
        double rx_cb_step_k{};       ///< Card LV-4: k·σ of the invariant step screen.
        bool lv1_seed_frame{};       ///< Card LV-1 (SPEC_level_information_budget): reframe per-LOS iono seeds into the field's frame at allocation; loader guarantees estimate_iono_sh.
        int lv1_min_sats{};          ///< Card LV-1: minimum raw seeds for the per-station median offset.
        double zwd_q{};
        bool carry_vce{};
        bool vce_apply_on{};   ///< M2 v2 (docs/SPEC_carry_vce_apply.md): the LASER 5-95 variance-factor apply. SOLVE-AFFECTING; loader guarantees carry_vce.
        bool ura_on{};
        double ura_alpha{};
        bool ura_retro{};
        double ura_refloat_inflate{};
        bool raim_on{};
        double raim_k{};
        double raim_s0_floor{};
        int raim_min_sta{};
        int raim_arc_persist{};
        bool raim_maturity_gate{};
        int raim_maturity_epochs{};
        double raim_withhold{};
        bool raim_redun{};
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
        bool phase_datum_prioritized{};
        double phase_hold_sigma{};
        bool phase_hold_on{};
        double phase_hold_var{};
        // --- eq34 reattach hygiene (docs/SPEC_eq34_reattach_hygiene.md) ---
        // All four RESOLVED against the datum mode: the loader already disarms them
        // outside an anchored election, and `anchor_hygiene_on` is the single "does
        // this run allocate the arc-age map at all" predicate every site reads, so an
        // OFF run never touches the new state.
        bool   anchor_hygiene_on{};   ///< Any increment armed on an anchored (non-ensemble) phase datum — the master gate for the arc-age map and the hygiene seam.
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
        // --- flicker/gap signal integrity (docs/SPEC_flicker_gap_integrity.md) ---
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
        // --- iono-deweight RELEASE hygiene (docs/SPEC_deweight_release_hygiene.md) ---
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
        // --- redundancy / testability census (docs/SPEC_redundancy_census.md) ---
        // `redun_census_on` is the single "does this run enter the census seam at
        // all" predicate the driver reads, so an OFF run never calls into the
        // translation unit and pays nothing beyond this bool test.  The census is
        // MEASURE-ONLY, so there is no estimation precondition to resolve against
        // — only the two arming knobs.
        bool   redun_census_on{};     ///< Master: the stride OR the escalation trigger is armed.
        // --- Baarda reliability numbers / MDB (docs/SPEC_network_mdb_reliability.md) ---
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
        // --- prior-residual census (docs/SPEC_prior_residual_census.md) -------
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
        bool   prires_rxcb{};         ///< INC-2 receiver code-bias carry family armed.
        bool   prires_corrob{};       ///< INC-3 raw-observable corroboration join armed.
        double prires_report_sigma{}; ///< Per-row TRACE threshold in σ̂₀ units (nothing acts on it).
        int    prires_event_holdoff{};///< Per-cell PRIORRES event rate limit, in census epochs.
        // --- constraint-aware global σ̂₀ (docs/SPEC_sigma0_constraint_aware.md)
        bool   prires_s0g{};          ///< The global variance-component rider is armed (implies @ref prires_on).
        bool   prires_s0g_redun{};    ///< A per-row Baarda r_i actually exists for the data side (`raim` AND `raim_redundancy_normalize`).  FALSE ⇒ the r ≡ 1 fallback, flagged per epoch.
        // --- BlockFormal LOS block census (docs/SPEC_block_baarda.md) ---------
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
        // --- Station congruence screen (docs/SPEC_station_congruence.md) -------
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
        // (docs/SPEC_prefit_census_observability.md).  `pc_walk_on` is the single
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
        // --- datum graph census (docs/SPEC_datum_graph_census.md) ------------
        // `dg_on` is the single "does this run predict the S-basis dimension at
        // all" predicate, read at THREE sites: the driver's census call, the
        // top-of-epoch clear of the spectral hand-off, and the solve glue's
        // write of it.  All three must agree or the census would reconcile
        // against a stale epoch — which is precisely why it is one resolved
        // knob and not three independent config reads.
        bool      dg_on{};            ///< Master: network.datum_graph_census.
        long long dg_event_holdoff{}; ///< Per-identity DATUMGRAPH event rate limit, in processed epochs.
        // --- product-referenced measurement admission
        //     (docs/SPEC_measurement_admission.md).  Nothing to resolve against the
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
        // INC-4 re-entry vet (card §10).  `adm_revet` is already AND-ed with
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

    /// @internal Tier-1 cross-epoch carry/run state (decomposition INC-4) —
    /// the former runNetwork() persistent declaration stretch, verbatim
    /// (comments included; they are the authoritative per-field docs and the
    /// snapshot-exemption record).
    struct NetworkCarryState {
        std::vector<CarryPrior> prev_carry;                 // D2 stash from the last solve
        std::set<std::string> prev_clock_refloat_sats;      // Card 1d: clock-monitor flags at the last stash (hard-refloat mode skip-stashes the sat, so prev_carry cannot carry the flag)
        std::map<std::pair<SharedParamKind, std::string>, double> tu_reinit_hist;  // Card 1d: (kind, sat) → content time of the last reinit FOLD (multi-epoch-gap coverage)
        SharedCarryCov prev_carry_cov;                      // C1: dense covariance side-car of prev_carry (empty when OFF)
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
        // Card 3 (I4 longevity election): last epoch's per-(station idx, sat) arc
        // elevation, for the rising-vs-setting test that ranks NEW spanning-tree edges
        // by how long they will survive.  Rebuilt each epoch from sat_el; empty and
        // unread unless network.phase_datum_eq34_longevity_election.
        std::map<std::pair<size_t, std::string>, double> eq34_prev_el;
        // Card 3: the datum receiver r1 (pdr=) of the LAST EMITTED contract, so a root
        // handover triggers an S-line re-broadcast (advanced only at emit, like the
        // pivot baselines).  Empty on every non-eq34 run.
        std::string eq34_emitted_pdr;
        // Card 3: wavelength provider for the pin-to-round re-pin target (λ·round(float/λ));
        // built lazily on first eq34 use.  Null on every non-eq34 run.
        std::unique_ptr<PppUtils> eq34_ppp_utils;
        // Card 3 run-scope observability counters, logged once in the run-end producer
        // summary (the per-epoch reref/erase lines are debug-only, which left the
        // Jul-10 field A/B blind to the reref volume).  Untouched on non-eq34 runs.
        long long eq34_reref_total = 0, eq34_erase_total = 0;
        long long eq34_shift_total = 0;   // carry values S-transformed (v2-shift) over the run
        long long eq34_gap_repin_total = 0;   // I2: tree edges resumed from a carry-alive gap (re-pinned to their pre-gap integer instead of re-leveling from 0)
        int eq34_mismatch_epochs = 0;
        // Card 3 (I2 extend-only): tree edges retained across the current gap because
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

        // --- eq34 REATTACH HYGIENE (docs/SPEC_eq34_reattach_hygiene.md) --------
        // Every member below stays EMPTY unless ctx.knobs.anchor_hygiene_on, so an
        // OFF run allocates nothing and the election is structurally unchanged.
        // None of it is serialized: the warm-resume rule is a SEED (see
        // anchor_age_resume_seed), not a restore — adding an age map to the snapshot
        // would change the format for every run, armed or not (the sd_ewl exemption
        // precedent), and sub-min_arc_ep fidelity across a resume is explicitly
        // deferred (spec §7).
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
        // --- FLICKER/GAP SIGNAL INTEGRITY (docs/SPEC_flicker_gap_integrity.md) ---
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

        // --- PRODUCT-REFERENCED MEASUREMENT ADMISSION
        //     (docs/SPEC_measurement_admission.md) ---
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
        // Card 6 (RTF-2, network.apriori_source = brdc_rts): the broadcast-ephemeris IODE
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
        // Card 6: sats whose eph rolled but whose PRIOR version was already evicted from
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
        // Card 2 (network.code_datum_time_linked): the code-bias datum's previous-epoch
        // solved values, keyed "<constellation>|<signal>" -> satellite -> value.  The
        // veteran-referenced sum-to-zero RHS and the IF-pin RHS read this at gauge
        // construction; it is cleared + rebuilt from the solved shared vector at the D2
        // harvest (kept epochs only — so a departed satellite is dropped and its return
        // re-seeds IT alone).  NOT snapshotted (a warm resume re-seeds, signalled).
        std::map<std::string, std::map<std::string, double>> code_bias_prev_;
        // Card 2: the IF-pin reference pair (sat -> {sig1,sig2}) that produced the values
        // in code_bias_prev_ — the SAME kept-epoch lifecycle (rebuilt at the D2 harvest
        // from the scratch below).  A pair CHANGE between kept epochs re-references the
        // emitted clock↔code split by the satellite's inter-channel bias (the band-1
        // C1C↔C1W flip), so the IF-pin RHS re-seeds and the disc bumps even when the new
        // pair's signals both have previous values.  NOT snapshotted.
        std::map<std::string, std::pair<std::string, std::string>> code_bias_ifpin_pair_;
        // Card 2: this epoch's selected IF-pin pair per satellite (valid pairs only),
        // captured in the main pass and copied into code_bias_ifpin_pair_ at the harvest
        // (kept epochs only — mirrors code_bias_prev_, so a pair recorded on a withheld
        // epoch never advances the comparison baseline).  Cleared each epoch.
        std::map<std::string, std::pair<std::string, std::string>> this_epoch_cb_ifpin;
        // Card 2: satellites that RE-SEED this epoch (IF-pin split or a cold sum-to-zero
        // channel).  A per-epoch scratch (cleared each epoch) populated in the main pass and
        // MERGED into cb_reseed_disc_pending only at the emit site — which every whole-epoch
        // withhold path `continue`s before reaching.  So a transient re-seed on a since-
        // withheld epoch (the frame reverts by the next kept epoch) never enters the pending
        // set and cannot fire a bump with no emitted value step (the emit-only discipline
        // this_epoch_if_pin_sel / last_if_pin_sel use), WITHOUT an erase that would destroy a
        // bump legitimately deferred for a satellite re-seeded while pruned from the emission.
        std::set<std::string> this_epoch_cb_reseed;
        // Card 2: satellites whose code-bias split has EVER been harvested — a later
        // re-seed of one of these is a RETURN (disc-bump), not a genuine first appearance
        // (seed silently).  Accumulates for the whole process; NOT snapshotted.
        std::set<std::string> code_bias_sat_seen_;
        // Card 2: a warm resume starts code_bias_prev_/seen EMPTY, but the pre-snapshot
        // product was emitted, so every first post-resume re-seed IS a re-reference the
        // consumer must be told about.  Set true in the resume block; makes re-seeds bump
        // regardless of code_bias_sat_seen_ for the rest of a resumed run (conservative —
        // harmless on a genuinely-new satellite's first emit).  NOT snapshotted.
        bool code_bias_datum_resumed_ = false;
        // Card 2: satellites to disc-bump for a time-linked datum RE-SEED (a no-veteran
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
        // Per-station ZWD carry (B9): persistent across epochs, keyed by station index
        // (ZWD is one-per-station).  Inert unless network.carry_zwd.
        std::map<int, LocalCarry> zwd_carry;
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
        /// rx-CB carry (docs/SPEC_rx_cb_carry.md): carried receiver code-bias
        /// posteriors, keyed (station index, CB state key).  Harvested OUTSIDE
        /// the carry_warm bracket (own gate — must work on the
        /// carry_invariant/no-leveling lineage).  Inert unless
        /// network.carry_rx_code_bias.
        std::map<std::pair<int, std::string>, LocalCarry> rx_cb_carry;
        // rx-CB carry run counters (armed-only run-end line).
        long long rx_cb_injected_total = 0, rx_cb_innov_skips_total = 0,
                  rx_cb_harvested_total = 0;
        /// Card LV-2 (docs/SPEC_level_information_budget.md): the information-
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
        /// LV-2: THIS epoch's reduction per family, retained only so the caller can
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
        // Card 4 (M1) D5: arcs the innovation gate flagged in the PREVIOUS epoch, to be
        // treated as slipped THIS epoch (the estimation-side fine-slip re-tap runs a post-solve
        // detection ⇒ it feeds the NEXT epoch's slip funnel).  Persists across epochs (the
        // per-epoch carry_slipped_ambs is rebuilt each epoch).  Empty unless carry_innov_gate.
        std::set<std::pair<int, std::string>> carry_innov_pending;
        long long carry_innov_flags_total = 0, carry_innov_retaps_total = 0;
        long long carry_innov_map_miss = 0;   // D5 flag→ambiguity reverse-map misses (should stay 0)
        // Card 4 (M1) D5 v4 (network.carry_invariant_innov_retap_only): the subset of
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

        // --- PD-safe DD fold (docs/SPEC_pd_safe_dd_fold.md) INC-0 cross-epoch state.
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

        // --- INC-1 PREFIT census, per-epoch scalars ---------------------------
        // (docs/SPEC_prefit_census_observability.md).  Filled by
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
        // Iono-2 v3 de-mean (SPEC_iono2_mechB_v3): epochs the global de-mean
        // applied, station-mean removals, and Σ|g| for the run-end mean |g|.
        long long shdev_demean_epochs_total = 0, shdev_demean_sta_total = 0;
        double    shdev_demean_abs_g_sum = 0.0;
        // Iono-2 v3k: D5-only re-taps that KEPT the level (variance re-tap) instead
        // of erasing.  shdev_slip_resets_total then counts actual erases only.
        long long shdev_d5_keeps_total = 0;

        // --- IONO-DEWEIGHT RELEASE HYGIENE (docs/SPEC_deweight_release_hygiene.md) ---
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
        /// INC-3b: consecutive epochs a quarantined LOS has gone with NO harvested
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

        // --- redundancy / testability census (docs/SPEC_redundancy_census.md) ---
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

        // --- Baarda MDB (docs/SPEC_network_mdb_reliability.md) ----------------
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

        // --- M2 v2 carry variance FACTOR (docs/SPEC_carry_vce_apply.md) -------
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

        // --- PRIOR-RESIDUAL census (docs/SPEC_prior_residual_census.md) --------
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
        /// Constraint-aware global σ̂₀ (docs/SPEC_sigma0_constraint_aware.md) —
        /// the three epoch-status scalars, stamped by the same emission seam and
        /// gated by the same @ref prires_ep_tow stamp, so an epoch the rider did
        /// not drain leaves them BLANK rather than repeating stale numbers.
        bool      ep_s0g_formed = false;///< σ̂₀²_global was defined this epoch (Σr > 0); false ⇒ the column is blank, never 0.
        double    ep_s0g_sigma0 = 0.0;  ///< σ̂₀_global (the SQRT — directly comparable to the median σ̂₀ it is measured against).
        bool      ep_s0g_tension_def = false; ///< the tension ratio had a data side to divide by.
        double    ep_s0g_tension = 0.0; ///< Σw·ê² / Σw·v² — THE HEADLINE column.
        int       ep_s0g_flags   = 0;   ///< @c NetworkPriorResidual::GlobalFlag bitmask: in-band degradation, never silent.

        // --- BLOCKFORMAL LOS block census (docs/SPEC_block_baarda.md) ---------
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

        // --- STATION CONGRUENCE screen (docs/SPEC_station_congruence.md) -------
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
        // (docs/SPEC_prefit_census_observability.md).  [snapshot: EXEMPT] — every
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
        /// Written from the EXCEEDANCE, deliberately upstream of the event-class
        /// mask and the per-cell holdoff: the census card's own principle is that
        /// detection must not depend on whether anyone is listening on the other
        /// side, and a trigger throttled by a diagnostic rate limiter would go
        /// quiet exactly when a cell is persistently bad.
        ///
        /// CODE rows only.  A phase row's prefit innovation is arc-specific (it
        /// carries that arc's ambiguity linearization point) and its station median
        /// is not a common mode at all — the census card measured 608 of 665 phase
        /// events on one leg as non-findings — so a phase-driven trigger would be a
        /// false-positive generator wired to an admission verdict.  It is also the
        /// same CODE-only-by-principle rule the admission statistic itself obeys.
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

        // --- datum graph census (docs/SPEC_datum_graph_census.md) -------------
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
    /// tallies (decomposition INC-4) — verbatim relocation.
    struct NetworkServiceCounters {
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
        // eq34 reattach hygiene (docs/SPEC_eq34_reattach_hygiene.md) run tallies —
        // every field documented on the struct.  Bumped only when
        // ctx.knobs.anchor_hygiene_on, and reported by the run-end block in
        // filter/NetworkTeardown.cpp.
        NetworkAnchorTallies hyg;
        // Flicker/gap signal integrity (docs/SPEC_flicker_gap_integrity.md) run
        // tallies — every field documented on the struct.  Bumped only when
        // ctx.knobs.sig_integrity_on, and reported by the run-end block in
        // filter/NetworkTeardown.cpp.  `demoted_rows` is summed there from the
        // per-station NetStation::si_rows_demoted counters (the fold runs inside the
        // parallel produce, so the ROW tally cannot live here).
        NetworkSignalIntegrityTallies sig;
        // Iono-deweight release hygiene (docs/SPEC_deweight_release_hygiene.md) run
        // tallies — every field documented on the struct.  Bumped only when
        // ctx.knobs.dw_hygiene_on, and reported by the run-end block in
        // filter/NetworkTeardown.cpp.
        NetworkIonoDwReleaseTallies dwr;
        // Product-referenced measurement admission
        // (docs/SPEC_measurement_admission.md) run tallies — every field documented
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
        // RTA-3: per-run solution-generation id, minted at the FIRST emit from that
        // epoch's GPS time (deterministic under replay — the id is serialized per
        // epoch; different across live restarts, which first-emit later).  0 = unset.
        uint32_t generation_id = 0;
        // RTA-5: product-time (s) of the last written S contract line (the deferred v2
        // header counts as the first); -1 until the contract writer opens.
        double last_contract_emit_s = -1.0;
        size_t n_emitted = 0, n_skipped = 0, n_gate_withheld = 0, n_runaway_epochs = 0;
        // RTB-6: operator health-record state (network.health_interval_s > 0).
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
        // Phase 2.2: epochs whose EMISSION the SH health gate withheld.  NOT a
        // subset of n_skipped — the solve/carry/QC all ran (emission-only gate),
        // so these epochs are counted nowhere else.
        size_t n_sh_gate_withheld = 0;
        // Prioritized-datum drop-and-heal observability: epochs where the gate threw and the
        // near-null phase-bias soft-anchor recovered it, and total columns anchored.  A run
        // that heals every epoch "succeeds" silently otherwise (the heal-success log is debug).
        size_t n_healed_epochs = 0, n_healed_cols = 0;
        // Card PA2 — the SAME quantities counted on EVERY fired heal, recovered or not.
        // The pair above is success-only, which makes a heal that fires every epoch and
        // never rescues the gate read as "0 healed" — measured on the 305 co-gated arm,
        // which logged "drop-and-heal failed" on all 6 epochs while reporting
        // n_healed_epochs = 0.  That is precisely the inert-looking-but-live wiring
        // standing invariant #4 exists to catch, so the attempt is counted separately:
        // attempts > 0 with recoveries == 0 is the signature of a heal that is aiming at
        // the right columns but cannot resolve the residual alone (Card PA's own
        // insufficiency criterion, PA5).
        size_t n_heal_attempt_epochs = 0, n_heal_attempt_cols = 0;
        // Card KP (network.cb_kaula_prior): epochs on which the admitted-OSB
        // physical-bound prior was folded, and the cumulative column count.
        // Standing invariant #4 — an armed KP run with n_kp_epochs == 0 means the
        // prior never reached a solve (no registered code columns, or the W6 block
        // never ran), which no amount of clean-looking log output would reveal.
        size_t n_kp_epochs = 0, n_kp_cols = 0;
        bool   kp_margin_logged = false;   ///< one-shot latch for the K2 margin check
        // Card 1b (network.carry_coupling_joint_whiten): #station-epochs whose joint
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
        int n_cpl_row_dump = 12;   // Card 1d debug: one-shot assembled coupling-row dump budget
        int n_cpl_solve_dump = 8;  // Card 1d debug: one-shot full-precision solve-checksum budget
        // Coverage-threshold union gate (network.phase_datum_min_stations): epochs on
        // which >=1 thin column was excluded, and the cumulative column-epoch count.
        size_t n_gated_epochs = 0, n_gated_key_epochs = 0;
        size_t n_perconst_partial_epochs = 0;     // RTA-10: epochs published PARTIALLY (σ̂0 carve-out pruned >=1 constellation)
        size_t n_perconst_diffuse_published = 0;  // RTA-10: epochs published in FULL although the global σ̂0 tripped (every constellation individually clean — the diffuse cross-constellation edge of the per-const approximation)
        FlagRate fr_if_slip;    // T1 per (station,sat,pair) IF-phase sample   k=if_phase_td_k_mad
        FlagRate fr_we_trigger; // T2 per warm within-epoch epoch: σ̂0_first>trig k=sigma0_trigger
        FlagRate fr_raim_cell;  // T3 per (sat,station) standardized residual   k=raim_k
        FlagRate fr_raim_sat;   // T3 consensus per spiked sat (>=min_sta)       k=raim_k
        FlagRate fr_attrib;     // T4 per median-polish SAT column: eff/σ̂0>k & σ̂0>trigger (B8/P1-4)
        FlagRate fr_clk_model;  // T5 per sat clock-model NIS (own, !ext)        k=clock_model_k
        FlagRate fr_clk_mon;    // T6 per sat clock-monitor abs/jump (own, !ext) absolute bounds
        double max_dur_anchor_s = 0.0;
        bool   max_dur_anchored = false;
        int    spectrum_diag_budget = 6;    // estimability spectrum dump: first N gate evaluations (was one-shot; a rows-epoch needs a later firing than epoch 1)

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

        // Card H2 (docs/SPEC_beidou_w6_datum.md §5) — the measurable cost of the
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

        // Card DG6 (docs/SPEC_beidou_w6_datum.md §2) — rank-gate calibration
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

        // --- redundancy / testability census (docs/SPEC_redundancy_census.md) ---
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

        /// Baarda reliability / MDB tallies (docs/SPEC_network_mdb_reliability.md).
        /// Every field documented on the struct; reported by @c networkMdbReport,
        /// which lives next to the aggregation that produced the numbers so the
        /// counters and the sentences interpreting them cannot drift apart.
        NetworkMdbTallies mdb;

        /// BlockFormal LOS block census tallies (docs/SPEC_block_baarda.md).
        /// Named `bb`, NOT `block`: `ctx.counters.bb` is the run-total POD and
        /// `ctx.carry.bb` is the accumulator that produced it — two different
        /// objects with one short name each, deliberately spelled the same so a
        /// reader never has to remember which card owns which prefix.  It is NOT
        /// the redundancy census's `redun_*` block: that one counts ROWS, this
        /// one counts BLOCKS, and their denominators are not interchangeable.
        NetworkBlockBaardaTallies bb;

        /// Station congruence screen tallies (docs/SPEC_station_congruence.md).
        /// Named `cg` on the same convention as `bb`: `ctx.counters.cg` is the
        /// run-total POD and `ctx.carry.cg` is the accumulator that produced it.
        /// It counts STATION-EPOCHS, where `bb` counts BLOCKS and the redundancy
        /// census's `redun_*` block counts ROWS — three denominators that are not
        /// interchangeable and must never be divided into one another.
        NetworkCongruenceTallies cg;

        // --- PRIOR-RESIDUAL census (docs/SPEC_prior_residual_census.md) --------
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
        double    prires_max_t = 0.0;           ///< the worst |t| over the whole run, any family, any label.
        std::string prires_max_id;              ///< its (family/label/station/sat/signal @tow) identity.
        double    prires_wall_ms = 0.0;         ///< run-total wall inside the emission seam (the §5 cost claim; the per-row addRow cost rides the carry harvests and is not separable).

        // --- CONSTRAINT-AWARE GLOBAL σ̂₀ (docs/SPEC_sigma0_constraint_aware.md) -
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
        /// ledger / flight-recorder tallies
        /// (docs/SPEC_prefit_census_observability.md).  Every field documented on
        /// the struct; reported by the run-end block in filter/NetworkTeardown.cpp.
        /// Standing invariant #4 in its sharpest form for this card: the census
        /// is a TRIPWIRE, and a tripwire that never fires looks exactly like a
        /// tripwire that was never connected — only `epochs` / `rows` /
        /// `rec_consults` separate them.
        NetworkPrefitCensusTallies pc;

        /// Datum graph census run tallies (docs/SPEC_datum_graph_census.md).
        /// Standing invariant #4 with an extra twist this card owns: the EXPECTED
        /// reading of tier 1 on a healthy global roster is k = 1 everywhere with
        /// zero predictions, which is byte-for-byte what a seam that never ran
        /// produces — only `epochs` vs `epochs_quiet` tells the two apart, and the
        /// teardown line prints both.
        NetworkDatumGraphTallies dg;

        // --- PD-safe invariant-DD fold (docs/SPEC_pd_safe_dd_fold.md) run tallies.
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
    };

    /// @internal The run context: one instance per runNetwork() invocation,
    /// passed by reference into the relocated lifecycle functions.  Non-copyable
    /// (owns move-only lazily-built helpers).
    struct NetworkRunContext {
        NetworkRunContext(const GnssConfig& config, const NetworkConfig& nc)
            : knobs(NetworkKnobs::snapshot(config, nc)) {}
        NetworkRunContext(const NetworkRunContext&) = delete;
        NetworkRunContext& operator=(const NetworkRunContext&) = delete;

        const NetworkKnobs     knobs;
        NetworkCarryState      carry;
        NetworkServiceCounters counters;
        NetworkBiasAdmission   admission;   // CB-ADM: latched census result (active=false ⇒ legacy path)
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

    void networkTryWarmResume(NetworkRunContext& ctx, const GnssConfig& config_,
                              const MultiRinexSync& sync, NetworkProductEmitter& emitter,
                              NetworkInvariantCarryDriver& carry_driver,
                              const std::shared_ptr<spdlog::logger>& log);

    void networkWriteStateSnapshot(NetworkRunContext& ctx, const GnssConfig& config_,
                                   const GnssTime& t, double pl_now_s,
                                   NetworkProductEmitter& emitter,
                                   const NetworkInvariantCarryDriver& carry_driver,
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
     * @brief Card LV-5 — reduce the rx-CB de-mean μ WITHIN the epoch
     *        (docs/SPEC_level_information_budget.md §12).
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

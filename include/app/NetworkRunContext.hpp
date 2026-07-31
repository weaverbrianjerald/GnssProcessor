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
        double zwd_q{};
        bool carry_vce{};
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
        double eq34_rpb_prior_sigma{};
        double eq34_rpb_prior_w{};
        double eq34_rpb_q{};
        bool eq34_shift_on{};
        bool eq34_longevity{};
        bool eq34_extend_only{};
        bool phase_datum_prioritized{};
        double phase_hold_sigma{};
        bool phase_hold_on{};
        double phase_hold_var{};

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

        // --- SPEC_day129_frame_guards P1-1: per-epoch carry churn -------------
        // The Card-4 counters above are RUN-CUMULATIVE (they mirror
        // carry_driver.counters(), which never resets), so the run-end log line is
        // the only place the churn is visible.  The trigger question — "did the
        // re-tap count SPIKE at the collapse epoch?" — needs the per-epoch series.
        // NetworkInstallCarry differences the cumulative totals against the
        // previous epoch's snapshot; the emit site copies the deltas into the
        // epoch-status row.  Pure trace: nothing reads them back.
        long long carry_prev_born = 0, carry_prev_slipkills = 0,
                  carry_prev_innov_flags = 0, carry_prev_innov_retaps = 0;
        long long ep_chord_births = 0, ep_slip_refloats = 0,
                  ep_d5_flags = 0, ep_d5_retaps = 0;

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

    void networkHarvestLocalCarries(NetworkRunContext& ctx, const GnssConfig& config_,
                                    NetworkEstimator& est, const Eigen::VectorXd& x_s,
                                    std::vector<NetStation>& stations,
                                    const std::vector<int>& est_index_of,
                                    const std::vector<StationAssembly>& asm_res,
                                    const NetworkSbasis::Plan& plan,
                                    const SphericalHarmonicIonosphere* sh_model,
                                    const SphericalHarmonicIonosphere* sh_model2,
                                    NetworkCarryVce& carry_vce_est, bool carry_warm,
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

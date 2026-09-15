/**
 * @file NetworkSolveGlue.cpp
 * @brief Implementation of @ref networkSolveGlue, the SB_SOLVEGLUE block relocated out
 *        of Application::runNetwork at decomposition INC-9b (2026-07-22).  The datum /
 *        solve / within-epoch-RAIM body is a byte-copy of the former block (dedent-8);
 *        the only edits are the four `continue;` -> SkipEpoch returns and the result
 *        epilogue (see the header banner).  The alias preamble byte-copies the run-TU
 *        nc / ctx / traces / ws / setup aliases + the emitEpochStatus / emitEvent
 *        lambdas so the body reads the original names.
 */

#include "filter/NetworkSolveGlue.hpp"

#include "app/NetworkRunContext.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"   // Phase 2.2 SH gate: coeffs()
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "filter/NetworkChainDatum.hpp"
#include "filter/NetworkClockAnchor.hpp"   // Card Q3 guarded temporal clock anchor
#include "filter/NetworkDatumGraph.hpp"   // datum graph census: the per-epoch spectral hand-off
#include "filter/NetworkFrameDiag.hpp"   // invariant #7: networkDatumInfoRatio
#include "filter/NetworkHandoverStransform.hpp"   // queue 1(b): the clock-pivot handover S-transform
#include "filter/NetworkPhaseDatum.hpp"
#include "filter/NetworkPivotHandover.hpp"
#include "filter/NetworkPrefitCensus.hpp"   // H-rider: networkPrefitSigma0 (census-fed H σ̂₀)
#include "filter/NetworkRelevel.hpp"
#include "filter/NetworkRhsAttrib.hpp"   // INC-5 flight-recorder trip latch + inline A4 dump
#include "filter/NetworkRobustStats.hpp"
#include "filter/NetworkSbasis.hpp"
#include "filter/StateKeySchema.hpp"
#include "products/LiveEphemerisStore.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "math/TimeUtils.hpp"

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

NetworkSolveGlueResult networkSolveGlue(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    NetworkEstimator& est,
    EpochWorkspace& ws,
    const SyncedEpoch& se,
    const std::vector<StationAssembly>& asm_res,
    std::vector<int>& est_index_of,
    const std::vector<bool>& present,
    const std::set<std::string>& shared_phase_keys,
    const std::vector<std::string>& registered_clock_sats,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::vector<std::string>& col_desc,
    int n_clock,
    int n_pb,
    int n_cb,
    int n_present,
    int total_pins,
    const std::set<std::string>* carry_we_drop_sats,
    const std::map<int, std::set<std::string>>* carry_we_drop_arcs,
    bool carry_warm,
    bool within_epoch_armed,
    double dt,
    double pl_now_s,
    double clk_anchor_sigma,
    const std::function<void()>& buildEnsembleGauges,
    const std::function<void()>& buildCarryDdPriors) {
    // --- Alias preamble (byte-copies of the run-TU alias block). ---
    const auto& nc = config_.getNetworkConfig();
    const bool brdc_rts = setup.brdc_rts;
    auto& sh_model = setup.sh_model;
    auto& sh_model2 = setup.sh_model2;   // Iono-2 dual-layer second shell (null unless armed)
    auto& stations = setup.stations;
    NetworkProductEmitter& emitter = *setup.emitter;
    auto& product_writer = setup.product_writer;
    auto& consumer = setup.consumer;
    const double q_clock = ctx.knobs.q_clock;        // m²/s
    auto& trace                = traces.trace;
    auto& events_trace         = traces.events_trace;
    auto& prev_carry = ctx.carry.prev_carry;
    auto& prev_pivot = ctx.carry.prev_pivot;
    auto& last_pivot = ctx.carry.last_pivot;
    auto& held_clock_ref = ctx.carry.held_clock_ref;
    auto& this_epoch_if_pin_sel = ctx.carry.this_epoch_if_pin_sel;
    auto& this_epoch_if_pin_real = ctx.carry.this_epoch_if_pin_real;
    auto& this_epoch_gf_pin_real = ctx.carry.this_epoch_gf_pin_real;
    auto& family_census = ctx.carry.family_census;
    auto& this_epoch_anchor = ctx.carry.this_epoch_anchor;
    auto& prev_clk = ctx.carry.prev_clk;
    auto& clock_osc = ctx.carry.clock_osc;
    auto& relevel_iode = ctx.carry.relevel_iode;
    auto& eph_cutover_disc_pending = ctx.carry.eph_cutover_disc_pending;
    auto& basis_relatch_disc_pending = ctx.carry.basis_relatch_disc_pending;
    auto& code_bias_prev_ = ctx.carry.code_bias_prev_;
    auto& code_bias_ifpin_pair_ = ctx.carry.code_bias_ifpin_pair_;
    auto& this_epoch_cb_ifpin = ctx.carry.this_epoch_cb_ifpin;
    auto& this_epoch_cb_reseed = ctx.carry.this_epoch_cb_reseed;
    auto& code_bias_sat_seen_ = ctx.carry.code_bias_sat_seen_;
    auto& code_bias_datum_resumed_ = ctx.carry.code_bias_datum_resumed_;
    auto& local_carry = ctx.carry.local_carry;
    auto& zwd_carry = ctx.carry.zwd_carry;
    const bool   pl_on         = ctx.knobs.pl_on;
    const int    pl_warmup     = ctx.knobs.pl_warmup;
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond; // Card 1: conditional local carry (coupling rows) — requires fullcov_on (enforced at load)
    const bool   carry_cpl_joint = ctx.knobs.carry_cpl_joint; // Card 1b: JOINTLY whiten the coupling rows (departed cols → M_w) — requires carry_cpl_cond (enforced at load)
    const bool   ura_retro     = ctx.knobs.ura_retro;             // B1: retro-bump flagged SATELLITE(s) on next emit
    const double ura_refloat_inflate = ctx.knobs.ura_refloat_inflate; // B1: URA multiplier post-refloat
    const double raim_k        = ctx.knobs.raim_k;      // standardized post-fit residual flag threshold
    const double raim_s0_floor = ctx.knobs.raim_s0_floor; // floor on the robust a-posteriori σ̂0 (1.0 default; lower lets σ̂0 < 1)
    const int    raim_min_sta  = ctx.knobs.raim_min_sta; // cross-station consensus count
    const double raim_withhold = ctx.knobs.raim_withhold; // A4: >0 withholds (not emits) an epoch whose post-adaptation σ̂0 exceeds this ceiling; 0 = off
    const bool   raim_redun    = ctx.knobs.raim_redun; // B2: Baarda per-row r_i normalization of the RAIM residual
    const bool   clk_mon_couple = ctx.knobs.clk_mon_couple;  // also refloat RAIM-flagged sats' clocks (opt-in)
    const bool   pivot_vr_signal = ctx.knobs.pivot_vr_signal;  // A1: also signal a vanish-return pivot change
    const bool   emit_clock_disc = ctx.knobs.emit_clock_disc;            // A2: clock disc channel + per-constellation bump scoping
    const bool   sticky_pivot    = ctx.knobs.sticky_pivot;              // RTA-1: keep the incumbent clock pivot while it stays registered
    const bool   clock_sum_to_zero = ctx.knobs.clock_sum_to_zero;    // anchor-free per-constellation clock gauge (Σδtˢ=0) instead of a pivot pin — no pivot/handover machinery
    const bool   clock_brdc_anchored = ctx.knobs.clock_brdc_anchored; // brdc_rts: per-sat broadcast anchors ARE the clock gauge (pre-gate) — no pivot machinery AND no sum-to-zero membership wander
    const bool   clock_signal_basis_hold = ctx.knobs.clock_signal_basis_hold; // W6: HOLD each sat's IF-pin reference pair fixed across epochs (time-consistent δtˢ↔dˢ split) instead of re-selecting from each epoch's coverage — inert without estimate_sat_code_bias
    const bool   cb_time_linked  = ctx.knobs.cb_time_linked;      // Card 2 (W6): impose the code-bias datum (IF pin + per-(const,signal) s2z) as a TIME datum — pin the CHANGE of the previous epoch's solved values, not the level to zero; mutually exclusive with clock_signal_basis_hold (load-validated)
    const bool   scoped_handover = ctx.knobs.scoped_handover;   // RTA-2: per-constellation scoping of the INTERNAL handover response
    const bool   perconst_on       = ctx.knobs.perconst_on;          // RTA-10: per-constellation carve-out of the A4 σ̂0 withhold
    const int    perconst_min_rows = ctx.knobs.perconst_min_rows;   // RTA-10: min σ̂0-pool rows for a constellation to be judgeable
    const bool   emit_epoch_status = ctx.knobs.emit_epoch_status;     // RTA-10: per-epoch H heartbeat in the product file (incl. withheld epochs)
    const bool   clk_model_on   = ctx.knobs.clk_model_on;
    const bool   emit_clock_rate_on = ctx.knobs.emit_clock_rate_on;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& n_emitted = ctx.counters.n_emitted;
    auto& n_skipped = ctx.counters.n_skipped;
    auto& n_gate_withheld = ctx.counters.n_gate_withheld;
    auto& n_sigma0_withheld = ctx.counters.n_sigma0_withheld;
    auto& n_healed_epochs = ctx.counters.n_healed_epochs;
    auto& n_healed_cols = ctx.counters.n_healed_cols;
    auto& n_cpl_solve_dump = ctx.counters.n_cpl_solve_dump;
    auto& n_perconst_diffuse_published = ctx.counters.n_perconst_diffuse_published;
    const bool trace_flag_rates = ctx.knobs.trace_flag_rates;
    auto& spectrum_diag_budget = ctx.counters.spectrum_diag_budget;
    const bool phase_datum_ensemble    = ctx.knobs.phase_datum_ensemble;
    const bool phase_datum_eq34        = ctx.knobs.phase_datum_eq34;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    const auto& t = est.sectionTimers();
    // H-rider (edge-queue §1.6 item 3a): the census-fed H σ̂₀, computed ONCE per
    // epoch, PRE-solve, so every H site downstream — including the pre-solve
    // observability-gate withhold just below — reads the same value.  The inputs
    // (asm_res row stacks) are frozen at assembly; MEASURE-ONLY.  0.0 = the H
    // grammar's "unavailable" (empty pool), and the reset keeps a knob-off run
    // carrying an inert 0 rather than a stale value.
    ctx.carry.ep_h_sigma0_census = 0.0;
    if (ctx.knobs.h_sigma0_census) {
        ctx.carry.ep_h_sigma0_census = networkPrefitSigma0(
            ctx, stations, est, asm_res, est_index_of, ctx.knobs.h_sigma0_class_mask);
        if (ctx.carry.ep_h_sigma0_census > 0.0) ++ctx.carry.h_sig0_census_nonzero;
    }
    auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
        // Measurement-admission census (docs/SPEC_measurement_admission.md INC-3):
        // stamped HERE rather than at the four withhold sites below, because the
        // census is MEASURE-ONLY and a withheld epoch is exactly the epoch a reader
        // most wants the absolute |z| population for.  The screen runs pre-assembly,
        // so by this point ctx.carry holds this epoch's numbers; the TOW compare
        // leaves the columns blank on an epoch it did not screen.
        // PD-safe DD fold (docs/SPEC_pd_safe_dd_fold.md INC-0): same reasoning — the PD
        // screen's verdict is exactly what a reader wants on a WITHHELD epoch, since a
        // degenerate carry block is a plausible cause of one.
        //
        // Read STRAIGHT off the kernel, NOT through a ctx.carry stamp.  The stamp is
        // written by the install seam, which runs AFTER this one and only on an ACCEPTED
        // epoch — so a stamp-based test here is dead wiring by construction and every
        // withheld row would silently blank the very columns this block exists to fill.
        // (Caught by the card's own review, 2026-08-03.)  `ddFoldStats()` is per-epoch:
        // NetworkEstimator::beginEpoch resets it, so `blocks == 0` means "no fold has run
        // this epoch" and the columns correctly stay blank.  A within-epoch RAIM re-solve
        // re-folds and overwrites, so what is read is always the latest fold.
        const bool have_adm =
            std::abs(ctx.carry.adm_ep_tow - se.time.tow) < 1e-6;
        const NetworkNormalEquations::DdFoldStats& es_ddfold = est.ddFoldStats();
        const bool have_ddfold = es_ddfold.blocks > 0;
        if (have_adm || have_ddfold) {
            EpochStatusRow rr = r;
            if (have_adm) {
                rr.adm_sta_worst_z = ctx.carry.ep_adm_sta_worst_z;
                rr.adm_net_worst_z = ctx.carry.ep_adm_net_worst_z;
            }
            if (have_ddfold) {
                rr.ddfold_degen_blocks  = es_ddfold.degen_blocks;
                rr.ddfold_degen_rows    = es_ddfold.degen_rows;
                rr.ddfold_min_pivot_rel = es_ddfold.min_pivot_rel;
            }
            traces.emitEpochStatus(se.time, disposition, rr);
            return;
        }
        traces.emitEpochStatus(se.time, disposition, r);
    };
    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };
    auto& cap_H = ws.cap_H;
    auto& cap_z = ws.cap_z;
    auto& cap_constraint_weight = ws.cap_constraint_weight;
    auto& cap_has_constraint = ws.cap_has_constraint;
    auto& cap_priors = ws.cap_priors;
    auto& cap_dense_keys = ws.cap_dense_keys;
    auto& cap_dense_cov = ws.cap_dense_cov;
    auto& cap_sbc_H = ws.cap_sbc_H;
    auto& cap_sbc_z = ws.cap_sbc_z;

    NetworkSolveGlueResult result;


// W6 satellite code-bias datum rows relocated to
// NetworkSbasis::buildCodeBiasDatumRows (INC-8) — full design rationale
// (IF pin + per-(constellation,signal) sum-to-zero, Card-2 time-linking,
// basis hold/latch/re-latch) in the method Doxygen.  The fold + the
// RAIM-replay cap_* capture and the first-N summary stay HERE (writer-site
// contract, NetworkRunTypes.hpp).
// Reset the hold-OFF IF-pin selection scratch for THIS epoch (mirrors the
// per-epoch this_epoch_anchor.clear()).  Unconditional — done even when the
// code-bias block below is skipped (no registered keys) so a stale prior-epoch
// selection can never reach the emit-site comparison.  A no-op on the empty
// map in the default (emit_clock_disc OFF) config.
this_epoch_if_pin_sel.clear();
// S-contract §1.5 declaration scratch — same unconditional per-epoch clear, for the
// same reason (a stale prior-epoch pair must never reach the emit-site fill).
this_epoch_if_pin_real.clear();
this_epoch_gf_pin_real.clear();
ctx.carry.this_epoch_family_gf_pin_real.clear();
this_epoch_cb_ifpin.clear();   // Card 2: this epoch's IF-pin pairs (persisted at harvest)
this_epoch_cb_reseed.clear();  // Card 2: this epoch's re-seeds (merged into the pending disc set at the emit site — withheld epochs never reach it)
if (config_.estimateNetworkSatCodeBias() && !registered_code_keys.empty()) {
    // Carrier frequency of a (sat,signal) from any present station's model.
    auto freqOf = [&](const std::string& sat, const std::string& sig) -> double {
        for (size_t i = 0; i < stations.size(); ++i)
            if (present[i]) {
                const double f = stations[i].mm->carrierFrequency(sig, sat);
                if (std::isfinite(f) && f > 0.0) return f;
            }
        return 0.0;
    };

    // --- FAMILY-GF: run-latched family census (docs/SPEC_family_gf_datum.md §3.2) ---
    // Runs whenever the GF datum is armed, knob ON or OFF, because an armed-but-inert
    // configuration must READ as such (standing invariant #4) — and because the OFF +
    // PARTITIONED line is exactly the diagnosis that was missing when
    // `_pc305_gg_tri_slant_20m` withheld every epoch on 2026-08-12 with nothing in the
    // log naming the receiver-family split as the cause.
    //
    // An INDEPENDENT scan, deliberately not a rider on the CB-ADM admission census: that
    // census reduces its per-(station, pair) map to an int support count and discards
    // station identity, so it cannot answer "does any station bridge the two families?",
    // and it does not run on this lineage at all (network.cb_admission is set in two
    // configs repo-wide, neither in the pc305 chain) — piggybacking would mean arming a
    // structurally different registration path to get a diagnostic.
    //
    // LATCHED on the first epoch with a usable station set: the partition is a property
    // of the receiver hardware population, not of an epoch.
    if (config_.networkCodeBiasGfDatum() && !family_census.latched) {
        std::map<std::string, std::set<std::string>> station_signals;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i] || !stations[i].sm) continue;
            auto& sigs_of = station_signals[stations[i].name];
            for (const auto& kv : stations[i].sm->getSatBiasStates())
                sigs_of.insert(kv.first);
        }
        if (!station_signals.empty()) {
            family_census = NetworkSbasis::runFamilyCensus(station_signals);
            for (const auto& line : family_census.lines)
                log->info("Network (FAMILY-GF census): {}{}", line,
                          (family_census.partitioned.empty() ||
                           config_.networkCodeBiasGfDatumPerFamily())
                              ? ""
                              : "  network.code_bias_gf_datum_per_family is OFF; expect "
                                "n_sats-1 undeclared near-null directions per partitioned "
                                "constellation and null-space gate withholds.");
        }
    }
    // Empty unless the knob is ON *and* the census found a partition ⇒ the row builder
    // folds nothing extra and the run is byte-identical in every other posture.
    static const std::set<std::string> kNoFamilies;
    const std::set<std::string>& gf_family_constellations =
        (config_.networkCodeBiasGfDatum() && config_.networkCodeBiasGfDatumPerFamily())
            ? family_census.partitioned
            : kNoFamilies;

    const auto cbd = NetworkSbasis::buildCodeBiasDatumRows(
        est, registered_code_keys, freqOf,
        cb_time_linked, clock_signal_basis_hold, emit_clock_disc,
        config_.networkCodeBiasGfDatum(),
        // RF-0c: the mode, and the run-latched broadcast targets (empty in every
        // mode but broadcast_tgd, which is what makes the builder fall back to a
        // zero target rather than guess).
        config_.getNetworkConfig().sat_gf_datum, setup.sat_gf_target_m,
        // RF-0e: the two arms of the level-datum A/B (loader refuses both at once).
        config_.getNetworkConfig().sat_if_level_datum,
        config_.getNetworkConfig().sat_gf_datum_time_linked,
        gf_family_constellations,
        code_bias_prev_, code_bias_ifpin_pair_,
        code_bias_sat_seen_, code_bias_datum_resumed_,
        held_clock_ref, this_epoch_if_pin_sel,
        this_epoch_if_pin_real, this_epoch_gf_pin_real,
        ctx.carry.this_epoch_family_gf_pin_real,
        this_epoch_cb_ifpin, this_epoch_cb_reseed,
        basis_relatch_disc_pending, emitEvent);
    if (cbd.H.rows() > 0) {
        est.attribTag("w6_cb_gauge");
        est.addSharedConstraintRows(cbd.H, cbd.z, NetworkSbasis::kCodeBiasGaugeWeight);
        // RF-0b ON-path telemetry.  In the no-constraint posture these rows are not a
        // gauge ALONGSIDE an ionosphere constraint, they are the ENTIRE satellite
        // code-bias datum, so the run has to be able to say how many epochs actually
        // folded them and how many channel levels they pinned (standing invariant #4:
        // "0 folded" is what inert wiring looks like, and the log alone cannot tell
        // that from a healthy run).
        if (config_.getNetworkConfig().w6_iono_constraint
            == NetworkW6IonoConstraint::None) {
            ++ctx.counters.n_w6_iono_none_epochs;
            ctx.counters.n_w6_iono_none_s2z_rows +=
                static_cast<std::size_t>(cbd.n_s2z_channels);
        }
        // RF-0c ON-path telemetry.  Under the no-constraint posture this row IS the
        // datum for the between-satellite geometry-free split, so the run has to be
        // able to say how many satellites actually received one and — under
        // broadcast_tgd — how many got a real group delay rather than a fallback zero
        // wearing a physical name (standing invariant #4).
        if (config_.getNetworkConfig().sat_gf_datum != NetworkSatGfDatum::NoneMode) {
            ++ctx.counters.n_sat_gf_epochs;
            ctx.counters.n_sat_gf_rows   += static_cast<std::size_t>(cbd.n_sat_gf_rows);
            ctx.counters.n_sat_gf_tgd    += static_cast<std::size_t>(cbd.n_sat_gf_tgd_hits);
            ctx.counters.n_sat_gf_tgd0   += static_cast<std::size_t>(cbd.n_sat_gf_tgd_zero);
            ctx.counters.n_sat_gf_fallback +=
                static_cast<std::size_t>(cbd.n_sat_gf_tgd_missing
                                         + cbd.n_sat_gf_pair_not_l1l2);
            ctx.counters.n_sat_gf_extra    += static_cast<std::size_t>(cbd.n_sat_gf_extra_rows);
            ctx.counters.n_sat_gf_isc0     += static_cast<std::size_t>(cbd.n_sat_gf_isc_zero);
            ctx.counters.n_sat_gf_sameband += static_cast<std::size_t>(cbd.n_sat_gf_sameband_free);
            ctx.counters.n_sat_gf_s2z_suppressed +=
                static_cast<std::size_t>(cbd.n_s2z_suppressed);
            ctx.counters.n_if_level_rows  += static_cast<std::size_t>(cbd.n_if_level_rows);
            ctx.counters.n_if_level_sats  += static_cast<std::size_t>(cbd.n_if_level_sats);
            ctx.counters.n_sat_gf_carried += static_cast<std::size_t>(cbd.n_sat_gf_carried);
            ctx.counters.n_sat_gf_anchored+= static_cast<std::size_t>(cbd.n_sat_gf_anchored);
        }
        if (within_epoch_armed) {
            cap_H = cbd.H; cap_z = cbd.z;
            cap_constraint_weight = NetworkSbasis::kCodeBiasGaugeWeight;
            cap_has_constraint = true;
        }
    }
    // --- Card KP: loose physical-bound prior on the ADMITTED OSB columns ---
    // Folded AFTER the tight gauge rows and BEFORE the rank gate, through
    // the DIAGONAL prior channel rather than the gauge stack, so
    // plan.declared_dimension is untouched by construction (K3).  Targets
    // the CODE-dominated residual direction the phase drop-and-heal cannot
    // reach: the heal anchors PB columns only, so a near-null direction
    // with zero phase weight is structurally beyond it.  OFF ⇒ empty list
    // ⇒ addSharedPrior returns immediately (byte-identical).
    if (config_.networkCbKaulaPrior()) {
        const auto kp = NetworkSbasis::buildCodeBiasKaulaPriors(
            registered_code_keys, config_.networkCbKaulaSigmaM());
        if (!kp.empty()) {
            est.attribTag("cb_kaula");
            est.addSharedPrior(kp);
            // The RAIM within-epoch replay re-solves from a fresh
            // beginEpoch(); a prior missing there would silently give the
            // replayed solve a DIFFERENT datum from the first one.
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), kp.begin(), kp.end());
            ++ctx.counters.n_kp_epochs;
            ctx.counters.n_kp_cols += kp.size();
            // The K2 anti-fragility margin is measured AFTER the gate (see
            // the one-shot block there), not here: it needs lambda_max of
            // the SAME reduced system the gate judges, and calling
            // reducedSharedNormals() at this point would force the Schur
            // reduction BEFORE the clock datum rows are folded.  That is
            // survivable — addSharedInfo carries a shared-border delta
            // through a cached reduction in place — but the clock rows'
            // w·HᵀH increment is not bit-symmetric, so reducing early would
            // make the first epoch's last bits depend on whether KP is armed.
        }
    }
    if (spectrum_diag_budget > 0)   // first-N (spectrum block decrements below)
        log->warn("Network[W6-cb]{}: nshared={}, n_cb={}, folded {} code-bias gauge "
                  "rows (IF pin for {} sats + sum-to-zero for {} (const,signal) levels"
                  "{}{}{})",
                  // RF-0b: in the no-constraint posture these rows are the WHOLE
                  // satellite code-bias datum, not a gauge beside an ionosphere
                  // constraint.  Naming the posture on the line that reports the row
                  // count is what lets a first-N read of any log say which datum
                  // produced the product it is looking at.
                  config_.getNetworkConfig().w6_iono_constraint
                          == NetworkW6IonoConstraint::None
                      ? "[iono-none]" : "",
                  est.numShared(), n_cb, cbd.H.rows(), cbd.n_if_pin_sats,
                  cbd.n_s2z_channels,
                  cbd.n_gf_pin_sats > 0
                      ? fmt::format(" + GEOMETRY-FREE pin for {} sats, of which {} carry "
                                    "no third band and are now fully datum-fixed",
                                    cbd.n_gf_pin_sats, cbd.n_gf_only_pair)
                      : std::string(),
                  // FAMILY-GF: the ON-path telemetry for the extra rows.  Printed only
                  // when rows actually folded, because "0 folded" is exactly what an
                  // inert knob looks like and the census line above already reports the
                  // verdict that explains a zero (invariant #4: check the counter, not
                  // the absence of a crash).
                  cbd.n_family_gf_rows > 0
                      ? fmt::format(" + FAMILY-GF pin for {} (sat,secondary-family) pairs, "
                                    "of which {} have a two-band family whose entire "
                                    "estimable content is the inter-family ISC",
                                    cbd.n_family_gf_rows, cbd.n_family_gf_only_pair)
                      : std::string(),
                  // RF-0c: name the datum on the line that reports the row count, so a
                  // first-N read of any log says which S-basis produced the product it
                  // is looking at — the zero and TGD frames differ by c*T_GD on every
                  // code bias and by the same amount, opposite sign, in the emitted
                  // ionosphere.
                  cbd.n_sat_gf_rows > 0
                      ? fmt::format(
                            " + SAT-GF {} datum for {} sats{}",
                            config_.getNetworkConfig().sat_gf_datum
                                    == NetworkSatGfDatum::BroadcastTgd
                                ? "broadcast_tgd" : "zero",
                            cbd.n_sat_gf_rows,
                            config_.getNetworkConfig().sat_gf_datum
                                    == NetworkSatGfDatum::BroadcastTgd
                                ? fmt::format(" ({} at c*T_GD, {} broadcast T_GD=0, "
                                              "{} fallback-to-zero)",
                                              cbd.n_sat_gf_tgd_hits, cbd.n_sat_gf_tgd_zero,
                                              cbd.n_sat_gf_tgd_missing
                                                  + cbd.n_sat_gf_pair_not_l1l2)
                                : std::string())
                      : std::string());
}

// --- Datum + null-space gate + solve --------------------------------
NetworkSbasis sb;
SbasisDeclaration decl;
decl.reference_receiver_id = nc.clock_datum_station;
decl.emit_code_bias = config_.estimateNetworkSatCodeBias();  // W6: self-describe the OSB datum
// Fix A: self-describe the anchor-free ensemble phase-bias frame so a
// consumer knows the emitted bias references the receiver ensemble (Σ_r z=0),
// not a single pivot receiver's ambiguity.
if (phase_datum_ensemble)
    decl.amb_datum = SbasisDeclaration::AmbDatum::SumToZeroPerReceiver;
// Card 3: self-describe the Eq-34 single-receiver-chain phase-bias frame so a
// consumer knows the emitted between-satellite reference is integer-consistent
// (each chain hop an integer) — wire value 2 rides the existing v2 S line.
else if (phase_datum_eq34)
    decl.amb_datum = SbasisDeclaration::AmbDatum::SingleReceiverChain;
if (clock_brdc_anchored) {
    // Broadcast-anchored clock gauge: buildSharedDatum branches on this to
    // emit one per-satellite anchor row (Δδtˢ→0 = its own broadcast
    // a-priori) at 1/σ² — the anchors ARE the datum (no pivot, empty datum,
    // declared dimension 0), and unlike sum-to-zero the frame does not
    // redistribute when membership changes.
    decl.isb_datum = SbasisDeclaration::IsbDatum::BroadcastAnchoredPerSatellite;
    decl.broadcast_anchor_sigma_m = clk_anchor_sigma;  // D6-resolved (brdc_rts auto = 3.0 m; load-validated binding)
} else if (clock_sum_to_zero) {
    // Anchor-free clock gauge: buildSharedDatum branches on this to emit
    // per-constellation sum-to-zero constraint rows (no pivot, empty datum).
    decl.isb_datum = SbasisDeclaration::IsbDatum::SumToZeroPerConstellation;
} else if (!nc.clock_datum_satellite.empty()) {
    decl.clock_pivot_sat[NetworkSbasis::constellationOf(nc.clock_datum_satellite)]
        = nc.clock_datum_satellite;
}

NetworkSbasis::Plan plan;
try {
    // RTA-1: in sticky mode the incumbent baseline is last_pivot, NOT
    // prev_pivot — prev_pivot is wholesale-replaced at each emit, so a
    // constellation absent from the last emitted epoch drops out of it,
    // and its return would spuriously re-select (and, with the
    // vanish-return arm off, SILENTLY re-reference the datum).
    // last_pivot is merge-updated at the same emit site, so per
    // constellation it holds the pivot of the last emitted product that
    // CONTAINED it — exactly the datum a consumer of that constellation
    // last adopted.  Both maps advance only at the emit site (P0-1), so
    // stickiness tracks the datum of the last EMITTED product: a
    // handover epoch that is subsequently withheld leaves the incumbent
    // unchanged, and if the incumbent returns before any new-datum epoch
    // emits, the datum simply never moves.
    plan = sb.buildSharedDatum(est, registered_clock_sats, decl,
                               sticky_pivot ? &last_pivot : nullptr);
} catch (const std::exception& ex) {
    log->warn("Network: buildSharedDatum failed ({}); skipping epoch", ex.what());
    ++n_skipped;
    { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
      emitEpochStatus("datum_failed", es); }
    prev_week = se.time.week; prev_tow = se.time.tow;
    result.action = EpochAction::SkipEpoch;
    return result;
}

// Constraint-realized clock datums: fold the gauge rows onto the shared
// border BEFORE the null-space gate — sum-to-zero mode carries one
// per-constellation Σδtˢ=0 row, broadcast-anchored mode one per-satellite
// Δδtˢ→0 anchor row at 1/σ² (the anchors ARE the datum there) — so plan
// carries constraint rows instead of hard pins and declared_dimension is 0.
// Folded exactly like the code-bias gauge (additive information; the
// within-epoch re-solve re-applies them from plan, which is in scope
// there).  Empty (0 rows) in PIN mode → this is a no-op, byte-identical.
if (plan.clock_constraint_H.rows() > 0) {
    est.attribTag("clock_datum_rows");
    est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                plan.clock_constraint_weight);
}

// Pivot handover: the previous epoch's shared estimate lives in the OLD
// datum, so a changed clock pivot invalidates the D2 carry-forward (and
// forces a discontinuity bump downstream).
// A1/A2: detect a clock-pivot datum re-reference and record WHICH
// constellations changed (for the A2 per-constellation emit-bump scoping).
// Arm (b) (vanish-return) is gated on network.pivot_vanish_return_signal;
// when off this reproduces the legacy adjacent-epoch-only detection.
std::set<char> changed_consts;
const bool datum_changed = pivotHandoverSignalled(
    plan.clock_pivot, prev_pivot, last_pivot, pivot_vr_signal, changed_consts);
// RTA-2: does THIS key's carried state belong to a retired datum?  With
// network.scoped_handover_response off this is exactly datum_changed (the
// legacy global response, byte-identical); when on, only keys of the
// constellation(s) whose pivot changed — the SAME changed_consts set the
// A2 emit bump consumes (single source of truth) — are invalidated.
const auto handover_affects = [&](const std::string& sat) {
    return datum_changed
        && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
};

networkInjectShVtecPrior(ctx, ws, config_, est, sh_model.get(),
                         sh_model2.get(), datum_changed,
                         dt, within_epoch_armed);

networkInjectPhaseHoldPrior(ctx, ws, est, within_epoch_armed);

// eq34 reattach hygiene INC-2: the open pin bridges.  Same PRE-gate placement and
// the same reason as the phase hold above — for a bridged key this soft row IS the
// datum (the election left it deliberately unpinned), so it has to be in the reduced
// normals the gate inspects.  No-op when no bridge is open.
networkInjectPinBridgePrior(ctx, ws, est, within_epoch_armed, log);
// Record THIS epoch's clock-datum verdict for the next election's bridge lifecycle: a
// pivot handover re-references every satellite phase bias (the phase observable ties
// δtˢ + b_s), so a level harvested before it belongs to the retired frame.  The
// bridge therefore drops at the NEXT hygiene pass with reason=datum.  The one-epoch
// exposure is deliberate and bounded: the election commits to the bridge before
// buildSharedDatum has chosen this epoch's pivots, and the row is SOFT, so a
// stale-by-one-epoch value is outvoted by the data everywhere except along the gauge
// direction it legitimately owns (spec §7 pre-declares this deferral).
ctx.carry.last_datum_changed = datum_changed;

// Prioritized phase-bias datum drop-and-heal relocated to
// filter/NetworkPhaseDatum (INC-8) — the seam's seed; the anchor election
// + incumbent carry join it at INC-8b.  Full rationale + the ★ PERF note
// (full eigenvector decomposition — gate-failure path ONLY, never per
// epoch) in the function Doxygen.

// Null-space rank gate.  The reduced shared normals are deliberately
// ill-conditioned: the phase rows pin the satellite-clock / phase-bias
// SUM to ~σ_phase precision (weight ~1e5), while CODE alone separates the
// two (the clock-vs-bias split), so that split is orders of magnitude
// weaker — yet still firmly non-zero.  Empirically the spectrum shows a
// clean ~7-decade gap between the true gauge freedoms (the per-
// constellation clock datum, σ ~ 1e-10 of σ_max) and the weakest REAL
// direction (σ ~ 1e-2).  The default 1e-6 relative tolerance sits above
// the weak-but-real directions and miscounts them as deficiencies; a
// 1e-9 relative tolerance lands squarely in that GPS+Galileo-era gap,
// counting only the genuine gauges while still catching a truly unpinned
// datum.
//
// Card DG (2026-07-22): the gap MOVES with the constellation mix, so the
// tolerance is a per-config CALIBRATION parameter (network.rank_gate_tol),
// resolved once into est.setRankGateTol() and read here — the same value
// the estimability dump and the drop-and-heal read (DG2: no consumer keeps
// a private tolerance, or the dump would describe a decision the gate did
// not make and the heal would soft-anchor weak-but-REAL directions).  It
// defaults to NetworkEstimator::kNullSpaceTol, so an unset key is
// byte-identical.  Adding BeiDou is the second instance of the same
// miscount 1e-6 → 1e-9 already fixed once: the BDS weak-but-real family
// layer sits ≈1.7e-10 of λmax, ~3 decades deeper relatively, with a
// six-decade measured gap down to the declared gauges.

// ---- One-shot estimability diagnostic (default-OFF) ---------------
// Spectrum dump relocated to NetworkEstimator::dumpEstimabilitySpectrum
// (INC-8) — the derivation and the eigen-vs-SVD cost rationale live in
// the method Doxygen.  The one-shot LATCH is always taken so the
// co-located W6-cb one-shot log above still fires exactly once.
// DG5: the returned struct is the SAME numbers the WARN lines print,
// mirrored into network_estimability.csv so the storm analyzer reads a
// CSV instead of re-parsing log text.  The log text itself is unchanged.
if (spectrum_diag_budget > 0) {
    --spectrum_diag_budget;
    if (nc.trace_estimability) {
        const auto dump = est.dumpEstimabilitySpectrum(
            se.time.tow, plan.declared_dimension, col_desc, log);
        if (dump.valid) {
            traces.writeEstimabilityEpochRow(se.time, dump.tow, dump.n_shared,
                                             dump.declared_dim, dump.ev_max,
                                             dump.cutoff, dump.n_below);
            for (const auto& d : dump.dirs)
                traces.writeEstimabilityDirRow(se.time, d.ev, d.top);
        }
    }
}

bool gate_ok = true;
// DG6: the observed near-null count, captured on BOTH the passing and the
// throwing path (the exception text cannot be parsed back into a number).
// "Under-specified" (observed > declared — a missing datum constraint) and
// "over-specified" (observed < declared — a tolerance below THIS config's
// gauge depth, i.e. a mis-calibration withholding a healthy config) demand
// opposite fixes, so they are counted separately.
// Card KP — the K2 anti-fragility margin, logged ONCE, on the SAME reduced
// system the gate is about to judge (lambda_max here == the gate's, and
// the reduction now includes the clock datum rows).  The CB-ADM card
// rejected a loose prior whose information sat AT the rank cutoff, so this
// one must sit >= 2 decades ABOVE it; a sub-2-decade margin WARNS.
if (config_.networkCbKaulaPrior() && !ctx.counters.kp_margin_logged
    && ctx.counters.n_kp_cols > 0) {
    ctx.counters.kp_margin_logged = true;
    const Eigen::MatrixXd& Nss_kp = est.reducedSharedNormals();
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es_kp(Nss_kp, Eigen::EigenvaluesOnly);
    if (es_kp.info() == Eigen::Success && es_kp.eigenvalues().size() > 0) {
        const double lmax = es_kp.eigenvalues()(es_kp.eigenvalues().size() - 1);
        const double cutoff = est.rankGateTol() * std::max(lmax, 1.0);
        const double info = 1.0 / (config_.networkCbKaulaSigmaM()
                                   * config_.networkCbKaulaSigmaM());
        const double decades = (cutoff > 0.0 && info > 0.0)
            ? std::log10(info / cutoff) : 0.0;
        if (decades >= 2.0)
            log->info("Network (Card KP): prior info {:.3e} vs rank-gate cutoff {:.3e} "
                      "— margin {:.1f} decades ABOVE the cutoff (K2 requires >= 2.0).  "
                      "sigma {:g} m on the admitted OSB columns.",
                      info, cutoff, decades, config_.networkCbKaulaSigmaM());
        else
            log->warn("Network (Card KP): prior info {:.3e} sits only {:.1f} decades "
                      "above the rank-gate cutoff {:.3e} — BELOW the K2 minimum of 2.0.  "
                      "This is the exact configuration the CB-ADM card rejected (prior "
                      "information AT the cutoff either trips the gate or survives by "
                      "coincidence).  TIGHTEN network.cb_kaula_sigma_m or re-place "
                      "network.rank_gate_tol from this config's estimability dump before "
                      "trusting the result.", info, decades, cutoff);
    }
}

// --- SBC S-basis completion (network.sbasis_complete) --------------------
// The declared dimension counts the STRUCTURAL gauges only (per-constellation
// clock in PIN mode); the realized epoch can carry more — measured GPS+Gal:
// per-satellite channel-family phase-bias↔iono trade directions (eigenvector
// ∝ μ_σ over one Q/X receiver family's PB columns) that only the SH prior
// resists once that family's code support thins.  Derive the REALIZED
// dimension from the same matrix state the gate inspects and constrain each
// extra direction along its own eigenvector at its CARRIED level, so the
// unobservable combination holds instead of stepping (engage edge) and the
// data re-determines it from the held level when observability returns
// (release edge) — both edges evented per standing invariant #7.  Refusals
// (cap / degenerate split / marginal weight) fold NOTHING and fall through
// to the unchanged gate → withhold; Card PA stays the armed backstop.
if (ctx.knobs.sbasis_complete_on) {
    // Continuity targets: the carried shared state mapped into this epoch's
    // columns.  PhaseBias rides pb_level_prev (prev_carry omits it under
    // carry_invariant — the same gap the INC-2 bridge hit), CodeBias prefers
    // the Card-2 kept-epoch map when present.  On a datum re-reference the
    // carried values belong to the retired frame → targets re-seed at 0,
    // like every other re-referenced quantity.
    Eigen::VectorXd sbc_x_prev = Eigen::VectorXd::Zero(est.numShared());
    if (!datum_changed) {
        for (const auto& cp : ctx.carry.prev_carry) {
            try {
                switch (cp.kind) {
                case SharedParamKind::Clock:
                    sbc_x_prev(est.clockColumn(cp.sat)) = cp.value; break;
                case SharedParamKind::CodeBias:
                    sbc_x_prev(est.codeBiasColumn(cp.sat, cp.signal)) = cp.value; break;
                case SharedParamKind::PhaseBias:
                    sbc_x_prev(est.phaseBiasColumn(cp.sat, cp.signal)) = cp.value; break;
                case SharedParamKind::VtecSh:
                    sbc_x_prev(est.vtecCoeffColumn(cp.sh_n, cp.sh_m, cp.sh_trig,
                                                   cp.sh_layer)) = cp.value; break;
                }
            } catch (...) { /* departed key — no target */ }
        }
        for (const auto& [key, v] : ctx.carry.pb_level_prev) {
            const auto us = key.find('_');
            if (us == std::string::npos || !std::isfinite(v)) continue;
            try {
                sbc_x_prev(est.phaseBiasColumn(key.substr(0, us),
                                               key.substr(us + 1))) = v;
            } catch (...) {}
        }
        for (const auto& [skey, sats] : code_bias_prev_) {
            const auto bar = skey.find('|');
            if (bar == std::string::npos) continue;
            const std::string sig = skey.substr(bar + 1);
            for (const auto& [sat, v] : sats) {
                if (!std::isfinite(v)) continue;
                try { sbc_x_prev(est.codeBiasColumn(sat, sig)) = v; } catch (...) {}
            }
        }
    }
    const auto sbc = sb.completeSbasisDimension(
        est.reducedSharedNormals(), plan.declared_dimension, est.rankGateTol(),
        sbc_x_prev,
        [&est](Eigen::Index c) { return est.sharedColumnLabel(c); },
        ctx.knobs.sbasis_complete_max);
    // --- datum graph census hand-off (docs/SPEC_datum_graph_census.md) ----
    // The SPECTRAL half of the census's contingency, captured HERE and only
    // here: this is the one place in the run where the realized near-null
    // count is measured PRE-completion and its directions still carry
    // attribution.  The gate below runs on the COMPLETED system and would
    // report `observed == declared`, i.e. the completion's own success, which
    // is not the quantity the census is reconciling against.
    //
    // MEASURE-ONLY and armed-only: nothing downstream of this block reads
    // ctx.carry.dg_spec, so an unarmed run pays one predictable branch.  The
    // identities come from parsing `d.detail` — the SAME string the
    // sbc_engage / sbc_skip events carry — so the census and the event stream
    // agree about a direction's identity by construction rather than by two
    // parallel derivations that can drift.  Both populations are recorded:
    // the CONSTRAINED extras and the SKIPPED orphans are equally "observed",
    // and dropping the orphans would silently delete the `observed only` cell
    // that measures this predictor's honest boundary.
    if (ctx.knobs.dg_on) {
        auto& dgs = ctx.carry.dg_spec;
        dgs.valid    = true;
        dgs.from_sbc = true;
        dgs.observed = sbc.observed;
        dgs.declared = plan.declared_dimension;
        // Overwrite, never append: if a within-epoch adaptation ever re-enters
        // this body the census must see THIS evaluation's directions, not the
        // union of every attempt.
        dgs.dirs.clear();
        const auto push = [&dgs](const NetworkSbasis::GaugeCompletion::Dir& d,
                                 bool skipped) {
            NetDatumGraphDir g;
            g.ev        = d.ev;
            g.skipped   = skipped;
            g.top_label = d.top_label;
            g.detail    = d.detail;
            g.ids       = netDatumGraphDetailIds(d.detail);
            dgs.dirs.push_back(std::move(g));
        };
        for (const auto& d : sbc.dirs)    push(d, false);
        for (const auto& d : sbc.skipped) push(d, true);
    }
    // Per-direction skips (unseparated from the declared-gauge cluster —
    // orphaned exact gauges, Card PA's class) are NOT epoch-fatal: the
    // separated extras still fold below, the gate then throws by exactly
    // n_skipped, and the drop-and-heal composes on that remainder.  Counted
    // per DIRECTION and evented per epoch so a skip is never silent.
    if (sbc.n_skipped > 0) {
        ctx.counters.n_sbc_degenerate += static_cast<std::size_t>(sbc.n_skipped);
        log->debug("Network (SBC): TOW {:.1f} skipped {} unseparated extra "
                   "direction(s) (machine-zero orphan class — left to the "
                   "drop-and-heal); {} separated extra(s) proceed.",
                   se.time.tow, sbc.n_skipped, sbc.n_extra);
        // One ATTRIBUTED event per skipped direction: a skipped orphan that
        // then withholds the epoch must name its own columns, or the withhold
        // is undiagnosable from the trace (the heal cannot see this class —
        // no PB span ≥ 0.3 — so this event is the only attribution there is).
        for (const auto& d : sbc.skipped)
            emitEvent("GATE", "sbc_skip",
                      d.sats.size() == 1 ? *d.sats.begin() : std::string{}, "",
                      d.ev, d.top_span, "leave_to_heal", d.detail);
    }
    if (sbc.capped || sbc.weight_marginal) {
        const char* why = sbc.capped ? "cap" : "weight";
        if (sbc.capped)          ++ctx.counters.n_sbc_capped;
        if (sbc.weight_marginal) ++ctx.counters.n_sbc_weight_marginal;
        log->warn("Network (SBC): TOW {:.1f} completion REFUSED ({}) — observed {} "
                  "vs declared {}; nothing folded, the gate decides the epoch.",
                  se.time.tow, why, sbc.observed, plan.declared_dimension);
        emitEvent("GATE", "sbc_refuse", "", "",
                  static_cast<double>(sbc.observed),
                  static_cast<double>(plan.declared_dimension), why, "");
        // Active set deliberately unchanged: nothing was constrained and the
        // epoch is headed for a withhold — a release edge here would claim a
        // re-coupling that never happened.
    } else if (sbc.n_extra > 0) {
        est.attribTag("sbasis_complete");
        est.addSharedConstraintRows(sbc.H, sbc.z,
                                    NetworkSbasis::kSbasisCompleteWeight);
        if (within_epoch_armed) {   // RAIM-replay capture (EpochWorkspace contract)
            cap_sbc_H = sbc.H;
            cap_sbc_z = sbc.z;
        }
        ++ctx.counters.n_sbc_epochs;
        ctx.counters.n_sbc_dirs += static_cast<std::size_t>(sbc.n_extra);
        ctx.counters.sbc_max_dirs = std::max(ctx.counters.sbc_max_dirs, sbc.n_extra);
        std::set<std::string> now_active;
        for (const auto& d : sbc.dirs) {
            now_active.insert(d.sats.begin(), d.sats.end());
            emitEvent("GATE", "sbc_engage",
                      d.sats.size() == 1 ? *d.sats.begin() : std::string{}, "",
                      d.ev, d.top_span, "gauge_row",
                      fmt::format("{};target={:.4f}", d.detail, d.target));
        }
        // Release edge (invariant #7's re-coupling): a satellite constrained
        // last epoch and in NO direction now has re-observed — the data takes
        // over from the held level.
        for (const auto& s : ctx.carry.sbc_active_sats)
            if (!now_active.count(s)) {
                ++ctx.counters.n_sbc_release;
                emitEvent("GATE", "sbc_release", s, "", std::nullopt, std::nullopt,
                          "reobserved", "held combination re-determined by data");
            }
        ctx.carry.sbc_active_sats.swap(now_active);
        if (ctx.counters.n_sbc_epochs == 1)
            log->info("Network (SBC): first completion at TOW {:.1f} — realized "
                      "S-basis dimension {} vs declared {}; {} extra direction(s) "
                      "constrained along their eigenvectors at the carried level "
                      "(events: sbc_engage/sbc_release, tier GATE).",
                      se.time.tow, sbc.observed, plan.declared_dimension,
                      sbc.n_extra);
        else
            log->debug("Network (SBC): TOW {:.1f} constrained {} extra "
                       "direction(s) (realized dim {} vs declared {}).",
                       se.time.tow, sbc.n_extra, sbc.observed,
                       plan.declared_dimension);
    } else if (!ctx.carry.sbc_active_sats.empty()) {
        // Clean epoch (no extras): every previously-constrained satellite has
        // re-observed — emit its release and clear the set.
        for (const auto& s : ctx.carry.sbc_active_sats) {
            ++ctx.counters.n_sbc_release;
            emitEvent("GATE", "sbc_release", s, "", std::nullopt, std::nullopt,
                      "reobserved", "held combination re-determined by data");
        }
        ctx.carry.sbc_active_sats.clear();
    }
}

int gate_observed       = -1;   // the LAST evaluation this epoch (post-heal if a re-gate ran)
int gate_first_observed = -1;   // this epoch's FIRST evaluation — the DG6 min/max sample
double gate_ev_max      = -1.0; // invariant #7: λmax of the FIRST evaluation (the gate's own cutoff)
++ctx.counters.n_gate_evaluated;
try {
    sb.assertNullSpaceRank(est.reducedSharedNormals(), plan.declared_dimension,
                           est.rankGateTol(), &gate_observed, &gate_ev_max);
    gate_first_observed = gate_observed;
} catch (const std::exception& ex) {
    // assertNullSpaceRank writes observed_out BEFORE the equality test, so
    // the count that caused this throw is already in gate_observed; latch it
    // now, before a possible re-gate overwrites it.
    gate_first_observed = gate_observed;
    // The reduced shared system carries MORE gauge freedoms than the
    // declared datum can pin — typically a satellite that is momentarily
    // unobservable (seen by a single station at marginal geometry), or —
    // under the prioritized datum — a phase-bias column whose anchor was
    // dropped / whose separation is weak.  For the prioritized case, try to
    // soft-anchor exactly those near-null phase-bias columns and RE-GATE
    // before withholding (the eigenVECTOR heal runs ONLY here, not every
    // epoch).  If the datum is still deficient after healing (or on the
    // legacy path), the product would be a meaningless least-norm split, so
    // WITHHOLD (the plan's "gate emission until convergence" safeguard).
    bool recovered = false;
    // PA2: per-healed-column attribution for the trace_events rows below.
    // Declared OUTSIDE the inner try so the rows are still emitted when the
    // RE-GATE (not the heal) is what threw — a heal that fired and did not
    // rescue the epoch is exactly the case worth seeing.
    std::vector<PhaseHealEvent> heal_events;
    int healed_n = 0;   // >0 only once the heal RETURNED (its priors are folded)
    // DG6 observability: the heal / re-gate failure text, CAPTURED rather than
    // logged in place, so the single withhold WARN below owns the whole
    // user-visible report.  Stays empty when the inner try returns without
    // throwing — which includes the case that used to be logged nowhere at all:
    // healed == 0 skips the re-gate, so nothing in the block can throw and the
    // epoch reaches the withhold carrying no exception of its own.
    std::string heal_fail_what;
    try {
        const int healed = healNearNullPhaseBias(
            est, shared_phase_keys, phase_datum_prioritized,
            within_epoch_armed, cap_priors,    // 0 unless prioritized + near-null PB
            &heal_events);
        healed_n = healed;
        // PA2: count the ATTEMPT before the re-gate can throw — the
        // success-only pair below cannot distinguish "the heal never ran"
        // from "the heal ran every epoch and never rescued the gate".
        if (healed > 0) {
            ++ctx.counters.n_heal_attempt_epochs;
            ctx.counters.n_heal_attempt_cols += static_cast<size_t>(healed);
        }
        if (healed > 0) {
            sb.assertNullSpaceRank(est.reducedSharedNormals(),
                                   plan.declared_dimension,
                                   est.rankGateTol(), &gate_observed);
            recovered = true;
            ++n_healed_epochs;
            n_healed_cols += static_cast<size_t>(healed);
            log->debug("Network: TOW {:.1f} soft-anchored {} near-null phase-bias "
                       "column(s) (dropped anchor / weak separation) — gate recovered.",
                       se.time.tow, healed);
        }
    } catch (const std::exception& ex2) {
        // The heal (eigensolve / addSharedPrior) or the re-gate reported a
        // deficiency it could not resolve — withhold rather than crash.  The
        // report is deferred to the unified WARN on the !recovered path: this
        // catch is NOT the only route to a withhold, and when it used to own
        // the only warn the other routes were invisible above debug level.
        heal_fail_what = ex2.what();
    }
    // PA2: one row per soft-anchored column (station-less by nature — the
    // heal acts on a SHARED column).  statistic = the eigenvalue of the
    // near-null direction the column spans most heavily, threshold = that
    // |component|; action records whether the heal actually rescued the
    // epoch, so an attributable-but-futile heal is distinguishable from a
    // working one.  Gated on healed_n > 0 so a heal that THREW mid-fold
    // cannot emit rows claiming columns were anchored when no prior was
    // installed — the re-gate throw (healed_n > 0, recovered false) is the
    // case worth seeing, and it is preserved.  No-op unless
    // network.trace_events.
    if (healed_n > 0) {
        for (const auto& h : heal_events)
            emitEvent("GATE", "phase_heal", h.sat, "", h.ev, h.span,
                      recovered ? "soft_anchor" : "soft_anchor_insufficient",
                      h.signal);
    }
    if (!recovered) {
        gate_ok = false;
        ++n_gate_withheld;
        // DG6: split the withhold by DIRECTION.  observed > declared is a
        // missing datum constraint (the BeiDou storm); observed < declared
        // means the tolerance sits BELOW this config's own gauge depth, so
        // the gauges stopped being counted — a mis-calibration withholding a
        // HEALTHY config, which no amount of datum work will fix.  gate_observed
        // is whatever the LAST gate evaluation of this epoch measured.
        if (gate_observed >= 0 && gate_observed < plan.declared_dimension)
            ++ctx.counters.n_gate_withheld_over;
        else
            ++ctx.counters.n_gate_withheld_under;
        // DG6 observability: ONE warn-level line on EVERY withhold, whatever
        // route reached it, reporting BOTH gate evaluations.  It replaces a
        // debug-only line and a warn that only the inner catch could emit —
        // two defects measured on the DOY-129 GPS+Gal cluster at TOW
        // 263250/263280/263310/263340/263370:
        //   (1) INVISIBLE WITHHOLDS.  A heal that anchors 0 columns (a
        //       machine-zero orphan direction with no phase-bias span, smallest
        //       ev -6.36e-12) never re-gates, so the inner catch never ran and
        //       the epoch was withheld with nothing above debug.  The run-end
        //       counter said 5, the log showed 4, and TOW 263250 appeared only
        //       in network_events.csv.
        //   (2) WRONG COUNT.  The old warn printed the RE-GATE text, i.e. the
        //       deficiency remaining AFTER the heal folded its soft anchors —
        //       "observed 4" in the log against "observed 5" in the CSV, the
        //       heal having collapsed exactly one of three extra directions.
        //       A log-only reader under-counted the deficiency by one.
        // gate_first_observed and gate_observed are latched for precisely this
        // purpose; ex.what() is the FIRST evaluation, so this line and the
        // gate_withheld CSV row below now tell the same story.
        const std::string regate_part =
            (healed_n > 0) ? fmt::format(", re-gate observed {}", gate_observed)
                           : std::string(" (no re-gate ran)");
        log->warn("Network: observability gate at TOW {:.1f} withheld product — "
                  "first gate observed {}, declared {}; heal anchored {} column(s){}."
                  "  First gate: {}{}",
                  se.time.tow, gate_first_observed, plan.declared_dimension,
                  healed_n, regate_part, ex.what(),
                  heal_fail_what.empty()
                      ? std::string{}
                      : fmt::format("  drop-and-heal failed: {}", heal_fail_what));
        emitEvent("GATE", "gate_withheld", "", "", std::nullopt, std::nullopt,
                  "skip_epoch", ex.what());
    }
}
// DG6: run-wide observed-near-null envelope, ONE sample per epoch (the
// FIRST gate evaluation — a post-heal re-gate answers a different
// question and is already counted by n_healed_epochs).  This is the
// evidence DG3's placement rule is audited against: a run whose observed
// count never leaves {declared} needs no calibration at all.
if (gate_first_observed >= 0) {
    auto& lo = ctx.counters.nullspace_obs_min;
    auto& hi = ctx.counters.nullspace_obs_max;
    if (lo < 0 || gate_first_observed < lo) lo = gate_first_observed;
    if (hi < 0 || gate_first_observed > hi) hi = gate_first_observed;
}
// STANDING INVARIANT #7 (docs/SPEC_card1_coupling_carry.md §13): measure how much
// information each DECLARED datum direction carries, on the SAME normals and cutoff the
// gate just judged.  The gate's integer count cannot distinguish "the tolerance is
// mis-placed" from "a carry FILLED a gauge direction", and those need opposite fixes —
// this ratio is the discriminator, and it is the reason that class can never again go
// silent.  Measure-only, unconditional, ~one k×k quadratic form per constellation.
// Deliberately AFTER the withhold accounting above so the coincidence counter
// (OVER-specified withhold on a breached epoch) can be formed here in one place.
networkDatumInfoRatio(ctx, traces, se, est, plan.declared_dimension,
                      registered_clock_sats, gate_ev_max,
                      /*gate_over_withheld=*/!gate_ok && gate_observed >= 0
                          && gate_observed < plan.declared_dimension);
// Datum graph census: the FALLBACK spectral hand-off, for runs without SBC.
// The gate's own first evaluation is then the only realized near-null count
// there is — a COUNT with no direction identities, which the census labels
// `obs_src=count` rather than pretending it can attribute.  Deliberately does
// NOT overwrite an SBC capture: with the completion armed the gate inspects
// the COMPLETED system and reads back the declared dimension, so taking its
// count would report the completion succeeding instead of the deficiency the
// census is reconciling against.
if (ctx.knobs.dg_on) {
    auto& dgs = ctx.carry.dg_spec;
    if (!dgs.from_sbc && gate_first_observed >= 0) {
        dgs.valid    = true;
        dgs.observed = gate_first_observed;
        dgs.declared = plan.declared_dimension;
    }
}
if (!gate_ok) {
    ++n_skipped;
    if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                     << ',' << n_present << ',' << n_clock << ',' << n_pb
                     << ',' << plan.declared_dimension << ",0,0,0,0," << total_pins << '\n';
    { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
      es.datum_changed = datum_changed ? 1 : 0;
      emitEpochStatus("gate_withheld", es); }
    // RTA-10: product-level heartbeat for a WITHHELD epoch — an H line with NO
    // E line, so a live tail can tell "gate withheld" from "producer died".
    // Before the deferred product writer opens (the first emit) the heartbeat is
    // BUFFERED in setup.pending_h_pre_open and flushed at the open — a leg that
    // opens inside a withhold era previously wrote nothing for the whole era.
    // σ̂0 is unavailable pre-solve (0 per the H grammar); withheld="*" = whole epoch.
    if (emit_epoch_status) {
        NetworkProducerStatus hst;
        hst.time.week = se.time.week;
        hst.time.tow  = se.time.tow;
        hst.state     = NetworkProducerStatus::State::WithheldGate;
        // H-rider: the census σ̂₀ is PRE-solve, so unlike the RAIM statistic it
        // exists even on a gate-withheld epoch (0.0 stays the sentinel when
        // the feed is off or the pool was empty).
        if (ctx.knobs.h_sigma0_census) hst.sigma0 = ctx.carry.ep_h_sigma0_census;
        hst.withheld  = "*";
        if (product_writer) {
            product_writer->writeEpochStatus(hst);
            if (nc.product_flush_per_epoch) product_writer->flush();
        } else if (setup.contract_pending) {
            // Buffer ONLY while a v2 writer open is still coming — with no
            // product path (or v1 immediate open) there is nothing to flush into.
            setup.pending_h_pre_open.push_back(hst);
        }
    }
    prev_week = se.time.week; prev_tow = se.time.tow;
    result.action = EpochAction::SkipEpoch;
    return result;
}

// Two-state clock model warmup gate (mirrors carry_warm): seed/predict only after the
// cold-start convergence horizon.  Its per-epoch predictions (for the post-solve update
// + normalized-innovation test) are stashed here.
const bool clk_model_warm = clk_model_on && (static_cast<int>(n_emitted) >= pl_warmup);
// network.emit_clock_rate: same warmup gate as the clock model — skip the cold-start
// epochs where the floated clock would finite-difference into a huge spurious C1 rate.
const bool emit_rate_warm = emit_clock_rate_on && (static_cast<int>(n_emitted) >= pl_warmup);
auto& clk_pred = ws.clk_pred;
auto& clk_prior_info = ws.clk_prior_info;

// --- Card 6 (RTF-2 / plan D3): ephemeris-cutover re-level ---------------------------
// Re-base the carried clock state by -Delta_eph on a broadcast IODE roll (MUST precede
// the prior injection + solve).  Relocated VERBATIM to filter/NetworkRelevel at the
// Card-6 relevel decomposition (2026-07-23); full ordering rationale lives there.
networkEphCutoverRelevel(ctx, log, traces, se, registered_clock_sats, brdc_rts);

// --- Queue 1(b): the clock-pivot handover S-transform (SPEC_pivot_visibility_gauge_defect §8)
// Re-express the CHANGED constellation's carried shared means in the INCOMING clock
// datum and mark them for fold release, so the prior below folds instead of being
// skipped for one epoch.  Placed here — immediately before the first fold that reads
// prev_carry — deliberately: every consumer of the carry must see the same frame, and
// this is the last point at which that is guaranteed by construction rather than by an
// audit of the intervening seams.  It runs AFTER the eph-cutover re-level so the two
// re-basings compose in the order the carry actually experienced them (cutover first:
// it is a change of the a-priori the carry is expressed AGAINST; the handover is a
// change of the gauge that carry realizes).  No-op and byte-identical when the knob is
// off; clears its own release set every epoch.
networkHandoverStransform(ctx, config_, log, traces, se.time, plan, datum_changed,
                          changed_consts, registered_clock_sats);

networkInjectD2CarryPrior(ctx, ws, traces, se.time, est, datum_changed,
                          changed_consts, dt, within_epoch_armed, log);

networkInjectClockOscPrior(ctx, ws, traces, se.time, est, registered_clock_sats,
                           clk_model_warm, datum_changed, changed_consts,
                           pl_now_s, dt, within_epoch_armed);

// --- A-priori satellite-clock anchor (cold-start conditioning) ------
// The shared clock column is Δδtˢ relative to the a-priori PRECISE product
// clock, so it is genuinely small (sub-metre for precise post-processing).
// With no absolute anchor the per-epoch joint solve floats it by hundreds
// of metres at cold start to absorb the clock↔iono↔bias confusion (no
// carry-forward exists yet at epoch 1, and epoch 2+ then anchors to that
// floated value).  A Tikhonov prior pulling each Δδtˢ toward 0 with
// σ = initial_std.sat_clock (the documented "Δδtˢ vs a-priori product clock"
// quality: sub-m precise, looser for RT broadcast) regularizes ONLY the
// ill-conditioned cold-start direction: the per-constellation clock DATUM
// stays fixed by the explicit Datum (a large-weight pin that dominates this
// soft prior), so the few-metre datum offset the data determines survives.
// Applied POST-gate (like the carry-forward) so the null-space rank gate
// still sees the clock datum as a gauge, and it combines in information space
// with the carry-forward (anchor-toward-0 + RW-toward-previous).  Gated to a
// BINDING std (< 100 m): the legacy loose default (1000 m) leaves the anchor
// off, so every existing network config is byte-identical.
//
// SCOPE (measured, 305-sta GPS+Gal, 2026-06-30; see [[project_global_sh_iono]]):
// σ=0.05 m collapses cold-start clock-Δ rms 56→2.9 m for ~6 epochs, then the
// W6 clock↔bias↔ambiguity systematic (data wants ~80 m, info ≫ the anchor)
// reasserts.  This improves cold-start CLOCK-PRODUCT quality only — the
// co-estimated SH-VTEC field divergence is a SEPARATE, DECOUPLED cold-start
// estimability defect (its large-scale level is unobservable under the
// common-mode-free iono R); pinning the clock does NOT move the field.
{
    const double clk_sigma = clk_anchor_sigma;  // m (D6-resolved; sp3 auto == initial_std.sat_clock)
    // Broadcast-anchored datum: the SAME per-satellite anchors were already
    // folded PRE-gate as the gauge realization (buildSharedDatum constraint
    // rows) — folding them again here would double the anchor information
    // (effective σ/√2).  Skip; every other mode keeps the post-gate
    // regularizer exactly as before.
    if (!clock_brdc_anchored
        && std::isfinite(clk_sigma) && clk_sigma > 0.0 && clk_sigma < 100.0) {
        const double clk_info = 1.0 / (clk_sigma * clk_sigma);
        std::vector<NetworkEstimator::SharedPrior> clk_anchor;
        clk_anchor.reserve(registered_clock_sats.size());
        for (const std::string& sat : registered_clock_sats) {
            NetworkEstimator::SharedPrior p;
            p.kind  = SharedParamKind::Clock;
            p.sat   = sat;
            p.value = 0.0;          // anchor Δδtˢ toward the a-priori clock
            p.info  = clk_info;
            clk_anchor.push_back(p);
        }
        est.attribTag("clk_anchor");
        est.addSharedPrior(clk_anchor);
        if (within_epoch_armed)
            cap_priors.insert(cap_priors.end(), clk_anchor.begin(), clk_anchor.end());
    }
}

// A clock-pivot handover re-references the whole shared solution; the
// forced bump steps BOTH phase and code discontinuity counters next
// emit (the code per-(const,signal) sum-to-zero and IF-pin re-references
// are detected by the emitter from the emitted epoch itself — see
// NetworkProductEmitter; only this clock-pivot signal is driver-known).
// Both the emitter datum bump (markDatumChange) AND the prev_pivot /
// last_pivot baseline advance are DEFERRED to the emit site: the maps
// must describe the datum of the last EMITTED product (the consumer's
// view).  Advancing them here — before the solve — would make a handover
// epoch that is subsequently skipped (solve failure, σ̂0 withhold)
// compare equal next epoch: the discontinuity signal would be lost
// forever AND the old-datum prev_carry/clock_osc would fold as priors
// into the new-datum solve.  With the baseline frozen across skipped
// epochs, every skip path re-detects the handover until one epoch
// actually emits in the new datum.

Eigen::VectorXd x_s;
try {
    // parallel_carry: the WHOLE est.solve() is Eigen-threaded work — foldReducedDdPriors
    // fans its per-station reduction across threads itself (Eigen PINNED), and its dense
    // GEMMs + the K×K middle inverse run under Eigen's own threading.  So, unlike the two
    // isolated build/install GEMM stages, solve() is NOT wrapped in a CarryEigenScope: an
    // un-pin here would nest the fold's OpenMP with Eigen's GEMM threads.  It is also why
    // parallel_carry cannot coexist with parallel_reduce (which would OpenMP-fan this same
    // Eigen-threaded solve across stations) — the loader keeps them mutually exclusive.
    x_s = est.solve(plan.shared_datum);
    // Card 1d debug: full-precision solve checksum for A/B-arm differencing
    // (print-precision-proof).  One-shot budget; log-only.
    if (carry_cpl_cond && n_cpl_solve_dump > 0) {
        --n_cpl_solve_dump;
        log->info("Network (Card 1d dbg): solve checksum TOW {:.1f}: sum={:.17e} "
                  "norm={:.17e} n={}", se.time.tow, x_s.sum(), x_s.norm(), x_s.size());
    }
} catch (const std::exception& ex) {
    log->warn("Network: solve failed at TOW {:.1f} ({}); skipping epoch",
              se.time.tow, ex.what());
    ++n_skipped;
    { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
      es.datum_changed = datum_changed ? 1 : 0;
      emitEpochStatus("solve_failed", es); }   // x_s empty — no clock stats
    prev_week = se.time.week; prev_tow = se.time.tow;
    result.action = EpochAction::SkipEpoch;
    return result;
}

// Satellites whose carried SHARED state (clock + coupled phase/code bias) is to be
// REFLOATED at the end-of-epoch stash: the per-satellite clock monitor (below, the
// detector for a lone lock-in the σ̂0 RAIM misses) plus — only when
// clock_monitor_couple_raim is opted in — every satellite the within-epoch / post-fit
// RAIM already flags (so a slip that leaked into the shared clock reinitializes the
// clock too, not just the ambiguity).  Empty unless network.clock_monitor, so the stash
// stays byte-identical when off.
std::set<std::string> clock_refloat_sats;
int n_arc_refloat_epoch = 0;   // T2d: isolated-arc RAIM refloats executed this epoch (heartbeat metric)
// B1 retro-bump / URA-inflate feed (P0-2): the union of satellites ANY QC tier
// flags this epoch, independent of clock_monitor_couple_raim.  clock_refloat_sats
// only captures the RAIM tiers under the couple opt-in (it drives the CARRY
// refloat, a numeric action), so a separate accumulator is needed so the
// retro-bump reaches every tier without forcing a carry refloat.  Populated only
// when a URA feature that consumes it is on; inert (byte-identical) otherwise.
std::set<std::string> retro_flag_sats;
const bool retro_feed = ura_retro || ura_refloat_inflate > 1.0;

// --- Card Q3: the guarded temporal clock anchor (docs/SPEC_clock_temporal_anchor.md) ---
// MUST run here — after the primary solve, BEFORE the within-epoch adaptation
// below.  A stiff clock prior fighting a genuine step dumps the tension into the
// residuals and raises σ̂0; the adaptation would then attribute a PRIOR fault to a
// satellite's or a station's DATA and drop it.  A prior fault is resolved first,
// by refloating the prior (never by de-weighting a measurement) and re-solving the
// epoch in place — so the assessment below sees the corrected solve and the epoch
// EMITS (adapt-and-emit; a withhold here would be an adaptation failure).
// clock_refloat_sats is empty at this point on every path except a warm carry of
// the previous epoch's flags, so the guard's `ext` coupling reads exactly the
// tiers that have already spoken.  No-op (not even a state allocation) when off.
networkClockAnchorGuard(ctx, config_, log, traces, se.time, est, plan, x_s, ws,
                        registered_clock_sats, clock_refloat_sats, datum_changed,
                        changed_consts);

// --- Within-epoch adaptation: drop the dominant culprit(s) and re-solve --------
// When σ̂0 (the robust a-posteriori variance factor) shows the joint solve is
// contaminated, peel the single most-prominent culprit — a whole SATELLITE (its
// rows de-weighted network-wide) or a whole STATION (excluded) — re-solve THIS epoch
// over a fresh beginEpoch() replay of the captured shared system, and repeat until
// σ̂0 falls below the trigger or a cap.  Only the (rare, already-contaminated) trigger
// epochs pay the re-solve; try/catch reverts to the last good solve on any failure.
// Uses the same robust median-polish attribution as the RAIM refloat.
int  n_adapt = 0;
bool we_adapt_attempted = false;   // a within-epoch re-solve was launched (est layout was rebuilt)
double sigma0_adapt = 0.0;         // A4: final within-epoch σ̂0 (0 = adaptation did not run this epoch)
// SPEC_withhold_era_release: era continuity resolved ONCE per epoch (the census-
// freshness idiom — the era lives iff the immediately-previous LOOP epoch was the
// last A4-withheld decide; any bypass/skip epoch breaks it, C5/C7).  Declared at
// function scope like sigma0_adapt because the A4 decision site consumes both
// after the within-epoch block closes.
const bool wh_era_live = ctx.carry.wh_era_len > 0 &&
    ctx.carry.wh_last_loop_ep ==
        static_cast<long long>(ctx.counters.n_loop_epochs) - 1;
bool wh_escalated = false;         // §3(a) budget escalation in effect THIS epoch
// RTA-10 (network.per_const_withhold): per-CONSTELLATION partition of the LAST
// assess()'s standardized-residual pool — the very rows whose median is the global
// σ̂0 the A4 withhold compares, keyed by the ambiguity's satellite constellation
// (pa.sat[0]).  Cleared and refilled on every assess() so, like sigma0_adapt, it
// describes the FINAL within-epoch solve.  Populated only when the carve-out is
// armed (empty otherwise — no cost, no behavior change on the default path).
std::map<char, std::vector<double>> we_sigma0_pool_c;
// R12: the B8/T2 flag-rate (first-solve σ̂0 > trigger) is DECIDED here but COMMITTED at
// the emit site, so a subsequently-withheld epoch is excluded from T2's denominator —
// matching T3–T6, which only accrue on emitted epochs.  Without this, T2 counted withheld
// (contaminated) epochs while the post-fit tiers did not, giving incomparable alphas.
bool we_t2_opp = false, we_t2_flag = false;
// R0 (SPEC_pivot_visibility_gauge_defect §5.07.5): reset the tier's per-epoch
// observability EVERY epoch that reaches the solve, so an epoch the tier does
// not assess leaves the epoch-status columns blank instead of repeating the
// last assessed epoch's numbers ("unavailable is not 0").
ctx.carry.ep_sigma0_first    = -1.0;
ctx.carry.ep_we_refloat_sats = -1;
ctx.carry.ep_we_clk_move_max = -1.0;
ctx.carry.ep_we_clk_move_sat.clear();
if (within_epoch_armed && carry_warm) {
    const double trig      = config_.networkRaimSigma0Trigger();
    const bool   surgical  = config_.networkRaimSurgicalStation();
    const double sta_bias  = surgical ? config_.networkRaimStationDropBias() : 1.0;
    constexpr int    kMaxAdapt   = 5;
    // SPEC_withhold_era_release §3(a): inside a live withhold era the budget
    // escalates so the era can end the SANCTIONED way — a landed adaptation
    // takes the normal accepted path and the full downstream healing (harvest +
    // post-fit RAIM commit) runs.  Nothing else changes: same culprit scoring,
    // same k-gate, same σ̂0, same ceiling.  Measured motivation (fld63 29-epoch
    // era): n_adapt pinned at kMaxAdapt on every era epoch with culprits still
    // clearing the k-gate.  Loader guarantees wh_escalate is 0 or > kMaxAdapt.
    wh_escalated = wh_era_live && ctx.knobs.wh_escalate > kMaxAdapt;
    const int max_adapt_eff = wh_escalated ? ctx.knobs.wh_escalate : kMaxAdapt;
    // --- WE-CI (network.raim_we_code_iono): code + iono-constraint row pools ----
    // The historical pool below is the carried-ambiguity PHASE rows; the code and
    // per-LOS↔SH iono-constraint rows were screened by NO tier (P1-4 skips
    // SH-touching rows by design), which is exactly where the DOY-129 19:26:00
    // SH detonation entered.  When armed, assess() additionally pools them into
    // per-pool robust σ̂0 with their own triggers, and the adaptation loop gains
    // two response classes: epoch-scoped station exclusion (the measured
    // BSHM/GOLD epicenter pattern) and network-wide satellite code-row de-weight
    // (σ×10 — fallback, not drop).  The phase pool, σ̂0_adapt and the A4 ceiling
    // are untouched; every new branch is behind we_ci, so OFF is byte-identical.
    const bool   we_ci     = config_.networkRaimWeCodeIono();
    const double trig_code = config_.networkRaimWeCodeTrigger();
    const double trig_iono = config_.networkRaimWeIonoTrigger();
    constexpr double kWeCiCodeWeightScale = 1e-2;   // de-weighted code row: w×1e-2 = σ×10
    // Shared-column classification, built once per epoch — the shared layout is
    // frozen for the epoch, so the maps stay valid across within-epoch re-solves.
    // Same label source and prefixes as the P1-4 pass (NetworkFrameDiag.cpp).
    std::vector<char> ci_sh_col, ci_cb_col;
    std::vector<std::string> ci_col_sat;
    if (we_ci) {
        const std::size_t nsh = static_cast<std::size_t>(est.numShared());
        ci_sh_col.assign(nsh, 0);
        ci_cb_col.assign(nsh, 0);
        ci_col_sat.assign(nsh, {});
        for (std::size_t u = 0; u < nsh; ++u) {
            std::string lbl;
            try { lbl = est.sharedColumnLabel(static_cast<Eigen::Index>(u)); }
            catch (...) { continue; }
            if (lbl.rfind("SH", 0) == 0) { ci_sh_col[u] = 1; continue; }
            if (lbl.rfind("CB:", 0) == 0) ci_cb_col[u] = 1;
            const std::size_t p1 = lbl.find(':');
            if (p1 != std::string::npos) {
                const std::size_t p2 = lbl.find(':', p1 + 1);
                ci_col_sat[u] = lbl.substr(p1 + 1, (p2 == std::string::npos)
                                                       ? std::string::npos : p2 - p1 - 1);
            }
        }
    }
    double s0_code_we = 0.0, s0_iono_we = 0.0;      // last assess()'s CI pool σ̂0
    std::map<std::string, double> ci_sat_code_max;  // sat → max standardized code row (last assess)
    std::map<int, double> ci_sta_max;               // station → max standardized code/iono row (last assess)
    std::set<std::string> dw_code_sats;             // chosen sat code-de-weight responses
    // Classify one data row against the shared maps: sh_row = iono-constraint row
    // (touches an SH column, which short-circuits); cb_row = touches a CB column;
    // row_sat = first sat-labelled shared column (the attribution key).
    auto ci_classify = [&](const StationAssembly& R, Eigen::Index row,
                           bool& sh_row, bool& cb_row, const std::string*& row_sat) {
        sh_row = false; cb_row = false; row_sat = nullptr;
        for (Eigen::Index c = 0; c < R.A_s.cols(); ++c) {
            if (R.A_s(row, c) == 0.0) continue;
            const std::size_t u = static_cast<std::size_t>(c);
            if (u < ci_sh_col.size() && ci_sh_col[u]) { sh_row = true; return; }
            if (u < ci_cb_col.size() && ci_cb_col[u]) cb_row = true;
            if (!row_sat && u < ci_col_sat.size() && !ci_col_sat[u].empty())
                row_sat = &ci_col_sat[u];
        }
    };

    std::vector<char> orig_added(stations.size(), 0);
    for (size_t i = 0; i < stations.size(); ++i)
        orig_added[i] = (est_index_of[i] >= 0) ? 1 : 0;

    // Culprit sets.  A bad AMBIGUITY (cycle slip — the dominant contamination the
    // carried-phase RAIM sees) needs REINITIALIZATION, not measurement de-weighting: the
    // re-solve REFLOATS the culprit's ambiguities (withholds their stale carry prior) so
    // they re-estimate from full-weight data and absorb the slip locally.  A whole bad
    // STATION (a data-quality problem, not a slip) is EXCLUDED — the de-weight/exclude
    // tool, which is the right one for a bad measurement.
    std::set<std::string> drop_sats;                       // refloat that sat's ambiguities network-wide
    std::set<int>         drop_stations;                   // excluded entirely (bad-data case)
    std::map<int, std::set<std::string>> drop_arcs;        // (station → sats) refloat at that station ONLY
    // C3c §3: the DD-carry re-solve (buildCarryDdPriors, via resolve_dropping)
    // treats a mid-epoch RAIM culprit as slipped, so its stale carry is not
    // re-folded (refold from unchanged predicted state, H13 + culprit-withhold).
    // Aliases live only while this block runs; nulled before the sets leave scope.
    carry_we_drop_sats = &drop_sats;
    carry_we_drop_arcs = &drop_arcs;

    // σ̂0 + median-polish effects of the CURRENT shared solution.  The (station × sat)
    // residual table (carried-ambiguity PHASE rows only) decomposes additively into a
    // per-satellite column effect, a per-station row effect, and an isolated-arc
    // interaction.  Computed over EVERY present ambiguity — a refloated ambiguity fits
    // its own data (its residual collapses after the re-solve), so σ̂0 stays an HONEST
    // measure of the REMAINING contamination without special-casing; the culprit is kept
    // out of the next pick by the candidate-set exclusion, not by hiding it here.
    auto assess = [&](const Eigen::VectorXd& xs, double& sigma0_out,
                      std::map<std::string, double>& sat_eff,
                      std::map<int, double>& sta_eff,
                      std::map<std::pair<int, std::string>, double>& arc_eff) {
        std::vector<double> r_pool;
        std::vector<double> ci_code_pool, ci_iono_pool;   // WE-CI per-pool standardized rows
        if (we_ci) { ci_sat_code_max.clear(); ci_sta_max.clear(); }
        if (perconst_on) we_sigma0_pool_c.clear();   // RTA-10: re-partition per assess
        std::map<std::pair<int, std::string>, double> cell_max;
        for (size_t i = 0; i < stations.size(); ++i) {
            // Prioritized: keep the former pivot in the σ̂0 pool — it now
            // carries fault-testable non-anchored arcs; its anchored (pinned)
            // keys are already absent from R.pl_amb (per-(sat,signal) partition).
            if (!orig_added[i] || est_index_of[i] < 0
                || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
            const StationAssembly& R = asm_res[i];
            if (R.A_s.cols() != xs.size()) continue;
            // WE-CI also screens stations with NO carried ambiguity (their code
            // and iono rows are data too); OFF keeps the historical skip.
            if (R.pl_amb.empty() && !we_ci) continue;
            Eigen::VectorXd x_r;
            try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), xs); }
            catch (...) { continue; }
            if (x_r.size() != R.A_r.cols()) continue;
            const Eigen::VectorXd resid = R.y - R.A_s * xs - R.A_r * x_r;
            Eigen::VectorXd row_r;   // B2: per-row Baarda redundancy (empty when off)
            if (raim_redun) {
                try {
                    row_r = est.rowRedundancy(static_cast<std::size_t>(est_index_of[i]),
                                              R.A_s, R.A_r, R.w, plan.shared_datum);
                } catch (...) { row_r.resize(0); }
            }
            for (const auto& pa : R.pl_amb) {
                if (pa.col < 0 || pa.col >= R.A_r.cols()) continue;
                double r_max = 0.0;
                for (Eigen::Index row = 0; row < R.A_r.rows(); ++row) {
                    if (std::abs(R.A_r(row, pa.col)) <= 0.5) continue;
                    const double w = (row < R.w.size()) ? R.w(row) : 1.0;
                    double r = std::abs(resid(row)) * std::sqrt(std::max(0.0, w));
                    if (raim_redun && row < row_r.size()) {   // B2: standardize v/(σ·√r_i)
                        constexpr double kRedFloor = 1e-6;
                        const double ri = row_r(row);
                        if (ri <= kRedFloor) continue;   // uncheckable row: exclude from σ̂0 pool + r_max
                        r /= std::sqrt(ri);
                    }
                    r_pool.push_back(r);
                    // RTA-10: the SAME standardized row (weighting and B2 √r_i
                    // normalization included; redundancy-floored rows excluded by
                    // the continue above), partitioned by constellation.
                    if (perconst_on && !pa.sat.empty())
                        we_sigma0_pool_c[pa.sat[0]].push_back(r);
                    if (r > r_max) r_max = r;
                }
                const auto key = std::make_pair(static_cast<int>(i), pa.sat);
                auto it = cell_max.find(key);
                if (it == cell_max.end() || r_max > it->second) cell_max[key] = r_max;
            }
            // --- WE-CI: pool this station's code + iono-constraint rows ---------
            if (we_ci) {
                std::vector<char> is_amb_col(static_cast<std::size_t>(R.A_r.cols()), 0);
                for (const auto& pa : R.pl_amb)
                    if (pa.col >= 0 && pa.col < R.A_r.cols())
                        is_amb_col[static_cast<std::size_t>(pa.col)] = 1;
                const Eigen::Index data_rows =
                    (R.cpl_row_start >= 0) ? R.cpl_row_start : R.A_r.rows();
                double sta_max = 0.0;
                for (Eigen::Index row = 0; row < data_rows && row < resid.size(); ++row) {
                    bool sh_row = false, cb_row = false;
                    const std::string* row_sat = nullptr;
                    ci_classify(R, row, sh_row, cb_row, row_sat);
                    if (!sh_row && !cb_row) continue;          // phase/clock-only row
                    if (!sh_row) {
                        bool phase_row = false;                // the phase pool's rows stay its own
                        for (Eigen::Index c = 0; c < R.A_r.cols() && !phase_row; ++c)
                            if (is_amb_col[static_cast<std::size_t>(c)]
                                && std::abs(R.A_r(row, c)) > 0.5)
                                phase_row = true;
                        if (phase_row) continue;
                    }
                    const double w = (row < R.w.size()) ? R.w(row) : 1.0;
                    double r = std::abs(resid(row)) * std::sqrt(std::max(0.0, w));
                    if (raim_redun && row < row_r.size()) {    // B2, same as the phase pool
                        constexpr double kRedFloor = 1e-6;
                        const double ri = row_r(row);
                        if (ri <= kRedFloor) continue;
                        r /= std::sqrt(ri);
                    }
                    if (!std::isfinite(r)) continue;
                    (sh_row ? ci_iono_pool : ci_code_pool).push_back(r);
                    if (r > sta_max) sta_max = r;
                    if (!sh_row && row_sat) {
                        double& m = ci_sat_code_max[*row_sat];
                        if (r > m) m = r;
                    }
                }
                if (sta_max > 0.0) ci_sta_max[static_cast<int>(i)] = sta_max;
            }
        }
        sigma0_out = 1.0;
        if (!r_pool.empty()) {
            std::nth_element(r_pool.begin(), r_pool.begin() + r_pool.size() / 2, r_pool.end());
            sigma0_out = std::max(raim_s0_floor, 1.4826 * r_pool[r_pool.size() / 2]);
        }
        std::vector<std::tuple<int, std::string, double>> tbl;
        tbl.reserve(cell_max.size());
        for (const auto& [k, v] : cell_max) tbl.emplace_back(k.first, k.second, v);
        const MedianPolish mp = medianPolish2Way(tbl);
        sat_eff = mp.col_eff;
        sta_eff = mp.row_eff;
        arc_eff.clear();
        for (std::size_t k = 0; k < tbl.size(); ++k)
            arc_eff[std::make_pair(std::get<0>(tbl[k]), std::get<1>(tbl[k]))] = mp.interaction[k];
        if (we_ci) {   // WE-CI per-pool robust σ̂0 (same estimator as the phase pool)
            auto pool_s0 = [](std::vector<double>& p) -> double {
                if (p.empty()) return 0.0;
                std::nth_element(p.begin(),
                                 p.begin() + static_cast<std::ptrdiff_t>(p.size() / 2),
                                 p.end());
                return 1.4826 * p[p.size() / 2];
            };
            s0_code_we = pool_s0(ci_code_pool);
            s0_iono_we = pool_s0(ci_iono_pool);
        }
    };

    // Replay the epoch over a fresh arrowhead (beginEpoch reopens the frozen layout;
    // only reset() would clear it), REFLOATING the culprit ambiguities.  Rebuilds
    // est_index_of so reconstructStation / harvest stay valid on the final solve.  The
    // measurements stay at FULL weight — de-weighting is the tool for a bad measurement,
    // whereas a slipped ambiguity needs a new VALUE.  A satellite culprit refloats its
    // ambiguities at every station; an arc culprit at one station only; a whole-station
    // culprit is excluded (its data, not a single ambiguity, is bad).  A refloated
    // ambiguity keeps its full-weight data (so its local block stays positive-definite —
    // no drop-induced singularity) but WITHHOLDS its carry prior, so the re-solve lets it
    // find its new value and absorb the slip locally; the harvest then re-stashes that
    // corrected value (self-contained reinit — no reliance on the post-fit refloat).
    auto resolve_dropping = [&]() -> Eigen::VectorXd {
        est.beginEpoch();
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!orig_added[i] || drop_stations.count(static_cast<int>(i))) {
                est_index_of[i] = -1;
                continue;
            }
            const StationAssembly& R = asm_res[i];
            // WE-CI: de-weight the culprit satellites' code rows (σ×10) in a
            // per-station copy; `*wr` binds R.w itself on the OFF path and
            // whenever no row matches, so the default numbers are untouched.
            Eigen::VectorXd w_ci;
            const Eigen::VectorXd* wr = &R.w;
            if (we_ci && !dw_code_sats.empty()) {
                std::vector<char> is_amb_col(static_cast<std::size_t>(R.A_r.cols()), 0);
                for (const auto& pa : R.pl_amb)
                    if (pa.col >= 0 && pa.col < R.A_r.cols())
                        is_amb_col[static_cast<std::size_t>(pa.col)] = 1;
                const Eigen::Index dw_data_rows =
                    (R.cpl_row_start >= 0) ? R.cpl_row_start : R.A_r.rows();
                for (Eigen::Index row = 0; row < dw_data_rows && row < R.w.size(); ++row) {
                    bool sh_row = false, cb_row = false;
                    const std::string* row_sat = nullptr;
                    ci_classify(R, row, sh_row, cb_row, row_sat);
                    if (sh_row || !cb_row || !row_sat || !dw_code_sats.count(*row_sat))
                        continue;
                    bool phase_row = false;
                    for (Eigen::Index c = 0; c < R.A_r.cols() && !phase_row; ++c)
                        if (is_amb_col[static_cast<std::size_t>(c)]
                            && std::abs(R.A_r(row, c)) > 0.5)
                            phase_row = true;
                    if (phase_row) continue;
                    if (w_ci.size() == 0) w_ci = R.w;
                    w_ci(row) *= kWeCiCodeWeightScale;
                }
                if (w_ci.size() > 0) wr = &w_ci;
            }
            // A_r columns of THIS station's ambiguities to refloat (withhold their prior).
            // A SATELLITE culprit refloats its ambiguities at every station tracking it: a
            // satellite-level contamination pulls the shared clock at ALL its stations, and
            // its arcs are contaminated to varying degrees — a strict per-arc gate on the
            // (inflated) σ̂0 would miss the moderately-slipped ones and leave a worse
            // mixed state, so the whole satellite is reinitialized consistently.  An ARC
            // culprit refloats at ONE station only (surgical, isolated slip).
            std::set<Eigen::Index> refloat_cols;
            const auto dai = drop_arcs.find(static_cast<int>(i));
            for (const auto& pa : R.pl_amb)
                if (drop_sats.count(pa.sat) ||
                    (dai != drop_arcs.end() && dai->second.count(pa.sat)))
                    refloat_cols.insert(pa.col);
            // Card 1: a refloated ambiguity's carry is a coupling ROW inside A_s/A_r
            // (not a carry_cols diagonal prior), so withholding it means ZEROING that
            // row before re-adding the block — the coupling-row analogue of the
            // diagonal-path prior withhold below.  The refloated ambiguity keeps its
            // full-weight DATA rows, so its local block stays PD.  A copy so the
            // primary R (and any iterated replay) is untouched.
            if (carry_cpl_cond && carry_cpl_joint && !R.cpl_raw.empty()
                && !refloat_cols.empty() && R.cpl_row_start >= 0) {
                // Card 1b: the joint whitening MIXES a station's coupling rows, so a
                // refloat cannot zero individual output rows — re-whiten the SURVIVING
                // rows (dropping the refloated locals) and re-append them after the
                // unchanged data rows.  A re-whiten failure drops all coupling rows for
                // the epoch (safe degrade: the refloated arcs re-float from data anyway).
                Eigen::MatrixXd Hr, Hs;
                Eigen::VectorXd z;
                const bool ok = whitenCouplingRows(R.cpl_raw, R.cpl_white_pw, refloat_cols,
                                                   R.A_r.cols(), R.A_s.cols(), Hr, Hs, z);
                const Eigen::Index nd = R.cpl_row_start;             // data + SH rows (unchanged)
                const Eigen::Index nk = ok ? Hr.rows() : 0;          // surviving coupling rows
                Eigen::MatrixXd As(nd + nk, R.A_s.cols()), Ar(nd + nk, R.A_r.cols());
                Eigen::VectorXd rw(nd + nk), ry(nd + nk);
                As.topRows(nd) = R.A_s.topRows(nd);
                Ar.topRows(nd) = R.A_r.topRows(nd);
                rw.head(nd)    = wr->head(nd);
                ry.head(nd)    = R.y.head(nd);
                if (nk > 0) {
                    As.bottomRows(nk) = Hs;
                    Ar.bottomRows(nk) = Hr;
                    rw.tail(nk).setOnes();
                    ry.tail(nk) = z;
                }
                est_index_of[i] = static_cast<int>(est.addStation(As, Ar, rw, ry));
            } else if (carry_cpl_cond && !refloat_cols.empty() && R.cpl_row_start >= 0) {
                Eigen::MatrixXd As = R.A_s, Ar = R.A_r;
                Eigen::VectorXd rw = *wr, ry = R.y;
                // F3 (network.phase_leveling_local_block): the whitening MIXES the
                // block's rows (each output row is a row of L⁻¹ over ALL block columns),
                // so a refloated column cannot be withheld by zeroing its own row — the
                // refloated columns are MARGINALIZED (dropped from Σ) and the SURVIVORS
                // re-whitened: the Card-1b re-whiten above, on the local block.
                // Survivors land in their own row slots; the dropped columns' slots stay
                // zero (the scalar loop below zeroes them again — consistent, since
                // cpl_row_local_cols carries each block row's PRIMARY column).  A
                // non-SPD re-whiten drops the whole block for this re-solve (safe
                // degrade: the refloated arcs re-float from data anyway).  Both outcomes
                // are counted: this IS invariant #7's re-coupling edge for the block.
                // Empty pl_blk (knob off / no block) ⇒ untouched, byte-identical.
                if (!R.pl_blk.rows.empty()) {
                    bool hit = false;
                    for (const CplRowRaw& br : R.pl_blk.rows)
                        if (refloat_cols.count(br.local_col)) { hit = true; break; }
                    const Eigen::Index b0 = R.cpl_row_start + R.pl_blk.row0;
                    const Eigen::Index nb = static_cast<Eigen::Index>(R.pl_blk.rows.size());
                    if (hit && b0 >= 0 && b0 + nb <= As.rows()) {
                        for (Eigen::Index k = 0; k < nb; ++k) {
                            As.row(b0 + k).setZero(); Ar.row(b0 + k).setZero();
                            rw(b0 + k) = 0.0; ry(b0 + k) = 0.0;
                        }
                        Eigen::MatrixXd Hr, Hs;
                        Eigen::VectorXd z;
                        std::vector<Eigen::Index> kept;
                        if (whitenLocalBlock(R.pl_blk, refloat_cols, R.A_r.cols(),
                                             R.A_s.cols(), Hr, Hs, z, kept)) {
                            for (std::size_t k = 0; k < kept.size(); ++k) {
                                const Eigen::Index slot = b0 + kept[k];
                                const Eigen::Index kk = static_cast<Eigen::Index>(k);
                                As.row(slot) = Hs.row(kk);
                                Ar.row(slot) = Hr.row(kk);
                                rw(slot) = 1.0;
                                ry(slot) = z(kk);
                            }
                            ++ctx.counters.n_pl_blk_refloat_rewhiten;
                        } else {
                            ++ctx.counters.n_pl_blk_refloat_fail;
                        }
                    }
                }
                for (std::size_t rr = 0; rr < R.cpl_row_local_cols.size(); ++rr)
                    if (refloat_cols.count(R.cpl_row_local_cols[rr])) {
                        const Eigen::Index row =
                            R.cpl_row_start + static_cast<Eigen::Index>(rr);
                        As.row(row).setZero(); Ar.row(row).setZero();
                        rw(row) = 0.0; ry(row) = 0.0;
                    }
                est_index_of[i] = static_cast<int>(est.addStation(As, Ar, rw, ry));
            } else {
                est_index_of[i] = static_cast<int>(est.addStation(R.A_s, R.A_r, *wr, R.y));
            }
            // B9: re-inject the pivot's ZWD carry too (its R.carry_cols is empty
            // unless network.carry_zwd), so the within-epoch re-solve matches the
            // primary solve.  refloat_cols only ever names ambiguity columns, so a
            // pivot's lone ZWD column is never in it (empty → the fast path).
            // Gate MIRRORS the primary solve's install site (ApplicationNetworkAssembly,
            // `(carry_warm || rx_cb_on || rx_cb_datum) && !R.carry_cols.empty()`) so the
            // re-solve folds exactly the priors the primary one did.  BYTE-IDENTICAL
            // today — this lambda lives inside `if (within_epoch_armed && carry_warm)`,
            // so carry_warm is already true here and the two added disjuncts cannot
            // change the predicate.  It is written out anyway because decoupling the
            // within-epoch tier from carry_warm is a sanctioned open fix (CLAUDE.md,
            // standard armed set): the moment that lands, a bare `carry_warm` here
            // would silently drop the rx-CB carry and RXCB-DATUM rows the primary
            // solve folded, and the replay would disagree with the solve it replays.
            if ((carry_warm || ctx.knobs.rx_cb_on || ctx.knobs.rx_cb_datum)
                && !R.carry_cols.empty()) {
                if (refloat_cols.empty()) {
                    est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                        R.carry_cols, R.carry_dNrr, R.carry_dur);
                } else {
                    // Inject the carry prior for every carried ambiguity EXCEPT the
                    // refloated ones (those are left free → re-estimated from data).
                    std::vector<Eigen::Index> cc;
                    Eigen::VectorXd cdn(R.carry_cols.size()), cdu(R.carry_cols.size());
                    cc.reserve(R.carry_cols.size());
                    Eigen::Index m = 0;
                    for (std::size_t k = 0; k < R.carry_cols.size(); ++k)
                        if (!refloat_cols.count(R.carry_cols[k])) {
                            cc.push_back(R.carry_cols[k]);
                            cdn(m) = R.carry_dNrr(static_cast<Eigen::Index>(k));
                            cdu(m) = R.carry_dur(static_cast<Eigen::Index>(k));
                            ++m;
                        }
                    if (!cc.empty()) {
                        cdn.conservativeResize(m);
                        cdu.conservativeResize(m);
                        est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                            cc, cdn, cdu);
                    }
                }
            }
        }
        est.attribTag("replay_refold");
        if (cap_has_constraint) est.addSharedConstraintRows(cap_H, cap_z, cap_constraint_weight);
        // Re-apply the sum-to-zero clock gauge on the fresh arrowhead (plan is
        // in scope; its rows are layout-frozen for the epoch so they match the
        // primary solve).  No-op / byte-identical in PIN mode (0 rows).
        if (plan.clock_constraint_H.rows() > 0)
            est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                        plan.clock_constraint_weight);
        // SBC completion rows: re-applied verbatim (EpochWorkspace writer-site
        // contract) so the replayed solve carries the SAME completed datum —
        // the weight is the compile constant, so none travels in the capture.
        if (cap_sbc_H.rows() > 0)
            est.addSharedConstraintRows(cap_sbc_H, cap_sbc_z,
                                        NetworkSbasis::kSbasisCompleteWeight);
        // Card C-GF: the replay must fold the SAME prior the primary solve folded.  It
        // previously called the plain accumulator unconditionally, so with the gauge-free
        // downdate armed the replayed system carried information along the datum gauge
        // that the primary solve had removed — and the within-epoch RAIM trip decision
        // was then taken against a DIFFERENT system than the one being tested.  Latent
        // while the downdate was reachable only through the Q3 anchor (outside the
        // standard armed set); live the moment it arms with the carry, since the
        // within-epoch tier IS in the standard armed set.
        if (!cap_priors.empty()) {
            if (ctx.knobs.carry_gauge_free)
                est.addSharedPriorClockGaugeFree(cap_priors,
                                                 ctx.knobs.carry_gauge_free_closure);
            else
                est.addSharedPrior(cap_priors);
        }
        // C1: replay the dense D2 fold on the fresh arrowhead.  The layout is
        // frozen for the epoch, so the key→column mapping and the LDLT are
        // bit-reproducible — the re-solve sees the identical shared system.
        // Non-empty only when the primary dense fold succeeded (its fallback
        // routed through cap_priors above), so this cannot throw where the
        // primary did not.
        if (!cap_dense_keys.empty())
            est.addSharedPriorDense(cap_dense_keys, cap_dense_cov, nullptr,
                                    ctx.knobs.carry_gauge_free,
                                    ctx.knobs.carry_gauge_free_closure);   // Card C-GF: same fold as the primary solve
        // Fix A: rebuild the ensemble phase-bias gauges on the fresh arrowhead
        // (beginEpoch reopened the layout but est_index_of was rebuilt above and
        // refloated stations may have dropped, so the stored gauges' station
        // indices are stale).  No-op when OFF.
        buildEnsembleGauges();
        // Card 4 (M1) C3b: rebuild the DD-prior blocks on the fresh arrowhead
        // (beginEpoch cleared them + est_index_of was rebuilt above).  The
        // predict is NOT re-run — only the fold is rebuilt.  No-op when OFF.
        buildCarryDdPriors();
        return est.solve(plan.shared_datum);
    };

    double sigma0 = 1.0;
    std::map<std::string, double> sat_eff;
    std::map<int, double> sta_eff;
    std::map<std::pair<int, std::string>, double> arc_eff;
    assess(x_s, sigma0, sat_eff, sta_eff, arc_eff);
    const double sigma0_first = sigma0;
    // R0: park the pre-adaptation σ̂₀ for the epoch-status row — this is the null
    // sample QUEUE item 4 needs on disk (it previously existed only inside a trip
    // event's detail span, so quiet epochs left nothing to fit).
    ctx.carry.ep_sigma0_first = sigma0_first;
    // R0 (§5.07.2 impulse-channel meter): the PRIMARY solve's satellite clocks,
    // captured before any within-epoch re-solve can move them.  The shared layout
    // is frozen for the epoch, so clockColumn stays valid across re-solves.
    std::map<std::string, double> we_clk_first;
    for (const std::string& s : registered_clock_sats) {
        try {
            const Eigen::Index c = est.clockColumn(s);   // throws if absent
            if (c >= 0 && c < x_s.size()) we_clk_first[s] = x_s(c);
        } catch (...) { /* no clock column — not in this epoch's layout */ }
    }
    if (trace_flag_rates) { we_t2_opp = true; we_t2_flag = (sigma0_first > trig); }  // B8/T2 (R12: committed at emit, below)
    if (we_ci) {   // invariant-#4 telemetry: first-assess trips + run maxima
        auto& sc = ctx.counters;
        if (s0_code_we > sc.we_ci_s0_code_max) sc.we_ci_s0_code_max = s0_code_we;
        if (s0_iono_we > sc.we_ci_s0_iono_max) sc.we_ci_s0_iono_max = s0_iono_we;
        if (s0_code_we > trig_code) ++sc.we_ci_code_trips;
        if (s0_iono_we > trig_iono) ++sc.we_ci_iono_trips;
    }
    std::string adapt_detail;
    for (int iter = 0;
         iter < max_adapt_eff
         && (sigma0 > trig
             || (we_ci && (s0_code_we > trig_code || s0_iono_we > trig_iono)));
         ++iter) {
        // Best NEW culprit per granularity (all gated on effect/σ̂0 > raim_k).  An
        // isolated ARC is the finest, least-destructive peel; a whole STATION is the
        // coarsest (it discards good arcs too) and is de-rated by sta_bias so it is
        // only chosen when it dominates the best satellite/arc by that margin.
        // Preference on equal score is ARC > SAT > STA (surgical-first tie-break).
        std::string best_arc_sat; int best_arc_sta = -1; double best_arc_eff = 0.0;
        if (surgical)
            for (const auto& [key, eff] : arc_eff) {
                const int s = key.first; const std::string& sat = key.second;
                if (drop_stations.count(s) || drop_sats.count(sat)) continue;
                const auto it = drop_arcs.find(s);
                if (it != drop_arcs.end() && it->second.count(sat)) continue;
                if (eff > best_arc_eff) { best_arc_eff = eff; best_arc_sta = s; best_arc_sat = sat; }
            }
        std::string best_sat; double best_sat_eff = 0.0;
        for (const auto& [sat, eff] : sat_eff)
            if (!drop_sats.count(sat) && eff > best_sat_eff) { best_sat_eff = eff; best_sat = sat; }
        int best_sta = -1; double best_sta_eff = 0.0;
        for (const auto& [sta, eff] : sta_eff)
            if (!drop_stations.count(sta) && eff > best_sta_eff) { best_sta_eff = eff; best_sta = sta; }

        // Consensus escalation: a satellite with ≥ raim_min_sta arcs currently over the
        // gate is a NETWORK-WIDE problem (its shared clock/phase-bias is corrupted), not a
        // stack of isolated arcs — reinitialize the whole satellite in ONE iteration (one
        // re-solve) rather than peeling arc by arc.  When the best arc belongs to such a
        // satellite, redirect to the satellite branch.  Reuses the RAIM cross-station
        // consensus count (§ post-fit RAIM).
        if (surgical && best_arc_sta >= 0 && !drop_sats.count(best_arc_sat)) {
            int n_bad = 0;
            for (const auto& [key, eff] : arc_eff) {
                if (key.second != best_arc_sat || drop_stations.count(key.first)) continue;
                const auto it = drop_arcs.find(key.first);
                if (it != drop_arcs.end() && it->second.count(key.second)) continue;
                if (eff / sigma0 > raim_k) ++n_bad;
            }
            if (n_bad >= raim_min_sta) {
                if (best_arc_eff > best_sat_eff) { best_sat = best_arc_sat; best_sat_eff = best_arc_eff; }
                best_arc_sta = -1;   // drop the whole satellite instead of this one arc
            }
        }

        enum class Kind { None, Arc, Sat, Sta } kind = Kind::None;
        double best_score = -1.0;   // strict '>' below → earlier (more surgical) wins ties
        if (best_arc_sta >= 0 && best_arc_eff / sigma0 > raim_k && best_arc_eff > best_score) {
            best_score = best_arc_eff; kind = Kind::Arc;
        }
        if (!best_sat.empty() && best_sat_eff / sigma0 > raim_k && best_sat_eff > best_score) {
            best_score = best_sat_eff; kind = Kind::Sat;
        }
        if (best_sta >= 0 && best_sta_eff / sigma0 > raim_k &&
            (best_sta_eff / sta_bias) > best_score) {
            best_score = best_sta_eff / sta_bias; kind = Kind::Sta;
        }
        bool ci_acted = false;
        if (kind == Kind::None && we_ci
            && (s0_code_we > trig_code || s0_iono_we > trig_iono)) {
            // WE-CI culprits (the phase effects had nothing left to peel): worst
            // station by its max code/iono row vs worst satellite by its
            // network-wide max code row, both raim_k-gated against the offending
            // pools' σ̂0.  Station exclusion is the epicenter response (BSHM/GOLD
            // class); a satellite culprit gets its code rows DE-WEIGHTED
            // (fallback, not drop).
            const double s0_ci = std::max({s0_code_we, s0_iono_we, 1e-9});
            int best_ci_sta = -1; double best_ci_sta_v = 0.0;
            for (const auto& [sta, v] : ci_sta_max)
                if (!drop_stations.count(sta) && v > best_ci_sta_v) {
                    best_ci_sta_v = v; best_ci_sta = sta;
                }
            std::string best_ci_sat; double best_ci_sat_v = 0.0;
            for (const auto& [sat, v] : ci_sat_code_max)
                if (!dw_code_sats.count(sat) && v > best_ci_sat_v) {
                    best_ci_sat_v = v; best_ci_sat = sat;
                }
            if (best_ci_sta >= 0 && best_ci_sta_v >= best_ci_sat_v
                && best_ci_sta_v / s0_ci > raim_k) {
                drop_stations.insert(best_ci_sta);
                ++ctx.counters.we_ci_sta_excl;
                ci_acted = true;
                adapt_detail += fmt::format("{}ci-sta:{}", adapt_detail.empty() ? "" : ",",
                                            stations[best_ci_sta].name);
            } else if (!best_ci_sat.empty() && best_ci_sat_v / s0_ci > raim_k) {
                dw_code_sats.insert(best_ci_sat);
                ++ctx.counters.we_ci_sat_dw;
                ci_acted = true;
                adapt_detail += fmt::format("{}ci-dwsat:{}",
                                            adapt_detail.empty() ? "" : ",", best_ci_sat);
            }
        }
        if (kind == Kind::None && !ci_acted) break;  // nothing bad enough left to drop

        if (kind == Kind::Arc) {
            drop_arcs[best_arc_sta].insert(best_arc_sat);
            adapt_detail += fmt::format("{}arc:{}/{}", adapt_detail.empty() ? "" : ",",
                                        stations[best_arc_sta].name, best_arc_sat);
        } else if (kind == Kind::Sat) {
            drop_sats.insert(best_sat);
            adapt_detail += fmt::format("{}sat:{}", adapt_detail.empty() ? "" : ",", best_sat);
        } else if (kind == Kind::Sta) {
            drop_stations.insert(best_sta);
            adapt_detail += fmt::format("{}sta:{}", adapt_detail.empty() ? "" : ",",
                                        stations[best_sta].name);
        }

        Eigen::VectorXd x_try;
        we_adapt_attempted = true;
        try { x_try = resolve_dropping(); }
        catch (const std::exception& ex) {
            // Should be unreachable now (the per-station full-weight retry uses the
            // known-PD original assembly).  Last resort: restore the clean no-drop solve
            // so the emit / harvest run on a consistent estimator rather than a
            // half-rebuilt one (the old "keep prior solve" left est mid-rebuild).
            log->warn("Network[RAIM within-epoch]: re-solve failed ({}); restoring clean solve.",
                      ex.what());
            drop_sats.clear(); drop_stations.clear(); drop_arcs.clear();
            try { x_s = resolve_dropping(); } catch (...) {}
            break;
        }
        x_s = x_try;
        ++n_adapt;
        assess(x_s, sigma0, sat_eff, sta_eff, arc_eff);
    }
    // R0: the settled clock displacement of this epoch's re-solves — (final −
    // primary) per satellite, the §5.07.2 impulse-channel meter.  Identically 0
    // when no re-solve ran (x_s is untouched), so only adapted epochs pay the
    // comparison loop.
    double      we_clk_move_max = 0.0;   // signed; compared by |·|
    std::string we_clk_move_sat;
    if (n_adapt > 0) {
        for (const auto& [s, v0] : we_clk_first) {
            double v1 = 0.0;
            try {
                const Eigen::Index c = est.clockColumn(s);
                if (!(c >= 0 && c < x_s.size())) continue;
                v1 = x_s(c);
            } catch (...) { continue; }
            const double dv = v1 - v0;
            if (std::abs(dv) > std::abs(we_clk_move_max)) {
                we_clk_move_max = dv;
                we_clk_move_sat = s;
            }
            // One trace row per satellite the tier measurably moved (> 1 mm) —
            // the per-identity channel; the epoch max goes to the status row.
            if (events_trace.is_open() && std::abs(dv) > 1e-3)
                emitEvent("T2_within_epoch", "we_clock_move", s, "", dv,
                          std::nullopt, "measure",
                          fmt::format("{:.4f}->{:.4f}", v0, v1));
        }
        // R0 run-end counters for the three response classes (previously only the
        // withhold had one) + the run-max clock move with its identity.
        auto& sc = ctx.counters;
        sc.we_refloat_sat_total += static_cast<long long>(drop_sats.size());
        {
            std::size_t na = 0;
            for (const auto& kv : drop_arcs) na += kv.second.size();
            sc.we_refloat_arc_total += static_cast<long long>(na);
        }
        sc.we_exclude_sta_total += static_cast<long long>(drop_stations.size());
        if (std::abs(we_clk_move_max) > std::abs(sc.we_clk_move_max_m)) {
            sc.we_clk_move_max_m   = we_clk_move_max;
            sc.we_clk_move_max_tow = se.time.tow;
            sc.we_clk_move_max_sat = we_clk_move_sat;
        }
    }
    if (n_adapt > 0) {
        log->info("Network[RAIM within-epoch]: TOW {:.1f} re-conditioned in {} re-solve(s) — "
                  "refloated {} sat / {} arc, {} sta excluded [{}]; σ̂0 {:.2f} → {:.2f} (trigger {:.2f}).",
                  se.time.tow, n_adapt, drop_sats.size(),
                  [&] { size_t n = 0; for (const auto& kv : drop_arcs) n += kv.second.size(); return n; }(),
                  drop_stations.size(), adapt_detail, sigma0_first, sigma0, trig);
        // T2: one event per settled culprit (whole-sat drop, per-(sta,sat) arc drop,
        // excluded station).  statistic=final σ̂0, threshold=trigger, detail=first->final.
        if (events_trace.is_open()) {
            const std::string span = fmt::format("{:.2f}->{:.2f}", sigma0_first, sigma0);
            for (const auto& s : drop_sats)
                emitEvent("T2_within_epoch", "we_refloat_sat", s, "", sigma0, trig,
                          "refloat", span);
            for (const auto& kv : drop_arcs)
                for (const auto& s : kv.second)
                    emitEvent("T2_within_epoch", "we_refloat_arc", s,
                              stations[kv.first].name, sigma0, trig, "refloat", span);
            for (int sta : drop_stations)
                emitEvent("T2_within_epoch", "we_exclude_station", "",
                          stations[sta].name, sigma0, trig, "exclude", span);
            for (const auto& s : dw_code_sats)   // WE-CI response class
                emitEvent("T2_within_epoch", "we_deweight_sat_code", s, "",
                          sigma0, trig, "deweight", span);
        }
    }

    // A within-epoch satellite refloat reinitialized that sat's AMBIGUITY at every
    // station; its coupled shared clock is just as suspect, so refloat the carried
    // clock too (the stash below skips it).  Opt-in (clock_monitor_couple_raim).
    if (clk_mon_couple)
        for (const auto& s : drop_sats) clock_refloat_sats.insert(s);
    // P0-2: a within-epoch whole-satellite drop contaminated that sat's emitted
    // correction — feed the retro-bump regardless of the (numeric) couple opt-in.
    if (retro_feed)
        for (const auto& s : drop_sats) retro_flag_sats.insert(s);

    // R0: park the tier's per-epoch observability for the epoch-status row.  The
    // refloat count is the tier's OWN settled whole-sat set (es.n_refloat_sats
    // reads the clock-COUPLE set, which is empty unless clock_monitor_couple_raim
    // — §5.07.4 defect 5); the clock move is 0.0 on an assessed-but-quiet epoch,
    // which is a real measurement, not an absence.
    ctx.carry.ep_we_refloat_sats = static_cast<int>(drop_sats.size());
    ctx.carry.ep_we_clk_move_max = std::abs(we_clk_move_max);
    ctx.carry.ep_we_clk_move_sat = we_clk_move_sat;

    sigma0_adapt = sigma0;   // A4: publish the final within-epoch σ̂0 for the withhold gate
}
// C3c §3: drop_sats/drop_arcs leave scope with the RAIM block — retire the aliases
// so the next epoch's primary buildCarryDdPriors never dereferences a dangling set.
carry_we_drop_sats = nullptr;
carry_we_drop_arcs = nullptr;

// Reduce-once invariant (design §3.5): the carried prior is injected
// pre-gate, so the single gate reduce() must be the ONLY per-station Schur
// recompute this epoch — UNLESS the within-epoch adaptation deliberately re-solved
// (each drop→re-solve is another reduce()).  A second recompute otherwise means a
// stray post-gate local-block change silently doubled the serial path.
if (pl_on && !we_adapt_attempted && est.reduceRecomputeCount() != 1) {
    log->error("Network[phase-leveling]: reduce() recomputed {} times this epoch "
               "(expected exactly 1) — a stray post-gate local-block change doubled "
               "the serial Schur path.", est.reduceRecomputeCount());
}

// --- Within-epoch escape-hatch: withhold rather than emit a still-contaminated epoch
// The capped adaptation (drop culprit → re-solve, kMaxAdapt passes) may not fully
// isolate the fault; if σ̂0 STILL exceeds the withhold ceiling the shared solution
// is too corrupted to publish.  Reuse the observability-gate disposition (no emit,
// no carry stash, epoch skipped — the contaminated solve never enters the store or
// the carry) instead of emitting it with only a log warning.  OFF (raim_withhold ≤ 0)
// is byte-identical: the epoch emits exactly as today.
//
// P0-4: gate on σ̂0 ALONE, NOT on we_adapt_attempted.  A DIFFUSE contamination
// that leaves NO single dominant culprit (kind == Kind::None) never launches a
// re-solve, so we_adapt_attempted stays false — yet σ̂0 is high, and that is
// exactly the case the withhold ceiling should catch.  sigma0_adapt is set to the
// assessed σ̂0 whenever the within-epoch RAIM block runs (whether or not a culprit
// was dropped) and stays 0 otherwise, so a clean/OFF epoch (sigma0_adapt == 0)
// still can never trip it.
// RTA-10 (network.per_const_withhold): per-CONSTELLATION carve-out of the σ̂0
// withhold.  When the ceiling trips, judge each constellation by its OWN σ̂0_c —
//
//   σ̂0_c = max(1, 1.4826 · median{ r_i : i ∈ constellation c's rows })
//
// over EXACTLY the rows the global σ̂0 pools (carried-ambiguity PHASE rows of
// every present non-pivot station, r_i = |v_i|·√w_i, divided by √(redundancy r̄_i)
// under B2 with its uncheckable-row exclusion), partitioned by the ambiguity's
// satellite constellation.  Same statistic, same ceiling, restricted support.
//
// APPROXIMATION (stated, not hidden): the residuals are evaluated at the JOINT
// solution — a contamination in constellation A perturbs shared per-station
// nuisances (receiver clock, ZWD, iono) that leak into B's residuals, and each
// row's B2 redundancy is its share of the JOINT system's redundancy, not of a
// per-constellation solve.  σ̂0_c therefore under-separates cross-constellation
// coupling; the "every constellation individually clean while the global σ̂0
// tripped" outcome below is the honest edge of that approximation and is
// published-with-warning + counted (n_perconst_diffuse_published), not silently
// absorbed.  A constellation also needs >= per_const_withhold_min_rows rows to
// be JUDGEABLE — fewer cannot certify cleanliness under a contaminated epoch.
//
// The RANK/OBSERVABILITY gate above keeps whole-epoch semantics untouched: a
// rank deficiency means the declared datum itself is invalid — the product
// would be a meaningless least-norm split, so no "healthy subset" exists to
// carve out.  This carve-out applies ONLY to the σ̂0 (contamination) withhold.
bool           perconst_active = false;   // a publishable subset exists this epoch
std::set<char> perconst_publish;          // constellations to keep in the emission
std::set<char> perconst_dec_withhold;     // constellations the decision withheld
std::map<char, NetworkEstimator::PerConstSigma0> perconst_stats;  // σ̂0_c + rows (events/log)
if (perconst_on && raim_withhold > 0.0 && sigma0_adapt > raim_withhold) {
    for (auto& [cst, pool] : we_sigma0_pool_c) {
        NetworkEstimator::PerConstSigma0 st;
        st.rows = static_cast<int>(pool.size());
        if (!pool.empty()) {   // same median × 1.4826, floor 1 as the global σ̂0
            std::nth_element(pool.begin(), pool.begin() + pool.size() / 2, pool.end());
            st.sigma0_c = std::max(raim_s0_floor, 1.4826 * pool[pool.size() / 2]);
        }
        perconst_stats[cst] = st;
    }
    const NetworkEstimator::PerConstWithholdDecision dec =
        NetworkEstimator::decidePerConstWithhold(perconst_stats, raim_withhold,
                                                 perconst_min_rows);
    if (!dec.publish.empty()) {
        perconst_active       = true;
        perconst_publish      = dec.publish;
        perconst_dec_withhold = dec.withhold;
        std::string detail;
        for (const auto& [cst, st] : perconst_stats)
            detail += fmt::format("{}{}:{:.2f}/{}{}", detail.empty() ? "" : " ", cst,
                                  st.sigma0_c, st.rows,
                                  dec.publish.count(cst) ? "" : "(withheld)");
        log->info("Network (RTA-10): TOW {:.1f} global σ̂0={:.2f} > ceiling {:.2f} — "
                  "per-constellation carve-out publishes {} of {} judged "
                  "constellation(s) [σ̂0_c/rows: {}].",
                  se.time.tow, sigma0_adapt, raim_withhold, dec.publish.size(),
                  perconst_stats.size(), detail);
    }
    // dec.publish empty ⇒ NO constellation is individually publishable — fall
    // through to the whole-epoch withhold below, exactly as without the toggle.
}
// INC-5 flight-recorder trigger (docs/SPEC_prefit_census_observability.md §7).
// Evaluated on EVERY epoch that reaches this decision, whichever way it goes —
// passing `false` is what clears the era and re-arms the latch, and the
// consultation count is the invariant-#4 proof that the site is wired.  The A4
// era is the CEILING BREACH, not the withhold action, so the per-constellation
// carve-out does not suppress it: the epoch is still contaminated.
const bool a4_over_ceiling = raim_withhold > 0.0 && sigma0_adapt > raim_withhold;
networkRhsTripEdge(ctx, "sigma0", a4_over_ceiling);
// --- A4 withhold-era bookkeeping (SPEC_withhold_era_release §3; both edges per
// invariant #7).  Gated on the policy knobs so the OFF path emits no events and
// keeps every counter at zero (network_events.csv is in the byte-cert set).  The
// era here is the withhold ACTION (a carve-out-published epoch EXITS it — the
// declared RTA-10 limitation); the recorder line above tracks the ceiling
// BREACH — a deliberately different edge, kept unconfusable by the n_wh_* names.
if (ctx.knobs.wh_escalate > 0 || ctx.knobs.wh_free_erase) {
    const bool wh_withholding = a4_over_ceiling && !perconst_active;
    if (wh_escalated) {
        ++ctx.counters.n_wh_escalated_epochs;
        if (wh_withholding) ++ctx.counters.n_wh_escalate_exhausted;
        else                ++ctx.counters.n_wh_escalate_landed;
    }
    if (wh_withholding) {
        if (ctx.carry.wh_era_len > 0 && !wh_era_live) {
            // An era interrupted by a bypass path (datum_failed / gate_withheld /
            // solve_failed / head-skip) never reached an exiting decide — close it
            // honestly before opening the new one (C7).
            ++ctx.counters.n_wh_era_exits;
            emitEvent("SIGMA0", "era_exit", "", "",
                      static_cast<double>(ctx.carry.wh_era_len), 0.0,
                      "interrupted", "era broken by a non-decide epoch");
            ctx.carry.wh_era_len = 0;
        }
        if (ctx.carry.wh_era_len == 0) {
            ++ctx.counters.n_wh_era_entries;
            emitEvent("SIGMA0", "era_enter", "", "", sigma0_adapt, raim_withhold,
                      "withhold_era", fmt::format("{} re-solve(s)", n_adapt));
        }
        ++ctx.carry.wh_era_len;
        ctx.carry.wh_last_loop_ep = static_cast<long long>(ctx.counters.n_loop_epochs);
        if (ctx.carry.wh_era_len > ctx.counters.n_wh_era_max_len)
            ctx.counters.n_wh_era_max_len = ctx.carry.wh_era_len;
    } else if (ctx.carry.wh_era_len > 0) {
        ++ctx.counters.n_wh_era_exits;
        if (a4_over_ceiling) ++ctx.counters.n_wh_perconst_exits;
        emitEvent("SIGMA0", "era_exit", "", "",
                  static_cast<double>(ctx.carry.wh_era_len), sigma0_adapt,
                  !wh_era_live         ? "interrupted"
                  : a4_over_ceiling    ? "perconst_published"
                  : wh_escalated       ? "escalate_landed"
                                       : "cleared",
                  fmt::format("{} withheld epoch(s)", ctx.carry.wh_era_len));
        ctx.carry.wh_era_len = 0;
        ctx.carry.wh_last_loop_ep = -1;
    }
}
if (a4_over_ceiling && !perconst_active) {
    log->warn("Network[RAIM within-epoch]: TOW {:.1f} σ̂0={:.2f} > withhold ceiling {:.2f} "
              "after {} re-solve(s) — WITHHOLDING epoch (too contaminated to publish).",
              se.time.tow, sigma0_adapt, raim_withhold, n_adapt);
    emitEvent("SIGMA0", "withhold", "", "", sigma0_adapt, raim_withhold,
              "skip_epoch", fmt::format("{} re-solve(s)", n_adapt));
    ++n_skipped;
    ++n_sigma0_withheld;   // R8: dedicated count of σ̂0-ceiling withholds (reported in the summary)
    if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                     << ',' << n_present << ',' << n_clock << ',' << n_pb
                     << ',' << plan.declared_dimension << ",1,0,0,0," << total_pins << '\n';
    { EpochStatusRow es; es.sigma0 = sigma0_adapt; es.n_adapt = n_adapt;
      es.n_present = n_present; es.n_clock_sats = n_clock;
      es.datum_changed = datum_changed ? 1 : 0;
      // R0: a withheld epoch is precisely where the tier's first-assess σ̂₀ and
      // the impulse meter matter most — carry them onto the disposition row.
      if (ctx.carry.ep_sigma0_first >= 0.0) {
          es.sigma0_first = ctx.carry.ep_sigma0_first;
          if (ctx.carry.ep_we_refloat_sats >= 0)
              es.n_we_refloat_sats = ctx.carry.ep_we_refloat_sats;
          if (ctx.carry.ep_we_clk_move_max >= 0.0) {
              es.we_clk_move_max_m   = ctx.carry.ep_we_clk_move_max;
              es.we_clk_move_max_sat = ctx.carry.ep_we_clk_move_sat;
          }
      }
      emitEpochStatus("sigma0_withheld", es); }
    // RTA-10: product-level heartbeat for the WITHHELD epoch (H line, no E line)
    // — a live tail can tell "σ̂0 withheld" from "producer died".  Before the
    // deferred product writer opens (the first emit) the heartbeat is BUFFERED
    // in setup.pending_h_pre_open and flushed at the open.  withheld="*" = whole epoch.
    if (emit_epoch_status) {
        NetworkProducerStatus hst;
        hst.time.week = se.time.week;
        hst.time.tow  = se.time.tow;
        hst.state     = NetworkProducerStatus::State::WithheldSigma0;
        // H-rider: UNIFORM statistic per wire — when the census feed is armed
        // EVERY H site carries it (a consumer thresholding field 5 needs one
        // population; the withhold REASON is already in the state token, and
        // the A4 ceiling itself still read sigma0_adapt above, unchanged).
        hst.sigma0    = ctx.knobs.h_sigma0_census ? ctx.carry.ep_h_sigma0_census
                                                  : sigma0_adapt;
        hst.withheld  = "*";
        if (product_writer) {
            product_writer->writeEpochStatus(hst);
            if (nc.product_flush_per_epoch) product_writer->flush();
        } else if (setup.contract_pending) {
            // Buffer ONLY while a v2 writer open is still coming — with no
            // product path (or v1 immediate open) there is nothing to flush into.
            setup.pending_h_pre_open.push_back(hst);
        }
    }
    // INC-5: this epoch is about to `continue`, so it never reaches the
    // SB_FRAMEDIAG consumer — the recorder has to fire HERE or the A4 trigger
    // would be permanently inert (it would only ever dump the NEXT epoch, which
    // is a different system).  The kernel and the captures are still this
    // epoch's, so the dump is exact; the same call also retains this epoch as
    // the next trip's "before" frame.
    networkRhsAttribTrip(ctx, config_, log, traces, se, setup.stations, est,
                         plan.shared_datum, asm_res, est_index_of);
    // SPEC_withhold_era_release §3(b): make the free-epoch response durable to
    // the discarded epoch.  Detection ran pre-solve on raw observables (a
    // withhold-independent verdict); erasing the freed entries makes
    // carry-absence the durable free re-level at the next ACCEPTED epoch
    // (Assembly:2781 — no entry, no inject).  Reset-class only: no value from
    // this contaminated solve is stored.  Bridged bands were already removed
    // from the latch, so a delta-bridged carry can never be erased here; the
    // pending-reseed mark was consumed in PASS-B — the ERASE is the
    // load-bearing half.  Touches nothing the epoch-status/H/recorder writes
    // above read (G5-safe placement).
    if (ctx.knobs.wh_free_erase) {
        for (const auto& fa : ctx.carry.relevel.free_ambs) {
            const auto parts = StateKeys::parseAmbiguityKey(fa.second);
            if (!parts) continue;
            if (ctx.carry.local_carry.erase(
                    std::make_tuple(fa.first, parts->satellite, parts->signal)))
                ++ctx.counters.n_wh_free_erases;
        }
    }
    prev_week = se.time.week; prev_tow = se.time.tow;
    result.action = EpochAction::SkipEpoch;
    return result;
}


    // --- SPEC_day129_frame_guards Phase 2.2: SH-field health GATE verdict -----
    // Post-solve, pre-emit (this is the only pre-emit seam holding x_s), on
    // epochs that survived the rank gate and the A4 σ̂0 withhold above.  The
    // verdict is EMISSION-ONLY: NetworkSolPrep + NetworkEmitStore early-return
    // on ctx.carry.sh_gate_engaged, while the carry stash and every QC tier
    // still run — an hours-long out-of-band era must not expire the carry the
    // way the A4 skip-epoch semantics would.  Out-of-band EPOCHS are counted
    // (not warns — the throttled T1a WARN under-read the reference run 23.8×)
    // and every engage/release transition is logged + evented un-throttled.
    if (nc.sh_health_gate && sh_model) {
        const GlobalVtecShConfig& shc_g = config_.getGlobalVtecShConfig();
        const auto& coeffs_g = sh_model->coeffs();
        double a00_total = std::numeric_limits<double>::quiet_NaN();
        double rms_all = 0.0;
        int rms_n = 0;
        bool coeff_ok = true;
        for (std::size_t k = 0; k < coeffs_g.size() && coeff_ok; ++k) {
            try {
                const double val =
                    x_s(est.vtecCoeffColumn(coeffs_g[k].n, coeffs_g[k].m, coeffs_g[k].trig));
                rms_all += val * val;
                ++rms_n;
                if (coeffs_g[k].n == 0 && coeffs_g[k].m == 0 && coeffs_g[k].trig == 'C')
                    a00_total = val;
            } catch (...) { coeff_ok = false; }
        }
        // Dual-layer: the physical band bounds the TOTAL mean VTEC (layer sum),
        // exactly as the T1a WARN does.
        if (coeff_ok && sh_model2 && std::isfinite(a00_total)) {
            try { a00_total += x_s(est.vtecCoeffColumn(0, 0, 'C', 1)); }
            catch (...) { coeff_ok = false; }
        }
        if (coeff_ok && std::isfinite(a00_total)) {
            rms_all = (rms_n > 0) ? std::sqrt(rms_all / rms_n) : 0.0;
            ctx.carry.sh_gate_last_a00 = a00_total;
            ctx.carry.sh_gate_last_rms = rms_all;
            const bool band_active = shc_g.a00_warn_low_tecu < shc_g.a00_warn_high_tecu;
            const bool band_oob = band_active
                && (a00_total < shc_g.a00_warn_low_tecu
                    || a00_total > shc_g.a00_warn_high_tecu);
            const bool rms_oob = nc.sh_gate_rms_max_tecu > 0.0
                && rms_all > nc.sh_gate_rms_max_tecu;
            const bool oob = band_oob || rms_oob;
            if (oob) {
                ++ctx.carry.sh_gate_oob_epochs_total;
                ++ctx.carry.sh_gate_oob_run;
            } else {
                ctx.carry.sh_gate_oob_run = 0;
            }
            const bool engage = ctx.carry.sh_gate_oob_run >= nc.sh_gate_min_epochs;
            // INC-5 flight-recorder trigger: the ENGAGE era.  Called on every
            // evaluated epoch (false clears the era), and it must precede the
            // engage block below so the SB_FRAMEDIAG consumer — which runs later
            // this same epoch — sees the pending trip.
            networkRhsTripEdge(ctx, "sh_gate", engage);
            if (engage && !ctx.carry.sh_gate_engaged) {
                ++ctx.carry.sh_gate_engagements;
                log->warn("Network (SH gate): ENGAGED at TOW {:.1f} — a00 {:.1f} TECU "
                          "(band [{:.1f}, {:.1f}]), rms_all {:.2f} TECU{} — out of band "
                          "{} consecutive epoch(s); WITHHOLDING emission until the field "
                          "returns (solve/carry/QC continue).",
                          se.time.tow, a00_total, shc_g.a00_warn_low_tecu,
                          shc_g.a00_warn_high_tecu, rms_all,
                          rms_oob ? fmt::format(" > ceiling {:.1f}", nc.sh_gate_rms_max_tecu)
                                  : std::string{},
                          ctx.carry.sh_gate_oob_run);
                emitEvent("SH_GATE", "engage", "", "", a00_total,
                          band_oob ? (a00_total < shc_g.a00_warn_low_tecu
                                          ? shc_g.a00_warn_low_tecu
                                          : shc_g.a00_warn_high_tecu)
                                   : nc.sh_gate_rms_max_tecu,
                          "withhold_emission",
                          fmt::format("rms_all={:.2f} oob_run={} rms_oob={}", rms_all,
                                      ctx.carry.sh_gate_oob_run, rms_oob ? 1 : 0));
            } else if (!engage && ctx.carry.sh_gate_engaged) {
                log->info("Network (SH gate): RELEASED at TOW {:.1f} — a00 {:.1f} TECU "
                          "back in band (rms_all {:.2f}); emission resumes.  Run totals: "
                          "{} out-of-band epoch(s), {} engagement(s), {} emission(s) "
                          "withheld.",
                          se.time.tow, a00_total, rms_all,
                          ctx.carry.sh_gate_oob_epochs_total, ctx.carry.sh_gate_engagements,
                          ctx.counters.n_sh_gate_withheld);
                emitEvent("SH_GATE", "release", "", "", a00_total, std::nullopt,
                          "resume_emission",
                          fmt::format("rms_all={:.2f} withheld_total={}", rms_all,
                                      ctx.counters.n_sh_gate_withheld));
            } else if (engage && (ctx.carry.sh_gate_oob_run % 20) == 0) {
                // Un-throttled visibility during a LONG engaged era: a periodic
                // count-bearing line every 20 epochs (10 min at 30 s) — the era's
                // full extent is in the per-epoch counters/events regardless.
                log->warn("Network (SH gate): still engaged at TOW {:.1f} — {} consecutive "
                          "out-of-band epoch(s), a00 {:.1f} TECU, rms_all {:.2f}; {} "
                          "emission(s) withheld so far.",
                          se.time.tow, ctx.carry.sh_gate_oob_run, a00_total, rms_all,
                          ctx.counters.n_sh_gate_withheld);
            }
            ctx.carry.sh_gate_engaged = engage;
        }
        // coeff_ok false ⇒ a column lookup failed (field not in this epoch's
        // solve) — leave the gate state unchanged rather than fabricating a
        // release; the next computable epoch re-evaluates.
    }

    // --- Result epilogue: hand the 18 crossing locals to the caller. ---
    result.x_s = std::move(x_s);
    result.plan = std::move(plan);
    result.changed_consts = std::move(changed_consts);
    result.datum_changed = datum_changed;
    result.clk_model_warm = clk_model_warm;
    result.emit_rate_warm = emit_rate_warm;
    result.clock_refloat_sats = std::move(clock_refloat_sats);
    result.retro_flag_sats = std::move(retro_flag_sats);
    result.retro_feed = retro_feed;
    result.n_arc_refloat_epoch = n_arc_refloat_epoch;
    result.n_adapt = n_adapt;
    result.sigma0_adapt = sigma0_adapt;
    result.perconst_active = perconst_active;
    result.perconst_publish = std::move(perconst_publish);
    result.perconst_dec_withhold = std::move(perconst_dec_withhold);
    result.perconst_stats = std::move(perconst_stats);
    result.we_t2_opp = we_t2_opp;
    result.we_t2_flag = we_t2_flag;
    return result;
}

} // namespace GnssProcessor

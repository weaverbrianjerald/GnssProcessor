/**
 * @file NetworkSolveGlue.cpp
 * @brief Implementation of @ref networkSolveGlue: the network producer's per-epoch datum
 *        rows, solve and within-epoch RAIM (block SB_SOLVEGLUE, moved out of
 *        Application::runNetwork at decomposition INC-9b).  Where the driver loop used
 *        `continue;`, the body sets EpochAction::SkipEpoch and returns (four sites); the
 *        result contract is in the header banner.  The alias preamble reconstructs the
 *        run-TU nc / ctx / traces / ws / setup aliases and the emitEpochStatus /
 *        emitEvent lambdas so the body reads the driver's names.
 */

#include "filter/NetworkSolveGlue.hpp"

#include "app/NetworkRunContext.hpp"
#include "atmosphere/SphericalHarmonicIonosphere.hpp"   // SH gate (Phase 2.2): coeffs()
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "filter/NetworkChainDatum.hpp"
#include "filter/NetworkClockAnchor.hpp"   // guarded temporal clock anchor (card Q3)
#include "filter/NetworkDatumGraph.hpp"   // datum graph census: the per-epoch spectral hand-off
#include "filter/NetworkFrameDiag.hpp"   // networkDatumInfoRatio (invariant #7)
#include "filter/NetworkHandoverStransform.hpp"   // the clock-pivot handover S-transform (queue 1(b))
#include "filter/NetworkPhaseDatum.hpp"
#include "filter/NetworkPivotHandover.hpp"
#include "filter/NetworkPrefitCensus.hpp"   // networkPrefitSigma0, the census-fed H σ̂₀ (H-rider)
#include "filter/NetworkRelevel.hpp"
#include "filter/NetworkRhsAttrib.hpp"   // flight-recorder trip latch and inline A4 dump (INC-5)
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
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond; // conditional local carry on the coupling rows; requires fullcov_on, enforced at load (card 1)
    const bool   carry_cpl_joint = ctx.knobs.carry_cpl_joint; // whiten the coupling rows jointly (departed columns into M_w); requires carry_cpl_cond, enforced at load (card 1b)
    const bool   ura_retro     = ctx.knobs.ura_retro;             // retro-bump the flagged satellites on the next emit (B1)
    const double ura_refloat_inflate = ctx.knobs.ura_refloat_inflate; // URA multiplier after a refloat (B1)
    const double raim_k        = ctx.knobs.raim_k;      // standardized post-fit residual flag threshold
    const double raim_s0_floor = ctx.knobs.raim_s0_floor; // floor on the robust a-posteriori σ̂0 (1.0 default; lower lets σ̂0 < 1)
    const int    raim_min_sta  = ctx.knobs.raim_min_sta; // cross-station consensus count
    const double raim_withhold = ctx.knobs.raim_withhold; // > 0 withholds (does not emit) an epoch whose post-adaptation σ̂0 exceeds this ceiling; 0 = off (A4)
    const bool   raim_redun    = ctx.knobs.raim_redun; // Baarda per-row r_i normalization of the RAIM residual (B2)
    const bool   clk_mon_couple = ctx.knobs.clk_mon_couple;  // also refloat RAIM-flagged sats' clocks (opt-in)
    const bool   pivot_vr_signal = ctx.knobs.pivot_vr_signal;  // also signal a vanish-return pivot change (A1)
    const bool   emit_clock_disc = ctx.knobs.emit_clock_disc;            // clock disc channel and per-constellation bump scoping (A2)
    const bool   sticky_pivot    = ctx.knobs.sticky_pivot;              // keep the incumbent clock pivot while it stays registered (RTA-1)
    const bool   clock_sum_to_zero = ctx.knobs.clock_sum_to_zero;    // anchor-free per-constellation clock gauge (Σδtˢ = 0) in place of a pivot pin; no pivot or handover machinery
    const bool   clock_brdc_anchored = ctx.knobs.clock_brdc_anchored; // brdc_rts: the per-satellite broadcast anchors are the clock gauge (pre-gate); no pivot machinery and no sum-to-zero membership wander
    const bool   clock_signal_basis_hold = ctx.knobs.clock_signal_basis_hold; // hold each satellite's IF-pin reference pair fixed across epochs (a time-consistent δtˢ↔dˢ split) instead of re-selecting it from each epoch's coverage; inert without estimate_sat_code_bias (W6)
    const bool   cb_time_linked  = ctx.knobs.cb_time_linked;      // impose the code-bias datum (IF pin and per-(constellation, signal) s2z) as a time datum: pin the change from the previous epoch's solved values rather than the level to zero; mutually exclusive with clock_signal_basis_hold, validated at load (card 2, W6)
    const bool   scoped_handover = ctx.knobs.scoped_handover;   // per-constellation scoping of the internal handover response (RTA-2)
    const bool   perconst_on       = ctx.knobs.perconst_on;          // per-constellation carve-out of the A4 σ̂0 withhold (RTA-10)
    const int    perconst_min_rows = ctx.knobs.perconst_min_rows;   // minimum σ̂0-pool rows for a constellation to be judged (RTA-10)
    const bool   emit_epoch_status = ctx.knobs.emit_epoch_status;     // per-epoch H heartbeat in the product file, withheld epochs included (RTA-10)
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
    // The census-fed H σ̂₀ is computed once per epoch, before the solve, so every H
    // site downstream (including the pre-solve observability-gate withhold further
    // down) reads the same value.  Its inputs, the asm_res row stacks, are fixed at
    // assembly, and the value is measure-only.  0.0 is the H grammar's "unavailable"
    // (empty pool); the reset keeps a knob-off run at an inert 0 rather than a stale
    // value (H-rider, edge-queue §1.6 item 3a).
    ctx.carry.ep_h_sigma0_census = 0.0;
    if (ctx.knobs.h_sigma0_census) {
        ctx.carry.ep_h_sigma0_census = networkPrefitSigma0(
            ctx, stations, est, asm_res, est_index_of, ctx.knobs.h_sigma0_class_mask);
        if (ctx.carry.ep_h_sigma0_census > 0.0) ++ctx.carry.h_sig0_census_nonzero;
    }
    auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
        // The measurement-admission census columns are stamped here rather than at the
        // four withhold sites below: the census is measure-only, and a withheld epoch is
        // the one for which the absolute |z| population is most wanted.  The screen runs
        // before assembly, so ctx.carry already holds this epoch's numbers; the TOW
        // compare leaves the columns blank on an epoch the screen did not see
        // (docs/SPEC_measurement_admission.md INC-3).
        // The PD-safe DD fold columns are stamped here for the same reason: a degenerate
        // carry block is a plausible cause of a withheld epoch
        // (docs/SPEC_pd_safe_dd_fold.md INC-0).
        //
        // The fold statistics are read from the kernel, not from a ctx.carry stamp.  The
        // stamp is written by the install seam, which runs after this one and only on an
        // accepted epoch, so a stamp-based test here would leave these columns blank on
        // every withheld row.  `ddFoldStats()` is per-epoch (NetworkEstimator::beginEpoch
        // resets it), so `blocks == 0` means no fold has run this epoch and the columns
        // stay blank.  A within-epoch RAIM re-solve folds again and overwrites, so the
        // value read is always from the latest fold.
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


// The W6 satellite code-bias datum rows are built by
// NetworkSbasis::buildCodeBiasDatumRows (INC-8), whose Doxygen holds the design
// (IF pin and per-(constellation, signal) sum-to-zero, card-2 time-linking, basis
// hold / latch / re-latch).  The fold, the RAIM-replay cap_* capture and the first-N
// summary stay here (writer-site contract, NetworkRunTypes.hpp).
// Clear the hold-off IF-pin selection scratch for this epoch (the counterpart of the
// per-epoch this_epoch_anchor.clear()).  The clear is unconditional, done even when
// the code-bias block below is skipped (no registered keys), so a prior epoch's
// selection cannot reach the emit-site comparison.  In the default config
// (emit_clock_disc off) the map is empty and the clear is a no-op.
this_epoch_if_pin_sel.clear();
// The S-contract §1.5 declaration scratch gets the same unconditional per-epoch clear,
// so a prior epoch's pair cannot reach the emit-site fill.
this_epoch_if_pin_real.clear();
this_epoch_gf_pin_real.clear();
ctx.carry.this_epoch_family_gf_pin_real.clear();
this_epoch_cb_ifpin.clear();   // this epoch's IF-pin pairs, persisted at harvest (card 2)
this_epoch_cb_reseed.clear();  // this epoch's re-seeds, merged into the pending disc set at the emit site, which withheld epochs never reach (card 2)
// This epoch's held extra-band keys get the same unconditional per-epoch clear: a stale
// hold would withhold a bias that the current epoch determined (B5D).
ctx.carry.this_epoch_extra_band_held.clear();
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

    // --- Family-GF: run-latched receiver-family census (docs/SPEC_family_gf_datum.md §3.2) ---
    // Runs whenever the GF datum is armed, with network.code_bias_gf_datum_per_family on
    // or off, so that an armed but inert configuration shows in the log (standing
    // invariant #4).  With the per-family knob off, the census lines of a partitioned
    // constellation also name the null-space gate withholds to expect, which otherwise
    // appear in the log with no pointer to the receiver-family split.
    //
    // This is a separate scan, not a rider on the CB-ADM admission census.  That census
    // reduces its per-(station, pair) map to an integer support count and discards
    // station identity, so it cannot tell whether any station bridges the two families,
    // and it runs only under network.cb_admission; riding on it would mean arming a
    // different registration path to get a diagnostic.
    //
    // Latched on the first epoch with a usable station set: the partition is a property
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
    // Empty unless the per-family knob is on and the census found a partition, so in
    // every other posture the row builder folds nothing extra and the run is
    // byte-identical.
    static const std::set<std::string> kNoFamilies;
    const std::set<std::string>& gf_family_constellations =
        (config_.networkCodeBiasGfDatum() && config_.networkCodeBiasGfDatumPerFamily())
            ? family_census.partitioned
            : kNoFamilies;

    // --- ISC bridging roster (RF-0f) --------------------------------------------
    // Which (satellite, companion signal) inter-signal corrections the data can reach
    // this epoch.  The test is per satellite and per same-band signal pair, not per
    // channel: an ISC is a pure bias difference, so it is determined if and only if some
    // present station carries both signals of that satellite in its own code-bias state
    // map, keyed "sat|signal" (StateKeys::parseSatBiasKey); the shared columns are
    // registered from the same map (networkAssemblyRegisterColumns_ in
    // ApplicationNetworkAssembly.cpp).  A channel that is dense network-wide, or two
    // signals present at different stations, does not determine it, because each
    // station's own receiver code bias absorbs its half.
    //
    // Built here rather than in the row builder because only the driver sees the
    // stations.  Empty unless network.sat_isc_state is armed, so an unarmed run never
    // reaches the builder's lookups.
    std::set<std::string> isc_bridged;
    if (config_.getNetworkConfig().sat_isc_state) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i] || !stations[i].sm) continue;
            // sat -> the signals this station holds for it
            std::map<std::string, std::set<std::string>> by_sat;
            for (const auto& kv : stations[i].sm->getSatBiasStates()) {
                const auto parts = StateKeys::parseSatBiasKey(kv.first);
                if (parts) by_sat[parts->satellite].insert(parts->signal);
            }
            for (const auto& [sat, sigs] : by_sat)
                for (const auto& a : sigs)
                    for (const auto& b : sigs)
                        if (a != b && a.size() >= 2 && b.size() >= 2 && a[1] == b[1])
                            isc_bridged.insert(sat + "|" + a);   // a is bridged to its band-mate
        }
    }

    // --- Extra-band determination roster (B5D) -----------------------------------
    // Which (satellite, extra signal) code-bias columns the data determine this epoch,
    // for a given pin pair.  The test is per satellite and per station, not per channel:
    // with the pinned pair fixing that station's slant ionosphere and the satellite
    // clock, one station carrying code on the pair's first band, on its second band and
    // on the extra signal determines d_k up to the family constant that its own receiver
    // extra-band code bias absorbs.  A channel that is dense network-wide does not: three
    // stations each holding one of the three codes leave the ionosphere free at all three.
    //
    // Keys carry the pin pair's band digits as well as the satellite and signal
    // ("sat|signal|b1b2"), because the builder's realized pair is not always (L1, L2)
    // (clock_signal_basis_hold can hold or re-latch it), and a roster that answered for
    // the wrong pair would free a column that nothing determines.  Built here rather than
    // in the row builder because only the driver sees the stations.  Empty unless
    // network.sat_gf_extra_band_free is armed, so an unarmed run never reaches the
    // builder's lookups.
    std::set<std::string> extra_band_determined;
    if (config_.getNetworkConfig().sat_gf_extra_band_free) {
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!present[i] || !stations[i].sm) continue;
            // sat -> the code signals this station holds for it (the sat-bias state map
            // is code-only: NetworkBiasAdmission admits raw 'C' observables alone).
            std::map<std::string, std::set<std::string>> by_sat;
            for (const auto& kv : stations[i].sm->getSatBiasStates()) {
                const auto parts = StateKeys::parseSatBiasKey(kv.first);
                if (parts) by_sat[parts->satellite].insert(parts->signal);
            }
            for (const auto& [sat, sigs] : by_sat) {
                std::set<char> bands;
                for (const auto& s : sigs)
                    if (s.size() >= 2) bands.insert(s[1]);
                if (bands.size() < 3) continue;   // a pair plus an extra band, or nothing
                for (const auto& sk : sigs) {
                    if (sk.size() < 2) continue;
                    for (const char b1 : bands) {
                        if (b1 == sk[1]) continue;
                        for (const char b2 : bands) {
                            if (b2 == b1 || b2 == sk[1]) continue;
                            extra_band_determined.insert(sat + "|" + sk + "|"
                                                         + std::string{b1, b2});
                        }
                    }
                }
            }
        }
    }

    const auto cbd = NetworkSbasis::buildCodeBiasDatumRows(
        est, registered_code_keys, freqOf,
        cb_time_linked, clock_signal_basis_hold, emit_clock_disc,
        config_.networkCodeBiasGfDatum(),
        // The satellite GF datum mode and the run-latched broadcast targets.  The
        // targets are empty in every mode except broadcast_tgd, and an empty target
        // makes the builder fall back to zero rather than guess (RF-0c).
        config_.getNetworkConfig().sat_gf_datum, setup.sat_gf_target_m,
        // The two arms of the level-datum A/B; the loader refuses both at once (RF-0e).
        config_.getNetworkConfig().sat_if_level_datum,
        config_.getNetworkConfig().sat_gf_datum_time_linked,
        config_.getNetworkConfig().sat_isc_state, isc_bridged,
        // The extra-band columns the data determine, and the knob that frees them (B5D).
        config_.getNetworkConfig().sat_gf_extra_band_free, extra_band_determined,
        gf_family_constellations,
        code_bias_prev_, code_bias_ifpin_pair_,
        code_bias_sat_seen_, code_bias_datum_resumed_,
        held_clock_ref, this_epoch_if_pin_sel,
        this_epoch_if_pin_real, this_epoch_gf_pin_real,
        ctx.carry.this_epoch_family_gf_pin_real,
        this_epoch_cb_ifpin, this_epoch_cb_reseed,
        basis_relatch_disc_pending, emitEvent);
    // Record the IF pin pair election in the signal roster.  This is the only site that
    // knows which signals the S-basis pinned this epoch (the realized pair, after the
    // basis hold and any re-latch).  It fills BandRoster::datum_code / datum_phase and a
    // census; the solve does not read them (the wire's signal declaration does, in
    // networkLedgerEmitBuildDeclaration).  No-op when the roster is disarmed (card SID
    // phase 2).
    networkSignalRosterDatumPin(ctx, this_epoch_if_pin_real);
    if (cbd.H.rows() > 0) {
        est.attribTag("w6_cb_gauge");
        est.addSharedConstraintRows(cbd.H, cbd.z, NetworkSbasis::kCodeBiasGaugeWeight);
        // On-path telemetry (RF-0b).  In the no-constraint posture these rows are the
        // whole satellite code-bias datum, not a gauge beside an ionosphere constraint,
        // so the run counts the epochs that folded them and the channel levels they
        // pinned.  A count of 0 folded identifies inert wiring, which the log alone
        // cannot tell apart from a working run (standing invariant #4).
        if (config_.getNetworkConfig().w6_iono_constraint
            == NetworkW6IonoConstraint::None) {
            ++ctx.counters.n_w6_iono_none_epochs;
            ctx.counters.n_w6_iono_none_s2z_rows +=
                static_cast<std::size_t>(cbd.n_s2z_channels);
        }
        // On-path telemetry (RF-0c).  Under the no-constraint posture this row is the
        // datum for the between-satellite geometry-free split, so the run counts the
        // satellites that received one and, under broadcast_tgd, how many got a real
        // group delay and how many a fallback zero (standing invariant #4).
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
            ctx.counters.n_isc_observed += static_cast<std::size_t>(cbd.n_isc_observed);
            ctx.counters.n_isc_pinned   += static_cast<std::size_t>(cbd.n_isc_pinned);
            // The same counters per constellation letter (card GAL, R3 iii).
            for (const auto& [cst, g] : cbd.sat_gf_by_const) {
                auto& a = ctx.counters.sat_gf_by_const[cst];
                a.rows += g.rows;              a.tgd_hits += g.tgd_hits;
                a.tgd_zero += g.tgd_zero;      a.tgd_missing += g.tgd_missing;
                a.pair_no_gd += g.pair_no_gd;  a.extra_rows += g.extra_rows;
                a.isc_zero += g.isc_zero;      a.sameband_free += g.sameband_free;
                a.carried += g.carried;        a.anchored += g.anchored;
            }
            // The three extra-band dispositions, and the sum-to-zero level re-references
            // of the freed channels, which are invariant #7's decoupling edge (B5D).
            ctx.counters.n_extra_band_free    += static_cast<std::size_t>(cbd.n_extra_band_free);
            ctx.counters.n_extra_band_carried += static_cast<std::size_t>(cbd.n_extra_band_carried);
            ctx.counters.n_extra_band_held    += static_cast<std::size_t>(cbd.n_extra_band_held);
            ctx.counters.n_s2z_reseed_rows    += static_cast<std::size_t>(cbd.n_s2z_reseed_rows);
            ctx.counters.n_s2z_reseed_sats    += static_cast<std::size_t>(cbd.n_s2z_reseed_sats);
            // The held keys are taken from the builder rather than re-derived here.  The
            // RF-0f verdicts below are re-derived because they are a two-way test on one
            // roster; a hold is a three-way branch over the roster and the carry map, and
            // a second derivation of it could drift from the first.  Empty unless armed
            // (B5D).
            ctx.carry.this_epoch_extra_band_held = cbd.extra_band_held;
            // Re-derive the per-key verdicts for the emit side from the same two inputs the
            // row builder used: this epoch's realized IF pin pair (so a held or
            // re-selected pair is respected) and the bridging roster.  They are derived
            // here rather than returned from the builder because the emit prep needs them
            // keyed, and a second return channel for the same fact could drift from the
            // first (RF-0f).
            ctx.carry.this_epoch_isc_obs.clear();
            ctx.carry.this_epoch_isc_pinned.clear();
            // The verdicts exist only under network.sat_isc_state, as RF-0f and the
            // NetworkSolPrep erase site document ("both sets are empty unless the knob is
            // armed").  With the knob off, isc_bridged is empty by construction, so this
            // loop would class every companion on the pin pair's second band as pinned
            // and the emit prep would erase it from the wire, including a Galileo C5X
            // column beside the (C1C, C5Q) pair, which the RF-0d rules pin at the pair's
            // own target and which the combined-channel consumer needs.  With the knob
            // off the RF-0d rule stands: the column is pinned, the assertion is counted
            // in n_sat_gf_isc_zero, and the bias is emitted.  On the GPS dual posture no
            // band-2 companion is registered (C2W is every station's latch), so GPS-only
            // output does not depend on this guard (card GAL).
            if (config_.getNetworkConfig().sat_isc_state) {
                for (const auto& [sat, sig] : registered_code_keys) {
                    const auto pit = this_epoch_if_pin_real.find(sat);
                    if (pit == this_epoch_if_pin_real.end()) continue;
                    const std::string& sig1 = pit->second.first;
                    const std::string& sig2 = pit->second.second;
                    if (sig == sig1 || sig == sig2) continue;
                    if (sig.size() < 2 || sig2.size() < 2 || sig[1] != sig2[1]) continue;
                    (isc_bridged.count(sat + "|" + sig) ? ctx.carry.this_epoch_isc_obs
                                                        : ctx.carry.this_epoch_isc_pinned)
                        .insert(sat + "|" + sig);
                }
            }
        }
        if (within_epoch_armed) {
            cap_H = cbd.H; cap_z = cbd.z;
            cap_constraint_weight = NetworkSbasis::kCodeBiasGaugeWeight;
            cap_has_constraint = true;
        }
    }
    // --- Loose physical-bound prior on the admitted OSB columns (card KP) ---
    // Folded after the tight gauge rows and before the rank gate, through the
    // diagonal prior channel rather than the gauge stack, so
    // plan.declared_dimension is unchanged by construction (K3).  It targets the
    // code-dominated residual direction that the phase drop-and-heal cannot
    // reach: it anchors PB columns only, so a near-null direction with zero phase
    // weight is outside its reach.  With the knob off the list is empty and
    // addSharedPrior returns immediately (byte-identical).
    if (config_.networkCbKaulaPrior()) {
        const auto kp = NetworkSbasis::buildCodeBiasKaulaPriors(
            registered_code_keys, config_.networkCbKaulaSigmaM());
        if (!kp.empty()) {
            est.attribTag("cb_kaula");
            est.addSharedPrior(kp);
            // The RAIM within-epoch replay re-solves from a fresh beginEpoch();
            // a prior missing there would give the replayed solve a different
            // datum from the first one, with nothing in the log.
            if (within_epoch_armed)
                cap_priors.insert(cap_priors.end(), kp.begin(), kp.end());
            ++ctx.counters.n_kp_epochs;
            ctx.counters.n_kp_cols += kp.size();
            // The K2 anti-fragility margin is measured after the gate (in the
            // one-shot block there), not here: it needs lambda_max of the same
            // reduced system the gate judges, and calling reducedSharedNormals()
            // here would force the Schur reduction before the clock datum rows
            // are folded.  addSharedInfo would carry the shared-border delta
            // through the cached reduction in place, but the clock rows' w·HᵀH
            // increment is not bit-symmetric, so reducing early would make the
            // first epoch's last bits depend on whether KP is armed.
        }
    }
    if (spectrum_diag_budget > 0)   // first-N (spectrum block decrements below)
        log->warn("Network[W6-cb]{}: nshared={}, n_cb={}, folded {} code-bias gauge "
                  "rows (IF pin for {} sats + sum-to-zero for {} (const,signal) levels"
                  "{}{}{})",
                  // In the no-constraint posture these rows are the whole satellite
                  // code-bias datum, not a gauge beside an ionosphere constraint.
                  // Naming the posture on the line that reports the row count lets a
                  // first-N read of any log tell which datum produced the product
                  // (RF-0b).
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
                  // Family-GF on-path telemetry for the extra rows.  Printed only when
                  // rows folded: "0 folded" is what an inert knob looks like, and the
                  // census line above already reports the verdict that explains a zero
                  // (invariant #4: check the counter, not the absence of a crash).
                  cbd.n_family_gf_rows > 0
                      ? fmt::format(" + FAMILY-GF pin for {} (sat,secondary-family) pairs, "
                                    "of which {} have a two-band family whose entire "
                                    "estimable content is the inter-family ISC",
                                    cbd.n_family_gf_rows, cbd.n_family_gf_only_pair)
                      : std::string(),
                  // Name the datum on the line that reports the row count, so a first-N
                  // read of any log tells which S-basis produced the product: the zero
                  // and TGD frames differ by c*T_GD on every code bias, and by the same
                  // amount with the opposite sign in the emitted ionosphere (RF-0c).
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
decl.emit_code_bias = config_.estimateNetworkSatCodeBias();  // self-describe the OSB datum (W6)
// Self-describe the anchor-free ensemble phase-bias frame, so a consumer knows the
// emitted bias references the receiver ensemble (Σ_r z = 0), not a single pivot
// receiver's ambiguity (fix A).
if (phase_datum_ensemble)
    decl.amb_datum = SbasisDeclaration::AmbDatum::SumToZeroPerReceiver;
// Self-describe the Eq-34 single-receiver-chain phase-bias frame, so a consumer knows
// the emitted between-satellite reference is integer-consistent (each chain hop is an
// integer).  The wire value is 2, on the existing v2 S line (card 3).
else if (phase_datum_eq34)
    decl.amb_datum = SbasisDeclaration::AmbDatum::SingleReceiverChain;
if (clock_brdc_anchored) {
    // Broadcast-anchored clock gauge: buildSharedDatum branches on this to emit
    // one anchor row per satellite (Δδtˢ → 0, toward its own broadcast a-priori)
    // at weight 1/σ².  The anchors are the datum (no pivot, empty datum, declared
    // dimension 0), and unlike sum-to-zero the frame does not redistribute when
    // membership changes.
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
    // In sticky mode the incumbent baseline is last_pivot, not prev_pivot.
    // prev_pivot is replaced wholesale at each emit, so a constellation absent
    // from the last emitted epoch drops out of it, and its return would
    // re-select a pivot spuriously (and, with the vanish-return arm off,
    // re-reference the datum without a signal).  last_pivot is merge-updated at
    // the same emit site, so per constellation it holds the pivot of the last
    // emitted product that contained it: the datum a consumer of that
    // constellation last adopted.  Both maps advance only at the emit site
    // (P0-1), so stickiness follows the datum of the last emitted product.  A
    // handover epoch that is then withheld leaves the incumbent unchanged, and if
    // the incumbent returns before any new-datum epoch emits, the datum does not
    // move (RTA-1).
    // Card HLT (network.sat_health): the election passes over a registered satellite the
    // broadcast flags unhealthy at this epoch.  In "exclude" mode the setup door removed
    // such a satellite's observations, so none is registered and the set stays empty.
    std::set<std::string> hlt_ineligible;
    if (ctx.knobs.sat_health != 0) {
        const double t_s = TimeUtils::continuousGpsSeconds(se.time);
        for (const std::string& s : registered_clock_sats)
            if (setup.sat_health.unhealthyAt(s, t_s)) hlt_ineligible.insert(s);
        ctx.counters.hlt_unhealthy_registered += static_cast<long long>(hlt_ineligible.size());
    }
    plan = sb.buildSharedDatum(est, registered_clock_sats, decl,
                               sticky_pivot ? &last_pivot : nullptr,
                               ctx.knobs.sat_health != 0 ? &hlt_ineligible : nullptr);
    ctx.counters.hlt_pivot_skips     += plan.pivot_health_skips;
    ctx.counters.hlt_pivot_fallbacks += plan.pivot_health_fallbacks;
} catch (const std::exception& ex) {
    log->warn("Network: buildSharedDatum failed ({}); skipping epoch", ex.what());
    ++n_skipped;
    { EpochStatusRow es; es.n_present = n_present; es.n_clock_sats = n_clock;
      emitEpochStatus("datum_failed", es); }
    prev_week = se.time.week; prev_tow = se.time.tow;
    result.action = EpochAction::SkipEpoch;
    return result;
}

// Constraint-realized clock datums: fold the gauge rows onto the shared border
// before the null-space gate.  Sum-to-zero mode carries one Σδtˢ = 0 row per
// constellation; broadcast-anchored mode carries one Δδtˢ → 0 anchor row per
// satellite at weight 1/σ² (there the anchors are the datum).  In both modes plan
// carries constraint rows instead of hard pins, and declared_dimension is 0.  The
// rows are folded like the code-bias gauge (additive information; the within-epoch
// re-solve re-applies them from plan, which is in scope there).  In pin mode plan
// has 0 rows and this is a no-op (byte-identical).
if (plan.clock_constraint_H.rows() > 0) {
    est.attribTag("clock_datum_rows");
    est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                plan.clock_constraint_weight);
}

// Pivot handover: the previous epoch's shared estimate is in the old datum, so a
// changed clock pivot invalidates the D2 carry-forward (and forces a discontinuity
// bump downstream).  Detect a clock-pivot datum re-reference and record which
// constellations changed, for the A2 per-constellation emit-bump scoping (A1, A2).
// Arm (b), vanish-return, is gated on network.pivot_vanish_return_signal; with it
// off, detection is the legacy adjacent-epoch-only test.
std::set<char> changed_consts;
const bool datum_changed = pivotHandoverSignalled(
    plan.clock_pivot, prev_pivot, last_pivot, pivot_vr_signal, changed_consts);
// Whether a key's carried state belongs to a retired datum.  With
// network.scoped_handover_response off this equals datum_changed (the legacy
// global response, byte-identical); with it on, only keys of the constellations
// whose pivot changed are invalidated, taken from the same changed_consts set that
// the A2 emit bump consumes (RTA-2).
const auto handover_affects = [&](const std::string& sat) {
    return datum_changed
        && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
};

networkInjectShVtecPrior(ctx, ws, config_, est, sh_model.get(),
                         sh_model2.get(), datum_changed,
                         dt, within_epoch_armed);

networkInjectPhaseHoldPrior(ctx, ws, est, within_epoch_armed);

// The open pin bridges (eq34 reattach hygiene, INC-2).  Injected before the gate for
// the same reason as the phase hold above: for a bridged key this soft row is the
// datum (the election left the key unpinned on purpose), so it must be in the reduced
// normals that the gate inspects.  No-op when no bridge is open.
networkInjectPinBridgePrior(ctx, ws, est, within_epoch_armed, log);
// Record this epoch's clock-datum verdict for the next election's bridge lifecycle.
// A pivot handover re-references every satellite phase bias (the phase observable
// ties δtˢ + b_s), so a level harvested before it belongs to the retired frame, and
// the bridge drops at the next hygiene pass with reason=datum.  The one-epoch
// exposure is deliberate and bounded: the election commits to the bridge before
// buildSharedDatum has chosen this epoch's pivots, and the row is soft, so a value
// one epoch stale is outweighed by the data everywhere except along the gauge
// direction that the row owns (docs/SPEC_eq34_reattach_hygiene.md §7 declares this
// deferral).
ctx.carry.last_datum_changed = datum_changed;

// The prioritized phase-bias datum drop-and-heal lives in filter/NetworkPhaseDatum
// (INC-8), with the anchor election and the incumbent carry (INC-8b).  Its function
// Doxygen holds the rationale and the performance note: the full eigenvector
// decomposition runs on the gate-failure path only, never per epoch.

// Null-space rank gate.  The reduced shared normals are ill-conditioned by design:
// the phase rows pin the satellite-clock / phase-bias sum to about σ_phase
// precision (weight about 1e5), while only code separates the two (the
// clock-vs-bias split), so that split is orders of magnitude weaker but still
// non-zero.  Measured spectra show a clean gap of about 7 decades between the true
// gauge freedoms (the per-constellation clock datum, σ ~ 1e-10 of σ_max) and the
// weakest real direction (σ ~ 1e-2).  A relative tolerance of 1e-6 sits above the
// weak but real directions and miscounts them as deficiencies; 1e-9 lands in the
// GPS+Galileo gap, counting only the true gauges while still catching an unpinned
// datum.
//
// The gap moves with the constellation mix, so the tolerance is a per-config
// calibration parameter (network.rank_gate_tol), resolved once into
// est.setRankGateTol() and read here.  The estimability dump and the drop-and-heal
// read the same value (DG2): a private tolerance would make the dump describe a
// decision the gate did not make, or make the heal soft-anchor weak but real
// directions.  It defaults to NetworkEstimator::kNullSpaceTol (1e-9), so an unset
// key is byte-identical.  With BeiDou the default miscounts in the same way: the
// BDS weak-but-real family layer sits at about 1.7e-10 of λmax, about 3 decades
// deeper, with a measured six-decade gap down to the declared gauges (card DG).

// ---- Estimability diagnostic (default off) --------------------------
// The spectrum dump is NetworkEstimator::dumpEstimabilitySpectrum (INC-8), whose
// Doxygen holds the derivation and the eigen-vs-SVD cost rationale.  The returned
// struct holds the numbers the warn lines print, mirrored into
// network_estimability.csv so the storm analyzer reads a CSV instead of parsing
// log text (DG5).
// The budget is re-armable (RF-NS): network.trace_estimability_budget sets how many
// evaluations it may spend (default 6), and network.trace_estimability_deficit_only
// spends them only on epochs whose census shows a deficit.  A fixed budget of 6 is
// spent during ignition on a field run, leaving later withhold stretches without a
// spectrum.  The budget is decremented after the dump, not before, so a clean epoch
// skipped under deficit_only does not consume it.  Otherwise every epoch that
// reaches this block decrements it, whether or not the dump is armed, and nothing
// between the two points reads the counter.  The W6-cb first-N log above is gated
// on the same budget.
if (spectrum_diag_budget > 0) {
    bool spend = true;
    if (nc.trace_estimability) {
        const auto dump = est.dumpEstimabilitySpectrum(
            se.time.tow, plan.declared_dimension, col_desc, log);
        if (dump.valid) {
            // The census has to run to tell whether this epoch is in deficit
            // (there is no cheaper predicate for "n_below > declared"), so the
            // eigen-decomposition cost is paid on every budgeted epoch either
            // way; deficit_only saves the budget and the rows.
            if (nc.trace_estimability_deficit_only
                && dump.n_below <= dump.declared_dim) {
                spend = false;
            } else {
                traces.writeEstimabilityEpochRow(se.time, dump.tow, dump.n_shared,
                                                 dump.declared_dim, dump.ev_max,
                                                 dump.cutoff, dump.n_below);
                for (const auto& d : dump.dirs)
                    traces.writeEstimabilityDirRow(se.time, d.ev, d.top);
            }
        }
    }
    if (spend) --spectrum_diag_budget;
}

bool gate_ok = true;
// The observed near-null count is captured on both the passing and the throwing
// path, because the exception text cannot be parsed back into a number (DG6).
// "Under-specified" (observed > declared: a missing datum constraint) and
// "over-specified" (observed < declared: a tolerance below this config's gauge
// depth, a mis-calibration that withholds a sound config) need opposite fixes, so
// they are counted separately.
// The K2 anti-fragility margin of the KP prior is logged once, on the same reduced
// system the gate is about to judge (lambda_max here equals the gate's, and the
// reduction includes the clock datum rows).  The CB-ADM card rejected a loose prior
// whose information sat at the rank cutoff, so this one must sit at least 2
// decades above it; a margin under 2 decades logs a warning (card KP).
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

// --- S-basis completion, SBC (network.sbasis_complete) -------------------
// The declared dimension counts only the structural gauges (the per-constellation
// clock in pin mode); a realized epoch can carry more.  Measured on GPS+Galileo:
// per-satellite channel-family phase-bias↔iono trade directions (eigenvector
// ∝ μ_σ over one Q/X receiver family's PB columns) that only the SH prior resists
// once that family's code support thins.  Derive the realized dimension from the
// same matrix state the gate inspects, and constrain each extra direction along
// its own eigenvector at its carried level, so the unobservable combination holds
// instead of stepping (engage edge) and the data re-determine it from the held
// level when observability returns (release edge); both edges are evented
// (standing invariant #7).  A refusal (cap, degenerate split, marginal weight)
// folds nothing and falls through to the unchanged gate, which withholds; card PA
// remains the armed backstop.
if (ctx.knobs.sbasis_complete_on) {
    // Continuity targets: the carried shared state mapped into this epoch's
    // columns.  Phase biases come from pb_level_prev (prev_carry omits them under
    // carry_invariant, the same gap the INC-2 pin bridge meets); code biases take
    // the card-2 kept-epoch map where it has a value.  On a datum re-reference the
    // carried values belong to the retired frame, so the targets re-seed at 0, like
    // every other re-referenced quantity.
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
            } catch (...) { /* departed key: no target */ }
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
    // --- Datum graph census hand-off (docs/SPEC_datum_graph_census.md) -----
    // The spectral half of the census's contingency, with direction identities,
    // is captured here: this is the only place in the run where the realized
    // near-null count is measured before completion and its directions still
    // carry attribution.  The gate below runs on the completed system and would
    // report `observed == declared`, the completion's own success, which is not
    // the quantity the census reconciles against.  (Runs without SBC get a
    // count-only fallback after the gate.)
    //
    // Measure-only and armed-only: only the datum graph census reads
    // ctx.carry.dg_spec, so the solve is unaffected and an unarmed run pays one
    // predictable branch.  The identities come from parsing `d.detail`, the same
    // string that the sbc_engage / sbc_skip events carry, so the census and the
    // event stream agree on a direction's identity by construction rather than
    // through two derivations that could drift.  Both populations are recorded:
    // the constrained extras and the skipped orphans are equally "observed", and
    // dropping the orphans would empty the `observed only` cell, which measures
    // the limit of this predictor.
    if (ctx.knobs.dg_on) {
        auto& dgs = ctx.carry.dg_spec;
        dgs.valid    = true;
        dgs.from_sbc = true;
        dgs.observed = sbc.observed;
        dgs.declared = plan.declared_dimension;
        // Overwrite, never append: if a within-epoch adaptation ever re-enters
        // this body, the census must see this evaluation's directions, not the
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
    // A per-direction skip (a direction not separated from the declared-gauge
    // cluster: an orphaned exact gauge, card PA's class) does not end the epoch:
    // the separated extras still fold below, the gate then throws by exactly
    // n_skipped, and the drop-and-heal works on that remainder.  Skips are
    // counted per direction and each one is evented, so none goes unreported.
    if (sbc.n_skipped > 0) {
        ctx.counters.n_sbc_degenerate += static_cast<std::size_t>(sbc.n_skipped);
        log->debug("Network (SBC): TOW {:.1f} skipped {} unseparated extra "
                   "direction(s) (machine-zero orphan class — left to the "
                   "drop-and-heal); {} separated extra(s) proceed.",
                   se.time.tow, sbc.n_skipped, sbc.n_extra);
        // One attributed event per skipped direction: a skipped orphan that then
        // withholds the epoch must name its own columns, or the withhold cannot be
        // diagnosed from the trace.  The heal does not see this class (no PB span
        // ≥ 0.3), so this event is its only attribution.
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
        // The active set is left unchanged on purpose: nothing was constrained and
        // the epoch is headed for a withhold, so a release edge here would report a
        // re-coupling that did not happen.
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
        // Release edge (invariant #7's re-coupling): a satellite constrained last
        // epoch and in no direction now has been re-observed, and the data take
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
        // Clean epoch (no extras): every satellite constrained on an earlier epoch
        // has been re-observed, so emit its release and clear the set.
        for (const auto& s : ctx.carry.sbc_active_sats) {
            ++ctx.counters.n_sbc_release;
            emitEvent("GATE", "sbc_release", s, "", std::nullopt, std::nullopt,
                      "reobserved", "held combination re-determined by data");
        }
        ctx.carry.sbc_active_sats.clear();
    }
}

int gate_observed       = -1;   // the last evaluation this epoch (after the heal if a re-gate ran)
int gate_first_observed = -1;   // this epoch's first evaluation: the DG6 min/max sample
double gate_ev_max      = -1.0; // λmax of the first evaluation, which sets the gate's own cutoff (invariant #7)
++ctx.counters.n_gate_evaluated;
try {
    sb.assertNullSpaceRank(est.reducedSharedNormals(), plan.declared_dimension,
                           est.rankGateTol(), &gate_observed, &gate_ev_max);
    gate_first_observed = gate_observed;
} catch (const std::exception& ex) {
    // assertNullSpaceRank writes observed_out before the equality test, so the
    // count that caused this throw is already in gate_observed; latch it now,
    // before a possible re-gate overwrites it.
    gate_first_observed = gate_observed;
    // The near-null count differs from the declared dimension.  Typically the
    // reduced shared system carries more gauge freedoms than the declared datum
    // can pin: a satellite that is momentarily unobservable (seen by a single
    // station at marginal geometry) or, under the prioritized datum, a
    // phase-bias column whose anchor was dropped or whose separation is weak.
    // In the prioritized case, soft-anchor those near-null phase-bias columns and
    // gate again before withholding (the eigenvector heal runs only here, not
    // every epoch).  If the datum is still deficient after the heal (or on the
    // legacy path), the product would be an arbitrary least-norm split, so
    // withhold (the plan's "gate emission until convergence" safeguard).
    bool recovered = false;
    // Per-healed-column attribution for the trace_events rows below (PA2).
    // Declared outside the inner try so the rows are still emitted when the
    // re-gate, not the heal, throws: a heal that fired and did not recover the
    // epoch is the case these rows exist for.
    std::vector<PhaseHealEvent> heal_events;
    int healed_n = 0;   // > 0 only once the heal has returned (its priors are folded)
    // The heal / re-gate failure text is captured rather than logged in place, so
    // the single withhold warning below carries the whole user-visible report
    // (DG6).  It stays empty when the inner try returns without throwing, which
    // includes healed == 0: the re-gate is skipped, nothing in the block can
    // throw, and the epoch reaches the withhold with no exception of its own.
    std::string heal_fail_what;
    try {
        const int healed = healNearNullPhaseBias(
            est, shared_phase_keys, phase_datum_prioritized,
            within_epoch_armed, cap_priors,    // 0 unless prioritized + near-null PB
            &heal_events);
        healed_n = healed;
        // Count the attempt before the re-gate can throw: the success-only pair
        // below cannot tell "the heal never ran" from "the heal ran every epoch
        // and never recovered the gate" (PA2).
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
        // deficiency it could not resolve: withhold rather than crash.  The report
        // is deferred to the single warning on the !recovered path, because this
        // catch is not the only route to a withhold; a warning emitted only here
        // would leave the other routes visible at debug level alone.
        heal_fail_what = ex2.what();
    }
    // One row per soft-anchored column (PA2), with no station because the heal
    // acts on a shared column.  statistic is the eigenvalue of the near-null
    // direction the column spans most heavily, threshold is that |component|, and
    // action records whether the heal recovered the epoch, so an attributable but
    // ineffective heal can be told from a working one.  Gated on healed_n > 0 so
    // a heal that threw mid-fold cannot emit rows claiming anchored columns when
    // no prior was installed; the re-gate throw (healed_n > 0, recovered false)
    // still emits its rows.  No-op unless network.trace_events.
    if (healed_n > 0) {
        for (const auto& h : heal_events)
            emitEvent("GATE", "phase_heal", h.sat, "", h.ev, h.span,
                      recovered ? "soft_anchor" : "soft_anchor_insufficient",
                      h.signal);
    }
    if (!recovered) {
        gate_ok = false;
        ++n_gate_withheld;
        // Split the withhold by direction (DG6).  observed > declared is a missing
        // datum constraint (the BeiDou storm); observed < declared means the
        // tolerance sits below this config's own gauge depth, so the gauges are no
        // longer counted: a mis-calibration withholding a sound config, which no
        // datum work will fix.  gate_observed is what the last gate evaluation of
        // this epoch measured; an unknown count (< 0) is counted as under.
        if (gate_observed >= 0 && gate_observed < plan.declared_dimension)
            ++ctx.counters.n_gate_withheld_over;
        else
            ++ctx.counters.n_gate_withheld_under;
        // One warning line on every withhold, whichever route reached it,
        // reporting both gate evaluations (DG6).  A heal that anchors 0 columns
        // (a machine-zero orphan direction with no phase-bias span) skips the
        // re-gate, so the inner catch never runs; this line still reports the
        // withhold.  The count reported first is the first evaluation's
        // (gate_first_observed; ex.what() is also from the first evaluation), not
        // the re-gate's, which measures only the deficiency left after the heal's
        // soft anchors and is reported after it.  This line and the gate_withheld
        // CSV row below therefore report the same count.
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
// Run-wide envelope of the observed near-null count, one sample per epoch from the
// first gate evaluation (DG6); a re-gate after the heal answers a different
// question and is counted by n_healed_epochs.  DG3's placement rule is audited
// against this envelope: a run whose observed count never leaves {declared} needs
// no calibration.
if (gate_first_observed >= 0) {
    auto& lo = ctx.counters.nullspace_obs_min;
    auto& hi = ctx.counters.nullspace_obs_max;
    if (lo < 0 || gate_first_observed < lo) lo = gate_first_observed;
    if (hi < 0 || gate_first_observed > hi) hi = gate_first_observed;
}
// Measure how much information each declared datum direction carries (standing
// invariant #7, docs/SPEC_card1_coupling_carry.md §13), on the same normals and
// cutoff the gate just judged.  The gate's integer count cannot tell "the tolerance
// is misplaced" from "a carry filled a gauge direction", and the two need opposite
// fixes; this ratio tells them apart, so a filled gauge direction is reported.
// Measure-only, unconditional, about one k×k quadratic form per constellation.
// Runs after the withhold accounting above so the coincidence counter (an
// over-specified withhold on a breached epoch) is formed here in one place.
networkDatumInfoRatio(ctx, traces, se, est, plan.declared_dimension,
                      registered_clock_sats, gate_ev_max,
                      /*gate_over_withheld=*/!gate_ok && gate_observed >= 0
                          && gate_observed < plan.declared_dimension);
// Datum graph census: the fallback spectral hand-off, for runs without SBC.  The
// gate's own first evaluation is then the only realized near-null count, a count
// with no direction identities, which the census labels `obs_src=count` rather
// than attributing it.  An SBC capture is not overwritten: with the completion
// armed, the gate inspects the completed system and reads back the declared
// dimension, so its count would report the completion's success instead of the
// deficiency the census reconciles against.
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
    // Product-level heartbeat for a withheld epoch: an H line with no E line, so a
    // live tail can tell "gate withheld" from "producer died" (RTA-10).  Before the
    // deferred product writer opens (at the first emit) the heartbeat is buffered in
    // setup.pending_h_pre_open and flushed at the open, so a leg that opens inside a
    // withhold era still records the era.  The RAIM σ̂0 is not available before the
    // solve (0 per the H grammar); withheld="*" means the whole epoch.
    if (emit_epoch_status) {
        NetworkProducerStatus hst;
        hst.time.week = se.time.week;
        hst.time.tow  = se.time.tow;
        hst.state     = NetworkProducerStatus::State::WithheldGate;
        // The census σ̂₀ is computed before the solve, so unlike the RAIM
        // statistic it exists on a gate-withheld epoch; 0.0 stays the sentinel
        // when the feed is off or the pool was empty (H-rider).
        if (ctx.knobs.h_sigma0_census) hst.sigma0 = ctx.carry.ep_h_sigma0_census;
        hst.withheld  = "*";
        if (product_writer) {
            product_writer->writeEpochStatus(hst);
            if (nc.product_flush_per_epoch) product_writer->flush();
        } else if (setup.contract_pending) {
            // Buffer only while a v2 writer open is still pending; with no
            // product path (or a v1 immediate open) there is nothing to flush into.
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
// network.emit_clock_rate: same warmup gate as the clock model, skipping the cold-start
// epochs where the floated clock would finite-difference into a large spurious C1 rate.
const bool emit_rate_warm = emit_clock_rate_on && (static_cast<int>(n_emitted) >= pl_warmup);
auto& clk_pred = ws.clk_pred;
auto& clk_prior_info = ws.clk_prior_info;

// --- Ephemeris-cutover re-level (card 6, RTF-2 / plan D3) ---------------------------
// Re-base the carried clock state by -Delta_eph on a broadcast IODE roll.  This must
// run before the prior injection and the solve; filter/NetworkRelevel holds the full
// ordering rationale.
networkEphCutoverRelevel(ctx, log, traces, se, registered_clock_sats, brdc_rts);

// --- Clock-pivot handover S-transform (queue 1(b), SPEC_pivot_visibility_gauge_defect §8)
// Re-express the changed constellation's carried shared means in the incoming clock
// datum and mark them for fold release, so the prior below folds instead of being
// skipped for one epoch.  It sits immediately before the first fold that reads
// prev_carry: every consumer of the carry must see the same frame, and this is the
// last point at which that holds by construction rather than by an audit of the seams
// in between.  It runs after the ephemeris-cutover re-level so the two re-basings
// compose in the order the carry experienced them: the cutover changes the a-priori
// the carry is expressed against, and the handover changes the gauge the carry
// realizes.  No-op and byte-identical when the knob is off; it clears its own release
// set every epoch.
networkHandoverStransform(ctx, config_, log, traces, se.time, plan, datum_changed,
                          changed_consts, registered_clock_sats);

networkInjectD2CarryPrior(ctx, ws, traces, se.time, est, datum_changed,
                          changed_consts, dt, within_epoch_armed, log);

networkInjectClockOscPrior(ctx, ws, traces, se.time, est, registered_clock_sats,
                           clk_model_warm, datum_changed, changed_consts,
                           pl_now_s, dt, within_epoch_armed);

// --- A-priori satellite-clock anchor (cold-start conditioning) ------
// The shared clock column is Δδtˢ relative to the a-priori precise product clock,
// so it is small (sub-metre for precise post-processing).  With no absolute anchor
// the per-epoch joint solve floats it by hundreds of metres at cold start to absorb
// the clock↔iono↔bias confusion (no carry-forward exists at epoch 1, and from
// epoch 2 on the carry anchors to that floated value).  A Tikhonov prior pulling
// each Δδtˢ toward 0 with σ = initial_std.sat_clock (the documented "Δδtˢ vs
// a-priori product clock" quality: sub-metre for precise products, looser for RT
// broadcast) regularizes only the ill-conditioned cold-start direction.  The
// per-constellation clock datum stays fixed by the explicit Datum (a large-weight
// pin that dominates this soft prior), so the few-metre datum offset that the data
// determine is kept.  The prior is applied after the gate (like the carry-forward)
// so the null-space rank gate still sees the clock datum as a gauge, and it
// combines in information space with the carry-forward (anchor toward 0 plus random
// walk toward the previous value).  It applies only when σ binds (< 100 m); the
// loose default (1000 m) leaves the anchor off and the run byte-identical.
//
// Measured scope (305 stations, GPS+Galileo; see [[project_global_sh_iono]]):
// σ = 0.05 m reduces the cold-start clock-Δ rms from 56 to 2.9 m for about 6
// epochs, after which the W6 clock↔bias↔ambiguity systematic returns (the data want
// about 80 m, with information far above the anchor's).  The anchor improves
// cold-start clock-product quality only.  The divergence of the co-estimated
// SH-VTEC field is a separate, decoupled cold-start estimability defect (its
// large-scale level is unobservable under the common-mode-free iono R), and
// pinning the clock does not move the field.
{
    const double clk_sigma = clk_anchor_sigma;  // m (D6-resolved; sp3 auto == initial_std.sat_clock)
    // Broadcast-anchored datum: the same per-satellite anchors were folded before
    // the gate as the gauge realization (buildSharedDatum constraint rows), and
    // folding them again here would double the anchor information (effective
    // σ/√2).  Skip them in that mode; every other mode keeps the post-gate
    // regularizer.
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

// A clock-pivot handover re-references the whole shared solution; the forced bump
// steps both the phase and the code discontinuity counters at the next emit.  (The
// emitter detects the code per-(constellation, signal) sum-to-zero and IF-pin
// re-references from the emitted epoch itself, see NetworkProductEmitter; only this
// clock-pivot signal is known to the driver alone.)  The emitter datum bump
// (markDatumChange) and the prev_pivot / last_pivot baseline advance are both
// deferred to the emit site, because the maps must describe the datum of the last
// emitted product (the consumer's view).  Advancing them here, before the solve,
// would make a handover epoch that is then skipped (solve failure, σ̂0 withhold)
// compare equal on the next epoch: the discontinuity signal would be lost, and the
// old-datum prev_carry / clock_osc would fold as priors into the new-datum solve.
// With the baseline frozen across skipped epochs, every skip path re-detects the
// handover until an epoch emits in the new datum.

Eigen::VectorXd x_s;
try {
    // parallel_carry: all of est.solve() is Eigen-threaded work.  foldReducedDdPriors
    // spreads its per-station reduction across threads itself (with Eigen pinned), and
    // its dense GEMMs and the K×K middle inverse run under Eigen's own threading.  So,
    // unlike the two isolated build/install GEMM stages, solve() is not wrapped in a
    // CarryEigenScope: an un-pin here would nest the fold's OpenMP inside Eigen's GEMM
    // threads.  For the same reason parallel_carry cannot coexist with parallel_reduce
    // (which would spread this Eigen-threaded solve across stations with OpenMP); the
    // loader keeps the two mutually exclusive.
    x_s = est.solve(plan.shared_datum);
    // Debug: full-precision solve checksum for differencing A/B arms without print
    // precision loss.  Budgeted by n_cpl_solve_dump; log only (card 1d).
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
      emitEpochStatus("solve_failed", es); }   // x_s is empty: no clock stats
    prev_week = se.time.week; prev_tow = se.time.tow;
    result.action = EpochAction::SkipEpoch;
    return result;
}

// Satellites whose carried shared state (clock and coupled phase/code bias) is
// refloated at the end-of-epoch stash: those flagged by the per-satellite clock
// monitor (below; it detects a lone lock-in that the σ̂0 RAIM misses) and, only when
// clock_monitor_couple_raim is opted in, every satellite the within-epoch or
// post-fit RAIM flags, so a slip that leaked into the shared clock reinitializes the
// clock as well as the ambiguity.  Empty unless network.clock_monitor, so the stash
// is byte-identical when off.
std::set<std::string> clock_refloat_sats;
int n_arc_refloat_epoch = 0;   // isolated-arc RAIM refloats executed this epoch, a heartbeat metric (T2d)
// Retro-bump / URA-inflate feed (B1, P0-2): the union of satellites that any QC
// tier flags this epoch, independent of clock_monitor_couple_raim.
// clock_refloat_sats captures the RAIM tiers only under the couple opt-in (it drives
// the carry refloat, a numeric action), so a separate accumulator lets the
// retro-bump reach every tier without forcing a carry refloat.  Populated only when
// a URA feature that consumes it is on; otherwise inert (byte-identical).
std::set<std::string> retro_flag_sats;
const bool retro_feed = ura_retro || ura_refloat_inflate > 1.0;

// --- Guarded temporal clock anchor (card Q3, docs/SPEC_clock_temporal_anchor.md) ---
// Must run here: after the primary solve and before the within-epoch adaptation
// below.  A stiff clock prior fighting a real step pushes the tension into the
// residuals and raises σ̂0, and the adaptation would then attribute a prior fault to
// a satellite's or a station's data and drop it.  A prior fault is resolved first,
// by refloating the prior (never by de-weighting a measurement) and re-solving the
// epoch in place, so the assessment below sees the corrected solve and the epoch
// emits (adapt-and-emit; a withhold here would be an adaptation failure).
// clock_refloat_sats is always empty at this call (nothing is inserted before the
// within-epoch adaptation below), so the guard's `ext` coupling reads no satellites
// on this path.  No-op, with no state allocation, when off.
networkClockAnchorGuard(ctx, config_, log, traces, se.time, est, plan, x_s, ws,
                        registered_clock_sats, clock_refloat_sats, datum_changed,
                        changed_consts);

// --- Within-epoch adaptation: drop the dominant culprits and re-solve ----------
// When σ̂0 (the robust a-posteriori variance factor) shows that the joint solve is
// contaminated, remove the single most prominent culprit, a whole satellite (its
// rows de-weighted network-wide) or a whole station (excluded), re-solve this epoch
// over a fresh beginEpoch() replay of the captured shared system, and repeat until
// σ̂0 falls below the trigger or a cap is reached.  Only the (rare, already
// contaminated) trigger epochs pay for the re-solve; try/catch reverts to the last
// good solve on any failure.  Attribution uses the same robust median polish as the
// RAIM refloat.
int  n_adapt = 0;
bool we_adapt_attempted = false;   // a within-epoch re-solve was launched (est layout was rebuilt)
double sigma0_adapt = 0.0;         // final within-epoch σ̂0; 0 = adaptation did not run this epoch (A4)
// Withhold-era continuity, resolved once per epoch (SPEC_withhold_era_release; the
// census-freshness idiom): the era is live if and only if the immediately previous
// loop epoch was the last A4-withheld decision, so any bypass or skip epoch breaks
// it (C5, C7).  Declared at function scope, like sigma0_adapt, because the A4
// decision site reads both after the within-epoch block closes.
const bool wh_era_live = ctx.carry.wh_era_len > 0 &&
    ctx.carry.wh_last_loop_ep ==
        static_cast<long long>(ctx.counters.n_loop_epochs) - 1;
bool wh_escalated = false;         // in effect this epoch: the budget escalation of §3(a)
// Per-constellation partition of the last assess()'s standardized-residual pool
// (RTA-10, network.per_const_withhold): the rows whose median is the global σ̂0 that
// the A4 withhold compares, keyed by the ambiguity's satellite constellation
// (pa.sat[0]).  Cleared and refilled on every assess() so that, like sigma0_adapt,
// it describes the final within-epoch solve.  Populated only when the carve-out is
// armed; otherwise empty, with no cost and no change on the default path.
std::map<char, std::vector<double>> we_sigma0_pool_c;
// The B8/T2 flag rate (first-solve σ̂0 > trigger) is decided here but committed at the
// emit site, so an epoch withheld later is excluded from T2's denominator, as for
// T3–T6, which accrue only on emitted epochs.  Counting withheld (contaminated) epochs
// in T2 alone would make its alpha incomparable with the post-fit tiers' (R12).
bool we_t2_opp = false, we_t2_flag = false;
// Reset the tier's per-epoch observability (R0, SPEC_pivot_visibility_gauge_defect
// §5.07.5) on every epoch that reaches the solve, so an epoch the tier does not
// assess leaves the epoch-status columns blank instead of repeating the last
// assessed epoch's numbers ("unavailable is not 0").
ctx.carry.ep_sigma0_first    = -1.0;
ctx.carry.ep_we_refloat_sats = -1;
ctx.carry.ep_we_clk_move_max = -1.0;
ctx.carry.ep_we_clk_move_sat.clear();
if (within_epoch_armed && carry_warm) {
    const double trig      = config_.networkRaimSigma0Trigger();
    const bool   surgical  = config_.networkRaimSurgicalStation();
    const double sta_bias  = surgical ? config_.networkRaimStationDropBias() : 1.0;
    constexpr int    kMaxAdapt   = 5;
    // Inside a live withhold era the adaptation budget escalates, per
    // SPEC_withhold_era_release §3(a), so the era can end through the sanctioned
    // route: a landed adaptation takes the normal accepted path, and the full
    // downstream recovery (harvest and post-fit RAIM commit) runs.  Nothing else
    // changes: same culprit scoring, same k-gate, same σ̂0, same ceiling.  Measured
    // motivation: in a withhold era, n_adapt sat at kMaxAdapt on every era epoch
    // with culprits still clearing the k-gate.  The loader guarantees that
    // wh_escalate is 0 or > kMaxAdapt.
    wh_escalated = wh_era_live && ctx.knobs.wh_escalate > kMaxAdapt;
    const int max_adapt_eff = wh_escalated ? ctx.knobs.wh_escalate : kMaxAdapt;
    // --- Code and iono-constraint row pools (WE-CI, network.raim_we_code_iono) ---
    // The pool below holds the carried-ambiguity phase rows.  Without WE-CI no tier
    // screens the code rows or the per-LOS↔SH iono-constraint rows (P1-4 skips
    // SH-touching rows by design), so a fault entering through them is not seen.
    // When armed, assess() also pools them into per-pool robust σ̂0 with their own
    // triggers, and the adaptation loop gains two response classes: epoch-scoped
    // station exclusion (for a fault concentrated at a few stations) and a
    // network-wide satellite code-row de-weight (σ×10, a fallback rather than a
    // drop).  The phase pool, σ̂0_adapt and the A4 ceiling are unchanged; every new
    // branch is behind we_ci, so with it off the run is byte-identical.
    const bool   we_ci     = config_.networkRaimWeCodeIono();
    const double trig_code = config_.networkRaimWeCodeTrigger();
    const double trig_iono = config_.networkRaimWeIonoTrigger();
    constexpr double kWeCiCodeWeightScale = 1e-2;   // de-weighted code row: w×1e-2 = σ×10
    // Shared-column classification, built once per epoch: the shared layout is
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

    // Culprit sets.  A bad ambiguity (a cycle slip, the dominant contamination the
    // carried-phase RAIM sees) needs reinitialization, not measurement de-weighting:
    // the re-solve refloats the culprit's ambiguities (withholds their stale carry
    // prior) so they re-estimate from full-weight data and absorb the slip locally.
    // A whole bad station (a data-quality problem, not a slip) is excluded, which is
    // the de-weight / exclude tool, the right one for a bad measurement.
    std::set<std::string> drop_sats;                       // refloat that sat's ambiguities network-wide
    std::set<int>         drop_stations;                   // excluded entirely (bad-data case)
    std::map<int, std::set<std::string>> drop_arcs;        // (station → sats) refloat at that station only
    // The DD-carry re-solve (buildCarryDdPriors, via resolve_dropping) treats a
    // mid-epoch RAIM culprit as slipped, so its stale carry is not folded again
    // (C3c §3, H13 plus culprit withhold: refold from the unchanged predicted
    // state).  The aliases live only while this block runs and are nulled before
    // the sets leave scope.
    carry_we_drop_sats = &drop_sats;
    carry_we_drop_arcs = &drop_arcs;

    // σ̂0 and median-polish effects of the current shared solution.  The (station × sat)
    // residual table (carried-ambiguity phase rows only) decomposes additively into a
    // per-satellite column effect, a per-station row effect and an isolated-arc
    // interaction.  Computed over every present ambiguity: a refloated ambiguity fits
    // its own data (its residual collapses after the re-solve), so σ̂0 remains a
    // faithful measure of the remaining contamination without special cases; the
    // culprit is kept out of the next pick by the candidate-set exclusion, not by
    // hiding it here.
    auto assess = [&](const Eigen::VectorXd& xs, double& sigma0_out,
                      std::map<std::string, double>& sat_eff,
                      std::map<int, double>& sta_eff,
                      std::map<std::pair<int, std::string>, double>& arc_eff) {
        std::vector<double> r_pool;
        std::vector<double> ci_code_pool, ci_iono_pool;   // per-pool standardized rows (WE-CI)
        if (we_ci) { ci_sat_code_max.clear(); ci_sta_max.clear(); }
        if (perconst_on) we_sigma0_pool_c.clear();   // re-partition per assess (RTA-10)
        std::map<std::pair<int, std::string>, double> cell_max;
        for (size_t i = 0; i < stations.size(); ++i) {
            // Prioritized datum: keep the legacy pivot station in the σ̂0 pool, as
            // it carries fault-testable non-anchored arcs; its anchored (pinned)
            // keys are already absent from R.pl_amb (per-(sat, signal) partition).
            if (!orig_added[i] || est_index_of[i] < 0
                || (!phase_datum_prioritized && stations[i].is_pivot)) continue;
            const StationAssembly& R = asm_res[i];
            if (R.A_s.cols() != xs.size()) continue;
            // WE-CI also screens stations with no carried ambiguity (their code
            // and iono rows are data too); with it off, such stations are skipped.
            if (R.pl_amb.empty() && !we_ci) continue;
            Eigen::VectorXd x_r;
            try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), xs); }
            catch (...) { continue; }
            if (x_r.size() != R.A_r.cols()) continue;
            const Eigen::VectorXd resid = R.y - R.A_s * xs - R.A_r * x_r;
            Eigen::VectorXd row_r;   // per-row Baarda redundancy, empty when off (B2)
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
                    if (raim_redun && row < row_r.size()) {   // standardize v/(σ·√r_i) (B2)
                        constexpr double kRedFloor = 1e-6;
                        const double ri = row_r(row);
                        if (ri <= kRedFloor) continue;   // uncheckable row: exclude from σ̂0 pool + r_max
                        r /= std::sqrt(ri);
                    }
                    r_pool.push_back(r);
                    // The same standardized row (weighting and B2 √r_i normalization
                    // included; redundancy-floored rows excluded by the continue
                    // above), partitioned by constellation (RTA-10).
                    if (perconst_on && !pa.sat.empty())
                        we_sigma0_pool_c[pa.sat[0]].push_back(r);
                    if (r > r_max) r_max = r;
                }
                const auto key = std::make_pair(static_cast<int>(i), pa.sat);
                auto it = cell_max.find(key);
                if (it == cell_max.end() || r_max > it->second) cell_max[key] = r_max;
            }
            // --- Pool this station's code and iono-constraint rows (WE-CI) -------
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
        if (we_ci) {   // per-pool robust σ̂0, same estimator as the phase pool (WE-CI)
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
    // only reset() would clear it), refloating the culprit ambiguities.  Rebuilds
    // est_index_of so reconstructStation / harvest stay valid on the final solve.  The
    // measurements stay at full weight (apart from the WE-CI code-row de-weight below):
    // de-weighting is the tool for a bad measurement, whereas a slipped ambiguity needs
    // a new value.  A satellite culprit refloats its ambiguities at every station, an
    // arc culprit at one station only; a whole-station culprit is excluded (its data,
    // not a single ambiguity, is bad).  A refloated ambiguity keeps its full-weight
    // data (so its local block stays positive-definite, with no singularity from a
    // drop) but withholds its carry prior, so the re-solve lets it find its new value
    // and absorb the slip locally; the harvest then stashes that corrected value (a
    // self-contained reinit that does not rely on the post-fit refloat).
    auto resolve_dropping = [&]() -> Eigen::VectorXd {
        est.beginEpoch();
        for (size_t i = 0; i < stations.size(); ++i) {
            if (!orig_added[i] || drop_stations.count(static_cast<int>(i))) {
                est_index_of[i] = -1;
                continue;
            }
            const StationAssembly& R = asm_res[i];
            // De-weight the culprit satellites' code rows (σ×10) in a per-station
            // copy (WE-CI); `*wr` binds R.w itself when WE-CI is off and whenever no
            // row matches, so the default numbers are unchanged.
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
            // A_r columns of this station's ambiguities to refloat (withhold their prior).
            // A satellite culprit refloats its ambiguities at every station tracking it: a
            // satellite-level contamination pulls the shared clock at all its stations,
            // and its arcs are contaminated to varying degrees.  A strict per-arc gate on
            // the (inflated) σ̂0 would miss the moderately slipped ones and leave a worse
            // mixed state, so the whole satellite is reinitialized consistently.  An arc
            // culprit refloats at one station only (a surgical response to an isolated
            // slip).
            std::set<Eigen::Index> refloat_cols;
            const auto dai = drop_arcs.find(static_cast<int>(i));
            for (const auto& pa : R.pl_amb)
                if (drop_sats.count(pa.sat) ||
                    (dai != drop_arcs.end() && dai->second.count(pa.sat)))
                    refloat_cols.insert(pa.col);
            // Under the conditional carry, a refloated ambiguity's carry is a coupling
            // row inside A_s/A_r (not a carry_cols diagonal prior), so withholding it
            // means zeroing that row before re-adding the block: the coupling-row
            // analogue of the diagonal-path prior withhold below.  The refloated
            // ambiguity keeps its full-weight data rows, so its local block stays PD.
            // The edit is made on a copy, so the primary R (and any later replay) is
            // unchanged (card 1).
            if (carry_cpl_cond && carry_cpl_joint && !R.cpl_raw.empty()
                && !refloat_cols.empty() && R.cpl_row_start >= 0) {
                // The joint whitening mixes a station's coupling rows, so a refloat
                // cannot zero individual output rows: re-whiten the surviving rows
                // (dropping the refloated locals) and append them after the unchanged
                // data rows.  A re-whiten failure drops all coupling rows for the epoch,
                // a safe degradation since the refloated arcs float from data anyway
                // (card 1b).
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
                // Local leveling block (F3, network.phase_leveling_local_block): the
                // whitening mixes the block's rows (each output row is a row of L⁻¹ over
                // all block columns), so a refloated column cannot be withheld by zeroing
                // its own row.  The refloated columns are marginalized (dropped from Σ)
                // and the survivors re-whitened, as in the card-1b re-whiten above but on
                // the local block.  Survivors land in their own row slots; the dropped
                // columns' slots stay zero (the scalar loop below zeroes them again,
                // which is consistent because cpl_row_local_cols carries each block row's
                // primary column).  A non-SPD re-whiten drops the whole block for this
                // re-solve, a safe degradation since the refloated arcs float from data
                // anyway.  Both outcomes are counted: this is invariant #7's re-coupling
                // edge for the block.  An empty pl_blk (knob off or no block) leaves the
                // rows untouched (byte-identical).
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
            // Re-inject the pivot's ZWD carry as well (its R.carry_cols is empty unless
            // network.carry_zwd), so the within-epoch re-solve matches the primary
            // solve (B9).  refloat_cols names only ambiguity columns, so a pivot's lone
            // ZWD column is never in it, and the pivot takes the fast path.
            // The gate follows the primary solve's install site in
            // ApplicationNetworkAssembly, `(carry_warm || rx_cb_on || rx_cb_datum ||
            // R.opin_rows > 0) && !R.carry_cols.empty()`, so the re-solve folds the
            // priors the primary one did; this copy lacks the oracle-pin disjunct
            // `R.opin_rows > 0`.  Today both reduce to `!R.carry_cols.empty()`: this
            // lambda runs inside `if (within_epoch_armed && carry_warm)`, so carry_warm
            // is true here and the other disjuncts cannot change the predicate.  They
            // are written out because decoupling the within-epoch tier from carry_warm
            // is a sanctioned open fix (CLAUDE.md, standard armed set); once that
            // lands, a bare `carry_warm` here would drop the rx-CB carry and
            // RXCB-DATUM rows that the primary solve folded (and, without its
            // disjunct, the oracle-pin rows), and the replay would disagree with the
            // solve it replays.
            if ((carry_warm || ctx.knobs.rx_cb_on || ctx.knobs.rx_cb_datum)
                && !R.carry_cols.empty()) {
                if (refloat_cols.empty()) {
                    est.addStationPrior(static_cast<std::size_t>(est_index_of[i]),
                                        R.carry_cols, R.carry_dNrr, R.carry_dur);
                } else {
                    // Inject the carry prior for every carried ambiguity except the
                    // refloated ones (those are left free and re-estimated from data).
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
        // Re-apply the constraint-realized clock gauge rows (sum-to-zero or
        // broadcast-anchored) on the fresh arrowhead.  plan is in scope, and its rows
        // are frozen with the layout for the epoch, so they match the primary solve.
        // In pin mode there are 0 rows and this is a no-op (byte-identical).
        if (plan.clock_constraint_H.rows() > 0)
            est.addSharedConstraintRows(plan.clock_constraint_H, plan.clock_constraint_z,
                                        plan.clock_constraint_weight);
        // SBC completion rows, re-applied unchanged (EpochWorkspace writer-site
        // contract) so the replayed solve carries the same completed datum; the
        // weight is the compile-time constant, so no weight travels in the capture.
        if (cap_sbc_H.rows() > 0)
            est.addSharedConstraintRows(cap_sbc_H, cap_sbc_z,
                                        NetworkSbasis::kSbasisCompleteWeight);
        // The replay must fold the same prior the primary solve folded.  With the
        // gauge-free downdate armed, the plain accumulator would leave information
        // along the datum gauge that the primary solve removed, and the within-epoch
        // RAIM trip decision would be taken against a different system from the one
        // under test.  The within-epoch tier is in the standard armed set, so this
        // applies whenever the downdate is armed with the carry (card C-GF).
        if (!cap_priors.empty()) {
            if (ctx.knobs.carry_gauge_free)
                est.addSharedPriorClockGaugeFree(cap_priors,
                                                 ctx.knobs.carry_gauge_free_closure);
            else
                est.addSharedPrior(cap_priors);
        }
        // Replay the dense D2 fold on the fresh arrowhead (C1).  The layout is frozen
        // for the epoch, so the key→column mapping and the LDLT are bit-reproducible
        // and the re-solve sees the identical shared system.  The capture is
        // non-empty only when the primary dense fold succeeded (its fallback went
        // through cap_priors above), so this cannot throw where the primary did not.
        if (!cap_dense_keys.empty())
            est.addSharedPriorDense(cap_dense_keys, cap_dense_cov, nullptr,
                                    ctx.knobs.carry_gauge_free,
                                    ctx.knobs.carry_gauge_free_closure);   // same fold as the primary solve (card C-GF)
        // Rebuild the ensemble phase-bias gauges on the fresh arrowhead (fix A):
        // beginEpoch reopened the layout, est_index_of was rebuilt above and
        // excluded stations may have dropped out, so the stored gauges' station
        // indices are stale.  No-op when off.
        buildEnsembleGauges();
        // Rebuild the DD-prior blocks on the fresh arrowhead (beginEpoch cleared
        // them, and est_index_of was rebuilt above).  The predict is not re-run;
        // only the fold is rebuilt.  No-op when off (card 4 (M1), C3b).
        buildCarryDdPriors();
        return est.solve(plan.shared_datum);
    };

    double sigma0 = 1.0;
    std::map<std::string, double> sat_eff;
    std::map<int, double> sta_eff;
    std::map<std::pair<int, std::string>, double> arc_eff;
    assess(x_s, sigma0, sat_eff, sta_eff, arc_eff);
    const double sigma0_first = sigma0;
    // Keep the pre-adaptation σ̂₀ for the epoch-status row (R0): it is the null sample
    // that queue item 4 needs on disk, and it is recorded on quiet epochs as well as
    // on those with a trip event.
    ctx.carry.ep_sigma0_first = sigma0_first;
    // The primary solve's satellite clocks (R0, §5.07.2 impulse-channel meter),
    // captured before any within-epoch re-solve can move them.  The shared layout
    // is frozen for the epoch, so clockColumn stays valid across re-solves.
    std::map<std::string, double> we_clk_first;
    for (const std::string& s : registered_clock_sats) {
        try {
            const Eigen::Index c = est.clockColumn(s);   // throws if absent
            if (c >= 0 && c < x_s.size()) we_clk_first[s] = x_s(c);
        } catch (...) { /* no clock column: not in this epoch's layout */ }
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
        // Best new culprit per granularity (all gated on effect/σ̂0 > raim_k).  An
        // isolated arc is the finest and least destructive removal; a whole station is
        // the coarsest (it discards good arcs too) and is de-rated by sta_bias, so it
        // is chosen only when it dominates the best satellite or arc by that margin.
        // On an equal score the preference is arc, then satellite, then station
        // (surgical-first tie-break).
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
        // gate is a network-wide problem (its shared clock or phase bias is corrupted),
        // not a stack of isolated arcs, so reinitialize the whole satellite in one
        // iteration (one re-solve) rather than arc by arc.  When the best arc belongs
        // to such a satellite, redirect to the satellite branch.  Reuses the RAIM
        // cross-station consensus count (§ post-fit RAIM).
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
            // WE-CI culprits, when the phase effects have nothing left to remove: the
            // worst station by its maximum code/iono row against the worst satellite
            // by its network-wide maximum code row, both gated on raim_k against the
            // larger of the two pools' σ̂0.  A station culprit is excluded (the
            // response to a fault concentrated at one station); a satellite culprit
            // has its code rows de-weighted (a fallback rather than a drop).
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
            // Not expected to be reachable (the per-station full-weight retry uses the
            // known-PD original assembly).  Last resort: restore the clean no-drop
            // solve so the emit / harvest run on a consistent estimator rather than a
            // half-rebuilt one (keeping the prior solve would leave est mid-rebuild).
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
    // The settled clock displacement of this epoch's re-solves, (final − primary) per
    // satellite: the §5.07.2 impulse-channel meter (R0).  Identically 0 when no
    // re-solve ran (x_s is unchanged), so only adapted epochs pay for the comparison
    // loop.
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
            // One trace row per satellite the tier moved measurably (> 1 mm): the
            // per-identity channel.  The epoch maximum goes to the status row.
            if (events_trace.is_open() && std::abs(dv) > 1e-3)
                emitEvent("T2_within_epoch", "we_clock_move", s, "", dv,
                          std::nullopt, "measure",
                          fmt::format("{:.4f}->{:.4f}", v0, v1));
        }
        // Run-end counters for the three response classes, and the run-maximum clock
        // move with its identity (R0).
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
        // One event per settled culprit (whole-satellite drop, per-(sta, sat) arc drop,
        // excluded station, WE-CI code de-weight): statistic = final σ̂0,
        // threshold = trigger, detail = first->final (T2).
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

    // A within-epoch satellite refloat reinitialized that satellite's ambiguity at
    // every station; its coupled shared clock is just as suspect, so refloat the
    // carried clock as well (the stash below skips it).  Opt-in
    // (clock_monitor_couple_raim).
    if (clk_mon_couple)
        for (const auto& s : drop_sats) clock_refloat_sats.insert(s);
    // A within-epoch whole-satellite drop means that satellite's emitted correction
    // is contaminated: feed the retro-bump regardless of the (numeric) couple opt-in
    // (P0-2).
    if (retro_feed)
        for (const auto& s : drop_sats) retro_flag_sats.insert(s);

    // Keep the tier's per-epoch observability for the epoch-status row (R0).  The
    // refloat count is the tier's own settled whole-satellite set (es.n_refloat_sats
    // reads the clock-couple set, which is empty unless clock_monitor_couple_raim;
    // §5.07.4 defect 5).  The clock move is 0.0 on an assessed but quiet epoch,
    // which is a real measurement, not an absence.
    ctx.carry.ep_we_refloat_sats = static_cast<int>(drop_sats.size());
    ctx.carry.ep_we_clk_move_max = std::abs(we_clk_move_max);
    ctx.carry.ep_we_clk_move_sat = we_clk_move_sat;

    sigma0_adapt = sigma0;   // publish the final within-epoch σ̂0 for the withhold gate (A4)
}
// drop_sats / drop_arcs leave scope with the RAIM block, so retire the aliases and the
// next epoch's primary buildCarryDdPriors cannot dereference a dangling set (see C3c
// §3 at the alias site).
carry_we_drop_sats = nullptr;
carry_we_drop_arcs = nullptr;

// Reduce-once invariant (design §3.5): the carried prior is injected before the gate,
// so the single gate reduce() must be the only per-station Schur recompute this
// epoch, unless the within-epoch adaptation re-solved (each drop and re-solve is
// another reduce()).  Otherwise a second recompute means that a stray post-gate
// local-block change doubled the serial path without notice.
if (pl_on && !we_adapt_attempted && est.reduceRecomputeCount() != 1) {
    log->error("Network[phase-leveling]: reduce() recomputed {} times this epoch "
               "(expected exactly 1) — a stray post-gate local-block change doubled "
               "the serial Schur path.", est.reduceRecomputeCount());
}

// --- Within-epoch escape hatch: withhold rather than emit a still-contaminated epoch
// The capped adaptation (drop a culprit and re-solve, up to kMaxAdapt passes) may not
// fully isolate the fault; if σ̂0 still exceeds the withhold ceiling, the shared
// solution is too corrupted to publish.  Reuse the observability-gate disposition
// (no emit, no carry stash, epoch skipped, so the contaminated solve never enters
// the store or the carry) instead of emitting it with only a log warning.  With the
// ceiling off (raim_withhold ≤ 0) the epoch emits as usual (byte-identical).
//
// Gate on σ̂0 alone, not on we_adapt_attempted (P0-4).  A diffuse contamination that
// leaves no single dominant culprit (kind == Kind::None) never launches a re-solve,
// so we_adapt_attempted stays false while σ̂0 is high, which is the case the withhold
// ceiling is meant to catch.  sigma0_adapt is set to the assessed σ̂0 whenever the
// within-epoch RAIM block runs (whether or not a culprit was dropped) and stays 0
// otherwise, so an epoch without the tier (sigma0_adapt == 0) cannot trip it.
// Per-constellation carve-out of the σ̂0 withhold (RTA-10,
// network.per_const_withhold).  When the ceiling trips, judge each constellation by
// its own σ̂0_c:
//
//   σ̂0_c = max(raim_s0_floor, 1.4826 · median{ r_i : i ∈ constellation c's rows })
//
// over the same rows the global σ̂0 pools (the carried-ambiguity phase rows of every
// present station, the pivot excluded unless the phase datum is prioritized;
// r_i = |v_i|·√w_i, divided by √(redundancy r̄_i) under B2 with its uncheckable-row
// exclusion), partitioned by the ambiguity's satellite constellation.  Same
// statistic, same floor (raim_s0_floor, default 1.0), same ceiling, restricted
// support.
//
// Approximation: the residuals are evaluated at the joint solution.  A contamination
// in constellation A perturbs shared per-station nuisances (receiver clock, ZWD,
// iono) that leak into B's residuals, and each row's B2 redundancy is its share of
// the joint system's redundancy, not of a per-constellation solve.  σ̂0_c therefore
// under-separates cross-constellation coupling.  The outcome "every constellation
// individually clean while the global σ̂0 tripped" marks the limit of that
// approximation; it is published with a warning and counted
// (n_perconst_diffuse_published).  A constellation also needs at least
// per_const_withhold_min_rows rows to be judged: fewer rows cannot establish that it
// is clean in a contaminated epoch.
//
// The rank / observability gate above keeps its whole-epoch semantics: a rank
// deficiency means the declared datum itself is invalid and the product would be an
// arbitrary least-norm split, so there is no sound subset to carve out.  The
// carve-out applies only to the σ̂0 (contamination) withhold.
bool           perconst_active = false;   // a publishable subset exists this epoch
std::set<char> perconst_publish;          // constellations to keep in the emission
std::set<char> perconst_dec_withhold;     // constellations the decision withheld
std::map<char, NetworkEstimator::PerConstSigma0> perconst_stats;  // σ̂0_c + rows (events/log)
if (perconst_on && raim_withhold > 0.0 && sigma0_adapt > raim_withhold) {
    for (auto& [cst, pool] : we_sigma0_pool_c) {
        NetworkEstimator::PerConstSigma0 st;
        st.rows = static_cast<int>(pool.size());
        if (!pool.empty()) {   // same median × 1.4826 and floor as the global σ̂0
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
    // An empty dec.publish means no constellation is individually publishable: fall
    // through to the whole-epoch withhold below, as without the toggle.
}
// Flight-recorder trigger (INC-5, docs/SPEC_prefit_census_observability.md §7).
// Evaluated on every epoch that reaches this decision, whichever way it goes:
// passing `false` clears the era and re-arms the latch, and the consultation count
// shows that the site is wired (invariant #4).  The recorder's A4 era is the ceiling
// breach, not the withhold action, so the per-constellation carve-out does not
// suppress it: the epoch is still contaminated.
const bool a4_over_ceiling = raim_withhold > 0.0 && sigma0_adapt > raim_withhold;
networkRhsTripEdge(ctx, "sigma0", a4_over_ceiling);
// --- Withhold-era bookkeeping for the A4 withhold (SPEC_withhold_era_release §3;
// both edges, per invariant #7).  Gated on the policy knobs, so with them off no
// events are emitted and every counter stays at zero (network_events.csv is in the
// byte-cert set).  The era here is the withhold action (a carve-out-published epoch
// exits it, the declared RTA-10 limitation); the recorder line above follows the
// ceiling breach, a different edge on purpose, kept distinct by the n_wh_* names.
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
            // solve_failed / head-skip) never reached an exiting decision: close it,
            // marked as interrupted, before opening the new one (C7).
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
    ++n_sigma0_withheld;   // dedicated count of σ̂0-ceiling withholds, reported in the summary (R8)
    if (trace) trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow)
                     << ',' << n_present << ',' << n_clock << ',' << n_pb
                     << ',' << plan.declared_dimension << ",1,0,0,0," << total_pins << '\n';
    { EpochStatusRow es; es.sigma0 = sigma0_adapt; es.n_adapt = n_adapt;
      es.n_present = n_present; es.n_clock_sats = n_clock;
      es.datum_changed = datum_changed ? 1 : 0;
      // A withheld epoch is where the tier's first-assess σ̂₀ and the impulse
      // meter matter most, so carry them onto the disposition row (R0).
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
    // Product-level heartbeat for the withheld epoch (an H line with no E line), so a
    // live tail can tell "σ̂0 withheld" from "producer died" (RTA-10).  Before the
    // deferred product writer opens (at the first emit) the heartbeat is buffered in
    // setup.pending_h_pre_open and flushed at the open.  withheld="*" means the whole
    // epoch.
    if (emit_epoch_status) {
        NetworkProducerStatus hst;
        hst.time.week = se.time.week;
        hst.time.tow  = se.time.tow;
        hst.state     = NetworkProducerStatus::State::WithheldSigma0;
        // One statistic per wire: when the census feed is armed, every H site
        // carries it, because a consumer thresholding field 5 needs one population.
        // The withhold reason is already in the state token, and the A4 ceiling
        // above still reads sigma0_adapt (H-rider).
        hst.sigma0    = ctx.knobs.h_sigma0_census ? ctx.carry.ep_h_sigma0_census
                                                  : sigma0_adapt;
        hst.withheld  = "*";
        if (product_writer) {
            product_writer->writeEpochStatus(hst);
            if (nc.product_flush_per_epoch) product_writer->flush();
        } else if (setup.contract_pending) {
            // Buffer only while a v2 writer open is still pending; with no
            // product path (or a v1 immediate open) there is nothing to flush into.
            setup.pending_h_pre_open.push_back(hst);
        }
    }
    // This epoch returns SkipEpoch (the driver's `continue`), so it never reaches the
    // SB_FRAMEDIAG consumer.  The recorder has to fire here, or the A4 trigger would
    // never dump the epoch that tripped it (only the next one, which is a different
    // system).  The kernel and the captures still belong to this epoch, so the dump
    // is exact; the same call also keeps this epoch as the next trip's "before" frame
    // (INC-5).
    networkRhsAttribTrip(ctx, config_, log, traces, se, setup.stations, est,
                         plan.shared_datum, asm_res, est_index_of);
    // Per SPEC_withhold_era_release §3(b), make the free-epoch response survive the
    // discarded epoch.  Detection ran before the solve on raw observables (a verdict
    // independent of the withhold); erasing the freed entries makes the missing carry
    // the free re-level at the next accepted epoch (assembly injects no carry for a
    // key with no local_carry entry).  Reset class only: no value from this
    // contaminated solve is stored.  Bridged bands were already removed from the
    // latch, so a delta-bridged carry cannot be erased here; the pending-reseed mark
    // was consumed in PASS-B, so the erase is the half that takes effect.  Touches
    // nothing that the epoch-status, H and recorder writes above read (G5-safe
    // placement).
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


    // --- SH-field health gate verdict (SPEC_day129_frame_guards Phase 2.2) -----
    // After the solve and before emission (this is the only pre-emit seam holding
    // x_s), on epochs that passed the rank gate and the A4 σ̂0 withhold above.  The
    // verdict affects emission only: NetworkSolPrep and NetworkEmitStore return
    // early on ctx.carry.sh_gate_engaged, while the carry stash and every QC tier
    // still run, because an out-of-band era lasting hours must not expire the carry
    // as the A4 skip-epoch semantics would.  Out-of-band epochs are counted, not
    // warned (the throttled T1a warning under-counts them), and every engage and
    // release transition is logged and evented without throttling.
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
        // Dual-layer: the physical band bounds the total mean VTEC (layer sum), as
        // the T1a warning does.
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
            // Flight-recorder trigger for the engage era (INC-5).  Called on every
            // evaluated epoch (false clears the era); it must precede the engage
            // block below so that the SB_FRAMEDIAG consumer, which runs later in the
            // same epoch, sees the pending trip.
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
                // Visibility during a long engaged era: a periodic line with the
                // counts every 20 epochs (10 min at 30 s).  The era's full extent is
                // in the per-epoch counters and events in any case.
                log->warn("Network (SH gate): still engaged at TOW {:.1f} — {} consecutive "
                          "out-of-band epoch(s), a00 {:.1f} TECU, rms_all {:.2f}; {} "
                          "emission(s) withheld so far.",
                          se.time.tow, ctx.carry.sh_gate_oob_run, a00_total, rms_all,
                          ctx.counters.n_sh_gate_withheld);
            }
            ctx.carry.sh_gate_engaged = engage;
        }
        // coeff_ok false means a column lookup failed (the field is not in this
        // epoch's solve): leave the gate state unchanged rather than report a
        // release that did not happen; the next computable epoch re-evaluates.
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

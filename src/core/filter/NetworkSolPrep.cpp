/**
 * @file NetworkSolPrep.cpp
 * @brief Implementation of @ref networkSolPrep — the SB_SOLPREP emit-preparation
 *        block relocated VERBATIM (dedent-only, zero substitution) out of
 *        Application::runNetwork at decomposition INC-11pre (2026-07-22).
 *
 * The body below is a byte-copy of the former SB_SOLPREP block
 * (ApplicationNetwork.cpp lines 5034-5770), dedented 4 spaces (loop-body indent, no
 * wrapping if).  The alias preamble is derived mechanically from the run-TU alias
 * block; the emitEvent forwarder is reconstructed.  The five crossing locals stay
 * declared in the body and are moved into NetworkSolPrepResult by the return
 * epilogue.  See NetworkSolPrep.hpp for the ordering contract.
 */

#include "filter/NetworkSolPrep.hpp"

#include "filter/NetworkRhsAttrib.hpp"   // INC-5 flight-recorder trip latch

#include "app/NetworkRunContext.hpp"            // ctx.* aliases
#include "config/GnssConfig.hpp"                // config_ getters + getNetworkConfig
#include "logging/NetworkRunTraces.hpp"         // events_trace + emitEvent
#include "math/TimeUtils.hpp"                   // TimeUtils::nowGpsTime
#include "products/CombinedCorrectionSigma.hpp" // computeCombinedCorrectionSigma / selectCombinedCodeRef
#include "products/LiveEphemerisStore.hpp"      // brdc_rts clock-iode stamp
#include "products/NetworkProductIO.hpp"        // NetworkProductWriter
#include "products/NetworkStateSnapshot.hpp"    // networkWarmupRampLifted

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

NetworkSolPrepResult networkSolPrep(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    const NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    double pl_now_s,
    bool datum_changed,
    const std::set<char>& changed_consts,
    double sigma0_adapt,
    bool perconst_active,
    const std::set<char>& perconst_publish,
    const std::set<char>& perconst_dec_withhold,
    const std::map<char, NetworkEstimator::PerConstSigma0>& perconst_stats) {
    // --- Alias preamble: mechanically-derived byte-copies of the former
    //     Application::runNetwork alias block + the reconstructed emitEvent
    //     forwarder, so the relocated SB_SOLPREP body reads the original names. --
    const auto& nc = config_.getNetworkConfig();
    const bool brdc_rts = setup.brdc_rts;
    NetworkProductEmitter& emitter = *setup.emitter;
    auto& product_writer = setup.product_writer;
    auto& prod_path = setup.prod_path;
    auto& pending_contract = setup.pending_contract;
    auto& contract_pending = setup.contract_pending;
    auto& events_trace         = traces.events_trace;
    auto& prev_pivot = ctx.carry.prev_pivot;
    auto& c1_zoh_active = ctx.carry.c1_zoh_active;
    auto& last_pivot = ctx.carry.last_pivot;
    auto& this_epoch_if_pin_sel = ctx.carry.this_epoch_if_pin_sel;
    auto& last_if_pin_sel = ctx.carry.last_if_pin_sel;
    auto& this_epoch_anchor = ctx.carry.this_epoch_anchor;
    auto& emitted_anchor = ctx.carry.emitted_anchor;
    auto& eq34_datum_rx = ctx.carry.eq34_datum_rx;
    auto& eq34_emitted_pdr = ctx.carry.eq34_emitted_pdr;
    auto& clock_osc = ctx.carry.clock_osc;
    auto& clock_rate_est = ctx.carry.clock_rate_est;
    auto& eph_cutover_disc_pending = ctx.carry.eph_cutover_disc_pending;
    auto& basis_relatch_disc_pending = ctx.carry.basis_relatch_disc_pending;
    auto& this_epoch_cb_reseed = ctx.carry.this_epoch_cb_reseed;
    auto& cb_reseed_disc_pending = ctx.carry.cb_reseed_disc_pending;
    auto& ura_sigma0_ewma = ctx.carry.ura_sigma0_ewma;
    auto& ura_refloat_horizon = ctx.carry.ura_refloat_horizon;
    auto& ura_retro_sats = ctx.carry.ura_retro_sats;
    auto& ura_ewma_updates = ctx.carry.ura_ewma_updates;
    auto& ramp_lifted = ctx.carry.ramp_lifted;
    auto& first_emit_content_s = ctx.carry.first_emit_content_s;
    auto& grade_metric_s = ctx.carry.grade_metric_s;
    const bool   pl_on         = ctx.knobs.pl_on;
    const int    pl_warmup     = ctx.knobs.pl_warmup;
    const bool   ura_on        = ctx.knobs.ura_on;                  // B1: emit per-sat URA quality channel
    const bool   ura_retro     = ctx.knobs.ura_retro;             // B1: retro-bump flagged SATELLITE(s) on next emit
    const bool   mark_refsat   = ctx.knobs.mark_refsat_reset_disc; // Edge-1 piece 2: mark ref-path slips as per-sat disc
    const double ura_refloat_inflate = ctx.knobs.ura_refloat_inflate; // B1: URA multiplier post-refloat
    const bool   raim_on       = ctx.knobs.raim_on;            // post-fit network RAIM (Increment 3, T2)
    const double raim_withhold = ctx.knobs.raim_withhold; // A4: >0 withholds (not emits) an epoch whose post-adaptation σ̂0 exceeds this ceiling; 0 = off
    const bool   emit_clock_disc = ctx.knobs.emit_clock_disc;            // A2: clock disc channel + per-constellation bump scoping
    const bool   emit_gen_time   = ctx.knobs.emit_gen_time;       // RT0-4: stamp wall-clock time-of-generation per emitted epoch
    const bool   emit_gen_id     = ctx.knobs.emit_gen_id;         // RTA-3: stamp a per-run solution-generation id (first-emit epoch time)
    const bool   clk_runaway_on  = ctx.knobs.clk_runaway_on;        // RT0-5: per-constellation clock-runaway quarantine
    const double clk_runaway_mag = ctx.knobs.clk_runaway_mag;         // RT0-5: median |clock−global median| ceiling (m)
    const double clk_runaway_rate= ctx.knobs.clk_runaway_rate;      // RT0-5: median |drift| ceiling (m/s)
    const int    perconst_min_rows = ctx.knobs.perconst_min_rows;   // RTA-10: min σ̂0-pool rows for a constellation to be judgeable
    const bool   clk_model_on   = ctx.knobs.clk_model_on;
    const double clk_model_max_gap = ctx.knobs.clk_model_max_gap;  // R11: clock-model carry reseed / drift-reset / C1-ZOH bound (defaults to pl_max_gap_s)
    const bool   emit_clock_rate_on = ctx.knobs.emit_clock_rate_on;
    auto& iod = ctx.counters.iod;
    auto& generation_id = ctx.counters.generation_id;
    auto& last_contract_emit_s = ctx.counters.last_contract_emit_s;
    auto& n_emitted = ctx.counters.n_emitted;
    auto& n_runaway_epochs = ctx.counters.n_runaway_epochs;
    auto& n_perconst_partial_epochs = ctx.counters.n_perconst_partial_epochs;
    auto& n_perconst_diffuse_published = ctx.counters.n_perconst_diffuse_published;
    const bool phase_datum_ensemble    = ctx.knobs.phase_datum_ensemble;
    const bool phase_datum_eq34        = ctx.knobs.phase_datum_eq34;
    const bool anchor_hygiene_on       = ctx.knobs.anchor_hygiene_on;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;

    auto emitEvent = [&](const char* tier, const char* event,
                         const std::string& sat, const std::string& station,
                         std::optional<double> statistic, std::optional<double> threshold,
                         const char* action, const std::string& detail) {
        traces.emitEvent(se.time, tier, event, sat, station, statistic, threshold,
                         action, detail);
    };

    // SPEC_day129_frame_guards Phase 2.2: an ENGAGED SH health gate withholds
    // this epoch's EMISSION — every step below is emit machinery (solution
    // serialization, disc-bump present/pending landing, pivot/anchor baseline
    // advance, URA/ramp stamps), all of which the existing whole-epoch withhold
    // paths skip by `continue`-ing before this seam.  Early-return replicates
    // exactly that: pending disc bumps stay QUEUED and land with the next
    // ACTUAL emit (the Card-2 value-step/signal coincidence rule), and the
    // emit-only baselines (last_pivot / emitted_anchor / last_if_pin_sel /
    // cb_reseed merge) stay at the last emitted product.  NetworkEmitStore
    // early-returns on the same flag (it writes the withheld H heartbeat +
    // counter there).  Solve/carry/QC upstream and downstream are untouched.
    if (nc.sh_health_gate && ctx.carry.sh_gate_engaged)
        return NetworkSolPrepResult{};

    // --- Emit -----------------------------------------------------------
    NetworkSolutionEpoch sol = est.toSolutionEpoch(x_s, se.time, iod++,
                                                   /*provider_id=*/0, /*solution_id=*/0,
                                                   /*ambiguity_datum_ensemble=*/phase_datum_ensemble);
    // Card 5 (RTF-2): stamp the broadcast-ephemeris VERSION the producer's clock-C0
    // baseline used, per emitted satellite, so the consumer applies C0 against the
    // SAME broadcast clock (getSatelliteClockCorrection pinned to clock_iode, Card 7)
    // and the eph-cutover re-level (Card 6) can detect a version roll.  The broadcast
    // clock is station-independent (one shared LiveEphemerisStore), so the version the
    // per-station emission path (computeEmissionState) selected this epoch is exactly
    // what computeSatelliteClock reports here — toe granularity (hours) dwarfs the
    // intra-epoch light-travel spread, so pick() lands on the same record.  Querying
    // the store directly at the emit site is a deliberate, equivalent simplification of
    // the plan's "thread the IODE out of the emission path" (no per-station out-param
    // plumbing needed).  GLONASS ('R') stays -1 (plan D8).  sp3 mode: the store is empty
    // ⇒ nullopt ⇒ clock_iode stays -1 everywhere ⇒ Card-4 writes no iode= token ⇒
    // byte-identical.
    if (brdc_rts) {
        auto& eph_store = LiveEphemerisStore::getInstance();
        for (auto& s : sol.sats) {
            if (!s.has_clock || s.sat.empty() || s.sat[0] == 'R') continue;
            if (auto bc = eph_store.computeSatelliteClock(s.sat, se.time))
                s.clock_iode = bc->iode;
        }
    }
    // RT0-4: stamp the wall-clock TIME-OF-GENERATION (distinct from the content
    // epoch se.time) so a consumer can measure product latency.  Only when armed
    // — otherwise generation_time stays {0,0} and no G record is serialized
    // (byte-identical).  Non-deterministic (wall clock), hence gated.
    if (emit_gen_time) sol.generation_time = TimeUtils::nowGpsTime();
    // RTA-3: stamp the per-run solution-generation id, minted once from the
    // FIRST EMITTED epoch's GPS time (week·604800 + ⌊tow⌋).  Deterministic under
    // file replay (the id is serialized per epoch as an I record and comes back
    // out of the file); a restarted live producer first-emits at a later epoch
    // and therefore mints a DIFFERENT id — the consumer treats the change as a
    // global datum break.  0 is reserved for "unset", so an armed run clamps to
    // 1 in the (never-in-practice) week-0 corner.  OFF ⇒ sol.generation_id stays
    // 0 ⇒ no I record, store generation unset (byte-identical).
    // RTB-7 seam: an RTB-7(a) WARM RESUME restores the previous run's non-zero
    // id BEFORE the epoch loop, so the generation_id == 0 guard below suppresses
    // this mint — re-minting on a warm resume would signal a global datum break
    // and force a spurious consumer-wide hard reset.  Only a genuine cold start
    // (no/stale/mismatched snapshot) reaches the mint.
    if (emit_gen_id) {
        if (generation_id == 0) {
            generation_id = static_cast<uint32_t>(se.time.week) * 604800u
                          + static_cast<uint32_t>(se.time.tow);
            if (generation_id == 0) generation_id = 1;
            log->info("Network (RTA-3): solution-generation id {} minted from the first "
                      "emitted epoch (week {}, tow {:.1f}).",
                      generation_id, se.time.week, se.time.tow);
        }
        sol.generation_id = generation_id;
    }
    // B3b: publish the two-state clock-model drift as C1 (consumer extrapolates
    // c0 + c1·dt).  clock_osc[sat].x1 is the drift carried INTO this epoch (from the
    // previous epoch's posterior sequence — the correct extrapolation basis; the
    // clock-model update for THIS epoch runs after the emit).  0 when clock_model off.
    if (clk_model_on)
        for (auto& s : sol.sats) {
            const auto oit = clock_osc.find(s.sat);
            if (oit == clock_osc.end() || !oit->second.init) continue;
            // R6: a satellite that vanished (eclipse) and returns still carries its PRE-GAP
            // drift here — the clock-model update that reseeds x1 to 0 runs after this emit
            // — so shipping oit->second.x1 unconditionally would emit an hours-old rate as C1
            // for one epoch.  Gate on the carried state's freshness against the SAME staleness
            // bound the prediction / drift finite-difference use (R11): past the gap the drift
            // is not trustworthy → leave c1 at 0 (ZOH), matching the reseed the update will do.
            const double sat_dt = (oit->second.t_last > 0.0) ? (pl_now_s - oit->second.t_last)
                                                             : 0.0;
            if (sat_dt >= 0.0 && sat_dt <= clk_model_max_gap) {
                s.clock_c1_m_s = oit->second.x1;
                c1_zoh_active.erase(s.sat);
            } else {
                // T5/R6 (V2 blind spot): a vanish-return past the gap ships c1=0 (ZOH) for one
                // epoch instead of an hours-old rate.  Edge-triggered (c1_zoh_active) so a
                // persistently-stale sat logs ONCE, not every emit.
                if (events_trace.is_open() && c1_zoh_active.insert(s.sat).second)
                    emitEvent("T5_clock_model", "c1_zoh_suppress", s.sat, "", sat_dt,
                              clk_model_max_gap, "c1_zero", "vanish_return");
            }
        }
    // network.emit_clock_rate: same C1 publish, but from the DECOUPLED rate state (no carry
    // prior, no refloat — the clock_model no-go's benefit was ~1× anyway; the asset is the
    // rate, not the carry).  clock_rate_est[sat].x1 is the drift finite-differenced from the
    // PREVIOUS epoch's posterior (its update runs after this emit).  Same R6 staleness ZOH gate.
    else if (emit_clock_rate_on)
        for (auto& s : sol.sats) {
            const auto rit = clock_rate_est.find(s.sat);
            if (rit == clock_rate_est.end() || !rit->second.init) continue;
            const double sat_dt = (rit->second.t_last > 0.0) ? (pl_now_s - rit->second.t_last)
                                                             : 0.0;
            if (sat_dt >= 0.0 && sat_dt <= clk_model_max_gap) {
                s.clock_c1_m_s = rit->second.x1;
                c1_zoh_active.erase(s.sat);
            } else {
                if (events_trace.is_open() && c1_zoh_active.insert(s.sat).second)
                    emitEvent("T5_clock_rate", "c1_zoh_suppress", s.sat, "", sat_dt,
                              clk_model_max_gap, "c1_zero", "vanish_return");
            }
        }
    // RTA-10: constellations withheld from THIS epoch's emission — filled by the
    // RT0-5 runaway quarantine and/or the per-constellation σ̂0 carve-out below;
    // feeds the H heartbeat's status (ok vs partial) and its withheld field.
    std::set<char> epoch_withheld_consts;
    // RT0-5: per-constellation clock-runaway quarantine.  Group this epoch's clock
    // solutions by constellation; if a constellation's median |clock − global median|
    // exceeds the magnitude ceiling (or median |drift| exceeds the rate ceiling), DROP
    // that constellation's satellites from the emission and flag it — a runaway (e.g.
    // the open BeiDou clock divergence) then degrades SCOPE, not the whole product.
    // OFF ⇒ sol untouched (byte-identical).  Placed AFTER the C1 drift-stamp pass so
    // the rate ceiling sees this epoch's clock_c1_m_s (0 when clock_model off ⇒ only
    // the magnitude ceiling applies), and BEFORE the URA pass so a withheld
    // constellation costs no URA work.
    //
    // HAZARD (documented, not defended): the magnitude test is referenced to the
    // GLOBAL median across ALL clock sats.  If the runaway constellation supplies
    // half or more of the epoch's clock satellites, the global median lands inside
    // the runaway cluster and the comparison INVERTS — the healthy minority trips
    // the ceiling and is quarantined while the runaway constellation passes.  The
    // check is sized for its intended regime (a minority constellation, e.g. the
    // open BeiDou divergence, walking off by ~km against a multi-constellation
    // majority); do not arm it in a run dominated by the suspect constellation.
    // The single-constellation degenerate case is warned at config load.
    if (clk_runaway_on && !sol.sats.empty()) {
        std::vector<double> all_c;
        for (const auto& s : sol.sats) if (s.has_clock) all_c.push_back(s.clock_c0_m);
        std::set<char> runaway;
        if (!all_c.empty()) {
            std::nth_element(all_c.begin(), all_c.begin() + all_c.size() / 2, all_c.end());
            const double gmed = all_c[all_c.size() / 2];   // common-mode datum
            std::map<char, std::vector<double>> dev, rate;
            for (const auto& s : sol.sats) {
                if (s.sat.empty() || !s.has_clock) continue;
                dev[s.sat[0]].push_back(std::abs(s.clock_c0_m - gmed));
                if (s.clock_c1_m_s != 0.0) rate[s.sat[0]].push_back(std::abs(s.clock_c1_m_s));
            }
            auto medianOf = [](std::vector<double>& v) {
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                return v[v.size() / 2];
            };
            for (auto& [c, d] : dev) {
                const double dm = medianOf(d);
                double       rm = 0.0;
                bool         bad = dm > clk_runaway_mag;
                if (!bad) {
                    const auto rit = rate.find(c);
                    if (rit != rate.end() && !rit->second.empty()) {
                        rm  = medianOf(rit->second);
                        bad = rm > clk_runaway_rate;
                    }
                }
                if (bad) {
                    runaway.insert(c);
                    const auto n_drop = std::count_if(sol.sats.begin(), sol.sats.end(),
                        [c](const NetworkSatSolution& s){ return !s.sat.empty() && s.sat[0] == c; });
                    log->warn("Network (RT0-5): constellation '{}' clock runaway — median |Δclk|="
                              "{:.1f} m (ceil {:.0f}), median |drift|={:.3f} m/s (ceil {:.2f}); "
                              "WITHHOLDING its {} sats this epoch.",
                              c, dm, clk_runaway_mag, rm, clk_runaway_rate, n_drop);
                    emitEvent("RT0-5_runaway", "constellation_withhold", std::string(1, c), "",
                              dm, clk_runaway_mag, "drop_const", fmt::format("drift={:.3f}", rm));
                }
            }
        }
        // INC-5 flight-recorder trigger (docs/SPEC_prefit_census_observability.md
        // §7): the RUNAWAY era.  Consulted on every epoch the check evaluates —
        // an empty `runaway` clears the era and re-arms the latch, which is what
        // makes the counter's denominator honest.  SB_SOLPREP runs before
        // SB_FRAMEDIAG, so the pending trip is consumed later this same epoch.
        networkRhsTripEdge(ctx, "clk_runaway", !runaway.empty());
        if (!runaway.empty()) {
            std::vector<NetworkSatSolution> kept;
            kept.reserve(sol.sats.size());
            for (auto& s : sol.sats)
                if (s.sat.empty() || runaway.count(s.sat[0]) == 0) kept.push_back(std::move(s));
            sol.sats.swap(kept);
            ++n_runaway_epochs;
            epoch_withheld_consts.insert(runaway.begin(), runaway.end());  // RTA-10: H heartbeat
        }
    }
    // RTA-10 (network.per_const_withhold): apply the per-constellation σ̂0 carve-out
    // decided at the withhold site above — prune the non-publishable constellations'
    // satellites from the emission EXACTLY the way the RT0-5 runaway quarantine does
    // (adjacent code, same swap-prune pattern).  Placed AFTER the C1 drift-stamp pass
    // (unlike RT0-5 nothing here reads clock_c1_m_s — a pruned sat merely had its C1
    // stamped needlessly, which is harmless) and BEFORE the URA pass, so a withheld
    // constellation costs no URA work — the same seam RT0-5 occupies.  A satellite
    // whose constellation is NOT in the publish set is pruned; that includes any
    // constellation ABSENT from the σ̂0 pools entirely (zero rows ⇒ unjudgeable ⇒
    // not publishable under a contaminated epoch).  The downstream emitter seams —
    // presence-aware pending datum bumps, the post-prune retro split, the rise/set
    // code-datum detection — all key on the post-prune sol.sats, so they behave for
    // this prune exactly as they do for the (tested) RT0-5 quarantine.
    if (perconst_active) {
        std::set<char> pruned;
        std::vector<NetworkSatSolution> kept;
        kept.reserve(sol.sats.size());
        for (auto& s : sol.sats) {
            if (!s.sat.empty() && perconst_publish.count(s.sat[0]) == 0)
                pruned.insert(s.sat[0]);
            else
                kept.push_back(std::move(s));
        }
        if (!pruned.empty()) {
            sol.sats.swap(kept);
            ++n_perconst_partial_epochs;
            epoch_withheld_consts.insert(pruned.begin(), pruned.end());
            for (char cst : pruned) {
                const auto sit    = perconst_stats.find(cst);
                const bool judged = sit != perconst_stats.end();
                log->warn("Network (RTA-10): TOW {:.1f} constellation '{}' WITHHELD by the "
                          "per-constellation σ̂0 carve-out — σ̂0_c={} over {} row(s) "
                          "(ceiling {:.2f}, min rows {}); its sats are pruned from this "
                          "epoch's emission.",
                          se.time.tow, cst,
                          judged ? fmt::format("{:.2f}", sit->second.sigma0_c)
                                 : std::string("n/a"),
                          judged ? sit->second.rows : 0, raim_withhold, perconst_min_rows);
                emitEvent("RTA-10_perconst", "constellation_withhold", std::string(1, cst),
                          "",
                          judged ? std::optional<double>(sit->second.sigma0_c) : std::nullopt,
                          raim_withhold, "drop_const",
                          fmt::format("rows={};global_sigma0={:.2f}",
                                      judged ? sit->second.rows : 0, sigma0_adapt));
            }
        } else if (perconst_dec_withhold.empty()) {
            // The honest edge of the per-constellation approximation: the GLOBAL σ̂0
            // exceeded the ceiling, yet EVERY judged constellation individually passed
            // (cross-constellation coupling — shared per-station nuisances — that no
            // per-constellation partial captures).  Publish everything, but say so
            // loudly and count it; a rising count says the carve-out's separability
            // assumption is failing on this network.
            ++n_perconst_diffuse_published;
            log->warn("Network (RTA-10): TOW {:.1f} DIFFUSE contamination published — global "
                      "σ̂0={:.2f} > ceiling {:.2f} while every judged constellation passed "
                      "individually (the per-constellation statistic disagreed with the "
                      "global one).",
                      se.time.tow, sigma0_adapt, raim_withhold);
            emitEvent("RTA-10_perconst", "diffuse_published", "", "", sigma0_adapt,
                      raim_withhold, "publish_all",
                      fmt::format("{} const(s) individually clean", perconst_stats.size()));
        }
        // else: the decision withheld constellation(s) that are not in this epoch's
        // emission anyway (RT0-5 already quarantined them, or they carry no solution
        // sats) — nothing pruned here, and not the diffuse edge either.
    }
    // B1: per-sat URA = EWMA(σ̂0) × formal clock σ (shared-covariance diagonal),
    // skipping datum-pinned clock columns (their variance is the artificial pin,
    // not a physical σ) and inflating a recently-refloated sat for its warmup horizon.
    // Uses the PREVIOUS epoch's σ̂0 EWMA (post-emit QC ordering — a one-epoch lag).
    if (ura_on) {
        // RTB-7(b): evaluate the quality-ramp LIFT rule (counts through the
        // previous epoch, matching the floor's indexing below): n_emitted ≥
        // max(warmup_epochs, phase_leveling_warmup_epochs) AND the σ̂0 EWMA
        // has received ≥ warmup_epochs updates — OR the EWMA is structurally
        // unavailable (the RTB-6 sentinel condition: emit_ura+raim+
        // phase_leveling not all armed; here ura_on holds, so raim_on&&pl_on
        // decides), in which case the epoch horizon alone lifts.  Logs the
        // RTB-7(c) cold-start-to-product-grade metric at the lift.  Guarded
        // by the floor ⇒ OFF (0) is byte-identical.
        if (nc.warmup_ura_floor_m > 0.0 && !ramp_lifted) {
            const bool ewma_structural = raim_on && pl_on;   // && ura_on (this block)
            if (networkWarmupRampLifted(static_cast<long long>(n_emitted),
                                        nc.warmup_epochs, pl_warmup,
                                        static_cast<long long>(ura_ewma_updates),
                                        ewma_structural)) {
                ramp_lifted    = true;
                grade_metric_s = (first_emit_content_s >= 0.0)
                                     ? (pl_now_s - first_emit_content_s)
                                     : 0.0;
                log->info("Network (RTB-7): cold-start-to-product-grade: {:.1f} s / {} "
                          "epochs — warmup URA floor {:.1f} m LIFTED (n_emitted {} >= "
                          "max(warmup_epochs {}, pl_warmup {}); sigma0-EWMA updates {}{}).",
                          grade_metric_s, n_emitted, nc.warmup_ura_floor_m, n_emitted,
                          nc.warmup_epochs, pl_warmup, ura_ewma_updates,
                          ewma_structural ? "" : " — EWMA structurally unavailable, "
                                                 "update count waived");
            }
        }
        const Eigen::MatrixXd& scov = est.sharedCovariance();
        std::set<Eigen::Index> pinned;
        for (const auto& pin : plan.shared_datum) pinned.insert(pin.column);
        const double s0 = std::max(1.0, ura_sigma0_ewma);
        for (auto& s : sol.sats) {
            if (!s.has_clock) continue;
            Eigen::Index col;
            try { col = est.clockColumn(s.sat); } catch (...) { continue; }
            if (pinned.count(col)) continue;   // datum pivot — bogus pin variance, no honest URA
            if (col < 0 || col >= scov.rows() || col >= scov.cols()) continue;
            const double var = scov(col, col);
            if (!(var > 0.0) || !std::isfinite(var)) continue;
            double ura = s0 * std::sqrt(var);
            const auto hit = ura_refloat_horizon.find(s.sat);
            if (ura_refloat_inflate > 1.0 && hit != ura_refloat_horizon.end() && hit->second > 0)
                ura *= ura_refloat_inflate;   // honest surrogate for the post-refloat DIA penalty
            // RTB-7(b): cold-start quality ramp — a freshly (re)started producer
            // advertises DEGRADED URA until the ramp LIFTS (the
            // networkWarmupRampLifted rule evaluated above: emitted-epoch
            // horizon AND σ̂0-EWMA stabilization, not a bare epoch count), so a
            // consumer never trusts a not-yet-converged network solution.  A
            // warm-resumed producer restores ramp_lifted from the snapshot and
            // is not degraded.  Off (floor 0) ⇒ byte-identical.
            if (nc.warmup_ura_floor_m > 0.0 && !ramp_lifted)
                ura = std::max(ura, nc.warmup_ura_floor_m);
            s.clock_ura_m = ura;
            s.has_ura     = true;
        }
    }
    // B1b (defect-2 lever B, Tienstra §6.6): per-(sat,signal) PHASE-BIAS correction
    // σ — the phase-bias block of Qŷ_B1 the clock-only URA channel omits.  The
    // consumer currently applies phase-bias corrections as if EXACT; this transmits
    // their formal 1σ so a wandering bias (the 6–69 cm differential defect-2 driver)
    // is down-weighted instead.  Gauge-exact: a COPY of the shared covariance with
    // the folded ensemble phase-bias gauges nulled (zeroPhaseGaugeColumns) and the
    // datum-pinned columns skipped (their 1/pin_scale is a gauge, not a physical σ) —
    // exactly the URA loop's pin treatment.  Scaled by the SAME σ̂0 EWMA as the URA.
    // Off ⇒ σ stays 0 ⇒ product/store byte-identical.
    if (config_.networkEmitPhaseBiasSigma()) {
        Eigen::MatrixXd        pcov = est.sharedCovariance();   // copy; treat gauges as exact
        est.zeroPhaseGaugeColumns(pcov);
        std::set<Eigen::Index> pb_pinned;
        for (const auto& pin : plan.shared_datum) pb_pinned.insert(pin.column);
        const double        s0 = std::max(1.0, ura_sigma0_ewma);
        std::vector<double> pb_sigmas;   // premise-check diagnostic: distribution of ABSOLUTE emitted σ
        // For the B2-vs-LeverA decision: collect per-signal columns so we can also
        // measure the BETWEEN-SATELLITE-DIFFERENCED σ.  The consumer's rx clock +
        // per-arc float ambiguity absorb the common/level directions of the
        // phase-bias error (TK-2015 Eq 88), so the ABSOLUTE σ (float-ambiguity-
        // entangled level, ~1 m at 0 fixes) overstates what actually reaches the
        // consumer estimate.  The between-satellite SD σ is the honest per-epoch
        // consumer-relevant magnitude.
        std::map<std::string, std::vector<Eigen::Index>> by_signal;
        for (auto& s : sol.sats) {
            for (auto& [signal, npb] : s.phase_bias) {
                Eigen::Index col;
                try { col = est.phaseBiasColumn(s.sat, signal); } catch (...) { continue; }
                if (pb_pinned.count(col)) continue;                 // anchor-pinned → exact, no honest σ
                if (col < 0 || col >= pcov.rows() || col >= pcov.cols()) continue;
                const double var = pcov(col, col);
                if (!(var > 0.0) || !std::isfinite(var)) continue;  // folded gauge (zeroed) or degenerate
                npb.sigma_m = s0 * std::sqrt(var);
                pb_sigmas.push_back(npb.sigma_m);
                by_signal[signal].push_back(col);
            }
        }
        // T3d: under the anchor-free ENSEMBLE datum the absolute σ above references the
        // zeroed receiver-ensemble gauge column (degenerate — the level is not observable).
        // Overwrite each emitted phase-bias σ with the honest BETWEEN-SATELLITE-SD σ per
        // signal (the quantity the consumer's rx-clock + float ambiguity leave observable),
        // referenced to the group's first eligible column; the reference sat itself takes the
        // group-median SD σ (its SD-vs-self is 0, not an honest uncertainty).  Only under
        // phase_datum_ensemble ⇒ byte-identical for the pivot/prioritized datum.
        if (phase_datum_ensemble) {
            // Under the ensemble gauge EVERY satellite phase-bias column is a FOLDED gauge
            // direction, so the zeroed pcov above gives σ=0 for all of them (by_signal is
            // empty).  Compute the honest between-sat SD σ from the RAW covariance instead:
            // the fold's common gauge-direction variance cancels in the between-sat
            // difference, leaving the relative uncertainty.  Reference each signal group to
            // its first sat; the reference sat takes the group-median SD σ (SD-vs-self is 0).
            const Eigen::MatrixXd rawcov = est.sharedCovariance();
            std::map<std::string, std::vector<std::pair<std::string, Eigen::Index>>> grp;
            for (auto& s : sol.sats)
                for (auto& [signal, npb] : s.phase_bias) {
                    (void)npb;
                    Eigen::Index col;
                    try { col = est.phaseBiasColumn(s.sat, signal); } catch (...) { continue; }
                    if (col >= 0 && col < rawcov.rows() && col < rawcov.cols())
                        grp[signal].push_back({ s.sat, col });
                }
            for (auto& [signal, sc] : grp) {
                if (sc.size() < 2) continue;                 // no between-sat difference
                const Eigen::Index ref = sc.front().second;
                std::map<std::string, double> sd_of;         // sat -> SD σ
                std::vector<double> sds;
                for (std::size_t i = 1; i < sc.size(); ++i) {
                    const Eigen::Index c = sc[i].second;
                    const double v = rawcov(c, c) + rawcov(ref, ref) - 2.0 * rawcov(c, ref);
                    if (v > 0.0 && std::isfinite(v)) {
                        const double sd = s0 * std::sqrt(v);
                        sd_of[sc[i].first] = sd;
                        sds.push_back(sd);
                    }
                }
                if (sds.empty()) continue;
                std::sort(sds.begin(), sds.end());
                sd_of[sc.front().first] = sds[sds.size() / 2];   // ref sat: group median
                for (auto& s : sol.sats) {
                    auto it = s.phase_bias.find(signal);
                    if (it == s.phase_bias.end()) continue;
                    auto sit = sd_of.find(s.sat);
                    if (sit != sd_of.end() && sit->second > 0.0)
                        it->second.sigma_m = sit->second;
                }
            }
        }
        // Lever-B premise check + the decisive ratio: ABSOLUTE σ vs between-sat-SD σ.
        if (!pb_sigmas.empty() && static_cast<long long>(n_emitted) % 120 == 0) {
            std::sort(pb_sigmas.begin(), pb_sigmas.end());
            // Between-satellite SD σ per signal, referenced to the first sat in the
            // signal group: sqrt(var_s + var_ref − 2·cov(s,ref)) × σ̂0.  If SD σ ≪
            // ABS σ, the level dominates (consumer-absorbed) ⇒ covariance carry is
            // structurally limited ⇒ the wander is temporal ⇒ pivot to Lever A.
            std::vector<double> sd_sigmas;
            for (auto& [signal, cols] : by_signal) {
                if (cols.size() < 2) continue;
                const Eigen::Index ref = cols.front();
                for (std::size_t i = 1; i < cols.size(); ++i) {
                    const Eigen::Index c = cols[i];
                    const double v = pcov(c, c) + pcov(ref, ref) - 2.0 * pcov(c, ref);
                    if (v > 0.0 && std::isfinite(v)) sd_sigmas.push_back(s0 * std::sqrt(v));
                }
            }
            double sd_lo = -1.0, sd_med = -1.0, sd_hi = -1.0;
            if (!sd_sigmas.empty()) {
                std::sort(sd_sigmas.begin(), sd_sigmas.end());
                sd_lo = sd_sigmas.front();
                sd_med = sd_sigmas[sd_sigmas.size() / 2];
                sd_hi = sd_sigmas.back();
            }
            log->info("Network (B1b): phase-bias σ for {} (sat,signal) — ABS min/med/max = "
                      "{:.3f}/{:.3f}/{:.3f} m | between-sat-SD min/med/max = {:.3f}/{:.3f}/{:.3f} m "
                      "(−1 = <2 sats/signal) (σ̂0 {:.3f}).",
                      pb_sigmas.size(), pb_sigmas.front(), pb_sigmas[pb_sigmas.size() / 2],
                      pb_sigmas.back(), sd_lo, sd_med, sd_hi, s0);
        }
    }
    // Datum bump + serialization, applied HERE (after a successful solve so a
    // withheld/failed epoch never leaks a pending bump).  The A1/A2 clock-pivot
    // handover (this epoch) is a per-CONSTELLATION datum re-reference (E-line
    // field-8); the B1 retro-bump (a post-emit QC tier flagged these SATELLITES
    // last epoch) is a per-SATELLITE discontinuity (R record).  Both must
    // reproduce on replay from the serialized flags.  A GLOBAL datum bump
    // (emit_clock_disc off) serializes an EMPTY constellation set so replay
    // re-applies the global bump.
    bool           global_bump = false;
    std::set<char> serialized_consts;
    if (datum_changed) {
        if (emit_clock_disc) {
            emitter.markDatumChange(changed_consts);
            serialized_consts.insert(changed_consts.begin(), changed_consts.end());
        } else {
            emitter.markDatumChange();   // global (pre-A2 byte-identical)
            global_bump = true;
        }
        // PIVOT (V1/V2): the datum re-reference is otherwise UNLOGGED.  One row per
        // changed constellation, sat=new pivot, detail=<const>:old->new + arm.  Arm is
        // derived here (pivotHandoverSignalled does not return it): prev_pivot still holds
        // the key ⇒ adjacent handover; else ⇒ vanish_return (old pivot from last_pivot).
        // Emitted BEFORE prev_pivot advances below.
        if (events_trace.is_open()) {
            for (char c : changed_consts) {
                const std::string key(1, c);
                auto np = plan.clock_pivot.find(key);
                const std::string new_p = (np != plan.clock_pivot.end()) ? np->second
                                                                         : std::string();
                std::string old_p;
                const char* arm;
                auto pp = prev_pivot.find(key);
                if (pp != prev_pivot.end()) { old_p = pp->second; arm = "adjacent"; }
                else {
                    auto lp = last_pivot.find(key);
                    old_p = (lp != last_pivot.end()) ? lp->second : std::string("?");
                    arm = "vanish_return";
                }
                emitEvent("PIVOT", "handover", new_p, "", std::nullopt, std::nullopt,
                          "datum_bump", fmt::format("{}:{}->{};{}", key, old_p, new_p, arm));
            }
        }
    }
    std::set<std::string> retro_emit_sats;   // B1 retro-bump applied this emit (serialized as an R record)
    // Edge-1 piece 2 (network.mark_refsat_reset_disc): fold this epoch's queued
    // ref-path-slip satellites into the SAME queue the B1 retro-bump drains, so they
    // inherit its RT0-5 "mark == land" semantics verbatim — a sat absent from this
    // epoch's emission stays PENDING rather than being consumed without landing, and
    // the R record therefore always describes bumps that actually reached the product.
    // Empty (never written) when the knob is off ⇒ this whole block is inert and the
    // OFF path is byte-identical.  Folding rather than adding a parallel drain is
    // deliberate: two independent drains would each need the pending/quarantine logic
    // and could double-bump a sat flagged by both tiers in one epoch.
    if (!ctx.carry.refsat_mark_sats.empty()) {
        ura_retro_sats.insert(ctx.carry.refsat_mark_sats.begin(),
                              ctx.carry.refsat_mark_sats.end());
        ctx.carry.refsat_mark_sats.clear();
    }
    // Edge-1 piece 2: the drain must also run when the refsat mark is armed but the
    // B1 retro-bump is NOT — otherwise queued sats would accumulate in ura_retro_sats
    // forever and no mark would ever land (a silent leak that would read as "the
    // feature does nothing").  mark_refsat is false by default, so the OFF-path
    // condition reduces to the original `ura_retro` exactly.
    if ((ura_retro || mark_refsat) && !ura_retro_sats.empty()) {
        // RT0-5 seam (P0-1: the R record must describe bumps that LANDED in the
        // emitted product): a flagged sat ABSENT from this epoch's emission —
        // its constellation quarantined above, or the sat itself vanished —
        // must not be consumed here.  The emitter would clear its mark without
        // any of its keys stepping, and the R record would overstate a bump
        // that never reached the product (replay would then diverge from
        // intent, not from the file).  Keep it PENDING so it is marked AND
        // serialized at its next actual emit; only sats present in the
        // (post-quarantine) sol.sats are consumed now, so mark == land and
        // replay re-marks exactly the landed sats on exactly those epochs.
        // Edge-triggered semantics preserved: a pending sat is already in
        // retro_active, so it is not re-queued while it waits.
        std::set<std::string> present_sats;
        for (const auto& s : sol.sats) present_sats.insert(s.sat);
        std::set<std::string> retro_pending;
        for (const auto& sat : ura_retro_sats)
            (present_sats.count(sat) ? retro_emit_sats : retro_pending).insert(sat);
        ura_retro_sats = std::move(retro_pending);   // absent sats wait for their next emit
        if (!retro_emit_sats.empty())
            emitter.markSatDiscontinuity(retro_emit_sats);   // per-sat, not constellation-wide
        // RETRO (V4): one row per per-sat discontinuity bump APPLIED this emit.  The QC
        // tier that flagged the sat last epoch is lost at the union, so detail=union
        // (honest — do not fabricate provenance).
        if (events_trace.is_open())
            for (const auto& s : retro_emit_sats)
                emitEvent("RETRO", "retro_bump", s, "", std::nullopt, std::nullopt,
                          "disc_bump", "union");
    }
    // Card 6: land the forced per-sat discontinuities for eph cutovers whose prior
    // version was evicted before Δ_eph could be computed (an unknown clock step → the
    // consumer must reset that sat's network-corrected ambiguity).  Independent of
    // ura_retro_bump — this is a genuine datum step, not a QC retro-bump — but reuses
    // the same present/pending split: a flagged sat ABSENT from this emission waits for
    // its next actual emit, so the bump lands with the sat present (replay-faithful).
    if (!eph_cutover_disc_pending.empty()) {
        std::set<std::string> present, mark, still_pending;
        for (const auto& s : sol.sats) present.insert(s.sat);
        for (const auto& sat : eph_cutover_disc_pending)
            (present.count(sat) ? mark : still_pending).insert(sat);
        eph_cutover_disc_pending = std::move(still_pending);
        if (!mark.empty()) {
            emitter.markSatDiscontinuity(mark);
            if (events_trace.is_open())
                for (const auto& s : mark)
                    emitEvent("RELEVEL", "eph_cutover_disc_landed", s, "", std::nullopt,
                              std::nullopt, "disc_bump", "prior_eph_version_evicted");
        }
    }
    // SPEC_day129_frame_guards Phase 2.1: land the code-bias discontinuity bump for
    // satellites whose CB carry the previous epoch's common-mode excursion detector
    // reinitialized.  Card-2 rule: the reinit makes the emitted value STEP this
    // epoch, so a signalled discontinuity must accompany it — without one, a
    // counter-following consumer integrates straight across the step, which is
    // exactly the DOY-129 failure (the +49 m fault bumped NOTHING).  Same
    // present/pending split as the two blocks around it, so the bump lands with the
    // satellite present and a replay reproduces the counters.  The set is empty
    // unless network.cb_excursion_reinit ⇒ byte-identical when OFF.
    if (!ctx.carry.cb_excursion_pending_bump.empty()) {
        std::set<std::string> present, mark, still_pending;
        for (const auto& s : sol.sats) present.insert(s.sat);
        for (const auto& sat : ctx.carry.cb_excursion_pending_bump)
            (present.count(sat) ? mark : still_pending).insert(sat);
        ctx.carry.cb_excursion_pending_bump = std::move(still_pending);
        if (!mark.empty()) {
            emitter.markSatDiscontinuity(mark);
            ctx.carry.cb_exc_bumps += static_cast<long long>(mark.size());
            if (events_trace.is_open())
                for (const auto& s : mark)
                    emitEvent("CB_EXC", "cb_excursion_disc_landed", s, "", std::nullopt,
                              std::nullopt, "disc_bump", "code_bias_carry_reinit");
        }
    }
    // Card Q3 (docs/SPEC_clock_temporal_anchor.md §2.4): land the per-satellite
    // discontinuity for a satellite whose temporal clock ANCHOR decoupled — the
    // invariant-#7 decoupling edge, made visible on the wire.  Its clock changed
    // BOTH temporal regime (stiff → loose) and, on a resolved trip, VALUE in the
    // same epoch, so a consumer that integrates across it is integrating across a
    // regime change it cannot see.  Same present/pending split as the three blocks
    // above (mark == land), so a replay reproduces the marks from the serialized R
    // records.  The set is empty unless network.clock_temporal_anchor AND
    // network.clock_anchor_mark ⇒ byte-identical otherwise.
    if (!ctx.carry.clk_anchor_disc_pending.empty()) {
        std::set<std::string> present, mark, still_pending;
        for (const auto& s : sol.sats) present.insert(s.sat);
        for (const auto& sat : ctx.carry.clk_anchor_disc_pending)
            (present.count(sat) ? mark : still_pending).insert(sat);
        ctx.carry.clk_anchor_disc_pending = std::move(still_pending);
        if (!mark.empty()) {
            // A satellite the B1 retro drain above already marked this epoch must
            // not have its counter bumped twice (one event, one step).
            std::set<std::string> fresh;
            for (const auto& s : mark)
                if (!retro_emit_sats.count(s)) fresh.insert(s);
            if (!fresh.empty()) emitter.markSatDiscontinuity(fresh);
            // …AND serialize it.  markSatDiscontinuity alone bumps only the
            // emitter's in-process counters; on a lineage without
            // network.serialize_disc_counters the per-record disc fields are not
            // written, so the R record is the ONLY carrier a NetworkProductIO
            // replay can read.  A mark visible to the live consumer and invisible
            // to the replay would make the two graders disagree about the same
            // wire — exactly the decomposition CLK-R's 3-way replay depended on.
            // Folding into retro_emit_sats also gives these satellites the
            // "forced" treatment downstream for free: a satellite whose clock was
            // just refloated must not vote in the CLK-R frame-hold median.
            retro_emit_sats.insert(mark.begin(), mark.end());
            if (events_trace.is_open())
                for (const auto& s : mark)
                    emitEvent("CLK_ANCHOR", "cta_disc_landed", s, "", std::nullopt,
                              std::nullopt, "disc_bump", "clock_anchor_decoupled");
        }
    }
    // T3c: land clock_signal_basis_hold RE-LATCH disc bumps this emit (same present/pending
    // split — a re-latched sat absent from this emission waits for its next actual emit).
    // markSatDiscontinuity bumps the per-sat phase/code/clock counters; the re-latch re-splits
    // δtˢ↔dˢ so all three step.  Populated only under clock_signal_basis_hold && emit_clock_disc
    // ⇒ the set is empty (this block a no-op) otherwise = byte-identical.  (Hold-OFF re-selection
    // is landed just below, at the emit-only this_epoch_if_pin_sel comparison.)
    if (!basis_relatch_disc_pending.empty()) {
        std::set<std::string> present, mark, still_pending;
        for (const auto& s : sol.sats) present.insert(s.sat);
        for (const auto& sat : basis_relatch_disc_pending)
            (present.count(sat) ? mark : still_pending).insert(sat);
        basis_relatch_disc_pending = std::move(still_pending);
        if (!mark.empty()) {
            emitter.markSatDiscontinuity(mark);
            if (events_trace.is_open())
                for (const auto& s : mark)
                    emitEvent("CLK_BASIS", "relatch_disc_landed", s, "", std::nullopt,
                              std::nullopt, "disc_bump", "if_pin_reference_moved");
        }
    }
    // T3c (hold OFF): the deterministic IF-pin re-selection discontinuity, resolved
    // ONLY here at the emit site so it describes the LAST EMITTED product's pair —
    // exactly the prev_pivot / emitted_anchor discipline.  Compare THIS emitted
    // product's selected pair (this_epoch_if_pin_sel, from the main pass) against the
    // last EMITTED pair (last_if_pin_sel) for every EMITTED satellite; a genuine
    // change re-references the emitted δtˢ↔dˢ split by the sat's inter-channel bias,
    // so force its per-sat disc bump (phase/code/clock counters).  Advancing the
    // baseline only here means a transient pin move on a WITHHELD epoch (which never
    // reaches this point) is dropped, and a pair that leaves and returns to the SAME
    // emitted reference across withheld epochs raises NO bump.  Only emitted sats are
    // compared/advanced (a sat withheld this epoch keeps its last-emitted baseline and
    // is compared when it next emits).  Empty scratch ⇒ no-op (byte-identical) when
    // emit_clock_disc is off or the run is under clock_signal_basis_hold.
    if (!this_epoch_if_pin_sel.empty()) {
        std::set<std::string> present;
        for (const auto& s : sol.sats) present.insert(s.sat);
        std::set<std::string> mark;
        for (const auto& [sat, sel] : this_epoch_if_pin_sel) {
            if (!present.count(sat)) continue;   // not emitted this epoch: baseline untouched
            const auto prev = last_if_pin_sel.find(sat);
            if (prev != last_if_pin_sel.end() && prev->second != sel) {
                mark.insert(sat);
                if (events_trace.is_open())
                    emitEvent("CLK_BASIS", "reselect", sat, "", std::nullopt, std::nullopt,
                              "if_pin_reference_moved",
                              prev->second.first + "+" + prev->second.second +
                                  "->" + sel.first + "+" + sel.second);
            }
            last_if_pin_sel[sat] = sel;   // advance the emit baseline (emitted sats only)
        }
        if (!mark.empty()) emitter.markSatDiscontinuity(mark);
    }
    // Card 2 (network.code_datum_time_linked): land the time-linked datum RE-SEED
    // disc bumps — a no-veteran sum-to-zero channel or an IF-pin split forced to the
    // 0 seed stepped the emitted value, so the consumer must reset instead of
    // integrating across the unsignalled step.  This_epoch_cb_reseed (built in the
    // main pass) is merged into the persistent pending set HERE, at the emit site —
    // which every whole-epoch withhold path `continue`s before reaching — so a
    // transient re-seed on a since-withheld epoch never enters the pending set (no
    // bump without an emitted step), while a re-seed the driver DID realize is
    // retained and deferred exactly like the eph-cutover / basis-relatch bumps: a
    // re-seeded sat ABSENT from THIS emission (pruned by quarantine / per-const
    // withhold) waits for its next actual emit so the bump lands with the sat present
    // (emit-only, replay-faithful).  Both sets are only ever populated under
    // code_datum_time_linked ⇒ no-op (byte-identical) otherwise.
    for (const auto& sat : this_epoch_cb_reseed) cb_reseed_disc_pending.insert(sat);
    if (!cb_reseed_disc_pending.empty()) {
        std::set<std::string> present, mark, still_pending;
        for (const auto& s : sol.sats) present.insert(s.sat);
        for (const auto& sat : cb_reseed_disc_pending)
            (present.count(sat) ? mark : still_pending).insert(sat);
        cb_reseed_disc_pending = std::move(still_pending);
        if (!mark.empty()) {
            emitter.markSatDiscontinuity(mark);
            if (events_trace.is_open())
                for (const auto& s : mark)
                    emitEvent("CB_DATUM", "reseed_disc_landed", s, "", std::nullopt,
                              std::nullopt, "disc_bump", "code_datum_time_linked_reseed");
        }
    }
    // The handover-detection baseline advances ONLY here, with the bump it
    // pairs with: prev_pivot/last_pivot describe the datum of the last
    // EMITTED product.  A handover epoch that never reaches this point
    // (gate withhold, solve failure, σ̂0 withhold) leaves the baseline
    // untouched, so the change re-detects — and re-skips the old-datum
    // carries — every epoch until one emits in the new datum.
    prev_pivot = plan.clock_pivot;
    // A1: merge-update the persistent last-ever pivot map (never erase a
    // constellation) so a later vanish-return with a different pivot is caught.
    for (const auto& kv : plan.clock_pivot) last_pivot[kv.first] = kv.second;
    // Prioritized phase-bias anchor: record per-(sat,signal) anchor HANDOVERS
    // for observability, and advance the emit-baseline (emitted_anchor).  The
    // consumer-facing DISCONTINUITY is already handled — replay-safely — by the
    // emitter's per-(sat,signal) bias-jump test: a handover steps the emitted
    // bias by a different receiver's integer ambiguity lump (≥ ~1 cycle, well
    // above disc_threshold_m), so emitToStore below bumps that key's counter and
    // the consumer resets the ambiguity.  We deliberately do NOT issue a separate
    // markSatDiscontinuity here: it is not reconstructable from the serialized
    // NetworkSolutionEpoch (replay does not re-run the election), so it would
    // make the product's disc counters diverge on NetworkProductIO replay.  The
    // baseline advances ONLY at emit (a withheld epoch re-detects next emit),
    // never erasing an absent key (matches the last_pivot policy).
    if (phase_datum_prioritized) {
        int n_handover = 0;
        for (const auto& [amb, sta] : this_epoch_anchor) {
            auto it = emitted_anchor.find(amb);
            if (it != emitted_anchor.end() && it->second != sta) {
                // eq34 (Card 3): a tree-edge REATTACH — the emitted δ̃_s steps by
                // an EXACT integer number of cycles (chain path difference; the
                // receiver fractional no longer leaks), usually 0 under pin-to-
                // round.  Disc still rides the emitter's replay-safe bias-jump
                // test (NEVER markSatDiscontinuity — not reconstructable on
                // NetworkProductIO replay).  Trace event only.
                //
                // ⚠ THE INTEGER CLAIM ABOVE HOLDS ONLY IF THE NEW HOST'S AMBIGUITY
                // HAS CONVERGED.  On a one-epoch-old arc the float is code-seeded
                // (± metres), so the "integer" step is an arbitrary re-level of the
                // whole component — the DOY-129 19:25:30 mechanism (this exact event,
                // G29 GAMB00PYF->INEG00MEX, followed one epoch later by a00 SUM
                // 38.7 → 5739 TECU and a 181 m total_IF scar that never healed).
                // That is now guarded in the ELECTION, not here:
                // docs/SPEC_eq34_reattach_hygiene.md /
                // filter/NetworkAnchorHygiene.hpp (network.eq34_anchor_min_arc_ep).
                // With the guard armed this event only fires for a host that has
                // cleared the arc-age threshold; a refused one is traced as
                // PHASE_DATUM/reattach_blocked_cold instead.
                emitEvent(phase_datum_eq34 ? "PHASE_DATUM" : "PHASE_ANCHOR",
                          phase_datum_eq34 ? "eq34_reattach" : "anchor_handover",
                          amb.substr(0, amb.find('_')), sta,
                          std::nullopt, std::nullopt, "reanchor",
                          it->second + "->" + sta);
                ++n_handover;
            }
            emitted_anchor[amb] = sta;
            // eq34 reattach hygiene, WARM CONDITIONING: stamp WHEN this key's datum was
            // last realized AND emitted.  The eligibility block is only warranted while
            // that memory is fresh — see filter/NetworkAnchorHygiene.hpp.  Advanced on
            // the same emit-only discipline as the baseline above (a withheld epoch
            // must not refresh it, or a long withhold would look warm forever).
            if (anchor_hygiene_on)
                ctx.carry.emitted_anchor_s[amb] =
                    TimeUtils::continuousGpsSeconds(se.time);
        }
        if (n_handover > 0)
            log->debug("Network: {} per-(sat,signal) phase-bias anchor handover(s) at "
                       "TOW {:.1f} (disc handled by the emitter bias-jump test).",
                       n_handover, se.time.tow);
    }
    // B6 (P1-1): open the deferred contract writer at the FIRST emit, stamping the
    // realized per-constellation clock pivots (this epoch's plan) into the v2 header.
    if (contract_pending) {
        contract_pending = false;
        for (const auto& [cst, sat] : plan.clock_pivot)
            pending_contract.decl.clock_pivot_sat[cst] = sat;   // realized auto-picked pivots
        // Card 3 (SingleReceiverChain): stamp the realized datum receiver r1 as
        // the diagnostic pdr= token (component-0 representative).
        if (phase_datum_eq34 && !eq34_datum_rx.empty()) {
            pending_contract.decl.eq34_datum_receiver = eq34_datum_rx.begin()->second;
            eq34_emitted_pdr = pending_contract.decl.eq34_datum_receiver;
        }
        // RTA-3: the generation id (minted above at this same first emit) travels
        // in the contract as an optional gid= field — 0 (unset) writes nothing.
        pending_contract.generation_id = sol.generation_id;
        product_writer = std::make_unique<NetworkProductWriter>(prod_path.string(), pending_contract);
        if (product_writer->good()) {
            log->info("Network: product serialization open — v2 S-basis contract, "
                      "{} realized clock pivot(s).", pending_contract.decl.clock_pivot_sat.size());
            last_contract_emit_s = pl_now_s;   // RTA-5: the header S line is the first broadcast
        } else {
            log->error("Network: failed to open product_output_path '{}' for writing; "
                       "disabling on-disk serialization for this run.", prod_path.string());
            product_writer.reset();
        }
    }
    // M4 (contract v2): per-key COMBINED-correction σ (sigc phase, sigcc code) +
    // code/phase correlation (rho) from the shared posterior covariance's
    // cross-terms.  The individual clock / phase-bias / code-bias formal σ (the
    // URA and B1b channels) are cm-level during a satellite's rise-reconvergence,
    // yet the COMBINED correction the user applies (clock + bias) is mm-level
    // because the errors are strongly correlated (Zhang 2021 Figs. 2–5; Psychas
    // 2024b Eq. 12).  A consumer weighting by sigc/sigcc keeps USING a fresh
    // satellite's products through reinitialization instead of deweighting them
    // 10–100×.  Iono is EXCLUDED (network iono is a global SH-VTEC field, not a
    // co-resident column, and is supplied separately via SsrSlantIono with its own
    // σ — bundling it here would double-count).  Gauge-nulled covariance copy +
    // datum-pin skip + positive-variance guard, exactly like B1b; a folded/zeroed
    // phase gauge (ensemble datum) yields no honest phase column ⇒ no token.
    // Scaled by the SAME σ̂0 EWMA (cancels in rho).  Off ⇒ every value 0 ⇒ no wire
    // token ⇒ product/store byte-identical.
    if (config_.networkEmitCombinedSigma()) {
        Eigen::MatrixXd cbcov = est.sharedCovariance();   // copy; treat gauges as exact
        est.zeroPhaseGaugeColumns(cbcov);
        std::set<Eigen::Index> cb_pinned;
        for (const auto& pin : plan.shared_datum) cb_pinned.insert(pin.column);
        const double s0 = std::max(1.0, ura_sigma0_ewma);
        // A column is usable only when present, in range, NOT a datum pin (its
        // 1/pin_scale variance is a gauge artefact), and carrying real positive
        // variance (rejects a zeroed phase gauge under the ensemble datum).
        auto colOrNeg = [&](auto fn) -> Eigen::Index {
            try {
                const Eigen::Index c = fn();
                if (c < 0 || c >= cbcov.rows() || c >= cbcov.cols() || cb_pinned.count(c))
                    return -1;
                if (!(cbcov(c, c) > 0.0) || !std::isfinite(cbcov(c, c))) return -1;
                return c;
            } catch (...) { return -1; }
        };
        for (auto& s : sol.sats) {
            if (!s.has_clock) continue;
            const Eigen::Index c = colOrNeg([&]{ return est.clockColumn(s.sat); });
            if (c < 0) continue;   // no honest clock column ⇒ nothing to combine
            // Per-signal combined PHASE σ (sigc) for every emitted phase bias.
            for (auto& [signal, npb] : s.phase_bias) {
                const Eigen::Index p =
                    colOrNeg([&]{ return est.phaseBiasColumn(s.sat, signal); });
                const auto cc = computeCombinedCorrectionSigma(cbcov, c, p, -1, s0);
                if (cc.sigc > 0.0) npb.sigc_m = cc.sigc;
            }
            // Per-sat combined CODE σ (sigcc) + code/phase corr (rho) at a REFERENCE
            // signal: prefer the first code signal whose PHASE sibling is present (so
            // rho pairs a real code/phase observable), else the first code signal (rho
            // then absent).  Code and phase biases live in DISJOINT key namespaces
            // (code "C1W" vs phase "L1W"), so the pairing needs the RINEX obs-type
            // swap selectCombinedCodeRef performs — a raw key equality never matches.
            // The C record is per-sat and the combined code σ is clock-dominated, so
            // this per-sat summary is a faithful proxy across the satellite's code
            // signals (documented approximation).
            std::vector<std::string> code_sigs;
            code_sigs.reserve(s.code_bias_m.size());
            for (const auto& [signal, bias] : s.code_bias_m) { (void)bias; code_sigs.push_back(signal); }
            std::set<std::string> phase_sigs;
            for (const auto& [signal, npb] : s.phase_bias) { (void)npb; phase_sigs.insert(signal); }
            const auto ref = selectCombinedCodeRef(code_sigs, phase_sigs);
            if (!ref.code.empty()) {
                const Eigen::Index b =
                    colOrNeg([&]{ return est.codeBiasColumn(s.sat, ref.code); });
                const Eigen::Index p = ref.phase.empty()
                    ? Eigen::Index(-1)
                    : colOrNeg([&]{ return est.phaseBiasColumn(s.sat, ref.phase); });
                const auto cc = computeCombinedCorrectionSigma(cbcov, c, p, b, s0);
                if (cc.sigcc > 0.0) { s.sigcc_m = cc.sigcc; s.rho = cc.rho; }
            }
        }
    }

    return { std::move(sol), std::move(epoch_withheld_consts), global_bump,
             std::move(serialized_consts), std::move(retro_emit_sats) };
}

} // namespace GnssProcessor

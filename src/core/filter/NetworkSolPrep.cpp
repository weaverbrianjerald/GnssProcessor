/**
 * @file NetworkSolPrep.cpp
 * @brief Implementation of @ref networkSolPrep, the SB_SOLPREP emit-preparation
 *        block moved out of Application::runNetwork at decomposition INC-11pre
 *        with a dedent and no other change.
 *
 * The body began as a copy of the former SB_SOLPREP block (ApplicationNetwork.cpp
 * lines 5034-5770 at the cut), dedented 4 spaces (loop-body indent, no wrapping
 * if); later cards extended it in place.  The alias preamble copies the run-TU
 * alias block so the body reads the original names, and the emitEvent forwarder is
 * rebuilt over NetworkRunTraces::emitEvent.  The five crossing locals stay declared
 * in the body and are moved into NetworkSolPrepResult by the return epilogue.  See
 * NetworkSolPrep.hpp for the ordering contract.
 */

#include "filter/NetworkSolPrep.hpp"

#include "filter/NetworkRhsAttrib.hpp"   // flight-recorder trip latch (INC-5)

#include "app/NetworkRunContext.hpp"            // ctx.* aliases
#include "common/GnssIdentity.hpp"              // systemOfSat, letterOfSat
#include "config/GnssConfig.hpp"                // config_ getters + getNetworkConfig
#include "filter/StateKeySchema.hpp"            // StateKeys::parseAmbiguityKey
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

// The run-end reports at the end of this file (card NOM step TD1b):
#include "filter/NetworkRunEnd.hpp"

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
    auto& this_epoch_if_pin_real = ctx.carry.this_epoch_if_pin_real;
    auto& last_if_pin_sel = ctx.carry.last_if_pin_sel;
    auto& this_epoch_anchor = ctx.carry.this_epoch_anchor;
    auto& emitted_anchor = ctx.carry.emitted_anchor;
    auto& eq34_datum_rx = ctx.carry.eq34_datum_rx;
    auto& eq34_emitted_pdr = ctx.carry.eq34_emitted_pdr;
    auto& clock_osc = ctx.carry.clock_osc;
    auto& eph_cutover_disc_pending = ctx.carry.eph_cutover_disc_pending;
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
    const bool   ura_on        = ctx.knobs.ura_on;                  // per-sat URA quality channel (B1)
    const bool   ura_retro     = ctx.knobs.ura_retro;             // retro-bump flagged satellites on next emit (B1)
    const bool   mark_refsat   = ctx.knobs.mark_refsat_reset_disc; // mark ref-path slips as per-sat disc (Edge-1 piece 2)
    const bool   datum_detect  = ctx.knobs.datum_station_detect;   // datum-station slip re-reference marks share the queue (§9.7.10)
    const double ura_refloat_inflate = ctx.knobs.ura_refloat_inflate; // URA multiplier after a refloat (B1)
    const bool   raim_on       = ctx.knobs.raim_on;            // post-fit network RAIM (Increment 3, T2)
    const double raim_withhold = ctx.knobs.raim_withhold; // >0 withholds (not emits) an epoch whose post-adaptation σ̂0 exceeds this ceiling; 0 = off (A4)
    const bool   emit_clock_disc = ctx.knobs.emit_clock_disc;            // clock disc channel + per-constellation bump scoping (A2)
    const bool   emit_gen_time   = ctx.knobs.emit_gen_time;       // stamp wall-clock time of generation per emitted epoch (RT0-4)
    const bool   emit_gen_id     = ctx.knobs.emit_gen_id;         // stamp a per-run solution-generation id from the first emitted epoch (RTA-3)
    const bool   clk_runaway_on  = ctx.knobs.clk_runaway_on;        // per-constellation clock-runaway quarantine (RT0-5)
    const double clk_runaway_mag = ctx.knobs.clk_runaway_mag;         // median |clock − global median| ceiling, m (RT0-5)
    const double clk_runaway_rate= ctx.knobs.clk_runaway_rate;      // median |drift| ceiling, m/s (RT0-5)
    const int    perconst_min_rows = ctx.knobs.perconst_min_rows;   // min σ̂0-pool rows for a constellation to be judged (RTA-10)
    const bool   clk_model_on   = ctx.knobs.clk_model_on;
    const double clk_model_max_gap = ctx.knobs.clk_model_max_gap;  // max gap (s) for clock-model reseed, drift reset, C1 ZOH; default pl_max_gap_s (R11)
    auto& iod = ctx.counters.iod;
    auto& generation_id = ctx.counters.generation_id;
    auto& last_contract_emit_s = ctx.counters.last_contract_emit_s;
    auto& n_emitted = ctx.counters.n_emitted;
    auto& n_runaway_epochs = ctx.counters.n_runaway_epochs;
    auto& n_perconst_partial_epochs = ctx.counters.n_perconst_partial_epochs;
    auto& n_perconst_diffuse_published = ctx.counters.n_perconst_diffuse_published;
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

    // Card GFC: count this kept epoch's pin pair changes and family-row realizations, and
    // declare each family-row switch.  This is datum bookkeeping, not emission.  Both
    // scratch maps are empty unless an arm is effective, so otherwise it does nothing.
    networkCodeDatumFamilyKeptEpoch(ctx, emitEvent);

    // --- Emit -----------------------------------------------------------
    NetworkSolutionEpoch sol = est.toSolutionEpoch(x_s, se.time, iod++,
                                                   /*provider_id=*/0, /*solution_id=*/0);
    // Stamp, per emitted satellite, the broadcast-ephemeris version (IODE) that the
    // producer's clock-C0 baseline used, so the consumer applies C0 against the same
    // broadcast clock (getSatelliteClockCorrection pinned to clock_iode, Card 7) and
    // the eph-cutover re-level (Card 6) can detect a version roll (Card 5, RTF-2).
    // The broadcast clock is station-independent (one shared LiveEphemerisStore), so
    // computeSatelliteClock here reports the version the per-station emission path
    // (computeEmissionState) selected this epoch: toe granularity (hours) is far
    // coarser than the intra-epoch light-travel spread, so pick() lands on the same
    // record.  Querying the store at the emit site is equivalent to the plan's
    // "thread the IODE out of the emission path" and needs no per-station
    // out-parameter.  GLONASS ('R') stays -1 (plan D8).  The block runs only in
    // brdc_rts mode; in sp3 mode clock_iode stays -1 everywhere, the Card-4 writer
    // emits no iode= token, and the product is byte-identical.
    if (brdc_rts) {
        auto& eph_store = LiveEphemerisStore::getInstance();
        for (auto& s : sol.sats) {
            if (!s.has_clock || s.sat.empty() || systemOfSat(s.sat) == System::GLONASS) continue;
            if (auto bc = eph_store.computeSatelliteClock(s.sat, se.time))
                s.clock_iode = bc->iode;
        }
    }
    // --- Band-2 companion inter-signal correction on the wire (RF-0f) ---------------
    // Both sets come from this epoch's solve-glue verdicts.  A companion whose ISC was
    // estimated is flagged `isc=obs`, so a consumer knows the value is
    // `b(pinned) + ISC^s` with a measured ISC rather than an assumed zero.  A companion
    // whose ISC had to be pinned at 0 is erased from the record: its value asserts
    // something the RINEX navigation message never stated, and the number alone cannot
    // tell a consumer so.  Withholding it is the signal card RF-0f specifies ("emitted
    // only when its ISC is observed").  Both sets are empty unless
    // network.sat_isc_state is armed, so otherwise the record stream is byte-identical.
    // --- Held extra-band column off the wire (B5D) -----------------------------------
    // The same rule one band further out.  A column on a band outside the pin pair that
    // no station determined this epoch and that had nothing to carry is held by a level
    // row in its channel's own frame: its value keeps the rank and is not measured, so
    // it is erased from the record for the reason RF-0f erases an unobservable ISC.  A
    // freed column is an estimate and is emitted normally; a carried one holds the
    // previous epoch's estimate and is emitted normally too.  The set is empty unless
    // network.sat_gf_extra_band_free is armed, so otherwise the record stream is
    // byte-identical.
    if (!ctx.carry.this_epoch_isc_obs.empty()
        || !ctx.carry.this_epoch_isc_pinned.empty()
        || !ctx.carry.this_epoch_extra_band_held.empty()) {
        for (auto& s : sol.sats) {
            for (auto it = s.code_bias_m.begin(); it != s.code_bias_m.end(); ) {
                const std::string key = s.sat + "|" + it->first;
                if (ctx.carry.this_epoch_isc_pinned.count(key)
                    || ctx.carry.this_epoch_extra_band_held.count(key)) {
                    it = s.code_bias_m.erase(it);
                } else {
                    if (ctx.carry.this_epoch_isc_obs.count(key))
                        s.isc_observed.insert(it->first);
                    ++it;
                }
            }
        }
    }
    // Stamp the wall-clock time of generation, distinct from the content epoch
    // se.time, so a consumer can measure product latency (RT0-4).  The wall clock is
    // non-deterministic, so the stamp is written only when armed; otherwise
    // generation_time stays {0,0}, no G record is serialized, and the output is
    // byte-identical.
    if (emit_gen_time) sol.generation_time = TimeUtils::nowGpsTime();
    // Stamp the per-run solution-generation id, minted once from the GPS time of the
    // first emitted epoch (week·604800 + ⌊tow⌋) (RTA-3).  The id is deterministic
    // under file replay, because it is serialized per epoch as an I record and read
    // back from the file.  A restarted live producer first emits at a later epoch
    // and so mints a different id, which the consumer treats as a global datum
    // break.  0 means "unset", so an armed run clamps to 1 in the week-0 corner,
    // which does not occur in practice.  Off: sol.generation_id stays 0, no I record
    // is written, the store generation stays unset, and the output is byte-identical.
    // A warm resume (RTB-7(a)) restores the previous run's non-zero id before the
    // epoch loop, so the generation_id == 0 guard below skips the mint; re-minting
    // would signal a global datum break and force a spurious consumer-wide hard
    // reset.  Only a cold start (no, stale or mismatched snapshot) reaches the mint.
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
    // Publish the two-state clock-model drift as C1; the consumer extrapolates
    // c0 + c1·dt (B3b).  clock_osc[sat].x1 is the drift carried into this epoch from
    // the previous epoch's posterior, which is the right extrapolation basis; this
    // epoch's clock-model update runs after the emit.  With clock_model off, C1 stays 0.
    if (clk_model_on)
        for (auto& s : sol.sats) {
            const auto oit = clock_osc.find(s.sat);
            if (oit == clock_osc.end() || !oit->second.init) continue;
            // A satellite returning from a gap (for example an eclipse) still carries its
            // pre-gap drift here, because the clock-model update that reseeds x1 to 0 runs
            // after this emit; publishing oit->second.x1 unconditionally would emit an
            // hours-old rate as C1 for one epoch (R6).  The carried state's age is gated
            // against the staleness bound the prediction and the drift finite difference
            // use (R11): past the gap the drift is not trusted, so c1 stays 0 (ZOH),
            // matching the reseed the update will do.
            const double sat_dt = (oit->second.t_last > 0.0) ? (pl_now_s - oit->second.t_last)
                                                             : 0.0;
            if (sat_dt >= 0.0 && sat_dt <= clk_model_max_gap) {
                s.clock_c1_m_s = oit->second.x1;
                c1_zoh_active.erase(s.sat);
            } else {
                // A return past the gap ships c1 = 0 (ZOH) for one epoch instead of an
                // hours-old rate (T5/R6, V2 blind spot).  The event is edge-triggered on
                // c1_zoh_active, so a satellite that stays stale logs once, not every emit.
                if (events_trace.is_open() && c1_zoh_active.insert(s.sat).second)
                    emitEvent("T5_clock_model", "c1_zoh_suppress", s.sat, "", sat_dt,
                              clk_model_max_gap, "c1_zero", "vanish_return");
            }
        }
    // Constellations withheld from this epoch's emission, filled by the RT0-5
    // runaway quarantine and the per-constellation σ̂0 carve-out below.  The set
    // feeds the H heartbeat's status (ok or partial) and its withheld field (RTA-10).
    std::set<char> epoch_withheld_consts;
    // Per-constellation clock-runaway quarantine (RT0-5).  This epoch's clock
    // solutions are grouped by constellation.  When a constellation's median
    // |clock − global median| exceeds the magnitude ceiling, or its median |drift|
    // exceeds the rate ceiling, its satellites are dropped from the emission and the
    // constellation is flagged, so a runaway (for example the open BeiDou clock
    // divergence) narrows the product's scope instead of degrading all of it.  Off:
    // sol is untouched (byte-identical).  The pass runs after the C1 drift stamp, so
    // the rate ceiling sees this epoch's clock_c1_m_s (satellites with C1 = 0 are left
    // out of the rate median, so with no C1 published only the magnitude ceiling
    // applies), and before the URA pass, so a withheld constellation costs no URA work.
    //
    // Known limitation, not guarded against: the magnitude test is referenced to the
    // global median over all clock satellites.  When the runaway constellation
    // supplies half or more of the epoch's clock satellites, the global median falls
    // inside the runaway cluster and the comparison inverts: the healthy minority
    // trips the ceiling and is quarantined while the runaway constellation passes.
    // The check is sized for a minority constellation (for example the BeiDou
    // divergence, walking off by kilometres against a multi-constellation majority)
    // and is not suitable for a run dominated by the suspect constellation.  The
    // single-constellation case is warned at config load.
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
                const char cons = letterOfSat(s.sat);
                dev[cons].push_back(std::abs(s.clock_c0_m - gmed));
                if (s.clock_c1_m_s != 0.0) rate[cons].push_back(std::abs(s.clock_c1_m_s));
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
                        [c](const NetworkSatSolution& s) {
                            return !s.sat.empty() && letterOfSat(s.sat) == c;
                        });
                    log->warn("Network (RT0-5): constellation '{}' clock runaway — median |Δclk|="
                              "{:.1f} m (ceil {:.0f}), median |drift|={:.3f} m/s (ceil {:.2f}); "
                              "WITHHOLDING its {} sats this epoch.",
                              c, dm, clk_runaway_mag, rm, clk_runaway_rate, n_drop);
                    emitEvent("RT0-5_runaway", "constellation_withhold", std::string(1, c), "",
                              dm, clk_runaway_mag, "drop_const", fmt::format("drift={:.3f}", rm));
                }
            }
        }
        // Flight-recorder trigger for the runaway era (INC-5,
        // docs/SPEC_prefit_census_observability.md §7): called on every epoch the check
        // evaluates, not only on a runaway.  An empty `runaway` clears the era and
        // re-arms the latch, and every call is counted, so the count covers all
        // evaluated epochs.  SB_SOLPREP runs before SB_FRAMEDIAG, so a pending trip is
        // consumed later in the same epoch.
        networkRhsTripEdge(ctx, "clk_runaway", !runaway.empty());
        if (!runaway.empty()) {
            std::vector<NetworkSatSolution> kept;
            kept.reserve(sol.sats.size());
            for (auto& s : sol.sats)
                if (s.sat.empty() || runaway.count(letterOfSat(s.sat)) == 0)
                    kept.push_back(std::move(s));
            sol.sats.swap(kept);
            ++n_runaway_epochs;
            epoch_withheld_consts.insert(runaway.begin(), runaway.end());  // for the H heartbeat (RTA-10)
        }
    }
    // Apply the per-constellation σ̂0 carve-out (RTA-10, network.per_const_withhold),
    // decided upstream at the σ̂0 withhold site and passed in as perconst_active and
    // perconst_publish.  Satellites of non-publishable constellations are pruned from
    // the emission with the same swap-prune as the RT0-5 runaway quarantine above,
    // and at the same point: after the C1 drift stamp and before the URA pass, so a
    // withheld constellation costs no URA work.  Unlike RT0-5, nothing here reads
    // clock_c1_m_s; a pruned satellite only had its C1 stamped needlessly, which is
    // harmless.  A satellite whose constellation is not in the publish set is pruned,
    // including any constellation absent from the σ̂0 pools (zero rows, so it cannot be
    // judged and is not publishable on a contaminated epoch).  The downstream emitter
    // steps (presence-aware pending datum bumps, the post-prune retro split, the
    // rise/set code-datum detection) all key on the post-prune sol.sats, so they treat
    // this prune as they treat the tested RT0-5 quarantine.
    if (perconst_active) {
        std::set<char> pruned;
        std::vector<NetworkSatSolution> kept;
        kept.reserve(sol.sats.size());
        for (auto& s : sol.sats) {
            const char cons = letterOfSat(s.sat);   // an empty id is kept below
            if (!s.sat.empty() && perconst_publish.count(cons) == 0)
                pruned.insert(cons);
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
            // Limit of the per-constellation approximation: the global σ̂0 exceeded the
            // ceiling, yet every judged constellation passed on its own (coupling
            // across constellations through shared per-station nuisances, which no
            // per-constellation partial captures).  Everything is published, with a
            // warning and a count (n_perconst_diffuse_published); a rising count means
            // the carve-out's separability assumption is failing on this network.
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
        // Otherwise the decision withheld constellations that are not in this epoch's
        // emission anyway (RT0-5 already quarantined them, or they have no solution
        // satellites): nothing is pruned here, and it is not the diffuse case either.
    }
    // --- Per-satellite wire withhold (GL-2, docs/SPEC_event_harness.md §24.19) ---
    // A satellite the gross-LOS screen stood down (over its bar at >= N stations in one
    // epoch, which indicates a satellite fault rather than N line-of-sight faults) is
    // pruned from this epoch's emission with the swap-prune the RT0-5 runaway
    // quarantine and the RTA-10 per-constellation carve-out use above.  The withhold is
    // per satellite, never per epoch: every other satellite still emits, and the epoch
    // stays adapt-and-emit.  The consumer learns of it through the retro disc bump the
    // screen queued into `ura_retro_sats` together with this set.  That queue is
    // drained below with mark == land semantics, so a withheld satellite stays pending
    // and is marked at its next actual emission, the epoch at which the consumer needs
    // the reset (provided the drain is armed; its condition below has no stand-down
    // term).  The set is empty unless network.gross_los_sat_standdown and
    // network.gross_los_sat_wire_withhold are both on, so otherwise the output is
    // byte-identical.
    if (!ctx.carry.gl2_wire_pending.empty()) {
        std::set<std::string> gl2_pruned;
        std::vector<NetworkSatSolution> gl2_kept;
        gl2_kept.reserve(sol.sats.size());
        for (auto& gs : sol.sats) {
            if (ctx.carry.gl2_wire_pending.count(gs.sat)) gl2_pruned.insert(gs.sat);
            else                                          gl2_kept.push_back(std::move(gs));
        }
        if (!gl2_pruned.empty()) {
            sol.sats.swap(gl2_kept);
            ctx.counters.gross_los.sat_wire_withheld +=
                static_cast<long long>(gl2_pruned.size());
            for (const auto& gsat : gl2_pruned)
                emitEvent("GROSSLOS", "gross_los_sat_wire_withhold", gsat, "",
                          std::nullopt, std::nullopt, "drop_sat",
                          "reason=whole_satellite_standdown;epoch_still_emits=1");
        }
    }
    // --- Declared line of sight leaves the contributor set (RF-3b B3,
    // docs/refound/RF-3_code_row_qc_2026-09-22.md §4): for the epoch it is declared,
    // the line of sight no longer counts in its satellite's contributor set.  Both
    // outcomes are exact and land on this epoch, because the screen's decision half
    // runs before this seam:
    //   * too few contributing stations remain to support the satellite's records: the
    //     satellite is withheld with the same swap-prune as the GL-2 block and the
    //     RT0-5 runaway quarantine, per satellite and never per epoch;
    //   * enough remain: the satellite still emits and carries the producer's
    //     per-satellite discontinuity, so the consumer is told that the contributor set
    //     moved instead of seeing an unsignalled step (through the ura_retro_sats
    //     queue, drained below with mark == land semantics).
    // Both sets are empty unless network.gross_los_emit_exclude is on, so otherwise the
    // output is byte-identical.
    if (!ctx.carry.gl_emit_disc.empty()) {
        for (const auto& gsat : ctx.carry.gl_emit_disc)
            ctx.carry.ura_retro_sats.insert(gsat);
        ctx.counters.gross_los.emit_disc_marked +=
            static_cast<long long>(ctx.carry.gl_emit_disc.size());
    }
    if (!ctx.carry.gl_emit_withhold.empty()) {
        std::set<std::string> gl3_pruned;
        std::vector<NetworkSatSolution> gl3_kept;
        gl3_kept.reserve(sol.sats.size());
        for (auto& gs : sol.sats) {
            if (ctx.carry.gl_emit_withhold.count(gs.sat)) gl3_pruned.insert(gs.sat);
            else                                          gl3_kept.push_back(std::move(gs));
        }
        if (!gl3_pruned.empty()) {
            sol.sats.swap(gl3_kept);
            ctx.counters.gross_los.emit_withheld +=
                static_cast<long long>(gl3_pruned.size());
            for (const auto& gsat : gl3_pruned)
                emitEvent("GROSSLOS", "gross_los_emit_withheld", gsat, "",
                          std::nullopt, std::nullopt, "drop_sat",
                          "reason=declared_los_left_contributor_set;epoch_still_emits=1");
        }
    }
    // Per-satellite URA = max(1, EWMA(σ̂0)) × formal clock σ, the shared-covariance
    // diagonal (B1).  Datum-pinned clock columns are skipped (their variance is the
    // artificial pin, not a physical σ), and a recently refloated satellite is
    // inflated for its warmup horizon.  The σ̂0 EWMA is the previous epoch's, because
    // the QC that updates it runs after the emit (a one-epoch lag).
    if (ura_on) {
        // Evaluate the quality-ramp lift rule (RTB-7(b)); counts run through the
        // previous epoch, matching the floor's indexing below.  The ramp lifts when
        // n_emitted ≥ max(warmup_epochs, phase_leveling_warmup_epochs) and the σ̂0
        // EWMA has received ≥ warmup_epochs updates.  When the EWMA is structurally
        // unavailable (the RTB-6 sentinel condition: emit_ura, raim and
        // phase_leveling not all armed; ura_on holds inside this block, so
        // raim_on && pl_on decides), the epoch horizon alone lifts it.  The lift
        // logs the cold-start-to-product-grade metric (RTB-7(c)).  The rule runs
        // only with a floor set, so with warmup_ura_floor_m = 0 the output is
        // byte-identical.
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
            if (pinned.count(col)) continue;   // datum pivot: the pin variance is artificial, so no URA
            if (col < 0 || col >= scov.rows() || col >= scov.cols()) continue;
            const double var = scov(col, col);
            if (!(var > 0.0) || !std::isfinite(var)) continue;
            double ura = s0 * std::sqrt(var);
            const auto hit = ura_refloat_horizon.find(s.sat);
            if (ura_refloat_inflate > 1.0 && hit != ura_refloat_horizon.end() && hit->second > 0)
                ura *= ura_refloat_inflate;   // stands in for the post-refloat DIA penalty
            // Cold-start quality ramp (RTB-7(b)): a freshly (re)started producer
            // advertises a URA of at least warmup_ura_floor_m until the ramp lifts
            // (the networkWarmupRampLifted rule above: emitted-epoch horizon and σ̂0
            // EWMA stabilization, not a bare epoch count), so a consumer does not
            // trust a network solution that has not converged.  A warm-resumed
            // producer restores ramp_lifted from the snapshot and is not degraded.
            // Off (floor 0): byte-identical.
            if (nc.warmup_ura_floor_m > 0.0 && !ramp_lifted)
                ura = std::max(ura, nc.warmup_ura_floor_m);
            s.clock_ura_m = ura;
            s.has_ura     = true;
        }
    }
    // Per-(sat, signal) phase-bias correction σ (B1b, defect-2 lever B, Tienstra §6.6):
    // the phase-bias block of Qŷ_B1 that the clock-only URA channel omits.  Without it
    // the consumer applies phase-bias corrections as if exact; with their formal 1σ, a
    // wandering bias (the 6–69 cm differential defect-2 driver) is down-weighted
    // instead.  Gauges are treated as exact: a copy of the shared covariance with the
    // folded phase-bias gauges nulled (zeroPhaseGaugeColumns), and
    // datum-pinned columns skipped (their 1/pin_scale variance is a gauge, not a
    // physical σ), the same pin treatment as the URA loop.  σ is scaled by the same
    // σ̂0 EWMA as the URA.  Off: σ stays 0, and the product and store are
    // byte-identical.
    if (config_.networkEmitPhaseBiasSigma()) {
        Eigen::MatrixXd        pcov = est.sharedCovariance();   // copy; treat gauges as exact
        est.zeroPhaseGaugeColumns(pcov);
        std::set<Eigen::Index> pb_pinned;
        for (const auto& pin : plan.shared_datum) pb_pinned.insert(pin.column);
        const double        s0 = std::max(1.0, ura_sigma0_ewma);
        std::vector<double> pb_sigmas;   // premise-check diagnostic: distribution of absolute emitted σ
        // Per-signal columns are also collected to measure the between-satellite
        // differenced σ, for the B2 versus Lever A decision.  The consumer's receiver
        // clock and per-arc float ambiguity absorb the common (level) direction of the
        // phase-bias error (TK-2015 Eq 88), so the absolute σ (a level entangled with
        // the float ambiguities, about 1 m at 0 fixes) overstates what reaches the
        // consumer's estimate.  The between-satellite SD σ is the per-epoch magnitude
        // relevant to the consumer.
        std::map<std::string, std::vector<Eigen::Index>> by_signal;
        for (auto& s : sol.sats) {
            for (auto& [signal, npb] : s.phase_bias) {
                Eigen::Index col;
                try { col = est.phaseBiasColumn(s.sat, signal); } catch (...) { continue; }
                if (pb_pinned.count(col)) continue;                 // anchor-pinned: exact, so no σ
                if (col < 0 || col >= pcov.rows() || col >= pcov.cols()) continue;
                const double var = pcov(col, col);
                if (!(var > 0.0) || !std::isfinite(var)) continue;  // folded gauge (zeroed) or degenerate
                npb.sigma_m = s0 * std::sqrt(var);
                pb_sigmas.push_back(npb.sigma_m);
                by_signal[signal].push_back(col);
            }
        }
        // Lever-B premise check and the deciding ratio: absolute σ against
        // between-satellite SD σ.
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
    // Datum bump and its serialization, applied here, after a successful solve, so a
    // withheld or failed epoch never leaks a pending bump.  The A1/A2 clock-pivot
    // handover (this epoch) is a per-constellation datum re-reference (E-line
    // field-8); the B1 retro-bump (a post-emit QC tier flagged these satellites last
    // epoch) is a per-satellite discontinuity (R record).  Both must reproduce on
    // replay from the serialized flags.  A global datum bump (emit_clock_disc off)
    // serializes an empty constellation set, so replay re-applies the global bump.
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
        // PIVOT events (V1/V2); without them the datum re-reference is not logged.  One
        // row per changed constellation: sat = new pivot, detail = <const>:old->new plus
        // the arm.  The arm is derived here because pivotHandoverSignalled does not
        // return it: when prev_pivot still holds the key the handover is adjacent,
        // otherwise it is a vanish_return, with the old pivot taken from last_pivot.  The
        // rows are emitted before prev_pivot advances below.
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
                // At the decoupling edge, state what the S-transform did to this
                // constellation's carry: applied, with its coefficient, or skipped, the
                // baseline free-fall (Queue 1(b) INC-2).  The tag is appended only when
                // network.handover_stransform is armed, so PIVOT rows without it are
                // byte-identical to those written before the card.
                std::string sts_tag;
                {
                    const auto cit = ctx.carry.sts_c0.find(c);
                    if (cit != ctx.carry.sts_c0.end())
                        sts_tag = fmt::format(";sts=applied;c0={:.6f};rows={}",
                                              cit->second, ctx.counters.sts_rows_retained);
                    else if (ctx.knobs.handover_stransform)
                        sts_tag = ";sts=skipped";
                }
                emitEvent("PIVOT", "handover", new_p, "", std::nullopt, std::nullopt,
                          "datum_bump",
                          fmt::format("{}:{}->{};{}{}", key, old_p, new_p, arm, sts_tag));
            }
        }
    }
    std::set<std::string> retro_emit_sats;   // per-sat disc bumps that landed this emit (serialized as the R record)
    // Fold this epoch's queued ref-path-slip satellites into the queue the B1
    // retro-bump drains (Edge-1 piece 2, network.mark_refsat_reset_disc), so they get
    // its RT0-5 "mark == land" semantics unchanged: a satellite absent from this
    // epoch's emission stays pending instead of being consumed without landing, so
    // the R record describes only bumps that reached the product.  With the knob off
    // the set is never written, so this block is inert and the output byte-identical.
    // One queue rather than a parallel drain by design: two independent drains would
    // each need the pending/quarantine logic and could bump a satellite twice when
    // both tiers flag it in one epoch.
    if (!ctx.carry.refsat_mark_sats.empty()) {
        ura_retro_sats.insert(ctx.carry.refsat_mark_sats.begin(),
                              ctx.carry.refsat_mark_sats.end());
        ctx.carry.refsat_mark_sats.clear();
    }
    // The drain must be armed whenever a source queues into ura_retro_sats, not only
    // with the B1 retro-bump.  A source queuing into a run where the drain is not
    // armed would accumulate satellites in ura_retro_sats indefinitely: no mark would
    // land, the R record would not be written, and nothing would report it.  The
    // terms of the arming OR:
    //   * ura_retro: the B1 retro-bump;
    //   * mark_refsat: reference-path slip marks (Edge-1 piece 2);
    //   * datum_detect: the datum-station slip re-reference, §9.7.10;
    //   * pd_held_basis: a declared anchor handover queues its satellite here, so the
    //     held-basis arm needs no second drain (RF-2 item 2);
    //   * gl_emit_exclude: the emission-exclusion disc mark (RF-3b B3).
    // The GL-2 stand-down also queues here but has no term of its own.  With the added
    // terms off (mark_refsat is false by default), the condition reduces to the
    // original `ura_retro`, and the output is byte-identical.
    if ((ura_retro || mark_refsat || datum_detect || ctx.knobs.pd_held_basis
         || ctx.knobs.gl_emit_exclude)
        && !ura_retro_sats.empty()) {
        // The R record must describe only bumps that landed in the emitted product
        // (P0-1, RT0-5).  A flagged satellite absent from this epoch's emission (its
        // constellation quarantined above, or the satellite itself gone) is not
        // consumed here: the emitter would clear its mark without any of its keys
        // stepping, and the R record would claim a bump that never reached the
        // product (replay would then match the file but not the intent).  It stays
        // pending and is marked and serialized at its next actual emit.  Only
        // satellites present in the post-quarantine sol.sats are consumed now, so
        // mark == land, and replay re-marks the landed satellites on the epochs they
        // landed.  The edge-triggered semantics hold: a pending satellite is already
        // in retro_active, so it is not re-queued while it waits.
        std::set<std::string> present_sats;
        for (const auto& s : sol.sats) present_sats.insert(s.sat);
        std::set<std::string> retro_pending;
        for (const auto& sat : ura_retro_sats)
            (present_sats.count(sat) ? retro_emit_sats : retro_pending).insert(sat);
        ura_retro_sats = std::move(retro_pending);   // absent sats wait for their next emit
        if (!retro_emit_sats.empty())
            // The union drain (Card MS): every source of ura_retro_sats arrives here
            // (URA-retro QC contamination, the reference-satellite mark, the
            // datum-station slip re-reference, the RF-2 held-basis anchor handover, the
            // gross-LOS marks), and provenance is dropped at the union by design (see
            // the detail="union" event below).  A contaminated epoch can have moved any
            // channel, and a held-basis handover moves the phase bias, so this mark keeps
            // the full satellite scope: Card MS narrows the scope only at sites that know
            // their channel, never at a union that does not.
            emitter.markSatDiscontinuity(retro_emit_sats,   // per-sat, not constellation-wide
                                         NetworkProductEmitter::kDiscChanAll);
        // RETRO events (V4): one row per per-satellite discontinuity bump applied this
        // emit.  The tier that flagged the satellite is not known after the union, so
        // detail=union rather than a guessed provenance.
        if (events_trace.is_open())
            for (const auto& s : retro_emit_sats)
                emitEvent("RETRO", "retro_bump", s, "", std::nullopt, std::nullopt,
                          "disc_bump", "union");
    }
    // Land the forced per-satellite discontinuities for eph cutovers whose prior
    // version was evicted before Δ_eph could be computed (Card 6): the clock step is
    // unknown, so the consumer must reset that satellite's network-corrected
    // ambiguity.  This is a datum step, not a QC retro-bump, so it does not depend on
    // ura_retro_bump, but it uses the same present/pending split: a flagged satellite
    // absent from this emission waits for its next actual emit, so the bump lands with
    // the satellite present.  The landed mark is folded into retro_emit_sats (the Q3
    // trap-1 rule, see the CLK_ANCHOR block below), so the R record carries it to a
    // NetworkProductIO replay.
    if (!eph_cutover_disc_pending.empty()) {
        std::set<std::string> present, mark, still_pending;
        for (const auto& s : sol.sats) present.insert(s.sat);
        for (const auto& sat : eph_cutover_disc_pending)
            (present.count(sat) ? mark : still_pending).insert(sat);
        eph_cutover_disc_pending = std::move(still_pending);
        if (!mark.empty()) {
            // One event, one step: a satellite the B1 retro drain above already marked
            // this epoch does not have its counters bumped twice.  Every landed mark is
            // still serialized: markSatDiscontinuity alone bumps only the in-process
            // counters, and without network.serialize_disc_counters the R record is the
            // only carrier a NetworkProductIO replay can read.  A mark visible live but
            // not on replay would make the two graders disagree about the same wire.
            std::set<std::string> fresh;
            for (const auto& s : mark)
                if (!retro_emit_sats.count(s)) fresh.insert(s);
            // Full scope (Card MS): the prior ephemeris version was evicted before
            // Delta_eph could be computed, so the step is unknown in magnitude and in
            // channel; the satellite clock re-references and the bias split moves with it.
            if (!fresh.empty())
                emitter.markSatDiscontinuity(fresh, NetworkProductEmitter::kDiscChanAll);
            retro_emit_sats.insert(mark.begin(), mark.end());
            if (events_trace.is_open())
                for (const auto& s : mark)
                    emitEvent("RELEVEL", "eph_cutover_disc_landed", s, "", std::nullopt,
                              std::nullopt, "disc_bump", "prior_eph_version_evicted");
        }
    }
    // Land the code-bias discontinuity bump for satellites whose CB carry was
    // reinitialized by the previous epoch's common-mode excursion detector
    // (SPEC_day129_frame_guards Phase 2.1).  The reinit steps the emitted value this
    // epoch, and under the Card-2 rule a signalled discontinuity must accompany it;
    // without one, a counter-following consumer integrates across the step (the
    // DOY-129 failure, where the fault bumped no counter).  The present/pending split
    // is the same as in the blocks around it, so the bump lands with the satellite
    // present, and the landed mark folds into retro_emit_sats (Q3 trap-1) so a replay
    // reproduces the counters from the R record.  The set is empty unless
    // network.cb_excursion_reinit is on, so otherwise the output is byte-identical.
    if (!ctx.carry.cb_excursion_pending_bump.empty()) {
        std::set<std::string> present, mark, still_pending;
        for (const auto& s : sol.sats) present.insert(s.sat);
        for (const auto& sat : ctx.carry.cb_excursion_pending_bump)
            (present.count(sat) ? mark : still_pending).insert(sat);
        ctx.carry.cb_excursion_pending_bump = std::move(still_pending);
        if (!mark.empty()) {
            // Dedupe against the blocks above (one event, one step) and serialize every
            // landed mark, since the counter-only path is invisible to replay (see the
            // eph-cutover block).
            std::set<std::string> fresh;
            for (const auto& s : mark)
                if (!retro_emit_sats.count(s)) fresh.insert(s);
            // Code-bias channel only (Card MS): the code-bias carry's excursion detector
            // reinitialized, so the emitted code bias steps and nothing else does.
            if (!fresh.empty())
                emitter.markSatDiscontinuity(fresh, NetworkProductEmitter::kDiscChanCodeBias);
            retro_emit_sats.insert(mark.begin(), mark.end());
            ctx.carry.cb_exc_bumps += static_cast<long long>(mark.size());
            if (events_trace.is_open())
                for (const auto& s : mark)
                    emitEvent("CB_EXC", "cb_excursion_disc_landed", s, "", std::nullopt,
                              std::nullopt, "disc_bump", "code_bias_carry_reinit");
        }
    }
    // Land the per-satellite discontinuity for a satellite whose temporal clock anchor
    // decoupled, which makes the decoupling edge of invariant #7 visible on the wire
    // (Card Q3, docs/SPEC_clock_temporal_anchor.md §2.4): in the same epoch its clock
    // changed temporal regime (stiff to loose) and, on a resolved trip, value, so a
    // consumer integrating across it would cross a regime change it cannot see.  The
    // present/pending split is the same as in the blocks above (mark == land), so a
    // replay reproduces the marks from the serialized R records.  The set is empty
    // unless network.clock_temporal_anchor and network.clock_anchor_mark are both on,
    // so otherwise the output is byte-identical.
    if (!ctx.carry.clk_anchor_disc_pending.empty()) {
        std::set<std::string> present, mark, still_pending;
        for (const auto& s : sol.sats) present.insert(s.sat);
        for (const auto& sat : ctx.carry.clk_anchor_disc_pending)
            (present.count(sat) ? mark : still_pending).insert(sat);
        ctx.carry.clk_anchor_disc_pending = std::move(still_pending);
        if (!mark.empty()) {
            // A satellite a block above (the B1 retro drain, an eph-cutover or a
            // CB-excursion landing) already marked this epoch must not have its
            // counter bumped twice (one event, one step).
            std::set<std::string> fresh;
            for (const auto& s : mark)
                if (!retro_emit_sats.count(s)) fresh.insert(s);
            // Clock channel only (Card MS): the Q3 temporal clock anchor decoupled, so the
            // satellite's clock changed temporal regime and, on a resolved trip, value;
            // its phase and code biases did not.  CST S7.1 measured this site: a clock
            // decouple moved the phase bias by at most 0.011 m, yet a full-scope mark
            // reset consumer satellites for it.
            if (!fresh.empty())
                emitter.markSatDiscontinuity(fresh, NetworkProductEmitter::kDiscChanClock);
            // Serialize the mark as well.  markSatDiscontinuity alone bumps only the
            // emitter's in-process counters; without network.serialize_disc_counters
            // the per-record disc fields are not written, so the R record is the only
            // carrier a NetworkProductIO replay can read.  A mark visible to the live
            // consumer but not to the replay would make the two graders disagree about
            // the same wire, and CLK-R's 3-way replay decomposition relies on them
            // agreeing.  Folding into retro_emit_sats also gives these satellites the
            // "forced" treatment downstream: a satellite whose clock was just refloated
            // must not vote in the CLK-R frame-hold median.
            retro_emit_sats.insert(mark.begin(), mark.end());
            if (events_trace.is_open())
                for (const auto& s : mark)
                    emitEvent("CLK_ANCHOR", "cta_disc_landed", s, "", std::nullopt,
                              std::nullopt, "disc_bump", "clock_anchor_decoupled");
        }
    }
    // The deterministic IF-pin re-selection discontinuity (T3c).  It is resolved only
    // here at the emit site, so it compares against the
    // last emitted product's pair, the same discipline as prev_pivot and
    // emitted_anchor.  For every emitted satellite, this product's selected pair
    // (this_epoch_if_pin_sel, from the main pass) is compared with the last emitted
    // pair (last_if_pin_sel).  A change re-references the emitted δtˢ↔dˢ split by the
    // satellite's inter-channel bias, so its per-satellite disc bump is forced (phase,
    // code and clock counters).  Because the baseline advances only here, a transient
    // pin move on a withheld epoch (which never reaches this point) is dropped, and a
    // pair that leaves and returns to the same emitted reference across withheld
    // epochs raises no bump.  Only emitted satellites are compared and advanced; a
    // satellite withheld this epoch keeps its last emitted baseline and is compared
    // when it next emits.  The scratch map is empty when emit_clock_disc is off, so the
    // block does nothing and the output is byte-identical.
    if (!this_epoch_if_pin_sel.empty()) {
        std::set<std::string> present;
        for (const auto& s : sol.sats) present.insert(s.sat);
        std::set<std::string> mark;
        // Card GFC (network.code_datum_pair_continuity): a satellite the builder continued
        // this epoch changed pair without moving its datum (the new pair's target is its
        // previous solved combination).  Its re-selection is left undeclared only when the
        // comparison here is the one the builder made: the last emitted pair is the pair
        // the builder continued from (it compared against the last KEPT pair, which
        // differs for a satellite pruned from an earlier kept epoch's emission, whose
        // earlier re-selection then still needs this declaration), and no re-seed marked
        // the satellite this epoch or is pending for it.  The map is this epoch's
        // main-pass scratch, empty unless the arm is effective.  A continued satellite
        // outside this emission keeps its last emitted pair as the baseline and is
        // compared when it next emits (the conservative side).
        const auto& changes = ctx.carry.this_epoch_if_pin_changes;
        using PinOutcome = NetworkSbasis::PinPairOutcome;
        for (const auto& [sat, change] : changes)
            if (change.outcome == PinOutcome::Continued && !present.count(sat))
                ++ctx.counters.n_if_pin_continued_unemitted;
        for (const auto& [sat, sel] : this_epoch_if_pin_sel) {
            if (!present.count(sat)) continue;   // not emitted this epoch: baseline untouched
            const auto prev = last_if_pin_sel.find(sat);
            if (prev != last_if_pin_sel.end() && prev->second != sel) {
                const auto change = changes.find(sat);
                const bool continued = change != changes.end()
                                    && change->second.outcome == PinOutcome::Continued;
                const bool cont = continued && change->second.from == prev->second
                               && change->second.to == sel
                               && !this_epoch_cb_reseed.count(sat)
                               && !cb_reseed_disc_pending.count(sat);
                if (cont) {
                    ++ctx.counters.n_if_pin_continued_undeclared;
                } else {
                    mark.insert(sat);
                    if (continued) ++ctx.counters.n_if_pin_continued_declared;
                }
                if (events_trace.is_open())
                    emitEvent("CLK_BASIS", cont ? "reselect_continued" : "reselect", sat, "",
                              std::nullopt, std::nullopt,
                              cont ? "if_pin_reference_continued" : "if_pin_reference_moved",
                              prev->second.first + "+" + prev->second.second +
                                  "->" + sel.first + "+" + sel.second);
            }
            last_if_pin_sel[sat] = sel;   // advance the emit baseline (emitted sats only)
        }
        if (!mark.empty()) {
            // Dedupe against the blocks above (one event, one step) and serialize every
            // landed mark (Q3 trap-1).  On the clock channel this mark is required: the
            // clock disc counter has only datum-change components (no magnitude jump
            // test, RTA-11), so without the R record a replay consumer integrates across
            // the emitted δtˢ↔dˢ re-split.  Replay does not re-run this pair comparison;
            // the R record is the reconstruction, so live and replay counters agree.
            std::set<std::string> fresh;
            for (const auto& s : mark)
                if (!retro_emit_sats.count(s)) fresh.insert(s);
            // Full scope (Card MS): the IF-pin re-selection re-references the emitted
            // dt^s <-> d^s split by the satellite's inter-channel bias, the same
            // three-channel re-split as the re-latch above.
            if (!fresh.empty())
                emitter.markSatDiscontinuity(fresh, NetworkProductEmitter::kDiscChanAll);
            retro_emit_sats.insert(mark.begin(), mark.end());
        }
    }
    // Land the time-linked datum re-seed disc bumps (Card 2,
    // network.code_datum_time_linked): a sum-to-zero channel with no veteran, or an
    // IF-pin split forced to the 0 seed, stepped the emitted value, so the consumer
    // must reset instead of integrating across an unsignalled step.
    // this_epoch_cb_reseed (built in the main pass) is merged into the persistent
    // pending set here, at the emit site, which every whole-epoch withhold path skips
    // with `continue`.  A transient re-seed on an epoch that was then withheld never
    // enters the pending set (no bump without an emitted step), while a re-seed the
    // driver did realize is kept and deferred like the eph-cutover and basis-relatch
    // bumps: a re-seeded satellite absent from this emission (pruned by the quarantine
    // or the per-constellation withhold) waits for its next actual emit, so the bump
    // lands with the satellite present, and the landed mark folds into retro_emit_sats
    // (Q3 trap-1) so the R record carries it to a replay.  Both sets are filled only
    // under code_datum_time_linked, so otherwise the block does nothing and the output
    // is byte-identical.
    for (const auto& sat : this_epoch_cb_reseed) cb_reseed_disc_pending.insert(sat);
    if (!cb_reseed_disc_pending.empty()) {
        std::set<std::string> present, mark, still_pending;
        for (const auto& s : sol.sats) present.insert(s.sat);
        for (const auto& sat : cb_reseed_disc_pending)
            (present.count(sat) ? mark : still_pending).insert(sat);
        cb_reseed_disc_pending = std::move(still_pending);
        if (!mark.empty()) {
            // Dedupe against the blocks above (one event, one step) and serialize every
            // landed mark, since the counter-only path is invisible to replay (see the
            // eph-cutover block).
            std::set<std::string> fresh;
            for (const auto& s : mark)
                if (!retro_emit_sats.count(s)) fresh.insert(s);
            // Code and clock channels (Card MS): after a sum-to-zero code channel with no
            // veteran, or an IF-pin split forced to the 0 seed, the emitted code bias
            // re-seeds; under the IF-pin convention the satellite clock is defined against
            // that same pinned pair, so the clock is re-referenced with it.  The phase
            // bias has its own datum and is left to its own jump test.
            if (!fresh.empty())
                emitter.markSatDiscontinuity(fresh,
                                             NetworkProductEmitter::kDiscChanCodeBias
                                                 | NetworkProductEmitter::kDiscChanClock);
            retro_emit_sats.insert(mark.begin(), mark.end());
            // Count the re-seed discs that land on the first post-resume epoch (RS-3,
            // SPEC_event_harness §21.7): a direct readout of whether the restored
            // code-bias memory kept the datum.  The count comes from `mark` (what
            // landed), not from this_epoch_cb_reseed, so a deferred bump is attributed
            // to the epoch at which the consumer sees it.
            if (ctx.carry.resume_datum_first_ep)
                ctx.counters.n_resume_datum_first_cb_disc +=
                    static_cast<long long>(mark.size());
            if (events_trace.is_open())
                for (const auto& s : mark)
                    emitEvent("CB_DATUM", "reseed_disc_landed", s, "", std::nullopt,
                              std::nullopt, "disc_bump", "code_datum_time_linked_reseed");
        }
    }
    // The handover-detection baseline advances only here, together with the bump it
    // pairs with: prev_pivot and last_pivot describe the datum of the last emitted
    // product.  A handover epoch that never reaches this point (gate withhold, solve
    // failure, σ̂0 withhold) leaves the baseline untouched, so the change is detected
    // again, and the old-datum carries are skipped again, on every epoch until one
    // emits in the new datum.
    // networkMultiGnssNotePivots records the realized clock pivots per constellation
    // at this one site where the handover baseline advances, before last_pivot is
    // merged, so a handover is counted between emitted products (Card GAL).  It
    // returns on its first line unless a constellation other than GPS is active.
    networkMultiGnssNotePivots(ctx, config_, plan.clock_pivot, last_pivot);
    prev_pivot = plan.clock_pivot;
    // Merge-update the persistent last-ever pivot map without erasing any
    // constellation, so a later vanish-return with a different pivot is caught (A1).
    for (const auto& kv : plan.clock_pivot) last_pivot[kv.first] = kv.second;
    // Prioritized phase-bias anchor: record per-(sat, signal) anchor handovers for
    // observability and advance the emit baseline (emitted_anchor).  The
    // consumer-facing discontinuity is already handled, replay-safely, by the
    // emitter's per-(sat, signal) bias-jump test: a handover steps the emitted bias by
    // a different receiver's integer ambiguity lump (≥ ~1 cycle, well above
    // disc_threshold_m), so emitToStore in SB_EMITSTORE bumps that key's counter and
    // the consumer resets the ambiguity.  No separate markSatDiscontinuity is issued
    // here by design: it could not be reconstructed from the serialized
    // NetworkSolutionEpoch (replay does not re-run the election), so the product's
    // disc counters would diverge on NetworkProductIO replay.  The baseline advances
    // only at emit (a withheld epoch re-detects at the next emit) and never erases an
    // absent key (the last_pivot policy).
    if (phase_datum_prioritized) {
        int n_handover = 0;
        for (const auto& [amb, sta] : this_epoch_anchor) {
            auto it = emitted_anchor.find(amb);
            if (it != emitted_anchor.end() && it->second != sta) {
                const auto amb_parts = StateKeys::parseAmbiguityKey(amb);   // "G01_L1C"
                // Under eq34 (Card 3) the handover is a tree-edge reattach: the emitted
                // δ̃_s steps by an exact integer number of cycles (the chain path
                // difference; the receiver fractional part does not leak), usually 0
                // under pin-to-round.  The discontinuity still goes through the
                // emitter's replay-safe bias-jump test, never markSatDiscontinuity
                // (not reconstructable on NetworkProductIO replay).  Trace event only.
                //
                // The step is an integer only if the new host's ambiguity has
                // converged.  On a one-epoch-old arc the float is code-seeded (metres
                // of error), so the step is an arbitrary re-level of the whole
                // component (the DOY-129 19:25:30 event).  The guard against it sits in
                // the election, not here: docs/SPEC_eq34_reattach_hygiene.md,
                // filter/NetworkAnchorHygiene.hpp (network.eq34_anchor_min_arc_ep).
                // With the guard armed this event fires only for a host whose arc has
                // passed the age threshold; a refused host is traced as
                // PHASE_DATUM/reattach_blocked_cold instead.
                emitEvent(phase_datum_eq34 ? "PHASE_DATUM" : "PHASE_ANCHOR",
                          phase_datum_eq34 ? "eq34_reattach" : "anchor_handover",
                          amb_parts ? amb_parts->satellite : amb, sta,
                          std::nullopt, std::nullopt, "reanchor",
                          it->second + "->" + sta);
                ++n_handover;
            }
            emitted_anchor[amb] = sta;
            // eq34 reattach hygiene, warm conditioning: stamp the time this key's datum
            // was last realized and emitted.  The eligibility block applies only while
            // that memory is fresh (see filter/NetworkAnchorHygiene.hpp).  The stamp
            // follows the emit-only discipline of the baseline above: a withheld epoch
            // must not refresh it, or a long withhold would look warm indefinitely.
            if (anchor_hygiene_on)
                ctx.carry.emitted_anchor_s[amb] =
                    TimeUtils::continuousGpsSeconds(se.time);
        }
        if (n_handover > 0)
            log->debug("Network: {} per-(sat,signal) phase-bias anchor handover(s) at "
                       "TOW {:.1f} (disc handled by the emitter bias-jump test).",
                       n_handover, se.time.tow);
        // Count the tree-edge reattaches on the first post-resume epoch (RS-3,
        // SPEC_event_harness §21.7), the second half of the eq34 churn readout (rerefs
        // are the frame steps, reattaches the host changes).
        if (ctx.carry.resume_datum_first_ep)
            ctx.counters.n_resume_datum_first_reattach += n_handover;
    }
    // Close the first-post-resume measurement window (RS-3).  This is the last of its
    // three sites in epoch order and the point at which an emission has certainly
    // happened.  A withheld epoch `continue`s long before this, so the window extends
    // to the first emitted epoch rather than the first processed one, which is the
    // right boundary for figures graded at the consumer.  Telemetry only.
    ctx.carry.resume_datum_first_ep = false;
    // Code-bias datum declaration (§1.5): refresh the realized code-bias datum (cbpin=)
    // on every emit, not once at the first, the same discipline the realized clock
    // pivots follow (NetworkEmitStore refreshes clock_pivot_sat / pdr= in place before
    // each RTA-5 re-broadcast).  A declaration
    // stamped once would go stale on the wire after a channel-set shift moved the pin,
    // which is the silent mis-framing the token exists to prevent.  The maps are built
    // fresh and assigned whole, so a constellation that stops being unanimous drops its
    // declaration at the next broadcast instead of keeping the last good one: a missing
    // declaration is safe for a consumer, a stale pair is not.
    {
        // Declare the realized code-bias IF pin pair per constellation (the §1.5 fill).
        // The S-line `cbpin=` field, its parser and its contract comparison come from
        // B6; this block fills the map.  The declaration matters: the emitted OSB stream
        // is internally consistent only within selectIfPinPair's realized pair, a
        // consumer mixing frames pays the §1.5 mixed-pair penalty, and the truth-graded
        // fixture cannot apply the §4.4 S-transform without it (the grader refuses
        // rather than assume (C1C,C2W)).
        //
        // A pair is declared only when this epoch's emitted satellites of a
        // constellation agree on it unanimously.  A per-satellite split is real (signal
        // sets differ per satellite) and a per-constellation field cannot describe it,
        // so nothing is declared and the split is logged when the declaration changes,
        // instead of publishing a pair that holds for only part of the stream.  With an
        // empty map the S line is written byte-for-byte as before, so a producer that
        // cannot declare a pin is unchanged.
        //
        // The source is this_epoch_if_pin_real, the unconditional declaration record,
        // not this_epoch_if_pin_sel.  The latter is the disc-compare scratch, gated on
        // emit_clock_disc, so reading it would tie the declaration to a QC-channel gate:
        // emit_clock_disc defaults to false, and a run without it would declare no
        // cbpin.  Declaration only: the solve and every emitted value are unchanged.
        //
        // This producer declares no gfpin= (the wire field NetworkProductContract::
        // gf_pin_pair stays in the contract, the S-line writer and the parser for
        // products written with the retired network.code_bias_gf_datum, and is left
        // empty here).
        std::map<char, std::set<std::pair<std::string, std::string>>> by_const;
        std::set<std::string> emitted_sats;
        for (const auto& s : sol.sats) emitted_sats.insert(s.sat);
        // Card GFC: the constellations with a satellite emitted on a held pin pair
        // (network.code_datum_pair_continuity), so that a split that drops the cbpin=
        // declaration is attributed to the hold when one took part.  Empty unless armed.
        std::map<char, System> held_const;
        for (const auto& [sat, sel] : this_epoch_if_pin_real)
            if (!sat.empty() && emitted_sats.count(sat)) {
                const char cons = letterOfSat(sat);
                by_const[cons].insert(sel);
                if (ctx.carry.this_epoch_if_pin_holds.count(sat))
                    held_const.emplace(cons, systemOfSat(sat));
            }
        std::map<std::string, std::string> cb_decl;
        std::string split_note;
        for (const auto& [cst, pairs] : by_const) {
            if (pairs.size() != 1) {
                split_note += fmt::format("{}{}:{} IF-pin pairs", split_note.empty() ? "" : " ",
                                          cst, pairs.size());
                if (const auto h = held_const.find(cst); h != held_const.end())
                    ++ctx.counters.cb_pair_by_const[h->second].hold_split_decl;
                continue;
            }
            const auto& p = *pairs.begin();
            cb_decl[std::string(1, cst)] = p.first + "-" + p.second;
        }
        // Log only on a change: this runs every emit, and restating a stable datum each
        // epoch is noise.  A change is worth a line because it means the pinned pair
        // moved, which re-references the emitted clock↔code split.
        if (cb_decl != pending_contract.cb_pin_pair) {
            std::string cd;
            for (const auto& [c, p] : cb_decl) cd += (cd.empty() ? "" : " ") + c + ":" + p;
            log->info("Network (S-contract §1.5): declaring code-bias datum cbpin=[{}]{}{}.  "
                      "cbpin is the frame the emitted OSB stream is internally consistent in.",
                      cd.empty() ? "-" : cd,
                      split_note.empty() ? "" : "; NOT declared (per-satellite split): ",
                      split_note);
        }
        pending_contract.cb_pin_pair = std::move(cb_decl);
        // Declare the value the hard per-satellite geometry-free datum was pinned at
        // (RF-0d).  It follows the config, not an epoch's realized rows: unlike cbpin= it
        // is not a per-epoch selection that can move with the signal set, the mode is
        // fixed for the run, and a token that appeared and vanished with row
        // availability would read as a datum change that did not happen.  The
        // field is empty in the default mode, so the S line is byte-identical to that of
        // a product written before RF-0d.
        switch (config_.getNetworkConfig().sat_gf_datum) {
        case NetworkSatGfDatum::Zero:
            pending_contract.sat_gf_datum = "zero";           break;
        case NetworkSatGfDatum::BroadcastTgd:
            pending_contract.sat_gf_datum = "broadcast_tgd";  break;
        case NetworkSatGfDatum::NoneMode:
            pending_contract.sat_gf_datum.clear();            break;
        }
    }
    // Open the deferred contract writer at the first emit and stamp the realized
    // per-constellation clock pivots (this epoch's plan) into the v2 header (B6, P1-1).
    if (contract_pending) {
        contract_pending = false;
        for (const auto& [cst, sat] : plan.clock_pivot)
            pending_contract.decl.clock_pivot_sat[cst] = sat;   // realized auto-picked pivots
        // Stamp the realized datum receiver r1 as the diagnostic pdr= token, the
        // component-0 representative (Card 3, SingleReceiverChain).
        if (phase_datum_eq34 && !eq34_datum_rx.empty()) {
            pending_contract.decl.eq34_datum_receiver = eq34_datum_rx.begin()->second;
            eq34_emitted_pdr = pending_contract.decl.eq34_datum_receiver;
        }
        // The generation id (minted above at this first emit, or restored by a warm
        // resume) travels in the contract as an optional gid= field; 0 (unset) writes
        // nothing (RTA-3).
        pending_contract.generation_id = sol.generation_id;
        product_writer = std::make_unique<NetworkProductWriter>(prod_path.string(), pending_contract);
        if (product_writer->good()) {
            log->info("Network: product serialization open — v2 S-basis contract, "
                      "{} realized clock pivot(s).", pending_contract.decl.clock_pivot_sat.size());
            last_contract_emit_s = pl_now_s;   // the header S line is the first broadcast (RTA-5)
            // Flush the buffered heartbeats of every withheld epoch that preceded this
            // open, in tow order (all earlier than this epoch by construction), so a
            // leg that opens inside a withhold era still covers that era on the wire
            // (RTA-10 gap fix).
            if (!setup.pending_h_pre_open.empty()) {
                for (const auto& hst : setup.pending_h_pre_open)
                    product_writer->writeEpochStatus(hst);
                log->info("Network (RTA-10): flushed {} buffered withheld-epoch "
                          "heartbeat(s) that preceded the writer open — the wire now "
                          "covers the pre-emit withhold era.",
                          setup.pending_h_pre_open.size());
                setup.pending_h_pre_open.clear();
                setup.pending_h_pre_open.shrink_to_fit();
            }
        } else {
            log->error("Network: failed to open product_output_path '{}' for writing; "
                       "disabling on-disk serialization for this run.", prod_path.string());
            product_writer.reset();
        }
    }
    // Per-key combined-correction σ (sigc for phase, sigcc for code) and the
    // code/phase correlation (rho), from the cross-terms of the shared posterior
    // covariance (M4, contract v2).  The individual clock, phase-bias and code-bias
    // formal σ (the URA and B1b channels) are cm-level while a satellite reconverges
    // after rising, yet the combined correction the user applies (clock + bias) is
    // mm-level because the errors are strongly correlated (Zhang 2021 Figs. 2–5;
    // Psychas 2024b Eq. 12).  A consumer weighting by sigc/sigcc keeps using a fresh
    // satellite's products through reinitialization instead of deweighting them
    // 10–100×.  The ionosphere is excluded: the network ionosphere is a global SH-VTEC
    // field, not a co-resident column, and is supplied separately via SsrSlantIono
    // with its own σ, so including it here would count it twice.  The covariance
    // handling matches B1b (gauge-nulled copy, datum-pin skip, positive-variance
    // guard); a folded or zeroed phase gauge leaves no usable phase
    // column and so no token.  Scaled by the same σ̂0 EWMA, which cancels in rho.  Off:
    // every value is 0, no wire token is written, and the product and store are
    // byte-identical.
    if (config_.networkEmitCombinedSigma()) {
        Eigen::MatrixXd cbcov = est.sharedCovariance();   // copy; treat gauges as exact
        est.zeroPhaseGaugeColumns(cbcov);
        std::set<Eigen::Index> cb_pinned;
        for (const auto& pin : plan.shared_datum) cb_pinned.insert(pin.column);
        const double s0 = std::max(1.0, ura_sigma0_ewma);
        // A column is usable only when present, in range, not a datum pin (its
        // 1/pin_scale variance is a gauge artefact), and carrying a finite positive
        // variance (which rejects a zeroed phase gauge).
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
            if (c < 0) continue;   // no usable clock column ⇒ nothing to combine
            // Per-signal combined phase σ (sigc) for every emitted phase bias.
            for (auto& [signal, npb] : s.phase_bias) {
                const Eigen::Index p =
                    colOrNeg([&]{ return est.phaseBiasColumn(s.sat, signal); });
                const auto cc = computeCombinedCorrectionSigma(cbcov, c, p, -1, s0);
                if (cc.sigc > 0.0) npb.sigc_m = cc.sigc;
            }
            // Per-satellite combined code σ (sigcc) and code/phase correlation (rho) at
            // a reference signal: the first code signal whose phase sibling is present
            // (so rho pairs a real code/phase observable), else the first code signal
            // (rho then absent).  Code and phase biases use disjoint key namespaces
            // (code "C1W", phase "L1W"), so the pairing needs the RINEX obs-type swap
            // that selectCombinedCodeRef performs; plain key equality never matches.
            // The C record is per satellite and the combined code σ is clock-dominated,
            // so this per-satellite summary stands in for all of the satellite's code
            // signals (a documented approximation).
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

    // --- Satellite bias-state trace (L14, network.trace_sat_bias_states) -------------
    // Closes the one export gap of the truth-graded synthetic fixture
    // (docs/SPEC_synthetic_minimum_network.md §4.8): the B product record's legend is
    // `# B,sat,signal,code_bias_m[,code_disc]`, with no sigma field in any
    // configuration, so the code-bias variance factor (vf) cannot be formed from the
    // product.  A `sigma=` token in the B record would change the product grammar and
    // any consumer fingerprint keyed on the header legend, so this is a trace instead.
    //
    // The covariance treatment is the one the URA, phase-bias σ (B1b) and combined-σ
    // (M4) channels above use: datum-pin skip, positive-variance guard and σ̂0 EWMA
    // scaling, with the phase gauges nulled as in B1b and M4.  A vf computed from this
    // file is therefore comparable with one computed from the U record rather than
    // following a second, slightly different convention.  s0 is written per row so a
    // grader can undo the scaling.
    //
    // A datum-pinned column is reported with an empty sigma_m and pinned=1: its
    // 1/pin_scale variance is a gauge artefact, and writing that tiny number would
    // produce an enormous variance factor from the datum choice alone.  Blank means
    // "not emitted", never 0, the convention network_zwd.csv and the U record use,
    // which lets the grader tell the two apart.
    if (traces.sat_bias_states_trace.is_open()) {
        Eigen::MatrixXd bcov = est.sharedCovariance();   // copy; treat gauges as exact
        est.zeroPhaseGaugeColumns(bcov);
        std::set<Eigen::Index> pinned_cols;
        for (const auto& pin : plan.shared_datum) pinned_cols.insert(pin.column);
        const double s0 = std::max(1.0, ura_sigma0_ewma);
        for (const auto& s : sol.sats) {
            for (const auto& [signal, bias_m] : s.code_bias_m) {
                Eigen::Index col = -1;
                try { col = est.codeBiasColumn(s.sat, signal); } catch (...) { col = -1; }
                const bool in_range =
                    col >= 0 && col < bcov.rows() && col < bcov.cols();
                const bool is_pinned = in_range && pinned_cols.count(col) > 0;
                const double var = in_range ? bcov(col, col) : -1.0;
                const bool have_sigma =
                    in_range && !is_pinned && var > 0.0 && std::isfinite(var);
                traces.sat_bias_states_trace
                    << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                    << s.sat << ',' << signal << ','
                    << fmt::format("{:.6f}", bias_m) << ',';
                if (have_sigma)
                    traces.sat_bias_states_trace
                        << fmt::format("{:.6e}", s0 * std::sqrt(var));
                traces.sat_bias_states_trace
                    << ',' << fmt::format("{:.4f}", s0) << ','
                    << (is_pinned ? 1 : 0) << '\n';
            }
        }
    }

    return { std::move(sol), std::move(epoch_withheld_consts), global_bump,
             std::move(serialized_consts), std::move(retro_emit_sats) };
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the clock-runaway quarantine (RT0-5).
int networkClockRunawayRunEnd(const NetworkRunEndView& v) {
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const bool clk_runaway_on = v.ctx.knobs.clk_runaway_on;
    const double clk_runaway_mag = v.ctx.knobs.clk_runaway_mag;
    const double clk_runaway_rate = v.ctx.knobs.clk_runaway_rate;
    const auto& n_runaway_epochs = v.ctx.counters.n_runaway_epochs;

    if (clk_runaway_on && n_runaway_epochs > 0)
        log->warn("Network (RT0-5): {} epoch(s) dropped at least one constellation for clock "
                  "runaway (magnitude ceiling {:.0f} m / rate ceiling {:.2f} m/s).",
                  n_runaway_epochs, clk_runaway_mag, clk_runaway_rate);
    return 0;
}

// Run-end report: the per-constellation sigma0 carve-out (RTA-10).
int networkPerConstellationRunEnd(const NetworkRunEndView& v) {
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const bool perconst_on = v.ctx.knobs.perconst_on;
    const auto& n_perconst_partial_epochs = v.ctx.counters.n_perconst_partial_epochs;
    const auto& n_perconst_diffuse_published = v.ctx.counters.n_perconst_diffuse_published;

    if (perconst_on && (n_perconst_partial_epochs > 0 || n_perconst_diffuse_published > 0))
        log->info("Network (RTA-10): σ̂0 carve-out — {} epoch(s) published PARTIALLY (>=1 "
                  "constellation pruned), {} DIFFUSE epoch(s) published in full (every judged "
                  "constellation individually clean while the global σ̂0 tripped).",
                  n_perconst_partial_epochs, n_perconst_diffuse_published);
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor

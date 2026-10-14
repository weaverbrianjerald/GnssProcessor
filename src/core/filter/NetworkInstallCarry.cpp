/**
 * @file NetworkInstallCarry.cpp
 * @brief Implementation of @ref networkInstallCarry, the SB_INSTALL block moved
 *        unchanged out of Application::runNetwork (decomposition INC-9b).  The body is
 *        the former if(carry_invariant) body; the alias preamble reconstructs
 *        nc/nmw/trace, the ctx.carry aliases and the wlAccumulate lambda so the code
 *        reads the original names.
 */

#include "filter/NetworkInstallCarry.hpp"

#include "app/NetworkRunContext.hpp"     // ctx.carry / ctx.counters
#include "config/GnssConfig.hpp"         // config_.getNetworkConfig() / .mw_wl
#include "filter/StateKeySchema.hpp"     // StateKeys::parse*
#include "slip/NetworkRawObservables.hpp"// SlipQcChannel
#include "logging/NetworkRunTraces.hpp"  // traces.trace

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

void networkInstallCarry(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkInvariantCarryDriver& carry_driver,
    const NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    const SyncedEpoch& se,
    const std::vector<NetStation>& stations,
    SlipQcChannel& slip_qc,
    const std::map<int, std::map<ArcKey, std::string>>& carry_arc_amb_key,
    bool carry_par,
    int n_threads,
    int seam_threads,
    NetworkLaneRun& lane_run) {
    // --- Alias preamble: reconstructs the run-TU aliases the relocated
    //     SB_INSTALL body reads (nc/nmw/trace, the ctx.carry aliases, and the
    //     wlAccumulate lambda).  Copies of the former runNetwork() lines. ----
    const auto& nc  = config_.getNetworkConfig();
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    auto& trace = traces.trace;
    // The lane switches (NetworkKnobs) and the lane machinery (NetworkLaneRun), under their
    // former parameter names (card NOM step TD1a).
    const bool carry_innov_gate = ctx.knobs.carry_innov_gate;
    const bool ewl_on = ctx.knobs.ewl_on;
    const bool uwl_on = ctx.knobs.uwl_on;
    const bool wl_inject_on = ctx.knobs.wl_inject_on;
    const bool mw_d5_dump = ctx.knobs.mw_d5_dump;
    const bool mw_lane_trace = ctx.knobs.mw_lane_trace;
    const bool mw_tagsets = ctx.knobs.mw_tagsets;
    std::vector<EwlLane>& ewl_lanes = lane_run.ewl_lanes;
    std::vector<WlInjectPlan>& wl_inject_epoch = lane_run.wl_inject_epoch;
    std::vector<LaneTraceEntry>& lane_trace_epoch = lane_run.lane_trace_epoch;
    WlCounters& wl_run = lane_run.wl_run;
    WlCounters& wl_epoch = lane_run.wl_epoch;
    std::map<char, WlCounters>& wl_cons_run = lane_run.wl_cons_run;
    std::map<char, WlCounters>& wl_cons_epoch = lane_run.wl_cons_epoch;
    auto& d5_ewl_stasat = ctx.carry.d5_ewl_stasat;
    auto& d5_hold_chords = ctx.carry.d5_hold_chords;
    auto& d5_inject_chords_prev = ctx.carry.d5_inject_chords_prev;
    auto& d5_inject_chords_now = ctx.carry.d5_inject_chords_now;
    auto& carry_prev_tow = ctx.carry.carry_prev_tow;
    auto& carry_epochs = ctx.carry.carry_epochs;
    auto& carry_born_total = ctx.carry.carry_born_total;
    auto& carry_marg_total = ctx.carry.carry_marg_total;
    auto& carry_merges_total = ctx.carry.carry_merges_total;
    auto& carry_splits_total = ctx.carry.carry_splits_total;
    auto& carry_swaps_total = ctx.carry.carry_swaps_total;
    auto& carry_zdim_last = ctx.carry.carry_zdim_last;
    auto& carry_slipkills_total = ctx.carry.carry_slipkills_total;
    auto& carry_dropped_total = ctx.carry.carry_dropped_total;
    auto& carry_innov_pending = ctx.carry.carry_innov_pending;
    auto& carry_innov_flags_total = ctx.carry.carry_innov_flags_total;
    auto& carry_innov_retaps_total = ctx.carry.carry_innov_retaps_total;
    auto& carry_innov_map_miss = ctx.carry.carry_innov_map_miss;
    // Tag corroborated-flat flags so next epoch's carry-membership test can spare
    // them.  The loader already forces the knob false without innov_gate (Card 4 D5 v4).
    const bool retap_only =
        carry_innov_gate && config_.networkCarryInvariantInnovRetapOnly();
    auto wlAccumulate = [](WlCounters& a, const WlCounters& b) {
        a.rows_seen           += b.rows_seen;
        a.cycles_decided      += b.cycles_decided;
        a.holds_folded        += b.holds_folded;
        a.injects             += b.injects;
        a.inject_abs_sum      += b.inject_abs_sum;
        a.skip_bridge_missing += b.skip_bridge_missing;
        a.skip_no_iono        += b.skip_no_iono;
        a.skip_bridge_wide    += b.skip_bridge_wide;
        a.skip_not_fixed      += b.skip_not_fixed;
        a.skip_closure        += b.skip_closure;
        a.skip_quarantine     += b.skip_quarantine;
        a.skip_basis          += b.skip_basis;
        a.sum_bridge_R        += b.sum_bridge_R;
        a.cold_bridge_cycles  += b.cold_bridge_cycles;
        a.mid_decided         += b.mid_decided;
        a.mid_fallback        += b.mid_fallback;
        a.mid_closure         += b.mid_closure;
        a.mid_disagree        += b.mid_disagree;
        a.top2_decided        += b.top2_decided;
        a.dual_dedupe         += b.dual_dedupe;
        a.sib_checked         += b.sib_checked;
        a.sib_unavail         += b.sib_unavail;
        a.sib_int_off         += b.sib_int_off;
        a.sib_blocked         += b.sib_blocked;
        a.sib_abs_frac_sum    += b.sib_abs_frac_sum;
    };

    // Hand the driver this epoch's corroboration set before the install consumes it: the
    // chords whose underlying arc the raw MW/GF observables measured as continuous.  Built
    // here because this seam holds all three inputs at once: slip_qc (the raw verdicts,
    // computed in PASS A before the solve), carry_arc_amb_key (the group→chord→ambiguity
    // reverse map rebuilt with the blocks), and carry_driver (Card 4 D5 v3).
    //
    // Membership requires "measured and flat": verdict.measured is true and the
    // (station, satellite) is absent from epoch.stepped.  An unmeasured arc is left out so
    // it still re-taps; suppressing on absence of evidence would disable the fine-slip
    // response on the arcs with the least information (standing invariant #1).  This is
    // the same tri-state the network_d5_flags.csv `raw_stepped` column encodes, so the
    // trace and the gate agree.
    if (carry_innov_gate && config_.networkCarryInvariantInnovCorroborate()
        && slip_qc.epoch.computed) {
        std::set<std::pair<int, ArcKey>> flat;
        for (const auto& [group, by_chord] : carry_arc_amb_key) {
            for (const auto& [chord, amb] : by_chord) {
                if (amb.empty() || chord.u < 0
                    || static_cast<std::size_t>(chord.u) >= slip_qc.epoch.verdict.size())
                    continue;
                const auto parts = StateKeys::parseAmbiguityKey(amb);
                if (!parts) continue;
                const std::string& sat = parts->satellite;
                if (slip_qc.epoch.stepped.count({ chord.u, sat })) continue;  // real step ⇒ re-tap
                const auto& vmap = slip_qc.epoch.verdict[static_cast<std::size_t>(chord.u)];
                const auto vit = vmap.find(sat);
                if (vit != vmap.end() && vit->second.measured)
                    flat.insert({ group, chord });   // measured and flat ⇒ suppressible
            }
        }
        carry_driver.setInnovFlatChords(std::move(flat));
    }

    // Post-resume D5 warm-up (RS-2b; network.resume_d5_warmup_ep, SPEC_event_harness
    // §21.6): for the first K processed epochs after a warm resume the D5 gate is
    // diagnostic-only.  The window is counted here, at the gate's only site, so a run
    // that never installs a posterior never consumes it, as the MW/LLI drain warm-up
    // counts at its own dispatch site.  The retire, which re-arms the gate, is an edge in
    // the sense of invariant #7 and is logged.  The flag is set false on every other epoch, so on a run that never
    // resumes the gate is byte-identical.
    const bool d5_warmup_now = ctx.carry.resume_d5_warmup_left > 0;
    carry_driver.setInnovDiagnosticOnly(d5_warmup_now);

    // parallel_carry: thread installPosterior's posterior-cov GEMMs through Eigen, and run
    // the install serial over blocks (block thread count 1, its byte-identical-to-serial
    // path) so Eigen's threading does not nest with the seam's per-block OpenMP.
    try { CarryEigenScope es(carry_par, n_threads);
          carry_driver.installPosterior(est, x_s, plan.shared_datum,
                                        carry_par ? 1 : seam_threads); }
    catch (const std::exception& ex) {
        log->warn("Network[Card4]: DD-carry install failed at TOW {:.1f} ({}); "
                  "carry reset (refloats next epoch).", se.time.tow, ex.what());
        carry_driver.reset();
    }
    // Consume one epoch of the warm-up window and retire it when empty (RS-2b).  This
    // runs after the install because the retire line quotes d5_warmup_suppressed, which
    // this epoch's gate has just incremented; logged before the install, it would report
    // the previous epoch's count (invariant #4).  It sits outside the try/catch so a
    // failed install still consumes the epoch: the window counts processed epochs, not
    // successful ones.
    if (d5_warmup_now) {
        ++ctx.counters.n_resume_d5_warmup_epochs;
        if (--ctx.carry.resume_d5_warmup_left == 0)
            log->info("Network (RS-2b resume D5 warm-up): RETIRED at TOW {:.1f} — the D5 "
                      "innovation gate acted diagnostic-only for {} post-resume epoch(s) "
                      "and suppressed {} trip(s); it re-taps from the next epoch.",
                      se.time.tow, ctx.counters.n_resume_d5_warmup_epochs,
                      carry_driver.counters().d5_warmup_suppressed);
    }
    carry_prev_tow = se.time.tow;

    // === PD-safe invariant-DD fold (docs/SPEC_pd_safe_dd_fold.md) telemetry =======
    // Always on and solve-neutral (INC-0).  When Pᶻ+B is not PD, the fold responds with
    // `M := 0`: the block takes no data and its posterior equals its prior, and without
    // network.carry_invariant_pd_safe it stays frozen, while dd_sigma_cyc, the D5 gate
    // and the AR probe do not see the carry.  This block gives that response counters,
    // log lines and events.  It is read here rather than in the kernel because this seam
    // holds the accepted solve's fold, the block layout, the station roster and the trace
    // sinks at once.
    //
    // The kernel's stats are per fold, not cumulative: a within-epoch RAIM re-solve
    // re-folds and overwrites them, so the values read here are always the accepted
    // solve's screen.
    {
        const auto& fs  = est.ddFoldStats();
        auto&       cnt = ctx.counters;
        if (fs.blocks > 0) {
            ++cnt.ddfold_epochs;
            cnt.ddfold_folds     += fs.blocks;
            cnt.ddfold_fold_rows += fs.rows;
            cnt.ddfold_min_pivot_rel =
                std::min(cnt.ddfold_min_pivot_rel, fs.min_pivot_rel);
        }
        cnt.ddfold_degen_blocks += fs.degen_blocks;
        cnt.ddfold_degen_rows   += fs.degen_rows;
        if (fs.degen_blocks > 0) ++cnt.ddfold_degen_epochs;
        cnt.ddfold_repairs      += fs.repairs;
        cnt.ddfold_repair_rows  += fs.repaired_rows;
        cnt.ddfold_repair_worst  = std::max(cnt.ddfold_repair_worst, fs.repair_worst);
        // No per-epoch value is stored in ctx.carry.  The epoch-status columns are read
        // directly from est.ddFoldStats() at both writer sites (NetworkStashStatus for an
        // accepted epoch, NetworkSolveGlue for a withheld one); see the rationale on
        // NetworkCarryState.  This seam owns only the run-cumulative roll-up above, the
        // cross-epoch transition ledger below, and the events.

        // The eigenvalue repair is meant for rounding damage at the 1e-12 level.  A clamp
        // above 1e-6·λmax means the block was damaged by something else, a defect that
        // needs investigation, so it is reported instead of passing silently as a PD
        // matrix.  Warned once (the run-end summary carries the totals); the counter
        // holds the population (INC-1c).
        if (fs.repair_worst > 1e-6 && ++cnt.ddfold_repair_loud == 1)
            log->warn("Network (Card 4 PD-safe fold, INC-1c): a DD-fold eigenvalue REPAIR "
                      "clamped a magnitude of {:.3e}·λmax at TOW {:.1f} — far above the "
                      "1e-12 dust floor the repair exists for.  The block was not merely "
                      "rounding-damaged; treat this as a NEW defect and investigate rather "
                      "than trusting the repaired covariance.  (Further occurrences are "
                      "counted, not logged.)",
                      fs.repair_worst, se.time.tow);

        // Log both edges of a degenerate DD-prior block (invariant #7): entry, when
        // Pᶻ+B fails the PD screen and the block stops taking data, and exit, when
        // its key leaves the degenerate set.  An exit means the block factors again
        // or its component was re-formed; the key cannot tell which.  Logging only
        // transitions keeps a long outage to two lines instead of one per epoch.
        // Keys are (group, first chord): block indices change as components merge
        // and split, and the first chord is stable enough to follow a block.
        std::set<std::string> ddfold_degen_now;
        const std::size_t ddfold_nb =
            std::min(carry_driver.numBlocks(), est.numDdPriorBlocks());
        for (std::size_t b = 0; b < ddfold_nb; ++b) {
            if (!est.ddPriorBlockDegenerate(b)) continue;
            const ArcKey c0 = carry_driver.blockFirstChord(b);
            const int    grp = carry_driver.blockGroup(b);
            const std::string key = fmt::format("{}:{}-{}", grp, c0.u, c0.v);
            ddfold_degen_now.insert(key);
            if (ctx.carry.ddfold_degen_prev.count(key)) continue;   // already reported
            ++cnt.ddfold_transitions;
            const std::string sta =
                (c0.u >= 0 && c0.u < static_cast<int>(stations.size()))
                    ? stations[static_cast<std::size_t>(c0.u)].name : std::string{};
            const double piv = est.ddPriorBlockMinPivotRel(b);
            const long long kk =
                static_cast<long long>(carry_driver.blockChordsForTest(b).size());
            log->warn("Network (Card 4 PD-safe fold, INC-0): DD-prior block {} (group {}, "
                      "{} chord(s), lead chord {}→{}{}) went DEGENERATE at TOW {:.1f} — its "
                      "Pᶻ+B failed the PD screen, so the fold is INERT (M=0) and the block's "
                      "posterior IS its prior: it takes NO data this epoch.  Without "
                      "network.carry_invariant_pd_safe this is PERMANENT (the prior never "
                      "improves, so it never factors again).  min pivot d_min/d_max = {:.3e}.",
                      b, grp, kk, c0.u, c0.v,
                      sta.empty() ? std::string{} : fmt::format(" / {}", sta),
                      se.time.tow, piv);
            // The pivot is also written into `detail` in exponential form: emitEvent
            // renders the statistic column with {:.6f}, which would print a pivot such
            // as −2.9e-10 as "-0.000000".
            traces.emitEvent(se.time, "DD_FOLD", "DD_FOLD_DEGENERATE", "", sta, piv,
                             1e-12, "freeze",
                             fmt::format("block={} group={} k={} chord={}-{} min_pivot={:.6e}",
                                         b, grp, kk, c0.u, c0.v, piv));
        }
        for (const std::string& key : ctx.carry.ddfold_degen_prev) {
            if (ddfold_degen_now.count(key)) continue;
            ++cnt.ddfold_recoveries;
            log->warn("Network (Card 4 PD-safe fold, INC-0): DD-prior block {} RECOVERED at "
                      "TOW {:.1f} — it factors again (or its component was re-formed).  The "
                      "re-coupling edge is reported for the same reason the decoupling one "
                      "is: a block that silently rejoins the solve after an unbounded frozen "
                      "era is invariant #7's exact failure mode.",
                      key, se.time.tow);
            traces.emitEvent(se.time, "DD_FOLD", "DD_FOLD_RECOVERED", "", "",
                             std::nullopt, std::nullopt, "recouple",
                             fmt::format("block_key={}", key));
        }
        ctx.carry.ddfold_degen_prev.swap(ddfold_degen_now);

        // Bounded, logged re-coupling: a block that could not be repaired is rebirthed
        // (means kept, Pᶻ re-seeded diffuse) instead of frozen.  The list is empty unless
        // network.carry_invariant_pd_safe is on (INC-2a).
        for (const auto& rb : carry_driver.lastFoldRebirths()) {
            ++cnt.ddfold_rebirths;
            cnt.ddfold_rebirth_rows += rb.k;
            const std::string sta =
                (rb.chord0.u >= 0 && rb.chord0.u < static_cast<int>(stations.size()))
                    ? stations[static_cast<std::size_t>(rb.chord0.u)].name : std::string{};
            log->warn("Network (Card 4 PD-safe fold, INC-2a): DD-prior block {} (group {}, "
                      "{} carried row(s), lead chord {}→{}{}) REBIRTHED at TOW {:.1f} — its "
                      "Pᶻ+B could not be factored even after the eigenvalue repair, so the "
                      "means were kept as the seed and Pᶻ was reset to the diffuse "
                      "fresh-chord prior.  ONE bounded epoch of information lost (the block "
                      "takes its data-only tap-off next epoch), which is the whole point: "
                      "the alternative is a silent permanent freeze.  min pivot = {:.3e}.",
                      rb.block, rb.group, rb.k, rb.chord0.u, rb.chord0.v,
                      sta.empty() ? std::string{} : fmt::format(" / {}", sta),
                      se.time.tow, rb.min_pivot_rel);
            traces.emitEvent(se.time, "DD_FOLD", "DD_FOLD_REBIRTH", "", sta,
                             rb.min_pivot_rel, 1e-12, "rebirth",
                             fmt::format("block={} group={} k={} chord={}-{} min_pivot={:.6e}",
                                         rb.block, rb.group, rb.k,
                                         rb.chord0.u, rb.chord0.v, rb.min_pivot_rel));
        }
    }

    // Clamp state of the calibrated σ̂₀ of the D5 gate (D5 v2).  The gate's threshold is
    // k·σ̂₀, and σ̂₀ reports 1.00× in four distinct states, three of which mean the gate
    // has fallen back to the raw formal (v1) test without any other sign.  Log the
    // transition, not the state, as for the degenerate-block ledger above: one line per
    // change instead of one per epoch.
    {
        using Infl = NetworkInvariantCarryDriver::InflationState;
        const auto  st  = carry_driver.innovInflationStatus();
        const char* tag = NetworkInvariantCarryDriver::inflationStateName(st.state);
        // `Off` is not announced: calibration was never armed, and every non-D5 network
        // run would open with the line.  The counters still tally it, so the run-end
        // summary can distinguish "v1 by config" from "v2 that never engaged".
        if (st.state != Infl::Off && ctx.carry.d5_calib_state_prev != tag) {
            // Only the degraded states (Floored, Capped) warn.  Warm-up is expected and
            // clears itself at the start of every calibrated run, so a warning there would
            // appear on epoch 1 of every run.  A run that never leaves warm-up is caught
            // by the run-end summary instead.
            const bool degraded = (st.state == Infl::Floored || st.state == Infl::Capped);
            const std::string from = ctx.carry.d5_calib_state_prev.empty()
                ? std::string{"(start)"} : ctx.carry.d5_calib_state_prev;
            const char* meaning =
                (st.state == Infl::Off)     ? "calibration is not armed — the gate is the raw formal (v1) test by config"
              : (st.state == Infl::WarmUp)  ? "armed but still below the warm-up row count — the gate is running as RAW FORMAL (v1)"
              : (st.state == Infl::Floored) ? "armed and warm, but the RAW σ̂₀² is BELOW 1 and the floor clamp bound it up — the gate is running as RAW FORMAL (v1) while reporting 1.00×"
              : (st.state == Infl::Capped)  ? "the RAW σ̂₀² exceeded the divergence cap and was bound DOWN — the gate is pinned, not calibrated"
                                            : "calibrated: σ̂₀ is the unclamped empirical estimate";
            const std::string line = fmt::format(
                "Network (Card 4/M1 D5 v2 calibration): σ̂₀ state {} → {} at TOW {:.1f} — {}.  "
                "raw σ̂₀² = {:.4g}, n_eff = {:.0f}, σ̂₀ in force = {:.2f}×.",
                from, tag, se.time.tow, meaning, st.raw_sigma0_sq, st.n_eff, st.sigma0);
            if (degraded) log->warn("{}", line); else log->info("{}", line);
            ctx.carry.d5_calib_state_prev = tag;
        }
    }

    // EWL lane (M5 card WL-2): the same accepted-install accumulation (invariant #4),
    // per lane instance (under UWL-1, one per tabled constellation when armed).
    if (ewl_on) {
        for (auto& lane : ewl_lanes) {
            lane.run.cycles_seen       += lane.epoch.cycles_seen;
            lane.run.rows_built        += lane.epoch.rows_built;
            lane.run.rows_fixed        += lane.epoch.rows_fixed;
            lane.run.rows_fixed_cold   += lane.epoch.rows_fixed_cold;
            lane.run.rows_cold_unfixed += lane.epoch.rows_cold_unfixed;
            lane.run.rows_capped       += lane.epoch.rows_capped;
            lane.run.skip_not_primary  += lane.epoch.skip_not_primary;
            lane.run.skip_missing_band += lane.epoch.skip_missing_band;
            lane.run.skip_missing_mw   += lane.epoch.skip_missing_mw;
            lane.run.skip_young_arc    += lane.epoch.skip_young_arc;
            lane.run.skip_bad_lambda   += lane.epoch.skip_bad_lambda;
            lane.run.skip_mixed_frame  += lane.epoch.skip_mixed_frame;
            lane.run.skip_quarantine   += lane.epoch.skip_quarantine;
            lane.run.sum_R             += lane.epoch.sum_R;
        }
    }
    // WL lane (M5 card WL-2, §3.3): apply the stashed inject plan against the fresh
    // posterior (this epoch's NL), then fold the run counters.  The plan was decided from
    // detector statistics (§3.0.1) in the accepted build.  Here the carried lane value
    // and NL sum are read and, only if the carry still disagrees with the WL integer by
    // more than inject_disagree_min_cyc (self-limiting), the lane difference is set to
    // the integer while the phase-determined sum is preserved.  injectCarriedChord marks
    // each moved chord D5-exempt for the next install (the §3.3 storm guard) internally.
    if (wl_inject_on) {
        for (const auto& pl : wl_inject_epoch) {
            double z1 = 0.0, v1 = 0.0, z2 = 0.0, v2 = 0.0;
            if (!carry_driver.carriedChordValue(pl.group_l1, pl.chord, z1, v1) ||
                !carry_driver.carriedChordValue(pl.group_l2, pl.chord, z2, v2)) {
                ++wl_run.skip_basis;   // not 1:1 across both bands' carry bases: skip (§3.3)
                if (uwl_on) ++wl_cons_run[pl.cons ? pl.cons : '?'].skip_basis;
                continue;
            }
            const double A1 = (z1 + pl.z_lin1) / pl.lambda1;   // absolute band DDs (cyc)
            const double A2 = (z2 + pl.z_lin2) / pl.lambda2;
            const double lane = A1 - A2, s = A1 + A2;          // WL DD + preserved NL sum
            if (std::abs(pl.w - lane) <= nmw.inject_disagree_min_cyc)
                continue;   // already lane-consistent (hold/fix row or prior inject): no edit
            const double A1p = 0.5 * (pl.w + s), A2p = 0.5 * (s - pl.w);
            const double sg1 = nmw.wl_fix_sigma_cyc * pl.lambda1;
            const double sg2 = nmw.wl_fix_sigma_cyc * pl.lambda2;
            const bool i1 = carry_driver.injectCarriedChord(
                pl.group_l1, pl.chord, A1p * pl.lambda1 - pl.z_lin1, sg1 * sg1);
            const bool i2 = carry_driver.injectCarriedChord(
                pl.group_l2, pl.chord, A2p * pl.lambda2 - pl.z_lin2, sg2 * sg2);
            const bool moved = i1 && i2;
            if (moved) { ++wl_run.injects; wl_run.inject_abs_sum += std::abs(pl.w - lane);
                         if (uwl_on) {
                             auto& wcr = wl_cons_run[pl.cons ? pl.cons : '?'];
                             ++wcr.injects; wcr.inject_abs_sum += std::abs(pl.w - lane);
                         }
                         if (mw_tagsets) d5_inject_chords_now.insert(pl.chord); }
            else       { ++wl_run.skip_basis;
                         if (uwl_on) ++wl_cons_run[pl.cons ? pl.cons : '?'].skip_basis; }
        }
        // Accepted-install accumulation: the pooled epoch tallies (also the whole walk
        // when the UWL card is off) plus, under UWL-1, the per-cons walk tallies, which
        // are folded into both the aggregate and the per-cons run totals.
        wlAccumulate(wl_run, wl_epoch);
        for (const auto& [wcons, we] : wl_cons_epoch) {
            wlAccumulate(wl_run, we);
            wlAccumulate(wl_cons_run[wcons], we);
        }

        // Lane trace (F-B): write every entry's post-install carried lane and tags.
        if (mw_lane_trace && !lane_trace_epoch.empty()) {
            std::map<ArcKey, double> w_of;   // decided cycles' integer targets
            for (const auto& pl : wl_inject_epoch) w_of[pl.chord] = pl.w;
            // Stream and header are owned by NetworkRunTraces (opened once, truncating, in
            // openMwWlDumps), so a re-run does not append to the previous run's rows.
            auto& ltf = traces.wl_lane_trace;
            if (ltf) {
                for (const auto& e : lane_trace_epoch) {
                    double z1 = 0.0, v1 = 0.0, z2 = 0.0, v2 = 0.0;
                    if (!carry_driver.carriedChordValue(e.g1, e.chord, z1, v1) ||
                        !carry_driver.carriedChordValue(e.g2, e.chord, z2, v2))
                        continue;   // not 1:1 carried in both bands this epoch
                    const double lane = (z1 + e.z_lin1) / e.lam1
                                      - (z2 + e.z_lin2) / e.lam2;
                    const double frac = lane - std::round(lane);
                    const auto wit = w_of.find(e.chord);
                    const std::string sta_name =
                        (e.chord.u >= 0 && e.chord.u < static_cast<int>(stations.size()))
                            ? stations[static_cast<std::size_t>(e.chord.u)].name
                            : std::string{};
                    ltf << se.time.week << ',' << fmt::format("{:.1f}", se.time.tow)
                        << ',' << e.chord.u << ',' << e.chord.v << ',' << sta_name
                        << ',' << e.sat << ',' << fmt::format("{:.4f}", lane) << ','
                        << fmt::format("{:.4f}", frac) << ',';
                    if (wit != w_of.end()) ltf << fmt::format("{:.0f}", wit->second);
                    ltf << ',' << (wit != w_of.end() ? 1 : 0) << ','
                        << (d5_hold_chords.count(e.chord) ? 1 : 0) << ','
                        << (d5_inject_chords_now.count(e.chord) ? 1 : 0) << '\n';
                }
            }
        }
    }
    const auto& cc = carry_driver.counters();
    carry_epochs       = cc.epochs;
    carry_born_total   = cc.chords_born;
    carry_marg_total   = cc.chords_marginalized;
    carry_merges_total = cc.merges;
    carry_splits_total = cc.splits;
    carry_swaps_total  = cc.tree_swaps;
    carry_zdim_last    = cc.z_dim;
    carry_slipkills_total = cc.slip_kills;
    carry_dropped_total   = cc.chords_dropped;
    carry_innov_flags_total  = cc.innov_flags;
    carry_innov_retaps_total = cc.innov_retaps;
    // Difference the run-cumulative driver counters into this epoch's churn for the
    // per-epoch trace columns (P1-1, SPEC_day129_frame_guards).  Taken here, at the only
    // site that owns the cumulative copy, so the deltas always agree with the run-end
    // totals.  Trace only; nothing reads them back.
    ctx.carry.ep_chord_births  = cc.chords_born  - ctx.carry.carry_prev_born;
    ctx.carry.ep_slip_refloats = cc.slip_kills   - ctx.carry.carry_prev_slipkills;
    ctx.carry.ep_d5_flags      = cc.innov_flags  - ctx.carry.carry_prev_innov_flags;
    ctx.carry.ep_d5_retaps     = cc.innov_retaps - ctx.carry.carry_prev_innov_retaps;
    ctx.carry.carry_prev_born           = cc.chords_born;
    ctx.carry.carry_prev_slipkills      = cc.slip_kills;
    ctx.carry.carry_prev_innov_flags    = cc.innov_flags;
    ctx.carry.carry_prev_innov_retaps   = cc.innov_retaps;
    // Fold the post-solve D5 innovation flags into the next epoch's slip funnel.
    // Each (group, chord) maps back to its (station, ambiguity) via the reverse map
    // built with this epoch's blocks; next epoch's buildCarryDdPriors excludes it from
    // the A_old membership so the affected cycle re-taps fresh (the C3c path).
    if (carry_innov_gate) {
        // A trip that the post-resume warm-up suppressed is a measurement, not a decision:
        // it kept its carried prior.  Marking its arc slipped here would refloat next
        // epoch the chord the window exists to hold, so the warm-up would no longer be
        // diagnostic-only (RS-2b).  The mask is index-aligned with lastInnovFlags() and
        // empty whenever the window is closed.
        const auto& d5_warm = carry_driver.lastInnovWarmupSuppressed();
        const auto& d5_flag_list = carry_driver.lastInnovFlags();
        for (std::size_t fx = 0; fx < d5_flag_list.size(); ++fx) {
            if (fx < d5_warm.size() && d5_warm[fx]) continue;
            const auto& [gk, chord] = d5_flag_list[fx];
            const auto git = carry_arc_amb_key.find(gk);
            const auto ait = (git != carry_arc_amb_key.end())
                                 ? git->second.find(chord) : std::map<ArcKey, std::string>::const_iterator{};
            if (git == carry_arc_amb_key.end() || ait == git->second.end()) {
                // A flagged chord that the reverse map (built with this epoch's blocks)
                // cannot resolve is not expected, because both come from the same
                // buildCarryDdPriors.  Warn once so a wiring regression is visible.  The
                // flag is dropped, which is safe: q_z and next-epoch detection remain.
                if (++carry_innov_map_miss == 1)
                    log->warn("Network[Card4 D5]: an innovation flag (group {}, arc {}→{}) did "
                              "not resolve to an ambiguity in the reverse map at TOW {:.1f} — "
                              "the next-epoch re-tap is skipped for it (reverse-map/block "
                              "mismatch; investigate if this recurs).",
                              gk, chord.u, chord.v, se.time.tow);
                continue;
            }
            // The insert result is the de-duplication signal the attribution
            // needs: carry_innov_pending was cleared at this epoch's head
            // (the C3c drain), so `.second` is true exactly once per
            // (station, ambiguity) per epoch.
            const bool first_this_epoch =
                carry_innov_pending.insert({chord.u, ait->second}).second;
            // Tag the flag with the same tri-state raw verdict the v3 corroboration gate
            // consults, so next epoch's carry-membership test can tell "the arc did not
            // step; this is model tension" from "the arc stepped, or there was no
            // evidence" (Card 4 D5 v4, network.carry_invariant_innov_retap_only).
            //
            // The funnel entry above is unconditional: every side consumer (the Iono-2 v3k
            // erase classifier, anchor hygiene, the PB emit filter, Card G's bridge
            // classifier) reads the full union.  This set only
            // decides whether the arc is also excluded from next epoch's A_old membership,
            // i.e. whether the DD the gate just re-tapped in-epoch is also discarded and
            // refloated from nothing.
            //
            // After a real slip the refloat completes the reset; suppressing it leaves
            // 0.48 cyc of error, because the in-epoch reset moves only by the post-fit
            // innovation v, which the fold's −GᵀΣG border term has already shrunk
            // (tests/test_network_invariant_carry_driver.cpp, GATE 10b).  Membership here
            // therefore requires positive evidence of continuity, as v3 does:
            //   stepped           ⇒ absent  ⇒ refloats (a real discontinuity)
            //   measured and flat ⇒ present ⇒ keeps its carry (nothing to complete)
            //   no evidence       ⇒ absent  ⇒ refloats (arc start / gap / single band)
            // The response of standing invariant #1 is narrowed to the diagnosed
            // population, not removed.
            if (retap_only && slip_qc.epoch.computed && chord.u >= 0
                && static_cast<std::size_t>(chord.u) < slip_qc.epoch.verdict.size()) {
                if (const auto pk = StateKeys::parseAmbiguityKey(ait->second)) {
                    const auto& vmap = slip_qc.epoch.verdict[static_cast<std::size_t>(chord.u)];
                    const auto vit = vmap.find(pk->satellite);
                    if (vit != vmap.end() && vit->second.measured
                        && !slip_qc.epoch.stepped.count({chord.u, pk->satellite})) {
                        ctx.carry.carry_innov_pending_flat.insert({chord.u, ait->second});
                    }
                }
            }
            // Attribute the D5 re-tap counter (Card G(c)): a raw-step coincidence test at
            // the same (station, satellite, epoch) separates "the gate caught a real
            // observable event" from "the gate is firing on model tension".
            // Instrumentation only; the flag is unchanged either way.  Both units are
            // counted: flag rows (the historical unit) and distinct funnel arcs (what
            // actually re-taps).
            if (nc.slip_qc.gap_instrument) {
                ++slip_qc.counters.d5_retaps;
                if (first_this_epoch) ++slip_qc.counters.d5_retap_ambs;
                if (const auto pk = StateKeys::parseAmbiguityKey(ait->second))
                    if (slip_qc.epoch.stepped.count({chord.u, pk->satellite})) {
                        ++slip_qc.counters.d5_retaps_stepped;
                        if (first_this_epoch) ++slip_qc.counters.d5_retap_ambs_stepped;
                    }
            }
        }
    }
    // Attribution dump (F-A; log-only, network.mw_wl.d5_dump): one CSV row per D5
    // flag with adjacency tags.  Uses the same reverse map as the funnel above.
    //
    // The trailing `raw_stepped` column is the exact-epoch raw-step tag (Card D5-A/D2,
    // docs/SPEC_slip_qc_hardening.md §9.1): did the receiver's own MW/GF
    // level step at this (station, satellite) on this epoch?  It is tri-state,
    // not 0/1:
    //   1     : this (station, sat) is in slip_qc.epoch.stepped (Card G's
    //           empirical per-station gate fired);
    //   0     : a step could be formed (adjacent-epoch dual-band predecessor)
    //           and it was flat;
    //   blank : no evidence (raw observables not computed this run, arc
    //           start / gap / single-band, or the flag's ambiguity key did not
    //           resolve).  Writing 0 there would claim "no raw step" for the
    //           population D5-A exists to decompose.
    // The ±1-epoch offline join in scripts/grade_slip_agreement.py --d5 remains
    // the reference; this column is a zero-cost exact-epoch tag that makes
    // network_d5_flags.csv self-contained.  Log-only either way: the solve never
    // reads it.
    if (mw_d5_dump && carry_innov_gate) {
        const auto& flags  = carry_driver.lastInnovFlags();
        const auto& sigmas = carry_driver.lastInnovFlagSigmas();
        const auto& terms  = carry_driver.lastInnovFlagTerms();
        const auto& warm_mask = carry_driver.lastInnovWarmupSuppressed();   // RS-2b
        if (!flags.empty()) {
            // Stream and header are owned by NetworkRunTraces (opened once, truncating, in
            // openMwWlDumps, under the arming expression this block tests), so repeated
            // runs do not append to one file and A/B row counts stay comparable.
            auto& d5f = traces.d5_flags_trace;
            if (d5f) {
                for (std::size_t fi = 0; fi < flags.size(); ++fi) {
                    const auto& [gk, chord] = flags[fi];
                    std::string amb, sat;
                    const auto git = carry_arc_amb_key.find(gk);
                    if (git != carry_arc_amb_key.end()) {
                        const auto ait = git->second.find(chord);
                        if (ait != git->second.end()) amb = ait->second;
                    }
                    if (!amb.empty())
                        if (const auto parts = StateKeys::parseAmbiguityKey(amb))
                            sat = parts->satellite;
                    const std::string sta_name =
                        (chord.u >= 0 && chord.u < static_cast<int>(stations.size()))
                            ? stations[static_cast<std::size_t>(chord.u)].name
                            : std::string{};
                    // Tri-state raw-step tag (D2; see the banner above).  Reads the
                    // same per-epoch evidence the run-end d5_retaps_stepped counter
                    // uses, so the CSV and the counter agree.
                    std::string raw_stepped;
                    if (slip_qc.epoch.computed && !sat.empty() && chord.u >= 0
                        && static_cast<std::size_t>(chord.u)
                               < slip_qc.epoch.verdict.size()) {
                        if (slip_qc.epoch.stepped.count({ chord.u, sat })) {
                            raw_stepped = "1";
                        } else {
                            const auto& vmap =
                                slip_qc.epoch.verdict[static_cast<std::size_t>(chord.u)];
                            const auto vit = vmap.find(sat);
                            if (vit != vmap.end() && vit->second.measured)
                                raw_stepped = "0";
                        }
                    }
                    // The terms behind std_innov (churn card), so the Cov(v)-vs-wander
                    // question can be answered from this file alone.  Written blank
                    // rather than zero when the driver has no term for the row, since
                    // a 0.0 on ẑ or var_v would read as a measurement.
                    std::string t_zhat, t_v, t_var, t_s0, t_zpost;
                    if (fi < terms.size()) {
                        const auto& T = terms[fi];
                        t_zhat  = fmt::format("{:.6f}", T.zhat);
                        t_v     = fmt::format("{:.6f}", T.v);
                        t_var   = fmt::format("{:.6e}", T.var_v);
                        t_s0    = fmt::format("{:.4f}", T.sigma0);
                        t_zpost = fmt::format("{:.6f}", T.z_post);
                    }
                    // This epoch's SH-constraint deweight weight for the station (the
                    // topology ledger's snapshot, taken pre-solve).  Blank without
                    // network.trace_topology, which is the only path that fills the map.
                    std::string t_dw;
                    if (const auto wit = ctx.carry.topo_prev_w.find(chord.u);
                        wit != ctx.carry.topo_prev_w.end())
                        t_dw = fmt::format("{:.4g}", wit->second);
                    d5f << se.time.week << ',' << fmt::format("{:.1f}", se.time.tow)
                        << ',' << gk << ',' << chord.u << ',' << chord.v << ','
                        << sta_name << ',' << amb << ','
                        << fmt::format("{:.2f}",
                               fi < sigmas.size() ? sigmas[fi] : 0.0) << ','
                        << (d5_ewl_stasat.count({ chord.u, sat }) ? 1 : 0) << ','
                        << (d5_hold_chords.count(chord) ? 1 : 0) << ','
                        << (d5_inject_chords_prev.count(chord) ? 1 : 0) << ','
                        << raw_stepped << ','
                        << t_zhat << ',' << t_v << ',' << t_var << ','
                        << t_s0 << ',' << t_zpost << ',' << t_dw << ','
                        // 1 = this trip was inside the post-resume D5 warm-up (RS-2b), so
                        // it was only counted: no re-tap, no funnel entry, no refold.
                        // Plain 0/1 rather than the raw_stepped tri-state, because this
                        // value is never unknown: the driver either suppressed the row or
                        // acted on it.
                        << (fi < warm_mask.size() && warm_mask[fi] ? 1 : 0) << '\n';
                }
            }
        }
    }
    if (mw_tagsets) {   // rotate: this epoch's applied injects tag the next install's flags
        d5_inject_chords_prev.swap(d5_inject_chords_now);
        d5_inject_chords_now.clear();
    }
}

} // namespace GnssProcessor

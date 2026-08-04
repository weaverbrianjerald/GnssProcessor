/**
 * @file NetworkInstallCarry.cpp
 * @brief Implementation of @ref networkInstallCarry, the SB_INSTALL block relocated
 *        VERBATIM (dedent-8, zero substitution) out of Application::runNetwork at
 *        decomposition INC-9b (2026-07-22).  Byte-copy of the former if(carry_invariant)
 *        body; the alias preamble reconstructs nc/nmw/trace, the ctx.carry aliases and
 *        the wlAccumulate lambda so the code reads the original names.
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
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
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
    const std::map<int, std::map<ArcKey, std::string>>& carry_arc_amb_key2,
    const std::filesystem::path& log_dir_,
    bool carry_par,
    int n_threads,
    int seam_threads,
    bool carry_innov_gate,
    bool mw_wl_on,
    bool lane_on,
    bool ewl_on,
    bool uwl_on,
    bool wl_inject_on,
    bool lane_fix_on,
    bool mw_d5_dump,
    bool mw_lane_trace,
    bool mw_tagsets,
    bool mw_wl_f6_dump,
    std::vector<EwlLane>& ewl_lanes,
    std::vector<WlInjectPlan>& wl_inject_epoch,
    std::vector<LaneTraceEntry>& lane_trace_epoch,
    NetworkMwWlLayer::Counters& mw_wl_run,
    NetworkMwWlLayer::Counters& mw_wl_epoch,
    WlCounters& wl_run,
    WlCounters& wl_epoch,
    std::map<char, WlCounters>& wl_cons_run,
    std::map<char, WlCounters>& wl_cons_epoch,
    F6Capture& mw_wl_f6) {
    // --- Alias preamble: reconstructs the run-TU aliases the relocated
    //     SB_INSTALL body reads (nc/nmw/trace, the ctx.carry aliases, and the
    //     wlAccumulate lambda).  Byte-copies of the former runNetwork() lines. ----
    const auto& nc  = config_.getNetworkConfig();
    const auto& nmw = config_.getNetworkConfig().mw_wl;
    auto& trace = traces.trace;
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
    // Card 4 (M1) D5 v4: tag corroborated-FLAT flags so next epoch's carry-membership
    // test can spare them.  The loader already forces it false without innov_gate.
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
        a.lane_mw_rows        += b.lane_mw_rows;
        a.lane_bridge_rows    += b.lane_bridge_rows;
        a.lane_fix_folded     += b.lane_fix_folded;
        a.lane_fix_unchanged  += b.lane_fix_unchanged;
        a.lane_skip_meas      += b.lane_skip_meas;
        a.lane_pinned_skips   += b.lane_pinned_skips;
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

    // Card 4 (M1) D5 v3: hand the driver this epoch's CORROBORATION set before the install
    // consumes it — the chords whose underlying arc the raw MW/GF observables measured as
    // CONTINUOUS.  Built here because this is the one seam holding all three inputs at once:
    // slip_qc (the raw verdicts, computed in PASS A before the solve), carry_arc_amb_key
    // (the group→chord→ambiguity reverse map rebuilt with the blocks), and carry_driver.
    //
    // MEMBERSHIP IS "MEASURED AND FLAT", NEVER "NO EVIDENCE" — verdict.measured must be true
    // AND the (station, satellite) must be absent from epoch.stepped.  An unmeasured arc is
    // deliberately left OUT so it still re-taps: suppressing on absence of evidence would
    // silently disable the fine-slip response on exactly the arcs we know least about
    // (standing invariant #1).  Same tri-state the network_d5_flags.csv `raw_stepped` column
    // encodes, so the trace and the gate cannot disagree.
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
                    flat.insert({ group, chord });   // measured AND flat ⇒ suppressible
            }
        }
        carry_driver.setInnovFlatChords(std::move(flat));
    }

    // parallel_carry: thread installPosterior's posterior-cov GEMMs.  Run it
    // SERIAL-over-blocks (n_threads=1, its byte-identical-to-serial path) so Eigen's
    // threading does not nest with the seam's per-block OpenMP.
    try { CarryEigenScope es(carry_par, n_threads);
          carry_driver.installPosterior(est, x_s, plan.shared_datum,
                                        carry_par ? 1 : seam_threads); }
    catch (const std::exception& ex) {
        log->warn("Network[Card4]: DD-carry install failed at TOW {:.1f} ({}); "
                  "carry reset (refloats next epoch).", se.time.tow, ex.what());
        carry_driver.reset();
    }
    carry_prev_tow = se.time.tow;

    // === PD-safe invariant-DD fold (docs/SPEC_pd_safe_dd_fold.md) telemetry =======
    // INC-0 is ALWAYS-ON and solve-neutral.  It exists because the fold's `M := 0`
    // response to a non-PD Pᶻ+B had no counter, no log and no event: a block that went
    // indefinite between epoch 2 and epoch 122 froze forever, its posterior pinned to its
    // prior, while dd_sigma_cyc / the D5 gate / the AR probe all stayed carry-blind.
    // Measured 2026-08-02 on `runs/churn_base` (63 sta, 3 h, DOY-129): 568 of 686 carried
    // rows — 78 % of the state — were diffusing behind exactly that silence, in the
    // control AND in every arm of the lineage.  Three weeks of campaigns were graded on
    // that substrate.  Read here rather than in the kernel because this is the one seam
    // that holds the ACCEPTED solve's fold, the block layout, the station roster and the
    // trace sinks at once.
    //
    // Per-fold, not cumulative: a within-epoch RAIM re-solve re-folds and overwrites the
    // kernel's stats, so what we read is always the accepted solve's screen.
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
        // NOTE: no per-epoch stamp into ctx.carry.  The epoch-status columns are read
        // straight off est.ddFoldStats() at both writer sites (NetworkStashStatus for an
        // accepted epoch, NetworkSolveGlue for a withheld one) — see the rationale on
        // NetworkCarryState.  This seam owns only the RUN-cumulative roll-up above, the
        // cross-epoch transition ledger below, and the events.

        // INC-1c: a repair is HYGIENE only while it stays dust.  A clamp above 1e-6·λmax
        // means the block was not merely rounding-damaged — that is a NEW bug and it must
        // scream rather than be silently laundered into a PD matrix.  Warned once (the
        // run-end summary carries the totals); the counter is the population.
        if (fs.repair_worst > 1e-6 && ++cnt.ddfold_repair_loud == 1)
            log->warn("Network (Card 4 PD-safe fold, INC-1c): a DD-fold eigenvalue REPAIR "
                      "clamped a magnitude of {:.3e}·λmax at TOW {:.1f} — far above the "
                      "1e-12 dust floor the repair exists for.  The block was not merely "
                      "rounding-damaged; treat this as a NEW defect and investigate rather "
                      "than trusting the repaired covariance.  (Further occurrences are "
                      "counted, not logged.)",
                      fs.repair_worst, se.time.tow);

        // Invariant #7 — BOTH edges of the decoupling.  The healthy→degenerate edge is the
        // DECOUPLING one (a block stops taking data); the degenerate→healthy edge is a
        // spontaneous RE-COUPLING (the block factored again on its own, without a
        // rebirth).  Warned on the TRANSITION only: a per-epoch line over 360 epochs
        // teaches readers to ignore it, which is precisely how this defect stayed
        // invisible.  Keyed by (group, first chord) — components merge and split, so no
        // block index is stable across epochs, but a giant block keeps its leading chord.
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
            // The pivot also rides `detail` in exponential form: emitEvent renders the
            // statistic column with {:.6f}, which turns a −2.9e-10 pivot — the entire
            // point of the row — into "-0.000000".
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

        // INC-2a: the bounded, LOUD re-coupling — a block that could not be rescued is
        // rebirthed (means kept, Pᶻ re-seeded diffuse) rather than frozen.  Empty unless
        // network.carry_invariant_pd_safe.
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

    // D5 v2 σ̂₀ CLAMP VISIBILITY.  The gate's threshold is k·σ̂₀ and σ̂₀ reports 1.00× in FOUR
    // distinct states, three of which mean the gate silently degraded to the raw formal (v1)
    // test.  Log the TRANSITION, not the state (the INC-0 rule: a per-epoch line over 360
    // epochs teaches readers to ignore it — which is exactly how the `churn_base2` lineage
    // ran an entire campaign on a v1 gate while its run-end line said "calibration armed").
    {
        using Infl = NetworkInvariantCarryDriver::InflationState;
        const auto  st  = carry_driver.innovInflationStatus();
        const char* tag = NetworkInvariantCarryDriver::inflationStateName(st.state);
        // `Off` is not a transition worth announcing — calibration was never armed, and
        // every non-D5 network run would open with the line.  The counters still tally it,
        // so the run-end summary can still distinguish "v1 by config" from "v2 that never
        // engaged", which is the distinction that matters.
        if (st.state != Infl::Off && ctx.carry.d5_calib_state_prev != tag) {
            // Only the SILENT degradations warn.  Warm-up is expected and self-clearing at
            // the start of every calibrated run, so warning on it would put a WARN on epoch
            // 1 of every run and train readers to skip the line — the failure this
            // instrument exists to end.  A run that never LEAVES warm-up is caught by the
            // run-end summary instead.
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

    // M5 card WL-1: fold the accepted build's WL eligibility tallies into the run
    // totals (this install runs only for an accepted epoch; the last buildCarryDdPriors
    // before the accepted solve set mw_wl_epoch).  Invariant #4: counters or it didn't happen.
    if (mw_wl_on) {
        mw_wl_run.cycles_seen       += mw_wl_epoch.cycles_seen;
        mw_wl_run.rows_built        += mw_wl_epoch.rows_built;
        mw_wl_run.rows_capped       += mw_wl_epoch.rows_capped;
        mw_wl_run.skip_not_primary  += mw_wl_epoch.skip_not_primary;
        mw_wl_run.skip_missing_band += mw_wl_epoch.skip_missing_band;
        mw_wl_run.skip_missing_mw   += mw_wl_epoch.skip_missing_mw;
        mw_wl_run.skip_young_arc    += mw_wl_epoch.skip_young_arc;
        mw_wl_run.skip_bad_lambda   += mw_wl_epoch.skip_bad_lambda;
        mw_wl_run.skip_mixed_frame  += mw_wl_epoch.skip_mixed_frame;
        mw_wl_run.sum_R             += mw_wl_epoch.sum_R;

        // --- F6 one-shot attribution write (log-only; docs/HANDOFF_mw_wl_card.md §6c). ---
        // The accepted epoch's x_s is final here; add reconstructed_abs (the estimator's
        // current WL DD over the same cycle) + the kernel WL-block innovation, then dump.
        if (mw_wl_f6_dump && !mw_wl_f6.written && mw_wl_f6.pending
            && mw_wl_f6.tow == se.time.tow) {
            std::map<int, Eigen::VectorXd> xr_of;   // est_station -> local corrections
            for (const auto& fr : mw_wl_f6.rows)
                for (const auto& a : fr.arcs)
                    if (a.est_station >= 0 && !xr_of.count(a.est_station)) {
                        try { xr_of[a.est_station] = est.reconstructStation(
                                  static_cast<std::size_t>(a.est_station), x_s); }
                        catch (...) { xr_of[a.est_station] = Eigen::VectorXd(); }
                    }
            // Kernel innovation v = z_free − Gᵀx_s − ẑ.  ddPriorInnovation is BLOCK-scoped, so
            // under f6_all_blocks each block gets its own request and each row indexes into
            // its own block's vector (rows of a block are contiguous in the capture).
            std::map<std::size_t, Eigen::VectorXd> v_of;
            {
                const Eigen::MatrixXd cov0 = Eigen::MatrixXd::Zero(x_s.size(), x_s.size());
                std::set<std::size_t> want(mw_wl_f6.row_block.begin(),
                                           mw_wl_f6.row_block.end());
                if (want.empty()) want.insert(mw_wl_f6.wl_block_index);
                for (const std::size_t bi : want) {
                    Eigen::VectorXd v; Eigen::MatrixXd cov_v;   // (cov unused; v is cov-free)
                    try { est.ddPriorInnovation(bi, x_s, cov0, v, cov_v); }
                    catch (...) { v.resize(0); }
                    v_of[bi] = std::move(v);
                }
            }
            std::map<std::size_t, std::size_t> row_in_block;   // block -> running row index
            std::error_code ec_f6;
            std::filesystem::create_directories(log_dir_, ec_f6);
            std::ofstream f6(log_dir_ / "network_mw_wl_f6.csv");
            if (f6) {
                f6 << "# M5 WL-1 F6 attribution dump — first arming epoch (week "
                   << mw_wl_f6.week << ", tow " << fmt::format("{:.1f}", mw_wl_f6.tow)
                   << ", loop_epoch " << mw_wl_f6.loop_epoch << ", wl_block_index "
                   << mw_wl_f6.wl_block_index << ")\n";
                f6 << "row,n_arcs,z_mw_cyc,z_mw_frac,z_lin_cyc,z_corr_cyc,zhat_block_cyc,"
                      "reconstructed_abs_cyc,recon_minus_zmw_cyc,recon_abs_frac,innovation_v_cyc\n";
                for (std::size_t r = 0; r < mw_wl_f6.rows.size(); ++r) {
                    const auto& fr = mw_wl_f6.rows[r];
                    double recon = 0.0; bool recon_ok = true;
                    for (const auto& a : fr.arcs) {
                        const auto it = xr_of.find(a.est_station);
                        double xr1 = 0.0, xr2 = 0.0;
                        if (it != xr_of.end()
                            && a.col1 >= 0 && static_cast<Eigen::Index>(a.col1) < it->second.size()
                            && a.col2 >= 0 && static_cast<Eigen::Index>(a.col2) < it->second.size()) {
                            xr1 = it->second(a.col1); xr2 = it->second(a.col2);
                        } else { recon_ok = false; }
                        if (a.lambda1 > 0.0 && a.lambda2 > 0.0)
                            recon += (a.coeff / a.lambda1) * (a.x_lin1 + xr1)
                                   - (a.coeff / a.lambda2) * (a.x_lin2 + xr2);
                    }
                    const double zmw_frac = fr.z_mw - std::round(fr.z_mw);
                    const std::size_t bi = (r < mw_wl_f6.row_block.size())
                                               ? mw_wl_f6.row_block[r]
                                               : mw_wl_f6.wl_block_index;
                    const std::size_t rb = row_in_block[bi]++;
                    const auto vit = v_of.find(bi);
                    const double vrow =
                        (vit != v_of.end() && static_cast<Eigen::Index>(rb) < vit->second.size())
                            ? vit->second(static_cast<Eigen::Index>(rb))
                            : std::numeric_limits<double>::quiet_NaN();
                    f6 << r << ',' << fr.arcs.size() << ','
                       << fmt::format("{:.4f}", fr.z_mw) << ','
                       << fmt::format("{:.4f}", zmw_frac) << ','
                       << fmt::format("{:.4f}", fr.z_lin) << ','
                       << fmt::format("{:.4f}", fr.z_corr) << ','
                       << fmt::format("{:.4f}", fr.zhat_block) << ','
                       << (recon_ok ? fmt::format("{:.4f}", recon) : std::string("nan")) << ','
                       << (recon_ok ? fmt::format("{:.4f}", recon - fr.z_mw) : std::string("nan")) << ','
                       << (recon_ok ? fmt::format("{:.4f}", recon - std::round(recon)) : std::string("nan")) << ','
                       << fmt::format("{:.4f}", vrow) << '\n';
                }
                f6 << "row,arc,est_station,sat,sig1,sig2,coeff,mw_mean_m,mw_arc_cyc,mw_count,"
                      "mw_sigma_m,bia_corrected,col1,col2,lambda1,lambda2,lambda_wl,"
                      "x_lin1_m,x_lin2_m,xr1_m,xr2_m,n1_abs_cyc,n2_abs_cyc,wl_arc_kf_cyc\n";
                for (std::size_t r = 0; r < mw_wl_f6.rows.size(); ++r) {
                    const auto& fr = mw_wl_f6.rows[r];
                    for (std::size_t k = 0; k < fr.arcs.size(); ++k) {
                        const auto& a = fr.arcs[k];
                        const auto it = xr_of.find(a.est_station);
                        double xr1 = 0.0, xr2 = 0.0; bool have_xr = false;
                        if (it != xr_of.end()
                            && a.col1 >= 0 && static_cast<Eigen::Index>(a.col1) < it->second.size()
                            && a.col2 >= 0 && static_cast<Eigen::Index>(a.col2) < it->second.size()) {
                            xr1 = it->second(a.col1); xr2 = it->second(a.col2); have_xr = true;
                        }
                        const double n1abs = (a.lambda1 > 0.0) ? (a.x_lin1 + xr1) / a.lambda1 : 0.0;
                        const double n2abs = (a.lambda2 > 0.0) ? (a.x_lin2 + xr2) / a.lambda2 : 0.0;
                        const double mw_arc_cyc = (a.lambda_wl > 0.0) ? a.mw_mean_m / a.lambda_wl : 0.0;
                        f6 << r << ',' << k << ',' << a.est_station << ','
                           << a.sat << ',' << a.sig1 << ',' << a.sig2 << ','
                           << fmt::format("{:.1f}", a.coeff) << ','
                           << fmt::format("{:.4f}", a.mw_mean_m) << ','
                           << fmt::format("{:.4f}", mw_arc_cyc) << ','
                           << a.mw_count << ','
                           << fmt::format("{:.4f}", std::sqrt(std::max(a.mw_var_m2, 0.0))) << ','
                           << (a.bia_corrected ? 1 : 0) << ','
                           << a.col1 << ',' << a.col2 << ','
                           << fmt::format("{:.6f}", a.lambda1) << ','
                           << fmt::format("{:.6f}", a.lambda2) << ','
                           << fmt::format("{:.6f}", a.lambda_wl) << ','
                           << fmt::format("{:.4f}", a.x_lin1) << ','
                           << fmt::format("{:.4f}", a.x_lin2) << ','
                           << (have_xr ? fmt::format("{:.4f}", xr1) : std::string("nan")) << ','
                           << (have_xr ? fmt::format("{:.4f}", xr2) : std::string("nan")) << ','
                           << fmt::format("{:.4f}", n1abs) << ','
                           << fmt::format("{:.4f}", n2abs) << ','
                           << fmt::format("{:.4f}", n1abs - n2abs) << '\n';
                    }
                }
                mw_wl_f6.written = true;
                mw_wl_f6.pending = false;
                log->info("Network (M5/WL-1 F6): wrote {} — first arming epoch tow {:.1f}, "
                          "{} row(s) over {} WL block(s); attribute z_mw_frac (MW "
                          "integer-ness) + recon_minus_zmw (state-frame gap).  ⚠️ This is a "
                          "capped SAMPLE (network.mw_wl.f6_max_rows={}, f6_all_blocks={}) — "
                          "its size is NOT the WL population; read the run-end WL-1 counters "
                          "for that.",
                          (log_dir_ / "network_mw_wl_f6.csv").string(), mw_wl_f6.tow,
                          mw_wl_f6.rows.size(),
                          std::set<std::size_t>(mw_wl_f6.row_block.begin(),
                                                mw_wl_f6.row_block.end()).size(),
                          config_.getNetworkConfig().mw_wl.f6_max_rows,
                          config_.getNetworkConfig().mw_wl.f6_all_blocks);
            }
        }
    }
    // M5 card WL-2 EWL lane: same accepted-install accumulation (invariant #4),
    // per lane instance (UWL-1: one per tabled constellation when armed).
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
    // M5 card WL-2 WL lane (§3.3): apply the stashed inject plan against the FRESH
    // posterior (this epoch's NL), THEN fold the run counters.  The plan was decided
    // from DETECTOR statistics (§3.0.1) in the accepted build; here we read the carried
    // lane value + NL sum and, only if the carry still disagrees with the WL integer by
    // more than inject_disagree_min_cyc (self-limiting), set the lane difference to the
    // integer while preserving the phase-determined sum.  injectCarriedChord marks each
    // moved chord D5-exempt for the NEXT install (the §3.3 storm guard) internally.
    if (wl_inject_on || lane_fix_on) {
        for (const auto& pl : wl_inject_epoch) {
            double z1 = 0.0, v1 = 0.0, z2 = 0.0, v2 = 0.0;
            if (pl.pair
                    ? !carry_driver.carriedPairValue(pl.group_l1, pl.chord, z1, v1, z2, v2)
                    : (!carry_driver.carriedChordValue(pl.group_l1, pl.chord, z1, v1) ||
                       !carry_driver.carriedChordValue(pl.group_l2, pl.chord, z2, v2))) {
                ++wl_run.skip_basis;   // not 1:1 across BOTH bands' carry bases — skip (§3.3)
                if (uwl_on) ++wl_cons_run[pl.cons ? pl.cons : '?'].skip_basis;
                continue;
            }
            const double A1 = (z1 + pl.z_lin1) / pl.lambda1;   // absolute band DDs (cyc)
            const double A2 = (z2 + pl.z_lin2) / pl.lambda2;
            const double lane = A1 - A2, s = A1 + A2;          // WL DD + preserved NL sum
            if (std::abs(pl.w - lane) <= nmw.inject_disagree_min_cyc)
                continue;   // already lane-consistent (hold/fix row / prior inject) ⇒ no fight
            const double A1p = 0.5 * (pl.w + s), A2p = 0.5 * (s - pl.w);
            const double sg1 = nmw.wl_fix_sigma_cyc * pl.lambda1;
            const double sg2 = nmw.wl_fix_sigma_cyc * pl.lambda2;
            bool moved = false;
            if (pl.pair) {
                // WL-3 fallback: one joint edit, both bands, one D5 exemption (§3.3
                // "should fire ~never in steady state — telemetry proves it").
                moved = carry_driver.injectCarriedChordPair(
                    pl.group_l1, pl.chord,
                    A1p * pl.lambda1 - pl.z_lin1, sg1 * sg1,
                    A2p * pl.lambda2 - pl.z_lin2, sg2 * sg2);
            } else {
                const bool i1 = carry_driver.injectCarriedChord(
                    pl.group_l1, pl.chord, A1p * pl.lambda1 - pl.z_lin1, sg1 * sg1);
                const bool i2 = carry_driver.injectCarriedChord(
                    pl.group_l2, pl.chord, A2p * pl.lambda2 - pl.z_lin2, sg2 * sg2);
                moved = i1 && i2;
            }
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
        // when the UWL card is off) plus, under UWL-1, the per-cons walk tallies —
        // folded into BOTH the aggregate and the per-cons run totals.
        wlAccumulate(wl_run, wl_epoch);
        for (const auto& [wcons, we] : wl_cons_epoch) {
            wlAccumulate(wl_run, we);
            wlAccumulate(wl_cons_run[wcons], we);
        }

        // F-B lane trace: write every entry's POST-install carried lane + tags.
        if (mw_lane_trace && !lane_trace_epoch.empty()) {
            std::map<ArcKey, double> w_of;   // decided cycles' integer targets
            for (const auto& pl : wl_inject_epoch) w_of[pl.chord] = pl.w;
            // Stream + header are owned by NetworkRunTraces (opened TRUNCATING once in
            // openMwWlDumps).  This used to re-open the path per epoch in std::ios::app,
            // which silently concatenated a re-run onto the previous run's rows.
            auto& ltf = traces.wl_lane_trace;
            if (ltf) {
                for (const auto& e : lane_trace_epoch) {
                    double z1 = 0.0, v1 = 0.0, z2 = 0.0, v2 = 0.0;
                    if (lane_on
                            ? !carry_driver.carriedPairValue(e.g1, e.chord, z1, v1, z2, v2)
                            : (!carry_driver.carriedChordValue(e.g1, e.chord, z1, v1) ||
                               !carry_driver.carriedChordValue(e.g2, e.chord, z2, v2)))
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
    // P1-1 (SPEC_day129_frame_guards): difference the RUN-CUMULATIVE driver
    // counters into THIS epoch's churn, for the per-epoch trace columns.  Taken
    // here — the one site that owns the cumulative copy — so the deltas can never
    // disagree with the run-end totals.  Pure trace; nothing reads them back.
    ctx.carry.ep_chord_births  = cc.chords_born  - ctx.carry.carry_prev_born;
    ctx.carry.ep_slip_refloats = cc.slip_kills   - ctx.carry.carry_prev_slipkills;
    ctx.carry.ep_d5_flags      = cc.innov_flags  - ctx.carry.carry_prev_innov_flags;
    ctx.carry.ep_d5_retaps     = cc.innov_retaps - ctx.carry.carry_prev_innov_retaps;
    ctx.carry.carry_prev_born           = cc.chords_born;
    ctx.carry.carry_prev_slipkills      = cc.slip_kills;
    ctx.carry.carry_prev_innov_flags    = cc.innov_flags;
    ctx.carry.carry_prev_innov_retaps   = cc.innov_retaps;
    // D5: fold the post-solve innovation flags into the NEXT epoch's slip funnel.
    // Each (group, chord) maps back to its (station, ambiguity) via the reverse map
    // built with this epoch's blocks; next epoch's buildCarryDdPriors excludes it from
    // the A_old membership so the affected cycle re-taps fresh (the C3c path).
    if (carry_innov_gate) {
        for (const auto& [gk, chord] : carry_driver.lastInnovFlags()) {
            const auto git = carry_arc_amb_key.find(gk);
            const auto ait = (git != carry_arc_amb_key.end())
                                 ? git->second.find(chord) : std::map<ArcKey, std::string>::const_iterator{};
            if (git == carry_arc_amb_key.end() || ait == git->second.end()) {
                // A flagged chord that the reverse map (built with THIS epoch's blocks)
                // cannot resolve should never happen — both come from the same
                // buildCarryDdPriors.  Warn once so a wiring regression is not silent;
                // the flag is dropped (safe: q_z + next-epoch detection are the backstop).
                if (++carry_innov_map_miss == 1)
                    log->warn("Network[Card4 D5]: an innovation flag (group {}, arc {}→{}) did "
                              "not resolve to an ambiguity in the reverse map at TOW {:.1f} — "
                              "the next-epoch re-tap is skipped for it (reverse-map/block "
                              "mismatch; investigate if this recurs).",
                              gk, chord.u, chord.v, se.time.tow);
                continue;
            }
            // The insert RESULT is the de-duplication signal the attribution
            // needs: carry_innov_pending was cleared at this epoch's head
            // (the C3c drain), so `.second` is true exactly once per
            // (station, ambiguity) per epoch.  Under the WL-3 joint pair
            // block the gate flags per band ROW, so the same (group, chord)
            // arrives twice and only the first insert is a real re-tap.
            const bool first_this_epoch =
                carry_innov_pending.insert({chord.u, ait->second}).second;
            // Card 4 (M1) D5 v4 (network.carry_invariant_innov_retap_only): tag the flag
            // with the SAME tri-state raw verdict the v3 corroboration gate consults, so
            // next epoch's carry-membership test can tell "the arc did not step, this is
            // model tension" from "the arc stepped, or nobody could look".
            //
            // The funnel entry above is UNCONDITIONAL and stays that way — every side
            // consumer (the Iono-2 v3k erase classifier, the WL-3 MW-row slip-skip, anchor
            // hygiene, the PB emit filter, Card G's bridge classifier) keeps reading the
            // union it always read.  This set only decides whether the arc is additionally
            // excluded from next epoch's A_old membership, i.e. whether the DD the gate
            // just re-tapped in-epoch is ALSO thrown away and refloated from nothing.
            //
            // MEASURED (tests/test_network_invariant_carry_driver.cpp, GATE 10b): after a
            // REAL slip the refloat is what completes the reset — suppressing it leaves
            // 0.48 cyc on the table, because the in-epoch reset moves only by the post-fit
            // innovation v, which the fold's −GᵀΣG border term has already shrunk.  So
            // membership here requires POSITIVE evidence of continuity, exactly as v3 does:
            //   stepped           ⇒ absent ⇒ refloats (a real discontinuity)
            //   measured and flat ⇒ PRESENT ⇒ keeps its carry (nothing to complete)
            //   no evidence       ⇒ absent ⇒ refloats (arc start / gap / single band)
            // Absence of evidence is never evidence of absence — standing invariant #1's
            // response is narrowed to the diagnosed population, never removed.
            if (retap_only && slip_qc.epoch.computed && chord.u >= 0
                && static_cast<std::size_t>(chord.u) < slip_qc.epoch.verdict.size()) {
                if (const auto pk = StateKeys::parseAmbiguityKey(ait->second)) {
                    const auto& vmap = slip_qc.epoch.verdict[static_cast<std::size_t>(chord.u)];
                    const auto vit = vmap.find(pk->satellite);
                    if (vit != vmap.end() && vit->second.measured
                        && !slip_qc.epoch.stepped.count({chord.u, pk->satellite})) {
                        ctx.carry.carry_innov_pending_flat.insert({chord.u, ait->second});
                        // WL-3 §3.4: the pair partner rides the same graph edge and the
                        // same raw verdict, so it must be tagged with it — otherwise the
                        // pair refloats on band 2 and the suppression is silently half-on.
                        if (lane_on) {
                            const auto g2 = carry_arc_amb_key2.find(gk);
                            if (g2 != carry_arc_amb_key2.end()) {
                                const auto a2 = g2->second.find(chord);
                                if (a2 != g2->second.end())
                                    ctx.carry.carry_innov_pending_flat.insert({chord.u, a2->second});
                            }
                        }
                    }
                }
            }
            // Card G(c): make the D5 re-tap counter ATTRIBUTABLE.  The audit's
            // 1 560 re-taps were un-decomposable — a raw-step coincidence test
            // at the same (station, satellite, epoch) separates "the gate caught
            // a real observable event" from "the gate is firing on model
            // tension".  Instrumentation only; the flag is unchanged either way.
            // Both units are counted: FLAG ROWS (continuity with the 2026-07-21
            // headline) and distinct funnel ARCS (what actually re-taps).
            if (nc.slip_qc.gap_instrument) {
                ++slip_qc.counters.d5_retaps;
                if (first_this_epoch) ++slip_qc.counters.d5_retap_ambs;
                if (const auto pk = StateKeys::parseAmbiguityKey(ait->second))
                    if (slip_qc.epoch.stepped.count({chord.u, pk->satellite})) {
                        ++slip_qc.counters.d5_retaps_stepped;
                        if (first_this_epoch) ++slip_qc.counters.d5_retap_ambs_stepped;
                    }
            }
            // WL-3 §3.4: a pair chord's flag refloats BOTH bands next epoch (they
            // share the graph edge) — feed the band-2 ambiguity into the funnel too.
            if (lane_on) {
                const auto g2it = carry_arc_amb_key2.find(gk);
                if (g2it != carry_arc_amb_key2.end()) {
                    const auto a2it = g2it->second.find(chord);
                    if (a2it != g2it->second.end())
                        carry_innov_pending.insert({chord.u, a2it->second});
                }
            }
        }
    }
    // F-A attribution dump (log-only, network.mw_wl.d5_dump): one CSV row per D5
    // flag with adjacency tags.  Uses the SAME reverse map as the funnel above.
    //
    // Card D5-A/D2 (docs/SPEC_slip_qc_followups.md §1): the trailing
    // `raw_stepped` column is the CHEAP EXACT-EPOCH raw-step tag — did the
    // receiver's own MW/GF level step at this (station, satellite) on this
    // epoch?  It is deliberately TRI-STATE, not 0/1:
    //   1     — this (station, sat) is in slip_qc.epoch.stepped (Card G's
    //           empirical per-station gate fired);
    //   0     — a step COULD be formed (adjacent-epoch dual-band predecessor)
    //           and it was flat;
    //   blank — no evidence at all (raw observables not computed this run, arc
    //           start / gap / single-band, or the flag's ambiguity key did not
    //           resolve).  Writing 0 there would be a false "no raw step"
    //           claim on exactly the population D5-A exists to decompose;
    //           absence of evidence is not evidence of absence.
    // The ±1-epoch offline join in scripts/grade_slip_agreement.py --d5 stays
    // the source of truth; this column is the zero-cost exact-epoch tag that
    // makes network_d5_flags.csv self-contained.  Log-only either way: the
    // solve never reads it.
    if (mw_d5_dump && carry_innov_gate) {
        const auto& flags  = carry_driver.lastInnovFlags();
        const auto& sigmas = carry_driver.lastInnovFlagSigmas();
        const auto& terms  = carry_driver.lastInnovFlagTerms();
        if (!flags.empty()) {
            // Stream + header are owned by NetworkRunTraces (opened TRUNCATING once in
            // openMwWlDumps, under the SAME arming expression this block tests).  The
            // former per-epoch std::ios::app re-open glued consecutive runs into one
            // file — an A/B whose arms had been re-run unequal numbers of times showed
            // a pure-artifact row-count divergence (hit for real, FC-1 INC-1, 2026-07-30).
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
                    // D2 tri-state raw-step tag (see the banner above).  Reads
                    // the SAME per-epoch evidence the run-end d5_retaps_stepped
                    // counter uses, so the CSV and the counter cannot disagree.
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
                    // Churn card (2026-08-02): the ALGEBRA behind std_innov, so the
                    // Cov(v)-vs-wander question is answerable from this file alone.
                    // Written blank rather than zero when the driver has no term for the
                    // row — a fabricated 0.0 on ẑ or var_v would read as a measurement.
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
                    // network.trace_topology — the map is simply never filled there.
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
                        << t_s0 << ',' << t_zpost << ',' << t_dw << '\n';
                }
            }
        }
    }
    if (mw_tagsets) {   // rotate: this epoch's applied injects tag NEXT install's flags
        d5_inject_chords_prev.swap(d5_inject_chords_now);
        d5_inject_chords_now.clear();
    }
}

} // namespace GnssProcessor

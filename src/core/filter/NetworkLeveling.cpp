/**
 * @file NetworkLeveling.cpp
 * @brief Implementation of @ref networkLeveling: the SB_LEVELING block, relocated verbatim
 *        (dedent 4, no substitution) out of Application::runNetwork at INC-11b.
 */

#include "filter/NetworkLeveling.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkIonoDeweightGate.hpp"
#include "filter/NetworkPivotHandover.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "math/Coordinates.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

void networkLeveling(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    const std::vector<StationAssembly>& asm_res,
    const std::vector<int>& est_index_of,
    NetworkCarryVce& carry_vce_est,
    NetworkPriorResidual& prires_est,
    bool carry_warm,
    double pl_now_s,
    const std::vector<std::string>& registered_clock_sats,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::set<std::string>& shared_phase_keys) {
    // --- Alias preamble: mechanically-derived run-TU aliases (+ reconstructions). --
    auto& sh_model = setup.sh_model;
    auto& sh_model2 = setup.sh_model2;   // Iono-2 dual-layer second shell (null unless armed)
    auto& stations = setup.stations;
    auto& local_carry = ctx.carry.local_carry;
    const bool   pl_warmup_on_conv  = ctx.knobs.pl_warmup_on_conv;  // T1d
    const bool   pl_refloat_on_conv = ctx.knobs.pl_refloat_on_conv; // T1d
    const double pl_conv_rms_thresh = ctx.knobs.pl_conv_rms_thresh;     // T1d
    auto& n_emitted = ctx.counters.n_emitted;
    auto& sh_converged_latch = ctx.carry.sh_converged_latch;
    auto& sh_coeff_prev = ctx.carry.sh_coeff_prev;
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();


    networkHarvestLocalCarries(ctx, config_, est, x_s, stations, est_index_of,
                               asm_res, plan, sh_model.get(), sh_model2.get(),
                               carry_vce_est, prires_est,
                               carry_warm, pl_now_s, registered_clock_sats,
                               shared_phase_keys, registered_code_keys);

    // --- Lane-observatory per-event rows on the existing events stream -------
    // (SPEC_lane_slip_response §7.6(a)).  Emitted here, like the LV-2 rows below,
    // because the trace sinks and the epoch time live on this side of the seam;
    // the spec's "do not create a new stream" rule applies.  One row per attempt,
    // detector-freed (`delta`) and T4-captured (`t4_delta`) alike; `action` is
    // "measured" or the skip channel, `statistic` the lane delta in cycles,
    // `threshold` the raw ΔMW it was differenced against.  Measure-only.  The
    // harvest buffers the rows; they are cleared here and again at the next
    // PASS-A clear().
    if (!ctx.carry.relevel.lane_obs_rows.empty()) {
        const auto cause_str = [](std::uint8_t c) {
            std::string s;
            const auto add = [&s](const char* tag) {
                if (!s.empty()) s += '|';
                s += tag;
            };
            if (c & RelevelEpochState::kCauseMw)        add("MW");
            if (c & RelevelEpochState::kCauseLli)       add("LLI");
            if (c & RelevelEpochState::kCauseIfTd)      add("IFTD");
            if (c & RelevelEpochState::kCauseT4Attrib)  add("T4A");
            if (c & RelevelEpochState::kCauseT4Consens) add("T3C");
            if (c & RelevelEpochState::kCauseWindup)    add("WU");
            if (c & RelevelEpochState::kCauseGap)       add("GAP");
            if (c & RelevelEpochState::kCauseGfJump)    add("GFJ");
            return s.empty() ? std::string("none") : s;
        };
        for (const auto& row : ctx.carry.relevel.lane_obs_rows) {
            const std::string& sta_name =
                (row.sta >= 0 && row.sta < static_cast<int>(stations.size()))
                    ? stations[static_cast<std::size_t>(row.sta)].name
                    : std::string{};
            traces.emitEvent(
                se.time, "LANE_OBS", row.t4 ? "t4_delta" : "delta",
                row.sat, sta_name,
                row.measured ? std::optional<double>(row.delta_cyc) : std::nullopt,
                std::optional<double>(row.d_mw_cyc),
                row.outcome,
                fmt::format("cause={};scope={};pair={}-{};el={:.1f}",
                            cause_str(row.cause),
                            row.t4 ? "t4" : (row.vertex_wide ? "vertex" : "band"),
                            row.sig1.empty() ? "?" : row.sig1,
                            row.sig2.empty() ? "?" : row.sig2, row.el_deg));
        }
        ctx.carry.relevel.lane_obs_rows.clear();
    }
    // --- eq34 (sat,σ) re-reference attribution rows (SPEC_lane_slip_response §7.7)
    // The chain datum counts its re-references, but without these rows only the
    // reattach hygiene emits per-event rows, so a frame step could not be joined
    // against the lane census afterwards.  `eq34_reref` persists until the next
    // chain-datum entry clears it, so at this seam it holds this epoch's set.  One
    // row per (sat,σ); the stream gate (network.trace_events) is inside
    // emitEvent.  Measure-only.
    for (const auto& [rr_sat, rr_sig] : ctx.carry.eq34_reref)
        traces.emitEvent(se.time, "PHASE_DATUM", "eq34_reref", rr_sat, "",
                         std::nullopt, std::nullopt, "re_realize", rr_sig);

    // --- Trace μ of the rx code-bias de-mean (docs/SPEC_level_information_budget.md, Card LV-2)
    // One `lv2_mu` row per receiver-code-bias family per epoch on the existing
    // events stream (the spec's "do not create a new stream for one scalar").  μ is
    // the direct observable of the common-mode feedback that the de-mean removes:
    // near zero in a healthy run, swinging during a frame move.  `action` says
    // whether this epoch's value was fresh or frozen by the thin-mean floor, which
    // makes invariant #7's decoupling edge visible in the row.  The harvest above
    // has just reduced μ; the row is emitted here because the trace sinks and the
    // epoch time live on this side of the seam.
    // Under LV-5 (rx_cb_demean_within) the row carries two numbers: the μ folded
    // against this epoch, and the folded set's own raw common mode measured
    // afterwards.  Their difference (`resid`) is the residual common mode the
    // correction left behind, which distinguishes a working de-mean from one that
    // ran without effect.
    for (const auto& [fam, rec] : ctx.carry.rx_cb_demean_last)
        traces.emitEvent(se.time, "lv2", "rx_cb_demean_mu", "", "",
                         rec.mu, static_cast<double>(ctx.knobs.rx_cb_demean_min_sta),
                         rec.fresh ? "fresh" : "frozen",
                         ctx.knobs.rx_cb_demean_within
                             ? fmt::format("family={};n_sta={};sum_lambda={:.6g};"
                                           "mode=within;folded_mu={:.6g};resid={:.6g}",
                                           fam, rec.n_sta, rec.sum_lam,
                                           rec.folded_mu, rec.folded_mu - rec.mu)
                             : fmt::format("family={};n_sta={};sum_lambda={:.6g};mode=lag1",
                                           fam, rec.n_sta, rec.sum_lam));

    // --- Trace μ of the rx code-bias harvest (docs/SPEC_level_information_budget.md §17 item 3, Card S2H)
    // One `s2h_mu` row per family per epoch on the events stream (the LV-2
    // pattern: no new stream for one scalar).  μ here is the common-mode content
    // the harvest removed from the carried set this epoch: close to the
    // per-epoch solve's own level wander in a healthy run, and the direct
    // observable of a frame event when the member set changes (n / n_used move
    // with it, so invariant #7's edges show in the row).  `action` distinguishes
    // an applied transform from a thin-set identity.
    for (const auto& [fam, rec] : ctx.carry.rx_cb_harvest_last)
        traces.emitEvent(se.time, "s2h", "rx_cb_harvest_mu", "", "",
                         rec.mu, static_cast<double>(ctx.knobs.rx_cb_demean_min_sta),
                         rec.applied ? "applied" : "identity",
                         fmt::format("family={};n={};n_used={};n_screened={};"
                                     "sum_lambda={:.6g};screen_z={:.3g}",
                                     fam, rec.n, rec.n_used, rec.n_screened,
                                     rec.sum_lam, ctx.knobs.rx_cb_harvest_screen_z));

    // --- SH-field convergence detector (latch + one-shot refloat; T1d) ----
    // The co-estimated SH field counts as stationary once the epoch-over-epoch
    // coefficient RMS change drops below the threshold and a₀₀ is inside the physical
    // band; the state then latches (once converged, it stays converged, since the split
    // it defines is the good one).  Runs after the harvest, so a refloat drops the
    // just-stashed carry (possibly contaminated early) and gives a true one-epoch
    // refloat.  carry_warm reads the latch before the next epoch's solve, so the
    // convergence-gated carry warms one epoch after the field first settles.  Gated on
    // either of the two toggles; with both off it is byte-identical (the reference
    // vector and the latch are never touched, and carry_warm keeps the fixed
    // epoch-count gate).
    if ((pl_warmup_on_conv || pl_refloat_on_conv) && sh_model && !sh_converged_latch) {
        const auto& sh_coeffs = sh_model->coeffs();
        Eigen::VectorXd cur(static_cast<Eigen::Index>(sh_coeffs.size()));
        double a00 = std::numeric_limits<double>::quiet_NaN();
        bool ok = true;
        for (std::size_t k = 0; k < sh_coeffs.size() && ok; ++k) {
            try {
                const double v = x_s(est.vtecCoeffColumn(
                    sh_coeffs[k].n, sh_coeffs[k].m, sh_coeffs[k].trig));
                cur(static_cast<Eigen::Index>(k)) = v;
                if (sh_coeffs[k].n == 0 && sh_coeffs[k].m == 0 && sh_coeffs[k].trig == 'C')
                    a00 = v;
            } catch (...) { ok = false; }
        }
        if (ok && cur.size() > 0 && sh_coeff_prev.size() == cur.size()) {
            const double rms_delta = std::sqrt(
                (cur - sh_coeff_prev).squaredNorm() / static_cast<double>(cur.size()));
            const bool a00_in_band = std::isfinite(a00)
                && a00 >= shc.a00_warn_low_tecu && a00 <= shc.a00_warn_high_tecu;
            if (rms_delta < pl_conv_rms_thresh && a00_in_band) {
                sh_converged_latch = true;
                log->info("Network (SH-iono): field CONVERGED at emitted epoch {} (coeff "
                          "RMS Δ {:.4f} < {:.4f} TECU, a₀₀ {:.1f} in band) — phase-leveling "
                          "carry {}.", n_emitted, rms_delta, pl_conv_rms_thresh, a00,
                          pl_warmup_on_conv ? "warm-gate released"
                                            : (pl_refloat_on_conv ? "refloated" : "unchanged"));
                if (pl_refloat_on_conv) {
                    // The largest single carry retirement in the producer: a
                    // whole-fleet clear.  Counted before the clear so the
                    // carry-retirement census can attribute it; otherwise the
                    // census would report zero for this event
                    // (SPEC_withhold_era_release §8).
                    ctx.counters.n_cr_sh_conv_flush +=
                        static_cast<long long>(local_carry.size());
                    ++ctx.counters.n_cr_sh_conv_flushes;
                    // Card PRM phase 3a: one count per carried arc flushed.
                    prmRespond(ctx.counters.prm_resp, ResponseSite::AmbFleetFlush,
                               static_cast<long long>(local_carry.size()));
                    local_carry.clear();   // one-shot flush ⇒ ambiguities refloat next epoch
                }
            }
        }
        if (ok) sh_coeff_prev = cur;   // roll the reference forward until latched
    }

    // --- Post-fit network RAIM (Increment 3, tier T2) -------------------
    // The next step, networkPostFitRaim (NetworkRaim.cpp), is called by the driver
    // right after this seam.  It is the safety net for the zero-process-noise carry.
    // The per-station slip pre-processors (MW/GF TurboEdit, IF-phase
    // triple-difference) are threshold/pool-limited, so a real slip on a setting
    // satellite (few tracking stations, under the pooled-MAD count) can pass
    // undetected, and with zero process noise the stale carry then stays in the
    // products indefinitely.
    //
    // RAIM runs after the joint solve and uses the network redundancy that the
    // per-station detectors cannot see.  A bad carried ambiguity leaks its error
    // into the shared satellite clock/phase bias.  The station at fault hides it
    // there (its own residual stays small), but every other station tracking that
    // satellite is held to its own (correct) carry and cannot absorb the shared
    // shift, so its post-fit phase residual spikes.  Each carried ambiguity's phase
    // residual is a Baarda-style normalized residual: v·√w divided by a robust
    // a-posteriori variance factor σ̂0 = max(raim_s0_floor, 1.4826·median|v·√w|)
    // (floor 1.0 by default) pooled network-wide this epoch, so a satellite is judged
    // an outlier relative to the network's current residual level, not to the mm
    // measurement noise (which would trip on ordinary cm model misfit).  A
    // cross-station consensus on the normalized residual (aggregated per satellite)
    // therefore fires when the contamination has reached the products (the shared
    // clock moved, so many unaffected stations spike) and stays quiet when a slip
    // stayed local and harmless.  This holds while the slipped fraction is small; an
    // epoch where everything moves together inflates σ̂0 itself and needs a separate
    // diagnosis.  RAIM refloats the flagged satellite's carry at every station (marks
    // it pending-reseed, so the next epoch re-estimates it from data, the IF-phase
    // detector's one-shot lifecycle).  It runs after this harvest so that the
    // just-completed stash's clearPendingReseed does not wipe the mark.  Off (the
    // default) is byte-identical (no marks, no state change).
}

} // namespace GnssProcessor

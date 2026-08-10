/**
 * @file NetworkLeveling.cpp
 * @brief Implementation of @ref networkLeveling — the SB_LEVELING block relocated VERBATIM
 *        (dedent-4, zero substitution) out of Application::runNetwork at INC-11b.
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

    // --- Card LV-2 (docs/SPEC_level_information_budget.md): trace μ ------------
    // One `lv2_mu` row per receiver-code-bias family per epoch on the EXISTING
    // events stream (the spec's "do not create a new stream for one scalar").  μ is
    // the direct observable of the flywheel this card removes: it should sit near
    // zero in a healthy run and swing during a frame move, and `action` says
    // whether this epoch's value was FRESH or FROZEN by the thin-mean floor —
    // invariant #7's decoupling edge, visible rather than inferred.  The harvest
    // above has just reduced it; emitted here because the trace sinks and the
    // epoch time live on this side of the seam.
    // Under LV-5 the row carries BOTH numbers: the μ actually folded against this
    // epoch, and the folded set's own raw common mode measured afterwards — their
    // difference is the residual force the correction left behind, which is the only
    // honest way to tell a working de-mean from one that merely ran.
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

    // --- Card S2H (docs/SPEC_level_information_budget.md §17 item 3): trace μ --
    // One `s2h_mu` row per family per epoch on the events stream (the LV-2
    // pattern — no new stream for one scalar).  μ here is the common-mode
    // content the harvest REMOVED from the carried set this epoch: near the
    // per-epoch solve's own level wander in a healthy run, and the direct
    // observable of a frame event when the member set changes (n / n_used move
    // with it — invariant #7's edges, visible in the row rather than inferred).
    // `action` distinguishes an APPLIED transform from a thin-set identity.
    for (const auto& [fam, rec] : ctx.carry.rx_cb_harvest_last)
        traces.emitEvent(se.time, "s2h", "rx_cb_harvest_mu", "", "",
                         rec.mu, static_cast<double>(ctx.knobs.rx_cb_demean_min_sta),
                         rec.applied ? "applied" : "identity",
                         fmt::format("family={};n={};n_used={};n_screened={};"
                                     "sum_lambda={:.6g};screen_z={:.3g}",
                                     fam, rec.n, rec.n_used, rec.n_screened,
                                     rec.sum_lam, ctx.knobs.rx_cb_harvest_screen_z));

    // --- T1d: SH-field convergence detector (latch + one-shot refloat) ----
    // Judge the co-estimated SH field "stationary" once the epoch-over-epoch coefficient
    // RMS change drops below the threshold AND a₀₀ is inside the physical band, then LATCH
    // (once converged, stay converged — the split it defines is the good one).  Runs POST-
    // harvest so a refloat drops the just-stashed (possibly contaminated early) carry → a
    // true one-epoch refloat.  The latch is read pre-solve NEXT epoch by carry_warm, so the
    // convergence-gated carry warms one epoch after the field first settles.  Gated on one
    // of the two toggles ⇒ byte-identical when both are OFF (the reference vector + latch
    // are never touched, carry_warm keeps the fixed epoch-count gate).
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
                if (pl_refloat_on_conv)
                    local_carry.clear();   // one-shot flush ⇒ ambiguities refloat next epoch
            }
        }
        if (ok) sh_coeff_prev = cur;   // roll the reference forward until latched
    }

    // --- Post-fit network RAIM (Increment 3, tier T2) -------------------
    // The safety net for the zero-process-noise carry.  The per-station slip
    // pre-processors (MW/GF TurboEdit, IF-phase triple-difference) are
    // threshold/pool-limited, so a real slip on a satellite that is SETTING (few
    // tracking stations, under the pooled-MAD count) passes undetected — and with
    // zero process noise the stale carry then freezes into the products forever.
    //
    // This runs AFTER the joint solve and spends the network redundancy the
    // per-station detectors cannot see.  A bad carried ambiguity leaks its error
    // into the SHARED satellite clock/phase-bias; the guilty station HIDES it
    // there (its own residual stays small), but every OTHER station tracking that
    // satellite is pinned to its own (correct) carry and CANNOT absorb the shared
    // shift, so its post-fit phase residual spikes.  Each carried ambiguity's phase
    // residual is a Baarda-style normalized residual: v·√w divided by a ROBUST
    // a-posteriori variance factor σ̂0 = max(1, 1.4826·median|v·√w|) pooled network-wide
    // this epoch, so a satellite is judged an outlier relative to the network's CURRENT
    // residual level, not to the mm measurement noise (which would trip on ordinary cm
    // model misfit).  A cross-station consensus on the normalized residual (aggregated
    // per satellite) therefore fires exactly when the contamination reached the PRODUCTS
    // (the shared clock actually moved ⇒ many innocent stations spike) and stays quiet
    // when a slip stayed local and harmless — robust WHILE THE SLIPPED FRACTION IS SMALL
    // (an all-move-together epoch inflates σ̂0 itself and is a separate diagnosis).  It
    // refloats the flagged satellite's
    // carry at EVERY station (marks pending-reseed, so the next epoch re-estimates
    // from data — the IF-phase detector's one-shot lifecycle).  Placed after the
    // harvest so the just-completed stash's clearPendingReseed does not wipe the
    // mark.  OFF (default) = byte-identical (no marks, no state change).
}

} // namespace GnssProcessor

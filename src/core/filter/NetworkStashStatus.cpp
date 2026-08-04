/**
 * @file NetworkStashStatus.cpp
 * @brief Implementation of @ref networkStashStatus — the SB_STASH_STATUS block relocated VERBATIM
 *        (dedent-4, zero substitution) out of Application::runNetwork at INC-11b.
 */

#include "filter/NetworkStashStatus.hpp"

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

void networkStashStatus(
    NetworkRunContext& ctx,
    const GnssConfig& config_,
    const std::shared_ptr<spdlog::logger>& log,
    NetworkRunTraces& traces,
    NetworkSetupResult& setup,
    const SyncedEpoch& se,
    NetworkEstimator& est,
    const Eigen::VectorXd& x_s,
    const NetworkSbasis::Plan& plan,
    NetworkCarryVce& carry_vce_est,
    const std::set<std::string>& clock_refloat_sats,
    int n_arc_refloat_epoch,
    const std::vector<std::string>& registered_clock_sats,
    const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
    const std::set<std::string>& retro_emit_sats,
    const std::set<std::string>& shared_phase_keys,
    double sigma0_adapt,
    int total_pins,
    bool vce_wants_cb,
    bool vce_wants_pb,
    double dt,
    bool datum_changed,
    const std::set<char>& changed_consts,
    int n_clock,
    int n_present,
    int n_pb,
    int n_cb,
    int n_dropped_sats,
    int n_adapt) {
    // --- Alias preamble: mechanically-derived run-TU aliases (+ reconstructions). --
    auto& sh_model = setup.sh_model;
    auto& sh_model2 = setup.sh_model2;   // Iono-2 dual-layer second shell (null unless armed)
    auto& stations = setup.stations;
    const double q_pb    = ctx.knobs.q_pb;   // m²/s
    const double q_cb    = ctx.knobs.q_cb;    // m²/s (W6 OSB carry-forward)
    auto& trace                = traces.trace;
    auto& epoch_status_trace   = traces.epoch_status_trace;
    auto& prev_carry = ctx.carry.prev_carry;
    auto& eq34_reref = ctx.carry.eq34_reref;
    const double pb_var_floor  = ctx.knobs.pb_var_floor;  // B4: D2 phase-bias carry floor; 0 = off
    const double cb_var_floor  = ctx.knobs.cb_var_floor;  // B4: D2 code-bias carry floor;  0 = off
    const bool   carry_vce     = ctx.knobs.carry_vce;
    auto& prev_tow = ctx.counters.prev_tow;
    auto& prev_week = ctx.counters.prev_week;
    auto& n_emitted = ctx.counters.n_emitted;
    const bool   phase_hold_on    = ctx.knobs.phase_hold_on;
    const bool scoped_handover = ctx.knobs.scoped_handover;
    const auto handover_affects = [&](const std::string& sat) {
        return datum_changed
            && datumChangeAffectsKey(sat, changed_consts, scoped_handover);
    };
    auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
        traces.emitEpochStatus(se.time, disposition, r);
    };


    // M2 (LS-VCE): snapshot the D2 shared-carry priors that were FOLDED this
    // epoch, keyed by identity, BEFORE the clear — their innovations against
    // the fresh solve are formed after the re-stash below.  Mirror the fold's
    // skip predicate so only ordinary (non-reinit, non-held, non-retired,
    // non-re-referenced) carries qualify.
    std::map<std::string, std::pair<double, double>> vce_shared_prior;  // key -> (prior value, var)
    if (carry_vce && (vce_wants_pb || vce_wants_cb)) {
        for (const CarryPrior& cp : prev_carry) {
            if (cp.inflate_q) continue;                       // clock-monitor reinit, not an ordinary carry
            if (cp.kind == SharedParamKind::PhaseBias) {
                if (!vce_wants_pb || phase_hold_on) continue;                 // hold path ≠ a q-carry
                if (eq34_reref.count({cp.sat, cp.signal})) continue;         // Card 3 D4: marginalized
            } else if (cp.kind == SharedParamKind::CodeBias) {
                if (!vce_wants_cb) continue;
            } else {
                continue;                                     // Clock / VtecSh out of scope (own models)
            }
            if (handover_affects(cp.sat)) continue;           // RTA-2 retired-datum (scoped mode)
            vce_shared_prior.emplace(sharedCarryKey(cp.kind, cp.sat, cp.signal),
                                     std::make_pair(cp.value, cp.var));
        }
    }

    networkStashCarry(ctx, config_, est, x_s, plan, sh_model.get(), sh_model2.get(),
                      clock_refloat_sats, registered_clock_sats,
                      shared_phase_keys, registered_code_keys, log);

    // M2 (LS-VCE): form the PB/CB shared-carry innovations against this epoch's
    // fresh solve and fold them into the monitor (the D2 analogue of the
    // ambiguity-carry feed above).  In the re-stashed prev_carry, cp.value /
    // cp.var are the SOLVED value / posterior variance; the fold prior is the
    // snapshot taken before the clear.  Gated + best-effort skip-mirroring, so
    // OFF is byte-identical and a mis-mirrored contaminated key at most nudges
    // a monitor number (never the solve).
    if (carry_vce && !vce_shared_prior.empty()) {
        for (const CarryPrior& cp : prev_carry) {
            if (cp.inflate_q) continue;
            const bool is_pb = (cp.kind == SharedParamKind::PhaseBias) && vce_wants_pb;
            const bool is_cb = (cp.kind == SharedParamKind::CodeBias)  && vce_wants_cb;
            if (!is_pb && !is_cb) continue;
            const auto it = vce_shared_prior.find(
                sharedCarryKey(cp.kind, cp.sat, cp.signal));
            if (it == vce_shared_prior.end()) continue;      // no folded prior last epoch
            const double var_prev = it->second.second;
            const double q     = is_cb ? q_cb : q_pb;
            const double floor = is_cb ? cb_var_floor : pb_var_floor;
            double fold_var = var_prev + q * std::max(0.0, dt);
            if (floor > 0.0) fold_var = std::max(fold_var, floor);
            carry_vce_est.addRow(is_cb ? "cb" : "pb", cp.sat,
                                 it->second.first - cp.value,   // carried − solved
                                 dt, cp.var, fold_var);
        }
    }

    // --- Trace ----------------------------------------------------------
    double sumsq = 0.0, maxabs = 0.0;
    for (const std::string& sat : registered_clock_sats) {
        const double v = x_s(est.clockColumn(sat));
        sumsq += v * v;
        maxabs = std::max(maxabs, std::abs(v));
    }
    const double rms = (n_clock > 0) ? std::sqrt(sumsq / n_clock) : 0.0;
    if (trace) {
        trace << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
              << n_present << ',' << n_clock << ',' << n_pb << ','
              << plan.declared_dimension << ",1,1,"
              << fmt::format("{:.4f}", rms) << ',' << fmt::format("{:.4f}", maxabs)
              << ',' << total_pins << '\n';
    }
    // L1: the emitted-epoch disposition row — the median+std+worst-sat clock spread
    // (over the SAME registered_clock_sats / x_s(clockColumn) series as the rms above)
    // plus the QC action counts.  Computed only when the trace is open.
    if (epoch_status_trace.is_open()) {
        EpochStatusRow es;
        es.sigma0          = sigma0_adapt;   // 0.0 unless within-epoch RAIM ran this epoch
        es.n_adapt         = n_adapt;
        es.n_present       = n_present;
        es.n_clock_sats    = n_clock;
        es.n_refloat_sats  = static_cast<int>(clock_refloat_sats.size());
        es.n_retro_applied = static_cast<int>(retro_emit_sats.size());   // bumps APPLIED this emit
        es.n_arc_refloat   = n_arc_refloat_epoch;   // T2d: isolated-arc RAIM refloats this epoch
        es.datum_changed   = datum_changed ? 1 : 0;
        // P1-1 (SPEC_day129_frame_guards): this epoch's carry churn, differenced
        // from the run-cumulative driver counters in NetworkInstallCarry.  The
        // §3.2 trigger hypothesis is a D5 avalanche — testable only per epoch.
        es.n_d5_flags      = ctx.carry.ep_d5_flags;
        es.n_d5_retaps     = ctx.carry.ep_d5_retaps;
        es.n_chord_births  = ctx.carry.ep_chord_births;
        es.n_slip_refloats = ctx.carry.ep_slip_refloats;
        // P1-2: the iono-deweight weight field, harvested at the serial
        // NetworkShHealth advance() site.  -1 = the gate did not advance this
        // epoch (inactive, or a held datum epoch) ⇒ leave the columns blank
        // rather than write a misleading "0 engaged".
        if (ctx.carry.ep_dw_engaged >= 0) {
            es.n_dw_engaged = ctx.carry.ep_dw_engaged;
            es.n_dw_zero    = ctx.carry.ep_dw_zero;
            es.dw_min_w     = ctx.carry.ep_dw_min_w;
            es.dw_mean_w    = ctx.carry.ep_dw_mean_w;
        }
        // P1-4: measure-only post-fit variance factor (blank unless armed).
        if (ctx.carry.ep_pf_rows > 0) {
            es.sigma0_pf       = ctx.carry.ep_sigma0_pf;
            es.sigma0_pf_code  = ctx.carry.ep_sigma0_pf_code;
            es.sigma0_pf_phase = ctx.carry.ep_sigma0_pf_phase;
            es.n_pf_rows       = ctx.carry.ep_pf_rows;
        }
        // Redundancy census (docs/SPEC_redundancy_census.md): the seam stamps the
        // TOW it computed for, so a strided run leaves these BLANK on the epochs
        // between sweeps instead of repeating the last sweep's numbers.  The
        // census seam runs in SB_FRAMEDIAG, i.e. before this block, every epoch.
        if (std::abs(ctx.carry.redun_ep_tow - se.time.tow) < 1e-6) {
            es.n_redun_rows         = ctx.carry.ep_redun_rows;
            es.n_uncheckable_rows   = ctx.carry.ep_uncheckable_rows;
            es.uncheckable_max_s    = ctx.carry.ep_uncheckable_max_s;
            es.n_unobservable_local = ctx.carry.ep_unobservable_local;
        }
        // PREFIT census (docs/SPEC_prefit_census_observability.md INC-1): the walk
        // runs in SB_FRAMEDIAG, i.e. before this block, and sets ep_pc_rows > 0
        // only on an epoch it actually classified rows for.  Blank otherwise.
        if (ctx.carry.ep_pc_rows > 0) {
            EpochStatusRow::PrefitCensusStats pc;
            for (int t = 0; t < kNetDataClasses; ++t) {
                pc.max_s[t]     = ctx.carry.ep_pc_max_s[t];
                pc.p99_s[t]     = ctx.carry.ep_pc_p99_s[t];
                pc.n_gt_10[t]   = ctx.carry.ep_pc_n_gt_10[t];
                pc.n_gt_1000[t] = ctx.carry.ep_pc_n_gt_1000[t];
            }
            es.prefit_census = pc;
        }
        // Measurement-admission census (docs/SPEC_measurement_admission.md INC-3):
        // the seam runs pre-assembly, i.e. long before this block, and stamps the
        // TOW it screened.  An epoch it skipped (every station under adm_min_sats,
        // or the card disarmed) leaves the columns BLANK rather than repeating the
        // last screened epoch's numbers.
        if (std::abs(ctx.carry.adm_ep_tow - se.time.tow) < 1e-6) {
            es.adm_sta_worst_z = ctx.carry.ep_adm_sta_worst_z;
            es.adm_net_worst_z = ctx.carry.ep_adm_net_worst_z;
        }
        // PD-safe DD fold (docs/SPEC_pd_safe_dd_fold.md INC-0): read STRAIGHT off the
        // kernel, which by this point holds the ACCEPTED solve's fold (a within-epoch RAIM
        // re-solve re-folds and overwrites, and NetworkEstimator::beginEpoch resets it once
        // per epoch).  `blocks == 0` means NO fold ran this epoch — carry off, no chords —
        // and the columns correctly stay BLANK.  Going through a ctx.carry stamp instead
        // would report a default-constructed min_pivot_rel of 1.0 on a chordless epoch,
        // i.e. a perfectly conditioned factorization that was never computed, and 1.0 is
        // ALSO what a genuine single-chord block reports (caught by the card's own review,
        // 2026-08-03).  "Zero degenerate blocks" and "the carry never ran" are different
        // statements, and conflating them is how the frozen 78 % stayed invisible.
        if (const auto& dfs = est.ddFoldStats(); dfs.blocks > 0) {
            es.ddfold_degen_blocks  = dfs.degen_blocks;
            es.ddfold_degen_rows    = dfs.degen_rows;
            es.ddfold_min_pivot_rel = dfs.min_pivot_rel;
        }
        std::vector<double> cvals; cvals.reserve(registered_clock_sats.size());
        for (const std::string& sat : registered_clock_sats)
            cvals.push_back(x_s(est.clockColumn(sat)));
        if (!cvals.empty()) {
            std::vector<double> sorted = cvals;
            std::sort(sorted.begin(), sorted.end());
            const size_t m = sorted.size();
            const double median = (m % 2) ? sorted[m / 2]
                                          : 0.5 * (sorted[m / 2 - 1] + sorted[m / 2]);
            double ss = 0.0, worst = 0.0;
            std::string worst_sat_name;
            for (size_t k = 0; k < cvals.size(); ++k) {
                const double d = cvals[k] - median;
                ss += d * d;
                if (std::abs(d) > worst) { worst = std::abs(d); worst_sat_name = registered_clock_sats[k]; }
            }
            es.clock_median_m = median;
            es.clock_std_m    = std::sqrt(ss / static_cast<double>(m));
            es.worst_sat      = worst_sat_name;
            es.worst_dev_m    = worst;
        }
        // Phase 2.2: an SH-gate-withheld epoch ran the full solve/carry/QC but
        // emitted nothing — its disposition row says so (the row itself is the
        // per-epoch out-of-band count the handoff §4.2 lesson demands).
        emitEpochStatus(config_.getNetworkConfig().sh_health_gate
                            && ctx.carry.sh_gate_engaged
                        ? "sh_gate_withheld" : "emitted", es);
    }

    const bool sh_gate_withheld_ep =
        config_.getNetworkConfig().sh_health_gate && ctx.carry.sh_gate_engaged;
    if (!sh_gate_withheld_ep && (n_emitted == 1 || n_emitted % 60 == 0)) {
        log->info("Network: TOW {:.1f} emitted epoch {} — {} stations, {} sat clocks, "
                  "{} phase biases, {} code biases ({} single-station sat(s) dropped), "
                  "datum dim {}, clock-Δ rms {:.3f} m",
                  se.time.tow, n_emitted, n_present, n_clock, n_pb, n_cb, n_dropped_sats,
                  plan.declared_dimension, rms);
        if (config_.estimateNetworkIonoSh() && sh_model) {
            double a00 = 0.0;
            try { a00 = x_s(est.vtecCoeffColumn(0, 0, 'C')); } catch (...) {}
            if (!sh_model2) {
                log->info("Network[SH-iono]:   mean VTEC (a00) = {:.2f} TECU over {} coeffs",
                          a00, sh_model->numCoeffs());
            } else {
                double a00_2 = 0.0;
                try { a00_2 = x_s(est.vtecCoeffColumn(0, 0, 'C', 1)); } catch (...) {}
                log->info("Network[SH-iono]:   mean VTEC (a00) = {:.2f} TECU "
                          "(layer1 {:.2f} + layer2 {:.2f}) over {}+{} coeffs",
                          a00 + a00_2, a00, a00_2, sh_model->numCoeffs(),
                          sh_model2->numCoeffs());
            }
        }
    }

    prev_week = se.time.week; prev_tow = se.time.tow;
}

} // namespace GnssProcessor

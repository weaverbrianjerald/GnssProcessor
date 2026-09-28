/**
 * @file NetworkStashStatus.cpp
 * @brief Implementation of @ref networkStashStatus: the SB_STASH_STATUS block relocated without
 *        change (dedent by 4, no substitution) out of Application::runNetwork at INC-11b.
 */

#include "filter/NetworkStashStatus.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "common/Constants.hpp"
#include "common/PppUtils.hpp"
#include "filter/NetworkCarryVce.hpp"
#include "filter/NetworkClockAnchor.hpp"     // gauge projection shared with the guard (§2.1 of card Q3)
#include "filter/NetworkPriorResidual.hpp"   // prior-residual census (SPEC_prior_residual_census)
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
    NetworkPriorResidual& prires_est,
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
    // --- Alias preamble: mechanically derived run-TU aliases (and reconstructions). --
    auto& sh_model = setup.sh_model;
    auto& sh_model2 = setup.sh_model2;   // Iono-2 dual-layer second shell (null unless armed)
    auto& stations = setup.stations;
    const double q_pb    = ctx.knobs.q_pb;   // m²/s
    const double q_cb    = ctx.knobs.q_cb;    // m²/s (W6 OSB carry-forward)
    auto& trace                = traces.trace;
    auto& epoch_status_trace   = traces.epoch_status_trace;
    auto& prev_carry = ctx.carry.prev_carry;
    auto& eq34_reref = ctx.carry.eq34_reref;
    // The anchored election's re-reference set (queue item 1(d)).  Every predicate
    // below that mirrors the injector's eq34_reref skip mirrors this one too, clause
    // for clause: the census's `folded` bit decides the dispersion branch (see the
    // prior-residual census note), and the VCE monitor must not read a deliberately
    // unfolded frame row as an ordinary carry innovation.
    const auto& pa_reref = ctx.carry.phase_anchor_reref;
    const double pb_var_floor  = ctx.knobs.pb_var_floor;  // D2 phase-bias carry floor (B4); 0 = off
    const double cb_var_floor  = ctx.knobs.cb_var_floor;  // D2 code-bias carry floor (B4);  0 = off
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
    // Mirrors networkInjectD2CarryPrior's release clause for clause (queue 1(b)).  A
    // re-expressed key folds, so the VCE monitor sees it as an ordinary carry
    // innovation and the prior-residual census labels it Clean, not Frame.  The
    // census's `folded` bit picks the dispersion branch, so a predicate that disagreed
    // with the injector would mis-attribute the re-expressed rows.  With the set empty
    // the behaviour is unchanged.
    const auto sts_released = [&](const std::string& sat) {
        return !ctx.carry.sts_consts.empty() && !sat.empty()
            && ctx.carry.sts_consts.count(sat.front()) > 0;
    };
    auto emitEpochStatus = [&](const char* disposition, const EpochStatusRow& r) {
        traces.emitEpochStatus(se.time, disposition, r);
    };


    // Snapshot the D2 shared-carry priors that were folded this epoch, keyed by
    // identity, before the clear (M2, LS-VCE); their innovations against the fresh
    // solve are formed after the re-stash below.  The fold's skip predicate is
    // mirrored so only ordinary (non-reinit, non-held, non-retired,
    // non-re-referenced) carries qualify.
    std::map<std::string, std::pair<double, double>> vce_shared_prior;  // key -> (prior value, var)
    if (carry_vce && (vce_wants_pb || vce_wants_cb)) {
        for (const CarryPrior& cp : prev_carry) {
            if (cp.inflate_q) continue;                       // clock-monitor reinit, not an ordinary carry
            if (cp.t_park_s > 0.0) continue;                  // parked: not an ordinary carry (a readmit seed included)
            if (cp.kind == SharedParamKind::PhaseBias) {
                if (!vce_wants_pb || phase_hold_on) continue;                 // hold path ≠ a q-carry
                if (eq34_reref.count({cp.sat, cp.signal})) continue;         // marginalized (card 3 D4)
                if (pa_reref.count({cp.sat, cp.signal})) continue;           // marginalized (queue 1(d))
            } else if (cp.kind == SharedParamKind::CodeBias) {
                if (!vce_wants_cb) continue;
            } else {
                continue;                                     // Clock / VtecSh out of scope (own models)
            }
            if (handover_affects(cp.sat) && !sts_released(cp.sat)) continue;  // retired datum, scoped mode (RTA-2); re-expressed keys fold (queue 1(b))
            vce_shared_prior.emplace(sharedCarryKey(cp.kind, cp.sat, cp.signal),
                                     std::make_pair(cp.value, cp.var));
        }
    }

    // Prior-residual census (docs/SPEC_prior_residual_census.md §6): the same
    // pre-clear prior snapshot, widened to the kinds the monitor above skips.  Those
    // skips are correct for estimating a variance component and wrong for a
    // detector, because they remove the rows a screen exists to find.
    //
    // The fold decision and its variance are reproduced here from
    // networkInjectD2CarryPrior's predicate, clause for clause, not approximated:
    // every input (prev_carry, datum_changed, changed_consts, eq34_reref,
    // phase_hold_on, dt, the q ladder, the B4 floors, clk_mon_reinit_var) is still
    // in scope and unchanged at this point.  The `folded` bit decides the dispersion
    // branch, and using the folded form on an unfolded row inflates |t| by about
    // 3000x through the ε clamp, which would put every Frame row in the top ladder
    // bin.
    //
    // Two kinds of row carry no calibration weight (spec §INC-2):
    //  * a `phase_hold` PB: the hold pins the posterior, so ê measures carry-vs-hold
    //    disagreement and neither branch's null applies.  Its null dispersion is
    //    undefined, which is the one admissible reason to censor a row from σ̂₀
    //    (never because its value is large).
    //  * anything whose fold variance could not be reconstructed: reported as raw
    //    formal and flagged, rather than standardized under a nominal q the
    //    mechanism never used.
    struct PriresSharedPrior {
        double value = 0.0, var = 0.0, fold_var = 0.0;
        bool   folded = false, hold = false;
        PriorResidualLabel label = PriorResidualLabel::Clean;
        const char* mech = "";
    };
    std::map<std::string, PriresSharedPrior> prires_shared_prior;
    const bool prires_pb = ctx.knobs.prires_pb && prires_est.enabled();
    const bool prires_cb = ctx.knobs.prires_cb && prires_est.enabled();
    // The satellite-clock family (docs/SPEC_clock_temporal_anchor.md §3, card Q3
    // INC-0).  The VCE monitor above excludes it ("Clock / VtecSh out of scope (own
    // models)"), which suits a carry that is not being calibrated; the clock anchor
    // derives its q from the measured dispersion of this prior, so the census
    // includes it.  The census standardizes by `fold_var − post_var`, which is
    // Var(x̂ − μ) exactly and is the anchor's guard statistic, so the clk family
    // needs only the population, no new statistic.  Trace-only.
    const bool prires_clk = ctx.knobs.prires_clk && prires_est.enabled();
    if (prires_pb || prires_cb || prires_clk) {
        const double clk_mon_reinit_var = ctx.knobs.clk_mon_reinit_var;
        const bool   scoped_handover_k  = ctx.knobs.scoped_handover;
        // The injector's outer gate: with a changed datum and scoped handover off,
        // the whole D2 carry block is skipped and nothing folded this epoch.  That is
        // a network-wide Frame edge (standing invariant #7 on the carry layer), and
        // its rows are reported under that label.
        const bool outer_gate = (!datum_changed || scoped_handover_k)
                             && !prev_carry.empty();
        for (const CarryPrior& cp : prev_carry) {
            if (cp.t_park_s > 0.0) continue;   // parked: skipped, even a readmit seed that folded
            const bool is_pb  = (cp.kind == SharedParamKind::PhaseBias) && prires_pb;
            const bool is_cb  = (cp.kind == SharedParamKind::CodeBias)  && prires_cb;
            const bool is_clk = (cp.kind == SharedParamKind::Clock)     && prires_clk;
            if (!is_pb && !is_cb && !is_clk) continue;   // VtecSh out of scope (own model)
            if (is_clk && ctx.knobs.clk_model_on) continue;  // clock is on clock_osc, never stashed
            PriresSharedPrior p;
            p.value = cp.value;
            p.var   = cp.var;
            const double q     = is_cb ? q_cb : (is_clk ? ctx.knobs.q_clock : q_pb);
            const double floor = is_cb ? cb_var_floor : (is_clk ? 0.0 : pb_var_floor);
            // var_eff as the injector forms it: the reinit variance on a
            // clock-monitor-flagged key (a soft large-q reinitialization, not the
            // nominal q recompute), else q·Δt inflation with the B4 floor.
            double var_eff = cp.inflate_q ? clk_mon_reinit_var
                                          : cp.var + q * std::max(0.0, dt);
            if (!cp.inflate_q && floor > 0.0) var_eff = std::max(var_eff, floor);
            // The census standardizes by the fold variance as used (M2 v2,
            // SPEC_carry_vce_apply.md §3), so its s absorbs α², and the census's
            // per-family σ̂₀ becomes an independent check on the apply (field gate 4:
            // σ̂₀,f on an applied family should move toward 1).  Mirrors the injector's exclusion
            // of inflate_q keys.  The factor is 1 when the apply path is off, so the
            // output is byte-identical.
            if (!cp.inflate_q && !is_clk)
                var_eff *= ctx.carry.vce_apply.factor(
                               is_cb ? NetworkCarryVceApply::kCb
                                     : NetworkCarryVceApply::kPb, cp.sat);
            // With the clock anchor armed, the injector does not use the expression
            // above for a clock key; it uses the anchor's own (per-satellite Δt,
            // floored) variance.  The decision is read back rather than re-derived,
            // so the census standardizes by the variance the mechanism used (card Q3).
            if (is_clk && ctx.knobs.cta_on && !cp.inflate_q) {
                const auto ait = ctx.carry.clk_anchor.sat.find(cp.sat);
                if (ait != ctx.carry.clk_anchor.sat.end() && ait->second.last_var_eff > 0.0)
                    var_eff = ait->second.last_var_eff;
            }
            p.fold_var = var_eff;
            // The per-key skip chain, in the injector's own order.
            if (!outer_gate) {
                p.label = PriorResidualLabel::Frame;
                p.mech  = datum_changed ? "datum_changed" : "no_carry";
            } else if (is_pb && phase_hold_on) {
                p.label = PriorResidualLabel::Frame;
                p.mech  = "phase_hold";
                p.hold  = true;                  // null dispersion undefined ⇒ no σ̂₀ weight
            } else if (handover_affects(cp.sat) && !sts_released(cp.sat)) {
                p.label = PriorResidualLabel::Frame;
                p.mech  = "handover";
            } else if (is_pb && eq34_reref.count({cp.sat, cp.signal})) {
                p.label = PriorResidualLabel::Frame;
                p.mech  = "eq34_reref";
            } else if (is_pb && pa_reref.count({cp.sat, cp.signal})) {
                p.label = PriorResidualLabel::Frame;   // anchored-election handover (queue 1(d))
                p.mech  = "phase_anchor_reref";
            } else if (!(var_eff > 0.0)) {
                p.mech  = "var_nonpos";          // injector's `continue`; nothing fired
            } else {
                // The C1 dense branch (network.shared_carry_full_cov) folds the same
                // key set with the same var_eff on the diagonal; only the retained
                // off-diagonals differ, and the marginal prior on any one state is
                // unchanged.  That is the property that makes the M2 carry
                // innovation independent of the representation, so `folded` and
                // `fold_var` are identical on both branches and this chain does not
                // fork.  (A dense fold that throws falls back per key to the
                // diagonal path with the same var_eff, so the fallback agrees too.)
                p.folded = true;
                // An inflate_q key does fold, with the large reinit variance rather
                // than the nominal q.  That is a Frame event (a soft
                // reinitialization); standardizing it under the nominal q is the
                // error spec §INC-2 names.
                if (cp.inflate_q) {
                    p.label = PriorResidualLabel::Frame;
                    p.mech  = "inflate_q";
                }
                // A clock key the anchor guard withdrew this epoch folded and was
                // then un-folded before the emitted solve, so its residual is neither
                // the folded nor the unfolded statistic.  Labelled Frame with the
                // mechanism named, the same treatment as a held phase bias (card Q3).
                if (is_clk && ctx.knobs.cta_on) {
                    const auto ait = ctx.carry.clk_anchor.sat.find(cp.sat);
                    if (ait != ctx.carry.clk_anchor.sat.end() && !ait->second.folded) {
                        p.label  = PriorResidualLabel::Frame;
                        p.mech   = "cta_refloat";
                        p.folded = false;
                        p.hold   = true;   // null dispersion undefined ⇒ no σ̂₀ weight
                    }
                }
            }
            prires_shared_prior.emplace(sharedCarryKey(cp.kind, cp.sat, cp.signal),
                                        std::move(p));
        }
    }

    networkStashCarry(ctx, config_, traces, se.time, est, x_s, plan, sh_model.get(), sh_model2.get(),
                      clock_refloat_sats, registered_clock_sats,
                      shared_phase_keys, registered_code_keys, log);

    // Form the PB/CB shared-carry innovations against this epoch's fresh solve and
    // fold them into the monitor (M2, LS-VCE; the D2 analogue of the ambiguity-carry
    // feed).  In the re-stashed prev_carry, cp.value / cp.var are
    // the solved value and posterior variance; the fold prior is the snapshot taken
    // before the clear.  Gated, with best-effort skip mirroring: off, the output is
    // byte-identical, and a mis-mirrored contaminated key can change only a monitor
    // number, never the solve.
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
            double fold_var_hand = var_prev + q * std::max(0.0, dt);
            if (floor > 0.0) fold_var_hand = std::max(fold_var_hand, floor);
            // The D2 injector multiplied this same hand value by α² before folding
            // (M2 v2; networkInjectD2CarryPrior's vce_scale_shared), and the factor
            // table has not changed since; the step below is the epoch's only write.
            // The used weight is therefore reconstructed exactly, and the hand value
            // is passed alongside as the factor iteration's α²-independent denominator.
            const double fold_var =
                fold_var_hand * ctx.carry.vce_apply.factor(
                                    is_cb ? NetworkCarryVceApply::kCb
                                          : NetworkCarryVceApply::kPb, cp.sat);
            carry_vce_est.addRow(is_cb ? "cb" : "pb", cp.sat,
                                 it->second.first - cp.value,   // carried − solved
                                 dt, cp.var, fold_var, fold_var_hand);
        }
    }

    // Prior-residual census: the same innovation over the widened population.
    // In the re-stashed prev_carry, cp.value / cp.var are the solved value and
    // posterior variance; the prior and its fold decision come from the snapshot
    // taken before the clear.  A key with no snapshot entry had no prior last
    // epoch (a fresh registration) and is absent, since there is no prior residual
    // without a prior.
    if (!prires_shared_prior.empty()) {
        // The satellite-clock family's ê has its per-constellation common mode (the
        // median) removed before standardizing (card Q3 INC-0).  The clock is the one
        // carried family with a free per-epoch gauge (receiver clocks are white,
        // q = 1000), so the raw carried−solved difference carries the epoch's whole
        // gauge re-realization on every satellite.  Without the removal the family
        // would report the gauge, not the per-satellite content, and its σ̂₀, from
        // which the anchor's q is derived, would be meaningless (see standing
        // invariant #5).  The guard uses the same statistic, so the ladder measured here is the
        // distribution its threshold is read from.
        std::map<char, double> clk_med;
        if (prires_clk) {
            std::map<char, std::vector<double>> by_const;
            for (const CarryPrior& cp : prev_carry) {
                if (cp.kind != SharedParamKind::Clock || cp.sat.empty()) continue;
                const auto it = prires_shared_prior.find(
                    sharedCarryKey(cp.kind, cp.sat, cp.signal));
                if (it == prires_shared_prior.end()) continue;
                by_const[cp.sat[0]].push_back(it->second.value - cp.value);
            }
            for (auto& [c, v] : by_const) {
                std::nth_element(v.begin(),
                                 v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2),
                                 v.end());
                clk_med[c] = v[v.size() / 2];
            }
        }
        // Gauge projection of the clock variances (§2.1 of card Q3).  The census
        // standardizes ê by `fold_var − post_var`, and ê above has the
        // per-constellation common mode removed, so both halves must be the variances
        // of that contrast; the raw marginals still carry the common-mode variance
        // the median took out.  Under the gauge-free fold the mismatch is large: the
        // prior puts no information on the gauge, so the raw posterior marginal is
        // data-only along it and can exceed the raw prior, driving `disp` negative.
        //
        // The projection goes through the same helper the guard uses, so the census
        // remains a cross-check on the guard rather than a second implementation
        // that can drift.  It is applied to whichever halves
        // `network.clock_anchor_gauge_free_stat` selects, so census and guard are the
        // same statistic in every configuration, `auto` included.
        std::map<std::string, double> clk_proj_post, clk_proj_prior;
        if (prires_clk && ctx.knobs.cta_on
            && (ctx.knobs.cta_stat_proj_post || ctx.knobs.cta_stat_proj_prior)) {
            const Eigen::MatrixXd& cov = est.sharedCovariance();
            struct ClkMember { std::string sat; Eigen::Index col; double fold_var; };
            std::map<char, std::vector<ClkMember>> by_c;
            for (const CarryPrior& cp : prev_carry) {
                if (cp.kind != SharedParamKind::Clock || cp.sat.empty()) continue;
                const auto it = prires_shared_prior.find(
                    sharedCarryKey(cp.kind, cp.sat, cp.signal));
                // Folded rows only: an unfolded row is on the other dispersion
                // branch and never reaches `fold_var − post_var`.  This is the
                // membership the guard uses, so the two groups agree.
                if (it == prires_shared_prior.end() || !it->second.folded) continue;
                Eigen::Index col;
                try { col = est.clockColumn(cp.sat); } catch (...) { continue; }
                if (col >= 0 && col < cov.rows() && col < cov.cols())
                    by_c[cp.sat[0]].push_back(ClkMember{cp.sat, col, it->second.fold_var});
            }
            for (const auto& [c, v] : by_c) {
                (void)c;
                if (v.size() < 2) continue;   // a lone clock is the gauge
                if (ctx.knobs.cta_stat_proj_post) {
                    std::vector<Eigen::Index> cols;
                    cols.reserve(v.size());
                    for (const auto& m : v) cols.push_back(m.col);
                    const std::vector<double> pv = clockAnchorGaugeProject(cov, cols);
                    for (std::size_t a = 0; a < pv.size(); ++a) clk_proj_post[v[a].sat] = pv[a];
                }
                if (ctx.knobs.cta_stat_proj_prior) {
                    std::vector<double> vv;
                    vv.reserve(v.size());
                    for (const auto& m : v) vv.push_back(m.fold_var);
                    const std::vector<double> pv = clockAnchorGaugeProjectDiag(vv);
                    for (std::size_t a = 0; a < pv.size(); ++a) clk_proj_prior[v[a].sat] = pv[a];
                }
            }
        }
        for (const CarryPrior& cp : prev_carry) {
            const bool is_pb  = (cp.kind == SharedParamKind::PhaseBias) && prires_pb;
            const bool is_cb  = (cp.kind == SharedParamKind::CodeBias)  && prires_cb;
            const bool is_clk = (cp.kind == SharedParamKind::Clock)     && prires_clk;
            if (!is_pb && !is_cb && !is_clk) continue;
            const auto it = prires_shared_prior.find(
                sharedCarryKey(cp.kind, cp.sat, cp.signal));
            if (it == prires_shared_prior.end()) continue;
            const PriresSharedPrior& p = it->second;
            NetworkPriorResidual::Input in;
            in.family   = is_cb ? "cb" : (is_clk ? "clk" : "pb");
            in.sat      = cp.sat;
            in.signal   = cp.signal;
            in.station  = -1;                    // shared: this prior has no station
            in.e        = p.value - cp.value;    // carried − solved
            if (is_clk && !cp.sat.empty()) {
                const auto mit = clk_med.find(cp.sat[0]);
                if (mit != clk_med.end()) in.e -= mit->second;
            }
            in.fold_var = p.fold_var;
            in.post_var = cp.var;
            // A clock row whose ê has the common mode removed uses the gauge-projected
            // halves (§2.1 of card Q3), so `disp = fold_var − post_var` is the dispersion
            // of the statistic reported.  Guarded on finiteness and on the
            // pair staying ordered: a non-positive or inverted projected pair means the
            // group was degenerate this epoch, and the raw pair (mismatched but well
            // defined) is reported instead of a negative dispersion the ladder would
            // clamp into its top bin.
            if (is_clk && !cp.sat.empty()) {
                const auto fit = clk_proj_prior.find(cp.sat);
                const auto pit = clk_proj_post.find(cp.sat);
                const double fv = (fit != clk_proj_prior.end()) ? fit->second : in.fold_var;
                const double pvv = (pit != clk_proj_post.end()) ? pit->second : in.post_var;
                if (std::isfinite(fv) && std::isfinite(pvv) && fv > 0.0 && pvv >= 0.0 && fv > pvv) {
                    in.fold_var = fv;
                    in.post_var = pvv;
                }
            }
            in.gap_s    = std::max(0.0, dt);
            in.folded   = p.folded;
            in.label    = p.label;
            in.mech     = p.mech;
            // The one admissible censoring: a held phase bias (or a clock key the
            // anchor guard withdrew) has no null dispersion, so its ê is neither the
            // folded nor the unfolded statistic.  It is counted, reported as raw
            // formal, and kept out of the calibration because the null is undefined,
            // not because the value is large.
            if (p.hold) { in.calibrate = false; in.var_nominal = true; }
            prires_est.addRow(in);
        }
    }

    // Prior-residual census: drain the epoch.  This call must stay here, after the
    // pb/cb/clk feed above (the last of the five) and before the L1 epoch-status row
    // below, which reads the ctx.carry.ep_prires_* scalars this call stamps.  Called
    // from the driver instead, it would publish the census columns one epoch late
    // (the same reason networkStashCarry is called from this seam rather than the
    // driver).  No-op when the census is unarmed.
    if (prires_est.enabled())
        networkPriorResidualEmit(ctx, config_, log, traces, se, stations, prires_est);

    // One LASER 5-95 factor iteration per epoch (docs/SPEC_carry_vce_apply.md §2,
    // M2 v2).  It must run here: after the last feed into the monitor (SB_LEVELING's
    // pl harvest, then the pb/cb feed above) and before any fold of the next epoch.
    // That placement makes the card's contract hold:
    //
    //     the fold of epoch N is scaled by α² estimated from every clean carry row
    //     through epoch N−1
    //
    // This is a strict predict-then-update with no self-reference, at the minimum
    // achievable lag (a row cannot exist until the solve that produced it has run,
    // and that solve already consumed the fold).  It also means the injector and
    // the harvest mirrors above, which all ran earlier in this epoch, read the same
    // factor table.
    //
    // `!datum_changed` is the monitor's own clean gate (NetworkCarryVce::beginEpoch),
    // reused here: a handover's re-levels are frame steps, not carry noise, and a
    // factor that absorbed them would inflate on a datum change.  Every clamp /
    // freeze edge lands in ctx.carry.vce_apply_edges and is emitted below (standing
    // invariant #7: this mechanism changes how much information a carried prior
    // injects, so both of its edges are evented with identities).
    if (ctx.knobs.vce_apply_on) {
        auto& edges = ctx.carry.vce_apply_edges;
        edges.clear();
        ctx.carry.vce_apply.stepEpoch(carry_vce_est, !datum_changed, &edges);
        for (const auto& e : edges)
            traces.emitEvent(se.time, "VCE_APPLY",
                             e.entered ? e.what : "release",
                             /*sat=*/std::string(1, e.sys ? e.sys : '?'),
                             /*station=*/"",
                             e.alpha2, e.target,
                             e.entered ? "hold" : "resume",
                             fmt::format("family={} edge={} reason={}",
                                         NetworkCarryVceApply::familyName(e.family),
                                         e.what, e.reason));
        traces.writeCarryVceApplyRows(se.time, ctx.carry.vce_apply,
                                      ctx.carry.vce_apply_trace_hour);
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
    // The emitted-epoch disposition row (L1): the clock spread (median, std, worst
    // satellite) over the same registered_clock_sats / x_s(clockColumn) series as the
    // rms above, plus the QC action counts.  Computed only when the trace is open.
    if (epoch_status_trace.is_open()) {
        EpochStatusRow es;
        es.sigma0          = sigma0_adapt;   // 0.0 unless within-epoch RAIM ran this epoch
        es.n_adapt         = n_adapt;
        es.n_present       = n_present;
        es.n_clock_sats    = n_clock;
        // n_refloat_sats counts the clock-couple set: post-fit RAIM / clock-QC
        // culprits plus, only under clock_monitor_couple_raim, the within-epoch
        // drops.  The within-epoch tier's own refloats are the appended
        // n_we_refloat_sats column below, so with the couple opt-in off this column
        // reads 0 even when the tier refloats (R0; SPEC_pivot_visibility_gauge_defect
        // §5.07.4 defect 5).
        es.n_refloat_sats  = static_cast<int>(clock_refloat_sats.size());
        es.n_retro_applied = static_cast<int>(retro_emit_sats.size());   // bumps applied this emit
        es.n_arc_refloat   = n_arc_refloat_epoch;   // isolated-arc RAIM refloats this epoch (T2d)
        es.datum_changed   = datum_changed ? 1 : 0;
        // The within-epoch tier's per-epoch observability, parked in ctx.carry by the
        // solve glue (R0).  Sentinel < 0 = the tier did not assess this epoch, and the
        // columns stay blank ("unavailable is not 0").
        if (ctx.carry.ep_sigma0_first >= 0.0) {
            es.sigma0_first = ctx.carry.ep_sigma0_first;
            if (ctx.carry.ep_we_refloat_sats >= 0)
                es.n_we_refloat_sats = ctx.carry.ep_we_refloat_sats;
            if (ctx.carry.ep_we_clk_move_max >= 0.0) {
                es.we_clk_move_max_m   = ctx.carry.ep_we_clk_move_max;
                es.we_clk_move_max_sat = ctx.carry.ep_we_clk_move_sat;
            }
        }
        // This epoch's carry churn, differenced from the run-cumulative driver
        // counters in NetworkInstallCarry (P1-1, SPEC_day129_frame_guards).  The §3.2
        // trigger hypothesis (a D5 avalanche) can only be tested per epoch.
        es.n_d5_flags      = ctx.carry.ep_d5_flags;
        es.n_d5_retaps     = ctx.carry.ep_d5_retaps;
        es.n_chord_births  = ctx.carry.ep_chord_births;
        es.n_slip_refloats = ctx.carry.ep_slip_refloats;
        // The iono-deweight weight field (P1-2), harvested at the serial
        // NetworkShHealth advance() site.  -1 = the gate did not advance this epoch
        // (inactive, or a held datum epoch); the columns are then left blank rather
        // than written as "0 engaged".
        if (ctx.carry.ep_dw_engaged >= 0) {
            es.n_dw_engaged = ctx.carry.ep_dw_engaged;
            es.n_dw_zero    = ctx.carry.ep_dw_zero;
            es.dw_min_w     = ctx.carry.ep_dw_min_w;
            es.dw_mean_w    = ctx.carry.ep_dw_mean_w;
        }
        // Measure-only post-fit variance factor (P1-4; blank unless armed).
        if (ctx.carry.ep_pf_rows > 0) {
            es.sigma0_pf       = ctx.carry.ep_sigma0_pf;
            es.sigma0_pf_code  = ctx.carry.ep_sigma0_pf_code;
            es.sigma0_pf_phase = ctx.carry.ep_sigma0_pf_phase;
            es.n_pf_rows       = ctx.carry.ep_pf_rows;
        }
        // Redundancy census (docs/SPEC_redundancy_census.md): the seam stamps the
        // TOW it computed for, so a strided run leaves these blank on the epochs
        // between sweeps instead of repeating the last sweep's numbers.  The census
        // seam runs in SB_FRAMEDIAG, before this block, every epoch.
        if (std::abs(ctx.carry.redun_ep_tow - se.time.tow) < 1e-6) {
            es.n_redun_rows         = ctx.carry.ep_redun_rows;
            es.n_uncheckable_rows   = ctx.carry.ep_uncheckable_rows;
            es.uncheckable_max_s    = ctx.carry.ep_uncheckable_max_s;
            es.n_unobservable_local = ctx.carry.ep_unobservable_local;
        }
        // Prefit census (docs/SPEC_prefit_census_observability.md INC-1): the walk
        // runs in SB_FRAMEDIAG, before this block, and sets ep_pc_rows > 0 only on
        // an epoch it classified rows for.  Blank otherwise.
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
        // the seam runs before assembly, well before this block, and stamps the TOW
        // it screened.  An epoch it skipped (every station under adm_min_sats, or
        // the feature disarmed) leaves the columns blank rather than repeating the
        // last screened epoch's numbers.
        if (std::abs(ctx.carry.adm_ep_tow - se.time.tow) < 1e-6) {
            es.adm_sta_worst_z = ctx.carry.ep_adm_sta_worst_z;
            es.adm_net_worst_z = ctx.carry.ep_adm_net_worst_z;
        }
        // PD-safe DD fold (docs/SPEC_pd_safe_dd_fold.md INC-0): read directly off the
        // kernel, which by this point holds the accepted solve's fold (a within-epoch RAIM
        // re-solve re-folds and overwrites, and NetworkEstimator::beginEpoch resets it once
        // per epoch).  `blocks == 0` means no fold ran this epoch (carry off, no chords),
        // and the columns stay blank.  A ctx.carry stamp would instead report a
        // default-constructed min_pivot_rel of 1.0 on a chordless epoch, a perfectly
        // conditioned factorization that was never computed, and 1.0 is also what a
        // genuine single-chord block reports.  "Zero degenerate blocks" and "the carry
        // never ran" must stay distinguishable.
        if (const auto& dfs = est.ddFoldStats(); dfs.blocks > 0) {
            es.ddfold_degen_blocks  = dfs.degen_blocks;
            es.ddfold_degen_rows    = dfs.degen_rows;
            es.ddfold_min_pivot_rel = dfs.min_pivot_rel;
        }
        // Prior-residual census (docs/SPEC_prior_residual_census.md): the emission
        // seam ran above in this epoch and stamped the TOW it drained, so an epoch
        // the census did not reach leaves the columns blank rather than repeating
        // stale numbers.  A genuine 0-row epoch (every carry family absent) still
        // writes 0; "ran, saw nothing" and "never ran" are the two states standing
        // invariant #4 requires a reader to distinguish.
        if (std::abs(ctx.carry.prires_ep_tow - se.time.tow) < 1e-6) {
            es.prires_rows       = ctx.carry.ep_prires_rows;
            es.prires_rows_resp  = ctx.carry.ep_prires_resp;
            es.prires_rows_frame = ctx.carry.ep_prires_frame;
            es.prires_n_reported = ctx.carry.ep_prires_reported;
            es.prires_max_t      = ctx.carry.ep_prires_max_t;
            // Constraint-aware global σ̂₀ (docs/SPEC_prior_residual_census.md §14):
            // same TOW stamp, same "unavailable is not 0" convention.  The flags are
            // written whenever the rider ran, including as 0, so a clean epoch and an
            // un-run one are distinguishable from this file alone; the two values
            // stay blank when the epoch could not form them, so "not formed" never
            // reads as "formed at zero".
            if (ctx.knobs.prires_s0g) {
                es.s0g_flags = ctx.carry.ep_s0g_flags;
                if (ctx.carry.ep_s0g_formed)      es.s0g_sigma0  = ctx.carry.ep_s0g_sigma0;
                if (ctx.carry.ep_s0g_tension_def) es.s0g_tension = ctx.carry.ep_s0g_tension;
            }
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
        // An SH-gate-withheld epoch ran the full solve/carry/QC but emitted nothing,
        // and its disposition row says so.  The row is the per-epoch out-of-band count
        // that the handoff §4.2 lesson requires (Phase 2.2).
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

    // Lifecycle of the re-reference set (queue item 1(d)): it is cleared here, in
    // the epoch tail that only a solve that committed its carry reaches, and not at
    // the election.  A withheld epoch (rank gate / A4 σ̂₀) skips this function, so
    // its marks persist into the next epoch's fold, which still faces the
    // retired-frame prev_carry; an SH-gate epoch runs the stash (emission-only
    // withhold) and clears.  The set is empty when the knob is off, so this is a
    // byte-identical no-op.
    ctx.carry.phase_anchor_reref.clear();

    prev_week = se.time.week; prev_tow = se.time.tow;
}

} // namespace GnssProcessor

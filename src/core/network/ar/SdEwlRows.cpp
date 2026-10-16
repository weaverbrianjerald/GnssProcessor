/**
 * @file SdEwlRows.cpp
 * @brief The SD pre-reduce EWL row stages declared in SdEwlRows.hpp.
 *
 * The bodies were cut verbatim from `networkAppendSdEwlRows`; only the bindings at the
 * head of each stage and the calls between stages are new.
 */

#include "network/ar/SdEwlRows.hpp"

#include "network/ar/SdEwlAr.hpp"             // sdEwlBands, SdEwlFix
#include "network/state/RunContext.hpp"
#include "common/Constants.hpp"
#include "common/GnssIdentity.hpp"            // systemOfSat, letterOfSat, ObservableId
#include "filter/StateKeySchema.hpp"           // StateKeys
#include "filter/StateManager.hpp"
#include "slip/MwSlipDetector.hpp"             // MwArcZdMean, arcZdMean

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace GnssProcessor {

void sdEwlFillBiasStates(const SdEwlRowView& v,
                         std::map<std::pair<std::string, std::string>, int>& pb_state,
                         std::map<std::pair<std::string, std::string>, int>& cb_state) {
    NetStation& st = v.st;

    for (const auto& kv : st.sm->getSatPhaseBiasStates()) {
        const auto p = StateKeys::parseSatPhaseBiasKey(kv.first);
        if (p) pb_state[{ p->satellite, p->signal }] = static_cast<int>(kv.second);
    }
    for (const auto& kv : st.sm->getSatBiasStates()) {
        const auto p = StateKeys::parseSatBiasKey(kv.first);
        if (p) cb_state[{ p->satellite, p->signal }] = static_cast<int>(kv.second);
    }
}

void sdEwlFillCandidates(const SdEwlRowView& v, SdEwlCandidates& cand) {
    NetStation& st = v.st;
    const std::map<int, int>& h2local = v.h2local;

    // --- This station's per-(satellite, band) ambiguity candidates ---------------------
    // Built once from StateManager's own key map (the authority) rather than by
    // reconstructing a key string per candidate: the ambiguity key format belongs to the
    // state layer, and a reconstructed key that does not round-trip fails without any
    // error (every arc looks absent).  A candidate also needs a local column this epoch:
    // h2local presence means "this arc has a phase row in A_r now".  Sorted ascending
    // by signal name so the pair search below follows a strict total order
    // (byte-identity: an insertion-order-dependent pick changes the row set from run to
    // run).  Entries are (signal, A_r local column, global state index); the state
    // index serves the correction-frame x_lin term, the local column the design.
    for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
        const auto parts = StateKeys::parseAmbiguityKey(key);
        if (!parts) continue;
        const std::string& sig = parts->signal;
        const auto phase = ObservableId::parse(systemOfSat(parts->satellite), sig);
        if (!phase || phase->type != ObsType::Phase) continue;
        const auto hit = h2local.find(static_cast<int>(sidx));
        if (hit == h2local.end()) continue;         // no phase row on this arc this epoch
        cand[{ parts->satellite, phase->signal.digit }].emplace_back(sig, hit->second,
                                                                     static_cast<int>(sidx));
    }
    for (auto& kv : cand)
        std::sort(kv.second.begin(), kv.second.end());   // ascending signal name first
}

void sdEwlResolveArcs(const SdEwlRowView& v, const Eigen::VectorXd& x_lin,
                      const std::map<std::pair<std::string, std::string>, int>& pb_state,
                      const std::map<std::pair<std::string, std::string>, int>& cb_state,
                      const SdEwlCandidates& cand,
                      std::map<char, std::vector<SdEwlArc>>& by_const) {
    NetStation& st = v.st;
    const NetworkEstimator& est = v.est;
    const std::vector<PppObservation>& sta_obs = v.sta_obs;
    const net::StationTable* fix = v.fix;
    std::size_t station_index = v.station_index;
    const auto& cfg = v.cfg;
    auto& ctr = v.ctr;
    const bool fix_on = v.fix_on;
    const auto& tr = v.tr;

    // --- Extra-wide-lane band pair per constellation -----------------------------------
    // Shared with the increment-2a integer decision through @ref sdEwlBands, a single
    // definition, so the row and the integer folded into it cannot disagree about which
    // lane they are on.  GPS (L2,L5) lambda_EWL 5.861 m; Galileo (E5b,E5a) 9.768 m.
    // BeiDou (deferred) and GLONASS (excluded) produce no rows.
    auto bandsFor = [](char cons, char& b1, char& b2, double& f1, double& f2) -> bool {
        return sdEwlBands(cons, b1, b2, f1, f2);
    };

    const auto xAt = [&x_lin](int idx) -> double {
        return (idx >= 0 && idx < static_cast<int>(x_lin.size())) ? x_lin(idx) : 0.0;
    };

    const auto stateOf = [](const std::map<std::pair<std::string, std::string>, int>& m,
                            const std::string& sat, const std::string& sig) -> int {
        const auto it = m.find({ sat, sig });
        return it == m.end() ? -1 : it->second;
    };

    // --- Resolve every eligible arc, grouped by constellation (sorted: deterministic) --
    // The MW arc key carries the two phase signal names, and the channel the detector
    // holds an arc on is the detector's choice (collectUncombinedPairs' KF-chosen /
    // attribute-priority pick), not this function's.  The candidate pairs are therefore
    // searched in ascending order and the first with a usable arc is taken, instead of
    // electing one channel and declaring the arc missing when the detector picked the
    // other.  A one-channel band (the common case) has one candidate, so the search
    // reduces to a direct lookup.
    for (const auto& o : sta_obs) {
        if (o.satellite.empty()) continue;
        const char cons = letterOfSat(o.satellite);
        char b1 = 0, b2 = 0;
        double fb1 = 0.0, fb2 = 0.0;
        if (!bandsFor(cons, b1, b2, fb1, fb2)) continue;
        ++ctr.sat_seen;

        const auto c1it = cand.find({ o.satellite, b1 });
        const auto c2it = cand.find({ o.satellite, b2 });
        if (c1it == cand.end() || c2it == cand.end()) {
            ++ctr.skip_missing_amb_col;
            tr("NO_AMB_COL", o.satellite, {}, {}, {}, {}, {}, 0, 0, false,
               0.0, 0.0, 0.0, 0.0, 0.0);
            continue;
        }
        // The first candidate pair with a mature arc wins.  The best near-miss is
        // remembered so that a skip is attributed to a cause (never formed / young /
        // unreconstructable) rather than to one combined tally.
        MwSlipDetector::MwArcZdMean zd;                 // the winner
        std::string sig1, sig2;
        int col1 = -1, col2 = -1, sidx1 = -1, sidx2 = -1;
        std::size_t n_cand = 0, best_n = 0;
        bool any_exists = false, any_recon_fail = false;
        std::string near_s1, near_s2;
        // The published integer, if any, for this (station, satellite) (increment 2b).
        // It is looked up but does not influence the channel search below.  Trying the
        // decision's pair first would let the integer path change which pair the float
        // path elects, and so change the solve even when no integer is folded (an
        // `inert` A/B arm, fix armed with the FFR gate unreachable, would then differ
        // from the float arm).  The row elects its pair exactly as increment 1 does, and
        // the fix is used only if it was decided on that pair; otherwise
        // `fix_pair_mismatch` counts it (measured at 0, because 2a searches the same
        // candidates in the same ascending order).
        const SdEwlFix* my_fix = nullptr;
        if (fix_on) {
            const auto* sta_fix = net::get(*fix, StationId{static_cast<int>(station_index)},
                                           &net::StationState::sd_ewl_fix);
            const auto git = sta_fix ? sta_fix->find(cons) : net::SdEwlFixes::const_iterator{};
            if (sta_fix && git != sta_fix->end()) {
                const auto sit = git->second.find(o.satellite);
                if (sit != git->second.end() && sit->second.usable)
                    my_fix = &sit->second;
            }
        }
        for (const auto& [s1, k1, i1] : c1it->second) {
            for (const auto& [s2, k2, i2] : c2it->second) {
                ++n_cand;
                if (!sig1.empty()) continue;            // already decided; just count
                const auto q = st.mw_slip->arcZdMean(o.satellite, s1, s2);
                if (!q.exists) continue;                // this pair was never formed
                any_exists = true;
                // arcZdMean reports !valid for two different reasons: fewer than two
                // lagged samples, and an SD arc whose group MW pivot arc is missing.
                // Only the latter is a frame-reconstruction failure; conflating them
                // would report a warm-up epoch as a datum defect.
                if (!q.valid && q.count >= 2) any_recon_fail = true;
                if (q.count > best_n) { best_n = q.count; near_s1 = s1; near_s2 = s2; }
                if (q.valid && q.count >= static_cast<std::size_t>(cfg.min_arc_epochs)) {
                    zd = q; sig1 = s1; sig2 = s2;
                    col1 = k1; col2 = k2; sidx1 = i1; sidx2 = i2;
                }
            }
        }
        if (sig1.empty()) {
            // Ordered from most to least specific: a pair that exists but cannot be
            // framed is a different defect from one that is merely young, and both
            // differ from a pair the detector never built.
            const char* why = !any_exists      ? "NO_ARC"
                            : any_recon_fail   ? "FRAME_RECON"
                                               : "IMMATURE";
            if (!any_exists)          ++ctr.skip_no_arc;
            else if (any_recon_fail)  ++ctr.skip_frame_recon;
            else                      ++ctr.skip_immature;
            tr(why, o.satellite, {}, near_s1, near_s2, {}, {}, n_cand, best_n,
               false, 0.0, 0.0, 0.0, 0.0, 0.0);
            continue;
        }
        const double f1 = fb1, f2 = fb2;
        // λ cross-check: the detector's own λ_WL comes from the actual signal
        // frequencies, so a disagreement with the tier's nominal c/(f1−f2) means the
        // elected pair is not the tier this row's coefficients assume (a mis-tiered
        // row would fold a wrong-wavelength constraint without any error).
        const double lam_nom = Constants::SPEED_OF_LIGHT / (f1 - f2);
        if (!(f1 > 0.0) || !(f2 > 0.0) || std::abs(f1 - f2) < 1.0
            || !(zd.lambda_wl > 0.0) || !std::isfinite(zd.lambda_wl)
            || std::abs(zd.lambda_wl - lam_nom) > 1e-6 * std::abs(lam_nom)) {
            ++ctr.skip_bad_lambda;
            tr("BAD_LAMBDA", o.satellite, {}, sig1, sig2, zd.code1, zd.code2, n_cand,
               zd.count, zd.bia_corrected, zd.lambda_wl, zd.mean_m, 0.0, 0.0, 0.0);
            continue;
        }
        // The satellite code-bias columns must be the ones the MW observable actually
        // carries.  The arc reports its own code channels (matchChannel's same-attribute
        // pick, with an in-band fallback), so nothing is guessed: without them the OSB
        // attribution would be an assumption, and a wrong OSB column is undetectable
        // downstream.
        if (zd.code1.empty() || zd.code2.empty()) {
            ++ctr.skip_missing_code;
            tr("NO_CODE", o.satellite, {}, sig1, sig2, zd.code1, zd.code2, n_cand,
               zd.count, zd.bia_corrected, zd.lambda_wl, zd.mean_m, 0.0, 0.0, 0.0);
            continue;
        }
        SdEwlArc a;
        a.sat = o.satellite;
        a.mean_m = zd.mean_m;
        // Attach the published integer only if the search settled on the pair the
        // integer was decided on (increment 2b).  A mismatch is not an error (the
        // decision runs before assembly and does not know which pairs will have local
        // columns this epoch), but folding across it would put one bias frame's integer
        // into another frame's row, so it is counted and the arc stays float.
        if (my_fix) {
            if (sig1 == my_fix->sig1 && sig2 == my_fix->sig2) {
                a.has_fix = true;
                a.n_fix_cyc = my_fix->n_cyc;
                a.fix_measured = my_fix->measured;
            } else {
                ++ctr.fix_pair_mismatch;
            }
        }
        // arcZdMean returns a variance of the mean (~1/n); it is floored so a long quiet
        // arc cannot dominate the shared border with an unphysically tight row.  A fixed
        // row takes the per-sample floor instead (amendment A1): the accumulator is
        // arc-cumulative, so consecutive-epoch rows share n−1 of n samples, and refolding
        // a variance of the mean as white every epoch over-claims information as sqrt(k),
        // increasingly so as the fixed-row residuals fall to code-noise scale.  Folding
        // the current mean at per-sample weight adds about one sample of information per
        // epoch, which asymptotically matches the information the data carry.
        a.var_m2 = a.has_fix
                       ? std::max(zd.var_of_mean_m2,
                                  cfg.fix_sigma_floor_m * cfg.fix_sigma_floor_m)
                       : std::max(zd.var_of_mean_m2,
                                  cfg.sigma_floor_m * cfg.sigma_floor_m);
        a.count = zd.count;
        a.bia = zd.bia_corrected;
        a.lam_ewl = zd.lambda_wl;
        a.lam_b1 = Constants::SPEED_OF_LIGHT / f1;
        a.lam_b2 = Constants::SPEED_OF_LIGHT / f2;
        a.f1 = f1;
        a.f2 = f2;
        a.amb_col_b1 = col1;
        a.amb_col_b2 = col2;
        // Non-throwing shared lookups: phaseBiasColumn / codeBiasColumn use `index_.at()`
        // and throw when the key is unregistered.  A bare call here would escape into the
        // parallel produce loop's catch and drop the whole station's block (including its
        // valid phase rows) because of one absent bias column.
        const auto pbCol = [&est, &o](const std::string& sig) -> Eigen::Index {
            try { return est.phaseBiasColumn(o.satellite, sig); }
            catch (...) { return -1; }
        };
        const auto cbCol = [&est, &o](const std::string& code) -> Eigen::Index {
            try { return est.codeBiasColumn(o.satellite, code); }
            catch (...) { return -1; }
        };
        a.pb_b1 = pbCol(sig1);
        a.pb_b2 = pbCol(sig2);
        a.cb_b1 = cbCol(zd.code1);
        a.cb_b2 = cbCol(zd.code2);
        if (a.pb_b1 < 0 || a.pb_b2 < 0 || a.cb_b1 < 0 || a.cb_b2 < 0) {
            ++ctr.skip_missing_shared_col;
            tr("NO_SHARED_COL", o.satellite, {}, sig1, sig2, zd.code1, zd.code2,
               n_cand, zd.count, a.bia, a.lam_ewl, a.mean_m, 0.0, 0.0, 0.0);
            continue;
        }
        // This arc's half of H·x_lin, with exactly the coefficients the row folds in
        // emitGroup below (the two must stay in step, or design and frame drift apart).
        // Split into its ambiguity and bias halves so the fixed row, whose design drops
        // the ambiguity columns, can drop the matching linearization terms at one site.
        a.pred_amb_m = (a.lam_ewl / a.lam_b1) * xAt(sidx1)
                     - (a.lam_ewl / a.lam_b2) * xAt(sidx2);
        a.pred_bias_m = (f1 / (f1 - f2)) * xAt(stateOf(pb_state, o.satellite, sig1))
                      - (f2 / (f1 - f2)) * xAt(stateOf(pb_state, o.satellite, sig2))
                      - (f1 / (f1 + f2)) * xAt(stateOf(cb_state, o.satellite, zd.code1))
                      - (f2 / (f1 + f2)) * xAt(stateOf(cb_state, o.satellite, zd.code2));
        a.pred_m = a.pred_amb_m + a.pred_bias_m;
        a.sig1 = sig1;
        a.sig2 = sig2;
        a.code1 = zd.code1;
        a.code2 = zd.code2;
        a.n_cand = n_cand;
        by_const[cons].push_back(std::move(a));
    }
}

void sdEwlEmitGroup(const SdEwlRowView& v, const std::vector<const SdEwlArc*>& g,
                    bool fixed_group) {
    const auto& cfg = v.cfg;
    auto& ctr = v.ctr;
    const auto& tr2 = v.tr2;

// --- Build the rows ---------------------------------------------------------------
// Increment 1 forms one whitened group per constellation.  Increment 2b forms two,
// the arcs whose integer is fixed and the rest, because a group cannot be mixed: R
// is whitened by a dense L^-1, which would re-populate a fixed row's zeroed local
// ambiguity columns from its float siblings, and the pivot arc's ambiguity columns
// appear in every row of a group, so one mixed group would both remove and
// estimate the pivot ambiguity.  Splitting keeps both halves exact and makes "a
// fixed row carries no local coefficients" true of the whitened block, not only of
// H.  With the integer path off, one group holds every arc, so the float behaviour
// is unchanged.
    if (g.size() < 2) {
        ++ctr.skip_no_pivot;
        for (const SdEwlArc* a : g)
            tr2("NO_PIVOT", a->sat, {}, a->sig1, a->sig2, a->code1, a->code2,
                a->n_cand, a->count, a->bia, a->lam_ewl, a->mean_m, a->pred_m,
                0.0, 0.0, a->n_fix_cyc, false, fixed_group);
        return;
    }
    const SdEwlArc& p = *g.front();
    tr2("PIVOT", p.sat, p.sat, p.sig1, p.sig2, p.code1, p.code2, p.n_cand, p.count,
        p.bia, p.lam_ewl, p.mean_m, p.pred_m, 0.0, std::sqrt(p.var_m2),
        p.n_fix_cyc, false, fixed_group);

    std::vector<SdEwlRow> rows;
    for (std::size_t k = 1; k < g.size(); ++k) {
        const SdEwlArc& s = *g[k];
        // Satellite-bias cancellation needs one frame across the difference
        // (counted as skip_mixed_frame).
        if (s.bia != p.bia) {
            ++ctr.skip_mixed_frame;
            tr2("MIXED_FRAME", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2,
                s.n_cand, s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, 0.0, 0.0,
                s.n_fix_cyc, false, fixed_group);
            continue;
        }
        // Correction frame: y = z_SD − H·x_lin, with H·x_lin = pred(s) − pred(p)
        // (the row's H is exactly the s-terms minus the p-terms).  The raw SD
        // difference is hundreds of metres; only this residual is consistent with
        // the station's block.
        //
        // Fixed row (increment 2b): the integer term is subtracted from the RHS and
        // the ambiguity columns leave H, so H·x_lin must lose exactly its ambiguity
        // half; a row whose design changed while its linearization point did not is
        // folded in a mixed frame.  Both edits are in this one expression for that
        // reason.
        const double z =
            fixed_group
                ? (s.mean_m - p.mean_m)
                      - p.lam_ewl * static_cast<double>(s.n_fix_cyc - p.n_fix_cyc)
                      - (s.pred_bias_m - p.pred_bias_m)
                : (s.mean_m - p.mean_m) - (s.pred_m - p.pred_m);
        // The limit gates the correction-frame residual, which is the quantity a
        // cold-clock x_lin error inflates (a 256 km x_lin makes pred absurd while
        // the raw statistic looks ordinary).
        if (cfg.max_abs_z_m > 0.0 && std::abs(z) > cfg.max_abs_z_m) {
            ++ctr.skip_big_z;
            tr2("BIG_Z", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2, s.n_cand,
                s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, z, 0.0,
                s.n_fix_cyc, false, fixed_group);
            continue;
        }
        if (!std::isfinite(z)) {   // a non-finite RHS must not reach u_r
            ++ctr.skip_bad_lambda;
            tr2("BAD_LAMBDA", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2,
                s.n_cand, s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, z, 0.0,
                s.n_fix_cyc, false, fixed_group);
            continue;
        }
        rows.push_back(SdEwlRow{ &s, z, s.var_m2 });
    }
    if (rows.empty()) return;
    if (cfg.max_rows_per_station > 0
        && rows.size() > static_cast<std::size_t>(cfg.max_rows_per_station)) {
        std::stable_sort(rows.begin(), rows.end(),
                         [](const SdEwlRow& a, const SdEwlRow& b) { return a.var < b.var; });
        const std::size_t keep = static_cast<std::size_t>(cfg.max_rows_per_station);
        ctr.rows_capped += static_cast<long long>(rows.size() - keep);
        for (std::size_t k = keep; k < rows.size(); ++k) {
            const SdEwlArc& s = *rows[k].s;
            tr2("CAPPED", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2, s.n_cand,
                s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m, rows[k].z,
                std::sqrt(rows[k].var), s.n_fix_cyc, false, fixed_group);
        }
        rows.resize(keep);
    }

    sdEwlAppendRows(v, p, rows, fixed_group);
}

void sdEwlAppendRows(const SdEwlRowView& v, const SdEwlArc& p, const std::vector<SdEwlRow>& rows,
                     bool fixed_group) {
    Eigen::MatrixXd& A_s = v.A_s;
    Eigen::MatrixXd& A_r = v.A_r;
    Eigen::VectorXd& w = v.w;
    Eigen::VectorXd& y = v.y;
    std::vector<RowTag>& tags = v.tags;
    auto& ctr = v.ctr;
    const auto& tr2 = v.tr2;

    const int mc = static_cast<int>(rows.size());
    const Eigen::Index m0 = A_s.rows();
    const Eigen::Index ncol_s = A_s.cols();
    const Eigen::Index ncol_r = A_r.cols();
    A_s.conservativeResize(m0 + mc, Eigen::NoChange);
    A_s.bottomRows(mc).setZero();
    A_r.conservativeResize(m0 + mc, Eigen::NoChange);
    A_r.bottomRows(mc).setZero();
    w.conservativeResize(m0 + mc);
    y.conservativeResize(m0 + mc);

    // Every row shares the pivot arc's noise => R = diag(var_s) + var_p * 11^T,
    // structurally the GIM common-mode R.
    Eigen::MatrixXd R = Eigen::MatrixXd::Constant(mc, mc, p.var_m2);
    Eigen::MatrixXd Hr = Eigen::MatrixXd::Zero(mc, ncol_r);
    Eigen::MatrixXd Hs = Eigen::MatrixXd::Zero(mc, ncol_s);
    Eigen::VectorXd zc(mc);
    for (int k = 0; k < mc; ++k) {
        const SdEwlArc& s = *rows[static_cast<std::size_t>(k)].s;
        R(k, k) += rows[static_cast<std::size_t>(k)].var;
        zc(k) = rows[static_cast<std::size_t>(k)].z;
        ctr.sum_var_m2 += R(k, k);
        // Local: lambda_EWL * (amb_b1/lambda_b1 - amb_b2/lambda_b2), s minus p.
        // A fixed row has none of these: the local degree of freedom is removed
        // (rank reduction), not constrained with a tight weight.  A tight prior on
        // a gauge-adjacent local column is a stiff prior, and increment 2 exists to
        // stop the local nuisance from absorbing the row's information so that the
        // shared satellite bias receives it.
        if (!fixed_group) {
            Hr(k, s.amb_col_b1) += s.lam_ewl / s.lam_b1;
            Hr(k, s.amb_col_b2) -= s.lam_ewl / s.lam_b2;
            Hr(k, p.amb_col_b1) -= p.lam_ewl / p.lam_b1;
            Hr(k, p.amb_col_b2) += p.lam_ewl / p.lam_b2;
        }
        // Shared: MW = phi_WL - P_NL, and the satellite code bias enters the
        // pseudorange with +, hence the negative CB coefficients.
        const double wl_s1 = s.f1 / (s.f1 - s.f2), wl_s2 = -s.f2 / (s.f1 - s.f2);
        const double nl_s1 = -s.f1 / (s.f1 + s.f2), nl_s2 = -s.f2 / (s.f1 + s.f2);
        const double wl_p1 = p.f1 / (p.f1 - p.f2), wl_p2 = -p.f2 / (p.f1 - p.f2);
        const double nl_p1 = -p.f1 / (p.f1 + p.f2), nl_p2 = -p.f2 / (p.f1 + p.f2);
        Hs(k, s.pb_b1) += wl_s1;  Hs(k, s.pb_b2) += wl_s2;
        Hs(k, s.cb_b1) += nl_s1;  Hs(k, s.cb_b2) += nl_s2;
        Hs(k, p.pb_b1) -= wl_p1;  Hs(k, p.pb_b2) -= wl_p2;
        Hs(k, p.cb_b1) -= nl_p1;  Hs(k, p.cb_b2) -= nl_p2;
    }
    // Whiten by L^-1 (LL^T = R) into unit-information rows, so the block contributes
    // exactly H^T R^-1 H / H^T R^-1 z and stays in the diagonal-weight path.
    const Eigen::LLT<Eigen::MatrixXd> llt(R);
    if (llt.info() != Eigen::Success) {
        // Unwind the append.  conservativeResize leaves the new w/y entries
        // uninitialized; left in place, even against all-zero design rows, they
        // risk 0·NaN = NaN in N_rr/u_r, so the rows are removed, not just zeroed.
        // (A positive-variance R cannot fail the factorization; this is a guard.)
        A_s.conservativeResize(m0, Eigen::NoChange);
        A_r.conservativeResize(m0, Eigen::NoChange);
        w.conservativeResize(m0);
        y.conservativeResize(m0);
        ctr.skip_bad_lambda += mc;
        return;
    }
    A_r.middleRows(m0, mc) = llt.matrixL().solve(Hr);
    A_s.middleRows(m0, mc) = llt.matrixL().solve(Hs);
    y.segment(m0, mc) = llt.matrixL().solve(zc);
    w.segment(m0, mc).setOnes();
    // One tag per row, in row order: the row's satellite (the arc on
    // the diagonal of L^-1; the pivot arc is common to every row of the group).
    for (int k = 0; k < mc; ++k)
        tags.push_back(makeRowTag(ContributionKind::SdEwl,
                                  rows[static_cast<std::size_t>(k)].s->sat));
    ctr.rows_built += mc;
    if (fixed_group) { ctr.rows_fixed += mc; ++ctr.groups_fixed; }
    else             { ctr.rows_float += mc; }
    for (int k = 0; k < mc; ++k) {
        const SdEwlArc& s = *rows[static_cast<std::size_t>(k)].s;
        tr2("BUILT", s.sat, p.sat, s.sig1, s.sig2, s.code1, s.code2, s.n_cand,
            s.count, s.bia, s.lam_ewl, s.mean_m, s.pred_m,
            rows[static_cast<std::size_t>(k)].z, std::sqrt(R(k, k)),
            s.n_fix_cyc - p.n_fix_cyc, fixed_group, fixed_group);
    }
}

void sdEwlBuildGroups(const SdEwlRowView& v, std::map<char, std::vector<SdEwlArc>>& by_const) {
    auto& ctr = v.ctr;
    const bool fix_on = v.fix_on;

    const auto emitGroup = [&](const std::vector<const SdEwlArc*>& g, bool fixed_group) {
        sdEwlEmitGroup(v, g, fixed_group);
    };

    for (auto& [cons, arcs] : by_const) {
        // Pivot election: most mature arc, ties broken by ascending satellite name.  It
        // must be deterministic, or the row set (and with it byte-identity) changes from
        // run to run.  Sorted once over the whole constellation, so each partition
        // inherits the same ordering and elects its own most mature member as pivot.
        std::sort(arcs.begin(), arcs.end(), [](const SdEwlArc& a, const SdEwlArc& b) {
            if (a.count != b.count) return a.count > b.count;
            return a.sat < b.sat;
        });
        if (!fix_on) {
            std::vector<const SdEwlArc*> all;
            all.reserve(arcs.size());
            for (const SdEwlArc& a : arcs) all.push_back(&a);
            emitGroup(all, false);
            continue;
        }
        std::vector<const SdEwlArc*> fixed_g, float_g;
        for (const SdEwlArc& a : arcs) (a.has_fix ? fixed_g : float_g).push_back(&a);
        // A fixed group needs at least one measured integer.  Datum-valued integers (the
        // component reference station's own satellites, each station's link satellite, a
        // satellite framed on first sighting) carry no rounding decision, so no gate can
        // reject them.  A group built only from those would assert a hard constraint with
        // no integer information while still removing a real local degree of freedom, and
        // the feature could not be disarmed through its own gates (an `inert` A/B arm,
        // fix armed with the FFR gate unreachable, would still differ from the float
        // arm).  A group mixing datum-valued and measured integers is correct: the datum
        // value is the reference the measurement is relative to.
        if (!fixed_g.empty()
            && std::none_of(fixed_g.begin(), fixed_g.end(),
                            [](const SdEwlArc* a) { return a->fix_measured; })) {
            ctr.fix_datum_only += static_cast<long long>(fixed_g.size());
            float_g.insert(float_g.end(), fixed_g.begin(), fixed_g.end());
            std::stable_sort(float_g.begin(), float_g.end(),
                             [](const SdEwlArc* a, const SdEwlArc* b) {
                                 if (a->count != b->count) return a->count > b->count;
                                 return a->sat < b->sat;
                             });
            fixed_g.clear();
        }
        // A single fixed arc cannot form a group of its own (there is nothing to
        // difference against).  It is counted as an unusable fix (fix_lone_arc) and
        // returned to the float group, re-sorted by the constellation order above, so
        // the float pivot is the one the float-only path elects over the same arcs and
        // the partition stays a pure function of the published integers.
        if (fixed_g.size() == 1) {
            ++ctr.fix_lone_arc;
            float_g.insert(float_g.begin(), fixed_g.front());
            std::sort(float_g.begin(), float_g.end(),
                      [](const SdEwlArc* a, const SdEwlArc* b) {
                          if (a->count != b->count) return a->count > b->count;
                          return a->sat < b->sat;
                      });
            fixed_g.clear();
        }
        emitGroup(fixed_g, true);
        emitGroup(float_g, false);
    }
}

} // namespace GnssProcessor

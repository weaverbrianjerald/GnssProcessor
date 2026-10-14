/**
 * @file NetworkCoupling.cpp
 * @brief Conditional coupling-row builder, cards 1, 1b, 1d (see the header).
 *
 * At decomposition step INC-7b the coupling code was moved unchanged from
 * Application::networkPassB_ (ApplicationNetworkAssembly.cpp): the six former [&]
 * lambdas became CouplingRowBuilder member functions whose captures are const
 * reference or value members, and the serial counter fold and the card 1d debug
 * dump became foldAndDumpCouplingRows.  The capture-to-member map is in the INC-7b
 * cut spec.
 */

#include "filter/NetworkCoupling.hpp"

#include "filter/NetworkEstimator.hpp"   // est.clockColumn / phaseBiasColumn / codeBiasColumn / vtecCoeffColumn
#include "filter/NetworkSharedHistory.hpp"   // sharedHistoryRowTerms (card SCL2 X2)
#include "config/GnssConfigTypes.hpp"    // GlobalVtecShConfig::q_per_degree (couplingQ)
#include "filter/StateKeySchema.hpp"     // StateKeys::parseSatPhaseBiasKey (card 1d dump)
#include "common/GnssIdentity.hpp"       // letterOfSat: a term's constellation letter

#include <spdlog/spdlog.h>               // debug dump of card 1d (log->info)
#include <fmt/format.h>                  // gauge-sum formatting in the card 1d dump

#include <cmath>
#include <limits>            // NaN sentinel for a missing harvest pair (F3 buildLocalBlock)
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunEnd.hpp"

namespace GnssProcessor {

Eigen::Index CouplingRowBuilder::resolveSharedCol(const CoupledShared& cs) const {
    try {
        switch (cs.kind) {
            case SharedParamKind::Clock:     return est.clockColumn(cs.sat);
            case SharedParamKind::PhaseBias: return est.phaseBiasColumn(cs.sat, cs.signal);
            case SharedParamKind::CodeBias:  return est.codeBiasColumn(cs.sat, cs.signal);
            case SharedParamKind::VtecSh:
                return est.vtecCoeffColumn(cs.n, cs.m, cs.trig, cs.sh_layer);
        }
    } catch (...) {}
    return -1;  // departed (unregistered this epoch)
}

std::string CouplingRowBuilder::couplingId(const CoupledShared& cs) {
    switch (cs.kind) {
        case SharedParamKind::Clock:     return "C|" + cs.sat;
        case SharedParamKind::PhaseBias: return "P|" + cs.sat + "|" + cs.signal;
        case SharedParamKind::CodeBias:  return "B|" + cs.sat + "|" + cs.signal;
        case SharedParamKind::VtecSh:    return (cs.sh_layer > 0 ? "V2|" : "V|")
                                                + std::to_string(cs.n) + "|"
                                                + std::to_string(cs.m) + "|" + cs.trig;
    }
    return {};
}

double CouplingRowBuilder::couplingQ(const CoupledShared& cs) const {
    switch (cs.kind) {
        case SharedParamKind::Clock:     return q_clock;
        case SharedParamKind::CodeBias:  return q_cb;
        case SharedParamKind::PhaseBias: return q_pb;
        case SharedParamKind::VtecSh: {
            // Per-layer q ladder.  It must stay consistent with the injector's
            // random-walk-toward-previous q (networkInjectShVtecPrior), which is the
            // same model expressed driver-side.
            const double q0 = (cs.sh_layer > 0) ? shc.layer2_q_per_degree
                                                : shc.q_per_degree;
            return (cs.n == 0) ? q0 : q0 / static_cast<double>(cs.n * cs.n);
        }
    }
    return 0.0;
}

void CouplingRowBuilder::tuGaugeRestore(
        std::vector<TuEnt>& ents,
        const std::map<std::pair<char, int>, double>* trunc_sum) const {
    std::map<std::pair<char, int>, std::pair<double, int>> deficit;
    for (const TuEnt& e : ents)
        if (e.cc && e.kind != SharedParamKind::VtecSh) {
            auto& d = deficit[{e.cc, static_cast<int>(e.kind)}];
            d.first += e.m_un - e.m_shr;
            ++d.second;
        }
    // A truncated column's coefficient was deleted (card 1f), so its family's sum is
    // short by that coefficient.  This is the same kind of deficit the κ shrink opens
    // and is corrected the same way: seeding the deficit map with the truncated sums,
    // in addition to the present entries' shrink, restores Σ_family M to its harvest
    // value instead of to the surviving-present value.  A family with truncated mass
    // and no present recipient cannot be corrected this epoch; the row stays
    // gauge-variant for that family, and the miss is counted (cpl_gauge_orphan,
    // cpl_orphan_fam) rather than partially fixed (invariant #4).
    if (trunc_sum)
        for (const auto& [fam, s] : *trunc_sum) {
            if (s == 0.0) continue;
            const auto it = deficit.find(fam);
            if (it == deficit.end() || it->second.second == 0) {
                ++R.cpl_gauge_orphan;
                auto& o = R.cpl_orphan_fam[fam];
                ++o.first;
                o.second += std::abs(s);
                continue;
            }
            it->second.first += s;
            ++R.cpl_gauge_held;
        }
    for (TuEnt& e : ents)
        if (e.cc && e.kind != SharedParamKind::VtecSh) {
            const auto it = deficit.find({e.cc, static_cast<int>(e.kind)});
            if (it != deficit.end() && it->second.second > 0)
                e.m_shr += it->second.first
                         / static_cast<double>(it->second.second);
        }
}

bool CouplingRowBuilder::tuReinitSince(const CoupledShared& cs, double since_s) const {
    // The κ time update is not the only gate (card 1g).  A column re-realized since
    // this row's harvest carries an unknown frame step in its stashed ŝ_prev, so
    // folding it as a full-weight present term injects that step as row bias,
    // whether or not the border time update is armed.
    if (!carry_cpl_tu && !carry_cpl_reinit) return false;
    const auto hit = [&](const std::string& sig) {
        const auto it = tu_reinit_hist.find({cs.kind, cs.sat, sig});
        return it != tu_reinit_hist.end() && it->second > since_s;
    };
    // On-path telemetry for card 1g (invariant #4).  The run's departed-term total
    // moves for many reasons and cannot show whether the per-signal widening acts;
    // this counter counts only the terms this rule diverts, i.e. where the exact
    // (kind, sat, signal) probe fires and the sat-wide probe would not.
    if (carry_cpl_reinit && !cs.signal.empty() && hit(cs.signal) && !hit(std::string{}))
        ++R.cpl_reinit_diverted;
    // The clock-QC feeds mark a whole satellite (empty signal = sat-wide wildcard);
    // the realization-step feeds mark one (sat, signal) family.  Probing the exact
    // key first and the wildcard second reproduces the sat-wide matching when only
    // the sat-wide feeds are populated.
    if (!carry_cpl_reinit) return hit(std::string{});
    return hit(cs.signal) || (!cs.signal.empty() && hit(std::string{}));
}

bool CouplingRowBuilder::readmitDiverted(const CoupledShared& cs) {
    // Re-admission re-coupling edge: a satellite-side column inside its
    // post-re-admission window takes the departed (q = ∞) route regardless of the
    // row's harvest time.  The column exists and is bound, but its realization is
    // still settling from the cold seed, so conditioning a stiff carried row on it
    // injects that settling as row bias (and F3's dense L⁻¹ then mixes it into every
    // ambiguity of the station block).
    if (!readmit_marg || readmit_marg->empty() || cs.kind == SharedParamKind::VtecSh
        || cs.sat.empty())
        return false;
    const auto it = readmit_marg->find(cs.sat);
    if (it == readmit_marg->end() || loop_ep >= it->second) return false;
    ++R.cpl_readmit_diverted;
    return true;
}

ContributionKind localBlockColKind(const LocalBlockCol& c) {
    if (c.is_zwd)    return ContributionKind::BlockZwdCarry;
    if (c.is_iono)   return ContributionKind::BlockIonoCarry;
    if (c.is_rx_clk) return ContributionKind::BlockRxClockCarry;
    if (c.is_rx_cb)  return ContributionKind::BlockRxCbCarry;
    if (c.is_pc)     return ContributionKind::BlockPhaseColourCarry;
    return ContributionKind::BlockAmbiguityCarry;
}

void CouplingRowBuilder::buildCplRow(Eigen::Index col, double x_lin_val,
                                     const LocalCarry& lc, double var_eff,
                                     double gap, const SatId& sat, const SignalId& signal) {
    if (carry_cpl_joint) {
        // Joint path (card 1b): the same present/departed split as the scalar
        // path, but the departed columns become shared M_w columns (marginalized
        // jointly at append) instead of a per-row scalar σ² addend.
        CplRowRaw raw;
        raw.local_col = col;
        raw.var_eff   = var_eff;
        raw.tag       = RowTag{ContributionKind::CouplingJoint, sat, signal};
        double rhs = lc.value_abs - x_lin_val;   // (r̂_prev − r_lin)
        std::vector<TuEnt> ents;                 // buffered present terms (card 1d)
        // With the gauge projection armed (card 1f), every present term is emitted
        // through the buffer, so the restore also runs with the κ time update off.
        // The iteration order is unchanged and departed terms never touch `rhs`, so
        // the off path is bit-identical.
        const bool buffer_all = carry_cpl_tu || carry_cpl_gauge;
        std::map<std::pair<char, int>, double> trunc;  // (cc,kind) → Σ truncated coeff (card 1f)
        for (const CoupledShared& cs : lc.coupling) {
            Eigen::Index c = resolveSharedCol(cs);
            // A column reinit-folded since this row's harvest (card 1d) is
            // decorrelated from its stashed value.  The κ = 0 (q = ∞) limit of the
            // coupling term is the departed treatment, so the term goes to the
            // joint M_w marginalization.  With carry_cpl_shmarg (card 1d-SH), VtecSh
            // terms take the same route (the GM border does not follow the κ update's
            // RW model).
            if (c >= 0 && (tuReinitSince(cs, lc.last_seen_s)
                           || (carry_cpl_shmarg
                               && cs.kind == SharedParamKind::VtecSh)))
                c = -1;
            // Re-admission re-coupling edge (network.readmit_coupling_marg_ep): a
            // column re-registered after a gap is still settling for K epochs; same route.
            if (c >= 0 && readmitDiverted(cs)) c = -1;
            if (c < 0) {                          // departed → M_w column
                const std::string id = couplingId(cs);
                auto it = white_map.find(id);
                int widx;
                if (it == white_map.end()) {
                    widx = static_cast<int>(white_pw.size());
                    white_map.emplace(id, widx);
                    white_pw.push_back(cs.p_prev);
                } else {
                    widx = it->second;
                    // Rows harvested at different epochs may carry slightly
                    // different P_prev for the same departed column; the larger
                    // is kept so that the shared marginalization is never
                    // optimistic.
                    if (cs.p_prev > white_pw[static_cast<std::size_t>(widx)])
                        white_pw[static_cast<std::size_t>(widx)] = cs.p_prev;
                }
                raw.departed.emplace_back(widx, cs.coeff);
                ++R.cpl_terms_departed;
                if (carry_cpl_gauge && !cs.sat.empty()
                    && cs.kind != SharedParamKind::VtecSh)
                    trunc[{letterOfSat(cs.sat), static_cast<int>(cs.kind)}] += cs.coeff;
                continue;
            }
            ++R.cpl_terms_present;
            // Time update (card 1d): shrink the present column by κ_j and inflate
            // this row's own diagonal by M²·κ_j·q_j·Δt, as on the scalar path below;
            // only the departed-column treatment (M_w) differs.
            if (carry_cpl_tu) {
                const double qj = couplingQ(cs) * gap;
                double m = cs.coeff;
                if (qj > 0.0 && cs.p_prev > 0.0) {
                    const double kap = cs.p_prev / (cs.p_prev + qj);
                    raw.var_eff += cs.coeff * cs.coeff * kap * qj;
                    m = kap * cs.coeff;
                    ++R.cpl_terms_shrunk;
                }
                ents.push_back(TuEnt{c, m, cs.coeff, cs.s_prev,
                                     cs.sat.empty() ? '\0' : letterOfSat(cs.sat),
                                     cs.kind});
                continue;
            }
            if (buffer_all) {   // unshrunk, but routed through the buffer (card 1f)
                ents.push_back(TuEnt{c, cs.coeff, cs.coeff, cs.s_prev,
                                     cs.sat.empty() ? '\0' : letterOfSat(cs.sat),
                                     cs.kind});
                continue;
            }
            raw.shared.emplace_back(c, cs.coeff);
            rhs += cs.coeff * cs.s_prev;          // + M_rj·ŝ_prev,j
        }
        if (buffer_all) {
            tuGaugeRestore(ents, carry_cpl_gauge ? &trunc : nullptr);
            for (const TuEnt& e : ents) {
                raw.shared.emplace_back(e.c, e.m_shr);
                rhs += e.m_shr * e.s_prev;
            }
        }
        if (!(raw.var_eff > 0.0) || !std::isfinite(rhs)) return;  // skip: σ² ≤ 0 or non-finite RHS
        raw.rhs = rhs;
        craw.push_back(std::move(raw));
        return;
    }
    // Scalar path (card 1).  The loop body lives in scalarRowIngredients so that the
    // F3 block path shares it; the operations and their order are those of the
    // former inline sequence, so the scalar path is byte-identical to it.
    PendCplRow row;
    double sigma2 = 0.0;
    if (!scalarRowIngredients(col, x_lin_val, lc, var_eff, gap, row, sigma2)) return;
    row.w = 1.0 / sigma2;
    row.tag = RowTag{ContributionKind::CouplingScalar, sat, signal};
    crows.push_back(std::move(row));
}

bool CouplingRowBuilder::scalarRowIngredients(Eigen::Index col, double x_lin_val,
                                              const LocalCarry& lc, double var_eff,
                                              double gap, PendCplRow& row, double& sigma2,
                                              CouplingExactRow* xr_keep) {
    // Card CTU: a row harvested at the stash the dense block belongs to takes the exact
    // route; any other row (harvested earlier, so its regression is on an older border
    // than P describes) keeps the card 1d route below, counted.  Off: cxu is null and
    // the body below is unchanged.
    if (cxu) {
        if (!cxu->ok) {
            ++R.cxu.rows_unfactored;
        } else if (std::abs(lc.last_seen_s - cxu->stash_s) <= 1e-6) {
            return exactRowIngredients(col, x_lin_val, lc, var_eff, gap, row, sigma2, xr_keep);
        } else {
            ++R.cxu.rows_stale;
        }
    }
    row.local_col = col;
    double rhs = lc.value_abs - x_lin_val;   // (r̂_prev − r_lin)
    double dep_var = 0.0;                    // Σ_departed M_rj²·P_prev,jj
    double tu_var  = 0.0;                    // Σ_present M_rj²·κ_j·q_j·Δt (card 1d)
    std::vector<TuEnt> ents;                 // buffered present terms (card 1d)
    const bool buffer_all = carry_cpl_tu || carry_cpl_gauge;   // see the joint path (card 1f)
    std::map<std::pair<char, int>, double> trunc;              // (cc,kind) → Σ truncated coeff (card 1f)
    // All three truncation branches below feed this map (card 1f).  A column dropped
    // because it left the layout and a column forced onto the departed route (by a
    // reinit or frame step, or by the re-admission window) perturb the coefficient
    // vector in the same way.
    const auto note_trunc = [&](const CoupledShared& cs) {
        if (carry_cpl_gauge && !cs.sat.empty() && cs.kind != SharedParamKind::VtecSh)
            trunc[{letterOfSat(cs.sat), static_cast<int>(cs.kind)}] += cs.coeff;
    };
    for (const CoupledShared& cs : lc.coupling) {
        const Eigen::Index c = resolveSharedCol(cs);
        if (c < 0) {                          // departed → marginalize into σ²
            dep_var += cs.coeff * cs.coeff * cs.p_prev;
            ++R.cpl_terms_departed;
            note_trunc(cs);
            continue;
        }
        // Re-admission re-coupling edge (network.readmit_coupling_marg_ep): the
        // returned satellite's columns take the departed route for K epochs (see the
        // joint path).
        if (readmitDiverted(cs)) {
            dep_var += cs.coeff * cs.coeff * cs.p_prev;
            ++R.cpl_terms_departed;
            note_trunc(cs);
            continue;
        }
        // A column reinit-folded since this row's harvest (card 1d) is the q = ∞
        // limit of the time update and is fully marginalized (κ = 0), mirroring the
        // dense fold's decorrelation rule.  With carry_cpl_shmarg (card 1d-SH),
        // VtecSh terms take the same treatment (the GM border does not follow the RW
        // κ model).
        if (tuReinitSince(cs, lc.last_seen_s)
            || (carry_cpl_shmarg && cs.kind == SharedParamKind::VtecSh)) {
            dep_var += cs.coeff * cs.coeff * cs.p_prev;
            ++R.cpl_terms_departed;
            note_trunc(cs);
            continue;
        }
        ++R.cpl_terms_present;
        if (carry_cpl_tu) {
            // Time update (card 1d): the row observed u = r + M_rᵀs at harvest; the border has
            // since moved to s⁺ = s + w, w ~ N(0,Q).  Exact re-independence against
            // s⁺: shrink M' = κ·M_r (the coefficient and its ŝ_prev RHS term) and
            // inflate σ² by M²·κ·q·Δt = Mᵀ(P⁻¹+Q⁻¹)⁻¹M per column.
            const double qj = couplingQ(cs) * gap;
            double m = cs.coeff;
            if (qj > 0.0 && cs.p_prev > 0.0) {
                const double kap = cs.p_prev / (cs.p_prev + qj);
                tu_var += cs.coeff * cs.coeff * kap * qj;
                m = kap * cs.coeff;
                ++R.cpl_terms_shrunk;
            }
            ents.push_back(TuEnt{c, m, cs.coeff, cs.s_prev,
                                 cs.sat.empty() ? '\0' : letterOfSat(cs.sat),
                                 cs.kind});
            continue;
        }
        if (buffer_all) {   // unshrunk, but routed through the buffer (card 1f)
            ents.push_back(TuEnt{c, cs.coeff, cs.coeff, cs.s_prev,
                                 cs.sat.empty() ? '\0' : letterOfSat(cs.sat),
                                 cs.kind});
            continue;
        }
        row.shared.emplace_back(c, cs.coeff);
        rhs += cs.coeff * cs.s_prev;          // + M_rj·ŝ_prev,j
    }
    if (buffer_all) {
        tuGaugeRestore(ents, carry_cpl_gauge ? &trunc : nullptr);
        for (const TuEnt& e : ents) {
            row.shared.emplace_back(e.c, e.m_shr);
            rhs += e.m_shr * e.s_prev;
        }
    }
    // Card SCL2 X2: off card CTU's exact route the history columns are not this row's
    // generation, so its history terms are marginalized at their harvest variance.
    if (shx) sharedHistoryRowTerms(*shx, lc, nullptr, dep_var, R.cxu);
    sigma2 = var_eff + dep_var + tu_var;
    if (!(sigma2 > 0.0) || !std::isfinite(rhs)) return false;  // skip: σ² ≤ 0 or non-finite RHS
    row.y = rhs;
    return true;
}

bool CouplingRowBuilder::exactRowIngredients(Eigen::Index col, double x_lin_val,
                                             const LocalCarry& lc, double var_eff,
                                             double gap, PendCplRow& row, double& sigma2,
                                             CouplingExactRow* xr_keep) {
    const CouplingExactEpoch& xe = *cxu;
    CouplingExactCounters& X = R.cxu;
    row.local_col = col;
    double rhs = lc.value_abs - x_lin_val;   // (r̂_prev − r_lin)
    double dep_var = 0.0;                    // Σ M²·P_prev over the marginalized terms with no dense row
    double tu_var  = 0.0;                    // Σ M²·κ·q over the scalar terms (no dense row; SH)
    // Every coefficient is accumulated per column: the dense spread over S, a pinned term
    // and the pivot's deficit can land on one column, and the append assigns (does not add).
    std::map<Eigen::Index, double> emit;
    std::map<char, double> out_clk_def;      // clock-family deficit of the terms outside the block
    std::vector<std::pair<int, double>> pterms;
    for (const CoupledShared& cs : lc.coupling) {
        if (cs.kind == SharedParamKind::VtecSh) {
            // Never in the dense block: the card 1d / 1d-SH treatment, unchanged.
            ++X.terms_out_sh;
            const Eigen::Index c = resolveSharedCol(cs);
            if (c < 0 || tuReinitSince(cs, lc.last_seen_s) || carry_cpl_shmarg) {
                dep_var += cs.coeff * cs.coeff * cs.p_prev;
                ++R.cpl_terms_departed;
                continue;
            }
            ++R.cpl_terms_present;
            const double qj = couplingQ(cs) * gap;
            double m = cs.coeff;
            if (qj > 0.0 && cs.p_prev > 0.0) {
                const double kap = cs.p_prev / (cs.p_prev + qj);
                tu_var += cs.coeff * cs.coeff * kap * qj;
                m = kap * cs.coeff;
                ++R.cpl_terms_shrunk;
            }
            emit[c] += m;
            rhs += m * cs.s_prev;
            continue;
        }
        const std::string key = sharedCarryKey(cs.kind, cs.sat, cs.signal);
        const auto pit = xe.p_of_key.find(key);
        if (pit != xe.p_of_key.end()) {
            // A stashed dense key: the one rule (S updated, D re-attributed).
            pterms.emplace_back(pit->second, cs.coeff);
            const int s = xe.s_of_p[static_cast<std::size_t>(pit->second)];
            if (s >= 0) {
                ++X.terms_s;
                ++R.cpl_terms_present;
                if (std::abs(cs.s_prev - xe.shat_s(s)) > 1e-9) ++X.sprev_mismatch;
            } else {
                const int route = xe.route_p[static_cast<std::size_t>(pit->second)];
                if (route >= 0 && route < kCouplingExactRoutes) ++X.terms_d[route];
                ++R.cpl_terms_departed;
            }
            continue;
        }
        const char cc      = cs.sat.empty() ? '\0' : letterOfSat(cs.sat);
        const bool is_clk  = (cs.kind == SharedParamKind::Clock);
        const Eigen::Index c = resolveSharedCol(cs);
        if (!(cs.p_prev > 0.0)) {
            // Datum-pinned (card 1e): exactly known, kept as harvested.
            if (c < 0) {
                if (is_clk) out_clk_def[cc] += cs.coeff;
                ++X.terms_out_departed;
                ++R.cpl_terms_departed;
                continue;
            }
            ++X.terms_pinned;
            ++R.cpl_terms_present;
            emit[c] += cs.coeff;
            rhs += cs.coeff * cs.s_prev;
            continue;
        }
        if (c < 0 || tuReinitSince(cs, lc.last_seen_s) || readmitDiverted(cs)) {
            // No dense row and not carried into this epoch: marginalized, as card 1 does.
            dep_var += cs.coeff * cs.coeff * cs.p_prev;
            if (is_clk) out_clk_def[cc] += cs.coeff;
            ++X.terms_out_departed;
            ++R.cpl_terms_departed;
            continue;
        }
        // Present with no dense row (the fold's diagonal leftover): card 1d's kappa at the
        // inflation the fold applies to that key, where the mirror has it.
        const auto qo = xe.q_outside.find(key);
        const double qj = (qo != xe.q_outside.end()) ? qo->second : couplingQ(cs) * gap;
        double m = cs.coeff;
        if (qj > 0.0) {
            const double kap = cs.p_prev / (cs.p_prev + qj);
            tu_var += cs.coeff * cs.coeff * kap * qj;
            m = kap * cs.coeff;
            ++R.cpl_terms_shrunk;
        }
        if (is_clk) out_clk_def[cc] += cs.coeff - m;
        ++X.terms_out_scalar;
        ++R.cpl_terms_present;
        emit[c] += m;
        rhs += m * cs.s_prev;
    }
    CouplingExactRow xr = couplingExactRow(xe, pterms);
    if (!xr.ok) {
        ++X.rows_unfactored;
        return false;                        // non-finite update: the row is not emitted
    }
    for (Eigen::Index s = 0; s < xe.nS(); ++s) {
        const double m = xr.m_new(s);
        if (m == 0.0) continue;
        emit[xe.col_of_s[static_cast<std::size_t>(s)]] += m;
        rhs += m * xe.shat_s(s);
        X.shrink_abs_sum += std::abs(xr.m_s(s) - m);
    }
    // Card SCL: the row's terms on the clock clones (the rows' copy of the epoch has the
    // cloned clocks in S on their clones, at Q = 0).  Empty, and nothing counted, off.
    if (!xe.clone_s.empty()) {
        long long nt = 0;
        for (Eigen::Index s = 0; s < xe.nS(); ++s)
            if (xe.clone_s[static_cast<std::size_t>(s)] && xr.m_new(s) != 0.0) ++nt;
        X.terms_clone += nt;
        if (nt > 0) ++X.rows_clone;
    }
    // The clock-family deficit onto the constellation's pinned pivot: the row keeps its
    // harvest identity along the declared clock gauge (the only null direction the rank
    // gate declares here), so the gate sees the same null space as without the card.
    std::map<char, double> def = xr.clk_deficit;
    for (const auto& [cc, d] : out_clk_def) def[cc] += d;
    for (const auto& [cc, d] : def) {
        if (d == 0.0) continue;
        const auto pv = xe.pivot.find(cc);
        if (pv == xe.pivot.end()) {
            // The stash's pivot left the layout (its satellite set, or a pivot handover):
            // spread the deficit evenly over the constellation's present clock columns in S,
            // which keeps the row's clock-family sum at its harvest value and so its
            // blindness to the declared gauge (card 1f's restore, used only here).  With no
            // present clock of the constellation the row stays gauge-variant (counted).
            std::vector<Eigen::Index> cs;
            for (Eigen::Index s = 0; s < xe.nS(); ++s) {
                const int p = xe.p_of_s[static_cast<std::size_t>(s)];
                if (xe.clk_p[static_cast<std::size_t>(p)] && xe.cc_p[static_cast<std::size_t>(p)] == cc)
                    cs.push_back(s);
            }
            if (cs.empty()) {
                ++X.gauge_orphan;
                continue;
            }
            const double share = d / static_cast<double>(cs.size());
            for (Eigen::Index s : cs) {
                emit[xe.col_of_s[static_cast<std::size_t>(s)]] += share;
                rhs += share * xe.shat_s(s);
            }
            ++X.gauge_spread;
            continue;
        }
        emit[pv->second.first] += d;
        rhs += d * pv->second.second;
        ++X.gauge_held;
    }
    // Card SCL2 X2: the row's terms on the shared-history columns (constant, no walk, prior
    // mean 0, so the right-hand side does not move; no gauge member, so no deficit); a stale
    // generation or a column not registered marginalizes them.  No-op off.
    if (shx) sharedHistoryRowTerms(*shx, lc, &emit, dep_var, X);
    if (xr.var_raw < 0.0) ++X.var_negative;
    X.var_add_sum += xr.var_add;
    ++X.rows_exact;
    row.shared.clear();
    row.shared.reserve(emit.size());
    for (const auto& [c, m] : emit)
        if (m != 0.0) row.shared.emplace_back(c, m);
    X.terms_emitted += static_cast<long long>(row.shared.size());
    sigma2 = var_eff + dep_var + tu_var + xr.var_add;
    if (!(sigma2 > 0.0) || !std::isfinite(rhs)) return false;
    row.y = rhs;
    if (xr_keep) *xr_keep = std::move(xr);   // card WSX: the pair term reads it in the block
    return true;
}

bool CouplingRowBuilder::buildLocalBlock(const std::vector<LocalBlockCol>& cols) {
    // Local block (F3, network.phase_leveling_local_block, SPEC_amb_iono_split_overcount).
    //
    // (1) Per column, the scalar-path ingredients, i.e. the row buildCplRow would
    //     have emitted for it: +1/local, the present shared terms (κ-shrunk per
    //     card 1d, gauge-restored per card 1f), the RHS, and
    //     σ² = var_eff + Σ_departed M²P_prev + Σ_present M²κqΔt.  The departed terms
    //     stay per row, on Σ's diagonal, as on the scalar path; the joint departed
    //     marginalization of card 1b is mutually exclusive with this block (the
    //     caller's predicate).  A column refused by the scalar screen is dropped from
    //     the block and counted.
    // xr: the row's exact-route result, kept only for card WSX's pair term (ok false otherwise).
    struct Ing { PendCplRow row; double sigma2{0.0}; const LocalBlockCol* c{nullptr}; CouplingExactRow xr; };
    const bool wsx = cxu_cross && cxu != nullptr && cxu->ok;
    std::vector<Ing> ing;
    ing.reserve(cols.size());
    for (const LocalBlockCol& c : cols) {
        Ing g;
        g.c = &c;
        if (!scalarRowIngredients(c.col, c.x_lin, *c.lc, c.var_eff, c.gap, g.row, g.sigma2,
                                  wsx ? &g.xr : nullptr)) {
            ++R.pl_blk_col_refused;
            if (c.is_pc) R.pc_refused.push_back(c.col);   // card LGM: the injector backstops it
            if (c.is_rx_cb) R.rxcb_refused.push_back(c.col);   // card L5B: not a fold
            continue;
        }
        // The ZWD column (B9-in-block) is counted only after it survives the scalar
        // screen, i.e. once it is a row of this block.  The caller cannot know that,
        // and counting at push time would report a refused ZWD as carried.
        if (c.is_zwd) ++R.pl_blk_zwd_cols;
        // A per-LOS iono column (I-in-block) follows the same rule for the same
        // reason: it is counted once it is a row of this block, not at push time.
        if (c.is_iono) ++R.pl_blk_iono_cols;
        // A receiver-clock-family column (RX-CLK-in-block) follows the same rule.
        // The ISB count is a sub-count, not a partition: an ISB column increments
        // both counters.
        if (c.is_rx_clk) {
            ++R.pl_blk_rxclk_cols;
            if (c.is_rx_isb) ++R.pl_blk_rxclk_isb_cols;
        }
        if (c.is_rx_cb) ++R.pl_blk_rxcb_cols;   // RXCB-IN-BLOCK, same rule
        ing.push_back(std::move(g));
    }
    const Eigen::Index n = static_cast<Eigen::Index>(ing.size());
    if (n == 0) return true;   // nothing to emit; the per-column route would emit nothing either
    // (2) Σ: the diagonal from (1); off the diagonal, the harvested conditional
    //     cross covariances (N_rr⁻¹)_{c_i c_j}, valid only when both entries were
    //     stashed at the same harvest (pointer-equal PlHarvestBlock: the same
    //     posterior at the same epoch).  Otherwise the pair is zeroed and counted.
    //     Σ is then block-diagonal by harvest stamp, each block a principal
    //     sub-matrix of one posterior, so Σ is PD by construction.  The M2 v2 VCE
    //     factor α² is already in var_eff (the diagonal), so the cross term scales
    //     by √(α_i²·α_j²); this is a congruence with a diagonal matrix and preserves
    //     PD.  The variance floor was applied to the diagonal only (raising a
    //     diagonal also preserves PD).  The q·gap term is on the diagonal only (a
    //     per-column random walk has no cross term).
    PlBlockRaw blk;
    blk.rows.reserve(static_cast<std::size_t>(n));
    blk.Sigma = Eigen::MatrixXd::Zero(n, n);
    for (Eigen::Index i = 0; i < n; ++i) {
        const Ing& g = ing[static_cast<std::size_t>(i)];
        CplRowRaw r;
        r.local_col = g.row.local_col;
        r.shared    = g.row.shared;
        r.rhs       = g.row.y;
        r.var_eff   = g.sigma2;
        r.tag       = RowTag{localBlockColKind(*g.c), g.c->sat, g.c->signal};
        blk.rows.push_back(std::move(r));
        blk.Sigma(i, i) = g.sigma2;
    }
    for (Eigen::Index i = 0; i < n; ++i) {
        const LocalBlockCol& ci = *ing[static_cast<std::size_t>(i)].c;
        const LocalCarry& li = *ci.lc;
        for (Eigen::Index j = i + 1; j < n; ++j) {
            const LocalBlockCol& cj = *ing[static_cast<std::size_t>(j)].c;
            const LocalCarry& lj = *cj.lc;
            double v = std::numeric_limits<double>::quiet_NaN();
            if (li.blk && li.blk == lj.blk && li.blk_idx >= 0 && lj.blk_idx >= 0
                && li.blk_idx < li.blk->C.rows() && lj.blk_idx < li.blk->C.cols()) {
                v = li.blk->C(li.blk_idx, lj.blk_idx);          // n_{c_i}(c_j)
                if (!std::isfinite(v)) v = li.blk->C(lj.blk_idx, li.blk_idx);   // n_{c_j}(c_i), the transpose read
            }
            if (!std::isfinite(v)) {
                ++R.pl_blk_pair_missing;
                // The ZWD's own missing pairs, split out of the aggregate
                // (B9-in-block).  A ZWD stashed without a harvest block (its harvest
                // threw, or the pl loop built none for this station) makes every
                // pair with it miss.  That is the signature of a carried ZWD that is
                // not coupled, and it is not visible in pl_blk_pair_missing alone.
                if (ci.is_zwd || cj.is_zwd) ++R.pl_blk_zwd_missing_cross;
                // The iono columns' own share, split out for the same reason
                // (I-in-block).  An iono entry stashed without a harvest block (its
                // per-column harvest threw, or the station built no block that
                // epoch) makes every pair with it miss.  A pair that is both
                // (iono, zwd) counts in both splits by design: neither counter
                // partitions pl_blk_pair_missing.
                if (ci.is_iono || cj.is_iono) ++R.pl_blk_iono_missing_cross;
                // The clock family's own share, split out for the same reason
                // (RX-CLK-in-block).  A clock entry stashed without a harvest block makes
                // every pair with it miss, which leaves a diagonal prior on the most
                // station-common direction.  A pair that is both (clock, zwd) or
                // (clock, iono) counts in both splits by design: no counter here
                // partitions pl_blk_pair_missing.
                if (ci.is_rx_clk || cj.is_rx_clk) ++R.pl_blk_rxclk_missing_cross;
                if (ci.is_rx_cb || cj.is_rx_cb) ++R.pl_blk_rxcb_missing_cross;   // RXCB-IN-BLOCK
                continue;
            }
            if (ci.vce_fac != 1.0 || cj.vce_fac != 1.0) v *= std::sqrt(ci.vce_fac * cj.vce_fac);
            // Card LGM: a Gauss-Markov column's cross covariance transitions with it.
            if (ci.phi != 1.0 || cj.phi != 1.0) v *= ci.phi * cj.phi;
            blk.Sigma(i, j) = v;
            blk.Sigma(j, i) = v;
        }
    }
    // (2a) Card WSX (network.carry_coupling_exact_cross): the exact update's bracket
    //      between two rows that both took the exact route, M_i' B M_k (couplingExactCross,
    //      the term the per-row form drops; the cross-station half stays out).  Added after
    //      the harvested cross covariances; not scaled by the VCE factor, as the
    //      diagonal's own addend is not.  B is positive semi-definite, so the added matrix
    //      is a Gram matrix over the exact rows, and its diagonal is already on Σ's diagonal
    //      (each row's var_add).  A pair with a row on card 1d's route takes nothing.
    if (wsx && n > 1) {
        CouplingExactCounters& X = R.cxu;
        bool any = false;
        for (Eigen::Index i = 0; i < n; ++i) {
            const Ing& gi = ing[static_cast<std::size_t>(i)];
            for (Eigen::Index k = i + 1; k < n; ++k) {
                const Ing& gk = ing[static_cast<std::size_t>(k)];
                if (!gi.xr.ok || !gk.xr.ok) {
                    ++X.cross_pairs_skipped;
                    continue;
                }
                const double v = couplingExactCross(*cxu, gi.xr, gk.xr);
                if (!std::isfinite(v)) {
                    ++X.cross_pairs_skipped;
                    continue;
                }
                blk.Sigma(i, k) += v;
                blk.Sigma(k, i) += v;
                any = true;
                ++X.cross_pairs;
                X.cross_abs_sum += std::abs(v);
                const double den = std::sqrt(gi.sigma2 * gk.sigma2);
                const double rho = den > 0.0 ? std::abs(v) / den : 0.0;
                const int cls = (gi.c->is_rx_cb || gk.c->is_rx_cb) ? 1 : 0;
                ++X.cross_rho_hist[cls][CouplingExactCounters::crossRhoBin(rho)];
                if (rho > X.cross_rho_max[cls]) X.cross_rho_max[cls] = rho;
            }
        }
        if (any) ++X.cross_blocks;
    }
    // (3) Whiten: Σ = LLᵀ (ridge retry inside), rows L⁻¹·[I | M | y] at unit weight.
    //     A non-SPD factor after the retry (or a kept-row count other than n)
    //     buffers nothing and returns false; the caller then emits the per-column
    //     rows and counts the fallback (invariant #4).
    Eigen::MatrixXd Hr, Hs;
    Eigen::VectorXd z;
    std::vector<Eigen::Index> kept;
    if (!whitenLocalBlock(blk, /*skip=*/{}, R.A_r.cols(), R.A_s.cols(), Hr, Hs, z, kept)
        || static_cast<Eigen::Index>(kept.size()) != n)
        return false;
    // (4) Buffer the n whitened rows as multi-local PendCplRow entries.  L⁻¹ is lower
    //     triangular, so row k has local entries on block columns 0..k (all pushed;
    //     the diagonal 1/L_kk > 0 guarantees a non-empty `local`, which is what
    //     distinguishes a block row from a scalar +1 row at append).  local_col names
    //     the row's primary column (block column k) for cpl_row_local_cols.
    blk.row0 = static_cast<Eigen::Index>(crows.size());
    for (Eigen::Index k = 0; k < n; ++k) {
        PendCplRow row;
        row.local_col = blk.rows[static_cast<std::size_t>(k)].local_col;
        row.local.reserve(static_cast<std::size_t>(k + 1));
        for (Eigen::Index j = 0; j <= k; ++j) {
            const Eigen::Index c = blk.rows[static_cast<std::size_t>(j)].local_col;
            row.local.emplace_back(c, Hr(k, c));
        }
        for (Eigen::Index s = 0; s < Hs.cols(); ++s) {
            const double v = Hs(k, s);
            if (v != 0.0) row.shared.emplace_back(s, v);
        }
        row.y = z(k);
        row.w = 1.0;                                     // unit-information whitened row
        row.tag = blk.rows[static_cast<std::size_t>(k)].tag;   // the primary column's (card NOM N2a)
        crows.push_back(std::move(row));
    }
    R.pl_blk = std::move(blk);
    ++R.pl_blk_blocks;
    R.pl_blk_rows += n;
    ++R.pl_blk_hist[n < 10 ? 0 : n < 25 ? 1 : n < 50 ? 2 : n < 75 ? 3 : 4];
    if (n > R.pl_blk_max_dim) R.pl_blk_max_dim = n;
    return true;
}

void CouplingRowBuilder::appendCouplingRows() {
    if (carry_cpl_joint) {
        // Joint path (card 1b): whiten this station's coupling rows jointly and append them as
        // unit-information rows (as appendShIonoRows does).  The pre-whitening
        // ingredients are retained so that a within-epoch RAIM refloat can
        // re-whiten the surviving rows.  R.carry_cols stays empty (the coupling
        // lives in R.A_s/R.A_r, not in the addStationPrior diagonal).
        if (!craw.empty()) {
            Eigen::MatrixXd Hr, Hs;
            Eigen::VectorXd z;
            std::vector<Eigen::Index> kept;   // every row (no skip set on the primary build)
            const bool ok = whitenCouplingRows(craw, white_pw, /*skip=*/{},
                                               R.A_r.cols(), R.A_s.cols(), Hr, Hs, z, kept);
            const Eigen::Index add = static_cast<Eigen::Index>(craw.size());
            const Eigen::Index m0  = R.A_s.rows();
            R.A_s.conservativeResize(m0 + add, Eigen::NoChange);
            R.A_r.conservativeResize(m0 + add, Eigen::NoChange);
            R.w.conservativeResize(m0 + add);
            R.y.conservativeResize(m0 + add);
            R.cpl_row_start = m0;                       // within-epoch-RAIM refloat handle
            R.cpl_row_local_cols.resize(static_cast<size_t>(add));
            for (Eigen::Index k = 0; k < add; ++k)
                R.cpl_row_local_cols[static_cast<size_t>(k)] =
                    craw[static_cast<size_t>(k)].local_col;
            // One tag per appended row (card NOM N2a): the joint rows, or on the scalar
            // fallback below the same rows as scalar coupling rows.
            const bool joint_ok = ok && Hr.rows() == add;
            for (Eigen::Index k = 0; k < add; ++k) {
                RowTag t = craw[static_cast<size_t>(k)].tag;
                t.kind = joint_ok ? ContributionKind::CouplingJoint
                                  : ContributionKind::CouplingScalar;
                R.row_tags.push_back(t);
            }
            if (ok && Hr.rows() == add) {
                R.A_s.bottomRows(add) = Hs;
                R.A_r.bottomRows(add) = Hr;
                R.w.segment(m0, add).setOnes();         // unit-information whitened rows
                R.y.segment(m0, add) = z;
                R.cpl_raw      = std::move(craw);        // for the refloat re-whiten
                R.cpl_white_pw = std::move(white_pw);
            } else {
                // Non-SPD joint covariance after the ridge retry: fall back to the
                // card 1 per-row scalar marginalization for this station this epoch
                // (dep_var = Σ_departed M_rj²·P_w, the joint diagonal), and leave
                // cpl_raw empty so that the refloat takes the scalar zero-row path.
                R.cpl_joint_fallback = true;
                R.A_s.bottomRows(add).setZero();
                R.A_r.bottomRows(add).setZero();
                for (Eigen::Index k = 0; k < add; ++k) {
                    const CplRowRaw& row = craw[static_cast<size_t>(k)];
                    double dep_var = 0.0;
                    for (const auto& dc : row.departed)
                        dep_var += dc.second * dc.second
                                 * white_pw[static_cast<size_t>(dc.first)];
                    const double sigma2 = row.var_eff + dep_var;
                    R.A_r(m0 + k, row.local_col) = 1.0;
                    for (const auto& sc : row.shared)
                        R.A_s(m0 + k, sc.first) = sc.second;
                    R.w(m0 + k) = (sigma2 > 0.0) ? 1.0 / sigma2 : 0.0;
                    R.y(m0 + k) = row.rhs;
                }
            }
        }
    } else {
        // Scalar path (card 1): batch-append the coupling rows to this station's block (one
        // conservativeResize, as appendShIonoRows does).  R.carry_cols is left
        // empty so that the serial consume skips addStationPrior; the coupling
        // lives entirely in R.A_s/R.A_r, consumed by addStation.
        const Eigen::Index add = static_cast<Eigen::Index>(crows.size());
        if (add > 0) {
            const Eigen::Index m0 = R.A_s.rows();
            R.A_s.conservativeResize(m0 + add, Eigen::NoChange);
            R.A_s.bottomRows(add).setZero();
            R.A_r.conservativeResize(m0 + add, Eigen::NoChange);
            R.A_r.bottomRows(add).setZero();
            R.w.conservativeResize(m0 + add);
            R.y.conservativeResize(m0 + add);
            R.cpl_row_start = m0;                       // within-epoch-RAIM refloat handle
            R.cpl_row_local_cols.resize(static_cast<size_t>(add));
            for (Eigen::Index k = 0; k < add; ++k) {
                const PendCplRow& row = crows[static_cast<size_t>(k)];
                if (row.local.empty())
                    R.A_r(m0 + k, row.local_col) = 1.0;
                else   // whitened block row (F3): one row of L⁻¹ over the block's local columns
                    for (const auto& lcoef : row.local)
                        R.A_r(m0 + k, lcoef.first) = lcoef.second;
                for (const auto& sc : row.shared)
                    R.A_s(m0 + k, sc.first) = sc.second;
                R.w(m0 + k) = row.w;
                R.y(m0 + k) = row.y;
                R.cpl_row_local_cols[static_cast<size_t>(k)] = row.local_col;
                R.row_tags.push_back(row.tag);   // card NOM N2a
            }
        }
    }
}

void foldAndDumpCouplingRows(bool carry_cpl_cond,
                            const std::vector<StationAssembly>& asm_res,
                            const NetworkEstimator& est,
                            const std::vector<std::string>& registered_clock_sats,
                            const std::set<std::string>& shared_phase_keys,
                            const std::vector<std::pair<std::string, std::string>>& registered_code_keys,
                            double tow,
                            const std::shared_ptr<spdlog::logger>& log,
                            long long& n_cpl_terms_present,
                            long long& n_cpl_terms_departed,
                            long long& n_cpl_terms_shrunk,
                            long long& n_cpl_gauge_held,
                            long long& n_cpl_gauge_orphan,
                            long long& n_cpl_reinit_diverted,
                            long long& n_cpl_readmit_diverted,
                            int& n_cpl_row_dump,
                            std::map<std::pair<char,int>, std::pair<long long,double>>*
                                cpl_orphan_fam_total) {
    // Coupling observability (cards 1, 1d): fold the per-station coupling-term
    // counts into the run totals.  Serial: the parallel produce writes only its own
    // slot.
    if (carry_cpl_cond) {
        std::map<std::pair<char,int>, std::pair<long long,double>> ep_orphans;
        for (const StationAssembly& Rc : asm_res) {
            n_cpl_terms_present  += Rc.cpl_terms_present;
            n_cpl_terms_departed += Rc.cpl_terms_departed;
            n_cpl_terms_shrunk   += Rc.cpl_terms_shrunk;
            n_cpl_gauge_held     += Rc.cpl_gauge_held;
            n_cpl_gauge_orphan   += Rc.cpl_gauge_orphan;
            n_cpl_reinit_diverted += Rc.cpl_reinit_diverted;
            n_cpl_readmit_diverted += Rc.cpl_readmit_diverted;
            for (const auto& [fam, o] : Rc.cpl_orphan_fam) {
                auto& e = ep_orphans[fam];
                e.first  += o.first;
                e.second += o.second;
            }
        }
        // Orphan attribution: one log line per epoch that has any orphans, naming
        // the (constellation, kind) families with their counts and Σ|m|, so that
        // the run log carries the per-epoch attribution the run total cannot.
        if (!ep_orphans.empty()) {
            std::string detail;
            for (const auto& [fam, o] : ep_orphans) {
                const char* kind_s =
                    fam.second == static_cast<int>(SharedParamKind::Clock)     ? "CLK" :
                    fam.second == static_cast<int>(SharedParamKind::PhaseBias) ? "PB"  :
                    fam.second == static_cast<int>(SharedParamKind::CodeBias)  ? "CB"  : "SH";
                detail += fmt::format("{}{}|{}:{} (Σ|m| {:.3g})", detail.empty() ? "" : ", ",
                                      fam.first, kind_s, o.first, o.second);
                if (cpl_orphan_fam_total) {
                    auto& t = (*cpl_orphan_fam_total)[fam];
                    t.first  += o.first;
                    t.second += o.second;
                }
            }
            log->info("Network (Card 1f orphan attribution) tow {:.0f}: {} family-sum "
                      "orphan(s) this epoch — {} — truncated mass with no present "
                      "same-constellation recipient; the row stays gauge-dirty for that "
                      "family (no cross-constellation fallback by design).",
                      tow, std::accumulate(ep_orphans.begin(), ep_orphans.end(), 0LL,
                                           [](long long a, const auto& kv) { return a + kv.second.first; }),
                      detail);
        }
    }
    // Debug dump (card 1d): the first assembled coupling rows of the run (weight, RHS,
    // A_s sparsity and magnitude), i.e. what addStation receives, for A/B-arm
    // comparison.  The budget n_cpl_row_dump spans the run; log-only.
    std::map<Eigen::Index, CoupledShared> cpl_col2desc_dbg;
    if (carry_cpl_cond && n_cpl_row_dump > 0) {
        for (const std::string& sat : registered_clock_sats) {
            try { CoupledShared d; d.kind = SharedParamKind::Clock; d.sat = sat;
                  cpl_col2desc_dbg.emplace(est.clockColumn(sat), std::move(d)); } catch (...) {}
        }
        for (const auto& pk : shared_phase_keys) {
            const auto parts = StateKeys::parseSatPhaseBiasKey(pk);
            if (!parts) continue;
            try { CoupledShared d; d.kind = SharedParamKind::PhaseBias;
                  d.sat = parts->satellite; d.signal = parts->signal;
                  cpl_col2desc_dbg.emplace(
                      est.phaseBiasColumn(parts->satellite, parts->signal), std::move(d)); } catch (...) {}
        }
        for (const auto& kv : registered_code_keys) {
            try { CoupledShared d; d.kind = SharedParamKind::CodeBias;
                  d.sat = kv.first; d.signal = kv.second;
                  cpl_col2desc_dbg.emplace(
                      est.codeBiasColumn(kv.first, kv.second), std::move(d)); } catch (...) {}
        }
    }
    if (carry_cpl_cond && n_cpl_row_dump > 0)
        for (size_t i = 0; i < asm_res.size() && n_cpl_row_dump > 0; ++i) {
            const StationAssembly& Rc = asm_res[i];
            if (!Rc.ok || Rc.cpl_row_start < 0) continue;
            const auto nrows = static_cast<Eigen::Index>(Rc.cpl_row_local_cols.size());
            for (Eigen::Index r = 0; r < nrows && n_cpl_row_dump > 0; ++r, --n_cpl_row_dump) {
                const Eigen::Index row = Rc.cpl_row_start + r;
                if (row >= Rc.A_s.rows()) break;
                const Eigen::Index nnz =
                    (Rc.A_s.row(row).array() != 0.0).count();
                // Exact-vanishing probe: a coupling row on a local column with no
                // data rows is absorbed completely by the free local in the Schur
                // elimination (it adds nothing to the reduced border, at any weight).
                const Eigen::Index lc = Rc.cpl_row_local_cols[static_cast<size_t>(r)];
                const Eigen::Index data_rows_on_col =
                    (Rc.A_r.col(lc).head(Rc.cpl_row_start).array() != 0.0).count();
                // Gauge diagnostic: Σ of the row's A_s coefficients over each
                // constellation's {clk, pb, cb} columns (the structural null
                // direction has weight +1 on each).  A gauge-invariant row has this
                // sum equal to minus its local's gauge weight: 0 for a local with no
                // gauge weight, −1 for a local moved by +1 under that constellation's
                // gauge (docs/SPEC_card1_coupling_carry.md §1).
                std::map<char, double> gsum;
                for (const auto& [col, d] : cpl_col2desc_dbg)
                    if (d.kind != SharedParamKind::VtecSh && col < Rc.A_s.cols()
                        && !d.sat.empty())
                        gsum[letterOfSat(d.sat)] += Rc.A_s(row, col);
                std::string gs;
                for (const auto& [cch, v] : gsum) gs += fmt::format("{}:{:+.3e} ", cch, v);
                log->info("Network (Card 1d dbg): TOW {:.1f} sta {} cplrow {} local_col {}: "
                          "w={:.9e} y={:.9e} A_s nnz={} max|A_s|={:.9e} data_rows_on_local={} "
                          "gauge_sums[{}]",
                          tow, i, r, lc, Rc.w(row), Rc.y(row), nnz,
                          nnz > 0 ? Rc.A_s.row(row).cwiseAbs().maxCoeff() : 0.0,
                          data_rows_on_col, gs);
            }
        }
}

double phaseBiasShiftOf(const std::map<std::pair<std::string, std::string>, double>& shift_sat,
                        const std::string& sat, const std::string& sig) {
    const auto bs = shift_sat.find({sat, sig});
    if (bs == shift_sat.end() || !std::isfinite(bs->second) || bs->second == 0.0) return 0.0;
    return bs->second;
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the coupling carry (cards 1, 1b, 1d).
int networkCouplingRunEnd(const NetworkRunEndView& v) {
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const bool carry_cpl_cond = v.ctx.knobs.carry_cpl_cond;
    const bool carry_cpl_joint = v.ctx.knobs.carry_cpl_joint;
    const bool carry_cpl_tu = v.ctx.knobs.carry_cpl_tu;
    const auto& n_cpl_joint_fallback = v.ctx.counters.n_cpl_joint_fallback;
    const auto& n_cpl_stash = v.ctx.counters.n_cpl_stash;
    const auto& n_cpl_stash_empty = v.ctx.counters.n_cpl_stash_empty;
    const auto& n_cpl_harvest_fail = v.ctx.counters.n_cpl_harvest_fail;
    const auto& n_cpl_terms_present = v.ctx.counters.n_cpl_terms_present;
    const auto& n_cpl_terms_departed = v.ctx.counters.n_cpl_terms_departed;
    const auto& n_cpl_terms_shrunk = v.ctx.counters.n_cpl_terms_shrunk;

    if (carry_cpl_joint && n_cpl_joint_fallback > 0)
        log->warn("Network (Card 1b): {} station-epoch(s) had a non-SPD joint coupling-row "
                  "covariance after the ridge retry and fell back to the Card-1 per-row scalar "
                  "marginalization for that station (the between-row DD correlation is not "
                  "recovered on those). Investigate if the count is a large fraction of "
                  "station-epochs.", n_cpl_joint_fallback);
    if (carry_cpl_cond)
        log->info("Network (Card 1/1d): coupling-carry volume — stashes: {} with a regression, "
                  "{} EMPTY-regression (diagonal degrade), {} harvest failures; injected terms: "
                  "{} present, {} departed, {} kappa-shrunk (time-update {}).",
                  n_cpl_stash, n_cpl_stash_empty, n_cpl_harvest_fail,
                  n_cpl_terms_present, n_cpl_terms_departed, n_cpl_terms_shrunk,
                  carry_cpl_tu ? "ON" : "OFF");
    return 0;
}

// Run-end report: coupling reinit sources (1g) and gauge projection (1f).
int networkCouplingProjectionRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const bool carry_cpl_cond = v.ctx.knobs.carry_cpl_cond;
    const auto& n_cpl_terms_departed = v.ctx.counters.n_cpl_terms_departed;

    // Realization-step reinit readout (Card 1g), with its own denominator: the run-total
    // departed count moves for many reasons and cannot tell armed-and-working from
    // armed-and-inert.
    if (carry_cpl_cond && ctx.knobs.carry_cpl_reinit) {
        log->info("Network (Card 1g): realization-step reinit sources — {} coupling term(s) "
                  "diverted to the departed route by the WIDENED (kind,sat,signal) probe "
                  "(terms the legacy sat-wide probe would have folded as PRESENT, in a "
                  "retired frame).", ctx.counters.n_cpl_reinit_diverted);
        if (ctx.counters.n_cpl_reinit_diverted == 0)
            log->warn("Network (Card 1g): armed but diverted ZERO terms — no realization-step "
                      "mark ever coincided with a live coupling term on the same "
                      "(kind,sat,signal).  The card is INERT for this run: either no "
                      "re-reference occurred, or the feeds are miswired.  Check the "
                      "PBGAUGE / eq34 / datum-slip event counts before reading an A/B as "
                      "'no effect'.");
    }
    // Coupling-row datum-gauge projection readout (Card 1f).  The projection acts only on
    // truncated rows, so "0 held / 0 orphan" with departed terms > 0 is a miswire, not a
    // quiet run; the line distinguishes the two (invariant #4).
    if (carry_cpl_cond && ctx.knobs.carry_cpl_gauge) {
        log->info("Network (Card 1f): coupling-row datum-gauge projection — {} "
                  "(constellation,kind) family sum(s) HELD at their harvest value against a "
                  "truncated column, {} ORPHANED (truncated mass with no present recipient — "
                  "those rows stayed gauge-dirty).",
                  ctx.counters.n_cpl_gauge_held, ctx.counters.n_cpl_gauge_orphan);
        if (ctx.counters.n_cpl_gauge_held == 0 && ctx.counters.n_cpl_gauge_orphan == 0
            && n_cpl_terms_departed > 0)
            log->warn("Network (Card 1f): {} departed coupling term(s) were injected but the "
                      "gauge projection held NOTHING — the card is armed and inert.  Expected "
                      "causes: every departed term was VtecSh or had an empty satellite id "
                      "(both excluded by design), or the rows never reached the restore.  "
                      "Investigate before reading an A/B as 'no effect'.",
                      n_cpl_terms_departed);
        if (ctx.counters.n_cpl_gauge_orphan > 0) {
            // Orphan attribution: per-(constellation, kind) totals, so the run-end line
            // names where the gauge-dirty residue lives; per-epoch lines are in the run log
            // ("Card 1f orphan attribution").  There is no recipient fallback, by design:
            // the sums are realized per constellation.
            std::string fam_detail;
            for (const auto& [fam, o] : ctx.counters.cpl_orphan_fam_total) {
                const char* kind_s =
                    fam.second == static_cast<int>(SharedParamKind::Clock)     ? "CLK" :
                    fam.second == static_cast<int>(SharedParamKind::PhaseBias) ? "PB"  :
                    fam.second == static_cast<int>(SharedParamKind::CodeBias)  ? "CB"  : "SH";
                fam_detail += fmt::format("{}{}|{}: {} (Σ|m| {:.3g})",
                                          fam_detail.empty() ? "" : ", ",
                                          fam.first, kind_s, o.first, o.second);
            }
            log->warn("Network (Card 1f): {} family(ies) had truncated coefficient mass with NO "
                      "present column to carry it — the whole family departed at once, so the "
                      "row cannot be made gauge-invariant this epoch and still observes the "
                      "datum gauge.  This is the residue the card cannot remove; if it is a "
                      "large fraction of `held`, the carry is outliving its border.  "
                      "Attribution: [{}].",
                      ctx.counters.n_cpl_gauge_orphan, fam_detail);
        }
    }
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor

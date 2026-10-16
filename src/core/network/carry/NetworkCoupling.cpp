/**
 * @file NetworkCoupling.cpp
 * @brief Conditional coupling-row builder, cards 1, 1d (see the header).
 *
 * At decomposition step INC-7b the coupling code was moved unchanged from
 * Application::networkPassB_ (ApplicationNetworkAssembly.cpp): the six former [&]
 * lambdas became CouplingRowBuilder member functions whose captures are const
 * reference or value members, and the serial counter fold and the card 1d debug
 * dump became foldAndDumpCouplingRows.  The capture-to-member map is in the INC-7b
 * cut spec.
 */

#include "network/carry/NetworkCoupling.hpp"

#include "network/solve/NetworkEstimator.hpp"   // est.clockColumn / phaseBiasColumn / codeBiasColumn / vtecCoeffColumn
#include "network/carry/NetworkSharedHistory.hpp"   // sharedHistoryRowTerms (card SCL2 X2)
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
#include "network/state/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/runend/NetworkRunEnd.hpp"
#include "network/driver/NetworkEpoch.hpp"   // NetworkEpoch (card NOM3 step EP1)
#include "network/driver/NetworkRun.hpp"     // NetworkRun (card NOM3 step EP1)
#include "network/driver/NetworkSetupResult.hpp"
#include "network/driver/NetworkRunTypes.hpp"
#include "network/diag/NetworkRunTraces.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "network/assembly/NetworkStationBuild.hpp"   // StationBuild, StationCarryFrame (card NOM3 step PB1b)
#include "network/assembly/NetworkContributions.hpp"
#include "network/assembly/NetworkParamInventory.hpp"
#include <Eigen/Dense>
#include <algorithm>
#include <tuple>
#include "network/carry/NetworkLevelingCarry.hpp"   // the ambiguity-carry helpers (card NOM3 step PB1b)

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
        const std::map<SharedParamKind, double>* h =
            net::getSig(sat_signals, cs.sat, sig, &net::SatSignalState::tu_reinit);
        if (!h) return false;
        const auto it = h->find(cs.kind);
        return it != h->end() && it->second > since_s;
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
    if (!sats || cs.kind == SharedParamKind::VtecSh || cs.sat.empty()) return false;
    const long long* until = net::getSat(*sats, cs.sat, &net::SatelliteState::readmit_marg);
    if (!until || loop_ep >= *until) return false;
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
    // With the gauge projection armed (card 1f), every present term is emitted through
    // the buffer, so the restore also runs with the κ time update off.  The iteration
    // order is unchanged and departed terms never touch `rhs`, so the off path is
    // bit-identical.
    const bool buffer_all = carry_cpl_tu || carry_cpl_gauge;
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
        // returned satellite's columns take the departed route for K epochs (a column
        // re-registered after a gap is still settling).
        if (readmitDiverted(cs)) {
            dep_var += cs.coeff * cs.coeff * cs.p_prev;
            ++R.cpl_terms_departed;
            note_trunc(cs);
            continue;
        }
        // A column reinit-folded since this row's harvest (card 1d) is the q = ∞
        // limit of the time update and is fully marginalized (κ = 0), mirroring the
        // dense fold's decorrelation rule.
        if (tuReinitSince(cs, lc.last_seen_s)) {
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
            // Never in the dense block: the card 1d treatment, unchanged.
            ++X.terms_out_sh;
            const Eigen::Index c = resolveSharedCol(cs);
            if (c < 0 || tuReinitSince(cs, lc.last_seen_s)) {
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
    //     stay per row, on Σ's diagonal, as on the scalar path.  A column refused by
    //     the scalar screen is dropped from the block and counted.
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
    // Batch-append the coupling rows to this station's block (one conservativeResize, as
    // appendShIonoRows does).  R.carry_cols is left empty so that the serial consume
    // skips addStationPrior; the coupling lives entirely in R.A_s/R.A_r, consumed by
    // addStation.
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

/**
 * @brief Run-end section: the coupling carry (cards 1, 1d).
 *
 * The coupling carry (cards 1, 1d): the carry volume.
 *
 * Gate: `carry_cpl_cond`.
 *
 * @param v The run at its end (network/runend/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkCouplingCarryRunEnd(const NetworkRunEndView& v) {
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const bool carry_cpl_cond = v.ctx.knobs.carry_cpl_cond;
    const bool carry_cpl_tu = v.ctx.knobs.carry_cpl_tu;
    const auto& n_cpl_stash = v.ctx.counters.n_cpl_stash;
    const auto& n_cpl_stash_empty = v.ctx.counters.n_cpl_stash_empty;
    const auto& n_cpl_harvest_fail = v.ctx.counters.n_cpl_harvest_fail;
    const auto& n_cpl_terms_present = v.ctx.counters.n_cpl_terms_present;
    const auto& n_cpl_terms_departed = v.ctx.counters.n_cpl_terms_departed;
    const auto& n_cpl_terms_shrunk = v.ctx.counters.n_cpl_terms_shrunk;

    if (carry_cpl_cond)
        log->info("Network (Card 1/1d): coupling-carry volume — stashes: {} with a regression, "
                  "{} EMPTY-regression (diagonal degrade), {} harvest failures; injected terms: "
                  "{} present, {} departed, {} kappa-shrunk (time-update {}).",
                  n_cpl_stash, n_cpl_stash_empty, n_cpl_harvest_fail,
                  n_cpl_terms_present, n_cpl_terms_departed, n_cpl_terms_shrunk,
                  carry_cpl_tu ? "ON" : "OFF");
    return 0;
}

/**
 * @brief Run-end section: coupling reinit sources (1g) and gauge projection (1f).
 *
 * The coupling rows' realization-step reinit sources (card 1g) and their datum-gauge
 * projection (card 1f).
 *
 * Gate: each section under its own condition, as in the teardown: `carry_cpl_cond &&
 * ctx.knobs.carry_cpl_reinit`; `carry_cpl_cond && ctx.knobs.carry_cpl_gauge`.
 *
 * @param v The run at its end (network/runend/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkCouplingProjectionRunEnd(const NetworkRunEndView& v) {
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

// Run-end report of this subsystem: its sections in print order (card NOM2 step TD2;
// declared in network/carry/NetworkCoupling.hpp).
int networkCouplingRunEnd(const NetworkRunEndView& v) {
    return networkRunEndSections(v, {
        networkCouplingCarryRunEnd,
        networkCouplingProjectionRunEnd
    });
}

// ---- End of the run-end reports ----

// ---- PASS B serial stages (card NOM3 step PB1a; generated by scripts/nom_pb1_cut.py) ----

void networkCouplingReinitHistory(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (card NOM3 step PB1a; generated by scripts/nom_pb1_cut.py,
    //      each line checked by scripts/nom_pb1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    double pl_now_s = ep.clock.pl_now_s;
    const std::set<std::pair<std::string, std::string>>& eq34_reref = ep.events.phase_datum.eq34_reref;
    // ---- End of the bindings ----
    // The alias lines of networkPassB_ this stage reads, verbatim (card NOM3 step PB1a).
    const bool   carry_cpl_tu   = ctx.knobs.carry_cpl_tu;  // border time update of the coupling rows at injection, κ shrink + M²κq inflation (Card 1d); requires carry_cpl_cond (enforced at load)
    const bool   carry_cpl_reinit = ctx.knobs.carry_cpl_reinit; // widen the reinit feed to the realization-step sources and honour it with κ off (Card 1g); requires carry_cpl_cond (enforced at load)
    const double clk_mon_reinit_sig = ctx.knobs.clk_mon_reinit_sig;  // >0: inflate-q to this σ; ≤0: hard skip-stash refloat
    auto& prev_carry = ctx.carry.prev_carry;
    const std::set<std::string> prev_clock_refloat_sats =
        net::satSet(ctx.entities.sats, &net::SatelliteState::prev_clock_refloat);
    net::NetworkEntities& entities = ctx.entities;   // tu_reinit: the satellite signals (N4d)

    // Coupling-row time update (Card 1d, network.carry_coupling_time_update): record
    // the (kind, sat) shared keys whose carried state folds this epoch with the loose
    // clock-monitor reinit variance (the D2 inflate_q path: the stash marks the
    // flagged sat's clock and its phase-/code-bias entries, independently of kind;
    // the phase-hold path keeps its pb time-locked by design and is stashed with
    // inflate_q=false, so mirroring the stash flags reproduces the fold's exact
    // per-kind treatment).  A coupling term on such a column must not shrink-carry:
    // the reinit decorrelates the border column from its stashed value, so the term
    // is fully marginalized (κ=0, σ² += M²·P_prev), mirroring the dense fold's
    // decorrelation rule.  Kept as a history map (key → content time of the last
    // reinit fold): a coupling row injects against its own harvest time, so a reinit
    // inside a multi-epoch carry gap still marginalizes (a q=∞ segment anywhere
    // inside the gap composes to κ=0).  The Clock entry also covers the clock_model
    // on oscillator reinit (the clock never rides prev_carry there) and the
    // hard-refloat mode (clk_mon_reinit_sig ≤ 0 skip-stashes the flagged sat
    // entirely, so there is no prev_carry entry to mirror; all three kinds
    // decorrelate, matching the skip-stash).  Updated serially; PASS B only reads it.
    //
    // Card 1g (network.carry_coupling_reinit_sources) widens both halves of this: the
    // key gains a signal field (empty = the satellite-wide wildcard that these
    // clock-QC feeds write, since they decorrelate a whole satellite kind by kind),
    // and the realization-step sources below join it.  Those are a different
    // failure: not a decorrelated variance but a frame step.  A phase-anchor
    // handover, an eq34 chain re-reference or a datum-slip PB re-realization each
    // erase the re-referenced family's own carry, yet every other carried local
    // keeps a coupling[] entry on that column holding a retired-frame ŝ_prev; folded
    // at full weight, that injects the step as row bias at conditional-variance
    // confidence.  The step is unknown (unlike the eq34 D4 S-transform, which knows
    // its own shift and re-bases the carry exactly), so the departed route is the
    // only correct treatment.  Both reref sets are resolved at the phase-datum
    // election, strictly before this serial write and hence before PASS B reads it.
    if (carry_cpl_tu || carry_cpl_reinit) {
        for (const CarryPrior& cp : prev_carry)
            if (cp.inflate_q)
                net::putSig(entities, cp.sat, "", &net::SatSignalState::tu_reinit)[cp.kind] = pl_now_s;
        for (const std::string& sat : prev_clock_refloat_sats) {
            net::putSig(entities, sat, "", &net::SatSignalState::tu_reinit)[SharedParamKind::Clock] = pl_now_s;
            if (!(clk_mon_reinit_sig > 0.0)) {
                net::putSig(entities, sat, "", &net::SatSignalState::tu_reinit)[SharedParamKind::PhaseBias] = pl_now_s;
                net::putSig(entities, sat, "", &net::SatSignalState::tu_reinit)[SharedParamKind::CodeBias] = pl_now_s;
            }
        }
    }
    if (carry_cpl_reinit) {
        // The anchored-election handovers (QUEUE item 1(d)) and the eq34 chain
        // datum's re-references: both are sets of (sat, signal), which is exactly
        // the granularity at which a phase-bias datum is realized.
        for (const auto& [sat, sig] : ctx.carry.phase_anchor_reref)
            net::putSig(entities, sat, sig, &net::SatSignalState::tu_reinit)[SharedParamKind::PhaseBias] = pl_now_s;
        for (const auto& [sat, sig] : eq34_reref)
            net::putSig(entities, sat, sig, &net::SatSignalState::tu_reinit)[SharedParamKind::PhaseBias] = pl_now_s;
        // The datum-slip re-realization marks whole satellites (the slip is on the
        // pin-holder's arc, not on one signal), so it uses the satellite-wide
        // wildcard.
        for (const std::string& sat : net::satSet(ctx.entities.sats, &net::SatelliteState::datum_reref_pb))
            net::putSig(entities, sat, "", &net::SatSignalState::tu_reinit)[SharedParamKind::PhaseBias] = pl_now_s;
    }
}

// ---- End of the PASS B serial stages (card NOM3 step PB1a) ----

// ---- PASS B station steps (card NOM3 step PB1b; generated by scripts/nom_pb1b_cut.py) ----

void networkStationLocalBlock(StationCarryFrame& carry) {
    // ---- From the station build (card NOM3 step PB1b; generated by scripts/nom_pb1b_cut.py,
    //      each line checked by scripts/nom_pb1b_verify.py). ----
    StationBuild& build = carry.build;
    const NetworkRunContext& ctx = build.ctx;
    double pl_now_s = build.clock.pl_now_s;
    const int i = build.i;
    NetStation& st = build.st;
    StationAssembly& R = build.R;
    std::map<int, int>& h2local = R.h2local;
    const bool legacy_local_inject = carry.legacy_local_inject;
    const bool do_amb_carry = carry.do_amb_carry;
    const Eigen::VectorXd& x_lin = carry.x_lin;
    CouplingRowBuilder& cb = carry.cb;
    std::vector<PlPend>& pl_pend = carry.pl_pend;
    std::vector<LocalBlockCol>& iono_bc = carry.iono_bc;
    std::vector<LocalBlockCol>& rxclk_bc = carry.rxclk_bc;
    std::vector<LocalBlockCol>& rxcb_bc = carry.rxcb_bc;
    std::vector<char>& rxcb_ifb = carry.rxcb_ifb;
    std::vector<LocalBlockCol>& pc_bc = carry.pc_bc;
    const auto pl_finish_var = [&](double var_eff, const std::string& sat) {
        return networkAmbCarryFinishVar(carry, var_eff, sat);
    };
    const auto amb_state_note = [&](const std::string& sat,
                                    const std::string& signal,
                                    double var_eff, AmbFoldRoute route) {
        networkAmbStateNote(carry, sat, signal, var_eff, route);
    };
    const auto pl_emit_row = [&](Eigen::Index col, int amb_idx,
                                 const LocalCarry& lc, double var_eff,
                                 double gap_clamped,
                                 const std::string& sat = {},
                                 const std::string& signal = {}) {
        networkAmbCarryEmitRow(carry, col, amb_idx, lc, var_eff, gap_clamped, sat, signal);
    };
    // The alias lines of networkPassB_ this function reads, verbatim (card NOM3 step PB1b).
    const double pl_max_gap_s  = ctx.knobs.pl_max_gap_s;
    // Record, per folded ambiguity column, the variance the fold actually used and the
    // route it took, into this station's own StationAssembly (network.trace_amb_state,
    // RF-1 §7.4).  Measure-only; when off the map is never touched, so the produce loop
    // pays one bool test per folded column.
    const bool   amb_state_tr  = ctx.knobs.trace_amb_state;
    const auto&  vce_apply     = ctx.carry.vce_apply;     // carry variance factor α², unity when off (M2 v2)
    // Live predicate for F3: the block's rows are Card-1 coupling rows (A_s/A_r) (the
    // loader warns that the knob is inert without them).  A single const bool, so the
    // off path never reaches a block branch.
    const bool   pl_blk_on = ctx.knobs.pl_blk_on;
    // ZWD carry random-walk process noise (B9, m²/s): both fold sites read
    // `zwd_q_eff` below rather than a bare `zwd_q` alias, so a seam window's own q
    // cannot be missed at one of them (RS-10b).
    // Seam-scoped ZWD re-seed and unconditional Z-row capture (§17 finding 3).
    // zwd_seam_live: the post-resume inject window is open; run the exact B9 fold.
    // zwd_capture: snapshot-armed run; record R.pl_zwd for the harvest (no inject).
    // Both are stable across the parallel produce phase (the countdown decrements
    // serially at the post-solve harvest).
    const bool   zwd_seam_live = ctx.carry.zwd_seam_ep_left > 0;
    // The ZWD carry's effective RW q for this epoch's folds (RS-10b,
    // docs/SPEC_event_harness.md §21.21).  network.zwd_carry_q is in the warm-resume
    // fingerprint (a continuous carry q must cold-start), while the seam response
    // that borrows its weight is not, by design, so without a separate knob the seam
    // window's stiffness could not be tuned on a resume leg.
    // network.zwd_seam_reseed_q (default -1 = unset) replaces it, and only while the
    // window is live: a continuous carry_zwd row of a non-resumed epoch never sees
    // it, and an unset knob leaves this exactly `zwd_q` (byte-identical).  Resolved
    // once here, from the same `zwd_seam_live` the fold gates read, so the diagonal
    // B9 row, the B9-in-block column and the harvest's prior-residual census cannot
    // disagree about which q was folded.
    const double zwd_q_eff     = ctx.knobs.zwdQEff(zwd_seam_live);
    // Live predicate for B9-in-block (network.carry_zwd_in_block): the carried ZWD
    // column joins the F3 whitened block instead of folding as a diagonal coupling row
    // outside it.  Conjoined with pl_blk_on, so a station-epoch that takes the
    // per-column route (F3 off, coupling rows off) is never touched; there the ZWD
    // keeps its B9 row.  The loader already refuses the arm without carry_zwd +
    // phase_leveling_local_block; this is the live half of the same gate.
    const bool   zwd_blk_on    = ctx.knobs.zwd_blk_on;
    const net::StationTable& sta_rows = ctx.entities.stations;   // zwd: read-only in the parallel build
    // ---- End of the bindings ----

    if (!(legacy_local_inject && do_amb_carry)) return;
    // Off: iono_bc and rxclk_bc are always empty, so this predicate reduces
    // to `if (!pl_pend.empty())` (byte-identical).  On, it also admits a
    // station whose ambiguity set is entirely young/stale/absent but whose
    // iono or receiver-clock carry is live: those columns still whiten
    // jointly among themselves, which is the correct prior, and the
    // pl_pend-driven code below is a no-op on an empty pl_pend.
    if (!pl_pend.empty() || !iono_bc.empty() || !rxclk_bc.empty()
        || !rxcb_bc.empty() || !pc_bc.empty()) {
        bool blk_done = false;
        if (pl_blk_on) {
            // Local block (F3, network.phase_leveling_local_block,
            // SPEC_amb_iono_split_overcount): the station's admitted columns as
            // one jointly whitened block.  The tail (var floor, VCE factor) is
            // applied per column exactly as the per-row routes apply it, on the
            // diagonal only, and the builder reads the harvested conditional
            // cross covariances for the off-diagonals (see buildLocalBlock).
            // Every carried entry of this station that is not in pl_pend
            // (departed / stale / freed / reseed / non-finite x_lin) is thereby
            // marginalized out of the block: invariant #7's decoupling edge,
            // counted here and never conditioned on.  A non-SPD factor falls
            // through to the per-column route below and is counted.
            std::vector<LocalBlockCol> bc;
            bc.reserve(pl_pend.size());
            for (const PlPend& p : pl_pend) {
                bc.push_back(LocalBlockCol{
                    p.col, x_lin(p.amb_idx), p.lc,
                    pl_finish_var(p.var_eff, p.sat), p.gap,
                    vce_apply.factor(NetworkCarryVceApply::kPl, p.sat),
                    /*is_zwd=*/false, /*is_iono=*/false});
                // The column's identity, for its block row's tag (card NOM N2a).
                if (const auto col_sat = SatId::parse(p.sat)) {
                    bc.back().sat = *col_sat;
                    if (const auto col_sig = SignalId::fromObsCode(col_sat->sys, p.signal))
                        bc.back().signal = *col_sig;
                }
            }
            long long n_sta_carry = 0;   // read-only walk of this station's entries
            for (auto it = net::arcsOfStation(ctx.entities.arcs, static_cast<int>(i));
                 it != ctx.entities.arcs.end() && it->first.station.index == static_cast<int>(i);
                 ++it)
                if (it->second.amb) ++n_sta_carry;   // O8: the station's carry entries
            R.pl_blk_marg_cols += std::max<long long>(
                0, n_sta_carry - static_cast<long long>(pl_pend.size()));
            // B9-in-block (network.carry_zwd_in_block): the station's carried
            // ZWD as one more column of this same block.  A diagonal B9 row
            // asserts the ZWD direction outside the block, while every
            // ambiguity row inside it asserts that same station-common
            // direction implicitly: the ~n_LOS over-count of
            // SPEC_amb_iono_split_overcount §7 item 17 (which calls for
            // blocking all carried local columns), and the structural reason
            // the diagonal carry was falsified on the quiet bed.  Inside the
            // block the joint Σ carries the harvested (N_rr⁻¹) cross terms,
            // so the ZWD's overlap with the ambiguities is subtracted exactly.
            //
            // The predicates are the B9 injector's own, re-read here (the
            // behaviour is not duplicated: the diagonal route below is skipped
            // for exactly the station-epochs this pushes).  Two deliberate
            // differences from an ambiguity column:
            //   * No pl_finish_var tail: the var floor and the M2 VCE factor
            //     are keyed to the phase-leveling ambiguity family (α² is a
            //     per-satellite lookup), so the block's ZWD diagonal is the
            //     same zvar_eff the B9 row would have used; vce_fac 1.0 leaves
            //     the cross terms unscaled.
            //   * It is recorded (R.pl_zwd / R.pl_zwd_present) here as well as
            //     in the B9 block below; the values are identical, and the
            //     harvest needs the record whichever route folded.
            bool zwd_pushed = false;
            if (zwd_blk_on) {
                const size_t ztidx_sz = st.sm->getTropoIndex();
                if (ztidx_sz != std::numeric_limits<size_t>::max()) {
                    const int ztidx = static_cast<int>(ztidx_sz);
                    if (ztidx >= 0 && ztidx < x_lin.size()
                        && std::isfinite(x_lin(ztidx))) {
                        const auto zhit = h2local.find(ztidx);
                        if (zhit != h2local.end()) {
                            const Eigen::Index zcol =
                                static_cast<Eigen::Index>(zhit->second);
                            R.pl_zwd_present = true;   // record for the post-solve stash
                            R.pl_zwd = PlZwd{zcol, x_lin(ztidx)};
                            const auto* zit = net::get(sta_rows, StationId{static_cast<int>(i)},
                                                       &net::StationState::zwd);
                            if (zit != nullptr) {
                                const double zgap = pl_now_s - zit->last_seen_s;
                                if (zgap <= pl_max_gap_s) {
                                    const double zvar_eff =
                                        zit->var
                                        + zwd_q_eff * std::max(0.0, zgap);
                                    if (zvar_eff > 0.0) {
                                        bc.push_back(LocalBlockCol{
                                            zcol, x_lin(ztidx), &*zit,
                                            zvar_eff, std::max(0.0, zgap),
                                            /*vce_fac=*/1.0, /*is_zwd=*/true});
                                        zwd_pushed = true;
                                    }
                                }
                                // Card PRM phase 3a: a gap refusal here is
                                // counted at the diagonal B9 gate below, which
                                // it always reaches (ZwdGapSkip, once per entry).
                            }
                        }
                    }
                }
            }
            // I-in-block: the station's carried per-LOS iono columns as more
            // columns of this same block, pushed after the ZWD, so the whole
            // station (ambiguities, ZWD, iono) folds through one Cholesky and
            // every local-local overlap is subtracted by the harvested cross
            // terms rather than asserted twice.  Push order does not matter to
            // the algebra (Σ is symmetric and the pairing is by harvest-block
            // pointer, not by position); it is fixed only so pl_iono_blk_cols
            // is deterministic for the RAIM census.
            for (const LocalBlockCol& ic : iono_bc) {
                bc.push_back(ic);
                R.pl_iono_blk_cols.push_back(ic.col);
            }
            // RX-CLK-in-block: the station's receiver-clock family as the last
            // columns of this same block, so the whole station (ambiguities,
            // ZWD, iono, clock) folds through one Cholesky and every
            // local-local overlap is subtracted by the harvested cross terms
            // rather than asserted twice.  Push order does not matter to the
            // algebra (Σ is symmetric and the pairing is by harvest-block
            // pointer, not by position); it is fixed only so
            // pl_rxclk_blk_cols is deterministic for the RAIM census.
            for (const LocalBlockCol& rc : rxclk_bc) {
                bc.push_back(rc);
                R.pl_rxclk_blk_cols.push_back(rc.col);
            }
            // RXCB-IN-BLOCK: the receiver code-bias columns last.
            for (const LocalBlockCol& cc : rxcb_bc) {
                bc.push_back(cc);
                R.pl_rxcb_blk_cols.push_back(cc.col);
            }
            // Card LGM: the phase-colour columns last.
            for (const LocalBlockCol& pc : pc_bc) bc.push_back(pc);
            blk_done = bc.empty() ? true : cb.buildLocalBlock(bc);
            if (!blk_done) {
                ++R.pl_blk_fallback;
                // Record the failed block's dimension so the serial scatter
                // can warn with identities (RF-1 step 3, design risk #3).
                // Kept here (produce-phase local) rather than logged here:
                // this loop runs in parallel, and a log from it would come
                // out in thread order.
                R.pl_blk_fallback_dim =
                    static_cast<long long>(bc.size());
            }
            // The block route's fold record (RF-1 §7.4), only on success: a
            // fallback re-emits every pending column through pl_emit_row
            // below, which records the route it actually took.  That
            // distinction is design risk #3 (a non-SPD factor changing the
            // vehicle on one station-epoch without notice), made visible per
            // arc instead of only as a run total.
            if (amb_state_tr && blk_done)
                for (const PlPend& p : pl_pend)
                    amb_state_note(p.sat, p.signal,
                                   pl_finish_var(p.var_eff, p.sat),
                                   AmbFoldRoute::Block);
            // I-in-block degrade edge (invariant #4/#7): a non-SPD block means
            // the iono carries did not act this station-epoch.  Unlike the ZWD
            // there is no diagonal row to fall back to, by design, so this is a
            // real "carried nothing", not "carried it the old way" (the counter
            // name says so), and pl_iono_blk_cols is cleared because no
            // whitened iono row exists for the RAIM census to recognise.
            if (!blk_done && !iono_bc.empty()) {
                R.pl_blk_iono_not_folded +=
                    static_cast<long long>(iono_bc.size());
                R.pl_iono_blk_cols.clear();
            }
            // RX-CLK-in-block degrade edge (invariant #4/#7), read the same
            // way: a non-SPD block means the clock carries did not act this
            // station-epoch.  There is no diagonal row to fall back to, by
            // design, so this is a real "carried nothing", and
            // pl_rxclk_blk_cols is cleared because no whitened clock row
            // exists for the RAIM census to recognise.
            if (!blk_done && !rxclk_bc.empty()) {
                R.pl_blk_rxclk_not_folded +=
                    static_cast<long long>(rxclk_bc.size());
                R.pl_rxclk_blk_cols.clear();
            }
            // RXCB-IN-BLOCK degrade edge (invariant #4/#7): no diagonal row to
            // fall back to, by design, so a non-SPD block means the receiver
            // code-bias carry did not act this station-epoch.
            if (!blk_done && !rxcb_bc.empty()) {
                R.pl_blk_rxcb_not_folded +=
                    static_cast<long long>(rxcb_bc.size());
                R.pl_rxcb_blk_cols.clear();
            } else if (blk_done) {
                R.pl_cb_injected += static_cast<int>(rxcb_bc.size());
                // Card L5B: the receiver IFB columns that became rows of this
                // factorized block (a column the scalar screen refused is not one).
                for (std::size_t k = 0; k < rxcb_ifb.size(); ++k)
                    if (rxcb_ifb[k]
                        && std::find(R.rxcb_refused.begin(), R.rxcb_refused.end(),
                                     rxcb_bc[k].col) == R.rxcb_refused.end())
                        ++st.l5b.blk_folds;
            }
            // Card LGM: a phase-colour column folded through the block unless the
            // scalar screen refused it; on a non-SPD block each column takes the
            // per-column route with the same predicted copy.  A column that folds
            // on neither route gets a backstop birth row after the carry section,
            // so the station block never loses the column's only prior.
            if (!pc_bc.empty()) {
                for (const LocalBlockCol& pc : pc_bc) {
                    if (blk_done) {
                        if (std::find(R.pc_refused.begin(), R.pc_refused.end(),
                                      pc.col) != R.pc_refused.end())
                            continue;
                        R.pc_folded.push_back(pc.col);
                        ++R.pc_block_folds;
                    } else {
                        const std::size_t n0 = cb.crows.size();
                        cb.buildCplRow(pc.col, pc.x_lin, *pc.lc, pc.var_eff, pc.gap,
                                       pc.sat, pc.signal);
                        if (cb.crows.size() > n0) {
                            R.pc_folded.push_back(pc.col);
                            ++R.pc_fallback_folds;
                        }
                    }
                }
            }
            // B9-in-block: when the ZWD folded through the block, the diagonal
            // B9 row below must not also fire (that would be the double count
            // the in-block route removes), and network_zwd.csv's `carried` flag
            // stays accurate.  On a non-SPD block the flag stays false and the
            // diagonal row runs, counted as pl_blk_zwd_fallback_diag at that
            // site; the fallback is the plain B9 behaviour, never "no ZWD
            // memory".
            if (zwd_pushed && blk_done) {
                R.pl_zwd_in_block = true;
                R.pl_zwd_injected = true;   // a carry was actually folded (L7)
            }
        }
        if (!blk_done) {
            // The F3 fallback: the plain per-column route, tail and build in
            // the original visiting order.
            for (const PlPend& p : pl_pend) {
                const double var_eff = pl_finish_var(p.var_eff, p.sat);
                pl_emit_row(p.col, p.amb_idx, *p.lc, var_eff, p.gap,
                            p.sat, p.signal);
            }
        }
    }
}

void networkCouplingRegionRows(StationCarryFrame& carry) {
    // ---- From the station build (card NOM3 step PB1b; generated by scripts/nom_pb1b_cut.py,
    //      each line checked by scripts/nom_pb1b_verify.py). ----
    StationBuild& build = carry.build;
    const NetworkRunContext& ctx = build.ctx;
    CouplingRowBuilder& cb = carry.cb;
    // The alias lines of networkPassB_ this function reads, verbatim (card NOM3 step PB1b).
    const bool   carry_cpl_cond = ctx.knobs.carry_cpl_cond; // conditional local carry, coupling rows (Card 1); requires fullcov_on (enforced at load)
    // ---- End of the bindings ----

    if (!carry_cpl_cond) return;
    // Batch-append this station's conditional coupling rows into
    // R.A_s/R.A_r (scalar per row, and the F3 block's whitened rows;
    // Card 1).
    cb.appendCouplingRows();
}

// ---- End of the PASS B station steps (card NOM3 step PB1b) ----

} // namespace GnssProcessor

/**
 * @file NetworkCoupling.cpp
 * @brief Card-1/1b/1d conditional coupling-row builder (see the header).
 *
 * Decomposition INC-7b (2026-07-17) - the coupling cluster relocated VERBATIM
 * from Application::networkPassB_ (ApplicationNetworkAssembly.cpp); the six
 * former [&] lambdas become CouplingRowBuilder member functions whose captures
 * are now const reference / value members, and the serial counter fold +
 * Card-1d debug dump become foldAndDumpCouplingRows.  Bodies are textually
 * unchanged (dedent only; capture->member map in the INC-7b cut spec).
 */

#include "filter/NetworkCoupling.hpp"
#include "filter/NetworkYoungInflate.hpp"

#include "filter/NetworkEstimator.hpp"   // est.clockColumn / phaseBiasColumn / codeBiasColumn / vtecCoeffColumn
#include "config/GnssConfigTypes.hpp"    // GlobalVtecShConfig::q_per_degree (couplingQ)
#include "filter/StateKeySchema.hpp"     // StateKeys::parseSatPhaseBiasKey (Card-1d dump)

#include <spdlog/spdlog.h>               // Card-1d debug dump (log->info)
#include <fmt/format.h>                  // Card-1d dump gauge-sum formatting

#include <cmath>
#include <limits>            // F3 buildLocalBlock: NaN sentinel for a missing harvest pair
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
            // Per-layer q ladder — must stay consistent with the injector's
            // RW-toward-previous q (networkInjectShVtecPrior), which is the
            // SAME model expressed driver-side.
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
    // Card 1f: a TRUNCATED column's coefficient was deleted outright, so the family
    // is short by exactly that coefficient — the same kind of deficit the κ shrink
    // opens, and correctable the same way.  Seeding the deficit map from the
    // truncated sums (rather than only from the present entries' shrink) is the
    // whole fix: it restores Σ_family M to its HARVEST value instead of to the
    // surviving-present value.  A family with truncated mass but NO present
    // recipient has nowhere to put it — the row cannot be made gauge-invariant
    // this epoch, so it is counted and left alone (invariant #4: the miss is
    // TELEMETRY, not a silent partial fix).
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
    // Card 1g: the κ time update is no longer the sole gate.  A column that was
    // RE-REALIZED since this row's harvest carries an unknown frame step in its
    // stashed ŝ_prev, so folding it as a full-weight PRESENT term injects that step
    // as row bias — independently of whether the border time update is armed.
    if (!carry_cpl_tu && !carry_cpl_reinit) return false;
    const auto hit = [&](const std::string& sig) {
        const auto it = tu_reinit_hist.find({cs.kind, cs.sat, sig});
        return it != tu_reinit_hist.end() && it->second > since_s;
    };
    // Card 1g ON-path telemetry (standing invariant #4).  The run's departed-term total
    // moves for many reasons, so it cannot answer "did the realization-step widening do
    // anything?".  This counts the terms THIS rule diverted, and only those: the exact
    // (kind,sat,signal) probe firing where the legacy sat-wide probe would not have.
    if (carry_cpl_reinit && !cs.signal.empty() && hit(cs.signal) && !hit(std::string{}))
        ++R.cpl_reinit_diverted;
    // The clock-QC feeds mark a whole satellite (empty signal = sat-wide wildcard);
    // the realization-step feeds mark one (sat,signal) family.  Probing the exact
    // key FIRST and the wildcard second reproduces the legacy sat-wide matching
    // exactly when only the legacy feeds are populated.
    if (!carry_cpl_reinit) return hit(std::string{});
    return hit(cs.signal) || (!cs.signal.empty() && hit(std::string{}));
}

bool CouplingRowBuilder::readmitDiverted(const CoupledShared& cs) {
    // READMIT re-coupling edge: a satellite-side column inside its post-re-admission
    // window takes the departed (q=∞) route regardless of the row's harvest time — the
    // column exists and is bound, but its realization is still settling from the cold
    // seed, so conditioning a stiff carried row on it injects that settling as row bias
    // (F3's dense L⁻¹ then mixes it into every ambiguity of the station block).
    if (!readmit_marg || readmit_marg->empty() || cs.kind == SharedParamKind::VtecSh
        || cs.sat.empty())
        return false;
    const auto it = readmit_marg->find(cs.sat);
    if (it == readmit_marg->end() || loop_ep >= it->second) return false;
    ++R.cpl_readmit_diverted;
    return true;
}

void CouplingRowBuilder::buildCplRow(Eigen::Index col, double x_lin_val,
                                     const LocalCarry& lc, double var_eff,
                                     double gap) {
    if (carry_cpl_joint) {
        // Card 1b: same present/departed split as the scalar path, but the
        // DEPARTED columns become shared M_w columns (marginalized JOINTLY
        // at append) instead of a per-row scalar σ² addend.
        CplRowRaw raw;
        raw.local_col = col;
        raw.var_eff   = var_eff;
        double rhs = lc.value_abs - x_lin_val;   // (r̂_prev − r_lin)
        std::vector<TuEnt> ents;                 // Card 1d: buffered present terms
        // Card 1f: with the gauge projection armed the buffer owns ALL present-term
        // emission, so the restore can run with the κ time update OFF.  Iteration
        // order is unchanged and departed terms never touch `rhs`, so the OFF path
        // is bit-identical.
        const bool buffer_all = carry_cpl_tu || carry_cpl_gauge;
        std::map<std::pair<char, int>, double> trunc;  // Card 1f: (cc,kind) → Σ truncated coeff
        for (const CoupledShared& cs : lc.coupling) {
            Eigen::Index c = resolveSharedCol(cs);
            // Card 1d: a (kind, sat) column reinit-folded since this row's
            // harvest is decorrelated from its stashed value — the coupling
            // term's κ=0 (q=∞) limit IS the departed treatment, so route it
            // to the joint M_w marginalization.  Card 1d-SH: VtecSh terms
            // take the same route (GM border ≠ the κ update's RW model).
            if (c >= 0 && (tuReinitSince(cs, lc.last_seen_s)
                           || (carry_cpl_shmarg
                               && cs.kind == SharedParamKind::VtecSh)))
                c = -1;
            // READMIT re-coupling edge (network.readmit_coupling_marg_ep): a column
            // re-registered after a gap is still settling for K epochs — same route.
            if (c >= 0 && readmitDiverted(cs)) c = -1;
            if (c < 0) {                          // DEPARTED → M_w column
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
                    // different P_prev for the same departed column; take the
                    // larger (conservative) so the shared marginalization is
                    // never optimistic.
                    if (cs.p_prev > white_pw[static_cast<std::size_t>(widx)])
                        white_pw[static_cast<std::size_t>(widx)] = cs.p_prev;
                }
                raw.departed.emplace_back(widx, cs.coeff);
                ++R.cpl_terms_departed;
                if (carry_cpl_gauge && !cs.sat.empty()
                    && cs.kind != SharedParamKind::VtecSh)
                    trunc[{cs.sat[0], static_cast<int>(cs.kind)}] += cs.coeff;
                continue;
            }
            ++R.cpl_terms_present;
            // Card 1d: shrink the PRESENT column by κ_j and inflate this
            // row's own diagonal by M²·κ_j·q_j·Δt — identical to the scalar
            // path below; only the departed-column treatment differs (M_w).
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
                                     cs.sat.empty() ? '\0' : cs.sat[0],
                                     cs.kind});
                continue;
            }
            if (buffer_all) {   // Card 1f: unshrunk, but routed through the buffer
                ents.push_back(TuEnt{c, cs.coeff, cs.coeff, cs.s_prev,
                                     cs.sat.empty() ? '\0' : cs.sat[0],
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
        if (!(raw.var_eff > 0.0) || !std::isfinite(rhs)) return;  // never poison
        raw.rhs = rhs;
        craw.push_back(std::move(raw));
        return;
    }
    // Card 1 scalar path (unchanged — byte-identical when carry_cpl_joint off).  The
    // loop body lives in scalarRowIngredients so the F3 block path shares it
    // verbatim; the operations and their order are the pre-F3 inline sequence.
    PendCplRow row;
    double sigma2 = 0.0;
    if (!scalarRowIngredients(col, x_lin_val, lc, var_eff, gap, row, sigma2)) return;
    row.w = 1.0 / sigma2;
    crows.push_back(std::move(row));
}

bool CouplingRowBuilder::scalarRowIngredients(Eigen::Index col, double x_lin_val,
                                              const LocalCarry& lc, double var_eff,
                                              double gap, PendCplRow& row, double& sigma2) {
    row.local_col = col;
    double rhs = lc.value_abs - x_lin_val;   // (r̂_prev − r_lin)
    double dep_var = 0.0;                    // Σ_departed M_rj²·P_prev,jj
    double tu_var  = 0.0;                    // Card 1d: Σ_present M_rj²·κ_j·q_j·Δt
    std::vector<TuEnt> ents;                 // Card 1d: buffered present terms
    const bool buffer_all = carry_cpl_tu || carry_cpl_gauge;   // Card 1f (see the joint path)
    std::map<std::pair<char, int>, double> trunc;              // Card 1f: (cc,kind) → Σ truncated coeff
    // Card 1f: BOTH truncation branches below feed this — a column dropped because it
    // left the layout and a column forced onto the departed route by a reinit/frame
    // step are the same perturbation of the coefficient vector.
    const auto note_trunc = [&](const CoupledShared& cs) {
        if (carry_cpl_gauge && !cs.sat.empty() && cs.kind != SharedParamKind::VtecSh)
            trunc[{cs.sat[0], static_cast<int>(cs.kind)}] += cs.coeff;
    };
    for (const CoupledShared& cs : lc.coupling) {
        const Eigen::Index c = resolveSharedCol(cs);
        if (c < 0) {                          // DEPARTED → marginalize into σ²
            dep_var += cs.coeff * cs.coeff * cs.p_prev;
            ++R.cpl_terms_departed;
            note_trunc(cs);
            continue;
        }
        // READMIT re-coupling edge (network.readmit_coupling_marg_ep): the returned
        // satellite's columns take the departed route for K epochs (see the joint path).
        if (readmitDiverted(cs)) {
            dep_var += cs.coeff * cs.coeff * cs.p_prev;
            ++R.cpl_terms_departed;
            note_trunc(cs);
            continue;
        }
        // Card 1d: a (kind, sat) column reinit-folded since this row's
        // harvest is the q=∞ limit of the time update: fully marginalize
        // (κ=0), mirroring the dense fold's decorrelation rule.  Card 1d-SH:
        // VtecSh terms take the same treatment (GM border ≠ RW κ model).
        if (tuReinitSince(cs, lc.last_seen_s)
            || (carry_cpl_shmarg && cs.kind == SharedParamKind::VtecSh)) {
            dep_var += cs.coeff * cs.coeff * cs.p_prev;
            ++R.cpl_terms_departed;
            note_trunc(cs);
            continue;
        }
        ++R.cpl_terms_present;
        if (carry_cpl_tu) {
            // Card 1d: the row observed u = r + M_rᵀs at HARVEST; the border
            // has since moved s⁺ = s + w, w~N(0,Q).  Exact re-independence
            // against s⁺: shrink M' = κ·M_r (coefficient AND its ŝ_prev RHS
            // term) and inflate σ² by M²·κ·q·Δt = Mᵀ(P⁻¹+Q⁻¹)⁻¹M per column.
            const double qj = couplingQ(cs) * gap;
            double m = cs.coeff;
            if (qj > 0.0 && cs.p_prev > 0.0) {
                const double kap = cs.p_prev / (cs.p_prev + qj);
                tu_var += cs.coeff * cs.coeff * kap * qj;
                m = kap * cs.coeff;
                ++R.cpl_terms_shrunk;
            }
            ents.push_back(TuEnt{c, m, cs.coeff, cs.s_prev,
                                 cs.sat.empty() ? '\0' : cs.sat[0],
                                 cs.kind});
            continue;
        }
        if (buffer_all) {   // Card 1f: unshrunk, but routed through the buffer
            ents.push_back(TuEnt{c, cs.coeff, cs.coeff, cs.s_prev,
                                 cs.sat.empty() ? '\0' : cs.sat[0],
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
    sigma2 = var_eff + dep_var + tu_var;
    if (!(sigma2 > 0.0) || !std::isfinite(rhs)) return false;  // never poison the row
    row.y = rhs;
    return true;
}

bool CouplingRowBuilder::buildLocalBlock(const std::vector<LocalBlockCol>& cols) {
    // F3 (network.phase_leveling_local_block, SPEC_amb_iono_split_overcount).
    //
    // (1) Per column, the SCALAR-path ingredients — exactly the row buildCplRow
    //     would have emitted for it: +1/local, the present shared terms (Card-1d
    //     κ-shrunk, Card-1f gauge-restored), the RHS, and
    //     σ² = var_eff + Σ_departed M²P_prev + Σ_present M²κqΔt.  The departed
    //     terms stay per-row (Σ's diagonal) — the scalar-path treatment; the joint
    //     departed marginalization is Card 1b's and is mutually exclusive with this
    //     block (the caller's predicate).  A column the scalar screen refuses is
    //     dropped from the block and counted, never poisoned.
    struct Ing { PendCplRow row; double sigma2{0.0}; const LocalBlockCol* c{nullptr}; };
    std::vector<Ing> ing;
    ing.reserve(cols.size());
    for (const LocalBlockCol& c : cols) {
        Ing g;
        g.c = &c;
        if (!scalarRowIngredients(c.col, c.x_lin, *c.lc, c.var_eff, c.gap, g.row, g.sigma2)) {
            ++R.pl_blk_col_refused;
            continue;
        }
        // B9-in-block: the ZWD column is counted only once it has SURVIVED the scalar
        // screen, i.e. once it is really a row of this block — the caller cannot know
        // that, and a column counted at push time would report a refused ZWD as carried.
        if (c.is_zwd) ++R.pl_blk_zwd_cols;
        // I-in-block: same rule, same reason — a per-LOS iono column counts once it is
        // really a row of this block, never at push time.
        if (c.is_iono) ++R.pl_blk_iono_cols;
        ing.push_back(std::move(g));
    }
    const Eigen::Index n = static_cast<Eigen::Index>(ing.size());
    if (n == 0) return true;   // nothing to emit — the per-column route would emit nothing either
    // (2) Σ: the diagonal from (1); the off-diagonals the HARVESTED conditional
    //     cross covariances (N_rr⁻¹)_{c_i c_j}, valid only when BOTH entries were
    //     stashed at the same harvest (pointer-equal PlHarvestBlock — the same
    //     posterior, the same epoch).  Otherwise the pair is ZEROED and counted:
    //     that makes Σ block-diagonal by harvest stamp, each block a principal
    //     sub-matrix of one posterior, so PD is preserved by construction rather
    //     than by luck.  The M2 v2 VCE factor α² is already in var_eff (the
    //     diagonal), so the cross term scales by √(α_i²·α_j²) — a congruence with a
    //     diagonal matrix, PD-preserving; the var floor was applied to the diagonal
    //     only (raising a diagonal is PD-preserving too).  The q·gap term is on the
    //     diagonal only (a per-column random walk has no cross term).
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
                if (!std::isfinite(v)) v = li.blk->C(lj.blk_idx, li.blk_idx);   // n_{c_j}(c_i) — the transpose read
            }
            if (!std::isfinite(v)) {
                ++R.pl_blk_pair_missing;
                // B9-in-block: split the ZWD's own missing pairs out of the aggregate.
                // A ZWD stashed WITHOUT a harvest block (its harvest threw, or the pl
                // loop never built one for this station) makes EVERY pair with it miss —
                // that is exactly the shape a "carried ZWD that is not really coupled"
                // has, and it is invisible inside pl_blk_pair_missing.
                if (ci.is_zwd || cj.is_zwd) ++R.pl_blk_zwd_missing_cross;
                // I-in-block: the iono columns' own share, split out for the same reason.
                // An iono entry stashed WITHOUT a harvest block (its per-column harvest
                // threw, or the station built no block that epoch) makes EVERY pair with
                // it miss — the shape a "carried iono that is not actually coupled to
                // anything" has, invisible inside the aggregate.  A pair that is BOTH
                // (iono, zwd) counts in both splits by design: neither counter claims to
                // partition pl_blk_pair_missing.
                if (ci.is_iono || cj.is_iono) ++R.pl_blk_iono_missing_cross;
                continue;
            }
            if (ci.vce_fac != 1.0 || cj.vce_fac != 1.0) v *= std::sqrt(ci.vce_fac * cj.vce_fac);
            blk.Sigma(i, j) = v;
            blk.Sigma(j, i) = v;
        }
    }
    // (2b) AGE-AWARE young-arc inflate (network.phase_leveling_young_inflate_var_m2,
    //      SPEC_amb_iono_split_overcount §6.17–§6.18): a column whose LAST RE-LEVEL is
    //      recent holds a CODE-quality level at the PHASE-stiff variance the conditional
    //      hands back (it is conditioned on the station's MATURE arcs), so the T4 tier
    //      erases an arc that is merely UNREFINED and the erase–re-level treadmill runs.
    //      The correction is a CONGRUENCE Σ ← DΣD with D = diag(f_j),
    //      f_j = √((Σ_jj + var_add_j)/Σ_jj) ≥ 1 — applied HERE, after the diagonal and
    //      the harvested cross terms are both in place and BEFORE the whitening, which
    //      is the only point at which "this column's row and column" is a well-defined
    //      object.  Three properties, and the reason it is a congruence and not a
    //      diagonal addition: the target diagonal is inflated EXACTLY (f_j²Σ_jj =
    //      Σ_jj + a_j); PD is preserved by Sylvester's law of inertia (D is diagonal,
    //      strictly positive — the scalar screen above already guaranteed Σ_jj > 0); and
    //      every correlation coefficient is UNCHANGED, because f_jf_kΣ_jk divided by
    //      √(f_j²Σ_jj·f_k²Σ_kk) is ρ_jk.  A freshly re-levelled arc is not LESS coupled
    //      to its station's other columns — it is worth less on its own.  OFF (every
    //      var_add 0) writes nothing to Σ, so the block is byte-identical.
    {
        std::vector<double> yng_add(static_cast<std::size_t>(n), 0.0);
        bool any_yng = false;
        for (Eigen::Index i = 0; i < n; ++i) {
            const double a = ing[static_cast<std::size_t>(i)].c->var_add;
            if (a > 0.0) { yng_add[static_cast<std::size_t>(i)] = a; any_yng = true; }
        }
        if (any_yng)
            R.pl_blk_yng_scaled += static_cast<long long>(
                NetworkYoungInflate::congruence(blk.Sigma, yng_add));
    }
    // (3) Whiten: Σ = LLᵀ (ridge retry inside), rows L⁻¹·[I | M | y] at unit weight.
    //     A non-SPD factor after the retry buffers NOTHING — the caller emits the
    //     per-column rows instead and counts the fallback (invariant #4).
    Eigen::MatrixXd Hr, Hs;
    Eigen::VectorXd z;
    std::vector<Eigen::Index> kept;
    if (!whitenLocalBlock(blk, /*skip=*/{}, R.A_r.cols(), R.A_s.cols(), Hr, Hs, z, kept)
        || static_cast<Eigen::Index>(kept.size()) != n)
        return false;
    // (4) Buffer the n whitened rows as multi-local PendCplRow entries.  L⁻¹ is lower
    //     triangular, so row k has local entries on block columns 0..k (all pushed —
    //     the diagonal 1/L_kk > 0 guarantees a non-empty `local`, which is what
    //     distinguishes a block row from a scalar +1 row at append).  local_col names
    //     the row's PRIMARY column (block column k) for cpl_row_local_cols.
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
        // Card 1b: JOINTLY whiten this station's coupling rows and append them
        // as unit-information rows (mirroring appendShIonoRows).  Retain the
        // pre-whitening ingredients so a within-epoch RAIM refloat can
        // re-whiten the surviving rows.  R.carry_cols stays EMPTY (the coupling
        // lives in R.A_s/R.A_r, not the addStationPrior diagonal).
        if (!craw.empty()) {
            Eigen::MatrixXd Hr, Hs;
            Eigen::VectorXd z;
            const bool ok = whitenCouplingRows(craw, white_pw, /*skip=*/{},
                                               R.A_r.cols(), R.A_s.cols(), Hr, Hs, z);
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
            if (ok && Hr.rows() == add) {
                R.A_s.bottomRows(add) = Hs;
                R.A_r.bottomRows(add) = Hr;
                R.w.segment(m0, add).setOnes();         // unit-information whitened rows
                R.y.segment(m0, add) = z;
                R.cpl_raw      = std::move(craw);        // for the refloat re-whiten
                R.cpl_white_pw = std::move(white_pw);
            } else {
                // Non-SPD joint covariance after the ridge retry → fall back to
                // the Card-1 per-row scalar marginalization for THIS station this
                // epoch (dep_var = Σ_departed M_rj²·P_w = the joint diagonal), and
                // leave cpl_raw EMPTY so the refloat takes the scalar zero-row path.
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
        // Card 1: batch-append the coupling rows to this station's block
        // (one conservativeResize, mirroring appendShIonoRows).  R.carry_cols
        // is left EMPTY so the serial consume's addStationPrior is skipped —
        // the coupling now lives entirely in R.A_s/R.A_r consumed by addStation.
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
                else   // F3 whitened block row: one row of L⁻¹ over the block's local columns
                    for (const auto& lcoef : row.local)
                        R.A_r(m0 + k, lcoef.first) = lcoef.second;
                for (const auto& sc : row.shared)
                    R.A_s(m0 + k, sc.first) = sc.second;
                R.w(m0 + k) = row.w;
                R.y(m0 + k) = row.y;
                R.cpl_row_local_cols[static_cast<size_t>(k)] = row.local_col;
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
    // Card 1/1d observability: fold the per-station coupling-term counts into the
    // run totals (SERIAL — the parallel produce above only writes its own slot).
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
        // Orphan ATTRIBUTION (multi-constellation owed item): one line per epoch that
        // produced any — names the (constellation,kind) families, counts and |mass|,
        // so the run log carries the per-epoch attribution the run-total cannot.
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
    // Card 1d debug dump: the first few ASSEMBLED coupling rows of the run
    // (weight, RHS, A_s sparsity/magnitude) — the ground truth of what addStation
    // actually receives, for A/B-arm comparison.  One-shot budget; log-only.
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
                // Exact-vanishing probe: a coupling row on a local column with ZERO
                // data rows is absorbed COMPLETELY by the free local in the Schur
                // elimination (adds nothing to the reduced border, any weight).
                const Eigen::Index lc = Rc.cpl_row_local_cols[static_cast<size_t>(r)];
                const Eigen::Index data_rows_on_col =
                    (Rc.A_r.col(lc).head(Rc.cpl_row_start).array() != 0.0).count();
                // Gauge diagnostic: Σ of the row's A_s coefficients over each
                // constellation's {clk, pb, cb} columns — ≈0 for a gauge-invariant
                // row (the structural null direction has weight +1 on each).
                std::map<char, double> gsum;
                for (const auto& [col, d] : cpl_col2desc_dbg)
                    if (d.kind != SharedParamKind::VtecSh && col < Rc.A_s.cols()
                        && !d.sat.empty())
                        gsum[d.sat[0]] += Rc.A_s(row, col);
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

long long rebaseCouplingPhaseBiasSprev(
    std::vector<CoupledShared>& coupling,
    const std::map<std::pair<std::string, std::string>, double>& shift_sat) {
    if (shift_sat.empty() || coupling.empty()) return 0;
    long long n = 0;
    for (CoupledShared& cs : coupling) {
        if (cs.kind != SharedParamKind::PhaseBias) continue;
        const auto bs = shift_sat.find({cs.sat, cs.signal});
        if (bs != shift_sat.end() && std::isfinite(bs->second)
            && bs->second != 0.0) {
            cs.s_prev += bs->second;   // same sign as the prev_carry walk's cp.value += s
            ++n;
        }
    }
    return n;
}

} // namespace GnssProcessor

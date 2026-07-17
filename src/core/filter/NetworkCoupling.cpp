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

#include "filter/NetworkEstimator.hpp"   // est.clockColumn / phaseBiasColumn / codeBiasColumn / vtecCoeffColumn
#include "config/GnssConfigTypes.hpp"    // GlobalVtecShConfig::q_per_degree (couplingQ)
#include "filter/StateKeySchema.hpp"     // StateKeys::parseSatPhaseBiasKey (Card-1d dump)

#include <spdlog/spdlog.h>               // Card-1d debug dump (log->info)
#include <fmt/format.h>                  // Card-1d dump gauge-sum formatting

#include <cmath>
#include <map>
#include <memory>
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
            case SharedParamKind::VtecSh:    return est.vtecCoeffColumn(cs.n, cs.m, cs.trig);
        }
    } catch (...) {}
    return -1;  // departed (unregistered this epoch)
}

std::string CouplingRowBuilder::couplingId(const CoupledShared& cs) {
    switch (cs.kind) {
        case SharedParamKind::Clock:     return "C|" + cs.sat;
        case SharedParamKind::PhaseBias: return "P|" + cs.sat + "|" + cs.signal;
        case SharedParamKind::CodeBias:  return "B|" + cs.sat + "|" + cs.signal;
        case SharedParamKind::VtecSh:    return "V|" + std::to_string(cs.n) + "|"
                                                + std::to_string(cs.m) + "|" + cs.trig;
    }
    return {};
}

double CouplingRowBuilder::couplingQ(const CoupledShared& cs) const {
    switch (cs.kind) {
        case SharedParamKind::Clock:     return q_clock;
        case SharedParamKind::CodeBias:  return q_cb;
        case SharedParamKind::PhaseBias: return q_pb;
        case SharedParamKind::VtecSh:    return (cs.n == 0)
            ? shc.q_per_degree
            : shc.q_per_degree / static_cast<double>(cs.n * cs.n);
    }
    return 0.0;
}

void CouplingRowBuilder::tuGaugeRestore(std::vector<TuEnt>& ents) {
    std::map<std::pair<char, int>, std::pair<double, int>> deficit;
    for (const TuEnt& e : ents)
        if (e.cc && e.kind != SharedParamKind::VtecSh) {
            auto& d = deficit[{e.cc, static_cast<int>(e.kind)}];
            d.first += e.m_un - e.m_shr;
            ++d.second;
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
    if (!carry_cpl_tu) return false;
    const auto it = tu_reinit_hist.find({cs.kind, cs.sat});
    return it != tu_reinit_hist.end() && it->second > since_s;
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
            raw.shared.emplace_back(c, cs.coeff);
            rhs += cs.coeff * cs.s_prev;          // + M_rj·ŝ_prev,j
        }
        if (carry_cpl_tu) {
            tuGaugeRestore(ents);   // shrink only gauge-orthogonally
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
    // Card 1 scalar path (unchanged — byte-identical when carry_cpl_joint off).
    PendCplRow row;
    row.local_col = col;
    double rhs = lc.value_abs - x_lin_val;   // (r̂_prev − r_lin)
    double dep_var = 0.0;                    // Σ_departed M_rj²·P_prev,jj
    double tu_var  = 0.0;                    // Card 1d: Σ_present M_rj²·κ_j·q_j·Δt
    std::vector<TuEnt> ents;                 // Card 1d: buffered present terms
    for (const CoupledShared& cs : lc.coupling) {
        const Eigen::Index c = resolveSharedCol(cs);
        if (c < 0) {                          // DEPARTED → marginalize into σ²
            dep_var += cs.coeff * cs.coeff * cs.p_prev;
            ++R.cpl_terms_departed;
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
        row.shared.emplace_back(c, cs.coeff);
        rhs += cs.coeff * cs.s_prev;          // + M_rj·ŝ_prev,j
    }
    if (carry_cpl_tu) {
        tuGaugeRestore(ents);   // shrink only gauge-orthogonally
        for (const TuEnt& e : ents) {
            row.shared.emplace_back(e.c, e.m_shr);
            rhs += e.m_shr * e.s_prev;
        }
    }
    const double sigma2 = var_eff + dep_var + tu_var;
    if (!(sigma2 > 0.0) || !std::isfinite(rhs)) return;  // never poison the row
    row.y = rhs;
    row.w = 1.0 / sigma2;
    crows.push_back(std::move(row));
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
                R.A_r(m0 + k, row.local_col) = 1.0;
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
                            int& n_cpl_row_dump) {
    // Card 1/1d observability: fold the per-station coupling-term counts into the
    // run totals (SERIAL — the parallel produce above only writes its own slot).
    if (carry_cpl_cond)
        for (const StationAssembly& Rc : asm_res) {
            n_cpl_terms_present  += Rc.cpl_terms_present;
            n_cpl_terms_departed += Rc.cpl_terms_departed;
            n_cpl_terms_shrunk   += Rc.cpl_terms_shrunk;
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

} // namespace GnssProcessor

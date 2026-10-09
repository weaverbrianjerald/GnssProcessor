/**
 * @file NetworkRhsAttrib.cpp
 * @brief In-assembly reduced-RHS class attribution (see NetworkRhsAttrib.hpp).
 *
 * Exactness argument, per station r and row class c: the kernel's reduced RHS is
 *   ũ_s = u_s − Σ_r N_sr N_rr⁻¹ u_r  (+ DD-fold corrections),
 * and every u vector is a sum over folded rows and priors.  With the epoch's
 * final N_sr / N_rr (all classes' information present), the map u ↦ ũ_s is
 * linear, so splitting u by class and pushing each part through the same
 * reduction is an exact decomposition:  Σ_c ũ_s^c ≡ ũ_s.  The DD folds are
 * recomputed per input block with the algebra the kernel uses
 * (G·M·(z_free − ẑ)).  The datum pins act on the solve's copy of the reduced
 * system, so they are reported as a separate section and excluded from the
 * kernel-level closure.
 *
 * Card NOM step N2b: the decomposition iterates exactly the channels the kernel folds
 * (@ref networkRhsAttribDecompose): a station's rows, its pre-PD prior (folded with the
 * block, the channel the attribution had missed: card NOM section 16, suspect 2) and its
 * post-PD diagonal prior when the consume folded it, then every captured shared fold
 * and the DD folds.  Rows are classed by their tags, station priors and shared folds by
 * their kinds' names, so the classes sum to the kernel's reduced RHS by construction.
 *
 * Known caveat, detected by the closure row: a within-epoch RAIM replay
 * rebuilds the kernel from modified row stacks after the shared-info captures
 * ran, and the closure row then reports the mismatch.  That RAIM tier is
 * carry_warm-gated, so it is inert with phase leveling off.
 *
 * Structure (SPEC_prefit_census_observability INC-0 / INC-5):
 *   - the row classifier lives in filter/NetworkRowClassify (it reads the rows' tags),
 *     one classifier shared with the census, the fuse and the topology ledger;
 *   - the decomposition runs off a @ref NetworkAttribFrame, so one code path
 *     serves the live epoch and a retained previous epoch.  The live path stores
 *     pointers to the caller's assemblies and copies only the kernel blocks.
 */

#include "filter/NetworkRhsAttrib.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>

#include <fmt/format.h>

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkRowClassify.hpp"
#include "logging/NetworkRunTraces.hpp"

namespace GnssProcessor {

namespace {

/// One (class, station) or (class, station, sat) attribution record.
struct AttribRow {
    std::string cls;
    std::string sta;      ///< empty for class totals
    std::string sat;      ///< empty unless a per-sat sub-row
    double c00_l1{0.0};
    double c00_l2{0.0};
    double norm{0.0};     ///< 2-norm of the full reduced contribution
};

/// 2-norm of @p v over the shared columns of family @p want, for the dumped
/// per-family norms.  (@ref NetColFam is the enum of the classifier in
/// NetworkRowClassify; the dump's column order sh/pb/cb/clk is part of the CSV
/// contract.)
double famNorm(const NetworkAttribFrame& f, const std::vector<NetColFam>& fam,
               const Eigen::VectorXd& v, NetColFam want) {
    double s = 0.0;
    for (Eigen::Index c = 0; c < f.n; ++c)
        if (fam[static_cast<std::size_t>(c)] == want) s += v(c) * v(c);
    return std::sqrt(s);
}

} // namespace

std::size_t NetworkAttribFrame::bytes() const {
    std::size_t b = 0;
    auto mat = [](const Eigen::MatrixXd& m) {
        return static_cast<std::size_t>(m.size()) * sizeof(double);
    };
    auto vec = [](const Eigen::VectorXd& v) {
        return static_cast<std::size_t>(v.size()) * sizeof(double);
    };
    for (const auto& m : Nsr) b += mat(m);
    for (const auto& m : Nrr) b += mat(m);
    for (const auto& v : ur)  b += vec(v);
    for (const auto& R : asm_own) {
        b += mat(R.A_s) + mat(R.A_r) + vec(R.w) + vec(R.y)
           + vec(R.carry_dNrr) + vec(R.carry_dur);
    }
    for (const auto& c : captures) b += vec(c.delta_rhs);
    for (const auto& blk : dd_blocks) b += mat(blk.Pz) + vec(blk.zhat);
    b += mat(N_red) + vec(b_red) + vec(x_s);
    return b;
}

void NetworkAttribFrame::clear() {
    *this = NetworkAttribFrame{};
}

void networkAttribCaptureFrame(NetworkAttribFrame& out, const SyncedEpoch& se,
                               const std::vector<NetStation>& stations,
                               const NetworkEstimator& est,
                               const NetworkEstimator::Datum& plan_datum,
                               const std::vector<StationAssembly>& asm_res,
                               const std::vector<int>& est_index_of,
                               bool own_assemblies,
                               const Eigen::VectorXd* x_s) {
    out.clear();
    out.week = se.time.week;
    out.tow  = se.time.tow;

    const auto& kernel = est.normals();
    const Eigen::Index n = est.numShared();
    if (n <= 0 || !kernel.isReduced()) return;
    out.n = n;

    const SharedColumnMap cm = buildSharedColumnMap(est, n);
    out.label   = cm.label;
    out.fam     = cm.fam;
    out.col_sat = cm.col_sat;
    out.c00_l1  = cm.c00_l1;
    out.c00_l2 = cm.c00_l2;

    // Admission predicate: folded into the kernel this epoch, block built,
    // dimensions agree.  It appears twice; keep the two copies textually
    // identical.  The second pass fills asm_ptr and must select the same
    // stations in the same order, or a station would be paired with another
    // station's kernel blocks.  It is written twice rather than hoisted because
    // asm_own reallocates while the first pass fills it, so the pointers cannot
    // be taken until it is complete.
    for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i) {
        if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
        const StationAssembly& R = asm_res[i];
        if (!R.ok || R.A_s.cols() != n || R.y.size() == 0) continue;
        const std::size_t slot = static_cast<std::size_t>(est_index_of[i]);
        out.sta_name.push_back(stations[i].name);
        out.slot.push_back(slot);
        out.Nsr.push_back(kernel.stationCrossBlock(slot));
        out.Nrr.push_back(kernel.stationLocalNormals(slot));
        out.ur.push_back(kernel.stationLocalRhs(slot));
        if (own_assemblies) out.asm_own.push_back(R);
    }
    // Pointers are taken last because asm_own may reallocate while it is filled.
    out.asm_ptr.reserve(out.sta_name.size());
    if (own_assemblies) {
        for (const StationAssembly& R : out.asm_own) out.asm_ptr.push_back(&R);
    } else {
        for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i) {
            if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
            const StationAssembly& R = asm_res[i];
            if (!R.ok || R.A_s.cols() != n || R.y.size() == 0) continue;
            out.asm_ptr.push_back(&R);
        }
    }

    out.captures  = est.attribCaptures();
    out.dd_blocks = est.ddPriorInputs();
    out.b_red     = kernel.reducedSharedRhs();
    out.N_red     = kernel.reducedSharedNormals();
    out.datum     = plan_datum;
    if (x_s && x_s->size() == n) out.x_s = *x_s;
    out.valid     = true;
}

std::string rhsAttribSharedClass(ContributionKind k) {
    return std::string("shared:") + contributionKindName(k);
}

NetworkRhsAttribDecomposition networkRhsAttribDecompose(const NetworkAttribFrame& f) {
    NetworkRhsAttribDecomposition out;
    if (!f.valid || f.n <= 0) return out;
    const Eigen::Index n = f.n;

    auto addTotal = [&](const std::string& k, const Eigen::VectorXd& v) {
        auto it = out.class_total.find(k);
        if (it == out.class_total.end()) out.class_total.emplace(k, v);
        else it->second += v;
    };

    // Per-slot cached local factorizations (reused by the DD recompute below).
    std::map<std::size_t, Eigen::LDLT<Eigen::MatrixXd>> slot_ldlt;
    std::map<std::size_t, Eigen::VectorXd> slot_nrr_inv_ur;
    std::map<std::size_t, std::size_t> slot_item;   // kernel slot -> frame item
    // Per-slot local-col -> "sat:signal" (from pl_amb), for DD-block labels.
    std::map<std::size_t, std::map<Eigen::Index, std::string>> slot_amb_label;

    // ---- pass 1: each station's channels, as the kernel folds them ----------
    // A station's u_s and u_r are the sum of three channels (NetworkNormalEquations): the
    // block's rows (addStation), the pre-PD station prior folded with the block
    // (addStation's StationPrior, network.station_prior_before_pd), and the post-PD
    // diagonal prior (addStationPrior), which the consume folds only when it sets
    // carry_prior_folded.  Each row is classed by its tag (classifyRow) and each prior
    // entry by its tag's kind (contributionKindName), so every term the kernel folded is
    // attributed once and the closure holds by construction (card NOM step N2b).
    for (std::size_t k = 0; k < f.asm_ptr.size(); ++k) {
        const StationAssembly& R = *f.asm_ptr[k];
        const std::size_t slot = f.slot[k];

        const Eigen::MatrixXd& Nsr = f.Nsr[k];
        Eigen::LDLT<Eigen::MatrixXd>& ldlt = slot_ldlt[slot];
        ldlt.compute(f.Nrr[k]);
        slot_nrr_inv_ur[slot] = ldlt.solve(f.ur[k]);
        slot_item[slot] = k;
        auto& amb_lbl = slot_amb_label[slot];
        for (const auto& pa : R.pl_amb) amb_lbl[pa.col] = pa.sat + ":" + pa.signal;

        // Class accumulators (u_s part, u_r part) for this station.
        std::map<std::string, std::pair<Eigen::VectorXd, Eigen::VectorXd>> per;
        auto acc = [&](const std::string& cls) -> auto& {
            auto it = per.find(cls);
            if (it == per.end())
                it = per.emplace(cls, std::make_pair(
                         Eigen::VectorXd::Zero(n).eval(),
                         Eigen::VectorXd::Zero(R.A_r.cols()).eval())).first;
            return it->second;
        };
        for (Eigen::Index row = 0; row < R.y.size(); ++row) {
            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            const double wy = w * R.y(row);
            if (!std::isfinite(wy)) continue;
            auto& a = acc(rowClassName(classifyRow(R, row, nullptr)));
            a.first.noalias()  += R.A_s.row(row).transpose() * wy;
            a.second.noalias() += R.A_r.row(row).transpose() * wy;
        }
        // A diagonal prior channel: RHS increments on local columns, one class per kind.
        // An entry the coalesce merged into another entry of the same column folded inside
        // the surviving entry, so its share is in that entry's class (the contributions
        // table counts the merged entries on their own line).
        const auto addPrior = [&](const std::vector<Eigen::Index>& cols,
                                  const Eigen::VectorXd& dur,
                                  const std::vector<RowTag>& tags) {
            for (std::size_t j = 0; j < cols.size()
                                    && j < static_cast<std::size_t>(dur.size()); ++j) {
                const Eigen::Index col = cols[j];
                if (col < 0 || col >= R.A_r.cols()) continue;
                const ContributionKind kind =
                    (j < tags.size()) ? tags[j].kind : ContributionKind::Unset;
                acc(contributionKindName(kind)).second(col) +=
                    dur(static_cast<Eigen::Index>(j));
            }
        };
        addPrior(R.prior_cols, R.prior_dur, R.prior_tags);   // pre-PD, with the block
        if (R.carry_prior_folded)                            // post-PD, when folded
            addPrior(R.carry_cols, R.carry_dur, R.carry_tags);
        // Reduce each class part through the station's final blocks.
        for (auto& [cls, uu] : per) {
            Eigen::VectorXd contrib = uu.first;
            if (uu.second.size() > 0)
                contrib.noalias() -= Nsr * ldlt.solve(uu.second);
            addTotal(cls, contrib);
            out.station_parts.push_back({cls, f.sta_name[k], contrib});
        }
    }

    // ---- pass 2: shared-info captures, one class per kind -------------------
    for (const auto& cap : f.captures) {
        if (cap.delta_rhs.size() != n) continue;
        addTotal(rhsAttribSharedClass(cap.kind), cap.delta_rhs);
    }

    // ---- pass 3: DD-fold corrections, recomputed per input block ------------
    const auto& blocks = f.dd_blocks;
    Eigen::VectorXd dd_total = Eigen::VectorXd::Zero(n);
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        const auto& blk = blocks[b];
        const Eigen::Index K = static_cast<Eigen::Index>(blk.rows.size());
        if (K == 0) continue;
        std::map<std::size_t, std::vector<std::pair<Eigen::Index, std::pair<Eigen::Index, double>>>> by_sta;
        for (Eigen::Index k = 0; k < K; ++k)
            for (const auto& t : blk.rows[static_cast<std::size_t>(k)])
                by_sta[t.station].push_back({k, {t.local_col, t.coeff}});
        Eigen::MatrixXd B = Eigen::MatrixXd::Zero(K, K);
        Eigen::MatrixXd G = Eigen::MatrixXd::Zero(n, K);
        Eigen::VectorXd z_free = Eigen::VectorXd::Zero(K);
        std::string blk_label;
        int label_terms = 0;
        bool ok = true;
        for (const auto& [si, terms] : by_sta) {
            auto lit = slot_ldlt.find(si);
            auto iit = slot_item.find(si);
            if (lit == slot_ldlt.end() || iit == slot_item.end()) { ok = false; break; }
            const Eigen::Index nl = f.Nrr[iit->second].rows();
            Eigen::MatrixXd CrT = Eigen::MatrixXd::Zero(nl, K);
            for (const auto& [k, ct] : terms) {
                if (ct.first < 0 || ct.first >= nl) { ok = false; break; }
                CrT(ct.first, k) += ct.second;
                if (label_terms < 3) {
                    const auto& al = slot_amb_label[si];
                    const auto ait = al.find(ct.first);
                    if (!blk_label.empty()) blk_label += "|";
                    blk_label += (ait != al.end()) ? ait->second
                                                   : ("loc" + std::to_string(ct.first));
                    ++label_terms;
                }
            }
            if (!ok) break;
            const Eigen::MatrixXd Y = lit->second.solve(CrT);
            B.noalias()      += CrT.transpose() * Y;
            G.noalias()      += f.Nsr[iit->second] * Y;
            z_free.noalias() += CrT.transpose() * slot_nrr_inv_ur[si];
        }
        if (!ok) continue;
        Eigen::MatrixXd PzB = blk.Pz + B;
        PzB = (0.5 * (PzB + PzB.transpose())).eval();
        Eigen::LDLT<Eigen::MatrixXd> ldlt(PzB);
        if (ldlt.info() != Eigen::Success || ldlt.vectorD().minCoeff() <= 0.0) continue;
        const Eigen::VectorXd delta = G * ldlt.solve(z_free - blk.zhat);
        dd_total += delta;
        out.dd_parts.push_back({b, blk_label, delta});
    }
    if (dd_total.size() == n && !blocks.empty()) addTotal("dd_fold", dd_total);

    // ---- closure (kernel-level; datum pins excluded by construction) --------
    Eigen::VectorXd sum = Eigen::VectorXd::Zero(n);
    for (const auto& [cls, v] : out.class_total) sum += v;
    const Eigen::VectorXd& b_red = f.b_red;
    out.closure_abs = (sum - b_red).cwiseAbs().maxCoeff();
    const double b_norm = std::max(1.0, b_red.norm());
    out.closure_rel = (sum - b_red).norm() / b_norm;
    out.valid = true;
    return out;
}

NetworkRhsColumnShares networkRhsAttribColumnShares(const NetworkAttribFrame& f,
                                                    const NetworkRhsAttribDecomposition& dec,
                                                    const std::vector<std::string>& labels) {
    NetworkRhsColumnShares out;
    out.labels = labels;
    const Eigen::Index J = static_cast<Eigen::Index>(labels.size());
    const Eigen::VectorXd zeroJ = Eigen::VectorXd::Zero(J);
    out.col.assign(labels.size(), -1);
    out.pinned.assign(labels.size(), 0);
    out.rhs_sum = out.sol_sum = out.b_red = out.x_full = zeroJ;
    out.x_solve = Eigen::VectorXd::Constant(J, std::numeric_limits<double>::quiet_NaN());
    out.rhs_closure_abs = out.rhs_closure_rel = zeroJ;
    out.sol_closure_abs = out.sol_closure_rel = zeroJ;
    if (!f.valid || f.n <= 0 || f.N_red.rows() != f.n || f.b_red.size() != f.n) return out;
    const Eigen::Index n = f.n;

    // ---- the named columns ------------------------------------------------------
    for (Eigen::Index j = 0; j < J; ++j) {
        const auto it = std::find(f.label.begin(), f.label.end(),
                                  labels[static_cast<std::size_t>(j)]);
        if (it != f.label.end())
            out.col[static_cast<std::size_t>(j)] = static_cast<Eigen::Index>(it - f.label.begin());
    }
    std::vector<char> is_pinned(static_cast<std::size_t>(n), 0);
    for (const auto& p : f.datum)
        if (p.column >= 0 && p.column < n) is_pinned[static_cast<std::size_t>(p.column)] = 1;
    for (std::size_t j = 0; j < labels.size(); ++j)
        if (out.col[j] >= 0) out.pinned[j] = is_pinned[static_cast<std::size_t>(out.col[j])];

    // ---- the solve's operator: NetworkEstimator::solve's Dirichlet pins on N_red --
    Eigen::MatrixXd Nc = f.N_red;
    Eigen::VectorXd uc = f.b_red;
    Eigen::VectorXd pin_total = Eigen::VectorXd::Zero(n);   // the datum's RHS move
    for (const auto& p : f.datum) {
        if (p.column < 0 || p.column >= n) continue;
        pin_total -= f.N_red.col(p.column) * p.value;
    }
    const double pin_scale =
        Nc.size() > 0 ? std::max(1.0, Nc.diagonal().cwiseAbs().maxCoeff()) : 1.0;
    for (const auto& p : f.datum) {   // the solve's own order of operations
        if (p.column < 0 || p.column >= n) continue;
        uc -= f.N_red.col(p.column) * p.value;
    }
    for (const auto& p : f.datum) {
        if (p.column < 0 || p.column >= n) continue;
        Nc.row(p.column).setZero();
        Nc.col(p.column).setZero();
    }
    for (const auto& p : f.datum) {
        if (p.column < 0 || p.column >= n) continue;
        Nc(p.column, p.column) = pin_scale;
        uc(p.column) = pin_scale * p.value;
    }
    const Eigen::LDLT<Eigen::MatrixXd> ldlt_c(Nc);
    if (ldlt_c.info() != Eigen::Success) return out;
    const Eigen::VectorXd x_c = ldlt_c.solve(uc);

    // g_j = N_c^-1 e_c: the row of the inverse that maps a reduced-RHS part to the
    // solution at column c (symmetric).  Its pinned entries are zero by the block
    // structure and are zeroed explicitly; at a pinned named column g_j is all zero.
    Eigen::MatrixXd G = Eigen::MatrixXd::Zero(n, J);
    for (Eigen::Index j = 0; j < J; ++j) {
        const Eigen::Index c = out.col[static_cast<std::size_t>(j)];
        if (c < 0) continue;
        out.b_red(j)  = f.b_red(c);
        out.x_full(j) = x_c(c);
        if (f.x_s.size() == n) out.x_solve(j) = f.x_s(c);
        if (out.pinned[static_cast<std::size_t>(j)]) continue;
        G.col(j) = ldlt_c.solve(Eigen::VectorXd::Unit(n, c));
        for (Eigen::Index q = 0; q < n; ++q)
            if (is_pinned[static_cast<std::size_t>(q)]) G(q, j) = 0.0;
    }
    // The load of a full reduced-RHS vector at every named column.
    const auto loadOf = [&](const Eigen::VectorXd& v, Eigen::VectorXd& rhs,
                            Eigen::VectorXd& sol) {
        rhs = zeroJ;
        for (Eigen::Index j = 0; j < J; ++j) {
            const Eigen::Index c = out.col[static_cast<std::size_t>(j)];
            if (c >= 0) rhs(j) = v(c);
        }
        sol = G.transpose() * v;
    };
    const auto newPart = [&](const char* record, const std::string& sta,
                             const std::string& kind, const std::string& sat,
                             const std::string& sig) {
        NetworkRhsColumnShares::Part p;
        p.record = record; p.sta = sta; p.kind = kind; p.sat = sat; p.sig = sig;
        p.rhs = zeroJ;
        p.sol = zeroJ;
        return p;
    };

    // ---- pass 1: each station's channels, by kind --------------------------------
    // R_loc(:, j) = N_rr^-1 N_sr^T e_c reads the reduced RHS at c of a local-RHS part;
    // T_loc(:, j) = N_rr^-1 N_sr^T g_j reads its solution share.  A row adds w y times
    // (A_s[c] - A_r R_loc) and (A_s g - A_r T_loc); a prior entry on local column l adds
    // -du R_loc(l) and -du T_loc(l).  Same channels as networkRhsAttribDecompose.
    std::map<std::string, NetworkRhsColumnShares::Part> kind_tot;
    for (std::size_t k = 0; k < f.asm_ptr.size(); ++k) {
        const StationAssembly& R = *f.asm_ptr[k];
        const std::string& sname = f.sta_name[k];
        const Eigen::MatrixXd& Nsr = f.Nsr[k];
        const Eigen::Index nr = R.A_r.cols();
        Eigen::MatrixXd R_loc = Eigen::MatrixXd::Zero(nr, J);
        Eigen::MatrixXd T_loc = Eigen::MatrixXd::Zero(nr, J);
        if (nr > 0 && f.Nrr[k].rows() == nr && Nsr.cols() == nr) {
            const Eigen::LDLT<Eigen::MatrixXd> ldlt(f.Nrr[k]);
            Eigen::MatrixXd Ec = Eigen::MatrixXd::Zero(n, J);
            for (Eigen::Index j = 0; j < J; ++j) {
                const Eigen::Index c = out.col[static_cast<std::size_t>(j)];
                if (c >= 0) Ec(c, j) = 1.0;
            }
            R_loc = ldlt.solve(Nsr.transpose() * Ec);
            T_loc = ldlt.solve(Nsr.transpose() * G);
        }
        std::map<std::string, NetworkRhsColumnShares::Part> per_kind;
        std::map<std::tuple<std::string, std::string, std::string>,
                 NetworkRhsColumnShares::Part> per_los;
        const auto add = [&](const RowTag& t, const Eigen::VectorXd& rhs,
                             const Eigen::VectorXd& sol) {
            const std::string kname = contributionKindName(t.kind);
            const std::string sat = t.sat.valid() ? t.sat.str() : std::string("?");
            const std::string sig =
                (t.signal.sys != System::Unknown) ? t.signal.str() : std::string();
            auto ik = per_kind.find(kname);
            if (ik == per_kind.end())
                ik = per_kind.emplace(kname, newPart("STAKIND", sname, kname, "", "")).first;
            ik->second.rhs += rhs; ik->second.sol += sol; ++ik->second.n;
            const auto key = std::make_tuple(kname, sat, sig);
            auto il = per_los.find(key);
            if (il == per_los.end())
                il = per_los.emplace(key, newPart("STALOS", sname, kname, sat, sig)).first;
            il->second.rhs += rhs; il->second.sol += sol; ++il->second.n;
        };
        Eigen::VectorXd rhs(J), sol(J);
        for (Eigen::Index row = 0; row < R.y.size(); ++row) {
            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            const double wy = w * R.y(row);
            if (!std::isfinite(wy)) continue;
            for (Eigen::Index j = 0; j < J; ++j) {
                const Eigen::Index c = out.col[static_cast<std::size_t>(j)];
                const double as_c = (c >= 0) ? R.A_s(row, c) : 0.0;
                const double ar_r = (nr > 0) ? R.A_r.row(row).dot(R_loc.col(j)) : 0.0;
                const double ar_t = (nr > 0) ? R.A_r.row(row).dot(T_loc.col(j)) : 0.0;
                rhs(j) = wy * (as_c - ar_r);
                sol(j) = wy * (R.A_s.row(row).dot(G.col(j)) - ar_t);
            }
            add(rowTagAt(R, row), rhs, sol);
        }
        const auto addPrior = [&](const std::vector<Eigen::Index>& cols,
                                  const Eigen::VectorXd& dur,
                                  const std::vector<RowTag>& tags) {
            for (std::size_t e = 0; e < cols.size()
                                    && e < static_cast<std::size_t>(dur.size()); ++e) {
                const Eigen::Index l = cols[e];
                if (l < 0 || l >= nr) continue;
                const double du = dur(static_cast<Eigen::Index>(e));
                for (Eigen::Index j = 0; j < J; ++j) {
                    rhs(j) = -du * R_loc(l, j);
                    sol(j) = -du * T_loc(l, j);
                }
                add((e < tags.size()) ? tags[e] : RowTag{}, rhs, sol);
            }
        };
        addPrior(R.prior_cols, R.prior_dur, R.prior_tags);   // pre-PD, with the block
        if (R.carry_prior_folded)                            // post-PD, when folded
            addPrior(R.carry_cols, R.carry_dur, R.carry_tags);
        for (auto& [kname, p] : per_kind) {
            auto it = kind_tot.find(kname);
            if (it == kind_tot.end())
                it = kind_tot.emplace(kname, newPart("KIND", "", kname, "", "")).first;
            it->second.rhs += p.rhs; it->second.sol += p.sol; it->second.n += p.n;
            out.rhs_sum += p.rhs;
            out.sol_sum += p.sol;
            out.station_kind.push_back(std::move(p));
        }
        for (auto& [key, p] : per_los) out.station_los.push_back(std::move(p));
    }
    for (auto& [kname, p] : kind_tot) out.kind_total.push_back(std::move(p));

    // ---- pass 2: the folds: shared captures by class, the DD folds, the datum ------
    std::map<std::string, NetworkRhsColumnShares::Part> shared;
    for (const auto& cap : f.captures) {
        if (cap.delta_rhs.size() != n) continue;
        const std::string cls = rhsAttribSharedClass(cap.kind);
        auto it = shared.find(cls);
        if (it == shared.end())
            it = shared.emplace(cls, newPart("SHARED", "", cls, "", "")).first;
        Eigen::VectorXd rhs, sol;
        loadOf(cap.delta_rhs, rhs, sol);
        it->second.rhs += rhs; it->second.sol += sol; ++it->second.n;
    }
    for (auto& [cls, p] : shared) out.folds.push_back(std::move(p));
    if (!dec.dd_parts.empty()) {
        NetworkRhsColumnShares::Part p = newPart("DDFOLD", "", "dd_fold", "", "");
        for (const auto& dd : dec.dd_parts) {
            if (dd.v.size() != n) continue;
            Eigen::VectorXd rhs, sol;
            loadOf(dd.v, rhs, sol);
            p.rhs += rhs; p.sol += sol; ++p.n;
        }
        out.folds.push_back(std::move(p));
    }
    for (const auto& p : out.folds) {
        out.rhs_sum += p.rhs;
        out.sol_sum += p.sol;
    }
    {
        NetworkRhsColumnShares::Part p = newPart("DATUMPIN", "", "datum_pin", "", "");
        for (const auto& pin : f.datum)
            if (pin.column >= 0 && pin.column < n) ++p.n;
        loadOf(pin_total, p.rhs, p.sol);
        // At a pinned named column the solve sets x_c to the pin value: the datum's share.
        for (const auto& pin : f.datum)
            for (Eigen::Index j = 0; j < J; ++j)
                if (out.col[static_cast<std::size_t>(j)] == pin.column) p.sol(j) = pin.value;
        out.sol_sum += p.sol;
        out.folds.push_back(std::move(p));
    }
    // KIND: the stations' kinds by name, then each fold as itself.
    for (const auto& p : out.folds) {
        NetworkRhsColumnShares::Part k = p;
        k.record = "KIND";
        out.kind_total.push_back(std::move(k));
    }

    // ---- closure ------------------------------------------------------------------
    const auto rel = [](double a, double b) {
        return (a == 0.0) ? 0.0 : a / std::max(std::abs(b), std::numeric_limits<double>::min());
    };
    for (Eigen::Index j = 0; j < J; ++j) {
        if (out.col[static_cast<std::size_t>(j)] < 0) continue;
        out.rhs_closure_abs(j) = std::abs(out.rhs_sum(j) - out.b_red(j));
        out.rhs_closure_rel(j) = rel(out.rhs_closure_abs(j), out.b_red(j));
        out.sol_closure_abs(j) = std::abs(out.sol_sum(j) - out.x_full(j));
        out.sol_closure_rel(j) = rel(out.sol_closure_abs(j), out.x_full(j));
    }
    out.valid = true;
    return out;
}

void networkRhsAttribDump(const NetworkAttribFrame& f,
                          const std::shared_ptr<spdlog::logger>& log,
                          const std::filesystem::path& dir, const char* kind,
                          const std::vector<std::string>& columns) {
    if (!f.valid || f.n <= 0) {
        log->warn("Network (rhs-attrib): {} frame at tow {:.1f} has no reduced "
                  "system — skipped", kind, f.tow);
        return;
    }
    const Eigen::Index n = f.n;
    const Eigen::Index c00_l1 = f.c00_l1, c00_l2 = f.c00_l2;

    // ---- the decomposition: passes 1 to 3 and the closure ---------------------
    const NetworkRhsAttribDecomposition dec = networkRhsAttribDecompose(f);
    const std::map<std::string, Eigen::VectorXd>& cls_total = dec.class_total;
    const double closure_abs = dec.closure_abs;
    const double closure_rel = dec.closure_rel;
    const Eigen::VectorXd& b_red = f.b_red;
    std::vector<AttribRow> rows_out;
    auto pushRow = [&](const std::string& cls, const std::string& sta,
                       const std::string& sat, const Eigen::VectorXd& v) {
        AttribRow r;
        r.cls = cls; r.sta = sta; r.sat = sat;
        r.c00_l1 = (c00_l1 >= 0) ? v(c00_l1) : 0.0;
        r.c00_l2 = (c00_l2 >= 0) ? v(c00_l2) : 0.0;
        r.norm = v.norm();
        rows_out.push_back(std::move(r));
    };
    for (const auto& p : dec.station_parts) pushRow(p.cls, p.sta, "", p.v);

    if (closure_rel > 1e-4) {
        log->warn("Network (rhs-attrib): tow {:.1f} closure VIOLATED (rel {:.3e}, "
                  "max-abs {:.3e}) — attribution untrusted this epoch (within-epoch "
                  "re-solve or unattributed injection)", f.tow, closure_rel,
                  closure_abs);
    }

    // ---- datum pins (solve-level; separate section) -------------------------
    const Eigen::MatrixXd& Nred = f.N_red;
    Eigen::VectorXd pin_total = Eigen::VectorXd::Zero(n);
    std::vector<std::tuple<std::string, double, Eigen::VectorXd>> pin_rows;
    for (const auto& p : f.datum) {
        if (p.column < 0 || p.column >= n) continue;
        Eigen::VectorXd v = -Nred.col(p.column) * p.value;
        pin_total += v;
        pin_rows.emplace_back(f.label[static_cast<std::size_t>(p.column)], p.value,
                              std::move(v));
    }

    // ---- per-sat second pass over the top loaded stations -------------------
    std::map<std::string, double> sta_load;   // station -> |c00 l1|+|c00 l2| over obs classes
    for (const auto& r : rows_out)
        if (!r.sta.empty() && (r.cls.rfind("obs_", 0) == 0 || r.cls == "iono_sh_row"))
            sta_load[r.sta] += std::abs(r.c00_l1) + std::abs(r.c00_l2);
    std::vector<std::pair<double, std::string>> ranked;
    for (const auto& [s, v] : sta_load) ranked.push_back({v, s});
    std::sort(ranked.rbegin(), ranked.rend());
    std::set<std::string> top_sta;
    for (std::size_t k = 0; k < ranked.size() && k < 8; ++k)
        top_sta.insert(ranked[k].second);
    for (std::size_t k = 0; k < f.asm_ptr.size(); ++k) {
        const StationAssembly& R = *f.asm_ptr[k];
        if (!top_sta.count(f.sta_name[k])) continue;
        const Eigen::MatrixXd& Nsr = f.Nsr[k];
        // The factorization the decomposition used (deterministic on the same N_rr).
        const Eigen::LDLT<Eigen::MatrixXd> ldlt(f.Nrr[k]);
        std::map<std::string, std::pair<Eigen::VectorXd, Eigen::VectorXd>> per;
        for (Eigen::Index row = 0; row < R.y.size(); ++row) {
            std::string sat;
            const NetRowClass rc = classifyRow(R, row, &sat);
            if (rc != NetRowClass::ObsCode && rc != NetRowClass::ObsPhase
                && rc != NetRowClass::ObsOther && rc != NetRowClass::IonoShRow)
                continue;
            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            const double wy = w * R.y(row);
            if (!std::isfinite(wy)) continue;
            const std::string key = std::string(rowClassName(rc)) + "\x1f"
                                  + (sat.empty() ? "?" : sat);
            auto it = per.find(key);
            if (it == per.end())
                it = per.emplace(key, std::make_pair(
                         Eigen::VectorXd::Zero(n).eval(),
                         Eigen::VectorXd::Zero(R.A_r.cols()).eval())).first;
            it->second.first.noalias()  += R.A_s.row(row).transpose() * wy;
            it->second.second.noalias() += R.A_r.row(row).transpose() * wy;
        }
        for (auto& [key, uu] : per) {
            const std::size_t sep = key.find('\x1f');
            Eigen::VectorXd contrib = uu.first;
            contrib.noalias() -= Nsr * ldlt.solve(uu.second);
            pushRow(key.substr(0, sep), f.sta_name[k], key.substr(sep + 1), contrib);
        }
    }

    // ---- per-row forensic dump, every station -------------------------------
    // w, y and h_c00 are written separately so a weight change and an
    // innovation change can be told apart; the class/station sums cannot
    // separate them.  About 3–4k rows per armed epoch; diagnostics-only.
    // The last column is the row's contribution kind (card ICF instrument A'): the
    // class lumps kinds (cpl_row holds the coupling rows and all six carried-prior
    // block kinds), the kind names the one the row's builder declared.  It is
    // appended last so the columns before it keep their positions.
    std::vector<std::string> row_dump;
    for (std::size_t k = 0; k < f.asm_ptr.size(); ++k) {
        const StationAssembly& R = *f.asm_ptr[k];
        const std::string& sname = f.sta_name[k];
        for (Eigen::Index row = 0; row < R.y.size(); ++row) {
            std::string sat;
            const char* cls = rowClassName(classifyRow(R, row, &sat));
            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            const double h1 = (c00_l1 >= 0) ? R.A_s(row, c00_l1) : 0.0;
            const double h2 = (c00_l2 >= 0) ? R.A_s(row, c00_l2) : 0.0;
            row_dump.push_back(fmt::format(
                "{},{},{},{},{:.6e},{:.6e},{:.6e},{:.6e},{:.6e},{}", sname, row,
                cls, sat.empty() ? "?" : sat, w, R.y(row), h1, h2,
                w * R.y(row) * h1, contributionKindName(rowTagAt(R, row).kind)));
        }
    }

    // ---- dump ---------------------------------------------------------------
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string tow_tag = fmt::format("{:.0f}", f.tow);
    {
        std::ofstream o(dir / ("attrib_" + tow_tag + ".csv"), std::ios::trunc);
        o << "week,tow,kind,class,station,sat,c00_l1,c00_l2,norm_sh,norm_pb,"
             "norm_cb,norm_clk,norm_total,extra\n";
        auto emit = [&](const char* row_kind, const std::string& cls,
                        const std::string& sta, const std::string& sat,
                        const Eigen::VectorXd& v, const std::string& extra) {
            o << f.week << ',' << fmt::format("{:.3f}", f.tow) << ','
              << row_kind << ',' << cls << ',' << sta << ',' << sat << ','
              << fmt::format("{:.6f}", (c00_l1 >= 0) ? v(c00_l1) : 0.0) << ','
              << fmt::format("{:.6f}", (c00_l2 >= 0) ? v(c00_l2) : 0.0) << ','
              << fmt::format("{:.4f}", famNorm(f, f.fam, v, NetColFam::Sh)) << ','
              << fmt::format("{:.4f}", famNorm(f, f.fam, v, NetColFam::Pb)) << ','
              << fmt::format("{:.4f}", famNorm(f, f.fam, v, NetColFam::Cb)) << ','
              << fmt::format("{:.4f}", famNorm(f, f.fam, v, NetColFam::Clk)) << ','
              << fmt::format("{:.4f}", v.norm()) << ',' << extra << '\n';
        };
        for (const auto& [cls, v] : cls_total) emit("TOTAL", cls, "", "", v, "");
        for (const auto& r : rows_out) {
            // Station/sat rows hold only scalars, so they are written directly
            // rather than through emit(): the family-norm fields stay empty and
            // the total norm comes from r.norm.
            o << f.week << ',' << fmt::format("{:.3f}", f.tow) << ','
              << (r.sat.empty() ? "STA" : "STASAT") << ',' << r.cls << ','
              << r.sta << ',' << r.sat << ','
              << fmt::format("{:.6f}", r.c00_l1) << ','
              << fmt::format("{:.6f}", r.c00_l2) << ",,,,,"
              << fmt::format("{:.4f}", r.norm) << ",\n";
        }
        for (const auto& dd : dec.dd_parts)
            emit("DDBLOCK", "dd_fold", "", "", dd.v,
                 "block=" + std::to_string(dd.block) + ";" + dd.label);
        for (const auto& [lbl, val, v] : pin_rows)
            emit("DATUMPIN", "datum_pin", "", "", v,
                 lbl + "=" + fmt::format("{:.6f}", val));
        emit("PINTOTAL", "datum_pin", "", "", pin_total, "");
        o << f.week << ',' << fmt::format("{:.3f}", f.tow)
          << ",CLOSURE,,,," << fmt::format("{:.6e}", closure_abs) << ','
          << fmt::format("{:.6e}", closure_rel) << ",,,,,"
          << fmt::format("{:.4f}", b_red.norm()) << ",\n";
    }
    if (!row_dump.empty()) {
        std::ofstream o(dir / ("rows_" + tow_tag + ".csv"), std::ios::trunc);
        o << "station,row,class,sat,w,y,h_c00_l1,h_c00_l2,w_y_h1,kind\n";
        for (const auto& s : row_dump) o << s << '\n';
    }
    {
        std::ofstream o(dir / ("vec_" + tow_tag + ".csv"), std::ios::trunc);
        o << "class,col,label,value\n";
        auto dumpVec = [&](const std::string& cls, const Eigen::VectorXd& v) {
            for (Eigen::Index c = 0; c < n; ++c)
                o << cls << ',' << c << ',' << f.label[static_cast<std::size_t>(c)]
                  << ',' << fmt::format("{:.9e}", v(c)) << '\n';
        };
        for (const auto& [cls, v] : cls_total) dumpVec(cls, v);
        dumpVec("datum_pin", pin_total);
        dumpVec("kernel_b_red", b_red);
    }

    log->info("Network (rhs-attrib): [{}] tow {:.1f} dumped {} classes / {} station "
              "rows / {} DD blocks / {} pins to {} (closure rel {:.2e}; C00 "
              "cols {}/{})",
              kind, f.tow, cls_total.size(), rows_out.size(), dec.dd_parts.size(),
              pin_rows.size(), (dir / ("attrib_" + tow_tag + ".csv")).string(),
              closure_rel, c00_l1, c00_l2);

    // ---- solution shares at the named columns (card ICF instrument A') ----------
    if (columns.empty()) return;
    const NetworkRhsColumnShares sh = networkRhsAttribColumnShares(f, dec, columns);
    if (!sh.valid) {
        log->warn("Network (rhs-attrib): [{}] tow {:.1f}: the datum-constrained reduced "
                  "system did not factorize; cols_{}.csv not written", kind, f.tow, tow_tag);
        return;
    }
    std::ofstream o(dir / ("cols_" + tow_tag + ".csv"), std::ios::trunc);
    o << "week,tow,record,station,kind,sat,signal,n";
    for (const auto& lbl : sh.labels) o << ",rhs[" << lbl << "],sol[" << lbl << "]";
    o << '\n';
    const std::string head = fmt::format("{},{:.3f},", f.week, f.tow);
    const auto num = [](double v) { return fmt::format("{:.9e}", v); };
    const auto line = [&](const char* record, const std::string& sta,
                          const std::string& kname, const std::string& sat,
                          const std::string& sig, const std::string& nterms,
                          const Eigen::VectorXd* rhs, const Eigen::VectorXd* sol) {
        o << head << record << ',' << sta << ',' << kname << ',' << sat << ',' << sig
          << ',' << nterms;
        for (std::size_t j = 0; j < sh.labels.size(); ++j) {
            const bool present = sh.col[j] >= 0;
            o << ',' << (present && rhs ? num((*rhs)(static_cast<Eigen::Index>(j))) : "")
              << ',' << (present && sol ? num((*sol)(static_cast<Eigen::Index>(j))) : "");
        }
        o << '\n';
    };
    {   // COLUMN: the column index and whether the datum pins it.
        o << head << "COLUMN,,,,,";
        for (std::size_t j = 0; j < sh.labels.size(); ++j) {
            if (sh.col[j] >= 0) o << ',' << sh.col[j] << ',' << int(sh.pinned[j]);
            else                o << ",,";
        }
        o << '\n';
    }
    const auto part = [&](const NetworkRhsColumnShares::Part& p) {
        line(p.record.c_str(), p.sta, p.kind, p.sat, p.sig, std::to_string(p.n), &p.rhs,
             &p.sol);
    };
    for (const auto& p : sh.station_kind) part(p);
    for (const auto& p : sh.station_los)  part(p);
    for (const auto& p : sh.folds)        part(p);
    for (const auto& p : sh.kind_total)   part(p);
    line("TOTAL", "", "", "", "", "", &sh.rhs_sum, &sh.sol_sum);
    line("FULL", "", "", "", "", "", &sh.b_red, &sh.x_full);
    line("SOLVE", "", "", "", "", "", nullptr, f.x_s.size() == n ? &sh.x_solve : nullptr);
    line("CLOSURE_ABS", "", "", "", "", "", &sh.rhs_closure_abs, &sh.sol_closure_abs);
    line("CLOSURE_REL", "", "", "", "", "", &sh.rhs_closure_rel, &sh.sol_closure_rel);

    // The run log names each column's solution, its closure and the solve check.
    for (std::size_t j = 0; j < sh.labels.size(); ++j) {
        const Eigen::Index jj = static_cast<Eigen::Index>(j);
        if (sh.col[j] < 0) {
            log->warn("Network (rhs-attrib): [{}] tow {:.1f}: named column '{}' is not a "
                      "shared column this epoch", kind, f.tow, sh.labels[j]);
            continue;
        }
        log->info("Network (rhs-attrib): [{}] tow {:.1f} {}{}: x {:.6f} (solve {:.6f}), "
                  "solution-share closure abs {:.3e} rel {:.3e}, rhs closure rel {:.3e}",
                  kind, f.tow, sh.labels[j], sh.pinned[j] ? " (pinned)" : "",
                  sh.x_full(jj), sh.x_solve(jj), sh.sol_closure_abs(jj),
                  sh.sol_closure_rel(jj), sh.rhs_closure_rel(jj));
    }
}

void networkRhsAttribution(NetworkRunContext& ctx, const GnssConfig& config_,
                           const std::shared_ptr<spdlog::logger>& log,
                           const NetworkRunTraces& traces, const SyncedEpoch& se,
                           const std::vector<NetStation>& stations,
                           NetworkEstimator& est,
                           const NetworkEstimator::Datum& plan_datum,
                           const std::vector<StationAssembly>& asm_res,
                           const std::vector<int>& est_index_of,
                           const Eigen::VectorXd* x_s) {
    const auto& nc = config_.getNetworkConfig();
    if (nc.rhs_attrib_tow < 0.0) return;
    if (ctx.carry.rhs_attrib_done >= std::max(1, nc.rhs_attrib_n_ep)) return;
    if (se.time.tow < nc.rhs_attrib_tow - 0.25) return;
    ++ctx.carry.rhs_attrib_done;

    NetworkAttribFrame f;
    networkAttribCaptureFrame(f, se, stations, est, plan_datum, asm_res,
                              est_index_of, /*own_assemblies=*/false, x_s);
    networkRhsAttribDump(f, log, traces.log_dir_ / "rhs_attrib", "armed",
                         config_.networkRhsAttribColumns());
    ++ctx.counters.pc.rec_dumps;
}

bool networkRhsTripEdge(NetworkRunContext& ctx, const char* kind, bool active) {
    if (!ctx.knobs.rec_on) return false;
    ++ctx.counters.pc.rec_consults;
    auto& eras = ctx.carry.rhs_trip_eras;
    const std::string key(kind ? kind : "?");
    const bool was = eras.count(key) != 0;
    if (active && !was) {
        eras.insert(key);
        ++ctx.counters.pc.rec_trips;
        // First writer wins: the first era to trip in an epoch names the dump.
        if (ctx.carry.rhs_trip_pending.empty()) ctx.carry.rhs_trip_pending = key;
        return true;
    }
    if (!active && was) eras.erase(key);
    return false;
}

void networkRhsAttribTrip(NetworkRunContext& ctx, const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log,
                          const NetworkRunTraces& traces, const SyncedEpoch& se,
                          const std::vector<NetStation>& stations,
                          const NetworkEstimator& est,
                          const NetworkEstimator::Datum& plan_datum,
                          const std::vector<StationAssembly>& asm_res,
                          const std::vector<int>& est_index_of) {
    if (!ctx.knobs.rec_on) return;
    const auto& nc = config_.getNetworkConfig();
    auto& ctr = ctx.counters.pc;
    const std::filesystem::path dir = traces.log_dir_ / "rhs_attrib";

    // --- 1. consume a pending trip -------------------------------------------
    if (!ctx.carry.rhs_trip_pending.empty()) {
        const std::string why = ctx.carry.rhs_trip_pending;
        ctx.carry.rhs_trip_pending.clear();
        NetworkAttribFrame now;
        networkAttribCaptureFrame(now, se, stations, est, plan_datum, asm_res,
                                  est_index_of, /*own_assemblies=*/false);
        networkRhsAttribDump(now, log, dir, ("trip:" + why).c_str(),
                             config_.networkRhsAttribColumns());
        ++ctr.rec_dumps;
        if (nc.rhs_attrib_on_trip_prev) {
            if (ctx.carry.rhs_prev_frame && ctx.carry.rhs_prev_frame->valid) {
                networkRhsAttribDump(*ctx.carry.rhs_prev_frame, log, dir,
                                     ("trip-prev:" + why).c_str(),
                                     config_.networkRhsAttribColumns());
                ++ctr.rec_prev_dumps;
            } else {
                log->warn("Network (rhs-attrib INC-5): trip '{}' at TOW {:.1f} has NO "
                          "retained previous frame — the pre-trip frame is the "
                          "informative one, so this dump answers only 'what does the "
                          "aftermath look like'.  (First processed epoch, a withheld "
                          "previous epoch, or retention auto-disabled above "
                          "network.rhs_attrib_prev_max_stations = {}.)",
                          why, se.time.tow, nc.rhs_attrib_prev_max_stations);
            }
        }
    }

    // --- 2. retain this epoch for the next one -------------------------------
    if (!nc.rhs_attrib_on_trip_prev) return;
    if (static_cast<int>(stations.size()) > std::max(0, nc.rhs_attrib_prev_max_stations)) {
        if (!ctx.carry.rhs_prev_oversize_logged) {
            ctx.carry.rhs_prev_oversize_logged = true;
            log->warn("Network (rhs-attrib INC-5): {} stations exceeds "
                      "network.rhs_attrib_prev_max_stations = {} — PREVIOUS-epoch "
                      "retention is DISABLED for this run (a trip still dumps the "
                      "current epoch).  Raise the cap only with the memory budget in "
                      "hand: retention is order 100 MB at 63 stations.",
                      stations.size(), nc.rhs_attrib_prev_max_stations);
        }
        if (ctx.carry.rhs_prev_frame) ctx.carry.rhs_prev_frame.reset();
        return;
    }
    if (!ctx.carry.rhs_prev_frame)
        ctx.carry.rhs_prev_frame = std::make_unique<NetworkAttribFrame>();
    networkAttribCaptureFrame(*ctx.carry.rhs_prev_frame, se, stations, est, plan_datum,
                              asm_res, est_index_of, /*own_assemblies=*/true);
    if (ctx.carry.rhs_prev_frame->valid) {
        ++ctr.rec_frames_retained;
        ctr.rec_prev_bytes_last =
            static_cast<long long>(ctx.carry.rhs_prev_frame->bytes());
    }
}

} // namespace GnssProcessor

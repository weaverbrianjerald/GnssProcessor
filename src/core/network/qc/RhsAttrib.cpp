/**
 * @file RhsAttrib.cpp
 * @brief In-assembly reduced-RHS class attribution (see RhsAttrib.hpp).
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
 * The decomposition iterates exactly the channels the kernel folds
 * (@ref networkRhsAttribDecompose): a station's rows, its pre-PD prior (folded with the
 * block, the channel the attribution had missed) and its
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

#include "network/qc/RhsAttrib.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>

#include <fmt/format.h>

#include "network/state/RunContext.hpp"
#include "network/solve/NormalEquations.hpp"
#include "network/assembly/RowClassify.hpp"
#include "network/diag/RunTraces.hpp"
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"

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
    for (const auto& p : local_parts)
        b += vec(p.free) + vec(p.feedback) + vec(p.sumzero) + vec(p.ddprior) + vec(p.x_r);
    for (const auto& names : local_names)
        for (const auto& s : names) b += s.capacity();
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
                               const Eigen::VectorXd* x_s,
                               bool local_readout) {
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
    std::vector<std::size_t> roster;   // each item's station index, for the local readout below
    for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i) {
        if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
        const StationAssembly& R = asm_res[i];
        if (!R.ok || R.A_s.cols() != n || R.y.size() == 0) continue;
        const std::size_t slot = static_cast<std::size_t>(est_index_of[i]);
        roster.push_back(i);
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
    // The station-local readout's inputs, read now because the kernel and the
    // station parameter sets move on after the epoch: each item's local-column names and the
    // kernel's reconstruction of its locals at x_s, terms apart.  Only when asked (a LOC:
    // label is named) and with x_s, so every other capture, the flight recorder's
    // per-epoch retention among them, does no extra work.
    if (local_readout && out.x_s.size() == n) {
        out.local_names.reserve(roster.size());
        out.local_parts.reserve(roster.size());
        for (std::size_t k = 0; k < roster.size(); ++k) {
            const std::size_t i = roster[k];
            const StationAssembly& R = asm_res[i];
            out.local_names.push_back(
                rhsAttribLocalColumnNames(R, stations[i].params));
            NetworkNormalEquations::StationReconstructionParts parts;
            // reconstructStationParts throws std::out_of_range / std::invalid_argument on a
            // bad slot or width (both std::logic_error); such an item keeps empty parts and
            // the readout reports it unresolved.  Anything else propagates.
            try { parts = kernel.reconstructStationParts(out.slot[k], out.x_s); }
            catch (const std::logic_error&) { parts = {}; }
            out.local_parts.push_back(std::move(parts));
        }
    }
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
    // attributed once and the closure holds by construction.
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

namespace {

/// The solve's datum-constrained operator, rebuilt from a frame: the kernel's reduced
/// shared normals with the epoch's minimum-constraint pins applied by the Dirichlet
/// elimination @c NetworkEstimator::solve uses (see @ref NetworkRhsColumnShares).
struct ConstrainedOperator {
    bool                         ok{false};  ///< The constrained normals factorized.
    Eigen::LDLT<Eigen::MatrixXd> ldlt;       ///< The factor of @f$N_c@f$.
    Eigen::VectorXd              x_c;        ///< The constrained solution @f$N_c^{-1}u_c@f$.
    Eigen::VectorXd              uc;         ///< The constrained right-hand side @f$u_c@f$ (the station-local re-solve check reuses it).
    double                       pin_scale{1.0};  ///< The pinned diagonal @f$s@f$ of @f$N_c@f$.
    Eigen::VectorXd              pin_total;  ///< The datum's RHS move @f$-\sum_p \tilde N_{:,p} v_p@f$.
    std::vector<char>            is_pinned;  ///< Per shared column, 1 where the datum pins it.
};

/**
 * @brief Rebuild the solve's datum-constrained operator from a frame.
 * @param f A frame with a reduced system (@c N_red and @c b_red of width @c n).
 * @return The operator; @c ok false when the constrained normals do not factorize.
 */
ConstrainedOperator constrainedOperator(const NetworkAttribFrame& f) {
    ConstrainedOperator op;
    const Eigen::Index n = f.n;
    op.is_pinned.assign(static_cast<std::size_t>(n), 0);
    for (const auto& p : f.datum)
        if (p.column >= 0 && p.column < n) op.is_pinned[static_cast<std::size_t>(p.column)] = 1;

    // ---- the solve's operator: NetworkEstimator::solve's Dirichlet pins on N_red --
    Eigen::MatrixXd Nc = f.N_red;
    Eigen::VectorXd uc = f.b_red;
    op.pin_total = Eigen::VectorXd::Zero(n);   // the datum's RHS move
    for (const auto& p : f.datum) {
        if (p.column < 0 || p.column >= n) continue;
        op.pin_total -= f.N_red.col(p.column) * p.value;
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
    op.pin_scale = pin_scale;
    op.uc = uc;
    op.ldlt.compute(Nc);
    if (op.ldlt.info() != Eigen::Success) return op;
    op.x_c = op.ldlt.solve(uc);
    op.ok = true;
    return op;
}

/**
 * @brief @f$g = N_c^{-1}h@f$ for a functional @f$h@f$ of the shared solution: it maps a
 *        reduced-RHS part to its share of @f$h^\top x_c@f$ (@f$N_c@f$ is symmetric).
 *
 * The pinned entries of @f$g@f$ are zero by the block structure of @f$N_c@f$ and are zeroed
 * explicitly; a functional on pinned columns only has @f$g = 0@f$ (the datum alone sets it).
 *
 * @param op The constrained operator (@c ok).
 * @param h  The functional (length @c n).
 * @return @f$g@f$ (length @c n).
 */
Eigen::VectorXd functionalGain(const ConstrainedOperator& op, const Eigen::VectorXd& h) {
    const Eigen::Index n = h.size();
    bool free_support = false;
    for (Eigen::Index q = 0; q < n && !free_support; ++q)
        free_support = h(q) != 0.0 && !op.is_pinned[static_cast<std::size_t>(q)];
    if (!free_support) return Eigen::VectorXd::Zero(n);
    Eigen::VectorXd g = op.ldlt.solve(h);
    for (Eigen::Index q = 0; q < n; ++q)
        if (op.is_pinned[static_cast<std::size_t>(q)]) g(q) = 0.0;
    return g;
}

/**
 * @brief The reduced-RHS load and the solution share of every part at a set of linear
 *        functionals of the shared solution (the core of @ref networkRhsAttribColumnShares,
 *        which passes unit functionals, and of the station-local readout's feedback split).
 *
 * For a functional @f$h@f$ (a column of @p H) with @f$g = N_c^{-1}h@f$ (its pinned entries
 * zeroed, and @f$g=0@f$ when @f$h@f$ lives on pinned columns only), a reduced-RHS part
 * @f$\tilde u^{(k)}@f$ loads @f$h^\top\tilde u^{(k)}@f$ and has the solution share
 * @f$g^\top\tilde u^{(k)}@f$; the datum's share is @f$g^\top(-\tilde N_{:,P}v_P) +
 * h_P^\top v_P@f$.  The shares sum to @f$h^\top x_c@f$.  The station parts are formed per
 * row as in @ref NetworkRhsColumnShares.
 *
 * @param f        The frame.
 * @param dec      Its class decomposition (the DD parts are reused).
 * @param op       Its constrained operator (@c ok).
 * @param H        The functionals, @c n × J.
 * @param unit_col Optional: per functional, the shared column of a unit functional (-1 for
 *                 a zero one).  When given, a load at the column is read by index, as the
 *                 named-column readout has always read it; nullptr reads it as a dot product.
 * @param out      [in,out] @c station_kind, @c station_los, @c folds, @c kind_total,
 *                 @c rhs_sum and @c sol_sum are filled (the sums must be zero on entry).
 * @param gains    Optional: the gains @f$g_j@f$ (@c n × J) already formed by the caller
 *                 (the station-local readout passes its refined @f$g_s@f$); nullptr forms
 *                 them here with @ref functionalGain.
 */
void functionalShares(const NetworkAttribFrame& f, const NetworkRhsAttribDecomposition& dec,
                      const ConstrainedOperator& op, const Eigen::MatrixXd& H,
                      const std::vector<Eigen::Index>* unit_col,
                      NetworkRhsColumnShares& out,
                      const Eigen::MatrixXd* gains = nullptr) {
    const Eigen::Index n = f.n;
    const Eigen::Index J = H.cols();
    const Eigen::VectorXd zeroJ = Eigen::VectorXd::Zero(J);

    // g_j = N_c^-1 h_j: maps a reduced-RHS part to the functional's solution.
    Eigen::MatrixXd G = Eigen::MatrixXd::Zero(n, J);
    if (gains && gains->rows() == n && gains->cols() == J) {
        G = *gains;
    } else {
        for (Eigen::Index j = 0; j < J; ++j) G.col(j) = functionalGain(op, H.col(j));
    }
    // The load of a full reduced-RHS vector at every functional.
    const auto loadAt = [&](const Eigen::VectorXd& v, Eigen::Index j) {
        if (unit_col) {
            const Eigen::Index c = (*unit_col)[static_cast<std::size_t>(j)];
            return (c >= 0) ? v(c) : 0.0;
        }
        return H.col(j).dot(v);
    };
    const auto loadOf = [&](const Eigen::VectorXd& v, Eigen::VectorXd& rhs,
                            Eigen::VectorXd& sol) {
        rhs = zeroJ;
        for (Eigen::Index j = 0; j < J; ++j) rhs(j) = loadAt(v, j);
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
    // R_loc(:, j) = N_rr^-1 N_sr^T h_j reads the reduced-RHS load of a local-RHS part;
    // T_loc(:, j) = N_rr^-1 N_sr^T g_j reads its solution share.  A row adds w y times
    // (A_s h - A_r R_loc) and (A_s g - A_r T_loc); a prior entry on local column l adds
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
            R_loc = ldlt.solve(Nsr.transpose() * H);
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
                double as_h;
                if (unit_col) {
                    const Eigen::Index c = (*unit_col)[static_cast<std::size_t>(j)];
                    as_h = (c >= 0) ? R.A_s(row, c) : 0.0;
                } else {
                    as_h = R.A_s.row(row).dot(H.col(j));
                }
                const double ar_r = (nr > 0) ? R.A_r.row(row).dot(R_loc.col(j)) : 0.0;
                const double ar_t = (nr > 0) ? R.A_r.row(row).dot(T_loc.col(j)) : 0.0;
                rhs(j) = wy * (as_h - ar_r);
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
        loadOf(op.pin_total, p.rhs, p.sol);
        // On the pinned columns the solve sets x to the pin values: the datum's share there
        // (h_P^T v_P; for a named pinned column, the pin value itself).
        for (const auto& pin : f.datum) {
            if (pin.column < 0 || pin.column >= n) continue;
            for (Eigen::Index j = 0; j < J; ++j) {
                if (unit_col) {
                    if ((*unit_col)[static_cast<std::size_t>(j)] == pin.column)
                        p.sol(j) = pin.value;
                } else if (H(pin.column, j) != 0.0) {
                    p.sol(j) += H(pin.column, j) * pin.value;
                }
            }
        }
        out.sol_sum += p.sol;
        out.folds.push_back(std::move(p));
    }
    // KIND: the stations' kinds by name, then each fold as itself.
    for (const auto& p : out.folds) {
        NetworkRhsColumnShares::Part k = p;
        k.record = "KIND";
        out.kind_total.push_back(std::move(k));
    }
}

}  // namespace

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
    const ConstrainedOperator op = constrainedOperator(f);
    for (std::size_t j = 0; j < labels.size(); ++j)
        if (out.col[j] >= 0) out.pinned[j] = op.is_pinned[static_cast<std::size_t>(out.col[j])];
    if (!op.ok) return out;

    // ---- the unit functionals of the named columns, and the solution there ----------
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(n, J);
    for (Eigen::Index j = 0; j < J; ++j) {
        const Eigen::Index c = out.col[static_cast<std::size_t>(j)];
        if (c < 0) continue;
        H(c, j) = 1.0;
        out.b_red(j)  = f.b_red(c);
        out.x_full(j) = op.x_c(c);
        if (f.x_s.size() == n) out.x_solve(j) = f.x_s(c);
    }
    functionalShares(f, dec, op, H, &out.col, out);

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

// =========================================================================================
// The station-local readout
// =========================================================================================

bool isRhsAttribLocalLabel(const std::string& label) {
    return label.rfind("LOC:", 0) == 0;
}

bool rhsAttribNamesLocalColumns(const std::vector<std::string>& labels) {
    return std::any_of(labels.begin(), labels.end(),
                       [](const std::string& l) { return isRhsAttribLocalLabel(l); });
}

RhsAttribLocalLabel parseRhsAttribLocalLabel(const std::string& label) {
    RhsAttribLocalLabel out;
    out.label = label;
    if (!isRhsAttribLocalLabel(label)) return out;
    const std::size_t a = 4;                       // after "LOC:"
    const std::size_t b = label.find(':', a);
    if (b == std::string::npos || b == a || b + 1 >= label.size()) return out;
    out.station = label.substr(a, b - a);
    out.key     = label.substr(b + 1);
    return out;
}

std::vector<std::string> rhsAttribLocalColumnNames(const StationAssembly& R,
                                                   const StationParams& set) {
    std::vector<std::string> names(static_cast<std::size_t>(R.A_r.cols()));
    for (const auto& [sidx, col] : R.h2local) {
        if (sidx < 0 || col < 0 || col >= R.A_r.cols()) continue;
        const auto k = static_cast<std::size_t>(sidx);
        if (k >= set.slots.size()) continue;
        names[static_cast<std::size_t>(col)] = stateLabel(set.slots[k], k);
    }
    return names;
}

Eigen::Index rhsAttribResolveLocalKey(const std::vector<std::string>& names,
                                      const std::string& key) {
    if (key.empty()) return -1;
    for (std::size_t c = 0; c < names.size(); ++c)
        if (names[c] == key) return static_cast<Eigen::Index>(c);
    Eigen::Index hit = -1;
    int n_hit = 0;
    for (std::size_t c = 0; c < names.size(); ++c) {
        const std::size_t colon = names[c].find(':');
        if (colon == std::string::npos) continue;
        if (names[c].compare(colon + 1, std::string::npos, key) == 0) {
            hit = static_cast<Eigen::Index>(c);
            ++n_hit;
        }
    }
    return n_hit == 1 ? hit : (n_hit > 1 ? -2 : -1);
}

namespace {

/// A data row of a station's block: one of the four data kinds, or an untagged row of
/// the split's data region.
bool isLocalDataRow(const StationAssembly& R, Eigen::Index row, ContributionKind k) {
    if (k == ContributionKind::DataCode || k == ContributionKind::DataCodeCompanion
        || k == ContributionKind::DataPhase || k == ContributionKind::DataPhaseSecond)
        return true;
    return k == ContributionKind::Unset && row < R.n_data_rows;
}

/// The order of a fold channel in the dump: the block's rows, then the pre-PD and the
/// post-PD prior entries.
int channelOrder(const std::string& c) {
    return c == "row" ? 0 : (c == "pre_pd" ? 1 : 2);
}

/**
 * @brief The geometry-free level direction of one station: the null space of its
 *        data-only local normals, when one-dimensional (see @ref NetworkRhsLocalShares).
 *
 * @param R     The station's assembly.
 * @param names Its local-column names.
 * @param d     [out] the direction, scaled so that its @c IONO: loadings average 1.
 * @param desc  [out] its loadings on the station-level columns, '|'-separated.
 * @param note  [out] why there is none.
 * @return true when the direction was found.
 */
bool stationLevelDirection(const StationAssembly& R, const std::vector<std::string>& names,
                           Eigen::VectorXd& d, std::string& desc, std::string& note) {
    const Eigen::Index nr = R.A_r.cols();
    if (nr <= 0) { note = "no local columns"; return false; }
    Eigen::MatrixXd Nd = Eigen::MatrixXd::Zero(nr, nr);
    for (Eigen::Index row = 0; row < R.A_r.rows(); ++row) {
        if (!isLocalDataRow(R, row, rowTagAt(R, row).kind)) continue;
        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
        if (!std::isfinite(w) || w == 0.0) continue;
        Nd.noalias() += w * R.A_r.row(row).transpose() * R.A_r.row(row);
    }
    const Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Nd);
    if (es.info() != Eigen::Success) { note = "data-only eigen-decomposition failed"; return false; }
    const Eigen::VectorXd& ev = es.eigenvalues();   // ascending
    const double lmax = ev(nr - 1);
    if (!(lmax > 0.0)) { note = "no data information"; return false; }
    int n_null = 0;
    for (Eigen::Index q = 0; q < nr; ++q) if (ev(q) < 1e-11 * lmax) ++n_null;
    if (n_null != 1) {
        note = fmt::format("data-only null space of dimension {} (eigenvalues {:.3e} {:.3e} "
                           "of {:.3e})", n_null, ev(0), nr > 1 ? ev(1) : 0.0, lmax);
        return false;
    }
    d = es.eigenvectors().col(0);
    double iono_sum = 0.0;
    int n_iono = 0;
    for (Eigen::Index c = 0; c < nr; ++c)
        if (names[static_cast<std::size_t>(c)].rfind("IONO:", 0) == 0) {
            iono_sum += d(c);
            ++n_iono;
        }
    if (n_iono > 0 && std::abs(iono_sum / n_iono) > 1e-12) {
        d /= (iono_sum / n_iono);
    } else {
        Eigen::Index big = 0;
        d.cwiseAbs().maxCoeff(&big);
        d.normalize();
        if (d(big) < 0.0) d = -d;
    }
    // The loadings: every station-level column by name, the slant ionospheres and the
    // ambiguities by their mean.
    double iono_mean = 0.0, amb_mean = 0.0;
    int n_amb = 0;
    for (Eigen::Index c = 0; c < nr; ++c) {
        const std::string& nm = names[static_cast<std::size_t>(c)];
        if (nm.rfind("IONO:", 0) == 0) { iono_mean += d(c) / n_iono; continue; }
        if (nm.rfind("AMB:", 0) == 0) { amb_mean += d(c); ++n_amb; continue; }
        if (!desc.empty()) desc += '|';
        desc += (nm.empty() ? ("col" + std::to_string(c)) : nm) + '=' + fmt::format("{:.4f}", d(c));
    }
    if (n_amb > 0) amb_mean /= n_amb;
    desc += fmt::format("|IONO_mean={:.4f}|AMB_mean={:.4f}|lambda_rel={:.2e}",
                        iono_mean, amb_mean, ev(0) / lmax);
    for (char& ch : desc) if (ch == ',') ch = ';';
    return true;
}

/**
 * @brief @f$N_c v@f$ for the solve's constrained operator, applied from the frame's reduced
 *        normals without forming @f$N_c@f$: a free row is @f$\tilde N@f$'s row over the
 *        free entries of @f$v@f$, a pinned row is @f$s\,v_p@f$.
 * @param f  The frame (its @c N_red).
 * @param op The frame's constrained operator (its pins and @f$s@f$).
 * @param v  A vector of length @c n.
 * @return @f$N_c v@f$.
 */
Eigen::VectorXd constrainedTimes(const NetworkAttribFrame& f, const ConstrainedOperator& op,
                                 const Eigen::VectorXd& v) {
    Eigen::VectorXd vf = v;
    for (Eigen::Index q = 0; q < v.size(); ++q)
        if (op.is_pinned[static_cast<std::size_t>(q)]) vf(q) = 0.0;
    Eigen::VectorXd r = f.N_red * vf;
    for (Eigen::Index q = 0; q < v.size(); ++q)
        if (op.is_pinned[static_cast<std::size_t>(q)]) r(q) = op.pin_scale * v(q);
    return r;
}

/// The gains of one station-local target (the algebra of @ref NetworkRhsLocalShares).
struct LocalGains {
    Eigen::VectorXd g;   ///< @f$N_{rr}^{-1}p@f$: reads a direct share.
    Eigen::VectorXd h;   ///< @f$-N_{sr}g@f$: the feedback as a functional of the shared solution.
    Eigen::VectorXd gs;  ///< @f$N_c^{-1}h@f$, pinned entries zero; zero when the operator did not factorize.
    Eigen::VectorXd T;   ///< @f$N_{rr}^{-1}N_{sr}^\top g_s@f$: reads a local part's shared-path share.
};

/**
 * @brief Form the gains of one target, optionally with one step of iterative refinement of
 *        each of its three solves (the residual formed with the frame's normals).
 * @param f      The frame.
 * @param op     Its constrained operator; @f$g_s@f$ is zero unless it is @c ok.
 * @param k      The item.
 * @param ldlt   The factorization of the item's @f$N_{rr}@f$.
 * @param p      The functional.
 * @param refine Apply one refinement step to @f$g@f$, @f$g_s@f$ and @f$T@f$.
 * @return The gains.
 */
LocalGains localGains(const NetworkAttribFrame& f, const ConstrainedOperator& op,
                      std::size_t k, const Eigen::LDLT<Eigen::MatrixXd>& ldlt,
                      const Eigen::VectorXd& p, bool refine) {
    LocalGains G;
    const Eigen::MatrixXd& Nrr = f.Nrr[k];
    G.g = ldlt.solve(p);
    if (refine) G.g += ldlt.solve(p - Nrr * G.g);
    G.h = -(f.Nsr[k] * G.g);
    G.gs = op.ok ? functionalGain(op, G.h) : Eigen::VectorXd::Zero(f.n);
    if (refine && op.ok && G.gs.squaredNorm() > 0.0) {
        // g_s is zero on the pinned columns by convention (the datum sets them), so only
        // the free rows' residual is refined.
        Eigen::VectorXd r = G.h - constrainedTimes(f, op, G.gs);
        for (Eigen::Index q = 0; q < f.n; ++q)
            if (op.is_pinned[static_cast<std::size_t>(q)]) r(q) = 0.0;
        Eigen::VectorXd d = op.ldlt.solve(r);
        for (Eigen::Index q = 0; q < f.n; ++q)
            if (op.is_pinned[static_cast<std::size_t>(q)]) d(q) = 0.0;
        G.gs += d;
    }
    const Eigen::VectorXd b = f.Nsr[k].transpose() * G.gs;
    G.T = ldlt.solve(b);
    if (refine) G.T += ldlt.solve(b - Nrr * G.T);
    return G;
}

/// A tag's satellite as the readout writes it (@c "?" for none).
std::string localTagSat(const RowTag& tag) {
    return tag.sat.valid() ? tag.sat.str() : std::string("?");
}

/// A tag's signal as the readout writes it ("1C"; empty for none).
std::string localTagSig(const RowTag& tag) {
    return (tag.signal.sys != System::Unknown) ? tag.signal.str() : std::string();
}

/**
 * @brief Decompose @f$p^\top x_r@f$ for one frame item at given gains into its rows, fold
 *        groups, whole-station terms, the feedback's split and the station nets (the
 *        algebra of @ref NetworkRhsLocalShares).
 *
 * @param f   The frame (holds x_s and the item's local parts).
 * @param dec The frame's class decomposition (its DD parts feed the feedback split).
 * @param op  The frame's constrained operator; the feedback split runs when it is @c ok,
 *            and without it every shared-path share is NaN (unknown), not zero.
 * @param k   The item.
 * @param G   The gains (@ref localGains).
 * @param p   The functional (a unit vector, or @f$d/d^\top d@f$).
 * @param dir The direction whose row coefficient @c a reports (@f$e_k@f$ or @f$d@f$).
 * @param t   [in,out] the target; its parts and sums are filled.
 */
void fillLocalTarget(const NetworkAttribFrame& f, const NetworkRhsAttribDecomposition& dec,
                     const ConstrainedOperator& op, std::size_t k, const LocalGains& G,
                     const Eigen::VectorXd& p, const Eigen::VectorXd& dir,
                     NetworkRhsLocalShares::Target& t) {
    using Part = NetworkRhsLocalShares::Part;
    const StationAssembly& R = *f.asm_ptr[k];
    const auto& lp = f.local_parts[k];
    const Eigen::VectorXd& g  = G.g;
    const Eigen::VectorXd& h  = G.h;
    const Eigen::VectorXd& gs = G.gs;
    const Eigen::VectorXd& T  = G.T;
    const bool split = op.ok;
    const double unknown = std::numeric_limits<double>::quiet_NaN();

    std::vector<Part> rows;
    std::map<std::pair<int, std::string>, Part> fold;   // (channel order, kind)
    std::map<std::tuple<int, std::string, std::string, std::string>, Part> fold_los;
    std::map<std::string, Part> by_kind;
    double attributed = 0.0, scale = 0.0;
    const auto accumulate = [](Part& q, double share, double share_sh) {
        if (q.n == 0) q.share_shared = 0.0;
        q.share += share;
        q.share_shared += share_sh;
        ++q.n;
    };
    const auto addKind = [&](const std::string& kname, double share, double share_sh) {
        auto it = by_kind.find(kname);
        if (it == by_kind.end()) {
            Part q;
            q.record = "KIND";
            q.kind = kname;
            it = by_kind.emplace(kname, std::move(q)).first;
        }
        accumulate(it->second, share, share_sh);
    };
    const auto addFold = [&](const std::string& channel, const RowTag& tag, double share,
                             double share_sh) {
        const std::string kname = contributionKindName(tag.kind);
        const int ch = channelOrder(channel);
        auto it = fold.find({ch, kname});
        if (it == fold.end()) {
            Part q;
            q.record = "FOLD";
            q.channel = channel;
            q.kind = kname;
            it = fold.emplace(std::make_pair(ch, kname), std::move(q)).first;
        }
        accumulate(it->second, share, share_sh);
        const auto key = std::make_tuple(ch, kname, localTagSat(tag), localTagSig(tag));
        auto il = fold_los.find(key);
        if (il == fold_los.end()) {
            Part q;
            q.record = "FOLDLOS";
            q.channel = channel;
            q.kind = kname;
            q.sat = localTagSat(tag);
            q.signal = localTagSig(tag);
            il = fold_los.emplace(key, std::move(q)).first;
        }
        accumulate(il->second, share, share_sh);
        addKind(kname, share, share_sh);
        attributed += share;
    };

    // The station's rows: a data row is its own part, any other row folds by kind.  Each
    // has its direct share (through u_r) and its share through the shared solution.
    const bool have_xs = f.x_s.size() == R.A_s.cols() && lp.x_r.size() == R.A_r.cols();
    for (Eigen::Index row = 0; row < R.y.size(); ++row) {
        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
        const double wy = w * R.y(row);
        if (!std::isfinite(wy)) continue;   // as the class decomposition: never folded
        const RowTag& tag = rowTagAt(R, row);
        const double share = wy * R.A_r.row(row).dot(g);
        const double share_sh =
            split ? wy * (R.A_s.row(row).dot(gs) - R.A_r.row(row).dot(T)) : unknown;
        if (!isLocalDataRow(R, row, tag.kind)) {
            addFold("row", tag, share, share_sh);
            continue;
        }
        Part q;
        q.record = "ROW";
        q.channel = "row";
        q.kind = contributionKindName(tag.kind);
        q.sat = localTagSat(tag);
        q.signal = localTagSig(tag);
        q.obs = rowObservationCode(tag);
        q.row = row;
        q.n = 1;
        q.w = w;
        q.y = R.y(row);
        q.v = have_xs ? R.y(row) - R.A_s.row(row).dot(f.x_s) - R.A_r.row(row).dot(lp.x_r)
                      : unknown;
        q.a = R.A_r.row(row).dot(dir);
        q.share = share;
        q.share_shared = share_sh;
        attributed += share;
        addKind(q.kind, share, share_sh);
        scale = std::max(scale, std::abs(share));
        rows.push_back(std::move(q));
    }
    // The diagonal prior channels, as the kernel folded them.
    const auto addPrior = [&](const char* channel, const std::vector<Eigen::Index>& cols,
                              const Eigen::VectorXd& dur, const std::vector<RowTag>& tags) {
        for (std::size_t e = 0; e < cols.size() && e < static_cast<std::size_t>(dur.size()); ++e) {
            const Eigen::Index l = cols[e];
            if (l < 0 || l >= R.A_r.cols()) continue;
            const double du = dur(static_cast<Eigen::Index>(e));
            addFold(channel, (e < tags.size()) ? tags[e] : RowTag{}, du * g(l),
                    split ? -du * T(l) : unknown);
        }
    };
    addPrior("pre_pd", R.prior_cols, R.prior_dur, R.prior_tags);
    if (R.carry_prior_folded) addPrior("post_pd", R.carry_cols, R.carry_dur, R.carry_tags);

    t.attributed   = attributed;
    t.kernel_free  = p.dot(lp.free);
    t.unattributed = t.kernel_free - attributed;
    t.feedback     = h.dot(f.x_s);
    t.sumzero      = p.dot(lp.sumzero);
    t.ddprior      = p.dot(lp.ddprior);
    t.total        = attributed + t.feedback + t.sumzero + t.ddprior;
    t.solved       = p.dot(lp.x_r);
    t.closure_abs  = std::abs(t.total - t.solved);
    t.closure_rel  = (t.closure_abs == 0.0)
                         ? 0.0
                         : t.closure_abs / std::max(std::abs(t.solved),
                                                    std::numeric_limits<double>::min());

    t.parts = std::move(rows);
    for (auto& [key, q] : fold) {
        scale = std::max(scale, std::abs(q.share));
        t.parts.push_back(std::move(q));
    }
    for (auto& [key, q] : fold_los) t.parts.push_back(std::move(q));
    for (auto& [key, q] : by_kind) t.parts.push_back(std::move(q));
    const auto whole = [&](const char* record, double share) {
        Part q;
        q.record = record;
        q.share = share;
        scale = std::max(scale, std::abs(share));
        t.parts.push_back(std::move(q));
    };
    whole("FEEDBACK", t.feedback);
    whole("SUMZERO", t.sumzero);
    whole("DDPRIOR", t.ddprior);
    t.scale = scale;
    t.resolved = true;

    // ---- the feedback, split by shared column and by what the shared solution holds ----
    for (Eigen::Index c = 0; c < f.n; ++c) {
        if (h(c) == 0.0) continue;
        Part q;
        q.record = "FBCOL";
        q.kind = f.label[static_cast<std::size_t>(c)];
        q.n = 1;
        q.y = f.x_s(c);
        q.a = h(c);
        q.share = h(c) * f.x_s(c);
        t.parts.push_back(std::move(q));
    }
    if (!split) return;
    NetworkRhsColumnShares fs;
    fs.rhs_sum = fs.sol_sum = Eigen::VectorXd::Zero(1);
    const Eigen::MatrixXd H = h;    // one functional
    const Eigen::MatrixXd Gs = gs;  // its gain, as formed above (refined or not)
    functionalShares(f, dec, op, H, nullptr, fs, &Gs);
    const auto fbPart = [&](const char* record, const NetworkRhsColumnShares::Part& s) {
        Part q;
        q.record = record;
        q.from = s.sta;
        q.kind = s.kind;
        q.sat = s.sat;
        q.signal = s.sig;
        q.n = s.n;
        q.share = s.sol(0);
        t.parts.push_back(std::move(q));
    };
    for (const auto& s : fs.station_kind) fbPart("FBSTA", s);
    for (const auto& s : fs.station_los)  fbPart("FBLOS", s);
    for (const auto& s : fs.folds)        fbPart("FBFOLD", s);
    t.fb_split   = true;
    t.fb_network = fs.sol_sum(0);
    t.fb_closure = std::abs(t.fb_network - t.feedback);

    // ---- the station nets: every kind of a station together -----------------------------
    // A station's per-kind parts cancel (its phase rows against its carried blocks), so the
    // readout's ranking unit is the net.  The target station's net adds its direct shares
    // (rows and prior entries) to its shared-path part.
    struct Net {
        double    shared{0.0};
        double    direct{0.0};
        long long n{0};
    };
    std::map<std::string, Net> sta_net;
    std::map<std::pair<std::string, std::string>, Net> sat_net;
    for (const auto& s : fs.station_kind) {
        Net& e = sta_net[s.sta];
        e.shared += s.sol(0);
        e.n += s.n;
    }
    for (const auto& s : fs.station_los) {
        Net& e = sat_net[{s.sta, s.sat}];
        e.shared += s.sol(0);
        e.n += s.n;
    }
    sta_net[t.station].direct = attributed;
    for (const auto& q : t.parts)
        if (q.record == "ROW" || q.record == "FOLDLOS") sat_net[{t.station, q.sat}].direct += q.share;
    const auto netPart = [](const char* record, const std::string& from, const std::string& sat,
                            const Net& e) {
        Part q;
        q.record = record;
        q.from = from;
        q.sat = sat;
        q.n = e.n;
        q.share = e.shared + e.direct;
        q.share_shared = e.shared;
        return q;
    };
    const auto byNet = [](const Part& a, const Part& b) {
        return std::abs(a.share) > std::abs(b.share);
    };
    std::vector<Part> nets, sats;
    for (const auto& [sta, e] : sta_net) nets.push_back(netPart("NETSTA", sta, "", e));
    for (const auto& [key, e] : sat_net) sats.push_back(netPart("NETSAT", key.first, key.second, e));
    std::stable_sort(nets.begin(), nets.end(), byNet);   // ties keep the name order
    std::stable_sort(sats.begin(), sats.end(), byNet);
    for (auto& q : nets) t.parts.push_back(std::move(q));
    for (auto& q : sats) t.parts.push_back(std::move(q));
}

/// The identity of a part within its target (record, channel, station, kind, satellite,
/// signal, row): matches the unrefined and the refined decomposition part by part.
std::string localPartKey(const NetworkRhsLocalShares::Part& q) {
    return q.record + '|' + q.channel + '|' + q.from + '|' + q.kind + '|' + q.sat + '|'
         + q.signal + '|' + std::to_string(q.row);
}

/**
 * @brief Append the @c REFINE part: the largest change any part of @p t (its @c share or
 *        @c share_shared) took between the unrefined decomposition @p t0 and the refined
 *        one, an estimate of the unrefined parts' rounding.
 * @param t0 The target decomposed at the unrefined gains.
 * @param t  [in,out] the same target at the refined gains.
 */
void addRefineCheck(const NetworkRhsLocalShares::Target& t0, NetworkRhsLocalShares::Target& t) {
    std::map<std::string, const NetworkRhsLocalShares::Part*> before;
    for (const auto& q : t0.parts) before.emplace(localPartKey(q), &q);
    double worst = 0.0;
    std::string what;
    long long n = 0;
    const auto consider = [&](double a, double b, const std::string& key) {
        if (!std::isfinite(a) || !std::isfinite(b)) return;
        const double d = std::abs(a - b);
        if (what.empty() || d > worst) {
            worst = d;
            what = key;
        }
    };
    for (const auto& q : t.parts) {
        const std::string key = localPartKey(q);
        const auto it = before.find(key);
        if (it == before.end()) continue;
        ++n;
        consider(q.share, it->second->share, key);
        consider(q.share_shared, it->second->share_shared, key + "|shared");
    }
    NetworkRhsLocalShares::Part r;
    r.record = "REFINE";
    r.kind = what;
    r.n = n;
    r.share = worst;
    t.refine_max = worst;
    t.refine_part = what;
    t.parts.push_back(std::move(r));
}

/**
 * @brief The whole effect of a group of one item's rows and prior entries on
 *        @f$p^\top x_r@f$ of item @p k, measured by re-solving the reduced system the frame
 *        holds without the group's prefits (the @c PERTURB check of
 *        @ref NetworkRhsLocalShares).
 *
 * The group's right-hand side (@f$w y a@f$ per row, @f$\eta@f$ per prior entry) leaves its
 * station's @f$u_s@f$ and @f$u_r@f$; the reduced right-hand side loses the group's reduced
 * contribution (the pinned entries keep the datum's values), the constrained system is
 * solved again with the operator's factor (a prefit moves no normal), and the target's
 * locals are formed as @f$N_{rr}^{-1}(u_r - N_{sr}^\top x_c)@f$ with and without the group.
 * The DD folds and the gauges stay at their solved right-hand sides and the constraint
 * corrections are held out on both sides.
 *
 * @param f       The frame.
 * @param op      Its constrained operator (@c ok).
 * @param k       The target's item.
 * @param ldlt_k  The factorization of item @p k's @f$N_{rr}@f$.
 * @param p       The functional.
 * @param j       The group's item.
 * @param take    Membership of a channel term: (row, tag) for a row of @c A_r, (-1, tag)
 *                for a prior entry.
 * @param n_terms [out] The rows and prior entries in the group.
 * @return @f$p^\top(x_r - x_r')@f$: the value with the group less the value without it.
 */
double resolvedEffect(const NetworkAttribFrame& f, const ConstrainedOperator& op,
                      std::size_t k, const Eigen::LDLT<Eigen::MatrixXd>& ldlt_k,
                      const Eigen::VectorXd& p, std::size_t j,
                      const std::function<bool(Eigen::Index, const RowTag&)>& take,
                      long long& n_terms) {
    const StationAssembly& R = *f.asm_ptr[j];
    const Eigen::Index nr = R.A_r.cols();
    Eigen::VectorXd du_s = Eigen::VectorXd::Zero(f.n);
    Eigen::VectorXd du_r = Eigen::VectorXd::Zero(nr);
    n_terms = 0;
    for (Eigen::Index row = 0; row < R.y.size(); ++row) {
        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
        const double wy = w * R.y(row);
        if (!std::isfinite(wy) || !take(row, rowTagAt(R, row))) continue;
        du_s.noalias() += R.A_s.row(row).transpose() * wy;
        du_r.noalias() += R.A_r.row(row).transpose() * wy;
        ++n_terms;
    }
    const auto prior = [&](const std::vector<Eigen::Index>& cols, const Eigen::VectorXd& dur,
                           const std::vector<RowTag>& tags) {
        for (std::size_t e = 0; e < cols.size() && e < static_cast<std::size_t>(dur.size()); ++e) {
            const Eigen::Index l = cols[e];
            if (l < 0 || l >= nr) continue;
            if (!take(-1, (e < tags.size()) ? tags[e] : RowTag{})) continue;
            du_r(l) += dur(static_cast<Eigen::Index>(e));
            ++n_terms;
        }
    };
    prior(R.prior_cols, R.prior_dur, R.prior_tags);
    if (R.carry_prior_folded) prior(R.carry_cols, R.carry_dur, R.carry_tags);
    if (n_terms == 0) return 0.0;

    Eigen::LDLT<Eigen::MatrixXd> other;
    if (j != k) other.compute(f.Nrr[j]);
    const Eigen::LDLT<Eigen::MatrixXd>& ldlt_j = (j == k) ? ldlt_k : other;
    Eigen::VectorXd uc1 = op.uc - (du_s - f.Nsr[j] * ldlt_j.solve(du_r));
    for (Eigen::Index q = 0; q < f.n; ++q)
        if (op.is_pinned[static_cast<std::size_t>(q)]) uc1(q) = op.uc(q);
    const Eigen::VectorXd xc1 = op.ldlt.solve(uc1);
    Eigen::VectorXd ur1 = f.ur[k];
    if (j == k) ur1 -= du_r;
    const Eigen::VectorXd xr0 = ldlt_k.solve(f.ur[k] - f.Nsr[k].transpose() * op.x_c);
    const Eigen::VectorXd xr1 = ldlt_k.solve(ur1 - f.Nsr[k].transpose() * xc1);
    return p.dot(xr0 - xr1);
}

/**
 * @brief Append the @c PERTURB parts of a target: for four groups, the attributed whole
 *        effect (@c share) beside the change a re-solve without the group's prefits
 *        measures (@c check, @ref resolvedEffect).  The groups: the target station's data
 *        row of largest whole effect (@c row), the other-station @c FBLOS group of largest
 *        |share| (@c los), the target station whole and the other station of largest |net|
 *        (@c station).
 * @param f    The frame.
 * @param op   Its constrained operator (@c ok).
 * @param k    The target's item.
 * @param ldlt The factorization of item @p k's @f$N_{rr}@f$.
 * @param p    The functional.
 * @param t    [in,out] the decomposed target (with its feedback split and nets).
 */
void addPerturbChecks(const NetworkAttribFrame& f, const ConstrainedOperator& op,
                      std::size_t k, const Eigen::LDLT<Eigen::MatrixXd>& ldlt,
                      const Eigen::VectorXd& p, NetworkRhsLocalShares::Target& t) {
    using Part = NetworkRhsLocalShares::Part;
    const auto itemOf = [&](const std::string& station) -> long long {
        for (std::size_t i = 0; i < f.sta_name.size(); ++i)
            if (f.sta_name[i] == station) return static_cast<long long>(i);
        return -1;
    };
    // The groups, copied out of t.parts (the checks are appended to it).
    const Part* row = nullptr;
    const Part* los = nullptr;
    const Part* own = nullptr;
    const Part* other = nullptr;
    for (const auto& q : t.parts) {
        if (q.record == "ROW" && std::isfinite(q.share_shared)
            && (!row || std::abs(q.share + q.share_shared)
                            > std::abs(row->share + row->share_shared)))
            row = &q;
        if (q.record == "FBLOS" && q.from != t.station
            && (!los || std::abs(q.share) > std::abs(los->share)))
            los = &q;
        if (q.record == "NETSTA") {
            if (q.from == t.station) own = &q;
            else if (!other || std::abs(q.share) > std::abs(other->share)) other = &q;
        }
    }
    std::vector<Part> checks;
    const auto check = [&](const char* group, const Part& src, double attributed,
                           long long item,
                           const std::function<bool(Eigen::Index, const RowTag&)>& take) {
        if (item < 0) return;
        Part c;
        c.record = "PERTURB";
        c.channel = group;
        c.from = src.from.empty() ? t.station : src.from;
        c.row = src.row;
        c.kind = src.kind;
        c.sat = src.sat;
        c.signal = src.signal;
        c.obs = src.obs;
        c.share = attributed;
        c.check = resolvedEffect(f, op, k, ldlt, p, static_cast<std::size_t>(item), take, c.n);
        t.perturb_max_err = std::max(t.perturb_max_err, std::abs(c.check - c.share));
        ++t.n_perturb;
        checks.push_back(std::move(c));
    };
    if (row) {
        const Eigen::Index r = row->row;
        check("row", *row, row->share + row->share_shared, static_cast<long long>(k),
              [r](Eigen::Index i, const RowTag&) { return i == r; });
    }
    if (los) {
        const std::string kind = los->kind, sat = los->sat, sig = los->signal;
        check("los", *los, los->share, itemOf(los->from),
              [kind, sat, sig](Eigen::Index, const RowTag& tag) {
                  return contributionKindName(tag.kind) == kind && localTagSat(tag) == sat
                      && localTagSig(tag) == sig;
              });
    }
    const auto all = [](Eigen::Index, const RowTag&) { return true; };
    if (own) check("station", *own, own->share, static_cast<long long>(k), all);
    if (other) check("station", *other, other->share, itemOf(other->from), all);
    for (auto& c : checks) t.parts.push_back(std::move(c));
}

/**
 * @brief Decompose @f$p^\top x_r@f$ for one frame item: the parts at the refined gains,
 *        the @c REFINE check against the unrefined ones and, when the feedback split ran,
 *        the @c PERTURB re-solve checks (see @ref NetworkRhsLocalShares).
 *
 * @param f    The frame (holds x_s and the item's local parts).
 * @param dec  The frame's class decomposition.
 * @param op   The frame's constrained operator.
 * @param k    The item.
 * @param ldlt The factorization of the item's @f$N_{rr}@f$.
 * @param p    The functional (a unit vector, or @f$d/d^\top d@f$).
 * @param dir  The direction whose row coefficient @c a reports (@f$e_k@f$ or @f$d@f$).
 * @param t    [in,out] the target; its parts and sums are filled.
 */
void decomposeLocalTarget(const NetworkAttribFrame& f, const NetworkRhsAttribDecomposition& dec,
                          const ConstrainedOperator& op, std::size_t k,
                          const Eigen::LDLT<Eigen::MatrixXd>& ldlt,
                          const Eigen::VectorXd& p, const Eigen::VectorXd& dir,
                          NetworkRhsLocalShares::Target& t) {
    NetworkRhsLocalShares::Target t0 = t;   // the unrefined decomposition, for REFINE only
    fillLocalTarget(f, dec, op, k, localGains(f, op, k, ldlt, p, /*refine=*/false), p, dir, t0);
    fillLocalTarget(f, dec, op, k, localGains(f, op, k, ldlt, p, /*refine=*/true), p, dir, t);
    addRefineCheck(t0, t);
    if (t.fb_split) addPerturbChecks(f, op, k, ldlt, p, t);
}

}  // namespace

NetworkRhsLocalShares networkRhsAttribLocalShares(const NetworkAttribFrame& f,
                                                  const NetworkRhsAttribDecomposition& dec,
                                                  const std::vector<std::string>& labels,
                                                  bool gf_direction) {
    NetworkRhsLocalShares out;
    const bool frame_ok = f.valid && f.n > 0 && f.x_s.size() == f.n
                       && f.N_red.rows() == f.n && f.b_red.size() == f.n
                       && f.local_parts.size() == f.asm_ptr.size()
                       && f.local_names.size() == f.asm_ptr.size();
    out.valid = frame_ok;
    // The solve's constrained operator, for the feedback's split (built once per frame).
    const ConstrainedOperator op = frame_ok ? constrainedOperator(f) : ConstrainedOperator{};

    // One factorization per item touched (the frame's N_rr, as the class decomposition).
    std::map<std::size_t, Eigen::LDLT<Eigen::MatrixXd>> ldlt_of;
    const auto itemOf = [&](const std::string& station) -> long long {
        for (std::size_t k = 0; k < f.sta_name.size(); ++k)
            if (f.sta_name[k] == station) return static_cast<long long>(k);
        return -1;
    };
    // The item's blocks agree with each other (every size the decomposition reads).
    const auto itemConsistent = [&](std::size_t k) {
        const StationAssembly& R = *f.asm_ptr[k];
        const Eigen::Index nr = R.A_r.cols();
        const auto& lp = f.local_parts[k];
        return nr > 0 && f.Nrr[k].rows() == nr && f.Nrr[k].cols() == nr
            && f.Nsr[k].cols() == nr && f.Nsr[k].rows() == f.n
            && lp.free.size() == nr && lp.feedback.size() == nr && lp.sumzero.size() == nr
            && lp.ddprior.size() == nr && lp.x_r.size() == nr
            && f.local_names[k].size() == static_cast<std::size_t>(nr);
    };
    const auto ldltOf = [&](std::size_t k) -> const Eigen::LDLT<Eigen::MatrixXd>& {
        auto it = ldlt_of.find(k);
        if (it == ldlt_of.end()) it = ldlt_of.emplace(k, Eigen::LDLT<Eigen::MatrixXd>(f.Nrr[k])).first;
        return it->second;
    };

    std::vector<std::string> gf_stations;   // stations with a resolved label, first-seen order
    for (const auto& label : labels) {
        const RhsAttribLocalLabel pl = parseRhsAttribLocalLabel(label);
        NetworkRhsLocalShares::Target t;
        t.label = label;
        t.station = pl.station;
        t.key = pl.key;
        if (pl.station.empty() || pl.key.empty()) {
            t.note = "malformed label (expected LOC:<station>:<key>)";
        } else if (!frame_ok) {
            t.note = "the frame holds no station-local capture (only the dumps with the "
                     "solve's x_s carry one)";
        } else if (const long long k = itemOf(pl.station); k < 0) {
            t.note = "station not in the solve this epoch";
        } else if (!itemConsistent(static_cast<std::size_t>(k))) {
            t.note = "inconsistent station blocks";
        } else {
            const std::size_t ku = static_cast<std::size_t>(k);
            const Eigen::Index nr = f.asm_ptr[ku]->A_r.cols();
            t.n_local = nr;
            const Eigen::Index c = rhsAttribResolveLocalKey(f.local_names[ku], pl.key);
            if (c == -2) {
                t.note = "key names more than one local column; use the full column name";
            } else if (c < 0) {
                t.note = "no local column of that key this epoch";
            } else if (ldltOf(ku).info() != Eigen::Success) {
                t.note = "the station's local normals did not factorize";
            } else {
                t.local_col = c;
                t.name = f.local_names[ku][static_cast<std::size_t>(c)];
                const Eigen::VectorXd e = Eigen::VectorXd::Unit(nr, c);
                decomposeLocalTarget(f, dec, op, ku, ldltOf(ku), e, e, t);
                if (std::find(gf_stations.begin(), gf_stations.end(), pl.station)
                    == gf_stations.end())
                    gf_stations.push_back(pl.station);
            }
        }
        out.targets.push_back(std::move(t));
    }

    if (!gf_direction) return out;
    for (const auto& station : gf_stations) {
        const std::size_t k = static_cast<std::size_t>(itemOf(station));
        NetworkRhsLocalShares::Target t;
        t.label = "GFDIR:" + station;
        t.station = station;
        t.key = "GFDIR";
        t.n_local = f.asm_ptr[k]->A_r.cols();
        Eigen::VectorXd d;
        std::string desc, note;
        if (!stationLevelDirection(*f.asm_ptr[k], f.local_names[k], d, desc, note)) {
            t.note = note;
        } else {
            t.name = desc;
            const Eigen::VectorXd p = d / d.squaredNorm();
            decomposeLocalTarget(f, dec, op, k, ldltOf(k), p, d, t);
        }
        out.targets.push_back(std::move(t));
    }
    return out;
}

namespace {

/**
 * @brief Write `cols_<tow>.csv`, the solution shares at the named shared columns
 *        (instrument A'; the format is documented at @ref networkRhsAttribDump).
 * @param f       The frame.
 * @param dec     Its class decomposition.
 * @param labels  The shared labels of @c network.rhs_attrib_columns.
 * @param log     Run logger.
 * @param dir     Output directory.
 * @param kind    The dump's reason (log text only).
 * @param tow_tag The file-name TOW.
 */
void writeColumnShares(const NetworkAttribFrame& f, const NetworkRhsAttribDecomposition& dec,
                       const std::vector<std::string>& labels,
                       const std::shared_ptr<spdlog::logger>& log,
                       const std::filesystem::path& dir, const char* kind,
                       const std::string& tow_tag) {
    const NetworkRhsColumnShares sh = networkRhsAttribColumnShares(f, dec, labels);
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
    line("SOLVE", "", "", "", "", "", nullptr, f.x_s.size() == f.n ? &sh.x_solve : nullptr);
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

/**
 * @brief Write `loc_<tow>.csv`, the decomposition of the named station-local solved
 *        increments (the format is documented at @ref networkRhsAttribDump).
 *
 * Nothing is written for a frame without the station-local capture (a retained previous
 * frame, which holds no solve @c x_s); the labels are then named once in the log.  An
 * unresolved label keeps its @c TARGET line with its note and is named once in the log.
 * The run log gives per target the increment and its split, the station nets (the ranking
 * unit: a station's per-kind parts cancel) and the two checks of the single parts.
 *
 * @param f       The frame.
 * @param dec     Its class decomposition.
 * @param labels  The `LOC:` labels of @c network.rhs_attrib_columns.
 * @param log     Run logger.
 * @param dir     Output directory.
 * @param kind    The dump's reason (log text only).
 * @param tow_tag The file-name TOW.
 * @param warned  The conditions already named this run (nullptr: name them every time).
 */
void writeLocalShares(const NetworkAttribFrame& f, const NetworkRhsAttribDecomposition& dec,
                      const std::vector<std::string>& labels,
                      const std::shared_ptr<spdlog::logger>& log,
                      const std::filesystem::path& dir, const char* kind,
                      const std::string& tow_tag, std::set<std::string>* warned) {
    using Part = NetworkRhsLocalShares::Part;
    const auto once = [&](const std::string& key) {
        if (!warned) return true;
        return warned->insert(key).second;
    };
    const NetworkRhsLocalShares ls = networkRhsAttribLocalShares(f, dec, labels);
    if (!ls.valid) {
        if (once("<no local capture>"))
            log->warn("Network (rhs-attrib decomposition): [{}] tow {:.1f}: the frame holds no "
                      "station-local capture, so the {} station-local label(s) are not "
                      "decomposed (the capture needs the solve's x_s, which the armed "
                      "network.rhs_attrib_tow dumps and a trip's current-epoch dump carry and "
                      "a retained previous frame does not; named once)",
                      kind, f.tow, labels.size());
        return;
    }
    std::ofstream o(dir / ("loc_" + tow_tag + ".csv"), std::ios::trunc);
    o << "week,tow,label,station,name,local_col,record,channel,from_station,row,kind,sat,"
         "signal,obs,n,w,y,v_post,a_col,share,share_shared,check\n";
    const std::string head = fmt::format("{},{:.3f},", f.week, f.tow);
    // A value the readout does not have (a shared-path share whose split did not run, a
    // post-fit residual without x_s) is an empty cell, never a number.
    const auto num = [](double v) {
        return std::isfinite(v) ? fmt::format("{:.9e}", v) : std::string();
    };
    const auto text = [](std::string s) {   // a free-text cell holds no comma
        for (char& ch : s) if (ch == ',') ch = ';';
        return s;
    };
    // The sixteen cells after the target's prefix: record, channel, from_station, row,
    // kind, sat, signal, obs, n, w, y, v_post, a_col, share, share_shared, check.
    using Cells = std::array<std::string, 16>;
    const auto put = [&](const std::string& pre, const Cells& c) {
        o << pre;
        for (std::size_t i = 0; i < c.size(); ++i) o << (i ? "," : "") << c[i];
        o << '\n';
    };
    for (const auto& t : ls.targets) {
        const std::string pre = head + t.label + ',' + t.station + ',' + text(t.name) + ','
                              + (t.local_col >= 0 ? std::to_string(t.local_col) : "") + ',';
        put(pre, Cells{"TARGET", "", "", "", text(t.note), "", "", "",
                       std::to_string(t.n_local), "", "", "", "",
                       t.resolved ? num(t.solved) : "", "", ""});
        if (!t.resolved) {
            if (once(t.label))
                log->warn("Network (rhs-attrib decomposition): [{}] tow {:.1f}: '{}' not "
                          "decomposed: {} (its slot is kept; named once)",
                          kind, f.tow, t.label, t.note);
            continue;
        }
        for (const auto& q : t.parts) {
            const bool row = q.record == "ROW";
            const bool col = q.record == "FBCOL";
            put(pre, Cells{q.record, q.channel, q.from,
                           q.row >= 0 ? std::to_string(q.row) : "", text(q.kind), q.sat,
                           q.signal, q.obs, std::to_string(q.n), row ? num(q.w) : "",
                           row || col ? num(q.y) : "", row ? num(q.v) : "",
                           row || col ? num(q.a) : "", num(q.share), num(q.share_shared),
                           num(q.check)});
        }
        const auto line = [&](const char* record, double v) {
            put(pre, Cells{record, "", "", "", "", "", "", "", "", "", "", "", "", num(v),
                           "", ""});
        };
        line("UNATTRIBUTED", t.unattributed);
        line("TOTAL", t.total);
        line("SOLVED", t.solved);
        line("CLOSURE_ABS", t.closure_abs);
        line("CLOSURE_REL", t.closure_rel);
        if (t.fb_split) {
            line("FBTOTAL", t.fb_network);
            line("FBCLOSURE", t.fb_closure);
        }

        // ---- the run log -----------------------------------------------------------------
        const std::string what = t.key == "GFDIR" ? std::string("level direction") : t.name;
        log->info("Network (rhs-attrib decomposition): [{}] tow {:.1f} {} ({}): x_r {:.6f} = "
                  "direct (this station's rows and folds) {:.6f} + feedback through the shared "
                  "solution {:.6f} + constraints {:.6f} (closure abs {:.3e} rel {:.3e}, "
                  "unattributed {:.3e})",
                  kind, f.tow, t.label, what, t.solved, t.attributed, t.feedback,
                  t.sumzero + t.ddprior, t.closure_abs, t.closure_rel, t.unattributed);
        if (!t.fb_split) {
            log->warn("Network (rhs-attrib decomposition): [{}] tow {:.1f} {}: the constrained "
                      "operator did not factorize; the shared-path shares are empty and no "
                      "station net is written", kind, f.tow, t.label);
            continue;
        }
        // The station nets, already sorted by |net|.  Only NETSTA is ranked: the
        // (station, satellite) nets cancel across a station's satellites through its
        // station-common columns, so they are detail, not a ranking.
        double own_net = 0.0, own_shared = 0.0, other_net = 0.0, folds = 0.0;
        int n_other = 0;
        std::string top_sta;
        for (const Part& q : t.parts) {
            if (q.record == "NETSTA") {
                if (q.from == t.station) {
                    own_net = q.share;
                    own_shared = q.share_shared;
                } else {
                    if (n_other < 6)
                        top_sta += fmt::format("{}{} {:+.4f}", top_sta.empty() ? "" : ", ",
                                               q.from, q.share);
                    other_net += q.share;
                    ++n_other;
                }
            }
            if (q.record == "FBFOLD") folds += q.share;
        }
        log->info("Network (rhs-attrib decomposition): [{}] tow {:.1f} {}: by station net (every "
                  "kind together, direct + through the shared solution): this station {:.6f} "
                  "(direct {:.6f}, shared {:.6f}), {} other stations {:.6f}, shared folds, DD "
                  "folds and datum {:.6f}, constraints {:.6f} (split closure {:.3e}); largest "
                  "other stations by |net|: {}",
                  kind, f.tow, t.label, own_net, t.attributed, own_shared, n_other, other_net,
                  folds, t.sumzero + t.ddprior, t.fb_closure, top_sta.empty() ? "-" : top_sta);
        std::string checks;
        for (const Part& q : t.parts) {
            if (q.record != "PERTURB") continue;
            std::string who = q.from;
            if (q.channel == "row")
                who += fmt::format(" row {} {} {}", q.row, q.sat, q.obs);
            else if (q.channel == "los")
                who += fmt::format(" {} {} {}", q.kind, q.sat, q.signal);
            checks += fmt::format("{}{} {}: {:+.6f} vs {:+.6f}", checks.empty() ? "" : "; ",
                                  q.channel, who, q.share, q.check);
        }
        log->info("Network (rhs-attrib decomposition): [{}] tow {:.1f} {}: checks of single "
                  "parts: one refinement step moved a part by at most {:.3e} m ({}); "
                  "attributed vs re-solved without the group's prefits: {} (largest "
                  "difference {:.3e} m)",
                  kind, f.tow, t.label, t.refine_max, t.refine_part,
                  checks.empty() ? "-" : checks, t.perturb_max_err);
    }
}

}  // namespace

void networkRhsAttribDump(const NetworkAttribFrame& f,
                          const std::shared_ptr<spdlog::logger>& log,
                          const std::filesystem::path& dir, const char* kind,
                          const std::vector<std::string>& columns,
                          std::set<std::string>* warned) {
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
    // The last column is the row's contribution kind: the
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

    // ---- the named columns: shared (cols_) and station-local (loc_) --
    std::vector<std::string> shared_labels, local_labels;
    for (const auto& c : columns)
        (isRhsAttribLocalLabel(c) ? local_labels : shared_labels).push_back(c);
    if (!shared_labels.empty())
        writeColumnShares(f, dec, shared_labels, log, dir, kind, tow_tag);
    if (!local_labels.empty())
        writeLocalShares(f, dec, local_labels, log, dir, kind, tow_tag, warned);
}

void networkRhsAttribution(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    const NetworkRunTraces& traces = run.traces;
    const SyncedEpoch& se = ep.se;
    const std::vector<NetStation>& stations = run.setup.stations;
    const NetworkEstimator& est = run.engines.est;
    const NetworkEstimator::Datum& plan_datum = ep.solve.plan.shared_datum;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const std::vector<int>& est_index_of = ep.assembly.est_index_of;
    const Eigen::VectorXd* x_s = &ep.solve.x_s;
    // ---- End of the bindings ----
    if (!est.attribArmed()) return;   // the driver's gate
    const auto& nc = config_.getNetworkConfig();
    if (nc.rhs_attrib_tow < 0.0) return;
    if (ctx.carry.rhs_attrib_done >= std::max(1, nc.rhs_attrib_n_ep)) return;
    if (se.time.tow < nc.rhs_attrib_tow - 0.25) return;
    ++ctx.carry.rhs_attrib_done;

    const auto& columns = config_.networkRhsAttribColumns();
    NetworkAttribFrame f;
    networkAttribCaptureFrame(f, se, stations, est, plan_datum, asm_res,
                              est_index_of, /*own_assemblies=*/false, x_s,
                              rhsAttribNamesLocalColumns(columns));
    networkRhsAttribDump(f, log, traces.log_dir_ / "rhs_attrib", "armed",
                         columns, &ctx.carry.rhs_attrib_loc_warned);
    ++ctx.counters.pc.rec_dumps;
}

bool networkRhsTripEdge(NetworkRunContext& ctx, std::string& trip, const char* kind, bool active) {
    if (!ctx.knobs.rec_on) return false;
    ++ctx.counters.pc.rec_consults;
    auto& eras = ctx.carry.rhs_trip_eras;
    const std::string key(kind ? kind : "?");
    const bool was = eras.count(key) != 0;
    if (active && !was) {
        eras.insert(key);
        ++ctx.counters.pc.rec_trips;
        // First writer wins: the first era to trip in an epoch names the dump.
        if (trip.empty()) trip = key;
        return true;
    }
    if (!active && was) eras.erase(key);
    return false;
}

void networkRhsAttribTripAt(NetworkRun& run, const NetworkEpoch& ep,
                            const NetworkEstimator::Datum& plan_datum,
                            const std::vector<int>& est_index_of, const Eigen::VectorXd* x_s) {
    // ---- From the run and the epoch (the former explicit parameters, each
    //      bound under its former name; the body below is unchanged) ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    const NetworkRunTraces& traces = run.traces;
    const SyncedEpoch& se = ep.se;
    const std::vector<NetStation>& stations = run.setup.stations;
    const NetworkEstimator& est = run.engines.est;
    const std::vector<StationAssembly>& asm_res = ep.assembly.asm_res;
    const std::string& rhs_trip_pending = ep.events.qc.rhs_trip_pending;
    // ---- End of the bindings ----
    if (!ctx.knobs.rec_on) return;
    const auto& nc = config_.getNetworkConfig();
    auto& ctr = ctx.counters.pc;
    const std::filesystem::path dir = traces.log_dir_ / "rhs_attrib";

    // --- 1. consume a pending trip -------------------------------------------
    if (!rhs_trip_pending.empty()) {
        const std::string why = rhs_trip_pending;
        NetworkAttribFrame now;
        networkAttribCaptureFrame(now, se, stations, est, plan_datum, asm_res,
                                  est_index_of, /*own_assemblies=*/false, x_s,
                                  rhsAttribNamesLocalColumns(config_.networkRhsAttribColumns()));
        networkRhsAttribDump(now, log, dir, ("trip:" + why).c_str(),
                             config_.networkRhsAttribColumns(),
                             &ctx.carry.rhs_attrib_loc_warned);
        ++ctr.rec_dumps;
        if (nc.rhs_attrib_on_trip_prev) {
            if (ctx.carry.rhs_prev_frame && ctx.carry.rhs_prev_frame->valid) {
                networkRhsAttribDump(*ctx.carry.rhs_prev_frame, log, dir,
                                     ("trip-prev:" + why).c_str(),
                                     config_.networkRhsAttribColumns(),
                                     &ctx.carry.rhs_attrib_loc_warned);
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
        if (!ctx.counters.rhs_prev_oversize_logged) {
            ctx.counters.rhs_prev_oversize_logged = true;
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

// ---- Epoch forms (generated by scripts/nom_ep1_cut.py): one forwarding
//      call each, with the bindings of the frozen call site; the explicit forms
//      above stay for their other callers. ----

void networkRhsAttribTrip(NetworkRun& run, const NetworkEpoch& ep) {
    networkRhsAttribTripAt(run, ep, ep.solve.plan.shared_datum, ep.assembly.est_index_of, &ep.solve.x_s);
}

// ---- End of the epoch forms ----

} // namespace GnssProcessor

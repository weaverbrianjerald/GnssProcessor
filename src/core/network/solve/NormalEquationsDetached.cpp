/**
 * @file NormalEquationsDetached.cpp
 * @brief The detached fold of fixed integer cycles: the
 *        @ref NetworkNormalEquations::foldReducedDdPriors algebra for blocks of
 *        soft cycle constraints, returned as operands instead of written into the
 *        kernel.
 *
 * A translation unit of its own so that @ref NetworkNormalEquations stays the single
 * owner of the DD-prior algebra while the second fold path (sparse, per block of
 * disjoint stations, read-only on the kernel) is kept apart from the Card-4 dense fold
 * (@c NormalEquations.cpp), its unit oracle.  This is the first use of Eigen's
 * sparse modules in the producer, and they are included here only.
 *
 * The algebra, per block of @f$K@f$ rows @f$z=Cx_L\sim N(\hat z,P^z)@f$ (the stations
 * of a block are touched by no other block, so the blocks' corrections add exactly):
 * @f[
 *   B=\sum_r C_rN_{rr}^{-1}C_r^\top,\quad
 *   G=\sum_r N_{sr}N_{rr}^{-1}C_r^\top,\quad
 *   z_{\mathrm{free}}=\sum_r C_rN_{rr}^{-1}u_r,\quad
 *   A=P^z+B,
 * @f]
 * @f[
 *   \Delta N=G A^{-1}G^\top,\qquad \Delta u=G A^{-1}(z_{\mathrm{free}}-\hat z),
 * @f]
 * the Woodbury fold of @ref NetworkNormalEquations::foldReducedDdPriors with
 * @f$M=A^{-1}@f$.  @f$P^z@f$ is diagonal (one hold variance per row), so @f$A@f$ is as
 * sparse as the rows' station overlap.
 *
 * Two screens guard a block: @f$B@f$ alone (singular exactly when the rows are linearly
 * dependent, whatever the hold) and then @f$A@f$.  A block that folds also keeps
 * @f$X=A^{-1}G^\top@f$ and @f$\mathrm{diag}(A^{-1})@f$, the latter by Takahashi's
 * selected inversion on the factor's own sparsity pattern, so the held solve can state
 * the mean of its statistic under right targets without a dense @f$K\times K@f$ inverse.
 */

#include "network/solve/NormalEquations.hpp"

#include <Eigen/SparseCholesky>   // SimplicialLDLT (and SparseCore, OrderingMethods)

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

namespace {

/// The sparse factorization used by both screens: an AMD-ordered LDLᵀ of the lower
/// triangle.
using SparseLdlt = Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>, Eigen::Lower,
                                         Eigen::AMDOrdering<int>>;

}  // namespace

/// The opaque factor of @ref NetworkNormalEquations::DetachedFoldBlock: an AMD-ordered
/// sparse LDLᵀ of the block's equilibrated @f$D(P^z+B)D@f$ (lower triangle).
struct NetworkNormalEquations::DetachedFoldFactor {
    SparseLdlt ldlt;
};

namespace {

/// Throw std::invalid_argument with the kernel's uniform prefix.
[[noreturn]] void failDetached(const std::string& what) {
    throw std::invalid_argument("NetworkNormalEquations::foldCycleRowsDetached: " + what);
}

/// Diagonal floor of the equilibration, relative to the largest diagonal (the value of
/// the dense fold's PD-safe screen, kDdEquilDiagFloorRel): a zero diagonal (a row whose
/// coefficients cancel) keeps a finite scale, and the pivot screen makes the decision.
constexpr double kEquilDiagFloorRel = 1e-16;

/// The outcome of one screened factorization.
struct Screened {
    Eigen::VectorXd scale;     ///< The Jacobi scale @f$D=\mathrm{diag}(M)^{-1/2}@f$ (floored).
    bool   ok{false};          ///< Factored, every pivot positive and at least the floor times the largest.
    double min_pivot_rel{0.0}; ///< Smallest pivot relative to the largest (0 when the factorization failed).
};

/// Jacobi-equilibrate the symmetric @p M (@f$DMD@f$, so every pivot is of order 1 and
/// the relative screen measures conditioning rather than the spread between a fresh and
/// a mature row), factor it into @p ldlt and screen the pivots against
/// @p pivot_floor_rel times the largest.
Screened factorScreened(const Eigen::SparseMatrix<double>& M, double pivot_floor_rel,
                        SparseLdlt& ldlt) {
    const Eigen::Index K = M.rows();
    Screened s;
    s.scale = Eigen::VectorXd::Ones(K);
    const Eigen::VectorXd d = M.diagonal();
    const double dmax_abs = d.cwiseAbs().maxCoeff();
    const double floor_d  = (dmax_abs > 0.0) ? kEquilDiagFloorRel * dmax_abs : 1.0;
    for (Eigen::Index k = 0; k < K; ++k) s.scale(k) = 1.0 / std::sqrt(std::max(d(k), floor_d));
    const Eigen::SparseMatrix<double> Ms = s.scale.asDiagonal() * M * s.scale.asDiagonal();
    ldlt.compute(Ms);
    if (ldlt.info() != Eigen::Success) return s;
    const Eigen::VectorXd piv = ldlt.vectorD();
    const double pmax = piv.cwiseAbs().maxCoeff();
    const double pmin = piv.minCoeff();
    s.min_pivot_rel = (pmax > 0.0) ? (pmin / pmax) : 0.0;
    s.ok = (pmax > 0.0) && (pmin > 0.0) && (pmin >= pivot_floor_rel * pmax);
    return s;
}

/// @f$\mathrm{diag}(M^{-1})@f$, in the input order, from the sparse LDLᵀ of @f$M@f$:
/// Takahashi's selected inversion on the factor's own pattern.
///
/// Eigen factors @f$PMP^\top=LDL^\top@f$ with @f$L@f$ unit lower triangular.  With
/// @f$Z=(LDL^\top)^{-1}@f$, the relation @f$L^\top Z=D^{-1}L^{-1}@f$ (lower triangular on
/// the right) gives, column by column from the last,
/// @f$Z_{ij}=-\sum_{k>j}L_{kj}Z_{ik}@f$ for @f$L_{ij}\ne0@f$ and
/// @f$Z_{jj}=1/D_j-\sum_{k>j}L_{kj}Z_{kj}@f$.  Every @f$Z_{ik}@f$ needed lies on the
/// pattern of @f$L@f$ (the rows of one column of the factor are pairwise connected after
/// elimination), so only the pattern's entries are formed: the cost is that of the
/// factorization, not of a dense inverse.
Eigen::VectorXd inverseDiagonal(const SparseLdlt& ldlt) {
    const Eigen::SparseMatrix<double>& L = ldlt.matrixL().nestedExpression();
    const Eigen::VectorXd D = ldlt.vectorD();
    const Eigen::Index n = D.size();

    // The strictly lower entries, column by column with the rows ascending (a copy, so
    // the recurrence does not depend on the factor's internal storage order).
    std::vector<int> colptr(static_cast<std::size_t>(n) + 1, 0);
    std::vector<int> rows;
    std::vector<double> lval;
    rows.reserve(static_cast<std::size_t>(L.nonZeros()));
    lval.reserve(static_cast<std::size_t>(L.nonZeros()));
    for (Eigen::Index j = 0; j < n; ++j) {
        std::vector<std::pair<int, double>> col;
        for (Eigen::SparseMatrix<double>::InnerIterator it(L, j); it; ++it)
            if (it.row() > j) col.emplace_back(static_cast<int>(it.row()), it.value());
        std::sort(col.begin(), col.end());
        for (const auto& [r, v] : col) {
            rows.push_back(r);
            lval.push_back(v);
        }
        colptr[static_cast<std::size_t>(j) + 1] = static_cast<int>(rows.size());
    }

    std::vector<double> zval(rows.size(), 0.0);   // Z on the strictly lower pattern
    Eigen::VectorXd zdiag(n);
    // Z(r, c) for r != c, both beyond the current column (already formed).
    auto zAt = [&](int r, int c) -> double {
        if (r < c) std::swap(r, c);
        const auto b = rows.begin() + colptr[static_cast<std::size_t>(c)];
        const auto e = rows.begin() + colptr[static_cast<std::size_t>(c) + 1];
        const auto it = std::lower_bound(b, e, r);
        if (it == e || *it != r) {
            throw std::logic_error("NetworkNormalEquations::foldCycleRowsDetached: the sparse "
                                   "factor's pattern is not closed under elimination");
        }
        return zval[static_cast<std::size_t>(it - rows.begin())];
    };
    for (Eigen::Index j = n - 1; j >= 0; --j) {
        const int pb = colptr[static_cast<std::size_t>(j)];
        const int pe = colptr[static_cast<std::size_t>(j) + 1];
        for (int p = pb; p < pe; ++p) {
            const int i = rows[static_cast<std::size_t>(p)];
            double s = 0.0;
            for (int q = pb; q < pe; ++q) {
                const int k = rows[static_cast<std::size_t>(q)];
                s += lval[static_cast<std::size_t>(q)] * (k == i ? zdiag(i) : zAt(i, k));
            }
            zval[static_cast<std::size_t>(p)] = -s;
        }
        double s = 0.0;
        for (int p = pb; p < pe; ++p)
            s += lval[static_cast<std::size_t>(p)] * zval[static_cast<std::size_t>(p)];
        zdiag(j) = 1.0 / D(j) - s;
    }

    // Back to the input order: (P M P^T)(P(k), P(k)) = M(k, k), P(k) = indices(k).
    const auto& P = ldlt.permutationP();
    if (P.size() == 0) return zdiag;
    Eigen::VectorXd out(n);
    for (Eigen::Index k = 0; k < n; ++k) out(k) = zdiag(P.indices()(k));
    return out;
}

/// One term of a block, grouped under its station: the block row, the local column and
/// the signed coefficient.
struct StationTerm {
    int          row;
    Eigen::Index col;
    double       coeff;
};

/// @f$D\,(DAD)^{-1}D\,R@f$ for a dense right-hand side of any column count.
Eigen::MatrixXd solveScaled(const NetworkNormalEquations::DetachedFoldFactor& f,
                            const Eigen::VectorXd& scale, const Eigen::MatrixXd& R) {
    const Eigen::MatrixXd SR = scale.asDiagonal() * R;
    const Eigen::MatrixXd Y  = f.ldlt.solve(SR);
    return scale.asDiagonal() * Y;
}

}  // namespace

NetworkNormalEquations::DetachedFold NetworkNormalEquations::foldCycleRowsDetached(
    const std::vector<std::vector<CycleRow>>& blocks, double pivot_floor_rel) const {
    DetachedFold out;
    out.dN = Eigen::MatrixXd::Zero(n_shared_, n_shared_);
    out.du = Eigen::VectorXd::Zero(n_shared_);
    out.blocks.resize(blocks.size());

    // ---- (1) Validate every input and group each block's terms by station.  Every
    // throw happens here, before anything is factored.  A station touched by two blocks
    // is refused: the blocks' Woodbury corrections each read the unmodified N_rr, so
    // their sum would drop the cross term between them (the block-Jacobi truncation the
    // Card-4 fold accepts and this path does not).
    std::map<std::size_t, std::size_t> owner;   // station -> the block that touches it
    std::vector<std::map<std::size_t, std::vector<StationTerm>>> by_station(blocks.size());
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        const std::vector<CycleRow>& rows = blocks[b];
        for (std::size_t k = 0; k < rows.size(); ++k) {
            const CycleRow& row = rows[k];
            if (row.terms.empty()) {
                std::ostringstream os;
                os << "block " << b << " row " << k << " has no terms";
                failDetached(os.str());
            }
            if (!std::isfinite(row.zhat_m) || !std::isfinite(row.pz_m2) || row.pz_m2 < 0.0) {
                std::ostringstream os;
                os << "block " << b << " row " << k
                   << ": the target must be finite and the hold variance finite and >= 0";
                failDetached(os.str());
            }
            for (const DdPriorBlock::Term& t : row.terms) {
                if (t.station >= n_active_) {
                    throw std::out_of_range("NetworkNormalEquations::foldCycleRowsDetached: "
                                            "station index out of range");
                }
                if (t.local_col < 0 || t.local_col >= stations_[t.station].n_local) {
                    failDetached("local column out of range");
                }
                if (!std::isfinite(t.coeff)) {
                    failDetached("non-finite coefficient");
                }
                const auto [it, fresh] = owner.emplace(t.station, b);
                if (!fresh && it->second != b) {
                    std::ostringstream os;
                    os << "blocks " << it->second << " and " << b << " both touch station "
                       << t.station << " (a station belongs to one block)";
                    failDetached(os.str());
                }
                by_station[b][t.station].push_back(
                    StationTerm{static_cast<int>(k), t.local_col, t.coeff});
            }
        }
    }

    // ---- (2) Per block: assemble, screen B, factor and screen A, fold.
    for (std::size_t b = 0; b < blocks.size(); ++b) {
        const std::vector<CycleRow>& rows = blocks[b];
        const Eigen::Index K = static_cast<Eigen::Index>(rows.size());
        DetachedFoldBlock& fb = out.blocks[b];
        fb.G      = Eigen::MatrixXd::Zero(n_shared_, K);
        fb.z_free = Eigen::VectorXd::Zero(K);
        fb.zhat   = Eigen::VectorXd(K);
        fb.scale  = Eigen::VectorXd::Ones(K);
        if (K == 0) continue;   // an inert slot: nothing to fold, nothing degenerate
        for (Eigen::Index k = 0; k < K; ++k) fb.zhat(k) = rows[static_cast<std::size_t>(k)].zhat_m;

        // Station by station, ascending (std::map order): only the K_r rows that touch
        // the station are formed.
        std::vector<Eigen::Triplet<double>> trip;
        for (const auto& [si, terms] : by_station[b]) {
            const Station& st = stations_[si];
            // The station's rows, ascending, and their compact index.
            std::vector<int> rows_r;
            rows_r.reserve(terms.size());
            for (const StationTerm& t : terms) rows_r.push_back(t.row);
            std::sort(rows_r.begin(), rows_r.end());
            rows_r.erase(std::unique(rows_r.begin(), rows_r.end()), rows_r.end());
            const Eigen::Index Kr = static_cast<Eigen::Index>(rows_r.size());
            auto compact = [&rows_r](int row) {
                return static_cast<Eigen::Index>(
                    std::lower_bound(rows_r.begin(), rows_r.end(), row) - rows_r.begin());
            };
            Eigen::MatrixXd CrT = Eigen::MatrixXd::Zero(st.n_local, Kr);   // C_r^T, compact
            for (const StationTerm& t : terms) CrT(t.col, compact(t.row)) += t.coeff;

            const Eigen::MatrixXd Y  = st.Nrr_ldlt.solve(CrT);              // N_rr^-1 C_r^T
            Eigen::MatrixXd Br = CrT.transpose() * Y;                         // C_r N_rr^-1 C_r^T
            Br = (0.5 * (Br + Br.transpose())).eval();
            const Eigen::MatrixXd GY = st.N_sr * Y;                           // N_sr N_rr^-1 C_r^T
            const Eigen::VectorXd zr = CrT.transpose() * st.Nrr_ldlt.solve(st.u_r);
            for (Eigen::Index j = 0; j < Kr; ++j) {
                const Eigen::Index kj = rows_r[static_cast<std::size_t>(j)];
                fb.G.col(kj) += GY.col(j);
                fb.z_free(kj) += zr(j);
                for (Eigen::Index i = 0; i < Kr; ++i)
                    trip.emplace_back(rows_r[static_cast<std::size_t>(i)], kj, Br(i, j));
            }
        }
        // The independence screen, on B alone: B is singular exactly when the rows are
        // linearly dependent, and a positive hold would hide that from the screen on A
        // (a repeated row would double its hold; a contradictory dependent set would be
        // averaged into the held solve).  Dependent rows are a placement error: the block
        // folds nothing and its rows stay float for the epoch.
        Eigen::SparseMatrix<double> B(K, K);
        B.setFromTriplets(trip.begin(), trip.end());   // duplicates add: B = sum_r C_r Y
        {
            SparseLdlt ldlt_b;
            const Screened sb = factorScreened(B, pivot_floor_rel, ldlt_b);
            fb.b_min_pivot_rel = sb.min_pivot_rel;
            if (!sb.ok) {
                fb.dependent  = true;
                fb.degenerate = true;
                continue;
            }
        }

        for (Eigen::Index k = 0; k < K; ++k)
            trip.emplace_back(k, k, rows[static_cast<std::size_t>(k)].pz_m2);  // P^z (diagonal)
        Eigen::SparseMatrix<double> A(K, K);
        A.setFromTriplets(trip.begin(), trip.end());   // duplicates add: A = P^z + B

        auto f = std::make_shared<DetachedFoldFactor>();
        const Screened sa = factorScreened(A, pivot_floor_rel, f->ldlt);
        fb.scale         = sa.scale;
        fb.min_pivot_rel = sa.min_pivot_rel;
        if (!sa.ok) {
            fb.degenerate = true;   // folds nothing; its rows stay float for the epoch
            continue;
        }
        fb.factor_nnz = static_cast<long long>(f->ldlt.matrixL().nestedExpression().nonZeros());

        // The block's increments: X = A^-1 G^T (one sparse solve per shared column), then
        // dN_b = G X and du_b = G A^-1 (z_free - zhat), kept per block (a caller that
        // tests one block alone folds its increments alone) and summed.  X and
        // diag(A^-1) = D diag((DAD)^-1) D stay with the block for the held solve's
        // statistics.
        fb.X  = solveScaled(*f, fb.scale, fb.G.transpose());
        fb.ainv_diag = fb.scale.cwiseAbs2().cwiseProduct(inverseDiagonal(f->ldlt));
        fb.dN = fb.G * fb.X;
        fb.dN = (0.5 * (fb.dN + fb.dN.transpose())).eval();
        fb.du = fb.G * solveScaled(*f, fb.scale, fb.z_free - fb.zhat);
        out.dN += fb.dN;
        out.du += fb.du;
        fb.factor = std::move(f);
    }
    return out;
}

Eigen::VectorXd NetworkNormalEquations::solveFoldBlock(const DetachedFoldBlock& b,
                                                       const Eigen::VectorXd& r) {
    if (!b.factor) {
        throw std::logic_error("NetworkNormalEquations::solveFoldBlock: the block has no "
                               "factor (degenerate or empty)");
    }
    if (r.size() != b.scale.size()) {
        throw std::invalid_argument("NetworkNormalEquations::solveFoldBlock: size mismatch");
    }
    return solveScaled(*b.factor, b.scale, r);
}

}  // namespace GnssProcessor

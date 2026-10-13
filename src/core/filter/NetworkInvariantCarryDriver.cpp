/**
 * @file NetworkInvariantCarryDriver.cpp
 * @brief Implementation of the DD invariant-carry driver (card 4, M1 phase C3b).
 *        The design rationale is in NetworkInvariantCarryDriver.hpp.
 */

#include "filter/NetworkInvariantCarryDriver.hpp"

#include "filter/NetworkArcGraph.hpp"   // Dsu, singleTreeOf, fundamentalCycle, promoteBridgingArcs, chordRestriction (card AGT)

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkRunEnd.hpp"
#include "filter/NetworkRunGroups.hpp"
#include <memory>
#include <spdlog/spdlog.h>

namespace GnssProcessor {

namespace {

/// RAII wall-clock accumulator, the same as NetworkNormalEquations' ScopedAccum: adds the
/// elapsed ms to @p acc on scope exit when @p on is set.  No clock reads when off. (INC-C0)
struct ScopedAccum {
    bool on;
    double* acc;
    std::chrono::steady_clock::time_point t0;
    ScopedAccum(bool on_, double* acc_) : on(on_), acc(acc_) {
        if (on) t0 = std::chrono::steady_clock::now();
    }
    ~ScopedAccum() {
        if (on)
            *acc += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
    }
};

/// Unit-normalized (NL, WL) lane rotation for a band pair.  The rows are orthonormal lane
/// directions in (z₁, z₂) meter space: row 0 is the phase-determined (NL) direction, row 1
/// the WL lane, i.e. the unit-normalized WL functional (1/λ₁, −1/λ₂).  RᵀR = I up to
/// floating point, so Rᵀ·diag(q_nl, q_wl)·R with q_nl == q_wl reduces to the isotropic
/// diagonal and the §6.1 equivalence case needs no special handling. (WL-3 §3.1)
inline Eigen::Matrix2d laneRotation(double lambda1, double lambda2) {
    const double a = 1.0 / lambda1, b = 1.0 / lambda2;
    const double n = std::sqrt(a * a + b * b);
    Eigen::Matrix2d R;
    R << b / n,  a / n,    // NL (⊥ the WL functional)
         a / n, -b / n;    // WL lane
    return R;
}

/// One WL cycle in the unit-normalized lane coordinate, 1/‖(1/λ₁,−1/λ₂)‖ state-meters.
/// Converts @c Config::sigma_wl_fresh_cyc to a σ in state units. (WL-3)
inline double laneMetersPerCycle(double lambda1, double lambda2) {
    const double a = 1.0 / lambda1, b = 1.0 / lambda2;
    return 1.0 / std::sqrt(a * a + b * b);
}

/// The W row of one new chord as the fold reads it: the coefficient of each old chord in the
/// chord's fundamental cycle (arcgraph::chordRestriction), in old-chord order, as doubles.
Eigen::VectorXd wRowOf(const std::map<ArcKey, int>& cycle, const std::vector<ArcKey>& old_chords) {
    const std::vector<int> coeff = arcgraph::chordRestriction(cycle, old_chords);
    Eigen::VectorXd w = Eigen::VectorXd::Zero(static_cast<Eigen::Index>(coeff.size()));
    for (std::size_t j = 0; j < coeff.size(); ++j)
        w(static_cast<Eigen::Index>(j)) = static_cast<double>(coeff[j]);
    return w;
}

// --- Carry-blob (de)serialization helpers (RTB-7).  Self-contained: the snapshot
//     layer stores the CARRY..CEND blob verbatim, as it does the emitter blob. ---
std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = line.find(',', start);
        if (pos == std::string::npos) { out.push_back(line.substr(start)); break; }
        out.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}
bool parseLL(const std::string& s, long long& v) {
    if (s.empty()) return false;
    errno = 0; char* end = nullptr;
    v = std::strtoll(s.c_str(), &end, 10);
    return end == s.c_str() + s.size() && errno != ERANGE;
}
bool parseDbl(const std::string& s, double& v) {
    if (s.empty()) return false;
    errno = 0; char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && errno != ERANGE;
}
std::string chomp(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

/// Equilibrated positive-definiteness screen of §4(b), applied to a bare carried
/// @f$P^z@f$ (no data information @f$B@f$ yet).  See docs/SPEC_pd_safe_dd_fold.md §5c
/// (PD-safe INC-2c).
///
/// It has the same form as the fold's screen, so the two agree on what "PD" means: scale
/// each row/col by @f$1/\sqrt{P_{ii}}@f$ so every pivot is O(1), then require
/// @f$d_{\min}\ge 10^{-12} d_{\max}@f$.  A non-positive or non-finite diagonal entry is
/// rejected before any factorization; corrupt blobs written before the PD-safe fix carry
/// negative diagonals.
///
/// This is a predicate, not a repair.  At restore the caller responds with a counted
/// rebirth (means kept, covariance re-seeded diffuse) rather than patching a covariance
/// of doubtful provenance.
bool carriedBlockIsPd(const Eigen::MatrixXd& P) {
    const Eigen::Index K = P.rows();
    if (K == 0) return true;              // a chordless block has nothing to violate
    if (P.cols() != K) return false;
    if (!P.allFinite()) return false;
    const Eigen::VectorXd dg = P.diagonal();
    const double dmax_abs = dg.cwiseAbs().maxCoeff();
    if (!(dmax_abs > 0.0)) return false;  // an all-zero Pᶻ carries no information either
    const double eps = 1e-16 * dmax_abs;
    Eigen::VectorXd s(K);
    for (Eigen::Index i = 0; i < K; ++i) {
        if (!(dg(i) > 0.0)) return false;
        s(i) = 1.0 / std::sqrt(std::max(dg(i), eps));
    }
    Eigen::MatrixXd At = s.asDiagonal() * P * s.asDiagonal();
    At = (0.5 * (At + At.transpose())).eval();
    const Eigen::LDLT<Eigen::MatrixXd> ldlt(At);
    if (ldlt.info() != Eigen::Success) return false;
    const Eigen::VectorXd d = ldlt.vectorD();
    const double dmax = d.cwiseAbs().maxCoeff();
    const double dmin = d.minCoeff();
    return (dmax > 0.0) && (dmin > 0.0) && (dmin >= 1e-12 * dmax);
}

}  // namespace

void NetworkInvariantCarryDriver::resetCarryState_() {
    carried_.clear();
    block_meta_.clear();
    last_innov_flags_.clear();
    last_innov_sigmas_.clear();
    last_innov_terms_.clear();
    gate_exempt_.clear();
    innov_flat_.clear();       // corroboration set is cleared with the carry (D5 v3)
    lane_fix_w_.clear();       // fold-on-change memory is cleared with the carry (WL-3)
    lane_fix_staged_.clear();
    lane_fix_epoch_.clear();   // skip grace clock is cleared with the carry (LF-1 INC-2)
    install_epoch_index_ = 0;
}

void NetworkInvariantCarryDriver::reset() {
    resetCarryState_();
    // reset() clears the carry but keeps the run counters.  It is reachable mid-run from
    // the install seam's catch around installPosterior; clearing counters_ there would
    // make the run-end totals under-report and turn the seam's per-epoch churn deltas
    // (cumulative − previous snapshot) negative for that epoch (invariant #4).
    //
    // z_dim is not a tally but the live dimension of the carry, so it is zeroed with the
    // now-empty carry.  The run maxima (max_z_dim / max_component_kc) are history and stay.
    counters_.z_dim = 0;
}

void NetworkInvariantCarryDriver::serializeCarry(std::ostream& os, bool diagonal_only) const {
    // Header v3 means the Pᶻ rows are one PD diagonal row; v2 means dense P rows.  The
    // version is informational: restoreCarry dispatches on the row tag (P or PD), so a
    // blob of either shape restores under either knob setting.  With diagonal_only off
    // the blob is byte-identical to the blob written before RTB-7(e). (RTB-7(e))
    os << (diagonal_only ? "CARRY,v3," : "CARRY,v2,") << carried_.size() << "\n";
    // S line: the D5 σ̂₀² inflation state, so a resume does not re-run the pre-warmup
    // raw-formal gate against a full set of restored chords.  Written under either header,
    // only when calibration is armed and the accumulator holds rows; without it the
    // restored accumulator starts from its seed. (RE-1)
    if (inflation_) {
        const NetworkCarryVce::Estimate est =
            inflation_->estimate(kInflationFamily, kInflationSat);
        if (est.n_eff > 0.0 && est.robust_floor > 0.0)
            os << fmt::format("S,{},{}\n", est.robust_floor, est.n_eff);
    }
    for (const CarriedBlock& b : carried_) {
        const Eigen::Index zdim = b.z.size();
        os << fmt::format("B,{},{},{},{},{},{},{},{}\n", b.group, b.pair ? 1 : 0,
                          b.lambda1, b.lambda2, static_cast<long long>(zdim),
                          static_cast<long long>(b.chords.size()),
                          static_cast<long long>(b.vertices.size()),
                          static_cast<long long>(b.arcs.size()));
        { std::string ln = "Z";
          for (Eigen::Index i = 0; i < zdim; ++i) ln += "," + fmt::format("{}", b.z(i));
          os << ln << "\n"; }
        // ZL row: the linearization DD beside the mean.  ẑ is the correction-frame DD, so
        // it has no meaning against a different x_lin, and a resumed run re-seeds every
        // station ambiguity from code.  Written only when the harvest captured it
        // (Config::carry_lin, so zl is sized like z) and only for zdim > 0, the same shape
        // rule as the PD row, so the two stay line-for-line aligned.  A reader that finds
        // no ZL row carries the block unchanged, so blobs without the row still restore.
        // (RS-2)
        if (zdim > 0 && b.zl.size() == zdim) {
            std::string ln = "ZL";
            for (Eigen::Index i = 0; i < zdim; ++i) ln += "," + fmt::format("{}", b.zl(i));
            os << ln << "\n";
        }
        if (diagonal_only) {
            // One row of zdim numbers, so v3 grows with Σ K_c rather than Σ K_c².  A
            // zdim == 0 block writes no row, as the dense path does (its loop is empty),
            // so the two shapes stay line-for-line aligned there.
            if (zdim > 0) {
                std::string ln = "PD";
                for (Eigen::Index r = 0; r < zdim; ++r) ln += "," + fmt::format("{}", b.Pz(r, r));
                os << ln << "\n";
            }
        } else {
            for (Eigen::Index r = 0; r < zdim; ++r) {
                std::string ln = "P";
                for (Eigen::Index c = 0; c < zdim; ++c) ln += "," + fmt::format("{}", b.Pz(r, c));
                os << ln << "\n";
            }
        }
        { std::string ln = "H";
          for (const ArcKey& a : b.chords) ln += fmt::format(",{},{}", a.u, a.v);
          os << ln << "\n"; }
        { std::string ln = "X";
          for (int v : b.vertices) ln += fmt::format(",{}", v);
          os << ln << "\n"; }
        { std::string ln = "A";
          for (const ArcKey& a : b.arcs) ln += fmt::format(",{},{}", a.u, a.v);
          os << ln << "\n"; }
    }
    os << "CEND\n";
}

bool NetworkInvariantCarryDriver::restoreCarry(std::istream& is, std::string* err) {
    auto fail = [&](std::string m) { if (err) *err = std::move(m); return false; };
    std::string line;
    if (!std::getline(is, line)) return fail("empty carry blob");
    line = chomp(line);
    if (line.rfind("CARRY,", 0) != 0) return fail("bad carry header: '" + line + "'");
    // This is a general "install this carry" API, so it starts from a clean driver: no
    // lane-fix, gate-exemption or innovation side-state survives the install.  The
    // counters are also cleared here, and only here.  reset() alone keeps the run tallies
    // (it is a mid-run carry cold start), whereas an installed carry starts a new count,
    // and tallies from earlier driver work would otherwise be attributed to it. (RE-4)
    reset();
    counters_ = Counters{};

    double seed_sigma0_sq = 0.0, seed_rows = 0.0;   // from the optional S line (RE-1)
    bool   saw_seed_line = false;                   // an S line was present (RE-1)
    long long n_diag_blocks = 0;                    // blocks read from a PD row (RTB-7(e))
    long long n_rebirth_blocks = 0;                 // blocks rebirthed at restore (PD-safe INC-2c)
    long long n_lin_blocks = 0;                     // blocks read with a ZL row, re-basable (RS-2)
    std::vector<CarriedBlock> restored;
    while (std::getline(is, line)) {
        line = chomp(line);
        if (line == "CEND") {
            carried_ = std::move(restored);
            // Seed the σ̂₀² accumulator so the D5 gate uses the restored inflation at
            // once.  With no S line or calibration off, it starts from its seed. (RE-1)
            resume_sigma0_seeded_ = false;
            resume_sigma0_value_  = 1.0;
            // Record what the blob offered as well as whether it was taken: the bool
            // alone cannot distinguish cold start, calibration off, no S line and a
            // rejected seed.  See ResumeSeedState. (RE-1)
            resume_seed_line_ = saw_seed_line;
            resume_seed_sq_   = seed_sigma0_sq;
            resume_seed_rows_ = seed_rows;
            if (inflation_ && seed_sigma0_sq > 0.0 && seed_rows > 0.0) {
                inflation_->seedRobustFloor(kInflationFamily, kInflationSat,
                                            seed_sigma0_sq, seed_rows,
                                            static_cast<long long>(seed_rows));
                resume_sigma0_seeded_ = true;
                resume_sigma0_value_  = std::sqrt(seed_sigma0_sq);
            }
            resume_restored_ = true;
            resume_chords_   = static_cast<long long>(totalZDim());
            // Count diagonal restores (invariant #4).  Without this count a diagonal restore
            // reports as warm and branch-faithful, with normal carry counters, while the
            // chord cross-covariances are gone. (RTB-7(e))
            resume_carry_diag_blocks_ = n_diag_blocks;
            resume_carry_rebirth_blocks_ = n_rebirth_blocks;   // rebirths (PD-safe INC-2c)
            resume_carry_lin_blocks_ = n_lin_blocks;           // ZL-row blocks (RS-2)
            return true;
        }
        if (line.rfind("S,", 0) == 0) {             // optional, either header (RE-1)
            const std::vector<std::string> sf = splitCsv(line);
            if (sf.size() != 3 || !parseDbl(sf[1], seed_sigma0_sq)
                || !parseDbl(sf[2], seed_rows))
                return fail("malformed S (sigma0) line: '" + line + "'");
            saw_seed_line = true;
            continue;
        }
        // Component header.
        const std::vector<std::string> f = splitCsv(line);
        if (f.size() != 9 || f[0] != "B") return fail("expected B header, got: '" + line + "'");
        long long grp = 0, pr = 0, zdim = 0, nch = 0, nvert = 0, narc = 0;
        double l1 = 0, l2 = 0;
        if (!parseLL(f[1], grp) || !parseLL(f[2], pr) || !parseDbl(f[3], l1)
            || !parseDbl(f[4], l2) || !parseLL(f[5], zdim) || !parseLL(f[6], nch)
            || !parseLL(f[7], nvert) || !parseLL(f[8], narc) || zdim < 0)
            return fail("malformed B header: '" + line + "'");
        CarriedBlock b;
        b.group   = static_cast<int>(grp);
        b.pair    = (pr != 0);
        b.lambda1 = l1;
        b.lambda2 = l2;
        // Z mean row.
        if (!std::getline(is, line)) return fail("carry: truncated at Z");
        const std::vector<std::string> zf = splitCsv(chomp(line));
        if (zf.empty() || zf[0] != "Z" || static_cast<long long>(zf.size()) != zdim + 1)
            return fail("carry: malformed Z row");
        b.z.resize(zdim);
        for (long long i = 0; i < zdim; ++i)
            if (!parseDbl(zf[static_cast<std::size_t>(i + 1)], b.z(i)))
                return fail("carry: bad Z value");
        // Optional ZL row: the writer's linearization DD, Σ c·x_lin.  One line of lookahead:
        // read the next line once and hand it to the Pᶻ parse below if it is not a ZL row.
        // A ZL row marks the block unbased (to be re-based against the live x_lin); without
        // one the block stays based and is carried unchanged, as there is nothing to re-base
        // from. (RS-2)
        bool have_lookahead = false;
        if (zdim > 0) {
            if (!std::getline(is, line)) return fail("carry: truncated after Z");
            std::vector<std::string> lf = splitCsv(chomp(line));
            if (!lf.empty() && lf[0] == "ZL") {
                if (static_cast<long long>(lf.size()) != zdim + 1)
                    return fail("carry: malformed ZL row");
                b.zl.resize(zdim);
                for (long long i = 0; i < zdim; ++i)
                    if (!parseDbl(lf[static_cast<std::size_t>(i + 1)], b.zl(i)))
                        return fail("carry: bad ZL value");
                b.unbased = true;
                ++n_lin_blocks;
            } else {
                have_lookahead = true;   // `line` already holds the P / PD row
            }
        }
        // Pᶻ: either zdim dense "P" rows (header v2) or one "PD" diagonal row (header v3,
        // RTB-7(e)).  The dispatch is on the row tag, not on the header version: the blob is
        // self-describing, so a diagonal blob restores in a run configured to write dense
        // ones and vice versa, and A/B arms can differ only in their seed bytes.  A PD
        // restore starts every chord cross-covariance at zero; the branch means are exact
        // and the covariance between them is rebuilt from data.
        b.Pz.setZero(zdim, zdim);
        if (zdim > 0) {
            if (!have_lookahead && !std::getline(is, line))
                return fail("carry: truncated at P");
            std::vector<std::string> pf = splitCsv(chomp(line));
            if (!pf.empty() && pf[0] == "PD") {
                if (static_cast<long long>(pf.size()) != zdim + 1)
                    return fail("carry: malformed PD row");
                for (long long c = 0; c < zdim; ++c)
                    if (!parseDbl(pf[static_cast<std::size_t>(c + 1)], b.Pz(c, c)))
                        return fail("carry: bad PD value");
                ++n_diag_blocks;
            } else {
                for (long long r = 0; r < zdim; ++r) {
                    if (r > 0) {
                        if (!std::getline(is, line)) return fail("carry: truncated at P");
                        pf = splitCsv(chomp(line));
                    }
                    if (pf.empty() || pf[0] != "P"
                        || static_cast<long long>(pf.size()) != zdim + 1)
                        return fail("carry: malformed P row");
                    for (long long c = 0; c < zdim; ++c)
                        if (!parseDbl(pf[static_cast<std::size_t>(c + 1)], b.Pz(r, c)))
                            return fail("carry: bad P value");
                }
            }
            // --- Restore guard (PD-safe INC-2c, SPEC §5c) ------------------------
            // A carry blob written before the PD-safe fix can hold a corrupt joint lane
            // block: an indefinite Pᶻ with negative diagonals and entries larger than the
            // diffuse prior.  Restoring it verbatim would carry that state into the resumed
            // run.  Such a block is rebirthed instead: the means are kept (the branch,
            // i.e. which integer realization the phase side landed on, lives in ẑ), the
            // covariance is reset to the fresh diffuse diagonal, and the rebirth is counted
            // (invariant #7).
            if (cfg_.pd_safe && !carriedBlockIsPd(b.Pz)) {
                b.Pz.setZero(zdim, zdim);
                b.Pz.diagonal().setConstant(cfg_.fresh_prior_var);
                ++n_rebirth_blocks;
            }
        }
        // Chords (H), vertices (X), arcs (A).
        auto readPairs = [&](char tag, long long npair, std::vector<ArcKey>* vec,
                             std::set<ArcKey>* setv) -> bool {
            if (!std::getline(is, line)) return false;
            const std::vector<std::string> ff = splitCsv(chomp(line));
            if (ff.empty() || ff[0].size() != 1 || ff[0][0] != tag
                || static_cast<long long>(ff.size()) != 2 * npair + 1)
                return false;
            for (long long i = 0; i < npair; ++i) {
                long long u = 0, v = 0;
                if (!parseLL(ff[static_cast<std::size_t>(2 * i + 1)], u)
                    || !parseLL(ff[static_cast<std::size_t>(2 * i + 2)], v))
                    return false;
                const ArcKey k(static_cast<int>(u), static_cast<int>(v));
                if (vec) vec->push_back(k);
                if (setv) setv->insert(k);
            }
            return true;
        };
        if (!readPairs('H', nch, &b.chords, nullptr)) return fail("carry: malformed H row");
        // Vertices (single ints).
        if (!std::getline(is, line)) return fail("carry: truncated at X");
        const std::vector<std::string> xf = splitCsv(chomp(line));
        if (xf.empty() || xf[0] != "X" || static_cast<long long>(xf.size()) != nvert + 1)
            return fail("carry: malformed X row");
        for (long long i = 0; i < nvert; ++i) {
            long long v = 0;
            if (!parseLL(xf[static_cast<std::size_t>(i + 1)], v)) return fail("carry: bad X value");
            b.vertices.insert(static_cast<int>(v));
        }
        if (!readPairs('A', narc, nullptr, &b.arcs)) return fail("carry: malformed A row");
        restored.push_back(std::move(b));
    }
    return fail("carry blob missing CEND");
}

bool NetworkInvariantCarryDriver::validateCarryBinding(int n_stations,
                                                       const std::set<int>& sat_vertices,
                                                       const std::set<int>& group_ids,
                                                       std::string* err) const {
    auto bad = [&](std::string m) { if (err) *err = std::move(m); return false; };
    auto vertexOk = [&](int w) {
        return (w >= 0 && w < n_stations) || sat_vertices.count(w) != 0;
    };
    for (const CarriedBlock& b : carried_) {
        if (group_ids.count(b.group) == 0)
            return bad(fmt::format("carried block group {} is not in the live "
                                   "carry_group_id map ({} entries)",
                                   b.group, group_ids.size()));
        for (const ArcKey& c : b.chords)
            if (!vertexOk(c.u) || !vertexOk(c.v))
                return bad(fmt::format("chord ({},{}) in group {} does not bind: neither a "
                                       "roster station [0,{}) nor a live sat vertex ({} known)",
                                       c.u, c.v, b.group, n_stations, sat_vertices.size()));
        for (const ArcKey& a : b.arcs)
            if (!vertexOk(a.u) || !vertexOk(a.v))
                return bad(fmt::format("arc ({},{}) in group {} does not bind: neither a "
                                       "roster station [0,{}) nor a live sat vertex ({} known)",
                                       a.u, a.v, b.group, n_stations, sat_vertices.size()));
    }
    return true;
}

std::size_t NetworkInvariantCarryDriver::applyResumeShock(double var_m2) {
    if (!(var_m2 > 0.0)) return 0;              // off: byte-identical
    std::size_t n = 0;
    for (CarriedBlock& b : carried_) {
        if (b.Pz.rows() <= 0) continue;
        // Diagonal only: the restored means and the cross-covariance geometry (the
        // branch) are untouched; each carried DD's own variance is widened for the
        // station-layer re-convergence window.
        b.Pz.diagonal().array() += var_m2;
        ++n;
    }
    resume_shock_sigma_m_ = std::sqrt(var_m2);
    return n;
}

void NetworkInvariantCarryDriver::beginEpoch(double dt_s, double content_s, bool clean) {
    // Advance the σ̂₀² accumulator's exponential forgetting once per epoch, before this
    // epoch's rows are folded in installPosterior.  Independent of q_z. (D5 v2)
    if (inflation_) inflation_->beginEpoch(content_s, clean);
    if (cfg_.qz <= 0.0 || dt_s <= 0.0) return;   // constant DDs (default): no q_z step
    ScopedAccum _tu(perf_timing_, &perf_timers_.time_update_ms);   // INC-C0
    const double add = cfg_.qz * dt_s;
    // Lane-aware process noise on pair blocks: Q = Rᵀ·diag(q_nl, q_wl)·R per chord
    // (q_nl = qz, q_wl = q_wl_scale·qz).  At q_wl_scale == 1 this equals the isotropic
    // diagonal, so the plain add is used (byte-identical to the per-band shape). (WL-3 §3.1)
    const bool lane_q = cfg_.lane_joint && cfg_.q_wl_scale != 1.0;
    for (auto& cb : carried_) {
        if (cb.Pz.rows() == 0) continue;
        if (!lane_q || !cb.pair || !(cb.lambda1 > 0.0 && cb.lambda2 > 0.0)) {
            cb.Pz.diagonal().array() += add;
            continue;
        }
        const Eigen::Index K = static_cast<Eigen::Index>(cb.chords.size());
        if (cb.Pz.rows() != 2 * K) { cb.Pz.diagonal().array() += add; continue; }  // defensive
        const Eigen::Matrix2d R = laneRotation(cb.lambda1, cb.lambda2);
        const Eigen::Matrix2d Q = R.transpose()
            * Eigen::Vector2d(cfg_.qz, cfg_.qz * cfg_.q_wl_scale).asDiagonal()
            * R * dt_s;
        for (Eigen::Index k = 0; k < K; ++k) {
            cb.Pz(k, k)         += Q(0, 0);
            cb.Pz(K + k, K + k) += Q(1, 1);
            cb.Pz(k, K + k)     += Q(0, 1);
            cb.Pz(K + k, k)     += Q(1, 0);
        }
    }
}

double NetworkInvariantCarryDriver::innovInflationSigma0() const {
    return innovInflationStatus().sigma0;
}

const char* NetworkInvariantCarryDriver::inflationStateName(InflationState s) {
    switch (s) {
        case InflationState::Off:     return "off";
        case InflationState::WarmUp:  return "warmup";
        case InflationState::Floored: return "floored";
        case InflationState::Capped:  return "capped";
        case InflationState::Active:  return "active";
    }
    return "?";
}

// Returns σ̂₀ together with the state that produced it.  The arithmetic (branches, clamp,
// order of operations) is the same as the σ̂₀-only computation it replaced, so σ̂₀ is
// unchanged.  A σ̂₀ of 1.00 alone is ambiguous: three degraded states and the active state
// can all report it (see InflationState). (D5 v2)
NetworkInvariantCarryDriver::InflationStatus
NetworkInvariantCarryDriver::innovInflationStatus() const {
    InflationStatus st;
    if (!inflation_) return st;                        // Off, σ̂₀ = 1 (the D5 v1 gate by config)
    const NetworkCarryVce::Estimate est = inflation_->estimate(kInflationFamily, kInflationSat);
    st.n_eff         = est.n_eff;
    st.raw_sigma0_sq = est.robust_floor;
    if (!(est.n_eff >= kInflationWarmupRows) || !(est.robust_floor > 0.0)) {
        st.state = InflationState::WarmUp;             // warm-up: raw formal gate (as D5 v1)
        return st;                                     // σ̂₀ stays 1.0
    }
    double f2 = est.robust_floor;                       // σ̂₀² = geo_mean(sᵢ²)/χ²₁-geomean
    f2 = std::min(std::max(f2, 1.0), kInflationFactorCap);  // only raises the threshold; caps a diverged σ̂₀
    st.state  = (est.robust_floor < 1.0)                 ? InflationState::Floored
              : (est.robust_floor > kInflationFactorCap) ? InflationState::Capped
                                                         : InflationState::Active;
    st.sigma0 = std::sqrt(f2);
    return st;
}

const char* NetworkInvariantCarryDriver::resumeSeedStateName(ResumeSeedState s) {
    switch (s) {
        case ResumeSeedState::NotRestored:    return "cold";
        case ResumeSeedState::CalibrationOff: return "calib-off";
        case ResumeSeedState::NoSeedLine:     return "no-S-line";
        case ResumeSeedState::Rejected:       return "rejected";
        case ResumeSeedState::Seeded:         return "seeded";
    }
    return "?";
}

// Readout over the state restoreCarry recorded; it decides nothing and does not touch the
// seed arithmetic.  The branches follow the causal order: with no blob there is nothing to
// seed from; with no accumulator there is nothing to seed into (and the gate is the D5 v1
// gate by config whatever the blob held); only then does the blob's content decide.
// (RTB-7(d), RE-1)
NetworkInvariantCarryDriver::ResumeSeedStatus
NetworkInvariantCarryDriver::resumeSigma0Status() const {
    ResumeSeedStatus st;
    st.blob_sigma0_sq = resume_seed_sq_;
    st.blob_rows      = resume_seed_rows_;
    if (!resume_restored_)      { st.state = ResumeSeedState::NotRestored;    return st; }
    if (!inflation_)            { st.state = ResumeSeedState::CalibrationOff; return st; }
    if (!resume_seed_line_)     { st.state = ResumeSeedState::NoSeedLine;     return st; }
    if (!(resume_seed_sq_ > 0.0 && resume_seed_rows_ > 0.0)) {
        st.state = ResumeSeedState::Rejected;                                 return st;
    }
    st.state  = ResumeSeedState::Seeded;
    st.sigma0 = resume_sigma0_value_;
    return st;
}

Eigen::Index NetworkInvariantCarryDriver::totalZDim() const {
    Eigen::Index t = 0;
    for (const auto& cb : carried_) t += cb.z.size();
    return t;
}

std::vector<NetworkNormalEquations::DdPriorBlock>
NetworkInvariantCarryDriver::buildDdPriors(const std::vector<GroupInput>& groups) {
    std::vector<NetworkNormalEquations::DdPriorBlock> out;
    block_meta_.clear();
    lane_fix_staged_.clear();   // per-build staging; a RAIM re-solve re-derives it (WL-3)
    ++counters_.epochs;
    if (perf_timing_) ++perf_timers_.build_epochs;   // INC-C0

    // The carry this epoch projects from; `carried_` is rebuilt in installPosterior.
    const std::vector<CarriedBlock>& prev = carried_;
    // Per previous block, the number of new components it contributes to (> 1 on a split).
    std::vector<int> prev_hit(prev.size(), 0);

    // Whether this build needs the per-arc linearization.  The write knob (carry_lin) needs
    // it at every harvest, since serializeCarry stores it; the read knob (carry_rebase) needs
    // it while a restored block is still unbased.  With both off, zl stays empty and none of
    // the linearization code below runs. (RS-2)
    const bool lin_on = cfg_.carry_lin || cfg_.carry_rebase;
    // Build-local re-base tallies, published to counters_ at the end of this call and only
    // when the re-base fired.  The re-base runs on the first build after a restore; a
    // within-epoch rebuild (the RAIM re-solve) recomputes the same numbers from the same
    // `carried_`, and later epochs have nothing unbased left.  Accumulating would therefore
    // double-count, and clearing unconditionally would erase the value the run-end line reports.
    long long rb_n = 0, rb_drop = 0;
    double    rb_max_dz = 0.0;

    for (const GroupInput& g : groups) {
        // --- (1) Assemble this epoch's graph for the group. -------------------
        struct ArcInfo {
            int est_station; int local_col; int local_col2; bool tree_edge;
            double x_lin; double x_lin2;   // this arc's linearization value(s), metres (RS-2)
        };
        std::map<ArcKey, ArcInfo> arcinfo;
        std::set<ArcKey>          slipped_arcs;   // arcs whose carried ambiguity slipped this epoch (C3c)
        // The shared link rule (card AGT): root(rx) under root(sat), in g.arcs order, never sorted.
        arcgraph::Dsu uf;
        std::vector<ArcKey> group_tree_edges;   // for per-component tree extraction
        std::set<ArcKey> slipped2_arcs;   // pair groups: band 2's own slips, see ArcInput::slipped2 (WL-3)
        for (const ArcInput& a : g.arcs) {
            if (a.rx_vertex == a.sat_vertex) continue;         // degenerate (defensive)
            const ArcKey k(a.rx_vertex, a.sat_vertex);
            arcinfo[k] = {a.est_station, a.local_col, a.local_col2, a.tree_edge,
                          a.x_lin, a.x_lin2};
            arcgraph::dsuUnite(uf, a.rx_vertex, a.sat_vertex);
            if (a.tree_edge) group_tree_edges.push_back(k);
            if (a.slipped) slipped_arcs.insert(k);
            if (g.pair && a.slipped2) slipped2_arcs.insert(k);
        }
        if (arcinfo.empty()) continue;
        // A pair chord counts as the two per-band DDs it co-locates, which keeps the
        // lifecycle counters comparable across the regroup (§6.1, WL-3).
        const long long tally = g.pair ? 2 : 1;

        // --- (2) Partition arcs into connected components (root → arcs). ------
        std::map<int, std::vector<ArcKey>> comp_arcs;
        std::map<int, std::set<int>>       comp_vertices;
        for (const auto& [k, info] : arcinfo) {
            const int root = arcgraph::dsuFind(uf, k.u);
            comp_arcs[root].push_back(k);
            comp_vertices[root].insert(k.u);
            comp_vertices[root].insert(k.v);
        }

        // Previous blocks of this group, for the vertex-overlap contributor gather.
        std::vector<std::size_t> group_prev;
        for (std::size_t p = 0; p < prev.size(); ++p)
            if (prev[p].group == g.key) group_prev.push_back(p);

        // --- (3) Reconcile each new component. --------------------------------
        for (auto& [root, arcs] : comp_arcs) {
            const std::set<int>& verts = comp_vertices[root];

            // Tree edges / chords of this component.
            std::vector<ArcKey> tree_edges, chords;
            for (const ArcKey& k : arcs)
                (arcinfo.at(k).tree_edge ? tree_edges : chords).push_back(k);
            // A pair group has no upstream election of its own: its arc set is the
            // intersection of two per-band graphs, and the caller's band-1 tree tags need
            // not span this subgraph (per-(rx,sat) channel heterogeneity fragments the
            // primary pairs across groups).  The tags are used first, then the spanning
            // forest is completed deterministically (ArcKey order) by promoting chords that
            // connect disjoint tree fragments.  Homogeneous regions keep the eq34 basis
            // unchanged (the §6.1 equivalence); fragmented regions stay carried instead of
            // being dropped whole by the tree guards below. (WL-3)
            if (g.pair && !chords.empty())
                counters_.pair_tree_promotions += arcgraph::promoteBridgingArcs(tree_edges, chords);
            if (chords.empty()) continue;   // spanning tree only → no DD to carry

            // The cycle algebra (arcgraph, card AGT): the tree of the component's tree edges.
            // A malformed (non-spanning or cyclic) tree from the election is not representable,
            // so the component is skipped rather than folded with a wrong cycle.
            const std::optional<arcgraph::IntTree> tree = arcgraph::singleTreeOf(tree_edges);
            if (!tree) continue;
            // Every chord's endpoints must be tree-connected (guard fundamentalCycle).
            bool tree_ok = true;
            for (const ArcKey& c : chords)
                if (!tree->vertices.count(c.u) || !tree->vertices.count(c.v)) { tree_ok = false; break; }
            if (!tree_ok) continue;

            // Gather contributing previous blocks (any shared vertex), concatenated
            // block-diagonally into the old basis (they were independent components).
            std::vector<ArcKey> old_chords;
            std::set<ArcKey>    A_old;
            std::vector<const CarriedBlock*> contributors;
            for (std::size_t p : group_prev) {
                const CarriedBlock& ob = prev[p];
                if (ob.pair != g.pair) continue;   // defensive: a shape mismatch cannot project (WL-3)
                bool overlap = false;
                for (int v : ob.vertices) if (verts.count(v)) { overlap = true; break; }
                if (!overlap) continue;
                contributors.push_back(&ob);
                ++prev_hit[p];
                old_chords.insert(old_chords.end(), ob.chords.begin(), ob.chords.end());
                A_old.insert(ob.arcs.begin(), ob.arcs.end());
            }
            if (contributors.size() > 1) counters_.merges += tally;
            const Eigen::Index n_old = static_cast<Eigen::Index>(old_chords.size());
            // Block-diagonal old covariance / mean over the contributors.  Pair blocks are
            // stored [band-1; band-2], so the concatenation keeps the two band halves
            // contiguous: contributor j's band 1 lands at [off, off+k) and its band 2 at
            // [n_old+off, n_old+off+k), with the four Pᶻ sub-blocks placed accordingly.  The
            // cross-band sub-block is zero unless the lane shape retained it. (WL-3)
            const Eigen::Index zdim_old = g.pair ? 2 * n_old : n_old;
            Eigen::VectorXd z_old(zdim_old);
            Eigen::MatrixXd Pz_old = Eigen::MatrixXd::Zero(zdim_old, zdim_old);
            {
                Eigen::Index off = 0;
                for (const CarriedBlock* ob : contributors) {
                    const Eigen::Index k = static_cast<Eigen::Index>(ob->chords.size());
                    if (k == 0) continue;
                    if (!g.pair) {
                        z_old.segment(off, k) = ob->z;
                        Pz_old.block(off, off, k, k) = ob->Pz;
                    } else {
                        z_old.segment(off, k)         = ob->z.head(k);
                        z_old.segment(n_old + off, k) = ob->z.tail(k);
                        Pz_old.block(off, off, k, k)                 = ob->Pz.topLeftCorner(k, k);
                        Pz_old.block(n_old + off, n_old + off, k, k) = ob->Pz.bottomRightCorner(k, k);
                        Pz_old.block(off, n_old + off, k, k)         = ob->Pz.topRightCorner(k, k);
                        Pz_old.block(n_old + off, off, k, k)         = ob->Pz.bottomLeftCorner(k, k);
                    }
                    off += k;
                }
            }
            // --- The contributors' writer-frame linearization DDs, concatenated in the
            // z_old order above (RS-2).  `rebase_here` is true only when the component draws
            // on a restored, unbased block and every contributor carries a usable ZL row.
            // `rebase_blocked` covers the remaining unbased case, where no exact re-base
            // exists; folding such a block would assert the writer's x_lin frame, so its
            // chords are marginalized (born fresh) instead.  A based contributor's ẑ is
            // already in this run's frame, so a component that mixes based and unbased
            // predecessors has no single correction (one new chord's cycle can draw on
            // both).  The condition is therefore all-or-nothing: every contributor unbased
            // and carrying a usable ZL row.  At the first post-resume build every
            // contributor comes from the same restoreCarry, so the mixed case is not expected
            // in practice; it is refused rather than approximated.
            bool any_unbased = false, rebasable = true;
            Eigen::VectorXd zl_old;
            if (cfg_.carry_rebase && n_old > 0) {
                for (const CarriedBlock* ob : contributors) {
                    const Eigen::Index k = static_cast<Eigen::Index>(ob->chords.size());
                    if (k == 0) continue;   // contributes no column to z_old / zl_old
                    if (ob->unbased) any_unbased = true; else rebasable = false;
                    if (ob->zl.size() != (ob->pair ? 2 * k : k)) rebasable = false;
                }
                if (any_unbased && rebasable) {
                    zl_old = Eigen::VectorXd::Zero(zdim_old);
                    Eigen::Index off = 0;
                    for (const CarriedBlock* ob : contributors) {
                        const Eigen::Index k = static_cast<Eigen::Index>(ob->chords.size());
                        if (k == 0) continue;
                        if (!ob->pair) {
                            zl_old.segment(off, k) = ob->zl;
                        } else {
                            zl_old.segment(off, k)         = ob->zl.head(k);
                            zl_old.segment(n_old + off, k) = ob->zl.tail(k);
                        }
                        off += k;
                    }
                }
            }
            const bool rebase_here    = cfg_.carry_rebase && any_unbased && rebasable
                                        && zl_old.size() == zdim_old;
            const bool rebase_blocked = cfg_.carry_rebase && any_unbased && !rebase_here;

            // Map an old chord arc → its column index in the old basis.
            std::map<ArcKey, Eigen::Index> old_col;
            for (Eigen::Index j = 0; j < n_old; ++j) old_col[old_chords[static_cast<std::size_t>(j)]] = j;

            // --- Per new chord: fundamental cycle, foldability, carried vs fresh.
            std::vector<ArcKey>                              fold_chords;
            std::vector<char>                                fold_carried;  // 1 = carried prior, 0 = born (D5 v2); for pair groups, the band-1 mask
            std::vector<char>                                fold_carried2; // pair groups: band-2 carried mask (equal to band 1 under the lane shape's §3.4 union; per band under the §6.1 equivalence shape) (WL-3)
            std::vector<std::vector<NetworkNormalEquations::DdPriorBlock::Term>> rows;
            std::vector<std::vector<NetworkNormalEquations::DdPriorBlock::Term>> rows2;  // pair groups: band-2 rows (same cycle, band-2 columns) (WL-3)
            std::vector<Eigen::VectorXd>                     Wrows;   // over the old basis (empty: fresh)
            // This epoch's linearization DD per emitted chord, Σ c·x_lin over the same
            // fundamental cycle that builds `terms`, i.e. the incidence the fold uses to form
            // z_free, so ẑ + zl is the arc-space absolute DD.  Empty unless lin_on. (RS-2)
            std::vector<double> zl_rows, zl_rows2;
            for (const ArcKey& c : chords) {
                const std::map<ArcKey, int> cyc = arcgraph::fundamentalCycle(*tree, c);

                // Foldable when every cycle arc resolves to a live (station, local col); for a
                // pair group, in both bands (membership requires band 2, §2, WL-3).
                bool foldable = true;
                std::vector<NetworkNormalEquations::DdPriorBlock::Term> terms, terms2;
                terms.reserve(cyc.size());
                if (g.pair) terms2.reserve(cyc.size());
                double zl_c = 0.0, zl_c2 = 0.0;   // Σ c·x_lin over this cycle (RS-2)
                for (const auto& [arc, coeff] : cyc) {
                    const ArcInfo& ai = arcinfo.at(arc);   // every cycle arc is in this component's graph
                    if (ai.est_station < 0 || ai.local_col < 0
                        || (g.pair && ai.local_col2 < 0)) { foldable = false; break; }
                    terms.push_back({static_cast<std::size_t>(ai.est_station), ai.local_col,
                                     static_cast<double>(coeff)});
                    if (g.pair)
                        terms2.push_back({static_cast<std::size_t>(ai.est_station), ai.local_col2,
                                          static_cast<double>(coeff)});
                    if (lin_on) {
                        zl_c += static_cast<double>(coeff) * ai.x_lin;
                        if (g.pair) zl_c2 += static_cast<double>(coeff) * ai.x_lin2;
                    }
                }
                if (!foldable) { counters_.chords_dropped += tally; continue; }

                // Cycle-length census.  The fundamental-cycle length is the tree-path span of
                // this DD: short cycles give a sparse DD information matrix J = Pᶻ⁻¹, so the
                // exact info-form carry stays bounded; long cycles give a dense J.  Counted
                // over foldable chords, the ones that enter the carry. (INC-C0, D-c viability)
                {
                    const long long clen = static_cast<long long>(cyc.size());
                    if (clen > counters_.max_cycle_len) counters_.max_cycle_len = clen;
                    counters_.sum_cycle_len += tally * clen;
                }

                // A chord is carried when its whole cycle existed last epoch (⊆ A_old) and no
                // cycle arc slipped this epoch; the DD is then a linear combination of the old
                // chords with w_j = χ_c(arc of old chord j).  A slip shifts the true DD by an
                // integer, and the q_z = 0 carry would hold a stiff, wrong prior, so a cycle
                // through a slipped arc is not carried and re-taps fresh (data-only tap-off)
                // (card 4 C3c).  This is exact with the projection W: a surviving carried
                // chord's cycle avoids the slipped arc, so the slipped-arc terms cancel from
                // its sum and the zero-weighted old chords need no explicit drop.
                bool would_carry = n_old > 0;
                if (would_carry)
                    for (const auto& [arc, coeff] : cyc)
                        if (!A_old.count(arc)) { would_carry = false; break; }
                // An unbased contributor that cannot be re-based (no usable ZL row, or mixed
                // with based contributors) is not folded: folding would assert the writer's
                // linearization frame on a run that re-seeded every x_lin from code.  The
                // chord is marginalized instead: it takes the ordinary birth path (diffuse
                // prior, data-only tap-off) and is counted. (RS-2)
                if (rebase_blocked && would_carry) {
                    would_carry = false;
                    rb_drop += tally;
                }
                bool slip_in_cycle = false;
                if (!slipped_arcs.empty())
                    for (const auto& [arc, coeff] : cyc)
                        if (slipped_arcs.count(arc)) { slip_in_cycle = true; break; }

                if (!g.pair) {
                    const bool carried = would_carry && !slip_in_cycle;
                    Eigen::VectorXd w;
                    if (carried) {
                        w = wRowOf(cyc, old_chords);
                        ++counters_.chords_carried;
                    } else {
                        ++counters_.chords_born;
                        if (would_carry && slip_in_cycle) ++counters_.slip_kills;  // carried DD lost to a slip
                    }
                    fold_chords.push_back(c);
                    fold_carried.push_back(carried ? 1 : 0);
                    rows.push_back(std::move(terms));
                    if (lin_on) zl_rows.push_back(zl_c);
                    Wrows.push_back(std::move(w));   // empty vector marks a fresh chord
                    continue;
                }

                // ---- Pair chord: per-band slip handling (WL-3).  Under the lane shape
                // (lane_joint) a slip on either band refloats both bands (§3.4): they share
                // the graph edge, and through the joint 2×2 coupling a prior that is stale in
                // one band corrupts the other.  Under the §6.1 equivalence shape each band
                // refloats on its own slips, like the two per-band groups it must reproduce
                // to ≤1e-9.
                bool slip2_in_cycle = false;
                if (!slipped2_arcs.empty())
                    for (const auto& [arc, coeff] : cyc)
                        if (slipped2_arcs.count(arc)) { slip2_in_cycle = true; break; }
                const bool union_slip = slip_in_cycle || slip2_in_cycle;
                const bool c1 = would_carry && !(cfg_.lane_joint ? union_slip : slip_in_cycle);
                const bool c2 = would_carry && !(cfg_.lane_joint ? union_slip : slip2_in_cycle);
                Eigen::VectorXd w;
                if (would_carry) w = wRowOf(cyc, old_chords);
                counters_.chords_carried += (c1 ? 1 : 0) + (c2 ? 1 : 0);
                counters_.chords_born    += (c1 ? 0 : 1) + (c2 ? 0 : 1);
                if (would_carry && !c1) ++counters_.slip_kills;
                if (would_carry && !c2) ++counters_.slip_kills;
                // A reborn pair chord has to earn its FFR fix again, so its fold-on-change
                // memory is erased here (WL-3 §3.4).  The erase is idempotent, so the RAIM
                // re-solve rebuild stays deterministic (carried_ does not change between
                // within-epoch builds).  Erasing lane_fix_epoch_ as well resumes increments
                // for the reborn chord in the same epoch; the slip response (invariant #1)
                // is unaffected. (LF-1 INC-2)
                if (!(c1 && c2)) { lane_fix_w_.erase({g.key, c}); lane_fix_epoch_.erase({g.key, c}); }
                fold_chords.push_back(c);
                fold_carried.push_back(c1 ? 1 : 0);
                fold_carried2.push_back(c2 ? 1 : 0);
                rows.push_back(std::move(terms));
                rows2.push_back(std::move(terms2));
                if (lin_on) { zl_rows.push_back(zl_c); zl_rows2.push_back(zl_c2); }
                Wrows.push_back(std::move(w));   // sized when the graph projection exists (per-band masks above)
            }
            if (fold_chords.empty()) continue;   // nothing foldable → carry nothing here

            // --- Assemble the DD-prior block(s) (carried joint + fresh diffuse). --
            const Eigen::Index K = static_cast<Eigen::Index>(fold_chords.size());
            const long long fold_dim = static_cast<long long>(tally) * static_cast<long long>(K);
            if (fold_dim > counters_.max_component_kc)
                counters_.max_component_kc = fold_dim;   // census; a pair joint fold costs 2K (INC-C0)
            {   // Bucket this emitted component's fold-dim (SPEC §7.4 Amdahl census, LF-1 INC-0).
                int bkt = 0;
                while (bkt < 9 && (1LL << bkt) <= fold_dim) ++bkt;  // floor(log2(fold_dim))+1, capped at 9 (≥256)
                ++counters_.comp_kc_hist[bkt];
            }
            Eigen::MatrixXd W;
            {   // Times the dense-K² change-of-basis projection (W · Pᶻ_old · Wᵀ, W·z_old), the
                // materialization that the factored (A+U S Uᵀ) store of INC-C2 removes (INC-C0).
                // W is graph-level: for a pair component it applies unchanged to each band's
                // half, i.e. blkdiag(W, W) on the joint [b1; b2] storage (WL-3).
                ScopedAccum _bp(perf_timing_, &perf_timers_.basis_project_ms);
                W = Eigen::MatrixXd::Zero(K, n_old);
                for (Eigen::Index i = 0; i < K; ++i)
                    if (Wrows[static_cast<std::size_t>(i)].size() == n_old && n_old > 0)
                        W.row(i) = Wrows[static_cast<std::size_t>(i)].transpose();

                // Tree swap: the carry was re-expressed in a different cycle basis if and only
                // if some carried chord's projection row mixes ≥2 old chords (W is not a signed
                // permutation).  This is the projection analogue of a phase-B unimodular pivot.
                for (const Eigen::VectorXd& wr : Wrows) {
                    if (wr.size() != n_old || n_old == 0) continue;   // fresh chord
                    if ((wr.array() != 0.0).count() >= 2) { counters_.tree_swaps += tally; break; }
                }

                // Census: an old chord that no surviving fundamental cycle references (its W
                // column is all-zero) is marginalized per chord, a drop that the whole-block
                // chords_marginalized count (prev_hit == 0) does not see.  It answers §0 (a)
                // vs (b): a large count means the projection kill path is firing (arc
                // churn); ~0 while max_z_dim grows means dead chords are retained in large
                // live components. (INC-C0)
                for (Eigen::Index j = 0; j < n_old; ++j)
                    if (W.col(j).isZero(0.0)) counters_.chords_marg_perchord += tally;
            }

            // --- Re-base a restored branch onto this run's linearization (RS-2). --------
            // The stored mean is the correction-frame DD ẑ = Z − zl, with Z the arc-space
            // absolute DD.  The projection W is a cycle-basis change and does not depend on
            // the frame: Z_new = W·Z_old exactly.  Hence
            //     ẑ_new_correct = Z_new − zl_now = (W·ẑ_old) + (W·zl_writer) − zl_now,
            // which is the correction below.  The writer's spanning tree is not serialized,
            // and this form does not need it: the re-base uses the projection instead of
            // recomputing the writer's cycles.  It is applied to the emitted block, never to
            // carried_, so a within-epoch rebuild is idempotent.  Fresh rows are skipped: their
            // prior is the diffuse zero-mean birth, already in this run's frame.  Pᶻ is not
            // changed, since an exact translation does not change the weights.
            auto applyRebase = [&](Eigen::VectorXd& zh, const Eigen::MatrixXd& Wb,
                                   const Eigen::VectorXd& zl_old_half,
                                   const std::vector<double>& zl_now,
                                   const std::vector<char>& mask) {
                if (!rebase_here || zh.size() == 0) return;
                const Eigen::VectorXd proj = Wb * zl_old_half;
                for (Eigen::Index i = 0; i < zh.size(); ++i) {
                    if (static_cast<std::size_t>(i) >= mask.size() || !mask[static_cast<std::size_t>(i)])
                        continue;
                    if (static_cast<std::size_t>(i) >= zl_now.size()) continue;
                    const double d = proj(i) - zl_now[static_cast<std::size_t>(i)];
                    zh(i) += d;
                    ++rb_n;
                    if (std::abs(d) > rb_max_dz) rb_max_dz = std::abs(d);
                }
            };

            if (!g.pair) {
                // ---- Per-band group: single-block emission (byte-identical to pre-WL-3). ----
                NetworkNormalEquations::DdPriorBlock blk;
                {
                    ScopedAccum _bp(perf_timing_, &perf_timers_.basis_project_ms);
                    blk.rows = std::move(rows);
                    blk.zhat = (n_old > 0) ? (W * z_old).eval() : Eigen::VectorXd::Zero(K);
                    // Carried–carried joint = W Pᶻ_old Wᵀ (fresh rows/cols are zero from W=0);
                    // fresh chords get an independent diffuse prior on the diagonal.
                    blk.Pz = (n_old > 0) ? (W * Pz_old * W.transpose()).eval()
                                         : Eigen::MatrixXd::Zero(K, K);
                    for (Eigen::Index i = 0; i < K; ++i)
                        if (Wrows[static_cast<std::size_t>(i)].size() != n_old || n_old == 0)
                            blk.Pz(i, i) += cfg_.fresh_prior_var;
                    blk.Pz = (0.5 * (blk.Pz + blk.Pz.transpose())).eval();
                }
                applyRebase(blk.zhat, W, zl_old, zl_rows, fold_carried);   // re-base carried rows (RS-2)

                Eigen::VectorXd blk_zhat = blk.zhat;   // prior means for the install-time innovation gate (D5)
                out.push_back(std::move(blk));
                BlockMeta bm0{g.key, std::move(fold_chords), verts,
                              std::set<ArcKey>(arcs.begin(), arcs.end()),
                              std::move(blk_zhat), std::move(fold_carried)};
                if (lin_on)   // the linearization the installed means are relative to (RS-2)
                    bm0.zl = Eigen::Map<const Eigen::VectorXd>(zl_rows.data(),
                                                               static_cast<Eigen::Index>(zl_rows.size()));
                block_meta_.push_back(std::move(bm0));
                ++counters_.components;
            } else {
                // ---- Pair component (WL-3, SPEC_lane_carry_card.md §2).  The projection
                // applies per band on the joint [band-1; band-2] storage.  Fresh chords get
                // the lane-basis prior (§3.1) under the lane shape and the isotropic diffuse
                // prior otherwise.  Emission is one joint 2K-row block (lane shape) or two
                // per-band K-row blocks (the §6.1 equivalence shape, carrying the same
                // information as two separate per-band groups).
                ++counters_.pair_components;
                Eigen::VectorXd zhat1, zhat2;
                Eigen::MatrixXd P11, P22, P12;
                // Declared outside the timing scope so the re-base below reuses the per-band
                // projections the means were formed with. (RS-2)
                Eigen::MatrixXd W1, W2;
                {
                    ScopedAccum _bp(perf_timing_, &perf_timers_.basis_project_ms);
                    if (n_old > 0) {
                        // The projection applies per band with that band's carried mask; a
                        // band whose chord refloated projects nothing (fresh row/col).
                        W1 = W; W2 = W;
                        for (Eigen::Index i = 0; i < K; ++i) {
                            if (!fold_carried[static_cast<std::size_t>(i)])  W1.row(i).setZero();
                            if (!fold_carried2[static_cast<std::size_t>(i)]) W2.row(i).setZero();
                        }
                        zhat1 = W1 * z_old.head(n_old);
                        zhat2 = W2 * z_old.tail(n_old);
                        P11 = W1 * Pz_old.topLeftCorner(n_old, n_old) * W1.transpose();
                        P22 = W2 * Pz_old.bottomRightCorner(n_old, n_old) * W2.transpose();
                        P12 = W1 * Pz_old.topRightCorner(n_old, n_old) * W2.transpose();
                    } else {
                        zhat1 = Eigen::VectorXd::Zero(K);
                        zhat2 = Eigen::VectorXd::Zero(K);
                        P11 = Eigen::MatrixXd::Zero(K, K);
                        P22 = Eigen::MatrixXd::Zero(K, K);
                        P12 = Eigen::MatrixXd::Zero(K, K);
                    }
                    // Fresh rows get the lane-basis prior Rᵀ·diag(σ_nl², σ_wl²)·R (σ_nl is the
                    // diffuse √fresh_prior_var, σ_wl the code-seeded lane scale), or the
                    // isotropic diffuse prior when the lane prior is not armed (§6.1 shape).
                    // The 2×2 lane prior applies only when both bands are fresh, which the lane
                    // shape's union refloat ensures; a per-band fresh row under the equivalence
                    // shape gets the plain diffuse diagonal.
                    const bool lane_prior = cfg_.lane_joint && cfg_.sigma_wl_fresh_cyc > 0.0
                                            && g.lambda1 > 0.0 && g.lambda2 > 0.0;
                    Eigen::Matrix2d Pf = Eigen::Matrix2d::Zero();
                    if (lane_prior) {
                        const double sig_wl_m =
                            cfg_.sigma_wl_fresh_cyc * laneMetersPerCycle(g.lambda1, g.lambda2);
                        const Eigen::Matrix2d R = laneRotation(g.lambda1, g.lambda2);
                        Pf = R.transpose()
                           * Eigen::Vector2d(cfg_.fresh_prior_var, sig_wl_m * sig_wl_m).asDiagonal()
                           * R;
                    }
                    for (Eigen::Index i = 0; i < K; ++i) {
                        const bool f1 = !fold_carried[static_cast<std::size_t>(i)];
                        const bool f2 = !fold_carried2[static_cast<std::size_t>(i)];
                        if (!f1 && !f2) continue;   // carried in both bands
                        if (lane_prior && f1 && f2) {
                            P11(i, i) += Pf(0, 0);
                            P22(i, i) += Pf(1, 1);
                            P12(i, i) += Pf(0, 1);
                        } else {
                            if (f1) P11(i, i) += cfg_.fresh_prior_var;
                            if (f2) P22(i, i) += cfg_.fresh_prior_var;
                        }
                    }
                }
                // Re-base per band, with that band's projection and carried mask. (RS-2)
                if (rebase_here && n_old > 0) {
                    applyRebase(zhat1, W1, zl_old.head(n_old), zl_rows,  fold_carried);
                    applyRebase(zhat2, W2, zl_old.tail(n_old), zl_rows2, fold_carried2);
                }
                const std::set<ArcKey> arcs_set(arcs.begin(), arcs.end());
                if (cfg_.lane_joint) {
                    // One joint 2K-row block: rows are K band-1 cycles then K band-2 cycles,
                    // and Pᶻ is the joint 2K×2K.  Its fold keeps the cross-band Woodbury term
                    // B₁₂ exactly, which the per-band shape drops; this is part of the lane
                    // design.
                    NetworkNormalEquations::DdPriorBlock blk;
                    blk.rows = std::move(rows);
                    blk.rows.insert(blk.rows.end(), std::make_move_iterator(rows2.begin()),
                                    std::make_move_iterator(rows2.end()));
                    blk.zhat.resize(2 * K);
                    blk.zhat << zhat1, zhat2;
                    blk.Pz.resize(2 * K, 2 * K);
                    blk.Pz.topLeftCorner(K, K)     = P11;
                    blk.Pz.bottomRightCorner(K, K) = P22;
                    blk.Pz.topRightCorner(K, K)    = P12;
                    blk.Pz.bottomLeftCorner(K, K)  = P12.transpose();
                    blk.Pz = (0.5 * (blk.Pz + blk.Pz.transpose())).eval();
                    Eigen::VectorXd blk_zhat = blk.zhat;
                    out.push_back(std::move(blk));
                    BlockMeta bm{g.key, std::move(fold_chords), verts, arcs_set,
                                 std::move(blk_zhat), std::move(fold_carried)};
                    bm.pair_band = 3;
                    bm.lambda1 = g.lambda1;
                    bm.lambda2 = g.lambda2;
                    if (lin_on) {   // [band-1; band-2], matching the joint zhat layout (RS-2)
                        bm.zl.resize(2 * K);
                        for (Eigen::Index i = 0; i < K; ++i) {
                            bm.zl(i)     = zl_rows[static_cast<std::size_t>(i)];
                            bm.zl(K + i) = zl_rows2[static_cast<std::size_t>(i)];
                        }
                    }
                    block_meta_.push_back(std::move(bm));
                    ++counters_.components;
                } else {
                    // Two per-band K-row blocks, band 2 adjacent at b+1.  The fold sees the
                    // same information as two per-band groups (block-diagonal, no B₁₂), so
                    // the §6.1 equivalence gate can hold to summation order.
                    NetworkNormalEquations::DdPriorBlock b1, b2;
                    b1.rows = std::move(rows);
                    b1.zhat = zhat1;
                    b1.Pz   = (0.5 * (P11 + P11.transpose())).eval();
                    b2.rows = std::move(rows2);
                    b2.zhat = zhat2;
                    b2.Pz   = (0.5 * (P22 + P22.transpose())).eval();
                    out.push_back(std::move(b1));
                    out.push_back(std::move(b2));
                    BlockMeta m1{g.key, fold_chords, verts, arcs_set,
                                 std::move(zhat1), std::move(fold_carried)};
                    m1.pair_band = 1;
                    m1.lambda1 = g.lambda1;
                    m1.lambda2 = g.lambda2;
                    BlockMeta m2{g.key, std::move(fold_chords), verts, arcs_set,
                                 std::move(zhat2), std::move(fold_carried2)};
                    m2.pair_band = 2;
                    m2.lambda1 = g.lambda1;
                    m2.lambda2 = g.lambda2;
                    if (lin_on) {   // one K-vector per emitted half (RS-2)
                        m1.zl = Eigen::Map<const Eigen::VectorXd>(
                            zl_rows.data(), static_cast<Eigen::Index>(zl_rows.size()));
                        m2.zl = Eigen::Map<const Eigen::VectorXd>(
                            zl_rows2.data(), static_cast<Eigen::Index>(zl_rows2.size()));
                    }
                    block_meta_.push_back(std::move(m1));
                    block_meta_.push_back(std::move(m2));
                    counters_.components += 2;
                }
            }
        }
    }

    // Splits: a previous block spread across ≥2 new components.  Marginalized: a
    // previous block that fed nothing this epoch (its arcs all died or re-referenced).
    // A pair block counts twice, as it holds two per-band DDs per chord (WL-3).
    for (std::size_t p = 0; p < prev.size(); ++p) {
        const long long ptally = prev[p].pair ? 2 : 1;
        if (prev_hit[p] >= 2) counters_.splits += ptally;
        if (prev_hit[p] == 0)
            counters_.chords_marginalized += ptally * static_cast<long long>(prev[p].chords.size());
    }
    // Publish the re-base figures only from a build that re-based, so the run-end line keeps
    // the resume epoch's values instead of the zeros every later epoch would write over them
    // (invariant #4, as in reset() above). (RS-2)
    if (rb_n > 0 || rb_drop > 0) {
        counters_.resume_rebased          = rb_n;
        counters_.resume_dropped_unbased  = rb_drop;
        counters_.resume_rebase_max_dz    = rb_max_dz;
    }
    return out;
}

void NetworkInvariantCarryDriver::seedCarriedForTest(
    int group, const std::vector<ArcKey>& chords, const Eigen::VectorXd& z,
    const Eigen::MatrixXd& Pz, const std::set<int>& vertices,
    const std::set<ArcKey>& arcs) {
    CarriedBlock cb;
    cb.group = group;
    cb.chords = chords;
    cb.z = z;
    cb.Pz = Pz;
    cb.vertices = vertices;
    cb.arcs = arcs;
    carried_.push_back(std::move(cb));
}

void NetworkInvariantCarryDriver::seedCarriedLinForTest(int group,
                                                        const Eigen::VectorXd& zl,
                                                        bool unbased) {
    for (auto it = carried_.rbegin(); it != carried_.rend(); ++it) {
        if (it->group != group) continue;
        it->zl = zl;
        it->unbased = unbased;
        return;
    }
}

bool NetworkInvariantCarryDriver::carriedDdForTest(
    int group, const ArcKey& chord, double& z, double& var) const {
    for (const CarriedBlock& cb : carried_) {
        if (cb.group != group) continue;
        auto it = std::find(cb.chords.begin(), cb.chords.end(), chord);
        if (it == cb.chords.end()) continue;
        const Eigen::Index i = it - cb.chords.begin();
        z = cb.z(i);
        var = cb.Pz(i, i);
        return true;
    }
    return false;
}

bool NetworkInvariantCarryDriver::injectCarriedChord(
    int group, const ArcKey& chord, double new_z, double new_var) {
    for (CarriedBlock& cb : carried_) {
        if (cb.group != group) continue;
        auto it = std::find(cb.chords.begin(), cb.chords.end(), chord);
        if (it == cb.chords.end()) continue;
        const Eigen::Index i = it - cb.chords.begin();
        const double keep_var = cb.Pz(i, i);       // fallback if new_var is non-positive
        cb.z(i) = new_z;                            // the lane-targeted correction-frame DD
        cb.Pz.row(i).setZero();                     // fresh independent assertion (PSD-safe)
        cb.Pz.col(i).setZero();
        cb.Pz(i, i) = (new_var > 0.0) ? new_var : keep_var;
        gate_exempt_.insert({ group, chord });      // D5-exempt at the next install (§3.3)
        return true;
    }
    return false;   // not a carried chord of `group` (cycle not 1:1); the caller skips and counts
}

void NetworkInvariantCarryDriver::seedCarriedPairForTest(
    int group, const std::vector<ArcKey>& chords, const Eigen::VectorXd& z,
    const Eigen::MatrixXd& Pz, double lambda1, double lambda2,
    const std::set<int>& vertices, const std::set<ArcKey>& arcs) {
    CarriedBlock cb;
    cb.group = group;
    cb.chords = chords;
    cb.z = z;
    cb.Pz = Pz;
    cb.vertices = vertices;
    cb.arcs = arcs;
    cb.pair = true;
    cb.lambda1 = lambda1;
    cb.lambda2 = lambda2;
    carried_.push_back(std::move(cb));
}

bool NetworkInvariantCarryDriver::carriedPairBlockForTest(
    int group, std::vector<ArcKey>& chords, Eigen::VectorXd& z, Eigen::MatrixXd& Pz) const {
    for (const CarriedBlock& cb : carried_) {
        if (cb.group != group || !cb.pair) continue;
        chords = cb.chords;
        z = cb.z;
        Pz = cb.Pz;
        return true;
    }
    return false;
}

bool NetworkInvariantCarryDriver::carriedPairValue(
    int group, const ArcKey& chord, double& z1, double& var1,
    double& z2, double& var2) const {
    for (const CarriedBlock& cb : carried_) {
        if (cb.group != group || !cb.pair) continue;
        auto it = std::find(cb.chords.begin(), cb.chords.end(), chord);
        if (it == cb.chords.end()) continue;
        const Eigen::Index K = static_cast<Eigen::Index>(cb.chords.size());
        const Eigen::Index i = it - cb.chords.begin();
        if (cb.z.size() != 2 * K) return false;   // defensive
        z1   = cb.z(i);
        var1 = cb.Pz(i, i);
        z2   = cb.z(K + i);
        var2 = cb.Pz(K + i, K + i);
        return true;
    }
    return false;
}

bool NetworkInvariantCarryDriver::injectCarriedChordPair(
    int group, const ArcKey& chord, double new_z1, double new_var1,
    double new_z2, double new_var2) {
    for (CarriedBlock& cb : carried_) {
        if (cb.group != group || !cb.pair) continue;
        auto it = std::find(cb.chords.begin(), cb.chords.end(), chord);
        if (it == cb.chords.end()) continue;
        const Eigen::Index K = static_cast<Eigen::Index>(cb.chords.size());
        const Eigen::Index i = it - cb.chords.begin();
        if (cb.z.size() != 2 * K) return false;   // defensive
        const double keep1 = cb.Pz(i, i), keep2 = cb.Pz(K + i, K + i);
        cb.z(i)     = new_z1;
        cb.z(K + i) = new_z2;
        cb.Pz.row(i).setZero();
        cb.Pz.col(i).setZero();
        cb.Pz.row(K + i).setZero();
        cb.Pz.col(K + i).setZero();
        cb.Pz(i, i)         = (new_var1 > 0.0) ? new_var1 : keep1;
        cb.Pz(K + i, K + i) = (new_var2 > 0.0) ? new_var2 : keep2;
        gate_exempt_.insert({ group, chord });   // D5-exempt (both band rows) at the next install (§3.3)
        return true;
    }
    return false;
}

bool NetworkInvariantCarryDriver::applyLaneMeasurement(
    std::vector<NetworkNormalEquations::DdPriorBlock>& blocks,
    std::size_t b, std::size_t k, double y_cyc, double r_cyc2) {
    if (b >= block_meta_.size() || b >= blocks.size()) return false;
    BlockMeta& bm = block_meta_[b];
    if (bm.pair_band != 3) return false;   // lane rows exist only on the joint lane shape
    const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
    if (static_cast<Eigen::Index>(k) >= K) return false;
    if (!(r_cyc2 > 0.0) || !std::isfinite(y_cyc)) return false;
    if (!(bm.lambda1 > 0.0 && bm.lambda2 > 0.0)) return false;
    NetworkNormalEquations::DdPriorBlock& blk = blocks[b];
    if (blk.zhat.size() != 2 * K || blk.Pz.rows() != 2 * K || blk.Pz.cols() != 2 * K)
        return false;
    // Times the dense rank-1 downdate (the irreducible O(K²)-per-row measurement work)
    // separately from the basis projection that the SB_DDBUILD wall includes. (LF-1 INC-0)
    ScopedAccum _lm(perf_timing_, &perf_timers_.lane_meas_ms);
    // Scalar KF update on the block prior with the sparse WL functional
    // h = +1/λ₁ at row k, −1/λ₂ at row K+k (correction frame, cycles).
    const double a1 = 1.0 / bm.lambda1, a2 = 1.0 / bm.lambda2;
    const Eigen::Index i1 = static_cast<Eigen::Index>(k), i2 = K + i1;
    const Eigen::VectorXd Ph = a1 * blk.Pz.col(i1) - a2 * blk.Pz.col(i2);
    const double S = a1 * Ph(i1) - a2 * Ph(i2) + r_cyc2;
    if (!(S > 0.0) || !std::isfinite(S)) return false;
    const double innov = y_cyc - (a1 * blk.zhat(i1) - a2 * blk.zhat(i2));
    if (!std::isfinite(innov)) return false;
    blk.zhat += (innov / S) * Ph;
    blk.Pz   -= (Ph * Ph.transpose()) / S;
    // The per-row re-symmetrize is required.  The rank-1 downdate keeps symmetry only to
    // ULP, and the stiff q_z = 0 DD carry amplifies that asymmetry across epochs (dropping it
    // moves the lane-on solution by ~1e-4 within four epochs).  With fold_blocked off it runs
    // per row (byte-identical to the build before LF-1); with fold_blocked on it moves to one
    // @ref finalizeLaneBatch pass per block (equivalence class, ≤1e-12 against sequential).
    // (LF-1 INC-1)
    if (!cfg_.fold_blocked)
        blk.Pz = (0.5 * (blk.Pz + blk.Pz.transpose())).eval();
    bm.zhat   = blk.zhat;   // the D5 innovation gate standardizes against the folded prior
    if (perf_timing_) ++perf_timers_.lane_meas_calls;
    return true;
}

int NetworkInvariantCarryDriver::applyLaneFix(
    std::vector<NetworkNormalEquations::DdPriorBlock>& blocks,
    std::size_t b, std::size_t k, long long w, double y_cyc, double sigma_cyc) {
    if (b >= block_meta_.size() || b >= blocks.size()) return -1;
    BlockMeta& bm = block_meta_[b];
    if (bm.pair_band != 3 || k >= bm.chords.size()) return -1;
    if (!(sigma_cyc > 0.0) || !std::isfinite(y_cyc)) return -1;
    if (!(bm.lambda1 > 0.0 && bm.lambda2 > 0.0)) return -1;
    const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
    NetworkNormalEquations::DdPriorBlock& blk = blocks[b];
    if (blk.zhat.size() != 2 * K || blk.Pz.rows() != 2 * K) return -1;

    // Times the idempotent lane set (the re-pins of decided lanes, the rows that INC-1's
    // O(K²)→O(K) sparse rewrite targets) apart from the measurement folds. (LF-1 INC-0)
    ScopedAccum _lf(perf_timing_, &perf_timers_.lane_fix_ms);
    if (perf_timing_) ++perf_timers_.lane_fix_calls;

    // The fix is an idempotent lane-coordinate set, not a measurement fold.  On every
    // decided epoch it re-centers the chord's WL lane coordinate on the integer, resets its
    // variance to σ² and zeroes the coordinate's cross-covariances: a fresh independent
    // assertion, PSD-safe, i.e. WL-2's inject semantics applied to the lane coordinate, with
    // NL preserved by construction.  A measurement fold would accumulate per-epoch decisions
    // that are not independent information (the F-A once-only rule); the set is idempotent
    // in information.  Re-applying it while the decision is unchanged keeps the lane pinned
    // against the q_wl re-loosening and the noisy MW increments; with fold-on-change alone,
    // decided lanes drift back to MW quality.
    const double a1 = 1.0 / bm.lambda1, a2 = 1.0 / bm.lambda2;
    const double hn = std::sqrt(a1 * a1 + a2 * a2);   // ‖h‖: cycles per lane-coordinate meter
    const Eigen::Index p = static_cast<Eigen::Index>(k), qq = K + static_cast<Eigen::Index>(k);
    const double up = a1 / hn, uq = -a2 / hn;          // the unit lane direction's two nonzeros
    const double target_coord = y_cyc / hn;            // correction-frame lane target (m)
    const double sig_coord    = sigma_cyc / hn;
    // z' = z + (t − uᵀz)·u;  P' = (I−uuᵀ)P(I−uuᵀ) + σ²·uuᵀ = P − Pu·uᵀ − u·Puᵀ + (uᵀPu+σ²)·uuᵀ.
    if (!cfg_.fold_blocked) {
        // Dense O(K²) form, byte-identical to the build before LF-1.
        Eigen::VectorXd u = Eigen::VectorXd::Zero(2 * K);
        u(p)  =  up;
        u(qq) =  uq;
        const Eigen::VectorXd Pu = blk.Pz * u;
        const double uPu = u.dot(Pu);
        if (!std::isfinite(uPu) || uPu < 0.0) return -1;
        blk.zhat += (target_coord - u.dot(blk.zhat)) * u;
        blk.Pz   += -Pu * u.transpose() - u * Pu.transpose()
                  + (uPu + sig_coord * sig_coord) * (u * u.transpose());
        blk.Pz    = (0.5 * (blk.Pz + blk.Pz.transpose())).eval();
    } else {
        // O(K) sparse set (LF-1 INC-1).  u has two nonzeros (rows p, qq), so Pu = P·u is a
        // two-column combination rather than a dense GEMV, and ΔP = −Pu·uᵀ − u·Puᵀ + c·uuᵀ is
        // nonzero only in rows/columns {p, qq}.  The two changed columns Mp = ΔP(:,p) and
        // Mq = ΔP(:,qq) are written symmetrically (column plus mirrored row).  Pz stays exactly
        // symmetric (ΔP is symmetric, so newp(qq) == newq(p)), and the per-row re-symmetrize
        // moves to finalizeLaneBatch.  Same math as the dense branch up to fp reassociation.
        const Eigen::VectorXd Pu = up * blk.Pz.col(p) + uq * blk.Pz.col(qq);
        const double uPu = up * Pu(p) + uq * Pu(qq);
        if (!std::isfinite(uPu) || uPu < 0.0) return -1;
        const double c = uPu + sig_coord * sig_coord;
        const double coef = target_coord - (up * blk.zhat(p) + uq * blk.zhat(qq));
        blk.zhat(p)  += coef * up;
        blk.zhat(qq) += coef * uq;
        Eigen::VectorXd Mp = (-up) * Pu;   // ΔP(i,p)  = −Pu(i)·up  (+ the i∈{p,qq} terms below)
        Eigen::VectorXd Mq = (-uq) * Pu;   // ΔP(i,qq) = −Pu(i)·uq  (+ the i∈{p,qq} terms below)
        Mp(p)  += -up * Pu(p)  + c * up * up;   Mp(qq) += -uq * Pu(p)  + c * uq * up;
        Mq(p)  += -up * Pu(qq) + c * up * uq;   Mq(qq) += -uq * Pu(qq) + c * uq * uq;
        const Eigen::VectorXd newp = blk.Pz.col(p)  + Mp;
        const Eigen::VectorXd newq = blk.Pz.col(qq) + Mq;
        blk.Pz.col(p)  = newp;   blk.Pz.col(qq) = newq;
        blk.Pz.row(p)  = newp.transpose();
        blk.Pz.row(qq) = newq.transpose();
    }
    bm.zhat   = blk.zhat;   // the D5 gate standardizes against the prior after the set

    const std::pair<int, ArcKey> key{ bm.group, bm.chords[k] };
    const auto it = lane_fix_w_.find(key);
    const bool changed = (it == lane_fix_w_.end() || it->second != w);
    lane_fix_staged_[key] = w;   // committed at installPosterior (RAIM re-solve safe)
    return changed ? 1 : 0;      // 1 = integer moved/new, 0 = re-pinned (unchanged)
}

bool NetworkInvariantCarryDriver::laneIncrementSkippable(std::size_t b, std::size_t k) const {
    if (!cfg_.skip_pinned_increments) return false;   // off: never skip (byte-identical)
    if (b >= block_meta_.size()) return false;
    const BlockMeta& bm = block_meta_[b];
    if (bm.pair_band != 3 || k >= bm.chords.size()) return false;   // lane rows only on joint blocks
    const auto it = lane_fix_epoch_.find({ bm.group, bm.chords[k] });
    if (it == lane_fix_epoch_.end()) return false;    // not committed-decided (or disturbed since)
    return (install_epoch_index_ - it->second) <= cfg_.skip_pinned_within_n;   // within the grace window
}

void NetworkInvariantCarryDriver::finalizeLaneBatch(
    std::vector<NetworkNormalEquations::DdPriorBlock>& blocks) const {
    if (!cfg_.fold_blocked) return;   // OFF: primitives symmetrized per-row (byte-identical)
    // Restore exact symmetry once per joint block, after the epoch's MW / bridge / fix rows.
    // This replaces the per-row 0.5·(P+Pᵀ) that the primitives skip under fold_blocked (they
    // keep symmetry only to ULP).  Only joint pair blocks (pair_band == 3) receive lane rows.
    const std::size_t n = std::min(blocks.size(), block_meta_.size());
    for (std::size_t b = 0; b < n; ++b) {
        if (block_meta_[b].pair_band != 3) continue;
        Eigen::MatrixXd& P = blocks[b].Pz;
        if (P.rows() != P.cols() || P.rows() == 0) continue;
        P = (0.5 * (P + P.transpose())).eval();
    }
}

bool NetworkInvariantCarryDriver::carriedCrossForTest(
    int group, const ArcKey& a, const ArcKey& b, double& cov) const {
    for (const CarriedBlock& cb : carried_) {
        if (cb.group != group) continue;
        auto ia = std::find(cb.chords.begin(), cb.chords.end(), a);
        auto ib = std::find(cb.chords.begin(), cb.chords.end(), b);
        if (ia == cb.chords.end() || ib == cb.chords.end()) continue;
        cov = cb.Pz(ia - cb.chords.begin(), ib - cb.chords.begin());
        return true;
    }
    return false;
}

double NetworkInvariantCarryDriver::carriedEstimableForTest(
    int group, const std::map<ArcKey, int>& target_cycle) const {
    for (const CarriedBlock& cb : carried_) {
        if (cb.group != group) continue;
        // The component must contain the target cycle's arcs (share a vertex).
        bool here = false;
        for (const auto& [arc, coeff] : target_cycle)
            if (cb.vertices.count(arc.u) || cb.vertices.count(arc.v)) { here = true; break; }
        if (!here) continue;
        double val = 0.0;
        for (Eigen::Index j = 0; j < static_cast<Eigen::Index>(cb.chords.size()); ++j) {
            auto it = target_cycle.find(cb.chords[static_cast<std::size_t>(j)]);
            if (it != target_cycle.end()) val += static_cast<double>(it->second) * cb.z(j);
        }
        return val;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

void NetworkInvariantCarryDriver::installPosterior(
    const NetworkEstimator& est, const Eigen::VectorXd& x_s,
    const NetworkEstimator::Datum& datum, int n_threads) {
    last_innov_flags_.clear();
    last_innov_sigmas_.clear();
    last_innov_terms_.clear();
    last_innov_warmup_.clear();    // index-aligned with last_innov_flags_ (RS-2b)
    last_fold_rebirths_.clear();   // fold-time rebirths (PD-safe INC-2a)
    // The datum-frame shared covariance (the n_shared² Σ_ss copy) is formed once per install;
    // every block's posterior-cov and innovation readout uses the same matrix.
    const Eigen::MatrixXd cov_free = est.datumFrameSharedCov(datum);
    // The empirical inflation σ̂₀² accumulated through the previous epoch (predict, then
    // update: this epoch's standardized innovations are fed after gating, below).  It is 1.0
    // (the raw formal test of D5 v1) when calibration is off or in warm-up.  The gate is the
    // Nσ test |v_i| > k·σ̂₀·√Cov(v)_ii. (D5 v2)
    const InflationStatus infl = innovInflationStatus();
    const double sigma0 = infl.sigma0;
    const double k2f    = cfg_.innov_k * cfg_.innov_k * sigma0 * sigma0;   // (k·σ̂₀)²
    // Tally which inflation state produced σ̂₀.  Always on (invariant #4): σ̂₀ = 1.00 from a
    // calibrated run and σ̂₀ = 1.00 from the floor clamp raising a small raw value are the same
    // number with opposite meanings.  Solve-neutral: nothing below reads these counters.
    ++counters_.d5_calib_epochs;
    switch (infl.state) {
        case InflationState::Off:     ++counters_.d5_calib_off_epochs;     break;
        case InflationState::WarmUp:  ++counters_.d5_calib_warmup_epochs;  break;
        case InflationState::Floored: ++counters_.d5_calib_floored_epochs; break;
        case InflationState::Capped:  ++counters_.d5_calib_capped_epochs;  break;
        case InflationState::Active:  ++counters_.d5_calib_active_epochs;  break;
    }
    if (infl.state != InflationState::Off && infl.state != InflationState::WarmUp) {
        counters_.d5_calib_raw_min = std::min(counters_.d5_calib_raw_min, infl.raw_sigma0_sq);
        counters_.d5_calib_raw_max = std::max(counters_.d5_calib_raw_max, infl.raw_sigma0_sq);
    }

    // ---- Deterministic-parallel readout (network.parallel_seam) -----------------
    // The per-block posterior mean/cov and innovation are const readouts of the folded
    // kernel (each reads only dd_priors_[b] and the shared read-only x_s / cov_free), so they
    // run in parallel.  Everything order-sensitive (the σ̂₀ accumulator feed
    // NetworkCarryVce::addRow, the gate decision, the flag/counter appends and the carried_
    // install) stays serial in the (block b, chord i) order below, so the σ̂₀ that gates the
    // next epoch, the counters and carried_ are bit-identical to the serial install for any
    // thread count.  The serial path (n_threads == 1, the default) has its own install body
    // below; keep the two install bodies in lockstep.
    // A D5-flagged pair chord loses its fold-on-change memory (it re-taps and has to earn
    // its FFR fix again, §3.4); laneFixErase below does this for every install shape. (WL-3)
    // D5 v3 corroboration gate (Cfg::innov_corroborate).  Called only after a chord has
    // tripped the k·σ̂₀ test; returns true when the re-tap is to be suppressed.
    //
    // Suppression requires positive evidence of continuity: membership of innov_flat_ means
    // the caller measured the raw MW/GF level and found it flat.  An arc without evidence is
    // not in the set and is re-tapped, so this narrows the slip response rather than removing
    // it (invariant #1).  The optional magnitude backstop re-admits very large innovations;
    // it defaults to 0 (inert) because corroboration was measured to fall, not rise, with
    // innovation size.
    //
    // Ordering contract: the σ̂₀ accumulator (inflation_->addRow) is fed before this is
    // consulted at both call sites, so suppression never censors the calibration input.  The
    // D5 v2 requirement that above-threshold rows are fed into σ̂₀ holds whether or not the
    // re-tap happens.
    const double corr_backstop2 =
        (cfg_.innov_corroborate && cfg_.innov_corroborate_backstop_k > 0.0)
            ? cfg_.innov_corroborate_backstop_k * cfg_.innov_corroborate_backstop_k * k2f
            : 0.0;
    auto d5Suppressed = [&](int group, const ArcKey& chord, double v2, double var) {
        if (!cfg_.innov_corroborate) return false;
        if (!innov_flat_.count({ group, chord })) return false;   // no positive evidence ⇒ re-tap
        if (corr_backstop2 > 0.0 && v2 > corr_backstop2 * var) return false;  // backstop wins
        ++counters_.innov_suppressed;
        return true;
    };
    // With Cfg::innov_carried_only armed, a chord without an informative prior is not gated
    // (D5 v5).
    //
    // The test is |v_i| > k·σ̂₀·√Cov(v)_ii, where Cov(v) is the H0 covariance of the
    // innovation about the carried prior.  A chord born this epoch has no carried prior: its
    // prior is the diffuse fresh_prior_var (1e6 by default), so Cov(v)_ii ≈ 1e6 and v is the
    // correction frame's distance from x_lin.  There is no H0 to test, and the reset a flag
    // triggers is a no-op (a diffuse chord's posterior already is the data-only tap-off).
    // The flag's only effect is to put the arc in the next epoch's slip funnel, which re-births
    // the chord, which is flagged again.  A chord re-born every epoch accumulates nothing, so
    // its dd_sigma_cyc cannot fall; this loop drives the chord churn.
    //
    // The D5 v2 σ̂₀ accumulator already excludes born chords (a born chord's diffuse Cov(v)
    // would pull σ̂₀² down); this applies the same exclusion to the gate decision, using the
    // same chord_carried mask so the two stay consistent.  It is a knob, default off, to keep
    // the default output byte-identical.
    //
    // The condition is informational, not only topological.  `chord_carried` says the cycle
    // lay in A_old and no arc slipped; it does not say the carried prior holds information.
    // A chord carried from a predecessor that was itself diffuse inherits a diffuse projected
    // prior and passes chord_carried.  Keying on chord_carried alone left most of the
    // surviving flags at √Cov(v) = 1000 m, still untestable, so the prior is tested directly.
    //
    // Cov(v) = (Pᶻ + B) − GᵀΣG is the prior-predictive variance, so Cov(v)_ii at or near
    // fresh_prior_var means Pᶻ_ii is diffuse whatever the topology says.  Half the fresh
    // prior is a deliberately loose bar: one epoch of real data reduces a DD's variance by
    // orders of magnitude, so a variance still within a factor of two of the diffuse prior
    // carries almost no information and there is no H0 to test against it.
    const double kDiffusePriorFrac = 0.5;
    auto bornSkipped = [this, kDiffusePriorFrac](bool have_carried,
                                                 const std::vector<char>& carried,
                                                 std::size_t idx, double var) {
        if (!cfg_.innov_carried_only) return false;
        // Without a mask, born and carried cannot be told apart, so only the variance test
        // applies.  The fail-safe direction keeps the slip response: a missing mask never
        // disarms the fine-scale layer of invariant #1.  buildDdPriors always emits one
        // chord_carried entry per chord, so this is defensive; the σ̂₀ feed above treats an
        // absent mask as "feed everything", and the two fail in the same direction.
        const bool topologically_born = have_carried && !carried[idx];
        const bool informationally_diffuse =
            cfg_.fresh_prior_var > 0.0 && var >= kDiffusePriorFrac * cfg_.fresh_prior_var;
        if (!topologically_born && !informationally_diffuse) return false;
        ++counters_.innov_born_skipped;
        return true;
    };
    auto laneFixErase = [this](int group, const ArcKey& chord) {
        if (!lane_fix_w_.empty())      lane_fix_w_.erase({group, chord});
        if (!lane_fix_staged_.empty()) lane_fix_staged_.erase({group, chord});
        if (!lane_fix_epoch_.empty())  lane_fix_epoch_.erase({group, chord});  // a D5 flag resumes increments (LF-1 INC-2)
    };
    // Per-chord D5 gate over one emitted per-band block, writing into @p cb at rows
    // [off, off+K).  A per-band group calls it with off = 0; a split pair component (WL-3)
    // calls it once per band half of the stitched pair block, with off = 0 and off = K.
    auto d5GateHalf = [&](const BlockMeta& bm, const Eigen::VectorXd& v,
                          const Eigen::MatrixXd& cov_v, CarriedBlock& cb, Eigen::Index off) {
        const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
        const bool have_carried = static_cast<Eigen::Index>(bm.chord_carried.size()) == K;
        if (!(v.size() == K && cov_v.rows() == K && cov_v.cols() == K && bm.zhat.size() == K))
            return;
        for (Eigen::Index i = 0; i < K; ++i) {
            const double var = cov_v(i, i);
            // Count rows skipped for a non-positive variance.  An inert fold caches the raw
            // non-PD Pᶻ+B as Minv, so Cov(v) inherits its negative diagonals and a degenerate
            // block's rows are all skipped here; the counter makes that visible. (PD-safe INC-0)
            if (!(var > 0.0)) { ++counters_.d5_nonpos_var_rows; continue; }
            if (gate_exempt_.count({ bm.group, bm.chords[static_cast<std::size_t>(i)] }))
                continue;
            if (inflation_ && (!have_carried || bm.chord_carried[static_cast<std::size_t>(i)]))
                inflation_->addRow(kInflationFamily, kInflationSat,
                                   v(i) / std::sqrt(var), /*gap_s=*/0.0,
                                   /*post_var=*/0.0, /*fold_var=*/1.0);
            if (bornSkipped(have_carried, bm.chord_carried,
                            static_cast<std::size_t>(i), var)) continue;
            if (v(i) * v(i) <= k2f * var) continue;
            if (d5Suppressed(bm.group, bm.chords[static_cast<std::size_t>(i)],
                             v(i) * v(i), var))
                continue;
            // Inside the post-resume warm-up a trip is only recorded.  The flag is recorded
            // (so it reaches the CSV and the epoch's n_d5_flags) and counted, ẑ, Pᶻ and the
            // lane fix are left alone, and the warm-up mark tells the caller to keep the arc
            // out of the slip funnel.  innov_retaps is not incremented: N flags with 0 re-taps
            // shows that the window was open. (RS-2b)
            if (cfg_.innov_diagnostic_only) {
                last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(i)]);
                last_innov_sigmas_.push_back(std::abs(v(i)) / std::sqrt(var));
                last_innov_terms_.push_back({bm.zhat(i), v(i), var, sigma0, cb.z(off + i)});
                last_innov_warmup_.push_back(1);
                ++counters_.innov_flags;
                ++counters_.d5_warmup_suppressed;
                continue;
            }
            cb.z(off + i) = bm.zhat(i) + v(i);
            cb.Pz.row(off + i).setZero();
            cb.Pz.col(off + i).setZero();
            cb.Pz(off + i, off + i) = sigma0 * sigma0 * var;
            last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(i)]);
            last_innov_sigmas_.push_back(std::abs(v(i)) / std::sqrt(var));
            last_innov_terms_.push_back({bm.zhat(i), v(i), var, sigma0, cb.z(off + i)});
            last_innov_warmup_.push_back(0);
            ++counters_.innov_flags;
            ++counters_.innov_retaps;
            laneFixErase(bm.group, bm.chords[static_cast<std::size_t>(i)]);
        }
    };
    // Per-chord D5 gate over a joint 2K pair block, applied per band row as in d5GateHalf
    // (row i < K is band 1 of chord i, row K+i band 2 of chord i).  The exemption and the
    // σ̂₀ carried flag are keyed by the graph chord, the same for both bands. (WL-3)
    auto d5GateJoint = [&](const BlockMeta& bm, const Eigen::VectorXd& v,
                           const Eigen::MatrixXd& cov_v, CarriedBlock& cb) {
        const Eigen::Index K  = static_cast<Eigen::Index>(bm.chords.size());
        const Eigen::Index K2 = 2 * K;
        const bool have_carried = static_cast<Eigen::Index>(bm.chord_carried.size()) == K;
        if (!(v.size() == K2 && cov_v.rows() == K2 && cov_v.cols() == K2 && bm.zhat.size() == K2))
            return;
        for (Eigen::Index i = 0; i < K2; ++i) {
            const Eigen::Index c = (i < K) ? i : i - K;
            const double var = cov_v(i, i);
            // Counted as in d5GateHalf: an inert (degenerate) fold's Cov(v) has non-positive
            // diagonals, and every affected row is skipped. (PD-safe INC-0)
            if (!(var > 0.0)) { ++counters_.d5_nonpos_var_rows; continue; }
            if (gate_exempt_.count({ bm.group, bm.chords[static_cast<std::size_t>(c)] }))
                continue;   // a pair inject's exemption spans both band rows
            if (inflation_ && (!have_carried || bm.chord_carried[static_cast<std::size_t>(c)]))
                inflation_->addRow(kInflationFamily, kInflationSat,
                                   v(i) / std::sqrt(var), /*gap_s=*/0.0,
                                   /*post_var=*/0.0, /*fold_var=*/1.0);
            if (bornSkipped(have_carried, bm.chord_carried,
                            static_cast<std::size_t>(c), var)) continue;
            if (v(i) * v(i) <= k2f * var) continue;
            // A pair chord's corroboration is keyed by the graph chord, so both band rows are
            // suppressed together, as with gate_exempt_ above.  Splitting them would re-tap
            // one band of a continuous pair and leave the other carried. (WL-3)
            if (d5Suppressed(bm.group, bm.chords[static_cast<std::size_t>(c)],
                             v(i) * v(i), var))
                continue;
            // Diagnostic-only window, as in d5GateHalf (RS-2b).  Decided per band row, like
            // every other decision in this body; the funnel mask is keyed by the graph
            // chord either way, so a pair whose two rows differ still leaves the arc unmarked.
            if (cfg_.innov_diagnostic_only) {
                last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(c)]);
                last_innov_sigmas_.push_back(std::abs(v(i)) / std::sqrt(var));
                last_innov_terms_.push_back({bm.zhat(i), v(i), var, sigma0, cb.z(i)});
                last_innov_warmup_.push_back(1);
                ++counters_.innov_flags;
                ++counters_.d5_warmup_suppressed;
                continue;
            }
            cb.z(i) = bm.zhat(i) + v(i);
            cb.Pz.row(i).setZero();
            cb.Pz.col(i).setZero();
            cb.Pz(i, i) = sigma0 * sigma0 * var;
            last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(c)]);
            last_innov_sigmas_.push_back(std::abs(v(i)) / std::sqrt(var));
            last_innov_terms_.push_back({bm.zhat(i), v(i), var, sigma0, cb.z(i)});
            last_innov_warmup_.push_back(0);
            ++counters_.innov_flags;
            ++counters_.innov_retaps;
            laneFixErase(bm.group, bm.chords[static_cast<std::size_t>(c)]);
        }
    };
    // --- Degenerate-block rebirth (PD-safe INC-2a, SPEC §5a) -------------------------
    // A block whose Pᶻ+B could not factor after the equilibrated screen and the
    // eigenvalue-clamp repair folds inertly (M = 0): it takes no data this epoch and its
    // posterior equals its prior.  Left alone, the block stays decoupled permanently, since
    // the prior never improves and so never factors.
    //
    // Invariant #7 requires the re-coupling edge to re-seed what it reconnects.  Rebirth
    // keeps the means ẑ (the block's branch, i.e. which integer realization the phase side
    // landed on; the fold left them untouched) and resets Pᶻ to the fresh diffuse diagonal.
    // The next epoch's diffuse prior then takes the data-only tap-off: a whole-block birth
    // that loses one epoch of information, the same semantics as a chord birth.
    //
    // It runs after the D5 gate on purpose: a degenerate block's Cov(v) has non-positive
    // diagonals, so the gate skipped those rows (counted), and any row it could test keeps
    // its data-only mean while the covariance is re-seeded uniformly.
    //
    // The fresh diagonal is isotropic even on a pair block: the lane-shaped fresh prior
    // (@c sigma_wl_fresh_cyc) belongs to buildDdPriors, and an isotropic diffuse reset is the
    // conservative choice for a block just declared unusable.  A rebirth is a prior reset,
    // not a slip; the slip funnel is not touched.
    auto rebirthBand = [this](CarriedBlock& cb, Eigen::Index off, Eigen::Index n,
                              std::size_t b, const BlockMeta& bm, double min_pivot_rel) {
        if (n <= 0 || cb.Pz.rows() < off + n || cb.Pz.cols() < off + n) return;
        cb.Pz.middleRows(off, n).setZero();
        cb.Pz.middleCols(off, n).setZero();
        cb.Pz.block(off, off, n, n).diagonal().setConstant(cfg_.fresh_prior_var);
        FoldRebirth fr;
        fr.block         = b;
        fr.group         = bm.group;
        fr.k             = static_cast<long long>(n);
        fr.min_pivot_rel = min_pivot_rel;
        if (!bm.chords.empty()) fr.chord0 = bm.chords.front();
        last_fold_rebirths_.push_back(fr);
        ++counters_.ddfold_rebirths;
        counters_.ddfold_rebirth_rows += static_cast<long long>(n);
    };
    // Query the kernel's per-block PD verdict, with a bounds check.  block_meta_ and the
    // kernel's fold slots are index-aligned by contract; an install against a stale
    // estimator reads as "not degenerate" rather than throwing.
    auto foldDegenerate = [&est](std::size_t b) {
        return b < est.numDdPriorBlocks() && est.ddPriorBlockDegenerate(b);
    };
    auto foldMinPivot = [&est](std::size_t b) {
        return b < est.numDdPriorBlocks() ? est.ddPriorBlockMinPivotRel(b) : 0.0;
    };

    // Commit this epoch's staged fold-on-change decisions and purge the memory of chords no
    // longer carried as pair chords.  A D5 flag above has already erased its chord from both
    // maps, so a flag takes precedence over a fix.  No-op (no allocation, no fp) on the
    // per-band path. (WL-3)
    auto laneFixCommit = [this]() {
        if (lane_fix_staged_.empty() && lane_fix_w_.empty()) return;
        // Stamp each re-decided chord with this epoch's index (the skip grace clock).  Only
        // staged (re-decided) chords advance; a held chord that was not re-decided keeps its
        // old stamp, so the grace window can expire and increments resume.  Gated on the
        // toggle, so lane_fix_epoch_ stays empty (no map cost) when INC-2 is off. (LF-1 INC-2)
        if (cfg_.skip_pinned_increments)
            for (const auto& [key, w] : lane_fix_staged_)
                lane_fix_epoch_[key] = install_epoch_index_;
        for (const auto& [key, w] : lane_fix_staged_) lane_fix_w_[key] = w;
        lane_fix_staged_.clear();
        std::set<std::pair<int, ArcKey>> live;
        for (const auto& bm : block_meta_)
            if (bm.pair_band == 1 || bm.pair_band == 3)
                for (const auto& c : bm.chords) live.insert({bm.group, c});
        for (auto it = lane_fix_w_.begin(); it != lane_fix_w_.end(); ) {
            if (live.count(it->first)) ++it;
            else it = lane_fix_w_.erase(it);
        }
        // Prune the epoch clock in lockstep (a chord no longer carried resumes increments if reborn).
        for (auto it = lane_fix_epoch_.begin(); it != lane_fix_epoch_.end(); )
            it = live.count(it->first) ? std::next(it) : lane_fix_epoch_.erase(it);
    };

    if (n_threads > 1) {
        const std::size_t NB = block_meta_.size();
        struct InstallSlot {
            CarriedBlock    cb;
            Eigen::VectorXd v;                 // innovation (only when innov_gate && K > 0)
            Eigen::MatrixXd cov_v;             // its H0 covariance
            bool            has_innov{false};
        };
        std::vector<InstallSlot> slots(NB);

        // (1, parallel over block index) Const readouts into per-block slots only.  The
        // driver pins Eigen to one thread, so there is no nested threading.  A pair
        // component's split halves or joint block read like any other block; the stitching
        // stays in the serial phase.
        #pragma omp parallel for schedule(dynamic) num_threads(n_threads)
        for (long long bi = 0; bi < static_cast<long long>(NB); ++bi) {
            const std::size_t b = static_cast<std::size_t>(bi);
            const BlockMeta& bm = block_meta_[b];
            InstallSlot& S = slots[b];
            S.cb.group    = bm.group;
            S.cb.chords   = bm.chords;
            S.cb.vertices = bm.vertices;
            S.cb.arcs     = bm.arcs;
            S.cb.zl       = bm.zl;   // the frame the installed means are relative to (RS-2)
            S.cb.z  = est.ddPriorPosteriorMean(b, x_s);
            S.cb.Pz = est.ddPriorPosteriorCovariance(b, cov_free);
            if (cfg_.innov_gate && static_cast<Eigen::Index>(bm.chords.size()) > 0) {
                est.ddPriorInnovation(b, x_s, cov_free, S.v, S.cov_v);
                S.has_innov = true;
            }
        }

        // (2, serial in (block b, chord i) order) σ̂₀ feed, gate, flags and install.  The
        // call sequence matches the serial path, so σ̂₀, the counters and carried_ match too.
        std::vector<CarriedBlock> next;
        next.reserve(NB);
        for (std::size_t b = 0; b < NB; ++b) {
            const BlockMeta& bm = block_meta_[b];
            if (bm.pair_band == 2) continue;   // stitched with its band-1 sibling at b-1 (WL-3)
            InstallSlot& S = slots[b];
            const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
            if (bm.pair_band == 0) {
                if (cfg_.innov_gate && K > 0 && S.has_innov)
                    d5GateHalf(bm, S.v, S.cov_v, S.cb, 0);
                if (cfg_.pd_safe && K > 0 && foldDegenerate(b))   // rebirth (INC-2a)
                    rebirthBand(S.cb, 0, S.cb.Pz.rows(), b, bm, foldMinPivot(b));
                next.push_back(std::move(S.cb));
                continue;
            }
            // Pair component: stitch the joint carried block. (WL-3)
            CarriedBlock cb;
            cb.group    = bm.group;
            cb.chords   = bm.chords;
            cb.vertices = bm.vertices;
            cb.arcs     = bm.arcs;
            cb.pair     = true;
            cb.lambda1  = bm.lambda1;
            cb.lambda2  = bm.lambda2;
            if (bm.pair_band == 1 && b + 1 < NB && block_meta_[b + 1].pair_band == 2) {
                InstallSlot& S2 = slots[b + 1];
                cb.z.resize(2 * K);
                cb.z << S.cb.z, S2.cb.z;
                if (S.cb.zl.size() == K && S2.cb.zl.size() == K) {   // same [b1; b2] layout (RS-2)
                    cb.zl.resize(2 * K);
                    cb.zl << S.cb.zl, S2.cb.zl;
                }
                cb.Pz = Eigen::MatrixXd::Zero(2 * K, 2 * K);   // cross-band coupling zeroed (§6.1)
                cb.Pz.topLeftCorner(K, K)     = S.cb.Pz;
                cb.Pz.bottomRightCorner(K, K) = S2.cb.Pz;
                if (cfg_.innov_gate && K > 0) {
                    if (S.has_innov)  d5GateHalf(bm, S.v, S.cov_v, cb, 0);
                    if (S2.has_innov) d5GateHalf(block_meta_[b + 1], S2.v, S2.cov_v, cb, K);
                }
                if (cfg_.pd_safe && K > 0) {   // each emitted half is screened separately (INC-2a)
                    if (foldDegenerate(b))     rebirthBand(cb, 0, K, b, bm, foldMinPivot(b));
                    if (foldDegenerate(b + 1)) rebirthBand(cb, K, K, b + 1,
                                                           block_meta_[b + 1],
                                                           foldMinPivot(b + 1));
                }
            } else {   // pair_band == 3: the joint lane-shape block (coupling retained)
                cb.z  = std::move(S.cb.z);
                cb.zl = std::move(S.cb.zl);   // already the joint 2K layout (RS-2)
                cb.Pz = std::move(S.cb.Pz);
                if (cfg_.innov_gate && K > 0 && S.has_innov)
                    d5GateJoint(bm, S.v, S.cov_v, cb);
                if (cfg_.pd_safe && K > 0 && foldDegenerate(b))   // whole joint 2K block (INC-2a)
                    rebirthBand(cb, 0, cb.Pz.rows(), b, bm, foldMinPivot(b));
            }
            next.push_back(std::move(cb));
        }
        carried_.swap(next);
        gate_exempt_.clear();   // the one-epoch inject exemption is consumed here (WL-2 §3.3)
        innov_flat_.clear();    // the one-epoch corroboration set is consumed here (D5 v3)
        laneFixCommit();        // fold-on-change memory commits on the accepted epoch (WL-3 §3.3)
        ++install_epoch_index_; // advance the skip grace clock, accepted installs only (LF-1 INC-2)
        counters_.z_dim = static_cast<long long>(totalZDim());
        if (counters_.z_dim > counters_.max_z_dim) counters_.max_z_dim = counters_.z_dim;  // INC-C0
        return;
    }

    // ---- Serial install (n_threads == 1, the default; the byte-identical path) ---
    std::vector<CarriedBlock> next;
    next.reserve(block_meta_.size());
    if (perf_timing_) ++perf_timers_.install_epochs;   // INC-C0
    for (std::size_t b = 0; b < block_meta_.size(); ++b) {
        const BlockMeta& bm = block_meta_[b];
        if (bm.pair_band == 2) continue;   // stitched with its band-1 sibling at b-1 (WL-3)
        CarriedBlock cb;
        cb.group    = bm.group;
        cb.chords   = bm.chords;
        cb.vertices = bm.vertices;
        cb.arcs     = bm.arcs;
        cb.zl       = bm.zl;   // the frame the installed means are relative to (RS-2)
        const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());

        if (bm.pair_band == 1 && b + 1 < block_meta_.size()
            && block_meta_[b + 1].pair_band == 2) {
            // --- Split pair (WL-3 §6.1): stitch two per-band posteriors into the joint
            // [band-1; band-2] carried block with the cross-band coupling zeroed, the
            // equivalence shape that carries the same information as two per-band groups.
            // D5 gates each emitted half in (b, b+1) row order, the per-band call sequence.
            const BlockMeta& bm2 = block_meta_[b + 1];
            cb.pair    = true;
            cb.lambda1 = bm.lambda1;
            cb.lambda2 = bm.lambda2;
            Eigen::VectorXd z1, z2;
            Eigen::MatrixXd P1, P2;
            { ScopedAccum _pm(perf_timing_, &perf_timers_.post_mean_ms);
              z1 = est.ddPriorPosteriorMean(b, x_s);
              z2 = est.ddPriorPosteriorMean(b + 1, x_s); }
            { ScopedAccum _pc(perf_timing_, &perf_timers_.post_cov_ms);
              P1 = est.ddPriorPosteriorCovariance(b, cov_free);
              P2 = est.ddPriorPosteriorCovariance(b + 1, cov_free); }
            cb.z.resize(2 * K);
            cb.z << z1, z2;
            if (bm.zl.size() == K && bm2.zl.size() == K) {   // same [b1; b2] layout (RS-2)
                cb.zl.resize(2 * K);
                cb.zl << bm.zl, bm2.zl;
            } else {
                cb.zl.resize(0);
            }
            cb.Pz = Eigen::MatrixXd::Zero(2 * K, 2 * K);
            cb.Pz.topLeftCorner(K, K)     = P1;
            cb.Pz.bottomRightCorner(K, K) = P2;
            if (cfg_.innov_gate && K > 0) {
                ScopedAccum _d5(perf_timing_, &perf_timers_.d5_gate_ms);
                Eigen::VectorXd v;
                Eigen::MatrixXd cov_v;
                est.ddPriorInnovation(b, x_s, cov_free, v, cov_v);
                d5GateHalf(bm, v, cov_v, cb, 0);
                est.ddPriorInnovation(b + 1, x_s, cov_free, v, cov_v);
                d5GateHalf(bm2, v, cov_v, cb, K);
            }
            if (cfg_.pd_safe && K > 0) {   // each emitted half is screened separately (INC-2a)
                if (foldDegenerate(b))     rebirthBand(cb, 0, K, b, bm, foldMinPivot(b));
                if (foldDegenerate(b + 1)) rebirthBand(cb, K, K, b + 1, bm2,
                                                       foldMinPivot(b + 1));
            }
            next.push_back(std::move(cb));
            continue;
        }
        if (bm.pair_band == 3) {
            // --- Lane shape (WL-3): the joint 2K posterior with the cross-band coupling
            // retained (the jointly covarianced pair, §2); D5 runs per band row.
            cb.pair    = true;
            cb.lambda1 = bm.lambda1;
            cb.lambda2 = bm.lambda2;
            { ScopedAccum _pm(perf_timing_, &perf_timers_.post_mean_ms);
              cb.z  = est.ddPriorPosteriorMean(b, x_s); }
            { ScopedAccum _pc(perf_timing_, &perf_timers_.post_cov_ms);
              cb.Pz = est.ddPriorPosteriorCovariance(b, cov_free); }
            if (cfg_.innov_gate && K > 0) {
                ScopedAccum _d5(perf_timing_, &perf_timers_.d5_gate_ms);
                Eigen::VectorXd v;
                Eigen::MatrixXd cov_v;
                est.ddPriorInnovation(b, x_s, cov_free, v, cov_v);
                d5GateJoint(bm, v, cov_v, cb);
            }
            if (cfg_.pd_safe && K > 0 && foldDegenerate(b))   // whole joint 2K block (INC-2a)
                rebirthBand(cb, 0, cb.Pz.rows(), b, bm, foldMinPivot(b));
            next.push_back(std::move(cb));
            continue;
        }

        { ScopedAccum _pm(perf_timing_, &perf_timers_.post_mean_ms);   // INC-C0
          cb.z  = est.ddPriorPosteriorMean(b, x_s); }
        { ScopedAccum _pc(perf_timing_, &perf_timers_.post_cov_ms);    // dense K×K install (INC-C0)
          cb.Pz = est.ddPriorPosteriorCovariance(b, cov_free); }

        // --- Estimation-side fine-slip innovation gate (card 4 D5) ------------------
        // The carried DD's coarse slip layer (MW/LLI/IF-TD) is deliberately relaxed; a finer
        // slip that the coarse layer missed inflates the carried DD's post-solve innovation
        // v = z_free − Gᵀx_s − ẑ.  A chord whose standardized innovation exceeds k·σ̂₀ is
        // treated as slipped and installed fresh here: its data-only tap-off, mean d = v+ẑ,
        // variance σ̂₀²·Cov(v)_ii (the calibrated innovation variance, not the smaller formal
        // one), cross-terms zeroed.  The stale value therefore does not propagate, and its
        // (group, chord) is recorded so the caller marks its arc into the next epoch's slip
        // funnel (the C3c re-tap).  With the gate off the posterior is installed
        // unconditionally (byte-identical to the C3c path).  The per-chord body is
        // d5GateHalf at offset 0, shared with the parallel path and the pair shapes above.
        if (cfg_.innov_gate && K > 0) {
            ScopedAccum _d5(perf_timing_, &perf_timers_.d5_gate_ms);   // INC-C0
            Eigen::VectorXd v;
            Eigen::MatrixXd cov_v;
            est.ddPriorInnovation(b, x_s, cov_free, v, cov_v);
            d5GateHalf(bm, v, cov_v, cb, 0);
        }
        if (cfg_.pd_safe && K > 0 && foldDegenerate(b))   // rebirth (INC-2a)
            rebirthBand(cb, 0, cb.Pz.rows(), b, bm, foldMinPivot(b));
        next.push_back(std::move(cb));
    }
    carried_.swap(next);
    gate_exempt_.clear();   // the one-epoch inject exemption is consumed here (WL-2 §3.3)
    innov_flat_.clear();    // the one-epoch corroboration set is consumed here (D5 v3)
    laneFixCommit();        // fold-on-change memory commits on the accepted epoch (WL-3 §3.3)
    ++install_epoch_index_; // advance the skip grace clock, accepted installs only (LF-1 INC-2)
    counters_.z_dim = static_cast<long long>(totalZDim());
    if (counters_.z_dim > counters_.max_z_dim) counters_.max_z_dim = counters_.z_dim;  // INC-C0
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the PD-safe invariant-DD fold.
int networkCarryDdFoldRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkInvariantCarryDriver& carry_driver = v.run.engines.carry_driver;

    // --- PD-safe invariant-DD fold run-end telemetry (SPEC_pd_safe_dd_fold.md) ------
    // An inert fold (`M := 0`, the fallback when Pᶻ+B fails the PD screen and is not
    // repaired) leaves the block's posterior equal to its prior; the D5 gate skips its
    // rows because Cov(v) inherits the negative diagonals, and dd_sigma_cyc and the AR
    // probe read a covariance that does not show the failure.  The INC-0 counters are
    // always on, so a carry_invariant run always gets a line here, armed or not; the
    // branch below covers the zero-fold case so that it too prints a line (SPEC §3,
    // "Run-end summary always"; invariant #4).  Keep those two branches exhaustive over
    // `carry_invariant`.
    if (ctx.knobs.carry_invariant && ctx.counters.ddfold_epochs > 0) {
        const auto& c = ctx.counters;
        const double degen_frac = (c.ddfold_fold_rows > 0)
            ? static_cast<double>(c.ddfold_degen_rows)
              / static_cast<double>(c.ddfold_fold_rows) : 0.0;
        log->info("Network (Card 4 PD-safe fold, INC-0): {} epoch(s) folded {} DD-prior "
                  "block(s) / {} carried row(s); {} block-epoch(s) and {} row-epoch(s) "
                  "({:.2f} % of the carried dimension) folded INERTLY on a failed PD "
                  "screen, over {} epoch(s).  Worst relative pivot d_min/d_max = {:.6g}.  "
                  "Edges: {} healthy->degenerate, {} degenerate->healthy.  D5 rows the "
                  "gate could not test (Cov(v) not positive): {}.",
                  c.ddfold_epochs, c.ddfold_folds, c.ddfold_fold_rows,
                  c.ddfold_degen_blocks, c.ddfold_degen_rows, 100.0 * degen_frac,
                  c.ddfold_degen_epochs, c.ddfold_min_pivot_rel,
                  c.ddfold_transitions, c.ddfold_recoveries,
                  carry_driver.counters().d5_nonpos_var_rows);
        if (ctx.knobs.carry_invariant_pd_safe) {
            log->info("Network (Card 4 PD-safe fold, INC-1/2): ARMED — {} eigen-clamp "
                      "repair(s) over {} row(s) (worst clamped |lambda|/lambda_max = "
                      "{:.6g}; {} fold(s) above the 1e-6 'this is not dust' bar), {} "
                      "block rebirth(s) re-seeding {} carried row(s), {} block(s) "
                      "rebirthed at warm-resume restore.",
                      c.ddfold_repairs, c.ddfold_repair_rows, c.ddfold_repair_worst,
                      c.ddfold_repair_loud, c.ddfold_rebirths, c.ddfold_rebirth_rows,
                      carry_driver.resumeCarryRebirthBlocks());
            if (c.ddfold_repair_loud > 0)
                log->warn("Network (Card 4 PD-safe fold, INC-1c): {} fold(s) needed a "
                          "repair LARGER than 1e-6 x lambda_max.  The repair exists for "
                          "round-off dust; a clamp this size means the block arrived "
                          "genuinely broken.  Do NOT read the repaired covariance as "
                          "healthy — find the upstream defect.", c.ddfold_repair_loud);
            if (c.ddfold_degen_rows > 0 && c.ddfold_rebirths == 0)
                log->warn("Network (Card 4 PD-safe fold, INC-2a): {} row-epoch(s) folded "
                          "inertly yet ZERO blocks were rebirthed — the re-coupling edge "
                          "never fired, so those blocks were decoupled with no bounded "
                          "response (standing invariant #7).  This is a WIRING failure, "
                          "not a quiet run.", c.ddfold_degen_rows);
        } else if (c.ddfold_degen_rows > 0) {
            log->warn("Network (Card 4 PD-safe fold): {} row-epoch(s) ({:.2f} % of the "
                      "carried dimension) folded INERTLY and network."
                      "carry_invariant_pd_safe is OFF — those blocks took NO data and, "
                      "their priors never improving, will not factor again: the "
                      "decoupling is PERMANENT and unbounded.  Anything graded on this "
                      "run's carry is graded on a frozen substrate.",
                      c.ddfold_degen_rows, 100.0 * degen_frac);
        }
    } else if (ctx.knobs.carry_invariant) {
        // Zero-fold branch of the run-end summary (SPEC §3, "Run-end summary always").
        // The gate above is `ddfold_epochs > 0`, and that counter advances only on an
        // accepted install whose fold saw blocks (NetworkInstallCarry, under
        // `if (fs.blocks > 0)`).  Zero is reachable with no fault at all: buildCarryDdPriors
        // emits nothing when there is no shared datum or no foldable chord, and the fold
        // returns early before it tallies.  Without this branch a carry_invariant run could
        // print no INC-0 line, and d5_nonpos_var_rows (read out only in the block above)
        // would go unreported.  The per-epoch trace columns are gated on the same
        // `blocks > 0`, so they give no fallback either, and a run with no line reads as
        // "clean".
        log->warn("Network (Card 4 PD-safe fold, INC-0): network.carry_invariant is ON but "
                  "the DD-prior fold ran on ZERO epochs — no accepted install ever reached a "
                  "factorization, so every INC-0 tally is structurally empty and this run is "
                  "evidence about NOTHING in the card (standing invariant #4: inert wiring "
                  "must be VISIBLE — this line is the only place it is).  Expected on a run "
                  "with no shared datum or no foldable chord; unexpected otherwise, and then "
                  "it means the carry never engaged.{}  D5 rows the gate could not test "
                  "(Cov(v) not positive): {}.",
                  ctx.knobs.carry_invariant_pd_safe
                      ? "  network.carry_invariant_pd_safe is ARMED and likewise never "
                        "executed — the arm proves nothing on this run."
                      : "",
                  carry_driver.counters().d5_nonpos_var_rows);
    } else if (ctx.knobs.carry_invariant_pd_safe) {
        log->warn("Network (Card 4 PD-safe fold): ARMED but the DD-prior fold ran on ZERO "
                  "epochs — the wiring never executed (invariant #4).  Nothing in this run "
                  "is evidence about the card.");
    }
    return 0;
}

// Run-end report: the card 4/M1 invariant carry.
int networkInvariantCarryRunEnd(const NetworkRunEndView& v) {
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const bool carry_invariant = v.ctx.knobs.carry_invariant;
    const auto& carry_epochs = v.ctx.carry.carry_epochs;
    const auto& carry_born_total = v.ctx.carry.carry_born_total;
    const auto& carry_marg_total = v.ctx.carry.carry_marg_total;
    const auto& carry_merges_total = v.ctx.carry.carry_merges_total;
    const auto& carry_splits_total = v.ctx.carry.carry_splits_total;
    const auto& carry_swaps_total = v.ctx.carry.carry_swaps_total;
    const auto& carry_zdim_last = v.ctx.carry.carry_zdim_last;
    const auto& carry_slipkills_total = v.ctx.carry.carry_slipkills_total;
    const auto& carry_dropped_total = v.ctx.carry.carry_dropped_total;

    if (carry_invariant)
        log->info("Network (Card 4/M1 invariant carry): {} epoch(s) driven, {} carried DD(s) at "
                  "run end; {} chord birth(s), {} marginalization(s), {} slip refloat(s), "
                  "{} unresolvable-cycle drop(s), {} tree swap(s) (basis re-expression), "
                  "{} component merge(s), {} split(s) over the run "
                  "(S-system-invariant DDs; q_z={} m²/s).",
                  carry_epochs, carry_zdim_last, carry_born_total, carry_marg_total,
                  carry_slipkills_total, carry_dropped_total,
                  carry_swaps_total, carry_merges_total, carry_splits_total,
                  config_.networkCarryInvariantQz());
    return 0;
}

// Run-end report: the D5 fine-slip innovation gate and its variants.
int networkD5GateRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkInvariantCarryDriver& carry_driver = v.run.engines.carry_driver;
    const bool carry_innov_gate = v.ctx.knobs.carry_innov_gate;
    const auto& carry_slipkills_total = v.ctx.carry.carry_slipkills_total;
    const auto& carry_innov_flags_total = v.ctx.carry.carry_innov_flags_total;
    const auto& carry_innov_retaps_total = v.ctx.carry.carry_innov_retaps_total;

    if (carry_innov_gate)
        // The warm-up clause is printed whenever the knob is armed, including at 0
        // suppressions (RS-2b): "armed and suppressed nothing" is a finding (the window
        // closed before any chord tripped, or nothing was restored to trip), and a clause
        // printed only on a nonzero count could not state it (invariant #4).
        log->info("Network (Card 4/M1 D5 fine-slip innovation gate): {} carried-DD innovation "
                  "flag(s), {} fresh re-tap(s) over the run (k={}; the estimation-side reset for "
                  "the fine slips the coarse MW/LLI/IF-TD layer is relaxed against — each flagged "
                  "chord re-taps data-only + marks its arc slipped for the next epoch).{}",
                  carry_innov_flags_total, carry_innov_retaps_total,
                  config_.networkCarryInvariantInnovK(),
                  config_.networkResumeD5WarmupEp() > 0
                      ? fmt::format("; {} trip(s) suppressed in the {}-epoch post-resume "
                                    "warm-up (counted, never re-tapped, never funnelled) "
                                    "over {} epoch(s) the window was open",
                                    carry_driver.counters().d5_warmup_suppressed,
                                    config_.networkResumeD5WarmupEp(),
                                    ctx.counters.n_resume_d5_warmup_epochs)
                      : std::string());
    if (carry_innov_gate && config_.networkCarryInvariantInnovCorroborate()) {
        const long long sup = carry_driver.counters().innov_suppressed;
        const long long tripped = carry_innov_flags_total + sup;
        log->info("Network (Card 4/M1 D5 v3 corroboration gate): {} of {} gate trips SUPPRESSED "
                  "({:.1f}%) because the raw observables measured the arc as CONTINUOUS; {} "
                  "re-tapped (raw step, or no raw evidence — absence of evidence never "
                  "suppresses).  Backstop k = {}.",
                  sup, tripped,
                  tripped > 0 ? 100.0 * static_cast<double>(sup) / static_cast<double>(tripped) : 0.0,
                  carry_innov_flags_total,
                  config_.networkCarryInvariantInnovCorroborateBackstopK());
        if (sup == 0)
            log->warn("Network (Card 4/M1 D5 v3): the corroboration gate is ARMED but suppressed "
                      "NOTHING — the flat set never reached the driver.  Standing invariant #4: "
                      "check that a slip-QC DECISION CARD ran the QUALIFY pass this run "
                      "(network.cycle_slip.gap_instrument / .mw_lli_corroborate / "
                      ".relevel_bridge_gf / the CS-GF authority — SlipQcState::cardsArmed()); "
                      "without one the gate is INERT, not permissive.  ⚠ network.trace_slip_raw "
                      "is NOT one of them: it COMPUTES the observables but adjudicates none, so "
                      "every verdict map is empty and nothing is ever taggable.");
    }
    if (carry_innov_gate && config_.networkCarryInvariantInnovCarriedOnly()) {
        const long long bs = carry_driver.counters().innov_born_skipped;
        log->info("Network (Card 4/M1 D5 v5 carried-only): {} chord-row(s) NOT gated because "
                  "the chord was BORN this epoch — no carried prior, so Cov(v) is the diffuse "
                  "fresh-chord variance and the standardized ratio is not a hypothesis test.  "
                  "Read against the {} flag(s) that DID fire: on the untreated 3 h reference "
                  "95.3% of all flags were born chords, and a chord re-borned every epoch can "
                  "never accumulate, which is what pins dd_sigma_cyc.  This is counted before "
                  "the threshold test, so it is the whole born population, not just the part "
                  "that would have flagged.",
                  bs, carry_innov_flags_total);
        if (bs == 0)
            log->warn("Network (Card 4/M1 D5 v5): carried-only is ARMED but skipped NOTHING — "
                      "no born chord ever reached the gate.  Standing invariant #4: INERT, not "
                      "benign.  Either the carry never births chords (check the invariant-carry "
                      "line's birth count) or the chord_carried mask is not being populated.");
    }
    if (carry_innov_gate && config_.networkCarryInvariantInnovRetapOnly()) {
        const long long sup = ctx.carry.carry_innov_kill_suppressed;
        log->info("Network (Card 4/M1 D5 v4 re-tap-only): {} carry-arc membership kill(s) "
                  "SUPPRESSED over the run — a D5 flag whose arc the raw observables MEASURED "
                  "as continuous no longer excludes that arc from A_old, so the in-epoch "
                  "re-tap the gate already installed survives instead of being refloated from "
                  "a diffuse prior.  A chord arc has cycle support 1 under a stable tree, so "
                  "no OTHER carried DD was protected.  Stepped / unmeasurable arcs, and any "
                  "arc a raw detector flagged in the same epoch, still killed their cycle.  "
                  "Read this against the {} slip refloat(s) the driver counted: the difference "
                  "is the churn this knob removed, and n_chord_births in "
                  "network_epoch_status.csv is where the chord-lifetime effect shows up.  "
                  "[Counted per arc per build call — a within-epoch RAIM re-solve counts its "
                  "arcs twice, the same convention as chords_born.]",
                  sup, carry_slipkills_total);
        if (sup == 0)
            log->warn("Network (Card 4/M1 D5 v4): re-tap-only is ARMED but suppressed NOTHING — "
                      "no corroborated-flat D5 flag ever reached a carry arc.  Standing "
                      "invariant #4: the knob is INERT this run, not benign.  Check, in order: "
                      "did the D5 gate flag at all (the innovation-gate line above); did a "
                      "slip-QC DECISION CARD run the QUALIFY pass (cardsArmed() — "
                      "gap_instrument / mw_lli_corroborate / relevel_bridge_gf / the CS-GF "
                      "authority; network.trace_slip_raw does NOT qualify, it computes without "
                      "adjudicating, so every flag reads 'no evidence' and none is taggable); "
                      "did the flags resolve through the "
                      "reverse map (carry_innov_map_miss); and is "
                      ".carry_invariant_innov_corroborate also armed, which suppresses the same "
                      "population one level earlier and leaves this correctly at zero.");
    }
    if (carry_innov_gate && config_.networkCarryInvariantInnovCalibrate()) {
        // σ̂₀ clamp visibility.  A clamped 1.00× looks the same as a calibrated 1.00×, so the
        // clamped σ̂₀ alone cannot show that the D5 gate is running as the raw formal (v1)
        // test (SPEC_pd_safe_dd_fold.md §10 finding 2).  The line reports the state
        // histogram and the raw range, and warns when the calibration was inert for most of
        // the run: an armed knob at its neutral value is not benign (invariant #4).
        const auto& cc  = carry_driver.counters();
        const auto  st  = carry_driver.innovInflationStatus();
        const long long den = std::max<long long>(1, cc.d5_calib_epochs);
        const long long inert = cc.d5_calib_warmup_epochs + cc.d5_calib_floored_epochs;
        const double inert_pct = 100.0 * static_cast<double>(inert) / static_cast<double>(den);
        const std::string raw_range =
            (cc.d5_calib_raw_max > 0.0)
                ? fmt::format("{:.4g} … {:.4g}", cc.d5_calib_raw_min, cc.d5_calib_raw_max)
                : std::string{"(never estimated past warm-up)"};
        // The two σ̂₀² figures are different quantities on purpose: the final one is the
        // post-run accumulator state (it includes the last install's own rows, which no gate
        // consulted), while the range covers the values the gate used.  The final value can
        // therefore sit outside the range; that is correct.
        log->info("Network (Card 4/M1 D5 v2 LS-VCE calibration): post-run empirical inflation "
                  "σ̂₀ = {:.2f}× in state '{}' (raw σ̂₀² = {:.4g}, n_eff = {:.0f}; gate threshold "
                  "k·σ̂₀ = {:.1f} formal-σ).  Over {} install(s) the σ̂₀ state was: active {} / "
                  "floored {} / warm-up {} / capped {}.  RAW σ̂₀² the gate CONSULTED past "
                  "warm-up: {} (the post-run value above is one accumulator step later).  "
                  "σ̂₀>1 quantifies how optimistic the formal Cov(v) is — the M2 monitor's "
                  "finding, closing the loop on the gate itself.",
                  st.sigma0, NetworkInvariantCarryDriver::inflationStateName(st.state),
                  st.raw_sigma0_sq, st.n_eff,
                  config_.networkCarryInvariantInnovK() * st.sigma0,
                  cc.d5_calib_epochs, cc.d5_calib_active_epochs, cc.d5_calib_floored_epochs,
                  cc.d5_calib_warmup_epochs, cc.d5_calib_capped_epochs, raw_range);
        if (cc.d5_calib_floored_epochs > 0)
            log->warn("Network (Card 4/M1 D5 v2 LS-VCE calibration): the σ̂₀² FLOOR CLAMP bound "
                      "on {} of {} install(s) ({:.1f}% of the run counting warm-up) — over "
                      "those epochs the calibration was ARMED but the gate ran as the RAW "
                      "FORMAL (v1) test, because the raw σ̂₀² fell below 1 (min {:.4g}) and the "
                      "clamp only RAISES the threshold.  A reported σ̂₀ of 1.00× over a floored "
                      "run is NOT evidence that the formal Cov(v) is honest; it is evidence "
                      "the calibration was inert.  A raw σ̂₀² far below 1 means the "
                      "innovations are far SMALLER than the formal Cov(v) predicts — the "
                      "classic signature of a carry that is not taking data (frozen or "
                      "over-diffuse priors), so read the ddfold counters above before reading "
                      "any D5 conclusion from this run.",
                      cc.d5_calib_floored_epochs, cc.d5_calib_epochs, inert_pct,
                      cc.d5_calib_raw_min);
        else if (cc.d5_calib_active_epochs == 0)
            log->warn("Network (Card 4/M1 D5 v2 LS-VCE calibration): the calibration NEVER "
                      "reached the active state over {} install(s) — {} in warm-up, {} capped. "
                      " Standing invariant #4: the knob is INERT this run, not benign; every "
                      "D5 number below came from a gate that was not calibrated.",
                      cc.d5_calib_epochs, cc.d5_calib_warmup_epochs, cc.d5_calib_capped_epochs);
    }
    return 0;
}

// Run-end report: D5 flags missing from the carry map.
int networkD5MapMissRunEnd(const NetworkRunEndView& v) {
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const bool carry_innov_gate = v.ctx.knobs.carry_innov_gate;
    const auto& carry_innov_map_miss = v.ctx.carry.carry_innov_map_miss;

    if (carry_innov_gate && carry_innov_map_miss > 0)
        log->warn("Network (Card 4/M1 D5): {} innovation flag(s) over the run did not resolve in the "
                  "flag→ambiguity reverse map (next-epoch re-tap skipped for them) — a reverse-map/"
                  "block wiring inconsistency; investigate.", carry_innov_map_miss);
    return 0;
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor

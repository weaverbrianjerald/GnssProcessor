/**
 * @file NetworkInvariantCarryDriver.cpp
 * @brief Implementation of the DD invariant-carry driver (card 4, M1 phase C3b).
 *        The design rationale is in NetworkInvariantCarryDriver.hpp.
 */

#include "network/carry/NetworkInvariantCarryDriver.hpp"

#include "network/ar/NetworkArcGraph.hpp"   // Dsu, singleTreeOf, fundamentalCycle, chordRestriction (card AGT)

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
#include "network/state/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/runend/NetworkRunEnd.hpp"
#include "network/driver/NetworkRunGroups.hpp"
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
    last_innov_terms_.clear();
    innov_flat_.clear();       // corroboration set is cleared with the carry (D5 v3)
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
        // Fields 2-4 (the pair flag, λ₁ and λ₂) belonged to the retired WL-3 lane pair
        // blocks.  They are written as zeros so the 9-field header keeps its format: a blob
        // written without the lane carry is byte-identical, and restoreCarry refuses a
        // non-zero pair flag.
        os << fmt::format("B,{},0,0,0,{},{},{},{}\n", b.group, static_cast<long long>(zdim),
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
    // innovation side-state survives the install.  The
    // counters are also cleared here, and only here.  reset() alone keeps the run tallies
    // (it is a mid-run carry cold start), whereas an installed carry starts a new count,
    // and tallies from earlier driver work would otherwise be attributed to it. (RE-4)
    reset();
    counters_ = Counters{};

    double seed_sigma0_sq = 0.0, seed_rows = 0.0;   // from the optional S line (RE-1)
    bool   saw_seed_line = false;                   // an S line was present (RE-1)
    long long n_diag_blocks = 0;                    // blocks read from a PD row (RTB-7(e))
    long long n_rebirth_blocks = 0;                 // blocks rebirthed at restore (PD-safe INC-2c)
    long long n_lin_blocks = 0;                     // blocks read with a ZL row (RS-2)
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
        // A non-zero pair flag marks a WL-3 lane pair block (joint 2K storage), which only
        // the retired lane carry wrote.  It cannot be read as a per-band block (nothing here
        // checks zdim against the chord count), so it is refused and the resume counts a
        // cold start.
        if (pr != 0)
            return fail("carry: lane pair block (retired WL-3 lane carry) cannot be "
                        "restored: '" + line + "'");
        CarriedBlock b;
        b.group   = static_cast<int>(grp);
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
        // The row is restored and counted; the block is carried unchanged either way (no
        // reader re-bases against it). (RS-2)
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
            // A carry blob written before the PD-safe fix can hold a corrupt
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

void NetworkInvariantCarryDriver::beginEpoch(double dt_s, double content_s, bool clean) {
    // Advance the σ̂₀² accumulator's exponential forgetting once per epoch, before this
    // epoch's rows are folded in installPosterior.  Independent of q_z. (D5 v2)
    if (inflation_) inflation_->beginEpoch(content_s, clean);
    if (cfg_.qz <= 0.0 || dt_s <= 0.0) return;   // constant DDs (default): no q_z step
    ScopedAccum _tu(perf_timing_, &perf_timers_.time_update_ms);   // INC-C0
    const double add = cfg_.qz * dt_s;
    for (auto& cb : carried_) {
        if (cb.Pz.rows() == 0) continue;
        cb.Pz.diagonal().array() += add;
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
    ++counters_.epochs;
    if (perf_timing_) ++perf_timers_.build_epochs;   // INC-C0

    // The carry this epoch projects from; `carried_` is rebuilt in installPosterior.
    const std::vector<CarriedBlock>& prev = carried_;
    // Per previous block, the number of new components it contributes to (> 1 on a split).
    std::vector<int> prev_hit(prev.size(), 0);

    // Whether this build needs the per-arc linearization: the write knob (carry_lin) needs it
    // at every harvest, since serializeCarry stores it.  Off, zl stays empty and none of the
    // linearization code below runs. (RS-2)
    const bool lin_on = cfg_.carry_lin;

    for (const GroupInput& g : groups) {
        // --- (1) Assemble this epoch's graph for the group. -------------------
        struct ArcInfo {
            int est_station; int local_col; bool tree_edge;
            double x_lin;   // this arc's linearization value, metres (RS-2)
        };
        std::map<ArcKey, ArcInfo> arcinfo;
        std::set<ArcKey>          slipped_arcs;   // arcs whose carried ambiguity slipped this epoch (C3c)
        // The shared link rule (card AGT): root(rx) under root(sat), in g.arcs order, never sorted.
        arcgraph::Dsu uf;
        std::vector<ArcKey> group_tree_edges;   // for per-component tree extraction
        for (const ArcInput& a : g.arcs) {
            if (a.rx_vertex == a.sat_vertex) continue;         // degenerate (defensive)
            const ArcKey k(a.rx_vertex, a.sat_vertex);
            arcinfo[k] = {a.est_station, a.local_col, a.tree_edge, a.x_lin};
            arcgraph::dsuUnite(uf, a.rx_vertex, a.sat_vertex);
            if (a.tree_edge) group_tree_edges.push_back(k);
            if (a.slipped) slipped_arcs.insert(k);
        }
        if (arcinfo.empty()) continue;

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
                bool overlap = false;
                for (int v : ob.vertices) if (verts.count(v)) { overlap = true; break; }
                if (!overlap) continue;
                contributors.push_back(&ob);
                ++prev_hit[p];
                old_chords.insert(old_chords.end(), ob.chords.begin(), ob.chords.end());
                A_old.insert(ob.arcs.begin(), ob.arcs.end());
            }
            if (contributors.size() > 1) ++counters_.merges;
            const Eigen::Index n_old = static_cast<Eigen::Index>(old_chords.size());
            // Block-diagonal old covariance / mean over the contributors.
            Eigen::VectorXd z_old(n_old);
            Eigen::MatrixXd Pz_old = Eigen::MatrixXd::Zero(n_old, n_old);
            {
                Eigen::Index off = 0;
                for (const CarriedBlock* ob : contributors) {
                    const Eigen::Index k = static_cast<Eigen::Index>(ob->chords.size());
                    if (k == 0) continue;
                    z_old.segment(off, k) = ob->z;
                    Pz_old.block(off, off, k, k) = ob->Pz;
                    off += k;
                }
            }

            // Map an old chord arc → its column index in the old basis.
            std::map<ArcKey, Eigen::Index> old_col;
            for (Eigen::Index j = 0; j < n_old; ++j) old_col[old_chords[static_cast<std::size_t>(j)]] = j;

            // --- Per new chord: fundamental cycle, foldability, carried vs fresh.
            std::vector<ArcKey>                              fold_chords;
            std::vector<char>                                fold_carried;  // 1 = carried prior, 0 = born (D5 v2)
            std::vector<std::vector<NetworkNormalEquations::DdPriorBlock::Term>> rows;
            std::vector<Eigen::VectorXd>                     Wrows;   // over the old basis (empty: fresh)
            // This epoch's linearization DD per emitted chord, Σ c·x_lin over the same
            // fundamental cycle that builds `terms`, i.e. the incidence the fold uses to form
            // z_free, so ẑ + zl is the arc-space absolute DD.  Empty unless lin_on. (RS-2)
            std::vector<double> zl_rows;
            for (const ArcKey& c : chords) {
                const std::map<ArcKey, int> cyc = arcgraph::fundamentalCycle(*tree, c);

                // Foldable when every cycle arc resolves to a live (station, local col).
                bool foldable = true;
                std::vector<NetworkNormalEquations::DdPriorBlock::Term> terms;
                terms.reserve(cyc.size());
                double zl_c = 0.0;   // Σ c·x_lin over this cycle (RS-2)
                for (const auto& [arc, coeff] : cyc) {
                    const ArcInfo& ai = arcinfo.at(arc);   // every cycle arc is in this component's graph
                    if (ai.est_station < 0 || ai.local_col < 0) { foldable = false; break; }
                    terms.push_back({static_cast<std::size_t>(ai.est_station), ai.local_col,
                                     static_cast<double>(coeff)});
                    if (lin_on) zl_c += static_cast<double>(coeff) * ai.x_lin;
                }
                if (!foldable) { ++counters_.chords_dropped; continue; }

                // Cycle-length census.  The fundamental-cycle length is the tree-path span of
                // this DD: short cycles give a sparse DD information matrix J = Pᶻ⁻¹, so the
                // exact info-form carry stays bounded; long cycles give a dense J.  Counted
                // over foldable chords, the ones that enter the carry. (INC-C0, D-c viability)
                {
                    const long long clen = static_cast<long long>(cyc.size());
                    if (clen > counters_.max_cycle_len) counters_.max_cycle_len = clen;
                    counters_.sum_cycle_len += clen;
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
                bool slip_in_cycle = false;
                if (!slipped_arcs.empty())
                    for (const auto& [arc, coeff] : cyc)
                        if (slipped_arcs.count(arc)) { slip_in_cycle = true; break; }

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
            }
            if (fold_chords.empty()) continue;   // nothing foldable → carry nothing here

            // --- Assemble the DD-prior block(s) (carried joint + fresh diffuse). --
            const Eigen::Index K = static_cast<Eigen::Index>(fold_chords.size());
            const long long fold_dim = static_cast<long long>(K);
            if (fold_dim > counters_.max_component_kc)
                counters_.max_component_kc = fold_dim;   // census (INC-C0)
            {   // Bucket this emitted component's fold-dim (SPEC §7.4 Amdahl census, LF-1 INC-0).
                int bkt = 0;
                while (bkt < 9 && (1LL << bkt) <= fold_dim) ++bkt;  // floor(log2(fold_dim))+1, capped at 9 (≥256)
                ++counters_.comp_kc_hist[bkt];
            }
            Eigen::MatrixXd W;
            {   // Times the dense-K² change-of-basis projection (W · Pᶻ_old · Wᵀ, W·z_old), the
                // materialization that the factored (A+U S Uᵀ) store of INC-C2 removes (INC-C0).
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
                    if ((wr.array() != 0.0).count() >= 2) { ++counters_.tree_swaps; break; }
                }

                // Census: an old chord that no surviving fundamental cycle references (its W
                // column is all-zero) is marginalized per chord, a drop that the whole-block
                // chords_marginalized count (prev_hit == 0) does not see.  It answers §0 (a)
                // vs (b): a large count means the projection kill path is firing (arc
                // churn); ~0 while max_z_dim grows means dead chords are retained in large
                // live components. (INC-C0)
                for (Eigen::Index j = 0; j < n_old; ++j)
                    if (W.col(j).isZero(0.0)) ++counters_.chords_marg_perchord;
            }

            // ---- Emission: one DD-prior block per component. ----
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
        }
    }

    // Splits: a previous block spread across ≥2 new components.  Marginalized: a
    // previous block that fed nothing this epoch (its arcs all died or re-referenced).
    for (std::size_t p = 0; p < prev.size(); ++p) {
        if (prev_hit[p] >= 2) ++counters_.splits;
        if (prev_hit[p] == 0)
            counters_.chords_marginalized += static_cast<long long>(prev[p].chords.size());
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
                                                        const Eigen::VectorXd& zl) {
    for (auto it = carried_.rbegin(); it != carried_.rend(); ++it) {
        if (it->group != group) continue;
        it->zl = zl;
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
    // Per-chord D5 gate over one emitted block, writing into @p cb row by row.
    auto d5Gate = [&](const BlockMeta& bm, const Eigen::VectorXd& v,
                      const Eigen::MatrixXd& cov_v, CarriedBlock& cb) {
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
            // (so it reaches the epoch's n_d5_flags) and counted, ẑ and Pᶻ are left alone,
            // and the warm-up mark tells the caller to keep the arc out of the slip funnel.
            // innov_retaps is not incremented: N flags with 0 re-taps shows that the window
            // was open. (RS-2b)
            if (cfg_.innov_diagnostic_only) {
                last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(i)]);
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
            last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(i)]);
            last_innov_terms_.push_back({bm.zhat(i), v(i), var, sigma0, cb.z(i)});
            last_innov_warmup_.push_back(0);
            ++counters_.innov_flags;
            ++counters_.innov_retaps;
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
    // The fresh diagonal is the isotropic diffuse prior, the conservative choice for a block
    // just declared unusable.  A rebirth is a prior reset, not a slip; the slip funnel is
    // not touched.
    auto rebirthBlock = [this](CarriedBlock& cb, std::size_t b, const BlockMeta& bm,
                               double min_pivot_rel) {
        const Eigen::Index n = cb.Pz.rows();
        if (n <= 0 || cb.Pz.cols() < n) return;
        cb.Pz.setZero();
        cb.Pz.diagonal().setConstant(cfg_.fresh_prior_var);
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
        // driver pins Eigen to one thread, so there is no nested threading.
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
            InstallSlot& S = slots[b];
            const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
            if (cfg_.innov_gate && K > 0 && S.has_innov)
                d5Gate(bm, S.v, S.cov_v, S.cb);
            if (cfg_.pd_safe && K > 0 && foldDegenerate(b))   // rebirth (INC-2a)
                rebirthBlock(S.cb, b, bm, foldMinPivot(b));
            next.push_back(std::move(S.cb));
        }
        carried_.swap(next);
        innov_flat_.clear();    // the one-epoch corroboration set is consumed here (D5 v3)
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
        CarriedBlock cb;
        cb.group    = bm.group;
        cb.chords   = bm.chords;
        cb.vertices = bm.vertices;
        cb.arcs     = bm.arcs;
        cb.zl       = bm.zl;   // the frame the installed means are relative to (RS-2)
        const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());

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
        // d5Gate, shared with the parallel path.
        if (cfg_.innov_gate && K > 0) {
            ScopedAccum _d5(perf_timing_, &perf_timers_.d5_gate_ms);   // INC-C0
            Eigen::VectorXd v;
            Eigen::MatrixXd cov_v;
            est.ddPriorInnovation(b, x_s, cov_free, v, cov_v);
            d5Gate(bm, v, cov_v, cb);
        }
        if (cfg_.pd_safe && K > 0 && foldDegenerate(b))   // rebirth (INC-2a)
            rebirthBlock(cb, b, bm, foldMinPivot(b));
        next.push_back(std::move(cb));
    }
    carried_.swap(next);
    innov_flat_.clear();    // the one-epoch corroboration set is consumed here (D5 v3)
    counters_.z_dim = static_cast<long long>(totalZDim());
    if (counters_.z_dim > counters_.max_z_dim) counters_.max_z_dim = counters_.z_dim;  // INC-C0
}

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

/**
 * @brief Run-end section: the PD-safe invariant-DD fold.
 *
 * The PD-safe invariant-DD fold (card 4, INC-0 and INC-1/2): folded and inert blocks and
 * the block rebirths; on a carry_invariant run that never folded, the zero-fold line.
 *
 * Gate: prints under `ctx.knobs.carry_invariant && ctx.counters.ddfold_epochs > 0`.
 *
 * @param v The run at its end (network/runend/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkCarryDdFoldRunEnd(const NetworkRunEndView& v) {
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

/**
 * @brief Run-end section: the card 4/M1 invariant carry.
 *
 * The card 4/M1 invariant carry's run line: epochs driven, carried DDs, births,
 * marginalizations, merges, splits, swaps, slip kills and drops.
 *
 * Gate: prints under `carry_invariant`.
 *
 * @param v The run at its end (network/runend/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkInvariantCarryRunEnd(const NetworkRunEndView& v) {
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const bool carry_invariant = v.ctx.knobs.carry_invariant;
    const auto& carry_epochs = v.ctx.counters.carry_epochs;
    const auto& carry_born_total = v.ctx.counters.carry_born_total;
    const auto& carry_marg_total = v.ctx.counters.carry_marg_total;
    const auto& carry_merges_total = v.ctx.counters.carry_merges_total;
    const auto& carry_splits_total = v.ctx.counters.carry_splits_total;
    const auto& carry_swaps_total = v.ctx.counters.carry_swaps_total;
    const auto& carry_zdim_last = v.ctx.counters.carry_zdim_last;
    const auto& carry_slipkills_total = v.ctx.counters.carry_slipkills_total;
    const auto& carry_dropped_total = v.ctx.counters.carry_dropped_total;

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

/**
 * @brief Run-end section: the D5 fine-slip innovation gate and its variants.
 *
 * The D5 fine-slip innovation gate and its variants: the v3 corroboration, the v5
 * carried-only, the v4 re-tap-only and the v2 LS-VCE calibration.
 *
 * Gate: each section under its own condition, as in the teardown: `carry_innov_gate`;
 * `carry_innov_gate && config_.networkCarryInvariantInnovCorroborate()`;
 * `carry_innov_gate && config_.networkCarryInvariantInnovCarriedOnly()`;
 * `carry_innov_gate && config_.networkCarryInvariantInnovRetapOnly()`; `carry_innov_gate
 * && config_.networkCarryInvariantInnovCalibrate()`.
 *
 * @param v The run at its end (network/runend/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkD5GateRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkInvariantCarryDriver& carry_driver = v.run.engines.carry_driver;
    const bool carry_innov_gate = v.ctx.knobs.carry_innov_gate;
    const auto& carry_slipkills_total = v.ctx.counters.carry_slipkills_total;
    const auto& carry_innov_flags_total = v.ctx.counters.carry_innov_flags_total;
    const auto& carry_innov_retaps_total = v.ctx.counters.carry_innov_retaps_total;

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
        const long long sup = ctx.counters.carry_innov_kill_suppressed;
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
                  "[Counted per arc per build call in which the arc survives: a within-epoch "
                  "RAIM re-solve counts its arcs twice, the same convention as chords_born, "
                  "except an arc a culprit of the re-solve kills (card WCF).]",
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

/**
 * @brief Run-end section: D5 flags missing from the carry map.
 *
 * D5 innovation flags that did not resolve in the carry's block map, a wiring
 * inconsistency.
 *
 * Gate: prints under `carry_innov_gate && carry_innov_map_miss > 0`.
 *
 * @param v The run at its end (network/runend/NetworkRunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkD5MapMissRunEnd(const NetworkRunEndView& v) {
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const bool carry_innov_gate = v.ctx.knobs.carry_innov_gate;
    const auto& carry_innov_map_miss = v.ctx.counters.carry_innov_map_miss;

    if (carry_innov_gate && carry_innov_map_miss > 0)
        log->warn("Network (Card 4/M1 D5): {} innovation flag(s) over the run did not resolve in the "
                  "flag→ambiguity reverse map (next-epoch re-tap skipped for them) — a reverse-map/"
                  "block wiring inconsistency; investigate.", carry_innov_map_miss);
    return 0;
}

// Run-end report of this subsystem: its sections in print order (card NOM2 step TD2;
// declared in network/carry/NetworkInvariantCarryDriver.hpp).
int networkInvariantCarryDriverRunEnd(const NetworkRunEndView& v) {
    return networkRunEndSections(v, {
        networkCarryDdFoldRunEnd,
        networkInvariantCarryRunEnd,
        networkD5GateRunEnd,
        networkD5MapMissRunEnd
    });
}

// ---- End of the run-end reports ----

}  // namespace GnssProcessor

/**
 * @file NetworkInvariantCarryDriver.cpp
 * @brief Card 4 (M1) phase C3b — implementation of the DD invariant-carry driver.
 *        See NetworkInvariantCarryDriver.hpp for the design rationale.
 */

#include "filter/NetworkInvariantCarryDriver.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace GnssProcessor {

namespace {

/// INC-C0: RAII wall-clock accumulator (mirrors NetworkNormalEquations' ScopedAccum) —
/// adds the elapsed ms to @p acc on scope exit iff @p on.  No clock reads when off.
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

/// WL-3 §3.1: the unit-normalized (NL, WL) lane ROTATION for a band pair — rows are
/// orthonormal lane directions in (z₁, z₂) meter space.  Row 0 is the phase-determined
/// (NL) direction, row 1 the WL lane = the unit-normalized WL functional (1/λ₁, −1/λ₂).
/// RᵀR = I exactly (up to fp), so Rᵀ·diag(q_nl, q_wl)·R with q_nl == q_wl reduces to the
/// isotropic diagonal — the §6.1 equivalence shape needs no special case here.
inline Eigen::Matrix2d laneRotation(double lambda1, double lambda2) {
    const double a = 1.0 / lambda1, b = 1.0 / lambda2;
    const double n = std::sqrt(a * a + b * b);
    Eigen::Matrix2d R;
    R << b / n,  a / n,    // NL (⊥ the WL functional)
         a / n, -b / n;    // WL lane
    return R;
}

/// WL-3: one WL cycle expressed in the (unit-normalized) lane COORDINATE is 1/‖(1/λ₁,−1/λ₂)‖
/// state-meters — the σ conversion for @c Config::sigma_wl_fresh_cyc.
inline double laneMetersPerCycle(double lambda1, double lambda2) {
    const double a = 1.0 / lambda1, b = 1.0 / lambda2;
    return 1.0 / std::sqrt(a * a + b * b);
}

/// A tiny path-compressing union-find over the (small, per-group) integer vertex set.
class UnionFind {
public:
    int find(int x) {
        auto it = parent_.find(x);
        if (it == parent_.end()) { parent_[x] = x; return x; }
        int r = x;
        while (parent_[r] != r) r = parent_[r];
        while (parent_[x] != r) { int nx = parent_[x]; parent_[x] = r; x = nx; }
        return r;
    }
    void unite(int a, int b) {
        int ra = find(a), rb = find(b);
        if (ra != rb) parent_[ra] = rb;
    }
private:
    std::unordered_map<int, int> parent_;
};

// --- RTB-7 carry-blob (de)serialization helpers (self-contained; the snapshot
//     layer captures the CARRY..CEND blob verbatim, exactly as the emitter blob). ---
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

/// PD-safe INC-2c (docs/SPEC_pd_safe_dd_fold.md §5c): the EQUILIBRATED positive-definiteness
/// screen of §4(b), applied to a bare carried @f$P^z@f$ (no data information @f$B@f$ yet).
///
/// Same shape as the fold's screen so the two cannot disagree about what "PD" means: scale
/// each row/col by @f$1/\sqrt{P_{ii}}@f$ so every pivot is O(1), then require
/// @f$d_{\min}\ge 10^{-12} d_{\max}@f$.  A non-positive or non-finite DIAGONAL is decided
/// up front — a variance that is not a positive number needs no factorization to reject,
/// and the corrupt blobs on disk carry exactly that (−2.9e7 m² diagonals measured
/// 2026-08-02).
///
/// Deliberately a PREDICATE, not a repair: at restore the caller's response is the bounded,
/// counted REBIRTH (means kept, covariance re-seeded diffuse), not a silent patch of a
/// covariance whose provenance is already suspect.
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
    innov_flat_.clear();       // D5 v3: the corroboration set dies with the carry too
    lane_fix_w_.clear();       // WL-3: the fold-on-change memory dies with the carry
    lane_fix_staged_.clear();
    lane_fix_epoch_.clear();   // LF-1 INC-2: the skip grace clock dies with the carry
    install_epoch_index_ = 0;
}

void NetworkInvariantCarryDriver::reset() {
    resetCarryState_();
    // The CARRY dies here; the run INSTRUMENT does not.  reset() is reachable mid-run
    // from the install seam's catch around installPosterior, and `counters_ = Counters{}`
    // there silently discarded the run history: the run-end line reads the live counter,
    // so every total behind it under-reported, and the seam's per-epoch churn deltas
    // (cumulative − previous-snapshot) went NEGATIVE for that epoch.  Standing invariant
    // #4 in its own plumbing — an instrument that loses its history reads clean and lies.
    //
    // z_dim is the one member that is NOT a tally but a live dimension, so it tracks the
    // carry it describes: the carry is now empty, and so is it.  The run MAXIMA
    // (max_z_dim / max_component_kc) are history and stay.
    counters_.z_dim = 0;
}

void NetworkInvariantCarryDriver::serializeCarry(std::ostream& os, bool diagonal_only) const {
    // RTB-7(e): v3 == "the Pᶻ rows are one PD diagonal row".  The version is written for
    // the reader's benefit only — restoreCarry dispatches on the ROW TAG (P vs PD), so a
    // blob of either shape restores under either knob setting.  OFF ⇒ the v2 header and
    // the dense P rows, byte-for-byte the pre-RTB-7(e) blob.
    os << (diagonal_only ? "CARRY,v3," : "CARRY,v2,") << carried_.size() << "\n";
    // RE-1: the D5 σ̂₀² inflation state, so a resume does not re-run the pre-warmup
    // raw-formal gate against a full set of restored chords.  Written only when
    // calibration is armed and the accumulator actually holds rows; its absence is
    // the legacy (v1) contract — accumulator starts at seed.
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
        if (diagonal_only) {
            // ONE row, zdim numbers — the whole point of v3 (Σ K_c vs Σ K_c²).  A
            // zdim == 0 block writes NO row at all, exactly as the dense path does
            // (its loop is empty), so the two shapes stay line-for-line aligned there.
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
    // RE-4: this is a general "install this carry" API — start from a clean driver so
    // no stale lane-fix / gate-exemption / innovation side-state survives the install.
    // Counters go too, and ONLY here: reset() alone now preserves the run tallies (it is
    // a mid-run carry cold start), but installing a FOREIGN carry is the start of a new
    // instrument — tallies inherited from earlier driver work would be attributed to a
    // carry that never produced them.
    reset();
    counters_ = Counters{};

    double seed_sigma0_sq = 0.0, seed_rows = 0.0;   // RE-1: from the optional v2 S line
    bool   saw_seed_line = false;                   // RE-1 provenance: an S line was PRESENT
    long long n_diag_blocks = 0;                    // RTB-7(e): blocks read from a PD row
    long long n_rebirth_blocks = 0;                 // PD-safe INC-2c: blocks rebirthed at restore
    std::vector<CarriedBlock> restored;
    while (std::getline(is, line)) {
        line = chomp(line);
        if (line == "CEND") {
            carried_ = std::move(restored);
            // RE-1: seed the σ̂₀² accumulator so the D5 gate trusts the restored
            // inflation immediately (v1 blob / calibration off ⇒ legacy seed start).
            resume_sigma0_seeded_ = false;
            resume_sigma0_value_  = 1.0;
            // RE-1 provenance: keep WHAT the blob offered alongside WHETHER it was taken.
            // The bool alone collapses four causes (cold / calibration off / no S line /
            // rejected seed) onto one word, and the run-end line used to assert one of
            // them unconditionally — see ResumeSeedState.
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
            // RTB-7(e) / invariant #4: a diagonal restore is otherwise INVISIBLE — the
            // run reports "warm + branch-faithful" and every carry counter looks normal
            // while the chord cross-covariances are gone.  Count it and say so.
            resume_carry_diag_blocks_ = n_diag_blocks;
            resume_carry_rebirth_blocks_ = n_rebirth_blocks;   // PD-safe INC-2c
            return true;
        }
        if (line.rfind("S,", 0) == 0) {             // RE-1 (v2 only; optional)
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
        // Pᶻ: EITHER zdim dense "P" rows (v1/v2) OR one "PD" diagonal row (RTB-7(e) v3).
        // The dispatch is on the ROW TAG, never on the header version — the blob is
        // self-describing, so a diagonal blob restores in a run configured to WRITE full
        // ones and vice versa (that is what lets the A/B arms differ only in their seed
        // bytes).  A v3 restore starts every chord cross-covariance at zero: the branch
        // means are exact, the geometry between them is re-earned from data.
        b.Pz.setZero(zdim, zdim);
        if (zdim > 0) {
            if (!std::getline(is, line)) return fail("carry: truncated at P");
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
            // --- PD-safe INC-2c: the restore guard (SPEC §5c) --------------------
            // Every carry blob written before this card can hold the corrupt joint lane
            // block — an indefinite Pᶻ with negative diagonals (−2.9e7 m²) and
            // super-fresh entries (+4.7e6 > the 1e6 diffuse prior).  Restoring it verbatim
            // re-imports the disease into a FIXED build and the resumed run silently
            // inherits the frozen 78 % of state the fix exists to release.  Rebirth it
            // here instead: keep the means (the branch — which integer realization the
            // phase side landed on — lives in ẑ), reset the covariance to the fresh
            // diffuse diagonal, and COUNT it.  Bounded and loud, per invariant #7.
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
    if (!(var_m2 > 0.0)) return 0;              // OFF ⇒ byte-identical
    std::size_t n = 0;
    for (CarriedBlock& b : carried_) {
        if (b.Pz.rows() <= 0) continue;
        // Diagonal ONLY: the restored means and the cross-covariance geometry (the
        // branch) are untouched; we merely widen each carried DD's own uncertainty
        // for the station-layer re-convergence window.
        b.Pz.diagonal().array() += var_m2;
        ++n;
    }
    resume_shock_sigma_m_ = std::sqrt(var_m2);
    return n;
}

void NetworkInvariantCarryDriver::beginEpoch(double dt_s, double content_s, bool clean) {
    // D5 v2: advance the σ̂₀² accumulator's exponential forgetting (once per epoch, BEFORE
    // this epoch's rows are folded in installPosterior) — independent of q_z.
    if (inflation_) inflation_->beginEpoch(content_s, clean);
    if (cfg_.qz <= 0.0 || dt_s <= 0.0) return;   // constant DDs (default) → no q_z op
    ScopedAccum _tu(perf_timing_, &perf_timers_.time_update_ms);   // INC-C0
    const double add = cfg_.qz * dt_s;
    // WL-3 §3.1: lane-aware process noise on pair blocks — Q = Rᵀ·diag(q_nl, q_wl)·R per
    // chord (q_nl = qz, q_wl = q_wl_scale·qz).  At q_wl_scale == 1 this is EXACTLY the
    // isotropic diagonal, so the plain add is used (byte-equal to the per-band shape).
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

// D5 v2.  The ARITHMETIC here is byte-for-byte the pre-visibility body (same branches, same
// clamp, same order of operations) — only the PROVENANCE is new.  See InflationState: a bare
// σ̂₀ of 1.00 is reported by three degraded states plus one healthy one, and the campaign that
// paid for this could not tell them apart from the run-end line.
NetworkInvariantCarryDriver::InflationStatus
NetworkInvariantCarryDriver::innovInflationStatus() const {
    InflationStatus st;
    if (!inflation_) return st;                        // Off, σ̂₀ = 1 (v1 by config)
    const NetworkCarryVce::Estimate est = inflation_->estimate(kInflationFamily, kInflationSat);
    st.n_eff         = est.n_eff;
    st.raw_sigma0_sq = est.robust_floor;
    if (!(est.n_eff >= kInflationWarmupRows) || !(est.robust_floor > 0.0)) {
        st.state = InflationState::WarmUp;             // warm-up ⇒ raw formal (v1)
        return st;                                     // σ̂₀ stays 1.0
    }
    double f2 = est.robust_floor;                       // σ̂₀² = geo_mean(sᵢ²)/χ²₁-geomean
    f2 = std::min(std::max(f2, 1.0), kInflationFactorCap);  // only RAISE the threshold; cap a diverged σ̂₀
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

// RTB-7(d) RE-1.  Pure readout over state restoreCarry already recorded — it decides
// NOTHING and the seed arithmetic above is untouched; only the PROVENANCE is new.  The
// branch order is the causal chain, most-fundamental first: with no blob there is nothing
// to seed from, with no accumulator nothing to seed INTO (and the gate is v1 by config
// whatever the blob said), and only then does the blob's own content decide.
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
    lane_fix_staged_.clear();   // WL-3: per-build staging (a RAIM re-solve re-derives it)
    ++counters_.epochs;
    if (perf_timing_) ++perf_timers_.build_epochs;   // INC-C0

    // The carry we project FROM this epoch; `carried_` is rebuilt in installPosterior.
    const std::vector<CarriedBlock>& prev = carried_;
    // Track, per previous block, how many NEW components it contributes to (split ⇒ >1).
    std::vector<int> prev_hit(prev.size(), 0);

    for (const GroupInput& g : groups) {
        // --- (1) Assemble this epoch's graph for the group. -------------------
        struct ArcInfo { int est_station; int local_col; int local_col2; bool tree_edge; };
        std::map<ArcKey, ArcInfo> arcinfo;
        std::set<ArcKey>          slipped_arcs;   // C3c: arcs whose carried ambiguity slipped this epoch
        UnionFind uf;
        std::vector<ArcKey> group_tree_edges;   // for per-component tree extraction
        std::set<ArcKey> slipped2_arcs;   // WL-3 pair: band-2's OWN slips (see ArcInput::slipped2)
        for (const ArcInput& a : g.arcs) {
            if (a.rx_vertex == a.sat_vertex) continue;         // degenerate (defensive)
            const ArcKey k(a.rx_vertex, a.sat_vertex);
            arcinfo[k] = {a.est_station, a.local_col, a.local_col2, a.tree_edge};
            uf.unite(a.rx_vertex, a.sat_vertex);
            if (a.tree_edge) group_tree_edges.push_back(k);
            if (a.slipped) slipped_arcs.insert(k);
            if (g.pair && a.slipped2) slipped2_arcs.insert(k);
        }
        if (arcinfo.empty()) continue;
        // WL-3: a pair chord tallies like the TWO per-band DDs it co-locates (§6.1 keeps
        // the lifecycle counters comparable across the regroup).
        const long long tally = g.pair ? 2 : 1;

        // --- (2) Partition arcs into connected components (root → arcs). ------
        std::map<int, std::vector<ArcKey>> comp_arcs;
        std::map<int, std::set<int>>       comp_vertices;
        for (const auto& [k, info] : arcinfo) {
            const int root = uf.find(k.u);
            comp_arcs[root].push_back(k);
            comp_vertices[root].insert(k.u);
            comp_vertices[root].insert(k.v);
        }

        // Previous blocks of THIS group, for the vertex-overlap contributor gather.
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
            // WL-3: a pair group has no upstream election of its own — its arc set is
            // the INTERSECTION of two per-band graphs, and the caller's (band-1) tree
            // tags need not span this subgraph (per-(rx,sat) channel heterogeneity
            // fragments the primary pairs across groups; measured ~7% tag mismatch at
            // 50 stations).  Prefer the tags, then COMPLETE the spanning forest
            // deterministically (ArcKey order) by promoting chords that connect
            // disjoint tree fragments.  Homogeneous regions keep the eq34 basis
            // EXACTLY (the §6.1 equivalence); fragmented regions stay carried instead
            // of being dropped wholesale by the tree guards below.
            if (g.pair && !chords.empty()) {
                UnionFind tf;
                for (const ArcKey& t : tree_edges) tf.unite(t.u, t.v);
                std::vector<ArcKey> kept;
                kept.reserve(chords.size());
                for (const ArcKey& c : chords) {
                    if (tf.find(c.u) != tf.find(c.v)) {
                        tf.unite(c.u, c.v);
                        tree_edges.push_back(c);   // promoted: completes the forest
                        ++counters_.pair_tree_promotions;
                    } else {
                        kept.push_back(c);
                    }
                }
                chords.swap(kept);
            }
            if (chords.empty()) continue;   // spanning tree only → no DD to carry

            // The cycle algebra: build a phase-B ZComponent from the tree.  A
            // malformed (non-spanning / cyclic) tree from the election is not
            // representable — skip the component rather than fold a wrong cycle.
            ZComponent zc;
            try {
                zc = ZComponent(tree_edges);
            } catch (const std::exception&) {
                continue;
            }
            // Every chord's endpoints must be tree-connected (guard fundamentalCycle).
            bool tree_ok = true;
            for (const ArcKey& c : chords)
                if (!zc.hasVertex(c.u) || !zc.hasVertex(c.v)) { tree_ok = false; break; }
            if (!tree_ok) continue;

            // Gather contributing previous blocks (any shared vertex), concatenated
            // block-diagonally into the old basis (they were independent components).
            std::vector<ArcKey> old_chords;
            std::set<ArcKey>    A_old;
            std::vector<const CarriedBlock*> contributors;
            for (std::size_t p : group_prev) {
                const CarriedBlock& ob = prev[p];
                if (ob.pair != g.pair) continue;   // WL-3 defensive: shape mismatch cannot project
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
            // Block-diagonal old covariance / mean over the contributors.  WL-3 pair
            // blocks are stored [band-1; band-2], so the concatenation keeps the two
            // band halves contiguous: contributor j's band-1 lands at [off, off+k) and
            // its band-2 at [n_old+off, n_old+off+k) (with the four Pᶻ sub-blocks placed
            // accordingly, cross-band included — zero unless the lane shape retained it).
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
            // Map an old chord arc → its column index in the old basis.
            std::map<ArcKey, Eigen::Index> old_col;
            for (Eigen::Index j = 0; j < n_old; ++j) old_col[old_chords[static_cast<std::size_t>(j)]] = j;

            // --- Per new chord: fundamental cycle, foldability, carried vs fresh.
            std::vector<ArcKey>                              fold_chords;
            std::vector<char>                                fold_carried;  // D5 v2: 1 = carried prior, 0 = born.  WL-3 pair: the BAND-1 mask.
            std::vector<char>                                fold_carried2; // WL-3 pair: the BAND-2 carried mask (== band-1 under the lane shape's §3.4 union; per-band under the §6.1 equivalence shape).
            std::vector<std::vector<NetworkNormalEquations::DdPriorBlock::Term>> rows;
            std::vector<std::vector<NetworkNormalEquations::DdPriorBlock::Term>> rows2;  // WL-3 pair: band-2 rows (same cycle, band-2 columns)
            std::vector<Eigen::VectorXd>                     Wrows;   // over old basis (empty ⇒ fresh)
            for (const ArcKey& c : chords) {
                const std::map<ArcKey, int> cyc = zc.fundamentalCycle(c);

                // Foldable ⇔ every cycle arc resolves to a live (station, local col) —
                // for a WL-3 pair group, in BOTH bands (membership requires band-2, §2).
                bool foldable = true;
                std::vector<NetworkNormalEquations::DdPriorBlock::Term> terms, terms2;
                terms.reserve(cyc.size());
                if (g.pair) terms2.reserve(cyc.size());
                for (const auto& [arc, coeff] : cyc) {
                    const ArcInfo& ai = arcinfo.at(arc);   // every cycle arc is in this component's graph
                    if (ai.est_station < 0 || ai.local_col < 0
                        || (g.pair && ai.local_col2 < 0)) { foldable = false; break; }
                    terms.push_back({static_cast<std::size_t>(ai.est_station), ai.local_col,
                                     static_cast<double>(coeff)});
                    if (g.pair)
                        terms2.push_back({static_cast<std::size_t>(ai.est_station), ai.local_col2,
                                          static_cast<double>(coeff)});
                }
                if (!foldable) { counters_.chords_dropped += tally; continue; }

                // INC-C0 (D-c viability): the fundamental-cycle length is the tree-path span of
                // this DD; short cycles ⇒ sparse DD information matrix J = Pᶻ⁻¹ ⇒ the exact
                // info-form carry is bounded, long cycles ⇒ dense J ⇒ it is not.  Measured on
                // FOLDABLE chords (the ones that enter the carry).
                {
                    const long long clen = static_cast<long long>(cyc.size());
                    if (clen > counters_.max_cycle_len) counters_.max_cycle_len = clen;
                    counters_.sum_cycle_len += tally * clen;
                }

                // Carried ⇔ the whole cycle lived last epoch (⊆ A_old) AND no cycle arc
                // slipped this epoch; then the DD is a linear combination of the old
                // chords with w_j = χ_c(arc of old chord j).  A cycle slip shifts the
                // true DD by an integer, so the q_z=0 carry pins a stiff WRONG prior
                // (Card 4 C3c) — EXCLUDE the slipped arc from the A_old membership so a
                // cycle through it re-taps fresh (data-only tap-off).  This is exact in
                // the projection-W architecture: for a surviving carried chord the
                // slipped-arc terms cancel from the sum (its cycle avoids the slipped
                // arc), so keeping the (zero-weighted) old chords needs no explicit drop.
                bool would_carry = n_old > 0;
                if (would_carry)
                    for (const auto& [arc, coeff] : cyc)
                        if (!A_old.count(arc)) { would_carry = false; break; }
                bool slip_in_cycle = false;
                if (!slipped_arcs.empty())
                    for (const auto& [arc, coeff] : cyc)
                        if (slipped_arcs.count(arc)) { slip_in_cycle = true; break; }

                if (!g.pair) {
                    const bool carried = would_carry && !slip_in_cycle;
                    Eigen::VectorXd w;
                    if (carried) {
                        w = Eigen::VectorXd::Zero(n_old);
                        for (Eigen::Index j = 0; j < n_old; ++j) {
                            auto it = cyc.find(old_chords[static_cast<std::size_t>(j)]);
                            if (it != cyc.end()) w(j) = static_cast<double>(it->second);
                        }
                        ++counters_.chords_carried;
                    } else {
                        ++counters_.chords_born;
                        if (would_carry && slip_in_cycle) ++counters_.slip_kills;  // a carried DD lost to a slip
                    }
                    fold_chords.push_back(c);
                    fold_carried.push_back(carried ? 1 : 0);
                    rows.push_back(std::move(terms));
                    Wrows.push_back(std::move(w));   // empty vector marks a fresh chord
                    continue;
                }

                // ---- WL-3 pair chord: PER-BAND slip semantics.  Under the LANE shape a
                // slip on either band refloats BOTH bands (§3.4 — they share the graph
                // edge and the joint 2×2 coupling makes a half-stale prior poisonous);
                // under the §6.1 EQUIVALENCE shape each band refloats on its OWN slips,
                // exactly like the two per-band groups it must reproduce to ≤1e-9.
                bool slip2_in_cycle = false;
                if (!slipped2_arcs.empty())
                    for (const auto& [arc, coeff] : cyc)
                        if (slipped2_arcs.count(arc)) { slip2_in_cycle = true; break; }
                const bool union_slip = slip_in_cycle || slip2_in_cycle;
                const bool c1 = would_carry && !(cfg_.lane_joint ? union_slip : slip_in_cycle);
                const bool c2 = would_carry && !(cfg_.lane_joint ? union_slip : slip2_in_cycle);
                Eigen::VectorXd w;
                if (would_carry) {
                    w = Eigen::VectorXd::Zero(n_old);
                    for (Eigen::Index j = 0; j < n_old; ++j) {
                        auto it = cyc.find(old_chords[static_cast<std::size_t>(j)]);
                        if (it != cyc.end()) w(j) = static_cast<double>(it->second);
                    }
                }
                counters_.chords_carried += (c1 ? 1 : 0) + (c2 ? 1 : 0);
                counters_.chords_born    += (c1 ? 0 : 1) + (c2 ? 0 : 1);
                if (would_carry && !c1) ++counters_.slip_kills;
                if (would_carry && !c2) ++counters_.slip_kills;
                // WL-3 §3.4: a reborn pair chord must RE-EARN its FFR fix — erase the
                // fold-on-change memory here (idempotent, so the RAIM re-solve rebuild
                // stays deterministic: carried_ is unchanged between within-epoch builds).
                // LF-1 INC-2: dropping lane_fix_epoch_ too resumes increments for the reborn
                // chord this same epoch (invariant #1: the slip response is unaffected).
                if (!(c1 && c2)) { lane_fix_w_.erase({g.key, c}); lane_fix_epoch_.erase({g.key, c}); }
                fold_chords.push_back(c);
                fold_carried.push_back(c1 ? 1 : 0);
                fold_carried2.push_back(c2 ? 1 : 0);
                rows.push_back(std::move(terms));
                rows2.push_back(std::move(terms2));
                Wrows.push_back(std::move(w));   // sized ⇔ the GRAPH projection exists (per-band masks above)
            }
            if (fold_chords.empty()) continue;   // nothing foldable → carry nothing here

            // --- Assemble the DD-prior block(s) (carried joint + fresh diffuse). --
            const Eigen::Index K = static_cast<Eigen::Index>(fold_chords.size());
            const long long fold_dim = static_cast<long long>(tally) * static_cast<long long>(K);
            if (fold_dim > counters_.max_component_kc)
                counters_.max_component_kc = fold_dim;   // INC-C0 census (pair joint fold cost is 2K)
            {   // LF-1 INC-0: bucket this emitted component's fold-dim (SPEC §7.4 Amdahl census).
                int bkt = 0;
                while (bkt < 9 && (1LL << bkt) <= fold_dim) ++bkt;  // floor(log2(fold_dim))+1, capped at 9 (≥256)
                ++counters_.comp_kc_hist[bkt];
            }
            Eigen::MatrixXd W;
            {   // INC-C0: time the dense-K² change-of-basis projection (W · Pᶻ_old · Wᵀ, W·z_old)
                // — the materialization the factored (A+U S Uᵀ) store of INC-C2 removes.
                // (The projection W is GRAPH-level: for a WL-3 pair component it applies to
                // each band's half unchanged — blkdiag(W, W) on the joint [b1; b2] storage.)
                ScopedAccum _bp(perf_timing_, &perf_timers_.basis_project_ms);
                W = Eigen::MatrixXd::Zero(K, n_old);
                for (Eigen::Index i = 0; i < K; ++i)
                    if (Wrows[static_cast<std::size_t>(i)].size() == n_old && n_old > 0)
                        W.row(i) = Wrows[static_cast<std::size_t>(i)].transpose();

                // Tree swap: the carry was re-expressed in a genuinely different cycle basis
                // iff some carried chord's projection row mixes ≥2 old chords (W is not a
                // signed permutation) — the projection analogue of a phase-B unimodular pivot.
                for (const Eigen::VectorXd& wr : Wrows) {
                    if (wr.size() != n_old || n_old == 0) continue;   // fresh chord
                    if ((wr.array() != 0.0).count() >= 2) { counters_.tree_swaps += tally; break; }
                }

                // INC-C0 census: an old chord no surviving fundamental cycle references (its W
                // column is all-zero) is marginalized PER-CHORD — the drop the whole-block
                // chords_marginalized (prev_hit==0) misses.  Answers §0 (a)-vs-(b): large ⇒ the
                // projection kill path is firing (arcs churn); ~0 while max_z_dim grows ⇒ dead
                // chords are retained in giant live components.
                for (Eigen::Index j = 0; j < n_old; ++j)
                    if (W.col(j).isZero(0.0)) counters_.chords_marg_perchord += tally;
            }

            if (!g.pair) {
                // ---- Per-band group: today's single-block emission (byte-identical). ----
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

                Eigen::VectorXd blk_zhat = blk.zhat;   // D5: the prior means, for the install-time innovation gate
                out.push_back(std::move(blk));
                block_meta_.push_back(BlockMeta{g.key, std::move(fold_chords), verts,
                                                std::set<ArcKey>(arcs.begin(), arcs.end()),
                                                std::move(blk_zhat), std::move(fold_carried)});
                ++counters_.components;
            } else {
                // ---- WL-3 pair component (SPEC_lane_carry_card.md §2): the projection
                // applies PER BAND on the joint [band-1; band-2] storage; fresh chords get
                // the lane-basis prior (§3.1) under the lane shape, the isotropic diffuse
                // prior otherwise.  Emission: ONE joint 2K-row block (lane shape) or TWO
                // per-band K-row blocks (the §6.1 equivalence shape, information-identical
                // to today's two per-band groups).
                ++counters_.pair_components;
                Eigen::VectorXd zhat1, zhat2;
                Eigen::MatrixXd P11, P22, P12;
                {
                    ScopedAccum _bp(perf_timing_, &perf_timers_.basis_project_ms);
                    if (n_old > 0) {
                        // The projection applies PER BAND with that band's carried mask —
                        // a band whose chord refloated projects NOTHING (fresh row/col).
                        Eigen::MatrixXd W1 = W, W2 = W;
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
                    // Fresh rows: the lane-basis prior Rᵀ·diag(σ_nl², σ_wl²)·R (σ_nl = the
                    // diffuse √fresh_prior_var, σ_wl = the honest code-seed lane scale) — or
                    // the isotropic diffuse when the lane prior is not armed (§6.1 shape).
                    // The 2×2 lane prior applies only when BOTH bands are fresh (the lane
                    // shape's union refloat guarantees it); a per-band fresh row under the
                    // equivalence shape gets the plain diffuse diagonal.
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
                const std::set<ArcKey> arcs_set(arcs.begin(), arcs.end());
                if (cfg_.lane_joint) {
                    // ONE joint 2K-row block: rows = K band-1 cycles then K band-2 cycles,
                    // Pᶻ the joint 2K×2K — its fold's cross-band Woodbury term B₁₂ is EXACT
                    // (the per-band shape drops it), which is part of the lane design.
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
                    block_meta_.push_back(std::move(bm));
                    ++counters_.components;
                } else {
                    // TWO per-band K-row blocks (band-2 adjacent at b+1) — the fold sees
                    // exactly today's information (block-diagonal, no B₁₂), so the §6.1
                    // equivalence gate can hold to summation order.
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
                    block_meta_.push_back(std::move(m1));
                    block_meta_.push_back(std::move(m2));
                    counters_.components += 2;
                }
            }
        }
    }

    // Splits: a previous block spread across ≥2 new components.  Marginalized: a
    // previous block that fed nothing this epoch (its arcs all died / re-referenced).
    // (WL-3: a pair block tallies 2× — it holds two per-band DDs per chord.)
    for (std::size_t p = 0; p < prev.size(); ++p) {
        const long long ptally = prev[p].pair ? 2 : 1;
        if (prev_hit[p] >= 2) counters_.splits += ptally;
        if (prev_hit[p] == 0)
            counters_.chords_marginalized += ptally * static_cast<long long>(prev[p].chords.size());
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
        gate_exempt_.insert({ group, chord });      // §3.3: D5-exempt at the NEXT install
        return true;
    }
    return false;   // not a carried chord of `group` (cycle not 1:1) — caller skips + counts
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
        gate_exempt_.insert({ group, chord });   // §3.3: D5-exempt (both band rows) at the NEXT install
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
    // LF-1 INC-0: time the dense rank-1 downdate (the irreducible O(K²)/row measurement work)
    // separately from the basis projection the SB_DDBUILD wall lumps it with.
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
    // LF-1 INC-1: the per-row re-symmetrize is LOAD-BEARING at the ULP level — the rank-1
    // downdate holds symmetry only to ULP, and that asymmetry, fed through the stiff q_z=0 DD
    // carry, amplifies across epochs (measured: dropping it drifts the lane-on solution ~1e-4
    // by epoch 4).  Keep it exactly per-row when fold_blocked is OFF (byte-identical to the
    // pre-card exe); under fold_blocked it is hoisted to ONE @ref finalizeLaneBatch pass per
    // block (equivalence class, ≤1e-12 vs sequential).
    if (!cfg_.fold_blocked)
        blk.Pz = (0.5 * (blk.Pz + blk.Pz.transpose())).eval();
    bm.zhat   = blk.zhat;   // the D5 innovation gate must standardize against the FOLDED prior
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

    // LF-1 INC-0: time the idempotent lane SET (the ~65%-decided re-pins — the row population
    // INC-1's O(K²)→O(K) sparsity rewrite targets) apart from the measurement folds.
    ScopedAccum _lf(perf_timing_, &perf_timers_.lane_fix_ms);
    if (perf_timing_) ++perf_timers_.lane_fix_calls;

    // The fix is an IDEMPOTENT lane-coordinate SET, not a measurement fold: every
    // decided epoch it re-centers the chord's WL lane COORDINATE on the integer and
    // re-collapses its variance to σ², zeroing the coordinate's cross-covariances
    // (a fresh independent assertion, PSD-safe — WL-2's inject semantics realized on
    // the lane coordinate, NL preserved by construction).  A measurement fold here
    // would ACCUMULATE per-epoch decisions that are not independent information (the
    // F-A once-only discipline); the set is information-idempotent, and re-applying
    // it while the decision is unchanged is what keeps the lane pinned against the
    // q_wl re-loosening + the noisy MW increments (measured Jul-16: fold-on-change
    // alone leaves decided lanes drifting to MW quality, med|f| 0.25-vs-0.14).
    const double a1 = 1.0 / bm.lambda1, a2 = 1.0 / bm.lambda2;
    const double hn = std::sqrt(a1 * a1 + a2 * a2);   // ‖h‖: cycles per lane-coordinate meter
    const Eigen::Index p = static_cast<Eigen::Index>(k), qq = K + static_cast<Eigen::Index>(k);
    const double up = a1 / hn, uq = -a2 / hn;          // the unit lane direction's two nonzeros
    const double target_coord = y_cyc / hn;            // correction-frame lane target (m)
    const double sig_coord    = sigma_cyc / hn;
    // z' = z + (t − uᵀz)·u;  P' = (I−uuᵀ)P(I−uuᵀ) + σ²·uuᵀ = P − Pu·uᵀ − u·Puᵀ + (uᵀPu+σ²)·uuᵀ.
    if (!cfg_.fold_blocked) {
        // Dense O(K²) form — byte-identical to the pre-card exe.
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
        // LF-1 INC-1: O(K) sparse SET.  u has exactly two nonzeros (rows p, qq), so Pu = P·u
        // is the 2-column combo (not a dense GEMV), and ΔP = −Pu·uᵀ − u·Puᵀ + c·uuᵀ is nonzero
        // only in columns/rows {p, qq}.  Build the two changed columns Mp = ΔP(:,p),
        // Mq = ΔP(:,qq) and write them symmetrically (col + mirrored row) — Pz stays exactly
        // symmetric (ΔP is symmetric, so newp(qq)==newq(p)), and the per-row re-symmetrize is
        // hoisted to finalizeLaneBatch.  Same math as the dense branch up to fp reassociation.
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
    bm.zhat   = blk.zhat;   // the D5 gate standardizes against the SET prior

    const std::pair<int, ArcKey> key{ bm.group, bm.chords[k] };
    const auto it = lane_fix_w_.find(key);
    const bool changed = (it == lane_fix_w_.end() || it->second != w);
    lane_fix_staged_[key] = w;   // committed at installPosterior (RAIM re-solve safe)
    return changed ? 1 : 0;      // 1 = integer moved/new, 0 = re-pinned (unchanged)
}

bool NetworkInvariantCarryDriver::laneIncrementSkippable(std::size_t b, std::size_t k) const {
    if (!cfg_.skip_pinned_increments) return false;   // OFF ⇒ byte-identical (never skip)
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
    // Restore exact symmetry once per joint block, after the epoch's MW / bridge / fix rows —
    // the per-row 0.5·(P+Pᵀ) the primitives skipped under fold_blocked (they hold symmetry only
    // to ULP).  Only joint pair blocks (pair_band == 3) ever received lane rows.
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
    last_fold_rebirths_.clear();   // PD-safe INC-2a
    // Hoist the datum-frame shared covariance (the n_shared² Σ_ss copy) to ONCE per install —
    // every block's posterior-cov AND innovation readout consumes the same matrix.
    const Eigen::MatrixXd cov_free = est.datumFrameSharedCov(datum);
    // D5 v2: the empirical inflation σ̂₀² accumulated THROUGH the previous epoch (predict-
    // then-update — this epoch's standardized innovations are fed AFTER gating, below).  It
    // is 1.0 (= the raw formal test, v1) when calibration is off or still in warm-up, so the
    // gate becomes a TRUE Nσ test: |v_i| > k·σ̂₀·√Cov(v)_ii.
    const InflationStatus infl = innovInflationStatus();
    const double sigma0 = infl.sigma0;
    const double k2f    = cfg_.innov_k * cfg_.innov_k * sigma0 * sigma0;   // (k·σ̂₀)²
    // ...and tally WHICH of the four 1.00×-reporting states produced it.  Always-on
    // instrument (invariant #4): "σ̂₀ = 1.00" from a calibrated run and "σ̂₀ = 1.00" because
    // the floor clamp bound a raw 0.0099 back up to 1 are the same number and opposite
    // findings.  Solve-neutral — nothing below reads these.
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
    // The per-block posterior mean/cov + innovation are CONST readouts of the folded
    // kernel (each reads only dd_priors_[b] + the shared read-only x_s / cov_free), so
    // they parallelize cleanly.  Everything ORDER-SENSITIVE — the σ̂₀ accumulator feed
    // (NetworkCarryVce::addRow), the gate decision, the flag/counter appends and the
    // carried_ install — stays SERIAL in the exact (block b, chord i) order below, so
    // the σ̂₀ that gates the NEXT epoch, the counters, and carried_ are all BIT-IDENTICAL
    // to the serial install regardless of thread count.  The serial path (n_threads == 1,
    // the default) is left completely UNTOUCHED — keep the two per-chord bodies in lockstep.
    // WL-3: a D5-flagged pair chord loses its fold-on-change memory (it re-taps and must
    // re-earn its FFR fix, §3.4) — shared by all install shapes below.
    // D5 v3 corroboration gate (Cfg::innov_corroborate).  Called ONLY after a chord has
    // already tripped the k·σ̂₀ test, and returns true when the re-tap must be SUPPRESSED.
    //
    // Suppress only on POSITIVE evidence of continuity — membership of innov_flat_ means the
    // caller measured the raw MW/GF level and found it flat.  An arc with no evidence is not
    // in the set and is therefore re-tapped as before, so this narrows the slip response
    // rather than removing it (standing invariant #1).  The optional magnitude backstop
    // re-admits very large innovations; it defaults to 0 (inert) because corroboration was
    // measured to FALL with innovation size, not rise.
    //
    // NOTE the ordering contract: the σ̂₀ accumulator (inflation_->addRow) is fed BEFORE this
    // is consulted at both call sites, so suppression never censors the calibration input —
    // the D5 v2 design note ("feeding above-threshold rows into σ̂₀ is NECESSARY, not merely
    // safe") holds unchanged whether or not the re-tap happens.
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
    // D5 v5 (Cfg::innov_carried_only): do not gate a chord that was BORN this epoch.
    //
    // The test is |v_i| > k·σ̂₀·√Cov(v)_ii where Cov(v) is the H0 covariance of the
    // innovation ABOUT THE CARRIED PRIOR.  A fresh chord has no carried prior — its
    // "prior" is the diffuse fresh_prior_var (1e6 by default), so Cov(v)_ii ≈ 1e6 and v is
    // whatever the correction frame's distance from x_lin happens to be.  There is no H0
    // to test, the "reset" it triggers is a no-op (a diffuse chord's posterior ALREADY is
    // the data-only tap-off), and the one thing the flag does accomplish is to put the arc
    // in the next epoch's slip funnel — which re-BORNS the chord, which flags again.
    //
    // MEASURED 2026-08-02 on `runs/churn_base` (63 sta, 3 h, DOY-129): 32 062 of 33 628
    // flags — 95.3 % — carry √var_v = 1000.000 m EXACTLY, i.e. the diffuse prior, with a
    // median |v| of 19.2 km.  27 chords are born-flagged on more than half of all flagged
    // epochs (URUM/G22 at duty 0.99).  A chord re-born every epoch has by construction
    // never accumulated anything, which is why dd_sigma_cyc cannot fall and med|frac_cyc|
    // stays uniform at 0.25.  This is the engine of the churn, not a symptom of it.
    //
    // The D5 v2 design already identified born chords as pathological here and excluded
    // them from the σ̂₀ accumulator ("a born chord's diffuse Cov(v) would tank σ̂₀²",
    // immediately above).  The same exclusion was simply never applied to the gate
    // DECISION.  This uses the identical chord_carried mask, so the two can never drift.
    //
    // Kept as an armable knob rather than made unconditional only because the project's
    // discipline is default-OFF + byte-identity; on the argument it is a correctness fix.
    // The condition is INFORMATIONAL, not topological, and getting that wrong was measured.
    // `chord_carried` says the cycle lay in A_old and no arc slipped — it does NOT say the
    // carried prior contains information.  A chord carried from a predecessor that was
    // itself diffuse inherits a diffuse PROJECTED prior and passes chord_carried anyway.
    // Measured 2026-08-02: keying only on chord_carried left 7 082 of 8 807 surviving flags
    // (80.4 %) still sitting at √Cov(v) = 1000 m, i.e. still untestable, after skipping
    // 13 643 rows.  So test the prior directly as well.
    //
    // Cov(v) = (Pᶻ + B) − GᵀΣG is the PRIOR-predictive variance, so Cov(v)_ii at or near
    // fresh_prior_var means Pᶻ_ii is diffuse whatever the topology says.  Half the fresh
    // prior is a deliberately loose bar: one epoch of real data collapses a DD's variance
    // by orders, so anything still within a factor of two of the diffuse prior has learned
    // essentially nothing and there is no H0 to test against it.
    const double kDiffusePriorFrac = 0.5;
    auto bornSkipped = [this, kDiffusePriorFrac](bool have_carried,
                                                 const std::vector<char>& carried,
                                                 std::size_t idx, double var) {
        if (!cfg_.innov_carried_only) return false;
        // No mask ⇒ cannot tell born from carried ⇒ fall through to the variance test only.
        // The fail-safe direction is toward KEEPING the slip response: a missing mask must
        // never silently disarm standing invariant #1's fine-scale layer.  (In practice
        // buildDdPriors always emits one chord_carried entry per chord, so this is
        // defensive — but the σ̂₀ feed just above treats an absent mask as "feed
        // everything", and the two must not fail in opposite directions.)
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
        if (!lane_fix_epoch_.empty())  lane_fix_epoch_.erase({group, chord});  // LF-1 INC-2: D5 flag resumes increments
    };
    // WL-3 D5 gate over ONE emitted per-band half of a pair component (split shape) or,
    // via offset arithmetic, kept in lockstep with the per-band body below.  Edits the
    // stitched pair block @p cb at rows [off, off+K).
    auto d5GateHalf = [&](const BlockMeta& bm, const Eigen::VectorXd& v,
                          const Eigen::MatrixXd& cov_v, CarriedBlock& cb, Eigen::Index off) {
        const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
        const bool have_carried = static_cast<Eigen::Index>(bm.chord_carried.size()) == K;
        if (!(v.size() == K && cov_v.rows() == K && cov_v.cols() == K && bm.zhat.size() == K))
            return;
        for (Eigen::Index i = 0; i < K; ++i) {
            const double var = cov_v(i, i);
            // PD-safe INC-0: this skip is where a degenerate block vanishes from view — an
            // inert fold caches the RAW non-PD Pᶻ+B as Minv, so Cov(v) inherits its
            // negative diagonals and every row here is dropped WITHOUT a trace.  Count it.
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
            cb.z(off + i) = bm.zhat(i) + v(i);
            cb.Pz.row(off + i).setZero();
            cb.Pz.col(off + i).setZero();
            cb.Pz(off + i, off + i) = sigma0 * sigma0 * var;
            last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(i)]);
            last_innov_sigmas_.push_back(std::abs(v(i)) / std::sqrt(var));
            last_innov_terms_.push_back({bm.zhat(i), v(i), var, sigma0, cb.z(off + i)});
            ++counters_.innov_flags;
            ++counters_.innov_retaps;
            laneFixErase(bm.group, bm.chords[static_cast<std::size_t>(i)]);
        }
    };
    // WL-3 D5 gate over a JOINT 2K pair block: per band-ROW exactly as today (row i<K =
    // band-1 of chord i, row K+i = band-2 of chord i); the exemption + σ̂₀-carried flag
    // key by the GRAPH chord (band-uniform membership).
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
            // PD-safe INC-0: see d5GateHalf — an inert (degenerate) fold's Cov(v) has
            // non-positive diagonals and every affected row is skipped silently.
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
            // WL-3: a pair chord's corroboration keys by the GRAPH chord, so BOTH band rows
            // are suppressed together — exactly like gate_exempt_ above.  Splitting them
            // would re-tap one band of a continuous pair and leave the other carried.
            if (d5Suppressed(bm.group, bm.chords[static_cast<std::size_t>(c)],
                             v(i) * v(i), var))
                continue;
            cb.z(i) = bm.zhat(i) + v(i);
            cb.Pz.row(i).setZero();
            cb.Pz.col(i).setZero();
            cb.Pz(i, i) = sigma0 * sigma0 * var;
            last_innov_flags_.emplace_back(bm.group, bm.chords[static_cast<std::size_t>(c)]);
            last_innov_sigmas_.push_back(std::abs(v(i)) / std::sqrt(var));
            last_innov_terms_.push_back({bm.zhat(i), v(i), var, sigma0, cb.z(i)});
            ++counters_.innov_flags;
            ++counters_.innov_retaps;
            laneFixErase(bm.group, bm.chords[static_cast<std::size_t>(c)]);
        }
    };
    // --- PD-safe INC-2a: the degenerate-block REBIRTH (SPEC §5a) ------------------
    // A block whose Pᶻ+B still could not factor after the equilibrated screen and the
    // eigenvalue-clamp repair folded INERTLY (M = 0): it took NO data this epoch and its
    // posterior IS its prior.  Left alone, that is the silent permanent decoupling this
    // card exists to end — the prior never improves, so it never factors, forever.
    //
    // Standing invariant #7 requires the RE-COUPLING edge to re-seed what it reconnects.
    // Rebirth does exactly that: keep the means ẑ (they are the block's branch — which
    // integer realization the phase side landed on, and the fold left them untouched),
    // reset Pᶻ to the fresh diffuse diagonal.  Next epoch's diffuse prior then takes the
    // data-only tap-off — a whole-block mass birth, ONE bounded epoch of information lost,
    // exactly the semantics chord births already have.
    //
    // Runs AFTER the D5 gate on purpose: a degenerate block's Cov(v) has non-positive
    // diagonals, so the gate skipped those rows anyway (counted), and any row it COULD
    // test keeps its data-only mean while the covariance is uniformly re-seeded.
    //
    // The fresh diagonal is isotropic even on a PAIR block: the lane-shaped fresh prior
    // (@c sigma_wl_fresh_cyc) is a buildDdPriors-time concern, and an isotropic diffuse
    // reset is the strictly conservative statement about a block we have just declared
    // unusable.  A rebirth is a PRIOR RESET, not a slip — the funnel is NOT touched.
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
    // Query the kernel's per-block PD verdict, defensively bounded (block_meta_ and the
    // kernel's fold slots are index-aligned by contract, but an install against a stale
    // estimator must degrade to "not degenerate", never throw).
    auto foldDegenerate = [&est](std::size_t b) {
        return b < est.numDdPriorBlocks() && est.ddPriorBlockDegenerate(b);
    };
    auto foldMinPivot = [&est](std::size_t b) {
        return b < est.numDdPriorBlocks() ? est.ddPriorBlockMinPivotRel(b) : 0.0;
    };

    // WL-3: commit this epoch's staged fold-on-change decisions (a D5 flag above already
    // erased its chord from BOTH maps, so flag wins over fix) and purge memory for chords
    // no longer carried as pair chords.  No-op (no allocation, no fp) on the per-band path.
    auto laneFixCommit = [this]() {
        if (lane_fix_staged_.empty() && lane_fix_w_.empty()) return;
        // LF-1 INC-2: stamp each RE-DECIDED chord with this epoch's index (the skip grace clock).
        // Only staged (re-decided) chords advance; a held-but-not-re-decided chord keeps its old
        // stamp so the grace window can eventually expire and resume increments.  Gated on the
        // toggle so lane_fix_epoch_ stays empty (and the map cost is zero) when INC-2 is off.
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

        // (1, PARALLEL over block index) const readouts into per-block slots only.
        // Eigen is pinned to one thread by the driver ⇒ no nested threading.  A pair
        // component's split halves / joint block read exactly like any other block —
        // the stitching stays in the serial phase.
        #pragma omp parallel for schedule(dynamic) num_threads(n_threads)
        for (long long bi = 0; bi < static_cast<long long>(NB); ++bi) {
            const std::size_t b = static_cast<std::size_t>(bi);
            const BlockMeta& bm = block_meta_[b];
            InstallSlot& S = slots[b];
            S.cb.group    = bm.group;
            S.cb.chords   = bm.chords;
            S.cb.vertices = bm.vertices;
            S.cb.arcs     = bm.arcs;
            S.cb.z  = est.ddPriorPosteriorMean(b, x_s);
            S.cb.Pz = est.ddPriorPosteriorCovariance(b, cov_free);
            if (cfg_.innov_gate && static_cast<Eigen::Index>(bm.chords.size()) > 0) {
                est.ddPriorInnovation(b, x_s, cov_free, S.v, S.cov_v);
                S.has_innov = true;
            }
        }

        // (2, SERIAL in (block b, chord i) order) the σ̂₀ feed + gate + flags + install —
        // identical call sequence ⇒ identical σ̂₀ / counters / carried_.
        std::vector<CarriedBlock> next;
        next.reserve(NB);
        for (std::size_t b = 0; b < NB; ++b) {
            const BlockMeta& bm = block_meta_[b];
            if (bm.pair_band == 2) continue;   // WL-3: stitched with its band-1 sibling at b-1
            InstallSlot& S = slots[b];
            const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());
            if (bm.pair_band == 0) {
                if (cfg_.innov_gate && K > 0 && S.has_innov)
                    d5GateHalf(bm, S.v, S.cov_v, S.cb, 0);
                if (cfg_.pd_safe && K > 0 && foldDegenerate(b))   // INC-2a
                    rebirthBand(S.cb, 0, S.cb.Pz.rows(), b, bm, foldMinPivot(b));
                next.push_back(std::move(S.cb));
                continue;
            }
            // WL-3 pair component: stitch the joint carried block.
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
                cb.Pz = Eigen::MatrixXd::Zero(2 * K, 2 * K);   // §6.1: cross-band coupling ZEROED
                cb.Pz.topLeftCorner(K, K)     = S.cb.Pz;
                cb.Pz.bottomRightCorner(K, K) = S2.cb.Pz;
                if (cfg_.innov_gate && K > 0) {
                    if (S.has_innov)  d5GateHalf(bm, S.v, S.cov_v, cb, 0);
                    if (S2.has_innov) d5GateHalf(block_meta_[b + 1], S2.v, S2.cov_v, cb, K);
                }
                if (cfg_.pd_safe && K > 0) {   // INC-2a: each emitted half screens on its own
                    if (foldDegenerate(b))     rebirthBand(cb, 0, K, b, bm, foldMinPivot(b));
                    if (foldDegenerate(b + 1)) rebirthBand(cb, K, K, b + 1,
                                                           block_meta_[b + 1],
                                                           foldMinPivot(b + 1));
                }
            } else {   // pair_band == 3: the joint lane-shape block (coupling retained)
                cb.z  = std::move(S.cb.z);
                cb.Pz = std::move(S.cb.Pz);
                if (cfg_.innov_gate && K > 0 && S.has_innov)
                    d5GateJoint(bm, S.v, S.cov_v, cb);
                if (cfg_.pd_safe && K > 0 && foldDegenerate(b))   // INC-2a: the joint 2K block
                    rebirthBand(cb, 0, cb.Pz.rows(), b, bm, foldMinPivot(b));
            }
            next.push_back(std::move(cb));
        }
        carried_.swap(next);
        gate_exempt_.clear();   // WL-2 §3.3: the one-epoch inject exemption is consumed here
        innov_flat_.clear();    // D5 v3: the one-epoch corroboration set is consumed here too
        laneFixCommit();        // WL-3 §3.3: fold-on-change memory commits on the accepted epoch
        ++install_epoch_index_; // LF-1 INC-2: advance the skip grace clock (accepted install only)
        counters_.z_dim = static_cast<long long>(totalZDim());
        if (counters_.z_dim > counters_.max_z_dim) counters_.max_z_dim = counters_.z_dim;  // INC-C0
        return;
    }

    // ---- Serial install (n_threads == 1, the default; byte-identical OFF path) ---
    std::vector<CarriedBlock> next;
    next.reserve(block_meta_.size());
    if (perf_timing_) ++perf_timers_.install_epochs;   // INC-C0
    for (std::size_t b = 0; b < block_meta_.size(); ++b) {
        const BlockMeta& bm = block_meta_[b];
        if (bm.pair_band == 2) continue;   // WL-3: stitched with its band-1 sibling at b-1
        CarriedBlock cb;
        cb.group    = bm.group;
        cb.chords   = bm.chords;
        cb.vertices = bm.vertices;
        cb.arcs     = bm.arcs;
        const Eigen::Index K = static_cast<Eigen::Index>(bm.chords.size());

        if (bm.pair_band == 1 && b + 1 < block_meta_.size()
            && block_meta_[b + 1].pair_band == 2) {
            // --- WL-3 §6.1 split pair: stitch two per-band posteriors into the joint
            // [band-1; band-2] carried block with the cross-band coupling ZEROED — the
            // information-identical-to-today equivalence shape.  D5 gates per emitted
            // half in (b, b+1) row order, exactly the per-band call sequence.
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
            if (cfg_.pd_safe && K > 0) {   // INC-2a: each emitted half screens on its own
                if (foldDegenerate(b))     rebirthBand(cb, 0, K, b, bm, foldMinPivot(b));
                if (foldDegenerate(b + 1)) rebirthBand(cb, K, K, b + 1, bm2,
                                                       foldMinPivot(b + 1));
            }
            next.push_back(std::move(cb));
            continue;
        }
        if (bm.pair_band == 3) {
            // --- WL-3 lane shape: the joint 2K posterior — cross-band coupling RETAINED
            // (the honest jointly-covarianced pair, §2); D5 per band-row as today.
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
            if (cfg_.pd_safe && K > 0 && foldDegenerate(b))   // INC-2a: the joint 2K block
                rebirthBand(cb, 0, cb.Pz.rows(), b, bm, foldMinPivot(b));
            next.push_back(std::move(cb));
            continue;
        }

        { ScopedAccum _pm(perf_timing_, &perf_timers_.post_mean_ms);   // INC-C0
          cb.z  = est.ddPriorPosteriorMean(b, x_s); }
        { ScopedAccum _pc(perf_timing_, &perf_timers_.post_cov_ms);    // INC-C0 (the dense K×K install)
          cb.Pz = est.ddPriorPosteriorCovariance(b, cov_free); }

        // --- Card 4 D5: estimation-side fine-slip innovation gate ------------------
        // The carried DD's coarse slip layer (MW/LLI/IF-TD) is deliberately relaxed; a
        // finer slip the coarse layer missed inflates the carried DD's post-solve
        // innovation v = z_free − Gᵀx_s − ẑ.  A chord whose standardized innovation exceeds
        // k·σ̂₀ is treated as slipped: install it FRESH here (its data-only tap-off, mean
        // d = v+ẑ and variance σ̂₀²·Cov(v)_ii — the CALIBRATED innovation variance, i.e. the
        // fresh chord's honest uncertainty, NOT the optimistic formal one; cross-terms
        // zeroed), so the stale value does not propagate, and record its (group, chord) so
        // the caller marks its arc into NEXT epoch's slip funnel (the C3c re-tap).  OFF =
        // the posterior is installed unconditionally (byte-identical to the C3c path).
        // (The per-chord body lives in d5GateHalf, offset 0 — kept in lockstep with the
        // parallel path and the WL-3 pair shapes above.)
        if (cfg_.innov_gate && K > 0) {
            ScopedAccum _d5(perf_timing_, &perf_timers_.d5_gate_ms);   // INC-C0
            Eigen::VectorXd v;
            Eigen::MatrixXd cov_v;
            est.ddPriorInnovation(b, x_s, cov_free, v, cov_v);
            d5GateHalf(bm, v, cov_v, cb, 0);
        }
        if (cfg_.pd_safe && K > 0 && foldDegenerate(b))   // INC-2a
            rebirthBand(cb, 0, cb.Pz.rows(), b, bm, foldMinPivot(b));
        next.push_back(std::move(cb));
    }
    carried_.swap(next);
    gate_exempt_.clear();   // WL-2 §3.3: the one-epoch inject exemption is consumed here
    innov_flat_.clear();    // D5 v3: the one-epoch corroboration set is consumed here too
    laneFixCommit();        // WL-3 §3.3: fold-on-change memory commits on the accepted epoch
    ++install_epoch_index_; // LF-1 INC-2: advance the skip grace clock (accepted install only)
    counters_.z_dim = static_cast<long long>(totalZDim());
    if (counters_.z_dim > counters_.max_z_dim) counters_.max_z_dim = counters_.z_dim;  // INC-C0
}

}  // namespace GnssProcessor

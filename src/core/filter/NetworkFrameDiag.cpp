/**
 * @file NetworkFrameDiag.cpp
 * @brief SPEC_day129_frame_guards Phase 1 trigger instrumentation (P1-3, P1-4).
 *
 * See NetworkFrameDiag.hpp for the derivation and the row-classification rules.
 * Both functions are measure-only: they read the solved system and write CSV.
 */

#include "filter/NetworkFrameDiag.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "logging/NetworkRunTraces.hpp"

#include <Eigen/Dense>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {

namespace {

/// Shared-column families the eigenvector mass is partitioned over.  Order is
/// the CSV column order; kFamilyCount is the array bound.
enum Family { kClk = 0, kCb = 1, kPb = 2, kSh = 3, kFamilyCount = 4 };

/// Classify one shared column from its @c sharedColumnLabel prefix.  The labels
/// are "CLK:", "CB:", "PB:", "SH:"/"SH2:" (NetworkEstimator.cpp:138-153); an
/// unrecognized label is charged to the phase-bias bucket rather than dropped,
/// so the four masses always sum to 1 and a future family cannot silently
/// disappear from the partition.
Family familyOf(const std::string& label) {
    if (label.rfind("CLK:", 0) == 0) return kClk;
    if (label.rfind("CB:",  0) == 0) return kCb;
    if (label.rfind("SH",   0) == 0) return kSh;   // "SH:" and the Iono-2 "SH2:"
    return kPb;
}

/// Number of smallest directions written to the mode-detail file.
constexpr int kModesReported = 3;
/// Per-column participation entries kept per reported mode.
constexpr std::size_t kTopColumns = 8;
/// A column counts toward span_* when it carries at least this share of the mode.
constexpr double kSpanShare = 0.01;

}  // namespace

void networkShSpectrum(NetworkRunContext& ctx, const GnssConfig& config_,
                       const std::shared_ptr<spdlog::logger>& log,
                       NetworkRunTraces& traces, const SyncedEpoch& se,
                       NetworkEstimator& est) {
    if (!traces.sh_spectrum_trace.is_open()) return;

    // reducedSharedNormals() is the SAME cached Schur complement the datum rank
    // gate tested this epoch — by the post-solve call site the reduction has
    // already happened, so this is a cache read, not a second reduce.
    const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
    const Eigen::Index n = Nss.rows();
    if (n <= 0) return;

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Nss, Eigen::ComputeEigenvectors);
    if (es.info() != Eigen::Success) {
        // Once per run: a failed eigensolve is a diagnostic gap, not a solve
        // problem (the solution is untouched), but it must not be silent.
        if (ctx.carry.sh_spectrum_fail_logged == false) {
            ctx.carry.sh_spectrum_fail_logged = true;
            log->warn("Network (P1-3): SelfAdjointEigenSolver did not converge on the "
                      "{}x{} reduced shared normals (TOW {:.1f}); network_sh_spectrum.csv "
                      "will be missing this and any similarly-failing epoch.",
                      Nss.rows(), Nss.cols(), se.time.tow);
        }
        return;
    }
    const Eigen::VectorXd& ev = es.eigenvalues();      // ASCENDING
    const Eigen::MatrixXd& V  = es.eigenvectors();

    // Per-column family map (built once per epoch — the shared layout is rebuilt
    // every epoch, so it cannot be cached across epochs).
    std::vector<Family> fam(static_cast<std::size_t>(n), kPb);
    int n_fam[kFamilyCount] = {0, 0, 0, 0};
    for (Eigen::Index c = 0; c < n; ++c) {
        Family f = kPb;
        try { f = familyOf(est.sharedColumnLabel(c)); } catch (...) { f = kPb; }
        fam[static_cast<std::size_t>(c)] = f;
        ++n_fam[f];
    }

    const double ev_min = ev(0);
    const double ev_max = ev(n - 1);
    const double cond   = (ev_min > 0.0) ? (ev_max / ev_min)
                                         : std::numeric_limits<double>::infinity();

    // Mass partition of the SMALLEST direction.  L1 (not L2) mass: the question
    // is "how much of this direction lives in the clocks vs the field", and the
    // L1 share of |v| answers it without over-weighting one dominant column the
    // way squared mass would.
    double mass[kFamilyCount] = {0.0, 0.0, 0.0, 0.0};
    int    span[kFamilyCount] = {0, 0, 0, 0};
    {
        const Eigen::VectorXd v0 = V.col(0).cwiseAbs();
        const double l1 = v0.sum();
        if (l1 > 0.0) {
            for (Eigen::Index c = 0; c < n; ++c) {
                const double share = v0(c) / l1;
                mass[fam[static_cast<std::size_t>(c)]] += share;
                if (share >= kSpanShare) ++span[fam[static_cast<std::size_t>(c)]];
            }
        }
    }

    auto& o = traces.sh_spectrum_trace;
    o << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
      << n << ',' << n_fam[kClk] << ',' << n_fam[kCb] << ',' << n_fam[kPb] << ','
      << n_fam[kSh] << ','
      << fmt::format("{:.9e}", ev_min) << ',' << fmt::format("{:.9e}", ev_max) << ','
      << fmt::format("{:.9e}", cond);
    for (Eigen::Index k = 0; k < 5; ++k) {
        o << ',';
        if (k < n) o << fmt::format("{:.9e}", ev(k));
    }
    for (int f = 0; f < kFamilyCount; ++f) o << ',' << fmt::format("{:.6f}", mass[f]);
    for (int f = 0; f < kFamilyCount; ++f) o << ',' << span[f];
    // The ABSOLUTE null-space cutoff the rank gate applies this epoch
    // (rankGateTol · max(λmax,1)).  Without it ev_min is uninterpretable: these are
    // the PRE-datum reduced normals (solve() pins a COPY — NetworkEstimator.cpp:255),
    // so the smallest eigenvalues legitimately include the DECLARED gauge freedoms.
    // "λ_min cratered" means nothing except relative to this line.
    o << ',' << fmt::format("{:.9e}", est.rankGateTol() * std::max(ev_max, 1.0));
    o << '\n';

    // --- Mode detail (strided) -------------------------------------------
    // Naming the columns that span the smallest directions is what turns "λ_min
    // cratered" into "λ_min cratered ALONG a clock+SH direction".  Strided
    // because it is ~kModesReported × kTopColumns rows per traced epoch.
    const int stride = config_.getNetworkConfig().trace_sh_spectrum_stride;
    if (stride > 0 && traces.sh_spectrum_mode_trace.is_open()
        && (ctx.carry.sh_spectrum_epochs % stride) == 0) {
        auto& m = traces.sh_spectrum_mode_trace;
        const Eigen::Index nmodes = std::min<Eigen::Index>(kModesReported, n);
        for (Eigen::Index j = 0; j < nmodes; ++j) {
            const Eigen::VectorXd vj = V.col(j).cwiseAbs();
            std::vector<Eigen::Index> idx(static_cast<std::size_t>(n));
            std::iota(idx.begin(), idx.end(), Eigen::Index{0});
            const std::size_t keep = std::min<std::size_t>(kTopColumns, idx.size());
            std::partial_sort(idx.begin(), idx.begin() + static_cast<std::ptrdiff_t>(keep),
                              idx.end(),
                              [&](Eigen::Index a, Eigen::Index b) { return vj(a) > vj(b); });
            for (std::size_t t = 0; t < keep; ++t) {
                std::string label;
                try { label = est.sharedColumnLabel(idx[t]); }
                catch (...) { label = "col" + std::to_string(idx[t]); }
                for (char& ch : label) if (ch == ',') ch = ';';   // keep one CSV field
                m << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                  << j << ',' << fmt::format("{:.9e}", ev(j)) << ','
                  << label << ',' << fmt::format("{:.6f}", vj(idx[t])) << '\n';
            }
        }
    }
    ++ctx.carry.sh_spectrum_epochs;
}

void networkPostfitSigma0(NetworkRunContext& ctx, const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log,
                          NetworkRunTraces& traces, const SyncedEpoch& se,
                          const std::vector<NetStation>& stations,
                          NetworkEstimator& est,
                          const Eigen::VectorXd& x_s,
                          const std::vector<StationAssembly>& asm_res,
                          const std::vector<int>& est_index_of) {
    (void)config_;   // toggles are expressed through which traces are open
    const std::size_t stations_n = stations.size();
    // §4.3 keyed extension: active only while its trace is open — the OFF path
    // (trace closed) adds zero label lookups and zero pool work.
    const bool keyed = traces.pf_keyed_trace.is_open();
    ctx.carry.ep_sigma0_pf       = 0.0;
    ctx.carry.ep_sigma0_pf_code  = 0.0;
    ctx.carry.ep_sigma0_pf_phase = 0.0;
    ctx.carry.ep_pf_rows         = 0;

    // Shared-column families used to classify each row.
    //  - SH: a row with any nonzero in an SH-VTEC column IS the SH↔slant-iono
    //    pseudo-observation, whose 3σ exceedance is a documented structural
    //    artifact (NetworkIonoDeweightGate.hpp:20-30) — excluded outright.
    //  - PB / CB: the PHASE/CODE discriminator.  An uncombined phase row carries
    //    a satellite PHASE-bias partial and a code row a satellite CODE-bias
    //    partial, so the split is a property of the measurement model and holds
    //    whatever the QC configuration is.  (The within-epoch RAIM's pl_amb set
    //    would be the natural discriminator but is populated only under
    //    phase_leveling — off on the DOY-129 reference run — which would silently
    //    put EVERY row in the code pool.  That is exactly the class of inert
    //    wiring standing invariant #4 exists to catch, so it is not used here.)
    std::vector<char> is_sh_col, is_pb_col, is_cb_col;
    // Keyed extension: per-column sat / signal, parsed from the SAME labels the
    // bitmap pass fetches ("CLK:G01" / "CB:G01:C1W" / "PB:G01:L1C") — no extra
    // label calls, and empty vectors (zero cost) when the keyed trace is closed.
    std::vector<std::string> col_sat, col_sig;
    if (x_s.size() > 0) {
        const std::size_t nsh = static_cast<std::size_t>(x_s.size());
        is_sh_col.assign(nsh, 0);
        is_pb_col.assign(nsh, 0);
        is_cb_col.assign(nsh, 0);
        if (keyed) { col_sat.assign(nsh, {}); col_sig.assign(nsh, {}); }
        for (Eigen::Index c = 0; c < x_s.size(); ++c) {
            std::string lbl;
            try { lbl = est.sharedColumnLabel(c); } catch (...) { continue; }
            const std::size_t u = static_cast<std::size_t>(c);
            if      (lbl.rfind("SH", 0) == 0) is_sh_col[u] = 1;
            else if (lbl.rfind("PB:", 0) == 0) is_pb_col[u] = 1;
            else if (lbl.rfind("CB:", 0) == 0) is_cb_col[u] = 1;
            if (keyed && lbl.rfind("SH", 0) != 0) {
                // "KIND:sat[:signal]" — sat between the first and (optional)
                // second colon, signal after the second.
                const std::size_t p1 = lbl.find(':');
                if (p1 != std::string::npos) {
                    const std::size_t p2 = lbl.find(':', p1 + 1);
                    col_sat[u] = lbl.substr(p1 + 1, (p2 == std::string::npos)
                                                        ? std::string::npos
                                                        : p2 - p1 - 1);
                    if (p2 != std::string::npos) col_sig[u] = lbl.substr(p2 + 1);
                }
            }
        }
    }

    // Keyed pools: SAT / SIG accumulate across stations ({code, phase} pairs);
    // the STA pools are per-station locals written as each station completes.
    std::map<std::string, std::pair<std::vector<double>, std::vector<double>>> sat_pool,
        sig_pool;
    struct PoolStats { std::size_t n; double s0, rms, mx; };
    auto poolStats = [](std::vector<double>& p) -> PoolStats {
        PoolStats s{p.size(), 0.0, 0.0, 0.0};
        if (p.empty()) return s;
        double ss = 0.0;
        for (double v : p) { ss += v * v; if (v > s.mx) s.mx = v; }
        s.rms = std::sqrt(ss / static_cast<double>(p.size()));
        std::nth_element(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(p.size() / 2),
                         p.end());
        s.s0 = 1.4826 * p[p.size() / 2];
        return s;
    };
    auto writeKeyedRow = [&](const char* kind, const std::string& key,
                             std::vector<double>& code, std::vector<double>& phase) {
        const PoolStats c = poolStats(code);
        const PoolStats p = poolStats(phase);
        traces.pf_keyed_trace
            << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
            << kind << ',' << key << ','
            << c.n << ',' << fmt::format("{:.4f}", c.s0) << ','
            << fmt::format("{:.4f}", c.rms) << ',' << fmt::format("{:.4f}", c.mx) << ','
            << p.n << ',' << fmt::format("{:.4f}", p.s0) << ','
            << fmt::format("{:.4f}", p.rms) << ',' << fmt::format("{:.4f}", p.mx) << '\n';
    };

    std::vector<double> pool_all, pool_code, pool_phase;
    for (std::size_t i = 0; i < stations_n && i < asm_res.size(); ++i) {
        if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
        const StationAssembly& R = asm_res[i];
        if (!R.ok || R.A_s.cols() != x_s.size() || R.y.size() == 0) continue;

        Eigen::VectorXd x_r;
        try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s); }
        catch (...) { continue; }
        if (x_r.size() != R.A_r.cols()) continue;

        const Eigen::VectorXd v = R.y - R.A_s * x_s - R.A_r * x_r;

        // Local columns carrying a present ambiguity — the PHASE discriminator
        // (the same pl_amb set the within-epoch RAIM pools over).
        std::vector<char> is_amb_col(static_cast<std::size_t>(R.A_r.cols()), 0);
        for (const auto& pa : R.pl_amb)
            if (pa.col >= 0 && pa.col < R.A_r.cols())
                is_amb_col[static_cast<std::size_t>(pa.col)] = 1;

        // Keyed: this station's own {code, phase} pools (STA row written below).
        std::vector<double> sta_code, sta_phase;

        for (Eigen::Index row = 0; row < v.size(); ++row) {
            // Card-1 coupling rows are statements about the CARRY, not data.
            if (R.cpl_row_start >= 0 && row >= R.cpl_row_start) continue;
            bool sh_row = false, pb_row = false, cb_row = false;
            const std::string* row_sat = nullptr;   // keyed: first labeled col's sat
            const std::string* row_sig = nullptr;   // keyed: first CB/PB col's signal
            for (Eigen::Index c = 0; c < R.A_s.cols(); ++c) {
                if (R.A_s(row, c) == 0.0) continue;
                const std::size_t u = static_cast<std::size_t>(c);
                if (is_sh_col[u]) { sh_row = true; break; }
                if (is_pb_col[u]) pb_row = true;
                else if (is_cb_col[u]) cb_row = true;
                if (keyed) {
                    if (!row_sat && !col_sat[u].empty()) row_sat = &col_sat[u];
                    if (!row_sig && !col_sig[u].empty()) row_sig = &col_sig[u];
                }
            }
            if (sh_row) continue;

            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            const double r = std::abs(v(row)) * std::sqrt(std::max(0.0, w));
            if (!std::isfinite(r)) continue;

            // Ambiguity-column touch is a secondary phase signal for a run whose
            // satellite phase biases are NOT estimated (no PB columns at all).
            bool phase_row = pb_row;
            for (Eigen::Index c = 0; c < R.A_r.cols() && !phase_row; ++c)
                if (is_amb_col[static_cast<std::size_t>(c)] && std::abs(R.A_r(row, c)) > 0.5)
                    phase_row = true;

            pool_all.push_back(r);
            if (phase_row)      pool_phase.push_back(r);
            else if (cb_row)    pool_code.push_back(r);
            // A row touching neither family (e.g. a clock-only pseudo-obs) still
            // counts in pool_all — it is data — but is not attributed to either
            // sub-pool rather than being misfiled.
            if (keyed && (phase_row || cb_row)) {
                auto& sta_pool = phase_row ? sta_phase : sta_code;
                sta_pool.push_back(r);
                if (row_sat) {
                    auto& sp = sat_pool[*row_sat];
                    (phase_row ? sp.second : sp.first).push_back(r);
                }
                if (row_sig) {
                    auto& gp = sig_pool[*row_sig];
                    (phase_row ? gp.second : gp.first).push_back(r);
                }
            }
        }
        if (keyed && (!sta_code.empty() || !sta_phase.empty()))
            writeKeyedRow("STA", stations[i].name, sta_code, sta_phase);
    }
    if (keyed) {
        for (auto& [sat, pools] : sat_pool)
            writeKeyedRow("SAT", sat, pools.first, pools.second);
        for (auto& [sig, pools] : sig_pool)
            writeKeyedRow("SIG", sig, pools.first, pools.second);
    }

    auto robust = [](std::vector<double>& p) -> double {
        if (p.empty()) return 0.0;
        std::nth_element(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(p.size() / 2), p.end());
        return 1.4826 * p[p.size() / 2];
    };
    ctx.carry.ep_pf_rows         = static_cast<long long>(pool_all.size());
    ctx.carry.ep_pf_code_rows    = static_cast<long long>(pool_code.size());
    ctx.carry.ep_pf_phase_rows   = static_cast<long long>(pool_phase.size());
    ctx.carry.ep_sigma0_pf       = robust(pool_all);
    ctx.carry.ep_sigma0_pf_code  = robust(pool_code);
    ctx.carry.ep_sigma0_pf_phase = robust(pool_phase);
    if (ctx.carry.ep_pf_rows > 0) ++ctx.carry.pf_epochs_total;
    else if (ctx.carry.pf_epochs_total == 0 && !ctx.carry.pf_empty_logged) {
        // Invariant #4: an inert diagnostic must say so, not look clean.
        ctx.carry.pf_empty_logged = true;
        log->warn("Network (P1-4): network.trace_postfit_sigma0 is ON but the first "
                  "computed epoch pooled 0 residual rows — every station was skipped "
                  "(no solve membership, dimension mismatch, or reconstruct failure). "
                  "The sigma0_pf columns will stay blank.");
    }
}

}  // namespace GnssProcessor

/**
 * @file NetworkFrameDiag.cpp
 * @brief SPEC_day129_frame_guards Phase 1 trigger instrumentation (P1-3, P1-4) and
 *        the datum-direction information ratio (invariant #7).
 *
 * See NetworkFrameDiag.hpp for the derivation and the row-classification rules.
 * All three functions are measure-only: they read the solved system and write CSV
 * rows, events and counters.
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
/// are "CLK:", "CB:", "PB:", "SH:"/"SH2:" (NetworkEstimator::sharedColumnLabel in
/// NetworkEstimator.cpp); an unrecognized label is charged to the phase-bias bucket
/// instead of being dropped, so the four masses always sum to 1 and a future family
/// stays in the partition.
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

    // reducedSharedNormals() is the cached Schur complement the datum rank gate
    // tested this epoch.  By the post-solve call site the reduction has already
    // happened, so this is a cache read, not a second reduce.
    const Eigen::MatrixXd& Nss = est.reducedSharedNormals();
    const Eigen::Index n = Nss.rows();
    if (n <= 0) return;

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Nss, Eigen::ComputeEigenvectors);
    if (es.info() != Eigen::Success) {
        // Logged once per run: a failed eigensolve is a diagnostic gap, not a solve
        // problem (the solution is untouched), but it is reported.
        if (ctx.carry.sh_spectrum_fail_logged == false) {
            ctx.carry.sh_spectrum_fail_logged = true;
            log->warn("Network (P1-3): SelfAdjointEigenSolver did not converge on the "
                      "{}x{} reduced shared normals (TOW {:.1f}); network_sh_spectrum.csv "
                      "will be missing this and any similarly-failing epoch.",
                      Nss.rows(), Nss.cols(), se.time.tow);
        }
        return;
    }
    const Eigen::VectorXd& ev = es.eigenvalues();      // ascending
    const Eigen::MatrixXd& V  = es.eigenvectors();

    // Per-column family map, built once per epoch: the shared layout is rebuilt
    // every epoch, so it cannot be cached across epochs.
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

    // Mass partition of the smallest direction.  L1 (not L2) mass: the question
    // is how much of this direction lives in the clocks versus the field, and the
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
    // The absolute null-space cutoff the rank gate applies this epoch
    // (rankGateTol · max(λmax,1)).  Without it ev_min cannot be interpreted: these
    // are the pre-datum reduced normals (solve() applies the datum to a copy), so the
    // smallest eigenvalues legitimately include the declared gauge freedoms.  A small
    // λ_min is meaningful only relative to this cutoff.
    o << ',' << fmt::format("{:.9e}", est.rankGateTol() * std::max(ev_max, 1.0));
    o << '\n';

    // --- Mode detail (strided) -------------------------------------------
    // Naming the columns that span the smallest directions shows along which
    // direction (for example clock+SH) λ_min dropped.  Strided because it is
    // ~kModesReported × kTopColumns rows per traced epoch.
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
                          const std::vector<int>& est_index_of,
                          const std::vector<std::vector<PppObservation>>* modeled) {
    (void)config_;   // toggles are expressed through which traces are open
    const std::size_t stations_n = stations.size();
    // Keyed extension (§4.3): active only while its trace is open; with the trace
    // closed it adds no label lookups and no pool work.
    const bool keyed = traces.pf_keyed_trace.is_open();
    // Rows extension (§24.6): the signed per-row dump, gated the same way.  The
    // per-column sat / signal labels are shared with the keyed path.
    const bool rows_on = traces.pf_rows_trace.is_open();
    const bool labels  = keyed || rows_on;
    ctx.carry.ep_sigma0_pf       = 0.0;
    ctx.carry.ep_sigma0_pf_code  = 0.0;
    ctx.carry.ep_sigma0_pf_phase = 0.0;
    ctx.carry.ep_pf_rows         = 0;

    // Shared-column families used to classify each row.
    //  - SH: a row with any nonzero in an SH-VTEC column is the SH↔slant-iono
    //    pseudo-observation, whose 3σ exceedance is a documented structural
    //    artifact (NetworkIonoDeweightGate.hpp:20-30); it is excluded outright.
    //  - PB / CB: the phase/code discriminator.  An uncombined phase row carries
    //    a satellite phase-bias partial and a code row a satellite code-bias
    //    partial, so the split is a property of the measurement model and holds
    //    whatever the QC configuration is.  The within-epoch RAIM's pl_amb set is
    //    not the primary discriminator because it is populated only under
    //    phase_leveling; without leveling it would put every row in the code pool
    //    (the inert wiring standing invariant #4 guards against).  It serves only
    //    as the secondary phase signal below.
    //  - CLK: the code side's fallback, the counterpart of the ambiguity-column
    //    touch on the phase side (card ZP; found by card PV).  A satellite
    //    code-bias column is not present in every posture: with the biases
    //    supplied externally (a .BIA product, "0 code biases") there are no CB
    //    columns, and with them estimated the W6 datum signal's OSB is the gauge
    //    and carries no column either (§24.9: the residual-pool trace omitted every
    //    C1C row at WROC).  Without the fallback every such code row goes to the
    //    unattributed counter and the wire census sees no code arcs on an
    //    external-bias posture.  Every real observation equation carries the
    //    satellite clock partial, so a data row that touches a CLK column and is
    //    not phase is a code row.  Rows with no shared partial (pivot pins, local
    //    prior rows) touch neither and stay unattributed, which is what that
    //    counter means.  Remaining limitation: such a row has no CB/PB label, so
    //    its label-derived signal is empty and it joins the per-station and
    //    per-satellite keyed pools but not the per-signal one (the rows trace
    //    takes the observation's own signal instead; see below).  The receiver
    //    code-bias local column does not help: on this lineage it is one
    //    per-constellation "GPS_DCB", not a per-signal state.
    std::vector<char> is_sh_col, is_pb_col, is_cb_col, is_clk_col;
    // Keyed extension: per-column sat / signal, parsed from the labels the bitmap
    // pass already fetches ("CLK:G01" / "CB:G01:C1W" / "PB:G01:L1C"): no extra
    // label calls, and empty vectors (no cost) when neither the keyed nor the rows
    // trace is open.
    std::vector<std::string> col_sat, col_sig;
    if (x_s.size() > 0) {
        const std::size_t nsh = static_cast<std::size_t>(x_s.size());
        is_sh_col.assign(nsh, 0);
        is_pb_col.assign(nsh, 0);
        is_cb_col.assign(nsh, 0);
        is_clk_col.assign(nsh, 0);
        if (labels) { col_sat.assign(nsh, {}); col_sig.assign(nsh, {}); }
        for (Eigen::Index c = 0; c < x_s.size(); ++c) {
            std::string lbl;
            try { lbl = est.sharedColumnLabel(c); } catch (...) { continue; }
            const std::size_t u = static_cast<std::size_t>(c);
            if      (lbl.rfind("SH", 0) == 0) is_sh_col[u] = 1;
            else if (lbl.rfind("PB:", 0) == 0) is_pb_col[u] = 1;
            else if (lbl.rfind("CB:", 0) == 0) is_cb_col[u] = 1;
            else if (lbl.rfind("CLK:", 0) == 0) is_clk_col[u] = 1;
            if (labels && lbl.rfind("SH", 0) != 0) {
                // "KIND:sat[:signal]": sat between the first and (optional)
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

        // Local columns carrying a present ambiguity: the secondary phase
        // discriminator (the pl_amb set the within-epoch RAIM pools over).
        std::vector<char> is_amb_col(static_cast<std::size_t>(R.A_r.cols()), 0);
        for (const auto& pa : R.pl_amb)
            if (pa.col >= 0 && pa.col < R.A_r.cols())
                is_amb_col[static_cast<std::size_t>(pa.col)] = 1;

        // Keyed: this station's own {code, phase} pools (STA row written below).
        std::vector<double> sta_code, sta_phase;
        // Rows: this station's satellite -> elevation (deg) off the modelled set,
        // built only while the rows trace is open (no cost otherwise).
        std::map<std::string, double> sta_el;
        if (rows_on && modeled && i < modeled->size())
            for (const PppObservation& o : (*modeled)[i]) sta_el[o.satellite] = o.elevation;

        for (Eigen::Index row = 0; row < v.size(); ++row) {
            // Card-1 coupling rows are statements about the carry, not data.
            if (R.cpl_row_start >= 0 && row >= R.cpl_row_start) continue;
            bool sh_row = false, pb_row = false, cb_row = false, clk_row = false;
            const std::string* row_sat = nullptr;   // keyed: first labeled col's sat
            const std::string* row_sig = nullptr;   // keyed: first CB/PB col's signal
            for (Eigen::Index c = 0; c < R.A_s.cols(); ++c) {
                if (R.A_s(row, c) == 0.0) continue;
                const std::size_t u = static_cast<std::size_t>(c);
                if (is_sh_col[u]) { sh_row = true; break; }
                if (is_pb_col[u]) pb_row = true;
                else if (is_cb_col[u]) cb_row = true;
                else if (is_clk_col[u]) clk_row = true;
                if (labels) {
                    if (!row_sat && !col_sat[u].empty()) row_sat = &col_sat[u];
                    if (!row_sig && !col_sig[u].empty()) row_sig = &col_sig[u];
                }
            }
            if (sh_row) continue;

            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            const double r = std::abs(v(row)) * std::sqrt(std::max(0.0, w));
            if (!std::isfinite(r)) continue;

            // Ambiguity-column touch is a secondary phase signal for a run whose
            // satellite phase biases are not estimated (no PB columns at all).
            bool phase_row = pb_row;
            for (Eigen::Index c = 0; c < R.A_r.cols() && !phase_row; ++c)
                if (is_amb_col[static_cast<std::size_t>(c)] && std::abs(R.A_r(row, c)) > 0.5)
                    phase_row = true;

            // Code = a data row that is not phase and carries a satellite partial:
            // the CB column when the posture estimates one, else the CLK column
            // every observation equation has (see the CLK note at the family
            // bitmaps).  A row with neither (no shared partial: a pivot pin, a local
            // prior row) still enters pool_all but stays unattributed instead of
            // being misfiled.
            const bool code_row = !phase_row && (cb_row || clk_row);

            pool_all.push_back(r);
            if (phase_row)      pool_phase.push_back(r);
            else if (code_row)  pool_code.push_back(r);
            if (keyed && (phase_row || code_row)) {
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
            if (rows_on) {
                if (phase_row || code_row) {
                    // Signed v (metres), the row weight, and the LOS elevation.  The
                    // sat comes from the CLK/CB/PB labels the keyed pools key on.  The
                    // signal is the observation's own (the assembly's row_sig,
                    // row-aligned with H for the data rows); an appended row falls back
                    // to the first labelled column's.  The label alone would leave a row
                    // whose only shared column is the clock with an empty signal, and
                    // every (station, sat, signal) grade would pool such rows across
                    // signals (card OWD, O5).
                    const std::string* obs_sig =
                        (row < static_cast<Eigen::Index>(R.row_sig.size())
                         && !R.row_sig[static_cast<std::size_t>(row)].empty())
                            ? &R.row_sig[static_cast<std::size_t>(row)] : nullptr;
                    const std::string* sig_out = obs_sig ? obs_sig : row_sig;
                    if (!sig_out) ++ctx.carry.pf_rows_no_signal;
                    if (obs_sig && row_sig && *obs_sig != *row_sig)
                        ++ctx.carry.pf_rows_sig_relabel;
                    auto& t = traces.pf_rows_trace;
                    t << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                      << stations[i].name << ','
                      << (row_sat ? *row_sat : std::string()) << ','
                      << (sig_out ? *sig_out : std::string()) << ','
                      << (phase_row ? "phase" : "code") << ','
                      << fmt::format("{:.5f}", v(row)) << ','
                      << fmt::format("{:.6e}", w) << ',';
                    const auto el = row_sat ? sta_el.find(*row_sat) : sta_el.end();
                    if (el != sta_el.end()) t << fmt::format("{:.2f}", el->second);
                    else ++ctx.carry.pf_rows_no_elev;
                    t << '\n';
                    ++ctx.carry.pf_rows_written;
                    ++(phase_row ? ctx.carry.pf_rows_phase : ctx.carry.pf_rows_code);
                } else {
                    ++ctx.carry.pf_rows_unattributed;
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

void networkDatumInfoRatio(NetworkRunContext& ctx, NetworkRunTraces& traces,
                           const SyncedEpoch& se, NetworkEstimator& est,
                           int declared_dim,
                           const std::vector<std::string>& registered_clock_sats,
                           double ev_max, bool gate_over_withheld,
                           const Eigen::MatrixXd* N_gate) {
    // The gate writes ev_max < 0 on any exit that computed no spectrum (empty
    // normals, non-square, solver failure).  There is then nothing to measure
    // against, and a cutoff reconstructed from a guess would mislead.
    if (!(ev_max >= 0.0) || declared_dim <= 0) return;
    // Card SCL: the gate's own view when it judged one (the clock clones marginalized;
    // the clock columns keep their indices); the reduced normals otherwise.
    const Eigen::MatrixXd& N = N_gate ? *N_gate : est.reducedSharedNormals();
    const Eigen::Index n = N.rows();
    if (n == 0 || N.cols() != n) return;

    // The gate's own cutoff, reconstructed from its own λmax: no second
    // decomposition and no re-derived tolerance, because every consumer reads the
    // one resolved rank_gate_tol (card DG2).
    const double cutoff = est.rankGateTol() * std::max(ev_max, 1.0);
    if (!(cutoff > 0.0)) return;

    // Declared datum directions, built analytically.  Plan::declared_dimension counts
    // one satellite-clock gauge per constellation in pin mode, and the direction is
    // the indicator over that constellation's clock columns: what
    // NetworkSbasis::buildSharedDatum groups (`by_const`) and what the sum-to-zero
    // branch writes out as an explicit all-ones row.  No eigensolve is involved:
    // vᵀNv needs the direction, not the spectrum.
    std::map<char, std::vector<Eigen::Index>> by_const;
    for (const std::string& sat : registered_clock_sats) {
        if (sat.empty()) continue;
        try { by_const[sat[0]].push_back(est.clockColumn(sat)); }
        catch (...) {}   // not in this epoch's layout, so not part of this epoch's gauge
    }
    if (by_const.empty()) return;

    ++ctx.counters.ddi_epochs;
    bool any_breach = false;
    std::set<std::string> evaluated;   // invariant #7: what this epoch actually judged
    for (const auto& [cc, cols] : by_const) {
        // A one-member group is not a gauge direction in any useful sense (the lone
        // clock is the datum), and normalizing it would report the column's own
        // information as a gauge breach on every epoch.
        if (cols.size() < 2) continue;
        // A range error is tracked by its own flag, not by the sign of q.  N is
        // positive-semidefinite only as a whole: its off-diagonals can be negative, so
        // a partial sum of vᵀNv routinely dips below zero.  Using q's sign as the error
        // sentinel would abort the accumulation on ordinary data and report zero
        // directions on every epoch.
        bool bad = false;
        double q = 0.0;
        for (const Eigen::Index a : cols) {
            if (a < 0 || a >= n) { bad = true; break; }
            for (const Eigen::Index b : cols) {
                if (b < 0 || b >= n) { bad = true; break; }
                q += N(a, b);      // vᵀNv with v = indicator (unnormalized)
            }
            if (bad) break;
        }
        // The completed quadratic form is >= 0 because N is PSD at the gate; a negative
        // final value is rounding at the gauge and is floored to 0 instead of being
        // discarded (a gauge direction reading exactly 0 is the expected case, and
        // dropping it would make that case indistinguishable from a miswire).
        if (bad || !std::isfinite(q)) continue;
        if (q < 0.0) q = 0.0;
        // vᵀv = |cols| for the indicator, so the Rayleigh quotient is q/|cols|.
        const double rq    = q / static_cast<double>(cols.size());
        const double ratio = rq / cutoff;
        if (!std::isfinite(ratio)) continue;
        ++ctx.counters.ddi_dirs;
        const std::string id = fmt::format("CLK|{}", cc);
        evaluated.insert(id);
        if (ctx.counters.ddi_ratio_max < 0.0 || ratio > ctx.counters.ddi_ratio_max) {
            ctx.counters.ddi_ratio_max     = ratio;
            ctx.counters.ddi_ratio_max_tow = se.time.tow;
            ctx.counters.ddi_ratio_max_dir = id;
        }
        if (ctx.counters.ddi_ratio_min < 0.0 || ratio < ctx.counters.ddi_ratio_min)
            ctx.counters.ddi_ratio_min = ratio;

        // Standing invariant #7: a declared gauge taking or losing information changes
        // which information flows into which states, so both edges are events.  The
        // breach edge says a declared gauge stopped being free; the clear edge says it
        // became free again, so a returning direction does not read as a first
        // sighting (FT63 topology ledger).
        const bool breach = ratio >= 1.0;
        const bool was    = ctx.carry.ddi_breached.count(id) > 0;
        if (breach) any_breach = true;
        if (breach != was) {
            ++ctx.counters.ddi_breach_edges;
            if (breach) ctx.carry.ddi_breached.insert(id);
            else        ctx.carry.ddi_breached.erase(id);
            traces.emitEvent(se.time, "GATE", breach ? "ddi_breach" : "ddi_clear",
                             std::string(1, cc), "", ratio, 1.0,
                             breach ? "gauge_pinned" : "gauge_free",
                             fmt::format("{} vtNv/|v|={:.6e} cutoff={:.6e} ncol={}",
                                         id, rq, cutoff, cols.size()));
        }
    }
    // A latched direction that is no longer evaluated (its constellation dropped below
    // two registered clocks, or left the layout) emits its clear edge here.  Otherwise
    // the flag would stay latched, and when the direction returned and breached again no
    // edge would fire: the re-coupling would read as already breached, the failure
    // invariant #7 names (a returning member mistaken for a continuing one).
    for (auto it = ctx.carry.ddi_breached.begin(); it != ctx.carry.ddi_breached.end();) {
        if (evaluated.count(*it)) { ++it; continue; }
        ++ctx.counters.ddi_breach_edges;
        traces.emitEvent(se.time, "GATE", "ddi_clear",
                         it->size() > 4 ? it->substr(4) : std::string{}, "",
                         std::nullopt, 1.0, "not_evaluated",
                         fmt::format("{} left the evaluated set (constellation below two "
                                     "registered clocks, or absent from the layout)", *it));
        it = ctx.carry.ddi_breached.erase(it);
    }
    if (any_breach) {
        ++ctx.counters.ddi_breach_epochs;
        // The coincidence that makes the run-end advice actionable: an over-specified
        // withhold on a breached epoch indicates a filled null, not a misplaced tolerance.
        if (gate_over_withheld) ++ctx.counters.ddi_over_breach;
    }
}

}  // namespace GnssProcessor

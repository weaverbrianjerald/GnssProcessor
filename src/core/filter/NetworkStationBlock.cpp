/**
 * @file NetworkStationBlock.cpp
 * @brief Implementation of the station-block audit (SPEC_event_harness §24.10,
 *        card ZP): the pure algebra (@ref GnssProcessor::StationBlockAudit) and the
 *        measure-only seam that writes @c network_station_block.csv.
 */

#include "filter/NetworkStationBlock.hpp"

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "filter/StateManager.hpp"
#include "logging/NetworkRunTraces.hpp"

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace GnssProcessor {
namespace StationBlockAudit {

std::string spanOf(const Eigen::VectorXd& v, const std::vector<std::string>& labels,
                   double cover) {
    const Eigen::Index n = v.size();
    if (n <= 0) return {};
    // The eigenvector is unit-norm by construction, but a caller may pass an
    // un-normalized direction; normalize the shares so they are comparable across
    // rows of the trace whatever the caller did.
    double total = 0.0;
    for (Eigen::Index k = 0; k < n; ++k) total += v(k) * v(k);
    if (!(total > 0.0) || !std::isfinite(total)) return {};

    std::vector<Eigen::Index> order(static_cast<std::size_t>(n));
    std::iota(order.begin(), order.end(), Eigen::Index{0});
    // Descending squared loading, ties broken by column index, so the field is a
    // deterministic function of the eigenvector and two arms of an A/B cannot
    // differ through the order of an equal pair.
    std::stable_sort(order.begin(), order.end(),
                     [&v](Eigen::Index a, Eigen::Index b) {
                         const double sa = v(a) * v(a), sb = v(b) * v(b);
                         if (sa != sb) return sa > sb;
                         return a < b;
                     });

    std::string out;
    double cum = 0.0;
    for (std::size_t t = 0; t < order.size() && t < 3; ++t) {
        const Eigen::Index k = order[t];
        const double share = (v(k) * v(k)) / total;
        const std::size_t ku = static_cast<std::size_t>(k);
        std::string label = (ku < labels.size() && !labels[ku].empty())
                                ? labels[ku]
                                : ("col" + std::to_string(k));
        // One CSV field: a label may not carry the delimiter or the separators.
        for (char& ch : label)
            if (ch == ',' || ch == '|' || ch == '=') ch = ';';
        if (!out.empty()) out.push_back('|');
        out += label;
        out.push_back('=');
        out += fmt::format("{:.3f}", share);
        cum += share;
        if (cum >= cover) break;   // the requested share is covered; the tail is omitted
    }
    return out;
}

Spectrum spectrum(const Eigen::MatrixXd& Nrr, const std::vector<std::string>& labels) {
    Spectrum sp;
    sp.n_local = Nrr.rows();
    if (Nrr.rows() <= 0 || Nrr.rows() != Nrr.cols()) return sp;
    if (!Nrr.allFinite()) return sp;   // a non-finite block is refused (ok=false)

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(Nrr);
    if (es.info() != Eigen::Success) return sp;

    sp.eigenvalues  = es.eigenvalues();    // ascending, by Eigen's contract
    sp.eigenvectors = es.eigenvectors();
    const Eigen::Index n = sp.eigenvalues.size();
    const double lmax = sp.eigenvalues(n - 1);
    // A block whose largest eigenvalue is not positive carries no information;
    // report rank 0 rather than dividing by it.
    const double floor_abs = (lmax > 0.0) ? kRankTolRel * lmax : 0.0;

    Eigen::Index rank = 0;
    double lmin_nonnull = std::numeric_limits<double>::infinity();
    for (Eigen::Index k = 0; k < n; ++k) {
        if (sp.eigenvalues(k) > floor_abs && sp.eigenvalues(k) > 0.0) {
            ++rank;
            lmin_nonnull = std::min(lmin_nonnull, sp.eigenvalues(k));
        }
    }
    sp.rank = rank;
    sp.cond = (rank > 0 && std::isfinite(lmin_nonnull) && lmin_nonnull > 0.0)
                  ? lmax / lmin_nonnull
                  : 0.0;

    for (int t = 0; t < 3; ++t) {
        if (t < n) {
            sp.ev_val[static_cast<std::size_t>(t)] = sp.eigenvalues(t);
            sp.ev_span[static_cast<std::size_t>(t)] =
                spanOf(sp.eigenvectors.col(t), labels);
        } else {
            sp.ev_val[static_cast<std::size_t>(t)] =
                std::numeric_limits<double>::quiet_NaN();
        }
    }
    sp.ok = true;
    return sp;
}

double effectiveInformation(const Spectrum& sp, Eigen::Index col) {
    if (!sp.ok || col < 0 || col >= sp.n_local) return 0.0;
    const Eigen::Index n = sp.eigenvalues.size();
    if (n <= 0) return 0.0;
    const double lmax = sp.eigenvalues(n - 1);
    const double floor_abs = (lmax > 0.0) ? kRankTolRel * lmax : 0.0;
    // (N^+)_{zz} = sum over the non-null subspace of V_{zk}^2 / lambda_k.  The
    // Schur complement of the column against every other local state is its
    // reciprocal; see the header for why the identity is used instead of an
    // explicit elimination of N_oo.
    double acc = 0.0;
    for (Eigen::Index k = 0; k < n; ++k) {
        if (!(sp.eigenvalues(k) > floor_abs) || !(sp.eigenvalues(k) > 0.0)) continue;
        const double vz = sp.eigenvectors(col, k);
        acc += (vz * vz) / sp.eigenvalues(k);
    }
    if (!(acc > 0.0) || !std::isfinite(acc)) return 0.0;   // no mass in the non-null subspace
    return 1.0 / acc;
}

Eigen::VectorXd redundancy(const Spectrum& sp, const Eigen::MatrixXd& A_r,
                           const Eigen::VectorXd& w) {
    if (!sp.ok || A_r.rows() <= 0 || A_r.cols() != sp.n_local) return {};
    const Eigen::Index n = sp.eigenvalues.size();
    if (n <= 0) return {};
    const double lmax = sp.eigenvalues(n - 1);
    const double floor_abs = (lmax > 0.0) ? kRankTolRel * lmax : 0.0;

    // Non-null columns only, so the pseudo-inverse and the rank agree by
    // construction: P = A_r V_nn, then h_i^T N^+ h_i = sum_k P(i,k)^2 / lambda_k.
    std::vector<Eigen::Index> keep;
    keep.reserve(static_cast<std::size_t>(n));
    for (Eigen::Index k = 0; k < n; ++k)
        if (sp.eigenvalues(k) > floor_abs && sp.eigenvalues(k) > 0.0) keep.push_back(k);

    Eigen::VectorXd r = Eigen::VectorXd::Ones(A_r.rows());
    if (keep.empty()) return r;   // a rank-0 block checks nothing; every r_i is 1

    Eigen::MatrixXd Vnn(sp.n_local, static_cast<Eigen::Index>(keep.size()));
    Eigen::VectorXd inv_lam(static_cast<Eigen::Index>(keep.size()));
    for (std::size_t t = 0; t < keep.size(); ++t) {
        Vnn.col(static_cast<Eigen::Index>(t)) = sp.eigenvectors.col(keep[t]);
        inv_lam(static_cast<Eigen::Index>(t)) = 1.0 / sp.eigenvalues(keep[t]);
    }
    const Eigen::MatrixXd P = A_r * Vnn;            // m x k
    for (Eigen::Index i = 0; i < A_r.rows(); ++i) {
        const double wi = (i < w.size()) ? w(i) : 1.0;
        double q = 0.0;
        for (Eigen::Index t = 0; t < P.cols(); ++t) q += P(i, t) * P(i, t) * inv_lam(t);
        r(i) = 1.0 - wi * q;
    }
    return r;
}

}  // namespace StationBlockAudit

namespace {

/// One local column's kind label for the span field.  Built from the station's
/// StateManager key maps through the assembly's H-column -> A_r-column map, so
/// the labels are correct under both vehicles (nothing here is carry-gated).
///
/// Every state kind the manager exposes is covered, because an unlabelled column
/// appears in a span as `col<k>`, which names no state and so no mechanism.
/// Kinds: the card's `clk`, `zwd`, `rxcb:<sig>`, `iono:<sat>`,
/// `amb:<sat>:<sig>`, plus `pos:<xyz>`, `vel:<xyz>`, `clkrate`, `phclk`,
/// `grdn` / `grde`, `dzwd`, `iono` (the single-LOS state), `ionorate:<sat>`,
/// `isb:<sys>`, `satbias:<key>`, `satpb:<key>`, `rxpb:<key>` and
/// `satclk:<sat>`.  The `col<k>` fallback survives only for a local column no
/// state map claims at all.
std::vector<std::string> buildLocalLabels(const StationAssembly& R,
                                          const StateManager& sm,
                                          Eigen::Index n_local,
                                          std::vector<char>& is_amb_col) {
    std::vector<std::string> labels(static_cast<std::size_t>(n_local));
    is_amb_col.assign(static_cast<std::size_t>(n_local), 0);
    const auto& h2l = R.h2local;
    auto put = [&](std::size_t state_idx, std::string label, bool amb) {
        const auto hit = h2l.find(static_cast<int>(state_idx));
        if (hit == h2l.end() || hit->second < 0) return;
        const Eigen::Index c = static_cast<Eigen::Index>(hit->second);
        if (c >= n_local) return;
        labels[static_cast<std::size_t>(c)] = std::move(label);
        if (amb) is_amb_col[static_cast<std::size_t>(c)] = 1;
    };

    const std::size_t kNone = std::numeric_limits<std::size_t>::max();
    auto putScalar = [&](std::size_t idx, const char* label) {
        if (idx != kNone) put(idx, label, false);
    };
    // Core block (index-stable by construction).
    if (sm.getPositionIndex() != kNone) {
        const std::size_t p = sm.getPositionIndex();
        put(p + 0, "pos:x", false); put(p + 1, "pos:y", false); put(p + 2, "pos:z", false);
    }
    if (sm.getVelocityIndex() != kNone) {
        const std::size_t v = sm.getVelocityIndex();
        put(v + 0, "vel:x", false); put(v + 1, "vel:y", false); put(v + 2, "vel:z", false);
    }
    putScalar(sm.getClockIndex(), "clk");
    putScalar(sm.getClockRateIndex(), "clkrate");
    putScalar(sm.getPhaseClockIndex(), "phclk");
    putScalar(sm.getTropoIndex(), "zwd");
    putScalar(sm.getDifferentialTropoIndex(), "dzwd");
    putScalar(sm.getTropoGradientNorthIndex(), "grdn");
    putScalar(sm.getTropoGradientEastIndex(), "grde");
    putScalar(sm.getIonoIndex(), "iono");
    // Dynamic key -> index maps.
    for (const auto& [key, idx] : sm.getCodeBiasStates())         put(idx, "rxcb:" + key, false);
    for (const auto& [sat, idx] : sm.getIonosphereStates())       put(idx, "iono:" + sat, false);
    for (const auto& [sat, idx] : sm.getIonosphereRateStates())   put(idx, "ionorate:" + sat, false);
    for (const auto& [sys, idx] : sm.getSystemTimeOffsetStates()) put(idx, "isb:" + sys, false);
    for (const auto& [key, idx] : sm.getSatBiasStates())          put(idx, "satbias:" + key, false);
    for (const auto& [key, idx] : sm.getSatPhaseBiasStates())     put(idx, "satpb:" + key, false);
    for (const auto& [key, idx] : sm.getReceiverPhaseBiasStates())put(idx, "rxpb:" + key, false);
    for (const auto& [sat, idx] : sm.getSatClockStates())         put(idx, "satclk:" + sat, false);
    for (const auto& [key, idx] : sm.getAmbiguities()) {
        // "<sat>_<signal>" -> "amb:<sat>:<signal>"; an unparseable key is labelled
        // "amb:<raw key>" rather than dropped, so the column is still named and
        // still marked as an ambiguity column.
        const auto parts = StateKeys::parseAmbiguityKey(key);
        put(idx,
            parts ? ("amb:" + parts->satellite + ':' + parts->signal) : ("amb:" + key),
            true);
    }
    return labels;
}

}  // namespace

void networkStationBlock(NetworkRunContext& ctx, const GnssConfig& config_,
                         const std::shared_ptr<spdlog::logger>& log,
                         NetworkRunTraces& traces, const SyncedEpoch& se,
                         const std::vector<NetStation>& stations,
                         NetworkEstimator& est,
                         const Eigen::VectorXd& x_s,
                         const std::vector<StationAssembly>& asm_res,
                         const std::vector<int>& est_index_of) {
    (void)config_;   // the toggle is expressed through which trace is open
    auto& t = traces.station_block_trace;
    if (!t.is_open()) return;   // off path: the driver does not call this; guard kept

    // Cadence: one row set per emitted epoch, for emissions 1..20 and every 60th
    // thereafter.  The gate keys on n_emitted rather than on the seam running
    // (C1/R11): an epoch withheld by the observability gate, the A4 sigma0 ceiling
    // or the SH health gate still reaches this group, and auditing it would mix
    // withheld epochs into a trace that describes the wire.  sb_last_emitted keeps
    // a repeated emission count from writing twice.
    const std::size_t ne = ctx.counters.n_emitted;
    if (ne == 0 || ne == ctx.carry.sb_last_emitted) return;
    ctx.carry.sb_last_emitted = ne;
    if (!(ne <= 20 || (ne % 60) == 0)) return;

    // Phase/code discriminator, built as the P1-4 post-fit pass builds it: a
    // satellite phase-bias shared column ("PB:") marks a phase row, a satellite
    // code-bias column ("CB:") a code row, and an SH-VTEC column ("SH") marks the
    // SH<->slant-iono pseudo-observation, which is not data.  The ambiguity local
    // columns are the second phase marker (a posture that estimates no satellite
    // phase biases has no PB columns).  Unlike the pass's `pl_amb` set, they are
    // read from the StateManager, so the discriminator also works under
    // `carry_invariant`.
    std::vector<char> is_sh_col, is_pb_col, is_cb_col;
    if (x_s.size() > 0) {
        const std::size_t nsh = static_cast<std::size_t>(x_s.size());
        is_sh_col.assign(nsh, 0);
        is_pb_col.assign(nsh, 0);
        is_cb_col.assign(nsh, 0);
        for (Eigen::Index c = 0; c < x_s.size(); ++c) {
            std::string lbl;
            try { lbl = est.sharedColumnLabel(c); } catch (...) { continue; }
            const std::size_t u = static_cast<std::size_t>(c);
            if      (lbl.rfind("SH", 0) == 0)  is_sh_col[u] = 1;
            else if (lbl.rfind("PB:", 0) == 0) is_pb_col[u] = 1;
            else if (lbl.rfind("CB:", 0) == 0) is_cb_col[u] = 1;
        }
    }

    long long rows_this_epoch = 0;
    const std::size_t stations_n = stations.size();
    for (std::size_t i = 0; i < stations_n && i < asm_res.size(); ++i) {
        if (i >= est_index_of.size() || est_index_of[i] < 0) {
            ++ctx.carry.sb_stations_skipped;
            continue;
        }
        const StationAssembly& R = asm_res[i];
        if (!R.ok || R.A_r.cols() <= 0 || R.A_s.cols() != x_s.size()) {
            ++ctx.carry.sb_stations_skipped;
            continue;
        }
        const std::size_t ei = static_cast<std::size_t>(est_index_of[i]);

        Eigen::MatrixXd Nrr;
        Eigen::VectorXd x_r;
        try {
            // Rebuilt from the cached LDL^T factor, so the station priors folded
            // through NetworkEstimator::addStationPrior (the leveling carry, the F3
            // block, the pivot pins) are included.  They are the only structural
            // difference between the two vehicles' blocks, which is why the audit
            // reads this matrix.
            Nrr = est.normals().stationLocalNormals(ei);
            x_r = est.reconstructStation(ei, x_s);
        } catch (...) {
            ++ctx.carry.sb_decomp_failed;
            continue;
        }
        if (Nrr.rows() <= 0 || Nrr.rows() != R.A_r.cols() || x_r.size() != R.A_r.cols()) {
            ++ctx.carry.sb_decomp_failed;
            continue;
        }

        std::vector<char> is_amb_col;
        const std::vector<std::string> labels =
            stations[i].sm ? buildLocalLabels(R, *stations[i].sm, Nrr.rows(), is_amb_col)
                           : std::vector<std::string>{};
        if (is_amb_col.empty())
            is_amb_col.assign(static_cast<std::size_t>(Nrr.rows()), 0);

        const StationBlockAudit::Spectrum sp = StationBlockAudit::spectrum(Nrr, labels);
        if (!sp.ok) { ++ctx.carry.sb_decomp_failed; continue; }

        // The ZWD column, located through getTropoIndex + h2local rather than a
        // carry-gated flag (see networkIonoDiag's L7 block for the same reason).
        Eigen::Index zcol = -1;
        if (stations[i].sm) {
            const std::size_t tropo_sz = stations[i].sm->getTropoIndex();
            if (tropo_sz != std::numeric_limits<std::size_t>::max()) {
                const auto zhit = R.h2local.find(static_cast<int>(tropo_sz));
                if (zhit != R.h2local.end() && zhit->second >= 0
                    && static_cast<Eigen::Index>(zhit->second) < Nrr.rows())
                    zcol = static_cast<Eigen::Index>(zhit->second);
            }
        }
        const double zwd_raw = (zcol >= 0) ? Nrr(zcol, zcol) : 0.0;
        const double zwd_eff = (zcol >= 0)
                                   ? StationBlockAudit::effectiveInformation(sp, zcol)
                                   : 0.0;

        // Row census.  Redundancy is computed over every row of A_r (all of them
        // enter N_rr), but only data rows are counted: the SH pseudo-observation
        // constrains the iono field and the Card-1 coupling rows (from
        // cpl_row_start on) constrain the carry, so neither is an observation whose
        // checkability the card measures.  A data row that is neither phase nor
        // code counts toward n_data only.
        const Eigen::VectorXd red = StationBlockAudit::redundancy(sp, R.A_r, R.w);
        long long n_code = 0, n_phase = 0, n_data = 0, zr_code = 0, zr_phase = 0;
        for (Eigen::Index row = 0; row < R.A_r.rows() && row < red.size(); ++row) {
            if (R.cpl_row_start >= 0 && row >= R.cpl_row_start) continue;
            bool sh_row = false, pb_row = false, cb_row = false;
            for (Eigen::Index c = 0; c < R.A_s.cols(); ++c) {
                if (R.A_s(row, c) == 0.0) continue;
                const std::size_t u = static_cast<std::size_t>(c);
                if (is_sh_col[u]) { sh_row = true; break; }
                if (is_pb_col[u]) pb_row = true;
                else if (is_cb_col[u]) cb_row = true;
            }
            if (sh_row) continue;
            bool phase_row = pb_row;
            for (Eigen::Index c = 0; c < R.A_r.cols() && !phase_row; ++c)
                if (is_amb_col[static_cast<std::size_t>(c)] && std::abs(R.A_r(row, c)) > 0.5)
                    phase_row = true;
            ++n_data;
            const bool zero_red = (red(row) < StationBlockAudit::kZeroRedundancy);
            if (phase_row)   { ++n_phase; if (zero_red) ++zr_phase; }
            else if (cb_row) { ++n_code;  if (zero_red) ++zr_code;  }
        }

        auto num = [](double v) {
            return std::isfinite(v) ? fmt::format("{:.6e}", v) : std::string();
        };
        t << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
          << stations[i].name << ',' << n_code << ',' << n_phase << ','
          << sp.n_local << ',' << sp.rank << ',' << num(sp.cond) << ','
          << num(sp.ev_val[0]) << ',' << sp.ev_span[0] << ','
          << num(sp.ev_val[1]) << ',' << sp.ev_span[1] << ','
          << num(sp.ev_val[2]) << ',' << sp.ev_span[2] << ',';
        if (zcol >= 0) {
            t << num(zwd_eff) << ',' << num(zwd_raw) << ','
              << ((zwd_raw > 0.0) ? num(zwd_eff / zwd_raw) : std::string()) << ','
              << num(x_r.norm()) << ',' << fmt::format("{:.6f}", x_r(zcol)) << ',';
        } else {
            // No ZWD column in this epoch's A_r: zwd_info_eff / zwd_info_raw /
            // zwd_info_ratio / x_r_zwd are empty rather than 0, so a reader cannot
            // average a missing state into a ratio.
            t << /* zwd_info_eff   */ ','
              << /* zwd_info_raw   */ ','
              << /* zwd_info_ratio */ ','
              << num(x_r.norm())   << ','
              << /* x_r_zwd        */ ',';
        }
        t << zr_code << ',' << zr_phase << ',' << n_data << '\n';
        ++rows_this_epoch;
        ++ctx.carry.sb_rows_written;
    }
    if (rows_this_epoch > 0) ++ctx.carry.sb_epochs_written;
    else
        // Invariant #4: an armed sweep that audits no station logs one line per
        // traced epoch instead of leaving an unexplained gap in the CSV.
        log->debug("Network (§24.10 station block): traced emission {} audited 0 station(s) "
                   "({} skipped, {} undecomposable)",
                   ne, ctx.carry.sb_stations_skipped, ctx.carry.sb_decomp_failed);
}

}  // namespace GnssProcessor

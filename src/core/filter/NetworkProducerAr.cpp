/**
 * @file NetworkProducerAr.cpp
 * @brief Implementation of @ref networkProducerAr — the log-only producer-side
 *        network-AR (Phase 2) diagnostic + one-shot NEQ export, relocated VERBATIM
 *        (dedent-4, zero substitution) out of Application::runNetwork at decomposition
 *        INC-9b (2026-07-22).
 *
 * The body below is a byte-copy of the former SB_NETAR block, dedented 4 spaces (the
 * block sat at loop-body indent, no wrapping if).  The alias preamble reconstructs the
 * three run-TU aliases the body reads (nc / netar / n_emitted); every other name is a
 * parameter named exactly as the former run-TU local.  See NetworkProducerAr.hpp.
 */

#include "filter/NetworkProducerAr.hpp"

#include "app/NetworkRunContext.hpp"     // ctx.counters.n_emitted
#include "config/GnssConfig.hpp"         // config_.getNetworkConfig() / .ambiguity_resolution
#include "common/GnssTypes.hpp"          // System
#include "common/PppUtils.hpp"           // ar_ppp_utils->getWavelength
#include "filter/IlsCore.hpp"            // ils::IlsGateConfig / IlsResult / ilsFix
#include "filter/StateKeySchema.hpp"     // StateKeys::parseAmbiguityKey
#include "filter/StateManager.hpp"       // stations[i].sm-> ...

#include <Eigen/Dense>

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace GnssProcessor {

void networkProducerAr(NetworkRunContext& ctx,
                       const GnssConfig& config_,
                       const std::shared_ptr<spdlog::logger>& log,
                       const SyncedEpoch& se,
                       const NetworkEstimator& est,
                       const Eigen::VectorXd& x_s,
                       const std::vector<NetStation>& stations,
                       const std::vector<int>& est_index_of,
                       const std::vector<StationAssembly>& asm_res,
                       const NetworkSbasis::Plan& plan,
                       const std::unique_ptr<PppUtils>& ar_ppp_utils,
                       const std::filesystem::path& log_dir_) {
    // --- Alias preamble: byte-copies of the former Application::runNetwork aliases,
    //     so the relocated SB_NETAR body below reads the original names. ----------
    const auto& nc        = config_.getNetworkConfig();
    const auto& netar     = nc.ambiguity_resolution;
    auto&       n_emitted = ctx.counters.n_emitted;

    // --- Producer-side network AR (Phase 2: DD fixability attempt, LOG ONLY) ---
    // The between-station double-difference ambiguities are datum-INVARIANT
    // estimable quantities, so this fix attempt is independent of the phase-bias
    // datum in use.  Phase 2 is READ-ONLY (no re-pin / re-solve yet — that is
    // Phase 3): it reconstructs the per-station float ambiguities, forms an
    // independent DD basis (star topology per (const,signal)), builds the exact
    // DD covariance from est.jointConditionalCovariance, converts to cycles, and
    // runs ils::ilsFix to log how many integers the network COULD fix.  Gated on
    // network.ambiguity_resolution.enable + min_epochs; byte-identical when off.
    if (netar.enable && n_emitted >= static_cast<size_t>(std::max(0, netar.min_epochs))
        && x_s.size() > 0 && ar_ppp_utils) {
        struct ArRec { double value_m; std::size_t est_id; Eigen::Index col; };
        // (const,signal) -> sat -> station index -> absolute float ambiguity + its
        // (est id, A_r column) for the covariance request.  GPS+Galileo only.
        std::map<std::pair<std::string, std::string>,
                 std::map<std::string, std::map<std::size_t, ArRec>>> ar_pool;
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            Eigen::VectorXd x_r;
            try { x_r = est.reconstructStation(static_cast<std::size_t>(est_index_of[i]), x_s); }
            catch (...) { continue; }
            const auto& h2l = asm_res[i].h2local;
            const Eigen::VectorXd x_lin = stations[i].sm->getStateVector();
            for (const auto& [key, sidx] : stations[i].sm->getAmbiguities()) {
                const auto parts = StateKeys::parseAmbiguityKey(key);
                if (!parts) continue;
                const std::string cst = NetworkSbasis::constellationOf(parts->satellite);
                if (cst != "G" && cst != "E") continue;   // GPS+Gal integer-capable; GLONASS FDMA stays float
                if (stations[i].sm->isAmbiguityUnresolvable(key)) continue;
                const int amb_idx = static_cast<int>(sidx);
                if (amb_idx < 0 || amb_idx >= x_lin.size()) continue;
                const auto hit = h2l.find(amb_idx);
                if (hit == h2l.end() || hit->second < 0
                    || static_cast<Eigen::Index>(hit->second) >= x_r.size()) continue;
                const double value_m = x_lin(amb_idx) + x_r(hit->second);
                if (!std::isfinite(value_m)) continue;
                ar_pool[{cst, parts->signal}][parts->satellite][i] =
                    ArRec{value_m, static_cast<std::size_t>(est_index_of[i]),
                          static_cast<Eigen::Index>(hit->second)};
            }
        }
        // Independent DD basis: per (const,signal), a reference sat (tracked at the
        // most stations) and a reference station (tracking ref-sat + the most sats);
        // one DD per (other station r, other sat j) that both r and ref-station track.
        std::vector<std::pair<std::size_t, Eigen::Index>> reqs;
        std::map<std::pair<std::size_t, Eigen::Index>, int> req_idx;
        auto reqOf = [&](const ArRec& r) -> int {
            const std::pair<std::size_t, Eigen::Index> k{r.est_id, r.col};
            auto it = req_idx.find(k);
            if (it != req_idx.end()) return it->second;
            const int idx = static_cast<int>(reqs.size());
            reqs.push_back(k); req_idx[k] = idx; return idx;
        };
        struct DDrow { int i_rj, i_rp, i_fj, i_fp; double dd_m, lambda; };
        std::vector<DDrow> dds;
        int n_const_signal = 0;
        for (auto& [cs, satmap] : ar_pool) {
            std::string ref_sat; std::size_t best = 0;
            for (auto& [sat, sm] : satmap)
                if (sm.size() > best) { best = sm.size(); ref_sat = sat; }
            if (ref_sat.empty() || satmap[ref_sat].size() < 2) continue;
            std::size_t ref_i = 0, ref_cnt = 0; bool have_ref = false;
            for (auto& [sta_i, rec] : satmap[ref_sat]) {
                (void)rec;
                std::size_t cnt = 0;
                for (auto& [sat, sm] : satmap) if (sm.count(sta_i)) ++cnt;
                if (!have_ref || cnt > ref_cnt) { ref_cnt = cnt; ref_i = sta_i; have_ref = true; }
            }
            if (!have_ref) continue;
            const System sys = (cs.first == "G") ? System::GPS : System::Galileo;
            const double lambda = ar_ppp_utils->getWavelength(sys, cs.second, ref_sat);
            if (!(std::isfinite(lambda) && lambda > 0.0)) continue;
            const ArRec& fp = satmap[ref_sat].at(ref_i);   // ref station, ref sat
            ++n_const_signal;
            for (auto& [sat, sm] : satmap) {
                if (sat == ref_sat) continue;
                const auto fj_it = sm.find(ref_i);          // ref station, sat j
                if (fj_it == sm.end()) continue;
                for (const auto& [sta_i, rj] : sm) {
                    if (sta_i == ref_i) continue;
                    const auto rp_it = satmap[ref_sat].find(sta_i);   // station r, ref sat
                    if (rp_it == satmap[ref_sat].end()) continue;
                    const double dd_m = (rj.value_m - rp_it->second.value_m)
                                      - (fj_it->second.value_m - fp.value_m);
                    dds.push_back(DDrow{ reqOf(rj), reqOf(rp_it->second),
                                         reqOf(fj_it->second), reqOf(fp), dd_m, lambda });
                }
            }
        }
        if (!dds.empty() && reqs.size() >= 2) {
            Eigen::MatrixXd cov_undiff;
            try { cov_undiff = est.jointConditionalCovariance(reqs, plan.shared_datum); }
            catch (...) { cov_undiff = Eigen::MatrixXd(); }
            if (cov_undiff.rows() == static_cast<Eigen::Index>(reqs.size())
                && cov_undiff.rows() > 0) {
                const int nd = static_cast<int>(dds.size());
                Eigen::MatrixXd D = Eigen::MatrixXd::Zero(nd, static_cast<int>(reqs.size()));
                Eigen::VectorXd y_cyc(nd);
                std::vector<double> lam(nd);
                for (int a = 0; a < nd; ++a) {
                    const DDrow& d = dds[a];
                    D(a, d.i_rj) += 1.0; D(a, d.i_rp) -= 1.0;
                    D(a, d.i_fj) -= 1.0; D(a, d.i_fp) += 1.0;
                    y_cyc(a) = d.dd_m / d.lambda;
                    lam[a] = d.lambda;
                }
                Eigen::MatrixXd cov_dd_m = D * cov_undiff * D.transpose();
                Eigen::MatrixXd Q_cyc(nd, nd);
                for (int a = 0; a < nd; ++a)
                    for (int b = 0; b < nd; ++b)
                        Q_cyc(a, b) = cov_dd_m(a, b) / (lam[a] * lam[b]);
                Q_cyc = 0.5 * (Q_cyc + Q_cyc.transpose());
                // --- ILS dimension cap (network.ambiguity_resolution.ils_max_dim) ---
                // MLAMBDA's search does not return on the uncapped full-network basis
                // (~2500 DDs at 305 stations, Jul-15).  When armed (> 0), search only
                // the ils_max_dim lowest-formal-σ DDs — the standard partial-AR
                // subset (ascending Q_aa) — and leave the rest float.  The summary
                // statistics (med σ / med frac / near-int) stay FULL-basis so the
                // convergence gauge is unaffected by the cap.
                Eigen::VectorXd y_fix = y_cyc;
                Eigen::MatrixXd Q_fix = Q_cyc;
                int nd_fix = nd;
                if (netar.ils_max_dim > 0 && nd > netar.ils_max_dim) {
                    nd_fix = netar.ils_max_dim;
                    std::vector<int> order(static_cast<std::size_t>(nd));
                    std::iota(order.begin(), order.end(), 0);
                    std::sort(order.begin(), order.end(), [&](int a, int b) {
                        return Q_cyc(a, a) < Q_cyc(b, b);
                    });
                    y_fix.resize(nd_fix);
                    Q_fix.resize(nd_fix, nd_fix);
                    for (int a = 0; a < nd_fix; ++a) {
                        y_fix(a) = y_cyc(order[static_cast<std::size_t>(a)]);
                        for (int b = 0; b < nd_fix; ++b)
                            Q_fix(a, b) = Q_cyc(order[static_cast<std::size_t>(a)],
                                                order[static_cast<std::size_t>(b)]);
                    }
                }
                ils::IlsGateConfig gates;
                gates.min_success_rate     = netar.min_success_rate;
                gates.ratio_threshold      = netar.ratio_threshold;
                gates.ratio_upper_bound    = netar.ratio_upper_bound;
                gates.nl_max_per_pair_frac = netar.nl_max_per_pair_frac;
                gates.enable_partial_ar    = netar.enable_partial_ar;
                ils::IlsResult fix;
                try { fix = ils::ilsFix(y_fix, Q_fix, gates); }
                catch (...) { fix = ils::IlsResult(); }
                // Median DD formal σ (cycles) — the convergence gauge: as the
                // network float tightens toward cm this drops and the DDs become
                // fixable.  Also the worst float-fraction (evidence the products
                // are integer-consistent even before they are confidently fixable).
                std::vector<double> dsig; dsig.reserve(nd);
                std::vector<double> dfrac; dfrac.reserve(nd);
                int n_near = 0;   // DDs whose float is within 0.15 cyc of an integer
                for (int a = 0; a < nd; ++a) {
                    if (Q_cyc(a, a) > 0.0) dsig.push_back(std::sqrt(Q_cyc(a, a)));
                    const double f = std::abs(y_cyc(a) - std::round(y_cyc(a)));
                    dfrac.push_back(f);
                    if (f <= 0.15) ++n_near;
                }
                auto med = [](std::vector<double>& v) {
                    if (v.empty()) return 0.0;
                    std::sort(v.begin(), v.end());
                    return v[v.size() / 2];
                };
                const double med_sig  = med(dsig);
                const double med_frac = med(dfrac);
                log->info("Network AR (Phase 2, log-only): TOW {:.1f} — {} DD / {} undiff-amb / "
                          "{} (const,signal); fixed {}{} of {} searched{} (ratio {:.2f}, "
                          "p_succ {:.4f}, med_DD_sigma {:.4f} cyc, med_DD_frac {:.3f}, "
                          "near_int {}/{})",
                          se.time.tow, nd, static_cast<int>(reqs.size()), n_const_signal,
                          fix.ok ? static_cast<int>(fix.kept.size()) : 0,
                          fix.partial ? " partial" : "", nd_fix,
                          nd_fix < nd ? " (ILS-capped)" : "", fix.ratio, fix.p_succ,
                          med_sig, med_frac, n_near, nd);
            }
        }
    }

    // --- One-shot full-arrowhead NEQ export (network.neq_export_epoch) -----------
    // Post-solve READ-ONLY dump of the assembled normal equations for offline
    // spectral / weak-direction attribution: border N_ss (raw + reduced) + RHS,
    // every station's N_sr / N_rr (rebuilt from its LDLᵀ) / u_r as row-major
    // little-endian double .bin, plus shared-column and per-station local-column
    // labels.  Solution is untouched; fires on exactly one epoch per run.
    if (nc.neq_export_epoch > 0
        && n_emitted + 1 == static_cast<size_t>(nc.neq_export_epoch)
        && x_s.size() > 0) {
        const std::filesystem::path dir = log_dir_ / "neq_export";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto appendBin = [](std::ofstream& f, const Eigen::MatrixXd& M) {
            // Explicit row-major copy so the offline reader needs no
            // storage-order convention (Eigen's default is column-major).
            const Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic,
                                Eigen::RowMajor> R = M;
            f.write(reinterpret_cast<const char*>(R.data()),
                    static_cast<std::streamsize>(sizeof(double) * R.size()));
        };
        auto sanitize = [](std::string s) {
            for (char& ch : s) if (ch == ',') ch = ';';
            return s;
        };
        const auto& kernel = est.normals();
        {
            std::ofstream f(dir / "Nss_raw.bin", std::ios::binary | std::ios::trunc);
            appendBin(f, kernel.sharedNormals());
        }
        {
            std::ofstream f(dir / "us_raw.bin", std::ios::binary | std::ios::trunc);
            appendBin(f, kernel.sharedRhs());
        }
        {
            std::ofstream f(dir / "Nss_red.bin", std::ios::binary | std::ios::trunc);
            appendBin(f, kernel.reducedSharedNormals());
        }
        {
            std::ofstream f(dir / "us_red.bin", std::ios::binary | std::ios::trunc);
            appendBin(f, kernel.reducedSharedRhs());
        }
        {
            std::ofstream f(dir / "shared_labels.csv", std::ios::trunc);
            f << "col,label\n";
            for (Eigen::Index c = 0; c < est.numShared(); ++c)
                f << c << ',' << sanitize(est.sharedColumnLabel(c)) << '\n';
        }
        std::ofstream sidx(dir / "stations_index.csv", std::ios::trunc);
        sidx << "est_slot,name,n_local\n";
        std::ofstream slab(dir / "stations_labels.csv", std::ios::trunc);
        slab << "est_slot,local_col,label\n";
        std::ofstream nsr(dir / "stations_Nsr.bin", std::ios::binary | std::ios::trunc);
        std::ofstream nrr(dir / "stations_Nrr.bin", std::ios::binary | std::ios::trunc);
        std::ofstream urf(dir / "stations_ur.bin", std::ios::binary | std::ios::trunc);
        for (size_t i = 0; i < stations.size(); ++i) {
            if (est_index_of[i] < 0) continue;
            const auto r = static_cast<std::size_t>(est_index_of[i]);
            const Eigen::Index nl = kernel.numLocal(r);
            sidx << r << ',' << stations[i].name << ',' << nl << '\n';
            appendBin(nsr, kernel.stationCrossBlock(r));
            appendBin(nrr, kernel.stationLocalNormals(r));
            appendBin(urf, kernel.stationLocalRhs(r));
            // Local col → StateManager label via the assembly's h2local inverse;
            // columns with no SM mapping (none expected) stay "?".
            std::vector<std::string> names(static_cast<std::size_t>(nl), "?");
            const auto sm_labels = stations[i].sm->getStateLabels();
            for (const auto& kv : asm_res[i].h2local) {
                const auto lc = static_cast<Eigen::Index>(kv.second);
                if (lc >= 0 && lc < nl
                    && static_cast<std::size_t>(kv.first) < sm_labels.size())
                    names[static_cast<std::size_t>(lc)] =
                        sm_labels[static_cast<std::size_t>(kv.first)];
            }
            for (Eigen::Index c = 0; c < nl; ++c)
                slab << r << ',' << c << ','
                     << sanitize(names[static_cast<std::size_t>(c)]) << '\n';
        }
        {
            std::ofstream meta(dir / "meta.csv", std::ios::trunc);
            meta << "week,tow,epoch,n_shared,n_stations\n"
                 << se.time.week << ',' << se.time.tow << ','
                 << nc.neq_export_epoch << ',' << est.numShared() << ','
                 << kernel.numStations() << '\n';
        }
        log->info("Network: NEQ export written to {} (epoch {}, n_shared {}, "
                  "{} station blocks)",
                  dir.string(), nc.neq_export_epoch, est.numShared(),
                  kernel.numStations());
    }
}

} // namespace GnssProcessor

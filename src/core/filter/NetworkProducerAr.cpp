/**
 * @file NetworkProducerAr.cpp
 * @brief Implementation of @ref networkProducerAr: the log-only producer-side
 *        network-AR (Phase 2) diagnostic and the one-shot NEQ export, relocated
 *        out of Application::runNetwork at decomposition INC-9b.
 *
 * The body started as a copy of the former SB_NETAR block, dedented 4 spaces.  The
 * alias preamble reconstructs the three run-TU aliases the body reads (nc / netar /
 * n_emitted); every other name is a parameter named as the former run-TU local.  See
 * NetworkProducerAr.hpp.
 *
 * Three additive departures from the relocated text, all outside the arithmetic (the
 * trace card; docs/network_ambiguity_resolution_
 * design.md, 2026-09-10 note §4.1, "a fix count is not a grade"):
 *   1. `DDrow` carries the six identity strings a truth join needs;
 *   2. the cap's `order` comes from @ref netar::lowestSigmaOrder so the trace can
 *      invert it (@ref netar::mapFixToBasis); the sort, comparator and subset are
 *      the same, so the search is unchanged;
 *   3. run-end tallies and the per-DD `network_producer_ar.csv` writer.
 * The solve is untouched, and the block is skipped entirely when
 * `network.ambiguity_resolution.enable` is off, so with it off the products are
 * byte-identical.
 */

#include "filter/NetworkProducerAr.hpp"

#include "app/NetworkRunContext.hpp"     // ctx.counters.n_emitted
#include "config/GnssConfig.hpp"         // config_.getNetworkConfig() / .ambiguity_resolution
#include "common/GnssIdentity.hpp"       // systemOfSat, systemFromName, constellationKey, SignalId
#include "common/GnssTypes.hpp"          // System
#include "filter/IlsCore.hpp"            // ils::IlsGateConfig / IlsResult / ilsFix
#include "filter/StateKeySchema.hpp"     // StateKeys::parseAmbiguityKey
#include "filter/StateManager.hpp"       // stations[i].sm-> ...
#include "logging/NetworkRunTraces.hpp"  // traces.producer_ar_trace

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
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// The run-end reports at the end of this file (card NOM step TD1b):
#include "filter/NetworkRunEnd.hpp"
#include <limits>
#include <memory>

namespace GnssProcessor {

namespace netar {

std::vector<int> lowestSigmaOrder(const Eigen::MatrixXd& Q_cyc, int cap) {
    const int n = static_cast<int>(std::min(Q_cyc.rows(), Q_cyc.cols()));
    std::vector<int> order(static_cast<std::size_t>(std::max(0, n)));
    std::iota(order.begin(), order.end(), 0);
    if (cap <= 0 || cap >= n) return order;   // uncapped: identity, un-sorted
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return Q_cyc(a, a) < Q_cyc(b, b);
    });
    order.resize(static_cast<std::size_t>(cap));
    return order;
}

std::vector<DdFixLabel> mapFixToBasis(int n_dd,
                                      const std::vector<int>& search_of,
                                      const std::vector<int>& kept,
                                      const Eigen::VectorXd& afix) {
    std::vector<DdFixLabel> out(static_cast<std::size_t>(std::max(0, n_dd)));
    for (int s : search_of)
        if (s >= 0 && s < n_dd) out[static_cast<std::size_t>(s)].in_subset = true;
    // `kept[m]` indexes the search vector; `afix(m)` is that entry's integer.  The
    // integer goes to basis entry search_of[kept[m]].  Using kept[m] directly as a
    // basis index would attribute it to whichever DD sits at that ordinal in the
    // basis, which on a capped, sigma-sorted search is an unrelated satellite.
    const int n_af = static_cast<int>(afix.size());
    for (std::size_t m = 0; m < kept.size() && static_cast<int>(m) < n_af; ++m) {
        const int s = kept[m];
        if (s < 0 || static_cast<std::size_t>(s) >= search_of.size()) continue;
        const int b = search_of[static_cast<std::size_t>(s)];
        if (b < 0 || b >= n_dd) continue;
        auto& lab = out[static_cast<std::size_t>(b)];
        lab.kept      = true;
        lab.fixed_int = afix(static_cast<Eigen::Index>(m));
    }
    return out;
}

} // namespace netar

void networkProducerAr(NetworkRunContext& ctx,
                       const GnssConfig& config_,
                       const std::shared_ptr<spdlog::logger>& log,
                       NetworkRunTraces& traces,
                       const SyncedEpoch& se,
                       const NetworkEstimator& est,
                       const Eigen::VectorXd& x_s,
                       const std::vector<NetStation>& stations,
                       const std::vector<int>& est_index_of,
                       const std::vector<StationAssembly>& asm_res,
                       const NetworkSbasis::Plan& plan,
                       const std::filesystem::path& log_dir_) {
    // --- Alias preamble: byte-copies of the former Application::runNetwork aliases,
    //     so the relocated SB_NETAR body below reads the original names. ----------
    const auto& nc        = config_.getNetworkConfig();
    const auto& netar     = nc.ambiguity_resolution;
    auto&       n_emitted = ctx.counters.n_emitted;

    // --- Producer-side network AR (Phase 2: DD fixability attempt, log only) ---
    // The between-station double-difference ambiguities are datum-invariant
    // estimable quantities, so this fix attempt is independent of the phase-bias
    // datum in use.  Phase 2 is read-only (re-pin and re-solve belong to Phase 3):
    // it reconstructs the per-station float ambiguities, forms an independent DD
    // basis (star topology per (const,signal)), builds the exact DD covariance
    // from est.jointConditionalCovariance, converts to cycles, and runs
    // ils::ilsFix to log how many integers the network could fix.  Gated on
    // network.ambiguity_resolution.enable and min_epochs; byte-identical when off.
    if (netar.enable && n_emitted >= static_cast<size_t>(std::max(0, netar.min_epochs))
        && x_s.size() > 0) {
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
                const System sys = systemOfSat(parts->satellite);
                if (sys != System::GPS && sys != System::Galileo)
                    continue;   // GPS+Gal integer-capable; GLONASS FDMA stays float
                const std::string cst = constellationKey(sys);
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
        // The identity fields (cons, signal, sat_j, ref_sat, sta_r, ref_sta) let a
        // traced DD be joined to truth_ambiguities.csv and the star reference be
        // differenced away (SPEC_amb_iono_split_overcount §6.12); without them the
        // trace gives only a fix count.  They are filled unconditionally: the block is
        // already gated on `netar.enable`, and six short strings per DD cost little
        // next to the joint conditional covariance below.
        struct DDrow {
            int i_rj, i_rp, i_fj, i_fp; double dd_m, lambda;
            std::string cons, signal, sat_j, ref_sat, sta_r, ref_sta;
        };
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
            const System sys = systemFromName(cs.first);   // "G" or "E"
            const std::optional<SignalId> amb_sig = SignalId::fromObsCode(sys, cs.second);
            const double lambda = amb_sig ? amb_sig->wavelengthM(0) : 0.0;   // CDMA: no FDMA slot
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
                                         reqOf(fj_it->second), reqOf(fp), dd_m, lambda,
                                         cs.first, cs.second, sat, ref_sat,
                                         stations[sta_i].name, stations[ref_i].name });
                }
            }
        }
        if (!dds.empty() && reqs.size() >= 2) {
            const bool ar_carry_aware = ctx.knobs.carry_aware_covariance;
            Eigen::MatrixXd cov_undiff;
            // Carry-aware readback (docs/SPEC_carry_aware_covariance.md §6).  With the
            // knob off (default), Q_cyc, and with it FFR, ADOP and the partial-AR
            // ordering below, is built from a covariance the solve does not use; that
            // expression is kept unchanged.  With it on, the probe uses the
            // estimator's own covariance.
            try { cov_undiff = est.jointConditionalCovariance(reqs, plan.shared_datum,
                                                              ar_carry_aware); }
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
                // (about 2500 DDs at 305 stations).  When armed (> 0), only the
                // ils_max_dim lowest-formal-σ DDs are searched (the standard partial-AR
                // subset, ascending Q_aa) and the rest stay float.  The summary
                // statistics (med σ / med frac / near-int) use the full basis, so the
                // convergence gauge is unaffected by the cap.
                Eigen::VectorXd y_fix = y_cyc;
                Eigen::MatrixXd Q_fix = Q_cyc;
                int nd_fix = nd;
                // search row -> basis index.  Computed outside the cap branch because
                // the trace has to invert it (see netar::mapFixToBasis).
                const std::vector<int> order = netar::lowestSigmaOrder(Q_cyc, netar.ils_max_dim);
                if (netar.ils_max_dim > 0 && nd > netar.ils_max_dim) {
                    nd_fix = netar.ils_max_dim;
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
                // Median DD formal σ (cycles) is the convergence gauge: as the
                // network float tightens toward cm it drops and the DDs become
                // fixable.  The median float fraction and the near-integer count
                // show whether the products are integer-consistent before they are
                // confidently fixable.
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
                // The covariance mode is logged on the same line because med_DD_sigma
                // and every ratio/p_succ derived from the covariance mean different
                // things under the two forms (docs/SPEC_carry_aware_covariance.md §4).
                log->info("Network AR (Phase 2, log-only): TOW {:.1f} — {} DD / {} undiff-amb / "
                          "{} (const,signal); fixed {}{} of {} searched{} (ratio {:.2f}, "
                          "p_succ {:.4f}, med_DD_sigma {:.4f} cyc, med_DD_frac {:.3f}, "
                          "near_int {}/{}), cov {}",
                          se.time.tow, nd, static_cast<int>(reqs.size()), n_const_signal,
                          fix.ok ? static_cast<int>(fix.kept.size()) : 0,
                          fix.partial ? " partial" : "", nd_fix,
                          nd_fix < nd ? " (ILS-capped)" : "", fix.ratio, fix.p_succ,
                          med_sig, med_frac, n_near, nd,
                          ar_carry_aware ? "CARRY-AWARE" : "carry-blind");
                // --- Run-end tallies (invariant #4): the log line above is per-epoch;
                //     the tallies give the run a denominator, so an armed but inert
                //     search is distinguishable from a quiet one. ---------------------
                auto& c = ctx.counters;
                ++c.netar_epochs;
                c.netar_carry_aware = ar_carry_aware;
                c.netar_n_dd.push_back(static_cast<double>(nd));
                c.netar_n_search.push_back(static_cast<double>(nd_fix));
                c.netar_med_sigma.push_back(med_sig);
                const int n_fixed = fix.ok ? static_cast<int>(fix.kept.size()) : 0;
                if (fix.ok) {
                    ++c.netar_fix_epochs;
                    c.netar_dd_fixed += n_fixed;
                    // ratio / p_succ describe an accepted fix; on a rejected epoch they
                    // are failure diagnostics, and one median over both populations
                    // cannot be interpreted.  Only accepted epochs are tallied.
                    c.netar_ratio.push_back(fix.ratio);
                    c.netar_p_succ.push_back(fix.p_succ);
                }
                // --- Per-DD trace (network.ambiguity_resolution.trace) --------------
                // Every DD in the basis gets a row, not only the searched subset: the
                // fixability ceiling the grader reports (share of DDs whose float is
                // within 0.25 cyc of truth) refers to the population the cap chose
                // from, which a subset-only trace cannot provide.
                if (traces.producer_ar_trace.is_open()) {
                    const auto labels = netar::mapFixToBasis(nd, order, fix.kept, fix.afix);
                    auto& os = traces.producer_ar_trace;
                    // ratio / p_succ: the accepted fix's values on a fixed epoch, the
                    // full-set attempt's otherwise.  IlsResult zeroes the accepted fields
                    // when nothing was accepted, so on most epochs they would be a column
                    // of zeros; the full-set values are the failure diagnostics.
                    // `n_fixed` on the same row separates the two populations (the
                    // grader splits on it).
                    const double ep_ratio  = fix.ok ? fix.ratio  : fix.full_ratio;
                    const double ep_psucc  = fix.ok ? fix.p_succ : fix.full_p_succ;
                    for (int a = 0; a < nd; ++a) {
                        const DDrow& d = dds[static_cast<std::size_t>(a)];
                        const auto& lb = labels[static_cast<std::size_t>(a)];
                        const double sig = Q_cyc(a, a) > 0.0 ? std::sqrt(Q_cyc(a, a)) : -1.0;
                        os << fmt::format("{},{:.1f},{},{},{},{},{},{},{:.9f},{:.6f},"
                                          "{:.6f},{},{},{},{:.4f},{:.6f},{},{},{}\n",
                                          se.time.week, se.time.tow,
                                          d.cons, d.signal, d.sat_j, d.ref_sat,
                                          d.sta_r, d.ref_sta, d.lambda,
                                          y_cyc(a), sig,
                                          lb.in_subset ? 1 : 0,
                                          lb.kept ? static_cast<long long>(std::llround(lb.fixed_int)) : 0LL,
                                          lb.kept ? 1 : 0,
                                          ep_ratio, ep_psucc, nd, nd_fix, n_fixed);
                    }
                    c.netar_rows += nd;
                }
            }
        }
    }

    // --- One-shot full-arrowhead NEQ export (network.neq_export_epoch) -----------
    // Post-solve, read-only dump of the assembled normal equations for offline
    // spectral / weak-direction attribution: border N_ss (raw and reduced) with
    // RHS, every station's N_sr / N_rr (rebuilt from its LDLᵀ) / u_r as row-major
    // little-endian double .bin, plus shared-column and per-station local-column
    // labels.  The solution is untouched; fires on the epoch where
    // n_emitted + 1 == neq_export_epoch, once per run.
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

// ---- Run-end reports (card NOM step TD1b; generated by scripts/nom_td_cut.py) ----

// Run-end report: the producer ambiguity-resolution search.
int networkProducerArRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    NetworkRunTraces& traces = v.traces;
    const NetworkConfig& nc = v.config.getNetworkConfig();

    // --- Producer-AR Phase-2 diagnostic run-end readout ------------------------------
    // Run-level summary of the producer AR search.  The per-epoch spdlog line describes
    // one epoch; this readout describes the run, following §4.1 of the 2026-09-10 note in
    // docs/network_ambiguity_resolution_design.md ("a fix count is not a grade").
    // `netar_epochs == 0` on an armed run means the search never reached the ILS and
    // every number below is structurally empty; that case gets its own warning
    // (invariant #4).
    if (!nc.ambiguity_resolution.enable) return 0;
    auto& c = ctx.counters;
    auto p50 = [](std::vector<double> v) {
        if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    // ratio and p_succ are collected on accepted-fix epochs only (a rejected epoch's
    // IlsResult zeroes them), so on a run that never fixed the sample is empty.
    // "n/a" says so; a printed "nan" or "0.00" would read like a measured value.
    auto p50s = [&](const std::vector<double>& v, int prec) {
        return v.empty() ? std::string("n/a")
                         : fmt::format("{:.{}f}", p50(v), prec);
    };
    if (c.netar_epochs > 0) {
        const double fix_pct = 100.0 * static_cast<double>(c.netar_fix_epochs)
                                      / static_cast<double>(c.netar_epochs);
        log->info("Network (producer AR, Phase 2 log-only): {} epochs searched, "
                  "{:.0f} DD p50 / {:.0f} searched p50, fixed epochs {} ({:.1f}%), "
                  "DD fixed total {}, ratio p50 {}, p_succ p50 {}, "
                  "med_DD_sigma p50 {:.4f} cyc; carry-{}; trace {}",
                  c.netar_epochs, p50(c.netar_n_dd), p50(c.netar_n_search),
                  c.netar_fix_epochs, fix_pct, c.netar_dd_fixed,
                  p50s(c.netar_ratio, 2), p50s(c.netar_p_succ, 4), p50(c.netar_med_sigma),
                  c.netar_carry_aware ? "aware" : "blind",
                  nc.ambiguity_resolution.trace ? "on" : "off");
        if (nc.ambiguity_resolution.trace) {
            if (c.netar_rows > 0)
                log->info("Network (producer AR, Phase 2): {} per-DD trace row(s) "
                          "written to {} — grade with "
                          "scripts/simnet_producer_ar_grade.py (a fix count is not a "
                          "grade; the DD basis is a STAR against one reference "
                          "satellite and one reference station, so report or "
                          "difference away that pair's share).",
                          c.netar_rows, traces.producer_ar_trace_path.string());
            else
                log->warn("Network (producer AR, Phase 2): the trace is ARMED and the "
                          "search ran {} epoch(s), yet ZERO rows were written — the "
                          "stream never opened (check the warning above) and this run "
                          "carries NO gradeable AR evidence.", c.netar_epochs);
        }
        if (c.netar_fix_epochs > 0 && !c.netar_carry_aware)
            log->warn("Network (producer AR, Phase 2): {} epoch(s) accepted a fix on a "
                      "CARRY-BLIND covariance (network.carry_aware_covariance is OFF), "
                      "so every gate the fix passed — FFR, ratio, the per-pair frac cap "
                      "— was computed from a covariance the SOLVE never used "
                      "(docs/SPEC_carry_aware_covariance.md §4).  A WRONG fix at "
                      "p_succ >= min_success_rate falsifies that covariance as a gate "
                      "input; grade the integers against truth before reading the fix "
                      "rate as a success.", c.netar_fix_epochs);
    } else {
        log->warn("Network (producer AR, Phase 2 log-only): ARMED but the ILS search "
                  "ran on ZERO epochs — min_epochs={} never cleared, the joint "
                  "conditional covariance never returned, or the DD basis never had "
                  "two undifferenced columns.  Every producer-AR number for this run "
                  "is structurally empty (standing invariant #4); this run is evidence "
                  "about NOTHING in the card.", nc.ambiguity_resolution.min_epochs);
    }
    return 0;
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor

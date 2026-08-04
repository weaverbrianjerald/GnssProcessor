/**
 * @file NetworkRhsAttrib.cpp
 * @brief In-assembly reduced-RHS class attribution (see NetworkRhsAttrib.hpp).
 *
 * Exactness argument, per station r and row class c: the kernel's reduced RHS is
 *   ũ_s = u_s − Σ_r N_sr N_rr⁻¹ u_r  (+ DD-fold corrections),
 * and every u vector is a SUM over folded rows/priors.  With the epoch's FINAL
 * N_sr / N_rr (all classes' information present), the map u ↦ ũ_s is linear, so
 * splitting u by class and pushing each part through the same reduction is an
 * exact decomposition:  Σ_c ũ_s^c ≡ ũ_s.  The DD folds are recomputed per input
 * block with the identical algebra the kernel uses (G·M·(z_free − ẑ)); the
 * datum pins act on the solve's COPY of the reduced system and are therefore
 * reported as a separate section, excluded from the kernel-level closure.
 *
 * Known caveat (self-detecting): a within-epoch RAIM replay rebuilds the kernel
 * from mutated row stacks after the shared-info captures ran; the closure row
 * then reports the mismatch instead of the dump silently lying.  On the FT63
 * stack that tier is carry_warm-gated (inert with leveling OFF).
 *
 * Structure (SPEC_prefit_census_observability INC-0 / INC-5):
 *   - the row classifier + shared-column label pass are HOISTED to
 *     filter/NetworkRowClassify — one classifier shared with the census, the
 *     fuse and the topology ledger;
 *   - the decomposition itself runs off a @ref NetworkAttribFrame, so the SAME
 *     code path serves the live epoch and a RETAINED previous epoch.  The frame
 *     is a pure re-binding of what the body always read — the live path stores
 *     POINTERS to the caller's assemblies, so it copies only the kernel blocks
 *     it would have read by reference anyway.
 */

#include "filter/NetworkRhsAttrib.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>

#include <fmt/format.h>

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkNormalEquations.hpp"
#include "filter/NetworkRowClassify.hpp"
#include "logging/NetworkRunTraces.hpp"

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

/// Family bucket of one shared column, for the dumped per-family norms.
/// (@ref NetColFam is the hoisted classifier's enum; the dump's column ORDER is
/// sh/pb/cb/clk and is part of the CSV contract.)
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
    b += mat(N_red) + vec(b_red);
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
                               bool own_assemblies) {
    out.clear();
    out.week = se.time.week;
    out.tow  = se.time.tow;

    const auto& kernel = est.normals();
    const Eigen::Index n = est.numShared();
    if (n <= 0 || !kernel.isReduced()) return;
    out.n = n;

    const SharedColumnMap cm = buildSharedColumnMap(est, n);
    out.label  = cm.label;
    out.c00_l1 = cm.c00_l1;
    out.c00_l2 = cm.c00_l2;

    // ADMISSION PREDICATE (appears twice — keep the two copies textually
    // identical): folded into the kernel this epoch, block built, dimensions
    // agree.  The second pass fills asm_ptr and must select exactly the same
    // stations in the same order, or a station would be paired with another
    // station's kernel blocks.  It is written twice rather than hoisted because
    // asm_own reallocates while the first pass fills it, so the pointers cannot
    // be taken until it is complete.
    for (std::size_t i = 0; i < stations.size() && i < asm_res.size(); ++i) {
        if (i >= est_index_of.size() || est_index_of[i] < 0) continue;
        const StationAssembly& R = asm_res[i];
        if (!R.ok || R.A_s.cols() != n || R.y.size() == 0) continue;
        const std::size_t slot = static_cast<std::size_t>(est_index_of[i]);
        out.sta_name.push_back(stations[i].name);
        out.slot.push_back(slot);
        out.Nsr.push_back(kernel.stationCrossBlock(slot));
        out.Nrr.push_back(kernel.stationLocalNormals(slot));
        out.ur.push_back(kernel.stationLocalRhs(slot));
        if (own_assemblies) out.asm_own.push_back(R);
    }
    // Pointers LAST: asm_own may reallocate while it is being filled.
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
    out.valid     = true;
}

void networkRhsAttribDump(const NetworkAttribFrame& f,
                          const std::shared_ptr<spdlog::logger>& log,
                          const std::filesystem::path& dir, const char* kind) {
    if (!f.valid || f.n <= 0) {
        log->warn("Network (rhs-attrib): {} frame at tow {:.1f} has no reduced "
                  "system — skipped", kind, f.tow);
        return;
    }
    const Eigen::Index n = f.n;
    const Eigen::Index c00_l1 = f.c00_l1, c00_l2 = f.c00_l2;

    // Per-column family + satellite, re-derived from the frame's OWN labels
    // (the classifier's parse — one truth, INC-0).
    SharedColumnMap cm;
    cm.n = n;
    cm.label = f.label;
    cm.fam.assign(static_cast<std::size_t>(n), NetColFam::Other);
    cm.col_sat.assign(static_cast<std::size_t>(n), {});
    cm.c00_l1 = c00_l1;
    cm.c00_l2 = c00_l2;
    for (Eigen::Index c = 0; c < n; ++c) {
        const std::size_t u = static_cast<std::size_t>(c);
        const std::string& L = cm.label[u];
        if      (L.rfind("SH", 0) == 0)  cm.fam[u] = NetColFam::Sh;
        else if (L.rfind("PB:", 0) == 0) cm.fam[u] = NetColFam::Pb;
        else if (L.rfind("CB:", 0) == 0) cm.fam[u] = NetColFam::Cb;
        else if (L.rfind("CLK:", 0) == 0) cm.fam[u] = NetColFam::Clk;
        if (cm.fam[u] == NetColFam::Pb || cm.fam[u] == NetColFam::Cb
            || cm.fam[u] == NetColFam::Clk) {
            const std::size_t p1 = L.find(':');
            const std::size_t p2 = L.find(':', p1 + 1);
            cm.col_sat[u] = L.substr(p1 + 1, (p2 == std::string::npos)
                                                 ? std::string::npos : p2 - p1 - 1);
        }
    }

    // ---- accumulators ------------------------------------------------------
    std::map<std::string, Eigen::VectorXd> cls_total;   // class -> reduced-RHS vector
    auto addTotal = [&](const std::string& k, const Eigen::VectorXd& v) {
        auto it = cls_total.find(k);
        if (it == cls_total.end()) cls_total.emplace(k, v);
        else it->second += v;
    };
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

    // Per-slot cached local factorizations (reused by the DD recompute below).
    std::map<std::size_t, Eigen::LDLT<Eigen::MatrixXd>> slot_ldlt;
    std::map<std::size_t, Eigen::VectorXd> slot_nrr_inv_ur;
    std::map<std::size_t, std::size_t> slot_item;   // kernel slot -> frame item
    // Per-slot local-col -> "sat:signal" (from pl_amb), for DD-block labels.
    std::map<std::size_t, std::map<Eigen::Index, std::string>> slot_amb_label;

    // ---- pass 1: station data rows + local carry priors --------------------
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

        // Class accumulators (u_s part, u_r part) for THIS station.
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
            auto& a = acc(rowClassName(classifyRow(cm, R, row, nullptr)));
            a.first.noalias()  += R.A_s.row(row).transpose() * wy;
            a.second.noalias() += R.A_r.row(row).transpose() * wy;
        }
        // Local carry priors (addStationPrior): RHS increments on local columns,
        // sub-classed by which harvest list owns the column.
        if (!R.carry_cols.empty()) {
            std::set<Eigen::Index> cb_cols, rpb_cols, amb_cols;
            for (const auto& t : R.pl_cb)  cb_cols.insert(std::get<1>(t));
            for (const auto& t : R.pl_rpb) rpb_cols.insert(std::get<1>(t));
            for (const auto& pa : R.pl_amb) amb_cols.insert(pa.col);
            for (std::size_t j = 0; j < R.carry_cols.size()
                                    && j < static_cast<std::size_t>(R.carry_dur.size()); ++j) {
                const Eigen::Index col = R.carry_cols[j];
                const char* cls = "prior_local_other";
                if      (cb_cols.count(col))                          cls = "prior_rxcb";
                else if (rpb_cols.count(col))                         cls = "prior_rpb";
                else if (R.pl_zwd_present && col == R.pl_zwd.col)     cls = "prior_zwd";
                else if (amb_cols.count(col))                         cls = "prior_plamb";
                if (col >= 0 && col < R.A_r.cols())
                    acc(cls).second(col) += R.carry_dur(static_cast<Eigen::Index>(j));
            }
        }
        // Reduce each class part through the station's final blocks.
        for (auto& [cls, uu] : per) {
            Eigen::VectorXd contrib = uu.first;
            if (uu.second.size() > 0)
                contrib.noalias() -= Nsr * ldlt.solve(uu.second);
            addTotal(cls, contrib);
            pushRow(cls, f.sta_name[k], "", contrib);
        }
    }

    // ---- pass 2: shared-info captures (D2 carries, Kaula, W6, datum rows) ---
    for (const auto& cap : f.captures) {
        if (cap.delta_rhs.size() != n) continue;
        const std::string cls = std::string("shared:") + cap.tag;
        addTotal(cls, cap.delta_rhs);
    }

    // ---- pass 3: DD-fold corrections, recomputed per input block ------------
    const auto& blocks = f.dd_blocks;
    Eigen::VectorXd dd_total = Eigen::VectorXd::Zero(n);
    std::vector<std::tuple<std::size_t, std::string, Eigen::VectorXd>> dd_rows;
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
        dd_rows.emplace_back(b, blk_label, delta);
    }
    if (dd_total.size() == n && !blocks.empty()) addTotal("dd_fold", dd_total);

    // ---- closure (kernel-level; datum pins excluded by construction) --------
    Eigen::VectorXd sum = Eigen::VectorXd::Zero(n);
    for (const auto& [cls, v] : cls_total) sum += v;
    const Eigen::VectorXd& b_red = f.b_red;
    const double closure_abs = (sum - b_red).cwiseAbs().maxCoeff();
    const double b_norm = std::max(1.0, b_red.norm());
    const double closure_rel = (sum - b_red).norm() / b_norm;
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
        auto& ldlt = slot_ldlt[f.slot[k]];
        std::map<std::string, std::pair<Eigen::VectorXd, Eigen::VectorXd>> per;
        for (Eigen::Index row = 0; row < R.y.size(); ++row) {
            std::string sat;
            const NetRowClass rc = classifyRow(cm, R, row, &sat);
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
    // (w, y, h_c00 separately, so a weight flip and an innovation flip are
    // distinguishable at a glance — the class/station sums cannot separate
    // them.)  ~3–4k rows per armed epoch; diagnostics-only.
    std::vector<std::string> row_dump;
    for (std::size_t k = 0; k < f.asm_ptr.size(); ++k) {
        const StationAssembly& R = *f.asm_ptr[k];
        const std::string& sname = f.sta_name[k];
        for (Eigen::Index row = 0; row < R.y.size(); ++row) {
            std::string sat;
            const char* cls = rowClassName(classifyRow(cm, R, row, &sat));
            const double w = (row < R.w.size()) ? R.w(row) : 1.0;
            const double h1 = (c00_l1 >= 0) ? R.A_s(row, c00_l1) : 0.0;
            const double h2 = (c00_l2 >= 0) ? R.A_s(row, c00_l2) : 0.0;
            row_dump.push_back(fmt::format(
                "{},{},{},{},{:.6e},{:.6e},{:.6e},{:.6e},{:.6e}", sname, row,
                cls, sat.empty() ? "?" : sat, w, R.y(row), h1, h2,
                w * R.y(row) * h1));
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
              << fmt::format("{:.4f}", famNorm(f, cm.fam, v, NetColFam::Sh)) << ','
              << fmt::format("{:.4f}", famNorm(f, cm.fam, v, NetColFam::Pb)) << ','
              << fmt::format("{:.4f}", famNorm(f, cm.fam, v, NetColFam::Cb)) << ','
              << fmt::format("{:.4f}", famNorm(f, cm.fam, v, NetColFam::Clk)) << ','
              << fmt::format("{:.4f}", v.norm()) << ',' << extra << '\n';
        };
        for (const auto& [cls, v] : cls_total) emit("TOTAL", cls, "", "", v, "");
        for (const auto& r : rows_out) {
            // Station/sat rows re-emit only scalars; rebuild a sparse two-entry
            // vector for the shared emit path (norms already captured in r.norm).
            o << f.week << ',' << fmt::format("{:.3f}", f.tow) << ','
              << (r.sat.empty() ? "STA" : "STASAT") << ',' << r.cls << ','
              << r.sta << ',' << r.sat << ','
              << fmt::format("{:.6f}", r.c00_l1) << ','
              << fmt::format("{:.6f}", r.c00_l2) << ",,,,,"
              << fmt::format("{:.4f}", r.norm) << ",\n";
        }
        for (const auto& [b, lbl, v] : dd_rows)
            emit("DDBLOCK", "dd_fold", "", "", v,
                 "block=" + std::to_string(b) + ";" + lbl);
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
        o << "station,row,class,sat,w,y,h_c00_l1,h_c00_l2,w_y_h1\n";
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
              kind, f.tow, cls_total.size(), rows_out.size(), dd_rows.size(),
              pin_rows.size(), (dir / ("attrib_" + tow_tag + ".csv")).string(),
              closure_rel, c00_l1, c00_l2);
}

void networkRhsAttribution(NetworkRunContext& ctx, const GnssConfig& config_,
                           const std::shared_ptr<spdlog::logger>& log,
                           const NetworkRunTraces& traces, const SyncedEpoch& se,
                           const std::vector<NetStation>& stations,
                           NetworkEstimator& est,
                           const NetworkEstimator::Datum& plan_datum,
                           const std::vector<StationAssembly>& asm_res,
                           const std::vector<int>& est_index_of) {
    const auto& nc = config_.getNetworkConfig();
    if (nc.rhs_attrib_tow < 0.0) return;
    if (ctx.carry.rhs_attrib_done >= std::max(1, nc.rhs_attrib_n_ep)) return;
    if (se.time.tow < nc.rhs_attrib_tow - 0.25) return;
    ++ctx.carry.rhs_attrib_done;

    NetworkAttribFrame f;
    networkAttribCaptureFrame(f, se, stations, est, plan_datum, asm_res,
                              est_index_of, /*own_assemblies=*/false);
    networkRhsAttribDump(f, log, traces.log_dir_ / "rhs_attrib", "armed");
    ++ctx.counters.pc.rec_dumps;
}

bool networkRhsTripEdge(NetworkRunContext& ctx, const char* kind, bool active) {
    if (!ctx.knobs.rec_on) return false;
    ++ctx.counters.pc.rec_consults;
    auto& eras = ctx.carry.rhs_trip_eras;
    const std::string key(kind ? kind : "?");
    const bool was = eras.count(key) != 0;
    if (active && !was) {
        eras.insert(key);
        ++ctx.counters.pc.rec_trips;
        // First writer wins: the FIRST era to trip in an epoch names the dump.
        if (ctx.carry.rhs_trip_pending.empty()) ctx.carry.rhs_trip_pending = key;
        return true;
    }
    if (!active && was) eras.erase(key);
    return false;
}

void networkRhsAttribTrip(NetworkRunContext& ctx, const GnssConfig& config_,
                          const std::shared_ptr<spdlog::logger>& log,
                          const NetworkRunTraces& traces, const SyncedEpoch& se,
                          const std::vector<NetStation>& stations,
                          const NetworkEstimator& est,
                          const NetworkEstimator::Datum& plan_datum,
                          const std::vector<StationAssembly>& asm_res,
                          const std::vector<int>& est_index_of) {
    if (!ctx.knobs.rec_on) return;
    const auto& nc = config_.getNetworkConfig();
    auto& ctr = ctx.counters.pc;
    const std::filesystem::path dir = traces.log_dir_ / "rhs_attrib";

    // --- 1. consume a pending trip -------------------------------------------
    if (!ctx.carry.rhs_trip_pending.empty()) {
        const std::string why = ctx.carry.rhs_trip_pending;
        ctx.carry.rhs_trip_pending.clear();
        NetworkAttribFrame now;
        networkAttribCaptureFrame(now, se, stations, est, plan_datum, asm_res,
                                  est_index_of, /*own_assemblies=*/false);
        networkRhsAttribDump(now, log, dir, ("trip:" + why).c_str());
        ++ctr.rec_dumps;
        if (nc.rhs_attrib_on_trip_prev) {
            if (ctx.carry.rhs_prev_frame && ctx.carry.rhs_prev_frame->valid) {
                networkRhsAttribDump(*ctx.carry.rhs_prev_frame, log, dir,
                                     ("trip-prev:" + why).c_str());
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

    // --- 2. retain THIS epoch for the next one -------------------------------
    if (!nc.rhs_attrib_on_trip_prev) return;
    if (static_cast<int>(stations.size()) > std::max(0, nc.rhs_attrib_prev_max_stations)) {
        if (!ctx.carry.rhs_prev_oversize_logged) {
            ctx.carry.rhs_prev_oversize_logged = true;
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

} // namespace GnssProcessor

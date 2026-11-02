/**
 * @file RaimPriorChannel.cpp
 * @brief The post-fit RAIM's prior channel (@c network.raim_prior_channel): the subject of a
 *        carried-prior row, the station walk, the prior table and its isolated-arc test, and
 *        the run-end section.  Declared in network/qc/RaimPriorChannel.hpp; the pass that
 *        calls them and the responses are in network/qc/Raim.cpp.
 */

#include "network/qc/RaimPriorChannel.hpp"

#include "network/assembly/Contributions.hpp"   // contributionInCouplingRegion, contributionKindName, RowTag
#include "network/assembly/RowClassify.hpp"     // rowTagAt
#include "network/driver/RunTypes.hpp"          // StationAssembly
#include "network/qc/RobustStats.hpp"           // medianPolish2Way, medianCopy
#include "network/runend/RunEnd.hpp"            // NetworkRunEndView
#include "network/state/RunContext.hpp"         // ctx.counters, ctx.knobs

#include <spdlog/spdlog.h>
#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace GnssProcessor {

const char* raimPriorSubjectKindName(RaimPriorSubjectKind k) {
    switch (k) {
        case RaimPriorSubjectKind::LineOfSight: return "los";
        case RaimPriorSubjectKind::RxCodeBias:  return "rxcb";
        case RaimPriorSubjectKind::Unwired:     return "unwired";
    }
    return "?";
}

std::string RaimPriorSubject::label(ContributionKind row_kind) const {
    switch (kind) {
        case RaimPriorSubjectKind::LineOfSight: return sat;
        case RaimPriorSubjectKind::RxCodeBias:  return "rxcb:" + cb_key;
        case RaimPriorSubjectKind::Unwired:     return contributionKindName(row_kind);
    }
    return "?";
}

RaimPriorSubject raimPriorSubjectOf(const StationAssembly& R, Eigen::Index row,
                                    Eigen::Index own_col) {
    const RowTag& tag = rowTagAt(R, row);
    RaimPriorSubject s;   // Unwired until a rule below names the subject
    // The state key of the station's receiver code-bias column at the own column, if any.
    const auto codeBiasKeyAt = [&](std::string& key) {
        for (const auto& [cb_key, cb_col, cb_x_lin] : R.pl_cb) {
            (void)cb_x_lin;
            if (cb_col == own_col) { key = cb_key; return true; }
        }
        return false;
    };
    const auto lineOfSight = [&]() {
        if (!tag.sat.valid()) return;
        s.kind = RaimPriorSubjectKind::LineOfSight;
        s.sat  = tag.sat.str();
    };
    switch (tag.kind) {
        case ContributionKind::BlockAmbiguityCarry:
        case ContributionKind::BlockIonoCarry:
        case ContributionKind::BlockPhaseColourCarry:
            lineOfSight();
            break;
        case ContributionKind::BlockRxCbCarry:
            if (codeBiasKeyAt(s.cb_key)) s.kind = RaimPriorSubjectKind::RxCodeBias;
            break;
        case ContributionKind::CouplingScalar:
            // A scalar coupling row names its satellite when its column is an ambiguity or a
            // phase colour; the wet delay's row names none.
            if (tag.sat.valid())
                lineOfSight();
            else if (codeBiasKeyAt(s.cb_key))
                s.kind = RaimPriorSubjectKind::RxCodeBias;
            break;
        default:   // the wet delay and the receiver clock in the block: no response here
            break;
    }
    return s;
}

std::size_t raimPriorChannelCollect(const StationAssembly& R, int station,
                                    const std::string& station_name,
                                    const Eigen::VectorXd& resid, const Eigen::VectorXd& row_r,
                                    bool normalize, double r_floor, double tow,
                                    std::vector<RaimPriorRow>& out, RaimPriorChannelTally& tally) {
    const Eigen::Index n_cpl = static_cast<Eigen::Index>(R.cpl_row_local_cols.size());
    if (R.cpl_row_start < 0 || n_cpl == 0) return 0;
    const Eigen::Index row_end = std::min<Eigen::Index>(R.cpl_row_start + n_cpl, R.A_r.rows());
    std::size_t n = 0;
    for (Eigen::Index row = R.cpl_row_start; row < row_end; ++row) {
        const RowTag& tag = rowTagAt(R, row);
        if (!contributionInCouplingRegion(tag.kind)) continue;
        const Eigen::Index own_col =
            R.cpl_row_local_cols[static_cast<std::size_t>(row - R.cpl_row_start)];
        if (own_col < 0 || own_col >= R.A_r.cols() || row >= resid.size()) continue;
        const double w = (row < R.w.size()) ? R.w(row) : 1.0;
        double stat = std::abs(resid(row)) * std::sqrt(std::max(0.0, w));
        if (normalize && row < row_r.size()) {   // the pools' convention (B2)
            const double ri = row_r(row);
            if (ri <= r_floor) { ++tally.rows_floor; continue; }
            stat /= std::sqrt(ri);
        }
        if (!std::isfinite(stat)) continue;
        RaimPriorRow pr;
        pr.station   = station;
        pr.row       = row;
        pr.own_col   = own_col;
        pr.kind      = tag.kind;
        pr.subject   = raimPriorSubjectOf(R, row, own_col);
        pr.statistic = stat;
        pr.own_coef  = std::abs(R.A_r(row, own_col));
        // The coefficient check: the row's largest coefficient on another carried column of the
        // station (the own columns of the coupling region, each counted once).
        for (Eigen::Index k = 0; k < n_cpl; ++k) {
            const Eigen::Index c = R.cpl_row_local_cols[static_cast<std::size_t>(k)];
            if (c == own_col || c < 0 || c >= R.A_r.cols()) continue;
            const double a = std::abs(R.A_r(row, c));
            if (a > pr.max_other_coef) { pr.max_other_coef = a; pr.max_other_col = c; }
        }
        tally.noteRow(pr.kind, pr.statistic, pr.otherDominant(), station_name,
                      pr.subject.label(pr.kind), tow);
        out.push_back(std::move(pr));
        ++n;
    }
    if (n > 0) ++tally.station_walks;
    return n;
}

bool RaimPriorSelection::acted(int station, const std::string& label) const {
    return acted_cells.count({station, label}) > 0;
}

RaimPriorSelection raimPriorChannelSelect(const std::vector<RaimPriorRow>& rows, double s0_floor,
                                          double k_sigma, const std::set<std::string>& caught_sats,
                                          const std::vector<std::pair<int, std::string>>& obs_arcs,
                                          bool mature) {
    RaimPriorSelection sel;
    if (rows.empty()) return sel;

    // The channel's robust scale: the pools' estimator on the channel's own rows.
    std::vector<double> stats;
    stats.reserve(rows.size());
    for (const RaimPriorRow& r : rows) stats.push_back(r.statistic);
    sel.scale = std::max(s0_floor, 1.4826 * medianCopy(stats));

    // The prior table: one cell per (station, label), the largest statistic of its rows.
    std::map<std::pair<int, std::string>, std::size_t> best;   // cell -> row index
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto key = std::make_pair(rows[i].station, rows[i].subject.label(rows[i].kind));
        const auto it = best.find(key);
        if (it == best.end() || rows[i].statistic > rows[it->second].statistic) best[key] = i;
    }
    std::map<std::pair<int, std::string>, std::size_t> cell_of;   // cell -> index in sel.cells
    sel.cells.reserve(best.size());
    sel.cell_row.reserve(best.size());
    for (const auto& [key, i] : best) {
        cell_of[key] = sel.cells.size();
        sel.cells.emplace_back(key.first, key.second, rows[i].statistic);
        sel.cell_row.push_back(i);
    }
    const MedianPolish mp = medianPolish2Way(sel.cells);
    sel.interaction = mp.interaction;
    sel.overall     = mp.overall;

    // The T4 isolated-arc test on the table, without the persistence gate.
    sel.isolated = raimSelectIsolatedArcs(sel.cells, sel.interaction, sel.scale, k_sigma,
                                          caught_sats, mature, /*persist_epochs=*/1,
                                          [](int, const std::string&) { return 0; });

    // Each flag to the response its subject takes.
    for (const auto& [sta, label] : sel.isolated.refloat) {
        const std::size_t c = cell_of.at({sta, label});
        const RaimPriorRow& r = rows[sel.cell_row[c]];
        switch (r.subject.kind) {
            case RaimPriorSubjectKind::LineOfSight: {
                const auto arc = std::make_pair(sta, r.subject.sat);
                if (std::find(obs_arcs.begin(), obs_arcs.end(), arc) != obs_arcs.end()) {
                    sel.arcs_both.push_back(c);   // refloated once, by the observation channel
                } else {
                    sel.arcs.push_back(arc);
                    sel.arc_cells.push_back(c);
                }
                sel.acted_cells.insert({sta, label});
                break;
            }
            case RaimPriorSubjectKind::RxCodeBias:
                sel.rxcb.push_back(c);
                sel.acted_cells.insert({sta, label});
                break;
            case RaimPriorSubjectKind::Unwired:
                sel.unwired.push_back(c);
                break;
        }
    }
    for (const RaimHeldArc& h : sel.isolated.held)
        sel.held_cells.push_back(cell_of.at({h.station, h.sat}));
    return sel;
}

void raimPriorChannelNoteSelection(const RaimPriorSelection& sel, bool any_rows,
                                   RaimPriorChannelTally& tally) {
    ++tally.passes;
    if (!any_rows) return;
    ++tally.scale_n;
    tally.scale_sum += sel.scale;
    tally.scale_max  = std::max(tally.scale_max, sel.scale);
    tally.cells     += static_cast<long long>(sel.cells.size());
    tally.flags     += static_cast<long long>(sel.isolated.refloat.size());
    tally.held      += static_cast<long long>(sel.isolated.held.size());
    tally.arcs_both += static_cast<long long>(sel.arcs_both.size());
    tally.unwired   += static_cast<long long>(sel.unwired.size());
}

std::string raimPriorEventDetail(const RaimPriorRow& r, double scale) {
    return fmt::format("sigma0_p={:.2f};row_kind={};own_col={};row_stat={:.3f};own_coef={:.4f};"
                       "max_other_coef={:.4f};max_other_col={};subject={}",
                       scale, contributionKindName(r.kind), r.own_col, r.statistic, r.own_coef,
                       r.max_other_coef, r.max_other_col, r.subject.label(r.kind));
}

// --- The tally (declared in network/qc/Raim.hpp) ---

void RaimPriorChannelTally::noteRow(ContributionKind kind, double statistic, bool other_dominant,
                                    const std::string& station, const std::string& label,
                                    double tow) {
    const std::size_t u = std::min<std::size_t>(static_cast<std::size_t>(kind), kContributionKinds);
    ++rows[u];
    if (other_dominant) ++rows_other_dominant;
    if (statistic > max_statistic) {
        max_statistic = statistic;
        max_tow       = tow;
        max_station   = station;
        max_label     = label;
        max_kind      = kind;
    }
}

long long RaimPriorChannelTally::rowsTested() const {
    long long n = 0;
    for (const long long v : rows) n += v;
    return n;
}

// --- The run-end section ---

std::vector<RaimPriorReportLine> raimPriorChannelReport(const RaimPriorChannelTally& t,
                                                        double k_sigma) {
    std::vector<RaimPriorReportLine> out;
    if (t.passes == 0) {
        out.push_back({true, "ARMED but NEVER RAN: the post-fit RAIM pass never reached the "
                             "attribution, so no carried-prior row was tested and the key "
                             "changed nothing but the pools."});
        return out;
    }
    const long long n_rows = t.rowsTested();
    std::string kinds;
    for (std::size_t u = 0; u <= kContributionKinds; ++u)
        if (t.rows[u] > 0)
            kinds += fmt::format("{}{} {}", kinds.empty() ? "" : ", ",
                                 contributionKindName(static_cast<ContributionKind>(u)), t.rows[u]);
    out.push_back({false, fmt::format(
        "{} pass(es), {} station-epoch(s) with carried-prior rows; rows tested {} ({}); {} left "
        "out at the redundancy floor.  The observation pools held observation rows only.",
        t.passes, t.station_walks, n_rows, kinds.empty() ? std::string("none") : kinds,
        t.rows_floor)});
    if (n_rows == 0) {
        out.push_back({true, "INERT: no carried-prior row on this run (no coupling region in any "
                             "station block), so the channel tested nothing; the key's only "
                             "effect was to keep the prior rows out of the pools, where there "
                             "were none."});
        return out;
    }
    const double scale_mean = t.scale_n > 0 ? t.scale_sum / static_cast<double>(t.scale_n) : 0.0;
    out.push_back({false, fmt::format(
        "{} cell(s) over the run; the channel's scale mean {:.3f}, max {:.3f}; the bar {:g} times "
        "the scale on the median-polish interaction.  The largest row statistic {:.2f} at TOW "
        "{:.1f}, {} {} ({}).",
        t.cells, scale_mean, t.scale_max, k_sigma, t.max_statistic, t.max_tow, t.max_station,
        t.max_label, contributionKindName(t.max_kind))});
    out.push_back({false, fmt::format(
        "flags {} (held by the maturity gate {}): {} line-of-sight refloat(s) reaching {} carried "
        "ambiguity(ies) ({} found no ambiguity in the pool), {} on an arc the observation channel "
        "refloated in the same pass; {} receiver code-bias reset(s) ({} with no carried entry); "
        "{} unwired.",
        t.flags, t.held, t.arcs, t.arcs_ambiguities, t.arcs_empty, t.arcs_both, t.rxcb_resets,
        t.rxcb_no_entry, t.unwired)});
    if (t.unwired > 0)
        out.push_back({true, fmt::format(
            "{} flag(s) on a carry the channel has no response for (the wet delay or the receiver "
            "clock in the block, or a row whose subject did not resolve): counted and written as "
            "T4_prior prior_unwired rows, NOTHING DONE.", t.unwired)});
    if (t.flags == 0 && t.held == 0)
        out.push_back({false, "NO FLAG: the channel tested every carried-prior row and no cell "
                              "stood out; that is a quiet run, not evidence of the channel's power."});
    out.push_back({false, fmt::format(
        "coefficient check: {} of {} tested row(s) ({:.1f} %) have their largest coefficient on "
        "another carried column than their own (the Cholesky-order spread of a whitened block).",
        t.rows_other_dominant, n_rows,
        100.0 * static_cast<double>(t.rows_other_dominant) / static_cast<double>(n_rows))});
    return out;
}

int networkRaimPriorChannelRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    if (!ctx.knobs.raim_prior_channel) return 0;
    for (const RaimPriorReportLine& l :
         raimPriorChannelReport(ctx.counters.prior_channel, ctx.knobs.raim_k))
        log->log(l.warn ? spdlog::level::warn : spdlog::level::info,
                 "Network (RAIM prior channel): {}", l.text);
    return 0;
}

} // namespace GnssProcessor

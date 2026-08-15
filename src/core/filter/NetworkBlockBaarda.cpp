/**
 * @file NetworkBlockBaarda.cpp
 * @brief BlockFormal — the (station, satellite) LOS block sweep and its emission
 *        (docs/SPEC_redundancy_census.md BB-INC-1..3).  See NetworkBlockBaarda.hpp for
 *        the design argument and the two corrections to SPEC §3's recipe.
 *
 * Measure-only.  Nothing here writes a weight, a state or a decision; the whole
 * translation unit reads the SOLVED system and emits counters, ladders and
 * events.  SPEC §9 row 1 pre-declares that ANY response to a loud block — a
 * deweight, a refloat, an exclusion — belongs to a different card, and §11 says
 * this one does not act on a block "including the loud ones".
 *
 * ## Where the cost goes
 *
 * The sweep rides the redundancy census's SWEPT branch, so it inherits that
 * card's stride / targeted-escalation / trip schedule and adds no scheduler
 * (SPEC §4).  Per swept station it pays one @c reconstructStation (a cached
 * LDLᵀ solve, no factorization), two GEMVs for the post-fit residual, and then
 * per block one @f$b\times b@f$ cofactor plus one @f$b\times b@f$ LDLᵀ — with
 * @f$b\le 8@f$ that is bounded by the @c rowRedundancy call the census already
 * made for the same station (measured there at 6.1 ms/station-sweep).  SPEC §5's
 * budget is "≤ 2× the census sweep it rides"; @c tallies.wall_ms is the number
 * that has to be checked against it, which is why the sweep is timed here rather
 * than trusted.
 */

#include "filter/NetworkBlockBaarda.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "app/NetworkRunContext.hpp"
#include "filter/NetworkNormalEquations.hpp"
#include "logging/NetworkRunTraces.hpp"

namespace GnssProcessor {

// ===========================================================================
//  The UNCHECKABLE test (SPEC §3's degenerate guard)
// ===========================================================================

bool blockCofactorIsCheckable(const Eigen::MatrixXd& Q, double rcond_eps,
                              bool* singular, bool* nonfinite) {
    if (singular)  *singular  = false;
    if (nonfinite) *nonfinite = false;
    const Eigen::Index b = Q.rows();
    if (b <= 0 || Q.cols() != b) { if (singular) *singular = true; return false; }
    // A non-positive row weight puts a non-finite entry on the diagonal ON
    // PURPOSE (the kernel's header note) precisely so it fails HERE rather than
    // becoming a finite number nobody questions.
    if (!Q.allFinite()) { if (nonfinite) *nonfinite = true; return false; }

    // Q is PSD by construction, so an LDLᵀ is the right factorization and its
    // pivots are the conditioning statistic.  The test is RELATIVE: a phase-only
    // block's cofactor entries run ~1e-6 m² and an absolute floor would call
    // every one of them singular.
    const Eigen::LDLT<Eigen::MatrixXd> ldlt(Q);
    if (ldlt.info() != Eigen::Success) { if (singular) *singular = true; return false; }
    const Eigen::VectorXd d = ldlt.vectorD();
    if (d.size() != b) { if (singular) *singular = true; return false; }
    double dmax = 0.0, dmin = std::numeric_limits<double>::infinity();
    for (Eigen::Index i = 0; i < b; ++i) {
        const double v = d(i);
        if (!std::isfinite(v)) { if (nonfinite) *nonfinite = true; return false; }
        // A NEGATIVE pivot is a genuine finding, not a small one: the cofactor of
        // a PSD quantity cannot have one, so the block's leverage path has lost
        // definiteness and its quadratic form would not even be a norm.
        if (v <= 0.0) { if (singular) *singular = true; return false; }
        dmax = std::max(dmax, v);
        dmin = std::min(dmin, v);
    }
    if (!(dmin > rcond_eps * dmax)) { if (singular) *singular = true; return false; }
    return true;
}

// ===========================================================================
//  INC-1 — the per-station block sweep
// ===========================================================================

void networkBlockBaardaSweepStation(NetworkBlockBaarda& acc,
                                    const NetworkNormalEquations& kernel,
                                    std::size_t slot, int station_index,
                                    const StationAssembly& R,
                                    const std::vector<NetRowClass>& cls,
                                    const std::vector<std::string>& sat,
                                    const Eigen::VectorXd& resid,
                                    const Eigen::MatrixXd& cov_free,
                                    const std::set<std::string>& responded_sats,
                                    bool datum_changed,
                                    NetworkBlockBaardaTallies& tallies,
                                    std::vector<BlockBaardaHit>& hits) {
    if (!acc.enabled()) return;
    const Eigen::Index m = R.y.size();
    if (m <= 0 || resid.size() != m
        || static_cast<Eigen::Index>(cls.size()) < m
        || static_cast<Eigen::Index>(sat.size()) < m) return;

    const auto t0 = std::chrono::steady_clock::now();
    ++tallies.station_sweeps;

    // Group the station's DATA rows by satellite.  Pins and coupling rows are
    // excluded: on a within-epoch re-solve the assembly's coupling rows are
    // re-whitened in a COPY before addStation, so they no longer describe the
    // system that was solved, and a pin's ~1e10 weight would swamp any block it
    // joined.  std::map keeps the block order deterministic across runs, which a
    // hash map would not — the run-end worst-block identity has to be
    // reproducible for the A/B to mean anything.
    std::map<std::string, std::vector<Eigen::Index>> by_sat;
    for (Eigen::Index row = 0; row < m; ++row) {
        const NetRowClass k = cls[static_cast<std::size_t>(row)];
        if (!isDataRowClass(k)) continue;
        const std::string& s = sat[static_cast<std::size_t>(row)];
        if (s.empty()) { ++tallies.unnamed_rows; continue; }
        by_sat[s].push_back(row);
    }

    for (const auto& kv : by_sat) {
        const std::string& s = kv.first;
        const std::vector<Eigen::Index>& rows = kv.second;
        const Eigen::Index b = static_cast<Eigen::Index>(rows.size());
        if (b <= 0) continue;
        if (acc.familyIndex(s) < 0) continue;    // constellation not requested

        NetworkBlockBaarda::Input in;
        in.station = station_index;
        in.sat     = s;
        in.b       = static_cast<int>(b);
        for (Eigen::Index i = 0; i < b; ++i) {
            const NetRowClass k = cls[static_cast<std::size_t>(rows[static_cast<std::size_t>(i)])];
            if (k == NetRowClass::ObsCode)  ++in.n_code;
            if (k == NetRowClass::ObsPhase) ++in.n_phase;
        }
        // SPEC §4's union rule, and the prior-residual census's ordering: any
        // RESPONDED row makes the block RESPONDED; only otherwise does FRAME
        // apply.  RESPONDED is an ESTIMATOR-side signal and the INC-2
        // corroboration is a RAW-observable one, so §8.2's positive control
        // (RESPONDED ladder vs CLEAN ladder) is not circular.
        in.label = (responded_sats.find(s) != responded_sats.end())
                       ? PriorResidualLabel::Responded
                       : (datum_changed ? PriorResidualLabel::Frame
                                        : PriorResidualLabel::Clean);

        // The block's rows, as the kernel wants them.
        Eigen::MatrixXd Bs(b, R.A_s.cols()), Br(b, R.A_r.cols());
        Eigen::VectorXd bw(b), be(b);
        for (Eigen::Index i = 0; i < b; ++i) {
            const Eigen::Index row = rows[static_cast<std::size_t>(i)];
            Bs.row(i) = R.A_s.row(row);
            Br.row(i) = R.A_r.row(row);
            bw(i) = (row < R.w.size()) ? R.w(row) : 0.0;
            be(i) = resid(row);
        }
        if (!be.allFinite()) { ++tallies.unnamed_rows; continue; }

        Eigen::MatrixXd Q;
        try { Q = kernel.blockResidualCofactor(slot, Bs, Br, bw, cov_free); }
        catch (...) { continue; }
        if (Q.rows() != b) continue;

        bool sing = false, nonfin = false;
        if (!blockCofactorIsCheckable(Q, kBlockCofactorRcondEps, &sing, &nonfin)) {
            // UNCHECKABLE: counted with its identity, NEVER inverted through.
            in.uncheckable = true;
            acc.addBlock(in);
            ++tallies.blocks; tallies.rows += b; ++tallies.uncheckable;
            if (sing)   ++tallies.singular;
            if (nonfin) ++tallies.nonfinite;
            continue;
        }

        // T_B = e' Q^-1 e / b, and the ROW tier's statistic on the same rows in
        // the same units (the b = 1 case) so INC-3's comparison is apples to
        // apples rather than a threshold on two different scales.
        const Eigen::VectorXd z = Q.ldlt().solve(be);
        const double quad = be.dot(z);
        if (!(quad >= 0.0) || !std::isfinite(quad)) {
            in.uncheckable = true;
            acc.addBlock(in);
            ++tallies.blocks; tallies.rows += b; ++tallies.uncheckable; ++tallies.singular;
            continue;
        }
        in.T = quad / static_cast<double>(b);
        double row_max = 0.0;
        for (Eigen::Index i = 0; i < b; ++i) {
            const double qii = Q(i, i);
            if (!(qii > 0.0) || !std::isfinite(qii)) continue;
            row_max = std::max(row_max, be(i) * be(i) / qii);
        }
        in.max_row_T = row_max;

        const NetworkBlockBaarda::BlockStat st = acc.addBlock(in);
        if (!st.accepted) continue;
        ++tallies.blocks; tallies.rows += b;
        if (st.reported) {
            ++tallies.reported;
            if (st.block_only) ++tallies.block_only;
            BlockBaardaHit h;
            h.station = station_index; h.sat = s; h.b = static_cast<int>(b);
            h.t = st.t; h.row_t = st.row_t; h.T = in.T; h.sigma0 = st.sigma0;
            h.n_code = in.n_code; h.n_phase = in.n_phase;
            h.block_only = st.block_only; h.raw_formal = st.raw_formal;
            h.label = in.label; h.corrob = st.corrob;
            hits.push_back(std::move(h));
        }
        if (st.t > tallies.max_t) {
            tallies.max_t  = st.t;
            tallies.max_id = fmt::format("#{}/{} b={} @tow {:.1f}",
                                         station_index, s, b, acc.lastTow());
        }
    }

    tallies.wall_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// ===========================================================================
//  Emission
// ===========================================================================

namespace {

/// Station name for an identity string, or a positional fallback.  A finding
/// that cannot be named must still be reported (the census's discipline).
std::string stationName(const std::vector<NetStation>& stations, int idx) {
    if (idx < 0 || idx >= static_cast<int>(stations.size()))
        return (idx < 0) ? std::string("(network)") : fmt::format("#{}", idx);
    return stations[static_cast<std::size_t>(idx)].name;
}

}  // namespace

void networkBlockBaardaEmit(NetworkRunContext& ctx, const GnssConfig& config_,
                            const std::shared_ptr<spdlog::logger>& log,
                            NetworkRunTraces& traces, const SyncedEpoch& se,
                            const std::vector<NetStation>& stations) {
    (void)config_;
    (void)log;
    NetworkBlockBaarda& acc = ctx.carry.bb;
    NetworkBlockBaardaTallies& t = ctx.counters.bb;
    if (!acc.enabled()) return;

    const auto t0 = std::chrono::steady_clock::now();
    ++t.passes;

    if (acc.epochBlocks() > 0) ++t.epochs;

    // ---- the per (family x label) ladder rows -----------------------------
    if (traces.block_baarda_trace.is_open()) {
        const auto& fams = acc.families();
        const auto& lad  = NetworkBlockBaarda::ladder();
        for (std::size_t f = 0; f < fams.size(); ++f) {
            for (int li = 0; li < kPriorResidualLabels; ++li) {
                const auto lab = static_cast<PriorResidualLabel>(li);
                const NetworkBlockBaarda::Ladder& L =
                    acc.epochLadder(static_cast<int>(f), lab);
                // All-zero rows would triple the file; "this label never
                // happened" is reported at run end instead, where it is
                // interpretable.
                if (L.n_blocks == 0) continue;
                auto& o = traces.block_baarda_trace;
                o << se.time.week << ',' << fmt::format("{:.3f}", se.time.tow) << ','
                  << fams[f] << ',' << priorResidualLabelName(lab) << ','
                  << L.n_blocks << ',' << L.n_rows << ',' << L.n_uncheckable << ','
                  << L.n_raw_formal << ',' << L.n_reported << ',' << L.n_block_only << ','
                  << fmt::format("{:.6g}", acc.sigma0(static_cast<int>(f))) << ','
                  << (acc.sigma0Warm(static_cast<int>(f)) ? 1 : 0) << ','
                  << fmt::format("{:.6g}", acc.sigma0Neff(static_cast<int>(f))) << ','
                  << fmt::format("{:.6g}", L.geoT());
                for (int k = 0; k < NetworkBlockBaarda::kLadderBins; ++k)
                    o << ',' << L.bins[static_cast<std::size_t>(k)];
                o << ',' << fmt::format("{:.6g}", L.max_t)
                  << ',' << fmt::format("{:.6g}", L.max_T)
                  << ',' << fmt::format("{:.6g}", L.max_row_t)
                  << ',' << L.max_b
                  << ',' << stationName(stations, L.max_station)
                  << ',' << L.max_sat
                  << ',' << (L.max_block_only ? 1 : 0);
                for (int r = 0; r < 2; ++r)
                    for (int c = 0; c < kPriorResidualCorrobs; ++c)
                        o << ',' << L.contingency[static_cast<std::size_t>(r)]
                                               [static_cast<std::size_t>(c)];
                o << '\n';
                (void)lad;
            }
        }
    }

    // ---- per-block identities, rate-limited -------------------------------
    const long long now_ep = acc.epochs();
    const long long holdoff = ctx.knobs.bb_event_holdoff;
    for (const BlockBaardaHit& h : ctx.carry.bb_hits) {
        const std::string sname = stationName(stations, h.station);
        const std::string key = sname + "\x1f" + h.sat;
        auto it = ctx.carry.bb_event_last.find(key);
        if (it != ctx.carry.bb_event_last.end() && now_ep - it->second < holdoff)
            continue;
        ctx.carry.bb_event_last[key] = now_ep;
        // The statistic goes in `detail` as well as in the typed column: emitEvent
        // formats statistic/threshold with a FIXED 6 decimals, which prints a
        // well-conditioned small t_B as 0.000000.
        traces.emitEvent(se.time, "BLOCKBAARDA",
                         h.block_only ? "block_only_outlier" : "block_outlier",
                         h.sat, sname, h.t, acc.reportSigma(), "observe",
                         fmt::format("t_B={:.6g};T_B={:.6g};row_t={:.6g};b={};"
                                     "n_code={};n_phase={};sigma0={:.6g};label={};"
                                     "corrob={};raw_formal={}",
                                     h.t, h.T, h.row_t, h.b, h.n_code, h.n_phase,
                                     h.sigma0, priorResidualLabelName(h.label),
                                     priorResidualCorrobName(h.corrob),
                                     h.raw_formal ? 1 : 0));
        ++t.events;
    }
    ctx.carry.bb_hits.clear();

    t.wall_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// ===========================================================================
//  Run-end readout (standing invariant #4)
// ===========================================================================

void networkBlockBaardaReport(const NetworkRunContext& ctx,
                              const std::shared_ptr<spdlog::logger>& log,
                              const std::vector<NetStation>& stations) {
    const NetworkBlockBaarda& acc = ctx.carry.bb;
    const NetworkBlockBaardaTallies& t = ctx.counters.bb;
    if (!acc.enabled()) return;

    // DENOMINATORS FIRST.  "The sweep ran and found nothing" and "the sweep never
    // ran" are the two readings this line exists to separate.
    log->info("Network (block-Baarda census): {} emission pass(es), {} with blocks, "
              "{} station-sweep(s), {} block(s) over {} data row(s); {} uncheckable "
              "({} singular, {} non-finite), {} unnamed row(s), {} station(s) with no "
              "post-fit residual; {} reported, {} block-only; {} event(s); {:.1f} ms.",
              t.passes, t.epochs, t.station_sweeps, t.blocks, t.rows, t.uncheckable,
              t.singular, t.nonfinite, t.unnamed_rows, t.no_resid, t.reported,
              t.block_only, t.events, t.wall_ms);

    if (t.station_sweeps == 0) {
        log->info("Network (block-Baarda census): ARMED but NO station sweep ever ran "
                  "— the redundancy census's stride/escalation never selected a station "
                  "(or every one was skipped).  This is a WIRING reading, not a finding.");
        return;
    }
    if (t.blocks == 0) {
        log->info("Network (block-Baarda census): {} station-sweep(s) produced ZERO "
                  "blocks.  Either no data row could be named with a satellite ({} "
                  "unnamed) or no requested constellation was present.  Not a finding.",
                  t.station_sweeps, t.unnamed_rows);
        return;
    }

    // The SIZE HISTOGRAM, before any ladder.  A run whose blocks were all b = 1
    // would produce a ladder identical to the scalar w-test and an INC-3 fraction
    // of 0 BY CONSTRUCTION — the card would read "no smearing" when what actually
    // happened is "there was never a block to smear across".
    {
        const auto& hb = acc.sizeHistogram();
        std::string hist;
        for (int k = 0; k <= kBlockBaardaMaxB; ++k) {
            if (hb[static_cast<std::size_t>(k)] == 0) continue;
            hist += fmt::format("{}b={}{}:{}", hist.empty() ? "" : "  ",
                                (k == kBlockBaardaMaxB) ? ">" : "",
                                (k == kBlockBaardaMaxB) ? kBlockBaardaMaxB : k + 1,
                                hb[static_cast<std::size_t>(k)]);
        }
        log->info("Network (block-Baarda census): block sizes  {}  |  composition: "
                  "{} code+phase, {} code-only, {} phase-only.  A b=1 population "
                  "cannot smear, so the INC-3 fraction below is only interpretable "
                  "against this line.",
                  hist.empty() ? "(none)" : hist, acc.runMixedBlocks(),
                  acc.runCodeOnlyBlocks(), acc.runPhaseOnlyBlocks());
    }

    const auto& fams = acc.families();
    const auto& lad  = NetworkBlockBaarda::ladder();
    long long tot_reported = 0, tot_block_only = 0;
    for (std::size_t f = 0; f < fams.size(); ++f) {
        const int fi = static_cast<int>(f);
        if (acc.runBlocks(fi) == 0) {
            log->info("Network (block-Baarda census) '{}': ZERO blocks over the run — "
                      "the constellation was requested but never swept.  Distinct from "
                      "'swept and quiet'.", fams[f]);
            continue;
        }
        log->info("Network (block-Baarda census) '{}': sigma0 = {:.4g} ({}, n_eff = "
                  "{:.1f}).  sigma0 is a SCALE, not a test — it is deliberately "
                  "unclamped, so a value below 1 means this family's blocks are "
                  "QUIETER than the model says and every t_B below is inflated "
                  "accordingly.",
                  fams[f], acc.sigma0(fi), acc.sigma0Warm(fi) ? "warm" : "WARM-UP",
                  acc.sigma0Neff(fi));
        for (int li = 0; li < kPriorResidualLabels; ++li) {
            const auto lab = static_cast<PriorResidualLabel>(li);
            const NetworkBlockBaarda::Ladder& L = acc.runLadder(fi, lab);
            if (L.n_blocks == 0) continue;
            std::string ladder_txt;
            for (int k = 0; k < NetworkBlockBaarda::kLadderBins; ++k)
                ladder_txt += fmt::format("{}>{:g}:{} ({:.3f}%)",
                                          k ? "  " : "", lad[static_cast<std::size_t>(k)],
                                          L.bins[static_cast<std::size_t>(k)],
                                          100.0 * L.frac(k));
            log->info("Network (block-Baarda census) '{}'/{}: n = {} block(s) / {} row(s) "
                      "({} uncheckable, {} raw-formal), geo(t_B) = {:.4g}.  Ladder over "
                      "t_B (a VARIANCE RATIO, E = 1 under H0 — NOT the sigma units of the "
                      "prior-residual ladder): {}",
                      fams[f], priorResidualLabelName(lab), L.n_blocks, L.n_rows,
                      L.n_uncheckable, L.n_raw_formal, L.geoT(), ladder_txt);
            if (L.max_station >= 0 || !L.max_sat.empty())
                log->info("Network (block-Baarda census) '{}'/{}: worst block t_B = "
                          "{:.4g} (T_B = {:.4g}, loudest row t = {:.4g}, b = {}) at "
                          "{}/{} @tow {:.1f}{}",
                          fams[f], priorResidualLabelName(lab), L.max_t, L.max_T,
                          L.max_row_t, L.max_b, stationName(stations, L.max_station),
                          L.max_sat, L.max_tow,
                          L.max_block_only ? "  [BLOCK-ONLY: every row of it was "
                                             "individually quiet]" : "");
            tot_reported   += L.n_reported;
            tot_block_only += L.n_block_only;

            if (acc.corroborating()) {
                // INC-2's b-cell: loud block, raw observable measurably FLAT.
                const auto& c = L.contingency;
                const long long hi_n = c[0][0] + c[0][1] + c[0][2];
                log->info("Network (block-Baarda census) '{}'/{}: contingency  "
                          "loud x [no_evidence {}, raw_flat {}, raw_stepped {}] | "
                          "quiet x [{}, {}, {}].  b-cell (loud AND raw-flat) = {} of {} "
                          "loud = {:.1f}% — the fraction of loud blocks the raw "
                          "observables do NOT corroborate.",
                          fams[f], priorResidualLabelName(lab),
                          c[0][0], c[0][1], c[0][2], c[1][0], c[1][1], c[1][2],
                          c[0][1], hi_n,
                          hi_n > 0 ? 100.0 * static_cast<double>(c[0][1])
                                              / static_cast<double>(hi_n) : 0.0);
            }
        }
    }

    // ---- INC-3: THE NUMBER THE CARD EXISTS TO PRODUCE ---------------------
    if (tot_reported == 0) {
        log->info("Network (block-Baarda census): INC-3 — ZERO blocks cleared t_B > "
                  "{:.3g} over {} block(s), so the smearing fraction is UNDEFINED, not "
                  "zero.  Either the field is quiet at this threshold or sigma0 has "
                  "absorbed the tail; read the ladders above before concluding either.",
                  acc.reportSigma(), t.blocks);
    } else {
        log->info("Network (block-Baarda census): INC-3 — block_only / blocks_flagged = "
                  "{} / {} = {:.1f}%.  This is the population invisible to EVERY existing "
                  "tier: loud as a block, and every one of its rows individually below "
                  "the same threshold.  SPEC 8.4 — if this is ~0 the block tier adds "
                  "nothing here and that closes the card honestly.",
                  tot_block_only, tot_reported,
                  100.0 * static_cast<double>(tot_block_only)
                        / static_cast<double>(tot_reported));
    }
    if (!t.max_id.empty())
        log->info("Network (block-Baarda census): worst block of the run t_B = {:.4g} "
                  "at {}.", t.max_t, t.max_id);
}

}  // namespace GnssProcessor
